# Agentxx 插件框架与插件体系 API v1 终极重构设计方案

> 目标：彻底抛弃历史版本的兼容性包袱，完全重置插件框架与所有插件；统一所有 API 与接口表版本为 1；根除已发现的并发死锁、野指针崩溃、内存泄漏与多实例污染 Bug；大幅提升代码复用、简化插件开发并消灭样板代码。
> 
> 目标文件：`/home/coolight/program/agentxx/resource/history/plugin-refactor-1/plugin.md`

---

## 1. 重构背景与核心目标

### 1.1 背景现状与历史遗留问题
Agentxx 插件系统基于纯 C ABI 与 COM 风格接口表查询，但在演进过程中积累了多项历史技术债务与架构缺陷：
1. **版本碎片化与历史包袱**：部分接口表版本为 2、部分为 3，且在 `plugin_kit.h` 内部并存着两套工具适配器（历史遗留的旧 C 风格 `registerSyncTool` / `SyncJob` 手工 malloc 拷贝套件与现代 `tool` / `blocking_tool` 并存），增加了维护成本；
2. **并发安全漏洞**：`CallToolAwaiter` 与 `InvokeCapAwaiter` 存在 Check-Then-Act 异步竞态死锁；跨边界异步回调在插件销毁时存在 Use-After-Free (UAF) 隐患；
3. **加载失败清理缺失**：宿主 Agent 侧插件在 `create` 失败时未调用 `detachAll`，导致已注册的工具/事件指针残留，随后在卸载动态库后引发非法内存访问（SIGSEGV）；
4. **多实例隔离被破坏**：个别插件存在可变全局变量（如 `agentxx_system_monitor` 的 `g_log_sink`）或无锁函数级 static 缓存（如 `text_selection_monitor` 的 `cachedPort`），违反单进程多宿主隔离原则；
5. **插件开发样板代码极其沉重**：每个插件都需要手写数百行深层嵌套的 `neograph::json` Schema，缺乏类型安全的参数提取机制；Client 端缺乏统一基类 `ClientPluginBase`，每个插件都必须自行手写一遍 `struct ClientCtx`。

### 1.2 核心设计原则（完全重置）
* **彻底无兼容包袱**：完全清空历史兼容妥协，重构后全局 API 版本及全部 23 张接口表版本统一重置为 **版本 1**；
* **零死锁与零野指针**：所有异步挂起与唤醒采用原子三态机原子交接；所有插件生命周期操作具备事务性与 RAII 自动回滚保障；
* **绝对的多实例隔离**：严禁任何可变全局变量与静态状态，所有状态严格依附于实例上下文堆对象；
* **极简优雅的开发体验（DX）**：提供声明式 `ToolSchemaBuilder`、强类型 `ArgReader`、对称的 `PluginBase` 与 `ClientPluginBase`、以及一键式导出宏，让插件代码量下降 50%~70%；
* **高性能跨边界通信**：消除跨边界多余的堆分配与二次内存拷贝，调度层引入不变上下文预取机制，避免跨线程高频 `ioCallSync` 挂起。

---

## 2. 全局 API v1 规范与接口表版本完全重置

### 2.1 ABI 基础契约规范
跨边界通信严格遵循以下底层准则，不依赖任何特定编译器与 C++ 标准库运行时：
1. **统一 8 字节结构体对齐**：头文件统一包裹 `#pragma pack(push, 8)` 与 `#pragma pack(pop)`；
2. **显式定长数据类型**：跨边界一律采用 `int32_t`、`int64_t`、`uint64_t`、`char*` 与 `AgentxxPluginStringView`，杜绝 `int`、`long`、`size_t` 等平台差异类型；
3. **显式调用约定**：跨边界导出符号、接口表函数指针与回调统一标注 `AGENTXX_PLUGIN_CALL`（Windows: `__stdcall`，x64 Linux/Unix 为空）；
4. **结构体传递与返回值规范**：禁止按值跨边界传递非 POD 聚合体，结构体入参统一为指针（`const Struct*`）；结构体返回值统一为函数出参（`Struct* out`）并以 `int32_t` 作为状态码返回（`0` 表示成功）；
5. **冻结核心 vtable（极简正交基）**：
   - 跨 CRT 堆内存管理操作：`alloc(uint64_t)` / `free(void*)`；
   - COM 风格能力查询：`query_interface(host, iid)`。

### 2.2 全局版本与接口表重置清单（全部为 Version 1）

所有接口表的 `int32_t version` 字段以及全局宏统一重置为 1：

```c
#define AGENTXX_PLUGIN_API_VERSION        1  /* agent 侧全局 */
#define AGENTXX_CLIENT_PLUGIN_API_VERSION 1  /* client 侧全局 */
```

#### 2.2.1 Agent 侧标准接口表（16 张，统一 Version 1）
| 序号 | 接口标识宏 (IID) | 版本宏定义 | 职责描述 |
| :--- | :--- | :--- | :--- |
| 1 | `AGENTXX_PLUGIN_IFACE_AGENT_TOOLS` | `1` | 工具注册与插件间异步工具互调 |
| 2 | `AGENTXX_PLUGIN_IFACE_AGENT_HOOKS` | `1` | 7 处中间件钩子点拦截与观察 |
| 3 | `AGENTXX_PLUGIN_IFACE_AGENT_EVENTS` | `1` | 插件事件总线发布与订阅 |
| 4 | `AGENTXX_PLUGIN_IFACE_AGENT_CAPABILITIES` | `1` | 动态扩展能力方法声明与互调 |
| 5 | `AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER` | `1` | 定时器 sleep、线程池 offload、IO 投递 |
| 6 | `AGENTXX_PLUGIN_IFACE_AGENT_SESSION` | `1` | 会话级 share_store、消息提示 tip |
| 7 | `AGENTXX_PLUGIN_IFACE_AGENT_PLUGINS` | `1` | 插件自省与插件间信息互查 |
| 8 | `AGENTXX_PLUGIN_IFACE_AGENT_CONFIG` | `1` | 宿主配置、插件专属参数、工作目录、语言读写 |
| 9 | `AGENTXX_PLUGIN_IFACE_AGENT_MODEL` | `1` | 宿主主模型及推理配置读取 |
| 10 | `AGENTXX_PLUGIN_IFACE_AGENT_CANCEL` | `1` | 会话轮次取消状态查询 |
| 11 | `AGENTXX_PLUGIN_IFACE_AGENT_PROMPT` | `1` | 宿主系统提示词读写与追加 |
| 12 | `AGENTXX_PLUGIN_IFACE_AGENT_JSON` | `1` | 宿主辅助 JSON 转义与字符串提取 |
| 13 | `AGENTXX_PLUGIN_IFACE_AGENT_LOG` | `1` | 宿主统一日志分级输出 |
| 14 | `AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES` | `1` | 贡献 Skill 目录、Memory 文件与 MCP 服务 |
| 15 | `AGENTXX_PLUGIN_IFACE_AGENT_GRAPH` | `1` | 自定义图节点注册与宿主执行图 JSON 读写 |
| 16 | `AGENTXX_PLUGIN_IFACE_AGENT_TASKS` | `1` | 宿主托管的后台协作任务生命周期与取消 |

#### 2.2.2 Client 侧标准接口表（7 张，统一 Version 1）
| 序号 | 接口标识宏 (IID) | 版本宏定义 | 职责描述 |
| :--- | :--- | :--- | :--- |
| 1 | `AGENTXX_IFACE_CLIENT_UI` | `1` | 状态栏、侧边栏面板、Info 段落、斜杠命令、工具装饰与特化渲染、通用 Action 绑定与 Overlay 弹窗 |
| 2 | `AGENTXX_IFACE_CLIENT_EVENTS` | `1` | 订阅客户端会话、连接、流式增量与插件数据事件 |
| 3 | `AGENTXX_IFACE_CLIENT_SESSION` | `1` | 客户端状态快照、代发用户输入、请求取消 |
| 4 | `AGENTXX_IFACE_CLIENT_WIRE` | `1` | 客户端向 Agent 侧对应插件发送通信数据通道 |
| 5 | `AGENTXX_IFACE_CLIENT_SELF` | `1` | 客户端插件自身信息、配置路径、参数与语言读写 |
| 6 | `AGENTXX_IFACE_CLIENT_JSON` | `1` | 客户端宿主 JSON 辅助工具 |
| 7 | `AGENTXX_IFACE_CLIENT_LOG` | `1` | 客户端宿主统一日志分级输出 |

---

## 3. 并发安全与生命周期彻底修复设计

### 3.1 `CallToolAwaiter` 与 `InvokeCapAwaiter` 零死锁原子三态机
#### 缺陷根因
原实现中先检查 `callbackFired`，随后再置 `suspended = true`。当 `call_tool_async` 异步迅速完成时，回调在主线程置 `suspended` 之前触发，回调误判为“尚未挂起”，只设标志后退出；主线程随后才挂起协程，导致协程永远不会被唤醒。

#### 重构设计方案
采用原子状态流转模型，完全消除时间窗口：
```cpp
enum class AwaiterState : uint32_t {
    INIT = 0,       // 初始态
    CALLING = 1,    // 正在发起跨边界调用
    SUSPENDED = 2,  // 协程已交出控制权并已挂起
    COMPLETED = 3   // 异步回调已触发并就绪
};
```
在 `CallToolState` / `InvokeCapState` 中引入 `std::atomic<AwaiterState> state{AwaiterState::INIT};`：

```cpp
template<typename Promise>
bool await_suspend(std::coroutine_handle<Promise> h) {
    st->coroAddr = h.address();
    st->state.store(AwaiterState::CALLING, std::memory_order_release);

    auto* holder = new std::shared_ptr<CallToolState>(st);
    AgentxxPluginString err{nullptr, 0};
    auto nameSv = PluginStringView::from(st->name.data(), st->name.size());
    auto argsSv = PluginStringView::from(st->argsJson.data(), st->argsJson.size());
    auto tidSv  = PluginStringView::from(st->threadId.data(), st->threadId.size());

    st->opHandle = st->tools->call_tool_async(
        st->host, &nameSv, &argsSv, &tidSv,
        [](void* ud, int32_t cbSt, const AgentxxPluginStringView* pl) {
            auto* hp = static_cast<std::shared_ptr<CallToolState>*>(ud);
            auto s = *hp;
            delete hp;

            s->status = cbSt;
            if (pl && pl->data && pl->size > 0) {
                s->payload.assign(pl->data, static_cast<size_t>(pl->size));
            }

            // 原子尝试将状态从 CALLING 切为 COMPLETED
            auto expected = AwaiterState::CALLING;
            if (s->state.compare_exchange_strong(expected, AwaiterState::COMPLETED,
                                                 std::memory_order_acq_rel)) {
                // 回调先于 await_suspend 结束：直接返回，主线程 await_suspend 将返回 false 立即恢复
                return;
            }

            // 否则说明主线程已成功挂起 (状态为 SUSPENDED)，由回调负责将协程唤醒投递至 IO 线程
            auto handle = std::coroutine_handle<Promise>::from_address(s->coroAddr);
            handle.promise().clear_outstanding();
            
            // 投递 IO 线程 resume...
            s->resumeCoro(handle);
        },
        holder, &err
    );

    if (!st->opHandle) {
        delete holder;
        if (err.data) {
            st->startError.assign(err.data, static_cast<size_t>(err.size));
            PluginString::free(st->host, &err);
        }
        return false; // 启动失败，立即恢复并在 await_resume 抛异常
    }

    // 主线程尝试将状态从 CALLING 切换为 SUSPENDED
    auto expected = AwaiterState::CALLING;
    if (st->state.compare_exchange_strong(expected, AwaiterState::SUSPENDED,
                                          std::memory_order_acq_rel)) {
        // 成功挂起：注册取消回调，并返回 true 真正挂起协程
        h.promise().set_outstanding([st = this->st]() {
            if (st->tools && st->tools->op_cancel && st->opHandle) {
                st->tools->op_cancel(st->opHandle);
            }
        });
        return true;
    }

    // 切换失败，说明回调已在另外线程先一步完成并置为了 COMPLETED！
    // 此时绝对不能返回 true，直接返回 false，协程无缝同步推进，绝无死锁！
    return false;
}
```

---

### 3.2 宿主 Agent 侧插件加载失败的事务性回滚（RAII 回滚）
#### 缺陷根因
`PluginManager::loadNativeAsync` 与 `loadBuiltinAsync` 中，当 `createFn` 返回非 0 失败时，宿主直接 `plugins_.erase` 并 `dlclose`，未清理插件在创建过程中已经向宿主登记的工具、钩子、事件订阅与资源。

#### 重构设计方案
为 `PluginInstance` 引入严密的装配作用域保护，实现失败即全量回滚的原子加载：
```cpp
plugins_[name] = inst;

int rc = -1;
try {
    rc = createFn(&inst->host, &inst->pluginCtx);
} catch (const std::exception& e) {
    XX_LOGE("Plugin `{}` create threw: {}", name, e.what());
    rc = -1;
} catch (...) {
    XX_LOGE("Plugin `{}` create threw unknown exception", name);
    rc = -1;
}

if (rc != 0) {
    XX_LOGE("Plugin `{}` create failed (code={}), performing rollback", name, rc);
    // 1. 彻底拔除一切残留
    detachAll(inst.get());
    eraseMiddleware(inst->middleware.get());
    inst->middleware = nullptr;
    if (auto c = agentContext_.lock()) {
        if (c->resourceApplier) {
            c->resourceApplier->removeAllOwned(inst->name);
        }
    }
    // 2. 从表移除并卸载动态库
    plugins_.erase(name);
    if (inst->dlHandle) {
        NativeLoader::close(inst->dlHandle);
        inst->dlHandle = nullptr;
    }
    co_return nullptr;
}
```

---

### 3.3 异步跨边界调用生命周期屏障（彻底消除 UAF）
在 `libagentxx_execute_javascript` 等插件中，异步能力调用传入裸指针上下文 `ud = ctx.get()`，若插件在回调未就绪时被释放，回调解引用引发野指针访问。
* **设计方案**：
  在 `PluginBase` 中内置 `LifeToken`（`std::shared_ptr<std::atomic<bool>> alive`），所有发起异步 C 回调操作时，通过捕获 `std::weak_ptr<std::atomic<bool>>` 作为安全凭证：
  ```cpp
  auto alive = ctx.lifeToken();
  invoke_capability_async(..., [alive](void* ud, int32_t st, auto* payload) {
      auto token = alive.lock();
      if (!token || !token->load()) {
          return; // 插件实例已被销毁，安全丢弃，绝不解引用 ud
      }
      auto* ctx = static_cast<MyPluginCtx*>(ud);
      // 安全执行业务逻辑...
  });
  ```

---

### 3.4 根除违规全局变量与函数级静态缓存（彻底贯彻多实例三铁律）
1. **`agentxx_system_monitor`**：
   - 彻底移除 `system_monitor_plugin.h` 中的 `inline std::atomic<const PluginLogSink*> g_log_sink;`；
   - 移除 `pluginLog(int level, ...)` 全局函数；
   - 将日志输出收敛至 `CpuGpuMonitor` 实例成员，在创建 `CpuGpuMonitor` 时由 `PluginCtx` 显式注入实例级 `Logger` 引用。
2. **`agentxx_text_selection_monitor`**：
   - 彻底移除 `findCDPPort` 中的 `static int cachedPort` 与 `static steady_clock::time_point cacheTime`；
   - 将 CDP 调试端口探测缓存移动至 `TextSelectionPluginCtx` 实例上下文，并加互斥锁保护。

---

### 3.5 会话取消状态（CancelRegistry）跨轮次自愈与复位机制
#### 缺陷根因
`PluginBase::sessionCancelled` 将查询到的已取消会话 ID 缓存在 `cancelRegistry.cancelledKeys_` 中，跨轮次后该集合从不被清理，导致该会话后续的所有调用被永久短路为已取消。

#### 重构设计方案
1. **区分主动取消与查询缓存**：`cancelledKeys_` 仅用于记录由当前插件实例在执行中主动捕获的 cancel 事件；
2. **挂钩会话轮次开始钩子**：宿主内部触发 `AGENTXX_PLUGIN_HOOK_AGENT_START` 时，向当前会话所有插件派发轮次重置，`PluginBase` 自动执行 `cancelRegistry.clearCancelled(session_id)`，确保新一轮任务具备干净的状态机。

---

## 4. 统一现代化插件开发 SDK (`plugin_kit.h`)

彻底废弃并移除 `plugin_kit.h` 内部旧有的 `registerSyncTool` / `syncToolStart` / `syncJobWork` / `syncJobDone` / `SyncJob` 等遗留代码，建立全新一代统一 SDK。

### 4.1 双端对称上下文基类设计
为 Agent 侧与 Client 侧提供完全对称的一等公民基类支持：

```cpp
namespace agentxx::plugin {

// ================================= Agent 侧上下文基类 =================================
class PluginBase {
public:
    const AgentxxPluginHost* host = nullptr;
    AgentIfaces              iface{};
    Logger                   log;
    CancelRegistry           cancelRegistry;

    virtual ~PluginBase() {
        cancelRegistry.cancelAll();
        stopSpawns();
    }

    void init(const AgentxxPluginHost* h);
    
    // 快捷常用操作
    std::string workDir(std::string_view tid = {}) const;
    std::string argsJson() const;
    std::string configPath() const;
    std::string language() const;
    bool setLanguage(std::string_view lang) const;
    bool sessionCancelled(std::string_view tid) const;
    ToolPromptText toolPrompt(std::string_view tool) const;

    // 声明式 Schema 构建入口
    ToolSchemaBuilder schema(std::string_view toolName) const;

    // 后台任务管理与 Shim 存储
    template<typename T> T* storeShim(std::unique_ptr<T> shim);
    template<typename Fn> void spawn(Fn&& fn);
};

// ================================= Client 侧上下文基类 =================================
class ClientPluginBase {
public:
    const AgentxxPluginHost* host = nullptr;
    ClientIfaces             iface{};
    Logger                   log;
    kit::ActionController    actions;

    virtual ~ClientPluginBase() = default;

    void init(const AgentxxPluginHost* h);

    // 快捷常用操作
    std::string clientState() const;
    std::string argsJson() const;
    std::string configPath() const;
    std::string language() const;
    bool setLanguage(std::string_view lang) const;
    void showToast(std::string_view text, int32_t level = 0) const;
    int32_t sendUserInput(std::string_view sessionId, std::string_view text) const;
    void requestCancel(std::string_view sessionId) const;

    // 快捷 UI 注册
    void registerTemplate(std::string_view tool, std::string_view display, std::string_view key);
    template<typename Fn>
    void registerRenderer(std::string_view tool, Fn&& fn);

    template<typename T> T* storeShim(std::unique_ptr<T> shim);
};

} // namespace agentxx::plugin
```

---

### 4.2 终极正交工具注册三原语
彻底简化工具注册入口，仅提供满足全部场景的三大正交基原语：

```cpp
// 1. 锚定协程工具 (支持跨插件互调 call_tool, co_await sleep, 异步流转)
template<typename Ctx, typename CoroFn>
void tool(Ctx& ctx, std::string_view name, std::string_view depict,
          std::string_view schema, CoroFn&& fn,
          int64_t timeout_ms = 0, int32_t flags = 0);

// 2. 快同步内联工具 (在宿主 IO 线程无挂起直跑，适于获取时间、纯数学计算、简单文本转换)
template<typename Ctx, typename SyncFn>
void fast_tool(Ctx& ctx, std::string_view name, std::string_view depict,
               std::string_view schema, SyncFn&& fn,
               int64_t timeout_ms = 0, int32_t flags = 0);

// 3. 阻塞池工具 (自动在宿主工作线程池执行，提供取消标志与工作目录预取，适合文件IO、子进程)
template<typename Ctx, typename BlockFn>
void blocking_tool(Ctx& ctx, std::string_view name, std::string_view depict,
                   std::string_view schema, BlockFn&& fn,
                   int64_t timeout_ms = 0, int32_t flags = 0);
```

---

### 4.3 声明式链式模式构建器 `ToolSchemaBuilder`
彻底终结手动拼接数十层 JSON 的痛苦，支持链式构建并自动与宿主动态 `toolPrompt` 参数描述合并：

```cpp
namespace agentxx::plugin {

class ToolSchemaBuilder {
public:
    explicit ToolSchemaBuilder(ToolPromptText prompt = {}) : prompt_(std::move(prompt)) {}

    ToolSchemaBuilder& string(std::string_view name, std::string_view desc,
                              bool required = false, std::optional<std::string> defVal = std::nullopt);

    ToolSchemaBuilder& integer(std::string_view name, std::string_view desc,
                               bool required = false, std::optional<int64_t> defVal = std::nullopt);

    ToolSchemaBuilder& number(std::string_view name, std::string_view desc,
                              bool required = false, std::optional<double> defVal = std::nullopt);

    ToolSchemaBuilder& boolean(std::string_view name, std::string_view desc,
                               bool required = false, std::optional<bool> defVal = std::nullopt);

    ToolSchemaBuilder& stringArray(std::string_view name, std::string_view desc,
                                   bool required = false);

    ToolSchemaBuilder& enumString(std::string_view name, std::string_view desc,
                                  std::vector<std::string> options,
                                  bool required = false, std::optional<std::string> defVal = std::nullopt);

    std::string build() const;

private:
    ToolPromptText prompt_;
    neograph::json properties_ = neograph::json::object();
    std::vector<std::string> required_;
};

} // namespace agentxx::plugin
```

---

### 4.4 强类型参数提取器 `ArgReader`
在工具执行体开头提供标准、鲁棒且友好的参数提取与校验：

```cpp
namespace agentxx::plugin {

class ArgReader {
public:
    explicit ArgReader(std::string_view jsonStr) {
        if (!jsonStr.empty()) {
            try {
                root_ = neograph::json::parse(jsonStr);
            } catch (...) {
                hasParseError_ = true;
            }
        } else {
            root_ = neograph::json::object();
        }
    }

    bool hasParseError() const noexcept { return hasParseError_; }

    template<typename T>
    std::optional<T> get(std::string_view key) const;

    template<typename T>
    T value(std::string_view key, const T& fallback) const;

    // 必填参数校验：若缺失则自动记录错误
    template<typename T>
    T require(std::string_view key);

    bool ok() const noexcept { return errors_.empty() && !hasParseError_; }
    std::string errorMessage() const;

    const neograph::json& raw() const noexcept { return root_; }

private:
    neograph::json root_;
    bool hasParseError_ = false;
    std::vector<std::string> errors_;
};

} // namespace agentxx::plugin
```

---

### 4.5 一键式插件导出宏族
消除所有插件重复手写的 `extern "C"` 样板代码，强制绑定异常安全守卫与 RAII 析构：

```cpp
#define AGENTXX_PLUGIN_AGENT_EXPORT(CtxType, Name, Ver, Desc, SetupBody)           \
    extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo*                      \
    agentxx_plugin_agent_get_info(void) {                                          \
        static const AgentxxPluginInfo info{                                       \
            AGENTXX_PLUGIN_API_VERSION, 0,                                         \
            agentxx::plugin::PluginStringView::fromCstr(Name),                     \
            agentxx::plugin::PluginStringView::fromCstr(Ver),                      \
            agentxx::plugin::PluginStringView::fromCstr(Desc),                     \
        };                                                                         \
        return &info;                                                              \
    }                                                                              \
    extern "C" AGENTXX_PLUGIN_EXPORT int32_t                                       \
    agentxx_plugin_agent_create(const AgentxxPluginHost* host, void** plugin_ctx) {\
        if (!host || !plugin_ctx) return -1;                                       \
        auto ctx = std::make_unique<CtxType>();                                    \
        ctx->init(host);                                                           \
        try {                                                                      \
            auto setup = SetupBody;                                                \
            int32_t rc = setup(*ctx);                                              \
            if (rc != 0) return rc;                                                \
        } catch (const std::exception& e) {                                        \
            ctx->log.error(fmt::format("Plugin setup exception: {}", e.what()));   \
            return -1;                                                             \
        } catch (...) {                                                            \
            ctx->log.error("Plugin setup unknown exception");                      \
            return -1;                                                             \
        }                                                                          \
        *plugin_ctx = ctx.release();                                               \
        return 0;                                                                  \
    }                                                                              \
    extern "C" AGENTXX_PLUGIN_EXPORT void                                          \
    agentxx_plugin_agent_destroy(void* plugin_ctx) {                               \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                             \
        if (ctx) delete ctx;                                                       \
    }
```

---

## 5. 性能优化与底层架构精简

### 5.1 消除 `blocking_tool` 结果跨边界的双重内存分配
在原 `blocking_tool` 中，工作线程返回 `std::string` 后，执行了一次 `host->alloc` 拷贝，随后在 `done` 回调中传给 `notify.done`，宿主 `OpCore::onDone` 又拷贝了一次，最后再 `host->free`。

**零多余分配优化**：
`Job` 直接内联持有 `std::string resultPayload`。工作线程将执行结果直接写入 `job->resultPayload`；
在 `done` 回调触发时（在宿主 IO 线程）：
```cpp
AgentxxPluginStringView payload = PluginStringView::from(job->resultPayload.data(), job->resultPayload.size());
job->notify.done(job->notify.host_ud, status, &payload);
delete job; // 一次性释放 job 及 payload
```
彻底去除中途调用 `hostMemoryAlloc` 和 `hostMemoryFree` 的开销，直接减少 50% 的跨边界内存操作。

### 5.2 调度派发时的不变上下文预取机制
在 `blocking_tool` 发起调用的瞬间（此时正处于宿主 IO 线程），`PluginBase` 在构造 `Job` 时直接预取好：
- `workDirCache`（当前会话工作目录）；
- `argsJsonCache`（插件参数）；
- `cancelFlag` 初始状态。
并将这些预取内容随 `Job` 传递给工作线程。工作线程执行整个业务逻辑过程中无需通过 `ioCallSync` 再次阻塞等待主 IO 线程，消除线程往返通信开销。

---

## 6. 现有官方插件重构改造示范

### 6.1 `agentxx_filesystem` 重构后代码对比
#### 重构前痛点
- 手写 JSON Schema 占据 500+ 行；
- 手写 `neograph::json::parse` 与各种 string/int 转换重复 6 次；
- Client 端手写 `struct ClientCtx` 与样板代码。

#### 重构后实现（极致精简与清晰）
```cpp
#include "agentxx/plugin/api/plugin_kit.h"
#include "filesystem_impl.h"

using namespace agentxx::plugin;

struct FsPluginCtx : public PluginBase {};

AGENTXX_PLUGIN_AGENT_EXPORT(FsPluginCtx, "agentxx_filesystem", "1.0.0",
    "File system tools: list, read, write, edit, glob, grep",
    [](FsPluginCtx& ctx) -> int32_t {
        // 1. List
        auto listSchema = ctx.schema("agentxx_filesystem_list")
            .string("path", "Path to file or directory", /*required=*/true)
            .boolean("recursive", "List subdirectories recursively", false, false)
            .integer("limit", "Max entries", false, 100)
            .number("timeout", "Execution timeout in seconds", false, 60.0)
            .build();

        blocking_tool(ctx, "agentxx_filesystem_list", "List files and directories...", listSchema,
            [](FsPluginCtx& c, std::string_view args_json, std::string_view tid, std::string_view workDir, volatile int* cancel) {
                ArgReader args(args_json);
                auto path = args.require<std::string>("path");
                if (!args.ok()) return args.errorMessage();
                return fileListExecute(args.raw(), std::string(workDir), [&] {
                    return (cancel && *cancel != 0) || c.cancelRegistry.isCancelled(tid);
                });
            });

        // 2. Read
        auto readSchema = ctx.schema("agentxx_filesystem_read")
            .string("path", "Path to text file", /*required=*/true)
            .integer("line_offset", "Lines to skip", false, 0)
            .integer("line_limit", "Max lines to read")
            .build();

        blocking_tool(ctx, "agentxx_filesystem_read", "Read text file...", readSchema,
            [](FsPluginCtx&, std::string_view args_json, std::string_view, std::string_view workDir) {
                ArgReader args(args_json);
                auto path = args.require<std::string>("path");
                if (!args.ok()) return args.errorMessage();
                return fileReadExecute(args.raw(), std::string(workDir));
            });

        // write, edit, glob, grep 同样以声明式 10 行内完成注册...
        return 0;
    }
);

// Client 侧同样以 ClientPluginBase 声明式注入
struct FsClientCtx : public ClientPluginBase {};

AGENTXX_PLUGIN_CLIENT_EXPORT(FsClientCtx, "agentxx_filesystem", "1.0.0",
    "Filesystem specialized renderer",
    [](FsClientCtx& ctx) -> int32_t {
        ctx.registerTemplate("agentxx_filesystem_list", "List", "path");
        ctx.registerTemplate("agentxx_filesystem_write", "Write", "path");
        ctx.registerRenderer("agentxx_filesystem_read", [](const ToolRenderInput& in, ToolRenderOutput& out) {
            ArgReader args(in.argsJson);
            out.displayName = "Read";
            out.summary = fmt::format(" · [{}] {}", args.value("line_limit", -1), args.value("path", ""));
        });
        return 0;
    }
);
```
**重构收益**：源码行数由原先的 930+ 行缩减至 250 行以内，可读性大幅提升，内存与调用契约 100% 自动化安全保障。

---

### 6.2 `agentxx_system_monitor` 纯净多实例隔离改造
彻底剥离全局静态变量，将采样逻辑与上下文解耦：
```cpp
struct SysMonCtx : public PluginBase {
    std::atomic<bool> usageEnabled{true};
    CpuGpuMonitor monitor; // 实例私有持有
};

AGENTXX_PLUGIN_AGENT_EXPORT(SysMonCtx, "agentxx_system_monitor", "1.0.0",
    "System resource monitor: CPU/memory/GPU usage tool",
    [](SysMonCtx& ctx) -> int32_t {
        // 注册快速查询工具
        blocking_tool(ctx, "agentxx_get_system_core_info", "Get CPU/memory/GPU usage", "{}",
            [](SysMonCtx& c, std::string_view) {
                auto usage = c.monitor.querySync();
                return formatUsageText(usage);
            });

        // 周期性后台监控任务：利用优雅的 spawn + sleep + offload
        ctx.spawn([](SysMonCtx& c, OpCtl ctl) -> Task<void> {
            while (!ctl.cancelled()) {
                if (c.usageEnabled.load(std::memory_order_relaxed)) {
                    auto usage = co_await offload(c, [&](volatile int*) {
                        return c.monitor.querySync();
                    });
                    if (ctl.cancelled()) break;
                    
                    std::string json = usageToJson(usage);
                    auto topicSv = PluginStringView::fromCstr("agentxx_system_monitor.usage");
                    auto jsonSv = PluginStringView::from(json.data(), json.size());
                    c.iface.events->publish(c.host, &topicSv, &jsonSv);
                }
                co_await sleep(c, 5000);
            }
        });
        return 0;
    }
);
```

---

## 7. 实施路线图与验收检查清单

### 7.1 实施阶段规划
1. **阶段 1：核心 API 与基础库清理（版本重置为 1）**
   - 更新 `plugin_api.h` 与 `client_plugin_api.h`：将所有 `AGENTXX_*_VERSION` 宏统一设为 1；
   - 彻底删除 `plugin_kit.h` 内部旧有的 `registerSyncTool`、`SyncJob`、`registerInlineTool` 废弃代码；
   - 在 `plugin_kit.h` 中实现 `ToolSchemaBuilder`、`ArgReader`、`ClientPluginBase`、`AGENTXX_PLUGIN_*_EXPORT` 宏。
2. **阶段 2：并发死锁与生命周期修复**
   - 改造 `CallToolAwaiter` 与 `InvokeCapAwaiter`：实现原子三态状态机；
   - 改造 `plugin_manager_lifecycle.cpp`：为 `loadNativeAsync` 与 `loadBuiltinAsync` 补齐 `create` 失败时的事务性回滚；
   - 修复 `PluginBase::sessionCancelled` 的缓存状态清理逻辑。
3. **阶段 3：官方插件重置升级**
   - 全面迁移 17 个官方插件至全新 SDK：
     - `agentxx_filesystem`
     - `agentxx_execute_command`
     - `agentxx_planning`
     - `agentxx_codegraph`
     - `agentxx_string`
     - `agentxx_math`
     - `agentxx_system`
     - `agentxx_system_monitor`（消除全局 `g_log_sink`）
     - `agentxx_text_selection_monitor`（消除静态变量）
     - 其余示例与内置插件。
4. **阶段 4：编译构建与全套回归测试**
   - 运行全量单元测试与压力测试；
   - 验证多实例并存并发加载/卸载场景。

### 7.2 验收检查清单 (Checklist)
- [ ] 所有接口表 `version` 与全局 API 版本恒等于 1；
- [ ] 插件动态库导出符号仅包含 `agentxx_plugin_*` 入口，第三方依赖符号全部隐藏；
- [ ] 并发调用 `co_await call_tool(...)` 10,000 次无任何死锁与挂死；
- [ ] 故意使插件 `create` 返回 -1，宿主工具表与事件总线无任何残留，再次调用不崩溃；
- [ ] 同一进程内创建 10 个独立的 `AgentContext` 并加载 `agentxx_system_monitor`，无全局状态覆盖与野指针；
- [ ] 全量官方插件代码量显著缩减，无冗余手写 JSON 样板。
