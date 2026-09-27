# agentxx 与 opencode 架构对比

> 对比对象
> - `agentxx`: `D:\0Acoolight\Program\cpp\agentxx`（C++23，Boost.Asio 协程 + NeoGraph 图引擎 + C ABI 插件，单 `io_context` 内多会话协程交错执行）
> - `opencode`: `D:\0Acoolight\Program\js\opencode`（TypeScript/Bun + Effect 4（beta），Bun workspace 多包；本仓库处于 **V2 核心重写**阶段：`packages/core` 是新的领域核心，`packages/opencode` 是仍在跑的旧单体，两者并存）
>
> 本文按模块通读两侧源码、测试与文档后逐模块记录差异、优缺点与**可迁移到 agentxx 的设计**。
> 阅读顺序建议：先看[§0 总览](#0-总览与对比方法)与[§18 迁移建议汇总](#18-迁移建议汇总)，再按模块展开。
>
> **编写说明**
> - 每个模块统一按「agentxx 现状 → 对比（表格 + 两者优缺点小结）→ 可迁移到 agentxx 的设计」三段展开；结论都给出源码/文档依据，依据清单见[附录 C](#附录-c本次精读的源码与文档清单)。
> - 只写**能在 agentxx 现有架构上落地**的迁移项，并标注落地位置与优先级（汇总见[§18](#18-迁移建议汇总)）；明确不该照搬的部分见[§19](#19-反向清单agentxx-不必照搬的设计)。
> - opencode 侧存在「设计文档 vs 源码」的漂移：`specs/v2/*.md` 与 `CONTEXT.md` 描述的是目标形态，`packages/core/src/session/runner/llm.ts` 顶部的复选框清单逐条标注了哪些已经实现、哪些还没有。本文凡引用都区分**「已成事实」与「规格/未实现」**，以源码为准。
> - 文中的数值（文件数、行数、测试数量、默认常量）均为本次在两侧仓库实测算得，统计口径写在对应段落内。
>
> **阅读提示：opencode 现在是「两代运行时 + 一层新核心」并存**
> - 第 1 代（线上形态）：`packages/opencode`（681 文件 / 16.3 万行）内的旧 `Session` 体系 + CLI/TUI/server；`packages/cli` 是新 CLI 外壳。
> - 第 2 代（新核心，本次对比重点）：`packages/core`（481 文件 / 6.2 万行）的 V2 会话核心 —— 会话按「durable inbox + 事件序列 + 投影」组织，上下文按「Context Epoch + 类型化来源」组织，工具按「物化定义 + 结算」组织，服务按「Location 作用域」组织。
> - 支撑层：`packages/llm`（协议/provider/传输/鉴权）、`packages/schema`（叶子 schema）、`packages/protocol`（HttpApi 定义）、`packages/server`（handler）、`packages/client` + `httpapi-codegen`（代码生成 SDK）、`packages/tui` + `packages/session-ui` + `packages/app`（三种前端）。
> 因此下文说「opencode 有/没有某能力」时，都尽量落到具体某一代，避免把新核心的设计稿当成线上能力。
>
> **修订记录**
> - **第 1 版（2026-09，分模块通读）**：按 17 个模块（§1–§17）通读两侧源码/测试/文档并逐模块落盘，正文结构为「现状 → 对比 → 可迁移」；迁移项按 P0/P1/P2 归档到 §18（第 1 版共 72 项），反向结论归档到 §19。本版主要发现：
>   1. **会话投递语义**是 opencode V2 最值得借鉴的一层（§2）：durable 收件箱（`session_input` 表）+ `steer`/`queue` 两种投递 + `admit → promote` 两段 + `wake`/`resume`/`interrupt` 三原语 + 每会话串行/跨会话并行。agentxx 目前是「轮内执行 + 内存排队 + 手工打断立即执行队列首条」。
>   2. **上下文纪元（Context Epoch）**（§3）把「不变的 provider 缓存基线」与「随时间变化的上下文增量」分开：基线只在压缩/移动会话时重建，其余变化合并成一条中间系统消息按时间追加。agentxx 目前把动态内容（时间、环境、记忆文件、技能提示）混在每轮重拼的 system prompt 里，长会话下 provider 前缀缓存命中率与可审计性都不如前者。
>   3. **工具结算边界**（§6）是 opencode V2 的硬约束：先持久化调用、再立即并行执行、按公告身份拒绝陈旧调用、最后统一限幅并落到会话历史。agentxx 已有更完整的工具特性（重试、延迟加载、重复调用询问、插件声明权限），但**同轮工具仍是串行执行**（源码 `// TODO: 真正并行`），且输出超限时是压缩丢弃而不是保留完整输出。
>   4. **权限系统方向相反且各有正当性**（§10）：agentxx 是「插件声明目标 + 宿主统一判定 + 工作区隔离」，opencode 是「工具自己发起 `assert` + 项目级 saved approvals + 拒绝时同会话待决请求一并拒绝」。本文建议补的是「批准记忆的持久化与撤销入口」「拒绝/批准对同会话待决请求的连带处理」，而不是取消声明式目标。
>   5. **扩展机制仍是 agentxx 更强**（§15）：C ABI + 20 张 agent 侧表与 10 张 client 侧表 + 生命周期事务 + 多实例规则 + 能力协商，是 opencode 的进程内 TS/Effect 插件完全没有的边界；opencode 值得借鉴的是「可回滚的声明式 transform + Scope 关闭即重算」与「单点更新只让依赖它的部分反应」。
>   6. **测试体系互补**（§16）：opencode 的强项是 HTTP 录制回放、代码生成产物提交 + CI 校验、导入边界测试、UI 组件故事化；agentxx 的强项是资源基准（模块级内存分解、基线对比）与端到端多会话/中断/取消测试。
> - **第 2 版（2026-09，细读实现 + 补充优缺点）**：本轮把两侧实现继续往下读了一层（主要新增阅读：agentxx 的 `nodes/modelcall.cpp` 的请求组装/修复/重试段、`nodes/agentcall.cpp` 的每轮清场、`agent/context.cpp` 的 `buildSystemPrompt`、`middlewares/{memory_file,summarization}.cpp`；opencode 的 `session/projector.ts`、`session/message-updater.ts`、`session/runner/{to-llm-message,model,publish-llm-event}.ts`、`llm/src/route/executor.ts`、`tool/{bash,question,todowrite}.ts`、`skill/guidance.ts`、`agent.ts`），并做两处结构调整：
>   1. **每个模块新增「X.4 两者的优缺点」**（原「可迁移」小节顺延为 X.5），逐项区分「谁更强、强在哪、代价是什么」；同时按细读结果补充了九处具体事实（§2 agentxx 的每轮清场与重试/修复细节、§3 系统提示词每轮整体重建、§5 投影器同时挂 V1/V2 且 delta 不落库、§6 `bash` 的默认超时/捕获上限/权限断言、§8 传输层重试与模型到协议的硬编码映射、§9 压缩子代理复用父前缀 vs 扁平序列化、§11 agent `mode` 与后台任务 promote）。
>   2. **迁移清单扩到 75 项**：新增 M73（重试按状态码/响应头 + 抖动）、M74（会话用量账本，含 cache read/write 与删除回退）、M75（陈旧调用/取消/中断三类收尾的一致性断言）。
> - **第 3 版（2026-09，插件框架专项细读）**：本轮把两侧插件系统的实现读到函数级（agentxx 侧读完 `cxx_pluginxx` 全部头文件：`api/{abi,entry,tables,export}.h`、`host/{lifecycle,host_core,tables_impl,manifest,event_bus,domain_hooks,capability_registry,loader}.h`、`runtime/{runtime,instance_base,manager_base,op_driver,driver}.h`、`kit/kit.h`，以及宿主侧 `plugin_manager.h`、`plugin_manager_lifecycle.cpp`、`tool_registry.h`、`plugin_graph_node.h`、`client_plugin_manager.cpp` 的结构；opencode 侧读完 `plugin/src/v2/effect/{PLAN.md,README.md}`、`v2/promise/README.md`、`core/src/plugin/{internal.ts,promise.ts,host.ts}`、`core/src/effect/layer-node.ts`、`core/src/config/plugin/external.ts`、`plugin/src/{tool.ts,tui.ts}`、`core/src/plugin/provider/anthropic.ts`、`core/src/plugin/variant.ts`、`tui/src/plugin/{slots.tsx,api.ts}`），产出：
>   1. **§15 扩写为插件框架专项**：15.1/15.2 按「内核四层 / SDK / 宿主管理器」与「transform vs hook / 域状态模型 / boot 批处理 / 服务图 / 外部装载 / TUI 插件」重写；新增 **§15.5「架构设计细读：逐维度对照」**（边界与契约、生命周期与所有权、贡献与撤销、顺序与依赖、线程与并发、卸载与关闭安全、隔离与信任、装配与替换、热更新语义、可观测性 十条），对比表补 7 行、优缺点按实现细节重写。
>   2. **迁移清单扩到 79 项**：新增 M76（顺序位 + 批量重算合并）、M77（按名替换服务的装配接缝）、M78（插件渲染错误记录）、M79（只读域视图查询）。
>   3. **两处「读文档会高估现状」的提醒**：opencode 的 `PLAN.md` 属目标设计（`ctx.tool.hook` 等运行时 hook 面未落地，v2 现状只有 transform + `aisdk` hook + `plugin.add/remove`）；agentxx 的表数量文档滞后于实现（19/9 vs 实测 20/10）。

## 目录

| 章节 | 内容 | 一句话结论 |
|---|---|---|
| [0](#0-总览与对比方法) | 总览与对比方法 | 两项目定位、规模、判据与结论速览 |
| [1](#1-总体架构与包分层) | 总体架构与包分层 | opencode 用「叶子 schema + 协议 + 核心 + 前端」的单向依赖表达边界并有测试守着；agentxx 是「核心库 + 客户端 + 插件」三层，边界靠文档 |
| [2](#2-会话运行模型投递轮次与执行所有权) | 会话运行模型 | 可借鉴**durable 收件箱 + steer/queue 投递 + wake/resume/interrupt 三原语**；agentxx 需要把「排队消息」从内存变成可恢复状态 |
| [3](#3-会话历史llm-上下文与上下文纪元) | 会话历史与上下文纪元 | 可借鉴**基线/增量分离的上下文纪元**，让 provider 缓存前缀稳定、上下文变化可审计 |
| [4](#4-会话位置工作区隔离与移动) | 会话位置、工作区隔离与移动 | opencode 把服务与文件系统按 Location 作用域化并支持会话迁移；agentxx 的 worktree 隔离 + 工作目录多源回退更贴近真实编码场景，缺的是「会话移动」与服务作用域的显式化 |
| [5](#5-持久化事件序列与崩溃恢复) | 持久化、事件序列与崩溃恢复 | 可借鉴**聚合内单调事件序列 + after 游标重放**，让断线/重启后的增量同步不再依赖服务端内存缓冲；不必照搬整库事件溯源 |
| [6](#6-工具系统) | 工具系统 | 可借鉴**公告身份防陈旧调用**「调用先持久化再并行执行」「完整输出落盘 + 预览」；agentxx 的重试/延迟加载/重复调用询问是可反向输出的优势 |
| [7](#7-系统提示词指令上下文与技能) | 系统提示词、指令与技能 | 可借鉴**指令集合变更的显式通知（含撤销与取代语义）**与**技能正文按权限按需加载** |
| [8](#8-llm-层协议provider鉴权与缓存) | LLM 层 | 可借鉴**prompt 缓存断点策略**、**provider 错误分类（溢出/鉴权/限流）**、**HTTP 录制回放**、**强制工具式结构化输出** |
| [9](#9-上下文压缩与预算控制) | 上下文压缩与预算 | 可借鉴**结构化滚动摘要模板**与**收到溢出错误后的一次性压缩重试**；agentxx 的失败降级/冷却/互斥更成熟 |
| [10](#10-权限与安全边界) | 权限与安全边界 | 可借鉴**批准记忆的持久化与撤销入口**、**拒绝时连带拒绝同会话待决请求**；agentxx 的声明式目标 + 三态路径判定 + 完全授权更完整 |
| [11](#11-子代理后台任务与并行) | 子代理、后台任务与并行 | opencode 的「子代理就是真会话（带 parentID）」便于审计；agentxx 的深度/并发预算、取消级联、中断即委派更紧凑 |
| [12](#12-大输出附件快照回滚与结构化询问) | 大输出、附件、快照与询问 | 可借鉴**大输出落盘保真**、**git 快照式回退**、**结构化询问工具** |
| [13](#13-客户端-ui-与渲染分层) | 客户端 UI 与渲染分层 | opencode 把 TUI/Web 拆成独立包并共享渲染组件；agentxx 的「服务端产出声明式组件树 + 客户端唯一渲染实现」在跨端一致性上更省事，需补的是渲染层与宿主解耦的测试边界 |
| [14](#14-远程协议sdk-与嵌入模式) | 远程协议、SDK 与嵌入模式 | 可借鉴**单一定义生成两端绑定**、**durable 事件流与实时流分离**、**不可透传游标**；agentxx 的「同进程与远程只换 transport」是更干净的一层 |
| [15](#15-扩展机制c-abi-插件-vs-effect-插件) | 扩展机制（插件框架专项） | agentxx 的四层内核（ABI/宿主内核/运行时/SDK）、五态生命周期与 admission lease + 延迟收尾领先；opencode 的 **transform 重放（重算而非撤销）**、**hook 串行后见前**、**批量重建合并**与**服务图替换**值得借鉴；**§15.5 逐维度细读**是本节重点 |
| [16](#16-测试与质量门禁) | 测试与质量门禁 | 可借鉴**HTTP 录制回放**、**生成产物一致性校验**、**导入边界测试**；agentxx 的资源基准与端到端会话测试可反向输出 |
| [17](#17-配置模型目录与装配) | 配置、模型目录与装配 | 可借鉴**配置版本迁移**、**远程模型目录 + 本地缓存**、**鉴权方式分层（API Key/OAuth/环境变量）** |
| [18](#18-迁移建议汇总) | 迁移建议汇总 | P0/P1/P2 三级清单（共 75 项，分 A–F 组与 4 个落地批次） |
| [19](#19-反向清单agentxx-不必照搬的设计) | 反向清单 | 明确哪些差异是正当取舍 |
| [20](#20-结语) | 结语 | 三条原则 |
| [附录 A](#附录-a关键文件与文档索引) | 关键文件/文档索引 | 两项目对应模块的路径对照 |
| [附录 B](#附录-b术语对照) | 术语对照 | 同一概念的两种叫法 |
| [附录 C](#附录-c本次精读的源码与文档清单) | 源码清单与结论依据 | 逐条列出本次实际读过的文件 |
| [附录 D](#附录-d可以继续深入的清单) | 可继续深入清单 | 尚未精读的位置与能回答的问题 |

---

## 0. 总览与对比方法

### 0.1 两个项目的定位与规模

| 维度 | agentxx | opencode |
|---|---|---|
| 语言/运行时 | C++23；Boost.Asio 协程，单 `io_context` 内多会话协程交错，无锁；可编译为可执行文件/动态库/静态库 | TypeScript（Bun + Node ≥ 22）；Effect 4（beta）+ Drizzle + SQLite；Bun workspace 多包 |
| 顶层结构 | `agent/lib`（libagentxx）、`agent/client`（CLI/TUI）、`agent/plugins`（15 个内置插件 + 5 个示例目录）、`agent/test`、`agent/benchmark` | 32 个包：`core`（V2 核心）、`opencode`（旧单体）、`cli`、`llm`、`schema`、`protocol`、`server`、`client`、`sdk`/`sdk-next`、`tui`、`ui`、`session-ui`、`app`、`desktop`、`codemode`、`plugin`、`httpapi-codegen`、`http-recorder` 等 |
| 编排核心 | NeoGraph 图引擎（节点 + 条件边 + `Command.goto_node`），节点被中间件栈包裹；ReAct 循环 = `agent_start → llm → tools → llm → agent_end` | `SessionRunner.run`（每会话串行 drain）+ `runTurnAttempt`（一次 provider 轮次）；`SessionRunCoordinator` 串行化同一会话、并行不同会话 |
| 会话与上下文 | 会话是 LLM 上下文的唯一权威（typed `ChatMessage` + `messagesVersion`）；另有 `viewMessages` 展示历史；SQLite 落库 + 节流写 | 会话 = `session` 行 + 不可变事件序列 + 投影出的 `session_message` 行；上下文 = （Context Epoch 基线）+（投影历史）；`session_input` 收件箱管理未提升的输入 |
| 压缩 | 阈值触发 + 工具输出去重/截断 + 子代理摘要 + 失败降级硬截断 + 冷却 | 请求预算（窗口 − max(输出, buffer)）触发；结构化滚动摘要 + 尾部 token 窗口保留；溢出错误触发一次压缩重试 |
| 工具系统 | `XXToolBase`/`XXToolWrap`（自动压缩、延迟加载、重试、重复调用询问）+ `ToolcallWrapNode`（串行执行）+ 插件声明权限 | `Tool.make`（输入/输出 codec + 执行 + 模型投影）+ `ToolRegistry.materialize/settle`（公告身份、并行执行、统一限幅、落盘保真） |
| 权限 | 中间件统一判定：白/黑名单 + 工作目录隔离 + 完全授权 + 记住选择 + worktree 写边界 + 插件的声明式目标 | `PermissionV2.assert/ask/reply`：ruleset（action/resource/effect）+ 项目级 saved approvals + 工具自行发起 + 拒绝连带处理 |
| 扩展形态 | 纯 C ABI 动态库插件；实测头文件中 agent 侧表 20 张（`pluginxx.*` 10 + `agentxx.agent.*` 10）、client 侧 10 张（`agentxx.client.*`）；生命周期 `create/start/stop/destroy` 事务 | 两代：v1 Promise hooks 插件（`packages/plugin`）+ v2 Effect 原生插件（`PluginV2.add(id, effect)`，Scope 关闭即撤销）；另有 codemode 受限 JS 编排 |
| 客户端 | FTXUI TUI（唯一渲染实现）+ stdio CLI + 远程 WS 客户端；服务端产出声明式组件树（`agentxx.ui.item`） | `packages/tui`（OpenTUI + Solid，独立包，只依赖 SDK）、`packages/session-ui`（Web 组件，与 TUI 共享消息渲染）、`packages/app`（Web/桌面）、`packages/desktop` |
| 远程/集成 | 手写 Wire JSON 协议（30+ 消息）+ WS + 进程内 Channel；MCP client/server、A2A server/client、ACP server、FFI | Effect `HttpApi` 定义 → 代码生成 Promise/Effect 客户端 + OpenAPI；SSE 事件流；Embedded OpenCode（内存 transport 复用同一 client） |
| 测试 | 自研 `agentxx_test`：本次实测注册 68 个模块（41 同步 + 27 异步）+ `agentxx_benchmark` 资源基准 | 682 个 `.test.ts` / 15.06 万行；`bun test` + `tsgo` 类型检查 + Playwright + Storybook；HTTP 录制回放、代码生成一致性校验 |
| 文档 | `docs/zh-cn/design`：index 165 KB、plugins 102 KB、tui 43 KB、benchmark 43 KB、ffi 24 KB | 仓库内 `specs/v2/*.md`（session 28 KB、config 29 KB、schema-changelog 39 KB…）+ `CONTEXT.md` 32 KB（术语表与关系定律）+ 各包 README |
| 代码规模（本次实测） | `agent/lib` 138 文件 / 5.42 万行；`agent/client` 67 文件 / 2.28 万行；`agent/plugins` 53 文件 / 2.35 万行；`agent/test` 167 文件 / 7.32 万行 | `core` 481 / 6.23 万；`llm` 105 / 1.86 万；`tui` 204 / 2.95 万；`session-ui` 96 / 1.97 万；`ui` 247 / 3.15 万；`app` 596 / 16.1 万；`opencode`（旧单体）681 / 16.3 万（口径：各包 `src` 下 `.ts/.tsx`，排除 `src/generated` 与 `dist`） |

### 0.2 对比方法与判据

- 每个模块都用**三方证据**核实：源码（真实行为）、测试（行为边界）、文档（设计意图）。凡文档与源码冲突处，以源码为准，并在文中标出漂移点。
- 「优点/缺点」的判据只有两条：
  1. **后续改动成本**：扩展点是否收敛、约束是否显式、有没有「只有作者知道」的隐含前提；
  2. **运行期正确性**：生命周期、取消、恢复、并发与资源回收是否可控。
- 迁移建议只写**能在 agentxx 现有架构（单 io_context 协程 + 图引擎 + C ABI 插件）上实现**的项，并标注落地位置（哪个文件/哪一层）与代价；不能落地的不写。

### 0.3 结论速览

| 模块 | agentxx 现状 | opencode 现状 | 更值得借鉴的方向 |
|---|---|---|---|
| 包分层 | 核心库 + 客户端 + 插件；边界靠文档与约定 | 叶子 schema → 协议 → 核心/服务端 → 客户端/SDK 单向依赖，有导入边界测试 | 把依赖方向写成可执行的检查 |
| 轮次与投递 | `runTurnAsync` 单轮 + 内存排队 + 打断立即执行队首 | durable 收件箱 + steer/queue + admit/promote + wake/resume/interrupt | 排队状态持久化、投递语义显式 |
| 上下文 | 每轮重拼 system prompt；动态内容混在其中 | Context Epoch：基线 + 快照 + 中间系统消息（按需惰性采样） | 基线与增量分离，缓存前缀稳定 |
| 位置与工作区 | 会话工作目录多源回退 + worktree 写边界 | Location 作用域服务 + 会话可移动（移动后清 epoch） | 服务作用域显式化、会话迁移语义 |
| 持久化 | 会话库（消息/上下文/viewMessages）+ 图 checkpoint，节流写 | 聚合事件序列 + 投影 + after 游标重放 | 会话内事件序列与重放游标 |
| 工具 | 特性丰富但串行、输出压缩丢内容 | 公告身份 + 并行结算 + 完整输出落盘 + 结果与投影分离 | 并行结算与输出保真 |
| 提示词/技能 | 字符串拼装 + 追加段 + 技能中间件 | 类型化上下文来源 + 指令集合变更通知 + 技能正文按权限加载 | 指令变更可审计、技能正文按需 |
| LLM 层 | 3 协议、无缓存断点、无错误分类 | 6 协议 + 10 个 provider 门面 + 32 个 provider 插件、缓存策略、溢出分类、录制回放 | 缓存断点、错误分类、录制回放 |
| 压缩 | 阈值 + 子代理摘要 + 硬截断兜底 + 冷却 | 结构化滚动摘要 + 尾部窗口 + 溢出重试 | 摘要模板与溢出恢复 |
| 权限 | 声明式目标 + 三态判定 + 完全授权 + 隔离 | 工具自行 assert + 项目级记忆 + 连带处理 | 批准记忆的持久化与撤销、连带处理 |
| 子代理 | 中断即委派 + 深度/并发预算 + 取消级联 | 子代理就是真会话（parentID）+ 前台/后台活动区分 | 子代理会话可选持久化 |
| 大输出/附件 | 附件跨设备选择、无回退、无结构化询问工具 | managed 输出文件 + git 快照回退 + question 工具 + PTY | 大输出保真、快照回退、结构化询问 |
| 客户端 | 声明式组件树 + 唯一渲染实现 | 独立 TUI 包 + 共享渲染组件 + 插件渲染槽 | 渲染层解耦与流式 markdown 处理 |
| 协议/SDK | 手写 wire + WS/Channel + MCP/A2A/ACP/FFI | HttpApi → 生成两套 SDK + OpenAPI + 内嵌模式 | 单一定义生成绑定、durable 事件流分离 |
| 扩展 | C ABI + 20/10 张表 + 生命周期 + 多实例 + 能力协商 | Effect 插件 + 声明式 transform + Scope 回滚 | Scope 关闭即重算、细粒度更新反应 |
| 测试 | 68 个模块 + 资源基准 | 682 个测试文件 + 录制回放 + 生成一致性 + 边界测试 | 录制回放、生成一致性、边界断言 |
| 配置 | base/overlay yaml + 设置 KV + 覆盖/移除语义 | 分层配置 + 版本迁移 + 远程模型目录 + OAuth 体系 | 配置迁移、模型目录、鉴权分层 |

---

## 1. 总体架构与包分层

### 1.1 agentxx：三层结构 + 图引擎内核

```
agent/
├── lib/            libagentxx（核心库：BaseAgent/CodeAgent、图节点、中间件、tools、
│                    protocol client/server、plugin 宿主、sqlite 会话存储、FFI）
├── client/         agentxx_cli（FTXUI TUI / stdio CLI / 远程 WS 客户端 / main 装配）
├── plugins/        15 个内置插件 + 5 个示例目录（每个插件静态链接 cxx_utilxx(_base) 一份）
├── test/           agentxx_test（68 个模块）
└── benchmark/      agentxx_benchmark（资源基准）
```

- 规模（实测）：`lib` 138 文件 / 5.42 万行、`client` 67 文件 / 2.28 万行、`plugins` 53 文件 / 2.35 万行、`test` 167 文件 / 7.32 万行。
- 大文件集中在少数「职责重」的实现上：`client_plugin_manager.cpp`（4 009 行）、`mcp_client.cpp`（2 171）、`openai_provider.cpp`（2 070）、`mcp_server.cpp`（1 571）、`session_server_agent_io.cpp`（1 519）、`training.cpp`（1 518）、`plugin_manager_vtable.cpp`（1 350）、`base_agent.cpp`（1 246）、`summarization.cpp`（1 186）、`toolcall.cpp`（1 184）。
- 分层是**运行形态分层**而不是「领域分层」：lib 内部把「会话运行、图节点、中间件、工具、协议、插件宿主、存储」放在同一层，靠头文件目录（`agent/`、`nodes/`、`middlewares/`、`tools/`、`protocol/`、`plugin/`、`event/`、`util/`）区分职责。
- 客户端与 agent 之间只有一条边界：`AgentIOBase`（端点）+ `AgentIOTransportBase`（传输）。同进程用 `ChannelAgentIOTransport`，跨进程用 `WsAgentIOTransport`，agent 侧统一是 `SessionServerAgentIO`。
- 依赖方向在文档里写得很清楚（client 只做 UI 渲染、agent 负责数据来源），但没有可执行的检查：`agent/lib` 的头文件被谁包含、client 是否间接依赖了 lib 内部实现，都靠人工约定与代码评审保证。

### 1.2 opencode：叶子 schema + 单向依赖 + 可执行的边界

新增核心（V2）的包依赖方向来自仓库根 `AGENTS.md` 的明文规则：

> Keep runtime dependencies directed from Schema to Core and Protocol, then from Core and Protocol to Server. Client runtime code may depend on Schema and Protocol but never Core or Server; `sdk-next` composes Client, Core, and Server.

```
schema（叶子：纯 schema，零 Effect/数据库/进程依赖）
  ├──→ core（领域核心：会话、工具、权限、provider、插件宿主、数据库）
  └──→ protocol（HttpApi 定义：路径/载荷/中间件位置）
          └──→ server（handler：把协议接到 core 服务）
                  └──→ sdk-next（内嵌宿主：内存 transport 组合同一套 client）
client（生成的 Promise/Effect 客户端：只依赖 schema + protocol）
tui / session-ui / ui / app（前端：只依赖 SDK + 自身 UI 依赖）
```

- 这条规则不是口号，`CONTEXT.md`「Client contract architecture」一节把它写成硬约束：`packages/schema` 与 `packages/protocol` **不得**间接加载数据库、Drizzle、会话执行、provider、watcher、原生模块或 WASM；并明确要求用**导入边界测试**守住浏览器安全的 `@opencode-ai/client` 与 `@opencode-ai/client/effect` 两个 bundle。
- `packages/core` 内部再分角色，规则写在 `specs/v2/instructions.md`：core 只放「领域 schema、类型化错误、状态容器、事件、插件 hook 契约」，provider 差异、配置差异、鉴权、模型发现这类**策略**一律进插件；`packages/opencode` 要「逐步变薄」，只保留 UI、路由、CLI、存储胶水与旧版兼容。
- 价格：包多、层多，`packages/opencode` 与新核心并存期间有 16.3 万行的旧单体仍是线上主路径（且 V2 通过 shadow bridge 把 V1 已可见的消息补发 `Prompted` 事件）。这是**迁移中的过渡态**，不是终态。

### 1.3 对比

| 维度 | agentxx | opencode | 结论 |
|---|---|---|---|
| 分层依据 | 运行形态（库/客户端/插件） | 依赖方向（叶子→协议→核心→服务端→前端） | opencode 的分层能防「反向依赖悄悄长出来」，agentxx 需要一条可执行检查 |
| 边界强制 | 文档 + 评审 | 导入边界测试 + `CONTEXT.md` 硬约束 | 可借鉴（低成本） |
| 领域与策略分离 | 策略散在中间件与配置里（如权限模式、模型选择、工具注入条件） | core 只放领域与容器，策略进插件（`provider.*`/`model.*`/`agent.*` hook） | 部分可借鉴：agentxx 已有中间件层，可把「策略」收敛为可替换的实现（见 §15） |
| 迁移成本 | — | 旧单体 16.3 万行 + 新核心 6.2 万行并存，V1/V2 双轨 | 反向教训：双轨期越短越好，迁移要一次收敛（§19） |
| 代码集中度 | lib 内 5.42 万行，10 个千行级文件 | core 内 6.23 万行，最大文件 1 610 行（github-copilot 适配） | opencode 的单文件更小、更适合局部替换；agentxx 的千行文件是改动风险点 |

### 1.4 两者的优缺点

**agentxx**
- 优点：只有三层（核心库 / 客户端 / 插件），编译形态灵活（可执行、动态库、静态库都能产出）；插件与主程序各自静态链接一份工具库，不同版本的工具库可以共存；lib 内部按职责分目录（`agent/`、`nodes/`、`middlewares/`、`tools/`、`protocol/`、`plugin/`、`event/`、`util/`），找代码不靠猜。
- 缺点：依赖方向只写在文档里，`client` 与 `lib` 同一个 superbuild，反向依赖（client 包含 lib 私有头、插件包含 lib 内部头）没有检查手段；lib 内有 10 个千行级文件（`session_server_agent_io.cpp` 1 519 行、`base_agent.cpp` 1 246 行、`toolcall.cpp` 1 184 行），改动风险集中；「领域逻辑」与「配置策略」混在运行代码里（是否注入子代理工具、权限默认模式、延迟加载集合都直接读配置）。

**opencode**
- 优点：依赖方向有明文规则（`Schema → Core/Protocol → Server`，client 不得依赖 Core/Server）并用导入边界测试守住；`core` 只放领域 schema、状态容器、事件与 hook 契约，provider/配置/鉴权/模型发现这类策略放插件；单文件小（`core` 内最大的业务文件是 github-copilot 协议适配 1 610 行），便于局部替换与单独测试。
- 缺点：32 个包 + Bun workspace catalog + `tsgo` 构成的构建链复杂度高；旧单体（16.3 万行）与新核心（6.2 万行）长期并存，同一能力常有两处实现，读代码先要判断「这是哪一代」；生成物入库与多包发布带来额外流程成本（改协议要走代码生成）。

### 1.5 可迁移到 agentxx 的设计

1. **[P1] 把依赖方向写成可执行检查**：新增一个测试模块（如 `agent/test/core/test_boundaries.h`，或构建期脚本），断言：
   - `agent/client/include`、`agent/client/src` 不包含 `agent/lib/src/**` 的私有头；
   - `agent/plugins/**` 只包含 `agentxx/plugin/api/**` 与 `cxx_utilxx*` 的公开头，不包含 lib 内部头；
   - 插件动态库导出符号仍只有白名单（已有 version script，可加一条自动化断言，见 §16）。
   成本低、收益直接：现在这些约束只靠 AGENTS.md 的文字。
2. **[P2] 领域与策略的显式分层**：agentxx 的「策略」目前分布在 `AgentConfig` 分支、中间件开关、工具注入条件里（例如是否注入 `agentxx_subagent`、权限默认模式、延迟加载工具集合）。可参照 opencode 的做法，把这类决策集中为一层「提供者/装配」代码（不必引入插件化 hook），让领域代码（会话、图、工具结算）不直接读配置策略。落地位置：`agent/lib/src/agent/base_agent.cpp`、`code_agent.cpp` 的装配段。
3. **[P2] 反向提醒**：不要为了「分层好看」把 lib 拆成多个动态库。agentxx 的独立可执行/动态库/静态库三形态与插件静态链接 `cxx_utilxx` 的约束，决定了「一个核心库 + 插件各自静态依赖」是更合适的形态（见 §15、§19）。

---

## 2. 会话运行模型：投递、轮次与执行所有权

### 2.1 agentxx 现状

- 运行入口：`BaseAgent::runTurnAsync` 驱动一次「用户输入 → 图执行 → 结束」，图内 ReAct 循环是 `llm` 节点按「本轮是否有 tool_calls」返回 `Command.goto_node`（tools / agent_end）。
- 中断与恢复：节点抛 `NodeInterrupt` 时图引擎 checkpoint 暂停，`AgentRunner` 统一处理中断循环（主 agent 与子代理共用同一实现），把中断结果写回 `graphData` 后 `resume_async` 恢复。
- 输入排队：执行中到达的用户输入由服务端按会话排队，经 `MessageQueueUpdate` 同步给客户端；客户端可 `RemoveQueueItem` / `ClearMessageQueue` / `InterruptAndRunNext`（打断当前轮次立即执行队首）。**队列在内存里，进程重启即丢**。
- 取消：`CancelToken` 双通道（轮询标记 + asio 信号中断），可在图 super-step 之间、工具调用前后、LLM 调用前与重试等待后生效。
- 每轮开始清场：`AgentStartCallWrapNode::onNodeStart` 会 `graphData[thread_id].clear()`，并发布 `plugin.agentxx.round_start`（插件侧据此清空会话取消标记），因此「每轮追加类」的图数据不会跨轮累积。
- 轮内重试（`ModelCallWrapNode::baseRun` 的 do/while）：重试计数不因部分输出而重置；失败或取消且剩余重试为 0 时，若末尾不是 assistant 或末尾 assistant 仍带 `tool_calls`，会插入一条带 `AutoInserted` 标记的 assistant 兜底消息（避免悬挂的 tool_calls 被误路由回 tools 节点重复执行）；部分输出 ≥512 字符时先把已生成内容落为 assistant 消息再继续重试；退避为 `retry × 3s`，命中限速关键字再加 `retry × 5s`。
- 调用前修复（`repairMessages`）：合并连续 user 消息（应对 Anthropic/部分兼容网关的 400）、改写重复的 `tool_call_id`、空消息补 `[Empty]`、UTF-8 修复，并在 system 消息内容哈希变化时打 ERROR 日志（说明系统提示词本应稳定）。
- 并发模型：单 `io_context`，多会话协程交错执行；会话状态按 `sessionId` 隔离，无锁（`getSessionCancelToken` 等按会话取令牌）。

### 2.2 opencode 现状（V2 会话核心）

V2 把「一次输入」拆成**收件箱条目**与**执行**两件事（`specs/v2/session.md`、`packages/core/src/session/input.ts`）：

```
sessions.prompt({ sessionID, prompt, delivery? = "steer", resume? })
  → SessionInput.admit(...)            // 写 session_input 行（可持久、可重放）
  → delivery=steer: 下一个安全边界提升为可见消息
  → delivery=queue: 等到会话本来要空转时，才按 FIFO 提升一条
  → resume !== false: 追加一次执行唤醒 execution.wake(sessionID)
```

- **两段状态**：`session_input` 表的 `admitted_seq`（被接受）与 `promoted_seq`（已进入模型可见历史）是两列，唯一索引保证同一 seq 不会被提升两次；投影器（`SessionProjector`）在同一事件事务里写可见消息 + 标记 inbox 行已提升。
- **三原语**（`packages/core/src/session/execution.ts` + `run-coordinator.ts`）：
  - `resume(sessionID)`：显式恢复 —— 执行中则加入（join）当前执行，空闲则强制开一次 drain；
  - `wake(sessionID)`：有新持久化工作时**建议性**唤醒，重复唤醒会合并（`pendingWake` 标志），只有在能提升输入时才会真的调用 provider；
  - `interrupt(sessionID)`：中断本进程执行并等待清理；空闲会话是 no-op；**不删除收件箱条目**，输入留给下一次 wake/resume。
- **执行所有权**：`SessionRunCoordinator` 按 key 串行化（同一会话只有一个执行者），不同 key 并行；`sessions.active()` 返回本进程活动会话快照，是运行时状态（重启后为空）；后台子代理与任务**不**把父会话标记为 active。
- **轮内循环**（`session/runner/llm.ts` 的 `runTurnAttempt` + `run`）：每轮 = 提升输入 → 初始化/准备 Context Epoch → 解析模型 → 载入投影历史 → 组装请求（system + messages + tools）→ 可能压缩 → 流式调用 provider（事件带序号持久化）→ 工具调用**先持久化再立即并行执行** → 等全部结算 → 重载历史 → 下一轮；`needsContinuation` 由「本轮是否有本地工具调用」决定，`steer` 输入可在会话仍需要继续时在下一个边界提升。
- **轮次上限**：agent 可配 `steps`，达到上限时注入 `MAX_STEPS_PROMPT` 并把 `toolChoice` 设为 `none`（最后一轮不再给工具），而不是直接报错。
- **规格里明确不做的事**（源码清单里仍是未勾选项）：不做通用 provider 超时/看门狗；不做「崩溃后自动继续 provider 工作」的猜测式恢复；不把 tool 结果当作 provider 失败的一部分重放。

### 2.3 对比

| 维度 | agentxx | opencode V2 | 评价 |
|---|---|---|---|
| 输入排队 | 内存队列 + 客户端消息同步，可删除/清空/插队 | `session_input` 表，`admitted_seq`/`promoted_seq` 两列，可重放 | opencode 更强的**可恢复性与多端一致性**；agentxx 的队列在崩溃/服务重启后丢失 |
| 投递语义 | 隐式：排队 + 手工打断立即执行 | 显式 `steer` / `queue`，边界规则写在文档与测试里 | opencode 的语义更清楚：steer 在「需要继续」时提升，queue 只在会话本来要空转时提升 |
| 执行所有权 | 每会话一条协程；无「加入/合并唤醒」概念 | coordinator：join + 唤醒合并 + 独立中断 | agentxx 当前没有「重复唤醒合并」的问题（runTurnAsync 由队列驱动），但**显式 resume/wake 分离**对远程客户端与多端场景有用 |
| 取消/中断语义 | CancelToken 双通道 + 中断缓存已有工具结果 | 中断是 Effect fiber 中断；已持久化的调用不会静默重放；启动时把上次进程遗留的 `running` 工具标记为失败（`Tool execution interrupted`） | opencode 的「启动时清账」值得借鉴：agentxx 的 `interruptToolcallCache` 在同一进程内有效，跨进程重启后遗留的工具状态需要显式收尾 |
| 幂等重试 | 无 request id 概念 | `prompt(id, ...)`：同 id 同内容视为精确重试，同 id 不同内容报 `PromptConflictError` | 网络重试下很有用（客户端重发不产生重复消息） |
| 轮次上限 | 有（step 预算相关配置） | `steps` + 最后一轮禁用工具 | 类似；opencode 的「最后一轮不给工具、注入收尾提示」更温和 |
| 崩溃恢复 | 图 checkpoint + 中断缓存（同进程） | 显式不做猜测式恢复；只有显式 `run` 才从投影历史继续 | 两者取向一致：都不猜测；opencode 把「哪些情况必须显式恢复」写成了规格 |

### 2.4 两者的优缺点

**agentxx**
- 优点：单 `io_context` 协程交错、按会话隔离状态、无锁；取消埋点覆盖到「LLM 调用前」「工具调用前后」「图 super-step 之间」，且取消与重试等待都能被打断；重试计数不因部分输出而重置（源码注释明确「避免消息无限堆积」），失败/取消时插入带 `AutoInserted` 标记的 assistant 兜底消息，保证末尾不是悬挂的 `tool_calls`，不会被错误路由回 tools 节点；`repairMessages` 在每次调用前主动修复历史（合并连续 user 消息、改写重复 `tool_call_id`、补 `[Empty]`、UTF-8 修复），对网关兼容性容错强。
- 缺点：执行中排队消息只在内存（服务端重启即丢），没有「已接受 / 已提升」两段状态；投递语义靠 UI 手势（`InterruptAndRunNext`）而不是显式类型；没有请求级幂等 id，客户端重发可能产生重复输入；没有启动清账，跨进程遗留的「执行中」工具没有收尾；重试策略是固定退避（`retry × 3s`，命中限速关键字再加 `retry × 5s`），不读 `retry-after`、无抖动、不按状态码分类。

**opencode**
- 优点：输入先进入 durable 收件箱（`admitted_seq` / `promoted_seq` 两列 + 唯一索引），可重放、可多端一致；`steer` / `queue` 语义与提升边界写在 `specs/v2/session.md` 与测试里；`wake`（建议性、可合并）/ `resume`（显式）/ `interrupt`（幂等）三原语职责清楚；`SessionRunCoordinator` 做到同会话串行、跨会话并行；`prompt(id)` 支持精确重试与冲突检测；每轮开始先把上次遗留的 `pending`/`running` 工具判为「Tool execution interrupted」（`failInterruptedTools`）；agent 可配 `steps`，最后一轮禁用工具并注入收尾提示而不是直接报错。
- 缺点：Effect fiber 的中断与作用域语义需要熟悉 Effect 才能追踪；`Session Drain` 明确「没有持久身份」，崩溃后的继续必须靠显式 `resume`（规格把自动恢复列为后续设计）；eager 工具执行不设上限（规格自述「当前本地实现无界」，收窄要靠 SQLite 结算吞吐限制）；运行期状态（活动会话集合）重启即空，需要客户端重新获取。

### 2.5 可迁移到 agentxx 的设计

1. **[P0] 输入排队持久化 + 两段状态**：把「待执行输入」从内存队列改为会话库中的一行（表可命名 `session_input`，字段：`id`、`session_id`、`payload`、`delivery`、`admitted_seq`、`promoted_seq`、`time_created`），
   - 写入：`sendUserInput` 接收后先落库，再决定是否需要唤醒执行；
   - 提升：`runTurnAsync` 开始一轮时按顺序把条目提升为可见 `viewMessages`/上下文消息，并写 `promoted_seq`；
   - 崩溃后：重启时按 `promoted_seq` 是否为空判断哪些输入还没跑，交给 UI 明确询问「是否继续」。
   落地位置：`agent/lib/src/agent/io/session_server_agent_io.cpp`（队列段）、`agent/lib/src/agent/session_store.cpp`（新表）。收益：跨进程/崩溃不丢输入，多客户端接入时状态一致。
2. **[P0] 区分 `steer` 与 `queue`**：客户端消息协议里已经能表达「打断并立即执行」（`InterruptAndRunNext`），把它归纳成投递类型：`steer`（当前轮次结束的边界立即提升，会话若仍需要继续则继续）、`queue`（会话本该空转时才提升）。落地位置：`WireUserInput` 增加 `delivery` 字段 + `AgentRunner` 提升判定。收益：把现在靠 UI 手势表达的意图变成可测试的语义。
3. **[P1] 启动清账：跨进程遗留的工具状态收尾**：进程启动/会话恢复时，扫描上下文里状态为「执行中」的工具调用，统一落一条「工具执行被中断」的结果（opencode 的 `failInterruptedTools`），而不是让它悬空。落地位置：`BaseAgent` 初始化会话时（与 `AgentRunner` 中断缓存逻辑相邻）。收益：避免模型看到「调用中」的假状态后重复发起或误判。
4. **[P1] 提示词提交幂等**：给用户输入加可选 `id`，同 id 同内容视为重试、同 id 不同内容报冲突（返回明确错误码）。落地位置：`agent/lib/include/agentxx/agent/io/wire_protocol.h`。收益：断线重连/客户端重发不会重复提交。
5. **[P2] `active()` 快照语义**：明确「哪些会话正在运行」只统计前台轮次，后台子代理不计入（agentxx 已有子代理 `agent.progress` 事件，可据此派生 UI 状态而不是把父会话标成运行中）。落地位置：`SessionServerAgentIO` 的状态查询响应（可挂在 `WireContextStats` 同一族）。

---

## 3. 会话历史、LLM 上下文与上下文纪元

### 3.1 agentxx 现状

- **会话是 LLM 上下文的唯一权威**（2026-09 重构后的方向）：上下文以 typed `std::vector<neograph::ChatMessage>` 存在会话里，带单调递增 `messagesVersion`；Json 形态 `Session::llmMessagesJson()` 只在落库 / `WireGetContext` / 插件查询时惰性生成。
- 写入口收敛到 `Session::appendMessages` / `replaceMessages` / `replaceMessagesFromJson` / `truncateMessages`；节点与中间件经 `agentxx/nodes/session_context.h` 读写（`sessionMessages` 只读借用不得跨 `co_await`）。
- 图状态里**没有** `messages` 通道，只有只读影子通道 `xx_messagesMeta`（条数/版本/末尾消息摘要），使 `state.serialize()`（每 super-step checkpoint、VALUES 事件、插件 `stateJson`）的载荷与上下文大小无关。
- 展示历史是另一份 `viewMessages`（append-only，绝对下标恒定），服务端只同步尾窗，客户端向上滚动时按 `beforeIndex/count` 分页拉更早历史。
- system prompt 由 `agent/lib/src/agent/prompt.cpp` 拼装：基础提示词 + 记忆文件 + 技能清单 + 追加段（planning/worktree）+ 动态信息（时间、工作目录、环境事实）。**动态内容与静态内容混在一起，每轮重拼**。
- 更精确地说（本次细读 `ModelCallWrapNode::baseRun` 与 `AgentContext::buildSystemPrompt`）：每轮调用前会就地把首条 system 消息的内容整体重建为 `buildSystemPrompt(thread_id)` 的结果，并把这次替换标记为「派生内容、不请求节流落盘」；`buildSystemPrompt` 依次拼接配置主提示词 → `appendSystemPrompts`（planning / skill / codegraph 优先，其余键按序遍历，`summarization` 跳过）→ 本轮由中间件推入 `graphData["xx_appendSystemMessage"]` 的段落（记忆文件内容、技能名单）→ 会话占位符替换（工作目录取**不含 worktree 绑定**的基准值，会话临时目录、会话 ID）。也就是说：只要记忆文件或技能名单内容变了，整段 system 前缀就会变。

### 3.2 opencode 现状（Context Epoch + 类型化上下文来源）

`packages/core/src/system-context/index.ts` 把「特权系统上下文」建模为**可独立刷新的类型化来源**：

```ts
interface Source<A> {
  key: Key                                  // 稳定命名空间键，如 "opencode/environment"
  codec: Schema.Codec<A, Json, never, never>  // 值的编解码（用于比较与持久化）
  load: Effect.Effect<A | Unavailable>       // 观测；返回 unavailable 表示暂时不可用
  baseline: (current: A) => string           // 基线渲染
  update: (previous: A, current: A) => string // 变化渲染
  removed?: (previous: A) => string          // 撤销渲染（可选）
}
```

- `combine(...)` 按贡献键顺序组合（重复键直接失败）；`initialize` 观测一次得到**基线文本 + 结构化快照**；`reconcile` 比较当前值与快照，返回 `Unchanged | Updated | ReplacementReady | ReplacementBlocked` 之一。
- `unavailable` 与「已加载的空值」严格区分：观测失败时保留上次生效值（stale-while-revalidate），**不**当作被移除；重建基线时若有已生效来源不可用，返回 `ReplacementBlocked` 而不是构造不完整基线。
- 落库形态（`session_context_epoch` 表）：`baseline`（精确的模型可见文本）+ `snapshot`（各来源的 JSON 值 + 预渲染的移除文本）+ `baseline_seq`（该基线对应的会话事件序号）。
- **运行时纪律**（`specs/v2/session.md`）：
  - 基线在**第一个 provider 轮次之前**初始化；初始化被阻塞时，待提升输入保持待提升（可重试）；
  - 之后每个轮次在**安全边界**惰性采样：先提升新输入与结算工具结果，再合并本边界内所有变化为**一条**中间系统消息（chronological system message），并在同一事务里推进快照；
  - 压缩完成后开启**新的 Context Epoch**：直接按当前完整上下文渲染新基线（旧的中间系统消息保留为审计历史但退出模型可见历史）；
  - 模型/provider 切换**不**重开纪元（下一轮生效）；会话移动会清空纪元，目标位置必须重新初始化完整基线；
  - 上下文变化**不会**唤醒空闲会话：只在自然到来的轮次边界采样。
- 历史投影（`session/history.ts`）：`load`（对外 `sessions.context()`）与 `entriesForRunner`（给 runner，带 `baseline_seq`）分开；投影规则 = 「最近的压缩检查点之后的全部消息」+「基线序号之后的系统消息」，即**压缩边界与基线序号共同决定模型能看到什么**。
- 事件与分页：消息行保留来源事件序号（`seq`），`sessions.events({ sessionID, after })` 可按序号重放并可续接 tail；`sessions.history()` 是有限分页版本（默认 50、上限 100）。

### 3.3 对比

| 维度 | agentxx | opencode V2 | 评价 |
|---|---|---|---|
| 系统提示词构成 | 字符串拼装：静态段 + 动态段 + 追加段，每轮重拼 | 结构化来源：`baseline`（首轮渲染并持久化）+ 逐来源 `update`/`removed` 渲染 | opencode 的动态部分**可审计、可比较、可撤销**；agentxx 只能说「这一轮的 prompt 长这样」 |
| 缓存友好性 | 动态内容混在 system prompt 里，任何字段变化都会让 prompt 前缀失效 | 基线不动，变化以**追加消息**形式进入历史 | 对 Anthropic 显式缓存与 OpenAI 前缀缓存都更友好（长会话成本差异明显） |
| 不可用语义 | 无（取不到就用空值/旧值，缺少区分） | `unavailable`（保留旧值）vs「已加载的空值」（可发撤销消息） | 值得借鉴，尤其是环境探测、记忆文件读取这类会临时失败来源 |
| 上下文与事件的一致性 | 上下文与 viewMessages 分表，压紧靠版本号 | 消息行带来源事件 `seq`，与事件序列一致 | opencode 的顺序可重放更强（见 §5） |
| 会话整体形态 | 会话即上下文权威（typed + 版本号），方向与 opencode 一致 | 会话 = 事件序列 + 投影；上下文由投影 + 基线导出 | **共识**：两边都拒绝把上下文塞进图状态/状态快照 |
| 变更可见性 | 记忆文件/技能变化时无显式通知，模型只能看到新 prompt | 变化会成为一条「当前生效状态」的中间系统消息，移除时发撤销消息 | opencode 的模型可见性更好（模型知道自己看到的是**更新后**的事实） |

### 3.4 两者的优缺点

**agentxx**
- 优点：上下文写入口收敛（`appendMessages` / `replaceMessages` / `truncateMessages`）且有单调版本号，读方不会看到半更新状态；图状态里没有 `messages` 通道，checkpoint / 插件 `stateJson` 的载荷与上下文大小无关（长会话下这点很关键）；系统提示词渲染刻意排除「worktree 绑定」以免提示词随工具操作变化（源码注释说明「模型从工具结果得知，系统提示词不跟着变化」）；每轮开始会清空 `graphData`（`AgentStartCallWrapNode::onNodeStart`），避免跨轮残留；`repairMessages` 里已有 `system_prompt_hash` 校验，变更时会打 ERROR 日志。
- 缺点：系统提示词**每轮整体重建并就地替换首条 system 消息**（`ModelCallWrapNode::baseRun` 注释「系统提示词属每轮重建的派生内容」），记忆文件、技能名单、planning、插件追加段任意一处变化都会改变整段前缀，进而使 provider 前缀缓存失效（这正是 `system_prompt_hash` 会报 ERROR 的原因）；没有来源快照/比较，无法回答「这一轮比上一轮多了什么」；没有「取不到（保留旧值）」与「确实为空」的区分；每轮重新拼接、缺少按文件的内容缓存与变更检测。

**opencode**
- 优点：上下文是类型化来源（`key` + `codec` + `load`/`baseline`/`update`/`removed`），比较与渲染分离；基线一次性渲染并持久化（`session_context_epoch.baseline` + `snapshot`），后续变化只发一条中间系统消息；`Unavailable` 与「已加载为空」严格区分，重建基线受阻时返回 `ReplacementBlocked`（不会悄悄构造不完整基线）；只在安全边界惰性采样，不唤醒空闲会话；压缩完成/会话移动会重开纪元；技能指导来源自带「取代」与「撤销」文本（`skill/guidance.ts` 逐字写明）。
- 缺点：抽象层次多（`SystemContext.make/combine/initialize/reconcile/replace` + 纪元表 + 历史投影规则），调试要同时看事件、快照与投影；`CONTEXT.md` 自己把若干语义列为待定（插件定义来源、嵌套指令发现、`experimental.chat.system.transform` 的等价物）；V1 对齐表里仍有 `missing`（provider 家族基础指令、按提示词的 system 覆盖、steering 提醒），实际行为要区分 V1/V2。

### 3.5 可迁移到 agentxx 的设计

1. **[P0] 拆出「基线 / 增量」：把每轮变化的动态上下文改为追加式系统消息**
   - 首轮（或压缩后）渲染一次完整 system prompt，并把「各来源的值 + 渲染文本」写入会话持久化（可复用现有会话库，新增一列或小表）；
   - 之后每轮只比较**会变的来源**（时间/日期、工作目录、环境事实、记忆文件、技能清单、追加段），有变化就渲染一条**中间系统消息**追加到上下文（agentxx 的 `ChatMessage` 支持 system 角色，可直接落一条），并在同一事务里推进快照；
   - 落地位置：`agent/lib/src/agent/prompt.cpp`（来源化改造）+ `agent/lib/src/nodes/agentcall.cpp`（安全边界采样点）+ 会话库 schema（`agent/lib/src/agent/session_store.cpp`）。
   - 收益：长会话的 prompt 前缀稳定（缓存命中）、上下文变化有据可查（审计/回放）、模型能感知「现在的事实」。
2. **[P0] 来源级类型化与三态**：为每个动态来源定义 `key`、取值、基线渲染、变化渲染、移除渲染（可选），并区分「取不到（保留旧值）」与「确实为空（可发撤销消息）」。落地位置：新增 `agent/lib/include/agentxx/agent/context_sources.h`（或中间件内的来源注册表），由 `resource_applier`/`prompt` 使用。
3. **[P1] 压缩/移动会话后重建基线**：压缩完成与会话工作目录切换（worktree 绑定/解绑）时清空「基线」记录并强制下一轮重渲染完整基线，避免新旧混杂。落地位置：`summarization.cpp`（压缩完成回调）与 worktree 工具绑定路径。
4. **[P1] 上下文变化只在安全边界采样**：明确「不使用异步推送改上下文」，只在轮次开始（提升输入、结算工具之后）比较与合并，避免同一轮内上下文漂移导致请求与历史不一致。落地位置：`ModelCallWrapNode` 的请求组装前。
5. **[P2] 对外暴露上下文来源（插件）**：`agentxx.agent.context` 表已有 `get_messages`/`messages_count`，可再补「当前生效的上下文来源与快照」，让插件与 UI 能展示「模型此刻看到的系统事实」。落地位置：`plugin_manager_vtable.cpp` + `agentxx.agent.context` 表。

---

## 4. 会话位置、工作区隔离与移动

### 4.1 agentxx 现状

- **会话工作目录多源回退**：`AgentContext::getSessionWorkDir` 按「worktree 绑定 > 会话工作目录覆写 > 配置 workDir > 进程 cwd」取值；会话临时目录与提示词占位符都基于它。
- **worktree 模式**（yaml `worktree.enable`，默认关闭）：`agentxx_git_worktree` 工具创建/绑定 worktree；权限中间件按会话记录 `SessionFsIsolation{allowPath, denyWritePath}`，命中主检出子树的写操作直接拒绝（隔离优先于白名单）。
- 权限路径规范化（`normalizePermissionPath`）以会话生效工作目录为基准，目录/文件尾斜杠语义显式区分；跨设备附件选择会区分「本地 / 服务端」两页签（服务端线程池扫描目录，路径全程 UTF-8）。
- 会话与目录是**单一绑定**：一个会话一个工作目录（可覆写），没有「会话在不同位置之间移动」的概念，也没有「服务按位置缓存」的概念。

### 4.2 opencode 现状

- **Location 是一等概念**（`packages/core/src/location.ts`）：`{ directory, workspaceID }`；`LocationServiceMap` 按 Location 缓存 `SessionRunner`、catalog、模型解析、工具注册表、权限状态、文件系统等服务；省略 `workspaceID` 表示隐式本地放置。
- **会话属于某个 Location**（`session.location`），因此 `SessionExecution.resume(sessionID)` 的路径是：`SessionStore.get(sessionID)` → `LocationServiceMap.get(session.location)` → 该 Location 的 runner 执行；**没有任何层直接吃一个 sessionID 去猜服务**。
- **会话可移动**（`session/move` + `packages/core/test/move-session.test.ts`）：移动会清空当前 Context Epoch，目标位置在下次运行前必须初始化完整基线；「Location 作用域的服务在会话移动后会自然重新解析生效上下文」。
- **文件夹/项目多源**：`ProjectV2`/`project-directories` 支持项目级目录集合；`FileSystem` 服务提供四种根（`workspace`/`directory`/`worktree`/`state`）并要求工具显式选择（`specs/v2/tools.md` 的 `filesystem.resolveRoot(input)`）。
- **权限以 Location 为边界**：`PermissionSaved` 按 `projectID` 存批准记录（区分项目）；文件系统授权按规范化后的资源身份判断（拒绝绝对路径、路径逃逸、符号链接逃逸）。
- **工作树（worktree）**：通过 workspace adapter 体系（`experimental_workspace.register(type, adapter)`）处理，比 agentxx 的单一 git worktree 工具更泛化但也更薄。

### 4.3 对比

| 维度 | agentxx | opencode | 评价 |
|---|---|---|---|
| 位置与服务的绑定 | 隐含：单进程单实例服务，工作目录按会话取值 | 显式：Location 作用域服务缓存 + 会话归属位置 | agentxx 单进程多会话场景下够用；一旦要「同一进程服务多个项目/远端工作区」，显式化是必需的 |
| 会话移动 | 无 | 有（清 epoch，目标位置重初始化） | 可借鉴的**边界纪律**：移动即失效派生态 |
| 多根文件系统 | 单工作目录 + worktree 隔离 | 四种根 + 工具显式选根 | opencode 更泛化；agentxx 的 worktree 写边界更实用（真实编码场景最需要的一条） |
| 工作区隔离 | 权限层强制写边界（隔离优先于白名单） | 由 workspace/沙箱外置（文档层面），权限层只管资源身份 | **agentxx 更强**：写边界在权限层硬保证 |
| 跨设备/跨端文件 | 附件选择支持本地/服务端两页签 | 附件 + managed 输出目录 + PTY 的宿主叠加环境 | 各有侧重：agentxx 面向「远程 agent + 本地文件」，opencode 面向「同机多端 UI」 |
| 远端工作区 | 通过 A2A/远程 agent 桥接 | workspace adapter 的 `remote` target（URL + headers） | 思路相同（远端即另一种位置），agentxx 已有 A2A 实现 |

### 4.4 两者的优缺点

**agentxx**
- 优点：会话工作目录按多源回退取值（worktree 绑定 > 会话覆写 > 配置 > 进程 cwd），符合真实使用；worktree 的写边界在**权限层**硬保证（隔离优先于白名单，见 `SessionFsIsolation` 注释），不依赖工具自觉；路径规范化显式区分目录与文件（尾斜杠语义），避免对文件请求权限时被加上 `/`；跨设备附件选择区分「本地 / 服务端」，服务端路径只传 `pathOrUrl` 由服务端读取（不塞 base64）。
- 缺点：没有显式的「会话工作上下文」对象，工作目录解析（`getSessionWorkDir`）与权限基准（`normalizePermissionPath`）分散在两处，容易不一致；没有会话迁移语义：换工作目录/换 worktree 只是工具行为，上下文基线、权限缓存等派生态不会失效；工具不能声明「我要用哪个根」，实现里直接取会话目录。

**opencode**
- 优点：`Location` 是一等概念，服务（runner/catalog/模型解析/工具注册表/权限/文件系统）按 Location 缓存，「从会话 id 找服务」只发生在 `SessionExecution → LocationServiceMap` 这一处；会话带位置且可移动（移动清空 Context Epoch，目标位置重建基线）；文件系统提供四种根并要求工具显式选择（`filesystem.resolveRoot(input)`）；文件系统授权按规范化后的资源身份判断（拒绝绝对路径、路径逃逸、符号链接逃逸）。
- 缺点：位置/工作区抽象对「单机单项目」场景偏重；`workspace adapter` 的能力比 agentxx 的 git worktree 工具薄（worktree 隔离不在权限层强制）；远端工作区只有 `{url, headers}` 目标形态，跨设备文件选择这类场景没有 agentxx 完整。

### 4.5 可迁移到 agentxx 的设计

1. **[P1] 显式化「会话工作上下文」并在关键切换点失效派生态**：把「会话生效工作目录 + 隔离边界」收敛成一个显式对象（`SessionWorkContext{directory, worktree, isolation}`），并规定：
   - worktree 绑定/解绑、工作目录覆写变更 → 清空「上下文基线」（§3 的基线记录），下一轮重渲染；
   - 权限路径规范化、工具执行、临时目录统一从该对象取值（现在散在 `getSessionWorkDir` 与权限中间件两处）。
   落地位置：`agent/lib/include/agentxx/agent/context.h` + `middlewares/permission.cpp`。
2. **[P1] 会话移动的语义（跨设备/跨工作区）**：现有「切换会话」是切到另一个会话 id；可补一条「同一会话换工作目录/换 worktree」的操作，规则参考 opencode：移动后旧基线、旧 delta 重放缓冲、旧权限批准缓存全部失效，需重新初始化。落地位置：`WireSwitchSession` 同族新增消息 + `SessionServerAgentIO::switchSession`。
3. **[P2] 工具显式声明「用哪个根」**：agentxx 的工具实现直接接工作目录；可在工具定义里加「根选择」（工作区/会话目录/worktree/临时目录），由宿主在调用前解析。收益：工具在 worktree 模式下行为可预期、可测试。落地位置：`XXToolBase` + `ToolcallWrapNode` 的调用前处理。
4. **[P2] 远端工作区与 A2A 的统一**：把「远端 agent 的工作目录」也表达为一种位置（而不是另一种协议），便于 UI 与权限层统一处理。

---

## 5. 持久化、事件序列与崩溃恢复

### 5.1 agentxx 现状

会话持久化（`{dataDir}/sqlite/sessions/{sessionId}/session.db`，WAL + busy_timeout，四张表）：

| 表 | 内容 | 写策略 |
|---|---|---|
| `view_message` | `viewMessages` 展示历史（append-only，每消息一行 JSON） | 追加/回填经**节流器**（`kPersistThrottleMs = 3s`），首次立即落盘，窗口内合并，轮末强制补存 |
| `llm_context` | 会话 LLM 上下文（单行整体替换） | 轮末整表替换（权威终态）；轮内变更只更新内存 |
| `meta` | `msgIdCounter` / `title` / `lastActiveMs` | 随同事务提交 |
| `store` | `agentxx_share_store` KV 条目（id 自增） | 写穿 DB + 每会话 3 条 LRU 内存缓存 |

- 节流队列只记录 `viewMessages` 下标，不持有消息副本（避免窗口内双份内存）；`Session::restore` 前先清空队列。
- 连接按会话 LRU 缓存（上限 `kMaxOpenSessionDbs = 32`），只关连接不丢数据。
- 容错取向：**所有落库失败只记日志**，不影响内存状态与主流程（尽力而为持久化）。
- 图侧另有 checkpoint（`agent/checkpoint_store.h`，节点中断时的图状态）与中断工具结果缓存（`interruptToolcallCache`）；`viewMessages` 用链式哈希校验（重启恢复时重建）。
- 传输侧：`SessionServerAgentIO` 在内存里保留 delta 重放缓冲，客户端重连带 `lastSeq` 增量续传，不连续或水位过高时回退全量 `Sync`；服务端重启后 seq 从 0 重新计数。

### 5.2 opencode 现状（聚合事件序列 + 投影 + 游标重放）

- **两层写模型**：
  1. `event` 表 + `event_sequence` 表：每个 durable 事件属于一个聚合（会话），聚合内有单调递增 `seq`（`EventSequenceTable` 记录当前 `seq` 与 `owner_id`），写入在**一个 SQLite 事务**里完成「读当前 seq → 插入事件 → 更新 seq → 执行可选 `commit(seq)` 本地投影」；
  2. 投影表：`session_message`（会话可见消息，带 `seq` 与类型）、`session`（会话信息）、`session_input`（收件箱）、`session_context_epoch`（上下文纪元）。
- **投影器（projector）**在事件事务的 `commit(seq)` 里同步更新投影表，所以「事件写入」与「读模型更新」是原子的；投影本身不重放（`PUBLISH` 的 `commit` 明确标注「本地操作投影，不参与重放与序列化」）。
- 本次细读 `session/projector.ts` 与 `session/message-updater.ts` 得到的几个具体事实：
  - 同一个投影器同时挂 V1 与 V2 两组事件（迁移期：旧 `SessionV1.Event.*` 写 `message`/`part` 旧表与 `session` 行，新 `SessionEvent.*` 写 `session_message`），由 `SessionMessageUpdater` 用 immer `produce` 把事件折叠为投影消息（文本、推理、工具参数与状态机、压缩、切换 agent/模型、会话消息）；
  - **流式 delta 事件没有注册投影**（`text.delta` / `reasoning.delta` / `tool.input.delta` 只在内存投影里追加），落到 durable 的只有 `started` / `ended` 与工具生命周期事件 —— 这是「实时流不参与重放」的代码依据；
  - 会话级用量账本由 V1 的 `step-finish` part 维护：写入时累加 `cost`/`tokens`，part 或消息被删除时按 `-1` 反向调整（`applyUsage(..., sign)`）；
  - `revert.commit` 会真正删除边界之后的消息行，以及 `admitted_seq` 或 `promoted_seq` 超过该边界的收件箱行（历史可截断）；
  - `session.next.retried` 在投影器里**被注释掉**（`// yield* events.project(SessionEvent.Retried, ...)`），即重试通知目前不进入投影 —— 属于「事件已定义但未投影」的现状。
- **可重放与分页**：
  - `Session.events({ sessionID, after })` 返回该会话 `after` 之后的 durable 事件流，并在之后继续 tail 新事件（先订阅再回放，避免缝隙）；
  - `Session.history({ sessionID, after, limit })` 是有限分页版本（默认 50、上限 100），返回 `{ events, hasMore }`；
  - 事件类型带版本（`durable-event-manifest` 的 `versionedType`，如 `session.next.compaction.started.1`），反序列化时按 manifest 校验；
  - `replay` / `replayAll` + `claim(aggregateID, ownerID)` 支持「同步重建投影」并按 owner 拒绝重复重放。
- **崩溃语义的显式化**：
  - 启动/新一轮之前把上次进程遗留的 `pending`/`running` 工具调用统一判为 `Tool execution interrupted`（`failInterruptedTools`），不静默重放；
  - 压缩有 `compaction.started` / `compaction.ended` 两个 durable 事件，**只有 ended 才会投影出模型可见的压缩消息**，所以「中途被杀」不会污染历史；
  - 上下文纪元快照与对应的中间系统消息在同一事务里推进（事件 `commit` 回调里 `advance(...)`）。
- **不做的事**：不做整库事件溯源（只对会话相关的关键事实建事件）；不做「崩溃后自动继续 provider 工作」；跨节点执行所有权（fencing）明确标为未实现。

### 5.3 对比

| 维度 | agentxx | opencode | 评价 |
|---|---|---|---|
| 会话内顺序 | `viewMessages` 绝对下标 + LLM 上下文整体替换 | 每个会话一条事件序列（`seq` 单调）+ 投影行带 `seq` | opencode 的**顺序可重放**更强，客户端与插件都能用同一游标；agentxx 只有内存 delta 缓冲 |
| 断线增量同步 | 服务端内存 delta 缓冲 + `lastSeq`，服务端重启后回退全量 | durable 事件流 `after` 游标，跨进程重启仍可续传 | agentxx 在服务重启后必须全量重传，长会话代价高 |
| 写放大与成本 | 节流 3s + 轮末整表替换上下文（写入量小、实现简单） | 每事件一行 + 投影行（写入更频繁、读模型清晰） | agentxx 的做法对超长上下文更省 I/O；opencode 的粒度让「重放/增量/审计」天然可行 |
| 崩溃后一致性 | 节流窗口内最多丢 <3s 的 view；上下文以轮末为准（可能落后一轮） | 事件 + 投影同事务；压缩/工具/上下文都有完成事件 | opencode 的**完成事件边界**（started/ended）值得借鉴 |
| 容错取向 | 落库失败只记日志，不阻塞主流程 | 数据库写失败 `orDie`（视为不可恢复缺陷），宁可失败也不静默降级 | 两种取向都有代价：agentxx 是可用性优先，opencode 是一致性优先；agentxx 的「静默降级」需要更明确的用户可见提示 |
| 版本演进 | yaml/表结构变更靠忽略与告警 | 事件类型带 `.version` 后缀 + manifest 校验 + schema changelog | 可借鉴：给持久化格式一个显式版本 |

### 5.4 两者的优缺点

**agentxx**
- 优点：节流写（`kPersistThrottleMs = 3s`，队列只记下标不复制消息）+ 轮末权威落盘，对「反复被杀」的会话很友好（最多丢一个窗口而非整轮）；`viewMessages` append-only 让绝对下标稳定，前插分页无竞态；连接按会话 LRU（上限 32）回收，避免 fd 耗尽；落库失败只记日志、不阻塞对话（可用性优先）；四表结构简单直观。
- 缺点：没有事件序列与重放游标 —— 断线增量同步依赖服务端内存缓冲，服务端重启后必须全量重传；没有「完成边界」，压缩写回即生效，中途被杀看不出「上次压缩是否完成」；持久化格式无版本号，也无旧数据兼容测试；落库失败只写日志，用户无从得知历史可能缺失；`llm_context` 轮末整表替换，轮内崩溃会丢一轮上下文（与 viewMessages 的节流粒度不一致）。

**opencode**
- 优点：聚合内单调 `seq` + 事件事务里同步投影，事件与读模型原子一致；`after` 游标可跨重启重放，`sessions.history` 与 `sessions.events` 满足不同客户端；事件类型带版本（`...started.1`）；`revert.commit` 会真正删除边界之后的消息行与未提升的收件箱行（历史可截断）；会话级用量账本（cost / tokens 含 cache read/write），删除 part 时按 `-1` 反向调整；流式 delta **刻意不入 durable**（只落 `started`/`ended` 边界），避免写放大。
- 缺点：每个 durable 事件一次事务 + 投影写入，写放大比 agentxx 明显（这也是必须把 delta 排除在 durable 之外的根因）；数据库错误按不可恢复缺陷处理（`Effect.orDie`），取向与 agentxx 的「尽力而为」相反；事件与投影两份模型增加理解与迁移成本；个别事件已定义但未投影（如 `session.next.retried` 在 `projector.ts` 中被注释掉），只看文档会误判现状。

### 5.5 可迁移到 agentxx 的设计

1. **[P0] 会话事件序列 + `after` 游标重放（不引入整库事件溯源）**
   - 在会话库新增 `event` 表（`session_id`, `seq`, `type`, `version`, `payload`, `time_created`，`unique(session_id, seq)`）与 `event_sequence` 计数行；
   - 把**关键事实**事件化：消息追加、工具结算（成功/失败/被中断）、压缩开始/结束、上下文基线重建、权限决定、轮次开始/结束；
   - `viewMessages`/上下文仍是读模型（可继续用现在的表与节流策略），事件表用于「增量同步 + 审计 + 崩溃判断」；
   - 客户端重连时先按 `after=lastSeq` 拉事件补齐，再进入实时 delta；服务端重启后只要事件表还在，就不必回退全量。
   落地位置：`agent/lib/src/agent/session_store.cpp`（新表 + API）、`agent/lib/src/agent/io/session_server_agent_io.cpp`（重放缓冲改为「先读库、再续实时」）、`agent/lib/include/agentxx/agent/io/wire_protocol.h`（`WireGetHistory{after, limit}` / `WireHistory{events, hasMore}`）。
2. **[P0] 完成事件边界**：对「有中间态的长操作」统一 started/ended 两事件（压缩是现成例子），只有 ended 才投影为模型可见或持久生效的内容。落地位置：`summarization.cpp`、`ToolcallWrapNode`。
3. **[P1] 持久化格式版本**：事件与表结构都带版本号，读取时按版本分派；旧版本数据给出显式迁移或拒绝，而不是静默忽略（对比现在 yaml 旧键「告警并忽略」的做法）。落地位置：`session_store.cpp` + `util/settings_db.h`。
4. **[P1] 可见降级**：落库失败目前只写日志；建议在会话上累计「持久化降级」状态并经 `WireContextStats`/事件推给 UI（让用户知道历史可能缺失），同时保留「不阻塞主流程」的取向。落地位置：`SessionStore` + `SessionServerAgentIO` 状态推送。
5. **[P2] 重放所有权（claim/owner）**：为将来的「投影重建」预留 `owner_id`（避免重复重放），当前可以不实现分布式语义，只做单进程校验。落地位置：`EventSequenceTable` 等价结构。

---

## 6. 工具系统

### 6.1 agentxx 现状

- 工具基类 `XXToolBase`（继承 `neograph::Tool`）暴露策略开关：
  - `autoSummaryOutput`：输出超长时按 `toolcallSummaryLimitOutputLength` 压缩；
  - `canDelayLoad`：初始只在 system prompt 里给名称等简略信息，由 `tool_skill_search` 检索后再加载全量定义（延迟加载）；
  - `maxRetry`：执行抛异常时重试（最多 `1 + maxRetry` 次）；
  - `repeatCallCheck`：连续相同调用（同一 llm↔tool 交替链内，阈值默认 5）经 permission 总线询问用户；
- `ToolcallWrapNode` 负责：参数类型自动修正（`autoFixArgsType`：字符串↔数组、字符串↔数值、布尔↔字符串、单元素数组解包）、权限检查、取消检查、执行、结果写回上下文、重复调用检测（`findConsecutiveRepeatCallKeys` 带回溯上界）。
- 执行模型：`for (const auto& toolcall : toolcalls)` **串行**逐个 `co_await execTool(...)`（源码标注 `// TODO: 真正并行`）；被取消时已完成的工具结果写入 `interruptToolcallCache` 后再重抛。
- 输出处理：超过限制的输出由压缩中间件（`createSummarizationToolHandle`）做摘要/截断；**没有**「完整输出另存为文件并可回取」的路径。
- 工具结果统一是字符串；UI 渲染由插件侧渲染器（`builtin_tool_renderers`、`agentxx.ui.item`）负责，与工具返回值之间没有结构化契约。

### 6.2 opencode 现状

- **一个不透明工具类型**（`packages/core/src/tool/tool.ts`）：

```ts
Tool.make({ description, input, output, structured?, toModelOutput?, execute })
```

  - `input`/`output` 是 Effect Schema：**输入先解码再执行，输出先编码再投影**（非法输入不会调用实现，非法输出不会被当成成功结算）；
  - `toModelOutput` 是纯函数：把「已验证的输出」投影为发给模型的文本/文件内容，未被投影时结构化输出保留；
  - 工具**没有内在名字**，名字在注册时给（同一工具值可在不同位置以不同名字注册）。
- **两级注册 + 覆盖语义**：`ApplicationTools`（进程级，全局）与 Location 级 `ToolRegistry`（每个 Location 一份，覆盖应用级）；同一名字「最新的活动注册生效」，关闭一个注册只移除自己，被覆盖的下层重新露出（Scope/finalizer 实现）。
- **物化 + 结算分离**（`tool/registry.ts`）：

```
materialize(permissions) → { definitions, settle }
  materialize: 按权限规则剔除「整体禁用」的工具，冻结每个名字当时的注册身份（identity）
  settle(call): 1) 有效注册查找 2) 身份比对（不一致 → "Stale tool call: name"）
                3) 解码输入 4) 执行 5) 编码输出 6) 投影内容 7) 统一限幅 8) 返回结算 + 托管输出路径
```

- **调用上下文**是固定四元组：`{ sessionID, agent, assistantMessageID, toolCallID }`（由 runner 提供，registry 不推断；结算事件带 assistant 消息 id，因为 provider 的 call id 可能跨轮重复）。
- **权限由工具自己发起**：可信工具（内置/受信插件）调用 `PermissionV2.assert({ action, resources, source: {type:"tool", messageID, callID} })`；registry **不**注入权限辅助函数。
- **输出限幅（`tool-output-store.ts`）**：默认 `MAX_LINES = 2_000` / `MAX_BYTES = 50 KiB`（取先到者）；超限时保留头尾预览、把**完整文本**写入共享 `tool-output` 目录（全局唯一名、保留 7 天），预览里给出托管路径；结构化结果不被路径污染；**完整输出保留失败时结算失败（不发布有损成功）**。
- **执行模型**：本地工具调用「先持久化调用，再立即并行执行（FiberSet），等全部结算后重载历史继续」；中断时不清空已开始的工作，等结算或按中断收尾。
- 本次细读 `tool/bash.ts`、`tool/question.ts`、`tool/todowrite.ts`、`tool-output-store.ts` 得到的具体参数与约定：
  - `bash`：默认超时 2 分钟、上限 10 分钟；单次捕获上限 1 MiB（超出加「capture truncated」提示）；输出结构化字段为 `{exit, truncated, timeout}`，模型可见文本是「输出 + 退出码说明」两段；执行前先按 `workdir` 解析位置，跨出位置时走 `external_directory` 权限断言，再对命令本身断言（`save: [input.command]`，即「始终允许」记忆的是这条命令）；对命令里的绝对路径只给「提示性警告」（工具描述里写明不是沙箱边界）；文件里保留了 12 条 TODO（tree-sitter 解析、BashArity 前缀批准、插件 `shell.env`、持久化后台任务、把完整输出流式写入托管存储等）。
  - `question` / `todowrite`：都先 `permission.assert({ action: "question" | "todowrite", resources: ["*"] })` 再落库/发问；`question` 的结果按 `"问题"="答案"` 拼成模型可见文本。
  - 输出托管：文本按「先到者」为准（默认 2 000 行 / 50 KiB），超限保留头尾预览并把完整文本写入共享 `tool-output` 目录（绝对路径可被普通工具读取），结构化字段不受路径污染。
- 内置工具集：`bash`、`read`（分页 + 二进制 base64）、`write`、`edit`、`apply_patch`（add/update/delete，逐 hunk 解析、批量批准、部分应用显式报告）、`glob`、`grep`、`skill`、`todowrite`、`webfetch`、`websearch`、`question`。

### 6.3 对比

| 维度 | agentxx | opencode | 评价 |
|---|---|---|---|
| 定义形态 | JSON Schema（`neograph::ChatTool`）+ 字符串返回值 | Schema codec 输入/输出 + 纯投影函数 | opencode 的「解码/执行/编码/投影」四段让错误边界清晰；agentxx 靠 `autoFixArgsType` 做容错，属于「先修再试」 |
| 结果形态 | 单个字符串 | 结构化结果 + 模型文本投影 + 文件/媒体内容 | agentxx 的 UI 渲染需要解析字符串；结构化后 UI/插件/后续处理都更稳 |
| 注册与覆盖 | 插件注册 + 宿主表（`agentxx.agent.tools`）；重名覆盖靠注册表实现 | Scope 化两级覆盖，关闭即露出下层 | opencode 的「可叠加 + 可回退」语义更完整（插件热重载时尤其明显） |
| 陈旧调用 | 无「公告身份」概念：工具被替换后调用会打到新实现 | 物化时冻结身份，结算时不一致就报 stale | 可借鉴，插件启用/禁用/热重载时防意外 |
| 并行 | **串行**（`// TODO: 真正并行`，取消时缓存已完成结果） | 先持久化调用，再并行执行，等全部结算 | 迁移价值最高的单点性能改进 |
| 输出超限 | 压缩/截断（内容丢失） | 头尾预览 + 完整文本落托管文件（7 天）+ 路径可读 | agentxx 直接丢内容；长输出场景（grep/命令）很常见 |
| 重试与循环检测 | 有（`maxRetry`、`repeatCallCheck` + 用户询问） | 无（规格里明确「不做重复相同工具调用的限制」仍是未完成项） | **agentxx 更强**，可反向输出 |
| 延迟加载 | 有（`tool_skill_search` 检索后加载） | 无（工具定义全部物化给模型，靠权限剔除） | **agentxx 更强**：工具很多时节省 token |
| 工具数量/一次调用 | 一次响应可有多个工具调用，串行执行 | 一次响应多个调用，并行执行，结算按 call id 回归 | opencode 更快；agentxx 的「按公告顺序串行」更简单但慢 |

### 6.4 两者的优缺点

**agentxx**
- 优点：工具特性丰富且都落在 `XXToolBase` 开关上（输出自动压缩、延迟加载 + 检索、失败重试、连续重复调用询问），插件声明即可；`autoFixArgsType` 对模型输出的类型错误容错强（字符串↔数组/数值/布尔、单元素数组解包）；重复调用检测带回溯上界与提前终止，长上下文下开销可控（源码注释写明终止条件）；权限声明 + 逐路径三态复核让 glob/grep 这类模式工具不会绕过子目录拒绝规则。
- 缺点：同轮多工具**串行**执行（源码 `// TODO: 真正并行`），多工具轮次的延迟明显；工具结果只有字符串，UI 与插件要靠解析；超限输出被压缩丢弃（内容不可回取）；没有「公告身份」，插件热重载/禁用后旧调用会打到新实现；取消时靠 `interruptToolcallCache` 在同进程内补结果，跨进程没有收尾；工具名与实现绑定，缺少「同一实现多注册/覆盖与回退」语义。

**opencode**
- 优点：输入/输出各有 codec，非法输入不执行、非法输出不算成功结算；执行结果与模型可见投影分离（`toModelOutput` 是纯函数）；两级 Scope 化注册（关闭一层露出下一层，插件禁用不需要手工撤销）；物化时冻结注册身份、结算时比对，替换过就报 `Stale tool call`；调用先持久化、再立即并行执行（FiberSet）、等全部结算后重载历史，取消时不清空已开始的工作；超限输出保留头尾预览并把完整文本写入托管文件（保留失败即结算失败）；`bash` 工具的输入/输出/超时/权限断言/提示性警告都显式写在工具定义里，并把未补齐的兼容项写成 TODO 清单。
- 缺点：没有重试，也没有重复相同调用的限制（规格把这两项列为待办）；所有工具定义都要物化给模型，缺少 agentxx 的延迟加载，工具多时 token 成本高；权限由工具自己 `assert`，一致性依赖每个工具实现（漏写就是漏判）；`bash` 明确不做沙箱；Windows/PowerShell 处理尚未补齐。

### 6.5 可迁移到 agentxx 的设计

1. **[P0] 同轮工具并行执行 + 结算按调用 id 归位**
   - 在 `ToolcallWrapNode::baseRun` 里把「解析参数 → 权限检查 → 执行 → 写回结果」拆开：解析与权限检查保持串行（顺序确定、询问卡片顺序稳定），**执行阶段并发**（每个工具一个协程），全部完成后按原始 `tool_call_id` 顺序写回上下文；
   - 取消语义：轮内取消时，把已完成的结果与未开始调用的占位统一收尾（复用现有 `interruptToolcallCache` 逻辑），保证上下文不出现「调用存在但无结果」的悬空；
   - 需要保护点：`getSessionCancelToken`（按会话取令牌）、权限询问（同一时间只发一次询问或多询问并发需去重）、工具内部的线程安全假设（工作线程池、`offload`）。
   落地位置：`agent/lib/src/nodes/toolcall.cpp`（`// TODO: 真正并行` 处）、`agent/lib/src/tools/*`。
2. **[P1] 公告身份防陈旧调用**：工具调用发起时记录「本次广告给模型的工具定义身份」（可用工具名 + 定义版本/插件实例 id + 定义哈希），结算前比对；不一致时把该调用结算为「工具已变更，请重新调用」，而不是执行新实现。落地位置：`ToolcallWrapNode` + `protocol`（请求里带工具定义 fingerprint，落地在 `ModelCallWrapNode` 的请求组装处）。
3. **[P1] 大输出保真：托管文件 + 头尾预览**
   - 超限输出不丢内容：完整文本写入 `{dataDir}/tool-output/`（全局唯一名，含会话与调用 id 便于排查），消息里给出头尾预览 + 文件路径；
   - 保留策略与清理：定期清理（参考 7 天）或按容量上限（LRU）；
   - 失败语义：写文件失败时，退化为当前的有损截断，但要在结果里标注「完整输出未能保存」。
   落地位置：`agent/lib/src/middlewares/summarization.cpp`（现有压缩路径旁）+ 新 `tool-output` 模块；与 `agentxx_filesystem_read` 配合即可回取。
4. **[P1] 结构化工具结果 + 模型可见投影分离**：在 `XXToolBase::execute_async` 之外增加可选的「结构化结果」出口（JSON），由宿主负责「投影为模型文本」与「交给 UI/插件」。收益：现有插件渲染器（`agentxx.ui.item`）可直接吃结构化结果，不再解析文本；工具输出限幅也可以按「文本部分」计量而不动结构化字段。落地位置：`agent/lib/include/agentxx/tools/tool.h` + `ToolcallWrapNode` 的写回段。
5. **[P2] 工具返回「取消不是结果」的一致性**：明确「中断/取消不写模型可见结果」（现在是 `CancelledException` 传播，但插件适配层与 offload 适配可能各自转换），在 `ToolcallWrapNode` 收尾处加统一断言。落地位置：`toolcall.cpp`。

---

## 7. 系统提示词、指令上下文与技能

### 7.1 agentxx 现状

- 提示词在 `agent/lib/src/agent/prompt.cpp` 拼装：基础提示词 + 工具定义/简略清单（延迟加载工具只给名字）+ 追加段（planning 的任务计划、worktree 的隔离说明）+ 中间件注入段。
- **MemoryFileMiddleware**：首轮读取配置的上下文文件（如 `AGENTS.md`），缓存内容并在每次模型调用时注入系统提示词；插件可在运行期增删文件（`addMemoryFiles`/`removeMemoryFiles`，靠 `resourceEpoch` 失效缓存，下次轮次全量重读）。
- **SkillMiddleware**：扫描技能目录，把「技能名 + 描述」列表按 Agent Skills 规范渲染进提示词（渐进式披露：正文由模型用 `agentxx_filesystem_read` 按需读取）；目录变更同样用 `resourceEpoch` 失效重扫；`readSkillFile` 解析 SKILL.md 的 frontmatter（name/description/license/compatibility/metadata/allowed_tools）。
- 指令文件与技能清单**只是提示词文本**：模型看不到「这一轮比上一轮多了/少了什么」，也没有权限过滤（谁能读到哪个技能取决于文件系统权限与模型自觉）。

### 7.2 opencode 现状

- **指令发现**（`packages/core/src/instruction-context.ts` + `test/instruction-context.test.ts`）：全局指令 + 向上逐级查找的项目 `AGENTS.md`，作为**一个有序聚合的 Context Source** 参与上下文纪元；
  - 路径先规范化，且遍历**限制在项目根内**（不允许逃逸）；
  - 尊重 `OPENCODE_DISABLE_PROJECT_CONFIG`（禁用项目配置，全局指令仍可用）；
  - 指令集合变化时，中间系统消息给出**当前完整的指令集合**并显式声明「取代此前内容」；最后一条指令被移除时发出**撤销消息**（而不是默默消失）。
- **技能指导也是 Context Source**：只列出该 agent 被允许的技能**名称与描述**；技能正文与位置只通过**权限检查过的 `skill` 工具**暴露；被全局拒绝的技能定义在「请求期工具物化」时移除（模型看不到它，也无法调用）。
- **请求组装**（`session/runner/llm.ts`）：`system: [agent.info?.system, system.baseline]` —— 即「agent 自带系统提示词」+「基线系统上下文」两部分拼接成 system 段；随后是投影历史 + 工具定义；最后一轮用 `MAX_STEPS_PROMPT` 追加一条 assistant 消息并把 `toolChoice` 置为 `none`。
- **未完成项**（`specs/v2/session.md` 的 V1 对齐清单逐条标注）：provider 家族基础指令、按 prompt 的 system 覆盖、steering/计划提醒、插件 message/system 变换等在 V2 尚无对应实现，仍在 V1 路径。

### 7.3 对比

| 维度 | agentxx | opencode | 评价 |
|---|---|---|---|
| 指令来源发现 | 配置路径列表（可含 glob）+ 插件运行期增删 | 全局 + 向上查找项目 `AGENTS.md`，规范化且限制在项目根内，可用环境变量禁用 | opencode 的「向上查找 + 边界限制」对多级仓库更友好；agentxx 的「可配置 + 可插件增删」更灵活 |
| 变化可见性 | 无（内容变了，模型只看到新文本） | 显式中间系统消息：给出完整集合与取代/撤销语义 | 可借鉴，尤其对长会话与缓存友好（§3） |
| 技能披露 | 名单 + 描述进提示词，正文按需读取 | 名单 + 描述进上下文来源，正文走权限检查工具 | 两者理念一致（渐进式披露）；opencode 多了**权限过滤** |
| 权限过滤 | 无（技能可见性由文件系统权限决定） | 按 agent 过滤 + 全局拒绝的定义直接不进工具表 | 可借鉴：把「该 agent 能看到哪些技能/工具」显式化 |
| 缓存与重载 | `resourceEpoch` 显式失效重建（实现干净） | Scope 关闭即重算 + Cache 提示 | agentxx 的做法简单直接，保留 |
| 提示词结构 | 字符串拼装 | 片段 + 来源 + 快照 | 见 §3 |

### 7.4 两者的优缺点

**agentxx**
- 优点：技能用渐进式披露（名单+描述进提示词，正文由模型按需 `read`），与 opencode 同思路；技能目录与记忆文件支持**运行期增删**（插件调用 `addSkillDirs`/`addMemoryFiles`），靠 `resourceEpoch` 失效缓存，下次轮次自动重建；提示词模板可定制（`appendSystemPrompts` 可由 yaml 配置或训练序列化）；渲染有会话级占位符（工作目录/临时目录/会话 ID），并把 worktree 排除在外以保持稳定。
- 缺点：技能正文走通用 `agentxx_filesystem_read`，没有「读取技能」的语义与权限分类；技能名单与指令内容变化**不会通知模型**（模型只看到新的文本）；技能的可见性不按 agent 过滤；提示词各段的拼接顺序与优先级只体现在 `AgentContext::buildSystemPrompt` 的代码里，没有文档化规则。

**opencode**
- 优点：指令发现做了规范化、限制在项目根内查找、并支持 `OPENCODE_DISABLE_PROJECT_CONFIG` 禁用项目指令；指令集合变化时给出「当前完整集合 + 取代此前内容」的文本，最后一条被移除时给出撤销文本；技能指导是上下文来源且按 agent 权限过滤（只列名字与描述），技能正文只能通过权限检查过的 `skill` 工具取得；agent 定义带 `mode`（子代理）/`hidden`，会话默认选择有明确回退顺序（配置默认 → `build` → 第一个可选）。
- 缺点：V2 的请求组装仍缺 provider 家族基础指令、按提示词的 system 覆盖、steering 与最终步提醒（对齐表标 `missing`）；插件改写 system 的等价 hook 尚未提供（`CONTEXT.md` 明确列为待定问题）；提示词来源分散在 runner、`skill/guidance.ts`、`instruction-context.ts` 三处，理解「最终 system 长什么样」需要拼三处。

### 7.5 可迁移到 agentxx 的设计

1. **[P1] 指令集合变更的显式通知**：把「记忆文件 + 技能清单」当作 §3 的上下文来源，变化时发一条中间系统消息（含当前完整集合与取代说明；集合清空时发撤销消息）。落地位置：`middlewares/memory_file.h`、`skill.h`（现在只有 `resourceEpoch` 失效，没有变更语义）+ `prompt.cpp`。
2. **[P1] 技能/工具的按 agent 可见性过滤**：若 agentxx 支持多 agent 配置（主/子代理），把「技能列表」与「工具集合」按 agent 过滤后再进提示词与工具表；被拒绝的技能直接不出现在模型可见列表里（而不是靠模型自觉）。落地位置：`CodeAgent` 装配 + `AgentConfig` 的 per-agent 段 + `ToolcallWrapNode` 的工具物化。
3. **[P2] 技能正文的权限检查**：技能正文当前由 `agentxx_filesystem_read` 读取（受文件系统权限约束但不带「技能」语义）。可加一个专用 `agentxx_skill_read`（或在 read 工具上按技能目录做语义检查），让权限询问卡片显示「读取技能 X」而不是「读取路径 Y」。落地位置：`agent/lib/src/middlewares/skill.cpp` + 插件侧技能工具。
4. **[P2] 指令发现的边界规则**：若将来支持「向上查找」式发现，必须同时照搬两条约束：路径规范化 + 不越出项目根；并给一个显式开关。落地位置：`prompt.cpp` / `resource_applier.cpp`。

---

## 8. LLM 层：协议、provider、鉴权与缓存

### 8.1 agentxx 现状

- 三个协议适配：OpenAI Chat Completions、Anthropic Messages、OpenAI Responses（Codex）；`ModelProviderRegistry` 支持运行时按会话切换模型。
- 流式：增量 `Delta` 事件（TextToken/ThinkToken/ToolStart/ToolEnd/TurnStart/TurnEnd/NodeStart/NodeEnd/MessageUITip/InsertMessage/UpdateMessage），每个 delta 带单调 `seq` 供重放。
- 重试与限流：模型调用支持重试（`neograph::llm::rate_limited_provider`），取消在重试等待后生效。
- 无 prompt 缓存提示；无 provider 错误分类（未见「上下文溢出」专用分支，压缩靠本地 token 估算）；测试中有 provider 对拍测试（`test_openai_provider`、`test_anthropic_provider`、`test_http`）但依赖真实/模拟 HTTP。
- 本次细读 `nodes/modelcall.cpp` 得到的具体事实：限速识别是**关键字匹配**（`AhoCorasick` 预置 `429`/`rate limit`/`has been exhausted`/`insufficient`/`速率限制`/`限速` 等，命中即加长退避）；退避公式 `retry × 3s`（限速再 `+ retry × 5s`），不读 `retry-after`、无抖动；重试上限为 `llmMaxRetry` 且计数不重置；另用 grep 全仓确认**没有任何** `context_length_exceeded` / `prompt is too long` / `context window` 之类的溢出识别代码 —— 即「provider 报溢出」这条路径当前没有专门处理。

### 8.2 opencode 现状

- **分层**（`packages/llm`，105 文件 / 1.86 万行）：
  - `schema/`：`messages`（含 `ToolDefinition`、`CacheHint`）、`events`（流事件，含 provider 错误事件）、`options`（生成参数、缓存策略、provider 选项）、`errors`（类型化错误）；
  - `protocols/`：6 个线上协议适配（`anthropic-messages`、`bedrock-converse`、`gemini`、`openai-chat`、`openai-compatible-chat`、`openai-responses`）+ `utils/`（工具 schema 转换、工具流解析、缓存、生命周期）；
  - `providers/`：10 个 provider 门面（openai/anthropic/google/azure/cloudflare/github-copilot/openrouter/xai/amazon-bedrock/openai-compatible，另有 profile/options 辅助模块）；**外加** `packages/core/src/plugin/provider/` 下 32 个 provider 插件（alibaba/cerebras/cohere/deepinfra/groq/gitlab/mistral/nvidia/perplexity/sap-ai-core/snowflake-cortex/vercel/zenmux…），每个插件声明 provider 的模型列表与鉴权方式；
  - `route/`：`client`（统一 `LLMClient.stream/generate`）、`executor`（重试/超时/取消编排）、`transport/http` + `transport/websocket`（如 responses 的 WS）、`auth`（API Key/OAuth 凭据注入）、`framing`、`endpoint`。
- **prompt 缓存策略**（`cache-policy.ts`）：默认 `auto` —— 在「最后一个工具定义」「最后一个 system 片段」「最新用户消息」各放一个缓存断点；只对支持内联缓存标记的协议（anthropic-messages、bedrock-converse）生效；调用方可用 `none` 关闭或给对象精确指定（含 TTL）；协议自动忽略不支持的内联标记，注释里把成本测算（Anthropic 写入 1.25×、读取 0.1×）写明，说明「一次 5 分钟内的复用就已经回本」。
- **provider 错误分类**：`isContextOverflowFailure` / `provider-error.ts` 把「上下文溢出」识别为可恢复类别，runner 据此触发一次性压缩重试；流式过程中已产生 assistant 输出后不允许再恢复。
- **结构化输出**：`LLM.generateObject` 在**所有协议上通用** —— 内部合成一个强制工具 `generate_object`（`toolChoice: named`），把模型输出按 schema 解码；注释明确「刻意不使用 provider 原生 JSON 模式，以保证行为一致」。
- **录制回放**：`packages/http-recorder`（Effect HTTP 客户端流量的确定性录制/回放）+ `packages/core/test/fixtures/recordings/session-runner/openai-chat-streams-text.json` 这样的固定装置，使 provider 相关测试可离线、可复现。
- **重试/超时**：规格明确「不做通用 provider 超时/看门狗」，重试策略留给后续显式设计（`specs/v2/session.md` 末段）。
- 本次细读 `packages/llm/src/route/executor.ts`、`session/runner/model.ts`、`session/runner/to-llm-message.ts` 得到的具体事实：
  - **传输层重试**确实存在：可重试状态码为 `429/503/504/529` 与 `≥500`；优先采用响应头 `retry-after-ms` / `retry-after`（带上限），否则用带抖动的指数退避 `BASE_DELAY_MS × 2^attempt × [0.8, 1.2]`（有最大延迟）；超时错误被映射为独立的 `Timeout` 传输错误类别；限速响应头还会被解析成结构化限速信息。
  - **模型解析**：会话只存 `{providerID, modelID, variant}`，实际解析在 `SessionRunnerModel.resolve`：先从 catalog 取模型（会话未选模型时用 catalog 默认，或第一个受支持的模型），再取 provider 的 integration 连接解析凭据，最后按 `model.api.package` **硬编码映射**到协议（`@ai-sdk/openai` → OpenAI Responses 路由、`@ai-sdk/anthropic` → Anthropic Messages 路由、`@ai-sdk/openai-compatible` + url → OpenAI 兼容 Chat 路由），不匹配就报 `UnsupportedApiError`；`variant` 是「额外 headers + body 覆盖」，由 catalog 定义；catalog 的窗口/输出上限会注进路由的 `limits`。
  - **换模型的历史投影规则**（`to-llm-message.ts`）：只有当历史 assistant 消息的 `providerID + modelID` 与当前选中模型一致、且该消息没有 error 时，才复用 provider 原生元数据（reasoning 签名、provider item id）；不一致时 reasoning 降级为普通文本、元数据整个丢弃；`agent-switched` / `model-switched` 消息**不发给模型**（返回空数组）；`compaction` 消息降级为一条 user 消息，内容包在 `<conversation-checkpoint>` 的 `summary` + `recent-context` 两个标签里；`shell` 消息降级为「Shell command: ...」的 user 消息。

### 8.3 对比

| 维度 | agentxx | opencode | 评价 |
|---|---|---|---|
| 协议数 | 3（OpenAI Chat / Anthropic / OpenAI Responses） | 6 协议 + 10 个 provider 门面 + 32 个 provider 插件 | opencode 的覆盖面大得多（尤其 Bedrock/Gemini/Vertex/各类网关）；agentxx 覆盖主流 3 家够用 |
| 缓存 | 无显式断点 | 默认 auto 断点 + TTL + 协议能力判断 | **迁移价值高、成本低**：长会话成本与首 token 延迟都受益 |
| 错误分类 | 未见溢出/鉴权专门分支 | 溢出可恢复（触发压缩重试）、provider 错误事件结构化 | 可借鉴「溢出 → 压缩一次再试」 |
| 结构化输出 | 无（靠提示词约束 + 解析文本） | 合成强制工具，跨协议一致 | 可借鉴，用于摘要/规划/参数化输出 |
| 鉴权 | API Key（env/yaml） | API Key + OAuth（GitHub Copilot/OpenRouter 等，含刷新与账户切换）+ 凭据与集成分离 | opencode 明显更强（见 §17） |
| 可测试性 | 真实/模拟 HTTP 测试 | HTTP 录制回放 + 固定装置 | 可借鉴录制回放（见 §16） |
| 多 provider 维护成本 | 手写适配，加一家要改代码 | 插件式：新增一家是「声明模型 + 鉴权 + 少量适配」 | agentxx 的插件体系其实已经具备承载条件（§15） |

### 8.4 两者的优缺点

**agentxx**
- 优点：三个协议覆盖主流场景，且有面向真实网关的修复（`repairMessages` 合并连续 user、改写重复 `tool_call_id`、补 `[Empty]`、UTF-8 修复）；重试会通知 UI（提示里的等待秒数与实际定时器用同一变量，不会出现「提示 5 秒实际等 3 秒」）；重试计数不因部分输出而重置，且 ≥512 字符的部分输出会被保留为 assistant 消息而非丢弃；限速场景（关键字命中）会自动加长退避；取消在重试等待后仍能生效。
- 缺点：没有 prompt 缓存断点，长会话的成本与首 token 延迟都吃亏（而 `system_prompt_hash` 的 ERROR 日志说明前缀本来就不稳定）；没有 provider 错误分类（本次 grep 未发现任何上下文溢出相关判定），压缩只能靠本地估算；重试不读 `retry-after`、无抖动、不退避分类（除限速关键字外），关键字匹配依赖各网关文案；模型窗口/价格等元数据要手填 yaml；没有录制回放测试；没有结构化输出通道。

**opencode**
- 优点：协议 × provider 覆盖面大（6 协议、10 个门面、32 个 provider 插件，含 Bedrock/Gemini/Vertex/各类网关）；传输层重试按状态码分类（429/503/504/529 与 ≥500）、优先使用 `retry-after` / `retry-after-ms`、退避带抖动（`BASE_DELAY_MS * 2^attempt * [0.8, 1.2]`，有上限），超时映射为独立的 Timeout 类别；prompt 缓存默认开启且按协议能力判断（`anthropic-messages`/`bedrock-converse` 才注入内联标记）；上下文溢出可恢复（`isContextOverflowFailure` + 一次压缩重试）；`generateObject` 用合成强制工具做到跨协议一致；换模型时按规则剔除 provider 原生元数据（reasoning 退化为普通文本），避免 continuation 失败；HTTP 录制回放让协议测试可离线复现。
- 缺点：一条请求要跨 protocol → adapter → route → executor → transport 4~5 个文件才能看全；runner 层不做超时/看门狗（规格明确「刻意延后」），长悬挂只能靠传输层与上层中断；模型到协议的映射是硬编码的包名前缀判断（`@ai-sdk/openai` → Responses、`@ai-sdk/anthropic` → Messages 等），新增后端要改这里；录制回放需要维护固定装置（录制文件与协议演进要同步）。

### 8.5 可迁移到 agentxx 的设计

1. **[P0] prompt 缓存断点策略**：在请求组装处按协议注入缓存标记：
   - Anthropic：在「最后一个工具定义」「system 末段」「最新用户消息」放 `cache_control: ephemeral`（可选 TTL）；
   - OpenAI/Responses：使用其隐式前缀缓存所需的前提（保持前缀稳定 —— 与 §3 的基线/增量分离是同一件事，二者应一起做）；
   - 落地位置：`agent/lib/src/protocol/anthropic_provider.cpp`、`openai_provider.cpp` 的请求体组装 + `prompt.cpp`（保证前缀稳定）。
   预期收益（opencode 注释里的口径）：Anthropic 缓存写入 1.25×、读取 0.1×，长会话下多轮复用即可回本。
2. **[P0] provider 错误分类 + 溢出恢复路径**：把「上下文溢出」「鉴权失败」「限流」「超时」分成类别；溢出时（且本轮尚无 assistant 输出）触发一次压缩后重试，其它类别不重试或按各自策略。落地位置：`protocol/provider_common.h` + `ModelCallWrapNode` 的错误处理 + `summarization.cpp` 的入口。
3. **[P1] 结构化输出（强制工具）**：加一个 `generateObject(schema)` 便捷入口：内部合成一个强制调用的工具，把模型输出按 JSON Schema 解码返回。用途：会话标题、任务规划、摘要（可让压缩走结构化输出，避免解析自由文本）、插件参数化输出。落地位置：`agent/lib/include/agentxx/protocol/protocol_base.h` 家族。
4. **[P1] 模型能力与元数据**：模型条目补充「上下文窗口、最大输出、成本、模态（文本/图片/推理）、缓存支持」，用于：预算判断（§9）、是否展示附件按钮（agentxx 已有 capabilities）、UI 展示与统计。落地位置：`model_registry.h` + yaml 模型段。
5. **[P2] 失败重试策略显式化**：把重试次数/退避/是否可重试的判定从「统一 maxRetry」改为按错误类别与请求阶段（连接/首字节/流中/工具后）区分，并写明「不重试已产生副作用的轮次」。落地位置：`ModelCallWrapNode` + 协议层。

---

## 9. 上下文压缩与预算控制

### 9.1 agentxx 现状

`SummarizationMiddleware`（`agent/lib/src/middlewares/summarization.cpp`，1 186 行）已经是相当成熟的实现：

- **触发**：上下文接近模型 token 上限时自动压缩；也可由客户端手动触发（`WireCompactContext`）。
- **内容处理**：工具输出按配置去重与截断；压缩结果立即写回会话上下文并请求节流落盘（被杀/崩溃不丢压缩结果，重启后不会因上下文重新超限而反复压缩）。
- **互斥与复用**：同一会话压缩互斥（手动与轮内自动不并发）；压缩提示消息按挂起 id 复用（中断续跑不产生重复提示）。
- **失败与降级**：失败按轮内计数累积；连续失败 ≥ 2 次或 token 占用 ≥ 95% 上限时降级为**硬截断**（按 token 预算丢最旧消息）兜底；「上次压缩后消息增长不足 2 条」视为冷却期，跳过重复压缩以避免每轮派生无效压缩子代理。
- **超时**：手动压缩等待上限 2 分钟，超时走硬截断（避免子代理卡住占用 io 线程）。
- 本次细读 `middlewares/summarization.cpp` 得到的具体做法：
  - 压缩提示词来自 `appendSystemPrompts["summarization"]`（**为空则直接跳过压缩**），经 `fmt::format` 注入 `{omitted_note}` 与 `{max_words}`（`max_words = summaryMaxTokens / 4`）；
  - 请求体是「原始消息副本（system + 待压缩段）+ 末尾一条 user 角色的压缩指令」，指令消息标记 `AutoInserted`；
  - 载荷裁剪：当请求副本超过模型窗口的 95% 时，从最旧消息（保留首条 system）开始丢弃，并把丢弃条数写进 `{omitted_note}` 让模型知道有省略；
  - 压缩**通过子代理完成**，且刻意传入「与父会话相同的 sessionId 与相同模型、不传任何工具、禁止二次压缩」，目的是**命中 provider 的 KV/前缀缓存**；子代理输出的纯文本就是摘要；
  - 另有 `direct` 手动压缩直派路径（agent 空闲触发，不经 `NodeInterrupt` 中断循环）。

### 9.2 opencode 现状

`packages/core/src/session/compaction.ts`（225 行）+ `specs/v2/session.md` 的 Automatic Compaction 段：

- **预算判断**：在每次 provider 轮次之前估算**完整模型可见请求**（system + messages + tools），与「模型上下文窗口 − 保留余量」比较；余量 = `max(请求的输出上限, 配置 compaction.buffer)`，默认 `buffer = 20 000`。
- **保留尾部**：`keep.tokens`（默认 `8_000`）用于切出「最近的对话片段」，压缩产物 = 「结构化摘要」+「按 token 限定的近期序列化上下文」两部分。
- **结构化滚动摘要**：固定的 Markdown 模板（Objective / Important Details / Work State{Completed, Active, Blocked} / Next Move / Relevant Files），规则包括「保留精确路径、符号、命令、错误串、URL」「不要提及压缩过程」；多次压缩时把**上一份摘要**与新对话合并，规则明确「冲突以对话为准，丢弃已完成项，保留仍需的约束与工作流」。
- **事件化**：`session.next.compaction.started.1`（durable）标记尝试，增量进度是实时流（不落库），`session.next.compaction.ended.1` 记录最终摘要与近期上下文；**只有 ended 才投影为模型可见的压缩消息** —— 中途失败/被杀时上一版历史边界继续生效。
- **溢出恢复**：provider 明确返回「上下文溢出」且此时尚无 assistant 输出或工具执行时，触发一次溢出压缩，然后用**剩下的一次尝试**重建同一轮；第二次溢出或压缩不可用即按普通终局失败处理（不循环、不重放副作用）。
- **压缩与上下文纪元联动**：压缩完成后的下一次 provider 尝试直接渲染新的基线上下文（新纪元），此前的中间系统消息保留为审计历史但退出模型可见历史。
- **工具输出裁剪**：确定性「旧工具结果裁剪」明确列为后续工作（**未实现**），当前只靠压缩与统一限幅。
- 本次细读 `session/compaction.ts` 得到的默认值与做法：`buffer = 20 000`、`keep.tokens = 8 000`、单条工具输出序列化上限 2 000 字符、摘要输出上限 4 096 token；预算判断是「system + messages + tools 的估算 token ≤ 窗口 − max(输出上限, buffer)」；压缩请求把上下文**序列化成扁平文本**（`[User]: …` / `[Assistant]: …` / `[Assistant tool call]: …(输入)` / `[Tool result]: …`），拼成一个 user 消息发给模型（不复用原消息前缀，也不传工具）；摘要提示词是固定模板 + 合并规则（首次压缩给模板，后续把 `<prior-summary>` 与新对话一起给，写明「冲突以对话为准」）。

### 9.3 对比

| 维度 | agentxx | opencode | 评价 |
|---|---|---|---|
| 触发判断 | 本地 token 估算接近上限 | 本地估算（完整请求，含工具定义）+ 溢出错误兜底 | opencode 把 `tools` 计入预算、把 buffer 与输出上限取大者，口径更保守清晰 |
| 压缩产物 | 摘要（由压缩子代理生成）+ 工具输出裁剪 | 结构化摘要 + 按 token 保留的近期上下文 | opencode 的**保留近期原文**能显著降低「压缩后模型丢失最近细节」的概率 |
| 摘要可续性 | 单次生成，失败降级硬截断 | 滚动合并：上一份摘要 + 新对话 → 新摘要，冲突规则明确 | 可借鉴：多轮压缩后仍保持一份自洽的「工作状态」 |
| 失败处理 | 计数 + 冷却 + 硬截断兜底 + 手动压缩超时 | 事件化：started/ended，只有 ended 生效 | 两者互补：agentxx 的降级策略更完善，opencode 的「完成边界」更干净 |
| 溢出恢复 | 无（靠本地估算提前压缩） | provider 报溢出时压缩一次再试 | 可借鉴（真实溢出比估算更权威） |
| 压缩与 UI | 压缩提示消息按挂起 id 复用（不重复） | started/ended 事件 + 模型可见压缩消息 | 两者都在意 UI，但 opencode 的事件更利于客户端重放 |
| 与上下文基线关系 | 压缩写回上下文（就地替换） | 压缩开启新纪元（重渲染基线） | 与 §3 的基线/增量分离必须一起做 |

### 9.4 两者的优缺点

**agentxx**
- 优点：失败降级链完整（失败计数 → 连续 2 次或占用 ≥95% 即转硬截断兜底），不会卡在压缩失败上；有冷却期（增长不足 2 条跳过）避免每轮派生无效压缩子代理；压缩结果立即写回并请求节流落盘（崩溃不丢、重启不反复压缩）；同会话压缩互斥、提示消息按挂起 id 复用（中断续跑不重复提示）；手动压缩有 2 分钟上限；**压缩子代理复用父会话同一 session/model 的请求前缀、不传工具、禁止二次压缩**（能吃到 provider KV 缓存，见 `summarization.cpp` 注释）；载荷超限时从最旧消息开始裁剪并在指令里写明省略条数（`{omitted_note}`）。
- 缺点：摘要模板是一段字符串（取自 `appendSystemPrompts["summarization"]`，为空直接跳过压缩），没有结构化字段，也没有「与上一份摘要合并」的规则，多次压缩后信息容易漂移；没有「保留尾部原文窗口」，压缩后只剩摘要 + 未压缩部分；没有 started/ended 完成边界；预算只看本地估算，provider 报溢出时没有恢复路径。

**opencode**
- 优点：结构化摘要模板（Objective / Important Details / Work State{Completed, Active, Blocked} / Next Move / Relevant Files）并把「保留精确路径、符号、命令、错误串」写进规则；多次压缩把上一份摘要与新对话合并，冲突以新对话为准（规则逐条写明）；按 token 预算保留最近上下文（`keep.tokens`，默认 8 000）与摘要一起进入检查点；`started`/`ended` 完成边界（只有 `ended` 投影为模型可见消息，中途失败不影响上一次历史边界）；provider 报溢出时可触发一次压缩重试（且不会在已有 assistant 输出后重放副作用）；压缩完成后重开上下文纪元。
- 缺点：压缩请求把历史序列化成扁平文本（`[User]: …` / `[Assistant tool call]: …`）而不是复用原消息前缀，等于放弃这段前缀缓存（这点 agentxx 的做法更省 token）；序列化时工具输出被截断到 2 000 字符，关键细节可能丢；`buffer = 20 000` / `keep.tokens = 8 000` 是代码常量而非配置项；确定性旧工具结果裁剪仍未实现（规格列为后续）。

### 9.5 可迁移到 agentxx 的设计

1. **[P0] 结构化摘要模板 + 滚动合并**
   - 把压缩提示词改为固定结构（目标 / 关键细节 / 工作状态（已完成、进行中、受阻）/ 下一步 / 相关文件），并明确「保留精确路径、命令、错误串」；
   - 多次压缩时把上一份摘要与新对话一起给出，规则写明冲突以新对话为准；
   - 落地位置：`agent/lib/src/middlewares/summarization.cpp`（提示词与结果落库处）。
   - 收益：崩溃/中断后模型仍能接手，减少「压缩后反复试错」。
2. **[P0] 压缩后保留尾部原文窗口**：压缩产物 = 结构化摘要 + 按 token 预算（如 8 000）保留的最近消息原文；预算作为配置项（`compaction.keep.tokens`）。落地位置：`summarization.cpp` + `AgentConfig`。
3. **[P0] provider 溢出触发的一次性压缩**：捕获协议层的「上下文溢出」错误（§8 的错误分类），在本轮尚无 assistant 输出时压缩一次并重试，且只允许一次；第二次溢出按普通失败。落地位置：`ModelCallWrapNode` 错误处理 + `summarization.cpp`。
4. **[P1] 压紧预算把工具定义算进去**：估算口径与 opencode 对齐（system + messages + tool definitions），buffer 与输出上限取大者；同时把口径写在配置注释里，避免「估算通过但请求被拒」。落地位置：`summarization.cpp` 的估算段。
5. **[P1] 压缩的完成边界事件化**：沿用 §5 的事件表，把「压缩开始/结束（含结果摘要 id）」写成 durable 事件，只有结束事件才参与模型可见历史与 UI 展示；客户端重放时可正确还原「上次压缩是否完成」。落地位置：`summarization.cpp` + `session_store.cpp`。
6. **[P2] 确定性旧工具结果裁剪**：对「很久之前且很大的工具结果」做按需裁剪（先于整体压缩发生），减少压缩频率。落地位置：`toolcall.cpp` 的写回段。

---

## 10. 权限与安全边界

### 10.1 agentxx 现状

`PermissionMiddlewareHandle`（`agent/lib/src/middlewares/permission.h/.cpp`）是宿主统一判定点：

- **规则表**：`XXRouter<PermissionOperator, 2>` 按最长前缀匹配，作用域分「读 / 写」两类（`FilesystemPermissionREAD/WRITE`）；`PermissionOperator = ALLOW | DENY | INTERRUPT`。
- **默认规则**：`noRuleOperator` 由配置 `permission.mode` 决定（`ask`/`all_ask` → INTERRUPT，`pass` → ALLOW，`deny` → DENY）。
- **声明式目标**：插件注册工具后声明 `ToolPermissionSpec{scope, targetKind(None/Path/Text), targetArgs, category}`；目标值按参数实际 JSON 类型处理（字符串为单目标、数组逐项）；未声明权限的工具**直接放行**（不参与判定）。
- **三态路径查询**：`decideTarget`/`decidePaths` 给出 `Deny | Allow | Ask`（只读、不弹窗），供 glob/grep/list 类工具先过滤实际路径再决定是否询问 —— 防止 `**` 模式绕过子目录拒绝规则。
- **隔离与完全授权**：`SessionFsIsolation{allowPath, denyWritePath}`（worktree 绑定后写主检出直接拒绝，隔离优先于白名单）；`isFullAuthorized` 可全局放开（客户端 `WireSetFullAuth` 切换，状态变化广播所有端点）。
- **配置拒绝路径**：`permissionDenyPaths`/黑名单始终拒绝且不询问（即使完全授权）。
- **批准记忆**：询问应答可携带 `remember`，为「该目标及其子路径」注册允许/拒绝规则；多端一致（状态经会话服务端点广播）。

### 10.2 opencode 现状

`packages/core/src/permission.ts`：

- **数据模型**：规则 = `{ action, resource, effect: "allow" | "deny" | "ask" }`；`evaluate(action, resource, ...rulesets)` 取**最后一条匹配**，都没有时默认 `ask`（默认拒绝倾向明确）。`Wildcard.match` 支持通配。
- **来源分层**：
  - Agent 配置的规则（`agent.permissions`，缺省时用 `[{action:"*", resource:"*", effect:"deny"}]` 作为「没有 agent 权限就拒绝」的保守默认 —— 代码注释明确「不允许出现『模型按 build agent 跑，但权限按空策略评估』」）；
  - 项目级 saved approvals（`PermissionSaved.list({ projectID })`，按项目存）；
  - 三者合并后判定：任一资源 deny → deny；否则有 ask → ask；否则 allow。
- **两种入口**：`ask()`（只判定并返回结果，供需要展示的调用方）与 `assert()`（deny → `BlockedError`；ask → 创建待决定请求、发 `Asked` 事件、等待应答；应答拒绝 → `DeclinedError`，带反馈文本时 → `CorrectedError`）。
- **应答语义**（`reply`）：
  - `reject`：拒绝当前请求，并**把同一会话其他待决定请求一并拒绝**（避免悬挂）；
  - `always`：写入 saved approval（按项目+action+resources），并**回溯批准**本会话内其它「在新规则下已满足」的待决定请求；
  - 服务关闭时把所有待决定请求全部置为拒绝（finalizer）。
- **工具自行发起**：`Tool.make` 的实现里显式 `permission.assert({ sessionID, agent, source: { type:"tool", messageID, callID }, action, resources, save })`；registry 不注入权限帮助函数（规则与工具自身语义贴近）。
- **公开接口**：`permission.request.list` / `permission.saved.list` / `permission.saved.remove`（HTTP 层，可给 UI 做「已批准列表与撤销」）。
- **沙箱取向**：V2 明确「bash 不做沙箱，使用宿主用户的文件系统/进程/网络权限」；只有结构化 `workdir` 会走 `external_directory` 授权；对绝对路径参数的扫描**只是提示性警告**，不构成边界（文档明确写清）。这一点与 agentxx 的「权限层硬边界」取向不同。

### 10.3 对比

| 维度 | agentxx | opencode | 评价 |
|---|---|---|---|
| 判定中心 | 宿主统一（插件只声明目标） | 工具自行 assert（宿主只提供判定与询问） | agentxx 更集中、更易于全局审计；opencode 更贴工具语义（如 bash 的 workdir 检查） |
| 规则形态 | 读/写两类作用域 + 最长前缀 + 通配 | `action × resource × effect` 通用三元组 + 最后匹配 | opencode 更通用（可表达非文件系统动作）；agentxx 的路径语义更强（尾斜杠、规范化） |
| 默认策略 | 由 `permission.mode` 决定（可 `ask`/`pass`/`deny`） | 无匹配默认 `ask`；无 agent 权限默认全 deny | 两者都保守；agentxx 可配置，opencode 更「默认询问」 |
| 批准记忆 | `remember` 注册到内存规则表（多端一致，落库见 share store/会话库） | 按项目持久化 + 有查询/撤销接口 | **可借鉴**：明确的项目级存储 + 撤销入口 |
| 连带处理 | 无（每个询问独立） | 拒绝连带拒绝同会话待决；always 回溯批准已满足的待决 | **可借鉴**：避免同时弹多个询问、或批准后旧询问仍悬挂 |
| 三态路径复核 | 有（glob/grep 先过滤再询问） | 无（工具自行对每个资源 assert） | **agentxx 更强**：模式类工具不会绕过子目录拒绝 |
| 工作区隔离 | 权限层硬拒绝（隔离优先白名单） | 交给容器/沙箱文档；V2 bash 不做沙箱 | **agentxx 更强**（真实编码场景的关键保障） |
| 完全授权 | 有（含状态广播给所有端点） | 无对应概念（靠 saved approvals 与 agent 规则） | agentxx 更贴合交互式使用 |
| 询问可见性 | 中断 UI 表单 | 待决定请求可列举（HTTP）、事件发布 | opencode 的「请求本身可查询」对多端与重连友好 |

### 10.4 两者的优缺点

**agentxx**
- 优点：判定集中在宿主（插件只声明「哪些参数是受约束目标」，判定口径与内置规则完全一致）；读/写两个作用域 + 最长前缀匹配 + 通配；提供**只读三态判定**（`decideTarget`/`decidePaths`），模式/前缀类工具可以逐路径复核后再决定是否询问，`**` 无法绕过子目录拒绝规则；worktree 隔离优先于白名单；配置显式拒绝的路径即使完全授权也保持拒绝；完全授权状态变化会广播给所有接入端点（多端一致）；询问卡片带分类文本（`category`）。
- 缺点：批准记忆只在内存规则表里（重启即失效），也没有「已记住的权限」列表与撤销入口；每个询问独立处理，一次拒绝不会处理同会话其他待决询问；待决定请求无法查询，重连后询问界面无法恢复；规则维度固定为读/写路径，非文件系统动作（如「允许终止进程」「允许访问某 MCP 工具」）难以表达；工具未声明权限即放行，声明漏写就是漏判。

**opencode**
- 优点：规则是通用三元组（`action × resource × effect`）+ 取最后一条匹配 + 无匹配默认 `ask`；来源分层清楚（agent 规则 → 项目级 saved approvals → 合并判定：任一 deny 即 deny，否则有 ask 即 ask）；拒绝一个请求会同时拒绝同会话其他待决请求，选择「始终允许」会回溯批准已满足的待决请求（`permission.ts` 逐段实现）；待决请求与已批准列表都有公开查询/撤销接口（`permission.request.list` / `permission.saved.list` / `permission.saved.remove`）；工具自行发起 `assert`，让权限语义贴合工具本身（`bash` 的 `workdir` 走 `external_directory`）。
- 缺点：权限逻辑分散在每个工具实现里，新增工具容易漏写；没有逐路径批量复核（模式类工具只能整体断言一个 action）；没有工作区写边界（`bash` 不做沙箱、绝对路径扫描只是提示，文档明说）；没有「完全授权」这类交互开关（长会话里反复询问只能逐条记住）。

### 10.5 可迁移到 agentxx 的设计

1. **[P0] 批准记忆的持久化与撤销入口**
   - 把「记住的选择」从内存规则表提升为持久化条目（按会话所属项目/工作区 + 作用域 + 目标 + 允许/拒绝 + 时间）；
   - 提供撤销接口（列出 + 删除），并在 TUI 里给一个「已记住的权限」列表（现在用户无法查看/撤销已记住的选择）；
   - 落地位置：`middlewares/permission.cpp`（remember 分支）+ `util/settings_db.h` 或会话库；客户端新增设置项（`agent/client/.../tui/`）。
2. **[P0] 拒绝与批准的连带处理**
   - 同一会话（或同一轮）内多个待决定请求：用户拒绝其一 → 其余一并拒绝；
   - 用户选择「始终允许」→ 重新评估其它待决定请求，已满足的直接批准并通知；
   - 落地位置：`PermissionMiddlewareHandle::requestPermission` 的应答处理（需要给「待决定请求」建一个按会话的登记表，现在只有单次询问的等待）。
3. **[P1] 待决定请求可查询**：把待决定请求作为会话状态的一部分暴露（`WireGetPermissionState` 可扩展返回待决列表），UI 可在重连后恢复询问界面。落地位置：`wire_protocol.h` + `session_server_agent_io.cpp`。
4. **[P1] 权限动作的通用化（可选）**：agentxx 的规则表按「读/写路径」建模；若将来需要非文件系统动作（如「允许访问某 MCP 工具」「允许终止进程」），可引入通用的 `action × target` 维度（保留现有路径语义作为其中一类）。落地位置：`permission.h` 的 `XXRouter` 扩容（现在的固定 2 个作用域可扩展为按 action 命名的作用域表）。
5. **[P2] 边界文档**：opencode 在 `SECURITY.md` 与规格里明确写「bash 不是沙箱、绝对路径扫描只是提示」，agentxx 也应在 `docs/zh-cn/design` 里给出同样的显式声明（哪些是硬边界、哪些只是提示），避免使用者误判。落地位置：文档（`docs/zh-cn/design/index.md` 权限段）。

---

## 11. 子代理、后台任务与并行

### 11.1 agentxx 现状

- **中断即委派**：父 agent 的 `agentxx_subagent` 工具首次调用时把中断参数写入 `graphData` 并抛 `NodeInterrupt`，图引擎 checkpoint 暂停父图；`AgentRunner` 统一中断循环解析中断参数，经会话/全局总线派发。
- **`AgentHost::spawnBatch`**：每个任务派生一个**独立 agent**（独立 `AgentContext`/engine/`SessionStore`/中间件栈），与主 agent 同构（`AgentNode`）；子代理默认轻量：不建 MCP 连接、不加载插件/RAG/CodeGraph、不注入父级技能与记忆、**不持久化**、默认用配置的 subagent 模型。
- **预算与保护**：宿主强制嵌套深度（`maxDepth`）与并发上限（`maxConcurrentSubagents`）；取消令牌透传，父取消级联中止子代理；进度经 `hostBus.agent.progress` 发布，结束经 `agent.done` 通知；运行结束立即回收 `AgentNode`（会话与中间件状态随 `AgentContext` 析构释放，无按 thread 累积泄漏）。
- **HIL 冒泡**：子代理会话继承父会话的 io 与总线，权限/中断询问直达用户；嵌套时从父级（上一级）`SessionStore` 查找；子代理由同一 `AgentRunner` 驱动中断循环，但没有 checkpoint 持久化与中断头消息。
- **结果回写**：经 `interruptResult` 通道写回 `graphData`，父图 `resume_async` 继续，工具侧按 `(tool_call_id + "_") + (result_id | 任务序号)` 提取结果（单任务纯文本、多任务 JSON 数组）。

### 11.2 opencode 现状

- **子代理就是会话**：`session.parentID` 指向父会话，子会话是完整会话（有历史、可重放、可查询），runner 请求上带 `x-parent-session-id`（用于服务端归因）；`sessions.active()` **只统计前台 drain**，后台子代理与任务不把父会话标成运行中。
- **后台任务服务**（`background-job.ts`）：进程级服务管理 `{ id, type, title, status: running|completed|error|cancelled, started_at, completed_at, output, error, metadata }`；支持在任务运行中追加输出（tail）、结束后把结果「提升（promote）」回会话（`onPromote`），并区分「已完成但尚未提升」的状态；作用域关闭（`Scope`）会取消任务。
- **并行**：不同类型的功能（catalog 变换、插件装载、上下文来源观测、工具执行）都显式声明并发度（`Effect.all(..., { concurrency: "unbounded" })` 或工具调用的 FiberSet），但**会话执行本身**按会话串行（`SessionRunCoordinator`），跨会话并行。
- **`codemode`**：另一个方向的「并行/编排」——让模型写一段**受限 JavaScript**，在一次工具调用里编排多个已有工具（顺序、分支、循环、`Promise.all`），只有被提供的工具能被调用；有执行限额（超时、工具调用次数、输出字节）、诊断是数据、宿主中断仍是中断。它不新增权限，只是把已有工具的组合能力交给模型。
- 本次细读 `agent.ts` 与 `background-job.ts` 补充：agent 记录里有 `mode`（`subagent` 不可作为会话默认，选择时被 `selectable()` 过滤）与 `hidden`；默认 agent 的解析顺序是「配置默认 → `build` → 第一个可选」；`agent-switched` / `model-switched` 会作为消息进入会话历史（但不下发给模型，见 §8）。后台任务的 `Active` 结构里带 `promoted` / `onPromote` 字段，即「任务完成」与「结果提升进会话」是两步，且可以「已完成但尚未提升」。

### 11.3 对比

| 维度 | agentxx | opencode | 评价 |
|---|---|---|---|
| 子代理身份 | 独立 agent 实例 + 内存会话（不持久化） | 真实会话（parentID），有历史、可重放、可查询 | opencode 便于审计与「回看子代理做了什么」；agentxx 更省资源 |
| 委派触发 | 中断即委派（图暂停 → 宿主派生 → 结果回灌 resume） | 常规工具调用（子代理会话由工具实现启动） | agentxx 的「暂停父图」使父会话状态稳定，代价是复杂度高 |
| 并发控制 | 深度 + 并发预算 + 取消级联（显式、宿主强制） | 靠 Scope/fiber 组合，没有显式的深度/并发预算概念 | **agentxx 更强**，尤其防「子代理递归爆炸」 |
| 资源回收 | 结束即回收 `AgentContext`（内存不留累积） | fiber/Scope 结束即释放；会话数据保留在库里 | 两者都干净；agentxx 需要额外注意「不持久化」带来的审计缺口 |
| 后台任务 | 通过子代理与事件总线表达，无统一的后台任务表 | 有 `background-job` 服务（状态/输出/提升/取消） | 可借鉴：统一的后台任务状态对 UI 与重连友好 |
| 任务列表 | planning 插件的计划（自定义状态机 + 图渲染） | `todowrite` 工具 + `session_todo` 表 | 两者都有；opencode 的更轻、agentxx 的带图渲染 |
| 编排粒度 | 一次工具调用 = 一个子代理任务（批量可多任务） | 一次工具调用 = 可编排任意多个工具（codemode） | codemode 适合「大量小调用」场景，但要额外沙箱与限额设计 |

### 11.4 两者的优缺点

**agentxx**
- 优点：宿主强制嵌套深度（`maxDepth`）与并发预算（`maxConcurrentSubagents`），天然防「子代理递归爆炸」；取消令牌从父级联到子代理；子代理是轻量配置（不建 MCP、不加载插件/RAG/CodeGraph、不注入父技能与记忆、默认用小模型），资源开销可控；结束即回收 `AgentNode`（会话与中间件状态随 `AgentContext` 析构释放，无按会话累积泄漏）；HIL 冒泡（子代理继承父会话 io 与总线，权限/中断询问直达用户）；中断结果 key 的写入/读取共用同一函数，单任务文本、多任务 JSON 的约定统一。
- 缺点：子代理不持久化，事后无法回看「子代理做了什么」；没有统一的后台任务状态表（进度靠 `agent.progress` 事件，客户端重连后拿不到历史）；父会话「运行中」与子代理运行的关系没有明确语义；结果只能经 `interruptResult` 回灌父图，没有「先跑完、稍后由模型决定怎么用」的形态；子代理默认模型/工具集在配置里表达，缺少 agent 级 `mode` 之类的显式属性。

**opencode**
- 优点：子代理就是真会话（`session.parentID`），有历史、可查询、可重放，便于审计；`background-job` 服务统一管理后台任务的 id/type/status/output/error，并支持「完成后提升（promote）」与作用域取消；`sessions.active()` 只统计前台 drain，后台子代理不会把父会话标成运行中（语义明确）；agent 定义带 `mode`（`subagent` 不可作为会话默认）与 `hidden`；不同类型工作的并发度显式声明（`concurrency: "unbounded"` 或 FiberSet），会话执行仍按会话串行。
- 缺点：没有嵌套深度/并发预算这类显式约束（靠 fiber 与 Scope 组合）；子代理会话与父会话同一数据库，长时间使用会话数量增长（需清理策略）；没有「中断即委派」的父图暂停语义，父会话状态与子代理并发时的一致性靠会话串行化保证；`background-job` 的重启恢复仍需自行定义（bash 工具的 TODO 里明确「持久化后台任务状态与恢复语义先定再暴露」）。

### 11.5 可迁移到 agentxx 的设计

1. **[P1] 后台任务状态统一**：把「长时间运行、结果稍后提升」的工作（子代理、RAG 索引、codegraph 构建、插件后台任务）统一登记为后台任务条目（id/type/title/status/output/error/时间），可查询、可取消；UI 用同一份数据渲染进度。落地位置：`agent/lib/src/agent/agent_host.cpp` 已有 `agent.progress`/`agent.done` 事件，可在此之上建表（内存 + 可选落库）。
2. **[P1] 子代理会话可选持久化**：给 `AgentConfig` 增加「子代理是否持久化」开关；开启时子代理会话写入会话库并在 UI 里可展开查看（复用现有会话库与会话切换机制）。落地位置：`agent_host.cpp` spawn 段 + `session_store.cpp`。
3. **[P1] 父会话活动状态的明确语义**：UI 上「会话运行中」只统计前台轮次；子代理运行不把父会话标成运行中（agentxx 现在可以把子代理进度单独展示，避免用户误以为父会话还在输出）。落地位置：`SessionServerAgentIO` 状态推送 + TUI 侧边栏。
4. **[P2] 子代理结果的「提升」语义**：现在结果通过 `interruptResult` 回灌；若将来支持「先跑完、稍后由模型决定如何使用」，可参考 opencode 的 promote（完成 → 提升 → 通知），避免结果在总线里悬空。落地位置：`agent_host.cpp` + `interrupt_bus`。
5. **[P2] 受限编排（大工程，可选）**：如果出现「模型发出几十个小工具调用、每次往返都很贵」的实际问题，可评估一个受限的编排入口（agentxx 已有 `agentxx_javascript_engine` 插件与 QuickJS，具备实现基础），但必须同时设计：可用工具白名单、调用次数上限、超时、输出上限、权限沿用（编排不新增权限）。落地位置：新插件 + `agentxx.agent.tools` 表。

---

## 12. 大输出、附件、快照回滚与结构化询问

### 12.1 agentxx 现状

- **大输出**：工具输出超限时按 `toolcallSummaryLimitOutputLength` 压缩（摘要中间件），内容**丢弃**；上下文压缩也会裁剪历史工具输出。
- **附件**：跨设备附件选择（远程模式下本地/服务端两页签）；服务端文件只传 `pathOrUrl`，由服务端自行读取编码（卸载到线程池）；中文路径统一经 UTF-8 转换。
- **文件回退**：无「回滚到某条消息对应的文件状态」能力；worktree 提供的是隔离与清理，不是逐消息回退。
- **结构化询问**：中断 UI（`interrupt_ui`）支持表单/选择等交互（权限询问、插件表单），但**没有**给模型的「向用户提问」工具；模型只能靠自然语言提问（用户不一定回答，回答也进不了结构化结果）。
- **终端/进程交互**：`agentxx_execute_*` 工具执行命令并可取消；无 PTY 会话（无交互式终端）。

### 12.2 opencode 现状

- **Managed tool output**（`tool-output-store.ts`）：超限文本保留头尾预览，完整文本写入共享 `tool-output` 目录（全局唯一名、7 天保留），预览里给出可读、可被普通工具读取的绝对路径；结构化结果不受影响；保留失败时结算失败（不发布有损成功）。
- **快照与回退**（`snapshot.ts` + `session/revert.ts`）：
  - `capture()` 把当前工作区状态抓成内容寻址的树（基于 git 对象存储的实现，尽力而为，不支持时返回 `undefined`）；
  - `files()/diff()/preview()` 比较两棵树、生成结构化逐文件 diff、预览选择性恢复的结果；
  - 会话回退三态：`stage`（以某条消息为边界，计算该消息之后被改动的文件并从快照恢复；把原始快照与 diff 记入会话 `revert` 字段并发 `RevertEvent.Staged`）、`clear`（恢复到原始状态并清空）、`commit`（接受当前状态，仅记录事件）；
  - 每条 assistant 消息保存 `snapshot: { start, files }`，因此「这条消息改了哪些文件」是持久事实。
- **`question` 工具**：模型可以结构化提问（多问题、多选、可自定义答案，默认附加「自己输入答案」选项），答案是标签数组，返回给模型的文本形如 `"问题"="答案"`；实现上先过权限 `assert({action:"question"})`（可被 agent 规则拒绝）。
- **PTY 服务**：`pty.ts` + `packages/protocol/src/groups/pty.ts` 提供终端会话（含 ticket 鉴权、环境叠加：调用方值 → 宿主叠加 → 核心强制项如 `TERM`/`OPENCODE_TERMINAL`）。
- **`todowrite` 工具**：把任务清单作为会话状态（`session_todo` 表）持久化，模型每次提交完整列表。

### 12.3 对比

| 维度 | agentxx | opencode | 评价 |
|---|---|---|---|
| 大输出保真 | 压缩丢弃 | 完整落盘 + 头尾预览 + 路径可回取 | **迁移价值高**（长 grep/命令输出很常见） |
| 文件回退 | 无 | 快照 + stage/clear/commit + 逐消息改动列表 | **迁移价值高**：交互式编码里「回到这条消息之前」是高频需求 |
| 附件 | 跨设备选择（本地/服务端）、路径优先传参 | data URI / 文件引用 + 媒体类型判定 + 目录附件 | agentxx 的跨设备设计更贴合远程 agent；opencode 的附件表达更完整（目录/媒体） |
| 结构化询问 | 只有中断 UI（宿主发起） | `question` 工具（模型发起）+ 权限门 | **迁移价值高、成本低**（agentxx 的表单能力已具备） |
| 任务清单 | planning 插件（图渲染） | `todowrite` + 表 | 各有优势：agentxx 可视化更强 |
| 终端交互 | 无 PTY | PTY 服务（多端共享、环境叠加） | 视需求；若要做「交互式命令」需要它 |
| 文件改动可见性 | worktree + 工具输出（模型自述） | 每条 assistant 消息带改动文件清单 | 可借鉴：改动清单是持久事实，UI 与回退都依赖它 |

### 12.4 两者的优缺点

**agentxx**
- 优点：附件支持跨设备选择（本地 / 服务端两页签），服务端文件只传路径由服务端自行读取编码（不传 base64，省带宽）；路径全程 UTF-8（含 Windows 代码页转换）；中断 UI 的表单与控件能力已具备（控件状态、校验、取值、提交回传一处实现），加一个「模型发起的提问工具」不需要新建 UI 体系。
- 缺点：工具输出超限时内容被压缩丢弃，没有「完整输出另存 + 可回取」；没有文件快照与回退能力（只有 worktree 隔离）；没有模型发起的结构化询问工具（只能自然语言追问，用户回答不进入结构化结果）；没有 PTY 交互式终端。

**opencode**
- 优点：托管输出（超限保留头尾预览 + 完整文本落 `tool-output` 目录 + 预览里给绝对路径 + 7 天保留 + 「保留失败则结算失败」的不变式）；快照/回退三态（`stage`/`clear`/`commit`）以「消息边界 + 该边界之后的改动文件清单」为持久事实（每条 assistant 消息带 `snapshot.start/end` 与 `files`）；`question` 工具让模型结构化提问（多问题、多选、可自定义答案，先过权限 `assert`）；`todowrite` 把任务清单持久化到会话表；PTY 服务支持多端共享与环境叠加（调用方值 → 宿主叠加 → 核心强制项）与 ticket 授权。
- 缺点：快照依赖 git 且「尽力而为」（不支持时返回 `undefined`，功能静默降级）；回退语义较重（`revert.commit` 会删除边界之后的消息行与未提升的收件箱行，用户需要理解）；托管输出的路径出现在公开 API 上（项目自己的规格也标注「存储封装尚不完整」）；`bash` 输出仍受 1 MiB 内存捕获上限约束（TODO 明确「改为流式写入托管存储、内存只留预览」）。

### 12.5 可迁移到 agentxx 的设计

1. **[P0] 大输出落盘保真**：见 §6.5 第 3 条（`tool-output` 目录 + 头尾预览 + 路径 + 过期清理），与 §9 的压缩配合。
2. **[P1] 模型发起的结构化提问工具**：新增 `agentxx_ask_user` 工具（问题列表 + 选项 + 是否多选 + 是否允许自定义答案），经中断 UI 表单呈现；答案为结构化结果（同时给模型一段文本投影）。权限上按工具权限声明走（询问动作本身也可被规则拒绝）。落地位置：`middlewares/interrupt_ui.cpp`（已有表单能力）+ 新工具 + `agentxx.ui.item` 的表单控件。
3. **[P1] 每条消息记录「本次改动的文件清单」**：在工具执行（写类工具）与会话历史之间建立关联：assistant 消息记录「本次轮次改动的文件 + 起始快照 id（若有）」。落地位置：`session_store.cpp` 的 `view_message` 行扩展 + `middlewares/permission.cpp` 的写路径记录（可在写工具通过权限检查时登记）。
4. **[P2] 快照与回退（依赖 git）**：若引入回退能力，最小可用形态是「基于 git 的临时树快照」：
   - 每次轮次开始/结束抓一次快照（内容寻址，缺失时静默降级）；
   - 用户选择「回退到这条消息」→ 计算该消息之后被改动的文件 → 选择性恢复 → 记录事件；
   - 与 worktree 模式天然兼容（快照在 worktree 内抓取）。
   落地位置：新工具/宿主能力 + TUI 交互（消息上的回退入口）；实现代价较高，与「§18 P2」同级。
5. **[P2] PTY 会话**：若需要「交互式命令」或「用户直接在 agent 的终端里操作」，可参考 opencode 的 PTY 服务形态（会话级终端 + 环境叠加 + ticket 授权）；否则不必实现。

---

## 13. 客户端 UI 与渲染分层

### 13.1 agentxx 现状

- **唯一渲染实现**：所有界面内容（面板、Info 侧栏、overlay、工具装饰、中断描述）都走同一套**声明式组件树**（`agentxx.ui.item`，数据层在 `agent/lib/{include/agentxx/ui,src/ui}`，渲染唯一实现在 `agent/client/include/agentxx-client/io/tui/ui_components.h/.cpp`）。
- 组件种类：文本/差异/状态图、横排（row）、分组框（box）、折叠（collapse）、表格（table）、树（tree）、键值（kv）、趋势图（sparkline）、计量条（meter）、控件（control）、提交行（submit）、自定义（custom）；`canvas` 仅解析与降级。
- 交互：元素内子区域命中（`UiHitRegion`）+ 滚动容器 `hitTestItem`；表单状态由宿主维护（`UiFormState`），提交经动作通道回传 `__submit` + `{"values":{...}}` / `__cancel`。
- 客户端能力名（capability 协商）：`agentxx.client.components` / `agentxx.client.form` / `agentxx.client.layout`，老宿主缺失即降级；单条 UI 描述有 1 MiB 上限，注册表条目带内容 `version`。
- 客户端插件：`ClientPluginManager`（4 009 行）驱动 10 张 client 侧接口表（`agentxx.client.{ui,events,session,wire,self,json,log,timer,keybind,panel}`），插件可注册面板、定时器、快捷键、渲染器。
- TUI 细节：markdown 代码块按显示宽度折行（`wrap_line_by_width`）、懒滚动（`LazyScrollable` + 前插锚定）、文件选择弹窗（本地/服务端两页签）、设置弹窗（分组 + 超长可滚动）、快捷键列表弹窗。

### 13.2 opencode 现状

- **三种前端 + 共享组件**：
  - `packages/tui`（204 文件 / 2.95 万行）：OpenTUI + Solid 的终端应用；按 `specs/tui-package.md` 从 `packages/opencode` 抽出为独立包，**只依赖 SDK**，不得依赖 `packages/opencode`/`cli`/`core`；拥有渲染生命周期、组件/路由/对话框/主题/键位、SDK 同步与事件消费、工具调用展示、TUI 插件契约、TUI 本地持久化（提示历史、stash、frecency、选中模型/主题）。
  - `packages/session-ui`（96 文件 / 1.97 万行）：Web 组件层，供 `packages/app`（Web/桌面）复用；重点是消息渲染（`message-part.tsx` 2 459 行）、markdown（`markdown.tsx` 653 行 + **worker 化的解析队列/传输** `markdown-worker*.ts`）、文件浏览（`file.tsx` 1 064）、会话评审（`session-review`）、工具错误卡片、`prompt-input` v2（带状态机 `machine.ts` + store）。
  - `packages/ui`（247 文件 / 3.15 万行）：跨端基础组件；`packages/app` 用 Solid + Tailwind + Kobalte 组装，含 Playwright e2e、性能基准与 Storybook。
- **插件渲染槽**：`tui/src/plugin/{api,adapters,runtime,slots,command-shim}.tsx` + `feature-plugins/builtins.ts`；宿主内置功能（侧栏、首页提示、diff 视图、which-key、通知）本身也按插件形态组织，插件契约与宿主内部实现共用同一套槽位。
- **数据同步**：TUI 通过 `context/sync.tsx`（643 行）消费 SDK 事件流并维护本地视图状态；`context/data.tsx`（557 行）管理服务器数据缓存。
- **纪律**（`specs/tui-package.md` 明文）：工具渲染必须对**未知工具与协议字段变化保持宽容**（局部 `unknown` 检查可以，导入后端工具实现换取类型安全不行）；SDK 是唯一后端边界，缺数据就加服务端 API，而不是从后端实现里 import。
- 主题体系：30+ 内置主题 JSON + `theme/index.ts`（1 025 行）主题引擎；`config/keybind.ts`（443 行）键位定义与冲突处理。

### 13.3 对比

| 维度 | agentxx | opencode | 评价 |
|---|---|---|---|
| 界面来源 | 服务端产出声明式组件树，客户端唯一渲染实现 | 前端各自拥有一套组件（TUI 的 OpenTUI 组件 + Web 的 Solid 组件），共享仅限 Web 侧 | agentxx 的「一份描述、一处渲染」跨端一致性更省事；opencode 的分工更适合两套原生 UI |
| 扩展面 | 客户端插件（10 张表）：面板/装饰/定时器/快捷键/overlay/表单 | TUI 插件 + 槽位（slots）；宿主功能自身也用插件形态 | agentxx 的扩展面更宽（ABI 边界），opencode 的槽位更细（渲染粒度） |
| 未知内容宽容 | 已有纯文本降级（`plainText`）+ 组件解析容错 | 明文纪律：未知工具/字段必须宽容，不导入后端类型 | 两者一致；opencode 把它写成规格条款 |
| 流式渲染 | deltas 事件驱动重渲染；markdown 折行在构建期完成 | markdown 解析**放到 worker**（队列 + 传输 + 缓存），长回答不阻塞 UI | 可借鉴：agentxx 的大 markdown 重解析在主线程（FTXUI 界面线程） |
| 渲染层边界 | `ui_components` 在 client 内，与 io/网络同包 | 独立包 + 只依赖 SDK，可用导入边界测试守住 | 可借鉴（低成本）：给渲染层加「不依赖网络/会话」的边界检查 |
| 主题/键位 | 有主题与键位（含冲突列表）；`normalizeKeybindSpec` 规范化 | 30+ 主题 JSON + 独立键位模块 + 冲突处理 | 两者都已成型；opencode 的主题资产与纯数据化更彻底 |
| 表单项 | 宿主维护 `UiFormState`，控件/校验/取值唯一实现 | `prompt-input` v2 状态机 + store | 两者都是「宿主状态 + 纯渲染」，方向一致 |
| 测试 | `tui_*` 模块（输入/滚动/表单/组件/主题/surface 等） | happy-dom 单测 + Storybook + Playwright e2e + 视觉稳定性基准 | 可借鉴 Storybook/视觉快照的思路（agentxx 可用组件快照测试替代） |

### 13.4 两者的优缺点

**agentxx**
- 优点：服务端产出声明式组件树、客户端只有一份渲染实现（`ui_components`），插件可跨语言贡献 UI 且跨端表现一致；组件种类齐全（表格/树/趋势/计量/控件/折叠/分组框等）并有纯文本降级；命中区域（`UiHitRegion`）与滚动容器命中分离，行元素内嵌 `reflect` 由元素自有节点持有，避免悬空；懒滚动支持前插锚定（历史前插不跳视口）；表单状态集中在宿主（`UiFormState`），交互只有一份实现；能力协商让老宿主降级；键位冲突可见（只读列表 + 冲突记录）。
- 缺点：渲染实现与 io/会话在同一客户端包内，没有「渲染层不依赖网络/会话」的边界检查；markdown 折行与行数统计在界面线程执行，长回答或大代码块会占用帧时间；组件描述有 1 MiB 上限与内容 `version`，但缺「未知组件类型/未知字段必须降级」的成文条款与对应测试；没有组件级快照测试。

**opencode**
- 优点：TUI 抽成独立包（`@opencode-ai/tui`）且**只依赖 SDK**，配套「不得依赖后端实现」的规则与导入边界测试，工具渲染对未知工具/未知字段必须宽容也写成了规格；markdown 解析放到 worker（队列 + 传输 + 缓存），长回答不阻塞渲染；`session-ui` 让 Web/桌面共享消息渲染；插件槽位细（可插首页提示、侧栏、diff 视图、which-key 等）；主题（30+ JSON）与键位是纯数据化模块；有 Storybook、happy-dom 单测、Playwright e2e 与视觉稳定性基准。
- 缺点：终端与 Web 两套组件各写一遍，跨端一致性靠人工维护；渲染相关代码分散在 `tui`/`session-ui`/`ui` 三个包，定位一个组件要跨包；与 Solid/OpenTUI 生态绑定较深，替换渲染栈成本高。

### 13.5 可迁移到 agentxx 的设计

1. **[P1] markdown 解析/测量的 offload**：长回答或超大代码块时，把「折行 + 行数统计 + 高亮」放到工作线程或分片处理，界面线程只消费结果；已有 `wrap_line_by_width` 与 `estimateMarkdownLines` 的口径必须保持一致（同一函数）。落地位置：`agent/client/src/io/tui/markdown_*`。
2. **[P1] 渲染层边界检查**：新增测试断言「渲染层（`ui_components.*`、组件解析）不依赖网络/会话/插件管理器」，保证它可独立测试与复用（对应 opencode 的「TUI 只依赖 SDK」）。落地位置：新增 `agent/test/client/test_tui_boundary.h`。
3. **[P1] 未知内容的宽容性条款化**：把「未知组件类型 → 纯文本降级、未知字段忽略、缺能力 → 降级」写进 `docs/zh-cn/design/tui.md`，并加一条测试：给出「来自未来版本」的组件树（含未知类型与字段）时渲染不崩且能给出降级文本。落地位置：`tui.md` + `agent/test/core/test_ui_items.h`。
4. **[P2] 组件快照测试**：为每个组件类型与关键组合补「固定输入 → 固定渲染结果」的快照断言（含窄终端、超长文本、CJK 宽度），替代人工目测。落地位置：`agent/test/client/`。
5. **[P2] 主题资产与键位数据化**：主题/键位做成纯数据 + 校验（现在分散在 TUI 实现里），便于用户自定义与插件提交。落地位置：`agent/client/.../tui/`（主题/键位模块）。

---

## 14. 远程协议、SDK 与嵌入模式

### 14.1 agentxx 现状

- **Wire 协议**：手写 JSON 消息（`wire_protocol.h`，30+ 消息类型）：Hello/HelloAck、UserInput、Cancel、SelectModel、GetModel/ModelInfo、Delta、Sync、InterruptRequest/Response/Expired、TurnResult、ContextStats、Error、Log、GetAppendComponentInfo/AppendComponentInfo、GetContext/ContextMessages、Ping/Pong、CompactContext、ListSessions/SessionList、SwitchSession、GetViewMessages/ViewMessagesPage、ClearMessageQueue、RemoveQueueItem、InterruptAndRunNext、MessageQueueUpdate、PluginData/PluginDataUp、ListDir/ListDirResult、GetPermissionState/SetFullAuth/PermissionState 等。
- **传输抽象**：`AgentIOBase`（端点）+ `AgentIOTransportBase`：同进程 `ChannelAgentIOTransport`（零序列化），跨进程 `WsAgentIOTransport`（JSON over WebSocket）；**同进程与远程只换 transport，端点和数据流完全一致**（无「直连旁路」）。
- **重连**：客户端带 `lastSeq` 增量续传；seq 不连续或水位高于服务端当前 seq 时回退全量 `Sync`；`SyncPayload.deltaSeq` 携带快照水位；`fromIndex`/`totalMessages` 支持历史尾窗 + 分页拉取。
- **其它协议**：MCP（client/server，多版本协商，HTTP SSE + stdio）、A2A（server/client，任务管理）、ACP（stdio server）、FFI（C API 桥）。
- **无 SDK/代码生成**：客户端（TUI/CLI）直接读写 wire 结构；第三方接入需要按文档手写。

### 14.2 opencode 现状

- **单一定义 → 代码生成**（`packages/protocol` + `httpapi-codegen` + `client`）：
  - `packages/protocol` 用 Effect `HttpApi` 定义分组（health/location/agent/session/message/model/provider/integration/credential/permission/fs/command/skill/event/pty/question/reference/projectCopy），并**拥有中间件位置**（Location / SessionLocation），具体中间件键由 Server 注入；
  - `httpapi-codegen` 把 `HttpApi` 编译成「SDK Contract IR」，再由两个 emitter 生成 **Promise 客户端**（零 Effect、结构型 wire 类型、语法解析、不运行时校验）与 **Effect 客户端**（解码后的领域值、保留 brand 与转换、运行时 schema 解码）；
  - 生成的客户端里对消费者友好的命名（`groupNames`/`endpointNames`/`omitEndpoints`）在 `client/src/contract.ts` 显式声明；生成物提交入库，CI 重新生成并校验工作树无差异（`check:generated`）。
- **同一个 client 两种部署**：网络客户端与 **Embedded OpenCode**（内嵌宿主）共用同一套生成客户端与同一份路由/handler/错误，唯一区别是 `HttpClient` 传输（真实网络 vs 内存）；内嵌宿主在同一个对象上直接暴露「仅同进程可用」的额外能力。
- **事件流分两类**：`sessions.events({sessionID, after})` 是**可重放的 durable 会话事件流**（校验会话、按 `after` 重放、继续 tail，SSE 传输，排除实时片段）；`events.subscribe()` 是**实例级实时流**（无重放保证，含连接/心跳/实例销毁事件）；两者**都不自动重连**，由消费者显式刷新后重新订阅。
- **分页规范**：列表与消息都用 `Page { items, previous, next }`，游标是不透明 brand，携带续查所需的排序/方向/锚点信息；文档明确「用另一个会话的游标是无效的」。
- **HTTP 层**：`server/` 提供 handler 与中间件（授权、schema 错误、会话位置）、CORS、鉴权；`packages/opencode` 内仍有旧 HTTP 服务与 SDK（`sdk` 包 44 文件 / 2.76 万行，含旧版 JS SDK）。

### 14.3 对比

| 维度 | agentxx | opencode | 评价 |
|---|---|---|---|
| 协议定义 | 手写结构体 + `toJson/fromJson` | 单一定义（HttpApi）+ 生成两套客户端 + OpenAPI | opencode 无手写漂移、天然有 OpenAPI；agentxx 的类型安全靠 C++ 编译期与测试 |
| 客户端接入 | 文档 + 手写（TUI/CLI 直接实现） | 生成 SDK（Promise/Effect），第三方直接可用 | opencode 对外集成门槛低；agentxx 若要开放生态，生成绑定价值大 |
| 同进程 vs 远程 | 只换 transport（端点和数据流一致） | 只换 `HttpClient`（路由/handler/错误一致） | **两者思路一致**，agentxx 的实现更彻底（无 HTTP 这一层） |
| 重连 | 自动重连 + lastSeq 增量 + 回退全量 + 尾窗分页 | 不自动重连，显式刷新；durable 事件流用 `after` 游标 | 各有取舍：agentxx 对终端用户更省心，opencode 对多端渲染更可控 |
| 事件流分层 | delta 流 + Sync 快照 + 内存重放缓冲 | durable 事件流（可重放）与实时流（不可重放）严格分开 | **可借鉴**：分层让「重连后如何恢复」有明确语义 |
| 分页 | `GetViewMessages(beforeIndex, count)` + 绝对下标 | 不透明游标 + Page 信封 | opencode 的游标封装更抗演进；agentxx 的绝对下标简单直观（append-only 才成立） |
| 鉴权 | WS token 鉴权 | 授权中间件 + 集成/凭据体系 + PTY ticket | opencode 的凭据体系更完整（见 §17） |
| 其它协议 | MCP/A2A/ACP/FFI 都在 | MCP、PTY、OpenAPI 与 SDK | agentxx 的协议面更宽；opencode 的 SDK/OpenAPI 面更强 |
| 生成物纪律 | 无生成物 | 生成物提交 + CI 校验 + `.httpapi-codegen.json` 记录归属 | 可借鉴（若引入生成） |

### 14.4 两者的优缺点

**agentxx**
- 优点：同进程与远程**只换 transport**（端点、数据流、事件序列完全一致，没有「同进程直连旁路」），`runTurnAsync(io=nullptr)` 的 headless 是唯一例外；自动重连 + `lastSeq` 增量 + 水位异常回退全量 + 尾窗分页（对终端用户最省心的一层）；`SyncPayload.deltaSeq` 明确携带快照水位，客户端去重有依据；进程内 `ChannelAgentIOTransport` 零序列化；协议面宽（MCP client/server、A2A server/client、ACP server、FFI）。
- 缺点：wire 消息是手写的结构体 + `toJson/fromJson`（30+ 消息类型），双端容易漂移，也没有 OpenAPI/SDK 供第三方接入；增量重放依赖服务端内存缓冲（服务端重启即失效）；历史分页用绝对下标（一旦允许删除或分支就失效）；错误语义不如 opencode 细分（缺少「会话不存在 / 消息不存在」这类可区分错误）。

**opencode**
- 优点：`HttpApi` 单一定义 → 生成 Promise/Effect 两套客户端 + OpenAPI，产物入库并有 CI 一致性校验（`check:generated`）；网络客户端与「Embedded OpenCode」共用同一客户端与同一路由（只换 `HttpClient`）；durable 会话事件流（可 `after` 重放）与实例级实时流（无重放保证）严格分开，两者都明确不自动重连（恢复策略留给消费者）；列表/消息统一 `Page` + 不透明游标；错误类型区分清楚（`SessionNotFound` / `MessageNotFound` / `PromptConflict` 等）；PTY 有 ticket 授权。
- 缺点：生成链长（HttpApi → Contract IR → 两个 emitter），改协议要走生成流程并接受产物 diff；不自动重连意味着 UI 必须自己实现「刷新 + 重订阅」，否则断线即状态陈旧；内嵌宿主的作用域要求严格（请求作用域必须覆盖到流式响应体结束）；包依赖规则（client 不得依赖 core/server）限制了一些「直接复用实现」的便利做法。

### 14.5 可迁移到 agentxx 的设计

1. **[P1] 单一定义的协议描述 + 生成绑定**：把 wire 消息集中到一份**机器可读描述**（JSON/YAML schema），由它生成：C++ 的编解码与结构体（替代手写 `toJson/fromJson`）、文档表格、可选的语言绑定（TS/Python）。落地位置：新增 `agent/lib/protocol/wire_schema.yaml` + 生成脚本（构建期，产物提交并加一致性校验，参考 opencode 的 `check:generated`）。
2. **[P0] 事件流分层**（与 §5 配套）：明确两类流：
   - **durable 会话事件流**：客户端带 `after` 拉取并续接（服务端重启后仍可续），用于重连恢复与审计；
   - **实时增量流**：文本/思考/工具参数等片段，不保证重放（现在 agentxx 的 delta 缓冲相当于把两者混在一起）。
   落地位置：`wire_protocol.h` 增加 `WireGetHistory`/`WireHistory`，`SessionServerAgentIO` 把「重放缓冲」改为「库内事件 + 实时续接」。
3. **[P1] 不透明游标规范**：为会话列表与历史分页定义「只由创建者解释」的游标（当前 `beforeIndex` 是绝对下标，一旦允许删除/分支就会失效）。落地位置：`wire_protocol.h` 的会话列表/历史消息段。
4. **[P1] 连接阶段与错误语义显式化**：把「未握手 → 未绑定会话 → 就绪 → 重连中」作为显式状态机，并对「请求了不存在的会话/消息」给出可区分错误（opencode 的 `SessionNotFoundError`/`MessageNotFoundError` 分得很清）。落地位置：`ws_io_transport.h` + `session_server_agent_io.cpp` 的 Hello 流程。
5. **[P2] 开放 SDK**：若要把 agentxx 作为「本地 agent 服务」开放给第三方（编辑器插件、脚本），提供一份生成的轻量 SDK（先做一个语言，如 TS），内嵌模式与远程模式共用同一 API 表面（agentxx 已有 FFI，可作为「同进程 SDK」的另一形态）。

---

## 15. 扩展机制：C ABI 插件 vs Effect 插件

### 15.1 agentxx 现状

- **纯 C ABI 动态库插件**：入口 `agentxx_plugin_agent_create/destroy`（client 侧 `agentxx_plugin_client_*`），全部接口表版本为 1，8 字节对齐，结构体参数传指针、返回值走 `int32_t` 状态码出参。
- **表结构（本次实测头文件）**：通用表 10 张（`pluginxx.{capabilities, config, log, json, events, tasks, scheduler, cancel, coroutine_runtime, plugins}`）+ 领域表 10 张（`agentxx.agent.{core, session, prompt, tools, model, permission, graph, hooks, resources, context}`）；client 侧 10 张（`agentxx.client.{ui, events, session, wire, self, json, log, timer, keybind, panel}`）。注：`AGENTS.md` 记的是 19/9，本文以头文件实测 20/10 为准（差异应是新增 `agentxx.agent.context` 等表后未同步文档）。
- **框架已经拆成独立工程 `cxx_pluginxx`，分四层**（本次细读其全部头文件）：
  1. **ABI 契约层**（`pluginxx/api/*`）：`abi.h` 定义冻结的跨边界契约（`PLUGINXX_EXPORT` / `PLUGINXX_CALL` / `PLUGINXX_API_VERSION = 1` / `#pragma pack(push,8)` / `PluginxxStringView` / `PluginxxString` / 统一异步原语 `PluginxxOperatorNotify`+`PluginxxOperatorHandle`+`PluginxxDriver`+`PluginxxCancelToken` / 核心 vtable 只有 `alloc`/`free`/`query_interface`）；`tables.h` 是 10 张通用表的 C 结构；`entry.h` 把**入口符号名**交给宿主（内核不含 `agentxx_plugin_*` 之类专名，宿主覆写 `entrySymbols()`，缺符号名装载直接失败而不是猜）。
  2. **内核宿主层**（`pluginxx/host/*`）：`manifest.h`（plugin.yaml：接口声明/依赖/资源）、`loader.h`（原生库加载）、`lifecycle.h`（1310 行：装载/启停/禁用启用/卸载/级联/关闭超时重试的唯一实现）、`host_core.h`（609 行：10 张通用表的**状态与实现**）、`tables_impl.h`（946 行：C ABI 入口 = 投递到 IO 线程 + 调用 `PluginHostCore` 同名方法）、`domain_hooks.h`（通用表需要宿主数据时的接缝）、`event_bus.h`、`capability_registry.h`。
  3. **内核运行时**（`pluginxx/runtime/*`）：`runtime.h`（`PluginRuntime` + `InstanceLifetime` 状态机 + 执行 lease + 动作投递/重放）、`instance_base.h`（`PluginInstanceBase`：元信息/依赖/启用标志/`InflightGuard`/驱动句柄表（任意线程取消 + 墓碑窗口）/宿主控制块 tombstone）、`manager_base.h`（插件表、名称预占、`postToIo`/`isIoThread`、`waitInflightZero`、反向必选依赖收集）、`op_driver.h`（协作式取消与操作驱动）、`driver.h`。
  4. **插件侧 SDK**（`pluginxx/kit/kit.h`，3545 行）：`PluginStringView`/`PluginString` RAII、`queryInterface<T>`/`PluginIfaceCore` 表聚合、实例级 `Logger`、`Task` 锚定协程与 `sleep`/`yield`/`offload`/`invoke_cap`、`CancelRegistry`/`OpCtl`/`ArgReader`、`capability` 注册、导出宏 `PLUGINXX_EXPORT_PLUGIN`；宿主领域部分在 `agentxx/plugin/api/plugin_kit.h`（umbrella：包含内核头 + 逐条 `using` 引入 `agentxx::plugin`，插件源码零改动）。
- **生命周期是状态机 + 事务，不只是四个函数**：`PluginInstanceState{Loading, Ready, Closing, Closed, CloseFailed}`；`create` 只构造上下文，`start` 是注册事务（在插件所属 executor 上执行，失败由宿主回滚），`stop` 撤销自管资源（可重复），`destroy` 前必须 `stop` 完成（`lifecycleStopPending()` 即「stop 欠着」的判据）；`finishLoad`/`rollbackLoad` 两条路径覆盖「原生库」与「内置合并」两种加载方式（内置插件 `create/destroy` 由宿主符号直接给，动态库走 `dlsym`）。
- **执行 lease 是卸载安全的核心**：所有跨边界入口先经 `enterPluginHost()` 取得 **admission lease**（`InflightGuard`），因此「已进入插件代码」与「已投递但还没执行」的调用都被卸载等待覆盖；`unloadAsync` 等 lease 归零（事件式 `waitIdleUntil`，不是轮询）；同步关闭发现活动 lease 时走 `deferInstanceShutdown` + `setIdleCleanup`，等最后一个 lease 释放再 `destroy`，**绝不在有 lease 的情况下 `dlclose`**。宿主控制块地址永不失效（实例关闭后只清空引用形成 tombstone），插件拿着旧 `host` 指针只会安全失败。
- **通用表入口的统一骨架**：`tables_impl.h` 里每个入口都走 `enterPluginHost`（`allowClosing` 区分「注册类」与「只读/取消类」）→ `ioCallSyncKeep` 投递到宿主 IO 线程 → 调用 `PluginHostCore<InstanceT>` 的同名方法；参数视图按值拷进闭包，跨线程返回值经 `host->alloc` 写回；注册类入口在做实际注册前还会复查 `acceptsRegistration()`（请求排队期间实例可能已进入 Closing/Disabled，否则会在撤销之后又留下残留注册）。
- **宿主侧管理器（agent 侧 `PluginManager`）负责的领域事务**：`detachDomainRegistrations()` 撤销工具注册、工具权限声明、图节点类型槽（`invalidate`）、提示词贡献（**`restorePromptBackup`**，即宿主对插件改过的 `systemPrompt` / `appendSystemPrompts` / `toolPrompt` 做备份并在禁用卸载时还原）、停用插件中间件；`clearDomainRegistrations()` 在 stop 成功后清空「由 start 事务重新声明」的记录；`releaseInstanceResources()` 释放工具对象与清单资源；`onInstanceEnabledChanged()` 同步资源应用器的启用标记。工具表 `ToolRegistry` 用 `shared_ptr` 持有，注册冲突（与静态工具名集合比对）即拒绝，摘除后执行中的调用仍由局部 `shared_ptr` 保活。
- **图节点与插件节点**：插件可注册图节点类型，宿主用 `GraphTypeSlot` 间接管理（注册表没有删除接口，于是用「实例弱引用 + 代次 generation + active 标记」让旧节点失效）；`PluginGraphNode::run` 把图状态序列化后调用插件的 `run_start` 回调，完成经 `notify->done` 上报节点输出，取消经 `run_cancel` 传递；插件按旧契约写 `messages` 通道时，宿主改写成会话上下文写入（空列表视为误写忽略），这是明确的兼容层。
- **client 侧同构但更重**：`ClientPluginManager`（4 009 行）复用同一内核（装载/启停/禁用/卸载由 `PluginHostLifecycle` 提供，只保留「装载」自有实现：`dlopen` 卸载到内部线程池 + 接口协商 + 双端入口探测），并额外维护：UI 注册表快照（`uiRegistrySnapshot`）、工具渲染缓存（输入哈希 + 在途去重 + 淘汰 + 按插件失效）、命令路由与动作回传、键位规范化（`normalizeKeybindSpec`）、客户端状态 JSON、以及把 agent 侧事件（`onDelta`/`onTurnResult`）转发给插件。
- **生命周期是事务**：`create` 只构造上下文；`start` 在宿主 io 线程执行注册（失败返回错误由宿主回滚）；`stop` 撤销自管资源（线程/定时器/订阅，可重复调用）；`destroy` 只释放本地对象。缺 `start/stop` 的插件宿主拒绝加载。
- **多实例三铁律**：禁止可变全局/函数级 static；实例状态只放 `*plugin_ctx` 堆块，回调经 `spec.user_data` 恢复；接口表查询结果存实例上下文。
- **符号与依赖纪律**：插件只导出入口符号（ELF `-fvisibility=hidden` + version script 白名单，macOS 导出列表，MSVC 只 `dllexport`）；插件静态链接 `cxx_utilxx_base`/`cxx_utilxx` 一份（可用 `cxx_json`/string/http/ws/regex 等）；主程序与插件动态链接 C++ 标准库，也可以各自静态链接。
- **权限声明**：插件注册工具后经 `agentxx.agent.permission` 声明「作用域 + 目标来源 + 目标参数名 + 分类」，宿主统一判定；声明随工具注销/插件禁用卸载自动撤销；模式/前缀参数工具另经 `check_paths` 做逐路径三态复核（§10）。
- **能力协商**：`pluginxx.capabilities` + client 侧能力名（如 `agentxx.client.components`/`form`/`layout`），老宿主缺失即降级。
- **插件形态**：15 个内置插件（filesystem/execute_command/string/math/system/websearch/rag_search/planning/codegraph/javascript_engine/system_monitor/screen_capture/computer_use/text_selection_monitor/audio_stream）+ 5 个示例目录（`example_plugin`、`example_graph_node`、`example_js`、`example_js_execute_command`、`example_resources`），平台支持矩阵按插件声明（screen_capture/computer_use/text_selection_monitor 仅 Windows，audio_stream 全平台未实现）。

### 15.2 opencode 现状

- **两代插件并存**：
  - **v1（Promise hooks，`packages/plugin/src/index.ts`）**：插件是 `(input, options) => Promise<Hooks>`，`Hooks` 覆盖 `tool`（新增工具）、`chat.message`（用户消息落地前变换）、`chat.params`（温度/上限/选项）、`chat.headers`、`permission.ask`（可改判定结果）、`tool.execute.before/after`（改参数与结果）、`shell.env`（注入环境变量）、`command.execute.before`、`experimental.chat.messages.transform`、`experimental.chat.system.transform`（任意改系统提示词）、`experimental.session.compacting`（改压缩提示词）、`experimental.text.complete`、`tool.definition`（改工具描述与参数）等；还包含 `auth`（providers 的 OAuth/API Key 流程与提示）、`provider`（声明模型列表）、`event`、`config`（改配置）。
  - **v2（Effect 原生插件，`packages/core/src/plugin.ts` + `plugin/host.ts`）**：插件是 `effect(host) => Effect`，装载时给出 `host`，包含**声明式 transform**：`agent.transform`、`catalog.transform`（provider/model 增删改与默认值）、`command.transform`、`integration.transform`（连接方式与授权方法）、`reference.transform`、`skill.transform`、`aisdk.hook.{sdk,language}`（改底层 SDK 或语言模型），以及 `plugin.add/remove`（插件装载其它插件）。
- **Scope 语义**：`PluginV2.add(id, effect)` 为每个插件开一个子 Scope；`remove`/`add`（替换）时**关闭旧 Scope**，插件在装载期间注册的 transform、工具、订阅全部随 Scope 结束自动撤销；重复装载同一 id 会先关旧的；装载失败会关闭子 Scope 并把失败状态记下（`wait` 可等待结果）。
- **v2 的两类扩展点区分得很清楚（`packages/plugin/src/v2/effect/PLAN.md`）**：
  - **transform（可重放）**：注册后由所属域「重放」出该域的最终状态；`Registration.dispose` 可提前摘除且幂等；注册/注销会自动触发该域重建；批量装载时自动重建被**推迟**（`State.batch`），一批结束后每个受影响的域只重建一次；`rebuild()` 总是重放该域的全部活动 transform，重建**串行化并合并**（重建期间到来的调用最多再排一次）；重建开始时快照注册列表，并发注册变更只影响下一次重建；重放期间禁止注册/注销 transform（当前域的重建被拒绝，其它域的重建被推迟到本次 transform 结束）；transform 没有类型化错误通道（意外失败即缺陷）。
  - **runtime hook（即时拦截）**：按注册顺序**串行**执行，后执行的 hook 能看到前面 hook 的修改；只影响后续调用（在途调用用开始时快照的注册列表跑完）；不参与域重建；接收「目的化的上下文对象」（只读数据 + 允许修改的方法），规范明确**不得暴露核心 draft 或内部对象**。
- **域的状态模型与终结化**：`base state → 按顺序重放活动 transform → 核心终结化（finalization）→ 提交生效状态 → 发布 updated 事件`；终结化只做不变式与物化（catalog 的策略过滤与校验、reference 的仓库物化、integration 的连接投影、索引构建、提交后事件），更新事件在新状态可见之后发；**没有跨域事务**（规格明确不引入），跨域读取只能读到对方「已提交」的状态，且插件自己要负责在依赖变化时触发自己的域重建。
- **boot 批处理与插件顺序**：`PluginInternal` 用**同一个 `PluginV2.add`** 注册全部内置插件（12 组：reference → agent → command → skill → models.dev → 配置派生的 agent/command/skill → 32 个 provider 插件 → 外部配置插件 → 配置 provider → variant），整个过程包在 `State.batch(...)` 里并 `forkScoped` 异步启动（不阻塞 Location 就绪）；默认顺序的意图写在 PLAN.md 的「Plugin Order」：内置 agents/commands/skills → models.dev 等基础数据源 → 配置投影 → provider 归一化与鉴权 → 用户插件 → 核心终结化；**同 id 替换会保留原来的顺序位置**（旧插件先被禁用再跑新的 setup）。
- **插件服务来自同一套服务图**：宿主给插件的 `PluginHost`（`core/src/plugin/host.ts`）不是内部对象的直接暴露，而是「域编辑器 + 域重载 + `aisdk` hook + `plugin.add/remove`」；域的 draft 用 `mutable()` 包一层，只暴露允许的修改方法。服务的装配/替换/生命周期由 `core/src/effect/layer-node.ts` 统一表达：节点分 `layer`/`unbound`/`group`，`tag` 用来把服务提升到全局或 Location 作用域，`replacements` 支持按节点替换（替换前后的 tag 与错误类型在**类型层**被校验），编译时做环检测并合成 Effect `Layer`。
- **外部插件与 Promise 适配**：`config/plugin/external.ts` 从配置文档的 `plugins` 段（npm 包名 / `file://` / 相对路径）与配置目录下的 `plugin/*.{ts,js}`（glob 后按名排序）收集候选，动态 `import()` 后用 Schema 校验模块形状（`{id, effect}` 或 `{id, setup}`），Promise 形态经 `PluginPromise.fromPromise` 适配成 Effect 插件——适配器保留 Scope 与**启动时的 fiber 上下文**，因此 Promise 插件的 transform 注册同样能并入 boot 批处理、同样随 Scope 撤销；整个过程在 scoped fiber 里跑且 `Effect.ignoreCause`，单个插件装载失败不拖垮宿主。
- **TUI 插件是一套独立但同形的体系**：`packages/plugin/src/tui.ts`（568 行）定义类型面（路由、键位层、对话框、主题、KV 存储、toast、attention/声音、`TuiSlotMap` 槽位、`TuiPromptProps` 等），并按 v1 兼容保留被标 `@deprecated` 的 `api.command` 旧形态；宿主侧 `packages/tui/src/plugin/{slots,api,runtime,adapters,command-shim}.tsx` 提供：槽位注册表（`createSolidSlotRegistry`，按「插件/槽/阶段」上报插件错误，不互相影响）、路由注册表（同名路由取**最后注册**的渲染函数，返回注销函数）、以及 `AbortController` 形式的生命周期信号；宿主自己的侧栏/首页提示/diff 视图/which-key 也以插件形态组织（`feature-plugins`），即**宿主内置功能与第三方插件走同一条路径**。
- **重算而非撤销**：catalog/config 类服务的状态由「当前活动的 transform 集合」重算（apply active transforms → apply policy → commit diff → 发 `Catalog.Event.Updated`），所以插件禁用后它的影响自然消失，不需要手工 undo（`specs/v2/catalog-config-plugin-lifecycle.md` 对比了「配置 transform + 全服务重载」与「catalog transform + 内部重建」两种方案，当前 core 选了后者的可重放 transform 形态）。
- **插件分发**：npm 包 / git / 配置目录 / 内置；`PluginInput` 提供 `client`（生成的 SDK 客户端）、`project`、`directory`、`worktree`、`serverUrl`、`$`（Bun shell）、实验性 workspace 注册。
- **分级信任**：TUI 插件与 server 插件分开（`PluginModule.tui` 明确标为 `never`，即 server 插件不能声明 TUI 部分）；UI 侧的扩展走 `packages/tui` 的槽位与 feature-plugins。
- **规格里明确未完成**：V2 插件尚未提供等价于 v1 `experimental.chat.system.transform` 的 hook（`CONTEXT.md` 的 Flagged ambiguities 明确记录），也没有插件定义的 Context Source（列为后续）。

### 15.3 对比

| 维度 | agentxx | opencode | 评价 |
|---|---|---|---|
| 边界 | C ABI 动态库：跨语言、跨编译器、版本隔离 | 进程内 TS/Effect：能力最强但只能同语言、同运行时 | **agentxx 更强**（生产分发与稳定性）；opencode 更灵活（可实现任意内部细节） |
| 表/接口数 | 20（agent）+ 10（client）张表 | v1 约 20 个 hook + v2 十余个 transform/hook | 两者都多；agentxx 的表按能力域划分，opencode 的 hook 按时机划分 |
| 生命周期 | create/start/stop/destroy 四段 + 宿主回滚 + 重复 stop 幂等 | Scope：装载即生效，关闭即全部撤销 | 两者思路接近；opencode 的「关闭即撤销 + 重算」更省手工清理 |
| 回滚语义 | 插件自己负责 stop 撤销；宿主只管生命周期与级联依赖 | 宿主负责：transform 集合重算，插件无需写 undo | **可借鉴**：对「声明式贡献」（模型/工具/提示词段/上下文来源）用重算替代手工撤销 |
| 多实例 | 明文三铁律 + 每实例 `plugin_ctx` | 每个 Location 一份宿主；插件装载按 Location 作用域 | agentxx 的多实例纪律更严格（写进文档与测试） |
| 依赖复用 | 静态链接 `cxx_utilxx(_base)`，符号隐藏，主程序/插件各自一份 | 直接 import 工作区包（同版本），无隔离 | agentxx 的隔离更安全（不同版本可共存）；opencode 更省心 |
| 权限与安全 | 插件受能力协商与权限声明约束；符号白名单 | 插件与宿主同权限（进程内代码），无能力边界 | **agentxx 更强** |
| 热重载 | 启用/禁用/卸载 + 级联依赖；禁用即撤销注册 | 每次 `add` 替换旧 Scope，单点更新只让依赖它的部分反应（细粒度事件） | **可借鉴**：细粒度更新与依赖反应 |
| 调试成本 | ABI 边界 + 无栈回溯成本高 | 同进程可直接调试 | opencode 更省事 |
| 契约形态 | 冻结的 C 结构体 + 宿主定义入口符号；同一内核可服务多个宿主（agentxx / musicxx） | TS 类型 + 运行时 Schema 校验；必须同语言同运行时 | 对外分发用 agentxx 的形态；内部扩展用 opencode 的形态 |
| 注册语义 | 宿主登记表 + 反向操作撤销（提示词用 `PromptBackup` 还原、图类型用代次槽失效） | transform 集合重算（禁用即影响消失） + hook 即时拦截（串行、后见前） | opencode 的「重算」更省手工撤销；agentxx 的记账更显式 |
| 顺序与依赖 | `depends`/`optional_depends` + 拓扑加载 + 反向级联卸载 | 无依赖声明；boot 显式顺序 + 同 id 替换保留顺序位 + 批量重建合并 | 互补：依赖模型 vs 顺序/重建语义 |
| 卸载安全 | admission lease + 事件式等空闲 + 延迟收尾 + 旧 host 指针 tombstone + 关闭超时状态 | Scope 关闭（fiber 中断 + finalizer） | **agentxx 明显更细**（因为要真的 `dlclose`） |
| 批量启停 | 逐个禁用/启用（各自重建 start 事务） | `State.batch` 内推迟重建，一批结束后每域重建一次 | 可借鉴：把批量重算合并（M76） |
| 装配替换 | 显式依赖注入，无「按名替换服务」接缝 | `LayerNode.replacements`（tag/错误类型在类型层校验）+ `hoist` 作用域提升 | 可借鉴（M77） |
| 错误隔离 | 关闭异常有状态与日志；插件渲染错误回退 | TUI 槽位按「插件/槽/阶段」上报错误，互不影响 | 可借鉴（M78） |
| 可观测性 | `list()`/`getPluginJson()` 含接口、能力、依赖、状态 | 事件 `Plugin.Event.Added` + 失败可 `wait` | agentxx 信息更全；可再加只读域视图（M79） |

### 15.4 两者的优缺点

**agentxx**
- 优点：纯 C ABI + 固定 ABI 规则（版本 1、8 字节对齐、定长类型、`PLUGINXX_CALL`、结构体指针参数 + `int32_t` 状态码）让插件可跨语言、跨编译器；**内核与宿主解耦**（入口符号名由宿主提供，内核零宿主专名，同一内核供多个宿主复用）；框架本体已拆为独立工程 `cxx_pluginxx`（ABI 契约层 / 内核宿主层 / 内核运行时 / 插件 SDK 四层）；`create` / `start` / `stop` / `destroy` 四段生命周期把「注册」做成宿主可回滚的事务，配五态状态机与 `CloseFailed` 显式失败态；**卸载安全机制完整**：admission lease（覆盖已进入代码与已排队未执行的调用）、事件式等待空闲、延迟收尾（`deferInstanceShutdown` + idle 回调）、旧 `host` 指针 tombstone、驱动句柄墓碑窗口、`hasPendingClose()` 自检；通用表入口统一「解析控制块 → 投递 IO 线程 → 调 `PluginHostCore` 同名方法」并在真正注册前复查实例状态；插件清单有 `depends`/`optional_depends`，加载做拓扑排序、卸载做反向级联；多实例三铁律 + 测试；导出符号白名单 + 静态链接工具库让不同版本可共存；能力协商使老宿主可降级；平台支持矩阵按插件声明。
- 缺点：接口表多（本次实测 agent 侧 20 张、client 侧 10 张），文档与实现容易不同步（`AGENTS.md` 记 19/9 与实测 20/10 不一致）；插件的声明类贡献要靠**逐项反向操作**撤销（提示词用备份还原、图类型用代次槽失效、中间件用标记停用），漏一处就会残留，缺少「集合重算」这种一次性语义；没有细粒度变更事件与批量重算合并（启用/禁用逐个处理，UI 多为整体刷新）；没有「按名字替换服务实现」的装配接缝（测试替身与嵌入变体要多写代码）；ABI 边界下的调试（无栈回溯、跨库异常）成本高；插件渲染错误只有回退，缺少可查询的错误记录；插件状态容器靠 `plugin_ctx` + `user_data` 约定。

**opencode**
- 优点：插件装载即开子 Scope，替换/卸载时关闭旧 Scope，注册物（transform/hook/工具/订阅）全部自动撤销，插件不用写 undo；贡献分两类且语义写得很细（transform 可重放、注册/注销触发域重建、批量装载推迟重建、重建串行化并合并、重建快照注册列表、重放期禁止再注册；hook 串行、后见前、只影响后续调用、不参与重建、不暴露内部 draft）；派生状态**重算**而不是手工回滚，禁用插件后影响自然消失；域状态模型明确（base → 重放 transform → 核心终结化 → 提交 → 发 updated 事件），且明确**不做跨域事务**；同 id 替换保留原顺序位；宿主给插件的是一组「域编辑器 + 域重载 + aisdk hook + plugin.add/remove」的目的化接口，不暴露核心 draft；服务装配由 `LayerNode` 表达（tag 作用域提升、按节点替换并在类型层校验、编译期环检测），测试与嵌入都靠它；内置插件与外部插件走同一 API（`PluginInternal` 用同一个 `PluginV2.add` 注册 12 组内置插件），外部插件支持 npm 包 / `file://` / 相对路径 / 配置目录 `plugin/*.{ts,js}`，动态 import + Schema 校验 + Promise 适配（保留 Scope 与启动 fiber 上下文），单个插件失败 `ignoreCause` 不影响宿主；TUI 插件有自己的槽位/路由/键位层体系，宿主内置功能也走同一路径，且槽位注册表按「插件/槽/阶段」隔离错误。
- 缺点：插件与宿主同权限（同进程同语言），能任意改系统提示词、替换 SDK 语言模型，没有任何能力边界；只能同语言同运行时，无法跨语言分发，也无法做二进制级版本隔离（v1/v2 两代 API 只能靠可选字段与 `@deprecated` 共存，能力不对等：v2 至今没有 `experimental.chat.system.transform` 的等价物）；`PLAN.md` 里规划的 `ctx.tool.hook("execute.before")` 等运行时 hook 面**尚未落地**（当前 v2 实际只有 transform + aisdk hook + plugin.add/remove，读文档容易高估现状）；hook 多但缺「能力声明/兼容协商」；插件装载与卸载没有像 agentxx 那样的「关闭超时 + 显式失败态 + 延迟收尾」路径，出问题只能靠 Effect 运行时的中断语义。

### 15.5 架构设计细读：逐维度对照

上一节的两张清单只是「有什么」，这一节按插件框架的十个关键维度把**实现方式**摆在一起看，是本节最值得读的部分。

**1) 边界与契约形态**
- agentxx：跨边界只有 C 结构体与函数指针，且 `api/abi.h` 明确「C 符号名 / 结构体名 / 宏名 / IID 字符串一律冻结」；入口**符号名**也由宿主给出（`entrySymbols()`），内核一个宿主专名都不含 —— 同一内核可以给 `agentxx` 与 `musicxx` 两个宿主用。
- opencode：跨边界是 TypeScript 类型与运行时 Schema（`Schema.Struct({id, effect})` 解析插件模块）；类型在编译期给作者提示，但**二进制层面没有契约**，插件与宿主必须同语言、同运行时、同版本线（v1 与 v2 两代 API 只能靠可选字段与 deprecated 标记共存）。
- 结论：agentxx 的边界更适合分发给第三方；opencode 的边界更适合「同一团队写扩展」。

**2) 生命周期与所有权**
- agentxx：`create → start（注册事务）→ stop（撤销）→ destroy`，配 `PluginInstanceState` 五态机；宿主提供 `finishLoad` / `rollbackLoad` 两条公共装配与回滚路径；`destroy` 幂等，且**有 lease 时拒绝销毁**（`destroyDeferred`）。
- opencode：装载即执行 `effect(host)`，注册物挂在 Scope 上；「撤销」= 关闭 Scope（fiber 中断 + finalizer）+ 域重放；没有独立的 start/stop 事务，也没有「stop 欠着」这种状态（收尾由 Effect 运行时保证）。
- 结论：agentxx 的状态更多、语义更显式（也因此更需要测试覆盖）；opencode 的状态更少、依赖运行时语义。

**3) 贡献与撤销路径**
- agentxx：宿主维护登记表（工具名、权限声明、图类型槽、hook、提示词贡献），卸载时**逐项撤销**；提示词类贡献用 `PromptBackup` 备份/还原，图节点用代次槽失效，中间件用 `disabled` 标记 —— 是「记账 + 反向操作」。
- opencode：域状态由「活动 transform 集合」**重算**，插件禁用后影响自然消失；宿主不记录「这个插件改过什么」。
- 结论：**这是最值得 agentxx 借鉴的一条**（M49 已列，本轮补充了实现细节）：对「声明类贡献」用重算替代反向操作，能显著减少「漏撤销」这类缺陷。

**4) 顺序与依赖**
- agentxx：插件清单里显式 `depends`（必选）/`optional_depends`（可选）；加载前做拓扑排序；卸载/禁用**级联**处理反向必选依赖（`collectReverseRequiredDeps`），启用时按需恢复被级联禁用的插件。
- opencode：没有插件级依赖声明；顺序由 boot 列表的显式次序 + 同域内注册顺序决定；同 id 替换保留原顺序位；批量启动用 `State.batch` 把「多次重建」压成「每域一次」。
- 结论：agentxx 有依赖模型（更强），opencode 有**顺序与重建合并**的明确语义（更省开销）；两者可以互补。

**5) 线程与并发**
- agentxx：框架假定「宿主 IO 线程是唯一状态所有者」。插件可从任意线程调用 ABI（入口内部投递并同步等待），注册类入口在真正执行前复查实例状态；插件侧另有协作式取消（`CancelRegistry` + 驱动 `drive_once` 回调，明确「不得阻塞、不得等待事件、不得同步调用宿主业务接口」）与 `offload` 卸载。
- opencode：一切在 Effect 运行时里，并发单位是 fiber；插件 hook 串行执行、await 即让出；中断语义由 fiber 中断承担。
- 结论：两者都在「单线程执行模型 + 明确的让出点」上收敛；agentxx 把约束写成了 ABI 注释（对插件作者更重要），opencode 把约束交给了运行时。

**6) 卸载与关闭安全（本维度 agentxx 明显更细）**
- agentxx：admission lease（`InflightGuard`）覆盖「已进入代码」与「已排队未执行」；`waitInflightZero` 事件式等待；同步关闭遇 lease 未归零时 `deferInstanceShutdown` + idle 回调收尾；宿主控制块 tombstone 让旧 `host` 指针安全失败；驱动句柄表带墓碑窗口（长期运行不增长）；`stop` 未完成时置 `CloseFailed` 并要求走 `shutdownAsync`；`hasPendingClose()` 供宿主在停 executor 前自检。
- opencode：关闭 Scope 即中断所属 fiber、跑 finalizer；`packages/core` 侧另有 `CONTEXT.md` 记录的「内嵌宿主的作用域必须覆盖流式响应体结束」这类边界要求。
- 结论：agentxx 在「插件二进制必须安全卸载」这个约束下做出了一整套机制，opencode 因为不需要 `dlclose` 而天然简单 —— 这部分**不要照搬 opencode 的做法**。

**7) 隔离与信任**
- agentxx：ABI 边界 + 能力协商 + 权限声明 + 符号白名单 + 静态链接工具库（版本可共存）+ 平台矩阵；插件的能力来自「表」而不是「内部对象」。
- opencode：插件与宿主同权限（能改系统提示词、能替换 SDK 语言模型、能读宿主服务），隔离靠「同一个仓库/同一套信任」，分发可控性弱。
- 结论：面向第三方分发的 agentxx 必须保留自己的边界。

**8) 装配与替换（测试/嵌入）**
- agentxx：依赖注入容器 + 插件宿主显式装配；插件的启用/禁用/卸载有完整路径，但没有「按服务替换实现」的通用机制。
- opencode：`LayerNode` 的 `replacements` 支持在编译期把某个服务节点换成另一个实现（tag 与错误类型在类型层校验），测试与嵌入模式都靠它；`hoist` 把指定 tag 的服务提升到上层作用域（全局 vs Location）。
- 结论：**可借鉴**：给 agentxx 的宿主装配加一层「按名字替换服务实现」的接缝，便于测试替身与嵌入场景（P2，落地在 `deps/injector.h` 与测试装配处）。

**9) 热更新语义**
- agentxx：禁用/启用会重建 start 事务（`stopForDisable`/`startForEnable`），卸载级联；注册表在禁用期不残留；`loadPluginAsync` 支持重复加载。
- opencode：`add` 同 id 即替换（旧插件先禁用，新插件按原顺序位装载），变换集合变化只重建受影响的域；models.dev 刷新、配置文件变化都通过「重跑相关域」表达。
- 结论：opencode 的「同 id 替换保留顺序位 + 只重建受影响的域」值得借鉴（M50 的细化）。

**10) 可观测性与诊断**
- agentxx：`list()` / `listPluginsJson()` / `getPluginJson()`（含接口列表、能力声明、依赖、状态），日志按插件名前缀；关闭异常有明确日志与状态（`CloseFailed`、lease 计数、延迟收尾原因）。
- opencode：插件装载发布 `Plugin.Event.Added`；TUI 槽位注册表按「插件/槽/阶段」上报错误；失败状态可 `wait` 查询。
- 结论：agentxx 的诊断信息更全（明文状态 + JSON 视图），opencode 的错误隔离更细（单个插件出错不影响其它插件渲染）。

### 15.6 可迁移到 agentxx 的设计

1. **[P1] 声明式贡献 + 重算**：对插件「声明类」贡献（记忆文件列表、技能目录、工具权限、提示词追加段、上下文来源、面板/渲染器）建立统一注册表：注册即进入「当前活动集合」，注销即离开；派生状态（系统提示词、工具清单、UI 描述）由集合重算并推送，而不是让插件手工撤销每一处。落地位置：`agent/lib/src/plugins/plugin_manager_domain_hooks.cpp` + 各资源应用点（`resource_applier.cpp`、`prompt.cpp`、`builtin_tool_renderers.cpp`）。
2. **[P1] 细粒度变更事件**：插件启用/禁用/资源变更时，发布「哪个域变了」（工具集变了、提示词段变了、模型表变了），让 UI 与依赖方只做必要的重算与重绘（现在多为整体刷新）。落地位置：`plugin_manager_lifecycle.cpp` + `events.h`。
3. **[P2] 插件作用域化的状态存储**：把「插件级状态」显式挂在插件实例上下文中（现在靠 `plugin_ctx` 与约定），并在禁用时统一清理（可参考 Scope 的「一个容器持有全部注册句柄」）。落地位置：`plugin_framework.h` / `plugin_guard.h`。
4. **[P2] 反向：能力边界不要放弃**：opencode 的 v2 hook 可以任意改系统提示词、任意改 SDK 语言模型，代价是插件拥有与宿主相同的权限。agentxx 应继续保留「表 + 能力协商 + 权限声明」的边界，新增能力时优先扩展表而不是开放内部结构（§19）。
5. **[P2] 文档同步**：把表数量（20/10）、新表（`agentxx.agent.context`）同步进 `AGENTS.md` 与 `docs/zh-cn/design/plugins.md`（当前记的 19/9 与实测不符）。
6. **[P1] 顺序语义显式化（借鉴 M50 的细化）**：把「同 id 重载保留原顺序位」与「批量启用/禁用时受影响的域只重算一次」写进 `PluginManager` 的启停实现：现在重复加载/启用走的是「先停再起」，顺序由加载顺序决定；建议记录「顺序位」并在替换时复用，同时把批量启停的派生重算合并到一批结束后执行一次。落地位置：`plugin_manager_lifecycle.cpp`、`plugin_manager_adapters.cpp`。
7. **[P2] 按名字替换服务实现（测试/嵌入接缝）**：给宿主装配加一层「服务名 → 替代实现」的接缝（opencode 用 `LayerNode.replacements` 在类型层做同样的事），便于测试替身、嵌入宿主与未来多宿主变体。落地位置：`deps/injector.h` + `base_agent.cpp` 的装配段。
8. **[P2] 插件错误隔离的展示面**：TUI 侧借鉴 opencode 的槽位注册表：按「插件 / 槽位 / 阶段」记录渲染错误并只影响该处显示（现在是插件渲染抛错后回退，缺少可查询的错误记录）。落地位置：`client_plugin_manager.cpp` 的 UI 注册表 + `builtin_tool_renderers.cpp`。
9. **[P2] 域视图的对外查询**：opencode 的 `PluginHost` 只暴露域编辑器与重载；agentxx 可给调试/工具链加一个只读的「域视图」（当前提示词段、工具清单、图节点类型、能力、订阅、依赖），把上面第 10 条列的诊断信息做成可查询的 JSON 接口。落地位置：`agentxx.agent.core` 表的查询方法（已有 `getPluginJson` 可在其上扩展）。

---

## 16. 测试与质量门禁

### 16.1 agentxx 现状

- **自研测试runner**：`agentxx_test`，本次实测 `test.cpp` 注册 **68 个模块**（41 个同步 + 27 个异步）；支持 `--fail-fast`、按模块名筛选。
  - 同步模块：工具函数（string/regex/json/json_view/json_reflection/diff/aho_corasick/util_misc）、事件与并发（events/concurrency/misc_fixes）、配置（config_loader/settings_db）、UI（ui_items/interrupt_ui + 大量 `tui_*`：input/scroll/lazy_view/sidebar/form/surface/theme/tool_header/widget/stream/context_overlay/markdown_block/markdown_flow/mermaid_state/ftxui_text）、插件（plugin_runtime/plugin_sdk/plugin_bridge）、FFI、training、sessionId 等；
  - 异步模块：agent/agent_host/event_bridge/event_stream/interrupt_bus/cancel/checkpoint_store/session_persistence/summarization/subagent_bus/subagent_tool/worktree/share_store/message_supplement/memgrowth/mcp/a2a/acp/remote_agent/websocket/http/openai_provider/anthropic_provider/network_timeout/plugins/client_plugins/plugin_resources。
- **资源基准**：`agentxx_benchmark`（release）覆盖同进程 CLI/TUI、真实两进程、真实 TUI（FTXUI 界面线程帧耗时）、真实 server 单独运行、PTY 驱动 TUI、插件逐项边际内存；指标含 RSS/PSS/私有脏页/匿名/峰值/线程/fd、glibc 堆碎片、smaps 模块级分解、逻辑内存、分阶段增量、CPU 时间；报告输出 `.json`（机器对比，支持 `--baseline` 差分）+ `.md`（人工阅读）。
- **缺口**：没有 HTTP 录制回放（provider/网络测试依赖真实或模拟服务）、没有「生成产物一致性」类校验、没有导入边界检查、UI 侧没有快照测试。

### 16.2 opencode 现状

- **规模**：682 个 `.test.ts` / 15.06 万行（本仓库实测）；每个包独立 `test` + `typecheck` 脚本（`bun test` / `tsgo`），根目录明确禁止跑测试（守卫 `do-not-run-tests-from-root`）。
- **纪律**（根 `AGENTS.md`）：尽量不用 mock（除万不得已不用 `globalThis.*`）；测试真实实现，不把逻辑抄进测试。
- **录制回放**：`packages/http-recorder`（Effect HTTP 客户端流量的确定性录制/回放）+ 固定装置文件（如 `core/test/fixtures/recordings/session-runner/openai-chat-streams-text.json`），使 provider 与网络相关测试可离线复现。
- **生成物一致性**：`packages/client` 的 `check:generated` = 重新生成 + `git diff --exit-code`（生成物与源码不一致即失败）；`httpapi-codegen` 自带编译期测试（用合成的 HttpApi 固定装置作为可执行规格）。
- **边界与兼容测试**：`CONTEXT.md` 要求「用导入边界测试保住浏览器安全的 client bundle」；`legacy-event-schema.test.ts`、`shared-schema.test.ts` 等守 schema 兼容；`database-migration.test.ts`（32 KB）守数据迁移。
- **前端**：`packages/app` 有单元测试（happy-dom）、浏览器测试、Playwright e2e、性能/视觉稳定性基准（timeline-stability）、Storybook（`packages/storybook`）。
- **文档即规格**：`specs/v2/*.md` 常带状态表（如 V1 对齐清单逐条 `complete/partial/missing`），`session/runner/llm.ts` 顶部用复选框列出已实现/未实现，使「哪些没做」有单一出处。

### 16.3 对比

| 维度 | agentxx | opencode | 评价 |
|---|---|---|---|
| 测试规模 | 68 模块 / 7.32 万行测试代码 | 682 文件 / 15.06 万行 | 规模相当（agentxx 模块粒度大） |
| 模块化筛选 | 支持按模块名运行、fail-fast | 按包/文件运行 | 两者都够用 |
| 依赖确定性 | provider/网络测试需真实或本地模拟 | HTTP 录制回放 + 固定装置 | **可借鉴**（收益直接：provider/协议回归可离线、可复现） |
| 生成物纪律 | 无生成物 | 生成 + CI 差异校验 + 归属清单 | 若引入协议生成（§14）应同时引入该纪律 |
| 边界测试 | 无 | 导入边界测试 + bundle 安全性 | 可借鉴（§1.5） |
| 兼容/迁移测试 | 有 `test_session_persistence`（会话库读写） | 有 `database-migration.test`、`legacy-event-schema.test` | 可借鉴：给持久化格式变更配「旧数据可读」测试 |
| 资源与性能 | 强项：模块级内存分解 + 基线差分 + 真实 TUI 帧耗时 | 前端有可视化稳定性/性能基准（浏览器侧） | **agentxx 更强**（服务端资源基准），可反向输出 |
| 未完成项可见性 | 分散在 TODO 与文档 | 规格文件内逐条状态表 + 源码顶部复选框 | 可借鉴（成本极低）：把「未实现项」收敛到单一清单 |
| UI 测试 | `tui_*` 模块（行为级） | 故事化 + 快照 + e2e | 可互补 |

### 16.4 两者的优缺点

**agentxx**
- 优点：68 个模块划分清楚（同步 41 + 异步 27），支持按模块运行与 `--fail-fast`；异步模块覆盖真实链路（中断总线、取消、会话持久化、子代理总线、MCP/A2A/ACP、WebSocket、重连、内存增长）；资源基准体系完整（模块级 smaps 分解、基线差分、真实 TUI 帧耗时、PTY 驱动、插件边际内存），并有专门的 benchmark 文档维护口径。
- 缺点：没有录制回放，provider/网络相关测试依赖真实或本地模拟服务，回归不够确定；没有生成物一致性校验（因为当前没有生成物）；没有导入边界测试（依赖方向只靠文档）；没有「旧数据 → 新版本可读」的持久化兼容用例；没有 UI 组件快照测试；未实现项的清单散落在 TODO 与设计文档里，没有单一出处。

**opencode**
- 优点：682 个测试文件覆盖到协议/组件层；HTTP 录制回放（`http-recorder` + 固定装置）让 provider 与网络行为可离线复现；生成物「重新生成 + `git diff --exit-code`」的 CI 校验保证生成与源一致；有导入边界与浏览器 bundle 安全测试；有数据迁移测试（32 KB）与 schema 兼容测试；测试纪律明文（尽量不用 mock、测真实实现）；文档即规格（状态表逐条标记 `complete/partial/missing`），「哪里没做」有单一出处。
- 缺点：规模大、依赖 Bun workspace 与 catalog 版本管理，跨包运行的准备成本高（根目录禁止跑测试即是这种耦合的信号）；前端测试依赖 happy-dom/Playwright，环境依赖重；没有服务端的资源基准（RSS/堆碎片/模块级分解这类能力在 agentxx 那边更成熟）；生成物入库会让每次改协议的 diff 变大。

### 16.5 可迁移到 agentxx 的设计

1. **[P0] HTTP 录制回放**：为 provider 与网络工具加一层可录制/回放层（录制模式下保存请求-响应对到固定装置文件，回放模式下按顺序/哈希匹配返回），用于：
   - 协议适配回归测试（OpenAI/Anthropic/Responses 的请求体与流式解析）；
   - 无需真实 key 的端到端会话测试。
   落地位置：新增 `agent/test/http_recorder`（或在 `agent/lib/src/protocol` 抽一层可注入的传输），固定装置放 `agent/test/fixtures/recordings/`。
2. **[P0] 「未实现项」单一清单**：在 `docs/zh-cn/design/index.md` 或新建 `docs/zh-cn/design/roadmap.md` 里维护「已实现/部分实现/未实现」状态表（含对应代码位置），并把散落的 `// TODO` 归拢引用；对每个模块给出验收标准。落地位置：文档 + 节点源码注释引用。
3. **[P1] 持久化兼容测试**：为会话库/设置库/共享存储各写「旧版本数据 → 新版本读取」的用例（构造历史 schema 的库文件），保证格式演进不破坏已有用户数据。落地位置：`agent/test/core/test_session_persistence.h`、`test_settings_db.h`。
4. **[P1] 边界测试**（与 §1.5 同一条）：断言 client/插件不依赖 lib 私有头、插件导出符号白名单。
5. **[P2] UI 快照测试**：`agentxx.ui.item` 的组件 → 渲染结果快照（含窄终端/CJK/超长），替代纯行为断言。落地位置：`agent/test/client/test_tui_ui_items.h`。
6. **[P2] 生成物一致性校验**（若引入 §14 的协议生成）：生成脚本 + 校验脚本纳入构建，产物入库。

---

## 17. 配置、模型目录与装配

### 17.1 agentxx 现状

- **配置分层**：overlay（工作目录或 `--config` 指定的 yaml + 同目录 `.env`）覆盖 base（overlay 的 `data_dir` 下的 `agentxx-config.yaml`/`.env`；未配置时取 `~/.agentxx/`）。
- **结构与合并语义**：`models` → `model.list`、`plugins` → `plugin.list`、`use_model` → `model.use`；列表段统一 `{overwrite: {mode: merge|replace, remove: [...]}, list: [...]}`（纯列表写法已废弃）；旧键/旧写法**告警并忽略**。
- **设置 KV**：`agentxx/util/settings_db.h` 提供全局设置 KV（sqlite），TUI 设置弹窗读写它（如 `tui.checkUpdateOnStartup`）。
- **模型来源**：yaml 的模型段（id/provider/baseUrl/key/能力/参数），无远程目录、无价格/窗口元数据来源。
- **鉴权**：API Key（env/yaml）。
- **插件来源**：yaml `plugin.list`（路径 + 参数）或构建期内置（`AGENTXX_PLUGIN_BUILTIN_LIST`，默认不内置）。
- **工作目录与数据目录**：`data_dir` 决定会话库/设置/插件数据位置；未配置 `dataDir` 且未指定会话目录时不持久化（仅内存，启动时警告）。

### 17.2 opencode 现状

- **分层配置**：全局/项目/环境变量/CLI 多层文档合并；配置模块按域自导出（`src/config` 的 `export * as ConfigAgent from "./agent"` 这类自导出风格，根 `AGENTS.md` 明确要求）。
- **数据迁移**：`data-migration.sql.ts` + `v1/config/migrate.ts`（240 行）+ `database-migration.test.ts`（32 KB）：老配置与老库在新版本启动时显式迁移，而不是「忽略未知键」。
- **模型目录（catalog）**：
  - `catalog.ts`（269 行）+ `model.ts` + `provider.ts`（schema 97/65 行）：provider 与模型的领域记录、默认模型、变体（variant）；
  - `models-dev.ts`（238 行）+ `plugin/models-dev.ts`：从 models.dev 拉取远程模型数据（上下文窗口、单价、模态等），以**定时刷新 + 本地缓存**的方式成为「配置 transform」的一个来源；
  - `specs/v2/provider-model.md` 把「生成控制」「协议语义的请求选项」「兼容性请求体字段」明确分区，并规定由统一摄取适配器把 models.dev/AI-SDK 形态的选项分类路由。
- **鉴权与集成**：`credential.ts`（凭据）+ `integration.ts`（476 行：连接方式、授权方法 env/key/oauth、连接解析）+ `oauth/`（含 OAuth 页面）+ `account.ts`；`account` 切换会触发对应 provider 的 catalog 刷新（`Account.switched → AuthPlugin.refresh → transform → Catalog.rebuild`）。
- **策略**：`policy.ts`（如允许/拒绝的 provider 选择）在 catalog 重算时最后一步应用。
- **插件来源**：npm/git/配置目录，安装与升级是后台任务（不阻塞位置就绪），完成后触发一次合并的重载。

### 17.3 对比

| 维度 | agentxx | opencode | 评价 |
|---|---|---|---|
| 分层合并 | 两段 yaml + 列表段 merge/replace/remove（语义明确） | 多源文档合并 + 按域自导出 | agentxx 的「列表段覆盖语义」更精细（可 remove 单项）；opencode 的来源更多 |
| 版本迁移 | 旧键告警并忽略 | 显式迁移脚本 + 迁移测试 | **可借鉴**：告警忽略会让用户以为生效了 |
| 模型元数据 | yaml 手填（无窗口/价格/模态来源） | 远程 models.dev + 定时刷新 + 本地缓存，可被插件 transform 覆盖 | **可借鉴**：模型窗口/价格/模态是预算、UI、成本统计的基础 |
| 鉴权 | API Key | API Key + OAuth（含刷新、账户切换）+ 凭据与集成分离 | **可借鉴**（如 GitHub Copilot / OpenRouter 这类 OAuth 场景） |
| 策略 | 分散（权限模式、插件启用、模型限制） | `policy.ts` 在 catalog 重算末尾统一应用 | 部分可借鉴 |
| 插件装配 | yaml 列表 + 构建期内置 | npm/git/目录 + 后台安装 + 完成后合并重载 | 两者差异源于分发形态；agentxx 的「内置列表可选合并」更简单可控 |
| 设置存储 | 全局 KV（sqlite）+ 配置 yaml | 配置文档 + 数据库（会话/凭据/策略） | 各有分工；agentxx 的 KV 与 yaml 的边界需在文档里写清（哪些属于「设置」哪些属于「配置」） |

### 17.4 两者的优缺点

**agentxx**
- 优点：base/overlay 两段配置 + `.env` 就近加载，规则简单可预期；列表段的 `overwrite{mode: merge|replace, remove}` 语义精细（可以按身份剔除 base 项，比「整段替换」更省事）；设置 KV 与会话/插件数据同库，落地一致；未配置 `data_dir` 时明确「不持久化」并告警，不会到处生成垃圾目录；模型、插件、MCP、路径列表统一了一套覆盖规则。
- 缺点：旧键/旧写法只告警并忽略（用户可能以为已生效），没有版本号与迁移函数；模型元数据（窗口/最大输出/价格/模态）没有来源，只能手填；鉴权只有 API Key；允许哪些 provider/模型的策略分散在权限模式、插件启用、模型段里；「设置」与「配置」的边界（谁能覆盖谁、哪份进版本库）没有文档化。

**opencode**
- 优点：多层配置文档合并 + 显式的数据迁移脚本与迁移测试；模型目录（`catalog` + `models.dev`）带远程刷新与本地缓存，并可被插件 transform 覆盖，模型元数据（窗口/输出/模态/价格）成为一等字段；凭据与集成分离（env/key/oauth 多种方式、账户切换触发 catalog 重算）；policy 在 catalog 重算的最后一步统一应用；插件从 npm/git/目录后台安装，不阻塞位置就绪。
- 缺点：来源多（全局/项目/env/CLI/models.dev/插件 transform）导致「这个值从哪来」需要专门的排查手段；配置 transform 可改任意字段，因此任何 transform 变化都可能触发一次全服务重算（规格自述的取舍）；OAuth/集成体系实现量大、维护成本高（对本项目规模而言偏重）；配置形状与旧版本兼容需要持续的迁移维护。

### 17.5 可迁移到 agentxx 的设计

1. **[P1] 配置显式迁移**：引入配置版本号 + 迁移函数（读到旧版本 → 转换 → 写回或内存内转换，并记录日志），把现在「告警并忽略旧键」的行为升级为「迁移或明确报错」。落地位置：`agent/lib/src/agent/config.cpp`（yaml 解析段）+ 文档。
2. **[P1] 模型元数据来源化**：模型条目补充并允许外部来源：
   - 上下文窗口与最大输出（预算与压缩用）；
   - 价格（token 用量与费用统计）；
   - 模态与能力（附件按钮、图片/推理支持）；
   - 来源优先级：内置默认 < 配置文件 < 远程目录（可选，带本地缓存与定时刷新）< 插件补充。
   落地位置：`agent/lib/include/agentxx/agent/model_registry.h` + yaml 模型段 + 可选的新插件负责远程目录。
3. **[P1] 鉴权方式分层**：把「API Key」抽象为「凭据来源」之一（env / 配置 / 凭据库 / OAuth 刷新），为将来支持 OAuth 类 provider（如 GitHub Copilot）留出接口；凭据与 provider 定义分离（凭据可复用、可撤销、可多账户）。落地位置：`provider_common.h` + `model_registry.h`。
4. **[P2] 策略集中**：把「允许使用哪些 provider/模型」「允许哪些工具类别」这类策略从多处配置收敛到一个显式策略层，在模型目录构建的末尾统一应用。落地位置：`AgentConfig` 的模型段 + 权限中间件的规则装配。
5. **[P2] 设置与配置的边界文档**：明确「yaml 配置（可版本化、可分发）」与「设置 KV（本机偏好）」的分工与优先级，避免同类项两处可配。落地位置：`docs/zh-cn/design/index.md` 配置章。

---

## 18. 迁移建议汇总

优先级判据：**P0** = 直接解决现有明确缺陷或带来数量级收益，且有清晰落地位置；**P1** = 收益明确、需要少量设计；**P2** = 依赖前置条件或属于长期演进。
「依赖」列写明必须先落地的项（同组内可一起做）。

### 18.1 A 组：会话可恢复性与输入投递（对应 §2、§5、§14）

| 编号 | 迁移项 | 落地位置 | 依赖 | 优先级 |
|---|---|---|---|---|
| M1 | durable 收件箱：待执行输入落库，`admitted_seq`/`promoted_seq` 两段状态 | `session_server_agent_io.cpp`、`session_store.cpp` | — | P0 |
| M2 | `steer` / `queue` 投递语义（当前轮边界提升 / 空转时提升） | `wire_protocol.h`、`agent_runner.cpp` | M1 | P0 |
| M3 | 会话事件序列（`event` 表 + 聚合内 `seq`）与 `after` 游标重放 | `session_store.cpp`、`session_server_agent_io.cpp` | — | P0 |
| M4 | durable 事件流与实时增量流分离（重连恢复不再依赖内存缓冲） | `wire_protocol.h`、`session_server_agent_io.cpp` | M3 | P0 |
| M5 | 完成事件边界：压缩/工具/上下文重建用 started/ended 表示 | `summarization.cpp`、`toolcall.cpp` | M3 | P1 |
| M6 | 启动清账：把上次进程遗留的「执行中」工具判为被中断 | `base_agent.cpp`（会话初始化） | — | P1 |
| M7 | 提示词提交幂等（可选 `id`，同 id 不同内容报冲突） | `wire_protocol.h` | M1 | P1 |
| M8 | 持久化格式版本号 + 读取分派 | `session_store.cpp`、`settings_db.h` | M3 | P1 |
| M9 | 持久化降级对用户可见（推送到 UI，而不是只写日志） | `session_store.cpp`、`session_server_agent_io.cpp` | M4 | P1 |
| M10 | 重放所有权（`owner_id` 校验，暂不做分布式） | `session_store.cpp` | M3 | P2 |

### 18.2 B 组：上下文、预算与 LLM 层（对应 §3、§7、§8、§9）

| 编号 | 迁移项 | 落地位置 | 依赖 | 优先级 |
|---|---|---|---|---|
| M11 | 上下文来源化 + 基线/增量分离（动态内容改为追加式系统消息） | `prompt.cpp`、`agentcall.cpp`、`session_store.cpp` | M3（基线持久化） | P0 |
| M12 | prompt 缓存断点（Anthropic 显式 / OpenAI 前缀稳定） | `anthropic_provider.cpp`、`prompt.cpp` | M11 | P0 |
| M13 | 结构化摘要模板 + 多次压缩滚动合并 | `summarization.cpp` | — | P0 |
| M14 | 压缩保留尾部原文窗口（token 预算可配） | `summarization.cpp`、`config.h` | — | P0 |
| M15 | provider 错误分类 + 溢出一次性压缩重试 | `provider_common.h`、`modelcall.cpp`、`summarization.cpp` | M13 | P0 |
| M73 | 重试策略按状态码/响应头驱动并加抖动（替代关键字匹配 + 固定退避） | `modelcall.cpp`、`provider_common.h` | M15 | P1 |
| M74 | 会话用量账本（cost / token，含 cache read·write，可随消息删除回退） | `session_store.cpp`、`modelcall.cpp` | M3 | P1 |
| M16 | 压缩预算口径包含工具定义（buffer 与输出上限取大者） | `summarization.cpp` | — | P1 |
| M17 | 压缩完成边界事件化（与 UI 展示、重放一致） | `summarization.cpp`、`session_store.cpp` | M3、M5 | P1 |
| M18 | 压缩完成 / 会话工作上下文切换后重建基线 | `summarization.cpp`、worktree 绑定路径 | M11 | P1 |
| M19 | 上下文变化只在安全边界采样（不做异步推送改上下文） | `modelcall.cpp` | M11 | P1 |
| M20 | 指令集合变更通知（含取代与撤销语义） | `memory_file.h`、`skill.h`、`prompt.cpp` | M11 | P1 |
| M21 | 确定性旧工具结果裁剪（先于整体压缩） | `toolcall.cpp` | M5 | P2 |
| M22 | 对外暴露当前上下文来源与快照 | `plugin_manager_vtable.cpp`（`agentxx.agent.context`） | M11 | P2 |

### 18.3 C 组：工具结算、结果保真与交互（对应 §6、§12）

| 编号 | 迁移项 | 落地位置 | 依赖 | 优先级 |
|---|---|---|---|---|
| M23 | 同轮工具并行执行 + 按 `tool_call_id` 归位结算 | `toolcall.cpp`（`// TODO: 真正并行`） | — | P0 |
| M24 | 大输出落盘保真（完整文本 + 头尾预览 + 路径 + 过期清理） | 新 `tool-output` 模块、`toolcall.cpp` | — | P0 |
| M25 | 公告身份防陈旧调用（定义指纹比对，不一致判 stale） | `modelcall.cpp`、`toolcall.cpp` | — | P1 |
| M26 | 结构化结果与模型可见投影分离 | `tools/tool.h`、`toolcall.cpp` | M24 | P1 |
| M27 | 模型发起的结构化提问工具（问题/选项/多选/自定义答案） | `interrupt_ui.cpp` + 新工具 | — | P1 |
| M28 | 每条 assistant 消息记录本次改动的文件清单 | `session_store.cpp`、`permission.cpp` | M3 | P1 |
| M29 | 「取消/中断不是工具结果」的统一断言 | `toolcall.cpp` | M23 | P2 |
| M30 | git 快照与消息级回退（stage/clear/commit 三态） | 新宿主能力 + TUI 入口 | M28 | P2 |
| M31 | PTY 会话（交互式终端） | 新模块 + wire 消息 | — | P2 |
| M75 | 陈旧调用 / 取消 / 中断三类收尾的一致性断言 | `toolcall.cpp`、`modelcall.cpp` | M23、M29 | P2 |

### 18.4 D 组：权限与安全（对应 §10、§7）

| 编号 | 迁移项 | 落地位置 | 依赖 | 优先级 |
|---|---|---|---|---|
| M32 | 批准记忆持久化 + 撤销入口（列表/删除/UI） | `permission.cpp`、`settings_db.h`、TUI 设置 | — | P0 |
| M33 | 拒绝/批准的连带处理（同会话待决请求一并结算） | `permission.cpp`（待决登记表） | M32 | P0 |
| M34 | 待决定请求可查询（重连后恢复询问界面） | `wire_protocol.h`、`session_server_agent_io.cpp` | M33 | P1 |
| M35 | 技能/工具按 agent 过滤可见性 | `code_agent.cpp`、`toolcall.cpp` | — | P1 |
| M36 | 权限动作通用化（保留路径语义为一类） | `permission.h` | — | P2 |
| M37 | 边界文档：硬边界 vs 提示性检查 | `docs/zh-cn/design/index.md` | — | P2 |
| M38 | 技能正文走权限语义（询问卡片显示技能名） | `skill.cpp` + 技能工具 | M35 | P2 |
| M39 | 指令发现的边界规则（规范化 + 不越出项目根 + 开关） | `prompt.cpp` | M20 | P2 |

### 18.5 E 组：客户端与协议（对应 §13、§14）

| 编号 | 迁移项 | 落地位置 | 依赖 | 优先级 |
|---|---|---|---|---|
| M40 | 渲染层边界检查（渲染不依赖网络/会话/插件管理器） | 新 `test/client/test_tui_boundary.h` | — | P1 |
| M41 | 未知内容宽容性条款化 + 「未来版本组件树」测试 | `docs/zh-cn/design/tui.md`、`test_ui_items.h` | — | P1 |
| M42 | markdown 解析/测量的 offload 与缓存 | `markdown_*`（client TUI） | — | P1 |
| M43 | 协议单一定义 + 生成 C++ 编解码/文档（可选语言绑定） | 新 wire schema + 生成脚本 | — | P1 |
| M44 | 不透明游标规范（会话列表/历史分页） | `wire_protocol.h` | M3 | P1 |
| M45 | 连接阶段与错误语义显式化（未握手/未绑定/就绪/重连中） | `ws_io_transport.h`、`session_server_agent_io.cpp` | — | P1 |
| M46 | 组件快照测试（窄终端/CJK/超长） | `test/client/` | M41 | P2 |
| M47 | 主题/键位数据化与校验 | client TUI 主题/键位模块 | — | P2 |
| M48 | 开放 SDK（先做一种语言的轻量绑定，内嵌与远程同 API） | 新 `sdk/` + 生成 | M43 | P2 |

### 18.6 F 组：扩展、测试与配置（对应 §11、§15、§16、§17、§1、§4）

| 编号 | 迁移项 | 落地位置 | 依赖 | 优先级 |
|---|---|---|---|---|
| M49 | 声明式贡献 + 重算（提示词段/工具/资源/UI 描述统一注册表） | `plugin_manager_domain_hooks.cpp`、`prompt.cpp` | — | P1 |
| M50 | 细粒度变更事件（哪个域变了 → 只重算依赖方） | `plugin_manager_lifecycle.cpp`、`events.h` | M49 | P1 |
| M51 | HTTP 录制回放（固定装置 + 回放） | 新 `test/http_recorder`、`agent/test/fixtures/` | — | P1 |
| M52 | 「已实现/部分/未实现」单一清单与验收标准 | `docs/zh-cn/design/` | — | P1 |
| M53 | 持久化兼容测试（旧数据 → 新版本可读） | `test_session_persistence.h`、`test_settings_db.h` | M8 | P1 |
| M54 | 边界测试（依赖方向 + 插件导出符号白名单） | 新测试模块 + 构建脚本 | — | P1 |
| M55 | 配置显式迁移（版本号 + 迁移函数） | `config.cpp` | — | P1 |
| M56 | 模型元数据来源化（窗口/输出/价格/模态/缓存支持 + 可插拔来源） | `model_registry.h` + 可选插件 | — | P1 |
| M57 | 鉴权方式分层（API Key / 环境 / 凭据库 / OAuth） | `provider_common.h`、`model_registry.h` | M56 | P1 |
| M58 | 后台任务状态统一（id/类型/状态/输出/取消） | `agent_host.cpp` + 新表 | — | P1 |
| M59 | 子代理会话可选持久化 | `agent_host.cpp`、`session_store.cpp` | M58 | P1 |
| M60 | 父会话「运行中」状态只统计前台轮次 | `session_server_agent_io.cpp`、TUI | M58 | P1 |
| M61 | 插件作用域化状态容器（禁用即整体清理） | `plugin_framework.h` | M49 | P2 |
| M62 | 文档同步：表数量（20/10）与新表 | `AGENTS.md`、`docs/zh-cn/design/plugins.md` | — | P2 |
| M76 | 插件顺序位 + 批量启停的重算合并（同 id 重载保留顺序位） | `plugin_manager_lifecycle.cpp`、`plugin_manager_adapters.cpp` | M49、M50 | P1 |
| M77 | 按名字替换服务实现的装配接缝（测试替身/嵌入变体） | `deps/injector.h`、`base_agent.cpp` | — | P2 |
| M78 | 插件渲染错误按「插件/槽位/阶段」记录并可查询 | `client_plugin_manager.cpp`、`builtin_tool_renderers.cpp` | — | P2 |
| M79 | 只读「域视图」查询（提示词段/工具/图类型/能力/订阅/依赖） | `agentxx.agent.core` 表扩展 | M49 | P2 |
| M63 | 策略集中（provider/模型/工具类别的允许与拒绝） | `AgentConfig` + `permission.cpp` | M56 | P2 |
| M64 | 设置与配置的边界文档 | `docs/zh-cn/design/index.md` | — | P2 |
| M65 | 子代理结果「先完成、后提升」语义 | `agent_host.cpp`、`interrupt_bus` | M58 | P2 |
| M66 | 显式化「会话工作上下文」对象（工作目录 + 隔离边界） | `context.h`、`permission.cpp` | — | P2 |
| M67 | 会话移动语义（换工作目录/换 worktree 时派生态失效） | `session_server_agent_io.cpp` | M11、M66 | P2 |
| M68 | 工具显式声明使用哪个根（工作区/会话目录/worktree/临时） | `tools/tool.h`、`toolcall.cpp` | M66 | P2 |
| M69 | 远端工作区与 A2A 统一为「位置」 | `a2a_*`、`context.h` | M66 | P2 |
| M70 | 领域与策略的显式分层（策略集中到装配层） | `base_agent.cpp`、`code_agent.cpp` | M63 | P2 |
| M71 | 生成物一致性校验（若引入 M43） | 构建脚本 + CI | M43 | P2 |
| M72 | 受限编排（模型写受限脚本编排工具，需沙箱与限额） | 新插件 | M23、M24 | P2 |

### 18.7 建议的落地顺序（4 个批次）

1. **批次 1（可恢复性）**：M1、M2、M3、M4 → 让「输入、消息、重连」三个环节都有持久依据；配套 M54（边界测试）、M52（未实现清单）。
2. **批次 2（上下文与成本）**：M11、M12、M13、M14、M15 → 长会话的缓存命中、压缩质量与溢出恢复；配套 M8、M53（格式版本与兼容测试）。
3. **批次 3（工具与交互）**：M23、M24、M25、M26、M27 → 延迟、结果保真、陈旧调用与结构化提问；配套 M51（录制回放，让 provider/工具回归可控）。
4. **批次 4（权限、扩展、装配）**：M32、M33、M49、M50、M55、M56、M57 → 权限记忆、声明式贡献重算、配置迁移与模型元数据。其中 M73（重试策略）与 M74（用量账本）可与批次 2 并行，二者都只依赖已有的重试/模型调用路径。

---

## 19. 反向清单：agentxx 不必照搬的设计

对比文档容易变成「什么都学」，这里明确列出**agentxx 现有做法更合适或 opencode 做法不适合照搬**的部分：

1. **Effect 运行时服务容器与 Layer 图**：opencode 用 Effect 的 `Context.Service` + Layer + fiber 表达一切（服务依赖、生命周期、并发、取消、重试）。表达力强但心智负担大，且与 TS 运行时特性深度绑定。agentxx 的「显式依赖注入容器 + 单 io_context 协程 + 自己写的取消令牌」在 C++ 里更直观、可预测，且没有运行时调度黑箱。**不照搬**。
2. **整库事件溯源**：opencode 只把会话相关关键事实事件化，没有把一切做成事件溯源（甚至明确「投影不参与重放」）。agentxx 应同样克制：只把「重连恢复、崩溃判断、审计」需要的事实事件化（M3/M5），**不要**把工具执行、UI 状态、插件状态全部事件化。
3. **双轨运行时的过渡态**：opencode 现在同时存在旧单体（16.3 万行）与新核心（6.2 万行），并通过 shadow bridge 把旧路径的消息补发成新事件。这是可行的迁移手段，但代价是两套心智模型长期共存、测试与文档都要标注「哪一代」。agentxx 若做同类重构，应尽量做到**单轨切换**（一次性替换 + 兼容层尽量薄、尽快删）。**不以双轨为目标**。
4. **多包 monorepo + 代码生成链**：opencode 有 32 个包、目录 cata 版本管理、`tsgo` 类型检查、生成物入库 + CI 校验。agentxx 是单 superbuild（CMake）+ 三个独立自研工具库；包边界靠目录与头文件表达即可。**只在「协议生成」（M43）这一处引入生成物纪律**，不要把整个构建改成多包。
5. **进程内插件同权限模型**：opencode v2 hook 能任意改系统提示词、替换 SDK 语言模型、读写配置；这对「同一团队维护的可信扩展」很高效，但没有能力边界。agentxx 面向第三方分发，**应保留 C ABI + 表 + 能力协商 + 权限声明**；不要为了「hook 更灵活」开放内部结构。
6. **bash 不设沙箱**：opencode 明确不做沙箱（靠容器/宿主权限），并把「绝对路径扫描」限定为提示性警告。agentxx 在权限层对写路径做了硬边界（含 worktree 隔离与配置拒绝路径），这对桌面端用户是实实在在的保护，**不要因为「沙箱不彻底就干脆不做」而放弃**。正确做法是把「哪些是硬边界、哪些只是提示」写清楚（M37）。
7. **codemode 式受限脚本解释器**：功能上很有吸引力（一次调用编排几十个工具），但需要解释器、限额、诊断与权限沿用一整套设计，且会显著扩大攻击面。agentxx 有 JS 引擎插件作为基础，但**只在出现明确需求（大量小调用导致的往返开销）时再做**（M72，P2）。
8. **会话/历史/事件的三层读模型**：opencode 的「事件 → 投影消息 → 重放流」带来极强一致性，但也带来读模型与写入路径的双份复杂度。agentxx 的「会话即权威 + 展示历史 + 定向事件化」更省事，**只需补齐重连与崩溃所需的序列与事件，不要引入完整投影层**。
9. **不自动重连的事件流**：opencode 明确要求消费者自己刷新后重订阅（把重连策略留给上层）。这对多端 UI 是合理分工，但 agentxx 的 TUI/CLI 是终端用户场景，**保留自动重连 + 增量续传**（现在已有）更合适，只需把「恢复依据」从内存缓冲换成持久事件序列（M4）。
10. **依赖 TypeScript 生态的鉴权/模型目录**：models.dev 目录、OAuth 全家桶、npm 分发插件都建立在 JS 生态上。agentxx 可借鉴「元数据来源化、凭据分层」的思路（M56/M57），但**不必**照搬某个具体来源或注册中心，可以先支持本地文件 + 可选远程 URL。
11. **每个会话一个「事件订阅者流」与游标 API**：C++ 侧新增一套流式订阅会引入协程生命周期管理的复杂度。agentxx 用现有 delta 推送 + 定向补拉（`after` 拉缺失区间）即可满足 TUI/CLI，**不必**做成通用流订阅框架。

---

## 20. 结语

三条原则，贯穿上面所有迁移建议：

1. **把「隐式状态」变成「显式状态」**：opencode 最值得学的地方不是某个功能，而是它把几件本来就存在的事写清楚了 —— 排队输入是「已接受 / 已提升」两段状态，上下文变化是「基线 + 增量」，工具调用是「已公告身份 + 结算」，流是「可重放 / 实时」两类，插件贡献是「活动集合 + 重算」。agentxx 现在的对应实现都能跑，但这些状态只存在于代码的控制流里；把它们落成状态与事件，是这一批评分最高的改动（M1–M4、M11、M23、M24、M49）。
2. **保留已经做得更好的部分**：C ABI 插件与多实例规则、声明式权限目标与逐路径三态复核、worktree 写边界、中断即委派的子代理与深度/并发预算、资源基准与模块级内存分解、同进程与远程「只换 transport」的统一端点模型 —— 这些都是 opencode 没有或更弱的，迁移时必须显式保护，不要在新设计里把它们冲掉（§19 逐条列出）。
3. **迁移要有边界**：不引入第二套运行时、不引入完整事件溯源与投影层、不为「灵活性」放弃能力边界；每一项迁移都应能在现有结构里指出落地文件，并配有可验证的测试（§16 的 M51/M53/M54 就是给迁移本身准备的安全网）。

---

## 附录 A：关键文件与文档索引

| 模块 | agentxx | opencode |
|---|---|---|
| 总体设计 | `docs/zh-cn/design/index.md`、`AGENTS.md` | 根 `AGENTS.md`、`CONTEXT.md`、`specs/v2/*.md` |
| 会话运行 | `agent/lib/src/agent/base_agent.cpp`、`agent/lib/src/agent/agent_runner.cpp` | `packages/core/src/session/runner/{index,llm,model,publish-llm-event}.ts`、`session/{execution,run-coordinator,input}.ts` |
| 上下文 | `agent/lib/src/agent/context.cpp`、`agent/lib/src/agent/prompt.cpp`、`nodes/session_context.h` | `packages/core/src/system-context/{index,registry}.ts`、`session/{context-epoch,history,projector}.ts` |
| 压缩 | `agent/lib/src/middlewares/summarization.cpp` | `packages/core/src/session/compaction.ts` |
| 工具 | `agent/lib/include/agentxx/tools/tool.h`、`agent/lib/src/nodes/toolcall.cpp` | `packages/core/src/tool/{tool,registry,application-tools}.ts`、`tool-output-store.ts`、`specs/v2/tools.md` |
| 权限 | `agent/lib/src/middlewares/permission.cpp`、`agent/lib/include/agentxx/middlewares/permission.h` | `packages/core/src/permission.ts`、`permission/saved.ts` |
| LLM 层 | `agent/lib/src/protocol/{openai,anthropic}_provider.cpp`、`agent/lib/include/agentxx/agent/model_registry.h` | `packages/llm/src/**`、`packages/core/src/plugin/provider/*`、`packages/llm/src/cache-policy.ts` |
| 持久化 | `agent/lib/src/agent/session_store.cpp`、`agent/lib/include/agentxx/agent/checkpoint_store.h` | `packages/core/src/{event,database/schema.gen}.ts`、`session/sql.ts`、`session/projector.ts` |
| 子代理 | `agent/lib/src/agent/agent_host.cpp`、`agent/lib/src/tools/subagent.cpp` | `packages/core/src/background-job.ts`、`session.ts`（`parentID`） |
| 客户端 | `agent/client/include/agentxx-client/io/tui/ui_components.h`、`agent/lib/include/agentxx/ui/item.h` | `packages/tui/src/**`、`packages/session-ui/src/**`、`specs/tui-package.md` |
| 协议/SDK | `agent/lib/include/agentxx/agent/io/wire_protocol.h`、`agent/lib/src/agent/io/session_server_agent_io.cpp` | `packages/protocol/src/**`、`packages/server/src/**`、`packages/client/src/contract.ts`、`packages/httpapi-codegen/README.md` |
| 插件 | `agent/lib/include/agentxx/plugin/**`、`docs/zh-cn/design/plugins.md` | `packages/plugin/src/index.ts`、`packages/core/src/{plugin,plugin/host}.ts`、`packages/core/src/plugin/boot.ts`、`specs/v2/catalog-config-plugin-lifecycle.md` |
| 测试 | `agent/test/**`、`docs/zh-cn/design/benchmark.md` | 各包 `test/**`、`packages/http-recorder/**` |
| 配置 | `agentxx-config.yaml`、`agent/lib/src/agent/config.cpp`、`agent/lib/include/agentxx/util/settings_db.h` | `packages/core/src/config*.ts`、`v1/config/migrate.ts`、`catalog.ts`、`models-dev.ts`、`credential.ts`、`integration.ts` |

## 附录 B：术语对照

| 概念 | agentxx | opencode |
|---|---|---|
| 会话 | `Session`（上下文权威 + 展示历史） | Session（事件聚合）+ 投影消息 |
| 展示历史 | `viewMessages` | `session_message`（投影） |
| LLM 上下文 | 会话持有 typed `ChatMessage` | 基线 System Context + 投影历史 |
| 流式增量 | `Delta`（带 seq） | `LLMEvent`（durable 事件 + 实时片段） |
| 轮次 | `runTurnAsync` 一次运行 | Provider Turn / Session Drain |
| 输入排队 | 消息队列（内存） | `session_input`（durable 收件箱）+ `admitted/promoted` |
| 中断 | `NodeInterrupt` + `AgentRunner` 中断循环 | Effect fiber 中断 + 结算收尾 |
| 取消 | `CancelToken`（双通道） | fiber 中断 / `interrupt(sessionID)` |
| 工具定义 | `neograph::ChatTool` + `XXToolBase` | `Tool.make`（codec + 执行 + 投影） |
| 工具执行 | `ToolcallWrapNode`（串行） | `ToolRegistry.materialize/settle`（并行） |
| 权限询问 | 中断 UI 表单 | `Permission.ask/reply`（Asked/Replied 事件） |
| 记忆文件 | `MemoryFileMiddleware`（上下文文件） | Instruction Context Source（AGENTS.md） |
| 技能 | `SkillMiddleware` + 延迟加载 | Skill Guidance 来源 + `skill` 工具 |
| 子代理 | `AgentHost` + `AgentNode`（独立 agent） | 子会话（`parentID`） |
| 工作目录隔离 | worktree + `SessionFsIsolation` | Location + workspace adapter |
| 插件 | C ABI 动态库（表 + 能力协商） | v1 Promise hooks / v2 Effect transform |
| 客户端渲染 | `agentxx.ui.item` 组件树 | TUI 组件 / `session-ui` 组件 / 槽位 |
| 传输 | `AgentIOTransportBase`（Channel / WS） | `HttpClient`（网络 / 内存） |
| 设置 | `settings_db`（KV）+ yaml 配置 | 配置文档 + 数据库（凭据/策略/会话） |

## 附录 C：本次精读的源码与文档清单

统计口径：行数经 `Measure-Object -Line` 实测；文档大小按字节换算。凡是本文给出的数字，都可在下列位置复核。

**agentxx**
- 代码规模与热点文件：`agent/lib/{include,src}`、`agent/client/{include,src}`、`agent/plugins`、`agent/test`（递归统计，见 §0.1 表）。
- 结构清单：`agent/lib/include/agentxx/**`（73 个头文件全量列出，见 §1.1）、`agent/lib/src/**`（按行数排序前 45）。
- 测试模块：`agent/test/test.cpp`（`runSync(...)` 41 处 + `run(...)` 27 处）；异步模块名逐个导出。
- 插件表名：`agent/lib/include/agentxx/plugin/**` 内 `"agentxx.agent.*"`（10）、`"pluginxx.*"`（10）、`"agentxx.client.*"`（10）。
- 文档：`docs/zh-cn/design/index.md`（164 741 B，读「功能效果 / 会话持久化 / 会话切换 / 子代理 / 远程通信 / 协议支持 / 客户端 UI / 核心设计模式」各节）、`plugins.md`（102 057 B）、`tui.md`（42 534 B）、`benchmark.md`（43 309 B）、`ffi.md`（24 427 B）、根 `AGENTS.md`（38 555 B）。
- 源码细读：`agent/lib/include/agentxx/tools/tool.h`、`nodes/toolcall.h`、`middlewares/permission.h`、`middlewares/{memory_file,skill}.h`（全量）；`agent/lib/src/nodes/toolcall.cpp`（定位 `// TODO: 真正并行`、中断缓存、重复调用检测相关行）。
- 第 2 版新增精读：`agent/lib/src/nodes/modelcall.cpp` 的请求组装段（560–700 行，含 `repairMessages` 与 system 消息重建）、重试循环段（695–870 行，含退避、UI 提示、兜底消息插入）、`agent/lib/src/nodes/agentcall.cpp`（全量，每轮 `graphData` 清场与 `plugin.agentxx.round_start`）、`agent/lib/src/agent/context.cpp` 的 `buildSystemPrompt`（537–596 行，含 worktree 排除说明与占位符替换）、`agent/lib/src/middlewares/memory_file.cpp`（注入段 95–176 行）、`agent/lib/src/middlewares/summarization.cpp`（456–540 行，压缩模板、载荷裁剪、同上下文子代理）、跨仓 grep 确认「无上下文溢出识别」。
- 第 3 版新增精读（插件框架）：`agent/third_party/cxx_pluginxx/include/pluginxx/` 全部头文件（`api/{abi,entry,export,tables}.h`、`host/{lifecycle,host_core,tables_impl,manifest,event_bus,domain_hooks,capability_registry,loader}.h`、`runtime/{runtime,instance_base,manager_base,op_driver,driver}.h`、`kit/{kit,guard}.h`、`version.h`）与 `src/{capability_registry,loader,manifest,version}.cpp`；`agent/lib/include/agentxx/plugin/{plugin_manager.h,tool_registry.h,plugin_graph_node.h,api/plugin_kit.h}`；`agent/lib/src/plugins/{plugin_manager_lifecycle.cpp,client_plugin_manager.cpp}`（结构级）；`agent/plugins/agentxx_math/{CMakeLists.txt,plugin.yaml}`（插件工程模板）；opencode 侧：`packages/plugin/src/v2/effect/PLAN.md`（全量）、`v2/effect/README.md`、`v2/promise/README.md`、`packages/core/src/plugin/{internal.ts,promise.ts,host.ts}`（全量）、`packages/core/src/effect/layer-node.ts`（全量）、`packages/core/src/config/plugin/external.ts`（前 90 行）、`packages/core/src/plugin/provider/anthropic.ts`、`packages/core/src/plugin/variant.ts`、`packages/plugin/src/{tool.ts,tui.ts 前 120 行}`、`packages/tui/src/plugin/{slots.tsx,api.ts}`。

**opencode**
- 规模统计：`packages/*/src` 下 `.ts/.tsx`（排除 `src/generated`、`dist`）、`.test.ts` 全仓（排除 `node_modules`）。
- 第 2 版新增精读（逐行或大段）：`session/projector.ts`（全量）、`session/message-updater.ts`（全量）、`session/runner/to-llm-message.ts`（全量）、`session/runner/model.ts`（全量）、`session/runner/publish-llm-event.ts`（前 170 行）、`skill/guidance.ts`（全量）、`agent.ts`（全量）、`tool/bash.ts`（全量）、`tool/question.ts`、`tool/todowrite.ts`（全量）、`llm/src/route/executor.ts`（重试与超时相关行）、`llm/src/cache-policy.ts`（全量）。
- 包清单与职责：各 `packages/*/package.json`（含 `description`）；`packages/core/src` 目录与文件清单（含行数排序前 60）。
- 会话核心：`packages/core/src/{session.ts,session/input.ts,session/compaction.ts,session/context-epoch.ts,session/history.ts,session/store.ts,session/sql.ts,session/run-coordinator.ts,session/execution.ts,session/runner/{index,llm}.ts}`（全量或大段）。
- 上下文：`packages/core/src/system-context/index.ts`（全量）。
- 工具：`packages/core/src/tool/{tool,registry,application-tools,question,todowrite}.ts`（全量）、`tool-output-store.ts`（大段）。
- 权限：`packages/core/src/permission.ts`（全量）。
- 持久化：`packages/core/src/event.ts`（前 260 行）、`database/schema.gen.ts` 表清单、`session/sql.ts`（全量）。
- 快照/回退：`packages/core/src/snapshot.ts`（前 70 行）、`session/revert.ts`（全量）。
- 插件：`packages/plugin/src/index.ts`（全量）、`packages/core/src/plugin.ts`（全量）、`plugin/host.ts`（全量）、`plugin/provider/*`（文件清单，32 个）。
- LLM：`packages/llm/src/{index,llm,provider,tool-runtime,cache-policy}.ts`（全量）、`src/{protocols,providers,route}`（文件清单与行数）。
- 协议/SDK：`packages/protocol/src/api.ts`、`packages/client/src/contract.ts`、`packages/httpapi-codegen/README.md`（全量）、`packages/{schema,protocol,server,client,tui,session-ui,sdk-next}/src`（文件清单与行数）。
- UI：`packages/tui/src`、`packages/session-ui/src`、`packages/app/package.json`（依赖与脚本）。
- 文档：`CONTEXT.md`（32 094 B，全量）、`AGENTS.md`（8 748 B，全量）、`specs/v2/{session.md,tools.md,instructions.md,catalog-config-plugin-lifecycle.md}`（全量）、`specs/tui-package.md`（前 180 行）、`packages/codemode/README.md`（全量）。

## 附录 D：可以继续深入的清单

以下位置本次只做了清单级或抽样阅读，若要做更细的迁移设计，需要继续读：

| 位置 | 能回答的问题 |
|---|---|
| `packages/core/src/session/projector.ts`（441 行）+ `session/message-updater.ts`（386 行） | 流式增量如何合并成投影行、节流与覆盖写策略 |
| `packages/core/src/session/runner/publish-llm-event.ts`（403 行） | LLM 事件 → 持久化事件的具体映射与顺序保证 |
| `packages/core/src/agent.ts`、`catalog.ts`、`model.ts`、`provider.ts` | 模型解析与 agent 选择的具体规则（切模型/切 agent 的作用域） |
| `packages/core/src/skill/**`、`reference/**`、`integration.ts` | 技能发现、引用展开、集成的具体行为与边界 |
| `packages/llm/src/route/{client,executor,auth}.ts` | 重试/超时/取消的执行器细节与鉴权注入方式 |
| `packages/core/src/filesystem/**`、`ripgrep.ts`、`patch.ts` | 文件系统服务、忽略规则、二进制/大文件处理 |
| `packages/tui/src/routes/session/index.tsx`（2 539 行）、`context/sync.tsx`（643 行） | TUI 如何消费事件流、渲染与滚动策略 |
| `packages/session-ui/src/components/markdown-worker*.ts` | markdown 解析 offload 的队列/传输/缓存设计 |
| `packages/opencode/src/**`（旧单体） | V1 与 V2 的桥接细节、`SessionPrompt` 与插件 v1 hook 的真实行为 |
| `agent/lib/src/agent/training.cpp`（1 518 行）、`ffi/**` | agentxx 侧尚未纳入本文对比的两块实现 |
| `agent/third_party/cxx_pluginxx/include/pluginxx/host/tables_impl.h`（946 行）、`runtime/op_driver.h`（688 行） | 10 张通用表每个入口的投递/返回细节、协作式取消与操作驱动的完整语义 |
| `agent/third_party/cxx_pluginxx/include/pluginxx/kit/kit.h`（3 545 行） | 插件侧 `Task` 锚定协程、`CancelRegistry`、`ArgReader`、`capability` 等 SDK 能力的完整实现 |
| `agent/lib/src/plugins/client_plugin_manager.cpp`（4 009 行） | 客户端插件的 UI 注册表快照、工具渲染缓存与在途去重、命令路由的完整实现 |
| `packages/core/src/plugin/{agent,command,skill,models-dev,provider}.ts` + `config/plugin/*.ts` | opencode 内置插件如何把配置/技能/模型目录投影成域变换 |
| `packages/plugin/src/tui.ts`（568 行）+ `packages/tui/src/plugin/adapters.tsx`（343 行） | TUI 插件 API 的完整类型面与宿主适配层（槽位、键位层、对话框、attention） |
| `agent/client/src/**`（1.7 万行） | TUI 布局/滚动/输入法的实现细节（本文只覆盖结构与能力） |
