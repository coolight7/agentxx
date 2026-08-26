# Agentxx 插件设计

> 待实现: Wire 远程热管理 / TUI 插件管理面板 / 签名校验
> 关联: [design.md](design.md)
> 目标: 原生 C++ 插件 + 脚本插件 (由 C++ 插件承载), 支持热插拔、强自定义
> 本文以当前源码为准 (plugin_api.h v1 / client_plugin_api.h v1, COM 风格接口表架构), 设计原稿与实现偏差见 [13. 实现状态与偏差](#13-实现状态与偏差)

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
- **统一异步操作模型 (2026-08 重构)**: 工具/钩子/能力方法一律为 start/poll/cancel 异步三件套, 宿主 op_driver 在 io 线程驱动并与内置工具协程交错执行; 同步插件经纯 C 垫片零成本迁移 (见 4.2.2)。
- **现有线程卸载基建**: `blockingPool` + `offload` (调用方持有 cancel_flag) 保留为慢同步操作的官方逃生通道; 超时经 `asyncWithTimeout`。

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
| `plugin_api.h` | `include/agentxx/plugin/` | 纯 C ABI 契约头 (v1, COM 风格接口表; 唯一跨版本稳定接口) |
| `client_plugin_api.h` | `include/agentxx/plugin/` | client 侧纯 C ABI 契约头 (v1, 独立符号集; 同架构) |
| `plugin_iface_helper.h` | `include/agentxx/plugin/` | 插件侧 C++ header-only 便捷层 (AgentIfaces/ClientIfaces 一次查询聚合; 非跨边界 ABI) |
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

### 4.2 plugin_api.h 契约要点 (v8, 以实际头文件为准)

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

### 4.2.1 会话资源贡献 (v8 新增: Skill / Memory / MCP)

插件可向宿主贡献三类会话组件, 由 agent-io 管线加载并经 `appendComponentInfo`
上报客户端 (TUI/CLI 自动可见), 两种通道:

**声明式** (plugin.yaml 可选段, 键名与主配置 yaml 一致; 相对插件目录解析):

```yaml
name: my_plugin
entry: libmy.so
skill:            # 追加 skill 扫描目录 (含 SKILL.md 或其父目录)
  - skills
memory:           # 追加 memory 上下文文件 (内容注入系统提示词)
  - assets/NOTES.md
mcp:              # 追加 MCP server (与主配置 yaml mcp 列表项同构)
  - namespace: weather
    url: https://mcp.example.com/sse
    timeout: 60   # 秒 (可选)
```

**运行时** (entry 内经 vtable 实时注册):

```c
vt.register_skill_dir(host, AGENTXX_SV("/abs/path/skills"));
vt.register_memory_file(host, AGENTXX_SV("/abs/path/notes.md"));
vt.register_mcp_server(host, AGENTXX_SV(
    R"({"namespace":"calc","url":"https://...","timeout":30})"));
```

**语义约定**:

| 规则 | 说明 |
|------|------|
| 冲突 | **主程序 yaml 配置优先**: 与主配置已有 skill 目录/memory 文件路径/MCP 命名空间重复 → 拒绝 + WARN; 插件之间先到先得; 同 owner 重复注册幂等成功 |
| 失败不生效 | 声明式资源在 entry 成功后才应用 (`applyDeclaredResources`) —— 插件加载失败其声明资源一律不生效 |
| 所有权 | 生效资源按插件名记录于 `AgentResourceApplier` (单一具体实现); 卸载 → 全部摘除; disable → 摘生效留记录; enable → 按记录恢复 (MCP 重新连接) |
| MCP 异步 | `register_mcp_server` 仅查重登记后立即返回; 连接协程派发 io executor, 各阶段检查 stale (注销竞态安全); 连接完成后工具动态进入 ToolRegistry (下一轮 modelcall 对模型可见); 连接失败仅记日志, 命名空间随即释放可重试 |
| 缓存失效 | 中间件扫描列表变更递增资源纪元 (epoch), 各线程状态缓存据此在下次轮次重建 |
| 装配范围 | `CodeAgent::initMiddleware` 装配 applier; BaseAgent 场景无中间件 → 资源 API 返回非 0 (不支持) |

JS 桥对应 API (脚本插件内): `agentxx.addSkillDir(path)` /
`removeSkillDir(path)` / `addMemoryFile(path)` / `removeMemoryFile(path)` /
`addMcpServer({namespace|name, url, timeout?})` / `removeMcpServer(ns)`。

示例插件: `agent/plugins/example_resources/` (双通道示范)。

**工具定义** (统一异步操作模型; 见 4.2.2):
```c
typedef struct AgentxxToolSpec {
    AgentxxPluginStringView name;            ///< 须全局唯一 (与内置/MCP 工具同名注册失败)
    AgentxxPluginStringView description;
    AgentxxPluginStringView parameters_json; ///< JSON Schema 字符串
    /// 异步三件套 (start/poll/cancel; 宿主 io 线程驱动, 与内置工具协程交错):
    void* (*execute_start)(void* user_data,
                           AgentxxPluginStringView args_json,
                           AgentxxPluginStringView thread_id,
                           AgentxxPluginStringView tool_call_id,
                           const AgentxxOpNotify* notify,
                           char** error_out);    ///< 启动 (非阻塞; 内联完成可返回 NULL)
    int   (*execute_poll)(void* user_data, void* op);     ///< 推进 (可空 = 只等通知)
    void  (*execute_cancel)(void* user_data, void* op);   ///< 协作式取消请求 (可空)
    void* user_data;
    long  default_timeout_ms;                 ///< 0 = 不限制
    int   flags;                              ///< AGENTXX_TOOL_FLAG_AUTO_SUMMARY 等
} AgentxxToolSpec;
```
- start/poll/cancel 全部在【宿主 io 线程】被驱动调用 (非阻塞快速返回约定);
  AgentxxOpNotify.done 可从插件任意线程回调 (宿主投递回 io 唤醒等待)
- 快同步工具在 start 内直接算完并 notify → 返回 NULL (内联完成);
  慢同步经 `sched->offload` 委托阻塞池; 真实并发 IO 经 poll 寄生驱动
  (`plugin_poll_loop.h`) 在宿主 io 线程交错执行 (见 4.2.3)
- 会话取消/超时 → 宿主调 execute_cancel + 后台收割协程推进至真正终结
  (inflight 保活转移, 卸载安全语义不变)

**钩子点** (与宿主 7 个中间件钩子一一对应; 同为异步三件套规格):
```c
typedef struct AgentxxHookSpec {
    AgentxxHookPoint point;
    void* (*hook_start)(void* user_data, AgentxxHookPoint point,
                        AgentxxPluginStringView node_input_json,
                        const AgentxxOpNotify* notify, char** error_out);
    int   (*hook_poll)(void* user_data, void* op);     ///< 可空
    void  (*hook_cancel)(void* user_data, void* op);   ///< 可空
    void* user_data;
} AgentxxHookSpec;
/// 注册: hooks->register_hook(host, &spec); 注销: unregister_hook(host, point)
/// - 每插件每钩子点至多一个 (重复注册覆盖); 慢钩子经 offload 不再违反快速返回
```

**核心 vtable + COM 风格接口表** (v1 全量架构; 所有表函数内部捕获异常, C ABI
边界无异常外泄):

核心 vtable 极简且【契约冻结】, 仅 4 个成员:

| 成员 | 说明 |
|------|------|
| `alloc` / `free` / `strdup` | 跨 CRT 堆边界唯一分配通道 (任意线程可调用) |
| `query_interface(host, iid)` | 按稳定 IID 字符串查询接口表; 未实现返回 NULL (安全失败) |

其余全部宿主能力按稳定 IID 查询独立接口表获取 (每表首字段为该表自身
`version`, 独立演进; 表内不支持的子能力成员为 NULL, 调用前判空):

| IID (宏) | 结构体 | 成员 |
|------|------|------|
| `agentxx.agent.tools` | `AgentxxToolsIface` | `register_tool` / `unregister_tool` / `call_tool` (互调: 查表 io 线程短临界区, 目标 execute 在调用方线程, 目标插件 shared_ptr 保活) |
| `agentxx.agent.hooks` | `AgentxxHooksIface` | `register_hook` / `unregister_hook` (热插拔, 轮次边界生效) |
| `agentxx.agent.events` | `AgentxxEventsIface` | `subscribe` / `unsubscribe` / `publish` (topic 自动加 `plugin.` 前缀; publish 异步投递且拒绝禁用插件) |
| `agentxx.agent.capabilities` | `AgentxxCapabilitiesIface` | `register_capability` / `register_capability_ex` / `unregister_capability` / `has_capability` / `invoke_capability` (`_ex` 附带方法回调 = 通用插件间通信通道, 如 `interpreter.js`; 提供者回调在【调用方线程】执行防死锁) |
| `agentxx.agent.scheduler` | `AgentxxSchedulerIface` | `is_io_thread` / `post_to_io` / `add_timer` / `cancel_timer` / `offload` (定时器 io 线程触发; offload 阻塞池执行 + inflight 保活, 调用方持有 cancel_flag) |
| `agentxx.agent.session` | `AgentxxSessionIface` | `get_share_store` / `emit_message_tip` (仅 io 线程) |
| `agentxx.agent.plugins` | `AgentxxPluginsIface` | `list_plugins` / `get_plugin` / `get_own_info` (JSON; name/version/path/enabled/tools/capabilities/depends/optional_depends) |
| `agentxx.agent.config` | `AgentxxConfigIface` | `get_config` / `get_plugin_args` / `get_tool_prompt` (dataDir/projectRoot/platform; yaml args 宿主不解析) |
| `agentxx.agent.prompt` | `AgentxxPromptIface` | `get_prompt` / `set_prompt` (完整提示词读写; 卸载自动回滚写入) |
| `agentxx.agent.json` | `AgentxxJsonIface` | `json_get_string` / `json_escape` (线程安全纯函数, 替代手写解析) |
| `agentxx.agent.log` | `AgentxxLogIface` | `log` (0=trace..4=error, 线程安全) |
| `agentxx.agent.resources` | `AgentxxResourcesIface` | `register_skill_dir` / `unregister_skill_dir` / `register_memory_file` / `unregister_memory_file` / `register_mcp_server` / `unregister_mcp_server` / `get_own_resources` (会话资源贡献见 4.2.1; 所有权按插件记录, 卸载摘除/禁用摘生效留记录/启用恢复) |

插件侧便捷设施: `plugin_iface_helper.h` 提供 `AgentIfaces::query(host)` /
`ClientIfaces::query(host)` 一次性查询聚合 (header-only, 非跨边界 ABI);
第三方插件可不用它而直接调 `query_interface` (纯 C 路径不受影响)。

**入口符号** (dlsym; v1 重构 — create/destroy 实例对):
```c
#define AGENTXX_PLUGIN_SYMBOL_GET_INFO "agentxx_plugin_get_info"  ///< 可选: 元信息 (纯静态, 加载前校验)
#define AGENTXX_PLUGIN_SYMBOL_CREATE   "agentxx_plugin_create"    ///< 必需: 创建一个实例
#define AGENTXX_PLUGIN_SYMBOL_DESTROY  "agentxx_plugin_destroy"   ///< 可选: 销毁对应实例
```
- `create` 可重入: 同一动态库可被同进程内不同宿主各自调用 N 次, 每次产出
  一个完全独立的存活实例 (COM 类工厂语义)。历史 `entry/unload` 命名已废弃,
  旧插件经符号查找失败被拒载 (无兼容路径)
- client 侧对称: `agentxx_client_get_info` / `agentxx_client_create` /
  `agentxx_client_destroy`

**多实例三铁律 (v1 契约)**:
1. 禁止任何可变全局 / 函数级 static 缓存 (常量表除外) —— 动态库 dlopen 引用
   计数共享代码段与 .data/.bss, 全局状态会被所有并存实例共享;
2. 一切实例状态只能存于 `*plugin_ctx` 指向的堆块; 一切注册回调必须设置
   `spec.user_data = ctx`, 回调内经其恢复自身实例;
3. 接口表查询结果存入实例上下文 (`AgentIfaces::query` 结果不得缓存到静态区)。

守卫异常日志经捕获实例上下文的闭包传入 `plugin_guard::guardCall`
(固定签名函数指针无法携带实例 → 仅限 get_info 纯静态边界使用)。
同步垫片 (`plugin_tool_sync.h`) 适配器为调用方内嵌存储 (PluginCtx 成员),
随实例生死 —— 反复加载/多实例零泄漏、零进程级登记。

**版本策略 (双层)**: 全局 `AGENTXX_PLUGIN_API_VERSION` (=1) 只覆盖核心契约
(核心 vtable 形状 + Info 结构 + 入口符号 + 共享类型); 宿主精确匹配门禁,
api_version 不匹配的插件直接拒绝加载 (仅拒绝不崩溃), **无历史版本兼容路径**
(v9 及以前的巨型核心 vtable 已废弃)。接口表各自携带 version 独立演进:
新增能力 = 定义新接口表或在表内追加成员并递增该表版本, 全局版本号不动、
其他插件与旧宿主二进制不受影响。

### 4.3 宿主侧适配 (vtable → 现有强类型世界)

- `register_tool` → 包装成 `PluginTool` (`XXToolBase` 子类), `execute_async` 经
  **op_driver (`op_driver.h`)** 在宿主 io 线程驱动三件套并与内置工具协程交错
  执行 —— 不再卸载线程池; 会话 CancelToken 联动 `execute_cancel`, 超时经
  `asyncWithTimeout`, 放弃路径由收割协程接管 inflight 保活直至插件真正终结。
  字符串字段 (name/description/parameters_json) 构造时从 string_view 拷贝进成员。
- `register_hook` → `PluginMiddlewareHandle`, 七个覆写经 op_driver 驱动钩子
  三件套后**push 进现有 `handles` vector** → 栈式执行、错误重抛、per-thread
  state 全部复用, 不改 `wrap_handle.h` 引擎逻辑。注册即创建中间件句柄
  (懒创建, 一个插件一个), disable 置 disabled 位, 轮末摘除。
- `subscribe` → 直接转发 `EventBus` (topic 加 `plugin.` 前缀, 载荷 `std::string`), 插件卸载自动退订。
- `call_tool` / `invoke_capability` → **异步原语**: `call_tool_async` /
  `invoke_capability_async` 返回 `AgentxxHostOp` 句柄 (目标插件三件套由宿主在
  io 线程后台驱动, 调用方任意线程 poll/take/cancel/free); 同名阻塞便捷版内部
  自旋轮询实现, **io 线程调用 fail-fast 拒绝** (防阻塞死锁)。

#### 4.2.2 统一异步操作模型 (v1 核心)

> 目标: 插件与主程序始终同线程协作 —— 插件操作与内置工具的 asio 协程在
> 宿主 io 线程上交错执行, 访问会话数据天然单线程安全; 同时保持"仅实现同步
> 代码"的插件编写体验, 且不强制导入任何异步库。

- **原语**: 被调方操作 = `start`(非阻塞启动, 可内联完成) + `poll`(推进,
  返回建议延迟 ms 或 DONE) + `cancel`(协作式取消请求); 完成经
  `AgentxxOpNotify.done(status, payload)` 恰好一次上报。方向无关:
  宿主→插件 (工具/钩子/能力方法) 由宿主 op_driver 驱动; 插件→宿主
  (`call_tool_async`/`invoke_capability_async`) 返回 `AgentxxHostOp` 由插件驱动。
- **驱动器** (`agent/lib/include/agentxx/plugin/op_driver.h`, lib 内部):
  等待形态 `awaitPluginOp()` (工具/钩子协程挂起等待 + 会话取消 watcher +
  看门狗慢调用告警) 与句柄形态 `makeHostOp()` (后台收割式驱动 + 线程安全
  sink)。放弃路径 (超时/取消提前退出) 自动转入收割协程推进至真正终结,
  inflight 保活随之转移 —— 卸载必须等操作终结的语义不变。
- **同步垫片** (`plugin_tool_sync.h`, 纯 C header-only): 
  `agentxx_register_inline_tool` (快同步内联完成) / 
  `agentxx_register_sync_tool` (慢同步委托 offload, execute 多收一个
  cancel_flag 形参) / `agentxx_register_sync_hook`; 传统同步函数零改动迁移。
- **poll 寄生驱动** (`plugin_poll_loop.h`, C++ header-only): 见 4.2.3 ——
  协程异步插件的第三姿势, 与内置工具同线程交错执行。

#### 4.2.3 poll 寄生驱动 (协程异步插件, 2026-08)

> 目标: 插件用任意**可步进(embed)**的异步框架编写协程, 经三件套嫁接到宿主
> io 线程协作式交错执行 —— 与内置工具完全同线程同语义, 零额外线程、零数据
> 竞争。`example_sleep` 即该模式的纯状态机特例; 内置 execute_command /
> websearch 插件为其 asio 实现参考。

- **机制**: 插件实例持有无线程寄生事件循环 `PollLoop` (`asio::io_context`,
  不 run() 不开线程); start 把工作协程 co_spawn 到其上立即返回; poll 调
  `io.poll()` 非阻塞推进一步 (执行全部就绪 handler), 按返回值建议宿主让出
  (0 = 本轮有 handler 执行) 或小睡 (>=1 ms); cancel 置 Job.cancelFlag 由
  协程在阶段边界轮询退出; done 在宿主 io 线程的 poll 推进内上报。
- **事件不丢失**: Linux epoll level-triggered / Windows IOCP 完成包排队,
  两次 poll 之间到达的事件下次 pollOnce 必然取得; 定时器唤醒延迟上界 =
  idleHintMs (默认 15ms, 每次 poll 为微秒级非阻塞取包不忙等)。
- **硬性约束**: 工作协程每次就绪段 (两次挂起间的同步代码) 必须 ~100ms 内
  回到挂起点 (宿主看门狗阈值), 协程体内禁止阻塞调用; CPU/阻塞密集段应切片
  或改走 offload (B 型混合)。协程内调用宿主 io 线程约束接口表 (get_work_dir /
  is_cancelled 等) 安全 —— 宿主 ioCallSync 检测到已在 io 线程时内联直执行。
- **适用判据 (三型选型)**:
  | 操作性质 | 姿势 |
  |---|---|
  | A 快同步 <~1ms (JSON/内存/时间) | inline 垫片 |
  | B 阻塞库调用 (磁盘遍历/sqlite/cmark/CPU 密集), 无异步 API | sync 垫片 (offload 池) |
  | C 真异步 IO (socket/子进程管道) | poll 寄生驱动 |
  | 不可步进的异步框架 (自带线程 runtime, 如 Go/阻塞 SDK) | 自有线程 + 手写三件套 notify (JS 引擎模式) 或 offload |
- **多实例契约**: PollLoop 为 PluginCtx 成员随实例生死; 宿主保证 destroy 前
  inflight==0 → 析构时无在途工作协程; 协程帧持 io shared_ptr 兜底析构竞态。


- **线程契约**: start/poll/cancel 仅 io 线程调用 (单次 ≤~1ms, 宿主看门狗
  >100ms WARN); notify/HostOp 方法任意线程可调; 阻塞便捷版 call_tool/
  invoke_capability 禁止 io 线程调用。

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
     extern "C" AGENTXX_PLUGIN_EXPORT int agentxx_plugin_create(const AgentxxHost* host, void** plugin_ctx);
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

**内置名单门控 (AGENTXX_PLUGIN_BUILTIN_LIST, 2026-08)**:

- `AGENTXX_ENABLE_PLUGIN_BUILTIN=ON` 时经 `AGENTXX_PLUGIN_BUILTIN_LIST`
  指定哪些启用插件内置合并:
  - 未设置 / 空 / `"all"`: 全部启用插件内置合并 (默认)
  - 分号或逗号分隔的插件名列表 (如
    `"agentxx_filesystem,agentxx_planning"`): 仅名单内插件内置合并,
    **名单外启用插件仍编译为独立动态库 (混合模式)** —— 运行期动态库优先
    dlopen 加载, 入口文件缺失时回退内置注册表, 两形态共存无冲突
- 实现要点: plugins/CMakeLists.txt 以 `agentxx_load_plugin()` macro 统一装载
  (macro 无新作用域, 子目录 PARENT_SCOPE 收集直接回本目录), 装载前按名单
  判定并 set 目录变量 `_AGENTXX_ADD_AS_BUILTIN`, 各插件子目录开头据此选择
  内置 OBJECT 收集分支 / 独立动态库分支
- 名单内插件的 `plugin.yaml` 资源拷贝为各子目录自包含 ALL custom_target
  (`<插件名>_builtin_resources`; 原跨目录 add_custom_command(OUTPUT) +
  父目录 DEPENDS 聚合的写法不会把规则注册进构建图 —— "No rule to make
  target", 已废弃)

**从 lib 内置工具迁移的编程工具插件 (默认全部内置编译)**:

- `agentxx_planning` (agentxx_planning_write) / `agentxx_filesystem`
  (list/read/write/edit/glob/grep) / `agentxx_execute_command`
  (bash/windows) / `agentxx_string` (html2markdown/regexp) /
  `agentxx_system` (get_current_datetime) / `agentxx_websearch`
  (web_search/web_fetch/web_fetch_markdown) / `agentxx_rag_search`
  —— 同名同行为, 实现于各插件 `*_impl.h` (头文件-only 纯函数,
  测试直测同一实现); 开关 `AGENTXX_ENABLE_PLUGIN_<NAME>`
- 宿主新增接口表支撑: `agentxx.agent.config` v2 (+`get_work_dir` 会话
  工作目录) / `agentxx.agent.model` (主模型与 websearch/rag 配置 JSON) /
  `agentxx.agent.cancel` (会话取消轮询) / `agentxx.agent.planning`
  (planning state 写入); 见 plugin_api.h 与 PluginManager 对应 vtable 实现
- 独立动态库链接注意: agentxx_util/libhs/libcrypto 等静态归档成员相互引用
  (如 libhs 引用 CRYPTO_memcmp、util 引用 html2md), 链接器单遍扫描无法自解析;
  各插件在链接行**尾部以绝对路径重复提供**相关归档兜底 (target 形式会被
  CMake 去重/重排, 裸路径不会)

**构建**:

- 顶层 superbuild 加 `-DAGENTXX_ENABLE_PLUGIN_BUILTIN=ON` 后, 由
  `agentxx_lib_repo` (libagentxx) 经 `add_subdirectory(../plugins)` 收集
  名单内插件内置合并; **名单外插件仍由本目录编译为独立动态库** (混合模式;
  全量内置时跳过 `agentxx_plugins_repo` 动态库构建)
- 冷构建注意: 内置模式下插件子目录的 `find_package(agentxx_util)` 经
  `if (NOT TARGET agentxx_util)` 门控 —— lib 目标树内 util 已存在 (尚未
  install), 直接复用; 独立构建插件时才经安装 config 导入
- 各插件子目录 CMakeLists.txt 内置分支 (AGENTXX_ENABLE_PLUGIN_BUILTIN):
  - 源文件编译为**独立 OBJECT 库** (`abp_<插件名>`, per-plugin 编译定义/
    包含路径互不影响), 目标文件经 `$<TARGET_OBJECTS:...>` 合并进
    agentxx_shared/agentxx_static
  - 入口符号经编译定义改名, 避免多插件合并编译冲突: `agentxx_plugin_create`
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
- client 侧插件 (agentxx_client_create) 仍走独立动态库构建 (client 可执行
  程序按需外置), 内置模式不产出 client 插件库; `client_plugins` 测试在
  内置模式下跳过

### 4.7 接口协商 (字符串接口集; 全量 COM 式接口表, v1)

> 目标: 适配不同 client 宿主 (cli / tui / gui, 甚至第三方 app) 支持的插件
> 接口各不相同 —— 同一插件目录在能力齐备的宿主全量启用, 在缺能力的宿主
> 明确跳过或降级, 而非半失效运行。

**不对称协商的关键事实**: server (agent-io) 只有 libagentxx 一个实现,
agent 侧接口集 ≡ 核心契约 + 全部标准接口表 IID (版本匹配即全集), 无需真正
协商; 协商的主战场在 client 侧。机制因此做成通用对称代码路径, 但实际起
作用的门禁集中在 client 侧 (第三方 agent 宿主未来可直接复用)。

**接口命名** (稳定字符串契约, 常量见 `plugin_common.h` 的
`plugin_interfaces` 命名空间):

| 名称 | 归属侧 | 说明 |
|---|---|---|
| `agentxx.agent.core` | agent | 元接口: 宿主实现完整核心契约 + 标准接口表全集 (libagentxx 即此类) |
| `agentxx.agent.tools` / `agentxx.agent.hooks` / `agentxx.agent.events` / `agentxx.agent.capabilities` / `agentxx.agent.scheduler` / `agentxx.agent.session` / `agentxx.agent.plugins` / `agentxx.agent.config` / `agentxx.agent.prompt` / `agentxx.agent.json` / `agentxx.agent.log` / `agentxx.agent.resources` | agent | COM 风格接口表 IID (= `AGENTXX_IFACE_AGENT_*` 宏); 插件可按实际查询的表精确声明 require |
| `agentxx.client.ui` | client | 展示扩展接口表整体 (`AGENTXX_IFACE_CLIENT_UI`) |
| `agentxx.client.status_item` | client | 状态栏项 (映射 agentxx.client.ui 非空成员) |
| `agentxx.client.panel` | client | 侧边栏面板 |
| `agentxx.client.toast` | client | toast 提示 |
| `agentxx.client.keybind` (预留) | client | 自定义键位 |
| `agentxx.client.prompt_modal` (预留) | client | 模态询问 |
| `agentxx.client.msg_decor` (预留) | client | 消息装饰 |
| `agentxx.client.info_section` | client | 侧边栏 Info 栏段落 |
| `agentxx.client.command` | client | 斜杠命令输入管线 |
| `<vendor>.<name>` | 双方 | 第三方私有接口 (不得使用保留前缀 `agentxx.`); 宿主不认识即不支持, 安全失败 |

> 命名规范: `agentxx.` 为本项目内置接口的**保留命名空间** —— 全部内置接口
> 名以 `agentxx.` 开头 (`agentxx.agent.*` / `agentxx.client.*`), 第三方插件
> 私有接口用 `<vendor>.<name>` 且不得占用该前缀。

**三层设计**:

1. **声明层**: 插件 `plugin.yaml` 可选段 (见 9.2):

   ```yaml
   interfaces:
     require:  [agentxx.agent.core, agentxx.client.command]  # 缺失任一(按前缀过滤后) → 该侧跳过加载
     optional: [agentxx.client.info_section]         # 缺失仅警告, 插件注册时自降级
   ```

   同一清单可同时声明两侧接口, 前缀决定归属: `agentxx.agent.*` 仅 agent 侧
   检查、`agentxx.client.*` 仅 client 侧检查、无前缀/`<vendor>.*` 两侧都检查
   (`sideCaresAboutInterface`)。

2. **校验层** (框架强制, dlopen 前后两道):
   - **require 门禁**: 宿主支持集 = `PluginUiAdapter::supportedInterfaces()`
     声明的名字集合 (单一事实来源); `checkInterfacesForSide` 比对 require
     未满足 → **跳过加载** (INFO 日志 + `skippedPlugins()` 记录原因供展示层
     排查; 非错误 —— 同一插件目录服务多种宿主是预期情况); optional 缺失仅
     警告。
   - **符号意图预检**: require 声明了某侧接口却未导出该侧入口符号
     (`requiredEntrySides`) → 明确报错, 声明意图优先于 sides==Auto 的静默
     容忍。
   - agent 侧走同一套公共函数, 支持集 = `{agentxx.agent.core}` + 全部标准接口表 IID
     (`PluginManager::agentHostSupportedInterfaces`)。

3. **决策层** (插件自主): 插件 entry 内经 `query_interface(name) != NULL`
   判单接口 (或经 `AgentIfaces::query` 聚合判空), 或读 EVT_READY payload 与
   `get_client_state` 的 `"interfaces": ["agentxx.client.panel", ...]` 字符串数组,
   自行决定注册哪些功能。

**全量 COM 式接口表** (`query_interface`, v1 架构):

- 核心契约冻结: 核心 vtable 仅 `alloc/free/strdup/query_interface` 四成员
  —— 一切宿主能力一律定义为独立接口表 (纯 C 结构体, 首字段恒为自身
  `version`), 经 `query_interface(host, iid)` 分发, 与全局版本号解耦,
  不再强制全部插件重编译。
- agent 侧已定义 12 张标准表 (`AGENTXX_IFACE_AGENT_*`, 见 4.2); client 侧
  已定义 7 张 (`agentxx.client.ui/events/session/wire/self/json/log`)。"agentxx.client.ui"
  (`AgentxxClientUiIface`) 承载状态栏/面板/Info 段落/命令/toast。
- 表内不支持的子能力成员为 NULL 函数指针, 插件调用前判空。典型用法:

  ```c
  auto ui = (const AgentxxClientUiIface*)
      host->vtable->query_interface(host, AGENTXX_SV(AGENTXX_IFACE_CLIENT_UI));
  if (ui && ui->register_panel) { /* ... */ }
  ```

  或经便捷层一次查询聚合:

  ```cpp
  auto ifs = agentxx::plugin::ClientIfaces::query(host);
  if (ifs.ui && ifs.ui->register_panel) { /* ... */ }
  ```

- 第三方精简宿主只需实现核心 + 想支持的接口表 (不查询的表返回 NULL 即可);
  新增能力永不修改核心结构。

**与版本门禁的关系** (重要): `api_version` 精确匹配门禁保留且不被本机制
替代 —— 核心结构是 C 结构体, 老宿主+新插件按新偏移读字段是 UB, 这只能靠
版本门禁挡住; 接口协商只解决"功能子集"维度, 接口表机制解决"新增能力不动
全局版本"维度。

**跨端感知与上报** (约定事件, 均以伪插件名 `agentxx_host` 承载):
- server → client: `server_plugins` 载荷为结构化对象数组
  `[{"name","version","interfaces":[...]},...]`; `WireHelloAck.plugins`
  同构。client 插件经 get_client_state 的 `"agentPlugins"` 字段查询对端
  可用性与声明的接口。
- client → server: `client_interfaces` (三期6) —— client 在 READY 时上报
  本宿主接口集 `{"sessionId","interfaces":[...]}`。**controller 与 client
  是 1:N** (同会话可多 client 接入/重连), 服务端不存储该上报, 仅转发到
  agent 总线; agent 侧插件订阅 `agentxx_host.client_interfaces`, 按事件
  到达感知各 client 快照并自适应 (如 emit_message_tip 在无 toast 接口的
  宿主上降级)。

**v1 重构行为变化** (相对历史版本, 不兼容):
- 历史巨型核心 vtable (plugin_api ≤v9 / client_plugin_api ≤v4 的全部能力
  成员) 整体废弃并拆分为命名接口表; 旧版插件因 api_version 门禁被直接拒绝
  加载, 无兼容路径。
- 位图方案不复存在; 最低接口要求由清单 `interfaces.require` 声明 (宿主
  加载前门禁)。
- `register_command` 按 `agentxx.client.command` 能力名门禁 (映射 agentxx.client.ui 表非空
  成员); CLI/TUI 适配器均声明该能力, 行为不变。
- 运行时接口探测统一为 `query_interface` 判空 (`has_interface` 已移除,
  等价于查询结果非 NULL)。
- 展示/命令/toast 经 "client.ui" 接口表访问 (仓库内全部插件已迁移, 可作
  参考实现)。

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
- 轮次边界: `runTurnAsync` 每轮开始 `flushPendingCleanup` (异常路径残留自愈) + `onTurnBegin`, 轮末 `onTurnEnd`; `hasRunningTurn()` 决定 disable 立即/延迟生效

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

**agentxx 桥 API** (每脚本插件独立 JSContext; 经 caller_host 挂到壳插件实例): `registerTool` / `unregisterTool` / `callTool` / `getShareStore` / `emitMessageTip` / `log` / `onHook` / `offHook` / `subscribe` / `unsubscribe` / `publish` / `listPlugins` / `getPlugin` + 全局 `setTimeout` / `clearTimeout` (定时器桥) + `addSkillDir` / `removeSkillDir` / `addMemoryFile` / `removeMemoryFile` / `addMcpServer({namespace|name, url, timeout?})` / `removeMcpServer(ns)` (v8 会话资源贡献, 见 4.2.1)。

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
| ABI 组织 | **独立头 `client_plugin_api.h` + 独立入口符号 `agentxx_client_create` + 独立版本号 (v1, COM 风格接口表)** | client 侧扩展点迭代频繁; 独立符号集与独立核心版本使两侧契约互不影响; 接口表各自带 version 演进 |
| 双端插件 | **同一动态库可导出两套入口**, 两个 PluginManager 各自 dlopen/装配 | codegraph 这类插件天然需要 "agent 侧提供工具 + client 侧展示进度" 的成对能力; dlopen 引用计数支持同库双实例 |
| 跨端通信 | **统一走 wire 协议**: 已有 `WirePluginData` (agent→client), 新增 `WirePluginDataUp` (client→agent) | 本地 Channel 模式与远程 WS 模式路径完全一致, 插件不感知部署形态 |
| 代码归属 | **UI 无关宿主层放 `agent/lib` (plugin/ 目录), TUI/CLI 适配器放 `agent/client`** | 未来 GUI 只需实现一个 `PluginUiAdapter` 即可复用宿主层 |

### 7.2 UI 无关语义层

插件对 UI 的全部控制力收敛为三件事:

1. **声明展示物**: 状态栏项 (id/text/align/order) / 侧边栏面板 (title + items: text(role: title/normal/hint)/progress/badge/action) / 侧边栏 Info 栏段落 (title + items, 同面板 items schema; 列表项按 Append 段样式 "|  xxx" 展示) —— 内容是宿主定义 schema 的 JSON, 不是组件;
2. **声明拦截点**: 斜杠命令 (如 `/usage`) —— 回调拿到参数, 返回动作 JSON;
3. **调用交互原语**: toast / 代发消息 / 请求取消 —— 命令式调用。

每个 UI 实现经 `supportedInterfaces()` 声明自己支持的接口名集合
(`plugin_interfaces` 常量), 宿主据此做子能力门禁与加载门禁;
不支持的注册项自动失败 (返回 NULL / 非 0), 插件自适应降级。展示/命令/toast
经 "client.ui" 接口表访问 (`AgentxxClientUiIface`, 经 `query_interface`
获取; 表内不支持子能力成员为 NULL):

```c
auto ui = (const AgentxxClientUiIface*)
    host->vtable->query_interface(host, AGENTXX_SV(AGENTXX_IFACE_CLIENT_UI));
if (ui && ui->register_panel) { /* ... */ }   /* 成员判空 = 能力判空 */
```

CLI 声明 `{agentxx.client.toast, agentxx.client.command}` (命令为输入管线一部分, 必然支持);
TUI = `{status_item, panel, toast, info_section, command}`; 未来 GUI 自行声明。

Info 栏段落 (register_info_section): 插件向侧边栏内置 Info tab 注入段落 (id 唯一, props `{"title": "..."}` 可省略), 内容经 update_info_section 更新 (items schema 与面板一致); 渲染在 Info 栏 Append 组件列表之后, 由 TUI 每帧从 UI 注册表快照读取 —— 适合把摘要/状态信息 (如 codegraph 索引状态、系统资源占用) 直接放进常驻 Info 栏, 而无需占用独立 tab。

**命令 execute 返回值 (动作 JSON, 宿主解释执行)**:
```json
{"action":"none"}                       已处理完毕
{"action":"send","text":"..."}          代为发送一条用户消息 (经 UI 排队语义)
{"action":"toast","text":"...","level":0|1|2}
```

**client 事件订阅** (`AgentxxClientEvent`, payload 均为 JSON 字符串):
`READY` (服务端就绪, {"interfaces":[...],"sessionId"}) / `CONN_STATE` / `USER_INPUT` / `DELTA` (增量) / `TURN_END` / `SESSION_SWITCH` / `PLUGIN_DATA` (WirePluginData 转发, {"plugin","event","data"})。

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

#### 7.3.1 宿主约定事件 (host convention events, 2026-08)

跨进程/分设备部署时, 插件的 client 端与 agent 端可能只有一侧加载 (另一侧未编译/
未配置/版本不同)。宿主以伪插件名 `agentxx_host` 发布两个约定事件, 用于状态快照
与对端可用性检测, 双向通道对缺失对端的行为由"静默丢弃"变为可观测:

| 事件 (topic `plugin.agentxx_host.{事件}`) | 载荷 | 发布时机 | 用途 |
|------|------|---------|------|
| `client_attached` | `{"sessionId":"..."}` | subscribePluginEvents 注册成功时 + 每次 handleHello 握手完成后 (重复发布无害, 快照重发幂等) | 双端插件收到后**重发当前完整状态快照** —— 修复"status 等一次性事件先于端点订阅/客户端接入而丢失 → 客户端 UI 永久滞留初始占位 (如 codegraph 'wait for index')" |
| `server_plugins` | `{"plugins":[{"name","version","interfaces":[...]},...]}` | handleHello 握手完成后 (与 HelloAck.plugins 同数据的事件形态) | client 插件查询服务端已加载的 agent 侧插件及其声明的接口, 对端缺失/缺能力时降级提示 |
| `client_interfaces` (三期6) | `{"sessionId":"...","interfaces":["client.toast",...]}` | client 端点 READY 时由 ClientPluginManager 上报 (经 WirePluginDataUp) | 服务端不存储 (controller:client = 1:N), 仅转发到 agent 总线; agent 侧插件订阅后按事件到达感知各 client 接口集并自适应 (如 emit_message_tip 在无 toast 接口的宿主上降级) |

- 两个事件不以 `client.` 开头, 会正常转发到客户端 (client 插件同样可订阅消费,
  如据 `server_plugins` 自适应降级)
- **单侧缺失行为** (此前完全静默): server 收到 `WirePluginDataUp` 但 agent 侧无
  同名插件 → 每插件名冷却限频 `XX_LOGW`; client 收到 `WirePluginData` 但本端无
  任何插件订阅 EVT_PLUGIN_DATA → 每插件名一次 `XX_LOGW`
- **对端可用性查询**: `get_client_state` 返回新增字段 `agentPlugins` (数组;
  空 = 服务端未提供该信息, 不得据此断言"未加载"); 参考实现: system_monitor
  `/sysinfo` 在 agent 侧插件缺失时 toast 警告"开关仅本地生效"

#### 7.3.2 部署矩阵 (分进程/分设备时的预期行为)

| 部署情形 | 行为 |
|------|------|
| 双端均加载同一双端插件 | 完整功能 (工具 + UI + 跨端互通) |
| 仅 server 加载 (client 未装/未配) | 工具可用; 无对应 UI; server 日志提示上行数据无消费者 (若有), client 日志提示收到的插件事件无订阅者 |
| 仅 client 加载 | UI 可注册但数据源缺失 (周期型事件自愈为空态); 上行操作 (如 /sysinfo) 经 agentPlugins/toast 明确提示"仅本地生效"; server 日志警告上行被丢弃 |
| 两端都加载但配置不同 | 各端独立生效 (args 不跨端同步); 状态以 agent 侧为准经事件同步 |

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
#define AGENTXX_CLIENT_PLUGIN_API_VERSION 4

typedef struct AgentxxClientPluginInfo {
    int api_version;                /* == AGENTXX_CLIENT_PLUGIN_API_VERSION */
    AgentxxPluginStringView name;   /* 全局唯一 (与 agent 侧插件共用命名空间) */
    AgentxxPluginStringView version;
    AgentxxPluginStringView description;
    /* v4 移除 min_ui_caps: 接口要求改由 plugin.yaml interfaces.require 声明 */
} AgentxxClientPluginInfo;

/* 入口符号 */
#define AGENTXX_CLIENT_SYMBOL_GET_INFO "agentxx_client_get_info"
#define AGENTXX_CLIENT_SYMBOL_CREATE   "agentxx_client_create"
#define AGENTXX_CLIENT_SYMBOL_DESTROY  "agentxx_client_destroy"
```

核心 `AgentxxClientHostVtable` 极简且【契约冻结】(仅 4 成员: alloc/free/
strdup/query_interface); 其余宿主能力全部按 IID 查询独立接口表 (与 agent
侧同内存/日志/JSON 语义; 各表首字段 version, 独立演进):

| IID (宏) | 结构体 | 成员 |
|------|------|------|
| `agentxx.client.ui` (`AGENTXX_IFACE_CLIENT_UI`) | `AgentxxClientUiIface` | `register/update/unregister_status_item`; `register/update/unregister_panel`; `register/update/unregister_info_section`; `register/unregister_command`; `show_toast` (不支持子能力成员为 NULL) |
| `agentxx.client.events` | `AgentxxClientEventsIface` | `subscribe` / `unsubscribe` (卸载自动退订) |
| `agentxx.client.session` | `AgentxxClientSessionIface` | `get_client_state` ({"sessionId","connState","model","models","isStreaming","interfaces","agentPlugins":[{name,version,interfaces}]}) / `send_user_input` (与用户输入同排队语义) / `request_cancel` |
| `agentxx.client.wire` | `AgentxxClientWireIface` | `send_plugin_data` (client → agent, topic `client.{插件名}.{事件名}`) |
| `agentxx.client.self` | `AgentxxClientSelfIface` | `get_own_info` / `get_plugin_args` |
| `agentxx.client.json` | `AgentxxClientJsonIface` | `json_get_string` / `json_escape` |
| `agentxx.client.log` | `AgentxxClientLogIface` | `log` |

### 7.6 装配 (mode_runners)

`setupClientPlugins<IoT, AdapterT>` 模板: 创建 `ClientPluginManager` → `setUiAdapter` (TUI/CLI 适配器) → `setSessionId` → TUI 额外 `setPluginManager` (须在 `start()` 之前, ctx_.pluginManager 在 UI 线程构建组件时读取) → `io->setEventSink(mgr)` → 调用方 co_await `loadConfiguredClientPlugins(plugins)`。

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
6. **命令接口声明**: 命令属于输入管线 (stdin 行解析 / TUI onSend 拦截), CLI/TUI 宿主恒声明 `agentxx.client.command` 接口; 无命令输入面的第三方宿主不声明 → register_command 被拒 (返回非 0), 插件降级。
7. **最低接口要求**: 经清单 `interfaces.require` 声明 (宿主加载前门禁, 见 4.7); 位图方案已废弃。

---

## 8. 内置插件清单

> 全部经 yaml `plugins` 段 path 配置加载 (不区分内置/外置, 无自动加载); 插件动态库输出到 `{AGENTXX_EXEC_INSTALL_PREFIX}/plugins/<插件名>/` (与 agentxx_cli/agentxx_test 同级 exec 目录下按插件名分目录; 各 CMakeLists 已按配置统一输出路径, 多配置生成器不再产生 Debug/ 等子目录)。

| 插件 | 目录 | 说明 |
|------|------|------|
| example_plugin | `agent/plugins/example_plugin/` | 示例插件 (双端): 3 工具 (echo/caller 互调/sleep 慢工具) + agent_start 钩子 + 事件订阅 + 能力声明 + client 入口 (状态栏/面板/Info 段落/命令/事件/跨端) |
| example_js | `agent/plugins/example_js/` | JS 示例插件 (C++ 壳 + plugin.js): 4 工具 (同步/async Promise/JS 内互调/宿主互调) + 钩子 + 事件订阅 + 互查 + 顶层异步初始化; depends: agentxx_javascript_engine |
| agentxx_javascript_engine | `agent/plugins/agentxx_javascript_engine/` | QuickJS 引擎插件 (链接 libqjs.a): 注册能力 `interpreter.js` (方法 load/unload); 专用 JS 线程 + 任务队列 + agentxx 桥 + 沙箱 |
| agentxx_codegraph | `agent/plugins/agentxx_codegraph/` | CodeGraph 代码分析: 8 工具 (search/context/callers/callees/impact/status/index/path); 索引进度/加载状态经 publish 事件 (topic `{插件名}.{事件名}`) 通知宿主, 由 SessionServerAgentIO 原样转发 WirePluginData; 插件 client 入口订阅该事件并以侧边栏 Info 栏段落 (CodeGraph) 渲染索引进度/就绪状态 —— TUI 不再直接解析渲染插件载荷; 工具提示词默认值从 lib AgentPrompt 剥离迁移 (2026-08), entry 时经 `set_prompt` 写入宿主 toolPrompt (宿主已有条目则跳过, 尊重用户 yaml 覆盖); 插件参数经 yaml plugins args (paths/ignore_paths/load_cwd/use_gitignore) |
| agentxx_screen_capture | `agent/plugins/agentxx_screen_capture/` | 屏幕捕获 (仅 Windows): 工具 `agentxx_screen_capture` (单帧/全部屏幕/鼠标屏/指定屏幕/流式推帧事件 topic `agentxx_screen_capture.frame`) |
| agentxx_computer_use | `agent/plugins/agentxx_computer_use/` | 键鼠控制 (仅 Windows): 工具 `agentxx_ui_control_keyboard_mouse`; plugin.yaml `depends: [agentxx_screen_capture]` (须同时配置加载) |
| agentxx_system_monitor | `agent/plugins/agentxx_system_monitor/` | 系统资源监控 (从 lib `src/expand/get_cpu_gpu_use` 拆分): 工具 `agentxx_get_system_core_info` (原内置工具迁移, lib 不再内置) + 能力 `agentxx.system_usage` (方法 query) + agent 侧周期采集线程 (每 5s 采样并 publish `agentxx_system_monitor.usage`, 定时/采集/发布完全位于插件内; 显示开关由 client `/sysinfo` 经跨端事件 `usage_enabled` 同步, 关闭期间跳过采集); 载荷为插件定义 schema 的 JSON 字符串, server 经 WirePluginData 原样转发; 插件 client 入口订阅该事件以状态栏项渲染 CPU/RAM 占用 (快速一览) + 侧边栏 Info 栏段落渲染明细 (CPU/RAM/GPU) —— 采集实现与渲染完全隔离在插件内, lib wire 层不含任何系统资源 DTO |
| agentxx_audio_stream | `agent/plugins/agentxx_audio_stream/` | 音频流捕获 (从 lib `src/expand/audio_stream` 拆分; 仅 Windows WASAPI): 系统输出/程序输出/麦克风; 工具 `agentxx_audio_stream` (start/stop/status); 帧经 publish 事件推送 (topic `agentxx_audio_stream.audio`, base64 PCM); 非 Windows no-op |
| agentxx_text_selection_monitor | `agent/plugins/agentxx_text_selection_monitor/` | 系统级文本选择事件流 (从 lib `src/expand/text_selection_monitor` 拆分; 仅 Windows UIAutomation/WinEvent/CDP/剪贴板兜底): 工具 `agentxx_text_selection_monitor` (start/stop/status); 选中文本经 publish 事件推送 (topic `agentxx_text_selection_monitor.selection`); 非 Windows no-op |
| example_resources | `agent/plugins/example_resources/` | 会话资源示例 (v8): 演示插件贡献 Skill/Memory/MCP 的双通道 —— plugin.yaml 声明式段 (skills//assets/NOTES.md/mcp 示例条目) + entry 内 vtable `register_skill_dir` 运行时注册 (skills_runtime/); 语义见 4.2.1 |

---

## 9. 配置与分发

### 9.1 yaml `plugins:` 配置段

```yaml
plugins:
  - path: ./plugins/agentxx_codegraph   # 插件动态库路径 或 插件目录 (含 plugin.yaml 时按清单分派)
    enabled: true                        # 默认 true
    sides: auto                          # auto|agent|client (双端插件用; 默认 auto)
    args:                               # 插件参数 (宿主原样保存并整体传递, 字段语义由插件定义;
                                        #   此为 agentxx_codegraph 的参数键名)
      paths: [src]                       # 加载(索引)路径列表
      ignore_paths: [build, third_party] # 忽略路径列表 (支持 * 通配符)
      load_cwd: true                     # paths 未配置时默认加载当前工作目录
      use_gitignore: true                # 忽略 .gitignore/.gitmodules/.git
```

- `sides` 语义: `agent` 仅 agent 侧加载; `client` 仅 client 侧加载; `auto` (同 `both`) 按导出符号自动决定 (client 侧: 有 `agentxx_client_create` 才加载, 无则跳过并记日志)
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
skill: []                     # 可选 (v8): 会话资源声明 —— skill 扫描目录列表
memory: []                    # 可选 (v8): memory 上下文文件列表
mcp: []                       # 可选 (v8): MCP server 列表项 {namespace,url,timeout(秒)}
                              #   语义见 4.2.1; entry 成功后应用 (失败不生效)
interfaces:                   # 可选 (接口协商, 见 4.7):
  require: [agentxx.agent.core]    #   必选接口 —— 任一缺失 (按前缀过滤出本侧) 该侧跳过加载
  optional: [agentxx.client.toast] #   可选接口 —— 缺失仅警告, 插件注册时自降级
```

宿主解析 name/entry/depends/optional_depends (YAML::LoadFile) + 资源声明段
(skill/memory/mcp, 见 4.2.1) + 接口声明段 (require/optional, 见 4.7); 目录
路径可直接传给 `loadPluginAsync` / yaml `plugins[].path`。直接给库路径时插件
名按文件名推导 (`pluginNameFromPath`: libfoo.so → foo), 资源/接口声明仅目录
形态生效。

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

#### 9.3.1 插件平台支持矩阵 (2026-08)

各插件并非全平台适配: 源码无对应平台的真实实现时 (仅有 "not supported/not
implemented" 桩), 各插件自身 CMakeLists.txt 按声明的平台支持列表跳过编译
(内置合并编译分支与独立动态库分支均生效, configure 期输出
`Skip plugin '...'` STATUS 日志)。平台判定复用项目既有 `XX_IS_LINUX_D`/
`XX_IS_WIN_D`/`XX_IS_MACOS_D`/`XX_IS_ANDROID_D`/`XX_IS_IOS_D` 变量 (嵌套
构建由顶层经 `_AGENTXX_COMMON_CMAKE_ARGS` 传入; 独立构建时按顶层相同规则
本地推导), 直接以标志位匹配, 无中间平台标识变量。

| 插件 | Windows | Linux | macOS/iOS | Android | 说明 |
|------|---------|-------|-----------|---------|------|
| example_plugin / example_js / javascript_engine / codegraph | ✅ | ✅ | ✅ | ✅ | 纯跨平台实现 |
| agentxx_system_monitor | ✅ PDH/DXGI | ✅ /proc+sysfs | ❌ 桩 | ✅ 复用 Linux 分支 | 共享头将非 `_WIN32`/非 `__APPLE__` 推导为 `XX_IS_LINUX_D=1`; `/proc/stat`、`/proc/meminfo` 在 Android 可读, GPU 经 sysfs/drm 枚举缺失时优雅降级 |
| agentxx_screen_capture | ✅ DXGI/GDI/WIC | ❌ 桩 | ❌ 桩 | ❌ 桩 | 仅 Windows 有桌面捕获实现 |
| agentxx_computer_use | ✅ user32 注入 | ❌ 桩(返回 not available) | ❌ | ❌ | 运行时依赖 screen_capture, 平台矩阵与其保持一致 |
| agentxx_text_selection_monitor | ✅ UIAutomation/CDP | ❌ 桩 | ❌ 桩 | ❌ 桩 | 仅 Windows 有文本选择监听实现 |
| agentxx_audio_stream | ❌ | ❌ | ❌ | ❌ | 全平台跳过: Windows WASAPI 实现被源码 `&& false` 停用, 其余平台为桩; 实现就绪后在矩阵中加回对应平台 |

各插件的支持平台声明**分散维护于各自 CMakeLists.txt 开头** (仅插件清楚自身
实现情况): 经 `agent/plugins/cmake/plugin_platform_support.cmake` 的
`agentxx_plugin_platform_gate(<名> <结果> <支持平台...>)` 判定, 不支持则提前
`return()` 跳过本目录全部目标 (内置合并/独立动态库两模式均生效)。跨平台插件
无需声明 (默认放行); 内置模式下清单登记 `AGENTXX_BUILTIN_PLUGIN_NAMES` 由各
插件子目录自追加, 提前 return 时天然不会误登记。测试侧对应模块
(test_screen_capture/test_text_selection_monitor/test_cpu_gpu_use) 已有
`#if XX_IS_WIN_D` 等编译期门控 + `TEST_SKIP`, 与矩阵一致。

---

## 10. 示例插件

### 10.1 C++ 插件骨架 (与现网 plugin_api.h v1 COM 风格一致)

```cpp
// my_tool_plugin.cpp —— 编译为 libmy_tool_plugin.so, 无需链接 libagentxx
// 多实例范式: 状态存 ctx, 回调经 user_data 恢复 (详见 4.2 多实例三铁律)
#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_iface_helper.h"   // 可选: 一次查询聚合便捷层

/// 每实例上下文 (create 创建经 *plugin_ctx 交付宿主, destroy 释放)
struct MyPluginCtx {
    const AgentxxHost*           host  = nullptr;
    agentxx::plugin::AgentIfaces iface {};
};

// execute 回调运行在宿主 io 线程; 返回字符串必须经宿主分配 (AGENTXX_STRDUP)
static char* my_exec(void* ud, AgentxxPluginStringView args_json,
                     AgentxxPluginStringView thread_id, AgentxxPluginStringView tool_call_id,
                     char** err_out) {
    auto* ctx = static_cast<MyPluginCtx*>(user_data);
    (void)args_json; (void)thread_id; (void)tool_call_id; (void)err_out;
    if (!ctx || !ctx->host) return nullptr;
    return ctx->host->vtable->strdup("{\"ok\": true}");
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
extern "C" int agentxx_plugin_create(const AgentxxHost* host, void** plugin_ctx) {
    (void)plugin_ctx;
    g_host = host;
    // COM 风格接口表查询: entry 内一次性查询全部已知 IID 并缓存
    static const agentxx::plugin::AgentIfaces s_if = agentxx::plugin::AgentIfaces::query(host);
    g_if = s_if;
    if (!s_if.tools || !s_if.tools->register_tool) return -1;

    AgentxxToolSpec spec{};
    spec.name            = AGENTXX_SV("my_tool");
    spec.description     = AGENTXX_SV("My first plugin tool");
    spec.parameters_json = AGENTXX_SV(R"({"type":"object","properties":{}})");
    spec.execute         = my_exec;
    return s_if.tools->register_tool(host, &spec);   // 0 = ok
}

// 可选: 卸载通知 (宿主已先自动反注册; 插件侧约定主动清理)
extern "C" void agentxx_plugin_destroy(void* plugin_ctx) {
    (void)plugin_ctx;
    if (g_host && g_if.tools && g_if.tools->unregister_tool) {
        g_if.tools->unregister_tool(g_host, AGENTXX_SV("my_tool"));
        g_host = nullptr;
    }
}
```

纯 C 路径 (不用便捷层): `host->vtable->query_interface(host, AGENTXX_SV(
AGENTXX_IFACE_AGENT_TOOLS))` 返回 `const void*`, 按头文件中 IID 对应的
`AgentxxToolsIface*` 解释即可。

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

同一动态库导出 `agentxx_plugin_create` (agent 侧工具/钩子) 与 `agentxx_client_create` (client 侧状态栏/面板/Info 段落/命令/事件/跨端), 两个 PluginManager 各自 dlopen/装配, 实例状态独立, 互通一律走 wire。参考 `agent/plugins/example_plugin/example_plugin.cpp`。

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
| `agent/lib/src/agent/base_agent.cpp` | `init()` 装配 ToolRegistry/PluginManager + setIoExecutor + 静态工具名收集 (必须在 own_tools 之前) + 加载配置插件; `runTurnAsync` 轮次边界登记 (flushPendingCleanup/onTurnBegin/onTurnEnd) |
| `agent/lib/include/agentxx/agent/config.h` + `config_loader.cpp` | `plugins:` 配置段 (path/enabled/sides/args; PluginConfig/PluginSide) |
| `agent/lib/include/agentxx/middlewares/middleware.h` + `nodes/wrap_handle.h` | `BaseMiddlewareHandleInterface::disabled` 位; start/end 跳过 disabled + end 按 start 记录回放 |
| `agent/lib/include/agentxx/agent/resource_applier.h` | AgentResourceApplier 接口 + PluginResourceDecls/AgentResourceSnapshot (v8 会话资源) |
| `agent/lib/include/agentxx/agent/resource_applier.h` + `src/agent/resource_applier.cpp` | AgentResourceApplier 具体实现 (单一实现, 无接口分层): Skill/Memory 中间件转发 + MCP 异步连接状态机 (stale 检查/所有权/冲突规则) |
| `agent/lib/src/plugins/plugin_common.*` (parsePluginManifest resources 参数) | 清单资源声明段解析 (相对插件目录 → 绝对路径; timeout 秒→毫秒) |
| `agent/lib/src/plugins/plugin_manager.*` / `agent/lib/src/agent/code_agent.cpp` | applier 装配 (CodeAgent::initMiddleware 注入 AgentContext::resourceApplier); loadNativeAsync/loadBuiltinAsync 成功尾调用 applyDeclaredResources (失败不生效); vtable xx_* 包装; detachAll/disable/enable 联动摘除与恢复 |

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
| `agent/client/include/agentxx-client/io/plugin/plugin_ui_adapter.h` | UI 适配器接口 (supportedInterfaces/注册/更新/toast/sendPluginMessage/sendPluginData) |
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
| 会话资源贡献 (v8, 2026-08) | plugin_api v8 (skill/memory/mcp 注册 API + 定时器/offload v7); plugin.yaml 资源声明段; AgentResourceApplier 具体实现 (MCP 异步连接状态机/所有权/冲突规则); 中间件动态增删 + epoch 缓存失效; JS 桥新 API; example_resources 示例 + 测试模块 `plugin_resources` | ✅ 已实现 |
| 接口协商一期 (2026-08) | plugin.yaml `interfaces.require/optional` 声明; 宿主支持集门禁 (require 缺失跳过 + 原因记录) / 符号意图预检 (requiredEntrySides); READY/get_client_state interfaces 数组 | ✅ 已实现 |
| 接口协商二期+三期 (2026-08, 本轮) | **位图方案整体移除** (AGENTXX_UI_CAP_*/IFACE_*/ui_caps/min_ui_caps); client_plugin_api v4 + plugin_api v9: `has_interface` 字符串判定; **COM 式扩展表** `query_extension` (核心契约冻结, 新增能力走独立 version 扩展表); 展示/命令/toast 物理迁至 "client.ui" 扩展表 (`AgentxxClientExtUiVtable`); WireHelloAck.plugins 结构化 [{name,version,interfaces}]; server_plugins 约定事件同步升级; get_client_state 的 agentPlugins 为结构化对象数组; client→server 上行约定事件 `client_interfaces` (**1:N 不存储**, 仅转发 agent 总线, 插件订阅 `agentxx_host.client_interfaces` 自适应); CLI/TUI/测试适配器与全部插件迁移完成 | ✅ 已实现 |
| COM 全量接口表重构 (2026-08) | **plugin_api/client_plugin_api 版本重置为 1 (不兼容变更, 抛弃历史兼容)**: 核心 vtable 收缩为 `alloc/free/strdup/query_interface` 四成员并契约冻结; agent 侧拆出 12 张标准接口表 (`agentxx.agent.tools/hooks/events/capabilities/scheduler/session/plugins/config/prompt/json/log/resources`), client 侧拆出 7 张 (`agentxx.client.ui/events/session/wire/self/json/log`), 各表首字段 version 独立演进; **内置接口名统一加保留前缀 `agentxx.`** (第三方私有接口用 `<vendor>.<name>`, 不得占用该前缀; 清单前缀过滤/入口符号推导同步改为 `agentxx.agent.*`/`agentxx.client.*` 规则); 新增 `plugin_iface_helper.h` (AgentIfaces/ClientIfaces 一次查询聚合); 全部插件与测试迁移完成; 旧版插件经 api_version 门禁直接拒绝 | ✅ 已实现 |
| 统一异步操作模型 (2026-08, 本轮; 不兼容变更) | **工具/钩子/能力方法统一为 start/poll/cancel 异步三件套** (tools/hooks/capabilities/scheduler 表 version → 2): 宿主新增 op_driver (`op_driver.h`) 在 io 线程驱动插件操作并与内置工具协程同线程交错执行 —— 插件不再被线程池黑盒阻塞, 访问会话数据单线程安全; 新增 `call_tool_async`/`invoke_capability_async` 返回 `AgentxxHostOp` 句柄 (宿主后台驱动 + 任意线程轮询), 阻塞便捷版 io 线程 fail-fast; scheduler.offload 增加**调用方持有 cancel_flag** 形参; 纯 C 同步垫片 `plugin_tool_sync.h` (inline/sync 工具 + sync 钩子一行注册); JS 引擎移除 postSync 阻塞桥 (工具 execute 与能力 load 改为 JS 线程任务 + 通知器上报, 根除 io↔引擎互等死锁面); 内置插件全部迁移 (快同步内联 / 慢同步 offload 垫片 / example_sleep 自管异步演示); 会话取消联动 `execute_cancel`; 宿主看门狗 (>100ms WARN) 监控 io 线程被插件卡住; 测试新增 HostOp 语义/poll 推进/取消联动用例 (plugins 模块 159 断言) | ✅ 已实现 |
| poll 寄生驱动 (2026-08, 本轮) | **协程异步插件第三姿势** `plugin_poll_loop.h`: 插件自有无线程寄生 io_context 由宿主 io 线程经 pollOnce 非阻塞步进, 协程与内置工具完全同线程交错 —— execute_command (bash/windows 子进程管道) / websearch (3 个 HTTP 工具) 自"局部 io_context 同步驱动 + offload 占线"迁移至此 (并发命令共享寄生 loop 不再每命令占死一个阻塞池线程至超时; 删除 runSync/局部 ioctx 模式); system_monitor 能力 system_usage 自"io 线程内联 ~100ms 采样"(违反快速返回契约) 改为寄生三件套; text_selection_monitor delayMs 去 asio 误用; rag_search 维持 sync 垫片 (检索为 CPU 密集 B 型); 测试新增并发不串行/取消及时断言 | ✅ 已实现 |
| 三期 (生态) | Wire 远程热管理 / TUI 插件管理面板 / skippedPlugins 展示层接入 / 签名校验 | ⏳ 待实现 |

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
9. **跨端单侧缺失兼容与可观测性 (2026-08, 见 §7.3.1/§7.3.2)**:
   - 宿主约定事件 `agentxx_host.client_attached` / `server_plugins` (状态快照
     重发 + 对端插件列表); codegraph/system_monitor 订阅 client_attached
     重发/加速首份数据
   - HelloAck 增加 `plugins` 字段; get_client_state 增加 `agentPlugins`
   - 上行 WirePluginDataUp 缺对端插件 → 冷却限频 WARN; 下行 PLUGIN_DATA 无
     client 订阅者 → 每插件名一次 WARN
   - agent 侧 sides=Auto 无入口降级 WARN 跳过 (纯 client 插件误配不再报错;
     显式 sides=agent 仍为错误)
   - client 侧目录 manifest 缺失/非法补 XX_LOGE (原静默跳过)
- **LLM 请求侧工具 schema 静态性**: 原稿只改造了 `toolcall.cpp` 执行侧查表, 但 `ModelCallWrapNode::build_params` 每轮从静态 `tools_` 组装工具定义发给 LLM —— 若不追加插件工具, 模型永远看不到新工具。实现中在 build_params 经 `ToolRegistry::appendDefinitions` 追加 (热注册后下一轮 modelcall 即对模型可见)。
- **执行中工具悬垂**: 原稿靠插件 inflight 计数, 实现中 `ToolRegistry::find` 返回 shared_ptr 保活 (与 execTool 的裸指针路径并存, 插件工具经 shared_ptr 传入), 双保险。
- **注册时序**: dlopen 在阻塞线程池, 但 entry 的注册动作必须回到 io 线程 (无锁模型); `loadNativeAsync` 在 io 线程协程内完成 dlopen (卸载到线程池) + entry 同步调用。
- **卸载超时**: `waitInflightZero` 带超时, 超时放弃卸载 (慢/恶意插件不无限阻塞 io 线程)。
- **拓扑排序加载**: 配置顺序无关 (Kahn 排序, 依赖者排在被依赖者之后), 避免配置顺序导致必选依赖缺失; 无进展 (环/缺失) 项附后由依赖检查报错。
- **插件配置 args**: yaml `plugins` 条目 args 宿主原样保存并整体传递 (`get_plugin_args`), 参数语义由插件定义 (如 agentxx_codegraph 的 paths/ignore_paths/load_cwd/use_gitignore)。
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
