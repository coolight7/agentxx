# agentxx 与 pi 架构对比

> 对比对象
> - `agentxx`: `D:\0Acoolight\Program\cpp\agentxx`（C++23，Boost.Asio 协程 + NeoGraph 图引擎 + C ABI 插件，单 `io_context` 内多会话交错）
> - `pi`: `D:\0Acoolight\Program\js\pi`（TypeScript/Node，npm workspace 多包；`packages/agent` 为 agent 运行时 + 持久化 harness，`packages/ai` 为多 provider LLM 层，`packages/coding-agent` 为 CLI/TUI，另有 `protocol`/`server`/`client`/`durable`/`chord`/`telemetry`/`tui` 等独立包）
>
> 本文按模块通读两侧源码、测试与文档后逐模块记录差异、优缺点与**可迁移到 agentxx 的设计**。
> 阅读顺序建议：先看[§0 总览](#0-总览与对比方法)与[§17 迁移建议汇总](#17-迁移建议汇总)，再按模块展开。
>
> **编写说明**
> - 全文统一按「现状 → 对比（优缺点）→ 可迁移到 agentxx 的设计」三段结构组织；每个模块的结论都给出源码依据，依据清单见[附录 C](#附录-c本次精读的源码清单与结论依据)。
> - 只写**能在 agentxx 现有架构上落地**的迁移项，并对每项标注落地位置与相对优先级（[§17](#17-迁移建议汇总)）；反向结论见[§18](#18-反向清单agentxx-不必照搬的设计)。
> - pi 是「设计文档先行」的项目：`packages/agent/docs/` 下有成体系的设计/交接文档（harness.md 211 KB、pico 系列、work-packages 系列），文档与源码存在漂移，凡冲突处以源码为准，并在文中标出漂移点。
> - 文中数值（文件数、代码行数、测试数量、结构体字段数）均为本次在两侧仓库实测算得，统计口径写在该段落内。
>
> **阅读提示：pi 有三代运行时代码并存**，读下文时请先区分当前所讲的是哪一代（细节见[§1.1](#11-pi11-个单一职责包--三代运行时并存)）：
> - 第 1 代（主路径，CLI/TUI/SDK 全部在用）：`packages/agent/src/agent.ts` + `agent-loop.ts` + `coding-agent/src/core/agent-session.ts`；
> - 第 2 代（设计完整、仅实验路径在用）：`packages/agent/src/harness/**` 的 `AgentHarness` 持久化操作状态机；
> - 第 3 代（在研，包内未导出）：`packages/agent/src/harness/pico3/**`，规格见 `docs/pico-v3.md`；另有旁支 `packages/durable`（当前无人依赖）。
> 因此「pi 有/没有某能力」的说法在本文里都尽量落到具体某一代，避免把设计稿当成现有能力。
>
> **修订记录**
> - **第 1 版（2026-09，分模块通读）**：按 16 个模块通读两侧源码/测试/文档并逐模块落盘，正文结构为「现状 → 对比 → 可迁移」；迁移项按 P0/P1/P2 归档到 §17，反向结论归档到 §18。本版主要发现与结论：
>   1. **pi 三代运行时并存**（§1.1、§2.2、§10.1）：主路径仍是第 1 代低层循环；持久化 `AgentHarness` 只被实验性 session-worker 使用；`packages/durable` 无人依赖；`pico3` 未导出。文档中标注的 8 类缺口（`harness.md §0.9`：J1/C1/R12/T1/S3/R11/WP08/H1）在本文引用时都明确区分了「规格 vs 实现」。
>   2. **agentxx 的两处明确能力缺口**：单轮多工具调用串行执行（源码 `// TODO: 真正并行`），以及缺少用量账本（成本统计散落）。两者都在 §5/§4 给出落地点，列入 §17 的 P0。
>   3. **压缩语义差异**（§3、§8）：pi 的压缩是"追加一条自包含检查点条目、原条目永不删除"，agentxx 是"就地替换上下文"。本文给出的迁移方向是"保留尾部 + 原文可回取"，而不是照搬 entry 树。
>   4. **权限取向不同且各有正当性**（§9）：agentxx 有声明式权限体系（含逐路径三态复核、完全授权、worktree 写边界），pi 明确不提供沙箱（`SECURITY.md` 列为 Out Of Scope）并交给容器化；本文建议补齐的是"边界文档 + 符号链接 + 项目信任"，而不是取消权限层。
>   5. **扩展机制两条路线**（§14）：pi 走进程内 TS 扩展 + chord（facet/服务/复制状态），agentxx 走 C ABI 插件 + 29 张表 + 图可编程。本文取长补短：借鉴依赖声明与生命周期事件，保留 ABI 边界与能力协商。
>   6. **测试体系互补**（§15）：pi 的强项是一致性框架与可控竞态（含写顺序断言），agentxx 的强项是资源基准（smaps 模块级分解、堆碎片、基线对比）；两者应各自补齐短板。

## 目录

| 章节 | 内容 | 一句话结论 |
|---|---|---|
| [0](#0-总览与对比方法) | 总览与对比方法 | 两项目定位、规模、判据与结论速览 |
| [1](#1-总体架构与包分层) | 总体架构与包分层 | pi 用「小核心 + 多个单一职责包」表达边界；agentxx 用「一个核心库 + 端点抽象」表达边界。可借鉴的是**把持久化/压缩/协议/UI 从核心库拆出去**的纪律 |
| [2](#2-agent-循环轮次与驱动模型) | Agent 循环、轮次与驱动模型 | pi 把「一轮」拆成**可持久化的操作状态机 + drive 原语**（accept/drive/requestAbort/inspectExecution），agentxx 是「一次 run_stream + 中断恢复循环」；可借鉴的是**操作状态机与投递语义显式化** |
| [3](#3-会话分支树与-llm-上下文) | 会话、分支树与 LLM 上下文 | pi 的 entry 树 + 分支 + 车道把「历史 / 上下文 / 并行工作」三者解耦；agentxx 的「会话即上下文权威」方向一致但缺分支与多车道 |
| [4](#4-持久化事务与崩溃恢复) | 持久化、事务与崩溃恢复 | pi 用**绑定地址 + 原子事务 + 总状态替换**换取可恢复性；agentxx 用「整表替换 + 节流落盘」，缺 intent/结算边界与格式版本 |
| [5](#5-工具系统) | 工具系统 | pi 的 effect gate + intent/outcome 两段提交 + 工具自选进度检查点值得借鉴；agentxx 的插件声明权限与重复调用确认是 pi 完全没有的能力 |
| [6](#6-系统提示词技能与请求组装) | 系统提示词、技能与请求组装 | pi 把提示词**建成带版本的结构化片段**并把工具集变化写进转录；agentxx 是字符串拼装 + 追加段 |
| [7](#7-llm-流式多-provider-与鉴权) | LLM 流式、多 provider 与鉴权 | pi 的 provider 目录生成化 + 能力元数据 + OAuth 体系是明显强项；agentxx 只有 3 个 provider 且鉴权仅 API Key |
| [8](#8-上下文压缩与分支摘要) | 上下文压缩与分支摘要 | pi 把压缩/摘要都变成**持久化的 operation**，压缩后仍保留 `retainedTail`；agentxx 是阈值触发 + 子代理摘要 + 硬截断兜底 |
| [9](#9-权限沙箱与安全边界) | 权限、沙箱与安全边界 | pi 明确「不内置权限系统」，改用项目信任 + 容器化文档 + 扩展拦门；agentxx 的权限中间件 + 插件声明式权限更强，但缺真实沙箱 |
| [10](#10-子代理任务与后台工作) | 子代理、任务与后台工作 | pi 有任务/依赖/所有权/取消的一整套抽象与 chord 服务层；agentxx 是「中断即委派 + 批量并发」 |
| [11](#11-会话检索遥测附件与大输出) | 会话检索、遥测、附件与大输出 | pi 的厂商中立 telemetry 契约 + 一致性测试、工具输出截断策略可低成本移植；检索仍是设计稿 |
| [12](#12-客户端-ui-与渲染分层) | 客户端 UI 与渲染分层 | pi 的 TUI 是独立可复用包（差分渲染 + 自研布局），组件模型是命令式对象树；agentxx 走「服务端产出声明式 JSON → 客户端通用渲染」 |
| [13](#13-远程协议sdk-与外部集成) | 远程协议、SDK 与外部集成 | pi 有两条远程面（CBOR RPC 服务 + JSONL RPC 模式）与 TS SDK；agentxx 有 WS wire + MCP/ACP/A2A，但无独立 SDK 与请求级取消协议 |
| [14](#14-扩展机制对比) | 扩展机制（TS extensions / chord vs C ABI 插件） | 两者都做「进程内可执行扩展」，agentxx 的 C ABI 在跨语言/版本隔离上更强，pi 的 API 形状在**声明式挂载点与资源生命周期**上更细 |
| [15](#15-测试与质量门禁) | 测试与质量门禁 | pi 有一致性测试框架（同一语义多后端跑同一套断言）+ 假 provider；agentxx 有自研多模块测试 + 资源基准 |
| [16](#16-配置设置与装配) | 配置、设置与装配 | pi 是「分层设置 + 项目信任 + 迁移脚本 + 包管理器」；agentxx 是「base/overlay 两段 yaml + 设置 KV」 |
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

| 维度 | agentxx | pi |
|---|---|---|
| 语言/运行时 | C++23；Boost.Asio 协程，单 `io_context` 内多会话协程交错，无锁 | TypeScript（Node ≥ 22 / Bun，`erasableSyntaxOnly` 无 emit 语法）；Node 事件循环 + Promise/AsyncGenerator，`AsyncLocalStorage` 未用于核心路径 |
| 顶层结构 | 3 层：`agent/lib`（libagentxx）、`agent/client`（CLI/TUI）、`agent/plugins`（内置能力插件 21 目录，其中 15 个真插件） | 11 个 npm workspace 包：`agent`（运行时+持久化）、`ai`（provider）、`coding-agent`（CLI/TUI/模式）、`tui`（终端 UI 库）、`protocol`、`server`、`client`、`durable`、`chord`（应用组合/服务/插件）、`telemetry`、`evals` |
| 编排核心 | NeoGraph 图引擎（节点 + 条件边 + `Command.goto_node`），中间件包裹节点 | 两层：`Agent`（低层事件循环，无持久化）+ `AgentHarness`（**操作状态机 + 车道 + 持久化**，§2/§4） |
| 会话与历史 | `Session` 持有 typed LLM 上下文（唯一权威）+ 展示历史（`ViewMessage`）+ SQLite 落库 | 会话 = 不可变 **entry 树** + 绑定地址上的可变 **值/列表** + **用量账本**；持久化后端 Memory/JSONL/SQLite 三选一 |
| 上下文压缩 | 阈值触发 + 工具输出压缩 + 子代理摘要 + `hardTruncate` 兜底 | 压缩与导航摘要都是持久化 operation；压缩条目保留 `retainedTail`，原条目永不删除 |
| 工具系统 | `XXToolBase`（图工具桥接）+ `ToolcallWrapNode` + 中间件 + 插件声明权限 | `AgentHarnessTool` + effect gate + intent/outcome 两段提交 + 工具自选进度检查点 |
| 权限与执行 | 权限中间件（白/黑名单 + 工作目录隔离 + 完全授权）+ 插件声明权限目标 + worktree 隔离 | **不内置权限系统**（README/security.md 明确声明）：项目信任只控制资源加载；隔离靠容器化文档与扩展拦门 |
| 扩展形态 | 纯 C ABI 动态库插件（19 张 agent 表 + 9 张 client 表），可跨语言、可禁用/卸载 | 进程内 TypeScript 扩展（jiti 运行时加载）+ skills（Markdown）+ prompt templates + themes + npm/git 包分发；另有 `chord` 的 facet/服务/包机制 |
| 客户端 | FTXUI TUI、stdio CLI、远程 WS 客户端；服务端产出声明式 UI JSON（`agentxx.ui.item`） | 自研 TUI 库（差分渲染 + 布局内核）+ `packages/tui` 可复用；TUI 直接调用 agent（同进程），远程另有 `protocol`/`server`/`client` 三包 |
| 远程/集成 | WS wire 协议 + MCP client/server + ACP server + A2A server/client + FFI（`docs/zh-cn/design/ffi.md`） | CBOR 长度前缀 RPC（`protocol`+`server`+`client`，含 attachment 栅栏）+ JSONL RPC 模式（子进程，89 个命令/事件）+ TS SDK + 实验性 mini/worker 架构 |
| 测试 | 自研 `agentxx_test`（约 80 模块）+ `agentxx_benchmark` 资源基准 | 675 个测试文件 / 154 k 行；vitest + node:test 双跑法；一致性测试框架（同一语义对 memory/jsonl/sqlite 三后端跑同一套断言）+ faux provider 假模型 + `evals` 包 |
| 文档 | 22 个 md / 735 KB（design/index 165 KB、plugins 102 KB、tui 43 KB、benchmark 43 KB、ffi 24 KB），中英双语 | 152 个 md / 3.67 MB（`packages/agent/docs` 含 harness.md 211 KB、pico 系列、work-packages 9 篇；`coding-agent/docs` 40 篇用户文档；各包 CHANGELOG 严格规范） |
| 代码规模（本次实测，仅 `src`，不含测试） | lib+client+plugins 258 文件 / 100.5 k 行 | 724 文件 / 160 k 行（agent 117/31.3 k、ai 189/23.0 k、coding-agent 278/68.7 k、tui 44/16.9 k、durable 29/8.4 k、chord 29/7.9 k、server 16/1.8 k、client 8/1.0 k、protocol 8/0.8 k、telemetry 6/0.8 k） |

### 0.2 对比方法与判据

- 每个模块都用**三方证据**核实：源码（真实实现）、测试（行为边界）、文档（设计意图）。凡文档与源码冲突处，以源码为准。
- 「优点/缺点」的判据只有两条：
  1. **后续改动成本**：扩展点是否收敛、约束是否显式、有没有「只有作者知道」的隐含前提；
  2. **运行期正确性**：生命周期、取消、恢复、并发与资源回收是否可控。
- 迁移建议只写**能在 agentxx 现有架构上实现**的项，并标注落地位置（哪个文件/哪一层）与代价；不能落地的不写。
- pi 侧大量设计处于「规格已写、实现未完成」状态（`harness.md §0.9` 逐条列出 J1/C1/R12/T1/S3/R11/WP08/H1 等缺口）。本文在引用这些设计时会明确标注「规格 vs 实现」，避免把设计稿当成现有能力。

### 0.3 结论速览

| 模块 | agentxx 现状 | pi 现状 | 更值得借鉴的方向 |
|---|---|---|---|
| 包分层 | 核心库 + 客户端 + 插件三层，客户端与 agent 同库 | 11 个包，持久化/协议/UI/遥测各自独立，`agent` 明确「不承载具体产品逻辑」 | 把持久化、压缩、协议、UI 组件层拆成可独立测试的边界 |
| 轮次语义 | `runTurnAsync` → `run_stream_async` → 中断处理循环 | 操作状态机（13 个叶子状态）+ `accept`/`drive`/`requestAbort` 四原语 | 操作状态与投递结果显式化、可恢复的 drive |
| 上下文 | 会话即权威（typed 上下文 + 版本号），无分支 | entry 树 + 分支 + 车道，上下文由「分支 + 投影」导出 | 分支与「上下文选择」分离、自定义条目投影 |
| 持久化 | 单库 SQLite，整表替换上下文 + 节流落盘 | 绑定地址 + 原子事务 + 每种操作的总状态替换 + intent/结算 | 格式版本、写者所有权、崩溃配平与重放 |
| 工具 | 图节点分发 + 中间件 + 重试 + 重复调用确认 + 输出压缩 | 三段式（prepare/execute/finalize）+ effect gate + 并行声明 + 进度检查点 | 结果顺序与完成顺序分离、工具自选检查点 |
| 提示词 | 字符串拼装 + 追加段 + 记忆文件 + skill 中间件 | 结构化片段 + 工具集变化写进转录 + 按需加载 skill | 提示词片段化与「工具集变化可见性」 |
| LLM 层 | 3 个 provider + 注册表 + 限流装饰器 | 60+ provider 目录（生成式）+ 能力元数据 + OAuth 全家桶 + 假 provider | 能力元数据、鉴权分层、假 provider |
| 压缩 | 阈值触发 + 子代理摘要 + 去重 + 硬截断 | 压缩/摘要为持久化 operation，保留尾部消息与 usage | 保留尾部、压缩可恢复、跨分支摘要 |
| 权限 | 中间件统一判定 + 插件声明目标 + 完全授权 | 无内置权限；项目信任 + 容器化 + 扩展拦门 | 项目信任（单一决策点）与容器化文档 |
| 子代理 | 中断即委派 + 批量并发 + 事件总线 | 任务/依赖/所有权/取消 + chord 服务层 + 子代理示例扩展 | 任务所有权链接、依赖语义 = 终态而非成功 |
| 检索/遥测 | share store + RAG 插件；无遥测 | 厂商中立 telemetry 契约 + 一致性套件；检索仍是设计稿 | telemetry 契约与一致性测试 |
| 客户端 | 声明式 UI JSON + 客户端通用渲染 + 客户端插件 | 独立 TUI 库（差分渲染/布局/图片/搜索）+ 命令式组件树 | 差分渲染与滚动区命中、可复用 UI 库边界 |
| 协议 | WS wire（手写结构体 + toJson/fromJson） | CBOR RPC（类型 schema 校验 + 连接分阶段 + attachment 栅栏）+ JSONL RPC | schema 校验边界、请求级取消、连接阶段机 |
| 扩展 | C ABI 插件（能力表 + 权限声明 + 事件 + 图节点） | 进程内 TS 扩展（事件/工具/命令/UI/provider）+ chord facet/服务 | 声明式挂载点、扩展生命周期事件、资源作用域 |
| 测试 | 自研多模块测试 + 资源基准 | vitest + 一致性框架 + 假 provider + 文档一致性 eval | 后端一致性套件、假 provider、录制式回归 |
| 配置 | base/overlay 两段 yaml + 设置 KV | 分层设置 + 项目信任 + 迁移脚本 + 包管理器 | 迁移脚本与信任决策点 |

---

## 1. 总体架构与包分层

### 1.1 pi：11 个单一职责包 + 三代运行时并存

**包依赖方向**（取自各 `packages/*/package.json` 实测）：

```
telemetry（零依赖）        tui（仅 get-east-asian-width / marked）
      ↑                        ↑
    chord（仅 esbuild，用于 facet 打包）       │
   ┌──┴────────┬───────────┐                │
protocol      durable    agent ──→ ai ──────┤
   ↑            │          ↑                │
 server ────────┘          │                │
   ↑                       │                │
 client ───────────────────┴──→ coding-agent ──→ tui
```

- 底层可复用件：`telemetry`（零运行期依赖，只定义契约与一致性测试）、`tui`（自研终端渲染库，依赖两个纯工具库）、`chord`（应用组合运行时：服务/复制状态/RPC/插件）。
- 领域层：`ai`（多 provider LLM + 鉴权）、`agent`（低层循环 + 持久化 harness + 压缩 + 工具）、`durable`（另一套持久会话运行时，当前**无人依赖**）。
- 服务/协议层：`protocol`（CBOR + TypeBox schema）、`server`（连接/会话路由）、`client`（RPC 客户端）。
- 产品层：`coding-agent`（CLI/TUI/打印/JSON/RPC 四种模式 + 扩展运行时 + 设置/包管理）。

**三代 agent 运行时并存（本次精读最重要的发现之一）**：

| 代际 | 位置 | 组成 | 当前谁在用 |
|---|---|---|---|
| 第 1 代：低层循环 | `agent/src/agent.ts` + `agent-loop.ts` | `Agent`（事件流 + steering/follow-up 队列）+ `runAgentLoop`；无持久化 | 主路径：`coding-agent/src/core/agent-session.ts`（3636 行）通过 SDK/Prompt/RPC/打印模式全部走这条 |
| 第 2 代：持久化 harness | `agent/src/harness/**`（`agent-harness.ts` + `runtime/**` + `session/**` + `compaction/**`） | `AgentHarness`（操作状态机 + 车道 + 分支树 + 绑定值/列表 + 事务存储） | 仅 `coding-agent/src/experimental/session-worker.ts`（实验性 worker 架构） |
| 第 3 代：pico v3 | `agent/src/harness/pico3/**`（29 文件） | 任务/定义/依赖/所有权/调度（`kinds/{generation,tool,job,plugin,post-tools,collapse}.ts`） | 包内未导出给 CLI，属在研实现 |
| 旁支：durable 包 | `durable/src/**` | `Session` + `documents` + `transaction` + jsonl/sqlite/memory 三后端 | 无包依赖它，独立产品线 |

**产品层内部分层**（`coding-agent/src`，模式并列但不共享内核）：

```
cli.ts / main.ts           参数解析、配置、信任决策、迁移、模式分派
core/                      agent-session（会话门面）、session-manager、compaction、
                           extensions（扩展运行时）、settings-manager、resource-loader、
                           model-runtime/resolver、package-manager、tools/*（内置 9 个工具）
modes/interactive/         TUI（interactive-mode.ts 6250 行）+ components/*（50+ 组件）+ theme
modes/{print,json,rpc}/    无界面模式；rpc 的 rpc-client.ts 是子进程客户端实现
experimental/              worker / mini(server+worker+tui) / micro / 服务化实验
```

**边界纪律（有明文规则）**：仓库 `AGENTS.md` 规定「扩展只在 `packages/coding-agent` 里做」「不要为兼容旧形态牺牲类型」「禁止 `any`」「只使用可擦除 TS 语法」；`packages/coding-agent/src/config.ts` 是资源定位的唯一入口（禁止直接用 `__dirname`）；`models.generated.ts` 禁止手改，必须改生成脚本。这些约束让 11 个包在 160 k 行规模下仍保持单向依赖。

### 1.2 agentxx：三层库 + 端点抽象 + C ABI 插件

```
agent/third_party/cxx_utilxx_base   基础件（日志/JSON/字符串/取消令牌/异步卸载）
                 cxx_utilxx         重依赖工具（HTTP/WS/正则/差异/worktree/散列）
                 cxx_pluginxx       插件框架内核（C ABI 基座 + 十张通用表 + 生命周期）
                 cxx_pluginxx_ui    界面描述层（pluginxx::ui::Item/parse/adapt/plainText，带自带测试）
                        ↑
agent/lib  libagentxx：BaseAgent/CodeAgent、节点、中间件、工具、协议(MCP/ACP/A2A/OpenAI/Anthropic)、
           插件管理、会话存储、事件总线；依赖以上四库
                        ↑
agent/client    CLI/TUI/远程客户端 + 客户端插件管理；唯一渲染实现（ui_components.cpp）
agent/plugins   15 个内置能力插件（各自独立 CMake，声明支持平台）
```

- 客户端与 agent 通过 `AgentIOBase` 端点解耦：`sendToPeer()` 发送、`onPeerMessage()` 分发到 `onDelta` 等回调；进程内走 `ChannelIOTransport`，跨进程走 `WsIOTransport`。
- 插件面向 C ABI：agent 侧 19 张接口表（10 张通用 `pluginxx.*` + 9 张领域 `agentxx.agent.*`），client 侧 9 张；插件与宿主的共享代码以**静态链接**方式各自带入，符号经导出白名单隐藏。
- 会话执行入口：`BaseAgent::runTurnAsync`（`lib/src/agent/base_agent.cpp`）→ `AgentRunner::run` → `engine->run_stream_async()`；中断-恢复在 `AgentRunner` 的 `while (result->interrupted)` 循环中完成。
- 线程模型：单 `io_context` 内多会话协程交错；会话经 `bindIoThread/assertIoThread` 抓跨线程误用；重活（附件读盘/base64/压缩计算）经 `utilxx::offloadAsync` 卸载到线程池。
- 编译产物形态：独立可执行 / 动态库 / 静态库；平台覆盖 Linux(+WSL)、Windows 10+、macOS、Android 5+。

### 1.3 对比

| 维度 | agentxx | pi |
|---|---|---|
| 边界表达方式 | 「库 + 端点 + C ABI 表」：跨进程/跨语言边界是硬边界，库内模块边界靠目录约定 | 「npm 包」：每个包是独立可发布单元（有自己 CHANGELOG/README/构建配置），边界由包依赖显式表达 |
| 可复用件拆分 | 4 个自研库（3 个已独立发布维护 + 1 个 UI 描述层），插件复用其中 2 个 | `telemetry`（0 依赖）、`tui`（2 依赖）、`chord` 三个可被外部产品单独使用 |
| 客户端与核心耦合 | 客户端插件与 UI 渲染实现都在 `agent/client`，但渲染的**描述层**在 `cxx_pluginxx_ui`（与宿主无关） | TUI 是独立包，coding-agent 只是它的使用方；核心 `agent` 包不认识 TUI |
| 服务面 | 一个可执行文件同时是 TUI/CLI/服务端/ACP 端，服务端无守护进程管理 | 主 CLI 一体，另有 `protocol`+`server`+`client` 三包提供**独立进程/独立部署**的服务面，`experimental/mini` 还演示了「server + worker + tui 三进程」 |
| 运行时代际 | 单代（图引擎 + 中间件），升级靠插件表版本协商 | 三代并存（低层循环 / durable harness / pico3）且主路径仍在第 1 代；三代共享 `ai`/`telemetry`，但数据模型互不兼容 |
| 模块可测性 | 每个库/插件都可独立编译测试；`cxx_pluginxx_ui` 自带独立测试工程 | 每个包自带 vitest 配置，可单独 `vitest --run`；后端一致性套件跨三个后端复用 |

**优点/缺点**

- pi 的包拆分让「不该知道的事」在物理上无法知道：`tui` 不认识 agent，`telemetry` 不认识业务，`protocol` 只依赖 `chord` 的 JSON 类型。代价是三代运行时并存期间出现明显的重复建设（`agent/harness/session/jsonl` 与 `durable/storage/jsonl` 是两套 JSONL 持久化；`agent/harness/compaction` 与 `coding-agent/core/compaction` 是两套压缩实现），文档需要专门章节（`harness.md` §0.9、`post-wp05-roadmap.md`）来记录「哪些规格还没实现」。
- agentxx 的端点抽象比 pi 的「把 TUI 当客户端」更彻底（同一份 agent 二进制既可从 stdio 也可从 WS 驱动），缺点是**服务能力与产品能力仍在一个可执行文件里**：没有「无界面后台常驻服务 + 多客户端 attach」的部署形态（`agentxx_cli server` 存在，但缺 PID/控制 socket/连接鉴权/会话接管配套）。
- agentxx 的库内模块边界靠约定（`lib/src/*` 目录 + 文档），没有编译期隔离：`agent/lib` 同时含协议适配（MCP/ACP/A2A）、插件管理、会话存储、事件总线、压缩中间件。代价是改动一个模块的编译/链接影响面更大；收益是单次构建即可产出全部能力，部署简单。

### 1.4 可迁移到 agentxx 的设计

1. **「新能力不进核心库」的书面纪律**（P0，成本近零）：pi 用仓库级 `AGENTS.md` 明确「不要往 codex/agent 核心加代码，先找别的包」。agentxx 已有类似约定（AGENTS.md 的代码结构节），建议补一条可执行的判据：**`lib/src/agent` 只放会话生命周期/上下文/持久化骨架；新增能力优先落到 `middlewares/`、`nodes/`、`plugins/` 或新库**，并在 `docs/zh-cn/design/index.md` 里给出当前边界清单（现状是 `lib/src` 里协议、插件、训练、FFI 与 agent 骨架并列，新人难以判断该往哪放）。
2. **把「可独立测试的后端一致性」做成包级能力**（P1）：pi 的 `durable/testing/storage-conformance.ts`（1451 行）对 memory/jsonl/sqlite 三个后端跑同一套断言。agentxx 现在只有一个 SQLite 会话库，但 `share_store`/`settings_db`/`SessionStore` 都是「后端可替换」的位置，可以抽出 `conformance` 测试骨架（见 §15）。
3. **UI 描述层已经拆出去了，建议再拆渲染层**（P2）：`cxx_pluginxx_ui` 已与宿主无关，但渲染实现只有一份且绑 FTXUI（`client/src/io/tui/ui_components.cpp`）。若要支持 GUI 客户端，建议把「行模型 → 终端元素」的最后一步收敛成一个小接口（当前 `renderItem`/`measureItem` 已按此组织），而不是让 GUI 复用 FTXUI 元素类型。
4. **服务模式的部署化**（P1，与 §13 合并）：把 `agentxx_cli server` 扩成可后台常驻（PID/锁文件/控制通道/连接鉴权），允许第二个客户端进程 attach 已有会话——`SessionServerAgentIO` 已具备连接注册与增量重放能力，缺的是生命周期管理（见 §13.4）。

---

## 2. Agent 循环、轮次与驱动模型

### 2.1 pi 第 1 代：低层事件循环（主路径）

`packages/agent/src/agent-loop.ts`（815 行）+ `agent.ts`（547 行）是 CLI 实际使用的循环：

- **事件流**：`agentLoop()` / `agentLoopContinue()` 返回 `EventStream<AgentEvent, AgentMessage[]>`；事件类型为 `agent_start` / `turn_start` / `message_start|update|end` / `tool_execution_start|update|end` / `turn_end` / `agent_end`。
- **两层队列**：`steeringQueue`（"在当前 assistant 轮结束、工具跑完后注入"，用于插话）与 `followUpQueue`（"等 agent 本来要停了再注入"），各自有 `QueueMode = "all" | "one-at-a-time"`。轮内每次检查 `getSteeringMessages()`，外层 while 在"没有更多工具调用"时检查 `getFollowUpMessages()` 并继续。
- **四个钩子点**：`prepareNextTurn`（下一轮前替换 context/model/thinking，可追加消息）、`prepareRequest`（每次请求前，含第一次；不投递队列）、`finishTurn`（一轮结束、`turn_end` 前；可返回 `continue` 强制再请求一次，或 `end` 硬结束）、`beforeToolCall`/`afterToolCall`（工具准备/收尾，可 block、可改 args、可 patch 结果）。
- **工具执行**：`toolExecution: "sequential" | "parallel"`（默认 parallel），工具可用 `executionMode` 覆盖；parallel 下**准备阶段逐个串行**（校验 + hook）、**执行阶段并发**，`tool_execution_end` 按完成顺序发出，而 tool-result 消息按 assistant 源顺序在最后统一发出（源码注释明确写了两套顺序）。
- **截断保护**：`stopReason === "length"` 时该 assistant 消息里的**所有** tool call 直接判失败（`failToolCallsFromTruncatedMessage`），理由写得很清楚：流式参数有 salvage 解析，截断后的参数可能"能解析、能过校验，但内容是残缺的"。
- **失败路径**：`Agent.runWithLifecycle` 捕获异常后合成一条 `stopReason: "aborted" | "error"` 的 assistant 消息，并按 `message_start`/`message_end`/`turn_end`/`agent_end` 正常事件序列发出——错误也是一种"轮次结果"，而不是抛出给调用方。

### 2.2 pi 第 2 代：AgentHarness 的操作状态机（设计 + 部分实现）

`AgentHarness` 把"一轮"抽象为**可持久化的 operation**，对外只有四个原语（`harness.md §0.2` 与 `agent-harness.ts`）：

| 原语 | 语义 |
|---|---|
| `accept(request, context)` | **原子地创建**一个操作（run / compaction / navigation），写入 `pi.op.meta` + `pi.op.state=starting` + lane 当前操作 id；不启动任务、不产生副作用 |
| `drive({operationId}, context)` | 推进**指定**操作；返回 `{kind:"settled"} \| {kind:"waiting"(reason: retry·deferred)}`。一个 lane 同时只有一个活动 drive（`activeDrive`），重复 drive 会 attach 到同一 drive 上等待 |
| `requestAbort(operationId)` | **先持久化** `control.status = cancel_requested`，再向 effect gate 发取消信号（源码顺序是"先写状态、后触发"；`harness.md §4.6` 把历史上的反向实现列为契约债） |
| `inspectExecution()` | 原子读出当前操作与最近终态结果（`LaneExecutionInfo`） |

- **操作状态是"总状态"**：`OperationState` 是 13 个叶子的扁平联合（`starting` / `checkpoint` / `assistant.ready` / `assistant.effect_pending` / `assistant.retry_wait` / `tools` / `deferred.suspended` / `deferred.effect_pending` / `summary.deciding` / `summary.ready` / `summary.effect_pending` / `summary.retry_wait` / `navigation.ready_to_commit`），每次转移**整份替换**；恢复时从该状态直接进入对应过程（procedure），不重放日志、不靠"缺什么推断到哪"。
- **drive 主循环**（`runtime/drive.ts`）：`for(;;)` 按 `state.at` 派发到 procedure；procedure 返回 `settled` / `waiting` / `continue`；若一轮下来状态没变则抛 `SessionInvariantError`（"Drive procedure made no progress"），把"状态机卡住"变成显式错误。
- **队列也是持久的**：`LaneState.inbox: InboxItem[]` 与 `pi.pending.entry/{entryId}`，三种投递 `steer`（下一轮前）/ `followUp`（agent 将停时）/ `nextRun`（下一轮开始时）；条目先成 pending、再在事务里变成 entry。
- **取消 / 等待的显式表达**：`DriveOutcome` 的 `waiting` 带 `reason: "retry"`（`notBefore`）或 `"deferred"`（`DeferredHandle` + `poll`），由宿主决定何时再 `drive`；harness 明确不做调度（"never creates platform alarms, scans repositories…"）。
- **单写者模型**：一个 session 同时只能有一个可写 owner（宿主保证），lane 用"最多一个操作"表达并行工作；多路并行靠**多 lane 共享同一 entry 树**（同一历史、各自推进）。

### 2.3 agentxx：一轮 = 一次图运行 + 中断恢复循环

`runTurnAsync`（`lib/src/agent/base_agent.cpp`）的骨架：

```
getSessionAsync → bindIoThread/assertIoThread → 插件轮次边界(flushPendingCleanup/onTurnBegin)
→ 追加用户消息（ViewMessage 展示历史 + Session typed 上下文两处）
→ 发 TurnStart 增量（附件只回显元数据，不含 dataUrl）
→ 建 CancelToken → AgentRunner::run(...)
     └ engine->run_stream_async(cfg)     // 图：__start__ → agent_start → llm ⇄ tools → agent_end → __end__
     └ while (result->interrupted) { 处理 HIL / 子代理委派 → engine->resume_async(...) }
→ 轮末统计提示（模型名 · 时长 · 速度）+ 上下文落盘 + 展示历史落盘 + TurnEnd 增量
```

- **轮次语义落在图上**：`llm` 节点执行后按"本轮是否有 tool_calls"经 `Command.goto_node` 返回 `tools` 或 `agent_end`（`xx_autoRoute`，图定义里仍保留静态默认边兜底）；`tools → llm` 形成 ReAct 环。图条件 `xx_has_tool_calls`（读 `xx_messagesMeta` 影子通道，不读上下文本体）供自定义图/插件重写图时使用。
- **LLM 节点**（`nodes/modelcall.cpp::baseRun`）：每轮把系统提示词重建成上下文首条 system 消息（`buildSystemPrompt`，含 memory/skill/动态追加段）→ 跑中间件 `onModelcallRunFunc` → `repairMessages` 修正角色顺序 → 调 provider；失败时**保留部分输出**（≥512 字符即插入一条 `flags=AutoInserted` 的 assistant 兜底消息，避免悬挂 `tool_calls` 被误路由回 tools 节点重复执行），并按 `retry*3s`（限速错误再加 `retry*5s`）重试，且重试计数不因部分输出重置。
- **工具节点**（`nodes/toolcall.cpp`）：`baseRun` 先按消息链检测"连续相同调用"（key = `{tool}_{arglen}_{arghash}`，阈值 `toolcallRepeatCheckThreshold` 默认 5），命中且工具开启了 `repeatCallCheck` 时经权限总线发起一次**用户确认卡片**（确认/拒绝/终止）；`execTool` 负责参数类型自动修正、权限询问、执行、重试（`maxRetry`）、输出压缩/摘要、取消时补齐占位 tool 结果。
- **中断即协作**：HIL（权限询问 / 中断表单 / 子代理委派）由节点抛出 `NodeInterrupt`，`AgentRunner` 经事件总线 `Topic::Interrupt` / `Topic::Subagent` 请宿主处理，再把结果作为 `resumeValues` 写回并 `engine->resume_async` **回到中断节点**继续。中断期间把节点信息与待处理参数转存到图状态通道 `xx_savedGraphData`，使进程重启后能续上（`resumeInterrupt` 分支）。
- **排队消息在 IO 端点**：`SessionServerAgentIO::messageQueue_`（`std::deque<MessageQueueItem>`，配套 `queuePaused_` / `interruptAndRunNext` / `clearMessageQueue` / `removeQueueItem`）保存"轮次进行中收下的用户消息"，轮末驱动下一条；队列经 `sendMessageQueueUpdate()` 同步给客户端展示，**不进入会话数据、不落盘**。

### 2.4 对比

| 维度 | agentxx | pi（第 1 代主路径 / 第 2 代 harness） |
|---|---|---|
| 「一轮」的单位 | 一次 `engine->run_stream_async` + 中断恢复循环（内存态） | 第 1 代：`prompt()`/`continue()` 一次调用；第 2 代：**持久化 operation**（有 id、有 meta、有终态结果记录） |
| 循环控制 | 图边 + `Command.goto_node`；节点可被中间件包裹 | 第 1 代：内层（工具/steering）+ 外层（follow-up）双 while；第 2 代：13 叶子状态机按状态派发 procedure |
| 插话（steering） | 轮次进行中收下的消息进 `messageQueue_`，轮末统一处理 | 第 1 代：`steer()` 在**本轮工具执行完之后**注入；第 2 代：`steer/followUp/nextRun` 三种投递 + 持久 inbox + 队列模式 |
| 「还要不要再请求一次」 | 由 `llm` 节点返回的 `Command` 决定 | 第 1 代：`finishTurn` 的 `continue`/`end` + 队列与工具调用共同决定；第 2 代：`CheckpointData.continuation = need_assistant \| may_finish` |
| 取消 | `CancelToken`：入口埋点 + 抛 `CancelledException`；中断处理完成、resume 前再检查一次（防止"打断后自动恢复"） | 第 1 代：`AbortController`；第 2 代：**持久化** `cancel_requested` + effect gate（`admit()` 同步拒绝新副作用，把取消做成"门"而不是"标志"） |
| 等待/长任务 | 中断等待超时可配置（`SessionServerAgentIO::interruptTimeout`，<=0 不限），由事件总线 `request` 阻塞等待 | 第 2 代：`drive` 返回 `waiting`，harness 不持有定时器，等待由宿主重新驱动 |
| 失败留痕 | 轮末 Tip 消息 + 保留部分输出的 assistant 兜底消息（`AutoInserted` 标志） | 第 1 代：合成 `stopReason=error/aborted` 的 assistant 消息；第 2 代：`pi.result/{operationId}` 记 `status/error/fromTipId/tipId/时间` |
| 持久化进度 | 图状态里 `xx_savedGraphData`（仅中断期间）+ 每轮节流落盘上下文 | 第 2 代：每次状态转移都是一次原子事务（`pi.op.state` 总状态替换） |

**优点/缺点**

- pi 第 2 代把「一轮」变成一等持久对象，带来三个直接收益：① 崩溃后能说清"这个操作到哪一步、要不要重跑副作用"；② 客户端可以只订阅操作状态而不依赖进程内回调；③ 一个 lane 的忙/闲、上次结果、排队项都能被外部（另一个进程）读到。代价是**状态数量与实现复杂度**：13 个叶子 + 迁移规则 + "不可无进展"断言，规则主要由一致性测试与 race catalog 维护（`harness.md` Part 9 列了 38 条不变式）。
- agentxx 的图驱动更简洁：控制流可由插件换图改写，中间件能包裹任意节点，中断恢复复用引擎 `resume_async`（不必另写状态机）。但"轮次"本身没有被建模：无轮次 id、无轮次结果记录、无"投递被接受还是排队"的显式返回值，导致 ① 客户端/插件只能靠 delta 事件顺序推断当前状态；② 队列在 IO 端点（易失、不落盘），跨进程重连或服务端重启后排队消息丢失。
- pi 第 1 代有一条 agentxx 没有的语义：**"用户插话"与"轮末补充"是两个不同投递通道**（`steer` vs `followUp`），且 `finishTurn` 可要求"再走一轮"。agentxx 只有一个队列，无法表达"这条消息要在工具跑完后立刻插进去"与"这条消息要等 agent 停下后再处理"的区别。

### 2.5 可迁移到 agentxx 的设计

1. **轮次记录（turn record）持久化**（P1）：为每轮写一条不可变记录（turnId、来源消息 id、起止消息 id、状态 completed/aborted/failed、错误码、耗时、模型、用量），并提供 `getTurn(turnId)` / `listTurns(limit)` 查询。落地位置：`agent/lib/src/agent/session_store.cpp`（新表）+ `BaseAgent::runTurnAsync` 收尾处写终态 + `Session` 暴露查询。收益：客户端与插件可以"问状态"而不是"数事件"，并为将来的 fork/重放（§3、§4）提供锚点。现状的 `turnStartMs/durationMs/tps` 等零散统计可先归并进该记录。
2. **投递结果显式化**（P1，成本低）：把"用户输入如何被接受"定义成返回值与事件：`started`（本轮执行）/ `queued`（进入队列）/ `steered`（轮内注入）/ `rejected`（无活动会话）。当前 `messageQueue_` 对外只暴露队列长度（`queueSizeForTest` 还是测试专用），客户端只能靠 `MessageQueueUpdate` 增量推断。落地位置：`SessionServerAgentIO::pushMessageQueueItem` 的调用点（`WireTurn` / `WireInterrupt` 处理路径）+ `wire_protocol.h` 增加响应字段。
3. **持久化队列（inbox）**（P2）：把 `messageQueue_` 从端点内存搬到会话存储（条目先落盘再消费，消费时写成用户消息），使服务端重启/客户端重连不丢排队消息，并让"排队项顺序与内容"成为会话数据的一部分。落地位置：`Session` 新增 pending 条目表 + `SessionServerAgentIO` 改为读写该表。
4. **重试结构化事件**（P1，独立小改动）：pi 的 `retry_scheduled` / `retry_start` / `retry_end`（带 step、attempt、maxAttempts、delayMs、notBefore、errorMessage）让 UI 能画出"还要等多久"。agentxx 现在只有一条 `MessageUITip` 文本（"X 秒后自动重试 (n/m)"）。建议在 `WireDelta::Type` 增加 `RetryScheduled`（同字段），`modelcall.cpp` 与 `summarization.cpp` 的退避点复用，UI 渲染倒计时。
5. **「无进展即错误」的断言**（P2，成本近零）：pi 在 drive 循环里断言"状态未变则抛错"。agentxx 的 `AgentRunner` 中断循环可加同类断言：`resume_async` 返回后若既没有新中断、也没有 `resumeValues`、也没有正常结束，应记录错误而不是静默退出（当前 `unresolvedInterrupt` 判定已接近此意，建议补齐日志级别与指标）。

---

## 3. 会话、分支树与 LLM 上下文

### 3.1 pi：entry 树 + 绑定值/列表 + 分支/车道，上下文是「投影」

**四件套**（`harness.md` Part 0.3）：

```
entries        会话树：写一次、永不修改（message / compaction / branch_summary / custom）
values/lists   当前可变状态：绑定类型地址上的可替换值（append-only 列表，只能整键删除）
usage ledger   成本账本：只追加
（操作私有状态）pi.op.* / pi.pending.*：只在操作存活期间存在，终态事务里删除
```

「每个载荷恰好存在于一处」是硬规则：不允许出现"既在树里、又在某个缓存里"的表示。

**Entry 树与放置（placement）**：

- 每条 entry 自带 `parentId`（指向父节点）、`seq`（提交时分配、会话内单调）、`timestamp`、`type`；`id` 是 UUIDv7（前 48 位是铸造时间，可时间排序，代价是泄漏创建时间）。
- **「内容先于位置」**：排队输入（steer/followUp/nextRun）与并发工具结果先以 `pi.pending.entry/{id}` 存在，真正入树时在同一个事务里「插入 entry + 删除 pending」。因此崩溃点只有"在事务之间"，不会出现半个事务。
- 工具结果**完成顺序**与**入树顺序**是两件事：并发工具各自完成即 stage 成 `outcome_ready`（此时工具永远不会再跑），再按 assistant 消息里的**源顺序**批量入树。
- 助手回复与空闲时直接追加是"生来就位"（一条事务完成）。

**分支与车道**：

- `Branch` = 树上一条命名路径，存在性等价于 `pi.branch.tip/{name}` 值存在；只拥有 tip、分支内查询、直接追加，**没有**模型/队列/操作状态。
- `AgentLane` = Branch + 车道配置（模型 `{provider, modelId}` / thinking / 活跃工具名）+ 车道状态（当前/上次操作、inbox）+ 每操作一条不可变结果记录。一个 lane 同时最多一个操作；并行工作靠**多条 lane 共享同一棵树**。

**上下文投影（provider 请求怎么拼）**（`harness.md §2.5`）：

1. 从 tip 沿父指针向根扫描，遇到**最新的 compaction 就停**；
2. 反转成 oldest-first，若有 compaction 则以「其 summary → `retainedTail` → 之后的所有 entry」为上下文，**更早的内容一律不读**；
3. 丢弃 `stopReason ∈ {error, aborted, deferred}` 的助手回复（真正的输出长度截断 `length` 保留）；
4. custom entry 经 `entryProjectors` 投影，没有投影器的 custom entry 不进上下文；
5. 再跑 `transform_context` 与 `toProviderMessages`。

**只追加不变式**：同一 lane 的多次请求，provider 上下文只能在**尾部增长**——插入到上一次请求尾部之前会击穿 KV 缓存、成倍增加成本。因此轮内写入会延迟到 checkpoint（在尾部追加），压缩是唯一被允许的"故意击穿缓存"。

**自定义条目与投影器**：`custom` entry + `EntryProjector` 让扩展把"不进模型上下文的数据"（如工具状态、渲染数据）与"要进上下文的内容"分开存，并统一在投影阶段决定是否进入请求。这与 agentxx 的「展示历史 / LLM 上下文双数据集」在目标上一致，但 pi 把它做成了同一条树上的两类节点 + 一个投影函数。

### 3.2 agentxx：会话 = 展示历史 + typed 上下文双数据集

`Session`（`lib/include/agentxx/agent/context.h`）字段与约束：

| 成员 | 说明 |
|---|---|
| `viewMessages` | **完整展示历史**，append-only、永不压缩；供 client 同步与展示 |
| `messages_`（typed `neograph::ChatMessage`） | **LLM 上下文**（唯一权威），可压缩/裁剪；写入口只有 `appendMessages`/`replaceMessages`/`replaceMessagesFromJson`/`truncateMessages` |
| `chainHash` | `viewMessages` 的链式哈希（FNV-1a 逐段追加，`count` + `tailHex`），供客户端校验一致性 |
| `deltaSeq` | 增量事件序号（单调，重放缓冲依赖） |
| `activity` | 会话活动状态（Idle/…，io 端点据此感知） |
| `contextStats` | 上下文占用统计（`std::atomic` 字段，跨线程安全），由压缩中间件在每次 modelcall 前更新 |
| `io` / `bus` / `cancelToken` / `modelName` | 会话级端点、事件总线、取消令牌、模型选择 |
| 线程约束 | `bindIoThread()`/`assertIoThread()`：可变状态只在绑定的 io 线程读写（Debug 断言，Release 记错误日志） |

- **上下文读写收敛**：节点/中间件经 `nodes/session_context.h` 访问 —— `sessionMessages()`（只读借用，**不得跨 `co_await` 持有**）、`appendSessionMessages()`（追加 + 发 CHANNEL_WRITE 事件）、`updateMessagesMeta()`（刷新图状态里的只读影子通道 `xx_messagesMeta`：`count`/`version`/`role_counts`/`last_role`/`last_tool_calls`/`last_tool_call_ids`）。
- **没有分支**：会话是唯一的线性上下文；重开会话 = 用持久化的上下文整体替换。`viewMessages` 保留所有历史（包括被压缩掉的部分），因此压缩只影响模型看到的内容，不影响展示。
- **压缩改写上下文本身**：`summarization` 中间件在超阈值时替换上下文（保留最近若干条 + 摘要），并刷新 `truncateMessages`/`replaceMessages` 版本号；`xx_messagesMeta.version` 是中间件与节点判断"上下文是否变化"的唯一信号。

### 3.3 对比

| 维度 | agentxx | pi |
|---|---|---|
| 会话数据模型 | 两个线性数据集（展示历史 + typed 上下文）+ 哈希 + 序号 | 一棵写一次的 entry 树 + 绑定值/列表 + 用量账本 |
| 历史可否分支 | 否（线性；同一 sessionId 只能一条上下文） | 是（entry 树天然支持分支；fork/branch 是一等操作） |
| 并行工作 | 多会话（每会话线性），或子代理（独立会话） | 多 lane 共享同一棵树，各自推进 |
| 上下文来源 | 会话持有的 typed 数组（压缩就地改写） | 由「分支 + 最近 compaction + 投影规则」导出，压缩不改写历史 |
| 压缩后的历史 | 展示历史完整保留（`viewMessages`），上下文被替换 | 原 entry 永不删除，新 compaction entry 成为上下文自包含检查点（含 `retainedTail`） |
| 自定义数据 | 展示侧自定义：`ViewMessage` 的角色子结构 + 插件 `ui` JSON；上下文侧无自定义条目 | `custom` entry + `entryProjectors` 参与上下文构造 |
| 一致性校验 | `chainHash`（展示历史）+ `deltaSeq` | 快照 + 事件折叠（`reduceLaneSnapshot`）+ 导航后的 `resnapshot` 屏障 |
| 上下文统计 | `ContextStats`（agent 侧计算并推送客户端） | `SessionStats`（存储侧维护投影，随每次提交更新） |
| 变更信号 | `messagesVersion`（O(1) 版本号，写进 `xx_messagesMeta.version`） | 每条 entry 的 `seq` + 事件流 |

**优点/缺点**

- pi 的树模型让「保留历史」「选择上下文」「并行探索」三件事互相独立：压缩不会丢历史（只是让上下文不再读更早内容），分支不需要复制历史（共享前缀），fork 有明确的字段级复制规则。代价是**数据模型与查询面变大**（entry 类型 + 分支扫描 + 分段索引 + 值/列表地址空间），且 SQLite 分支索引存在明确的 O(history) 复制缺口（`harness.md §2.6` 标为未解决）。
- agentxx 的双数据集模型实现简单、展示与上下文互不干扰，`chainHash` + `deltaSeq` 让客户端同步成本很低；但**没有分支**意味着"试试另一种方案再回来"只能靠新会话（丢掉上下文），且压缩是**破坏性**的：一旦上下文被摘要替换，原始细粒度内容只在 `viewMessages` 里保留（模型再也读不到，除非手写工具去读展示历史）。
- pi 的"只追加不变式"是关于**成本**的显式设计（保护 KV 缓存）；agentxx 在 `modelcall` 里每轮重建 system 消息（内容不变时前缀也稳定，因此同样吃到前缀缓存），但没有把"不要在中间插入"写成规则——中间件若有"往中间插一条消息"的写法会静默放大成本。

### 3.4 可迁移到 agentxx 的设计

1. **自定义条目 + 投影器（低成本、收益明确）**（P0）：在 `Session` 里加一类「不进上下文、但属于会话数据」的条目（如插件/中间件私有的状态快照、工具附加上下文），并提供一个投影函数决定它是否进入 `modelcall` 的请求。落地位置：`Session` 新增 `customEntries` + `AgentContext::buildSystemPrompt`/`modelcall` 组装阶段调用投影。收益：插件不必再"要么塞进上下文、要么自己找地方存"；也为 §2 的轮次记录提供了自然位置。
2. **压缩改成「保留尾部 + 不改历史」**（P1）：当前压缩就地替换上下文，导致"压缩后模型无法再看到原始细节"。建议压缩时把被压缩段落的原文以不可变形式留在会话数据里（现在只有 `viewMessages` 的展示副本），并把摘要作为**上下文起点**而不是替换上下文，即 `上下文 = [摘要] + 保留尾部`。落地位置：`middlewares/summarization.cpp`（生成阶段）+ `Session` 增加「上下文起点指针」概念。这同时是 §8 的迁移项。
3. **fork / 分支会话（P2，较大改动）**：pi 的 fork 语义给出了可直接借用的边界规则：会话名复制、标签按条目归属复制、用量从 0 开始、操作/pending 状态不复制、条目 id 保留、目标 `nextSeq` 取源高水位（防序号复用）。agentxx 若要支持"从某条消息分叉"，建议先以**整会话复制 + 指定消息为截断点**的形式落地（`SessionStore` 加 `forkSession(sourceId, uptoMsgId)`），而不是一次引入多分支。
4. **上下文只追加规则写入文档与检查**（P1，成本近零）：在 `docs/zh-cn/design/index.md` 明确「同一会话的请求上下文只允许在尾部增长；中间插入会击穿前缀缓存」，并在 `modelcall` 的 `repairMessages` 之后加一条 Debug 断言/日志（对比上一次请求的消息 id 序列是否为前缀）。这能防住中间件"插入式改写"带来的静默成本上升。
5. **上下文变更的显式版本 + 事件**（P1）：`xx_messagesMeta.version` 已经是版本号，但只写进图状态。建议把它作为 `WireDelta`/事件发布出去（如 `ContextVersion`），使客户端与插件能"以版本判断是否需要重新拉上下文"，不必比对内容哈希。

---

## 4. 持久化、事务与崩溃恢复

### 4.1 pi：绑定地址 + 原子事务 + 总状态替换

**存储模型**（`harness.md` Part 1）：

- **事务**：`commit(writes[])` 一次性提交；写类型只有 6 种（插 entry / 插 usage / 值 set / 值 delete / 列表 append / 列表整键 delete）；`seq` 在会话内严格递增（允许空洞）；一个事务要么全成功要么全不可见；提交失败**让整个 harness 进入 fault 状态**（不是可忽略的错误）。
- **绑定类型地址**：`value<T>(namespace, key)` / `list<T>(namespace, key)`；命名空间 `pi.*` 为内置保留；内置地址清单固定（`pi.branch.tip`、`pi.lane.config`、`pi.lane.state`、`pi.result`、`pi.op.meta`、`pi.op.state`、`pi.op.tool_args`、`pi.op.tool_memo`、`pi.op.preparation`、`pi.pending.entry`、`pi.pending.tool_output`、`pi.pending.assistant_frame`、`pi.session.name`、`pi.entry.label`）；列表只支持整键删除与按 `seq` 游标分页读取（默认上限 1000，硬顶 10000），**故意不提供"读整表"接口**。
- **「总状态替换」是恢复的唯一依据**：`pi.op.state` 每次转移整份替换；进程重启后按该状态直接进入对应过程，不重放日志、不推断。
- **代价与结算两段提交**：provider 请求与真实工具调用都写成「intent（要做什么、输出用哪些保留 id）→ 不确定的外部效果 → settlement（完整输出 + 下一状态）」。钩子走另一套"重放契约"：钩子结果在消费它的事务里变成持久数据，崩溃早于该事务则可能重跑。
- **用量账本**：每次结算的 provider 尝试都写一行（含失败/重试/合成），追加写、永不删除；`getStats()` 是维护好的投影（账本和 + message entry 计数），每次提交后都等于账本总和。

**三个后端**（同一套一致性测试）：

| 后端 | 关键机制 | 已知缺口 |
|---|---|---|
| Memory | Map + 单队列串行提交；无日志 | 无 |
| JSONL | 一行 = 一次 commit（或一次 commit 的多条写用数组行）；打开时按行重放成 Map；**截断的尾行整条丢弃**（含数组行全部元素），行内的坏帧视为损坏 | J1 快照压缩未实现：逻辑删除立即生效，但**物理字节永不回收**（`pi.op.state` 反复覆盖、帧列表删除等都留在文件里） |
| SQLite | 每会话一个库文件（也支持共享容器，全部按 `session_id` 分区）；`entries`/`scalar_values`/`list_values`/`usage_ledger` + 私有分支索引 `branch_entries`/`branch_meta` + 会话行（含 `next_seq`、stats）；触发器强制 id 唯一与父节点顺序；**可能写的每个事务必须 `BEGIN IMMEDIATE`**（deferred BEGIN 后升级写锁会 `database is locked`，`busy_timeout` 救不了） | 分支索引在未压缩长分支上的首次分叉会复制 O(history) 行（规格与实现矛盾，open）；R11 迁移机制只有规格 |

**崩溃恢复策略**（`harness.md §4.5`）按"孤儿重启点"分四类，四类的处理都写成规则：助手生成 `effect_pending`（读已提交帧前缀 `pendingAssistantFrames`，用 pi-ai 的 `reduceAssistantMessageFrames` 还原部分输出，合成一条零用量 error 响应并给出明确警告文案）；结构化生成 `effect_pending`（整次尝试视为不确定，按捕获的策略重试或到上限失败）；工具调用 `effect_pending`（声明 `replay:"safe"` 才重跑，否则合成"被中断"结果并保留已提交检查点内容）；deferred 轮询 `effect_pending`（按许可重取）。

**格式版本与迁移**（Part 7，规格未实现）：会话级 `storageVersion`；打开时 `version < current` 则按链式迁移（每步一个事务、幂等），`version > current` 拒绝打开（旧程序不开新会话）。迁移被定义为**总量映射**：状态机形状变化时，新状态的作者必须在同一次改动里为每个可达旧状态写出映射。

### 4.2 agentxx：单库 SQLite + 整表替换 + 节流落盘

- **一个会话一个 SQLite 库**（`{root}/{sessionId}/`，root 由 `dataDir` 派生或 `sessionStoreDirectory` 指定）；schema 由 `session_store.cpp::kSessionSchema` 建：`view_message(seq AUTOINCREMENT, json, msg_id)`、`llm_context(id=1, json)`（**整表只有一行**）、`meta(key, value)`、`store(id, value)`（share store 用）。
- **上下文整表替换**：`saveLlmMessages()` → `DELETE FROM llm_context; INSERT INTO llm_context(id, json) VALUES (1, ?)`；即每次落盘重写整段上下文。
- **节流落盘**：`requestSaveLlmMessages()` 按 `kPersistThrottleMs`（3s）节流，首次立即落盘；`flushViewMessages()` 同理，另有待处理的展示历史写操作队列（`PendingViewOp`）在轮末补齐。轮末再做一次权威保存。
- **检查点**：`SingleCheckpointStore`/`InMemorySingleCheckpointStore`（仅进程内存）；`save` 时自动淘汰该会话的历史 checkpoint，只保留最新一个（引擎恢复/`update_state` 只依赖最新 checkpoint 与其 pending writes）。图状态里另存 `xx_savedGraphData`（仅中断期间：中断节点 + 待处理参数），用于进程重启后继续未完成的中断。
- **展示历史与服务端缓冲**：`viewMessages` 全量落库；服务端另有增量重放缓冲（以字节上限控制）+ 历史分页（`WireGetViewMessages` / `WireViewMessagesPage`，`fromIndex`/`totalMessages`）；`WireSyncPayload` 带 `deltaSeq`，服务端进程重启后 seq 复位为 0，客户端据此重置去重序号（否则新会话 seq=1,2… 会被当成重复丢弃——源码里有专门注释）。
- **无格式版本、无迁移机制**：表结构变更靠 `ALTER TABLE` + `PRAGMA table_info` 探测（见 `view_message` 补 `msg_id` 列与索引的代码路径），没有版本号字段。

### 4.3 对比

| 维度 | agentxx | pi（harness） |
|---|---|---|
| 存储单元 | 会话 = 一个 SQLite 库；上下文整表一行 JSON | 会话 = 一组存储事务对象（entries/values/lists/ledger），三后端可选 |
| 写粒度 | 整段上下文重写（`DELETE`+`INSERT`）；展示历史逐条 insert/update | 6 种写原语，事务内可混合，`seq` 严格递增 |
| 崩溃语义 | 节流窗口内（<3s）的增量可能丢；轮末权威保存 | 崩溃只在事务之间；每个不确定副作用都有恢复策略 |
| 恢复依据 | 图状态里的 `xx_savedGraphData` + 最新 checkpoint + 会话上下文 | `pi.op.state` 总状态（含 intent 中的保留 id / 工具参数 / 重放声明） |
| 副作用重放 | 无显式声明；中断恢复靠 `resume_async` 回到中断节点 | 工具声明 `replay: safe/never`，规则化处理孤儿效果 |
| 格式版本 | 无（用 PRAGMA 探测列，手工 ALTER） | `storageVersion` + 链式迁移（迁移为总量的字段映射） |
| 分支/多写者 | 单写者（会话绑定 io 线程 + 断言） | 单写者（宿主保证）+ 存储不检测第二个写者 |
| 物理回收 | 删除即时（SQLite 页面复用） | JSONL 未实现（J1 缺口）；SQLite 就地 upsert |
| 成本账本 | 无独立账本；统计散落在会话/事件里 | 追加式 `usage_ledger` + `getStats()` 投影，压力测试断言"提交后统计=账本和" |
| 大对象策略 | 附件走 base64 / 服务端自取；share store 溢出改存 | `pending.entry` 延迟放置；工具检查点有界；帧列表按 `seq` 分页 |

**优点/缺点**

- pi 的模型把「崩溃可恢复」提升为数据模型的性质：事务原子 + 总状态替换 + 显式 intent/结算，使恢复路径可以被穷举（`harness.md Part 9` 的 38 条不变式 + race catalog）。代价是可观的设计与实现成本，而且有明确未落地项（J1 物理回收、R11 迁移、C1 远端会话），JSONL 后端在长会话下会持续增长。
- agentxx 的整表替换在**短中会话**下极其简单可靠（一次 `DELETE`+`INSERT`，无并发问题，无脏状态），且 SQLite 删除即时回收页面；但有两个结构性代价：① 每次落盘重写整段上下文，长上下文（数十万 token）时写放大明显（`memory-1/memory-2` 系列优化正是围绕此）；② 没有格式版本与账本，跨版本升级与会话级成本统计都要额外机制。
- **用量账本**是 agentxx 明显缺失的一块：现在 token/成本统计散落在 `ContextStats`、会话 meta、UI 提示里，无法审计，也无法回答"某个会话/某轮花了多少"。这是低成本高收益的迁移项。
- 崩溃恢复方面，agentxx 的 `xx_savedGraphData` 只覆盖"中断期间"，而**LLM 流式过程中**崩溃（已收到部分 token、未落盘）没有任何恢复语义：重启后上下文里没有这一轮，用户看到的是"上一轮末尾"。pi 用「帧前缀 + 明确警告文案」把这一窗口变成可见、可解释的状态。

### 4.4 可迁移到 agentxx 的设计

1. **用法账本替换零散统计**（P0，成本低收益高）：新增 `usage` 表（`id`/`seq`/`turnId`/`msgId`/provider/model/`input`/`output`/`cacheRead`/`cacheWrite`/`cost`/`adjustment`/`details`），每次 LLM 结算写一行（含失败与重试），并在会话提供 `getUsageStats(fromSeq)` 聚合。落地位置：`session_store.cpp`（新表）+ `nodes/modelcall.cpp`（结算点）+ `ConnectionStats`/UI 改为读账本。收益：可审计的成本统计、可做"按会话/按轮"报表、为将来 `train` 模式与插件分析提供数据。
2. **存储格式版本 + 迁移入口**（P1）：在会话库 `meta` 表写 `storage_version`；打开时 `version < current` 走链式迁移（每步一个事务、幂等），`version > current` 拒绝打开并提示升级程序。落地位置：`SessionStore` 打开路径 + 一个 `migrations/` 目录（当前 `ALTER TABLE` 探测逻辑收进去）。这同时解决"用户跨版本回退程序后读坏数据"的风险。
3. **未完成轮的可见化（帧前缀或等价物）**（P1）：落盘策略从"整段上下文覆盖"改为"轮内增量可追加"（已具备：`appendSettledLlmMessages` 思路），并对"流式期间崩溃"给出显式语义：要么在会话里留一条 `AutoInserted` 的 warn 提示（"本轮请求被中断，以下为已收到部分"），要么在恢复时插入等价提示。落地位置：`modelcall.cpp` 的流式结算 + `base_agent.cpp` 的会话恢复分支（`resumeInterrupt` 旁边的 `unfinishedTurn` 判定）。
4. **工具重放声明**（P1，接口很小）：`XXToolBase` 增加 `replayPolicy()`（`never`/`safe`，默认 `never`），并在工具中断/取消兜底路径（`insertAbortedToolResults`）以及将来的"进程重启后未完成工具"路径里使用：`safe` 的读写类工具可在恢复时重跑，`never` 的写类工具只生成"被中断"结果。落地位置：`tools/tool.h` + `nodes/toolcall.cpp`。
5. **列表/大对象的有界读**（P2）：pi 明确不提供"读整表"，只提供按 `seq` 游标的分页（默认 1000 / 硬顶 10000）。agentxx 的 `share_store` 与 `viewMessages` 也有类似压力（长会话全量同步）。建议给会话历史与 share store 增加统一的分页读取接口与硬上限（`getViewMessagesRange` 已具备区间读，缺的是"默认上限 + 明确拒绝无上限请求"的约定）。
6. **提交失败即不可继续**（P2，设计约定）：pi 规定"提交失败 → harness fault，进程必须重启"，避免在损坏状态上继续跑。agentxx 目前落盘失败只记日志继续（`SessionStoreHooks` 注释即写明"尽力而为"）。建议区分「可忽略的展示写失败」与「上下文写失败」：后者应在会话层标记为不可持久化状态并在 UI 明确告警，而不是静默继续。

---

## 5. 工具系统

### 5.1 pi：声明 → 三段式执行 → 结果顺序与完成顺序分离

**工具声明与调用形状**（`agent/src/types.ts`）：

```ts
interface AgentTool<TParameters, TDetails> extends Tool<TParameters> {
  label: string;                                  // UI 展示名
  prepareArguments?(args: unknown): Static<TParameters>;  // 原始参数兼容垫片（校验前）
  execute(toolCallId, params, signal?, onUpdate?, ...): Promise<AgentToolResult<TDetails>>;
  replay?: "never" | "safe";                      // 崩溃恢复策略
  executionMode?: "sequential" | "parallel";      // 单工具覆盖批次模式
}
interface AgentToolResult<T> { content: (TextContent|ImageContent)[]; details: T; usage?: Usage; terminate?: boolean }
```

- 参数用 TypeBox schema 声明并**在准备阶段校验**；`prepareArguments` 是"旧参数形状兼容"的垫片，必须在校验前跑且必须确定（可重复执行）。
- 工具结果分两层：`content`（给模型看）+ `details`（给 UI/状态重建用，`undefined` 表示无结构化细节）。
- 工具可以上报自己的 `usage`（例如内部又发起了模型调用），会话总量据此保持一致。
- `terminate: true`：**当批次里每个已定稿工具都置位**时才提前结束本轮（可用于"提交最终结果"型工具）。

**三段式执行**（`harness/execution/tools.ts`，harness 侧的显式阶段）：

```
prepareToolCall        查工具 + prepareArguments + 首次校验  → 未知工具/校验失败 = 立即合成 error
before_tool            钩子流水线（可改参数需重新校验，可 block）
executeToolCall        effect gate 准入 → tool.execute(...)，异常转 error 结果
finalizeToolCall       after_tool 逐字段 patch（content/details/isError/usage/terminate）
createToolResultMessage 生成规范的 ToolResultMessage
```

- **effect gate**（§2.2）：`gate.admit(() => tool.execute(...))` 是一次**同步**的准入检查——准备阶段全部做完再进入，取消先到就一次也不会跑。这是"取消不是标志而是门"的体现。
- **duriable 两段提交**：进入执行前先写 `pi.op.tool_args`（有效参数）并把该调用置为 `effect_pending`（带 `replay` 声明）；执行结束把完整结果 stage 进 `pi.pending.entry` 并置 `outcome_ready`（此后永不再执行）；最后按 assistant **源顺序**批量入树。
- **进度检查点**：`onUpdate(partial, { checkpoint: true })` 请求把"当前完整快照"写成该调用的持久检查点；写入是同步入队（不 await），取消/结算时"屏障式"等待最后一次写入，staging 会删除该值。工具自己负责检查点的大小、节奏与去重（内置 bash 策略：100ms 实时更新、最多每 2s 一次检查点、内容未变则不写）。

**内建工具与横切关注点**（`coding-agent/src/core/tools/*`，共 8 个工具 + 渲染器 + 包装器）：

| 关注点 | pi 的做法 |
|---|---|
| 输出截断 | `truncateHead`/`truncateTail`：**行数（2000）与字节（50KB）双限，先命中者生效，永不返回半行**；截断时把完整输出落盘（spill）并告诉模型文件路径 |
| 并发写同一文件 | `withFileMutationQueue(env, path, fn)`：按「环境 + canonical 路径」串行化整个"读-改-写"过程（先 `absolutePath` 再 `canonicalPath`，`not_found` 时退化为绝对路径），队列用完即删（避免 Map 无限增长） |
| 执行环境可替换 | 每个内建工具都接受 `XXXOperations` 接口（如 `ReadOperations.readFile/access/detectImageMimeType`），默认实现走本地文件系统；覆盖实现即可把工具指向 SSH/微虚拟机等远程环境（`gondolin` 示例即此机制） |
| 渲染与逻辑分离 | 工具逻辑在 `core/tools/*.ts`，渲染在 `core/tools/renderers/*.ts`；`tool-definition-wrapper.ts` 把定义适配成 agent 工具 |
| 动态工具集 | 扩展先注册全部工具，再用 `pi.setActiveTools(names)` 激活子集；名字必须是已注册的，未知名字忽略；工具集变化会以系统消息形式写进转录（见 §6） |
| 工具覆盖 | 扩展可整体替换某内建工具（`tool-override.ts` 示例），也可只禁用它 |

### 5.2 agentxx：图节点分发 + 中间件环绕 + 插件声明式权限

`XXToolBase`（`lib/include/agentxx/tools/tool.h`）的四个策略位与两个接口面：

| 成员 | 作用 |
|---|---|
| `autoSummaryOutput` | 输出超过 `toolcallSummaryLimitOutputLength` 时走摘要压缩（经 summarization 句柄） |
| `canDelayLoad` | 延迟加载：初始系统提示只给名称等简短信息，由 `tool_skill_search` 检索后再展开完整定义 |
| `maxRetry` | 执行抛异常时重试（最多 1+N 次） |
| `repeatCallCheck` | 连续相同调用检查开关（阈值 `toolcallRepeatCheckThreshold`，默认 5），命中时经 HIL 询问用户 |
| `execute_async(const utilxx_base::Json&)` | **业务主接口**（子类覆写） |
| `execute_async(const neograph::json&)` / `execute(...)` | 图兼容桥接（`ToolDispatchNode` 只认 `neograph::Tool`），经 `neograph_json_bridge` 转换 |

**`ToolcallWrapNode::baseRun` 的执行流程**（`nodes/toolcall.cpp`）：

```
解析 assistant 消息中的 tool_calls
→ findConsecutiveRepeatCallKeys(messages, assistantMsgIndex, threshold)   // key = {tool}_{argLen}_{argHash}
→ 为每个 tool_call 构造执行协程 onExecTool(tc)：
     execTool(...):
       ① autoFixArgsType（按 JSON Schema 自动修正参数类型/枚举大小写）
       ② 权限检查：经事件总线 Topic::ToolPermissionCheck 请求；未配置权限服务则默认放行
       ③ 连续重复调用确认：requestInterrupt(...) → 抛 NodeInterrupt → 宿主弹卡 → resume 回来后解析用户响应
       ④ 取消埋点（执行前）
       ⑤ 执行（XXToolBase 主接口 / 原生 neograph::Tool 桥接），失败按 maxRetry 重试
       ⑥ 输出压缩/摘要（超过长度阈值时）
       ⑦ 取消埋点（执行后）+ 记录 startTimeMs/durationMs
→ 依次 co_await 收集结果（源码中有 `// TODO: 真正并行`）
→ 取消时补齐未完成 tool 的占位结果（insertAbortedToolResults），保证消息顺序完整
```

- **取消语义**：取消/中断都不抛到节点外——`NodeInterrupt` 在 `onExecTool` 内部被捕获（置 `MessageFlag::Interrupt`，内容 `[Interrupt]`），由 `AgentRunner` 统一处理；`CancelledException` 则记录后跳出循环并补齐占位。
- **参数注入约定**：执行前把 `sessionId`（= `thread.id`）与 `tool_call_id` 注入 arguments，工具据此拿到会话取消令牌（`getSessionCancelToken`）与中断 resultId。
- **工具来源**：内置工具（`BaseAgent::initTools`）、中间件自带工具（`initMiddlewareTools` 自动收集）、MCP 工具（经 `XXToolWrap` 包装）、插件工具（`ToolRegistry`，注册时做与静态工具的重名冲突检测）、`tool_skill_search`（延迟加载检索）、子代理工具（`SubagentManagerMiddlewareHandle` 注入）。
- **权限由工具来源方声明**：插件注册工具后经 `agentxx.agent.permission` 表声明权限（作用域读/写、目标来源无/路径/文本、目标参数名；目标值按参数实际 JSON 类型处理：字符串单目标、数组逐项、可选分类文本）；**未声明的工具不参与权限判定**（直接放行）。模式/前缀参数工具（glob/grep/list）另用 `check_paths` 批量三态查询（DENY/ALLOW/ASK）逐路径复核，避免 `**` 模式绕过子目录拒绝规则。
- **完全授权**：`PermissionMiddlewareHandle::isFullAuthorized` 是状态源，客户端只持镜像（点击先乐观更新，服务端广播校准）。

### 5.3 对比

| 维度 | agentxx | pi |
|---|---|---|
| 工具定义 | `XXToolBase` 子类（C++ 虚函数 + JSON Schema），四个策略位 | `AgentTool` 对象/接口（TypeBox schema），`replay`/`executionMode` 两个策略位 |
| 参数修正 | `autoFixArgsType`（类型/枚举/字符串数组自动纠正）**在执行前** | `prepareArguments` 垫片（显式、由工具自己实现）+ 严格校验 |
| 执行编排 | 图节点 `ToolcallWrapNode` + 中间件环绕 | harness procedure 的三段式 + effect gate + intent/outcome |
| 并行执行 | **当前串行**（源码 `// TODO: 真正并行`），一次 assistant 消息里的多个调用逐个跑 | 默认并行（准备串行、执行并发），结果按源顺序入树 |
| 取消 | `CancelToken` + 取消埋点；取消后补齐未完成工具的占位结果 | `gate.admit()` 同步准入 + `AbortSignal` 透传 + 合成 `isError` 结果 |
| 崩溃恢复 | 无工具级恢复语义（重启后未完成的轮次不重放） | `replay: safe/never` + 检查点 + 恢复时"合成中断结果"或安全重跑 |
| 进度上报 | 无持久检查点；只有 UI 增量（`ToolStart`/`ToolEnd`）与耗时字段 | `onUpdate` 实时 + `checkpoint:true` 持久快照（工具自选策略） |
| 输出限制 | 长度阈值触发**摘要压缩**（会改写给模型看的内容） | 行/字节双限**截断** + 完整输出落盘并告知路径（不改写语义） |
| 权限 | 中间件统一判定 + 来源方声明目标 + 工作目录隔离 + 完全授权 + 重复调用确认 | 无内置权限；项目信任只控制资源加载；隔离靠容器/扩展拦门（示例 `permission-gate.ts`） |
| 沙箱/远程执行 | 工具内部自行实现（如 worktree 隔离、`agentxx_filesystem` 插件） | 统一的 `XXXOperations` 注入点，可把内建工具整体指向远程环境 |
| 渲染与逻辑 | 渲染在客户端（`builtin_tool_renderers.cpp` + 声明式 UI 描述） | 渲染器与逻辑在同包但分离文件（`renderers/*.ts`），扩展可替换 |
| 动态工具集 | 插件启用/禁用即时生效；`tool_skill_search` 延迟加载 | `setActiveTools` 激活子集 + 工具集变化经系统消息告知模型 |

**优点/缺点**

- agentxx 的**权限体系明显强于 pi**：声明式目标（按参数名解析）+ 三态批量复核 + 完全授权 + 重复调用确认 + 工作目录/ worktree 隔离，这些都建立在"谁注册工具谁声明权限"的纪律上；pi 的做法是"不提供安全边界"，只做项目信任（管资源加载）与容器化文档，具体拦门留给扩展（`permission-gate.ts` 只有几十行，且**非交互模式默认阻止**）。两者对"默认安全"的取向完全不同——这是值得保留的差异（见 §18）。
- pi 在工具执行上的工程细节更完整：参数准备（校验前垫片）→ 闸门准入 → 结果顺序与完成顺序分离 → 检查点 → 恢复策略，每一步都有对应测试层级；agentxx 目前有**一处明确缺口**：多工具调用串行执行（源码留 TODO），这直接让"并行读多个文件/跑多个查询"的收益丢失。
- 输出处理策略不同：pi 是"截断 + 全量落盘 + 告诉模型路径"（保留原文可回溯），agentxx 是"超长就摘要压缩"（更省上下文，但改写内容且需要额外一次模型调用）。两者并不冲突，agentxx 可以两者都留：默认截断，超长阈值以上再摘要。
- 参数处理取向不同：agentxx 在宿主侧统一自动修正参数类型（对模型不太准的 JSON 比较宽容），pi 要求工具显式提供 `prepareArguments`。前者对弱模型更友好，后者更可预测。

### 5.4 可迁移到 agentxx 的设计

1. **多工具并行执行**（P0，收益明确）：把 `ToolcallWrapNode::baseRun` 里"构造协程后逐个 `co_await`"改成真正的并发收集（`co_spawn` + 收集 awaitable，或用 `asio::experimental::awaitable_operators`），并保持**结果按 assistant 源顺序**写入会话（这是 pi 明确区分的两套顺序，agentxx 现在天生满足顺序但牺牲了并发）。落地位置：`nodes/toolcall.cpp` 的 `toolcallResults` 循环；注意与取消补齐逻辑、`isInterrupt` 并发写入（源码注释已指出"协程并发等 co_await 执行完成时可能参数数组已经不是单一值"）一起改造。
2. **工具检查点**（P1）：给长时间运行的插件工具（`agentxx_system_monitor`、`websearch`、`execute_command`）一个可选的持久进度上报接口：`onUpdate(partialResult, {checkpoint:true})` → 写入会话级 pending 值（键 `{sessionId}:{toolCallId}`），结算时删除。收益：崩溃/重连后 UI 能显示"上次看到的进度"，而不是从零开始；实现成本低（一张表或复用 store 表）。
3. **工具重放策略与"合成中断结果"**（P1，与 §4.4-4 合并）：`replay: never` 的工具在恢复时生成明确的"被中断 + 未知结果"提示（含最新检查点内容），避免静默丢弃或误以为成功。
4. **文件写工具的统一串行化**（P1）：agentxx 的 `edit`/`write` 类工具当前依赖各自实现；pi 的 `withFileMutationQueue` 按「环境 + 规范化路径」串行化"读-改-写"整段，是实现简单、防错有效的模式。落地位置：`agentxx_filesystem` 插件内的写路径（或宿主提供 `utilxx::withPathMutex`）。
5. **工具输出截断（行 + 字节双限 + 落盘）**（P1）：作为摘要压缩之外的第一道防线，先"截断并告知完整输出路径"，只有需要时再摘要。落地位置：`XXToolBase` 增加 `maxOutputLines/maxOutputBytes` 与统一截断工具函数（可利用 `utilxx::truncateOutput` 若已有，否则新增）。
6. **执行环境注入点**（P2）：pi 每个内建工具都接受 `Operations` 接口，使"工具在哪跑"与"工具做什么"分离。agentxx 已有 C ABI 插件与补丁式实现，若要支持"工具在远端/容器内执行"，建议在 `XXToolBase` 的宿主侧增加一个可选的执行代理接口（而不是每个工具各自实现远端路径）。

---

## 6. 系统提示词、技能与请求组装

### 6.1 pi：提示词是「带名字的片段表」，且变化以消息形式进转录

**分层**：

| 层 | 位置 | 职责 |
|---|---|---|
| 片段构造 | `coding-agent/src/core/system-prompt.ts`：`buildSystemPromptSections(options) → SystemPromptSections` | 产出**有序、按名字索引的片段表**：`preamble`（未加标签的前言）、`tools`、`rules`、`docs`、`addendum`、`project_context`、`skills`、`cwd`，外加自定义片段（每个非 preamble 片段包在 XML 标签里） |
| 转录表示 | `pi-ai` 的 `SystemMessage` | `content`（文本）+ `sections`（命名片段，`null` 删除某片段）+ `toolsAdded` / `toolsRemoved`（工具集变化） |
| 片段规则 | `buildRules(selectedTools, toolGuidelines, promptGuidelines)` | 只把**当前已选工具**的指引加入 `rules`；去重、稳定顺序 |
| 项目上下文 | `contextFiles`（如 AGENTS.md 等按目录收集的文件） | 渲染成 `project_context` 片段 |
| 技能 | `formatSkillsForSystemPrompt(skills)`（`agent/src/harness/system-prompt.ts`） | 只放 **name / description / location**，完整指令按需读取 |
| 扩展注入 | `before_agent_start` 钩子可改片段、可用 `systemPrompt` 强行替换整份提示词（`forceSystemPrompt`） | 强行替换时转录仍记录结构化片段，provider 收到的是被替换的文本作为前导系统提示 |

**关键机制：提示词差异是消息**。`agent-loop.ts::declareToolChanges` 在每次请求前比较「转录里已声明的工具」与「运行时实际可执行的工具」，把差异写成一条 `system` 消息（`toolsAdded`/`toolsRemoved`）；若已有 pending 的 system 消息，则把它的工具字段当作"意图"重算成"提交转录与可执行集合之间的差"。这样：

- **重放转录即可精确复现"模型当时看到的工具集"**（不是靠运行时状态推断）；
- 工具集与提示词片段的变化都留下历史，审计/调试可以看到"第 N 轮时模型知道什么";
- 代价是 provider 适配层要理解"system 消息 = 对系统上下文的补丁"，不支持该语义的 provider 需要一次完整的转录检查点（会击穿前缀缓存，源码注释明确提示这一点）。

**技能与提示模板**：

- skill = 目录 + `SKILL.md`（frontmatter：`name`/`description`，可选 `disable-model-invocation`），遵循 Agent Skills 规范；启动时只把 name/description/path 放进提示词（`<available_skills>` 段），任务匹配时模型自己读 `SKILL.md`（也提供 `/skill:name` 强制加载）。
- prompt template = 可复用文本片段，在**编辑器输入变成用户消息之前**展开（`expandPromptTemplate`），可选参数。
- 项目信任决定"是否加载项目级 skill/扩展/设置"（见 §9）。

### 6.2 agentxx：字符串拼装 + 追加段 + 工具提示词表

- `AgentPrompt`（`lib/src/agent/prompt.cpp`）三部分：
  - `systemPrompt`：一段完整的系统提示词字符串（默认内容在构造函数里，含 agent 名、工作方式、权限说明等）；
  - `appendSystemPrompts`：**按 key 索引的追加片段表**（`planning`/`skill`/`codegraph`/`summarization` 等），`buildSystemPrompt` 按固定顺序拼接（`planning → skill → codegraph → 其余按 key 顺序`），跳过 `summarization` 等不进提示词的段；
  - `toolPrompt`：按工具名索引的 `ToolPrompt`（`depict` 描述 + 参数说明），在工具定义生成时拼进工具 description（`init()` 里对空 description 告警）。
- `AgentContext::buildSystemPrompt(sessionId)`：基础提示词 + 追加段 + **动态追加消息**（`graphDataKey_appendSystemMessage`，`std::vector<std::string>`，会话级）；换行拼接时做空行规整。
- `modelcall` 节点每轮把该字符串写成上下文的**第一条 system 消息**（首条已是 system 就就地更新），因此提示词变化立即生效，但"变化历史"不进上下文。
- **资源注入**：`AgentResourceApplier` 负责 Skill（`middlewares/skill.cpp`：技能目录扫描 + 名称/描述注入 + `tool_skill_search` 检索）、Memory 文件（`middlewares/memory_file.cpp`：AGENTS.md 类文件注入为追加段或消息）、MCP（工具/资源）。
- **工具提示词与模块共享**：工具的参数说明集中在 `toolPrompt` 表里（与 `checkToolSchemaValidity` 的启动期 schema 校验配合），插件工具可自带 prompt。

### 6.3 对比

| 维度 | agentxx | pi |
|---|---|---|
| 提示词表示 | 一个字符串 + 追加片段表（key → 文本） | 有序命名片段表（`SystemPromptSections`），文本只是渲染产物 |
| 转录中的形态 | 每轮重建，只保留最新一条 system 消息 | 首条 system 消息声明片段；后续 system 消息是片段替换/工具集变化的补丁 |
| 工具集变化 | 工具集在启动时固定（插件启用/禁用 + 动态注册）；模型感知靠工具定义本身 | 差异写成 `toolsAdded`/`toolsRemoved` 消息，可回放；不可表示的 provider 走整份检查点 |
| 工具说明归属 | 集中在 `toolPrompt` 表（宿主持有） | 工具自带 `description` + 每工具可选 `snippet`/`guidelines` 贡献（宿主只负责选择哪些工具处于活跃） |
| 技能 | 目录 + 描述注入 + `tool_skill_search` 检索延迟加载 | 目录 + `SKILL.md` frontmatter + `<available_skills>` 段 + 模型自读 / `/skill:` 强制 |
| 项目上下文文件 | Memory 中间件（AGENTS.md 类） | `contextFiles` → `project_context` 片段（受项目信任控制） |
| 提示模板 | 无（用户输入即原文） | `prompt-template`：编辑器展开、可带参数 |
| 动态追加 | `graphDataKey_appendSystemMessage`（会话级字符串列表） | `before_agent_start` 钩子可改片段/整份替换；转录记录结构化片段 |
| 每轮重建的代价 | 每轮拼接字符串并替换首条 system | 只在片段变化时发补丁；普通轮次上下文前缀稳定 |

**优点/缺点**

- pi 的"片段表 + 差异即消息"把**可审计性与缓存友好度**都做出来了：模型当时看到的提示词可以从转录精确重建，工具集变化不会静默发生；`rules` 只包含活跃工具的指引，减少无关噪音。代价是 provider 适配层必须理解 system-补丁语义（不支持时要发完整检查点，可能击穿缓存），以及提示词构造与转录写入要成对维护。
- agentxx 的提示词构造简单直接（字符串拼接 + 有序追加段），每轮重建保证"提示词与配置永远一致"；但模型看不到"提示词变了"这件事，插件/中间件也无法把"重要变更"以结构化方式告知模型（现在只能插一条 Tip 或往 `appendSystemMessage` 塞文本）。`toolPrompt` 集中在宿主表里，好处是统一管理，坏处是**插件工具的提示词与插件代码分离**（插件作者要同时改插件和宿主表，或自带 prompt 绕过该机制）。
- 技能加载两边思路接近（都靠"描述入提示词、正文按需读"），agentxx 用 `tool_skill_search` 做检索式延迟加载（对工具也适用），pi 用 `/skill:` 命令强制加载。可以互补：agentxx 缺"用户强制加载"的显式入口（TUI 里有用但不易发现）。

### 6.4 可迁移到 agentxx 的设计

1. **提示词片段化（P1）**：把 `systemPrompt` + `appendSystemPrompts` 重构成有序命名片段（`preamble`/`tools`/`rules`/`skills`/`memory`/`cwd`/`addendum` + 自定义），`buildSystemPrompt` 只负责渲染与稳定排序；每个片段有稳定名字与内容哈希。收益：① 可对比两轮之间的片段差异并只记录差异；② 插件/中间件可以声明"我贡献哪个片段"；③ 为下面的"变化可见性"提供基础。落地位置：`lib/src/agent/prompt.cpp` + `context.cpp::buildSystemPrompt`。
2. **工具集变化的结构化告知（P1）**：插件启用/禁用或动态注册工具后，在下一轮上下文里追加一条**结构化 system 消息**（或等价的可解析条目）说明"新增/移除的工具名 + 一句话说明"，而不是仅靠工具定义变化。落地位置：`modelcall` 组装阶段对比"上轮工具名集合 vs 本轮"，差异写入上下文（`AutoInserted` 标志）。收益：模型不必"自己发现工具变了"，也便于审计。
3. **工具自带提示词贡献（P2）**：允许 `XXToolBase` 提供 `snippet()/guidelines()`（默认从 `toolPrompt` 表取，便于兼容），插件工具可自带；`buildSystemPrompt` 只把**当前启用工具**的指引拼进 `rules` 段。落地位置：`tools/tool.h` + `prompt.cpp`。
4. **显式技能强制加载入口（P2）**：TUI/CLI 支持 `/<skill>:name args` 形式直接注入技能正文（当前只能靠模型自行检索或 `agentxx_filesystem_read`）。落地位置：`client` 输入解析 + `middlewares/skill.cpp` 提供"按名取正文"接口。
5. **提示模板（P2）**：把常用指令片段做成可复用模板，在"输入 → 用户消息"之间展开（可带参数），与 §14 的插件资源机制合并实现（插件可提供模板文件）。

---

## 7. LLM 流式、多 provider 与鉴权

### 7.1 pi：目录生成化的 provider 体系 + 分层鉴权 + 能力元数据

**包结构**（`packages/ai`，189 文件 / 23 k 行）：

| 位置 | 内容 |
|---|---|
| `types.ts` | 统一类型：`Message`/`AssistantMessage`/`Usage`/`StopReason`/`Tool`/`Context`/`StreamOptions`/`Model`/`Provider` |
| `api/*.ts` | 各 provider 协议实现（openai-completions、openai-responses、anthropic-messages、google-generative-ai、bedrock-converse-stream、mistral-conversations、pi-messages…），每个另有 `.lazy.ts` 变体按需加载 |
| `providers/*.ts` + `providers/*.models.ts` | 42 个 provider 的工厂与模型目录数据 |
| `models.generated.ts` | 由 `scripts/generate-models.ts` 生成的目录汇总（禁止手改） |
| `auth/` | ApiKey + OAuth（anthropic、github-copilot、openai-codex、kimi、openrouter、radius、xai、设备码流、PKCE、浏览器回调页）+ 凭据库 + 解析 |
| `utils/` | retry（含可重试性分类）、overflow（上下文溢出判定）、estimate（token 估算）、event-stream、assistant-message-frame（流式帧编解码 + reducer）、abort、headers、代理支持等 |
| `compat.ts` | 旧全局 API 兼容层（`pi-ai/compat` 子路径导出） |

**`Models` 注册表**（`models.ts`）统一暴露：`getProviders/getProvider/getModels/getAllModels/refresh/checkAuth/getAvailable/getAuth/login/logout/streamSimple/completeSimple/streamDeferred/fetchDeferred/cancelDeferred/generateImages/classify`。

- **模型目录是数据**：42 个 `*.models.ts`（含分类器模型、图像模型）由脚本从上游目录生成；`AGENTS.md` 明令只能改生成脚本。
- **能力元数据**：`Model` 自带 `reasoning`、`input`（如 `image`）、`contextWindow`、`maxTokens`、`cost` 与各 provider 的 thinking 级别；调用方按能力决策（如模型不支持图片就加提示并从请求省略）。
- **鉴权分层**：`ApiKey` 与 `OAuth`（设备码、PKCE、浏览器回调）统一在 `auth/resolve.ts` 解析，支持按调用动态取 key（短时 token 刷新），并提供 `checkAuth` 给 CLI 启动检查。
- **重试 = 分类 + 退避**（`utils/retry.ts`）：`isRetryableAssistantError` 用两组正则区分「**不可重试的账户/额度限制**」（`GoUsageLimitError`、`insufficient_quota`、`billing`、`out of budget`、`Monthly usage limit reached`…）与「可重试的瞬时错误」（overloaded、`rate.?limit`、429/5xx、连接/网络类、`stream ended before message_stop`、WebSocket 关闭、gRPC `ResourceExhausted`…）；`retryAssistantCall` 实现 `baseDelayMs * 2^(attempt-1)` 指数退避（默认上限 60s），并回调 `onRetryScheduled`/`onRetryAttemptStart`/`onRetryFinished` 供 UI 展示；**退避期间被取消会归一化成 `stopReason:"aborted"`**。
- **假 provider**（`providers/faux.ts`，651 行）：可编程本地 provider，测试与 `test/suite` 用它做确定性回归，且走的是与生产一致的接口路径。
- **延迟流（deferred）**：`streamDeferred/fetchDeferred/cancelDeferred` 支持"先挂起、稍后取结果"的 provider 模式（长任务型响应），harness 侧对应 `deferred.suspended` / `deferred.effect_pending` 两个持久状态。
- **流式帧**（`utils/assistant-message-frame.ts`）：把流事件编码成紧凑帧 + `reduceAssistantMessageFrames` 还原，用于崩溃恢复与重连展示（§4）。

### 7.2 agentxx：注册表 + 三类协议实现 + 限流装饰器

- **`ModelProviderRegistry`**（`lib/src/agent/model_registry.cpp`）：命名模型配置表 + provider 实例缓存；提供 `registerModel`/`setDefaultModel`/`resolveModelName`/`getProvider`/`setProvider`（测试与嵌入方可注入自定义 provider）；"当前选择的模型"由**各会话独立记录**（`Session::setModelName`），UI 经 `WireSelectModel` 切换。
- **实现层**：`ModelConfig::type` 支持 `anthropic`、`openai-responses`、默认 OpenAI Chat Completions；`protocol/openai_provider.cpp`（2070 行）与 `anthropic_provider.cpp` 为自研实现（流式解析、thinking/reasoning 提取、工具调用增量拼接），另有 `neograph::llm` 的 `rate_limited_provider`（限流装饰器）与 `schema_provider`（结构化输出策略注册）。
- **请求参数能力**：`ModelConfig` 提供 `baseUrl`、`apiKey`（默认 `"EMPTY"`）、`modelName`、`connectTimeoutSeconds`（默认 16）、`readChunkTimeoutSeconds`（默认 60）、`modelContenxtMaxToken`、扩展请求参数（合并进请求 body）、连接池并发上限（`max_concurrent_connections`）、是否携带 thinking 等。
- **重试在节点里**：`modelcall` 节点自带重试（`llmMaxRetry` 默认 5 → 最多 6 次），退避 `retry*3s`（限速类错误再加 `retry*5s`）；失败时保留部分输出（≥512 字符即插入 `AutoInserted` 的 assistant 兜底消息）；重试提示以 `MessageUITip` **文本**发给 UI。
- **多模态输入**：附件按 `MediaType`（image/audio/video）分组，转成 `image_urls`/`audio_urls`/`video_urls` 进入用户消息；单文件与单消息条数上限在 `conversation_types.h` 里以常量定义（10/25/50 MB、最多 5 个）。服务端可自行读取本地路径并转 base64（线程池卸载）。
- **无鉴权体系**：只有 API Key（yaml 或 `.env`），没有 OAuth 流程、凭据库与登录/登出。

### 7.3 对比

| 维度 | agentxx | pi |
|---|---|---|
| provider 数量 | 3 类（Anthropic / OpenAI Responses / OpenAI Chat Completions）+ 限流与结构化装饰器 | 42 个 provider 目录 + 多协议实现（含 Google、Bedrock、Mistral、Cloudflare、各类网关） |
| 模型目录 | 手工在 yaml 配 `availableModels`（名称 → 配置） | 生成式目录（42 个 `*.models.ts` + `models.generated.ts`），带能力/价格/窗口元数据 |
| 能力元数据 | 上下文上限 + 多模态能力（客户端据此显示附件按钮） | `Model.input`/`reasoning`/`contextWindow`/`maxTokens`/`cost`/thinking 级别 |
| 鉴权 | API Key（yaml/.env） | ApiKey + OAuth 全家桶 + 凭据库 + 动态取 key |
| 重试 | 节点内固定退避（3s×retry，限速额外 5s），上限 6 次，所有异常同等对待 | 可重试性分类 + 指数退避 + 三个回调事件 + 默认 60s 上限 |
| 重试可见性 | 一条 `MessageUITip` 文本 | `retry_scheduled`/`retry_start`/`retry_end` 结构化事件（含 `notBefore`、attempt/maxAttempts、errorMessage） |
| 假 provider | 无（可经 `setProvider` 注入测试实例） | `providers/faux.ts` 可编程假 provider，测试套件统一使用 |
| 长任务/延迟结果 | 无 | `streamDeferred`/`fetchDeferred`/`cancelDeferred` + harness deferred 状态 |
| 崩溃期部分输出 | 无（重启后本轮不可见） | 流式帧列表 + reducer 还原部分输出 + 明确警告文案 |
| 多模态 | image/audio/video，客户端能力表驱动 UI | image（自动缩放）+ provider 能力校验，另有独立图像生成/分类接口 |
| 结构化输出 | `schema_provider`（策略注册） | 各 provider 实现内处理 |

**优点/缺点**

- pi 的 provider 层是"数据 + 适配器"：模型能力是数据（可生成、可刷新），协议实现按需加载（lazy），鉴权独立成层。新增 provider 只需加一个目录项与一个适配器文件；agentxx 加 provider 要改 C++ 并重新编译。
- pi 的**重试分类**值得直接借鉴：把账户/额度限制与瞬时错误分开，避免对前者做无意义退避。agentxx 目前对所有异常一律退避重试（余额不足这类确定性失败也会退避 6 次）。
- 取舍不同：agentxx 自研 provider 实现（对协议细节可控，能处理各家灰度差异，如 thinking 提取与工具调用增量拼接），代价是维护成本；pi 复用官方 SDK（`@anthropic-ai/sdk`、`openai`、`@google/genai`、`@aws-sdk/client-bedrock-runtime`），代价是依赖体积与升级约束（`AGENTS.md` 专设依赖升级条款）。
- 多模态面上 agentxx 更宽（音频/视频），pi 只有图像（外加独立的图像生成/分类 API），但 pi 的图像处理更细（自动缩放、非视觉模型的明确提示）。
- pi 的 deferred 是一条独立能力通道，agentxx 完全没有；接入"后台响应型"模型时会成为功能缺口。

### 7.4 可迁移到 agentxx 的设计

1. **重试可重试性分类**（P0，成本低）：在 `modelcall` 的 catch 分支先分类错误文本（HTTP 状态码、`insufficient_quota`/`billing`/`out of budget`、`rate limit`/429、连接类、5xx、流提前结束…），**不可重试的错误直接失败并给出明确提示**，只有可重试的才退避。落地位置：`nodes/modelcall.cpp` 的分类函数 + `config.h` 增加可配置的"不可重试关键字"列表。
2. **重试结构化事件**（与 §2.5-4 合并，P1）：把重试三阶段（scheduled / attempt start / finished）做成 `WireDelta` 类型，带 `attempt`/`maxAttempts`/`delayMs`/`notBefore`/`errorMessage`，UI 渲染倒计时而不是一段文本。
3. **错误分类进可审计记录**（P2）：重试耗尽的轮次把"最后错误类型 + 原始消息摘要"写进轮次记录（§2.5-1）与 Tip 消息，事后能区分是额度、网络还是网关问题。
4. **假 provider（确定性测试）**（P1）：在 `agentxx_test` 里实现一个可脚本化 `neograph::Provider`（按输入返回预设流/错误/延迟），使会话级测试（中断、重试、压缩、工具循环）不依赖真实网络。落地位置：复用 `ModelProviderRegistry::setProvider` 注入，新增若干端到端用例。
5. **模型能力元数据扩展**（P2）：把 `ModelConfig` 扩成能力表（`supportsVision`/`supportsToolCall`/`supportsStructuredOutput`/`supportsThinking`/`contextWindow`/`maxOutput`/价格），UI 与中间件按能力决策（非视觉模型不显示附件按钮、压缩阈值按 `contextWindow` 而非固定常量）。落地位置：`config.h` 的 `ModelConfig` + `buildModelInfo` 响应。
6. **延迟结果通道（P2，按需）**：若将来接入长任务型模型，在 provider 抽象里预留 deferred handle 语义，并在会话里记录 deferred 状态与轮询次数，避免把"等待"实现成阻塞请求。

---

## 8. 上下文压缩与分支摘要

### 8.1 pi：压缩是「持久化 operation」，摘要条目自带保留尾部

**两套摘要机制**（`coding-agent/docs/compaction.md` + `agent/src/harness/compaction/*`）：

| 机制 | 触发 | 作用 |
|---|---|---|
| Compaction（压缩） | 上下文超过 `contextWindow - reserveTokens`（默认保留 16384 tokens）或 `/compact` | 摘要旧消息，释放上下文 |
| Branch summarization（分支摘要） | `/tree` 导航切换分支时 | 保留"离开的那条分支"的要点 |

**触发时机有四个检查点**：① 新建用户输入前；② 多轮运行中，工具结果追加后、下一次助手响应前（在 `prepareNextTurn` 里做）；③ provider 返回上下文溢出错误后（可做**一次**"压缩并重试"）；④ 早停的 `stopReason: "length"`。压缩请求**关闭 prompt-cache 写**（一次性提示词不会被复用）。

**压缩算法**（`keepRecentTokens` 默认 20k）：

1. 从会话投影**反向**累计 token，走到 `keepRecentTokens` 得到切点；
2. 收集「上一次保留边界（或会话起点）→ 切点」之间的消息；
3. 用 LLM 生成**结构化摘要**，若存在上一次摘要则作为迭代上下文一起传入；
4. 追加一条 `CompactionEntry`（`summary` + `retainedTail` + `tokensBefore` + `details` + `usage` + `fromHook`）；
5. 下一次请求重建上下文：**摘要 + `retainedTail` + 其后的 entry**。

细节设计：

- **切分对齐用户消息跨度**：一个"用户消息跨度"= 一条用户消息及其后所有轮次，直到下一条用户消息。压缩通常切在跨度边界；当单个跨度超过 `keepRecentTokens` 时会在跨度**内部**切（`isSplitTurn = true`，`turnPrefixMessages` 单独处理）——避免"一条超长用户任务"永远无法压缩。
- **迭代压缩**：重复压缩时被摘要区间从"上一次压缩的保留边界"开始，而不是压缩条目本身，因此上次保留下来的消息仍会被这次摘要覆盖一次（不会出现缝隙）。
- **`tokensBefore` 在写完摘要前按重建后的投影重算**，保证记录的是"实际被替换掉的上下文量"。
- **原始条目永不删除**：省略只影响"下次请求带什么"，历史、报表、检索扩展仍能看到被省略的尝试。
- **溢出恢复顺序固定**：先持久化最终助手回复 → 触发 `turn_end` → 触发 `agent_end` → 追加省略编辑（context_edit）→ 若成功则压缩 → 以**新的 run** 重试；恢复失败则保留省略编辑、不压缩、不内部重试（队列中的工作照常按 steering/followUp 规则处理）。
- **压缩/导航摘要共用同一持久四元组**（`summary.deciding → summary.ready → summary.effect_pending ↔ summary.retry_wait`），`SummaryTask.boundary` 决定语义：`resume_checkpoint`（运行中压缩）/`finish`（独立压缩）/`commit_navigation`（带摘要的导航）；每次嵌套 provider 请求的用量单独结算并进账本。
- **钩子可干预**：`before_compaction`（可 decline、可直接给结果）与 `before_navigation`（同上）；hook 提供的摘要会带 `fromHook: true` 落库。

### 8.2 agentxx：阈值触发 + 去噪/折叠/去重 + 子代理摘要 + 硬截断兜底

`middlewares/summarization.cpp`（1186 行）的处理链：

```
每轮 modelcall 前（中间件 onModelcallRunFunc）：
  countTokens 估算上下文 token（按模型上下文上限 modelContenxtMaxToken）
  → 达到 75% 上限时自动压缩：
       ① cleanNoiseMessages：丢弃噪声消息
       ② downgradeMultimodalUrlsToText：多模态 URL 降级成文本（省 token）
       ③ foldExploratoryToolcalls：折叠"探索型"工具调用
       ④ doSummarizeToolcall：对**可压缩工具**的输出做摘要（按工具注册的 summarize 句柄）；
          带 generateDeduplicationKey 的工具输出先按 key 去重（相同 key 只保留一条）
       ⑤ splitRecentByTokenBudget：按 token 预算切出"要摘要的旧段"与"保留的最近段"
       ⑥ doSummarizeWithLLM：经**同上下文子代理**生成摘要（subagent 中断 → Session 派生 → resume 返回摘要），
          子代理显式关闭 summarization 以避免二次压缩破坏前缀缓存一致性
       ⑦ 写回会话上下文（replaceMessages，唯一权威）
  → 压缩后仍 ≥95% 上限：hardTruncate 兜底（保证请求一定能发出）
  → 压缩失败 / 反复触发时进入 coolDown（冷却期，避免每轮都派生 subagent）
```

- 工具的可压缩性由工具自己提供句柄：`XXToolBase::createSummarizationToolHandle()`，`BaseAgent::initSummarizationHandles` 收集为 `summarizationToolHandles`（工具名 → 句柄）；句柄可声明"生成去重 key"与"摘要算法"。
- 手动压缩：经事件总线请求（`doSummarizeWithLLM` 的 subagent 路径），有 `asyncWithTimeout` 保护，失败返回空串交由硬截断兜底。
- 压缩期间的中断语义：压缩子代理执行完后父轮次 resume；`AgentRunner` 专门注释了"取消后不得自动恢复"的双重检查。
- 摘要/压缩结果**替换上下文**（`replaceMessages`），展示历史 `viewMessages` 不受影响。

### 8.3 对比

| 维度 | agentxx | pi |
|---|---|---|
| 触发点 | `modelcall` 前（每轮检查），阈值 = 上下文上限的 75% | 四处：新建输入前 / 轮间（工具结果后、下次助手响应前）/ 溢出后（一次重试）/ 手动 |
| 阈值口径 | 比例（75%）+ 硬截断兜底（≥95%） | 绝对保留量（`reserveTokens` 16384）+ `keepRecentTokens`（20k） |
| 摘要生成者 | **同上下文子代理**（复用父上下文前缀，命中 KV 缓存；显式关闭二次压缩） | 独立 LLM 请求（结构化摘要，迭代传入上次摘要，关闭 cache 写） |
| 压缩产物 | 替换上下文（摘要 + 保留段写回会话） | 追加一条 `CompactionEntry`（含 `retainedTail`），原条目永不删除 |
| 切点规则 | 按 token 预算切"旧段/最近段"，并对工具输出做去重与折叠 | 用户消息跨度边界优先，超长跨度内部切（`isSplitTurn` + `turnPrefixMessages`） |
| 工具输出处理 | **摘要 + 去重（dedup key）+ 探索型调用折叠 + 多模态降级** | 不压缩工具输出（靠工具侧截断），压缩只针对消息跨度 |
| 迭代压缩 | 基于当前上下文（压缩后上下文即新基线） | 显式从"上次保留边界"开始，避免缝隙 |
| 失败兜底 | `hardTruncate`（保证请求能发出）+ 冷却期 | 恢复失败则不压缩、不重试；溢出只允许一次压缩重试，第二次溢出直接终态失败 |
| 可干预性 | 中间件在链上（可插入自定义策略） | `before_compaction`/`before_navigation` 钩子（可 decline 或直接提供结果）+ `fromHook` 记录来源 |
| 用量记录 | 无（摘要子代理的消耗混在会话里） | 每次嵌套请求单独结算进账本，摘要条目自带 `usage` |
| 分支摘要 | 无（没有分支） | 有（切换分支时对离开的分支生成摘要条目） |

**优点/缺点**

- agentxx 的压缩链更"懂工程细节"：噪声清理、多模态降级、探索型调用折叠、工具输出按 dedup key 去重、子代理复用父上下文以命中 KV 缓存，这些都是长会话真实痛点，pi 侧没有对应机制（pi 靠工具自己截断输出）。
- pi 的模型更"结构化"：压缩是一条可恢复的 operation，失败/取消/重试都有明确状态；摘要条目自带 `retainedTail`，因此**上下文读取不依赖"重新遍历到压缩点"以外的任何状态**，且原条目永久保留（可审计、可检索、可导出）。agentxx 的压缩是"就地替换"——一旦写入，被压缩掉的原文对模型永久消失（展示副本还在，但没有任何机制把它带回上下文）。
- pi 的溢出恢复有明确次数上限（一次），避免"溢出-压缩-再溢出"的抖动；agentxx 用 75%/95% 双阈值 + 冷却期解决同类问题，思路不同但都能止住抖动。
- pi 的分支摘要解决的是"并行探索"场景，agentxx 没有分支所以没有该问题——但也因此无法支持"回到某个岔路口"。

### 8.4 可迁移到 agentxx 的设计

1. **压缩产物保留尾部 + 作为"上下文起点"而不是替换**（P1，与 §3.4-2 合并）：`replaceMessages` 写入 `[摘要] + 保留段` 时，额外把被压缩掉的原始段落以不可变条目留在会话存储里（可复用 §3.4-1 的 custom entry 机制），并提供工具/命令"把某段原始历史取回上下文"。收益：压缩不再不可逆；长会话可以放心压缩。
2. **摘要条目携带用量与元信息**（P1）：压缩结果记录 `tokensBefore`/`tokensAfter`/`摘要用量`/`触发原因（阈值·溢出·手动）`/`保留段起止`。落地位置：`summarization.cpp` 写回处 + 会话存储（新 `compaction` 表或 meta 字段）。收益：可审计"每次压缩省了多少 token、代价多少"，并作为阈值调优依据。
3. **压缩的可恢复化**（P2）：把压缩做成"可重入"操作：记录"压缩意图（切点、保留段、摘要目标）→ 完成（写回）"两阶段，进程在中途崩溃时能判定"要么提交要么重做"，而不是留下半压缩状态。落地位置：`summarization.cpp` + 图状态（`agent_call` 已有类似的可恢复语义可参考）。
4. **溢出只允许一次压缩重试**（P1，成本低）：当前阈值/兜底逻辑较复杂（75%/95%/冷却/硬截断），建议增加一条明确规则：**同一次请求因上下文溢出失败后只允许一次压缩重试，第二次溢出直接失败并给出明确提示**（同时记录到轮次记录）。这能防住"反复压缩-反复溢出"的循环，也让错误可见。
5. **切点对齐用户消息跨度**（P2）：当前按 token 预算切段，可能在"用户任务中途"切断语义单元。建议切点优先落在用户消息边界（agentxx 的上下文里 `role=="user"` 可识别），只有当单个用户跨度超预算时才在内部切，并记录 `isSplitTurn` 之类的标记，供摘要提示词区分。
6. **压缩策略可干预接口**（P2）：给插件/中间件一个"压缩前置钩子"（可拒绝本次压缩、可直接提供摘要、可调整切点），与现有中间件链一致；同时把 `fromHook` 这类来源标记写进压缩记录，便于区分"模型摘要"与"扩展提供"。

---

## 9. 权限、沙箱与安全边界

### 9.1 pi：明确「不内置权限系统」，用信任决策 + 容器化 + 扩展拦门

**官方立场**（`README.md`、`SECURITY.md`、`docs/security.md`）：

- 「Pi 不包含限制文件系统、进程、网络或凭据访问的内置权限系统。默认以启动进程的用户权限运行。」
- `SECURITY.md` 的 Out Of Scope 逐条列出：本地代码执行与沙箱行为（**intentionally does not have a sandbox**）、用户安装的扩展/技能行为、不可信仓库风险、提示注入、需要"先有本地写权限"的报告等。
- 安全边界被定义为**操作系统账户边界**：该账户可写的文件（home、workspace、shell 启动文件、环境变量、Pi 配置）都视为同一信任域。

**三层缓解手段**：

| 层 | 机制 | 作用范围 |
|---|---|---|
| 项目信任（project trust） | `ProjectTrustStore` + `resolveProjectTrusted(appMode)` + `hasTrustRequiringProjectResources`；对未信任的项目先询问，未信任则不加载项目级设置/资源/扩展 | 只控制"**加载什么**"，不是执行沙箱 |
| 扩展拦门 | `tool_call` 钩子可 `block`：示例 `permission-gate.ts`（危险命令确认，**非交互模式默认阻止**）、`protected-paths.ts`、`dirty-repo-guard.ts`、`confirm-destructive.ts`、`timed-confirm.ts`、`plan-mode` | 进程内的策略拦截，策略完全由用户/扩展决定 |
| 环境隔离 | `docs/containerization.md` 的四种模式：Plain Docker（整进程进容器）、Docker Sandboxes（托管沙箱，凭据留宿主由代理替换）、OpenShell（策略化沙箱，含网络/凭据策略）、**Gondolin 扩展**（Pi 留宿主，只把内建工具与 `!` 命令路由进本地微虚拟机） | 真正的边界；工具级隔离挂在 `Operations` 注入点（§5.1） |

- 文档明确写出工具级隔离的局限："Tool-only isolation does not constrain the host Pi process or extension tools that do not use the isolated backend."
- 宿主不提供规则表/白名单/黑名单/完全授权等概念；"记住本次允许"由扩展自行实现（示例用 `ctx.ui.select`）。

### 9.2 agentxx：权限中间件 + 声明式目标 + 工作区隔离 + 完全授权

**判定顺序**（`middlewares/permission.h/.cpp`）：

```
判定单个目标 (checkTargetPermission):
  ① worktree 会话隔离边界   (allowPath 例外优先 → denyWritePath 内写操作拒绝, 读不受限)
  ② 配置拒绝路径 (denyPaths)
  ③ 完全授权 (fullAuthorized_)
  ④ 规则表命中 (最长前缀匹配 + * 通配符): ALLOW / DENY / INTERRUPT
  ⑤ noRuleOperator 兜底 (未命中任何规则时): 由 permission.mode 决定
```

- **模式**（`AgentConfig::permissionMode` → 兜底操作）：`ask`（工作目录内允许、其他路径询问）/ `all_ask`（一律询问）/ `pass`（一律放行）/ `deny`（一律拒绝）。
- **规则注册**：`setFilesystemPermission(path, op, READ|WRITE)`（读写分别注册）、`addConfigDenyPath(path)`。源码注释解释了为什么不依赖 `"/*"` 兜底规则（路由最长前缀回退到深层子树时不会命中根节点规则，必须由 `noRuleOperator` 保证语义）；工作目录取 `AgentConfig::resolvedWorkDir()`（跟随会话配置而非进程启动目录），取不到时不注册放行规则（安全兜底）。
- **工具权限由来源方声明**（插件在注册工具后经 `agentxx.agent.permission` 表声明）：
  - `ToolPermissionSpec{ scope(READ/WRITE), targetKind(none/path/text), argNames[], category }`；
  - 目标值按**参数实际 JSON 类型**解析：字符串=单目标、数组=逐项；任一目标被拒绝即拒绝该工具调用；
  - **未声明的工具不参与权限判定**（直接放行）——权限随工具来源（插件）走，声明随工具注销/插件禁用自动撤销；非本实例工具名或非法枚举取值会被拒绝并记日志。
- **模式/前缀类工具的逐路径复核**：`agentxx.agent.permission` 的 `check_paths` 批量三态查询（`DENY`/`ALLOW`/`ASK`，**查询阶段不弹窗**）：`glob`/`grep`/`list` 这类"参数是模式、真正访问哪些路径由模式决定"的工具必须先批量查询再逐项丢弃被拒或未获批准的目标，避免 `**` 绕过子目录拒绝规则；SDK 侧有 `filterPathPermissions` 等便利方法。
- **询问通道（HIL）**：`INTERRUPT` 经会话事件总线请求外部授权者（CLI/TUI/ACP 各自注册处理器）；询问卡片可携带"记住本次选择"（`RespPermission::remember`）；用户可勾选"完全授权"（`fullAuthorized_`，经 `WireSetFullAuth` 切换，状态变化广播 `EventPermissionFullAuthChanged`）。
- **其他相关能力**：git worktree 隔离（`agentxx_git_worktree` 工具 + 会话绑定，绑定后相对路径基准与权限边界都切到 worktree）、工具超时（`AgentConfig::toolTimeout` 默认 120s）、重复调用确认、输出长度限制。

### 9.3 对比

| 维度 | agentxx | pi |
|---|---|---|
| 内置权限系统 | 有（中间件 + 规则表 + 模式 + 完全授权） | **没有**（官方明示不提供，属产品定位） |
| 判定粒度 | 工具 + 目标（路径/文本），逐路径三态复核 | 无；扩展在 `tool_call` 钩子里自行实现（通常只能看参数文本，看不到"这次会访问哪些路径"） |
| 声明方式 | 工具来源（插件）声明"哪些参数是受约束目标" | 无声明面；扩展硬编码工具名与参数名 |
| 路径安全 | 词法规范化 + 最长前缀匹配 + worktree 写边界；**已知不跟随符号链接**（源码 TODO 说明原因与影响） | 不判定路径（边界靠容器/微虚拟机） |
| 交互与记忆 | 询问卡片 + 记住选择 + 完全授权 + 客户端镜像状态 | 扩展自行询问，宿主无记忆机制 |
| 真沙箱 | 无（worktree 只是写边界，不隔离进程/网络/凭据） | 有（容器/沙箱/微虚拟机四种模式，含凭据代理与网络策略） |
| 提示注入 / 不可信仓库 | 文档未显式划分责任边界 | `SECURITY.md` 明确划为 Out Of Scope，并写明"AGENTS.md/注释可被用于提示注入，无法防护" |
| 加载信任 | 无（按配置加载） | 项目信任决策：未信任不加载项目级设置/资源/扩展 |

**优点/缺点**

- agentxx 在**"工具会做什么"**这一层明显更强：声明式目标（按参数名解析）、逐路径三态复核、记住选择、完全授权、worktree 写边界，且规则与判定只有一处实现。pi 的扩展只能看到参数文本，无法知道 `glob` 展开后会碰哪些文件——这正是 agentxx `check_paths` 解决的问题。
- pi 在**"进程能做什么"**这一层更清醒：不假装自己有沙箱，明确把边界交给容器/微虚拟机，并给出四种方案（含"只把工具路由进虚拟机"的折中）。agentxx 目前没有这一层：worktree 只挡写，工具仍以宿主进程权限运行；文档也没有把"权限中间件 ≠ 沙箱"讲清楚。
- 已知缺口：agentxx 的路径判定**不解析符号链接**（源码 TODO：允许范围内的链接可跟随进入被拒目录，逐路径过滤也看不到链接目标；彻底处理需对已存在路径做 `weakly_canonical` 再判定一次，涉及所有工具与查询接口的性能与 Windows 语义）。
- pi 的**项目信任**（未信任不加载项目级资源）是 agentxx 完全没有的概念：agentxx 在某个目录启动就会加载该目录的配置/技能/记忆文件，缺少"先问一次"的决策点。

### 9.4 可迁移到 agentxx 的设计

1. **责任边界文档化**（P0，成本近零）：在 `docs/zh-cn/design/index.md` 或新增 `security.md` 里明确写出：权限中间件是**策略层**而非沙箱；工具以宿主进程权限运行；worktree 只提供写边界；不可信仓库与提示注入属用户责任。并列出当前已覆盖与未覆盖的攻击面（含符号链接缺口）。收益：避免使用者把"有权限系统"误读为"有隔离"。
2. **符号链接处理**（P1）：在 `checkTargetPermission` 与 `check_paths` 路径上，对**已存在路径**补一次 `weakly_canonical` 判定（保留词法判定作为第一道），并缓存规范化结果避免热路径重复系统调用。落地位置：`middlewares/permission.cpp` 的目标规范化函数 + 白/黑名单注册时预规范化。
3. **项目信任决策点**（P1）：会话首次进入某工作目录时询问一次"是否信任该目录的资源"（技能/记忆文件/插件配置）；未信任则不加载项目级资源。落地位置：`AgentContext`/`Session` 的资源配置路径 + `client` 复用中断卡片机制提供询问 UI。收益：防住"克隆一个仓库就自动加载其中的 AGENTS.md/技能指令"。
4. **工具级隔离接缝（面向将来）**（P2）：pi 的 `Operations` 注入点让"工具在哪执行"可替换。agentxx 若要支持"工具进容器/远端执行"，建议在 `XXToolBase` 增加可选执行代理（与 §5.4-6 同一条），并明确"只隔离工具执行、不隔离插件代码"的语义。
5. **授权审计**（P2）：现在"记住允许/拒绝"与"完全授权"只有运行时状态，没有落盘，也没有记录"谁在什么时候授权了什么"。建议为两者加轻量审计（写入会话 meta 或账本），便于事后复查与团队策略审查。

---

## 10. 子代理、任务与后台工作

### 10.1 pi：任务记录 + 依赖 + 所有权（pico v3 设计）；子代理是扩展示例

**现状分层**（这是本章最需要注意的地方）：

| 代际 | 位置 | 能力 |
|---|---|---|
| 主路径（第 1 代） | `agent`/`coding-agent` | **没有子代理原语**；官方以 `examples/extensions/subagent/` 扩展的形式提供（见下方说明） |
| pico v3（设计 + 部分实现） | `agent/docs/pico-v3.md` + `agent/src/harness/pico3/**` | 完整的任务/依赖/所有权/调度/取消模型（本节主要依据） |
| chord | `packages/chord` | 应用组合运行时：服务提供者/消费者、复制状态、RPC、插件（facet） |

**`examples/extensions/subagent/index.ts`（35 KB）的做法**（说明"没有原语时如何做"）：

- 每次子代理调用**spawn 一个新的 `pi` 子进程**（JSON 模式运行），因此子代理有独立上下文窗口与独立进程；
- 三种模式：single（`{agent, task}`）、parallel（`{tasks:[...]}`，上限 8 任务 / 并发 4）、chain（`{chain:[...]}`，前一步结果通过 `{previous}` 注入下一步）；
- 子代理定义从 frontmatter 文件发现（`agents.ts`：`name`/`description`/`tools`/`model`/`systemPrompt`，按 user/project 作用域扫描）；
- 每个任务的输出上限 50 KB；渲染层展示 tokens/成本/工具调用摘要。

**pico v3 的任务模型**（设计规格，`§5`）：

- **任务 = 一条会话上的持久工作记录**：`{ id, conversationId, kind, status, role, state, after[], foreground, spawnedBy?, ownedConversationId? }`；`role ∈ {ready, inflight, waiting, terminal}` 由 definition 的 `roles` 映射（如 generation 的 `streaming → inflight`、job 的 `running → waiting`），存储侧索引 role 以保证"查询活跃工作不必解码全部历史任务"。
- **definition 是可替换的行为单元**：`{ kind, initialStatus, roles, transitions, validateState, execute, recover, abort }`；注册表里有 `generation` / `tool` / `post_tools` / `job` 四类。
- **依赖语义 = 终态而非成功**：`T.after=[A,B]` 中 A 失败/中止也算"已结算"，T 是否继续由 T 自己解释（scheduler 不传播成功/失败策略）；`spawnedBy`（来源）与 `after`（依赖）是两种关系。
- **一次生成原子发布工具与其汇聚点（join）**：助手消息带 `[A,B]` → 一条结算命令里同时追加助手条目、创建 tool A/B、创建 `P = post_tools.after:[A,B]`、结算 generation；A/B 各自结算后 P 变为可执行，再由 P 决定"继续下一轮生成（G2）还是结束"。**工具只结算自己**，不做兄弟检查、不排空队列、不创建下一轮。
- **durable wait 不是任务生命周期承诺**：委派子代理的工具应"提交依赖 + 返回"，而不是在工具里 await 子代理的整个生命周期（内含一段明确的 DO NOT / DO 示例）。
- **前台/后台由创建时决定**：`foreground launch` = 创建 S 并把工具置为 `finishing after=[S]`（父交换无法完成直到 S 给结果）；`background launch` = 创建 S、把 S.id 放进工具结果、立即结算工具（父交换可继续）。会话级 abort 只选中前台/必要工作，另有 `abortBackground(taskId?)` 专门负责后台。
- **所有权链接任务与子会话**：`S.ownedConversationId` 与 `C.ownerTaskId` 互为反向引用且同事务提交；子会话完成在同一命令里结算所有者，不依赖"以后的通知"补造工作。
- **调度**：`ready = role==ready + 所有 after 终态 + notBefore 到点 + 所需 permit 由 drive 提供 + 未取消/未在执行`；一次调度趟（pass）先读取范围内活跃任务、**先向所有受影响的执行发取消信号**，再在独占 claim 下逐个执行；"未变化但对 ready 任务反复失败"要报错而不是无限重启（`§6.3`）。
- **取消**：一条取消命令标记所有非终态目标 `abortRequested`（标记跨重启存活、不因先结算的任务而缩小范围），然后**先 join 已有执行再调用 definition.abort**（同一个 claim 下），避免效果与 abort 并发写；前台 abort 会清掉排队中的 steer/followUp，保留 write/nextRun。
- **应用侧集成**：`job`（进程集成：spawn/adopt/exit 观察、PID 不是身份）、`schedule`（周期任务：稳定取消句柄 + 存下一次时间而非后继链）、`collapse`（压缩作为任务）、approval（把工具挂起等待人工决定，工具状态 `waiting after:[A]`，grant/deny 与 abort 串行化）。
- **`watch` 的无缝提交流**：一次序列化步骤里"捕获边界 + 读取有界页面 + 注册监听"，之后交付整批提交（`{firstSeq,lastSeq,changes}`）与工作区预览；客户端在**减去完整批次后**再发布（防止出现"终态与后继之间可观察的 idle 空隙"）。

### 10.2 agentxx：中断即委派 + 批量并发 + 深度/并发预算

- **委派路径**：`SubAgentManagerTool`（`tools/subagent.cpp`）被模型调用 → 构造中断参数 `{tasks:[...]}` → 抛 `NodeInterrupt` → `AgentRunner` 在中断处理循环里经 `events::Topic::Subagent` 发 `ReqSubagentBatch` → 宿主 `AgentHost::spawnOneTask` 派生**独立 agent 实例（独立会话、独立上下文）**执行 → 结果经 `buildSubagentResumeValues` 写回 `resumeValues` → `engine->resume_async` 回到中断节点，工具从"中断返回"处拿到结果。
- **批量与并发**：`ReqSubagentBatch.tasks[]` 一次可提交多个任务；宿主对每个子代理 `co_spawn` 独立协程，用 channel 收集完成信号（`wait_for_all` 模式），因此**批量任务真并发**（与 §5 的单轮多工具串行形成对比）。
- **预算与策略**（`AgentHost::spawnOneTask`）：嵌套深度预算 `maxDepth`（按 `sessionDepth_` 记录，超限直接拒绝并回错误）、并发上限 `maxConcurrentSubagents`、工具策略三态（`none` / 继承父工具（用 `AgentContext::toolNames`）/ 自定义白名单）、`enableSummarization` 开关、子代理模型（`AgentConfig::subagentModel`，未配置则用主模型）。
- **同上下文模式**：`messages` 参数可把结构化消息列表**原样透传**作为子代理初始上下文（含 system），压缩就是靠这个模式复用父上下文前缀以命中 KV 缓存（§8.2）；该模式强制使用父会话当前模型，避免"压缩后模型不一致"。
- **结果 key 规则统一**：`makeSubagentResumeKey(toolCallId, resultId, idx)` 由中断循环写入侧与工具读取侧共用（源码注释指出这是为消除三处重复规则、防止漂移）。
- **取消级联**：委派请求携带父会话的 `cancelToken`，父取消 → 中止全部在跑子代理（`RespSubagentBatchItem.cancelled`）。
- **没有"后台任务"概念**：所有委派都是**前台**——父轮次在中断处等待（等待期间可被取消），不存在"父会话结束、子任务继续在后台跑"的语义；也没有任务记录（不落盘、不可查询、重启后不存在）。

### 10.3 对比

| 维度 | agentxx | pi（pico v3 设计 / 主路径扩展） |
|---|---|---|
| 子代理实现 | 宿主内置：独立 agent 实例（同进程、独立会话） | 主路径靠扩展 spawn 子进程；pico v3 把"子代理"建模为任务 + 子会话所有权 |
| 任务记录 | 无（纯运行时对象；不落盘、不可查） | 有：`Task{kind,status,role,state,after[],foreground,spawnedBy,ownedConversationId}` 持久化并可索引查询 |
| 依赖 | 无（父轮次在中断处阻塞等待） | `after[]` 显式依赖，语义是"终态"而非"成功"；一个 join 任务（`post_tools`）聚合 |
| 前台/后台 | 只有前台（父轮次等待） | 两者都有：创建时决定（工具置 `finishing after=[S]` vs 直接返回 S.id） |
| 并发控制 | `maxDepth`（嵌套深度）+ `maxConcurrentSubagents`（全局并发） | `foreground` 判定 + drive 作用域 + permit（如 deferred-poll）+ 每任务一次执行 claim |
| 取消 | 级联 `CancelToken`（父取消→全部子代理取消），无持久标记 | 一条取消命令标记所有选中目标（跨重启存活），先 join 执行再调 `abort`，未启动的任务可直接 abort |
| 重启恢复 | 无（子代理与任务都不落盘） | 按 role 恢复：`inflight → recover`（外部效果不确定）、`waiting → restore observation`（重新装观察者，不重复） |
| 进程集成（job） | 无 | 有：spawn/adopt/exit 观察、PID 不是身份、输出排空、lost 状态 |
| 周期任务 | 无 | 有：稳定取消句柄 + `notBefore` + 有界补偿（防重启后疯狂补跑） |
| 人工审批 | 复用中断表单（权限询问/重复调用确认/中断表单同一套卡片） | approval 是任务：工具 `waiting after:[A]`，grant/deny 与 abort 串行化；恢复时重建决策界面，**不会自动授权** |
| 观测 | 事件总线 + 展示历史 | `watch` 的无缝提交流（边界捕获 + 整批提交 + 工作区预览），显式禁止"终态与后继之间出现 idle 空隙" |

**优点/缺点**

- agentxx 的子代理实现**开箱可用且真并发**（同进程、批量并发、深度与并发预算、工具策略、模型覆盖、同上下文压缩模式），比 pi 主路径"靠扩展 spawn 子进程"更省资源；pi 的 pico v3 虽然把任务模型做得很完整（依赖/所有权/恢复/取消/后台），但仍是**规格 + 部分实现**，主产品没有用上。
- 代价是 agentxx 没有任务层：① 委派不可查询（"我现在有几个子代理在跑？"只能看日志/UI 增量）；② 不能后台执行（父轮次必须等，长任务会长时间占用父会话）；③ 重启后子代理工作全部丢失（不落盘），且没有"未知结果"语义；④ 没有依赖/汇聚概念，无法表达"等两个子代理都完成后由第三个继续"（只能在工具里自己并发等待）。
- pi 的**依赖即终态**、**前台/后台由创建时决定**、**所有权链接子会话**、**取消先 join 再 abort** 这几条，在语义上都比"父轮次阻塞等待"更通用；其中"取消先 join 再 abort"与 agentxx 现有 `CancelledException` + 补齐占位结果的思路可以对齐（都在避免"效果与取消并发写"）。
- pi 的 `watch` 无缝提交流与 agentxx 的 `deltaSeq` + 重放缓冲解决的其实是同一个问题（客户端不能丢事件、不能看到不一致中间态），两边的取舍不同：pi 用"整批提交 + 客户端先减批次再发布"，agentxx 用"单调序号 + 去重 + 全量 Sync 兜底"。

### 10.4 可迁移到 agentxx 的设计

1. **子代理运行记录与可查询性**（P0，成本低）：为每次委派写一条记录（`taskId`、父/子 sessionId、名称、状态 running/done/failed/cancelled、起止时间、用量、结果摘要），供 UI/插件查询与审计；进程重启后这些记录至少能显示"上次有未完成的委派"。落地位置：`AgentHost::spawnOneTask` + `SessionStore` 新表 + 事件总线新增 `Topic::SubagentStatus`。
2. **后台委派（detached）**（P1）：给 `SubAgentManagerTool` 增加 `detach: true` 选项：创建子代理后立即返回 `{taskId}`（不阻塞父轮次），完成时经事件总线通知并在父会话插入一条结果提示。落地位置：`AgentHost`（不把任务加入父中断等待集合）+ `subagent.cpp`（参数与返回格式）+ 会话事件。
3. **依赖/汇聚语义**（P2）：当委派多任务且后续步骤依赖"全部完成"时，目前的表达方式是在工具里并发等待；建议引入轻量的"汇聚"概念（父轮次等所有子任务终态后再继续，任一失败不阻断其余），并在中断参数里显式声明 `join: "all" | "any"`。
4. **取消前的 join 顺序**（P1）：明确并测试"父取消 → 先等子代理当前执行返回（或标记后 join）→ 再调 abort"，避免效果与取消并发写；当前实现已拒绝"取消后自动恢复"，可在此基础上把顺序写进文档与测试。
5. **周期/定时委派**（P2）：`AgentConfig` 增加定时任务配置（如"每 N 分钟检查一次构建"），用稳定句柄 + 下次时间落盘，重启后按有界补偿执行（不补偿多于一次），避免"重启即疯狂补跑"。
6. **人工审批统一到任务**（P2）：现在审批/中断/重复调用确认都走中断卡片（这套机制已经很好），可进一步把"某工具在等人工决定"表达成一个可查询的挂起项（谁在等、等了多久、超时策略），而不是只在 UI 上看到一张卡片。

---

## 11. 会话检索、遥测、附件与大输出

### 11.1 pi

**会话检索（S3：设计稿，未实现）**

- `agent/src/search/index.ts` 目前只是一个与设计冲突的草稿骨架（`SessionSearchService`），**没有实现**。
- 设计要点（`harness.md §2.8`）：检索是**独立服务 + 自己的存储**，仓库（repository）完全不知道它；同步用拉取式（`repo.list()` + 只读打开 + 按 `(sessionId, storeGeneration)` 游标增量索引），事件只是"提醒"（丢了也会被下一次扫描补上）；索引是**可重建投影、零权威**（索引失败绝不影响会话提交）；参考实现是一个独立 SQLite（FTS5 表 + 游标表），对 JSONL 会话文件同样工作；多个进程可共享（WAL + `busy_timeout` + `BEGIN IMMEDIATE` + 幂等行 + 单调游标）。
- 未决问题也写明：元数据过滤（如按 `cwd` 过滤会话列表）如果放在排序后过滤是不正确的（会丢掉结果），需要考虑索引哪些元数据。

**遥测（`packages/telemetry`，零运行期依赖）**

- 契约面很小：`TelemetryContext.startSpan(name, attrs, cb)` / `TelemetrySpan{addEvent,setAttributes,setStatus}` + `NOOP_TELEMETRY_CONTEXT` + 内存实现 + **类型化 schema 机制**（`defineTelemetrySchema({version, spans:{name:{description, parents, startAttributes, endAttributes, events, status}}})`），属性定义带类型、是否敏感、基数（low/high）。
- 一致性测试（`testing/conformance.ts`）对任意实现跑同一套断言 → 换实现（OTel 等）不需要改调用方。
- 实际落地程度有限（`harness.md §5.8` 的 T1 缺口）：声明了 `pi.harness.*` / `pi.ai.request` / `pi.session.write` 等 span，但生产只启动了 `pi.harness.hook`（且仅对注册了 before/after_tool 钩子的场景），RPC 只有请求级取消、没有 trace 传播。
- 遥测的**内容边界**写得很清楚：属性只允许 id/名称/计数/时长/状态/用量，**绝不允许** prompts、补全内容、工具参数/结果、文件内容、provider 载荷、headers、handle、凭据；事件与钩子（不进入遥测后端）可以带这些内容。

**附件与图像**

- 图像处理链路完整：MIME 检测、EXIF 方向校正、按模型约束自动缩放（`image-resize-worker.ts` 放到 worker）、非视觉模型的明确提示；工具结果里的图像也会被规范化（`normalizeToolResultImages`）。
- 大图/多图的上限与降级策略在工具层（`read` 工具带 `autoResizeImages`），并有 `show-images-selector` 之类的 UI 开关。

**大输出与产物**

- 工具输出截断 + spill 文件（§5.1）；`output-accumulator.ts`/`shell-output.ts` 负责"边跑边累积 + 按尾/头策略保留"。
- **会话导出**：`export-html`（自包含 HTML：`template.html` + `template.css`(22 KB) + `template.js`(79 KB) + ANSI→HTML 转换 + 工具渲染器）与 `exportToJsonl`；`session-share` 支持把当前分支导出后经 Radius 或 gist 分享。
- 用量汇总：`usage-totals.ts`（input/output/cacheRead/cacheWrite/cost 累加），UI 页脚与子代理渲染都用它。

### 11.2 agentxx

| 能力 | 现状 |
|---|---|
| 会话级数据寄存（检索替代） | `SessionShareStoreTool`（`agentxx_share_store`）：会话内 KV（id 自增 + `kCacheCapacity=3` 条 LRU 内存缓存，DB `store` 表为唯一数据源），用于"给模型/节点/skill/tool 之间传递数据、节省上下文"；工具输出超长时可改存到此处（避免占上下文）。它同时提供 summarization 句柄 |
| 语义检索 | `agentxx_rag_search` 插件（向量/关键词检索，依赖外部索引）、`agentxx_codegraph` 插件（代码图谱索引与查询）、`tool_skill_search`（工具/skill 延迟加载检索） |
| 历史检索 | 无（会话列表按标题/时间；无全文检索） |
| 遥测 | **无遥测框架**；只有日志（`XX_LOG*`）与基准统计（`AgentConfigStatic::enableBenchmark` 控制 TUI 帧耗时等全局标记，基准程序打开） |
| 用量统计 | `ContextStats`（上下文占用）+ `TurnEnd` 事件里的 `tps` + 会话 meta；**无成本账本**（§4.4-1 已提出） |
| 附件 | 图片/音频/视频三类，客户端能力表驱动；远程模式下"服务端页"列举与读取在服务端完成（`ListDir`/`read` 两路）；单文件与条数上限以常量固定 |
| 大输出 | 工具输出摘要压缩（§8.2）+ share store 寄存 + 展示历史分页拉取（`WireGetViewMessages`/`WireViewMessagesPage`，`fromIndex`/`totalMessages`）+ 服务端增量缓冲按字节上限裁剪（`estimateWireDeltaBytes`） |
| 导出/分享 | **无**（会话数据只在 SQLite 与会话目录里；没有 HTML/JSONL 导出，也没有分享流程） |
| 提示词优化 | **有**：`EvolutionTrainingAgent`（`lib/src/agent/training.cpp`，1518 行）——进化式训练模式：测试用例加载、LLM 优化器/变异器产出 prompt patch、按用例评分（自定义评分回调）、种群去重与备份轮转、变体落盘与加载 |

### 11.3 对比

| 维度 | agentxx | pi |
|---|---|---|
| 会话全文检索 | 无 | 设计完整（独立服务 + 拉取式索引 + 零权威投影），**未实现** |
| 语义/代码检索 | 有插件（RAG、codegraph） | 无内置（`grep`/`find` 工具 + 扩展可做） |
| 会话内数据寄存 | 有（`agentxx_share_store`，KV + LRU 缓存 + 溢出改存） | 无对应物（大内容靠工具截断 + spill 文件 + 文件系统） |
| 遥测 | 无框架（只有日志与基准标记） | 有契约 + 一致性测试 + 生成式 schema 文档，但实现覆盖很低（T1 缺口） |
| 用量/成本统计 | 零散（上下文统计 + tps），无账本 | 账本（`usage_ledger`）+ `getStats()` 投影 + `usage-totals` + 每次提交断言"统计=账本和" |
| 附件能力 | 三类媒体 + 客户端能力表 + 远程服务端自取 | 图像（EXIF/缩放/worker）+ 非视觉模型提示；音频/视频无 |
| 大输出策略 | 摘要压缩为主 + share store 寄存 + 历史分页 | 截断 + spill 文件为主（不改写内容）+ 工具检查点 |
| 导出/分享 | 无 | 自包含 HTML 导出 + JSONL 导出 + gist/Radius 分享 |
| 提示词优化 | **有进化式训练模式**（测试用例 + 优化器 + 评分 + 种群管理） | 无（`evals` 包只做文档/行为评测） |

**优点/缺点**

- agentxx 在**"数据存放与检索"上更偏实用**：`share_store` 解决了"大工具输出不该占上下文、但又需要被后续调用引用"的真实问题（pi 用文件路径 + spill + 模型自己再读一次）；RAG/codegraph 插件给了语义检索能力，pi 完全没有内置检索（其搜索仍是设计稿）。
- agentxx 在**可观测性与产物化上明显缺失**：没有账本（无法回答"这个会话花了多少"）、没有追踪（无法回答"这一轮慢在哪"）、没有导出（无法把一段会话分享给同事或存档）。这三项在 pi 分别由账本、遥测契约、HTML 导出承担，且都属可低成本补齐的项（账本已有明确落点；导出可先做"会话 → 自包含 HTML/Markdown"）。
- pi 的遥测设计有一个**值得直接抄的边界约定**：属性只允许 id/名称/计数/时长/状态/用量，禁止 prompts/工具参数/文件内容/凭据——这避免了"遥测变成隐私泄漏面"。agentxx 若加遥测，应先把这条写进文档。
- agentxx 独有的**进化式提示词训练模式**是 pi 没有的能力（pi 的 `evals` 只做评测与文档一致性检查）。这是 agentxx 的一个差异化优势，值得在文档中突出，并考虑把评价数据（账本 + 轮次记录）与训练流程打通。

### 11.4 可迁移到 agentxx 的设计

1. **遥测契约 + 内容边界（先定边界，再实现）**（P1）：定义极小的 `TelemetryContext`（`startSpan/addEvent/setAttributes/setStatus`）+ no-op 实现 + 一致性测试骨架；同时明确属性白名单（id/名称/计数/时长/状态/用量，禁止提示词、工具参数与结果、文件内容、凭据）。落地位置：新增 `lib/include/agentxx/util/telemetry.h`（或放 `cxx_utilxx_base`）；第一批埋点建议是 `turn`/`step`（modelcall 请求）/`tool`/`session.write` 四个 span，与 benchmark 的 `enableBenchmark` 开关共用。
2. **会话导出（HTML / Markdown / JSONL）**（P1）：把 `Session` 的展示历史（含工具折叠、中断卡片结果）导出为自包含 HTML 或 Markdown，作为分享/存档/问题复现材料。落地位置：`agent/client` 新增 `export_html`/`export_md` 命令（复用 TUI 的渲染路径生成静态快照），`lib` 提供 `SessionExport` 数据接口。这是低成本、用户感知强的一项。
3. **失败会话的取证包**（P2）：pi 有 `bug-report.ts`（收集诊断信息并生成摘要，可上传）；agentxx 可提供"一键导出诊断包"：会话历史 + 日志尾部 + 配置（脱敏）+ 环境信息 + 插件清单。落地位置：`client` 命令 + `AgentContext` 提供已加载组件清单（`collectAppendComponentInfo` 已有）。
4. **检索接入现有存储**（P2）：会话全文检索不必另建服务，可直接在会话库上做 FTS 或倒排（`view_message.json` 里已有文本），并明确"索引是投影、可重建"；跨会话检索用一个独立库（`{dataDir}/search.db`），按 `(sessionId, msgSeq)` 增量索引。
5. **用量账本暴露到 UI/插件**（P0，与 §4.4-1 合并）：账本落盘后，TUI 页脚显示本会话/本轮成本；插件经接口表读取（用于配额管理、训练评分、报表）。
6. **训练模式与账本打通**（P2）：把 §4.4-1 的账本与 §2.5-1 的轮次记录作为训练评分的输入（成本、耗时、失败率），使"提示词优化"不只按最终答案评分，也能按效率评分。

---

## 12. 客户端 UI 与渲染分层

### 12.1 pi：独立 TUI 库（差分渲染 + 自研布局）+ 应用层组件

**`packages/tui`（44 文件 / 16.9 k 行，仅 2 个依赖）**是一个可独立使用的终端 UI 框架：

| 能力 | 实现 |
|---|---|
| 渲染器抽象 | 共享 `TUI` 接口 + 两个实现：`TuiMainScreen`（写入主缓冲，保留终端回滚）与 `TuiAltScreen`（备用屏 + 固定高度视口 + 应用自管滚动） |
| 差分渲染 | 只更新变化的行（主屏）或视口行（备用屏）；首帧输出全部、宽度变化或"视口上方变化"时整屏重绘、其余情况移动到首个变化行后清屏重绘 |
| 原子刷新 | 每帧用**同步输出** `\x1b[?2026h … \x1b[?2026l` 包裹，避免闪烁/撕裂 |
| 组件模型 | `Component{ render(width): string[]; handleInput?; handleMouse?; invalidate() }`；**每行不得超过 width**（否则报错），并提供 `visibleWidth/truncateToWidth/wrapTextWithAnsi` |
| 布局 | `layout-node.ts`：`VStack`/`HStack` 条目支持 `basis/grow/shrink/minSize/maxSize` 与响应式 `visible`；`ScrollView` 拥有一个区域的滚动（`follow:"end"`、越界链式滚动 `overscroll:"chain"`）；**布局几何每帧重建，组件保留已渲染行缓存** |
| 浮层（overlay） | 锚点（9 种）+ 百分比定位 + `margin` + `minWidth/maxWidth/maxHeight` + `visible(termW,termH)` 响应式 + 焦点回退策略（`unfocus({target})`、不可见即释放焦点）+ 层级与 `hide/setHidden` |
| 鼠标 | 归一化事件（`press/release/move/drag/click/wheel` + 本地/屏幕坐标 + 修饰键 + 连击数 + wheelDelta）；处理结果显式声明 `{handled, capture, focus, render}`；未处理的手势回落到默认行为（滚动、主键拖选、OSC 8 链接、右键粘贴） |
| 文本编辑 | `Editor`（2112 行）：多行编辑、折行、自动补全（`/` 命令 + Tab 路径补全）、大段粘贴标记、`kill-ring`、`undo-stack`、`word-navigation`、点击定位光标 |
| 输入法（IME） | `Focusable` + `CURSOR_MARKER`（零宽 APC 序列）：组件在假光标处插入标记，TUI 扫描后把**硬件光标**移到该位置，使 CJK 输入法候选框位置正确 |
| 图像 | Kitty 图形协议 / iTerm2 内联图像；备用屏支持 Kitty 图像的**视口裁剪**，iTerm2 因协议无法删除/裁剪而在备用屏降级为文本占位 |
| 颜色 | 索引色 / sRGB / OKLCH（含 OKHSL）三态，`mixColors` 等颜色运算（文档明确提醒：OKLCH 转换较贵，热路径应转换一次复用） |
| 其他 | 备用屏搜索面板（可点击箭头、命中高亮、快捷键可配）、OSC 133 语义提示符跳转、滚轮 hover 展开/拖动滑块/点击轨道跳转、拖选 + OSC 52 剪贴板、Kitty 键盘协议键位匹配（`matchesKey/Key`）、`latex.ts`、`stdin-buffer.ts` |
| 测试 | `VirtualTerminal`（基于 `@xterm/headless`）可用作无终端环境下的渲染测试；`PI_TUI_WRITE_LOG` 可导出原始 ANSI 流 |

**应用层**（`coding-agent/src/modes/interactive/`，`interactive-mode.ts` 6250 行 + 约 50 个组件）：聊天视口、消息组件（assistant/thinking/tool/diff/compaction/branch summary）、会话树选择器（`tree-selector.ts` 1284 行）、模型/主题/设置选择器、按行差异渲染（`diff.ts`）、ANSI→HTML 导出、`theme/`（主题控制器 + JSON 主题 + 系统主题跟随）。

**模式并列**：`print`（一次性输出）、`json`（事件 JSONL）、`rpc`（JSONL 命令/事件，子进程形态）与 TUI 共用同一 `AgentSession`，因此"无界面模式"与"有界面模式"行为一致。

### 12.2 agentxx：FTXUI + 声明式 UI 描述 + 客户端唯一渲染实现

- **渲染底座**：FTXUI（第三方库）。`TUIClientAgentIO`（`agent_tui.cpp` 2804 行）负责线程（UI 线程 + client 线程，`TUISharedState::mutate()` 后每帧 `readSnapshot()` 无锁渲染）、协议处理（`onDelta/onSync/onPeerMessage`）与组件树装配。
- **组件拆分**：`MessageListComponent`（消息列表 + 折叠交互 + 懒加载滚动 `lazy_scrollable.cpp` 650 行）、`InputComponent`（输入栏 + 发送）、`StatusBarComponent`、`SidebarComponent`（侧边栏 tab + 拖拽 + 区域可见性上报）、Overlay 组件集（`overlays.cpp` 2178 行：模型选择、设置、会话选择、文件选择（含远程"本地/服务端"标签页）、待发送队列、上下文查看、快捷键列表、更新提示等）、`interrupt_view.cpp`（中断表单渲染）。
- **声明式 UI 描述层**：`cxx_pluginxx_ui` 库（`pluginxx::ui::Item`/`parse`/`adapt`/`plainText`，自带测试工程）定义与宿主无关的组件描述；`ui_components.cpp`（2210 行）是**唯一渲染实现**，把描述渲染成"行模型"（元素 + 行数 + 元素内可命中区域），并保证"高度估算走同一条渲染路径"（`measureItem`）。终端列宽与 GUI 逻辑像素的换算只在渲染时发生（描述里长度是 u，能力段声明 `cell`）。
- **命中与表单**：`ui_hit.h`（`UiHitRegion` 局部坐标）+ 滚动容器 `Scrollable::hitTestItem`（避免视口外子项的反射框残留）；表单状态由宿主维护（`UiFormState`），提交经动作通道回传 `__submit` + `{"values":{...}}`；中断表单与插件表单共用同一套渲染与交互。
- **客户端插件能力**：9 张接口表（ui/events/session/wire/self/json/log/timer/keybind）；插件可添加面板 tab、overlay、定时器（`pause_when_hidden`，动画等级 Disabled 时拒绝注册）、快捷键（冲突先注册者优先，`ClientKeybindConflict` 记录冲突）；可见性快照由 UI 每帧上报。
- **其它**：i18n（`tui_i18n.cpp`）、主题（`tui_theme.h`）、剪贴板复制（`copySelectionToClipboard`）、更新检查（GitHub releases，启动延迟 3s + 设置项控制）。
- 没有图像渲染（附件以卡片展示）、没有终端能力协商（Kitty 键盘/图形协议、OSC 52 等）、没有搜索面板与 OSC 133 跳转。

### 12.3 对比

| 维度 | agentxx | pi |
|---|---|---|
| UI 底座 | FTXUI（第三方，差分重绘） | 自研 `packages/tui`（差分渲染 + 布局引擎 + 视口），独立可复用 |
| 原子刷新 | 未使用 CSI 2026 同步输出 | 每帧 `?2026h … ?2026l` 包裹 |
| 渲染分层 | **描述（`pluginxx::ui`，与宿主无关）→ 行模型 → FTXUI 元素**，单一渲染实现 | 组件对象树（命令式），`render(width) → lines`，应用层自建组件 |
| 布局 | FTXUI 的 box/flexbox + 自定义懒加载滚动 | 显式 `VStack/HStack/ScrollView` 布局节点（basis/grow/shrink/minSize + 响应式 visible） |
| 浮层 | 自建 overlay 管理（逐个弹窗实现） | 通用 overlay（锚点/百分比/margin/响应式可见/焦点回退） |
| 鼠标 | 需要时经 `handleSidebarRegionClick` 等具体处理 + 命中区 | 归一化鼠标事件 + `{handled,capture,focus,render}` 结果语义 + 默认回落 |
| 组件可复用性 | 描述层可复用（GUI 客户端可共用），渲染实现绑定 FTXUI | 整个 UI 库可复用（独立包） |
| 输入法（IME） | 依赖 FTXUI 的假光标，未做硬件光标定位 | `CURSOR_MARKER` + 硬件光标定位（CJK 候选框位置正确） |
| 图像 | 无终端内联图像（附件卡片） | Kitty/iTerm2 内联图像（备用屏含 Kitty 裁剪） |
| 搜索/跳转 | 无 | 备用屏搜索面板 + OSC 133 提示符跳转 |
| 测试 | 组件层单测（`tui_ui_items`/`tui_form`/`tui_widget`/`tui_theme`）+ 基准（帧耗时/渲染字节） | `VirtualTerminal`（xterm headless）可用于无终端测试；文档要求组件缓存渲染行 |
| 多模式一致性 | TUI/CLI/server/ACP 共用 agent 与 wire | TUI/print/json/rpc 共用 `AgentSession` |
| 插件 UI | 声明式描述 + 客户端插件表（9 张） | 扩展直接拿到组件与 `ctx.ui`（进程内、命令式、同语言） |

**优点/缺点**

- agentxx 的"声明式描述 → 通用渲染"是**为跨客户端设计的**（描述层与宿主无关，TUI 只是第一个渲染实现），插件可以在不写任何渲染代码的情况下得到一致的界面；pi 的扩展是进程内 TS，直接构造组件，灵活但**与终端绑定**（RPC 模式下自定义组件不可用，只能走受限的 dialog/notify）。
- pi 的 TUI 在**工程细节**上更完整：原子刷新（无闪烁）、通用 overlay（含响应式可见与焦点回退）、IME 硬件光标（CJK 输入体验）、内联图像、搜索与语义提示符跳转、虚拟终端测试。这些是长期打磨的产物，agentxx 缺其中若干项，其中**IME 光标定位**与**原子刷新**对中文用户/长会话流式渲染影响最直接。
- agentxx 的布局能力依赖 FTXUI，缺少 pi 那种"区域可独立滚动 + 越界链式滚动"的布局语义；在"消息列表滚动 + 固定输入栏/侧边栏"这类场景下需要自己实现（已有 `lazy_scrollable` 与侧边栏拖拽），但缺少统一的布局节点抽象，新面板容易各写一套。
- 两边都把"渲染与业务逻辑分离"做得不错：pi 靠组件层，agentxx 靠描述层 + 单一渲染实现。

### 12.4 可迁移到 agentxx 的设计

1. **原子刷新（CSI 2026 同步输出）**（P1，成本低收益直接）：在每帧写入前后包裹 `\x1b[?2026h` / `\x1b[?2026l`（终端不支持时无副作用），消除流式长输出时的闪烁/撕裂。落地位置：`agent_tui.cpp` 的帧提交处（FTXUI 的 `Screen::ToString` 之后）。
2. **IME 硬件光标定位**（P1，中文用户直接受益）：输入栏渲染时把**硬件光标**移动到插入点（FTXUI 已有 `cursor_position`/`Cursor` 事件支持），使 IME 候选框出现在正确位置。落地位置：`components/input_bar.cpp` + `agent_tui.cpp` 的帧循环。
3. **统一浮层管理器**（P1，减少重复代码）：当前每个 overlay 各自实现尺寸/定位；建议抽出通用 overlay 管理（锚点/百分比/margin/响应式 `visible`/焦点回退/`hide` 与 `setHidden` 区分），并把现有弹窗迁移过去。落地位置：`framework/` 新增 `overlay_manager.h`，`overlays.cpp` 逐步迁移。收益：新面板（插件 overlay）自动获得一致的定位与焦点行为。
4. **终端能力协商**（P2）：启动时探测并缓存终端能力（truecolor/256 色、Kitty 键盘协议、图形协议、OSC 52 剪贴板、`?2026` 支持），并在描述层能力段里如实上报（已有 `tuiUiCapabilities`，可扩展为"终端能力 + 组件能力"两段）。落地位置：`agent_tui.cpp` 启动探测 + `ui_components.h` 的能力段。
5. **虚拟终端测试**（P1）：引入一个"把渲染输出喂给终端模拟器并断言最终屏幕内容"的测试通道，替代当前"逐组件断言行模型"的方式，能抓到跨组件、跨滚动、跨浮层的组合问题。落地位置：`agent/test/client` 新增模块（可用 `agent/lib` 已有渲染函数产出 ANSI 快照；如需真实模拟器可在测试侧引入轻量 VT 解析）。
6. **搜索面板与提示符跳转**（P2，体验项）：长会话中"在消息列表里搜索"与"跳到上一次用户输入"是高频需求；可在 `MessageListComponent` 上实现（搜索命中高亮 + 上下跳转 + 快捷键），对应 pi 的搜索面板与 OSC 133 跳转。

---

## 13. 远程协议、SDK 与外部集成

### 13.1 pi：两条远程面（CBOR RPC 服务 + JSONL RPC 模式）+ TS SDK

**面 A：`protocol` + `server` + `client` 三包（可独立部署的 RPC 服务）**

| 层 | 设计 |
|---|---|
| 线格式（`packages/protocol`，8 文件 / 790 行） | **4 字节大端长度前缀 + CBOR 载荷**；最大帧默认 16 MiB（可配，上限 uint32）；解码器是增量状态机，遇超长帧/截断帧立即失败并拒绝后续数据；协议版本常量（当前 8），`hello` 必须是客户端第一条消息 |
| 消息 schema | TypeBox 定义并**运行时校验**：`ClientMessage = hello \| request{id,target,call} \| cancel{id,target}`；`ServerMessage = hello \| hello_error \| response{id,ok,result\|error} \| service_update{subscriptionId,update} \| attachment{attachment\|null}`；`target = server{serverId} \| session{serverId,sessionId,attachmentId}` |
| 服务端（`packages/server`） | `Server`（连接生命周期：握手超时 5s、连接数/错误回调、`closing` 状态机、聚合关闭错误）+ `SessionRouter`（多会话路由：同一会话同一时刻只有一个宿主句柄；每连接一份 attachment，attachment 带随机 `attachmentId` 作为栅栏，路由前校验 `sessionId + attachmentId` 匹配）+ 传输（unix socket；testing 目录另有内存 host/client） |
| 客户端（`packages/client`） | 连接 + 请求关联（id ↔ promise）+ 订阅（`service_update` 流）+ unix socket + promise 小工具 |
| 特性 | 订阅式服务更新 + `cancel` 消息（**按 id 取消正在执行的 RPC**）+ attachment 变更推送 + 服务状态编码器（快照/增量）+ 断连时按序释放与会话清理 |

**面 B：`coding-agent` 的 RPC 模式（子进程 JSONL，给任何语言用）**

- `pi --mode rpc`：stdin 收命令（`rpc-types.ts` 中 `RpcCommand` 联合含 30+ 个命令，命令/响应/事件记录定义合计 79 个）、stdout 出 `response` 与**会话事件流**，另有扩展 UI 的一条双向记录；
- **严格 JSONL 分帧**：一行一个完整 JSON；文档专门警告不要用会按 `U+2028/U+2029` 断行的通用行读取器；stdout 只放协议记录、诊断走 stderr；要求双方处理背压；
- `prompt` 的响应只表示"已接受/排队/已处理"（`data.disposition`），**不代表完成**；完成要等 `agent_settled`（`agent_end` 之后还可能有重试、压缩、steering）；
- `RpcClient`（TS，子进程客户端）提供 40+ 强类型方法（prompt/steer/followUp/abort/compact/bash/fork/clone/switchSession/getTree/exportHtml…）+ `promptAndWait()` + `waitForIdle()` + 事件订阅；
- 扩展 UI 协议（`rpc-extension-ui.md`）：把 dialog/notify 等交互转发给 RPC 客户端，**自定义终端组件不可用**（模式差异被明确记录）。

**面 C：SDK（进程内嵌入）**：`createAgentSession(options)` 返回会话与 runtime，提供与 CLI 一致的会话能力（模型/工具/扩展/技能绑定、`subscribe` 事件、`reload` 等），并有 13 个 `examples/sdk/*.ts` 从最小用法演示到完全控制。

**实验性多进程形态**：`experimental/mini/`（server + worker + tui 三进程，自备 `shared/protocol.ts`/`rpc.ts`/`transport.ts`）、`experimental/session-worker*.ts`（把会话放进 worker，用 `AgentHarness` 驱动）。

### 13.2 agentxx：WS wire + 多协议内置 + FFI

| 面 | 现状 |
|---|---|
| 主协议（`wire_protocol.h`） | **JSON 文本 + 类型字符串**：`{"type":"<msgType>", "id":…, "sessionId":…, …}`。客户端 → 服务端：hello/user_input/interrupt_response/cancel/select_model/get_model/get_append_component_info/ping/compact_context/list_sessions/switch_session/clear_message_queue/remove_queue_item/interrupt_and_run_next/get_view_messages/list_dir/get_permission_state/set_full_auth 等；服务端 → 客户端：hello_ack/delta/sync/interrupt_request/interrupt_expired/turn_result/context_stats/error/log/model_info/append_component_info/get_context/context_messages/session_list/pong/plugin_data/plugin_data_up/message_queue_update/view_messages_page/list_dir_result/permission_state 等；高频路由用 `JsonView` 零拷贝提取 `type` 再物化 |
| 传输 | `ChannelIOTransport`（进程内）与 `WsIOTransport`（WebSocket，423 行）；`AgentIOBase` 端点抽象使 client/server 同构 |
| 会话互操作 | `Sync`（全量或尾窗，带 `fromIndex`/`totalMessages`/`deltaSeq`）+ `Delta`（seq 单调 + 客户端去重）+ 服务端重连重放缓冲（按字节上限裁剪）；`switch_session` 重绑并回推全量 |
| 服务模式 | `agentxx_cli server`（`--host`/`--port`，默认 `127.0.0.1:7007`）+ token 鉴权（URL 查询串或 `--token`）；无 PID/锁文件/控制通道 |
| 外部协议 | `mcp_client.cpp`（2171 行）/`mcp_server.cpp`（1571 行）：完整 MCP 客户端与服务端；`acp_server.cpp`（816 行）：ACP stdio 服务；`a2a_server.cpp`（781 行）/`a2a_client.cpp`：Agent2Agent |
| 语言互操作 | **FFI C API**（已实现并测试 `agentxx_test ffi_c_api`）：26 个顶层 C 符号、`#pragma pack(8)`、定长类型、`AGENTXX_FFI_CALL` 调用约定、字符串视图/跨堆字符串、错误双通道（同步返回码 + 异步事件）；每实例独占 Client 线程 + Agent 线程，内部经 `ChannelAgentIOTransport` 与 `SessionServerAgentIO` 通信（**与 TUI/CLI 完全同构，agent 核心零改动**），配套其他语言导出接口与示例 |
| SDK | 无官方 SDK；嵌入路径是 FFI 与插件 ABI |

### 13.3 对比

| 维度 | agentxx | pi |
|---|---|---|
| 线编码 | JSON 文本（`{"type":…}`） | CBOR 二进制 + TypeBox 运行时校验（面 A）；JSONL（面 B） |
| 分帧 | WebSocket 帧（面 A）；行分帧（stdio/ACP） | 4 字节长度前缀 + 最大帧限制（面 A）；严格 JSONL（面 B） |
| 版本协商 | 无显式协议版本（靠字段可选性兼容旧行为，如 `list_sessions` 分页字段缺省退化为全量列举） | `PROTOCOL_VERSION` + `hello` 版本校验 + `hello_error`（面 A） |
| 请求-响应关联 | 部分消息带 `id` + 事件流 | 严格 `id` 关联 + `cancel{id}` **请求级取消** |
| 会话栅栏 | `sessionId`（`switch_session` 重绑） | `serverId + sessionId + attachmentId` 三元组（attachment 为随机 id，防旧连接误操作新会话） |
| 会话路由 | 单端点单会话 | `SessionRouter` 多会话路由 + 引用计数 + 关闭聚合 + 宿主化接口（`resolveSession/openSession`） |
| 传输 | 进程内 Channel + WS | 进程内 channel + stdio + WebSocket + 控制 socket；client 包另有 unix socket；RPC 模式为子进程 stdio |
| 多语言集成 | MCP/ACP/A2A + FFI C API（26 符号，进程内嵌入） | JSONL RPC（任何语言、子进程）+ TS SDK（进程内）+ 13 个 SDK 示例 |
| 部署形态 | 单可执行文件 `server` 模式 + token | `protocol/server/client` 可独立部署（实验性 mini 演示三进程） |
| 扩展 UI 跨进程 | 插件 UI 描述经 wire 传输（`plugin_data` 等）并在客户端渲染 | RPC 扩展 UI 协议（仅 dialog/notify，自定义组件不可用） |
| 外部代理协议 | MCP（client+server）、ACP、A2A | MCP（作为工具来源）+ skills/扩展 |

**优点/缺点**

- agentxx 的**外部协议面更宽**：MCP 客户端与服务端、ACP、A2A 全部内置，加上 26 符号的 FFI C API，"被别的 agent 调用"与"嵌入到别的语言"都不需要额外组件。pi 的对外集成主要是"RPC 协议 + SDK + MCP 工具"，没有 A2A/ACP 服务端。
- pi 在**协议工程细节**上更严谨：CBOR（省带宽、天然二进制）、TypeBox 运行时校验（不可信边界校验、内部对象信任）、`attachmentId` 栅栏、请求级 `cancel`、连接分阶段（awaitingHello → handshaking → ready）+ 握手超时 + 关闭聚合错误，以及"prompt 响应 ≠ 完成"的明确语义。agentxx 的 JSON wire 没有统一 schema 校验层（靠手写 `fromJson`/`toJson`），`cancel` 是"取消本轮会话"而非"取消某个请求"，也没有显式协议版本字段。
- agentxx 的 FFI 方案（每实例双线程 + 通道 + 与 TUI 同构的 client 端点）设计干净：宿主语言只需处理 C 回调与字符串生命周期，"agent 核心零改动"这点值得保留并写进文档。
- pi 的"两条远程面"实际对应两种部署形态（独立服务 + 子进程/任何语言），文档清晰区分适用场景；agentxx 目前只有 WS 一种形态，做 IDE 插件这类集成需要适配 WS 而非标准输入输出。

### 13.4 可迁移到 agentxx 的设计

1. **协议 schema 与版本**（P0，成本可控）：为 wire 消息引入显式版本字段与 schema 校验（可在 JSON 之上加轻量校验表：字段类型/必填/枚举），握手时校验版本，不匹配返回明确错误而不是靠"字段可选性"兼容。落地位置：`wire_protocol.h`（加 `kProtocolVersion` + 每消息校验函数）+ `agent_io.cpp`/`session_server_agent_io.cpp` 入口校验。收益：客户端与服务端可独立升级，错误可定位。
2. **请求级取消**（P1）：现在 `cancel` 只取消当前轮次；建议支持 `cancel{id}` 取消某个挂起请求（如 `list_dir`、`get_context`、`get_view_messages`），避免长扫描请求长期占用连接。落地位置：`wire_protocol.h`（`cancel` 带 `id`）+ 服务端请求表（按 id 记录 cancel token）。
3. **会话栅栏（attachmentId）**（P1）：`switch_session` 重绑存在"旧界面操作新会话"的风险窗口；建议每次会话绑定一个随机 `attachmentId`，会话级请求必须携带，服务端不匹配即拒绝（缺失时按当前会话处理以保持兼容）。落地位置：`SessionServerAgentIO` 的绑定结构 + wire 字段。
4. **无界面/子进程集成形态**（P1）：提供 `agentxx_cli rpc`（stdio JSONL）形态，使 IDE 插件等集成方无需 WS 也能驱动；可复用现有 wire 消息，只换分帧方式。落地位置：`client` 新模式 + `AgentIOBase` 的 stdio 端点（已有 `agent_stdio` 可参考）。
5. **服务端部署化**（P1，与 §1.4-4 合并）：`--daemon`（PID/锁文件）、控制通道（列会话/连接/健康检查）、鉴权加强（token → 可选 TLS）。落地位置：`client/main.cpp` 的 server 模式 + `agent_server.cpp`。
6. **协议往返测试与生成式文档**（P1）：为每个 wire 消息补"序列化 → 反序列化 → 断言字段"的往返测试，并从消息定义生成文档片段（当前 `wire_protocol.h` 注释详尽但没有机器校验）。落地位置：`agent/test/client` 新增 `wire_protocol` 模块。
7. **"响应 ≠ 完成"的语义文档化**（P0，成本近零）：明确写出 `user_input` 响应的语义（已受理？已排队？已开始？），并在客户端实现里对齐（参考 pi 的 `disposition` 字段）。落地位置：`wire_protocol.h` 注释 + `SessionServerAgentIO::pushMessageQueueItem` 的响应路径。

---

## 14. 扩展机制对比

### 14.1 pi：三条扩展路径（进程内 TS 扩展、chord facet、声明式资源）

**路径 1：进程内 TypeScript 扩展（主产品形态）**

- **加载**：`jiti` 运行时加载（本地 TS 无需编译）+ 目录/包发现（个人 / 项目 / 命令行 `--extension`）+ 包管理器（npm/git）；`loader.ts`（710 行）负责发现与依赖解析，`virtual-modules.ts` 提供宿主模块的虚拟映射，`runner.ts`（1322 行）负责事件分发。
- **API**（`core/extensions/types.ts`，1723 行）：一个 `ExtensionAPI` 对象，能力面为：

| 能力 | 接口 |
|---|---|
| 事件 | `pi.on(event, handler)` → 返回取消订阅函数；处理按注册顺序，`on()` 的变更不影响正在进行的派发 |
| 工具 | `registerTool()`（TypeBox schema + execute + details） |
| 命令/快捷键/参数 | `registerCommand()`/`registerShortcut()`/`registerFlag()` |
| 消息 | `sendUserMessage()`/`sendMessage()`（后者可作为上下文内容发给模型） |
| 持久化 | `appendEntry()`（不进入模型上下文的数据） |
| 会话控制 | `setActiveTools()`、模型/thinking 切换、压缩、shutdown |
| provider | `registerProvider()`（自定义模型服务，配套 OAuth 类型） |
| UI | `ctx.ui`（dialog/notify/status/widget/title/editor/自定义组件）+ 渲染器注册 |
| 扩展间通信 | `pi.events`（事件总线） |
| 钩子点 | `session_start`/`session_shutdown`/`before_agent_start`/`agent_start`/`agent_end`/`agent_before_settle`/`agent_settled`/`message_end`/`tool_call`/`tool_result`/`provider_stream_event`/`context`/`context_with_system`/`turn_end`/`user_bash`/`project_trust`/`cache_warming_decision` 等 |

- **契约写得很细**（`docs/extensions.md`）：工厂函数里**不要**起进程/套接字/定时器（有些加载路径不起会话）；长生命周期资源在 `session_start` 起、在幂等的 `session_shutdown` 关；`context` 只变换消息、不动系统消息（要动整份转录用 `context_with_system` 且必须保持 system 在 0 号位）；`turn_end`/`agent_before_settle` 可追加条目并 `continue: true` 请求"再来一轮"（文档提醒必须加条件，否则会无限循环）；`tool_call` 处理失败会**阻止工具**（fail-safe），工具执行失败变成模型可见的错误结果；错误处理策略"报告并继续"。
- **可靠性**：进程内、与宿主同权限（文档明确"只从可信来源加载扩展"）；重载会替换整个扩展运行时（`await ctx.reload()` 之后再写的代码不得复用旧状态）。

**路径 2：chord（应用组合运行时，独立包）**

- 面向"一个功能需要跑在多处"（agent worker / TUI / 远程 WebUI）的场景：
  - **plugin/facet**：插件是同步 setup 单元，声明它提供的服务与需要的服务；所有插件声明完形状后，宿主校验依赖图、**先激活 provider 再激活 consumer**、并按依赖逆序释放；facet 是插件的可分离部分，各自打包并加载到对应进程/环境（后端、浏览器、TUI）。
  - **service**：类型化稳定 token，单例或按 key 多实例；可进程内（无限制 JS 契约）或远程可暴露；consumer 在 provider 断开/替换期间保留稳定 facade（`services/provider.ts`、`consumer.ts`、`state.ts`、`wire.ts`）。
  - **replicated state**：`change(context, cb)` 发布原子 overlay 事务，consumer 只收到完整不可变值；draft 代理仅在回调期间有效（`delta/tracker.ts` 2056 行 + `apply-immutable-trusted.ts`）；delta 追踪保留常见字符串/数组操作、支持 durable base 批次、**对不可信操作做应用期校验**；批次保证收敛但不是最小化。
  - **打包**：`bundler.ts` 用 esbuild 把每个 facet 单独打包；`node/bundle-loader.ts`（395 行）加载与校验；有 manifest/package 概念（可作为插件分发单元）。
  - **测试**：`chord/test` 覆盖 delta 一致性、服务替换、打包加载等。
- 定位明确："它不是 Pi 包，不依赖任何其他 Pi 工作区包，可被无关应用使用"。

**路径 3：声明式资源**：skills（`SKILL.md`）、prompt templates、themes、keybindings、`settings.json`；由 resouce-loader 统一发现，受项目信任控制。

### 14.2 agentxx：纯 C ABI 插件 + 接口表 + 生命周期契约

- **边界**：跨边界只能用 C API（不能传标准库结构体/复用代码的结构体），插件编译成动态库、按名导出入口符号；导出符号白名单控制（ELF version script / macOS `-exported_symbols_list` / MSVC 仅 `dllexport`），插件复用工具库时静态链接一份并由导出控制隐藏符号。
- **入口与生命周期**：`agentxx_plugin_agent_create/destroy` + **必备的 `start/stop`**：`create` 只构造上下文（不注册、不起线程），`start` 是注册事务（在宿主 IO 线程执行，失败返回 `NULL + error` 由宿主回滚），`stop` 撤销自管资源（线程/定时器/订阅，可重复），`destroy` 只释放本地对象；缺失 `start/stop` 的插件被拒绝加载。SDK 宏（`AGENTXX_PLUGIN_AGENT_EXPORT`）一次生成五个入口。
- **多实例三铁律**：禁止可变全局/函数级 static 缓存；实例状态只能放 `*plugin_ctx` 堆块（回调经 `spec.user_data` 恢复）；接口表查询结果存实例上下文。
- **接口表**（每张表首字段独立版本号 + 函数指针可为 NULL）：
  - agent 侧 **19 张领域表**：`tools`（注册/注销/互调 + op_cancel）、`permission`（声明式工具权限 + `check_paths` 三态批量查询）、`hooks`（7 个钩子点）、`events`（订阅/发布，topic 自动加 `plugin.` 前缀）、`capabilities`（能力注册/调用）、`scheduler`（`is_io_thread`/`post_to_io`/`sleep`/`op_cancel`/`offload`）、`coroutine_runtime`（通用协程驱动：driver ticket/wake 协议）、`session`（share store / 消息提示）、`context`（会话上下文按需查询：`get_messages`/`messages_count`）、`plugins`（列表/自述）、`config`（配置/插件参数/工具提示词/会话工作目录/语言）、`model`、`cancel`、`prompt`（提示词读写）、`json`、`log`、`resources`（skill 目录/memory 文件/MCP 服务器注册）、`graph`（注册节点类型、读写执行图 JSON）、`tasks`（后台任务托管：`register_task/cancel_task`，卸载时统一取消 + 精确等待 inflight 归零）；
  - 另有 10 张通用表（`pluginxx.*`：能力/事件/调度/任务/取消投递的通用实现）。
  - client 侧 **9 张表**：`ui`（状态项/面板/Info 段/命令/toast/工具装饰/工具渲染器/动作处理/overlay）、`events`、`session`（客户端状态快照、发送输入、请求取消）、`wire`（插件数据上行）、`self`、`json`、`log`、`timer`（一次性/周期 + 挂起策略）、`keybind`（全局快捷键）。
- **禁用/启用/卸载**：级联依赖处理、关闭超时重试、`detachAll` + `waitInflightZero`（避免协程帧悬挂）、注册随卸载自动撤销（工具权限/事件/定时器/快捷键）。
- **能力协商**：插件声明支持的接口名集合，宿主据此决定可用表（老宿主缺表时插件降级）；客户端 UI 能力段（`agentxx.client.ui`）同理用于组件降级。
- **代价**：插件作者用 C/C++ 写、需要交叉编译到目标平台、调试不如 TS 方便（无热重载）；但换来跨语言、可隔离（未来可放到独立进程）、显式版本协商与严格的 ABI 约束。

### 14.3 对比

| 维度 | agentxx | pi |
|---|---|---|
| 扩展载体 | C ABI 动态库（C/C++/Rust/… 皆可，只要导出 C 符号） | 进程内 TypeScript（jiti 运行时加载）+ chord facet（esbuild 打包） |
| 能力面 | 29 张表（19 领域 + 10 通用）agent 侧 + 9 张 client 侧，覆盖工具/权限/钩子/事件/能力/调度/协程/会话/上下文/图/任务/UI/定时器/快捷键 | 单个 `ExtensionAPI` 对象（事件/工具/命令/快捷键/消息/持久化/会话控制/provider/UI/扩展间通信） |
| 生命周期 | `create`（只构造）→ `start`（注册事务）→ `stop`（撤资源）→ `destroy`；宿主可禁用/启用/卸载并级联处理依赖 | 工厂函数（同步注册）→ `session_start`（起长资源）→ `session_shutdown`（幂等关；重载替换整个运行时） |
| 依赖装配 | 插件自行经能力表查询；无依赖声明 | chord：插件声明 provides/requires，宿主校验依赖图、按 provider→consumer 顺序激活、逆序释放 |
| 状态共享 | 能力调用（`invoke_capability_async`）+ 事件总线 + share store | chord：类型化 service（单例/keyed、本地/远程）+ 复制状态（原子 overlay + 增量 delta） |
| 跨进程扩展 | 天然支持（插件在宿主进程内，但 ABI 约束使其可移到独立进程；client 插件本身在客户端进程） | facet 按环境分别打包与加载（进程/浏览器/TUI 各自一份），但都是 JS |
| 版本协商 | 每张表独立版本号 + 函数指针可空 + 插件声明支持表 | 无版本协商（内部 monorepo 版本一致；第三方扩展随包发布） |
| 隔离性 | 同进程（与宿主同权限）；ABI 边界严格（结构体对齐/定长类型/调用约定/符号白名单） | 同进程同权限（文档明示只加载可信扩展） |
| 分发 | 各平台动态库（每平台/架构一份） | npm/git 包（跨平台同一份 JS），可依赖 npm 生态 |
| 热重载 | 禁用/启用即时生效；卸载后重新加载 | `ctx.reload()` 替换整个扩展运行时 |
| 声明式资源 | skill/memory/MCP/提示词（`resources` 表注册） | skills/prompt templates/themes/keybindings/settings（受项目信任控制） |

**优点/缺点**

- agentxx 的插件模型在"**边界与演进**"上更强：纯 C ABI、每表独立版本、可空函数指针、能力协商、多实例约束、禁用/卸载的资源回收（含 inflight 精确等待），且天然支持用任何语言写插件。代价是开发体验（交叉编译、无热重载、调试成本）与"每平台一份二进制"的分发负担。
- pi 的扩展模型在"**开发体验与生态**"上更强：TS 直接写、npm 分发、进程内可以直接操作组件树与 UI；`chord` 更进一步提供了依赖声明、服务替换（provider 断开时 consumer 保留 facade）、复制状态与 delta 同步这些 agentxx 完全没有的机制。代价是不可隔离（与宿主同权限）、无版本协商（依赖"同一 monorepo 版本"）、以及自定义 UI 组件在 RPC 模式下不可用。
- 两边都明确了"只加载可信扩展"的责任边界；agentxx 通过 ABI 约束与符号控制降低了"插件破坏宿主符号空间"的风险，pi 通过 monorepo 依赖管理降低了"依赖漂移"风险。
- agentxx 的 `graph` 表（插件可注册节点类型、改写执行图）是 pi 没有的能力：扩展不只是"加工具/加钩子"，还能**改控制流本身**。这是 agentxx 架构的一个独特优势（同时也意味着图定义的正确性需要校验与回退——`BaseAgent::init` 已有"图构建失败回退默认图"的实现）。

### 14.4 可迁移到 agentxx 的设计

1. **依赖声明（provides/requires）**（P1）：借鉴 chord 的装配模型：插件可在 manifest 里声明"我提供什么能力/我依赖什么能力"，宿主在加载阶段校验依赖图、按 provider→consumer 顺序启动、卸载时逆序停止并**保留 consumer 的 facade**（provider 短暂不可用时 consumer 不必感知）。落地位置：`plugin_manager_lifecycle.cpp`（拓扑排序）+ manifest 扩展字段 + `capabilities` 表增加"依赖查询"。
2. **扩展生命周期事件补齐**（P1）：pi 的 `session_start`/`session_shutdown`/`agent_settled` 等事件让扩展有明确的"起/停/已稳定"时间点。agentxx 已有 start/stop 与 `onTurnBegin/onTurnEnd`，建议补"会话开始/结束"与"agent 稳定（本轮所有异步工作结束）"两类事件，便于插件管理会话级资源。落地位置：`events` 表新增 topic + `BaseAgent` 在会话创建/销毁与轮末稳定点发布。
3. **插件参数与配置路径的规范化**（P2）：pi 用 `settings.json` + 项目信任控制配置来源；agentxx 已有 `get_plugin_args`/`get_plugin_config_path`，建议补"插件级设置项"的读写接口（插件可在设置弹窗中声明自己的设置项），与现有设置 KV 打通。落地位置：`client` 设置弹窗 + `config` 表扩展。
4. **UI 组件能力协商已有，建议补"终端能力段"**（P2，与 §12.4-4 合并）：插件目前按"客户端组件能力"降级；再补"终端能力"（颜色/图形/同步输出）后，插件可对"终端是否支持图片/超链接"做决策。
5. **扩展的"可观测自述"**（P2）：pi 的扩展有明确的注册登记（工具/命令/快捷键可在 UI 里列出并显示归属）；agentxx 的快捷键列表已有归属显示，建议把"插件注册的工具/命令/面板/定时器"也纳入统一的"插件贡献清单"（`list_plugins` 扩展字段），便于用户审计。
6. **服务替换与复制状态（长期）**（P2）：若将来要做"多客户端协同"（同一会话在 TUI 与 Web 同时查看），chord 的 replicated state + 类型化 service + 依赖装配是一套现成参考模型；短期不建议引入，但值得在设计文档里留出"会话状态可复制"的方向（当前 `Sync`/`Delta` 已具备最小复制语义）。

---

## 15. 测试与质量门禁

### 15.1 pi：逐包测试 + 一致性框架 + 假 provider + 文档评测

**测试规模与组织**：675 个测试文件 / 154 k 行；每个包有自己的 `vitest.config.ts`（`packages/tui` 用 `node:test`）；根 `test.sh` 在**隔离环境**里跑全部测试（临时 HOME/TMPDIR/XDG/Git 配置，清空 API key 相关变量，`PI_NO_LOCAL_LLM=1`，只保留平台必需的少数变量，且带"只删除自己创建的临时目录"的校验），默认跳过依赖真实 LLM 的测试。

**五类关键测试手段**：

| 手段 | 说明 |
|---|---|
| 一致性测试框架 | `durable/testing/storage-conformance.ts`（1451 行）对 Memory/JSONL/SQLite 三个后端跑**同一套断言**；`agent/src/harness/session/testing/conformance/{storage,session-repo}.ts` 对会话/仓库接口跑同一套断言（含 `benchmark/`、`instrumented-storage.ts` 装饰器、`gating-storage.ts` 可控闸门） |
| 假 provider | `ai/providers/faux.ts`（651 行）可编程生成流/错误/延迟；`coding-agent/test/suite/harness.ts` 用它做会话级端到端；文档明确规定 `test/suite` **不得用真实 provider 与付费 token** |
| 测试用装饰器 | `instrumented-storage.ts`（记录每次 commit 的写序列，用于断言**写顺序**）、`gating-storage.ts`（把提交挂起，用于强制交错竞态的两个顺序）、`storage-decorator.ts`、`loaded-footprint.ts`（内存占用测量） |
| 竞态目录 | `harness.md §9.2` 列出约 27 组竞态并**要求两个顺序都测**（如 `requestAbort` vs 结算、检查点 vs 工具结算、abort vs 结构提交、观察者注册 vs 状态发布）；`§9.3` 定义三层测试（状态与驱动 / 写者一致性 / 确定性交错） |
| 文档与行为评测 | `evals` 包：`documentation-audit.eval.ts`、`models.docs.eval.ts`、`openai-provider.docs.eval.ts`、`custom-provider.docs.eval.ts`、`extensions.docs.eval.ts`、`tui.docs.eval.ts`（用 LLM 检查文档与实现是否一致，需显式启用） |

**质量门禁（`npm run check`，`AGENTS.md` 要求零错误零警告零 info）**：biome（lint + format，`--error-on-warnings`）+ `tsc --noEmit` + 一组**自定义检查脚本**：`check-pinned-deps`（直接依赖必须精确版本）、`check-runtime-deps`、`check-ts-imports`（只允许可擦除 TS 语法/import 形式）、`check-entry-graphs`、`check-shrinkwrap`（生成并比对 npm 打包依赖锁定文件）、`check-install-lock`、`check:browser-smoke`。另有 GitHub Actions 的定时 `npm audit --omit=dev` 与签名校验。

**其它工程约束**：`packages/ai/src/models.generated.ts` 禁止手改（改生成脚本）；`pre-commit` 阻止 lockfile 变更（除非显式环境变量）；多个 pi 会话可能在同一工作目录并行修改（`AGENTS.md` 明确给出"只提交自己改的文件"与一堆禁令）。

### 15.2 agentxx：自研多模块测试 + 资源基准 + 插桩

- **`agentxx_test`**：单可执行、多模块（本次源码清点约 80 个 `run(...)`/`runSync(...)` 注册项），支持 `--fail-fast`/`-f`、按模块名筛选（如 `agentxx_test string_util regex`）；模块覆盖：工具库（string/regex/json/json_view/diff/aho_corasick/util_misc）、事件与并发（events/concurrency/event_stream/event_bridge/interrupt_bus/subagent_bus）、agent 核心（agent/summarization/cancel/message_supplement/checkpoint_store/session_persistence/memgrowth/share_store/subagent_tool/agent_host）、插件（plugin_runtime/plugin_sdk/plugin_bridge/plugin_resources/plugin_multi_instance/client_plugins）、协议（mcp/acp/a2a/openai_provider/anthropic_provider/http/websocket/network_timeout/remote_agent）、客户端（tui_* 约 15 个模块：输入/中断/滚动/懒加载/侧边栏/表单/流式/主题/工具头/组件项/widget/surface）、UI 描述层（ui_items/interrupt_ui）、工具（string/math/datetime/filesystem/command/worktree/cpu_gpu）、FFI（ffi_c_api）、配置（config_loader/settings_db/tui_settings）、其它（training/update_check/markdown/ftxui_text…）。
- **Debug 插桩**：`AGENTXX_ENABLE_SANITIZER`（默认 ON）同时启用 ASan + UBSan，并为插件框架开定向探针（仅非 Release 生效，因为探针需要 UBSan 运行库符号）。
- **`agentxx_benchmark`**（一般仅 Release 编译）：资源基准模块（同进程 CLI/TUI、真实两进程、真实 TUI 界面线程、真实 server 单独运行、PTY 驱动的 TUI 子进程、插件逐项边际内存），指标覆盖 RSS/PSS/私有脏页/匿名/峰值/线程/fd + glibc 堆在用与碎片 + `malloc_trim` 可回收 + smaps 模块级分解 + 逻辑内存 + 分阶段增量 + CPU 时间，输出 JSON（机器对比）与 Markdown（人工阅读），支持 `--baseline` 对比上次。
- **构建/发布**：脚本化（linux/windows/macos/android），Release 默认 LTO + ICF，strip 由发布脚本负责。
- **缺口**：没有"跨后端/跨装配一致性"的测试框架（只有一个 SQLite 后端）；没有假 provider（会话级测试大多要连真实 API 或用桩）；没有对"写顺序/竞态两个顺序"的系统化测试；文档与实现的一致性靠人工。

### 15.3 对比

| 维度 | agentxx | pi |
|---|---|---|
| 测试规模 | 170 文件 / 75.5 k 行测试代码；约 80 个测试模块 | 675 文件 / 154 k 行 |
| 组织方式 | 单一可执行 + 模块注册表 + 名称筛选 | 每包独立 vitest/node:test 配置 + 根 `test.sh` |
| 隔离性 | 各模块自建临时目录；无统一环境净化 | `test.sh` 建临时 HOME/TMPDIR，清空 API key，固定 Git/npm 配置，带删除校验 |
| 假 provider | 无（部分测试用桩 provider 注入） | 有（`faux.ts` + `test/suite` 规定不用真实 provider） |
| 一致性测试 | 无框架 | 存储三后端 + 会话/仓库接口各一套 conformance |
| 竞态测试 | 靠单体用例覆盖具体场景 | 竞态目录 + 测试装饰器强制两个顺序 |
| 写顺序断言 | 无 | `instrumented-storage` 装饰器记录提交写序列 |
| 内存基准 | 强（`agentxx_benchmark`：RSS/PSS/smaps/堆碎片/模块级分解/基线对比） | 有 `agent/benchmark/session/*`（含 loaded-footprint/allocation-profile 测量） |
| 文档一致性检查 | 人工评审 | `evals` 包（LLM 检查文档与实现，需显式启用） |
| 静态门禁 | 编译器警告 + ASan/UBSan；无格式强制（约定不自动跑 clang-format） | biome + tsc + 7 个自定义脚本（依赖锁定/导入形式/入口图/shrinkwrap/browser smoke） |
| 依赖治理 | 第三方库以子模块/预编译形式内置 | 直接依赖精确版本 + 生成式 shrinkwrap + 定时 audit + pre-commit 拦 lockfile |

**优点/缺点**

- pi 的测试体系有两个 agentxx 目前缺的关键能力：① **一致性框架**（同一语义对多后端/多装配跑同一套断言，换实现即回归）；② **可控竞态与写顺序断言**（用装饰器/闸门强制交错，而不是"碰运气跑到"）。这两者对"并发 + 持久化"类代码的价值极高。
- agentxx 的**资源基准**明显强于 pi：模块级内存分解（smaps）、glibc 堆碎片、基线对比、真实 TUI/真实两进程/PTY 场景——pi 的基准集中在会话存储与加载占用，没有 agentxx 这种"整机资源画像"。这是 agentxx 应保留并继续投入的差异化能力。
- pi 的质量门禁是"自动化 + 可执行"的（依赖版本、导入形式、入口图、打包锁定文件全部脚本校验），适合 11 包的仓库规模；agentxx 靠编译器警告与人工评审，适合单仓小团队，但缺"依赖/配置漂移"的自动拦截。
- 两边对"测试不得依赖真实付费服务"的取向一致（pi 明确禁止，agentxx 通过桩），但 pi 的假 provider 让"会话级端到端"能在 CI 里稳定跑，agentxx 目前这类测试较少。

### 15.4 可迁移到 agentxx 的设计

1. **一致性测试骨架**（P0，收益长期）：为"可替换实现"抽出 conformance 断言（当前最直接的对象是会话存储：未来若加内存/文件后端，或把 `share_store`/`settings_db` 抽象成接口）。落地位置：`agent/test/include/agentxx-test/core/conformance_*.h`（模板函数，接受接口实现）。
2. **可编程假 provider + 会话级端到端**（P0，与 §7.4-4 合并）：实现 `neograph::Provider` 的测试桩（脚本化流/错误/延迟），写出覆盖"工具循环、重试、压缩、中断、取消、恢复"的端到端用例。落地位置：`agent/test/core/fake_provider.*` + 复用 `ModelProviderRegistry::setProvider`。
3. **可控闸门与写顺序断言**（P1）：为会话存储加一个测试用装饰器：① 记录每次提交的写序列（断言"先写 A 再写 B"）；② 能在指定提交处挂起（构造"提交中途到达别的操作"的交错）。落地位置：`agent/lib/src/agent/session_store.cpp` 抽出 `ISessionStore` 接口 + 测试实现。
4. **竞态清单化**（P1，成本低）：把已知并发点写成一份清单（取消 vs 工具结算、取消 vs 中断恢复、切换会话 vs 活动轮次、插件禁用 vs 进行中调用、同步 vs 增量、落盘节流 vs 轮末权威保存），每条要求"两个顺序都测"。落地位置：`docs/zh-cn/design/index.md` 增加"并发与竞态"章节 + 对应测试用例。
5. **测试环境隔离**（P1，成本低）：借鉴 `test.sh` 的做法——在测试入口创建临时 `HOME`/`TMPDIR`/`XDG` 并清空凭据类环境变量，避免测试读写用户真实配置与会话数据（agentxx 的 `dataDir` 与 `agentxx-config.yaml` 默认落在工作目录/用户目录，测试若不隔离会污染真实数据）。落地位置：`agent/test/test.cpp` 启动时设置环境变量或在脚本层隔离。
6. **门禁脚本化**（P2）：把"依赖/配置漂移"做成可执行检查：如"插件声明的平台支持与实际 CMake 一致"、"wire 消息类型常量与文档一致"、"配置项在 yaml 示例与代码中一致"。落地位置：`agent/script/` 增加 `check_*.py`/`check_*.ps1`（或放进 `agentxx_test` 的 `meta` 模块）。

---

## 16. 配置、设置与装配

### 16.1 pi：分层设置 + 项目信任 + 迁移脚本 + 包管理器

| 件 | 说明 |
|---|---|
| 设置来源 | 个人（`~/.pi/agent/settings.json`）与项目（`<project>/.pi/settings.json`）；**项目覆盖个人，资源列表则合并**；`defaultProjectTrust` 只能在个人设置里改 |
| `SettingsManager`（1224 行） | 类型化的 `Settings` 接口（模型/thinking、交互、工具、会话与上下文、TUI、终端、图像、markdown、警告、包来源等分组），带 `SettingsScope`、`SettingsStorage` 抽象（文件/内存两种实现，便于测试）、JSON schema 校验、诊断收集（`settings-diagnostics.ts`） |
| 资源定位 | `config.ts` 是**唯一入口**（`getAgentDir()`/`getPackageDir()`/`expandTildePath()`），禁止业务代码直接用 `__dirname`——这让源码检出、npm 安装、独立二进制三种形态都能正确定位资源 |
| 项目信任 | `ProjectTrustStore` + 启动期询问；未信任的项目：不加载项目设置/资源/扩展；`hasTrustRequiringProjectResources` 决定是否需要询问 |
| 迁移与弃用 | `migrations.ts`（`runMigrations` + `showDeprecationWarnings`）+ `utils/deprecation.ts`；changelog 规范要求把 Breaking/Added/Changed/Fixed/Removed 写在 `## [Unreleased]` 下 |
| 包管理器 | `package-manager.ts`（2434 行）+ `package-manager-cli.ts`（992 行）：从 npm/git 安装扩展与资源包（`packages.md`），有 `pi-manifest.ts` 描述包内容；安装走 `--ignore-scripts` 等安全默认值 |
| 模型/作用域 | `model-resolver.ts`（704 行）：`--models`/`enabledModels` 模式解析、按会话作用域模型（`scopedModels`）；`model-registry.ts`、`model-runtime.ts` 管理可用模型与刷新；`provider-composer.ts` 组合 provider（含自定义 provider） |
| 其它配置面 | 环境变量（`environment-variables.md`）、键位（`DEFAULT_EDITOR_KEYBINDINGS`/`DEFAULT_APP_KEYBINDINGS`，**不允许硬编码键位检查**）、主题（JSON 主题 + 系统主题跟随）、CLI 参数（`cli/args.ts`） |
| 特性开关 | `experimental.ts` + `--extension`/`--mode` 等；`AGENTS.md` 要求"新功能优先做成可配置项而不是硬编码" |

### 16.2 agentxx：两段 yaml（base + overlay）+ `.env` + 设置 KV

- **配置分层**（`config_loader.cpp` 1455 行 + `client/main.cpp`）：
  - **overlay**：`--config` 指定（默认 `工作目录/agentxx-config.yaml`）及其同目录 `.env`；
  - **base**：overlay 里 `data_dir` 指向目录下的 `agentxx-config.yaml` / `.env`（未配 `data_dir` 时取系统数据目录 `~/.agentxx/`）；
  - 合并：标量/映射逐键覆盖；列表段 `{overwrite:{mode:merge|replace, remove:[...]}, list:[...]}` 支持按键归并与剔除（模型按 name、插件按 path/name、MCP 按 namespace、路径列表按字符串）；`.env` 同名变量取 overlay 值；旧键/旧写法告警并忽略。
- **`AgentConfig`**（`lib/include/agentxx/agent/config.h`）是装配清单：模型（`model`/`availableModels`/`currentModelName`/`subagentModel`/`websearchModel`，每个 `ModelConfig` 含 baseUrl/apiKey/timeout/上下文上限/连接池/扩展请求参数）、权限（`permissionMode`/白名单/黑名单）、压缩（`enableSummarization`、输出长度阈值、阈值比例）、子代理（`enableSubagent`、`maxDepth`、`maxConcurrentSubagents`）、工具（`enableToolFiltering`/`toolWhitelist`、`toolTimeout`、`toolcallRepeatCheckThreshold`）、重试（`llmMaxRetry`）、存储（`enableSessionStore`/`dataDir`/`sessionStoreDirectory`）、工作目录（`workDir` + `resolvedWorkDir()`）、插件列表、资源（skill 目录/memory 文件/MCP）、提示词（`AgentPrompt`：systemPrompt + appendSystemPrompts + toolPrompt）、语言、日志与调试开关。
- **运行时设置（KV）**：客户端设置弹窗项落 SQLite（`settings_db`，如 `tui.lang`、`tui.checkUpdateOnStartup`、动画等级、思考展示模式等），分组为 界面/显示/更新/其他；设置项支持即时生效（主题、语言、动画等级）。
- **插件配置**：插件经 `get_plugin_args`/`get_plugin_config_path` 读取 yaml 中 `plugins[].config`（可指向文件或目录）；插件可自行解析（如 JSON/YAML）。
- **资源定位**：`dataDir` 派生出 `{dataDir}/sqlite/sessions/`（会话库）、codegraph 索引、设置库等；未配置 `dataDir` 时全部退化为内存并给出警告（`init()` 里输出，避免构造函数早于日志 sink 导致警告丢失——这个细节与 pi 的 `config.ts` 单一入口是同类问题的不同解法）。
- **CLI**：`agentxx_cli [mode] [options]`（tui/cli/server/acp/train + `--config`/`--env`/`--agent`/`--token`/`--model`/`--host`/`--port`）。
- **无迁移机制**：yaml 结构变化靠"旧键告警并忽略"；设置 KV 无版本号。

### 16.3 对比

| 维度 | agentxx | pi |
|---|---|---|
| 配置形态 | yaml（base + overlay 两段）+ `.env` | JSON 设置（个人 + 项目两段）+ 环境变量 + CLI 参数 |
| 覆盖规则 | 标量/映射覆盖；列表段支持 merge/replace/remove（按键身份） | 项目覆盖个人（资源列表合并） |
| 运行时设置 | 设置 KV（SQLite，TUI 弹窗即时生效） | `settings.json` 直接编辑 + 设置弹窗写入（`SettingsManager` 类型化 + 校验） |
| 资源定位 | `dataDir` 派生 + 显式路径 | `config.ts` 单一入口（适配源码检出/npm/独立二进制） |
| 项目信任 | 无 | 有（未信任不加载项目设置/资源/扩展） |
| 迁移/弃用 | 旧键告警并忽略 | `migrations.ts` + 弃用告警 + changelog 规范 |
| 包管理 | 无（插件是本地动态库，配置里写路径） | 有（npm/git 包管理器 + manifest + 安装安全默认） |
| 模型配置 | yaml 中的 `availableModels`（手工维护） | 生成式目录 + `enabledModels` 模式 + 按会话作用域模型 |
| 校验 | 启动期检查（工具 schema、提示词条目），配置项类型靠解析 | JSON schema 校验 + 诊断收集 + 类型化接口 |
| 键位/主题 | 主题内置 + 键位在客户端（冲突有记录） | 键位默认表可配置 + JSON 主题 + 系统主题跟随 |

**优点/缺点**

- agentxx 的 `base + overlay` 分层对"嵌入式部署"很实用（宿主的默认配置在 data_dir，每个项目/会话可覆盖），列表段的 merge/replace/remove 也解决了"叠加还是替换"的常见歧义；pi 的分层更简单（个人 + 项目），但配套更全（信任、迁移、包管理、schema）。
- pi 的**项目信任**与**迁移机制**是 agentxx 缺少的两块（§9.4-3 已把信任列为迁移项）；迁移机制在 agentxx 目前等价于"告警并忽略"，长期会导致配置兼容逻辑散落在解析代码里。
- pi 的**包管理器**让扩展/资源可以分发安装（npm/git），agentxx 的插件是"本地路径 + 平台动态库"，没有分发层——这与两种扩展模型的定位一致（§14），但可以考虑为内置插件做一个"可下载安装"的简化通道（例如从官方地址下载对应平台动态库到 `dataDir/plugins/`）。
- agentxx 的 `dataDir` 未配置即退化为内存 + 明确告警，是很务实的降级策略；pi 的资源定位靠单一入口函数 + 三种形态适配，两者解决的是同一个问题的不同侧面。

### 16.4 可迁移到 agentxx 的设计

1. **配置版本与迁移脚本**（P1）：在 yaml 里支持 `config_version`，加载时按需运行迁移（字段改名/结构升级），把"旧键告警并忽略"逐步替换为"明确迁移 + 告警"。落地位置：`client/src/config_loader.cpp` + 新增 `config_migrations.{h,cpp}`；设置 KV 同步加 `schema_version`。
2. **配置校验与诊断清单**（P1）：加载完成后产出"诊断清单"（未知键、类型不符、路径不存在、插件缺失、模型缺 apiKey 等）并在 TUI 里可查看（现在只有日志）。落地位置：`config_loader.cpp` 收集 + `client` 设置弹窗新增"诊断"入口。收益：用户能自己看清"为什么没生效"。
3. **资源定位单一入口**（P2）：把"配置目录/数据目录/工作目录/插件目录/技能目录"的解析集中到一个模块（供 agent、client、插件共同使用），避免各处自行拼接路径（当前 `dataDir`/`sessionStoreDirectory`/`resolvedWorkDir`/插件 `config` 路径分散在多处）。落地位置：`agentxx/util/paths.h` 或 `AgentConfigStatic`。
4. **插件分发通道（可选）**（P2）：为内置/第三方插件提供"从 URL 安装到 `dataDir/plugins/`"的简化通道（带校验和与平台匹配），并记录已安装清单；不引入包管理器，但必须有卸载与版本记录。
5. **项目信任**（P1，与 §9.4-3 合并）：与配置加载联动——未信任项目时不加载其 yaml/技能/记忆文件。
6. **设置项的插件贡献接口**（P2，与 §14.4-3 合并）：允许插件声明自己的设置项（类型/默认值/描述），由客户端设置弹窗统一渲染并落 KV，避免每个插件自建配置界面。

---

## 17. 迁移建议汇总

本节把前文所有"可迁移到 agentxx 的设计"按优先级归并。判据：**P0** = 成本低、收益立即可见、无架构依赖；**P1** = 需要少量设计或局部改造，但能在现有架构内落地；**P2** = 需要较大改造或属于方向性投入，建议先做设计与验证。

### 17.1 P0（建议优先落地）

| # | 迁移项 | 模块 | 落地位置 | 说明 |
|---|---|---|---|---|
| P0-1 | 多工具并行执行 | §5 | `nodes/toolcall.cpp::baseRun` | 源码已有 `// TODO: 真正并行`；改为并发收集 + 保持结果按 assistant 源顺序写入 |
| P0-2 | 用量账本 | §4/§11 | `session_store.cpp` 新表 + `nodes/modelcall.cpp` 结算点 | 每次 LLM 结算写一行（含失败/重试），提供聚合查询；UI/插件可读 |
| P0-3 | 重试可重试性分类 | §7 | `nodes/modelcall.cpp` + `config.h` 关键字配置 | 额度/计费类错误直接失败，不再做无意义退避 |
| P0-4 | 轮次记录与投递结果显式化 | §2 | `session_store.cpp` 新表 + `SessionServerAgentIO` 响应字段 | 每轮写不可变记录（状态/耗时/模型/用量/起止消息）；用户输入返回 `started/queued/rejected` |
| P0-5 | 假 provider + 会话级端到端测试 | §7/§15 | `agent/test/core/fake_provider.*` | 复用 `ModelProviderRegistry::setProvider`；覆盖工具循环/重试/压缩/中断/取消 |
| P0-6 | 一致性测试骨架 | §15 | `agent/test/include/agentxx-test/core/conformance_*.h` | 为可替换实现（会话存储、share store、设置库）抽出同一套断言 |
| P0-7 | 自定义条目 + 投影器 | §3 | `Session` 新增 custom 条目 + `modelcall` 组装阶段投影 | 插件/中间件私有的会话数据有地方放，并可控进入模型上下文 |
| P0-8 | 提示词片段化 | §6 | `prompt.cpp` + `context.cpp::buildSystemPrompt` | 有序命名片段 + 稳定排序，为"差异可见性/成本优化"打基础 |
| P0-9 | 责任边界与安全模型文档化 | §9 | `docs/zh-cn/design/index.md` 或新增 `security.md` | 明确"权限 ≠ 沙箱"、worktree 只挡写、符号链接已知缺口 |
| P0-10 | 协议 schema + 版本 + 响应语义 | §13 | `wire_protocol.h` + `agent_io.cpp` | 显式版本、字段校验、`user_input` 响应语义（已受理/已排队） |
| P0-11 | 核心库边界纪律 | §1 | `docs/zh-cn/design/index.md` | 明确"新能力优先落 `middlewares/`/`nodes/`/`plugins/` 或新库；`lib/src/agent` 只放骨架" |

### 17.2 P1（第二批）

| # | 迁移项 | 模块 | 落地位置 | 依赖/代价 |
|---|---|---|---|---|
| P1-1 | 压缩保留尾部 + 保留原文可回取 | §3/§8 | `summarization.cpp` + `Session` 上下文起点 | 依赖 P0-7（custom 条目） |
| P1-2 | 工具检查点与重放声明 | §4/§5 | `tools/tool.h`（`replayPolicy`）+ `toolcall.cpp`（恢复路径） | 需先定义"未完成工具"的恢复入口 |
| P1-3 | 子代理运行记录与后台委派 | §10 | `AgentHost::spawnOneTask` + 新表 + `subagent.cpp` 参数 | 后台委派需要完成通知与 UI 呈现 |
| P1-4 | 项目信任 | §9/§16 | 会话资源配置路径 + 客户端询问卡片 | 与配置加载联动 |
| P1-5 | 符号链接处理 | §9 | `middlewares/permission.cpp` 目标规范化 | 需评估 `weakly_canonical` 性能与 Windows 语义 |
| P1-6 | 配置版本与迁移、诊断清单 | §16 | `config_loader.cpp` + 设置 KV | 迁移脚本需逐版本设计 |
| P1-7 | 原子刷新 + IME 光标 | §12 | `agent_tui.cpp` 帧提交 / `input_bar.cpp` | 终端兼容性需降级处理 |
| P1-8 | 统一浮层管理器 | §12 | `framework/overlay_manager.h` | 迁移现有弹窗，工作量集中在回归测试 |
| P1-9 | 虚拟终端测试 | §12/§15 | `agent/test/client` 新模块 | 需要一个 VT 解析器（可自研轻量版） |
| P1-10 | 请求级取消 + attachment 栅栏 + stdio 形态 | §13 | `wire_protocol.h` + `SessionServerAgentIO` + `client` 新模式 | 栅栏需兼容旧客户端 |
| P1-11 | 遥测契约与内容边界 | §11 | `util/telemetry.h` + 埋点（turn/step/tool/session.write） | 先定属性白名单再实现 |
| P1-12 | 会话导出（HTML/Markdown） | §11 | `client` 命令 + `lib` 导出接口 | 复用 TUI 渲染路径生成静态快照 |
| P1-13 | 可控闸门与写顺序断言、竞态清单 | §15 | `ISessionStore` 接口 + 测试装饰器 + 设计文档 | 抽接口是主要成本 |
| P1-14 | 扩展依赖声明与生命周期事件 | §14 | `plugin_manager_lifecycle.cpp` + manifest + 事件表 | 需与现有 start/stop 语义对齐 |
| P1-15 | 上下文只追加规则 + 版本事件 | §3 | `modelcall.cpp` 断言/日志 + 新 WireDelta | 成本低，但与中间件写法约定相关 |
| P1-16 | 上下文片段差异可见性（工具集变化告知模型） | §6 | `modelcall.cpp` 组装阶段 | 依赖 P0-8 |

### 17.3 P2（方向性投入）

| # | 迁移项 | 模块 | 说明 |
|---|---|---|---|
| P2-1 | 会话 fork / 分支 | §3 | 先做"整会话复制 + 指定消息截断"，再考虑多分支 |
| P2-2 | 工具执行环境注入点（远端/容器） | §5/§9 | 需与插件模型协调（只隔离工具执行） |
| P2-3 | 周期/定时委派与任务依赖汇聚 | §10 | 需要落盘的任务元数据与有界补偿策略 |
| P2-4 | 检索（会话全文/跨会话） | §11 | 独立索引库 + 拉取式同步（零权威投影） |
| P2-5 | 插件分发通道与设置项贡献接口 | §14/§16 | 简化版安装/卸载 + 插件声明设置项 |
| P2-6 | 终端能力协商 | §12 | 颜色/图形/同步输出/剪贴板能力上报 |
| P2-7 | 延迟结果（deferred）通道 | §7 | 接入长任务型 provider 时再做 |
| P2-8 | 存储格式版本 + 迁移（会话库） | §4 | 与配置迁移合并推进 |
| P2-9 | 提示词训练与账本/轮次记录打通 | §11 | agentxx 独有能力的增强 |
| P2-10 | 会话状态复制（多客户端协同） | §13/§14 | 长期方向，chord 的 replicated state 是参考 |

### 17.4 建议的推进顺序（按依赖关系）

```text
第一批（互不依赖，可并行）
  P0-1 并行工具 → P0-3 重试分类 → P0-9 文档边界 → P0-11 库边界纪律
  P0-2 账本 ──┬─→ P0-4 轮次记录（共用"轮/结算"概念）
             └─→ 后续 P2-9 训练打通
  P0-5 假 provider ──→ P0-6 一致性骨架 ──→ P1-13 闸门/竞态
  P0-7 自定义条目 ──→ P1-1 压缩保留尾部 ──→ P2-1 fork
  P0-8 片段化 ──→ P1-16 差异可见性
  P0-10 协议版本/语义 ──→ P1-10 请求取消/栅栏/stdio

第二批（依赖第一批）
  P1-2 工具检查点/重放（依赖账本与轮次概念）
  P1-3 子代理记录/后台（依赖轮次记录与事件）
  P1-4 项目信任（依赖配置加载与询问 UI）
  P1-5 符号链接（独立）
  P1-6 配置迁移（独立）
  P1-7/P1-8/P1-9 客户端（UI 体验与测试）
  P1-11/P1-12 遥测与导出（独立，收益直接）
```

---

## 18. 反向清单：agentxx 不必照搬的设计

对比不只用于"学什么"，也用于"确认哪些差异是正当取舍"。以下 pi 侧做法不建议照搬到 agentxx：

1. **三代运行时并存**（§1.1）：pi 同时维护低层循环、durable harness、pico3 三套数据模型，导致 JSONL 持久化、压缩实现各写两遍，文档需要专门章节记录"哪些规格未实现"。agentxx 应保持**单一运行时 + 单一数据模型**，新能力通过中间件/节点/插件扩展，而不是另起一套会话模型。
2. **不内置权限系统**（§9.1）：pi 把安全边界完全交给容器的做法与其"运行在用户账户内、由用户监控"的定位一致，但 agentxx 已经把声明式权限、逐路径复核、完全授权做成了产品能力，这是资产而非负担；应做的是补齐"沙箱叙事"（明确边界）与符号链接缺口，而不是取消权限层。
3. **扩展与宿主同权限、同语言**（§14.1）：TS 扩展的体验优势明显，但"与宿主同权限 + 无版本协商 + 可任意改宿主状态"的组合在长期维护与安全上代价高。agentxx 的 C ABI 约束与能力协商应保留；可以借鉴的是**开发体验**（如提供更完善的 C++ 插件模板、SDK 头与示例）。
4. **JSONL 后端的物理不回收**（§4.1 的 J1 缺口）：长会话下文件只增不减，是已知设计债。agentxx 用 SQLite 即时回收页面，不需要复制这种取舍。
5. **完整的任务状态机（pico v3 全量）**（§10.1）：13 个操作状态 + 任务 role + permit + 观察者重建，对"单机 coding agent"属于过度设计。agentxx 只需取"任务记录 + 后台委派 + 依赖汇聚"三件，不必引入持久化的任务调度器与 permit 概念。
6. **把 TUI 做成独立包**（§12.1）：pi-tui 的独立性带来复用价值，但 agentxx 的 TUI 与声明式 UI 描述层深度耦合（`pluginxx::ui` + 唯一渲染实现），当前拆包收益有限。更值得做的是**渲染层与 FTXUI 的解耦**，而不是把 TUI 拆成独立工程。
7. **两条远程面（CBOR RPC 服务 + JSONL RPC 模式）同时维护**（§13.1）：两套分帧/两套消息定义/两套客户端会带来长期同步成本。agentxx 已有统一的 wire 消息，建议只加"分帧适配层"（WS / stdio 行分帧）而不是第二套协议。
8. **`compat.ts` 式的长期兼容层**（§7.1）：pi 为旧全局 API 保留兼容出口，会拖累迭代。agentxx 的插件表已有独立版本号与可空函数指针，兼容应通过"表版本 + 能力协商"表达，避免堆叠兼容分支。
9. **42 个 provider 目录全量照搬**（§7.1）：模型目录生成化是对的方向，但一次性支持 40+ 家 provider 与 OAuth 全家桶超出当前范围。建议先做"能力元数据 + 目录数据文件"，provider 实现按需增加。
10. **快照 + 事件折叠的双状态模型**（§3.3）：pi 需要"客户端把快照在事件上折叠出下一个快照"（`reduceLaneSnapshot`）才能保证一致，代价是 reducer 必须与服务端严格对齐。agentxx 的"单调 seq + 去重 + 全量 Sync 兜底"更简单，且已有实践检验；除非要做多客户端协同复制，不必引入 reducer 契约。

---

## 19. 结语

三条贯穿全文的原则：

1. **显式化胜过推断**。pi 值得学习的地方几乎都能归纳为一句"把隐含状态变成显式数据"：轮次是操作（有 id、有状态、有结果记录）、副作用有意向与结算、工具重放有声明、提示词变化是消息、会话栅栏是 attachment id、错误是可重试性分类。agentxx 的图引擎与中间件已经提供了很好的扩展点，缺口主要在"把这些运行时概念写进数据"。

2. **能力边界要写在文档里，而不只写在代码里**。pi 的 `SECURITY.md` 明确"没有沙箱"、`harness.md §0.9` 明确"哪些规格未实现"、`AGENTS.md` 明确"不要往 core 加代码"。agentxx 的设计文档很详尽（165 KB 的主设计文档），但缺少同样明确的"不做什么 / 已知缺口 / 责任边界"章节。这属于成本最低、收益最持久的一类改动。

3. **差异即取舍，先确定取舍再决定是否模仿**。两项目在权限、扩展模型、数据模型上的差异，多数不是"一边对一边错"，而是定位不同：pi 面向"多形态产品 + 丰富 provider + 进程内扩展生态"，agentxx 面向"单机多平台 + 严格边界 + 图可编程"。本文 §18 把这些差异逐条记下，是希望后续改动**不要无意中把某一侧的取舍当成另一侧的缺陷**去修。

最后，本文所有结论都以两侧源码为准；pi 的三代运行时并存意味着"文档描述的能力"与"主路径实际能力"可能不同（§2.2、§10.1 已逐条标注），引用时请注意区分。

---

## 附录 A：关键文件与文档索引

| 模块 | agentxx | pi |
|---|---|---|
| 主设计文档 | `docs/zh-cn/design/index.md`（165 KB）、`plugins.md`、`tui.md`、`benchmark.md`、`ffi.md` | `packages/agent/docs/harness.md`（211 KB）、`pico-v3.md`（153 KB）、`values.md`、`tool-durability.md`、`runtime-simplification.md`、`packages/coding-agent/docs/*`（40 篇） |
| Agent 循环 | `lib/src/agent/base_agent.cpp`、`agent_runner.cpp`、`nodes/modelcall.cpp`、`nodes/toolcall.cpp` | 第 1 代：`agent/src/agent.ts`、`agent-loop.ts`；第 2 代：`agent/src/harness/runtime/{drive.ts,lane.ts,drive/*.ts}`、`harness/agent-harness.ts` |
| 会话与上下文 | `lib/include/agentxx/agent/context.h`（`Session`）、`lib/include/agentxx/agent/conversation_types.h`、`lib/src/nodes/session_context.cpp` | `agent/src/harness/session/{types.ts,session.ts,values.ts}`、`harness/session/jsonl/*`、`packages/durable/src/**` |
| 持久化 | `lib/src/agent/session_store.cpp`（schema）、`checkpoint_store.cpp` | `agent/src/harness/session/{storage 后端}`、`packages/session-backends/sqlite-node`、`packages/durable/src/storage/*` |
| 工具 | `lib/include/agentxx/tools/tool.h`、`lib/src/nodes/toolcall.cpp`、`lib/src/tools/*` | `agent/src/harness/execution/{tools.ts,effect-gate.ts}`、`harness/tools/*`、`coding-agent/src/core/tools/*` |
| 提示词/技能 | `lib/src/agent/prompt.cpp`、`lib/src/agent/resource_applier.cpp`、`lib/src/middlewares/{skill,memory_file}.cpp` | `coding-agent/src/core/system-prompt.ts`、`agent/src/harness/system-prompt.ts`、`coding-agent/docs/skills.md` |
| LLM 层 | `lib/src/protocol/{openai,anthropic}_provider.cpp`、`lib/src/agent/model_registry.cpp` | `packages/ai/src/{models.ts,types.ts}`、`providers/*`、`api/*`、`utils/retry.ts`、`providers/faux.ts` |
| 压缩 | `lib/src/middlewares/summarization.cpp` | `agent/src/harness/compaction/*`、`coding-agent/src/core/compaction/*`、`coding-agent/docs/compaction.md` |
| 权限 | `lib/src/middlewares/permission.cpp`、`lib/include/agentxx/middlewares/permission.h` | `SECURITY.md`、`coding-agent/docs/containerization.md`、`examples/extensions/permission-gate.ts` |
| 子代理/任务 | `lib/src/agent/agent_host.cpp`、`lib/src/tools/subagent.cpp`、`lib/src/middlewares/subagent_manager.cpp` | `agent/docs/pico-v3.md`（§5–§8）、`examples/extensions/subagent/*`、`packages/chord/src/services/*` |
| 客户端 UI | `client/src/io/tui/*`、`client/include/agentxx-client/io/tui/*`、`third_party/cxx_pluginxx_ui` | `packages/tui/src/*`、`coding-agent/src/modes/interactive/*`、`packages/tui/README.md` |
| 协议/远程 | `lib/include/agentxx/agent/io/wire_protocol.h`、`session_server_agent_io.cpp`、`ws_io_transport.cpp`、`lib/src/protocol/{mcp,acp,a2a}_*.cpp`、`docs/zh-cn/design/ffi.md` | `packages/{protocol,server,client}/src/*`、`coding-agent/src/modes/rpc/*`、`coding-agent/docs/{rpc,rpc-commands,json,rpc-extension-ui}.md` |
| 扩展 | `lib/src/plugins/plugin_manager_*.cpp`、`docs/zh-cn/design/plugins.md` | `coding-agent/src/core/extensions/*`、`coding-agent/docs/extensions.md`、`packages/chord/*` |
| 测试 | `agent/test/test.cpp`（模块表）、`agent/test/include/agentxx-test/**`、`agent/benchmark/**` | 各包 `vitest.config.ts`、`durable/testing/storage-conformance.ts`、`agent/src/harness/session/testing/*`、`packages/evals/*` |
| 配置 | `client/src/config_loader.cpp`、`lib/include/agentxx/agent/config.h`、`lib/src/util/settings_db.cpp` | `coding-agent/src/core/settings-manager.ts`、`config.ts`、`trust-manager.ts`、`migrations.ts`、`package-manager.ts` |

## 附录 B：术语对照

| pi | agentxx | 说明 |
|---|---|---|
| Session（entry 树 + 值/列表 + 账本） | Session（展示历史 + typed 上下文 + 元数据） | pi 的会话是"树 + 可变状态"；agentxx 的会话是"两条线性数据集" |
| Branch / AgentLane | （无） | 分支 / 分支 + 配置 + 队列 + 一个操作 |
| Entry（message / compaction / branch_summary / custom） | ViewMessage（User/Assistant/Think/System/Tool/Interrupt/Tip）+ 上下文消息 | 展示与上下文在 agentxx 是分开的两套记录 |
| Operation（run / compaction / navigation） | 轮次（runTurnAsync）+ 中断恢复循环 | pi 把"一次工作"显式持久化 |
| Drive / accept / requestAbort / inspectExecution | `runTurnAsync` / `resume_async` / CancelToken | 推进、受理、取消、观察 |
| Effect gate | CancelToken 埋点 + 抛出/捕获 | 取消的准入控制 |
| Intent / settlement | （无显式对应） | 副作用的两段提交 |
| Tool checkpoint | （无） | 工具持久进度快照 |
| Hook（`before_run` 等 11 个） | 中间件（`onModelcallRunFunc`/总线服务） | 拦截点形态不同 |
| Facet / service / replicated state（chord） | 插件 + 接口表 + 事件总线 | 组合与依赖装配 |
| Extension（进程内 TS） | 插件（C ABI 动态库） | 扩展载体 |
| Skills / prompt templates / themes | Skill 中间件 / memory 文件 / 主题 | 声明式资源 |
| RPC mode / SDK | wire 协议 + FFI / MCP / ACP / A2A | 对外集成 |
| `pi.result` / usage ledger | （缺） | 结果记录与成本账本 |
| `xx_messagesMeta` 对应：transcript system 消息 | 图状态影子通道 | 上下文变化对模型/插件的可见性 |

## 附录 C：本次精读的源码清单与结论依据

**pi 侧（完整或大段精读）**

- `packages/agent/src/`：`agent.ts`、`agent-loop.ts`、`types.ts`、`harness/agent-harness.ts`、`harness/session/types.ts`、`harness/session/values.ts`、`harness/runtime/drive.ts`、`harness/runtime/lane.ts`（关键方法）、`harness/execution/tools.ts`、`harness/execution/effect-gate.ts`、`harness/tools/bash.ts`、`harness/tools/file-mutation-queue.ts`、`harness/utils/truncate.ts`、`harness/system-prompt.ts`、`index.ts`
- `packages/agent/docs/`：`harness.md`（Part 0–9 与附录，全文）、`pico-v3.md`（§5–§10）、`tool-durability.md`（前段）
- `packages/ai/src/`：`types.ts`（关键接口）、`models.ts`（接口面）、`utils/retry.ts`、`index.ts`、`providers/*.models.ts` 清单、`models.generated.ts`（头部）
- `packages/coding-agent/src/`：`core/system-prompt.ts`（结构）、`core/system-prompt.ts` 相关调用点、`core/tools/{index.ts,read.ts,bash.ts}`、`core/tools/file-mutation-queue.ts`、`core/session-manager.ts`（接口面）、`core/agent-session.ts`（方法清单）、`core/settings-manager.ts`（接口面）、`core/export-html/*`（清单）、`core/usage-totals.ts`、`modes/rpc/{rpc-types.ts,rpc-client.ts}`（接口面）、`main.ts`（导入与方法面）
- `packages/coding-agent/docs/`：`extensions.md`、`rpc.md`、`compaction.md`、`containerization.md`、`skills.md`、`how-pi-works.md`、`index.md`、`settings.md`（头部）
- `packages/coding-agent/examples/extensions/`：`subagent/{index.ts,agents.ts}`（头部）、`permission-gate.ts`
- `packages/{protocol,server,client}/src/`：`protocol.ts`、`framing.ts`、`codec.ts`、`server.ts`、`session-router.ts`（全文）
- `packages/telemetry/src/index.ts`（接口面）、`packages/tui/README.md`（全文）、`packages/tui/src/{tui.ts,layout-node.ts}`（头部）
- 根目录：`package.json`（scripts）、`test.sh`、`AGENTS.md`、`README.md`、`SECURITY.md`、各 `packages/*/package.json`
- 统计口径：`packages/*/src/**/*.ts`（排除 `test/`）文件数与行数；测试文件数与行数按 `**/test/**` 计

**agentxx 侧（完整或大段精读）**

- `agent/lib/src/agent/`：`base_agent.cpp`（init/runTurnAsync/收尾，含附件加载与系统提示词重建）、`agent_runner.cpp`（全文）、`prompt.cpp`（结构）、`context.cpp`（`buildSystemPrompt` 与持久化）、`session_store.cpp`（schema 与语句）、`checkpoint_store.cpp`、`model_registry.cpp`
- `agent/lib/src/nodes/`：`modelcall.cpp`（`baseRun` 与重试/取消/兜底）、`toolcall.cpp`（`execTool` 与 `baseRun` 执行收集）、`session_context.cpp`（全文）
- `agent/lib/src/middlewares/`：`permission.cpp`（判定顺序与符号链接注释）、`summarization.cpp`（结构与方法清单）
- `agent/lib/src/agent/agent_host.cpp`（spawnOneTask 与批量并发）、`agent/lib/src/tools/subagent.cpp`/`subagent.h`
- `agent/lib/include/agentxx/`：`agent/base_agent.h`、`agent/context.h`、`agent/conversation_types.h`、`agent/config.h`（字段清单）、`agent/model_registry.h`、`agent/io/{wire_protocol.h,session_server_agent_io.h}`、`middlewares/permission.h`、`tools/{tool.h,share_store.h,subagent.h}`、`plugin/{plugin_interfaces.h,client_plugin_manager.h}`
- `agent/client/`：`main.cpp`（参数与模式）、`src/mode_runners.cpp`（结构）、`src/io/tui/*`（结构）、`include/agentxx-client/io/tui/ui_components.h`、`.../framework/tui_settings.h`
- `agent/third_party/cxx_pluginxx_ui/`（文件清单与 CMake）、`agent/plugins/`（目录清单）
- `docs/zh-cn/design/`：`plugins.md`（接口表清单段）、`ffi.md`（前段）
- `agent/test/test.cpp`（模块注册表）、`agent/script/*`（构建脚本清单）
- 统计口径：`agent/{lib,client,plugins}/**/*.{cpp,h,hpp}`（排除 `third_party/`）文件数与行数；测试按 `agent/test/**/*.{cpp,h}` 计

## 附录 D：可以继续深入的清单

按收益排序，以下位置本轮未逐行读完，若后续要落地具体改动建议先精读：

1. **`agent/lib/src/plugins/plugin_manager_*.cpp`（约 3.7 k 行）**：生命周期/注册/调度/能力协商的完整实现；对应 §14 的落地项（依赖声明、卸载顺序、inflight 等待）需要逐函数核对。
2. **`agent/lib/src/middlewares/summarization.cpp`（1186 行）**：压缩链的每步实现细节（去重 key、探索型折叠、切点计算、冷却判定）；对应 §8 的落地项。
3. **`agent/client/src/io/tui/{agent_tui.cpp,components/*}`（约 8 k 行）**：帧循环、命中、浮层、滚动；对应 §12 的落地项（原子刷新、IME、浮层管理）。
4. **`agent/lib/src/protocol/{mcp,acp,a2a}_*.cpp`（约 4.3 k 行）**：外部协议实现的边界与错误处理；对应 §13 的协议治理建议。
5. **`agent/lib/src/agent/training.cpp`（1518 行）**：进化式提示词训练的实现细节；对应 §11/§17 的 P2-9（与账本/轮次打通）。
6. **pi `packages/agent/src/harness/runtime/{lane.ts,drive/*.ts}`（约 5 k 行）**：若要把操作状态机迁移到 agentxx，需要逐过程核对（尤其 §3.7 分类顺序、§3.8 工具阶段、§4.5 恢复表）。
7. **pi `packages/tui/src/components/editor.ts`（2112 行）与 `tui-alt-screen.ts`（1625 行）**：若要做"编辑体验/视口/搜索面板"级别的对齐，这两处是主要参照。
8. **pi `packages/chord/src/**`（约 8 k 行）**：若要做多客户端协同或 facet 化扩展，需要通读 delta tracker（2056 行）与服务生命周期。
9. **pi `packages/ai/src/api/*`（约 12 k 行）**：若要做 provider 能力元数据与"统一错误分类"，各 provider 的差异处理是主要参考。
10. **两侧的 CI/发布流程**（pi `.github/workflows`、`scripts/*`；agentxx `agent/script/*`）：本轮只看了要用的部分，完整流程对比（发布物、签名、版本策略）尚未展开。

