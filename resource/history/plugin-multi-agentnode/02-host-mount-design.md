# 02 宿主 / 挂载设计（ABI v2）

约定：类都在 `agentxx::plugin`；`ctx` 指 `AgentContext`。本篇所有代码是设计示意，
不是最终签名。

## 1. 分层与所有权

```
AgentHost（进程级容器；FFI 多句柄时每句柄一个）
├── ioCtx_ / threadPool_ / hostBus_ / AgentRegistry
├── PluginManager pluginHost_        ← 唯一插件宿主
│    ├── 插件表: PluginInstance[]     ← 每插件一份（dlopen/create/start 一次）
│    ├── 挂载表: weak_ptr<PluginMount>[] ← 每 agent 一份（诊断/按 agentId 反查）
│    └── 生命周期: 加载/卸载/禁用/启用/依赖/配置
└── AgentNode[]（root + subagent）
     └── BaseAgent → AgentContext
           ├── pluginHost（借用宿主, shared_ptr）
           └── pluginMount（shared_ptr<PluginMount>）
                 ├── Entry[] ← 每个已挂载插件一条
                 └── 该 agent 的实体: 工具 / 权限声明 / 钩子表+派发器 /
                      提示词贡献 / 功能点 / 图类型 / 图定义 / 资源 / 事件订阅
```

所有权规则：

1. **宿主持有插件实例**，实例活到宿主关闭或该插件被卸载。
2. **挂载只持弱引用指向 agent**（`weak_ptr<AgentContext>`）；agent 先死时由 `destroyAgent` 拆挂载。
3. **agent 侧实体由 agent 的容器持有**，挂载只记撤销所需的句柄/名字。

## 2. 数据结构

### 2.1 宿主 `PluginManager`（per-agent → per-host）

类名保留（减少改动面），语义为"插件宿主"。

| 成员 | 说明 |
|---|---|
| `ioExecutor_` | 宿主 io 执行器；所有加载/挂载/注册/摘除都在这条线程 |
| 插件表（`PluginHostLifecycle`） | 不变 |
| `mounts_`（`vector<weak_ptr<PluginMount>>`） | 全部挂载；诊断用，也是 §3.3 按 agentId 反查挂载的来源 |
| `mountSeq_`（`uint64_t`） | 挂载序号分配器：每次成功挂载 +1，随 `MountSpec` 下发给插件 |

**搬出宿主**（原管理器成员 → 挂载）：`registry_`（直接用 `ctx->toolRegistry`）、
`permissionMiddleware()`、`hookHandlers_`/`hookDispatch_`/`hookSeq_`、`pendingCleanups_`、
`runningTurns_`/`flushPendingCleanup`/`onTurnBegin`/`onTurnEnd`、`promptKeys_`、
`graphDefinitionOwner_`/`graphDefinitionBase_`、钩子派发统计。

**从宿主删除**（本次不再需要）：`declarations_`（声明层）、`promptBackup` 相关、
"收养自建宿主" 逻辑。

```cpp
class PluginManager : public pluginxx::PluginHostLifecycle<PluginInstance>,
                      public std::enable_shared_from_this<PluginManager>,
                      public pluginxx::DomainHooks {
public:
    /// 进程级宿主（由 AgentHost 创建）
    static std::shared_ptr<PluginManager> createShared(asio::any_io_executor ex);
    /// 单 agent / 测试用法：自建宿主，加载列表随宿主一起给出
    static std::shared_ptr<PluginManager> createSelfHosted(
        asio::any_io_executor ex, std::vector<agentxx::agent::PluginConfig> load);

    /// 一次性加载全部插件（幂等；失败只记日志）
    asio::awaitable<void> loadAllAsync();

    /// 为某个 agent 建立挂载（内部逐实例 mount + 插件 mount 回调）
    asio::awaitable<std::shared_ptr<PluginMount>>
        mountAgentAsync(std::shared_ptr<agentxx::agent::AgentContext> ctx);

    /// 宿主关闭：先拆所有挂载 → stop 全部 → destroy → dlclose
    asio::awaitable<bool> shutdownAsync(std::chrono::milliseconds timeout);

    // ---- 挂载查找（插件侧经 §3 的 agent 引用间接使用） ----
    std::shared_ptr<PluginMount> findMountById(std::string_view agentId) const;  // 无 = nullptr
    PluginMount*                 mountOf(const PluginxxScopeRef* ref) const;     // §3.3
};
```

### 2.2 实例 `PluginInstance`

```cpp
class PluginInstance : public pluginxx::PluginInstanceBase {
public:
    /// 清单声明的资源（plugin.yaml 的 skill/memory/mcp）；每个挂载应用一次
    agentxx::agent::PluginResourceDecls manifestResources;

    /// 插件侧入口（加载时解析；缺失 = 该插件无对应能力）
    AgentxxPluginAgentMountFn   mountFn   = nullptr;
    AgentxxPluginAgentUnmountFn unmountFn = nullptr;

    /// 全部挂载（弱引用，诊断/反查用）
    std::vector<std::weak_ptr<PluginMount>> mounts;

    // 不再有：declarations / tools / toolNames / permissionToolNames /
    //         hookRegistrations / graphNodeTypes / featurePoints / featureImpls /
    //         promptBackup —— 全部下沉到 PluginMount::Entry
};
```

### 2.3 挂载 `PluginMount`

```cpp
/// 一个 agent 对插件的挂载集合（agent 的 init 期间创建，随 agent 销毁）
class PluginMount : public std::enable_shared_from_this<PluginMount> {
public:
    /// 单个插件的挂载记录
    struct Entry {
        std::shared_ptr<PluginInstance> inst;
        void* mountUserData = nullptr;  ///< 插件 mount 返回的 per-agent 状态
        bool  pluginMounted = false;    ///< 插件 mount 是否成功（没有 mount 入口 = true）

        // ---- 撤销所需的记录（注册时逐条追加） ----
        std::vector<std::string> tools;            ///< 已注册工具名
        std::vector<std::string> permissionTools;  ///< 已声明的工具权限
        std::vector<int64_t>     hookHandles;
        std::vector<std::string> featurePoints;
        std::vector<std::string> featureImpls;
        std::vector<std::string> graphTypes;
        std::vector<std::string> skillDirs;
        std::vector<std::string> memoryFiles;
        std::vector<std::string> mcpNamespaces;
        std::vector<std::string> promptKeys;
        std::shared_ptr<MountEventSource> eventSource;
        bool        ownsGraphDefinition = false;
        std::string graphDefinitionBase;
    };

    PluginMount(std::shared_ptr<PluginManager> host,
                std::weak_ptr<agentxx::agent::AgentContext> agent,
                std::string agentId, std::string agentName);

    const std::string& agentId() const noexcept;
    /// 挂载序号（宿主分配，单调递增；插件引用里的 `seq` 必须等于它，见 §3.2）
    uint64_t           mountSeq() const noexcept;
    /// 该挂载的插件配置覆盖（plugin.mount[].args 与加载表 args 的合并结果）
    const utilxx_base::Json& config() const noexcept;

    // ---- 仅宿主 io 线程 ----
    Entry*                findEntry(const PluginInstance* inst) const;
    asio::awaitable<bool> mountOneAsync(const std::shared_ptr<PluginInstance>& inst);
    asio::awaitable<void> unmountOneAsync(PluginInstance* inst);
    asio::awaitable<void> unmountAllAsync();
    void                  unmountAllSync();      ///< 析构/异常路径的尽力而为版

    void onTurnBegin();
    void onTurnEnd();
    void flushPendingCleanup();

    PluginManager::RegistrationInventory inventory() const;
    std::vector<PluginManager::PluginListView> listMounted() const;

    // 钩子表 / 提示词合成 / 轮次状态（原在管理器上）
};
```

### 2.4 `AgentContext`

| 字段 | 变化 |
|---|---|
| `pluginManager` | 保留，语义 = 宿主 |
| `pluginMount` | 新增，本 agent 的挂载（`mountAgentAsync` 之后非空） |
| `shutdownPluginsAsync` | **删除**（改 `pluginMount->unmountAllAsync()`） |
| `toolRegistry` / `graphRegistry` / `features` / `promptSources` / `resourceApplier` | 不变 |

调用点改动：

| 位置 | 现在 | 改为 |
|---|---|---|
| `base_agent.cpp:365` | `make_shared<PluginManager>(ctx)` | 取注入的宿主；没有则 `createSelfHosted`（§9.4） |
| `base_agent.cpp:532` | `loadConfiguredPlugins(...)` | `co_await pluginHost->mountAgentAsync(ctx)` |
| `base_agent.cpp:1217` | `pluginManager->list()` | `pluginMount->listMounted()` |
| `base_agent.cpp:1255/1689` | `flushPendingCleanup` / `onTurnBegin/End` | `pluginMount` 同名方法 |
| `base_agent.cpp:1757` | `shutdownPluginsAsync` | `co_await pluginMount->unmountAllAsync()` |
| `assembly_snapshot.cpp:96/167` | `pluginManager->list()` | 宿主列表 + 本挂载视图 |
| `assembly_snapshot.cpp:385/412` | `graphDefinitionOwner` / `hooksJson` | `pluginMount->...` |
| `agent_host.cpp:1051` | `pluginManager->hasPendingClose()` | 同样查宿主，但先确保该 agent 已拆挂载 |

## 3. agent 引用（显式传参）

**规则**：凡是要落到某个 agent 上的接口，都带一个 `const PluginxxScopeRef* agent` 参数，
由插件显式说明"这是哪个 agent"。宿主不保存"当前是哪个挂载"这类隐式状态。

### 3.1 为什么不用隐式压栈

| 隐式压栈（前稿） | 显式引用（本稿） |
|---|---|
| 主程序在每次回调前后 `push/pop` 一个宿主级栈 | 宿主没有 `mountStack_`，没有"当前" |
| 插件自建线程、定时器、任务完成回调里"不知道自己在哪"，只能拿 `lastMountHint` 或 `sessionId` 猜 | 插件在 `mount` 时拿到引用并存进自己的 per-agent 状态，任何线程都用同一份 |
| 猜错就操作到别的 agent：串会话、取消错轮次、写错存储 | 不存在的状态无法猜错；传错由宿主拒绝 |
| 将来给子 agent 挂载时，"当前栈"必须能表达嵌套 | 引用天然支持任意多个 agent 并存（含子 agent） |
| 出问题只有一处日志，看不出是谁 | 每次拒绝都带上 `agent_id` / `seq`，可诊断 |

一句话：**隐式状态在单 agent 时省事，在多 agent 时是错误来源**。

### 3.2 引用结构体

定义在通用头 `pluginxx/api/abi.h`（不放在业务头，是因为 4 张通用表也需要它，见 §3.5）：

```c
/// 主程序侧作用域的引用；agentxx 里作用域 = 一个已挂载的 agent
typedef struct PluginxxScopeRef {
    uint32_t           struct_size; ///< sizeof(PluginxxScopeRef)
    uint32_t           flags;       ///< 预留 (0)
    PluginxxStringView id;          ///< 挂载时下发的 agentId（UTF-8；借用插件内存）
    uint64_t           seq;         ///< 挂载序号；0 = 不校验
} PluginxxScopeRef;

#define PLUGINXX_SCOPE_REF_BASELINE_SIZE sizeof(PluginxxScopeRef)
```

- `id` 是**借用视图**，指向插件自己的内存，只要本次调用期间有效即可（宿主只在本调用内读它）；
- `seq` 是 `mount` 时随 spec 下发的单调序号。同一 agentId 卸载后重新挂载会拿到新序号，
  上一轮留在插件里的旧引用自动失效（否则"上一轮挂载的状态"会写进新的一轮）；
- 结构体带 `struct_size`：以后追加字段（例如 `agent_name`）时老插件传旧长度，
  宿主只读基线以内的字段，判定复用 `specSizeOk` 那一套。

### 3.3 宿主怎么校验

```cpp
/// 把引用解析成本次调用要落的挂载；失败返回 nullptr，由调用点决定返回码
PluginMount* PluginManager::mountOf(const PluginxxScopeRef* ref) const {
    if (!pluginxx_scope_ref_ok(ref)) return nullptr;                    // 为空 / 长度不足
    if (!ref->id.data || ref->id.size == 0) return nullptr;             // 该接口必须带 agent
    auto mount = findMountById(std::string_view(ref->id.data, ref->id.size));
    if (!mount) return nullptr;                                          // 没挂载过 / 已卸载
    if (ref->seq != 0 && ref->seq != mount->mountSeq()) return nullptr;  // id 被复用
    return mount.get();
}
```

失败处理统一：**注册类**返回 `AGENTXX_PLUGIN_ERR_NO_AGENT` 且不产生任何副作用；
**读取类**返回 `-1`；两种情况都记一条 WARN（含 `agent_id`、`seq`、接口名）。
`agent` 传 `NULL` 只在 §3.5 列的宿主级接口上合法，其余一律算失败。

`mount` / `unmount` 回调里要注册的东西，用的就是**本次挂载的引用**：宿主在调回调前
已经把 `(agent_id, mount_seq)` 填进 `AgentxxPluginAgentMountSpec`，插件照抄即可，
不需要问宿主"我挂在谁身上"。

### 3.4 插件侧怎么写

```cpp
// per-agent 状态：一个 agent 一份，随 unmount 释放
struct AgentState {
    agentxx::plugin::AgentRef ref;      // 值类型，内部持有 id 字符串
    std::string               group;
    std::vector<agentxx::plugin::Timer> timers;
};

struct Ctx {
    // agentId -> AgentState（只在宿主 io 线程访问，或由插件自己加锁）
    std::unordered_map<std::string, std::unique_ptr<AgentState>> agents;
};

void* mount(void* ctx, const AgentxxPluginAgentMountSpec* spec, ...) {
    auto* self = static_cast<Ctx*>(ctx);
    auto  st   = std::make_unique<AgentState>();
    st->ref    = agentxx::plugin::AgentRef(*spec);   // 复制 id + seq
    ...
    return st.release();                             // 作为 mount_ud 交回宿主
}

void on_message(void* ud, const char* text) {        // 插件自建线程
    auto* st = static_cast<AgentState*>(ud);
    // 任何线程都直接用自己那份引用，不需要"当前挂载"
    self->sessions.store(*st->ref, st->group, text);
}
```

`AgentRef` 是 SDK 的值对象：`std::string id_` + `uint64_t seq_`，`raw()` 返回
`PluginxxScopeRef`（视图指向 `id_`，所以 `raw()` 只能在 `AgentRef` 存活期间使用）。
它可拷贝、可比较、可排序（日志与诊断用）。`unmount` 时宿主把这个 `mount_ud` 原样交回，
插件在其中 `delete` 掉自己的 `AgentState`。

### 3.5 哪些接口带、哪些不带

| 接口 | `agent` 参数 | 说明 |
|---|---|---|
| `pluginxx.plugins` / `.log` / `.json` | 无 | 与 agent 无关 |
| `pluginxx.scheduler` / `.coroutine_runtime` / `.tasks` | 无 | 按实例记账，不落到 agent |
| `pluginxx.config.get_config` / `.get_plugin_args` / `.get_plugin_config_path` / `.get_language` / `.set_language` | 无 | 宿主级配置 |
| `pluginxx.capabilities.register_capability(_ex)` / `.unregister_capability` / `.has_capability` | 无 | 能力是宿主级 |
| `pluginxx.capabilities.invoke_capability_async` | 可空 | 空 = 发起方不在 agent 上下文；非空 = 归因给该 agent（被调方知道是谁在调） |
| `pluginxx.events.subscribe` | 可空 | 空 = 订阅宿主总线（进程级）；非空 = 订阅该 agent 的总线（会话/轮次事件） |
| `pluginxx.events.publish` / `.unsubscribe` | 无 | 去向由订阅句柄决定 |
| `agentxx.agent.mount.list` | 无 | 全部挂载 |
| `agentxx.agent.mount.get` | 用 `const PluginxxStringView*` 指定 id | 查一个挂载的摘要 |
| 其余全部（工具 / 权限 / 钩子 / 会话 / 上下文 / 模型 / 提示词 / 功能点 / 图 / 资源 / 取消 / 会话工作目录） | **必须非空** | 缺 = `ERR_NO_AGENT`（注册类）或 `-1`（读取类） |

## 4. 生命周期

### 4.1 加载（进程一次）

```cpp
auto host = agentxx::agent::AgentHost::create({
    .ioCtx      = agent->ioCtx,
    .pluginList = agent->agentContext->agentConfig->pluginLoadList,   // 加载表
});
co_await host->loadPluginsAsync();      // 拓扑排序 + 依赖 + builtin + create + start
co_await agent->init();                 // 内部挂载
host->attachRoot(agent);
```

`start` 阶段允许：读配置、建连接、起线程、注册能力（`pluginxx.capabilities`）、
订阅宿主总线事件（`subscribe` 的 `agent` 传 `NULL`）。此时还没有任何挂载，
agent 级接口拿不到引用 → 返回 `ERR_NO_AGENT`。

### 4.2 挂载（每 agent 一次）

```
BaseAgent::init  step "plugin_mount"（原 plugins_load）
  └─ pluginHost->mountAgentAsync(ctx)
       1. 读策略 ctx->agentConfig->pluginMountPolicy（all / none / listed）
       2. 按宿主实例顺序（拓扑序）选出要挂的实例
       3. 对每个实例：
          a. entries_.emplace_back(Entry{inst});
          b. 应用清单资源（manifestResources → 本 agent 的 resourceApplier）
          c. 若有 mount 入口：
               分配 seq = ++mountSeq_，填 MountSpec（agent_id/agent_name/mount_seq/配置…）
               co_await 驱动 mount 操作（与 start 同一套 awaitPluginLifecycle）
                 成功 → entry.mountUserData = 插件返回值
                 失败 → revoke(entry) 回滚，记 ERROR，继续下一个实例
          d. entry.pluginMounted = true
       4. 把 PluginMount 挂到 ctx->pluginMount
```

**顺序**：清单资源先应用，插件的 `mount` 后执行（这样插件能看到资源已就位，且可以覆盖
非冲突项）。注册类调用在 `mount` 回调里直接落到本 agent，不经过任何中间记录表。

### 4.3 卸载

```
unmountAllAsync()
  └─ 逆序逐实例：
       1. 若 pluginMounted && unmountFn：带 entry 的 (agent_id, seq) 驱动 unmount 操作
       2. revoke(entry)：按 §6 的顺序撤销本 agent 上的实体
       3. entries_ 清空
```

`unmount` 回调契约：应快速（只释放内存、撤销自己持有的登记）；允许异步（宿主会等），
但慢收尾会拖慢 agent 关闭，关连接/等线程这类放到 `stop`。
析构与异常路径用 `unmountAllSync()`：仍调用插件 `unmount`（同步返回的部分生效），不等异步，
记一条 WARN。

### 4.4 禁用 / 启用 / 卸载

| 动作 | 效果 |
|---|---|
| `disable(name)` | 实例 `enabled=false`；**所有挂载** `unmountOneAsync`（调插件 `unmount` + 摘实体），实例不销毁、不 `stop` |
| `enable(name)` | 实例 `enabled=true`；所有挂载重新 `mountOneAsync`（重新调插件 `mount`） |
| `unload(name)` | 所有挂载先 `unmountOneAsync`；再 `stop` + `destroy` + `dlclose` |

### 4.5 关闭顺序

| 场景 | 处理 |
|---|---|
| agent 正常关闭 | `BaseAgent::shutdownAsync` 阶段③：`co_await pluginMount->unmountAllAsync()` |
| `destroyAgent/Async` | `unmountAllSync/Async` → `registry_.remove` → 释放 agent |
| `AgentContext` 析构 | 仍有挂载 → `unmountAllSync()` + WARN |
| 宿主关闭 | 遍历 `mounts_` → `unmountAllSync()` → `stop` 全部 → `destroy` → `dlclose` |
| agent 先于宿主销毁 | 挂载随 agent 拆，`mounts_` 弱引用自动失效 |

## 5. 注册与读取（按 agent 引用定位）

### 5.1 注册类（只能在 `mount` 里）

统一三步：

1. `mountOf(agent)` 解析出本挂载（失败 = `ERR_NO_AGENT`，**不产生任何副作用**）；
2. 落到该 agent 的容器（工具注册表 / 权限中间件 / 钩子表 / 提示词 / 功能点 / 图 / 资源）；
3. 在该挂载的 `Entry` 里记一条撤销记录。

| 表 / 成员 | `agent` | 落到哪 | 撤销记录 |
|---|---|---|---|
| `tools.register_tool` / `unregister_tool` | 必须 | `ctx->toolRegistry` + `PluginTool(AgentRef)` | `Entry::tools` |
| `permission.register_tool_permission` / `unregister_...` | 必须 | 权限中间件 | `Entry::permissionTools` |
| `hooks.register_hook` / `unregister_hook` | 必须 | `PluginMount::hookHandlers_` + 派发器 | `Entry::hookHandles` |
| `prompt.set_prompt` | 必须 | `ctx->agentConfig->prompt`（贡献） | `Entry::promptKeys` |
| `feature.define_point` / `register_impl` / `undefine` / `unregister` | 必须 | `ctx->features` | `Entry::featurePoints/featureImpls` |
| `graph.register_node_type` / `unregister_node_type` | 必须 | `ctx->graphRegistry` | `Entry::graphTypes` |
| `graph.set_graph_json` | 必须 | `ctx->graphDefinitionJson`（独占 slot 按 agent 判定） | `Entry::ownsGraphDefinition` |
| `resources.register_skill_dir` / `memory` / `mcp` | 必须 | `ctx->resourceApplier` | `Entry::skillDirs/...` |
| `events.subscribe`（带 `agent`） | 必须 | 该 agent 的总线 | `Entry::eventSource`（卸载按 source 筛） |

在 `start` 里调用这些接口拿不到引用（那时还没有任何挂载），返回 `ERR_NO_AGENT`，
WARN 消息直接点明"请在 mount 里注册"。

### 5.2 读取类（按引用定位，没有兜底）

所有 agent 级读取都靠调用方给的引用定位，**宿主不做"按 sessionId 猜 agent"、
也不再有"没有引用就落到根 agent"**：

| 表 / 成员 | 定位 |
|---|---|
| `session.get_share_store` / `add_share_store` / `emit_message_tip` | 该 agent 的 share store / 会话 |
| `context.get_messages` / `messages_count` | 该 agent 的会话上下文 |
| `model.get_config` | 该 agent 的模型配置 |
| `prompt.get_prompt` | 该 agent 的合成结果 |
| `resources.get_own_resources` | 该挂载的生效项 |
| `graph.get_graph_json` | 该 agent 的图定义 |
| `permission.check_paths` | 该 agent 的权限中间件 |
| `tools.call_tool_async` | 该 agent 的工具表；找不到失败 |
| `pluginxx.config.get_session_work_dir` | 该 agent 解析出的会话工作目录 |
| `pluginxx.cancel.is_cancelled` | 该 agent 的轮次状态 |
| `hooks.list_hooks` / `feature.list_points` | 带 `agent` = 本挂载视图；`NULL` = 全部挂载的合并视图（诊断用） |
| `mount.list` / `mount.get` | 不落到 agent（§10.7） |

读取类失败统一返回 `-1`（`PluginxxString` 出参不分配），并记 WARN；不静默改落到别的 agent。

### 5.3 事件

- `start` 里 `subscribe` → 宿主总线（进程级事件）；
- `mount` 里 `subscribe(agent, ...)` → 该 agent 总线（会话/轮次事件）；
- 卸载时遍历 `inst->subscriptions`，撤销 `source == 本挂载事件源` 的那些（句柄幂等）。

### 5.4 能力

`pluginxx.capabilities` 是宿主级：`start` 里注册，跨 agent 共享。被调方需要知道"谁在调"时，
由调用方在 `invoke_capability_async` 里带上自己的 agent 引用（`NULL` = 宿主外部发起）。

## 6. 卸载时撤销什么（顺序固定）

```
事件订阅 → MCP → memory → skill → 图定义 → 图节点类型 → 提示词 →
功能点实现 → 功能点声明 → 钩子 → 工具权限 → 工具
```

理由：断连接要在摘工具之后；恢复图定义要在图类型失效之后。**不做**单一栈式回滚
（类别之间有语义顺序）。

| 类别 | 撤销动作 | 失败处理 |
|---|---|---|
| 事件订阅 | 按 source 撤销句柄 | 幂等，忽略失败 |
| MCP | `resourceApplier->removeMcpServer` | 失败记日志 |
| memory / skill | `removeMemoryFile` / `removeSkillDir` | 同上 |
| 图定义 | 恢复 `graphDefinitionBase` | JSON 解析失败保持原样 |
| 图节点类型 | slot 失活（类型名保留，见 H3） | — |
| 提示词 | 删本挂载贡献并重新合成 | 逐键独立 |
| 功能点 | `removeImpl` / `undefinePluginPoint` | 不存在则跳过 |
| 钩子 | 按句柄摘除；无处理器时轮末摘派发器 | 单条失败继续 |
| 工具权限 | `unregisterToolPermission` | 中间件缺失则跳过 |
| 工具 | `registry->unregisterTool(name)` | 返回的 shared_ptr 保活执行中的调用 |

## 7. 会话定位（agent + sessionId）

### 7.1 两层标识

| 层 | 是什么 | 从哪来 |
|---|---|---|
| **agent** | 一个已挂载的 agent（root 或将来的子 agent / 外部 agent） | `mount` 时下发 `agent_id` + `mount_seq`，插件保存为 `AgentRef` |
| **sessionId** | 该 agent 内的会话名 | 客户端给的字符串（`SessionServerAgentIO::Config::sessionId`、`WireHello.sessionId`） |

会话落盘在 `{agent 的 sessionStoreDirectory}/{sanitized(sessionId)}/session.db`，
写租约是该目录下的 `.writer.lock`。所以**会话身份 = (agent, sessionId)**，两个坐标缺一不可，
而 agent 这一层现在由参数显式给出。

### 7.2 接口怎么定位

```
agent 引用（参数） → 挂载 → 该 agent 的 SessionsManager → sessionId → Session
```

| 调用位置 | 怎么拿 agent 引用 |
|---|---|
| 工具执行 / 钩子 / 功能点 / 图节点 / 事件回调 | 插件在 `mount` 时存下来的那份（按 agent 分表），或回调 `user_data` 里带的 |
| `mount` / `unmount` 回调 | `AgentxxPluginAgentMountSpec` 直接给了 `agent_id` / `mount_seq` |
| 插件自建线程 / 定时器 / 任务 / offload 完成回调 | 同上（per-agent 状态对象里带着） |
| `start` / `stop` 回调 | **没有** → 只能调宿主级接口；需要 agent 的工作必须推迟到 `mount` |
| 宿主外部（FFI 主程序、测试） | 自己按 agentId 构造引用 |

注意"会话级"的工作在插件里通常是 **两层键**：`(agentId, sessionId)` 或
`(AgentRef, 群号)`。别退化成只用 sessionId 做键 —— 两个 agent 下同名会话是合法的。

### 7.3 同名会话：现在只剩一种真问题

| 情况 | 以前 | 现在 |
|---|---|---|
| 不同 agent、不同存储根、同一个 sessionId | 按 sessionId 路由会串数据 | **正常**：各自一份会话，引用不同、互不影响 |
| 同一存储根、同一个 sessionId，两个 agent 都用 | 写租约冲突（运行期才炸） | **仍是真问题**，但和插件无关：`AgentHost` 在挂载 agent 时比对规范化后的存储根，相同就记 ERROR 并拒绝第二个 agent（早失败，而不是等写库时才发现） |
| 子 agent 复用父会话 | 子 agent 不挂载，不参与 | 本期子代理 `plugin.mount=none`，不产生引用；将来给子 agent 挂载时它有自己的 `agent_id`（如 `sub:3`），引用机制已经支持 |

也就是说：**"同一份磁盘会话被两个 agent 打开"从"插件猜错"降级成"配置错误，宿主直接拒绝"**。

### 7.4 相比前稿删掉的东西

| 删掉 | 原因 |
|---|---|
| `PluginManager::resolveBySession()` | 不再按 sessionId 猜 agent |
| `PluginManager::rootMount()` | 不再有"没引用就落到根 agent"的兜底 |
| `PluginManager::mountStack_` / `MountScope` | 不再有"当前挂载" |
| `PluginInstance::lastMountHint` | 不再有"最近一次回调属于谁"这种近似 |

危害也随之消失：写错 share store、读错上下文、`is_cancelled` 误判别的轮次、
权限判定用错工作目录 —— 这些都需要"猜"，猜的动作已经不存在。

### 7.5 迁移提示

- 原来"不带 agent 的调用"：改成在 `mount` 里存 `AgentRef`，调用时带上；
- 原来"用 sessionId 当全局键"：改成 `(agentId, sessionId)` 或 per-agent 一张表；
- 原来"假设进程里只有一个 agent"：只能在 `plugin.mount=none` 的插件里继续成立
  （那种插件本来就不该碰 agent 级接口）。

## 8. 线程模型

| 项 | 约定 |
|---|---|
| 宿主线程 | 宿主 io executor 单线程；插件表、挂载表、各挂载的钩子表只在这条线程 |
| 挂载发起 | 必须在 agent 的 io 线程且与宿主同一 `io_context`；否则拒绝（子代理默认不挂载，C6） |
| 插件回调 | 全部在宿主 io 线程（操作协议：`start`/`cancel` 由宿主驱动，`done` 可任意线程） |
| 跨线程调用 | `ioCallSyncKeep` 投递 + 等待；agent 引用随闭包带入（§3.3） |
| 工具执行 | 调用方线程池；工具对象自带 `(agent_id, seq)` 引用，调用插件前投递到宿主线程 |
| 关闭 | `unmountAllAsync`（宿主线程）；`stop`/`destroy` 走 `shutdownAsync` |

## 9. 配置与启动顺序

### 9.1 yaml

```yaml
plugin:
  list:                     # 加载表：进程一次，只有根配置会读
    - path: builtin://agentxx_filesystem
    - path: ./plugins/qq
      args: { accounts: ["123456"] }
      config: ./plugins/qq/qq.yaml

  mount: all                # 本 agent 挂哪些：all / none / 列表；不写 = all
  # mount:
  #   - name: agentxx_filesystem
  #   - name: qq
  #     args: { groups: ["123"] }     # 覆盖加载表 args，仅本挂载可见
```

`AgentConfig`：

```cpp
enum class PluginMountPolicy { All, None, Listed };

struct PluginMountConfig {
    std::string       name;           ///< 插件名（plugin.yaml 的 name）或路径
    bool              enabled = true;
    utilxx_base::Json args;           ///< 与加载表 args 浅合并后作为 mount 的 config_json
};

std::vector<PluginConfig>      pluginLoadList;                        ///< 原 plugins（改名）
PluginMountPolicy              pluginMountPolicy = PluginMountPolicy::All;
std::vector<PluginMountConfig> pluginMounts;
```

- 子代理：`makeSubagentConfig` 设 `None`；
- `Listed` 里未命中的实例不挂载；命中但 `enabled=false` 的跳过；
- 名称解析：加载后宿主建 `path → name`、`name → 实例` 两张表；`name` 查不到再按路径查，
  都查不到记 WARN 跳过。

### 9.2 五处启动路径

```cpp
// 以 mode_runners.cpp 为例（其余四处同形）
asio::co_spawn(*agent->ioCtx, [agent, serverIO, clientIO, sessionId]() -> asio::awaitable<void> {
    auto host = agentxx::agent::AgentHost::create({
        .ioCtx      = agent->ioCtx,
        .pluginList = agent->agentContext->agentConfig->pluginLoadList,
    });
    co_await host->loadPluginsAsync();
    co_await agent->init();          // 内部挂载
    host->attachRoot(agent);
    clientIO->requestAppendComponentInfo(sessionId);
    clientIO->onServerReady();
    co_await serverIO->run();
}, asio::detached);
```

| 文件 | 行 | 改法 |
|---|---|---|
| `client/src/mode_runners.cpp` | 232~242 | 如上；`attachRootAgentHost` 删除 |
| `client/src/io/jsonl/jsonl_mode.cpp` | 118~124 | 同上 |
| `client/main.cpp`（agent server） | 1036~1040 | 同上；`hostHolder` 存宿主 |
| `lib/src/ffi/ffi_runtime.cpp` | 429~452 | 同上 |
| `benchmark/**` | 各场景 | 需要插件的场景接宿主；其余走自建宿主（§9.3） |

### 9.3 自建宿主（单 agent / 测试 / 基准）

`plugin_system` 步骤：

```cpp
if (!ctx->pluginHost) {
    // 没有外部宿主：自建一份，只服务本 agent（不与他人共享）
    ctx->pluginHost = plugin::PluginManager::createSelfHosted(
        co_await asio::this_coro::executor, ctx->agentConfig->pluginLoadList);
    XX_LOGI("No plugin host attached; using a private host for this agent");
}
```

与旧设计的区别：**没有"收养"**。自建宿主永远只服务它的 agent；要共享就在启动时先建宿主
（§9.2）。`AgentHost` 不再接管 agent 自建的宿主，也不提供 `attachRootAgentHost`。

### 9.4 测试

`makeAgentCtx()` 改为：

```cpp
ctx->pluginHost  = plugin::PluginManager::createSelfHosted(ex, {});
ctx->pluginMount = co_await ctx->pluginHost->mountAgentAsync(ctx);
```

需要 `co_await` 的测试把构造挪进协程；不涉及挂载的纯同步测试可只建宿主、不挂载。

## 10. ABI 演进契约（v2）

### 10.1 三个版本号、两条判定

| 值 | 归属 | 本次 | 含义 |
|---|---|---|---|
| `PLUGINXX_API_VERSION` | 框架头（主程序与插件各自编译期带入） | 1→2 | 本框架的 SDK 版本；插件的 `get_info` 上报的就是**它编译时**的这个值 |
| `PLUGINXX_MIN_API_VERSION` | 框架头（只有主程序用它判定） | 1→2 | 框架能接受的**最老**插件 SDK 版本 |
| `PluginxxInfo.api_version` | 插件上报（导出宏自动填） | 自动变 2 | 插件编译时的框架 SDK 版本 |
| `X_VERSION` | 每张表 | 改动的表 1→2 | 表的布局/语义级别 |
| `X_MIN_VERSION` | 每张表（插件侧常量） | 改动的表 1→2 | 插件能接受的**最低**表版本 |
| `X_BASELINE_SIZE` / `struct_size` | 每张表 | 改动的表 = 新 `sizeof` | 表基线 / 主程序实际提供的长度 |

判定链（顺序固定，每条各管一个方向）：

```
① 加载：主程序读 PluginxxInfo.api_version
        要求 api_version >= PLUGINXX_MIN_API_VERSION
        → 不满足：拒绝加载（dlopen 之后、create 之前），报错带重编提示
② 取表：插件 queryInterface<Iface>() 内部 validateInterface 判
        IID 命中 && version >= MIN_VERSION && struct_size >= BASELINE_SIZE
        → 不满足：返回 NULL，插件走"主程序不支持"分支
③ 取成员：插件读基线之外（追加段）的成员前用 ifaceHas<&T::m>
```

为什么只有这三条就够：

- ① 只有**主程序**能做（它读插件的 `PluginxxInfo`），管"旧插件 + 新框架"；
- ②③ 只有**插件**能做（它读主程序给的静态表），管"新插件 + 旧框架"；
- 两个方向各有一条闸，不需要别的字段。

"插件要求最低框架版本"**不新增 `PluginxxInfo` 字段**，而是用 ② 表达：插件把它用到的
那张表的 `X_MIN_VERSION` 提到该布局的版本号，老框架给的表 `version` 更低 → `NULL`，
插件自己降级。这样粒度更细（可以只在那张表上升），也避开一个坑：`PluginxxInfo` **没有**
`struct_size`，给它加字段会让"新框架读旧插件"越界读（旧插件返回的是自己那份短结构体）。

插件判断"框架有没有某个能力/新表"一律用 `queryInterface` 是否返回 `NULL`
（例如挂载表 `agentxx.agent.mount`），不要靠宏 —— 宏是编译期的，判不出运行中的主程序。

### 10.2 双向兼容矩阵

| 插件 \ 框架 | v1（本次之前） | v2（本次） | v3（未来，`MIN` 仍为 2） | v3（未来，`MIN` 提到 3） |
|---|---|---|---|---|
| v1 插件 | 正常 | **拒绝加载**（① `1 < 2`） | 拒绝（`1 < 2`） | 拒绝（`1 < 3`） |
| v2 插件（本次） | 加载通过（`2 >= 1`），取改过的表 `version 1 < MIN 2` → `NULL` → 降级 | 正常 | 正常（② 表 `version 3 >= 2`；不读新成员） | **拒绝加载**（① `2 < 3`） |
| v3 插件（未来） | 加载通过（`3 >= 1`），用到 v3 表/成员的按 ②③ 判空降级 | 同左 | 正常 | 正常 |

一句话：**框架向后兼容"老一点的插件"，能老到哪一代由 `MIN_API_VERSION` 决定；
插件向后兼容"老一点的主程序"，能老到哪一代由每张表的 `MIN_VERSION` / `BASELINE` 决定。**

### 10.3 本次只做两件事

| 动作 | 值 | 挡住谁 |
|---|---|---|
| 抬下限 | `PLUGINXX_API_VERSION`、`PLUGINXX_MIN_API_VERSION` = 2 | 用 v1 SDK 编的插件（整体换代） |
| 升表版本 | 每个改动的表：`X_VERSION` = 2、`X_MIN_VERSION` = 2、`X_BASELINE_SIZE` = 新 `sizeof` | v2 插件放到 v1 主程序上（局部换代） |

**IID 一个都不改。** 理由：

1. 本次每张改动的表都加了参数，`sizeof` 与 v1 不同 → v1 插件用自己头里的
   `BASELINE_SIZE` 去比主程序给的 `struct_size` 必然失败，本来就被拦；
2. 就算某张表大小恰好撞回旧值，还有两条兜底：v1 插件 + v2 框架被 ① 拒，
   v2 插件 + v1 框架被 ② 拒（`version 1 < MIN 2`）。**不存在能落到"按旧布局读新表"
   的组合**；
3. 改 IID 只有成本（新 IID 字符串、三处台账、文档与测试同步），没有额外保护。
   它真正有用的场景是"同一位置并存两代契约"（例如允许两代插件同进程运行），
   真出现时再改不迟。

### 10.4 本次的变更清单

| 表 | 布局变化 | `VERSION` | `MIN_VERSION` | `BASELINE` |
|---|---|---|---|---|
| `agentxx.agent.tools` | 每个成员加 `agent`；删 `op_cancel`（并入 `pluginxx.cancel`） | 1→2 | 1→2 | 新 `sizeof` |
| `agentxx.agent.hooks` | 加 `agent`；基础/扩展双入口合并为 3 个句柄式成员 | 1→2 | 1→2 | 新 `sizeof` |
| `agentxx.agent.feature` | 加 `agent`；删 `op_cancel` | 1→2 | 1→2 | 新 `sizeof` |
| `agentxx.agent.permission` | 加 `agent` | 1→2 | 1→2 | 新 `sizeof` |
| `agentxx.agent.session` | 加 `agent` | 1→2 | 1→2 | 新 `sizeof` |
| `agentxx.agent.context` | 加 `agent` | 1→2 | 1→2 | 新 `sizeof` |
| `agentxx.agent.model` | 加 `agent` | 1→2 | 1→2 | 新 `sizeof` |
| `agentxx.agent.prompt` | 加 `agent` | 1→2 | 1→2 | 新 `sizeof` |
| `agentxx.agent.resources` | 加 `agent` | 1→2 | 1→2 | 新 `sizeof` |
| `agentxx.agent.graph` | 加 `agent` | 1→2 | 1→2 | 新 `sizeof` |
| `agentxx.agent.mount` | **新增**（`list` / `get`，§10.7） | 1（第 1 代） | 1 | `sizeof` |
| `pluginxx.events` | `subscribe` 加 `agent`（可空） | 1→2 | 1→2 | 新 `sizeof` |
| `pluginxx.capabilities` | `invoke_capability_async` 加 `agent`（可空） | 1→2 | 1→2 | 新 `sizeof` |
| `pluginxx.config` | `get_session_work_dir` 加 `agent` | 1→2 | 1→2 | 新 `sizeof` |
| `pluginxx.cancel` | `is_cancelled` 加 `agent`；表尾追加 `op_cancel` | 1→2 | 1→2 | 新 `sizeof` |
| 其余 6 张通用表（`scheduler` / `coroutine_runtime` / `plugins` / `json` / `log` / `tasks`） | 不动 | 1 | 1 | 不变 |

规则压成一句：**改动过的表 = `VERSION` 2 / `MIN_VERSION` 2 / `BASELINE` = 新 `sizeof`；
没动的表一个字节都不动。**

新增的结构体（`PluginxxScopeRef`、`AgentxxPluginAgentMountSpec`）也从"基线 = `sizeof`"起步，
以后追加字段时基线改 `offsetof`、读取方用 `specHas` —— 与表的规则完全一样。

判定链各条落在哪：

| 检查 | 实现位置 | 常量来源 |
|---|---|---|
| ① 全局下限 | `pluginxx/host/lifecycle.h` 的加载函数（动态库与内置插件同一处） | 主程序头 `abi.h` |
| ② 整表可用 | `pluginxx::validateInterface`（`pluginxx/kit/kit.h`，被 `queryInterface` 调用） | 插件头 |
| ③ 追加成员 | `pluginxx::ifaceHas` / `ifaceHasVersion`（`pluginxx/api/iface_meta.h`） | 插件头 |
| spec 字段 | `specSizeOk` / `specHas`（`agentxx/plugin/api/plugin_spec.h`） | 插件头 |

v1 插件被拒时的日志（`plugin_runtime` 有断言用例）：

```
plugin 'qq_group' built with API v1, host requires >= 2; rebuild the plugin
```

### 10.5 四件套写法（示例）

改布局的表：**IID 不变**，只把版本提到 2、基线重设为新布局（以 `hooks` 为例）：

```c
#define AGENTXX_PLUGIN_IFACE_AGENT_HOOKS                 "agentxx.agent.hooks"   /* 不变 */
#define AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_VERSION         2   /* 1 -> 2：布局变了 */
#define AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_MIN_VERSION     2   /* 1 -> 2：低版本主程序给不出这个布局 */
#define AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_BASELINE_SIZE sizeof(AgentxxPluginHooksIface)

typedef struct AgentxxPluginHooksIface {
    int32_t  version;     ///< 主程序填 AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_VERSION
    uint32_t struct_size;

    /// 登记一个处理器，返回句柄（一个实例一个点可登记多个）
    int32_t(PLUGINXX_CALL* register_hook)(
        const PluginxxHost*          host,
        const PluginxxScopeRef*      agent,   ///< 新增：落到哪个 agent（§3）
        const AgentxxPluginHookSpec* spec,
        int64_t*                     out_handle);
    /// 按句柄撤销
    int32_t(PLUGINXX_CALL* unregister_hook)(const PluginxxHost* host, int64_t handle);
    /// 清单（诊断/界面；agent 可空 = 全部挂载合并视图）
    int32_t(PLUGINXX_CALL* list_hooks)(
        const PluginxxHost* host, const PluginxxScopeRef* agent, PluginxxString* out);
} AgentxxPluginHooksIface;
```

以后再追加成员（本次没有这种表，留作规则）：基线**保持原值**，新成员落在基线之外，
读取方用 `ifaceHas` 判：

```c
#define PLUGINXX_IFACE_LOG_VERSION        2        /* 1 -> 2 */
#define PLUGINXX_IFACE_LOG_MIN_VERSION    1        /* 不变：老主程序仍可用，只是没有新成员 */
#define PLUGINXX_IFACE_LOG_BASELINE_SIZE  offsetof(PluginxxLogIface, flush) /* = 原来的 sizeof */
```

读取方（以后读追加成员时统一这个写法）：

```cpp
if (ifaceHas<&PluginxxCancelIface::op_cancel>(cancel) && cancel->op_cancel) {
    cancel->op_cancel(op);
} else {
    // 老主程序：没有该成员，按超时处理
}
```

### 10.6 结构体（spec / input）

- 全部保留 `struct_size`（`_reserved` 补齐保留）；
- 本次允许重排/删字段：`AGENTXX_*_BASELINE_SIZE` 重设为**新完整布局**（`sizeof`），
  即"v2 起，基线就是当前布局"；
- 未来追加字段：基线改为 `offsetof`，读取方 `specHas(&Spec::新字段, spec->struct_size)`；
- 新增 `PluginxxScopeRef`（§3.2）放在通用头 `pluginxx/api/abi.h`：带 `struct_size` + `flags`，
  基线 = `sizeof`；
- 新增 `AgentxxPluginAgentMountSpec` 按同一约定：带 `struct_size`、`flags`、`_reserved`，
  基线 = `sizeof`。

```c
typedef struct AgentxxPluginAgentMountSpec {
    uint32_t           struct_size;   ///< sizeof(本结构体)
    uint32_t           flags;         ///< 预留 (0)
    PluginxxStringView agent_id;      ///< "root" / "sub:3" / "ext:qq:1"
    PluginxxStringView agent_name;
    PluginxxStringView work_dir;      ///< 绝对路径 UTF-8
    PluginxxStringView language;
    PluginxxStringView model_json;
    PluginxxStringView config_json;   ///< 加载 args ⊕ plugin.mount[].args
    uint64_t           mount_seq;
} AgentxxPluginAgentMountSpec;

#define AGENTXX_PLUGIN_AGENT_MOUNT_SPEC_BASELINE_SIZE sizeof(AgentxxPluginAgentMountSpec)
```

主程序填 `struct_size = sizeof(...)`；插件侧用 `specSizeOk<AgentxxPluginAgentMountSpec>()` 校验，
新增字段用 `specHas`。

### 10.7 新增表 `agentxx.agent.mount`

```c
#define AGENTXX_PLUGIN_IFACE_AGENT_MOUNT             "agentxx.agent.mount"
#define AGENTXX_PLUGIN_IFACE_AGENT_MOUNT_VERSION     1
#define AGENTXX_PLUGIN_IFACE_AGENT_MOUNT_MIN_VERSION 1
#define AGENTXX_PLUGIN_IFACE_AGENT_MOUNT_BASELINE_SIZE sizeof(AgentxxPluginMountIface)

typedef struct AgentxxPluginMountIface {
    int32_t  version;
    uint32_t struct_size;

    /// 全部挂载的摘要 JSON 数组
    int32_t(PLUGINXX_CALL* list)(const PluginxxHost*, PluginxxString* out);
    /// 按 agentId 查一个挂载的摘要；没有这个挂载返回 -1
    int32_t(PLUGINXX_CALL* get)(
        const PluginxxHost*, const PluginxxStringView* agent_id, PluginxxString* out);
} AgentxxPluginMountIface;
```

只有两个成员：插件自己就拿着 agent 引用（§3.2），不需要再问宿主"我在谁身上"；
`list` / `get` 的用途是诊断与"我在这几个 agent 上是否都挂好了"这类自查。

```json
{"agentId":"root","agentName":"Agentxx","workDir":"D:/work","language":"zh-cn",
 "mountSeq":1,"config":{"groups":["123"]}}
```

### 10.8 入口符号与导出宏

```c
/* 与 start/stop 同形 */
typedef void*(PLUGINXX_CALL* AgentxxPluginAgentMountFn)(
    void* plugin_ctx, const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify* notify, PluginxxString* out_error);
typedef void*(PLUGINXX_CALL* AgentxxPluginAgentUnmountFn)(
    void* plugin_ctx, void* mount_ud,
    const PluginxxOperatorNotify* notify, PluginxxString* out_error);

PLUGINXX_EXPORT void* agentxx_plugin_agent_mount(...);
PLUGINXX_EXPORT void* agentxx_plugin_agent_unmount(...);
```

```c
/* 一套宏，固定 8 个参数；没有 per-agent 状态的插件传 nullptr */
#define AGENTXX_PLUGIN_AGENT_EXPORT(Ctx, Name, Ver, Desc, StartFn, StopFn, MountFn, UnmountFn) \
    PLUGINXX_EXPORT_PLUGIN(agentxx_plugin_agent_, Ctx, Name, Ver, Desc, StartFn, StopFn)      \
    AGENTXX_PLUGIN_AGENT_MOUNT_EXPORT(Ctx, MountFn, UnmountFn)

/* 手写 create/destroy 的插件用这个 */
#define AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT(Ctx, StartFn, StopFn, MountFn, UnmountFn)
```

新增入口符号是**能力新增**，不是表布局变化：v1 主程序不会查找它（插件侧 `mount` 也就不会被调用）。

### 10.9 错误码

```c
/// 需要 agent 的接口上，引用缺失/无效/指向已卸载的挂载/seq 不匹配（含"在 start 里注册工具"）
#define AGENTXX_PLUGIN_ERR_NO_AGENT (-2)
```

注册类接口统一返回它（不产生任何副作用）；读取类仍返回 `-1`；两者都记 WARN，
WARN 里带接口名 + `agent_id` + `seq`，方便定位是哪个 agent 的问题。

### 10.10 表数量与落点

| 侧 | 数量 | 说明 |
|---|---|---|
| 通用表 `pluginxx.*` | 10 | 4 张升到 v2（`events`/`capabilities`/`config`/`cancel`），6 张不动 |
| 业务表 `agentxx.agent.*` | 11 | 原 10 张全部升到 v2 + 新增 `mount`（v1）；IID 全部不变 |
| 合计 | **21** | `PluginManager::kInterfaceTableCount = 10 + 11` |

落点（缺一不可，`boundaries` 会逐项校验）：

| 位置 | 改动 |
|---|---|
| `pluginxx/api/abi.h` | `PLUGINXX_API_VERSION` 2、`PLUGINXX_MIN_API_VERSION` 2、新增 `PluginxxScopeRef` |
| `pluginxx/api/tables.h` | 4 张表加 `agent` 参数 / 追加 `op_cancel`，每张 `VERSION` 2 + `MIN` 2 + `BASELINE` 新 `sizeof`；其余 6 张不动 |
| `pluginxx/api/iface_meta.h` | 4 张变动表的 `IfaceMeta` 基线改新 `sizeof` |
| `pluginxx/kit/kit.h` | `validateInterface` 不变；SDK 新增 `AgentRef` / `scopeOk` |
| `agentxx/plugin/api/plugin_api.h` | 10 张表加 `agent` 参数与配套成员改动、`mount` 表、`MountSpec`、`ERR_NO_AGENT` |
| `agentxx/plugin/api/plugin_iface_meta.h` | 10 张变动表 + `mount` 表的 `IfaceMeta` 特化 |
| `agentxx/plugin/api/plugin_spec.h` | `AgentxxPluginAgentMountSpec`；其余基线重设为 `sizeof` |
| `agentxx/plugin/api/plugin_kit.h` | `AgentIfaces::mount`、SDK、8 参数宏、`AgentRef` 引入 |
| `plugin_manager_vtable.cpp` | `g_ifaceMount` + 分发 + 三实现；各表的 `agent` 参数校验（`mountOf`） |
| `test_boundaries.cpp` / `test_plugin_runtime.cpp` | 台账与用例按 v2 更新（含"v1 插件被拒"、"传错 agent 被拒"） |
| `docs/zh-cn/design/plugins.md` §17 | 更新版本表与"本次换代"的说明 |

## 11. 历史包袱清零（执行清单）

与 `01` §4 一一对应。**这里只删与 ABI 演进无关的历史处理**；§10 的演进机制一律保留。

| 组 | 动作 |
|---|---|
| 入口 | 删 `attachRootAgentHost`；删"宿主收养自建宿主" |
| 实例 | 删 `PromptBackup` / `restorePromptBackup` / `promptKeys_` 的 base-applied 旧模型（只留贡献） |
| 钩子 | 合并基础/扩展为句柄式；删按点撤销与 `base` 标志 |
| 操作取消 | `tools`/`feature` 的 `op_cancel` 收拢到 `pluginxx.cancel` |
| 宏 | 两套导出宏合并为一套 8 参数 |
| 配置 | 删 `warnLegacySectionKeys` 与纯列表废弃告警；`AgentConfig::plugins` 改名 `pluginLoadList` |
| 代码注释 | 去掉"兼容旧契约""不再区分""重置前"等历史解释（`plugin_graph_node.cpp` 的 messages 改写保留功能、改表述） |
| 文档 | `plugins.md` §13/§15.7/§17 按 v2 更新；`AGENTS.md` 契约段更新；`docs/zh-cn/design/index.md` 补一句 |

**明确不做**（否则等于放弃未来兼容）：

- 不删 `version` / `struct_size` / `_VERSION` / `_MIN_VERSION` / `_BASELINE_SIZE`；
- 不删 `IfaceMeta` / `ifaceHas` / `ifaceHasVersion` / `validateInterface`；
- 不删 `plugin_spec.h` / `plugin_iface_meta.h` / `specSizeOk` / `specHas` / 结构体 `struct_size`；
- 不删 `boundaries` 的四件套校验与成员顺序台账。

## 12. 诊断

`PluginManager::list()` 增加：

```cpp
std::vector<std::string> mountAgents;   ///< 当前挂载该插件的 agent
size_t                   mountCount = 0;
bool                     hasMountEntry = false;
size_t                   manifestResourceCount = 0;  ///< 清单资源条数
```

加载快照（`assembly_snapshot.cpp`）：

| 段 | 内容 |
|---|---|
| `plugins` | 宿主视角：每插件挂载数、挂载 agent、清单资源、禁用原因 |
| `plugins_agent`（新增） | agent 视角：本挂载挂了哪些插件、实体计数（`RegistrationInventory`） |
| `hooks` | 本挂载的钩子表 |
| `graph_definition_owner` | 本挂载的占用者 |

日志要点：

| 事件 | 级别 | 内容 |
|---|---|---|
| 宿主加载完成 | INFO | `plugin host: N plugins in X ms` |
| 挂载完成 | INFO | `agent 'root' mounted N/M plugins in X ms` |
| 未挂载 | INFO | 原因（不在列表 / 禁用 / mount 失败） |
| 注册冲突 | WARN | 哪个 agent、哪个工具/类型、与谁冲突、已跳过 |
| agent 引用无效的注册 | WARN | 表名 + 方法名 + `agent_id` + `seq` + "引用无效，请在 mount 里注册" |
| 卸载 | INFO | `agent 'root' unmounted N plugins` |
| 版本不符 | ERROR | `plugin built with API v1, host requires >= v2; rebuild` |

## 13. 预算与上限

| 项 | 上限 |
|---|---|
| 每宿主已加载实例数 | 64 |
| 每实例挂载数 | 64 |
| 每挂载钩子/事件订阅 | 不设上限（沿用） |

超限拒绝并记 ERROR（`ERR_BUDGET` 语义）。

## 14. 行为变化（对照现状）

单 agent 场景 1~6 应完全等价（验收标准：现有插件测试行为不变）。

| # | 变化 | 单 agent 等价 | 说明 |
|---|---|---|---|
| 1 | 实例从每 agent 一份 → 每宿主一份 | 是 | — |
| 2 | agent 侧注册从 `start` 移到 `mount` | 是（同一 init 步骤） | 内置插件要改代码 |
| 3 | 在 `start` 里注册 → 报 `ERR_NO_AGENT` | 是（改完后不会发生） | 明确失败，不静默 |
| 4 | 工具与内置工具重名被拒绝 | 可能（以前漏检） | 修 H1 |
| 5 | agent 关闭不再 `stop` 插件 | 是（延迟到宿主关闭） | 迁移见 `03` §7 |
| 6 | 钩子表/派发器搬挂载 | 是 | — |
| 7 | `set_graph_json` 只在挂载内生效 | 是 | 不再有"全局模板" |
| 8 | 插件工具不受 `toolWhitelist` 过滤 | 是（现状如此） | 文档写明 |
| 9 | 接口契约换代 v2 | 否（重编后等价） | `API`/`MIN_API` 1→2；14 张表 `VERSION`/`MIN_VERSION`→2（**IID 不变**）；版本/基线机制保留 |
| 10 | `AgentConfig::plugins` 改名 `pluginLoadList` | 是 | 配置键 `plugin.list` 不变 |

## 15. 失败与边界

| 情形 | 处理 |
|---|---|
| 插件版本不符 | 拒绝加载，ERROR + 重编译提示；不影响其他插件 |
| 加载失败 | 跳过并给"怎么改"建议，不阻断启动 |
| `mount` 返回失败 | 只在**该 agent** 回滚该插件；其他 agent 不受影响；ERROR |
| 同 agent 重复挂载 | 幂等：已有 entry 直接返回成功 |
| 挂载期间单条注册失败 | WARN 继续（不因一个工具失败而整体失败） |
| 卸载期间插件又注册 | 实例 `enabled=false` 已生效 → 返回 `ERR_NO_AGENT` |
| agent 引用无效的注册 | `ERR_NO_AGENT` + WARN，无副作用 |
| agent 引用无效的读 | `-1` + WARN（**不再落到根 agent**） |
| 插件在自己线程里注册 | 允许（投递到宿主线程）；引用有效就生效，无效则 `ERR_NO_AGENT`。文档要求注册放 `mount` |
| agent 先销毁 | `destroyAgent` 先拆挂载；弱引用自动失效 |
| 宿主先销毁 | 不允许：`AgentHost` 先拆 agent 再关宿主；自建宿主随 ctx 释放 |

## 16. 对下一篇（`plugin-event`）的接口约束

`../plugin-event/` 要在本篇之上加"插件创建根 + 会话通道 + 事件触发"。它依赖本篇的加载模型，
所以下面几条在本篇落地时就固定，下一篇实现时不能再改：

| # | 约束 | 为什么 | 详见 |
|---|---|---|---|
| S1 | **进程内只有一份插件宿主**：`AgentHost` 持有它，任何 agent（含插件后来创建的根）都必须使用宿主注入的 `ctx->pluginHost` | `create/start` 只能一次；`BaseAgent::init` 的 `createSelfHosted` 分支只允许"没有外部宿主"的单 agent / 测试场景 | §9.3 |
| S2 | 插件创建的根**也是一个 agent**，走同一条 `mountAgentAsync` 路径，按该根的 `pluginMountPolicy` 挂载 | 根要有文件/命令工具，就得把那几个插件挂到它身上 | §4.2 |
| S3 | 插件创建的根默认 `pluginMountPolicy = None`（只有内置工具），要工具由创建者显式给 `Listed` / `All` | 最小权限：不该因为"某个插件建了个根"就把整套 coding 插件塞进那个根。注意 `AgentConfig::pluginMountPolicy` 的默认值仍是 `All`，那只对内置根/普通 agent 生效；**不要把全局默认改成 None** | §9.1 |
| S4 | 创建根的插件默认**不挂到自己的根上**（`include_creator = false`） | 结构性切断"我在我的根里再建一个根"；需要给根提供本插件工具时显式打开 | §4.2 |
| S5 | **在 `mount` 回调里调用"创建根"必须被拒绝**（宿主在驱动 mount 期间记 `mounting_`，直接返回错误） | `mount` 每个 agent 一次，允许在里面建根就等于允许无限嵌套 | §4.2 |
| S6 | 会话占用键是 `(存储根, sessionId)`，不是单独的 sessionId | 不同存储根可以同名；同存储根必须独占（`SessionWriterLease` 在进程内可重入，撞了不报错） | §7.3 |
| S7 | 下一篇只新增一张表 `agentxx.agent.runtime`（第 1 代）；表数量 21 → 22；版本/基线机制不变 | 避免为了加一张表再抬一次下限 | §10 |
| S8 | 运行时的根 / 通道 / 触发器是**实例级**对象（`start` 里创建、`stop` 里撤销）；挂载级撤销只管工具 / 权限 / 钩子 / 提示词 / 功能点 / 图 / 资源 / 事件 | 两套生命周期混进一张记账表会让禁用与卸载语义不清 | §6 |

根与插件挂载的名字容易混，统一说法：

| 说法 | 含义 |
|---|---|
| 挂载根 | 把一个根 agent 挂到 `AgentHost`（下一篇的 `mount_root`） |
| 插件挂载 | 把一个已加载插件挂到某个 agent（本篇的 `PluginMount`） |
| 根挂载 | 同"挂载根"，在 `../plugin-event/` 里沿用 |
