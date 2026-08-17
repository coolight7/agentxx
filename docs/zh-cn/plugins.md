# Agentxx 插件系统设计方案

> 状态: 一期 (C++ 插件框架) + 二期 (JS 插件) 已实现 (2026-08); 三期 (生态) 待实现
> 关联: [design.md](design.md)
> 目标: 原生 C++ 插件 + JS 插件 (由 C++ 插件承载), 支持热插拔、强自定义

## 目录

- [1. 目标与范围](#1-目标与范围)
- [2. 现状盘点与可行性分析](#2-现状盘点与可行性分析)
- [3. 总体架构](#3-总体架构)
- [4. C++ 插件接口设计 (核心)](#4-c-插件接口设计-核心)
- [5. 热插拔机制](#5-热插拔机制)
- [6. JS 插件实现方案](#6-js-插件实现方案)
- [7. 三期落地路线](#7-三期落地路线)
- [8. 与现有代码的接入点](#8-与现有代码的接入点)
- [9. 安全与权限](#9-安全与权限)
- [10. 示例插件](#10-示例插件)
- [12. client 侧插件系统 (四期)](#12-client-侧插件系统-四期)

---

## 1. 目标与范围

### 1.1 目标

- **能力最强**: 插件可扩展 agent 的 工具 / 中间件 / 事件 / 提示词 / UI 五个面
- **自定义能力强**: 从零代码(声明式)到原生 C++ (全量能力) 的全谱系覆盖
- **热插拔**: 运行中加载/卸载/启停插件, 不重启 agent

### 1.2 范围决策

| 决策 | 结论 | 理由 |
|------|------|------|
| 插件形态 | **C++ 动态库 (一级公民)** | 能力天花板最高, 无翻译层损耗 |
| 脚本语言 | **JS**, 舍弃 Lua | JS 熟悉度高; 由内置 C++ 插件承载, 核心框架形态单一 |
| 语言解释器 | **降格为 C++ 插件** | 语言支持可插拔 (未来 Python 同理) |
| 主图拓扑 | **不支持运行期修改** | checkpoint/恢复依赖节点名, 风险不可控; 自定义流程走子图注入 |
| 插件接口 | **纯 C ABI + vtable** | 跨编译器/STL ABI 稳定, 热插拔安全 |

---

## 2. 现状盘点与可行性分析

### 2.1 已有扩展点 (插件化的天然基础)

| # | 扩展点 | 载体 | 注册方式 | 热插拔现状 |
|---|--------|------|----------|-----------|
| 1 | 工具 Tool | `XXToolBase`/`XXToolWrap` (继承 `neograph::Tool`/`AsyncTool`) | `BaseAgent::createTools()` 虚函数返回 vector; 中间件可附带 `toolcalls` | ❌ 启动时固化 |
| 2 | 中间件 Middleware | `BaseMiddlewareHandle<T>` (7 钩子) | `setupMiddleware()` push 到 `middlewareHandleContext->handles` | ⚠️ 运行时可增 |
| 3 | 节点 Node + 图 | `GraphRegistry.register_type` + `buildGraphDefinition()` | `registerNodes()` 虚函数 | ❌ 主图固定; 子图可独立 |
| 4 | 事件 EventBus | `EventStream<T>` / `RequestResponseStream<Req,Resp>` | `bus.get<T>(topic).subscribe()` | ✅ 运行时可订阅 |
| 5 | 来源注入 | Skill (SKILL.md) / MemoryFile / Planning | Skill/MemoryFile 中间件按目录扫描 | ⚠️ 目录启动时固定 |
| 6 | 远程能力 | MCP Client (HTTP SSE + stdio) / A2A / ACP / DI 容器 | yaml 配置 `mcp_servers` | ⚠️ 启动时连接 |

### 2.2 关键代码事实 (设计依据)

- **工具查找是静态快照**: `toolcall.cpp` `std::find_if(tools_.begin(), tools_.end(), name)` 从 `ToolDispatchNode` 继承的 `tools_` (来自 `NodeContext`, init 时固化) 查找, 找不到回 `[Error] Tool not found`。
- **中间件是活读取**: `wrap_handle.h` `WrapHandleBaseNode::run()` 每次执行都重新读 `handles` 长度并按**下标**遍历 (`for (; i < len; ++i)`), 运行中 `push_back` 不会破坏迭代 (len 已缓存、元素 shared_ptr) → **中间件热增在引擎层安全**。
- **Tool 接口被冻结**: `neograph/tool.h` 注释 "the frozen Tool vtable"; `ContextualAsyncTool` 是外挂扩展; `XXToolBase` 是薄封装 (加 autoSummaryOutput / canDelayLoad / maxRetry)。
- **无锁单线程模型**: Session 可变状态 `assertIoThread()` 强制仅 io 线程读写; 取消/切模型经 Wire 消息投递 → 插件注册/注销**必须 post 到 io 线程串行执行**。
- **现有线程卸载基建**: `blockingPool` + `offloadAsync` / `offloadCancellableAsync` (带 CancelToken watcher) 可直接复用为插件同步回调的异步执行通道。

### 2.3 热插拔可行性矩阵

| 扩展点 | 热加载 | 热卸载 | 难度 | 方案要点 |
|--------|:------:|:------:|------|----------|
| 注册新工具 | ✅ | ✅ | 低 | `ToolRegistry` 动态查表 |
| 卸载/禁用工具 | ✅ | ✅ | 低 | Registry 摘引用; 在途工具靠 shared_ptr 引用计数跑完; CancelToken 可中止 |
| 事件订阅/发布 | ✅ | ✅ | 低 | EventBus 本身可运行期订阅, RAII 退订 |
| 中间件 (新类) | ✅ (轮次边界) | ✅ (禁用标记) | 中 | handles 按索引活读取已安全; 卸载用 disabled 位而非 erase |
| 提示词/Skill/Memory | ✅ | ✅ | 中 | onAgentcallStart 每轮动态组装; 目录可重扫 |
| 自定义节点类型 | ⚠️ 新会话/子图 | ❌ 运行中 | 高 | GraphRegistry 新类型仅影响之后编译的图 |
| 主图拓扑 | ❌ | ❌ | 极高 | 不支持 |
| 模型 Provider | ⚠️ 启动时 | ❌ 运行中 | 中 | 需加动态注册接口 |
| TUI/CLI 侧 UI 扩展 | ✅ | ✅ | 中 | client 侧独立 PluginManager |
| 协议打包 (MCP/A2A) | ⚠️ 启动时 | ⚠️ | 中 | 复用现有 MCP client 装配 |

**结论**: 工具 / 事件 / 中间件 / 提示词 / UI 五类可做到**真正运行期热插拔**, 覆盖 90% 用户扩展诉求; 节点/主图属编译期形态, 通过子图注入获得自定义流程。

---

## 3. 总体架构

```
libagentxx 核心 (只认识 C++ 插件)
┌────────────────────────────────────────────────────────────┐
│ PluginManager (io 线程串行, 无锁模型不变)                   │
│   ├─ NativeLoader: dlopen/LoadLibrary → C ABI 入口          │
│   ├─ 生命周期: Discover→Load→Enable→(在途计数)→Disable→Unload │
│   └─ CapabilityRegistry: 能力注册表 (插件互查/分级委派)      │
│              ▲ 持有                                        │
│   ToolRegistry │ MiddlewareHookRegistry │ EventHooks        │
│   (改 ToolcallWrapNode 查表)                                │
│              ▲                                            │
│   PluginHost vtable (C ABI)  ←── 每个 C++ 插件一个宿主句柄   │
└──────────────┬─────────────────────────────────────────────┘
               │
      ┌────────┴───────────┐
      │ 内置 C++ 插件(随发行包) │
      │ agentxx_javascript_engine    │  ← 内嵌 QuickJS
      │  能力: interpreter.js │     把自己注册为 JS 解释器
      └────────┬─────────────┘
               │ 委派加载 (PluginManager 查能力表)
               ▼
         *.js 插件 (plugin.yaml + plugin.js)
```

**核心规则**:
- libagentxx 核心**不依赖** QuickJS/任何脚本引擎; 核心只认 C++ 动态库插件。
- JS (及未来任何语言) 支持 = 一个实现了 `interpreter.<lang>` 能力的 C++ 插件。
- 插件与宿主一切数据交换走 C ABI (JSON 字符串 / 函数指针 / 不透明句柄)。

### 3.1 组件清单 (新增, 均在 `agent/lib/src/plugins/`)

| 组件 | 说明 |
|------|------|
| `plugin_api.h` | 纯 C ABI 契约头 (唯一跨版本稳定接口) |
| `PluginManifest` | 插件目录 `plugin.yaml` 清单解析 (name/version/entry/depends/...) |
| `PluginManager` | 生命周期管理; 所有写操作 post 到 io 线程串行 |
| `NativePluginLoader` | 平台封装的 dlopen / LoadLibrary |
| `PluginHost` | C++ 侧宿主实现, vtable 分派到强类型 API |
| `ToolRegistry` | 全局工具查表 (动态注册/摘除) |
| `CapabilityRegistry` | 能力注册表 (如 `interpreter.js`) |

---

## 4. C++ 插件接口设计 (核心)

### 4.1 为什么纯 C 稳定 ABI

C++ 跨动态库边界传对象有三大致命问题: 编译器不匹配 (GCC/Clang/MSVC 的 STL/异常 ABI 不同)、`libstdc++`/`libc++` 混用、RTTI/typeid 不一致。故插件接口定义为**纯 C 头 + vtable**, 插件可用任意编译器/任意 C++ 标准编译。

### 4.2 plugin_api.h (契约草案)

```c
// agentxx/plugin/plugin_api.h —— 纯 C, 无任何 C++ 类型
#ifdef __cplusplus
extern "C" {
#endif

#define AGENTXX_PLUGIN_API_VERSION 6

/// 跨边界字符串参数统一形态: 只读借用 (data+size, 不要求 NUL 结尾,
/// 生命周期仅覆盖本次调用; 宿主不得保存引用)
typedef struct AgentxxPluginStringView {
    const char* data;
    size_t      size;
} AgentxxPluginStringView;
// 便捷构造: agentxx_plugin_sv(data, size) / agentxx_plugin_sv_cstr(s) / AGENTXX_SV(s)

/// 插件元信息 (dlsym 入口后由宿主获取)
typedef struct AgentxxPluginInfo {
    int api_version;   ///< 必须 == AGENTXX_PLUGIN_API_VERSION
    AgentxxPluginStringView name;          ///< 唯一标识, 如 "agentxx_javascript_engine"
    AgentxxPluginStringView version;
    AgentxxPluginStringView description;
} AgentxxPluginInfo;

/// 工具定义 (LLM 侧, JSON Schema 以字符串视图传)
typedef struct AgentxxToolSpec {
    AgentxxPluginStringView name;            ///< 须全局唯一
    AgentxxPluginStringView description;
    AgentxxPluginStringView parameters_json; ///< JSON Schema (json 字符串)
    /// 同步执行回调; 返回 host->alloc 字符串(宿主 free), error_out 同理
    char* (*execute)(void* user_data, AgentxxPluginStringView args_json,
                     AgentxxPluginStringView thread_id, AgentxxPluginStringView tool_call_id,
                     char** error_out);
    void* user_data;
    long  default_timeout_ms;    ///< 0 = 宿主默认
} AgentxxToolSpec;

/// 中间件钩子点 (与现有 7 钩子一一对应)
typedef enum AgentxxHookPoint {
    AGENTXX_HOOK_AGENT_START, AGENTXX_HOOK_AGENT_END,
    AGENTXX_HOOK_MODEL_START, AGENTXX_HOOK_MODEL_RUN, AGENTXX_HOOK_MODEL_END,
    AGENTXX_HOOK_TOOL_START,  AGENTXX_HOOK_TOOL_END
} AgentxxHookPoint;

/// 钩子回调 (NodeInput 序列化为 json 字符串视图; 允许通过 out_json 修改)
typedef int (*AgentxxHookFn)(void* user_data, AgentxxHookPoint point,
                             AgentxxPluginStringView node_input_json,
                             char** out_json, char** error_out);

/// 事件订阅句柄
typedef struct AgentxxSubscription AgentxxSubscription;

/// 宿主函数表 —— 插件获得的全部能力 (字符串参数均为 AgentxxPluginStringView;
/// 返回的 char* 均为 host->alloc 分配, 调用方须 host->free)
typedef struct AgentxxHostVtable {
    // ---- 注册/注销 (热插拔四个面) ----
    int  (*register_tool)(const struct AgentxxHost*, const AgentxxToolSpec*);
    int  (*unregister_tool)(const struct AgentxxHost*, AgentxxPluginStringView name);
    int  (*register_hook)(const struct AgentxxHost*, AgentxxHookPoint,
                          AgentxxHookFn, void* user_data);
    int  (*unregister_hook)(const struct AgentxxHost*, AgentxxHookPoint, AgentxxHookFn);
    AgentxxSubscription* (*subscribe)(const struct AgentxxHost*, AgentxxPluginStringView topic,
                                      void (*handler)(AgentxxPluginStringView event_json, void* ud),
                                      void* ud);
    void (*unsubscribe)(AgentxxSubscription*);
    int  (*register_capability)(const struct AgentxxHost*, AgentxxPluginStringView capability_name);
    // ---- 会话/上下文访问 (线程安全, 内部 post 到 io 线程) ----
    char* (*call_tool)(const struct AgentxxHost*, AgentxxPluginStringView name,
                       AgentxxPluginStringView args_json, AgentxxPluginStringView thread_id,
                       char** error_out);
    char* (*get_share_store)(const struct AgentxxHost*, AgentxxPluginStringView thread_id, long long id);
    void  (*emit_message_tip)(const struct AgentxxHost*, AgentxxPluginStringView thread_id,
                              AgentxxPluginStringView text);
    void  (*log)(const struct AgentxxHost*, int level, AgentxxPluginStringView msg);
} AgentxxHostVtable;

/// dlopen 后 dlsym 的入口符号
typedef int  (*AgentxxPluginEntry)(const struct AgentxxHost* host, void** plugin_ctx);
typedef void (*AgentxxPluginUnload)(void* plugin_ctx);
#ifdef __cplusplus
}
#endif
```

> 注: 最终以 `plugin_api.h` 实现为准; vtable 后续按需扩展, 递增 `api_version` 兼容。

### 4.3 宿主侧适配 (vtable → 现有强类型世界)

- `register_tool` → 包装成 `XXToolBase` 子类, `execute_async` 内部用 `offloadCancellableAsync` (+ CancelToken watcher) 调用 C 回调 → 返回字符串; 注册进 `ToolRegistry`。**同步 C 回调放线程池而不是阻塞 io_context**, 取消/超时语义天然接入 `execTool(*it, args, in.ctx.cancel_token)` 链路。
- `register_hook` → 包装成 `MiddlewareWrapHandle<BaseMiddlewareState>` 七个 std::function, **push 进现有 `handles` vector** → 栈式执行、错误重抛、per-thread state 全部复用, 不改 `wrap_handle.h` 引擎逻辑。
- `subscribe` → 直接转发 `EventBus`。
- `call_tool` → 经 ToolRegistry 查表调用; 是插件互调 + JS 插件"工具调用其他工具"的唯一通道。

### 4.4 SDK 形态

- `agentxx/plugin/plugin_api.h`: 纯 C ABI 头 (**唯一跨版本契约**)
- `agentxx/plugin/plugin_sdk.hpp`: header-only C++ 便捷包装 (RAII 类 `XXPluginTool` 等), **非必须**, 插件可以直接按 C 头写
- 插件编译**无需链接 libagentxx**

---

## 5. 热插拔机制

### 5.1 C++ 特有坑与对策

| 坑 | 现象 | 对策 |
|----|------|------|
| 在途调用悬垂 | dlclose 后插件代码段被卸载, 执行中 tool 崩溃 | 每插件 `std::atomic<size_t> inflight`; 卸载顺序: 摘除注册 → 等 inflight==0 (io 线程协程等待) → 调 `unload` 回调 → 再 dlclose |
| static 全局析构 / 回调持有插件代码 | dlclose 触发静态析构顺序错乱; 已注册回调指向已卸载代码 | SDK 约定: 插件不得在 static 对象里注册回调; `unload` 回调内必须反注册自身; 宿主二次校验注册表无残留才 dlclose |
| 编译器/STL ABI 不匹配 | 跨库传 `std::string`/异常崩溃 | C ABI 层零 C++ 类型跨边界; 异常不外泄 (error_out 字符串化) |
| 中间件运行中卸载 | 迭代器/下标失效 | disabled 标记位机制 (C++ 插件注册的 hook 同样走 handles 的 disabled 位) |
| Windows/Linux/Android 差异 | dlopen 三平台 API 不同 | `NativePluginLoader` 平台封装 (`dlopen/dlsym/dlclose` ↔ `LoadLibrary/GetProcAddress/FreeLibrary`); Android 5.0+ 可用, 插件须按目标 ABI 编译 |
| 插件状态与会话隔离 | 插件跨 thread 共享状态; 执行线程(线程池)与 io 线程混杂 | 约定: 共享只读, 会话态用宿主 API (shareStore 线程安全 per-thread KV); 受信任才允许插件自管状态 |

### 5.2 中间件热插拔具体机制

- **加载**: 任意轮次边界经 agent 线程 `handles.push_back`, 下一节点执行即生效 (by-index 活读取已安全)
- **卸载/禁用**: 不 `erase` (防执行中下标错位/悬垂), 置 `disabled` 位 — `WrapHandleBaseNode` 遍历时跳过; 卸载请求到达时若该中间件正在执行, 延迟到其 end 回调返回后再摘除
- **State**: 按 thread_id 存于中间件实例内部 `states` map, 卸载即释放 (`loadStateItem/saveStateItem` 骨架可继承)

### 5.3 PluginManager 线程模型

- 全局唯一; 挂在 `AgentContext` 上
- 所有插件注册/注销/启停操作 `co_spawn` 到 agent io_context 线程串行执行 (复用 AsyncMutex / concurrent_channel)
- 满足 `assertIoThread` 无锁模型; wire 消息 (远程管理) 同样投递到该线程

---

## 6. JS 插件实现方案

### 6.1 引擎选型: QuickJS

| 引擎 | 体积 | 依赖 | Android | 许可 | 适合度 |
|------|------|------|---------|------|--------|
| **QuickJS** | ~1-2MB | 无 | ✅ | MIT | ✅ 首选 |
| Duktape | ~500KB | 无 | ✅ | MIT | 仅 ES5.1, 能力偏弱 |
| V8/JSC | 数十 MB | 极重 | ❌ | 混合 | 不适合嵌入分发 |
| Node.js embed | 巨大 | 系统 Node | ⚠️ | MIT | 太重, 违背"零额外动态库依赖" |

QuickJS 与项目"仅依赖基本系统库、跨 Linux/Win/Android"目标完全吻合 (同 hyperscan/curl 一样走 `agent/third_party/` 编译基建)。

### 6.2 实现层次 (`agentxx_javascript_engine`, 内置 C++ 插件)

```
agentxx_javascript_engine (C++ 插件, 用 plugin_api.h 编写)
├── 初始化: 注册能力 "interpreter.js" 到 CapabilityRegistry
├── JS 运行时: QuickJS 上下文 + 宿主桥接对象 (agentxx.*)
├── JS 插件加载: PluginManager 委派 load_js(path)
│    ├── 读 plugin.yaml (type: js, entry: plugin.js)
│    ├── 执行 plugin.js → 得到 plugin 对象 (tools 数组, hooks 对象)
│    ├── 逐个经 host->register_tool 注册 (execute 桥接为 JS 函数调用)
│    └── hooks.* 经 register_hook 注册
└── 执行桥:
     ├── host→js: args_json 传入 → JS 函数 → 结果 JSON 字符串返回
     ├── js→host: 注入全局 agentxx 对象 (callTool/getShareStore/log/...,
     │             桥到 host vtable)
     └── async 支持: QuickJS Job 队列 (JS_ExecutePendingJob 驱动 Promise)
```

### 6.3 JS 插件示例 (用户视角)

```yaml
# plugins/weather/plugin.yaml
type: js
name: weather
entry: plugin.js
```

```js
// plugins/weather/plugin.js
agentxx.registerTool({
  name: "weather_query",
  description: "查询城市天气",
  parameters: { city: { type: "string", required: true, description: "城市名" } },
  execute: async (args, ctx) => {
    const page = await agentxx.callTool("agentxx_web_fetch",
                                        { url: `https://wttr.in/${args.city}` }, ctx.thread_id);
    return { summary: page.slice(0, 500) };
  }
});
agentxx.onToolEnd((ctx) => agentxx.emitMessageTip(ctx.thread_id, "天气查询完成"));
```

### 6.4 沙箱与安全

- 默认无 IO / 无 `require` / 无 `process`; 仅显式注入的 `agentxx.*` 桥接白名单
- 资源上限: QuickJS 内存/栈限制 + 宿主侧执行超时 (复用 `offloadCancellableAsync` 超时通道)
- 插件声明的权限仍过 `PermissionMiddleware`

---

## 11. 一期实现状态 (2026-08)

> 本节记录一期实际落地的代码与对上述设计的偏差, 后续实现以此为准。

### 11.1 已落地

| 组件 | 文件 | 说明 |
|------|------|------|
| C ABI 契约 | `agent/lib/include/agentxx/plugin/plugin_api.h` | 纯 C 头 (API v6); vtable 含 `alloc/free/strdup` (跨 CRT 堆边界唯一分配通道), 插件返回字符串必须经 `host->alloc` 系分配; **字符串参数/字段一律 `AgentxxPluginStringView` (data+size 只读借用, 不要求 NUL 结尾), 便捷构造 `AGENTXX_SV(s)` / `agentxx_plugin_sv(data,size)` / `agentxx_plugin_sv_cstr(s)`** |
| 工具注册表 | `plugin/tool_registry.h/.cpp` | 动态插件工具表 (shared_ptr 持有); 与静态工具名集合冲突检测; `appendDefinitions` 供 LLM 侧 schema 组装 |
| 插件管理器 | `plugin/plugin_manager.h/.cpp` | 生命周期 load/enable/disable/unload + shutdownAll; `NativeLoader` 平台封装 (dlopen↔LoadLibraryW); `CapabilityRegistry` |
| 插件工具适配 | `PluginTool` | C 回调经 `offloadCancellableAsync` (CancelToken watcher) 卸载到线程池, 超时经 `asyncWithTimeout`; inflight 计数保活代码段 |
| 钩子适配 | `PluginMiddlewareHandle` | 7 钩子 → C 回调转发 (io 线程同步, 快速返回约定); 注册即 push 进 handles 栈 |
| 事件桥 | 订阅经 `EventBus<std::string>` (topic 自动加 `plugin.` 前缀), 发布异步投递 |
| 热插拔 | 工具摘除即时生效; 中间件 `disabled` 位 (WrapHandleBaseNode 遍历跳过) + 轮末 `flushPendingCleanup` 摘除; 卸载等 inflight==0 + 轮次结束后彻底摘除并 dlclose |
| LLM 侧可见性 | `ModelCallWrapNode::build_params` 追加插件工具定义 (本设计原稿遗漏的关键点, 见 11.3) |
| 执行侧查找 | `ToolcallWrapNode::baseRun` 静态列表未命中 → `ToolRegistry` 回退 |
| Agent 装配 | `BaseAgent::init` 创建 toolRegistry/pluginManager 并注入 AgentContext, 装配静态工具名, 加载配置插件; `runConversationTurnAsync` 轮次边界登记 |
| 示例插件 | `agent/plugins/example_plugin/` | 2 工具 (echo + call_tool 互调) + agent_start 钩子 + 事件订阅 + 能力声明 |
| 集成测试 | `agent/test/core/test_plugins.*` | 37 项: 加载/工具执行/互调/钩子/事件回环/禁用启用/卸载/冲突/列表 (模块名 `plugins`) |

### 11.2 与设计原稿的偏差 (实现为准)

1. **跨 CRT 堆边界**: vtable 增加 `alloc/free/strdup`; `AgentxxToolSpec::execute` 返回的字符串与 `error_out` 均须经宿主分配 (`AGENTXX_STRDUP` 宏)。示例插件用 `g_host->vtable->strdup`, 不得用 `strdup`/`malloc`。
2. **字符串参数统一字符串视图 (v6)**: 所有跨边界"字符串参数/字段" (`AgentxxPluginInfo`/`AgentxxToolSpec` 字段、execute/hook/event 回调参数、vtable 的 name/topic/json 等参数) 从 `const char*` 改为 `AgentxxPluginStringView` (data+size, 只读借用, 不要求 NUL 结尾, 生命周期仅覆盖本次调用); 仅"宿主分配"的返回值与 `error_out` 保持 `char*` (host->alloc)。插件侧构造: 字面量用 `AGENTXX_SV("...")`, 运行时字符串用 `agentxx_plugin_sv(str.data(), str.size())`。宿主侧 `PluginTool` 构造时拷贝字符串字段 (name/description/parameters_json 指向插件侧视图, 不依赖插件内存存活)。
3. **工具冲突检测**: `ToolRegistry::setStaticToolNames` 由 init 在 `own_tools` 之前收集内置工具名 (注意: own_tools 会 move 空 tools vector, 收集必须在其之前), 插件工具与内置/MCP 工具同名注册失败。
4. **订阅回调签名**: `subscribe(topic, handler, ud)` 载荷为 JSON 字符串; 宿主侧经 `EventBus::get<std::string>` 桥接, topic 自动加 `plugin.` 前缀。
5. **call_tool 语义**: 仅可调用本插件注册的工具 (不暴露宿主内置工具); 在调用方线程**同步执行** —— 工具回调(线程池)内安全, io 线程内调用会阻塞 io 线程 (JS 二期提供异步桥)。
6. **钩子回调签名**: `fn(user_data, point, node_input_json, out_json, error_out)`; `node_input_json` 为节点输入摘要 (thread_id/point/messages_count/has_tool_calls); out_json 一期恒 NULL。
7. **卸载彻底性**: `unloadAsync` 等 inflight==0 **且**进行中轮次结束后立即 `eraseMiddleware` (断开 中间件↔实例 shared_ptr 循环引用), 并从 pendingCleanup 移除; disable 仍走轮末摘除。`AgentContext::~AgentContext` 先调 `pluginManager->shutdownAll()`。
8. **插件名**: 未导出 `get_info` 时按库文件名推断 (libfoo.so → foo)。
9. **中间件 disabled 位**: 新增于 `BaseMiddlewareHandleInterface` (普通中间件恒 false); `WrapHandleBaseNode` start/end 循环均跳过 disabled 项, 保证配对。

### 11.3 设计原稿遗漏、实现中补上的关键点

- **LLM 请求侧工具 schema 静态性**: 原稿只改造了 `toolcall.cpp` 执行侧查表, 但 `ModelCallWrapNode::build_params` 每轮从静态 `tools_` 组装工具定义发给 LLM —— 若不追加插件工具, 模型永远看不到新工具。一期在 build_params 中经 `ToolRegistry::appendDefinitions` 追加 (热注册后下一轮 modelcall 即对模型可见)。
- **执行中工具悬垂**: 原稿靠插件 inflight 计数, 实现中 `ToolRegistry::find` 返回 shared_ptr 保活 (与 execTool 的裸指针路径并存, 插件工具经 shared_ptr 传入), 双保险。
- **注册时序**: dlopen 在阻塞线程池, 但 entry 的注册动作必须回到 io 线程 (无锁模型); `loadNativeAsync` 在 io 线程协程内完成 dlopen (卸载到线程池) + entry 同步调用。

### 11.4 二期实现状态 (2026-08)

> 二期 (JS 插件支持) 已实现, 以下为实际落地内容。

#### 11.4.1 已落地

| 组件 | 位置 | 说明 |
|------|------|------|
| QuickJS 引擎 | `agent/third_party/quickjs/` (git submodule, quickjs-ng) | 核心 4 源文件 (quickjs.c/libregexp.c/libunicode.c/dtoa.c) 静态库 `libqjs.a`, 经 `quickjs_repo` ExternalProject 构建 install 到 `AGENTXX_INSTALL_DIR` |
| 构建开关 | `AGENTXX_ENABLE_PLUGIN_JS` (默认 ON) / `AGENTXX_BUILD_PLUGINS` (默认 ON) | 顶层 option; `AGENTXX_BUILD_PLUGINS` 控制 `agentxx_plugins_repo` (插件整体) 构建, `AGENTXX_ENABLE_PLUGIN_JS` 控制 quickjs_repo 及 JS 插件 (javascript_engine/example_js) 构建 |
| 插件编译 | `agent/plugins/CMakeLists.txt` + 各插件子目录 CMakeLists.txt | 独立于 test 编译: 每个插件子目录自带 CMakeLists.txt, 可独立构建 (`cmake -B build -S agent/plugins -DAGENTXX_INSTALL_DIR=...`), 插件动态库输出到 `AGENTXX_EXEC_INSTALL_PREFIX` |
| JS 引擎插件 | `agent/plugins/javascript_engine/javascript_engine.cpp` → `libagentxx_javascript_engine.so` | 独立动态库 (链接 libqjs.a), 经 `register_capability_ex` 注册 `interpreter.js` 能力 (方法 load/unload) |
| 宿主委派 | `PluginManager::loadPluginAsync` | 插件目录 (plugin.yaml) 分派: `type: native` → dlopen, `type: js` → 创建脚本 PluginInstance (dlHandle 空) 并调引擎 `load_script` (host 为脚本插件自身句柄) |
| 卸载级联 | `unloadAsync` 依赖图 | 卸载引擎插件前先级联卸载 depends 它的脚本插件 (先子后父); 脚本插件卸载时经委派记录通知引擎 `unload_script` (投递式); 见 11.5 |
| codegraph 插件 | `agent/plugins/agentxx_codegraph/` → `libagentxx_codegraph.so` | 统一经 yaml `plugins` 段 path 配置加载 (不区分内置/外置, 无自动加载); 8 个工具 (search/context/callers/callees/impact/status/index/path); 工具提示词默认值从 lib AgentPrompt 剥离迁移 (2026-08), entry 时经 `set_prompt` 写入宿主 toolPrompt (宿主已有条目则跳过, 尊重用户 yaml 覆盖); 索引进度/加载状态经 publish 事件 (topic 约定 `{插件名}.{事件名}`) 通知宿主, 由 SessionServerAgentIO 原样转发 WirePluginData 供 TUI 展示 |
| screen_capture 插件 | `agent/plugins/agentxx_screen_capture/` → `libagentxx_screen_capture.so` | 仅 Windows 可用, 经 yaml `plugins` 段 path 配置加载; 工具 `agentxx_screen_capture` (单帧/全部屏幕/鼠标屏/流式推帧事件 topic `agentxx_screen_capture.frame`) |
| computer_use 插件 | `agent/plugins/agentxx_computer_use/` → `libagentxx_computer_use.so` | 仅 Windows 可用, 经 yaml `plugins` 段 path 配置加载; 工具 `agentxx_ui_control_keyboard_mouse`; 工具提示词默认值 (完整 depict/args) 从 lib AgentPrompt 剥离迁移, entry 时经 `set_prompt` 写入宿主 (宿主已有条目则跳过); plugin.yaml `depends: [agentxx_screen_capture]` (须与 screen_capture 同时配置加载) |
| system_monitor 插件 | `agent/plugins/agentxx_system_monitor/` → `libagentxx_system_monitor.so` | 从 lib `src/expand/get_cpu_gpu_use` 拆分独立 (2026-08): 工具 `agentxx_get_system_core_info` (原内置工具迁移, lib 不再内置; 提示词默认值剥离迁移, entry 时经 `set_prompt` 写入宿主) + 能力 `agentxx.system_usage` (方法 query, 返回使用率 JSON); SessionServerAgentIO 处理 WireGetSystemUsage 时经能力调用采集 (卸载到 blockingPool), 载荷为插件定义 schema 的 JSON 字符串经 WireSystemUsage 原样透传, TUI System 侧边栏按约定字段渲染 —— 采集实现与数据类型完全隔离在插件内, lib wire 层不含任何系统资源 DTO (后续 client 插件化渲染 UI 时, 插件可直接消费同一 JSON) |
| audio_stream 插件 | `agent/plugins/agentxx_audio_stream/` → `libagentxx_audio_stream.so` | 从 lib `src/expand/audio_stream` 拆分独立 (2026-08): Windows WASAPI 系统输出/程序输出/麦克风捕获; 工具 `agentxx_audio_stream` (start/stop/status); 捕获帧经 publish 事件推送 (topic `agentxx_audio_stream.audio`, base64 PCM); 非 Windows 平台 no-op |
| text_selection_monitor 插件 | `agent/plugins/agentxx_text_selection_monitor/` → `libagentxx_text_selection_monitor.so` | 从 lib `src/expand/text_selection_monitor` 拆分独立 (2026-08): Windows 系统级文本选择事件流 (UIAutomation/WinEvent/CDP/剪贴板兜底); 工具 `agentxx_text_selection_monitor` (start/stop/status); 选中文本经 publish 事件推送 (topic `agentxx_text_selection_monitor.selection`); 非 Windows 平台 no-op |
| 宿主配置访问 | vtable `get_config` / `get_plugin_args` / `get_tool_prompt` / `get_prompt` / `set_prompt` | `get_config` 读取通用宿主信息 (dataDir/projectRoot/platform); `get_plugin_args` 整体返回 yaml `plugins` 条目 args (宿主不解析字段语义, 参数含义由插件定义, 如 agentxx_codegraph 的 paths/ignore_paths); `get_tool_prompt` 读取工具提示词 (depict/args), 实现与内置工具一致的动态描述; `get_prompt` 读取完整提示词 (system 3 段 + toolPrompt), `set_prompt` 合并更新 (仅覆盖 JSON 出现的字段) —— 插件卸载时自动回滚加载期间写入的提示词 (恢复加载前状态), 实现"提示词归插件": 插件把默认工具描述写入宿主 toolPrompt, 用户 yaml 覆盖优先 (见 11.7.4) |
| 跨线程投递 | vtable `is_io_thread` / `post_to_io` + `PluginManager::setIoExecutor` | JS 线程/宿主线程池调用的 io 线程约束操作 (注册/钩子/订阅/能力/shareStore/tip) 经 `ioCallSync` post 到 io 线程同步等待; `call_tool` 整体在 io 线程执行 (registry 竞争防护) |
| JS 线程模型 | 专用 JS 线程 + 任务队列 (mutex+cv) | 所有 QuickJS 操作集中单线程; 同步等待方向无环 (见 11.4.2) |
| agentxx 桥 | 全局 `agentxx` 对象 (每脚本插件独立 JSContext) | registerTool/unregisterTool/callTool/getShareStore/emitMessageTip/log/onHook/offHook/subscribe/unsubscribe/publish + 全局 `setTimeout`/`clearTimeout` (定时器桥) |
| Promise 驱动 | `drivePromise` | 循环 `JS_ExecutePendingJob` 直至 settle; 无 job 时执行到期定时器 + 让出 (支持 await/setTimeout 异步); 超时由 interrupt handler 兜底 |
| 沙箱 | 内存 64MB / 栈 512KB / 任务 60s 超时 | 不引入 quickjs-libc (无 os/std 模块); 仅标准 ECMA + agentxx 桥 |
| JS 示例插件 | `agent/plugins/example_js/` (plugin.yaml + plugin.js) | 4 工具 (同步/async Promise/JS 内互调/宿主互调) + 钩子 + 事件订阅 + 顶层异步初始化; 构建时拷贝到 exec/plugins/ |
| 测试 | `test_plugins` 模块 12-19 段 | 引擎加载/脚本插件加载/工具执行/互调/卸载/级联 (67 项全过) |

#### 11.4.2 JS 线程模型要点 (实现为准)

```
宿主线程池 ──postSync──▶ JS 线程 (execute 桥: 驱动 Promise 后返回结果)
io 线程    ──post──────▶ JS 线程 (钩子/事件回调: fire-and-forget, 不等待)
JS 线程    ──vtable────▶ io 线程 (注册/钩子/订阅/shareStore/tip: ioCallSync 同步等待)
JS 线程    ──callTool──▶ 本引擎 JS 工具: 同线程内联 (防自锁)
JS 线程    ──callTool──▶ 宿主插件工具: vtable call_tool (io 线程执行)
```

- 卸载安全: 脚本插件卸载 (deleted) 后已入队任务检查标志跳过; JSContext 由
  JsPluginCtx 析构释放 (在途任务计数归零后); 引擎卸载 delete JsEngine →
  join 等待 JS 线程处理完已入队任务 → JS_FreeRuntime
- 定时器: setTimeout 注册到 timers_ (JS 线程), 任务循环 wait_until +
  drivePromise 空闲时内联触发 (execute 等待期间定时器也能工作)
- 栈检测: JS_NewRuntime 在 io 线程创建, **JS 线程入口必须 JS_UpdateStackTop**
  (否则栈溢出检测基于 io 线程栈指针误判)

#### 11.4.3 实现中踩过的坑 (供参考)

1. **JS_SetProperty* 是 move 语义**: 属性消费传入值, 调用方不得再 Free
   (否则 double free → ASan heap-use-after-free)
2. **JS_PromiseResult 返回新引用**: 直接返回, 不得再 Dup (泄漏 → JS_FreeRuntime
   断言 gc_obj_list 非空 abort)
3. **JS_IsFunction 是双参** (ctx, val), 其他 JS_Is* 单参 (quickjs-ng 与 bellard 差异)
4. **JS_NewCFunction2 是 6 参** (含 JSCFunctionEnum cproto), magic 版函数签名
   `(JSContext*, JSValueConst, int, JSValueConst*, int)` 需强转 JSCFunction*
5. **JS_JSONStringify 的 space 参数是 JSValue** 不是 int
6. **yaml description 值含 `: ` 需引号包裹** (illegal map value)
7. **沙箱无 setTimeout**: 示例插件的 `new Promise(r => setTimeout(r, 10))`
   抛 ReferenceError → promise reject → 工具返回 `{}` (异常对象 JSON 序列化)

### 11.5 统一插件模型 (架构重构, 2026-08)

> **所有插件统一为 C++ 插件**, PluginInstance 无 type 概念 (无 native/js/py 之分):
> 每个插件都有 dlHandle/entry; 脚本能力是插件内部实现的一部分 —— 经 manifest
> 依赖声明 (depends 引擎插件) + 能力调用 (invoke_capability 通用插件间通信)
> 把脚本代码交给 interpreter 引擎执行。宿主不特判任何脚本类型 (未来 py/lua
> 引擎注册 interpreter.py 能力即可被脚本插件依赖, 宿主零改动)。

#### 11.5.1 统一模型

```
PluginInstance (一切插件都是 C++ 插件)
  ├── depends         // 必选依赖 (插件名): 未安装→加载失败; 卸载/禁用时级联
  ├── optionalDepends // 可选依赖: 未安装仅警告; 插件运行时用互查 API 自适应
  ├── dlHandle        // 动态库句柄 (所有插件都有)
  └── 注册残留         // 工具/钩子/订阅/能力 —— 宿主 detachAll 统一清理
```

- **加载**: manifest 解析 → 依赖检查 → dlopen(entry) → entry()。entry 总是指向
  动态库; 脚本插件 = 薄 C++ 壳, 壳在 entry 里经能力调用加载脚本
- **脚本执行 (插件间通信, 宿主不参与)**:
  - 引擎插件 (agentxx_javascript_engine) 注册能力 `interpreter.js` 并附带方法回调
    (`register_capability_ex`): 方法 `load` (执行脚本) / `unload` (释放运行时)
  - 脚本插件的 C++ 壳: `get_own_info` 拿自身 name/path → 推导脚本文件 →
    `invoke_capability("interpreter.js", "load", {name, path})`
  - 引擎执行脚本; 脚本内 `agentxx.registerTool` 等经 **caller_host (壳实例)**
    注册 → 注册残留归属壳插件, 宿主 detachAll 统一清理
  - 壳的 unload 回调里 `invoke_capability("interpreter.js", "unload", {name})`
    通知引擎释放脚本运行时
- **多脚本/混合插件**: 一个壳插件可 invoke 多个语言引擎 (interpreter.js +
  interpreter.py), 天然支持; 也可同时含原生 C++ 实现
- **卸载 (依赖图级联)**: 收集 `depends 含目标` 的插件 → 必选依赖者递归卸载
  (先子后父, 保证引擎 dlclose 前无脚本插件残留) → detachAll → 等 inflight==0
  → unload 回调 (壳通知引擎释放脚本运行时) + dlclose
- **禁用/启用**: 同样依赖图级联传播 (disable 先子后父; enable 先父后子)
- **互查 API** (vtable): `list_plugins` / `get_plugin` / `get_own_info` →
  JSON (name/version/description/path/enabled/tools/capabilities/depends);
  JS 桥成 `agentxx.listPlugins()` / `agentxx.getPlugin(name)`

#### 11.5.2 能力调用 (invoke_capability) 线程模型

```
脚本插件 C++ 壳 (io 线程 entry / 线程池工具回调)
  └─ invoke_capability("interpreter.js", "load", args)
       ├─ 查表: io 线程短临界区 (ioCallSync, 拷贝 Entry)
       └─ 提供者回调: 【调用方线程】执行 (关键!)
            └─ 引擎 loadScriptInEngine: postSync 到 JS 线程
                 ├─ 执行脚本代码
                 └─ 脚本内 agentxx.registerTool → vtable register_tool
                      └─ ioCallSync 回 io 线程注册 (io 线程空闲, 无死锁)
```

- **死锁规避要点**: 提供者回调绝不能在 io 线程执行 —— 引擎的 load 会阻塞
  等待其 JS 线程, 而 JS 线程内脚本注册回调又要回 io 线程; 若回调在 io 线程
  执行则 io↔引擎互等死锁。查表与回调分离: 查表 (短) 在 io 线程, 回调 (长) 在
  调用方线程。

#### 11.5.3 依赖检查规则

| 场景 | 行为 |
|------|------|
| 必选依赖缺失 | 加载失败, 日志提示先加载谁 |
| 可选依赖缺失 | 加载成功, 警告 |
| 依赖环 (A→B→A) | 拒绝 (DFS 访问链检测, 防卸载级联死循环) |
| 卸载被依赖插件 | 必选依赖者级联卸载 (自动, 日志列出); 可选依赖者仅警告 |
| 引擎插件卸载 | 先级联卸载全部 depends 它的脚本插件, 再 dlclose 引擎 (binding 永不悬垂) |
| 加载顺序 | 脚本插件须在引擎之后加载 (本期报错提示; 三期做拓扑排序/自动装配) |

### 11.6 尚未实现 (后续阶段)

> 2026-08 更新: `plugins:` yaml 配置段解析 (config_loader + main.cpp) 与插件依赖拓扑排序 (loadConfiguredPlugins) 已实现 (见 11.7.3); 提示词读写 (get_prompt/set_prompt) 与部分 tool 提示词剥离到插件已实现 (见 11.7.4)

- Wire 协议远程热管理 / TUI 插件管理面板 (三期)
- 插件签名校验 (依赖排序已完成)
- 插件钩子的 out_json 修改能力、permission 联动注册 (插件工具默认走 PermissionMiddleware 的未注册规则兜底)
- client 插件二期: prompt/quick_pick 模态、input_filter、消息装饰、keybind、设置项注册、热重载 (见 12.2 能力位预留)

### 11.7.4 提示词读写与 tool 提示词剥离 (2026-08)

> 目标 (TODOS): 插件支持修改覆盖提示词; 剥离部分 tool 提示词到对应插件 (插件卸载即移除, 宿主不残留)

**宿主 API (v6 末尾追加, 非破坏性; 插件侧判空调用)**:

| vtable 函数 | 语义 |
|------|------|
| `get_prompt` | 返回完整提示词 JSON: `{"systemPrompt","systemPlanningPrompt","systemSkillPrompt","toolPrompt"}` (与 `AgentPrompt::toJson` 一致); 未装配 AgentConfig 返回 NULL |
| `set_prompt` | 合并更新 (与 `AgentPrompt::mergeFromJson` 一致, 仅覆盖 JSON 出现的字段; toolPrompt 条目不存在时插入); 返回 0 成功 |

**回滚机制 (提示词归插件)**:

- `PluginInstance::promptBackup` 记录插件加载期间经 `set_prompt` 写入前的原值
  (system 3 字段 + 每个 toolPrompt 条目, 首次写入某条目前备份一次, 重复写入
  不覆盖备份); 插件卸载时 `detachAll` → `restorePromptBackup` 恢复加载前状态
  (原本存在 → 恢复原值; 原本不存在 → 删除条目)
- disable/enable 不经过回滚 (提示词条目保留, enable 后仍可用)

**剥离范围 (lib `AgentPrompt` 删除, 默认描述迁移到插件)**:

| 插件 | 剥离条目 | 说明 |
|------|----------|------|
| `agentxx_codegraph` | `agentxx_codegraph_search/context/callers/callees/impact/status/index/path` ×8 | 完整 depict + args 默认文本迁移 (`defaultToolPrompt`), entry 时 `ensureToolPromptsInHost` 写入宿主 |
| `agentxx_computer_use` | `agentxx_ui_control_keyboard_mouse` | 完整 depict (含 Actions/Key Names/Examples) 迁移 (`kUiControlDefaultDepict`) |
| `agentxx_system_monitor` | `agentxx_get_system_core_info` | depict 迁移 |

- 写入规则: `get_prompt` 检查宿主已有条目 → 已存在 (用户 yaml 覆盖 / 之前已写入)
  则跳过, 尊重用户配置; 未存在才 `set_prompt` 写入默认值
- 兼容旧宿主: `get_prompt`/`set_prompt` 判空, 缺失时 `registerTool` 回退插件内置
  默认描述, 行为不变
- 参数 schema 说明: codegraph 插件 schemaBuilder 已对全部参数有空值回退
  (宿主无 args 时用插件内置默认), 故宿主 toolPrompt 无条目时工具参数说明仍完整

**示例**: `example_plugin` entry 演示完整链路 (get_prompt 检查 → set_prompt 写入
example_echo 默认描述), 测试第 28 段验证 写入/读取/卸载回滚/用户覆盖优先。

### 11.7.5 内置合并编译 (AGENTXX_ENABLE_PLUGIN_BUILTIN, 2026-08)

> 目标: 可选把启用的插件源文件直接编译进 libagentxx, 运行期无需插件动态库
> (无 dlopen; 适合嵌入式/单文件分发/不便携带 .so/.dll 的场景)。

**构建**:

- 顶层 superbuild 加 `-DAGENTXX_ENABLE_PLUGIN_BUILTIN=ON` 后, 跳过
  `agentxx_plugins_repo` (独立插件动态库构建); 由 `agentxx_lib_repo`
  (libagentxx) 经 `add_subdirectory(../plugins)` 以内置分支收集启用的插件,
  与 lib 源码一并编译
- 各插件子目录 CMakeLists.txt 内置分支 (AGENTXX_ENABLE_PLUGIN_BUILTIN):
  - 源文件编译为**独立 OBJECT 库** (`abp_<插件名>`, per-plugin 编译定义/
    包含路径互不影响), 目标文件经 `$<TARGET_OBJECTS:...>` 合并进
    agentxx_shared/agentxx_static
  - 入口符号经编译定义改名, 避免多插件合并编译冲突: `agentxx_plugin_entry`
    → `agentxx_plugin_builtin_entry_<插件名>` (get_info/unload/client 入口
    同理; 纯编译期 -D, 插件源码零改动)
  - 依赖库登记进 `AGENTXX_BUILTIN_PLUGIN_LINK_LIBS` 由 libagentxx 链接:
    quickjs (JS 引擎) / codegraph_core+tree_sitter 系 (codegraph) /
    pdh / psapi / uiautomationcore (平台库); 其余 (simdjson/fmt/asio/
    d3d11/dxgi/winhttp 等) 为 libagentxx 既有依赖, 无需重复
  - `plugin.yaml` (+ example_js 的 `plugin.js`) 仍拷贝到
    `${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/<插件名>/` (运行期资源)
- 入口清单 `builtin_plugins.cpp` (plugins/CMakeLists.txt 经
  `builtin_plugins.cpp.in` 生成) 汇总改名后的入口符号静态表, 编译进
  libagentxx, 经 `agentxx_get_builtin_plugins()` 暴露 (见
  `lib/include/agentxx/plugin/builtin_plugin.h`); 默认构建由
  `src/plugins/builtin_plugin_registry.cpp` 提供空表实现

**运行期 (配置零改动)**:

- 配置仍写插件目录路径 (plugin.yaml 在), `loadPluginAsync` 目录分支解析
  manifest (depends/拓扑排序/级联卸载与动态模式完全一致), 入口文件缺失时
  自动回退 `loadBuiltinAsync` (内置注册表按名查找, entry 卸载到线程池执行,
  与 dlopen 路径同线程模型)
- **`inst->path` 传 manifest 入口文件路径** (如 `…/plugins/example_js/
  libexample_js.dll`, 与动态加载同形态): 插件侧按"库路径所在目录"推导资源
  (example_js 壳 dirOf 取同目录 plugin.js), 传目录会误推导到上一级
- `inst->dlHandle` 为空, unload 回调经 `builtinUnload` 直接调用
  (shutdownAll/unloadAsync 两路径均支持)
- 也支持显式 `builtin://插件名` 路径 (无资源需求的插件/测试直连用)
- client 侧插件 (agentxx_client_entry) 仍走独立动态库构建 (client 可执行
  程序按需外置), 内置模式不产出 client 插件库; `client_plugins` 测试在
  内置模式下跳过

---

## 7. 三期落地路线

### 一期 (C++ 插件框架)

1. `plugin_api.h` C ABI 契约
2. `ToolRegistry` 改造: `ToolcallWrapNode` 工具查找改为 Registry 优先 → NodeContext 静态列表回退
3. `PluginManager` / `NativePluginLoader` / `PluginHost` (vtable 实现)
4. 中间件 hook C ABI (7 钩子复用现有 handles 机制)
5. 事件订阅 (EventBus 转发)
6. 热插拔生命周期: inflight 计数 / disabled 位 / 卸载顺序 (等 0 → unload → dlclose)
7. `AgentConfig` + `config_loader`: `plugins:` 配置段
8. `BaseAgent::init()` 装配 PluginManager; `notifyStartup` 上报
9. `agentxx_cli`: `plugin list/install/unload/disable` 命令
10. CMake 接入 (lib 插件模块 + 示例插件 BUILD + 测试)
11. 示例插件 ×2 + 集成测试 (动态注册工具、运行中卸载不崩)

验收标准:
- 运行中加载插件 → 新工具立即可被 LLM 调用
- 运行中卸载插件 → 工具从注册表消失, 执行中的调用安全完成后才 dlclose
- 中间件钩子运行中启用/禁用生效
- 无新外部依赖 (一期不引入 QuickJS)

### 二期 (JS 支持)

1. QuickJS 集成到 `agent/third_party/quickjs/` (AGENTXX_ENABLE_PLUGIN_JS 编译开关)
2. 内置 `agentxx_javascript_engine` (实现 `interpreter.js` 能力)
3. PluginManager 委派加载 `type: js` 插件
4. JS 沙箱 + async/Promise 桥
5. JS 示例插件 + 测试

### 三期 (生态)

1. Wire 协议: `WirePluginList` / `WirePluginCmd` (远程热管理)
2. TUI 插件管理面板
3. 插件依赖排序 / 签名校验
4. CapabilityRegistry 插件互操作规范 (如第三方提供 `interpreter.python`)

---

## 8. 与现有代码的接入点

| 位置 | 改动 |
|------|------|
| `agent/lib/src/nodes/toolcall.cpp` | 工具查找改为 `ToolRegistry` 优先 (~10 行, 地基) |
| `agent/lib/include/agentxx/agent/context.h` | `AgentContext` 增 `ToolRegistry` / `PluginManager` / `CapabilityRegistry` 成员 |
| `agent/lib/src/agent/base_agent.cpp` | `init()` 装配 PluginManager; 启动阶段加载配置插件 |
| `agent/lib/include/agentxx/agent/config.h` + `config_loader` | `plugins:` 配置段 (目录/启停/参数) |
| 新增 `agent/lib/include/agentxx/plugin/plugin_api.h` | C ABI 契约 (纯 C) |
| 新增 `agent/lib/src/plugins/` | manager / native_loader / host_impl / tool_registry / capability_registry |
| 新增 `agent/third_party/quickjs/` | QuickJS 引擎 (二期) |
| 新增 `agent/plugins/javascript_engine/` | agentxx_javascript_engine 动态库 (二期) |
| `agent/client` | `plugin` CLI 命令; TUI 面板 (三期) |
| `agentxx_cli` 分发 | 附带 `plugins/` 目录 + 示例插件 |

---

## 9. 安全与权限

- 插件默认最小权限; manifest 声明所需权限 (`tool:register` / `network` / `command` / `filesystem`), 加载时校验
- C++ 插件视为受信 (等同内建代码), manifest 需来源提示/可选签名
- JS 插件沙箱: 无 IO、白名单 API、内存/栈/超时限制
- 插件执行路径仍经过现有 `PermissionMiddleware` (文件路径 HIL 拦截天然生效)
- 单个插件加载/执行失败仅记日志, 不影响 agent 主流程 (`catchErrorAsync` 语义)

---

## 10. 示例插件

### 10.1 C++ 插件骨架

```cpp
// my_tool_plugin.cpp —— 编译为 my_tool_plugin.so, 无需链接 libagentxx
#include "agentxx/plugin/plugin_api.h"

static char* my_exec(void* ud, const char* args_json, const char* thread_id,
                     const char* tool_call_id, char** err_out) {
    // 解析 args_json, 干活, 返回 malloc 字符串 (宿主 free)
    return strdup("{\"ok\": true}");
}

static const AgentxxHostVtable* g_host = nullptr;

extern "C" int agentxx_plugin_entry(const AgentxxHost* host, void** plugin_ctx) {
    g_host = &host->vtable;
    AgentxxToolSpec spec{};
    spec.name = "my_tool";
    spec.description = "My first plugin tool";
    spec.parameters_json = R"({"type":"object","properties":{}})";
    spec.execute = my_exec;
    return g_host->register_tool(host, &spec);   // 0 = ok
}
extern "C" void agentxx_plugin_unload(void* plugin_ctx) {
    g_host->unregister_tool(host, "my_tool");     // 卸载前反注册
}
```

### 10.2 插件目录布局

```
plugins/
└── my-plugin/
    ├── plugin.yaml        # 清单: name/version/entry/library/type(js|native)/permissions
    ├── libmy_plugin.so    # (native) 编译产物
    ├── plugin.js          # (js) 脚本入口
    └── assets/            # 附带资源 (SKILL.md / memory 文件 / prompt 模板)
```

---

## 12. client 侧插件系统 (四期)

> 状态: 已设计并实现一期 (2026-08): C ABI 契约 + ClientPluginManager + UI 适配器 +
> TUI/CLI 介入 + 跨端数据通道 (WirePluginDataUp) + 示例双端插件 + 集成测试
> (模块 `client_plugins`, 40 项)
> 目标: 插件可介入 client 的 CLI / TUI / 未来 GUI, 与 agent 侧插件体系并存、互通

### 12.1 背景与核心决策

agent 侧插件 (工具/钩子/事件/能力) 已完备, 但 client (CLI/TUI) 是另一类宿主:
有自己的生命周期、线程模型与 UI 形态。一期 `WirePluginData` 仅提供 agent→client
的只读数据通道, 插件无法自定义展示、无法接收用户操作。

| 决策 | 结论 | 理由 |
|------|------|------|
| 插件能否直接操作 UI 组件树 | **不能**。扩展点是 UI 无关的"语义层" | 暴露 FTXUI 组件会把插件绑死在 TUI 框架上, GUI 无法复用, CLI 无从谈起 |
| ABI 组织 | **独立头 `client_plugin_api.h` + 独立入口符号 `agentxx_client_entry` + 独立版本号** | client 侧扩展点迭代频繁; 独立符号集使 agent 侧 `plugin_api.h` 零改动, 旧插件不受影响 |
| 双端插件 | **同一动态库可导出两套入口**, 两个 PluginManager 各自 dlopen/装配 | codegraph 这类插件天然需要 "agent 侧提供工具 + client 侧展示进度" 的成对能力; dlopen 引用计数支持同库双实例 |
| 跨端通信 | **统一走 wire 协议**: 已有 `WirePluginData` (agent→client), 新增 `WirePluginDataUp` (client→agent) | 本地 Channel 模式与远程 WS 模式路径完全一致, 插件不感知部署形态 |
| 代码归属 | **UI 无关宿主层放 `agent/lib` (plugin/ 目录), TUI/CLI 适配器放 `agent/client`** | 未来 GUI 只需实现一个 `PluginUiAdapter` 即可复用宿主层 |

### 12.2 UI 无关语义层

插件对 UI 的全部控制力收敛为三件事:

1. **声明展示物**: 状态栏项 / 侧边栏面板 —— 内容是宿主定义 schema 的 JSON
   (text/kv/progress/action), 不是组件;
2. **声明拦截点**: 斜杠命令 (如 `/usage`) —— 回调拿到参数, 返回动作 JSON;
3. **调用交互原语**: toast / 代发消息 —— 命令式调用。

每个 UI 实现经 `ui_caps` 位图向插件声明自己支持什么, 不支持的注册项自动失败
(返回 NULL / 非 0), 插件自适应降级。CLI 的 caps = `TOAST` (命令为输入管线一部分,
必然支持); TUI 全量; 未来 GUI 自行声明。

### 12.3 跨端数据流 (本地/远程同路径)

```
agent 插件 publish("agentxx_codegraph.progress", {...})
  → SessionServerAgentIO → WirePluginData ──(Channel | WS)──▶ client 端点
  → ClientEventSink::onPluginData → 分发到订阅 EVT_PLUGIN_DATA 的 client 插件

client 插件 send_plugin_data("rebuild_request", {...})
  → WirePluginDataUp ──(Channel | WS)──▶ SessionServerAgentIO
  → agent 事件总线 publish("client.{插件名}.{事件名}", data)
  → agent 侧插件订阅消费
```

### 12.4 线程模型

| 线程 | 职责 | 插件可见性 |
|------|------|-----------|
| client io 线程 (端点所在 io_context) | 全部插件回调: entry 注册落地、事件 handler、命令 execute、panel action | 插件回调的唯一线程, 沿用"快速返回"约定 |
| UI 线程 (TUI FTXUI Loop) | 渲染语义元素、产生交互 (点击/按键) | **从不直接调用插件代码**; 交互经 postToIo 反向投递 |
| agent io 线程 | 现有 agent 插件体系 | 与 client 插件完全隔离 |
| client 侧线程池 | dlopen + entry 执行 | 插件无感 (注册经 ioCallSync 回 io 线程) |

安全不变式: 跨线程递给 UI 的数据一律是宿主已拷贝的 JSON 字符串; UI 线程持有
的数据不引用插件内存 → UI 侧不参与 inflight 计数, 卸载只需等 io 线程 inflight
归零 (复用 InflightGuard 模式)。

### 12.5 组件清单

| 组件 | 位置 | 说明 |
|------|------|------|
| `client_plugin_api.h` | `agent/lib/include/agentxx/plugin/` | 纯 C ABI 契约头 (client v1); 复用 `AgentxxPluginStringView`/`AGENTXX_SV` 约定; 入口符号 `agentxx_client_get_info` / `agentxx_client_entry` / `agentxx_client_unload` |
| `ClientPluginManager` | `agent/lib/src/plugins/client_plugin_manager.cpp` | client 侧管理器: 生命周期 (load/enable/disable/unload/shutdownAll) + UI 注册表 (status items/panels/commands) + 事件分发 (实现 `ClientEventSink`) + vtable 实现; 依赖拓扑排序; 加载时过滤 `sides` 配置 |
| `ClientEventSink` | `agent/lib/include/agentxx/agent/io/client_event_sink.h` | 端点 → 插件的事件通道 (ready/conn_state/user_input/delta/turn_end/session_switch/plugin_data); `AgentIOBase::setEventSink` 注入 |
| `PluginUiAdapter` | `agent/client/include/agentxx-client/io/plugin/` | UI 无关宿主层要求的适配器接口 (uiCaps/注册/更新/toast/sendPluginMessage) |
| `TuiPluginAdapter` | `agent/client/src/io/tui/` | 状态栏项 → StatusBarComponent 槽位; 面板 → SidebarComponent tab; toast → 既有 showToast; 命令动作 → enqueueUiAction 投递 |
| `CliPluginAdapter` | `agent/client/src/io/stdio/` | caps 仅 TOAST; toast → stderr (保持 stdout 纯净); 命令动作 send → 端点 sendToPeer |
| Wire 消息 | `agent_io_transport.h` / `wire_protocol.h` / `ws_io_transport.cpp` | `WirePluginDataUp` 变体 + `MsgType::PluginDataUp` + 序列化; 服务端 onPeerMessage 收到后 publish 到总线 `client.{plugin}.{event}` |

### 12.6 入口 ABI 摘要

```c
#define AGENTXX_CLIENT_PLUGIN_API_VERSION 1

typedef struct AgentxxClientPluginInfo {
    int api_version;                /* == AGENTXX_CLIENT_PLUGIN_API_VERSION */
    AgentxxPluginStringView name;   /* 全局唯一 (与 agent 侧插件共用命名空间) */
    AgentxxPluginStringView version;
    AgentxxPluginStringView description;
    uint32_t min_ui_caps;           /* 宿主不满足则拒绝加载 */
} AgentxxClientPluginInfo;

/* 事件订阅 (payload 均为 JSON 字符串) */
typedef enum {
    AGENTXX_CLIENT_EVT_READY, AGENTXX_CLIENT_EVT_CONN_STATE,
    AGENTXX_CLIENT_EVT_USER_INPUT, AGENTXX_CLIENT_EVT_DELTA,
    AGENTXX_CLIENT_EVT_TURN_END, AGENTXX_CLIENT_EVT_SESSION_SWITCH,
    AGENTXX_CLIENT_EVT_PLUGIN_DATA, AGENTXX_CLIENT_EVT_COUNT
} AgentxxClientEvent;

/* 命令 execute 返回的动作 JSON (宿主解释执行):
   {"action":"none"}                      已处理完毕
   {"action":"send","text":"..."}         代为发送一条用户消息
   {"action":"toast","text":"...","level":0|1|2} */

typedef struct AgentxxClientHostVtable {
    /* 内存/日志/JSON (与 agent 侧 vtable 同语义) */
    void* (*alloc)(size_t);  void (*free)(void*);  char* (*strdup)(const char*);
    void  (*log)(const AgentxxClientHost*, int, AgentxxPluginStringView);
    char* (*json_get_string)(...);  char* (*json_escape)(...);
    /* 能力协商 */
    uint32_t (*ui_caps)(const AgentxxClientHost*);
    /* 展示扩展 (语义 JSON) */
    AgentxxStatusItem* (*register_status_item)(const AgentxxClientHost*,
        AgentxxPluginStringView id, AgentxxPluginStringView initial_json, int align, int order);
    int  (*update_status_item)(const AgentxxClientHost*, AgentxxStatusItem*, AgentxxPluginStringView json);
    void (*unregister_status_item)(const AgentxxClientHost*, AgentxxStatusItem*);
    AgentxxPanel* (*register_panel)(const AgentxxClientHost*, AgentxxPluginStringView id,
        AgentxxPluginStringView props_json);
    int  (*update_panel)(const AgentxxClientHost*, AgentxxPanel*, AgentxxPluginStringView items_json);
    void (*unregister_panel)(const AgentxxClientHost*, AgentxxPanel*);
    /* 输入扩展 */
    int  (*register_command)(const AgentxxClientHost*, AgentxxPluginStringView name,
        AgentxxPluginStringView description,
        char* (*execute)(void* ud, AgentxxPluginStringView args_json, char** error_out), void* ud);
    int  (*unregister_command)(const AgentxxClientHost*, AgentxxPluginStringView name);
    /* 交互原语 */
    void (*show_toast)(const AgentxxClientHost*, AgentxxPluginStringView text, int level);
    /* 事件订阅 (卸载自动退订) */
    AgentxxSubscription* (*subscribe)(const AgentxxClientHost*, int event,
        void (*handler)(AgentxxPluginStringView payload_json, void* ud), void* ud);
    void (*unsubscribe)(AgentxxSubscription*);
    /* 会话上下文快照 */
    char* (*get_client_state)(const AgentxxClientHost*);
    /* 会话操作 */
    int  (*send_user_input)(const AgentxxClientHost*, AgentxxPluginStringView thread_id,
        AgentxxPluginStringView text);
    void (*request_cancel)(const AgentxxClientHost*, AgentxxPluginStringView thread_id);
    /* 跨端数据: client 实例 → wire → agent 侧实例 */
    int  (*send_plugin_data)(const AgentxxClientHost*, AgentxxPluginStringView event,
        AgentxxPluginStringView json);
    /* 自描述 */
    char* (*get_own_info)(const AgentxxClientHost*);
    char* (*get_plugin_args)(const AgentxxClientHost*);
} AgentxxClientHostVtable;
```

### 12.7 配置格式

```yaml
plugins:
  - path: ./plugins/agentxx_codegraph   # 双端: 按导出符号自动生效 (sides 缺省 auto)
  - path: ./plugins/my-statusbar
    sides: client                       # client|agent|auto(默认; both 同 auto)
    args: { refresh: 2 }
```

- `sides` 语义: `agent` 仅 agent 侧加载; `client` 仅 client 侧加载; `auto` 按导出
  符号 (有 `agentxx_client_entry` 才在 client 侧加载, 无则跳过并记日志);
- 本地一体模式: agent 侧加载逻辑不变 (BaseAgent::init), client 侧由 mode_runners
  加载同一配置 (经 sides 过滤); 双端插件同库被两个管理器各自 dlopen (引用计数
  互不影响);
- 远程 client 模式: 只加载 client 侧条目, agent 条目被过滤 (agent 在 server 进程)。

### 12.8 与现有代码的接入点

| 位置 | 改动 |
|------|------|
| 新增 `agent/lib/include/agentxx/plugin/client_plugin_api.h` | client C ABI 契约 (纯 C) |
| 新增 `agent/lib/include/agentxx/plugin/client_plugin_manager.h` + `src/plugins/client_plugin_manager.cpp` | client 侧管理器 + vtable 实现 + UI 注册表 + 事件分发 |
| 新增 `agent/lib/include/agentxx/agent/io/client_event_sink.h` | 端点 → 插件事件通道 |
| `agent/lib/include/agentxx/agent/io/agent_io.h/.cpp` | `setEventSink` + `emitEventSink`; 基类 onPeerMessage 分发 Delta/TurnResult/PluginData 到 sink; `sendUserInput` 通知; `onServerReady` 默认实现通知 ready |
| `agent/lib/include/agentxx/agent/io/agent_io_transport.h` | 新增 `WirePluginDataUp` 变体 |
| `agent/lib/include/agentxx/agent/io/wire_protocol.h` | `MsgType::PluginDataUp` + `makePluginDataUp`/`pluginDataUpFromJson` |
| `agent/lib/src/agent/io/ws_io_transport.cpp` | serialize/deserialize 支持 `WirePluginDataUp` |
| `agent/lib/src/agent/io/session_server_agent_io.cpp` | onPeerMessage 收 `WirePluginDataUp` → 总线 publish `plugin.client.{插件名}.{事件名}` (载荷 std::string); 环回跳过 (plugin.client. 前缀事件不转发回客户端) |
| `agent/lib/include/agentxx/agent/config.h` | `PluginConfig` 增 `sides` (PluginSide 枚举: Auto/Agent/Client) |
| `agent/client/src/config_loader.cpp` | 解析 `sides` 字段 |
| 新增 `agent/client/include/agentxx-client/io/plugin/plugin_ui_adapter.h` | UI 适配器接口 |
| 新增 `agent/client/include/agentxx-client/io/tui/tui_plugin_adapter.h` + `io/stdio/cli_plugin_adapter.h` | TUI/CLI 适配器 |
| `agent/client/src/io/tui/agent_tui.h/.cpp` | EventSink 通知点; 命令管线 (onSend 拦截 `/`); 插件状态栏槽位 / 侧边栏 tab 挂载 (addPluginPanelTab/renderPluginPanel) |
| `agent/client/src/io/tui/components/status_bar.cpp` | 状态栏渲染插件状态栏项 (左/右分组, order 排序) |
| `agent/client/src/io/stdio/agent_stdio.h/.cpp` + `mode_runners.cpp` | CLI 命令管线; `sendPluginUserInput` 路径接入 sink |
| `agent/client/src/mode_runners.cpp` + `main.cpp` | 4 个模式装配 ClientPluginManager + 适配器, 传入 yaml plugins 配置 |
| `agent/plugins/example_plugin/` | 增加 client 入口演示双端插件 (状态栏/面板/命令/事件/跨端) |
| 新增 `agent/test/core/test_client_plugins.*` | client 插件测试模块 (40 项) |

### 12.9 实现偏差与要点 (以代码为准)

1. **事件上行 topic 前缀**: 服务端发布 `plugin.client.{插件名}.{事件名}` (而非设计稿的
   `client.{...}`), 与 agent 侧事件统一加 `plugin.` 前缀; 插件侧 subscribe 传
   `client.{插件名}.{事件名}` 即可。
2. **环回防护**: `subscribePluginEvents` 跳过 `plugin.client.` 前缀 (client 上行事件
   不转发回客户端, 防止 client 插件收到自己发出的数据形成环回)。
3. **命令动作 JSON**: 一期支持 `send` / `toast` / `none`; `json_escape` 返回带引号的
   JSON 字符串字面量 (与 agent 侧一致), 插件拼装动作 JSON 时不得再额外加引号。
4. **句柄生命周期**: disable 保留注册信息与句柄 (enable 重建), unload 时句柄存活到
   实例析构 (插件的 unload 回调可能主动反注册, 句柄必须有效)。
5. **远程 client 模式**: 只加载 client 侧条目 (agent 条目被 sides 过滤, agent 在
   server 进程); `io->onServerReady()` 在连接建立后调用 (TUI 覆写版幂等, 同时通知
   事件接收器)。

### 12.9 安全

- `send_user_input` 等价于代用户发言: 动作 send 一律经 UI 既有排队语义
  (流式中进 pendingInputs), 不绕过 UI 状态机;
- 可见性声明: 插件可订阅 EVT_USER_INPUT / EVT_DELTA (全量消息), 文档明确告知;
- UI 不越权: 语义层原语无"读屏/注入按键"能力; 命令只拦截以 `/` 开头的输入,
  未命中命令照常作为普通消息发送;
- 跨端通道: `WirePluginDataUp` 在服务端发布到 `plugin.client.` 前缀 topic
  (agent 侧插件订阅 `client.{插件名}.{事件名}` 消费), 频率由插件控制;
  服务端不解析载荷语义 (与 WirePluginData 对称), 且环回跳过 (不转发回客户端)。