# agentxx 与 deepseek-harness (dsh) 架构对比

> 对比对象
> - `agentxx`: `D:\0Acoolight\Program\cpp\agentxx`（C++23, Boost.Asio 协程 + NeoGraph 图引擎 + C ABI 插件）
> - `deepseek-harness`（下称 **dsh**）: `D:\0Acoolight\Program\js\deepseek-harness`（TypeScript/Node 22, Cordis 插件树 + 事件溯源会话日志）
>
> 本文按模块通读两个项目的源码、测试与文档后逐模块记录差异、优缺点与**可迁移到 agentxx 的设计**。
> 阅读顺序建议：先看[总览](#0-总览与对比方法)与[迁移建议汇总](#16-迁移建议汇总)，再按模块展开。
>
> **修订记录**
> - 第 1 版：以架构文档/包 README/子系统文档为主，建立模块划分与结论。
> - 第 2 版：**逐文件精读两侧源码**核对结论，修正了第 1 版中若干凭文档推断的错误（详见 [§0.2](#02-对比方法与判据) 的修订说明），并为每节补充“源码要点”，新增[附录 C](#附录-c本次精读的源码清单与关键发现) 列出实际读过的文件及其证明/修正的内容。
> - 第 2 版增补：**插件系统专题**（[§18](#18-插件系统深入对比第-2-版增补)），拆开两侧插件栈的四个层次逐层对比（Cordis 内核/Loader/组合层/运行时自修改 vs pluginxx 内核 ABI/宿主骨架/运行时/领域宿主），并给出双向可迁移项。
> - 第 2 版增补 2：**每个模块末尾增加「各自做得好的设计」**（见下方阅读提示），分别列出 agentxx 与 dsh 各自更强、更值得保留的设计，避免只写“谁缺什么”而忽略“谁强在哪”。

## 目录

| 章节 | 内容 | 一句话结论 |
|---|---|---|
| [0](#0-总览与对比方法) | 总览与对比方法 | 两项目定位、规模、判据与结论速览 |
| [1](#1-总体架构与扩展模型) | 总体架构与扩展模型 | dsh 一切皆插件；agentxx 分层 + C ABI 插件，应吸收“事件分层 + 可逆注册” |
| [2](#2-agent-循环与轮次语义) | Agent 循环与轮次语义 | 应显式化轮次/步骤与输入通道，并给取消/重试明确语义 |
| [3](#3-会话与-llm-上下文) | 会话与 LLM 上下文 | agentxx 的“会话即权威”保留，吸收来源元数据与失败留痕 |
| [4](#4-持久化与崩溃恢复) | 持久化与崩溃恢复 | 需补格式版本、写者所有权与崩溃配平 |
| [5](#5-工具系统) | 工具系统 | 工具应返回结构化值并分离渲染，补上下文回注与单调守卫 |
| [6](#6-系统提示词与请求组装) | 系统提示词与请求组装 | 段落排序注册 + 动态上下文快照化（KV Cache 友好） |
| [7](#7-llm-流式与适配器) | LLM 流式与适配器 | 能力协商下沉到 provider；重试统一策略点 |
| [8](#8-上下文压缩) | 上下文压缩 | 拆成“剪枝 → 度量 → 摘要”，并让压缩状态可检测 |
| [9](#9-权限审批与沙箱) | 权限、审批与沙箱 | 引入失败姿态声明、单调守卫与执行世界抽象 |
| [10](#10-子代理后台任务与工作流) | 子代理、后台任务与工作流 | 后台作业与可继续子代理是最大功能缺口 |
| [11](#11-会话检索标题遥测附件与大输出) | 检索、标题、遥测、附件、大输出 | FTS 检索 + 附件校验元数据 + spill 式定位符 |
| [12](#12-客户端-ui-与客户端插件) | 客户端 UI 与客户端插件 | 抽客户端模型层；引入 UI 插槽与命令注册 |
| [13](#13-远程协议sdk-与外部集成) | 远程协议、SDK 与外部集成 | Wire 编解码生成化 + 协议能力位 |
| [14](#14-测试与质量门禁) | 测试与质量门禁 | 录制会话回放 + 注册表清理测试 |
| [15](#15-文档与配置体系) | 文档与配置体系 | 生成式目录 + 决策记录 + 术语表 |
| [16](#16-迁移建议汇总) | 迁移建议汇总 | P0/P1/P2 三级清单 + 源码精读后的新增项 |
| [17](#17-反向清单agentxx-不必照搬的设计) | 反向清单 | 明确哪些差异是正当取舍 |
| [18](#18-插件系统深入对比第-2-版增补) | 插件系统深入对比（增补） | 两侧插件栈四层拆解、机制对照与迁移建议 |
| [19](#19-结语) | 结语 | 三条可迁移的工程原则 |
| [附录 A](#附录-a关键文件与文档索引) | 关键文件/文档索引 | 两项目对应模块的路径对照 |
| [附录 B](#附录-b术语对照) | 术语对照 | 同一概念的两种叫法 |
| [附录 C](#附录-c本次精读的源码清单与关键发现) | 源码清单与关键发现 | 逐文件列出本次精读的源码与核对结论 |
| [附录 D](#附录-d可以继续深入的清单按收益排序) | 后续可深入清单 | 尚未精读的位置 + 能回答的问题 + 接续方式 |

---

## 0. 总览与对比方法

### 0.1 两个项目的定位与规模

| 维度 | agentxx | dsh |
|---|---|---|
| 语言/运行时 | C++23, Boost.Asio 协程, 单线程 io_context 交错执行多会话 | TypeScript (ESM), Node ^22.19 / >=24, 单进程事件循环 |
| 核心抽象 | NeoGraph 图引擎（节点 + 条件边）+ 中间件钩子 + 传输端点 | Cordis 插件树（服务 + 类型化事件 + 可逆副作用）|
| 扩展形态 | 纯 C ABI 动态库插件（`.so`/`.dll`），宿主按名查符号，接口表按名称查询 | npm 包插件（同进程、同语言），经 Cordis `ctx.<service>` 注入与事件扩展 |
| 装配方式 | `agentxx-config.yaml`：base（`data_dir`）+ overlay（工作目录）两层合并 | profile + bundle + 有序 patch（`cordis.patch.yml`），逐条覆盖/插入 |
| 会话语义 | 会话（`Session`）持有 typed 上下文，SQLite 落库；图状态只有影子元信息 | 会话 = 仅追加 `SessionEvent` 日志；模型历史由日志 `deriveMessages()` 派生 |
| 客户端 | FTXUI TUI / stdio CLI / 远程 WS 客户端，客户端插件（C ABI）| Web（浏览器）+ Electron 桌面 + headless，客户端插件（浏览器 bundle）|
| 内置能力形态 | 内置工具/能力本身也是插件（`agent/plugins/agentxx_*`）| 一切皆插件（含 agent loop、模型适配器、会话日志）|
| 测试 | 自研 `agentxx_test` 多模块（C++ 断言，模块化运行）| vitest 多套配置 + 覆盖率门禁 + 录制会话快照回放 + e2e |
| 文档 | `docs/zh-cn/design/*.md` 手写设计文档 + `resource/history/*` 实现记录 | 双语配对文档 + **生成式目录**（配置/持久化/工具/模块图/事件）+ doc 门禁 |

### 0.2 对比方法与判据

- 每个模块都从**三方证据**核实：源码（真实实现）、测试（行为边界）、文档（设计意图）。
- 第 2 版把“源码”这一方做实：两侧各精读了十几个关键实现文件（清单见附录 C），凡与文档表述冲突处**以源码为准**，并逐条修正。
- “优点/缺点”的判据只有两条：**能否降低后续改动成本**（扩展点是否收敛、约束是否显式）与**运行期是否正确/稳定**（生命周期、取消、恢复、并发）。
- “迁移建议”只写**能在 agentxx 现有架构上实现**的项，并标注落地位置（哪个文件/哪个层次）与代价。

**第 2 版的修订说明（第 1 版凭文档推断导致）**

| # | 第 1 版的说法 | 源码核实后的更正 |
|---|---|---|
| 1 | “agentxx 取消后历史里没有占位结果，与用户所见可能不一致” | **错误**。`ToolcallWrapNode::baseRun` 的取消分支会为未完成的 tool_call 补写 `[User canceled]` 占位结果（`AutoInserted`），保证每条声明的 tool_call 都有对应结果；另有一层读时修复 `repairMessages` 兜底。 |
| 2 | “agentxx 工具并行能力取决于图/工具自身” | 更准确：`ToolcallWrapNode` **明确是顺序执行**，代码中保留 `// TODO: 真正并行`；并行只能由图层面或工具自身实现。 |
| 3 | 压缩触发条件写为“超过 75% 上限自动压缩” | 补全：还有**冷却规则**（上次压缩后消息数增长 ≤2 且无失败时跳过 LLM 压缩）与 **API usage 优先、启发式兜底** 的 token 计数口径。 |
| 4 | 权限“未声明权限的工具直接放行” | 决策顺序更细：worktree 隔离 → 配置拒绝（**即使完全授权也不放行、不询问**）→ 完全授权 → 最长前缀规则 → `noRuleOperator` 兜底；空/无法规范化路径按“未获批准（Ask）”处理。 |
| 5 | dsh 工具并行“分类 + 有界池 + 顺序提交” | 补充实现细节：注册表把流水线拆成 `prepare`/`dispatch`/`finalize`/`finish` 四段，**前置策略顺序执行、执行体可重叠、后置策略按模型顺序提交**；调度器还会在每个调用启动前**重新读取执行模式**。 |
| 6 | dsh 压缩“先剪枝后摘要” | 补充：摘要请求会**重建被压缩区间的可缓存前缀**（系统提示 + 工具 schema + 区间消息），从而在 provider 侧复用 KV Cache。 |
| 7 | 会话/上下文只描述了三种写入口 | 补充：所有变更经 `markMessagesChanged()`（版本号 + 脏标记），持久化是**节流写入**（`kPersistThrottleMs`）+ 轮末权威保存；`context.h` 中“快照/回滚依赖 messagesVersion”的注释已与实际用途（影子通道）不符。 |

### 0.3 结论速览

| 模块 | agentxx 现状 | dsh 现状 | 更值得借鉴的方向 |
|---|---|---|---|
| 扩展模型 | C ABI 插件 + 接口表 | Cordis 插件树 + 服务/事件 | 事件分层与可逆注册、按 agent 作用域 |
| Agent 循环 | 图引擎驱动的 ReAct 循环 | turn/step 驱动器 + inbox | 轮次语义显式化、steering/inject 分通道 |
| 会话与上下文 | 会话为上下文唯一权威 | 事件溯源日志 + 派生历史 | 历史“可替换/可回放”的表达力 |
| 持久化 | SQLite 单库多表（按会话目录隔离） | 句柄式持久化 seam + JSONL | 格式版本与迁移、写句柄独占、崩溃配平 |
| 工具系统 | `XXToolBase` + 图分发 | schema DSL + 流水线 waterfall | 输出契约（值/渲染分离）、结果上下文回注 |
| 提示词 | 字符串拼装 + 追加段 | 段落注册表 + 动态上下文 + 变量 | 段落排序注册、`complete` 覆盖、提示词入历史 |
| LLM 层 | provider 适配 + 限流 | 适配器 seam + 能力协商 | 能力协商、attempt 留痕、请求冻结 |
| 压缩 | 阈值 + 子代理摘要 + 去重/冷却 | 剪枝 + 摘要 + surface 替换 | 先剪枝后摘要、在打开步骤内恢复 |
| 权限/沙箱 | 中间件统一判定 + 插件声明 | approval seam + 沙箱后端 + 单调守卫 | 单调守卫、执行后端抽象 |
| 子代理/作业 | subagent 委派（异步） | subagent seam + jobs + workflow | 作业化与“可续跑”抽象 |
| 检索/标题 | share_store + RAG 插件 | session-query + 标题 seam | 会话全文索引、标题提供方 |
| 客户端 | TUI 声明式组件 | Web 客户端 + 插槽 + 投影 | 投影/插槽的宿主-客户端分层 |
| 远程协议 | WS + wire protocol + MCP/ACP/A2A | BFF + SDK JSON-RPC + ACP + MCP | 协议与类型图生成、附着重连 |
| 测试 | 多模块 C++ 测试 | 覆盖率 + 快照回放 + 门禁 | 录制会话快照回放、运行时不变式 |
| 文档/配置 | 设计文档 + 实现记录 | 生成式目录 + 决策记录 | 生成式目录、决策记录规范 |

---

## 1. 总体架构与扩展模型

### 1.1 dsh：Cordis 插件树

**五个核心概念**（`docs/cordis-primer.zh.md`）：

- **插件**是“实现 Service 的对象”，可以是带 `inject` + `apply(ctx)` 的函数，也可以是 `Service` 子类。
- **上下文是服务容器**：一个服务占一个稳定键（`ctx.tools` / `ctx.llm` / `ctx.sessions` / `ctx.agents` / `ctx.systemPrompt` …），其他插件按 key 查找服务，不导入具体实现。
- **`inject` 声明依赖**：插件等待依赖服务就绪才启动，加载顺序由服务依赖表达，而非人工编排。
- **类型化事件是通信方式**：声明合并（declaration merging）注册事件名，分发模式分五种——`emit`（观察）、`waterfall`（环绕包装、可短路）、`parallel`、`serial`、`bail`。
- **注册是可逆副作用**：所有贡献经 `ctx.effect()` / `ctx.on()` 安装，卸载时自动撤销（`AGENTS.md` 的硬性约定：“Registrations are effects”）。

**装配**（`docs/architecture.zh.md#profile-与组合包`）：运行中的 `dsh` 是启动时按序叠加出的插件树——
`profile`（具名组装，声明自己叠放的 bundle、树外插件、用户 `cordis.patch.yml`）→ 每个 bundle 依次应用 → profile patch → home 级 patch → `--patch` overlay；一条 patch 按 id 定位条目并替换其整个 config，或插入新条目。`dsh-base` 是所有 profile 的共享第一层。
`dsh --profile web --dump-config` 可以把整棵配置树打印出来，**用户看到的每个条目都可被自己的 patch 替换**。

**“不存在需要打补丁的特权内核”**：连 agent loop 本身（`dsh-agent-loop`）都是插件，扩展程序 = 挂到别的插件旁边，而不是改内核。核心主干被压缩为六个包（session / system-prompt / tools / agent / agent-loop / scope），其余全是可替换能力。

**能力 seam（capability seam）**：一项可替换能力固定包含三种角色——Service Definition（抽象类，占 `ctx.<key>`）、Service Provider（实现者）、Consumer（通常是面向模型的工具）。`packages/shell` 是范例：`dsh-shell`（定义）+ `dsh-bash-local`/`dsh-bash-sandbox`（提供方）+ `dsh-tool-bash`（消费方）。仓库约定：**扩展插件依赖 Service Definition，绝不依赖具体提供方**（`packages/README.zh.md#依赖`），所以“把 fs 与 subprocess 指向远端沙箱”就能把 Bash/PTY/LSP 整体搬走，不需要 fork。

**按 agent 作用域注册（agent-scope）**（`docs/subsystems/scope.zh.md`）：注册只有两层——全局，或归属于恰好一个 `ScopeKey`（约定上一个活跃 agent 就是自己的 scope key）。带作用域的注册**不会向下继承给 subagent**；同名的带作用域注册**遮蔽（shadowing）**全局注册；`tools.restrict` 为单个 scope 过滤全局工具集合（多个 restriction 取交集），被过滤掉的工具在提示词里不出现、执行时也拒绝，与“不存在”无法区分。

**运行期自修改（extensions）**（`docs/subsystems/extensions.zh.md`）：模型可以定义带版本的 Cordis 包、跑其 host 半边与浏览器半边（`ctx.dynamicCordisRunner`），并有 `ctx.cordisInspect` 让模型在写代码前查询运行时元数据；客户端半边需要用户批准（`approveFutureVersions` 决定后续版本是否沿用授权）。

**源码要点（第 2 版补充，`vendor/cordis/src/{fiber.ts,events.ts}`）**

- **副作用（effect）的表达力比“一个析构函数”更强**：`ctx.effect()` 接受**同步/异步析构函数、其 Promise、或（可以是异步的）生成器**——生成器每 yield 一个 disposer 就立刻登记一个，因此插件可以边启动边注册清理；fiber 卸载时**按注册的逆序**执行，异步 disposer 会被等待。
- **effect 自带可诊断标签**：注册时可以给一个标签（框架自用形如 `ctx.on("event")` / `ctx.provide("name")`），并维护**嵌套 effect 标签树**（`EffectMeta.children`）——出问题时能直接看出“哪个插件注册了什么”。
- **状态机阻止在非活跃上下文上注册**：`FiberState` 有 `ACTIVE`/`FAILED`/`UNLOADING`/`DISPOSED` 等值，在非活跃上下文上创建 effect 会抛 `cannot create effect on inactive context` —— 这就是“注册必须属于某个活着的 fiber”的执行机制。
- **waterfall 语义在框架层固化**：五种分发模式（`emit`/`parallel`/`serial`/`bail`/`waterfall`）；waterfall 的每个监听器**包住其余链路**，不调用 `next()` 直接返回即“否决下游”，与仓库约定“waterfall 监听器必须调用 `next()`”互为表里。
- **框架不强制“注册即可逆”**：effect 机制只保证“有登记就会被执行”，是否登记仍靠纪律与门禁（这与 agentxx 靠 `register_*`/`unregister_*` 成对约定的处境相同，只是 dsh 有一个统一的登记入口可以审计）。

### 1.2 agentxx：分层 + C ABI 插件

- **分层**：`agent/client`（CLI/TUI/配置加载）、`agent/lib`（libagentxx：BaseAgent/CodeAgent、节点、中间件、协议、UI 描述层）、`agent/plugins`（内置能力插件）、`agent/third_party`（含自研三库 `cxx_utilxx_base`/`cxx_utilxx`/`cxx_pluginxx`）。
- **插件契约**（`agent/lib/include/agentxx/plugin/api/plugin_api.h`）：API v1，纯 C ABI——
    - 8 字节对齐 + 定长基础类型（`int32_t/int64_t/uint64_t`）+ 统一调用约定 `PLUGINXX_CALL`；
    - 结构体**入参传指针、返回值用出参**（`int32_t` 状态码）；
    - 插件导出五个入口：`agentxx_plugin_agent_{get_info,create,start,stop,destroy}`（client 侧同名 `agentxx_plugin_client_*`）；
    - 能力经**接口表（vtable）按名称查询**：`query_interface(host, iface_name, version, out)`，agent 侧 10 张通用表 + 9 张领域表（`agentxx.agent.tools` / `permission` / `hooks` / `session` / `model` / `prompt` / `resources` / `graph` / `context`），client 侧 9 张。
- **生命周期契约**：`create` 只构造上下文；`start` 是**注册事务**（在宿主 IO 线程执行，失败返回 `NULL` + error，宿主回滚）；`stop` 撤销自管资源（线程/定时器/订阅，可重复）；`destroy` 只释放本地对象。缺 `start`/`stop` 的插件宿主拒绝加载。
- **多实例契约**（同库同进程多实例并存）：禁止可变全局/函数级 static 缓存；实例状态只能放 `*plugin_ctx` 堆块；接口表查询结果存实例上下文。
- **宿主侧能力**：`PluginManager`（生命周期骨架来自内核 `pluginxx::PluginHostLifecycle`：装载/启停/禁用启用/卸载/级联依赖/关闭超时重试）、`ToolRegistry`（插件工具与内置工具统一注册、冲突检测、静态工具名集合）、`PluginGraphNode`（插件可注册/修改执行图）、能力协商（`capability_registry`）。
- **中间件机制**（`agentxx/middlewares/middleware.h`）：`BaseMiddlewareHandleInterface` 提供 7 个钩子点（`onAgentcallStart/End`、`onModelcallStart/Run/End`、`onToolcallStart/End`），每个 handle 持有 `states`（按 `sessionId` 分片）与 `toolcalls`（中间件自带工具，`init()` 后由 `initMiddlewareTools` 收集）。
- **内置工具即插件**：`agent/plugins/agentxx_*`（filesystem、execute_command、websearch、rag_search、planning、math、string、system、codegraph、system_monitor、screen_capture…）；`BaseAgent::initTools()` 只放核心（如 `SessionShareStoreTool`），CodeAgent 再加 MCP/GitWorktree。
- **配置装配**：`agentxx-config.yaml` 分两层——base（overlay 的 `data_dir` 目录，缺省 `~/.agentxx/`）+ overlay（工作目录或 `--config`），`model.list` / `plugin.list` 等列表段用 `{overwrite:{mode:merge|replace, remove:[...]}, list:[...]}` 表达继承/替换/剔除。

**源码要点（第 2 版补充，`agent/lib/src/plugins/plugin_manager_vtable.cpp`、`agent/lib/include/agentxx/nodes/wrap_handle.h`）**

- **每个 vtable 入口都有统一前置**：`enterHost()` 校验“传入的 host 指针确属本宿主发放的视图 + 实例未卸载/未关闭”，失败即返回错误（**绝不把调用转交给后来加载的同名实例**）；`allowClosing=true` 只用于只读查询（此时靠 admission lease 保证卸载会等它返回），注册/投递新工作类入口在 Closing/Disabled 之后一律拒绝。
- **写类入口统一切到 IO 线程**：`onInstanceIo()` 把业务逻辑投递到 IO 线程执行，闭包按值捕获参数（跨边界视图只在本次调用内有效），并持实例/管理器强引用 + admission lease（`ioCallSyncKeep` 覆盖“已排队但尚未在 IO 线程执行”的阶段，卸载的 idle 等待会把这段算进去）。
- **接口表自校验**：每张表带 `version` + `struct_size`；查询时若调用方给的 `struct_size` 小于当前结构体（旧布局）会被拒绝，避免“结构体增字段后旧宿主/旧插件互相踩内存”。
- **多来源贡献的合成规则**：可累加的键（提示词段、资源等）遵循“**基础值 ⊕ 按 sequence 顺序的全部存活贡献**”——首次贡献前记录基础值，被外部修改时 rebase，卸载时按 sequence 逆序回退，因此插件的增删改不会互相覆盖。
- **节点包装钩子的错误语义**（`WrapHandleBaseNode`）：用 `startedIdxs` 记录**真正执行过 `onHandleStart` 的中间件**，end 阶段按此**逆序回放**（未开始的不进入 end，保证对称）；异常分两类——控制流异常（取消/中断，`isControlFlow`）**永远重抛**，普通异常允许节点声明“拦截”并以错误结果返回；每个 handle 会收到 `onHandleStartError`/`onHandleBaseRunError`/`onHandleEndError(errorRethrow, isCurrentError, ...)` 回调，用来做自身状态回滚或清理。
- **图定义的可失败替换**：插件改写的图会经 `GraphCompiler::parse` + `GraphValidator::require_valid`；不合法时宿主**回退默认图并记错误日志**（默认图再失败属实现错误，直接上抛）——这是“插件可改循环”的安全阀。

### 1.3 对比

| 维度 | dsh | agentxx | 评价 |
|---|---|---|---|
| 扩展单元 | npm 包（同语言、同进程） | 动态库（跨语言、ABI 稳定） | agentxx 适合“第三方/异构能力”，dsh 适合“快速演化与内省” |
| 依赖注入 | 服务键 + `inject` 声明，加载顺序自动 | 接口表按名查询 + 端口/能力注册 | dsh 依赖关系显式且自动化；agentxx 更接近“能力发现”，但依赖拓扑靠自己维护 |
| 注册可逆性 | 强制：一切注册经 `ctx.effect()`，返回 disposer | 显式：`register_*`/`unregister_*` 成对，卸载时宿主统一撤销（如 `unregister_tool_permission`） | dsh 的“可逆副作用”是语言级约束，漏写会被 review/gate 挡下；agentxx 靠契约与测试保证 |
| 事件模型 | 5 种分发模式（含 waterfall 环绕、bail 短路） | 7 个固定钩子点 + EventBus（请求/响应式 `req*`/`resp*` 消息） | dsh 的 waterfall 让“策略包装而不改循环”成为默认写法 |
| 作用域 | 全局/按 agent 两层 + shadowing + restriction | 工具注册是进程/agent 级；按会话的数据放 middleware `states` | dsh 的 scope 是**能力可见性**维度的解耦；agentxx 目前主要靠“按 sessionId 分片” |
| 装配 | profile + bundle + 有序 patch（含用户 patch/overlay） | 两层 yaml 合并 + 插件清单 | dsh 的 patch 能对**任意条目**替换 config；agentxx 目前是段级合并 |
| 运行期自修改 | 支持（模型写 Cordis 包并挂载，客户端半边需批准） | 支持（动态库 + 资源贡献），但需重新编译 | 结论：agentxx 的“自修改”边界受编译期约束，应把重点放在**热重载/热禁用**而非即时定义 |
| 隔离性 | 同进程同语言，无隔离 | 插件符号隐藏（`-fvisibility=hidden` + version script），只导出入口符号 | agentxx 的符号控制避免了“插件覆盖宿主符号”类事故 |

### 1.4 可迁移到 agentxx 的设计

1. **注册的可逆性写进契约**：为每个 `register_*` 强制提供 `unregister_*`，并在 `PluginManager` 卸载路径上做“未撤销注册”的断言（现有 `flushPendingCleanup`/`disabled` 已是雏形，建议补一条测试：加载→注册→禁用→卸载后，`ToolRegistry`/能力表/hook 列表回到加载前状态）。
   落地：`agent/lib/src/plugins/plugin_manager_lifecycle.cpp`、`plugin_manager_vtable.cpp`。
2. **把“能力作用域”显式化为两层**（全局 / 按 agent）：当前插件工具、提示词追加段、权限声明基本是 agent 级；若将来做“会话级工具集合”（如子代理继承父工具、工作区隔离工具），可借鉴 `scope` + `restriction` 的表达：全局注册 + 按 scope 的 allow/deny 过滤，被过滤项在 schema 与执行两处都不可见。
   落地：`ToolRegistry`（增加 per-agent 过滤视图）、`BaseAgent::initTools`。
3. **细粒度事件点的 waterfall 语义**：agentxx 的 7 个钩子是“通知式”（`onXxxStart` 可改 `NodeInput`，`onXxxEnd` 可改 `NodeOutput`），但缺少 dsh 那种“监听器可以包装下游返回值/短路决策链”的统一写法。可先在**工具执行**这一条链上做试点：`pre-execute`（可改参数/拒绝/标记需审批）→ `execute`（可环绕：超时、重试、计量）→ `post-execute`（检查/替换结果/追加上下文），每个环节都是“有序监听器链 + 链尾为真实执行”。
   落地：`ToolcallWrapNode::execTool`（现在是硬编码流程 + 中间件钩子）。
4. **配置条目级 patch**：dsh 的 `--patch` 能按 id 替换任意插件 config，且能打印最终装配树（`--dump-config`）。agentxx 已有 `model.list`/`plugin.list` 的 merge/replace/remove，建议扩展为“按插件名/模型名定位并替换其整个 config 子树”，并提供对应的“导出最终合并结果”能力（便于排障与文档化）。
   落地：`agent/lib/src/agent/config.cpp`、`agent/client/src/config_loader.cpp`。
5. **装配树可打印**：为 agentxx 增加 `--dump-config` 等价物，输出（模型列表、插件列表及来源层、中间件顺序、工具名清单、图定义），这是排查“插件改了图/加了工具”的最低成本手段。
   落地：`agent/client/src/main.cpp`。当前已有的 `agentxx.agent.graph` 接口表可复用为数据源。

---

### 1.5 各自做得好的设计

**agentxx 相比 dsh 做得好的：**

- **纯 C ABI 换来跨语言与二进制稳定**：8 字节对齐 + 定长类型 + 出参返回码 + 显式调用约定，第三方可用任意语言实现插件，插件与宿主各自链接自己的标准库/工具库而不互相干扰；dsh 的插件必须与宿主同语言同进程。同时插件侧符号被 `-fvisibility=hidden` + version script 收敛到少数入口，避免第三方静态库符号污染宿主。
- **版本自校验落到结构体层面**：全局 `PLUGINXX_API_VERSION` 兜底、每张表自带 `version` + `struct_size`，加载与调用时拒绝旧布局，避免“老宿主 + 新插件按新偏移读字段”这类 C 结构 UB；dsh 靠类型系统与声明合并，不需要这层。
- **内置插件与动态插件同构**：`PluginxxBuiltinInfo` 让同一份插件源码既能编译进宿主也能编译成 `.so`，热插拔路径与内置路径共用一套生命周期；dsh 里 bundle 行与树外插件是两种形态。
- **多实例契约说得清楚且可检验**：禁可变全局/函数级 static、实例状态只能放 `*plugin_ctx` 堆块、接口表查询结果存实例上下文 —— 同一份 `.so` 在同一个进程里被多个 agent 各自创建实例是受支持的一等场景。
- **依赖顺序静态可打印**：`plugin.yaml` 的 `depends` 经 Kahn 拓扑排序得到装载顺序，配置即顺序、顺序即日志；dsh 的装载顺序由运行时服务可用性决定，排障时需要理解框架内部状态。
- **工程化细节更多**：进程级 `alloc/free` 作为跨 CRT 堆的唯一通道；能力注册册可列举；`plugin.yaml` 的 `resources`（skill/memory/mcp）让插件自带资源声明而不必写代码。

**dsh 相比 agentxx 做得好的：**

- **依赖注入，无需人工编排**：插件声明 `inject` 即可，加载顺序、等待就绪、依赖变化重启都由框架负责；agentxx 需要自己维护依赖图并显式级联。
- **注册的可逆性由框架强制**：一切贡献经 `ctx.effect()` 登记，卸载时逆序执行、异步会被等待、生成器可边启动边登记，并有 `EffectMeta` 标签树可诊断；agentxx 靠 `register_*`/`unregister_*` 成对约定加宿主兜底清扫。
- **作用域隔离是框架能力**：`ctx.isolate` 让同名服务在不同作用域解析到不同实现，preset 还有“root realm 服务泄漏检测”兜底；agentxx 的隔离靠注册归属与文件系统边界，没有“同名字、不同实现”的表达。
- **组合层（profile/bundle/patch）成熟**：patch 按 id 覆盖整段 config、多层叠加有序、`--dump-config` 可打印最终树、volatile 字段改值不重挂载；agentxx 目前是段级合并 + 两级 yaml。
- **运行期自修改有完整边界**：模型可定义并运行带版本的插件，配合 vm 沙箱、注册边界 guard、人类批准；agentxx 的能力扩展需要重新编译与重启/重载。

---

## 2. Agent 循环与轮次语义

### 2.1 dsh：turn/step 驱动器

**层次词汇**（`docs/glossary.zh.md#循环层级`）被严格区分：

- **步骤（step）** = 一次模型请求 + 它引发的工具执行；
- **轮次（turn）** = 一次对已接纳输入的排空过程，包含 0 个或多个步骤；
- **Round** = 承载一个轮次的外层策略迭代（Goal Round / Ralph Round），计数器归策略所有。

**一轮的完整流程**（`docs/architecture.zh.md#turn-flow`）：

```text
turn/start
  claim next-step input + one queued message
  assemble prompt sections + tool schemas; project runtime context
  -> agent/pre-step   reject | enter(messages, startsRequestSeries?)
     step/start
     agent/request -> prepareCall
     reconcile system/message; append user/message; log request/header|context
     derive + freeze model history
     stream bound prepared call -> llm/stream -> agent/assistant-stream
     tool/call* -> tools/pre-execute -> tools/execute -> tools/post-execute -> tool/result*
     step/end
     tools owe another request, or next-step input arrived -> claim -> next step
  -> agent/turn-stopping
turn/end
```

**几个值得注意的语义决定**：

1. **inbox 分两个边界**（`next-turn` / `next-step`）：`followup()` 进 next-turn（自己独占一轮），`steer()` 进 next-step（在最近的下一个步骤边界被消费），`inject()` 只排队不唤醒（供插件投放上下文，如文件变更通知、子目录 AGENTS.md、skill 内容、cron 通知）。所有 inbox 变更都经结构化 `splice` 记录成持久事件（`agent/inbox/spliced`），并有 `inserted`/`claimed`/`discarded` 通知 —— **待处理队列本身也是可回放状态**。
2. **`agent/pre-step` 是请求推导前唯一的 waterfall**：监听器可以改写或拒绝已领取的输入；“首次领取被拒绝或为空”会**关闭一个不含步骤的轮次**（而不是塞一条空请求给模型）。
3. **通知义务的显式化**：`agent/request`/`prepareCall()` 期间取消**既不提交系统提示词也不提交用户消息**；`system/message`、`user/message`、`request/header` 的记录顺序被固定，使“每个请求都能从日志重建”（invariant 配套 `src/invariant.ts` 会在运行时校验）。
4. **重试不重复前置工作**：`agent/request-error` 的监听器返回 `{kind:'retry'}` 即在同一打开的步骤内重试，复用已渲染的组装结果，**不重复 `agent/pre-step`、不重复用户消息准入**；未处理的失败保持终态。
5. **取消是显式建模的**：`AgentCancelCause = user | parent | hook{reason} | disposed`，第一因胜出；`turn/end` 持久记录 `{kind:'aborted', reason}`；被取消的流会以 `assistant/message {interrupted:true}` 提交“用户已经看到的文本前缀”，而**未分发的工具调用**被合成 `tool/call` + `ABORTED_BEFORE_DISPATCH` 结果，保证下一轮历史与用户所见一致。
6. **状态只有两个值**：`idle`/`running`（`running` 覆盖整个排空区间，可能跨多个连续轮次），`whenIdle()` 观察整 agent 停稳；另有 `runMaintenance()` 允许在真正空闲期跑“非轮次维护任务”。
7. **并行工具调度的分类**：每个待处理调用经 `isConcurrencySafe(args)` 分类为 `parallel` 或 `exclusive`；独占调用形成排序屏障，并行调用走有界滚动池（`maxParallelToolCalls`，默认 10）；**结果按模型顺序提交**（`src/tool-calls.ts`）。
8. **没有内置轮次预算**：包 README 明确“限制失控轮次的策略必须从既有生命周期扩展点（如 `agent/turn-stopping`）执行取消”——策略留在插件，循环保持中性。

**源码要点（第 2 版补充，`packages/core/agent-loop/src/{agent.ts,index.ts,inbox.ts,tool-calls.ts}`）**

- **相位机是三态而非两态**：内部 `Phase = idle | maintenance | running`，对外 `status` 把 `maintenance` 也报成 `idle`；`runMaintenance()` 仅在 `idle` 相位可用，否则**同步抛错**（`already has active work`），任务结束后若期间有唤醒请求且队列非空则补一次 `wakeDriver()`。
- **唤醒闩锁（wake latch）**：`wakeDriver()` 在“维护中”或“上一个活动已中止”时**不启动驱动**，而是把 `wakeRequested` 置位，等收敛后再补跑；`cancel()` 会清闩锁；`disposed` 原因的取消**永不闩锁**（teardown 不等待任何模型轮次）。`send()` 在插入前先判定 `wakingAfterAbort`（一旦活动已被中止，唤醒输入被改投 `next-turn`），避免被 splice 观察者的重入取消改变分类。
- **`whenIdle()` 的语义**：循环等待 `activityDone` 直到它不再变化（即“观察到的工作全部停稳”），因此替换性工作也会被跟随。
- **一轮的代码骨架**：`turn()` = append `turn/start` → 循环 `preStep` → 空首批/首次被拒则不开步骤直接收尾 → `step/start` → `step()` → `finally step/end` → 无 pending 时跑 `agent/turn-stopping` → 还有 pending 就把 `target` 切到 `next-step` 再开一步；`turnEnds` 的 **max-tokens 是粘性的**（后续正常完成的步骤不能把它降级）。
- **失败与取消的落库**：`turn/end` 在 `finally` 中写入；被取消时 reason 为 `{kind:'aborted', reason}`（原因从 `AbortSignal.reason` 复制，只取日志能表达的字段，避免把 Node fetch 附加的 `stack` 写进日志）；其他失败写成 `{kind:'error', error: LlmError.failure 或 {message: errorChain(error), code:'UNKNOWN'}}`。
- **`step()` 的重试内循环**：每次尝试都重新 `prepareRequest`（`agent/request` waterfall → `llm.prepareCall`，容忍 `NO_ADAPTER` 以便中间件自行为未注册路由提供 provider），然后**协调系统提示词节点**（`SystemPromptProjection.project(...)` 返回逐条提交，带 `surfaceOp` 意图）、仅在首次尝试追加已准入的用户消息、`buildRequest()`（写 header/context、生成工具增删的 `developer/message`、冻结请求）、最后流式消费。
- **流式与结算**：`AssistantStreamAttempt` 负责进程本地的 start/chunk/end 帧；异常时若已有交付内容则结算 `assistant/message {interrupted:true}`（携带已交付前缀与 usage），否则结算 `assistant/attempt`；结算失败会抛 `AggregateError`（把原始错误与结算错误一起带上）。
- **重试动作**：`agent/request-error` waterfall 返回 `{kind:'retry'}` 时 `continue` 同一内循环（复用已渲染组装结果，不重复 pre-step、不重复用户消息准入）。
- **工具调度**：`executeToolCalls` 按 `executionMode()` 分组；`runGroup` 里 `commitReady()` 只提交**连续的模型顺序槽位**，`fillPool()` 在每次启动前**重读执行模式**（注册表在运行中被改动时可以形成新的屏障），并用 `Promise.race(inFlight)` 推进；中止时先结算已启动调用，再为每个未启动调用补写 `tool/call` + 错误结果（`sourceEventSeqs: [callSeq]`），从而“重放仍然有效”。
- **请求冻结**：`buildRequest` 对每个消息对象只冻结一次（`WeakSet` 记录已冻结对象，避免持有被替换的历史），随后 `Object.freeze` 消息数组并冻结整个请求对象（`markAgentLoopRequest`）。
- **创建事务与拆除顺序**（`index.ts`）：`prepare()` 在任何资源产生**之前**就把三个中止源（调用方 signal、所属 fiber 卸载、工厂 teardown）融合进一个 `AbortController`；反向拆除是**记忆化**的：`abort → 等发布完成 → machine.cancel({kind:'disposed'}) → whenIdle() → scope.dispose() → handle.close()（排空并释放写所有权）→ 摘除注册 → 记账`，多处失败会聚合成单个错误或 `AggregateError` 抛出（绝不静默）。
- **恢复流程**：`resume` 先 `open(id,'write')` 拿写所有权（排除同进程并发恢复）→ 冷读日志 → `interruptedTurnClosers()` 计算并**经同一句柄**追加闭合事件 → 用 `seed = persisted + closers` 构造会话。`create` 侧的 seed 与 setup 期事件不经 `session/event`，由 `appendUnstoredSuffix()` 在发布提交点一次性补写；游标按“已存储条数”推进（等待期间新 append 的事件留到下次 flush）。
- **配置声明式 agent 的健壮性**：启动时校验 `sessionId`/`resumeSessionId` 互斥与“精确身份不得重复”；重挂载时只把 `SessionPersistenceNotFoundError` 当“首次创建”，损坏/所有权冲突/后端故障**保持报警**；同 id 正在拆除时会先等 `agent/disposed`/`session/disposed`。
- **工厂级所有权**：`FactoryOwnership` 维护 `accepting`、teardown `AbortController`、活跃 agent 与启动任务集合；`dispose()` 先置止收与中止信号，再等待所有活跃 agent 与启动任务；启动失败经 `agent-loop/config-start-failed` 事件广播（监听器异常只记日志）。
- **投影自带**：`AgentLoop` 注册两个投影单元 —— `turnBoundary`（折叠 turn/step 边界，供“重挂载时取 lastTurn”和 UI 读取）与 `inbox`（待处理队列的完整折叠，冷消费方也能读）。
- **提示词变量由循环提供**：`provider` / `model` / `cwd` 三个变量分别取自 agent options 与 `session.header.cwd`（见 §6）。

### 2.2 agentxx：图引擎驱动的 ReAct 循环

- **一次 `runTurnAsync(sessionId, userInput, io, modelName, attachments)`** 就是一轮：构造/复用 `CancelToken` → `engine->run_stream_async(...)` → 返回 `TurnResult{hasError, errorMessage, interrupted}`（`agent/lib/src/agent/base_agent.cpp`）。
- **循环由执行图表达**：默认图 `agentxx.default` 为 `__start__ → agent_start → llm → [条件边 xx_has_tool_calls] → tools / agent_end → __end__`；`llm` 节点按本轮是否有 tool_calls 返回 `Command.goto_node`，节点级 `xx_autoRoute=false` 可交回图定义路由。插件可经 `agentxx.agent.graph` 接口表改写图定义（非法图会被 `GraphCompiler/GraphValidator` 拒绝，宿主回退默认图并记日志）。
- **节点即扩展点**：`AgentStart`/`AgentEnd`/`ModelCall`/`Toolcall` 四个核心节点 + `WrapHandleBaseNode` 模板负责把中间件钩子包在每个节点的 start/run/end 上（`nodes/wrap_handle.cpp`）。
- **中间件**：7 个钩子点（agentcall/modelcall/toolcall × start/end + modelcall run）；中间件可自带工具（`initMiddlewareTools` 收集）；每个 handle 的 `states` 按 `sessionId` 分片，`graphData` 也按 `sessionId` 分片，轮末清理。
- **取消**：`CancelToken` 传入 engine 配置，工具执行时透传给 `ContextualAsyncTool`，工具自行轮询或下传传输层；取消经 `WireCancel` 从客户端到达 agent 线程，`runTurnAsync` 把取消映射为 `errorMessage = "Cancelled by user"` 并插入一条 Tip 消息。
- **用户输入排队**：`SessionServerAgentIO` 维护 `messageQueue_`（`MessageQueueItem` + `WireMessageQueueUpdate`），运行中的输入入队，轮末出队继续；并有 `WireClearMessageQueue`/`WireRemoveQueueItem`/`WireInterruptAndRunNext`（中断当前轮并立即跑队列）。
- **中断（HIL）与恢复**：工具/中间件可 `requestInterrupt()` 抛 `NodeInterrupt`，携带 `InterruptHandleArg`（含声明式 UI 描述 `InterruptUi`）；客户端渲染为表单，回传 `{"values":{...}}`；`graphData` 中的中断现场被序列化进图状态通道（`xx_interruptNode`/`xx_interruptValue`/`xx_interruptToolcallCache`），程序重启后可经 `AgentRunner` 的 `resume_async` 恢复执行。
- **活动状态**：`SessionActivity{Idle, Streaming, ExecutingTool, WaitingInput}` 供 UI 显示。

**源码要点（第 2 版补充，`base_agent.cpp` / `modelcall.cpp` / `toolcall.cpp`）**

- **轮首插件钩子**：`runTurnAsync` 开头调 `pluginManager->flushPendingCleanup()` + `onTurnBegin()` —— 上一轮异常路径残留的待摘除中间件在这里自愈，同时登记“本轮进行中”供插件 `disable` 的立即/延迟生效判定。
- **引擎 RunConfig 的三个关键取值**：`max_steps = 1 << 30`（实际上是“不限步数”，失控保护交给策略而非循环）；`stream_mode = EVENTS|TOKENS|UPDATES`，**刻意去掉 `VALUES`**（图状态里已不存上下文，每 super-step 的全量状态序列化没有消费者）；`resume_if_exists = false` 且注释明确解释了原因：checkpoint 只在进程内存活，端点重建（客户端重连/切回会话）时引擎仍持有该线程的旧 checkpoint，若为 true 会先用旧 checkpoint 里的控制通道恢复，与当前会话数据不一致；中断恢复走 `AgentRunner` 的 `initialResult/resume_async` 路径，与本标志无关。
- **中断恢复的判定与重建**：若 `graphData` 中缺该会话数据，则从引擎状态里取 `xx_savedGraphData`；存在则 `setGraphDataFromState(...)` 并置 `resumeInterrupt`，随后**跳过首跑**，用 graphData 里的 `xx_interruptNode`/`xx_interruptValue` 合成一个 `interrupted=true` 的 `RunResult`，直接进入“中断处理 + resume”循环。
- **统一的运行/中断/恢复循环**：`AgentRunner::run(...)` 被主 agent 与子代理共用（hooks: `eventCallback`/`onInterruptTip`/`onBeforeResume`/`onRunResult`）；中断头提示由 agent 线程插入会话历史（UI 不再自己构造）。
- **取消与错误的区分**：整段执行包在 `agentxx::util::catchErrorAsync` 里，三个处理器分别对应“异常”“取消（`operation_aborted` → 按取消语义）”“错误字符串”；取消把 `TurnResult.errorMessage` 设为 `"Cancelled by user"` 并插入 `[Cancel Request]` Tip，异常插入 Error Tip —— **两者都只是 UI 提示，不写回上下文**。
- **轮末**：清理 `xx_savedGraphData`；会话上下文保持权威（**不回滚**，节点抛出前已写入的消息保留）；先插入“轮次统计”Tip 再 `flushViewMessages()`（保证统计提示确实落盘）。
- **模型调用前的上下文修复**（`ModelCallWrapNode::repairMessages`，每次调用前执行）：
    - 末尾消息若不是 `system`/`user`/`tool`，补一条 `AutoInserted` 的 `user`“请继续”消息（不产生通道事件、不在 UI 显示）；
    - 清理**悬挂的 tool_calls**（声明了 tool_call id 但到下一条 agent 消息之前没有对应 tool 结果的 assistant 组）：清空其 `tool_calls` 并删除其后的孤儿 tool 结果 —— 因为 provider 会直接 400（OpenAI `invalid tool_call_id` / Anthropic `orphan tool result`）；
    - 修复**重复的 tool_call id**（LLM 重试复用 id 或 provider 自动回填的 `call_{i}` 撞车）：按出现顺序重命名，并一一回填对应的 tool 结果 id；
    - 合并**连续 user 消息**（部分 provider 要求 user/assistant 交替）。
- **重试语义**（`ModelCallWrapNode::baseRun` 的 `do/while`）：重试前清掉临时 thinking/content（graphData）、重跑 `onModelcallRun` 钩子（**契约要求可重复执行**）、再 `repairMessages`、再做一次取消埋点；`retry` 计数**不因部分输出而重置**（保证总失败次数 ≤ `llmMaxRetry`），退避为 `retry*3s`（命中限速再加 `retry*5s`），并把同一数值同时用于日志、UI 提示与定时器；≥512 字符的部分输出会先落成一条 `AutoInserted` assistant 消息再重试；重试耗尽/取消时若末尾不是“无 tool_calls 的 assistant”，补一条兜底 assistant 消息，避免悬挂 tool_calls 被路由回 tools 节点。
- **工具调用的执行与取消**（`ToolcallWrapNode::baseRun`）：
    - 自动向工具参数注入 `sessionId` 与 `tool_call_id`；
    - 先查 `toolcallsCache`（按 `tool_call_id`）复用中断前已完成的结果；
    - 顺序执行（`for` + `co_await`，代码保留 `// TODO: 真正并行`）；
    - 每个工具执行前后各有一次 `cancel_token->throw_if_cancelled(...)` 埋点；
    - **取消分支**把已完成结果与未完成调用的 `[User canceled]` 占位一并写入会话（`AutoInserted`），再重抛取消异常 —— 上下文角色顺序与“每条 tool_call 都有结果”的完整性由此保证；
    - **中断分支**把本轮 toolcall 结果缓存进 `graphData`、保存上下文 JSON，再抛 `NodeInterrupt`（供 resume 后原样恢复）。
- **插件工具的安全卸载**：静态工具表未命中时查 `toolRegistry->find(name)`，返回 `shared_ptr` 以**保持插件代码段存活**（与插件的 inflight 计数配合，计数归零后才 `dlclose`）；找不到则回 `[Error] Tool not found: <name>`。
- **异常文本约定**：工具/钩子异常统一格式化为 `[<阶段>/Exception aborted: <msg>]`（`start`/`baseRun` 阶段前缀可区分），取消用 `[User canceled]`，权限拒绝用 `[Permission denied]`。

### 2.3 对比

| 维度 | dsh | agentxx |
|---|---|---|
| 循环表达 | 代码状态机（驱动器内部显式 turn/step） | 执行图（节点 + 条件边，可被插件替换） |
| 轮次/步骤可见性 | 一等公民：持久事件 + 实时事件 | 隐含在图的 super-step 中，UI 只见 delta/结果 |
| 输入通道 | `followup` / `steer` / `inject` 三通道 + 持久 inbox | 单通道消息队列（轮末消费）+ 中断-恢复通道 |
| 前置拦截 | `agent/pre-step` waterfall（可改写/拒绝，可关空轮） | 中间件 `onAgentcallStart/onModelcallStart`（可改 `NodeInput`，无“拒绝本轮”语义） |
| 请求期取消语义 | 取消不提交系统提示与用户消息（显式不变量，`agent.ts` 中先 `throwIfAborted` 再 append） | 取消时**已完成的部分照常写入会话**（不回滚），以 Tip 消息 + `[User canceled]` 占位表达 |
| 重试语义 | `agent/request-error` 可重试且复用组装结果与已准入的用户消息 | `ModelCallWrapNode` 内部重试：退避、部分输出保留、重试计数不重置；无外部策略点 |
| 未分发的工具调用 | 调度器为未启动调用补 `tool/call` + `ABORTED_BEFORE_DISPATCH` 错误结果（并引用 call 事件的 seq） | `ToolcallWrapNode` 取消分支补 `[User canceled]` 占位；另有 `repairMessages` 读时兜底（清悬挂 tool_calls、删孤儿结果、修重复 id、合并连续 user） |
| 上下文一致性策略 | **写时占位**（取消即补结果，历史随时自洽） | **写时占位 + 读时修复**（节点补占位，模型调用前再统一修复一遍） |
| 并行工具 | 分类 + 有界池 + 顺序提交；注册表把策略/执行/提交拆成四段以允许执行体重叠 | `ToolcallWrapNode` 明确顺序执行（`// TODO: 真正并行`） |
| 失控保护 | 无内置预算，交 `agent/turn-stopping` 策略 | `max_steps = 1 << 30`（实际不限）；有“连续重复调用检测 + 询问”与压缩失败兜底 |
| 引擎/检查点参数 | 持久化由句柄 + flush 检查点控制，恢复以日志为准 | `resume_if_exists = false`（checkpoint 仅进程内）、`stream_mode` 去掉 `VALUES`、中断恢复经 `AgentRunner` + `graphData` |

**dsh 更强的地方**：语义被命名、被持久化、被测试（`tests/loop.spec.ts` 79 KB、`cancel.spec.ts` 50 KB、`resume.spec.ts` 63 KB）；任何“用户能看到的东西”都能从日志重建。**agentxx 更强的地方**：循环本身可被插件重排（图定义），节点级包装钩子天然支持“在任意节点前后插入行为”，且协程模型让多会话交错执行不需要线程锁。

### 2.4 可迁移到 agentxx 的设计

1. **区分“下一步输入”与“下一轮输入”**：现在只有单一队列。建议给队列项加 `target: next-turn | next-step` 语义——`next-step` 项在当前轮的下一次 modelcall 之前注入（相当于 steering），`next-turn` 项维持现有行为。这是成本最低、体验提升最明显的一项（用户可以在 agent 干活途中补充约束而不必等整轮结束）。
   落地：`wire_protocol.h`（`WireUserInput` 增加字段）、`SessionServerAgentIO::pushMessageQueueItem`、`BaseAgent::runTurnAsync` 消费点、`nodes/modelcall.cpp` 的入队检查。
2. **独立的“注入上下文”通道**：插件/中间件需要往模型上下文里放“非用户输入”的信息（如工作区文件变更通知、加载的 skill 内容、MCP 工具列表变化）。dsh 用 `agent.inject()` + `source` 标注来源，且**不唤醒驱动器**。建议给 agentxx 加 `appendInjectedContext(sessionId, text, source)`（写入会话上下文并标注来源，UI 侧按来源分类展示）。
   落地：`nodes/session_context.h`（新增入口，沿用 `appendSessionMessages` 的事件语义）、`conversation_types.h`（消息来源标记）。
3. **空轮与拒绝语义**：让中间件能在“进入模型之前”拒绝本轮（例如权限中间件发现全部输入被拒），并让 `runTurnAsync` 返回可区分的 `TurnResult`（`skipped`/`rejected`），而不是照常请求模型。当前 agentxx 用“取消 + Tip 消息”表达，语义上更含糊。
   落地：`nodes/agentcall.cpp`、`BaseAgent::runTurnAsync` 的返回结构。
4. **轮次结束原因枚举化并落库**：把 `TurnResult` 扩展为带原因枚举（`completed` / `aborted_by_user` / `aborted_by_parent` / `failed` / `interrupted` / `max_steps`），并把原因随消息历史一起落库。恢复会话时可据此判断“上一轮是被中断的”，从而做 dsh 那样的崩溃配平（见 §4）。
   落地：`agent/lib/src/agent/session_store.cpp`（meta/轮次表）、`TurnResult`。
5. **重试策略点**：把“请求失败后是否重试”抽成中间件可介入的决策（对应 `agent/request-error`），而不是散在 provider/节点内部；重试时复用已渲染的提示词与工具 schema，避免重复执行 `onModelcallStart` 里的副作用（如再次插入提示消息）。
   落地：`nodes/modelcall.cpp` 的失败分支 + 一个 `retryPolicy` 钩子。
6. **未分发工具调用的占位结果**：取消发生在“模型已返回 tool_calls、尚未执行”时，为每个未执行调用写入一条“已中止”的工具结果，保证下一轮上下文自洽（否则模型会看到“调用了但没有结果”的历史）。
   落地：`nodes/toolcall.cpp` 的取消路径。

---

### 2.5 各自做得好的设计

**agentxx 相比 dsh 做得好的：**

- **循环本身是数据**：默认循环是一份可被插件改写/替换的图定义（节点 + 条件边），插件能在不改核心代码的前提下重排循环；dsh 的循环是代码，替换循环等于替换包。
- **中断（HIL）可以跨进程恢复**：`NodeInterrupt` 会把中断现场（节点名、携带值、已完成的工具结果缓存）序列化进图状态，进程重启后 `AgentRunner` 能恢复并继续；dsh 的中断是轮内的询问流程，没有“执行现场”的概念。
- **钩子既能改输入又能改输出，且有对称性保证**：`onXxxStart` 可改 `NodeInput`、`onXxxEnd` 可改 `NodeOutput`，`startedIdxs` 保证 end 阶段只回放真正开始过的中间件且逆序执行；控制流异常（取消/中断）永远重抛，普通异常可由节点声明拦截。
- **取消埋点密度高**：LLM 调用前、每个工具执行前后都有 `throw_if_cancelled`，且取消时会为未完成的工具补 `[User canceled]` 占位、模型调用前还有 `repairMessages` 兜底，历史一致性有双保险。
- **重试对用户可见**：每次自动重试都会推一条 UI 提示，且提示里的等待秒数与定时器用同一个变量，不会出现“提示 5 秒实际等 8 秒”。
- **单线程协程 + 线程池卸载**：多会话在同一 io_context 上交错执行而不需要锁，阻塞/CPU 操作（附件读取、base64）显式卸载到线程池，资源占用可预测。

**dsh 相比 agentxx 做得好的：**

- **轮次与步骤是一等语义且可持久化**：`turn/start|end`、`step/start|end` 是日志事件，`TurnEndReason` 是枚举，任何“这轮为什么结束”都能回放回答；agentxx 的轮次边界只存在于控制流里。
- **取消被显式建模**：`AgentCancelCause`（user/parent/hook/disposed）+ 第一因胜出 + 取消原因随 `turn/end` 持久化；并且明确规定“请求期取消不提交系统提示与用户消息”。
- **输入分三通道**：`followup`（独占一轮）/`steer`（下一 step 边界消费）/`inject`（只排队不唤醒），且 inbox 变更本身就是持久事件（`splice` + inserted/claimed/discarded 通知）；agentxx 只有单一队列 + 中断恢复通道，没有 steering 语义。
- **`agent/pre-step` 是唯一的请求前拦截点**：监听器可改写或拒绝已领取输入，首次被拒/为空时会**关闭一个不含步骤的轮次**，而不是硬塞一条空请求给模型。
- **重试语义干净**：`agent/request-error` 返回 `{kind:'retry'}` 时复用已渲染的组装结果重试，**不重复 pre-step、不重复用户消息准入**；工具调度是“策略有序 + 执行体可重叠 + 结果按模型顺序提交”。
- **边界条件被显式建模并测试**：max-tokens 粘性、唤醒闩锁（wake latch）、维护相位（maintenance）、`whenIdle()` 跟随替换性工作等，都有对应实现与测试。

---

## 3. 会话与 LLM 上下文

### 3.1 dsh：事件溯源日志 + 派生历史

- `Session` 是一份**仅追加的 `SessionEvent` 日志**（架构文档列出的核心事件为 13 种：`turn/start`、`turn/end`、`step/start`、`step/end`、`user/message`、`system/message`、`assistant/message`、`assistant/attempt`、`tool/call`、`tool/result`、`request/header`、`request/context`、`session/end-seed`；`developer/message` 与 `compaction/*`、`approval/*`、`hook/*` 等由各子系统经声明合并加入），是唯一真源；**LLM 消息历史由 `deriveMessages()` 从日志投影**，从不单独存储。
- **Surface 事件与 `surfaceOp`**：产生消息的事件（system/user/assistant/tool-result）带 surface 元数据，声明“如何加入有序派生 surface”，可以指向更早的事件做**替换/遮蔽**。这使“压缩替换旧历史”“提示词变更替换第 0 号系统节点”都只是**追加一条新事件**，而不是就地改写历史。
- **仅追加 + 可扩展事件表**：`SessionEventMap` 用声明合并扩展（compaction、hook 桥接各自加事件）；未识别事件默认**必须拒绝重建**，只有显式标记 `ignorable: true` 的纯信息事件才能跳过 —— 宁可过度拒绝，也不静默回放被掏空的会话。
- **每个 assistant 消息嵌入精确的紧凑 stream**：`assistant/message` 携带产生它的 stream 与 `usage`；失败/重试/取消的尝试则以 `assistant/attempt` 留痕而不伪造模型历史。恢复 UI、transcript、遥测、持久化全部从这些持久 settlement 派生。
- **不变量“模型可见即已记录”**：任何进入模型请求的东西都必须能从日志重建；新增模型可见输入**必须**新增会话事件（`packages/AGENTS.md`）。`src/invariant.ts` 与 `tests/request-reconstruction.spec.ts`（44 KB）把这条约定变成可执行检查。
- **投影 seam**（`docs/subsystems/session-projection.zh.md`）：领域插件注册**纯同步折叠单元**（`init` + `apply(state, event) → state`），注册表负责驱动；客户端只收到“成品值”（`wire.view(state)`），从不自己折叠。`snapshot()` 在一个 tick 内给出所有单元的**一致读切面**（`asOfSeq` 水位线）。

**源码要点（第 2 版补充，`packages/core/session/src/{index.ts,surface.ts}`）**

- **读写分离的两层实现**：`Session` 内部用 `SurfaceManager` 做**增量折叠**（不是每次读都全量重放）；`deriveMessages()` 维护一个“派生消息缓存”，只对尚未见过的节点扩展投影，已冻结的消息对象保持同一身份 —— 这使“同一消息在多次请求间逐字节一致”成为可依赖的性质（KV Cache 前缀复用的前提）。
- **transcript 与模型 surface 是两个视图**：`isAppendSurfaceEvent()`（从未作为替换副本出现的事件）才是**人类 transcript 的持久素材**；`isReplacementSurfaceEvent()` 的替换副本“只对模型可见”。这条区分避免了“压缩落地后用户已经看过的对话被擦掉”。
- **节点 0 保护**：覆盖 surface 第 0 号节点的替换必须**本身是一条 `system/message` 且范围恰为该节点**；后续系统节点不受保护（压缩可以遮蔽它们）。
- **工具结果替换的限制**：替换只能作用于某一个当前结果的 `content`（不能借“替换”把结果换成另一条消息的身份）。
- **空内容不产生消息**：`deriveEventMessage()` 对内容为空的 system/developer/assistant 返回 `null`（因此“空渲染的系统提示词”等于“没有系统提示词”）。
- **插件消息投影**：插件可以注册“某个自有事件如何改写既有消息内容”的纯解释器（`SessionMessageProjection`）；`SessionStore` 收集它们，读日志时**必须提供全部已提交的投影**，缺一个就抛错（不允许“少了处理器也能勉强回放”）。
- **写入口做数据校验**：`Session.append` 会校验事件数据是合法 JSON（否则拒绝），并在写入时快照 surface 元数据；`seq = log.length` 单调递增。

### 3.2 agentxx：会话即上下文权威

- **会话（`Session`）持有 typed 上下文**：`std::vector<neograph::ChatMessage>` + `messagesVersion`；写入口只有 `appendMessages` / `replaceMessages` / `replaceMessagesFromJson` / `truncateMessages`（`agent/lib/include/agentxx/agent/conversation_types.h`）。
- **图状态不持有消息**：节点/中间件经 `nodes/session_context.h` 读写；`sessionMessages()` 是只读借用（不得跨 `co_await`），`appendSessionMessages()` 追加并发出 `{"channel":"messages","value":[...]}` 的 CHANNEL_WRITE 事件，由 `EventBridge` 转成 UI 增量与节流落盘；图状态里只保留只读影子通道 `xx_messagesMeta`（条数/版本/角色分布/末条 tool_calls 摘要）。
- **Json 形态惰性生成**：`Session::llmMessagesJson()` 只在落库 / `WireGetContext` / 插件查询时生成。
- **展示历史与 LLM 上下文分离**：`view_message` 表（append-only，每条展示消息一行 JSON，可 update 回填）+ `llm_context` 表（单行整体替换，每轮结束保存）。
- **`xx_autoRoute` 与图条件**：`llm` 节点按本轮是否有 tool_calls 返回 `Command.goto_node`；条件 `xx_has_tool_calls` 读 `xx_messagesMeta.last_assistant_tool_calls`，无该通道时回退扫描 `messages` 通道（兼容旧图）。

**源码要点（第 2 版补充，`agent/lib/src/agent/context.cpp`、`include/.../agent/context.h`、`src/event/event_stream.cpp`）**

- **单一变更入口**：`appendMessages` / `replaceMessages` / `replaceMessagesFromJson` / `truncateMessages` / `appendSettledLlmMessages` 全部经 `markMessagesChanged()` 汇合 —— 该函数同时做两件事：`++messagesVersion_` 与 `llmMessagesJsonDirty_ = true`（Json 形态是**惰性缓存**，脏了才重新生成）。
- **持久化是节流写 + 轮末权威写**：写入口默认 `persistThrottled = true`，经 `requestSaveLlmMessages()` 判断 `kPersistThrottleMs` 窗口；窗口内只置位，等下一次触发或轮末 `flushViewMessages()` 落库。系统提示词重建、`AutoInserted` 提示这类“派生内容”显式传 `false`（不请求节流落盘，交给轮末保存）。
- **展示历史的批量提交与过期保护**：展示消息的写入先进 `pendingViewOps_`（按“追加/更新”操作排队），`flushPendingViewOps()` 批量提交；若期间发生过 `restore()`（整体替换），批量里的**旧下标会被识别为过期并丢弃**（日志记录），避免把恢复后的历史写坏。
- **展示历史有链式哈希**：`restore()` 与 `appendViewMessage` 都维护 `chainHash`（对不含 id 的消息内容哈希），用于客户端校验历史一致性；`msgIndex_`（msgId → 下标）让工具结果的回填是 O(1) 定位。
- **delta 序号统一分配**：`EventBridge::emitDelta` 与 `SessionServerAgentIO` 的新增 WireDelta 都经 `Session::nextDeltaSeq()` 取号 —— 服务端的**增量重放缓冲**依赖该 seq 单调，任何绕过它的新产出都会破坏重放。
- **EventBridge 是“图事件 → 会话/UI”的唯一转换点**：`LLM_TOKEN`（含 thinking 段结算）、`CHANNEL_WRITE`、`NODE_START/END`、`ERROR` 各一个 handler；assistant 消息被**展开**成 Think / Assistant / Tool 三类展示消息（顺序与 TUI 渲染一致，客户端不必再解析 JSON），并对思考时长/加密思考做兜底计算；未知事件类型会重置流式 chunk 类型（视为新流开始）。
- **过期注释（源码核对发现）**：`context.h` 中 `messagesVersion` 的注释写着“中断/异常快照与回滚依赖该版本号”，但全仓只有 `session_context.cpp` 把它写进影子通道 `xx_messagesMeta.version`；实际的中断/异常路径已经改为“会话不回滚”。建议把注释改成现状（版本号用于插件观察与 UI 判断上下文变化）。

### 3.3 对比

| 维度 | dsh | agentxx |
|---|---|---|
| 真源 | 仅追加事件日志（消息是投影） | 会话里的 typed 消息数组（展示历史另存） |
| 历史改写 | 追加 `surfaceOp` 替换/遮蔽事件 | `replaceMessages`/`truncateMessages` 就地改写，随后落库覆盖 |
| 请求可重建性 | 强不变量 + 可执行检查（invariant/e2e） | 有“会话唯一权威 + 影子通道”的口径约定，但没有“从日志重建请求”的校验 |
| 失败/重试留痕 | `assistant/attempt` 持久记录 | 重试/失败只在日志与 UI 提示里，不进入会话轨迹 |
| 事件扩展 | 声明合并 + 未知事件默认拒绝 | 图通道（`channel_*`）扩展，插件可写通道 |
| 内存策略 | 日志在内存（可选缓存），冷读走 session-query 缓存 | 会话消息常驻内存；连接 LRU（`kMaxOpenSessionDbs=32`）、share_store 仅缓存 3 条 |
| 一致性切面 | `snapshot()` 同步一致读（asOfSeq） | 客户端经 Wire 增量同步（delta 缓冲 + 重连重放） |

**优势对照**：dsh 的“历史只能追加”让**压缩、提示词替换、fork、恢复、UI 回放**共用一套机制，代价是事件类型与投影规则多、实现抽象层次高；agentxx 的“会话为唯一权威”让读写路径短、内存与 SQLite 都是直给，代价是**改写历史后无法回放“为什么会变成这样”**，且失败尝试不留痕（同一上下文下重复调试困难）。

### 3.4 可迁移到 agentxx 的设计

1. **给上下文变更加“原因/来源”元数据**：`appendSessionMessages` 已经是唯一追加入口，建议在事件载荷里增加 `source`（`user` / `tool` / `plugin:<name>` / `summary` / `inject:<plugin>`）与可选 `reason`。收益：(a) UI 可按来源分类展示（现在只能靠角色猜测）；(b) 后续做“投影/审计”有依据；(c) 插件注入的上下文可被识别与清理。
   落地：`nodes/session_context.h` 的 CHANNEL_WRITE 载荷、`conversation_types.h` 的消息来源字段、`EventBridge` 的转换。
2. **失败与重试留痕**：把“模型请求失败/重试/被取消”作为一类**只影响诊断、不进入模型历史**的记录落库（dsh 的 `assistant/attempt` 对应物）。当前只在运行日志里，用户复盘压缩/重试行为很困难。
   落地：`session_store` 增加 `attempt` 表（或 meta 段），由 `nodes/modelcall.cpp` 在失败/重试时写入。
3. **历史替换显式化**：压缩、截断、工具去重现在都是 `replaceMessages`/`truncateMessages` 的就地改写。建议增加一条“替换记录”（`{op: replace|truncate, range:[a,b), reason}`）随上下文一起持久化，让恢复与 UI 能解释“上下文为什么变短了”。
   落地：`conversation_types.h` 的 `messagesVersion` 旁增加替换历史；`SessionStore::saveLlmMessages` 一并写入。
4. **一致的读切面**：dsh 的 `snapshot(asOfSeq)` 思路可用来解决 agentxx 的一个潜在问题——客户端拿“上下文条数/用量/消息列表”时可能来自不同时刻。建议在 Wire 层给一次同步响应带上 `messagesVersion`，客户端只接受与自身视图版本一致的数据（`WireContextMessages`/`WireContextStats` 已有各自字段，可统一）。
   落地：`wire_protocol.h` 的 `WireContextMessages`/`WireContextStats`、`session_server_agent_io.cpp`。
5. **消息写入点的收敛审计**：dsh 靠不变量测试保证“没有绕过日志的写入”。agentxx 建议补一条测试：遍历所有 `Session::messages` 写入调用点，断言都经过 `appendSessionMessages`/`replaceMessages` 等受控入口（即在 `Session` 里把裸 vector 设为 private，只暴露这些方法），防止新增代码直接改 vector 导致 EventBridge 不触发。
   落地：`conversation_types.h` 的封装 + `agent/test` 新增用例。

---

### 3.5 各自做得好的设计

**agentxx 相比 dsh 做得好的：**

- **读写路径短**：会话直接持有 typed 上下文（`std::vector<ChatMessage>`），节点/工具读取是一句 `sessionMessages()` 借用；dsh 每次取历史都要走派生投影，虽然可回放但成本更高、理解门槛更高。
- **变更入口收敛且带节流**：所有写操作汇合到 `markMessagesChanged()`（版本号 + Json 惰性缓存脏标记），持久化是节流写 + 轮末权威写；派生内容（系统提示、AutoInserted 提示）显式不请求落盘，避免无意义写。
- **展示与上下文分治且有校验**：`view_message`（可回填、带链式哈希与 msgId 索引，O(1) 定位）与 `llm_context` 分表；展示历史的批量提交还带“过期下标丢弃”，恢复后不会写坏历史。
- **读时修复兜底**：`repairMessages` 在每次模型调用前统一处理悬挂 tool_calls、孤儿结果、重复 tool_call id、连续 user 消息，对“历史里已经存在脏数据”的容错更强。
- **影子通道设计巧妙**：`xx_messagesMeta` 只放与上下文大小无关的元信息（条数/版本/角色分布/末条 tool_calls 摘要），让插件与图条件能观察上下文而不必触碰完整消息。

**dsh 相比 agentxx 做得好的：**

- **仅追加日志 + 派生历史**：压缩、替换、fork、恢复、UI 回放共用同一套机制，历史修改是“追加一条带 `surfaceOp` 的事件”而不是就地改写；agentxx 的 `replaceMessages`/`truncateMessages` 是就地改写，事后无法完整解释“为什么变成这样”。
- **transcript 与模型 surface 分离**：`isAppendSurfaceEvent` 才是人类 transcript 的素材，替换副本只对模型可见 —— 压缩落地不会擦掉用户已经看过的对话。
- **失败尝试留痕**：`assistant/attempt` 记录失败/重试/取消的尝试且不伪造模型历史，每个成功的 `assistant/message` 还嵌入精确 stream 与 usage；agentxx 的重试痕迹只留在运行日志里。
- **未知事件默认拒绝重建**：只有显式 `ignorable: true` 的纯信息事件允许跳过，宁可过度拒绝也不静默回放被掏空的会话。
- **“模型可见即已记录”是可执行不变量**：`invariant.ts` + 44 KB 的重建测试把这条约定变成会失败的检查；agentxx 对应的是文档口径与代码约定。
- **一致读切面**：投影 seam 的 `snapshot()` 在一个 tick 内给出所有单元的一致值（带 `asOfSeq` 水位线），客户端只读成品值，不会自己折叠出偏差。

---

## 4. 持久化与崩溃恢复

### 4.1 dsh：句柄式持久化 seam + 生成版本

- **seam 划分**（`docs/subsystems/persistence.zh.md`）：抽象服务 `ctx.sessionPersistence` 直接在既有 `SessionEvent` 上暴露 `create`/`open`/`stat`/`list`/`export`，**没有平行的“持久化事件类型”**；`create`/`open` 返回**逐会话句柄** `SessionHandle`（`read`/`append`/`flush`/`close`），句柄是“跨进程写租约把守的唯一入口”。
- **单写者所有权**：已有活跃写句柄时第二次 `open(id,'write')` 抛 `SessionAlreadyOwnedError`；读句柄上的修改抛 `SessionReadOnlyError`。这条约束让“崩溃修复”不会与活跃轮次竞速（见下）。
- **耐久性语义分层**：`append` 只承诺“被接受、有序、在本后端实例可见”；**只有 `flush` 承诺“崩溃后仍在”**。`session/flush` 是循环在领取下一个普通轮次之前使用的顺序与错误观察检查点。
- **写后缓冲（write-behind）**：`session/event` 是同步通知，后端把它路由进活跃写句柄的有界窗口，不阻塞生产方；窗口到期后发起一次持久化 `append`；期间接纳的事件形成后续批次。
- **崩溃恢复是“读方的职责”**：持久化层**不截断也不修复**中断的轮次（长任务的单个轮次可能非常庞大），只返回物理上有效的连续日志，丢弃撕裂尾部（JSONL 后端能部分解码撕裂的 Zstandard 帧，完整记录在下次 append 前重写）。修复由 resume（agent-loop）完成：读已存日志 → 计算 `interruptedTurnClosers`（缺失的工具错误、未闭合的 `step/end`、合成的 `turn/end{reason:{kind:'interrupted'}}`）→ 在持有写所有权的前提下作为普通批次追加。
- **格式版本与迁移**：物理文件按代（generation）命名（`session.vN.jsonl[.zstd]`），**已提交的代绝不重命名/替换/删除**；`open` 拒绝未来版本，或只 decode 并组合一次**构建时静态确定的相邻迁移链**（每个迁移包只负责一个 `vN → vN+1`）；写 open 会先在旁边排他发布最终版本命名的后继，再返回校验后的当前逻辑事件。
- **头部与日志分离**：`SessionHeader`（格式版本、cwd、`isSeeded` 谱系位）与精确的 `inheritedEventCount` 存在日志之外，都不进入 `deriveMessages()`。

**源码要点（第 2 版补充，`packages/session/session-persistence-jsonl/src/lease.ts`）**

- **写所有权是内核级锁，不是进程内标记**：POSIX 用非阻塞 `flock(2)` 锁会话目录旁的 `session.lock`；Windows 用**命名内核信号量**（由同一路径派生），因此“读者、检索、目录删除”都不受写锁影响。
- **锁的持有期 = 写句柄的生命周期**：竞争映射为 `SessionAlreadyOwnedError`；进程崩溃时内核自动释放（持有者 fd/句柄关闭即释放），**故意没有超时过期**：宁可让一个卡住的写者继续持有，也不冒“被抢占后两处 append 交错撕开日志”的风险。
- **inode 校验**：POSIX 的锁属于 inode 而非路径，加锁后要确认“锁住的 inode 仍是该路径上的文件”，否则重试（有人 unlink 后重建了锁文件）。锁文件**永不删除**（保留稳定 inode）；浏览器 worker 直接 stub 掉 flock（单进程，进程内写声明已足够互斥）。
- **懒物化**：会话 `create` 不产生任何文件；锁只在“对已存在产物 write-open”或“首次可物化写入之前”获取 —— 未物化的会话在崩溃后等于从未存在。

### 4.2 agentxx：SQLite 单库 + 图检查点

- **目录结构**：`{dataDir}/sqlite/sessions/{sanitizedSessionId}/session.db`，单库四张表：
    - `view_message`：展示历史，append-only，每条消息一行 JSON，可 `updateViewMessage` 回填（工具结果、折叠状态）；
    - `llm_context`：LLM 上下文消息，**单行整体替换**，每轮结束保存（`saveLlmMessages`）；
    - `meta`：`msgIdCounter`、session 摘要（sessionId/title/lastActiveMs）；
    - `store`：share store KV（自增 id → 文本，内容超限时把长文本外置）。
- **连接管理**：`kMaxOpenSessionDbs = 32` 的连接 LRU（fd + WAL + page cache 占用考虑），关闭后下次写入自动重开；会话枚举用两阶段扫描（先 stat 拿近似顺序，再逐库读精确 meta），支持 keyset 游标分页与早停。
- **恢复**：`SessionStore::loadSession` 同时取回 `viewMessages` + `llmMessages` + `msgIdCounter`（保证 id 不冲突）；图侧用 `InMemorySingleCheckpointStore` 只保留每个 thread 最新 checkpoint（把 checkpoint 存储从 O(super-steps) 降到 O(threads)），中断恢复由 `AgentRunner::resume_async` + `graphData` 中的 `xx_interruptNode/xx_interruptValue/xx_interruptToolcallCache` 驱动。
- **写失败策略**：持久化回调“尽力而为”，内部捕获异常并记日志，不中断主流程。

**源码要点（第 2 版补充，`agent/lib/src/agent/session_store.cpp`）**

- **DDL 是四张最小表**：`view_message(seq, json, msg_id)`、`llm_context(id=1, json)`（单行整体替换）、`meta(key, value)`（KV + `ON CONFLICT DO UPDATE` upsert）、`store(id, value)`（share store，自增/覆盖两种写入语句）。
- **一次性就地升级**：`ensureSchema()` 幂等建表后，还会 `PRAGMA table_info(view_message)` 检查 `msg_id` 列是否存在，缺列时用 `json_extract(json,'$.id')` 回填并建索引（老库平滑升级到可 O(1) 定位）。
- **每条 SQL 都准备了语句对象**（`prepare` 后复用），写入成对提交（如“消息 + msgIdCounter 同事务”）；
- **会话活动时间取自消息内容**：列表排序用 `COALESCE(json_extract(json,'$.startTimeMs'), json_extract(json,'$.start_time_ms'))`，兼容两代字段名；两阶段扫描（先 stat 近似排序、再逐库读 meta）配合“已收满一页且后续 mtime 早于页边界即早停”。
- **连接是 LRU 的**：`kMaxOpenSessionDbs = 32`，超出按最近使用淘汰并关闭（关闭后下次写入自动重开）；读路径在目录不存在时直接返回空，**不为只读访问创建目录/空库**。
- **sessionId 会被清洗成目录名**（非法字符替换、超长截断、保留名规避、改写后附加哈希尾缀保证唯一）——即“会话 id 与磁盘路径不是同一份数据”，排障时要注意映射。

### 4.3 对比

| 维度 | dsh | agentxx |
|---|---|---|
| 存储形态 | 每会话 JSONL（可 zstd），按代命名 | 每会话一个 SQLite 库（4 表） |
| 写入模型 | 句柄 + write-behind 窗口 + 显式 flush 屏障 | 每消息/每轮同步写（事务提交），无显式屏障 |
| 并发写保护 | 单写者租约（跨进程） | 进程内互斥锁（`SessionStore::mutex_`） |
| 崩溃恢复 | 修复交给读方（resume 追加闭合事件），撕裂尾部丢弃 | 无“未闭合轮次”概念；恢复靠 checkpoint + 中断现场 |
| 格式演进 | 显式版本号 + 相邻迁移链 + 代不覆盖 | 无显式版本号；表结构变更靠 `ensureSchema` 幂等建表 |
| 查询能力 | 会话查询单独成体系（session-query + SQLite FTS） | SQLite 直查（共享 store、会话列表、分页） |
| 数据分离 | 头部（元数据）与事件日志分离 | 展示历史 / LLM 上下文 / meta / KV 分表 |

**结论**：dsh 的持久化层是“为可恢复性设计”，agentxx 的是“为写入简单与查询方便设计”。dsh 的两个点对 agentxx 有直接价值：**（a）显式的耐久性屏障与单写者所有权；（b）崩溃后由读方配平未闭合的轮次**。agentxx 的两个点反过来值得保留：SQLite 事务 + 分页查询 + 连接 LRU 的工程化。

### 4.4 可迁移到 agentxx 的设计

1. **会话数据格式版本号**：在 `meta` 表加 `schemaVersion` / `formatVersion`，启动时对低于当前版本的库执行**单步迁移链**（`v1→v2→v3`），并保证“已发布版本不回写、不原地破坏”。当前 `ensureSchema` 只保证建表幂等，一旦字段语义变化（如消息结构、`messagesVersion` 语义）没有升级路径。
   落地：`agent/lib/src/agent/session_store.cpp` 的 `ensureSchema`。
2. **写句柄/所有权**：进程内已有互斥锁，但**跨进程无保护**（两个 agentxx 进程指向同一 `dataDir` 时会互相覆盖；SQLite 的行级锁只保护单条语句，agentxx 又是“每次调用一个短事务”，所以两进程的写入可以安全交错，但“读-改-写”的语义会被破坏）。建议照 dsh 的做法给会话目录加一层**内核级写租约**：POSIX 用 `flock`、Windows 用命名内核对象，持有期 = 写句柄生命周期，**不设超时**（崩溃时由内核释放，避免抢占导致日志撕裂）。这比“meta.owner + pid + 时间戳”更可靠（后者无法防“卡住的写者”且需要人工判断陈旧标记）。
   落地：`SessionStore` 打开写路径时获取锁（可参考 `agent/third_party/cxx_utilxx` 或直接 `flock`/`CreateSemaphore`），并把 `SessionAlreadyOwnedError` 语义映射到“同一会话不允许两个写者”。
3. **轮次闭合记录与崩溃配平**：在 `meta`（或新表 `turn`）记录 `turn/start` 与 `turn/end{reason}`；启动/恢复时若发现未闭合轮次，则补齐“被中断”的收尾（含未执行工具调用的占位结果），这也是 §2.4 第 4、6 项的数据基础。
   落地：`SessionStore` + `BaseAgent::runTurnAsync`。
4. **写入屏障 API**：为需要“崩溃后必须存在”的时刻（如压缩结果、用户消息、权限决策）提供 `flush(sessionId)` 语义（当前是每条写立即提交，但有节流写入路径如 EventBridge 的节流落盘）。至少在压缩完成、轮次开始/结束三个点显式屏障，避免“压缩后立刻崩溃导致重复压缩”。
   落地：`session_store.h` + `summarization.cpp`。
5. **元数据与上下文分离**：dsh 把 `SessionHeader` 与 `inheritedEventCount` 放在日志之外。agentxx 建议把“会话工作目录、worktree 绑定、模型选择、语言、权限模式、完全授权”等会话级设置统一放进 `meta`（部分已在内存/Wire 上传递），使恢复后行为一致（现在部分设置只存在内存里）。
   落地：`session_store.cpp` 的 meta 表结构。

---

### 4.5 各自做得好的设计

**agentxx 相比 dsh 做得好的：**

- **写入路径简单且可查询**：每会话一个 SQLite 库、四张最小表、事务提交与预编译语句；会话列表用 keyset 游标 + 两阶段扫描（先 stat 近似排序，再逐库读 meta）+ 早停，海量会话下依然可用 —— dsh 的日志格式为了“可恢复性”放弃的查询便利，agentxx 直接拿到了。
- **连接与内存工程化**：数据库连接 LRU（32 个上限，超出即关，关闭后自动重开）、只读访问不创建目录/空库、`msg_id` 列缺失时就地回填并建索引（老库平滑升级）。
- **元数据与业务同库**：`meta`（标题/最近活动/msgIdCounter）与 `store`（share_store）和消息同库同事务，备份/删除/迁移都是一次目录操作。
- **展示历史的写入有防错**：批量提交 + 过期下标丢弃，避免恢复/整体替换后旧批次写坏历史。

**dsh 相比 agentxx 做得好的：**

- **跨进程写所有权由内核保证**：POSIX `flock` / Windows 命名信号量，持有期等于写句柄生命周期，**故意不设过期**（崩溃由内核释放，杜绝抢占导致的日志撕裂），并做 inode 校验；agentxx 目前只有进程内互斥锁。
- **耐久性语义分层**：`append` 只承诺“被接受、可见”，`flush` 才承诺“崩溃后仍在”；写入经有界 write-behind 窗口，生产方不被阻塞 —— 该快的地方快、该稳的地方稳。
- **崩溃恢复职责划分明确**：持久化只保证“物理上有效的连续日志”（撕裂尾部丢弃），语义修复（未闭合轮次、缺失工具结果）由 agent 层在读方完成并**在写所有权下追加**，不会与活跃轮次竞态。
- **格式演进有路径**：显式格式版本 + 相邻迁移链（每个包只负责一个 `vN→vN+1`）+ 已提交代际绝不改名/删除/覆盖；agentxx 目前是幂等建表。
- **头部与日志分离**：`SessionHeader` 与精确 `inheritedEventCount` 存在日志之外，fork 切点是一等数据；agentxx 的“会话级设置”还有一部分只在内存中。

---

## 5. 工具系统

### 5.1 dsh：注册表 + 三段落流水线

- **`ToolDefinition`**：面向模型的 `ToolSchema`（`name`/`description`/`parameters`）+ **必需的规范输出声明 `output`**（`schema` + 纯函数 `render(args, value) → ContentBlock[]` + 可选 `presentationMeta`）+ `execute(args, exec)` + 可选 `projectContent` / `finalizeContent` / `timeoutMs` / `isConcurrencySafe` / `presentCall` / `presentResult`。
    - `execute` 只返回**规范值**（lossless JSON，由 `output.schema` 校验）；模型可见内容是 `render` 的投影。**规范值不持久化**，只有渲染后的 `content` 入库 —— 回放能重现展示，但重建不出中间值。
    - `finalizeContent` 是“最后一道仅内容的不变量”（工具自己限制长度/去掉敏感片段），**对流水线失败也生效**；`projectContent` 在 `post-execute` 策略之前安装“执行准备内容”。
    - `timeoutMs` 由 `dsh-tool-call-timeout-policy`（一个 `tools/execute` 包装器）强制执行，**绝不发给模型**。
- **schema DSL**：`defineTool({name, description, parameters, output, execute})` 统一工具参数与**输出值**的类型推导（`InferValue`/`InferArgs`，16 层容器内精确推导）；参数不匹配抛 `ToolArgsError(INVALID_ARGS)`，函数体返回非法值抛 `ToolOutputError(INVALID_TOOL_OUTPUT)`。
- **流水线**（`docs/tool-execution-pipeline.zh.md`）：
  `tools/pre-execute`（waterfall：allow/deny/ask，可改写决策，**不可改参数**，因为参数已记录并已展示给用户）→ **单调 guard**（`ToolGuard` 返回 reason 即拒绝，没有 allow 结果，所以后续监听器无法把拒绝翻回允许）→ `tools/execute`（环绕分派：超时、重试、计量）→ `projectContent` → `tools/post-execute`（accept/replace/**block**，block 会把纠正反馈变成 `isError` 结果）→ `finalizeContent` → `tools/result`（冻结的权威结果通知）。
- **失败归一化**：未知工具 → `UNKNOWN_TOOL`；工具抛异常 → 结构化错误；流水线自身抛异常 → 也被归一化为 `isError`（“调用失败但不终止当前轮次”）。
- **上下文回注**：工具可通过 `deferContext()` 把 `UserMessage` 挂到**自己的结果上**，循环在该 `tool/result` 之后按 FIFO 追加 `user/message`（“活跃批次附加上下文”）——让工具能“把发现告诉模型”而不污染自己的输出文本。
- **`concludeTurn()`**：成功结果可标记“本轮到此结束”（如 `todo` 全部完成后的收尾）。
- **并行调度**：`isConcurrencySafe(args) → true` 才可加入并行组（唯一 opt-in；省略/异常/非 `true` 都是独占），独占形成屏障，结果按模型顺序提交（`src/tool-calls.ts`，测试 `tool-order.spec.ts`）。
- **展示与执行分离**：`presentCall(args)` / `presentResult(args, result)` 是**纯函数**，UI 实时流式与日志回放共用同一份实现（`docs/subsystems/conversation.zh.md`）。

**源码要点（第 2 版补充，`packages/core/tools/src/index.ts`）**

- **流水线在实现上是四段**（对调度器开放，普通调用方只用 `execute`）：`prepare`（物化参数 → 有序 `tools/pre-execute` → 审批 `ask` → 单调 guard）→ `dispatch`（环绕包装 + 工具体）→ `finalize`（`tools/post-execute` → `finalizeContent` → 物化 → 通知）→ `finish`（只做 `finalizeContent` + 物化 + 通知，跳过 post-execute）。拆段的目的是让**前置/后置策略保持模型顺序，而执行体可以重叠**（见 §5 并行调度）。
- **参数只物化一次并且不可改写**：`createExecution` 做一次无损 JSON 快照 + 深冻结；参数一旦记录/展示就不可变（前置策略只能 allow/deny/ask/cancel）。
- **回调在物化之前就被捕获**：`projectContent`/`finalizeContent` 的引用在 `snapshotJsonValue` 之前绑定 —— 因为参数对象上的 getter 可以在物化期间“换掉”已注册的回调，注册表必须用调用开始那一刻的快照，避免执行中途换实现。
- **PTC 折叠的调用在策略之前就被拒**：处于 `ptc` 模式下被折叠（不可直接调用）的工具，会在 `pre-execute`/审批/guard **之前**直接失败（`UNKNOWN_TOOL` + 提示“请在 `run_code` 里调用它”）—— 否则策略可能“批准一个注定失败的调用”；而真正未知的工具仍走正常分发阶段，让策略能观察到每一个到达注册表的名字。
- **信号只会被融合、不会被替换**：`tools/execute` 包装层可以替换自己委托期的 signal，但注册表会用 `fuseToolSignals(caller, wrapper)` 把**调用方信号**重新融合进来，包装层无法“取消掉取消”。
- **取消按“是否已进入工具体”区分**：执行对象上记录 `bodyInvoked`，据此给出 `ABORTED` 或 `ABORTED_BEFORE_DISPATCH` 两个规范错误码；中止结果会保留此前已积累的 `additionalContexts`（上下文不丢）。
- **规范值与投影值分开跟踪**：被标记为 canonical 的结果放在 `WeakMap` 里（`markCanonical`），观察者与持久化只拿渲染后的 `content`/`error`/`meta`，**规范值不落库**（回放能重现展示，重建不出中间值）。
- **错误归一化覆盖“流水线本身出错”**：未知工具 → `ToolNotFoundError`（`UNKNOWN_TOOL`）、参数不合法 → `ToolArgsError`（`INVALID_ARGS`）、输出不合法 → `ToolOutputError`（`INVALID_TOOL_OUTPUT`）；物化失败会被转成 JSON 安全的 `isError` 结果，**仍然会经过 `finalizeContent`**。
- **展示模式会改写可见工具集**：`presentAs(mode)` / `ToolPresentationMode` 配合 `collapseSection()`（折叠说明段）与 `sdkSection()`（SDK 用法段）把工具**折叠成少数入口**（PTC 模式只暴露 `run_code`），并限制子调用并行度（`maxParallelSubCalls`，默认 10）。
- **作用域分层是真数据结构**：`ScopedLayers` + `ToolLayer`（`admits(name)` / `guardReason(exec)` / 具名与匿名条目），`restrict()` 与 `guard()` 都返回 disposer；`view(scope)` 给出某个作用域下的完整可见视图。

### 5.2 agentxx：`XXToolBase` + 图分发 + 中间件协同

- `agentxx::tools::XXToolBase`（`agent/lib/include/agentxx/tools/tool.h`）：`get_name()` / `get_definition()`（`neograph::ChatTool`）/ `execute_async(args) → std::string`；工具返回**字符串**（模型可见内容即返回值）。
- **分发在 `ToolcallWrapNode`**（`nodes/toolcall.cpp`，55 KB）：遍历模型返回的 tool_calls，逐个 `execTool`；错误按约定区分（参数检查失败抛 `std::invalid_argument`、运行期错误抛 `std::runtime_error`），由节点统一格式化为 `[Exception aborted: <msg>]`。
- **参数兼容修正**：`autoFixArgsType()`（string→数组、string↔number、bool↔string、单元素数组解包），减少模型传参类型错误导致的重试。
- **重复调用检测**：`findConsecutiveRepeatCallKeys()` 在“assistant(tool_calls) ↔ tool 结果交替链”内统计连续相同调用（key = `toolName_len_hash`，有界回溯 + 提前终止），达到阈值且工具启用 `repeatCallCheck` 时经权限总线**询问用户**是否继续。
- **工具结果压缩**：工具可注册 `SummarizationToolHandle`（`generateDeduplicationKey` + `truncateRequest`/`truncateResponse`），压缩时对同类调用去重/截断；长输出经 `AGENTXX_PLUGIN_TOOL_FLAG_AUTO_SUMMARY` 外置到 share store（内容换 id）。
- **插件工具**：`PluginManager` 经 `agentxx.agent.tools` 接口表注册（宿主校验与内置/MCP 工具同名冲突）；插件可为自己注册的工具**声明权限**（`agentxx.agent.permission`），提供“模式/前缀参数工具”的三态批量查询 `check_paths`。
- **渲染在客户端**：`builtin_tool_renderers.h` + TUI 装饰（`setToolDecor`）按工具名渲染头部/结果。
- **MCP 工具**：`McpClient::createTool()` 把 MCP 工具包装成 `XXToolBase` 注册进同一表（命名空间前缀）。

**源码要点（第 2 版补充，`agent/lib/src/nodes/toolcall.cpp`）**

- **单个工具的执行顺序**（`execTool`）：`service.permission.check` 权限检查（经 EventBus 请求；**没有权限服务时默认放行**）→ 命中连续重复时经权限总线询问用户 → 真正执行；权限被拒直接返回 `[Permission denied]` 文本（**不是**异常）。
- **参数注入**：执行前把 `sessionId` 与 `tool_call_id` 注入工具参数对象（工具无需自己从上下文猜），跳过非对象参数。
- **结果复用**：先按 `tool_call_id` 查 `toolcallsCache`，命中则直接复用并**补算耗时（0ms）** —— 这是中断恢复路径“不重复执行副作用工具”的关键。
- **顺序执行**：所有 tool_call 先构造成协程，再逐个 `co_await`；代码中保留 `// TODO: 真正并行` —— 也就是说 agentxx 目前**没有**工具级并行（并行只能由图或工具自身实现）。
- **取消的落地**：捕获 `CancelledException` 后停止执行后续工具，把已完成结果 + 未完成调用的 `[User canceled]` 占位（`AutoInserted`）一起写入会话，再重抛取消；中断（`NodeInterrupt`）不走这条路（它由 `AgentRunner` 恢复）。
- **异常与编码**：`[Exception aborted: <err>]` 由 `onHandleStartError`/`onHandleBaseRunError` 统一格式化（带阶段前缀），错误以“文本结果”形式回到上下文而不是终止循环。
- **热插拔安全**：动态工具经 `toolRegistry->find()` 拿到 `shared_ptr`，与插件的 inflight 计数配合，保证插件卸载（`dlclose`）不会发生在仍在执行的工具中间。
- **结果去重/截断不在节点里**：`SummarizationToolHandle`（`generateDeduplicationKey` / `truncateRequest` / `truncateResponse`）是**压缩中间件**调用的回调，节点只负责执行与落库 —— 也就是“按工具定制压缩”是压缩侧的策略，不是分发侧的行为。

### 5.3 对比

| 维度 | dsh | agentxx |
|---|---|---|
| 返回契约 | 规范值 + 渲染函数（值/展示分离） | 直接返回模型可见字符串 |
| 参数校验 | `defineTool` 类型推导 + 运行时 schema 校验，错误分类明确 | 工具自行检查，抛异常类型约定；另有 `autoFixArgsType` 容错 |
| 前置拦截 | waterfall（可 deny/ask；**参数不可改**） | 权限中间件（permission 总线可 INTERRUPT/ALLOW/DENY）；中间件 `onToolcallStart` 可改 `NodeInput` |
| 后置处理 | `post-execute`（accept/replace/block + 追加上下文） | 中间件 `onToolcallEnd` 可改结果；`SummarizationToolHandle` 做去重/截断 |
| 单调守卫 | 有（拒绝不可被后续插件撤销） | 无对应物（权限判定在中间件内一次性完成） |
| 超时 | 由包装器按 `timeoutMs` 统一强制 | 工具级 `default_timeout_ms`（插件表）+ MCP 配置 `toolTimeout` |
| 并行 | 分类器 + 有界池 + 顺序提交（内建）；**前置/后置策略顺序、执行体可重叠**（四段流水线） | 明确顺序执行（`// TODO: 真正并行`） |
| 上下文回注 | `deferContext()`（结果后追加 user/message） | 无直接对应物（工具只能写进自己的返回文本） |
| 大输出 | spill seam（存储 seam + 本地实现 + spill 策略） | share_store 外置 + `AUTO_SUMMARY` 标志 |
| 重复调用 | 无内建（`guard/` 组有建议性提醒插件） | 内建连续重复检测 + 经权限总线询问 |
| 中断/恢复时的结果 | 走同一调度器，取消即补规范错误结果 | 按 `tool_call_id` 的 `toolcallsCache` 复用已完成结果（恢复不重复副作用） |
| 错误码 | 规范化错误码伴持久记录（`UNKNOWN_TOOL`/`INVALID_ARGS`/`INVALID_TOOL_OUTPUT`/`ABORTED*`） | 约定异常类型 → 统一文本（`[Exception aborted: ...]`/`[Permission denied]`/`[User canceled]`），不落结构化错误码 |
| 工具集裁剪 | 展示模式折叠（PTC 只暴露 `run_code`）+ 作用域 restrict | 由插件加载/禁用与中间件注入决定；无“模式折叠”概念 |
| 参数注入 | 无（工具自行从 `exec` 拿身份） | 自动注入 `sessionId` / `tool_call_id` |

### 5.4 可迁移到 agentxx 的设计

1. **工具返回结构化值 + 渲染分离**（优先级最高）。让 `XXToolBase` 可以返回 `{value, content}`：`content` 进模型与 UI，`value` 留给程序化消费（渲染器、UI 卡片、压缩器、插件互调 `call_tool_async` 的调用方）。收益：TUI 的工具卡片不必再解析文本；`share_store`/去重等下游处理有结构化输入；插件互调不再需要“解析字符串结果”。
   落地：`tools/tool.h`（新增 `executeJson` 或返回值结构体）、`nodes/toolcall.cpp`（分发与持久化路径）、`client` 侧渲染器。
2. **结果后追加“上下文”**：给工具一个 `deferContext(text, source)` 能力，工具执行完成后由 `ToolcallWrapNode` 把这些上下文作为**独立的 user 消息**追加（而不是塞进工具返回文本）。这能显著改善“工具发现的信息”在压缩时的命运（可被单独剪枝/摘要，而不是混在结果里被一起截断）。
   落地：`nodes/toolcall.cpp` + `nodes/session_context.h`。
3. **单调守卫（不可被撤销的拒绝）**：当前权限判定在中间件内完成，插件钩子（`onToolcallStart`）之后仍可能有人改写 `NodeInput`。建议定义“guard”概念：在插件钩子链**之后**执行、只能拒绝不能放行，用于安全类约束（工作区写边界、危险命令黑名单）。这样“后加载的插件”无法绕过前面的安全策略。
   落地：`ToolcallWrapNode::baseRun` 的钩子顺序。
4. **超时统一为包装层**：把 `default_timeout_ms` 的强制执行从“工具自己遵守”变成“分发层保证”（竞速取消 + 结果归一化），工具只声明预算。
   落地：`nodes/toolcall.cpp::execTool`。
5. **工具结果 `meta`（展示元数据）**：dsh 的 `tool/result.meta` 是“对核心不透明、由工具自己在 `presentResult` 里读回”的 JSON。agentxx 的工具卡片目前靠客户端注册表按工具名渲染；引入 `meta` 可让**工具自己决定卡片内容**（如 diff 的上下文行、检索命中列表），无需客户端硬编码。
   落地：`ViewMessage` 的工具结果字段 + `builtin_tool_renderers`。
6. **保留 agentxx 的优势项**：`autoFixArgsType`（容错）与“连续重复调用检测 + 询问”在 dsh 里没有等价实现，属于 agentxx 经验；建议把前者文档化为“显式容错策略”，后者抽成可配置阈值并补齐测试。

---

### 5.5 各自做得好的设计

**agentxx 相比 dsh 做得好的：**

- **参数容错 `autoFixArgsType`**：字符串↔数组/数值/布尔、单元素数组解包等常见错型自动修正，直接减少“模型参数写错 → 工具报错 → 再试一轮”的往返；dsh 是严格 schema 校验（错了就返回 `INVALID_ARGS`）。
- **循环调用防护**：`findConsecutiveRepeatCallKeys` 在“assistant(tool_calls) ↔ tool 结果”交替链内有界回溯，命中阈值且工具启用 `repeatCallCheck` 时**向用户询问**是否继续 —— 这是“模型陷入重复调用”最实用的止血点，dsh 只有建议性 guard 插件。
- **中断恢复不重复副作用**：按 `tool_call_id` 的 `toolcallsCache` 直接复用中断前已完成的结果（耗时补 0），对“写文件/发请求”这类工具尤其重要。
- **热插拔安全**：动态工具表以 `shared_ptr` 持有工具，执行中调用靠引用计数保活，插件卸载要等 inflight 归零才 `dlclose`；注册时还与静态工具名集合做冲突检测。
- **参数自动注入**：执行前把 `sessionId`/`tool_call_id` 注入参数对象，工具不必自己从上下文猜身份（对第三方插件尤其友好）。
- **按工具定制压缩**：`SummarizationToolHandle`（去重 key + 截断 request/response）把“这类工具的输出怎么压缩”交给工具自己声明，配合 `AUTO_SUMMARY` 自动外置长输出。

**dsh 相比 agentxx 做得好的：**

- **输出契约清晰**：工具返回**规范值**（由 `output.schema` 校验），模型可见内容是纯函数 `render(args, value)` 的投影，展示元数据由工具自己声明；规范值不落库，UI 实时与日志回放共用同一份渲染实现。
- **四段流水线让“策略有序 + 执行体重叠”成为可能**：`prepare`（有序前置策略 + 审批 + 单调 guard）→ `dispatch`（可重叠的环绕包装 + 工具体）→ `finalize`（后置策略 + 最终内容）→ `finish`；这是 agentxx 顺序执行结构无法直接支持的。
- **单调 guard**：只能拒绝、不能放行，因此后续插件无法把前面的拒绝“翻回允许”；参数在记录/展示之后不可改写，避免审计与执行不一致。
- **取消分级与占位**：`ABORTED` / `ABORTED_BEFORE_DISPATCH` 两个规范错误码，未启动的调用会补 `tool/call` + 错误结果（并引用 call 事件 seq），保证日志可重放；同时保留已积累的 `additionalContexts`。
- **结果后回注上下文**：`deferContext()` 把上下文挂到该次结果上、在 `tool/result` 之后按序注入；`concludeTurn()` 让“收尾型工具”结束本轮 —— 两者都避免了把控制信息混进工具输出文本。
- **工具集可按模式折叠**：展示模式 + `collapseSection`/`sdkSection` 让 PTC 模式只暴露 `run_code`，作用域 `restrict`/guard 提供可见性与策略两层过滤。

---

## 6. 系统提示词与请求组装

### 6.1 dsh：段落注册表 + 动态上下文 + 提示词入历史

- **`PromptSection`**：`{name, order, text | (ctx)=>string, interpolate?, complete?}`；先按 `order` 升序、再按名称代码单元序排列；`complete: true` 表示“这一条就是完整提示词”（组装仍跑 waterfall 以解析工具/变量，然后恢复该段为唯一段落；**多于一条 effective complete 会让组装失败**）。
- **`PromptContext`**：动态模型上下文（时间、审批策略快照、目录清单…），作为**持久的 user 角色快照**追加在历史之后；只在完整快照变化或被压缩移除时重发。
- **变量**：`variable(name, provider)`，名字必须匹配 `[a-z][a-z0-9_]*`；段落文本里的 `{{var}}` 在 `renderPrompt` 时插值。
- **工具 schema 由提供方贡献**：`tools(provider)` 返回 `ToolProviderResult{schemas, knownNames}`，`knownNames` 用于区分“配置名拼错”和“已知工具在此作用域被有意隐藏”。
- **组装 waterfall**：`system-prompt/assemble` 可整体包装/替换组装结果（专家口子）。
- **关键设计：提示词作为 surface 第 0 号 `system/message` 节点进入历史**，而不是请求字段。于是“提示词变更”= 替换/追加一个系统节点；“空渲染文本”会清除所有生效的系统节点（模型不再看到旧指令）。这带来两个好处：请求头（`EpochHeader`）不含 `system`，**同一消息序列内提示词不变则前缀缓存可复用**；提示词变化对模型与回放是同一件事（都是历史）。
- **路由能力协商影响提示词更新方式**：`request/context.systemPromptUpdate = 'in-history'` 的路由可以在缓存前缀之后**追加**非空提示词更新（KV Cache 友好）；不具备该能力的路由会把非空提示词**归并到首个系统节点**（从该节点起缓存失效），并为其后的非空系统节点记录“空内容替换”。

**源码要点（第 2 版补充，`packages/core/system-prompt/src/index.ts` + `agent-loop/src/index.ts`）**

- **循环自带三个提示词变量**：`provider` / `model` / `cwd`（分别来自 agent options 与 `session.header.cwd`），插件可直接在段落文本里用 `{{provider}}` 之类引用，不必自己注册。
- **组装产物是一个结构化对象**：`PromptAssembly` 同时携带 sections、contexts、tools 与 variables；循环分别用 `renderPrompt()` 取系统文本、`renderContextSections()`/`joinContextSections()` 取动态上下文（这些内容随后作为**持久的 user 角色快照**进入历史）。
- **顺序由中央分配而非贡献者自定义**：段落用 `getSectionOrder(name)`、上下文用 `getContextOrder(name)` 取“仓库统一分配的排序位”，避免各插件各自约定数字造成插队。
- **能力抑制口子**：`suppressRuntimeContext()` 可在调用方作用域内**关闭全部动态上下文贡献**而不卸载提供者（用于 headless/受限场景）。
- **作用域遮蔽**：带作用域的段落/变量**遮蔽同名全局项**；同一层内重名或非法名字直接抛错（配置错误在最早可解析点失败）。

### 6.2 agentxx：字符串拼装 + 追加段

- **`AgentPrompt`**（`agent/lib/include/agentxx/agent/prompt.h`）：
    - `systemPrompt`：主体文本（文本集中在 `prompt.cpp`，改文本不牵连所有编译单元）；
    - `appendSystemPrompts`：`map<key, text>`，键为插件/功能标识（`planning`/`skill`/`codegraph`…），**按字典序拼接**；插件经 `agentxx.agent.prompt` 接口表写入，卸载时按备份恢复；
    - `toolPrompt`：按工具名覆写 `depict`/`args` 描述；
    - 会话级占位符 `${work_dir}` / `${temp_dir}` / `${session_id}`，由 `renderVars` 纯文本替换（取不到值替换为 `unknown`，不留 token）；
    - 训练支持：`toJson`/`fromJson`/`mergeFromJson`/`promptHash`（种群去重）。
- **动态追加**：`graphDataKey_appendSystemMessage`（`xx_appendSystemMessage`）承载 skill/memory 等动态系统消息；`buildSystemPrompt()` 拼装 `systemPrompt + appendSystemPrompts(字典序) + appendSystemMessage`。
- **请求组装**：`ModelCall` 节点取会话上下文（会话为唯一权威）+ 本轮系统提示 + 工具 schema（工具描述来自 `ChatTool`，可被 `toolPrompt` 覆写）。

**源码要点（第 2 版补充，`agent/lib/src/nodes/modelcall.cpp`、`include/.../agent/prompt.h`）**

- **系统消息是“就地替换”而不是追加**：模型调用前，若上下文首条已是 `system` 就**原地更新其 `content`**（保留其余字段），否则在开头插入一条；随后 `replaceMessages(..., false)`（不请求节流落盘，因为系统提示是每轮重建的派生内容，交给轮末权威保存）。
- **占位符是纯文本替换且有兜底**：`AgentPrompt::renderVars` 只替换 `${work_dir}` / `${temp_dir}` / `${session_id}` 三个固定 token（不按 `fmt` 模板解析，避免自定义提示词里的 `{`/`}` 抛异常）；取不到值时替换为 `unknown`，**不留 token**（让模型看到信息缺失，也不会把空路径写进提示词）。
- **附加段与工具描述都可被训练覆盖**：`appendSystemPrompts`（按键字典序拼接）、`toolPrompt`（按工具名覆写 `depict`/`args`）都有 `toJson`/`fromJson`/`mergeFromJson`/`promptHash`（提示词种群去重）——这套序列化是为“训练/自更新提示词”服务的，dsh 侧没有对应能力。
- **动态附加消息走图数据**：`xx_appendSystemMessage`（skill/memory 等）在 `buildSystemPrompt()` 里拼接，属于“每轮重新计算”的内容，因此组装的最终文本会随会话内资源变化而变。

### 6.3 对比

| 维度 | dsh | agentxx |
|---|---|---|
| 段落注册 | 注册表 + `order` 排序 + 名称唯一性校验 | `appendSystemPrompts` map，**键字典序**决定位置 |
| 动态内容 | `PromptContext`（持久 user 快照，随变化重发） | `xx_appendSystemMessage`（每轮拼接，不区分“快照/通知”） |
| 变量插值 | `{{name}}` + `variable()` 注册（非法名报错） | `${work_dir}` 等固定 token（`renderVars`） |
| 整体覆盖 | `complete: true`（多份冲突即失败） | 无（只能改键值段） |
| 提示词在请求里的位置 | 历史第 0 号 system 节点（可替换/追加） | 每轮拼装后作为首条 system 消息 |
| 工具描述 | schema 由提供方贡献，白名单裁剪 | `ChatTool` + `toolPrompt` 覆写 |
| KV Cache 意识 | 明确（`systemPromptUpdate` 能力、前缀复用说明） | 未显式建模 |

### 6.4 可迁移到 agentxx 的设计

1. **段落排序号**：`appendSystemPrompts` 现在按键字典序（键名一改顺序就变）。建议改为 `{key, order, text}` 结构，`order` 由宿主中央分配（如 `100=persona`, `200=planning`, `300=skill`, `900=动态记忆`），保证插件之间位置稳定、可文档化。
   落地：`prompt.h` 的 `appendSystemPrompts` + `agentxx.agent.prompt` 接口表。
2. **动态上下文区分“快照 / 通知”**：dsh 的 `ContextForm{instructions, catalog, snapshot, notice, relay, recall}` 是一个很实用的词汇——它决定了“同一来源的新内容是否覆盖旧内容”“UI 是否默认折叠”。agentxx 的 `appendSystemMessage` 现在是纯文本拼接，建议加来源与形态字段（与 §3.4 第 1 项的 `source` 合并设计）。
   落地：`prompt.h` + `middlewares/skill.cpp`/`memory_file.cpp` 的写入点。
3. **提示词整体覆盖**：现有 `systemPrompt` 只能整体替换文本，但没有“插件声明自己提供完整提示词”的机制。建议加 `complete` 语义（多份冲突时报错而不是静默取一个），用于 headless/嵌入式场景。
4. **KV Cache 友好的提示词更新**：agentxx 每轮都把系统提示放在消息最前面，任何提示词变化都会让整个前缀失效。可借鉴的两条：(a) 把动态段（时间、目录清单、审批状态）从系统提示移出，作为**历史中的快照消息**并在变化时才追加；(b) 提示词变化时优先“追加到已缓存历史之后”而不是改写首条 system 消息。这两点直接决定长会话的 token 成本。
   落地：`modelcall.cpp` 的请求组装 + `summarization.cpp` 的保留策略。

---

### 6.5 各自做得好的设计

**agentxx 相比 dsh 做得好的：**

- **提示词是数据、可训练可自更新**：`AgentPrompt` 的 `toJson`/`fromJson`/`mergeFromJson`/`promptHash` 让提示词能整体序列化、按补丁合并、做种群去重 —— 这套能力是为“让 agent 自己改提示词”服务的，dsh 的提示词是插件代码里的常量与函数。
- **附加段按插件键位管理且可恢复**：`appendSystemPrompts` 是 `键 → 文本`，插件写入时记账、卸载时按备份恢复，天然支持“装/卸插件即装/卸提示词段落”。
- **工具描述可按工具名覆写**：`toolPrompt` 让同一套工具在不同部署下有不同的描述文本，而不必改工具实现。
- **占位符替换足够保守**：`renderVars` 只替换三个固定 token、不按 `fmt` 模板解析，自定义提示词里出现未配对的 `{}` 不会抛异常；取不到值替换成 `unknown` 而不是留 token。

**dsh 相比 agentxx 做得好的：**

- **段落是注册表而非 map**：`order` 由中央统一分配、同名冲突直接失败、`complete: true` 可由某个插件整体接管提示词（多于一个 effective complete 会报错）—— 位置与优先级是显式契约，而不是“键名字典序”这种隐含规则。
- **动态上下文区分形态**：`instructions`/`catalog`/`snapshot`/`notice`/`relay`/`recall` 六种语义形态，决定“同来源的新内容是否覆盖旧的”以及 UI 如何呈现；agentxx 目前只有一段拼接文本。
- **KV Cache 意识内建**：提示词作为 surface 节点进入历史，变更=替换/追加节点；路由可以声明 `systemPromptUpdate: 'in-history'`，从而把非空更新追加到缓存历史之后而不是改写首条系统消息。
- **变量注册有校验与作用域**：变量名必须匹配 `[a-z][a-z0-9_]*`，作用域可遮蔽全局；循环还自带 `provider`/`model`/`cwd` 三个会话相关变量。
- **可整体抑制**：`suppressRuntimeContext()` 能在调用方作用域里关掉全部动态上下文贡献而不卸载提供者（适合 headless/受限场景）。

---

## 7. LLM 流式与适配器

### 7.1 dsh

- **词汇包** `packages/llm`：`ContentBlockMap`（text / reasoning / image / file / tool-call / tool-addition / tool-removal）、`MessageSourceMap`（含 `kind` 与 `form` 两级语义）、`FinishReasonMap`、`StreamChunk`（**封闭**可辨识联合：文本/推理/工具调用交错，`index` 关联 delta，`block-end` 直接给出组装好的 `ContentBlock`，`switch` 以 `assertNever` 收尾）。
- **适配器契约**：每个 provider 实现 `prepareCall()`（在活跃轮次信号下校验/解析推理强度、输出上限等**适配器自有默认值**）与流式调用；`prepareCall` 的返回值决定**提示词准入能力**（`systemPromptUpdate`）与 `request/context` 记录。
- **请求冻结**：循环把消息深冻结并在同一 agent 内复用冻结证据（`tests/request-freeze.spec.ts` 11 KB），请求由 `header.config + deriveMessages() + header.tools` 构成，**不携带 `system` 字段**。
- **attempt 留痕**：每次模型尝试要么落 `assistant/message`（成功，含 stream 与 usage），要么落 `assistant/attempt`（失败/重试/取消，不伪造模型历史）；`llm/stream` 是 waterfall（可拦截流）。
- **推理内容**：`ReasoningBlock` 与可见文本分离（thinking 不进正文）。

**源码要点（第 2 版补充，`packages/llm/llm/src/{assembler.ts,retry-policy.ts}`）**

- **流式组装只有一份实现**：`BlockAssembler` 是“chunk → 完整 ContentBlock → 最终 assistant 消息”的**唯一规范算法**（循环边喂边记录原始 chunk）；它同时容忍“只有 delta、没有 block-start/end”的简化协议，并**忽略某个 index 已经 `block-end` 之后又到的 delta**（坏适配器既不能让内存增长，也不能污染已完成块）；对外暴露 `blocks()`/`message()`/`usage`/`finish` 与取消时的 `interruptedBlocks()`。
- **重试策略是 provider 的数据，不是节点的 if-else**：适配器在注册路由时解析出一份**不可变的**重试策略，两种模式——`normal`（按失败码白名单重试，默认 `EMPTY_RESPONSE`/`RATE_LIMIT`/`SERVER`/`TIMEOUT`/`TRANSPORT`，默认最多 5 次）与 `always`（直到成功/取消/销毁）；退避是“有界指数 + 对称抖动”（默认 500ms 起、10s 上限、0.1 抖动），配置校验严格到“未知键、重复失败码、越界延时”都直接抛错。
- **执行者是独立插件**：真正“何时重试”由可选的 `dsh-llm-retry` 插件挂在循环的失败扩展点（`agent/request-error`）上执行 —— 于是“策略（provider 数据）/ 执行（插件）/ 触发点（循环）”三者分离，任何一方都能独立替换。

### 7.2 agentxx

- `protocol/openai_provider.cpp`（87 KB）、`anthropic_provider.cpp`、`McpClient`/`mcp_server`、`provider_common.h`；模型注册表 `ModelProviderRegistry` 支持多模型与运行时切换（`selectModel` 按 sessionId 隔离）。
- Neograph 侧提供 `RateLimitedProvider`（限流）与 `SchemaProvider`（结构化输出）适配层。
- 流式增量经 `io->sendToPeer(WireDelta)` 推送；推理内容用独立的 `xx_ModelCallWrap_tempLLMThinking` 通道暂存；`xx_ModelCallWrap_LLMTokenUsage` 记录用量，`ContextStats{contextTokens, maxContextTokens, tps}` 供 UI 显示。
- 重试：`onModelcallRun` 钩子在重试时会被多次触发；无统一的重试决策点。

**源码要点（第 2 版补充，`agent/lib/src/nodes/modelcall.cpp`）**

- **重试策略写死在节点里**：退避公式 `retry*3 秒`，命中限速（`defaultRateLimitTag`）时再加 `retry*5 秒`；上限只有配置项 `llmMaxRetry`（没有“按失败码决定是否重试”的维度）。
- **可观测性做得不错**：每次重试都会经 `CHANNEL_WRITE` 发一条 `message_tip`（warning）给 UI，并且**提示里的秒数与定时器用同一个变量**（避免“提示 5 秒实际等 8 秒”）。
- **部分输出保留策略**：≥512 字符的部分输出会先落成 `AutoInserted` 的 assistant 消息再重试，并**不重置重试计数**，从而既不让用户白等、也不会无限重试。
- **与 dsh 的对照**：dsh 把“策略/执行/触发点”三者分开（provider 数据 + 插件执行 + 循环触发点），agentxx 把三者合并在模型调用节点内 —— 这是可迁移性最强的一处差异（见 §7.3）。

### 7.3 对比与迁移建议

| 维度 | dsh | agentxx |
|---|---|---|
| 流式协议 | 封闭联合 + `block-end` 携带组装结果 | 由 provider 回调 + WireDelta 推送 |
| 能力协商 | `prepareCall` 返回路由能力（上下文窗口、提示词更新模式） | 模型配置里静态声明（`modelContenxtMaxToken` 等） |
| 请求冻结/可重建 | 强（可执行不变量） | 弱（请求即时组装） |
| 失败留痕 | `assistant/attempt` | 仅日志 |
| 推理块 | 一等 `ReasoningBlock` | 独立临时通道 + UI 展开能力 |
| 流式组装 | `BlockAssembler` 唯一实现（容忍 delta-only、忽略已关闭 index 的 delta） | 各 provider 自行组装，UI 侧也参与 delta 拼装 |
| 重试策略 | provider 数据（`normal`/`always` + 失败码白名单 + 有界指数退避 + 抖动）+ 独立插件执行 | 节点内硬编码退避公式，配置只暴露 `llmMaxRetry` |

**建议**：
1. **能力协商下沉到 provider**：把上下文窗口、是否支持 reasoning、是否支持并行工具、提示词更新方式等作为 provider 暴露的能力事实，而不是散在配置与各调用点。收益：压缩阈值、UI 提示、提示词更新策略都能按“实际路由”决策（当前 `defaultModelSupportMaxToken` 是配置兜底）。
   落地：`model_registry.h` / `provider_common.h`。
2. **失败/重试留痕**见 §3.4 第 2 项。
3. **重试决策点统一**见 §2.4 第 5 项。
4. **流式协议收敛**：把 provider 回调统一成“带 `index` 的 chunk + 组装完成事件”，避免 UI 侧各自拼装 delta（当前 TUI 与 stdio 两条路径都要处理 delta 拼装）。
5. **重试策略从节点里抽出来**：把“哪些错误可重试、重试几次、退避曲线”做成**按模型/路由的配置数据**（两种模式：白名单重试 / 一直重试），并让执行器成为可替换的中间件（挂 `onModelcallRun` 或独立的失败处理点）。收益：不同 provider 的限速/超时特性可以分别调参；重试行为可被插件观测与覆盖；节点只负责“请求失败后交给策略”。
   落地：`model_registry.h` 的 `ModelConfig` 增加 `retry` 段 + `modelcall.cpp` 的失败分支改为调用策略接口。

---

### 7.4 各自做得好的设计

**agentxx 相比 dsh 做得好的：**

- **多 provider 在库内就绪**：OpenAI / Anthropic 适配器均由本项目实现，配套限流（`RateLimitedProvider`）与结构化输出（`SchemaProvider`）适配层，部署时不需要额外插件包。
- **用量与速度按会话实时可见**：`ContextStats{contextTokens, maxContextTokens, tps}` 由模型调用节点与流式统计更新，UI 直接显示上下文占比与生成速度；dsh 的 usage 是随消息持久化的数据，实时速率不在核心路径上。
- **服务端自主加载附件**：只有路径时由服务端读文件、按媒体类型做体积上限检查、base64 与读盘都卸载到线程池，避免客户端二次中转与大包上行。
- **模型切换是一等操作**：按 `sessionId` 隔离的模型选择（`selectModel`）与按会话解析的当前模型名/上限，使“同一进程不同会话用不同模型”是默认能力。

**dsh 相比 agentxx 做得好的：**

- **流式组装唯一实现**：`BlockAssembler` 是“chunk → ContentBlock → assistant 消息”的唯一算法，容忍只有 delta 的简化协议、忽略已 `block-end` 之后又到的 delta（坏适配器既不能涨内存也污染不了已完成块），并同时提供 `interruptedBlocks()`。
- **重试策略是数据、执行是插件**：`normal`（失败码白名单 + 有界重试）/`always`（直到成功）两种模式 + 有界指数退避 + 对称抖动，配置校验严格到“未知键/重复失败码/越界延时”都报错；**策略（provider 数据）/执行（可替换插件）/触发点（循环失败扩展点）三者分离**。
- **能力协商按实际路由决策**：`prepareCall()` 返回该路由的上下文窗口与提示词更新模式（`systemPromptUpdate`），压缩阈值、提示词更新策略都能按真实路由走；agentxx 的 `modelContenxtMaxToken` 等仍是静态配置。
- **请求冻结与可重建**：请求对象一次性深冻结（消息用 `WeakSet` 记录已冻结对象，避免持有被替换的历史），并有“从日志重建请求”的不变量测试。
- **失败留痕**：`assistant/attempt` 记录失败/重试/取消尝试，`assistant/message` 携带精确 stream 与 usage，离线复盘不需要依赖运行日志。

---

## 8. 上下文压缩

### 8.1 dsh：压缩是一等 seam

- **三者分工**：Service Definition `ctx.compaction`（`compactIfNeeded(agent, trigger, signal)` / `compactNow` / `compactRegion`）、Provider（`dsh-compaction-basic` 拥有阈值、保留尾部策略、事件排序、摘要调用）、Consumer（`/compact` 命令）。
- **事件化**：`compaction/start`（获取**日志记录的锁**，`turn: number | null` 区分自动/手动）→ `compaction/summary`（摘要块、被遮蔽的 surface 边界与 seq 集合、被遮蔽 token 数、摘要调用的 envelope/usage）→ `compaction/end`（释放锁，可带 `error`）。锁括住整个操作，**中途崩溃表现为可检测的遗留锁**（有 start 无 end），而不是虚假的完成。
- **摘要本身是 surface 替换**：摘要作为一条 `user/message` 携带 `surfaceOp: {op:'replace', startSeq, endSeq}` 遮蔽被压缩范围 —— 不需要任何“历史改写”特权 API。
- **两个触发**：`agent/pre-step`（压力，请求推导之前）与 `agent/request-error`（仅规范上下文溢出）；任一触发后**先做可选工具结果剪枝**（`ctx.toolResultPruner`，报告每次替换的 seq 与字符数变化），再用 `ctx.tokenMeter` 重新测量，然后才摘要。
- **恢复在打开的步骤内**：只有“剪枝或摘要推进了 surface replacement generation”时才返回 `{kind:'retry'}`，否则保持原始请求错误为终态；重试**不重复组装/pre-step/用户消息准入**。
- **区域边界保持工具调用/结果配对**（`toolPairingBalancedBefore/After`），但不保持整个轮次，所以一个过大轮次里较早关闭的步骤也能被压缩。
- **图片省略**单独成为一个 provider（`compaction-image-offload`），记录“节点 + 深度优先图片序号”，保留消息身份与位置，不携带 `surfaceOp`。
- **手动压缩错误分类**：`busy | cancelled | changed | summary | commit | persistence`，其中 `changed`/`summary` 会“闭合失败尝试并持久化到日志”，不写摘要替换。

**源码要点（第 2 版补充，`packages/compaction/compaction-basic/src/{index.ts,region.ts}`）**

- **选段规则**（`selectCompactableRange`）：保留一段“有价格预算的近期尾部”，**绝不切开 assistant tool-call/其结果的配对**；位于 surface 第 0 号的 `system/message` 永不进入压缩范围（没有它时范围从 0 号节点开始）。
- **锁与提交的相邻性**：空闲/日志校验与 `compaction/start` 是**同步相邻**的，因此“持久开锁标记”在摘要真正开始 yield 之前就已提交；此后任何失败都只尝试写一次 `compaction/end`，**写不成功就故意留下“未配对的 start”**，让下次启动能检测到。
- **锁的复查**：`assertNoActiveCompaction()` 会在异步策略决策**之后**再检查一次持久锁；若发现未配对的 start，除非后续有构造期 seed 边界证明它属于上一代生命周期，否则一律拒绝（避免并发压缩）。
- **替换校验只要求“同一段仍是同一段”**：只要求所选区间仍存在、连续、计价相同、工具配对平衡；**区间之外新增的节点不影响摘要的有效性**（不会因为期间插入了注入上下文就整体作废）。
- **摘要请求复用 KV Cache**：摘要前会重建“被遮蔽区间的可缓存前缀”——surface 第 0 号的系统提示 + 请求头里的工具 schema + 该区间按顺序的派生消息，**然后才追加压缩指令**。这样压缩请求与用户上一条真实请求共享尽可能长的前缀，provider 侧缓存命中，压缩成本显著下降。
- **溢出恢复是逐 agent 的计数**：`overflowRetries`（`WeakMap<Agent, number>`）配 `maxOverflowRetries` 上限；agent 回到 `idle` 或一次成功响应后清零；只有 `surface.replaceGeneration` **确实前进**（剪枝或摘要落地）时才返回 `{kind:'retry'}`，否则保留原始请求错误为终态；取消永远优先。
- **压力配置错误的降噪**：`TargetPressureConfigError` 按 `targetKey` **只告警一次**（避免每步都刷同一条日志）。

### 8.2 agentxx：摘要中间件（工程化很强）

- 触发：超过上限 **75%** 自动压缩；最近消息保留 **20%** token 预算；system 与最近消息不压缩。
- 分层处理：
    1. **确定性噪音清理** `cleanNoiseMessages`（删空消息、相邻重复、连续 AutoInserted 提示只留最后一条）；
    2. **工具调用去重/截断**（`SummarizationToolHandle`，按 `generateDeduplicationKey`）+ **探索型调用序列折叠**（连续 ≥3 次同工具单调用段只留最后一组）；
    3. **LLM 同上下文摘要**：经 `NodeInterrupt` 派生一个**压缩子代理**（同 thread id + 同模型以命中 KV cache、无工具、禁止二次压缩），摘要回写。
- 结果布局：`[system] | [user 压缩指令] | [assistant 摘要] | 最近消息`；压缩完成立即写回会话并落盘（避免“压缩后崩溃 → 重启再压缩”）。
- 兜底：同一轮失败 ≥2 次或 token ≥ **95%** 时硬截断，保证请求能发出；`xx_summarizationLastMsgCount` 做冷却（避免反复派生 subagent）；`xx_summarizationTipMsgId` 复用挂起提示消息（resume 后不重复追加）。
- token 估算：纯启发式（asciiCharsPerToken=4.0、unicodeCharsPerToken=1.1、每图 400、每消息 +3）。

**源码要点（第 2 版补充，`agent/lib/src/middlewares/summarization.cpp`）**

- **token 口径是“API usage 优先、启发式兜底”**：先读 `xx_ModelCallWrap_LLMTokenUsage` 里的 API 返回值作为 `tokenUsage`，取不到才用 `countTokens()`（启发式）估算；两者都会写进 `session->contextStats`（UI 显示占比用 API 值）。
- **触发点与阈值**：`onModelcallRun`（每次模型调用前，含重试）里判断 `tokenUsage >= modelContenxtMaxToken * 0.75`；模型上限取自会话当前模型配置（`modelContenxtMaxToken`），缺省 256k。
- **冷却规则是精确的**：`coolDownActive = (failCount == 0 && lastSummarizedMsgCount > 0 && messages.size() <= lastSummarizedMsgCount + 2)` —— 即“上次压缩后消息数几乎没增长而上下文仍超限”，说明 LLM 摘要没能把消息数降下来，此时**跳过 LLM 压缩直接硬截断**，避免每轮都派生一次压缩子代理做无用功。
- **重启/恢复期的提示复用**：压缩提示消息的 id 存在 `xx_summarizationTipMsgId`；resume 后本函数从头重跑时用**更新**而不是再次追加，避免留下多条永远停在 “Summarizing LLM Context...” 的提示。
- **压缩子代理的调用形态**：同 thread id + 同模型（命中 KV cache）、无任何工具、禁止二次压缩（`enable_summarization=false`），并经 `NodeInterrupt` 中断父轮次来派生；压缩段边界由 `splitRecentByTokenBudget`（按 token 预算 + 轮次对齐，避免切开 tool 组）决定。
- **确定性阶段先于 LLM 阶段**：先 `doSummarizeToolcall`（按工具注册的去重/截断回调 + 探索型调用折叠）再 `cleanNoiseMessages`（删空消息、相邻重复、连续 AutoInserted 提示），最后才考虑 LLM 摘要。
- **硬截断兜底**：`hardTruncate()` 在“同一轮失败 ≥2 次”或“token ≥ 95% 上限”时执行，保证请求一定能发出（宁可丢历史也不卡死）。

### 8.3 对比

| 维度 | dsh | agentxx |
|---|---|---|
| 抽象层级 | seam（定义/提供方/消费方分离），可换后端 | 单个中间件（策略、实现、UI 提示耦合在一处） |
| 触发点 | `agent/pre-step`（压力）+ `agent/request-error`（溢出） | 每次 modelcall 前的 `onModelcallStart`（阈值判断） |
| 剪枝 vs 摘要 | 显式两阶段（先剪枝可省一次摘要） | 合并在一次压缩流程内（噪音清理 + 去重 + 摘要） |
| 结果表达 | 一条带 `surfaceOp` 的 `user/message` 遮蔽旧范围 | 用 `replaceMessages` 就地重写上下文 |
| 锁/并发 | 日志记录的锁（崩溃可检测） | 轮内单线程 + 失败计数；无跨进程锁 |
| 崩溃安全 | 锁与替换都在日志里，重启可判定 | 压缩完成即落盘（避免重复压缩），但“压缩中崩溃”无标记 |
| 手动/自动区分 | `turn: number` 或 `null` + 手动错误分类 | `WireCompactContext` 手动触发，同一实现 |
| 失败兜底 | 保留原始请求错误为终态（可不重试） | 失败计数 → 硬截断（保证能发出请求） |
| KV Cache | 明确考虑提示词与历史前缀复用 | 明确考虑：压缩子代理复用同 thread/模型 |
| 工具结果剪枝的"记账" | `PrunedEntry{originalSeq, replacementSeq, callId, charsBefore/After}` | 无（去重不记录） |

### 8.4 可迁移到 agentxx 的设计

1. **压缩拆成“剪枝 → 度量 → 摘要”三步并对每步记账**：agentxx 已有去重与折叠，但缺“这次压缩到底省了多少、动了哪些消息”的记录。建议增加压缩结果结构（旧 token → 新 token、条目数与原因、时间），既用于 UI 展示，也用于冷却决策。
   落地：`middlewares/summarization.cpp` 的完成回调（已有“旧上下文 token 量 → 新 token 量 · 耗时”文本，可结构化）。
2. **压缩状态可恢复/可检测**：给压缩流程加“进行中标记”（写库：`compaction_start`），崩溃后重启时能判定“上次压缩未完成”，从而避免重复压缩或错误统计。
   落地：`session_store` 的 meta 段 + `summarization.cpp` 的挂起/恢复路径（现有 `TipMsgId` 复用逻辑可扩展为持久标记）。
3. **历史替换的原因记录**：压缩走 `replaceMessages`（就地改写）。建议把“替换范围 + 原因 + 摘要 id”一并落库，让 UI/排障能解释上下文为何变短（与 §3.4 第 3 项合并）。
4. **摘要子代理的参数化**：当前压缩子代理的参数（同 thread、无工具、禁二次压缩、模型选择）散在实现里，建议抽成 `SummarizationConfig`（含“是否同上下文摘要”“使用的模型”“最大输出 token”“是否允许工具”），便于对不同模型调优（如小模型做摘要、长上下文模型直接截断）。
   落地：`middlewares/summarization.h` 的构造参数已有一部分，补齐结构体与文档。
5. **剪枝与摘要的分离**：把“确定性噪音清理 + 去重折叠”与“LLM 摘要”分成两个可单独配置/单独关闭的阶段（现在耦合在一个中间件里），可以在“上下文只超一点”时只剪枝不摘要（省一次模型调用）。
   落地：`summarization.cpp` 的策略分支。

---

### 8.5 各自做得好的设计

**agentxx 相比 dsh 做得好的：**

- **兜底链条完整，保证“请求一定发得出去”**：同一轮失败 ≥2 次或 token ≥95% 时硬截断；压缩前有冷却判断（上次压缩后消息数几乎没增长就跳过 LLM 压缩，直接降级截断），避免每轮都派生一次压缩子代理做无用功。
- **确定性阶段先行**：先做噪音清理（删空消息、相邻重复、连续 AutoInserted 提示只留最后一条）与工具调用去重/探索折叠，把不需要模型的活先干掉，剩下的才交给 LLM 摘要。
- **压缩子代理的取巧很实用**：同 thread id + 同模型（命中 provider 缓存）+ 无任何工具 + 禁止二次压缩，用最小代价获得“懂上下文”的摘要器。
- **恢复期的提示复用**：压缩提示消息 id 存在 `graphData`，中断 resume 后本函数从头重跑时用**更新**而不是再次追加，不会留下多条永远停在 “Summarizing LLM Context...” 的提示。
- **计量口径务实**：API usage 优先、启发式兜底；模型上限取自会话当前模型配置，运行时切模型即时生效；压缩完成即写回并落盘，避免“压缩后崩溃 → 重启再压缩”。

**dsh 相比 agentxx 做得好的：**

- **压缩是一等 seam**：Service Definition / Provider / Consumer 三角色分离，自动与手动路径共用同一个日志记录事务，换后端不动消费方。
- **锁是日志里的一等事实**：`compaction/start` 与校验同步相邻，之后任何失败只尝试写一次 `compaction/end`，写不成功就故意留下“未配对的 start”，下次启动能判定“上次压缩没做完”；异步决策之后还会复查锁。
- **摘要请求重建可缓存前缀**：把系统提示 + 工具 schema + 待压缩区间的消息拼成前缀、压缩指令放末尾，从而复用 provider 的 KV Cache —— 压缩本身也要省钱。
- **替换校验的判据更精确**：只要求“所选区间仍存在、连续、计价相同、工具配对平衡”，**区间之外新增的节点不影响摘要有效性**；工具配对边界有专门的 `toolPairingBalancedBefore/After`。
- **溢出恢复策略可控**：逐 agent 的重试计数 + 上限、仅在 `surface.replaceGeneration` 前进时才重试、取消优先；先剪枝（可省一次模型调用）再摘要，两级处理。

---

## 9. 权限、审批与沙箱

### 9.1 dsh：三个独立旋钮 + 失败即拒绝

- **审批 seam（`ctx.approval`）**：`request(req) → ApprovalOutcome = allowed-once | rejected | cancelled | unavailable`；**调用方对后三者一律拒绝**（fail closed）；缺失应答器、应答器抛异常或返回非法值都归一化为 `unavailable`。
    - 请求要求“会话处于未结束的轮次内”（审计事件对必须被持久日志的提交边界包住，空闲时询问会**在追加任何事件之前**被拒绝）。
    - 审计事件 `approval/asked` / `approval/decided` **仅写日志**，不进模型 transcript；模型可见后果由调用方派生的工具结果与运行时上下文快照承担。
    - 按会话策略 `ask | never`；`never` **在服务内部、waterfall 之前**执行，因此后注册的 `prepend` 应答器也绕不过去。
    - `ApprovalRequest` 有意**不带工具参数**（只带 `callId` 指向已流式展示的工具调用），避免出现第二份可能漂移的副本。
- **权限预设（`ctx.permissionPresets`）**：把两个独立旋钮——沙箱模式（`sandbox/mode`）与审批策略（`approval/policy`）——捆成具名预设（`workspace-write` = workspace-write + ask；`danger-full-access` = danger-full-access + never）；`custom`/`auto` 是保留名；`set()` 先写 `permission/preset`（仅日志的用户意图）再写各自旋钮（仅当生效值变化时）。**配置错误在插件加载时即失败**（例如在没有隔离能力的 bash 执行器之上组合预设）。
- **沙箱**（`packages/sandbox`）：进程限制 seam，后端 bwrap / Landlock / Seatbelt；“fs 与 subprocess 提供方共享同一个执行世界”，因此把二者指向远端沙箱就把 Bash/PTY/LSP 整体搬走。
- **守卫（`packages/guard`）**：循环卫生守卫——建议性的重复调用提醒 + `tools/execute` 截止时间强制执行器。
- **文件系统守卫**：`tools/pre-execute` 阶段通过 `fs/*` 事件实现“先读后编辑”检查（`fs/write-intent` / `fs/edit-intent`）。
- **权限的 UI 语义**：拒绝时结果带结构化 `ToolErrorInfo{name, code, reason?}`（`reason` 面向用户、不进模型内容），并且 `reason`/`displayReason` 分离（审计文本 vs 本地化提示文本）。

**源码要点（第 2 版补充，`packages/interaction/user-approval/src/index.ts` + `packages/sandbox/sandbox-policy/src/index.ts`）**

- **策略本身是“模型可见上下文”而不是系统提示**：审批策略以运行时上下文段落 `approval:policy` 的形式贡献（`getContextOrder('APPROVAL_POLICY')` 取位置），因此策略切换**不会改写系统提示缓存前缀**；模型通过两条固定语句得知当前策略（`never` 语句还明确告诉模型“不要请求沙箱升级”）。
- **策略写入是纯日志事件**：`setApprovalPolicy()` 只 `append('approval/policy', {policy})`，并要求它在未结束的轮次内提交（审计对必须被日志提交边界包住）。
- **审计事件不进模型记录**：`approval/asked` 与 `approval/decided` 是仅日志事件；模型看到的是调用方派生的工具结果与策略上下文。
- **非法策略值在写入点就抛错**（`APPROVAL_POLICIES` 白名单校验），而不是等到读取时归一化。

### 9.2 agentxx：统一判定 + 插件声明 + 声明式询问卡片

- **权限中间件**（`middlewares/permission.h`）：`XXRouter<PermissionOperator, 2>` 按读/写两个作用域做**最长路径匹配**（支持 `*` 通配符）；未命中规则的默认动作 `noRuleOperator` 由 `permission.mode` 决定（`ask`/`all_ask` → INTERRUPT、`pass` → ALLOW、`deny` → DENY）。
- **插件声明权限**：`ToolPermissionSpec{scope, targetKind(None/Path/Text), targetArgs, category}`；工具参数按**实际 JSON 类型**处理（字符串单目标、数组逐项）；未声明权限的工具**不参与判定**（直接放行）。
- **三态批量查询**：`check_paths` 返回 `DENY/ALLOW/ASK`（不发起询问），供 glob/grep/list 这类“模式/前缀参数工具”逐项复核，防止 `**` 模式绕过子目录拒绝规则；逐项丢弃被拒或未获批准的路径。
- **工作区隔离**：`SessionFsIsolation{allowPath, denyWritePath}`（worktree 模式），子目录内的写拒绝优先判定。
- **询问与记住选择**：INTERRUPT 经中断 UI（`InterruptUi` 声明式卡片 + `preset::permissionCard`）询问；用户选择可被记住；“完全授权”状态由宿主广播（`WireSetFullAuth` / `WirePermissionState`）。
- **命令与网络等文本目标**：`targetKind=Text` 原样匹配（命令、URL）。

**源码要点（第 2 版补充，`agent/lib/src/middlewares/permission.cpp`）**

- **判定顺序（`decideTarget`）是固定的**：worktree 会话隔离（先判“是否在 worktree 子树内”，再判“是否落在主检出写边界内 → 拒绝”）→ 配置文件的显式拒绝路径（**即使已完全授权也拒绝且不询问**）→ 完全授权 → 最长前缀规则表 → `noRuleOperator` 兜底（默认 `ALLOW`，CodeAgent 按 `permission.mode` 设为询问/拒绝/放行）。
- **空/无法规范化的目标按“未获批准”处理**（返回 `Ask` 而不是 `Allow`）：`decidePaths()` 会先按会话工作目录把相对路径规范化，规范化失败就视为未批准 —— 这是“宁可多问一次”的取向。
- **已知未处理的绕过面（代码内 TODO，值得优先修）**：判定基于**词法规范化路径**，不解析符号链接；允许目录内的软链（`root/link -> root/deny`）被读写时会跟随进入被拒目录，且逐路径过滤接口看不到链接目标。彻底处理需对已存在路径做 `weakly_canonical` 后再判定一次（影响所有工具与查询接口，需评估性能与 Windows 语义）。
- **询问没有超时**：`requestPermission` 经 EventBus 请求时**不限制等待时间**（用户可能长时间不响应），并显式注释说明避免被总线默认 30s 超时误判为拒绝；**没有会话总线时直接返回拒绝**（安全默认）。
- **相对路径按“会话生效工作目录”规范化**，与工具实际访问路径、规则匹配口径一致（避免“规则按主检出匹配、工具按 worktree 访问”这类错位）。

### 9.3 对比

| 维度 | dsh | agentxx |
|---|---|---|
| 旋钮划分 | 沙箱模式、审批策略两个独立 knob + 预设捆绑 | 单一 `permission.mode` + 规则表 + 完全授权 |
| 失败姿态 | 一律 fail closed（unavailable → 拒绝） | 未命中规则按 `noRuleOperator`，默认历史行为是 ALLOW |
| 守卫单调性 | 有（guard 只能拒绝） | 无（钩子可改写输入） |
| 执行隔离 | 沙箱后端（bwrap/Landlock/Seatbelt）+ 可换 fs/subprocess 提供方 | 进程内路径级规则 + worktree 边界；无进程隔离 |
| 审计 | `approval/asked/decided` 持久（不进模型） | 询问/结果进会话轨迹（`InterruptHandleArg`/结果） |
| 询问语义 | 一次性授权（`allowed-once`），参数不重复传递 | 声明式卡片 + `{"values":{...}}`，可记住选择 |
| 批量复核 | 由 fs 工具在 `pre-execute` 内自查（`fs/*` 事件） | 内建 `check_paths` 三态批量查询（更实用） |

### 9.4 可迁移到 agentxx 的设计

1. **把“默认放行”改为可显式声明的失败姿态**：现在“未声明权限的工具直接放行”与“未命中规则默认 ALLOW”是两处隐式放行。建议 (a) 在插件清单里要求显式声明 `permission: none | declared`，未声明时记一条 debug 日志（可审计）；(b) 提供 `permission.defaultNoRule` 配置项，允许“未知即询问”。
   落地：`permission.h` + `plugin_manager_vtable.cpp`。
2. **沙箱/执行后端抽象**：把“命令执行的世界”抽成后端（本地 / worktree 内 / 未来远端/容器），权限与路径规范化都基于后端提供的“执行世界根”。这是 dsh 最值得抄的一条架构原则（`fs` 与 `subprocess` 共享同一世界）。它对 agentxx 的直接收益是：worktree、远程 agent、容器化三种模式共用一套路径与权限逻辑，而不是靠 `SessionFsIsolation` 特判。
   落地：`agentxx_execute_command` 插件 + `permission.h` + `context.h` 的会话工作目录/隔离字段。
3. **审批决策的“一次性授权”语义**：当前“记住的选择”是持久策略；建议再区分 `allowed-once`（仅本次）与“记住”两类结果，并把二者在 UI 上明确区分（dsh 的调用方对非 `allowed-once` 一律拒绝，语义清晰、无歧义）。
   落地：`interrupt_presets.h` 的权限卡片 + `permission.cpp` 的结果解析。
4. **审计与展示分离**：询问过程建议同时落一份“仅审计”记录（谁在何时批准了什么），与用户可见的会话消息分开；dsh 的做法是审计事件不进模型 transcript。agentxx 现在询问卡片本身就是会话消息，复盘时难以区分“决策”与“对话”。
   落地：`SessionStore` 新表 + `permission.cpp`。
5. **“先读后编辑”守卫**：对写类工具引入“本次会话是否已读过目标文件”的检查（需要时可由插件实现），避免模型盲改文件。这是 dsh 在 `tool-fs` 下的标准能力，agentxx 目前没有。
   落地：`agentxx_filesystem` 插件 + 会话级读取记录。

---

### 9.5 各自做得好的设计

**agentxx 相比 dsh 做得好的：**

- **规则表达力更强也更贴近日常**：读/写两套规则表 + 最长路径前缀匹配 + 通配符，还能按“文本目标”匹配命令/URL；`permission.mode`（ask/all_ask/pass/deny）给出未命中规则时的明确语义。
- **插件声明式权限，零配置即可扩展**：插件注册工具后声明 `{作用域, 目标来源(None/Path/Text), 目标参数名}`，参数是数组时自动逐项判定；未声明权限的工具直接放行 —— 新插件不必改宿主权限代码就能接入统一判定。
- **批量三态查询解决真实绕过**：`check_paths` 返回 `DENY/ALLOW/ASK` 且不弹窗，让 glob/grep/list 这类“模式参数工具”逐项复核并丢弃拒绝项，防止 `**` 之类的模式绕过子目录拒绝规则。
- **会话隔离与授权状态是运行时可见的**：worktree 绑定会同时切换工作目录基准与权限边界（`allowPath` 例外优先、`denyWritePath` 写拒绝），完全授权状态由服务端广播、客户端乐观更新后校准。
- **询问本身是声明式 UI**：权限卡片可含多个控件、一次提交，并能“记住选择”；询问等待**不设超时**（考虑用户可能长时间不响应），没有会话总线时按拒绝处理。

**dsh 相比 agentxx 做得好的：**

- **失败姿态明确 fail closed**：`unavailable`/超时/应答器异常一律按拒绝；`never` 策略在服务内部、waterfall 派发**之前**生效，后注册的应答器也绕不过。
- **单调 guard + 参数不可改写**：拒绝一旦作出就不能被后续监听器翻回允许；参数在记录与展示之后不可变，审计与执行永远一致。
- **两个正交旋钮 + 预设**：沙箱模式与审批策略各自独立演进，再用具名预设（workspace-write / danger-full-access）捆给 UI 选择；配置错误在插件加载期就失败（例如在没有隔离能力的执行器上组合预设）。
- **执行世界抽象**：`fs` 与 `subprocess` 共享同一执行世界，因此把二者指向远端沙箱就把 Bash/PTY/LSP 一起搬走 —— 权限与路径逻辑不必为每种形态改一遍。
- **审计与展示分离**：`approval/asked`/`approval/decided` 只写日志、不进模型 transcript；拒绝结果携带结构化错误码与“面向用户的原因”，与模型可见内容分开。

---

## 10. 子代理、后台任务与工作流

### 10.1 dsh

- **subagent seam（`ctx.subagents`）**：与其他能力 seam 不同，**同一上下文可共存多个提供方**，按名称注册（进程内 spawn / 进程内 fork / ACP / Codex / Claude Code / dsh-sdk 六个兄弟包）。
    - **能力声明 + 前置校验**：提供方静态描述符声明能力（`agentOptions`/`outputSchema`/`depthLimit`/`toolFilter`/`persona`）；请求若依赖提供方没有的能力会被**明确拒绝**（`UNSUPPORTED_CAPABILITY`），绝不“接受后静默忽略”（"fail loud, no silent degradation"）。
    - **单次启动**：`SubagentStartRequest{prompt, parent, signal, agentOptions?, outputSchema?, maxDepth?, toolFilter?, persona?}`；父级派生 cwd/谱系/委派深度；`toolFilter` 在子 agent 创建窗口内是**可见性**过滤（工具从提示词消失且拒绝执行，与不存在无法区分）。
    - **可继续子 agent（Activation）**：持久化子会话 + 至多一个进程内激活（retained `AgentHandle` + inbox 作为唯一 FIFO + 拥有的子激活集合）；`sendMessage()` 按目标状态路由（running → steer 最近 step；waiting → 唤醒并 steer；无激活 → **冷恢复**）；权限来自“确切在线 sender”（parent↔child 双向鉴权，sibling/self/陈旧对象拒绝）；`interrupt()` 是唯一公开停止操作（`cancel(cause, {keepInbox:true})`）。
    - **子级优先释放**：父级在拥有未 dispose 的子激活时无法 settle。
- **jobs（`ctx.jobs`）**：通用后台任务运行时，`JobKind` 可合并扩展（`bash`/`subagent`）；`JobSpec{kind, label, owner, outputLimitBytes?, output?, run}`；`JobId` 品牌化 `<kind>-N`；状态 `running|stopping|completed|killed|failed`；输出环（append 按 UTF-8 字节推进 offset）+ `progress` 行 + 结算 `detail`/`result`；**owner 的活跃 Agent 一旦销毁就取消并等待该 job**。
- **workflow（`ctx.workflowEngine`）**：模型编写编排脚本；`WorkflowStartRequest{script, meta, args, subagentProvider?, maxTotalAgents?, parent, signal?}`；脚本里 `agent()` 派生出的每个子 agent 都归属 `parent`（cwd/谱系/深度经 subagent seam 传递）；引擎用 schema 校验 `meta`，**绝不通过求值脚本文本获取数据**。
- **goal / plan / schedule / todo**：会话级持久目标（带修订号的状态机 + Goal Round 上限）、plan 协作状态（`/plan` 直接进入 + 经评审退出）、Host 拥有的定时后续操作、`todo_write` 工具各自独立成包。

**源码要点（第 2 版补充，`packages/jobs/jobs/src/index.ts`）**

- **访问边界是“owner 会话”而不是“id 保密”**：读、等待、kill、删除等每个操作都接收 `caller` 会话并与 job 的 owner 比对；job id 是可预测的（`<kind>-N`），**授权**才是边界。
- **结算是一次性的（first-wins）**：一条终态记录 → 释放等待者 → 广播一次 `settled` 事件（事件里报告“是否释放了等待者”`awaited`，让完成报告方跳过没人等的结算）。
- **记录的生命周期比任务长**：已结算的记录会**保留在可见集合中**直到 owner 被销毁或被显式移除，因此“任务完成后还能回看输出”是设计的一部分。
- **输出环与结算裁剪**：每个 job 一个输出环，超过上限丢最旧（append 永不失败）；结算时按“结算保留策略”裁剪并结束流；环没有独立生命周期。
- **owner 销毁即取消**：owner 的活跃 Agent 一旦被销毁就取消并等待其 job；即使 teardown 取消本身抛错，也只把该记录强制标记失败（不谎称“工作已停止”），结算以 `cause: 'teardown'` 广播。
- **事件投递可按 owner 过滤**：`{ owners: 'scope' }` 下，注册在无作用域上下文里的监听器服务所有 owner，注册在某个 agent 作用域下的只服务该 agent。

### 10.2 agentxx

- **subagent 委派**（`middlewares/subagent_manager.h` + `tools/subagent.h` + `AgentHost`）：
    - 模型经 `agentxx_subagent` 工具发起委派；`SubAgentTaskBase` 只有静态元数据（name/depict/systemPrompt），**实际执行由 `AgentHost` 派生独立 agent**（中断委派，不再用图内嵌套 subgraph）。
    - 委派请求经 EventBus `service.subagent.execute` 传递：`ReqSubagentBatch{parentAgentName, parentSessionId, tasks[]}` + 级联 `CancelToken`（父取消 → 中止全部在跑子代理）；`sessionDepth_` 限制嵌套深度预算。
    - 结果是 `RespSubagentBatch`，按 `makeSubagentResumeKey(toolCallId, resultId, idx)` 写回中断结果 map（单任务返回纯文本，多任务按序编号，错误任务写 `{"error": ...}`）。
    - 中断路径：`NodeInterrupt` + `AgentRunner` 的中断处理循环（`resume` 后回到原 toolcall）。
    - 压缩子代理（`summarization`）复用同一套 `subagent` 服务（同 thread/同模型以命中 KV cache、无工具、禁二次压缩）。
- **无后台任务（jobs）概念**：`agentxx_execute_command` 支持 `timeout`（默认 60s，0=不限）、取消注册表、进程组清理（`kill(-pid)`，含 bash 派生的子孙），但**没有“命令继续跑、模型继续对话、稍后读取输出”的作业模型**。
- **无工作流脚本**：没有“模型写脚本、脚本派生并编排多个子代理”的能力；编排目前只能靠模型自己一轮轮调用工具。
- **计划/待办**：`agentxx_planning` 插件（两级规划：roadmap + todo 列表 + memo，带客户端渲染注册）；`agentxx_planning` 通过通用持久化 + 事件，不依赖专用接口。

**源码要点（第 2 版补充，`agent/lib/src/agent/agent_host.cpp`）**

- **委派入口是事件总线的请求-响应**：子代理请求经 `ctx->bus->getRR<ReqSubagentBatch, RespSubagentBatch>` 到达宿主（`AgentHost` 注册服务端），因此**非工具路径**（如压缩子代理）也能发起委派。
- **深度预算是按会话维护的 map**：`sessionDepth_`（子会话 id → 深度）配合 `cfg_.maxDepth`，超限直接返回错误项（`Subagent \`x\` rejected: max nesting depth (N) exceeded`），且**父会话被取消时不再派生新任务**（spawn 前先查 `cancelToken->is_cancelled()`）。
- **子代理配置由根 agent 派生**：`toolWhitelist = rootAgent_->getContext()->toolNames`（**继承父 agent 的工具集合**）、`inheritedWorktreePath`（继承 worktree 绑定，供提示词中间件注入）、工作目录等一次性派生，子代理本身不感知绑定机制。
- **逐任务的取消级联与清理**：`CancelToken` 从父传到每个子任务；任务结束后从 `sessionDepth_` 移除该子会话条目（避免长期运行的宿主累积）。
- **结果按固定 key 规则回填**：`makeSubagentResumeKey(toolCallId, resultId, idx)` 由“中断写入侧”和“工具读取侧”共用（防止规则漂移），单任务返回纯文本、多任务按序编号、失败任务写成 `{"error": ...}`。

### 10.3 对比

| 维度 | dsh | agentxx |
|---|---|---|
| 子代理提供方 | 6 个可互换提供方（含进程外产品/协议） | 1 种（宿主内派生 agent） |
| 能力声明 | 静态描述符 + 前置校验（缺失能力明确拒绝） | 无（能力差异靠配置与代码分支） |
| 交互式子代理 | 可继续 activation（steer / 冷恢复 / 双向消息 + 鉴权） | 一次性委派 + 结果回填 |
| 释放顺序 | 子级优先（父级无法在子级未释放前 settle） | 级联取消；无“父等子”的显式约束 |
| 后台任务 | jobs seam（输出环、进度、owner 取消） | 无（命令工具同步等待 + 超时） |
| 工作流 | 模型写脚本编排多个子代理（沙箱执行） | 无 |
| 计划/待办 | goal / plan / schedule / todo 各自成包 | planning 插件（roadmap+todo+memo） |

### 10.4 可迁移到 agentxx 的设计

1. **子代理能力声明与前置拒绝**：给 subagent 委派增加“能力声明”（是否支持模型覆盖、是否支持结构化输出、是否支持工具过滤、是否支持 persona）并在委派**之前**校验，缺失能力时明确失败而不是静默忽略。当前 agentxx 的差异靠配置分支实现，容易变成“看起来成功但没生效”。
   落地：`tools/subagent.h` 的 `SubAgentTaskBase` + `AgentHost` 的派生化。
2. **后台任务（持续命令/长任务）**：这是当前最明显的功能缺口。建议引入 `JobSpec` 式的抽象：任务由 agent 拥有的运行时登记（id、类型、标签、owner 会话、输出环、进度行），工具提供 `list`/`read`/`stop`，**owner 销毁时取消并等待**。直接收益：长构建/长测试不再阻塞模型轮次，也不占满超时预算。
   落地：新的 `middlewares/job_manager.h`（或 `tools/job.h`）+ `agentxx_execute_command` 插件增加“后台模式”参数 + `wire_protocol` 增加 job 列表/输出读取消息。
3. **可继续子代理（交互式会话）**：agentxx 的委派是“一次性、结果回填”。dsh 的 activation 模型（子会话持久 + 空闲时驻留 + `send_message` 冷恢复 + 双向鉴权）值得逐步引入，至少先做“**同一子代理可被多次追问**”（保留子会话，后续消息进其 inbox），这是多代理协作的基础。
   落地：`AgentHost` 的会话生命周期 + `subagent` 工具参数（`target`/`sessionId`）。
4. **工作流的轻量替代**：不需要引入 JS 脚本引擎，但可以提供一个“**批量委派工具**”：一次调用传入多个任务描述，宿主并行派生多个子代理并把结果汇总（agentxx 的 `ReqSubagentBatch` 已经支持批量语义，只是未暴露为模型可用的编排能力，`maxTotalAgents` 式上限也可直接复用）。
   落地：`SubAgentManagerTool::execute_async` 的批量分支 + 提示词。
5. **计划状态的独立持久化**：`agentxx_planning` 已用“通用持久化 + 事件”实现 roadmap/todo/memo；建议将其纳入 §3.4 的“来源/原因”体系（计划变更作为可追溯的会话状态），并在 Info 面板以投影形式展示（而不是客户端自行解析工具参数）。

---

### 10.5 各自做得好的设计

**agentxx 相比 dsh 做得好的：**

- **委派只有一个入口，内部路径复用同一服务**：模型工具与内部路径（压缩子代理）都走 `service.subagent.execute` 的请求-响应，因此“内部也要派生 agent”不需要另开通道，也就不存在两套语义漂移。
- **深度与继承规则显式**：`sessionDepth_` + `maxDepth` 给出嵌套预算；子代理**继承父的工具集合与 worktree 绑定**，行为可预期；父被取消后不再派生新任务，任务结束清理深度条目避免长期累积。
- **结果回填的 key 规则由双端共用**：`makeSubagentResumeKey` 同时被“中断写入侧”和“工具读取侧”调用（含单任务纯文本/多任务编号/错误对象的约定），从机制上消除规则漂移。
- **委派的落地成本低**：不需要额外的作业系统或提供方注册表，一个中间件 + 一个工具 + 一个宿主服务就完成委派链。

**dsh 相比 agentxx 做得好的：**

- **subagent 是 seam，多提供方按名注册**：进程内 spawn/fork、ACP、Codex、Claude Code、SDK 六个提供方共用同一接口；提供方用静态描述符声明能力（agentOptions/outputSchema/depthLimit/toolFilter/persona），请求依赖缺失能力时**明确拒绝而不是接受后静默忽略**。
- **可继续子代理（activation）**：持久子会话 + 至多一个驻留激活 + 收件箱作为唯一 FIFO；支持 steer 最近 step、唤醒、冷恢复，子↔父双向鉴权，且父级在子激活未释放前无法 settle（子级优先释放）。
- **后台作业运行时（jobs）**：owner 授权（id 可预测、授权才是边界）+ 输出环（按上限丢最旧）+ 进度行 + 结算 first-wins + 结算记录保留到 owner 销毁 + owner 销毁即取消并按 `teardown` 结算。
- **工作流让模型自己编排**：脚本可派生多个子代理，`meta` 有 schema 校验且在开始前拒绝无效数据；脚本不能观察或替换引擎级策略（子提供方/总子代理上限）。
- **目标/计划/排程/待办各自成包**：可以只装其中几个，不存在“必须接受整套”的耦合。

---

## 11. 会话检索、标题、遥测、附件与大输出

### 11.1 dsh

- **session-query**：逻辑会话语料库（live 优先）+ 有界读取 + 血缘 + 语义过滤 + **SQLite 全文搜索**；`SessionLogSnapshot`（脱离运行时、经回放验证的完整原始日志）、`SessionSurfaceSnapshot`（一次原子观测 + `capturedThroughSeq`）、`SessionTitleObservation`（标题与 header 同源观测，便于授权校验）。分类 `'current' | 'shadowed' | 'log-only'` 复用与模型历史推导**同一个 `foldSurface()`**，不会出现“查询口径与运行时口径不一致”。
- **session-title**：唯一提供方（`ctx.sessionTitle`），标题由日志折叠得出，可被检索与列表使用。
- **session-telemetry / product-telemetry**：会话遥测（事件、用量）与产品遥测分开，后者是可选能力。
- **attachment**：`ctx.attachments` 把二进制所有权与会话日志分离——只有对象持久化完成后才发布**不可变内容寻址引用**（本地后端 `sha256:<digest>`）；会话事件与模型可见块只带引用与**经校验的元数据**（媒体类型、字节数、固有宽高、可选原始尺寸）；有明确的准入上限（每条消息 ≤20 张、单图 ≤20 MiB / 6400 万像素 / 单边 8192，规范化后长边 ≤2048、≤4 MiB）。`ctx.fileUploads` 单独管理浏览器上传的暂存凭证。
- **spill**：`ctx.spillStore.saveText()` 原样持久化文本并返回**不透明定位符 + 检索提示 + 精确字节数**；`SpillOwner{sessionId}` 决定存储分组（fork 的子会话继承已存在的定位符但**不复制、不转移所有权**）；来源信息（工具调用 id 或源会话）只用于描述，**从不作为访问控制**。

### 11.2 agentxx

- **会话列表/检索**：`SessionStore::listSessions` / `listSessionsPage`（keyset 游标分页 + 两阶段扫描 + 早停），meta 里有 sessionId/title/lastActiveMs；标题目前来自会话内容或目录名兜底；**没有全文检索**。
- **RAG 检索工具**：`agentxx_rag_search` 插件基于 embedding 对配置的文档路径做语义检索（需要 `agentxx.agent.model` 接口）；另有 `agentxx_codegraph` 插件做代码符号/调用图检索。
- **share_store**：`agentxx_share_store` 工具 + `store` 表（自增 id → 文本），内存只保留最近 3 条（LRU），用于把长输出外置成 id 引用；工具可声明 `AUTO_SUMMARY` 标志自动外置。
- **附件**：`MediaAttachment`（dataUrl / pathOrUrl），远程模式下可选“服务端文件”标签页（只发路径，服务端自行读取编码）；会话消息里保存附件元数据。

**源码要点（第 2 版补充，`agent/lib/src/agent/base_agent.cpp`）**

- **服务端附件自主加载**：当 `dataUrl` 为空且 `pathOrUrl` 是本地路径（非 http/https）时，服务端直接读文件并转成 `data:...;base64,...`；读取前按媒体类型做**体积上限检查**（超限只记日志并保持原样），路径经 `utf8ToPath` 转换（Windows 客户端中文路径）。
- **读文件与 base64 走线程池**：单附件可达数 MB，若在 io 线程上做会阻塞同一时刻所有会话的流式输出与工具执行；无线程池（测试/嵌入式）时才退化为同步执行。
- **大包规避**：`TurnStart` 增量只回显附件**元数据**（显式清空 `dataUrl`），客户端首屏即可渲染附件卡片，完整 dataUrl 由后续 `Sync`/会话库提供。
- **遥测**：有日志（`XX_LOG*`）与基准统计（`AgentConfigStatic::enableBenchmark`），无产品遥测。

### 11.3 对比与迁移建议

| 维度 | dsh | agentxx |
|---|---|---|
| 会话检索 | 全文索引 + 语义过滤 + 血缘 + 有界读取 | 列表/分页；无全文检索 |
| 查询口径一致性 | 复用运行时 `foldSurface()` | 无对应概念（查询即读表） |
| 标题 | 独立 seam（日志折叠） | meta 表 + 内容兜底 |
| 附件 | 内容寻址 + 校验元数据 + 准入上限 | dataUrl / 路径，元数据随消息 |
| 大输出 | spill seam（定位符 + 检索提示） | share_store（id + 内存 LRU 缓存） |
| 遥测 | 会话遥测与产品遥测分离 | 日志 + 基准统计 |

**建议**：
1. **会话全文检索**：SQLite 已内置 FTS5，成本很低。把 `view_message`/`llm_context` 的关键文本建 FTS 索引（或按需构建），支持“按关键词找历史会话/找某次工具输出”。这是 dsh `session-query` 中最实用的一条。
   落地：`session_store.cpp` 的 `ensureSchema` + 新查询 API + TUI 会话弹窗搜索框。
2. **附件校验元数据与准入上限**：给 `MediaAttachment` 补上媒体类型/字节数/像素尺寸等**经校验**的元数据（现在客户端与服务端都可能只信任声明），并加准入上限（单图字节数、像素、每条消息张数）。收益：防止超大图片把上下文与内存打爆；UI 无需解码即可排布历史。
   落地：`agent/lib/include/agentxx/agent/conversation_types.h` + `session_server_agent_io.cpp` 的附件接收路径。
3. **spill 的“定位符 + 使用说明”**：share_store 现在只给模型一个 id，模型需要知道“怎么用”。建议返回结构化的“定位符 + 字节数 + 检索/分段读取提示”，并允许分段读取（`read(id, offset, length)`），避免模型一次性把整段拉回来又被截断。
   落地：`tools/share_store.cpp` + 提示词。
4. **会话标题 seam**：把“标题生成”做成独立能力（首次成功后落库、可手动重命名、可用于列表与检索），而不是隐含在 UI 或会话内容里。
   落地：`session_store.cpp` meta 表 + TUI 列表。

---

### 11.4 各自做得好的设计

**agentxx 相比 dsh 做得好的：**

- **服务端附件自主加载**：只传路径时由服务端读文件编码（读盘与 base64 卸载到线程池，避免阻塞同一时刻所有会话），并在读取前按媒体类型做**体积上限检查**；中文路径经 `utf8ToPath` 处理。
- **大包规避做在协议层**：`TurnStart` 增量只回显附件**元数据**（显式清空 `dataUrl`），客户端首屏即可渲染附件卡片，完整内容走后续 `Sync`。
- **会话列表可用性更强**：keyset 游标分页 + 两阶段扫描 + 早停，直接支撑“几百上千个会话”的浏览；`store` 表按需取 `max(id)` 恢复自增计数，内存占用与条目数无关。
- **外置引用贴近工具用法**：share_store 给模型一个整数 id，工具可声明 `AUTO_SUMMARY` 自动外置超长输出，压缩时会顺手把长文本换成 id 引用。

**dsh 相比 agentxx 做得好的：**

- **附件是不可变内容寻址引用 + 经校验元数据**：会话事件里只有 `sha256:<digest>` 这类不透明 id、媒体类型、字节数与尺寸（尺寸让客户端不解码即可排布历史），并有明确准入上限（每条消息 ≤20 张、单图 ≤20 MiB / 6400 万像素 / 单边 8192，规范化后长边 ≤2048、≤4 MiB）。
- **spill 给的是“定位符 + 使用说明”**：返回不透明定位符、精确字节数与后端提供的检索提示；`SpillOwner` 决定存储分组，fork 子会话**继承已存在的定位符但不复制、不转移所有权**；来源信息只用于描述、绝不作为访问控制。
- **查询口径与运行时一致**：`session-query` 的事件分类复用与模型历史推导同一个 `foldSurface()`，并提供 `SessionLogSnapshot` / `SessionSurfaceSnapshot`（带 `capturedThroughSeq`）这样“一次原子观测”的读法；agentxx 的查询是直接读表，没有“查询口径”这一层。
- **标题与遥测分层**：标题是独立 seam（由日志折叠、可批量观测、可校验来源 header），会话遥测与产品遥测分开，各自可替换。

---

## 12. 客户端 UI 与客户端插件

### 12.1 dsh：Host 权威 + Client 投影 + 插槽组合

- **分层**（`docs/subsystems/web-client.zh.md#分层与所有权`）：Host 应用（权威状态/持久化/顺序/访问策略/流生产）→ 传输与 API assembly（Connection + API Gateway + Remotes）→ **Client model**（不依赖 React 的 Host 状态镜像，处理 stream/unary 竞态、对象 identity）→ UI adapter → Conversation 数据（每个 target 自己组装快照：`chat`/`trajectory`/Turn Tail）→ 组合与渲染（Slots + Renderer）→ React。
- **硬约束**：`Presentation component 绝不接收 Cordis ctx、transport object 或其他功能插件的实现`；功能包依赖生成的 service face，而非 Gateway 实现。
- **数据通路**分四类，各有独立的恢复语义：持久会话展示（journal + `page()` 补历史 + gap repair）、瞬态控制（每代新 baseline 原子替换）、后台任务（job 名册 + 输出视图）、Workspace 状态（baseline + increment）。
- **重连**：物理恢复（WebSocket）与逻辑恢复（每个 stream 自行重开）独立；持久 journal 校验逻辑 seq 区间；普通转发通知不 replay（需要可靠恢复的域必须提供 baseline/cursor/查询）。
- **客户端插件体系**：`dsh.client` 声明 + `__DSH_BOOT__` 图（entry + batch + rev），bundle 按 combo 路由 + HMR（按 mtime/ctime/size 派生 rev，不哈希可执行字节）；**运行时自修改的客户端半边**需用户批准。
- **UI 扩展点**：Slots（类型化 registry + lifecycle ledger）、Conversation target（keyed renderer）、Settings card、命令（`ctx.commands`，不走模型）、快捷键（每个命令 owner 声明各平台默认键位）、**UI 文案归语言包**（`verify-client-ui-i18n` 拒绝硬编码文案）。

### 12.2 agentxx：TUI + 声明式组件 + 客户端插件

- **端点抽象**：`AgentIOBase` 派生 stdio CLI / TUI / 服务端端点（`SessionServerAgentIO`）；client 与 agent 既可在同进程（Channel），也可跨进程（WebSocket）。
- **设计约定**（项目 AGENTS.md）：client 只做 UI 渲染，数据来源与消息插入尽量由 agent 提供。
- **声明式组件层**：`agentxx.ui.item`（数据层，零 ABI 变更）+ 唯一渲染实现 `ui_components.{h,cpp}`：文本/差异/状态图/横排/分组框/折叠/表格/树/键值/趋势图/计量条/控件/提交行/自定义；命中测试统一走 `measureItem`；表单状态由宿主维护（`UiFormState`），提交经动作通道回传 `__submit`/`__cancel`；有 1 MiB 单条描述上限、条目 `version`、树折叠由宿主管理。
- **中断（HIL）**：中断描述与表单共用同一套渲染与交互（`InterruptView`），结果回传 `{"values":{...}}`。
- **客户端插件**：C ABI，client 侧 9 张接口表（ui/events/session/wire/self/json/log + timer/keybind）；定时器与快捷键独立成表（动画等级 Disabled 时拒绝注册；键位规范化 + 冲突先注册者优先）；可见性快照每帧上报，`pause_when_hidden` 据此顺延。
- **远程交互**：HelloAck 携带服务端设备 id 与会话工作目录；文件选择弹窗有“本地/服务端”标签页；完全授权状态由服务端广播 + 客户端乐观更新校准；设置弹窗/更新检查等本地能力。

### 12.3 对比

| 维度 | dsh | agentxx |
|---|---|---|
| 形态 | 浏览器 Web + Electron 桌面 + headless | TUI（FTXUI）+ stdio CLI + 远程客户端 |
| 状态权威 | Host 权威，Client 是不依赖框架的镜像模型 | agent 权威，client 只渲染（显式约定） |
| 扩展点 | Slots / Conversation target / Settings card / 命令 / 快捷键 | ui.item 组件 + 描述式面板/overlay/装饰 + 客户端插件表 |
| 数据恢复 | 四类通路各有 baseline/cursor/page 语义 | delta 缓冲 + 重连重放（`deltaBuffer_` 有上限）+ Wire 同步 |
| 插件分发 | bundle 图 + combo 路由 + HMR | 动态库 + 接口协商（双端入口探测，dlopen 卸载到线程池） |
| 文案 | 语言包字典 + 门禁拒绝硬编码 | `tui_i18n.cpp`（TUI 文案表） |
| 表单/交互 | Conversation + 交互 seam（审批/提问） | 声明式控件 + `UiFormState` + 动作通道 |

### 12.4 可迁移到 agentxx 的设计

1. **客户端模型层（不绑定 UI 框架）**：agentxx 的 TUI 直接消费 Wire 增量。dsh 的经验是“Client model 处理 stream/unary 竞态与对象 identity，UI 只订阅快照”。建议把 TUI 的状态收敛为一个**可测试的模型层**（会话列表/当前会话/消息窗口/投影值 + 明确的合并规则），UI 组件只读快照。直接收益：重连、分页、乐观更新的正确性可以单独测试，不必起 UI。
   落地：`agent/client/src/io/tui/framework/tui_state.h` + 新增 model 层与单元测试。
2. **按数据语义定义恢复策略**：把 Wire 数据分为“持久展示 / 瞬态控制 / 后台任务 / 工作区状态”四类，分别定义 baseline 与补历史规则（agentxx 已有 delta 缓冲与重放，但缺少“哪类数据可 replay、哪类只能替换”的显式约定）。
   落地：`session_server_agent_io.cpp` + `wire_protocol.h` 的注释与文档。
3. **插槽化的 UI 扩展点**：agentxx 的插件能注册面板/overlay/装饰/键位，但**没有通用的“UI 插槽”概念**（例如“在消息卡片尾部插入一个操作行”“在侧栏插入一段指标”）。建议引入具名 slot + 注册顺序 + 生命周期账本（与现有 `ui.item` 组件结合），让插件不必替换整个面板。
   落地：`plugin/api/client_plugin_api.h` 的 ui 表 + `agent/client` 的渲染接入点。
4. **命令与快捷键 owner 化**：dsh 让“命令 owner 声明各平台默认键位”，agentxx 已有键位表与冲突记录。建议把“命令 → 默认键位 → 用户覆盖”的三层关系显式化，并在设置弹窗里展示来源（插件名 + 是否被用户覆盖）。
5. **文案字典门禁**：agentxx 的 TUI 文案集中在 `tui_i18n.cpp`，建议加一条测试：界面代码中不出现直接面向用户的硬编码字符串（至少对新增文件生效），避免语言支持退化。

---

### 12.5 各自做得好的设计

**agentxx 相比 dsh 做得好的：**

- **声明式组件层解耦了“插件想展示什么”与“界面怎么画”**：`agentxx.ui.item` 是纯数据描述（文本/差异/折叠/表格/树/键值/趋势/计量/控件…），宿主提供唯一渲染实现与命中测试；插件升级界面观感不需要改 ABI，也不需要自带前端工程。
- **表单与中断共用一套机制**：控件状态由宿主维护（点击/键盘/校验/取值只有一份实现），提交/取消走统一动作通道；中断描述与表单只是“结果去处不同”。插件实现 HIL 交互几乎零成本。
- **客户端注册表是 COW 快照 + 线程边界明确**：UI 线程读快照、派发回 client io 线程执行，**UI 线程绝不进入插件代码**；自定义渲染器还带“关闭时先失效控制块，旧快照只能回退”的保护。
- **可解释的冲突与生命周期**：键位冲突会记录“谁占用了、谁没抢到”并在设置里展示；插件禁用/卸载时自动摘除注册（快捷键/定时器/面板/装饰），动画等级为 Disabled 时拒绝注册定时器，区域不可见时 `pause_when_hidden` 顺延。
- **同进程/跨进程统一**：`AgentIOBase` + Channel/WS 两套 transport 让“client 与 agent 同进程”和“跨设备远程”共用同一套端点与消息，客户端代码不需要分叉。

**dsh 相比 agentxx 做得好的：**

- **Host 权威 + Client 模型的严格分层**：Client model 不依赖 React、自己处理 stream/unary 竞态与对象 identity，presentation 组件拿不到 `ctx`/transport；这使 UI 正确性可以脱离框架单独测试。
- **按数据语义定义恢复方式**：持久 journal（校验 seq 区间 + `page()` 补历史 + gap repair）、瞬态控制（每代新 baseline 原子替换）、通知（不 replay）三类通路各有明确规则，而不是“统一 resync”。
- **扩展点体系更完整**：Slots（类型化 registry + lifecycle ledger）、Conversation target（chat/trajectory/Turn Tail 各自组装快照 + keyed renderer）、Settings card、命令、快捷键 owner 声明。
- **客户端插件可热加载**：bundle 图 + combo 路由 + 由 mtime/ctime/size 派生的 `rev` + HMR 对账，插件能边开发边生效；同时 `dsh.client` 声明 + index 注入让“装包=多一个界面入口”。
- **投影 seam 让客户端只读成品值**：主机侧纯同步折叠、客户端拿一致读切面（`asOfSeq`），客户端永远不会因为自己折叠事件而产生偏差；UI 文案归语言包并有门禁拒绝硬编码。

---

## 13. 远程协议、SDK 与外部集成

### 13.1 dsh

- **API Gateway / Remote BFF**：Host 业务 service 用 Typert Remote decorator 标记可调用方法，**构建期生成**严格 descriptor、runtime codec、声明合并与 source map；Client 侧按生成贡献挂到 `ctx.remote.<namespace>`；Connection 拥有物理 carrier 与信任策略，Gateway 拥有 dispatch/取消/逻辑流。
- **SDK**：进程外 JSON-RPC（TypeScript 与 Python 双 SDK，且**两个 SDK 都投影 agent loop**：loop/会话生命周期/`SessionEventMap` 变更必须同步更新两个 SDK 的预期输出）。
- **ACP**：仅自动化的 ACP 服务器（审批可交给机器决策）。
- **MCP**：每服务器一个连接插件，工具适配进原生工具注册表（共享工具名带服务器名前缀）；协议协商交官方 SDK；刷新失败保留上一代工具；服务器指令进入已记录的系统提示词。
- **hooks**：Claude Code / Codex 线协议桥接（`hook/invoked`/`hook/result` 只写日志的会话事件）。
- **命令**：`ctx.commands` 让插件注册斜杠命令，**不产生模型消息**；命令可声明自由输入与是否记录 `rawInput`。

### 13.2 agentxx

- **传输**：`AgentIOBase` 端点 + `ChannelTransport`（同进程）/ `WsIoTransport`（WebSocket）；`WireProtocol` 定义全部消息（Hello/HelloAck + 插件清单、用户输入、取消、模型选择、中断请求/响应/过期、轮次结果、上下文统计、错误、日志、模型信息、组件信息、上下文查询/压缩、会话列表/切换、插件数据上下行、消息队列、view 消息分页、目录列举、完全授权状态、权限状态、轮次）
- **协议集成**：MCP client + MCP server、ACP server（自动化）、A2A server/client（网络 agent 互调）、FFI（C API + 供其他语言嵌入的运行时）。
- **重连与缓冲**：`deltaBuffer_`（有容量上限）+ `sendMessageQueueUpdate` 推送队列变化；服务端 `SessionServerAgentIO` 由 BaseAgent 驱动。

**源码要点（第 2 版补充，`agent/lib/src/agent/io/session_server_agent_io.cpp`）**

- **增量缓冲与去重**：新 delta 只有在 `seq > 缓冲尾 seq` 时才入缓冲（重放出来的 delta 不会二次入队）；缓冲超过 `deltaBufferCap` 时从头弹出。
- **重连的两级恢复**：缓冲足够 → 重放增量；**缓冲已溢出 → 退化回全量 Sync**（注释明确：“保证重连后客户端与服务端严格一致”）；首次接入按 `initialSyncTailCount` 决定全量还是尾窗同步。
- **协议顺序陷阱被显式处理**：重放的 Sync/delta 必须**在 HelloAck 之后**发送，否则客户端会按“未握手”丢弃它们、导致重连后历史丢失 —— 这类“顺序敏感”的坑在 dsh 里由“opening baseline + 逐 stream 重开”分摊到各数据通路。
- **会话切换的防闪烁**：切到同一会话时不做全量 Sync（历史已同步），只回推一次尾窗 Sync 校准客户端状态。
- **中断等待可被取消**：中断请求的等待超时取自 `interruptTimeout`，并用 `bind_cancellation_slot` 让 `WireCancel` 能立即打断等待（而不是等超时）。

### 13.3 对比与迁移建议

| 维度 | dsh | agentxx |
|---|---|---|
| 远程调用层 | Typert 类型图生成（构建期） | 手写 Wire 结构 + JSON 编解码（`wire_protocol.h/.cpp`） |
| 版本演进 | 生成产物随类型变化；协议与类型强一致 | 手动维护字段与向后兼容 |
| SDK | TS + Python（都投影 loop，有门禁） | FFI C API（供嵌入） |
| 外部协议 | ACP、MCP、hooks 桥接 | MCP（client+server）、ACP、A2A |
| 命令面 | 插件注册斜杠命令（不走模型） | TUI 本地命令（未见插件注册通道） |

**建议**：
1. **从结构体生成 Wire 编解码**：现在每个 Wire 消息都要手写 `xxxToJson/xxxFromJson` 与消息类型分派（`wire_protocol.cpp` 19 KB），字段增删容易漏。可以用一个简单的声明式表（或宏/X-macro）生成两侧编解码 + 类型分派，并生成一份“协议字段清单”文档。
   落地：`agent/lib/include/agentxx/agent/io/wire_protocol.h`。
2. **插件可注册命令（不走模型）**：给插件提供“命令注册”能力（名称、说明、是否接受附件、执行体、结果渲染），让 `/xxx` 直接由 agent 侧执行（如 `/compact`、`/model`、`/permission`）。收益：省 token、省一次模型往返，且能表达“用户意图无法用自然语言可靠描述”的操作。
   落地：新的 `agentxx.agent.commands` 接口表 + `wire_protocol` 的命令上行 + TUI 输入栏识别。
3. **协议字段的可观测性**：为 Wire 层加一个“协议版本 + 能力位”握手（HelloAck 已有插件清单，可扩展为能力位集合），便于老客户端与新服务端组合时明确降级路径（现有“能力协商”只覆盖插件接口表）。
4. **SDK 投影测试**：agentxx 的 FFI 已存在，建议对齐 dsh 的做法：**凡影响对外协议/事件语义的改动，必须同步更新 FFI 测试与协议文档**（把这条写进贡献约定，避免协议漂移）。

---

### 13.4 各自做得好的设计

**agentxx 相比 dsh 做得好的：**

- **一套 Wire 协议覆盖同进程与跨进程**：`AgentIOBase` 端点 + `ChannelTransport`/`WsIoTransport`，client 与 agent 既可同进程也可跨设备，客户端不因部署形态分叉；协议本身是显式的结构体集合（`wire_protocol.h`）。
- **顺序敏感的坑被显式处理**：重放的增量/全量 `Sync` 必须在 `HelloAck` 之后发送（否则客户端会按未握手丢弃、导致重连丢历史）；缓冲溢出退化全量 Sync；会话切换只补尾窗 Sync 避免闪烁。
- **中断等待可被打断**：中断请求用 `bind_cancellation_slot`，`WireCancel` 能立即打断等待，而不是只能等超时；`interruptTimeout` 可配置、≤0 表示不限制。
- **对外集成面广**：MCP client 与 server、ACP 服务器、A2A 服务器与客户端、FFI（纯 C API + 供其他语言嵌入的运行时）——一个项目同时能“作为工具被别人用”和“调用别的东西”。
- **增量序号统一分配**：所有新产出的 `WireDelta` 都经 `Session::nextDeltaSeq()` 取号，服务端重放缓冲因此始终单调可靠。

**dsh 相比 agentxx 做得好的：**

- **协议由类型图生成**：Typert 在构建期产出 descriptor、runtime codec、声明合并与 source map，客户端按生成的 service face 调用 —— 类型与协议强一致，手写编解码的漂移风险被结构性消除。
- **双 SDK 投影同一循环且有门禁**：TypeScript 与 Python SDK 都投影 agent loop/会话生命周期，仓库约定“改了必须同步更新两个 SDK 的预期输出”。
- **物理 carrier 与逻辑 stream 分工**：Connection 管物理重连与 generation，Gateway 管逻辑流的打开/取消/替换，业务方按自己的数据语义选择恢复方式。
- **命令面与钩子面**：`ctx.commands` 让插件注册斜杠命令（不产生模型消息，可声明附件与是否记录原始输入），`hooks` 组桥接 Claude Code/Codex 线协议，把外部 agent 生态纳入同一运行时。

---

## 14. 测试与质量门禁

### 14.1 dsh

- 分层：单元（vitest，**每个注册表都有 HMR 安全测试**：对贡献该注册表的 fiber 执行 dispose 并断言清理完成）、覆盖率门禁（`packages/*/*/src` **按文件 100%**，未覆盖行通常意味着死代码应删除）、真实 API e2e（带密钥；缺密钥自动跳过）、所属位置预期输出、性能基准（Linux PR 门禁）、**快照回放**（录制会话 → 离线回放 → 比对持久化结果/UI 输出，模型 transcript 变化时重新录制）、Web 浏览器快照（Chromium/WebKit + ARIA 证据）。
- 原则：**优先真实实现而非 mock**（只 mock 昂贵或不确定的边界：LLM 适配器、网络、时钟）；**验证外部世界而非自我报告**（e2e 重新运行命令或重读文件，断言未修改文件逐字节一致）；**测试真实入口路径**（构建后的 `lib/`、真实 Loader/Profile 启动，不做仅测试用的组合）；**守卫必须被证明有效**（引入回归 → 观察变红 → 回退）。
- 运行时不变式：`packages/runtime-diagnostics` 提供按包归属的运行时不变式检查与报告（只在不变量可独立观测时才发布）。

### 14.2 agentxx

- 单一可执行 `agentxx_test`，多模块（core/plugin/client），可用模块名过滤、`--fail-fast`；模块内多为行为级测试 + 边界测试（如 `test_openai_provider.cpp` 193 KB、`test_plugins.cpp` 159 KB）。
- Debug 默认启用 ASan + UBSan + 插件框架定向探针（Release 全部关闭）；基准程序 `agentxx_benchmark` 有资源基准体系（RSS/PSS/堆/模块级分解/fd/线程 + 基线对比 JSON/MD）。

### 14.3 对比

| 维度 | dsh | agentxx |
|---|---|---|
| 测试组织 | 大量小 spec + 多套 vitest 配置 + 快照 | 少量大文件 C++ 模块 |
| 覆盖率门禁 | 按文件 100%（CI 强制） | 无覆盖率统计 |
| 离线回放 | **录制会话快照 + 回放比对** | 无（有 `AGENTXX_BENCH_*` 基准与录制式测试用例） |
| 真实入口路径 | 强制（构建产物 + Loader/Profile） | 测试直接链接库（构建产物级测试少） |
| Mock 策略 | 只 mock 边界（LLM/网络/时钟） | 多用真实实现 + 本地 HTTP 测试服务器 |
| 性能门禁 | benchmarks 目录（PR gate） | benchmark 模块（手动/发布前跑） |
| 运行时不变式 | 独立包 + 报告 | Debug 断言 + sanitizer + 插件探针 |

**源码要点（第 2 版补充，`packages/core/agent-loop/tests/cancel.spec.ts` 与 `agent/test/core/test_agent.cpp`）**

- **dsh 的测试是“真插件图 + 只 mock 适配器”**：spec 里逐个 `ctx.plugin(...)` 挂上 `LlmRuntime`/`SessionStore`/`SessionProjectionRegistry`/`SystemPrompt`/`ToolRuntime`/`AgentRegistry`/`AgentLoop`，只有 LLM 适配器是脚本化的 `MockAdapter`；因此测试跑的是**真实的注册、投影、工具流水线与调度**，mock 只替换了最外层的不确定边界。
- **断言打到持久日志上**：`agent.session.snapshotEvents()` 被用来过滤事件类型、核对“实际发生了什么”（例如“哪些 user 消息真的进入了上下文”），而不是只看内存对象或状态字段。
- **并发窗口被逐个枚举**：取消测试的注释直接写明覆盖“每一个落地窗口 + 信号重置 + `whenIdle()` 停稳 + 每个被取消轮次记录的原因”；并且会**模拟传输层对取消原因对象的副作用**（如 Node 给信号 reason 挂 `stack`），断言它**不会**进入日志。
- **agentxx 的测试直驱 BaseAgent + 记录式 IO**：测试用 `TestAgentIO` 覆写 `sendToPeer` 把产出的 `WireDelta` 收进数组（没有 transport、没有真实对端），断言“agent 到底发了什么事件”；模块内用局部计数器（`g_da_passed`/`g_da_failed`）映射断言宏，避免跨编译单元状态。
- **两侧的共同点与差异**：都倾向“尽量用真实实现、只 mock 边界”；差异是 dsh 断言**持久日志 + 事件**（因此覆盖到不变式层面），agentxx 断言**产出的事件流与内存状态**（更贴近 UI 契约，但不校验“日志可重建请求”这类不变量）。

### 14.4 可迁移到 agentxx 的设计

1. **录制会话快照回放**（最高价值）：录制一次真实会话（模型响应、工具调用、事件流），之后**离线回放**同一份输入，比对最终会话状态与关键输出。它能捕获“单元测试全绿但产品坏了”的问题，且不消耗 API 额度。实现路径：把 provider 换成“回放 provider”（读录制文件按调用顺序返回），比对 `view_message` 序列 + 工具结果 + 事件序列。
   落地：`agent/test` 新增 `replay` 模块 + provider 抽象里加一个回放实现。
2. **注册表清理测试（HMR 安全测试的等价物）**：agentxx 的插件注册表（工具、权限声明、能力、hook、事件订阅、定时器、键位）都应有“加载 → 注册 → 禁用 → 卸载 → 断言回到加载前状态”的测试。现有插件测试里已有类似用例，建议**系统性覆盖每一张表**并作为新增接口表的模板。
   落地：`agent/test/plugin/test_plugin_runtime.cpp`。
3. **覆盖率统计（至少对核心目录）**：C++ 可用 LLVM source-based coverage（GCC 用 gcov/lcov）。不必追 100%，但把“新增代码的未覆盖行”作为 review 项能显著减少死代码与漏测。
   落地：CMake 选项 + 脚本。
4. **测试真实入口路径**：至少保证一组测试通过**构建后的可执行文件**运行（`agentxx_cli` + 子进程 + PTY），覆盖“tsx 掩盖的失败”类问题（agentxx 已有 PTY 基准脚本可复用）。
   落地：`agent/test` 的 e2e 模块 + 脚本。
5. **守卫有效性证明**：对安全类改动（权限、路径规范化、插件边界）要求“引入回归 → 测试变红 → 回退”的证据写进 PR/记录，避免“测试写得很像但测不到”。

---

### 14.5 各自做得好的设计

**agentxx 相比 dsh 做得好的：**

- **插桩与性能两头都要**：Debug 默认 ASan + UBSan + 插件框架定向探针，Release 全部关闭；同一套代码既能做内存/未定义行为检查，又能用于发布性能测试。
- **资源基准体系完整且可回归**：RSS/PSS/私有脏页/匿名页/峰值/线程/fd + glibc 堆在用与碎片 + malloc_trim 可回收 + smaps 模块级分解（可执行文件/项目库/各插件/系统库/堆/匿名）+ 逻辑内存 + 分阶段增量 + CPU 用户/内核时间，输出 JSON（机器对比）与 MD（人工阅读），并支持 `--baseline` 与真实两进程/真实 TUI（FTXUI 线程含帧耗时）/PTY 子进程场景 —— dsh 的 benchmarks 更偏“耗时预算”，没有这层内存分解。
- **测试直驱组件、契约可断言**：`TestAgentIO` 覆写 `sendToPeer` 记录产出事件，断言的就是“agent 到底发了什么”，UI 契约因此可测；模块可单跑、支持 `--fail-fast`。
- **多平台/嵌入式可测**：无线程池、无 transport、无持久化的退化路径都有对应测试分支（测试即真实使用场景的一部分）。

**dsh 相比 agentxx 做得好的：**

- **覆盖率是门禁且按文件 100%**：未覆盖的行通常是“该删的死代码”，把覆盖率当成清理信号而不只是补测试指标。
- **录制会话快照回放**：录制一次真实会话就能无密钥离线回放，比对最终持久化结果与用户可见输出；改动模型可见行为必须更新快照 —— 这是 agentxx 目前最缺的回归手段。
- **断言打到持久状态与不变量**：测试读 `snapshotEvents()` 校验“实际记录了什么”，跑真插件图只 mock LLM 适配器；并且有“从日志重建请求”的不变量测试，测试同时保护了可重建性。
- **真实入口路径与真实 API**：构建产物 + Loader/Profile 启动的冒烟测试、真实 API e2e（缺密钥自动跳过）、Web 浏览器快照（含 ARIA 证据）。
- **守卫必须被证明有效**：要求“引入回归 → 观察变红 → 回退”的证据，避免写出测不到东西的测试；还有运行时不变式包与文档门禁。

---

## 15. 文档与配置体系

### 15.1 dsh

- **双语配对**：每篇文档有中英两份，`verify-translation-pairing` 记录配对；生成的图（时序图/流程图）由脚本产出，中文作为“经评审对侧”维护。
- **生成式目录**：`config-catalog`（20 万字符，列出每个配置字段与默认值）、`persistence-catalog`（每个持久事件类型，含 payload/是否 surface/声明位置）、`tool-catalog`、`module-graph`（依赖图）、`dependency-catalog.json`；由 `scripts/gen-*` 生成，`doc-sync` 门禁校验新鲜度。
- **Cordis 服务/事件目录**：从源码 JSDoc 生成 `ctx.*` 服务与事件签名（`gen-cordis-catalog.ts`），并用 `@mode` 标签交叉校验“声明与分发调用点一致”。
- **Agent Notes**：`.agents/notes/` 记录**持久决策依据**（implemented / archived / proposed），归档笔记冻结、不得当权威引用；机械/局部改动豁免。
- **文档规则**：当前状态散文、一段一行、一个事实只有一个归属地、字数预算门禁；禁用含糊术语（`contract`/`boundary`/`shape` 需先检查是否有更准确的词）。

### 15.2 agentxx

- `docs/zh-cn/design/`：手写设计文档（index/plugins/tui/benchmark 等）；`resource/history/*`：实现与修复历史记录（含逐项设计说明）；代码内注释为 markdown 风格并有明确规范（本文件所在仓库的 AGENTS.md）。
- 无生成式目录；无决策记录（ADR/Notes）制度；文档更新靠人工同步。

### 15.3 可迁移到 agentxx 的设计

1. **生成式目录**（投入产出比最高）：把以下内容从源码/结构定义生成文档，并加一条“新鲜度检查”测试：
    - **配置目录**：`AgentConfig` 每个字段的键名/类型/默认值（现在散在 `config.cpp` 与多份文档）；
    - **接口表目录**：`plugin_api.h`/`client_plugin_api.h` 中每张表的方法签名、版本、结构体字节数（可直接解析头文件生成）；
    - **Wire 消息目录**：`wire_protocol.h` 中每条消息的字段与方向（配合 §13.4 第 1 项的生成式编解码）；
    - **工具目录**：每个内置插件工具的名称/参数/权限声明（插件 `plugin.yaml` 已有部分信息）。
   落地：`agent/script/` 新增生成脚本 + `docs/zh-cn/design/generated/`。
2. **决策记录（Notes）**：为重大架构决定（如“会话为上下文唯一权威”“插件 API v1 的 8 字节对齐与出参返回码”“图定义可由插件改写”）建立 `resource/history/<topic>/` 的一页式决策记录，写明**背景 / 决定 / 备选方案 / 影响**，并规定“归档后不改”。现在这些理由分散在代码注释里，难以追溯。
3. **文档门禁**：至少加两条低成本检查：(a) 设计文档中引用的文件/符号路径必须存在；(b) 新接口表/新配置字段变更时，对应的目录文档必须同时更新（或用生成脚本保证）。
4. **术语表**：dsh 的 `glossary.zh.md` 为每个概念规定一个规范术语并给出唯一含义（如“轮次/步骤/Round”严格区分）。agentxx 的“轮 / 轮次 / 会话 / 上下文 / 图状态”在文档与注释中偶有混用，建议建立一份术语表并统一。

---

### 15.4 各自做得好的设计

**agentxx 相比 dsh 做得好的：**

- **文档贴着真实改动**：`resource/history/<topic>/` 记录每次功能实现/修复的动机与做法，能回答“当初为什么这么改”；dsh 的 Agent Notes 更正式（有归档与冻结规则），但两者的差别在于 agentxx 的记录与代码改动是同一批提交的一部分。
- **代码注释即契约**：头文件里的长注释直接说明“线程约定/生命周期/失败姿态/已知 TODO”（本文大量结论就是从这些注释得到的），阅读成本低；dsh 的契约更多在生成的目录与规范文档里。
- **插件自描述**：`plugin.yaml`（name/entry/depends/optional_depends/resources/interfaces）让二进制层面的插件自带元信息，第三方只看 yaml + 头文件就能用；dsh 侧对应的是 package.json + 生成的目录。
- **配置就近可读**：两层 yaml 的叠加规则（`model.list`/`plugin.list` 的 merge/replace/remove）在配置里就能看懂，不需要理解 profile/bundle/patch 这套组合模型。

**dsh 相比 agentxx 做得好的：**

- **生成式目录 + 新鲜度门禁**：配置目录（每个字段/默认值）、持久化事件目录、工具目录、模块依赖图、Cordis 服务与事件目录（从 JSDoc 生成并用 `@mode` 交叉校验声明与调用点）—— 文档与代码不会漂移。
- **决策记录（Agent Notes）制度**：implemented / archived / proposed 三态，归档笔记冻结且不得作为权威引用；重大决定有唯一归属地。
- **文档规则具体可执行**：当前状态散文、一段一行、一个事实一个归属地、字数预算门禁、禁用含糊术语（`contract`/`boundary`/`shape` 需要先找更准确的词）。
- **双语配对与校验**：中英文档成对维护并校验，翻译漂移能被发现。
- **README 契约**：每个包 README 必须覆盖用途、配置、扩展点、模型体验（并列出允许省略的清单），把“文档要求”变成可检查的门禁。

---

## 16. 迁移建议汇总

按“收益 / 成本”排序，P0 = 收益高、成本可控；P1 = 收益明确但需要一定改造；P2 = 长期演进方向。

### P0

| # | 建议 | 主要落地位置 | 收益 |
|---|---|---|---|
| 1 | 输入分通道：`next-turn` / `next-step`（steering），并补 `inject`（插件上下文） | `wire_protocol.h`、`session_server_agent_io.cpp`、`base_agent.cpp`、`modelcall.cpp` | 用户可在 agent 工作途中补充约束；插件可投放上下文而不打断 |
| 2 | 上下文变更带来源/原因（`source`）并用于 UI 分类展示 | `nodes/session_context.h`、`conversation_types.h`、`event_stream.cpp` | 可解释、可审计、可清理的上下文 |
| 3 | 录制会话快照回放测试 | `agent/test` 新增 replay 模块 + provider 回放实现 | 不花额度即可回归“真实模型轨迹” |
| 4 | 工具返回结构化值 + 渲染分离（`{value, content}`） | `tools/tool.h`、`nodes/toolcall.cpp`、客户端渲染器 | UI 卡片、插件互调、压缩都有结构化输入 |
| 5 | 会话数据格式版本 + 迁移链 | `session_store.cpp` | 结构演进有路径，不再依赖隐式兼容 |
| 6 | 生成式目录（配置/接口表/Wire/工具） | `agent/script/` + `docs/zh-cn/design/generated/` | 文档与代码不漂移；新人上手成本大降 |

### P1

| # | 建议 | 主要落地位置 | 收益 |
|---|---|---|---|
| 7 | 后台任务（jobs）：长命令不阻塞轮次，可读取/停止 | 新 `job_manager`、`agentxx_execute_command`、wire | 长构建/长测试场景可用性 |
| 8 | 轮次闭合记录 + 崩溃配平（含未执行工具调用的占位结果） | `session_store.cpp`、`base_agent.cpp`、`toolcall.cpp` | 恢复一致性；下一轮上下文自洽 |
| 9 | 工具结果后追加“上下文”（`deferContext` 等价物）与工具 `meta` | `nodes/toolcall.cpp`、`view message` 字段 | 工具自解释、压缩更友好 |
| 10 | 单调守卫（只拒绝不放行）+ 未声明权限的显式策略 | `toolcall.cpp` 钩子顺序、`permission.h` | 插件无法绕过安全策略 |
| 11 | 子代理能力声明 + 可继续子代理（至少支持追问） | `tools/subagent.h`、`AgentHost` | 多代理协作能力 |
| 12 | 会话全文检索（FTS） | `session_store.cpp`、TUI 会话弹窗 | 历史可查 |
| 13 | 提示词段落排序号 + 动态上下文快照化（KV Cache 友好） | `prompt.h`、`modelcall.cpp` | 长会话成本与稳定性 |
| 14 | 客户端模型层（不依赖 UI 的状态镜像 + 恢复语义分类） | `agent/client` TUI 框架 | 重连/分页/乐观更新可测 |
| 15 | 插件可注册命令（不走模型） | 新接口表 + wire + TUI 输入栏 | 省 token、交互更可靠 |
| 16 | 注册表清理测试（每张表）+ 守卫有效性证明 | `agent/test/plugin/*` | 卸载/热禁用不出残留 |

### P2

| # | 建议 | 主要落地位置 | 收益 |
|---|---|---|---|
| 17 | 执行世界/沙箱后端抽象（本地 / worktree / 远端共享一套路径与权限） | `permission.h`、`execute_command` 插件、`context.h` | 远程与容器化模式的统一基础 |
| 18 | 附件校验元数据 + 准入上限 | `conversation_types.h`、服务端接收路径 | 防止上下文/内存被大图打爆 |
| 19 | 压缩拆成“剪枝 → 度量 → 摘要”并对每步记账、可恢复标记 | `summarization.h/.cpp` | 省一次模型调用；崩溃可检测 |
| 20 | Wire 编解码从结构体生成 + 协议版本/能力位 | `wire_protocol.h/.cpp` | 协议演进安全、少手写错误 |
| 21 | 工作流的轻量替代：批量委派工具（模型侧编排） | `SubAgentManagerTool` | 无需脚本引擎即可编排多个子代理 |
| 22 | 决策记录（Notes）与术语表 | `resource/history/`、`docs/zh-cn/` | 设计理由可追溯、术语统一 |
| 23 | 覆盖率统计（核心目录）+ 构建产物级 e2e（PTY 驱动 CLI） | `agent/test`、脚本 | 减少死代码与“只在源码路径下成立”的假绿 |

### 16.1 源码精读后新增的建议（第 2 版）

这几项是读源码时才暴露出来的，优先级单独标注。

| 优先级 | 建议 | 依据（源码） | 落地位置 |
|---|---|---|---|
| **P0（安全）** | 权限判定解析符号链接：对已存在路径做 `weakly_canonical` 后再判定一次，或禁止跨出允许范围的软链 | `permission.cpp` 的 TODO：允许目录内的软链可被跟随进入被拒目录，逐路径过滤接口也看不到链接目标 | `PermissionMiddlewareHandle::decideTarget` / `decidePaths`，并评估对 Windows 语义与性能的影响 |
| **P1（数据安全）** | 会话目录的内核级写租约（POSIX `flock` / Windows 命名内核对象），持有期 = 写句柄生命周期，**不设超时** | 外部参照：dsh `lease.ts`；agentxx 现状只有进程内互斥锁 | `SessionStore` 写入路径 |
| **P2（成本）** | 压缩（与重试）请求复用已缓存前缀：把“系统提示 + 工具 schema + 待压缩区间的消息”作为前缀，压缩指令追加在末尾 | dsh `region.ts` 的 `rebuildCacheablePrefix` 思路；agentxx 目前只做到“同 thread/同模型” | `summarization.cpp` 的 `doSummarizeWithLLM` |
| **P2（吞吐）** | 工具执行体真正并行：把“权限/审批/前置钩子”保持有序、执行体并行，结果按模型顺序回填 | dsh 的四段流水线（prepare/dispatch/finalize/finish）；agentxx `toolcall.cpp` 的 `// TODO: 真正并行` | `ToolcallWrapNode::baseRun` |
| **P2（可维护）** | 修正过期注释与隐式约定：如 `messagesVersion` 的“回滚依赖”说法、`xx_autoRoute` 与图条件的关系 | 源码核对发现（见附录 C） | `context.h` 等头文件注释 |

> 插件系统本身还有一组建议（patch 语义、配置分级、失效写回、教学式错误、信任声明等），集中在 [§18.10](#1810-由插件系统对比得出的迁移建议)。

---

## 17. 反向清单：agentxx 不必照搬的设计

对比的另一半是“确认哪些差异是本项目的正当取舍”，避免为了对齐而引入不必要的复杂度。

1. **事件溯源会话日志**：dsh 的“仅追加事件 + 派生历史”非常强，但它把“读历史”从一次数组下标变成了一次投影计算，并且要求所有消费方理解 surface/替换规则。agentxx 的“会话为唯一权威 + 影子元信息通道”在 C++ 下更直接、内存与写入开销更低；建议只吸收“替换记录/来源元数据/失败留痕”这些**局部**优点，不整体改造。
2. **单线程事件循环 + 同语言插件**：dsh 的插件可以即时定义、热挂载，代价是全部同进程同语言、无隔离。agentxx 的 C ABI + 符号隐藏 + 多实例契约换来跨语言与稳定性，应当坚持；运行期自修改应以“热重载/热禁用 + 重新编译”为边界，而不是追求即时定义。
3. **按文件 100% 覆盖率门禁**：对 C++ 与现有规模的测试体系不现实，建议只在核心目录统计并把未覆盖行作为 review 依据。
4. **把 UI 文案完全语言包化**：TUI 文案已集中管理即可，不必引入构建期门禁的复杂度（可先用测试约束“新增硬编码文案”）。
5. **无预算的轮次策略**：dsh 明确“没有内置轮次预算”，把限制留给策略插件。agentxx 也应保持类似边界（可提供策略钩子，但不要在循环里硬编码上限）。
6. **把一切都做成 seam（三件套）**：dsh 对每个能力都要求“定义/提供方/消费方”三角色，这在其生态下成立；agentxx 的能力更多以“插件 + 接口表”表达，过度拆分会让头文件与构建负担上升。建议只对**确实需要独立演进**的能力（执行世界、压缩、检索）引入 seam 式拆分。

---

## 18. 插件系统深入对比（第 2 版增补）

前 17 章把插件系统当作“扩展模型”的一个维度；本章单独把它拆开，因为**这是两个项目差异最大、也最难事后改造的一层**。两侧都读了内核实现（Cordis 内核 4 个文件 + Loader 3 个文件 + 组合/自修改/预设共 8 个文件；pluginxx 内核 ABI/manifest/lifecycle/capability/runtime/instance_base 7 个文件 + agentxx 宿主 3 个文件）。

### 18.1 dsh 插件栈的四层

| 层 | 组件 | 职责 |
|---|---|---|
| L1 内核 | `vendor/cordis`（context/registry/fiber/reflect/service/events）+ `vendor/schemastery` | 服务容器、依赖注入、fiber 生命周期、effect、事件分发、配置校验 |
| L2 装载 | `vendor/loader` + `vendor/include`（`!!js`）+ `vendor/group` + `vendor/hmr` | 读取配置行 → 装载插件；配置热更新与写回；模块热替换 |
| L3 组合 | `packages/boot/app-boot`（profile/bundle/patch）+ `packages/boot/plugin-manager` + `packages/bundle/*` | 把多个 bundle 的 patch 按序叠加成“运行中的插件树”；安装/卸载树外插件 |
| L4 运行时自修改 | `packages/extensions/cordis-host-runner`（+ `packages/preset` 的按 agent 组合） | agent 自己定义带版本的 Cordis 包（沙箱 + 注册边界 + 人类批准）；按会话组装能力集 |

### 18.2 Cordis 内核：五个机制决定了它的全部能力与代价

**① 服务是“按隔离标签存放的实现记录”，不是模块导出。**
`ReflectService.store` 是 `symbol(隔离标签) → Impl{name, fiber, value, check}`；`ctx[isolate][name]` 决定用哪个标签。读属性走 ctx **Proxy 的 get trap**：沿 fiber 链向上找 `store[name]`，途中一旦发现“该名字在 `inject` 里但当前 fiber 不可用”就抛“inactive context”错误。`ctx.isolate(name, label)` 只改隔离映射（原型链上的影子），因此**同一进程里可以让不同的 agent 各自拥有一份 `ctx.fs`**，互不可见。
> 对比 agentxx：能力不是“按名字解析的实现记录”，而是**插件注册表 + 接口表查询**。agentxx 的隔离靠“注册时记 owner、卸载时按 owner 撤销”，没有“同一名字在不同作用域解析到不同实现”的能力。

**② 依赖变化 = 自动重启（epoch 机制）。**
每个 fiber 维护 `epoch`：`':' + impl.fiber.uid` 拼接它所有注入服务的当前实现；任一实现的 uid 变了 → epoch 变 → `_reload()`（先卸后载）。`provide()` 的 disposer 会**先通知依赖者、等它们 settlement，再撤自己的存储项**（`Promise.allSettled(fibers.map(f => f.await()))`）。这带来一个 agentxx 完全没有的性质：**替换一个服务实现，所有依赖它的插件自动重启到新实现上**，不需要重启进程，也不需要手工编排顺序。
代价是“重启”语义很重：插件必须假设自己的 `apply` 会被反复调用（所有资源都要登记成 effect），否则会重复注册。

**③ 注册必须登记成 effect，否则“可逆性”无从保证。**
`ctx.provide` / `ctx.on` / `ctx.mixin` / `ctx.accessor` 内部全是 `ctx.fiber.effect(...)`；effect 支持**生成器**（`yield disposer` 边启动边登记）、逆序执行、异步清理；`effectInertia` 让外层 effect 能 join 一个别人已开始的清理（结构性 owner 可以等子级停稳）。注册发生在 `UNLOADING` 状态会抛 `INACTIVE_EFFECT`。
> 对比 agentxx：每个 `register_*` 都是裸函数，靠调用方在 `stop` 里成对调用 `unregister_*`；宿主在 `detachAll` 里兜底撤销（取消操作、撤订阅、撤能力、撤领域注册），但**没有语言级强制**。agentxx 的做法更像是“宿主保守清扫”，dsh 是“框架强制登记”。

**④ 事件分发是五种语义明确的模式，waterfall 只是其中一种。**
`emit`（不 await）/`parallel`/`serial`（按序 await，首个 bail 停）/`bail`（同步短路）/`waterfall`（每个监听器包住其余链路，不调 `next()` 即否决下游）。模式是事件契约的一部分（JSDoc `@mode`），并用生成目录交叉校验声明与调用点。
> 对比 agentxx：`EventBus` 是请求-响应式（`getRR`/`request`，主要是 RPC 用途），钩子是固定 7 个点的 C 回调。**没有“包装器”语法**，因此“在别人返回前改写返回”这类策略必须由宿主提供专用钩子（如 `onToolcallEnd` 能改 `NodeOutput`）。这是 agentxx 里“必须改核心才能加策略”的根因。

**⑤ 服务可以声明“可用性谓词”与“拦截配置”。**
`ctx.provide(name, value, check)` 的 `check` 让“已提供但未就绪”成为一态（依赖者保持 PENDING 而不是报错）；`ctx.intercept(name, config)` / `Service::resolveConfig` 让下游插件给“被依赖服务的配置”叠加参数（按祖先顺序合并，`Config.merge` 可定制）。`Loader` 自己就用它实现 `inject: {loader: {await: true}}`：**loader 还有待处理任务时，依赖它的插件保持 PENDING**——启动顺序因此由数据（是否有未完成任务）决定，而不是靠人工编排。

### 18.3 Loader 层：配置行就是插件实例的持久身份

- **`Entry` 是配置里的一行**（`{id, name, config, group?, disabled?, inject?}`），挂在 `EntryTree`/`EntryGroup` 上，支持嵌套 id（`:` 分隔）与子树（`include` 用 `subtree` 承载另一个树）。
- **`disabled` 可以是 `!!js` 表达式**，在装载期用 loader 上下文求值（`!ctx.get('profileContext')` 这类），原始节点保留以便写回 YAML。这就是“同一份 patch 在不同启动方式下启用不同行”的机制。
- **热配置分两级**：只有 volatile 字段变化时**不重启**（`equalExceptVolatile` + `updateVolatile` + `loader/volatile-update` 只派发给该 fiber）；普通字段变化才走 `internal/update` → 重启，并**把新配置写回 YAML**（`Config.simplify` 反序列化）。
- **卸载会写回配置**：`internal/plugin` 监听 fiber 被 dispose，过滤出“由 loader 行为导致”（依赖缺失、配置更新、祖先 group 被禁用）的情形，把 `entry.options.disabled = true` 并 `tree.write()` —— 也就是**运行时发生的失效被持久化**，用户下次看到的是“这行被禁用了”而不是“它为什么没起”。
- **入口导入失败只记日志**：`_init()` 里 import 失败不抛给调用方（错误已落在日志），`fiber.await()` 才把启动/校验错误抛给等待者；`Loader[Service.check]` 让“树里还有任务”成为“依赖 loader 的插件不许启动”的条件。
- **`unwrapExports` 处理 ESM/CJS/default 互操作**（esbuild 的 `__esModule` 坑），这是同语言生态特有的适配层——C ABI 的 agentxx 没有这个问题，但**有符号名与平台扩展名的适配**（`resolvePluginEntryPath`）。

### 18.4 组合层：bundle / profile / patch 的语义比看起来更严格

读 `packages/bundle/base/cordis.patch.yml` 与 `packages/boot/app-boot` 后，可以提炼出四条硬规则：

1. **patch 是“按 id 替换整段 config”，不是深合并**（文件头明确写了 “replaces the targeted row's whole `config` rather than merging into it”）。因此**不同模式（headless/web/sdk）用到的行必须各自在自己的 bundle 里完整重述配置**，基础 bundle 只放共享行与中性默认值。
2. **行顺序不携带装载语义**（`Row order carries no load semantics; activation is service-availability driven`）。文件顺序只是给人看的——真正的顺序由服务可用性与 `inject` 决定。这与 agentxx 的 `plugin.list` 需要**拓扑排序**（`topoSortPlugins`）形成鲜明对照：agentxx 必须自己算依赖顺序，dsh 交给运行时。
3. **`disabled` 是配置的一部分**，用 `!!js` 表达“只有 launcher 拥有 profile 上下文时才启用”（plugin-manager / hmr / settings / config-editor 都是这样被排除在源码直跑之外的）。
4. **bundle 是真的安装单元**：`package.json` 里 `dsh.bundle.patch` 指向 patch 文件，`dependencies` 列全它引用的插件包，因此“装一个 bundle”= 装齐它的行所需的包。

此外，profile 是**用户级具名组装**（`$DSH_HOME/profiles/<name>/`），有 `cordis.patch.yml` 与树外插件目录；`dsh --profile web --dump-config` 能打印最终树——这三件事（具名组装、用户 patch、可打印最终态）在 agentxx 里对应的是“两层 yaml + 待补的 dump 能力”。

### 18.5 按 agent 组合：preset 的隔离 realm 与泄漏检测

`packages/preset/agent-preset-registry` 是 dsh 里最接近“每个 agent 一套插件”的机制，实现上有三个值得抄的点：

- **mount 是一个带审计的事务**：`mountPreset()` 建树 → 逐行 `auditRows()`（未启动/导入失败/激活失败 → 记为 failed；依赖缺失 → 记为 pending，允许稍后随宿主 provider 就绪自动激活）→ **failed 非空则整个 mount 失败**，且 **root realm 的服务泄漏也导致失败**。
- **泄漏检测**：`leakedServices()` 遍历 reflect store，检查“子树内的实现是否落在 root 的隔离符号上”。也就是说，**preset 里的行必须用 `isolate` realm 发布服务**，否则会污染宿主全局——这条规则有检测器，不是靠约定。
- **standing key 与 agent 的父子作用域**：agent 的 scope key **父挂**在 preset 的 standing key 上，因此“我属于哪个 preset”可以只靠父指针匹配出来（mount 不在 agent 子树里）；`serviceForAgent()` 则反过来在 mount 子树里找该 agent 的服务实例。

这套设计回答了一个 agentxx 也要回答的问题：**当同一份能力被多个 agent 各自持有时，如何保证它们互不干扰、且能被审计出来**。agentxx 的答案是“按 owner 撤销 + 多实例契约（禁全局态/状态放实例堆）”，dsh 的答案是“隔离 realm + 泄漏检测 + 按子树归属查询”。前者更省资源，后者更难写错。

### 18.6 dsh 的运行时自修改：沙箱 + 注册边界 + 显式信任声明

`cordis-host-runner` 让模型写插件并运行，边界做得很克制：

- **沙箱**是 `node:vm` 新 realm：注入带前缀的直写 console、`harness.defineTool/registerTool/handle`、`btoa/atob`（宿主闭包，不用 `Buffer`）、`TextEncoder/Decoder`；把 `require`/`setTimeout`/`setInterval`/`fetch` 等替换成**教学式陷阱**（错误信息里直接写该用什么 cordis 服务），数据型全局（`process`）保持 `undefined` 以免 `typeof process` 探测即炸。
- **明确写明“这不是容器”**：模块注释直接说 "not containment: host-realm helper functions remain an escape route" —— 信任姿态是“合作方”，不是“对抗方”。这种**把安全边界写在代码注释里**的做法本身值得学。
- **注册边界（guard）**是最有技术含量的一层：沙箱里的 schema/值经跨 realm JSON 克隆**重建为宿主 realm 对象**，`parameters` 规范化成宿主 `ParameterSchemaSpec`（先校验再登记，非法词表在注册期就报教学错误）；工具渲染输出做形状检查；**invalid execute 结果只让那次调用失败，不污染会话日志**；错误文本截断以免烧掉模型的 token。
- **双端与批准**：host 半边在沙箱里跑，client 半边需要**人类批准**（`approveFutureVersions` 决定后续版本是否沿用），批准记录与运行身份绑定（`pluginRunId` / `packageId`）。
- **解析期教学**：`precheckCode` 用 `new Function` 只编译不执行做定义期门禁，并用 `vm.Script` 拿“源码行 + 插入符”上下文；发现 `as` 就提示“沙箱跑纯 JS，去掉类型注解”，括号不平衡就提示“你的代码是 async 函数体、结尾不该是 `});`”。
  > 这些细节对 agentxx 的直接启示不是“也做 JS 沙箱”，而是**错误信息应当教会调用方如何修正**——对“模型驱动的插件系统”尤其如此。

### 18.7 agentxx 插件栈的四层

| 层 | 组件 | 职责 |
|---|---|---|
| L1 内核 ABI | `pluginxx/api/{abi.h,tables.h,entry.h}` | 跨边界契约：版本、对齐、字符串、唯一内存通道、COM 查询、十张通用表、统一异步原语、驱动协议、取消令牌 |
| L2 内核宿主骨架 | `pluginxx/host/*`（loader/manifest/lifecycle/host_core/tables_impl/domain_hooks/event_bus/capability_registry） | 动态库装载、清单解析与拓扑排序、create/start/stop/destroy 事务、禁用启用级联、inflight 等待、通用表实现、能力注册册 |
| L3 运行时 | `pluginxx/runtime/*`（runtime/instance_base/op_driver/manager_base/driver） | 实例状态机与执行 lease、tombstone host 控制块、Operation 驱动器、跨线程投递与重放 |
| L4 宿主领域 | `agentxx/plugin/*`（plugin_manager / plugin_interfaces / tool_registry / plugin_graph_node / api/plugin_kit）+ client 侧 | 领域表（工具/权限/钩子/会话/模型/提示词/资源/图/上下文）、接口协商、领域注册与撤销、客户端 UI 注册表 |

### 18.8 agentxx 内核：一套“最小 ABI + 显式状态机”的设计

**① 跨边界契约只有四件事。** `PluginxxStringView`（data+size 的 UTF-8 视图）、`PluginxxString`（宿主堆分配、显式所有权转移）、`alloc/free`（**跨 CRT 堆的唯一通道**）、`query_interface(host, iid)`（COM 风格按名查表）。其余一律是“表”：十张通用表（events/capabilities/scheduler/coroutine_runtime/plugins/config/cancel/json/log/tasks）+ 宿主领域表。全局版本 `PLUGINXX_API_VERSION` 只在“结构体布局”层面兜底，**能力演进走每张表自带的 `version` 字段**——这一点与 dsh「新增能力不动全局版本」的意图相同，但手段不同（表版本 vs 依赖协商）。
> 值得注意的还有 **入口符号名属于宿主命名空间**：内核不硬编码 `agentxx_plugin_agent_*`，由宿主实现 `entrySymbols()`；内置（编译进宿主）插件与动态库插件共用同一份入口描述结构（`PluginxxBuiltinInfo`），因此**同一份插件源码既能编进宿主也能编成 .so**。

**② 统一异步操作原语，语义极窄但很硬。**
`execute_start(...)` 返回 op 句柄 + `notify->done(status, payload)` **恰好回调一次**（OK/CANCELLED/FAILED）；`execute_cancel` 只请求取消；`PluginxxOperatorHandle` **只能取消、不能轮询或收尸**（生命周期归宿主）。回调可以从插件任意线程触发，宿主负责把等待协程唤醒到 io 线程。
> 对比 dsh：同语言里“Promise 已经解决了这一切”，代价是**跨语言时没有任何等价物**。agentxx 用这组原语把“跨语言异步”压缩成 4 个函数指针，是 C ABI 插件系统的必要投资。

**③ 协程驱动是显式协议，而不是共享事件循环。**
`pluginxx.coroutine_runtime` 表定义 driver/pump：插件申请一张 ticket，宿主**异步投递**（永不内联）一次有界回调；一次 ticket 至多跑一次；插件必须保证每个 driver 对应“真实存在的可运行工作”，重复唤醒要自己合并；宿主另做 ticket 去重与关闭期取消作为最后防线。驱动回调**不得阻塞、不得等待事件、不得同步调用宿主业务接口**，且必须自己捕获异常（跨 C ABI 的异常是 UB）。
> 这是“单线程 io_context + 多语言协程”的接缝设计：它把“跨语言协程互操作”降级为“有界回调推进 + 唤醒合并”，从而避免把插件的私有 reactor 接进宿主执行序列。

**④ 生命周期状态机 + 执行 lease + tombstone 指针，三件一起才安全。**
- `InstanceLifetime` 把**准入位与计数放在一次 CAS** 里（关闭与跨线程取 lease 之间没有空隙），最后一个 lease 释放时投递一次通知唤醒所有 idle 等待者；`closeRequested()` 之后拒绝新工作。
- `PluginHostControl`：**给每个实例分配一块永不释放的控制块**存放 `PluginxxHost*` 视图，关闭时只把实例引用清空（tombstone）。于是插件把 host 指针存进线程/延迟任务、卸载后再调用时，每个 vtable 入口都只会**安全失败**，既不会 use-after-free，也不会误指到后来加载的同名实例。代价是约 100 字节/实例，且注释明确承认这是“故意不回收”。
- 实例上有 `InflightGuard`（事件 handler / 命令执行 / 异步 op 入口计数），卸载要等 inflight 归零才 `dlclose`；驱动器另有登记表（可跨线程取消、完毕后 prune）。
> 这三条对 dsh 的世界观是“不需要的”：JS 里对象被闭包引用就不会被回收，也不存在 dlclose。**但只要做跨语言/可卸载的插件，这三条就是不可省的。**

**⑤ 禁用/启用是显式级联，且区分“用户意图”。**
`disableImpl(name, userInitiated)`：用户禁用置 `userDisabled`；级联禁用只置 `blockedByDependencies`（**绝不覆盖用户标记**）；先摘除宿主侧注册（取消未完成操作、撤订阅、撤能力、撤领域注册），再投递 `stop` 事务（只有“已 start 且未 stop”的实例需要）；随后**递归禁用所有直接/间接的必选依赖者**（含三级与菱形依赖）。
`enableImpl`：`!userInitiated && userDisabled` 直接返回（用户禁用的插件不被级联恢复）；**先置位再递归**（让注册复查通过且循环依赖不会无限递归）；**先启用必选依赖**（子插件 start 需要父插件能力就绪）；注册由插件自己的 `start` 事务重新声明，宿主只投递并处理失败（失败 → 回 Disabled **且不留部分注册**，回滚顺序是“先撤注册、再释放资源”）；最后级联恢复“因本插件而被禁用”的依赖者。
> 对比 Cordis 的 epoch 自动重启：**agentxx 用“显式级联 + 用户意图标记 + 重新声明注册”换取了可预期性**（不会因为依赖换实现而静默重启），代价是宿主必须自己维护依赖图与重声明逻辑。

**⑥ 能力注册册（插件互调）。**
`CapabilityRegistry` 记录 `能力名 → 提供者插件 + start/cancel 回调 + ctx`；注册要求全局唯一（重名拒绝），**只有提供者本人能注销**；调用侧经 `invoke_capability_async` 走同一条统一异步原语。能力载荷是 JSON（不携带宿主领域语义），因此注册册可以放在与领域无关的内核里，被 agentxx / musicxx 共用。
> 对比 dsh：插件互调直接依赖 `ctx.<service>` 的**接口（类型）**，注册是隐式的（`provide`）。agentxx 的“具名 JSON 能力”丢失了类型安全，但换来了**跨语言可调用**与“能力可被别的插件发现并列出”（`names()`）。

**⑦ 接口协商是三层，且明确承认“机制通用、限制集中在 client 侧”。**
1. 声明层：`plugin.yaml` 的 `interfaces.require/optional`（稳定字符串名；`agentxx.*` 是内置保留命名空间，第三方用 `<vendor>.*`，宿主不认识即视为不支持）；
2. 校验层：宿主按前缀过滤出本侧声明并与自身支持集比对——**`require` 未满足一律跳过加载并记 INFO（不是错误）**，因为“同一插件目录服务多种宿主”是预期情况；**声明了本侧接口却缺对应入口符号才明确报错**；
3. 决策层：插件在入口里用 `query_interface` 判空 / `EVT_READY` / `get_client_state` 的 `interfaces` 数组决定启用哪些功能（细粒度能力名优于声明整张表）。
并且明确划界：**`api_version`（>= 基准）不能被接口协商替代**——核心结构体是 C 结构，老宿主 + 新插件按新偏移读字段是 UB；接口协商只解决“功能子集”维度。

### 18.9 两套插件系统的正面对比

| 维度 | dsh（Cordis + Loader + bundle/patch） | agentxx（pluginxx + 宿主领域表） |
|---|---|---|
| 扩展单元 | npm 包 / 内存中的 Cordis 插件对象（同语言、同进程） | 动态库（.so/.dll/.dylib）或编译内置（同一入口结构） |
| 依赖表达 | `inject` 声明服务名；**运行时按服务可用性自动排序**；依赖实现变化触发重启 | `plugin.yaml` 的 `depends`/`optional_depends`；宿主**拓扑排序**后按序装载；依赖禁用触发**级联禁用** |
| 隔离 | 隔离 realm（`ctx.isolate`）：同名服务在不同作用域解析到不同实现；preset 有泄漏检测 | owner 归属：注册记 owner、卸载按 owner 撤销；隔离靠“实例状态在自己的堆块 + 文件系统/权限边界” |
| 装载顺序语义 | **行序无语义**（由服务可用性决定） | **行序参与语义**（拓扑排序的输入），因此顺序错会改变行为 |
| 配置组合 | bundle patch 按 id 覆盖**整段 config**，profile + 用户 patch 分层，`--dump-config` 可打印最终树 | 两层 yaml 的段级 merge/replace/remove（`list` 段） |
| 热更新 | HMR（模块热替换）+ volatile 字段“改值不重启”+ 重启写回 YAML | 禁用/启用（重新声明注册）；没有源码级热更新，需要重启进程或重新装载 |
| 卸载语义 | dispose fiber → 逆序跑 effect；`provide` 的撤下会**等待依赖者先卸载** | stop（撤销自管资源）→ 等 inflight 归零 → destroy → dlclose；**分超时与重试**，还保留未安全关闭的实例以便重试 |
| 迟到调用安全 | 不存在（同进程对象引用） | **tombstone host 控制块**：旧 host 指针只会安全失败，绝不误指新实例 |
| 异步互操作 | 语言内 Promise | 统一异步原语（exactly-once 完成通知 + 仅取消句柄 + 驱动 ticket 协议） |
| 策略/包装能力 | waterfall：任意监听器可包住下游并否决/改写 | 固定钩子点（7 个）+ 领域表回调；没有通用包装语法 |
| 错误风格 | 配置/启动抛错并带 `ValidationError` 聚合；动态插件用“教学式错误” | 失败记日志 + 返回状态码；`require` 未满足按“跳过”处理（INFO） |
| 自描述与目录 | `ctx.<service>` 生成服务/事件目录（含 `@mode`）交叉校验；bundle patch 是给人读的清单 | `plugin.yaml`（name/entry/depends/resources/interfaces）+ 各接口表 `version`/`struct_size` 自校验 |
| 多实例 | 一个 Cordis 运行时内同一插件可有多个 fiber；跨 agent 用隔离 realm | 同一动态库可在同进程多实例并存（禁全局态、状态放实例堆、接口表结果存实例上下文） |
| 跨语言 | 不支持（同语言才能进插件树） | 原生支持（C ABI，含 Rust/Go/Python 侧 FFI 的可能） |
| 客户端插件 | 浏览器 bundle 图（combo 路由 + rev + HMR）+ 双端 dynamic plugin（client 半边需人类批准） | 同一 ABI 的 client 侧入口（双端入口探测；UI 注册表 COW 快照；**UI 线程不进插件代码**） |
| 权限/安全 | 自修改插件用 vm sandbox + 注册边界 guard，并明确声明“非容器” | 接口协商 + 命名空间保留 + `api_version` 下限；插件是原生代码，**没有沙箱**（信任边界是“同进程原生扩展”） |

### 18.10 由插件系统对比得出的迁移建议

**agentxx 可吸收：**

1. **patch 语义（按 id + 整段替换）与“打印最终装配树”**：现在 agentxx 只有段级 merge；配置漂移（哪一层生效了什么）很难回答。建议给 `plugin.list` / `model.list` 之外的段也引入“按插件名/工具名定位并整段替换”，并提供 `--dump-config` 等价物（插件清单 + 每项的来源层 + 中间件顺序 + 工具名清单 + 图定义）。
2. **配置字段分级（volatile vs 需重启）**：Cordis 允许“只改 volatile 字段不重挂载”。agentxx 可给 `PluginConfig`/`AgentConfig` 标注“可热改字段”，改这些字段时只更新运行中实例的引用并广播一次事件（`loader/volatile-update` 的等价物），其余字段才走 disable/enable。
3. **失效即写回**：Loader 把“插件被依赖消失/被卸载而失效”写回配置文件（`disabled: true`）。agentxx 可把“插件装载失败/被级联禁用”写回 `plugin.list` 的覆盖段，避免用户每次启动都被同一条失败刷屏。
4. **接口协商的“跳过而非报错”原则**：agentxx 已经做到了（`require` 未满足 INFO 跳过），建议把它写进对外文档并在 `plugin.yaml` 校验里给出“你声明了 client 侧接口但宿主是 cli”这类**可解释的跳过原因**（现在只有 INFO 日志）。
5. **教学式错误**：把“插件加载失败/接口缺失/参数非法”的错误文本改成“**告诉你该怎么做**”（对应 dsh 的 sandbox 陷阱消息与 guard 的 registration 错误）。对“模型会读日志并自我修正”的场景收益最高。
6. **能力注册册的可列举性**：`CapabilityRegistry::names()` 已经能列出全部能力；建议把“插件清单 + 能力清单 + 接口支持集”作为 UI 可查看的页面（对应 dsh 的 `cordis_inspect` / `--dump-config`），这会显著降低插件开发与排障成本。
7. **显式的信任声明**：像 dsh 那样在文档/注释里写明“插件是原生代码、同进程、无沙箱，信任边界是什么”，避免使用方误以为插件环境有隔离。

**dsh 也可借鉴 agentxx 的：**

8. **tombstone 式句柄**（跨卸载的迟到调用安全失败）：对动态插件的 host 句柄、以及任何“可能被缓存的、指向已卸载对象的句柄”，永不复用地址是比“引用计数 + 祈祷”更简单的正确性方案。
9. **exactly-once 的完成通知语义**：agentxx 用一句话把跨语言异步的契约钉死（“必须恰好回调一次”），这是跨语言边界最值得抄的一条规则。
10. **快捷键冲突的可解释性**：agentxx 会记录“谁抢到了键位、谁没抢到”（`ClientKeybindConflict`，可在设置里展示）；dsh 的快捷键是 owner 声明的默认键位，缺少这类“为什么没生效”的解释记录。
11. **卸载超时与重试语义**：agentxx 的 `unloadAsync(timeout)` / `shutdownAll` 明确区分“安全关闭 / 延迟关闭（仍有 lease）/ CloseFailed 待重试”，dsh 的 teardown 只有“等待 settlement”一种姿态。

### 18.11 反向结论（两边都不该照搬对方）

- **agentxx 不要引入“依赖变化自动重启”**：C++ 里重装插件意味着重新 dlopen、重建全部线程与 `io_context` 关系，代价远高于 JS 的 `apply()` 重跑；显式禁用/启用 + 明确的重新声明路径更符合成本模型。
- **agentxx 不要追求“行序无语义”**：它的依赖排序是**静态的、可打印的**（拓扑排序输入就是配置），这比“由运行时服务可用性决定”更好排障；应该保留并强化，而不是向 dsh 靠。
- **dsh 不要引入 C ABI 式的兼容负担**：它已有一套“版本 + 声明合并 + 门禁”的演进机制；跨语言带来的对齐、字符串所有权、CRT 边界问题在同语言生态里是纯负担。
- **dsh 可借鉴 agentxx 的“内置插件与动态插件同构”**：agentxx 用同一入口结构让插件既能编进宿主又能单独编译；dsh 里 bundle 行与树外插件是两种形态，若统一描述可简化 profile 组装与打包。

### 18.12 各自做得好的设计

**agentxx 相比 dsh 做得好的：**

- **最小 ABI 做得非常克制**：跨边界只保留四件事（字符串视图、宿主堆字符串、`alloc/free` 唯一内存通道、COM 风格接口表查询），其余全部是“表”；每张表自带 `version`/`struct_size` 自校验，能力演进不动全局版本号。
- **跨卸载的指针安全有真正的解法**：`tombstone host 控制块`（永不释放、关闭只清空实例引用）+ 执行 lease + inflight 归零等待，使“插件把 host 指针存到线程/延迟任务里，卸载后再调用”只会**安全失败**，既不会 use-after-free 也不会误指到后来加载的同名实例。
- **禁用/启用语义把“用户意图”和“依赖状态”分开**：`userDisabled` 与 `blockedByDependencies` 分别记录，级联禁用绝不覆盖用户选择，级联恢复也不会把用户禁用的插件拉起来；注册由插件自己的 `start` 事务**重新声明**（宿主只投递并处理失败），回滚顺序固定为“先撤注册、再释放资源”。
- **卸载有超时与重试语义**：安全关闭 / 延迟关闭（仍有 lease）/ `CloseFailed` 待重试三态分明，`shutdownAll` 不会清空实例表从而丢掉可重试的入口。
- **内置与动态插件同构**：同一入口描述结构（`PluginxxBuiltinInfo`）让同一份源码既能编进宿主也能单独编成 `.so`。
- **接口协商“跳过而非报错”**：`require` 未满足按 INFO 跳过（同一插件目录服务多种宿主是预期情况），只有“声明了本侧接口却缺入口符号”才报错；细粒度能力名让插件只声明自己真正用到的子能力。
- **可解释性做在用户可见处**：能力注册册可列举、客户端键位冲突会记录“谁占用/谁没抢到”并在设置里展示、插件清单与资源声明在 `plugin.yaml` 里自描述。

**dsh 相比 agentxx 做得好的：**

- **依赖注入 + 服务可用性驱动激活**：声明 `inject` 即可，加载顺序由“服务是否就绪”决定而不是人工排序；`ctx.inject(deps, cb)` 还会在依赖变化时自动重跑。
- **epoch 机制（依赖实现变化即自动重启）**：替换一个服务实现，所有依赖它的插件自动先卸后载到新实现上；agentxx 需要手动 disable/enable 并依赖插件把注册写在 `start` 事务里。
- **注册可逆由框架强制**：一切贡献经 `ctx.effect()` 登记，卸载逆序执行、异步会被等待、生成器可边启动边登记，还有 `EffectMeta` 标签树；`provide` 的撤下会**等依赖者先卸载**。
- **作用域隔离是框架能力**：`ctx.isolate` 让同名服务在不同作用域解析到不同实现，preset 装载还做 **root realm 服务泄漏检测**（preset 里的行必须用 isolate realm 发布服务）。
- **组合层（patch/bundle/profile）语义严格且可打印**：patch 按 id 替换整段 config、行序不携带装载语义、`--dump-config` 打印最终树、volatile 字段改值不重挂载、插件失效会**写回配置**。
- **运行期自修改有完整安全边界**：vm 沙箱（教学式陷阱替代 Node API）+ 注册边界 guard（沙箱值重建为宿主对象后才允许注册）+ 客户端半边人类批准，并且**在代码注释里显式声明“这不是容器”**——把信任姿态讲清楚本身就是设计。
- **错误信息面向“调用方如何修正”**：动态插件解析失败会给出源码行与插入符、提示“去掉 TS 类型注解”“注意是 async 函数体、结尾不该是 `});`”。

---

## 19. 结语

两个项目在同一个问题上给出了两种答案：

- **dsh** 用“事件 + 插件树 + 显式契约”把**可解释性、可恢复性、可替换性**做到极致，代价是抽象层数多、实现与文档体量巨大、同语言同进程。
- **agentxx** 用“图引擎 + 中间件 + C ABI 插件 + 协程”把**性能、跨语言扩展、多会话并发**做到极致，代价是若干语义（轮次/步骤、历史替换、失败留痕、作用域）没有显式建模，行为只能从代码推断。

从本次通读看，**最值得迁移的不是 dsh 的某个具体实现，而是它的三条工程原则**：

1. **把运行时的关键事实显式化为可持久化、可回放的状态**（轮次原因、历史替换、失败尝试、权限决策、上下文来源）——这直接决定了排障与恢复能力；
2. **把扩展点设计成“可包装、可短路、可单调”的链条**（waterfall + guard），而不是一堆通知回调——这直接决定了策略类插件的表达能力与安全性；
3. **把文档与测试做成“可生成、可校验、可回放”的设施**（生成式目录、决策记录、快照回放、注册表清理测试）——这直接决定了项目长期的可维护性。

而 agentxx 已有的三项优势应当保留并继续强化：**图定义可被插件替换**（循环本身可扩展）、**C ABI 的稳定与隔离**（跨语言、符号隐藏、多实例）、**单线程协程下的多会话并发与低内存开销**（配合基准体系持续验证）。

第 2 版逐文件精读源码后，可以给上面的判断再加一条限定：**两侧真正的差距不在“谁的功能更多”，而在“哪些事实被显式建模”**。dsh 把轮次原因、历史替换、失败尝试、请求冻结、写所有权、审批策略都变成了可持久化、可回放、可测试的对象；agentxx 把这些多数放在“代码里的约定 + 注释”里。前者的代价是抽象与体量，后者的代价是**每次改动都要靠人记住约定**（例如“取消要补 `[User canceled]`”“模型调用前要 `repairMessages`”“工具参数要注入 sessionId”）。因此对 agentxx 最实际的路线不是照搬 dsh 的架构，而是**把已有约定逐个变成显式对象与断言**（见 §16 与附录 C.3）。

---

## 附录 A：关键文件与文档索引

列出本次通读中**支撑每个模块结论**的主要位置，便于后续复查或按图索骥。

### A.1 agentxx

| 模块 | 关键位置 |
|---|---|
| 入口/装配 | `agent/client/src/main.cpp`、`agent/client/src/config_loader.cpp`、`agent/lib/src/agent/config.cpp` |
| Agent 主体 | `agent/lib/include/agentxx/agent/base_agent.h`、`agent/lib/src/agent/base_agent.cpp`、`code_agent.cpp` |
| 图与节点 | `agent/lib/src/agent/base_agent.cpp`（`initGraphDefinition`）、`agent/lib/src/nodes/{modelcall,toolcall,agentcall,wrap_handle,session_context,graph_conditions}.cpp` |
| 中间件 | `agent/lib/include/agentxx/middlewares/middleware.h`、`agent/lib/src/middlewares/{permission,summarization,skill,memory_file,subagent_manager}.cpp` |
| 会话与持久化 | `agent/lib/include/agentxx/agent/{context.h,session_store.h,checkpoint_store.h}`、`agent/lib/src/agent/{session_store.cpp,wire_protocol.cpp}` |
| 工具 | `agent/lib/include/agentxx/tools/tool.h`、`agent/lib/src/tools/*.cpp`、`agent/plugins/agentxx_*/` |
| 插件框架 | `agent/third_party/cxx_pluginxx/{include/pluginxx/** ,src/*}`（ABI/清单/生命周期/运行时/能力册）、`agent/lib/include/agentxx/plugin/**`（领域表/协商/工具表/图节点/客户端注册表）、`agent/lib/src/plugins/*.cpp`、`agent/plugins/agentxx_*/` |
| 远程与协议 | `agent/lib/include/agentxx/agent/io/{agent_io.h,wire_protocol.h,session_server_agent_io.h,ws_io_transport.h}`、`agent/lib/src/protocol/{mcp_client,mcp_server,acp_server,a2a_server}.cpp` |
| 客户端 | `agent/client/src/io/tui/agent_tui.cpp`、`agent/client/include/agentxx-client/io/tui/`、`agent/lib/include/agentxx/ui/item.h` |
| 测试与基准 | `agent/test/test.cpp` + `agent/test/{core,plugin,client}/`、`agent/benchmark/` |

### A.2 dsh

| 模块 | 关键位置 |
|---|---|
| 架构总览 | `docs/architecture.zh.md`、`docs/agent-lifecycle.zh.md`、`docs/glossary.zh.md` |
| 插件框架 | `vendor/cordis/src/*`（context/registry/fiber/reflect/service/events）、`vendor/loader/src/*`（entry 树/配置写回/volatile）、`vendor/hmr`、`packages/boot/{app-boot,plugin-manager}`、`packages/bundle/*`（patch 组合）、`packages/preset/*`（按 agent 隔离 realm）、`packages/extensions/cordis-host-runner`（自修改）、`docs/cordis-primer.zh.md`、`docs/cordis-api/` |
| 核心主干 | `packages/core/{session,system-prompt,tools,agent,agent-loop,scope}/`、`docs/subsystems/core.zh.md` |
| Agent loop | `packages/core/agent-loop/src/{agent.ts,index.ts,inbox.ts,tool-calls.ts,invariant.ts}` + `tests/`、`packages/core/agent-loop/README.zh.md` |
| 会话/持久化/投影 | `docs/subsystems/{session,persistence,session-projection}.zh.md`、`packages/session/*` |
| 工具流水线 | `docs/{tool-execution-pipeline,tool-catalog}.zh.md`、`docs/subsystems/tools.zh.md` |
| LLM | `docs/subsystems/llm-streaming.zh.md`、`packages/llm/*` |
| 压缩 | `docs/subsystems/compaction.zh.md`、`packages/compaction/*` |
| 权限/沙箱 | `docs/subsystems/{approval,permission-presets,sandbox}.zh.md`、`packages/sandbox/*` |
| 子代理/作业/工作流 | `docs/subsystems/{subagent,jobs,workflow,goal,plan,schedule}.zh.md` |
| 检索/附件/spill | `docs/subsystems/{session-query,attachment,spill}.zh.md` |
| 客户端 | `docs/subsystems/{web-client,client-modules,slots,conversation}.zh.md`、`packages/client/*`、`packages/api/*` |
| 协议与集成 | `docs/{api-gateway,web-server}.zh.md`、`docs/subsystems/{mcp,skills,commands}.zh.md`、`packages/{sdk,acp,mcp,hooks}/*` |
| 测试 | `docs/testing.zh.md`、`packages/*/*/tests/`、`snapshots/`、`benchmarks/` |
| 生成式文档 | `docs/{config-catalog,persistence-catalog,module-graph,dependency-catalog.json}`、`scripts/gen-*.ts` |

---

## 附录 B：术语对照

| 概念 | agentxx | dsh | 备注 |
|---|---|---|---|
| 一次用户输入到模型停止 | 一轮（`runTurnAsync`） | 轮次 turn（可含多个步骤） | dsh 把“轮次/步骤”分开，agentxx 只有“轮” |
| 一次模型请求 + 工具 | 图里的 `llm` → `tools` 一次迭代（super-step） | 步骤 step | agentxx 的步骤边界不对外可见 |
| 循环本体 | 执行图定义（可被插件改写） | `agent-loop` 包（唯一实现，可替换） | 两者都可替换，粒度不同 |
| 扩展点 | 中间件钩子（7 点）+ 接口表 + 图节点 | 服务 + 类型化事件（5 种分发模式） | dsh 的 waterfall 支持包装与短路 |
| 注册撤销 | `unregister_*` / 卸载钩子 | `ctx.effect()` 返回 disposer（强制） | |
| 会话上下文真源 | `Session::messages`（typed） | `SessionEvent` 日志（消息是投影） | agentxx 直给，dsh 可回放 |
| 会话持久化 | `session.db`（SQLite，4 表） | `session.vN.jsonl[.zstd]` + `SessionHandle` | |
| 上下文压缩 | summarization 中间件 | compaction seam + 事件 | |
| 中断（HIL） | `NodeInterrupt` + `InterruptUi` 表单 | `ctx.approval` / `user-questions` seam | |
| 子代理 | subagent 委派（中断派生独立 agent） | subagent seam（多提供方 + 可继续 activation） | |
| 大输出外置 | share_store（id + LRU 缓存） | spill store（不透明定位符 + 检索提示） | |
| 客户端 | TUI / stdio / 远程客户端 | Web / Electron / headless | |
| 插件 ABI | 纯 C ABI + 接口表按名查询 | 同语言 npm 包 + Cordis 上下文 | |
| 会话工作目录 | 会话级 `workDir`（可被 worktree 切换） | `SessionHeader.cwd` + `agentPreset` | |

---

## 附录 C：本次精读的源码清单与关键发现

按模块列出实际读过的实现文件，以及“文档没有写、但影响对比结论”的发现。

### C.1 dsh（TypeScript）

| 文件 | 关键发现 |
|---|---|
| `packages/core/agent-loop/src/agent.ts` | 相位数是 `idle/maintenance/running`；唤醒闩锁与 `wakingAfterAbort` 重分类；`whenIdle` 跟随替换性工作；`turnEnds` 的 max-tokens 粘性；`agent/request-error` → `{kind:'retry'}` 在同一内循环重试；请求对象一次性深度冻结（`WeakSet` 记录已冻结消息） |
| `packages/core/agent-loop/src/index.ts` | 创建事务的“先注册中止源、后建资源”顺序；记忆化反向拆除与失败聚合；`resume` 先取写所有权 → 冷读 → `interruptedTurnClosers` → `appendUnstoredSuffix`；只有 `SessionPersistenceNotFoundError` 才回退“首次创建”；`turnBoundary`/`inbox` 两个投影单元；`provider/model/cwd` 三个提示词变量 |
| `packages/core/agent-loop/src/tool-calls.ts` | `commitReady()` 只提交连续模型顺序槽位；`fillPool()` 每次启动前重读执行模式；中止时为未启动调用补 `tool/call` + 错误结果（`sourceEventSeqs: [callSeq]`）；调度器故障只排空已启动调用、不伪造结果 |
| `packages/core/session/src/index.ts`、`surface.ts` | `SurfaceManager` 增量折叠 + 派生消息缓存（保持对象身份）；transcript 用 append-origin 事件、替换副本只对模型可见；节点 0 的 `system/message` 受保护；工具结果替换只能改自身内容；读日志要求提供全部已提交的消息投影；写入校验 JSON 合法性 |
| `packages/core/tools/src/index.ts` | 四段流水线（prepare/dispatch/finalize/finish）让策略有序而执行体重叠；回调在参数物化前捕获（防 getter 换实现）；PTC 折叠在策略之前拒绝；`fuseToolSignals` 保证包装层无法取消掉取消；`bodyInvoked` 决定 `ABORTED`/`ABORTED_BEFORE_DISPATCH`；规范值只活在执行期（`WeakMap` 标记） |
| `packages/core/system-prompt/src/index.ts` | 段落/顺序由中央分配（`getSectionOrder`/`getContextOrder`）；`suppressRuntimeContext()` 可关闭动态上下文；作用域遮蔽同名全局项 |
| `packages/llm/llm/src/{assembler.ts,retry-policy.ts}` | `BlockAssembler` 是唯一流式组装算法（容忍 delta-only、忽略已关闭 index 的 delta）；重试策略是 provider 注册时解析的不可变数据（两种模式 + 失败码白名单 + 有界指数退避 + 抖动），由独立插件在失败扩展点执行 |
| `packages/session/session-persistence-jsonl/src/lease.ts` | 写所有权是内核级锁（POSIX `flock`、Windows 命名信号量）；**故意无过期**；inode 校验；锁文件永不删除；未物化会话无文件 |
| `packages/compaction/compaction-basic/src/{index.ts,region.ts}` | 锁与 `compaction/start` 同步相邻；`assertNoActiveCompaction` 在异步决策后复查；替换校验只要求“同一段仍是同一段”；**摘要请求重建可缓存前缀**复用 KV Cache；`maxOverflowRetries` 逐 agent 计数、`replaceGeneration` 前进才重试 |
| `packages/interaction/user-approval/src/index.ts` | 策略以运行时上下文段落 `approval:policy` 贡献（不改系统提示缓存）；策略写入是纯日志事件；非法策略值在写入点抛错 |
| `packages/jobs/jobs/src/index.ts` | 访问边界是 owner 会话而非 id 保密；结算 first-wins 且广播 `awaited`；结算记录保留至 owner 销毁；输出环按上限丢最旧；owner 销毁取消并按 `teardown` 结算 |
| `vendor/cordis/src/{fiber.ts,events.ts}` | effect 支持生成器（边启动边登记清理）、逆序执行、标签树可诊断；非活跃上下文注册直接抛错；五种分发模式与 waterfall 的“否决下游”语义 |
| `packages/core/agent-loop/tests/cancel.spec.ts` | 挂真插件图（会话/投影/提示词/工具/agent/循环）+ 只 mock LLM 适配器；断言读持久日志（`snapshotEvents()`）；逐个枚举取消落地窗口，并验证“传输层给 signal 挂的属性不会进日志” |
| `vendor/cordis/src/{context.ts,registry.ts,service.ts,reflect.ts,fiber.ts}` | Context 是 Proxy + `ReflectService`；服务按**隔离标签**存放 `Impl{name,fiber,value,check}`；fiber 用 **epoch（注入实现 uid 拼接）** 驱动“依赖变化即重启”；`provide` 撤下时**先通知依赖者并等其停稳**；effect 支持生成器/逆序/再入安全（`effectInertia`）；五种分发模式与 waterfall 的“否决下游” |
| `vendor/loader/src/{index.ts,config/entry.ts,config/tree.ts}` | `Entry` = 配置行（id/name/config/group/disabled/inject）挂在 entry 树上（嵌套 id、include 子树）；`disabled` 支持 `!!js` 表达式；**volatile 字段变化不重启**（`volatile-update` 只派发给该 fiber）；普通变化走 `internal/update` 并**写回 YAML**；插件自卸载/失效会写回 `disabled: true`；**行序无语义**（激活由服务可用性决定） |
| `packages/bundle/base/{cordis.patch.yml,package.json}` | patch = **按 id 替换整段 config**（非深合并），因此模式差异必须由各 mode bundle 完整重述；`dsh.bundle.patch` 声明 bundle 的 patch 文件，`dependencies` 列全引用包；`disabled: !!js "!ctx.get('profileContext')"` 用来把 profile 专属行排除在源码直跑之外 |
| `packages/preset/agent-preset-registry/src/{mount.ts,index.ts}` | preset = **隔离 realm 下按 agent 组装的插件子树**；mount 是带审计的事务（failed 拒挂、pending 允许随宿主 provider 就绪自动激活）；**root realm 服务泄漏检测**（`leakedServices`）强制 preset 行必须 `isolate`；agent 的 scope key **父挂**在 preset 的 standing key 上，`serviceForAgent` 反查该 agent 的服务实例 |
| `packages/extensions/cordis-host-runner/src/{sandbox.ts,guard.ts,index.ts}` | host 半边在 `node:vm` 新 realm 执行：直写 console、`harness.*`、编码原语，Node API 换成**教学式陷阱**；模块注释**显式声明“不是容器”**；注册边界把沙箱 schema/值经跨 realm 克隆重建为宿主对象（非法词表在注册期报错）；invalid execute 只让该次调用失败；client 半边需人类批准并与 `pluginRunId` 绑定 |

### C.2 agentxx（C++）

| 文件 | 关键发现 |
|---|---|
| `agent/lib/src/agent/base_agent.cpp` | 轮首 `flushPendingCleanup` + `onTurnBegin`；`RunConfig` 的 `max_steps=1<<30` / 去掉 `VALUES` / `resume_if_exists=false`（含理由注释）；中断恢复从 `xx_savedGraphData` 重建并跳过首跑；`AgentRunner` 与子代理共用；`catchErrorAsync` 三处理器区分取消与异常；轮末清 graphData + 插统计 Tip 后落盘；附件服务端加载与线程池卸载 |
| `agent/lib/src/nodes/modelcall.cpp` | `repairMessages` 四类修复（补 user 尾、清悬挂 tool_calls 与孤儿结果、修重复 tool_call id、合并连续 user）；重试退避与“部分输出保留 + 计数不重置”；重试耗尽时补兜底 assistant 消息避免误路由回 tools；系统消息**就地替换** |
| `agent/lib/src/nodes/toolcall.cpp` | 权限检查经总线（无服务则放行）→ 重复调用询问 → 执行；注入 `sessionId`/`tool_call_id`；`toolcallsCache` 复用；**顺序执行**（`// TODO: 真正并行`）；取消补 `[User canceled]` 占位；中断缓存结果并保存上下文 |
| `agent/lib/src/middlewares/summarization.cpp` | 75% 触发 + API usage 优先；冷却规则与硬截断兜底；提示消息 id 复用（resume 后不重复追加）；压缩子代理同 thread/同模型/无工具/禁二次压缩；确定性阶段先于 LLM 阶段 |
| `agent/lib/src/middlewares/permission.cpp` | 固定判定顺序（worktree 隔离 → 配置拒绝 → 完全授权 → 最长前缀 → 兜底）；空/无法规范化目标按未批准；**符号链接未解析的绕过面（TODO）**；询问无超时、无总线即拒绝 |
| `agent/lib/src/agent/session_store.cpp`、`context.cpp` | 四张表的最小 DDL 与 `msg_id` 就地升级；语句预编译；`markMessagesChanged()` 统一版本号与脏标记；节流落盘 + 轮末权威写；展示消息批量提交与“过期下标丢弃”；链式哈希 + msgId 索引 |
| `agent/lib/src/event/event_stream.cpp` | `emitDelta` 统一取 delta seq（重放缓冲依赖）；EventBridge 把 assistant 消息展开为 Think/Assistant/Tool 展示消息并做思考时长兜底；未知事件重置流式段 |
| `agent/lib/src/plugins/plugin_manager_vtable.cpp` | vtable 入口的 host 校验 + 关闭期只读放行；写类入口投递到 IO 线程并持 lease；接口表 `version`/`struct_size` 自校验；可累加贡献的“基础值 ⊕ sequence 顺序存活贡献”合成规则 |
| `agent/lib/include/agentxx/nodes/wrap_handle.h` | `startedIdxs` 逆序回放保证 start/end 对称；控制流异常永远重抛、普通异常可被节点拦截；每个 handle 有独立错误回调 |
| `agent/lib/src/agent/agent_host.cpp` | 委派经总线的请求-响应入口；`sessionDepth_` + `maxDepth` 深度预算；工具集合与 worktree 绑定继承自根 agent；父取消不再派生；结束后清理深度条目；resume key 规则双端共用 |
| `agent/test/core/test_agent.cpp` | 直驱 `BaseAgent`（无 transport），用 `TestAgentIO` 覆写 `sendToPeer` 记录产出的 `WireDelta` 做断言；模块内计数器映射断言宏；权限路径期望值与中间件同一口径（含 Windows 大小写说明） |
| `cxx_pluginxx/include/pluginxx/api/{abi.h,tables.h}` | 最小 ABI：字符串视图/宿主堆字符串、`alloc/free` 是**跨 CRT 堆的唯一通道**、COM 风格 `query_interface`；统一异步原语（`notify->done` 恰好一次 + 仅取消句柄 + 任意线程回调由宿主唤醒 io 协程）；十张通用表；**driver ticket 协议**（永不内联、一次一有界步骤、唤醒需自行合并） |
| `cxx_pluginxx/include/pluginxx/host/{manifest.h,capability_registry.h}` | `plugin.yaml`（name/entry/depends/optional_depends/resources/interfaces）+ 名称推导（`libfoo.so`→`foo`）+ 入口路径平台化与多配置子目录回退 + **Kahn 拓扑排序**（环/缺依赖留在原序，由加载期报错）；能力注册册（全局唯一、仅提供者可注销、可列举、JSON 载荷） |
| `cxx_pluginxx/include/pluginxx/host/lifecycle.h` | 装载→create→start 事务→应用资源；禁用/启用的**显式级联**（`userDisabled` 与 `blockedByDependencies` 分开记，级联不覆盖用户意图）；stop 事务与**注册重声明**（stop 成功后清空插件侧记录，失败回滚顺序“先撤注册再释放资源”）；卸载等待 inflight 归零、超时与 `CloseFailed` 待重试 |
| `cxx_pluginxx/include/pluginxx/runtime/{runtime.h,instance_base.h}` | `InstanceLifetime` 把准入位与计数放在**同一次 CAS**，最后一个 lease 释放唤醒 idle 等待者；`InstanceLease` 保证关闭等待覆盖所有已进入插件代码的执行；**tombstone host 控制块**（永不释放，旧 host 指针只安全失败、绝不误指新实例） |
| `agentxx/lib/include/agentxx/plugin/{plugin_interfaces.h,plugin_manager.h,tool_registry.h,plugin_graph_node.h,client_plugin_manager.h}` | **三层接口协商**（声明/校验/决策；`require` 未满足=跳过并记 INFO，声明了本侧接口却缺入口符号=报错；`api_version` 下限不可被协商替代）；PluginManager 只保留领域注册与接缝，生命周期全继承；动态工具表以 `shared_ptr` 保活 + 静态工具名冲突检测；图类型用 slot 代次切换；客户端 UI 注册表 COW 快照、键位冲突可解释、**UI 线程绝不进入插件代码** |

### C.3 精读带来的结论变化（摘要）

1. **“取消后的上下文一致性”两侧都做了，只是机制不同**：dsh 在调度器里补规范错误结果（写时），agentxx 在节点里补 `[User canceled]` 占位（写时）+ 每次模型调用前 `repairMessages`（读时）——agentxx 的方案对“历史里已经存在的脏数据”更健壮，dsh 的方案对“重放可验证性”更严格。
2. **“并行工具”的差距比第 1 版更大**：dsh 的调度器把策略与执行拆段以支持执行体重叠；agentxx 明确是顺序执行并留有 TODO。
3. **KV Cache 意识是 dsh 的一等设计**（提示词更新模式、派生消息缓存的对象身份、摘要请求重建前缀），agentxx 只在压缩子代理上做了“同 thread/同模型”的局部优化。
4. **写所有权与原子性是 agentxx 最值得补的一块**（内核级租约 + 单写者语义），也是当前唯一会导致“数据损坏”而非“体验下降”的差异。
5. **两侧都依赖“纪律 + 门禁”保证注册可逆**，但 dsh 有统一登记入口与标签树可审计，agentxx 只能靠接口表的成对约定与测试覆盖。
6. **测试的“断言对象”不同**：dsh 断言持久日志与不变量（因此测试同时保护可重建性），agentxx 断言产出的事件流与内存状态（保护 UI 契约）。agentxx 若要把 §16 的各项“显式化”落地，测试断言对象必须先从“事件流”扩展到“持久状态 + 可重建性”。
7. **插件系统的差异是“两种成本模型”，不是“成熟度差距”**：dsh 把成本花在“框架兜底”（强制 effect 登记、依赖变化自动重启、隔离 realm + 泄漏检测、patch 可打印、教学式错误），agentxx 把成本花在“跨语言边界”（最小 ABI、exactly-once 完成通知、driver 协议、lease 与 tombstone 指针、显式级联 + 注册重声明）。两者的可迁移项因此是**互补**的：agentxx 值得补“组合与自描述（patch/dump/分级热改/教学式错误）”，dsh 值得补“跨卸载句柄安全与卸载超时重试语义”。详见 §18。

---

*本文基于对两个项目的源码、测试与文档的通读整理；第 2 版已逐文件核对源码（见附录 C）。若后续任一项目在对应模块做出改动，建议同步更新本文对应小节（每节末尾的“可迁移”清单可直接作为改动清单使用），并重新核对附录 C 中受影响的行。*

---

## 附录 D：可以继续深入的清单（按收益排序）

本文已覆盖“主干 + 插件系统”；以下位置**尚未逐文件精读**，按对结论的影响程度排序，供后续接续。每项都写明“读什么、能回答什么问题”。

### D.1 优先级最高（会改变对一侧的评价）

| # | 位置 | 能回答的问题 |
|---|---|---|
| 1 | agentxx `agent/lib/src/plugins/client_plugin_manager.cpp`（155 KB，全项目最大文件） | 客户端插件注册表的线程模型（UI 线程 vs client io 线程）、COW 快照的失效与回退、键位冲突记录、UI 表各子能力的真实生命周期。这是“agentxx 客户端插件是否真的做到 UI 线程不进插件代码”的唯一证据来源 |
| 2 | dsh `packages/api/*-controller/src/{index.ts,client/*}`（session/workspace/job controller + Remote 生成） | “Host 权威 + Client 镜像”到底怎么落地：stream 与 unary 的竞态合并规则、对象 identity 如何保持、重连时 baseline 替换的精确语义 |
| 3 | agentxx `agent/lib/src/middlewares/interrupt_ui.cpp` + `interrupt_presets.cpp` + client `components/interrupt_view.cpp` | 声明式中断/表单的**完整数据通路**（宿主表单状态机 ↔ 动作通道 ↔ 插件回调），对照 dsh 的 `user-questions`/`approval` seam；也是“UI 描述层（`ui.item`）与渲染层分工是否彻底”的检验点 |
| 4 | dsh `packages/typert/*` + `scripts/gen-*` 的 RPC/目录生成器 | 类型图生成的具体机制（descriptor/codec/source map/声明合并），判断 agentxx 的 wire 生成化方案该照哪一层抄 |
| 5 | agentxx `agent/lib/src/protocol/{openai_provider,anthropic_provider}.cpp`（87 KB / 24 KB） | 真实 provider 的请求构造、流式解析、reasoning 处理、错误映射与重试交互；对照 dsh `BlockAssembler` + 重试策略数据化，判断“流式协议收敛”的改造成本 |

### D.2 优先级中等（补全模块结论）

| # | 位置 | 能回答的问题 |
|---|---|---|
| 6 | dsh `packages/sandbox/*`（bwrap / Landlock / Seatbelt 后端）+ `packages/fs/*` | 真实沙箱后端的能力差异与降级策略；agentxx 若要引入“执行世界抽象”，这是最直接的参照 |
| 7 | dsh `packages/session-query-sqlite` + `session-query` 的 FTS 实现与有界读取 | “会话全文检索”的具体索引与分页设计，可直接映射为 agentxx 的 SQLite FTS 改造方案 |
| 8 | agentxx `agent/lib/src/agent/resource_applier.cpp` + `middlewares/{skill,memory_file}.cpp` | 插件资源声明（skill/memory/mcp）如何并入提示词与会话；对照 dsh 的 `skill` seam 与 `context` 子系统 |
| 9 | dsh `packages/mcp/mcp-client` + `packages/skills/*` + `packages/hooks/*` | 外部能力接入的边界处理（工具名作用域、指令注入、协议降级），与 agentxx 的 MCP client/server、skill 中间件对照 |
| 10 | agentxx `agent/lib/src/protocol/{mcp_server,acp_server,a2a_server}.cpp` | 三类对外协议的服务端实现差异（尤其 ACP/A2A 的会话与中断映射） |
| 11 | dsh `packages/compaction/compaction-tool-result-pruner` + `token-meter` | 剪枝策略与计量口径（对照 agentxx 的启发式 token 估算与 `SummarizationToolHandle`） |

### D.3 优先级较低（工程细节，可随任务顺手读）

| # | 位置 | 能回答的问题 |
|---|---|---|
| 12 | agentxx `agent/client/src/io/tui/**`（`agent_tui.cpp` 130 KB、`message_list.cpp` 92 KB、`ui_components.cpp` 75 KB） | TUI 的渲染/测量/命中/滚动实现，以及 `ui.item` 组件层在真实界面里的用法与边界（1 MiB 限制、树折叠、表单提交） |
| 13 | dsh `packages/client/ui-*`（slots/conversation/chat/trajectory） | Slot 体系与 Conversation target 的组装方式，判断“UI 插槽”该按什么粒度搬到 agentxx |
| 14 | agentxx `agent/benchmark/**` | 资源基准的实现细节（RSS/PSS/模块级分解/PTY 驱动），以及“真实两进程/真实 TUI”场景的可信度 |
| 15 | dsh `benchmarks/**` + `.agents/skills/*`（流程技能） | 性能门禁与“流程技能”这一可复用 AI 工作流（skill 驱动开发流程）的组织方式 |
| 16 | 两侧的 Python 侧：dsh `python/**`（SDK 运行时）、agentxx `agent/lib/src/ffi/**` | 被外部语言嵌入时的接口设计与测试方式 |
| 17 | dsh `native/node-addon-system` | `flock` / 命名信号量等系统能力的原生实现（附录 C 提到的写租约底座） |

### D.4 建议的接续方式

1. **按“问题”而不是按“文件”推进**：例如“客户端插件是否真的与 UI 线程解耦”“文本检索方案怎么落地”，一次只回答一个问题，写完就在本文对应小节追加，避免变成源码摘抄。
2. **每读完一处，先补附录 C 的行**（读了什么 → 发现了什么 → 是否推翻现有结论），再决定要不要改正文。
3. **对结论有影响的发现优先**：能推翻或强化某条对比结论的（D.1）先做；只补细节的（D.2/D.3）可以随任务顺手读。
4. **改代码前先把对应小节写成“可执行清单”**：本文每节末尾的迁移建议已经足够具体（落地文件 + 收益），可以直接当实施计划用；实施后回填“已实现/已变更”标记即可。


