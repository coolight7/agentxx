# Agentxx 插件系统设计方案

> 待实现: Wire 远程热管理 / TUI 插件管理面板 / 签名校验
> 关联: [design.md](design.md)
> 目标: 原生 C++ 插件 + 脚本插件 (由 C++ 插件承载), 支持热插拔、强自定义
> 本文以当前源码为准 (plugin_api.h v6 / client_plugin_api.h v2), 设计原稿与实现偏差见 [13. 实现状态与偏差](#13-实现状态与偏差)

## 目录

- [1. 目标与范围](#1-目标与范围)
- [2. 现状盘点与可行性分析](#2-现状盘点与可行性分析)
- [3. 总体架构](#3-总体架构)
- [4. C++ 插件接口设计 (核心)](#4-c-插件接口设计-核心)
- [5. 热插拔机制](#5-热插拔机制)
- [6. JS 插件支持 (统一插件模型)](#6-js-插件支持-统一插件模型)
- [7. client 侧插件系统](#7-client-侧插件系统)
- [8. 内置插件清单](#8-内置插件清单)
- [9. 配置与分发](#9-配置与分发)
- [10. 示例插件](#10-示例插件)
- [11. 安全与权限](#11-安全与权限)
- [12. 与现有代码的接入点](#12-与现有代码的接入点)
- [13. 实现状态与偏差](#13-实现状态与偏差)
- [14. 尚未实现与路线图](#14-尚未实现与路线图)

---

## 1. 目标与范围

### 1.1 目标

- **能力最强**: 插件可扩展 agent 的 工具 / 中间件钩子 / 事件 / 能力 / UI (client 侧) 多个面
- **自定义能力强**: 从零代码(声明式 yaml 配置)到原生 C++ (全量能力) 的全谱系覆盖; JS 脚本插件经引擎插件承载
- **热插拔**: 运行中加载/卸载/启停插件, 不重启 agent

### 1.2 范围决策

| 决策 | 结论 | 理由 |
|------|------|------|
| 插件形态 | **C++ 动态库 (一级公民)** | 能力天花板最高, 无翻译层损耗 |
| 脚本语言 | **JS** (QuickJS 引擎插件承载) | JS 熟悉度高; 核心框架形态单一 (统一插件模型, 宿主不特判脚本类型) |
| 语言解释器 | **降格为 C++ 插件** | 语言支持可插拔 (未来 Python 注册 `interpreter.py` 能力即可, 宿主零改动) |
| 主图拓扑 | **不支持运行期修改** | checkpoint/恢复依赖节点名, 风险不可控; 自定义流程走子图注入 |
| 插件接口 | **纯 C ABI + vtable** | 跨编译器/STL ABI 稳定, 热插拔安全, 插件可用任意语言/编译器实现 |
| 脚本模型 | **统一插件模型** | 所有插件都是 C++ 插件 (有 dlHandle/entry); 脚本能力是插件内部实现的一部分, 经能力调用委派给 interpreter 引擎 |

---

## 2. 现状盘点与可行性分析

### 2.1 已有扩展点 (插件化的天然基础)

| # | 扩展点 | 载体 | 注册方式 | 热插拔现状 |
|---|--------|------|----------|-----------|
| 1 | 工具 Tool | `XXToolBase` (继承 `neograph::Tool`) | `BaseAgent::createTools()` 虚函数返回 vector; 中间件可附带 `toolcalls` | ✅ 插件工具经 ToolRegistry 动态注册 |
| 2 | 中间件 Middleware | `BaseMiddlewareHandle<T>` (7 钩子) | `setupMiddleware()` push 到 `middlewareHandleContext->handles` | ✅ 插件钩子经 handles 热增 + disabled 位热移除 |
| 3 | 节点 Node + 图 | `GraphRegistry.register_type` + `buildGraphDefinition()` | `registerNodes()` 虚函数 | ❌ 主图固定; 子图可独立 |
| 4 | 事件 EventBus | `EventStream<T>` / `RequestResponseStream<Req,Resp>` | `bus.get<T>(topic).subscribe()` | ✅ 运行时可订阅 (插件事件桥转发) |
| 5 | 来源注入 | Skill (SKILL.md) / MemoryFile / Planning | Skill/MemoryFile 中间件按目录扫描 | ⚠️ 目录启动时固定 |
| 6 | 远程能力 | MCP Client (HTTP SSE + stdio) / A2A / ACP / DI 容器 | yaml 配置 `mcp_servers` | ⚠️ 启动时连接 |

### 2.2 关键代码事实 (设计依据)

- **工具查找是静态快照 + 动态回退**: `toolcall.cpp` 先从静态 `tools_` (NodeContext init 时固化) 查找, 未命中再查 `ToolRegistry` (插件动态工具表, 返回 shared_ptr 保活) → 插件工具热注册/摘除即时生效。
- **LLM 请求侧 schema 动态组装**: `modelcall.cpp` `ModelCallWrapNode` 每轮从静态 `tools_` 组装工具定义后, 追加 `toolRegistry->appendDefinitions` → 插件工具热注册后下一轮 modelcall 即对模型可见, 卸载后定义自动消失。
- **中间件是活读取**: `wrap_handle.h` `WrapHandleBaseNode::run()` 每次执行重新读 `handles` 长度并按**下标**遍历, 运行中 `push_back` 不破坏迭代 (len 已缓存、元素 shared_ptr); start 阶段记录实际执行下标, end 阶段按记录逆序回放 → **中间件热增在引擎层安全, 运行中禁用不破坏 start/end 配对**。
- **中间件 disabled 位**: `BaseMiddlewareHandleInterface::disabled` (普通中间件恒 false); `WrapHandleBaseNode` start/end 循环均跳过 disabled 项。
- **Tool 接口被冻结**: `neograph/tool.h` 注释 "the frozen Tool vtable"; `XXToolBase` 是薄封装 (加 autoSummaryOutput / canDelayLoad / maxRetry)。
- **无锁单线程模型**: Session 可变状态 `assertIoThread()` 强制仅 io 线程读写; 插件注册/注销**必须 post 到 io 线程串行执行** (vtable 内部经 `ioCallSync` 处理, 插件无感)。
- **现有线程卸载基建**: `blockingPool` + `offloadAsync` / `offloadCancellableAsync` (带 CancelToken watcher) 复用为插件同步 C 回调的异步执行通道; 超时经 `asyncWithTimeout`。

### 2.3 热插拔可行性矩阵

| 扩展点 | 热加载 | 热卸载 | 难度 | 方案要点 |
|--------|:------:|:------:|------|----------|
| 注册新工具 | ✅ | ✅ | 低 | `ToolRegistry` 动态查表 (shared_ptr 保活) |
| 卸载/禁用工具 | ✅ | ✅ | 低 | Registry 摘引用; 在途工具靠 shared_ptr 引用计数跑完; 插件实例 inflight 计数保证卸载前代码段存活 |
| 事件订阅/发布 | ✅ | ✅ | 低 | EventBus 运行期订阅, RAII 退订; 插件订阅自动清理 |
| 中间件钩子 (新类) | ✅ (轮次边界) | ✅ (disabled 位) | 中 | handles 按索引活读取已安全; 卸载用 disabled 位 + 轮末 `flushPendingCleanup` 摘除 |
| 自定义节点类型 | ⚠️ 新会话/子图 | ❌ 运行中 | 高 | GraphRegistry 新类型仅影响之后编译的图 |
| 主图拓扑 | ❌ | ❌ | 极高 | 不支持 |
| 模型 Provider | ⚠️ 启动时 | ❌ 运行中 | 中 | 需加动态注册接口 |
| client UI 扩展 (状态栏/面板/Info 段落/命令) | ✅ | ✅ | 中 | client 侧独立 ClientPluginManager + UI 注册表 COW 快照 |
| 协议打包 (MCP/A2A) | ⚠️ 启动时 | ⚠️ | 中 | 复用现有 MCP client 装配 |

**结论**: 工具 / 事件 / 钩子 / 能力 / client UI 可做到**真正运行期热插拔**, 覆盖 90% 用户扩展诉求; 节点/主图属编译期形态, 通过子图注入获得自定义流程。

---

## 3. 总体架构

```
libagentxx 核心 (只认识 C++ 插件; 不依赖任何脚本引擎)
┌────────────────────────────────────────────────────────────┐
│ PluginManager (io 线程串行, 无锁模型不变)                   │
│   ├─ NativeLoader: dlopen/LoadLibraryW → C ABI 入口符号     │
│   ├─ 生命周期: Load→Enable→(inflight 计数)→Disable→Unload   │
│   ├─ ToolRegistry: 动态工具注册表 (shared_ptr 持有)          │
│   ├─ CapabilityRegistry: 能力注册表 (插件互查/委派调用)      │
│   └─ 依赖图: depends/optionalDepends + 拓扑排序加载 + 级联   │
│              ▲ 持有                                        │
│   PluginHost vtable (C ABI, 每插件一个 AgentxxHost 句柄)    │
└──────────────┬─────────────────────────────────────────────┘
               │
      ┌────────┴────────────┐
      │ 引擎插件 (随发行包)   │        ← 插件间委派 (宿主不参与)
      │ agentxx_javascript_engine │◄──┐ invoke_capability("interpreter.js", load/unload)
      │  能力: interpreter.js     │   │
      └────────┬───────────────┘   │
               │                   │
      ┌────────┴────────────┐     │
      │ 脚本插件 (C++ 壳)     │─────┘
      │ example_js: 壳+plugin.js  │  depends: [agentxx_javascript_engine]
      └─────────────────────┘

client 侧 (CLI/TUI, 独立宿主)
┌──────────────────────────────────────────────┐
│ ClientPluginManager (client io 线程串行)       │
│   ├─ UI 注册表: status items / panels / info sections / commands│
│   ├─ ClientEventSink: 端点事件 → 插件订阅分发   │
│   ├─ PluginUiAdapter: UI 无关语义层 → TUI/CLI  │
│   └─ 跨端: WirePluginData / WirePluginDataUp   │
└──────────────────────────────────────────────┘
```

**核心规则**:
- libagentxx 核心**不依赖** QuickJS/任何脚本引擎; 核心只认 C++ 动态库插件。
- JS (及未来任何语言) 支持 = 一个实现了 `interpreter.<lang>` 能力的 C++ 插件 (引擎插件)。
- **统一插件模型**: 所有插件都是 C++ 插件 (有 dlHandle/entry); 脚本插件 = 薄 C++ 壳, 壳在 entry 里经能力调用把脚本交给引擎执行; 宿主不特判任何脚本类型。
- 插件与宿主一切数据交换走 C ABI (JSON 字符串 / 函数指针 / 不透明句柄)。

### 3.1 组件清单 (新增, 均在 `agent/lib/`)

| 组件 | 位置 | 说明 |
|------|------|------|
| `plugin_api.h` | `include/agentxx/plugin/` | 纯 C ABI 契约头 (v6, 唯一跨版本稳定接口) |
| `client_plugin_api.h` | `include/agentxx/plugin/` | client 侧纯 C ABI 契约头 (v2, 独立符号集) |
| `PluginManager` | `src/plugins/plugin_manager.cpp` | agent 侧生命周期管理 (load/enable/disable/unload/shutdownAll + 依赖图) |
| `ClientPluginManager` | `src/plugins/client_plugin_manager.cpp` | client 侧管理器 (UI 注册表 + 事件分发 + vtable 实现) |
| `PluginInstance` | `include/agentxx/plugin/plugin_manager.h` | 已加载插件实例 (注册残留/inflight/依赖) |
| `PluginTool` | 同上 | C ABI spec → `XXToolBase` 适配 (线程池执行 + 取消/超时) |
| `PluginMiddlewareHandle` | 同上 | 7 个 C 钩子 → 现有 handles 栈式执行 |
| `ToolRegistry` | `src/plugins/tool_registry.cpp` | 动态工具查表 (shared_ptr 持有 + 静态名冲突检测) |
| `CapabilityRegistry` | plugin_manager 内 | 能力注册表 (含方法回调, `invoke_capability` 通用插件间通信) |
| `NativeLoader` | plugin_manager 内 | 平台封装 dlopen ↔ LoadLibraryW |
| `ClientEventSink` | `include/agentxx/agent/io/client_event_sink.h` | 端点 → client 插件事件通道 |
| `PluginUiAdapter` | `client/include/agentxx-client/io/plugin/` | client UI 适配器抽象 (TUI/CLI/未来 GUI 实现) |

---

## 4. C++ 插件接口设计 (核心)

### 4.1 为什么纯 C 稳定 ABI

C++ 跨动态库边界传对象有三大致命问题: 编译器不匹配 (GCC/Clang/MSVC 的 STL/异常 ABI 不同)、`libstdc++`/`libc++` 混用、RTTI/typeid 不一致。故插件接口定义为**纯 C 头 + vtable**, 插件可用任意编译器/任意语言实现, 无需链接 libagentxx。

### 4.2 plugin_api.h 契约要点 (v6, 以实际头文件为准)

`agent/lib/include/agentxx/plugin/plugin_api.h` —— 纯 C, 无任何 C++ 类型。要点:

**字符串约定 (v6)**:
- 所有跨边界"字符串参数/字段"一律 `AgentxxPluginStringView` (data+size 只读借用, 不要求 NUL 结尾, 生命周期仅覆盖本次调用; 宿主不得保存引用)
- 所有"宿主分配"的字符串返回值 (`execute` 结果 / `error_out` / `strdup` / `list_plugins` / `get_plugin` / `json_*` 等) 为 `char*` (NUL 结尾, `host->alloc` 分配), 调用方用完必须 `host->free`
- 插件侧构造: 字面量用 `AGENTXX_SV("...")`, 运行时字符串用 `agentxx_plugin_sv(data, size)` / `agentxx_plugin_sv_cstr(s)`

**跨 CRT 堆边界**: vtable 提供 `alloc` / `free` / `strdup` (跨 CRT 堆边界的唯一分配通道); 插件返回的字符串必须经宿主分配 (`AGENTXX_STRDUP(host, s)` 宏), 不得直接用 `malloc`/`strdup`。

**宿主 API (插件侧判空调用)**:

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

**工具定义**:
```c
typedef struct AgentxxToolSpec {
    AgentxxPluginStringView name;            ///< 须全局唯一 (与内置/MCP 工具同名注册失败)
    AgentxxPluginStringView description;
    AgentxxPluginStringView parameters_json; ///< JSON Schema 字符串
    char* (*execute)(void* user_data,         ///< 同步执行回调 (宿主线程池线程)
                     AgentxxPluginStringView args_json,
                     AgentxxPluginStringView thread_id,
                     AgentxxPluginStringView tool_call_id,
                     char** error_out);       ///< 失败返回 NULL 并 error_out (host->alloc)
    void* user_data;
    long  default_timeout_ms;                 ///< 0 = 不限制
    int   flags;                              ///< AGENTXX_TOOL_FLAG_AUTO_SUMMARY 等
} AgentxxToolSpec;
```
- 回调内可调用 `call_tool` / `log` / `json_*`; 不得阻塞宿主 io 线程
- 宿主超时/取消仅终止"等待", 回调一旦开始执行将持续到返回 (宿主按插件实例 inflight 计数保证执行期间代码段不被卸载)

**钩子点** (与宿主 7 个中间件钩子一一对应):
```c
typedef enum AgentxxHookPoint {
    AGENTXX_HOOK_AGENT_START, AGENTXX_HOOK_AGENT_END,    ///< 会话轮次开始/结束
    AGENTXX_HOOK_MODEL_START, AGENTXX_HOOK_MODEL_RUN,    ///< LLM 调用 (RUN 重试时多次)
    AGENTXX_HOOK_MODEL_END,                              ///< LLM 调用结束
    AGENTXX_HOOK_TOOL_START,  AGENTXX_HOOK_TOOL_END,     ///< 工具分发开始/结束
    AGENTXX_HOOK_COUNT
} AgentxxHookPoint;
/// 钩子回调 (io 线程同步调用, 快速返回): node_input_json 为节点输入摘要
/// ({"thread_id","node","messages_count",...}); out_json 预留
typedef int (*AgentxxHookFn)(void* user_data, AgentxxHookPoint point,
                             AgentxxPluginStringView node_input_json,
                             char** out_json, char** error_out);
```

**宿主函数表** (vtable 全部函数内部捕获异常, C ABI 边界无异常外泄):

| 分组 | 函数 | 说明 |
|------|------|------|
| 内存 | `alloc` / `free` / `strdup` | 跨 CRT 堆边界唯一分配通道 |
| 工具注册 | `register_tool` / `unregister_tool` | 热插拔; 名称冲突返回非 0 |
| 中间件钩子 | `register_hook` / `unregister_hook` | 热插拔, 轮次边界生效 |
| 事件 | `subscribe` / `unsubscribe` / `publish` | topic 自动加 `plugin.` 前缀; 载荷 JSON 字符串; publish 异步投递 |
| 能力 | `register_capability` / `register_capability_ex` / `unregister_capability` / `has_capability` | `_ex` 附带方法回调 (能力调用 = 通用插件间通信通道, 如 `interpreter.js` 的 load/unload) |
| 能力调用 | `invoke_capability` | 调用提供者方法; 提供者回调在**调用方线程**执行 (查表短临界区在 io 线程, 防死锁) |
| 线程投递 | `is_io_thread` / `post_to_io` | 非 io 线程调用方使用; vtable 内部自动处理 |
| 会话访问 | `call_tool` / `get_share_store` / `emit_message_tip` / `log` | call_tool 仅可调用插件注册的工具 (查表在 io 线程短临界区, 目标 execute 在调用方线程; 目标插件 shared_ptr 保活) |
| 插件互查 | `list_plugins` / `get_plugin` / `get_own_info` | JSON 数组/单对象; 含 name/version/path/enabled/tools/capabilities/depends/optional_depends |
| JSON 辅助 | `json_get_string` / `json_escape` | 提取 key 字符串值 / 字符串 → 带引号 JSON 字面量 (防注入/语法错误, 替代手写解析) |
| 宿主配置 | `get_config` / `get_plugin_args` / `get_tool_prompt` | 通用宿主信息 (dataDir/projectRoot/platform) / 本插件 yaml args (宿主不解析) / 工具提示词 (depict/args) |

**入口符号** (dlsym):
```c
#define AGENTXX_PLUGIN_SYMBOL_GET_INFO "agentxx_plugin_get_info"  ///< 可选: 元信息 (加载前校验)
#define AGENTXX_PLUGIN_SYMBOL_ENTRY    "agentxx_plugin_entry"     ///< 必需: 入口
#define AGENTXX_PLUGIN_SYMBOL_UNLOAD   "agentxx_plugin_unload"    ///< 可选: 卸载通知
```
- `entry` 运行在宿主线程池; 其中经 vtable 的注册/订阅等 io 线程约束操作由宿主自动投递回 io 线程串行执行 (插件无感, entry 内可安全调用任意 API)
- `unload` 在宿主等全部在途回调完成后调用; 宿主会在此之前自动反注册该插件的一切工具/钩子/订阅/能力

**版本策略**: 修改契约时递增 `AGENTXX_PLUGIN_API_VERSION`; 宿主拒绝 api_version 不匹配的插件 (仅拒绝, 不崩溃)。

### 4.3 宿主侧适配 (vtable → 现有强类型世界)

- `register_tool` → 包装成 `PluginTool` (`XXToolBase` 子类), `execute_async` 内部经 `offloadCancellableAsync` (+ CancelToken watcher) 卸载到线程池调用 C 回调; 取消/超时语义天然接入 toolcall 链路。字符串字段 (name/description/parameters_json) 构造时从 string_view 拷贝进成员 (不依赖插件侧内存存活)。
- `register_hook` → `PluginMiddlewareHandle` (`BaseMiddlewareHandle<BaseMiddlewareState>` 子类), 七个覆写转发到 C 回调, **push 进现有 `handles` vector** → 栈式执行、错误重抛、per-thread state 全部复用, 不改 `wrap_handle.h` 引擎逻辑。注册即创建中间件句柄 (懒创建, 一个插件一个), disable 置 disabled 位, 轮末摘除。
- `subscribe` → 直接转发 `EventBus` (topic 加 `plugin.` 前缀, 载荷 `std::string`), 插件卸载自动退订。
- `call_tool` → 经 ToolRegistry 查表调用; 是插件互调 + JS 插件"工具调用其他工具"的唯一通道。

### 4.4 SDK 形态

- `agentxx/plugin/plugin_api.h`: 纯 C ABI 头 (**唯一跨版本契约**), 插件编译**无需链接 libagentxx**
- 插件侧可直接按 C 头编写 (参考 `agent/plugins/example_plugin/`); 无额外 C++ 包装要求

### 4.5 导出符号控制 (2026-08)

> 目标: 严格限制插件动态库的导出面, 仅宿主 dlsym/GetProcAddress 按名查找的
> 入口符号导出, 其余全部隐藏 (插件自身 C++ 内部符号 + 第三方静态库符号)。

**背景**: 插件默认按全局可见性编译, 内部符号 (STL 实例化/typeinfo/第三方静态库
如 simdjson/tree-sitter/io_uring 的符号) 全部进入动态符号表, 实测 `libagentxx_codegraph.so`
导出 **12479** 个符号 / `libagentxx_system_monitor.so` 导出 **4394** 个。危害:
污染宿主动态符号表、多插件同名符号冲突、宿主符号误绑定到插件副本、加载/重定位
开销。

**机制** (三层, 缺一不可):

1. **源码标记**: `plugin_api.h` 定义 `AGENTXX_PLUGIN_EXPORT` 宏
   - GCC/Clang: `__attribute__((visibility("default")))`
   - MSVC: `__declspec(dllexport)`
   - 内置合并编译模式 (`AGENTXX_PLUGIN_BUILTIN`): 展开为空 (符号直接并入
     libagentxx, 无需导出)
   - 插件定义入口函数时必须加宏前缀 (宏在 `extern "C"` **之后**):
     ```c
     extern "C" AGENTXX_PLUGIN_EXPORT int agentxx_plugin_entry(const AgentxxHost* host, void** plugin_ctx);
     ```
2. **编译期隐藏**: 插件项目统一 `-fvisibility=hidden -fvisibility-inlines-hidden`
   (plugins/CMakeLists.txt `add_compile_options`, 作用于全部插件目标)
3. **链接期兜底**: 隐藏第三方静态库中按默认可见性编译的符号
   - Linux/Android (ELF): `-Wl,--version-script=<白名单.map>` —
     `global:` 列出 6 个入口符号, `local: *` 隐藏其余全部
   - macOS: `-Wl,-exported_symbols_list` (符号带 `_` 前缀)
   - Windows/MSVC: 不做自动导出 (`CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS` 保持 OFF),
     仅 `__declspec(dllexport)` 标记的入口符号导出; 静态库链入 DLL 的符号
     默认不导出

**效果** (实测): 9 个插件动态库导出符号数 = 入口符号数
(双端插件 6 个: agent 入口 3 + client 入口 3; 单端插件 3 个); dlopen/dlsym
全部正常, 插件内部符号不可见 (dlsym 查不到 simdjson/内部函数等)。

### 4.6 工具函数复用 (agentxx_util 静态库, 2026-08)

> 目标: 插件复用主程序 `agent/lib/include/agentxx/util` 的部分工具函数,
> 同时保持 C ABI 契约的独立性 (插件不链接 libagentxx)。

**约束** (AGENTS.md): 主程序与插件的复用代码必须**静态链接进各自**,
确保不同版本编译的主程序/插件可互相加载运行; 复用代码的结构体不得跨边界传递。

**方案选型**: 独立静态库 `agentxx_util` (而非插件直接编译 util cpp 或共享
libagentxx):

| 方案 | 结论 |
|------|------|
| 插件直接导入 util 头 + 编译其 cpp | 每个插件重复配置 include/宏/第三方依赖 (fmt/uchardet/iconv/sqlite3), 内置模式需同步维护, 编译时间浪费 |
| 独立静态库 `agentxx_util` | **采用**: 编译一次; CMake 一行链接 + 依赖/宏传递; libagentxx 与插件各自静态链接一份, 符号经导出控制隐藏互不冲突 |
| 链接 libagentxx | 违反 C ABI 契约 (STL/异常 ABI 耦合), 不可行 |

**agentxx_util 组成** (lib/CMakeLists.txt, **src/util/ 全部源文件**):

- 收录: `http_client` / `http_server` / `ws_client` / `string_util` / `util` /
  `sqlite` / `settings_db` / `log` / `regex` / `http_header` 全部 10 个 cpp
  + `include/agentxx/util/` 全部头文件 (`aho_corasick` / `async_mutex` /
  `async_offload` / `diff_util` / `exception` / `http_client` / `http_header` /
  `http_server` / `log` / `lru_cache` / `regex` / `router` / `settings_db` /
  `sqlite` / `stream` / `string_util` / `util` / `ws_client`)
- **定位**: 面向 agentxx 内置插件 (与主程序同一 superbuild 构建、依赖环境
  齐全) 的便捷复用库; 第三方插件完全不需要它 (纯 C ABI 头即可, 甚至不用
  C++), 不 find_package 本库即零影响

**依赖: 全部 PUBLIC 链接 + include 传递** (内置插件链接本库后直接获得全部
util 能力, 无需自行配置任何依赖):

- **PUBLIC 链接接口** (随 INTERFACE 传播, 插件 `find_package(agentxx_util)`
  后自动获得):
  - 轻量 imported target: `fmt` / `SQLite3` / `uchardet` / `iconv` (config
    `find_dependency` 链解析)
  - 重依赖裸库名: neograph 系 (sqlite→acp→mcp→async→llm→core, 顺序敏感) +
    `yyjson` (经 PUBLIC `link_directories` (install/lib) 解析)
  - `OpenSSL::SSL` / `OpenSSL::Crypto` (find_dependency 链, include 随
    INTERFACE 传递 — 插件 include http_client.h 时 beast ssl 无需自行配置)
  - 条件 pkg-config 模块: `PkgConfig::hyperscan` (AGENTXX_ENABLE_HYPERSCAN) /
    `PkgConfig::uring` (AGENTXX_LINUX_IO_URING_SUPPORTED) — 插件项目须定义
    同名 target (plugins/CMakeLists.txt 已统一 pkg_check_modules, 与
    client/test 一致); hs_runtime 裸库名经 link_directories 解析
- **PUBLIC include**: install 头 / neograph deps (yyjson) / Boost 经
  `_AGENTXX_USAGE_TARGETS` 循环随 INTERFACE 导出
- **裁剪**: 静态库 (archive) 按目标文件提取 — 插件只链接实际引用的符号所属
  的 `.cpp.o`, 未使用的模块 (如插件不调用 http_client) 整个目标文件不被
  提取, 其重依赖符号不进入插件动态库 (9 个插件 DT_NEEDED 实测仅系统库)
- 链接依赖须用 imported target (Iconv::iconv / fmt::fmt ...) 而非裸库名
  (`charset` 裸名在插件项目无 link_directories 会链接失败); 裸库名仅用于
  install/lib 内存在的库 (neograph 系/yyjson/hs_runtime, 经 PUBLIC
  link_directories 解析); config 内 `find_dependency` 用小写包名 (`iconv`,
  其 config 文件为 `iconvConfig.cmake` 只匹配小写)

**插件接入** (每个插件 CMakeLists 动态分支):
```cmake
find_package(agentxx_util REQUIRED)   # 经 AGENTXX_INSTALL_DIR 的 config;
                                       # 依赖链 find_dependency 传递解析
target_link_libraries(${PLUGIN_NAME} PRIVATE agentxx_util)
```
内置合并编译分支: `target_link_libraries(${_target} PRIVATE agentxx_util)`
(仅传递编译宏/头路径; libagentxx 自身已链接 agentxx_util)。

**插件侧用法** (见 `agent/plugins/example_plugin/example_plugin.cpp`):
```cpp
#include "agentxx/util/string_util.h"  // util 头依赖 XX_IS_* 宏,
                                       // 链接 agentxx_util 后经 INTERFACE 自动获得
std::string b64 = agentxx::util::base64Encode(sv);  // 静态链入本插件副本
```
- `XX_IS_*` 平台/编译模式宏经 agentxx_util 的 PUBLIC 编译定义传播 (util 头
  内部 `#if XX_IS_WIN_D` 等分支依赖)
- 插件内 util 符号为本地隐藏符号 (实测 example_plugin 内 161 个 util 本地
  符号, 动态导出表仍仅 6 个入口), 与宿主 libagentxx 内同名符号互不冲突,
  版本可各自独立演进
- libagentxx (共享库) 内嵌全部 util 目标文件 (实测 3707 个 util 导出符号),
  与插件各自持有的副本互不可见

**libagentxx 侧链接注意** (仅构建者关注):
- `agentxx_static` 不链接 hyperscan/uring (静态库 PRIVATE 依赖导出为
  LINK_ONLY 且排于 INTERFACE 开头、位于 agentxx_util 之前, GNU ld 单遍
  扫描归档无法回溯解析 regex.cpp.o / asio io_uring 符号); 静态库消费者
  (client/test/benchmark) 均已自行 PRIVATE 链接 hyperscan/uring
- `agentxx_shared` 保留 PRIVATE 链接 (符号内嵌 .so, 运行时不依赖外部)

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

## 5. 热插拔机制

### 5.1 C++ 特有坑与对策

| 坑 | 现象 | 对策 |
|----|------|------|
| 在途调用悬垂 | dlclose 后插件代码段被卸载, 执行中 tool 崩溃 | 每插件 `std::atomic<size_t> inflight` (execute/hook/event 入口 RAII 递增); 卸载顺序: 摘除注册 (detachAll) → 等 inflight==0 (`waitInflightZero`, 超时放弃) → 调 unload 回调 → 实例析构时 dlclose; 双保险: ToolRegistry 以 shared_ptr 持有工具, 在途调用持有引用 |
| 卸载超时 | 慢/恶意插件回调长时间不返回 | `waitInflightZero` 带超时 (默认策略), 超时放弃卸载 (插件保持已 detach 状态, 可稍后重试), 不无限阻塞 io 线程 |
| static 全局析构 / 回调持有插件代码 | dlclose 触发静态析构顺序错乱; 已注册回调指向已卸载代码 | SDK 约定: 插件不得在 static 对象里注册回调; unload 回调内应主动反注册; 宿主 detachAll 二次校验注册表无残留才 dlclose |
| 编译器/STL ABI 不匹配 | 跨库传 `std::string`/异常崩溃 | C ABI 层零 C++ 类型跨边界; 异常不外泄 (宿主 vtable 兜底捕获 + error_out 字符串化) |
| 中间件运行中卸载 | 迭代器/下标失效 | disabled 位机制 (`WrapHandleBaseNode` 遍历跳过) + 轮末 `flushPendingCleanup` 物理摘除; start/end 配对按 start 实际执行下标回放, 运行中禁用不破坏配对 |
| Windows/Linux/Android 差异 | dlopen 三平台 API 不同 | `NativeLoader` 平台封装 (dlopen/dlsym/dlclose ↔ LoadLibraryW/GetProcAddress/FreeLibrary); Windows 走宽字符路径; Android 5.0+ 可用, 插件须按目标 ABI 编译 |
| 插件状态与会话隔离 | 插件跨 thread 共享状态; 执行线程(线程池)与 io 线程混杂 | 约定: 共享只读, 会话态用宿主 API (shareStore 线程安全 per-thread KV); 受信任才允许插件自管状态 |

### 5.2 中间件热插拔具体机制

- **加载**: 任意轮次边界 push 进 `handles` (下一节点执行即生效, by-index 活读取已安全)
- **卸载/禁用**: 不 `erase` (防执行中下标错位/悬垂), 置 `disabled` 位 — `WrapHandleBaseNode` 遍历时跳过; 轮次中禁用记入 `pendingCleanup_`, 轮末 `flushPendingCleanup` 物理摘除 (无轮次执行时立即摘除); enable 时按注册记录重建中间件并重新挂栈 (热启用不丢钩子)
- **State**: 按 thread_id 存于中间件实例内部 `states` map, 卸载即释放

### 5.3 PluginManager 线程模型

- 全局唯一; 挂在 `AgentContext` (`pluginManager` 成员, `BaseAgent::init` 装配)
- 所有插件注册/注销/启停操作在 io 线程串行执行 (满足 `assertIoThread` 无锁模型); 非 io 线程 (JS 线程/宿主线程池) 经 `setIoExecutor` 装配的 executor `postToIo`/`ioCallSync` 投递回 io 线程同步等待
- dlopen + entry 卸载到 `blockingPool` (异步), entry 内的注册动作由 vtable 内部自动回到 io 线程
- 轮次边界: `runConversationTurnAsync` 每轮开始 `flushPendingCleanup` (异常路径残留自愈) + `onTurnBegin`, 轮末 `onTurnEnd`; `hasRunningTurn()` 决定 disable 立即/延迟生效

---

## 6. JS 插件支持 (统一插件模型)

> 2026-08 重构: **所有插件统一为 C++ 插件**, 无 native/js/py 之分。脚本能力是插件内部实现的一部分 —— 经 manifest 依赖声明 (depends 引擎插件) + 能力调用 (invoke_capability 通用插件间通信) 把脚本代码交给 interpreter 引擎执行。宿主不特判任何脚本类型 (未来 py/lua 引擎注册 `interpreter.py` 能力即可被脚本插件依赖, 宿主零改动)。

### 6.1 引擎选型: QuickJS

| 引擎 | 体积 | 依赖 | Android | 许可 | 适合度 |
|------|------|------|---------|------|--------|
| **QuickJS** (quickjs-ng) | ~1-2MB | 无 | ✅ | MIT | ✅ 首选 |
| Duktape | ~500KB | 无 | ✅ | MIT | 仅 ES5.1, 能力偏弱 |
| V8/JSC | 数十 MB | 极重 | ❌ | 混合 | 不适合嵌入分发 |
| Node.js embed | 巨大 | 系统 Node | ⚠️ | MIT | 太重, 违背"零额外动态库依赖" |

QuickJS 与项目"仅依赖基本系统库、跨 Linux/Win/Android"目标完全吻合 (同 hyperscan/curl 一样走 `agent/third_party/` 编译基建; git submodule quickjs-ng, 静态库 `libqjs.a`)。

### 6.2 统一插件模型

```
PluginInstance (一切插件都是 C++ 插件)
  ├── depends         // 必选依赖 (插件名): 未安装→加载失败; 卸载/禁用时级联
  ├── optionalDepends // 可选依赖: 未安装仅警告; 插件运行时用互查 API 自适应
  ├── dlHandle        // 动态库句柄 (所有插件都有)
  └── 注册残留         // 工具/钩子/订阅/能力 —— 宿主 detachAll 统一清理
```

- **加载**: manifest 解析 → 依赖检查 → dlopen(entry) → entry()。entry 总是指向动态库; 脚本插件 = 薄 C++ 壳, 壳在 entry 里经能力调用加载脚本。
- **脚本执行 (插件间通信, 宿主不参与)**:
  - 引擎插件 (agentxx_javascript_engine) 注册能力 `interpreter.js` 并附带方法回调 (`register_capability_ex`): 方法 `load` (执行脚本) / `unload` (释放运行时)
  - 脚本插件的 C++ 壳: `get_own_info` 拿自身 name/path → 推导脚本文件 → `invoke_capability("interpreter.js", "load", {name, path})`
  - 引擎执行脚本; 脚本内 `agentxx.registerTool` 等经 **caller_host (壳实例)** 注册 → 注册残留归属壳插件, 宿主 detachAll 统一清理
  - 壳的 unload 回调里 `invoke_capability("interpreter.js", "unload", {name})` 通知引擎释放脚本运行时
- **多脚本/混合插件**: 一个壳插件可 invoke 多个语言引擎, 天然支持; 也可同时含原生 C++ 实现
- **卸载 (依赖图级联)**: 收集 `depends 含目标` 的插件 → 必选依赖者递归卸载 (先子后父, 保证引擎 dlclose 前无脚本插件残留) → detachAll → 等 inflight==0 → unload 回调 (壳通知引擎释放脚本运行时) + dlclose
- **禁用/启用**: 同样依赖图级联传播 (disable 先子后父; enable 先父后子; 被级联禁用的插件不置 `userDisabled`, 用户显式 enable 依赖方时可级联恢复; 用户手动禁用的插件不被 enable 级联恢复)
- **互查 API** (vtable): `list_plugins` / `get_plugin` / `get_own_info` → JSON (name/version/description/path/enabled/tools/capabilities/depends/optional_depends); JS 桥成 `agentxx.listPlugins()` / `agentxx.getPlugin(name)`

### 6.3 能力调用 (invoke_capability) 线程模型

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

- **死锁规避要点**: 提供者回调绝不能在 io 线程执行 —— 引擎的 load 会阻塞等待其 JS 线程, 而 JS 线程内脚本注册回调又要回 io 线程; 若回调在 io 线程执行则 io↔引擎互等死锁。查表与回调分离: 查表 (短) 在 io 线程, 回调 (长) 在调用方线程。

### 6.4 JS 引擎实现层次 (agentxx_javascript_engine)

```
agentxx_javascript_engine (C++ 插件, 用 plugin_api.h 编写, 链接 libqjs.a)
├── 初始化: register_capability_ex("interpreter.js", {load, unload})
├── JS 运行时: QuickJS 上下文 + 宿主桥接对象 (agentxx.*) + 专用 JS 线程
└── 执行桥:
     ├── host→js: args_json 传入 → JS 函数 → 结果 JSON 字符串返回
     ├── js→host: 注入全局 agentxx 对象 (桥到 host vtable)
     └── async 支持: drivePromise 循环 JS_ExecutePendingJob 直至 settle;
          无 job 时执行到期定时器 + 让出 (支持 await/setTimeout); 超时 interrupt 兜底
```

**JS 线程模型** (专用 JS 线程 + 任务队列 mutex+cv, 所有 QuickJS 操作集中单线程):

```
宿主线程池 ──postSync──▶ JS 线程 (execute 桥: 驱动 Promise 后返回结果)
io 线程    ──post──────▶ JS 线程 (钩子/事件回调: fire-and-forget, 不等待)
JS 线程    ──vtable────▶ io 线程 (注册/钩子/订阅/shareStore/tip: ioCallSync 同步等待)
JS 线程    ──callTool──▶ 本引擎 JS 工具: 同线程内联 (防自锁)
JS 线程    ──callTool──▶ 宿主插件工具: vtable call_tool (io 线程查表, 目标线程执行)
```

- 卸载安全: 脚本插件卸载 (deleted) 后已入队任务检查标志跳过; JSContext 由 JsPluginCtx 析构释放 (在途任务计数归零后); 引擎卸载 delete JsEngine → join 等待 JS 线程处理完已入队任务 → JS_FreeRuntime
- 定时器: setTimeout 注册到 timers_ (JS 线程), 任务循环 wait_until + drivePromise 空闲时内联触发 (execute 等待期间定时器也能工作)
- 栈检测: JS_NewRuntime 在 io 线程创建, **JS 线程入口必须 JS_UpdateStackTop** (否则栈溢出检测基于 io 线程栈指针误判)

**沙箱**: 内存 64MB / 栈 512KB / 单任务 60s 超时; 不引入 quickjs-libc (无 os/std 模块); 仅标准 ECMA + agentxx 桥白名单。

### 6.5 JS 插件示例 (用户视角)

```yaml
# plugins/weather/plugin.yaml —— 脚本插件的 C++ 壳 (统一插件模型)
# entry 指向壳动态库; 壳经能力调用把 plugin.js 交给引擎执行
name: weather
version: 1.0.0
entry: libweather.so
depends:
  - agentxx_javascript_engine
```

```js
// plugins/weather/plugin.js
agentxx.registerTool({
  name: "weather_query",
  description: "查询城市天气",
  parameters: { city: { type: "string", description: "城市名" } },
  execute: async (args, ctx) => {
    const page = await agentxx.callTool("agentxx_web_fetch",
                                        { url: `https://wttr.in/${args.city}` }, ctx.thread_id);
    return { summary: page.slice(0, 500) };
  }
});
// 钩子: point 0 = AGENTXX_HOOK_AGENT_START
agentxx.onHook(0, (info) => agentxx.log(2, "agent_start: " + JSON.stringify(info)));
```

**agentxx 桥 API** (每脚本插件独立 JSContext; 经 caller_host 挂到壳插件实例): `registerTool` / `unregisterTool` / `callTool` / `getShareStore` / `emitMessageTip` / `log` / `onHook` / `offHook` / `subscribe` / `unsubscribe` / `publish` / `listPlugins` / `getPlugin` + 全局 `setTimeout` / `clearTimeout` (定时器桥)。

### 6.6 依赖检查规则

| 场景 | 行为 |
|------|------|
| 必选依赖缺失 | 加载失败, 日志提示先加载谁 |
| 可选依赖缺失 | 加载成功, 警告 |
| 依赖环 (A→B→A) | 拒绝 (DFS 访问链检测, 防卸载级联死循环) |
| 卸载被依赖插件 | 必选依赖者级联卸载 (自动, 日志列出); 可选依赖者仅警告 |
| 引擎插件卸载 | 先级联卸载全部 depends 它的脚本插件, 再 dlclose 引擎 (binding 永不悬垂) |
| 加载顺序 | 拓扑排序 (Kahn): 配置顺序无关, 依赖者排在被依赖者之后 (loadConfiguredPlugins) |

---

## 7. client 侧插件系统

> 状态: 已实现 (2026-08): C ABI 契约 + ClientPluginManager + UI 适配器 + TUI/CLI 介入 + 跨端数据通道 (WirePluginDataUp) + 示例双端插件 + 集成测试 (模块 `client_plugins`, 50 项断言)
> 目标: 插件可介入 client 的 CLI / TUI / 未来 GUI, 与 agent 侧插件体系并存、互通

### 7.1 背景与核心决策

agent 侧插件 (工具/钩子/事件/能力) 已完备, 但 client (CLI/TUI) 是另一类宿主: 有自己的生命周期、线程模型与 UI 形态。 `WirePluginData` 仅提供 agent→client 的只读数据通道, 插件无法自定义展示、无法接收用户操作。

| 决策 | 结论 | 理由 |
|------|------|------|
| 插件能否直接操作 UI 组件树 | **不能**。扩展点是 UI 无关的"语义层" | 暴露 FTXUI 组件会把插件绑死在 TUI 框架上, GUI 无法复用, CLI 无从谈起 |
| ABI 组织 | **独立头 `client_plugin_api.h` + 独立入口符号 `agentxx_client_entry` + 独立版本号 (v3)** | client 侧扩展点迭代频繁; 独立符号集使 agent 侧 `plugin_api.h` 零改动, 旧插件不受影响 |
| 双端插件 | **同一动态库可导出两套入口**, 两个 PluginManager 各自 dlopen/装配 | codegraph 这类插件天然需要 "agent 侧提供工具 + client 侧展示进度" 的成对能力; dlopen 引用计数支持同库双实例 |
| 跨端通信 | **统一走 wire 协议**: 已有 `WirePluginData` (agent→client), 新增 `WirePluginDataUp` (client→agent) | 本地 Channel 模式与远程 WS 模式路径完全一致, 插件不感知部署形态 |
| 代码归属 | **UI 无关宿主层放 `agent/lib` (plugin/ 目录), TUI/CLI 适配器放 `agent/client`** | 未来 GUI 只需实现一个 `PluginUiAdapter` 即可复用宿主层 |

### 7.2 UI 无关语义层

插件对 UI 的全部控制力收敛为三件事:

1. **声明展示物**: 状态栏项 (id/text/align/order) / 侧边栏面板 (title + items: text(role: title/normal/hint)/progress/badge/action) / 侧边栏 Info 栏段落 (title + items, 同面板 items schema; 列表项按 Append 段样式 "|  xxx" 展示) —— 内容是宿主定义 schema 的 JSON, 不是组件;
2. **声明拦截点**: 斜杠命令 (如 `/usage`) —— 回调拿到参数, 返回动作 JSON;
3. **调用交互原语**: toast / 代发消息 / 请求取消 —— 命令式调用。

每个 UI 实现经 `ui_caps` 位图向插件声明自己支持什么, 不支持的注册项自动失败 (返回 NULL / 非 0), 插件自适应降级:

```c
#define AGENTXX_UI_CAP_STATUS_ITEM  (1u << 0) ///< 状态栏项
#define AGENTXX_UI_CAP_PANEL        (1u << 1) ///< 侧边栏面板
#define AGENTXX_UI_CAP_TOAST        (1u << 2) ///< toast 提示
#define AGENTXX_UI_CAP_KEYBIND      (1u << 3) ///< 自定义键位 (预留)
#define AGENTXX_UI_CAP_PROMPT       (1u << 4) ///< 模态询问 (预留)
#define AGENTXX_UI_CAP_MSG_DECOR    (1u << 5) ///< 消息装饰 (预留)
#define AGENTXX_UI_CAP_INFO_SECTION (1u << 6) ///< 侧边栏 Info 栏段落扩展
```

CLI 的 caps = `TOAST` (命令为输入管线一部分, 必然支持); TUI = `STATUS_ITEM | PANEL | TOAST | INFO_SECTION`; 未来 GUI 自行声明。

Info 栏段落 (register_info_section): 插件向侧边栏内置 Info tab 注入段落 (id 唯一, props `{"title": "..."}` 可省略), 内容经 update_info_section 更新 (items schema 与面板一致); 渲染在 Info 栏 Append 组件列表之后, 由 TUI 每帧从 UI 注册表快照读取 —— 适合把摘要/状态信息 (如 codegraph 索引状态、系统资源占用) 直接放进常驻 Info 栏, 而无需占用独立 tab。

**命令 execute 返回值 (动作 JSON, 宿主解释执行)**:
```json
{"action":"none"}                       已处理完毕
{"action":"send","text":"..."}          代为发送一条用户消息 (经 UI 排队语义)
{"action":"toast","text":"...","level":0|1|2}
```

**client 事件订阅** (`AgentxxClientEvent`, payload 均为 JSON 字符串):
`READY` (服务端就绪, {"uiCaps": n}) / `CONN_STATE` / `USER_INPUT` / `DELTA` (增量) / `TURN_END` / `SESSION_SWITCH` / `PLUGIN_DATA` (WirePluginData 转发, {"plugin","event","data"})。

### 7.3 跨端数据流 (本地/远程同路径)

```
agent 插件 publish("agentxx_codegraph.progress", {...})
  → SessionServerAgentIO (subscribePluginEvents) → WirePluginData ──(Channel | WS)──▶ client 端点
  → AgentIOBase::onPeerMessage → ClientEventSink::onPluginData → 分发到订阅 EVT_PLUGIN_DATA 的 client 插件

client 插件 send_plugin_data("rebuild_request", {...})
  → WirePluginDataUp ──(Channel | WS)──▶ SessionServerAgentIO
  → 总线 publish("plugin.client.{插件名}.{事件名}", data)  ← agent 侧插件 subscribe("client.{插件名}.{事件名}") 消费
  → 环回防护: "plugin.client." 前缀事件不转发回客户端 (subscribePluginEvents 跳过)
```

### 7.4 线程模型

| 线程 | 职责 | 插件可见性 |
|------|------|-----------|
| client io 线程 (端点所在 io_context) | 全部插件回调: entry 注册落地、事件 handler、命令 execute | 插件回调的唯一线程, 沿用"快速返回"约定 |
| UI 线程 (TUI FTXUI Loop) | 渲染语义元素 (从注册表快照读取)、产生交互 (点击/按键) | **从不直接调用插件代码**; 交互经 postToIo 反向投递; 组件树操作 (sidebar tab/toast) 经 postToUi 投递到 UI 线程 |
| agent io 线程 | 现有 agent 插件体系 | 与 client 插件完全隔离 |
| client 侧线程池 | dlopen + entry 执行 | 插件无感 (注册经 vtable ioCallSync 回 io 线程) |

安全不变式: 跨线程递给 UI 的数据一律是宿主已拷贝的 JSON 字符串 (UI 注册表 COW shared_ptr 快照); UI 线程持有的数据不引用插件内存 → UI 侧不参与 inflight 计数, 卸载只需等 io 线程 inflight 归零 (InflightGuard 模式)。

### 7.5 client 入口 ABI 摘要

```c
#define AGENTXX_CLIENT_PLUGIN_API_VERSION 3

typedef struct AgentxxClientPluginInfo {
    int api_version;                /* == AGENTXX_CLIENT_PLUGIN_API_VERSION */
    AgentxxPluginStringView name;   /* 全局唯一 (与 agent 侧插件共用命名空间) */
    AgentxxPluginStringView version;
    AgentxxPluginStringView description;
    uint32_t min_ui_caps;           /* 宿主 ui_caps() 不满足则拒绝加载 */
} AgentxxClientPluginInfo;

/* 入口符号 */
#define AGENTXX_CLIENT_SYMBOL_GET_INFO "agentxx_client_get_info"
#define AGENTXX_CLIENT_SYMBOL_ENTRY    "agentxx_client_entry"
#define AGENTXX_CLIENT_SYMBOL_UNLOAD   "agentxx_client_unload"
```

`AgentxxClientHostVtable` 分组 (与 agent 侧 vtable 同内存/日志/JSON 语义):

| 分组 | 函数 |
|------|------|
| 内存 | `alloc` / `free` / `strdup` |
| 能力协商 | `ui_caps` |
| 展示扩展 | `register_status_item` / `update_status_item` / `unregister_status_item`; `register_panel` / `update_panel` / `unregister_panel`; `register_info_section` / `update_info_section` / `unregister_info_section` (返回句柄, 卸载自动清理) |
| 输入扩展 | `register_command` / `unregister_command` (execute 返回动作 JSON) |
| 交互原语 | `show_toast` |
| 事件订阅 | `subscribe` / `unsubscribe` (卸载自动退订) |
| 会话上下文 | `get_client_state` ({"threadId","connState","model","models","isStreaming","uiCaps"}) |
| 会话操作 | `send_user_input` (与用户输入同排队语义) / `request_cancel` |
| 跨端数据 | `send_plugin_data` (client → agent, topic `client.{插件名}.{事件名}`) |
| 自描述 | `get_own_info` / `get_plugin_args` |
| 日志/JSON | `log` / `json_get_string` / `json_escape` |

### 7.6 装配 (mode_runners)

`setupClientPlugins<IoT, AdapterT>` 模板: 创建 `ClientPluginManager` → `setUiAdapter` (TUI/CLI 适配器) → `setThreadId` → TUI 额外 `setPluginManager` (须在 `start()` 之前, ctx_.pluginManager 在 UI 线程构建组件时读取) → `io->setEventSink(mgr)` → 调用方 co_await `loadConfiguredClientPlugins(plugins)`。

4 个模式全部接入:
- 本地 CLI (runLocalCliUnified): `CliPluginAdapter`; 输入循环发送前 `tryInvokePluginCommand` 拦截 "/" 命令
- 本地 TUI (runLocalTuiUnified): `TuiPluginAdapter`; 命令经 onSend 拦截
- 远程 CLI (runRemoteCli): `CliPluginAdapter`; 只加载 client 侧条目 (agent 条目被 sides 过滤, agent 在 server 进程)
- 远程 TUI (runRemoteTui): `TuiPluginAdapter`

### 7.7 实现要点 (以代码为准)

1. **事件上行 topic 前缀**: 服务端发布 `plugin.client.{插件名}.{事件名}` (与 agent 侧事件统一加 `plugin.` 前缀); 插件侧 subscribe 传 `client.{插件名}.{事件名}` 即可。
2. **环回防护**: `subscribePluginEvents` 跳过 `plugin.client.` 前缀 (client 上行事件不转发回客户端, 防止 client 插件收到自己发出的数据形成环回)。
3. **命令动作 JSON**: 支持 `send` / `toast` / `none`; `json_escape` 返回带引号的 JSON 字符串字面量 (与 agent 侧一致), 插件拼装动作 JSON 时不得再额外加引号。
4. **句柄生命周期**: disable 保留注册信息与句柄 (enable 重建); unload 时句柄存活到实例析构 (插件的 unload 回调可能主动反注册, 句柄必须有效)。
5. **远程 client 模式**: 只加载 client 侧条目; `io->onServerReady()` 在连接建立后调用 (TUI 覆写版幂等, 同时通知事件接收器)。
6. **命令注册不占 cap 位**: 命令属于输入管线 (stdin 行解析 / TUI onSend 拦截), 必然支持。
7. **min_ui_caps**: 插件可声明最低 UI 能力 (如必须面板); 宿主不满足则拒绝加载。

---

## 8. 内置插件清单

> 全部经 yaml `plugins` 段 path 配置加载 (不区分内置/外置, 无自动加载); 插件动态库输出到 `AGENTXX_EXEC_INSTALL_PREFIX` (与 agentxx_cli/agentxx_test 同目录)。

| 插件 | 目录 | 说明 |
|------|------|------|
| example_plugin | `agent/plugins/example_plugin/` | 示例插件 (双端): 3 工具 (echo/caller 互调/sleep 慢工具) + agent_start 钩子 + 事件订阅 + 能力声明 + client 入口 (状态栏/面板/Info 段落/命令/事件/跨端) |
| example_js | `agent/plugins/example_js/` | JS 示例插件 (C++ 壳 + plugin.js): 4 工具 (同步/async Promise/JS 内互调/宿主互调) + 钩子 + 事件订阅 + 互查 + 顶层异步初始化; depends: agentxx_javascript_engine |
| agentxx_javascript_engine | `agent/plugins/agentxx_javascript_engine/` | QuickJS 引擎插件 (链接 libqjs.a): 注册能力 `interpreter.js` (方法 load/unload); 专用 JS 线程 + 任务队列 + agentxx 桥 + 沙箱 |
| agentxx_codegraph | `agent/plugins/agentxx_codegraph/` | CodeGraph 代码分析: 8 工具 (search/context/callers/callees/impact/status/index/path); 索引进度/加载状态经 publish 事件 (topic `{插件名}.{事件名}`) 通知宿主, 由 SessionServerAgentIO 原样转发 WirePluginData; 插件 client 入口 (agentxx_client_entry) 订阅该事件并以侧边栏 Info 栏段落 (CodeGraph) 渲染索引进度/就绪状态 —— TUI 不再直接解析渲染插件载荷; 参数经 yaml plugins args (loadPaths/ignorePaths/loadCwd/useGitignore). 工具提示词默认值从 lib AgentPrompt 剥离迁移 (2026-08), entry 时经 `set_prompt` 写入宿主 toolPrompt (宿主已有条目则跳过, 尊重用户 yaml 覆盖) |
| agentxx_screen_capture | `agent/plugins/agentxx_screen_capture/` | 屏幕捕获 (仅 Windows): 工具 `agentxx_screen_capture` (单帧/全部屏幕/鼠标屏/流式推帧事件 topic `agentxx_screen_capture.frame`)、`agentxx_get_screen_frames` (获取当前屏幕图像帧数组，多屏每个屏幕一张，支持指定屏幕下标) |
| agentxx_computer_use | `agent/plugins/agentxx_computer_use/` | 键鼠控制 (仅 Windows): 工具 `agentxx_ui_control_keyboard_mouse`; plugin.yaml `depends: [agentxx_screen_capture]` (须同时配置加载) |
| agentxx_system_monitor | `agent/plugins/agentxx_system_monitor/` | 系统资源监控 (从 lib `src/expand/get_cpu_gpu_use` 拆分): 工具 `agentxx_get_system_core_info` (原内置工具迁移, lib 不再内置) + 能力 `agentxx.system_usage` (方法 query) + agent 侧周期采集线程 (每 5s 采样并 publish `agentxx_system_monitor.usage`, 定时/采集/发布完全位于插件内; 显示开关由 client `/sysinfo` 经跨端事件 `usage_enabled` 同步, 关闭期间跳过采集); 载荷为插件定义 schema 的 JSON 字符串, server 经 WirePluginData 原样转发; 插件 client 入口 (agentxx_client_entry) 订阅该事件以状态栏项渲染 CPU/RAM 占用 (快速一览) + 侧边栏 Info 栏段落渲染明细 (CPU/RAM/GPU) —— 采集实现与渲染完全隔离在插件内, lib wire 层不含任何系统资源 DTO |
| agentxx_audio_stream | `agent/plugins/agentxx_audio_stream/` | 音频流捕获 (从 lib `src/expand/audio_stream` 拆分; 仅 Windows WASAPI): 系统输出/程序输出/麦克风; 工具 `agentxx_audio_stream` (start/stop/status); 帧经 publish 事件推送 (topic `agentxx_audio_stream.audio`, base64 PCM); 非 Windows no-op |
| agentxx_text_selection_monitor | `agent/plugins/agentxx_text_selection_monitor/` | 系统级文本选择事件流 (从 lib `src/expand/text_selection_monitor` 拆分; 仅 Windows UIAutomation/WinEvent/CDP/剪贴板兜底): 工具 `agentxx_text_selection_monitor` (start/stop/status); 选中文本经 publish 事件推送 (topic `agentxx_text_selection_monitor.selection`); 非 Windows no-op |

---

## 9. 配置与分发

### 9.1 yaml `plugins:` 配置段

```yaml
plugins:
  - path: ./plugins/agentxx_codegraph   # 插件动态库路径 或 插件目录 (含 plugin.yaml 时按清单分派)
    enabled: true                        # 默认 true
    sides: auto                          # auto|agent|client (双端插件用; 默认 auto)
    args:                               # 插件参数 (宿主原样保存并整体传递, 不解析字段语义)
      loadPaths: [src]
      ignorePaths: [build, third_party]
```

- `sides` 语义: `agent` 仅 agent 侧加载; `client` 仅 client 侧加载; `auto` (同 `both`) 按导出符号自动决定 (client 侧: 有 `agentxx_client_entry` 才加载, 无则跳过并记日志)
- 相对路径按程序工作目录解析为绝对路径 (main.cpp resolvePath)
- 本地一体模式: agent 侧加载逻辑不变 (BaseAgent::init), client 侧由 mode_runners 加载同一配置 (经 sides 过滤); 双端插件同库被两个管理器各自 dlopen (引用计数互不影响)
- 远程 client 模式: 只加载 client 侧条目, agent 条目被过滤 (agent 在 server 进程)

### 9.2 plugin.yaml 清单 (插件目录)

```yaml
name: example_plugin          # 必填, 全局唯一
version: 1.0.0                # 可选
description: "..."            # 可选
entry: libexample_plugin.so   # 必填, 指向动态库 (所有插件统一为 C++ 插件)
depends: []                   # 可选, 必选依赖 (插件名)
optional_depends: []          # 可选, 可选依赖 (插件名)
```

宿主仅解析 name/entry/depends/optional_depends (YAML::LoadFile); 目录路径可直接传给 `loadPluginAsync` / yaml `plugins[].path`。直接给库路径时插件名按文件名推导 (`pluginNameFromPath`: libfoo.so → foo)。

### 9.3 构建开关

| 开关 | 默认 | 控制 |
|------|------|------|
| `AGENTXX_BUILD_PLUGINS` | ON | `agentxx_plugins_repo` (插件整体) 构建 |
| `AGENTXX_ENABLE_PLUGIN_JS` | ON | quickjs_repo + JS 插件 (javascript_engine/example_js) |
| `AGENTXX_ENABLE_PLUGIN_EXAMPLE` | ON | example_plugin / example_js |
| `AGENTXX_ENABLE_PLUGIN_CODEGRAPH` | ON | agentxx_codegraph (需 codegraph libs) |
| `AGENTXX_ENABLE_PLUGIN_COMPUTER_USE` | ON | agentxx_computer_use |
| `AGENTXX_ENABLE_PLUGIN_SCREEN_CAPTURE` | ON | agentxx_screen_capture |
| `AGENTXX_ENABLE_PLUGIN_SYSTEM_MONITOR` | ON | agentxx_system_monitor |
| `AGENTXX_ENABLE_PLUGIN_AUDIO_STREAM` | ON | agentxx_audio_stream |
| `AGENTXX_ENABLE_PLUGIN_TEXT_SELECTION` | ON | agentxx_text_selection_monitor |

每个插件子目录自带 CMakeLists.txt, 可独立构建 (`cmake -B build -S agent/plugins -DAGENTXX_INSTALL_DIR=...`); 插件仅依赖纯 C ABI 头 (libagentxx include 目录), **不链接 libagentxx**。QuickJS 经 `quickjs_repo` ExternalProject 构建 install 到 `AGENTXX_INSTALL_DIR`。

---

## 10. 示例插件

### 10.1 C++ 插件骨架 (与现网 plugin_api.h v6 一致)

```cpp
// my_tool_plugin.cpp —— 编译为 libmy_tool_plugin.so, 无需链接 libagentxx
#include "agentxx/plugin/plugin_api.h"

static const AgentxxHost* g_host = nullptr;

// execute 回调运行在宿主线程池; 返回字符串必须经宿主分配 (AGENTXX_STRDUP)
static char* my_exec(void* ud, AgentxxPluginStringView args_json,
                     AgentxxPluginStringView thread_id, AgentxxPluginStringView tool_call_id,
                     char** err_out) {
    (void)ud; (void)args_json; (void)thread_id; (void)tool_call_id; (void)err_out;
    if (!g_host) return nullptr;
    return g_host->vtable->strdup("{\"ok\": true}");
}

// 可选: 元信息 (加载前校验; 未导出则跳过)
extern "C" const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        AGENTXX_SV("my_tool_plugin"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV("My first plugin tool"),
    };
    return &info;
}

// 必需: 入口 (宿主线程池调用; 注册动作宿主自动投递回 io 线程)
extern "C" int agentxx_plugin_entry(const AgentxxHost* host, void** plugin_ctx) {
    (void)plugin_ctx;
    g_host = host;
    AgentxxToolSpec spec{};
    spec.name            = AGENTXX_SV("my_tool");
    spec.description     = AGENTXX_SV("My first plugin tool");
    spec.parameters_json = AGENTXX_SV(R"({"type":"object","properties":{}})");
    spec.execute         = my_exec;
    return host->vtable->register_tool(host, &spec);   // 0 = ok
}

// 可选: 卸载通知 (宿主已先自动反注册; 插件侧约定主动清理)
extern "C" void agentxx_plugin_unload(void* plugin_ctx) {
    (void)plugin_ctx;
    if (g_host) {
        g_host->vtable->unregister_tool(g_host, AGENTXX_SV("my_tool"));
        g_host = nullptr;
    }
}
```

### 10.2 插件目录布局

```
plugins/
└── my-plugin/
    ├── plugin.yaml        # 清单: name/version/entry/depends/optional_depends
    ├── libmy_plugin.so    # (native) 编译产物
    ├── plugin.js          # (js) 脚本入口 (由 C++ 壳经能力调用交给引擎)
    └── assets/            # 附带资源 (SKILL.md / memory 文件 / prompt 模板)
```

### 10.3 双端插件 (agent + client 入口共存)

同一动态库导出 `agentxx_plugin_entry` (agent 侧工具/钩子) 与 `agentxx_client_entry` (client 侧状态栏/面板/Info 段落/命令/事件/跨端), 两个 PluginManager 各自 dlopen/装配, 实例状态独立, 互通一律走 wire。参考 `agent/plugins/example_plugin/example_plugin.cpp`。

---

## 11. 安全与权限

- 插件默认最小权限; C++ 插件视为受信 (等同内建代码), manifest 需来源提示 (签名校验三期)
- JS 插件沙箱: 无 IO、无 `require`/`process`, 仅显式注入的 `agentxx.*` 桥接白名单 + 全局 setTimeout/clearTimeout; 内存 64MB / 栈 512KB / 单任务 60s 超时 (interrupt handler 兜底)
- 插件执行路径仍经过现有 `PermissionMiddleware` (文件路径 HIL 拦截天然生效); 插件工具默认走未注册规则兜底
- 单个插件加载/执行失败仅记日志, 不影响 agent 主流程 (`catchErrorAsync` 语义)
- client 侧: `send_user_input` 等价于代用户发言 — 动作 send 一律经 UI 既有排队语义 (流式中进 pendingInputs), 不绕过 UI 状态机; 可见性声明: 插件可订阅 EVT_USER_INPUT / EVT_DELTA (全量消息), 文档明确告知; UI 不越权: 语义层原语无"读屏/注入按键"能力; 命令只拦截以 `/` 开头的输入, 未命中命令照常作为普通消息发送; 跨端通道频率由插件控制, 服务端不解析载荷语义
- 依赖声明即信任边界: 引擎插件卸载前级联卸载全部 depends 它的脚本插件; 依赖环拒绝加载

---

## 12. 与现有代码的接入点

### 12.1 agent 侧

| 位置 | 改动 |
|------|------|
| `agent/lib/include/agentxx/plugin/plugin_api.h` | C ABI 契约 (纯 C, v6) |
| `agent/lib/include/agentxx/plugin/tool_registry.h` + `src/plugins/tool_registry.cpp` | 动态工具注册表 (shared_ptr 持有; 静态名冲突检测; appendDefinitions) |
| `agent/lib/include/agentxx/plugin/plugin_manager.h` + `src/plugins/plugin_manager.cpp` | PluginManager / PluginInstance / PluginTool / PluginMiddlewareHandle / CapabilityRegistry / NativeLoader |
| `agent/lib/src/nodes/toolcall.cpp` | 工具查找: 静态列表未命中 → `ToolRegistry::find` 回退 (~10 行, 地基) |
| `agent/lib/src/nodes/modelcall.cpp` | `build_params` 追加 `toolRegistry->appendDefinitions` (热注册后下一轮对模型可见) |
| `agent/lib/include/agentxx/agent/context.h` | `AgentContext` 增 `toolRegistry` / `pluginManager` 成员 |
| `agent/lib/src/agent/base_agent.cpp` | `init()` 装配 ToolRegistry/PluginManager + setIoExecutor + 静态工具名收集 (必须在 own_tools 之前) + 加载配置插件; `runConversationTurnAsync` 轮次边界登记 (flushPendingCleanup/onTurnBegin/onTurnEnd) |
| `agent/lib/include/agentxx/agent/config.h` + `config_loader.cpp` | `plugins:` 配置段 (path/enabled/sides/args; PluginConfig/PluginSide) |
| `agent/lib/include/agentxx/middlewares/middleware.h` + `nodes/wrap_handle.h` | `BaseMiddlewareHandleInterface::disabled` 位; start/end 跳过 disabled + end 按 start 记录回放 |

### 12.2 client 侧

| 位置 | 改动 |
|------|------|
| `agent/lib/include/agentxx/plugin/client_plugin_api.h` | client C ABI 契约 (纯 C, v1) |
| `agent/lib/include/agentxx/plugin/client_plugin_manager.h` + `src/plugins/client_plugin_manager.cpp` | ClientPluginManager + ClientPluginInstance + ClientUiRegistry + PluginUiAdapter + hostVtable |
| `agent/lib/include/agentxx/agent/io/client_event_sink.h` | 端点 → 插件事件通道 (7 回调) |
| `agent/lib/include/agentxx/agent/io/agent_io.h/.cpp` | `setEventSink` + `emitEventSink`; onServerReady 默认通知 ready |
| `agent/lib/include/agentxx/agent/io/agent_io_transport.h` | `WirePluginDataUp` 变体; `WirePluginData` |
| `agent/lib/include/agentxx/agent/io/wire_protocol.h` | `MsgType::PluginData` / `PluginDataUp` + make/fromJson |
| `agent/lib/src/agent/io/ws_io_transport.cpp` | serialize/deserialize 支持各 wire 变体 |
| `agent/lib/src/agent/io/session_server_agent_io.cpp` | onPeerMessage: WirePluginDataUp → 总线 publish `plugin.client.{插件名}.{事件名}` (环回跳过); subscribePluginEvents 转发 `plugin.` 前缀事件 |
| `agent/client/src/config_loader.cpp` | 解析 `plugins` 段 (path/enabled/sides/args) |
| `agent/client/include/agentxx-client/io/plugin/plugin_ui_adapter.h` | UI 适配器接口 (uiCaps/注册/更新/toast/sendPluginMessage/sendPluginData) |
| `agent/client/include/agentxx-client/io/tui/tui_plugin_adapter.h` + `io/stdio/cli_plugin_adapter.h` | TUI/CLI 适配器实现 |
| `agent/client/src/io/tui/agent_tui.h/.cpp` | EventSink 通知点 (WirePluginData 原样转发); 命令管线 (onSend 拦截 `/`); addPluginPanelTab/removePluginPanelTab/renderPluginPanel; sendPluginUserInput/sendPluginDataUp; uiToast |
| `agent/client/src/io/tui/components/status_bar.cpp` | 插件状态栏项渲染 (左/右分组, order 排序) |
| `agent/client/src/io/tui/tui_sidebar_content.cpp` | Info 侧边栏 (Planning/Append 组件 + 插件 Info 段落); 系统资源与 CodeGraph 渲染已剥离到插件 client 侧 (经 register_info_section) |
| `agent/client/src/mode_runners.cpp` + `main.cpp` | 4 个模式装配 ClientPluginManager + 适配器 (setupClientPlugins 模板); 命令拦截 (tryInvokePluginCommand) |
| `agent/test/core/test_plugins.*` | agent 插件测试模块 `plugins` (140 项断言: 加载/工具执行/互调/钩子/事件/禁用启用/卸载/冲突/列表/JS 引擎/级联/拓扑/超时卸载竞态/shutdownAll/sides 过滤/args 传递/publish 禁用) |
| `agent/test/core/test_client_plugins.*` | client 插件测试模块 `client_plugins` (69 项断言: 加载/UI 注册表/事件分发/命令/跨端/禁用启用/卸载/订阅扩容与派发中动态订阅/loadConfiguredClientPlugins sides 过滤与 args 传递) |

---

## 13. 实现状态与偏差

### 13.1 实现历史

| 阶段 | 内容 | 状态 |
|------|------|------|
| 一期 (C++ 插件框架) | C ABI 契约 / ToolRegistry / PluginManager / 钩子 / 事件 / 热插拔生命周期 / plugins 配置段 / 示例插件 / 集成测试 | ✅ 已实现 |
| 二期 (JS 支持) | QuickJS 集成 / agentxx_javascript_engine (interpreter.js 能力) / JS 沙箱 + Promise 桥 / 示例 JS 插件 / 测试 | ✅ 已实现 |
| 统一插件模型 (2026-08) | 所有插件统一为 C++ 插件 (壳 + 能力调用委派); 依赖图级联卸载/禁用; 互查 API; 拓扑排序加载 | ✅ 已实现 |
| 内置插件化 (2026-08) | codegraph / screen_capture / computer_use / system_monitor / audio_stream / text_selection_monitor 从 lib 拆分独立 | ✅ 已实现 |
| client 侧插件系统 | client_plugin_api v2 (状态栏/面板/Info 段落/命令/事件/跨端) / ClientPluginManager / UI 适配器 / 双端插件 / WirePluginDataUp | ✅ 已实现 |
| 三期 (生态) | Wire 远程热管理 / TUI 插件管理面板 / 签名校验 | ⏳ 待实现 |

### 13.2 与设计原稿的偏差 (实现为准)

0. **client 侧订阅生命周期 (2026-08 修复)**: `ClientPluginInstance::subscriptions` 为
   `vector<shared_ptr<Subscription>>`; `ClientSubscriptionImpl::sub` 为强引用 ——
   多次订阅扩容/退订 erase/派发中动态订阅/unload 回调内退订均不悬垂
   (原实现按值存储 + 裸指针 → UAF, 见 §7 缺陷修复记录)
1. **跨 CRT 堆边界**: vtable 增加 `alloc/free/strdup`; `AgentxxToolSpec::execute` 返回的字符串与 `error_out` 均须经宿主分配 (`AGENTXX_STRDUP` 宏)。示例插件用 `g_host->vtable->strdup`, 不得用 `strdup`/`malloc`。
2. **字符串参数统一字符串视图 (v6)**: 所有跨边界"字符串参数/字段" (`AgentxxPluginInfo`/`AgentxxToolSpec` 字段、execute/hook/event 回调参数、vtable 的 name/topic/json 等参数) 从 `const char*` 改为 `AgentxxPluginStringView` (data+size, 只读借用, 不要求 NUL 结尾, 生命周期仅覆盖本次调用); 仅"宿主分配"的返回值与 `error_out` 保持 `char*` (host->alloc)。插件侧构造: 字面量用 `AGENTXX_SV("...")`, 运行时字符串用 `agentxx_plugin_sv(str.data(), str.size())`。宿主侧 `PluginTool` 构造时拷贝字符串字段 (name/description/parameters_json 指向插件侧视图, 不依赖插件内存存活)。
3. **工具冲突检测**: `ToolRegistry::setStaticToolNames` 由 init 在 `own_tools` 之前收集内置工具名 (注意: own_tools 会 move 空 tools vector, 收集必须在其之前), 插件工具与内置/MCP 工具同名注册失败。
4. **订阅回调签名**: `subscribe(topic, handler, ud)` 载荷为 JSON 字符串; 宿主侧经 `EventBus::get<std::string>` 桥接, topic 自动加 `plugin.` 前缀; 另增 `publish` (异步投递)。
5. **call_tool 语义**: 仅可调用插件注册的工具 (不暴露宿主内置工具); 查表在 io 线程短临界区, 目标工具 execute 回调在【调用方线程】执行 —— 线程池/JS 线程内调用不阻塞 io 线程; io 线程内调用会阻塞 (罕见场景, 插件应避免); 目标插件由宿主引用计数保活。
6. **钩子回调签名**: `fn(user_data, point, node_input_json, out_json, error_out)`; `node_input_json` 为节点输入摘要 (thread_id/point/messages_count/has_tool_calls)
7. **卸载彻底性**: `unloadAsync` 等 inflight==0 **且**进行中轮次结束后立即 `eraseMiddleware`, 并从 pendingCleanup 移除; disable 仍走轮末摘除 (无轮次时立即摘除)。`AgentContext::~AgentContext` 先调 `pluginManager->shutdownAll()`。2026-08 起 `PluginMiddlewareHandle` 持实例**弱引用** (与实例互不持有, 根除循环引用), `pendingCleanup_` 改为中间件弱引用记录 (flush 不依赖实例存活, 加载失败路径也能摘除)。
8. **插件名**: 未导出 `get_info` 时按库文件名推断 (libfoo.so → foo)。
9. **中间件 disabled 位**: 新增于 `BaseMiddlewareHandleInterface` (普通中间件恒 false); `WrapHandleBaseNode` start/end 循环均跳过 disabled 项; end 阶段按 start 实际执行下标回放, 运行中禁用不破坏配对 (M7 回归)。
10. **能力调用 (invoke_capability)**: 原稿仅规划"能力注册表互查", 实现中扩展为通用插件间通信通道 —— 能力可附带方法回调 (`register_capability_ex`), 调用方提供者回调在调用方线程执行 (查表与回调分离防死锁, 见 6.3)。
11. **禁用级联语义**: 被级联禁用的插件不置 `userDisabled` (用户显式 enable 依赖方时可级联恢复); 用户手动禁用的插件不被 enable 级联恢复 (M8 回归)。
12. **shutdownAll 顺序**: 按依赖图逆序 (先子后父): 脚本插件先卸载 (unload 回调需经 invoke_capability 通知引擎), 引擎插件最后 dlclose; 不等在途回调 (调用方须保证无在途插件回调, 进程退出/上下文销毁路径满足)。
13. **client 事件上行 topic**: 服务端发布 `plugin.client.{插件名}.{事件名}` (而非原稿的 `client.{...}`), 与 agent 侧事件统一加 `plugin.` 前缀。

### 13.3 设计原稿遗漏、实现中补上的关键点

0. **公共设施提取 (2026-08)**: 新增 `plugin_common.h/.cpp` 集中两侧共享基建
   (pluginNameFromPath / parsePluginManifest / resolvePluginEntryPath /
   topoSortPlugins / collectReverseRequiredDeps / ioCallSync / XX_PLUGIN_CATCH_*),
   agent/client 两个管理器删除本地重复实现, 防止行为漂移
   (历史上 client 侧多次"漏掉 agent 侧已修的问题")。
1. **client 侧 entry 线程模型 (2026-08 修复)**: `ClientPluginManager::loadNativeAsync`
   entry 调用卸载到内部线程池 (原实现 io 线程同步执行, 违背头文件契约且阻塞
   client io 事件循环; 与 agent 侧 blockingPool 语义对齐)。
2. **client 侧卸载状态机 (2026-08 修复)**: unloadAsync 顺序改为
   detach → 等 inflight → unload 回调 → 从表移除 (析构 dlclose);
   超时保持已 detach 状态可重试 (原实现先 erase 后等待, 超时后句柄泄漏且
   不可重试); shutdownAll 改为依赖图级联先子后父 (原实现按 map 字母逆序,
   依赖顺序无保证); `~ClientPluginInstance` 统一负责 dlclose。
3. **client 侧配置 args (2026-08 修复)**: `loadNativeAsync` 增加 cfg 参数,
   args 随加载直接写入实例 (原实现恒为空对象, get_plugin_args 恒返回 "{}");
   agent 侧同步改为 cfg 直接传入, 废除"事后按路径推导名回查配置"
   (manifest name 与目录名不一致时原实现静默丢失)。
4. **agent 侧加载失败清理 (2026-08 修复)**: entry 返回非 0 时, 已注册的中间件
   必须摘除/登记待轮末摘除 —— 原实现漏清理, 中间件↔实例循环引用导致实例
   不析构 → dlHandle 永不 dlclose (模块泄漏)。配套: `PluginMiddlewareHandle`
   持实例弱引用 (与实例互不持有, 根除循环引用); `pendingCleanup_` 改为
   中间件弱引用记录, flush 不依赖实例存活。
5. **agent 侧 sides 过滤 (2026-08 修复)**: `loadConfiguredPlugins` 跳过
   `sides == Client` 的配置项 (原实现漏过滤, agent 侧会误加载纯 client 插件)。
6. **vtable 收紧 (2026-08)**: `publish` 校验插件 enabled (禁用插件不再外发事件);
   `postToIo` 无 executor 兜底路径加警告日志; `pluginNameFromPath` 扩展名剥离
   改 rfind (my.plugin.so → my.plugin) 且仅剥过扩展名才去 lib 前缀;
   `PluginTool` 名称单一来源 (spec.name, 删除构造双参数)。
7. **client 侧杂项 (2026-08)**: `json_get_string`/`json_escape` 改为纯函数直执行
   (原实现绕道 io 线程); Auto 侧入口探测与正式加载合并为一次 dlopen
   (原实现探测 dlopen→close 后正式加载再 dlopen); waitInflightZero 指数退避
   (10/20ms → 1s 上限); detachAll 卸载路径断订阅句柄链 (unload 回调内退订安全)。
8. **example_plugin Info 段落 JSON 修复 (2026-08)**: `on_client_turn_end` 把
   `json_escape` 的返回值 (带引号的 JSON 字面量) 嵌入模板 `{}` 产生非法 JSON
   (`"text":"Turns: "1""`), 宿主解析失败静默丢弃 (Info 段落恒为空); 改为
   整段文本放入 escape 调用。
- **LLM 请求侧工具 schema 静态性**: 原稿只改造了 `toolcall.cpp` 执行侧查表, 但 `ModelCallWrapNode::build_params` 每轮从静态 `tools_` 组装工具定义发给 LLM —— 若不追加插件工具, 模型永远看不到新工具。实现中在 build_params 经 `ToolRegistry::appendDefinitions` 追加 (热注册后下一轮 modelcall 即对模型可见)。
- **执行中工具悬垂**: 原稿靠插件 inflight 计数, 实现中 `ToolRegistry::find` 返回 shared_ptr 保活 (与 execTool 的裸指针路径并存, 插件工具经 shared_ptr 传入), 双保险。
- **注册时序**: dlopen 在阻塞线程池, 但 entry 的注册动作必须回到 io 线程 (无锁模型); `loadNativeAsync` 在 io 线程协程内完成 dlopen (卸载到线程池) + entry 同步调用。
- **卸载超时**: `waitInflightZero` 带超时, 超时放弃卸载 (慢/恶意插件不无限阻塞 io 线程)。
- **拓扑排序加载**: 配置顺序无关 (Kahn 排序, 依赖者排在被依赖者之后), 避免配置顺序导致必选依赖缺失; 无进展 (环/缺失) 项附后由依赖检查报错。
- **插件配置 args**: yaml `plugins` 条目 args 宿主原样保存并整体传递 (`get_plugin_args`), 参数语义由插件定义 (如 agentxx_codegraph 的 loadPaths/ignorePaths)。
- **禁用立即/延迟生效**: `hasRunningTurn()` 判定 —— 轮次中禁用延迟到轮末摘除中间件, 无轮次立即摘除; 每轮开始 flushPendingCleanup 自愈异常路径残留。
- **vtable 便捷 API**: `json_get_string` / `json_escape` (替代插件手写 JSON 解析, 对转义/嵌套可靠); `get_config` / `get_tool_prompt` (宿主配置访问, 插件注册工具时生成与内置工具一致的动态描述)。

### 13.4 实现中踩过的坑 (JS 引擎, 供参考)

1. **JS_SetProperty\* 是 move 语义**: 属性消费传入值, 调用方不得再 Free (否则 double free → ASan heap-use-after-free)
2. **JS_PromiseResult 返回新引用**: 直接返回, 不得再 Dup (泄漏 → JS_FreeRuntime 断言 gc_obj_list 非空 abort)
3. **JS_IsFunction 是双参** (ctx, val), 其他 JS_Is\* 单参 (quickjs-ng 与 bellard 差异)
4. **JS_NewCFunction2 是 6 参** (含 JSCFunctionEnum cproto), magic 版函数签名 `(JSContext*, JSValueConst, int, JSValueConst*, int)` 需强转 JSCFunction*
5. **JS_JSONStringify 的 space 参数是 JSValue** 不是 int
6. **yaml description 值含 `: ` 需引号包裹** (illegal map value)
7. **沙箱无 setTimeout**: 示例插件的 `new Promise(r => setTimeout(r, 10))` 抛 ReferenceError → promise reject → 工具返回 `{}` (异常对象 JSON 序列化); 实现中已由全局 setTimeout 桥解决

---

## 14. 尚未实现

- 插件签名校验 (manifest 来源提示/签名)
- CapabilityRegistry 插件互操作规范 (如第三方提供 `interpreter.python`)
- 插件钩子的 out_json 修改能力、permission 联动注册 (插件工具默认走 PermissionMiddleware 的未注册规则兜底)
- client 插件二期能力: prompt 模态 / input_filter / 消息装饰 / keybind / 设置项注册 / 热重载
- 模型 Provider 动态注册接口
