# 多根 agent + 会话通道 + 统一运行时（接口设计）

> 关联: [index.md](index.md) · [01-analysis.md](01-analysis.md) · [03-event-driven.md](03-event-driven.md) · [04-prompt-and-impl.md](04-prompt-and-impl.md)

## 1. 概念模型

```
        ┌──────────────────────────── AgentHost（进程级容器，多根）────────────────────────────┐
        │  ioCtx / threadPool / hostBus / 子代理预算（全局共享）                                │
        └───────────────┬───────────────────────────────────────────────────────────────────┘
                        │
        ┌───────────────┴────────────────┬────────────────────────────────┐
        ▼                                ▼                                ▼
  Root("root")                    Root("ext:qq:1")                 Root("ext:ops:1")
  内置 agent（coding）            插件根：QQ 群助手                插件根：运维助手
  宿主自己的配置                   自定义 systemPrompt              自定义 systemPrompt
                                  自定义挂载/工具白名单             自定义挂载/工具白名单
                                  模型/语言/工作目录                模型/语言/工作目录
        │                                │                                │
        │ 内置会话                        │ open_channel ×N                │ open_channel ×N
        ▼                                ▼                                ▼
   Channel(内置)              Channel(qq_group_1) …           Channel(ops_group_9) …
                              每个群一个 sessionId              互不影响
```

| 概念 | 职责 | 不负责 |
|---|---|---|
| **Root** | 一个挂载到 `AgentHost` 的根 agent 节点，持有独立 `AgentConfig`（提示词、插件挂载、工具、模型） | 不持有会话 |
| **Channel** | 一个 `sessionId` 的会话通道：输入、事件、中断、状态 | 不创建 agent（绑在某个 Root 上） |
| **Trigger** | 把事件/定时转成某个 Channel 的输入 | 不解析业务语义 |
| **Runtime**（表实现） | Root / Channel / Trigger 的记账、路由、生命周期 | 不实现会话逻辑 |

三条边界：

- **多根用来隔离"配置"**：提示词、插件挂载、工具白名单、模型、开关都是 `AgentConfig` 级，只能靠独立 agent 表达。
- **会话用来隔离"对话"**：上下文、模型选择、工作目录、权限边界、share_store 都是 `sessionId` 级。
- **同配置的多个群共用一个 Root**（默认形态），不要每群一个 Root（成本见 §11）。

## 2. 多根在 AgentHost 里的语义

`AgentHost` 现在是单根（`rootAgent_` + 单个 `subagentServerId_`）。改造点：

| 位置 | 现状 | 改成 |
|---|---|---|
| `rootAgent_` | 单个 | `primaryRoot_`（内置根）+ `roots_` 列表 |
| `attachRoot` | 单根，重复调用只警告 | 内部转调 `mountRoot(agent, {agentId="root", primary=true})`，语义不变 |
| `pluginHost_` | `AgentHost` 持有唯一 `PluginManager`（上一篇） | 不动；`mountRoot` 构造根时**强注入** `ctx->pluginHost = pluginHost_`（禁止根自带 `createSelfHosted` 分支） |
| `subagentServerId_` | 单个 | 每根一个（各自 `ctx->bus` 上注册一次） |
| `spawnOneTask` | 用 `rootAgent_` 取父配置/工具/记账 | 用"发起请求那根"的 `AgentContext`（注册时闭包捕获） |
| `publishProgress` | 借 `rootAgent_->taskScope` | 走 `hostBus_` 直接 `co_spawn` |
| `AgentRegistry` | 根固定 `"root"` | 挂载根用 `"ext:<owner>:<n>"`；重复 id 拒绝（现在是覆盖） |
| `destroyAgent` | 主要面向子代理 | 挂载根先 `unmountRoot`（摘服务/节点）再释放；内置根保持现状 |

新增接口：

```cpp
struct MountSpec {
    std::string agentId;                  // 空 = 生成 "ext:<owner>:<n>"
    std::string owner;                    // 归属（插件名；按 owner 批量卸载）
    bool        primary = false;          // 内置根
    bool        registerSubagentService = true;

    // ---- 该根的插件挂载（来自创建者，默认两个都不设 = 只有内置工具）----
    // 这两项最终落到该根 AgentConfig::pluginMountPolicy / pluginMounts，
    // 由 mountAgentAsync 在 init 里执行（上一篇 §4.2）
    agentxx::agent::PluginMountPolicy              pluginMountPolicy = PluginMountPolicy::None;
    std::vector<agentxx::agent::PluginMountConfig> pluginMounts;
    bool                                           includeCreator = false;  // 创建者插件是否挂到自己根上
};

std::string mountRoot(std::shared_ptr<BaseAgent> agent, MountSpec spec); // 返回 agentId
bool        unmountRoot(std::string_view agentId);                       // 内置根返回 false
size_t      unmountRootsByOwner(std::string_view owner);
std::shared_ptr<BaseAgent> findRoot(std::string_view agentId) const;
std::vector<std::shared_ptr<MountedRoot>> roots() const;                 // 快照
```

预算与归属：

| 项 | 语义 |
|---|---|
| `maxDepth` / `maxConcurrentSubagents` | **全局共享**（进程级预算），不按根拆分 |
| `maxRoots`（新增，默认 8） | 进程内根数上限，超出 `mountRoot` 返回空 |
| 用量记账 | 每根的子代理用量记到**该根**的会话（闭包捕获的 ctx 决定），不串到内置根 |
| `hostBus` / `threadPool` | 全部共享 |
| 生命周期 | 挂载根由 owner 记账；插件禁用/卸载时 `unmountRootsByOwner` |

## 3. `AgentSessionRuntime`（统一启动壳）

CLI 本地直连、FFI、插件通道共用的唯一启动壳（01 §2 说明三处目前各写一遍的 7 个步骤）。

```cpp
// agent/lib/include/agentxx/agent/session_runtime.h
namespace agentxx::agent {

class AgentSessionRuntime : public std::enable_shared_from_this<AgentSessionRuntime> {
public:

    struct Config {
        std::string sessionId;
        std::chrono::milliseconds interruptTimeout{0};
        size_t deltaBufferCap = 0;          // 0 = 不保留重放（FFI/插件）
        size_t initialSyncTailCount = 0;    // TUI 用 100
        bool   forwardPluginEvents = false;
        bool   autoRequestComponents = true;
        /// 宿主（空 = 由 runtime 新建一个只服务本 agent 的宿主）
        /// - CLI / FFI：留空或传入已建好的宿主
        /// - 插件通道：必填（复用 mount_root 时所在的宿主）
        std::shared_ptr<AgentHost> host;
        /// agent 已初始化（插件挂载根：mount_root 时已 init）；false = runtime 负责 init
        bool   skipInit = false;
        std::string language = "en";
        uint64_t lastSeq = 0;
        std::string tailHash;
    };

    /// `agent` 须已构造；`cfg.skipInit=true` 表示 agent 已初始化（插件挂载根用）
    static std::shared_ptr<AgentSessionRuntime> create(
        std::shared_ptr<BaseAgent>   agent,
        std::shared_ptr<AgentIOBase> clientIO,
        Config                       cfg
    );

    /// 装配 + 启动（幂等）
    /// - skipInit=false: co_spawn( host->loadPluginsAsync()
    ///                            → agent->init()（内部挂载插件）→ host->attachRoot(agent)
    ///                            → requestAppendComponentInfo → onServerReady
    ///                            → serverIO->run() )
    /// - skipInit=true : 直接 co_spawn( requestAppendComponentInfo → onServerReady
    ///                                  → serverIO->run() )
    void start();

    /// 发起关闭（不等待）：onRuntimeStopping → close transport → serverIO->stop()
    void stop();

    bool running() const;

    void sendInput(WireUserInput input);   // 自动补 sessionId
    void cancel();
    void selectModel(std::string modelName);
    void requestContextMessages();

    std::shared_ptr<SessionServerAgentIO> serverIO() const;
    std::shared_ptr<AgentIOBase>          clientIO() const;
};

} // namespace agentxx::agent
```

`AgentIOBase` 新增空实现钩子：

```cpp
/// 运行壳即将关闭（client 端点可在此失败挂起的中断、冲刷缓冲）
virtual void onRuntimeStopping() {}
```

三处用法：

| 使用方 | agent | skipInit | host | client 端点 |
|---|---|---|---|---|
| CLI 本地直连 | 自建 | false | 留空（runtime 新建） | `TUIClientAgentIO` / `StdIOClientAgentIO` |
| FFI | 自建 | false | 留空或传入 | `FfiClientAgentIO`（独立线程） |
| 插件通道 | 挂载根（`mount_root` 时已 init） | true | **必填**（宿主的那一份） | `PluginSessionAgentIO`（继承 `EmbeddedClientAgentIO`） |

## 4. C ABI：`agentxx.agent.runtime`

### 4.1 错误码

```c
#define AGENTXX_PLUGIN_RUNTIME_OK              0
#define AGENTXX_PLUGIN_RUNTIME_ERR_ARG        (-1)   ///< 参数非法
#define AGENTXX_PLUGIN_RUNTIME_ERR_STATE      (-2)   ///< 状态错误（未就绪 / 关闭中）
#define AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND  (-3)   ///< root/channel/trigger id 不存在
#define AGENTXX_PLUGIN_RUNTIME_ERR_BUDGET     (-4)   ///< 超预算（根数 / 通道数 / 触发器数）
#define AGENTXX_PLUGIN_RUNTIME_ERR_CONFLICT   (-5)   ///< (存储根, sessionId) 已被占用
#define AGENTXX_PLUGIN_RUNTIME_ERR_UNSUPPORTED (-6)  ///< 宿主未提供运行时能力
#define AGENTXX_PLUGIN_RUNTIME_ERR_FAILED     (-7)   ///< 执行失败（配置非法 / init 失败）
```

### 4.2 通道事件

```c
typedef enum AgentxxPluginChannelEventKind {
    AGENTXX_PLUGIN_CHANNEL_EVT_READY = 0,       ///< 通道可用
    AGENTXX_PLUGIN_CHANNEL_EVT_TURN_START,      ///< 一轮开始（text = 用户输入回显）
    AGENTXX_PLUGIN_CHANNEL_EVT_DELTA_TEXT,      ///< 正文增量（text）
    AGENTXX_PLUGIN_CHANNEL_EVT_DELTA_THINKING,  ///< 思考增量（text）
    AGENTXX_PLUGIN_CHANNEL_EVT_TOOL_START,      ///< 工具开始
    AGENTXX_PLUGIN_CHANNEL_EVT_TOOL_END,        ///< 工具结束
    AGENTXX_PLUGIN_CHANNEL_EVT_GEN_STATUS,      ///< 生成阶段（waiting/tool_call/done）
    AGENTXX_PLUGIN_CHANNEL_EVT_TURN_END,        ///< 一轮结束（text = 最终回答）
    AGENTXX_PLUGIN_CHANNEL_EVT_CONTEXT_STATS,   ///< 上下文统计与累计用量
    AGENTXX_PLUGIN_CHANNEL_EVT_INPUT_ACK,       ///< 输入受理回执
    AGENTXX_PLUGIN_CHANNEL_EVT_QUEUE_STATE,     ///< 队列状态
    AGENTXX_PLUGIN_CHANNEL_EVT_INTERRUPT,       ///< HIL 询问（interrupt_id + payload）
    AGENTXX_PLUGIN_CHANNEL_EVT_INTERRUPT_EXPIRED, ///< 询问已过期
    AGENTXX_PLUGIN_CHANNEL_EVT_TIP,             ///< 提示（错误/警告文本）
    AGENTXX_PLUGIN_CHANNEL_EVT_ERROR,           ///< 运行时错误（payload.code/message）
    AGENTXX_PLUGIN_CHANNEL_EVT_CLOSED,          ///< 通道已关闭（回调的最后一条）
    AGENTXX_PLUGIN_CHANNEL_EVT_COUNT
} AgentxxPluginChannelEventKind;

typedef struct AgentxxPluginChannelEvent {
    uint32_t struct_size;   ///< sizeof(AgentxxPluginChannelEvent)；0 = 基线布局
    int32_t  kind;          ///< AgentxxPluginChannelEventKind
    int64_t  channel_id;
    int64_t  interrupt_id;  ///< 仅 INTERRUPT / INTERRUPT_EXPIRED 有效
    PluginxxStringView text;         ///< 增量 / 回显 / 最终回答 / 提示
    PluginxxStringView payload_json; ///< 结构化载荷（§5）
} AgentxxPluginChannelEvent;

/// 通道事件回调（【该根所属 agent 的 IO 线程】，必须快速返回）
typedef void(PLUGINXX_CALL* AgentxxPluginChannelEventFn)(
    void*                            user_data,
    const AgentxxPluginChannelEvent* event
);
```

### 4.3 规格结构

```c
/// 挂载根 agent（插件自己的 agent，带自己的配置）
typedef struct AgentxxPluginMountRootSpec {
    uint32_t struct_size;
    /// 展示名（也用于生成 root id："qq" → "ext:qq:1"）
    PluginxxStringView name;
    /// AgentConfig 覆盖 JSON（字段与规则见 §6）。空 = 只用派生默认值
    PluginxxStringView config_json;
    /// 该根的**插件挂载策略** JSON（与 yaml `plugin.mount` 同构）：
    /// {"mode":"none|all|listed","list":["agentxx_filesystem",...],"args":{"插件名":{...}}}
    /// - 空串 = "none"：根只有内置工具（share_store / subagent / git_worktree）
    /// - 要文件/命令工具就必须给 listed（或 all）
    PluginxxStringView mount_plugins;
    /// 0 = 创建者插件**不**挂到该根（默认，切断递归）；
    /// 1 = 挂（想让根也能调本插件的工具时，配合 mount_plugins 使用）
    int32_t            include_creator;
    int32_t            _reserved;
} AgentxxPluginMountRootSpec;

/// 打开会话通道（绑定某个根）
typedef struct AgentxxPluginChannelSpec {
    uint32_t struct_size;
    int32_t  _reserved;
    PluginxxStringView root_id;        ///< 必需（mount_root 返回的 id）
    PluginxxStringView session_id;     ///< 空 = 框架生成；非空时 **(存储根, sessionId)** 进程内必须唯一
    PluginxxStringView title;          ///< 会话标题（可空）
    PluginxxStringView model;          ///< 初始模型（可空）
    int32_t            max_queue;      ///< 输入队列上限；0 = 默认 100
    int32_t            _reserved2;
    int64_t            interrupt_timeout_ms; ///< 0 = 不限；>0 = 超时按取消应答
    AgentxxPluginChannelEventFn on_event;
    void*                       user_data;
} AgentxxPluginChannelSpec;

#define AGENTXX_PLUGIN_TRIGGER_POLICY_QUEUE        0
#define AGENTXX_PLUGIN_TRIGGER_POLICY_COALESCE     1
#define AGENTXX_PLUGIN_TRIGGER_POLICY_DROP_IF_BUSY 2

typedef struct AgentxxPluginTriggerSpec {
    uint32_t struct_size;
    int32_t  policy;
    int32_t  debounce_ms;       ///< COALESCE 合并窗口（默认 300）
    int32_t  max_queue;         ///< 合并后待投递上限（0 = 默认 20）
    PluginxxStringView topic;   ///< 主题（相对名自动加 "plugin." 前缀）
    int64_t            channel_id;
    PluginxxStringView delivery; ///< next-turn（默认）/ next-step / inject / collect
} AgentxxPluginTriggerSpec;

typedef struct AgentxxPluginTimerSpec {
    uint32_t struct_size;
    int32_t  policy;
    int64_t  channel_id;
    int64_t  first_delay_ms;
    int64_t  interval_ms;       ///< 0 = 一次性
    PluginxxStringView input_json;
} AgentxxPluginTimerSpec;
```

### 4.4 接口表

```c
#define AGENTXX_PLUGIN_IFACE_AGENT_RUNTIME             "agentxx.agent.runtime"
#define AGENTXX_PLUGIN_IFACE_AGENT_RUNTIME_VERSION     1
#define AGENTXX_PLUGIN_IFACE_AGENT_RUNTIME_MIN_VERSION 1
#define AGENTXX_PLUGIN_IFACE_AGENT_RUNTIME_BASELINE_SIZE sizeof(AgentxxPluginRuntimeIface)

typedef struct AgentxxPluginRuntimeIface {
    int32_t  version;
    uint32_t struct_size;

    /* ---- 根 agent ---- */
    /// 挂载一个带自己配置的根（异步 init，返回即拿到 id）
    /// - 只能在 `start`（或其它宿主级时机）调用；**在 `mount` 回调里调用返回 ERR_STATE**
    /// - 配置非法返回 ERR_ARG（日志给字段）；超 maxRoots 返回 ERR_BUDGET；
    ///   宿主没有插件宿主（未 attach）返回 ERR_UNSUPPORTED
    /// - 新根由宿主强注入 `pluginHost`，按 `mount_plugins` 策略挂载已加载的插件
    /// - init 完成前对该根 open_channel 会排队，就绪后自动继续
    /// - out_root_id: 框架分配的 id（= 该根的 agentId；host->alloc）
    int32_t(PLUGINXX_CALL* mount_root)(
        const PluginxxHost*               host,
        const AgentxxPluginMountRootSpec* spec,
        PluginxxString*                   out_root_id
    );
    /// 卸载根（其下通道先全部关闭，不等待收尾）
    /// - 内置根、其他实例的根、已卸载的 id 都返回 ERR_NOT_FOUND
    int32_t(PLUGINXX_CALL* unmount_root)(
        const PluginxxHost*       host,
        const PluginxxStringView* root_id
    );

    /* ---- 会话通道 ---- */
    int32_t(PLUGINXX_CALL* open_channel)(
        const PluginxxHost*             host,
        const AgentxxPluginChannelSpec* spec,
        int64_t*                        out_channel_id
    );
    int32_t(PLUGINXX_CALL* close_channel)(const PluginxxHost* host, int64_t channel_id);

    /* ---- 输入与控制 ---- */
    int32_t(PLUGINXX_CALL* send_input)(
        const PluginxxHost*       host,
        int64_t                   channel_id,
        const PluginxxStringView* input_json
    );
    int32_t(PLUGINXX_CALL* cancel)(const PluginxxHost* host, int64_t channel_id);
    int32_t(PLUGINXX_CALL* select_model)(
        const PluginxxHost*       host,
        int64_t                   channel_id,
        const PluginxxStringView* model_name
    );
    int32_t(PLUGINXX_CALL* respond_interrupt)(
        const PluginxxHost*       host,
        int64_t                   channel_id,
        int64_t                   interrupt_id,
        const PluginxxStringView* values_json
    );

    /* ---- 查询 ---- */
    int32_t(PLUGINXX_CALL* get_channel_state)(
        const PluginxxHost* host,
        int64_t             channel_id,
        PluginxxString*     out_json
    );
    int32_t(PLUGINXX_CALL* get_context_messages)(
        const PluginxxHost* host,
        int64_t             channel_id,
        PluginxxString*     out_json
    );
    /// [{"rootId","name","state","configSummary":{...},"channels":[...]}, ...]
    int32_t(PLUGINXX_CALL* list_runtime)(const PluginxxHost* host, PluginxxString* out_json);

    /* ---- 事件驱动 ---- */
    int32_t(PLUGINXX_CALL* register_trigger)(
        const PluginxxHost*             host,
        const AgentxxPluginTriggerSpec* spec,
        int64_t*                        out_trigger_id
    );
    int32_t(PLUGINXX_CALL* unregister_trigger)(const PluginxxHost* host, int64_t trigger_id);
    int32_t(PLUGINXX_CALL* emit_event)(
        const PluginxxHost*       host,
        const PluginxxStringView* topic,
        const PluginxxStringView* payload_json
    );

    /* ---- 定时触发 ---- */
    int32_t(PLUGINXX_CALL* register_timer)(
        const PluginxxHost*           host,
        const AgentxxPluginTimerSpec* spec,
        int64_t*                      out_timer_id
    );
    int32_t(PLUGINXX_CALL* unregister_timer)(const PluginxxHost* host, int64_t timer_id);
} AgentxxPluginRuntimeIface;
```

16 个成员，全部只操作"本实例创建的"对象。

**与上一篇 `PluginxxScopeRef` 的关系**：这张表是**宿主级**接口，用 `root_id` / `channel_id` 这类
句柄寻址，所以不带 `agent` 引用参数。`root_id` 与该根的 `agentId` 是同一个值，需要 agent 级操作时
（比如查它挂了哪些插件、或给它的 `mount` 里注册的工具传引用）直接当 agentId 用。

## 5. 事件载荷

| kind | text | payload_json |
|---|---|---|
| `READY` | — | `{"rootId","sessionId","model"}` |
| `TURN_START` | 用户输入回显 | `{"msgId","model","attachments":[{"type","displayName","pathOrUrl"}]}` |
| `DELTA_TEXT` / `DELTA_THINKING` | token | `{}` |
| `TOOL_START` | — | `{"toolName","toolCallId","arguments"}` |
| `TOOL_END` | — | `{"toolName","toolCallId","result","hasError"}` |
| `GEN_STATUS` | — | `{"phase":"waiting"|"tool_call"|"done","toolName"}` |
| `TURN_END` | **最终回答** | `{"hasError","errorMessage","interrupted","startTimeMs","durationMs","tps","fullText"}` |
| `CONTEXT_STATS` | — | `{"contextTokens","maxContextTokens","tps","usage":{...}}` |
| `INPUT_ACK` | — | `{"requestId","status","reason","detail","itemId","delivery"}` |
| `QUEUE_STATE` | — | `{"state","items":[{"id","text","createdAtMs"}]}` |
| `INTERRUPT` | — | `{"id","node","value","argJson"}` |
| `INTERRUPT_EXPIRED` | — | `{"id"}` |
| `TIP` | 提示文本 | `{"level":"info"|"warning"|"error"}` |
| `ERROR` | 错误文本 | `{"code","message"}` |
| `CLOSED` | — | `{"reason":"closed"|"root_unmounted"|"error"}` |

`TURN_END.text`：本轮最后一个非空正文段（工具调用之后的那段）；`payload.fullText` 是整轮拼接文本。

## 6. `config_json`：插件的"自定义配置"

这是满足"换系统提示词、换工具"的关键。示例（根挂哪些插件**不在这里**，由 `spec.mount_plugins` 单独给，见 §4.3）：

```json
{
  "agentName": "QQ群助手",
  "model": "gpt-4o",
  "language": "zh-cn",
  "workDir": "D:/work/qq",

  "toolWhitelist": [
    "agentxx_filesystem_read",
    "agentxx_filesystem_glob",
    "agentxx_filesystem_grep"
  ],
  "enableToolFiltering": true,

  "prompt": {
    "systemPrompt": "你在 QQ 群里回答问题。回答简短，不超过三句话；不要暴露工具细节。",
    "append": {
      "qq-rules": "只回答与群主题相关的问题；不确定时说不确定。"
    }
  },

  "inherit": {
    "model": true,
    "language": true,
    "workDir": true,
    "permission": true,
    "prompt": false,
    "mcp": false,
    "skill": false,
    "memory": false
  },

  "enableSessionStore": true,
  "enableSubagent": false,
  "enableSummarization": true
}
```

派生规则：

| 字段 | 默认 | 说明 |
|---|---|---|
| `pluginMount` | **不在 `config_json` 里** | 由 `spec.mount_plugins` 单独给（避免两个来源）；两处都给 → `ERR_ARG`。空 = `none`（只有内置工具）。注意：加载是进程级的，根只能选"挂载哪些"，不能选"加载哪些" |
| `prompt` | 不继承（用 `AgentPrompt` 默认 = coding） | 插件应显式给 `systemPrompt`；`append` 按 key 写入附加段 |
| `toolWhitelist` / `enableToolFiltering` | 不继承 | 只影响这个根 |
| `model` / `availableModels` | 继承（`inherit.model`） | 也可在根上换模型 |
| `language` / `workDir` / `permissionAllowPaths`/`denyPaths` | 继承 | `inherit.*` 关闭则用配置值或默认 |
| `mcpServerUrls` / `skillDirPaths` / `memoryFilePaths` / `ragDocsPaths` | 清空 | `inherit.mcp/skill/memory` 为 true 才拷贝 |
| `agentName` | `"{宿主名}-{name}"` | |
| `enableSessionStore` | true | 会话持久化；`sessionStoreDirectory` 缺省与宿主相同 —— 于是"同存储根 + 同 sessionId"就是同一份会话，必须全局唯一；想用同名会话就给这个根单独的目录 |
| `enableSubagent` | false | 对外应答的根通常不需要 |

**根的插件挂载与递归切断**（约束 C1 / C8）：

新根也是一个 agent，它的工具来自"把已加载的插件挂到它身上"，不是"再加载一遍"
（上一篇之后 `create`/`start` 全程只有一次）。所以：

1. **挂载不是加载**：`mount_plugins` 里的插件只在这个根上跑一次 `mount`（注册工具、应用清单资源），
   不重跑 `start`、不重复建连。QQ 插件在自己的根上被 `mount` 时只注册工具。
2. **默认 `none`**：根只有内置工具（share_store / subagent / git_worktree）。要文件/命令工具
   就在 `mount_plugins` 里列出来。
3. **递归三层防护**：
   - 建根只能在 `start`（进程级时机）；
   - 宿主在驱动任何 `mount` 期间调用 `mount_root` → `ERR_STATE`（`PluginManager::isMounting()`）；
   - `include_creator = 0`（默认）时创建者插件不挂到自己的根上，即使前两道都漏了，根里也不会
     再跑到"建根"那段代码。
4. **漏配会自己暴露**：根挂完可以用 `agentxx.agent.mount.list` 查看实际挂了哪些，模型也会发现
   "没有文件工具"。

插件侧写法（`start` 建根 + 连协议，`mount` 只注册工具）：

```cpp
int32_t start(QqCtx& ctx) {                       // 进程级，只一次
    if (!ctx.hasRuntime()) { return 0; }           // 老主程序：没有 runtime 表
    ctx.rootId = mountRoot(ctx, MountRootOptions{
        .name = "qq",
        .config = personaConfig(),
        .mountPlugins   = R"({"mode":"listed","list":["agentxx_filesystem"]})",
        .includeCreator = true,                    // 让根能调 qq_send_message
    });
    return startQqProtocol(ctx);                   // 连 QQ（进程级）
}

void* mount(QqCtx& ctx, const AgentxxPluginAgentMountSpec* spec, ...) {
    auto* st = new AgentState{AgentRef(*spec)};    // 哪个 agent 的挂载
    tool(ctx, st->ref, "qq_send_message", "...", schema, handler);   // 只注册工具
    return st;                                     // 不建根、不建连
}
```

## 7. 输入 JSON

与 FFI 一致，解析提炼为 `agentxx::agent::parseUserInputJson`：

```json
{
  "text": "……",
  "attachments": [{"type":"image","displayName":"a.png","pathOrUrl":"D:/a.png"}],
  "model": "gpt-4o",
  "delivery": "next-turn"
}
```

`delivery`：`next-turn`（默认）/ `next-step` / `inject` / `collect`。受理结果经 `INPUT_ACK`。

## 8. 线程模型

| 动作 | 执行线程 | 说明 |
|---|---|---|
| 表入口 | 插件任意线程 | 经 `ioCallSyncKeep` 落到宿主 io 线程；调用方在 io 线程时直接执行 |
| 事件回调 | 该根所属 io 线程 | 不得同步阻塞 |
| `mount_root` 的 init | 宿主 io 线程（协程） | 每根一次完整 init |
| `AgentSessionRuntime::start/stop` | 任意线程 | 内部投递，不等待收尾 |

### 8.1 入口不能 `co_await`

`ioCallSyncKeep` 在调用方已是 io 线程时直接执行（`pluginxx/host/abi_util.h`），回调内调用安全。限制是入口必须是同步函数：

| 动作 | 入口内做什么 | 结果怎么拿 |
|---|---|---|
| `mount_root` | 派生配置、构造 `BaseAgent`、分配 id、`co_spawn(init → mountRoot)` | 其通道的 `EVENT_READY` / `EVENT_ERROR` |
| `open_channel` | 占位 sessionId、建 runtime、`co_spawn` 启动 | `EVENT_READY` |
| `close_channel` / `unmount_root` | 标记 Closing、`co_spawn` 关闭 | `EVENT_CLOSED` |
| `send_input` 等 | 校验后 `post` 给端点 | `INPUT_ACK` / 轮次事件 |
| 读操作 | 读原子快照 | 直接返回 |

### 8.2 登记表与 sessionId 独占

入口都在 io 线程执行，`RuntimeState`（本实例的根/通道/触发器）只在 io 线程读写，不需要锁。跨线程只读的是原子快照字段。

这里分两层，两层都要有：

| 层 | 管什么 | 由谁做 |
|---|---|---|
| agent 级 | 两个 agent 用同一存储根（谁开到同一份目录都算冲突） | 上一篇 §7.3：`AgentHost` 在挂载 agent 时比对规范化存储根，相同就拒绝 |
| 会话级 | 同一个存储根下同名 `sessionId` 被两个通道同时驱动 | 本节的 `claimSession`（跨插件实例） |

但 **`(存储根, sessionId)` 的独占是进程级**：两个通道若打开同一个存储根下的同名会话，
`SessionWriterLease` 在进程内可重入、不会报错，两个 `SessionStore` 会互盖数据（约束 C4；
与上一篇 `02` §7.3 是同一条规则）。因此占用表不能按实例建，要挂到进程唯一的 `AgentHost` 上：

```cpp
// agent_host.h（新增）
/// 会话占用注册表（进程级；键 = 规范化存储根 + sessionId）
bool claimSession(std::string_view storeRoot, std::string_view sessionId, std::string_view owner);
void releaseSession(std::string_view storeRoot, std::string_view sessionId, std::string_view owner);
```

- 键必须是 `(存储根, sessionId)`：不同根下的同名会话是**两份**，不该互相拒绝；
- `claimSession` 在 io 线程调用；内部 `std::mutex` + map（跨插件实例）；
- 占用者与释放者不一致时拒绝（防止误释放别人的会话）；
- `AgentHost` 不存在时（插件宿主未 attach）`mount_root` 返回 `ERR_UNSUPPORTED`。

## 9. 时序

### 9.1 挂载根 → 开通道 → 一轮对话

```
插件线程              Runtime(io 线程)                    AgentHost / BaseAgent
   │ mount_root ──────▶ 派生配置、构造 agent、分配 id
   │ ◀──── root_id     │ co_spawn: init() ───────────────▶ 各 init 步骤
   │                   │                                   其中含插件挂载（按 mount_plugins）
   │                   │ init 完成 → mountRoot() ────────▶ 注册节点 + 子代理服务
   │ open_channel ────▶ 建 runtime + 端点、co_spawn
   │ ◀──── channel_id  │
   │ ◀ EVENT_READY ────│
   │ send_input ──────▶ WireUserInput ──────────────────▶ 队列 → runTurnAsync
   │ ◀ DELTA_TEXT ─────│ ◀──────────── WireDelta ─────────
   │ ◀ TURN_END ───────│ ◀──────────── WireTurnResult ────
```

### 9.2 中断

```
工具/权限 ─▶ NodeInterrupt ─▶ AgentRunner ─▶ session->bus request(service.interrupt)
                                                  │（每通道一个 serverIO，注册在自己的会话 bus）
                                        WireInterruptRequest
                                                  ▼
                            client 端点 → EVENT_INTERRUPT（插件）
                                                  │ 插件发到群里等人工答复
                                                  ▼
                            respond_interrupt(channel, id, values)
                                                  ▼
                                      中断结果 → resume
```

### 9.3 关闭

```
unmount_root ─▶ 该根全部 channel: onRuntimeStopping → close transport
             ─▶ serverIO->stop() → 等 run() 退出 → EVENT_CLOSED
             ─▶ host->unmountRoot（摘子代理服务 + 移除节点）
             ─▶ 释放 BaseAgent
```

`close_channel` / `unmount_root` 都不等待；要"确实关完了"就订阅 `EVENT_CLOSED`。

## 10. SDK 便捷层（`plugin_kit.h`）

```cpp
namespace agentxx::plugin {

struct MountRootOptions {
    std::string name;
    Json        config = Json::object();
    /// 该根的插件挂载策略 JSON（同 yaml `plugin.mount`）：
    /// {"mode":"none|all|listed","list":[...],"args":{...}}；空 = none（只有内置工具）
    std::string mountPlugins;
    /// 创建者插件是否也挂到该根（默认 false = 切断递归）
    bool        includeCreator = false;
};
struct ChannelOptions {
    std::string  rootId;
    std::string  sessionId;
    std::string  title;
    std::string  model;
    std::chrono::milliseconds interruptTimeout{0};
    int32_t      maxQueue = 0;
};

enum class ChannelEventKind : int32_t {
    Ready = 0, TurnStart, DeltaText, DeltaThinking, ToolStart, ToolEnd, GenStatus,
    TurnEnd, ContextStats, InputAck, QueueState, Interrupt, InterruptExpired,
    Tip, Error, Closed,
};
struct ChannelEvent {
    ChannelEventKind kind = ChannelEventKind::Ready;
    int64_t          channelId = 0;
    int64_t          interruptId = 0;
    std::string_view text;
    std::string_view payloadJson;
};

enum class TriggerPolicy : int32_t { Queue = 0, Coalesce = 1, DropIfBusy = 2 };
struct TriggerOptions {
    std::string   topic;
    int64_t       channelId = 0;
    TriggerPolicy policy = TriggerPolicy::Queue;
    int32_t       debounceMs = 300;
    int32_t       maxQueue = 0;
    std::string   delivery;
};
struct TimerOptions {
    int64_t       channelId = 0;
    std::string   inputJson;
    int64_t       firstDelayMs = 0;
    int64_t       intervalMs = 0;
    TriggerPolicy policy = TriggerPolicy::DropIfBusy;
};

/// 挂载根：返回空串 = 失败（原因在日志里）
/// PluginBase 新增成员：`bool hasRuntime() const { return iface.runtime != nullptr; }`
template<typename Ctx> inline std::string mountRoot(Ctx& ctx, const MountRootOptions& opt);
template<typename Ctx> inline int32_t unmountRoot(Ctx& ctx, std::string_view rootId);

/// 开通道；onEvent 在该根 io 线程调用，事件只在回调期间有效
template<typename Ctx, typename Fn>
inline int64_t openChannel(Ctx& ctx, const ChannelOptions& opt, Fn&& onEvent);

template<typename Ctx> inline int32_t closeChannel(Ctx& ctx, int64_t channelId);
template<typename Ctx> inline int32_t sendInput(Ctx& ctx, int64_t channelId, std::string_view json);
template<typename Ctx> inline int32_t sendText(Ctx& ctx, int64_t channelId, std::string_view text,
                                               std::string_view delivery = {});
template<typename Ctx> inline int32_t cancelChannel(Ctx& ctx, int64_t channelId);
template<typename Ctx> inline int32_t respondInterrupt(Ctx& ctx, int64_t channelId,
                                                       int64_t interruptId,
                                                       std::string_view valuesJson);
template<typename Ctx> inline std::string channelStateJson(Ctx& ctx, int64_t channelId);
template<typename Ctx> inline std::string listRuntimeJson(Ctx& ctx);

template<typename Ctx> inline int64_t addTrigger(Ctx& ctx, const TriggerOptions&);
template<typename Ctx> inline int64_t addTimer(Ctx& ctx, const TimerOptions&);
template<typename Ctx> inline int32_t removeTrigger(Ctx& ctx, int64_t id);
template<typename Ctx> inline int32_t removeTimer(Ctx& ctx, int64_t id);
template<typename Ctx> inline int32_t emitEvent(Ctx& ctx, std::string_view topic,
                                                std::string_view payloadJson);
}
```

约定：

- Shim 放堆上，`user_data` 指回去，`closeChannel` / 实例销毁时释放。
- `sendText` 内部组装输入 JSON。
- `iface.runtime == nullptr`（老主程序，没有这张表）时全部返回 `-6`，不抛异常。
  **注意**：它只表示"主程序不提供运行时"，不再用来区分"宿主实例 / 根内实例"——插件全程只有一份实例（上一篇）。

## 11. 资源预算

| 项 | 默认上限 | 说明 |
|---|---|---|
| 进程内根 | 8（含内置根） | `AgentHost::maxRoots`；每根 = 一次完整 init（AgentContext + 工具 + 图 + **按策略的插件挂载**，不含 `dlopen`/`create`/`start`）。默认只挂 1 个 |
| 每根通道 | 64 | 群数量上限（同配置共用一个根） |
| 每实例触发器 | 256 | 含定时器 |
| 通道 delta 重放缓冲 | 0 | `deltaBufferCap=0`；与 transport 队列（`makePair` 默认 4096）不是一回事 |
| 通道输入队列 | 100 | `max_queue` |

成本对照（量级）：

| 形态 | 额外 init 次数 | 适用 |
|---|---|---|
| 1 个根 + N 个通道 | 1 | **推荐**：QQ/微信群共用一个机器人配置 |
| K 个根（按用途分类）+ N 个通道 | K | 客服/运维等不同人设与工具集 |
| N 个根（每群一个） | N | 不推荐：群多时启动与内存线性增长 |

## 12. 与现有表的关系

| 场景 | 用哪张表 |
|---|---|
| 换整个人设 / 工具集 / 插件 | `runtime.mount_root` 的 `config_json`（本表） |
| 同一根下不同群微调提示词 | 后续可选：`agentxx.agent.prompt` 表尾追加 perSession 来源（04 §3） |
| 读通道上下文 | `runtime.get_context_messages(channel)` |
| 读宿主会话上下文 | `agentxx.agent.context.get_messages`（原语义不变） |
| 自定义执行图 | 本期不做；后续在 `MountRootSpec` 追加 `graph_json` |
| 查某个根挂了哪些插件 | `agentxx.agent.mount.list` / `.get`（上一篇） |
| 给根的插件传参数 | `spec.mount_plugins` 的 `args`（与 yaml `plugin.mount[].args` 同义） |
