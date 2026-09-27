# agentxx 与 codex 架构对比

> 对比对象
> - `agentxx`: `D:\0Acoolight\Program\cpp\agentxx`（C++23, Boost.Asio 协程 + NeoGraph 图引擎 + C ABI 插件）
> - `codex`: `D:\0Acoolight\Program\js\codex`（Rust 工作区 `codex-rs` + 极薄 Node 包装 `codex-cli`，app-server 作为客户端/核心之间的中枢）
>
> 本文按模块通读两侧源码、测试与文档后逐模块记录差异、优缺点与**可迁移到 agentxx 的设计**。
> 阅读顺序建议：先看[总览](#0-总览与对比方法)与[迁移建议汇总](#17-迁移建议汇总)，再按模块展开。
>
> **编写说明**
> - 全文统一按“现状 → 对比（含优缺点）→ 可迁移到 agentxx 的设计”三段结构组织；每个模块的结论都给出源码依据，依据清单见[附录 C](#附录-c本次精读的源码清单与结论依据)。
> - 只写**能在 agentxx 现有架构上落地**的迁移项，并对每项标注落地位置与相对优先级（[§17](#17-迁移建议汇总)）；反向结论见[§18](#18-反向清单agentxx-不必照搬的设计)。
> - 文中数值（代码行数、测试与快照数量、crate 数量）均为本次实测，统计口径写在该段落内。
>
> **修订记录**
> - **第 2 版（源码精读）**：本轮把两侧关键实现文件整段读完（不再只看文件头与目录），逐模块补入“实现细节”并修正第 1 版中凭接口推断的结论。主要新增与更正：
>   1. 轮次语义（§2.4）：补上 `InputQueue`/`TurnState`/`MailboxDeliveryPhase` 的具体结构，以及**请求抢占**机制（`StepContext.preempt` 令牌 + `watch_user_input`，用户输入到达即中断当前采样并复用原始输入重发）——第 1 版只写了“steering 通道”，未说明抢占实现。
>   2. 工具系统（§5.4）：补上工具注册表的治理规则（保留名、冲突记录、`ToolExposure::deferred` 命名空间）、`build_tool_call` 的 payload 映射、工具取消时的 `select!` 与 `AbortedToolOutput`；**更正**：`parallel_tool_calls` 请求字段恒为 `true`（`build_prompt` 里写死），并发与否完全由运行时闸门按工具声明决定，第 1 版表述容易读成“请求级开关与工具声明共同决定”。
>   3. 工具系统（agentxx 侧，§5.5）：`execTool`/`baseRun` 的完整控制流（参数类型自动修正、总线权限询问、重复调用确认卡片、重试、share store 溢出改存、中断缓存复用、取消占位补齐、动态插件工具查找）。
>   4. 压缩（§8.5）：codex 压缩本身就是一次采样轮（摘要提示词作为用户输入、走同一套流与重试），且保留用户消息；agentxx 压缩用**同会话同模型子代理**（命中 KV 缓存）、有“手动压缩直派宿主”这条为绕开空闲态中断逃逸而专门加的分支，并给出 `hardTruncate` 的完整兜底算法。
>   5. 权限（§9.5）：agentxx `decideTarget` 的判定顺序与源码内已知缺口（不解析符号链接，含原因说明）、`requestPermission` 的“规则必须注册在中间件而非 IO 端点”的原因、无 prompter 默认拒绝等；codex 侧补审批缓存实现与 `exec_approval` 对策略修订的落盘路径。
>   6. 持久化（§4.5）：codex `live_writer` 的进程内写者互斥 + 跨进程锁文件 + 恢复路径解析；agentxx 的节流实现细节（`kPersistThrottleMs = 3000`、首次立即落盘、展示历史写操作排队 `PendingViewOp`）。
>   7. 服务与协议（§13.3）：app-server 的请求处理器划分（约 24 个 RequestProcessor）、请求序列化队列、连接 RPC 闸门、轮次准入、成本/模型刷新等后台工作线程。
>
> - **第 3 版（再深入）**：继续读完上一版列在附录 D 的“优先级最高”文件（`client.rs`、`codex_thread.rs`、`thread_manager.rs`、`context_manager/history.rs`）以及若干此前只列过名字的实现（`unified_exec/process_manager.rs`、`hooks/engine/*`、`rollout/search.rs`、`execpolicy/parser.rs`、agentxx 的 `wire_protocol.h`/`tool_registry.cpp`/`subagent.cpp`/`ui_components.cpp`/`plugin_manager_lifecycle.cpp`）。新增与更正：
>   1. LLM 层（§7.1）：`ModelClient`（会话级）与 `ModelClientSession`（轮次级）的职责边界、WebSocket 增量复用判定、`x-codex-turn-state` 粘性路由令牌的生命周期约束、**HTTP 回退是会话级**（一次回退后本轮后续所有轮次都用 HTTP）。
>   2. 会话与线程接缝（§3.1、§10.1）：`ContextManager` 的字段与不变式（`Arc` 共享快照、`retained_context` 与模型窗口解耦、三个版本号）、`CodexThread` 的对外 API 语义（含 `suspend_turn_and_shutdown` 的“不得中途转移所有权”约束）。
>   3. 持久执行（§10.1）：`unified_exec` 的环境加固清单与限额（进程数、yield 时间夹取、stdin 审批 8000 字节、Ctrl-C 中断、晚到网络拒绝 100ms 宽限）。
>   4. 检索与规则（§9.1、§11.1）：命令策略是 **Starlark DSL**，且规则可声明正/负样例并在加载期校验；会话搜索优先调用 `rg` 并回落到内置扫描（含压缩文件）。
>   5. 扩展面（§16.1）：hooks 引擎的执行器结构（command/mcp 两类 handler、事件匹配规则、运行摘要）与插件生命周期接缝函数清单。
>   6. 更正（§13.2 与 §17-P0 第 7 条）：wire 消息常量在 `wire_protocol.h` 里**逐条带注释文档**，缺口不是“没文档”，而是缺机器可校验的往返测试与生成式文档（测试目录中未搜到 wire 往返用例）。

## 目录

| 章节 | 内容 | 一句话结论 |
|---|---|---|
| [0](#0-总览与对比方法) | 总览与对比方法 | 两项目定位、规模、判据与结论速览 |
| [1](#1-总体架构与服务分层) | 总体架构与服务分层 | codex 把“客户端—服务—核心”做成可远程的 RPC 枢纽，agentxx 用进程内端点 + 传输抽象达到近似效果；可借鉴的是**服务层可独立部署**与**契约生成** |
| [2](#2-agent-循环与轮次语义) | Agent 循环与轮次语义 | 应显式化“轮次/步骤/输入通道”，把 steering 与 inject 分开，并给取消、恢复、重放明确语义 |
| [3](#3-会话与-llm-上下文) | 会话与 LLM 上下文 | agentxx 的“会话即权威”方向正确；可吸收**历史快照 + 世界状态差分**这一套“上下文只增量变化”的做法 |
| [4](#4-持久化与崩溃恢复) | 持久化与崩溃恢复 | 需补格式版本、写者所有权、崩溃配平与重建（重放）路径 |
| [5](#5-工具系统) | 工具系统 | 应把“审批/沙箱/重试”提升为编排器，把工具输出契约与渲染分离，并支持并行执行声明 |
| [6](#6-系统提示词请求组装与世界状态) | 系统提示词、请求组装与世界状态 | 段落化 + 快照/差分是提升前缀缓存命中的关键，代价是要维护片段识别与基线 |
| [7](#7-llm-流式适配器与网络层) | LLM 流式、适配器与网络层 | 能力协商下沉到 provider，重试与请求粘性收敛为统一策略点 |
| [8](#8-上下文压缩与上下文窗口) | 上下文压缩与上下文窗口 | 把“剪枝/摘要/换窗”拆成可组合策略，并让压缩状态可检测、可恢复 |
| [9](#9-权限审批与沙箱执行) | 权限、审批与沙箱执行 | 引入“权限档案（profile）+ 审批缓存 + 命令策略 + 真实沙箱”四层，agentxx 目前缺最后一层 |
| [10](#10-子代理多代理与后台任务) | 子代理、多代理与后台任务 | 代理注册表 + 邮箱 + 派生（fork）语义 + 占用配额是最大功能缺口 |
| [11](#11-检索标题遥测附件记忆与大输出) | 检索、标题、遥测、附件、记忆与大输出 | 索引化检索 + 输出截断策略 + 记忆读写分离，都可以低成本落地 |
| [12](#12-客户端-ui-与服务端渲染分层) | 客户端 UI 与服务端渲染分层 | codex 的“TUI 是 app-server 的客户端”值得借鉴：客户端只做投影与渲染 |
| [13](#13-远程协议sdk-与外部集成) | 远程协议、SDK 与外部集成 | 协议生成化 + 能力位 + 无界面执行入口（exec/exec-server）是明显缺口 |
| [14](#14-测试与质量门禁) | 测试与质量门禁 | 录制式模型模拟 + 快照 UI 测试 + 断言/风格门禁显著降低回归成本 |
| [15](#15-配置特性开关与装配体系) | 配置、特性开关与装配体系 | 配置分层栈 + 生成式 schema + 特性阶段标记，比“读一个 yaml”更抗腐化 |
| [16](#16-扩展机制专题五种扩展面-vs-c-abi-插件) | 扩展机制专题 | codex 有 5 种扩展面且各有边界；agentxx 的 C ABI 插件在跨语言/隔离上更强，缺的是“声明式挂载点” |
| [17](#17-迁移建议汇总) | 迁移建议汇总 | P0/P1/P2 三级清单 |
| [18](#18-反向清单agentxx-不必照搬的设计) | 反向清单 | 明确哪些差异是正当取舍 |
| [19](#19-结语) | 结语 | 三条原则 |
| [附录 A](#附录-a关键文件与文档索引) | 关键文件/文档索引 | 两项目对应模块的路径对照 |
| [附录 B](#附录-b术语对照) | 术语对照 | 同一概念的两种叫法 |
| [附录 C](#附录-c本次精读的源码清单与结论依据) | 源码清单与结论依据 | 逐条列出本次实际读过的文件 |
| [附录 D](#附录-d可以继续深入的清单) | 可继续深入清单 | 尚未精读的位置与能回答的问题 |

---

## 0. 总览与对比方法

### 0.1 两个项目的定位与规模

| 维度 | agentxx | codex |
|---|---|---|
| 语言/运行时 | C++23；Boost.Asio 协程，单 `io_context` 内多会话交错执行 | Rust（`codex-rs` 工作区）；tokio 多任务 + `async_channel`/`oneshot`/`watch` 通道 |
| 顶层结构 | 3 层库：`agent/lib`（libagentxx）、`agent/client`（CLI/TUI）、`agent/plugins`（内置能力插件） | 111 个顶层 crate（另有 `ext/*`、`utils/*`、`memories/*` 等嵌套 crate）：`core`（会话/循环）、`app-server`（RPC 服务）、`tui`（客户端）、`exec`（无界面执行）、`protocol`（线协议）、`sandboxing`/`execpolicy`（执行安全）、`rollout`/`thread-store`/`state`（持久化）等 |
| 扩展形态 | 纯 C ABI 动态库插件，接口表按名称查询；宿主侧 19 张 agent 表 + 客户端 9 张表 | 5 种扩展面并存：扩展 crate（`ext/*`，编译期）、hooks（外部进程）、skills（Markdown）、MCP、插件市场包（npm/bundle） |
| 编排核心 | NeoGraph 图引擎（节点 + 条件边），中间件包裹节点 | `Session` 提交循环 + `SessionTask` 实现（regular/compact/review/user-shell）+ `TurnContext`/`StepContext` 两级快照 |
| 会话与历史 | `Session` 持有 typed LLM 上下文（唯一权威）+ 展示历史；SQLite 落库 | 历史在内存 `ContextManager`，持久化为**逐行 JSONL rollout**（append-only），另有 SQLite 状态库做索引/元数据 |
| 权限与执行 | 权限中间件 + 插件声明权限目标 + worktree 隔离；命令直接在宿主进程执行 | 权限档案（profile）+ 审批缓存 + `execpolicy` 命令策略 + 系统级沙箱（Seatbelt/Landlock/bubblewrap/Windows）+ 受管网络代理 |
| 客户端 | FTXUI TUI、stdio CLI、远程 WS 客户端；客户端插件（C ABI）；UI 走自定义 JSON 组件描述 | ratatui TUI（当作 app-server 的客户端）、`exec` 无界面模式、TS/Python SDK、IDE 集成 |
| 测试 | 自研 `agentxx_test`（约 80 个模块）+ `agentxx_benchmark` 资源基准 | `core/tests/suite` 201 个集成测试文件 + 1324 个 insta UI 快照 + mock SSE 服务器 |
| 构建 | CMake + 3 个可独立发布的工具库；Debug 默认 ASan/UBSan | cargo + Bazel 双构建；clippy/rustfmt/自定义 lint（含参数注释 lint） |
| 代码规模（本次实测） | `lib` 138 文件 / 54.1k 行；`client` 68 文件 / 23.6k 行；内置插件 15 个 | `codex-rs` 4907 个 `.rs` / 1.81M 行（含测试与快照） |

### 0.2 对比方法与判据

- 每个模块都用**三方证据**核实：源码（真实实现）、测试（行为边界）、文档（设计意图）。凡文档与源码冲突处，以源码为准。
- “优点/缺点”的判据只有两条：
  1. **后续改动成本**：扩展点是否收敛、约束是否显式、有没有“只有作者知道”的隐含前提；
  2. **运行期正确性**：生命周期、取消、恢复、并发与资源回收是否可控。
- 迁移建议只写**能在 agentxx 现有架构上实现**的项，并标注落地位置（哪个文件/哪一层）与代价；不能落地的不写。

### 0.3 结论速览

| 模块 | agentxx 现状 | codex 现状 | 更值得借鉴的方向 |
|---|---|---|---|
| 服务分层 | 进程内端点或 WS 传输，客户端与 agent 同库 | 客户端—app-server—core 三段，RPC 契约可跨进程/跨机器 | 服务层独立部署 + 契约生成 |
| 轮次语义 | 单通道消息队列（轮末消费）+ HIL 中断恢复 | `start-or-steer` / `start-if-idle` / `steer` / `continue` 四种投递模式，结果显式（Started/Steered/NotSubmitted） | steering 通道与显式投递结果 |
| 会话上下文 | 会话即权威（已有 typed 上下文 + 版本号） | `ContextManager` + world state 差分 + 上下文片段注解 | 世界状态差分与片段标注 |
| 持久化 | 单会话单 SQLite 库（消息/上下文/meta/store） | JSONL rollout（权威日志）+ SQLite 索引 + 线程写者锁 + 压缩/重建 | 写者所有权、重放重建、格式版本 |
| 工具 | `XXToolBase` + 图分发 + 中间件 + 插件声明权限 | `ToolRegistry`→`ToolRouter`→`ToolOrchestrator`（审批/沙箱/重试）+ 并行声明 + hooks 注入 | 编排器、输出契约、并行声明 |
| 提示词 | 系统提示词字符串拼装 + 追加段 | `prompts` crate + 上下文片段结构体 + 世界状态快照/差分 | 片段化与差分 |
| LLM 层 | provider 注册表 + 限流装饰器 + 重试 | provider 能力协商 + 响应流事件 + 请求粘性/缓存键 | 能力协商、请求粘性 |
| 压缩 | 阈值触发 + 子代理摘要 + 去重/冷却 | 前置/轮中/轮后/手动四时机 + 本地/远端两实现 + 换窗 | 时机拆分与换窗 |
| 权限/沙箱 | 中间件统一判定 + 声明式询问卡片 | profile + 审批缓存 + 命令策略 + 系统沙箱 + 网络代理 | 真实沙箱与审批缓存 |
| 子代理 | 子代理中间件 + 事件总线批量委派 | 线程管理器 + 代理注册表 + 邮箱 + 派生模式 + 占用配额 | 派生语义与邮箱 |
| 检索/记忆 | share store + RAG 插件 | 会话索引/搜索 + 记忆读写流水线 | 索引化检索与记忆读写分离 |
| 客户端 | TUI 直接持有 agent，声明式组件描述 | TUI 是 app-server 客户端，快照测试覆盖 | 客户端只做投影/渲染 |
| 协议/SDK | 自定义 WS wire + MCP/ACP/A2A | JSON-RPC v1/v2 + TS/Python SDK + exec/exec-server | 生成式类型与无界面入口 |
| 测试 | 自研多模块测试 + 资源基准 | 集成测试套件 + UI 快照 + mock 模型服务 | 录制/模拟模型 + 快照 |
| 文档 | 手写设计文档（index/plugins/tui/benchmark/ffi） | 生成式 schema + 规范化的 API 变更流程 | 生成式契约 |
| 扩展 | C ABI 插件（跨语言、可隔离） | 扩展 crate + hooks + skills + MCP + 市场插件 | 声明式挂载点与按 agent 作用域 |

---

## 1. 总体架构与服务分层

### 1.1 codex：crate 工作区 + app-server 中枢

**四条运行路径，一个核心**

```
codex-tui ──┐
exec/SDK ───┼─→ app-server（JSON-RPC v1/v2）─→ codex-core（Session/Turn/Tool）
IDE 插件 ───┘         │
                      └─ transport: stdio / websocket / control socket（daemon）/ 进程内 channel
```

- `codex-rs/app-server` 是唯一对外服务面：`lib.rs` 里同时启动 stdio 连接（`start_stdio_connection`）、WebSocket 接收（`start_websocket_acceptor`）、控制 socket（`start_control_socket_acceptor`、`acquire_app_server_startup_lock`）、远端控制（`start_remote_control`），每条连接有独立的 `ConnectionId` 与出站队列（`OutgoingMessageSender`/`OutgoingEnvelope`），并有连接清理与背压（`transport.rs` 的 `CHANNEL_CAPACITY`）。
- TUI 并不直接持有 `Session`，而是持有一个 `AppServerClient`：本机运行为 `InProcessAppServerClient`（拿到一个进程内 channel），远程运行为 `RemoteAppServerClient`（`RemoteAppServerEndpoint`）。`codex-rs/tui/src/lib.rs` 的导入列表里同时出现这两种客户端与 `app_server_session::AppServerSession`，说明“同一套客户端代码既跑进程内也跑远程”是刻意设计。
- `codex-core` 是最大也最容易被滥用的 crate：仓库 `AGENTS.md` 直接写着 **“resist adding code to codex-core”**，并给出判断顺序：先找别的 crate，其次新建 crate，最后才考虑改 core。这不是口号——`rollout`/`thread-store`/`state`/`sandboxing`/`execpolicy`/`hooks`/`skills`/`memories`/`code-mode*`/`app-server-protocol` 都是被这样拆出去的结果。
- 编译期扩展走 `codex-extension-api`：`SessionSpawnArgs` 里传入 `extensions: Arc<ExtensionRegistry<Config>>`，注册表按能力分组暴露 `context_contributors()`、`tool_lifecycle_contributors()`、`token_usage_contributors()`、`config_contributors()`；`ext/` 下的 `agent`、`guardian-reviewer`、`memories`、`skills`、`queue`、`web-search`、`items`、`history-notes` 等都在此基础上做功能，改动不落在 `core` 里。

**实现细节（`session/input_queue.rs`、`state/turn.rs`、`session/turn.rs`）**

- 队列分两层：**会话级** `InputQueue` 持有邮箱双端队列（`VecDeque<PendingMailboxCommunication>`，每项带 `TurnStartOptions` 与一个诊断 gauge）与一个 `activity_tx: watch`；**轮次级** `TurnInputQueue` 持有 `items: Vec<TurnInput>`（用户输入 / 工具输出 / 响应项 / 代理间消息四类）。
- 邮箱投递有明确状态机 `MailboxDeliveryPhase::{CurrentTurn, NextTurn}`：轮次开始是 `CurrentTurn`（允许子代理邮件并入本轮的下一次请求）；一旦本轮已经产出对用户可见的最终文本，切到 `NextTurn`（晚到的子代理邮件留在队列里等下一轮，而不是延长已经展示过答案的那一轮）；若同一轮之后又出现“显式同轮工作”（用户 steer，或未标注前导文本后的工具调用），再切回 `CurrentTurn`。
- `defer_mailbox_delivery_to_next_turn` 里有一处细节：如果挂起项里存在**非**“仅排队”的子代理邮件（`trigger_turn == false` 之外的项），就不切换到 `NextTurn`，因为那些是同轮工作、仍需后续采样。
- **请求抢占**：`StepContext.preempt` 是一个 `CancellationToken`（仅在 `Feature::InstantInterrupt` 开启时创建）。`run_sampling_request` 在开始时调用 `watch_user_input(...)`，它订阅活动通知并 spawn 一个任务：一旦 `TurnInputQueue` 里出现 `UserInput`，立刻取消 `preempt`。采样循环对 `stream.next()` 与重试都套 `or_cancel(&preempt)`：被抢占时**不报错**，而是返回 `needs_follow_up = true` 并把 `original_input` 原样交回，于是同一轮进入下一次 step、把新输入并入——这就是“用户插话即时生效”的实现，而不是等整轮结束。
- 邮箱邮件也能抢占，但只在模型处于“思考/评论”阶段：`preempt_for_mailbox_mail` 对 `MessagePhase::Commentary` 的助手消息与 `Reasoning` 项为真（`AgentMessage` 为假）。
- 工具结果是**按序收集**的：`try_run_sampling_request` 维护 `in_flight: FuturesOrdered<InFlightFuture>`，多个工具调用可并行派发，但结果按模型声明顺序落地；`OutputItemDone` 到达时会结束该调用对应的 `active_tool_argument_diff_consumer`（流式参数差分）。
- `TurnState` 才是“这一轮的全部可变状态”：5 张挂起等待表（审批、权限申请、用户提问、MCP 征询、动态工具）、`pending_input`、邮箱投递相位、按环境记录的已授予权限（`merge_permission_profiles` 合并）、`strict_auto_review`、工具调用计数、记忆引用标记、轮初用量快照与按模型的用量、`last_known_step_context`。
- `RunningTask` 同时持有 7 样东西：完成通知 `Notify`、任务种类、任务对象、取消令牌、`AbortOnDropHandle`、`TurnContext`、代理执行配额守卫（`AgentExecutionGuard`），外加诊断 gauge 与 OTEL 计时器——取消、指标、配额都挂在同一个结构上，避免散落。

**源码要点**

- `SessionSpawnArgs`（`core/src/session/mod.rs`）有 40+ 个字段，本身就是一个“装配清单”：auth、models_manager、git_root_discovery、environment_manager、skills_service、plugins_manager、mcp_manager、code_mode_session_provider、extensions、thread_store、image_store、attestation_provider、external_time_provider……。这套“依赖注入 + 特性开关”让 core 不必知道具体实现，也让测试可以替换任意一层。
- 出站事件统一走 `Session::send_event*` 一族：先写 rollout（`persist_rollout_items`），再投递给 `tx_event`（客户端），必要时镜像到实时语音通道（`maybe_mirror_event_text_to_realtime`）与父代理（`maybe_notify_parent_of_terminal_turn`）。**“先落盘、后通知”**是这里的隐含顺序约定。

### 1.2 agentxx：分层库 + 端点传输

- 三层职责分明：`agent/lib`（libagentxx：BaseAgent/CodeAgent、节点、中间件、协议、UI 描述层）、`agent/client`（CLI/TUI/配置加载）、`agent/plugins`（15 个内置能力插件）。
- 客户端与 agent 的解耦靠 `AgentIOBase` 端点：`sendToPeer()` 发送、`onPeerMessage()` 接收并分发到受保护回调（`onDelta` 等）。进程内运行时用 Channel 传输，跨进程时用 WebSocket（`agent/client` 可 `--agent` 启动服务端）。
- 会话执行入口是 `BaseAgent::runTurnAsync`（`lib/src/agent/base_agent.cpp`）→ `AgentRunner::run`（`lib/src/agent/agent_runner.cpp`）→ `engine->run_stream_async()`；中断-恢复在 `AgentRunner` 的 `while (result->interrupted)` 循环里处理。
- 线程模型：单 `io_context` 内多会话协程交错，不需要线程锁；`Session::bindIoThread()` 在轮次开始时把会话绑定到当前线程，用于抓出跨线程误用。重活（附件读盘、base64、压缩计算）经 `utilxx::offloadAsync` 卸载到线程池，避免阻塞 io 线程（`base_agent.cpp` 中服务端附件加载即此模式）。

### 1.3 对比

| 维度 | agentxx | codex |
|---|---|---|
| 服务面 | 端点抽象（进程内/WS），TUI 与 agent 同库同进程或经 WS | app-server 是独立进程/独立 crate，支持 stdio/WS/控制 socket/进程内四种连接 |
| 跨机器 | 支持（WS 客户端—服务端），附件支持“服务端自取” | 支持（`RemoteAppServerClient`、`codex exec-server`、远程环境 `remote_env` 测试场景） |
| 依赖方向 | `lib` ← `client` / `plugins`；`lib` 内节点/中间件/工具/插件分层 | 严格单向：`protocol`（纯类型）← `core` ← `app-server` ← `client`；安全与持久化独立成 crate |
| 装配方式 | `BaseAgent::init()` 顺序：中间件 → 工具 → 图；子类 override 扩展 | `SessionSpawnArgs` 注入 + `ExtensionRegistry` 注册表 + `Feature` 开关 |
| 客户端耦合 | 客户端插件（C ABI）与 UI 组件描述层与 agent 共享 libagentxx | 客户端只依赖 `app-server-protocol` + `app-server-client`，不依赖 core |

**优点/缺点**

- codex 的“客户端只依赖协议 crate”让 TUI、exec、SDK、IDE 插件共用一条链路，代价是 `core` 与 `app-server` 之间的类型映射代码量大（`app-server-protocol` 里 v1/v2 两套协议、`bespoke_event_handling`、`event_mapping`）与额外的序列化开销。
- agentxx 的端点抽象更轻，缺点是**服务能力与 UI 能力混在同一个可执行文件里**：“无界面后台服务 + 多客户端 attach”这种部署形态没有成为一等公民（`agentxx_cli --agent` 是服务端，但缺少 daemon 生命周期管理、控制 socket、连接鉴权等配套）。

### 1.4 可迁移到 agentxx 的设计

1. **独立服务模式 + 控制通道**（建议 P1）：把 `agentxx_cli` 的 agent 服务端做成可后台常驻（PID/锁文件/控制 socket），允许后续用第二个客户端进程 attach 到已有会话。落地位置：`agent/client/main.cpp`（启动模式）、`SessionServerAgentIO`（连接注册与重放已具备）、新增 transport 类型。
2. **协议契约生成**（建议 P2）：wire 消息目前是手写 C++ 结构体 + `toJson/fromJson`，可加一层“从声明生成序列化与文档 + 生成 TS 类型”的脚本，避免客户端与服务端手工同步。落地位置：`agent/lib/src/agent/wire_protocol.cpp`、`docs/zh-cn/design/index.md`。
3. **“新功能新模块”纪律**（建议 P0，成本近零）：借鉴 `AGENTS.md` 对 `codex-core` 的约束，在项目文档里明确“通用库不承载具体功能，新能力优先放 `plugins/` 或新目录”，并给出现有边界（`lib/src/agent` 只放会话/上下文/持久化骨架）。

---

## 2. Agent 循环与轮次语义

### 2.1 codex：提交循环 + 任务对象 + 步骤快照

**三层概念**

| 层 | 类型 | 作用 |
|---|---|---|
| 提交 | `Submission`（含 `Op`、trace、parent/root turn id、residency guard） | 所有外部动作都进 `async_channel::bounded(512)` 队列，由 `submission_loop` 串行处理 |
| 轮次 | `SessionTask`（`RegularTask`/`CompactTask`/`ReviewTask`/`UserShellCommandTask`） | 一个任务的完整生命周期：取消令牌、span、abort 回调、完成事件 |
| 步骤 | `TurnContext`（整轮不变）+ `StepContext`（每次采样前重新捕获） | 把“用户设置”和“本次请求实际用到的工具/环境/模型”分开，避免一轮内漂移 |

**轮次内循环**（`core/src/session/turn.rs::run_turn`）依次做：

1. 轮前压缩（`run_pre_sampling_compact`）→ 记录用户输入（`run_hooks_and_record_inputs`）；
2. 捕获 step（`capture_step_context_with_required_mcp_servers`，会等 MCP/插件就绪）→ 记录上下文更新并建立 `reference_context_item`（只有第一个真实用户轮注入全量上下文，之后只发差分）；
3. `loop {` 排空 pending input（steering）→ 记录世界状态差分 → 按会话历史构造请求 → 采样；
4. 采样返回 `needs_follow_up`（有工具调用）或最终消息；有 pending input 也算需要下一轮；
5. token 到顶或显式“换窗”请求 → `run_auto_compact`（轮中压缩）后 `continue`；
6. 无后续 → 跑 stop hooks（可阻止结束并追加续跑提示）→ 可选轮后压缩 → `break`。

**取消、中断与留痕**

- `abort_all_tasks(reason)` 支持 `TurnAbortReason::{Replaced, Interrupted, BudgetLimited, ...}`；先取消 `CancellationToken`，等 `done` 通知，最多 `GRACEFULL_INTERRUPTION_TIMEOUT_MS = 100ms` 后 `handle.abort()` 强杀，再调用 task 的 `abort()` 做收尾。
- 中断会**写进模型可见历史**：`interrupted_turn_history_marker` 按配置与多代理版本选择 `ContextualUser`（用户上下文片段）、`Developer`（开发者片段）或 `Disabled`（不写），然后 `flush_rollout()` 保证客户端读到中断事件时历史已经落盘。
- 有个容易漏掉的细节：`abort_all_tasks` 先让任务观察取消，**再**清理 pending 审批（`clear_pending`），否则“正在等待审批的调用”会先被当成拒绝反馈给模型。

**输入通道**（`protocol/src/turn_input.rs`）

| 投递模式 | 语义 |
|---|---|
| `StartOrSteer` | 空闲则开新轮，忙则并入当前轮（steering） |
| `StartIfIdle` | 仅空闲时开新轮，否则 `NotSubmitted{NotIdle}` |
| `ContinueIfIdle{expected_previous_turn_id}` | 内部续跑，若已被别的工作抢占则 `Superseded` |
| `Steer{expected_turn_id}` | 只并入指定轮次，不匹配则 `ExpectedTurnMismatch` |

- 结果显式返回 `Started{turn_id}` / `Steered{turn_id}` / `NotSubmitted{reason}`，`NotSubmittedReason` 列了 9 种原因（`NotIdle`、`PendingTriggerTurn`、`PlanMode`、`NoActiveTurn`、`ActiveTurnNotSteerable{turn_kind}`、`ActiveTurnOutputSchemaMismatch`、`EmptyInput`、`ServerDraining`、`Superseded`）——调用方不需要猜。
- 代理间消息走邮箱（`InterAgentCommunication` + `MessageDeliveryMode::{QueueOnly, TriggerTurn}`），空闲线程可在“有待触发邮件”或“有未完成的 durable sleep”时自动开轮（`maybe_start_turn_for_pending_work`）。

**源码要点**

- `start_task` 里有一处刻意的断言面：`debug_assert!(turn.task.is_none())` 与 `self.record_started_turn(...)` 同时在 `active_turn` 锁内完成，保证“记录已开始轮次”与“占用活动轮次槽位”原子；任务 span 里预声明了 7 个 token 用量字段，结束时回填。
- `run_turn` 里 `can_drain_pending_input` 的取值分三种情况：轮首为 `false`（先采样本轮输入）、正常轮为 `true`、轮中压缩后按“是否需要续跑/是否被抢占”决定——这就是 steering 输入与压缩之间的顺序规则。

### 2.2 agentxx：图引擎驱动的 ReAct 循环

- 入口 `BaseAgent::runTurnAsync`：绑定会话 io → 建/复用事件总线 → 组装 `EventBridge`（会话级 seq 递增后 `sendToPeer`）→ 追加用户消息 → `AgentRunner::run`。
- `AgentRunner::run`：`engine->run_stream_async()`；若结果 `interrupted`，进入循环：把中断现场写入 `graphData` 并序列化进图状态通道（`xx_interruptNode`/`xx_interruptValue`），按中断参数类型分流——
  - `subagent` 类型：经 `ctx->bus` 请求 `Topic::Subagent`，批量委派后把结果写成 resume 值；
  - 其余走 HIL：经 `Topic::Interrupt` 请求客户端表单，收到 `{"values":{...}}` 后写回 resume 值；
  然后 `engine->resume_async()` 从中断节点继续。循环退出条件明确区分“中断已处理完”与“中断未完成”（`outcome.unresolvedInterrupt`）。
- 取消：`CancelToken` 贯穿 LLM 请求与工具执行；取消时保留已定稿消息，未完成的 tool_call 由 `ToolcallWrapNode` 补 `[User canceled]` 占位；另有读时修复 `repairMessages`（清悬挂 tool_calls、删孤儿结果、合并连续 user）兜底。
- 一轮内的“输入”只有一条通道：`SessionServerAgentIO::messageQueue_`（`MessageQueueItem`），运行中的用户输入入队，**轮末**出队继续；`WireClearMessageQueue`/`WireRemoveQueueItem`/`WireInterruptAndRunNext` 分别用于清空、删除单项、中断当前轮并立即跑队列。
- 会话活动状态 `SessionActivity{Idle, Streaming, ExecutingTool, WaitingInput}` 供 UI 展示；设置（模型/语言/压缩参数）按 `sessionId` 分片存储，模型可在轮次间切换（`selectModel`）。

### 2.3 对比

| 维度 | agentxx | codex |
|---|---|---|
| 轮次入口 | `runTurnAsync` 一次调用跑完一轮（含中断恢复循环） | `Op` 队列 + `SessionTask`，同一会话多任务串行、任务种类可扩展 |
| 步骤 | 图节点（llm/tools）粒度，无显式 step 快照 | `StepContext` 每次采样前重捕获，工具集/模型/环境随 step 变化 |
| 运行中输入 | 客户端队列（轮末消费）+ “中断并跑下一条” | `steer` 通道，轮内任意采样边界消费；配合 `preempt` 令牌可在采样中途抢占当前请求 |
| 中断语义 | HIL 中断（表单）或取消（CancelToken） | `TurnAbortReason` 枚举 + 中断留痕写入模型历史 + 100ms 优雅期 |
| 恢复 | 图状态 `resume_async`（进程内/重启后） | rollout 重放（`apply_rollout_reconstruction`）重建历史与设置，再开始新轮 |
| 拒绝/空轮 | 中间件可改 `NodeInput`，但没有“本轮不请求模型”的语义 | `NotSubmitted{reason}` 与 stop hooks 的 block 语义 |
| 并发 | 单 io_context 多会话交错，同一会话内工具顺序执行 | tokio 多任务，同一轮内工具可并行（工具自声明） |

**优点/缺点**

- codex 的强项是**语义显式**：请求是“开始还是并入”，结果是“开始/并入/被拒（原因）”，中断有原因与留痕。代价是类型与状态机多（`TurnInput`/`TurnInputRequest`/`TurnStartOptions`/`TurnInputSubmission`/`NotSubmittedReason` 五组类型）。
- agentxx 的强项是**中断即恢复**：HIL 中断能在进程重启后从图状态继续，这是 codex 没有对等能力的（codex 的审批是活进程内的一次性 `oneshot`，进程没了就没了）。缺点是轮次语义薄：只有“一轮 + 队列”，没有 steering、没有显式的投递结果、没有“本轮不请求模型”。

### 2.4 可迁移到 agentxx 的设计

1. **给队列项加目标语义（next-turn / next-step）**（建议 P0）：现在运行中输入只能等轮末。给 `MessageQueueItem` 增加目标字段，`next-step` 项在当前轮的**下一次 modelcall 之前**注入（等价 steering），`next-turn` 保持现状。落地：`wire_protocol.h`（`WireUserInput`/`WireMessageQueueUpdate`）、`SessionServerAgentIO::pushMessageQueueItem`、`base_agent.cpp` 轮内循环、`nodes/modelcall.cpp` 的注入点。
2. **投递结果显式化**（建议 P1）：`WireUserInput` 的回执改成 `accepted{target}` / `rejected{reason}` 枚举（原因至少区分：会话不存在、正在压缩、等待用户输入中、被取消），UI 才能给出准确反馈。落地：`wire_protocol.h` + `agent_tui.cpp` 的输入处理路径。
3. **中断留痕结构化**（建议 P1）：`[User canceled]` 占位已经在做，建议升级为带原因与阶段的结构化片段（用户取消 / 超时 / 被新输入抢占 / 预算耗尽），并保证“未分发的 tool_call”一定有对应结果（现在靠取消分支 + `repairMessages` 两处兜底）。
4. **优雅中断超时**（建议 P2）：agentxx 目前取消后由节点自行响应，可在 `AgentRunner`/`BaseAgent` 层加“取消后 N ms 未收敛则强制结束本轮并记录日志”的保护，避免个别工具卡住整轮。落地：`agent_runner.cpp` 循环与 `nodes/toolcall.cpp` 的取消点。
5. **步骤级快照**（建议 P2）：一轮内如果模型/工具集/权限发生变化（如插件在轮中启用），当前实现直接读会话最新设置。可引入 `StepSnapshot`（模型 + 工具名集合 + 权限快照）在每次 modelcall 前捕获，工具分发时只用快照内的工具，避免“模型看到的工具与执行用的工具不一致”。
6. **采样请求抢占（建议 P1，与第 1 条配套）**：仅做“轮末消费”的队列无法让插话立刻生效。可在 `ModelCallWrapNode` 的流式等待处引入一个抢占令牌：会话收到新输入（next-step 目标）时取消该令牌，节点随即中断当前 HTTP 流、保留已收到的部分内容并按同一请求续跑，从而让新输入在**本次采样**就进入上下文。落地：`nodes/modelcall.cpp` 的流等待点、`Session` 的输入投递路径。
7. **“已展示最终答案后不再延长本轮”规则**（建议 P2）：把“何时允许新输入并入当前轮”做成显式状态（对应 `MailboxDeliveryPhase`），避免用户已经看到答案后又被卷入同一轮的长尾工作；落地：`Session` 的输入队列状态 + `WireMessageQueueUpdate` 语义说明。

---

## 3. 会话与 LLM 上下文

### 3.1 codex：历史在内存，权威在 rollout

**上下文对象**

- `ContextManager` 持有 `Vec<ResponseItemEnvelope>`，每个 envelope 有 `item` 与可选 `metadata`（`CodexHarnessMetadata`）：工具输出的截断 token 上限、用户输入顺序号、MCP 归属检查点、压缩输出的标记、交付给模型的助手消息等。这些元数据让“历史里的同一段内容”在不同用途下可以有不同解释（模型输入 / UI 展示 / 重放重建）。
- `for_prompt(&modalities)` 在构造请求时按模型输入能力过滤（如不支持图片的模型剔除图片项），并支持“估计 token 数”（`estimate_token_count_with_base_instructions`）。
- 上下文片段都有**类型标记**：`ContextualUserFragment` trait 要求 `role()`、`content_kind()`（形如 `feature.name`）、`markers()`/`type_markers()`、`body()`，并提供 `matches_text()` 用于回扫识别。也就是说“注入的上下文”在历史里是**可识别、可统计、可替换**的，而不是一段裸文本。
- 世界状态（`context/world_state`）把模型可见的长期状态（AGENTS.md、环境、权限、协作模式、多代理模式、工具集、插件指令、持久模式、上下文窗口提示等）分成若干 **section**，每段独立 `snapshot()` 与 `render_diff(previous)`；轮次只把**差分**写进历史，并把新快照作为基线持久化（`WorldStateItem::patch`/`full`）。这是“上下文只增量变化”的具体实现（对前缀缓存友好，见 §6）。

**会话初始化与重放**

- `Session::record_initial_history` 按 `InitialHistory::{New, Cleared, Resumed, Forked}` 分三路：
  - `Resumed`：`apply_rollout_reconstruction` 重建历史、保留上下文（`retained_context`）、guardian 历史、上一轮设置、自动压缩窗口编号；若历史记录的模型与当前不同要发警告；并把 `TokenCount`/`TokenUsageRecord` 恢复出来，UI 一进来就能显示用量。
  - `Forked`：复制历史后按 `ForkPersistence` 决定落盘策略（`Copied`/`CopiedDeferred`/`Referenced{history_base, inherited_item_count}`），子代理的“继承前缀”可以留在祖先文件里，只写自己的增量。
- 重放的媒体处理是**一次性**的：`prepare_image_response_items(..., ImageResizeNoticeMode::Disabled, &InlineAttachmentStore)` 保证“重放不重新上传、不重新迁移”，并用断言守住“准备好前后条目一一对应”。

**源码要点**

- `history` 里除了正常历史还有 `guardian_history`、`retained_context`、`world_state_baseline`、`reference_context_item`、`latest_token_usage_record`、`reset_version`——它们都参与 `CompactedItem` 的持久化，所以“换窗/压缩后重启”能恢复成同一状态。
- 用户消息的 UI 信息与模型信息分开处理：`record_user_prompt_and_emit_turn_item` 从 `UserInput` 发出 UI 轮次条目（保留 `text_elements` 这类纯 UI 数据），而写进历史的是 `ResponseItem::Message`，两侧刻意不共用同一结构。

**实现细节（`core/src/context_manager/history.rs` 精读）**

`ContextManager` 的字段本身就是这套设计的规格（每个字段都有注释解释不变式）：

| 字段 | 作用与不变式 |
|---|---|
| `items: Arc<Vec<ResponseItemEnvelope>>` | 历史条目；**最短项在前**，快照共享同一 `Arc`，只有需要修改时复制——只读消费者（UI/插件/搜索）不会引起深拷贝 |
| `review_history` | 兼容用的旧审查历史（历史遗留文本检查点可据此恢复根指令） |
| `retained_context: Arc<RetainedContext>` | **与模型窗口解耦的宿主事实**（快照共享不可变状态），压缩替换模型窗口时它们仍在 |
| `guardian_review_mode` | 审查策略随历史快照一起移动，而不是另行捕获 |
| `retain_inherited_user_messages` | 派生/继承时是否保留用户消息 |
| `history_version` / `reset_version` | 前者在“历史被重写（压缩、回滚）”时递增；后者只记录**破坏性替换**，普通输入与压缩不改变它 |
| `user_message_revision` | 与压缩代次独立的用户输入/重置修订号 |
| `token_info` | 用量信息（随历史一起快照/恢复） |
| `reference_context_item` | 下一轮的差分基线；为 `None` 时下一轮做**全量上下文重注入** |
| world state 基线 | 世界状态差分对照点，压缩后可能只保留被保留的部分 |

- 文件头的那段注释是一组纪律：压缩替换的是**模型窗口**、快照必须把审查策略与保留事实**一起**带走、token 估算只计条目内容不计传输元数据、超长指令保留“不完整摘录”以便根审查、旧格式文本检查点可回填根指令备份。
- 历史里还有“用户授权消息”“守护上下文消息”等分类判定（`history_user_authorization.rs`、`event_mapping` 的 `is_contextual_user_message_content`），说明“模型可见历史”在内部是有类型区分的，而不是一锅文本。

### 3.2 agentxx：会话即权威，图状态只有影子

- `Session` 持有 typed 上下文（`std::vector<neograph::ChatMessage>`）与展示历史（`ViewMessage`），写入口收敛为 `appendMessages`/`replaceMessages`/`replaceMessagesFromJson`/`truncateMessages`，并维护 `messagesVersion` 单调递增；`llmMessagesJson()` 只在落库、`WireGetContext`、插件查询时惰性生成（带 dirty 标记与缓存）。
- 图状态不再持有 `messages` 通道：`agent/lib/include/agentxx/nodes/session_context.h` 提供 `sessionMessages`（只读借用，不得跨 `co_await`）、`appendSessionMessages`（追加 + 发 `{"channel":"messages"}` 写事件）、`updateMessagesMeta`（刷新只读影子通道 `xx_messagesMeta`）。
- 持久化是**节流写入**（追加消息时带 `persistThrottled=true`，约 3 秒窗口）+ 轮末权威保存（`base_agent.cpp` 注释明确“进程中途被杀最多丢一个节流窗口的增量”）。
- 附件在服务端按需读取（`dataUrl` 为空且 `pathOrUrl` 是本地路径时读盘 → base64），读盘与编码卸载到线程池；展示历史落库时会剥离 `dataUrl`（`stripAttachmentDataUrl`）只留路径与元数据，避免 SQLite 被 Base64 撑爆。

### 3.3 对比

| 维度 | agentxx | codex |
|---|---|---|
| 权威位置 | 会话对象（内存）→ 落库；图状态无消息 | 内存 `ContextManager` + rollout 文件（append-only 日志） |
| 注入上下文的表达 | 直接拼进系统提示词或作为普通消息追加 | 结构体片段 + 类型标记 + content kind，可回扫识别 |
| 长期状态 | 每轮重建系统提示词（`buildSystemPrompt`） | world state section 快照 + 差分写入历史 |
| 媒体 | 展示历史剥离 Base64；上下文保留引用 | 历史里存 `File` 引用 id，附件经 `AttachmentStore`，重放不重复上传 |
| 断开/续跑 | 图检查点 + 中断现场可恢复 | rollout 重放重建，含设置、用量、窗口编号 |
| 元数据 | 消息级 flags（`AutoInserted` 等） | envelope 级 metadata（截断上限、输入顺序、压缩标记、MCP 归属） |

**优点/缺点**

- agentxx 的“会话即权威 + 节流落盘”实现简单、内存占用可控（关联的 LRU 缓存策略见 §11），缺点是**重启后的等价性依赖若干约定**（节流窗口丢尾、`repairMessages` 读时修复、工具结果回填就地更新）。
- codex 的 rollout 是**只追加的事件日志**：任何状态都能从日志重建（`reconstruct_history_from_rollout`），测试与远端同步都靠它；代价是文件多、体量大，于是又需要压缩（`rollout/src/compression.rs`）、反向扫描（`reverse_jsonl_scanner`）、可寻址读取（`seekable_reader`）与 SQLite 索引（`state_db`、`session_index`、`rollout_reference_index`）来兜住查询性能。

### 3.4 可迁移到 agentxx 的设计

1. **注入片段的类型标记**（建议 P0）：给注入进模型上下文的消息加轻量来源标记（如 `source` 字段或内容前缀标记），使压缩、去重、UI 折叠、审计都能按来源处理，而不是依赖文本前缀匹配。落地：`conversation_types.h`（消息结构体加 `source`）、`nodes/session_context.h`、`middlewares/summarization.cpp`（按来源决定可否合并）。
2. **长期状态的差分更新**（建议 P1）：把系统提示词里那些“每轮都可能变”的段落（工作目录、权限状态、启用的插件、可用工具、记忆文件摘要）抽成带哈希的 section，只在其变化时向历史追加一条差分消息。这与 §6 的段落化是同一件事的两面，收益是前缀缓存命中与更好的可观测性。
3. **文件引用替代内联 Base64**（建议 P1，已有部分基础）：展示历史已经剥离 dataUrl；可再进一步：上下文里对图片/音频只存引用 id + 元数据，由 provider 层按需取用，避免长会话里重复携带大块数据。落地：`agentxx/agent/conversation_types.h` 的 `image_urls` 处理、`provider_common.h`。
4. **重放重建入口**（建议 P2）：把“从落库数据重建会话状态”做成显式函数（现在分散在 `SessionStore` 读取 + 图 checkpoint），并写明重建的资源边界（附件不重传、只读校验）。落地：`session_store.cpp` + `checkpoint_store.cpp` 的读路径统一入口。
5. **只读快照共享（建议 P2）**：codex 的历史条目放在 `Arc<Vec<...>>` 里，快照共享同一份、只在需要修改时复制，因此 UI/插件/搜索这类只读消费者不会引起深拷贝。agentxx 目前多处按值拷贝消息列表（且注释提醒“拷贝后引用会失效”），可考虑给会话上下文提供“只读快照句柄”（引用计数 + 版本号），把拷贝成本从“每条消息”降到“一次引用”。落地：`agent/context.h` 的 `messages()`/`llmMessagesJson()` 读取路径。

---

## 4. 持久化与崩溃恢复

### 4.1 codex：三层持久化 + 写者所有权

| 层 | 载体 | 职责 |
|---|---|---|
| 权威日志 | `{codex_home}/sessions/**/rollout-*.jsonl` | 逐行 append 的事件/条目（`RolloutLine{timestamp, ordinal, item}`），是重放的唯一真相 |
| 索引/元数据 | SQLite（`rollout/src/state_db.rs`、`state/` crate、`thread-store` 的本地实现） | 会话列表、标题、搜索、项目/分区/附件、迁移（`state/migrations`） |
| 线程存储抽象 | `thread-store` 的 `ThreadStore` trait（local/in-memory） | 把“线程”抽象成可替换后端：创建/读取/恢复/分叉/归档/删除/搜索/分页/元数据补丁 |

**源码要点**

- **写者所有权**：`rollout/src/writer_lock.rs` 用 `{codex_home}/thread-writer-locks/{thread_id}.lock` 做线程级互斥（`try_lock` 返回 `WouldBlock` 表示已有写者），另有一个 `.coordination.lock` 保护锁目录清理；第一次失败会尝试清理陈旧锁。这直接解决“两个进程同时写同一会话文件”的经典问题。
- **延迟耐久性**：`thread-store/src/store.rs` 的 `PersistContext` 明确区分语义——`ThreadPreparation`/`Standard` 必须同步落盘；`SubagentSpawn`（子代理派生可延后）、`TurnStart`、`SteeredUserInput` 允许后台排队，由后续耐久屏障兑现（`allows_background_persistence()`）。子代理继承历史的耐久屏障由调用方显式等待，避免“父已确认，子未落盘”。
- **截断与派生**：`thread_rollout_truncation` 提供 `truncate_rollout_after_turn_id` / `truncate_rollout_before_turn_id` 与 `truncate_rollout_to_last_n_fork_turns`；`SpawnAgentForkMode::{FullHistory, LastNTurns(n)}` 复用同一套语义。分支/回退（`revert_thread`、`app_backtrack`）在 TUI 侧也建立在这上面。
- **压缩与维护**：`rollout/src/compression.rs`（含 `RolloutCompressionTrigger`、`RolloutLineReader`、`existing_rollout_path`）、`maintenance.rs`、`persistence_metrics.rs`、`sqlite_metrics.rs` 说明长会话文件是被当成“需要定期整理的存储”管理的，而不是只写不整理。
- **守护进程恢复**：`core/src/session/daemon_recovery.rs` + `app-server/src/daemon_thread_recovery.rs` + `app-server-transport` 的 `daemon_recovery_file_path` 提供“服务端宕机后客户端重连并把未完成轮次恢复回来”的路径；`TurnInputMode::ContinueIfIdle{expected_previous_turn_id}` 就是为这种恢复准备的。

### 4.2 agentxx：单会话单库 + 图检查点

- `SessionStore`（`lib/src/agent/session_store.cpp`）为每个会话建一个目录 `{dataDir}/sqlite/sessions/{sanitizedSessionId}/`，库内四张表：
  - `view_message(seq, json, msg_id)`：展示历史，`msg_id` 单独成列并建索引（注释里明确写了原因：早期用 `json_extract` 导致长会话每次回填工具结果都要全表扫描 + 逐行 JSON 解析）；
  - `llm_context(id=1, json)`：LLM 上下文全文；
  - `meta(key, value)`：`sessionId`（目录名清洗后的原名恢复）、`title`、`lastActiveMs`、`msgIdCounter`；
  - `store(id, value)`：会话级共享存储（share store），内存只保留 3 条 LRU 缓存，首次访问只取 `max(id)` 作自增计数。
- 图状态由 `CheckpointStore` 落库（会话目录内），条目按轮次保存；`AgentRunner` 在中断处理期间会把中断现场序列化进状态通道，以便进程重启后 `resume_async`。
- 全局设置另有 `settings_db`（`agent/lib/src/util/settings_db.*`）KV 库；目录名清洗有 96 字符上限（`kMaxSessionDataDirLen`）并用 FNV-1a 哈希截断（`utilxx_base::path_sanitize`），规避 Windows `MAX_PATH`。
- 崩溃配平手段是“幂等建表 + 节流写 + 轮末权威写 + 读时修复（`repairMessages`）”，没有写者锁、没有重放重建、没有文件整理。

**实现细节（`thread-store/src/local/live_writer.rs`、`core/src/session/handlers.rs`）**

- 打开线程时三道关：先取**进程内**写者互斥（`store.live_writer_locks.lock(thread_id)`，同一进程内避免重复登记）、再 `ensure_live_recorder_absent(thread_id)`（确认没有遗留 recorder）、最后 `acquire_writer_lock(thread_id)`（跨进程锁文件）。之后 `insert_live_recorder(thread_id, recorder, rollout_id, history_mode, writer_lock)` 把“路径 + 历史模式 + 锁”作为一个整体登记，锁的生命周期与 recorder 绑定（recorder 掉了锁才放）。
- 恢复（`resume_thread`）要把历史模式、rollout 路径、cwd 三件事都定下来：历史模式优先取传入历史，其次是按 rollout 路径读到的记录，最后回落到存储默认；`cwd` 缺失直接报 `InvalidRequest`（本地存储的硬前提）；`RolloutRecorder::new_with_writer_lock` 复用同一把锁继续追加。
- 关闭路径（`shutdown_session_runtime`）有明确顺序：先置 `shutting_down` 停止新准入并取走启动预热句柄 → 关实时语音 → 中断所有任务 → 停 shell 快照预热 → 关 hooks → 排空异步 hook 结果通道 → 终止所有持久进程 → 关 code mode 会话 → 停 MCP 预热与运行时 → 跑 SessionEnd hooks → 通知扩展线程停止。顺序本身是“先停止产生新工作的东西，再回收资源”的体现。

### 4.3 对比

| 维度 | agentxx | codex |
|---|---|---|
| 结构 | 每会话一个 SQLite 库（4 张表）+ 图检查点 | JSONL 权威日志 + SQLite 索引 + 可插拔 `ThreadStore` |
| 写者冲突 | 未处理（假定单写者） | 线程级锁文件 + 陈旧锁清理 + 延迟耐久语义 |
| 恢复能力 | 会话状态 + 图检查点（中断可继续） | 从日志重建任意状态（含设置/用量/窗口编号） |
| 派生/分叉 | 无独立概念（会话之间互相独立） | 一等公民：`Forked`/`Referenced`/`LastNTurns`/截断 |
| 长会话整理 | 无 | 压缩、维护、反向扫描、索引、指标 |
| 版本/迁移 | 表结构幂等 + 列迁移（`ensureViewMessageMsgIdColumn`） | 独立 migration 目录 + 迁移测试（`state/migrations`、`state/goals_migrations` 等） |
| 崩溃语义 | “最多丢一个节流窗口（<3s）” | 耐久屏障按 `PersistContext` 分类，父确认子落盘 |

**优点/缺点**

- agentxx 的库结构小、查询直接（`msg_id` 索引解决的是真实痛点），缺点是**没有写者保护与整理机制**：多客户端 attach 或“同目录二次启动”会互相覆盖；长会话的上下文 JSON 也会越来越大。
- codex 的日志 + 索引分离能同时满足“重放正确”与“查询快”，还能做跨会话搜索/分页；缺点是组件多（日志、索引、压缩、迁移、锁），任何一处出错都需要专门的指标与恢复路径，工程成本高。

**实现细节（`Session` 节流落盘，`agent/context.h` + `agent/context.cpp`）**

- 节流常量写在头文件里：`kPersistThrottleMs = 3000`，两条独立计时线 `llmLastSaveMs_`（上下文）与 `viewLastPersistMs_`（展示历史），规则是“**首次触发立即落盘**，之后距上次落盘不足 3 秒的触发只登记待落盘，由下次结算或轮末 `saveLlmMessages()` 统一写”。
- 上下文与展示历史走两套写路径：上下文是整份覆盖（`llm_context` 单行 JSON），展示历史是**增量操作排队**（`PendingViewOp` 队列 + `enqueueViewPersist` / `flushPendingViewOps`），因此 `view_message` 表不是每次全量重写。
- 上游通过 `setStoreHooks(SessionStoreHooks)` 注入“怎么落盘”，`Session` 只负责“何时落盘”——存储实现（SQLite/内存/测试替身）与节流策略因此可以分开演进。
- 一个具体的一致性取舍写在注释里：**系统提示词属于每轮重建的派生内容，不参与节流落盘**（`replaceMessages(msgs, /*persistThrottled*/ false)`），由轮末权威保存写入；这样“每轮重建系统提示词”不会造成每轮一次的整份上下文写盘。
- 对照 codex：它的等价物是 `PersistContext::{ThreadPreparation, Standard, SubagentSpawn, TurnStart, SteeredUserInput}` 这套“为什么要落盘”的枚举 + 后台排队 + 耐久屏障；agentxx 目前只有“是否节流”一个布尔位，缺少“这次落盘属于哪类场景”的语义（排障时看不出是谁触发的写）。

### 4.4 可迁移到 agentxx 的设计

1. **写者锁（建议 P0，成本低收益高）**：为会话目录加锁文件（`{sessionDir}/.writer.lock`，`try_lock` 失败即拒绝第二次打开并给出明确错误），跨进程/跨实例都受益。落地：`SessionStore` 构造与 `BaseAgent` 会话打开路径。
2. **耐久语义分级（建议 P1）**：把落盘调用分成“必须同步”（用户输入、工具结果、轮末状态）与“可延后”（展示历史节流、内存统计），并在 API 上显式命名（如 `persistNow()` / `persistThrottled()` 已有雏形，可再补 `PersistContext` 风格的原因参数用于日志与指标）。
3. **结构版本号与迁移记录（建议 P1）**：`meta` 表加 `schemaVersion`，把迁移步骤写成有序列表并配测试（当前是就地 `ensureXxxColumn`）。落地：`session_store.cpp` 建表段 + `resource/history/` 记录。
4. **长会话整理（建议 P2）**：会话目录达阈值时提供整理入口（例如把 `llm_context` 的旧消息镜像到冷表/分文件、对 `view_message` 做分页表），避免单库无限增长。落地：`session_store.cpp` + `agent/script/` 维护脚本。
5. **派生/分叉语义（建议 P2）**：若未来要支持“从某轮分叉尝试”，建议先定义派生元数据（`forkedFromSessionId`、`baseMessageId/seq`、写入策略），不要靠复制目录。

---

## 5. 工具系统

### 5.1 codex：注册表 → 路由 → 编排器

三件东西分层：**注册表**（有哪些工具）、**路由**（这一轮模型看到哪些工具）、**编排器**（怎么执行一次调用）。

**注册表与路由**

- `ToolRegistry` 用 `IndexMap` 保存“工具名 → runtime”，注册时检测冲突；`CoreToolRuntime` 在通用 `ToolExecutor` 之上补了若干可选能力：`immutable_spec()`（规格可共享时避免重复分配）、`cached_code_mode_definitions()`、`wait_until_ready()`（如 MCP 服务就绪等待）、`mcp_server_name()`、`matches_kind()`（认领哪类 payload）、`telemetry_tags()`、`pre/post_tool_use_payload()` 与 `with_updated_hook_input()`（hooks 改写参数）、`create_diff_consumer()`（工具参数流式差分）。
- `ToolRouter` 是“本轮请求视图”：`model_visible_specs`（发给模型的 schema 数组，`Arc<[ToolSpec]>` 共享）、`tool_mode`（普通 / CodeMode / CodeModeOnly）、CodeMode 下的嵌套工具名映射、命名空间信息、`can_manage_children`（只有子代理管理工具全部暴露时才为真）。工具可以有**命名空间**（如 `collaboration.*`），扁平化字符串只在 hooks/遥测等老边界使用（`flat_tool_name` 的注释写明了这一点）。
- 工具发现与建议：`DiscoverableTool`、`ToolSuggestCandidates`、`tool_search` 处理器让模型先搜索再加载工具，而不是把全部 schema 塞进请求（配套测试 `scenarios_tools_namespace_budget.rs`、`tool_search.rs`）。

**编排器：审批 → 沙箱 → 尝试 → 升级重试**

`core/src/tools/orchestrator.rs` 的文件头注释就是它的规格：

```
approval → select sandbox → attempt → retry with an escalated sandbox strategy on denial
```

- `run_attempt` 里先建立网络审批上下文（`begin_network_approval`），把沙箱尝试与网络代理参数一起交给工具；失败时结算“延迟的网络审批”（`finish_deferred_network_approval`）。
- 审批有缓存：`sandboxing.rs` 的 `ApprovalStore` 用“序列化后的 key”缓存 `ApprovedForSession`，`with_cached_approval` 支持**多 key**（apply_patch 会同时改多个文件），全部命中才跳过询问；批准后逐个 key 记账。

**并行执行**

- 工具自声明 `supports_parallel_tool_calls()`；`ToolCallRuntime` 持有一个 `Arc<RwLock<()>>` 作为闸门：并行安全的工具取读锁（可同时进行），声明独占的工具取写锁（互斥）。`parallel.rs` 里还有工具计时守卫（`ToolCallTimingGuard`：开始时间、执行开始时间、归属会话/轮次），以及 `AbortedToolOutput` 表达“被取消的工具输出”。

**输出契约与截断**

- 工具返回 `ToolOutput` → `AnyToolResult` → 通过 `to_response_item(call_id, payload)` 变成历史条目；错误分两类：`FunctionCallError::Fatal`（升级为致命错误，结束轮次）与 `RespondToModel`（变成给模型看的错误文本）。工具还可以提供 `tool_result_metadata`（附加到 envelope 元数据）。
- 工具输出有统一的截断策略（`TruncationPolicy` + `formatted_truncate_text` / `with_serialization_allowance`），并且**策略在写入历史时记录**（`history_truncation_token_limit`），重放时保持一致。
- hooks 在工具前后各有一层：`run_pre_tool_use_hooks` 可以改写参数（`with_updated_hook_input`），`run_post_tool_use_hooks` 可以否决结果（`on_tool_result_accepted` 只在其后触发）。

**实现细节（`tools/registry.rs`、`tools/router.rs`、`tools/parallel.rs`）**

- 注册表的治理规则比“一个 map”严格得多：`tool_policy.allows(&tool_name)` 先做策略过滤；默认命名空间下 `exec_command`/`shell_command` 属于**保留名**，外部注册会被拒绝并记录冲突（避免插件覆盖内置执行工具）；重复名不覆盖而是记录 `first_collision`，供上层决定是否报错；删除用 `shift_remove`（保持剩余工具的插入顺序，而不是 `swap_remove` 的交换式删除）。
- 每个工具带 `ToolExposure`：`deferred` 的工具不进模型可见 schema，而是按命名空间汇总出描述（`deferred_tool_namespaces`），由 `tool_search` 在需要时加载——这是“工具 schema 预算”的具体落点。
- `build_tool_call(item)` 把响应项映射为调用：`FunctionCall` → `ToolPayload::Function`；`ToolSearchCall`（且 `execution == "client"`）→ `ToolPayload::ToolSearch`（参数解析失败会变成给模型的错误而非崩溃）；`CustomToolCall` → `ToolPayload::Custom`；其他返回 `None`（不是工具调用）。
- `tool_supports_parallel` 对未注册工具返回 `false`（保守默认）；`dispatch_tool_call_with_state` 只负责组装 `ToolInvocation`（session + turn + step_context + cancel token + tracker + call_id + tool_name + source + payload）后交给注册表 `dispatch_any_with_state`。
- 取消路径写得很细：派发任务在 `tokio::select!` 里与取消令牌竞争；若调用已经到达终态或派发任务已完成，就**取回结果**而不是 abort；否则 `dispatch_handle.abort()` 并合成 `AbortedToolOutput`（`exec_command` 的文案是 `Wall time: X seconds\naborted by user`，其它工具是 `aborted by user after Xs`），同时上报 `notify_tool_aborted` 与调用轨迹的 `result_ready`。
- 计时守卫只在**直连调用**上生效（`ToolCallSource::Direct`/`DirectPlaintextMessage`）：Code Mode 的嵌套调用被显式跳过，避免嵌套事件被误读成独立的工具延迟；`finish()` 一次性产出 dispatch/handler/total 三段耗时并记录一条 `codex.tool_call` 结构化日志。

### 5.2 agentxx：`XXToolBase` + 图分发 + 中间件协同

- `XXToolBase`（`lib/include/agentxx/tools/tool.h`）把工具能力做成**构造期开关**：
  | 开关 | 作用 |
  |---|---|
  | `autoSummaryOutput` | 输出超过阈值时自动摘要（阈值取 `AgentConfig::toolcallSummaryLimitOutputLength`） |
  | `canDelayLoad` | 初始只把工具名等简短信息放进系统提示词，模型用 `tool_skill_search` 检索后再加载全量定义 |
  | `maxRetry` | 执行抛异常时最多重试 `1 + maxRetry` 次 |
  | `repeatCallCheck` | 同一 llm↔tool 交替链内连续相同调用超过阈值时，经 permission 总线询问用户 |
- 注册来源有三个并汇集到同一张工具表：`initTools()`（子类）、中间件自带工具（`initMiddlewareTools` 在 `initTools()` 之后收集）、插件工具（`plugins/tool_registry.cpp`，含冲突检测与静态工具名集合）；每个工具还能 `createSummarizationToolHandle()` 提供自己的压缩策略。
- 执行在 `ToolcallWrapNode`（`lib/src/nodes/toolcall.cpp`）：解析参数（严格数值解析、拒绝十六进制浮点等）、注入 `sessionId` 以取得会话取消令牌、顺序执行（源码留有 `// TODO: 真正并行`）、取消时补占位结果；输出超限自动摘要。
- 参数错误约定明确：参数检查失败抛 `std::invalid_argument`、运行期错误抛 `std::runtime_error`，由 `ToolcallWrapNode` 统一格式化为 `[Exception aborted: <msg>]`。
- 启动期做工具 schema 合法性检查（`checkToolSchemaValidity`）：`enum` 元素必须是标量、`array` 必须带 `items`、带 `items` 时 `type` 必须是单一 `"array"`——原因是严格网关（Gemini 等）会直接 400，且报错含糊。
- 权限由**插件声明**而非硬编码：插件注册工具后经 `agentxx.agent.permission` 表声明作用域（读/写）、目标来源（无/路径/文本）与目标参数名；模式类工具（glob/grep/list）另有 `check_paths` 三态批量查询（DENY/ALLOW/ASK），逐路径复核，避免 `**` 绕过子目录拒绝规则。

**实现细节（`lib/src/nodes/toolcall.cpp` 精读）**

`baseRun`（工具分发主流程）与 `execTool`（单个工具执行）的分工是硬性的：

1. **取认定稿的调用声明**：从会话上下文末尾往前找**最后一条带 `tool_calls` 的 assistant 消息**，把 `declaredToolCalls` 拷贝出来后立刻释放引用（后续追加消息会让引用失效——源码里专门写了这条注释）。
2. **中断现场复用**：进入前先读 `graphDataKey_interruptToolcallCache`（上一轮中断时暂存的已完成工具结果，key 为 `tool_call_id`）并清空该项；命中的调用直接返回缓存内容、`durationMs = 0`，这就是“中断恢复后不重复执行已完成的工具”。
3. **重复调用检测**：以当前 assistant 结尾的 `llm ↔ tool` 交替链内统计重复 key（`makeRepeatCallKey` = 工具名 + 参数长度 + 参数哈希），达到阈值（`AgentConfig::toolcallRepeatCheckThreshold`，默认 5）的 key 集合交给 `execTool`，只有**命中且该工具启用了 `repeatCallCheck`** 才会询问用户。
4. **参数注入**：`Json::parse(tc.arguments)` 成功后注入 `sessionId`（会话取消令牌与权限判定都依赖它）与 `tool_call_id`（供子代理等工具做中断 `resultId`）；随后 `execTool` 内部先做 `autoFixArgsType`——按工具 JSON Schema 尽量把类型纠正过来（string↔number/bool、string→字符串数组等），失败只记日志不阻断。
5. **权限与重复确认**：经会话总线请求 `service.permission.check`（`ToolPermissionCheck` 主题，超时 0 = 不限时；无权限服务时默认放行），返回不允许则直接以 `[Permission denied]` 作为工具结果。重复调用确认走 HIL 中断：`InterruptHandleArg.name = "repeat_toolcall"`、`resultId = tool_call_id`、UI 用预设确认卡片；**解析响应必须按自身 `resultId` 下钻**（源码注释点明：直接对顶层取 `allow` 会恒取不到，会让用户点“允许”也被拒绝），拒绝时返回 `[Repeated call denied by user: ...]` 让模型换方式。
6. **执行与重试**：执行前再检查一次取消令牌（权限询问可能已经挂起过）；`maxRetry` 从工具 `extra` 里读出；循环里区分取消/中断异常（`CancelledException`/`NodeInterrupt` 直接重抛）与普通异常（达到 `maxRetry` 才重抛）；插件工具与静态工具走同一入口，动态工具（热插拔）额外经 `toolRegistry->find` 取 `shared_ptr`，与插件 inflight 计数配合保证 `dlclose` 前代码段不被卸载。
7. **异常与取消的处理位置是分开的**：执行体用 `catchErrorAsync` 包装（放行取消与中断、把 `operation_aborted` 转成 `CancelledException`），普通异常就地写成 `[Exception aborted: msg]` 作为工具结果，让模型自己看到失败原因并继续。
8. **超长输出改存**：`autoSummaryOutput` 且长度达到阈值时，把全文写入会话 share store，返回带 ID 与行号的定位串（分两种形态：按行切分可给“显示 [1, N] / 隐藏 [N+1, M]”，内容集中在一行时只给全文 ID 与总行数）——模型可用 `agentxx_share_store` 分页取回。
9. **中断与取消的收尾**：
   - 中断（工具内部 `requestInterrupt`）：把**已完成的**工具结果 JSON 存进 `graphDataKey_interruptToolcallCache`，把当前上下文（`llmMessagesJson()`）交给 `throwNodeInterruptBase`，图挂起等待外部应答后 resume；
   - 取消：为未完成的调用补齐 `[User canceled]` 占位（`AutoInserted` 标记），把已完成结果与占位一起 `appendSessionMessages` 落进会话（会话是唯一权威，图回滚不会丢），然后重抛取消异常——这保证“每条 assistant 声明的 tool_call 都有对应结果”。
10. **中间件钩子在 START/BASERUN 出错时的处理**：`onHandleStartError`/`onHandleBaseRunError` 会按“错误是否属于当前节点”决定是否补插一批 `[<阶段>/Exception aborted: ...]` 结果，保证消息顺序与角色序列仍然合法。
11. **并发现状**：代码先把所有 `tool_calls` 组织成 `std::vector<asio::awaitable<ChatMessage>>`，但随后是**逐个 `co_await`**（源码注释 `// TODO: 真正并行`）——结构上已经为并行留好了位置，目前仍是顺序执行。
12. **调试期自查**：Debug 构建下若“最后一条 assistant 的 tool_calls 已全部有结果”，会记一条警告（说明存在异常路径重复调度），但不跳过执行，避免掩盖真实问题。

### 5.3 对比

| 维度 | agentxx | codex |
|---|---|---|
| 分发 | 图节点（`ToolcallWrapNode`）+ 中间件包裹 | `ToolRegistry` + `ToolRouter` + `ToolOrchestrator` |
| 并发 | 顺序执行（源码 TODO） | 工具自声明并行能力，读写闸门控制 |
| 审批/沙箱 | 权限中间件统一判定 + 声明式询问卡片 | 编排器内建：审批（带缓存）→ 沙箱选择 → 失败升级重试 |
| 输出处理 | 统一截断/自动摘要 + 异常约定 | 输出契约 + 截断策略随历史记录 + hooks 可否决 |
| 工具发现 | `canDelayLoad` + `tool_skill_search` | `DiscoverableTool` + `tool_search` + 命名空间预算 |
| 参数流式差分 | 无（工具参数只在完成后使用） | `ToolArgumentDiffConsumer`（流式显示参数变化） |
| 错误分类 | 异常类型约定（invalid_argument/runtime_error） | `Fatal` 与 `RespondToModel` 两类显式语义 |
| 扩展工具 | 插件工具 + 中间件工具，统一注册 | MCP 工具（按 step 刷新）+ 扩展 crate 工具 + 动态工具（`DynamicToolSpec`） |

**优点/缺点**

- codex 的编排器把“审批、沙箱、重试、网络”四件事收敛到一处，任何工具都自动获得一致的安全语义；代价是抽象层多（`ToolRuntime`/`Sandboxable`/`Approvable`/`SandboxAttempt` 等 trait 组合）。
- agentxx 的工具开发体验更直接（一个 `XXToolBase` 子类 + 构造开关），缺点是**安全语义分散**：审批在中间件、沙箱在前缀规则、重试在构造参数、并行还没有，新增工具时需要自己判断该接哪些。

### 5.4 可迁移到 agentxx 的设计

1. **工具级并行声明 + 闸门**（建议 P0）：给 `XXToolBase` 增加 `supportsParallel()`（默认 `false`），`ToolcallWrapNode` 在同一 assistant 消息的多个 `tool_calls` 之间，按“读并行/写独占”的规则并发执行（`asio::experimental::channel` 或 `co_spawn` + 收集）。收益明显（多个只读工具并行检索/读文件），风险可控（默认关闭）。
2. **输出契约与截断策略随消息记录**（建议 P1）：把“截断上限/策略”与工具结果一起写进消息元数据，压缩与恢复时按同一策略处理，避免长会话中同一结果在不同阶段被截成不同长度。
3. **错误分类显式化**（建议 P1）：在 `[Exception aborted: ...]` 之外，区分“参数错误（可让模型改正重试）”与“致命错误（应结束本轮）”；现在两者都变成一条工具结果，模型可能反复重试必然失败的操作。
4. **审批缓存**（建议 P1）：`repeatCallCheck` 与权限询问目前每次都问；可引入“同一会话内已批准的决定摘要”缓存（键 = 工具 + 规范化参数/路径），命中即跳过询问并记录命中原因。
5. **工具发现与命名空间预算**（建议 P2）：`canDelayLoad` 已经是很好的基础，可再加“按命名空间分组 + 每轮只暴露相关组”的机制（例如按当前工作目录与规划选择组），并为“工具 schema 占用 token 预算”加指标。
6. **参数流式差分**（建议 P2）：工具参数在流式生成时可增量展示（如 apply_patch 的补丁片段、写文件的路径），改善长参数工具的等待体验；落地在 `modelcall.cpp` 的流事件处理与 UI 层。
7. **注册表治理规则（建议 P1）**：借助 codex 的三条硬规则补齐 agentxx 的工具注册表——① 内置/保留名不允外部覆盖（或覆盖必须显式确认）；② 重名不覆盖而是记录冲突并告警（现在 `tool_registry.cpp` 有冲突检测，可再补“首个冲突”记录与对上层可见的错误码）；③ 为工具加“暴露等级”（始终可见 / 延迟搜索可见），与 `canDelayLoad` 对接，便于按命名空间或来源分组控制 schema 预算。
8. **工具调用分段计时与来源标记（建议 P2）**：为每次工具调用记录“派发等待 / 执行 / 总耗时”三段与来源（模型直连 / 插件内部 / 子代理），仅在直连调用上计时（避免嵌套重复计数）；这些数据既用于 UI 展示，也是排查“用户觉得卡”的第一手证据。
9. **并发工具的取消语义（建议 P1，与第 1 条配套）**：并行化之后必须同时定义取消行为——已完成的结果保留、未完成的补占位、正在执行的尽量立刻中止（codex 的做法是 `select!` 竞争 + `abort()` + 合成 `AbortedToolOutput`，并在调用已到达终态时改取结果）。

---

## 6. 系统提示词、请求组装与世界状态

### 6.1 codex：段落化提示词 + 世界状态差分

**提示词来源分四层**

1. **基础指令**：优先级写在源码注释里——`config.base_instructions` 覆盖 > 历史 `session_meta.base_instructions` > 按当前模型模板渲染（`codex_prompts::render_model_instructions(&model_info)`）。`get_prompt_base_instructions` 在请求侧做“只影响本次请求”的裁剪（如关闭 `update_plan` 时移除对应说明），不改动持久化内容与派生继承。
2. **prompts crate 的静态段**：`model_instructions`、`permissions_instructions`、`multi_agent_instructions`、`update_plan_instructions`、`review_request`、`SUMMARIZATION_PROMPT`/`SUMMARY_PREFIX`、`guardian_instructions`、`realtime`。
3. **上下文片段**（`core/src/context/*`，50+ 个结构体）：每个片段实现 `ContextualUserFragment`，带角色、内容分类与标记，例如 `EnvironmentContext`、`CurrentTimeReminder`、`DeveloperInstructions`、`ManagedDeveloperInstructions`、`PluginsInstructions`、`RecommendedPluginsInstructions`、`HookAdditionalContext`、`ImageResizeNotice`、`TurnAborted`、`TokenBudgetContext`、`ApprovedCommandPrefixSaved`、`NetworkRuleSaved`、`InterAgentMessage`、`SubagentNotification`、`Guardian*` 系列（约 15 个）。
4. **world state section**（`core/src/context/world_state/*`）：把“长期模型可见状态”做成可快照、可差分的段——`agents_md`、`environment`、`environments_instructions`、`permissions`、`tools`、`model`、`collaboration_mode`、`plugins_instructions`、`multi_agent_mode`、`multi_agent_usage_hint`、`context_window_guidance`、`persistent_mode`、`compact_permissions`、`managed_developer_instructions`、`realtime`。每段实现 `snapshot()` 与 `render_diff(previous)`，`ErasedWorldStateSection` 还提供 `matches_legacy_fragment`/`matches_retained_fragment` 用于**回扫识别旧格式片段**（这是演进过程中必不可少的兼容层）。

**组装规则**（`build_initial_context_with_world_state`）

- 开发者片段默认**合并成一条 developer 消息**；声明 `requires_separate_message()` 的片段单独成条（守护/审查类就是这样隔离的）；用户上下文片段合并成一条 user 消息（`build_rendered_message` + `merge_contextual_fragments` 只合并相邻同角色且可合并的片段）。
- 顺序有讲究：模型切换指令插到开发者段最前；多代理模式在开发者段之后；守护策略在守护会话里单独成条；托管开发者指令单独成条；token 预算上下文单独成条（且启用 `Feature::TokenBudget` 时才出现）。
- 差分模式：只有第一个真实用户轮注入全量上下文并建立 `reference_context_item`；之后每轮只发 `update_world_state()` 算出的差分（`WorldStateItem::patch`），并把新快照写进 rollout。历史里的旧片段会通过 `matches_*` 被识别并保持“同一段状态只有一个当前值”的语义。

**其他**

- AGENTS.md 由 `agents_md_manager` 管理：每步 `refresh(config, environments)`（可多环境），产出 `LoadedAgentsMd` 与警告；`instruction_sources()` 暴露来源路径列表供客户端展示。skills 有类似的加载/缓存与“隐式调用检测”（`detect_implicit_skill_invocation_for_command`）。
- 工具说明也参与预算：命名空间信息（`TurnToolNamespacesInfo`）与 `scenarios_tools_namespace_budget.rs` 表明仓库把“工具 schema 占用上下文”当成需要测试约束的资源。

### 6.2 agentxx：注册表式提示词，每轮重建

- `AgentPrompt`（`lib/include/agentxx/agent/prompt.h`）三块：
  - `systemPrompt`：主体文本（写在 `prompt.cpp`，头文件只声明成员，改文本不牵连编译单元） + 会话级占位符 `${work_dir}`/`${temp_dir}`/`${session_id}`；
  - `appendSystemPrompts`：**按键的附加段**（键为插件或功能标识，如 `planning`、`skill`、`codegraph`、`git-worktree`），插件经 prompt 接口表注入/更新，卸载时按备份恢复；最终 system 消息 = 主体 + 按键字典序拼接的附加段 + 动态段（skill/memory）；
  - `toolPrompt`：按工具名覆写 `depict`（描述）与 `args`（参数说明）。
- `BaseAgent::buildSystemPrompt(sessionId)` 每轮重新拼装（供 modelcall 节点与 UI 的上下文查看复用）；工具定义来自 `XXToolBase::get_definition()`，并把 `toolPrompt` 覆写合入；`canDelayLoad` 的工具只放简短信息。
- 运行时状态（工作目录、权限状态、启用插件等）不在系统提示词里做差分，而是通过工具返回值、Tip 消息、`appendSystemMessage` 动态段、或直接拼接体现；没有“状态快照 + 回扫识别”的概念。

### 6.3 对比

| 维度 | agentxx | codex |
|---|---|---|
| 提示词组织 | 主体 + 按键附加段（插件可写）+ 工具覆写 | 基础指令 + 静态段（crate）+ 上下文片段（结构体）+ world state 段 |
| 会话变量 | `${work_dir}`/`${temp_dir}`/`${session_id}` 占位符替换 | 片段自带渲染逻辑（环境片段等），值来自 step 快照 |
| 更新方式 | 每轮重建系统消息 | 首轮全量 + 之后差分（并持久化新基线） |
| 可识别性 | 无类型标记（靠文本内容与位置） | 片段有角色/分类/标记，可回扫匹配 |
| 顺序规则 | 主体 → 附加段（字典序）→ 动态段 | 有明确优先级（模型切换最前、托管指令与预算上下文独立成条等） |
| 工具预算 | `canDelayLoad` + 延迟检索 | 命名空间 + `tool_search` + 预算测试 |

**优点/缺点**

- agentxx 的提示词表（按键的附加段 + 工具覆写）在“插件各写各的”这一场景下很好用，插件卸载还能按备份恢复；缺点是**每轮重发全量系统提示词**，前缀缓存在提示词内容变化时会整体失效，且注入内容无法被后续阶段识别。
- codex 的差分方案对缓存友好、可审计、可回扫，但引入了持续成本：每段都要写 `snapshot`/`render_diff`/`matches_*` 三个方向，还有“基线持久化”的一致性要求（历史与基线必须一起更新，`replace_compacted_history` 就是为此把世界状态项与替换历史一起写）。

### 6.4 可迁移到 agentxx 的设计

1. **注入内容的类型标记**（建议 P0，与 §3.4 第 1 条同一项）：为附加段与动态段定义稳定的“来源键 + 内容分类”，使压缩、去重、审计、UI 折叠都能按来源处理。
2. **可变段落的差分注入**（建议 P1）：把系统提示词里随会话变化的段落（工作目录、权限状态、启用插件与工具、记忆摘要）从 system 消息里移出，改成“首轮全量注入 + 变化时追加一条带标记的差分消息”。落地：`BaseAgent::buildSystemPrompt` 拆成“稳定段”与“易变段”，后者经 `appendSessionMessages` 注入并记录基线。
3. **提示词优先级显式化**（建议 P1）：把“基础指令 > 历史继承 > 模板渲染”的优先级写进 `AgentPrompt` 的取值逻辑与文档，避免插件、配置、训练数据三处同时改写同一段时没有明确规则。
4. **顺序与独立成条规则**（建议 P2）：为附加段定义少量固定槽位（如 `env` / `policy` / `capabilities` / `plan`），而不是纯字典序拼接，让提示词结构与模型预期稳定。

---

## 7. LLM 流式、适配器与网络层

### 7.1 codex

**请求对象与流**

- `Prompt`（`core/src/client_common.rs`）就是“一次采样的请求”：`input`（历史条目）、`tools: Arc<[ToolSpec]>`（共享的 schema 数组，避免每轮克隆）、`parallel_tool_calls` 标志、`base_instructions`、可选 `output_schema` 与 `output_schema_strict`。注意 `parallel_tool_calls` 是**请求级开关**，与工具自身的并行声明配合。
- `ResponseStream` 是 `Stream<Item = Result<ResponseEvent>>`，额外带两个控制通道：`interrupt: Option<oneshot::Sender<()>>`（通知上游中断）与 `consumer_dropped: CancellationToken`（`Drop` 时取消，避免消费端提前退出后上游继续跑）。
- 图片细节按模型能力归一化（`normalize_image_details`）：不支持 `original` 就降级为默认细节；`use_responses_lite` 的模型直接去掉 detail 字段——**同类能力差异在请求组装处统一处理**，不散落在各调用点。

**模型与 provider 层**

- `core/src/client.rs`（2808 行）是会话级 `ModelClient`/`ModelClientSession`：会话内复用一个 session（缓存 WebSocket 与粘性路由状态），带 `x-codex-routing-hint`、`x-codex-turn-metadata`、`x-codex-installation-id` 等头；`build_model_client_beta_features_header` 把开启的实验特性拼成 `x-codex-beta-features`。
- 独立的 `codex-api` crate 管 HTTP/SSE 细节（`endpoint`/`requests`/`sse`/`api_bridge`/`images`/`files`/`rate_limits`/`safety_buffering`），`model-provider` crate 管 provider 定义与鉴权（含 Amazon Bedrock、工作区路由、bearer/combined auth）；`models-manager` 管模型目录与刷新策略（`RefreshStrategy::{OnlineIfUncached, Offline}`，子代理固定离线，避免每个子代理都拉目录）。
- 安全与合规相关组件是独立 crate/模块：`safety_buffering.rs`、`attestation.rs`、`responses-api-proxy`（本地代理）、`websocket-fallback`（连接降级）、`otel` + `otel-trace-websocket`（链路追踪）、`realtime-webrtc`（实时语音通道）。

**源码要点**

- `run_sampling_request` 里把“采样 + 工具执行”绑在一次 step 上：先构造请求（`client_session.new_session()` 复用）、流式处理（`ResponseEvent` 分派到 UI 事件与历史记录）、工具分发给 `ToolCallRuntime`，最后返回 `SamplingRequestResult{needs_follow_up, last_agent_message}`。
- 重试策略集中在 `responses_retry.rs`（含 `retry_after` 处理与测试），重试与“重置客户端会话”是两件不同的事：`AGENTS.md` 明确提醒“不要无谓调用 `reset_client_session`，让增量检查逻辑决定是否复用上次请求”。

**实现细节（`core/src/client.rs` 精读，2808 行）**

- 两个对象、两条生命周期：
  - `ModelClient` 是**会话级**：只持有整个会话稳定的东西（`thread_id`、provider、工作区路由、会话来源、originator、verbosity、以及一批特性开关与 `disable_websockets: AtomicBool`、缓存的 WebSocket 会话）。源码注释明确写了“不要让它持有完整 `Config`，轮次级设置一律按调用点显式传入”。
  - `ModelClientSession` 是**轮次级**：轮内多次请求复用同一条 Responses WebSocket 连接；缓存上一次完整请求，只有在“当前请求是上次请求的增量扩展”时才复用增量载荷。注释里还有一条硬约束：**跨轮复用会把这个轮的粘性路由令牌带进下一轮，违反客户端/服务端约定**，因此每轮新建。
- 增量复用判定用 `responses_request_properties_match`，写成了**穷尽解构**（`input`、`stream_options`、`client_metadata`、`access_programs` 被显式排除并各自注明原因），这样以后给请求结构体加字段会**编译期强迫**作者做一次“要不要参与复用”的决定，而不是悄悄沿用旧判定。
- 输入项比较用 `response_items_equal_ignoring_internal_metadata`：允许“晚到的工具结果元数据”更新已有输出（增量载荷无法携带这种更新），但内容差异一律视为不可复用；比较前会清掉内部聊天消息元数据。
- 粘性路由：服务端在轮次开始时通过 `x-codex-turn-state` 响应头下发，客户端保存在 `OnceLock<String>` 里，**同一轮内所有请求（重试、增量追加、续跑）原样回传**，认证主体变化时清空以便新主体拿到新的路由状态。
- WebSocket 失败回退是**会话级**的：`force_http_fallback` 一旦触发，本会话后续所有轮次都走 HTTP（避免每轮都尝试并失败一次）；`WebsocketSession::reset(reason)` 在重置时保留 `auth_owner_generation`，并把“丢失原因”跨重连/跨轮保留到下一次发送，供后端指标区分“首次发送”与“重发”。
- 请求侧还有 `prompt_cache_key` 与 `responses_session_id`（由响应元数据派生）用于提示词缓存，以及每轮按需构建的 beta 特性头、安装 id 头、子代理头、证明（attestation）头等。

### 7.2 agentxx

- provider 层是注册表（`agent/lib/src/agent/model_registry.cpp`）：OpenAI 兼容、Anthropic 等 provider 经 `neograph::llm::Provider` 接口接入，另有限流装饰器（`neograph::llm::rate_limited_provider`）与 schema provider（把工具 schema 做整形以适配严格网关）。
- `ModelCallWrapNode`（`lib/src/nodes/modelcall.cpp`）承担的主要工作：
  - **发送前的消息整形**：合并连续 user 消息、去重/修正重复 `tool_call` id、清理悬挂 tool_calls 等（注释里逐条写明原因，例如“历史中因重试/中断/客户端竞态出现 user+user”）；
  - **重试**：`llmMaxRetry` 上限；退避 = `retry*3` 秒 + 命中限流关键词（`429`、`rate limit` 等，用 Aho-Corasick 匹配）再追加 `retry*5` 秒；重试计数**不因部分输出重置**（避免“部分输出 → 重置 → 无限重试”）；
  - **部分输出保留**：已有 ≥512 字符输出时，重试前插入一条 assistant 消息保存已有内容，保证消息角色交替合法；
  - **取消埋点**：进入 LLM 调用前检查取消令牌（重试路径也检查）。
- 流式 delta 经 EventBridge 分配会话级 seq 后 `sendToPeer`；UI 提示（如“N 秒后自动重试”）走 `WireDelta::MessageUITip`。
- 会话级模型选择可在轮次间切换（`selectModel(sessionId, modelName)`），模型目录与默认值在 `modelsConfig`/`AgentConfig` 中。

### 7.3 对比与迁移建议

| 维度 | agentxx | codex |
|---|---|---|
| provider 抽象 | 注册表 + 装饰器（限流/整形） | `model-provider` + `codex-api` 分层，provider 能力信息进 `ModelInfo` |
| 能力协商 | 主要体现在 provider 实现内部 | `ModelInfo` 驱动（图片细节、响应格式、服务等级、上下文窗口等） |
| 重试 | 节点内重试：指数退避 + 限流加时 + 部分输出保留 | 独立 `responses_retry`，与客户端会话复用解耦 |
| 中断 | 取消令牌 + 取消即抛；部分输出已写会话 | `ResponseStream` 中断通道 + `consumer_dropped` 令牌 |
| 请求复用 | 每次调用新建请求 | 会话级 `ModelClientSession` 缓存连接与路由粘性 |

**可迁移到 agentxx 的设计**

1. **模型能力信息集中化**（建议 P0）：把“是否支持图片细节/并行工具调用/工具 schema 严格模式/上下文窗口/服务等级”等整理到模型条目（`modelsConfig`），由节点读取并调整请求，而不是让每个 provider 各自判断。落地：`agent/lib/include/agentxx/agent/model_registry.h` 的模型描述 + `nodes/modelcall.cpp`。
2. **消费端退出的取消传播**（建议 P1）：LLM 流被上层放弃时（例如 UI 关闭、轮次被替换），当前依赖调用方显式取消；可仿照 `ResponseStream::Drop` 用 RAII 触发取消，避免上游连接悬挂。
3. **请求级并行开关**（建议 P1，与 §5.4 第 1 条配套）：`parallel_tool_calls` 作为请求字段显式传递，只有工具表里存在并行安全工具时才置位。
4. **重试策略独立成模块**（建议 P2）：把退避、限流识别、部分输出保留三件事从 `modelcall.cpp` 中抽出为可测试策略对象，便于按 provider/模型覆盖（如某些网关没有 `retry-after`）。
5. **客户端状态分层与粘性令牌（建议 P2）**：codex 把客户端拆成“会话级”（认证、provider、传输回退状态）与“轮次级”（连接、上次请求、粘性路由令牌）两层，并明确规定粘性令牌**只在同一轮内回传、绝不跨轮**。agentxx 若接入支持会话亲和（session affinity）或提示词缓存的网关，建议同样显式区分这两层状态，并在日志里记录令牌的生成/使用/清空时机，避免把“跨轮复用”这类错误变成难查的路由异常。

---

## 8. 上下文压缩与上下文窗口

### 8.1 codex：四个时机 + 两种实现 + 换窗

**时机**（都在 `core/src/session/turn.rs` 与 `compact.rs` 里显式建模）

| 时机 | 触发 | 说明 |
|---|---|---|
| 轮前 | `run_pre_sampling_compact` | 在记录本轮输入之前预判是否会超限，先压缩再写入新输入 |
| 轮中 | token 到顶或显式换窗请求 | 用 `InitialContextInjection::BeforeLastUserMessage`，把初始上下文注入到“最后一条真实用户消息之上”，随后继续本轮 |
| 轮后 | 配置百分比阈值 + `model_post_turn_compact_threshold_percent` | 只在没有 pending input、未被取消时执行；失败只告警不影响已完成的轮次 |
| 手动 | `/compact`（TUI 命令） | 走 `CompactTask` |

**实现**

- 本地实现：走摘要提示词（`SUMMARIZATION_PROMPT`/`SUMMARY_PREFIX`），`COMPACT_USER_MESSAGE_MAX_TOKENS = 20_000`；压缩结果作为 `CompactedItem` 持久化，包含 `replacement_history`、`guardian_history`、`retained_context`、`mcp_resource_origins`、`window_number`/`window_id`、`compaction_response_id`、`latest_token_usage_record` 与 `resume_metadata`（多代理版本、最后开始的轮次 id、上一轮设置）。
- 远端实现（V2）：`compact_remote_v2.rs` + `compact_remote_history.rs` + `compact_v2 图片预算`/`attempt` 等文件，说明远端压缩有自己的独立协议、图片预算与重试，并且**能力协商**（`RemoteCompactionSupport::{V2, Unsupported}`）决定走哪条路；失败时还有 `compact_model_fallback.rs`（换模型兜底）。
- 换窗：`start_new_context_window` 不做摘要，直接重建初始上下文（供 token 预算特性的“重置窗口”语义使用），并保留客户端开发者消息（`RetainClientDeveloperMessages` 特性 + `truncate_retained_messages_for_remote_compaction` 的预算控制）。

**状态可检测**

- `context_window_token_status` 返回一组明确状态：`token_limit_reached`、`full_context_window_limit_reached`、`turn_end_compaction_threshold_reached`、`auto_compact_scope_tokens`/`auto_compact_scope_limit`/`auto_compact_window_prefill_tokens`，还有 `AutoCompactTokenLimitScope::{BodyAfterPrefix, ...}` 决定“按整段上下文还是按前缀之后的主体”计量。也就是说“为什么压缩”与“按什么口径压缩”都能被观测和测试（配套 `auto_compact_window.rs`、`token_budget.rs`、`rollout_budget.rs`）。

**实现细节（`core/src/compact.rs` 精读）**

压缩不是特殊路径，而是**一次普通的采样轮**，只是提示词换成了摘要指令：

- `run_inline_auto_compact_task` 用 `turn_context.config.compact_prompt` 或内置 `SUMMARIZATION_PROMPT` 组成一条合成用户输入（`UserInput::Text`，`text_elements` 为空），随后调用 `run_compact_task_inner`。
- `run_compact_task_inner` 先跑 `run_pre_compact_hooks`（返回 `Stopped` 即按 `TurnAborted` 结束），执行后跑 `run_post_compact_hooks`；整段用 `CompactionAnalyticsAttempt` 记录 trigger/reason/implementation/phase/status 与耗时、用量等细节。
- 压缩请求走 `drain_to_completed`：复用**同一个 `ModelClientSession`**（保住粘性路由与 WebSocket 增量请求状态），失败按 `stream_max_retries` 与 `backoff(retries)` 重试，每次重试发一条 `StreamError` 通知（文案 `Reconnecting... n/m`）。
- 压缩自身撞上上下文上限时有专门策略：先 `history.remove_first_item()`（从最旧开始删，保住前缀缓存）并把重试计数归零；只剩一条时置 `set_total_tokens_full` 并失败返回。
- 成功后组装新历史：摘要文本 = `SUMMARY_PREFIX` + 摘要正文（`PostTurn` 阶段必须从压缩响应里取到最后一条非空助手消息，否则报流错误）；**用户消息被保留**（`collect_annotated_user_messages`），即压缩只折叠中间过程、不丢用户意图；再按 `InitialContextInjection` 把初始上下文插到“最后一条真实用户消息或摘要之前”。
- 收尾：`advance_auto_compact_window()` 递增窗口编号 → `replace_compacted_history(new_history, reference_context_item, world_state_baseline, metadata)`（metadata 含摘要全文、窗口号、压缩响应 id、模型 comp hash）→ `recompute_token_usage` → 事件与警告（明确提示“长线程与多次压缩会降低准确度，尽量开新线程”）。

### 8.2 agentxx：摘要中间件 + 子代理执行

- `SummarizationMiddlewareHandle`（`lib/src/middlewares/summarization.cpp`）是压缩主体，参数化程度高：`defaultModelSupportMaxToken`、`asciiCharsPerToken`、`unicodeCharsPerToken`、`tokensPerImage`、`extraTokensPerMessage`、`recentTokenBudgetRatio`、`summaryMaxTokens`。
- 压缩前的整理动作都写在同一个文件里，动机明确：
  - 折叠“噪音消息”（`[Please continue]`、`[User canceled]`、`[Exception aborted]`、`[Empty]` 等 `AutoInserted` 且无 tool_calls 的连续消息）；
  - 把多模态 data URL 降级为文本标记（`[用户附带了图片]` 等）并清空 URL，避免 Base64 吃掉上下文；
  - 相邻完全相同的消息去重（`isSameMessage` 做全字段比较）。
- 摘要由**子代理**执行（复用委派通道），因此摘要请求本身有独立上下文与模型选择；工具输出另有 `SummarizationToolHandle`（每个工具可自定义摘要策略，由 `initSummarizationHandles` 收集）。
- 触发条件与冷却：按 compare-dsh.md 的核对结论，除阈值外还有冷却规则（上次压缩后消息数增长 ≤2 且无失败时跳过 LLM 压缩）；token 计数优先用 API 返回的 usage，启发式仅在缺失时兜底。
- 压缩现场可恢复：压缩作为图中间的 HIL 中断处理路径之一（`AgentRunner` 的 `subagent` 分支），中断现场序列化进图状态，重启后可继续。

**实现细节（`middlewares/summarization.cpp` 精读）**

- **摘要提示词是配置项**：取自 `AgentPrompt.appendSystemPrompts["summarization"]`（可定制、可随训练序列化），渲染时注入 `{omitted_note}` 与 `{max_words}`（`summaryMaxTokens / 4`）；取不到就直接放弃压缩（返回空串）。
- **载荷裁剪**：请求副本（system + 待压缩段 + 指令）若超过模型上限的 95%，从最旧开始删（system 保留），删掉的数量写成 `NOTE: The oldest N message(s) were omitted...` 放进提示词——被丢掉多少是**告诉模型的**，而不是静默截断。
- **摘要由“同上下文子代理”完成**：透传结构化消息（不是文本转录）、子代理用**与父会话相同的 sessionId 与模型**（命中 provider KV/前缀缓存）、`tools = []`（无工具，避免副作用与成本）、`enable_summarization = false`（禁止二次压缩）；子代理内部跑完整 agent 循环，输出的纯文本即摘要。
- **两条投递路径**，原因是踩过坑：
  1. 常规路径经 `Topic::SubagentExecute` 请求，内部 `requestInterrupt` 抛 `NodeInterrupt`，由 `AgentRunner` 派生 subagent，resume 后返回结果；
  2. **手动压缩直派宿主**：agent 空闲时没有 `AgentRunner` 中断循环，走中断路径的异常会穿过 `catchErrorAsync` 逃逸到事件总线里被 asio 静默丢弃，表现为压缩永久卡在 “Summarizing...” —— 因此空闲态改为直接 `host->spawnBatch(...)` 并 `await`，配 2 分钟超时（超时返回空串，由调用方走 `hardTruncate` 兜底）。
- **错误串透传防护**：子代理返回若形如 `{"error": ...}`（如被取消）会被判定为失败，绝不当作摘要写回上下文——否则表现为“压缩成功 + 继续执行”，取消形同虚设。
- **`hardTruncate` 兜底算法**（摘要不可用时保证请求能发出去）：保留 system → 插入一条 `[Earlier conversation was truncated due to context limit...]` 提示（`AutoInserted | Summarized`）→ 取最近消息填 30% token 预算 → 仍超 95% 就从最旧（recent 段内）开始丢，至少保留 1 条 → 只剩 1 条仍超限时对**该条文本**做二分截断（保住开头语义，消息结构与角色不变）。
- 其他整理动作：`cleanNoiseMessages`（折叠 `[Please continue]`/`[User canceled]` 等无 tool_calls 的自动插入消息）、`foldExploratoryToolcalls`（探索型工具调用的折叠）、`doSummarizeToolcall`（工具结果级摘要，配合工具的 `createSummarizationToolHandle`）、`splitRecentByTokenBudget`（按 token 预算切出最近段）。
- 触发与冷却、API usage 优先等策略在 `onModelcallRunFunc` 中（见 §2.2 与 compare-dsh.md 的核对结论）。

### 8.3 对比

| 维度 | agentxx | codex |
|---|---|---|
| 时机 | 阈值触发（含冷却），主要集中在轮中 | 轮前/轮中/轮后/手动四时机显式建模 |
| 实现 | 中间件 + 子代理摘要 | 本地摘要 / 远端压缩 V2 双实现，能力协商选择，另有模型兜底 |
| 换窗 | 无独立概念 | `new_context_window` + 初始上下文重建（不摘要） |
| 计量口径 | 启发式 + API usage 优先 | 多种口径（全窗/主体/窗口预填）并可观测 |
| 压缩结果 | 上下文被替换为新消息集 | `CompactedItem` 携带替换历史与恢复元数据，重启可复原 |
| 图像/大内容 | 压缩前降级 data URL 为文本 | 图片预算（远端压缩）、保留消息预算 |

**优点/缺点**

- agentxx 的压缩在“整理”这一步做得细（噪音折叠、去重、多模态降级、工具级摘要策略），这些是长会话里真实省 token 的手段；缺点是**时机单一**（只有阈值）、**结果不可复原**（压缩后只剩新消息集，没有专门的恢复元数据）、**口径不透明**（为什么触发只有日志）。
- codex 的时机与口径更完备，压缩结果与恢复元数据一起持久化，重启/分叉后行为一致；代价是分支多（本地/远端/V2/换窗/兜底模型），每加一种都要在 `CompactedItem` 与重建路径上对齐。

### 8.4 可迁移到 agentxx 的设计

1. **压缩时机前置**（建议 P0）：增加“轮前预判压缩”（在新输入写入前估算并压缩），避免“先写入超长输入 → 请求失败 → 再压缩”的往返；落地：`base_agent.cpp` 轮首、`middlewares/summarization.cpp` 暴露可调用的预判接口。
2. **压缩结果带恢复元数据**（建议 P1）：压缩时记录摘要对应区间（消息 id 范围或版本号）、使用的模型、口径与时间戳，写入会话 meta；后续排查与“跳回压缩前”都有依据。落地：`session_store.cpp` 的 `meta` 表 + 压缩中间件。
3. **口径可观测**（建议 P1）：把 token 状态做成可查询结构（当前估算、上限、剩余、触发阈值、是否刚压缩过），经 wire 暴露给 UI（现在 TUI 只有粗略展示），并加指标用于回归测试。
4. **换窗与摘要分离**（建议 P2）：当上下文被“压缩但想保留目标”时（例如 token 预算类特性、或用户要求“从当前目标重开”），提供不做摘要的换窗入口，直接重建初始上下文并保留必要的开发者/权限信息。
5. **压缩时保留用户消息（建议 P1）**：codex 的替换历史是“摘要 + 全部用户消息”，因此压缩不会丢用户意图；agentxx 目前的摘要会把整个区间折叠成一段摘要，长会话里早期需求容易丢失。建议在替换历史时按规则保留用户消息（或至少保留其要点列表），并写入压缩元数据以便追溯。
6. **摘要前缀与来源标记（建议 P1）**：给压缩产生的摘要固定前缀（如 `[Compaction summary]`）并标注摘要覆盖的消息区间与模型，使后续轮次、UI 与审计都能识别“这段是摘要而不是原始对话”；与 §3.4 的注入内容标记是同一套机制。
7. **压缩失败要能看出原因（建议 P2）**：agentxx 的兜底是 `hardTruncate`（静默硬截断），建议把它记成结构化事件（触发原因、丢弃条数、是否二分截断），否则用户只会看到“上下文突然变短了”。

---

## 9. 权限、审批与沙箱执行

### 9.1 codex：权限档案 + 命令策略 + 真实沙箱 + 网络管控

**四层结构**

| 层 | 载体 | 作用 |
|---|---|---|
| 权限档案 | `PermissionProfile`（`protocol/src/models.rs`）+ `FileSystemSandboxPolicy`/`NetworkSandboxPolicy`（`protocol/src/permissions.rs`） | 描述“这一轮允许什么”：文件系统 kind（`Restricted` 等）、读写拒绝条目、网络开关 |
| 审批策略 | `AskForApproval::{Never, OnRequest, UnlessTrusted, Granular(cfg)}`、`ApprovalsReviewer` | 决定“遇到需要授权时问不问、问谁、问什么粒度” |
| 命令策略 | `execpolicy` crate（`Policy`：按程序的多重映射规则 + 网络规则） | 对具体命令做前缀规则判定，批准可**持久化为策略改动**（`append_amendment_and_update`） |
| 执行沙箱 | `sandboxing` crate（Seatbelt/Landlock/bubblewrap/Windows）+ `network-proxy` crate | 真正限制进程能力，越权尝试被记录为违规事件 |

**源码要点**

- 文件系统条目有**冲突优先级**：注释明确“同具体度时 `deny` 胜过 `write`，`write` 胜过 `read`”（`permissions.rs`），并保护工作区元数据路径（`.git`/`.agents`/`.codex`/`.aws`），另提供 `forbidden_agent_metadata_write(path, cwd, policy)` 供工具在执行前直接拒绝。
- 沙箱后端各自独立：macOS 是 Seatbelt SBPL 策略（基础/网络/只读平台默认三份 `.sbpl` 文件），Linux 是 Landlock（含遗留模式开关 `use_legacy_landlock`）与 bubblewrap（WSL1 有专门警告），Windows 有受限令牌/提权/MXC 三条路径与“读白名单”（`grant_read_root_non_elevated`）。`SandboxManager` 负责把策略与命令转换成具体启动参数（`SandboxTransformRequest` → `SandboxCommand` → `spawn_process`），并对不支持组合给出显式错误（`SandboxTransformError` → `CodexErr`）。
- 被拒绝会被识别并**升级重试**：`denial.rs` 判断“是否像沙箱拒绝”（含执行器托管沙箱的差异），`violation.rs` 记录文件系统/网络违规事件；编排器据此选择更宽的执行方式重试（并且因为有审批缓存，不会重复询问）。
- 网络是独立管控面：`network-proxy` 提供受管代理、域名允许/拒绝表、凭据代理与“被拦截请求观察者”；审批产生的网络规则会同时写进运行中的代理与 `execpolicy` 的网络规则（`persist_network_policy_amendment`，且实现里对“主机名必须与批准的主机一致”做了校验）。
- 审批有“第二意见”机制：`ApprovalsReviewer::AutoReview` + `guardian_review.rs`（`ext/guardian-reviewer`）让一个受信任的审查代理先判断风险，可减少对用户的打扰；`request_permissions` 工具让模型在轮次**中途**申请提权（`PermissionGrantScope::{Turn, Session}`，含 `strict_auto_review` 语义与“请求与批准求交”的归一化处理）。
- 审批等待是活跃进程内的一次性 `oneshot`，注册在活动轮次的 `turn_state` 里（`insert_pending_approval` / `insert_pending_request_permissions` / `insert_pending_user_input`），响应到达时按 `call_id`（或 `approval_id`）投递；找不到对应项就记警告。

**实现细节（`core/src/session/handlers.rs`、`state/turn.rs`、`tools/sandboxing.rs`）**

- 审批决定的落点分三种：`ReviewDecision::Abort` → `interrupt_task()`（直接中断整轮，而不是继续执行）；`ApprovedExecpolicyAmendment{...}` → 先把修订**写进命令策略**（失败发 Warning 事件，但不阻断本次调用），再 `notify_approval`；其余决定只投递给等待者。
- 权限授予分两个作用域存放：会话级写在 `SessionState`（`record_granted_permissions`），轮次级写在 `TurnState.granted_permissions_by_environment_id`（按环境 id 合并，`merge_permission_profiles`）；这样“本轮临时提权”不会泄漏到下一轮，而“会话级提权”能跨轮生效。
- `request_permissions` 的响应会被**求交**：`intersect_permission_profiles_with_context(申请, 批准, 策略上下文)`——模型申请再多也只能拿到批准的子集；若响应同时带 `strict_auto_review` 与 `Session` 作用域，直接降级为 `Turn` 且清空权限（防止“自动审查模式下被长期提权”）。
- 审批缓存 `ApprovalStore` 的键是**序列化后的 JSON**（`serde_json::to_string`），因此任何可序列化结构都能当键；`with_cached_approval` 支持多键（apply_patch 可能同时改多个文件），只有全部命中 `ApprovedForSession` 才跳过询问，批准后逐键记账。

**实现细节（`execpolicy/src/parser.rs`、`rule.rs`、`amend.rs`）**

- 命令策略不是简单的“程序 + 前缀”配置文件，而是 **Starlark 脚本**：`PolicyParser` 用 `Dialect::Extended`（开启 f-string）解析策略文件，通过自定义 builtin（`policy_builtins`）构造规则，最终 `build()` 出 `Policy{rules_by_program: MultiMap<String, RuleRef>, network_rules, host_executables_by_name}`。
- **规则自带测试**：解析期收集 `pending_example_validations`，加载结束后 `validate_pending_examples_from(...)` 统一校验——也就是策略里写下的“应该匹配/不该匹配”的样例会在**加载时**被验证，规则写错会直接暴露而不是等到运行时。
- 程序名归一化有两套键：`executable_lookup_key`（按名字）与 `executable_path_lookup_key`（按路径），并有 `host_executables_by_name: HashMap<String, Arc<[AbsolutePathBuf]>>` 记录宿主上的实际可执行路径——这解决“同一个名字在不同 PATH 下是不同程序”的问题。
- 错误带位置信息（`ErrorLocation`/`TextPosition`/`TextRange`），并有 `format_exec_policy_error_with_source` 这类“把错误落到源码位置”的展示函数；此外 `amend.rs` 负责把批准结果（命令前缀、网络主机规则）追加回策略，`prefix_rule_migration` 处理历史格式升级。

### 9.2 agentxx：统一判定 + 声明式询问

- 判定顺序（按 compare-dsh.md 核对源码后的结论）：worktree 隔离 → 配置拒绝（即使完全授权也不放行、不询问）→ 完全授权 → 最长前缀规则 → `noRuleOperator` 兜底；空路径或无法规范化的路径按“未获批准（Ask）”处理。
- 权限来源是**插件声明**（`agentxx.agent.permission` 表）：作用域（读/写）、目标来源（无/路径/文本）、目标参数名；模式类工具再经 `check_paths` 三态批量查询逐路径复核（详见 §5.2）。
- 询问是**声明式 UI 卡片**：中间件经中断通道请求用户决定，UI 渲染为表单（含路径、命令、理由等结构化字段），用户选择可被记住；`GetPermissionState`/`SetFullAuth`/`PermissionState` 三条 wire 消息维护“完全授权”状态，TUI Info 侧边栏可切换。
- 执行侧没有系统沙箱：命令以宿主进程权限运行（`execute_command` 插件），隔离手段是工作区路径规则 + git worktree 绑定 + 权限询问；没有命令策略引擎（没有“某程序 + 某前缀 = 允许/拒绝”这类可持久化规则），也没有网络层管控。
- 好处是跨平台一致（Windows/Linux/Android 同一套判定逻辑），坏处是**防护强度取决于询问是否被正确回答**：用户点了“记住”，后续同类命令就不再询问。

**实现细节（`middlewares/permission.cpp` 精读，544 行）**

`decideTarget(path, index, sessionId)` 的判定顺序（`index` 为读/写作用域）：

1. **空路径 → `Ask`**（明确注释：不按“已批准”处理，调用方按未获批准丢弃）；
2. **worktree 会话隔离**：`index == WRITE` 且路径不在会话自己的 worktree 子树（`allowPath`）内、但落在主检出（`denyWritePath`）下 → `Deny`（读不受限）。判定顺序有原因：真实 worktree 位于主检出的 `.agentxx/agent/worktrees/` 下，必须先判 `allowPath` 再判 `denyWritePath`，否则会话连自己的工作区都写不了；
3. **配置显式拒绝路径** → `Deny`，且**即使已完全授权也不放行、不询问**；
4. **完全授权状态** → `Allow`；
5. **最长前缀规则表**（`XXRouter<PermissionOperator, 2>::get(path, index, re_path, /*prefix*/ true)`）→ `ALLOW`/`DENY`/`INTERRUPT(Ask)`；
6. **未命中任何规则 → `noRuleOperator`**（CodeAgent 按 `permission.mode` 设置，默认 `ALLOW`，与历史行为一致）。

配套细节：

- 工具级入口 `checkToolPermission`：**未声明权限的工具直接放行**（权限限制随插件走）；目标参数按实际 JSON 类型处理——数组逐项判定、字符串视为单目标、缺省或空值不参与判定；路径目标按会话工作目录规范化为绝对路径（使绝对路径规则也能匹配相对路径访问）；**任一目标被拒即整调用被拒**，询问是逐个目标进行的。
- `decidePaths` 供模式类工具（glob/grep/list）做批量三态查询：逐项规范化后判定，规范化失败（空路径）按 `Ask` —— 这就是“逐路径复核、不被 `**` 绕过”的实现。
- `requestPermission`：无会话总线 → **默认拒绝**（保安全，注释写明）；等待时间传 `0`（不限制），因为总线默认 30s 会把用户的真实决策截断成拒绝；`remember` 时**由中间件自己注册规则**（源码注释解释了为什么不能让 IO 端点注册：端点走会话总线发布规则事件，而中间件订阅的是 agent 全局总线，事件到不了，表现为“勾选记住后下次还问”）；目录规则按最长前缀自动覆盖子目录；`fullAuth && allow` 才切换到完全授权。
- 源码内记录的已知限制（值得迁移时一并解决）：**判定基于词法规范化路径，不解析符号链接** —— 允许范围内的链接可以指向被拒目录，逐路径过滤接口也看不到链接目标；彻底处理需对已存在路径 `weakly_canonical` 后再判定一次，源码注明会牵动所有工具与查询接口，暂缓。

### 9.3 对比

| 维度 | agentxx | codex |
|---|---|---|
| 权限表达 | 中间件规则（白/黑名单 + 前缀 + 完全授权） | 权限档案（文件系统/网络策略对象） |
| 规则来源 | 插件声明 + 配置（路径规则） | 配置 requirements/managed 层 + `execpolicy` 规则文件 |
| 命令级策略 | 无（按路径/工具判定） | 有（按程序 + 前缀 + 网络主机，可持久化修订） |
| 沙箱 | 无系统沙箱（宿主权限执行） | Seatbelt / Landlock / bubblewrap / Windows 三种后端 |
| 网络 | 无管控 | 受管代理 + 域名规则 + 凭据代理 |
| 审批 | 声明式表单（信息丰富）+ 记住选择 | 固定决策枚举 + 审批缓存 + 命令/网络修订 |
| 第二意见 | 无 | Guardian 自动审查（可配置审查者） |
| 提权请求 | 无（只能靠询问触发） | `request_permissions` 工具（轮次内申请，会话/轮次作用域） |
| 跨平台 | 一致（不依赖系统能力） | 各后端能力不同，需处理不支持组合 |

**优点/缺点**

- codex 的安全模型是“**默认收窄 + 越权可升级 + 决策可持久化**”，并且每层都能被测试（suite 里有 `exec_policy.rs`、`permissions_messages.rs`、`windows_sandbox.rs`、`network_approval*.rs`、`scenarios_mxc.rs` 等）；代价是平台差异与配置复杂度，用户需要理解档案/审批/规则三层概念。
- agentxx 的询问体验更好（结构化卡片 + 完全授权开关 + 记住选择），但**没有兜底防线**：一旦用户授权，错误或恶意的命令会以完整权限执行，且没有可事后审计的“越权尝试记录”。

### 9.4 可迁移到 agentxx 的设计

1. **命令策略规则引擎（建议 P0）**：把“询问并记住”的结论落成**按程序 + 参数前缀 + 网络主机**的规则表（会话级/用户级），后续判定先查规则表；这也是把“记住选择”从内存提升为可审计配置的最短路径。落地：新增 `agent/lib/src/middlewares/exec_policy.{h,cpp}`（或复用 permission 中间件内的规则段）、`settings_db` 持久化，配置形态参考现有 yaml 分层。
2. **越权尝试留痕（建议 P1）**：即使没有系统沙箱，也可以在执行前后做“声明与实际”比对（例如工具声明只读但命令含重定向），把可疑行为写成结构化事件推给 UI 与会话 meta，形成可审计记录。
3. **审批缓存与最小化打扰（建议 P1，与 §5.4 第 4 条同一项）**：缓存键取“工具 + 规范化目标（程序/路径前缀）”，命中即跳过询问并注明命中来源；对只读操作默认不询问（可按配置放开）。
4. **轮次内提权请求（建议 P2）**：为模型提供“申请更高权限”的工具形态（申请 → 用户或规则批准 → 记录作用域为轮次/会话），避免把“需要提权”混在工具失败重试里。
5. **沙箱适配层可选落地（建议 P2，需评估）**：Linux 下可优先接 bubblewrap（存在即用，缺失回退），macOS 下接 Seatbelt；接口设计成“若可用则收窄，不可用则记录原因并继续”，不要让平台差异进入业务逻辑。落地：`execute_command` 插件的执行路径 + 新增 `sandbox/` 适配目录。
6. **修掉判定里的符号链接缺口（建议 P1，安全相关）**：现在 `decideTarget` 只做词法规范化；对**已存在**的路径再取 `std::filesystem::weakly_canonical` 做一次判定（或至少对写操作做），即可覆盖“允许目录里的链接指向被拒目录”的绕过路径。落地：`middlewares/permission.cpp` 的 `decideTarget`/`decidePaths`，并同步给 `check_paths` 批量接口；需评估 Windows 语义与性能（只对存在的路径调用）。
7. **提权响应求交与降级规则（建议 P2）**：若引入“轮次内提权申请”，建议同时实现两条约束：申请与批准**求交**（模型拿到的是批准子集），以及“自动审查模式下不允许会话级长期提权”（降级为轮次级）——这两条在 codex 里是写死在归一化逻辑里的。
8. **提权作用域分离（建议 P2）**：会话级授权写会话状态、轮次级授权写轮次状态，避免临时提权泄漏到后续轮次（agentxx 目前只有“完全授权”这一个全局开关）。
9. **规则自带样例校验（建议 P1）**：codex 的命令策略是 Starlark 脚本，规则可声明“应匹配/不应匹配”的样例并在**加载期**校验。agentxx 的路径/命令规则表可以照此加一个可选的 `examples` 段（正例/反例），启动时跑一遍校验，把“规则写错导致误放行/误拒绝”挡在运行之前。落地：`middlewares/permission.cpp` 的规则注册 + 配置校验路径。

---

## 10. 子代理、多代理与后台任务

### 10.1 codex：线程管理器 + 代理控制面

**分层**

| 组件 | 职责 |
|---|---|
| `ThreadManager`（`core/src/thread_manager.rs`，2531 行） | 线程（会话）生命周期：新建/恢复/派生/关闭；`NewThread`/`StartThreadOptions`/`ForkSnapshot`/`ThreadShutdownReport` |
| `LocalAgentControl`（`core/src/agent/control.rs` + 20 个子模块） | 单个会话的代理树控制：`api`（对外接口）、`spawn`、`delivery`（邮箱投递）、`completion`、`inspection`、`interrupt`、`residency`、`budget`、`resume`、`runtime`、`service_tier`、`target`、`watch`（状态订阅）、`user_authorization`、`spawn_guard`/`spawn_telemetry` |
| 代理身份与角色 | `agent-identity`、`agent-roles`（角色配置解析、`DEFAULT_ROLE_NAME`）、`AgentPath`（形如树路径的身份）、`AgentStatus` |
| 代理间通信 | `InterAgentCommunication`、`agent-message-board-client` + `ext/agent-message-board`、`AgentMessage::{Plaintext, Encrypted}` |
| 代理关系存储 | `agent-graph-store`（父/子/派生关系），`ext/agent` 提供多代理工具面 |

**关键机制**

- **占用与配额**：`AgentExecutionGuard`（`#[must_use]`）由 `agent_control.admit_turn(...)` 在 `start_task` 时取得，轮次结束才释放；注释写明“远端后端还要能在 worker 丢失后恢复预留”。这把“同时跑多少代理/轮次”变成可替换策略，而不是硬编码。
- **派生（fork）语义**：`SpawnAgentForkMode::{FullHistory, LastNTurns(n)}`，配合 `ForkPersistence::{Copied, CopiedDeferred, Referenced}` 决定子线程的历史是复制、延后落盘还是引用祖先文件；`SpawnAgentOptions` 里还带 `parent_thread_id`/`parent_turn_id`/`root_turn_id`/`turn_trigger`（用量归因到发起轮次）。
- **消息投递**：`MessageDeliveryMode::{QueueOnly, TriggerTurn}` 与多代理版本（`MultiAgentVersion::{V1, V2, Disabled}`）配合；V2 下子代理的终局事件要回报直接父代理（`maybe_notify_parent_of_terminal_turn`），并且会带上 `initiating_agent_path` 以便沿树回传。
- **继承与覆盖**：子代理继承父的设置但**服务等级强制取根代理**（`capture_step_context_inner` 里对 `ThreadSpawn` 子代理重置 `service_tier`），模型目录刷新对子代理关闭（`RefreshStrategy::Offline`），避免子代理重复拉目录。
**实现细节（`unified_exec/process_manager.rs`、`codex_thread.rs`、`thread_manager.rs`）**

- **持久 shell 的执行纪律**（`process_manager.rs` 顶部常量即规格）：
  - 环境加固 `UNIFIED_EXEC_ENV`：`NO_COLOR=1`、`TERM=dumb`、`LANG/LC_CTYPE/LC_ALL=C.UTF-8`、清空 `COLORTERM`、`PAGER/GIT_PAGER/GH_PAGER=cat`、`CODEX_CI=1`——目的是让命令输出可解析（不分页、不带颜色、不依赖区域设置）。
  - 注入的环境变量既有变量名也有语义：`CODEX_THREAD_ID`、`CODEX_VERSION`、`CODEX_PERMISSION_PROFILE`（让子进程知道自己处在哪个会话与权限档案下）。
  - 限额与夹取：`MAX_UNIFIED_EXEC_PROCESSES` 限制并发进程数；yield 时间有上下限夹取（`MIN_YIELD_TIME_MS`/`MAX_YIELD_TIME_MS`/`MIN_EMPTY_YIELD_TIME_MS`）；stdin 需要审批时单次写入上限 `MAX_STDIN_APPROVAL_BYTES = 8000`；中断字符是 `\u{3}`（Ctrl-C）。
  - 输出用**头尾缓冲**（`HeadTailBuffer`）：长输出保留开头与结尾，中间省略——比“只留开头”更符合排错需求。
  - 进程退出与流式输出都由独立 watcher 任务处理（`spawn_exit_watcher`、`start_streaming_output`、`emit_exec_end_for_unified_exec`），因此“命令还在跑”与“UI 已经在显示输出”是两条并行路径。
  - 晚到的网络拒绝有 100ms 宽限（`LATE_NETWORK_DENIAL_GRACE_PERIOD`）：避免把“刚启动就因其后到达的网络策略变更而失败”误判成用户可见错误。
  - 还给插件命令装配了指标侧车（`PluginMetricsSidecar`、`PLUGIN_METRICS_OUTPUT_ENV_VAR`）与危险命令检测（`is_dangerous_command`），测试可用 `set_deterministic_process_ids_for_tests` 获得确定的进程 id。
- **`CodexThread` 的对外语义**（`codex_thread.rs`）是“一个线程能被怎么用”的完整清单，且每条都写清了边界：
  - `start_or_steer_turn` / `start_turn_if_idle` / `continue_turn_if_idle{expected_previous_turn_id}` / `steer_turn{expected_turn_id}` / `recover_turn_if_idle`：分别对应四种投递模式；`continue_turn_if_idle` 额外要求“输入必须是响应项，且永不视为用户授权”，并在“已有更新的任务开始过（即使已结束）”时拒绝。
  - `recover_turn_if_idle` 恢复被中断的轮次时**沿用已记录的 turn id**，并从参考上下文里恢复 `root_turn_id`；进入前要先过 `ensure_execution_capacity_for_turn_start`（配额准入）。
  - `suspend_turn_and_shutdown`：拒绝非根线程；存在**当前加载的后代**时拒绝挂起；调用方在挂起成功前**不得转移所有权**（需要停止执行、刷新历史、关闭写者）；会话会处理已接受的请求，即使调用方断开。
  - `inject_if_running`：把模型可见条目注入当前活动轮次，没有活动轮次时**原样返回条目**（调用方自己决定丢弃或留待下一轮）。
  - `active_turn_environment_selections`：给出活动轮次选定的环境快照（含仍在启动或已失败的），宿主据此对每个执行器做 steering 授权——这就是“跨机器/跨环境 steering 安全”的落点。
- **`ThreadManager` 是能力装配中心**（`thread_manager.rs`）：对外提供 `start_thread`/`spawn_internal_session`/`fork_internal_session`/`reserve_thread_id`/`spawn_legacy_subagent`/`resume_legacy_thread_from_rollout`/`list_thread_ids`/`get_thread`/`update_thread_metadata`/`move_thread_to_section`/`list_agent_subtree_thread_ids`，并通过 `with_thread_id_generator`、`with_agent_control_factory`、`with_code_mode_session_provider` 注入可替换实现；`subscribe_thread_created` 用广播通道通知宿主新线程，后台 rollout 迁移也有独立入口（`start_background_rollout_migration`）。

### 10.2 agentxx：子代理即独立 agent + 中断委派

- 子代理由 `AgentHost` 派生**独立 agent**（独立图、独立会话、独立上下文），不再用图内嵌套子图（`tools/subagent.h` 注释明确“中断委派，不再使用图内嵌套 subgraph；旧的 getSubgraph/onSubagentEnd 已移除”）。
- 委派路径有两条，最终都落到同一个执行服务：
  1. 模型可见路径：`SubAgentManagerTool`（`agentxx_subagent`）——由 `SubagentManagerMiddlewareHandle` 按 `AgentConfig::enableSubagent` 决定是否注入工具（关闭时模型看不到，但总线服务仍在，压缩等内部路径照常）；
  2. 程序内部路径：经事件总线 `Topic::Subagent`（`service.subagent.execute`）请求，宿主在根与每个子代理的总线上统一注册服务，因此**嵌套委派与根委派同路径**（扁平化）。
- 中断委派流程：`AgentRunner` 遇到 `interruptArg.name == "subagent"` 时解析批量参数 → 请求总线 → 收集结果（含 `cancelled` 结构化字段，用于区分“子代理被取消”与“正常返回”）→ 写回 resume 值，然后 `resume_async` 继续执行。
- `SubAgentTaskBase` 承载子代理静态元数据（名称/描述/系统提示），`SubAgentNormalTask` 是默认“隔离上下文独立运行”的形态；子代理结果回到父会话后再由父模型继续处理。
- 没有独立的后台任务模型：长跑工作只能表现成“子代理委派（阻塞式等待）”，没有“可继续/可查询的后台作业”抽象，也没有占用配额（并发度取决于调用方式与宿主）。

### 10.3 对比

| 维度 | agentxx | codex |
|---|---|---|
| 子代理形态 | 独立 agent（独立图/会话/上下文），经 AgentHost 派生 | 独立线程（`ThreadManager`），与主会话同一套 Session 机制 |
| 委派入口 | 模型工具 + 总线服务（内部路径同源） | 工具面（`spawn_agent`/`send_message`/`followup_task`）+ 邮箱 |
| 隔离 | 上下文隔离 + 独立系统提示词 | 支持 `SessionIsolation::Isolated`（只保留快照指令）、独立权限档案、可选远端环境 |
| 派生 | 无（新建独立会话） | `FullHistory` / `LastNTurns(n)` + 引用式持久化 |
| 通信 | 中断委派返回值 + 事件总线 | 邮箱（队列/触发）+ 消息板（跨代理共享板）+ 终局回报 |
| 并发/配额 | 无配额概念 | `AgentExecutionGuard` 可替换配额策略，轮次占用受控 |
| 后台任务 | 无（子代理阻塞式） | 持久 shell 进程管理、后台终端列表、进程数指标 |
| 继承规则 | 子代理使用自己的配置与提示词 | 设置继承 + 服务等级/目录刷新等按子代理类型覆盖 |

**优点/缺点**

- codex 的多代理是“**会话机制的同构复用**”：子代理就是另一个线程，因此历史、持久化、审批、压缩全都能直接复用，派生/引用式持久化还能省磁盘；代价是控制面复杂（20 个子模块）与树状状态同步成本。
- agentxx 的子代理在**隔离性**上更干净（独立 agent 实例、独立图），委派与压缩共用一条总线服务；缺点是没有派生语义（不能“带着半截历史继续”）、没有配额（可能同时起很多子代理）、没有后台作业（长任务只能等）。

### 10.4 可迁移到 agentxx 的设计

1. **子代理配额与生命周期守卫**（建议 P0）：在 `SubAgentManagerTool` 与总线服务入口加并发上限与排队（配置项），并用 RAII 守卫保证异常/取消路径也会释放名额。落地：`lib/src/tools/subagent.cpp`、`middlewares/subagent_manager.cpp`。
2. **派生（fork）语义**（建议 P1）：允许“带着父会话最近 N 轮历史/摘要”启动子代理，而不是只能空上下文起步；实现上可复用会话存储（父会话消息按 id 范围复制到子会话上下文）。落地：`AgentHost` 创建子 agent 的路径 + `conversation_types.h`。
3. **可继续的后台作业**（建议 P1）：把长任务（构建、测试、长时间命令）做成后台作业：作业 id、状态、输出增量、终止接口，经 wire 暴露给 UI；模型侧可有“查询作业/等待作业”的工具。落地：`execute_command` 插件 + `events` 主题 + wire 消息扩展。
4. **代理间消息通道**（建议 P2）：当前子代理只有“委派—返回”模式，若将来支持多代理协作，建议先做邮箱式投递（队列/触发两种语义）而不是共享内存或全局表，避免并发语义不清。
5. **执行环境加固（建议 P0，成本极低）**：codex 在持久 shell 里固定注入 `NO_COLOR=1`、`TERM=dumb`、`LANG/LC_* = C.UTF-8`、`PAGER/GIT_PAGER/GH_PAGER=cat`、`CODEX_CI=1`，并注入 `CODEX_THREAD_ID`/`CODEX_VERSION`/`CODEX_PERMISSION_PROFILE`。agentxx 的 `execute_command` 直接继承父进程环境，容易出现“命令调用分页器卡住”“输出带 ANSI 颜色影响解析”“区域设置导致编码错乱”等问题；把这份环境清单落到命令执行处即可显著降低偶发卡死。落地：`agentxx_execute_command` 插件 + 工具执行器。
6. **外部 API 语义清单文档化（建议 P2）**：`CodexThread` 的每个公开方法都写清了“会做什么/不会做什么/在什么条件下拒绝”，这类文档正是外部集成方最需要的；agentxx 可在 `base_agent.h` 的公开方法上补同样口径的说明（尤其是 `runTurnAsync` 的中断/恢复语义与 `shutdownAsync` 的超时行为）。

---

## 11. 检索、标题、遥测、附件、记忆与大输出

### 11.1 codex

| 能力 | 实现 | 要点 |
|---|---|---|
| 会话检索 | `rollout/src/search.rs` + `state_db` + `session_index` + `thread-store` 的 `search_threads`/`search_thread_occurrences` | 日志反向扫描（`reverse_jsonl_scanner`）与可寻址读取（`seekable_reader`）配合索引，支持分页与“按出现位置”搜索 |
| 标题与摘要 | `append_thread_name`、`find_thread_meta_by_name_str`、`read_head_for_summary`、`context-fragments` 的 `recap_prompt` | 标题是线程元数据，摘要从日志头部读取（不必加载全量） |
| 遥测 | `otel` crate（`SessionTelemetry`、指标常量、`current_span_w3c_trace_context`）、`analytics` crate（`AnalyticsEventsClient` 的各类 fact）、`feedback` crate | 轮次/工具/内存/网络代理等都有指标；轮次 span 里预声明 token 用量字段；链路追踪上下文可跨代理与远程传递 |
| 附件与图片 | `attachment-store`（`AttachmentStore` trait + `InlineAttachmentStore`）、`image_preparation.rs`、`original_image_detail.rs`、`image_resize_notice` | 附件抽象成 store，重放不重新上传；图片细节按模型能力归一化；缩放会产出可识别通知片段 |
| 记忆 | `ext/memories` + `memories/read` + `memories/write` | 读侧有 `search`/`list`/`ad_hoc_note`，写侧有 phase1/phase2 两阶段与沙箱/工作区约束；记忆版本与引用（`memory_version.rs`、`memory_citation.rs`）让引用可校验 |
| 大输出 | `utils/output-truncation`（`TruncationPolicy`、`truncate_text`、`formatted_truncate_text`、`approx_token_count`）、`unified_exec` 的 `head_tail_buffer`、`hooks/output_spill` | 截断策略与输出一起记录；头尾保留而非只留开头；hook 输出溢出落盘 |
| 预算 | `rollout_budget.rs`、`token_budget.rs`、`StateDbHandle` 用量记录 | 轮次/会话/账号层面的用量都可累计并触发提醒 |

**实现细节（`rollout/src/search.rs`、`compression.rs`、`maintenance.rs`）**

- 会话搜索是“**外部工具优先 + 内置回落**”：`search_rollout_matches` 先尝试调用 `rg`（`-l --fixed-strings --ignore-case --no-ignore --glob *.jsonl`，找不到 rg 或执行失败则回落 `scan_rollout_matches` 内置扫描），并且会额外扫描**压缩后的 rollout**（`scan_compressed_rollout_matches`）；搜索词会做 JSON 转义（`json_escaped_search_term`），因为要匹配的是 JSONL 里的字段值。
- 命中上下文窗口固定（`MATCH_CONTEXT_BEFORE_CHARS = 48`、`MATCH_CONTEXT_AFTER_CHARS = 96`），返回结构是“路径 → 可选片段”的映射，便于 UI 直接展示命中行。
- rollout 压缩（1315 行）与维护（36 行的 `maintenance.rs` 只是入口）说明长会话文件是被**周期性整理**的资产；`session_index.rs`（278 行）与 `state_db.rs`（711 行）负责索引与元数据查询，搜索/列表因此不必解析全部日志。

### 11.2 agentxx

- 会话级共享存储 `SessionShareStore`（`store` 表 + 内存 3 条 LRU 缓存，首次访问只取 `max(id)`），供工具/插件放置跨轮数据；会话列表来自各会话目录 `meta`（`title` 取首条用户消息单行预览并截断 60 个 UTF-8 字符、`lastActiveMs`）。
- 检索能力靠插件：`agentxx_rag_search`（向量/关键词检索）与 `agentxx_codegraph`（代码符号图）、`tool_skill_search`（延迟加载工具的检索）；没有会话级全文检索。
- 记忆：`memory_file` 中间件按文件读写长期记忆，内容以附加段/动态段进入系统提示词；没有“记忆版本/引用校验”。
- 遥测：没有指标/追踪体系；性能相关只有 `AgentConfigStatic::enableBenchmark` 控制的一组基准标记（TUI 帧耗时等），由 `agentxx_benchmark` 消费。
- 大输出：工具级 `autoSummaryOutput`（超限自动摘要）+ 每个工具可自定义的 `SummarizationToolHandle`；没有统一的截断策略对象，也没有溢出落盘。

### 11.3 对比与迁移建议

| 维度 | agentxx | codex |
|---|---|---|
| 会话检索 | 无（只有目录 + meta） | 索引 + 反向扫描 + 分页 + 出现位置搜索 |
| 标题 | 首条用户消息预览 | 独立元数据（可改名、可按名查找） |
| 遥测 | 仅基准模式标记 | 指标 + 结构化事件 + 链路追踪 + 用量记录 |
| 附件 | store 表 + 服务端自取 + 展示剥离 Base64 | `AttachmentStore` trait + 重放不重传 + 图片预算 |
| 记忆 | 记忆文件 + 提示词注入 | 读写分离流水线 + 版本与引用校验 |
| 大输出 | 工具级自动摘要 | 统一截断策略 + 头尾保留 + 溢出落盘 |

**可迁移到 agentxx 的设计**

1. **会话搜索（建议 P1）**：先做“会话目录级”轻量搜索（标题 + 关键词命中消息计数 + 命中片段），再考虑倒排索引；数据源已经在 SQLite 里，`view_message` 加 FTS5 虚拟表即可起步。
2. **标题独立成元数据并支持改名（建议 P1）**：现在标题是首条消息预览，无法改名也无法用于查找；把它升级成 `meta.title` + `meta.titleSource`（自动/用户），列表与搜索都用它。
3. **输出截断策略统一化（建议 P1）**：把“超限自动摘要”扩展成统一策略（截断/头尾保留/落盘定位符），并由工具声明自己的策略；长输出落盘后可给模型一个定位符，需要时再读。
4. **关键路径指标（建议 P2）**：至少给“轮次耗时、首 token 时间、工具调用数、压缩次数与耗时、上下文 token 估算”加内部计数并经 wire 暴露，便于回归与用户可见的性能提示（现有 benchmark 模块已具备采集基础设施，可以复用）。
5. **记忆引用校验（建议 P2）**：记忆文件内容进入提示词时带版本号/哈希，工具读到不一致时可提示模型刷新，避免“引用了已被删除的记忆”。

---

## 12. 客户端 UI 与服务端渲染分层

### 12.1 codex：TUI 是 app-server 的客户端

- 依赖方向明确：`codex-tui` 只依赖 `app-server-protocol` 与 `app-server-client`（`tui/src/lib.rs` 的导入里没有任何 `codex_core` 的类型），因此 TUI 既能在进程内连（`InProcessAppServerClient`）也能连远端（`RemoteAppServerClient`）。
- 结构分层（`tui/src/`）：
  - `app.rs`（1157 行）编排：终端事件循环、`AppEvent`/`AppEventSender`、反向回溯（`app_backtrack.rs`）、会话恢复与选择、启动流程（`startup_*` 系列）。
  - `chatwidget.rs`（1939 行）+ `bottom_pane/*`：对话区与输入区（`chat_composer`、`footer`），`AGENTS.md` 明确要求“不要把新逻辑继续塞进这两个文件”。
  - `transcript_view.rs`/`insert_history.rs`/`live_wrap.rs`/`transcript_reflow.rs`：滚动缓冲与重排（窗口尺寸变化后重排历史）。
  - 渲染支撑：`markdown_render.rs`、`markdown_stream.rs`、`markdown_text_merge.rs`、`diff_render.rs`、`wrapping.rs`、`line_utils`、`style.rs`（styles.md 规范：优先用 ratatui 的样式助手，不用硬编码白色）。
  - 交互支撑：`keymap.rs`/`keymap_setup.rs`、`text_selection.rs`、`clipboard_copy.rs`/`clipboard_paste.rs`、`terminal_hyperlinks.rs`、`pager_overlay.rs`、`resume_picker.rs`、`theme_picker.rs`、`vim_search`、`file_search`。
  - 可访问性：`screen_reader.rs` + `screen_reader_windows.rs` 提供屏幕阅读器模式，`tests/snapshots` 有专门快照。
- **UI 变更必须带快照**：`AGENTS.md` 规定任何影响用户可见输出的改动都要有 insta 快照覆盖；本次统计 `tui/src` 下 `.snap` 文件 1324 个，这是 UI 回归成本最低的做法。

### 12.2 agentxx：TUI 直接持有 agent + 声明式组件描述

- `agent_tui` 在进程内持有一个 `io_context` 与 agent（或经 transport 连远端），`handleInterrupt` 处理 HIL 表单，`running_`/`awaitingInterruptInput_` 控制输入状态；`WireInterruptAndRunNext` 等消息在 UI 侧驱动。
- 组件层是这套系统里设计得最独立的：描述层在 `agent/lib`（`agentxx.ui.item`：文本/差异/状态图/横排/分组框/折叠/表格/树/键值/趋势图/计量条/控件/提交行/自定义），渲染实现只有一份（`agent/client/.../tui/ui_components.cpp`），插件与内置 UI 共用同一套描述；`canvas` 只做解析与纯文本降级。
- 命中与滚动：元素内子区域用 `UiHitRegion`（局部坐标）+ 滚动容器 `Scrollable::hitTestItem`，避免视口外子项残留命中区；行内 `reflect` 框由 `OwnedReflect` 持有，接入点搬动元素不会悬空。
- 表单：状态由宿主维护（`UiFormState`），点击/键盘/校验/取值只有一份实现，提交经动作通道回传 `__submit` + `{"values":{...}}`；中断表单与插件表单共用同一渲染与交互。
- 客户端插件能力面（C ABI 表）：UI 组件（`setPanelItems`/`showItemsOverlay`）、定时器（`agentxx.client.timer`：一次性/周期，区域不可见时顺延，动画等级 Disabled 时拒绝注册）、快捷键（`agentxx.client.keybind`：键位规范化、冲突先注册者优先）、装饰（`setToolDecor`）、面板（`panelItems`）。
- 其他 UI 细节：设置弹窗按 界面/显示/更新/其他 分组且内容超高可滚动；文件选择弹窗在跨设备时出现“本地/服务端”标签页；Info 侧边栏显示并切换“完全授权”；markdown 代码块按显示宽度折行（`wrap_line_by_width`，与 `estimateMarkdownLines` 同口径）。

**实现细节（`client/src/io/tui/ui_components.cpp`，2210 行）**

- 渲染是**按组件类型分派 + 一小套共享布局原语**：布局宽度/高度/间隙/内边距分别由 `colsOf`/`rowsOf`/`gapCols`/`insetsOf` 统一解析（支持 `SizeValue` 的灵活写法），文本值经 `textOf` 解析（支持上下文变量），JSON 值经 `jsonText`/`jsonOf` 转文本。
- 组件渲染各自独立：`renderTable`（列宽计算 `layoutColumnWidths` + 单元格填充对齐 `cellPadded`）、`renderKeyValue`、`renderTree`、`renderSparkline`（含 `bucketize` 分桶）、`renderProgress`、`buildDiagram`（状态图）、差异块（`diffBlockLines`）；交互控件统一走 `inputField`/`inputStepButton` 并**注册命中区域**（`addControlRegion`），按钮角色/宽度由 `buttonRoleOf`/`buttonWidth` 决定。
- 表单状态与渲染是**分开**的：`controlEditText`/`controlSelected`/`controlChecked` 只负责从 `UiFormState` 取值，控件本身不保存状态；`isFormSubmitAction` 判定提交动作 id（与中断表单共用 `middleware::kInterruptSubmitActionId`）。
- 行/子项渲染是递归的（`renderItemRows` → `renderChildren`），并把命中区域与测量结果一并写进 `UiRenderResult`——测量与渲染共用同一实现，避免“量出来的高度和画出来的不一致”（这是 TUI 里最常见的错位来源）。

### 12.3 对比

| 维度 | agentxx | codex |
|---|---|---|
| 客户端与核心 | 同进程直接持有（或经 WS 连服务端） | 只经 app-server 协议交互，进程内/远程同构 |
| 组件表达 | 声明式 JSON 描述 + 单一渲染实现（插件可写） | 原生 ratatui 组件（`Widget`），插件无法扩展 UI |
| 插件扩展 UI | 支持（组件/面板/overlay/快捷键/定时器/装饰） | 不支持（但有 hooks 与 MCP） |
| UI 测试 | 有组件/表单/接入点测试，无整屏快照 | 1324 个 insta 快照（含屏幕阅读器、主题、日志渲染） |
| 文本与排版 | markdown-ui + 折行口径统一 | markdown 渲染、流式合并、重排、超链接、文本选择、剪贴板 |
| 可访问性 | 无 | 屏幕阅读器模式（含 Windows 专门实现） |
| 大历史滚动 | lazy view + 滚动容器 | transcript view + 历史插入 + 重排 |

**优点/缺点**

- agentxx 的“声明式组件描述 + 单一渲染实现”让插件也能定义界面，这在同类项目里少见，值得保留；缺点是缺少整屏快照测试，UI 回归只能靠肉眼与局部测试。
- codex 的 UI 工程化程度高（快照、排版辅助、屏幕阅读器、主题、回溯、重排），代价是 UI 与核心隔着协议，做“临时调试面板”这类需求成本更高（要加协议字段）。

### 12.4 可迁移到 agentxx 的设计

1. **整屏快照测试（建议 P0）**：用固定尺寸 + 固定数据渲染整屏文本，存成基准文件，改动 UI 后 diff。落地：`agent/test/client/test_tui_surface.h` 已有 surface 测试基础，可加“基准文本比对 + 一键更新”模式。
2. **排版与重排工具集中化（建议 P1）**：折行、缩进、前缀、合并相邻文本这几件事已有实现（如 markdown 折行），建议收敛成公共小工具并被所有组件复用，避免各处各写一份宽度计算。
3. **可访问性预留（建议 P2）**：至少为“屏幕阅读器模式”预留开关（关闭动画、输出线性文本、避免装饰字符），并把该模式纳入快照测试。
4. **回溯/回退操作（建议 P2）**：codex 的 `app_backtrack` 允许回退到历史某点继续；agentxx 有会话与检查点基础，可考虑“从某条消息分叉新会话”的 UI 入口（与 §4.4 的派生语义配套）。

---

## 13. 远程协议、SDK 与外部集成

### 13.1 codex

- **app-server 协议**：JSON-RPC 2.0，分 v1/v2 两套（`app-server-protocol/src/protocol/{common,v1,v2}.rs`）。规范写在 `AGENTS.md` 里，包含：新 API 只加 v2、命名 `*Params`/`*Response`/`*Notification`、方法名 `<resource>/<method>` 且 resource 单数、线上字段 camelCase（配置类例外用 snake_case）、v2 字段禁止 `skip_serializing_if`、列表方法默认游标分页（`cursor`/`limit` → `data`/`next_cursor`）、时间戳用整数秒且命名 `*_at`、实验特性用 `#[experimental(...)]` 标记。
- **类型生成**：请求/响应结构体带 `#[ts(export_to = "v2/")]`，TypeScript 类型由 Rust 定义生成；schema 夹具由 `just write-app-server-schema` 更新并有测试（`schema_fixtures.rs`）。这解决了“手写两套类型”的同步问题。
- **SDK**：`sdk/typescript`（`codex.ts`/`thread.ts`/`threadOptions.ts`/`turnOptions.ts`/`events.ts`/`items.ts`/`exec.ts`，含结构化输出用 zod 的样例）、`sdk/python`。
- **无界面与远程执行**：`codex exec`（一次性/脚本化运行）、`exec-server` + `exec-server-protocol`（把命令执行放到另一台机器/另一个 OS 的远端环境，测试里有 `remote_env.rs`、`remote_env_failure_tests.rs` 与 `TestCodexBuilder::build_with_auto_env()` 的强制约定）。
- **MCP**：`codex-mcp` 既做客户端（`connection_manager`、`rmcp_client`、绑定与授权、工具目录缓存）也做服务端（`mcp-server` 暴露自身能力）；`codex_apps`/`connectors` 是“应用连接器”形态的外部能力接入（含审批与可信访问控制）。
- **Code Mode**：模型写代码（JS）来调用工具（`code-mode`、`code-mode-host`、`code-mode-protocol`、`code-mode-runtime`），协议走 gRPC；工具模式 `ToolMode::{CodeMode, CodeModeOnly}` 与嵌套工具名映射在 `ToolRouter` 里。这实质上是“工具调用的第二种表示”。
- **其他连接层**：`arg0`（同一二进制按 argv[0] 分发子命令，便于做成多入口）、`stdio-to-uds`、`tcp-tunnel`、`uds`、`websocket-auth`/`websocket-client`、`app-server-daemon`（后台守护 + 客户端重连恢复）、`app-server-test-client`（协议级测试客户端）。

### 13.2 agentxx

- wire 协议是自定义 JSON（`agent/lib/src/agent/wire_protocol.cpp` + `agentxx/protocol/*.h`）：消息有类型枚举、`toJson/fromJson` 成对实现；传输可走进程内 Channel 或 WebSocket（`agent/client` 的 `--agent` 服务端模式）。
- 协议内容围绕 UI 与交互：`WireDelta`（增量事件，含 seq）、`WireGetViewMessages`/`WireMessagesPage`（历史分页）、消息队列管理（`WireMessageQueueUpdate`/`WireClearMessageQueue`/`WireRemoveQueueItem`/`WireInterruptAndRunNext`）、中断请求/过期（`WireInterruptRequest`/`WireInterruptExpired`）、上下文查询（`WireGetContext`）、会话列表、设置读写、附件列举（`ListDir`/`ListDirResult`，跨设备时按服务端列举）、权限状态（`GetPermissionState`/`SetFullAuth`/`PermissionState`）。
- 外部集成：MCP 客户端（`protocol/mcp_client.*`）与 MCP 服务端（`mcp_server.*`）、ACP 服务端（`acp_server.*`，给 IDE 用）、A2A 客户端/服务端（`a2a_client.*`/`a2a_server.*`）、RemoteAgent（远程 agent 调用）。
- 没有：无界面一次性执行入口（`cli` 模式仍会走完整 TUI 之外的 stdio 交互）、SDK（无 TS/Python 包）、协议类型生成、协议级测试客户端。

**实现细节（`lib/include/agentxx/agent/io/wire_protocol.h`，1161 行）**

- 协议是**一层消息常量表 + 每个消息的 toJson/fromJson**：`struct MsgType` 里客户端→服务端 15 条（hello、user_input、interrupt_response、cancel、select_model、get_model、get_append_component_info、ping、compact_context、list_sessions、switch_session、clear_message_queue、remove_queue_item、interrupt_and_run_next、get_view_messages、list_dir、get_permission_state、set_full_auth），服务端→客户端 22 条（hello_ack、delta、sync、interrupt_request、interrupt_expired、turn_result、context_stats、error、log、model_info、append_component_info、get_context、context_messages、session_list、pong、plugin_data、plugin_data_up、message_queue_update、view_messages_page、list_dir_result、permission_state 等）。
- **每条消息在头文件里都有中文注释说明用途与语义**（例如 `InterruptExpired` 明确写了“客户端应把未操作的中断消息标记为过期”、`GetViewMessages` 说明“恢复长会话时初始仅同步末尾窗口，滚动时按页拉取”）。因此缺口不是“没有文档”，而是缺少**机器可校验**的东西：测试目录里没有搜到 wire 往返用例（`test_events.h`/`test_event_bridge.h` 覆盖的是事件与桥接），协议文档也没有生成机制。
- 另有两处值得注意的设计：`CloseReason{UserCancel, ClientDisconnected, Timeout}` 把“中断来源”做成明确枚举（供 `BaseAgent` 区分是用户取消、客户端掉线还是超时）；`WireDelta::Type` 有 11 种（文本/思考/工具起止/轮起止/节点起止/提示/插入消息/更新消息），事件经 EventBridge 分配会话级 seq 后下发——即“渲染指令”和“状态变更”是两类不同的下行消息。

### 13.3 app-server 请求分发（codex 实现细节）

> 精读来源：`app-server/src/message_processor.rs`

- app-server 不是“一个大 match”，而是按资源拆成约 24 个请求处理器：`ThreadRequestProcessor`、`TurnRequestProcessor`、`ConfigRequestProcessor`、`McpRequestProcessor`、`PluginRequestProcessor`、`MarketplaceRequestProcessor`、`CatalogRequestProcessor`、`AppsRequestProcessor`、`SkillsRequestProcessor`、`CommandExecRequestProcessor`、`ProcessExecRequestProcessor`、`FsRequestProcessor`、`GitRequestProcessor`、`SearchRequestProcessor`、`ProjectRequestProcessor`、`AccountRequestProcessor`、`FeedbackRequestProcessor`、`EnvironmentRequestProcessor`、`RemoteControlRequestProcessor`、`ThreadGoalRequestProcessor`、`ThreadQueueRequestProcessor`、`WindowsSandboxRequestProcessor`、`ExternalAgentConfigRequestProcessor`、`InitializeRequestProcessor`。
- 除了分发还有几层横切设施：`RequestSerializationQueues`（按 key 串行化同类请求，避免并发写同一资源）、`ConnectionRpcGate`（连接级 RPC 闸门，配合 30 秒的排空超时 `CONNECTION_RPC_DRAIN_TIMEOUT`）、`TurnAdmission`（轮次准入）、`ConnectionCapabilities`（按连接记录客户端能力）、`ThreadStateManager`（线程状态与连接映射）。
- 后台工作线程也在这里启动：模型列表刷新（`ModelsRefreshWorker`）、轮次成本统计（`TurnCostWorker`）、技能目录监视（`SkillsWatcher`）、文件系统监视（`FsWatchManager`）、MCP 事件流（`McpEventStreams`）、Code Mode 会话提供者。
- 协议入口严格：`deserialize_client_request` 会先 `reject_obsolete_request_fields`（拒绝已废弃字段），再按 v1/v2 与实验特性标记校验（`ExperimentalApi`/`experimental_required_message`），错误统一映射为 JSON-RPC 错误码（`invalid_params`/`invalid_request`/`internal_error`）。
- 这些名字本身就是一张“一个成熟 agent 服务端需要哪些能力”的清单，可直接用于对照 agentxx 的 wire 协议缺口（配置读写、文件系统、搜索、市场/插件、账户、诊断、远程控制等目前 agentxx 都没有独立消息）。

### 13.4 对比

| 维度 | agentxx | codex |
|---|---|---|
| 协议形态 | 自定义 JSON + 手写编解码，围绕 UI 交互 | JSON-RPC 2.0，v1/v2 分层，围绕“会话/轮次/配置”资源 |
| 类型同步 | 手写（C++ 结构体 ↔ JSON） | Rust 定义 + TS 生成 + schema 夹具测试 |
| 客户端种类 | TUI/CLI + 客户端插件 | TUI/exec/SDK/IDE/守护进程/远程 exec |
| 无界面入口 | 无独立入口 | `codex exec`、`exec-server` |
| 远程执行 | 不支持（远程只传消息，命令在服务端宿主执行） | 支持（`exec-server` 远程环境，可跨 OS） |
| MCP | 客户端 + 服务端 | 客户端 + 服务端 + 应用连接器 |
| 其他协议 | ACP、A2A、RemoteAgent | Code Mode gRPC、WebSocket 鉴权、UDS/TCP 隧道 |
| 测试客户端 | 无（用自研测试覆盖） | `app-server-test-client` + 协议级集成测试 |

**优点/缺点**

- codex 的协议工程化程度高（生成类型 + 夹具 + 测试客户端 + 守护恢复），代价是协议面大（v1/v2、两个 SDK、连接器、Code Mode），维护成本高。
- agentxx 的协议更贴近自己的 UI 需求（分页、队列、中断、权限状态都有专门消息），缺点是**手写同步风险**与**缺少无界面/SDK 入口**——外部工具想集成只能走 WS 协议自己实现。

### 13.5 可迁移到 agentxx 的设计

1. **协议文档化与一致性测试（建议 P0）**：为每条 wire 消息补“方向、字段、语义、错误”的表格文档，并加一条“所有消息类型都有 `toJson`/`fromJson` 往返测试”的用例（现有 `test_events.h`/`test_event_bridge.h` 可作为落点），防止新增消息漏实现或字段不一致。
2. **无界面执行入口（建议 P1）**：提供 `agentxx_cli run --prompt ... --output json` 这类一次性入口（复用现有 agent 与 session 存储，只用 stdio 输出结构化事件），这是最容易被外部脚本与 CI 采用的能力。落地：`agent/client/main.cpp` 新增子命令 + `io/stdio` 端点复用。
3. **协议类型生成（建议 P2）**：从 C++ 结构体定义生成客户端类型（至少生成 TS 与文档），减少跨端手写。
4. **远程执行环境（建议 P2，需评估）**：若将来支持“客户端在 Windows、命令在 Linux 容器执行”，建议先做“执行环境”抽象（命令、工作目录、环境变量、结果编码），再把现有 `execute_command` 迁移过去，而不是把远端细节塞进权限中间件。

---

## 14. 测试与质量门禁

### 14.1 codex

- **集成测试为主**：`core/tests/suite` 下 201 个 `.rs` 文件，覆盖从 `exec.rs`、`apply_patch_cli.rs`、`tool_parallelism.rs` 到 `scenarios_*`（共享指令、邮箱抢占、MCP 资源消息、Windows MXC 等）的多层场景；`AGENTS.md` 规定“改变 agent 逻辑的功能必须加集成测试”，并给出需要测试的行为清单。
- **模型模拟**：`core_test_support::responses` 提供 mock SSE 服务（`mount_sse_once`、`ev_*` 构造器、`ResponseMock` 可断言发出去的请求体），`TestCodexBuilder` 提供带环境自适配的实例（`build_with_auto_env()`，多 OS 远程测试要求）；断言偏好 `wait_for_event` 而非自定义超时。
- **UI 快照**：TUI 1324 个 insta 快照，UI 改动必须更新快照，且提供 `cargo insta pending-snapshots` / `show` / `accept` 的固定流程。
- **测试组织约定**：新测试模块放在独立 `*_tests.rs` 并用 `#[path = "..."]` 引入；用 `pretty_assertions::assert_eq`；尽量深比较整个对象；不要在测试里改进程环境；需要在测试中启动仓库二进制时用 `codex_utils_cargo_bin::cargo_bin`（Cargo 与 Bazel 下路径都成立）。
- **静态检查**：clippy 配置在 `codex-rs/clippy.toml`，仓库级禁止事项明确（`collapsible_if`、`uninlined_format_args`、`redundant_closure_for_method_calls`）；库代码 `#![deny(clippy::print_stdout, clippy::print_stderr)]`、`#![deny(clippy::disallowed_methods)]`；`AGENTS.md` 还要求“不要写只有一个调用点的小助手函数”“避免 bool/Option 位置参数，必要时用 `/*param_name*/` 注释并由 Bazel 的 lint 校验”。
- **基准**：divan 写基准，`just bench` / `just bench-smoke`；`justfile` 是统一入口（`just test -p xxx`、`just fix -p xxx`、`just fmt`、`just write-config-schema`、`just write-app-server-schema`）。
- **双构建**：cargo（日常）与 Bazel（CI/多平台）。`AGENTS.md` 专门提醒 Bazel 不会自动把源码树文件提供给编译期读取（`include_str!`/`sqlx::migrate!` 需在 `BUILD.bazel` 声明 `compile_data`），这类“只在另一套构建里失败”的坑被写进了规范。

**实现细节（`core/tests/common/responses.rs`）**

- 测试基建是 **wiremock + 自建请求捕获器**：`ResponseMock` 保存所有发往 `/responses` 的请求，并提供 `single_request()`（数量不为 1 直接 panic，逼出“意外多请求”这类回归）、`requests()`、`last_request()`、`saw_function_call(call_id)`、`function_call_output_text(call_id)` 等断言点；测试可直接对请求体断言 `parent_turn_id`/`root_turn_id`（多代理归因）。
- WebSocket 场景用 `tokio_tungstenite` 起真实握手服务（含 deflate 压缩配置），因此 SSE 与 WebSocket 两条传输都在覆盖范围内；配合 `TestCodexBuilder::build_with_auto_env()` 保证跨 OS 场景可跑。

### 14.2 agentxx

- 自研测试可执行 `agentxx_test`：按模块组织（`agent/test/include/agentxx-test/{core,plugin,client}/...`，本次统计约 80 个 `test_*.h`），支持只跑指定模块与 `--fail-fast`；测试分组覆盖核心（agent、session_persistence、summarization、cancel、concurrency、subagent、events、mcp、acp、a2a、http、websocket）、插件（runtime/sdk/multi_instance/resources/bridge/client_plugins）、客户端（TUI 组件、表单、框架、markdown、mermaid、设置、输入、滚动、主题、更新检查）。
- Debug 默认开启 ASan + UBSan（`AGENTXX_ENABLE_SANITIZER`），并给插件框架加定向探针；Release 关闭。提示：sanitizer 与 mimalloc 互斥，后者当前默认关闭。
- 性能/资源基准 `agentxx_benchmark`：多场景（同进程 CLI/TUI、真实两进程、真实 TUI 帧耗时、真实 server 空载、PTY 驱动 TUI、插件边际内存），每场景独立子进程隔离，指标含 RSS/PSS、私有脏页、匿名、峰值、线程、fd、glibc 堆在用与碎片、malloc_trim 可回收、smaps 分模块（可执行文件/项目库/各插件/系统库/堆/匿名）、逻辑内存、分阶段增量、CPU 用户/内核；报告 JSON（机器对比）+ MD（人工阅读），支持 `--baseline` 输出差值与模块级差异。
- 文档化程度高：`docs/zh-cn/design/benchmark.md`（556 行）记录了实测数据与结论（mimalloc 开关取舍、`mallopt` 与轮末归还页的实测收益等），把“性能决策”也留了证据。

**实现细节（`agent/test/include/agentxx-test/test_framework.h`）**

- 测试框架把“跨平台定位可执行文件目录”做成了公共工具（`executableDir()`：Linux 读 `/proc/self/exe`、macOS 用 `_NSGetExecutablePath`、Windows 用 `GetModuleFileNameW`），这样从任意工作目录跑测试都能找到 exe 同目录的插件产物——这正是插件测试能在 CI 里稳定运行的隐含前提。结果统计用 `TestResult{passed, failed}` 累加，模块按 `core`/`plugin`/`client` 分组注册在 `test.cpp`。

### 14.3 对比

| 维度 | agentxx | codex |
|---|---|---|
| 集成测试 | 有（agent/session/plugin/协议层），以模块为单位 | 201 个套件 + 协议级测试客户端 |
| 模型模拟 | 有 provider 测试与本地 mock | mock SSE 服务器 + 请求体断言（成熟） |
| UI 测试 | 组件/表单/接入点测试 | 1324 张整屏快照 |
| 动态检查 | ASan/UBSan + 插件探针 | clippy 严格规则 + 自定义 lint + 双构建一致性检查 |
| 性能测试 | 资源基准（RSS/PSS/堆/smaps/CPU，分场景隔离） | divan 基准（吞吐/延迟） |
| 文档化 | 设计文档 + 实现记录（30 个 history 目录） | 规范写入 AGENTS.md，API 变更流程固定 |

**优点/缺点**

- agentxx 的**资源基准**比 codex 更贴近“常驻内存与长会话”这类问题（PSS/smaps/插件边际内存、每场景独立子进程），这是很实用的差异点；缺点是没有 UI 快照与“请求体级”的协议断言。
- codex 的**快照 + mock 模型 + 请求断言**组合使“行为回归”几乎自动化；缺点是测试基础设施本身也占大量代码，且 Bazel/cargo 双套构建带来额外认知成本。

### 14.4 可迁移到 agentxx 的设计

1. **UI 整屏快照 + 一键更新**（建议 P0，与 §12.4 第 1 条同一项）：把 surface 测试升级为“基准文件比对”，任何 UI 改动先看 diff。
2. **请求体级断言（建议 P1）**：现有 provider 测试可加“捕获发往模型的实际请求 JSON 并断言结构”（工具 schema 形状、消息顺序、system 消息内容），这能抓住“提示词与工具注册悄悄变化”这类问题，也是 schema 严格网关问题的早期预警。
3. **模块化回归清单（建议 P1）**：在 `resource/history/` 的条目里固定记录“本次改动会影响哪些行为、对应哪个测试模块”，形成可检索的回归映射（现有 30 个历史条目已经接近这个形态）。
4. **基准对比进 CI/本地例行（建议 P2）**：`--baseline` 已支持机器可读对比，可考虑在发布前固定跑一次资源基准并与上次对比，异常时给出模块级差异（插件边际内存尤其适合这种方式）。

---

## 15. 配置、特性开关与装配体系

### 15.1 codex：配置分层栈 + 生成式 schema + 特性阶段

- **配置来源分层**：`codex-config` crate 的 `ConfigLayerStack` 组织系统级、托管级（企业/requirements）、用户级（`~/.codex/config.toml`）、项目级与会话覆盖；`Session::refresh_runtime_config_inner` 只把**用户层**从新快照刷新进来，保留会话本地的覆盖层（这解释了为什么“重新读配置”不会清掉本轮的临时覆盖）。
- **生成式 schema**：`core/config.schema.json` 由代码生成（`just write-config-schema`），`config-schema` crate 负责校验；`AGENTS.md` 把“改了 `ConfigToml` 就要跑生成”写成硬性要求。
- **特性开关（Feature）**：`features` crate 定义 `Feature` 与阶段（含实验菜单描述），`ManagedFeatures` 支持约束与回滚；`build_model_client_beta_features_header` 把开启的实验特性拼成请求头——**开关状态是外显的**，服务端据此调整行为。此外有 `unstable_features_warning_event` 向用户提示不稳定特性。
- **权限档案的配置化**：`config/permission_profile_selection.rs`、`permission_profile_catalog.rs`、`resolved_permission_profile.rs`、`requirements.rs` 把“档案选择 + 企业要求 + 解析结果”分开，`PermissionProfileSnapshot` 可持久化与比较。
- **app-server 侧配置管理**：`app-server/src/config_manager*.rs` 提供 config 读写/列表 RPC（线上字段用 snake_case 以对齐 `config.toml` 键名，这是规范里唯一的例外），`config_layer.rs` 暴露分层结构；`cloud-config` 支持从远端下发配置包（`CloudConfigBundleLoader`）。
- **跨环境一致性**：`codex_home` 解析（`find_codex_home`）、`install-context`、`arg0`（多入口二进制）、`process-hardening`（进程加固）、`keyring-store`/`secrets`（凭据存储）都是为“同一份配置在 CLI/IDE/桌面/服务端表现一致”服务的。

### 15.2 agentxx：两层 yaml + 显式合并语义

- **两层加载**：base（`data_dir` 目录下的 `agentxx-config.yaml` / `.env`，未配置 data_dir 时取 `~/.agentxx/`）+ overlay（工作目录或 `--config` 指定的 yaml + 同目录 `.env`）。
- **合并语义显式**：列表段统一为 `{overwrite: {mode: merge|replace, remove: [...]}, list: [...]}`——按身份归并/追加去重、整段替换、按身份剔除；标量为覆盖、映射逐键合并；`.env` 同名变量取 overlay 值；旧键与纯列表写法会告警并忽略。这套设计比“深合并”这类隐式规则更容易推理。
- **分层职责**：`models`（模型列表与默认值）、`plugins`（插件路径/名称与启用状态）、`mcp`（按 namespace）、提示词与工具配置、`data_dir`、语言、权限模式、worktree、子代理开关等，都有明确的键；UI 侧设置（主题、快捷键提示、更新检查）另存 `settings_db`。
- **插件配置经接口读**：插件通过接口表读取自己的配置与资源，卸载时资源释放由宿主协调（`resource_applier`、`applyDeclaredResources`/`releaseInstanceResources`）。
- 目录层面还有 `AgentConfigStatic` 统一提供数据目录、会话目录、临时目录等派生路径，避免各处自行拼路径。

### 15.3 对比

| 维度 | agentxx | codex |
|---|---|---|
| 配置形态 | 两层 YAML（base + overlay），显式合并语法 | 多层 stack（系统/托管/用户/项目/会话），TOML |
| schema | 手写文档 + 代码校验（键名告警） | 生成式 JSON schema + 校验 crate |
| 特性开关 | 配置项 + 编译期平台开关 | `Feature` 枚举 + 阶段 + 托管约束 + 外显请求头 |
| 企业/托管策略 | 无（可自行下发 base 配置） | requirements/managed 层 + 权限档案目录 |
| 配置读写 API | 客户端读写设置（`settings_db`） | app-server 提供 config 读/写/列表 RPC（含分层结构） |
| 远端配置 | 无 | 云端配置包下发 |

### 15.4 可迁移到 agentxx 的设计

1. **生成式配置 schema（建议 P1）**：从 C++ 的配置结构体生成 JSON schema 与文档（字段、类型、默认值、取值范围），客户端与插件都能用同一份定义校验；现在依赖手写文档与告警。
2. **配置来源标记（建议 P1）**：加载后为每个键保留“来自 base 还是 overlay、是否被忽略”的信息并暴露给 UI/日志（现在只有启动期告警），排障时不用猜。
3. **特性阶段与约束（建议 P2）**：把实验特性做成显式清单（名称、阶段、是否对外提示），避免散落的布尔配置项无人清理。
4. **配置读写 API 覆盖分层结构（建议 P2）**：若将来支持“设置面板修改配置并落回文件”，建议一次性设计好分层写回规则（哪些键写到 overlay，哪些必须写 base），否则会出现改不动或改错层的问题。

---

## 16. 扩展机制专题：五种扩展面 vs C ABI 插件

### 16.1 codex 的五种扩展面

| 扩展面 | 边界 | 表达能力 | 隔离/风险 |
|---|---|---|---|
| 扩展 crate（`ext/*` + `codex-extension-api`） | 编译期、同进程、强类型 | 可贡献上下文片段、工具生命周期回调、配置变更回调、用量回调、世界状态段 | 与核心同权限，靠代码评审控制 |
| Hooks | 外部进程 + JSON 协议 | 12 个事件（`PreToolUse`、`PermissionRequest`、`PostToolUse`、`PreCompact`、`PostCompact`、`SessionStart`、`SessionEnd`、`UserPromptSubmit`、`SubagentStart`、`SubagentStop`、`Stop`、`Interrupt`） | 任意命令，有信任开关与输出溢出落盘 |
| Skills | 目录 + Markdown（frontmatter 描述/接口/依赖） | 让模型按需加载“能力说明与操作步骤”，可依赖 MCP 工具 | 纯文本，风险在于内容诱导 |
| MCP | 协议（本地/远端服务器） | 动态工具、资源、审批（elicitation） | 独立进程/远端，有授权与可信访问控制 |
| 插件市场包 | npm 源 / bundle 包 + manifest | 可提供命令、hooks、MCP 覆盖、脚本、推荐项；有安装/启用策略 | 需要策略与归属信息（脚本归属、命令迁移） |

- 钩子/技能/插件都是**声明式**的：注册是列出清单（hooks JSON、SKILL.md、manifest），核心只按名称与匹配器调度，因此跨语言、跨进程都成立。`hooks/src/lib.rs` 里 `HOOK_EVENT_NAMES` 是 12 个字符串常量，另有一份“哪些事件使用 matcher”的说明。
- 扩展 crate 与其它面互补：需要“改行为”的进 crate，需要“用户可配置”的走声明式。

**实现细节（`hooks/src/engine/*`）**

- hooks 引擎按职责拆成 6 个模块：`discovery`（发现配置里的 hooks）、`schema_loader`（加载/校验 hooks JSON schema）、`command_runner`（执行外部命令 handler）、`mcp_runner`（把 MCP 工具当 handler 执行）、`output_parser`（解析 handler 输出为结构化结果）、`dispatcher`（选择并调度 handler）。
- handler 有两类来源（`HookHandlerType`：command / mcp 工具），带执行模式（`HookExecutionMode`）、作用域（`HookScope`）与“内置/来源路径/显示顺序”等元信息；每次运行都会生成 `HookRunSummary`（状态、开始/完成时间、耗时、条目），因此 UI 能展示“哪个 hook 跑了、跑了多久、结果如何”。
- 事件与匹配器有明确规则：`PreToolUse`/`PermissionRequest`/`PostToolUse`/`SessionStart`/`SessionEnd`/`SubagentStart`/`SubagentStop`/`PreCompact`/`PostCompact` 参与匹配器判断，而 `UserPromptSubmit`/`Stop`/`Interrupt` **忽略匹配器**（永远选中）；同一 handler 即使能被多个兼容名（如 `apply_patch|Write|Edit`）匹配，也**只执行一次**（源码注释专门解释了这一点，避免一次工具调用触发多份同样的 hook）。
- 执行是并行的（`FuturesUnordered` 风格的批量执行），但记录 `completion_order` 以保持结果可解释；`parsed` 结果与完成事件一起返回，供调用方决定是“追加上下文”“阻止本次工具调用”还是“要求继续”。

### 16.2 agentxx 的扩展面

- **C ABI 插件**（主扩展面）：导出 `agentxx_plugin_{agent,client}_{get_info,create,start,stop,destroy}` 五个入口；能力经**接口表按名称查询**（agent 侧 19 张：10 张通用表 + `tools`/`permission`/`hooks`/`session`/`model`/`prompt`/`resources`/`graph`/`context`；client 侧 9 张）。生命周期契约明确：`create` 只构造上下文、`start` 是注册事务（失败回滚）、`stop` 撤销自管资源、`destroy` 只释放本地对象；多实例契约（禁止可变全局、状态放实例堆块、接口表查询结果存上下文）。
- **中间件**（编译期内扩展）：7 个钩子点（agentcall/modelcall 的 start/run/end、toolcall 的 start/end），按会话分片保存状态；内置能力（权限、压缩、技能、记忆文件、子代理、中断 UI）本身就是中间件。
- **节点与图**：插件可注册/修改执行图（`PluginGraphNode` + `agentxx.agent.graph` 表），节点类型注册走 per-agent `GraphRegistry`（不依赖全局工厂）。
- **客户端插件**：可注册 UI 组件/面板/overlay、定时器、快捷键、工具装饰，并有能力协商（老宿主缺失能力即降级）。
- **权限与资源声明**：插件声明工具权限与资源需求，卸载时自动撤销（这是 codex 声明式扩展面里没有对等物的部分）。

**实现细节（`lib/src/plugins/tool_registry.cpp`、`plugin_manager_lifecycle.cpp`、`lib/src/tools/subagent.cpp`、`wire_protocol.h`）**

- **工具注册表的冲突规则**（`tool_registry.cpp`）：`registerTool` 先判空名/空指针，再判“表内已有同名”与“与内置静态工具名冲突”（`staticToolNames_` 由 `BaseAgent` 初始化时注入），两者都只记警告并返回 `false`，不覆盖；`unregisterTool` 返回被摘除的 `shared_ptr`（调用方可据此与 inflight 计数配合，等计数归零再释放）；`appendDefinitions` 把插件工具定义追加进模型可见工具表。对照 codex：codex 用 `tool_policy.allows` + 保留名 + `first_collision`，两者思路一致，agentxx 少的是“策略过滤”与“暴露等级”。
- **插件生命周期接缝**（`plugin_manager_lifecycle.cpp`，445 行）说明生命周期骨架已在内核（`pluginxx::PluginHostLifecycle`），宿主只实现接缝：`createInstance`、`detachDomainRegistrations`（拆领域表注册）、`detachDomainOwnedResources`（拆领域资源）、`clearDomainRegistrations`、`releaseInstanceResources`、`onInstanceEnabledChanged`（禁用/启用时的中间件与工具取舍）、`loadNativeAsync`/`loadBuiltinAsync`/`loadPluginAsync`、`flushPendingCleanup`（轮末摘除待清理中间件，异常路径自愈）、`eraseMiddleware`。这几个函数名本身就是“卸装插件时到底要收回什么”的清单。
- **子代理批量委派参数**（`subagent.cpp` 的 `parseSubagentBatchFromInterrupt`）：每个任务可带 `subagent`、`system_prompt`、`message`、**`messages`（结构化消息前缀，用于同上下文模式）**、`sessionId`（在父会话上下文里跑）、`tools`（无工具/继承父/自定义）、`enable_summarization`（是否允许对该子代理再做压缩）、`result_id`（结果标识，缺省按序号兜底）；同时兼容“单发参数”与 `{tasks: [...]}` 批量两种形态，批量即并行。与摘要中间件复用的是同一条路径，所以“压缩子代理”和“用户发起的子代理”语义一致。
- **协议侧**：`CloseReason` 区分 `user_cancel`/`client_disconnected`/`timeout`，`WireDelta` 11 种类型区分“渲染指令”与“状态变更”；消息常量表带逐条注释（见 §13.2）。

### 16.3 对比

| 维度 | agentxx | codex |
|---|---|---|
| 跨语言 | 支持（纯 C ABI，插件可用任意语言） | hooks/MCP 支持跨语言；crate 扩展只能 Rust |
| 类型安全 | 靠接口表 + 版本号手动约定 | 扩展 crate 强类型；声明式面靠 schema |
| 编译期扩展 | 中间件（需同库编译或改代码） | 扩展 crate（工作区内新增 crate 即可） |
| 用户可写扩展 | 需要写动态库 | hooks/skills/插件包（文本或 npm 包） |
| 事件面 | 事件总线主题（interrupt/subagent/…）+ 中间件钩子 | 12 个 hook 事件 + 扩展 crate 回调 |
| 动态工具 | 插件工具 + MCP | MCP + 动态工具 + 扩展 crate 工具 + CodeMode |
| 安装/分发 | 放入插件目录，由 `plugin.list` 装配 | 市场/包管理 + 推荐安装 + 策略控制 |
| 隔离 | 插件为独立动态库（同进程），有实例契约 | hooks 为独立进程，MCP/插件可为远端 |

**优点/缺点**

- agentxx 的插件体系在**跨语言与接口稳定性**上更彻底（C ABI + 版本化接口表 + 能力协商），并且插件能扩展 UI、权限、图结构——表达面比 codex 的任何单一扩展面都宽；缺点是每一个宿主-插件接触面都要手工设计 ABI，插件数量多起来后版本兼容成本高。
- codex 的多面并存让不同需求各走其道（改行为→crate，加能力→MCP/skill，用户自动化→hooks，分发→市场），缺点是概念多、边界要靠文档约束；好处是**没有把所有需求都塞进一个 ABI**。

### 16.4 可迁移到 agentxx 的设计

1. **Hooks 式外部命令扩展（建议 P1，价值最高）**：为关键事件（工具前后、轮次前后、会话开始/结束、压缩前后、中断）提供可配置的外部命令挂钩，输入输出用 JSON 协议。这让用户不必写 C++ 动态库就能做审计、代码格式化、环境准备等自动化。落地：新增 `agentxx.agent.hooks` 接口表 + 中间件内的调度点 + 配置段（参考现有 yaml 分层与插件资源声明）。
2. **声明式能力清单（建议 P1）**：插件的权限/资源声明已经是声明式的，可扩展到“插件可声明的 hook 事件、可声明的命令、可声明的推荐项”，形成一张可审阅的清单（类似 codex 的 manifest + hook declarations）。
3. **接口表版本策略文档化（建议 P2）**：把“接口表如何演进、旧插件如何降级、能力协商如何在缺表时回退”写成明确规则并配测试（现有 `plugin_sdk`/老宿主降级的端到端测试是很好的基础）。
4. **技能/提示式扩展与插件分离（建议 P2）**：agentxx 的 skill 中间件已经在做“文本能力包”，可参考 codex 的 frontmatter（说明/接口/依赖）让技能声明自己需要哪些工具与依赖，从而在加载期做校验而不是运行期失败。

---

## 17. 迁移建议汇总

### P0（明确收益、成本可控，建议优先做）

1. **运行中输入分级（next-turn / next-step）**：把 steering 变成协议与循环的一等语义（§2.4-1）。
2. **工具级并行声明**：`supportsParallel()` + 读写闸门，同一条 assistant 消息内的多个 tool_call 可并发（§5.4-1）。
3. **UI 整屏快照测试**：固定尺寸 + 固定数据的基准文本比对与一键更新（§12.4-1、§14.4-1）。
4. **权限规则引擎与审批缓存**：把“询问并记住”的结论落成按程序/路径前缀/网络的规则表，命中即跳过询问（§9.4-1、§5.4-4）。
5. **会话目录写者锁**：`try_lock` 失败即拒绝二次打开，避免两端互写（§4.4-1）。
6. **注入内容的类型标记**：附件/技能/记忆/动态上下文带稳定来源键与分类，供压缩、去重、审计、UI 使用（§3.4-1、§6.4-1）。
7. **协议一致性与文档**：wire 消息常量已在 `wire_protocol.h` 逐条带注释，缺的是**机器可校验**的一层——为每条消息补 toJson/fromJson 往返用例（测试目录暂未搜到）与生成式字段文档，防止新增消息漏实现或字段不一致（§13.2、§13.5-1）。
8. **子代理并发上限与生命周期守卫**：RAII 守卫 + 排队，避免无界并发（§10.4-1）。
9. **模型能力信息集中化**：把图片细节、并行工具、严格 schema、上下文窗口等能力整理进模型条目（§7.3-1）。
10. **“新功能新模块”纪律与边界说明**：写进项目文档，避免核心库继续变胖（§1.4-3）。
11. **采样请求抢占（输入即刻生效）**：新输入到达时取消当前采样请求、保留已收内容并按同一请求续跑，让插话在本次采样就进入上下文，而不是等整轮结束（§2.4-6）。
12. **执行环境加固**：命令执行时固定注入 `NO_COLOR`/`TERM=dumb`/`C.UTF-8`/`PAGER=cat`/`CODEX_CI` 与 `CODEX_THREAD_ID` 等变量，消除分页器卡死、ANSI 颜色与编码问题（§10.4-5）。

### P1（收益明确，需要一定设计与迁移）

1. **压缩时机前置 + 恢复元数据**：轮前预判压缩、记录摘要区间与口径（§8.4-1、§8.4-2）。
2. **可变提示词段落的差分注入**：稳定段/易变段拆分，易变段改写历史差分（§6.4-2）。
3. **外部命令 hooks**：工具/轮次/会话/压缩事件的可配置挂钩（§16.4-1）。
4. **会话搜索与标题元数据**：FTS 起步、标题独立成元数据并支持改名（§11.3-1、§11.3-2）。
5. **无界面执行入口**：一次性运行 + 结构化输出，便于 CI 与脚本集成（§13.5-2）。
6. **输出截断策略统一化**：截断/头尾保留/溢出落盘定位符，策略随消息记录（§11.3-3、§5.4-2）。
7. **错误分类与工具错误语义**：参数错误 vs 致命错误分离（§5.4-3）。
8. **请求体级断言测试**：捕获真实请求 JSON 并校验结构（§14.4-2）。
9. **耐久语义分级（PersistContext 风格）**：把“必须同步”与“可延后”显式化并命名（§4.4-2）。
10. **配置来源标记与生成式 schema**：键级来源信息 + 生成式校验（§15.4-1、§15.4-2）。
11. **派生（fork）上下文**：子代理可带父会话最近 N 轮或摘要起步（§10.4-2）。
12. **可继续的后台作业**：作业 id/状态/输出增量/终止，供长任务使用（§10.4-3）。
13. **越权尝试留痕**：声明与实际不符时记录结构事件（§9.4-2）。
14. **声明式能力清单**：插件可声明 hook 事件/命令/推荐项（§16.4-2）。
15. **消费端退出传播取消**：流被放弃时自动取消上游（§7.3-2）。
16. **工具注册表治理规则**：保留名、冲突记录、暴露等级（§5.4-7）。
17. **并发工具的取消语义**：已完成保留、未完成补占位、执行中立即中止（§5.4-9）。
18. **权限判定的符号链接缺口**：对已存在路径做一次 `weakly_canonical` 复核（§9.4-6，安全相关）。
19. **压缩保留用户消息与摘要标记**：替换历史时保留用户消息，摘要带固定前缀与区间标记（§8.4-5、§8.4-6）。
20. **规则自带样例校验**：路径/命令规则附正例反例并在加载期校验（§9.4-9）。

### P2（长期演进，需评估）

1. 换窗与摘要分离（§8.4-4）。
2. 系统沙箱适配层（bubblewrap/Seatbelt，可用则收窄）（§9.4-5）。
3. 轮次内提权请求（§9.4-4）。
4. 协议类型生成（TS/文档）（§13.5-3）。
5. 远程执行环境抽象（§13.5-4）。
6. 派生/分叉的持久化语义（§4.4-5、§12.4-4）。
7. 可访问性模式与快照覆盖（§12.4-3）。
8. 接口表版本策略文档化（§16.4-3）。
9. 技能依赖声明与加载期校验（§16.4-4）。
10. 关键路径指标与基准对比例行化（§11.3-4、§14.4-4）。
11. 记忆引用校验（§11.3-5）。
12. 提示词槽位顺序规则（§6.4-4）。
13. 工具发现与命名空间预算（§5.4-5）。
14. 配置分层写回规则（§15.4-4）。
15. 参数流式差分展示（§5.4-6）。
16. 工具调用分段计时与来源标记：派发等待/执行/总耗时，仅直连调用计时（§5.4-8）。
17. 提权作用域分离与求交规则：会话级/轮次级分开存放，批准结果取交集，自动审查下禁止长期提权（§9.4-7、§9.4-8）。
18. 压缩兜底可观测：`hardTruncate` 记成结构化事件（原因、丢弃条数、是否二分截断）（§8.4-7）。
19. 服务端能力清单对照：配置读写、文件系统、搜索、诊断、市场/插件等 RPC 面（§13.3）。
20. 展示历史写路径分级：区分“追加/更新/删除”操作并批量提交，而不是每次全量重写（§4.5）。
21. 只读快照共享：历史/上下文读取走引用计数快照，避免按值深拷贝（§3.4-5）。
22. 客户端状态分层与粘性令牌：区分会话级/轮次级状态，令牌只在同一轮内回传（§7.3-5）。
23. 对外 API 语义清单文档化：每个公开方法写清“会做什么/不会做什么/何时拒绝”（§10.4-6）。

---

## 18. 反向清单：agentxx 不必照搬的设计

- **不要把全部能力都做成“声明式扩展面”**：codex 的 hooks/skills/MCP/插件包并存是它历史与生态的结果；agentxx 的 C ABI 插件已经覆盖“加能力”这一层，盲目再加三四套会带来概念与测试负担。若要新增，优先只加 **hooks（外部命令）** 一种，其余走现有插件与技能。
- **不要引入 JSONL 日志 + SQLite 索引双写**：这是 codex 为了“重放正确 + 查询快”付的代价；agentxx 的会话库结构简单且已有版本与节流机制，只需补写者锁、迁移记录与整理策略即可，不必改成事件日志。
- **不要照搬 codex 的 crate 粒度**：150 个 crate 是大型团队并行开发的产物；agentxx 用“目录 + 明确依赖方向 + 文档约束”能达到同样效果，拆成几十个小库反而增加构建与发布负担。
- **不要为了对齐 1:1 引入“步骤（step）”的完整状态机**：agentxx 的图节点已经是步骤边界的天然载体；只需在“模型/工具集/权限可能在一轮内变化”的地方补最小快照，避免引入第二套生命周期。
- **不要照搬权限档案/审批/规则三层概念**：agentxx 的用户模型（消息队列 + 中断表单 + 完全授权）更贴近它的客户端形态；只需补规则持久化与缓存，不必引入档案对象与能力协商矩阵。
- **不要追求无差别平台沙箱**：Windows/Android 上没有等价能力；若做，做成“可用则收窄 + 不可用则记录原因”的可选层，避免平台差异渗透到工具实现。
- **不要用“指标全覆盖”代替可观测性设计**：agentxx 目前缺遥测，正确做法是先定义少量关键口径（轮次耗时、首 token、工具数与失败数、压缩次数与耗时、上下文估算），并保证它们能被测试与 UI 使用，而不是先铺埋点。

---

## 19. 结语

对比下来，两个项目在两个方向上各有取舍：

- **codex 的强项是“把不确定性关进笼子”**：模型可见状态做成可快照/可差分的 section；请求做成有明确模式的投递；权限、沙箱、审批、重试收敛到编排器；持久化做成可重放的日志；UI 变更必须过快照。它的代价是组件多、类型多、平台差异多，需要规范（`AGENTS.md`）持续约束。
- **agentxx 的强项是“把机制做薄”**：会话即上下文权威、图引擎承载 ReAct、中间件承载横切关注点、C ABI 插件承载能力扩展、声明式组件描述承载 UI。它的代价是把若干语义留在了实现细节里（轮次语义、注入来源、权限规则、持久化边界），这些正是本文 P0/P1 清单要补的部分。

三条可迁移的工程原则（与具体模块无关）：

1. **语义显式化**：能被请求方观察到的结果（接受/拒绝及原因）、能被后续阶段识别的注入内容、能被用户配置的规则，都应当有名字与结构，而不是靠注释和约定。
2. **危险动作留痕**：越权尝试、被拒绝的调用、压缩与取消的决策，都应留下可检索记录；这既是排障依据，也是安全底线。
3. **变更走证据**：UI 改动用快照说话，行为改动用请求断言与集成测试说话，性能改动用基准与基线对比说话；没有证据的“优化”与“修复”最终都会回到原点。

---

## 附录 A：关键文件与文档索引

### A.1 agentxx

| 模块 | 路径 |
|---|---|
| Agent 基类与循环 | `agent/lib/include/agentxx/agent/base_agent.h`、`agent/lib/src/agent/base_agent.cpp` |
| 轮次执行与中断恢复 | `agent/lib/src/agent/agent_runner.cpp` |
| 会话与上下文 | `agent/lib/include/agentxx/agent/context.h`、`agent/lib/include/agentxx/nodes/session_context.h` |
| 提示词 | `agent/lib/include/agentxx/agent/prompt.h`、`agent/lib/src/agent/prompt.cpp` |
| 工具基类与分发 | `agent/lib/include/agentxx/tools/tool.h`、`agent/lib/src/nodes/toolcall.cpp` |
| 中间件与钩子 | `agent/lib/include/agentxx/middlewares/middleware.h`、`permission.h`、`summarization.h`、`subagent_manager.h`、`skill.h`、`memory_file.h`、`interrupt_ui.h` |
| 持久化 | `agent/lib/src/agent/session_store.cpp`、`checkpoint_store.cpp`、`agent/lib/include/agentxx/util/settings_db.h` |
| 协议 | `agent/lib/src/agent/wire_protocol.cpp`、`agent/lib/include/agentxx/protocol/*.h`（mcp/acp/a2a/provider） |
| 插件 | `agent/lib/include/agentxx/plugin/api/plugin_api.h`、`agent/lib/src/plugins/*`、`docs/zh-cn/design/plugins.md` |
| 客户端 | `agent/client/src/io/tui/agent_tui.cpp`、`agent/client/include/agentxx-client/io/tui/ui_components.h`、`docs/zh-cn/design/tui.md` |
| 测试与基准 | `agent/test/test.cpp`、`agent/test/include/agentxx-test/**`、`agent/benchmark`、`docs/zh-cn/design/benchmark.md` |
| 设计文档 | `docs/zh-cn/design/index.md`、`plugins.md`、`tui.md`、`ffi.md`、`benchmark.md` |

### A.2 codex

| 模块 | 路径 |
|---|---|
| 会话与循环 | `codex-rs/core/src/session/{mod,session,turn,handlers,input_queue,step_context,turn_context,startup}.rs` |
| 轮次任务 | `codex-rs/core/src/tasks/{mod,regular,compact,review,user_shell}.rs` |
| 协议类型 | `codex-rs/protocol/src/{protocol,models,items,permissions,turn_input}.rs` |
| 工具系统 | `codex-rs/core/src/tools/{registry,router,orchestrator,parallel,sandboxing,handlers/*}.rs` |
| 上下文与提示词 | `codex-rs/core/src/context/**`、`codex-rs/context-fragments/src/**`、`codex-rs/prompts/src/**`、`codex-rs/core/src/context_manager/**` |
| 压缩 | `codex-rs/core/src/compact*.rs`、`codex-rs/core/src/session/context_window.rs` |
| 权限与沙箱 | `codex-rs/execpolicy/src/**`、`codex-rs/sandboxing/src/**`、`codex-rs/network-proxy/**`、`codex-rs/linux-sandbox`、`codex-rs/windows-sandbox-rs`、`codex-rs/mxc-sandbox` |
| 持久化 | `codex-rs/rollout/src/{recorder,writer_lock,state_db,search,compression}.rs`、`codex-rs/thread-store/src/**`、`codex-rs/state/src/**` |
| 多代理 | `codex-rs/core/src/{thread_manager,agent/**}.rs`、`codex-rs/agent-graph-store/**`、`codex-rs/agent-message-board-client/**` |
| 客户端 UI | `codex-rs/tui/src/**`（`app.rs`、`chatwidget.rs`、`bottom_pane/**`、`transcript_view.rs`、`wrapping.rs`） |
| 服务与协议 | `codex-rs/app-server/src/**`、`codex-rs/app-server-protocol/src/**`、`codex-rs/app-server-transport/**`、`codex-rs/app-server-daemon/**` |
| SDK 与外部集成 | `sdk/typescript/**`、`sdk/python/**`、`codex-rs/codex-mcp/src/**`、`codex-rs/code-mode*/**`、`codex-rs/exec-server/**` |
| 扩展面 | `codex-rs/ext/**`、`codex-rs/hooks/src/**`、`codex-rs/skills/src/**`、`codex-rs/core-plugins/src/**`、`codex-rs/features/src/**` |
| 测试与规范 | `codex-rs/core/tests/suite/**`、`codex-rs/tui/src/snapshots/**`、`AGENTS.md`、`justfile` |

## 附录 B：术语对照

| 概念 | agentxx | codex |
|---|---|---|
| 会话 | `Session`（会话即上下文权威） | `Thread` / `Session`（会话 + 线程概念并存，`ThreadId` 对外） |
| 一轮 | `runTurnAsync` 的一次调用 | `Turn`（由 `SessionTask` 驱动，有 turn id） |
| 步骤 | 图节点执行（llm/tools 节点） | `Step`（每次采样请求一个 step，`StepContext` 快照） |
| 历史与上下文 | 会话内的 typed 消息 + 展示历史 | `ContextManager` 历史 + rollout 日志 |
| 注入内容 | 附加系统提示段 / 追加消息 | `ContextualUserFragment` 片段 + world state section |
| 工具调用 | `tool_calls` + `ToolcallWrapNode` | `FunctionCall`/`CustomToolCall` + `ToolCallRuntime` |
| 审批 | 权限中间件 + 声明式中断表单 | approval policy + `ReviewDecision` + 审批缓存 |
| 沙箱 | 无系统沙箱（路径规则 + worktree 隔离） | SandboxManager（Seatbelt/Landlock/bwrap/Windows） |
| 压缩 | 摘要中间件（含子代理摘要） | compaction（本地/远端 + 换窗） |
| 子代理 | 独立 agent（`AgentHost` 派生） | 子线程（`ThreadManager` + `AgentControl`） |
| 插件 | C ABI 动态库 + 接口表 | 扩展 crate / hooks / skills / MCP / 市场包 |
| 客户端 | TUI/CLI（可远程 WS） | TUI/exec（app-server 客户端） |

## 附录 C：本次精读的源码清单与结论依据

**agentxx**

| 文件 | 证明/确认的结论 |
|---|---|
| `agent/lib/include/agentxx/agent/base_agent.h`、`lib/src/agent/base_agent.cpp` | ReAct 循环入口、`runTurnAsync` 的会话绑定与事件桥、附件加载卸载线程池、节流落盘与轮末权威保存 |
| `agent/lib/src/agent/agent_runner.cpp` | 中断-恢复循环、HIL 与子代理两条 resume 路径、未完成中断的判定 |
| `agent/lib/src/nodes/toolcall.cpp` | 参数严格解析、顺序执行（`TODO: 真正并行`）、取消占位、输出摘要 |
| `agent/lib/include/agentxx/tools/tool.h` | `autoSummaryOutput`/`canDelayLoad`/`maxRetry`/`repeatCallCheck` 四个开关与图桥接 |
| `agent/lib/include/agentxx/middlewares/middleware.h` | 7 个钩子点、按会话分片的状态、中间件自带工具收集 |
| `agent/lib/include/agentxx/middlewares/subagent_manager.h`、`tools/subagent.h` | 子代理独立 agent、总线服务、`enableSubagent` 语义 |
| `agent/lib/src/middlewares/summarization.cpp` | 压缩参数、噪音折叠、多模态降级、相邻去重 |
| `agent/lib/src/agent/session_store.cpp` | 四张表结构、`msg_id` 索引动机、目录名清洗、meta 键 |
| `agent/lib/include/agentxx/agent/context.h` | 上下文写入口、`messagesVersion`、惰性 JSON 缓存 |
| `agent/lib/include/agentxx/agent/prompt.h`、`lib/src/agent/prompt.cpp` | 提示词三块结构、占位符替换、按键附加段 |
| `agent/lib/src/agent/wire_protocol.cpp` | 消息类型与往返编解码、队列与中断相关消息 |
| `agent/client/.../tui/agent_tui.*`、`ui_components.h` | 运行/等待中断状态、组件描述与表单常量 |
| `agent/test/test.cpp` 与 `agent/test/include/agentxx-test/**` | 测试模块划分（约 80 个）与分组方式 |

**第 2 版新增精读（整段读完的实现文件）**

| 文件 | 证明/确认的结论 |
|---|---|
| `lib/src/nodes/toolcall.cpp`（500–1184） | `execTool` 的完整控制流（参数类型自动修正、总线权限、重复调用确认卡片与 resultId 下钻、`maxRetry` 循环、share store 溢出改存两种文案）、`baseRun` 的中断缓存复用/动态插件工具查找/顺序 await/取消补占位/中断存缓存并抛中断、START 与 BASERUN 错误补插结果 |
| `lib/src/nodes/modelcall.cpp`（640–900） | 每轮重建 system 消息且不节流落盘、`repairMessages` 的修正范围与“纠错不产生 UI 增量”、重试退避公式与部分输出保留（≥512 字符）、失败前保证末条为无 tool_calls 的 assistant |
| `lib/src/middlewares/permission.cpp`（230–500） | `decideTarget` 六级判定顺序、worktree 写拒绝的先后原因、配置拒绝优先于完全授权、`requestPermission` 的默认拒绝/不限时/规则必须注册在中间件、符号链接缺口 |
| `lib/src/middlewares/summarization.cpp`（450–720） | 摘要提示词来自 `appendSystemPrompts["summarization"]`、载荷裁剪到 95% 并把丢弃条数写进提示词、同会话同模型子代理摘要（tools 为空、禁二次压缩）、手动压缩直派宿主的原因与 2 分钟超时、错误串透传防护、`hardTruncate` 的完整兜底算法 |
| `lib/src/agent/context.cpp` 与 `context.h` | `kPersistThrottleMs = 3000`、首次立即落盘、上下文整份写与展示历史 `PendingViewOp` 排队写两条路径、`SessionStoreHooks` 解耦“何时写/怎么写” |

**第 3 版新增精读**

| 文件 | 证明/确认的结论 |
|---|---|
| `lib/include/agentxx/agent/io/wire_protocol.h`（1161 行，头部 120 行） | 消息常量表（客户端→服务端 15 条 / 服务端→客户端 22 条）且**逐条带注释**、`CloseReason` 三态（用户取消/客户端掉线/超时）、`WireDelta` 11 种类型区分渲染指令与状态变更；测试目录未搜到 wire 往返用例（缺口是机器校验，不是文档） |
| `lib/src/plugins/tool_registry.cpp`（全文） | 注册冲突规则（表内同名、内置静态名冲突均拒绝并告警）、`unregisterTool` 返回 `shared_ptr` 供与 inflight 计数配合、`appendDefinitions` 追加插件工具定义 |
| `lib/src/plugins/plugin_manager_lifecycle.cpp`（函数清单，445 行） | 生命周期骨架在内核、宿主只实现接缝：`createInstance`/`detachDomainRegistrations`/`detachDomainOwnedResources`/`clearDomainRegistrations`/`releaseInstanceResources`/`onInstanceEnabledChanged`/`flushPendingCleanup`/`eraseMiddleware` 等 |
| `lib/src/tools/subagent.cpp`（头部 110 行） | 批量委派参数（`messages` 结构化透传、`sessionId` 同上下文、`tools` 策略、`enable_summarization`、`result_id`）、单发与批量两种形态兼容、批量即并行 |
| `client/src/io/tui/ui_components.cpp`（2210 行，函数清单） | 布局原语（`colsOf`/`rowsOf`/`gapCols`/`insetsOf`/`textOf`）统一解析、按组件类型分派渲染、控件命中区域注册、表单状态从 `UiFormState` 取值（控件不持状态）、测量与渲染共用同一实现 |
| `agent/test/include/agentxx-test/test_framework.h`（头部 80 行） | `executableDir()` 的跨平台实现（`/proc/self/exe` / `_NSGetExecutablePath` / `GetModuleFileNameW`）与 `TestResult` 累加——插件测试能在任意 cwd 稳定运行的前提 |

**codex**

| 文件 | 证明/确认的结论 |
|---|---|
| `AGENTS.md`（仓库根） | crate 纪律、模型可见上下文规则（禁止历史重写、注入项必须有上限）、测试与 API 变更规范、TUI 约定 |
| `codex-rs/core/src/session/mod.rs` | `SessionSpawnArgs` 装配清单、提交循环与 `SessionIo`、审批/用户输入/提权的 pending 表、事件发送顺序（先落盘后通知） |
| `codex-rs/core/src/session/turn.rs` | `run_turn` 主循环、step 捕获、压缩时机、stop hooks、中断留痕 |
| `codex-rs/core/src/tasks/mod.rs` | `SessionTask` 契约、任务启动/取消/收尾、优雅中断 100ms、用量指标 |
| `codex-rs/protocol/src/turn_input.rs` | 四种投递模式、三种结果与 9 种拒绝原因 |
| `codex-rs/protocol/src/protocol.rs` | `Op`/`EventMsg`/`AskForApproval`/`SandboxPolicy` 等协议规模（`EventMsg` 约 173 个变体） |
| `codex-rs/protocol/src/permissions.rs` | 权限档案、冲突优先级（deny > write > read）、受保护元数据路径 |
| `codex-rs/protocol/src/models.rs`、`core/src/client_common.rs` | `Prompt` 结构、`ResponseStream` 的中断与消费端退出取消、图片细节归一化 |
| `codex-rs/core/src/tools/{registry,router,orchestrator,parallel,sandboxing}.rs` | 三层工具结构、编排器流程、并行闸门、审批缓存与多 key 批准 |
| `codex-rs/core/src/context/**`、`context-fragments/src/fragment.rs`、`context_manager/updates.rs` | 片段契约（role/content_kind/markers）、消息合并规则 |
| `codex-rs/core/src/context/world_state/mod.rs` | 世界状态 section 的快照/差分/回扫匹配接口 |
| `codex-rs/core/src/compact.rs` | 压缩时机与初始上下文注入语义、`CompactedItem` 元数据 |
| `codex-rs/rollout/src/{lib,writer_lock}.rs`、`thread-store/src/store.rs` | rollout 行格式、写者锁、`PersistContext` 耐久语义 |
| `codex-rs/sandboxing/src/lib.rs`、`execpolicy/src/policy.rs` | 平台沙箱矩阵与错误映射、命令策略（按程序规则 + 网络规则） |
| `codex-rs/core/src/agent/{types,control}.rs` | 代理身份/派生选项、控制面 20 个子模块划分 |
| `codex-rs/app-server/src/lib.rs`、`app-server-protocol/src/protocol/common.rs`、`tui/src/lib.rs` | app-server 多传输、协议规范与 v1/v2、TUI 作为 app-server 客户端 |
| `codex-rs/features/src/lib.rs`、`justfile`、`codex-rs/core/config.schema.json` | 特性阶段、开发入口命令、生成式配置 schema |
| 目录与统计（`core/tests/suite` 201 文件、`tui/src` 1324 快照、`codex-rs` 4907 个 `.rs`/1.81M 行） | 测试与代码规模结论 |

**第 2 版新增精读（整段读完的实现文件）**

| 文件 | 证明/确认的结论 |
|---|---|
| `core/src/session/input_queue.rs`（全文） | 两层队列结构、`MailboxDeliveryPhase` 状态机、`watch_user_input` 与请求抢占、`get_pending_input` 只在 `CurrentTurn` 相位排空邮箱 |
| `core/src/state/turn.rs`（全文） | `TurnState` 五张挂起等待表、按环境的已授予权限合并、`RunningTask` 七项持有物 |
| `core/src/session/handlers.rs`（全文） | Op 处理细节（`exec_approval` 的修订落盘与中断、`compact` 先替换再起任务、`run_user_shell_command` 的双路径）、`shutdown_session_runtime` 的关闭顺序 |
| `core/src/session/turn.rs`（`run_auto_compact`/`build_prompt`/`run_sampling_request`/`try_run_sampling_request`） | `parallel_tool_calls` 恒为 true、抢占返回 `needs_follow_up`、`FuturesOrdered` 按序收集、邮箱可抢占的相位条件 |
| `core/src/compact.rs`（110–460） | 压缩即一次采样轮、PreCompact/PostCompact 钩子可中止、撞上限时从最旧删一条并归零重试、摘要前缀与用户消息保留、窗口推进与元数据落盘 |
| `core/src/stream_events_utils.rs`（头部） | 响应项落库、记忆引用解析、隐藏标记剥离（引用/计划块） |
| `core/src/tools/registry.rs`（376–520） | 注册表治理（策略过滤、保留名、冲突记录、顺序保持）、`ToolExposure::deferred` 命名空间描述、MCP 命名空间 |
| `core/src/tools/router.rs`（230–430） | `build_tool_call` 的 payload 映射、`tool_supports_parallel` 默认 false、派发只组装 `ToolInvocation` |
| `core/src/tools/parallel.rs`（130–420） | 派发任务的 `select!` 取消竞争、`AbortedToolOutput` 文案、终态优先取结果、分段计时守卫仅对直连调用生效 |
| `app-server/src/message_processor.rs`（头部） | 约 24 个 RequestProcessor、请求序列化队列、连接 RPC 闸门与 30s 排空、轮次准入、后台工作线程 |
| `thread-store/src/local/live_writer.rs` | 打开线程三道关（进程内互斥 / recorder 空缺 / 跨进程锁）、恢复时历史模式与 cwd 的解析与硬前提 |

**第 3 版新增精读**

| 文件 | 证明/确认的结论 |
|---|---|
| `core/src/client.rs`（180–480、函数清单） | `ModelClient` 会话级 / `ModelClientSession` 轮次级职责边界、WebSocket 增量复用判定（穷尽解构 + 内部元数据豁免）、`x-codex-turn-state` 粘性令牌“同轮回传、绝不跨轮”、HTTP 回退是会话级、`WebsocketSession::reset` 保留认证代次与丢失原因 |
| `core/src/codex_thread.rs`（300–560） | 五种投递/恢复 API 的语义与拒绝条件、`suspend_turn_and_shutdown` 的“存在加载中后代则拒绝 + 挂起成功前不得转移所有权”、`inject_if_running` 无活动轮次时原样返回、`active_turn_environment_selections` 支撑跨执行器 steering 授权、进入新轮前先过配额准入 |
| `core/src/thread_manager.rs`（函数清单） | 线程管理器的能力面（start/spawn/fork/reserve/resume/subtree 查询/元数据/分区）、可注入的线程 id 生成器与 agent 控制工厂、`subscribe_thread_created` 广播、后台 rollout 迁移入口 |
| `core/src/context_manager/history.rs`（头部 120 行） | `ContextManager` 字段与不变式：`Arc` 共享快照、`retained_context` 与模型窗口解耦、`history_version`/`reset_version`/`user_message_revision` 三个版本号语义、`reference_context_item` 为 None 时全量重注入 |
| `core/src/unified_exec/process_manager.rs`（头部 120 行） | 持久 shell 的环境加固清单（NO_COLOR/TERM=dumb/LC_*/PAGER=cat/CODEX_CI）、注入的 `CODEX_THREAD_ID`/`CODEX_VERSION`/`CODEX_PERMISSION_PROFILE`、进程数与 yield 时间限额、stdin 审批 8000 字节、Ctrl-C 中断、头尾缓冲、退出/流式输出独立 watcher、晚到网络拒绝 100ms 宽限 |
| `core/src/context_manager/*` 目录、`rollout/src/*` 行数 | 历史规范化（normalize）、用户授权消息单独成模块；rollout 压缩 1315 行、搜索 341 行、索引 278 行、状态库 711 行 |
| `rollout/src/search.rs`（头部 120 行） | 搜索优先调用 `rg`（固定串、忽略大小写、忽略 .gitignore、仅 *.jsonl）并回落到内置扫描、额外扫描压缩 rollout、搜索词 JSON 转义、命中上下文窗口固定（前 48 / 后 96 字符） |
| `execpolicy/src/parser.rs`（头部 100 行） | 命令策略是 **Starlark 脚本**（Extended 方言 + 自定义 builtin）、规则可声明正/负样例并在**加载期**校验、程序名按“名字/路径”两套键归一化并记录宿主可执行路径、错误带源码位置、`amend.rs` 负责把批准写回策略 |
| `hooks/src/engine/*` | hooks 引擎六个模块分工（discovery/schema_loader/command_runner/mcp_runner/output_parser/dispatcher）、handler 两类来源与执行模式、事件是否参与匹配器的规则、同一 handler 只执行一次、并行执行但记录完成顺序、运行摘要供 UI |
| `core/tests/common/responses.rs`（头部 90 行） | 请求捕获器与断言点（`single_request` 数量断言、`saw_function_call`、`function_call_output_text`）、可断言 `parent_turn_id`/`root_turn_id`、WebSocket 测试用真实握手服务（含 deflate 配置） |

## 附录 D：可以继续深入的清单

> 第 3 版已读：`client.rs`、`codex_thread.rs`、`thread_manager.rs`、`context_manager/history.rs`、`unified_exec/process_manager.rs`、`rollout/search.rs`、`execpolicy/parser.rs`、`hooks/engine/*`、agentxx 的 `wire_protocol.h`/`tool_registry.cpp`/`subagent.cpp`/`ui_components.cpp`/`plugin_manager_lifecycle.cpp`/`test_framework.h`。下表是**尚未读**的部分。

### D.1 优先级最高（会改变对某一侧的评价）

1. `codex-rs/app-server/src/transport.rs` 与 `outgoing_message.rs`：背压、连接清理、并发请求处理与出站队列顺序保证（§1、§13）。
2. `codex-rs/core/src/tools/runtimes/**`（含 `unified_exec` runtime）：工具运行时如何与编排器拼接、attempt 参数如何构造（§5、§9）。
3. `codex-rs/ext/guardian-reviewer/**` 与 `core/src/guardian_review.rs`：自动审查的判定逻辑、授权刷新与预算（§9）。
4. `codex-rs/codex-mcp/src/connection_manager.rs` 与 `tool_catalog_cache.rs`：MCP 工具的热更新、命名冲突与授权缓存（§5、§13）。

### D.2 优先级中等（补全模块结论）

1. `codex-rs/core/src/context_manager/{normalize,history_user_authorization}.rs`：历史规范化的具体规则与用户授权消息的判定（§3）。
2. `codex-rs/rollout/src/{compression,state_db,session_index}.rs` 全量：压缩阈值、索引结构与查询路径（§4、§11）。
3. `codex-rs/skills/src/{loading,parser}.rs` 与 `memories/write/src/{phase1,phase2}.rs`：技能加载与记忆写入流水线（§11、§16）。
4. `codex-rs/core/src/tools/handlers/{apply_patch,unified_exec,multi_agents_v2}.rs`：各内置工具的参数契约与副作用边界（§5）。
5. `agent/lib/src/middlewares/{skill,memory_file}.cpp`：技能与记忆在 agentxx 侧的具体注入方式（§11）。
6. `agent/lib/src/plugins/plugin_manager_vtable.cpp` 与 `plugin_manager_domain_hooks.cpp`：领域表与通用表的边界、事件后端包装（§16）。

### D.3 优先级较低（工程细节，可随任务顺手读）

1. `codex-rs/core/src/tools/code_mode/**` 与 `code-mode-runtime`：Code Mode 的协议与安全边界（§13）。
2. `codex-rs/tui/src/{app.rs,chatwidget.rs,bottom_pane/*}`：TUI 事件循环与组件组织（§12）。
3. `codex-rs/hooks/src/engine/{command_runner,mcp_runner,output_parser}.rs`：hook 执行与输出解析细节（§16）。
4. `agent/client/src/io/tui/agent_tui.cpp` 的输入与中断处理路径、`agent/client/src/main.cpp` 的启动模式（§12、§1）。

### D.4 建议的接续方式

- 任何模块若要落到实现，先在本文对应小节补齐“现状（含文件行号）→ 方案 → 代价”三小段，再开工；完成后回填到 `resource/history/` 的实现记录，并在本文对应位置标注已落地。
- 结论如与源码冲突，一律以源码为准，并在本文的“编写说明”处追加修正记录（参考 `compare-dsh.md` 第 2 版把“凭文档推断的错误”逐条更正的做法）。
