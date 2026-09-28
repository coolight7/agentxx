# agentxx 架构改进计划（六篇对比文档整理）

> **本文是什么**：把 `compare-codex.md` / `compare-dsh.md` / `compare-harness.md` / `compare-openclaw.md` / `compare-opencode.md` / `compare-pi.md` 六篇对比文档里"可迁移到 agentxx 的设计"合并、去重、排序后形成的实施计划。
>
> **阅读方式**：先看 [§0 整理方法与优先级口径](#0-整理方法与优先级口径) 与 [§17 整体整理](#17-整体整理)（那里有统一的 P0/P1/P2 清单、落地批次、不做清单与已实现核对表），再按模块查细节。
>
> **每项的记法**：`编号 | 设计 | 来源 | 优先级 | 落地位置 | 说明`。编号前缀是模块码（如 `TOOL-3`），`来源` 用六个项目的名字，多篇都提到的条目会全部列出（说明这是跨项目共识，可信度更高）。
>
> **编写约定**：
> - 只在**能在 agentxx 现有架构上落地**的范围内写（单 `io_context` 协程 + 图引擎 + C ABI 插件 + 声明式 UI 描述层 + 本地优先可嵌入），不能落地的一律进 [§17.5 不做清单](#175-不做清单六篇反向结论汇总)。
> - 每条都标了落地位置（哪个文件/哪一层），便于直接当实施清单用。
> - 若六篇里某条建议在 agentxx **已经实现**，不重复作为迁移项，收进 [§17.6 已实现核对表](#176-已实现核对表避免重复投入)（这一节同样重要：六篇里有 7 处"读文档会低估现状"的修正）。

---

## 0. 整理方法与优先级口径

### 0.1 六篇对比的对象与侧重

| 文档 | 对比对象 | 语言/形态 | 该篇最有价值的几块 |
|---|---|---|---|
| compare-codex | codex（`codex-rs` 工作区） | Rust + tokio；客户端/app-server/core 三段 | 轮次投递四态与请求抢占、rollout 权威日志 + 写者锁、工具编排器（审批/沙箱/重试）、hooks、执行环境加固 |
| compare-dsh | deepseek-harness | TypeScript + Cordis 插件树 | 事件溯源会话、waterfall + guard 的扩展链、压缩是带锁的持久操作、后台作业与「可继续子代理」 |
| compare-harness | Harness Open Source | Go 多租户服务端 | 作业表 + 状态机 + 超期回收、迁移框架、出网策略（netpolicy）、权限决策理由与审计、CI 门禁 |
| compare-openclaw | openclaw | TypeScript 多通道网关 | 车道级并发预算、上下文引擎槽位、写者租约 + 事务内复验、工具结果聚合预算 + 缓存过期剪枝、可执行边界规则 |
| compare-opencode | opencode（V2 核心） | TypeScript + Effect | durable 收件箱（admitted/promoted）、Context Epoch（基线 + 增量）、工具物化/结算（公告身份、并行、落盘保真）、声明式 transform + 重算 |
| compare-pi | pi | TypeScript 多包 | 操作状态机 + drive 原语、entry 树 + 分支 + 车道、绑定地址 + 原子事务、用量账本、一致性测试框架、假 provider |

六篇的**共同结论**（这一层比任何单条建议都重要）：

1. **agentxx 的方向没错**：会话即上下文唯一权威、图引擎承载 ReAct、中间件承载横切关注点、C ABI 插件承载能力扩展、声明式组件承载 UI —— 六篇都给了肯定，并多次指出这些是对方没有或更弱的。
2. **agentxx 缺的主要不是功能，而是"把已有约定变成显式对象"**：轮次语义、投递结果、注入来源、历史替换、失败尝试、写者所有权、审批记忆、未实现清单 —— 这些在六篇里反复以"应该显式化"的形式出现。
3. **成本（token / 前缀缓存）在 agentxx 还没有被当成一等设计**：不是缺某个功能，而是缺一条纪律（前缀稳定 + 只在尾部增长 + 缓存断点 + 溢出恢复）。六篇里有四篇把它列为 P0。
4. **安全上"策略层很强、边界层缺失且责任没写清"**：权限中间件比 pi/opencode 都强，但（a）有副作用的大工具没声明权限，（b）没有出网策略，（c）没有把"权限 ≠ 沙箱"写出来。
5. **可恢复性链条不完整**：输入排队、事件序列、写者所有权、持久化格式版本、未完成轮次的收尾 —— 五处都能补齐，且都不动架构形状。

### 0.2 优先级口径

| 级别 | 判据 | 典型特征 |
|---|---|---|
| **P0** | 现在就有明确损失（错误/卡死/上下文膨胀/重复执行/安全缺口），且改动边界清晰 | 不动架构形状、不新增运行时、能配一条可验证的测试 |
| **P1** | 收益明确，需要一点设计或局部改造 | 可能新增一张表、一条 wire 消息、一个配置段 |
| **P2** | 依赖前置项，或属方向性投入/长期演进 | 需要先做设计验证，或与定位存在张力（需评估） |

### 0.3 去重与合并规则

- 同一设计在多篇出现 → 合成一项，`来源` 列全部列出；若不同篇的深度不同，取最深的那篇的表述。
- 同一模块内互为前置的项（例如「上下文来源化」与「基线/增量分离」）在说明里写清依赖，不重复计入工作量。
- 六篇各自编号（如 openclaw 的 M26、harness 的 P0-1）**不再保留**，本文用统一编号；对应关系见各模块的 `来源` 列与[附录 A](#附录-a来源对照)。
- 与 agentxx 定位冲突的项（多通道接入、原生壳、容器沙箱、事件溯源整库、双轨运行时等）统一进 [§17.5](#175-不做清单六篇反向结论汇总)，不占 P0/P1/P2 名额。

### 0.4 三条主线

把 130 余项按"解决什么问题"归拢，实际只有三条主线：

| 主线 | 一句话 | 覆盖模块 | 代表条目 |
|---|---|---|---|
| **一、可恢复性与可解释性** | 把"运行时状态"写进数据：输入、轮次、事件、写者、未完成工作 | §2 §5 §10 §12 §15 | LOOP-1、STO-1、STO-3、SUB-1、SEC-3 |
| **二、成本与上下文** | 让长会话的成本可控：前缀稳定、增量注入、预算单一口径、溢出可恢复 | §3 §4 §7 §8 §6 | PRM-1、CTX-3、LLM-2、CMP-2、TOOL-6 |
| **三、边界与信任** | 把边界写清、把危险动作留痕：权限全覆盖、出网策略、审计、规则可校验 | §9 §13 §14 §16 | SEC-1、SEC-6、PLG-5、TST-1 |

---

## 1. 总体架构、服务分层与装配

**现状要点**：三层（`lib` / `client` / `plugins`）+ 端点抽象（`AgentIOBase` + 传输）+ C ABI 插件；装配分散在 `BaseAgent::init()` 的 `initRegisterNodes` → `initMiddleware` → `initTools` → `initModelRegistry` → `initEventBus` → `initGraphDefinition`，顺序靠注释约定；依赖方向只写在文档里，没有可执行检查；服务能力与 UI 能力在同一个可执行文件里。

### 1.1 建议吸收的设计

| 编号 | 设计 | 来源 | 优先级 | 落地位置 | 说明 |
|---|---|---|---|---|---|
| ARC-1 | **把依赖方向写成可执行检查** | opencode、harness、dsh | P1 | 新增 `agent/script/check_boundaries.{sh,py}` 或 `agentxx_test` 的 `meta` 模块 | 三条最基本断言：① `agent/client/**` 不包含 `agent/lib/src/**` 私有头；② `agent/plugins/**` 只包含 `agentxx/plugin/api/**`（SDK umbrella）与 `cxx_utilxx*` 公开头；③ 插件动态库导出符号仍只有入口白名单（已有 version script，补一条自动断言）。现在这三条只有文字约定，最容易被无意破坏 |
| ARC-2 | **按目录放局部规则文件**（对应 scoped `AGENTS.md`） | openclaw | P1 | 新增 `agent/lib/AGENTS.md`、`agent/lib/include/agentxx/plugin/AGENTS.md`、`agent/client/AGENTS.md`、`agent/plugins/AGENTS.md`、`agent/test/AGENTS.md` | 每个目录只写该目录的硬约束（插件源码只能用哪个 umbrella、测试头必须走 `agentxx-test/...` 完整路径、客户端不得引用 lib 私有头…）。根 `AGENTS.md` 保留跨目录原则并注明"规则更新到 owner 目录"。openclaw 实测 30 份，是本项目最缺的一层 |
| ARC-3 | **装配清单化 + 启动期依赖断言** | harness | P1 | `agent/lib/src/agent/base_agent.cpp` 的 `init()` | 把 `init*` 的调用序列变成一份显式阶段清单（`{name, fn, rollbackFn}`），并在末尾统一断言关键依赖（模型可用、dataDir 可写、插件目录可读、sqlite 可开、必要节点已注册）。任一失败给出明确错误并以非零码退出，而不是"少个工具静默继续"。收益：把"运行期才发现"变成"启动即失败" |
| ARC-4 | **分阶段关闭 + 后台任务收敛** | harness、codex、dsh | P1 | `BaseAgent::shutdownAsync` + 新增 `agentxx::util::TaskScope` | 现在 `shutdownAsync` 只有"停插件 + 等待"。建议按「停止接收新输入 → 等在跑轮次/作业 → 停定时器 → 停插件 → 刷盘」分阶段，各阶段带超时（总超时后可强制）。`TaskScope` 语义对齐 `errgroup`：注册后台任务（插件定时器、子代理、作业），任一"致命失败"即取消其余并按依赖倒序关闭 |
| ARC-5 | **生效装配快照可查询** | harness、openclaw、dsh、opencode | P1 | `WireGetContext` 同族新增 `get_diagnostics` + TUI 诊断弹窗 | 输出：模型列表与来源层、插件清单与状态、中间件顺序、工具名清单（含延迟加载/不可用原因）、图定义、生效配置（含回填后的路径）、打开的会话库数、事件队列积压、在跑作业。这是排障"插件改了图/加了工具"的最低成本手段，也是六篇里出现次数最多的运维诉求 |
| ARC-6 | **控制面 / 运行面分离的显式约定** | openclaw | P2 | `docs/zh-cn/design/plugins.md` + 头文件注释 | 规定「发现、清单解析、配置校验、装配提示属控制面；插件执行属运行面」，且**读元数据的路径不得初始化插件实例**。agentxx 的装载已经是 `dlopen` + 符号查找（天然惰性），缺的是把这条写成规则，防止将来加"启动期预热"破坏它 |
| ARC-7 | **窄接口绑定（消费者视角）** | harness | P2 | `agent/lib/include/agentxx/agent/context.h` 拆访问视图 | 现在中间件/插件拿的是一揽子 `AgentContext`。可参照 wire 的 `Bind`，按消费者定义窄接口（`SessionReader`、`PermissionCheck`、`ToolRegistrar`），宿主用适配器满足。收益：测试可只造窄桩；代价是引入一层适配代码，故列 P2 |
| ARC-8 | **"新能力不进核心骨架"的书面纪律** | codex、pi、openclaw | P0 | `docs/zh-cn/design/index.md` | 明确边界：`lib/src/agent` 只放会话生命周期 / 上下文 / 持久化骨架；新增能力优先落 `middlewares/`、`nodes/`、`plugins/` 或新目录。成本近零，收益是防止核心继续变胖（六篇里有三篇把这条列为最高优先级） |

### 1.2 本项目已有优势（保留）

- 端点抽象比"把 TUI 当客户端"更彻底：同一份 agent 二进制既可从 stdio 也可从 WS 驱动，且 `runTurnAsync(io=nullptr)` 是唯一的 headless 例外（六篇都确认这一层更干净）。
- 插件是**真二进制边界**：跨语言、导出白名单、多实例三铁律、表级版本自校验 —— 六篇一致认为这是 agentxx 最强的部分。
- 编译产物三形态（可执行 / 动态库 / 静态库）+ 工具库静态并入，适合嵌入与分发。
- 内置插件与动态插件同构（`PluginxxBuiltinInfo`），热插拔与内置共用一套生命周期。

---

## 2. Agent 循环、轮次与投递语义

**现状要点**：一轮 = `runTurnAsync` → `run_stream_async` → `while(interrupted)` 的中断恢复循环；运行中的用户输入进 `SessionServerAgentIO::messageQueue_`（内存、轮末消费），有 `queuePaused_` / `interruptAndRunNext` / 清空 / 删单项；取消是 `CancelToken` + 埋点；没有轮次 id、没有投递结果、没有 steering、没有步进快照。

### 2.1 建议吸收的设计

| 编号 | 设计 | 来源 | 优先级 | 落地位置 | 说明 |
|---|---|---|---|---|---|
| LOOP-1 | **输入投递语义显式化：`next-step`（steer）/ `next-turn`（queue）/ `collect`** | codex、dsh、openclaw、opencode、pi | **P0** | `wire_protocol.h`（`WireUserInput` 加 `delivery`）、`session_server_agent_io.cpp`（队列策略）、`nodes/modelcall.cpp`（下一个安全边界注入点） | 现在只有"排队（轮末消费）"和"打断并跑队首"两种手势。补 `next-step`：在当前轮的**下一次 modelcall 之前**作为 user 消息注入（等价 steering）；`collect`：静默窗口（默认 500ms）合并多条为一次。这是六篇里出现次数最多的一项，也是体验提升最直接的一项 |
| LOOP-2 | **投递结果显式返回** | codex、pi、opencode | P1 | `wire_protocol.h` + `pushMessageQueueItem` 的响应路径 | `user_input` 的回执改成 `started{target}` / `queued{position}` / `steered` / `rejected{reason}`（原因至少区分：会话不存在、正在压缩、等待用户输入中、被取消）。客户端才能给出准确反馈，而不是"发出去了不知道去哪了" |
| LOOP-3 | **排队状态的显式状态机** | openclaw | P1 | `session_server_agent_io.cpp`：把 `queuePaused_` / `pendingInsert_` 合成 `QueueState{idle,running,paused,draining}` | 现在两个 bool 组合出四种状态却没有集中校验（"空闲收到 Cancel 不应暂停"这条是靠补丁修的）。改成枚举后规则变成状态转移表的一行，可做穷举测试 |
| LOOP-4 | **排队项持久化 + 重启后的显式交代** | openclaw、opencode、pi | P1 | `SessionStore` 新表（或会话 meta 段）+ `SessionServerAgentIO` 启动恢复 | 现在队列在内存，进程重启即丢。最小落地：排队项落库（文本 + 附件 + 模型选择），重启后恢复为**非自动执行**的"待重发"列表，由客户端提示用户确认。不要做成"自动重放"（会与上下文状态不一致） |
| LOOP-5 | **采样请求抢占（插话在本次采样即生效）** | codex | P1 | `nodes/modelcall.cpp` 的流等待点 + `Session` 的输入投递路径 | 只做"轮末消费"无法让插话立刻生效。做法：会话收到 `next-step` 输入时取消一个抢占令牌，节点中断当前 HTTP 流、保留已收到内容并按同一请求续跑。与 LOOP-1 配套。代价：需要处理"部分输出 + 重发"的一致性与限流计数 |
| LOOP-6 | **轮次记录（turn record）持久化** | pi、opencode、harness | P1 | `SessionStore` 新表 `turn` + `runTurnAsync` 收尾 | 每轮写一条不可变记录：`turnId / 来源消息 id / 起止消息 id / 状态(completed·aborted·failed·max_steps) / 错误码 / 耗时 / 模型 / 用量`。收益：客户端与插件可以"问状态"而不是"数事件"；为将来的 fork / 重放 / 崩溃配平提供锚点。现状的 `turnStartMs/durationMs/tps` 可先归并进来 |
| LOOP-7 | **中断留痕结构化** | codex、dsh | P1 | `nodes/toolcall.cpp` 的取消分支 + `modelcall.cpp` | `[User canceled]` 占位已经在做，建议升级为带原因与阶段的结构化片段（用户取消 / 超时 / 被新输入抢占 / 预算耗尽），并保证"未分发的 tool_call 一定有对应结果"。现在靠"取消分支 + `repairMessages`"两处兜底，缺一条一致性断言（见 TOOL-11） |
| LOOP-8 | **优雅中断超时** | codex、harness | P1 | `agent_runner.cpp` 循环 + `nodes/toolcall.cpp` 的取消点 | 现在取消后由节点自行响应，个别工具卡住会拖住整轮。加一条"取消后 N ms 未收敛则强制结束本轮并记录日志"（N 可配，默认参照 codex 的 100ms 优雅期但放宽到秒级）。与 SUB-4 的作业超时共用一套实现 |
| LOOP-9 | **"已展示最终答案后不再延长本轮"规则** | codex | P2 | 队列状态 + `wire_protocol.h` 语义说明 | 对应 `MailboxDeliveryPhase`：一旦本轮已产出对用户可见的最终文本，晚到的输入/子代理邮件不再并入本轮。避免"用户已经看到答案又被卷入长尾工作" |
| LOOP-10 | **步进快照（StepSnapshot）** | codex、dsh | P2 | `nodes/modelcall.cpp` 每次调用前捕获 | 一轮内若模型/工具集/权限发生变化（插件在轮中启用、模型切换），当前直接读会话最新设置，会出现"模型看到的工具与执行用的工具不一致"。最小形态：每次 modelcall 前捕获 `{模型, 工具名集合, 权限模式}`，工具分发只认快照 |
| LOOP-11 | **"无进展即错误"的断言** | pi | P2 | `AgentRunner` 中断循环 | `resume_async` 返回后若既没有新中断、也没有 `resumeValues`、也没有正常结束，应记录错误而不是静默退出（现在的 `unresolvedInterrupt` 判定已接近此意，补日志级别与指标即可） |

### 2.2 本项目已有优势（保留）

- 中断（HIL）可跨进程恢复：中断现场（节点、载荷、已完成工具结果缓存）序列化进图状态，重启后 `AgentRunner` 能续上 —— 六篇里 codex/dsh/opencode/pi 都没有对等能力。
- 主代理与子代理共用同一个 `AgentRunner` 循环（差异收敛为 hooks），从结构上消除两套循环的行为漂移。
- 取消/中断建模为**控制流**（`ExceptionClassification.isControlFlow`）而非错误，能穿透所有 `catchError`；`installExceptionClassifier` 让第三方库也认这套语义。
- `resumeCfg` 必须带 `cancel_token`、必须沿用首跑 `stream_mode/max_steps`，以及"中断未完成不能用 `result->interrupted` 判断"这几条都有源码注释记录，是踩过坑的经验。

---

## 3. 会话与 LLM 上下文

**现状要点**：`Session` 持有 typed 上下文（`messagesVersion` 单调）+ 展示历史（`viewMessages` append-only + 链式哈希）；图状态只有只读影子通道 `xx_messagesMeta`；写入口四个（`appendMessages` / `replaceMessages` / `replaceMessagesFromJson` / `truncateMessages`）；**每轮把系统提示词整体重建并就地替换首条 system 消息**；压缩是就地替换上下文。

### 3.1 建议吸收的设计

| 编号 | 设计 | 来源 | 优先级 | 落地位置 | 说明 |
|---|---|---|---|---|---|
| CTX-1 | **上下文来源化 + 基线/增量分离** | opencode、dsh、codex、openclaw | **P0** | `agent/lib/src/agent/prompt.cpp`、`nodes/agentcall.cpp`（安全边界采样）、`SessionStore`（基线持久化） | 现状：记忆文件、技能名单、时间、环境事实等动态内容混在每轮重拼的 system prompt 里，任何一处变化就让 provider 前缀缓存整体失效。改法：① 首轮（或压缩/切换工作目录后）渲染一次完整 system prompt 并把「各来源的值 + 渲染文本」落库；② 之后每轮只比较会变的来源，有变化就追加**一条中间 system 消息**（agentxx 的 `ChatMessage` 支持 system 角色），并在同一事务里推进快照。收益：长会话成本 + 可审计性（能回答"这一轮比上一轮多了什么"） |
| CTX-2 | **来源的三态语义：`unavailable` / 空值 / 有值** | opencode | P1 | 与 CTX-1 同批：`agent/lib/include/agentxx/agent/context_sources.h` | "取不到（保留上次生效值，不要把已生效内容判成被移除）"与"确实为空（可发撤销消息）"必须区分。现在两者都退化成"拼出来是空的"，导致环境探测失败会静默抹掉上一轮的事实 |
| CTX-3 | **注入内容的类型标记（source + 分类）** | codex、dsh、openclaw | **P0** | `conversation_types.h`（消息来源字段）、`nodes/session_context.h`、`middlewares/summarization.cpp` | 给注入进模型上下文的消息加轻量来源标记（`user` / `tool` / `plugin:<name>` / `summary` / `inject:<plugin>`）。用途：压缩时按来源决定可否合并/丢弃、UI 按来源分类展示、插件注入可被识别与清理、审计有依据。现在只能靠文本前缀匹配 |
| CTX-4 | **自定义条目 + 投影器** | pi、opencode | P1 | `Session` 新增 custom 条目 + `modelcall` 组装阶段投影 | 一类"属于会话数据但不进模型上下文"的条目（插件私有状态、轮次记录、工具附加上下文），由投影函数决定是否进入请求。收益：插件不必再"要么塞进上下文、要么自己找地方存"；也是 CTX-1 基线快照的天然落点 |
| CTX-5 | **只读快照共享（引用计数 + 版本号）** | codex、dsh | P2 | `agent/lib/include/agentxx/agent/context.h` 的 `messages()` / `llmMessagesJson()` 读取路径 | 现在多处按值拷贝消息列表（注释还提醒"拷贝后引用会失效"）。给会话上下文提供"只读快照句柄"（共享底层 + 版本号），把拷贝成本从"每条消息"降到"一次引用"。收益在长上下文时明显 |
| CTX-6 | **上下文只追加规则写入文档 + Debug 断言** | pi、openclaw | P1 | `docs/zh-cn/design/index.md` + `nodes/modelcall.cpp` 组装后 | 明确"同一会话的请求上下文只允许在尾部增长；中间插入会击穿前缀缓存"，并在 `repairMessages` 之后加一条 Debug 断言/日志（对比上一次请求的消息 id 序列是否为前缀）。成本近零，能防住中间件"插入式改写"带来的静默成本上升 |
| CTX-7 | **上下文变更的显式版本事件** | pi、dsh | P1 | `wire_protocol.h` 新增 Delta/事件类型 + `xx_messagesMeta.version` 已有 | 把 `xx_messagesMeta.version` 作为事件/增量发布出去（`ContextVersion`），使客户端与插件"以版本判断是否需要重新拉上下文"，不必比对内容哈希。同时解决"客户端拿到的条数/用量/消息列表来自不同时刻"的问题（一致读切面） |
| CTX-8 | **历史替换的原因记录** | dsh、openclaw、opencode | P1 | `Conversation`/`Session` 增加替换历史；`SessionStore::saveLlmMessages` 一并写 | 压缩、截断、工具去重现在都是 `replaceMessages` / `truncateMessages` 就地改写。增加一条"替换记录"（`{op, range, reason, atMs}`）随上下文持久化，让恢复与 UI 能解释"上下文为什么变短了" |
| CTX-9 | **重放重建入口（统一读路径）** | codex、harness、opencode | P2 | `session_store.cpp` + `checkpoint_store.cpp` 的读路径统一入口 | 把"从落库数据重建会话状态"做成显式函数，并写明资源边界（附件不重传、只读校验）。现在分散在 `SessionStore` 读取 + 图 checkpoint 两处 |
| CTX-10 | **会话工作上下文对象 + 切换点失效派生态** | opencode、harness | P2 | `agent/lib/include/agentxx/agent/context.h` + `middlewares/permission.cpp` | 把「会话生效工作目录 + 隔离边界」收敛为一个显式对象（`SessionWorkContext`），并规定 worktree 绑定/解绑、工作目录覆写变更时清空上下文基线、权限缓存等派生态。现在 `getSessionWorkDir` 与权限基准分散在两处，容易不一致 |
| CTX-11 | **文件/媒体引用替代内联 Base64** | codex、dsh、harness、openclaw | P1 | `conversation_types.h` 的附件处理 + `provider_common.h` | 展示历史已经剥离 `dataUrl`；再进一步：上下文里对图片/音频只存引用 id + 元数据，由 provider 层按需取用（压缩里的 `downgradeMultimodalUrlsToText` 旁已有 TODO）。避免长会话重复携带大块数据 |

### 3.2 本项目已有优势（保留）

- 「会话即上下文唯一权威 + 图状态只有只读影子通道」与六篇的方向一致（opencode/pi/dsh 都各自演化到类似结论），且 `state.serialize()` 的载荷与上下文大小无关 —— 长会话下这是关键。
- `repairMessages` 的五类修复（补 user 尾、清悬挂 tool_calls 与孤儿结果、修重复 `tool_call_id`、合并连续 user、空消息/UTF-8）集中在请求前一次完成，比 openclaw/opencode 把消息级修复分散在各阶段更完整、更好测。
- `RunConfig` 显式去掉 `StreamMode::VALUES`（每 super-step 全量状态序列化无人消费），是一次已有实测收益的优化。

---

## 4. 提示词、技能与记忆

**现状要点**：`AgentPrompt` = 主体文本 + `appendSystemPrompts`（按 key 字典序拼）+ `toolPrompt`（按工具名覆写描述）；动态段（skill 名单、记忆文件内容）每轮重建；技能是"名单 + 描述 + 路径"渐进式展开（正文由模型按需读）；记忆文件是**全量注入**。

### 4.1 建议吸收的设计

| 编号 | 设计 | 来源 | 优先级 | 落地位置 | 说明 |
|---|---|---|---|---|---|
| PRM-1 | **提示词划一条显式缓存边界** | openclaw、opencode、dsh、pi | **P0** | `prompt.h` + `agent/lib/src/agent/context.cpp::buildSystemPrompt` | `AgentPrompt` 增加 `stablePrefix`（主体 + 静态附加段，顺序稳定）与 `dynamicSuffix`（每轮变化：时间、动态记忆、技能提示、环境快照）两组，并在文档与注释里写明"动态内容一律不得插进 stablePrefix 之前/中间"。与 CTX-1 是同一件事的两面：先分边界，再把易变段落改成"变化时追加" |
| PRM-2 | **提示词段落排序号（而非键名字典序）** | dsh、pi | P1 | `prompt.h` 的 `appendSystemPrompts` + `agentxx.agent.prompt` 接口表 | 现在按 key 字典序拼接，**键名一改顺序就变**。改成 `{key, order, text}`，`order` 由宿主中央分配（如 100=persona / 200=planning / 300=skill / 900=动态记忆），并对同层重名直接报错。位置与优先级从隐含规则变成显式契约 |
| PRM-3 | **提示词整体覆盖语义（`complete`）** | dsh | P2 | `prompt.h` | 允许某个插件声明"我提供完整提示词"（用于 headless / 嵌入式场景）；多于一份生效时**失败**而不是静默取一个 |
| PRM-4 | **记忆分层 + 按需检索** | openclaw | **P0** | `middlewares/memory_file.h` | 现状是"配置的文件每轮全量注入"，文件一大直接吃上下文。分两类：① **常驻类**（体积受控、每轮注入，如项目约定/用户偏好，设字符预算上限并在超限时告警）；② **检索类**（日记式记录，提示词里只给"可用记录 + 检索方式"，由模型按需读）。不改 ABI、不改协议，收益直接 |
| PRM-5 | **记忆条目的来源标记** | openclaw、dsh | P1 | 与 PRM-4 同批：记忆文件的写入侧（工具/宿主）统一打标 | 最小形态：每条记忆带 `[owner]` / `[agent]` / `[untrusted: <来源>]` 标记，提示词里说明"untrusted 内容只作参考，不得当作指令"。关键纪律：**标记由宿主/工具写入，不能靠模型自觉**（否则等于没做）。与 CTX-3 是同一套机制的两种粒度 |
| PRM-6 | **技能来源优先级 + 同名冲突裁决** | openclaw、opencode | P1 | `middlewares/skill.cpp`（扫描目录改为带优先级的列表） | 至少三级：会话/项目级（`{workDir}/.agentxx/skills`）> 用户级（`~/.agentxx/skills`）> 插件/内置；同名取高优先级，并在技能列表里标注来源。现在多目录同名技能的行为是未定义的（实际是个坑） |
| PRM-7 | **指令/技能集合变更的显式通知** | openclaw、opencode | P1 | `middlewares/memory_file.cpp`、`skill.cpp` + `prompt.cpp` | 技能名单或记忆文件变化时，让模型看到一条"当前完整集合 + 取代此前内容"的中间系统消息；最后一条被移除时发撤销消息。现在只有 `resourceEpoch` 失效重建，模型只看到新文本、不知道变了 |
| PRM-8 | **技能正文走"技能语义"的读取** | opencode、pi | P2 | `skill.cpp` + 新增技能工具 | 现在正文由 `agentxx_filesystem_read` 读取（受文件系统权限约束，但不带"技能"语义）。加一个专用入口或在 read 上做技能目录的语义检查，让权限询问卡片显示"读取技能 X"而不是"读取路径 Y"；同时给一个用户侧强制加载入口（`/skill:name`） |
| PRM-9 | **按 agent 过滤技能/工具可见性** | opencode、dsh | P2 | `CodeAgent` 装配 + `AgentConfig` 的 per-agent 段 + 工具物化处 | 若将来支持多 agent 配置（主/子代理），把「技能列表」与「工具集合」按 agent 过滤后再进提示词与工具表；被拒绝的技能直接不出现在模型可见列表（不是靠模型自觉）。对应 dsh 的 scope + `restriction` 语义 |
| PRM-10 | **提示词快照回归** | codex、openclaw、pi | P2 | `agent/test/core` 新增模块 | 为默认提示词与几类典型拼装（有/无技能、有无 worktree、有无 planning、有无记忆文件）各存一份快照文本或哈希；改提示词文本时测试失败并要求更新快照。成本极低，能挡住"无意中改了提示词" |

### 4.2 本项目已有优势（保留）

- 技能注入方案省 token 且贴规范：只给名字/描述/路径，正文按需读（`line_limit=1000`），元数据校验按 Agent Skills 规范做了硬约束。
- 提示词是**数据**：`toJson` / `fromJson` / `mergeFromJson` / `promptHash` 支持整体序列化、按补丁合并、种群去重 —— 这是为"让 agent 自己改提示词"（`training.cpp` 的进化式训练）服务的，六篇里没有对等能力。
- `appendSystemPrompts` 按插件键位管理、卸载时按备份恢复；`toolPrompt` 可按工具名覆写描述文本。
- `renderVars` 只替换三个固定 token、不按 `fmt` 模板解析（自定义提示词里的 `{}` 不会抛异常），取不到值替换为 `unknown` 而不是留 token。
- `resourceEpoch` 让技能/记忆目录变更时的缓存自愈，不需要到处发通知。

---

## 5. 持久化、事务与崩溃恢复

**现状要点**：每个会话一个 SQLite 库（`view_message` / `llm_context` 单行 / `meta` / `store`）；上下文是**整表替换**；落盘是"轮内节流（3s）+ 轮末权威保存"；连接 LRU 上限 32；schema 靠幂等建表 + 手写 `ALTER`（无版本号）；`viewMessages` 有链式哈希；跨连接重放依赖**内存** delta 缓冲（服务重启后回退全量 Sync）。

### 5.1 建议吸收的设计

| 编号 | 设计 | 来源 | 优先级 | 落地位置 | 说明 |
|---|---|---|---|---|---|
| STO-1 | **会话目录写者所有权（内核级租约）** | codex、dsh、harness、opencode、openclaw | **P0** | `SessionStore` 打开写路径 | 六篇里有五篇提到，agentxx 是目前唯一完全没有保护的。做法：POSIX 用非阻塞 `flock`（Windows 用命名内核对象），**持有期 = 写句柄生命周期、不设超时**（崩溃由内核释放，杜绝"被抢占后两处写交错"）。openclaw 的四步顺序值得照抄：**取快照 → 准入断言（会话身份一致）→ 原子认领 + 事务内复验 + 回读确认 → 才允许作废前任**；判定的失败不得影响仍持有权威的一方。现状：多进程/多实例指向同一 `dataDir` 会互相覆盖 |
| STO-2 | **持久化格式版本 + 迁移链** | dsh、harness、opencode、openclaw、pi | **P0** | `SessionStore` 打开路径 + 新 `agent/lib/include/agentxx/util/migration.h` | `meta` 表加 `schema_version`；打开时 `version < current` 走链式迁移（每步一个事务、幂等），**`version > current` 拒绝打开**并给出明确错误（旧程序不开新库）；保留"首次迁移前自动备份 `session.db.bak`"作为安全网（本地数据没有 DBA，用户不会手工恢复）。现有的 `ensureViewMessageMsgIdColumn` 直接变成 `v1→v2` 的一步 |
| STO-3 | **会话事件序列 + `after` 游标重放** | opencode、dsh、pi、openclaw、harness | **P0** | `SessionStore` 新表 `event(session_id, seq, type, version, payload, time_created)` + `session_server_agent_io.cpp`（重放改为"先读库、再续实时"） | 只把**关键事实**事件化：消息追加、工具结算（成功/失败/被中断）、压缩开始/结束、上下文基线重建、权限决定、轮次开始/结束。读模型仍用现有表。收益：① 断线重连不再依赖内存缓冲，**服务重启后也能续传**；② 崩溃后能判断"上次做到哪"；③ 审计与将来 fork/回放有锚点。注意克制：**不要**做整库事件溯源（见 §17.5） |
| STO-4 | **durable 事件流与实时增量流分开** | opencode、openclaw | P1 | `wire_protocol.h`（新增 `WireGetHistory{after,limit}` / `WireHistory{events,hasMore}`）+ `session_server_agent_io.cpp` | 现在 delta 缓冲把两类流混在一起。区分后：**durable 流**可 `after` 重放与续接；**实时增量流**（文本/思考/工具参数片段）不保证重放。这样"重连后怎么恢复"有明确语义，也避免为文本片段做持久化（写放大） |
| STO-5 | **完成事件边界（started / ended）** | opencode、dsh | P1 | `summarization.cpp`、`toolcall.cpp` | 对"有中间态的长操作"统一 started/ended 两事件，**只有 ended 才投影为模型可见或持久生效的内容**。典型场景：压缩中途被杀不会污染历史（现在压缩写回即生效，看不出"上次压缩是否完成"） |
| STO-6 | **启动清账：跨进程遗留的"执行中"状态收尾** | opencode、dsh、codex | P1 | `BaseAgent` 会话初始化 + `nodes/toolcall.cpp` | 进程启动/会话恢复时，扫描状态为"执行中"的工具调用，统一落一条"工具执行被中断"的结果（而不是让它悬空）。现状：`interruptToolcallCache` 只在同进程内有效，重启后遗留状态没有任何收尾 |
| STO-7 | **用量账本（token / cost / cache read-write）** | pi、opencode、harness、openclaw | **P0** | `SessionStore` 新表 `usage`（`seq/turnId/msgId/provider/model/input/output/cacheRead/cacheWrite/cost/details`）+ `nodes/modelcall.cpp` 结算点 | 现在 token/成本统计散落在 `ContextStats`、会话 meta、UI 提示里，无法审计，也无法回答"这个会话/这一轮花了多少"。每次 LLM 结算写一行（含失败与重试），提供 `getUsageStats(fromSeq)` 聚合；客户端改为读账本。收益：可审计成本、可做按会话/按轮报表、为训练模式评分提供输入 |
| STO-8 | **耐久语义分级 + 屏障 API** | codex、dsh | P1 | `session_store.h` + 压缩完成、轮次开始/结束三个点 | 落盘调用分成"必须同步"（用户输入、工具结果、轮末状态、压缩结果）与"可延后"（展示历史节流、内存统计），并在 API 上显式命名（`persistNow()` / `persistThrottled(reason)`）。现状只有一个"是否节流"的布尔位，排障时看不出"这次落盘属于哪类场景" |
| STO-9 | **可见降级：落库失败要能被用户知道** | opencode、pi、harness | P1 | `SessionStore` 累计降级状态 + `SessionServerAgentIO` 推送 | 现在落盘失败只记日志（"尽力而为"）。保留不阻塞主流程的取向，但在会话上累计"持久化降级"状态并经事件/`WireContextStats` 推给 UI（让用户知道历史可能缺失）。同时区分"可忽略的展示写失败"与"上下文写失败"——后者应标记为不可持久化状态并明确告警 |
| STO-10 | **避免长耗时写入阻塞 io 线程** | openclaw、harness | P1 | `SessionStore` 大文本写入路径 | 所有 DB 访问现在都在 agent io 线程。最简改动：超过阈值的写入（超大工具结果、完整上下文）走线程池，写完 `asio::post` 回 io 线程更新内存状态。**注意**：`llm_context` 是整表替换，每次节流落盘都重写整段 JSON —— 更该先做的是"增量落盘"（配合 STO-3 的事件序列），而不是只搬线程 |
| STO-11 | **本地 SQL 错误分类（领域错误）** | harness | P2 | `agent/lib/src/util/sqlite.cpp` + `session_store.cpp` | `SqliteDb` 在错误上附加分类（`NotFound` / `Duplicate` / `Constraint` / `Busy` / `Io`），`SessionStore` 对外暴露枚举，便于上层写明显分支（"会话不存在则新建"与"繁忙则重试"）。现在只有异常 + 文本 |
| STO-12 | **设置库并发写用版本列** | harness | P1 | `settings_db.h` + 客户端写入路径 | `settings_db` 现由多客户端（TUI，将来 GUI）并发写。加 `version` 列，冲突时返回"已被其他客户端修改"，客户端用统一重试闭包（对齐 harness 的 `UpdateOptLock`），避免静默覆盖 |
| STO-13 | **大对象统一限额 + 定位符结构化** | harness、codex、openclaw、pi | P1 | `AgentConfig` 新增 `limits` 段 + `nodes/toolcall.cpp` + `SessionStore` | 把附件大小、share_store 单条大小、工具输出阈值、历史分页窗口统一到一处；超限给**明确错误码与原因**（不是静默截断）。见 TOOL-4/TOOL-5 |
| STO-14 | **提交失败即不可继续（分级）** | pi | P2 | `SessionStore` + `BaseAgent` 错误路径 | pi 规定"提交失败 → harness fault，必须重启"。agentxx 不必这么严格，但至少要把"上下文写失败"与"展示写失败"分开处理（见 STO-9） |
| STO-15 | **会话导出（HTML / Markdown / JSONL）** | pi | P1 | `agent/client` 新增 `export_html` / `export_md` 命令 + `lib` 提供 `SessionExport` 接口 | 复用 TUI 渲染路径把展示历史（含工具折叠、中断卡片结果）导出为自包含 HTML 或 Markdown，作为分享/存档/问题复现材料。成本低、用户感知强 |
| STO-16 | **失败会话取证包** | pi | P2 | `client` 命令 + `AgentContext` 已加载组件清单 | 一键导出：会话历史 + 日志尾部 + 配置（脱敏）+ 环境信息 + 插件清单。`collectAppendComponentInfo` 已有基础 |

### 5.2 本项目已有优势（保留）

- 按会话分库带来天然隔离：删会话 = 删目录、会话级 WAL 写串行、备份/迁移单会话 = 拷文件；连接 LRU 上限避免了 fd 耗尽（Linux 默认 `ulimit -n` 常为 1024）。
- `llm_context` 单行整表替换在短中会话下极简可靠（一次 `DELETE` + `INSERT`，无并发问题、无脏状态），SQLite 删除即时回收页面。
- 会话列表分页：keyset 游标 + 两阶段扫描（先 `stat` 拿近似顺序、**只作读取顺序启发绝不据此跳过目录**）+ 早停，支撑几百上千个会话。
- `SingleCheckpointStore` 每个 thread 只保留最新一个 checkpoint，把开销从 O(super-steps) 降到 O(threads)，并在注释里写明安全性依据。
- 展示历史批量提交带"过期下标丢弃"，`restore()` 后不会把旧批次写坏历史。

---

## 6. 工具系统

**现状要点**：`XXToolBase` 四个开关（`autoSummaryOutput` / `canDelayLoad` / `maxRetry` / `repeatCallCheck`）+ `ToolcallWrapNode` 顺序执行（`// TODO: 真正并行`）+ 中间件环绕 + 插件声明权限；输出超限走 share_store 保真落盘 + 预览 + 按行分页取回（已实现）；连续重复调用会询问用户（已有）。

### 6.1 建议吸收的设计

| 编号 | 设计 | 来源 | 优先级 | 落地位置 | 说明 |
|---|---|---|---|---|---|
| TOOL-1 | **同轮工具并行执行 + 结算按 `tool_call_id` 归位** | codex、dsh、openclaw、opencode、pi、harness | **P0** | `nodes/toolcall.cpp::baseRun`（`// TODO: 真正并行` 处） | 六篇全部提到，是收益最高的单点性能改进。要点：① 解析参数与**权限询问保持串行**（顺序确定、询问卡片顺序稳定），**执行阶段并发**；② 结果按 assistant **源顺序**写回上下文（不是完成顺序）；③ 取消语义要一致：已完成结果保留、未启动补占位、执行中尽量立刻中止（可参照 openclaw 的 `guard → commit → 实现` 三段相邻 + 启动期提交握手）；④ 结果按 `tool_call_id` 归位，不按完成顺序 |
| TOOL-2 | **并发上限与分类（读并行 / 写独占）** | harness、dsh、openclaw | **P0** | `AgentConfig` 新增 `toolcall.maxParallel` / `maxParallelPerKind`；`XXToolBase` 增加 `supportsParallel()`（默认 `false`） | TOOL-1 的必备配套：没有上限，并行会立刻变成资源炸弹（一次打出几十条命令）。规则：只读类（`FsRead` / `NetEgress`）可并行；写类、交互类、需要询问的保持串行；按作用域分类上限（命令类单独限 1）。默认关闭并发，按工具逐项开启 |
| TOOL-3 | **工具结果落上下文前的守卫** | openclaw、opencode | P1 | 新增 `agent/lib/src/nodes/tool_result_guard.cpp`（或 `execTool` 收尾处） | 三条检查：① 结果必须是合法 UTF-8 且能构成合法消息；② 超硬上限时截断但保留首尾，完整内容落 share_store 并给出取回方式；③ 与上下文里既有 `tool_call_id` 的配对关系自洽。现在工具返回任意文本会直接进入后续请求 |
| TOOL-4 | **工具结果的聚合预算（在已有 offload 之上）** | openclaw、harness、opencode | **P0** | `ToolcallWrapNode::execTool` 的 offload 分支 + `AgentConfig` 配额 | 现在只按**单个**调用限幅：五个 15K 的结果照样把上下文塞满。补"一次请求内所有工具结果合计上限"（按模型窗口折算，如窗口 × 50%）；超合计上限时按**最旧优先**把已完成的工具结果替换为 offload 预览（内容已在 share_store，不需要新机制） |
| TOOL-5 | **预览口径升级：首尾保留 + 中段省略 + 结构化定位符** | openclaw、opencode、harness | P1 | `ToolcallWrapNode::execTool` 的预览文本生成 | 现在预览是单向截断（只留开头）。工具输出常见"开头是命令、结尾是错误摘要"，应保留首尾、中段插省略标记。同时把定位符从**自然语言文本**改为**结构化字段**（`{spillId, lines, bytes, kind}` 写进结果 metadata），这样 UI 能提供"展开原文"按钮、模型/插件能可靠解析。压缩恢复场景用不同的提示后缀 |
| TOOL-6 | **缓存过期感知的结果剪枝** | openclaw | P1 | 与 TOOL-4 同批：offload/剪枝判定处 | 已过 provider 缓存 TTL 的旧工具结果**优先剪**（图片换 `[image removed during context pruning]`、文本换 `[Old tool result content cleared]`）。理由：既然不在缓存里了，剪掉不破坏前缀命中，是最便宜的可回收空间。agentxx 若接入了缓存统计（LLM-5）就能精确实现；没有时按"轮数/时间"近似 |
| TOOL-7 | **工具调用超时预算（分发层保证，而非工具自觉）** | harness、codex、dsh | **P0** | `nodes/toolcall.cpp::execTool` + `XXToolBase` 声明预算 | 现在超时靠各子系统自己配（MCP 有、命令有、其它不一定），没有统一"每个工具调用都有硬超时"。做法：工具只声明预算（`timeoutMs`，0=不限），由分发层竞速取消 + 结果归一化为"超时"结果给模型，而不是让整轮卡死。与 LOOP-8 的优雅中断共用实现 |
| TOOL-8 | **错误分类：可让模型改正 vs 致命** | codex、harness、opencode | P1 | `nodes/toolcall.cpp` + `util/exception.h` | 在 `[Exception aborted: ...]` 之外区分"参数错误（可让模型改正重试）"与"致命错误（应结束本轮）"。现在两者都变成一条工具结果，模型可能反复重试必然失败的操作（例如余额不足、凭证缺失） |
| TOOL-9 | **审批/询问缓存（最小化打扰）** | codex、harness、openclaw | P1 | `middlewares/permission.cpp` + `settings_db` | 缓存键取"工具 + 规范化目标（程序/路径前缀）"，命中即跳过询问并注明命中来源；对只读操作默认不询问（可按配置放开）。与 SEC-3 的"记住的选择持久化"是同一件事的两面 |
| TOOL-10 | **工具注册表治理：保留名 + 冲突记录 + 暴露等级** | codex、opencode、dsh | P1 | `agent/lib/src/plugins/tool_registry.cpp` + `BaseAgent` | 三条硬规则：① 内置/保留名不允许外部覆盖（现已有静态工具名冲突检测，补"显式拒绝 + 可读原因"）；② 重名不覆盖而是记录冲突并告警（补"首个冲突"记录与对上层可见的错误码）；③ 为工具加"暴露等级"（始终可见 / 延迟搜索可见），与 `canDelayLoad` 对接，便于按命名空间或来源分组控制 schema 预算 |
| TOOL-11 | **陈旧调用防护（公告身份）** | opencode、openclaw | P1 | `nodes/modelcall.cpp`（请求带工具定义指纹）+ `nodes/toolcall.cpp`（结算前比对） | 工具匹配"最新活动注册"。插件启用/禁用/热重载后，旧调用可能打到新实现。做法：请求组装时冻结每个工具的"定义身份"（工具名 + 定义哈希 + 插件实例 id），结算前比对，不一致就把该调用结算为"工具已变更，请重新调用"，而不是执行新实现 |
| TOOL-12 | **「可用性与授权」两层拆开** | openclaw、opencode、dsh | P1 | `AgentContext` 新增工具可用性计算；权限中间件保持授权职责 | 可用性只看运行环境：模型是否支持并行工具调用、客户端是否支持附件、插件是否已禁用、沙箱/worktree 是否激活；授权仍由权限中间件判定。收益："工具没出现"与"工具被拒"在诊断上归因明确 |
| TOOL-13 | **工具面诊断入口** | openclaw | P2 | TUI 命令 + `AgentContext` 查询接口 | 输出一条"为什么这个工具没出现/为什么被拒"：依次列出「插件是否加载 → 是否声明权限 → 权限判定结果 → 是否延迟加载未命中 → 是否被模型能力过滤」。agentxx 策略层数比 openclaw 少，做起来更便宜、收益更直接 |
| TOOL-14 | **参数流式差分展示** | codex | P2 | `nodes/modelcall.cpp` 的流事件处理 + UI 层 | 工具参数在流式生成时可增量展示（如写文件的路径、补丁片段），改善长参数工具的等待体验 |
| TOOL-15 | **工具调用分段计时与来源标记** | codex | P2 | `nodes/toolcall.cpp`（已有 `startTimeMs/durationMs`）+ 事件 | 记录"派发等待 / 执行 / 总耗时"三段与来源（模型直连 / 插件内部 / 子代理），**仅在直连调用上计时**（避免嵌套重复计数）。既是 UI 展示数据，也是排查"用户觉得卡"的第一手证据 |
| TOOL-16 | **结构化结果与模型可见投影分离** | opencode、dsh、pi | P2 | `tools/tool.h` + `ToolcallWrapNode` 写回段 | 在 `execute_async` 之外增加可选的结构化结果出口（JSON），宿主负责"投影为模型文本"与"交给 UI/插件"。收益：现有插件渲染器可直接吃结构化结果，不再解析文本；限幅也能按文本部分计量而不动结构化字段。跨 C ABI 边界的形态建议是"插件返回 JSON + 宿主按描述裁剪" |
| TOOL-17 | **压缩后重复调用守卫** | openclaw、dsh | P2 | `summarization.cpp` + `findConsecutiveRepeatCallKeys` | 现有检测是"连续 N 次相同调用"。补一条针对性的：**压缩发生后的下一轮**若出现与压缩前相同的 `(tool, args, result)` 三元组，判为无进展并中止本轮或强制询问（对症掐"溢出 → 压缩 → 又重复同一调用"的死循环） |
| TOOL-18 | **文件写工具的统一串行化** | pi | P1 | `agentxx_filesystem` 插件写路径（或宿主提供 `utilxx::withPathMutex`） | 按「环境 + canonical 路径」串行化整个"读-改-写"过程；队列用完即删（避免 Map 无限增长）。防止并发写同一文件互相覆盖（TOOL-1 并行后这条更必要） |
| TOOL-19 | **中断幂等 helper + 必做清单** | harness、dsh | P1 | 新增 `co_await interruptOnce(key, fn)` + `AGENTS.md` 清单 + 测试模板 | agentxx 已经用 graphData 键**逐点实现**了幂等（`summarizationTipMsgId` 复用、`interruptToolcallCache` 复用），但没有框架支持也没有清单，新写中断点很容易漏。补：① helper（内部按 key 记录已执行）；② 必做清单（记录状态 → 恢复时按状态分支）；③ 测试模板（同一次中断跑两遍，断言副作用只发生一次） |
| TOOL-20 | **执行环境加固（成本极低，收益直接）** | codex | **P0** | `agentxx_execute_command` 插件 + 工具执行器 | 命令执行时固定注入：`NO_COLOR=1`、`TERM=dumb`、`LANG/LC_CTYPE/LC_ALL=C.UTF-8`、`COLORTERM` 清空、`PAGER/GIT_PAGER/GH_PAGER=cat`、`CI=1`，以及会话标识（`AGENTXX_THREAD_ID` / `AGENTXX_VERSION` / `AGENTXX_PERMISSION_PROFILE`）。现在直接继承父进程环境，容易出现"命令调分页器卡住""输出带 ANSI 影响解析""区域设置导致编码错乱" |

### 6.2 本项目已有优势（保留）

- 四个开关正好覆盖四类现实问题（工具太多 → 延迟加载、输出太长 → 自动压缩、网络抖动 → 重试、模型打转 → 重复询问），且可按工具独立配置 —— 六篇里 pi/opencode 都没有延迟加载或重复调用确认。
- 循环检测实现讲究：限定"`llm ↔ tool` 交替链"避免误判正常对话，回溯有界 + 提前终止，阈值可配，询问复用已有 HIL 通道。
- `autoFixArgsType` 参数类型自愈（string↔数组/数值/布尔、单元素数组解包），对弱模型更友好。
- 中断恢复不重复副作用：按 `tool_call_id` 的 `toolcallsCache` 直接复用中断前已完成的结果（耗时补 0）。
- 热插拔安全：动态工具表以 `shared_ptr` 持有，执行中调用靠引用计数保活，插件卸载要等 inflight 归零才 `dlclose`。
- 参数自动注入 `sessionId` / `tool_call_id`，工具不必自己从上下文猜身份。
- 按工具定制压缩：`SummarizationToolHandle`（去重 key + 截断 request/response）把"这类工具的输出怎么压缩"交给工具自己声明。
- `execute_command` 的工程细节完整：`polled_tool` 不占线程、Linux `setsid` / macOS `setpgid` / Windows Job Object 进程组清理、`CancelRegistry` 事件驱动 kill（不再是 20ms 轮询）、stdout/stderr 分开独立压缩 + `[Content offloaded]` 定位符。

---

## 7. LLM 层（协议、provider、鉴权与缓存）

**现状要点**：`ModelProviderRegistry`（命名模型 + provider 缓存，会话级模型选择）；三类协议（Anthropic / OpenAI Responses / OpenAI Chat）；重试在 `modelcall` 节点里（固定退避 `retry*3s`，限速关键字再 `+retry*5s`，上限 `llmMaxRetry`）；无限速之外无错误分类、无 prompt 缓存断点、无鉴权分层、无缓存用量统计。

### 7.1 建议吸收的设计

| 编号 | 设计 | 来源 | 优先级 | 落地位置 | 说明 |
|---|---|---|---|---|---|
| LLM-1 | **模型空闲看门狗（静默超时）** | openclaw、dsh、opencode | **P0** | `nodes/modelcall.cpp` 的流式回调 + `ModelConfig` | 现在 provider 长时间不吐 chunk 只能等整体超时，用户侧表现为"卡住"。做法：记录"距离上次收到 chunk 的静默时长"，超阈值（建议默认 120s，可按模型覆盖）即中止本次请求并按既有重试策略处理。与"任务本来就长"区分开 |
| LLM-2 | **provider 错误分类 + 溢出一次性压缩重试** | openclaw、opencode、dsh、harness | **P0** | `protocol/provider_common.h` + `nodes/modelcall.cpp` + `summarization.cpp` | 至少四类：**上下文溢出 / 鉴权失败 / 限流 / 超时**。溢出时（且本轮尚无 assistant 输出）触发一次压缩后重试，**复用已完成工具结果、不重放副作用**；第二次溢出直接失败（不循环）。这是长会话里最常见的一类失败，agentxx 现在完全没有这条路 |
| LLM-3 | **重试策略数据化（可重试性分类 + 状态码/响应头 + 抖动）** | opencode、pi、dsh、codex | **P0** | `ModelConfig` 增加 `retry` 段 + `nodes/modelcall.cpp` 失败分支改为调用策略接口 | 三件事：① 按状态码分类（429/503/504/5xx 可重试；额度/计费类**直接失败**不再退避 —— 现在余额不足也会退避重试 6 次）；② 优先读 `retry-after-ms` / `retry-after`，否则用带抖动的指数退避（如 `BASE × 2^attempt × [0.8, 1.2]`，有上限）；③ 替换现在的关键字匹配（依赖各网关文案，脆弱）。策略放在 provider/模型的**数据**里，执行留给节点或可替换中间件 |
| LLM-4 | **重试结构化事件（UI 可画倒计时）** | pi、opencode | P1 | `wire_protocol.h` 的 `WireDelta::Type` 新增 `RetryScheduled` | 现在只有一条 `MessageUITip` 文本。改成带 `attempt` / `maxAttempts` / `delayMs` / `notBefore` / `errorMessage` 的结构化事件，UI 渲染倒计时（`modelcall.cpp` 与 `summarization.cpp` 的退避点共用） |
| LLM-5 | **缓存断点策略 + 缓存用量统计** | opencode、openclaw、codex | **P0** | `protocol/anthropic_provider.cpp` / `openai_provider.cpp` 的请求组装 + `ModelConfig` 的 `cacheRetention`；统计进账本（STO-7） | Anthropic 侧在"最后一个工具定义 / system 末段 / 最新用户消息"放 `cache_control: ephemeral`（可选 TTL）；OpenAI 侧主要是"保持前缀稳定 + 路由亲和"，前提是 PRM-1/CTX-1 先做。同时把 `cache_read_input_tokens` / `cache_creation_input_tokens` 计入账本。收益口径（opencode 注释实测）：Anthropic 缓存写入 1.25×、读取 0.1×，长会话多轮复用即可回本 |
| LLM-6 | **模型能力元数据集中化 + 来源化** | codex、dsh、opencode、pi、openclaw | P1 | `model_registry.h` 的 `ModelConfig` + yaml 模型段（可选插件提供远程目录） | 补：上下文窗口 / 最大输出 / 价格 / 模态（文本·图片·推理）/ 是否支持工具调用·并行工具·结构化输出·缓存。用于：压缩预算（现在是固定常量 + 经验系数）、附件按钮（已有部分）、UI 展示与成本统计。来源优先级：内置默认 < 配置文件 < 远程目录（可选，带本地缓存与定时刷新）< 插件补充 |
| LLM-7 | **模型级回退链（轮次局部，不改会话选择）** | openclaw、dsh | P1 | `ModelConfig` 增加 `fallbacks: [...]` + `nodes/modelcall.cpp` | "重试耗尽且错误属于可回退类别（限流/鉴权失败/服务不可用）"时切到下一个候选。关键规则照抄 openclaw：**回退是轮次局部的，不改变会话选择**，只在结果里标注"本轮由 X 回答" |
| LLM-8 | **多鉴权档位与冷却** | openclaw、pi、opencode | P2 | `ModelConfig`（`apiKeys` 列表）+ provider 层轮换 | 最小形态：一个模型条目可配多个 api key，失败后轮换并给该 key 加冷却时间；不需要 OAuth。对"多个 key 分摊限流"的场景直接有用。OAuth 全家桶属方向性投入，见 §17.3 |
| LLM-9 | **能力协商下沉到 provider（按实际路由决策）** | dsh、opencode | P1 | `model_registry.h` / `provider_common.h` | 把"上下文窗口、是否支持 reasoning、是否支持并行工具、提示词更新方式（能否在缓存历史之后追加）"作为 provider 暴露的能力事实，而不是散在配置与各调用点。压缩阈值、UI 提示、提示词更新策略都能按真实路由决策 |
| LLM-10 | **消费端退出传播取消（RAII）** | codex、dsh、opencode | P1 | `nodes/modelcall.cpp` 的流等待对象 | LLM 流被上层放弃时（UI 关闭、轮次被替换），现在依赖调用方显式取消。仿 `ResponseStream::Drop` 用 RAII 触发取消，避免上游连接悬挂 |
| LLM-11 | **流式组装唯一实现** | dsh、opencode | P2 | `protocol/provider_common.h` | 把"chunk → 完整消息"的组装收敛成一份算法（容忍只有 delta 的简化协议、忽略已 `block-end` 之后又到的 delta），各 provider 只做协议解析，UI 侧不再各自拼装 delta |
| LLM-12 | **图片细节按模型能力归一化** | codex、pi | P2 | provider 请求组装处 | 不支持 `original` 细节就降级为默认；部分模型直接去掉 detail 字段。把同类能力差异在**请求组装处统一处理**，不散落在各调用点 |
| LLM-13 | **假 provider（可脚本化，用于确定性测试）** | pi、opencode、harness | **P0** | `agent/test/core/fake_provider.*` + 复用 `ModelProviderRegistry::setProvider` | 按输入返回预设流/错误/延迟。用于：工具循环、重试、压缩、中断、取消、恢复的**会话级端到端**测试，不依赖真实网络与付费额度。六篇里有三篇把它列为高优先级 |
| LLM-14 | **HTTP 录制回放** | opencode、harness | P1 | 新增 `agent/test/http_recorder`（或在 `protocol` 抽一层可注入传输）+ `agent/test/fixtures/recordings/` | 录制模式下保存请求-响应对，回放模式下按顺序/哈希匹配返回。用于协议适配回归（请求体与流式解析）与无需真实 key 的端到端会话测试 |
| LLM-15 | **结构化输出（强制工具式，跨协议一致）** | opencode、pi | P2 | `protocol/protocol_base.h` 家族新增 `generateObject(schema)` | 内部合成一个强制调用的工具，把模型输出按 JSON Schema 解码。用途：会话标题、任务规划、摘要（可让压缩走结构化输出，避免解析自由文本）、插件参数化输出 |

### 7.2 本项目已有优势（保留）

- 自研 provider 实现（对协议细节可控：thinking 提取、工具调用增量拼接、各家灰度差异），且**面向真实网关的修复**齐全（`repairMessages` 的五类修复）。
- 多模态面比 pi 宽：图片 / 音频 / 视频三类，客户端能力表驱动 UI。
- 服务端自主加载附件：只传路径时由服务端读文件编码（读盘与 base64 卸载到线程池，避免阻塞同线程所有会话），读取前按媒体类型做体积上限检查。
- 模型切换是一等操作：按 `sessionId` 隔离，同一进程不同会话可用不同模型。
- 压缩的 API usage 优先 + 估算兜底口径，以及 token/s 实时速度统计（`ContextStats`）。

---

## 8. 上下文压缩与预算控制

**现状要点**：阈值触发（75%）+ 确定性压缩先行（噪音清理 / 工具去重 / 探索型折叠 / 多模态降级）+ 同上下文子代理摘要 + 冷却 + `hardTruncate` 兜底；压缩后**就地替换上下文**；无 started/ended 完成边界；无"溢出错误重试"；摘要模板只是 `appendSystemPrompts["summarization"]` 里的一段字符串。

### 8.1 建议吸收的设计

| 编号 | 设计 | 来源 | 优先级 | 落地位置 | 说明 |
|---|---|---|---|---|---|
| CMP-1 | **预算口径统一到一处** | openclaw、dsh、harness、opencode | P1 | 新增 `ContextBudget` 结构 + `middlewares/summarization.cpp` | 现在三处口径并存（压缩判断 / 统计展示 / 实际请求），且系数是经验值。把「模型上限 / 估算系数 / 预留输出 token / 压缩阈值 / 兜底阈值」收拢到一处，压缩、统计、预检都读它；估算口径与 opencode 对齐（system + messages + **tool definitions**，buffer 与输出上限取大者） |
| CMP-2 | **压缩产出结构化摘要 + 保留尾部原文窗口** | opencode、pi、openclaw | **P0** | `middlewares/summarization.cpp` + `AgentConfig` 的 `compaction.keep.tokens` | 两件事一起做：① 摘要模板改成固定结构（目标 / 关键细节 / 工作状态（已完成·进行中·受阻）/ 下一步 / 相关文件），并写明"保留精确路径、命令、错误串"；② 压缩产物 = **结构化摘要 + 按 token 预算（默认 8K）保留的最近消息原文**（现在是"摘要 + 最近段"，缺的是把最近段预算做成显式配置项与"保留原文可回取"）。收益：压缩后模型不丢最近细节，少一轮试错 |
| CMP-3 | **多次压缩滚动合并** | opencode、pi | P1 | `summarization.cpp` 的摘要提示词与结果落库处 | 重复压缩时把**上一份摘要**与新对话一起给出，规则写明"冲突以新对话为准，丢弃已完成项，保留仍需的约束与工作流"。现在每次生成独立摘要，多次压缩后信息容易漂移 |
| CMP-4 | **压缩改成"保留尾部 + 不改历史"并保留原文可回取** | pi、dsh、harness | P1 | `summarization.cpp` 生成阶段 + `Session` 的"上下文起点"概念 + CTX-4 的 custom 条目 | 现状压缩就地替换上下文，被压缩掉的原文对模型永久消失（展示副本还在但没有机制带回）。建议：把被压缩段落以不可变条目留在会话数据里，并把摘要作为**上下文起点**而不是替换上下文，即 `上下文 = [摘要] + 保留尾部`；提供工具/命令"把某段原始历史取回上下文" |
| CMP-5 | **压缩的完成边界事件化（started/ended）+ 幂等键** | opencode、dsh、openclaw | P1 | `summarization.cpp` + `SessionStore` 事件表（STO-3/STO-5） | 只有 `ended` 才投影为模型可见的压缩消息，中途失败/被杀时上一版历史边界继续生效；同时给一次压缩一个单调 `compactionGeneration`（同一段上下文只允许压缩一次，重试发现已压缩则跳过） |
| CMP-6 | **压缩结果带恢复元数据** | codex、opencode、pi | P1 | 会话 meta / 新 `compaction` 记录 + `summarization.cpp` | 记录：摘要对应区间（消息 id 范围或版本号）、`tokensBefore`/`tokensAfter`、摘要用量、触发原因（阈值·溢出·手动）、使用的模型、时间戳、保留段起止。收益：可审计"每次压缩省了多少、代价多少"，作为阈值调优依据，也让"跳回压缩前"有依据 |
| CMP-7 | **压缩质量门（轻量）** | openclaw、dsh | P2 | `summarization.cpp::doSummarizeWithLLM` 之后 | 对摘要做最小校验：非空、长度在预期范围内、包含要求的小标题/关键标识符；不合格重试一次，仍不合格就退回确定性压缩**而不是写入坏摘要**（对应 openclaw 的 "不合格就不写"） |
| CMP-8 | **压缩切点对齐用户消息跨度** | pi、dsh | P2 | `splitRecentByTokenBudget` | 切点优先落在用户消息边界（`role=="user"` 可识别），只有当单个用户跨度超预算时才在内部切，并记录 `isSplitTurn` 标记供提示词区分。现在按 token 预算切，可能在"用户任务中途"切断语义单元 |
| CMP-9 | **压缩前提醒写记忆** | openclaw | P2 | `summarization.cpp`（压缩前追加一条提示） | 成本极低：压缩前插一条"先把值得长期保留的信息写入记忆文件"的自动提示，能显著提高跨会话记忆质量 |
| CMP-10 | **压缩策略可干预接口** | pi、dsh、opencode | P2 | 中间件链（与现有 `onModelcallRun` 同级） | 给插件一个"压缩前置钩子"（可拒绝本次压缩、可直接提供摘要、可调整切点），并把来源标记（`fromHook`）写进压缩记录，便于区分"模型摘要"与"扩展提供" |
| CMP-11 | **确定性旧工具结果裁剪先于整体压缩** | opencode、openclaw | P2 | `nodes/toolcall.cpp` 写回段 | 对"很久之前且很大的工具结果"做按需裁剪（先于整体压缩发生），减少压缩频率。与 TOOL-6 的缓存过期剪枝是同一机制的两个触发条件 |
| CMP-12 | **手动压缩的聚焦指令** | openclaw、opencode | P2 | `WireCompactContext` 增加可选 `focus` 字段 | `/compact [聚焦指令]`，宿主限制长度并转义为提示词数据（openclaw 限 800 码点）。客户端侧手动压缩用 `keepRecentTokens` 作为切点预算 |

### 8.2 本项目已有优势（保留）

- 三层降级保证"请求一定发得出去"：确定性压缩 → LLM 摘要 → `hardTruncate`（且 `hardTruncate` 有两级兜底：丢最旧 recent → 对最后一条做二分截断）。
- 确定性阶段先行：噪音折叠、多模态 URL 降级、工具输出去重（dedup key）、探索型调用折叠 —— 这些是长会话真实省 token 的手段，pi/opencode 没有对等机制。
- 压缩子代理**同 thread id + 同模型 + 无工具 + 禁二次压缩**，直接命中 provider 的 KV/prefix 缓存，是很懂成本的一笔。
- 冷却规则贴合真实病根：用"消息条数增长 ≤2 且仍超阈值"判断"LLM 摘要没起作用"，跳过无效压缩直接降级。
- 恢复期提示复用（`summarizationTipMsgId` 用更新而非追加），中断续跑不会留下多条永远停在 "Summarizing LLM Context..." 的提示。
- 压缩完成即写回 + 请求节流落盘，崩溃后不会反复压缩。

---

## 9. 权限、审批与安全边界

**现状要点**：权限中间件统一判定（worktree 隔离 → 配置拒绝 → 完全授权 → 最长前缀规则 → 模式兜底）+ 插件声明式权限目标 + `check_paths` 三态批量复核 + 声明式中断卡片 + "记住选择"；**没有**：审批审计、判定理由、作用域扩展（只有读/写路径）、出网策略、执行身份绑定、符号链接解析、把"权限 ≠ 沙箱"写清。

### 9.1 建议吸收的设计

| 编号 | 设计 | 来源 | 优先级 | 落地位置 | 说明 |
|---|---|---|---|---|---|
| SEC-1 | **权限声明全覆盖（最危险的工具当前没声明）** | harness、codex、openclaw | **P0** | 各插件 `start` 事务 + `plugin_manager_vtable.cpp` + `middlewares/permission.h` | 源码核实：全仓只有 `agentxx_filesystem` 调用 `register*PathPermission`；`execute_command`、`computer_use`、`screen_capture`、`text_selection_monitor`、`websearch`、`rag_search`、MCP 桥接工具**都没声明权限**，按"未声明即放行"直接绕过闸门 —— 破坏力最大的恰好不在闸门内。做法：① scope 从"读/写"扩展为枚举 `{FsRead, FsWrite, NetEgress, ProcessExec, DeviceCapture}`；② 逐个插件补声明（命令取 `targetKind=Text` 的命令文本；websearch/rag 取 URL/域名；桌面控制取 `DeviceCapture`）；③ 宿主侧加"有副作用但未声明权限的工具 → 记 warning 或按配置拒绝加载"的检查 |
| SEC-2 | **判定结果结构化 + 理由** | harness、dsh、openclaw | **P0** | `middlewares/permission.h` 的 `PathDecision` → `PermissionDecision{decision, reason, rule{source,pattern,scope}, target, category}` | `reason ∈ {SessionIsolation, ConfigDeny, FullAuth, RuleAllow, RuleDeny, RuleAsk, ModeDefault, Unresolved}`。用途：日志一行说清理由；询问卡片显示"为什么问"；Info/诊断展示有效规则视图。现在只有 bool/三态，用户与日志都答不出"是被配置拒绝、隔离拒绝还是规则拒绝" |
| SEC-3 | **审批记忆持久化 + 撤销入口** | openclaw、opencode、pi、codex | **P0** | `middlewares/permission.cpp` 的 remember 分支 + `settings_db` + TUI 设置项 | 现在"记住的选择"只在内存，重启即失效。落库（按项目/工作区 + 作用域 + 目标 + 允许/拒绝 + 时间），并提供"列出已记住的规则 + 删除"的界面能力。收益是用户体感最明显的一条 |
| SEC-4 | **审批的连带处理** | opencode、codex、openclaw | P1 | `permission.cpp` 的应答处理（需按会话建"待决定请求登记表"） | ① 用户拒绝其一 → 同一会话其它待决请求一并拒绝（避免悬挂）；② 选择"始终允许" → 重新评估其它待决请求，已满足的直接批准并通知。现在每个询问独立处理 |
| SEC-5 | **待决定请求可查询（重连后恢复询问界面）** | opencode、openclaw | P1 | `wire_protocol.h`（`WireGetPermissionState` 扩展返回待决列表）+ `session_server_agent_io.cpp` | 现在重连后询问界面无法恢复（只能等超时）。同时把"某工具在等人工决定"表达成可查询的挂起项（谁在等、等了多久、超时策略） |
| SEC-6 | **符号链接真题（已知缺口）** | codex、harness、openclaw | P1 | `permission.cpp` 的 `decideTarget` / `decidePaths` + 路径规范化函数 | 源码已挂 TODO：判定基于**词法规范化路径**，允许范围内的链接（`<root>/link -> <root>/deny`）被读写时会跟随进入被拒目录，逐路径过滤也看不到链接目标。做法：对**已存在**的路径再取一次 `std::filesystem::weakly_canonical` 判定（保留词法判定作第一道），并缓存规范化结果避免热路径重复系统调用；评估 Windows 语义与性能。建议"写操作优先"先落地 |
| SEC-7 | **出网策略（netpolicy）** | harness、codex、openclaw | **P0** | `cxx_utilxx_base` 新增 `net_policy.h` + `cxx_utilxx` 的 `http_client`/`ws_client` + 配置项 | 现状：websearch / rag / MCP 可访问任意地址（含内网），没有统一出网闸门。做法照抄 harness：① 地址分类 `{Public, Loopback, LinkLocal, Private, Reserved}`（先 `Unmap()` 处理 IPv4-mapped IPv6，multicast 优先于 link-local，6to4/Teredo/NAT64 归 Reserved）；② 在 connect 之前判定（等价 `Dialer.Control`：解析完成、连接发起前拒绝，被拒地址永不接触，**既不会成功握手也不报连接错误，无法用来探测内网**）；③ 对外部进程（`git clone`、stdio MCP server）用"先解析校验再交给进程"的路径；④ 错误统一为 `address not allowed`，不区分"被拒"与"不可达"；⑤ 默认只允许公网，配置可放开 `network.allowPrivate/allowLoopback/allowLinkLocal` |
| SEC-8 | **审计留痕（权限决定 + 工具执行）** | harness、codex、openclaw、pi | P1 | 会话库或独立 `audit.db` 新表 + 各执行点 | 字段：`ts / opId / sessionId / actor(tool·user·plugin) / target / scope / decision / reason / remembered / result / durationMs`。openclaw 的经验值得抄：**审计账本有意只存元数据**（不复制提示词、消息、参数与结果内容），降低二次泄露面。同时提供"列出并撤销已记住的规则" |
| SEC-9 | **执⾏身份绑定（审批看到什么就执行什么）** | openclaw、codex | P2 | `execute_command` 等工具的批准流程 + `permission.cpp` | 现状批准的是"这次调用"，执行时重新解析命令。最小版本：批准时记录解析出的可执行文件绝对路径 + argv + cwd（可写可执行文件再加内容哈希），执行前复验；**批准窗口内解析结果变化即拒绝**；**绑定不出唯一文件时拒绝铸造批准**（不要假装覆盖） |
| SEC-10 | **作用域扩展与有效规则视图** | harness、dsh | P1 | `permission.h`（scope 枚举，见 SEC-1）+ 新增 `listEffectiveRules(sessionId)` | 让 `execute_command` / `websearch` / `screen_capture` / `computer_use` 用同一套判定与询问；并给客户端一个"当前状态"视图：是否完全授权、当前模式、已记住的允许/拒绝、隔离边界。现在这些状态分散在 `fullAuthorized_`、`sessionIsolations_`、router 规则表三处 |
| SEC-11 | **规则自带样例校验** | codex | P1 | `permission.cpp` 的规则注册 + 配置校验路径 | 路径/命令规则可附可选 `examples` 段（正例/反例），启动时跑一遍校验，把"规则写错导致误放行/误拒绝"挡在运行之前（codex 的命令策略是 Starlark 脚本且加载期校验样例，agentxx 只需覆盖子集） |
| SEC-12 | **规则定义与判定分离（可序列化）** | harness、codex | P2 | `permission.cpp` 的规则项 + 配置形态 | 把规则从运行时状态升级为可序列化定义（`{scope, target, operator, source(配置·用户记住·隔离·模式), createdAt, createdBy}`），判定走纯函数并产出带 `code` 的违规说明。收益：规则可导出/导入、可审计、可用构造 Input 单测（无需真实会话） |
| SEC-13 | **待决定请求/审批的拒绝分类与反馈文本** | opencode、dsh | P2 | `permission.cpp` + `interrupt_presets.h` | 拒绝时结果带结构化错误码与"面向用户的原因"，与模型可见内容分开（opencode 有 `DeclinedError` / `CorrectedError`）；`reason` 与 `displayReason` 分离（审计文本 vs 本地化提示） |
| SEC-14 | **责任边界与安全模型文档化** | pi、opencode、codex、openclaw | **P0** | `docs/zh-cn/design/index.md` 或新增 `security.md` | 明确写出：权限中间件是**策略层不是沙箱**；工具以宿主进程权限运行；worktree 只提供写边界；不可信仓库与提示注入属用户责任；列出已覆盖与未覆盖的攻击面（含 SEC-6 的符号链接缺口）。成本近零、收益最持久 —— pi 的 `SECURITY.md` 把"不做什么"写得比"做什么"还清楚 |
| SEC-15 | **项目信任决策点** | pi、opencode | P1 | 会话资源配置路径 + 客户端复用中断卡片 | 会话首次进入某工作目录时询问一次"是否信任该目录的资源"（技能/记忆文件/插件配置）；未信任则不加载项目级资源。防住"克隆一个仓库就自动加载其中的 AGENTS.md/技能指令" |
| SEC-16 | **内容侧安全（可选）** | harness | P2 | 写文件/提交前 | 扫描待写入内容里的密钥（对齐 harness 的 `scan_secrets.go`），与权限中间件互补。可选，按需 |
| SEC-17 | **执行预算（墙钟 + 输出上限 + 资源提示）** | harness、codex | P1 | `agentxx_execute_command` + `nodes/toolcall.cpp` | 为命令/工具引入"墙钟超时 + 输出字节上限 +（可选）CPU/内存提示"。与 TOOL-7 的统一超时是同一条；命令超时用 `CancelRegistry` kill 进程组（已有），但要把"超时"返回为结构化结果而不是异常 |
| SEC-18 | **沙箱/执行后端抽象（可选落地）** | codex、harness、dsh、opencode | P2 | `execute_command` 插件的执行路径 + 新增 `sandbox/` 适配目录 | Linux 优先接 bubblewrap、macOS 接 Seatbelt；接口设计成"**若可用则收窄，不可用则记录原因并继续**"，不让平台差异进入业务逻辑。与 CTX-10 的"执行世界"是同一层 |
| SEC-19 | **权限动作通用化** | opencode、dsh | P2 | `permission.h` 的 `XXRouter` 扩容 | 现在规则维度固定为读/写路径，非文件系统动作（"允许访问某 MCP 工具""允许终止进程"）难表达。可引入通用 `action × target` 维度（保留现有路径语义作为其中一类） |
| SEC-20 | **可复用假实现（测试替身）** | harness、pi、opencode | P2 | `agent/test/include/agentxx-test/core/` | `FakeSession` / `FakePermissionPrompter` / `FakeProvider` 统一现有手写的应答器与桩，减少各模块重复搭桩 |

### 9.2 本项目已有优势（保留）

- 规则表达力强：读/写两套规则表 + 最长路径前缀 + 通配符 + 文本目标（命令/URL），`permission.mode` 给出未命中规则的明确语义。
- **`check_paths` 三态批量复核解决了一个真实绕过**：模式/前缀参数工具（glob/grep/list）先枚举实际路径再逐项判定并丢弃被拒项，`**` 无法绕过子目录拒绝规则；"无法规范化时按 Ask 而不是按已批准"是错误的正确方向。fi/opencode 都没有对等能力。
- 判定顺序合理且**隔离优先**：`allowPath` 先于 `denyWritePath`（真实 worktree 就在主检出的 `.agentxx/agent/worktrees/` 下，顺序写反会导致绑定 worktree 后写不了任何文件）；配置显式拒绝**先于**完全授权（黑名单不被全权授权绕过）。
- 插件声明式权限 + 声明随工具注销/插件禁用自动撤销，宿主不硬编码任何工具名。
- 询问不设超时（用户可能长时间不响应）、无会话总线时默认拒绝。
- 完全授权状态经总线广播给所有客户端，多端一致。

---

## 10. 子代理、后台任务与并行

**现状要点**：子代理 = 宿主派生的**独立 agent**（独立 `AgentContext`/engine/`SessionStore`/中间件栈）；中断即委派（`NodeInterrupt` → `service.subagent` → `spawnBatch` → resume 回填）；深度预算 `maxDepth` + 并发上限 `maxConcurrentSubagents` + 取消级联 + 工具策略三态 + 同上下文模式（命中 KV 缓存）；**没有**任务记录、后台委派、作业表、定时任务。

### 10.1 建议吸收的设计

| 编号 | 设计 | 来源 | 优先级 | 落地位置 | 说明 |
|---|---|---|---|---|---|
| SUB-1 | **持久化后台作业表 + 调度循环 + 超时预算** | harness、dsh、opencode、codex、pi | **P0** | 新增 `agent/lib/{include/agentxx/job,src/job}/` + `nodes/toolcall.cpp` + `tools/subagent.cpp` + `agentxx_execute_command` | 六篇里五篇提到，是当前最明显的功能缺口。作业表字段（harness 精简版）：`uid/type/created/updated/state/scheduled/deadline/run_by/progress/result/error/attempt/max_attempts/group_id/data`；提供的操作：`create / list_ready / count_running / update_execution / update_progress / list_by_group`。要点：① **先落库再执行**（保证崩溃后 `run_by`/`deadline` 留痕，配合超期回收）；② 启动时扫描 `state=running 且 deadline 已过` 的作业做**超期回收**（标失败 + 通知用户）；③ 容量控制（`max_running` + `count_running`）；④ 回写用"脱离取消的上下文"（作业执行完必须更新库，即使作业 ctx 已取消或正在关机）。适用对象：长命令、子代理批次、上下文压缩、插件声明的后台工作 |
| SUB-2 | **作业进度 + 卡点定位** | harness | **P0** | 与 SUB-1 同批：`progress` 事件 + 结果里的 `detail{kind, ref}` | 超时/失败不能只给一句话：要把"**卡在哪一步**"写进结果与日志（哪个工具/哪条命令/哪个子代理；对齐 harness 合并队列把"卡在哪个检查"写进日志字段的做法）。同时 `detail` 允许携带可跳转的定位（文件路径#行号 / share_store id / 作业 id），UI 据此着色、折叠、跳转 |
| SUB-3 | **子代理运行记录与可查询性** | opencode、pi、dsh、openclaw | **P0** | `AgentHost::spawnOneTask` + `SessionStore` 新表（或作业表）+ `Topic::SubagentStatus` | 为每次委派写一条记录（`taskId`、父/子 sessionId、名称、状态、起止时间、用量、结果摘要），供 UI/插件查询与审计；进程重启后至少能显示"上次有未完成的委派"。现在委派是纯运行时对象（不落盘、不可查） |
| SUB-4 | **后台委派（detached）** | openclaw、dsh、opencode、codex | P1 | `SubAgentManagerTool` 增加 `detach: true` + `AgentHost` + 会话事件 | 现在所有委派都是前台（父轮次在中断处等待），长任务会长时间占用父会话。做法：创建子代理后立即返回 `{taskId}`（不阻塞父轮次），完成时经事件总线通知并在父会话插一条结果提示。可参照 openclaw：**前台 = 工具置为"finishing after=[S]"，后台 = 立即结算工具并返回 taskId** |
| SUB-5 | **生产配额与生命周期守卫（RAII）** | codex、openclaw | P1 | `tools/subagent.cpp` + `middlewares/subagent_manager.cpp` + `AgentHost` | 现状已有深度与并发上限，补：**用 RAII 守卫保证异常/取消路径也释放名额**；"等待后台工作的调用方不占槽位"（openclaw 的这条规则避免了调度器阻塞自己等待的子任务这种死锁型设计错误）；后台工作独立小预算（如 3 个槽位），不与前台抢 |
| SUB-6 | **车道化并发预算** | openclaw、opencode、pi | P2 | `AgentConfig`（`maxConcurrentTurns` 全局 / `maxConcurrentSubagents` 每会话）+ 后台类任务独立预算 | 现状是"有多少会话就并行多少"，长上下文 + 多会话下容易把内存与上游限流推满。并发满时排队而不是拒绝；后台类任务（压缩、检索、标题生成）走独立小预算 |
| SUB-7 | **依赖/汇聚语义** | pi、dsh | P2 | 中断参数增加 `join: "all" | "any"` + `AgentHost` | 现在"等全部子代理完成"只能靠工具里自己并发等待。轻量方案：父轮次等所有子任务**终态**（不区分成功/失败 —— pi 的"依赖语义是终态而非成功"值得抄），再继续；任一失败不阻断其余 |
| SUB-8 | **取消前的 join 顺序** | pi、dsh | P1 | `AgentHost` 取消路径 + 测试 | 明确并测试："父取消 → 先等子代理当前执行返回（或标记后 join）→ 再调 abort"，避免效果与取消并发写。现在已拒绝"取消后自动恢复"，可在此基础上把顺序写进文档与测试 |
| SUB-9 | **子代理默认收窄工具面 + fork 父上下文** | openclaw、codex、dsh | P1 | `makeSubagentConfig` + `SubagentBatchItem` 增加 `contextMode` | ① 工具策略缺省从"默认全量"改为**不给**会话/消息/子代理类工具（避免递归委派与跨会话写），需要时显式开；② 补 `fork` 上下文模式：由宿主自动取父会话当前上下文（可按 token 预算裁剪 + 去掉工具结果细节）作为子代理初始上下文 —— 现有 `messages`/`sessionId` 是"同上下文"的全量透传，调用方要自己准备 |
| SUB-10 | **「完成 ≠ 目标达成」的显式提示** | openclaw、pi | P2 | 子代理工具的描述文本 + 结果前缀 | 写明"子代理完成仅表示这一次委派结束，不代表用户目标完成；若范围内仍有工作，应继续推进"。成本极低，能减少"父模型看到子代理返回就把任务标记完成"的误判 |
| SUB-11 | **定时/常驻任务（轻量）** | openclaw、pi、dsh | P2 | 新插件（如 `agentxx_scheduler`）+ 任务表 | 最小形态：一张任务表（时间/周期/提示词/目标会话）+ 基于 `steady_timer` 的调度协程，到点后在目标会话里跑一轮；掉电重启后从表恢复，并**有界补偿**（不补偿多于一次，避免"重启即疯狂补跑"）。授权层要显式声明「范围/触发器/审批门/升级规则」四要素，否则后台自主执行不可信 |
| SUB-12 | **跨会话观察者（watch）** | openclaw、dsh | P2 | `HostBus` 扩展事件 + 与 STO-3 的事件序列表配套 | 父会话可注册"关注某子会话的关键变化"，变化时收到一条合并通知而不是轮询。可直接照抄三条规则：**同一 watcher/target 只发一条通知**、**游标冻结水位**、**自身造成的事件不通知自己**、重启后由持久游标重新物化 |
| SUB-13 | **后台整理通道（轻量版做梦）** | openclaw、dsh | P2 | 可选的"会话结束/空闲整理"任务，走独立小预算 | 会话空闲 N 分钟后（或用户显式触发），用一个无工具的子代理读本轮 transcript，产出"值得长期保留的事实"候选，写入**检索类**记忆文件（不直接改常驻类），并保留原始出处；产出可人工复核。约束：不占回复路径、失败不影响回复 |

### 10.2 本项目已有优势（保留）

- 子代理即**独立 agent 实例**：与主 agent 完全平等、不共享可变状态、结束即整体析构（会话/中间件状态随 `AgentContext` 释放，无按 thread 累积泄漏）。
- 委派只有一条路径：模型工具与内部路径（压缩子代理）都走 `service.subagent.execute` 请求-响应，嵌套与根委派同路径（扁平化），不存在两套语义漂移。
- 深度与并发预算显式（`maxDepth` / `maxConcurrentSubagents`）+ 取消级联 + 工具集合/worktree 绑定继承自根 agent；父被取消后不再派生新任务。
- 同上下文模式（`messages` + `sessionId` + 强制父会话当前模型）是为压缩场景专门设计的 KV 缓存优化。
- 结果回填 key 规则（`makeSubagentResumeKey`）由写入侧与读取侧共用同一函数，从机制上消除规则漂移。
- 批量委派真并发（每任务一个 `co_spawn` + channel 收集），比 pi 主路径"靠扩展 spawn 子进程"更省资源。

---

## 11. 客户端 UI 与渲染分层

**现状要点**：FTXUI TUI（唯一渲染实现）+ 声明式组件描述（`agentxx.ui.item`，数据层在 `lib`）+ 能力协商 + 表单状态宿主维护（`UiFormState`）+ 命中区（`UiHitRegion`）+ `OwnedReflect` 绑定 Box 所有权 + `LazyScrollable` 三档缓存预算；客户端插件 9 张表。

### 11.1 建议吸收的设计

| 编号 | 设计 | 来源 | 优先级 | 落地位置 | 说明 |
|---|---|---|---|---|---|
| UI-1 | **渲染与命中的快照回归测试** | codex、openclaw、harness、opencode、pi | **P0** | `agent/test/client/` 新增模块 | 对代表性组件（表格/树/差异/Markdown/表单/中断卡片）在**固定尺寸**下生成"纯文本渲染结果"与"命中区列表"作为快照断言；改动渲染时失败并要求更新快照。这是六篇里出现次数最多的测试类建议（codex 的 1324 张 insta 快照、openclaw 的视觉门禁、harness 的渲染快照）。agentxx 已有 `tui_surface` 基础，补"基准文本比对 + 一键更新"即可 |
| UI-2 | **原子刷新（CSI 2026 同步输出）** | pi | P1 | `agent_tui.cpp` 的帧提交处（`Screen::ToString` 之后） | 每帧写入前后包裹 `\x1b[?2026h` / `\x1b[?2026l`（终端不支持时无副作用），消除流式长输出时的闪烁与撕裂。成本低、收益直接 |
| UI-3 | **IME 硬件光标定位** | pi | P1 | `components/input_bar.cpp` + `agent_tui.cpp` 帧循环 | 输入栏渲染时把**硬件光标**移到插入点（FTXUI 已有 `cursor_position`/`Cursor` 支持），使 CJK 输入法候选框出现在正确位置。对中文用户影响最直接 |
| UI-4 | **统一浮层管理器** | pi | P1 | 新增 `framework/overlay_manager.h` + 逐步迁移 `overlays.cpp` | 现在每个 overlay 各自实现尺寸/定位。抽出通用能力：锚点（9 种）+ 百分比定位 + `margin` + `min/max` 尺寸 + 响应式 `visible(w,h)` + 焦点回退策略（不可见即释放焦点）+ 层级 + `hide` 与 `setHidden` 区分。新面板（含插件 overlay）自动获得一致的定位与焦点行为 |
| UI-5 | **渲染层边界检查** | opencode、pi | P1 | 新增 `agent/test/client/test_tui_boundary.h` | 断言"渲染层（`ui_components.*`、组件解析、命中）不依赖网络/会话/插件管理器"，保证它可独立测试与复用（对应 opencode 的"TUI 只依赖 SDK"）。这层一旦被网络/会话依赖渗透，将来 GUI 复用就无从谈起 |
| UI-6 | **未知内容宽容性条款化 + 测试** | opencode、pi | P1 | `docs/zh-cn/design/tui.md` + `agent/test/core/test_ui_items.h` | 把"未知组件类型 → 纯文本降级、未知字段忽略、缺能力 → 降级"写成明文条款，并加一条测试：给出"来自未来版本"的组件树（含未知类型与字段）时渲染不崩且能给出降级文本。现在有 `plainText` 降级与解析容错，但缺成文条款与对应测试 |
| UI-7 | **markdown 解析/测量的 offload 与缓存** | opencode、pi | P1 | `agent/client/src/io/tui/markdown_*` | 长回答或超大代码块时把"折行 + 行数统计 + 高亮"放到工作线程或分片处理，界面线程只消费结果；`wrap_line_by_width` 与 `estimateMarkdownLines` 的口径必须保持一致（同一函数，改动折行规则时两处同步） |
| UI-8 | **虚拟终端测试** | pi | P1 | `agent/test/client` 新模块 | 把渲染输出喂给终端模拟器并断言**最终屏幕内容**，替代/补充当前"逐组件断言行模型"的方式，能抓到跨组件、跨滚动、跨浮层的组合问题（pi 用 `@xterm/headless` 做 `VirtualTerminal`；agentxx 可先做轻量 VT 解析） |
| UI-9 | **可更新的结构化进度展示位** | openclaw | P2 | `agentxx.client.panel` / `status_item` 扩展 | 给"长任务的可见进度"一个宿主维护、可原地更新的展示位（对应 openclaw 的 `progress_card`），而不是让模型每次重述。与 SUB-1/SUB-2 的作业进度配套 |
| UI-10 | **UI 槽位 + 插件贡献清单** | dsh、opencode、openclaw | P2 | `agentxx.client.ui` 表扩展 + 设置弹窗 | ① 具名 slot（"在消息卡片尾部插入一个操作行""在侧栏插入一段指标"）让插件不必替换整个面板；② 把"插件注册的工具/命令/面板/定时器/快捷键"纳入统一的**插件贡献清单**（`list_plugins` 扩展字段），便于用户审计（快捷键列表已有归属显示，可推广） |
| UI-11 | **能力协商补"体验级别"声明** | openclaw、dsh | P2 | `agentxx.client.*` 能力段 | 现有能力段是"支持哪些块/控件"。补两项：① 表单提交能力（是否支持一次提交多字段、是否支持取消）；② 布局容器的实际可用尺寸（终端行数是否无界）。让插件按"体验"而不是"有没有"来组织内容 |
| UI-12 | **终端能力协商** | pi | P2 | `agent_tui.cpp` 启动探测 + `ui_components.h` 能力段 | 探测并缓存终端能力（truecolor/256 色、Kitty 键盘协议、图形协议、OSC 52 剪贴板、`?2026` 支持），在能力段里如实上报（"终端能力 + 组件能力"两段） |
| UI-13 | **界面文案字典门禁** | dsh、openclaw | P2 | `tui_i18n.cpp` + 测试 | agentxx 的 TUI 文案已集中在 `tui_i18n.cpp`；加一条测试：界面代码中不出现直接面向用户的硬编码字符串（至少对新增文件生效），避免语言支持退化 |
| UI-14 | **搜索面板与语义跳转** | pi | P2 | `MessageListComponent` | 长会话中"在消息列表里搜索"与"跳到上一次用户输入"是高频需求（pi 有搜索面板与 OSC 133 提示符跳转）。命中高亮 + 上下跳转 + 快捷键即可 |

### 11.2 本项目已有优势（保留）

- **「描述层 → 适配 → 唯一渲染实现」是跨端一致性的最优解**：同一份插件声明在终端与将来的 GUI 上表现一致，降级规则集中在适配层，渲染层不需要知道"谁不支持什么"（六篇都确认这个方向比"各端各写一遍"更省事）。
- 能力协商与降级用**同一份数据**（`tuiUiCapabilities()` 与 `adaptItems`），不会出现"声明支持但画不出来"；约定"少声明只会降级，多声明会画不出来"。
- 尺寸抽象干净：一个单位 `u` + 终端 `cell` 换算（`{"percent": n}` 基准为直接父容器，`"auto"`，不允许负值）。
- **表单状态单一归属**：控件状态由宿主维护（`UiFormState`），点击/键盘/校验/取值只有一份实现，插件只交声明；中断与插件表单共用同一套渲染与交互。
- `OwnedReflect` 把 FTXUI 的 Box 所有权附着到元素上（移动/缓存元素即带走 Box），解决了"缓存渲染结果与引用外部生命周期冲突"这一类 use-after-free。
- `LazyScrollable` 的三档预算（条数 / 字节 / 短条目豁免）+ 锚点模型，是把"GC 语言里免费得到的东西"逐项手工实现的成果，且每项都有明确不变量与失败案例记录。
- 键位冲突可解释（`ClientKeybindConflict` 记录"谁抢到了、谁没抢到"并在设置里展示）；定时器 `pause_when_hidden` 与动画等级 Disabled 拒绝注册这类界面语境取舍，服务端项目不会有。

---

## 12. 远程协议、SDK 与嵌入

**现状要点**：手写 JSON wire（30+ 消息，头文件逐条带注释）+ `AgentIOBase` 端点 + Channel/WS 传输 + delta `seq` 重放（内存缓冲）+ 历史分页 + MCP/ACP/A2A + FFI C API（26 符号）；**没有**协议版本、幂等键、schema 机器可读定义、请求级取消、无界面/stdio 集成形态、SDK。

### 12.1 建议吸收的设计

| 编号 | 设计 | 来源 | 优先级 | 落地位置 | 说明 |
|---|---|---|---|---|---|
| PRO-1 | **副作用消息的幂等键** | openclaw、opencode、harness、pi | **P0** | `wire_protocol.h`（`user_input` / `interrupt_and_run_next` / `compact_context` 加可选 `idempotencyKey`）+ `SessionServerAgentIO` 小型去重表（TTL 几分钟） | 远程模式下网络抖动会导致重发，现在会重复执行。opencode 的语义值得抄：**同 id 同内容视为精确重试、同 id 不同内容报冲突**（返回明确错误码） |
| PRO-2 | **协议版本与能力协商字段** | pi、opencode、openclaw | P1 | `hello` / `hello_ack` | `hello_ack` 已有模型能力与设备信息，但**没有协议版本**。补 `protocolVersion`（整数）+ 客户端在 `hello` 里声明自己的版本与可选能力列表；服务端据此决定是否发送新消息类型（老客户端忽略未知类型，但主动降级更稳）。不匹配返回明确错误，而不是靠"字段可选性"兼容 |
| PRO-3 | **协议单一定义 → 生成编解码与文档** | opencode、dsh、harness、codex | P1 | 新增 `agent/lib/protocol/wire_schema.{yaml,json}` + 构建期生成脚本（产物提交并加一致性校验） | 现在每个消息手写 `toJson/fromJson` 与分派，字段增删容易漏。用一份机器可读描述生成：C++ 编解码与结构体、协议字段文档表、（可选）TS 绑定。同时加一条"每个消息类型都有 toJson/fromJson 往返测试"的用例（现在测试里没有 wire 往返用例） |
| PRO-4 | **消息往返测试 + 生成式字段文档** | codex、harness、pi、opencode | **P0** | `agent/test/client` 新增 `wire_protocol` 模块 | 门槛最低的一步：为 36 类消息各写一条"序列化 → 反序列化 → 断言字段"的用例；协议文档可由同一份描述生成。这是 PRO-3 的前置，也可以独立先做 |
| PRO-5 | **不透明游标规范** | opencode、harness | P1 | `wire_protocol.h` 的会话列表 / 历史分页段 | 现在历史分页用绝对下标（`beforeIndex`），一旦允许删除或分支就失效；会话列表用 keyset 游标（已较好）。统一成"只由创建者解释"的不透明游标，并写明"用另一个会话的游标无效" |
| PRO-6 | **连接阶段机与可区分错误** | pi、opencode | P1 | `ws_io_transport.h` + `session_server_agent_io.cpp` 的 Hello 流程 | 把「未握手 → 未绑定会话 → 就绪 → 重连中」作为显式状态机；对"请求了不存在的会话/消息"给出可区分错误码（opencode 的 `SessionNotFound` / `MessageNotFound` 分得很清）。同时保留现有的顺序不变量：**先 `HelloAck` 再重放**（否则客户端会按"未握手"丢弃全部重放消息） |
| PRO-7 | **会话栅栏（attachmentId）** | pi | P1 | `SessionServerAgentIO` 的绑定结构 + wire 字段 | `switch_session` 重绑存在"旧界面操作新会话"的风险窗口。每次会话绑定一个随机 `attachmentId`，会话级请求必须携带，服务端不匹配即拒绝（缺失时按当前会话处理以保持兼容） |
| PRO-8 | **请求级取消** | pi、harness、opencode | P1 | `wire_protocol.h`（`cancel` 带 `id`）+ 服务端请求表（按 id 记 cancel token） | 现在 `cancel` 只取消当前轮次；`list_dir` / `get_context` / `get_view_messages` 这类长请求没有取消手段，会长期占用连接 |
| PRO-9 | **无界面 / 子进程集成形态（stdio JSONL）** | pi、codex、opencode | P1 | `agent/client` 新增 `rpc` 模式（复用现有 wire 消息，只换分帧） | 使 IDE 插件等集成方无需 WS 也能驱动。要点照抄 pi：严格 JSONL（一行一个完整 JSON，注意不要用会按 `U+2028/U+2029` 断行的读取器）、stdout 只放协议记录、诊断走 stderr、双方处理背压；`user_input` 响应只表示"已受理"，完成要等轮次结束事件（**响应 ≠ 完成**，与 LOOP-2 配套） |
| PRO-10 | **独立服务模式 + 控制通道** | codex、pi、harness | P1 | `agent/client/main.cpp`（启动模式）+ `SessionServerAgentIO`（连接注册与重放已具备）+ 新增 transport | 把 `agentxx_cli server` 做成可后台常驻：PID/锁文件、控制 socket（列会话/连接/健康检查）、可选 TLS。允许第二个客户端进程 attach 已有会话。缺的是生命周期管理，不是会话能力 |
| PRO-11 | **重放缓冲的观察者计数与按需释放** | openclaw | P2 | `SessionServerAgentIO` 的 `deltaBuffer_` | 现在是固定容量 FIFO，不看有没有人在消费。两点：① 无客户端在线时缩短保留（减少内存）；② 客户端长期不消费时主动降级为"下次全量 sync"，而不是无限追加 |
| PRO-12 | **沿用并文档化「重放 vs 刷新」取舍** | openclaw、opencode | P2 | `docs/zh-cn/design/tui.md` / `index.md` | 把前提写清：环形缓冲只在**单进程生命周期内**有效，进程重启后客户端必须走全量 sync；避免客户端误以为 `seq` 跨重启连续（若 STO-3/STO-4 落地，可升级为持久事件序列） |
| PRO-13 | **真外对端端到端测试** | harness、opencode、pi | P1 | `agent/test/core/test_mcp.cpp` 扩展 | 为 MCP/ACP/A2A 增加"起一个最小真实对端"的端到端测试（例如用 `cxx_utilxx` 的 http/ws server 起桩 MCP server，或用参考实现作可选依赖 + 环境变量开关），避免只测模拟响应 |
| PRO-14 | **开放 SDK（可选）** | pi、opencode、codex | P2 | 新 `sdk/`（先做一种语言的轻量绑定）+ 生成（依赖 PRO-3） | 若要把 agentxx 作为"本地 agent 服务"开放给第三方（编辑器插件、脚本），提供一份生成的轻量 SDK，内嵌模式与远程模式共用同一 API 表面（FFI 可作为"同进程 SDK"的另一形态） |

### 12.2 本项目已有优势（保留）

- **「同进程与远程只换 transport」**：会话语义、重放、分页、队列镜像、权限同步都在协议层，不因部署形态变化；进程内 `ChannelAgentIOTransport` 零序列化。
- 重连做得好：`seq` + 环形缓冲 + 环比不可用即回退全量（而不是尾窗）+ 首接入尾窗 + 按需分页；比 openclaw 的"事件不重放"和 opencode 的"不自动重连"对终端用户更省心。
- 顺序敏感的坑被显式处理并写在注释里（先 HelloAck 再重放、缓冲溢出回退全量、切同一会话只补尾窗 Sync、`seq == 0` 的 delta 不进重放缓冲）。
- 中断等待可被 `WireCancel` 立即打断（`bind_cancellation_slot`），而不是只能等超时。
- 对外集成面宽：MCP client + server、ACP server、A2A server/client、FFI C API（26 符号、每实例双线程 + 通道，**agent 核心零改动**）。
- 增量序号统一分配（所有新产出都经 `Session::nextDeltaSeq()`），重放缓冲因此始终单调可靠；`TurnEnd` 携带 `historyCount` + `tailHash` 供客户端校验历史一致性（六篇里只有 dsh/pi 有类似机制）。

---

## 13. 扩展机制（插件框架）

**现状要点**：纯 C ABI 动态库插件，四层结构（ABI 契约 / 宿主骨架 / 运行时 / SDK）+ 宿主领域层；生命周期是事务（`create` 只构造、`start` 是注册事务可回滚、`stop` 可重复、`destroy` 只释放本地对象）+ 五态状态机 + 执行 lease + tombstone + 级联依赖 + 接口协商三层 + 多实例三铁律；实验实测表数量 20/10（文档记 19/9，已漂移）。

### 13.1 建议吸收的设计

| 编号 | 设计 | 来源 | 优先级 | 落地位置 | 说明 |
|---|---|---|---|---|---|
| PLG-1 | **声明式贡献 + 重算（替代逐项反向撤销）** | opencode、dsh | P1 | `plugin_manager_domain_hooks.cpp` + `resource_applier.cpp` + `prompt.cpp` + `builtin_tool_renderers.cpp` | 现在插件的声明类贡献要靠**逐项反向操作**撤销（提示词用备份还原、图类型用代次槽失效、中间件用标记停用），漏一处就残留。改成：注册即进入"当前活动集合"，注销即离开；派生状态（系统提示词、工具清单、UI 描述）由集合**重算**并推送。这是 opencode 最值得抄的一条 |
| PLG-2 | **细粒度变更事件（哪个域变了 → 只重算依赖方）** | opencode、dsh | P1 | `plugin_manager_lifecycle.cpp` + `events.h` | 插件启用/禁用/资源变更时发布"工具集变了 / 提示词段变了 / 模型表变了"，让 UI 与依赖方只做必要重算（现在多为整体刷新）。与 PLG-1 配套 |
| PLG-3 | **批量启停的重算合并 + 顺序位** | opencode | P1 | `plugin_manager_lifecycle.cpp` / `plugin_manager_adapters.cpp` | 两条：① 批量启用/禁用时把"派生重算"推迟到一批结束后执行一次（对应 `State.batch`）；② 同 id 重载**保留原顺序位**（现在重复加载走"先停再起"，顺序由加载顺序决定）。收益：减少重复计算，且插件的相对顺序稳定可预期 |
| PLG-4 | **单槽位（slot）替换语义** | openclaw、opencode | P1 | `agentxx.agent.*` 表扩展 + `PluginHostLifecycle` | 现在的模型是"能力叠加"，适合工具，不适合"换掉压缩策略 / 换掉记忆实现 / 换掉上下文装配"这类独占扩展点。为少数**独占型**扩展点定义槽位配置（`slots.compressor` / `slots.memory` / `slots.context_assembler`），**卸载时自动重置回内置实现**（agentxx 已有"卸载按备份恢复"机制可复用） |
| PLG-5 | **清单能力对齐：不执行插件即可校验配置 + 静态能力快照** | openclaw、harness | P1 | `plugin.yaml`（`pluginxx/host/manifest.h`）+ 宿主校验路径 | 现状 `plugin.yaml` 主要声明接口与平台支持。补三类元数据：① **配置 schema**（宿主在加载前校验插件配置并给出可读错误，而不是等 `start` 失败）；② **配置 UI 提示**（设置面板里的分组/说明/默认值）；③ **静态能力快照**（声明自己提供哪些能力，供发现/清单展示，不需要实例化）。第 ① 条收益最大 |
| PLG-6 | **依赖声明（provides/requires）与装配顺序** | dsh、pi | P2 | manifest 扩展字段 + `plugin_manager_lifecycle.cpp` 拓扑排序 + `capabilities` 表 | agentxx 已有 `depends` / `optional_depends` + 拓扑装载 + 反向级联，比 opencode 强、与 dsh 的"服务可用性驱动"不同取舍。可补的是"**能力级**依赖查询"：插件能问"谁提供了能力 X"（`CapabilityRegistry::names()` 已有基础，补按能力反查提供者） |
| PLG-7 | **卸载后配置回落防护** | openclaw | P2 | 与 PLG-4 配套 | 引入槽位后需补一条：卸载或禁用后，指向该能力的配置**自动回落到内置实现**并记日志（不要留下悬空引用）。agentxx 已有 `onInstanceUnloaded` + 备份恢复基础 |
| PLG-8 | **教学式错误（错误信息告诉调用方怎么改）** | dsh、opencode | P1 | 插件加载/接口缺失/配置非法的错误文本 + 宿主日志 | dsh 的做法值得抄：解析失败给出源码行与插入符 + "去掉 TS 类型注解""注意是 async 函数体"这类提示。对"模型会读日志并自我修正"的场景收益最高。agentxx 现在的失败信息多是状态码 + 简短原因 |
| PLG-9 | **只读「域视图」查询（可列举的贡献清单）** | opencode、dsh、harness | P2 | `agentxx.agent.core` 表扩展（已有 `getPluginJson` 可在其上扩展） | 输出当前：提示词段、工具清单、图节点类型、能力、事件订阅、依赖、配置。既是插件开发与排障的入口，也是 ARC-5 生效装配快照的数据源 |
| PLG-10 | **插件错误隔离的展示面** | opencode、dsh | P2 | `client_plugin_manager.cpp` 的 UI 注册表 + `builtin_tool_renderers.cpp` | 按「插件 / 槽位 / 阶段」记录渲染错误并只影响该处显示（现在是插件渲染抛错后回退，缺少可查询的错误记录） |
| PLG-11 | **插件元数据集中管理 + 参数表单可视化** | harness、openclaw | P2 | 全局库缓存 + TUI 设置页（可直接复用声明式组件层的 `control`/`submit`） | 把 `plugin.yaml` 的解析结果 + 插件导出的 `get_info()` 元信息缓存，由设置页按 `configSchema` 渲染可编辑表单。与 PLG-5 的配置 schema 是同一件事的两侧 |
| PLG-12 | **高权限插件显式许可（加载策略）** | harness、openclaw | P2 | 配置段 + 加载前检查 | 对"声明需要 `ProcessExec` / `DeviceCapture`"的插件，要求配置里显式列出才允许加载（把"权限声明"与"加载许可"分开）。对应 harness 的 `Privileged` 白名单 |
| PLG-13 | **插件成本剖析工具** | openclaw | P2 | `agent/benchmark`（插件逐项边际内存已存在） | 补一个"只加载某个插件、测启动耗时与常驻内存"的独立脚本，作为插件评审的固定证据（新增内置插件时跑一次） |
| PLG-14 | **文档分页化 + 快速上手** | openclaw、pi、dsh | P1 | 把 `docs/zh-cn/design/plugins.md`（100 KB 单文件）拆成子目录 | 建议拆成：`getting-started`（最小插件 + 导出宏 + 一次完整示例）、`lifecycle`（五态与事务语义）、`kit`（SDK 与表清单）、`host`（宿主实现与领域表）、`client`（client 侧与 UI）、`rules`（三铁律、导出控制、平台 gate、权限声明）。与 ARC-2 配合 |
| PLG-15 | **文档同步：表数量与新表** | opencode、dsh | P2 | `AGENTS.md` + `docs/zh-cn/design/plugins.md` | 实测 agent 侧 20 张、client 侧 10 张，文档记 19/9（差异应是新增 `agentxx.agent.context` 等表后未同步）。把"清单"变成可断言的常量表即可根治（见 TST-7） |
| PLG-16 | **按名字替换服务实现的装配接缝** | opencode | P2 | `deps/injector.h` + `base_agent.cpp` 装配段 | 给宿主装配加一层「服务名 → 替代实现」接缝（opencode 用 `LayerNode.replacements` 在类型层做同样的事），便于测试替身、嵌入宿主与多宿主变体 |

### 13.2 本项目已有优势（保留）

- **最小 ABI 做得非常克制**：跨边界只有四件事（字符串视图 / 宿主堆字符串 / `alloc·free` 唯一内存通道 / COM 风格接口查询），每张表自带 `version` + `struct_size` 自校验，能力演进不动全局版本号；入口符号名由宿主提供，内核零宿主专名（同一内核可服务多个宿主）。
- **跨卸载的指针安全有真正的解法**：tombstone 宿主控制块（永不释放、关闭只清空实例引用）+ 执行 lease（覆盖"已进入插件代码"与"已排队未执行"）+ inflight 归零等待 —— 插件把 host 指针存进线程/延迟任务、卸载后再调用只会**安全失败**。
- **禁用/启用把"用户意图"与"依赖状态"分开**：`userDisabled` 与 `blockedByDependencies` 分别记录；级联禁用不覆盖用户选择，级联恢复不会把用户禁用的插件拉起来；注册由插件自己的 `start` 事务**重新声明**，回滚顺序固定为"先撤注册、再释放资源"。
- 卸载有超时与重试语义（安全关闭 / 延迟关闭 / `CloseFailed` 待重试三态分明，`shutdownAll` 不清空实例表从而保留可重试入口）。
- 接口协商"跳过而非报错"：`require` 未满足按 INFO 跳过（同一插件目录服务多种宿主是预期情况），只有"声明了本侧接口却缺入口符号"才报错；细粒度能力名（`agentxx.client.components` / `.form` / `.layout` …）让插件只声明真正用到的子能力。
- 内置与动态插件同构；`plugin.yaml` 自描述（name/entry/depends/optional_depends/resources/interfaces）；平台支持矩阵按插件声明门禁。
- 验证体系是新项目里少见的完整：负面编译测试（故意写错的插件必须编译失败 + 正控）、C17 ABI 校验、真实 DSO 双端、多实例、终态协议、泄漏检测。

---

## 14. 测试与质量门禁

**现状要点**：自研 `agentxx_test`（模块化、可筛选、`--fail-fast`）+ `agentxx_benchmark` 资源基准（分模块 smaps、基线 JSON 对比、真实 TUI/两进程/PTY 场景）+ Debug 默认 ASan+UBSan + 插件定向探针；**没有**：覆盖率统计、录制回放、一致性框架、渲染快照、CI 门禁脚本、测试成本预算、输出脱敏、边界检查。

### 14.1 建议吸收的设计

| 编号 | 设计 | 来源 | 优先级 | 落地位置 | 说明 |
|---|---|---|---|---|---|
| TST-1 | **CI 门禁脚本（把已有检查串成一条命令）** | harness、openclaw、opencode | **P0** | 新增 `agent/script/ci_check.{sh,bat}` | 串起：构建（Debug）→ `agentxx_test --fail-fast` → 负面编译目标 → 边界检查（ARC-1）→ 生成物一致性（TST-7）→ 基准阈值（TST-4）→（可选）sanitizer。任一失败即非零退出。现状是这些检查散在各处、靠人记得跑 |
| TST-2 | **一致性测试骨架（同一语义对多实现跑同一套断言）** | pi、harness | P1 | `agent/test/include/agentxx-test/core/conformance_*.h`（模板函数） | 最直接的对象：会话存储（若将来加内存/文件后端）、`share_store`、`settings_db`、工具库的后端（sqlite / 内存）。收益：换实现即回归，不需要重写测试 |
| TST-3 | **可控闸门 + 写顺序断言** | pi | P1 | 抽出 `ISessionStore` 接口 + 测试装饰器 | ① 记录每次提交的写序列（断言"先写 A 再写 B"）；② 能在指定提交处挂起（构造"提交中途到达别的操作"的交错）。这是把"碰运气跑到"变成"确定性构造"的关键手段 |
| TST-4 | **基准阈值门禁** | harness、openclaw | P1 | `agentxx_benchmark --fail-on-delta-rss=2MB` 等参数 + TST-1 | 已有 `--baseline` 做 ΔRSS/ΔPSS/Δ堆对比，缺的是"超阈值即失败"。把"资源回归"从人工观察变成门禁 |
| TST-5 | **渲染快照测试** | codex、openclaw、harness、opencode、pi | **P0** | `agent/test/client/` 新模块 | 见 UI-1（同一条，测试侧视角）：组件 → 文本快照基线（含窄终端 / CJK / 超长），改动渲染时失败并要求更新 |
| TST-6 | **队列/竞态清单化（两个顺序都测）** | pi、dsh | P1 | `docs/zh-cn/design/index.md` 新增"并发与竞态"章节 + 对应用例 | 把已知并发点写成清单并逐条要求"两个顺序都测"：取消 vs 工具结算、取消 vs 中断恢复、切会话 vs 活动轮次、插件禁用 vs 进行中调用、同步 vs 增量、落盘节流 vs 轮末权威保存、TOOL-1 并行后的批次取消 |
| TST-7 | **契约/生成物一致性检查** | openclaw、opencode、harness | P1 | 构建期脚本 + TST-1 | 至少三条：① 插件接口表常量集合 == 宿主声明集合（根治 PLG-15 的漂移）；② `kTuiBlockNames` == 渲染层实际实现的分支；③ 文档里引用的关键文件路径/符号必须存在。三条都属"文档/契约漂移"高发点 |
| TST-8 | **测试环境隔离** | pi | P1 | `agent/test/test.cpp` 启动时或脚本层 | 创建临时 `HOME` / `TMPDIR` / `XDG`，清空凭据类环境变量。现状 `dataDir` 与 `agentxx-config.yaml` 默认落在工作目录/用户目录，测试若不隔离会污染真实数据 |
| TST-9 | **测试成本预算（模块耗时 + 上限规则）** | openclaw | P1 | `agent/test/AGENTS.md` + 测试程序打印模块耗时 | ① 每个模块打印墙钟耗时与用例数；② 规则写明耗时上限（如同步模块 < 1s、异步模块 < 5s）与理由；③ 避免真实睡眠/轮询（改用可控时钟或事件驱动等待）。防止"跑一遍全量越来越慢" |
| TST-10 | **测试输出与报告脱敏** | openclaw | P1 | 测试输出路径 + `agentxx_benchmark` 报告生成 | 规则：打印真实路径时用相对路径或 `~` 归一化；配置里的 api key 一律不进断言输出；基准报告的 smaps 模块名白名单化。成本极低，避免凭据/隐私路径写进日志与报告文件 |
| TST-11 | **覆盖率统计（至少核心目录）** | dsh、harness | P2 | CMake 选项 + 脚本（LLVM source-based 或 gcov/lcov） | 不必追 100%，把"新增代码的未覆盖行"作为 review 项即可显著减少死代码与漏测 |
| TST-12 | **测试真实入口路径（构建产物级 e2e）** | dsh、harness | P2 | `agent/test` e2e 模块 + 脚本 + PTY（基准里已有 PTY 驱动脚本可复用） | 至少一组测试通过**构建后的可执行文件**运行（`agentxx_cli` + 子进程 + PTY），覆盖"源码路径下成立、产物里失败"的问题 |
| TST-13 | **「已实现 / 部分 / 未实现」单一清单** | opencode、dsh、pi | P1 | `docs/zh-cn/design/roadmap.md` | 维护状态表（含对应代码位置与验收标准），把散落的 `// TODO` 归拢引用。opencode 的 `specs/v2` 状态表、dsh 的 Notes、pi 的 `harness.md §0.9` 都在解决同一问题："哪些没做"要有单一出处 |
| TST-14 | **守卫有效性证明** | dsh、harness | P2 | 记录/PR 约定 | 对安全类改动（权限、路径规范化、插件边界）要求"引入回归 → 测试变红 → 回退"的证据，避免写出测不到东西的测试 |
| TST-15 | **测试目录二次分类** | opencode | P2 | `agent/test/include/agentxx-test/core/` | 当模块继续增长时按 `protocol/`、`session/`、`tools/`、`middleware/`、`ui/` 再分组（与 `agent/lib` 目录对应）。现在 50+ 个平铺头文件已需要靠名字猜分类 |
| TST-16 | **测试替身工厂** | harness、pi、opencode | P2 | `agent/test/include/agentxx-test/core/` | 统一 `FakeSession` / `FakePermissionPrompter` / `FakeProvider`（与 SEC-20、LLM-13 合并），减少各模块重复搭桩 |

### 14.2 本项目已有优势（保留）

- **资源基准体系是真正的差异化优势**：RSS/PSS/私有脏页/匿名/峰值/线程/fd + glibc 堆在用与碎片 + `malloc_trim` 可回收 + **smaps 模块级分解**（可执行文件/项目库/各插件/系统库/堆/匿名）+ 逻辑内存 + 分阶段增量 + CPU 用户/内核；每个场景**独立子进程**避免污染基线；输出 JSON（机器对比）+ MD（人工阅读）并支持 `--baseline`。六篇里没有任何一个项目有等价能力。
- **PTY 驱动的真实 TUI 子进程基准**覆盖了"真实界面线程开销"这个常被忽略的维度。
- 负面编译测试 + 真实 DSO 插件测试 + 多实例测试：覆盖了 harness/openclaw 完全没有的领域（ABI 约束、插件加载/回滚/多实例），是 C++/ABI 项目里的高质量实践。
- Debug 默认 ASan + UBSan + 插件框架定向探针，Release 全部关闭（含"探针需要 UBSan 运行库符号"这条依赖关系的处理）。
- 测试程序支持模块筛选与 fail-fast，单模块可独立跑；include 根唯一 + 完整路径约定避免同名头歧义。
- `executableDir()` 的跨平台实现（`/proc/self/exe` / `_NSGetExecutablePath` / `GetModuleFileNameW`）是插件测试能在任意 cwd 稳定运行的前提。

---

## 15. 可观测性、诊断与产物化

**现状要点**：统一 `XX_LOG*`（不直接 `std::cout/cerr`，避免破坏 TUI）+ `tui_log_sink.cpp` 接界面 + 基准统计开关 `AgentConfigStatic::enableBenchmark` + `WireContextStats` 推送上下文统计；**没有**：统一 trace id、审计、指标导出、健康检查、运行期诊断入口、遥测。

### 15.1 建议吸收的设计

| 编号 | 设计 | 来源 | 优先级 | 落地位置 | 说明 |
|---|---|---|---|---|---|
| OBS-1 | **统一操作标识（opId / callId）进日志上下文** | harness、dsh、codex | **P0** | `utilxx_base/log.h`（支持任务本地字段）+ `toolcall.cpp` + 插件 SDK 边界 | 现在已有 sessionId / agentName / tool_call_id / `correlationId`（事件总线请求-响应自带）/ delta seq，缺的是把它们**统一成一个 opId（每轮）与 callId（每次工具调用）并落到日志字段**。收益：一次工具调用跨 agent → 总线 → 插件 → 事件流的日志能串起来，不必人工关联 |
| OBS-2 | **运行期诊断入口** | harness、openclaw、codex | P1 | `wire_protocol.h` 新增 `get_diagnostics` + TUI 诊断弹窗 | 输出：进程 RSS / 线程数 / 打开的会话库数、事件队列积压、当前在跑的作业、最近错误摘要、缓存命中（若有）。与 ARC-5 的装配快照合并成一个"系统信息页" |
| OBS-3 | **遥测契约 + 内容边界（先定边界再实现）** | pi、dsh | P1 | 新增 `agentxx/util/telemetry.h`（或放 `cxx_utilxx_base`）+ 首批埋点 | 极小的 `TelemetryContext`（`startSpan` / `addEvent` / `setAttributes` / `setStatus`）+ no-op 实现 + 一致性测试骨架。首批 span：`turn` / `modelcall` / `tool` / `session.write` 四个，与 `enableBenchmark` 开关共用。**属性白名单**照抄 pi：只允许 id / 名称 / 计数 / 时长 / 状态 / 用量；**禁止**提示词、补全内容、工具参数与结果、文件内容、provider 载荷、headers、凭据 |
| OBS-4 | **关键路径指标** | harness、codex、pi | P2 | 与账号本/OBS-3 一起：轮次耗时、首 token 时间、工具调用数与失败数、压缩次数与耗时、上下文 token 估算 | 至少做到"能被测试与 UI 使用"，而不是先铺埋点。现有 benchmark 模块已具备采集基础设施，可复用 |
| OBS-5 | **缓存命中率可观测** | harness、opencode、dsh | P2 | TUI Info/诊断 + `WireContextStats` 同级消息 | 把 TUI 渲染缓存命中率、`share_store` 缓存命中率、会话库连接复用率、prompt 缓存读写计数写进诊断页，"卡顿/内存增长/成本上升"类问题能快速定位 |
| OBS-6 | **模块级日志开关（含插件）** | harness | P2 | 配置 `log.level` + `log.modules: {plugin.exec: debug}` | 现在日志等级是全局的。按模块/插件前缀配置级别，排障时不必全局开 debug（全局开 debug 在 TUI 里也会造成噪声） |
| OBS-7 | **技术报告/取证包导出** | pi、harness | P2 | 与 STO-16 合并 | 一键导出诊断包：会话历史 + 日志尾部 + 配置（脱敏）+ 环境 + 插件清单 + 基准摘要 |

### 15.2 本项目已有优势（保留）

- `XX_LOG` + TUI sink 避免日志破坏界面；异常分类让"取消/中断"不污染错误统计。
- 内存治理有**实测依据**（glibc `M_ARENA_MAX=1` 实测 VmSize 610MB→290MB；轮末 `malloc_trim` RSS -1.7~-2.8MB；mimalloc 默认关闭并给出 Windows/Linux 实测数据；THP 必须关的原因），不是照搬"用 mimalloc 就快"。
- `test_memgrowth`（内存增长回归）是很多项目缺失的一环。

---

## 16. 配置、模型目录与装配

**现状要点**：两层 yaml（base = `data_dir` 下；overlay = 工作目录或 `--config`）+ `.env` 两层；列表段统一 `{overwrite:{mode:merge|replace, remove:[...]}, list:[...]}`；`settings_db` 存客户端设置；旧键/旧写法**告警并忽略**（无版本、无迁移）；模型元数据手填。

### 16.1 建议吸收的设计

| 编号 | 设计 | 来源 | 优先级 | 落地位置 | 说明 |
|---|---|---|---|---|---|
| CFG-1 | **配置回填与校验层** | harness、openclaw、opencode | **P0** | 新增 `agent/lib/src/agent/config_resolve.cpp` + `config_loader.cpp` | 集中处理：`dataDir → ~/.agentxx` 回退、`workDir → cwd` 回退、会话库根目录推导、插件默认启用集合、`permission.mode` 与白/黑名单一致性、模型必填项。校验失败返回**带路径的错误**（形如 `model.list[2].apiKey: 不能为空`），由客户端启动时打印并退出非零。现状"配置写错"常常表现为运行期某个工具不可用 |
| CFG-2 | **配置版本 + 显式迁移** | openclaw、opencode、pi、harness | P1 | `config_loader.cpp` + 新增 `config_migrations.{h,cpp}` | 配置顶部支持 `config_version`；加载时若版本低于当前，依次跑迁移函数（键重命名/结构改写），**改前备份原文件**、迁移后写回；迁移函数与被废弃的键集中在一处。把"告警并忽略"升级为"迁移或明确报错"（否则用户以为生效了） |
| CFG-3 | **配置诊断清单（可查看）** | pi、harness | P1 | `config_loader.cpp` 收集 + TUI 设置弹窗新增"诊断"入口 | 加载完成后产出：未知键、类型不符、路径不存在、插件缺失、模型缺 apiKey、取值越界。现在只有日志，用户看不到 |
| CFG-4 | **默认值声明式集中** | harness | P2 | 一个 `constexpr` 默认表（或 yaml 内嵌模板） | 把 `connectTimeoutSeconds` / `readChunkTimeoutSeconds` / `maxConcurrentConnections` / `modelContenxtMaxToken` 等默认集中一处，供配置文档、`/config` 展示、测试共享（现在写在类成员初始化式里，文档与代码两处维护） |
| CFG-5 | **资源定位单一入口** | pi | P2 | `agentxx/util/paths.h` 或 `AgentConfigStatic` | 把"配置目录 / 数据目录 / 工作目录 / 插件目录 / 技能目录"的解析集中到一个模块（供 agent、client、插件共用），避免各处自行拼路径（现在 `dataDir` / `sessionStoreDirectory` / `resolvedWorkDir` / 插件 config 路径分散） |
| CFG-6 | **装配快照 + 版本号** | openclaw、pi | P2 | `ModelProviderRegistry` + `AgentConfig` 加载路径 | 配置热更新时：整体重建快照 → 递增 `configGeneration` → 替换；读取路径读快照而不是读配置对象；日志与 wire 消息带上 `configGeneration`，便于诊断"客户端看到的配置"与"服务端生效的配置"是否一致。客户端拿到的模型列表也应是一个整体快照（避免半新半旧） |
| CFG-7 | **旧配置自动迁移（写回）** | harness、openclaw | P2 | 与 CFG-2 同批 | 参照 harness 的 `~/.gitrpc → ~/.gitness` 迁移：对已废弃键（`models` → `model.list` 等）在告警之外提供一次性自动改写（写回 overlay yaml 并提示），减少用户手工改配置 |
| CFG-8 | **设置与配置的边界文档** | opencode、pi | P2 | `docs/zh-cn/design/index.md` 配置章 | 明确"yaml 配置（可版本化、可分发）"与"设置 KV（本机偏好）"的分工与优先级，避免同类项两处可配 |
| CFG-9 | **第三方依赖版本台账** | harness、pi | P2 | `agent/third_party/README.md` | 为 24 个第三方定义"版本记录 / 来源 / 补丁 / 升级注意事项"位置（对标 harness 的工具版本固定与更新流程、pi 的精确版本 + 生成式 shrinkwrap） |
| CFG-10 | **构建时间基线与 ccache/sccache 支持** | harness | P2 | 构建脚本 + 文档 | 记录并公开"从零构建"与"增量构建"耗时基线（与资源基准同思路），把"改一行等 20 分钟"这类问题量化；可选启用编译缓存 |

### 16.2 本项目已有优势（保留）

- **base/overlay 分层 + 列表段三态语义（merge / replace / remove 按身份剔除）的表达力远超环境变量方案**，本地多项目/多环境场景更实用；`data_dir` 单一入口让库侧不假设路径，便于测试、便携部署与多实例隔离。
- 同一文件只加载一次（防 `--config <data_dir>/agentxx-config.yaml` 这类自指情况）。
- 未配置 `dataDir` 时明确退化为内存并给出警告（而不是到处生成垃圾目录）。
- 设置 KV（客户端偏好）与服务端 agent 配置分离，职责清楚。

---

## 17. 整体整理

### 17.0 整理后的整体判断

把上面 16 个模块的建议放在一起看，得到四条判断：

1. **六篇的共识集中在少数几件事上**。P0 里有 7 项被**五篇以上**同时提出：工具并行（TOOL-1，六篇全部）、写者所有权（STO-1）、持久化格式版本（STO-2）、事件序列与游标重放（STO-3）、输入投递语义（LOOP-1）、作业化/后台任务（SUB-1）、渲染快照测试（UI-1）。**这些是最该先做的**；只出现一次或两次的项多为特定项目的取舍，进 P2 或不做清单更合适。
2. **agentxx 的短板是"把约定变成显式对象"，不是"缺功能"**。所以这份计划的主体不是"新增一个大系统"，而是"把已经存在的运行时事实写进数据"：轮次、投递结果、注入来源、历史替换、失败尝试、写者所有权、审批记忆、未完成工作、未实现清单。
3. **成本（token / 前缀缓存）是本项目最欠的技术债**，而且它不是一个功能，是一条纪律：前缀稳定（PRM-1）→ 变化只在尾部追加（CTX-1）→ 只在缓存有效处放断点（LLM-5）→ 溢出可恢复（LLM-2）→ 压缩保留尾部（CMP-2）→ 工具结果有聚合预算（TOOL-4）。这条链条任何一环缺失都会让前面的努力漏掉。
4. **安全上的优先级是"覆盖"而不是"强化"**。权限中间件本身已经比 pi/opencode 强，问题是最危险的工具不在闸门内（SEC-1）、没有出网闸门（SEC-7）、没有审计（SEC-8）、边界没有写出来（SEC-14）。这四件事的性价比远高于引入沙箱。

**规模**：模块表共 213 行（其中 `UI-1` 与 `TST-5` 是同一项的两个视角，去重后 **212 项**），其中 **P0 35 项 / P1 100 项 / P2 77 项**。不必全做：P0 是"现在就有损失"的部分，P1 是"收益明确但需要一点设计"，P2 是候选池（依赖前置项或属方向性投入）。建议按 [§17.4](#174-落地批次与依赖关系) 的批次推进，每批做完再评估下一批。

### 17.1 统一 P0 清单（35 项）

#### A. 主线一：可恢复性与可解释性（10 项）

| 编号 | 设计 | 来源 | 批次 | 一句话收益 |
|---|---|---|---|---|
| STO-1 | 会话目录写者所有权（内核级租约，四步认领顺序） | codex、dsh、harness、opencode、openclaw | 2 | 多进程/多实例不再互相覆盖会话数据 |
| STO-2 | 持久化格式版本 + 迁移链（含"旧程序不开新库"与迁移前备份） | dsh、harness、opencode、openclaw、pi | 2 | 表结构可安全演进；用户跨版本回退不会读坏数据 |
| STO-3 | 会话事件序列 + `after` 游标重放（只事件化关键事实） | opencode、dsh、pi、openclaw、harness | 2 | 断线重连不再依赖内存缓冲，服务重启也能续传 |
| STO-7 | 用量账本（token / cost / cache read·write，含失败与重试） | pi、opencode、harness、openclaw | 6 | 能回答"这个会话/这一轮花了多少"，可审计 |
| LOOP-1 | 输入投递语义显式化（`next-step` / `next-turn` / `collect`） | codex、dsh、openclaw、opencode、pi | 2 | 用户可在 agent 干活途中补充约束；攒批减少往返 |
| SUB-1 | 持久化作业表 + 调度循环 + 超时预算 + 超期回收 | harness、dsh、opencode、codex、pi | 5 | 长任务不再"进程一停就消失"，也不再拖住整轮 |
| SUB-2 | 作业进度 + 卡点定位（`detail{kind, ref}`） | harness | 5 | 失败/超时能说清"卡在哪一步"，可跳转 |
| SUB-3 | 子代理运行记录与可查询性 | opencode、pi、dsh、openclaw | 5 | 能看到"现在有几个子代理在跑、上次的委派做到哪" |
| OBS-1 | 统一 opId / callId 进日志上下文 | harness、dsh、codex | 1 | 一次调用链跨 agent→总线→插件→事件流的日志能串起来 |
| PRO-1 | 副作用消息幂等键（同 id 同内容=重试，不同=冲突） | openclaw、opencode、harness、pi | 2 | 网络抖动重发不会重复执行 |

#### B. 主线二：成本与上下文（10 项）

| 编号 | 设计 | 来源 | 批次 | 一句话收益 |
|---|---|---|---|---|
| PRM-1 | 提示词划显式缓存边界（stablePrefix / dynamicSuffix） | openclaw、opencode、dsh、pi | 1 | 稳定段不被易变段打断，前缀缓存可复用 |
| CTX-1 | 上下文来源化 + 基线/增量分离（变化以中间系统消息追加） | opencode、dsh、codex、openclaw | 4 | 长会话成本下降 + "这一轮比上一轮多了什么"可审计 |
| CTX-3 | 注入内容的类型标记（source + 分类） | codex、dsh、openclaw | 4 | 压缩/去重/审计/UI 折叠可按来源处理 |
| PRM-4 | 记忆分层 + 按需检索（常驻类 vs 检索类） | openclaw | 4 | 不再全量注入记忆文件，上下文预算回到可控 |
| LLM-5 | 缓存断点策略 + 缓存用量统计 | opencode、openclaw、codex | 4 | Anthropic 侧写入 1.25×/读取 0.1×，长会话多轮即回本 |
| LLM-1 | 模型空闲看门狗（静默超阈值即中止并按重试处理） | openclaw、dsh、opencode | 1 | provider 卡住不再表现为"一直等" |
| LLM-2 | provider 错误分类 + 溢出一次性压缩重试（不重放工具） | openclaw、opencode、dsh、harness | 4 | 消灭长会话里最常见的一类失败 |
| LLM-3 | 重试策略数据化（分类 + retry-after + 抖动） | opencode、pi、dsh、codex | 4 | 额度/计费类错误不再无意义退避 6 次 |
| CMP-2 | 结构化摘要模板 + 保留尾部原文窗口 | opencode、pi、openclaw | 4 | 压缩后不丢最近细节，少一轮试错 |
| TOOL-4 | 工具结果的聚合预算（单次限幅之上再加合计上限） | openclaw、harness、opencode | 5 | 五个中等输出不会一起把上下文塞满 |

#### C. 主线三：边界与信任（7 项）

| 编号 | 设计 | 来源 | 批次 | 一句话收益 |
|---|---|---|---|---|
| SEC-1 | 权限声明全覆盖（最强破坏力的工具当前没声明） | harness、codex、openclaw | 3 | 命令执行/桌面控制/联网不再绕过权限闸门 |
| SEC-2 | 判定结果结构化 + 理由（`PermissionDecision`） | harness、dsh、openclaw | 3 | 日志/询问卡片/诊断都能说清"为什么允许/拒绝" |
| SEC-3 | 审批记忆持久化 + 撤销入口 | openclaw、opencode、pi、codex | 3 | 用户不必每次重启重新批准一遍 |
| SEC-7 | 出网策略（地址分类 + connect 前判定 + 错误不泄露信息） | harness、codex、openclaw | 3 | 联网类工具不能再任意访问内网 |
| SEC-14 | 责任边界与安全模型文档化（"权限 ≠ 沙箱"） | pi、opencode、codex、openclaw | 1 | 使用者不会误以为有隔离；缺口被明确记录 |
| TOOL-20 | 执行环境加固（NO_COLOR / TERM / LC_ALL / PAGER / 会话标识） | codex | 1 | 消除"命令调分页器卡住""输出带 ANSI""编码错乱" |
| TOOL-7 | 工具调用超时预算（分发层保证，工具只声明） | harness、codex、dsh | 5 | 个别工具不再能把整轮卡死 |

#### D. 工程能力与验证（8 项）

| 编号 | 设计 | 来源 | 批次 | 一句话收益 |
|---|---|---|---|---|
| TOOL-1 | 同轮工具并行 + 结算按 `tool_call_id` 归位 | 六篇全部 | 5 | 多工具轮次延迟从"累加"变成"取最大" |
| TOOL-2 | 并发上限与分类（读并行 / 写独占 / 分类上限） | harness、dsh、openclaw | 5 | 并行不变成资源炸弹（TOOL-1 的必备配套） |
| LLM-13 | 假 provider（可脚本化，会话级端到端测试） | pi、opencode、harness | 6 | 工具循环/重试/压缩/中断/取消的测试不依赖网络与额度 |
| UI-1 | 渲染与命中的快照回归测试 | codex、openclaw、harness、opencode、pi | 6 | "改一处顺便改坏三处"能被自动抓住 |
| TST-1 | CI 门禁脚本（把已有检查串成一条命令） | harness、openclaw、opencode | 6 | 检查不再靠人记得跑 |
| PRO-4 | wire 消息往返测试 + 生成式字段文档 | codex、harness、pi、opencode | 1 | 新增消息漏实现/字段不一致能被拦住 |
| ARC-8 | "新能力不进核心骨架"的书面纪律与边界清单 | codex、pi、openclaw | 1 | 防止核心继续变胖（成本近零） |
| CFG-1 | 配置回填与校验层（带路径的错误 + 非零退出） | harness、openclaw、opencode | 6 | "配置写错"从运行期症状变成启动期错误 |

### 17.2 P1 清单（100 项，按模块列出）

| 模块 | 编号与设计 |
|---|---|
| 架构与装配（5） | ARC-1 依赖方向可执行检查；ARC-2 按目录局部规则文件；ARC-3 装配清单化 + 启动期依赖断言；ARC-4 分阶段关闭 + TaskScope；ARC-5 生效装配快照可查询 |
| 循环与投递（7） | LOOP-2 投递结果显式返回；LOOP-3 排队状态机；LOOP-4 排队项持久化；LOOP-5 采样请求抢占；LOOP-6 轮次记录持久化；LOOP-7 中断留痕结构化；LOOP-8 优雅中断超时 |
| 上下文（6） | CTX-2 来源三态（unavailable/空/有值）；CTX-4 自定义条目 + 投影器；CTX-6 只追加规则 + Debug 断言；CTX-7 上下文变更版本事件；CTX-8 历史替换的原因记录；CTX-11 文件/媒体引用替代内联 Base64 |
| 提示词/技能/记忆（4） | PRM-2 段落排序号；PRM-5 记忆条目来源标记；PRM-6 技能来源优先级 + 同名裁决；PRM-7 指令集合变更通知 |
| 持久化（9） | STO-4 durable 流与实时流分开；STO-5 完成事件边界；STO-6 启动清账；STO-8 耐久语义分级 + 屏障 API；STO-9 可见降级；STO-10 避免长耗时写入阻塞 io 线程；STO-12 设置库版本列；STO-13 大对象统一限额；STO-15 会话导出（HTML/Markdown/JSONL） |
| 工具（10） | TOOL-3 结果落上下文前守卫；TOOL-5 预览首尾保留 + 结构化定位符；TOOL-6 缓存过期感知剪枝；TOOL-8 错误分类（可改正 vs 致命）；TOOL-9 审批/询问缓存；TOOL-10 注册表治理（保留名/冲突记录/暴露等级）；TOOL-11 陈旧调用防护；TOOL-12 可用性与授权分层；TOOL-18 文件写统一串行化；TOOL-19 中断幂等 helper + 清单 |
| LLM（6） | LLM-4 重试结构化事件；LLM-6 模型能力元数据集中化 + 来源化；LLM-7 模型级回退链（轮次局部）；LLM-9 能力协商下沉 provider；LLM-10 消费端退出传播取消；LLM-14 HTTP 录制回放 |
| 压缩（5） | CMP-1 预算口径统一（ContextBudget）；CMP-3 多次压缩滚动合并；CMP-4 保留尾部 + 原文可回取；CMP-5 完成边界事件化 + 幂等键；CMP-6 压缩结果带恢复元数据 |
| 权限与安全（8） | SEC-4 审批连带处理；SEC-5 待决定请求可查询；SEC-6 符号链接真题；SEC-8 审计留痕（仅元数据）；SEC-10 作用域扩展 + 有效规则视图；SEC-11 规则自带样例校验；SEC-15 项目信任决策点；SEC-17 执行预算（墙钟 + 输出上限） |
| 子代理/后台（4） | SUB-4 后台委派（detached）；SUB-5 配额与生命周期守卫（RAII + 等待方不占槽位）；SUB-8 取消前 join 顺序；SUB-9 子代理默认收窄工具面 + fork 父上下文 |
| 客户端 UI（7） | UI-2 原子刷新（CSI 2026）；UI-3 IME 硬件光标定位；UI-4 统一浮层管理器；UI-5 渲染层边界检查；UI-6 未知内容宽容性条款化 + 测试；UI-7 markdown 解析 offload；UI-8 虚拟终端测试 |
| 协议（9） | PRO-2 协议版本与能力协商；PRO-3 协议单一定义 → 生成编解码与文档；PRO-5 不透明游标；PRO-6 连接阶段机与可区分错误；PRO-7 会话栅栏（attachmentId）；PRO-8 请求级取消；PRO-9 无界面/stdio JSONL 形态；PRO-10 独立服务模式 + 控制通道；PRO-13 真外对端端到端测试 |
| 插件（7） | PLG-1 声明式贡献 + 重算；PLG-2 细粒度变更事件；PLG-3 批量启停重算合并 + 顺序位；PLG-4 单槽位替换语义；PLG-5 清单能力对齐（配置 schema / UI 提示 / 静态能力快照）；PLG-8 教学式错误；PLG-14 插件文档分页化 |
| 测试（9） | TST-2 一致性测试骨架；TST-3 可控闸门 + 写顺序断言；TST-4 基准阈值门禁；TST-6 竞态清单化（两个顺序都测）；TST-7 契约/生成物一致性检查；TST-8 测试环境隔离；TST-9 测试成本预算；TST-10 输出与报告脱敏；TST-13 「已实现/部分/未实现」单一清单 |
| 可观测（2） | OBS-2 运行期诊断入口；OBS-3 遥测契约 + 内容边界 |
| 配置（2） | CFG-2 配置版本 + 显式迁移；CFG-3 配置诊断清单 |

### 17.3 P2 清单（77 项，候选池）

P2 的含义是"依赖前置项、或属方向性投入、或与定位存在张力需先评估"，**不是不重要**。按主题归拢：

| 主题 | 编号 |
|---|---|
| 上下文与提示词 | CTX-5 只读快照共享；CTX-9 重放重建入口统一读路径；CTX-10 会话工作上下文对象；PRM-3 提示词整体覆盖语义；PRM-8 技能正文的技能语义读取；PRM-9 按 agent 过滤可见性；PRM-10 提示词快照回归 |
| 循环与投递 | LOOP-9「已展示最终答案后不再延长本轮」；LOOP-10 步进快照；LOOP-11「无进展即错误」断言 |
| 持久化与恢复 | STO-11 本地 SQL 错误分类；STO-14 提交失败即不可继续（分级）；STO-16 失败会话取证包 |
| 工具 | TOOL-13 工具面诊断入口；TOOL-14 参数流式差分展示；TOOL-15 分段计时与来源标记；TOOL-16 结构化结果与投影分离；TOOL-17 压缩后重复调用守卫 |
| LLM | LLM-8 多鉴权档位与冷却；LLM-11 流式组装唯一实现；LLM-12 图片细节按能力归一化；LLM-15 结构化输出（强制工具式） |
| 压缩 | CMP-7 压缩质量门；CMP-8 切点对齐用户消息跨度；CMP-9 压缩前提醒写记忆；CMP-10 压缩策略可干预接口；CMP-11 确定性旧工具结果裁剪；CMP-12 手动压缩聚焦指令 |
| 安全 | SEC-9 执行身份绑定；SEC-12 规则定义与判定分离；SEC-13 拒绝分类与反馈文本；SEC-16 内容侧安全（密钥扫描）；SEC-18 沙箱/执行后端抽象；SEC-19 权限动作通用化；SEC-20 认证/权限测试替身 |
| 子代理与后台 | SUB-6 车道化并发预算；SUB-7 依赖/汇聚语义；SUB-10「完成 ≠ 目标达成」提示；SUB-11 定时/常驻任务；SUB-12 跨会话观察者；SUB-13 后台整理通道 |
| 客户端 | UI-9 可更新进度展示位；UI-10 UI 槽位 + 插件贡献清单；UI-11 能力协商补体验级别；UI-12 终端能力协商；UI-13 文案字典门禁；UI-14 搜索面板与语义跳转 |
| 协议 | PRO-11 重放缓冲观察者计数；PRO-12 重放 vs 刷新取舍文档化；PRO-14 开放 SDK |
| 插件 | PLG-6 能力级依赖查询；PLG-7 卸载后配置回落防护；PLG-9 只读域视图查询；PLG-10 插件渲染错误隔离；PLG-11 插件元数据集中管理 + 参数表单；PLG-12 高权限插件显式许可；PLG-13 插件成本剖析工具；PLG-15 表数量文档同步；PLG-16 按名字替换服务实现的接缝 |
| 测试与工程 | TST-11 覆盖率统计；TST-12 构建产物级 e2e；TST-14 守卫有效性证明；TST-15 测试目录二次分类；TST-16 测试替身工厂 |
| 可观测 | OBS-4 关键路径指标；OBS-5 缓存命中率可观测；OBS-6 模块级日志开关；OBS-7 取证包导出 |
| 配置 | CFG-4 默认值声明式集中；CFG-5 资源定位单一入口；CFG-6 装配快照 + 版本号；CFG-7 旧配置自动迁移写回；CFG-8 设置与配置边界文档；CFG-9 第三方依赖版本台账；CFG-10 构建时间基线 + 编译缓存 |
| 架构 | ARC-6 控制面/运行面分离约定；ARC-7 窄接口绑定 |

### 17.4 落地批次与依赖关系

```text
第 1 批（低成本高收益，彼此独立，可并行开工）
  PRM-1 提示词缓存边界 ──┐
  LLM-1 空闲看门狗      ├─ 都不动架构形状，每项配一条可验证测试
  TOOL-20 执行环境加固   │
  SEC-14 责任边界文档    │
  ARC-8 核心边界纪律     │
  OBS-1 opId            │
  PRO-4 wire 往返测试 ───┘

第 2 批（可恢复性底座）
  STO-1 写者所有权（四步认领顺序）
  STO-2 格式版本 + 迁移链 ──→ 后续所有 schema 改动的前提
  STO-3 事件序列 + after 游标 ──┬─→ STO-4 durable/实时流分开
                                 ├─→ STO-5 完成事件边界 ──→ CMP-5
                                 ├─→ SUB-1 作业表（进度/超期回收落库）
                                 └─→ SUB-3 子代理运行记录
  LOOP-1 投递语义 ──┬─→ LOOP-3 排队状态机
                    ├─→ LOOP-4 排队项持久化
                    └─→ LOOP-5 采样抢占（与 LOOP-1 配套）
  PRO-1 幂等键（与 LOOP-2 投递结果同批）

第 3 批（安全）
  SEC-1 权限声明全覆盖 ──→ SEC-10 作用域扩展 + 有效规则视图
  SEC-2 判定理由结构化 ──→ SEC-8 审计（仅元数据）
                        └─→ SEC-4/SEC-5 连带处理与待决查询
  SEC-3 审批记忆持久化 ──→ TOOL-9 询问缓存
  SEC-7 出网策略（独立，可先做）
  SEC-6 符号链接真题（与 SEC-14 的限制说明一起做）

第 4 批（上下文与成本，建议连成一条链）
  PRM-1 → CTX-1 来源化/基线增量 ──┬─→ CTX-2 三态语义
                                  ├─→ CTX-7 版本事件
                                  ├─→ CMP-4 压缩保留尾部 ──→ CMP-3 滚动合并
                                  └─→ LLM-5 缓存断点
  PRM-4 记忆分层 ──→ PRM-5 来源标记（与 CTX-3 注入类型标记同批）
  LLM-3 重试数据化 ──→ LLM-2 溢出恢复 ──→ CMP-2 结构化摘要 + 保留尾部
                                         └─→ CMP-1 预算口径统一

第 5 批（可靠性 + 工具）
  TOOL-7 工具超时预算 ──┬─→ TOOL-1 同轮并行 ──→ TOOL-2 并发上限
                        │                     └─→ TOOL-18 写工具串行化
                        └─→ LOOP-8 优雅中断超时
  TOOL-4 聚合预算 ──→ TOOL-6 缓存过期剪枝
                  └─→ TOOL-5 预览口径 + 结构化定位符
  SUB-1 作业表 ──→ SUB-2 卡点定位 ──→ SUB-4 后台委派 ──→ SUB-5 配额守卫

第 6 批（工程底座）
  TST-1 CI 门禁 ──┬─→ ARC-1 边界检查
                  ├─→ TST-7 契约一致性
                  ├─→ TST-4 基准阈值
                  └─→ TST-9 成本预算 + TST-10 脱敏
  LLM-13 假 provider ──→ TST-2 一致性骨架 ──→ TST-3 闸门/写顺序
  UI-1 渲染快照 ──→ UI-8 虚拟终端测试
  CFG-1 配置回填校验 ──→ CFG-2 迁移 + CFG-3 诊断清单
  STO-7 用量账本（可与第 4/5 批并行，只要有结算点就能做）
```

**跨批次的依赖与冲突注意**：

- **STO-2/STO-3 是"改动要落库"的通用前提**：任何"把状态写进数据"的项（LOOP-4、LOOP-6、SUB-1、CMP-5、SEC-3、STO-7）都建议在它们之后做，否则会各写一套临时表。
- **TOOL-1 会放大一切没做好的并发问题**：先做 TOOL-7（超时）、TOOL-2（上限）、TOOL-11（陈旧调用防护）、TOOL-18（写串行化、SEC-10 的路径判定），再开并行，顺序反了会引入难查的竞态。
- **PRM-1 与 CTX-1 不要分开做**：先分边界再改注入方式，中途状态会同时踩两个坑（既有动态段打断前缀，又有追加式消息打乱顺序）。
- **LLM-5 依赖 PRM-1/CTX-1**：缓存断点只有在"前缀稳定"成立时才有意义。
- **SEC-2 是 SEC-8/SEC-4/SEC-5 的前置**：没有结构化判定就没有可审计、可连带处理的载体。

### 17.5 不做清单（六篇反向结论汇总）

这一节的判据是"是否与 agentxx 的定位冲突（本地优先 / 可嵌入 / 单 `io_context` 无锁 / C ABI 二进制边界 / 声明式 UI）"，而不是"难不难做"。

| # | 不做的事 | 来源 | 原因 |
|---|---|---|---|
| N1 | **整库事件溯源 + 投影读模型层** | dsh、opencode、pi | "仅追加事件 + 派生历史"很强，但把"读历史"从一次数组下标变成一次投影计算，并要求所有消费方理解替换规则。agentxx 只吸收"关键事实事件化 + 游标重放"（STO-3），不引入完整投影层 |
| N2 | **第二套运行时 / 双轨迁移** | opencode、pi | opencode 现在同时维护旧单体 16.3 万行 + 新核心 6.2 万行、pi 有三代运行时并存，代价是同一能力两处实现、读代码先判断"这是哪一代"。agentxx 应保持**单一运行时 + 单一数据模型** |
| N3 | **把核心拆成多个包/库** | opencode、codex、pi | 32 个包 / 111 个 crate / 11 个包是大型团队并行开发的产物。agentxx 用"目录 + 依赖方向 + 可执行检查（ARC-1）"能达到同样效果，拆开只会增加构建与发布负担 |
| N4 | **编译期依赖注入（wire 式）** | harness | agentxx 的核心扩展能力是**运行时插件**；引入编译期 DI 会让"插件在运行期注册能力"变得别扭。收益用 ARC-3 的"装配清单化 + 启动期断言"替代 |
| N5 | **Redis 化的一切 / 远端必需依赖** | harness | 会破坏"离线可用 / 免外部服务 / 可嵌入"的定位。事件持久化只需本地 sqlite |
| N6 | **RBAC + 多主体权限矩阵** | harness | agentxx 是单用户本地工具，主体只有"用户/模型/插件"。真正需要的是作用域化的目标级判定（已有基础，见 SEC-1/SEC-10） |
| N7 | **把协议换成 REST/HTTP + Swagger** | harness | 交互是"会话内长连接 + 中断协商"，WS/JSONL 更贴合。可借用的是"契约生成"与"错误分类"（PRO-3/PRO-4/SEC-13），不是协议形态 |
| N8 | **默认容器化执行世界** | harness、pi、opencode | agentxx 的卖点之一是"直接用本机环境"。容器化作为**可选后端**（SEC-18）可以，作为默认路径会破坏体验与平台覆盖 |
| N9 | **多前端各自渲染** | openclaw、pi | "声明式组件描述 + 适配 + 唯一渲染实现"在跨端一致性上更强；代价是只有一种渲染实现，但这正是它省成本的原因 |
| N10 | **富输出的文本标记**（`MEDIA:` / `[[audio_as_voice]]` / `[embed]`） | openclaw | agentxx 的附件已是结构化字段、界面走组件描述。引入文本标记会让"内容"与"交付指令"混在一起，是倒退 |
| N11 | **一次性支持 40+ provider 与 OAuth 全家桶** | pi、opencode | 方向对（元数据 + 适配器），但超出当前范围。先做"能力元数据 + 目录数据文件"（LLM-6），provider 与鉴权方式按需增加（LLM-8） |
| N12 | **完整设备节点体系 / 插件市场 / 原生壳** | openclaw | 超出"可嵌入的 agent 库"的定位。只取"客户端在握手时声明自己支持什么"这半（已有接口集上报，见 PRO-2 的能力位） |
| N13 | **媒体 / 语音 / 会议 / 浏览器能力层进核心** | openclaw | 不是 coding agent 的刚需，且会持续增加核心的每次请求成本。若需要，按插件做（插件只交声明与工具，不进核心） |
| N14 | **约 200 个测试 project 的配置复杂度 / 双构建系统** | openclaw、codex | 那是 1.8 万个测试文件与 Bazel+cargo 双构建的产物。agentxx 用"模块名 + 耗时打印 + 成本预算"（TST-9）就够 |
| N15 | **四种插件形态与复杂 hook 决策语义照搬** | openclaw | agentxx 的 C ABI + 五态生命周期 + 接口表已经比它更严格；需要学的是**把决策语义写清楚**（PLG-8），不是把形态分类搬过来 |
| N16 | **"事件不重放，客户端自行刷新"** | openclaw、opencode | agentxx 的 `seq` + 重放对客户端更友好，应保留并升级为持久事件序列（STO-3/STO-4），而不是退回到刷新 |
| N17 | **无差别平台沙箱** | codex、harness、dsh | Windows/Android 上没有等价能力。若做，做成"可用则收窄 + 不可用则记录原因"的可选层（SEC-18） |
| N18 | **流程型重量门禁**（截图门禁、PR 模板、发布预检清单） | openclaw、harness | 是多贡献者协作与产品发布的需要。只取可自动化的部分（UI-1 快照、TST-7 契约、TST-4 基准阈值） |
| N19 | **配置的"无兼容分支"硬规矩** | openclaw | 它敢这么做是因为有 doctor 与更新器预检配套。agentxx 应先建迁移能力（CFG-2/CFG-7），成熟后再收紧 |
| N20 | **`ask_user` 只在主会话可用的限制** | openclaw | 该限制在其编排模型下必要；agentxx 的子代理是独立 agent 且 HIL 能冒泡，因此子代理也可以提问（经冒泡回父 IO） |
| N21 | **Effect 运行时服务容器 / Layer 图** | opencode | 表达力强但与 TS 运行时深度绑定。agentxx 的"显式注入容器 + 单 io_context 协程 + 自研取消令牌"在 C++ 里更直观可预测，没有调度黑箱 |
| N22 | **"依赖变化自动重启"（epoch 式）** | dsh | C++ 里重装插件意味着重新 `dlopen`、重建线程与 executor 关系，代价远高于 JS 重跑 `apply()`。显式禁用/启用 + `start` 事务重声明更合适 |
| N23 | **"行序无语义"（激活完全由服务可用性决定）** | dsh | agentxx 的拓扑排序是静态、可打印、可排障的，比"由运行时服务可用性决定"更好查问题，应保留并强化 |
| N24 | **bash 不设边界的取向** | opencode、pi | agentxx 在权限层对写路径做了硬边界（含 worktree 隔离与配置拒绝路径），对桌面端用户是实实在在的保护。正确做法是把"哪些是硬边界、哪些只是提示"写清（SEC-14），不是放弃 |
| N25 | **受限脚本编排（codemode 式）作为常规路径** | opencode、pi | 功能有吸引力但需要解释器、限额、诊断与权限沿用一整套设计，且扩大攻击面。只在出现明确需求（大量小调用导致往返开销）时再评估 |
| N26 | **把 TUI 拆成独立包/工程** | pi、opencode | agentxx 的 TUI 与声明式 UI 描述层深度耦合，当前拆包收益有限。更值得做的是**渲染层与 FTXUI 解耦**（UI-5） |
| N27 | **同时维护两条远程面**（CBOR RPC + JSONL RPC） | pi | 两套分帧/两套消息定义/两套客户端会带来长期同步成本。agentxx 已有统一 wire，只加分帧适配层（PRO-9） |
| N28 | **`compat.ts` 式的长期兼容层** | pi | 会拖累迭代。兼容应通过"表版本 + 能力协商"表达（已有机制），不堆叠兼容分支 |

### 17.6 已实现核对表（避免重复投入）

六篇里有 7 处"读文档会低估现状"的修正，逐条记下（这些**不要再当迁移项做**）：

| 机制 | agentxx 现状 | 六篇里被误判为缺失的位置 |
|---|---|---|
| 工具输出保真落盘 + 预览 + 按行分页取回 | **已实现**：`autoSummaryOutput` 且超阈值时全文写入 share_store，返回 `[Content offloaded. ... Total N lines, show [1,k], hide [k+1,N].]` 预览；`agentxx_share_store` 支持 `line_offset`/`line_limit` 分片读；`execute_command` 还自己按 stdout/stderr 分开压缩 | openclaw M28、harness P0-8（均已修正为"缺聚合预算与结构化定位符"） |
| 压缩三层降级 + 冷却 + 提示复用 | **已实现**：确定性压缩 → LLM 子代理摘要 → `hardTruncate`（两级兜底）；冷却用"消息条数增长 ≤2"判断无效压缩；`summarizationTipMsgId` 复用提示 | openclaw §9.6、harness §28（修正为"缺结构化摘要/保留尾部/溢出恢复"） |
| 请求前消息修复（`repairMessages`） | **已实现**：补 user 尾、清悬挂 tool_calls 与孤儿结果、修重复 `tool_call_id`、合并连续 user、空消息与 UTF-8 校验；且修复**不产生 UI 增量** | openclaw §3.6（修正：agentxx 的集中式修复比对方更完整） |
| 中断幂等 | **已实现（逐点）**：`summarizationTipMsgId`（提示复用）、`interruptToolcallCache`（已完成结果复用）、`xx_savedGraphData`（中断现场） | harness 修正 3（修正为"缺框架 helper + 必做清单 + 测试模板"→ TOOL-19） |
| delta 序号与一致性校验 | **已实现**：`Session::nextDeltaSeq()` 统一分配（重放缓冲依赖单调性）、`TurnEnd` 携带 `historyCount` + `tailHash`（展示历史链式哈希） | harness 修正 1/2（修正为"缺客户端侧校验 + 快照配对 + 契约文档化"） |
| 上下文 token 口径 | **已实现**：API usage 优先、估算兜底；估算按 UTF-8 前导字节分类、ascii 与 unicode 各自除系数再相加；结果写入 `session->contextStats` 供 UI 显示占用 | openclaw §3.6、harness §28（修正为"缺预算口径统一"→ CMP-1） |
| 附件体积上限 + 线程池卸载 | **已实现**：`maxBytesForMediaType` 上限、`http(s)` 不加载、读盘与 base64 卸载到线程池、`TurnStart` 只回显元数据（清空 `dataUrl`） | openclaw M61（修正为"缺 ① 服务端按路径加载前的权限判定 ② `http(s)` 附件的显式策略"） |

另外这些也是**已有**的（六篇都确认，不要重做）：

| 机制 | 说明 |
|---|---|
| 会话即上下文唯一权威 + 影子通道 `xx_messagesMeta` | 图状态不存 `messages`，`state.serialize()` 载荷与上下文大小无关 |
| 中断可跨进程恢复 | `AgentRunner` + `xx_savedGraphData`；主代理与子代理共用同一循环 |
| 插件生命周期事务 + 五态状态机 + 执行 lease + tombstone | 卸载安全机制完整（详见 §13.2） |
| 插件接口协商三层 + 表级版本 + 多实例三铁律 | 六篇一致认为是最强的部分 |
| 权限隔离优先 + 三态路径批量复核 + 配置拒绝先于完全授权 | 详见 §9.2 |
| worktree 写边界（隔离优先于白名单） | 六篇没有对等能力 |
| 子代理深度/并发预算 + 取消级联 + 同上下文 KV 模式 | 详见 §10.2 |
| 资源基准体系（smaps 模块级分解 + 基线对比 + 真实 TUI/PTY） | 六篇都没有等价能力 |
| 负面编译测试 + 真实 DSO 双端 + 多实例测试 | C++/ABI 项目里的高质量实践 |
| 中断表单/插件表单共用同一套渲染与交互（`UiFormState` + `UiHitRegion` + `OwnedReflect`） | 六篇确认这个方向比"各端各写"更省事 |
| FFI C API（26 符号、每实例双线程、agent 核心零改动） | 详见 §12.2 |
| 「同进程与远程只换 transport」 | 六篇确认比"把 TUI 当客户端"更彻底 |

### 17.7 验收方式（每项怎么算做完）

统一要求：**每项都要有一条能失败的测试**，否则不算做完。按类型给最低验收标准：

| 类型 | 最低验收标准 |
|---|---|
| 写入数据（STO-*、LOOP-4/6、SUB-1/3、SEC-3/8、STO-7） | ① 单测覆盖正常路径与边界（阈值、重复、冲突）；② "写失败 → 读回"路径有人测（含"旧版本数据 → 新版本可读"的兼容用例，TST-2/TST-13）；③ 崩溃/中断后重启能读到正确状态 |
| 安全类（SEC-1/2/3/6/7/11/15） | ① 每个判定分支产生唯一的 `reason`；② 负面用例（未声明权限的工具、被拒路径、越界 URL、符号链接逃逸）必须被拦；③ 守卫有效性证明：引入回归 → 测试变红 → 回退（TST-14） |
| 性能/并发类（TOOL-1/2/7、SUB-5、STO-10、UI-7） | ① 并发测试：多任务耗时应接近 max 而非 sum，且结果顺序稳定；② 上限测试（超限时排队/拒绝）；③ 竞态测试两个顺序都覆盖（TST-6）；④ 基准跑一次并附 Δ 数字（TST-4） |
| 上下文/成本类（PRM-1、CTX-1/3、LLM-5、CMP-2、TOOL-4） | ① 请求体级断言：捕获发给 provider 的实际请求 JSON，断言 system 段结构、消息顺序、缓存标记位置；② 同一会话连续两轮的前缀稳定性断言（前一轮的消息序列是后一轮的前缀）；③ 成本类改动用账本数字说话（STO-7） |
| UI 类（UI-1~UI-8） | ① 固定尺寸下的渲染文本快照 + 命中区快照；② "来自未来版本"的组件树渲染不崩且能降级；③ CJK 宽度与窄终端用例 |
| 协议类（PRO-1/2/4/5/6/7/8） | ① 每个消息类型的往返测试；② 幂等键：同 key 重发只执行一次；③ 版本不匹配给出明确错误；④ 阶段机：未握手/未绑定/重连中的请求被正确拒绝 |
| 工程底座（TST-1~TST-16、ARC-1~ARC-5、CFG-1~CFG-3） | ① 门禁脚本能在干净环境跑通，且**故意破坏一处应当失败**；② 配置校验失败输出必须带键路径与非零退出码；③ 诊断输出可复制为文本用于报障 |
| "不做清单"类（§17.5） | 不需要验收；但每次有人提出要引入时应回看对应条目，确认定位是否变了 |

**回填约定**（沿用六篇的做法）：每项落地后回填到 `resource/history/` 的实现记录，并在本文对应表行标注「已落地 + 日期 + 实际实现与计划的差异」；若实施中发现计划有误，**以源码为准**，并在 [§0.1](#01-六篇对比的对象与侧重) 后追加修正记录。

---

## 附录 A：来源对照

`来源` 列给出的是"哪些项目的设计里有这一条"；下表给出每个项目在六篇里的对应章节，便于回查原始论证与源码依据。

| 项目 | 对应文档 | 该篇最相关的章节 |
|---|---|---|
| codex | `compare-codex.md` | §2 轮次语义与请求抢占；§4 rollout 与写者锁；§5 工具编排器；§7 LLM 客户端状态分层与粘性令牌；§9 权限档案/命令策略/沙箱；§10 持久 shell 与执行环境加固；§16 hooks；§17 迁移建议汇总（P0 清单） |
| dsh | `compare-dsh.md` | §1 Cordis 的 effect/waterfall/scope；§2 turn/step 与三通道投递、持久 inbox；§5 工具四段流水线（策略有序·执行重叠）；§8 压缩是带锁的持久操作；§9 approval seam 与 fail-closed；§14 测试（一致性/竞态/快照）；§18 插件系统四层对比 |
| harness | `compare-harness.md` | §3 派生回填与启动期校验；§6 迁移框架与 SQL 错误翻译；§8 事件消费者组/重试/丢弃；§9 job.Scheduler 与超期回收；§10 授权语义与审计；§11 netpolicy 与执行后端；§12 只读/写两阶段与限流；§14 可观测；§19 迁移汇总（P0 清单）；§22~§33 深读；§34~§43 插件框架专题 |
| openclaw | `compare-openclaw.md` | §1 scoped AGENTS.md 与可执行边界；§2 车道与投递四态；§3 上下文引擎槽位 + 提示词缓存边界；§5 写者租约四步；§6 工具结果预算与缓存过期剪枝；§7 记忆分层与来源；§8 世代快照/看门狗/缓存策略；§9 压缩与溢出恢复；§10 审批执行身份绑定与运行期权威复验；§14 幂等键与协议生成；§15 插件槽位与清单；§16 测试成本预算与门禁；§19 迁移汇总（M1–M95） |
| opencode | `compare-opencode.md` | §2 durable 收件箱（admitted/promoted）与三原语；§3 Context Epoch 与历史投影；§5 聚合事件序列与 after 游标；§6 工具物化/结算与输出托管；§7 指令集合变更通知；§8 缓存策略/错误分类/录制回放；§9 结构化摘要与溢出重试；§10 权限 saved approvals 与连带处理；§15 插件 transform/hook 与逐维度对照；§18 迁移汇总（A–F 组） |
| pi | `compare-pi.md` | §2 操作状态机与 drive 原语；§3 entry 树 + 分支 + 车道与只追加不变式；§4 绑定地址 + 原子事务 + 崩溃恢复表；§5 工具三段式 + effect gate + 检查点；§7 provider 目录/能力元数据/重试分类/假 provider；§8 压缩保留尾部；§9 不内置权限系统的取向；§11 账本/遥测边界/导出；§12 自研 TUI（差分渲染/IME/浮层）；§13 两条远程面；§15 一致性框架与门禁；§17 迁移汇总（P0-1~P2-10） |

**同一条设计在多篇的编号对应**（只列 P0 里的一对多情形，避免重复计数）：

| 本文编号 | 其他文档里的对应编号 |
|---|---|
| TOOL-1 同轮并行 | codex §5.4-1；dsh §5「四段流水线」；harness P0-7；openclaw M26；opencode M23；pi P0-1 |
| LOOP-1 投递语义 | codex §2.4-1；dsh §2.4-1；harness M6；openclaw M6；opencode M1/M2；pi P1（steer/followUp） |
| STO-1 写者所有权 | codex §4.4-1；dsh §4.4-2；harness P0-6/P1（写租约）；openclaw M21；opencode M21 |
| STO-2 格式版本 | dsh §4.4-1；harness P0-6；openclaw M22；opencode M8；pi P2-8 |
| STO-3 事件序列 | dsh §3.4-3；harness P1-11（持久化订阅）；openclaw M23；opencode M3/M4；pi（entry 树 + seq） |
| STO-7 用量账本 | harness P1-15；openclaw M39；opencode M74；pi P0-2 |
| PRM-1 缓存边界 | openclaw M11；opencode M11；dsh §6.4-4；pi P0-8 |
| CTX-1 基线/增量 | opencode M11；dsh §6.4-2；codex §3.4-2/§6.4-2 |
| SEC-1/SEC-2/SEC-3 权限三件套 | harness P0-1/P0-3；openclaw M48/M51/M95；opencode M32/M33；codex §9.4-1/§9.4-9；dsh §9.4-1/§9.4-4 |
| SEC-7 出网策略 | harness P0-2；codex §9.4-5（可选沙箱思路）；openclaw（附件 URL 白名单，M61） |
| SUB-1 作业化 | harness P0-5；dsh §10.4-2；openclaw M58；opencode M58；pi P1-3；codex §10.4-3 |
| LLM-1/2/3 LLM 三件套 | openclaw M38/M44/M39；opencode M15/M73；pi P0-3；dsh（重试策略数据化）；harness P1-14（执行预算） |

---

*本文整理自六篇对比文档；所有"现状"描述都以 agentxx 源码为准（六篇的精读结论已逐条核对并把修正点收进 [§17.6](#176-已实现核对表避免重复投入)）。若后续 agentxx 有大幅改动，建议同步更新对应模块的"现状要点"与"已有优势"两节，并在改动落地后回填 [§17.6](#176-已实现核对表避免重复投入)*

