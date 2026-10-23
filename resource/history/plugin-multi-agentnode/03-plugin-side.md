# 03 插件侧契约与迁移指南

写给插件作者。主程序侧实现见 `02`。

## 1. 心智模型

```
插件动态库（进程内一份）
└── PluginInstance（一个 plugin_ctx；create / start 各一次）
     ├── 进程级状态：连接、线程、全局缓存、配置        ← 所有 agent 共享
     └── 挂载 N 份（每个挂载一次 mount / unmount）
          ├── per-agent 状态（mount 返回值 mount_ud，里面存着 AgentRef）
          ├── 该 agent 里的工具 / 钩子 / 提示词 / 功能点 / 图 / 资源 / 事件
          └── 该 agent 的配置覆盖（config_json）
```

三条硬规则：

1. **`start` 只做进程级的事**：读配置、建连接、起线程、注册能力、订阅宿主事件。
2. **所有 agent 侧注册都在 `mount` 里做**：不在挂载里调用注册接口会返回
   `AGENTXX_PLUGIN_ERR_NO_AGENT`，不会有"延后生效"这种事。
3. **需要区分 agent 的接口都要带 `AgentRef`**：在 `mount` 里拿到、存进 per-agent 状态，
   之后任何线程都用同一份。不带 = 报错，宿主不会替你猜。

## 2. 六个入口

| 入口 | 次数 | 线程 | 职责 | 禁止 |
|---|---|---|---|---|
| `create` | 1 | 宿主 io 线程 | 构造 `plugin_ctx`、查接口表 | 任何注册 |
| `start` | 1（重新启用会再来） | 宿主 io 线程 | 进程级资源；能力（`pluginxx.capabilities`）；宿主事件（`pluginxx.events`） | agent 侧注册（工具/钩子/提示词/功能点/图/资源/权限） |
| `mount` | 每 agent 一次 | 宿主 io 线程 | 收下 `(agent_id, mount_seq)`；建 per-agent 状态；注册该 agent 的全部接入点；读本挂载配置 | 阻塞、长耗时 |
| `unmount` | 与 mount 配对 | 宿主 io 线程 | 释放 per-agent 状态（注册由宿主自动撤销） | 异步长耗时 |
| `stop` | 1（卸载/宿主关闭） | 宿主 io 线程 | 释放进程级资源（线程、连接、定时器、订阅） | — |
| `destroy` | 1 | 宿主 io 线程 | 释放 `plugin_ctx` | — |

### 2.1 签名

`mount` / `unmount` 与 `start` / `stop` 同一套操作协议（带 `notify`，可同步完成也可异步）：

```c
typedef void*(PLUGINXX_CALL* AgentxxPluginAgentMountFn)(
    void*                              plugin_ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    out_error);

typedef void*(PLUGINXX_CALL* AgentxxPluginAgentUnmountFn)(
    void*                         plugin_ctx,
    void*                         mount_ud,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               out_error);
```

- 同步完成：立刻 `notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr)`；
- 异步：记下 `notify`，完成时再 `done`；
- 失败：`done(..., PLUGINXX_OPERATOR_FAILED, "原因")` 或返回 `NULL` +
  `PluginString::set(host, out_error, "...")`；失败只回滚**这一个 agent**；
- `mount` 的返回值就是 `mount_ud`（可 `nullptr`），`unmount` 收到的就是它；
- `unmount` 应快速：只释放内存与自己持有的登记。慢收尾放 `stop`。

### 2.2 导出宏（一套，8 个参数）

```cpp
AGENTXX_PLUGIN_AGENT_EXPORT(
    Ctx, "name", "1.0.0", "desc",
    startFn, stopFn,        // 进程级
    mountFn, unmountFn      // agent 级；没有 per-agent 状态就传 nullptr
);

// 手写 create/destroy 的插件：
AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT(Ctx, startFn, stopFn, mountFn, unmountFn);
```

`mountFn` 传 `nullptr` 表示"本插件不提供 agent 侧能力"（例如纯后台守护插件）。
宏内部把 `PLUGINXX_API_VERSION` 填进 `get_info`，所以插件用哪个版本的 SDK 头编译，
主程序就能看到对应的版本号（见 §7 第 8 条）。

## 3. 挂载入参 `AgentxxPluginAgentMountSpec`

```c
typedef struct AgentxxPluginAgentMountSpec {
    uint32_t           struct_size;   // sizeof(本结构体)
    uint32_t           flags;         // 预留 (0)
    PluginxxStringView agent_id;      // "root" / "sub:3" / "ext:qq:1"
    PluginxxStringView agent_name;    // AgentConfig::agentName
    PluginxxStringView work_dir;      // 绝对路径，UTF-8
    PluginxxStringView language;      // "zh-cn" / "en"
    PluginxxStringView model_json;    // 当前模型配置 JSON
    PluginxxStringView config_json;   // 加载表 args ⊕ plugin.mount[].args
    uint64_t           mount_seq;     // 宿主内单调序号（防 id 复用）
} AgentxxPluginAgentMountSpec;
```

- 字符串只在调用期间有效，需要就自己拷进 per-agent 状态（SDK 的 `AgentRef` 会拷 `agent_id`）；
- `config_json` 是本挂载的配置；同一插件在不同 agent 上可以拿到不同值；
- `mount_seq` 每次挂载都不同：同一 agentId 卸载后重新挂载，旧引用自动失效。

### 3.1 引用：`AgentRef`

```cpp
namespace agentxx::plugin {

/// per-agent 引用的值类型：持有 agentId 字符串 + 挂载序号，可拷贝、可比较
class AgentRef {
public:
    AgentRef() = default;
    explicit AgentRef(const AgentxxPluginAgentMountSpec& spec);  // 从挂载入参构造
    std::string_view id() const noexcept;   // agentId
    uint64_t         seq() const noexcept;  // mount_seq
    PluginxxScopeRef raw() const noexcept;  // 传给接口表（视图指向本对象内部）
};

}  // namespace agentxx::plugin
```

`raw()` 返回的结构体里 `id` 是视图，指向 `AgentRef` 内部的字符串，所以**只在 `AgentRef`
存活期间可用**；接口调用都是同步的，正常用法不会踩到。

### 3.2 查询挂载（`agentxx.agent.mount`）

```cpp
std::string all = ctx.listMounts();       // 全部挂载的摘要 JSON 数组
std::string one = ctx.mountInfoOf(id);    // 按 agentId 查；没有返回空串
```

没有"当前挂载"这类查询 —— 你自己手里就有 `AgentRef`。这两项用于诊断，以及
"我在这几个 agent 上是否都挂好了"这类自查。

## 4. 状态与缓存

### 4.1 三种状态

| 状态 | 放哪 | 生命周期 | 例子 |
|---|---|---|---|
| 进程级 | `plugin_ctx` 成员 | create → destroy | QQ 长连接、登录态、HTTP 客户端、线程池 |
| 挂载级 | `mount` 返回的 `mount_ud`（或 `MountStore`），**内含 `AgentRef`** | mount → unmount | 该 agent 的工具集、群列表、会话映射 |
| 会话级 | 主程序的会话 / `share_store`；或挂载级状态里按 `sessionId` 分键 | 会话存在期间 | 上次说话时间、未读计数 |

**不要**把挂载级状态放进进程级裸字段（会被后挂载的 agent 覆盖）。

### 4.2 `MountStore`

```cpp
/// per-agent 状态表（放 plugin_ctx 里；只在宿主 io 线程访问，无需加锁）
template<typename T>
class MountStore {
public:
    /// mount 里调用；T 需要有一个可赋值的 `AgentRef ref` 成员
    T* create(const AgentRef& ref) {
        auto [pos, ok] = items_.try_emplace(std::string{ref.id()});
        if (ok) pos->second->ref = ref;
        return pos->second.get();
    }
    T* find(const AgentRef& ref) const {              // id 与 seq 都要匹配
        auto it = items_.find(std::string{ref.id()});
        if (it == items_.end() || it->second->ref.seq() != ref.seq()) return nullptr;
        return it->second.get();
    }
    void erase(const AgentRef& ref) { items_.erase(std::string{ref.id()}); }
    size_t size() const noexcept { return items_.size(); }

    template<typename Fn> void forEach(Fn&& fn) const {
        for (const auto& [id, item] : items_) fn(*item);
    }
private:
    std::map<std::string, std::unique_ptr<T>, std::less<>> items_;
};
```

回复"这个回调属于哪个 agent"只有一条路：**你自己带进来的**。两种写法：

```cpp
// A. 注册时把 per-agent 状态直接闭包捕获（最直观，推荐）
tool(ctx, st->ref, "group_stats", "...", "{}",
     [st](std::string_view) { return st->stats(); });   // st 来自 mount

// B. 回调只给 user_data，进去取状态（跨模块/跨语言绑定时用）
void on_event(void* ud, std::string_view json) {
    auto* st = static_cast<State*>(ud);   // 里面就有 st->ref
    ...
}
```

### 4.3 缓存纪律

1. 进程级缓存必须与 agent 无关（"某个群的资料"可以，"当前会话"不行）；
2. 挂载级缓存必须带 agentId 维度，条目里存 `AgentRef`，调主程序接口时把它传回去；
3. `unmount` 里把该 agent 的条目删干净，避免同 id 重新挂载拿到脏数据；
4. 后台线程用 `(agentId, sessionId)` 或 `(AgentRef, 群号)` 做键，别只用 sessionId。

## 5. 在哪个入口做什么（决策表）

| 能力 | 入口 | 说明 |
|---|---|---|
| 工具 `register_tool` | `mount` | 在 `start` 里调用 → `ERR_NO_AGENT` |
| 工具权限声明 | `mount`（注册工具之后） | 同上 |
| 钩子 `register_hook` | `mount` | 只在该 agent 的轮次里派发 |
| 提示词 `set_prompt` | `mount` | 只改该 agent 的提示词 |
| 功能点声明/实现 | `mount` | 落到该 agent 的功能点注册表 |
| 图节点类型 | `mount` | 该 agent 的图注册表 |
| 图定义 `set_graph_json` | `mount` | 只改该 agent 的图；独占 slot 按 agent 判定 |
| 资源（skill/memory/mcp） | `mount`（清单声明自动应用） | 每个 agent 各应用一次（MCP 每 agent 一条连接） |
| 事件订阅 | `start` = 宿主总线（`agent` 传 `NULL`）；`mount` = 该 agent 总线 | 会话/轮次事件要订在 `mount` 里 |
| 能力 `pluginxx.capabilities` | `start` | 宿主级，跨 agent 共享；被调方靠调用方传的引用判断"谁在调" |
| 定时器 / sleep / offload | `start` 或 `mount` | 完成回调不带 agent → 注册时把 `AgentRef` 捕获进闭包 |
| `share_store` / `context` / `session` / `cancel` / 会话工作目录 | 回调里（工具/钩子/事件/图节点） | **必须带 `AgentRef`** + 会话 id |

## 6. 回调里怎么拿到 agent 引用

### 6.1 三个来源

| 回调 | 来源 |
|---|---|
| 工具执行 | 注册工具时闭包捕获的 `st->ref`，或工具 `user_data` |
| 钩子 / 功能点 / 图节点 | 同上（注册时给的 `user_data`） |
| 事件回调 | `subscribe` 时给的 `user_data` 指向 per-agent 状态 |
| `mount` / `unmount` | `spec->agent_id` / `spec->mount_seq`（SDK 包成 `AgentRef`） |
| 定时器 / `sleep` / `offload` / 任务完成 | 注册时捕获的 `AgentRef`（回调本身不带） |

### 6.2 后台线程

```cpp
void startPoller(Ctx& ctx, AgentState& st) {          // mount 时调用
    st.thread = std::thread([&ctx, ref = st.ref] {    // ← 拷一份引用进来
        while (auto msg = popMessage()) {
            // 任何线程都直接用自己那份引用
            ctx.sessions.store(ref, st.group, msg->text);
        }
    });
}
```

裸 `std::thread` 只是示意；实际优先用 `pluginxx.scheduler.offload` 或 `tasks.register_task`，
把 `AgentRef` 拷进任务上下文即可。**不要**把 `AgentRef::raw()` 的返回值长期保存 ——
保存 `AgentRef` 本身，用的时候再 `raw()`。

### 6.3 跨挂载广播

```cpp
states.forEach([&](AgentState& st) { st.notifyReconnected(); });   // 每份都带自己的引用
```

## 7. 迁移指南（本次必须改）

现有 20 个内置插件都要动，改动是机械的。

| # | 改动 | 说明 |
|---|---|---|
| 1 | 导出宏加两个参数 | `AGENTXX_PLUGIN_AGENT_EXPORT(Ctx, Name, Ver, Desc, start, stop, mount, unmount)`；没有 per-agent 状态传 `nullptr, nullptr` |
| 2 | 把 agent 侧注册从 `start` 搬到 `mount` | 典型形态：把 `setup(ctx)` 整个挪进 `mountFn`，`start` 只留 `notify->done` |
| 3 | **每个 agent 级调用加 `AgentRef`** | 注册类（工具/权限/钩子/提示词/功能点/图/资源）与读取类（session/context/model/prompt/cancel/工作目录）都要带 |
| 4 | per-agent 状态里存 `AgentRef` | `MountStore<T>::create(spec)` 自动填；工具/回调闭包捕获 `st->ref` |
| 5 | 结构体仍要填 `spec.struct_size = sizeof(...)` | `struct_size` 保留；本次基线重设为 `sizeof`，填写方式不变 |
| 6 | 钩子改单一入口 | `register_hook` 返回句柄；按点撤销改为按句柄撤销 |
| 7 | `op_cancel` 换位置 | `iface.tools->op_cancel` / `iface.feature->op_cancel` → `iface.cancel->op_cancel` |
| 8 | 重新编译 | 插件头里的 `PLUGINXX_API_VERSION` = 2；v1 二进制加载时报"API 版本低于最低要求" |
| 9 | 去掉"当前挂载"相关调用 | `currentAgentId()` / `mountInfo()` 不再存在，改用自己保存的 `AgentRef` |
| 10 | 全局状态按 agentId 分键 | 以前一个实例只服务一个 agent，现在可能多个 |
| 11 | 后台线程/定时器里别用 sessionId 猜 agent | 捕获 `AgentRef`；键用 `(agentId, sessionId)` |

### 7.1 `start` / `mount` 拆分示例

```cpp
// 旧：全部在 start 里
static void* oldStart(Ctx& ctx, const Notify* notify, PluginxxString* err) {
    registerTools(ctx);          // 每个 agent 都要有
    ctx.client = connect(ctx);   // 进程级
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

// 新：拆成两半
static void* newStart(Ctx& ctx, const Notify* notify, PluginxxString* err) {
    ctx.client = connect(ctx);   // 进程级，只一次
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}
static void* newMount(Ctx& ctx, const AgentxxPluginAgentMountSpec* spec,
                      const Notify* notify, PluginxxString* err) {
    auto* st = ctx.agents.create(AgentRef(*spec));   // per-agent 状态（内含 ref）
    registerTools(ctx, *st);                         // 只为这个 agent 注册，带 st->ref
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return st;                                       // = mount_ud
}
static void* newUnmount(Ctx& ctx, void* ud, const Notify* notify, PluginxxString*) {
    ctx.agents.erase(static_cast<State*>(ud)->ref);
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}
```

### 7.2 自查清单

- [ ] `start` 里没有任何工具/钩子/提示词/功能点/图/资源/权限注册
- [ ] 每个 agent 级调用都带了 `AgentRef`（不是空指针）
- [ ] per-agent 状态里有 `AgentRef`，且 `unmount` 里释放
- [ ] 所有可写状态有 agentId 维度，或明确是进程级
- [ ] 宏参数从 6 个改成 8 个
- [ ] 结构体的 `struct_size` 仍按 `sizeof(...)` 填写（新增字段时改用 `specHas` 判定）
- [ ] 钩子登记改为句柄式，`op_cancel` 改为 `iface.cancel->op_cancel`
- [ ] 不再调用 `currentAgentId()` / `mountInfo()`，没有长期保存 `raw()` 的地方
- [ ] 后台线程/定时器里没有"只用 sessionId 定位会话"的写法
- [ ] 卸载一个 agent 后，其他 agent 的工具/提示词/钩子仍正常（用 `plugin_multi_mount` 验证）
- [ ] 插件用 v2 头重新编译，加载时不再报版本不符

## 8. 示例：多挂载计数器

```cpp
struct CounterCtx : agentxx::plugin::PluginBase {
    struct State { AgentRef ref; int count = 0; };
    MountStore<State> states;
};

static void* counterStart(CounterCtx&, const PluginxxOperatorNotify* notify, PluginxxString*) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);   // 无进程级资源
    return nullptr;
}

static void* counterMount(CounterCtx& ctx, const AgentxxPluginAgentMountSpec* spec,
                          const PluginxxOperatorNotify* notify, PluginxxString*) {
    auto* st = ctx.states.create(AgentRef(*spec));
    tool(ctx, st->ref, "counter_inc", "Increase this agent's counter and return the new value.",
         "{}",
         [st](std::string_view) { return std::to_string(++st->count); });
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return st;
}

static void* counterUnmount(CounterCtx& ctx, void* ud,
                            const PluginxxOperatorNotify* notify, PluginxxString*) {
    ctx.states.erase(static_cast<State*>(ud)->ref);
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_EXPORT(
    CounterCtx, "example_counter", "2.0.0", "per-agent counter",
    counterStart, counterStop, counterMount, counterUnmount);
```

验证：两个 agent 各调一次都返回 `1`；A 连调 3 次后 A=4、B=1；卸载 A 后 A 的工具不可用、B 正常；
A 卸载又挂载后旧 `AgentRef`（seq 不同）被拒绝。

## 9. 与群聊插件的关系（下一篇）

群聊插件是"一个根 agent + 每个群一个会话"（`../plugin-event/`）：

- **只挂载一次**（一个根 = 一个挂载），群 ↔ 会话映射放挂载级状态；
- 进程级只放账号/连接（一个 QQ 客户端）；
- 提示词按群定制走会话级提示词来源，而不是每群一个 agent；
- 因此插件不需要"每群一个挂载"，但必须在同一挂载里维护 N 个会话的状态
  （`map<sessionId, 群状态>`，即 §4.1 的"会话级"）；后台线程带的是同一个 `AgentRef`
  加不同的 `sessionId`。

