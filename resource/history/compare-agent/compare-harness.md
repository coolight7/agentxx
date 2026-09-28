# agentxx 与 harness 架构对比

> 对比对象
> - `agentxx`: `D:\0Acoolight\Program\cpp\agentxx`（C++23, Boost.Asio 协程 + NeoGraph 图引擎 + 线程间端点 + C ABI 插件）
> - `harness`（Harness Open Source / 原 Gitness）: `D:\0Acoolight\Program\js\harness`（**Go** 1.20+, 四层 Web 服务 + google/wire 编译期依赖注入 + sqlite/postgres 双驱动 + React 前端）
>
> 注意：目录名 `js/harness` 容易误判，harness 是 **Go 服务端项目**（DevOps 平台：代码托管 + CI 流水线 + Gitspaces 云开发环境 + 制品库），不是 JS agent 项目。
> 因此本文的对比重心不是"谁的 agent 循环更好"，而是：**一个成熟的多租户服务端工程把"配置/装配/存储/事件/作业/权限/沙箱/可观测/测试"这些横切层做到了什么程度**，其中哪些做法能搬进 agentxx 这个"单机本地优先、可远程、可插件扩展"的 agent 运行时。
>
> 本文按模块通读两个项目的源码、测试与文档后逐模块记录差异、优缺点与**可迁移到 agentxx 的设计**。
> 全文分三部分：**第一部分（§0~§21）**按模块给出结论与迁移清单；**第二部分（§22~§33）**是对源码实现细节的深读（含对第一部分的 4 处修正，见 [§32](#32-对第一部分的修正与补强)）；**第三部分（§34~§43）**是插件框架专题（[§34](#34-harness-的插件是什么三套语义--一套真正的扩展机制) 解释 harness 的"插件"到底是哪三套语义）。
> 阅读顺序建议：先看[总览](#0-总览与对比方法)与[迁移建议汇总](#19-迁移建议汇总)，再按模块展开；若要了解"机制到底怎么实现的"，读第二部分；关心扩展机制的直接读第三部分。

**修订记录**

- 第 1 版：建立模块划分（[§1](#1-总体架构分层与扩展模型)~[§18](#18-错误处理与上下文传递)），逐模块精读两侧源码后写入结论，每个模块给出「harness 做法（源码证据）→ agentxx 现状（源码证据）→ 优缺点 → 可迁移设计」。
- 第 2 版：补写易被忽略的两块——[§11 执行世界与沙箱](#11-执行世界与沙箱含-ci-执行器) 与 [§12 执行引擎](#12-执行引擎图循环--流水线与-agent-循环) 的源码级对照，并新增[附录 C](#附录-c本次精读的源码清单) 列出实际读过的文件。
- 第 3 版：**实现细节深读**（[§22](#22-深读harness-的资源寻址与一致性)~[§31](#31-深读agentxx-的客户端渲染与缓存)）——
  逐文件核对了两侧的寻址/乐观锁（§22）、状态机与规则引擎（§23）、作业 SQL 守卫与事件服务落地壳（§24）、外部世界接入（§25）、agentxx 的一轮会话（§26）、中断/恢复与逐点幂等（§27）、上下文压缩全流程（§28）、会话状态不变量与持久化（§29）、插件生命周期与执行体（§30）、渲染缓存与命中（§31）；
  并据此在 [§32](#32-对第一部分的修正与补强) 修正第一部分的 4 处结论、补强 2 处，在 [§33](#33-深读后的再评估) 给出新增迁移项（P0-12、P1-26~P1-30、P2 若干）与优先级调整。
- 第 4 版（本版）：**插件框架专题**（[§34](#34-harness-的插件是什么三套语义--一套真正的扩展机制)~[§43](#43-插件框架的双向可迁移项与结论)）——
  先把 harness 里"plugin"的四种用法分开（CI 步骤插件 / 模板 / 制品库包格式注册表 / wire 编译期扩展），再逐层拆开 agentxx 的插件框架（ABI 基座 / 宿主骨架 / 运行时 / SDK / 宿主领域层），
  并逐项深读 ABI 约束（§36）、Operation 终态协议与执行租约（§37）、SDK 的锚定协程与 `pollOne` 驱动（§38）、生命周期状态机与启用/禁用事务（§39）、接口协商与版本治理（§40）、安全/隔离/多实例（§41）、验证体系（§42），
  最后给出双向可迁移项（harness → agentxx：插件元数据集中管理 + 参数表单、插件包分发、高权限插件显式许可；反向：生命周期/终态/租约/协商四条成熟经验）。
- 整理版：重排全文结构、统一术语（"harness" 指 Go 服务端、"agentxx" 指本项目）、补全目录与交叉引用、把散落的迁移项收敛到 [§19](#19-迁移建议汇总) 的 P0/P1/P2 三级清单。

## 目录

| 章节 | 内容 | 一句话结论 |
|---|---|---|
| [0](#0-总览与对比方法) | 总览与对比方法 | 两个项目定位不同，可比的是横切层工程做法 |
| [1](#1-总体架构分层与扩展模型) | 总体架构、分层与扩展模型 | harness 靠 wire 编译期装配、agentxx 靠 C ABI 运行时插件，各有代价 |
| [2](#2-装配依赖注入与启动序列) | 装配、依赖注入与启动序列 | 应把"装配清单"显式化，并把启动/关闭做成可回滚的阶段序列 |
| [3](#3-配置体系) | 配置体系 | 派生默认值回填 + 启动期校验是 agentxx 缺的一环 |
| [4](#4-并发模型与生命周期) | 并发模型与生命周期 | 应用 errgroup 式的"失败即收敛 + 分阶段优雅关闭" |
| [5](#5-api-层与协议) | API 层与协议 | 错误分类渲染、请求解码分层、流式响应值得吸收 |
| [6](#6-持久化层) | 持久化层 | 缺迁移框架与写者所有权；store 接口 + 错误翻译值得学 |
| [7](#7-缓存与失效) | 缓存与失效 | 缓存装饰器 + 事件失效（Evictor）比"少缓存"更可控 |
| [8](#8-事件与消息) | 事件与消息 | 事件流 + 消费者组 + 重试/丢弃口径，是 agentxx 事件总线缺的一层 |
| [9](#9-后台作业与调度) | 后台作业与调度 | 持久化作业 + 状态机 + 进度 + 超期回收，直接可迁移 |
| [10](#10-认证授权与权限) | 认证、授权与权限 | 权限判定应显式产出"决策 + 理由"，且拒绝要带分类 |
| [11](#11-执行世界与沙箱含-ci-执行器) | 执行世界与沙箱（含 CI 执行器） | 出网策略（netpolicy）+ 执行后端抽象可直接迁移 |
| [12](#12-执行引擎图循环--流水线与-agent-循环) | 执行引擎（图循环 / 流水线与 agent 循环） | 只读/写两阶段执行 + 可取消排队，可缓解 agentxx 工具串行 |
| [13](#13-对象存储附件与大输出) | 对象存储、附件与大输出 | blob 抽象 + 流式读取 + 日志外部化 |
| [14](#14-可观测性与运维) | 可观测性与运维 | 结构化日志 + 审计 + pprof + 指标，是生产化的必要项 |
| [15](#15-客户端与-ui) | 客户端与 UI | 生成式客户端 + SSE 投影，与 agentxx 声明式组件互补 |
| [16](#16-测试与质量门禁) | 测试与质量门禁 | 测试分层（单元/集成/端到端/负载）+ 门禁自动化 |
| [17](#17-构建依赖与发布) | 构建、依赖与发布 | 对比 C++ superbuild 与 Go 单体模块的不同代价 |
| [18](#18-错误处理与上下文传递) | 错误处理与上下文传递 | 双轨错误（用户可见/内部）+ 上下文携带请求标识 |
| [19](#19-迁移建议汇总) | 迁移建议汇总 | P0/P1/P2 三级清单 + 落地位置 |
| [20](#20-反向清单agentxx-不必照搬的设计) | 反向清单 | 明确哪些差异是正当取舍 |
| [21](#21-结语) | 结语 | 三条可迁移的工程原则 |
| [附录 A](#附录-a关键文件与文档索引) | 关键文件/目录索引 | 两项目对应模块的路径对照 |
| [附录 B](#附录-b术语对照) | 术语对照 | 同一概念的两种叫法 |
| [附录 C](#附录-c本次精读的源码清单) | 精读源码清单 | 逐文件列出本次实际读过的位置 |
| [附录 D](#附录-d可以继续深入的清单) | 后续可深入清单 | 尚未精读的位置与能回答的问题 |

**第二部分：源码实现细节深读**

| 章节 | 内容 | 一句话结论 |
|---|---|---|
| [22](#22-深读harness-的资源寻址与一致性) | 深读：资源寻址与一致性 | 段链表 + 别名路径（可空列进唯一索引）、refcache 逐级消解、乐观锁重试闭包 |
| [23](#23-深读harness-的领域状态机与规则引擎) | 深读：状态机与规则引擎 | checks 驱动的合并队列（队首推进 + 作业兜底超期）；Definition/Verify 分离 + 结构化违规 |
| [24](#24-深读harness-的作业与事件落地细节) | 深读：作业与事件落地 | 状态机的真正实现是 SQL 守卫（`state='running'` 等）；服务接入作业是"常量 + 注册 + 排期"三段式 |
| [25](#25-深读harness-的外部世界接入) | 深读：外部世界接入 | git 数据面分层（api/command/parser）+ 安全头只给 UI + AES-GCM 兼容模式 |
| [26](#26-深读agentxx-一轮会话的完整实现) | 深读：一轮会话 | 线程不变量、唯一增量出口、RunConfig 的三个"不能改"、TurnEnd 带 tailHash |
| [27](#27-深读agentxx-的中断恢复与幂等) | 深读：中断/恢复与幂等 | graphData 键族承载跨中断状态；resume 必须带 cancel_token 与同 stream_mode；委派扁平化 |
| [28](#28-深读agentxx-的上下文压缩实现) | 深读：上下文压缩 | 75% 阈值 + API usage 优先 + 确定性压缩先行 + 冷却降级硬截断 + 工具级压缩句柄 |
| [29](#29-深读agentxx-的会话状态与持久化) | 深读：会话状态与持久化 | 四条线程/版本不变量、双写与节流、`SessionStoreHooks` 依赖倒置 |
| [30](#30-深读agentxx-的插件运行时与执行体) | 深读：插件运行时与执行体 | 生命周期五接缝、析构宁可泄漏不 dlclose、polled_tool 不占线程、进程组/Job Object 清理 |
| [31](#31-深读agentxx-的客户端渲染与缓存) | 深读：渲染与缓存 | OwnedReflect 绑定 Box 所有权、三档缓存预算 + 锚点模型 |
| [32](#32-对第一部分的修正与补强) | 对第一部分的修正 | delta 序号/tailHash、中断幂等、大输出外置"已存在"，共 4 处修正 + 2 处补强 |
| [33](#33-深读后的再评估) | 深读后的再评估 | 新增 P0-12、P1-26~P1-30 与 P2 若干；优先级调整（大输出外置降为 P1） |

**第三部分：插件框架深入对比**

| 章节 | 内容 | 一句话结论 |
|---|---|---|
| [34](#34-harness-的插件是什么三套语义--一套真正的扩展机制) | harness 的"插件"是什么 | 四种用法：CI 步骤插件（YAML 模板 + 容器执行）/ 模板 / 制品库包格式注册表 / wire 编译期；**没有进程内 ABI** |
| [35](#35-agentxx-的插件框架四层结构) | agentxx 插件框架四层结构 | ABI 基座 / 宿主骨架 / 运行时 / SDK + 宿主领域层；19 张 agent 表 + 9 张 client 表 |
| [36](#36-abi-细节为什么每一条约束都存在) | ABI 细节 | 每条约束都有具体理由（对齐/定长/调用约定/两态字符串/单一内存通道/表级版本/C4190） |
| [37](#37-operation-终态协议与执行租约宿主运行时) | Operation 终态与租约 | 恰一次完成、取消线性化三条规则、两侧租约、完成包重放、`CloseFailed` 可重试 |
| [38](#38-sdktask-锚定协程pollonebridge-与取消登记) | SDK：锚定协程与驱动 | `Task` 锚定到 Operation、pump/wake 合并、`polled_tool` vs `blocking_tool`、`CancelRegistry` |
| [39](#39-生命周期状态机启用禁用事务与清单) | 生命周期与启停事务 | create/start/stop/destroy 契约 + Failed 可重试 + 级联启用顺序 + prompt 贡献模型 + 宁可泄漏不 dlclose |
| [40](#40-接口协商与版本治理) | 接口协商与版本治理 | 三层协商（声明/校验/决策）+ 细粒度能力名 + 表内可空成员 + `api_version` 兜底 |
| [41](#41-安全隔离与多实例) | 安全、隔离与多实例 | 三铁律（含唯一例外）、权限声明与三态 `check_paths`、符号隐藏与校验脚本、跨边界禁异常 |
| [42](#42-插件框架的验证体系harness-侧无对应物) | 插件验证体系 | 负面编译 + C17 ABI 校验 + 真实 DSO 双端 + 多实例 + 终态协议 + 平台矩阵（harness 无对应物） |
| [43](#43-插件框架的双向可迁移项与结论) | 双向可迁移项与结论 | 可借鉴的是"周边"（元数据集中管理/参数表单/包分发/高权限许可），ABI 本身已成熟无需改 |

---

## 0. 总览与对比方法

### 0.1 两个项目的定位与规模

| 维度 | agentxx | harness |
|---|---|---|
| 语言/运行时 | C++23（`c++26/c17` 标准开关）, Boost.Asio 协程, 单线程 io_context 交错执行多会话 | Go 1.20+, 每请求 goroutine, `context` 贯穿, `errgroup` 收敛后台任务 |
| 形态 | 本地优先的 agent 运行时：库 `libagentxx` + 客户端（TUI / CLI / 远程 WS）+ C ABI 插件 | 多租户服务端平台：单二进制 `gitness`（HTTP + SSH + git 数据面 + CI 执行器 + 制品库） |
| 主要能力 | LLM 会话（多会话并发、工具调用、压缩、权限、子代理、worktree 隔离） | 代码托管（space/repo/pullreq/git）、流水线（pipeline/stage/step）、Gitspaces（容器化开发环境）、制品库 registry |
| 扩展模型 | **运行时插件**：`agentxx_plugin_agent_create/start/stop/destroy` + 19 张 agent 接口表 + 9 张 client 接口表 | **编译期装配**：每个包导出 `WireSet`，`cmd/gitness/wire.go` 汇总，`wire_gen.go` 生成构造函数；无运行时插件 ABI |
| 客户端 | FTXUI TUI（唯一渲染实现）+ stdio CLI + 远程 WS 客户端 + FFI 宿主 | React Web（`web/src`, 1638 文件）+ Swagger 生成客户端 + `cli/textui` 交互式 shell |
| 服务端形态 | 可选：`agentxx_cli` 既可是同进程 TUI，也可 `--server` 提供 WS/HTTP 端点给远端客户端 | 必须：HTTP(:3000) + 可选 SSH(:3022/22) + 可选 metrics server + CI poller |
| 存储 | SQLite 单库/会话（`SessionStore` 按 sessionId 分目录）, `settings_db` 全局 KV | SQLite **或** PostgreSQL（同一套 `squirrel` 构造器 + `sqlx`）, 表数量级数十张, 迁移序列 `migrate_0001..0160+` |
| 横向依赖 | 无外部服务依赖（可选 Redis 无） | 可选 Redis：pubsub / lock / events stream / cache 可切 redis 后端；Webhook/通知/触发器/作业调度都建立其上 |
| 事件 | 进程内 `EventBus`（agent 全局 + 会话级）+ 通道写事件 + 客户端 delta 推送 | `events`（持久化事件流 + 消费者组 + 重试/丢弃）+ `pubsub`（实时广播）+ `sse`（推给浏览器）+ webhook/notification/trigger 消费者 |
| 后台作业 | 插件定时器、offload 线程池、子代理（`subagent`）、TUI 定时器 | `job.Scheduler`：作业表 + cron + 优先级 + 进度 + 重试 + 超期回收 + 保留清理 + 全局锁 + 取消广播 |
| 权限 | 权限中间件（白/黑名单 + 模式 + 记住选择 + 完全授权 + worktree 隔离），插件声明权限 | AuthN（token/jwt/session/PAT/service account）+ AuthZ（spaces/repos 的 RBAC + public access）+ 审计 |
| 测试 | 自研 `agentxx_test`（模块化, 173 文件 / 8.5 万行）+ `agentxx_benchmark` 资源基准 + sanitizer | Go 标准测试（288 文件 / 7 万行）+ `httptest` 风格 handler 测试 + 集成/负载测试 + `.testapi` + golangci-lint |
| 规模（本次统计，不含第三方与 build 产物） | lib 139 文件 / 6.07 万行；client 68 / 2.59 万；plugins 53 / 2.59 万；test 173 / 8.51 万；自研三库 67 / 2.61 万 | 非测试 Go 2526 文件 / 33.97 万行；测试 288 / 6.97 万行；`web/src` 1638 / 18.77 万行 |
| 文档 | `docs/zh-cn/design/*.md`（index 2156 行 + plugins 1310 + tui 560 + ui-layer 243 + benchmark 685 + ffi 351，中英双语目录） | 仅 `README.md` / `CONTRIBUTING.md`；其余信息靠代码注释、`web/src/services/code/swagger.yaml` 与运行时 `/swagger` 页面 |

**定位差异的直接影响**：harness 的每一个设计约束都来自"多进程、多实例、多用户、可重启、数据不能丢"；agentxx 的每一个设计约束都来自"单进程、单用户、低延迟、可离线、插件可热插拔、多平台（含 Android）"。因此**不能照抄整体架构**，只能照抄那些"与部署形态无关的工程做法"——错误分类、作业状态机、事件流重试口径、迁移框架、出网策略、缓存失效广播等。

### 0.2 对比方法与判据

- 每个模块都从**三方证据**核实：源码（真实实现）、测试（行为边界）、文档/注释（设计意图）。凡与文档表述冲突处**以源码为准**。
- 引用格式：`文件路径:行号近似位置`（harness 侧路径以仓库根为基准，agentxx 侧以 `agent/` 为基准）。为避免行号漂移，多数结论以"文件 + 函数/结构体名"定位。
- "优点/缺点"的判据只有两条：
    1. **能否降低后续改动成本**：扩展点是否收敛、约束是否显式、新增能力需要改几个地方；
    2. **运行期是否正确/稳定**：生命周期、取消、恢复、并发、失败姿态。
- "迁移建议"只写**能在 agentxx 现有架构上实现**的项，并标注落地位置（哪个文件/哪个层次）与代价。

### 0.3 结论速览

| 模块 | agentxx 现状 | harness 现状 | 更值得借鉴的方向 |
|---|---|---|---|
| 分层/扩展 | 分层清晰 + C ABI 插件，装配分散在 `init*` 虚函数 | 四层（handler→controller→service→store）+ wire 装配清单 | 装配清单显式化、启动阶段可回滚 |
| 配置 | YAML 两层合并（base/overlay）+ `settings_db` 运行时 KV | `envconfig` 结构体 + `.env` + kingpin 参数 + 派生 URL 回填 | 派生默认值回填、启动期校验与报错定位 |
| 并发/生命周期 | asio 协程 + io_context + `CancelToken`；客户端/服务端同线程模型 | goroutine + `context` + `errgroup` + 分阶段优雅关闭 | 失败即收敛、关闭顺序按依赖倒序、带超时 |
| API/协议 | 手写 wire 协议（1293 行）+ 事件流 + TUI 直渲 | REST + openapi 生成 + 请求解码中间件 + 流式/SSE 渲染 | 错误分类渲染、请求解码分层、动态数组流式输出 |
| 持久化 | `SqliteDb` RAII + 会话分库 + 手写幂等建表/ALTER | store 接口 + sqlx + 双驱动 + `dbtx` 事务 + 迁移序列 + SQL 错误翻译 | 迁移框架、写者所有权、SQL 错误分类翻译 |
| 缓存 | 少量 LRU（`share_store` 3 条 / 会话连接 32 条） | `cache.Cache` + TTL/LRU + `Evictor` 经 pubsub 广播失效 | 缓存装饰器化 + 失效广播（多实例/多客户端一致） |
| 事件 | 进程内 `EventBus` + 通道写事件 + delta 推送 | `events`（事件流/消费者组/重试/丢弃）+ pubsub + SSE | 消费者组 + 重试/idle 超时/丢弃计数口径 |
| 后台作业 | 依赖插件/会话生命周期内的临时机制 | `job.Scheduler`（持久化 + cron + 进度 + 重试 + 超期回收） | 持久化作业表 + 状态机 + 超期回收 |
| 权限 | 中间件统一判定 + 插件声明 + 记住选择 + 完全授权 | AuthN/AuthZ 分层 + 资源路径模型 + 审计 | 决策理由可观测、拒绝分类、审计留痕 |
| 执行世界 | 本地执行 + `execute_command` + worktree 隔离 + 权限模式 | Gitspace 容器编排 + devcontainer + infraprovider + netpolicy | 出网策略（netpolicy）、执行后端抽象 |
| 引擎 | 图引擎（ReAct 循环）+ 工具串行 | 流水线调度（stage/step）+ 只读/写两阶段 + worker 拉取 | 只读阶段批并行、可取消排队 |
| 大输出 | 工具结果截断/回填、share_store（已有 spill + 定位符 + 分片读，见 [§32 修正 4](#修正-4大输出外置与定位符已实现工具级缺统一策略)） | blob 抽象 + logs store（DB/S3）+ 流式读取 | 统一 spill 策略 + 结构化定位符 |
| 可观测 | `XX_LOG` + 基准程序 + 内存指标 | zerolog 结构化 + request id + audit + livelog + metrics + pprof | 请求标识贯穿、审计、pprof 端点 |
| 客户端 | 声明式 UI 组件 + 表单宿主态 + 能力协商 | React + Swagger 生成 + SSE 投影 | 生成式协议/客户端、事件投影 |
| 测试 | 自研模块化测试 + 资源基准 + sanitizer | 标准测试 + 集成/负载 + 门禁 | 测试分层与门禁自动化 |
| 构建 | CMake superbuild + 静态三库 + 插件隐藏符号 + LTO/ICF | Makefile + 单模块 + vendor + Docker | 交叉/平台矩阵门禁、依赖图生成 |
| 错误 | `catchError*` + `ToolcallWrapNode` 统一格式化 | `usererror`（用户可见）+ `errors`（分类/状态码）双轨 | 双轨错误码 + 上下文携带 |

---

## 1. 总体架构、分层与扩展模型

### 1.1 harness：四层 + 编译期装配

**入口与分发**（`cmd/gitness/main.go`, `app/router/router.go`）

- 进程入口只做三件事：解析命令行（`kingpin`）→ 注册子命令（`migrate` / `server` / `user(s)` / `account` / `hooks` / `swagger`）→ `MustParse`。
- HTTP 侧不是一张大路由表，而是**按"流量资格"分发的路由器数组**（`app/router/router.go:NewRouter`）：`Router.ServeHTTP` 遍历 `routers`，第一个 `IsEligibleTraffic(req)` 为真的接管请求：
    - `APIRouter`：`Path` 以 `/api/` 开头 → 剥掉前缀后交给 API 处理器；
    - `GitRouter`：git 智能 HTTP 协议路径（含 LFS）→ git 数据面；
    - `WebRouter`：其余路径 → 静态前端（`web/dist.go` 嵌入）或重定向。
- 这种"资格判定 + 前缀剥离 + 各路由自持日志上下文"的写法（`app/router/api_router.go`）让三类流量（API / git / UI）的中间件栈互不污染——git 流量不需要 JSON 渲染栈，UI 流量不需要认证中间件。

**API 层四段**（`app/api/`）

| 段 | 包 | 职责 | 证据 |
|---|---|---|---|
| 解码 | `app/api/request/*.go`（40+ 文件） | 路径参数/查询参数/请求体解码为 `types` 输入结构，含路径规范化（`types/path.go`）与 `RawPath` 处理 | `app/request/request.go:ReplacePrefix` 明确区分 `URL.Path` 与 `URL.RawPath`（后者保持转义，避免 `%2F` 被解开导致路由错位） |
| 中间件 | `app/api/middleware/{authn,authz,encode,address,logging,nocache,principal,goget,web}` | 认证（失败降级为匿名）/授权限制（类型/管理员）/编码修正/地址回填/无缓存/goget | `authn/authn.go:Attempt`：`ErrNoAuthData` → 匿名会话；其它错误 → 401 |
| 端点 | `app/api/handler/<domain>/<action>.go`（约 300 个文件，每端点一文件） | 只做编排：解码 → 调用 controller → `render` 输出 | `app/api/handler/` 目录结构即 API 面 |
| 业务 | `app/api/controller/<domain>/`（28 个域） | 业务规则、事务边界、事件发布 | `app/api/controller/{repo,pullreq,space,gitspace,pipeline,...}` |
| 输出 | `app/api/render/*` | JSON / 流式 JSON 数组 / 原文流 / SSE / git basic-auth 挑战 | `render.go:JSONArrayDynamic`：边取边写，出错时"已有数据就只能关数组" |

**横切基础设施**（与 API 层平行、被 service 与 controller 共用）

- `store/`（包级）：领域错误词表（`store/errors.go`）+ `store/database/*`（sqlx + squirrel 实现 + 迁移 + 错误翻译） + `store/database/dbtx`（事务接口与锁）。
- `events/`（事件流）+ `pubsub/`（广播）+ `stream/`（内存/Redis 流后端）+ `cache/`（缓存抽象）+ `lock/`（分布式锁）+ `job/`（后台作业）。
- `types/`（领域模型 + `enum/` 枚举 + `check/` 校验 + 分页与过滤器）+ `errors/`（分类错误）+ `types/config.go`（配置结构）。

**扩展模型**：**没有运行时插件 ABI**（下文 §34 会说明：harness 仓库里名为 "plugin" 的东西有四种用法，都不属于进程内扩展机制）。

- `app/pkg.go` 只 import `github.com/google/wire/cmd/wire`（`//go:build tools`），说明装配完全在编译期完成。
- 新增一个能力 = 新增 Go 包 + 该包导出 `WireSet` + 在 `cmd/gitness/wire.go` 的 `initSystem` 里追加一行 + 在 `providers.go` 里给出 Provider 函数；由 `wire_gen.go`（62 KB 生成物）在编译期推导构造顺序并检测缺失依赖。
- `types/plugin.go` 里的 "plugin" 是**制品库插件（包格式）**语义，与扩展机制无关，容易误读。

### 1.2 agentxx：分层 + 运行时 C ABI 插件

- **分层**（据 `AGENTS.md` 与目录）：`agent/client`（TUI/CLI/远程客户端 + 配置加载）、`agent/lib`（libagentxx：`BaseAgent`/`CodeAgent`、`nodes`、`middlewares`、`tools`、`protocol`、`plugins`、`io`、`util`）、`agent/plugins`（15 个内置能力插件）、`agent/third_party`（含自研三库 `cxx_utilxx_base` / `cxx_utilxx` / `cxx_pluginxx`）。
- **装配点分散但显式**：`BaseAgent::init()` 依次调用 `initRegisterNodes` → `initMiddleware` → `initTools` → `initModelRegistry` → `initEventBus` → `initGraphDefinition`（见 `agent/lib/include/agentxx/agent/base_agent.h` 注释："在 `init()` 中于 `initMiddleware()` 之前调用"等约定）。子类 `CodeAgent` 覆写这些钩子即可改变 agent 形态（`agent/lib/src/agent/code_agent.cpp`）。
- **执行引擎**：图引擎（NeoGraph）。默认图 `agentxx.default`：`__start__ → agent_start → llm →(条件边 xx_has_tool_calls)→ tools / agent_end → __end__`（`base_agent.h:initGraphDefinition` 注释 + `nodes/graph_conditions.h`）。
- **上下文归属**：会话（`Session`）是 LLM 上下文的唯一权威，图状态里没有 `messages` 通道，只有只读影子通道 `xx_messagesMeta`（`nodes/session_context.h`, `nodes/graph_conditions.h` 注释）。
- **插件**：API v1 纯 C ABI，`create` 只构造、`start` 是"注册事务"、`stop` 撤销自管资源、`destroy` 释放本地对象；能力经**接口表按名称查询**；多实例契约（禁可变全局/函数级 static、实例状态只放 `*plugin_ctx`）。
- **内置能力也是插件**：`agent/plugins/agentxx_{filesystem,execute_command,websearch,rag_search,planning,math,string,codegraph,system,system_monitor,screen_capture,computer_use,text_selection_monitor,javascript_engine,audio_stream}`，共 53 文件 / 2.59 万行。

### 1.3 对比：两种扩展模型的代价

| 维度 | harness（wire 编译期） | agentxx（C ABI 运行时） |
|---|---|---|
| 依赖检查 | 编译期（缺 Provider 直接编译失败） | 运行期（接口表查询/版本协商） |
| 第三方扩展 | 需重新编译并分发二进制 | 投放 `.so`/`.dll` 即可，可禁用/卸载 |
| 隔离 | 同进程、同语言、无边界 | ABI 边界（结构体对齐/定长类型/调用约定显式约定） |
| 卸载/热替换 | 不支持（静态） | 支持（`stop` + 卸载 + 级联依赖 + 关闭超时重试） |
| 装配可读性 | 单一清单（`initSystem` 200+ 行）+ 生成代码 | 分散在 `init*` 钩子 + 插件管理器 |
| 错误代价 | 一个 Provider 缺失 = 全量编译失败（定位容易） | 一个接口版本不匹配 = 运行期日志（定位稍难） |
| 性能 | 直接调用 | 一次 vtable 间接调用（可忽略），但状态只能放堆块 |

**harness 的优点**：装配是**一棵显式的树**，`cmd/gitness/wire.go` 的 `initSystem` 事实上就是"系统由哪些东西构成"的唯一权威文档；`wire.Bind(new(gitspaceinfraeventservice.Orchestrator), new(orchestrator.Orchestrator))` 这种"把宽接口绑到窄接口"的写法，使事件服务只依赖自己声明的窄接口，避免了包体量与循环依赖（代码注释明确说明：把 docker/oras 重的 orchestrator 挡在事件服务的 import 图之外）。这一点对 agentxx 有直接借鉴价值——agentxx 的插件与内置能力之间存在类似的"宽实现 vs 窄依赖"问题（例如插件要拿会话、要拿事件总线、要拿工具注册表）。

**harness 的缺点**：没有运行时扩展点，导致"运维期开关"只能靠配置项与 build tag（`nosqlite`、`wireinject`）；新增一个可替换能力必须新增接口 + 两个 Provider + 装配项，模板代码多。

**agentxx 的优点**：插件模型支撑了"内置能力全部插件化"这个更强的目标（15 个内置插件），且多实例契约、禁用/卸载/级联依赖、`start` 事务化回滚都是 harness 完全没有的复杂度，agentxx 已经解决。

**agentxx 的缺点**：装配清单缺少"单一入口"。`BaseAgent::init()` 的调用顺序靠注释约定（"在 initTools() 之前调用"），新增中间件/工具/节点/条件需要同时改多处；插件管理器的生命周期骨架（`PluginHostLifecycle`）虽然已统一，但**宿主侧装配**（哪些插件默认启用、哪些中间件默认挂载）仍是分散的。

**可迁移设计（§1）**

1. **装配清单化（P1）**：在 `BaseAgent::init()` 内部引入一个显式的"阶段清单"结构（如 `std::vector<InitStep>{name, fn, rollbackFn}`），把 `initRegisterNodes/initMiddleware/initTools/initModelRegistry/initEventBus/initGraphDefinition` 收敛为数据而非调用序列；同时提供"当前生效能力的快照"供 `WireGetContext`/Info 面板展示（harness 的 `initSystem` 与 `swagger` 页面是同类"系统自描述"）。
    - 落地：`agent/lib/src/agent/base_agent.cpp`（`init()`）+ `agent/lib/include/agentxx/agent/context.h`（新增 `capabilitySnapshot`）。
2. **窄接口绑定（P1）**：插件/中间件对宿主的依赖目前是"一揽子 `AgentContext`"，可借鉴 wire 的 `Bind`，按消费者定义窄接口（如 `SessionReader`、`PermissionCheck`、`ToolRegistrar`），宿主侧用适配器满足；好处是中间件/插件测试可以只造窄桩。
    - 落地：`agent/lib/include/agentxx/agent/context.h` 拆分访问视图；插件 SDK 侧已具备（`agentxx/plugin/api/plugin_kit.h`）。
3. **启动阶段可回滚（P2）**：harness 的插件 `start` 失败返回 `NULL + error` 由宿主回滚，是 agentxx 已有的机制；反过来 agentxx 的**内置能力**初始化失败缺少回滚（部分中间件已挂载、工具已注册，然后抛异常）。建议给 `init()` 加对称的清理路径。
    - 落地：`agent/lib/src/agent/base_agent.cpp`。

---

## 2. 装配、依赖注入与启动序列

### 2.1 harness：wire + 分阶段启动

- **装配**：`cmd/gitness/wire.go:initSystem` 是唯一装配根，`wire.Build(...)` 罗列 150+ 个 `WireSet`/Provider。每个包自己声明 `WireSet`（例：`app/store/wire.go` 只提供两个"字符串变换函数"；`app/store/cache/wire.go` 提供 5 个缓存 + 2 个失效器；`events/wire.go` 用 `ProvideSystem` 依据配置在 inmemory/redis 之间切换，并在内部构造 broker/consumer/producer）。
- **配置注入**：`cli/operations/server/config.go` 是一组 `ProvideXxxConfig(config *types.Config) XxxConfig` 函数——**配置到子系统的转换层**。例如 `ProvideDatabaseConfig` 只挑 5 个字段；`ProvideBlobStoreConfig` 还会在 filesystem provider 且 bucket 为空时把 bucket 回填为 `~/.gitness/blob`；`ProvideEventsConfig/ProvideLockConfig/ProvidePubsubConfig/ProvideJobsConfig` 分别把 `types.Config` 的对应段落映射为子系统配置结构。这让"配置结构"与"子系统配置"解耦，子系统可在测试里被直接构造。
- **启动序列**（`cli/operations/server/server.go:run`）：
    1. `signal.NotifyContext(SIGINT, SIGTERM)` 建根 ctx；
    2. `godotenv.Load(envfile)`（文件不存在不报错）→ `LoadConfig()`（envconfig）→ `SetupLogger` → `SetupProfiler`；
    3. `initializer(ctx, config)` = wire 生成的 `initSystem`（构造整棵对象树，**不启动任何东西**）；
    4. `system.bootstrap(ctx)`（建 admin 用户/系统 principal/流水线 principal/gitspace principal，见 `app/bootstrap/bootstrap.go`：全部是"找不到就创建，遇到 duplicate 再查一次"的幂等写法）；
    5. `errgroup.WithContext(ctx)`：把指标收集器、仓库大小计算、清理服务、**作业调度器**一起跑；再 `g.Go(gHTTP.Wait)`、`g.Go(gMetric.Wait)`；
    6. 可选：CI 插件解析器 + 构建轮询器、SSH 服务；
    7. 等 `gCtx.Done()` → 复位信号处理 → `context.WithTimeout(GracefulShutdownTime)` 分步关闭：HTTP → SSH → metric → instrumentation → `JobScheduler.WaitJobsDone` → `g.Wait()`。
- **关闭顺序有讲究**：先停"入口"（HTTP/SSH），再停"后台生产/消费"，最后等 goroutine 收尾。`JobScheduler.WaitJobsDone(ctx)` 用"由外部 ctx 限时的等待"避免卡死。

### 2.2 agentxx：注入 + 会话循环

- `BaseAgent` 直接持有 `ioCtx` / `engine` / `agentContext` / `graphRegistry`，构造后需显式 `co_await init()`（`agent/lib/include/agentxx/agent/base_agent.h`）。
- 依赖传递靠 `AgentContext`（`agent/lib/include/agentxx/agent/context.h`，730 行）：会话存储、中间件上下文、模型注册表、事件总线、插件管理器、图定义 JSON 都挂在这里；中间件与插件通过 `weak_ptr<AgentContext>` 取用（`middlewares/middleware.h:BaseMiddlewareHandleInterface`）。
- 启动/关闭：`init()` 做注册；`shutdownAsync(timeout)` 用于停止 agent 插件并切回 agent IO executor；客户端侧有 `agent_host.cpp` / `agent_runner.cpp` 组织多模式运行（本地 TUI / serve / train 等，见 `client/mode_runners.cpp`）。
- 配置：`AgentConfig`（`agent/lib/include/agentxx/agent/config.h`，380 行）在客户端 `config_loader.cpp`（1550 行）里由 YAML + `.env` 分层合并得到。

### 2.3 对比与结论

| 问题 | harness | agentxx | 结论 |
|---|---|---|---|
| 依赖缺失何时暴露 | 编译期 | 运行期（可能只体现在日志） | agentxx 应把"关键依赖齐备"做成启动期断言，失败即拒绝启动（而不是降级运行） |
| 配置到子系统的映射 | 显式 `ProvideXxxConfig` 转换层 | 直接读 `AgentConfig` 字段 | agentxx 可为每个子系统提供 `XxxConfig` 视图，便于测试与默认值回填 |
| 启动/关闭对称性 | 分阶段 + 超时 + 倒序 | `init()` / `shutdownAsync()`，缺"阶段化" | 引入阶段化启动（含回滚）与倒序关闭 |
| 幂等引导 | `bootstrap.go` 对每种内置实体"找不到就建/重复就查" | 无对应需求（无多实例竞争） | 不必迁移 |
| 自描述 | `/swagger`、`/api/v1/system/config`（`handler/system/list_config.go`）暴露生效配置 | Info 面板展示模型/插件/工作目录 | 可补"生效装配快照"（见 §1 建议 1） |

**各自做得好的设计**

- harness：①"配置 → 子系统配置"的显式转换层，使子系统可在测试中独立构造；②启动即"先构造后启动"的两段式，任何启动失败都在同一层被 `errgroup` 收敛；③关闭顺序显式且带超时（`GracefulShutdownTime`）。
- agentxx：①`start` 作为"注册事务"由宿主回滚（插件），比"边构造边注册"更安全；②多实例契约让同一动态库可在多个 agent 宿主并存，这是服务端项目完全不需要的能力；③"会话是上下文唯一权威"避免了图状态与上下文双写不一致。

**可迁移设计（§2）**

1. **子系统配置视图（P1）**：为 `ModelRegistry`/`SessionStore`/`PluginManager`/`ToolRegistry`/`Summarization` 各提供 `XxxConfig` 结构 + `makeXxxConfig(const AgentConfig&)` 函数，默认值与校验写在一处；`config_loader.cpp` 只负责产出 `AgentConfig`。
    - 落地：`agent/lib/include/agentxx/agent/config.h` + 各子系统构造处。
2. **启动期依赖断言（P0）**：`BaseAgent::init()` 末尾统一校验（模型可用、dataDir 可写、插件目录可读、sqlite 可开、必要节点已注册），任一失败给出明确错误并以非零退出码终止，而不是"少个工具静默继续"。
    - 落地：`agent/lib/src/agent/base_agent.cpp`；客户端侧 `agent/client/src/mode_runners.cpp` 汇总错误。
3. **分阶段关闭（P1）**：`shutdownAsync` 目前是单一超时；建议按"停止接收新输入 → 等待当前轮结束 → 停止定时器/作业 → 关闭插件 → 刷盘（sqlite checkpoint）"分阶段，并支持总超时 + 强制。
    - 落地：`agent/lib/src/agent/base_agent.cpp`（`shutdownAsync`）+ `agent/lib/src/plugins/plugin_manager_lifecycle.cpp`。

---

## 3. 配置体系

### 3.1 harness：环境变量 + 结构体标签 + 派生回填 + 启动期校验

- **单一配置结构**：`types/config.go` 的 `Config` 是唯一配置源，字段带 `envconfig:"GITNESS_XXX"` 与 `default:"..."` 标签，由 `kelseyhightower/envconfig` 一次性填装（`cli/operations/server/config.go:LoadConfig`）。
    - 默认值写在 schema 里（`GITNESS_HTTP_PORT default:"3000"`、`GITNESS_GRACEFUL_SHUTDOWN_TIME default:"300s"`、`GITNESS_DATABASE_MAX_OPEN_CONNS default:"25"`），代码里不需要 `if x == 0 { x = 3000 }`。
    - 嵌套结构体分组即配置分区（`URL` / `Git` / `HTTP` / `SSH` / `CI` / `Database` / `BlobStore` / `Claims` / …），每个字段都有注释说明"何时可省略、省略后由什么推导"。
- **`.env` 加载**：`godotenv.Load(envfile)`，文件不存在**不报错**（`_ = godotenv.Load(...)`），使"无配置启动"成为合法路径。
- **派生回填（backfill）**：`LoadConfig` 里两段"从已有值推导缺失值"的逻辑很值得学：
    1. `backfillURLs`：先按 HTTP/SSH 配置算出 base，再依次回填 `URL.Internal`（`localhost`）、`URL.Container`（`host.docker.internal`，供容器内构建回调宿主）、`URL.GitSSH`、`URL.Base`、`URL.API`（`Base + /api`）、`URL.Git`（`Base + /git`）、`URL.UI`、`URL.Registry`；用户显式提供 `URL.Base` 时校验 scheme 只允许 http/https、host 必须非空，并把 SSH host 对齐到 base host。
    2. `Git.Root` 缺省 `~/.gitness`，且**自动迁移旧路径**：若存在 `~/.gitrpc` 则尝试 `os.Rename`，失败则回退旧路径（兼容升级）。
- **环境相关标识推导**：`getSanitizedMachineName()` 把主机名经 NFD 分解 → 去变音符 → 小写 → 只保留 `[a-z0-9-.]`、其余替换为 `_`，作为 `InstanceID`（注释明确指出这是 k8s 命名规范，且"可能重叠，可显式传入"）。
- **启动期校验**：`events.Config.Validate()` / `lock` / `pubsub` / `blob` 各自有校验，`ProvideXxxConfig` 在构造阶段就把非法配置变成 error（如 base url 缺 host 直接返回错误），不会拖到第一次请求才炸。
- **运行期配置暴露**：`handler/system/list_config.go` 把生效配置暴露给管理员（配合 `web` 设置页）。
- **运维开关**：全部是环境变量（`ENABLE_CI`、`GITNESS_SSH_ENABLE`、`GITNESS_NESTED_SPACES_ENABLED`…），容器化部署友好；Docker/Gitspace 场景还专门有 `GITNESS_DOCKER_HOST` / `GITNESS_DOCKER_API_VERSION`（README）。

### 3.2 agentxx：YAML 两层合并 + 运行时设置库

- **分层**：overlay（工作目录或 `--config` 指定 yaml + 同目录 `.env`）覆盖 base（overlay 的 `data_dir` 下的 `agentxx-config.yaml` / `.env`；未配置 `data_dir` 时取 `~/.agentxx/`）。
- **合并规则**（`AGENTS.md` + `agent/client/src/config_loader.cpp`）：列表段统一 `{overwrite: {mode: merge|replace, remove: [...]}, list: [...]}`；`merge` 按键归并/追加去重，`replace` 整段不继承，`remove` 按身份剔除 base 项（model=name / plugin=path·name / mcp=namespace / 路径列表=字符串）；其余键为标量覆盖、映射逐键合并；`.env` 同名变量取 overlay 值；旧键/旧写法被告警并忽略。
- **配置模型**：`AgentConfig`（`agent/lib/include/agentxx/agent/config.h`）把模型（`ModelConfig`：baseUrl/apiKey/modelName/超时/多模态能力/连接池上限/额外请求头/`extraConfig`）、MCP（`McpServerConfig`：url + 工具调用超时）、插件（`PluginConfig`：path/name/sides/是否启用…）、权限模式（`PermissionMode::{Ask,AllAsk,Pass,Deny}`）等收敛为强类型结构；`PermissionMode` 直接决定 `CodeAgent` 注册的默认规则（见 `config.h` 注释与 `code_agent.cpp`）。
- **运行时可变设置**：`settings_db`（`agent/lib/include/agentxx/util/settings_db.h` + `agent/lib/src/util/settings_db.cpp`，130 行）是全局 KV；客户端 `tui_settings.cpp` 经它持久化「是否启动检查更新」等界面设置，服务端侧另有 `WireSetFullAuth` 等跨端状态。
- **不一致点（值得补）**：
    - 默认值分散：`ModelConfig::defaultModelConfig` 是一个静态默认；但 `connectTimeoutSeconds=16` / `readChunkTimeoutSeconds=60` / `maxConcurrentConnections=5` 这类默认写在类成员初始化式里，配置文档与代码两处维护；
    - 缺"派生回填"层：例如 `dataDir`、`workDir`、`resolvedWorkDir`、会话数据库根目录之间的推导分散在 `SessionStore` 构造与 `permission` 中间件（`normalizePermissionPath` 用 `AgentConfig::resolvedWorkDir`，未配置时回退进程 cwd）；
    - 缺统一校验入口：`ModelConfig::isValid()` 只覆盖单模型，跨字段一致性（例如 `plugin.sides=client` 却放在 agent 段、`permission.mode` 与白名单冲突）没有统一检查点。

### 3.3 对比与结论

| 维度 | harness | agentxx |
|---|---|---|
| 来源 | 环境变量 + `.env`（可选）+ CLI flag | YAML（两层）+ `.env`（两层）+ 运行时 KV |
| 默认值 | 声明在 schema 标签，一处 | 分散在结构体初始化式 / 代码分支 |
| 派生值 | 显式 backfill 函数 + 校验 | 分散在子系统构造处 |
| 校验时机 | `ProvideXxxConfig` 构造期 + `Validate()` | 部分（`isValid` / 解析时报错） |
| 列表继承 | 无（无分层） | 有（merge/replace/remove，较 harness 更强） |
| 运行期可变 | 基本不可变（改了要重启） | `settings_db` 支持部分项热改 |
| 敏感值 | 环境变量注入（CI/容器友好） | yaml/.env（本地友好） |

**harness 的优点**：①**派生回填**把"用户只配两三个值也能跑"变成代码里的一条明确链路，且旧路径自动迁移；②默认值声明式，避免"文档与代码不一致"；③配置校验在装配期完成，错误信息带上下文（`failed to parse base url 'x': ...`）。

**agentxx 的优点**：①两层合并 + 列表级 `merge/replace/remove` 的表达力远超环境变量方案，**本地多项目/多环境**场景更实用（这也是"工作目录 yaml 覆盖 `~/.agentxx`"的诉求来源）；②运行时设置库让界面设置不必重启；③配置即数据（模型列表/MCP/插件都是列表项），天然支持多模型并存与按会话选择。

**agentxx 的缺点**：①没有统一校验与回填，导致"配置写错"常常表现为运行期某个工具不可用；②同一个语义的路径（dataDir/workDir/worktree 根）在多处推导，容易不一致；③`ModelConfig` 的能力位（`imageInput` 等）与实际 provider 行为的一致性靠注释说明，缺少"启动期自检"。

**可迁移设计（§3）**

1. **配置回填与校验层（P0）**：
    - 新增 `agent/lib/src/agent/config_resolve.cpp`（或 `agentxx::agent::resolveConfig(AgentConfig&)`）：集中处理 `dataDir → ~/.agentxx` 回退、`workDir → cwd` 回退、会话库根目录推导、插件默认启用集合、`permission.mode` 与白/黑名单一致性；
    - 校验失败返回带路径的错误（形如 `model.list[2].apiKey: 不能为空`），由客户端在启动时打印并退出非零。
    - 落地：`agent/lib/include/agentxx/agent/config.h` + `agent/client/src/config_loader.cpp`。
2. **默认值声明式（P1）**：把 `connectTimeoutSeconds/readChunkTimeoutSeconds/maxConcurrentConnections/modelContenxtMaxToken` 等默认集中到一个 `constexpr` 默认表（或 yaml 内嵌模板），供配置文档、`/config` 展示、测试共享。
3. **旧配置自动迁移（P2）**：参照 `Git.Root` 的 `~/.gitrpc → ~/.gitness` 迁移，为已废弃键（`models` → `model.list` 等）在告警之外提供一次性自动改写（写回 overlay yaml 并提示），减少用户手工改配置。
4. **生效配置可查询（P1）**：TUI 的 Info/设置面板应能展示"解析后的生效值"（含回填后的路径、默认模型、权限模式、插件启用列表），供排障；harness 对应能力是 `/swagger` + `list_config`。

---

## 4. 并发模型与生命周期

### 4.1 harness：goroutine + context + errgroup

- **并发单位**：一次 HTTP 请求一个 goroutine（标准库 `net/http`）；后台任务（事件消费者、作业、清理、CI 轮询）各自是长期 goroutine，全部由**同一个 ctx 树**统辖。
- **失败收敛**：`errgroup.WithContext(ctx)` —— 任一后台 goroutine 返回 error，`gCtx` 取消，其他人收到取消，`<-gCtx.Done()` 解除阻塞，进入关闭流程（`cli/operations/server/server.go`）。
- **取消贯穿**：`context.Context` 一路传到存储层（`QueryContext`）与 HTTP 客户端；`dbtx.runnerDB.WithTx` 甚至在 `sql.ErrTxDone` 时把 ctx 的错误取出来返回（"事务失败其实是 ctx 取消"这一常见误判被显式修正）。
- **panic 隔离**：三处明确的 recover：
    - 作业执行（`job/executor.go:exec`）：`panic` → 转成 job failure（带 stack）；
    - 作业调度主循环（`job/scheduler.go:Run`）：单次迭代里的 panic 不影响调度器存活；
    - 流消费者处理消息（`stream/memory_consumer.go:processMessage`）：panic → 转 error 走重试/丢弃。
- **去重与幂等**：多实例并发靠"全局锁 + 行级状态"，而不是内存锁——`lock.MutexManager` 的 `globalLock(ctx, ...)` 包住了 `processReadyJobs`、`createNecessaryJobs`、`runJob` 执行后的写回、`CancelJob`、`jobOverdue.Handle`（注释说明：`UpdateProgress` 只改单行所以不需要全局锁）。
- **锁的两种实现**：`lock/memory.go`（单机进程内）与 `lock/redis.go`（多实例）；`dbtx` 层面还有一层"sqlite 需要全局 RWMutex、postgres 不需要"的**驱动相关同步策略**（`store/database/dbtx/locker.go:needsLocking(driver)`，非 postgres 时所有 SQL 都过一把全局 RWMutex，写事务用写锁、只读事务用读锁）。
- **优雅关闭**：入口分步 + 超时（见 §2.1），作业侧额外提供 `WaitJobsDone(ctx)`；SSH/metric server 各自 `Shutdown(ctx)`。
- **流背压**：`stream.MemoryConsumer` 用容量 500 的消息队列 + 固定并发 worker（默认 2，上限 64）限流；`app/sse` 的 channel 容量 100 且发送用 `select/default` **丢弃而非阻塞**（"实时通知宁可丢，也不能拖住发布者"）。

### 4.2 agentxx：asio 协程 + io_context + CancelToken

- **并发单位**：`asio::awaitable<T>` 协程；多会话在同一 `io_context` 上交错执行（`BaseAgent::ioCtx` 注释强调："不要在同线程中传递到 `runCliAsync` 内使用，`engine->run_stream_async` 会启动其他 io_context，交替 ioCtx 会互相等待卡住"）。
- **取消**：`utilxx::CancelToken`（通用）与 `neograph::graph::CancelToken`（图引擎）两套抽象，经 `agentxx/util/cancel_adapter.h` 互通；`agentxx/util/exception.h` 把"取消导致的 `operation_aborted`"识别为取消语义而非传输错误（`isCancelAbort`），并把 neograph 的 `CancelledException` / `NodeInterrupt` 注册为通用库的追加分类器（`installExceptionClassifier`）。
- **异常分类**：`ExceptionClassification{isControlFlow, controlKind, errInfo, exPtr}` 统一区分"取消/中断/普通错误/未知"，上层据此决定是否重抛（`catchErrorAsync` 放行取消与中断）。
- **超时**：多为按子系统配置（HTTP 连接超时/分段读超时、MCP 工具超时、shutdown 超时），没有统一"每个工具调用都有硬超时"的机制——这是与 harness 作业 `MaxDurationSeconds + RunDeadline` 的明显差距。
- **线程模型**：agent 侧单线程 io 线程执行所有会话（"不需要线程锁"是设计目标）；阻塞型工作经 offload 线程池（`plugin_tool_sync.h` 适配器、`utilxx_base/async_offload.h`）；插件宿主对 `offload` 实现为**调用方内嵌存储**，随实例销毁释放。
- **TUI/客户端**：UI 线程与 agent 线程分离，经 transport 双向通信（进程内 `ChannelIOTransport` 或 WS）；`SessionServerAgentIO` 负责 delta 缓冲/重连重放。

### 4.3 对比与结论

| 维度 | harness | agentxx | 结论 |
|---|---|---|---|
| 并发原语 | goroutine + channel + ctx | 协程 + executor + CancelToken | 各自合适；agentxx 的优势是单线程模型天然无数据竞争 |
| 失败收敛 | `errgroup` 一处收敛全局 | 分散：一轮失败进 `TurnResult.hasError` | **建议迁移**：agent 内部后台任务（插件定时器/作业）应有统一错误汇聚点 |
| 超时预算 | 作业有 deadline；HTTP 有 server timeout | 子系统级超时，工具调用缺硬超时 | **建议迁移**：工具调用预算（见 §9/§11） |
| panic 隔离 | 三处显式 recover | 异常分类体系完备（比 recover 更细） | 各有侧重；agentxx 的分类器更适合"协作式取消" |
| 跨实例一致 | 全局锁 + 状态行 + pubsub | 不需要（单进程） | 不必迁移 |
| 驱动相关同步 | sqlite 全局 RWMutex / postgres 无锁 | 每连接互斥（`SessionStore::mutex_`） + WAL | agentxx 的做法更简单有效；可借鉴"多连接 LRU 上限"（已有） |
| 关闭 | 分阶段 + 超时 + 倒序 | 单一 `shutdownAsync(timeout)` | **建议迁移**（见 §2） |
| 背压 | 有界队列 + 固定 worker + 丢弃策略 | asio 队列天然背压；缺丢弃/限流策略 | **建议迁移**：客户端推送队列的"丢弃 vs 阻塞"策略应显式 |

**各自做得好的设计**

- harness：①`errgroup` 把"任一路径失败 → 全局收敛 → 统一关闭"做成一行代码；②`dbtx` 的"驱动决定是否需要锁"是一个诚实的工程折中，并在 `runnerTx` 里用 `commit/rollback` 标记避免重复回滚；③SSE 的 `select/default` 丢弃策略表明"实时通知允许丢"这一取舍被显式写下来。
- agentxx：①取消语义被建模成**控制流**而非错误（`ExceptionClassification.isControlFlow`），使取消能穿透所有 `catchError` 而不被吞；②`installExceptionClassifier` 让第三方库（http/ws）内部捕获时也认这套语义；③单线程会话模型 + 显式 offload 让"哪里会阻塞"必须写出来。

**可迁移设计（§4）**

1. **后台任务错误汇聚 + 失败即收敛（P1）**：引入 `agentxx::util::TaskScope`（语义对齐 `errgroup`）：注册后台任务（插件定时器、子代理、作业、TUI 动画），任一返回"致命错误"时取消其余并按依赖倒序关闭；非致命错误只记日志/暴露到 Info。
    - 落地：`agent/lib/include/agentxx/util/`（新增）+ `base_agent.cpp` 的 `shutdownAsync` + `agent/client/src/io/tui/agent_tui.cpp`。
2. **工具调用预算（P0）**：为每次工具调用引入 deadline（默认值可配，`0=不限`），超时后向模型返回"超时"结果而不是让整轮卡死；与 harness 作业 `MaxDurationSeconds` + `RunDeadline` + 超期回收同构，且 agentxx 已有 `CancelToken` 可承接。
    - 落地：`agent/lib/src/nodes/toolcall.cpp` + `agent/lib/include/agentxx/tools/tool.h`。
3. **有界推送队列 + 显式丢弃策略（P1）**：客户端 delta 推送（`SessionServerAgentIO`）与 TUI 事件队列应有容量上限与"满时丢弃可丢项（如 spinner/统计）但保留内容项"的策略，避免慢客户端把 agent 线程拖住。
    - 落地：`agent/lib/src/agent/io/session_server_agent_io.cpp`。
4. **关闭阶段化（P1）**：见 §2 建议 3。

---

## 5. API 层与协议

### 5.1 harness：REST + 中间件链 + 分类错误渲染 + 流式响应

**分层要点**（`app/api/`）：

1. **端点只编排**：`handler/<domain>/<action>.go` 里只有"取 session → 解析路径/分页参数 → 调 controller → render"。例：`handler/gitspace/events.go` 只有 40 行，序列是 `GetGitspaceRefFromPath` → `path.DisectLeaf` → `ParsePage/ParseLimit` → `ctrl.Events(...)` → `render.Pagination + render.JSON`。
2. **请求解码集中在 `request` 包**：`request.ParsePage`、`GetSpaceRefFromPath`、`GetGitspaceRefFromPath` 等函数把 HTTP 细节挡在业务之外；`app/api/request/*.go` 按域拆文件（40+ 个），每个域定义自己的"从路径/查询/体解码"函数。**编码修正中间件**（`middleware/encode/encode.go`）专门处理 git 与"以 `+` 结尾的路径"这类特殊编码（`EncodedPathSeparator = "%2F"`、`pathTerminatedWithMarker`），避免 `/space1/repo.git`、`/space/+` 被错误拆分——注释明确指出"git 请求只看后缀 + 方法"。
3. **输出集中在 `render`**：`JSON` / `Unprocessable` / `Violations` / `Reader`（流式原文）/ `JSONArrayDynamic`（动态 JSON 数组，边取边写）/ `StreamSSE` / `GitBasicAuth`。
4. **错误渲染分两轨**：
    - `app/api/usererror`：**用户可见错误**，结构 `{message, values?}` + 隐藏的 `Status`（`json:"-"`），内置 30+ 具名错误（`ErrNotFound` / `ErrDuplicate` / `ErrResourceLocked(423)` / `ErrNotMergeable(412)` / `ErrQuarantinedArtifact(403)`…），并且 `usererror.Translate(ctx, err)` 做本地化（`translate.go`）；
    - `errors` 包：库级分类错误（`InvalidArgument` / `NotFound` / `Conflict` / `PreconditionFailed` / `Status`），由 `render.TranslatedUserError` 统一映射到 HTTP 语义。
5. **流式响应**：
    - `render.JSONArrayDynamic[T]`：从 `types.Stream[T]` 逐条取数据（diff/commit 列表等大结果），首条数据到达才写头；出错时若已发数据则"只关数组不报错"，未发数据才返回错误——把"响应已开始就无法改状态码"这一 HTTP 约束显式处理掉；
    - `render.StreamSSE`：`text/event-stream` + `Flusher` 逐事件 flush + 30s ping 保活 + 2 小时 tail 上限 + `app shutdown` 时主动结束 + 结束发 `event: error\ndata: eof` 哨兵；`X-Accel-Buffering: no` 防 nginx 缓冲。
6. **分页一致**：`render.Pagination(r,w,page,limit,count)` 统一写分页响应头；`request.ParsePage/ParseLimit` 统一解析；存储层 `database.Limit(size)/Offset(page,size)` 统一换算 SQL 的 `LIMIT/OFFSET`（默认 100）。
7. **OpenAPI 生成**：`app/api/openapi/*.go` 按域拆 20+ 文件，`cli/operations/swagger/swagger.go` 提供 `gitness swagger` 子命令导出 `swagger.yaml` → 前端 `yarn services` 据此生成 TS 客户端（`web/restful-react.config.js`）。**契约是生成物**，前后端不会手写漂移。

### 5.2 agentxx：手写 wire 协议 + 事件流 + 端点抽象

- **端点抽象**：`AgentIOBase`（`agent/lib/include/agentxx/agent/io/agent_io.h`）是输入输出端点基类；实现有 stdio CLI、FTXUI TUI、`SessionServerAgentIO`（服务端驱动，含 delta 缓冲/重连重放）。两端经 transport 双向通信：进程内 `ChannelIOTransport`（线程间）或 `WsIOTransport`（跨进程）。
- **协议**：`agent/lib/include/agentxx/agent/io/wire_protocol.h`（1293 行）。约定 `{"type": ..., "id": <请求 id>, "sessionId": ..., ...payload}`，`MsgType` 常量表列出 **客户端→服务端 18 种**（hello / user_input / interrupt_response / cancel / select_model / get_model / get_append_component_info / ping / compact_context / list_sessions / switch_session / clear_message_queue / remove_queue_item / interrupt_and_run_next / get_view_messages / list_dir / get_permission_state / set_full_auth）与**服务端→客户端 21 种**（hello_ack / delta / sync / interrupt_request / interrupt_expired / turn_result / context_stats / error / log / model_info / append_component_info / get_context / context_messages / session_list / pong / plugin_data / plugin_data_up / message_queue_update / view_messages_page / list_dir_result / permission_state）；`WireDelta::Type` 11 种增量（text_token / thinking_token / tool_start / tool_end / turn_start / turn_end / node_start / node_end / message_tip / insert_message / update_message）。
- **握手与能力**：`hello` / `hello_ack` 携带设备 id、会话工作目录（远程模式下客户端据此决定是否显示"服务端文件"标签页）。
- **服务端端点职责重**：`session_server_agent_io.cpp`（1627 行）承担 delta 缓冲、断线重连重放、请求路由（`WireGetContext`/`WireListDir`/`WireGetPermissionState`…）。
- **事件与推送分离**：进程内 `EventBus`（agent 全局 + 会话级）承载语义事件（`event/events.h` 的 `Topic` 常量：`agent.turn.start`、`agent.model.token`、`agent.tool.start`、`io.display`、`service.permission`、`service.subagent`、`service.crossagent`、`service.token.count`…）；单向事件用 `EventStream<T>`，请求-响应用 `RequestResponseStream<TReq,TResp>`。
- **UI 描述层**（与协议平行）：`agent/lib/include/agentxx/ui/item.h` 定义组件 JSON（文本/差异/表格/树/键值/趋势图/计量条/控件…），由客户端的唯一渲染实现渲染；中断/overlay/Info/装饰共用同一套描述与渲染，并走能力协商（`agentxx.client.components` / `form` / `layout`）。

### 5.3 对比与结论

| 维度 | harness | agentxx |
|---|---|---|
| 接口定义方式 | 生成式（openapi → swagger → TS 客户端） | 手写常量表 + 手写 JSON 编解码（`wire_protocol.cpp` 584 行） |
| 端点分层 | handler（编排）/controller（业务）/store（数据） | IO 端点（渲染/交互）与 agent（循环/工具）分离；无中间"controller"层 |
| 错误输出 | 双轨分类 + 本地化 + HTTP 语义映射 | `error` 消息 + 文本；工具错误统一格式化为 `[Exception aborted: msg]` |
| 流式 | SSE + 动态 JSON 数组 + 原文流 | delta 增量 + sync 快照 + 重连重放 |
| 保活/上限 | 30s ping，2h tail 上限，关闭哨兵 | WS ping/pong + 重连；无统一"会话流上限"策略 |
| 分页 | 全局一致（ParsePage/Limit/Offset + 响应头） | 按需：会话列表用 keyset 游标（`listSessionsPage`）、历史用 `get_view_messages` 分页 |
| 能力协商 | 无（前端与后端同版本） | 有（组件/表单/布局能力名） |

**harness 的优点**：①**流式响应的边界条件被显式处理**（首条到达才写头、已写数据后只能关数组、SSE 结束哨兵、ping 与 tail 上限），这些正是长连接场景最容易漏的地方；②错误"用户可见 vs 内部"分轨 + 本地化，使同一个业务错误可以在 CLI/Web/git 三个入口呈现不同措辞；③协议契约生成化，杜绝前后端漂移。

**agentxx 的优点**：①**协议更贴合交互语义**（delta 11 种类型 + 重连重放 + 中断过期通知 `interrupt_expired`），这些是"人机协作中断"特有的，harness 没有对应物；②能力协商 + 声明式 UI 组件让"同一个 agent 服务多个客户端（TUI/未来 GUI）"无需改协议；③请求-响应事件（`service.permission` 等）把"向用户提问"建模成总线上的请求，比 HTTP 端点更贴合会话内交互。

**agentxx 的缺点**：①协议是手写常量表 + 手写解析，1293 行头 + 584 行实现需人工同步（新增消息漏改一处即静默失配）；②缺少统一的流控策略（delta 推送无上限/丢弃策略，见 §4）；③错误码不含"类别"，客户端只能按文本判断（例如"文件不存在"与"权限拒绝"无法区分）；④`sync`（快照）与 `delta` 的一致性**已有序号与尾哈希**（`delta.seq` / `TurnEnd.tailHash`，见 [§32 修正 1~2](#修正-1delta-序号与一致性校验不是缺失而是缺客户端侧使用)），但这些机制尚未变成显式契约、客户端也未据此校验。

**可迁移设计（§5）**

1. **错误码分类（P0）**：为 wire `error` 消息增加 `code` 字段（枚举：`INVALID_ARGS` / `NOT_FOUND` / `PERMISSION_DENIED` / `TIMEOUT` / `CANCELED` / `UPSTREAM` / `INTERNAL`）+ 可选 `values`，与 `agentxx::util::ExceptionClassification` 和工具错误的 `invalid_argument/runtime_error` 约定对齐；客户端据此显示不同提示与交互（例如权限拒绝时直接给"记住此选择"选项）。
    - 落地：`agent/lib/include/agentxx/agent/io/wire_protocol.h` + `agent/lib/src/nodes/toolcall.cpp`（错误 → code 映射）+ 客户端展示。
2. **delta 序号与快照版本（P1）**：给每轮 delta 加单调序号、给 `sync` 加"快照对应序号"，客户端可检测丢帧并按序号请求重放；等价于 SSE 的 `Last-Event-ID` 语义。
    - 落地：`SessionServerAgentIO` 的缓冲与 `WireDelta`/`WireSync` 编解码。
3. **协议表单一来源 + 生成校验（P1）**：把 `MsgType`/`WireDelta::Type` 的字符串与 C++ 类型做一次集中声明（如 X-macro 或描述文件），由它生成 `toString/fromString/字段映射`，并加一个单测"每个枚举值都能往返"（agentxx 已有 `test_remote_agent.cpp` 3408 行可扩展）。
    - 落地：`agent/lib/include/agentxx/agent/io/wire_protocol.h` + `agent/lib/src/agent/wire_protocol.cpp`。
4. **长流上限与保活（P2）**：为 WS 会话流引入"最大空闲/最大持续时长 + 保活间隔"，超限时优雅结束并让客户端重连（harness 的 30s ping / 2h tail 是合理起点）。
5. **分页口径统一（P2）**：会话列表已用 keyset，`view_messages` 分页已是游标；建议把"游标 = 上一页最后一条 + 取更早"的语义写成公共 helper 并补边界测试（新增/删除并发下的重复与遗漏）。

---

## 6. 持久化层

### 6.1 harness：接口 + 双驱动 + 事务抽象 + 迁移序列 + 错误翻译

**五个正交部件**：

1. **存储接口**（`app/store/database.go`，1766 行，纯接口）：按聚合拆分为 `PrincipalStore` / `RepoStore` / `SpaceStore` / `PullReqStore` / `JobStore` / `WebhookStore` / `GitspaceConfigStore` …；每个方法都收 `ctx` 第一个参数，返回 `(*T, error)`；读取方法区分 `Find`（单条）/`FindManyByX`（批量，用于消除 N+1）/`List`（带 filter）/`Count`（与 List 同 filter，保证分页总数一致）。
2. **SQL 实现**（`app/store/database/*.go`，60+ 文件）：`sqlx` 做结构体扫描（`db:"xxx"` 标签），`squirrel` 做条件构造（全局 `Builder = squirrel.StatementBuilder.PlaceholderFormat(squirrel.Dollar)`，「硬编码 postgres 占位符，因为 sqlite3 兼容」）。**读路径普遍先批量取依赖再拼装**（`FindManyByUID`），避免 N+1。
3. **事务抽象**（`store/database/dbtx`）：
    - `Transactor.WithTx(ctx, fn, opts...)` 把事务句柄通过 **ctx 传递**（`context.WithValue(ctx, ctxKeyTx{}, TransactionAccessor(rtx))`），因此"某方法是跑在事务里还是自动提交"对调用方透明（store 实现内部 `dbtx.GetAccessor(ctx, db)` 取当前 Accessor）；
    - `runnerDB` 对 sqlite 用全局 `sync.RWMutex`（只读事务 RLock、写事务 Lock、每条 SQL 也加锁），postgres 用 noop locker；
    - `runnerTx` 记录 `commit/rollback` 标记，避免 defer 里重复回滚；`sql.ErrTxDone` 时用 ctx 的错误替代（把"取消"语义还原）。
4. **迁移**（`app/store/database/migrate/`）：`//go:embed postgres/*.sql` + `//go:embed sqlite/*.sql` 两套 SQL，用 `maragudk/migrate` 顺序执行，`migrations` 表记录版本；**Before/After 回调**承载"SQL 表达不了的数据迁移"（如 `0039_alter_table_webhooks_uid`、`0153/0155/0173_migrate_artifacts`、`0160`），并在迁移期间临时关闭 sqlite 外键（`PRAGMA foreign_keys = OFF`，结束后恢复），`Current()` 支持查询当前版本；CLI 提供 `gitness migrate` / `migrate to <version>`（`cli/operations/migrate/*`）。
5. **错误翻译**（`store/database/util.go:ProcessSQLErrorf`）：把驱动错误统一映射为领域错误——`sql.ErrNoRows → store.ErrResourceNotFound`、唯一约束 → `store.ErrDuplicate`、外键 → `store.ErrForeignKeyViolation`（`util_sqlite.go` 同时处理 sqlite `sqlite3.Error.ExtendedCode` 与 pq `23505`）。上层业务只 `errors.Is(err, store.ErrDuplicate)`，与驱动无关。

**其他细节**：`PartialMatch` 统一处理 `LIKE` 通配符转义（`\`/`_`/`%`，必要时加 `ESCAPE '\'`，因为 SQLite 需要而 Postgres 不需要）；`EncodeToSQLXJSON` 把任意值转 JSON 文本存库；`Limit/Offset` 统一分页换算；`prepareDatasourceForDriver` 对 sqlite 强制 `_foreign_keys=on`；`pingDatabase` 带 30 次 ×1s 重试（容器启动时数据库可能尚未就绪）。

### 6.2 agentxx：轻量 RAII + 会话分库 + 幂等建表

- **驱动封装**：`agentxx::util::SqliteDb`（`agent/lib/include/agentxx/util/sqlite.h`，99 行声明 + 216 行实现）——RAII、move-only、`open` 时启用 `WAL` + `busy_timeout(5s)` + `synchronous=NORMAL`；`exec` / `prepare`（1-based 绑定、0-based 列）/ `bindInt64|Text|Null` / `step` / `reset` / `beginImmediate|commit|rollback` / `lastInsertRowid`；**所有失败抛 `std::runtime_error`**（带 sqlite3 错误文本），由 `catchError*` 分类；不持锁（单连接单线程）。
- **会话存储**：`SessionStore`（`agent/lib/src/agent/session_store.cpp`，847 行）按 sessionId 分目录 `{root}/{sanitizedSessionId}/session.db`，单库四张表：`view_message`（展示历史，append-only，每消息一行 JSON，另有 `updateViewMessage` 用于工具结果回填）、`llm_context`（上下文整体替换，每轮末保存）、`meta`（msgIdCounter / title / lastActiveMs）、`store`（share store KV，id 自增）。
    - **并发**：`std::mutex` 保护所有访问（"常规使用下调用发生在 agent io 线程，锁仅在多线程并发访问时生效"）；
    - **连接管理**：`kMaxOpenSessionDbs = 32`，超出按 LRU 关闭最久未用连接（避免 fd 耗尽，Linux `ulimit -n` 常为 1024）；
    - **schema 演进**：`ensureSchema`（`CREATE TABLE IF NOT EXISTS`）+ `ensureViewMessageMsgIdColumn`（检测列不存在则 `ALTER TABLE` + 回填），即**幂等 DDL + 手写小迁移**；
    - **读写分离的目录语义**：读路径下"目录不存在"直接返回空（不为只读访问创建空库）；写路径才创建。
- **会话列表分页**：`listSessionsPage(beforeMs, beforeId, limit)` 两阶段（先 stat 各库 mtime 排序，再逐个打开读 meta），游标语义"严格位于 (lastActiveMs, sessionId) 之后"，注释明确讨论了 mtime 与 meta 不完全一致时"只用 mtime 定顺序、绝不用它跳过目录"的边界。
- **全局 KV**：`settings_db`（130 行）存界面/全局设置。
- **没有的东西**：迁移版本表、迁移序列、事务上下文传递、SQL 错误分类、批量查询原语（`FindManyByX` 式）、只读事务语义。

### 6.3 对比与结论

| 维度 | harness | agentxx |
|---|---|---|
| 抽象 | 接口 + 实现分层，驱动无关 | 直接调用封装类 |
| 驱动 | sqlite3 / postgres 双驱动，同一套 SQL 构造器 | sqlite3 单驱动 |
| 事务 | ctx 传递 + 全局锁策略 + 回滚幂等 | 显式 `beginImmediate/commit/rollback` |
| 迁移 | 版本表 + 有序 SQL + Before/After 数据迁移 + CLI | 幂等 DDL + 手写 ALTER，无版本表 |
| 错误 | SQL 错误 → 领域错误（Not Found / Duplicate / FK） | 抛异常 + 文本 |
| 分页/搜索 | 统一 Limit/Offset + LIKE 转义 | keyset 游标（会话列表） |
| 连接管理 | 连接池（25 开 / 5 闲 / 5min 寿命） | 每会话连接 + LRU 上限 32 |
| 多写者 | 支持多实例（全局锁 + 行状态） | 单进程单写者（互斥锁 + WAL） |
| 存储粒度 | 单库多表（数十张表） | **每会话一个库**（天然隔离、便于备份/删除） |

**harness 的优点**：①**迁移序列 + 版本表**是长期演进项目的必需品；②`dbtx` 用 ctx 传递事务句柄，业务代码无须感知自己在事务内；③SQL 错误分类翻译使业务层不依赖驱动；④`FindManyByX` 批量接口把 N+1 从"容易犯"变成"默认避免"；⑤`PartialMatch` 这类细节（LIKE 转义 + ESCAPE 子句差异）在库层统一解决。

**agentxx 的优点**：①**每会话一个库**在本地场景优势明显——删除会话 = 删目录、备份/迁移单会话 = 拷文件、会话并发互不干扰，且"第一次访问只取 `max(id)` 不读全部条目"的设计把内存占用与会话数解耦；②连接 LRU 上限显式，避免 fd 耗尽（这是本地长期运行的常见故障）；③幂等 DDL 让"老库直接打开即可用"互不阻塞；④`SettingsDb` 让全局设置与会话数据分离。

**agentxx 的缺点**：①**无版本表/迁移序列**：schema 演进靠 `if (!hasColumn)` 猜测，多步迁移（改类型、数据回填、索引重建）无法安全表达，也无法"降级到版本 X"排障；②错误只有文本，`store.ErrDuplicate` 这类可判定语义缺失（现在靠异常类型 + 文本），不利于上层分支（如"会话已存在"）；③连接 LRU 关闭后 WAL 文件与 checkpoint 时机没有显式管理（`SqliteDb::close` 是否 `wal_checkpoint` 需确认）；④没有"只读事务"概念，读多写少场景下 `mutex_` 串行化所有访问（本地场景可接受，但会话列表扫描这类批量读会阻塞写入）。

**可迁移设计（§6）**

1. **迁移框架（P0）**：
    - 新增 `agent/lib/include/agentxx/util/migration.h`：`struct Migration { uint32_t version; std::string_view name; void(*fn)(SqliteDb&); }` + `runMigrations(db, migrations, &appliedVersion)`；
    - `session.db` 增 `schema_meta(key TEXT PRIMARY KEY, value TEXT)` 存 `schema_version`；启动时对比并顺序执行；`ensureSchema` 与 `ensureViewMessageMsgIdColumn` 合并为 `version 1/2` 两个迁移；
    - 保留"老库首次迁移前自动备份 `session.db.bak`"作为安全网（本地数据无 DBA，用户不会手工恢复）。
    - 落地：`agent/lib/include/agentxx/util/sqlite.h`（新增声明）+ `agent/lib/src/util/`（新增实现）+ `session_store.cpp` + `settings_db.cpp`。
2. **领域错误映射（P1）**：`SqliteDb` 在错误上附加分类（如 `SqliteError{kind: NotFound|Duplicate|Constraint|Busy|Io, code, message}`），`SessionStore` 对外暴露 `store::ErrResourceNotFound` 式枚举，便于上层写明显分支（如"会话不存在则新建"与"繁忙则重试"）。
    - 落地：`agent/lib/src/util/sqlite.cpp` + `session_store.cpp`。
3. **连接关闭前的 checkpoint（P0 复核项）**：关闭会话库前显式 `PRAGMA wal_checkpoint(TRUNCATE)`（或确认 `sqlite3_close` 已做），避免 `-wal` 文件长期堆积与"拷贝目录得到不一致快照"。会话删除/导出路径必须走 checkpoint。
    - 落地：`agent/lib/src/util/sqlite.cpp:close` + `session_store.cpp` 的 LRU 淘汰与删除路径。
4. **批量读取原语（P1）**：为多会话场景提供 `loadSessions(ids)` 之类的批量接口（单库多语句/多库循环 + 合并返回），避免上层循环单查。
5. **只读访问路径（P2）**：为"只读查询"（历史分页、会话列表）使用独立连接或 `PRAGMA query_only`，避免与写入争用同一把互斥锁。

---

## 7. 缓存与失效

### 7.1 harness：缓存装饰器 + 事件驱动失效

- **抽象**（`cache/cache.go`）：
    - `Cache[K,V]`：`Stats() (hit, miss)` / `Get(ctx,key)` / `Evict(ctx,key)`；
    - `ExtendedCache[K,V Identifiable[K]]`：再加 `Map(ctx, keys) (map[K]V, error)`（批量取，内部逐键走 `Get`，命中则不算 miss）；
    - `Getter[K,V]` / `ExtendedGetter[K,V]`：缓存只依赖"能按 key 取值"的接口，**因此缓存是装饰器而不是子系统**——`spaceIDCacheGetter{spaceStore}` 就是一层薄适配。
- **实现**（`cache/`）：
    - `LRUCache`（`hashicorp/golang-lru/v2/expirable`，容量 + TTL，`countHit/countMiss` 原子计数，`Get` 未命中即回源并写入）；
    - `TTLCache`、`NoCache`（测试/关闭用）、`RedisCache`（多实例共享）；
    - `cache_test.go` / `no_cache_test.go` / `redis_cache_test.go` 三者共用同一套行为断言（实现可替换性的测试保障）。
- **谁在用**（`app/store/cache/`）：`PrincipalInfoCache`（30s）、`SpaceIDCache`（15min）、`SpacePathCache`（15min）、`SpaceCaseInsensitiveCache`（15min）、`RepoIDCache`（15min）、`RepoRefCache`（15min，key = `types.RepoCacheKey`）、`InfraProviderResourceCache`（5min）。这些正好是**"每个请求都要解析、但极少变化"的引用消解数据**（把 `space/path`、`repo ref` 解析为 ID），属于最典型的缓存收益点。
- **失效（关键设计）**：`app/store/cache/evictor.go`
    - `Evictor[T]{nameSpace, topicName, bus pubsub.PubSub}`：`Evict(ctx, key)` 把 key **gob 编码**后 `Publish` 到 topic；`Subscribe(ctx, fn)` 解码后调用 `fn`；
    - 因此"谁改了数据 → 广播 key → 所有实例（含多副本）各自失效"，不依赖共享缓存；
    - 具体策略写在构造函数里，且有注释解释权衡：`NewSpaceIDCache` 失效时直接 `EvictAll`（"更新 space core 是罕见操作，与其在缓存里找子空间不如整表清空"）；
    - `Evictor` 在 `bus == nil` 时退化为 noop，因此不配 pubsub 也能跑（只是本地 TTL）。
- **多级/热路径缓存**：git 的 last-commit cache 有 `inmemory/redis/none` 三种模式（`types/config.go:Git.LastCommitCache`）；`git/enum/last_commit_cache.go` 决定实现。
- **可观测**：`Stats()` 暴露命中率，便于接入指标。

### 7.2 agentxx：按需缓存 + 已有边界意识

- **数据层缓存**：
    - `SessionShareStore`：内存只保留 `kCacheCapacity = 3` 条 LRU，首次访问某会话只取回 `max(id)` 作为自增计数，"内存占用与条目数无关"；
    - `SessionStore`：打开连接 LRU 上限 `kMaxOpenSessionDbs = 32`（fd 保护）。
- **渲染层缓存**（TUI，`agent/client/src/io/tui/`）：
    - `lazy_scrollable.h`：`cacheable` 标记 + `CacheBudget` + `hasCache_` 逐项缓存渲染结果，只对可见区域渲染；
    - `message_list.h:invalidateCache()`：消息变更时整表失效，注释指出"cacheable 处理下的旧帧快照不会随动画推进更新"——即**动画与缓存互斥**的已知边界；
    - `agent_tui.h`：`logLineCache_` + `logCachePoppedCount_` + `logCacheLineCount_`（日志行增量缓存）；
    - `overlays.h`：`cachedMermaid_/cachedMaxW_/cachedThemeName_/cachedDiagram_/cachedElement_`（按"内容 + 宽度 + 主题"三元组失效的 markdown/mermaid 渲染缓存）。
- **没有的东西**：跨端点（远程客户端 vs 服务端）缓存一致性机制；缓存命中率统计；统一 TTL 抽象。

### 7.3 对比与结论

| 维度 | harness | agentxx |
|---|---|---|
| 抽象层次 | `Cache`/`ExtendedCache` 接口 + 多实现 + 装饰器工厂 | 各子系统自持缓存（LRU / 逐项 Element） |
| 失效 | 事件广播（pubsub）+ TTL + EvictAll 策略 | 显式 `invalidateCache()` / 键变化失效（内容+宽度+主题） |
| 命中率统计 | 有（`Stats()`） | 无 |
| 可关闭 | `NoCache` 实现（测试/排障） | 无开关 |
| 批量取 | `Map(keys)` | 无（逐条） |
| 缓存边界显式性 | 注释中写明"罕见更新 → 整表清空" | 注释中写明"动画与缓存互斥"、"mtime 只定顺序不跳过目录" |

**harness 的优点**：①**缓存是装饰器**——同一份 store 接口可以在 wi 装配时插一层缓存，业务代码不变；②失效走事件广播，**多实例/多进程天然一致**；③`Stats()` 让"缓存是否有效"可度量，避免"加了缓存更慢"；④`NoCache` 让排障可一键对比。

**agentxx 的优点**：①缓存的**边界条件被写进注释**（动画/宽度/主题/命中范围），这类隐式约束在渲染层极易踩坑，写明即是资产；②`lazy_scrollable` 的逐项缓存 + 视口渲染是 TUI 长列表的关键优化，harness 侧对应物是 Web 虚拟滚动（不同技术栈，不可直接比较）；③"只取 max(id) 不读全量"体现了对内存占用的显式取舍。

**agentxx 的缺点**：①没有统一抽象，新增缓存要重复实现（容量/淘汰/统计/关闭）；②**远程模式下的双端缓存一致性**没有专门机制（服务端改了某会话状态，客户端缓存靠 delta/sync 被动刷新，一旦丢帧就只能靠重连）；③缺命中率统计，无法判断"要不要加缓存"。

**可迁移设计（§7）**

1. **缓存抽象下沉到 `utilxx_base`（P2）**：`utilxx_base` 已有 `lru_cache.h`，可补 `Cache<K,V>` 概念 + `TTL/无缓存` 变体 + 命中/未命中计数，统一 `share_store`、会话连接表、TUI 渲染缓存的使用方式。
    - 落地：`agent/third_party/cxx_utilxx_base/include/utilxx_base/`（新增）+ 各处替换。
2. **客户端缓存一致性（P1）**：结合 §5 建议 2 的 delta 序号：服务端在 `sync` 中下发快照版本，客户端为关键缓存（消息列表、历史分页窗口）记录版本，序号不连续即整块失效重拉；等价于 harness 的 Evictor，但走已有 delta 通道而非额外总线。
    - 落地：`agent/lib/src/agent/io/session_server_agent_io.cpp` + `agent/client/src/io/tui/components/message_list.cpp`。
3. **缓存统计暴露到 Info/诊断（P2）**：把 TUI 渲染缓存命中率、`share_store` 缓存命中率、会话库连接复用率写入 Info 面板与 `WireContextStats` 同级消息，便于在"卡顿/内存增长"类问题上快速定位。
4. **可关闭开关（P2）**：为渲染缓存提供"禁用缓存"的隐藏设置（参考 `NoCache`），用于确认是否为缓存导致的显示异常。

---

## 8. 事件与消息

### 8.1 harness：事件流（持久化 + 消费者组）+ pubsub（实时广播）+ SSE（推客户端）

**三层消息机制被明确区分**（这是本模块最值得学的结构）：

| 机制 | 语义 | 实现 | 典型用户 |
|---|---|---|---|
| `events`（事件流） | **业务事件，可靠、可重放、多消费者独立消费、有重试** | `stream` 抽象之上的"分类 + 类型 + 消费者组 + 处理器" | webhook、trigger、notification、keywordsearch、repoactivity、branch、instrumentation、gitspace 事件服务 |
| `pubsub` | **实时广播，不保证送达/不重放** | 内存 / Redis 发布订阅 | 缓存失效（Evictor）、作业状态变化、作业取消、SSE 事件源 |
| `app/sse` | **面向浏览器的推送** | 基于 pubsub 的 topic（`spaces:<id>`）+ HTTP SSE 渲染 | 空间事件面板、Gitspace 事件 |

**事件流设计要点**（`events/*`, `stream/*`）：

- **命名**：`getStreamID(category, eventType) = "events:<category>:<type>"`——每个"分类 + 事件类型"是一个独立流，消费者按需注册。`app/events/<domain>/` 为每个域定义 `Reader`（封装 `ReaderRegisterEvent[T]` 的类型化注册方法）+ `Reporter` 包装（如 `pullreqevents`、`gitevents`、`gitspaceevents`）；服务只需声明自己关心的事件。
- **载荷**：`Event[T]{ID(uuid v7), Timestamp, Payload T}`，gob 编码后放进 stream payload 的 `"event"` 键。uuid v7 保证**时间有序**（便于按 ID 排序/分页）。
- **消费者组**：`ReaderFactory[R].Launch(ctx, groupName, readerName, setup)`：
    - `groupName` 决定"谁和谁竞争同一条消息"（同组内轮询、不同组各拿一份）——例：`gitness:webhook`、`gitness:trigger`；
    - `readerName` 是实例标识（`config.InstanceID`，见 §3 的主机名清洗）；
    - `setup(reader)` 里 `Configure(WithConcurrency, WithHandlerOptions(WithIdleTimeout, WithMaxRetries))` 再逐个 `RegisterXxx(handler)`。
- **重试/idle 语义**（`stream/memory_consumer.go` + `stream/options.go`）：
    - 处理中若超过 `idleTimeout`（默认 1min，最小 5s）未 ack，消息被**重新入队**；处理成功则取消重试定时器；
    - 超过 `maxRetries`（默认 2，上限 64）→ 丢弃并上报错误；
    - `concurrency` 默认 2、上限 64（`WithConcurrency` 参数越界直接 panic，属于"配置错误就早失败"的姿态）；
    - 错误与信息各走独立 channel（`Errors()/Infos()`），`ReaderFactory` 起两个 goroutine 消费并转日志 + 计数（`Collector.RecordError/RecordDiscard`）。
- **业务可主动丢弃**：`events.NewDiscardEventError(inner)` 让 handler 表达"这条事件处理方法即使失败也不要重试"（`reader.go` 检测 `errDiscardEvent` 后返回 nil）；这是"毒消息"问题的显式出口。
- **可替换后端**：`events/wire.go:ProvideSystem` 按 `config.Mode` 在 `stream.MemoryBroker` 与 Redis Streams 之间选择，**业务代码零感知**（只在装配层切换）；内存实现保留 `MaxStreamLength` 上限以模拟 Redis 的 `MAXLEN` 裁剪。
- **持久化**：Redis 模式下事件本身存于 Redis Stream（`MaxStreamLength/ApproxMaxStreamLength` 控制裁剪），因此**消费者重启后能续消费**；内存模式退化为"进程内可靠"。

**pubsub 设计要点**（`pubsub/*`）：`Publisher.Publish(ctx, topic, payload, opts...)` + `PubSub.Subscribe(ctx, topic, handler, opts...) Consumer`；`Consumer{Subscribe, Unsubscribe, Close}` 生命周期显式；命名空间选项（`WithPublishNamespace`/`WithChannelNamespace`）避免不同用途串频道；内存实现与 Redis 实现同接口。

### 8.2 agentxx：强类型事件总线 + 请求-响应通道 + 事件桥

- **强类型总线**（`agent/lib/include/agentxx/event/event_stream.h`）：
    - `EventStream<T>`：`subscribe(handler, execHit)`（`execHit>0` 为有限次订阅，到期自动移除）/ `unsubscribe(id)` / `hasListeners()` / `publish(data)`；发布**顺序派发**，单个订阅者抛异常被捕获记录、不影响其他订阅者；有限订阅的计数递减**放在派发之后**（注释解释：若在派发前递减，中途取消会导致"少执行一次就消失"）。
    - `RequestResponseStream<Req,Resp>`：注册服务端（`std::function<awaitable<Resp>(req, correlationId)>`）+ `request()` 返回 `std::expected<Resp,std::string>`；用于权限询问、中断、子代理、跨 agent 查询、token 计数等。
    - `TimerEventStream`：定时事件流（周期/延迟），带 `handler(shared_ptr<T>, data)` 形态。
    - `EventBus`：按 topic 字符串注册/查找流（`publish/request/hasListeners/registerService/unregisterService`），另有**前缀订阅**（`PrefixSub`，用于插件事件转发等）。
- **类型安全**：`EventStreamInterface` 保存 `typeid`，`EventBus::get/getRR` 校验类型一致，防"同 topic 不同类型"的 UB。
- **topic 命名规范**：`<scope>.<subject>.<detail>`，scope ∈ {agent, service, subagent, io}（`event/events.h:Topic`）：
    - 单向：`agent.turn.start/end`、`agent.model.start/token/end`、`agent.tool.start/end`、`io.display`、`io.user_input`、`io.cancel`、`agent.error`、`subagent.progress`；
    - 请求-响应：`service.interrupt`、`service.permission`、`service.subagent`、`service.crossagent`、`service.permission.check`、`service.subagent.execute`、`service.token.count`；
    - 单向控制：`service.permission.set_isolation` / `clear_isolation` / `full_auth`、`service.summarization.compact`。
- **桥接**：`EventBridge`（`event_stream.h:587`）把图引擎的 `GraphEvent` 转成 `WireDelta` 推给客户端（含 TPS 推送节流 `setTpsPushInterval`、思考段合并 `finalizeThinkSegment`、`handleChannelWrite` 处理通道写事件）。
- **通道写入 → 会话消息**：消息写入方先写会话（权威），再发 `{"channel":"messages","value":[...]}` 的 CHANNEL_WRITE 事件；`EventBridge` 只做 UI 展开 + 节流落盘，不再追加消息。
- **跨 agent**：`AgentHost`（`agent/lib/src/agent/agent_host.cpp`，1010 行）持有多个 agent（含子代理），经 `service.crossagent` 应答查询。

### 8.3 对比与结论

| 维度 | harness | agentxx |
|---|---|---|
| 事件抽象 | 事件流（分类+类型+消费者组）、pubsub、SSE 三层 | 强类型 `EventStream<T>` / `RequestResponseStream` 一层 |
| 可靠性 | Redis 模式跨进程持久化 + 重试 + idle 重投 + 丢弃计数 | 进程内、无重试、无持久化（消息即内存态） |
| 消费者隔离 | 消费者组（同组竞争 / 跨组各一份）+ 多实例 reader name | 订阅者列表（每个订阅者都收到），无组语义 |
| 请求-响应 | 无（HTTP 语义承担） | **原生支持**（`service.permission` 等），带关联 id |
| 类型安全 | Go 泛型 handler 注册（`ReaderRegisterEvent[T]`），编译期类型绑定 | 模板类 + `typeid` 运行期校验 |
| 失败姿态 | 重试 N 次 → 丢弃 → 记指标 | 订阅者异常被吞（记录日志），发布不回滚 |
| 后端可替换 | memory / redis（装配层切换） | 仅进程内 |
| 命名规范 | `events:<category>:<type>` + 组名常量 | `<scope>.<subject>.<detail>` |
| 主动丢毒消息 | `NewDiscardEventError` | 无（异常即吞） |
| 派生：事件→UI | SSE（投影到浏览器） | `EventBridge` → `WireDelta`（投影到客户端） |

**harness 的优点**：①**三层消息机制各司其职**，避免了"用一个总线干所有事"的常见混乱——可靠业务事件、实时广播、UI 推送分开；②消费者组 + 重试 + idle 重投 + 丢弃计数，构成可运维的事件处理口径；③事件载荷带 uuid v7（时间有序）；④`NewDiscardEventError` 给"毒消息"一个显式出口；⑤后端可替换（内存/Redis）而业务零改动。

**agentxx 的优点**：①**请求-响应事件**是 harness 完全没有的能力，正好对应"agent 需要向用户提问/向子代理委派/跨 agent 查询"这类交互，比"HTTP 回调"更贴合会话内语义；②强类型 + `execHit`（有限次订阅）+ `unsubscribe(id)` 让"一次性的等待者"表达干净（如等待工具结束通知）；③`hasListeners()` 让高频发布方（每 token）可以零成本短路；④订阅者异常隔离，一个中间件出错不影响其他订阅者。

**agentxx 的缺点**：①事件**不持久化、不重试**——中途丢一次（例如客户端重连）就永久丢失，只能靠 `sync` 快照兜底；②没有"消费者组/游标"概念，多订阅者都要自行处理"我是否已经处理过"；③发布顺序与并发：`publish` 顺序派发是安全的，但 `RequestResponseStream::request` 用 `co_spawn` 真并发，未定义"多个服务端注册时选谁"的确定性（需确认实现）；④没有事件丢弃/错误计数上报（无 `Collector`）。

**可迁移设计（§8）**

1. **事件处理口径（P1）**：为需要"至少一次"语义的事件（工具结果回填、会话落盘、插件订阅的外部事件）引入 `EventBus` 的**可选持久化订阅**：`subscribePersistent(topic, key, handler)`，记录游标（复用 sqlite），失败按 idle 超时重投、超过重试上限记入"丢弃计数"并写日志/Info。
    - 落地：`agent/lib/include/agentxx/event/event_stream.h` + `agent/lib/src/event/event_stream.cpp`（新增游标存储，复用 `SqliteDb`）。
2. **毒消息显式出口（P2）**：为订阅处理器增加"不要重试/丢弃该事件"的表达（等价 `NewDiscardEventError`），避免一条坏消息被反复重试。
3. **事件计数与错误上报（P2）**：在 `EventBus` 层统计每 topic 的发布数、失败数、丢弃数，暴露到 Info/诊断与基准程序（对齐 harness 的 `Collector`）。
4. **服务端注册确定性（P2）**：明确 `RequestResponseStream` 多服务端注册时的选取规则（或禁止重复注册并给出错误），避免竞态。
5. **事件投影统一（P1）**：`EventBridge` 目前直接产出 `WireDelta`；建议把"内部事件 → 客户端展示项"的映射与"内部事件 → 纯文本/plainText 降级"统一到 UI 描述层（`agentxx/ui/item.h`）已有路径上，新增事件只需补一处映射（对齐 harness 的 SSE 投影由 `sse.Event{Type, Data}` 单点定义）。

---

## 9. 后台作业与调度

### 9.1 harness：把"后台作业"做成数据表 + 状态机

**作业包结构**（`job/`，16 个文件，无测试外依赖）：

| 文件 | 职责 |
|---|---|
| `definition.go` | 提交侧输入：`{UID, Type, MaxRetries, Timeout, Data}`，`Validate()` 校验 Type/UID/超时不小于 1s |
| `types.go` | 作业行：`UID/Created/Updated/Type/Priority/Data/Result/MaxDurationSeconds/MaxRetries/State/Scheduled/TotalExecutions/RunBy/RunDeadline/RunProgress/LastExecuted/IsRecurring/RecurringCron/ConsecutiveFailures/LastFailureError/GroupID` |
| `enum.go` | `State`（scheduled/running/finished/failed/canceled）+ `Priority` + 状态流转辅助 |
| `store.go` | 作业表接口：`Create/Find/Upsert/ListReady(now, limit)/CountRunning/NextScheduledTime/UpdateExecution/UpdateProgress/DeleteByUID/DeleteByGroupID/ListByGroupID` |
| `scheduler.go` | 771 行：调度主循环 + 容量控制 + 超期 + 取消 + 优雅停止 |
| `executor.go` | 按 `Type` 注册 `Handler{Handle(ctx, input, ProgressReporter) (result, err)}`；`finishRegistration()` 之后拒绝再注册 |
| `job_overdue.go` / `job_purge.go` | 调度器自带的两个内建作业：超期回收（每 20 分钟）与旧作业清理（保留期） |
| `timer.go` | 可"提前唤醒"的定时器（`RescheduleEarlier/ResetAt(deadline, edgy)`） |
| `pubsub.go` | 作业状态变化广播（topic + `StateChange` 载荷） |
| `lock.go` | `globalLock(ctx, mutexManager)`：全局锁封装 |

**调度循环的关键设计**（`scheduler.go`）：

1. **无忙等**：主循环阻塞在 `timer.Ch()` 或 `signal`（提前唤醒）或 `ctx.Done()`；每轮结束用 `ResetAt(nextExec, edgy)` 重排。`edgy` 表示"上一轮没跑完所有就绪作业"，此时任一作业结束都会触发 tick（因为有容量空出来了）。
2. **容量**：`maxRunning`（配置 `BackgroundJobsMaxRunning`）与 `CountRunning(ctx)` 比较得到空位数，`ListReady(now, available+1)` 多取一条用于判断"是否取全了"（取全才需要算 `NextScheduledTime`，否则立刻重跑）。
3. **先落库再执行**：`preExec(job)` 设置 `State=running/RunBy=instanceID/RunDeadline/TotalExecutions++/Progress=0/Result=""`，`UpdateExecution` 成功后才 `publishStateChange` + `runJob`（后台 goroutine）。这保证了"崩溃后 `RunBy` 与 `RunDeadline` 留下痕迹"，配合 `jobOverdue` 可回收。
4. **执行超时**：`context.WithDeadline(ctx, RunDeadline)`，把 deadline 交给 Handler；Handler 的约定是"尽量遵守 ctx，尽快中止"。
5. **结束与重排**（`postExec`）：失败 → `ConsecutiveFailures++`、`State=failed`、`LastFailureError`；成功 → `finished`。**周期性作业**用 `cronexpr.Next(now)` 重排（cron 解析失败 → 作业判失败并记原因）；**一次性失败作业**在 `ConsecutiveFailures <= MaxRetries` 时按固定 15s 延迟重排。
6. **进度上报**：`ProgressReporter(progress, result)` 写入单行（`UpdateProgress`）+ 广播状态变化；注释明确"改单行不需要全局锁"。
7. **取消**：`CancelJob` 先改库（`State=canceled`）再取消 ctx；若本实例不在跑（`cancelJobMap` 未命中）则**经 pubsub 广播**，由持有该作业的实例取消（多实例正确性）。
8. **回写用 `context.Background()`**：作业执行完后必须更新库，"即使作业 ctx 已取消或正在关机"（代码里显式用 background ctx 并加 lint 豁免）——这是"清理路径不能依赖可能已取消的 ctx"的典型处理。
9. **超期回收**（`job_overdue.go`）：扫描 `ListDeadlineExceeded(now)` 的作业，用 `postExec(job, "", "deadline exceeded")` 复用同一套状态机决定"是否重排/重试"，再按最小 scheduled 时间唤醒调度器。注释写明触发场景："DB 写回失败、或进程在作业运行时突然终止"。
10. **优雅停机**：`WaitJobsDone(ctx)` 等待所有在跑作业结束，受外部超时约束。
11. **分组**：`RunJobs(ctx, groupID, defs)` 一次提交多个作业并共享 `GroupID`（更省锁），配套 `GetJobProgressForGroup` / `PurgeJobsByGroupID`——批量作业的进度聚合与清理。

### 9.2 agentxx：长任务靠"会话内协程 + 委派"表达

- **子代理委派**（`tools/subagent.h`, `middlewares/subagent_manager.h`, `agent/lib/src/tools/subagent.cpp` 523 行）：
    - `SubAgentManagerTool`（模型可见工具 `agentxx_subagent`）支持**批量任务**（`{tasks: [...]}`），由 `AgentHost` 派生独立 agent 运行（"中断委派，不再使用图内嵌套 subgraph"）；
    - 级联取消：`ReqSubagentBatch.cancelToken` 让父取消级联中止全部在跑子代理；
    - 嵌套深度预算按会话记录（`sessionDepth_`）；
    - `agentxx::agent::AgentConfig::enableSubagent` 控制是否向模型注入该工具（关闭时事件总线服务仍注册，供上下文压缩等内部路径使用）；
    - 进度观测：`subagent.progress` 单向事件。
- **上下文压缩**：`summarization.cpp`（1265 行）会调用子代理执行摘要（经 `service.subagent.execute`），属于"内部长任务"。
- **定时与周期性工作**：
    - 插件侧：`agentxx.client.timer` 表（一次性/周期定时器，`pause_when_hidden` 区域不可见时顺延、动画等级 Disabled 时拒绝注册、插件禁用/卸载自动取消）；
    - 客户端：启动延迟 3 秒的更新检查（`update_check.cpp`）、TUI 动画/轮询定时器；
    - 会话侧：无"定时触发一轮"的机制。
- **阻塞工作**：`utilxx_base/async_offload.h` 的卸载线程池 + 插件侧 `plugin_tool_sync.h` 适配器（调用方内嵌存储，随实例销毁释放）。
- **缺的东西**：作业持久化、进度持久化、超时预算、超期回收、跨重启续跑、作业分组与批量进度、取消跨实例广播（单进程不需要，但**跨重启**需要）。

### 9.3 对比与结论

| 维度 | harness | agentxx |
|---|---|---|
| 作业定义 | 数据表行（可重启续跑） | 会话内协程（进程内） |
| 触发 | 立即 / 定时（cron）/ 周期 | 工具调用、事件、定时器（插件） |
| 状态机 | scheduled→running→finished/failed/canceled（落库） | 无（只有内存态） |
| 超时 | `RunDeadline` + deadline ctx | 工具/子代理各自超时（无统一预算） |
| 重试 | 一次性作业固定 15s 延迟重试（≤MaxRetries）；周期作业按 cron 重排 | 无自动重试（靠模型重试） |
| 进度 | 0~100 + result + 广播 | 事件流（`subagent.progress`）临时上报 |
| 崩溃恢复 | 超期回收作业 + RunBy/RunDeadline | 无（进程退出即丢；会话数据靠 sqlite 落盘） |
| 容量控制 | `maxRunning` + CountRunning | 无（并发由用户/模型驱动） |
| 取消 | 本地 ctx + pubsub 广播 | 级联 CancelToken（父子会话内） |
| 批量 | GroupID + 组进度 + 组清理 | 批量 tasks 数组（无组进度查询） |
| 阶段化停机 | `WaitJobsDone` | 无 |

**harness 的优点**：①**作业是数据**——重启后续跑、可查询、可清理、可分组；②状态机 + 超期回收 + 保留清理三件套构成"无人值守也能自愈"；③统一 `ProgressReporter` 让长任务对 UI 可观测；④取消走"先改库再取消 + 广播兜底"，多实例正确；⑤回写用 background ctx 这类细节被显式写在代码里。

**agentxx 的优点**：①子代理是**真 agent**（独立上下文、独立系统提示、可带工具），比"作业"表达力更强，且支持批量与级联取消；②进度经既有事件总线推送，客户端无需额外订阅机制；③插件定时器考虑了"区域不可见时顺延"和"动画等级关闭"这类**界面语境**，是服务端项目不会有的细致取舍。

**agentxx 的缺点**：①**长任务不可恢复**：一个跑了 10 分钟的子代理/命令，进程重启后全部消失且无痕迹（既不能续跑，也不能告诉用户"曾有一个未完成任务"）；②没有统一超时预算，工具/命令/子代理各自为政，容易出现"某个工具把一轮卡死"；③没有作业容量上限，批量 subagent 可能瞬时打出几十个并发会话（每个都带 LLM 请求）；④没有"作业列表"可观测面（客户端无法查看"后台任务/历史任务"）。

**可迁移设计（§9）**

1. **持久化后台作业表（P0）**：新增 `job` 表（建议放在全局库 `settings_db` 同库或独立 `jobs.db`），字段对齐 harness 精简版：`uid/type/created/updated/state/scheduled/deadline/run_by/progress/result/error/attempt/max_attempts/group_id/data`；提供 `create/list_ready/count_running/update_execution/update_progress/list_by_group`。
    - 适用对象：子代理批次、长命令（`execute_command`）、上下文压缩、插件声明的后台工作。
    - 落地：`agent/lib/include/agentxx/util/sqlite.h`（如需）+ 新增 `agent/lib/{include/agentxx/job,src/job}/`。
2. **调度循环（P0）**：单线程内实现"下次触发时间 + 容量 + 提前唤醒"三件套（无需 cron 表达式，先支持"立即 + 延迟 + 周期(秒)"三种）；每次执行前落库（running + deadline），结束后按"失败是否可重试"决定重排；进程启动时扫描 `state=running 且 deadline 已过` 的作业做**超期回收**（标失败 + 通知用户）。
    - 落地：同上；与 `BaseAgent::ioCtx` 同一 io_context，天然无锁。
3. **超时预算与进度（P0/P1）**：作业化后统一把 `deadline` 传给工具/子代理（复用 `CancelToken`），并发 `progress` 事件给客户端（已有 `subagent.progress`，扩展为通用 `job.progress`）。
    - 落地：`agent/lib/src/nodes/toolcall.cpp` + `agent/lib/src/tools/subagent.cpp`。
4. **作业容量与批量组（P1）**：`max_running` 配置 + 组 id；客户端增加"后台任务"面板（Info 侧栏同级），展示正在跑/最近完成/失败的作业，可取消与重试（对齐 harness 的 `GetJobProgress/GetJobProgressForGroup/CancelJob`）。
    - 落地：TUI `ui_sidebar_content.cpp` + wire 消息（`WireJobList`/`WireJobCancel`）。
5. **阶段化停机纳入作业（P1）**：`shutdownAsync` 中"等待在跑作业结束"（已有文件写盘与事件总线收尾），并可把未完成作业标记为"可恢复"。

---

## 10. 认证、授权与权限

### 10.1 harness：认证（谁）与授权（能做什么）彻底分离

**认证 AuthN**（`app/auth/authn/`）：`Authenticator.Authenticate(r *http.Request) (*auth.Session, error)`

- 认证源覆盖：token（PAT 等）、JWT（`authn/jwt.go` 239 行，用于 Web）、session cookie、git 的 basic auth、SSH 公钥（`keyfetcher` + `ssh` 服务）；
- **"无认证数据"不是错误**：返回 `authn.ErrNoAuthData`，中间件把会话置为 `auth.AnonymousPrincipal` 继续往下走（`middleware/authn/authn.go`）——公开资源无需特判；
- 会话结构 `auth.Session{Principal, Metadata}`，`Metadata` 是接口：`TokenMetadata{TokenType}` / `EmptyMetadata` / git hook 元数据等，**认证方式随会话传递**（因此 `authz.BlockSessionToken` 能识别"这次是 session token 在操作 git"并拒绝）；
- 日志上下文注入 `principal_uid/principal_type/principal_admin`（每次请求日志都带操作者）。

**授权 AuthZ**（`app/auth/authz/`）：

- `Authorizer` 接口三方法：`Check(ctx, session, scope, resource, permission) (bool, error)`、`CheckAll(...PermissionCheck)`（全部满足）、`CheckMany(...)`（逐项返回）——**返回值语义被注释精确定义**：`(true,nil)` 允许 / `(false,nil)` 不允许 / `(false,err)` 检查出错应当拒绝（失败关闭）；
- 权限枚举按资源分组（`types/enum/permission.go`：`space_view/edit/delete`、`repo_view/push/review/reportCommitCheck`、`pipeline_execute`、`secret_access`、`gitspace_*`、`registry_*`…）；
- 实现：`membership.go`（空间/仓库成员角色 → 权限集合）+ `membership_cache.go`（成员关系缓存，避免每个请求查库）+ `public_access.go`（公开资源放行）；
- 测试辅助：`app/auth/authz/authztest/authztest.go` 提供**假授权器**（76 行），使 handler/controller 测试不必搭真实权限数据；
- 审计：`audit/` 包（`middleware.go` + `objects.go` + `context.go`）把"谁在什么资源上做了什么"落库，供事后追溯。

### 10.2 agentxx：权限中间件 + 插件声明 + 人机协商

- **两级作用域**：`PermissionMiddlewareHandle::FilesystemPermissionREAD = 0` / `WRITE = 1`；规则表 `XXRouter<PermissionOperator, 2> filesystemPermission`（**最长路径匹配 + `*` 通配符**），`configDenyPermission_` 单独存"配置显式拒绝"（**即使完全授权也拒绝且不询问**）。
- **判定顺序**（`checkTargetPermission` / `decideTarget` 注释）：
    1. 工作区隔离写拒绝（worktree：命中 `denyWritePath` 直接拒绝，`allowPath` 为其例外子树，**隔离优先于白名单与模式默认**）；
    2. 配置拒绝路径；
    3. 完全授权（`fullAuthorized_`）；
    4. 规则表命中（ALLOW / DENY / INTERRUPT）；
    5. `noRuleOperator` 兜底（`CodeAgent` 按 `permission.mode` 设置：`ask/all_ask → INTERRUPT`、`pass → ALLOW`、`deny → DENY`）。
- **三态结果**：`PathDecision{Deny, Allow, Ask}` + 批量 `decidePaths(paths, scope, sessionId)`——"纯只读判定：不发起询问、不产生中断、不修改任何状态"，供模式/前缀参数工具（glob/grep/list）**逐项过滤**，避免 `**` 模式绕过子目录拒绝规则；空路径或规范化失败一律按 `Ask`（"无法判定时不按已批准处理"）。
- **工具权限由插件声明**（`ToolPermissionSpec`）：`scope` + `targetKind{None, Path, Text}` + `targetArgs[]` + 可选 `category`；目标值按**参数实际 JSON 类型**处理（字符串单目标、数组逐项），未声明权限的工具**不参与判定（直接放行）**；声明随工具注销/插件禁用卸载撤销。
- **人机协商**：`INTERRUPT` 时经**会话总线**（`service.permission` 请求-响应）询问用户，卡片带工具名/参数/分类文本；应答可携带 `remember` → 以该目标为根注册 ALLOW/DENY 规则（"同目标及其子路径不再询问"）。无 prompter（无 IO 端点）或被拒 → false。
- **完全授权**：状态由服务端持有，变更时经总线广播（`service.permission.full_auth`）使多客户端一致（TUI Info 侧栏可切换，客户端先乐观更新再被广播校准）。
- **跨端服务**：`service.permission.check`（工具调用统一入口，供节点/中间件调用）、`set_isolation`/`clear_isolation`（worktree 绑定/解绑时由 `agentxx_git_worktree` 调用）。

### 10.3 对比与结论

| 维度 | harness | agentxx |
|---|---|---|
| 主体模型 | Principal（user/service/serviceaccount/anonymous）+ 会话元数据（token 类型等） | 单用户 + 会话（无多主体） |
| 权限模型 | RBAC：资源 → 权限枚举 → 角色/成员/公开访问 | 规则表（最长前缀 + 通配） + 模式默认 + 隔离 + 完全授权 |
| 判定入口 | `Authorizer.Check/CheckAll/CheckMany`，失败即关闭 | `checkToolPermission` → `checkTargetPermission`（含询问） |
| 失败姿态 | `(false, err)` 明确要求拒绝 | 规范化失败按 `Ask`（不按已批准） |
| 权限粒度 | 资源级（repo_push 等） | 目标级（路径/文本 + 作用域） |
| 询问/交互 | 无（返回 403） | **有**（HIL 中断卡片，可记住选择） |
| 缓存 | 成员关系缓存 | 无（规则表在内存，天然快） |
| 审计 | `audit/` 落库 | 无（仅日志） |
| 测试辅助 | `authztest` 假授权器 | 测试通过 `EventBus` 挂应答器（`test_interrupt_bus.cpp` 等） |
| 声明能力 | 静态代码 | **插件声明**（`ToolPermissionSpec`），随插件生命周期生效/撤销 |

**harness 的优点**：①**认证与授权分离**且都用"接口 + 多实现"表达，新增认证方式（SSH key、PAT、JWT）不影响授权逻辑；②`Authorizer` 返回 `(bool, error)` 并明确"出错即拒绝"，避免"检查失败默认放行"的安全漏洞；③**会话携带认证方式**（`TokenMetadata`），使"禁止 session token 操作 git"这类策略可实现；④审计留痕；⑤有假授权器便于测试；⑥权限枚举集中定义，可作为文档与前端权限位。

**agentxx 的优点**：①**权限判定与路径语义深度绑定**（最长前缀 + 通配 + 目录/文件尾斜杠归一化），这比"资源级 RBAC"更贴合文件工具的实际需求；②**人机协商 + 记住选择**把"逐次询问"与"永久放行"衔接起来，是 agent 场景的核心体验；③**三态判定 + 批量过滤**解决了"模式参数（glob）绕过子目录拒绝规则"这一实质安全问题；④**插件声明权限**避免宿主硬编码工具名（声明随工具/插件生命周期自动生效与撤销）；⑤worktree 隔离语义与前端工具（Claude Code 风格）对齐，且有明确"隔离优先于白名单"的优先级说明。

**agentxx 的缺点**：①**无审计**：谁在什么时候允许/拒绝了什么路径没有落盘记录（只有日志），事后追溯与"为什么这个操作被放过/拦下"缺少证据；②**判定结果不带理由**：只有 bool/三态，客户端与日志难以回答"是被配置拒绝、隔离拒绝还是规则拒绝"；③**作用域只有读/写两类**：网络访问、进程执行、环境变量、外设（screen capture/computer use 插件）等没有统一权限作用域，插件各自实现（例如网络类工具缺少统一出网策略，见 §11）；④规则来源分散（配置、记住的选择、worktree 隔离、插件声明、模式默认）缺少"合并后的有效规则视图"；⑤测试辅助偏手写（应答器/总线），缺少 harness 那种可复用的假实现。

**可迁移设计（§10）**

1. **判定结果结构化 + 理由（P0）**：把 `bool` / `PathDecision` 升级为 `PermissionDecision{decision, reason, rule{source, pattern, scope}, target, category}`，`reason ∈ {SessionIsolation, ConfigDeny, FullAuth, RuleAllow, RuleDeny, RuleAsk, ModeDefault, Unresolved}`。
    - 用途：日志一行说清理由；客户端询问卡片显示"为什么问"；Info/诊断展示有效规则视图。
    - 落地：`agent/lib/include/agentxx/middlewares/permission.h` + `agent/lib/src/middlewares/permission.cpp`。
2. **权限审计表（P1）**：在会话库或全局库写 `permission_audit(id, ts, sessionId, tool, target, scope, decision, reason, remembered)`，供用户事后回看与"撤销某条记住的选择"。同时提供"列出已记住规则 + 删除"的界面能力（TUI 设置项）。
    - 落地：`agent/lib/src/middlewares/permission.cpp` + `agent/lib/src/agent/session_store.cpp`（或全局 `settings_db`）。
3. **作用域扩展（P1）**：把 scope 从"文件读/写"扩展为枚举 `{FsRead, FsWrite, NetEgress, ProcessExec, DeviceCapture}`，让 `execute_command`、`websearch`、`screen_capture`、`computer_use` 等插件用同一套判定与询问（配合 §11 的出网策略），并保持向后兼容（旧声明默认 FsRead/FsWrite）。
    - 落地：`permission.h` 的 `ToolPermissionSpec.scope` 改为强类型 + `agent/lib/src/plugins/plugin_manager_vtable.cpp` 的接口表转换。
4. **有效规则视图（P1）**：提供 `listEffectiveRules(sessionId)` 供客户端展示"当前状态：完全授权? 模式? 已记住的允许/拒绝? 隔离边界?"，替代现在分散在多处的隐式状态（`fullAuthorized_`、`sessionIsolations_`、router 规则表）。
    - 落地：`permission.h` + wire 消息 `WireGetPermissionState` 扩展字段。
5. **可复用假实现（P2）**：为权限与询问提供测试替身（`PermissionPrompterStub` / `PermissionRuleBuilder`），统一现有 `test_interrupt_bus.cpp` / `test_plugin_*` 里手写的应答器。
    - 落地：`agent/test/include/agentxx-test/core/`（新增）。

---

## 11. 执行世界与沙箱（含 CI 执行器）

> 本模块把两侧"真正执行东西的地方"放在一起看：harness 是 CI/Gitspace 的容器执行与出网策略；agentxx 是命令执行、worktree 隔离、外设类插件与权限模式。

### 11.1 harness：执行后端抽象 + 出网策略 + 容器编排

**(1) 执行后端抽象**（`infraprovider/`）

- `InfraProvider` 接口（99 行）抽象"谁来提供机器/容器"；`Factory.GetInfraProvider(enum.InfraProviderType)` 按类型返回实现，目前注册 `DockerProvider`（363 行）与 `docker_client_factory.go`（Docker API 版本协商）。
- 上层（Gitspace）只依赖接口与类型枚举，因此"把执行搬到远端/别的容器运行时"是替换 Factory 实现的事——与 agentxx 的 C ABI 接口表在思路上同源。

**(2) 开发环境编排**（`app/gitspace/`）

- `orchestrator/`（触发 `orchestrator_trigger.go` 551 行 / 恢复 `orchestrator_resume.go` 451 行）负责"创建/启动/停止/删除"一套环境；
- `orchestrator/container/`：`embedded_docker_container_orchestrator.go`（890 行）+ `devcontainer_container_utils.go`（935 行）+ `runarg_utils.go`（562 行）+ `devcontainer_config_utils.go`，即**以 devcontainer.json 为契约解析镜像、特性、runArgs、挂载、端口**；
- `orchestrator/ide/`：VS Code / VS Code Web / Cursor / Windsurf / JetBrains 各自的接入（每个 100~320 行），彼此独立；
- `orchestrator/utils/`：镜像解析、特性下载与构建（`download_features.go` 394 / `build_with_features.go` 236）、环境变量、git 初始化、AI agent 辅助（`ai_agent.go` 153）；
- `infrastructure/`：`trigger_infra_event.go`（363）与 `infra_post_exec.go`（101）把"编排步骤"变成**事件序列**（`app/events/gitspace*`），失败可重放；
- `logutil/stateful_logger.go`：把编排过程的状态变化写日志并可回放给用户。

**(3) 出网策略**（`netpolicy/netpolicy.go`，本模块最有价值的可迁移件）

- **地址分类**：`Class{Public, Loopback, LinkLocal, Private, Reserved}` + `ClassifyAddr`（先 `Unmap()` 处理 IPv4-mapped IPv6，multicast 优先于 link-local，transition 段 6to4/Teredo/NAT64 归为 Reserved，避免"用 IPv6 绕回被封的 IPv4"）。
- **两种检查点，用途不同**（注释里写明取舍）：
    - `Policy.ControlFunc(errFn)`：作为 `net.Dialer.Control` 钩子，**在解析之后、connect syscall 之前**拒绝——"被拒地址永不接触，所以既不成功握手也不报连接错误，无法用来探测内网"；且"DNS 应答无法在校验后被替换成内网地址"；
    - `Policy.CheckHost/CheckURLHost`：用于**交给外部进程的目标**（例如 git clone），注释明确"解析与连接之间地址理论上可变，优先用 ControlFunc"。
- **错误统一**：`ErrAddressNotAllowed` 对"被拒"与"不可达"返回同一种错误，理由写在注释里："一个能暴露拒绝原因的错误会让 dialer 变成内网状态的探针"（安全优雅）。
- 配置面：`Webhook.AllowPrivateNetwork/AllowLoopback/AllowLinkLocal`；webhook 服务为此维护 4 个 HTTP client（secure/insecure × internal/external）。

**(4) CI 执行器**（`app/pipeline/`）

- 模型：`pipeline → stage → step`；`scheduler/queue.go` 是**拉取式队列**：`Request(ctx, Filter)` 注册一个 `worker{kind,type,os,arch,kernel,variant,labels}` 并阻塞等派发；`signal()` 在**全局锁**下 `ListIncomplete`，按"资源 kind/type 匹配 → 平台匹配（os/arch/variant/kernel）→ 标签匹配（`checkLabels` 要求完全一致）"把 stage 推给某个 worker（`w.channel <- item` 或 `w.done` 已关闭则跳过），随后从 map 删除该 worker；
- **限流**：`withinLimits(stage, siblings)`（同名 stage 并发数受 `stage.Limit` 约束）与 `shouldThrottle(..., LimitRepo)`（单仓库并发上限，按 ID 顺序保证先到先跑）；
- **取消**：`canceler` 用 `subscribers map[chan struct{}]int64` + `cancelled map[int64]time.Time`（**TTL 5 分钟**，"给网络中断的客户端留出重连并收到取消通知的窗口"），`Cancelled(ctx,id)` 阻塞等待；
- **执行**：`runner/` 用 drone 的 docker engine 起容器（`Privileged` 白名单、`ExtraHosts: host.docker.internal:host-gateway`（macOS 除外）、`Networks: config.CI.ContainerNetworks`、`secret.Encrypted()` 注入密钥）；`poller.go` 以**固定并行 worker 数**轮询 stage 并 `runner.Run`，并在 dispatch 外层 recover 转错误；
- **状态与服务**：`checks/`（检查项）、`converter/`（YAML → 内部模型）、`triggerer/`、`manager/`、`logger/`（执行日志）、`canceler/`。

### 11.2 agentxx：本机执行为主 + 权限为闸门 + worktree 隔离

- **命令执行**（`agent/plugins/agentxx_execute_command`，4 文件 1906 行）：`agentxx_execute_bash_command` / `agentxx_execute_windows_command` 两个工具；实现为 asio 协程（子进程管道绑定协程 executor，等待插件本地 reactor 的管道就绪事件），注册为**声明式受控轮询工具**（`polled_tool`）——"并发多条命令共享同一条 polled 驱动序列与同一个本地 reactor，不再每条命令占死一个宿主工作线程直到超时"；会话取消经 `CancelRegistry` 事件驱动 **kill 进程组**；关闭 `AGENTXX_ENABLE_BOOST_PROCESS` 时的 popen 回退仍是阻塞实现（走 `blocking_tool` + offload 线程）。
    - 提示词（含环境探测出的 python/node/PowerShell 说明）由插件在 `start` 事务内生成并注入宿主提示词表，卸载时自动撤销。
- **隔离手段**：
    - **权限模式**（`PermissionMode::{Ask,AllAsk,Pass,Deny}`）决定默认放行/询问/拒绝（见 §10）；
    - **worktree 隔离**（`agent/lib/src/tools/git_worktree.cpp` 622 行 + `setSessionIsolation`）：绑定 worktree 后，主检出内的**写操作直接拒绝**（"隔离优先于白名单与模式默认规则"），worktree 自身为允许例外；读与其他路径不受影响；
    - **子代理**：独立上下文 + 独立系统提示（隔离上下文，不是隔离权限）；
    - **插件多实例/禁用**：可整体卸载某类能力。
- **外设与桌面控制**（Windows 专属插件）：`screen_capture`（1327 行）、`computer_use`（1299 行）、`text_selection_monitor`（1889 行）——**没有任何权限声明**（见下"缺口"）。
- **受控脚本执行**：`agentxx_javascript_engine`（2702 行）在进程内跑 JS；`codegraph`（3755 行）做代码图查询。
- **外部工具接入**：MCP client（2381 行）/ server（1707 行）、ACP（931）、A2A（882/190）——远端工具即"外部执行世界"，但同样不受 `permission` 覆盖。

**缺口（源码核实，值得优先处理）**

- **只有文件系统插件声明了权限**：全仓库 `registerReadPathPermission/registerWritePathPermission` 只出现在 `agentxx_filesystem.cpp`（list/read/write/edit/glob/grep 六处）；**`execute_command`、`computer_use`、`screen_capture`、`websearch`、`rag_search` 等均未声明任何权限**，按"未声明即放行"的规则直接绕过权限中间件。
- 即：`git`、命令执行、桌面控制这些**破坏力最大的能力**恰好不在权限闸门内；而 `check_paths` 批量判定只有 filesystem 插件在用（`agentxx_filesystem.cpp:72` 的 `filterPathPermissions`）。

### 11.3 对比与结论

| 维度 | harness | agentxx |
|---|---|---|
| 执行位置 | 容器（CI/Gitspace）+ 可选远端 infra provider | 本机进程（+ 远端 MCP/ACP/A2A 工具） |
| 后端抽象 | `InfraProvider` 工厂 + Docker 实现 | 插件（C ABI）+ MCP/ACP/A2A 客户端 |
| 环境描述 | devcontainer.json（镜像/特性/runArgs/端口/IDE） | 无（用当前机器环境+探测提示词） |
| 出网控制 | `netpolicy` 地址分类 + dial 钩子 + 对外部进程的解析校验 | **无**（websearch/MCP 可访问任意地址，含内网） |
| 权限覆盖 | RBAC 资源级（含 pipeline_execute、secret_access） | 目标级（路径/文本）但**仅文件系统工具声明** |
| 隔离 | 容器 + 网络策略 + 密钥注入 | worktree（写边界）+ 权限模式 + 子代理上下文 |
| 资源限制 | worker 并发、stage/repo 限流、容器资源 | 工具超时/并发连接数（模型侧），命令无统一预算 |
| 取消 | 队列级 canceler（TTL 窗口）+ ctx | `CancelRegistry` 事件驱动 kill 进程组 |
| 可观测 | 编排事件 + 执行日志（DB/S3）+ livelog | 事件总线进度 + 日志（TUI） |

**harness 的优点**：①**出网策略**是安全工程上的成熟做法（分类完整、两种检查点按用途分、错误不泄露信息）；②执行后端抽象让"本地 Docker → 远端"可替换；③限流维度多（stage 同名并发、单仓库并发、worker 并行数），避免"一个大仓库把平台打死"；④取消带 TTL 窗口，照顾断线客户端；⑤环境即声明（devcontainer.json），可复现。

**agentxx 的优点**：①命令执行**不占线程**（协程 + 本地 reactor + 声明式轮询），这在"多会话并发跑命令"的场景下是关键差异（harness 侧是 goroutine，天然无此问题）；②取消直接 kill 进程组，语义干净；③worktree 隔离把"agent 只改自己的分支"落到权限层，语义与业界前端工具一致；④插件化让"平台专属能力"（Windows 外设、屏幕）不影响其他平台；⑤提示词由插件按环境探测生成，模型看到的工具说明与真实环境一致。

**agentxx 的缺点**：①**权限覆盖不完整**（最危险的工具没声明权限），这是"声明制"的固有风险——新增插件容易忘记声明；②**没有出网策略**：websearch/rag/MCP 可访问内网地址，缺少 harness 那种"统一分类 + dial 钩子"；③命令执行无资源限制（CPU/内存/输出字节/时长），只有取消；④没有"执行环境声明"，模型只能靠 prompt 探测结果推测环境；⑤外设类插件在非 Windows 平台静默缺失（有平台门禁，但用户侧没有"能力为什么不可用"的解释）。

**可迁移设计（§11）**

1. **出网策略（P0）**：把 `netpolicy` 思想落到 `cxx_utilxx_base`/`cxx_utilxx` 的 HTTP/WS 客户端层：
    - 新增 `utilxx_base/net_policy.h`：`Class`（Public/Loopback/LinkLocal/Private/Reserved）+ `Classify(addr)` + `Policy{allowLoopback, allowPrivate, allowLinkLocal}` + `checkHost/checkUrlHost`；
    - `HttpClient`/`WsClient` 建连时用 asio 的对端地址做校验（等价 `Dialer.Control`：解析后、connect 前判定），默认**只允许 public**；配置项 `network.allowPrivate/allowLoopback/allowLinkLocal`；
    - 对外部进程（`git clone`、MCP 的 stdio server 拉起的命令）用"先解析校验再交给进程"的 `checkHost` 路径；
    - 错误信息统一为 `address not allowed`，不区分"拒绝"与"不可达"。
    - 落地：`agent/third_party/cxx_utilxx_base/include/utilxx_base/net_policy.h` + `cxx_utilxx` 的 `http_client`/`ws_client` + `agentxx-config.yaml` 配置项。
2. **权限声明强制化 + 审计覆盖（P0）**：为所有能产生副作用的工具强制声明权限作用域（扩展 §10 的 scope 枚举）：`execute_command → ProcessExec`（目标取命令文本，`targetKind=Text`）、`websearch/rag_search → NetEgress`（目标取 URL/域名）、`computer_use/screen_capture/text_selection_monitor → DeviceCapture`；并在**宿主侧**加"内置插件加载时校验：有副作用但未声明权限的工具 → 记 warning 或按配置拒绝加载"的检查（对齐 harness 的"失败关闭"）。
    - 落地：各插件 `start` + `agent/lib/src/plugins/plugin_manager_capability.cpp`（或 tool_registry 校验）。
3. **执行预算（P1）**：为命令/工具引入"墙钟超时 + 输出上限 + （可选）CPU/内存提示"，默认值可配；超时用 `CancelRegistry` kill 进程组，返回结构化超时结果（配合 §9 作业化）。
    - 落地：`agent/plugins/agentxx_execute_command/execute_command_impl.h` + `agent/lib/src/nodes/toolcall.cpp`。
4. **能力不可用说明（P2）**：平台门禁跳过的插件应在启动时产出"能力不可用 + 原因（平台不支持/缺少依赖）"的清单，供 Info 面板与模型提示词使用（避免模型反复尝试不存在的工具）。
    - 落地：`agent/lib/src/plugins/plugin_manager_lifecycle.cpp` + `client` 的 Info 面板。
5. **环境声明（P2）**：把 `start` 事务内探测出的环境（python/node/shell、可用命令）结构化为"环境说明"，既用于提示词，也可用于 `execute_command` 的参数校验（例如 bash 工具在无 bash 的平台直接不可用而非运行失败）。
    - 落地：`agent/plugins/agentxx_execute_command/execute_command_env.h`。

---

## 12. 执行引擎（图循环 / 流水线与 agent 循环）

### 12.1 agentxx：图引擎驱动的 ReAct 循环

- **默认图**：`__start__ → agent_start → llm →(条件边 xx_has_tool_calls)→ tools / agent_end → __end__`；条件由 `nodes/graph_conditions.h` 提供（读影子通道 `xx_messagesMeta.last_assistant_tool_calls`，无该通道时回退扫描 `messages` 通道，两者都无则返回 "false" = 安全默认"结束本轮"）。
- **节点**：
    - `AgentStart`/`AgentEnd`（`nodes/agentcall.cpp` 107 行）：一轮一次的钩子（中间件在此加 system message，注释明确"否则会被重复添加多次"）；
    - `ModelCall`（`nodes/modelcall.cpp` 868 行）：取会话上下文 → 拼系统提示词 → 调 provider（流式）→ 写会话 + 发事件；
    - `ToolcallWrapNode`（`nodes/toolcall.cpp` 1237 行）：参数类型自动修正（`autoFixArgsType`，按 JSON Schema 做 string↔number/bool、string→数组等纠正）→ 权限检查（总线上 `service.permission.check`）→ 重复调用检查（HIL 询问）→ 执行 → 结果回填 + 事件；
    - `WrapHandle`（`nodes/wrap_handle.h` 277 行）：把中间件的 7 个钩子套在节点前后。
- **工具调用是顺序执行**（源码事实）：`baseRun` 里对每个 declared tool call 构造一个 awaitable 放进 `toolcallResults`，然后**逐个 `co_await`**，循环里留着 `// TODO: 真正并行`（`toolcall.cpp` 约 1101 行）。
- **取消语义**：取消（`CancelledException`）会**停止后续 tool**，并把未完成的 tool call 补一条 `[User canceled]`（`MessageFlag::AutoInserted`）结果消息，"保证每条 assistant tool_call 都有对应的 tool 结果消息，上下文角色顺序和内容完整"；已完成的工具结果直接追加会话（"会话是唯一权威，不受图状态回滚影响"）。中断（`NodeInterrupt`）则不同——它会让图 resume 到本节点继续执行（`execTool` 内部捕获处理，不抛到外层）。
- **中断/恢复**：中间件 `requestInterrupt` 首次抛出、恢复后返回用户响应，"恢复后本函数重新执行"（`execTool` 的注释），因此中断实现必须幂等（同一处逻辑在恢复时重跑）。
- **上下文权威**：会话持有 `std::vector<neograph::ChatMessage>`；节点经 `nodes/session_context.h` 读写（`sessionMessages` 只读借用不得跨 co_await；`appendSessionMessages` 追加并发同名通道写事件）。
- **多会话并发**：单 io_context 协程交错；每个会话一个 `CancelToken`；`AgentHost`（1010 行）管理多 agent 实例与跨 agent 查询。

### 12.2 harness：拉取式流水线调度

- **模型**：`pipeline`（YAML 定义）→ `stage` → `step`；`stage` 是调度粒度，`step` 是容器内执行粒度。
- **调度**：`scheduler/queue.go` 用**拉取（pull）**而不是推送：runner 侧 `Request(ctx, Filter)` 注册 worker 阻塞等待；调度侧在全局锁下 `ListIncomplete()` 取未完成 stage，按 kind/type/平台/标签匹配后 `w.channel <- item`，成功即从等待集合移除；`ready` channel（容量 1）+ 每分钟定时 tick 触发 `signal()`。
- **并发与公平**：`withinLimits`（同名 stage 并发上限）、`shouldThrottle`（单仓库上限，按 ID 保证先到先跑）、`ListIncomplete` 跳过 `Running` 与已有 `Machine` 的 stage（避免重复派发）。
- **取消**：`canceler` 的订阅者集合 + 5 分钟 TTL 的已取消集合；`Cancelled(ctx,id)` 支持阻塞等待且容忍断线重连。
- **执行**：`runner` 用 docker engine 编译并运行 stage/step，`poller` 按 `CI.ParallelWorkers` 并发轮询；执行日志由 `logger/` + `livelog/` 处理（`logs.go` store 支持 DB 或 S3）。
- **检查与触发**：`checks/`（状态检查上报）、`triggerer/`（触发执行）、`converter/`（YAML 编译）、`commit/`（提交信息）、`manager/`（执行状态机）、`file/`（配置读取）。

### 12.3 对比与结论

| 维度 | agentxx | harness |
|---|---|---|
| 单位 | 轮（turn）→ 节点（node）→ 工具调用 | 流水线 → stage → step |
| 执行驱动 | 图引擎状态转移（条件边） | DB 状态 + 拉取式队列 |
| 并行 | **工具串行**（TODO 并行未实现）；多会话/多子代理并行 | stage 级并行（worker 拉取 + 限流） |
| 顺序保证 | 上下文消息顺序（tool_call ↔ tool 结果一一对应） | 按 ID 顺序保证公平与限流 |
| 取消 | CancelToken 级联 + 补占位结果 | canceler 订阅/TTL + ctx |
| 恢复/重放 | 中断 → resume 到节点；取消 → 从图开始重跑（结果已在会话） | stage 状态 + Machine 字段 + 队列重派 |
| 观测 | 事件总线（turn/model/tool 事件 + delta） | 执行日志（DB/S3）+ livelog + 状态上报 |
| 资源控制 | 无统一预算 | worker 并发 + stage/repo 限流 |

**agentxx 的优点**：①**语义完整性**被显式保护（每条 tool_call 必须有结果、取消也补占位、上下文角色顺序和内容完整），这类不变式是 agent 相比 CI 流水线更容易出错的地方；②中断（可恢复、幂等重跑）与取消（从图起点重跑、会话不受图回滚影响）被明确区分并各自实现；③上下文权威唯一，避免了"图状态回滚导致上下文与展示不一致"；④节点可被插件改写（`PluginGraphNode` + `graph` 接口表），扩展性远强于固定流水线模型。

**harness 的优点**：①**拉取式调度 + 限流**把"谁执行、执行几个、按什么顺序"变成显式策略（worker 标签匹配、同名并发、仓库并发），比"来一个跑一个"可控；②`Machine` 字段 + 状态行让"执行中"可被其他实例识别（避免重复派发与僵尸）；③取消带 TTL 窗口，容忍客户端断线；④执行日志外部化（DB/S3），与状态分离，便于长日志与回看。

**agentxx 的缺点**：①**工具串行**：一轮里多个独立工具（读 3 个文件、跑 2 条无关命令）必须排队，直接拉长响应时间；源码里 `// TODO: 真正并行` 是已知项；②没有"并发上限/优先级"概念：并行一旦实现，缺少 harness 那样的并发策略会立刻出现"一次打出几十个命令"；③中断幂等性依赖"同处逻辑重跑"的实现纪律，缺少机制性检查（例如中断恢复时不得重复写入副作用）；④执行观测只有事件流，长输出与历史执行记录没有外部化存储。

**可迁移设计（§12）**

1. **只读工具并行 + 明确合并顺序（P0/P1）**：
    - 依据 §10 的 scope 判定：`FsRead` / `NetEgress` 等只读工具可在同一轮内**并发执行**（用一个 asio 并发组，`max_parallel_tools` 可配，默认 2~4）；
    - 结果必须**按模型声明的 tool call 顺序**合并写回（对齐 harness"后置按模型顺序提交"的做法），保证上下文顺序稳定；
    - 写类/交互类工具（`FsWrite`、`ProcessExec`、`DeviceCapture`、需要询问的 INTERRUPT）仍串行执行。
    - 落地：`agent/lib/src/nodes/toolcall.cpp`（替换 1101 行附近的串行循环）+ `XXToolBase` 增加"可并发"声明（或从权限 scope 推导）。
2. **并发上限与优先级（P0 配套）**：并行落地时必须同时引入 `toolcall.maxParallel`、`maxParallelPerKind`（如命令类单独限 1）与"高优先级（用户显式触发）优先"，否则并行会立刻变成资源炸弹。
3. **中断幂等检查（P1）**：为 `requestInterrupt` 的恢复路径提供"副作用只执行一次"的机制化支持（例如把"恢复后重跑"标记成显式 API：`co_await interruptOnce(key, fn)`，内部按 key 记录已执行），减少"恢复时重复写入"这类隐患（可参考 harness 作业的 `TotalExecutions` + 状态机思路）。
    - 落地：`agent/lib/include/agentxx/middlewares/interrupt_presets.h` / `interrupt_ui.h` + `toolcall.cpp`。
4. **执行记录外部化（P1）**：把"每轮工具执行记录"（工具名、参数摘要、起止时间、结果长度、是否被拒）写入会话库新表并暴露查询（TUI 可展开/复制），避免只靠内存事件流。
    - 落地：`agent/lib/src/agent/session_store.cpp` + `agent/lib/src/nodes/toolcall.cpp`（已有 `startTimeMs/durationMs` 字段）。
5. **标签化调度（P2，可选）**：如果未来出现"多个执行后端"（本地 / 远端 / 沙箱容器），可借鉴 harness 的 `worker{kind,type,os,arch,labels}` + 匹配派发，把"哪个工具在哪执行"变成策略而非硬编码。

---

## 13. 对象存储、附件与大输出

### 13.1 harness：blob/日志/流式三条路径

**(1) 对象存储抽象**（`blob/`）

- `Store` 接口只有 5 个方法：`Upload(ctx, reader, path)` / `GetSignedURL(ctx, path, expire, opts...)` / `Download(ctx, path)` / `Move(src, dst)` / `Delete(path)`，错误集合只有 `ErrNotFound` / `ErrNotSupported`——**能力面故意收窄**，让不同后端（`filesystem.go` 174 行 / `gcs.go` 256 行）都能实现。
- `Config{Provider, Bucket, KeyPath, TargetPrincipal, ImpersonationLifetime}`；`MaxFileSize` 默认 10 MB（防止把大文件塞进 DB 或内存）。
- 消费方式两种并存（`app/api/handler/upload/download.go`）：能拿到 `io.ReadCloser` 就**代理流式**返回（并显式设 Content-Type + 安全头），否则 307 重定向到**签名 URL**（由存储后端直接承载流量）——"代理 vs 定位符"的选择被写进实现而不是硬编码。
- `filesystem.go` 有 607 行测试（`filesystem_test.go`），`interface_test.go` 用同一套断言跑不同实现（与 §7 的 cache 测试同思路）。

**(2) 大输出的第二类：执行日志**（`app/store/logs.go` + `app/store/logs/{db,s3,combine}.go`）

- `LogStore{Find/Create/Update/Delete}`，`Find` 返回 `io.ReadCloser`（**流式读**）、`Create/Update` 接收 `io.Reader`；
- DB 实现把日志按 `log_id` 整块存 `log_data`（`io.ReadAll` 后写库），S3 实现存对象存储，`combine.go` 组合两者；
- 决策在装配层（`ProvideLogsConfig` / wire），业务只认接口。

**(3) 大输出的第三类：流式响应**

- `types.Stream[T] interface { Next() (T, error) }`（只有 3 行）是所有"可分片产出"的统一抽象（diff、commit 列表、归档等）；
- `render.JSONArrayDynamic` 与 `render.Reader` 是它的两种出口（动态 JSON 数组 / 原始字节流）；
- 仓库归档（zip/tar）、LFS 对象传输也走流式路径（`handler/repo/archive.go`、`handler/lfs/*`）。

### 13.2 agentxx：会话内共享存储 + 附件两态 + 输出压缩

**(1) 工具间共享存储（share_store）**

- `SessionShareStoreTool`（`agentxx_share_store`）"寄存信息，节省模型上下文、为 llm/node/skill/tool 之间方便传递数据"；底层是会话库 `store` 表 + 内存 3 条 LRU；
- 容量与条目数解耦：首次访问只取 `max(id)`；id 大于 lastId（从未分配）按参数错误抛异常；已不支持删除条目。

**(2) 附件两态**

- `runTurnAsync(..., attachments)` 接收 `MediaAttachment`；远程模式下客户端与服务端设备不同时，TUI 文件选择弹窗多出「本地 / 服务端」标签页（同设备不渲染该行）；
- 客户端列举由服务端经 `ListDir` / `ListDirResult` 完成（服务端线程池扫描、路径全程 UTF-8）；
- 选中服务端文件时**只发 `pathOrUrl`（不含 base64）**，由服务端自行读取编码（`BaseAgent` 收到 dataUrl 为空且 pathOrUrl 为本地路径的附件时按路径加载，卸载到线程池）；
- 中文路径一律经 `utilxx_base::pathToUtf8Generic` / `utf8ToPath` 转换（Windows 本地代码页会导致乱码）。

**(3) 大输出处理**

- 工具结果超过 `AgentConfig::toolcallSummaryLimitOutputLength` 且该工具 `autoSummaryOutput=true` 时**自动压缩**（`XXToolBase::autoSummaryOutput` + `createSummarizationToolHandle()`，由 summarization 中间件执行）；
- 工具可声明 `canDelayLoad`（先只放简短信息给系统提示词，`tool_skill_search` 检索后再加载全量定义）——"大 schema 延迟加载"是省上下文的重要机制；
- 展示历史分页：`get_view_messages` 按游标拉取更早窗口（长会话恢复时初始只同步末尾）；
- 持久化仍是"每会话一个 sqlite"，没有外部对象存储。

### 13.3 对比与结论

| 维度 | harness | agentxx |
|---|---|---|
| 对象存储 | blob 抽象（filesystem / GCS）+ 签名 URL | 无（本机文件 + 会话 sqlite） |
| 附件传递 | 上传到 blob → 返回路径/签名 URL | base64 dataUrl 或 pathOrUrl（远端服务端自读） |
| 大文本 | LogStore（DB / S3）+ 流式读 | share_store + 工具输出自动压缩 |
| 流式接口 | `types.Stream[T]` + 动态 JSON / 原始流 | 事件流 + delta（面向 UI，不面向大数据） |
| 上传限额 | `MaxFileSize` 默认 10MB | 受 base64 与消息体大小间接约束 |
| 定位符 vs 代理 | 两者都有（signed URL 或代理流） | 服务端路径（pathOrUrl）+ share_store id |

**harness 的优点**：①**能力面收窄的存储抽象**（5 方法 + 2 错误）让换后端成本极低，且用同一套测试覆盖实现；②"签名 URL 或代理流"让大文件不经过 API 进程（对齐真实生产需求）；③日志与状态分离存储（DB/S3 可换），长日志不拖累主库；④`Stream[T]` 三行接口统一了所有分片产出。

**agentxx 的优点**：①**share_store 解决了 agent 特有的问题**——工具之间传递大中间结果而不占模型上下文，这是 harness 完全没有的需求；②**附件两态设计**（本地 base64 / 服务端自读路径）避免了把大文件往返编码，且跨设备选择体验完整（含目录列举）；③工具输出自动压缩 + schema 延迟加载，从根上控制上下文膨胀；④路径 UTF-8 转换规则明确（Windows 代码页陷阱有专门处理与测试）。

**agentxx 的缺点**：①**没有独立的对象存储层**（深读修正：**工具级 spill 已存在**——`share_store` 寄存 + 定位符 + 分片读，见 [§32 修正 4](#修正-4大输出外置与定位符已实现工具级缺统一策略)）：数据仍都在本机 sqlite/文件，没有跨进程/跨设备的大对象层，也缺统一的大小限额与"定位符"结构化表达；②没有统一的大小限额与配额（附件、share_store、工具输出各自约束）；③大输出"截断/压缩"策略分散（工具自身截断、summarization 压缩、share_store 寄存），缺少统一决策入口；④历史展示分页只有"窗口"概念，缺少"按类型/工具过滤 + 搜索"（harness 的 keywordsearch 是同类需求的另一侧）。

**可迁移设计（§13）**

1. **大输出外部化 + 定位符（P0）**：为工具结果引入统一的 `spill` 策略——超过阈值（如 32KB 且超模型可见预算）时写入会话目录 `{session}/blobs/{id}`，上下文里只放"摘要 + 定位符（id/路径）+ 行数/字节数"，模型可用 `read`/`grep` 工具按需分段读取（agentxx 已有文件工具与 share_store，可复用）。
    - 落地：`agent/lib/src/nodes/toolcall.cpp` + `agent/lib/src/agent/session_store.cpp`（新增 blobs 目录）+ `agentxx_filesystem` 的 read 支持偏移分页（如已有则复用）。
2. **大小限额集中化（P1）**：把附件大小、share_store 单条大小、工具输出阈值、历史分页窗口统一到 `AgentConfig` 的一组 `limits` 配置，并在超限时给**明确错误码与原因**（而不是静默截断）。
    - 落地：`agent/lib/include/agentxx/agent/config.h`（新增 `limits` 段）+ 各消费点。
3. **附件元数据校验（P1）**：附件携带 `{mime, size, sha256, source(local|server)}`，服务端按声明校验实际内容（防止"声称 png 实为可执行文件"被模型当图片处理），并在提示词中说明附件来源（对齐 harness 的 `contentTypeForFile` + 扩展名校验思路）。
    - 落地：`agent/lib/include/agentxx/agent/conversation_types.h`（`MediaAttachment`）+ `base_agent.cpp` 附件读取路径。
4. **会话内容检索（P2）**：为共享存储与历史消息提供本地检索（关键词/正则/按工具过滤），替代"只能靠模型重新读取"；实现可直接复用 `cxx_utilxx` 的正则/aho-corasick 与 sqlite LIKE（注意转义，参照 harness `PartialMatch`）。
    - 落地：`agent/lib/src/agent/session_store.cpp` + TUI 搜索弹窗。

---

## 14. 可观测性与运维

### 14.1 harness：结构化日志 + 审计 + 实时日志 + 指标 + pprof

**(1) 日志**

- 全局 `zerolog`；终端是 tty 时切 pretty console writer（`SetupLogger`），非 tty 输出 JSON；
- **上下文携带**：`logging.NewContext(ctx, opts...)` 派生带注解的日志上下文，`UpdateContext` 就地追加（注释明确"未来所有用该 ctx 的日志都会受影响"）；内置 `WithRequestID`；
- `Router.ServeHTTP` 为每个请求建 logger（含 `http.original_url`、`logging.router`），认证中间件追加 `principal_uid/principal_type/principal_admin`，事件消费者追加 `events.category/group/reader`，作业追加 `job.UID/job.Type`；
- 第三方库（如 drone runner、oras）经 `logr.NewContext(ctx, zerologr.New(&log))` 接入同一日志流。
- 日志级别由 `GITNESS_DEBUG/GITNESS_TRACE` 控制（`SetupLogger`）。

**(2) 审计**（`audit/`）：`middleware.go` + `objects.go` + `context.go` + `interface.go`，把"谁在什么资源上做了什么"落库；`audit.WireSet` 参与装配；部分 controller 显式调用（`audit` 出现在 webhook 等服务的依赖列表里）。

**(3) 实时日志**（`livelog/`）：`Line{pos, out, time}` + `LogStream{Create/Delete/Write/Tail/Info}`；`Tail` 返回 `(<-chan *Line, <-chan error)`；`Info()` 返回 `Streams map[int64]int`（每个 step 的订阅者数量）——**把"有多少人在看"做成可观测数据**；内存实现（`memory.go`）与基于 stream 的实现（`stream.go`）+ 订阅者管理（`sub.go`）。

**(4) 指标与使用量**：

- `app/services/metric`（`CollectorJob`，注册为后台作业）+ `app/services/usage`（`usage_metrics` 表 + `usage_metrics_test.go` 337 行）+ `app/services/instrument`（`Service` + `Consumer` + `RepositoryCount`，停机时 `Close(ctx)`）；
- 独立 metrics 端点：`cli/operations/server/server.go` 起 `system.metricServer`（`cliserver.ProvideNoOpMetricServer` 可关）+ `http/auxilary_server.go`。

**(5) 性能剖析与健康检查**：`profiler.Profiler{StartProfiling(service, version)}`，实现 `GCPProfiler` / `NoopProfiler`（`ParseType` 不认则只记日志不启动，**不阻塞启动**）；`/healthz` 式 `HandleHealth`（200 即健康）、`HandleVersion`（版本信息）。

**(6) 运维细节**：启动完成打一条汇总日志（host/port/revision/version，`server.go`）；关闭流程每一步都有日志（"shutting down gracefully (press Ctrl+C again to force)"）；`GITNESS_GRACEFUL_SHUTDOWN_TIME` 控制关闭窗口（注释解释"5 分钟足够大多数 git clone 完成"）。

### 14.2 agentxx：分级日志 + 基准程序 + 内存治理

**(1) 日志**：统一 `XX_LOG*` 宏（`utilxx_base/log.h`），**不直接 std::cout/cerr**（避免破坏 TUI 显示）；TUI 侧有 `tui_log_sink.cpp` 把日志接到界面日志区；异常分类（`ExceptionClassification`）让日志能区分取消/中断/错误。

**(2) 资源基准（本项目的强项，见 `docs/zh-cn/design/benchmark.md` 685 行）**：

- `agentxx_benchmark`（一般仅 Release 编译）聚合 `resource_*` 模块，每个场景**独立子进程**（`AGENTXX_BENCH_NO_ISOLATE=1` 可退回同进程）；
- 指标：RSS/PSS/私有脏页/匿名/峰值/线程/fd + glibc 堆在用与碎片 + `malloc_trim` 可回收 + **smaps 模块级分解**（可执行文件/项目库/各插件/系统库/堆/匿名）+ 逻辑内存（会话消息/TUI 状态/工具 schema）+ 分阶段增量 + CPU 用户/内核时间；
- 场景：同进程 CLI/TUI、真实两进程（server/client 子进程）、真实 TUI（FTXUI 界面线程，含帧耗时/渲染字节）、真实 server 空载漂移/WS 轮次/断连回收、PTY 驱动 TUI 子进程、插件逐项边际内存；
- 报告：`{exec}/bench/bench_<时间戳>.json` + `.md`，`--baseline` 输出与上次的 ΔRSS/ΔPSS/Δ堆与模块级差异；
- 统计开关 `AgentConfigStatic::enableBenchmark`（默认关，"热路径只多一次无等待原子读"）。

**(3) 内存治理**：`tuneProcessAllocator()`（glibc `M_ARENA_MAX=1`，实测 VmSize 610MB→290MB）；`releaseFreeHeapPages()`（轮末 `malloc_trim(0)`，服务端 RSS -1.7~-2.8MB）；mimalloc 默认关闭并给出实测依据（长上下文下常驻内存 2~3.7 倍 vs CPU 省 ~10%）；THP 关闭（`MI_ALLOW_THP=OFF`）；sanitizer 与 mimalloc 互斥的自动处理。

**(4) 诊断能力**：`test_memgrowth.cpp`（491 行）做内存增长回归；`WireContextStats` 把上下文 token 统计推给客户端；Info 侧栏展示模型/工具/工作目录/权限状态。

### 14.3 对比与结论

| 维度 | harness | agentxx |
|---|---|---|
| 日志 | zerolog + 请求/主体/作业/事件字段 + tty 自适应 + logr 桥接 | `XX_LOG*` 分级 + TUI sink |
| 请求标识 | `request_id` + `logging.router` 贯穿 ctx | 无统一 trace id（有 sessionId/agentName 作为定位键） |
| 审计 | 独立 audit 表 | 无 |
| 实时日志 | livelog（Line/Tail/Info 订阅者计数） | 事件流 + TUI 渲染（无订阅者计数） |
| 指标 | metric collector + usage 表 + metric server | 无（有基准程序） |
| 性能剖析 | profiler 接口（GCP / noop） | 无（有资源基准与 memgrowth 测试） |
| 健康检查 | `/healthz`、`/version` | 无（进程就是客户端） |
| 资源治理 | 连接池/缓冲/GC（语言托管） | **显式内存治理（arena/malloc_trim/mimalloc 取舍）+ 基准门禁** |

**harness 的优点**：①`logging.NewContext/UpdateContext` 的"日志上下文"模型让每个请求/作业/事件自动带定位字段，排障成本极低；②audit 独立留痕；③livelog 把"日志订阅者数"作为指标暴露，运维可直接看出"有没人卡在日志流上"；④profiler 不认配置时只警告不阻塞启动（失败姿态友好）；⑤健康检查与版本端点是最低成本的运维接口。

**agentxx 的优点**：①**资源基准体系远超同类项目**（分模块 smaps 分解、独立子进程隔离基线、JSON 基线对比、真实 TUI 帧耗时），这是 C++ 本地程序最需要的能力；②内存治理有**实测依据**（不是照搬"用 mimalloc 就快"），且默认值经权衡；③`XX_LOG` + TUI sink 避免了日志破坏界面；④异常分类让"取消/中断"不污染错误统计。

**agentxx 的缺点**：①**没有统一 trace id**：一次工具调用跨 agent → 总线 → 插件 → 事件流，无法用一个 id 串起所有日志（现在只能靠 sessionId + 工具名人工关联）；②**没有审计**：权限决定、工具执行、文件写入没有可回看记录（与 §10 的权限审计项合并）；③没有指标导出与健康检查（远程模式下服务端"是否健康/在忙/上下文多大"缺统一查询，部分靠 `WireContextStats`）；④没有运行期性能剖析入口（长会话卡顿时只能靠日志）；⑤日志等级/分类缺少"模块化开关"（harness 可按子系统配日志级别）。

**可迁移设计（§14）**

1. **统一 trace/操作标识（P0）**：
    - 在会话层生成 `opId`（每轮）与 `callId`（每次工具调用，复用现有 tool_call_id），并把它放进 `XX_LOG` 的上下文（`utilxx_base/log.h` 已有 `XX_LOG`，可加"线程/任务本地字段"支持）；
    - 事件总线的请求-响应已带 `correlationId`（`RequestResponseStream`），把它纳入日志字段即可实现跨模块串联。
    - 落地：`agent/third_party/cxx_utilxx_base/include/utilxx_base/log.h`（上下文字段）+ `agent/lib/src/nodes/toolcall.cpp` + 插件 SDK 边界（`plugin_kit.h` 的 log 接口）。
2. **审计留痕（P1）**：复用 §10 建议 2 的 `permission_audit`，并扩展为 `action_audit`（工具执行、文件写入、命令执行、附件读取、子代理委派），字段含 `ts/opId/sessionId/actor(tool|user|plugin)/target/decision/result/durationMs`；提供 TUI 查看与导出（JSONL）。
    - 落地：`agent/lib/src/agent/session_store.cpp`（或独立 `audit.db`）+ 各执行点。
3. **运行期诊断入口（P1）**：
    - 远程模式：新增 wire 消息 `get_diagnostics`（进程 RSS/线程数/打开的会话库数、事件队列积压、当前在跑的作业、最近错误摘要）；
    - 本地模式：TUI 增加"诊断"弹窗（复用 Info 面板）。
    - 落地：`agent/lib/include/agentxx/agent/io/wire_protocol.h` + `agent/client/src/io/tui/components/overlays.cpp`。
4. **日志级别与模块开关（P2）**：配置支持 `log.level` 与按模块覆盖（`log.modules: {plugin.exec: debug}`），与 harness 的 `GITNESS_DEBUG/TRACE` + 字段过滤同思路但更细。
5. **基准门禁自动化（P2）**：把 `--baseline` 的 ΔRSS/ΔPSS 阈值接到 CI（超过阈值即失败），把"资源回归"从人工观察变成门禁（agentxx 已有 JSON 基线，缺的是阈值判定与 CI 集成）。

---

## 15. 客户端与 UI

### 15.1 harness：Web 前端为主体 + 生成式契约 + 嵌入式分发

- **前端**：`web/src`（React + TypeScript，1638 文件 / 18.8 万行），webpack 多入口（默认界面 / `cde` Gitspace 界面 / `ar` 制品库界面各一份配置），样式走 CSS Modules + `typed-scss-modules` 生成类型。
- **契约生成**：`gitness swagger` 导出 `openapi.yaml`（`cli/operations/swagger/swagger.go`）→ `yarn services`（`restful-react import`）生成 `src/services/code/*` 客户端；运行时 `/openapi.yaml` 与 `/swagger` 直接可用（`app/router/web.go`，并注明"每次都现场生成+序列化，可能值得优化"）。
- **分发**：`web/dist.go` 用 `//go:embed dist/*` 把前端打进二进制，`Router` 的 `WebRouter` 兜底所有未匹配路径（SPA）。开发时可用 `uiSourceOverride` 指向本地 dev server（`web.Handler(uiSourceOverride)`）。
- **实时更新**：SSE（`app/sse` + `render.StreamSSE`），按 space 主题订阅；公开访问由 `middleware/web` 的 `PublicAccess` 处理。
- **CLI**：`cmd/gitness` 用 kingpin 注册子命令；`cli/` 下是各子命令实现（`server` / `migrate` / `user(s)` / `account` / `hooks` / `swagger`）；`cli/session` 保存登录会话，`cli/textui/input.go` 提供交互式输入（密码等），`cli/provide/provider.go` 提供 API 客户端。

### 15.2 agentxx：终端 UI 为主体 + 声明式组件层 + 多形态宿主

- **TUI（唯一渲染实现）**：FTXUI，`agent/client/src/io/tui/agent_tui.cpp` 2947 行；组件 `components/{message_list, input_bar, sidebar, status_bar, overlays, file_picker_overlay, interrupt_view, spinner}`；框架件 `framework/{owned_reflect, ui_action_list, tui_state}`；性能件 `lazy_scrollable.cpp`（712 行，视口渲染 + 逐项缓存）、`scrollable.cpp`、`text_layout.cpp`；`markdown_block.cpp` + `markdown-ui`（含代码块按显示宽度折行、mermaid 状态图渲染）。
- **声明式 UI 描述层**（本项目的特色）：`agentxx.ui.item` 组件 JSON（文本/差异/表格/树/键值/趋势图/计量条/控件/提交行/自定义…）由**数据层**（`agent/lib/src/ui/item.cpp` 解析 + 纯文本降级 `plainText`）与**渲染层**（`ui_components.cpp` 2352 行，渲染 + 测量唯一实现）分离；插件只产出 JSON 声明（`agentxx::ui::build` / `agentxx::ui::kit`），由客户端 `parse → adapt(caps) → render`；能力名 `agentxx.client.components` / `form` / `layout` 支持老宿主降级。
- **表单与交互**：控件状态由宿主维护（`UiFormState`），点击/键盘/校验/取值只有一份实现；提交经动作通道回传 `__submit` + `{"values":{...}}` / `__cancel`；中断视图（`InterruptView`）复用同一套渲染与交互，仅结果去处不同（结果通道）。
- **多形态宿主**：
    - 同进程 TUI / CLI（`io/stdio`、`io/tui`）；
    - 远程 WS 客户端 ↔ agent server（`agent_io_transport.h` / `ws_io_transport.cpp` / `session_server_agent_io.cpp`）；
    - FFI 宿主（`ffi_api.h` 388 行 + `ffi/ffi_runtime.cpp` 829 行，供其他语言/App 嵌入，含 `ffi_client_io.cpp` 把宿主界面接成 IO 端点）；
    - 训练/批处理模式（`agent/lib/src/agent/training.cpp` 1668 行 + `client/train/train.cpp`）。
- **本地化与主题**：`tui_i18n.cpp`（316 行，中英）、`tui_theme.cpp`（202 行，主题名 + 动画等级，动画等级影响定时器注册与缓存）。
- **设置与诊断界面**：设置弹窗（分 界面/显示/更新/其他 分组、超高可滚动、滚轮移动选中项）、快捷键列表弹窗（含冲突段）、Info 侧栏（模型/上下文统计/工作目录/完全授权切换）、LLM 上下文查看弹窗。

### 15.3 对比与结论

| 维度 | harness | agentxx |
|---|---|---|
| 主要客户端 | 浏览器（React SPA） | 终端（FTXUI TUI） |
| 契约 | OpenAPI 生成 TS 客户端（生成式） | 手写 wire 协议 + 声明式 UI JSON |
| 分发 | embed 前端进二进制 | 单可执行文件（TUI 内建） |
| 实时推送 | SSE（按 space 订阅） | WS delta/sync（按会话订阅） |
| 扩展 UI | 前端插件无（后端定义 + 前端内置） | **插件产出 UI JSON**，客户端能力协商降级 |
| 表单 | 前端自管 | 宿主态（`UiFormState`）+ 统一提交协议 |
| 多语言 | 前端 i18n（`web/scripts/strings`） | `tui_i18n.cpp`（中/英） |
| 嵌入其它应用 | 无（CLI + HTTP） | FFI + Channel transport |
| 交互模型 | 请求/响应 + SSE | 请求/响应 + 事件流 + **中断（可恢复的 HIL）** |

**harness 的优点**：①**契约生成**让前后端接口不漂移（OpenAPI 是唯一源）；②前端 embedding 进二进制，分发简单；③`uiSourceOverride` 支持"本地前端 + 远端后端"的开发模式；④webpack 多入口让同仓库承载多个产品界面。

**agentxx 的优点**：①**声明式 UI 组件层**是 harness 完全没有的能力：插件不写界面代码、只交 JSON，客户端统一降级（最多降到 `Text`），这使得"同一 agent 服务 TUI/未来 GUI"成为可能；②**中断（HIL）交互**被建模成一等公民（可恢复、可过期、结果有去处），这是 agent 场景特有的；③**能力协商**让老客户端与新插件共存；④**FFI + Channel + WS 三种宿主形态**共存，覆盖"同进程/跨进程/嵌入应用"三类场景；⑤渲染层性能工程完备（视口 + 逐项缓存 + 测量与布局同源）。

**agentxx 的缺点**：①**手写协议**带来同步成本（§5 已提）；②TUI 交互受终端限制（表格宽表、复杂表单体验有限），而声明式组件层已能描述的内容多于 TUI 能优雅呈现的内容（例如 `canvas` 仅解析与降级）；③设置/诊断信息分散在多个弹窗，缺少"统一系统信息页"（harness 有 `/swagger` 与 `list_config`）；④没有 Web/远端"纯操作台"形态（远程客户端仍是 TUI 逻辑）。

**可迁移设计（§15）**

1. **协议/组件描述生成化（P1，与 §5 建议 3 合并）**：为 UI 组件描述建立单一 schema 文件（类似 `cxx_pluginxx_ui` 的 `schema/ui.def.json` + 生成器思路），由它生成 C++ 结构体/常量与校验；插件侧 kit 与客户端渲染都能据此校验，避免"描述写了但渲染不认"。
2. **统一系统信息页（P1）**：把 Info/设置/诊断合并为可搜索的"系统信息"视图（模型、插件、生效配置、权限状态、后台作业、缓存命中、日志路径），支持复制为文本便于报障（对齐 harness 的 `/swagger`+`list_config`+`/healthz` 三件套在本地形态下的等价物）。
3. **界面开发生态（P2）**：提供"本地 TUI 客户端 + 远端 server"的组合开关（agentxx 已支持，但缺等价于 `uiSourceOverride` 的"渲染层替换"入口）；为将来 GUI 客户端预留"能力上报 + 组件子集"的契约测试（已有 `ui_kit`/`ui_items` 模块可作为基础）。
4. **表单能力对齐（P2）**：把 `__submit`/`__cancel` 的域内约定写入能力声明（例如 `agentxx.client.form.submit`），使第三方插件可按能力探测决定表单形态，而不是假定宿主一定支持。

---

## 16. 测试与质量门禁

### 16.1 harness：标准测试 + 集成/一致性/负载 + 门禁流水线

**(1) 单元/集成测试**：288 个 `*_test.go` / 6.97 万行，分布上明显"贴着业务"：

- controller 级（`app/api/controller/pullreq` 11 个、`repo` 8 个、`app/services/protection` 9 个、`mergequeue` 6 个…）；
- handler 级（`app/api/handler/users/*_test.go` 等）——端点测试与实现同目录；
- 存储级（`app/store/database` 7 个，含 `setup_test.go` 141 行提供测试库初始化、`repo_test.go` 329 行、`root_space_move_test.go` 376 行）；
- 迁移测试（`app/store/database/migrate/migrate_test.go` 360 行）；
- 类型与工具（`types/*_test.go` 7 个、`store/errors_test.go`）。

**(2) 一致性/契约测试**：`registry/tests/{npm,maven,cargo,gopkg}` 各带 README 与用例，`make ar-conformance-test` / `hot-conformance-test` 起服务跑标准包管理器客户端（**用真实客户端验证协议实现**，比自测更有说服力）。

**(3) 端到端/负载**：`tests/load/`（`usage_metrics_test.go` + `init.go` 载入 `.local.env`/`.test.env`，并用 `GITNESS_E2E_TEST_ENABLED` 环境变量**开关式跳过**——默认不跑，CI 需要时打开）；`web/cypress` 前端 E2E（`yarn dev:cypress`）。

**(4) 手动/联调工具**：`.testapi/*.http`（REST Client 文件：login/space/diff）+ `http-client.env.json`，配合 VS Code 直接点运行；`.test.env` / `.local.env` 提供两套环境。

**(5) 门禁（Makefile + golangci-lint）**：

- `make test`（先 `generate` → `generate: wire`，保证生成物最新）；
- `make lint`（CI 模式）/`lint-full`/`lint-local`（只查未跟踪与已暂存改动，本地快）；
- `make format`（格式化并"有差异即报错"）；`make modernize[-fix]`；`make sec`（govulncheck 已知漏洞扫描 + 版本更新提示）；
- `make generate-mocks`（`mocks/` 17 个生成物）；`.golangci.yml` 14 KB 配置（大量 linter 与按目录/按规则的排除项）；
- `Makefile` 里 `dep`/`tools` 目标固定工具版本（`go.tool.mod`/`go.tool.sum`）。

### 16.2 agentxx：自研模块化测试 + 负面编译 + 资源基准 + 消毒器

**(1) 自研测试程序**：`agent/test/`（173 文件 / 8.51 万行），入口 `test.cpp`（420 行）注册模块；运行方式（据 `AGENTS.md`）：

```bash
agentxx_test                # 跑全部模块，出错也继续
agentxx_test --fail-fast    # 任一模块出错即停
agentxx_test string_util regex   # 只跑指定模块
```

- 目录分三类：`core/`（核心库，约 46 个模块：协议 provider/openai/anthropic、MCP 4004 行、WS 1397、会话持久化 1572、压缩 2219、中断 1539、事件桥 927、记忆增长 491…）、`plugin/`（插件框架与内置插件，约 13 个模块，含 `test_plugin_abi_c17.c` 257 行的 **C17 编译校验**）、`client/`（TUI/配置/渲染，约 21 个模块）。
- include 根唯一（`agentxx-test/`），源码写完整路径避免歧义（`AGENTS.md` 约定）。

**(2) 编译期/加载期负面测试**：

- `plugin/negative_compile/`：`stl_param_hook.cpp`、`wrong_capability_return.cpp`、`wrong_hook_return.cpp`、`wrong_polled_return.cpp`、`wrong_tool_return.cpp` + `positive_control.cpp`——**故意写错的插件源码必须编译失败**，由 CMake 断言（这类测试在 C ABI 项目里非常关键）；
- `plugin/dso_plugins/test_start_fail` / `test_client_start_fail`：真实 `.so` 插件在 `start` 阶段失败，验证宿主回滚；
- `test_plugin_multi_instance.cpp`（450 行）：同库多实例并存契约。

**(3) 消毒器与探针**：Debug 默认开 `AGENTXX_ENABLE_SANITIZER`（ASan + UBSan 一体，MSVC 仅 ASan），并给插件框架加定向探针（`lib/src/plugins/*`、`test/plugin/*`）；Release 下两者都自动关闭（否则缺 UBSan 运行库符号会链接失败）。

**(4) 资源基准**：见 §14.2（`agentxx_benchmark`，模块化场景 + smaps 分解 + JSON 基线对比），另有 `test_memgrowth.cpp` 做内存增长回归。

**(5) 其他质量手段**：插件平台支持门禁（`plugin_platform_support.cmake` 按平台跳过编译）、插件导出符号控制（version script / exported_symbols_list / `-fvisibility=hidden`）、LTO/ICF 体积优化与实测记录、`AGENTS.md` 里沉淀的"常见问题"（如 MSVC `/FS /MP` 陷阱、ICE 处理）。

### 16.3 对比与结论

| 维度 | harness | agentxx |
|---|---|---|
| 框架 | Go 标准 `testing`（+ `httptest`、`mocks/`） | 自研 `agentxx_test`（模块化、可筛选、可 fail-fast） |
| 规模 | 288 文件 / 6.97 万行 | 173 文件 / 8.51 万行 |
| 测试分层 | 单元（controller/store/types）+ 端点 + 一致性 + 前端 E2E + 负载 | 模块内单测/集成 + 负面编译 + 插件 DSO 实测 + 基准 |
| 契约测试 | registry conformance（真实包管理器客户端） | provider 协议用例（模拟上游响应）+ ABI C17 编译校验 |
| 环境 | `.local.env`/`.test.env` + `GITNESS_E2E_TEST_ENABLED` 开关跳过 E2E | 测试内构造环境（临时目录/内存库），无外部依赖 |
| 静态检查 | golangci-lint（14KB 配置）+ govulncheck + modernize | 编译器警告 + sanitizer + 负面编译 |
| 资源回归 | 无 | **有**（基准 JSON + 基线对比） |
| 手动联调 | `.testapi/*.http` | TUI 内直接测 |
| CI 门禁 | Makefile 全套（generate/lint/sec/test） | 构建脚本 + 可选测试目标 |

**harness 的优点**：①**一致性测试**（用真实 npm/maven/cargo 客户端打服务）是协议实现最有力的验证；②`mocks/` 生成物 + 假授权器等**测试替身**体系让上层测试不需要真实依赖；③门禁动作齐全（格式化即门禁、漏洞扫描、mock/wire 生成物必须最新）；④E2E 用环境变量开关，默认不拖慢本地。

**agentxx 的优点**：①**负面编译测试 + 真实 DSO 插件测试**覆盖了 harness 完全没有的领域（ABI 约束、插件加载/回滚/多实例），这在 C++/ABI 项目里是极高质量的实践；②**资源基准作为一等公民**（分模块 smaps、基线 JSON 对比），别的项目普遍缺失；③自研测试框架支持模块筛选与 fail-fast，单模块可独立跑，适合本地快速迭代；④include 根唯一 + 完整路径的约定避免了同名头文件歧义；⑤测试覆盖了真实并发/取消/中断/持久化等边界（如 `test_cancel.cpp` 712 行）。

**agentxx 的缺点**：①**缺"真实外部客户端"型一致性测试**：provider 测试是模拟上游响应（可能"自说自话"），MCP 也没有用真实 MCP server 做端到端；②缺**门禁自动化**（没有 CI 脚本把"测试 + sanitizer + 基准阈值 + 负面编译"串成一条命令/流水线）；③测试替身偏手写（缺少统一的"假会话/假权限/假 provider"工厂）；④前端/渲染类断言依赖文本比对，缺少"渲染快照"机制（harness 有 jest 快照的另一侧）。

**可迁移设计（§16）**

1. **CI 门禁脚本（P0）**：新增 `agent/script/ci_check.sh|bat` 串起：构建（Debug）→ `agentxx_test --fail-fast` → 负面编译目标 → 基准跑一次与基线比对（阈值）→ 可选 sanitizer 跑，任一失败即非零退出。
    - 落地：`agent/script/`。
2. **真实外部依赖端到端（P1）**：为 MCP/ACP/A2A 增加"起一个最小真实对端"的端到端测试（例如用 `cxx_utilxx` 的 http/ws server 起桩 MCP server，或用 Python 的参考实现作为可选依赖 + 环境变量开关），避免只测模拟响应。
    - 落地：`agent/test/core/test_mcp.cpp` 扩展 + `agent/test/include/agentxx-test/core/`。
3. **渲染快照测试（P1）**：TUI 渲染已有纯文本降级（`plainText`）与 `measureItem`，可把"组件 → 文本快照"作为快照测试基线（新增/修改组件时差异可见），覆盖 harness 中 jest 快照的等价需求。
    - 落地：`agent/test/client/test_tui_ui_items.cpp` / `test_ui_items.cpp` 扩展 + 快照文件目录。
4. **测试替身工厂（P2）**：统一 `FakeSession` / `FakePermissionPrompter` / `FakeProvider` 等（与 §10 建议 5 合并），减少各模块重复搭桩。
    - 落地：`agent/test/include/agentxx-test/core/`。
5. **基准阈值门禁（P1）**：给 `--baseline` 增加 `--fail-on-delta-rss=2MB` 之类的阈值参数，作为资源回归门禁（当前只能人工比对 JSON）。

---

## 17. 构建、依赖与发布

### 17.1 harness：单 Go 模块 + 前端构建 + 容器/Helm 分发

- **语言与模块**：`go.mod` 声明 `go 1.26.6`，`require` 块内约 229 行依赖声明（直接与间接依赖同块）；工具依赖单独放 `go.tool.mod`/`go.tool.sum`（`tools`/`go:build tools` 模式），避免工具污染主依赖图。
- **构建入口**：`Makefile`
    - `make web-build`（`pushd web && yarn install && yarn build`）→ `web/dist`；
    - `make build`（依赖 `generate` → `wire`）产出单个 `gitness` 二进制，前端经 `//go:embed` 打进二进制；
    - 代码生成被纳入门禁：`wire`/`force-wire`（`scripts/wire/gitness.sh`）、`generate-mocks`、`ar-api-update`（制品库 API）、前端 `yarn services`（OpenAPI → TS）。
- **依赖工具链版本固定**：`make dep`/`tools` 安装固定版本（README 要求 protoc 3.21.11、protoc-gen-go v1.28.1、protoc-gen-go-grpc v1.2.0）；`go.tool.mod` 里可升级（`make govulncheck-update` 会改它）。
- **分发**：`Dockerfile` 三段构建（`node:16` 构前端 → `golang:1.26.6-alpine` 构后端 → `alpine` + 证书镜像），另有 `Dockerfile.uiv2`；`charts/gitness` 提供 Helm chart（deployment/ingress/pvc/service/serviceaccount）；README 给出 docker run 一行命令（挂 docker.sock + 数据卷）。
- **仓库治理**：`scripts/license/insert-license-headers.sh`（统一插入 Apache 头，`.license-extensions.txt` 指定范围）、`scripts/security/govulncheck.sh`、`scripts/coverage/test_script.sh`、`.githooks`（pre-commit 格式校验）、`.github/workflows/ci-lint.yml`（目前 CI 主要做 lint）。
- **跨平台**：Go 交叉编译天然支持（README："支持 Go 支持的所有 OS/架构"）；运行时差异用环境变量表达（如 `GITNESS_DOCKER_HOST`）。

### 17.2 agentxx：CMake superbuild + 24 个第三方 + 静态复用库 + 插件符号控制

- **构建系统**：顶层 `agent/CMakeLists.txt` 判定平台/编译器宏（`XX_IS_LINUX_D/WIN/MACOS/ANDROID/IOS`、`XX_IS_MSVC/GCC/CLANG/MINGW`）并经 `_AGENTXX_COMMON_CMAKE_ARGS` 下发到各嵌套构建（lib/client/test/benchmark/plugins/third_party），保证"同一套开关贯穿 superbuild"。
- **依赖**：`agent/third_party/` 24 项，其中三个是**自研独立工程**（`cxx_utilxx_base` / `cxx_utilxx` / `cxx_pluginxx`，与主程序同一 superbuild 构建，但也可独立发布；构建变量统一 `XX_*` 前缀，未传入时各自回退默认值）；第三方含 Boost、FTXUI、NeoGraph、simdjson、sqlite3、openssl、curl、hyperscan、liburing、mimalloc、cmark-gfm、markdown-ui、yaml-cpp、fmt、zlib-ng 等。
- **条件依赖的处理方式（值得记下）**：工具库导出接口里**只声明库名**（`PkgConfig::hyperscan` + 裸库名 `hs_runtime` / `PkgConfig::uring`），不含路径；使用方（lib/client/test/benchmark/plugins）必须**先按开关 `pkg_check_modules` 出这些目标，再 `find_package` 工具库**；插件目录查找一次即覆盖全部插件目标。
- **插件构建**：
    - 导出符号控制：ELF `-fvisibility=hidden` + version script 白名单（通配符 `agentxx_plugin_agent_*` / `agentxx_plugin_client_*`）、macOS `-exported_symbols_list`、MSVC 仅 `dllexport`；
    - 平台门禁：`plugin_platform_support.cmake` 按平台跳过编译（screen_capture/computer_use/text_selection_monitor 仅 Windows，audio_stream 全平台未实现，system_monitor 覆盖 win/linux/android/macos）；
    - 复用库静态链入：插件 `target_link_libraries(PRIVATE cxx_utilxx_base_static cxx_utilxx_static)`，符号经导出控制隐藏，与宿主互不冲突；未引用模块自动裁剪（9 个插件 `DT_NEEDED` 仅系统库）。
- **优化**：Release 默认 LTO（全部产物无例外，含 hyperscan 改基线 ISA 以参与 LTO）；ICF（`--icf=all`，仅 mold/gold/lld）实测体积 -2.7%~-3.2%；strip 由发布脚本完成。
- **内存分配器**：mimalloc 默认关闭（有 Windows/Linux 实测依据），`AGENTXX_MIMALLOC_LINK=STATIC|SHARED` 可选，接入逻辑集中在 `agent/cmake/agentxx_mimalloc.cmake`（STATIC 用 `-Wl,-u,malloc` 让程序自身定义进入动态符号表，避免跨模块 free 不匹配）；sanitizer 开启时自动关闭；macOS/iOS 自动关闭（Mach-O 两层次命名空间，静态覆盖在跨模块释放会崩）。
- **构建脚本**：`agent/script/{linux,macos}_{debug,release}_build.sh`、`windows_*_build.bat`、`cross_android_release_build.sh`；脚本末尾 `strip` 并复制产物到 `*-output` 目录。AGENTS.md 记录了"编译输出过滤只抓关键词"的习惯用法与 MSVC `/FS /MP` 陷阱等经验。

### 17.3 对比与结论

| 维度 | harness | agentxx |
|---|---|---|
| 构建系统 | Makefile + go build + yarn | CMake superbuild（多子项目） |
| 依赖数量 | 229（单一语言生态，包管理器统一） | 24 个第三方 + 3 个自研库（构建系统各异） |
| 语言混合 | Go + TS（构建期混合，运行期单语言） | C++（+ C ABI 插件），构建期与运行期都混合 |
| 产物 | 单二进制（含前端） + Docker 镜像 + Helm | 可执行/动态库/静态库 + 插件 `.so`/`.dll` |
| 生成物门禁 | wire/mocks/swagger 生成纳入 `make build`/`test` | 无（手写为主） |
| 跨平台 | Go 天然交叉编译 | 需平台矩阵脚本 + `XX_IS_*` 宏 + 平台门禁 |
| 体积优化 | 无（二进制较大但可接受） | LTO + ICF + strip + 静态库裁剪，实测数据记录 |
| 发布 | Docker/Helm/二进制 | 脚本产出 + 多平台构建 |
| 条件依赖 | 无（依赖图由 go.mod 静态确定） | 显式开关（hyperscan/io_uring）+ 目标存在性校验 |

**harness 的优点**：①**工具依赖与主依赖分离**（`go.tool.mod`）避免工具版本污染；②生成物（wire/mocks/swagger）是构建的一部分，**不会出现"忘记重新生成"**；③容器 + Helm 让部署标准化；④许可证头脚本与格式 hook 降低了协作成本。

**agentxx 的优点**：①**条件依赖只声明库名、由使用方解析**这一设计很好地解决了"superbuild 里开关组合爆炸"的问题（AGENTS.md 有明确说明）；②插件符号控制方案完整（三平台 + 通配符 + Android lld 兼容），这是 C ABI 插件项目必须解决的问题；③体积与内存优化都有实测记录（LTO/ICF 收益、mimalloc 取舍、THP 影响），不是拍脑袋；④静态复用库让"宿主与插件可独立编译、版本不同也能加载"；⑤平台矩阵脚本 + 平台门禁让"同代码多平台"可控。

**agentxx 的缺点**：①**构建时间长、门槛高**（superbuild + 24 第三方 + LTO），新人上手成本明显高于 `make build`；②没有"生成物门禁"（协议表、UI 组件 schema 等手写件缺校验，容易漏改，与 §5/§15 建议呼应）；③缺少打包/分发脚本（无 Docker/Helm 等价物，也无"一键产物包"），跨平台发布靠人工记忆脚本组合；④CMake 变量传递层数多（`_AGENTXX_COMMON_CMAKE_ARGS` + 各库 `XX_*` 回退），排查构建问题需理解多层约定。

**可迁移设计（§17）**

1. **生成物门禁（P1）**：把"协议常量表、UI 组件 schema、插件接口表版本"等手工维护项纳入构建校验（例如 CMake 目标 `check_generated` 跑一个 C++ 工具比对头文件与实现/JSON schema 是否一致），并接到 CI 门禁脚本（§16 建议 1）。
2. **一键发布打包（P1）**：提供 `agent/script/package.sh|bat`：构建 → strip → 收集可执行 + 插件 + 默认配置 + 许可证 → 打 zip/tar.gz（包含各平台）。对标 harness 的 Docker/Helm 在"本地程序"语境下的等价物。
3. **构建时间治理（P2）**：记录并公开"从零构建"与"增量构建"耗时基线（与 §14 的资源基准同思路），把"改一行等 20 分钟"这类问题量化；可选启用 `ccache`/`sccache` 支持。
4. **依赖升级策略（P2）**：为 24 个第三方定义"升级检查 + 版本记录"位置（例如 `agent/third_party/README.md` 记录版本、来源、补丁、升级注意事项），对标 harness 的工具版本固定与更新流程（`update-tools`）。

---

## 18. 错误处理与上下文传递

### 18.1 harness：双轨错误 + 分类状态 + 上下文携带

**(1) 两套错误类型，用途不同**

- **库级分类错误**（`errors/` 包）：`Error{Status, Message, Err, Details}`，`Status` 是**机器可读枚举**（`conflict` / `internal` / `invalid` / `not_found` / `not_implemented` / `unauthorized` / `forbidden` / `failed` / `precondition_failed` / `aborted` / `unprocessable_entity`）；带链式 setter（`SetErr`/`SetDetails`）与解包函数（`AsStatus` / `Message` / `Details`），并提供泛型助手：`IsType[T error]`、`AsType[T error] (T, bool)`（"一行绑定并判定"）。
    - `errors/stderr.go` 只是对标准库的**薄转发**（`New/Is/As`），意义在于让业务代码统一从本包导入、未来可替换。
    - **非应用错误一律 `StatusInternal`**（`AsStatus` 的注释），即"忘了分类 → 默认内部错误"，失败姿态安全。
- **用户可见错误**（`app/api/usererror`）：`Error{Status int, Message, Values}`，30+ 具名常量，由 `usererror.Translate(ctx, err)` 本地化，`render.TranslatedUserError` 负责映射到 HTTP。
- 两层之间用 `errors.As` 桥接：库层给出分类，端点层决定"给用户的措辞与状态码"。

**(2) 上下文传递**

- `context.Context` 贯穿一切 IO（DB/HTTP/git/Hook），取消与超时统一；
- **请求级元数据也走 ctx**：`request.WithAuthSession`（会话）、`dbtx`（事务句柄）、`logging`（日志注解）、`audit`（审计上下文）、`principal`；
- `contextutil.WithNewTimeout(ctx, timeout)`：`context.WithTimeout(context.WithoutCancel(ctx), timeout)`——**"脱离父取消但保留值"**，用于"父请求被取消后仍需完成的清理/上报"（这类需求在服务端很常见，注释一句话讲清）。
- 事务句柄走 ctx（§6）使"业务不感知自己在事务里"成为可能；同时 `dbtx.runnerDB.WithTx` 会在 `sql.ErrTxDone` 时把 ctx 的错误还原（避免"取消了却报事务已结束"）。

**(3) 边界与失败姿态**

- `panic` 在作业执行、调度循环、流消费者三处被转成错误并带 stack（§4）；
- 端点层 `render` 对"响应已开始"的情形有明确规定（写头前可报错，写头后只能收尾，§5）；
- git 特殊场景用"HTTP 200 + 错误体"（`GitHookRestrict` 在未认证时返回 `hook.Output{Error}`，因为 git 不打印错误信息）。

### 18.2 agentxx：异常分类 + 控制流语义 + 统一工具错误格式

- `agentxx/util/exception.h`（244 行）+ `utilxx_base/exception.h`：`ExceptionClassification{isControlFlow, controlKind, errInfo, exPtr}`；`catchError<T>` / `catchErrorAsync` 系列**放行取消与中断**，只把真正的错误交给错误处理分支；`installExceptionClassifier()` 把 neograph 的控制流注册到通用库的追加分类器，使 http/ws 等第三方库内部捕获时也认这套语义（"否则图引擎的取消/中断异常会被当作普通错误吞掉"）。
- `isCancelAbort(e, cancelToken)`：把"取消导致的 `operation_aborted`"识别为取消（而不是超时/传输错误）——这是 asio 项目常见的误判点，被显式处理。
- 工具错误约定：**参数检查失败抛 `std::invalid_argument`、运行期错误抛 `std::runtime_error`**，由 `ToolcallWrapNode` 统一格式化为结果文本 `[Exception aborted: <msg>]`；插件侧在 SDK 边界捕获并上报 FAILED（取消上报 CANCELLED）；**不返回 `{"error": ...}` 形态**（约定写进 AGENTS.md）。
- 取消的语义完整性（§12）：取消后未完成的 tool call 补 `[User canceled]`；中断用 `NodeInterrupt` 走 resume。
- 与图的交互：`neograph::graph::CancelledException` / `NodeInterrupt` 是控制流异常，图引擎据此决定"回滚并重跑"或"resume 到节点"。

### 18.3 对比与结论

| 维度 | harness | agentxx |
|---|---|---|
| 错误模型 | 双轨（分类错误 + 用户错误），分类是枚举 | 三分类（控制流取消/中断 + 普通异常）+ 文本 |
| 机器可读性 | `Status` 枚举 + `Details` map | 依赖异常类型与文本（wire 无 code，§5） |
| 传递 | `context` 携带会话/事务/日志/审计 | `CancelToken` 携带取消；事件消息携带 sessionId/agentName |
| 取消语义 | ctx 取消（Go 惯例，随处可检查） | 异常 + `isCancelAbort` + 放行机制 |
| 事务/日志上下文 | 走 ctx（对业务透明） | 显式参数/成员（`AgentContext`） |
| 默认失败姿态 | 未知错误 = `StatusInternal`（不泄露细节） | 未分类异常 = 错误结果 + 日志 |
| 跨边界格式 | HTTP 状态码 + JSON 消息 | 统一 `[Exception aborted: msg]` 或 `[Permission denied]` |
| 特殊端点 | git 用 200+错误体（客户端限制） | 无对应问题 |

**harness 的优点**：①**分类枚举 + Details** 让错误可编程处理（重试/降级/展示分派），比"字符串匹配"健壮得多；②"未知错误默认内部错误"避免把内部细节泄漏给用户；③`context.WithoutCancel` 这类 helper 把常见的"清理必须完成"需求变成一行；④错误与用户提示彻底分离，本地化在单独一层。

**agentxx 的优点**：①**把取消/中断建模成控制流**（不是错误）在 asio 协程场景下更正确——`operation_aborted` 的误判被专门处理，且能穿透所有统一捕获；②异常分类器可**注入**（`setExtraExceptionClassifier`），第三方库也认这套语义，设计上很干净；③工具错误格式统一（`[Exception aborted: ...]`），模型与用户都能识别，且取消/拒绝有专门文案；④"会话是唯一权威"让取消后的上下文仍然完整（已完成的 tool 结果不丢）。

**agentxx 的缺点**：①**跨边界缺少机器可读错误码**（wire `error` 无 code，插件接口表状态码只有 0/非 0 语义），客户端只能按文本判断（§5 建议 1）；②取消/中断/超时/拒绝四类"非成功"结果在文本层区分，程序化区分需靠异常类型（跨插件边界就丢了）；③"异常 → 结果文本"的转换在 `ToolcallWrapNode` 一处完成，其它路径（事件总线服务、插件内部调用）可能不一致；④没有 `Details` 式的结构化补充信息（例如"权限拒绝时给出被拒路径与原因"，§10 建议 1）。

**可迁移设计（§18）**

1. **统一错误对象（P0，与 §5、§10 合并）**：定义 `agentxx::util::AppError{code(枚举), message, details(map), cause}`；工具/节点/插件边界一律用它（toolcall 的文本格式化改为 `AppError::toText()`），wire 与插件接口表都带上 `code` 与可选 `details`（JSON 字符串）。
    - 落地：`agent/lib/include/agentxx/util/exception.h` + `plugin/api/plugin_api.h`（状态码扩展）+ `nodes/toolcall.cpp`。
2. **"清理必须完成"的上下文（P1）**：为 offload/IO 提供"脱离取消但保留会话上下文"的执行包装（等价 `context.WithoutCancel`），用于落盘、审计写入、事件上报等收尾工作；避免"取消时数据没落盘"。
    - 落地：`agent/lib/include/agentxx/util/cancel_adapter.h` + `session_store.cpp` 写入路径。
3. **失败姿态默认值（P1）**：明确"未分类异常 = 内部错误（对模型只暴露简短说明，对日志保留 stack）""权限/隔离类拒绝 = 明确文案 + 理由"两类默认行为，写入 AGENTS.md 与测试（避免各处自由发挥）。
4. **非成功结果的程序化区分（P2）**：为工具结果增加 `result_status` 元数据（success / denied / canceled / timeout / error），既用于 UI 着色，也用于统计与重试决策（对齐 harness 的 `Status` 与 `JobState`）。

---

## 19. 迁移建议汇总

按"能独立落地、能单独验证"的原则整理。优先级判据：**P0** = 涉及安全/数据正确性/明显体验缺口，且改动边界清晰；**P1** = 结构性问题，收益大但需要设计；**P2** = 打磨与远期。

> 深读（第二部分）后，本清单有 4 项被调整（P0-8 降为 P1、P0-11 目标更明确、P1-6/P1-25 收敛），并新增 P0-12 与 P1-26~P1-30，详见 [§33.2](#332-新增与调整的迁移项)。下表保留原编号以便交叉引用。

### 19.1 P0：安全与数据正确性

| # | 建议 | 为什么（harness 对应做法） | 落地位置 | 验证方式 |
|---|---|---|---|---|
| P0-1 | **权限声明全覆盖**：`execute_command` / `computer_use` / `screen_capture` / `text_selection_monitor` / `websearch` / `rag_search` / MCP 桥接工具全部声明权限作用域（新增 `ProcessExec` / `NetEgress` / `DeviceCapture`），并在宿主侧对"有副作用但未声明"的工具记 warning/按配置拒载 | harness 的 `Authorizer` 默认"未明确允许即拒绝"，且权限枚举覆盖每个资源动作 | 各插件 `start`；`agent/lib/src/plugins/plugin_manager_vtable.cpp`；`middlewares/permission.h` | 扩展 `test_plugins.cpp`：逐个工具断言"未声明即不出现/被拒"；新增"有副作用未声明"的负面用例 |
| P0-2 | **出网策略**：新增 `utilxx_base/net_policy.h`（地址分类 + `Policy`），接入 HTTP/WS 客户端建连前校验与外进程目标的解析校验；默认只允许公网，配置可放开 | `netpolicy` 的分类枚举 + `ControlFunc` 拨号钩子 + 统一错误（不泄露内网状态） | `agent/third_party/cxx_utilxx_base/include/utilxx_base/net_policy.h`；`cxx_utilxx` 的 `http_client`/`ws_client`；`agentxx-config.yaml` | 单测覆盖分类边界（IPv4-mapped、6to4/Teredo/NAT64、link-local）+ 端到端（拒绝后不产生连接）；`websearch` 插件集成用例 |
| P0-3 | **权限判定结构化 + 审计**：`PermissionDecision{decision, reason, rule, target}`；`action_audit` 表记录每次判定与工具执行 | harness 的 `audit/` 留痕 + `Authorizer` 返回值语义明确 | `middlewares/permission.h` / `permission.cpp`；`session_store.cpp`（或独立 `audit.db`） | 单测：每条优先级分支产生唯一 reason；审计表可回读并支持删除"记住的选择" |
| P0-4 | **统一错误对象 + wire 错误码**：`AppError{code, message, details}`，工具/节点/插件边界统一使用；wire `error` 与插件接口表带 `code` | harness 的 `errors.Status` 枚举 + `usererror` 双轨；`AsStatus` 未知即 internal | `util/exception.h`；`agent/io/wire_protocol.h`；`plugin/api/plugin_api.h`；`nodes/toolcall.cpp` | 往返测试（每个 code 可解析）；客户端依据 code 的展示分支测试 |
| P0-5 | **持久化后台作业 + 调度循环 + 超时预算**：作业表（state/deadline/progress/attempt/group）、启动时超期回收、执行前落库、结束后按策略重排；工具/命令/子代理统一挂到作业上 | `job.Scheduler` + `jobOverdue` + `Executor.ProgressReporter` + `postExec` 重排规则 | 新增 `agent/lib/{include/agentxx/job,src/job}/`；`nodes/toolcall.cpp`；`tools/subagent.cpp`；`plugins/agentxx_execute_command` | 单测：容量、重排、超期回收、取消；集成：进程被杀后重启能看到"未完成作业" |
| P0-6 | **迁移框架 + checkpoint**：`schema_meta` 版本表 + 有序迁移 + 迁移前备份；关闭会话库前 `wal_checkpoint(TRUNCATE)` | harness 的 `migrate` 版本表 + Before/After 回调 | `util/sqlite.h`（新增 `migration.h`）；`src/util/`；`session_store.cpp`；`settings_db.cpp` | 迁移单测（老库 → 新库、失败回滚、重复执行幂等）；WAL 文件在关闭后归零 |
| P0-7 | **工具并发（只读）与并发上限**：只读类工具同轮并行，写/交互类串行；结果按模型声明顺序合并；`maxParallelTools` / 分类上限可配 | harness 的 worker 拉取 + `stage`/`repo` 限流 + 后置按顺序提交 | `nodes/toolcall.cpp`（替换串行循环）；`config.h` | 并发测试（多工具耗时应接近 max 而非 sum；结果顺序稳定）；上限测试（超限时排队） |
| P0-8 | **大输出外部化 + 定位符**：超阈值工具结果写入会话 `blobs/`，上下文只放摘要 + 定位符，配套按需读取 | harness 的 `blob.Store` + `LogStore` + `types.Stream[T]` | `nodes/toolcall.cpp`；`session_store.cpp`；`agentxx_filesystem` 的 read 分页 | 单测：阈值边界、定位符可读回、上下文 token 明显下降 |
| ↳ | *深读修正*：机制**已存在**（share_store + 定位符 + 分片读 + 工具级压缩句柄），改为"统一策略 + 结构化定位符 + 附件外置"，优先级降为 P1-28（[§32 修正 4](#修正-4大输出外置与定位符已实现工具级缺统一策略)） | | | |
| P0-9 | **启动期依赖断言 + 配置回填/校验**：`resolveConfig()` 统一回填与校验，`init()` 末尾统一断言关键依赖 | harness 的 `LoadConfig` backfill + `ProvideXxxConfig` 构造期校验 | `agent/lib/src/agent/config_resolve.cpp`（新增）；`base_agent.cpp`；`config_loader.cpp` | 单测：缺 dataDir/模型无效/插件目录不可读 → 明确错误；老配置兼容 |
| P0-10 | **CI 门禁脚本**：构建 + `agentxx_test --fail-fast` + 负面编译 + 基准阈值 +（可选）sanitizer | harness 的 `make test/lint/sec/format` 全套门禁 | `agent/script/ci_check.sh|bat` | 门禁本身要能在干净环境跑通；故意破坏一处应失败 |
| P0-11 | **统一 trace/opId**：每轮 `opId`、每次工具调用 `callId` 进入日志上下文与事件消息，跨 agent/插件/总线可串联 | harness 的 `request_id` + 日志上下文模型 | `utilxx_base/log.h`（上下文字段）；`toolcall.cpp`；插件 SDK 日志接口 | 日志断言：一次调用链的所有日志含同一 opId |

### 19.2 P1：结构与体验

| # | 建议 | 说明 | 参考 |
|---|---|---|---|
| P1-1 | 装配清单化 + 窄接口绑定 | `init()` 阶段清单化（含回滚），消费者按需声明窄接口 | §1 wire 的 `WireSet`/`Bind` |
| P1-2 | 子系统配置视图 | 每个子系统一个 `XxxConfig` + `makeXxxConfig()` | §2 `ProvideXxxConfig` |
| P1-3 | 分阶段关闭 | 停止输入 → 等在跑轮/作业 → 停定时器 → 停插件 → 刷盘 | §2 关闭序列 |
| P1-4 | 后台任务错误汇聚 | `TaskScope`：任一任务致命失败即收敛取消 | §4 `errgroup` |
| P1-5 | 有界推送队列 + 丢弃策略 | delta/统计类可丢，内容类保留 | §4 SSE `select/default` |
| P1-6 | delta 序号 + sync 版本 | 客户端可检测丢帧并请求重放（深读修正：**序号已存在**，见 §33.2 → P1-26） | §5 `Last-Event-ID` 语义 |
| P1-7 | 协议表单一来源 + 往返测试 | X-macro/描述文件生成常量与映射 | §5 OpenAPI 生成 |
| P1-8 | 领域错误映射（sqlite → 语义） | `NotFound`/`Duplicate`/`Busy` 等可编程判定 | §6 `ProcessSQLErrorf` |
| P1-9 | 批量读取原语 | 多会话/多条目批量取，避免 N+1 式循环单查 | §6 `FindManyByX` |
| P1-10 | 客户端缓存一致性 | 快照版本 + 序号，失效依赖现有 delta 通道 | §7 `Evictor` |
| P1-11 | 事件持久化订阅（可选） | 关键事件游标落库 + 重试 + 丢弃计数 | §8 `events` + `Collector` |
| P1-12 | 作业容量/分组/面板 | `max_running` + group + TUI 后台任务面板 | §9 `GetJobProgressForGroup` |
| P1-13 | 作用域扩展 + 有效规则视图 | scope 枚举扩展；`listEffectiveRules(sessionId)` | §10 权限枚举 + authz 假实现 |
| P1-14 | 执行预算 | 命令/工具墙钟超时 + 输出上限 + 结构化超时结果 | §11 CI 的 worker/限流 |
| P1-15 | 执行记录外部化 | 每轮工具执行记录入会话库并可查询/导出 | §11 执行日志（DB/S3） |
| P1-16 | 附件元数据校验 | mime/size/sha256 + 来源标记 + 实际内容校验 | §13 `contentTypeForFile` |
| P1-17 | 运行期诊断入口 | `get_diagnostics` + TUI 诊断弹窗 | §14 `/healthz` + `list_config` |
| P1-18 | 组件/协议描述生成化 | 单一 schema 生成 C++ 结构与校验 | §15 OpenAPI → TS |
| P1-19 | 统一系统信息页 | 模型/插件/生效配置/权限/作业/缓存/日志路径 | §15 `/swagger` 等 |
| P1-20 | 真实外部依赖 e2e | 起真实 MCP/ACP 桩做端到端 | §16 conformance 测试 |
| P1-21 | 渲染快照测试 | 组件 → 文本快照基线 | §16 jest 快照 |
| P1-22 | 基准阈值门禁 | `--fail-on-delta-rss` | §16 门禁 |
| P1-23 | 生成物门禁 | 协议表/schema/接口表一致性校验 | §17 `make generate` |
| P1-24 | 一键发布打包 | 产物 + 插件 + 配置 + 许可证打包 | §17 Docker/Helm |
| P1-25 | 中断幂等支持 | `interruptOnce(key, fn)` 机制化防重复副作用（深读修正：**逐点幂等已存在**，见 §33.2 → P1-27） | §9 `TotalExecutions`/状态机 |

### 19.3 P2：打磨与远期

- **配置**：旧键自动迁移（写回 overlay yaml）、生效配置可查询、日志按模块开关。
- **缓存**：抽象下沉到 `utilxx_base`（容量/TTL/统计/可关）+ 命中率暴露。
- **事件**：毒消息显式出口、事件计数上报、`RequestResponseStream` 多服务端注册确定性。
- **权限**：可复用假实现（测试替身工厂）、规则可视化编辑。
- **执行**：能力不可用说明（平台/依赖原因）、环境声明结构化、标签化调度（多执行后端）。
- **存储**：只读访问路径分离、会话内容检索。
- **客户端**：界面开发生态（渲染层替换入口）、表单能力声明。
- **工程**：构建耗时基线与 ccache、第三方依赖版本台账。

### 19.4 建议实施顺序（含依赖关系）

```text
第一批（互不阻塞，可并行）
  P0-11 trace/opId ──┐
  P0-9  配置回填校验 ─┼─ 后续所有排障都依赖它们
  P0-10 CI 门禁脚本 ──┘（先跑通"测试+负面编译"，基准阈值后补 P1-22）
  P0-6  迁移框架 + checkpoint
  P0-4  统一错误对象 + 错误码
第二批（安全）
  P0-1  权限声明全覆盖
  P0-3  判定理由 + 审计（依赖 P0-1 的作用域枚举与 P0-4 的错误码）
  P0-2  出网策略（独立，可先做）
第三批（可靠性 + 体验，二者可并行）
  P0-5  作业化（超时预算/超期回收；依赖 P0-6 的存储）
  P0-7  工具并发（依赖 P0-5 的预算与 P0-1 的作用域分类）
  P0-8  大输出外部化（依赖 P0-6 的存储）
第四批（结构）
  P1-* 按 §19.2 顺序（先 P1-1/2/3/4 打基础，再 P1-10/11/12 做一致性，再其余）
```

---

## 20. 反向清单：agentxx 不必照搬的设计

对比中容易产生"harness 有我们就该有"的错觉，这一节明确**哪些差异是正当取舍**：

| harness 的设计 | 为什么不建议搬到 agentxx |
|---|---|
| **google/wire 编译期依赖注入** | agentxx 的核心扩展能力是**运行时插件**；引入编译期 DI 会让"插件在运行期注册能力"变得别扭。只要做到"装配清单化 + 启动期断言"即可获得主要收益。 |
| **Redis 化的一切**（pubsub/lock/stream/cache） | harness 需要多实例一致性；agentxx 是单进程（甚至单线程）模型，引入远端依赖会破坏"离线可用 / 免外部服务 / 可嵌入式" 的定位。事件持久化只需本地 sqlite。 |
| **RBAC + 多主体（principal/space/repo 权限矩阵）** | agentxx 是单用户本地工具，主体只有"用户 / 模型 / 插件"。搬 RBAC 只会增加概念负担；真正需要的是**作用域化的目标级判定**（已有基础）。 |
| **HTTP/REST + OpenAPI + Swagger UI** | agentxx 的交互是"会话内长连接 + 中断协商"，WS + delta 更贴合；REST 化会增加一次语义转换。可借用的是"契约生成"与"错误分类"，不是协议形态。 |
| **容器化执行世界（devcontainer/gitspace）** | agentxx 的卖点之一是"直接用本机环境"（用户本来就希望 agent 操作自己的仓库与工具链）。容器化作为**可选后端**（P2 标签化调度）可以，作为默认路径会破坏体验与平台覆盖（Android/iOS/Windows 桌面）。 |
| **多租户隔离、软删除、git hooks、代码托管域模型** | 与 agent 领域无关，属于 harness 的业务本体。 |
| **前端 React + webpack 多入口** | agentxx 的 UI 策略是"声明式组件 + 多客户端渲染"，TUI 之后的目标是 GUI 复用同一描述层；不需要引入 Web 前端栈。 |
| **作业的 cron 表达式** | agent 场景的定时需求基本是"延迟 N 秒 / 每 N 秒"，引入 cron 解析器徒增依赖；需要时再加。 |
| **服务端 push 式 SSE 广播到"所有订阅者"** | agentxx 的会话是点对点的（一个会话一个/多个客户端），广播语义会带来"谁该看到什么"的额外权限问题。 |
| **作业/事件的多实例竞争语义（globalLock、RunBy、Machine）** | 单进程不需要；但**超期回收**这一个子集值得保留（见 P0-5），因为它解决的是"崩溃恢复"而不是"多实例"。 |

---

## 21. 结语

harness 与 agentxx 处在两个坐标系里：一个把"多租户服务的长期正确性"做到极致（作业状态机、事件消费者组、迁移序列、审计、出网策略），另一个把"本地 agent 的交互完整性与资源效率"做到极致（会话权威、中断/取消语义、声明式 UI、资源基准）。

对比下来，真正可迁移的不是具体实现，而是**三条工程原则**：

1. **把"状态"从内存搬到可观察的地方**：harness 的 job/event/audit 都是"先落库再动作、可查询、可恢复"；agentxx 的长任务目前只在内存里。作业表 + 审计表是最直接的收益点（P0-5、P0-3）。
2. **把"策略"从代码里搬到声明里**：harness 用配置与权限枚举表达策略；agentxx 的权限虽然已经声明化，但**覆盖不完整**（最危险的工具没声明），且作用域只有文件读写。补全声明 = 用最小代价换最大安全收益（P0-1、P0-2）。
3. **把"失败"当成一等公民**：harness 的 `(bool, error)` 授权语义、`Status` 枚举、`context.WithoutCancel`、三处 recover、超期回收；agentxx 有很好的取消/中断控制流模型，但跨边界的错误码、预算与恢复路径仍不完整（P0-4、P0-7、P0-11）。

反过来，agentxx 有三件事明显强于 harness，值得继续保持：**运行时插件与 ABI 契约的完整度**（多实例、start 事务、导出符号控制、负面编译测试）、**取消/中断的控制流建模**（可穿透所有捕获、跨模块一致）、**资源基准与内存治理的工程化程度**（分模块 smaps、基线 JSON、真实 TUI 帧耗时）。

---

## 附录 A：关键文件与文档索引

| 模块 | harness | agentxx |
|---|---|---|
| 入口/装配 | `cmd/gitness/main.go`, `cmd/gitness/wire.go`, `cmd/gitness/wire_gen.go`, `app/bootstrap/bootstrap.go` | `agent/client/main.cpp`, `agent/client/src/mode_runners.cpp`, `agent/lib/src/agent/base_agent.cpp` |
| 启动/关闭 | `cli/operations/server/server.go`, `cli/operations/server/config.go` | `agent/client/src/mode_runners.cpp`, `BaseAgent::init/shutdownAsync` |
| 配置 | `types/config.go`, `cli/operations/server/config.go` | `agent/lib/include/agentxx/agent/config.h`, `agent/client/src/config_loader.cpp`, `util/settings_db.h` |
| HTTP/路由 | `app/router/*.go`, `http/server.go`, `http/auxilary_server.go` | `agent/lib/include/agentxx/agent/io/*`, `agent/lib/src/agent/io/ws_io_transport.cpp` |
| API 层 | `app/api/{api.go,handler/,controller/,request/,render/,usererror/,middleware/,openapi/}` | `agent/lib/src/agent/io/session_server_agent_io.cpp`, `agent/lib/include/agentxx/agent/io/wire_protocol.h` |
| 存储 | `app/store/*.go`, `app/store/database/*`, `store/database/dbtx/*`, `store/errors.go` | `agent/lib/include/agentxx/util/sqlite.h`, `agent/lib/src/agent/session_store.cpp`, `util/settings_db.h` |
| 缓存 | `cache/*`, `app/store/cache/*` | `utilxx_base/lru_cache.h`, `tools/share_store.cpp`, `client/.../lazy_scrollable.cpp` |
| 事件/消息 | `events/*`, `stream/*`, `pubsub/*`, `app/events/*`, `app/sse/*` | `agent/lib/include/agentxx/event/{event_stream.h,events.h}`, `src/event/event_stream.cpp` |
| 作业 | `job/*` | 无（见 §19 P0-5） |
| 认证/权限 | `app/auth/{authn,authz}/*`, `types/enum/permission.go`, `audit/*` | `middlewares/permission.h`, `src/middlewares/permission.cpp`, `plugin/api/plugin_api.h` |
| 执行/沙箱 | `infraprovider/*`, `app/gitspace/*`, `netpolicy/*`, `app/pipeline/*` | `plugins/agentxx_execute_command/*`, `lib/src/tools/git_worktree.cpp`, `plugins/agentxx_{computer_use,screen_capture}` |
| 存储/大输出 | `blob/*`, `app/store/logs*`, `types/stream.go` | `tools/share_store.cpp`, `middlewares/summarization.cpp` |
| 可观测 | `logging/*`, `livelog/*`, `profiler/*`, `app/services/{metric,usage,instrument}/*` | `utilxx_base/log.h`, `agent/benchmark/*`, `client/.../tui_log_sink.cpp` |
| 客户端/UI | `web/src/*`, `web/dist.go`, `cli/*`, `app/api/openapi/*` | `agent/client/src/io/tui/*`, `agent/lib/src/ui/*`, `agent/lib/include/agentxx/ui/*` |
| 测试 | `*_test.go`, `app/testing/*`, `registry/tests/*`, `tests/load/*`, `mocks/*`, `.testapi/*` | `agent/test/*`, `agent/benchmark/*`, `agent/plugins/cmake/plugin_platform_support.cmake` |
| 构建 | `Makefile`, `Dockerfile`, `charts/gitness/*`, `scripts/*` | `agent/CMakeLists.txt`, `agent/script/*`, `agent/cmake/*` |
| 文档 | `README.md`, `CONTRIBUTING.md`, `/swagger` | `docs/zh-cn/design/*.md`, `resource/history/*` |

## 附录 B：术语对照

| 概念 | harness | agentxx |
|---|---|---|
| 请求/响应单元 | HTTP request + `auth.Session` | 会话（sessionId）+ 一轮（turn） |
| 操作者 | `types.Principal`（user/service/serviceaccount/anonymous） | 用户（单主体）；模型/插件是"行为来源" |
| 作用域 | `types.Scope` + `types.Resource`（space/repo/…） | 会话（sessionId）/ 工作目录 / worktree 根 |
| 执行单元 | `stage` / `step`（容器） | 节点（node）/ 工具调用（tool call）/ 作业（job，规划中） |
| 扩展单元 | Go 包 + `WireSet` | 插件（C ABI）+ 接口表 |
| 事件 | `events.Event[T]`（事件流）；`sse.Event`（推送） | `EventStream<T>` / `RequestResponseStream<Req,Resp>`；`WireDelta`（推送） |
| 失效通知 | `Evictor`（gob key over pubsub） | delta/sync（会话内） |
| 持久化 | store 接口 + sqlx + 迁移 | `SqliteDb` + 每会话库 |
| 缓存的"键" | 资源 ID / 路径 | 会话 ID / 条目 ID / 渲染键 |
| 询问用户 | 无（403） | 中断（`NodeInterrupt`）+ `service.permission` 请求-响应 |
| 失败分类 | `errors.Status` 枚举 | `ExceptionClassification` + 控制流种类 |
| 超时 | `RunDeadline` / ctx deadline | 各子系统超时（作业化后统一） |
| 权限声明 | `types/enum.Permission` + 角色 | `ToolPermissionSpec`（插件声明） |

## 附录 C：本次精读的源码清单

**harness（Go 服务端）**

- 装配/启动：`cmd/gitness/{main.go,wire.go,driver_sqlite.go,driver_pq.go}`, `app/bootstrap/bootstrap.go`, `cli/operations/server/{server.go,config.go}`, `app/pkg.go`, `app/store/wire.go`, `app/services/wire.go`, `app/server/server.go`
- 路由/中间件：`app/router/{router.go,api_router.go,web.go}`, `app/request/request.go`, `app/api/{api.go}`, `app/api/render/{render.go,render_error.go,sse.go}`, `app/api/usererror/usererror.go`, `app/api/middleware/{authn/authn.go,authz/authz.go,address/address.go,principal/principal.go,encode/encode.go}`, `app/api/handler/{space/events.go,gitspace/events.go,upload/download.go,system/health.go}`
- 存储：`store/errors.go`, `store/database/{store.go,util.go,util_sqlite.go,dbtx/interface.go,dbtx/runner.go,dbtx/locker.go}`, `app/store/{database.go,cache.go}`, `app/store/cache/{wire.go,space_id.go,evictor.go}`, `app/store/logs.go`, `app/store/logs/db.go`, `app/store/database/{migrate/migrate.go,database.go,encode.go}`
- 基础设施：`cache/{cache.go,lru_cache.go}`, `lock/*`（目录级）, `pubsub/pubsub.go`, `events/{events.go,reader.go,reporter.go,system.go,wire.go,options.go,error.go}`, `stream/{stream.go,options.go,memory_consumer.go}`, `job/{scheduler.go,types.go,definition.go,executor.go,job_overdue.go}`, `netpolicy/netpolicy.go`
- 领域/服务：`app/services/{webhook/service.go,trigger/service.go}`, `app/auth/authz/authz.go`, `app/auth/authz/*`（目录级）, `types/enum/permission.go`, `types/{config.go,pagination.go,stream.go}`, `app/sse/sse.go`, `app/pipeline/{scheduler/{scheduler.go,queue.go,canceler.go},runner/{runner.go,poller.go}}`, `infraprovider/infra_provider_factory.go`, `blob/{interface.go,config.go}`
- 运维/测试：`logging/logging.go`, `profiler/profiler.go`, `livelog/livelog.go`, `errors/{status.go,stderr.go,util.go}`, `contextutil/contextutil.go`, `tests/load/init.go`, `web/{dist.go,package.json}`, `Makefile`, `.golangci.yml`（目录级）, `charts/gitness/*`, `scripts/*`（目录级）

**agentxx（C++ agent）**

- 核心：`agent/lib/include/agentxx/agent/{base_agent.h,config.h,session_store.h}`, `agent/lib/include/agentxx/middlewares/{middleware.h,permission.h,subagent_manager.h}`, `agent/lib/include/agentxx/tools/{tool.h,subagent.h,share_store.h}`, `agent/lib/include/agentxx/nodes/graph_conditions.h`, `agent/lib/include/agentxx/util/{exception.h,sqlite.h}`, `agent/lib/src/nodes/toolcall.cpp`（关键区段）
- 协议/事件：`agent/lib/include/agentxx/agent/io/wire_protocol.h`, `agent/lib/include/agentxx/event/{event_stream.h,events.h}`
- 插件/执行：`agent/plugins/agentxx_execute_command/{agentxx_execute_command.cpp,execute_command_impl.h,execute_command_env.h}`（头部与注释）, `agent/plugins/agentxx_filesystem/agentxx_filesystem.cpp`（权限声明处）
- 测试/基准/客户端：`agent/test/test.cpp`, `agent/test/**`（目录级统计）, `agent/benchmark/**`（目录级）, `agent/client/src/io/tui/**`（目录级），`docs/zh-cn/design/{index.md,benchmark.md,plugins.md,tui.md,ui-layer.md}`（目录级统计）

## 附录 D：可以继续深入的清单

| 位置 | 尚未精读的部分 | 能回答的问题 |
|---|---|---|
| `app/api/controller/*`（28 域） | 事务边界与事件发布的组合方式（`dbtx.WithTx` 内发事件、`*OptLock` 乐观锁） | agentxx 若要"写入会话 + 发事件"原子化，可参考哪些写法 |
| `app/services/*`（40+ 服务） | 事件消费者服务的通用骨架（`common.go`）、重试策略差异 | 事件处理服务的模板化程度 |
| `registry/*`（742 文件） | 制品库的多后端存储、GC、配额、上传并发 | 大对象存储与配额治理（对应 §13） |
| `git/*`（157 文件） | git 数据面抽象、`last_commit_cache`、hook 流程 | "把外部命令当服务"的接口设计（对 `execute_command` 有参考） |
| `app/gitspace/orchestrator/container/*` | devcontainer 解析与容器生命周期细节 | 若将来做"可选沙箱执行后端"，可借鉴哪些抽象 |
| `app/services/protection/*` | 分支保护规则引擎（规则 DSL + 违规结果） | 规则声明 + 违规结果结构化的实现方式（对权限规则可参考） |
| `web/src/**` | 前端状态管理与 SSE 接入方式 | 未来 GUI 客户端的组件化与事件投影 |
| `agent/lib/src/middlewares/summarization.cpp`（1265 行） | 压缩策略与缓存前缀重建 | §13 与 §12 的结合点（长上下文与大输出） |
| `agent/lib/src/plugins/client_plugin_manager.cpp`（4278 行） | 客户端插件装载/接口协商/双端入口探测 | 客户端插件与宿主的一致性保障 |
| `agent/lib/src/protocol/{mcp_client.cpp,mcp_server.cpp}` | MCP 的传输与工具映射细节 | §11 的"外部执行世界"权限如何覆盖 |

---

# 第二部分：源码实现细节深读

> 这一部分是对第一部分的**下钻**：不再停留在"谁有这种机制"，而是逐行核对**机制是怎么实现的、边界条件写在哪里、踩过什么坑**。凡是与第一部分结论不一致的，在 [§32](#32-对第一部分的修正与补强) 集中列出修正。
>
> 深读范围：harness 的资源寻址 / 状态机与规则引擎 / 作业与事件落地 / 外部世界接入（§22~§25）；agentxx 的一轮会话、中断恢复、上下文压缩、会话状态、插件运行时、渲染缓存（§26~§31）。

## 22. 深读：harness 的资源寻址与一致性

### 22.1 一个 space 可以有多个路径：`space_paths` 表的真实形状

`app/store/database/space_path.go` 揭示了 harness 寻址模型的核心（比"树 + 唯一路径"更灵活）：

    type spacePathSegment struct {
        ID               int64     `db:"space_path_id"`
        Identifier       string    `db:"space_path_uid"`         // 原始标识（大小写保留）
        IdentifierUnique string    `db:"space_path_uid_unique"`  // 变换后标识（唯一索引用）
        IsPrimary        null.Bool `db:"space_path_is_primary"`   // true=主路径, nil=别名路径
        ParentID         null.Int  `db:"space_path_parent_id"`    // NULL=根层段
        SpaceID          int64     `db:"space_path_space_id"`
        CreatedBy/Created/Updated ...
    }

三个关键设计：

1. **路径 = 段的链表**：每段只存"父段 ID + 自己的标识"，整条路径由 `FindByPath` 按段逐级 `SELECT` 拼出（`paths.Concatenate`），并在每层累计 `isPrimary`（全 primary 才是主路径）。因此**重命名一段不必改写所有后代路径**——这也是删除子树必须用 `WITH RECURSIVE` CTE（`DeletePathsAndDescendandPaths`）的原因。
2. **别名路径**：`space_path_is_primary` 用 `null.Bool` 而非 `bool`，注释解释得很直接：
   > "to allow DB enforcement of at most one primary path per repo/space we have a unique index on spaceID + IsPrimary and set IsPrimary to true for primary paths and to **nil** for non-primary paths."
   即：用唯一索引 `(space_id, is_primary)` 保证"最多一条主路径"，别名以 `NULL` 参与唯一索引（SQL 中 NULL 不冲突）——**用可空列把"至多一个真"塞进唯一索引**，是很实用的关系库技巧。
3. **大小写策略由可注入变换函数决定**：`store.SpacePathTransformation` 在装配层给出（`app/store/wire.go` 给的是 `ToLowerSpacePathTransformation`），写入与查询两侧都过同一函数（`mapToInternalSpacePathSegment` 写 `IdentifierUnique = f(Identifier, isRoot)`），因此"是否大小写不敏感"是**配置而非硬编码**。

### 22.2 路径语义的边界条件（`app/paths/paths.go`）

- `DisectLeaf("space1/space2/space3") → ("space1/space2", "space3")`，`DisectRoot` 相反——repo 引用总是"父 space 路径 + repo 标识"两段。
- `Concatenate` **清理首尾与连续分隔符**（逐 rune 状态机），保证 `space1/ + /space2/ → space1/space2`，避免 `space1//space2` 这类只在部分校验路径下暴露的脏值。
- `IsAncesterOf` 的注释说明了一个常见错误：
  > "append the separator to both sides so that an ancestor relation requires a full leading segment match: this rejects prefixes that aren't segment boundaries (space1 vs space10, space1/in vs space1/inner) as well as paths that merely appear as a trailing segment of other (space2 vs space1/space2)."
  即"祖先判断必须按段边界，而不是字符串前缀"——这类缺陷在权限/可见性判断里是安全级问题。

### 22.3 refcache：把"引用消解"从每个 handler 里拿出来

`app/services/refcache/{space_finder.go,repo_finder.go}` 是薄聚合服务，本身不含缓存实现，只组合缓存与失效器：

    type RepoFinder struct {
        repoStore      store.RepoStore
        spacePathCache store.SpacePathCache
        repoIDCache    store.RepoIDCache
        repoRefCache   store.RepoRefCache
        evictor        cache.Evictor[*types.RepositoryCore]
    }

    func (r RepoFinder) FindByRef(ctx, repoRef string) (*types.RepositoryCore, error) {
        repoID, err := strconv.ParseInt(repoRef, 10, 64)            // ① 数字 → 直接当 ID
        if err != nil || repoID <= 0 {
            spaceRef, repoIdentifier, _ := paths.DisectLeaf(repoRef)
            spacePath, _ := r.spacePathCache.Get(ctx, spaceRef)      // ② 路径 → spaceID
            key := types.RepoCacheKey{SpaceID: spacePath.SpaceID, RepoIdentifier: repoIdentifier}
            repoID, _ = r.repoRefCache.Get(ctx, key)                 // ③ (spaceID, 标识) → repoID
        }
        return r.repoIDCache.Get(ctx, repoID)                        // ④ repoID → RepositoryCore
    }

深读得到的四个结论（第一部分未写到的）：

1. **引用形态有两种**：数字 ID 或路径，且都接受（`ParseInt` 成功即当 ID）——这解释了 API 中 `repoRef` 既能写 ID 也能写 `space1/repo1`。
2. **缓存链是"逐级消解"而非"整串 key → 对象"**：路径 → spaceID → (spaceID,标识) → repoID → Core；每一级独立命中，多个 repo 共享同一 space 的解析结果。
3. **`MarkChanged` 只做一件事**：把 Core 交给 `evictor.Evict(ctx, core)`（gob 编码 + pubsub 广播），由**各实例自己订阅失效**（见 §7.3）。因此多实例一致不需要共享缓存。
4. **低频高敏操作绕过缓存**：`FindDeletedByRef` 直接查库（软删除对象按 `deleted` 时间戳查），说明"删除/恢复"这类操作被显式排除在缓存之外。

### 22.4 乐观锁：`UpdateOptLock` 重试循环 + 版本列

`app/store/database/{space.go,repo.go}` 里核心就是一个可重试闭包：

    for {
        dup := *space                        // ① 在快照上拷贝
        if err := mutateFn(&dup); err != nil { return nil, err }
        err = s.Update(ctx, &dup)            // ② UPDATE ... WHERE version = ?
        if err == nil { return &dup, nil }
        if !errors.Is(err, gitness_store.ErrVersionConflict) { return nil, err }
        space, err = s.find(ctx, space.ID, space.Deleted)   // ③ 冲突则重读再试
    }

配套细节：

- `Update` 自增 `version` 并作为 `WHERE` 条件，冲突返回 `ErrVersionConflict`——**冲突是"可判定错误"而不是字符串**（这正是 §6.3 建议 2 的来源）。
- 两个变体承载**前置条件**：`UpdateOptLock`（要求未删除，否则 `ErrResourceNotFound`）与 `updateDeletedOptLock`（要求已删除）。
- `FindForUpdate` 只在 postgres 上加 `FOR UPDATE`：`if strings.HasPrefix(s.db.DriverName(), "sqlite") { return s.find(...) }`，注释"sqlite allows at most one write to proceed (no need to lock)"——与 `dbtx` 的"非 postgres 走全局 RWMutex"是同一判断的两处体现。
- 业务侧把它当作"读-改-写"的复合操作入口（`SoftDelete` 直接 `UpdateOptLock(..., func(r){ r.Deleted = &deletedAt })`）。

### 22.5 与 agentxx 的对照

| 机制 | harness | agentxx | 深读结论 |
|---|---|---|---|
| 寻址 | 段链表 + 主/别名路径 + 可注入大小写变换 | 会话 ID（原始字符串）+ 清洗后的目录名 | agentxx 无需多路径，但"显示形态与唯一键分离"的做法（`sanitizeSessionId` + 原始 ID 映射）值得保留 |
| 引用消解缓存 | 逐级消解 + 独立失效广播 | 无（按 ID 直取） | 只有在引入"会话别名/重命名"时才有意义 |
| 并发写 | 乐观锁（版本列 + 重试闭包） | 互斥锁串行化（`SessionStore::mutex_`） | 单进程串行更简单；但 `settings_db` 会被多客户端并发写，**值得**用版本列表达冲突 |
| 前置条件 | `*OptLock` / `updateDeletedOptLock` 明确"必须存在/必须已删" | 无 | 建议把"会话必须存在/必须未完成"这类前置条件显式化（与 §18 错误码建议合并） |

**新增可迁移项（D22）**

1. **设置库并发写用版本列（P1）**：`settings_db` 现由多客户端（TUI，未来 GUI）并发写；加 `version` 列并在冲突时返回"已被其他客户端修改"，客户端用统一重试闭包（对齐 `UpdateOptLock`），避免静默覆盖。
2. **显示形态与唯一键分离（P2）**：会话目录名已清洗；若将来支持**会话重命名**，应像 space 路径那样"保留原始标识 + 变换唯一键"，不要直接改写目录名（会让历史路径失效）。

---

## 23. 深读：harness 的领域状态机与规则引擎

### 23.1 合并队列：checks 驱动的状态机（`app/services/mergequeue/`）

文件名即状态机骨架：`mq_process.go`（主推进）、`mq_checks.go`/`handlers_checks.go`（外部检查回调驱动）、`mq_fast_forward.go`、`mq_merge_commit.go`、`mq_reset.go`、`pull_request_{enqueue,prioritize,remove,is_enqueued}.go`、`job_overdue_checks.go`。

从 `handlers_checks.go` 与 `job_overdue_checks.go` 读出的机制：

- **状态**：`ChecksPending` → `ChecksInProgress` → …；外部 CI 上报的检查通过 `ChecksCommitSHA` 与队列条目关联。
- **队首才推进**："Only act on success checks for entries that are the checks leader."（`if entry.State != MergeQueueEntryStateChecksInProgress { ... }`）——队列天然串行，后面的条目即使检查通过也要等。
- **完成判定**：取该 SHA 的全部检查（`largeLimit = 10000`，注释"need all reported checks"），对 `mergeQueueSetup.RequiredChecks` 逐个比对；`BypassedByID != nil` 的**视为满足**（与上报路径同口径）。
- **超时由作业兜底**：`job_overdue_checks.go` 注册为作业 `gitness:merge-queue:overdue-checks`，扫描 `ListOverdueChecks(now)`，对每个超期条目拉取名下检查并**把第一个未完成项写进日志字段**（`incompleteCheckIdent`/`incompleteCheckLink`）——排障时直接看到"卡在哪个检查"。
- **单一 deadline**：注释 "A merge queue entry has a single deadline for all MQ checks to complete."——用一个 deadline 代替每检查一个计时器，简化状态与存储。

**启示**：agentxx 的作业化（§9 P0-5）不该只做"超时 kill"，还要做**"卡在哪一步"的结构化记录**（哪个工具/哪个子代理/哪条命令），否则超时后用户只看到一句话。

### 23.2 checks 模型：外部状态回注的统一入口

`app/api/handler/check/*`（list / recent / report）+ `app/services/checkreq` + `types/check` 构成"外部系统上报状态"的入口：`FindByIdentifier(repoID, commitSHA, checkIdent)`、`Status.IsCompleted()`、`Link`（详情链接）、`BypassedByID`。

agentxx 的对应物是"工具/子代理/命令的执行状态"，目前只有结果文本（`[Exception aborted]` / `[Permission denied]` / `[User canceled]`），缺"结构化状态 + 可跳转详情"。深读后 §18 建议 4（`result_status`）可以更具体：**状态 + 详情定位**（文件路径#行号 / share_store id / 作业 id），并允许 UI 依状态着色与折叠。

### 23.3 保护规则引擎：Definition / Verify 分离 + 结构化违规（`app/services/protection/`）

| 文件 | 角色 |
|---|---|
| `service.go` | `Manager{defGenMap map[enum.RuleType]DefinitionGenerator, ruleStore}`；`Register/FromJSON/SanitizeJSON/ListRepoXxxRules/FilterXxxProtection` |
| `verify_*.go` | 验证动作族：`RefChangeVerifier`、`PushVerifier`、`MergeVerifier`、`CreatePullReqVerifier`、`MergeQueueBranchUpdateVerifier`、`DeleteSourceBranchGetter`、`MergeQueueSetupGetter` |
| `rule_{branch,tag,push}.go` | 规则实体（一个规则 = 若干 Definition 组合） |
| `set_{branch,tag,push,common}.go` | 规则集合：按顺序跑多条规则并汇总 violations |
| `pattern.go` / `repo_target.go` / `bypass.go` / `validators.go` / `json.go` | 名字模式（globstar）、作用目标、绕过判定、校验、序列化 |

三个关键设计：

1. **Definition 是"可序列化策略数据"，Verify 是"纯函数判定"**：`Definition` 只有 `Sanitize()` 与 `SupportsParent(enum.RuleParent)`；`FromJSON` 在严格模式用 `DisallowUnknownFields()`（把"字段写错"变成显式错误），宽松模式用于读旧数据。判定统一签名 `(ctx, Input) ([]types.RuleViolations, error)`，`Input` 显式携带 `actor / allowBypass / isRepoOwner / refAction / refType / refNames / resolveUserGroupID`——**判定无副作用，所需输入全部入参**。
2. **违规带机器可读代码**：`violations.Addf(codeLifecycleCreate, "Creation of tag %q is not allowed.", ...)`，代码形如 `lifecycle.create` / `lifecycle.update.force`；`RuleViolations` 连同规则信息与 `Bypassed` 一起向上返回，最后由 `GenerateErrorMessageForBlockingViolations` 统一拼用户消息（取第一条阻塞规则，但若后面有更详细内容则改用它；消息含规则类型、标识、作用域与首条违规文本）。
3. **规则可分层继承**：`SupportsParent` 校验"该规则能否定义在此父级"，`ListOnlyRepoRules` 与 `ListRepoRules` 区分"只看本仓库"与"含父级继承"。

**对 agentxx 的直接映射**：agentxx 的权限规则目前是运行时状态（`XXRouter` + map）。若要按 §19 P0-3 做"判定结构化"，可直接采用 harness 的形状：

    PermissionRuleDefinition(Sanitize/SupportsParent)
      → PermissionVerifier(ctx, Input{tool, target, scope, actor, session}) → []PermissionViolation{code, message}
      → PermissionDecision{allow|deny|ask, reason, violations}

**新增可迁移项（D23）**

1. **规则定义与判定分离（P1，强化 P0-3）**：把 `filesystemPermission` 的注册项升级为可序列化定义（`{scope, target, operator, source(配置|用户记住|隔离|模式), createdAt, createdBy}`），判定走纯函数并产出带 `code` 的 violation。收益：规则可导出/导入、可审计、可用构造 Input 单测（无需真实会话）。
2. **违规消息生成集中化（P2）**：像 `GenerateErrorMessageForBlockingViolations` 一样统一"多条违规怎么拼给用户/模型"（agentxx 现在各处自行拼 `[Permission denied]`），日志保留全部违规。

---

## 24. 深读：harness 的作业与事件落地细节

### 24.1 job store 的三处守卫，才是状态机的真正实现

`app/store/database/job.go` 的 SQL 条件里藏着三条不变量（第一部分只写了"状态机"）：

    // ① 进度只能在"运行中"更新，防止把已结束作业改回 running
    UpdateProgress:  UPDATE jobs SET ... WHERE job_uid = :job_uid AND job_state = 'running'

    // ② 清理只删终态且非周期性的旧作业
    DeleteOld:       job_state IN (finished, failed, canceled) AND job_is_recurring = false
                     AND job_last_executed < :olderThan

    // ③ 就绪/超期/下一次时间各自独立查询
    ListReady:             job_state = scheduled AND job_scheduled <= now  (LIMIT N)
    ListDeadlineExceeded:  job_state = running   AND job_run_deadline < now
    NextScheduledTime:     job_state = scheduled AND job_scheduled > now   (LIMIT 1)

配合 `CountRunning(job_state = running)` 得到容量，整个调度循环**完全由 SQL 条件定义正确性**，进程内没有额外状态需要维护。这回答了第一部分留下的"多实例怎么不互相踩"：**用状态列 + 条件更新，而不是内存锁**。

### 24.2 领域服务如何接入作业框架（三个真实例子）

1. **清理服务**（`app/services/cleanup/service.go`）：
   ```go
   func (s *Service) Register(ctx context.Context) error {
       if err := s.registerJobHandlers(); err != nil { ... }               // executor.Register(type, handler)
       if err := s.scheduleRecurringCleanupJobs(ctx); err != nil { ... }   // scheduler.AddRecurring(uid,type,cron,maxDur)
   }
   ```
   作业类型常量与 cron 定义在包内，保留期来自 `Config`（webhook 执行/已删仓库的 retention、cleanup cron 与 `MaxDuration`），`Config.Prepare()` **拒绝零值**——"没配就报错，不猜默认"。
2. **指标收集**（`app/services/metric/collector_job.go`）：`const jobType = "metric-collector"`，`AddRecurring(ctx, jobType, jobType, "0 0 * * *", time.Minute)`（每天一次、单次上限 1 分钟）；同一服务的 `event_handlers.go`（616 行）又是事件消费者——**定时作业与事件消费并存**。
3. **合并队列超期检查**（`mergequeue/job_overdue_checks.go`）：`Handle` 只扫描 + 记录（含"卡在哪个检查"），**不代替业务决策**；状态推进仍由正常流程负责。

对 agentxx 的映射：建议的作业表应照搬这套三段式——**作业类型常量 + handler 注册 + 周期/延迟排期**，且**超时上限与保留期必须配置化且拒绝零值**。

### 24.3 事件消费者服务的"落地壳"（同一形状重复十几处）

    const eventsReaderGroupName = "gitness:<name>"
    type Config struct { EventReaderName string; Concurrency int; MaxRetries int }
    func (c *Config) Prepare() error { /* 逐字段校验 + 回填 */ }

    func New(ctx, config, <stores...>, readerFactory *events.ReaderFactory[*xxx.Reader]) (*Service, error) {
        _, err := readerFactory.Launch(ctx, eventsReaderGroupName, config.EventReaderName,
            func(r *xxx.Reader) error {
                r.Configure(stream.WithConcurrency(config.Concurrency),
                            stream.WithHandlerOptions(stream.WithIdleTimeout(time.Minute),
                                                      stream.WithMaxRetries(config.MaxRetries)))
                _ = r.RegisterCreated(service.handleEventXxxCreated)
                return nil
            })
    }

三个值得记的细节：①**构造函数里就 Launch**（装配期决定生命周期）；②`Config.Prepare()` 里的**回填**（如 `HeaderIdentity` 缺省取 `UserAgentIdentity`）；③**策略写在调用点并给理由**——`trigger` 的 pullreq 读者显式 `WithMaxRetries(0)` 并注释"retries not needed for builds which failed to trigger"。

---

## 25. 深读：harness 的外部世界接入

### 25.1 git 数据面：把外部命令包装成"内部服务"（`git/`，157 文件）

目录即分层：`api/`（对外接口）、`command/`（命令构造与执行）、`parser/`（输出解析）、`diff/`、`merge/`、`hook/`、`sharedrepo/`、`storage/`、`tempdir/`、`hash/`、`sha/`、`enum/`、`types/`；顶层按能力划分（`repo.go` 813 行、`operations.go` 449、`diff.go` 453、`tag.go` 447、`merge.go` 420、`optimize.go` 378、`pre_receive_pre_processor.go` 375、`scan_secrets.go` 229、`branch.go` 296…）。

可提炼的实现模式（对 agentxx 的 `execute_command` / `git_worktree` 有直接参考）：

- **内部 RPC 化**：`interface.go` 定义服务接口，`service.go`/`service_pack.go`/`wire.go` 负责装配；git 智能 HTTP 经 `app/router/git*.go` 转发，因此"git 操作"与"HTTP 服务"解耦。
- **推送前置处理器**（`pre_receive_pre_processor.go`）：把"推送前校验"做成流水线式预处理器，与 protection 规则引擎（§23.3 的 `PushVerifier`）联动。
- **内容侧安全**（`scan_secrets.go`）：扫描推送内容里的密钥——与权限（能否操作）互补的另一类策略。
- **维护作业化**（`optimize.go` / `repack.go` / `maintenance/`）：仓库优化也纳入后台作业体系。
- **能力边界写在注释里**：`types/config.go` 的 `Git.Trace` 注释"Currently limited to 'push' operation until we move to internal command package"。

### 25.2 静态资源与 Web 安全

- `app/router/secure.go`：把 `unrolled/secure` 的 13 个选项从配置逐项映射（AllowedHosts/SSLRedirect/STS/FrameDeny/ContentTypeNosniff/CSP/ReferrerPolicy），注释明确 **"not meant APIs"**——安全头只作用于 UI 路由，不作用于 API 路由（避免破坏客户端）。
- `app/api/middleware/goget/goget.go`：为 Go 模块代理协议（`?go-get=1`）返回带 `go-import`/`go-source` meta 的 HTML，用 `html/template`（默认转义）渲染并校验路径深度——把"非标准协议"作为独立中间件插入，而不是塞进某个 handler。
- `encrypt/aesgcm.go`：AES-GCM，`Seal(nonce, ...)` 把 nonce 前置；解密长度不足且 `Compat` 打开时**按明文返回**（"mixed-mode：库里同时有加密与未加密内容"）——**迁移期兼容策略被显式编码**，不靠运维手工处理。

### 25.3 与 agentxx 的对照与新增建议

| 场景 | harness | agentxx 现状 | 深读结论 |
|---|---|---|---|
| 外部命令 | `command/` 统一构造 + `parser/` 统一解析 + 预处理器流水线 | `execute_command` 插件自管进程/管道/取消（实现已很完整：setsid/进程组、Windows Job Object、CancelRegistry 事件驱动） | 缺的是**输出解析分层**：把"命令输出 → 结构化结果"抽成可测函数（现为 `execute_command_impl.h` 内的格式约定） |
| 内容安全 | `scan_secrets.go` | 权限中间件只回答"能否读/写"，不扫内容 | 若希望防"模型把密钥写进提交"，这是可选补强（P2） |
| 兼容迁移 | `encrypt.Compat` 新旧格式共存 | 配置层有 `merge/replace` 兼容；数据层无"兼容读旧 schema"概念 | 支撑 §6 P0-6：迁移框架应支持"兼容读旧格式"而非一次性强转 |
| 非标准协议 | goget 中间件 | wire 协议的插件事件上行/下行 | "把扩展协议做成独立层"的思路对 agentxx 插件事件通道有价值 |

---

## 26. 深读：agentxx 一轮会话的完整实现

`BaseAgent::runTurnAsync`（`agent/lib/src/agent/base_agent.cpp:774-1200`）是所有入口（TUI/CLI/远程/子代理/FFI）共用的一轮实现，读完后可以把第一部分"ReAct 循环"的描述细化成一条**带不变量的流水线**：

### 26.1 开轮：线程绑定与插件轮次边界

    auto session = co_await agentContext->getSessionAsync(sessionId);
    session->bindIoThread();
    session->assertIoThread();                                  // ① 线程不变量强制

    if (agentContext->pluginManager) {
        agentContext->pluginManager->flushPendingCleanup();     // ② 清理上一轮遗留的待摘除中间件
        agentContext->pluginManager->onTurnBegin();             // ③ 登记本轮进行中
    }
    if (!session->bus) { session->bus = std::make_shared<EventBus>(executor); }
    if (io) { io->registerOnBus(session->bus); }
    session->io = std::move(io);

- **① 线程不变量**：会话的展示历史/上下文/chainHash **只允许 io 线程读写**，`bindIoThread()` 首次绑定时生效，`assertIoThread()` 在 Debug 下 assert、Release 下记错误返回（`context.h` 注释）。这是"单线程多协程交错"能免锁的前提。
- **② 轮次边界自愈**：`flushPendingCleanup()` 处理"上一轮异常退出留下的待摘除中间件"——把插件热卸载的清理点放在**轮次边界**，而不是在卸载瞬间强拆（见 §30）。
- **③ 会话级总线**：每个会话一个 `EventBus`（中断/权限/工具事件都在上面），IO 端点 `registerOnBus(session->bus)` 后即可参与请求-响应。

### 26.2 唯一增量出口：EventBridge + `insertMessageTip`

    auto eventBridge = std::make_shared<EventBridge>(agentName, sessionId, agentContext, session, ioPtr);

    auto insertMessageTip = [&](std::string text, TipLevel level, int64_t startMs=0, int64_t durMs=0) {
        if (!session->io) { return; }                       // headless 场景不插入
        auto vm = ViewMessage::makeText(ViewMessage::Role::Tip, std::move(text), startMs, durMs);
        vm.tip->tipLevel = level; vm.collapsed = true;
        vm.id = session->appendViewMessage(vm);             // ① 先落地到会话历史（带 id）
        eventBridge->emitDelta(WireDelta{.type = InsertMessage, .message = std::make_shared<ViewMessage>(std::move(vm))});
    };

- 提示消息一律**"先 appendViewMessage（分配 id、落库节流）→ 再 emitDelta(InsertMessage)"**，保证 UI 展示、Sync 恢复、持久化三者内容一致（第二部分新增结论：这就是"UI 不做业务判断"的实现方式）。
- headless（`io == nullptr`）时直接返回：提示消息是展示用途，无处理者就不产生。

### 26.3 中断恢复入口与用户输入双写

    bool resumeInterrupt = false;
    if (false == agentContext->middlewareHandleContext->graphData.contains(sessionId)) {
        auto data = fromNeographJson(engine->get_state(sessionId).value_or(neograph::json{}));
        if (data.contains(channel_savedGraphData) && data[...].is_object()) {
            resumeInterrupt = true;
            agentContext->middlewareHandleContext->setGraphDataFromState(data[channel_savedGraphData], sessionId);
        }
    }

即：**程序重启后的"恢复未完成中断"是从图状态的 `xx_savedGraphData` 通道里重建内存态 graphData**（写入点在 §27），随后走同一套中断处理循环（首跑被跳过）。

用户消息**双写**（这是第一部分没写清的细节）：

| 写入目标 | 内容 | 用途 |
|---|---|---|
| `viewMessages`（`ViewMessage::makeText(Role::User, ..., startTimeMs)` + `attachments`）| 展示形态，带开始时间戳（会话列表 `lastActiveMs` 依赖它）| 客户端同步/展示、会话列表活动时间 |
| 会话上下文（`neograph::ChatMessage`，经 `toNeographJson` → `from_json` 转换）| LLM 形态（含 `image_urls`/`audio_urls`/`video_urls` 字段）| 调用 LLM API |

附件处理的两条路径（`base_agent.cpp`）：客户端上传的走 `dataUrl`；**服务端本地路径附件**（`dataUrl` 为空且 `pathOrUrl` 非 http(s)）由服务端读出并转 Base64——`maxBytesForMediaType(att.type)` 限制大小、无 `mimeType` 时按扩展名推断、读取与编码经 `offloadAsync` 卸载到线程池（"单附件可达数 MB，避免在 io 线程上执行导致所有会话停摆"）。类型分发后写入上下文的 `image_urls/audio_urls/video_urls`。

### 26.4 RunConfig：三个"看起来可以改但不能改"的取值

    auto cfg = neograph::graph::RunConfig{
        .thread_id = std::string{sessionId},
        .input     = neograph::json::object(),        // 上下文不播种到图状态
        .max_steps = 1 << 30,
        .stream_mode = EVENTS | TOKENS | UPDATES,     // 去掉 VALUES
        .cancel_token = cancelToken,
        .resume_if_exists = false,                    // 固定 false，注释给三条理由
    };

- **去掉 `StreamMode::VALUES`**：`VALUES` 每 super-step 发一次整段状态事件，而 `EventBridge` 只消费 `message_tip`/`messages`，上下文又已不在图状态——**去掉即每步少一次整段状态序列化**（性能与"每轮序列化大上下文"的旧问题直接相关）。
- **`resume_if_exists=false` 的理由**（注释）：①引擎 checkpoint 仅进程内存活（`InMemorySingleCheckpointStore`），真重启后无 checkpoint；②同进程内端点重建（客户端重连/切回会话）时引擎仍有 checkpoint，若为 true 会先 restore 旧控制通道，与当前会话控制数据不一致；③中断恢复走 `AgentRunner` 的 `initialResult`/`resume_async` 路径，不依赖此标志。

### 26.5 中断处理与恢复循环的调用形态

`runTurnAsync` 把"首跑 + 中断处理 + 恢复"整个循环委托给 `AgentRunner{}.run(...)`，只提供 hooks（`eventCallback`、`onInterruptTip`）与 `recovered`（重启恢复时的初始中断结果）。因此**根会话与子代理共用同一实现**（`agent_runner.cpp` 279 行，详见 §27）。

### 26.6 三路收尾：错误/取消都补"提示消息"，且不回灌上下文

    co_await agentxx::util::catchErrorAsync<bool>(
        [&]() -> awaitable<bool> { /* AgentRunner 全流程 */ },
        [&](std::string errmsg) -> awaitable<bool> {           // 错误分支
            turnResult.hasError = true; turnResult.errorMessage = errmsg;
            insertMessageTip(errmsg, TipLevel::Error); co_return true;
        },
        [&](std::string& errmsg) -> std::optional<bool> {       // 取消分支
            turnResult.hasError = true; turnResult.errorMessage = "Cancelled by user";
            insertMessageTip("[Cancel Request]", TipLevel::Info); return true;
        },
        cancelToken                                            // 让 operation_aborted 归入取消语义
    );

错误分支的注释值得抄下来：

    // 出现异常/取消时: 上下文以会话为唯一权威, 不随图状态回滚 —— 节点在抛出前
    // 已写入会话的消息 (部分完成的 tool 结果 / 兜底提示) 依然保留, 这里只需
    // 把当前上下文落盘 (轮末权威保存), 不再需要从快照回灌的恢复逻辑

### 26.7 轮末：清理、统计、权威落盘、TurnEnd

顺序（每一处都有理由）：

1. `state.remove(channel_savedGraphData)`——**中断已处理完，清掉恢复用的状态通道**（否则下次启动会误判"有未完成中断"）。
2. 计算 `durationMs`、`turnTps`（`eventBridge->takeTurnTps()`）。
3. `insertMessageTip("model · duration · timestamp")`——**轮次统计提示必须在 `flushViewMessages()` 之前插入**（注释："确保提示消息落盘持久化到 SQLite"）。
4. `session->saveLlmMessages()`（上下文整表替换，权威终态）+ `session->flushViewMessages()`（补存节流窗口内的展示消息操作）。
5. `emitDelta(TurnEnd{historyCount, tailHash, startTimeMs, durationMs, tps})`——**`tailHash` 是 chainHash 的尾哈希**，客户端用它校验自己拼出的历史与服务端一致（第一部分漏了这个机制，见 §32 修正 2）。
6. `pluginManager->onTurnEnd()`。

### 26.8 `modelcall` 节点：三处容易被忽略的契约

`agent/lib/src/nodes/modelcall.cpp`（868 行）深读后得到：

1. **单 system 消息契约**：`baseRun` 每轮用 `buildSystemPrompt(sessionId)` **就地更新**首条 system（存在则替换、否则前插），而 `build_params` 里若 `instructions_` 非空且首条不是 system 才插入——注释说明理由："两条 system 消息对 Anthropic 这类单系统提示 API 是畸形输入，对 OpenAI 家族未定义"。
2. **插件工具动态可见**：`build_params` 里 `ctxPtr->toolRegistry->appendDefinitions(tool_defs)`——"插件工具注册后，下一轮 modelcall 即随请求发送给 LLM；卸载后定义从列表消失"。**热插拔工具对模型的可见性就靠这一行**。
3. **失败路径不产生悬挂 tool_calls**：`appendAbortMessage(...)` 插入 `AutoInserted` 的 assistant 兜底消息，注释：
   > "保证末尾消息角色为 assistant 且无 tool_calls，使重试耗尽/取消路径直接以 agent_end 结束本轮，而不是把悬挂的 tool_calls 误路由回 tools 节点重复执行。"
   同时重试计数 `retry` 注释"达到配置上限后停止重试，不因部分输出重置，保证总失败次数严格不超过 `llmMaxRetry`"。取消埋点 `in.ctx.cancel_token->throw_if_cancelled("before llm call")` 放在每次调用前（重试路径也查）。异常捕获顺序也写明："`boost::exception` 需在 `std::exception` 之前捕获，同时继承两者的异常（如 `boost::system::system_error`）才能取到完整诊断信息"。

---

## 27. 深读：agentxx 的中断/恢复与幂等

### 27.1 graphData：会话级"图外内存 + 可序列化通道"

`MiddlewareContext` 用一组 **graphData key** 承载"节点执行期间需要跨中断/恢复存活的状态"，深读中出现的键（部分）：

| key | 写入方 | 作用 |
|---|---|---|
| `graphDataKey_interruptNode` / `_interruptValue` | AgentRunner | 记录中断位置与载荷；重启恢复用 |
| `graphDataKey_interruptArgs` | 中断点（如 BaseAgent 的中断处理循环写入侧） | 待处理的 HIL 参数列表 |
| `graphDataKey_interruptResult` | AgentRunner | 用户应答的 resume 值 |
| `graphDataKey_interruptToolcallCache` | ToolcallWrapNode | 中断前已完成的工具结果缓存（避免恢复重复执行） |
| `channel_savedGraphData` | AgentRunner / runTurnAsync | **graphData 整体序列化进图状态**（崩溃恢复的唯一落点） |
| `graphDataKey_tempLLMThinking` / `_tempLLMContent` | modelcall | 流式期间的临时内容（重试前清理） |
| `graphDataKey_LLMTokenUsage` | modelcall | 上次 API 返回的 token 用量（压缩判定优先用它） |
| `graphDataKey_summarizationTipMsgId` | summarization | "正在压缩"提示消息 id（**续跑时复用，不重复插入**） |
| `graphDataKey_summarizationLastMsgCount` / `_summarizationFailCount` | summarization | 冷却判定输入 |

**因此 agentxx 的"幂等"不是框架能力，而是"每个中断点自己在 graphData 里记状态"的纪律**：summarization 记提示消息 id、toolcall 记已完成结果、AgentRunner 记中断位置。这条纪律需要在文档里写清（并见 §32 修正 3）。

### 27.2 中断处理循环的六步（`agent_runner.cpp`）

    while (result.has_value() && result->interrupted) {
        ① 记录 interruptNode / interruptValue 到 graphData
        ② 把 graphData 写进 state 的 xx_savedGraphData 通道（防中断处理期间进程退出）
        ③ 遍历 interruptArgs：
             - name == "subagent" → ctx->bus 请求 service.subagent（**超时传 0 = 不限制**：
               子代理可能长时间运行，总线默认 30s 会截断长任务）
             - 其它 → HIL：hooks.onInterruptTip 插提示 + session->bus 请求 service.interrupt
               （超时取 IO 端点 interruptTimeout，<=0 不限；避免用户长时间未响应丢失中断）
        ④ 收集 resumeValues：
             - subagent 结果里有 cancelled 字段 → 抛 CancelledException（不写回，避免当摘要用）
             - HIL 应答 resultJson 含 "__cancelled__" → 抛取消
             - 无处理者/未响应 → 不写回（外层按"中断未完成"处理）
        ⑤ resumeValues 非空：清 interruptArgs、写 interruptResult、fOnBeforeResume（落盘）
           → 取消检查（resume 前取消则直接抛）→ resume_async(resumeCfg, ...) → fOnRunResult
        ⑥ resumeValues 为空 → 置 unresolvedInterrupt 并退出循环
    }

四个"踩过坑"的注释（这些是文档里最有价值的部分）：

- **`resumeCfg` 必须带 `cancel_token`**：
  > "否则 resume 出的新 run 无取消能力，后续 llm/toolcall 的取消埋点（if cancel_token）全部跳过，执行中 HTTP 也无法被打断，表现为'压缩完成后怎么都停不下来'。"
- **`resumeCfg` 必须沿用首跑的 `stream_mode`/`max_steps`**：首跑 `cfg` 已被 move 进 engine，因此**提前拷贝**这两个字段（否则事件回调/步数预算漂移）。
- **不能用 `result->interrupted` 判定"中断未完成"**：
  > "while 的退出条件已保证此时 interrupted 必为 false（死条件），会使'中断未完成'被调用方（子代理 spawnOneTask）当作成功结果继续使用。"
- **取消 vs 中断语义不同**：取消会重新从图起点执行（会话不受回滚影响，已完成工具结果保留）；中断 resume 到本节点继续。因此"取消后必须补 `[User canceled]`"（§12）。

### 27.3 子代理委派的"扁平化"

根会话与每个子代理都在自己的 `ctx->bus` 上 `registerServer(service.subagent / service.subagent.execute)`，因此**嵌套委派与根委派走同一条路径**（`parseSubagentBatchFromInterrupt` + `buildSubagentResumeValues` 为两侧共享实现）：

- resume 值的 key 规则统一为 `makeSubagentResumeKey(toolCallId, resultId, idx)`——"写入侧（中断处理循环）与读取侧（`SubAgentManagerTool::execute_async`）必须使用同一函数"，注释明确"消除 BaseAgent / AgentHost / SubAgentManagerTool 三处重复的 key 规则与参数组装，防止规则漂移"。
- 结果形态有讲究：单任务返回**纯文本**，多任务按序号编号，错误任务写成 `{"error": ...}`；注释提醒"正常结果必须用圆括号直接初始化为标量字符串（而非 `{}` 列表初始化，否则会命中 `initializer_list` 构造产生 `["content"]` 数组包裹，破坏读取端'单任务返回纯文本'语义）"——**一个 C++ 初始化语法陷阱被写进注释**。

---

## 28. 深读：agentxx 的上下文压缩实现

`summarization.cpp`（1265 行）是 agentxx 最"厚"的中间件，深读后的完整流程：

### 28.1 触发点与 token 口径

    // 每次 modelcall 前（onModelcallRunFunc）
    const auto& currentModelConfig = agentCtxPtr->getSessionCurrentModelConfig(sessionId);
    size_t modelContenxtMaxToken = currentModelConfig.modelContenxtMaxToken > 0
                                 ? currentModelConfig.modelContenxtMaxToken
                                 : modelSupportMaxTokenDefault;
    enableCountThinking = currentModelConfig.sendThinking;

    size_t apiTokenUsage = graphData(graphDataKey_LLMTokenUsage);   // ① 优先 API usage
    const auto countTokenUsage = countTokens({}, messages, enableCountThinking);
    const auto tokenUsage = (apiTokenUsage > 0) ? apiTokenUsage : countTokenUsage;   // ② 否则启发式

- 注释解释了 API usage 为什么"可能不准"：
  > "接口返回的 token usage，可能不准确，因为 llm node 重试时可能会额外附加消息、也可能是上一轮的 api 返回的，本轮开始已经添加了 toolcall / userInput 等消息。"
- 统计结果直接写进跨线程的 `session->contextStats`（`contextTokens` / `maxContextTokens`），UI 因此能显示"上下文占用百分比"（`ContextStats` 是 `std::atomic` 字段，唯一允许跨线程读的会话状态）。
- 模型级覆盖：`modelContenxtMaxToken` 来自模型配置（`config.h` 的注释："0 表示未指定，此时使用默认值"）。

### 28.2 阈值与"确定性压缩先行"

    if (tokenUsage >= modelContenxtMaxToken * 0.75) {
        doSummarizeToolcall(messages);      // ① 工具调用去重 + 探索型调用折叠
        cleanNoiseMessages(messages);       // ② 噪音清理（isNoiseMessage / isSameMessage）
        ...
    }

也就是**先做不需要 LLM 的确定性压缩**（去重、折叠、清噪），再看是否还需要 LLM 摘要——这是"能省则省"的设计：多数长会话靠 ①② 就能把 token 降下来。

### 28.3 冷却与降级硬截断（本模块最关键的设计）

    coolDownActive = (failCount == 0 && lastSummarizedMsgCount > 0
                      && messages.size() <= lastSummarizedMsgCount + 2);
    if (coolDownActive) { /* 跳过重复 LLM 压缩 */ }

注释给出理由：

    // 冷却检查: 若上次压缩后消息增长不足 (<= 2 条) 且当前消息数仍 >= 75% 上限，
    // 说明普通的 LLM 摘要无法把消息数降下来，为避免每轮 modelcall 都反复派生 subagent
    // 做无效压缩, 直接降级硬截断以彻底释放空间

即：**"压缩无效"本身是被检测的状态**（`failCount` + `lastSummarizedMsgCount`），并据此降级到 `hardTruncate`——这是第一部分"压缩"章节完全没有的实现细节，也解释了为什么重复压缩不会变成"每轮都烧一次 subagent"。

### 28.4 切分与压缩段构造

- `splitRecentByTokenBudget(messages, systemCount, recentBudget)`：按 token 预算切出"最近的保留段"，`recentBudget = modelContenxtMaxToken * recentTokenBudgetRatio`。
- 送给 LLM 的压缩段 = `system + 旧段（不含 recent）`；`downgradeMultimodalUrlsToText(toSummarize)` 先把 Base64 多模态 URL 降级为纯文本标签，注释带 TODO："替换前存储为文件，记录路径"（说明作者已知 Base64 进摘要浪费 token，只是尚未落地外置）。
- LLM 压缩走**同上下文子代理**（`doSummarizeWithLLM(..., direct=false)`）——中断后由 Session 派生并 resume，因此压缩过程本身也能被取消/中断。

### 28.5 工具级压缩句柄（`SummarizationToolHandle`）

`XXToolBase::createSummarizationToolHandle()` 让**每个工具声明自己的压缩语义**，share_store 的例子很典型：

    generateDeduplicationKey = 参数 (id, line_offset, line_limit) → "share_store:{id}:lo:{lo}:ll:{ll}"
    truncateRequest  = 把过期 tool_call 参数替换为 "[Outdated Message Truncated]"
    truncateResponse = 把过期响应替换为 "[Outdated Content truncated]"，
                       并打上 flag ShareStoreTruncated | Outdated

即：**"过期的 share_store 读取"被识别为可安全丢弃的内容**（下次模型需要时再读），而普通对话/工具结果不会被这样处理。这比"统一截断"精细得多，也是 §13 建议 1（大输出外置）可以直接复用的机制。

---

## 29. 深读：agentxx 的会话状态与持久化

### 29.1 `Session` 的四条不变量（`context.h` 类注释逐条落实）

| 状态 | 约束 | 实现 |
|---|---|---|
| `viewMessages`（展示历史，append-only，永不压缩） | 仅 io 线程读写 | `appendViewMessage` + `assertIoThread()` |
| `messages`（LLM 上下文，可压缩） | 仅 io 线程；所有变更走 `appendMessages/replaceMessages/truncateMessages` | `messagesVersion` 单调递增（注释：中断/异常快照与回滚依赖该版本号） |
| `chainHash` | 仅 io 线程（随 append 更新） | `HashInfo{count, tailHex}` → TurnEnd delta 的 `historyCount`/`tailHash` |
| `contextStats` | **跨线程可读** | `std::atomic` 字段（UI 需要读，只有它被允许跨线程） |
| `deltaSeq` | 仅 io 线程递增；新 delta 必须经 `nextDeltaSeq()` 分配 | 服务端增量重放缓冲依赖 seq 单调性 |

UI 线程不能直接读会话状态：**UI 的取消/切模型等操作经 Wire 消息（`WireCancel`/`WireSelectModel`）发到 agent 线程执行**（`context.h` 注释）。这与第一部分"客户端只做 UI 渲染"的说法一致，但机制更明确：**跨线程通信只有 Wire 一条路，读也走 Wire（Sync/Delta）**。

### 29.2 双写与节流：`persistThrottled` + 轮末权威保存

- `appendMessages(msgs, persistThrottled = true)` / `replaceMessages(...)` / `truncateMessages(count, ...)`：默认请求一次**节流落盘**（`session->requestSaveLlmMessages()`），modelcall 更新 system 消息时显式传 `false`（"系统提示词属每轮重建的派生内容，由轮末权威保存落库"）——**不是所有变更都值得写盘**。
- 轮末：`saveLlmMessages()`（整表替换）+ `flushViewMessages()`（补存节流窗口内的展示消息操作）；注释明确"进程中途被杀最多丢一个节流窗口（< 3s）的增量"。
- 展示消息的**回填**：工具执行结束后 `updateViewMessage(msg)`（按 `msg.id` 定位行），使"重启恢复的历史与内存状态一致"（§6.2 提过，这里补上它是**成对**的：append + update）。

### 29.3 `SessionStoreHooks`：用回调把 sqlite 依赖反转出去

    struct SessionStoreHooks {
        std::function<void(const ViewMessage&, uint64_t msgIdCounter)> onAppendViewMessage;
        std::function<void(const ViewMessage&)>                        onUpdateViewMessage;
        std::function<void(const utilxx_base::Json&)>                  onSaveLlmMessages;
    };

`SessionsManager` 创建 `Session` 时注入这些回调（"解耦 sqlite 依赖"），因此 `Session` 本身不依赖存储实现——**测试与嵌入式宿主可以给空实现**，会话逻辑照常运行。这是 agentxx 里少见的"依赖倒置"用法，值得在其它子系统（权限规则、作业、审计）沿用。

---

## 30. 深读：agentxx 的插件运行时与执行体

### 30.1 生命周期接缝的五个动作（`plugin_manager_lifecycle.cpp`）

| 接缝 | 做什么 | 深读要点 |
|---|---|---|
| `createInstance(name)` | 只设置 `self`/`manager` | 元信息/宿主控制块由内核骨架 `makeInstance` 统一装配，宿主不重复 |
| `detachDomainRegistrations(inst)` | 注销工具 → **撤销工具权限声明** → `graphNodeType.slot->invalidate(inst)` → `restorePromptBackup` → `middleware->disabled = true` | "只摘除宿主侧生效的注册，保留实例内的注册记录"（启用时由 start 事务重新声明）；权限声明**随工具一起撤销** |
| `detachDomainOwnedResources(inst)` | `eraseMiddleware` + `resourceApplier->setOwnerEnabled(inst->name, false)` | 中间件句柄与"资源启用标记"是实例专属资源 |
| `clearDomainRegistrations(inst)` | stop 成功后清空工具/权限/hook/图记录 | "避免下次 start 在旧记录上重复累积" |
| `releaseInstanceResources(inst)` / `onInstanceEnabledChanged(...)` | 释放工具对象列表 + 清资源所有权；同步启用标记 | 资源应用器（Skill/Memory/MCP 等清单资源）按所有者记账 |

**析构的保守策略**（写得很直白的工程取舍）：

    PluginInstance::~PluginInstance() {
        if (lifecycleStopPending()) {
            // stop 从未执行：此时 destroy 会看到不完整的插件状态。析构无法"保留"对象本身，
            // 因此宁可泄漏上下文与 DSO，也不能调用插件 destroy/dlclose。
            XX_LOGE("Plugin `{}` destroyed with lifecycle stop pending; keeping plugin context and DSO loaded ...");
            return;
        }
        if (!destroyPlugin()) {
            // 析构不能绕过仍在运行的插件代码。此处宁可保留动态库句柄，
            // 也不能在 worker/callback 仍可能执行时 dlclose。
            XX_LOGE("Plugin `{}` destroyed while {} lease(s) remain; refusing to dlclose", ...);
            return;
        }
        ...
    }

即：**"泄漏"优先于"未定义行为"**——这是 C ABI 插件宿主必须明确的失败姿态（也解释了为什么测试里要覆盖 `test_start_fail` 这类场景）。

### 30.2 配置驱动装载：拓扑排序 + 内嵌清单

    loadConfiguredPlugins(plugins):
        过滤 enabled / sides != Client
        for each: 解析 depends
            - builtin://<name>  → 先查内嵌清单（parseBuiltinManifest）
                                  命中则用其 depends；否则探测 exe 目录/plugins/<name>/plugin.yaml
            - 目录路径          → parsePluginManifest 取 name/depends
            - 其它路径          → pluginNameFromPath
        ordered = topoSortPlugins(items)        // 依赖顺序
        for each ordered: loadPluginAsync(path, cfg, allowClientOnlySkip = true)

三个设计点：①**内置插件用内嵌清单**（无需文件系统探测即可确定依赖，回退探测保证开发态可用）；②**拓扑排序**保证被依赖者先加载（与内核的级联依赖/卸载互补）；③`allowClientOnlySkip=true` 让"只在 client 侧实现的插件"在 agent 侧被安静跳过（`PluginSide::Client` 语义）。

### 30.3 执行体：`polled_tool` 与取消的物理实现（`execute_command_impl.h`，914 行）

这是"命令执行不占线程"的全部机制：

- **声明式受控轮询**（`polled_tool`）：协程跑在**插件实例本地 reactor**（桥的 `local_executor`）上，管道/子进程/计时器都绑定该 executor；宿主 IO 线程经 driver 请求 `poll_one` **有界步进**（"有进展立即续，无进展退避，无未完成操作时不驱动"）；并发多条命令共享同一条驱动序列与同一个 reactor。关闭 `AGENTXX_ENABLE_BOOST_PROCESS` 时的 popen 回退是阻塞实现，走 `blocking_tool`（offload 工作线程）。
- **进程组清理**：Linux 子进程经 `setsid` 启动（pgid == pid），`kill(-pid)` 整组清理；**macOS/iOS 无 `setsid(1)`**（Linux util-linux 专有），因此在 exec 前用 `setpgid(0, 0)` 达到同语义（`NewProcessGroup`，作为 boost.process v2 的 initializer 传入）；Windows 用 Job Object + `TerminateJobObject` 整树清理。
- **取消监听协程的生命周期语义**（注释里写明是修过的 bug）：
  > "动作完成后不立即结束，而是挂起直至被并行组取消：`||` 组合下'任一先完成即整体完成并取消其余'，若本协程在 kill 后立刻返回，会在主工作组装结果前把它整体取消（丢失输出）；挂起让主工作自然收尾…（历史 bug: detached watcher + RAII guard 互相死等）"
  并且 **watcher 只终止、不 `async_wait`**："boost.process v2 的 `async_wait` op 内部引用共享的 `exit_status_` 成员，与主协程并发 wait 会产生数据竞争"——子进程回收由主协程负责。
- **取消是事件驱动而非轮询**：经 `CancelRegistry`（已提升为插件框架通用基础设施）回调触发 kill 与关管道，"彻底代替 20ms 轮询与跨线程同步 post 到 io 线程造成的严重抖动"。

### 30.4 输出格式：插件自己压缩，stdout/stderr 分开

    // 禁用 ToolcallNode 的自动压缩，改由自己实现压缩, 分别独立对 stdout、stderr 压缩
    // 格式:
    // [ExitCode]0
    // [StdOut][Content offloaded. xxx]
    // xxx
    // [StdErr][Content offloaded. xxx]
    // xxx
    //
    // - 这是为了方便 LLM 判断是否存在 [StdErr] 内容，由 ToolcallNode 压缩时如果 [StdOut] 过长，
    //   可能导致 [StdErr] 全部被裁剪隐藏，LLM 需要额外读取全量才能判断是否存在 [StdErr] 内容

即"整体截断会掩盖错误输出"这个问题被识别并解决：**结构化分段 + 各自独立截断 + 给出 `[Content offloaded]` 定位符**（与 `ToolcallNode::execTool` 的截断格式同源，见 §32 修正 4）。

---

## 31. 深读：agentxx 的客户端渲染与缓存

### 31.1 `OwnedReflect`：把 FTXUI 的 Box 所有权绑到元素上

`agent/client/include/agentxx-client/io/tui/framework/owned_reflect.h` 的注释把问题与解法写得很清楚：

> "FTXUI 的 `reflect(Box&)` 只保存**引用**，Box 必须比元素活得久。行模型/命中登记的元素常被接入点搬进滚动容器、消息块缓存（可能跨帧存活），而生成它的 `UiRow::box` / 命中登记项往往随局部结果析构 —— 一旦只搬元素，元素随后参与布局时就会写已释放内存（ASan: heap-use-after-free，栈顶为 `ftxui::Reflect::SetBox`，`layoutAndMeasure` 为调用方）。"
> "本节点把 Box 的所有权绑在元素上：移动/缓存元素即等于带走 Box，接入点无需额外保存。"

实现就是把 `shared_ptr<Box>` 作为成员，`SetBox` 写回、`Render` 与 stencil 求交——**语义与 FTXUI 原生 `Reflect` 完全一致**，只是生命周期不短于元素。接入点（`renderItem`/`mergeTextButton`、`UiHitRegistry::add/addRegions`）必须经它绑定。

这条对 agentxx 的意义：**"缓存渲染结果"与"引用外部生命周期"天然冲突**，解法不是"禁止缓存"，而是把被引用对象的所有权一并交给缓存对象。

### 31.2 `LazyScrollable`：三档预算 + 锚点模型

    struct CacheBudget {
        size_t maxItems = 256;                    // 条数上限
        size_t maxBytes = 16 * 1024 * 1024;       // 渲染结果内存估算上限
        size_t byteExemptThreshold = 1024;         // 豁免阈值：<=1KB 的条目不计字节预算
    };

- **文件注释明确 `sourceBytes` 的语义**："应为渲染结果的内存估算，而非源数据字节"——避免把"文本很短但渲染后很大"（如长表格）漏算。
- **豁免阈值的作用**："避免大量短条目（如状态行）过早触发字节淘汰；仍受 maxItems 条数约束"——**两类预算各管一件事**。
- **锚点模型**（类注释不变量 1）：滚动偏移 = `rowsAboveAnchor_ + anchorRow_`，未实测子项按 `quickHeight` 粗略估算计入 `totalHeight`，因此"`scrollOffset()` 只保证滚动条/预取判定够用，**不代表**视口内容对应的高度"；吸附底部时严格等于 `totalHeight() - viewportHeight()`。
- 与 `measureItem`（内部渲染一次）配合：**测量与布局同源**，但"缓存帧快照不随动画推进更新"（`message_list.h` 注释），所以 `cacheable` 标记要按内容形态给出。

### 31.3 与 harness 前端的对照

| 关注点 | harness（React） | agentxx（FTXUI 自绘） |
|---|---|---|
| 大列表 | 浏览器虚拟滚动（DOM 复用） | `LazyScrollable`：条数 + 字节双预算 + 短条目豁免 + 锚点定位 |
| 引用生命周期 | 框架托管（GC + 虚拟 DOM） | 手工：`OwnedReflect` 把 Box 所有权附着到元素 |
| 命中测试 | DOM 事件 + 坐标命中 | `UiHitRegion` 局部坐标 + 命中登记表（滚动容器不 `reflect` 子项） |
| 缓存失效 | React 重渲染语义 | 显式 `invalidateCache()`；动画与缓存互斥（`cacheable`） |
| 测量 | 浏览器布局 | `measureItem` 渲染一次取高度（与真实布局一致） |

**深读结论**：agentxx 的渲染层把"GC 语言里免费得到的东西"（对象生命周期、虚拟滚动、命中测试）**逐项手工实现**，并且每项都有明确的不变量与失败案例记录。这也是 §15 里"UI 层是 agentxx 的强项"的具体依据。

---

## 32. 对第一部分的修正与补强

深读源码后，第一部分有四处结论需要修正（其余结论经核实成立，前文已就地补强）：

### 修正 1：delta 序号与一致性校验——**不是缺失，而是缺"客户端侧使用"**

- 第一部分原话（§5.3）："`sync`（快照）与 `delta` 的一致性只在实现里保证，没有'版本号/序号'这样的显式契约。"
- **源码事实**：`Session` 有 `deltaSeq`（`context.h` 注释："WireDelta 流序号（单调递增；仅 io 线程读写）——由 EventBridge / `Session::nextDeltaSeq` 统一分配，服务端增量重放缓冲依赖 seq 单调性；除重放路径外，新产出的 WireDelta 必须经 nextDeltaSeq 分配"），delta 上都带 `.seq`；另有 `chainHash`（`HashInfo{count, tailHex}`），`TurnEnd` delta 携带 `historyCount` + `tailHash`，客户端可用它校验自己拼装的历史是否与服务端一致。
- **真正的缺口**：①客户端是否**校验 seq 连续性**并在缺帧时主动请求重放（服务端已有重放缓冲，但契约未文档化）；②`sync` 快照是否与 seq 配对（"这份快照对应到第 N 号 delta"）。
- 影响：§19 的 P1-6 由"新增序号"改为"**把已有序号与 tailHash 变成显式契约**（文档化 + 客户端校验 + 断点续传测试）"。

### 修正 2：轮次一致性校验机制不止 seq，还有 `tailHash`

第一部分完全没提到 `chainHash`/`tailHash`。源码事实：`viewMessages` 每次 append 都更新链式哈希（`chainHash.count()`, `chainHash.tailHex()`），轮末 `TurnEnd` delta 把二者发给客户端。因此 agentxx 的一致性模型其实是**"序号（增量顺序）+ 尾哈希（内容一致性）"两条**，比第一部分描述的更完整。

### 修正 3：中断幂等——**已有逐点实现，缺的是框架级收敛**

- 第一部分原话（§12 建议 P1-25）："为 `requestInterrupt` 的恢复路径提供'副作用只执行一次'的机制化支持…减少'恢复时重复写入'这类隐患。"
- **源码事实**：agentxx 已经用 **graphData 键**逐点实现了幂等，且注释明确写了原因。典型两例：
    - summarization：`graphDataKey_summarizationTipMsgId` 保存"正在压缩"提示的 id；续跑时**复用同一条消息**（update 而非 append），注释："第二次执行必须复用首次创建的提示消息（更新而非追加），否则每次压缩都会遗留一条永远停留在 'Summarizing LLM Context...' 的重复提示消息"。
    - toolcall：`graphDataKey_interruptToolcallCache` 缓存中断前已完成的工具结果，避免恢复后重复执行。
- **真正的缺口**：这套纪律**没有框架支持，也没有清单**——新写一个中断点很容易漏掉"记录状态 → 恢复时按状态分支"这一步。修正后的建议是"**提供 helper（`co_await interruptOnce(key, fn)`）+ 在 AGENTS.md 里给出必做清单 + 用一个测试模板覆盖（同一次中断跑两遍，断言副作用只发生一次）**"。

### 修正 4：大输出外置与定位符——**已实现（工具级），缺统一策略**

- 第一部分原话（§13 建议 P0-8）："为工具结果引入统一的 `spill` 策略…上下文里只放'摘要 + 定位符'。"
- **源码事实**：该机制已存在，只是**分散在工具与节点两处**：
    1. `ToolcallWrapNode`：当工具带 `autoSummaryOutput=true` 且 `result.size() >= toolcallSummaryLimitOutputLength` 时，调用 `addShareStoreItemValue(sessionId, result)` 寄存，并生成定位符文本：
       > `[Content offloaded. Use the agentxx_share_store tool to fetch the content by ID {}. Total {} lines, show [1, {}], hide [{}, {}].]`
       （另一种形态：`Total {} lines`，不带行号窗口）
    2. `execute_command` 插件：**禁用**节点级压缩，自己按 stdout/stderr 分别压缩，输出 `[ExitCode] / [StdOut][Content offloaded...] / [StdErr]...` 结构。
    3. `share_store` 工具支持 `line_offset`/`line_limit` **分片读取**。
    4. summarization 通过 `SummarizationToolHandle`（`truncateRequest`/`truncateResponse` + `ShareStoreTruncated|Outdated` flag）把**过期的 share_store 读取**替换成占位文本。
- **真正的缺口**：①**非** `autoSummaryOutput` 的工具（含插件工具）没有统一兜底；②阈值只有一个全局值（`toolcallSummaryLimitOutputLength`），没有按工具/按类型的策略；③附件与多模态 Base64 尚未外置（`downgradeMultimodalUrlsToText` 旁有 `TODO: 替换前存储为文件，记录路径`）；④定位符是**自然语言文本**而非结构化字段（模型/UI 都无法可靠解析，UI 也无法提供"展开原文"按钮）。
- 修正后的建议：**把已有机制收敛成显式契约**——统一 spill 开关与阈值、定位符改为结构化（`{id, lines, bytes, kind}` 写入结果 metadata）、补 UI 展开与"按行读取"入口。

### 补强 5：`shutdownAsync` 的"薄"

`base_agent.cpp:1201-1219` 的 `shutdownAsync` 只有 19 行（停止插件 + 等待），确实没有阶段化/超时分级——第一部分 §2.3 建议 3 成立，且证据更明确。

### 补强 6：`RunConfig` 已经做过一次"大上下文序列化"优化

`runTurnAsync` 里 `stream_mode` 显式去掉 `StreamMode::VALUES`，注释："VALUES（每 super-step `__state__` 全量状态事件）无人消费…去掉后每步少一次整段状态序列化"。说明"每轮把整段上下文序列化"的问题在**图状态路径**已被解决；仍需审计的是**其余路径**（对话库整表替换、Wire Sync 全量拷贝、LLM 请求体构造）——建议把"全路径序列化审计"列入 P1（结合 §14 的资源基准）。

---

## 33. 深读后的再评估

### 33.1 结论变化一览

| 模块 | 第一部分结论 | 深读后 |
|---|---|---|
| §5 协议一致性 | 缺序号/版本契约 | **有** seq + tailHash；缺"客户端校验 + 快照配对 + 契约文档化" |
| §12 中断幂等 | 缺机制化支持 | **有**逐点实现（graphData 键）；缺 helper 与清单 |
| §13 大输出外置 | 缺外置与定位符 | **已有**（share_store + 定位符 + 分片读 + 工具级压缩句柄）；缺统一策略与结构化定位符 |
| §14 trace | 无统一 trace id | 有 sessionId/agentName/tool_call_id/correlationId/delta seq；缺统一 opId 与日志字段化 |
| §9 作业化 | 建议"持久化作业 + 超时回收" | 成立；**补充**：还要记录"卡在哪一步"（对齐 merge queue overdue 的做法） |
| §6 迁移/并发 | 建议迁移框架与 checkpoint | 成立；**补充**：迁移框架要支持"兼容读旧格式"（对齐 `encrypt.Compat`）与版本列冲突语义（对齐 `UpdateOptLock`） |
| §10 权限 | 建议判定结构化 + 审计 | 成立；**补充**：规则定义可序列化 + violation 带 `code`（对齐 protection 的 Definition/Verify 分离） |

### 33.2 新增与调整的迁移项

**新增（并入 §19 的编号体系）**

| # | 优先级 | 建议 | 依据 |
|---|---|---|---|
| P0-12 | P0 | **非成功结果的结构化状态 + 卡点定位**：工具/作业/子代理的结果带 `status(success/denied/canceled/timeout/error)` 与 `detail{kind, ref}`（文件路径#行号 / share_store id / 作业 id），UI 据此着色、折叠、跳转；作业超时时把"卡在哪一步"写进结果与日志 | §23.1（merge queue overdue 记录未完成检查）、§23.2（check 模型带 Link）、§18 建议 4 |
| P1-26 | P1 | **序号与尾哈希契约化**：把 `delta.seq` + `TurnEnd.tailHash` 写进协议文档，客户端校验连续性（缺帧 → 请求按 seq 重放），`sync` 附"对应 seq" | §32 修正 1/2 |
| P1-27 | P1 | **中断幂等 helper + 清单**：`interruptOnce(key, fn)`；AGENTS.md 列出"新增中断点必做"；测试模板覆盖"同一次中断执行两次" | §32 修正 3、§27 |
| P1-28 | P1 | **spill 统一策略 + 结构化定位符**：所有有副作用/大输出的工具走同一 spill 判定；结果 metadata 带 `{spillId, lines, bytes}`；UI 提供展开与按行读取；附件 Base64 也走外置（落实已有 TODO） | §32 修正 4、§28.5、§30.4 |
| P1-29 | P1 | **设置库版本列 + 冲突可见**：`settings_db` 加 `version`，冲突返回"已被其他客户端修改"，客户端用统一重试闭包 | §22.4（`UpdateOptLock`） |
| P1-30 | P1 | **权限规则可序列化 + violation code**：规则定义带来源与创建者，判定产出 `{code, message}` 列表，消息拼装集中化 | §23.3（protection Definition/Verify） |
| P2-新1 | P2 | **命令输出解析分层**：把 `execute_command` 的"输出 → 结构化结果"抽成可测函数（对齐 `git/command` + `git/parser` 分层） | §25.1 |
| P2-新2 | P2 | **迁移框架支持兼容读旧格式**（而非一次性强转），并为"混合态"（新旧库并存）提供测试 | §25.2（`encrypt.Compat`）、§6 P0-6 |
| P2-新3 | P2 | **内容侧安全（可选）**：提交/写文件前的密钥扫描（对齐 `scan_secrets.go`），与权限中间件互补 | §25.1 |
| P2-新4 | P2 | **插件扩展协议的独立层**：把"插件事件上行/下行"等扩展消息做成独立中间层（对齐 goget 作为独立中间件），避免污染核心 wire 协议 | §25.2 |

**调整**

- **P1-6**（原"delta 序号 + sync 版本"）→ 收敛为 **P1-26**（契约化 + 客户端校验），因为服务端侧已具备。
- **P1-25**（原"中断幂等支持"）→ 收敛为 **P1-27**（helper + 清单 + 测试模板），因为逐点实现已存在。
- **P0-8**（原"大输出外置"）→ 收敛为 **P1-28**（统一策略 + 结构化定位符 + 附件外置），因为核心机制已存在；**优先级从 P0 降为 P1**（现状不阻塞使用，但影响一致性与体验）。
- **P0-11**（trace/opId）→ 保持 P0，但目标更明确：不是"从无到有"，而是**把已有标识（sessionId/agentName/tool_call_id/correlationId/delta seq）统一成一个 opId 并落到日志字段**。

### 33.3 深读后更明确的"轻重缓急"

深读把三件事的优先级**往上推**（因为源码证明它们是真实风险，而不是理论缺口）：

1. **P0-1 权限声明全覆盖**：`execute_command` 等工具未声明权限这一点，在深读中再次确认（全仓库只有 `agentxx_filesystem` 调用 `register*PathPermission`）。命令执行、桌面控制都能绕过权限闸门，属于必须修的安全缺口。
2. **P0-5 作业化 + P0-12 卡点定位**：`execute_command` 的实现依赖 `CancelRegistry` + 进程组 kill，但没有"作业"层面的超时预算与"卡在哪一步"的记录；`agent_runner` 的取消/中断处理非常精致，却**没有把"未完成任务"持久化**（进程被杀即丢）。
3. **P0-6 迁移框架**：`SessionStore` 目前靠 `ensureSchema` + `ensureViewMessageMsgIdColumn` 的"探测列是否存在"演进 schema，深读确认没有版本表；而会话库是**用户不可重建的数据**，风险等级高。

同时，深读也让两件事**降级为"打磨"**：大输出外置（P1-28）与序号契约（P1-26）——因为底层机制已经存在且可用。

---

---

# 第三部分：插件框架深入对比

> 第三部分专门看"扩展机制"这一层：harness 的"插件"是什么、agentxx 的插件框架是怎么分层实现的。它也回答一个容易被目录名误导的问题——**harness 里的 plugin 与 agentxx 里的 plugin 完全不是同一类东西**，因此可迁移项主要不是"抄机制"，而是"抄周边（元数据集中管理、schema 驱动 UI、分发方式）"与"确认 agentxx 这套 ABI 框架的成熟度"。
>
> 深读范围：harness `types/plugin.go` / `app/store/database/plugin.go` / `app/api/{controller,handler}/plugin/*` / `app/pipeline/resolver/*` / `app/pipeline/triggerer/trigger.go` / `app/pipeline/runner/runner.go` / `registry/app/factory/*`；agentxx `cxx_pluginxx`（`api/{abi,entry,tables}.h`、`host/{lifecycle,host_core,tables_impl,manifest,loader,event_bus,capability_registry,domain_hooks}.h`、`runtime/{runtime,instance_base,manager_base,op_driver,driver}.h`、`kit/{kit,guard}.h`）与 `agent/lib/{include/agentxx/plugin,src/plugins}`。

## 34. harness 的"插件"是什么：三套语义 + 一套真正的扩展机制

harness 里出现 "plugin" 的地方有**四种**，必须分开看：

| # | 名称 | 位置 | 本质 | 扩展形态 |
|---|---|---|---|---|
| A | **CI 步骤插件** | `types/plugin.go`、`app/store/database/plugin.go`、`app/pipeline/resolver/*` | 一份 **YAML 规格模板**（`spec`）+ 元数据（description/type/version/logo），供流水线 YAML 引用与 UI 表单生成；运行期由 drone runner 以**容器**执行 | 进程外：容器契约（镜像 + 输入 + 退出码） |
| B | **模板（template）** | 同 resolver、`templateStore` | 与插件同一套解析机制，作用域是"某个 space 内" | 进程外：同样是 YAML 展开 |
| C | **制品库包格式 handler** | `registry/app/factory/package.go`、`registry/app/pkg/{npm,maven,cargo,docker,...}` | 编译期注册表：每种包格式实现 `PackageHelper`，`Register/Get/IsValidPackageType` 按类型取用 | 编译期：Go 接口 + map 注册 |
| D | **真正的通用扩展机制** | `cmd/gitness/wire.go` + 各包 `WireSet` | 编译期依赖注入（第一部分 §1/§2 已详述） | 编译期：新增 Go 包 + 装配一行 |

### 34.1 A 的完整实现（harness 唯一的"插件框架"）

1. **元数据表**：`plugins` 表字段 `plugin_uid / plugin_description / plugin_type / plugin_version / plugin_logo / plugin_spec`；`types.Plugin.Matches()` 逐字段比较用于"是否要更新"。类型注释点明用途：
   > "It has an associated template stored in the spec field. The spec is used by the UI to provide a **smart visual editor** for adding plugins to YAML schema."
2. **来源与装载**（`app/pipeline/resolver/manager.go`，262 行）：`Populate(ctx)` 从配置 `CI.PluginsZipURL` 取 zip——**本地路径优先（`os.Stat` 命中即直接用），否则下载到临时文件**（`downloadZip` 用 `http.NewRequestWithContext` + `io.Copy`）；默认值是 drone 插件仓库的 `master.zip`。随后遍历包里匹配 `**/plugins/*/*.yaml` 的条目，用 drone 的 `parse.ParseBytes` 解析，**只接受 `*v1yaml.PluginStep` / `*v1yaml.PluginStage`**，其余告警跳过。
3. **清单细节**：`Identifier = config.Name`、`Type = config.Type`、`Spec = 原文`、`Description` 从 spec 取；**同目录 `logo.svg` 一并读入存库**（UI 展示）；已存在且 `Matches` 则跳过，不同则 `Update`——注释坦白版本策略的欠账：
   > "Once we start using versions, we can think of whether we want to keep different schemas for each version in the database. For now, we will simply overwrite the existing version with the new version."
4. **解析期（pipeline 编译）**：`resolver.Resolve(ctx, pluginStore, templateStore, spaceID)` 返回 `func(name, kind, typ, version)`；`kind=plugin` 时**只允许 step 级**（源码断言 `only step level plugins are currently supported`），取 spec 再 `parse.ParseString` 还原成 v1 yaml；`kind=template` 时按 space 查找模板。触发链在 `app/pipeline/triggerer/trigger.go`：`specresolver.Resolve(config, lookupFunc)` 把 pipeline YAML 里的 plugin/template 引用**整体展开替换**。
5. **执行期**（`app/pipeline/runner/runner.go`）：drone 的 docker engine，`Privileged` 白名单（`plugins/docker`、`acr`、`ecr`、`gcr`、`heroku`）、`ExtraHosts: host.docker.internal:host-gateway`（macOS 除外）、`Networks: config.CI.ContainerNetworks`、`secret.Encrypted()` 注入密钥；`poller.go` 按 `CI.ParallelWorkers` 并发拉 stage。**插件本体（镜像）不在 harness 仓库里**——harness 只负责"模板 + 调度 + 执行契约"。

**结论**：harness 的插件模型 = **配置模板 + 容器执行**。没有 ABI、没有生命周期、没有卸载、没有能力协商、没有权限声明；隔离由容器提供，参数由 YAML/secret 提供，"权限"由平台侧（容器权限 + `Privileged` 白名单）决定。

### 34.2 C 的价值：一个很干净的"编译期扩展点"范式

`registry/app/factory/package.go` 只有 67 行，但把"多实现注册表"写得很标准：

    type PackageFactory interface {
        Register(helper interfaces.PackageHelper)
        Get(packageType string) interfaces.PackageHelper
        GetAllPackageTypes() []string
        IsValidPackageType(packageType string) bool
    }
    // Register: packageType := helper.GetPackageType(); if _, ok := f.factory[packageType]; !ok { f.factory[packageType] = helper }
    // IsValidPackageType: 空串视为合法（默认类型），否则要求命中

`registry/app/pkg/` 下每种包格式一个子包（`npm` / `maven` / `cargo` / `docker` / `nuget` / `python` / `rpm` / `gopackage` / `huggingface` / `generic` / `filemanager` / `quarantine` …），各自实现同一接口——**与 agentxx 的"接口表 + 多实现"是同一种思想，只是注册发生在编译期**。

## 35. agentxx 的插件框架：四层结构

agentxx 的插件框架已拆为**独立工程 `cxx_pluginxx`（命名空间 `pluginxx`）** + 宿主侧实现（`agentxx::plugin`），共四层加一个宿主领域层：

| 层 | 位置 | 职责 | 关键文件（行数） |
|---|---|---|---|
| **① ABI 基座** | `pluginxx/api/` | 跨边界契约：导出/调用约定、定长类型、8 字节对齐、字符串视图、异步原语、核心 vtable（`alloc/free/query_interface`）、入口符号类型、内置插件描述 | `abi.h`(240) / `entry.h`(50) / `tables.h`(353) |
| **② 宿主骨架** | `pluginxx/host/` | 通用表实现（10 张）+ 生命周期状态机 + 清单解析 + 装载器 + 能力注册表 + 事件后端接口 + 领域钩子 | `host_core.h`(657) / `lifecycle.h`(1406) / `tables_impl.h`(1007) / `manifest.h`(220) / `event_bus.h`(140) / `domain_hooks.h`(80) / `capability_registry.h`(73) / `loader.h`(35) |
| **③ 运行时** | `pluginxx/runtime/` | 实例基类与宿主视图控制块、实例生命周期/租约、Operation 驱动器、协程驱动 ticket、管理器基类 | `instance_base.h`(654) / `runtime.h`(523) / `op_driver.h`(736) / `driver.h`(270) / `manager_base.h`(410) |
| **④ SDK** | `pluginxx/kit/` | 插件侧 header-only SDK：字符串/日志、`Task<T>` 锚定协程、`PollOneBridge`、`CancelRegistry`、`OpCtl`、`ArgReader`、`PluginBaseT`、导出宏 | `kit.h`(3960) / `guard.h`(127) |
| **宿主领域层** | `agent/lib/{include,src}/agentxx/plugin` | 9 张 agent 领域表 + 领域注册（工具/权限/钩子/会话/模型/提示词/资源/图/上下文）+ 管理器接缝 | `api/plugin_api.h`(455) / `api/client_plugin_api.h`(632) / `api/plugin_kit.h`(3104) / `plugin_manager.h`(668) / `src/plugins/*`（vtable 1447、大适配 756、lifecycle 485、domain_hooks 288、graph_node 242、tool_registry 81） |

**接口表清单（19 张 agent + 9 张 client）**：

- **通用 10 张**（内核实现，宿主无需重复）：`pluginxx.events`（订阅/发布）、`pluginxx.capabilities`（含 `register_capability_ex` 的 start/cancel 形态）、`pluginxx.scheduler`（`is_io_thread`/`post_to_io`/`sleep`/`offload`）、`pluginxx.coroutine_runtime`（driver ticket）、`pluginxx.plugins`（插件互查）、`pluginxx.config`、`pluginxx.json`、`pluginxx.log`、`pluginxx.tasks`（后台任务登记 + cancel）、`pluginxx.cancel`。
- **agent 领域 9 张**：`agentxx.agent.tools`（注册/注销工具 + `call_tool_async` 插件互调）、`agentxx.agent.permission`（权限声明 + `check_paths`）、`agentxx.agent.hooks`（7 个钩子点）、`agentxx.agent.session`（share_store 读/写 + `emit_message_tip`）、`agentxx.agent.context`（LLM 上下文查询）、`agentxx.agent.model`（主模型配置）、`agentxx.agent.prompt`（提示词读写）、`agentxx.agent.resources`（skill/memory/mcp 贡献 + 自有资源查询）、`agentxx.agent.graph`（节点类型注册 + 图 JSON 读改）。
- **client 领域 9 张**：`agentxx.client.{ui,events,session,wire,self,json,log,timer,keybind}`。

**冻结契约的边界划得很清楚**（`abi.h` 顶部注释）：

> "本头中的 **C 符号名 / 结构体名 / 宏名 / IID 字符串一律冻结**，改动即破坏已编译的插件二进制。领域接口表……由宿主自行定义；本头只承载与领域无关的通用部分。"

**入口符号名属于宿主命名空间**（`entry.h` 注释）：内核**不**定义入口符号名常量（agentxx 用 `agentxx_plugin_agent_*` / `agentxx_plugin_client_*`，其他宿主如 musicxx 用 `musicxx_plugin_*`），宿主覆写 `PluginHostLifecycle::entrySymbols()` 交出符号名；**未提供时装载直接失败并说明原因，"不会去猜宿主专名"**。入口集合里 `get_info` 可空（跳过元信息校验）、`create/start/stop` 必需、`destroy` 由实例类的 `pluginDestroySymbol()` 给出。

**分层的一个具体收益**（`plugin_framework.h` 注释）：宿主侧要用内核类型，若处处写 `pluginxx::` 噪声大，于是用**逐条 `using` 声明**把它们引到 `agentxx::plugin`——"类型本体只有 `pluginxx` 一份（不存在两份实现或 ODR 风险）"，且**明确不是转发头**（内核头只能经 `pluginxx/...` 包含）。

## 36. ABI 细节：为什么每一条约束都存在

| 约束 | 实现 | 为什么 |
|---|---|---|
| 8 字节对齐 | `#pragma pack(push, 8)` 包住全部跨边界结构 | 不同编译器默认对齐不同 |
| 定长类型 | 只用 `int32_t/int64_t/uint64_t`，禁止裸 `int/long/size_t` | 32/64 位与不同 ABI 的宽度差异 |
| 调用约定 | `PLUGINXX_CALL`（Windows `__stdcall`；x86 GCC `stdcall`；其余空） | 32 位 Windows 默认约定不一致 |
| 传参与返回 | 入参一律 `const Struct*`；返回值用出参 `Struct* out` + `int32_t` 状态码 | 避免依赖结构体值传递布局；状态码可判定 |
| 字符串两态 | `PluginxxStringView{data,size}`（只读借用、不要求 NUL 结尾）与 `PluginxxString{data,size}`（**宿主堆**分配，接收方 `host->free`） | 借用态零拷贝；所有权态显式，避免跨 CRT 释放 |
| 内存通道 | 核心 vtable 仅 `alloc`/`free`（`strdup` 改为头文件内联 `pluginxx_strdup`） | 跨 CRT 堆边界只有一条通道；vtable 面越小越稳 |
| 版本 | 全局 `PLUGINXX_API_VERSION`（要求插件 `>=` 宿主）+ 每表 `version`/`struct_size` | 全局版本防"核心结构新增字段"；表内独立演进避免频繁动全局版本。`plugin_interfaces.h` 注释强调协商**不替代** api_version："老宿主+新插件按新偏移读 C 结构是 UB" |
| 表内可空成员 | 函数指针为 `NULL` = 宿主未实现该子能力 | 子能力不引入新版本号 |
| C++ 便捷层分离 | `PluginStringView` / `PluginString`（RAII）等放在 `kit.h`，不进纯 C 头 | 注释给出真实原因：**按值返回含 C++ 成员函数的 struct 会触发 MSVC C4190** |

**三类异步原语**（框架最核心的设计，也是与 harness"一次 HTTP 调用"模型的根本差异）：

1. **`PluginxxOperatorNotify.done(status, payload)`**：操作完成通知，被调方**必须恰好回调一次**（`OK/CANCELLED/FAILED`）；可从任意线程调用（宿主内部投递回 IO 线程）。
2. **`PluginxxOperatorCallback`**：宿主侧完成回调，保证在宿主 IO 线程派发，payload 只在本次回调内借用。
3. **`PluginxxDriveOnceFn`（driver ticket）**：插件协程"推进一步"的许可，**一次 ticket 至多执行一次回调且永不内联**；回调中不得阻塞、不得等待事件、不得同步调宿主业务接口；异常必须自捕获（跨 ABI 的异常是 UB）。

## 37. Operation 终态协议与执行租约（宿主运行时）

`runtime/{op_driver.h,runtime.h}` 把"插件启动异步操作 → 宿主记账 → 完成/取消 → 唤醒等待者"收敛到一处，**所有异步入口共用**（工具/钩子/能力/图节点/后台任务/生命周期 start/stop）。

### 37.1 句柄与完成端点：为"插件违约"而设计

- 句柄 `PluginxxOperatorHandle` 是宿主托管的不透明指针，插件只用于取消，不得释放、不得在终结后使用；校验走进程内表：**伪造/过期指针只被安全忽略**。
- `PluginxxOperationCompletionEndpoint` **独立于 `OpCore` 保存**，理由写在注释里：
  > "端点由句柄 tombstone 保活……即使 manager/runtime 先析构，插件仍持有 `notify.host_ud` 时也不会落到已经释放的 OpCore；第一次完成会原子化地取走这份强引用。"
- 迟到 `done`：*"Operation 已回收时，迟到 done 只记录并丢弃，不访问已经失效的插件操作状态。"*

### 37.2 取消与完成的线性化协议（三条规则）

`OpCore` 的注释把并发正确性变成可检查的规则：

> 1. `completionSubmitted_` 是"done 已被接受"的唯一切换点：任意线程都只在持有 `submitMutex_` 时读取与置位，因此重复 done、done 与 cancel/reject 竞争只会有一个赢家。
> 2. 持有 `submitMutex_` 期间**绝不调用插件或调用方代码**。插件可能在自己的 cancel 实现里同步调用 `notify.done`，若锁内进入插件就会自锁；因此这里用普通 `std::mutex` 也安全（原先的 `recursive_mutex` 不再需要）。
> 3. 终态与调用方回调只在 IO 线程的 `commit`/`reject` 中生效，且只在 `completionSubmitted_` 置位之后；`cancelOnIo` 本身不产生终态，只把状态推进到 Cancelling。

配合语义：`start` 返回 `NULL + error` = **拒绝**（不得调 done，宿主不产生回调）；返回句柄 = 接受（同步/异步 done 皆保证**恰一次**回调）；终态之后 `cancel` 是空操作，**不再进入插件代码**。

### 37.3 执行租约（lease）与安全卸载

- **两侧租约**：跨插件互调同时持有 **caller 与 provider** 两侧租约——"caller 的完成回调返回前，caller 不会被卸载/destroy"。
- **卸载等待必然覆盖**：每个 Operation 与每个 driver ticket 都持有实例执行 lease，因此"等 inflight 归零"天然覆盖它们，`dlclose` 不会越过仍在执行的插件代码。
- **关闭期间仍允许驱动**（`driver.h` 注释，理由很关键）：Closing 后仍需允许 driver，因为"关闭的第一步就是取消全部 Operation，而插件的取消收束（取消回调 → 唤醒 → 下一个有限步骤）必须能继续跑完，否则关闭必然超时"；禁止**新**工作由 `acceptsOperations` 与各注册入口把关，**不由 driver 把关**。
- **完成包重放**：executor 停止时，已产生的完成包进入 runtime 待重放队列，Operation 保持未终结并继续持有 lease；实例关闭因此会等到截止时间进入 `CloseFailed`（保留 ctx/DSO，**可重试**），而不是静默泄漏；executor 重绑后完成包重放且只提交一次。
- **driver 句柄的地址注册表**：`cancel_driver` 的 ABI 形态**不含 host 参数**，宿主无法从实例反查合法句柄，因此用**进程级地址注册表**（只存 `weak_ptr`）兜底：命中才升级强引用并取消，未命中只记日志；实例侧另保留最近 **16 张**已收束请求作墓碑窗口，吸收"刚收束就取消"的迟到调用。
- **ticket 终态由 CAS 仲裁**："执行"与"取消"两方竞争同一所有权令牌，胜者收束（释放 lease），保证"取消之后不再执行回调"与"lease 恰好释放一次"。

## 38. SDK：`Task` 锚定协程、`PollOneBridge` 与取消登记

`kit/kit.h`（3960 行，header-only）是插件侧全套基础设施，深读后最有价值的三块：

### 38.1 锚定协程与两种"根"

- 插件协程**不拥有自己的事件循环**：`Task<T>` 的帧**锚定到当前 Operation**，完成时经完成协议上报宿主（`CompletionGuard`/`PromiseBase`/`FinalAwaiter`）。
- 两种根区分两类工作：**一次性任务**（快速完成，`BridgeRoot`）与**受控轮询根**（等的是插件本地 reactor 上的事件，需要宿主反复给 ticket 才能推进，`PolledRoot`）。
- `PollOneBridge` 实现 pump/wake 协议：插件本地有新工作就**自行合并重复唤醒**（同一实例同时只登记一次 ticket），再 `request_driver` 申请下一次；**"无工作时不得持续申请 ticket"**被写进契约（否则就是隐藏轮询）。

### 38.2 `polled_tool` vs `blocking_tool`

| 形态 | 执行方式 | 适用 |
|---|---|---|
| `polled_tool` | 协程跑在**插件实例本地 reactor**，宿主 IO 线程经 driver 请求 `poll_one` **有界步进**（有进展立即续、无进展退避、无未完成操作时不驱动） | 需等待 I/O 但不占线程（`execute_command` 的 bash/PowerShell 工具） |
| `blocking_tool` | 阻塞函数交给宿主工作线程池（`pluginxx.scheduler.offload`） | 无法异步化的同步实现（关闭 `AGENTXX_ENABLE_BOOST_PROCESS` 时的 popen 回退） |

这就是第一部分"命令执行不占线程"的**全部实现机制**（进程组/Job Object/取消监听细节见 §30.3）。

### 38.3 `CancelRegistry` / `OpCtl` / `ArgReader`

- `CancelRegistry`：框架级取消登记 + `ScopedRegistration` RAII；插件登记"会话键 → 取消回调"，宿主取消时**事件驱动**回调（`execute_command` 用它 kill 进程组），注释明确它"彻底代替 20ms 轮询与跨线程同步 post 到 io 线程造成的严重抖动"。
- `OpCtl`：操作控制对象（配合 `Task` 管理"是否已 done / 是否已取消"）。
- `ArgReader`：强类型参数提取器；`PluginBaseT<IfacesT>`：插件实例上下文基类（接口表查询结果 + 日志 + bridge 都在这里），领域表由宿主侧派生基类补齐（`plugin_kit.h` 是 umbrella）。

## 39. 生命周期状态机、启用/禁用事务与清单

### 39.1 状态机与四个入口的契约（`plugins.md` §15.1 + `lifecycle.h`）

    create (纯构造) → start (注册事务) → Ready
    Ready ⇄ Disabled            (用户或依赖级联)
    Ready/Disabled → Closing → Closed
    Closing → CloseFailed → Closing (可重试)

| 入口 | 契约（原文要点） |
|---|---|
| `create` | "只分配上下文、查询接口、初始化纯本地字段；**不提交**工具/hook/能力/事件/UI/prompt/graph 注册，不启动不受托管的线程" |
| `start` | "在宿主 IO 线程执行注册事务；同步 `notify.done` 返回即视为成功。失败时返回 `NULL + error`（视为拒绝，宿主回滚本次已生效的注册并撤销实例）"——回滚范围覆盖工具、hook、能力、事件订阅、资源、prompt 贡献、图类型槽位与 client UI 项/命令/订阅 |
| `stop` | "用于撤销插件自管的线程/定时器/订阅；**可重复尝试**，失败时实例保持 `Disabled`/`CloseFailed` 且保留上下文与动态库" |
| `destroy` | "只在 `stop` 完成且租约归零后调用；不得创建异步工作、不得调用宿主注册接口" |

且**start/stop 是必备入口**：缺失时宿主拒绝加载并给出明确原因（不再支持"create 期注册、无生命周期入口"的旧形态）。SDK 用 `AGENTXX_PLUGIN_AGENT_EXPORT(Ctx, Name, Ver, Desc, StartFn, StopFn)` 一次生成五个入口（带异常兜底），手写 create/destroy 的插件用 `..._LIFECYCLE_EXPORT` 只生成 start/stop trampoline。

### 39.2 启用/禁用事务（易被低估的难点）

- `disable(name)`：宿主**同步摘除**该实例的注册（工具/hook/能力/事件/资源/prompt 贡献/graph/UI），并把 `stop` 事务**投递**到所属 IO 线程；级联按**直接依赖者**递归处理。
- `enable(name)`：恢复启用状态后由插件的 `start` 事务**重新声明**注册（start 成功前不恢复宿主侧记录）；启用顺序"先依赖、后依赖者"；**用户显式禁用的插件不被级联恢复**。
- 两类禁用被区分开：`userDisabled`（用户显式）与 `blockedByDependencies`（必选依赖不可用而级联禁用）——这让"依赖恢复后要不要自动启用"有明确依据。
- **prompt 以 `(owner, key, sequence, value)` 贡献模型合成**：卸载/禁用只删除该 owner 的贡献并重新合成，不覆盖其他 owner，也不写回已卸载 owner 的旧值（`execute_command` 插件正是这样注入工具提示词的）。
- **Client 侧动作的"代次"防护**：动作按钮在渲染时记录 `plugin/generation/owner`，派发到 IO 线程复查——**同名插件重载后，旧点击一律丢弃**。

### 39.3 宿主接缝与"宁可泄漏也不 UB"

`PluginHostLifecycle<InstanceT>` 把与领域无关的生命周期逻辑收敛到一处，宿主只实现接缝：

| 接缝 | 用途 |
|---|---|
| 纯虚：`selfRef()` / `createInstance()` / `hostVtable()` | 管理器自引用（异步收尾期间保活）、实例构造、交插件的 vtable；`selfRef` 的存在有具体理由——"框架内核不能自己继承 `enable_shared_from_this`，与宿主管理器的 `enable_shared_from_this<ManagerT>` 会形成多基类歧义" |
| 可选：`entrySymbols()` / `logTag()` | 入口符号名（无默认值，缺失即失败）、日志前缀（同进程多宿主区分） |
| 可选：`detachDomainRegistrations` / `detachDomainOwnedResources` / `clearDomainRegistrations` / `releaseInstanceResources` / `onInstanceEnabledChanged` | 领域注册的摘除、实例专属资源所有权、注册记录清空、启用标记同步 |
| 可选：`applyDeclaredResources` / `onInstanceLoaded` / `onInstanceUnloaded` | 清单资源应用与装载/卸载钩子 |
| 可选：`cascadeUnloadEnabledOnly` | 级联卸载是否只处理启用中的实例 |

**析构的保守策略**（`instance_base.h` + §30.1 的实测代码）：仍有 lease 或 start 未完成时，**保留上下文与 DSO 并记日志**，等 `shutdownAsync` 重试——"析构不能绕过仍在运行的插件代码"。`warnPendingCloseOnDestroy` 在 owner 析构前自检并给出明确日志。

### 39.4 清单（`plugin.yaml`）与名称推导

`host/manifest.h` 是纯函数集合（任意线程可调用）：

- `pluginNameFromPath`：`libfoo.so → foo`、`foo.dll → foo`、`libfoo.so.1.2 → foo`、`my.plugin.so → my.plugin`；注释给出一条边界——"**仅当剥离过扩展名后才去 `lib` 前缀**：无扩展名的库/目录（如插件目录 `libanalysis`）保持原名，避免误剥"。
- `parsePluginManifest` / `parsePluginManifestFromString` / `parseBuiltinManifest`：解析 `name` / `entry` / `depends` / `optional_depends` / `resources` / `interfaces`。
- `PluginManifestResources`：`skill`(dirs) / `memory`(files) / `mcp`(`{namespace,url,timeout}`)——**键名与主配置 yaml 一致**（"降低理解成本"），相对路径解析为绝对；仅 agent 侧使用（client 宿主忽略）。
- `PluginManifestInterfaces`：`require` / `optional` 两段（见 §40）。
- `topoSortPlugins`：Kahn 拓扑排序；`resolvePluginEntryPath` 做平台扩展名修正与配置子目录回退。
- **内置插件（`builtin://<name>`）**：表由宿主经 `configure_file` 生成（`plugins/builtin_plugins.cpp.in`），符号不属于内核，因此宿主启动早期经 `setBuiltinPluginProvider` 注册；**未注册即视为"无内置插件"**（纯动态库宿主无需注册）。配置驱动装载时先查内嵌清单取依赖，未命中再探测"exe 目录 / 工作目录 / plugins/<name>/plugin.yaml"。

## 40. 接口协商与版本治理

`agentxx/plugin/plugin_interfaces.h` 把"插件需要什么 / 宿主有什么"设计成**三层协商**（注释明确指出这套机制是**不对称**的：机制通用，实际起作用的限制集中在 client 侧，因为 client 宿主形态多样而 server 只有一种）：

1. **声明层**：插件 `plugin.yaml` 的 `interfaces.require/optional` 列稳定接口名（内置命名空间 `agentxx.agent.*` / `agentxx.client.*`；第三方私有接口用 `<vendor>.*`；agent 侧可精确到表 IID 如 `agentxx.agent.tools`）。
2. **校验层**（`checkInterfacesForSide` + `requiredEntrySides`）：
    - 前缀归属由 `sideCaresAboutInterface` 决定：`agentxx.agent.` → 只 agent 关心；`agentxx.client.` → 只 client 关心；**无前缀 / `<vendor>.*` / 其他 `agentxx.*` 子命名空间 → 两侧都查**（"宿主不认识即不支持，保守安全"）；
    - `require` 未满足 → **跳过加载**（INFO + 原因记录，且强调"非错误：同一插件目录服务多种宿主是预期情况"）；声明了本侧接口却**缺对应入口符号** → 明确报错（`requiredEntrySides` 做 dlsym 意图预检）。
3. **决策层**：插件在 `entry` 内自行判空 `query_interface` 结果、或读 `EVT_READY` / `get_client_state().interfaces` 决定启用哪些功能；展示类子能力经 `agentxx.client.ui` 表访问（**表内不支持项为 NULL 函数指针**）。

**细粒度能力名**（client 侧，来自 `plugin_interfaces.h`）值得一记——它们把"能力协商"做到了子功能级：

`agentxx.client.components`（能否渲染 `agentxx.ui.item` 全量组件）、`.form`（控件交互 + `__submit`/`__cancel` 回传）、`.layout`（区域尺寸感知）、`.status_item` / `.panel` / `.toast` / `.msg_decor` / `.info_section` / `.command` / `.action` / `.overlay` / `.keybind` / `.timer`。注释给出选择建议："插件应优先按细粒度能力名精确声明自己实际使用的子能力——声明表整体会让'宿主缺某子能力'变成'整个插件不可加载/告警'"。

**一处硬上限**：`xx_check_paths` 有 `kPermissionCheckPathsMax = 16384`（`plugin_manager_vtable.cpp`），理由写在注释——"判定在宿主 io 线程执行，限制单次规模避免长时间占用"；SDK 侧对应建议"按需分批（如 512 项）"。

## 41. 安全、隔离与多实例

### 41.1 多实例三铁律（`plugins.md` §3）

同一动态库在单进程内可能被多个 agent 宿主分别创建并存实例（FFI 多句柄、`AgentHost` 子代理）：

1. **禁止可变全局/函数级 static**：所有可变状态放随实例创建的上下文堆（`*plugin_ctx`，通常继承 `kit::PluginBase`）；
2. **状态经上下文闭包恢复**：所有工具/钩子/事件回调通过 `spec.user_data` 恢复当前实例上下文；
3. **接口表缓存存入实例上下文**：查询结果存实例成员；offload 适配器为调用方内嵌存储，随实例销毁释放。

并给出**唯一例外**（写得很克制）："进程级单调计数器"（如 `agentxx_filesystem` 的临时文件名序号 `static std::atomic<uint64_t>`）是允许的——"它不承载实例语义，改成每实例计数反而会让两个实例生成同名临时文件；例外只适用于'只增不减、只用于唯一性'的原子计数器"。

### 41.2 权限声明与"失败兜底"

- 权限声明接口（`agentxx.agent.permission`）只有三个函数：`register_tool_permission` / `unregister_tool_permission` / `check_paths`；声明内容为 `{tool_name, scope(READ|WRITE), target_kind(NONE|PATH|TEXT), target_arg, category}`，`struct_size` 允许 0（"按当前布局解析"）——**向前兼容的读法**。
- `check_paths` 的三态（`DENY/ALLOW/ASK`）与工具调用判定**同一口径**，唯一区别是"应询问时返回 ASK 而不询问"；调用建议写明："DENY 丢弃；**ASK 表示'未获批准'同样不应访问**"；失败时"调用方应跳过过滤，按原行为处理"（明确的失败姿态）。
- 权限声明是**附加能力**："宿主未装配权限中间件时返回非 0，插件可忽略"——因此插件在精简宿主上仍可用（只是少了闸门）。

### 41.3 边界纪律

- **跨边界禁止异常**：`kit/guard.h` 提供 `guardCall` / `guardCallVoid` / `reportCurrentException`，把 C++ 异常转成 `error_out`；`executeCommand` 侧也强调"异常必须由插件自行捕获（跨越 C ABI 的异常是未定义行为）"。
- **符号控制 + 校验脚本**：ELF `-fvisibility=hidden` + version script 白名单（通配符兼容单端插件在 Android lld 下链接）、macOS `-exported_symbols_list`、MSVC `dllexport`；`agent/script/check_plugin_exports.sh` 用 `nm -D --defined-only` 遍历产物，**出现任何额外导出符号即失败**（用于确认第三方静态依赖与 `cxx_utilxx*` 的符号确实被隐藏）。
- **"安全失败"契约**：插件持有宿主分配的 token（如 `host->opaque`）在实例卸载后继续使用时，宿主保证**入口返回失败值、不访问已释放对象**（现场查不到实例 → 直接失败）。

## 42. 插件框架的验证体系（harness 侧无对应物）

| 验证面 | 手段 | 说明 |
|---|---|---|
| ABI 约束 | `test/plugin/negative_compile/*`（`stl_param_hook` / `wrong_capability_return` / `wrong_hook_return` / `wrong_polled_return` / `wrong_tool_return` + `positive_control`） | **故意写错的插件源码必须编译失败**，由 CMake 断言；正控证明"不是所有插件都编不过" |
| C 语言兼容 | `test_plugin_abi_c17.c`（257 行） | 用 C17 编译纯 C ABI 头，保证不以 C++ 语法污染契约 |
| 生命周期与终态 | `test_plugin_runtime.cpp`（2070 行） | 断言"拒绝时不产生回调、operations 清空、两侧 lease 归零"；"接受后恰一次回调、回调受 lease 保护且在 IO 线程"；"取消一次、cleanup 一次、终态唯一"；重复 done/迟到 done/同步 done 重入；关闭等待与 `CloseFailed` |
| start 失败回滚 | `plugin/dso_plugins/test_start_fail`、`test_client_start_fail`（真实 `.so`） | 全量注册后 `start` 失败 → **无残留** → 再次加载同名注册全部成功 |
| 多实例 | `test_plugin_multi_instance.cpp`（450 行） | 两个 AgentContext 各持独立 PluginManager 加载同一 `example_plugin`；按 sessionId 回显区分路由；卸载 A 后 B 仍可用；反复 load/unload 不泄漏（LeakSanitizer 兜底） |
| SDK | `test_plugin_sdk.cpp`（938 行） | `Task`/锚定/`polled_tool`/`CancelRegistry` 语义 |
| 桥与通用表 | `test_plugin_bridge.cpp`（1213 行） | 事件订阅/发布、能力声明与调用、任务登记与取消 |
| 端到端（双端） | `test_plugins.cpp`（3224 行）、`test_client_plugins.cpp`（3669 行） | 真实 DSO 插件在 agent/client 两侧的注册与撤销（含 UI 项/命令/订阅） |
| 平台矩阵 | `plugin_platform_support.cmake` + `plugins.md` §15.6 | Windows/Linux/macOS 已验证（ASan/LSan/定向 TSan；"插件框架 TSan 0 告警"）；Android 未验证；工作目录要求（Windows 插件目录按 `GetModuleFileNameW` 推导） |

**harness 侧的对应物**是"编译期 wire + 生成物门禁"：`make generate`（wire/mocks）、`make lint/sec`、`.testapi/*.http` 手工联调——**没有 ABI 类测试，因为不需要**（没有 ABI）。

## 43. 插件框架的双向可迁移项与结论

### 43.1 harness → agentxx（可迁移）

| # | 优先级 | 建议 | 依据 |
|---|---|---|---|
| PI-1 | P1 | **插件清单集中存储 + UI 可视化编辑**：`plugins` 表把"插件元数据（name/version/description/logo/spec）”集中管理，UI 依 spec 生成参数表单；agentxx 目前只有 `plugin.yaml` + Info 列表，缺"配置表单 / 参数说明可视化"。建议：把 `plugin.yaml` 的 `args/entry/resources/interfaces` 解析结果 + 插件导出的 `get_info()` 元信息缓存到全局库，TUI 设置页按 `configSchema` 渲染可编辑表单（可直接复用声明式组件层的 `control`/`submit`） | §34.1（plugin 表 + "smart visual editor"）、§38.3（`config_schema_json` 目前"仅供导出/文档，引擎不校验"） |
| PI-2 | P2 | **插件来源可配置（zip/URL 离线包）**：harness 的 `PluginsZipURL` 支持"本地路径优先、否则下载"，便于离线/内网分发与批量更新插件目录。agentxx 的插件来自路径 `plugins/<name>`；可加"插件包目录/压缩包"入口与版本比对（`Matches` 式的差异化更新） | §34.1 第 2/3 点 |
| PI-3 | P2 | **Privileged 式白名单**：harness 对"能在 CI 里以特权运行的插件"用显式白名单（`plugins/docker` 等）。agentxx 可对"高权限插件"引入同类声明（例如声明需要 `ProcessExec`/`DeviceCapture` 的插件需在配置里显式列出才允许加载），把 §41.2 的权限声明与"加载许可"区分开 | §34.1 第 5 点 |
| PI-4 | P2 | **容器执行后端**（远期）：harness 的 CI/Gitspace 用容器提供强隔离；agentxx 若提供"可选沙箱执行后端"，可复用 devcontainer 思路（第一部分 §11 P2 已提） | §34.1 第 5 点、§25.1 |
| PI-5 | P2 | **包格式式编译期注册表范式**（可选）：制品库 `PackageFactory` 的"Register/Get/IsValidPackageType"极简范式，可用于 agentxx 内部"多实现注册表"（如多 provider、多渲染器）——**编译期**扩展点比运行期插件便宜 | §34.2 |

### 43.2 agentxx → harness（反向参考）

如果 harness 将来想让插件从"容器契约"走向"进程内契约"（例如轻量插件、Gitspace 内的 AI 工具扩展），agentxx 这套框架里有四件事是**已经踩过坑的成熟经验**：

1. **生命周期状态机 + 回滚**（create/start/stop/destroy + Disabled/Closing/CloseFailed + start 事务回滚）——比"装载即注册"安全得多；
2. **Operation 终态协议**（恰一次 done、取消是协作式、终态唯一、late done 安全丢弃）——任何异步插件都必须有这一层；
3. **执行租约 + 安全卸载**（lease 覆盖插件代码、关闭期允许驱动、完成包重放、宁可泄漏不 dlclose）；
4. **接口协商三层 + 每表独立版本 + 表内可空成员**——在"多宿主/多客户端"场景下比一个全局版本号强得多。

### 43.3 结论

- 两套"插件"不在一个维度：harness 是**进程外容器契约 + 配置模板**，agentxx 是**进程内 C ABI + 生命周期/租约/协商**。前者换来最强隔离与生态复用（Docker 镜像即插件），后者换来最小开销、热插拔与深度集成（工具/钩子/图节点/UI/提示词全可扩展）。
- **agentxx 的插件框架成熟度明显更高**：契约冻结、入口符号治理、Operation 终态、租约与安全卸载、启用/禁用事务、级联依赖、多实例铁律、接口协商三层、负面编译 + 真实 DSO 双端 + 泄漏检测的验证体系——这些在 harness 里没有对应物（也不需要）。
- **真正可借鉴的方向是"周边"**：把插件元数据与参数 schema 集中管理并做成可编辑界面（PI-1）、插件包的分发与版本比对（PI-2）、以及"高权限插件需显式许可"的加载策略（PI-3）。这些能直接改善 agentxx 的用户体验与运维性，而不触碰已经冻结的 ABI。

---

*本文基于两个仓库的当前工作副本（harness 侧为本地 `D:\0Acoolight\Program\js\harness` 快照）通读源码后撰写；所有结论均以源码为准，若与文档/注释不一致，以"源码事实"节为准。第二部分（§22~§33）为逐文件深读补充，凡与第一部分不一致处以 [§32](#32-对第一部分的修正与补强) 的修正为准；第三部分（§34~§43）为插件框架专题，harness 侧结论见 [§34](#34-harness-的插件是什么三套语义--一套真正的扩展机制)，agentxx 侧见 [§35](#35-agentxx-的插件框架四层结构)~[§42](#42-插件框架的验证体系harness-侧无对应物)。*

