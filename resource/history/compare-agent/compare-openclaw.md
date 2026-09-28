# agentxx 与 openclaw 架构对比

> **对比对象**
> - `agentxx`: `D:\0Acoolight\Program\cpp\agentxx`（C++23，Boost.Asio 协程 + NeoGraph 图引擎 + 纯 C ABI 插件；单 `io_context` 内多会话协程交错执行、不加锁；可编译为可执行文件/动态库/静态库）
> - `openclaw`: `D:\0Acoolight\Program\js\openclaw`（TypeScript / Node ≥ 22 ESM + pnpm workspace，版本 `2026.9.6`；自述为 **multi-channel AI gateway**：一个常驻 Gateway 进程同时接管 WhatsApp / Telegram / Slack / Discord / Signal / iMessage / WebChat 等消息面，控制端与设备节点都经 WebSocket 接入）
>
> 本文按模块通读两侧源码、测试与文档后逐模块记录差异、优缺点与**可迁移到 agentxx 的设计**。
> 阅读顺序建议：先看[§0 总览](#0-总览与对比方法)与[§19 迁移建议汇总](#19-迁移建议汇总)，再按模块展开。
>
> **编写说明**
> - 每个模块统一按「agentxx 现状 → openclaw 现状 → 对比（表格 + 两者优缺点）→ 可迁移到 agentxx 的设计」展开；结论都给出源码/文档依据，依据清单见[附录 C](#附录-c本次精读的源码与文档清单)。
> - 只写**能在 agentxx 现有架构上落地**的迁移项，并标注落地位置与优先级（汇总见[§19](#19-迁移建议汇总)）；明确不该照搬的部分见[§20](#20-反向清单agentxx-不必照搬的设计)。
> - 两侧的规模数字均为本次在各自仓库实测算得（口径写在对应段落内）。
> - openclaw 侧存在大量「设计文档描述目标形态」的情况（例如 `docs/` 里的能力条目、`.agents/skills/` 里的流程约束）；凡引用都区分**「已成事实（源码可验证）」与「规格/约定（文档描述）」**，冲突处以源码为准，并在文中标出漂移点。
>
> **阅读提示：openclaw 是「一个网关 + 一层运行时 + 近两百个内置插件」，不是单一 agent CLI**
> - `src/`（约 1.29 万个非测试 TS 文件 / 90.8 MB）：**核心**。其中 `agents/`（agent 运行时，21 个二级模块）、`gateway/`（WS 网关与 RPC 方法）、`channels/`（消息通道边界）、`plugins/`+`plugin-sdk/`（插件装载与对面契约）、`infra/`、`config/`、`state/`（SQLite 与状态）、`auto-reply/`（入站消息 → 回复）是主干。
> - `packages/`（25 个 `@openclaw/*` 工作区包）：**可复用库**，如 `agent-core`（agent 循环）、`gateway-protocol`（协议 schema）、`ai`（provider 适配与流式运行时）、`plugin-sdk`、`sdk`、`memory-host-sdk`、`terminal-core`。
> - `extensions/`（约 200 个目录）：**内置插件包**（通道、provider、工具、记忆、诊断、迁移工具…），经 `package.json` 的 `openclaw` 字段声明资源。
> - `apps/`（macOS/iOS/Android/Linux/Windows/swabble 原生壳）、`ui/`（Web 控制台）、`crates/`（少量 Rust）。
> - 因此下文说「openclaw 有/没有某能力」时，都尽量落到具体某一层（核心 / 库 / 内置插件 / 原生壳），避免把插件能力当成核心能力。
>
> **修订记录**
> - **第 1 版（2026-09，分模块通读）**：按 §1–§18 共 18 个模块通读两侧源码/测试/文档并逐模块落盘，正文结构为「现状 → 对比 → 可迁移」；迁移项按 P0/P1/P2 归档到 §19（第 1 版共 **88 项**：P0 6 / P1 33 / P2 45 / 无需迁移 4），反向结论归档到 §20（15 条）。本版主要发现：
>   1. **两侧不是同类产品，但同一类问题的解法可以对照**：agentxx 是「给一个工作目录干活的编码代理」，openclaw 是「替你在各种聊天软件里收发消息、并在你的设备上执行动作的常驻助理」。本文因此把对比重点放在**机制**（会话串行、上下文装配、工具准入、插件边界、测试门禁）而不是功能清单。
>   2. **agentxx 最值得先补的六项（P0）**：提示词缓存边界（§3，长会话直接省 token）、同轮工具并行（§6，独立调用不再白等）、记忆分层按需检索（§7，全量注入是上下文预算的最大浪费）、模型空闲看门狗（§8，provider 卡住只能等整体超时）、溢出错误的压缩重试（§9，长会话最常见的一类失败）、副作用消息幂等键（§14，远程模式下网络抖动会重复执行）。六项都不改架构形状。
>   3. **openclaw 最值得借鉴的三层机制**：① **写者租约 + 事务内复验**（§5）解决「多入口写同一会话」；② **上下文装配作为带契约的可替换槽位**（§3）把「谁拥有压缩、谁拥有预检」讲清；③ **把架构约束写成可执行检查**（§1/§16：scoped `AGENTS.md` + 自定义 lint 边界规则 + 按边界拆的 CodeQL + 契约/快照一致性 CI + 测试成本预算）。
>   4. **agentxx 相对更强的地方也明确记录下来**（避免盲目照搬）：C ABI 插件边界与五态生命周期事务、多实例三铁律、三层接口协商、`share_store` 大内容暂存、worktree 隔离（隔离优先于白名单）、三层压缩降级（确定性 → LLM → 硬截断）、声明式 UI 组件层 + 唯一渲染实现、`seq` 重放、资源基准体系。
>   5. **明确不照搬的 15 条**（§20）大多可以用 openclaw 自己的原则解释：核心每加一样东西都会落到「每一次模型请求」上，所以能做成插件的就不进核心；agentxx 的核心更小、扩展点更硬，这条纪律同样适用。
> - **第 2 版（2026-09，实现精读）**：本轮把附录 D 列出的「只读接口没读实现」的位置逐处读到底（agentxx 侧：`agent_runner.cpp` 全文、`base_agent.cpp::runTurnAsync`、`nodes/modelcall.cpp`（含 `repairMessages` 与重试循环）、`middlewares/summarization.cpp`（压缩全流程与 `hardTruncate`）、`middlewares/permission.cpp`（`decideTarget`/`requestPermission`）、`nodes/toolcall.cpp::execTool`、`agent/io/session_server_agent_io.cpp`（queue/grace/hello/replay）、`plugins/plugin_manager_lifecycle.cpp` 与内核 `pluginxx/host/lifecycle.h`；openclaw 侧：`packages/agent-core/src/agent-loop.ts`（含并行启动握手、taint、abort 落库）、`packages/sdk/src/{event-hub,run-event-replay}.ts`、`src/agents/embedded-agent-runner/run/session-bootstrap.ts` 的 `claimAgentSessionWriter`、`src/agents/embedded-agent-runner/tool-result-truncation.ts`、`src/agents/agent-tools.policy.ts` 与 `tool-result-limits.ts`）。产出：
>   1. **新增 7 项迁移建议**（M89–M95），并把 **M28 按实际情况重写**——它原先要求的「工具输出保真落盘 + 预览」在 agentxx 里**已经实现**（`execTool` 把超限输出写进 share_store 并返回带 store id / 总行数 / 显隐行范围的预览，配套 `agentxx_share_store` 按行分页取回）；真正缺的是**一次请求内所有工具结果的聚合预算**（M28）与**缓存过期感知的剪枝**（M89）。同时修正了 §0.3、§6.1、§6.3、§6.4、§12.1 中受此影响的表述。
>   2. **补齐 9 个「实现细读」小节**（§2.6、§3.6、§5.6、§6.6、§9.6、§10.6、§12.6、§14.6、§15.6），记录只读接口看不出来的约束，例如：入站队列在「错误/中断」后**暂停出队**而不是继续灌消息；`repairMessages` 的五类修复；LLM 重试的四个不变量（含「失败必须留合法收尾消息」避免悬挂 tool_calls 被误路由回 tools 节点）；压缩的**条数冷却**与 `hardTruncate` 的两级兜底；权限判定顺序中「配置拒绝先于完全授权」与源码里挂着的**符号链接 TODO**；以及 openclaw 的**并行工具启动握手**（`guard → commit → 实现` 相邻、启动前持久化）、**`turnTainted`**、**abort 也要产出合法 transcript 与完整事件序列**、**写者租约四步顺序**、**工具结果聚合预算 + 缓存 TTL 剪枝 + 首尾保留/中段省略**、**SDK 的有界重放 + 观察者释放 + 抖动退避**、**插件关闭路径区分 lease 与 stop 两种未完成状态**。
>   3. **校正三处原先的偏差**：① 工具输出不是「压缩丢弃」而是「offload + 预览」；② agentxx 已有「API usage 优先 + 估算兜底」的上下文统计口径与 token/s 速度统计（M39 的范围相应缩小为「累计用量与缓存读写计数」）；③ 附件侧已有体积上限与线程池卸载（M61 相应收窄为两条真缺口）。
> - **待办（本文未完成的部分）**：附录 D 已按本轮精读结果更新——剩余未逐行读的主要是 openclaw 的 `attempt-*.ts` 阶段机细节、`src/gateway/server-methods/` 的组织方式、`src/state/` 的 worker 访问实现，以及 agentxx 客户端渲染层（`ui_components.cpp` 与 `framework/`）的实现。

## 目录

| 章节 | 内容 | 一句话结论 |
|---|---|---|
| [0](#0-总览与对比方法) | 总览与对比方法 | 两项目定位、规模、判据与结论速览 |
| [1](#1-总体架构与包分层) | 总体架构与包分层 | openclaw 是「核心 + 库 + 插件包 + 原生壳」四层，边界靠 scoped `AGENTS.md`、oxlint 边界规则与测试项目矩阵守着；agentxx 是「核心库 + 客户端 + 插件」三层，边界靠文档与约定 |
| [2](#2-会话运行模型与投递语义) | 会话运行模型与投递语义 | 可借鉴**四态投递模式（steer/followup/collect/interrupt）+ 车道级并发预算**；agentxx 需要把排队状态显式化 |
| [3](#3-会话上下文与-llm-上下文组织) | 会话上下文与 LLM 上下文组织 | 可借鉴**上下文装配作为可替换槽位**与**「只有压缩才重写历史」**；agentxx 最该补的是**提示词缓存边界** |
| [4](#4-工作区沙箱与多设备节点) | 工作区、沙箱与多设备节点 | openclaw 把「位置」分成状态目录/工作区/沙箱工作区/执行目录四层并写清规则；agentxx 的 worktree 隔离更强、隔离层更弱（应当写清边界） |
| [5](#5-持久化状态与崩溃恢复) | 持久化、状态与崩溃恢复 | 可借鉴**写者租约 + 事务内复验 + schema 版本骨架**；agentxx 需补「同会话多写者的显式裁决」 |
| [6](#6-工具系统) | 工具系统 | 可借鉴**同轮并行（带启动检查点）**、**结果守卫与保真落盘**、**审批的执行身份绑定**；agentxx 的四开关与循环检测更强 |
| [7](#7-系统提示词技能与记忆) | 系统提示词、技能与记忆 | 可借鉴**记忆分层 + 来源标记 + 按需检索**与**技能来源优先级**；agentxx 的技能渐进式展开已是好设计 |
| [8](#8-llm-层协议provider鉴权与缓存) | LLM 层 | 可借鉴**模型空闲看门狗**、**轮次局部的模型回退**、**用量与缓存统计**；agentxx 的三协议实现直接清晰 |
| [9](#9-上下文压缩与预算控制) | 上下文压缩与预算 | 可借鉴**溢出错误的识别与压缩重试（不重放已完成工具）**与**预算口径单一来源**；agentxx 的三层降级更稳 |
| [10](#10-权限审批与安全边界) | 权限、审批与安全边界 | 可借鉴**审批记忆持久化**、**执行身份绑定**、**运行期权威复验规矩**；agentxx 的声明式目标 + 三态判定 + 隔离优先更完整 |
| [11](#11-子代理后台任务与并行) | 子代理、后台任务与并行 | 可借鉴**fork 父上下文**、**子代理默认收窄工具面**、**后台预算（等待方不占槽位）**；agentxx 的中断委派 + 独立 agent + 同上下文压缩更强 |
| [12](#12-大输出附件媒体与结构化询问) | 大输出、附件、媒体与询问 | 可借鉴**结构化提问工具**与**附件来源校验**；agentxx 的 `share_store` 与跨设备附件选择是优势 |
| [13](#13-客户端与渲染分层) | 客户端与渲染分层 | agentxx 的「声明式组件 + 适配 + 唯一渲染实现」在跨端一致性上更强；需要补渲染快照回归 |
| [14](#14-远程协议sdk-与嵌入模式) | 远程协议、SDK 与嵌入模式 | 可借鉴**副作用消息的幂等键**、**协议版本与能力协商**、**协议 schema 机器可读**；agentxx 的重放与「同进程/远程只换 transport」更干净 |
| [15](#15-扩展机制插件框架专项) | 扩展机制（插件框架专项） | agentxx 的 C ABI + 五态事务 + 三铁律 + 三层接口协商是 openclaw 没有的边界；openclaw 的**单槽位替换**、**清单先行校验**、**文档分页化**值得借鉴 |
| [16](#16-测试与质量门禁) | 测试与质量门禁 | 可借鉴**测试成本预算**、**契约/生成物一致性检查**、**输出脱敏**、**边界静态守卫**；agentxx 的资源基准是差异化优势 |
| [17](#17-配置模型目录与装配) | 配置、模型目录与装配 | 可借鉴**配置迁移骨架（版本 + 迁移 + 备份）**与**装配快照 + 版本号**；agentxx 的 base/overlay + 列表三态语义更可预期 |
| [18](#18-openclaw-独有侧重的域多通道接入与自动化) | openclaw 独有侧重：多通道接入与自动化 | 通道边界契约、多用户路由、cron/常驻授权、做梦（离线整理）是 agentxx 没有的领域，按需借鉴（§18.6 给出相关性与建议） |
| [19](#19-迁移建议汇总) | 迁移建议汇总 | P0 6 项 / P1 34 项 / P2 51 项 / 无需迁移 4 项，共 95 项；含 6 个落地批次 |
| [20](#20-反向清单agentxx-不必照搬的设计) | 反向清单 | 15 条经调研确认的正当取舍 |
| [21](#21-结语) | 结语 | 三条原则：约束可执行、状态与语义显式、分清能力与代价的边界 |
| [附录 A](#附录-a关键文件与文档索引) | 关键文件/文档索引 | 两项目对应模块的路径对照 |
| [附录 B](#附录-b术语对照) | 术语对照 | 同一概念的两种叫法 |
| [附录 C](#附录-c本次精读的源码与文档清单) | 源码清单与结论依据 | 逐条列出本次实际读过的文件 |
| [附录 D](#附录-d可以继续深入的清单) | 可继续深入清单 | 尚未精读的位置与能回答的问题 |

---

## 0. 总览与对比方法

### 0.1 两个项目的定位与规模

| 维度 | agentxx | openclaw |
|---|---|---|
| 定位 | 可嵌入的 coding agent：核心库 + 客户端 + 插件，产物是可执行文件/动态库/静态库 | 多通道 AI 网关：一个常驻进程接管所有消息面，另有控制端、设备节点、原生壳与 Web 控制台 |
| 语言/运行时 | C++23；Boost.Asio 协程；单 `io_context` 内多会话协程交错执行，无锁；MSVC/GCC/Clang、Linux/Windows/Android/macOS | TypeScript（Node ≥ 22，ESM）；promises + worker_threads；publish 为 npm 包 `openclaw`（bin `openclaw.mjs`） |
| 顶层结构 | `agent/lib`（libagentxx）、`agent/client`（TUI/CLI）、`agent/plugins`（15 个内置插件 + 6 个示例目录）、`agent/test`、`agent/benchmark` | `src/`（核心）、`packages/`（25 个库）、`extensions/`（约 200 个内置插件）、`apps/`（原生壳）、`ui/`（Web 控制台）、`crates/`（少量 Rust） |
| 编排核心 | NeoGraph 图引擎（节点 + 条件边 + `Command.goto_node`），节点被中间件栈包裹；ReAct = `agent_start → llm → tools → llm → agent_end` | 自研 attempt 循环（`packages/agent-core` 的 `agent-loop.ts`）+ `src/agents/embedded-agent-runner` 的轮次编排；每会话一条车道串行 |
| 会话与上下文 | 会话是 LLM 上下文的唯一权威（typed `ChatMessage` + `messagesVersion`）；图状态里没有 `messages` 通道 | 会话 = SQLite 行 + transcript 事件；模型上下文由**上下文引擎**（可替换槽位，默认 legacy）在每轮装配 |
| 工具 | `XXToolBase`/`XXToolWrap`（自动压缩、延迟加载、重试、重复调用询问）+ `ToolcallWrapNode`（串行执行） | `src/agents/tools` + `agent-tools*.ts`（参数 schema、工具策略、调用前后适配器）+ 审批门 + 结果截断 |
| 权限 | 中间件统一判定：插件声明的目标 + 白/黑名单 + 工作目录隔离 + 完全授权 + 记住选择 + worktree 写边界 + 三态路径复核 | `exec approvals` + 工具策略矩阵 + 操作员角色（operator scopes）+ 设备配对信任；安全策略按「风险 vs 可用性」权衡写在 `SECURITY.md` |
| 扩展形态 | 纯 C ABI 动态库插件；agent 侧 19 张接口表（10 张通用 `pluginxx.*` + 9 张领域 `agentxx.agent.*`），client 侧 9 张；生命周期 `create/start/stop/destroy`；多实例三铁律 | 两类插件：code plugin（进程内 TS，`openclaw/plugin-sdk/*` 契约）与 bundle plugin（skills/MCP/配置）；`package.json` 的 `openclaw` 字段声明资源；`plugins.slots.*` 单槽位替换 |
| 客户端 | FTXUI TUI（唯一渲染实现）+ stdio CLI + 远程 WS 客户端；服务端产出声明式组件树（`agentxx.ui.item`） | `openclaw tui`、Control UI（Web）、macOS/iOS/Android/Linux/Windows 原生壳；共享协议 schema + 生成式 Swift 模型 |
| 远程/集成 | 手写 Wire JSON 协议 + WS + 进程内 Channel；MCP client/server、A2A server/client、ACP server、FFI | Gateway WS JSON 协议（`req`/`res`/`event`，首帧必须 `connect`；`@openclaw/gateway-protocol` 定义 schema）；节点角色 `role: node`；MCP、ACP、HTTP webhook |
| 测试 | 自研 `agentxx_test`（本次实测注册模块数见 §16）+ `agentxx_benchmark` 资源基准；Debug 默认 ASan+UBSan | `vitest` 项目矩阵（`test/vitest/` 下约 200 个 `vitest.*.config.ts`，按模块切分 + 权重分片）+ `tsgo` 类型检查 + `oxlint`（含自定义边界规则）+ CodeQL + semgrep |
| 文档 | `docs/zh-cn/design/`：index 166 KB、plugins 103 KB、tui 45 KB、benchmark 60 KB、ffi 24 KB | `docs/`：1326 个文件 / 21.7 MB（含 `concepts/`、`gateway/`、`channels/`、`tools/`、`plugins/`、`providers/`…）+ 各目录 scoped `AGENTS.md` |
| 代码规模（实测） | `agent/lib` 139 文件 / 5.45 万行；`agent/client` 68 / 2.36 万；`agent/plugins` 53 / 2.35 万；`agent/test` 172 / 7.64 万（口径：`.cpp/.h`，排除 `build/`、`third_party/`） | `src/` 非测试 12853 文件 / 90.8 MB；`src/` 测试 10404 文件 / 141 MB；全仓 `*.test.ts` 18596 个；`packages/` 1248 文件 / 8.0 MB；`extensions/` 1.07 万文件 / 90 MB（口径：`*.ts/*.tsx` 等，排除 `node_modules/`、`dist/`） |

两个项目**不是同一类产品**：agentxx 是「给一个工作目录干活的编码代理」，openclaw 是「替你在各种聊天软件里收发消息、并在你的设备上执行动作的常驻助理」。因此本文的对比重点不在功能多少，而在**同一类问题上两边各自选择了什么机制**（会话串行、上下文装配、工具准入、插件边界、测试门禁），以及哪些机制搬到 C++ 协程 + 图引擎这一侧仍然成立。

### 0.2 对比方法与判据

- 每个模块都用**三方证据**交叉核实：源码（真实行为）、测试（行为边界）、文档（设计意图）。文档与源码冲突处以源码为准并标注漂移。
- 「优点/缺点」的判据只有两条：
  1. **后续改动成本**：扩展点是否收敛、约束是否显式、有没有「只有作者知道」的隐含前提；
  2. **运行期正确性**：生命周期、取消、恢复、并发与资源回收是否可控。
- 迁移建议只写**能在 agentxx 现有架构（单 `io_context` 协程 + 图引擎 + C ABI 插件 + 声明式 UI）上实现**的项，并标注落地位置与代价；不能落地的不写。
- openclaw 的很多机制依赖「Node 单线程事件循环 + worker_threads + SQLite」这一组合，评估迁移时先判断它在协程模型下是否有对应物；没有对应物的，明确写进 §20 反向清单。

### 0.3 结论速览

| 模块 | agentxx 现状 | openclaw 现状 | 更值得借鉴的方向 |
|---|---|---|---|
| 分层 | 核心库/客户端/插件三层，边界靠文档 | 核心/库/插件包/原生壳四层，边界靠 scoped `AGENTS.md` + lint 边界规则 + 测试矩阵 | 把边界写成可执行检查（导入规则、包契约测试） |
| 会话运行 | 轮内执行 + 内存排队 + 打断立即执行队首 | 每会话一条车道；投递四态（steer/followup/collect/interrupt）；车道并发预算分级 | 投递语义显式化、排队状态可观测、并发预算分域 |
| 上下文 | 会话是唯一权威；每轮重拼 system prompt | 上下文引擎槽位（ingest/assemble/compact/commitTurn）+ 「只有压缩能改写历史」 | 装配过程可替换且可审计、提示词前缀稳定契约 |
| 工作区 | 工作目录多源回退 + worktree 写边界 | 工作区 + 沙箱重定向 + 设备节点（caps/commands）+ managed worktrees | 设备能力协商、沙箱工作区重定向 |
| 持久化 | 会话库（消息/上下文/viewMessages）+ 图 checkpoint，节流写 | SQLite + 写者租约（`activeWriterRunId`）+ 事务内校验 + 数据库访问下沉 worker | 写者租约式防陈旧写、事务内复验 |
| 工具 | 特性丰富但同轮串行；大输出已 offload 到 share_store 保真（缺聚合预算与缓存感知剪枝） | 工具策略矩阵 + 审批门 + 结果截断分层 + 调用前后钩子 | 准入策略矩阵、审批门、聚合预算与缓存过期剪枝 |
| 提示词/技能 | 字符串拼装 + 追加段 + 技能中间件 | 分层渲染 + 提示词稳定性契约（重建会毁掉前缀缓存）+ 技能/记忆插件槽位 | 提示词分层的显式契约、槽位化 |
| LLM 层 | 3 协议、无缓存断点、无错误分类 | provider 门面 + 模型运行时世代快照 + 空闲看门狗 + 失败切换 + 缓存观测 | 世代快照、看门狗分层、失败切换 |
| 压缩 | 阈值触发 + 工具输出去重/截断 + 子代理摘要 + 硬截断兜底 + 冷却 | 预检 + 溢出恢复（先收尾已准入工具）+ 压缩后重试 | 溢出恢复路径、预检权威口径 |
| 权限 | 声明式目标 + 三态判定 + 完全授权 + 隔离 | 审批门 + 操作员角色 + 设备配对；等待审批不计入执行预算 | 预算与等待解耦、运行期权威复验 |
| 子代理 | 中断即委派 + 深度/并发预算 + 取消级联 | 车道化并发预算 + 子代理会话（可 fork 上下文）+ 群的独立预算 | 车道化预算、子代理上下文模式显式化 |
| 大输出/附件 | 附件跨设备选择、无回退、无结构化询问工具 | 媒体理解/生成能力层 + 结构化询问工具 + 富输出协议 | 媒体能力分层、结构化询问、富输出协议 |
| 客户端 | 声明式组件树 + 唯一渲染实现 | 多前端共用协议 schema + 生成式 SDK + TUI 复用会话渲染 | 协议驱动的前端生成、组件故事化 |
| 协议/SDK | 手写 wire + WS/Channel + MCP/A2A/ACP/FFI | Gateway WS（握手/能力发现/幂等键/设备配对）+ 生成式 SDK | 握手与能力发现、幂等键、设备令牌 |
| 扩展 | C ABI + 19/9 张表 + 生命周期事务 + 多实例 + 接口协商 | 声明式清单 + 两类插件 + `slots` 单槽替换 + 资源发现回退 | 槽位化替换、清单声明与发现回退、插件信任分级 |
| 测试 | 模块化测试程序 + 资源基准 | 测试项目矩阵 + 权重分片 + 边界 lint + 测试成本预算 | 测试分片与成本预算、静态边界守卫 |
| 配置 | base/overlay yaml + 设置 KV + 覆盖/移除语义 | 分层配置 + doctor 拥有迁移 + 每 agent 一份模型运行时快照 | 迁移归属唯一、装配快照化 |
| 独有域 | — | 通道边界契约、定时任务/常驻指令/做梦（离线整理）、设备节点能力 | 按需借鉴（§18） |

---

## 1. 总体架构与包分层

### 1.1 agentxx 现状

三层结构（外加第三方与测试/基准两类外围目录）：

| 层 | 路径 | 规模（本次实测） | 职责 |
|---|---|---|---|
| 核心库 | `agent/lib`（`libagentxx`） | 139 文件 / 5.45 万行 | `agent/`（运行核心、配置、会话库、上下文、模型注册、wire 协议、io）、`nodes/`（图节点）、`middlewares/`（中间件栈）、`protocol/`（各家 LLM 协议 + MCP/A2A/ACP）、`tools/`（工具基类、worktree、子代理、共享库）、`plugins/`（插件宿主）、`event/`（事件流）、`util/`（sqlite、设置库、异常）、`ffi/` |
| 客户端 | `agent/client` | 68 文件 / 2.36 万行 | `io/stdio`（CLI）、`io/tui`（FTXUI，唯一渲染实现）、`io/tui/framework`（框架层：滚动/命中/表单/键位）、`train/`、`util/` |
| 内置插件 | `agent/plugins` | 53 文件 / 2.35 万行（15 个插件 + 6 个示例目录） | filesystem / execute_command / websearch / system / system_monitor / planning / math / string / codegraph / rag_search / javascript_engine / screen_capture / computer_use / text_selection_monitor / audio_stream |
| 测试/基准 | `agent/test`、`agent/benchmark` | 172 文件 / 7.64 万行、14 文件 / 0.93 万行 | 自研测试程序（模块化注册）+ 资源基准（内存/CPU/模块级分解） |
| 第三方 | `agent/third_party` | — | 自研三库 `cxx_utilxx_base`/`cxx_utilxx`/`cxx_pluginxx` + boost/ftxui/neograph/curl/openssl/hyperscan 等，按独立工程维护 |

边界的表达方式：

- **依赖方向**：`client` 依赖 `lib`，`lib` 不依赖 `client`；插件只依赖 `plugin SDK`（内核 `kit.h` + 宿主 `plugin_kit.h` umbrella）与两个工具静态库，不引用 `lib` 内部头。
- **宿主与插件边界**：纯 C ABI。插件是动态库，只导出宿主按名查找的入口符号（`agentxx_plugin_agent_*` / `agentxx_plugin_client_*`），ELF 侧用 `-fvisibility=hidden` + version script 白名单，其余符号全隐藏。
- **能力子集**：插件 `plugin.yaml` 的 `interfaces.require/optional` 声明依赖的接口名，宿主在加载前与自己支持的集合比对；`agentxx.agent.*` / `agentxx.client.*` 是保留命名空间，第三方私有接口用 `<vendor>.<name>`，宿主不认识即视为不支持（安全失败）。
- **约束靠文档**：`docs/zh-cn/design/` 的 index/plugins/tui/benchmark/ffi 五份文档是主要约束载体；代码里没有「导入方向检查」类工具。

### 1.2 openclaw 现状

四层 + 两类外围（原生壳、Web 控制台）：

| 层 | 路径 | 规模（本次实测） | 职责 |
|---|---|---|---|
| 核心 | `src/` | 非测试 12853 文件 / 90.8 MB；测试 10404 文件 / 141 MB | `agents/`（运行时，21 个二级模块：`embedded-agent-runner`、`tools`、`subagents`、`sessions`、`harness`、`auth-profiles`、`cli-runner`、`sandbox`、`worktrees`…）、`gateway/`（`server-methods` = RPC 方法、`worker-environments`）、`channels/`、`plugins/` + `plugin-sdk/`、`infra/`、`config/`、`state/`、`auto-reply/`、`cron/`、`sessions/`、`tui/`、`node-host/`、`memory*/`、`secrets/`、`media*/`、`talk/`、`tts/` 等 77 个二级模块 |
| 库 | `packages/*`（25 个 `@openclaw/*`） | 1248 文件 / 8.3 MB | `agent-core`（agent 循环/harness 类型/压缩助手）、`gateway-protocol`（协议 schema + 运行时校验器）、`ai`（provider 适配 + 流式运行时）、`gateway-client`（参考 WS 客户端）、`plugin-sdk`、`sdk`、`memory-host-sdk`、`terminal-core`、`markdown-core`、`media-*`、`normalization-core`、`tool-call-repair`、`retry`、`net-policy`、`session-url-contract`、`workboard-contract` |
| 内置插件 | `extensions/*` | 约 200 个目录 / 1.07 万文件 / 94 MB | 通道（whatsapp/telegram/slack/discord/signal/imessage/matrix/feishu/msteams/irc/line…）、provider（anthropic/openai/google/deepseek/qwen/mistral/bedrock/lmstudio/ollama…）、工具（browser/canvas/diffs/code-mode-quickjs/spreadsheet…）、记忆（memory-core/memory-lancedb/memory-wiki）、诊断（otel/prometheus）、迁移（migrate-claude/migrate-hermes） |
| 原生壳 | `apps/*` | 72 文件 / 1.5 MB | macOS / iOS / Android / Linux / Windows / swabble + `shared`、`.i18n` |
| Web 控制台 | `ui/` | 4440 文件 | Lit 组件 + 端到端 UI 测试 |
| 其它 | `crates/`（Rust 小件）、`skills/`、`qa/`、`deploy/`、`security/` | — | — |

边界的表达方式（这是 openclaw 最值得看的一层）：

1. **每个目录一份 scoped `AGENTS.md`**（实测 30 份，其中 4 份是 agent 工作区模板，见 `docs/reference/templates/`；模块规则覆盖 `src/plugins`、`src/gateway`、`src/gateway/server-methods`、`src/agents`、`src/agents/tools`、`src/agents/embedded-agent-runner/run`、`src/agents` 下的 `tools`/`run`、`src/channels` 及其 `plugins/contracts/test-helpers`、`src/infra/outbound`、`src/plugin-sdk`、`src/tui`、`extensions`（含 `acpx`/`agentsapi`/`telegram`）、`apps/{android,ios,macos}`、`ui`、`test`、`test/helpers`、`scripts`、`docs`）。根 `AGENTS.md` 只写跨目录原则（「一个责任一个 owner」「小核心、强插件」「稳定会话上下文」），并明确要求「把规则更新到它自己的 owner 目录，不要在这里加竞争规则」。
2. **自定义 lint 边界规则**：`config/oxlint/boundary-guards.json` 启用 `scripts/oxlint-boundary-guards.mjs` 这个 oxlint 插件，规则名即边界：`no-raw-window-open-call`、`no-register-http-handler-call`、`no-widen-then-assert`、`no-chained-type-assertions`（后两条限制用类型断言掩盖真实契约）。
3. **CodeQL 按「边界 + 重要度」拆配置**：`.github/codeql/` 下有十余份，例如 `codeql-plugin-trust-boundary-critical-security.yml`、`codeql-channel-runtime-boundary-critical-quality.yml`、`codeql-process-exec-boundary-critical-security.yml`——把「信任边界」当成可静态分析的对象。
4. **控制面/运行面分离**：`src/plugins/AGENTS.md` 明确要求「发现、清单解析、配置校验、装配提示、激活计划属控制面；真正的插件执行属运行面」，并要求「发现与激活保持惰性」——发现阶段不得 eager import 内置插件的重型 barrel。
5. **公共面单一来源**：`src/plugin-sdk/AGENTS.md` 拥有「公开边界扩张」的决策权；插件只能用 `openclaw/plugin-sdk/*` 入口，禁止 import `src/**` 内部实现。核心侧也不许给内置插件开后门（「Do not create private backdoors that bundled plugins can use but external plugins cannot」）。
6. **数据与存储归一**：状态与缓存统一进 SQLite（Kysely 访问），禁止新增 JSON/JSONL/sidecar 存储；数据库访问下沉 worker 线程，网关主线程只 await 结果。

### 1.3 对比

| 维度 | agentxx | openclaw |
|---|---|---|
| 分层数量 | 3 层（库/客户端/插件） | 4 层（核心/库/插件包/原生壳）+ Web 控制台 |
| 边界载体 | 目录约定 + `docs/zh-cn/design/*.md` | 目录约定 + 29 份 scoped `AGENTS.md` + lint 规则 + CodeQL 分区 + 各包 `tsconfig` 边界文件 |
| 边界执行 | 无自动检查；靠 review 与文档 | 部分可执行：oxlint 边界规则、插件包契约测试、`pnpm build` 触发的 fanout 校验 |
| 跨语言/跨进程 | C ABI 动态库（真正的二进制边界） | 进程内 TS import（无二进制边界）；跨进程只有 Gateway WS 协议 |
| 扩展成本 | 高（要写 C 代码、走 ABI、注意多实例铁律） | 低（写 TS，`openclaw/plugin-sdk/*` 即可）；但需遵守惰性与清单约束 |
| 单点真相 | 会话库是上下文的唯一权威；模型注册表、权限状态各有单一 owner | 「一个责任一个 owner」写在根 `AGENTS.md`，并用「配置迁移由 doctor 拥有」「模型运行时世代由宿主发布」等具体规则落实 |
| 代码组织粒度 | 一个库内按 `nodes/middlewares/tools/protocol` 分类，文件数少、单文件大 | 按职责切成极细文件（`run/` 下 200+ 个 `.ts`，很多只有几十行），配合「每目录一个 owner」 |

### 1.4 两者的优缺点

**agentxx**

- 优点
  - **边界是真边界**：插件是独立动态库，ABI 版本、导出白名单、多实例规则、接口协商四种机制齐全；插件崩了不会污染宿主。
  - **依赖少、产物自包含**：可编成可执行文件/动态库/静态库，第三方依赖静态并入，适合嵌入与分发（openclaw 的 `node_modules` + 原生壳是另一条路）。
  - **分层清爽**：`lib` 与 `client` 只有一个方向，代理核心不感知 UI，UI 只消费声明式描述（`agentxx.ui.item`）。
- 缺点
  - **约束不可执行**：分层、命名空间、`lib` 内部头不可被插件引用等规则没有工具检查；改动靠人读文档，容易出现「文档说 19 张表、实现已经 20 张」这类漂移。
  - **目录粒度粗**：`nodes/modelcall.cpp`、`middlewares/permission.cpp` 这类单文件承担多种责任，评审与定位成本随文件增长（对比 openclaw 的 `run/` 目录）。
  - **缺少跨目录「责任归属」文档**：只有一份大 index，没有按目录划分的规则文件，新人（或新 agent）改 `plugins/` 时看不到该目录的局部约束。

**openclaw**

- 优点
  - **把架构规则写进可执行检查**：边界 lint 规则、包契约测试、CodeQL 分区、快照/预算测试，让「架构约束」不依赖人记忆。
  - **scoped `AGENTS.md` 的威力**：规则就近声明、就近维护，且明确反对「在根目录叠加竞争规则」；对多 agent 协作的项目几乎等于机器可读的模块说明书。
  - **一致的数据与并发纪律**：SQLite 单一事实源 + 写事务同步（事务回调里禁止 `await`，先完成异步规划再复读权威行再写）+ 数据库访问下沉 worker，避免主线程被 I/O 拖住。
  - **惰性边界写进规则**：发现/校验/装配走轻量元数据，只有真正执行才加载重型 runtime。
- 缺点
  - **没有编译期边界**：插件与核心同进程、同语言，靠约定与 lint 兜底；规则一旦漏进某个 barrel 导入，问题以「启动变慢/内存上涨」的形式出现，成本不低（所以有 `scripts/profile-extension-memory.mts` 这类专项）。
  - **规则量巨大带来理解成本**：根 `AGENTS.md` 约 28 KB，各目录又有自己的规则，加上 `docs/` 22.7 MB，改动前「读完该读的」是硬门槛。
  - **测试矩阵膨胀**：`test/vitest/` 下约 200 个 project 配置 + 大量 `*-paths.mjs`，配置复杂度本身成为维护负担。

### 1.5 可迁移到 agentxx 的设计

| 编号 | 设计 | 落地位置 | 优先级 | 说明 |
|---|---|---|---|---|
| M1 | **按目录放局部规则文件**（对应 scoped `AGENTS.md`） | 新增 `agent/lib/AGENTS.md`、`agent/lib/include/agentxx/plugin/AGENTS.md`、`agent/client/AGENTS.md`、`agent/plugins/AGENTS.md`、`agent/test/AGENTS.md` | P1 | 每个目录只写该目录的硬约束（如「插件源码只能包含 `plugin_kit.h` umbrella，不得包含 `agentxx/plugin/api/*` 之外的头」「测试头文件必须走 `agentxx-test/...` 完整路径」）。避免所有规则堆在 `docs/` 的大文档里；根 `AGENTS.md` 保留跨目录原则并要求「规则更新到 owner 目录」。 |
| M2 | **把边界规则做成可执行检查** | 新增一个轻量检查脚本（如 `agent/script/check_boundaries.py` 或 cmake 自定义目标），进 CI/构建前置 | P1 | 至少覆盖三条：① `agent/lib` 不得包含 `agentxx-client/...`；② `agent/plugins/*` 不得包含 `agentxx/...` 内部头（只允许 SDK umbrella 与工具库）；③ `agent/client` 不得包含 `agent/lib/src/...`。这三条是当前最容易被无意破坏的。 |
| M3 | **表数量与接口清单单一来源** | `agent/lib/include/agentxx/plugin/plugin_interfaces.h` 已有的常量汇总，扩成一个自检：宿主启动时断言「支持的接口名集合 == 声明常量集合」 | P2 | 目前文档与实现已经漂移过（文档写 19/9，实测 client 侧已含 `timer`/`keybind`）。把「清单」变成可断言的常量表即可根治。 |
| M4 | **控制面/运行面分离的显式约定** | `agent/lib/include/agentxx/plugin/`、`agent/client/src/plugins/` | P2 | openclaw 要求「发现、清单解析、配置校验、装配提示属于控制面，且必须惰性」。agentxx 的插件加载已是 `dlopen` + 符号查找，天然惰性；可借鉴的是**在文档与代码注释里显式区分两侧**，并规定「读取插件元数据的路径不得初始化插件实例」。 |
| M5 | **存储归一** | 已是现状（会话库 + 图 checkpoint + `settings_db` 全部走 sqlite） | — | 无需迁移，但可把该约定写进 `agent/lib/AGENTS.md`，避免后续新增 JSON sidecar。 |

---

## 2. 会话运行模型与投递语义

### 2.1 agentxx 现状

**执行所有权**：每个会话由 `SessionServerAgentIO` 这一个「会话控制器」持有（1 个会话 = 1 个控制器 = 1 条驱动协程），多个会话的驱动协程在同一个 `io_context` 上交错执行，成员状态只在 `ex_` 线程访问，因此**不用锁**。

驱动循环（`SessionServerAgentIO::run()`）的形状是「取输入 → 跑一轮 → 推结果」：

```
run() 协程:  循环 { co_await getInput() → BaseAgent::runTurnAsync() → 推送轮次结果 }
```

**投递语义**（`session_server_agent_io.h` / `.cpp` 实测）：

| 机制 | 说明 |
|---|---|
| 消息队列 | `std::deque<MessageQueueItem>`，每项带自增 `id` 与文本/模型/附件；客户端有只读镜像（`WireMessageQueueUpdate` 广播） |
| 排队时机 | `pushMessageQueueItem()` 判断 `turnActive_`：空闲则立即唤醒驱动循环执行；忙碌则**进入队列并等轮次结束**（等同「followup」） |
| 失败/中断即暂停队列 | 轮次收尾时只有「无错误且未中断」才继续出队；否则 `queuePaused_ = true` 并通知客户端（详见 §2.6） |
| 暂停队列 | `WireCancel` 在轮次进行中会把 `queuePaused_ = true`，队列停止自动出队直到用户显式放行 |
| 中断并跑下一条 | `WireInterruptAndRunNext` → `interruptAndRunNext()`：取消当前轮次并置 `pendingInsert_`，轮次收尾后立刻执行队首 |
| 删除排队项 | `removeQueueItem(id)`（客户端点掉某条待发消息） |
| 断线宽限 | 只要有客户端在线就不进宽限期；全部断开且轮次进行中才起 `gracePeriod`（默认 30s）定时器，期间任意客户端接入即取消；`gracePeriod <= 0` 时断线立即取消轮次 |
| 增量重放 | `deltaBuffer_` 环形缓冲（默认 4096 条）+ 单调 `seq`；重连时 `deltasSince(seq)` 能连续就重放，否则回退全量 `sync` |
| 历史分页 | `initialSyncTailCount > 0` 时首屏只同步末尾 N 条，客户端向上滚动再经 `WireGetViewMessages` 分页拉取 |
| 一对多 | 1 个会话控制器可挂 N 个客户端；实时事件广播，握手/分页/查询类消息单播给发起方 |
| 会话切换 | `switchSession()` 重绑 `sessionId`、清空 delta 缓冲、回推全量 sync；仅在无进行中轮次时生效 |

同进程内还有另一条路径：`agent/client` 直接以 `stdio`/`tui` 驱动 `BaseAgent`（进程内 Channel transport），语义与上面完全一致——**换 transport 不换语义**。

### 2.2 openclaw 现状

**执行所有权**：每会话一条**车道**（`session:<sessionKey>`），可选再进一条**全局车道**（`main`）。车道是通用 FIFO 队列（`src/process/command-queue.ts`），带并发上限：

| 车道 | 默认并发 |
|---|---|
| `main`（入站轮次，全进程） | `max(8, CPU 并行度 × 4)`，可被 `agents.defaults.maxConcurrent` 覆盖 |
| `session:<key>` | 1（保证同一会话同时只有一个轮次） |
| `subagent:<spawning session>` | 8（`agents.defaults.subagents.maxConcurrent`） |
| `subagent:swarm:<group>` | 32（`tools.swarm.maxConcurrent`） |
| `cron` / `cron-nested` | 独立预算，后台任务不挤占入站回复 |
| 后台工作（技能评审、插件后台完成、做梦） | 全进程 3 个槽位 |

**投递语义**：通道/客户端在「同会话已有活动轮次」时的四种模式（`docs/concepts/queue.md`）：

| 模式 | 行为 |
|---|---|
| `steer`（默认） | 把消息**注入正在跑的轮次**：已启动的工具跑完、尚未启动的串行调用跳过、在下一个工具启动或模型决策前让 steer 可见；若运行时不能接受 steer，退化为等轮次结束 |
| `followup` | 不注入，作为**下一次轮次**入队 |
| `collect` | 不注入，在静默窗口内把若干条**合并成一次** followup 轮次（不同通道/线程各自出队以保路由） |
| `interrupt` | 中止当前轮次，跑最新一条 |

配套参数：`debounce`（默认 500ms 静默窗口）、`cap`（默认 20 条）、`drop`（`summarize` 默认保留压缩摘要 / `old` 直接丢最旧 / `new` 拒绝最新）。模式解析优先级：会话内 `/queue` 覆盖 > `messages.queue.byChannel.<channel>` > 全局配置 > 默认 `steer`。

**输入持久性**：`chat.send` 到已有会话的普通输入**先落库再确认**（同 agent 数据库）；`collect` 模式下「合并轮次 + 标记来源输入已消费」在同一个事务里完成。网关在排队输入落到 transcript 之前停掉时，重启后它显示为「被打断的输入」并要求显式重发——**内存队列本身不重放**。

**排队轮次的取消身份**：网关为排队中的客户端 `runId` 保留一个「网关持有的取消身份」，`chat.abort` 带 `runId` 可取消仍在排队的轮次；不带 `runId` 时**先取消已授权的排队轮次，再中止活动轮次**（避免队列把工作推进到一个已半停的会话）。

**写者租约**：轮次被准入时记录持久化的 `activeWriterRunId`；此后每次 transcript 追加/重写都要带上 `expectedWriterRunId`，同步提交事务校验它仍等于当前活动声明——被取代的轮次因此无法提交陈旧数据。

**超时分五层**（`agent.wait` 30s 仅等待 / agent 运行预算默认 48h / 模型空闲看门狗 云 120s、自建 300s / provider HTTP 超时 / 各后台清理窗口），且「有进度不重置整体预算」。

### 2.3 对比

| 维度 | agentxx | openclaw |
|---|---|---|
| 串行单位 | 会话控制器（1 会话 1 协程） | 车道（`session:<key>` 并发 1 + 全局 `main`） |
| 排队状态 | 内存 `deque` + 客户端镜像广播；**失败/中断后暂停出队**（需用户放行）；断线宽限期 | 内存队列 + 未落库输入在重启后标为「被打断」；已接收的 `chat.send` 先落库 |
| 投递模式 | 隐式两种：忙碌即排队（≈followup）；`Cancel` 暂停队列；`InterruptAndRunNext` 打断并跑队首 | 显式四态 steer / followup / collect / interrupt，可全局、按通道、按会话覆盖 |
| 合并批量输入 | 无（一条一条排队） | `collect` + `debounce` 静默窗口合并 |
| 运行中注入 | 无（要么等，要么打断） | `steer` 注入活动轮次，且定义了对工具批次的可见性时点 |
| 并发预算 | 会话数由使用方控制（每会话一个控制器）；无全局并发上限概念 | 车道分级并发上限（全局/子代理/群组/后台各一套） |
| 排队项可见性 | 有：`WireMessageQueueUpdate` 广播 + 单项删除 | 有：Control UI 显示待执行输入、可按 `runId` 取消 |
| 断线恢复 | delta 环形缓冲按 `seq` 重放，超范围回退全量 sync | 事件**不重放**（文档明示），客户端必须在 gap 后自行刷新 |
| 陈旧写入防护 | 会话内只有一个轮次在写，靠「同一控制器单协程」保证 | 显式写者租约 `activeWriterRunId` + 事务内校验 |

### 2.4 两者的优缺点

**agentxx**

- 优点
  - **模型简单且已经工程化**：一个会话一个控制器，队列、暂停、打断、单项删除、宽限期都有对应实现与客户端镜像；单 `io_context` 协程模型下没有锁与竞态面。
  - **重连体验更好**：`seq` + delta 环形缓冲 + 尾窗历史分页，长会话恢复不需要全量重传（openclaw 明确不重放事件）。
  - **一对多天然支持**：同一会话多客户端接入是控制器的常规路径，openclaw 的多客户端是靠网关与投影另做一层。
- 缺点
  - **投递语义不外显**：只有「排队/打断」两种隐含行为，用户没法表达「这条先别急，攒一起再说」或「插到当前轮次里」；`steer` 这类能力缺失会直接影响交互效率（用户要么等一整轮，要么粗暴打断）。
  - **排队状态易被误置**：`queuePaused_` 由 `WireCancel` 置位，空闲时收到取消不置位这条补丁说明该状态是「隐式且易错」的；缺少显式状态机。
  - **队列只活在内存里**：进程重启后排队项全丢，且没有 openclaw 那种「展示为被中断输入并要求重发」的显式交代。
  - **没有全局并发预算**：多会话并发数没有上限概念，实际并发由客户端行为决定；后台任务（压缩子代理等）与前台轮次共用同一份资源，没有分级。

**openclaw**

- 优点
  - **投递语义是一等公民**：四态 + 三层覆盖 + 批量/合并/丢弃策略，把「用户说话时机器人在忙」这一日常问题解法化了。
  - **预算分级清楚**：入站、子代理、群组、后台各有独立预算，后台任务不会饿死前台回复（`cron-nested`、后台 3 槽位都是显式设计）。
  - **写者租约解决陈旧写入**：在多入口（网关 RPC、CLI、cron、子代理）都能写同一会话的前提下，用一条不变量替代了「大家小心」。
  - **输入持久性与「不确定态」处理**：先落库再确认、不确定时明确告知「可能已提交，不要再重发」。
- 缺点
  - **机制面很宽**：四种模式 × 三层覆盖 × debounce/cap/drop × 取消身份 × 车道策略，规则文档（`queue.md` 16 KB）本身就是维护成本，任何新入口都要理解这一整套。
  - **事件不重放**：客户端重连后必须走「刷新」路径，实时流与补齐流是两套语义。
  - **steer 的可见性时点依赖运行时能力**：native CLI 后端不能 steer，`steer` 会静默退化成 followup——同一模式在不同运行时的行为不一致，用户很难预期。

### 2.5 可迁移到 agentxx 的设计

| 编号 | 设计 | 落地位置 | 优先级 | 说明 |
|---|---|---|---|---|
| M6 | **投递模式显式化（至少补 `steer` 与 `collect`）** | `SessionServerAgentIO`（新增 `DeliveryMode` 配置 + 队列策略）、`wire_protocol.h`（新增模式设置消息）、TUI 输入区 | P1 | 最小落地：会话级 `deliveryMode` 三态 `queue`（现状）/ `collect`（新增，攒 debounce 窗口合并成一条）/ `steer`（新增，注入当前轮次）。`steer` 在 agentxx 侧有天然实现点：模型调用节点前的「检查点」——`modelcall` 节点每轮开始前可读取一个「待注入消息」列表并追加为 user 消息。 |
| M7 | **排队状态的显式状态机** | `SessionServerAgentIO`（把 `queuePaused_`/`pendingInsert_` 两个 bool 换成 `QueueState` 枚举：`idle / running / paused / draining`） | P1 | 现在的两个 bool 组合出四种状态却没有一处集中校验；改成枚举后「空闲收到 Cancel 不应暂停」这类规则变成状态转移表的显式一行，并可在测试里做状态机穷举。 |
| M8 | **排队项持久化 + 重启后的显式交代** | `SessionStore`（新增排队项表或在会话表加 pending 段）、`SessionServerAgentIO` 启动恢复 | P2 | openclaw 的做法是「已接收必须先落库；未落库的显示为被打断输入」。agentxx 可先做半步：排队项落库（文本+附件+模型选择），重启后恢复为非自动执行的「待重发」列表，由客户端提示用户确认。 |
| M9 | **车道化并发预算** | `BaseAgent`/`AgentConfig`（全局轮次并发上限）、`SubagentManager`（子代理独立预算）、后台任务（压缩/标题生成）独立小预算 | P2 | 现状是「有多少会话就并行多少」，在长上下文 + 多会话下容易把内存与上游限流推满。建议：`AgentConfig` 加 `maxConcurrentTurns`（全局）与 `maxConcurrentSubagents`（每会话），并发满时排队而不是拒绝；后台类任务（如手动压缩、标题生成）走独立的小预算，避免与前台抢。 |
| M10 | **`debounce` 合并窗口参数化** | 与 M6 的 `collect` 同批 | P2 | 默认 500ms 静默窗口；给 TUI 与配置各留一个可调项（`messages.queue.debounceMs` 对应）即可，不需要通道级细分。 |

### 2.6 实现细读（本轮新增）

> 本节是逐行读 `agent_runner.cpp`、`base_agent.cpp::runTurnAsync`、`session_server_agent_io.cpp::run/handleHello` 与 openclaw `agent-loop.ts` 后补的实现级事实，用来校正前面「只看接口」得出的判断。

#### agentxx：一个「运行 + 中断 + 恢复」循环，主代理与子代理共用

`AgentRunner::run()`（`agent/lib/src/agent/agent_runner.cpp`，278 行）是所有轮次执行的唯一入口，主代理与子代理**共用同一份循环**，差异全部收敛为 `Hooks`（`eventCallback` / `onInterruptTip` / `onBeforeResume` / `onRunResult`）——这从结构上消除了两套循环的行为漂移。实现里有六条不靠注释很难发现的规则：

| 规则 | 实现 | 为什么重要 |
|---|---|---|
| 中断信息先落 checkpoint 再处理 | 处理中断前先把 `graphData` 写进 state 的 `channel_savedGraphData` | 中断处理期间进程被杀，重启后能凭它重建中断并 resume（`runTurnAsync` 检测到该通道即走 `initialResult` 直接进中断循环） |
| resume 必须沿用首跑参数 | 保存 `cfg.stream_mode` / `cfg.max_steps` 再 `resume_async` | 否则事件回调与步数预算漂移 |
| resume 必须带 `cancel_token` | `resumeCfg.cancel_token = cancelToken` | 注释写得很直白：不带则 resume 出的新 run 无取消能力，表现为「压缩完成后怎么都停不下来」 |
| HIL 超时取 IO 端点配置 | `SessionServerAgentIO::interruptTimeout`，`<=0` 不限制 | 避免权限弹窗被总线默认 30s 超时提前截断而误判为拒绝 |
| 委派请求不设超时 | `service.subagent` 请求传 `milliseconds{0}` | 子代理可能长时间运行 |
| 取消有三处显式检查 | ① 子代理结果 `cancelled` 置位 → 抛取消；② HIL 应答含 `__cancelled__` → 抛取消；③ 中断处理完成后、resume 前再查一次 | 防「打断后马上自动恢复继续执行」 |

另有两处细节值得记住：

- **「中断未完成」不能用 `result->interrupted` 判断**。循环退出时空结果是合法状态（无处理者/未响应），源码注释明确指出用 `result.has_value() && result->interrupted` 是**死条件**，会让子代理把「未完成的中断」当成功结果继续用。
- **非协程转发包装**：`engineRunStreamAsync` / `engineResumeAsync` 是 `AGENTXX_NOINLINE` 的**非协程**转发函数，用来阻止 MSVC + 全程序 LTO 把内层协程帧尺寸复制进外层帧（MB 级帧沿链翻倍，见 benchmark.md 第 11/13 节）。这是 C++ 协程工程里少见的实测结论。

#### agentxx：入站队列的收尾规则比「排空队列」更谨慎

`SessionServerAgentIO::run()` 的驱动循环（`session_server_agent_io.cpp`）在每轮结束后的处理是：

```cpp
if (!turnResult.hasError && !turnResult.interrupted) queuePaused_ = false;  // 仅"干净成功"才继续出队
else { queuePaused_ = true; /* 通知客户端队列已暂停 */ }
```

即**错误与中断都会暂停队列**（而不是继续把剩下的排队消息一条条发出去），用户需要显式放行。这比我原先写的「忙碌即排队（≈followup）」要保守得多，也更合理——失败/中断往往意味着上下文或环境有问题，继续灌消息只会放大问题。同时：

- `run()` 启动时先**异步预热会话**（`getSessionAsync` 把 SQLite 加载卸载到线程池）。注释说明原因：长历史会话的同步加载可达数百毫秒，会把 io 线程上所有会话的 LLM 流与工具执行一起卡住。
- 新产出的 `WireDelta` **必须分配会话级 `seq`**（`Session::nextDeltaSeq()`）；未分配（seq=0）的 delta 不进重放缓冲，断线重连时该消息直接丢失。
- `handleHello` 有明确的**顺序不变量**：先发 `HelloAck` 再重放。原因是客户端握手循环会丢弃 `HelloAck` 之前的消息；若先重放后握手，全量 sync 与增量 delta 都会被客户端丢掉。重连时还会**重发未应答的中断卡片**（`pending_`），并按 `HelloAck` 里的插件清单（名字/版本/声明接口）让 client 插件判断对端可用性。
- 重放回退分两档：`lastSeq > 0` 且缓冲仍覆盖 → 增量重放；**缓冲溢出 → 全量 sync（不走尾窗，客户端整体重置历史窗口）**；首接入（`lastSeq == 0`）→ 尾窗 sync + 按需分页。

#### openclaw：轮次循环把「steer 提交」和「abort 落库」都当成不变量

`packages/agent-core/src/agent-loop.ts::runLoop` 的结构是外层 `while(true)` + 内层 `while (hasMoreToolCalls || pendingMessages.length > 0)`，其中：

- **待注入消息（steer）在每轮开头统一提交**（`commitPendingMessages`），并且**取消检查做两次**（`message_start` 事件前后各一次）——因为取消可能发生在事件发送期间；若整批都在提交前被取消，则 `continue` 重评估循环，**避免发出一个空的 provider 续跑请求**。
- **`turnTainted`（轮次污染标记）**：用户消息会**清除**该标记，工具结果可能**置位**该标记（`toolResultTaintsTurn`）；assistant 消息在落库前统一 `withAssistantTurnTaint` 打标。即「这一轮的产出是否源自不可信的工具内容」是随消息走的元数据，供后续（记忆写入、审计、缓存）判定信任级别。
- **中止也要产出合法 transcript + 完整事件序列**：`stopIfAborted()` 在终止前先落一条**失败 assistant 消息**，再补齐 `turn_start` / `message_start` / `message_end` / `turn_end`（必要时补 `appendInterruptedTurnMessage`）与 `agent_end`。注释点明目的：让会话后处理**不会**从一条悬挂的 toolUse 消息继续压缩或续跑。
- **provider 失败就不再执行剩余工具**：`providerFailed = stopReason === "error" | "aborted"` 时 `remainingToolCalls = []`；否则只执行「未执行过、且 `stopReason === toolUse` 或声明为 async」的**终端工具批次**（`streamed.executedIds` 用于防重复执行）。

**并行工具的启动握手**（`launchParallelToolCalls`，对应本文 M26）比我原先的描述精细：批内每个调用在真正启动时先执行 `batchLifecycle.commitReadyCalls([{toolCallId, args}])`（把这次调用提交进 transcript），**提交失败就整批拒绝**；并且游标推进由「最后一个 source-start 回调」触发（`queueMicrotask`），以保证 `guard → commit → 实现` 三步相邻，而实现体之间是重叠执行的；若某个调用在 start 回调之前就已结束，则**不提交、直接推进**。也就是说 openclaw 的并行不是「裸并发」，而是给每个调用绑定了一次**启动期的持久化握手**。

**对本文结论的影响**：M26（同轮并行）需要补三条实现约束——① 派发前统一做权限询问（agentxx 的 `execTool` 已经是「先问后做」）；② 每个调用在真正开始执行时做一次「调用已提交」的握手（agentxx 可对应「把 assistant 的 tool_calls 与 tool 结果配对写入上下文」这一步，或至少保证失败时不会留下半提交状态）；③ 结果按 `tool_call_id` 归位（已在 M26 说明）。

---

## 3. 会话上下文与 LLM 上下文组织

### 3.1 agentxx 现状

**上下文归属**（2026-09 定案）：**会话是 LLM 上下文的唯一权威**。

- `Session` 存 typed 上下文（`std::vector<neograph::ChatMessage>` + `messagesVersion`）；Json 形态（`Session::llmMessagesJson()`）只在落库 / `WireGetContext` / 插件查询时惰性生成。
- 写入口只有四个：`appendMessages` / `replaceMessages` / `replaceMessagesFromJson` / `truncateMessages`。
- 图状态里**没有** `messages` 通道，只有一个只读影子通道 `xx_messagesMeta`（条数/版本/角色分布/末尾 assistant 是否带 tool_calls）。
- 节点与中间件统一经 `agentxx/nodes/session_context.h` 读写：`sessionMessages()`（只读借用，**不得跨 `co_await`**）、`appendSessionMessages()`（追加 + 发同形 CHANNEL_WRITE 图事件）、`updateMessagesMeta()`（刷新影子通道）。
- 展示历史与上下文是两份数据：`viewMessages`（UI 展示用，含工具调用展开）与 `llmMessages`（真正发给模型）。

**系统提示词**（`AgentPrompt` / `BaseAgent::buildSystemPrompt`）：

```
system 消息 = systemPrompt
            + 按 key 字典序拼接的 appendSystemPrompts（插件/功能段：planning、skill、codegraph、git-worktree…）
            + appendSystemMessage（skill/memory 每轮动态生成）
```

- 提示词文本里可写会话级占位符 `${work_dir}` / `${temp_dir}` / `${session_id}`，拼装时按会话替换（`renderVars` 做纯文本替换，不做 fmt 模板解析；取值缺失替换为 `unknown` 而不是留空 token）。
- **每轮整体重建**：`buildSystemPrompt(sessionId)` 每次调用都重新拼一遍（含动态段），模型看到的 system 消息内容随环境变化。

**压缩**（`SummarizationMiddlewareHandle`）：

| 环节 | 现状 |
|---|---|
| 触发 | 上下文超过模型上限的 75% 自动触发；`>= 95%` 或同轮压缩失败 ≥ 2 次则硬截断兜底 |
| 确定性压缩 | 噪声清理（空消息、相邻重复、连续 AutoInserted 提示）+ 工具调用去重截断 + 探索型调用序列折叠（连续 ≥3 次同工具单调用段只留最后一组） |
| LLM 压缩 | 经 `NodeInterrupt` 派生压缩子代理：**同 `threadid` + 同模型**（命中 KV cache）、`tools=[]`、禁止二次压缩；子代理输出纯文本摘要 |
| 切分口径 | `splitRecentByTokenBudget` 按预算切 near/far，且做轮次对齐（recent 开头的 tool 消息回退到发起它的 assistant；压缩段末尾悬挂 tool_calls 整组划入 recent） |
| 写回 | **压缩完成立刻写回会话上下文并落盘**（进程在压缩后到轮末之间崩掉也不丢压缩结果、不会反复压缩） |
| 结果形状 | `[system] | [user 压缩指令] | [assistant 摘要] | 最近消息`（最近段预算默认占上限 20%） |
| 手动触发 | `compactSessionContext(sessionId)`（客户端「压缩上下文」按钮） |

### 3.2 openclaw 现状

**上下文装配是一个可替换的槽位**（`plugins.slots.contextEngine`，默认内置 `legacy`）：

| 生命周期 | 时机 | 职责 |
|---|---|---|
| `ingest` / `ingestBatch` | 消息进入会话时 / 一轮结束后 | 存入引擎自己的数据仓（索引、向量库…） |
| `assemble` | 每次模型调用前 | 返回**适配 token 预算的消息序列** + `estimatedTokens` + 可选的 `systemPromptAddition`（拼在 system 提示词前）+ `promptAuthority` + `contextProjection` |
| `compact` | 窗口满或用户 `/compact` | 压缩旧历史 |
| `bootstrap` / `maintain` | 引擎首次见到会话 / 引导、成功轮次后、压缩后 | 初始化与「安全重写 transcript」（经 `runtimeContext.rewriteTranscriptEntries()`） |
| `afterTurn` | 一轮结束后 | 持久化状态、触发后台压缩 |
| `commitTurn` | **被接受的成功轮次**收尾时 | 以 `advancementKey` 做**原子幂等**提交，重试返回 `duplicate` |
| `prepareSubagentSpawn` / `onSubagentEnded` | 子代理派生/结束 | 共享上下文状态的准备与清理 |
| `dispose` | 拥有它的操作结束时 | 释放引擎实例资源 |

关键约束（这些是它值得学的部分）：

- **`ownsCompaction`**：`true` 时宿主关掉自己的自动压缩与通用预检，交给引擎；`false` 时引擎的 `compact()` 仍会被 `/compact` 与溢出恢复调用，但内置自动压缩可能仍然跑。文档明确警告：非自持引擎写空实现的 `compact()` 是**不安全**的。
- **`promptAuthority`**：默认 `"assembled"`（只检查装配后的估算）；引擎若自持压缩则宿主默认跳过通用预检；只有引擎声明 `"preassembly_may_overflow"` 时宿主才保留预检并取「装配估算」与「装配前全历史估算」的较大值。
- **`contextProjection`**：`mode: "thread_bootstrap"` + 稳定 `epoch` 表示「宿主别每轮重投影，按 epoch 注入一次并复用后端线程」（为 Codex app-server 这类有持久后端线程的运行时准备）。
- **`acceptedHostParams`**：引擎声明自己接受哪些宿主注入字段，宿主取交集，未声明/未知字段**永不注入**。
- **宿主能力要求（`hostRequirements`）**：引擎可声明「需要 `assemble-before-prompt`」，宿主在不满足时**fail closed**（在启动前拒绝）；而运行时**方法抛错**走的是另一条路——**隔离（quarantine）该引擎到进程结束并降级到 `legacy`**，让回复继续。声明了却无法履约（例如精确锚点消息已被改写）则**只在本轮降级**，下轮再试。
- **transcript 语义声明**：持久化准入轮次要声明 `currentTurnFence: "before-current-turn-entry-v1"` 与 `turnAdvancementIdempotency: "atomic-idempotent-v1"`，否则整轮（含重试）退回 legacy 路径。

**系统提示词**同样是显式分层（`docs/concepts/system-prompt.md`）：

- **三层**：`buildAgentSystemPrompt`（纯渲染器，不读全局配置）/ `buildConfiguredAgentSystemPrompt`（叠加配置项）/ 运行时适配器（收集工具、沙箱、通道能力、上下文文件等实时事实）。
- **固定段列表**：Tooling / Execution Bias / Promised Work / Care / Runtime Context / Skills / OpenClaw Control / Messaging / Workspace / Documentation / Sandbox / Temporal Context / Assistant Output Directives / UI Presentation / Collapsible Details / Runtime / Reasoning。
- **缓存边界写在设计里**：大块稳定内容（Project Context、静态 Memory Recall 指令）放在**内部提示词缓存边界之上**；易变的每轮段落（UI 呈现、Control UI 嵌入指引、Messaging、折叠细节、语音、群聊上下文、React、Runtime、项目记忆事实、通道专属提示、委派/编排模式）**追加在边界之下**，以便本地后端的**前缀缓存**复用稳定工作区前缀。provider 插件也能注入「稳定前缀（缓存边界之上）」与「动态后缀（边界之下）」。
- **提示词模式**：`full` / `minimal`（子代理：去掉记忆段、模型别名、用户身份、输出指令、Messaging…）/ `none`（只留身份行）。
- **提示词快照进 CI**：`test/fixtures/agents/prompt-snapshots/codex-runtime-happy-path/` 固定了几类轮次（Telegram 私聊、Discord 群、心跳）的提示词层栈，改提示词要同 PR 更新快照，`pnpm prompt:snapshots:check` 在 CI 里查漂移。
- **工作区引导文件**：`AGENTS.md` / `SOUL.md` / `IDENTITY.md` / `USER.md` / `MEMORY.md` / `BOOTSTRAP.md` 来自配置的 agent 工作区；会话在别的工作目录/托管 worktree 里运行时，**只把该目录的 `AGENTS.md` 作为项目上下文追加**，不从执行目录加载其余身份文件。
- **根 `AGENTS.md` 的设计优先级之一就是这条**：「稳定会话上下文——重建过去的上下文会毁掉提示词前缀复用；保持生成的提示词/工具/上下文增补有界且确定，保留 transcript 字节，必需指令整段提供，**只有压缩才重写历史**」。

### 3.3 对比

| 维度 | agentxx | openclaw |
|---|---|---|
| 上下文归属 | 会话是唯一权威；图状态只留只读影子通道 | transcript（会话记录）+ 可替换上下文引擎；`commitTurn` 只对成功轮次推进引擎状态 |
| 装配过程 | 固定流水线（中间件 + 节点），无替换点 | 引擎槽位 + 明确的生命周期与方法契约，可整体替换 |
| 提示词拼装 | 每轮整体重建（含动态段） | 分层渲染 + **缓存边界**显式区分稳定段与易变段 |
| 压缩触发 | 75% 阈值 + 95%/失败 2 次硬截断 | 预算（窗口 − max(输出, buffer)）+ 溢出错误触发一次性压缩重试 |
| 压缩归属 | 中间件（单一实现） | 可声明 `ownsCompaction` 交给引擎；否则内置压缩与引擎 `compact()` 并存 |
| 压缩产物形态 | `[system] [user 指令] [assistant 摘要] [最近消息]` | 结构化滚动摘要 + 尾部 token 窗口；引擎可自定义（DAG 摘要、向量检索…） |
| 幂等/防重 | 压缩完成即落盘，避免重复压缩 | `commitTurn` 按 `advancementKey` 幂等，重试返回 `duplicate` |
| 提示词验证 | 无快照 | 提示词快照 fixture + CI 漂移检查 |
| 失败处理 | 压缩失败降级硬截断（保证请求能发出） | 引擎抛错→隔离并降级 legacy；宿主能力不满足→fail closed；锚点失效→本轮降级 |

### 3.4 两者的优缺点

**agentxx**

- 优点
  - **单一权威 + typed 上下文**：避免了「图状态里的 messages 与会话里的 messages 谁是新」的经典双写问题；影子通道只读、只暴露元信息，插件也能观察规模。
  - **压缩路径实战导向**：确定性压缩先削（去重、折叠探索、清噪），LLM 压缩同 threadid 同模型（命中 KV cache），失败还有硬截断兜底——三层降级保证「请求一定发得出去」。
  - **压缩即落盘**：这一条比「轮末统一落盘」在崩溃场景下更稳（不会反复压缩同一段）。
- 缺点
  - **装配过程不可替换、且不透明**：要换压缩策略/换召回实现只能改中间件本体，没有「换一个装配器」的入口；`assemble` 的产出（消息序列、估算 token）也没有一个统一的「本轮上下文报告」可用于诊断。
  - **提示词无缓存边界概念**：每轮把 `systemPrompt + 全部附加段 + 动态段` 整体重拼，动态段（时间、记忆、技能提示）夹在稳定段之后会让**前缀缓存**在动态段处失效；对长会话 + 支持 prompt cache 的 provider 是实打实的成本。
  - **没有提示词快照/回归**：提示词文本改动没有自动校验，容易「悄悄改变模型行为」。
  - **预算口径散落**：token 估算在压缩中间件里（`asciiCharsPerToken=4.0` 等经验系数），上下文统计另有一套，模型真上限来自模型配置——三处口径没有统一来源。

**openclaw**

- 优点
  - **装配是可替换且带契约的**：`assemble` 的返回结构（消息 + 估算 + 提示词增补 + 权威口径 + 投影模式）把「谁负责什么」写清楚了；`ownsCompaction` / `promptAuthority` 两个开关把「压缩归属」与「预检归属」解耦。
  - **降级策略分级**：引擎坏 → 隔离并降级；能力不满足 → 启动前拒绝；锚点失效 → 只本轮降级。三档处理让「插件坏了不能让用户失语」这条产品目标落地。
  - **幂等提交**：`commitTurn` 的 `advancementKey` 让重试安全，避免重试把同一轮重复写进上下文引擎。
  - **提示词缓存边界是一等设计**：稳定段在上、易变段在下，且对本地后端（前缀缓存）与云 provider 都成立；提示词快照 + 漂移检查把这层约束守住了。
  - **工作区身份文件与执行目录分离**：换目录干活时不会把「别人仓库的 `AGENTS.md`」误当身份文件加载。
- 缺点
  - **概念负担重**：9 个生命周期方法 + 3 个语义声明 + 2 个开关 + 宿主能力要求，写一个合格的引擎门槛不低；文档也承认「非自持引擎写空 `compact()` 是不安全的」这类陷阱。
  - **上下文正确性依赖声明**：`currentTurnFence` / `turnAdvancementIdempotency` 声明不对就静默退回 legacy，问题不容易被察觉。
  - **提示词分层是隐式契约**：段的「上/下边界」靠文档与 reviewer 记忆维护，段位置放错只会表现为「缓存命中率下降」，没有测试能直接抓住。

### 3.5 可迁移到 agentxx 的设计

| 编号 | 设计 | 落地位置 | 优先级 | 说明 |
|---|---|---|---|---|
| M11 | **给系统提示词划一条显式缓存边界** | `agent/lib/src/agent/context.cpp`（`buildSystemPrompt`）、`agent/lib/include/agentxx/agent/prompt.h` | P0 | 现状把动态段拼在稳定段之后。建议改为：`AgentPrompt` 增加 `stablePrefix`（systemPrompt + 静态附加段，按键序稳定）与 `dynamicSuffix`（每轮变化的段：当前时间、动态记忆、技能提示、环境快照）两组；`buildSystemPrompt` 输出 `stablePrefix + dynamicSuffix`，并在文档与注释里写明「动态内容一律不得插入 stablePrefix 之前/中间」。这条对支持前缀缓存的 provider 直接省 token。 |
| M12 | **补齐「上下文装配报告」** | `agent/lib/src/agent/context.cpp` + `wire_protocol.h`（`WireContextStats` 已有，扩展字段） | P1 | 每轮记录并上报：装配后消息数、估算 token、压缩是否发生、压缩前后 token、被裁掉的消息数、稳定前缀哈希。诊断长会话问题（尤其「为什么又压缩了」）时不用靠日志猜。 |
| M13 | **压缩触发口径统一** | `SummarizationMiddlewareHandle` + `AgentContext` | P1 | 把「模型上限」「估算系数」「预留输出 token」「压缩阈值/兜底阈值」收拢到一处（如 `ContextBudget` 结构），压缩、统计、预检都读它；消除三处口径不一致的可能。 |
| M14 | **压缩幂等键** | `SummarizationMiddlewareHandle::onModelcallRunFunc` | P2 | 现阶段靠「压缩完成即落盘」防重复压缩。可再补一个单调 `compactionGeneration`：同一段上下文只允许压缩一次，重试时若发现该段已被压缩则直接跳过（对应 openclaw 的 `advancementKey` 幂等）。 |
| M15 | **提示词快照回归** | `agent/test/core/`（新增模块） | P2 | 为默认提示词与几类典型拼装（有/无技能、有无 worktree、有无 planning）各存一份快照文本（或哈希），改动提示词文本时测试失败并要求更新快照。成本极低，能挡住「无意中改了提示词」。 |
| M16 | **上下文装配的可替换出口（评估后做）** | `agent/lib/include/agentxx/plugin/api/plugin_api.h` 的 `agentxx.agent.context` 表 | P2 | 现在该表只有 `get_messages` / `messages_count` 两个查询。若要支持「换装配策略」，可在同表扩展一个可选的 `assemble` 回调（默认不注册＝走内置流水线），并**明确契约**：返回的消息序列 + 估算 token + 是否自持压缩。注意 C ABI 边界上不宜传大对象，实际形态建议是「插件返回 JSON 描述 + 宿主按描述裁剪」，避免跨边界拷贝整段上下文。这项属研究性迁移，先不做。 |
| M93 | **会话轮次的内容来源标记（taint）** | `Session` 的 `ChatMessage` 扩展或独立元数据表；`appendSessionMessages` 统一打标 | P2 | openclaw 的 `turnTainted`：用户消息清除标记、工具结果可置位、assistant 落库前打标（见 §3.6）。用途是让「这一轮产出是否源自不可信的工具/外部内容」随消息走，供记忆写入（M33/M34）、审计（M52）与缓存决策消费。落地时**必须由宿主统一打标**，不能靠模型或工具自觉。 |

### 3.6 实现细读（本轮新增）

> 逐行读 `nodes/modelcall.cpp`（含 `repairMessages`、重试循环）、`middlewares/summarization.cpp`、`packages/agent-core/src/agent-loop.ts` 后补充。

#### agentxx：`repairMessages()` 是「发请求前的最后一道合法性闸门」

每次 LLM 调用前都会跑一遍（`modelcall.cpp::repairMessages`），共五类修复，每一类都对应一种真实故障：

| # | 修复 | 触发场景 | 细节 |
|---|---|---|---|
| 1 | 末尾角色修正 | 上下文以 assistant 结尾（如中断/异常后重新入图） | 追加一条 `user` 角色、`MessageFlag::AutoInserted` 的「continue」提示 |
| 2 | 悬挂 `tool_calls` 清理 | 取消/异常后「assistant 带 tool_calls 但没有对应 tool 结果」 | 遍历**所有** assistant（不只最后一条，历史中任意位置的残留都会污染请求）；无对应结果则清空该组 `tool_calls`，并**删除其孤儿 tool 结果**——因为孤儿结果会触发 OpenAI `invalid tool_call_id` / Anthropic `orphan tool result` 400；删除按倒序避免索引位移 |
| 3 | `tool_call_id` 去重 | 模型重试时复用同一 id，或 provider 自动回填的 `call_{i}` 与模型给出的 id 冲突 | 用全局自增序号生成新 id（并避开所有已见 id 防碰撞），再按**声明顺序**把对应 tool 结果（第 k+1 个结果对应第 k 个调用）回填；边界情况（只有 1 个结果却出现重复 assistant）保持原样不错配 |
| 4 | 连续 user 合并 | 重试/中断/客户端竞态产生 `user`+`user` | Anthropic 与部分兼容网关会直接 400；合并策略：`content`/`reasoning_content` 以 `\n\n` 拼接、多模态数组追加、`tool_calls` 追加、`flags` 按位或、`extra` 对象字段级合并（后者覆盖） |
| 5 | 空消息与 UTF-8 校验 | 任何来源的脏消息 | 空内容清理 + UTF-8 校验 |

修复后的上下文**整体覆盖回会话**（唯一权威），且**不发 UI 消息增量**——注释写明这与旧实现「state.overwrite 不发通道事件」语义一致：修复属纠错，不是新内容。系统消息则单独处理：首条已是 `system` 就**就地更新内容**（承载每轮重建的系统提示词），否则在开头插入；且明确不请求节流落盘（派生内容，由轮末权威保存落库）。

对照 openclaw：它把「参数修复」抽成独立包 `tool-call-repair`，而**消息级修复**分散在 attempt 阶段。就「历史合法性」这一维度，agentxx 的集中式修复反而更完整、更容易测试——这是一处可以反向输出的优势（本文 §6.5 原先只写了「参数自愈」，低估了这层）。

#### agentxx：LLM 重试循环的四个不变量

`modelcall.cpp::baseRun` 的重试循环（`do { … } while(true)`）有几条不变量值得记下：

- **失败必须留一条「合法收尾」消息**：重试耗尽或取消时，若末尾不是 assistant、或末尾 assistant 仍带 `tool_calls`（悬挂），就插入一条 `AutoInserted` 的 assistant 兜底消息（内容 = 已收到部分输出 + 异常/取消提示）。目的是让本轮以 `agent_end` 结束，**而不是把悬挂的 tool_calls 误路由回 tools 节点重复执行**。
- **部分输出不丢**：流式 token 会累积到 `graphDataKey_tempLLMContent` / `tempLLMThinking`；失败时若 `thinking+content >= 512` 字符，就把这部分保留成 assistant 消息再重试。重试不重置计数（注释明确：重置会导致无限重试与消息堆积）。
- **退避是确定的**：第 n 次重试等待 `n*3` 秒；若错误文本命中限速标记再加 `n*5` 秒。日志、UI 提示、定时器**共用同一个 `delaySec`**（注释强调三者必须一致，否则提示的等待时长与实际不符）。
- **两处取消埋点**：进入 LLM 调用前与工具执行前各自 `throw_if_cancelled`，因为之前的 `co_await`（权限询问、重试等待）可能已经处于取消状态。

另外两处协议健壮性处理：provider 未返回 `tool_call.id` 时（如部分 Ollama 兼容实现）**合成唯一 id**；`record_usage(in.ctx, completion)` 把 usage 折进引擎的 `UsageAccumulator`，同时把 `usage.total_tokens` 写进 `graphDataKey_LLMTokenUsage` 供压缩与上下文统计使用。

#### agentxx：上下文统计与压缩共用「API usage 优先」的取值口径

`summarization.cpp::onModelcallRunFunc` 开头即确立用量取值顺序：

```
apiTokenUsage   = graphData 里的上次 API 返回 total_tokens（可能偏旧/不精确）
countTokenUsage = 本地估算（ascii/4.0 + unicode/1.1 + 多模态按 400 token/项 + 每条 3 token）
tokenUsage      = apiTokenUsage > 0 ? apiTokenUsage : countTokenUsage
```

注释诚实写明「接口返回的 token usage 可能不准确」的原因（重试会额外附加消息、可能是上一轮 API 的返回值、本轮已新增 toolcall/user 输入）。取值后写入 `session->contextStats`（`contextTokens` / `maxContextTokens`）供 UI 显示占用百分比——也就是说**上下文占用百分比是「API 优先 + 估算兜底」的口径**，这比本文 §8 原先写的「有 token 统计」更精确。`ModelConfig::modelContenxtMaxToken > 0` 时才覆盖默认上限（默认 256K）。

估算函数本身也有一处实现讲究：**按 UTF-8 前导字节逐字符分类**（0xF8+ 视为无效前导按单字节计，避免吞掉后续字节），然后 **ascii 与 unicode 各自除自己的系数后再相加**——注释说明「先相加再整体截断会高估」。

#### openclaw：`turnTainted` 是「写入侧信任」的运行时表达

`runLoop` 里 `turnTainted` 的更新规则只有三条：用户消息提交时清零、工具结果可置位、assistant 消息落库前统一打标。它把 `docs/concepts/memory-architecture.md` 里「写路径就是安全边界」这条原则落在**消息级元数据**上：后续任何消费方（记忆整理、审计、缓存）都能问「这段内容是否源自不可信工具结果」，而不必回溯分析内容本身。

**对本文结论的影响**：M34（记忆条目标注来源）与新增的 M93 是同一思路的两个层次——M34 管「记忆条目」，M93 管「轮次产出」。建议一起做，并遵守同一条纪律：**标记由宿主写入，模型/工具无法自证可信**。

---

## 4. 工作区、沙箱与多设备节点

### 4.1 agentxx 现状

**工作目录解析**（`AgentConfig::workDir` + `AgentContext::resolveWorkDir`）：

- 优先级：**会话绑定的 worktree**（若有）> 会话工作目录覆写 > agent 配置 `work_dir` > 进程 cwd；取不到时返回空串。
- 会话级覆写经 `AgentConfig` 的 `sessionWorkDir` 段（按 sessionId 取值，详见 §17 配置）。
- 系统提示词里的 `${work_dir}` 读的是**不含 worktree 绑定**的工作目录——「进出 worktree 不改变提示词」，避免上下文抖动（这是显式设计决定）。
- 路径卫生：中文/非 ASCII 路径一律经 `utilxx_base::pathToUtf8Generic` / `utf8ToPath` 转换（Windows 本地代码页会乱码）。

**git worktree 隔离**（`GitWorktreeTool`，`worktree.enable` 开启后注册）：

| 能力 | 行为 |
|---|---|
| `create` | 在 `{repoRoot}/.agentxx/agent/worktrees/{name}` 创建独立 worktree（含独立分支），成功后**把当前会话的相对路径解析基准切到 worktree** |
| 绑定语义 | 绑定后所有文件操作与命令执行自动落在 worktree 内；权限规则同步切换（主检出写 DENY、worktree 读写 ALLOW） |
| `info` / `status` | 当前绑定状态、仓库全部 worktree 列表（含脏标记）、当前工作区摘要（未提交变更/领先提交） |
| `remove` | 删除指定/当前 worktree；存在未提交工作时需显式确认 |
| 子代理继承 | 派生时把 worktree 路径预置为子代理的解析基准（`inheritedWorktreePath` 标记），整条工具链自动落入同一 worktree |
| 权限隔离 | worktree 子树放行，**读主检出不受限**；隔离优先于白名单/模式默认规则 |

**沙箱**：agentxx 没有通用沙箱概念。隔离手段是三层：插件声明的权限目标 + 宿主权限中间件（含 worktree 写边界）+ 工具自身对路径的处理。没有「文件系统命名空间隔离」「容器化执行」这类强隔离。

**多设备**：没有节点/远程执行概念。远程能力仅有「客户端连服务端」（同一台机器或经网络的 UI 接入），不支持把「文件读写/命令执行」派发到另一台设备。

### 4.2 openclaw 现状

**三个不同层次的位置概念**，文档把它们的区别写在显眼处：

| 概念 | 路径/位置 | 说明 |
|---|---|---|
| 配置与状态目录 | `~/.openclaw/`（可被 `OPENCLAW_STATE_DIR` / `OPENCLAW_PROFILE` 改写） | 配置、凭据、会话数据库 |
| 工作区（workspace） | 默认 `~/.openclaw/workspace` | agent 的「家」：文件工具的默认 cwd + 工作区上下文来源（`AGENTS.md`/`SOUL.md`/`USER.md`/`IDENTITY.md`/`BOOTSTRAP.md`/`memory/`/`skills/`）。文档明确警告：**它是默认 cwd，不是硬沙箱**，绝对路径仍可达主机任意位置 |
| 沙箱工作区 | `~/.openclaw/sandboxes` 下 | 开启 `agents.defaults.sandbox` 且 `workspaceAccess != "rw"` 时，工具在沙箱工作区里操作 |
| 执行目录 | 会话运行所在的目录 / 托管 worktree | 该目录的 `AGENTS.md` 作为**项目上下文**追加；但**不从执行目录加载身份文件**（`SOUL/IDENTITY/USER/MEMORY/BOOTSTRAP`） |

**设备节点（nodes）是一等公民**：

- 节点（macOS/iOS/watchOS/Android/headless）以 `role: "node"` 连同一个 Gateway WS，在 `connect` 帧里声明 **caps / commands / permissions**。
- 命令面按平台默认 allowlist 治理，危险命令需显式 opt-in（`gateway.nodes` 配置 + `docs/nodes/command-policy.md`）；另有 `exec` 的节点绑定（把命令指到某个节点上执行）。
- 节点可以**托管会话**（session hosting）、发布 MCP server 与技能、做本地模型推理、做设备命令（widget 面板/相机/录屏/定位/短信）；文件传输插件负责目录列举/取回/写入。
- 配对与信任：所有 WS 客户端（操作员与节点）都要带设备身份；新设备需配对批准并发设备令牌；本地回环可自动批准；tailnet/LAN（含同机 tailnet）仍需显式批准；连接必须签 `connect.challenge` nonce，签名载荷 v3 还绑定 `platform` 与 `deviceFamily`；重连时网关固定已配对元数据，元数据变更要走「修复配对」。
- 沙箱：`agents.defaults.sandbox`（+ 每 agent 覆盖），沙箱种子只接受工作区内的常规文件，指向工作区外的符号链接/硬链接别名会被忽略。

### 4.3 对比

| 维度 | agentxx | openclaw |
|---|---|---|
| 工作目录 | 会话级覆写 > 配置 > 进程 cwd；解析点唯一（`resolveWorkDir`） | 状态目录 / 工作区 / 沙箱工作区 / 执行目录四个概念分开；工作区可被 `OPENCLAW_WORKSPACE_DIR`、profile、state-dir、每 agent 条目多层覆盖 |
| 身份文件 | 无（提示词文本内建，插件可追加段） | 工作区里的 `AGENTS.md`/`SOUL.md`/`USER.md`/`IDENTITY.md`/`MEMORY.md`，且有「执行目录不加载身份文件」的明确规则 |
| 隔离 | git worktree（写边界）+ 权限规则；无强沙箱 | 可选沙箱（沙箱工作区 + 种子限制）；文件工具默认 cwd 只是「默认」不是沙箱 |
| 并行开发隔离 | worktree 强隔离，且提示词不随进出变化 | managed worktrees（`docs/concepts/managed-worktrees.md`，56 KB）——比 agentxx 的 worktree 更重的一套 |
| 远程/多设备 | 无 | 节点（caps/commands 协商）+ 设备配对 + 令牌 + 签名 nonce + 命令策略 allowlist |
| 命令执行位置 | 始终在本机 | 本机或经节点绑定把 exec 指到已配对设备 |

### 4.4 两者的优缺点

**agentxx**

- 优点
  - **worktree 语义干净**：绑定即切换解析基准 + 权限边界同步切换，且明确「提示词不随 worktree 变化」避免上下文抖动；子代理继承路径使并行开发链路完整。
  - **单点解析**：`resolveWorkDir` 是唯一取值口，避免了「有的地方读配置、有的地方读 cwd」的分裂。
  - **路径编码处理细致**：跨平台路径统一 UTF-8，Windows 代码页问题显式解决过。
- 缺点
  - **没有强隔离**：没有沙箱/容器，工具直接在用户机器上以用户权限执行；安全边界全靠权限中间件与工具自觉。
  - **没有多设备概念**：无法把「跑在另一台机器上的命令/文件操作/设备能力」纳入统一工具面；这在「我的电脑不在身边」的场景下是硬缺口。
  - **工作目录的「一致性」缺少跨端校验**：远程客户端与服务端各自有工作目录概念，客户端展示的工作目录来自服务端 HelloAck（方案见 UI 文档），但没有「双方不一致时如何提示」的显式流程。

**openclaw**

- 优点
  - **位置语义分离清楚**：配置目录 / 工作区 / 沙箱工作区 / 执行目录各自的职责与加载规则写得很细，避免「在别人仓库里干活时误加载了身份文件」这类事故。
  - **设备能力是协议级概念**：节点在握手时声明 caps/commands，网关可以据此做准入与展示；设备身份 + 配对 + 令牌 + nonce 签名构成完整信任链，且对本地回环有例外（体验优先）。
  - **危险能力显式 opt-in**：平台默认 allowlist + 危险命令单独开关，符合它「安全是产品权衡、要给出清晰旋钮」的定位。
- 缺点
  - **概念多、覆盖层次多**：工作区就有 4 条覆盖规则（profile / `OPENCLAW_WORKSPACE_DIR` / state-dir / 每 agent），文档要专门澄清「旧目录不会被合并」。
  - **沙箱是可选且被文档反复提醒「默认不是沙箱」**：默认安装下工作区并不隔离，用户容易误以为「有 workspace 就安全了」。
  - **managed worktrees 的体量**：单篇文档 56 KB，说明这套机制本身复杂度不低（对编码代理场景，agentxx 的轻量 worktree 反而更容易理解与维护）。

### 4.5 可迁移到 agentxx 的设计

| 编号 | 设计 | 落地位置 | 优先级 | 说明 |
|---|---|---|---|---|
| M17 | **把「默认 cwd ≠ 沙箱」写清楚，并给一个工作区外的写边界** | `docs/zh-cn/design/index.md` + `permission.h` | P1 | agentxx 的权限中间件已有工作目录隔离，但没有把「默认工作目录不是安全边界、绝对路径可越界」这条写进面向用户的说明。建议：① 文档明确写出当前边界；② 给一个「工作区外写操作必须询问（默认）」的兜底规则（若尚未有），让默认行为与 openclaw 的「least restrictive effective safeguard」取向一致但不留空白。 |
| M18 | **轻量设备能力协商（不做完整节点体系）** | `wire_protocol.h`（`HelloAck` 扩展 capabilities 列表）+ `agentxx.client.*` 表 | P2 | openclaw 的节点体系对 agentxx 太重，但「客户端在握手时声明自己支持什么」这一半是现成的（已有接口集上报）。可扩展为：客户端声明「我能做本地文件选择 / 我能做本地截图 / 我能在本机执行命令」，服务端据此决定是否展示/启用对应工具的本地路径。收益：远程模式下把「文件选择走服务端」这类特例（见 §12/§13）泛化成一个通用机制。 |
| M19 | **命令执行的「位置」字段** | 工具参数（`execute_command` 等） | P2 | 与 M18 配套：声明客户端支持本机执行后，工具可带 `target: local|server` 参数。不改 ABI，只加参数与权限声明；服务端在接受 `local` 时校验该客户端确实声明了该能力。 |
| M20 | **身份文件与执行目录分离的显式规则** | 已是现状（agentxx 无常驻工作区身份文件），无需迁移 | — | agentxx 的提示词是内建 + 插件追加段，天然没有这个问题；但若将来支持「工作区里放 `AGENT.md` 自动注入」，必须同时定下「执行目录不加载身份文件」这条。 |

---

## 5. 持久化、状态与崩溃恢复

### 5.1 agentxx 现状

**存储布局**：每个会话一个 SQLite 库，路径 `{dataDir}/sqlite/sessions/{sanitizedSessionId}/session.db`（`dataDir` 由客户端经 `AgentConfig::dataDir` 重定向；取不到用户主目录时回退系统临时目录）。

| 表 | 形态 | 写入时机 |
|---|---|---|
| `view_message` | 追加式，每消息一行 JSON（含 `msg_id` 列与索引） | 消息产生时追加；内容后变的消息（tool 结果回填、折叠）按 `msg.id` 更新行 |
| `llm_context` | **单行整体替换** | **每轮对话结束时**保存（`saveLlmMessages`） |
| `meta` | `msgIdCounter` / 会话元数据（sessionId、title、lastActiveMs） | 追加消息时与消息同事务提交 |
| `store` | share store KV（自增 `id` → value） | 工具写入时；内存只留最近 3 条（LRU），其余按 id 读回 |

配套机制：

- **连接 LRU**：同时打开的会话库连接最多 32 个（每个连接占 fd + WAL + page cache；Linux 默认 `ulimit -n` 常为 1024），超出按 LRU 关闭，下次写入自动重开。
- **会话列表分页**：两阶段扫描——先 `stat` 各目录 `session.db`/`-wal` 的 mtime 得到近似顺序（**只作读取顺序启发，绝不据此跳过目录**），再按序打开 DB 读精确 meta；游标是 `(lastActiveMs, sessionId)` 的 keyset，规避 offset 分页在活跃会话位移时的重复/遗漏。
- **图 checkpoint**：`SingleCheckpointStore` 每个 thread **只保留最新一个 checkpoint**（`save` 时淘汰历史 checkpoint 与其上挂载的 pending writes），把开销从 O(super-steps) 降到 O(threads)。代价写明：`list`/`get_state_history` 最多返回一条，fork/按历史 id 恢复不可用。
- **写入节流**：UI 展示侧由 EventBridge 展开图事件为 view 消息增量，并「请求节流落盘」，不是每条消息一次事务。
- **压缩即落盘**：压缩结果立刻写回会话上下文并落盘（见 §3）。
- **全局设置库**：`settings_db`（agentxx 侧 KV，供 TUI 设置项持久化）。
- **崩溃后的状态**：`live` 侧（delta 环形缓冲、消息队列、会话内 worktree 绑定、权限已记住的选择中的会话部分）**不持久化**；重启后从 `session.db` 恢复展示历史与上下文、从 checkpoint 恢复图状态（仅最新）。

### 5.2 openclaw 现状

**两个数据库角色**：

| 库 | 位置 | 内容 |
|---|---|---|
| 共享状态库（state DB） | 状态目录下 | 控制面状态：通道、设备配对、会话能见度、信号日志、迁移回执（`migration_runs`/`migration_sources`）、插件状态等 |
| 每 agent 一个 agent DB | 每 agent 数据目录下 | agent 数据：会话、transcript、消息、用量等 |

版本与迁移契约（`docs/reference/database-schemas.md` + 根 `AGENTS.md`）：

- 每个库带 schema 版本（`package.json` 的 `openclaw.schemaVersions`：`state: 19`、`agent: 23`）；**打开时向前迁移**；更老的构建**拒绝**更新 schema 写出的库。
- 「schema 版本 / 完整性 / canonical index / 表存在性」检查**只属于打开与准入**，运行路径必须**携带已准入的 schema 事实**，不得每条 SQL 重新查询；缓存新鲜度用 `PRAGMA data_version` 探测外部提交。
- CI 跑 `scripts/check-native-state-schema-version.mjs`，Swift 与 TS 两侧的 schema 版本声明不一致就失败。
- 文件到 SQLite 的迁移归 `openclaw doctor --fix` 所有，每次迁移在共享库里记一条回执；**运行时不新增独立的兼容读取路径**。
- 更新路径要求：改 schema 必须带迁移；更新器**先**做 schema 支持预检，schema 提升前做「已验证的备份」，并有回滚策略。

访问与并发纪律（根 `AGENTS.md` 的 "Runtime and code safeguards"）：

- **数据库访问在 worker 线程**，网关主线程只 await 结果并安装已发布事实；只有四种同步例外：启动准入、迁移、Doctor/CLI 一次性命令、锁原语。
- **写事务是同步的**：先完成异步规划 → **再复读权威行** → 才写；事务回调里禁止 `Promise`/`await`。
- **写者租约**：轮次准入时记录持久化的 `activeWriterRunId`；此后每次 transcript 追加或重写都带 `expectedWriterRunId`，**同步提交事务校验它仍等于当前活动声明**——被取代的轮次无法提交陈旧数据。SQLite writer 队列按 agent 排序变更，状态目录锁防止第二个网关或 `openclaw agent --local` 并发占用同一状态目录。
- **网关调度器**：所有者的截止时间存在各自的 store 里，启动时由所有者重建调度，**不持久化第二份调度器状态**；`beginClose()` 关调度准入并取消待唤醒，`stop()` 才 join 已在跑的回调。

**会话状态信号日志（`session_state_events`）**——这是它独有的一层：

- 被观察的会话发生实质变化时追加一条**类型化事件**（`created` / `human_direct_message` / `upstream_missing` / `goal_changed` / `child_spawned` / `run_completed` / `run_failed` / `compacted` / `adopted`），只带元数据与一句话摘要，**从不带消息内容**；每条事件标注操作者（`human`/`agent`/`system`）。
- 会话的 **state version = 日志里最大序号**，由持久化的 per-session head 维护（裁剪后仍存活）。
- **观察者（watcher）**持 per-target 游标（`session_watch_cursors`）：父会话派生时自动 seed；群/房间/通道会话在首次人类回合后被主会话环境观察；显式 `sessions_send watch: true`。
- **notice 只发一条**：同一 watcher/target 只有一个待处理通知，文本字节稳定（系统事件队列按内容去重）；游标在通知入队时**冻结水位**，之后的变化只推进 material watermark 不再重复通知；通知被消费时游标前进，若有交错事件则**只重开一条**新通知；自身造成的事件不通知自己；**重启后由持久游标重新物化**待处理通知。

### 5.3 对比

| 维度 | agentxx | openclaw |
|---|---|---|
| 库划分 | 每会话一个 `session.db`（含 view/llm/meta/store 四表）+ 全局设置库 | 共享状态库 + 每 agent 一个 agent DB |
| 上下文落盘 | 单行整体替换，**每轮结束**写一次 | transcript 追加 + 写者租约逐次提交；运行路径携带已准入 schema 事实 |
| 并发写保护 | 同一会话只有一条协程在写（控制器模型），加内部互斥锁 | 显式**写者租约**（`activeWriterRunId`）+ 事务内校验 + 状态目录锁 |
| 事务纪律 | 单条语句 + 事务（消息与计数器同事务） | 写事务同步、先规划后复读再写、事务内禁 await |
| 访问线程 | 调用发生在 agent io 线程，锁仅在多线程并发时生效 | 一律下沉 worker 线程，主线程只 await |
| 会话内事件序列 | 无（会话内没有可重放的事件序列；跨连接重放靠内存 delta 缓冲） | `session_state_events` + 游标 + notice 合并/冻结水位/重启重物化 |
| 迁移 | 幂等建表 + 老库 `ALTER` 回填（如 `msg_id`） | 版本化迁移 + 打开时前向迁移 + 老构建拒绝新库 + CI 校验双端版本一致 + doctor 拥有文件→SQLite 迁移 |
| 调度持久化 | 无独立调度器 | 截止时间随 owner 存，启动重建，不存第二份调度状态 |
| 备份/回滚 | 无（数据目录可整体备份） | 更新前已验证备份 + 回滚策略 + 每库快照与异地副本 |

### 5.4 两者的优缺点

**agentxx**

- 优点
  - **按会话分库**带来天然隔离：删会话＝删目录，会话级 WAL 写入按会话串行；连接 LRU 上限避免了 fd 爆炸。
  - **checkpoint 只留最新**是一个漂亮的空间优化：把 neograph 语义下用不到的 fork/时间旅行砍掉，换 O(threads) 的存储开销，并在注释里写明安全性依据。
  - **会话列表分页**考虑到了「mtime 只作顺序启发、绝不据此跳过目录」这类真实陷阱。
- 缺点
  - **缺少会话内事件序列**：跨连接重放依赖内存 delta 环形缓冲，进程重启即失；也无法回答「这个会话自某个版本以来发生了什么变化」。
  - **并发写保护依赖「只有一个写者」这一隐含前提**：目前成立（单控制器单协程），但一旦引入第二个写者（如后台压缩子代理跨会话写、或将来多进程），没有任何显式约束拦住它；openclaw 的写者租约正是为这种「多入口写同一会话」的形态准备的。
  - **迁移只有一个方向**：没有 schema 版本概念，只有「幂等建表 + 必要的 ALTER 回填」；未来若要做列语义变更/表重构，缺少「版本 + 前向迁移 + 老版本拒绝」这套骨架。
  - **数据库访问在 io 线程**：`SessionStore` 内部有互斥锁，且调用点在 agent io 线程，短事务下没问题；但一旦出现大结果写入（长 tool 输出落盘）会占用 io 线程（协程模型下会阻塞同线程其他会话的推进）。

**openclaw**

- 优点
  - **写入权威有明文不变量**：写者租约 + 事务内复读 + 禁 await 事务，三条一起把「多入口写同一会话」这个真实问题钉死。
  - **schema 版本契约完整**：前向迁移、老构建拒绝、双端版本一致性 CI 校验、迁移回执落库——属于「能长期演进的库」该有的样子。
  - **会话状态信号日志**解决了一个被大多数 agent 忽视的问题：多个会话/多个人同时动同一份工作时，「我基于的假设已经过期」这件事需要被检测并通知一次（且只通知一次）。
  - **调度状态不重复持久化**：截止时间随所有者存，启动重建，避免两处调度状态不一致。
- 缺点
  - **机制重**：worker 线程 + 租约 + 准入事实 + 版本迁移 + 通知去重，理解与调试成本高；文档要写「现有同步主线程访问属遗留，别再新增」这种迁移规则。
  - **事件不重放**：客户端必须实现「gap 后刷新」路径，实时流与补齐流两套语义。
  - **每 agent 一个 DB** 在多 agent 场景下带来跨库查询复杂度（这也是它写「两个数据库角色」文档的原因）。

### 5.5 可迁移到 agentxx 的设计

| 编号 | 设计 | 落地位置 | 优先级 | 说明 |
|---|---|---|---|---|
| M21 | **写入权威的显式断言（轻量版写者租约）** | `Session` 增加 `writerEpoch`（单调计数）+ `AgentContext::beginTurn` 记录本轮 epoch；`SessionStore::saveLlmMessages` 接受并校验 `expectedWriterEpoch` | P1 | 现在靠「一个会话一条协程」的隐含前提。加一个 epoch 后：若同一会话出现第二个写者（例如将来后台任务直接写会话），陈旧写会被拒绝并记日志，而不是静默覆盖。成本极低（一个 uint64 + 一处校验）。**实现顺序参考 openclaw 的三步（§5.6）**：先取快照并校验会话身份 → 原子地把 `activeWriter` 改成自己并**回读确认已持久化** → 之后才允许把前任标记为作废；判定的失败**不得**影响仍持有权威的那一方。 |
| M22 | **会话级 schema 版本与迁移骨架** | `agentxx/util/sqlite.h` + `SessionStore::ensureSchema` | P1 | 现在只有幂等建表。建议：`meta` 表存 `schema_version`，`ensureSchema` 改为「读版本 → 依次跑迁移函数 → 写回版本」；老版本代码遇到更高版本**拒绝打开**并给出明确错误（而不是按老表结构读出错数据）。这是将来改表结构的前置条件。 |
| M23 | **会话内事件序列（最小实现）** | `SessionStore` 新增 `session_event` 表（`seq / kind / actor / summary / atMs`，不含消息内容） | P2 | 用于三件事：① 断线重连的「事件补齐」（现在只能靠内存 delta 缓冲，重启即失）；② 会话状态变化的可审计轨迹；③ 将来多端/多写者场景的「你落后了」提示。注意口径与 openclaw 一致：**只记元数据与一句话摘要，不复制消息内容**。 |
| M24 | **避免长耗时写入阻塞 io 线程** | `SessionStore`（大文本写入路径） | P2 | 现状所有 DB 访问在 agent io 线程。可先做最简改动：对超过阈值的写入（如超大 tool 结果、完整上下文）走一次性小线程或线程池，写完用 `asio::post` 回到 io 线程更新内存状态；不必照搬 openclaw 的完整 worker 体系。**注意**：`llm_context` 是单行整体替换（§5.6），每次节流落盘都会重写整段上下文 JSON——这条既是本项动机，也说明优化时要优先考虑「增量写」而不是只搬线程。 |
| M94 | **取消/异常时立即产出合法 transcript 尾部** | `ToolcallWrapNode` 的取消路径 + `AgentRunner` 的取消分支 | P2 | 现状是**惰性**修复：悬挂的 `tool_calls` 与孤儿 tool 结果由下一次请求前的 `repairMessages` 清理（§3.6）。openclaw 的做法是**即时**：中止时先落一条失败 assistant 消息并补齐事件序列，让会话后处理（压缩、续跑、导出）可以假设「尾部合法」。建议在取消/异常收尾处补一条合法 assistant 收尾消息（`modelcall` 已有同形逻辑，工具执行被取消的路径缺这一条）。 |
| M25 | **`data_version` 式的"外部改动"探测（评估后做）** | `SessionStore` | P2 | 当出现「两个进程访问同一会话目录」的场景（多客户端各起一个 agent 指向同一 dataDir）时，用 `PRAGMA data_version` 探测外部提交，避免缓存与磁盘不一致。当前单进程模型下不急。 |

### 5.6 实现细读（本轮新增）

#### agentxx：落盘是「轮内节流 + 轮末权威」两级，且节流窗口只有约 3 秒

`runTurnAsync` 的收尾与 `EventBridge` 的分工（`base_agent.cpp` + `summarization.cpp`）实际是这样的：

| 时机 | 动作 | 说明 |
|---|---|---|
| 轮内、按消息结算 | `EventBridge` 请求「节流落盘」（`Session::requestSaveLlmMessages`） | 进程在轮内被杀最多丢一个节流窗口（注释写明 <3s）的增量 |
| 压缩完成后立即 | `replaceMessages()` 后立刻请求节流落盘 | 重启后不会因上下文重新超限而反复压缩 |
| 中断处理完成、resume 前 | `onBeforeResume` → `requestSaveLlmMessages()` | 保证 resume 前上下文已按节流窗口落盘 |
| 轮末 | `saveLlmMessages()` + `flushViewMessages()` | **权威终态同步**：补齐节流窗口内尚未落库的 view 消息操作（含刚插入的轮次统计提示） |

也就是说本文 §5.1 写的「`llm_context` 每轮结束保存」只对了一半——**轮内还有一层按消息结算的节流落盘**，两级合起来才能解释「崩溃最多丢几秒」这个实际保证。

两处实现级注意点：

- **`llm_context` 是单行整体替换**：每次保存都把完整上下文 JSON 重写一遍。长会话下这是实打实的写放大（支持 M24「避免长耗时写入阻塞 io 线程」，也说明 M23「会话事件序列」不该按消息粒度再叠一份全量拷贝）。
- **已有一次「就地迁移」**：`SessionStore::ensureViewMessageMsgIdColumn` 会对老库做幂等 `ALTER` + 回填 `msg_id`。这说明 M22（schema 版本骨架）不是从零开始，而是把「已经存在的手工迁移」规范化成「版本 + 迁移函数链 + 老版本拒绝」。

另外 `SingleCheckpointStore` 与中断还有一个配合点（§2.6）：中断处理**之前**会把 `graphData` 写进 state 的 `channel_savedGraphData`，轮末再清除；进程在中断处理期间被杀时，重启后靠这份数据重建中断并继续，而不是丢掉整轮。

#### openclaw：写者租约是「先取快照 → 原子认领 → 回读确认 → 才能作废前任」

`claimAgentSessionWriter`（`src/agents/embedded-agent-runner/run/session-bootstrap.ts`）的四步顺序是这段设计里最值得照搬的部分：

1. **取快照**：`loadSessionEntry({ readConsistency: "latest" })` 拿到持久化会话条目（含 `sessionId` / `lifecycleRevision` / `activeWriterRunId`）。
2. **准入断言**：`resolveAgentHarnessRunAdmissionError` 校验运行时/harness 能否接管这个会话；随后**比对 `entry.sessionId === params.sessionId`**，不一致直接抛 `Session changed before writer admission`。
3. **原子认领 + 事务内复验**：`updateSessionEntry` 的回调里**再次**校验 `sessionId` 与 `lifecycleRevision`（不一致抛 `Session changed before writer claim commit`），然后把 `activeWriterRunId` 改成自己；之后**回读确认**真的写进去了（`claimed.activeWriterRunId !== params.runId` → 抛 `Session writer claim was not persisted`）。
4. **作废前任**：只有在自己已经拥有持久化行之后，才 `supersedeEmbeddedAgentRunByRunId(previousWriterRunId, …)`（用前任的 `lifecycleGeneration` 作为代际标记）。源码注释一句话总结了顺序约束：**「替代者必须先拥有持久化行，前任才可以被终结；认领失败时保持原权威 run 不受影响。」**

围栏的消费侧：`expectedWriterRunId` 被**逐调用点**传进所有 transcript 追加/重写路径（实测出现在 `run-session-target.types.ts`、`cli-runner/*`、`compact.queued-execution.ts`、`cli-backend-dispatch*` 等处），由同步写事务在提交时校验它仍等于当前活动声明。

**对本文结论的影响**：M21 的落地顺序有了明确参照（见 M21 说明）；同时说明「写者租约」不是加一个字段那么简单，**关键在于「认领成功才允许作废」与「提交时复验」这两条顺序约束**——这也是它排在 §19 第 3 批（运行语义）的原因。

---

## 6. 工具系统

### 6.1 agentxx 现状

**工具基类**（`XXToolBase`，以及包装外部工具的 `XXToolWrap`）带四个可配开关：

| 开关 | 语义 |
|---|---|
| `autoSummaryOutput` | 输出超过 `toolcallSummaryLimitOutputLength` 时**保真落盘 + 返回预览**（详见 §6.6 实现细读）：完整内容写入 share_store 并返回「store id + 总行数 + 显示/隐藏行范围」的预览头，模型可用 `agentxx_share_store` 按 `line_offset`/`line_limit` 分页取回；另可用 `SummarizationToolHandle` 做逐工具定制 |
| `canDelayLoad` | 延迟加载：初始只在 system prompt 里留名字与简短描述，由 `tool_skill_search` 子代理检索命中后才加载完整定义并允许调用 |
| `maxRetry` | 执行抛异常时重试，最多 `1 + maxRetry` 次 |
| `repeatCallCheck` | 同一 `llm ↔ tool` 交替链内连续多次（默认阈值 5）相同 tool + 相同参数调用时，经权限总线**询问用户**；用户确认才继续 |

**调用链**（`ToolcallWrapNode`）：

- 由中间件栈包裹（节点 `start → handle 钩子 → baseRun → end`，并有 `onHandleStartError` / `onHandleBaseRunError` 两条错误分派路径）；`interceptOrdinaryError_` 决定普通异常是重抛结束本轮，还是拦截成「工具错误消息」让 agent 继续。
- `findConsecutiveRepeatCallKeys()` 做循环检测：从最新 assistant 消息向前**只允许** `assistant(带 tool_calls)` 与 `tool` 结果交替，遇到 user/system/纯文本 assistant 即断开；回溯有上界（最多 threshold 条 assistant）且带提前终止。
- `autoFixArgsType()` 按工具 JSON Schema 做**参数类型自愈**（string↔数组/数值/布尔、单元素数组解包等，仅当目标类型不含当前类型时转换）。
- 同轮多个 tool_call **串行 `co_await` 执行**。
- 错误约定：参数检查失败抛 `std::invalid_argument`、运行期错误抛 `std::runtime_error`，由节点统一格式化为 `[Exception aborted: <msg>]` 结果文本；**不返回 `{"error": ...}` 形态**。取消上报 CANCELLED。
- 取消传播：`getSessionCancelToken()` 从参数里的 `sessionId` 取会话取消令牌交给工具，工具可轮询或传播到传输层。

**工具来源**：内置 C++ 工具（filesystem / execute_command / websearch / planning / math / codegraph / rag_search / …）+ 插件注册的工具（`tool_registry.h`，插件经 `agentxx.agent.tools` 表注册与注销，并可用 `agentxx.agent.permission` 表声明权限目标）+ MCP 工具（经 `XXToolWrap` 包装即获得上述四开关能力）。

**工具与提示词**：`AgentPrompt::toolPrompt`（按工具名覆写 `depict` 与各参数描述）是提示词侧的可定制入口；延迟加载工具只在提示词里保留名字与简短描述。

### 6.2 openclaw 现状

**工具面极大**（`src/agents/tools` 约 150 个非测试文件，加上层 200 余个 `agent-tools.*` / `bash-tools.*` / `tool-*` 文件），按类别划分：

| 类别 | 代表工具 |
|---|---|
| Runtime | `exec`、`process`、`terminal`、`code_execution` |
| Files | `read`、`write`、`edit`、`apply_patch` |
| Human input | `ask_user`、`secrets` |
| Web | `web_search`、`x_search`、`web_fetch` |
| Browser | `browser` |
| Operator UI | `screen`、`theme` |
| Session progress | `progress_card`（子代理不可用） |
| Messaging | `message` |
| Sessions / agents | `sessions_*`、`subagents`、`agents_wait`、`agents_list`、`session_status`、goal 系列 |
| Automation | `cron`、`heartbeat_respond` |
| Gateway / nodes | `gateway`、`nodes` |
| Plugin lifecycle | `plugins` |
| Media | `view_image`、`image_generate`、`music_generate`、`video_generate`、`tts` |
| 大目录 | Code Mode 的 `exec`、`wait`、`tool_search`、`tool_describe`、`tool_call` |

**准入是一条流水线**（`tool-policy.ts` / `tool-policy-pipeline.ts` / `requester-tool-policy.ts` / `sandbox-tool-policy.ts` / `provider-tool-policy.ts` / `agent-tool-availability.ts`）：

> 模型只看到**同时通过**「profile + allow/deny 策略 + provider 限制 + 沙箱状态 + 通道权限 + 插件可用性」的工具。

- 工具组（`TOOL_GROUPS`）可整体允许/拒绝；shipped policy 名称带**重命名映射**（`update_plan` → `progress_card`）与**家族展开**（`canvas` → `show_widget`），老配置继续有效。
- **可用性（availability）与授权（authorization）明确分开**：某工具对调用者可见，不等于允许执行；授权另由运行期权威检查与审批门负责。
- 有专门的诊断入口（`tool-access-diagnostics.ts`、`exec-policy show`）回答「为什么这个工具没出现」。

**审批门（exec approvals）**：

- 命令只有在 `policy + allowlist + （可选）用户批准` 三者都同意时才运行；审批**叠在**工具策略与提权门之上，且**只能收紧不能放松**（有效策略取更严者）。
- 已批准的命令会绑定：工作目录、精确 argv、env 绑定、可执行文件**真实路径**（可写可执行文件还绑**内容哈希**）；**批准窗口内解析结果变化**（例如 PATH 上出现更早的同名可执行文件）即拒绝。
- shell 脚本与解释器调用还会尽量绑定一个具体本地文件，文件在批准后变化就拒绝；**绑定不出唯一文件时拒绝铸造批准**，而不是假装覆盖。
- 文档明确：审批**不是**逐用户认证边界，也不是文件系统只读策略。

**调用前后钩子**（`before_tool_call` / `after_tool_call` / `tool_result_persist`）：`{ block: true }` 是终态并停止更低优先级的处理器，`{ block: false }` 是 no-op 且**不会清除先前的 block**（同类语义也用于 `before_install` 与 `message_sending`）。

**结果守卫与限幅**：`session-tool-result-guard`（结果写入 transcript 前的转录合法性）+ `tool-result-limits`（按有效模型上下文自动定档：默认 16K 字符；上下文 ≥100K token 时 32K；≥200K 时 64K；同时不超过「上下文窗口 × 30% × 4」字符的硬上限）。

**循环与无进展检测**：滚动重复模式检测（默认关闭）+ **压缩后守卫**（`enabled !== false` 时总生效：压缩重试后若窗口内重复同一 `(tool, args, result)` 三元组就中止本轮）+ 无进展检测。

**参数修复**：独立包 `packages/tool-call-repair` + 运行时 `attempt.tool-call-argument-repair.ts`。

**Code Mode / Tool Search**：工具目录很大时不把全部 schema 发给模型，而是给一个受限编程接口（`code-mode-quickjs` 插件）配合 `tool_search` / `tool_describe` / `tool_call` 三个原语与排名逻辑。

### 6.3 对比

| 维度 | agentxx | openclaw |
|---|---|---|
| 工具定义 | `XXToolBase` + `get_definition()`；插件经 C ABI 注册 | Tool 定义 + 多运行时适配器（`agent-tool-definition-adapter`） |
| 准入 | 权限中间件（插件声明目标 + 白/黑名单 + 隔离 + 完全授权）；可用性与授权未分层 | 可用性由六道过滤决定；授权另由审批门与运行期权威检查负责 |
| 延迟加载 | `canDelayLoad` + 子代理检索 + 提示词只留名字 | Code Mode / Tool Search（把大目录变成可编程接口） |
| 重试 | `maxRetry`（异常触发） | 会话级重试预算 + 分层超时 + `tool-replay-safety` 判定可重放性 |
| 重复调用 | 连续相同调用询问用户（阈值可配） | 滚动模式检测 + 压缩后强制守卫 + 无进展检测 |
| 输出限幅 | 超阈值 **offload 到 share_store + 预览 + 按行分页取回**（逐工具开关；**无聚合计费预算、无缓存感知剪枝**，见 §6.6） | 分层自适定档 + **聚合预算（窗口 ×2×0.5 字符）** + **缓存过期剪枝** + 首尾保留/中段省略 + spill 详情 |
| 参数鲁棒性 | `autoFixArgsType` 类型自愈 | `tool-call-repair` 独立包 + 参数修复阶段 |
| 同轮并行 | **串行**（`for` + `co_await`） | 并行（有 launch checkpoint 概念） |
| 调用前后钩子 | 中间件 handle 的 start/end/error（可改参数与结果） | 类型化 plugin hooks，含终态语义（block 不可被后续 handler 清除） |
| 审批门 | 权限中间件询问（三态路径、完全授权、记住选择） | exec approvals（策略 + allowlist + 批准，绑定 argv/env/cwd/可执行身份与哈希，只收紧不放松） |
| 执行位置绑定 | 无（始终本机） | 有（exec 可绑定 gateway 或某个 node） |
| 工具错误形态 | 抛异常 → 统一格式化为 `[Exception aborted: …]` | 工具结果状态 + 失败警告注入 |
| 结果入 transcript | 无独立守卫（工具自行返回文本） | 有守卫（转录合法性 + 限幅） |

### 6.4 两者的优缺点

**agentxx**

- 优点
  - **四个开关正好覆盖四类现实问题**：工具太多（延迟加载）、输出太长（自动压缩）、网络抖动（重试）、模型打转（重复询问），且都能按工具独立配置。
  - **循环检测实现讲究**：限定「llm↔tool 交替链」避免把正常对话误判，回溯有界 + 提前终止，阈值可配，询问复用已有的用户交互通道。
  - **参数自愈 + 错误形态统一**：类型不匹配先尝试修正；错误统一成模型能读懂的文本，而不是让它面对结构化错误码。
  - **权限目标由插件声明**：工具作者最清楚它会读写哪些路径，声明式比宿主硬编码更可扩展。
- 缺点
  - **同轮工具串行**：多个独立调用要排队，长任务里延迟直接叠加。
  - **输出 offload 有前提**：只有启用 `autoSummaryOutput` 的工具才会把超限输出 offload 到 share_store；未启用的工具仍可能把超大内容直接塞进上下文（缺一次请求内所有工具结果的**聚合预算**，见 M28），也缺 openclaw 那种「已过缓存期的旧结果优先剪掉」的成本感知策略。
  - **没有「可用性 vs 授权」分层**：工具面与授权判定混在一处，无法表达「这个模型/这个客户端下不该出现这个工具」。
  - **缺少结果入 transcript 的守卫**：工具返回任意文本，畸形或超长内容会直接进入后续请求。
  - **审批不绑定执行身份**：见下条 M31 的说明。

**openclaw**

- 优点
  - **准入流水线可解释**：模型看到什么由六道过滤决定；「可用性 ≠ 授权」被明文写清，避免把「工具没出现」当成「没权限」。
  - **审批的绑定粒度是这套体系里最扎实的一处**：argv/env/cwd/可执行真实路径/内容哈希 + 批准后漂移即拒绝 + 绑定不出唯一文件就拒绝铸造批准——把「批准时看到的东西」与「实际执行的东西」钉死。
  - **限幅口径自适应模型容量，并与保真落盘配合**：既不炸上下文也不丢信息。
  - **压缩后守卫强制开启**：对症掐「溢出 → 压缩 → 又重复同一调用」的死循环。
  - **Code Mode 是工具面膨胀的正解**。
- 缺点
  - **文件数量与概念量巨大**：工具策略相关文件 200+，一个 `exec` 拆成十几个文件；「读完再改」不现实。
  - **策略层数多，归因困难**（因此需要专门的诊断入口）。
  - **并行 + steer 的可见性语义复杂**：需要「launch checkpoint」这类概念才说得清哪些调用会被跳过。

### 6.5 可迁移到 agentxx 的设计

| 编号 | 设计 | 落地位置 | 优先级 | 说明 |
|---|---|---|---|---|
| M26 | **同轮工具并行执行（带启动检查点）** | `agent/lib/src/nodes/toolcall.cpp` 的 `baseRun` | P0 | 现在 `for (tc : tool_calls) co_await execTool(...)` 串行。改为：先对整批调用做前置校验与权限询问（保持串行，因为可能要弹 UI），通过后并行派发互不依赖的工具再汇聚结果。注意三点：① 写类工具之间可能需要顺序保证（可先只并行只读工具，用工具的只读标记区分）；② 结果按 `tool_call_id` 归位，**不按完成顺序写入上下文**（保持 transcript 稳定）；③ 任一工具取消时整批取消要有一致语义。 |
| M27 | **工具结果落上下文前的守卫** | 新增 `agent/lib/src/nodes/tool_result_guard.cpp`，或在 `ToolcallWrapNode::execTool` 收尾处 | P1 | 三条检查：① 结果必须是合法 UTF-8 且能构成合法消息（防止畸形内容污染后续请求）；② 超过硬上限时截断但保留首尾，完整内容落 share_store 并给出取回方式；③ 与上下文里既有 `tool_call_id` 的配对关系自洽。 |
| M28 | **工具结果的聚合预算（在已有 offload 基础上补强）** | `ToolcallWrapNode::execTool` 的 offload 分支 + `AgentConfig` 新配额 | P1 | agentxx 已实现「超阈值 offload 到 share_store + 预览 + 按行取回」（§6.6），**缺的是「一次请求内所有工具结果合计」的上限**：当前只按单个调用限幅，五个 15K 的结果照样把上下文塞满。建议加两个配额：① 单次结果上限（现有 `toolcallSummaryLimitOutputLength` 之和）+ ② 一轮内所有工具结果合计上限（按模型窗口折算，例如窗口 × 50%）；超合计上限时按「最旧优先」把已完成的工具结果替换为 offload 预览（内容已在 share_store，不需要新机制）。 |
| M89 | **缓存过期感知的结果剪枝** | 与 M28 同批：offload/剪枝判定处 | P1 | openclaw 的做法：**provider 缓存 TTL 已过期的旧工具结果优先剪**（图片换成 `[image removed during context pruning]`、文本换成 `[Old tool result content cleared]`）。理由是成本：既然这段内容已经不在缓存里，剪掉它不会破坏前缀缓存命中，是「最便宜的可回收空间」。agentxx 若接入了缓存统计（M39）就可以照此实现；没有缓存统计时至少可按「轮数/时间」近似（例如超过 N 轮的旧工具结果先剪）。 |
| M90 | **预览口径升级：首尾保留 + 中段省略** | `ToolcallWrapNode::execTool` 的预览文本生成 | P2 | 现在的预览是「按行窗口/按字节」单向截断（只留开头）。openclaw 的做法是**保留首尾、中段插入省略标记**，并用 `hasImportantTail` 判断尾部是否重要（工具输出常常「开头是命令、结尾是错误摘要」）。另有细节值得抄：压缩恢复场景用**不同的提示后缀**（告诉模型这是压缩后的结果），最小保留长度可配（普通 2000 字符、恢复场景 0）。 |
| M29 | **「可用性与授权」两层拆开** | `AgentContext` 新增工具可用性计算；权限中间件保持授权职责 | P1 | 可用性只看运行环境：模型是否支持并行工具调用、客户端是否支持附件、插件是否已禁用、沙箱/worktree 是否激活；授权仍由权限中间件判定。收益是「工具没出现」与「工具被拒」在诊断上归因明确。 |
| M30 | **压缩后重复调用守卫** | `SummarizationMiddlewareHandle` + `ToolcallWrapNode::findConsecutiveRepeatCallKeys` | P2 | 现有检测是「连续 N 次相同调用」。补一条针对性的：**压缩发生后的下一轮**若出现与压缩前相同的 `(tool, args, result)` 三元组，判为无进展并中止本轮（或强制询问）。 |
| M31 | **工具审批的「执行身份绑定」** | `PermissionMiddlewareHandle` + `execute_command` 等工具的批准流程 | P2 | 现状批准「这次调用」，执行时重新解析命令。可借鉴的最小版本：批准时记录解析出的可执行文件绝对路径 + argv + cwd（可写可执行文件再加内容哈希），执行前复验；不一致则拒绝并要求重新批准。 |
| M32 | **工具面诊断入口** | TUI/CLI 命令 + `AgentContext` 查询接口 | P2 | 提供一条「为什么这个工具没出现/为什么被拒」的输出：依次列出「插件是否加载 → 是否声明权限 → 权限判定结果 → 是否延迟加载未命中 → 是否被模型能力过滤」。agentxx 策略层数少，做起来比 openclaw 便宜、收益也更直接。 |

### 6.6 实现细读（本轮新增）

#### agentxx：`execTool` 的实际顺序（五个阶段，每一步都有对应理由）

逐行读 `nodes/toolcall.cpp::execTool` 得到的事实顺序：

| 阶段 | 实现 | 理由/注意点 |
|---|---|---|
| ① 参数自愈 | `autoFixArgsType(tool->get_definition(), args)` | 失败只记 warning（`XX_LOGW`），不阻断调用 |
| ② 权限检查 | 经 `service.permission.check` 请求，**超时 0 = 不限制** | 拒绝时把 `[Permission denied]` 作为**工具结果文本**返回（模型能看到并改方式），不是抛异常 |
| ③ 重复调用询问 | 仅当 baseRun 的「llm↔tool 交替链」检测触发阈值（默认 5）**且**该工具在 `extra` 里声明了 `repeatCallCheck` | 经 HIL 中断弹**预设确认卡**（控件 id `allow`，取值 `"true"`/`"false"`）；结果必须**按 `tool_call_id` 下钻取值**（源码注释记录了这个坑：直接对顶层对象取 `allow` 恒取不到，会让用户点「允许」也被拒）；拒绝时返回可读文本要求换方式或换参数 |
| ④ 取消埋点 | `cancelToken->throw_if_cancelled("before tool execution")` | 因为 ②③ 都可能长时间挂起（权限弹窗不设超时），执行前必须重查一次 |
| ⑤ 执行 + 重试 | `XXToolBase::execute_async`；`maxRetry` 从 `extra` 读（用 `find`，避免 `operator[]` 往共享 map 插入键） | 重试次数与 `toolcallRepeatCheckThreshold` 都是逐工具可配 |

**输出 offload 的真实形态**（纠正本文 §6 早期版本的判断）：`autoSummaryOutput` 且结果长度超过 `toolcallSummaryLimitOutputLength` 时——

1. 全文写入 `share_store`（`addShareStoreItemValue` 返回 `storeId`）；
2. 返回的预览头形如 `[Content offloaded. Use the \`agentxx_share_store\` tool to fetch the content by ID {id}. Total {n} lines, show [1, k], hide [k+1, n].]` + 按行截取的前缀；
3. 两种分支：**能按行给出有意义窗口**（`lastLineIndex >= targetIndex/3`）时显示行范围；内容集中在少数超长行（按行截取近乎全隐藏）时退化为**按字节**截取并省略行范围；
4. 配套工具 `agentxx_share_store` 支持 `line_offset` / `line_limit` 分页，且它**自身启用了重复调用检查**（反复取同一内容也是循环信号）。

也就是说「保真落盘 + 预览 + 按需取回」这条链路完整存在，M28 已按实际情况改写为「聚合预算」，并新增 M89/M90 补齐两处更细的口径。

#### openclaw：工具结果预算分两层，且把「缓存过期」当成可回收信号

读 `src/agents/embedded-agent-runner/tool-result-truncation.ts`（约 57 KB）得到：

| 机制 | 参数/常量 | 含义 |
|---|---|---|
| 单调用预算 | 默认 16K 字符；上下文 ≥100K token → 32K；≥200K → 64K；上限 `window × 0.3 × 4` 字符 | 与 `tool-result-limits.ts` 一致 |
| **聚合预算** | `AGGREGATE_TOOL_RESULT_CONTEXT_SHARE = 0.5`、`PROMPT_TOOL_RESULT_AGGREGATE_CAP_MULTIPLIER = 4`、`resolveToolResultContextMaxChars ≈ window × 2 × 0.5` | **一次请求内所有工具结果合计**的上限；超出的结果被「省略标记」替换，多次省略合并成一条聚合说明 |
| **缓存过期剪枝** | `pruneExpiredCacheTtlToolResults` / `softPruneCacheTtlToolResult`；图片阈值 `CACHE_TTL_IMAGE_CHARS = 8000`；文本占位 `[Old tool result content cleared]`、图片占位 `[image removed during context pruning]` | 已过 provider 缓存 TTL 的旧结果**优先软剪**——既然不在缓存里了，剪掉它不破坏前缀命中，是最便宜的空间回收 |
| 截断口径 | 保留首尾 + `MIDDLE_OMISSION_MARKER`（中段省略）；`hasImportantTail(text)` 判断尾部重要性；`MIN_KEEP_CHARS = 2000`（恢复场景 0）；`appendBoundedTruncationSuffix` 有界追加后缀 | 工具输出常见「开头是命令、结尾是错误摘要」，单向截断会丢关键信息 |
| 恢复专用后缀 | `COMPACT_RECOVERY_SUFFIX` | 压缩恢复路径用不同提示，告诉模型这是压缩后的结果 |
| 完整内容回填 | `getToolResultSpillDetails(message)` | 完整内容落盘 + 详情回填（与 agentxx 的 share_store 同思路） |
| **存储/投影分离** | `TOOL_RESULT_PROJECTION_KEY = Symbol(...)`（不可枚举键） | 同一条 tool 结果消息同时持有「存储形态」与「可重建的模型投影形态」，投影按预算随时重算 |
| 警告去重 | `TOOL_RESULT_WARNING_DEDUPE_LIMIT = 1024` | 截断/省略警告不会刷屏 |

**对本文结论的影响**：① M28 由「保真落盘」改为「聚合预算」；② 新增 M89（缓存过期感知剪枝）与 M90（首尾保留口径）；③ 「存储形态 vs 模型投影」这一维度本文原先没有，已作为 M91 记入 §12.5；④ 单调用 + 聚合两层预算是「一个数值不够」的直接证据：单调用限幅只能防「一次超大输出」，防不住「多次中等输出」。 |

---

## 7. 系统提示词、技能与记忆

### 7.1 agentxx 现状

**提示词的三个来源**（详见 §3.1）：

| 来源 | 载体 | 生命周期 |
|---|---|---|
| 内置文本 | `AgentPrompt::systemPrompt`（prompt.cpp 内定义） | 进程内固定 |
| 功能/插件追加段 | `AgentPrompt::appendSystemPrompts`（map，按 key 字典序拼接） | 插件装载时注入，卸载时按备份恢复 |
| 每轮动态段 | `appendSystemMessage`（技能元数据列表、记忆文件内容等） | 每轮重建 |

**技能中间件**（`SkillMiddlewareHandle`）：

- 扫描目录列表可变（`addSkillDirs` / `removeSkillDirs`，供插件运行期增删）。
- 每个 `SKILL.md` 解析出 `name` / `description` / `license` / `compatibility` / `metadata` / `allowed_tools` / `mdText`，并做**规范约束校验**（name 1–64 字符、只允许小写字母数字与连字符、不得以 `-` 开头/结尾、不得有连续 `--`、必须与父目录同名）。
- 注入方式：提示词里给出**技能名 + 描述 + 路径**，正文由模型按需用 `agentxx_filesystem_read`（`line_limit=1000`）读取——即**渐进式展开**；提示词模板里明确写了这条工作流。
- 缓存与失效：`resourceEpoch`（目录变更即递增）+ `cachedResourceEpoch`，状态侧缓存据此失效重建；`needReloadSkillMetadata` 标记下一轮全量重扫。
- 错误：读不出来的技能进 `loadErrors`，不影响其他技能。

**上下文文件（记忆文件）中间件**（`MemoryFileMiddlewareHandle`）：

- 首次 agent 调用时读取配置的上下文文件并缓存内容，**每次模型调用注入系统提示词**。
- 同样支持插件运行期增删文件（`addMemoryFiles` / `removeMemoryFiles`）与 `resourceEpoch` 失效。

**记忆相关的其它机制**：`agentxx.agent.session`（会话查询/读写）、`share_store`（工具级大内容暂存）、`tool_skill_search` 子代理（需要时再检索技能正文与延迟加载工具）。

### 7.2 openclaw 现状

**技能（skills）**：

- 技能是 `SKILL.md`（YAML frontmatter + markdown 正文）指令包；**加载顺序有 7 级优先级**（工作区技能 > 项目 agent 技能 `<workspace>/.agents/skills` > 个人 `<~/.agents/skills>` > 托管/本地 `<state-dir>/skills` > 工作坊技能 > 内置技能 / 保管技能 > 额外目录与插件技能），同名取最高优先级。
- 加载期就按**环境、配置、可执行文件存在性**过滤（gating）；会话在执行目录（含托管 worktree）里运行时，也会加载该目录的 `skills/` 与 `.agents/skills/`，沙箱运行读的是物化副本。
- 技能有快照（snapshot refresh）与允许清单（agent allowlists）语义。
- 「技能工作坊（Skill Workshop）」负责评审并批准 agent 自拟的技能；「自学习（Self-learning）」把它接成闭环；技能可发布到 ClawHub（社区市场）。

**记忆（memory）**——`docs/concepts/memory-architecture.md` 给出五条设计原则：

1. **无隐藏状态**：模型只记得写进 agent 工作区文件里的东西；每个记忆面都能用文本编辑器查看与修改（记忆索引是 SQLite，但事实源是文件）。
2. **写入才是难点**：长周期评测显示「写进去什么」比「怎么索引」更决定效果（引 LongMemEval, arXiv:2410.10813）；因此把整理从繁忙的回复路径移到**专门的后台通道**（dreaming）。
3. **写路径就是安全边界**：内容级扫描挡不住投毒，所以在写时**强制来源（provenance）**并用结构化的门控决定能否晋级，而不是事后检测坏记忆。
4. **确定性门控 + 门内的模型判断**：打分、阈值、资格、匹配、生命周期都是确定性代码；只在真正需要语言判断处用模型，且始终在确定性的界内。
5. **失败不阻塞回复**：回复路径上的每一步记忆操作都有超时或降级；记忆子系统坏掉只降低召回质量，绝不吃掉一轮。

**分层（tier）模型**：

| 层 | 载体 | 谁写 | 是否注入 |
|---|---|---|---|
| 指令 | `AGENTS.md` 与工作区指令文件 | 仅人类 | 每会话开始必注入 |
| 精编核心 | `MEMORY.md`、`USER.md` | dreaming 合并 / 用户直接要求 | 会话开始按来源资格注入，且有字符预算（`USER.md` 4000 字符） |
| 情景 | `memory/YYYY-MM-DD.md` 日记、会话 transcript | agent 干活时、记忆冲刷、transcript 捕获 | **从不自动注入**，只能按需搜索 |
| 前瞻 | 常驻意图（SQLite）+ cron 任务 | `intent` 工具、定时任务 | 仅在触发器命中时 |
| 复核 | `DREAMS.md`、dreaming 报告 | dreaming 各阶段 | 从不注入，供人阅读 |

关键边界是「精编核心 ↔ 情景」：核心小、按资格常驻上下文、只能经门控合并写入；情景大、可追加、只能经显式搜索或升级通道到达；**情景内容不经晋级门控不会跨到核心**。

**来源（provenance）**：索引里的每条都带模型无法用散文写入的列——`origin class` 是封闭集合（`owner` 人类在可信通道里亲口说的 / `agent` agent 从 owner 内容推导的 / `untrusted` 来自网页、工具输出、群里非 owner 参与者 / `system` 心跳提示与 cron 前言这类脚手架）、`session kind`（interactive / cron / heartbeat / sub-agent）、观测时间戳与 **supersession key**（新观测可**取代**旧的，而不是并列堆积）。

**相关子机制**：active memory（主动记忆）、user model（`USER.md` 的指令式用户模型，条目按日期写成 active/superseded）、standing intents（常驻意图）、dreaming（离线整合，属后台通道，与其他后台工作共享 3 个槽位）。

### 7.3 对比

| 维度 | agentxx | openclaw |
|---|---|---|
| 技能来源 | 扫描目录列表（配置 + 插件动态增删）；无优先级概念 | 7 级优先级 + 名字冲突裁决 + 执行目录追加 + 沙箱物化副本 |
| 技能注入 | 名字 + 描述 + 路径，"按需读正文"（渐进式展开） | 同类做法，但有快照/允许清单/gating（环境、二进制、配置） |
| 技能治理 | 无（自己放进去就生效） | 工作坊评审、自学习闭环、社区市场（ClawHub） |
| 记忆载体 | 上下文文件（配置路径列表）+ 每调用注入；另有大内容暂存 share_store | 分层文件（指令/精编核心/情景/前瞻/复核）+ SQLite 索引 |
| 记忆写入 | 由模型/用户直接写文件；无门控 | **写路径即安全边界**：来源归类 + 晋级门控 + supersession key |
| 记忆读取 | 全量注入（配置的文件内容每轮注入） | 核心按资格预算注入；情景只能按需搜索（从不自动注入） |
| 记忆背景整理 | 无 | dreaming（后台通道 + 3 槽位预算 + 可人工复核的报告） |
| 提示词段管理 | `appendSystemPrompts` 按 key 有序拼接；插件卸载按备份恢复 | 固定段列表 + 缓存边界（见 §3） |
| 提示词回归 | 无 | 提示词快照 fixture + CI 漂移检查 |

### 7.4 两者的优缺点

**agentxx**

- 优点
  - **技能注入方案省 token 且贴合规范**：只给名字/描述/路径，正文按需读，且明确要求 `line_limit=1000`；元数据校验按 Agent Skills 规范做了硬约束。
  - **资源纪元机制干净**：目录/文件列表由插件运行期增删时，用 `resourceEpoch` 让各线程缓存失效自愈，而不是到处发通知。
  - **提示词段的归属清楚**：`appendSystemPrompts` 按 key 归属到插件/功能，卸载时按备份恢复——避免「插件卸了但提示词段还在」。
- 缺点
  - **记忆是「全量注入」**：配置的上下文文件内容每轮都进提示词，文件一大就直接吃掉上下文预算，且没有「按需检索」通道；也没有「这条记忆从哪来、是否可信」的概念。
  - **技能无优先级与冲突裁决**：多个目录里同名技能谁生效没有定义；插件装的技能与用户放的技能无法区分优先级。
  - **没有后台整理通道**：写记忆完全在回复路径上完成（模型自己写文件），既拖慢回复，也没有「沉淀/取代」这类语义。
  - **技能无治理**：放进去即生效，没有评审/来源标记（对本地编码代理问题不大，但对「共享/多用户」形态会立刻成为问题）。

**openclaw**

- 优点
  - **「写入才是难点」这一判断很硬**：把整理移出回复路径、用确定性门控约束模型判断，并用 supersession key 做「取代」而非「堆积」——这三点是长期记忆系统的真正难点。
  - **来源（provenance）作为结构而非约定**：来源分类是模型写不进去的列，因此「网页里看到的说法」不可能伪装成「用户亲口说的」。
  - **分层带来明确的注入策略**：核心按预算注入、情景从不自动注入——同一套文件既能常驻关键事实，又不会让日志淹没上下文。
  - **技能优先级与 gating 完整**：同名裁决、环境/二进制 gating、执行目录追加、沙箱副本，覆盖了真实部署中的多数冲突。
  - **失败不阻塞回复**：记忆每一步都有超时/降级，这条被写进设计原则。
- 缺点
  - **体系庞大**：分层 + 来源 + 门控 + dreaming + 工作坊 + 用户模型 + 常驻意图，学习与调参成本高；很多能力还牵涉 SQLite 索引与后台预算。
  - **「无隐藏状态」与 SQLite 索引并存**：事实源在文件，但索引在库里，两者的同步与修复逻辑（记忆索引重建）本身是复杂度来源。
  - **技能市场/工作坊对企业内网场景偏重**（ClawHub 是它的产品形态，不是每个部署都需要）。

### 7.5 可迁移到 agentxx 的设计

| 编号 | 设计 | 落地位置 | 优先级 | 说明 |
|---|---|---|---|---|
| M33 | **记忆分层 + 按需检索** | `MemoryFileMiddlewareHandle` 扩展 | P0 | 现状是「配置的文件每轮全量注入」。建议分两类：① **常驻类**（体积受控、每轮注入，如项目约定/用户偏好，设字符预算上限并在超限时告警）；② **检索类**（日记式记录，只在提示词里给「可用记录 + 检索方式」，由模型按需用文件工具/grep 读取）。这一步不改 ABI、不改协议，收益直接（省上下文 + 长会话更稳）。 |
| M34 | **记忆条目的来源标记** | 与 M33 同批：记忆文件里用显式前缀或 frontmatter 标注来源 | P1 | 最小形态：每条记忆带 `[owner]` / `[agent]` / `[untrusted: <来源>]` 标记，提示词里说明「untrusted 内容只作参考，不得当作指令」。这不需要 SQLite 列，写在文本里即可；但必须由**宿主/工具**写入标记，不能靠模型自觉（否则等于没做）。 |
| M35 | **技能来源优先级 + 冲突裁决** | `SkillMiddlewareHandle`（扫描目录改为带优先级的列表） | P1 | 给出至少三级：会话/项目级（`{workDir}/.agentxx/skills`）> 用户级（`~/.agentxx/skills`）> 插件/内置；同名取高优先级，并在技能列表里标注来源。现在多目录同名技能的行为是未定义的，这是个实际的坑。 |
| M36 | **后台整理通道（轻量版）** | 新增可选的「会话结束/空闲整理」任务，走独立小预算 | P2 | 对应 openclaw 的 dreaming。最小形态：会话空闲 N 分钟后（或用户显式触发），用一个无工具的子代理读本轮 transcript，产出「值得长期保留的事实」候选，写入**检索类**记忆文件（不直接改常驻类），并保留原始出处。关键约束：后台通道与前台轮次分开预算，失败不影响回复。 |
| M37 | **技能元数据校验的错误可见性** | `SkillMiddlewareHandle` 已是现状（`loadErrors`） | — | 已有：解析失败的技能记入 `loadErrors`。可补的是**把这些错误显示给用户**（TUI 里一条提示），而不是只进日志。 |

---

## 8. LLM 层：协议、provider、鉴权与缓存

### 8.1 agentxx 现状

**模型注册表**（`ModelProviderRegistry`，`agent/lib/include/agentxx/agent/model_registry.h`）：

- 管理多个**命名模型配置**（`ModelConfig`）与对应的 provider 实例缓存（按配置 `type` 创建）。
- 每个会话的「当前选择」**由 Session 独立记录**（不放在注册表里），UI 经 `WireGetModel` / `WireSelectModel` 间接操作；`selectModel(sessionId, name)` 与 `getCurrentModelName(sessionId)` 是会话级入口。
- 只在 agent io 线程访问，无锁；`setProvider` 允许嵌入方注入自定义 provider（测试或嵌入场景）。
- 注册表同时提供「可用模型列表 + 各模型多模态能力（image/audio/video 输入）」给客户端，客户端据此决定是否显示附件按钮。

**协议实现**（`agent/lib/src/protocol/`）：

| 文件 | 作用 |
|---|---|
| `openai_provider.cpp` | OpenAI Chat Completions（`type: "openai"`） |
| `openai_provider.cpp` 同文件覆盖 | OpenAI Responses API（`type: "openai-responses"`） |
| `anthropic_provider.cpp` | Anthropic Messages（含 `thinking` / `redacted_thinking` 块原样回传） |
| `mcp_client.cpp` / `mcp_server.cpp` | MCP 客户端/服务端（工具面与协议面） |
| `a2a_client.cpp` / `a2a_server.cpp` | A2A（agent 到 agent） |
| `acp_server.cpp` | ACP（编辑器类客户端接入） |

**请求组装与韧性**：

- `ModelCallWrapNode` 负责：解析会话当前 provider/模型 → `build_params(sessionId)` → `callLLM()` → `onReceiveToken()` 流式收 token。
- `repairMessages()`：请求前的消息修复（保证历史合法，例如悬挂 `tool_calls`、配对缺失）。
- 重试：`AgentConfig::llmMaxRetry` 默认 5（最多 6 次尝试）。
- 多模态能力表来自配置；附件以 base64 dataUrl 或路径形式进入消息。
- 缓存：**没有 prompt cache 断点/保留策略配置**，也没有 cache read/write 用量统计。

### 8.2 openclaw 现状

**模型运行时世代（generation）快照**——这是它 LLM 层最核心的设计：

> 网关启动、配置变更、插件变更或鉴权发布时，为**每个已配置的 agent** 构建**一份**「已准备好的模型运行时世代」。每份世代把「发现到的鉴权模板 + 模型注册表 + 投影出的模型目录」作为一个**原子快照**持有；agent 运行时从该快照**分叉**出可变的鉴权与注册表副本。浏览、状态、cron、doctor、TUI、PDF、图像这些路径**读已发布的目录**，而不是各自重复做文件系统发现。
>
> **失败的或陈旧的世代绝不会与更新的部分世代并存被服务**——生命周期所有者必须先发布一份完整替代品。
>
> 运行时选择在**请求方 agent 的作用域**里解析后才成为 owner key；lease 准入把这份已准备的选择带下去并读取**精确 owner 的快照**。重试必须观察到「owner 变化」或「发布门」，发布状态未变则返回可重试错误，而不是阻塞网关事件循环。

**运行时选择（harness）**：内置运行时 id 是 `openclaw`（历史别名 `pi` 归一化到它），插件 harness 可注册额外 id（如 `codex`）；选择由「模型级 `agentRuntime.id` 覆盖 provider 级」决定，未设置或 `default` 解析为 `auto`——`auto` 会选一个支持该 provider 路由的已注册插件 harness，否则用内置。**仅凭 provider 或模型前缀永远不选 harness**；OpenAI 只在「精确的官方 HTTPS Platform Responses / ChatGPT Responses 路由且没有作者写的请求覆盖」时才隐式选 `codex`。

**失败切换（failover）两级**：

1. 当前 provider 内**鉴权档位轮换**（auth-profile rotation + 冷却）。
2. 轮换到 `agents.defaults.model.fallbacks` 里的下一个模型。

轮换/换模型之前先做**有界同模型恢复**（针对临时限流与 provider 故障），并**继续既有 transcript**（保留部分输出与已完成工作），提示 agent「检查被打断的动作再决定是否重做」。回退是**轮次局部**的：只持久化「回退通知状态」以便 `/status` 区分「选中的模型」与「实际回答的模型」，**不把回退结果写成下一轮的模型选择**。显式的用户会话选择是「严格」的（不走配置回退）。

**超时分层**（与 §2 呼应）：`agent.wait` 30s（仅等待）→ agent 运行预算（默认 48h）→ **模型空闲看门狗**（云 120s / 自建 300s，`models.providers.<id>.timeoutSeconds` 可延长但受更小的 agent 运行超时约束）→ provider HTTP 请求超时。

**提示词缓存（`docs/reference/prompt-caching.md`）**：

- `cacheRetention: "none" | "short" | "long"`，可配在全局 / 模型 / agent 三层，**合并顺序后者胜**。
- **保持模型设置稳定**：换模型一定开始新的缓存谱系；换 thinking/reasoning 级别也可能让复用失效；支持的原生 OpenAI Responses 请求会保留原 effort 并追加轮次级配置控制（含传输过期/网关重启后仍有已保存的重放元数据与历史匹配时）。结论写得很直白：**在意缓存连续性就在建会话时定好模型与思考级别并保持不变，计划变更就开新会话**。
- 用量归一化：provider 暴露 `cacheRead` / `cacheWrite` 计数器时统一归一化；用量摘要优先用实时快照，缺缓存计数时回退到最后一条 transcript 用量条目，**实时非零值永远胜过回退值**。
- provider 侧还有 `google-prompt-cache.ts` 这类专属实现。

**模型目录与发现**：`packages/model-catalog-core`（目录核心）+ `src/model-catalog` + `src/model-picker`（选择 UI）+ `src/provider-runtime`；provider 数量按 `docs/providers/`（约 100 篇）与 `extensions/` 里的 provider 插件（约 50 个）计，鉴权方式分层（API Key / OAuth / 环境变量 / 设备码），有独立的 `docs/concepts/oauth.md` 与 `auth-credential-semantics.md`；`src/agents/auth-profiles`（165 个文件）负责鉴权档位与冷却。

### 8.3 对比

| 维度 | agentxx | openclaw |
|---|---|---|
| 协议族 | 3 种类型（openai / openai-responses / anthropic），各自一个 provider 实现 | provider 适配层在 `packages/ai`（186 文件）+ `src/llm/providers`；约 50 个 provider 插件 |
| 模型注册与装配 | `ModelProviderRegistry`（命名配置 + provider 缓存），进程内一次性装配 | **每 agent 一份模型运行时世代快照**（鉴权模板 + 注册表 + 投影目录），原子替换、不复用陈旧世代 |
| 会话级模型选择 | Session 独立记录「当前选择」，会话间隔离 | 会话级选择 + 「显式选择严格、不参与回退」的规则 |
| 失败切换 | 仅 `llmMaxRetry` 次数重试（同模型） | 同模型有界恢复 → 鉴权档位轮换 → 模型回退（轮次局部，不改会话选择） |
| 超时 | HTTP 连接超时 + 整体重试 | 五层（等待/运行预算/模型空闲看门狗/provider HTTP/清理窗口） |
| 空闲看门狗 | 无（长思考或无 chunk 到达时只能等整体超时） | 有，且区分云/自建档位 |
| 用量与缓存统计 | 有 token 统计（上下文统计/压缩用） | 归一化 `cacheRead`/`cacheWrite`，用量摘要带回退规则 |
| prompt 缓存策略 | 无（也没有缓存边界设计，见 §3） | `cacheRetention` 三层配置 + 「保持设置稳定」的显式规则 |
| 目录/鉴权 | 配置文件里手写模型条目与 api key | 远程模型目录 + 本地缓存；鉴权分层（API Key/OAuth/环境变量），有冷却与轮换 |
| 请求修复 | `repairMessages()`（消息合法性） | 参数修复阶段 + 消息修复 + `tool-call-repair` 包（分开处理） |

### 8.4 两者的优缺点

**agentxx**

- 优点
  - **模型配置与会话选择分离得干净**：注册表只管「有哪些模型」，会话管「用哪个」，多会话可以各用各的模型；嵌入方还能注入自定义 provider。
  - **协议实现直接**：三种类型的请求/响应差异都在各自 provider 内消化，没有中间抽象层，读代码成本低。
  - **能力表驱动 UI**：模型的多模态能力数据直接决定客户端是否显示附件按钮，避免「按钮在但发不出去」。
- 缺点
  - **没有空闲看门狗**：模型长时间不吐 chunk（provider 卡住）只能等整体超时/重试，用户侧表现为「卡住」。这是最容易补且收益明显的一项。
  - **没有用量与缓存统计**：长会话下「这轮花了多少 token、缓存命中多少」不可见；压缩阈值也只能靠估算。
  - **重试是同模型重试**：provider 整体不可用（限流/区域故障）时不换模型，只能失败。
  - **鉴权是静态配置**：一份 api key 写死在配置里，没有档位轮换/冷却/OAuth 概念；多 key 场景需要用户自己起多个模型条目并手动切换。
  - **无请求级缓存策略**：没有任何「这个前缀是稳定的，请缓存」的表达，配合 §3 的提示词重建问题，长会话成本偏高。

**openclaw**

- 优点
  - **世代快照解决了真实痛点**：配置/插件/鉴权变更时把「鉴权模板 + 注册表 + 目录」当一个原子替换，且明确禁止「新旧并存」与「陈旧世代被服务」——避免了「一半用新凭据一半用旧目录」的错态。
  - **失败切换的分级正确**：先同模型有界恢复（保留部分输出、继续 transcript），再轮换鉴权档位，最后才换模型；且回退**不改会话选择**，语义干净。
  - **超时分层实用**：模型空闲看门狗与整体运行预算分开，能区分「provider 卡了」与「任务本来就长」。
  - **缓存策略是可操作的**：`cacheRetention` 三层配置 + 「改模型/改思考级别就换谱系」的明文规则 + 缓存用量归一化，让成本可观测、可控制。
  - **鉴权体系完整**：档位、冷却、OAuth、凭据语义文档，支撑多账号与多 provider 的真实运营。
- 缺点
  - **复杂度高**：世代快照 + 发布门 + lease 准入 + owner 作用域解析，理解门槛高；「重试必须观察到 owner 变化或发布门」这类规则很难自我验证。
  - **provider 数量带来维护面**：50 个 provider 插件 × 各家协议差异（重放策略、工具兼容、流包装），需要共享 helper 与「家族」抽象来收敛（`src/plugins/AGENTS.md` 专门要求「provider 策略分层、别在插件里重复编码策略」）。
  - **缓存连续性对用户是硬约束**：想改模型/思考级别就得开新会话——这是有意为之的取舍，但对用户不总是方便。

### 8.5 可迁移到 agentxx 的设计

| 编号 | 设计 | 落地位置 | 优先级 | 说明 |
|---|---|---|---|---|
| M38 | **模型空闲看门狗** | `ModelCallWrapNode::callLLM` / `onReceiveToken`（`agent/lib/src/nodes/modelcall.cpp`） | P0 | 加一个「距离上次收到 chunk 的静默时长」计时：超过阈值（建议默认 120s，可按模型配置覆盖）即中止本次请求并按既有重试策略处理。与整体超时分开，避免「provider 卡住」被当成「任务很长」。实现上只需在每个 token 回调里更新时间戳 + 一个 steady_timer。 |
| M39 | **token 用量与缓存统计** | `ModelCallWrapNode` + 会话统计 + `WireContextStats` | P1 | 从 provider 响应里取 usage（含 `cache_read_input_tokens` / `cache_creation_input_tokens` 这类字段），按会话累计并展示：本轮输入/输出/缓存读/缓存写。既有压缩与统计也能改用真实值而非估算系数（配合 M13）。 |
| M40 | **模型级回退链** | `ModelProviderRegistry` + `ModelCallWrapNode` | P1 | `ModelConfig` 增加 `fallbacks: [模型名…]`；「重试耗尽且错误属于可回退类别（限流/鉴权失败/服务不可用）」时切到下一个候选。规则照抄 openclaw 的关键一条：**回退是轮次局部的，不改变会话选择**，只在结果里标注「本轮由 X 回答」。 |
| M41 | **多鉴权档位与冷却** | `ModelConfig`（`apiKeys` 列表）+ provider 层轮换 | P2 | 最小形态：一个模型条目可配多个 api key，失败后轮换并给该 key 加冷却时间；不需要 OAuth。对「多个 key 分摊限流」的场景直接有用。 |
| M42 | **缓存保留策略** | `ModelConfig` 增加 `cacheRetention`；provider 按各家 API 落地 | P2 | Anthropic 侧是显式 cache breakpoint（可指定缓存到哪一段）；OpenAI 侧主要是「保持前缀稳定 + 路由亲和」。agentxx 的先决条件是 §3 的 M11（提示词缓存边界）。建议顺序：先做 M11，再在 provider 层加可选断点。 |
| M43 | **装配快照化** | `ModelProviderRegistry` + `AgentConfig` 加载路径 | P2 | openclaw 的「世代快照」在 agentxx 里可以退化成一个更简单的形态：配置热更新时**整体重建**注册表并以「新表替换旧表 + 递增版本号」发布，禁止边改边用；同时把「可用模型 + 能力表 + 目录」当作一个整体快照发给客户端（避免客户端拿到半新半旧的模型列表）。 |

---

## 9. 上下文压缩与预算控制

> §3 已把两侧的压缩机制做过对照（触发阈值、产物形态、归属、幂等）。本节只聚焦**预算口径与恢复路径**这两个容易出问题的维度。

### 9.1 agentxx 现状

**预算来源分散在三处**（这是当前最实际问题）：

| 位置 | 口径 |
|---|---|
| 压缩中间件 | 模型上限取 `ModelConfig::modelContenxtMaxToken`（未配置时 256K 默认）；估算系数 `asciiCharsPerToken=4.0` / `unicodeCharsPerToken=1.1` / `tokensPerImage=400` / `extraTokensPerMessage=3`；触发阈值 75%；硬截断兜底 >=95% 或同轮失败 ≥2 次 |
| 上下文统计 | 另一套 `countTokens(systemMsgs, messages, countThinking)` 调用路径（供 `WireContextStats` 展示） |
| 模型请求 | provider 实际请求的 token 上限由模型自身决定（agentxx 不设输出预留参数） |

**压缩切分**：`splitRecentByTokenBudget` 按「最近段预算 = 上限 × `recentTokenBudgetRatio`（默认 0.20）」切分，并做轮次对齐（recent 开头若是 tool 消息则回退到发起它的 assistant；压缩段末尾若悬挂 tool_calls 则整组划入 recent）。**至少保留 1 条 recent 消息**。

**恢复路径**：

- LLM 压缩失败（返回空）→ 调用方保留原消息；同轮失败 ≥2 次 → 硬截断（system + 截断说明 + 最近 30% 预算）。
- 压缩经 `NodeInterrupt` 派生子代理完成；`resume` 后中间件**从头重新执行**，压缩提示消息按挂起 id 复用（更新而非追加，不产生重复提示）。
- 手动压缩（`compactSessionContext`）在 agent 空闲时走 `direct=true` 路径，直接经宿主 `spawnBatch` 派生子代理并等待，不抛 `NodeInterrupt`。
- **没有「溢出错误后重试」这条路径**：agentxx 是在请求**发出前**判断阈值并压缩，因此不会遇到「provider 返回 context overflow」这种事后错误（除非估算严重偏低）。

### 9.2 openclaw 现状

**预检与触发**：

- 请求预算 = 窗口 − max(输出预留, buffer)；接近上限触发自动压缩（默认挡位 `"safeguard"`，比 `"default"` 多了质量审计）。
- **提前预检的权威口径**由上下文引擎的 `promptAuthority` 决定（见 §3.2）：`"assembled"` 只检查装配后的估算，引擎自持压缩时宿主默认跳过通用预检，`"preassembly_may_overflow"` 时取「装配估算」与「装配前全历史估算」的较大值。这个设计的用意是：**只有引擎自己最清楚"我返回的消息之外还有没有溢出风险"**。

**溢出恢复（overflow recovery）**——这是它比 agentxx 多出来的一整条路径：

- 识别**数十种 provider 专有的溢出错误串**（Anthropic / OpenAI / Bedrock / Gemini / Ollama / OpenRouter…，如 `request_too_large`、`context length exceeded`、`input exceeds the maximum number of tokens`、`ollama error: context length exceeded`）。
- 命中后**压缩并重试**。若 provider 在**工具调用全部完成之后**才拒绝请求，内置运行时可以「压缩并从这些已记录的结果继续」：**保持当前模型与账号、保留原请求、不重放已完成的动作**；前提是工具结果已结算（pending 工具、审批中、已取消、或有意结束本轮的工具不走这条路）。
- 溢出恢复会**在当前模型上下文窗口内裁掉工具结果**；更早的消息与重置边界留在保留历史里，不复制进新的 transcript 条目。

**压缩后行为**：

- 内置运行时在推理前完成必需的 checkpoint 与压缩；在常驻网关会话里，可选的记忆冲刷与压缩**等回复投递结算且前台 owner 关闭之后**再做，用**独立的 session owner + 本轮剩余时间**；新消息会先取消并结算这些可选工作，再读会话用于自己的推理。
- `openclaw agent --local` 一次性命令跳过轮后可选工作，下一条命令在推理前做必需维护。
- `agents.defaults.compaction.enabled: false` 只关掉「主动阈值压缩 + 可选维护」，**溢出恢复压缩与手动 `/compact` 仍然可用**。
- 压缩前会自动提醒 agent「把重要笔记写进记忆文件」。
- **停止/超时也会停止它的溢出或超时恢复**：取消后不再启动恢复钩子、维护、transcript 截断或重试；取消不是回滚——已完成的压缩留在 transcript 并计数。
- 手动 `/compact` 可带聚焦指令（宿主限制 800 个 Unicode 码点并转义为提示词数据）；客户端侧手动压缩用 `keepRecentTokens`（默认 20,000）作为切点预算。
- safeguard 挡位下：最终摘要预算在**校验前**应用；必需标题必须留在保留正文里，待办与精确标识符必须原样出现在将存储的文本里；不合法只允许配置次数的纠正尝试；**没有合格摘要就停止压缩、保留原始历史、上报既有的恢复结果**。

### 9.3 对比

| 维度 | agentxx | openclaw |
|---|---|---|
| 预检依据 | 本地估算（经验系数）与阈值 75% | 请求预算 + 引擎声明的 `promptAuthority`（谁有权威决定用哪个估算） |
| 溢出错误 | 不处理（靠事前压缩规避） | 识别数十种 provider 错误串 → 压缩并重试；工具完成后的拒绝可「从已记录结果继续」 |
| 溢出后的裁剪 | 不适用 | 在当前窗口内裁工具结果，保留更早历史不复制 |
| 压缩时机 | 模型调用前（中间件 start 阶段） | 推理前必需压缩 + 回复结算后的可选维护（独立 owner + 剩余时间） |
| 压缩质量门 | 无（失败即降级） | `safeguard` 挡位：标题/待办/标识符校验 + 有限次纠正 + 不合格就不写 |
| 手动压缩 | 有空闲路径（`direct=true`） | `/compact [聚焦指令]`，指令有长度与转义约束 |
| 取消与压缩的关系 | 压缩是轮内动作，取消即中止 | 取消不是回滚：已完成的压缩保留并计数；取消后不再启动恢复 |
| 压缩前提醒 | 无 | 提醒 agent 先写记忆 |

### 9.4 两者的优缺点

**agentxx**

- 优点
  - **三层降级保证请求能发出**：确定性压缩 → LLM 压缩 → 硬截断，失败 2 次或 95% 就兜底，工程上很稳。
  - **切分口径考虑周全**：轮次对齐（不把 tool 结果与发起它的 assistant 拆开）、悬挂 tool_calls 处理、至少保留一条 recent。
  - **压缩即落盘 + 提示消息按挂起 id 复用**：崩溃不丢压缩结果，中断恢复不产生重复提示。
  - **手动与自动两条路径分开**（`direct` 标志），避免了「空闲时调用却抛 NodeInterrupt 无人处理」这类结构性错误。
- 缺点
  - **预算口径三处并存**：压缩判断、统计展示、实际请求各自算一遍；系数是经验值，模型换起来容易失真。
  - **没有溢出恢复**：一旦估算偏低（例如图片/长 tool 输出被低估），provider 直接报错，本轮失败——而这是长会话里最常见的失败类型之一。
  - **压缩质量无门**：LLM 摘要内容不做校验（标题/待办/标识符是否保留），差摘要会静默进入后续上下文。
  - **压缩与回复结算耦合**：压缩在轮内完成，长会话下这一轮的延迟里包含了压缩时间。

**openclaw**

- 优点
  - **溢出恢复是一整条独立路径**，且「工具已完成 → 从记录结果继续、不重放」这条尤其正确：既不重复副作用，也不丢已完成的工作。
  - **预检权威交给引擎声明**（`promptAuthority`），避免宿主用自己的估算覆盖「更懂上下文的那一方」。
  - **压缩质量门 + 不合格就不写**：宁可保留原历史也不写一个坏摘要，这条与「失败不阻塞回复」原则一致。
  - **可选维护与必需压缩分离，且用独立 owner + 本轮剩余时间**：不拖慢回复，又不会丢维护。
  - **取消的语义写得很清楚**：取消不是回滚，已完成的压缩保留并计数。
- 缺点
  - **溢出错误串表是维护负担**：数十种 provider 专有字符串需要跟着各家 API 变化更新，且匹配失败就意味着恢复路径失效。
  - **压缩时机的规则多**（必需 vs 可选、常驻 vs 一次性、前台 owner 关闭之后…），实现与测试都要覆盖这些组合。
  - **质量门带来额外失败面**：safeguard 挡位可能因为「摘要不合格」而放弃压缩，此时若窗口已满，后续请求仍要靠其它机制兜底。

### 9.5 可迁移到 agentxx 的设计

| 编号 | 设计 | 落地位置 | 优先级 | 说明 |
|---|---|---|---|---|
| M44 | **溢出错误的识别与压缩重试** | `ModelCallWrapNode`（捕获 provider 错误）+ `SummarizationMiddlewareHandle` | P0 | provider 返回的错误里识别「上下文溢出」类（OpenAI/Anthropic 各一条最常见串即可起步），命中则：压缩当前会话上下文 → **用同一轮已完成的工具结果继续**（不重放工具）→ 重试一次；再失败就走既有硬截断。这一条能直接消灭长会话里一大类失败。 |
| M45 | **压缩质量门（轻量版）** | `SummarizationMiddlewareHandle::doSummarizeWithLLM` 之后 | P2 | 对 LLM 摘要做最小校验：非空、长度在预期范围内、包含压缩指令要求的固定小标题（如「已完成」「待办」「关键标识符」）；不合格则**重试一次**，仍不合格就退回确定性压缩而不是写入坏摘要。 |
| M46 | **压缩挪到回复结算之后（可选路径）** | `SummarizationMiddlewareHandle` + `SessionServerAgentIO` 的轮次收尾 | P2 | 现状压缩在轮内。可保留「必需压缩」（阈值触发，必须在请求前做）在轮内，把「压缩质量优化/二次整理」放到回复投递完成后、用本轮剩余预算做；若用户已经发下一条消息则先取消它。注意 agentxx 的并发模型（单 io_context）下「后台做」意味着「轮次结束后另起一个协程」，需要确保它与下一次轮次不会同时写会话（可用 M21 的 writerEpoch 保护）。 |
| M47 | **压缩前提醒写记忆** | `SummarizationMiddlewareHandle`（压缩前追加一条提示） | P2 | 成本极低：压缩前插入一条「先把值得长期保留的信息写入记忆文件」的自动提示消息，能显著提高跨会话记忆质量（openclaw 直接把它做成压缩流程的一部分）。 |

### 9.6 实现细读（本轮新增）

#### agentxx：一次压缩的完整状态机（`summarization.cpp::onModelcallRunFunc`）

逐行读下来，真实流程比 §3.1 的表格更细，且每一处都有防重复/防震荡的考虑：

```
读会话消息副本 + 模型上限(模型配置 > 默认 256K) + sendThinking
tokenUsage = apiTokenUsage > 0 ? apiTokenUsage : 本地估算        ← 发布到 session->contextStats
if (tokenUsage < 75%) 直接返回
  ├─ 提示消息: 首次 → InsertMessage 并记住 id(graphData.summarizationTipMsgId)
  │             resume 后 → 用同一 id 发 UpdateMessage（避免重复的"正在压缩"提示）
  ├─ 确定性压缩: doSummarizeToolcall(去重/探索折叠) + cleanNoiseMessages
  ├─ 冷却判断: failCount==0 且 lastMsgCount>0 且 messages.size() <= lastMsgCount+2
  │            → 跳过 LLM 压缩（否则每轮都派生 subagent 做无效压缩）
  ├─ 切分: recentBudget = 上限 × 0.20 → splitRecentByTokenBudget(轮次对齐)
  ├─ 待压缩段 = [system] + [systemCount, oldEnd)；多模态 data URL 降级为文本标签
  ├─ doSummarizeWithLLM(同 threadid 同模型, 直接给消息不走 share_store)
  ├─ 成功 → Compact: [system] + user"请压缩" + assistant"[Previous conversation summary]" + recent
  │  失败 → failCount++；failCount>=2 或 tokenUsage>=95% → HardTruncate
  └─ 兜底: 压缩后仍 >=95% → HardTruncate（覆盖"摘要本身超长/单条消息超大"）
写回: replaceMessages + updateMessagesMeta + 更新冷却基准 + 更新提示文本与 contextStats + 清 tip id
```

其中两处值得单独记：

- **冷却基准用的是「消息条数」而不是 token**：压缩成功即记录压缩后的条数，若下次再来时消息只增长了 ≤2 条而占用仍 ≥75%，说明 LLM 摘要没能真正减少条数（例如工具结果被反复回填），继续压缩是无用功，于是直接跳过 LLM 压缩。这比「按时间冷却」更贴合真实病根。
- **`hardTruncate` 有两级兜底**：① `system + 截断说明 + recent(30% 预算)`；② 若仍超 95%，则**从最旧的 recent 开始丢**（至少保留最后 1 条，会话不能为空）；③ 若只剩 1 条还超，就对这条的 `content` 做**二分截断**（保留开头，消息结构与角色不变），保证请求载荷一定发得出去。

另外，压缩前的 `downgradeMultimodalUrlsToText` 会把 Base64 data URL 换成文本标签再送给摘要子代理——源码里挂着一条 TODO「替换前存储为文件并记录路径」，这与 openclaw 的 `[image removed during context pruning]` 是同一个问题的两种处理（openclaw 的占位符更明确，且带缓存过期语义）。

#### openclaw：溢出恢复与「压缩质量门」在实现层的两条约束

结合文档与 `attempt.overflow-compaction.harness.ts` / `compact.queued*.ts` 的实现思路，两条约束值得单独提出：

- **溢出恢复只认「已结算的工具结果」**：工具调用全部完成（已结算）时才能「压缩并从记录结果继续」；pending 工具、审批中、已取消、或有意结束本轮的工具都保留各自原有处理。这条把「重放已完成动作」的风险从设计上排除。
- **压缩质量门「不合格就不写」**：safeguard 挡位下最终摘要预算在**校验前**应用，必需标题/待办/精确标识符必须留在将被存储的文本里；不合格只允许配置次数的纠正尝试，仍不合格则**停止压缩、保留原历史**并上报既有恢复结果。

**对本文结论的影响**：agentxx 的三层降级（确定性 → LLM → 硬截断）与 openclaw 的「质量门 + 不合格不写」是互补的两条路：前者保证「请求一定发得出去」，后者保证「写进去的摘要不劣化」。本文原先只提了 M44（溢出恢复）与 M45（质量门），本轮补充说明：**建议先做 M44**（缺的是恢复路径），质量门（M45）优先级低于它，因为 agentxx 已有硬截断兜底不会卡死。

---

## 10. 权限、审批与安全边界

### 10.1 agentxx 现状

**声明式目标 + 宿主统一判定**（`PermissionMiddlewareHandle`）：

| 组成 | 说明 |
|---|---|
| 权限声明 | 插件在注册工具后经 `agentxx.agent.permission` 声明：作用域（读=0 / 写=1）、目标来源（None / Path / Text）、目标参数名（多个，任一被拒即拒）、分类文本；目标值按**参数实际 JSON 类型**处理（字符串＝单目标，数组逐项判定） |
| 规则表 | `XXRouter<PermissionOperator, 2>`：最长路径匹配 + `*` 通配符；两套作用域分别维护；`ALLOW` / `DENY` / `INTERRUPT`（询问） |
| 兜底 | `noRuleOperator`：默认 `ALLOW`；CodeAgent 按 `permission.mode` 设置（`ask`/`all_ask` → INTERRUPT，`pass` → ALLOW，`deny` → DENY） |
| 判定顺序 | ① 工作区隔离写拒绝 → ② 配置显式拒绝路径（始终拒绝且不询问）→ ③ 完全授权 → ④ 规则表命中 → ⑤ `noRuleOperator` 兜底 |
| 完全授权 | `fullAuthorized_`，状态变化经总线发布并广播给所有客户端（多端一致）；客户端可切换（`WireSetFullAuth`） |
| 记住选择 | 询问应答带 `remember` 时，为该目标注册允许/拒绝规则，后续同目标及其子路径不再询问 |
| worktree 隔离 | 每会话 `SessionFsIsolation{allowPath, denyWritePath}`：命中 `denyWritePath` 的写直接拒绝（**隔离优先于白名单与模式默认规则**），`allowPath` 是该约束的例外子树；读不受限 |
| 路径规范化 | `normalizePermissionPath`：绝对路径 + Unix 分隔符（Windows 转小写）+ 目录保留尾斜杠/文件去尾斜杠；相对路径按**会话生效工作目录**解析（worktree 绑定优先） |
| 只读批量判定 | `decidePaths()` / `decideTarget()`：**不发询问**，`INTERRUPT` 场合返回 `Ask`；供模式/前缀参数工具（glob/grep/list）在枚举出实际路径后**逐项复核**，避免 `**` 绕过子目录拒绝规则 |
| 询问通道 | 经**会话总线**发起（应答方是绑定到会话的 IO 端点），工具权限检查服务注册在 **agent 全局总线** |

**没有的东西**：没有审批记录持久化（记住的选择在进程内）、没有「运行期权威复验」概念、没有执行身份绑定（见 §6 的 M31）、没有沙箱。

### 10.2 openclaw 现状

**安全定位**（`VISION.md` + 根 `AGENTS.md`）：

> 「安全是 OpenClaw 里一个**有意的权衡**：强默认值但不杀死能力。目标是保持对真实工作的强大，同时让高风险路径显式且由操作员控制。优先采用限制最少的有效保护；在信任模型与审批边界内，有界且被理解的风险为明显的可用性收益是可以接受的。解释权衡，而不是加更多门。」

**三层权限结构**：

| 层 | 机制 |
|---|---|
| 工具策略 | 六道过滤（profile/allow-deny/provider/沙箱/通道/插件可用性）决定「模型能看到什么」；可用性 ≠ 授权 |
| 审批（exec approvals） | 本地执行主机上强制；`policy + allowlist + 用户批准` 三者都同意才运行；审批只能收紧不能放松；绑定 cwd/argv/env/可执行真实路径/内容哈希，批准后漂移即拒绝 |
| 操作员角色（operator scopes） | 「命名操作员角色」定义权限上限；排队或委派给子会话的网关输入**保留原始操作员与作用域上限**（不会借用当前活动或最新发送者的权限）；被撤销的设备权限不会因为「turn 已被接受」而延长 |

**其它安全机制**：

- `permission.mode` 类语义（`deny` / `allowlist` / `ask` / `auto` / `full`）与 Codex Guardian 映射、ACPX harness 权限。
- 设备信任：设备身份 + 配对批准 + 设备令牌 + `connect.challenge` nonce 签名（v3 绑定 platform 与 deviceFamily）+ 本地回环例外。
- 网络侧：Tailscale/SSH 隧道优先，TLS + 可选 pinning，`gateway.auth.mode`（token/password/none/trusted-proxy/Tailscale Serve）。
- 威胁模型文档（`security/THREAT-MODEL-ATLAS.md`、`CONTRIBUTING-THREAT-MODEL.md`）、形式化验证文档、事件响应文档；CodeQL 有专门的 `plugin-trust-boundary` / `process-exec-boundary` / `network-ssrf-boundary` 规则集。
- 审计：网关把 lifecycle 与工具起止事件投影到**有界、仅元数据**的审计账本（记录来源与结果码，**不复制**提示词、消息、工具参数与结果）。
- 凭据：`secrets` 工具的「元数据先行 + 需要时才请求掩码值 + 返回 SecretRef」；`docs/reference/secret-placeholder-conventions.md`。

### 10.3 对比

| 维度 | agentxx | openclaw |
|---|---|---|
| 权限模型 | 声明式目标 + 宿主统一判定（规则表 + 模式兜底 + 隔离 + 完全授权 + 记住选择） | 工具可见性策略 + 本地审批门 + 操作员角色上限，三层分开 |
| 谁决定「要不要问」 | 宿主（规则命中 INTERRUPT 就弹卡片） | 工具策略与审批文档（`ask: on-miss` 等模式）共同决定，且审批只能收紧 |
| 路径级判定 | 有，且能做只读批量复核（`decidePaths`）逐项过滤 | 有路径策略（`path-policy.ts`）与 `tool-fs-policy`，但粒度与「模式参数逐项复核」的显式描述不如前者 |
| 审批记录 | 进程内（记住选择） | 主机本地审批文档 + 项目级/设备级持久化，可查询（`openclaw approvals get`）与撤销 |
| 执行身份绑定 | 无 | 有（argv/env/cwd/真实路径/内容哈希 + 漂移即拒绝 + 绑不出唯一文件就拒绝） |
| 运行期权威复验 | 无显式概念（单控制器内天然串行） | 有明文要求：特权动作需「当前 owner 持有的权威」，await 之后与副作用之前都要复验；「token/签名/未过期/ID 匹配」单独不构成权威证明 |
| 撤销与作用域 | 会话内状态，随进程结束 | 设备权限撤销、操作员角色降级、排队输入的来源上限保持 |
| 隔离 | worktree 写边界（隔离优先） | 沙箱工作区 + 沙箱工具策略 + 节点命令 allowlist |
| 审计 | 有会话库 + 事件流（可含内容） | 有界**仅元数据**审计账本（有意不含提示词/消息/参数/结果） |
| 安全文档 | 分散在 design 文档 | 专门威胁模型 + 形式化验证 + 事件响应 + 安全报告流程 |

### 10.4 两者的优缺点

**agentxx**

- 优点
  - **声明式目标是很合适的分工**：工具作者声明「哪些参数是受约束目标、属读还是写」，宿主只管统一判定；新增工具不需要改权限核心。
  - **只读批量复核（`decidePaths`）解决了一个真实绕过**：模式/前缀参数工具先枚举实际路径再逐项判定，避免 `**` 之类模式绕过子目录拒绝规则；并且「无法规范化时按 Ask 而不是按已批准」是错误的正确方向。
  - **判定顺序清晰且隔离优先**：`denyWritePath` 优先于白名单与模式默认，配合 worktree 场景语义明确。
  - **多端一致的状态广播**：完全授权状态变更经总线广播给所有客户端，避免多端界面不一致。
- 缺点
  - **审批记录不持久化**：记住的选择在进程内，重启即失效；用户要重新批准一遍。
  - **没有执行身份绑定**：批准的是「这次调用」，实际执行时重新解析——如果命令、路径或文件内容在批准与执行之间发生变化，没有检测机制。
  - **缺少运行期权威复验的明文规矩**：目前靠「一个会话一个控制器」的结构保证；一旦引入并行工具（M26）或子代理直接执行特权动作，这条需要显式化。
  - **审计可能含内容**：会话库里保存完整上下文（这是设计需要），但没有「仅元数据审计」这条可选路径——对合规/多用户部署可能不够。

**openclaw**

- 优点
  - **审批的绑定与漂移检测做到了工程上少见的程度**：argv/env/cwd/真实路径/内容哈希，且绑定不出唯一文件就拒绝铸造批准。
  - **「可用性 ≠ 授权」被写进规则与文档**，避免了「工具没出现＝没权限」的常见误判；同时保留 server 侧校验、工具授予与实时执行权威。
  - **权限上限随输入传播**：排队/委派的输入保留原始操作员与作用域上限，不会借用更宽权限；被撤销的设备权限不会因「turn 已接受」而延续。这条在多用户/多客户端场景下很关键。
  - **审计账本有意只存元数据**：`prompt`/消息/参数/结果不复制出 transcript，降低二次泄露面。
  - **安全定位明确写在产品文里**：强默认 + 显式旋钮 + 解释权衡，而不是无限加门。
- 缺点
  - **机制面广、使用者需要理解多套概念**（工具策略/审批/操作员角色/沙箱/设备信任），且规则之间有「取更严者」这类组合语义。
  - **审批机制与执行主机绑定**：网关主机与节点主机的审批状态各自独立，跨设备行为需要运维理解差异。
  - **文档量大**：`SECURITY.md` 38 KB + 威胁模型 + 形式化验证 + 各绑定细节，落地成本不低。

### 10.5 可迁移到 agentxx 的设计

| 编号 | 设计 | 落地位置 | 优先级 | 说明 |
|---|---|---|---|---|
| M48 | **审批记住的选择持久化** | `settings_db` 或会话库（新增 `permission_rule` 表） | P1 | 现在「记住选择」只在进程内存里。落到 `settings_db`（全局）或按会话存储，配合一个可查询/可撤销的入口（TUI 设置里列出已记住的规则并支持删除）。这是用户体感最明显的一条。 |
| M49 | **执行身份绑定（配合 M31）** | `execute_command` 等工具的批准流程 | P2 | 批准时记录解析结果（可执行绝对路径 + argv + cwd + 需要时的内容哈希），执行前复验；不一致就拒绝并要求重新批准。 |
| M50 | **运行期权威复验的显式规矩** | `docs/zh-cn/design/index.md` + `permission.h` 注释 + 并行工具实现（M26） | P1 | 写清三条：① 权限判定如在 `co_await` 之前做，则副作用之前必须复验；② 并行工具派发前统一做权限询问（不要在各自协程里分散询问）；③ 取消后不得再执行已判定的特权动作。这是 M26 的前置条件。 |
| M51 | **权限规则查询/撤销入口** | TUI 设置面板 + `agentxx.agent.permission` 表扩展 | P2 | 提供「当前生效的规则列表（来源：配置 / 记住的选择 / 工作区隔离）」与撤销入口。openclaw 有 `openclaw approvals get` / `exec-policy show`；agentxx 层数少，做成 TUI 里一个列表即可。 |
| M52 | **仅元数据的审计轨迹（可选）** | 会话库新增 `audit_event` 表（工具名/结果码/来源/时间，**不含参数与结果内容**） | P2 | 面向多用户或需要事后追责的部署。与 §5 的 M23（会话事件序列）可以合并设计：同一张表既做状态信号又做审计，但**字段口径必须严格**（只存元数据），避免把内容复制出 transcript。 |
| M53 | **凭据收集走掩码流程（若将来加凭据类工具）** | 相关工具与 TUI 输入 | P2 | openclaw 的 `secrets` 工具是「元数据先行 + 需要时才请求掩码值 + 返回引用」。agentxx 目前凭据都在配置文件里，暂不需要；若将来支持「让 agent 帮你配 key」，必须走掩码输入而不是让用户贴进对话。 |
| M95 | **权限判定取符号链接的真实路径（已存在的已知缺口）** | `PermissionMiddlewareHandle::decideTarget` / `normalizePermissionPath` | P2 | 源码里已有 `TODO(符号链接跟随)`：判定基于**词法规范化路径**，不解析符号链接，因此允许范围内的链接（`<root>/link -> <root>/deny`）被读写时会跟随链接进入被拒目录，`decidePaths` 逐项过滤也看不到链接目标。彻底处理需对已存在路径取 `std::filesystem::weakly_canonical` 再判定一次（影响所有工具与查询接口，需评估性能与 Windows 语义）。建议按「写操作优先、仅在路径被拒绝风险高时才做二次判定」的折中实现，并把该限制写进安全说明（M17）。 |

### 10.6 实现细读（本轮新增）

#### agentxx：判定顺序与「谁注册规则」都有明确理由

`permission.cpp::decideTarget` 的实际判定顺序（与 §10.1 表格一致，但实现里有几处细节值得记）：

```
1. 目标为空 → 返回 Ask（"无法判定时不按已批准处理"）
2. worktree 隔离:
   命中 allowPath（worktree 子树）→ 跳过下面的主检出写拒绝，继续按规则处理
   否则 index==WRITE 且命中 denyWritePath → 直接 Deny（读不受限）
3. 命中配置显式拒绝路径 → 直接 Deny（在"完全授权"之前判定，即配置文件的黑名单不会被全权授权绕过）
4. isFullAuthorized() → Allow
5. 规则表最长前缀匹配 → ALLOW / DENY / Ask
6. 未命中 → noRuleOperator（CodeAgent 按 permission.mode 设置；默认 ALLOW）
```

两处实现级结论：

- **`allowPath` 必须先于 `denyWritePath` 判定**。源码注释解释了原因：真实 worktree 位于主检出的 `.agentxx/agent/worktrees/` 下，即 `allowPath` 本身**就在** `denyWritePath` 子树内；顺序写反会导致「绑定 worktree 后无法写任何文件」。
- **「记住本次选择」的注册归属在中间件，不在 IO 端点**。`requestPermission` 注释写明了原因：应答方（IO 端点）经**会话总线**发事件，而权限规则表由**agent 全局总线**上的中间件持有，若让端点注册规则则事件根本到不了（表现为「勾选记住后下次访问仍反复询问」）。规则表按最长前缀匹配，因此目录级规则自动覆盖其全部子目录与文件。
- 询问等待时间同样是 `milliseconds{0}`（不限制），与 §2.6 的 HIL 超时口径一致——**凡是等人回应的路径都显式关掉总线默认 30s 超时**，这是 agentxx 在这类交互上的一致做法。

#### openclaw：审批的绑定对象是「解析后的执行身份」

`docs/tools/exec-approvals.md` 描述的绑定项在实现侧对应一组具体对象：`cwd`、精确 `argv`、env 绑定、可执行文件**真实路径**（可写可执行文件再加**内容哈希**）、shell 脚本/解释器调用的**单个具体本地文件**；并有两个反向约束：**批准窗口内解析结果变化即拒绝**，以及**绑定不出唯一具体文件时拒绝铸造批准**（而不是假装覆盖）。它同时明确审批「不是逐用户认证边界，也不是文件系统只读策略」——把「审批解决什么问题、不解决什么问题」写清楚，避免被当成万能安全层。

**对本文结论的影响**：① M31/M49（执行身份绑定）现在有了明确的绑定清单与两条反向约束；② 新增 M95（符号链接规范化）并关联 M17；③ 「凡等人回应的路径关掉默认超时」这条一致性做法值得写进 §10 的规则文件（并入 M1）。

---

## 11. 子代理、后台任务与并行

### 11.1 agentxx 现状

**子代理就是「独立 agent」**（`AgentHost` + `AgentRegistry`）：

- 主 agent 与子代理**完全平等**：每个节点持有独立的 `BaseAgent` 实例（独立 `AgentContext` / engine / `SessionStore` / 中间件栈），节点间**不共享可变状态**；宿主共享 `io_context` 与线程池。
- **委派走「中断 + 宿主派生」**，不是图内嵌套子图：`agentxx_subagent` 工具构造 `ReqSubagentBatch` → `NodeInterrupt` → `AgentRunner` 的中断处理循环 → 宿主**全局总线**上的 `service.subagent`/`service.subagent.batch` 服务 → `spawnBatch()` 派生。子代理总线在派生时**对称注册**同样的服务，因此嵌套委派与根委派走**完全同一条路径**（扁平化，不会因层级不同出现行为差异）。
- **预算**：`maxDepth`（默认 3，根 = 0，超出拒绝派生）、`maxConcurrentSubagents`（默认 8）；嵌套深度按 `sessionId` 记录在 `sessionDepth_`。
- **取消级联**：父会话的取消令牌透传给子代理，父取消即中止全部在跑的子代理。
- **HIL 冒泡**：子代理的权限/中断类请求经**子代理会话总线冒泡到父 IO**（`parentAgentCtx` 指定父 agent 上下文；嵌套时是上一级子代理而非根）。
- **各 agent 独立记录**（按 `thread_id` 取值）：`Agent_IO`、`CancelToken`、上下文统计、模型选择都按会话/线程隔离。
- **同上下文模式**（压缩场景的关键）：`spawnBatch` 支持 `messages`（结构化消息透传作为初始上下文，可含 system）与 `sessionId`（子代理运行在指定 thread 而非独立线程，并**强制使用该 thread 父会话的当前模型**）；两者配合保证「相同上下文前缀 + 相同 threadid + 相同模型」，以命中 provider 的 KV/prefix cache。
- **工具策略**：`tools` 缺省 = 默认全量；`[]` = 无工具；`["*"]` = 全量继承父 agent 工具（解析为父工具名白名单）；`["name", …]` = 自定义白名单。`enableSummarization` 缺省继承父配置（压缩派生的子代理必须显式 `false`，避免二次压缩）。
- **结果回填**：`makeSubagentResumeKey(toolCallId, resultId, idx)` 是写入侧与读取侧**共用的唯一 key 规则**（前缀避免同轮多个中断的序号 key 互相覆盖）；单任务返回纯文本，多任务按任务序号编号，错误任务写成 `{"error": ...}`。
- **跨 agent 消息**：`HostBus` 的 `agent.message` RR + `Mailbox` 注册；远程 agent 经 A2A 桥接（`registerRemoteAgent` + `SendMessage`/`GetTask` 轮询），**本地 agent 与远程 agent 在消息面完全同构**。

**后台任务**：目前主要是压缩（可经 `direct=true` 在空闲时直接派生子代理）与 `tool_skill_search` 检索子代理；没有通用的「定时任务 / 常驻指令 / 离线整理」框架。

### 11.2 openclaw 现状

**子代理是「真会话」**：

- 每个子代理运行在自己的会话（`agent:<agentId>:subagent:<uuid>`）里，默认**把结果 announce 回请求者**供检查；子代理运行受原生子代理生命周期所有者跟踪。
- **上下文模式**：默认隔离（自己的上下文与 token 用量）；需要父会话当前 transcript 时用 `context: "fork"`（线程绑定的子代理会话默认 `fork`，因为它把当前对话分叉成一个后续线程）。
- **工具面刻意收窄**：子代理**默认拿不到 session 与 message 类工具**；有专门的「子代理工具策略」文档与 `inherited-tool-deny` / `subagent-tool-policy` 机制。
- **嵌套与深度上限**有专门文档（`docs/tools/subagents/nesting.md`：深度上限、announce 链、鉴权）。
- **并发**：`agents.defaults.subagents.maxConcurrent`（每派生会话默认 8）+ `maxChildrenPerAgent` 准入上限；**群组（Swarm）收集器子代理**用 `subagent:swarm:<group>` 车道，默认上限 32，**不占用父会话的普通子代理额度**；群组另有 `maxChildrenPerGroup` / `maxTotalPerGroup` 准入上限。
- **协作原语**：`sessions_yield`（子代理可以让出并等待父继续）、`sessions_send`（跨会话发消息，可 `watch: true` 注册观察者）、`agents_wait`、`subagents` 列表；`thread-bound sessions` 把子代理绑到某个通道线程。
- **完成语义**：announce 型子代理把完成事件推给父（非阻塞、push-based）；收集器型需要显式收集结果；文档强调「**子代理完成 ≠ 被委派的用户目标完成**」，持久会话在范围内还有工作时应继续。

**后台工作预算**：

- 技能工作坊评审、插件后台完成、**做梦（dreaming）**共享一个**独立的 3 个并发槽位**预算；工作坊最多占 1 个，每个插件最多占 3 个可用槽位。
- 「等待后台工作的调度器**本身不占**这个预算」——只有被派发的工作在完成或取消清理前占槽位，因此调度器不会阻塞它等待的子任务。
- 已取消的排队工作在开始前被移除；网关重启或运行时退休会阻止陈旧完成开始或返回结果。
- Control UI 有一个「系统繁忙度」总览与 `diagnostics.lanes` 把这类工作统一报成一行 `background`。

**定时与心跳**：`cron` 车道与 `cron-nested` 车道（隔离的 cron agent 回合持有 `cron` 槽位，内部 agent 执行用 `cron-nested`）；心跳（heartbeat）嵌入运行走 `cron-nested` 做全局准入，避免慢的后台工作挡住入站回复；`standing intents`（常驻指令）+ `cron` 构成「前瞻性记忆」。

### 11.3 对比

| 维度 | agentxx | openclaw |
|---|---|---|
| 子代理本质 | 独立 `BaseAgent` 实例（独立 AgentContext/engine/SessionStore），与主 agent 平等 | 真会话（`agent:<agentId>:subagent:<uuid>`），有独立 transcript 与用量 |
| 派生方式 | 中断 + 宿主服务（`NodeInterrupt` → `service.subagent*`），嵌套与根同路径 | 工具调用（`sessions_spawn`）+ 原生 subagent 生命周期所有者 |
| 上下文模式 | `messages` / `sessionId`（同上下文，命中 KV cache）或独立线程 | `isolated`（默认）/ `fork`（有父 transcript） |
| 深度与并发 | `maxDepth`（3）+ `maxConcurrentSubagents`（8） | 每派生会话 8 + `maxChildrenPerAgent` + 群组独立额度（默认 32） |
| 工具策略 | `[]` / `["*"]` / 白名单；`enableSummarization` 可关 | 默认收窄（无 session/message 工具）+ 独立策略文档 + 继承拒绝机制 |
| 结果回传 | 中断 resume 回填（单任务纯文本 / 多任务编号 / 错误对象） | announce 回请求者（push-based）+ 收集器模式需显式收集 |
| 取消 | 父→子级联（透传取消令牌） | 停止子树 + 运行可中止性查询 |
| 跨 agent 消息 | `HostBus` `agent.message` RR + Mailbox + A2A 桥接（本地/远程同构） | `sessions_send`（可注册观察者）+ `sessions_yield` + `agents_wait` |
| 后台工作预算 | 无（后台任务与前台共用资源） | 独立 3 槽位（工作坊/插件后台/做梦），且「等它的调度器不占槽位」 |
| 定时/常驻 | 无 | cron 车道 + 心跳 + 常驻指令 |
| 完成语义 | 子代理返回即完成（结果回填给模型） | 明确区分「子代理完成」与「用户目标完成」，有防止误判的提示与流程 |

### 11.4 两者的优缺点

**agentxx**

- 优点
  - **「子代理 = 独立 agent」的实现很干净**：不共享可变状态，回收即整体析构（`Session`/中间件状态随 `AgentContext` 释放），不存在按 thread 累积泄漏；嵌套与根委派同路径（扁平化）避免层级导致的行为差异。
  - **同上下文模式（`messages` + `sessionId` 同 thread 同模型）是为压缩场景专门设计的**，直接命中 provider KV cache，是很懂成本的一笔。
  - **取消级联 + HIL 冒泡**：中断与权限请求能穿过层级的完整链路，且 key 规则两侧共用（明确防漂移）。
  - **A2A 桥接让远程 agent 与本地 agent 消息面同构**。
- 缺点
  - **子代理上下文开销固定为「整份独立」**：没有「fork 父 transcript」这类半共享模式，需要父上下文时必须显式传 `messages`（要调用方自己准备），普通研究与长任务场景下要么重复劳动、要么无法复用父探索结果。
  - **没有后台工作预算**：压缩、检索等后台类子代理与前台轮次抢同一份资源；多会话并发时容易互相拖慢（与 §2 的 M9 同源）。
  - **没有定时/常驻任务**：无法表达「明天提醒我」「每小时检查一次」这类需求。
  - **完成语义只有「返回结果」一种**：没有「完成 ≠ 目标达成」的显式提示，父模型容易把「子任务返回了文本」当成「事情办完了」。

**openclaw**

- 优点
  - **子代理会话可持久化与可交互**：可见子代理成为会话树里的普通会话，能继续对话、能被 steer；审计与人工介入都自然。
  - **预算分域细致且有一条漂亮规则**：「等待后台工作的调度器不占槽位」——避免调度器阻塞自己等待的子任务这类死锁型设计错误。
  - **完成语义被认真对待**：明确「子代理完成 ≠ 用户目标完成」，并在提示词层面要求继续持久会话。
  - **协作原语成体系**：yield / send / watch / wait / spawn 覆盖了编排里绝大多数形态（还有 swarm 的收集器模式）。
  - **上下文模式可选**：`isolated` 省钱、`fork` 给需要父上下文的场景，选择权在调用方。
- 缺点
  - **子代理是「会话」，因此承担会话的全部机制重量**（transcript、写者租约、车道、announce 状态），实现与调试成本高。
  - **编排语义多**（announce/collector/yield/watch/thread-bound/嵌套深度/群组额度），要写清这些组合本身就是大量文档（子代理文档拆成 7 页）。
  - **群组/收集器等形态偏产品化**（swarm 是它自己的编排产品），对 agentxx 这类嵌入库不一定适用。

### 11.5 可迁移到 agentxx 的设计

| 编号 | 设计 | 落地位置 | 优先级 | 说明 |
|---|---|---|---|---|
| M54 | **子代理「fork 父上下文」模式** | `AgentHost::spawnBatch`（`SubagentBatchItem` 增加 `contextMode`） | P1 | 现有 `messages`/`sessionId` 是「同上下文」的全量透传。补一个 `fork`：由宿主自动取父会话当前上下文（可按 token 预算裁剪 + 去掉工具结果细节），作为子代理初始上下文。收益：子代理不必从零探索，尤其适合「深挖某个已发现的线索」这类任务。 |
| M55 | **子代理默认收窄工具面** | `makeSubagentConfig` / `SubagentBatchItem.tools` 默认值 | P1 | 现状缺省是「默认全量工具」。建议默认改为：**不给**会话/消息/子代理类工具（避免递归委派与跨会话写），需要时显式开；这与「工具面难以被误用」的取向一致，也降低子代理误伤父会话的风险。 |
| M56 | **后台工作独立预算** | `AgentHost`（新增后台槽位计数） | P1 | 与 §2 的 M9 同批实现：把「压缩整理 / 技能检索 / 会话收尾整理」等后台类子代理放进一个独立的小预算（例如 3 个槽位），与前台轮次分开；并采纳 openclaw 的一条细节：**等待后台工作的调用方不占槽位**，只有真正在执行的任务占。 |
| M57 | **「完成 ≠ 目标达成」的显式提示** | `SubAgentManagerTool` 的定义文本 + 子代理结果模板 | P2 | 在工具描述与结果前缀里写明：「子代理完成仅表示这一次委派结束，不代表用户目标完成；若范围内仍有工作，应继续推进」。成本极低，能减少「父模型看到子代理返回就把任务标记完成」的误判。 |
| M58 | **定时/常驻任务（轻量版）** | 新插件（如 `agentxx_scheduler`）+ 会话或全局 sqlite 存任务 | P2 | 对应 openclaw 的 cron + standing intents。最小形态：一张任务表（时间/周期/提示词/目标会话）+ 一个基于 `steady_timer` 的调度协程，到点后在目标会话里跑一轮；掉电重启后从表恢复。适合「每天早上总结昨天的工作」这类需求，也是 agentxx 相对薄弱的一块。 |
| M59 | **跨会话观察者（对应 `watch`）** | `HostBus` 扩展事件 | P2 | 与 §5 的 M23（会话事件序列）配套：父会话可注册「关注某子会话的关键变化」，在变化时收到一条合并通知（而不是轮询）。openclaw 的「同一 watcher/target 只发一条通知 + 冻结水位 + 自身事件不通知自己」是可直接照搬的三条规则。 |

---

## 12. 大输出、附件、媒体与结构化询问

### 12.1 agentxx 现状

**附件（`MediaAttachment`）**：

- 两种形态：base64 dataUrl（内嵌）或 `pathOrUrl`（路径/URL）。BaseAgent 收到「dataUrl 为空且 `pathOrUrl` 是本地路径」的附件时，**由服务端按路径自行读取编码**（卸载到线程池），客户端不需要把文件内容传过来。
- **跨设备选择**：远程模式下客户端与服务端比对 `deviceId`（HelloAck 携带服务端设备 id 与会话工作目录），不同设备时文件选择弹窗多出「本地 / 服务端」标签页；服务端页经 `ListDir`/`ListDirResult` 列举（服务端线程池扫描，路径全程 UTF-8）。
- 多模态能力由模型配置提供，客户端据此决定是否显示附件按钮（见 §8.1）。

**大内容**：`share_store`（会话库 `store` 表）——工具可以把大内容暂存并只把引用（id）放进上下文；内存只保留最多 3 条 LRU 缓存，内容按 id 按需读回。配套的 `agentxx_share_store` 工具支持 `line_offset` / `line_limit` 按行分页取回（并启用了重复调用检查）。**这是 agentxx 独有的、专门为「大输出不进上下文」设计的一层**，且工具输出的自动 offload 复用它（§6.6）。

**工具输出的压缩与暂存**：`autoSummaryOutput`（超阈值自动压缩，见 §6）；MCP 工具结果、图片结果由 `XXToolWrap`/`tool-media-payloads` 类似逻辑处理（有图像 payload 尺寸限制与日志洁净处理）。

**结构化询问**：通过**中断机制**实现——`InterruptUi*` 系列描述（头行分段、文本/markdown/差异/分隔线/空行/控件/提交行/自定义块），控件形态有 buttons/select/checkbox/radio/text/textarea/number 等，值变化即派发，提交经 `__submit`/`__cancel` 动作回传 `{"values":{...}}`。使用场景：**权限询问卡片**（含「完全授权所有权限」勾选）、**重复调用警告**、插件自定义询问。中断描述与面板/Info/overlay **共用同一套组件描述与渲染实现**。

**媒体理解与生成**：没有。agentxx 能接收图片/音频/视频输入（取决于模型能力），但没有「读图中文字」「生成图片」「语音合成」这类内置能力层。

### 12.2 openclaw 现状

**媒体能力是一整层**：

- 理解类：`view_image`、`media-understanding`（节点侧媒体理解文档）、`document-extract`、`web-readability`、`pdf` 工具；`packages/media-*` 与 `src/media-understanding`。
- 生成类：`image_generate`、`music_generate`、`video_generate`、`tts`（各有独立文档，`video-generation.md` 32 KB）；`packages/media-generation-core`、`src/media-generation`；有后台生成任务（`media-generate-background-shared.ts`）与进度上报（Runtime Context 里包含媒体生成进度快照）。
- 播放/传输类：`media-playback`（通过节点播放音视频）、`nodes/media-*`、音频与语音留言文档。

**富输出协议（rich output protocol）**——助手输出携带**交付/渲染指令**的几条专用通道：

| 通道 | 用途 |
|---|---|
| 结构化 `mediaUrl` / `mediaUrls` 字段 | 附件交付（首选路径） |
| `[[audio_as_voice]]` | 音频呈现提示 |
| `[[reply_to_current]]` / `[[reply_to:<id>]]` | 回复元数据 |
| `[embed ...]` | Control UI 富渲染（**仅 Web**，不是媒体别名） |
| `MEDIA:` 行 | 兼容路径（仅在自动可见回复模式下，且限定「行首、非 markdown 包裹、非代码块」） |

安全约束写得很明确：**远程附件必须是公网 `https:` URL**（拒绝 `http:`、回环、链路本地、私有地址与内部主机名），本地附件要走 agent 文件读策略与媒体类型检查；**工具/插件/浏览器输出/消息动作/流式块负载必须使用结构化字段而不是文本命令**。

**附件引用**：入站附件可用 `media://inbound/<id>` 引用（网关解析为已存文件并施以同样的文件读与沙箱检查）；Control UI 里相对路径按**会话工作目录**解析（含选中的项目/worktree）；**另一个执行主机上的文件必须先作为托管附件交付**。

**结构化询问（`ask_user`）**：

- 让 agent 向人问 1–3 个结构化问题并等待回答；**只在主会话可用**（子代理拿不到）。
- 各界面均有原生呈现：Control UI 在输入框上方停靠问题面板（多问题带步进器）；TUI 支持方向键/数字键选择、**Other…** 自由输入、Skip、多选切换、Esc 返回后 `/question` 重开；Telegram 单问题单选渲染成全宽原生按钮；Discord/Slack/Mattermost 渲染原生按钮。
- 纯文本回答也被接受（数字/选项标签/自答；多选用逗号），但要求**回答者权限与创建者匹配**。
- 生命周期细节：`registerPendingAgentQuestion`、问题阶段机、`QUESTION_RPC_GRACE_MS`、超时/过期后保留措辞、重连后恢复待答问题、**回答不授予额外权限**。
- 明确要求：**永远不要用 `ask_user` 回答凭据**；需要 API key 时走 `secrets` 工具。

### 12.3 对比

| 维度 | agentxx | openclaw |
|---|---|---|
| 附件输入 | base64 或路径；跨设备选择（本地/服务端标签页） | 结构化字段 + `media://inbound/<id>` + 托管附件；远程只允许公网 https |
| 大内容暂存 | **有**（`share_store`：内容进库、上下文只放引用） | 有（大输出落盘 + 预览；`MEDIA:` 兼容行） |
| 媒体理解 | 无（仅模型自身多模态输入） | 有（图片/文档/PDF/网页可读性 + 节点侧媒体理解） |
| 媒体生成 | 无 | 有（图像/音乐/视频/语音，含后台任务与进度） |
| 富输出指令 | 由声明式组件描述承担（见 §13） | 专用通道（结构化字段 + `[[...]]` 标签 + `[embed]`） |
| 结构化询问 | 经**中断机制**（控件 + 提交行 + 值回传），用于权限/警告/插件询问 | 独立 `ask_user` 工具（1–3 问、主会话限定、各端原生呈现、答案权限校验） |
| 询问与轮次的关系 | 中断挂起轮次，超时/取消有明确语义 | 问题面板停靠、可跳过/过期、重连恢复、纯文本回答可用 |
| 安全校验 | 路径经权限中间件判定 | 附件 URL 白名单（仅公网 https）+ 文件读策略 + 沙箱检查 |

### 12.4 两者的优缺点

**agentxx**

- 优点
  - **`share_store` 是「大内容不进上下文」的正解**：内容留库、上下文只放引用；与工具输出压缩配合，把「长会话被大输出撑爆」这个问题从根上解决。
  - **跨设备附件选择实现得细**：设备 id 比对、服务端目录列举、路径全程 UTF-8、服务端读文件不传 base64——这几条正好避开「远程模式下把大文件塞进协议」的常见坑。
  - **中断即询问**：询问复用中断机制与组件描述层，权限卡片、重复调用警告、插件询问共用一套渲染与交互，实现只有一份。
- 缺点
  - **没有媒体理解/生成层**：agent 不能看图说内容、不能生成图片反馈；这在「个人助理」类场景是明显缺口（对编码代理场景影响较小）。
  - **没有面向模型的「结构化提问」工具**：现在的询问入口都是**宿主发起**的（权限/警告）；模型自己不能主动发起「请在我给的三个方案里选一个」。中断 UI 能力齐备但缺一个工具壳。
  - **附件安全策略不显式**：路径类附件经权限中间件判定，但没有「远程 URL 必须公网 https、内部主机名一律拒绝」这类对附件的显式校验（有 SSRF 风险面）。
  - **取回是模型自觉行为**：`agentxx_share_store` 工具与预览头都在（§6.6），但提示词侧没有硬性要求「看到 offload 标记就先取回完整内容再下结论」，模型可能仅凭预览作答。这属提示词工程问题，成本很低（并入 M62）。

**openclaw**

- 优点
  - **富输出协议分工清楚**：结构化字段是正规路径、`[[...]]` 是交付元数据、`[embed]` 明确只属于 Web；并明文禁止工具/插件用文本命令走附件。
  - **附件安全校验严格**：远程只允许公网 https + 服务端媒体抓取器再加自己的网络守卫 + 本地附件走文件读策略，把 SSRF 与越权读取两个风险面都堵上。
  - **`ask_user` 的生命周期与权限规则完备**：主会话限定、创建者绑定、回答不授予权限、过期/重连/跳过都有定义，且各端都有原生呈现——「让 agent 停下来问人」这件事被当作一等产品能力。
  - **媒体能力分层**（理解/生成/播放/传输）+ 生成任务后台化 + 进度上报，覆盖了真实使用场景。
- 缺点
  - **媒体层体量与运维成本**：图像/音乐/视频/语音各自有 provider 配置、限额、后台任务与进度，文档加起来近 90 KB。
  - **富输出协议有历史包袱**：`MEDIA:` 兼容行需要「行首、非 markdown 包裹、非代码块」这类细则，说明文本通道与结构化通道并存会长期带来解析歧义。
  - **`ask_user` 只在主会话可用**：编排场景下子代理无法直接问人（设计上要求子代理把问题带回父会话），这会限制某些交互式子任务。

### 12.5 可迁移到 agentxx 的设计

| 编号 | 设计 | 落地位置 | 优先级 | 说明 |
|---|---|---|---|---|
| M60 | **`ask_user` 式结构化提问工具** | 新工具（`agentxx_ask_user`），复用 `InterruptUi` 描述层与中断机制 | P1 | 现有中断 UI 能渲染控件、收集值、回传 `{"values":{...}}`，缺的只是一个「模型可调用」的壳。最小契约：1–3 个问题、每个问题含选项 + 必带自由输入（Other）、可选多选；**限定在主会话可见**（子代理不给）；答案回传为工具结果。这条把「权限询问」之外的交互能力补齐，成本主要在提示词与校验。 |
| M61 | **附件来源显式校验（按实现现状收窄范围）** | `BaseAgent::runTurnAsync` 的附件处理段 | P1 | 体积上限（`maxBytesForMediaType`）、`http(s)` 不加载、线程池卸载**都已实现**（§12.6）。真正缺两条：① **服务端按本地路径加载前是否过权限判定**——当前实现直接读文件，建议在读取前调一次权限中间件（读作用域）；② `http(s)` 附件的显式策略——是允许 provider 自行去取，还是要求客户端先下载（并相应地做地址校验）。 |
| M62 | **大内容取回的显式约定** | `share_store` + 提示词/工具 | P2 | 现状 `share_store` 只是工具可用的一层。建议：① 给它一个标准取回工具（`agentxx_share_get(id)`）或明确「用文件工具读」的路径；② 工具输出被截断时，结果文本里**自动带上引用 id 与取回方式**（配合 M28）；③ 提示词里说明「输出被截断时应先取回完整内容再下结论」。 |
| M63 | **媒体能力插件化（按需，不做核心）** | 新插件（如 `agentxx_image`） | P2 | 若要补「看图/生成图」，按 openclaw 的思路分层：理解类（把图片转成文字描述交给主模型）与生成类（调用外部 API 产图并作为附件回传）分开，各自一个插件。**不应进核心**（核心加能力的上下文代价是持续的，见 openclaw 的「小核心」原则）。 |
| M91 | **工具结果「存储形态 / 模型投影」分离** | `ToolcallWrapNode` 的 offload 分支 + `Session` 的消息表示 | P2 | 现在 offload 后写进上下文的**预览文本是一次性的**：# 预算变化（换模型、压缩恢复）时无法重算，也无法把「原文 + 当前预算」重新投影一次。建议：消息里只保留「store id + 投影参数（行窗/字数）」，模型看到的预览由**按当前预算重算**得到；这样换模型后能自动调整预览口径（对应 openclaw 的 `TOOL_RESULT_PROJECTION_KEY`）。 |
| M64 | **富输出的显式通道（评估后做）** | `wire_protocol.h` + 组件描述层 | P2 | agentxx 走的是「声明式组件描述」路线，已经比文本命令干净；需要补的只是**交付语义**：把「附件」「引用回复」「语音呈现提示」这类元数据从文本里彻底拿出来，作为消息结构里的独立字段（而不是像 `MEDIA:` 那样混在文本）。当前 agentxx 的附件已经是结构化字段，这条主要是防止未来把交付信息写进文本。 |

### 12.6 实现细读（本轮新增）

#### agentxx：附件走「服务端自主加载 + 线程池卸载」，且带类型化体积上限

`base_agent.cpp::runTurnAsync` 里的附件处理（原先只写了「服务端按路径读取编码」）实际是：

1. 仅当 `dataUrl` 为空、`pathOrUrl` 非空**且不是 `http(s)://`** 时，服务端才自行加载——**HTTP(S) URL 原样透传给 provider**（客户端与服务端都不下载）。
2. 加载动作整体卸载到线程池（`utilxx::offloadAsync`），注释说明原因：单附件可达数 MB，读文件 + base64 都是阻塞/CPU 操作，留在 io 线程会让**同一时刻所有会话**的 LLM 流与工具执行一起停摆。
3. **体积上限按媒体类型取**：`maxBytesForMediaType(att.type)`，超限只记 `XX_LOGW` 并原样返回（不加载）。
4. `mimeType` 缺失时按扩展名推断；路径经 `utf8ToPath` 转换（Windows 客户端中文路径的已知坑）。
5. 加载后按 `MediaType` 分装进 `image_urls` / `audio_urls` / `video_urls`（图片与音视频同为一个数组字段，见 §9 的 token 估算按项计 400）。
6. 回显给客户端的 `TurnStart` 增量**清空 `dataUrl`**，只带元数据（注释：防大包），dataUrl 由后续 Sync 补齐。

**对本文结论的影响**：M61（附件来源校验）需要按实际实现重新表述——**体积上限已存在**，「HTTP(S) 不加载」也已明确；真正缺的是两件事：① 对 `http(s)` 附件 URL 的显式策略（是否允许 provider 直接去取、是否校验目标地址）；② 服务端按路径加载时**是否已过权限判定**（当前实现里看不到权限检查调用，路径直接读）。这两点写进 M61 的说明。

#### openclaw：工具结果的「存储形态 / 投影形态」分离

`tool-result-truncation.ts` 用 `TOOL_RESULT_PROJECTION_KEY = Symbol(...)`（不可枚举键）把**模型投影**挂在消息对象上：同一条 tool 结果消息同时持有完整存储形态与可重建的投影形态。带来的能力是：预算变化（换模型、压缩恢复、缓存过期剪枝）时可以**只重算投影**，而不是重新截断已损内容；也避免了「截断一次就永久丢失原文」。

**对本文结论的影响**：这条与 M28/M89/M90 是一组，作为 M91 记入 §12.5。agentxx 的等价物是「share_store 全文 + 预览文本」——但预览文本一旦写进上下文就固定了，缺少「按新预算重算预览」的能力；M91 建议把「预览」做成可按需重算的投影，而不是一次性文本。

---

## 13. 客户端与渲染分层

### 13.1 agentxx 现状

**四层结构，渲染只有一份实现**（见 `docs/zh-cn/design/ui-layer.md`）：

```
插件 JSON（声明） ──parse──► 规范模型 ──adapt(caps)──► 客户端能画的模型 ──► 客户端渲染
                                  └─ 校验上限与未知项（截断 / fallback / 提示）
```

| 层 | 位置 | 职责 |
|---|---|---|
| 描述层（独立库） | `agent/third_party/cxx_pluginxx_ui`（submodule） | 模型 / 解析 / 适配 / 纯文本降级 / 构建器 / kit / 能力段 |
| 插件 SDK | `agent/lib/include/agentxx/plugin/api/plugin_kit.h` | 领域 helper + `agentxx::ui` + 扩展 kit + 提交入口 |
| 扩展 kit（生成物） | `agent/lib/include/agentxx/plugin/api/agentxx_ui_kit.g.h` + `agent/js/agentxx_ui_kit.js` | agentxx 的留白口径与常用组合（定义在 `agent/schema/agentxx-ui-kit.def.json`） |
| TUI 渲染 | `agent/client/.../io/tui/ui_components.{h,cpp}` | 消费**适配后**的模型；`measureItem` 与渲染同源 |

要点：

- **组件词汇 24 个**（`kTuiBlockNames`）：Text / Divider / Gap / Button / Block / Row / Column / Expanded / Spacer / SizedBox / Padding / Align / Collapse / KV / Table / Tree / Progress / Badge / Control / Markdown / Icon / Diff / Sparkline / Diagram。客户端没实现的块由**适配层统一降级**（`Stack` → 最后一个子节点、`Image` → `alt` 文本、`musicxx.Shader` → 跳过）；**不在渲染层写「我不支持谁」**。
- **尺寸只有一个单位 u**：GUI 1u = 1 逻辑像素；终端按能力段的 `cell`（默认 `{width: 8, height: 20}`）换算成列/行。另有 `{"percent": n}`（基准＝直接父容器）与 `"auto"`；不允许负值。
- **能力协商**：`tuiUiCapabilities()` 上报 `blocks` / `controls` / `cell` / `gap` / `percent` / `aspect` / `limits`，`adaptItems` 用**同一份**做降级；「少声明只会降级，多声明会画不出来」是明确约定。
- **控件值变化即派发**；「一组值一起提交」是域内约定（`__submit` / `__cancel`），由中断表单收集各控件当前值后提交。
- **中断 ↔ 组件层的唯一映射**：`middleware::itemOf` / `blockOf` + `preset::blocksOf`；中断描述与面板 / Info / overlay / 装饰共用同一套渲染与交互。
- **表单状态由宿主维护**（`UiFormState`：点击/键盘/校验/取值只有一份实现）。
- **命中测试**：元素内子区域 `UiHitRegion`（局部坐标）+ 滚动容器 `Scrollable::hitTestItem`；行元素内的 `reflect` 框由元素自有节点（`OwnedReflect`）持有，避免悬空。
- **插件侧 UI 表**：`agentxx.client.{ui,components,form,layout,timer,keybind}` 等；定时器支持 `pause_when_hidden`（区域不可见时顺延）与动画等级 Disabled 时拒绝注册；快捷键有冲突先注册者优先、无修饰键可打印字符不参与匹配等规则；设置弹窗里有「快捷键」条目与只读列表弹窗。
- **单条 UI 描述 1 MiB 上限**（入口拒绝整条更新并记日志，注册表保持上次成功内容），注册表条目带内容 `version`。
- 客户端形态：FTXUI TUI（唯一渲染实现）、stdio CLI、远程 WS 客户端。

### 13.2 openclaw 现状

**多个前端，共用协议与部分组件**：

| 前端 | 技术 | 说明 |
|---|---|---|
| `openclaw tui` | TS + `@earendil-works/pi-tui`（第三方终端组件库） | `src/tui`（165 文件）；有 `tui AGENTS.md` 与专门的 PTY 测试通道 |
| Control UI（Web） | Lit Web 组件（`ui/`，4440 文件） | Web 控制台：会话、设置、仪表盘、面板、门户、主题 |
| WebChat | 静态 UI | 走网关 WS 做历史与发送 |
| 原生壳 | Swift（macOS/iOS）、Kotlin（Android）、Linux/Windows | `apps/*`；Swift 模型由 JSON Schema **生成** |
| session-ui | Web 组件 | 与 TUI 共享消息渲染 |

**渲染相关的包**：`markdown-core`（28 文件）、`terminal-core`（22 文件）、`mermaid-renderer`（3 文件）、`normalization-core`（31 文件）、`session-url-contract`、`workboard-contract`。

**输出呈现的方向**：富输出协议（§12）负责「交付/渲染指令」；Control UI 侧有 `screen` / `theme` 工具让 agent 主动布置操作员界面（面板、窗格、主题），另有 `progress_card`（父会话的进度卡）、`show_widget` + 门户（portals）、web dashboard。

**UI 质量门**：`ui/AGENTS.md` 拥有「控制 UI 状态、请求与展示」的规则；UI 测试分几档（`vitest.ui-e2e.*`、`vitest.ui-browser.config.ts`、`vitest.ui-timing.config.ts`、`vitest.ui-isolated.config.ts`）；根 `AGENTS.md` 有一条**硬门禁**：视觉外观或渲染状态变化，必须在来源会话里附上**已检查、已脱敏的前后截图**，并在 PR 正文里嵌入可渲染的图片，否则不能 land。

### 13.3 对比

| 维度 | agentxx | openclaw |
|---|---|---|
| 前端数量 | TUI（唯一渲染）、stdio CLI、远程 WS 客户端 | TUI、Control UI（Web）、WebChat、原生壳（macOS/iOS/Android/Linux/Windows） |
| 界面定义来自哪 | **服务端产出声明式组件描述**（JSON），客户端只负责渲染与交互 | 各前端**自行渲染**协议数据；助手输出携带渲染/交付指令（富输出协议） |
| 跨端一致性 | 天然一致（同一份描述 + 同一套适配规则） | 靠协议 + 共享组件包 + 各端实现对齐（有 UI e2e 测试守） |
| 新前端接入成本 | 实现渲染 + 上报能力即可，描述层/适配/kit/降级直接复用 | 实现完整 UI（或复用 Web 组件），渲染逻辑分散在各端 |
| 能力协商 | 有（`blocks` / `controls` / `cell` / `limits`，适配层按能力降级） | 有（客户端能力影响工具可用性，`client-caps.ts`），但不用于「组件级降级」 |
| 尺寸模型 | 一个单位 u + 终端 cell 换算，GUI/终端同一套描述 | 各端自行处理（终端单元格 vs 浏览器像素） |
| 交互状态归属 | 宿主维护表单状态（`UiFormState`），插件只声明 | 各端自行维护；`ask_user` 有跨端原生呈现 |
| 渲染测试 | 组件层解析/渲染/命中（`ui_items` / `tui_ui_items` / `tui_form` / `tui_widget`） | PTY 假后端通道 + UI e2e + 浏览器时序测试 + 截图门禁 |
| 主题/外观 | `tui_theme`（客户端本地） | `theme` 工具（agent 可改）+ Control UI 主题 |

### 13.4 两者的优缺点

**agentxx**

- 优点
  - **「描述层 + 适配 + 唯一渲染实现」是跨端一致性的最优解**：同一份插件声明在终端与将来的 GUI 上表现一致，降级规则集中在适配层，渲染层不需要知道「谁不支持什么」。
  - **能力协商与降级是同一份数据**（`tuiUiCapabilities()` 与 `adaptItems` 用同一份），不会出现「声明支持但画不出来」。
  - **尺寸抽象（u + cell 换算）干净**，把「终端字符格」与「GUI 逻辑像素」统一到一个模型里。
  - **交互状态单一归属**：表单状态在宿主，控件点击/键盘/校验/取值只有一份实现，插件只交声明。
  - **中断与界面共用一套组件模型**：权限卡片、重复调用警告、插件询问、装饰都走同一套渲染与命中逻辑。
- 缺点
  - **只有一种渲染实现**：描述层的价值要等第二个渲染器（GUI）落地才能完全体现；目前组件行为事实上由 TUI 实现定义（`kTuiBlockNames` 就是事实清单）。
  - **无视觉回归门禁**：没有渲染快照/截图类门禁；TUI 正确性靠单元测试断言文本，风格或布局变化容易漏。
  - **能力粒度到组件级，但「体验」语义不足**：插件知道宿主支持 `Control`，但不知道宿主对复杂表单的整体体验（例如一次提交多字段的支持程度）。

**openclaw**

- 优点
  - **多前端共享协议 + 共享组件包**：TUI 与 Web 能共用消息渲染，原生壳模型由 JSON Schema 生成，减少手工对齐。
  - **UI 质量门禁硬**：视觉变化必须附前后截图且 PR 里可渲染，把界面改动的验收钉在产品层；另有 PTY 假后端、浏览器时序等分层测试。
  - **agent 能主动布置界面**（`screen` / `theme` / `dashboard` / `portal` / `progress_card`），把操作员界面当作可编排对象。
  - **UI 与工具可用性联动**（客户端能力影响工具面），避免「界面不支持却调用了依赖它的工具」。
- 缺点
  - **各端渲染逻辑分散**：同一语义要在 TUI/Web/原生壳各写一遍，靠测试与组件库对齐，成本随端数增长。
  - **「交付指令」混在助手输出里**：`[[audio_as_voice]]` / `MEDIA:` 这类文本标记需要解析细则与兼容路径，长期是歧义来源。
  - **前端量级大**：`ui/` 4440 文件 + 多个原生壳，维护面远超单机代理解的需要。

### 13.5 可迁移到 agentxx 的设计

| 编号 | 设计 | 落地位置 | 优先级 | 说明 |
|---|---|---|---|---|
| M65 | **渲染与命中的快照回归** | `agent/test/client/` 新增模块 | P1 | TUI 是唯一渲染实现，最经济的做法是「渲染快照 + 命中区快照」：对若干代表性组件（表格/树/差异/Markdown/表单/中断卡片）在固定尺寸下生成**纯文本渲染结果**与命中区列表，作为快照断言；改动渲染时失败并要求更新快照。成本低，能挡住「改一处顺便改坏三处」。 |
| M66 | **可更新的结构化进度展示位** | 已有 `agentxx.client.panel` / `status_item` / `toast` / `overlay`；可补等价于 `progress_card` 的形态 | P2 | openclaw 的 `progress_card`（父会话的持久进度卡）值得借鉴：给「长任务的可见进度」一个宿主维护、可原地更新的展示位，而不是让模型每次重述。agentxx 已有面板与状态项，缺的是「可更新的结构化进度」这一具体形态。 |
| M67 | **能力协商补「体验级别」声明** | `agentxx.client.*` 能力段 | P2 | 现有能力段是「支持哪些块/控件」。可补两项：① 表单提交能力（是否支持一次提交多字段、是否支持取消）；② 布局容器的实际可用尺寸（终端行数是否无界）。让插件按「体验」而不是「有没有」来组织内容。 |
| M68 | **UI 改动的验收清单（写进规则文件）** | 配合 M1 的 `agent/client/AGENTS.md` | P2 | openclaw 的硬门禁（视觉变化必须附截图）对 agentxx 可弱化为：改渲染层必须补一条渲染快照断言；改组件描述必须同步 `kTuiBlockNames` 与能力表。写进规则文件即可，不必真做截图流程。 |

---

## 14. 远程协议、SDK 与嵌入模式

### 14.1 agentxx 现状

**手写 Wire JSON 协议**（`wire_protocol.h`）：`{"type": "<msgType>", "id": <opt>, "sessionId": ..., ...payload}`。

- 消息类型：**17 个 client→server**（`hello` / `user_input` / `interrupt_response` / `cancel` / `select_model` / `get_model` / `ping` / `compact_context` / `list_sessions` / `switch_session` / `clear_message_queue` / `remove_queue_item` / `interrupt_and_run_next` / `get_view_messages` / `list_dir` / `get_permission_state` / `set_full_auth`）+ **19 个 server→client**（`hello_ack` / `delta` / `sync` / `interrupt_request` / `interrupt_expired` / `turn_result` / `context_stats` / `error` / `log` / `model_info` / `append_component_info` / `get_context` / `context_messages` / `session_list` / `pong` / `plugin_data` / `plugin_data_up` / `message_queue_update` / `view_messages_page` / `list_dir_result` / `permission_state`）= 36 类。
- **传输可替换**：`AgentIOTransportBase` 之下有进程内 `Channel` 与 `ws_io_transport`（WebSocket）两种实现；`AgentIOBase` 的两端（客户端 io、服务端 `SessionServerAgentIO`）共用同一套消息语义——**同进程与远程只换 transport**。
- 握手与恢复：`hello` → `hello_ack`（含服务端设备 id、会话工作目录、模型与能力表）；`delta` 带单调 `seq`，重连时能连续则重放、否则全量 `sync`。
- 会话切换、历史分页、消息队列镜像、权限状态同步、插件数据双向转发（`plugin_data` / `plugin_data_up`，服务端发布到总线 topic `client.{插件名}.{事件名}`）都在协议内。
- **没有**：协议 schema 的机器可读定义（就是头文件里的字符串常量 + 结构体）、没有认证/令牌层、没有幂等键概念、没有版本协商（`api_version` 是插件侧的，不是 wire 侧的）。
- 其它集成面：MCP client/server、A2A server/client、ACP server、FFI（`agent/lib/src/ffi`，供嵌入方以 C ABI 驱动 agent）。

### 14.2 openclaw 现状

**协议定义与生成**：

- TypeBox schema 定义协议 → 生成 JSON Schema → **由 JSON Schema 生成 Swift 模型**；`packages/gateway-protocol`（174 文件）提供类型化 schema 与**运行时校验器**（含 frame guards、错误详情、审批结果校验等）。
- 网关在收到非 JSON 或首帧不是 `connect` 时**直接关闭连接**（握手强制）。

**线格式**：

| 形态 | 字段 |
|---|---|
| 请求 | `{type:"req", id, method, params}` → 响应 `{type:"res", id, ok, payload\|error}` |
| 事件 | `{type:"event", event, payload, seq?, stateVersion?}` |

- `hello-ok.features.methods` / `features` 是**发现元数据**，文档明确它不是「所有可调用路由的生成式转储」。
- **副作用方法必须带幂等键**（`send`、`agent`），服务端保留一个短时去重缓存以安全重试。
- **事件不重放**：客户端必须在发现 gap 后自行刷新（这是与 agentxx 相反的取向）。

**鉴权与信任**：

- 共享密钥在 `connect.params.auth.token` 或 `.password`；`gateway.auth.mode` 决定用哪个（配置值），而不是必须用哪个线字段。
- 具身份的模式（Tailscale Serve、非回环的 `trusted-proxy`）从**请求头**满足鉴权，而不是 `connect.params.auth.*`。
- **设备身份是所有 WS 客户端的必备项**：新设备 id 需配对批准并发设备令牌；本地回环可自动批准；tailnet/LAN（含同机 tailnet）仍需显式批准；**所有连接都必须签 `connect.challenge` nonce**，签名载荷 v3 还绑定 `platform` 与 `deviceFamily`；重连时网关**固定已配对元数据**，元数据变更要走修复配对。
- 网关鉴权对**所有**连接生效（本地也生效），与配对是两件事。

**SDK / 嵌入**：

- `packages/sdk`（16 个非测试文件）：`transport.websocket`、`run-event-stream`、`run-event-replay`、`event-hub`（带 retention 与 replay-scope）、`chat-projection`、`run-terminal`、`run-reconnect`、`client.ts` —— 即**面向第三方客户端的「运行事件流 + 重放 + 保留」SDK**。
- `packages/gateway-client`：参考 WS 客户端，含握手、请求超时、重连策略、watchdog、TLS、设备鉴权、序列与投影一致性等**逐项测试**（`protocol-client.*.test.ts`、`session-projection*.test.ts`）。
- **嵌入模式**（`docs/gateway/embedding.md`）明确推荐「宿主监督已安装的 `openclaw` 可执行文件，把网关 WS 协议当控制面，把子进程视为可替换运行时」，并给出一组嵌入预设环境变量（`OPENCLAW_DISABLE_BONJOUR` / `OPENCLAW_NO_RESPAWN` / `OPENCLAW_EXEC_SHELL_SNAPSHOT=0` / `OPENCLAW_SKIP_CHANNELS`）与 Electron 下 `process.execPath` 不是 Node 的坑；`--allow-unconfigured` 只跳过启动守卫，不写配置、不修损坏配置。
- CLI 侧还有 `openclaw attach`（附加一个 MCP 客户端）等接入路径。

### 14.3 对比

| 维度 | agentxx | openclaw |
|---|---|---|
| 协议定义 | 头文件字符串常量 + 结构体（手写） | TypeBox schema → JSON Schema → 生成 Swift 模型；运行时校验器 |
| 握手 | `hello` → `hello_ack`（模型/能力/设备/工作目录） | 首帧必须 `connect`，否则直接关闭；`hello-ok` 带发现元数据与 presence/health 快照 |
| 鉴权 | 无（本机或可信网络内使用） | 共享密钥 + 网关鉴权模式 + 设备身份 + 配对批准 + 令牌 + nonce 签名 |
| 幂等 | 无 | 副作用方法必须带幂等键 + 服务端短时去重缓存 |
| 断线恢复 | delta 环形缓冲按 `seq` 重放，超范围回退全量 sync | **事件不重放**，客户端在 gap 后自行刷新；SDK 侧另有 `run-event-replay` + `event-hub` 的保留与重放（面向 SDK 消费者） |
| 历史补齐 | 有（尾窗 + 分页拉取） | 有（会话投影 + 刷新） |
| 多客户端 | 1 会话 : N 客户端（控制器内建广播/单播） | 多客户端经网关 + 投影；会话状态归网关 |
| 插件数据通道 | 有（`plugin_data` 双向 + 总线 topic 约定） | 有（插件可注册通道/工具/hook；客户端能力影响工具面） |
| 嵌入方式 | 直接以库形式嵌入（进程内 Channel）或起 agentxx_cli；FFI 提供 C ABI | 监督子进程 + WS 控制面（进程边界明确）；也可用 `tui --local` 走嵌入式运行时 |
| SDK 形态 | 无独立 SDK（协议 + 示例客户端） | `packages/sdk` + `gateway-client`（带完整测试）+ 生成式 Swift 模型 |

### 14.4 两者的优缺点

**agentxx**

- 优点
  - **「同进程与远程只换 transport」这一层抽象非常干净**：会话语义、重放、分页、队列镜像、权限同步都在协议层，不因部署形态变化；嵌入方可直接用库（无进程边界开销）。
  - **重放做得好**：`seq` + 环形缓冲 + 环比超范围即全量，客户端重连体验优于 openclaw 的「事件不重放」。
  - **多客户端内建**：一个会话控制器天然支持 N 个客户端（广播/单播分工明确），不需要另加一层投影服务。
  - **协议面小而全**：36 类消息覆盖了会话、模型、队列、权限、插件数据、目录列举，读一遍即可掌握。
- 缺点
  - **协议无机器可读定义**：字符串常量 + 结构体是唯一真源，跨语言客户端（若将来有非 C++ 客户端）要手抄；也没有版本协商字段（协议演进靠「老客户端忽略新字段」的默契）。
  - **没有认证与授权层**：协议本身不区分「谁能连、谁有权限做什么」；远程部署依赖外部网络隔离。
  - **没有幂等键**：`user_input` 这类副作用消息在网络抖动重发时会重复执行（在本地进程内不明显，远程模式下是真实风险）。
  - **无独立 SDK**：第三方接入要自己读 `wire_protocol.h` 并实现传输。
  - **嵌入方式唯一**：作为库嵌入时，`BaseAgent::runTurnAsync` 的说明里明确提醒「不要把 io_context 在不同线程间传递」，嵌入方需要理解协程与 executor 的约束。

**openclaw**

- 优点
  - **协议有单一可生成的定义**：TypeBox → JSON Schema → Swift 模型，多语言客户端与原生壳共享同一份定义；运行时校验器保证了「收到的东西符合契约」。
  - **信任链完整**：设备身份 + 配对 + 令牌 + nonce 签名（绑定平台与设备族）+ 重连元数据固定 + 网关鉴权对所有连接生效；同时保留了「本地回环自动批准」这类体验优先的例外。
  - **幂等键是必需项而不是可选建议**：副作用方法不幂等就没法安全重试，这条把分布式系统的常识写进协议。
  - **嵌入模式的建议很务实**：把子进程当可替换运行时、明确禁止依赖私有状态布局、给出 Electron 的 `process.execPath` 坑与预设环境变量。
  - **SDK 带完整测试**（握手/超时/重连/序列/投影一致性逐项测）。
- 缺点
  - **事件不重放**：客户端要自己实现「gap 后刷新」，实时流与补齐流两套语义；SDK 侧又另做了一套 replay（`run-event-replay`），说明「不重放」在网络抖动下确实需要补偿。
  - **协议面积巨大**：`packages/gateway-protocol` 174 文件 + 上百个 RPC 方法（`server-methods` 971 文件），学习与维护成本高；文档也需要专门澄清「hello-ok.features 不是完整路由转储」。
  - **嵌入要走进程边界**：虽然清晰，但对「想在同一个进程里做轻量集成」的场景偏重（它另用 `tui --local` 覆盖那类需求）。

### 14.5 可迁移到 agentxx 的设计

| 编号 | 设计 | 落地位置 | 优先级 | 说明 |
|---|---|---|---|---|
| M69 | **副作用消息的幂等键** | `wire_protocol.h` + `SessionServerAgentIO` | P0 | 给 `user_input` / `interrupt_and_run_next` / `compact_context` 这类**会产生副作用**的消息加可选 `idempotencyKey`，服务端保留一个小型去重表（TTL 几分钟）；重复 key 直接回上次结果而不重复执行。远程模式下网络抖动会导致重发，这是当前最实际的一个协议缺口。 |
| M70 | **协议版本与能力协商字段** | `hello` / `hello_ack` | P1 | 现在 `hello_ack` 里有模型能力与设备信息，但没有**协议版本**。补一个 `protocolVersion`（整数）+ 客户端在 `hello` 里声明自己的版本与可选能力列表；服务端据此决定是否发送新消息类型（老客户端忽略未知类型，但主动降级更稳）。 |
| M71 | **协议 schema 的机器可读清单** | 由 `wire_protocol.h` 的 `MsgType` + 各消息字段生成一份 JSON（构建期或 CI 生成） | P2 | 不必引入 TypeBox 那样的框架；一个从 C++ 头/结构体生成的 `wire-schema.json` 就能让第三方客户端按 schema 生成代码，也让「协议变了但文档没更新」可被 CI 发现。 |
| M72 | **连接级鉴权钩子（可选）** | `SessionServerAgentIO::handleHello` | P2 | 远程模式下的最小形态：宿主可注入一个 `onAuthorize(hello) -> bool/拒绝原因` 回调（默认放行），由使用方决定要不要校验令牌/来源地址。**不进核心逻辑**，只留接缝——这比把鉴权做进协议更符合「agentxx 是嵌入库」的定位。 |
| M73 | **沿用并文档化「重放 vs 刷新」的取舍** | `docs/zh-cn/design/tui.md` / index.md | — | agentxx 已有更好的重放机制，这一条是把取舍写清楚：环形缓冲只在**单进程生命周期内**有效，进程重启后客户端必须走全量 sync；把这个前提写进文档，避免客户端误以为 `seq` 跨重启连续（配合 §5 的 M23 若实现，可升级为持久事件序列）。 |
| M92 | **重放缓冲的「观察者计数 + 按需释放」** | `SessionServerAgentIO` 的 `deltaBuffer_` | P2 | 现在的环形缓冲是固定容量 FIFO（默认 4096），不看有没有人在消费。openclaw 的 SDK 侧做法是：**按 run 维护重放状态 + 观察者计数**，最后一个观察者离开时释放保留的文本/投影，并用 `trimReplayRuns()` 定期裁剪（上限 100 个 run × 每 run 500 事件）。agentxx 可借鉴两点：① 会话级缓冲在「无客户端在线」时可缩短保留（减少内存）；② 重连后若发现客户端长期不消费，主动降级为「下次全量 sync」而不是无限追加。 |

### 14.6 实现细读（本轮新增）

#### agentxx：重放、握手顺序与「重发未应答的中断」

`session_server_agent_io.cpp` 里几条实现级不变量（比接口描述更具体）：

| 不变量 | 实现 | 若不满足会怎样 |
|---|---|---|
| 新 delta 必须带会话级 `seq` | 统一经 `Session::nextDeltaSeq()`；`seq == 0` 的 delta **不进重放缓冲** | 断线重连时该条消息丢失，客户端历史与服务端不一致（源码注释明确写了这条） |
| 先 `HelloAck` 后重放 | `handleHello` 内部顺序固定；注释说明客户端握手循环会丢弃 HelloAck 之前的消息 | 先重放会被客户端整批丢掉，表现为「重连后历史丢失」 |
| 缓冲溢出回退「全量 sync」而不是尾窗 | `deltasSince(lastSeq)` 返回 `nullopt` → `buildFullSync()` | 增量缺口无法用尾窗表达；客户端需要整体重置历史窗口 |
| 首接入用尾窗 + 分页 | `lastSeq == 0` → `buildTailSync(initialSyncTailCount)` | 长会话恢复时全量传输（几百条以上会明显拖慢首屏） |
| 重连重发未应答中断 | 遍历 `pending_` 生成 `WireInterruptRequest` | 客户端会看不到（也无从回应）那个卡住的权限/询问卡片 |
| `HelloAck` 携带已加载插件清单 | 名字 + 版本 + 声明接口（`PluginInfo`） | client 插件无法判断对端是否可用（这是它的正式通道） |

另外，会话控制器的驱动循环在**开始时就把会话预热**（`getSessionAsync` 卸载到线程池），理由与附件加载一致：长历史会话同步加载可达数百毫秒，会把 io 线程上其它会话的推进一起卡住。

#### openclaw：协议不重放，但 SDK 用「有界重放 + 观察者释放」补齐

`packages/sdk/src/event-hub.ts` 与 `run-event-replay.ts` 是这条策略的落点：

- **`EventHub`（有界重放 + 异步迭代）**：`replayLimit` 为 0 时不保留；每次 `publish` 追加并在超限时从头部裁剪。`stream(filter, {replay})` 返回 `AsyncIterable`，内部维护 per-stream 队列 + `queueHead` 游标，**已消费元素立即置 `undefined`，并在「已消费前缀达到缓冲一半且至少 1024 个」时才一次性压缩**（摊销 dequeue 成本）；`close()` 会唤醒所有等待中的读；迭代器 `return()`（提前退出）**丢弃自己的积压但允许 hub close 后继续排空**——这两条语义写得很细。
- **`SdkRunReplay`（按 run 的保留与恢复）**：上限 `MAX_REPLAY_RUNS = 100`、`MAX_REPLAY_EVENTS_PER_RUN = 500`；`observers` 计数，**最后一个观察者离开时**标记 `owned=false`、中止恢复、必要时丢弃保留的文本与投影，再 `trimReplayRuns()`；恢复失败有上限（`MAX_UNAVAILABLE_RECOVERY_OBSERVATIONS = 4`）。
- **恢复重试带退避与抖动**：`computeBackoff({ initialMs: 1000, maxMs: 25000, factor: 2, jitter: 0 })` 再乘 `1 + random()*0.2`——注释特意说明要与 Gateway 重连的「正 20% 抖动」一致，且**不要把随机值夹到上限**（否则所有客户端会同步重试形成尖峰）。

也就是说：**「协议层不重放」这条取舍并不等于用户拿不到补齐能力**——能力被放到了客户端 SDK 里，并配了有界保留、观察者释放与抖动退避三件套。这对 agentxx 的启示是双向的：agentxx 在**传输层**做得更好（服务端保留 + seq 重放），但在**客户端侧的缓冲生命周期管理**上没有等价物（M92）。

**对本文结论的影响**：① 新增 M92；② 本文 §14.4 对 openclaw「事件不重放：客户端要自己实现 gap 后刷新」的批评需要加一句限定——它实际提供了 SDK 侧的补偿路径，代价是「同一语义分散在两侧」；③ agentxx 的 `seq` 重放应补上「观察者/消费进度」维度，否则固定容量 FIFO 在「客户端长期不在线」时会一直做无用保留（或过早丢弃给全量 sync 造成额外流量）。

---

## 15. 扩展机制（插件框架专项）

> 本节是全文最需要细读的一节：两侧的扩展机制代表了两种完全不同的取舍——agentxx 用**二进制边界换稳定性**，openclaw 用**进程内同语言换扩展速度**。

### 15.1 agentxx 现状

**四层内核**（`agent/third_party/cxx_pluginxx`，独立工程维护）：

| 层 | 位置 | 职责 |
|---|---|---|
| 纯 C ABI 基座 | `pluginxx/api/` | 结构体对齐/定长类型/调用约定、8 字节结构体对齐与 `PLUGINXX_CALL`、跨边界字符串工具（`pluginxx_strdup` 等） |
| 插件 SDK | `pluginxx/kit/`（`kit.h`、`guard.h`） | 通用（跨边界字符串工具、通用表聚合 `PluginIfaceCore`、Task 锚定协程/原语、`CancelRegistry`、`OpCtl`、`ArgReader`、`PluginBaseT<IfacesT>`、导出宏） |
| 运行时 | `pluginxx/runtime/` | runtime / instance_base / manager_base / op_driver / driver |
| 宿主内核 | `pluginxx/host/` | `lifecycle.h`（生命周期骨架）、`host_core.h`（十张通用表实现）、`tables_impl.h`（C ABI 入口）、`domain_hooks.h`（宿主数据接缝）、`event_bus.h`（事件后端）、`manifest.h`（清单解析）、`loader.h` |

agentxx 侧对应：

- **宿主领域 helper**：`agent/lib/include/agentxx/plugin/api/plugin_kit.h`（umbrella：包含内核头 + 逐条 `using` 引入 `agentxx::plugin`，**插件源码零改动**）。
- **接口表清单**：agent 侧 **19 张**（10 张通用 `pluginxx.*`：events / capabilities / scheduler / coroutine_runtime / plugins / config / cancel / json / log / tasks；9 张领域 `agentxx.agent.*`：tools / permission / hooks / session / context / model / prompt / resources / graph）；client 侧 **9 张**（`agentxx.client.{ui, events, session, wire, self, json, log, timer, keybind}`）。
- **宿主实现**：`plugin_manager_domain_hooks.cpp`（事件后端包装 `agentxx::events::EventBus`）、`plugin_manager_vtable.cpp`（领域表 + `query_interface` 先查通用表）、`plugin_manager_lifecycle.cpp`（接入内核 `PluginHostLifecycle`）、`client_plugin_manager.cpp`（client 侧，仅「装载」保留自有实现：dlopen 卸载到内部线程池 + 接口协商 + 双端入口探测）。

**生命周期契约（必备）**：

- 所有插件必须导出 `agentxx_plugin_{agent,client}_{start,stop}`：
  - `create` 只构造上下文（`new Ctx + init(host)`，**不注册、不起线程**）；
  - `start` 是注册事务（在宿主 IO 线程执行，失败返回 `NULL + error` 由宿主回滚）；
  - `stop` 撤销自管资源（线程/定时器/订阅，**可重复**）；
  - `destroy` 只释放本地对象。
- **缺失 start/stop 的插件宿主拒绝加载**（不再支持 create 期注册的旧形态）。
- SDK 侧 `AGENTXX_PLUGIN_AGENT_EXPORT(Ctx, Name, Ver, Desc, StartFn, StopFn)` 一次生成五个入口；手写 create/destroy 的用 `AGENTXX_PLUGIN_{AGENT,CLIENT}_LIFECYCLE_EXPORT`。

**多实例三铁律**（同一动态库可被同进程内不同宿主创建多个并存实例）：

1. 禁止可变全局/函数级 `static` 缓存；
2. 实例状态只能放 `*plugin_ctx` 堆块，回调经 `spec.user_data` 恢复；
3. 接口表查询结果存实例上下文。

（另有 offload 线程池适配器的调用方内嵌存储约束；详见 `docs/zh-cn/design/plugins.md` §4。）

**导出符号控制**：插件动态库只导出宿主按名查找的入口符号；ELF 用 `-fvisibility=hidden` + version script 白名单（通配符 `agentxx_plugin_agent_*`/`agentxx_plugin_client_*`），macOS 用 `-exported_symbols_list`，MSVC 仅 `dllexport`。

**接口协商（三层）**：① 声明层（`plugin.yaml` 的 `interfaces.require/optional`）；② 校验层（宿主按前缀过滤本侧声明并与支持集比对：`require` 未满足 → 跳过加载并记 INFO 原因；声明了本侧接口却缺入口符号 → 明确报错）；③ 决策层（插件 entry 内 `query_interface` 判空 / 收到 `EVT_READY` 与 `get_client_state` 的 `interfaces` 自行决定启用哪些功能）。命名规范：`agentxx.` 是保留命名空间，第三方用 `<vendor>.<name>`。

**平台支持矩阵**：各插件并非全平台适配，源码无对应平台实现时**跳过编译**（在插件自身 CMakeLists 声明，经 `plugins/cmake/plugin_platform_support.cmake` 的 gate 判定）。

**工具权限声明**：插件在注册工具后经 `agentxx.agent.permission` 声明权限（作用域/目标来源/目标参数名），权限中间件**不硬编码任何工具名**；声明随工具注销/插件禁用卸载自动撤销；模式/前缀参数工具另用同表的 `check_paths` 批量三态查询。

**其它**：`pluginxx.config`（插件配置表）、`pluginxx.tasks`（任务）、`pluginxx.cancel`（取消注册表）、`pluginxx.scheduler`（调度）、`pluginxx.coroutine_runtime`（协程驱动，kit 的 `PollOneBridge` 依赖它）、`pluginxx.capabilities`（能力协商）。

### 15.2 openclaw 现状

**两类插件，明确鼓励后者**：

| 类型 | 说明 |
|---|---|
| code plugin | 进程内 TS，可注册能力（provider / CLI backend / embedding / speech / realtime transcription / realtime voice / media understanding / transcript source / image / music / video generation / web fetch / web search / channel / gateway discovery / migration）、工具、命令、后台服务、路由、hooks |
| bundle plugin | 只打包稳定的外部面：技能、MCP server、相关配置。**接口更小、更稳定、安全边界更好，能表达时优先用** |

**能力模型（capability model）**：`api.registerProvider(...)` 等 16 种能力注册方法；按实际注册行为把插件分成四种形态（`plain-capability` / `hybrid-capability` / `hook-only` / `non-capability`），并可用 `openclaw plugins inspect <id>` 查看形态与能力明细。文档明确「能力模型已落地并被内置插件使用，但外部插件的兼容门槛仍高于『导出了就算冻结』」——对外部插件而言 hooks 仍是最安全的兼容路径。

**清单（manifest）与发现**：

- 原生插件**必须**在插件根提供 `openclaw.plugin.json`；OpenClaw 用它**在不执行插件代码的前提下**校验配置（缺失或非法即视为插件错误，阻塞配置校验）。
- 清单内容：身份/配置校验/配置 UI 提示、鉴权与 onboarding 元数据、控制面激活提示、根 CLI 命令名与子命令标记、模型家族简写、静态能力归属快照 `contracts`、仪表盘控件数据绑定、静态 MCP servers、备份资源、QA 元数据、通道专属配置元数据。**明确不用它做**：注册运行时 hook、声明完整运行时入口、npm 安装元数据。
- 另一条并行路径：`package.json` 的 `openclaw` 字段声明资源（`extensions`/`skills`/`prompts`/`themes`，值为相对包根的文件路径或 glob）；**未列出的资源类型回退到约定目录发现**（`extensions/`、`skills/`、`prompts/`、`themes/`）。
- 兼容 bundle 格式：Agent Plugins（`plugin.json`，符合开放标准）、Codex（`.codex-plugin/plugin.json`）、Claude（`.claude-plugin/plugin.json` 或默认组件布局）、Cursor（`.cursor-plugin/plugin.json`）——**自动识别但不按自己的 schema 校验**。

**槽位（slots）替换**：`plugins.slots.contextEngine` / `plugins.slots.memory` 等**单槽位**——运行时只解析一个引擎或记忆插件。卸载当前被选为槽位的插件时，OpenClaw **把槽位重置回默认**（`legacy` / 默认记忆），不需要手工改配置。

**hooks（类型化）**：`api.on(...)` 注册；覆盖模型解析前、提示词构建前、agent 回复前、agent 结束、压缩前/后、工具调用前/后、安装前、工具结果持久化前、消息收发、会话起止、网关起止。决策语义统一：`block: true` / `cancel: true` 是**终态**并停止更低优先级处理器，`false` 是 no-op 且**不清除先前的 block**。

**控制面 / 运行面分离**（`src/plugins/AGENTS.md`）：

- 发现、清单解析、配置校验、装配提示、激活计划属**控制面**；插件执行属**运行面**。
- **清单优先**：发现/配置校验/装配必须能在插件 runtime 启动前完成。
- **保持惰性**：loader/registry/公共产物变更时不得在元数据足够时 eager import 内置插件的重型 barrel；若插件有轻/重两面，发现/清单/装配状态检查一律走轻路径。
- 禁止「内置插件能用的私有后门」；契约校验走专门的 bundled registry 路径，不要依赖激活 provider 的生产解析流程。
- 可用性/选择契约：网关运行期插件元数据**视为稳定**，复用快照与有界缓存，不做每次调用的 stat/read/hash；元数据变更需要重启或 owner 的显式 reload/install/doctor 流程。**已配置/可用状态与实时健康要区分**：有凭据或缓存描述符不证明服务可达。

**分发与信任**：npm 包分发 + 本地扩展加载开发；插件官方发布者状态、来源与安全评审在 ClawHub；`security.installPolicy` 与 `before_install` 钩子负责安装期允许/警告/拦截（并覆盖 CLI 安装与更新路径）；有 `docs/plugins/plugin-permission-requests.md`（插件权限请求）；插件状态存 SQLite（`plugin_state`）；doctor 契约负责插件自有配置的修复；有 `plugin-package-contract` 包与依赖解析/安装覆盖/打包（bundles）文档；有插件内存剖析脚本（`scripts/profile-extension-memory.mts`）。

### 15.3 对比

| 维度 | agentxx | openclaw |
|---|---|---|
| 边界性质 | **二进制边界**（C ABI 动态库；符号白名单导出） | **进程内同语言**（TS import；靠 SDK 入口与规则约束） |
| 插件可扩展的能力 | 工具、hooks、提示词、资源、模型、权限声明、图节点、UI 组件、事件、定时器、快捷键、配置、任务、取消、日志、JSON、调度、协程驱动 | 16 种能力注册 + 工具/命令/服务/路由/hooks |
| 版本兼容 | `api_version` + 每张接口表自带 version 独立演进 + 三层接口协商 | 清单 schema + SDK 入口；文档明确「导出≠冻结」，hooks 是外部兼容基线 |
| 生命周期 | 五态事务（create/start/stop/destroy + 启停/禁用启用/卸载），内核 `PluginHostLifecycle` 统一实现（含级联依赖、关闭超时重试） | 插件由网关加载/启用/禁用/卸载；槽位卸载自动重置；有 lifecycle hooks |
| 多实例 | 三铁律明确要求（禁止可变 static、状态放 ctx、接口表存实例） | 进程内单实例/单网关为主；多实例不是设计目标 |
| 清单 | `plugin.yaml`（interfaces 段 + 平台支持 + 资源） | `openclaw.plugin.json`（校验/元数据）+ `package.json` 的 `openclaw` 字段（资源 glob）+ 约定目录回退 |
| 发现与激活 | 加载前按接口声明过滤 + 平台 gate 跳过编译/加载 | 清单优先 + 惰性导入 + 控制面/运行面分离 + 激活计划 |
| 单槽位替换 | 无（多插件并存，能力叠加） | 有（contextEngine / memory 等槽位，独占且卸载自动重置） |
| 分发 | 编译产物（动态库）+ plugin.yaml，随宿主或独立发布 | npm 包 + ClawHub 注册表（发布者状态/来源/安全评审）+ 本地路径/归档/git 安装 |
| 安装期管控 | 无（编译期由构建门控） | `security.installPolicy` + `before_install` 钩子（覆盖 CLI 安装与更新） |
| 权限声明 | 有（工具权限目标 + `check_paths` 批量三态） | 有（`plugin-permission-requests.md`）；插件工具同样参与工具策略与审批 |
| 插件状态存储 | 插件自管（`pluginxx.config` + 宿主 KV） | 统一进 SQLite（`plugin_state`） |
| 可观测 | 探针（Debug 插桩）+ 日志 | 内存剖析脚本 + `plugins inspect` + 形态分类 + diagnostics 事件 |
| 文档 | `plugins.md` 103 KB（单文件） | 数十篇分页文档（architecture/manifest/sdk-*/bundles/…） |

### 15.4 两者的优缺点

**agentxx**

- 优点
  - **二进制边界带来三个硬收益**：① 语言无关（插件可以是 C/C++ 甚至 Rust 导出 C ABI）；② 崩溃与内存隔离面更清晰（不同编译单元、导出白名单）；③ 宿主与插件的构建/发布可以完全解耦（不同编译器的复用代码各自静态链接进两侧）。
  - **生命周期是事务性的**：`start` 定义为「注册事务」且失败由宿主回滚，`stop` 可重复——这两条是卸载/禁用路径能可靠工作的前提，很多插件框架恰好缺这个。
  - **多实例三铁律被明确写出并被测试覆盖**：同一动态库多宿主并存是真实需求（agentxx 是嵌入库），把它当成设计约束而不是事后补丁。
  - **接口表机制（COM 风格）让兼容性可控**：每张表自带 version 独立演进，新增能力不再动全局版本号；三层协商让「插件依赖什么能力」在加载前就能判定。
  - **平台 gate 与符号白名单**：不适配的平台直接跳过，导出面被严格限制。
- 缺点
  - **插件开发门槛高**：要写 C/C++、理解 ABI 对齐/调用约定、遵守三铁律、处理跨边界字符串与内存归属；这直接限制了生态规模。
  - **没有单槽位替换与分发市场**：能力是叠加的，没有「换一个上下文引擎」这种整槽替换（见下条 M74）；也没有包管理与注册表，插件分发靠手工摆放动态库。
  - **清单能力弱于 openclaw**：`plugin.yaml` 主要承载接口声明与平台支持，没有「不执行代码就能校验配置」「声明式 UI 绑定」「静态能力快照」这类元数据。
  - **文档集中在一个大文件**：103 KB 的单篇插件文档对「只想写一个小插件」的人不友好（openclaw 用分页文档 + 快速上手教程解决这个问题）。

**openclaw**

- 优点
  - **扩展成本极低**：写 TS、用 `openclaw/plugin-sdk/*`，且有「mini manifest 的最小白名单教程」这类上手路径；生态规模因此大得多（约 200 个内置插件 + ClawHub）。
  - **能力模型统一了扩展点**：16 种注册方法把「provider / 通道 / 媒体 / 搜索 / 迁移」都归到同一抽象，并按实际注册行为给插件分类（可 inspect），这对「我该用哪种扩展方式」很有帮助。
  - **清单优先 + 惰性 + 控制面/运行面分离**：三条规则一起保证「不启动插件也能校验配置/做发现」，直接决定启动性能与稳定性（并有内存剖析脚本守它）。
  - **单槽位替换与自动重置**：`plugins.slots.*` 让「独占能力」有明确语义；卸载时自动重置避免了「配置指向一个不存在的引擎」。
  - **分发与信任链完整**：npm + ClawHub（发布者、来源、安全评审）+ 安装策略钩子 + doctor 契约修复插件配置。
  - **hook 决策语义统一**：`block`/`cancel` 的终态与 no-op 语义一致，避免「谁来否决」的歧义。
- 缺点
  - **没有真正的边界**：同进程同语言，插件能造成的影响与核心一样大；安全只能靠 SDK 契约、评审与静态分析（所以它有 plugin-trust-boundary 的 CodeQL 规则集与 `semgrep`）。
  - **规则负担重**：插件作者要理解「不 import core 内部」「保持惰性」「不做每次调用的元数据读取」「区分可用性与健康」等一整套要求，违反的后果往往以性能/启动问题的形式延后出现。
  - **没有多实例概念**：进程内单网关模型下不需要，但对嵌入场景（同一进程内多个 agent 宿主）不适用。
  - **文档切片多**：13+ 篇插件文档 + SDK overview + 若干能力分页，查找一个字段要跳几页。

### 15.5 可迁移到 agentxx 的设计

| 编号 | 设计 | 落地位置 | 优先级 | 说明 |
|---|---|---|---|---|
| M74 | **单槽位（slot）替换语义** | `agentxx.agent.*` 表扩展 + `PluginManagerLifecycle` | P1 | openclaw 的 `plugins.slots.*` 解决了「独占能力只能有一个实现」的问题（上下文引擎、记忆插件）。agentxx 现在的模型是「能力叠加」，适合工具，不适合「换掉压缩策略 / 换掉记忆实现」这类需求。建议：为少数**独占型扩展点**（上下文装配、记忆检索、摘要器）定义槽位配置（`slots.context_assembler` 等），并在卸载时**自动重置为内置实现**（agentxx 已有「卸载按备份恢复」的机制可复用）。 |
| M75 | **清单能力对齐：不执行插件即可校验配置、声明式元数据** | `plugin.yaml`（`pluginxx/host/manifest.h`） | P1 | 现状 `plugin.yaml` 主要声明接口与平台支持。可补三类 openclaw 用得很重的元数据：① **配置 schema**（宿主在加载前校验插件配置，给出可读错误，而不是等 `start` 失败）；② **配置 UI 提示**（设置面板里的分组/说明/默认值）；③ **静态能力快照**（插件声明自己提供哪些能力，供「发现/清单展示」用，不需要实例化）。第 ① 条收益最大。 |
| M76 | **按目录分页的插件文档 + 快速上手** | `docs/zh-cn/design/plugins.md` 拆分或新增 `docs/zh-cn/design/plugins/` 子目录 | P1 | 103 KB 单文件对「只想写小插件」的人门槛过高。建议拆成：`getting-started`（最小插件 + 导出宏 + 一次完整示例）、`lifecycle`（五态与事务语义）、`kit`（SDK 与表清单）、`host`（宿主实现与领域表）、`client`（client 侧插件与 UI）、`rules`（三铁律、导出控制、平台 gate、权限声明）。与 M1（目录规则文件）配合。 |
| M77 | **插件内存/启动成本的剖析工具** | `agent/benchmark`（插件逐项边际内存已存在） | P2 | agentxx 有「插件逐项边际内存」基准，说明这个维度已被重视。可再进一步照 openclaw 的做法：一个「只加载某个插件、测启动耗时与常驻内存」的独立脚本，作为插件评审的固定证据（新增内置插件时跑一次）。 |
| M78 | **hook 决策语义的显式规则** | `agentxx.agent.hooks` 表文档与实现 | P2 | 若 hooks 表已支持「返回值否决」，需要像 openclaw 那样写清：什么值代表**终态**（停止更低优先级处理器）、什么值代表 **no-op**（不清除先前的否决）、多个处理器冲突时谁优先。这类语义不写清楚，插件之间就会互相打架。 |
| M79 | **插件安装/卸载的「配置指向不存在能力」防护** | 与 M74 配套 | P2 | agentxx 已有「插件卸载按备份恢复」的机制。若引入槽位，需补一条：卸载或禁用后，指向该能力的配置**自动回落到内置实现**并记日志（不要留下悬空引用）。 |

### 15.6 实现细读（本轮新增）

> 逐行读 `agent/third_party/cxx_pluginxx/include/pluginxx/host/lifecycle.h`（1310 行）与 agentxx 侧接缝实现（`plugin_manager_lifecycle.cpp`）后补充。

#### 生命周期骨架的「谁拥有什么」

内核 `PluginHostLifecycle<InstanceT>` 是装载/启停/禁用启用/卸载/级联依赖/关闭超时的**唯一实现**，agentxx 只提供接缝（纯虚 `selfRef`/`createInstance`/`hostVtable` + 可选 `detachDomainRegistrations`/`clearDomainRegistrations`/`applyDeclaredResources`/`releaseInstanceResources`/`onInstanceEnabledChanged`/`onInstanceLoaded`/`onInstanceUnloaded`/`cascadeUnloadEnabledOnly`/`logTag`）。这种「骨架在内核、领域在宿主」的切分带来的直接好处是：**同一套启停语义在 agent 宿主与 client 宿主上行为一致**（client 侧只有「装载」阶段有自有实现：dlopen 卸载到内部线程池 + 接口协商 + 双端入口探测）。

#### 注册摘除与重声明：三个函数名的区别就是契约

| 函数 | 做什么 | 不做什么 |
|---|---|---|
| `detachInstanceRegistrations(inst)` | `detachAll`（**只请求取消**未终结的 Operation；撤销事件订阅；撤销能力声明）+ `detachDomainRegistrations`（宿主侧：工具/权限/钩子/图/提示词）+ `detachDomainOwnedResources` | **不删除**实例内的注册记录，也不释放实例资源——因为 start 事务还要按同一份记录重新声明 |
| `clearPluginOwnedRegistrations(inst)` | 在 **stop 成功之后**调用：清空订阅/能力/领域注册记录 | 不触碰通用表登记（那由内核自身维护） |
| `releaseInstanceResources(inst)` | 真正释放实例级资源（宿主接缝） | 与「摘除注册」分离，避免禁用后再启用时把资源也一起丢了 |

`detachAll` 里对未终结 Operation 的处理注释写得明确：**「只请求取消，不删除活跃记录；终态提交负责唯一一次清理」**，并且遍历的是快照（`inst->outstandingOps` 的拷贝）——因为取消回调可能登记新的操作，直接迭代容器会失效。

#### 禁用/启用的级联语义：两条方向相反的规则

`disableImpl` / `enableImpl` 的注释与实现合起来说明了两条规则，且它们**方向相反**：

- **禁用向下传播**：禁用 A → 递归禁用「反向必需依赖者」（只收集**直接**依赖者再逐层递归，覆盖三级与菱形依赖）。级联禁用只记 `blockedByDependencies`，**不改写用户显式禁用标记**。
- **启用向上补齐**：启用 A → **先启用 A 的必需依赖**（注释理由：子插件的 start 需要父插件的能力/工具已可用），再投递 A 的 start，最后才级联恢复依赖者；且 `!userInitiated && inst->userDisabled` 时**直接返回**——**用户显式禁用的插件不会被级联重新启用**。

两处护栏：进入任一方向前先查 `lifetime->closeRequested()`（已进入关闭流程就不再接受启用状态变化，避免与 stop/destroy 交错）；`stopForDisable` 在等待期间发现「用户又启用了该插件」则**本次 stop 作废**（`if (inst->enabled) co_return;`）。

#### 关闭路径：为什么同步 shutdown 必须保留实例表

- `shutdownAsync(timeout)`：`shutdownAllAsync` 让**所有实例共享同一个截止时刻**（避免「每个插件各自 30s」导致总时长随插件数线性增长）。
- 同步 `shutdownAll()` 的三条语义写得很清楚：① 等不到未返回的插件回调（调用方须保证没有在跑的插件回调）；② **仍有活动 lease 的实例保留上下文与动态库**，登记为「空闲收尾」，可稍后由 `shutdownAsync` 重试；③ stop 事务未完成的实例标记 `CloseFailed` **并保留在实例表中**——注释给出理由：*不能无条件清空实例表，否则最后一个 lease 释放后将失去可重试的关闭入口*。
- 装载失败回滚：`rollbackLoad(inst, closeHandle)`（必要时关闭库句柄）；`finishLoad` 里经 `applyDeclaredResources` 应用清单声明的资源。

**对本文结论的影响**：§15.4 原来评价 agentxx「生命周期是事务性的」是对的，但更准确的说法是**「禁用/启用/卸载三条路径各有明确的注册摘除/重声明/资源释放契约，并且关闭路径区分 lease 与 stop 两种未完成状态」**。这三条正是 openclaw 侧没有对应物的部分（它的插件生命周期是「进程内加载 + slot 重置 + 卸载时清理」，没有 lease 概念）。同时新增一条可迁移项：M79 的「卸载后配置回落」在 agentxx 侧其实已有基础（`onInstanceUnloaded` + 备份恢复），只需在引入槽位时接上。

---

## 16. 测试与质量门禁

### 16.1 agentxx 现状

**一个自研测试程序**（`agent/test`，产物 `{build}/exec/agentxx_test`）：

| 维度 | 现状 |
|---|---|
| 组织 | 头文件统一在 `agent/test/include/agentxx-test/{core,plugin,client}`（与源码目录同名对应），源码一律写完整路径 include 避免同名头歧义；本次实测测试模块头文件 **79 个**（core + plugin 58，client 21，client 部分受 `AGENTXX_BUILD_CLIENT` 门控） |
| 运行方式 | `agentxx_test` 跑全部模块（遇错不终止）；`--fail-fast` / `-f` 遇错即停；`agentxx_test <模块名…>` 只跑指定模块 |
| 测试类型 | 同步模块（`runCtx` 风格，带 context）+ 异步模块（协程 `run(...)`） |
| 覆盖重点 | 协议层（openai/anthropic provider、http、websocket、mcp/acp/a2a）、会话与持久化（session_persistence、checkpoint_store、summarization、share_store）、并发与取消（cancel、concurrency、interrupt_bus、subagent_bus）、插件框架（plugins、plugin_resources、plugin_runtime、plugin_sdk、plugin_multi_instance、plugin_bridge、client_plugins）、UI 层（ui_items、ui_kit、tui_form、tui_interrupt、tui_scroll…）、工具（filesystem/command/math/string/rag/codegraph/screen_capture/text_selection）、资源增长（memgrowth） |
| 插桩 | Debug 默认 ASan + UBSan（`AGENTXX_ENABLE_SANITIZER`），并对插件框架做**定向探针**（`lib/src/plugins/*.cpp`、`test/plugin/*.cpp`）；Release 下插桩与探针都关闭（否则缺运行库符号） |
| 基准 | `agentxx_benchmark`（一般仅 Release 编译）：资源基准（同进程 CLI/TUI、真实两进程、真实 TUI 含帧耗时/渲染字节、真实 server 空载漂移/WS 轮次/断连回收、PTY 驱动的 TUI 子进程、插件逐项边际内存）；指标含 RSS/PSS/私有脏页/匿名/峰值/线程/fd + glibc 堆在用与碎片 + `malloc_trim` 可回收 + smaps 模块级分解 + 逻辑内存 + 分阶段增量 + CPU 用户/内核时间；报告落 `{exec}/bench/bench_<时间戳>.{json,md}`，支持 `--baseline` 做 ΔRSS/ΔPSS/Δ堆 与模块级差异对比 |
| 隔离 | 聚合模块 `resource` 每个场景用独立子进程执行（避免前序场景污染基线），`AGENTXX_BENCH_NO_ISOLATE=1` 可退回同进程 |

### 16.2 openclaw 现状

**测试项目矩阵**（`test/vitest/`）：约 **200 个 `vitest.*.config.ts`**，按模块与用途切分，例如：

| 类别 | 例子 |
|---|---|
| 核心单元 | `vitest.unit.config.ts`、`vitest.unit-fast*.config.ts`（含 isolated / fake-timers 变体）、`vitest.unit-security.config.ts` |
| 分区聚合 | `vitest.full-core-{unit,contracts,runtime,support-boundary,tooling}.config.ts` |
| 按模块 | `vitest.agents*.config.ts`（含 `agents-core-isolated`、`agents-embedded-agent-*`、`agents-spawn-production-boundary`、`agents-tools`）、`vitest.gateway-*.config.ts`、`vitest.commands-*.config.ts`、`vitest.auto-reply-*.config.ts`、`vitest.channels.config.ts`、`vitest.cron/daemon/database-worker-*/hooks/logging/media/media-understanding/process/secrets/session-*/wizard/...` |
| 插件与扩展 | `vitest.plugins.config.ts`、`vitest.plugin-sdk{,-light}.config.ts`、`vitest.package-contract.config.ts`、每个扩展各有 `vitest.extension-<id>.config.ts` + 配套 `*-paths.mjs` |
| UI | `vitest.ui*.config.ts`（含 browser / e2e / e2e-prebuilt / timing / isolated） |
| TUI | `vitest.tui.config.ts`、`vitest.tui-pty.config.ts`（PTY 假后端通道） |
| 端到端/真机 | `vitest.e2e.config.ts`、`vitest.live.config.ts`、`vitest.full-agentic.config.ts` |
| 其它 | `vitest.boundary.config.ts`、`vitest.performance-config.ts`、`vitest.type-contracts` 相关、`vitest.startup-corpus-*` |

配套设施：

- **路径清单与分片**：每个大块都有 `vitest.<block>-paths.mjs`（导出该块的路径集合），加上 `vitest.test-shards.mjs` / `vitest.weighted-sharding.ts`（按权重分片）、`vitest.project-shard-config.ts`、`vitest.pattern-file.ts`（把 pattern 文件变成测试用例）。
- **报告器**：`vitest.reporters.ts` + `credential-redaction.ts` + `redacting-reporter.ts`（**测试输出也要脱敏**）。
- **超时与池**：`vitest.timeouts.ts`、`vitest.forks-pool.ts`（自定义 forks pool）、`vitest.fork-diagnostics.mjs`、`vitest.node-policy.global-setup.ts`。
- 目录职责：`test/{contracts,e2e,fixtures,helpers,mocks,plugins,scripts,tsconfig,type-contracts,types,vitest}`。

**其它质量门禁**：

| 门禁 | 说明 |
|---|---|
| 类型检查 | `tsgo` 各 lane；`pnpm changed:lanes --json` 用于确定受影响范围 |
| Lint | oxlint + 格式化 `oxfmt`；**自定义边界规则** `scripts/oxlint-boundary-guards.mjs`（`no-raw-window-open-call` / `no-register-http-handler-call` / `no-widen-then-assert` / `no-chained-type-assertions`） |
| 静态分析 | 十余份 CodeQL 配置，按「边界 + 重要度」拆（plugin-trust-boundary / process-exec-boundary / network-ssrf-boundary / channel-runtime-boundary / gateway-runtime-boundary / memory-runtime-boundary / mcp-process-* / provider-runtime-boundary / session-diagnostics-boundary / web-media-runtime-boundary / config-boundary / agent-runtime-boundary）；另有 semgrep（`.semgrepignore`）与 pre-commit 配置 |
| 生成物一致性 | `scripts/check-native-state-schema-version.mjs`（Swift/TS 状态库 schema 版本一致）；提示词快照 `pnpm prompt:snapshots:gen` / `:check`（漂移检查进 CI）；代码生成产物提交 + CI 校验 |
| 测试成本预算 | 根 `AGENTS.md` 明确要求：新测试要在 PR 里说明实测成本（`pnpm test <file> --maxWorkers=1` 的墙钟与 CI 秒数），且必须落在预算内：**禁止真实 timer/sleep/polling**、有 suite 级 fixture 时不得每个测试都启动网关或进程、不得新增串行配置或 worker pin、不得用宽 barrel import |
| CI 成本纪律 | 「CI 很贵，不要用它来做证明」——用本地定向运行证明改动，不要为了拿绿而反复重跑；`main` 变红是紧急事件（应当直接推修复到 `main`）；预先存在的失败不阻塞 land（但要指认） |
| 视觉门禁 | 视觉/渲染变化必须附已检查、已脱敏的前后截图，且 PR 里能渲染（见 §13.2） |
| 失败处理 | 把测试失败当缺陷：有界复现 → 找根因（fixture/共享状态/顺序/产品）→ 建立修复后补回归；**不得**用重试、加长超时、弱化断言、放宽 mock、改基线来掩盖 |

### 16.3 对比

| 维度 | agentxx | openclaw |
|---|---|---|
| 测试框架 | 自研测试程序（模块化注册 + fail-fast + 模块选择） | vitest 项目矩阵（约 200 个 project 配置）+ 权重分片 |
| 规模 | 测试代码 7.64 万行 / 172 文件；模块头 79 个 | 全仓 `*.test.ts` 18596 个；`src/` 内测试 10404 文件 / 141 MB |
| 分层 | 同步模块 + 异步（协程）模块 + 资源基准 | unit / unit-fast / isolated / contracts / boundary / e2e / live / UI / TUI-PTY / performance |
| 插桩 | Debug 默认 ASan+UBSan + 插件框架定向探针 | 无 sanitizer 类（JS）；有类型检查、lint 边界规则、CodeQL、semgrep |
| 生成物一致性检查 | 无 | 有（schema 版本一致、提示词快照、代码生成产物） |
| 成本预算 | 无（测试可自由追加） | 有明确预算与 PR 说明义务 |
| 报告脱敏 | 不涉及 | 有（credential-redaction + redacting reporter） |
| 性能/资源 | **有系统化资源基准**（模块级内存分解、基线对比、子进程隔离） | 有性能配置与 release-only 性能扫描（`docs/reference/release-performance-sweep.md`），但内存分解粒度不如前者 |
| UI 测试 | 组件层解析/渲染/命中单测 | PTY 假后端 + UI e2e + 浏览器时序 + 截图门禁 |
| 视觉门禁 | 无 | 有（硬门禁） |

### 16.4 两者的优缺点

**agentxx**

- 优点
  - **测试程序自研但很实用**：模块选择 + fail-fast + 同步/异步两类模块，运行时可控；79 个模块覆盖从协议到 UI 的完整链路。
  - **资源基准是真正的差异化优势**：RSS/PSS/私有脏页/匿名/峰值/线程/fd + glibc 堆在用与碎片 + `malloc_trim` 可回收 + smaps 模块级分解 + 逻辑内存 + 分阶段增量 + CPU 用户/内核；并且**每个场景独立子进程**避免污染、支持 `--baseline` 做机器对比。这套东西在 JS 项目里很难等价实现（GC 与运行时开销掩盖了细节）。
  - **Debug 双 sanitizer**（ASan + UBSan）加上插件框架定向探针，能把内存/未定义行为问题挡在 CI 之前。
  - **PTY 驱动的真实 TUI 子进程基准**覆盖了「真实界面线程开销」这个常被忽略的维度。
- 缺点
  - **没有测试成本预算**：新增测试不受约束，随规模增长会出现「跑一遍全量很慢」的问题（当前 79 模块尚可，但方向需要控制）。
  - **缺少生成物/契约一致性检查**：例如「文档里的接口表数量 vs 实现」「客户端能力表 vs 渲染清单」这类不一致只能靠人发现（§1 的 M3、§13 的 M68 就是补这个）。
  - **没有输出脱敏**：测试与基准报告可能包含真实路径、API key 片段（尤其在把配置内容打进断言时）。
  - **测试与实现的边界靠约定**：没有 lint 规则级别的守卫（例如禁止从测试里 import 内部实现细节）。
  - **模块命名/分层是平的**：`core/` 下 58 个头文件按功能命名，缺少子分类（`core/protocol/`、`core/session/`…），规模再大时定位会变慢。

**openclaw**

- 优点
  - **项目矩阵 + 路径清单 + 权重分片**：把「跑哪些测试、怎么并行」变成配置问题，能支撑 1.8 万个测试文件的规模；每块都有 `*-paths.mjs` 便于被其它块复用与检查。
  - **测试成本预算是明文规则**：禁止真实 timer/sleep/polling、禁止 per-test 起网关、限制串行配置与 worker pin、限制 barrel import——这几条直接决定 CI 能否长期维持。
  - **生成物与契约一致性进 CI**：schema 版本双端一致、提示词快照漂移、代码生成产物校验——都是「文档/契约与实现漂移」的自动拦截。
  - **报告脱敏**：连测试输出都要过 redactor，这对含凭据的多用户场景是必须的。
  - **边界静态守卫**：自定义 oxlint 规则 + 十余份按边界拆的 CodeQL，把架构约束变成可执行检查。
  - **失败处理纪律**：明确禁止用重试/加超时/弱化断言掩盖失败，并要求根因修复 + 回归。
- 缺点
  - **配置复杂度高**：约 200 个 vitest 配置 + 大量 `*-paths.mjs` + 分片与池定制，本身成为需要维护的资产。
  - **测试量带来的运行时间**：1.8 万个测试文件即使分片也需要相当资源，所以才有那么多「不要浪费 CI」的规则。
  - **没有系统化的资源基准**：Node 运行时下内存细节不可控，它的性能工作偏「场景耗时」而非「内存分解」。

### 16.5 可迁移到 agentxx 的设计

| 编号 | 设计 | 落地位置 | 优先级 | 说明 |
|---|---|---|---|---|
| M80 | **测试成本预算（写进规则文件 + 度量）** | `agent/test/AGENTS.md`（配合 M1）+ 测试程序支持「模块耗时打印」 | P1 | 现在模块跑完只打印 passed/failed，没有耗时。建议：① 每个模块打印墙钟耗时与用例数；② 规则文件写明「新增模块的耗时上限（例如同步模块 < 1s、异步模块 < 5s）与理由」；③ 避免在测试里用真实睡眠/轮询（改用可控时钟或事件驱动等待）。 |
| M81 | **契约/一致性检查进构建** | `agent/script/`（新增一个 check 脚本）+ CI/构建前置 | P1 | 至少三条：① 插件接口表常量集合 == 宿主声明集合（M3）；② `kTuiBlockNames` == 渲染层实际实现的分支（M68）；③ 文档中关键数字（表数量、模块数）由脚本生成或校验。这三条都是「文档/契约漂移」的高发点。 |
| M82 | **测试输出与报告脱敏** | `agent/test` 输出路径 + `agentxx_benchmark` 报告生成 | P1 | 规则：打印真实路径时用相对路径或 `~` 归一化；配置里的 api key 一律不进断言输出；基准报告的 smaps 模块名白名单化。成本极低，避免把凭据/隐私路径写进日志与报告文件。 |
| M83 | **测试目录二次分类** | `agent/test/include/agentxx-test/core/` | P2 | 当 core 下模块继续增长时，按 `protocol/`、`session/`、`tools/`、`middleware/`、`ui/` 再分组（与 `agent/lib` 的目录结构对应）。现在 58 个平铺头文件已经开始需要靠名字猜分类。 |
| M84 | **基准的门禁化用法（可选）** | `agentxx_benchmark --baseline` | P2 | 已有 `--baseline` 做 ΔRSS/ΔPSS/Δ堆 对比。可在「内存相关改动」的规则里写成一条：改动了会话/上下文/插件加载路径时，必须跑一次基准并附 Δ 数字（openclaw 要求 agent 性能改动记录 before/after 秒数与 RSS，这只是把它本地化）。 |

---

## 17. 配置、模型目录与装配

### 17.1 agentxx 现状

**分层配置加载**（`agent/client/src/config_loader.cpp`）：

- **overlay**：工作目录下的 `agentxx-config.yaml`（或 `--config` 指定）+ 同目录 `.env`。
- **base**：由 overlay 的 `data_dir` 指向的目录下的 `agentxx-config.yaml` / `.env`；未配置 `data_dir` 时取系统数据目录 `~/.agentxx/`（工作目录没有 yaml 时即直接加载该配置）。
- **base 与 overlay 指向同一文件时只加载一次**（避免同一配置被当成两层重复合并）。
- 变量替换：`${ENV}` 风格由 `.env` 与进程环境提供；相对路径按程序工作目录解析、`~` 展开。
- 列表段的统一结构：`{overwrite: {mode: merge|replace, remove: [...]}, list: [...]}`——`merge`（默认）按键归并/追加去重、`replace` 整段不继承、`remove` 按身份剔除 base 项（身份口径：model=name / plugin=path·name / mcp=namespace / 路径列表=字符串）；旧键与纯列表写法会被告警并忽略。
- 关键配置项：`data_dir`、`work_dir`（会话工作目录）、`model.{list,use}`、`plugin.list`、`mcp`、`worktree.enable`、`permission.mode`、`enableSubagent`、`llmMaxRetry`、`toolcallSummaryLimitOutputLength`、`toolcallRepeatCheckThreshold`、`availableModels`（含多模态能力）、`language`。
- **客户端设置项**存在 `settings_db`（全局 KV，如 `tui.checkUpdateOnStartup`）；会话级覆写走 `AgentConfig::sessionWorkDir` 等按 sessionId 取值的段。
- **插件配置**：经 `pluginxx.config` 表读取，插件自管键空间。

### 17.2 openclaw 现状

**分层配置 + 「迁移归 doctor 所有」**：

- 运行时**只读当前 schema**；**不保留长期别名或兼容分支**去静默接受旧键。当配置变更使现有用户配置失效时，**同一个改动必须带上 doctor 迁移**：`openclaw doctor --fix` 检测旧形态、解释它、必要时备份、并改写成规范格式。
- 核心拥有的配置与鉴权状态由核心 doctor 代码修复；**插件拥有的配置由该插件的 doctor 契约修复**。
- 配置面文档分很多页：`configuration.md`、`configuration-reference.md`、`configuration-examples.md`、以及按领域拆的 `config-{agents,automation,browser-ui-desktop,channels,cloud-workers,extensions,gateway,hooks,observability,runtime,secrets-env,tools}.md`；`docs/gateway/doctor/config-migrations.md` 是迁移的共享 owner。
- 键名/凭据语义有专门文档（`auth-credential-semantics.md`、`secret-placeholder-conventions.md`、`secretref-credential-surface.md` + 一份 JSON 矩阵）。

**装配与目录**：

- **每 agent 一份模型运行时世代快照**（见 §8.2）：鉴权模板 + 模型注册表 + 投影目录作为原子快照；**浏览/状态/cron/doctor/TUI/PDF/图像等路径读已发布目录**，不重复文件系统发现。
- 模型目录来自远程 + 本地缓存（`model-catalog-core` + `src/model-catalog`），选择 UI 在 `src/model-picker`。
- 装配相关模块：`src/config`（1226 文件）、`src/state`、`src/plugins`（清单与激活计划）、`src/secrets`（215 文件）、`src/infra`。
- 多 agent 阵容（roster）：`agents.entries.*`，每 agent 可覆盖工作区、模型、工具策略、沙箱、prompt 参数（`docs/gateway/config-agents.md`）。
- 引导与修复入口：`openclaw onboard` / `configure` / `setup`（创建工作区与种子文件）、`openclaw doctor`（诊断 + `--fix`）、`openclaw config`（CLI 读写配置）、Control UI 设置页。

### 17.3 对比

| 维度 | agentxx | openclaw |
|---|---|---|
| 分层 | base（data_dir）+ overlay（工作目录/--config）+ .env | 全局配置 + 每 agent 条目 + 插件配置 + 环境变量 / SecretRef |
| 列表合并 | 显式 `overwrite.{mode,remove}` 语义（merge/replace/remove + 按身份剔除） | 无同类通用机制（配置以键为主，列表型配置各自定义语义） |
| 配置迁移 | 旧键/旧写法告警并忽略；无迁移工具 | **doctor 拥有迁移**，同一改动必须带迁移；核心与插件各自的 doctor 契约 |
| 装配 | 启动时构造 `AgentConfig` + `ModelProviderRegistry`（进程内一次性装配） | 每 agent 一份模型运行时世代快照，原子替换，陈旧世代不服务 |
| 目录发现 | 配置文件里手写模型条目 | 远程模型目录 + 本地缓存 + 投影；浏览/状态等路径读已发布目录 |
| 配置来源 | yaml + .env；设置项在 settings_db（客户端侧） | 配置文件 + 环境变量 + SecretRef + Control UI + CLI；凭据语义有专门文档与矩阵 |
| 多实例/多 agent | 多会话同 agent；多 agent 靠进程或多 AgentHost | 多 agent 阵容（roster）+ 每 agent 工作区/模型/工具策略覆盖 |
| 校验时机 | 加载时解析（类型/结构）；插件配置由插件自管 | 清单优先：不执行插件代码即可校验其配置；核心配置有 schema |

### 17.4 两者的优缺点

**agentxx**

- 优点
  - **base/overlay 分层 + 列表段语义是实用设计**：`merge`/`replace`/`remove`（按身份剔除）三态正好覆盖「项目级追加」「整段替换」「剔除默认项」三类真实需求，比「深层合并」这类含糊规则更可控。
  - **`data_dir` 单一入口**：数据目录由客户端重定向，库侧不假设路径；便于测试、便携部署与多实例隔离。
  - **同一文件只加载一次**的防护考虑到了 `--config <data_dir>/agentxx-config.yaml` 这类自指情况。
  - **设置项与会话配置分开**：客户端偏好（settings_db）与服务端 agent 配置分离，职责清楚。
- 缺点
  - **没有配置迁移工具**：改键名/改结构只能「告警并忽略旧键」，老用户要么手工改要么丢配置；openclaw 的做法（同一改动必须带 doctor 迁移 + 备份 + 回执）明显更成熟。
  - **缺配置校验的「提前量」**：插件配置要到 `start` 失败才发现问题（M75 要解决的正是这条）。
  - **装配是启动时一次性的**：没有「快照替换」概念（§8 的 M43），热更新路径需要额外小心。
  - **文档没有单一参考页**：配置项散落在 design/index.md（166 KB）里，查找成本高。

**openclaw**

- 优点
  - **「迁移归 doctor 所有」是一条能长期演进的硬规矩**：运行时只读当前 schema，禁止兼容读取路径，避免「兼容分支越积越多」这一最常见的配置腐化模式；插件也必须自带 doctor 契约。
  - **配置面按领域分页**（13+ 篇 config-* 文档）+ 键名/凭据专门文档 + JSON 矩阵，可查性强。
  - **世代快照装配**让「配置改了但一半模块还用旧值」这类问题不可能出现。
  - **多 agent 阵容与逐 agent 覆盖**：工作区、模型、工具策略、沙箱都能按 agent 覆盖，适合「一个网关服务多个人/多个角色」。
- 缺点
  - **配置面极广**：`src/config` 1226 文件，加上 SecretRef/凭据语义/环境变量矩阵，学习成本高。
  - **没有 agentxx 那种通用列表合并语义**（列表型配置各自定义），跨配置项的心智模型不统一。
  - **doctor 是必需环节**：升级路径强依赖 `openclaw doctor --fix` 被执行；跳过它的用户会停在「配置无效」的状态（它用更新器预检与提示来缓解）。

### 17.5 可迁移到 agentxx 的设计

| 编号 | 设计 | 落地位置 | 优先级 | 说明 |
|---|---|---|---|---|
| M85 | **配置迁移骨架（config version + migrate）** | `agent/client/src/config_loader.cpp` + `docs/zh-cn/design/index.md` | P1 | 现在旧键只被「告警并忽略」。建议：配置顶部支持 `config_version`（整数，缺省按 1）；加载时若版本低于当前，依次跑迁移函数（键重命名/结构改写）并在**改前备份原文件**、迁移后写回；迁移函数与被废弃的键集中在一处（一个 `config_migrations.cpp`），避免散落。这是 M22（会话库 schema 版本）在配置侧的对应物。 |
| M86 | **配置的单一参考页** | 新增 `docs/zh-cn/design/config.md` | P2 | 从 index.md 抽出完整配置参考（每个键：类型、默认值、作用、是否热更新、示例），index.md 只留指向它的链接。同时可让「文档里的键集合」由脚本与解析器比对（M81 的第 ③ 条）。 |
| M87 | **插件配置的加载前校验** | 与 M75 同批：`plugin.yaml` 增加 `config_schema` 段 | P1 | 宿主在加载插件前按其声明的 schema 校验配置，给出「哪个键、期望什么、实际什么」的可读错误；插件不必在 `start` 里手写校验。与 M75 是同一件事的两侧（清单侧 + 宿主侧）。 |
| M88 | **装配快照 + 版本号（与 M43 合并）** | `AgentConfig` / `ModelProviderRegistry` | P2 | 配置热更新时：整体重建快照 → 递增 `configGeneration` → 替换；所有读取路径读快照而不是读配置对象；日志与 wire 消息带上 `configGeneration`，便于诊断「客户端看到的配置」与「服务端生效的配置」是否一致。 |

---

## 18. openclaw 独有侧重的域：多通道接入与自动化

> 这一节的内容在 agentxx 里**没有对应物**，因此不做「两侧现状对比」，只记录「它解决了什么问题、用了什么机制、agentxx 在什么条件下会需要」。

### 18.1 通道边界契约（channel boundary）

这是 openclaw 最值得研究的一层抽象。它把「消息通道」的责任切得很清楚（`src/channels/AGENTS.md` + `docs/plugins/sdk-channel-plugins.md`）：

| 归属 | 内容 |
|---|---|
| **核心拥有** | 共享的消息工具、动作词汇（action vocabulary）与派发（dispatch）；「保留类型化的命令/审批/URL/动作区分直到编码」；**绝不从原始字符串推断产品命令** |
| **通道拥有** | 自己的账号、安全、会话与传输契约（`channel.ts` / `channel.setup.ts` / `gateway.ts` / `outbound.ts`） |
| 扩展面 | 插件只能用 `openclaw/plugin-sdk/*`，不得直接 import `src/channels/**`；需要一个新接缝就先加类型化 SDK 契约或门面 |

配套的工程规则（不是理念，是可执行的约束）：

- **热导入路径**：`channel.ts` / `shared.ts` / `channel.setup.ts` / `gateway.ts` / `outbound.ts` 被视为热路径，**不得静态拉入** send / monitor / probe / directory-live / setup-login 这类异步面与大型 `runtime-api.ts` barrel；要用 `channel-api.ts` / `*.runtime.ts` 这类小接缝把重代码挡在热路径之外。
- **发现路径优先轻量产物**：网关与 agent 工具的发现路径优先用轻量的 bundled-plugin 产物，只有回退时才加载完整通道插件。
- **不要混用静态与动态导入**同一重型模块族（要么全程惰性，要么全程加载）。
- **共享通道改动影响全体**：改共享代码要检查路由、配对、allowlist、命令门控、onboarding、回复行为在所有通道上的表现。
- 验证时跑构建 + 用 `scripts/profile-extension-memory.mts` 剖析受影响插件的启动成本。

**留给 agentxx 的问题**：agentxx 目前只有「客户端」这一种外部入口（本地 stdio、FTUI TUI、远程 WS）。若将来要接「第二个外部入口」（例如 IM 机器人、HTTP webhook、邮件），这套「核心拥有动作词汇与派发 / 入口拥有账号与传输 / 重代码必须惰性」的分工是最省事的起点。

### 18.2 多用户与多通道会话路由

| 机制 | 语义 |
|---|---|
| `session.dmScope` | 私聊共用 main（默认）/ 按发送者 / 按通道+发送者（推荐）/ 按账号+通道+发送者 |
| `session.groupScope` | 群/房间/频道各自独立（默认）或路由进 main；路由绑定可按通道+peer 覆盖 |
| `session.identityLinks` | 把同一个人在不同通道的身份映射到一个规范 peer id，共用会话 |
| 线程绑定会话 | Slack Agent/Assistant View 的每个可见根都有自己的 `:thread:<rootTs>` 会话；子代理也可绑定到通道线程 |

**关键点**：路由是**会话归属**问题（谁和谁共用上下文），与「谁能看见什么」的安全问题绑定，所以文档反复强调「多用户场景必须开 DM 隔离，否则 Alice 的私聊会被 Bob 看到」。

**留给 agentxx 的问题**：agentxx 的会话是与「工作目录 + 本地用户」绑定的，没有「多人共用同一 agent」的概念。若将来做多用户（例如一个团队共用一个 agent server），这套「作用域 + 身份链接 + 默认隔离」的取舍需要照抄一遍（尤其是**默认必须隔离**这条）。

### 18.3 自动化与常驻授权

| 机制 | 说明 |
|---|---|
| **cron 任务** | 定时/周期任务，隔离的 cron agent 回合持 `cron` 槽位、内部执行用 `cron-nested`；有独立的超时与清理（超时后跑有界清理再记录，避免陈旧子会话把车道卡住） |
| **心跳（heartbeat）** | 定期唤醒 agent 检查是否有事要做；嵌入运行走 `cron-nested` 车道，避免挡住入站回复；主会话观察者会被心跳立刻唤醒 |
| **常驻指令（standing orders）** | 用「程序（program）」的形式授予**长期运行授权**：每个程序声明 **范围 / 触发器 / 审批门 / 升级规则**；建议写进 `AGENTS.md`（每会话自动注入）。这是「不用每次提示就能自主干活」的组织方式 |
| **hooks** | 进程内两类：内部 hooks（`HOOK.md`，命令与生命周期事件如 `command:new`）+ 插件 hooks（类型化 `api.on`）；另有 **HTTP webhooks**（接受外部请求触发工作，与「订阅 agent 循环事件」是两回事） |
| **IMAP 等外部触发** | 邮件等外部源的接入插件 |

**留给 agentxx 的问题**：agentxx 没有任何「未来时间点」的概念——没有 cron、没有心跳、没有常驻授权。要补的话有两层：**机制层**（一个调度器 + 任务表，对应 M58）与**授权层**（「哪些事可以自主做、哪些必须问」的显式声明，对应 standing orders 的「范围/触发器/审批门/升级规则」四要素）。后者比前者更容易被忽略，但它是「后台自主执行」能被信任的前提。

### 18.4 做梦（dreaming）与离线整理

openclaw 把「记忆整理」做成一条**独立的后台通道**（`docs/concepts/dreaming.md`，19 KB）：

- 它与其他后台工作（技能工作坊、插件后台完成）**共享 3 个并发槽位**，且「等待它的调度器不占槽位」。
- 产出物是**给人读的报告**（`DREAMS.md` + dreaming 报告），以及经门控晋级到精编核心的记忆条目（见 §7.2）。
- 与之并列的还有 `active-memory`（主动记忆）、`user-model`（用户模型）、`standing-intents`（常驻意图）。

**留给 agentxx 的问题**：M36 已经提出轻量版。要强调的正是 openclaw 的两条设计原则：**① 整理不占回复路径；② 产出必须可人工复核**（不要做「后台悄悄改长期记忆」这种不可审计的设计）。

### 18.5 语音、会议与浏览器（能力层）

| 域 | 规模 | 说明 |
|---|---|---|
| 语音 | `src/talk`（88 文件）+ `src/tts`（58）+ `realtime-transcription`（4）+ `extensions/{elevenlabs,deepgram,...}` | 实时语音对话、唤醒词、语音留言、TTS provider 插件 |
| 会议 | `src/meeting-bot`（98）+ `extensions/{google-meet,teams-meetings,zoom-meetings}` | 会议机器人 + 转录源 provider（`api.registerTranscriptSourceProvider`）+ 会议转录表（`meeting_transcript_sessions` / `_utterances` / `_summaries`） |
| 浏览器 | `extensions/browser` + `src/proxy-capture` | 浏览器控制、登录流程、WSL2/Windows 远程 CDP 排障、浏览器认证 |
| 画布/门户 | `src/canvas`（21）+ `gateway/portals` | 托管 widget 文档（`/__openclaw__/canvas/`）与 A2UI 渲染资产（`/__openclaw__/a2ui/`） |
| 图像/音乐/视频 | `src/{image,music,video}-generation` + `media-generation` | 见 §12 |

**留给 agentxx 的问题**：这些都是「个人助理」形态的能力，对 coding agent 不是刚需。**不建议迁移**，但其中两条方法论值得记住：① 能力做成 **provider 型插件**（同一个能力面多种实现，靠注册方法统一）；② 能力的**进度与状态**要进上下文（openclaw 把媒体生成进度放进 Runtime Context 载体），而不是让模型猜。

### 18.6 本节小结

| 域 | 对 agentxx 的相关性 | 建议 |
|---|---|---|
| 通道边界契约 | 中（未来接第二个外部入口时） | 记住三原则：核心拥有动作词汇与派发、入口拥有账号与传输、重代码必须惰性 |
| 多用户会话路由 | 中（未来做共享 agent 时） | 记住一条：默认必须隔离 |
| 自动化与常驻授权 | 高 | 机制层 M58（调度器 + 任务表）；授权层要显式声明「范围/触发器/审批门/升级规则」 |
| 离线整理（做梦） | 高 | M36 轻量版；坚持「不占回复路径」与「产出可复核」 |
| 语音/会议/浏览器 | 低 | 不迁移；仅记住「能力 provider 化」与「进度进上下文」两条 |

---

## 19. 迁移建议汇总

本文共提出 **95 项**迁移建议（M1–M95），按优先级与主题归档如下。优先级判据是「收益 ÷ 落地成本」，并考虑与 agentxx 现有架构的契合度。

> **本轮（实现精读）的增量**：M89–M95 共 7 项为本轮逐行读实现后新增（其中 M28 被按实际情况重写——它原本要求的「保真落盘」在 agentxx 里已经实现）。§1.5–§18.6 中新增的「实现细读」小节记录了这些结论的依据。

| 优先级 | 数量 | 判据 |
|---|---|---|
| **P0** | 6 | 现在就有明显损失（错误、卡住、上下文膨胀、重复执行），且落地成本低 |
| **P1** | 34 | 明显改善可靠性/可维护性/用户体验，需要一两天的实现与测试 |
| **P2** | 51 | 有价值但依赖前置项、或收益偏长期、或需要先评估 |
| （无需迁移） | 4 | 调研后确认 agentxx 现状即可（或差异是正当取舍） |

### 19.1 P0（建议优先落地，共 6 项）

| 编号 | 主题 | 设计 | 章节 | 落地位置 |
|---|---|---|---|---|
| M11 | 上下文与提示词 | **给系统提示词划一条显式缓存边界**（stablePrefix / dynamicSuffix） | [§3.5](#35-可迁移到-agentxx-的设计) | `agent/lib/src/agent/context.cpp`、`agent/lib/include/agentxx/agent/prompt.h` |
| M26 | 工具 | **同轮工具并行执行（带启动检查点）** | [§6.5](#65-可迁移到-agentxx-的设计) | `agent/lib/src/nodes/toolcall.cpp` 的 `baseRun` |
| M33 | 记忆 | **记忆分层 + 按需检索**（常驻类 vs 检索类） | [§7.5](#75-可迁移到-agentxx-的设计) | `MemoryFileMiddlewareHandle` |
| M38 | LLM | **模型空闲看门狗**（静默超阈值即中止并按既有重试处理） | [§8.5](#85-可迁移到-agentxx-的设计) | `agent/lib/src/nodes/modelcall.cpp` |
| M44 | 压缩 | **溢出错误的识别与压缩重试**（不重放已完成工具） | [§9.5](#95-可迁移到-agentxx-的设计) | `ModelCallWrapNode` + `SummarizationMiddlewareHandle` |
| M69 | 协议 | **副作用消息的幂等键** | [§14.5](#145-可迁移到-agentxx-的设计) | `wire_protocol.h` + `SessionServerAgentIO` |

这 6 项的共同点：都不改变现有架构形状（不加新层、不改 ABI、不动图引擎），但各自消除一类明确的损失——提示词前缀缓存被动态段打断（M11）、同轮独立工具白等（M26）、全量注入记忆吃上下文（M33）、provider 卡住只能等整体超时（M38）、估算偏低直接失败一轮（M44）、网络抖动导致重复执行（M69）。

### 19.2 P1（分批推进，共 34 项）

**A. 工程护栏与规则（7 项）**

| 编号 | 设计 | 章节 |
|---|---|---|
| M1 | 按目录放局部规则文件（scoped `AGENTS.md`） | [§1.5](#15-可迁移到-agentxx-的设计) |
| M2 | 把边界规则做成可执行检查（三条最基本的 include 规则） | [§1.5](#15-可迁移到-agentxx-的设计) |
| M80 | 测试成本预算（模块耗时打印 + 上限规则） | [§16.5](#165-可迁移到-agentxx-的设计) |
| M81 | 契约/一致性检查进构建（接口表集合、能力表、文档数字） | [§16.5](#165-可迁移到-agentxx-的设计) |
| M82 | 测试输出与报告脱敏 | [§16.5](#165-可迁移到-agentxx-的设计) |
| M85 | 配置迁移骨架（`config_version` + 迁移函数 + 改前备份） | [§17.5](#175-可迁移到-agentxx-的设计) |
| M87 | 插件配置的加载前校验（清单声明 schema + 宿主校验） | [§17.5](#175-可迁移到-agentxx-的设计) |

**B. 会话运行与持久化（6 项）**

| 编号 | 设计 | 章节 |
|---|---|---|
| M6 | 投递模式显式化（补 `steer` 与 `collect`） | [§2.5](#25-可迁移到-agentxx-的设计) |
| M7 | 排队状态的显式状态机（`QueueState` 枚举取代两个 bool） | [§2.5](#25-可迁移到-agentxx-的设计) |
| M21 | 写入权威的显式断言（轻量版写者租约 `writerEpoch`） | [§5.5](#55-可迁移到-agentxx-的设计) |
| M22 | 会话级 schema 版本与迁移骨架 | [§5.5](#55-可迁移到-agentxx-的设计) |
| M50 | 运行期权威复验的显式规矩 | [§10.5](#105-可迁移到-agentxx-的设计) |
| M70 | 协议版本与能力协商字段（`protocolVersion`） | [§14.5](#145-可迁移到-agentxx-的设计) |

**C. 上下文、压缩与记忆（5 项）**

| 编号 | 设计 | 章节 |
|---|---|---|
| M12 | 补齐「上下文装配报告」（每轮的装配与压缩事实） | [§3.5](#35-可迁移到-agentxx-的设计) |
| M13 | 压缩触发口径统一（`ContextBudget` 单一来源） | [§3.5](#35-可迁移到-agentxx-的设计) |
| M34 | 记忆条目的来源标记（`[owner]`/`[agent]`/`[untrusted]`） | [§7.5](#75-可迁移到-agentxx-的设计) |
| M35 | 技能来源优先级 + 同名冲突裁决 | [§7.5](#75-可迁移到-agentxx-的设计) |
| M60 | `ask_user` 式结构化提问工具 | [§12.5](#125-可迁移到-agentxx-的设计) |

**D. 工具与权限（7 项）**

| 编号 | 设计 | 章节 |
|---|---|---|
| M27 | 工具结果落上下文前的守卫（UTF-8/限幅/配对自洽） | [§6.5](#65-可迁移到-agentxx-的设计) |
| M28 | 工具结果的聚合预算（已有 offload 之上补聚合上限） | [§6.5](#65-可迁移到-agentxx-的设计) |
| M29 | 「可用性与授权」两层拆开 | [§6.5](#65-可迁移到-agentxx-的设计) |
| M48 | 审批记住的选择持久化（+ 可查询/可撤销） | [§10.5](#105-可迁移到-agentxx-的设计) |
| M61 | 附件来源显式校验（按实现现状收窄为两条） | [§12.5](#125-可迁移到-agentxx-的设计) |
| M17 | 写清「默认 cwd ≠ 沙箱」并给工作区外写边界 | [§4.5](#45-可迁移到-agentxx-的设计) |
| M89 | 缓存过期感知的结果剪枝 | [§6.5](#65-可迁移到-agentxx-的设计) |

**E. LLM 与子代理（5 项）**

| 编号 | 设计 | 章节 |
|---|---|---|
| M39 | token 用量与缓存统计 | [§8.5](#85-可迁移到-agentxx-的设计) |
| M40 | 模型级回退链（轮次局部，不改会话选择） | [§8.5](#85-可迁移到-agentxx-的设计) |
| M54 | 子代理「fork 父上下文」模式 | [§11.5](#115-可迁移到-agentxx-的设计) |
| M55 | 子代理默认收窄工具面 | [§11.5](#115-可迁移到-agentxx-的设计) |
| M56 | 后台工作独立预算（等待方不占槽位） | [§11.5](#115-可迁移到-agentxx-的设计) |

**F. 客户端与插件（4 项）**

| 编号 | 设计 | 章节 |
|---|---|---|
| M65 | 渲染与命中的快照回归 | [§13.5](#135-可迁移到-agentxx-的设计) |
| M74 | 单槽位（slot）替换语义 | [§15.5](#155-可迁移到-agentxx-的设计) |
| M75 | 清单能力对齐（配置 schema / UI 提示 / 静态能力快照） | [§15.5](#155-可迁移到-agentxx-的设计) |
| M76 | 插件文档分页化 + 快速上手 | [§15.5](#155-可迁移到-agentxx-的设计) |

### 19.3 P2（评估后实施，共 51 项）

按主题分组列出，编号后括号内为章节。

| 主题 | 编号（设计） |
|---|---|
| 规则与文档 | M3（接口清单单一来源）、M4（控制面/运行面约定）、M68（UI 改动验收清单）、M83（测试目录二次分类）、M84（基准门禁化用法）、M86（配置单一参考页） |
| 会话与协议 | M8（排队项持久化）、M9（车道化并发预算）、M10（debounce 参数化）、M23（会话事件序列）、M24（避免长耗时写入阻塞 io 线程）、M25（外部改动探测）、M71（协议 schema 机器可读）、M72（连接级鉴权钩子）、M92（重放缓冲的观察者计数与按需释放）、M94（取消/异常时立即产出合法 transcript 尾部） |
| 上下文与记忆 | M14（压缩幂等键）、M15（提示词快照回归）、M16（装配可替换出口）、M36（后台整理通道）、M45（压缩质量门）、M46（压缩挪到回复结算后）、M47（压缩前提醒写记忆）、M93（轮次内容来源标记 taint） |
| 工具与权限 | M30（压缩后重复调用守卫）、M31（审批执行身份绑定）、M32（工具面诊断入口）、M49（执行身份绑定落地）、M51（权限规则查询/撤销）、M52（仅元数据审计）、M53（凭据掩码流程）、M90（预览首尾保留 + 中段省略）、M91（存储形态/模型投影分离）、M95（符号链接规范化） |
| LLM 与装配 | M41（多鉴权档位与冷却）、M42（缓存保留策略）、M43（装配快照化）、M88（装配快照 + 版本号） |
| 子代理与自动化 | M57（「完成 ≠ 目标达成」提示）、M58（定时/常驻任务）、M59（跨会话观察者） |
| 客户端与媒体 | M62（大内容取回约定）、M63（媒体能力插件化）、M64（富输出显式通道）、M66（可更新进度展示位）、M67（能力协商补体验级） |
| 插件 | M77（插件成本剖析工具）、M78（hook 决策语义规则）、M79（卸载后配置回落防护） |
| 设备与位置 | M18（轻量设备能力协商）、M19（命令执行位置字段） |

### 19.4 无需迁移（4 项）

| 编号 | 主题 | 结论 |
|---|---|---|
| M5 | 存储归一 | agentxx 已是「会话库 + 图 checkpoint + settings_db 全走 sqlite」，只需把这条约定写进规则文件（并入 M1）。 |
| M20 | 身份文件与执行目录分离 | agentxx 的提示词是内建 + 插件追加段，天生没有「执行目录里的身份文件」问题；仅在将来支持工作区身份文件时需同步定这条规则。 |
| M37 | 技能校验错误的可见性 | `SkillMiddlewareHandle` 已有 `loadErrors`；只差把错误显示到界面上，属顺手可做的小改动。 |
| M73 | 重放 vs 刷新 | agentxx 的重放机制比 openclaw 更好，结论是**保持并写清前提**，不是迁移。 |

### 19.5 落地批次建议

| 批次 | 内容 | 理由 |
|---|---|---|
| **第 1 批（低成本高收益）** | M11、M38、M44、M26、M69、M33 | 六项 P0。彼此独立、都不动架构形状，可并行开工；每项都能配一条可验证的测试。M26 落地时请参考 §2.6 的三条实现约束（派发前统一询问、启动期提交握手、按 `tool_call_id` 归位）。 |
| **第 2 批（护栏先行）** | M1、M2、M81、M82、M80 | 先把「规则可执行、契约可校验、输出可脱敏、测试有预算」立起来，后续所有改动都受益（也避免第 3 批起就开始累积漂移）。 |
| **第 3 批（运行语义）** | M7、M6、M21、M22、M13、M12 | 排队状态机 + 投递模式 + 写者租约 + schema 版本 + 预算口径统一 + 装配报告。这六项互相咬合（状态机影响 M6，写者租约是 M22/M46 的前提）；M21 的实现顺序以 §5.6 的四步为准。 |
| **第 4 批（上下文与工具口径）** | M27、M28、M29、M89、M60、M61、M48、M50、M35、M34、M93 | 工具结果的两层预算与缓存感知剪枝（M28/M89）、结果守卫、可用性/授权分层、结构化提问、附件两条校验、审批记忆持久化、权威复验规矩、技能优先级、记忆与轮次的来源标记（M34/M93 同批做最省事）。 |
| **第 5 批（可靠性与装配）** | M39、M40、M54、M55、M56、M70、M85、M87、M65、M74、M75 | LLM 用量与回退、子代理三项、协议版本、配置迁移、插件配置校验、渲染快照、插件槽位与清单。 |
| **第 6 批（按需）** | 其余 P2（含本轮新增的 M90、M91、M92、M94、M95） | 依赖前置项或收益偏长期，按实际需求挑选；其中 M58（定时任务）与 M36（后台整理）建议与产品需求一起评估；M95（符号链接）建议与 M17（边界说明）一起做，先写清限制再决定是否实现二次判定。 |

---

## 20. 反向清单：agentxx 不必照搬的设计

以下差异经调研后确认是**正当取舍**，不应照搬（照搬反而会削弱现有优势或引入与定位不符的成本）。

| # | 不照搬的设计 | 原因 |
|---|---|---|
| R1 | **TypeBox/JSON Schema → 代码生成的协议体系** | agentxx 的 36 类 wire 消息与手写结构体已足够清晰；引入生成管线会带来构建复杂度与依赖，收益（多语言客户端）在当前并不存在。真要做的是 M71（导出一份 schema 供第三方用）。 |
| R2 | **把数据库访问全部下沉 worker 线程** | agentxx 是单 `io_context` 协程模型，短事务在 io 线程执行是合理且简单的；只有「超大写入」需要规避（M24）。引入 worker 体系会带来跨线程状态同步的复杂度，与「无锁」的设计取向冲突。 |
| R3 | **每 agent 一个数据库 + 共享状态库的双库结构** | agentxx 按会话分库（删会话＝删目录、会话级 WAL 写入串行）更贴合其使用形态。要学的是**schema 版本骨架**（M22），不是库的划分方式。 |
| R4 | **多前端各自渲染（含原生壳）** | agentxx 的「声明式组件描述 + 适配 + 唯一渲染实现」在跨端一致性上更强；代价是只有一种渲染实现，但这正是它省成本的原因。 |
| R5 | **富输出的文本标记（`MEDIA:` / `[[audio_as_voice]]` / `[embed …]`）** | agentxx 的附件已是结构化字段、界面走组件描述。引入文本标记会让「内容」与「交付指令」混在一起，是倒退。 |
| R6 | **数十种 provider 溢出错误串的完整匹配表** | 维护负担大且跟随各家 API 变化。agentxx 只需覆盖自己实际支持的三种协议类型（openai / openai-responses / anthropic）最常见的几条串（M44）。 |
| R7 | **完整的设备节点体系 / ClawHub 市场 / 原生壳** | 超出「可嵌入的 agent 库」的定位；agentxx 只取其中「能力协商」的一半（M18）。 |
| R8 | **媒体、语音、会议、浏览器的能力层** | 不是 coding agent 的刚需，且会持续增加核心上下文成本（违背「小核心」原则）。若需要，按 M63 做成插件。 |
| R9 | **约 200 个测试 project 的配置复杂度** | 这是 openclaw 规模（1.8 万个测试文件）的产物。agentxx 的 79 个模块用「模块名 + 耗时打印 + 预算规则」（M80）就够；分层测试的价值用「模块内分层」实现即可。 |
| R10 | **四种插件形态与 hook 的复杂决策语义** | agentxx 的 C ABI + 五态生命周期 + 接口表已比它更严格；需要学的是**决策语义写清楚**（M78），不是把形态分类照搬。 |
| R11 | **「事件不重放，客户端自行刷新」** | agentxx 的 `seq` + 环形缓冲重放对客户端更友好。不照搬。 |
| R12 | **可选沙箱/容器隔离** | agentxx 的定位是嵌入库：隔离应由宿主进程/容器边界提供；库内做半套沙箱只会制造「看起来安全」的错觉。要做的是把边界写清楚（M17）。 |
| R13 | **流程型的重量门禁（截图门禁、PR 模板、release 预检清单）** | 这些是 openclaw 多贡献者协作与产品发布的需要。agentxx 取其中**可自动化的部分**（M65 渲染快照、M81 契约检查、M84 基准门禁），不引入人工流程。 |
| R14 | **配置的「无兼容分支」硬规矩** | openclaw 敢这么做是因为它有 doctor 与更新器预检配套。agentxx 应逐步过渡：先做迁移骨架（M85），迁移能力成熟后再收紧「不保留兼容分支」这条。 |
| R15 | **`ask_user` 只在主会话可用这一限制** | 该限制在 openclaw 的编排模型下是必要的（子代理的结果要带回父会话）。agentxx 的子代理是独立 agent 且 HIL 能冒泡（§11.1），因此 M60 实现时可以让子代理也提问（经冒泡回父 IO），不必照搬这条限制。 |

---

## 21. 结语

通读两侧源码、测试与文档后，最值得留下的三条原则：

**一、把约束写成可执行的东西，而不是写成文档。**
openclaw 在这件事上走得最远：30 份 scoped `AGENTS.md`、自定义 oxlint 边界规则、按边界拆的 CodeQL、「schema 版本双端一致」「提示词快照漂移」「生成物一致性」三类 CI 校验、以及「测试成本必须落在预算内」这条明文规则。agentxx 的架构约束（分层、命名空间、插件不得引用内部头、能力表与渲染清单一致）目前主要活在文档与人的记忆里——M1/M2/M3/M81/M68 就是把这些搬进构建。

**二、状态与语义要显式，不要靠隐含前提。**
两侧都在这条上有过教训与对策：openclaw 用写者租约（`activeWriterRunId`）替代「大家小心不要并发写」、用投递四态替代「忙碌时排队」、用来源标记替代「记忆大致可信」、用幂等键替代「重试应该没事」；agentxx 已经把「会话是上下文唯一权威」「压缩即落盘」「worktree 绑定即隔离边界」做成了显式设计——继续沿着这条走，M7（排队状态机）、M21（写者租约）、M29（可用性 vs 授权）、M50（权威复验）都是同一个方向。

**三、分清「能力」与「代价」的边界。**
openclaw 的 `VISION.md` 把这条写得很直白：核心每加一个工具、一行提示词、一个配置键，都会落到每个操作员的**每一次模型请求**上，所以核心的门槛最高；插件、技能、通道、应用不承担这种持续成本，因此鼓励在那里扩张。agentxx 的核心更小、扩展点更硬（C ABI），但同样需要这条纪律：能做成插件的（媒体、定时任务、记忆检索）就不进核心；进核心的每一项都必须能说清「它换来了什么、代价落在哪一次请求上」。反向清单（§20）里 15 条，几乎都可以用这条原则解释。

---

## 附录 A：关键文件与文档索引

| 模块 | agentxx | openclaw |
|---|---|---|
| 总览/架构 | `docs/zh-cn/design/index.md` | `docs/concepts/architecture.md`、`docs/agent-runtime-architecture.md`、根 `AGENTS.md`、`VISION.md` |
| 会话运行 | `agent/lib/include/agentxx/agent/base_agent.h`、`agent/lib/include/agentxx/agent/io/session_server_agent_io.h`、`agent/lib/src/agent/io/session_server_agent_io.cpp` | `docs/concepts/agent-loop.md`、`docs/concepts/queue.md`、`src/agents/embedded-agent-runner/run/lane-controller.ts`、`packages/agent-core/src/stream-steering.ts` |
| 上下文/压缩 | `agent/lib/include/agentxx/middlewares/summarization.h`、`agent/lib/include/agentxx/nodes/session_context.h`、`agent/lib/src/agent/context.cpp`、`agent/lib/include/agentxx/agent/prompt.h` | `docs/concepts/context-engine.md`、`docs/concepts/compaction.md`、`docs/concepts/system-prompt.md`、`docs/reference/prompt-caching.md` |
| 工作区/隔离 | `agent/lib/include/agentxx/tools/git_worktree.h`、`agent/lib/src/agent/context.cpp`（`resolveWorkDir`） | `docs/concepts/agent-workspace.md`、`docs/concepts/managed-worktrees.md`、`docs/nodes/index.md` |
| 持久化 | `agent/lib/include/agentxx/agent/session_store.h`、`agent/lib/include/agentxx/agent/checkpoint_store.h`、`agent/lib/include/agentxx/util/sqlite.h` | `docs/reference/database-schemas.md`、`docs/concepts/session-state.md`、`src/state/` |
| 工具 | `agent/lib/include/agentxx/tools/tool.h`、`agent/lib/include/agentxx/nodes/toolcall.h`、`agent/lib/src/nodes/toolcall.cpp` | `docs/tools/index.md`、`src/agents/agent-tools.policy.ts`、`src/agents/tool-result-limits.ts`、`docs/tools/exec-approvals.md`、`docs/tools/loop-detection.md` |
| 提示词/技能/记忆 | `agent/lib/include/agentxx/middlewares/{skill,memory_file}.h`、`agent/lib/include/agentxx/agent/prompt.h` | `docs/tools/skills.md`、`docs/concepts/memory-architecture.md`、`docs/concepts/dreaming.md` |
| LLM 层 | `agent/lib/include/agentxx/agent/model_registry.h`、`agent/lib/src/protocol/{openai,anthropic}_provider.cpp`、`agent/lib/include/agentxx/nodes/modelcall.h` | `src/llm/`、`packages/ai/`、`docs/concepts/model-failover.md`、`docs/reference/prompt-caching.md` |
| 权限/安全 | `agent/lib/include/agentxx/middlewares/permission.h` | `docs/tools/exec-approvals.md`、`SECURITY.md`、`security/THREAT-MODEL-ATLAS.md` |
| 子代理/并行 | `agent/lib/include/agentxx/agent/agent_host.h`、`agent/lib/include/agentxx/tools/subagent.h` | `docs/tools/subagents.md`（7 篇子页）、`docs/tools/loop-detection.md` |
| 附件/媒体/询问 | `agent/lib/include/agentxx/agent/conversation_types.h`（`MediaAttachment`）、`agent/lib/include/agentxx/middlewares/interrupt_ui.h` | `docs/reference/rich-output-protocol.md`、`docs/tools/ask-user.md`、`docs/tools/media-overview.md` |
| 客户端/UI | `docs/zh-cn/design/{ui-layer,tui}.md`、`agent/client/.../io/tui/ui_components.cpp` | `docs/concepts/streaming.md`、`src/tui/AGENTS.md`、`ui/AGENTS.md` |
| 协议/SDK | `agent/lib/include/agentxx/agent/io/wire_protocol.h`、`agent/lib/src/ffi/` | `packages/gateway-protocol/`、`packages/sdk/`、`packages/gateway-client/`、`docs/gateway/embedding.md` |
| 插件 | `docs/zh-cn/design/plugins.md`、`agent/lib/include/agentxx/plugin/api/*`、`agent/third_party/cxx_pluginxx/` | `docs/plugins/architecture.md`、`docs/plugins/manifest.md`、`src/plugins/AGENTS.md`、`src/plugin-sdk/AGENTS.md` |
| 测试/门禁 | `agent/test/`、`agent/benchmark/`、`docs/zh-cn/design/benchmark.md` | `test/vitest/`、`config/oxlint/boundary-guards.json`、`.github/codeql/`、`docs/help/testing/` |
| 配置 | `agent/client/src/config_loader.cpp`、`agent/lib/include/agentxx/agent/config.h` | `docs/gateway/configuration*.md`、`docs/gateway/doctor/config-migrations.md` |
| openclaw 独有域 | — | `docs/channels/index.md`、`src/channels/AGENTS.md`、`docs/automation/*`、`docs/concepts/{session,multi-user,standing-intents,dreaming}.md` |

## 附录 B：术语对照

| 概念 | agentxx | openclaw |
|---|---|---|
| 一次对话轮 | 轮次（`runTurnAsync`） | run / turn（`agent` RPC 返回 `runId`） |
| 会话标识 | `sessionId` | `sessionKey` / `sessionId`（`agent:<agentId>:<kind>:<id>`） |
| 会话串行 | 会话控制器（`SessionServerAgentIO` 单协程） | 车道 lane（`session:<key>`，并发 1） |
| 同轮注入用户消息 | （无） | steer |
| 排队等待下一轮 | 消息队列（`messageQueue_`） | followup |
| 合并多条为一次 | （无） | collect |
| 打断并跑最新 | `InterruptAndRunNext` | interrupt |
| 上下文 | `Session` 的 LLM 上下文 | transcript + 上下文引擎 |
| 上下文装配器 | （固定流水线，无替换点） | context engine（`plugins.slots.contextEngine`） |
| 压缩 | `SummarizationMiddlewareHandle` | compaction（+ 上下文引擎的 `compact`） |
| 提示词附加段 | `appendSystemPrompts`（按 key 拼接） | 提示词分层段（缓存边界上下） |
| 工具基础能力 | `XXToolBase` 四开关 | tool policy + hooks + 结果守卫 |
| 权限询问 | 中断（`PermissionMiddlewareHandle::requestPermission`） | 审批（exec approvals）+ 操作员角色 |
| 工作目录隔离 | worktree 绑定（`SessionFsIsolation`） | 工作区 / 沙箱工作区 / managed worktree |
| 子代理 | `AgentHost::spawnBatch`（独立 agent） | `sessions_spawn`（独立会话） |
| 大内容暂存 | `share_store` | 大输出落盘 + 预览 |
| 插件 | C ABI 动态库 + 接口表 | code plugin / bundle plugin |
| 插件清单 | `plugin.yaml` | `openclaw.plugin.json` + `package.json` 的 `openclaw` 字段 |
| 插件槽位 | （无，能力叠加） | `plugins.slots.*`（独占替换） |
| UI 描述 | `agentxx.ui.item`（声明式组件） | 各前端自行渲染 + 富输出协议 |
| 客户端能力 | `uiCapabilitiesJson()` | `client-caps.ts` / 连接能力契约 |
| 远端连接 | Wire JSON + WS / 进程内 Channel | Gateway WS（`req`/`res`/`event`） |
| 运行器世代 | （无） | model runtime generation（快照） |
| 事件重放 | delta 环形缓冲（`seq`） | 事件不重放；SDK 侧 `run-event-replay` |

## 附录 C：本次精读的源码与文档清单

**agentxx**

- 设计与规则：`AGENTS.md`、`docs/zh-cn/design/{index,plugins,tui,ui-layer,benchmark,ffi}.md`、`.agentxx/memory/self.md`
- agentxx 库：`agent/lib/include/agentxx/agent/{base_agent,context,config,model_registry,prompt,session_store,checkpoint_store,agent_host,agent_runner}.h`、`agent/lib/include/agentxx/agent/io/{wire_protocol,session_server_agent_io}.h`、`agent/lib/include/agentxx/nodes/{modelcall,toolcall,wrap_handle,session_context}.h`、`agent/lib/include/agentxx/middlewares/{summarization,permission,skill,memory_file,subagent_manager,interrupt_ui}.h`、`agent/lib/include/agentxx/tools/{tool,subagent,tool_skill_search,git_worktree,share_store}.h`、`agent/lib/include/agentxx/plugin/{plugin_interfaces.h,api/plugin_api.h,api/client_plugin_api.h}`
- agentxx 实现（**第 2 版新增**）：`agent/lib/src/agent/agent_runner.cpp`（全文）、`agent/lib/src/agent/base_agent.cpp::runTurnAsync`、`agent/lib/src/nodes/modelcall.cpp`（`build_params` / `callLLM` / `repairMessages` / 重试循环）、`agent/lib/src/middlewares/summarization.cpp`（`countTokens` / `splitRecentByTokenBudget` / `hardTruncate` / `onModelcallRunFunc` 全流程）、`agent/lib/src/middlewares/permission.cpp`（`checkToolPermission` / `decideTarget` / `decidePaths` / `requestPermission`）、`agent/lib/src/nodes/toolcall.cpp::execTool`、`agent/lib/src/agent/io/session_server_agent_io.cpp`（`run` / `handleHello` / 队列入出队）、`agent/lib/src/tools/share_store.cpp`、`agent/lib/src/plugins/plugin_manager_lifecycle.cpp`、`agent/third_party/cxx_pluginxx/include/pluginxx/host/lifecycle.h`（1310 行，重点：`loadNative/loadBuiltin/loadPlugin`、`detachInstanceRegistrations`、`disableImpl/enableImpl`、`stopForDisable`、`shutdownAll/shutdownAsync`）、`agent/third_party/neograph/include/neograph/graph/run_context.h`（`record_usage`）与 `neograph/types.h`（`ChatCompletion::Usage`、`UsageAccumulator`）
- 客户端与工具链：`agent/client/src/config_loader.cpp`、`agent/client/src/io/tui/*`（目录结构）、`agent/test/test.cpp`、`agent/script/*`（构建脚本说明）

**openclaw**

- 规则与愿景：根 `AGENTS.md`、`VISION.md`、`README.md`（部分）、`src/agents/AGENTS.md`、`src/plugins/AGENTS.md`、`src/channels/AGENTS.md`、`src/tui/AGENTS.md`
- 概念文档：`docs/concepts/{architecture,agent-loop,context-engine,queue,system-prompt,compaction,session,session-state,agent-workspace,memory-architecture,model-failover}.md`
- 参考文档：`docs/reference/{database-schemas,prompt-caching,rich-output-protocol}.md`、`docs/tools/{index,skills,subagents,loop-detection,exec-approvals,ask-user}.md`、`docs/gateway/embedding.md`、`docs/nodes/index.md`、`docs/automation/standing-orders.md`、`docs/plugins/{architecture,manifest}.md`、`docs/docs_map.md`、`docs/agent-runtime-architecture.md`
- 源码（**第 2 版新增**）：`packages/agent-core/src/agent-loop.ts`（`runLoop` / `commitPendingMessages` / `stopIfAborted` / `launchParallelToolCalls` / `validateToolCallForBatchAdmission` / `shouldTerminateToolBatch`）、`packages/agent-core/src/{stream-steering.ts,agent.ts,types.ts}`、`packages/sdk/src/{event-hub.ts,run-event-replay.ts,run-event-stream.ts,replay-scope.ts}`、`src/agents/embedded-agent-runner/run/{session-bootstrap.ts,lane-controller.ts}`、`src/agents/embedded-agent-runner/tool-result-truncation.ts`、`src/agents/{agent-tools.policy.ts,tool-result-limits.ts,tool-policy.ts}`、`src/agents/tools/ask-user-tool.ts`、`src/agents/embedded-agent-runner/run/`（文件清单）、`src/agents/embedded-agent-runner/`（文件清单）
- 工程配置：`package.json`、`pnpm-workspace.yaml`、`vitest.config.ts`、`test/vitest/`（配置清单）、`config/oxlint/boundary-guards.json`、`.github/codeql/`（配置清单）

## 附录 D：可以继续深入的清单

| 未精读的位置 | 能回答的问题 |
|---|---|
| `src/agents/embedded-agent-runner/run/` 的 `attempt-*.ts`（约 200 文件） | 一次 attempt 的完整阶段机（准备/提示词/流/收尾/恢复）如何切分、哪些阶段可重入、哪些状态跨阶段携带。本轮只读了 `attempt.ts` 的导出面与 `session-bootstrap.ts` 的写者租约，其余仍未展开 |
| `src/gateway/server-methods/`（971 文件） | 上百个 RPC 方法如何按「方法表 + 校验器 + 授权」三层组织；错误码与幂等键的具体落点 |
| `src/state/`（470 文件） | 写事务、worker 访问（read-only scope / writer broker）、lease、`data_version` 探测的具体实现；database preflight 与迁移细节。本轮只读了写者租约的入口函数 |
| `packages/sdk/` 的 `run-event-stream.ts` / `transport.ts` | durable 运行事件流的对外形态与传输重连的具体状态机（本轮读了 `event-hub.ts` / `run-event-replay.ts` / `replay-scope.ts`） |
| `src/agents/subagents/`（356 文件） | announce 链、swarm 收集器、线程绑定会话、恢复与并发的完整实现 |
| `packages/ai/`（186 文件）与 `src/llm/providers/` | provider 适配层如何抽象各家协议差异（重放策略、工具兼容、流包装）；agentxx 侧对应的是 `protocol/{openai,anthropic}_provider.cpp` 的请求组装细节 |
| `ui/`（4440 文件）与 `apps/*` | 多前端共享组件与生成式 Swift 模型的实际边界；UI e2e 与截图门禁的落地方式 |
| `extensions/` 挑选若干（如 `codex`、`browser`、`memory-core`） | 「能力 provider 化」在具体插件里的形态；插件契约的边界感 |
| agentxx：`agent/lib/src/agent/context.cpp` 的 `buildSystemPrompt` 全文、`agent/lib/src/events/event_stream.cpp`（`EventBridge`） | 系统提示词的逐段拼装顺序与动态段来源；事件到 WireDelta / 持久化的完整映射（本轮读了调用点与该模块的注释面，未逐行读实现） |
| agentxx：`agent/client/src/io/tui/` 的 `ui_components.cpp`、`framework/` | 渲染与命中、尺寸换算、表单状态的实际实现（本文按文档与接口描述，未逐行读实现） |
| agentxx：`agent/lib/src/agent/agent_host.cpp` 全文 | 子代理派生的完整流程（配置派生、工具白名单解析、取消级联的实现细节）与 A2A 桥接的消息路径 |
