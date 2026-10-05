# agentxx 重构融合设计计划（六篇架构对比最终整理版）

> 本文根据以下六篇比较文档及其引用的源码、测试、设计文档整理：
> `compare-codex.md`、`compare-dsh.md`、`compare-harness.md`、`compare-openclaw.md`、`compare-opencode.md`、`compare-pi.md`。
>
> 整理原则：六篇文档中“已实现、部分实现、建议迁移、反向结论”的编号不直接沿用；重复建议合并为一个设计对象，冲突建议按 agentxx 的单进程本地优先、可嵌入、跨平台和 C ABI 约束裁决。源码与文档不一致时以源码为准。

## 0. 总结决定

### 0.1 总体结论

六个项目反复出现的共同问题不是“缺少某个大功能”，而是运行时已经存在的事实没有被建模成可检查、可持久化、可恢复的对象。agentxx 的主要重构方向应是：

1. **把隐含语义变成显式状态**：轮次、输入投递结果、工具结算、权限决定、压缩过程、作业状态、上下文来源、失败尝试都要有名称和结构。
2. **把重要事实写入会话存储，但不把整个系统改成事件溯源**：读模型仍由 `Session` 和现有 SQLite 表负责；只为恢复、重连、审计和诊断增加关键事实事件。
3. **先稳定成本边界，再扩大并发**：稳定提示词前缀、动态内容增量注入、统一 token 预算、工具结果总量限制、溢出恢复是一个连续方案，不能只做其中一项。
4. **安全先补覆盖，再谈强化**：agentxx 的路径规则、worktree 隔离和三态批量检查已经很好；真正的缺口是危险工具未全部声明权限、没有统一出网策略、决定没有结构化理由、没有持久化审批记录。
5. **保留 agentxx 的形状**：单 `io_context` 协程、多会话无锁交错、会话是 LLM 上下文唯一权威、NeoGraph 图驱动 ReAct、C ABI 可卸载插件、声明式 UI 描述层和 SQLite 会话库不改成另一套运行时。

### 0.2 六篇文档各自最值得吸收的内容

| 来源 | 最值得吸收的设计 | 不应整体照搬的部分 |
|---|---|---|
| codex | `next-step`/`next-turn` 投递、采样抢占、写者锁、工具编排、步骤快照、命令环境加固、UI 快照 | Rust crate 数量、完整系统沙箱、rollout + SQLite 双写 |
| dsh | 输入 inbox、来源化上下文、waterfall/单调 guard、结构化工具结果、压缩锁、可继续子代理、生成式目录 | Cordis 运行时、依赖变化自动重启、整库事件投影 |
| harness | 作业状态机、迁移链、结构化错误和规则理由、netpolicy、审计、启动校验、超期回收 | 多租户/RBAC/Redis/REST/容器化默认执行世界 |
| openclaw | 提示词缓存边界、投递四态、写者租约四步、结果聚合预算、缓存过期剪枝、记忆分层、协议幂等键、scoped 规则 | 多通道网关、设备节点、原生壳、超大测试矩阵 |
| opencode | durable inbox、Context Epoch、工具公告身份与结算、一次溢出压缩重试、saved approvals、声明式 transform 重算 | Effect 服务容器、V1/V2 双轨运行时、完整投影读模型 |
| pi | 假 provider、请求体断言、持久化一致性测试、操作状态和 drive 语义、用量账本、终端快照 | 三代运行时、完整操作状态机、两套远程协议、无内置权限 |

### 0.3 当前 agentxx 必须保护的优势

- **会话是上下文唯一权威**：`Session` 保存 typed LLM 消息，图状态只保存 `xx_messagesMeta` 等轻量影子信息，不能把完整上下文重新塞回图 checkpoint。
- **中断可恢复且不重复副作用**：`AgentRunner` 统一处理 HIL 和子代理中断，`xx_savedGraphData`、`interruptToolcallCache` 和 `tool_call_id` 复用规则继续保留。
- **取消是控制流，不是普通错误**：继续使用 `catchErrorAsync` 和异常分类，不能用 `catch (...)` 把取消吞成普通工具结果。
- **插件 ABI 已经成熟**：API 版本、表版本和 `struct_size`、唯一 `alloc/free`、入口白名单、多实例三铁律、start 事务、lease、tombstone、inflight 和可重试卸载不重做。
- **权限的工作区边界已经正确**：worktree 的允许子树必须优先于主检出写拒绝；配置显式拒绝必须优先于完全授权；`check_paths` 逐路径三态过滤要保留。
- **大输出已经有保真基础**：工具超限结果已经写入 `share_store`，返回定位 id 和按行取回提示；后续只补聚合预算、结构化定位符和投影规则，不新建第二套 blob 系统。
- **压缩已有可靠兜底**：确定性清理 → 同上下文子代理摘要 → `hardTruncate`，且有冷却和 API usage 优先；后续在此基础上增加预算、记录和溢出重试。
- **UI 描述层和唯一渲染实现**：插件只提供 JSON 组件描述，客户端统一适配和渲染；不改成插件各自写 FTXUI，也不让渲染层决定能力降级。
- **资源基准真实有效**：RSS/PSS/smaps、独立子进程、真实两进程、PTY TUI 和插件边际内存继续作为性能证据。

### 0.4 优先级定义

- **P0**：已有明确错误、数据丢失、安全绕过、长会话成本浪费或严重等待；可在现有架构内落地，并且必须配失败测试。
- **P1**：收益明确，需要新增局部对象、表、wire 字段或测试基础设施。
- **P2**：依赖前置工作、需要较大产品设计，或只在特定部署形态有价值。
- **现有**：源码已经提供主要能力，只需要文档、诊断或小范围补强。
- **不做**：与本项目定位冲突，或引入的复杂度大于收益。

当前计划共有 182 个设计条目：P0 45 个、P1 93 个、P2 44 个。这个数量是去重后的设计条目数，不是承诺全部实施的任务数；P2 主要作为有条件的候选池。

### 0.5 统一验收原则

任何迁移项都必须至少有一条会失败的测试。只写代码、日志或设计文档，不算完成。

| 类型 | 最低验收要求 |
|---|---|
| 持久化 | 正常写入、重复写入、迁移、写失败、进程重启和旧版本数据读取 |
| 并发 | 两个顺序都测试；验证结果顺序、取消、上限和资源释放 |
| 安全 | 正常允许、明确拒绝、未知目标、软链接逃逸、未声明权限和越权复验 |
| 上下文/成本 | 捕获真实 provider 请求，验证消息顺序、稳定前缀、工具定义、压缩前后 token |
| 协议 | 每个消息 `toJson/fromJson` 往返；版本不匹配、重复幂等键、错误码分支 |
| UI | 固定尺寸纯文本快照、命中区快照、窄终端、CJK、未来未知组件降级 |
| 插件 | 加载、注册、禁用、卸载、重载、缺接口、多个实例、迟到调用和导出符号 |
| 性能 | 记录基准前后 ΔRSS/ΔPSS/堆/CPU；工具并发应接近最长任务而不是总和 |

---

## 1. 总体架构、目录边界和装配

### 1.1 当前情况与判断

agentxx 已有 `agent/lib`、`agent/client`、`agent/plugins` 三层，插件与客户端通过 `AgentIOBase` 和 transport 解耦，C ABI 是真正的二进制边界。但 `BaseAgent::init()` 仍按 `initModelRegistry`、`initEventBus`、`initMiddleware`、`initTools`、图装配和插件加载的调用顺序组织，依赖方向主要依靠文档；客户端、插件和 lib 私有头的边界缺少自动检查。

### 1.2 适合融合的设计

| 编号 | 设计 | 来源 | 优先级 | 融合方式与落点 | 人工核定 |
|---|---|---|---|---|---|
| ARC-1 | 核心边界可执行检查 | opencode、openclaw、dsh | P0 | 新增 `agent/script/check_boundaries.py` 或测试模块，检查 client 不含 `agent/lib/src` 私有头、插件只含 SDK umbrella 和工具库公开头、DSO 导出仍是入口白名单。加入 CI。 | 已实施（`agent/test/core/test_boundaries.cpp`，测试模块 `boundaries`；二进制导出白名单由 `agent/script/check_plugin_exports.sh` 继续负责） |
| ARC-2 | 目录级规则文件 | openclaw、pi | P1 | 新增 `agent/lib/AGENTS.md`、`agent/client/AGENTS.md`、`agent/plugins/AGENTS.md`、`agent/test/AGENTS.md`；只写局部硬约束，根 `AGENTS.md` 只保留跨目录规则。 | 已实施（`agent/{lib,client,plugins,test}/AGENTS.md` 四份均已存在） |
| ARC-3 | 装配清单和启动断言 | harness、codex | P0 | 把启动阶段整理为带名字、依赖、回滚函数的 `InitStep` 清单；末尾统一检查模型、必要节点、工具 schema、插件目录和 SQLite。只有在配置要求持久化时才要求 dataDir/session root 可写；空 dataDir 的内存模式继续保留，但必须明确记录为非持久化，不能静默产生部分持久化。 |已实施（`InitStep{name, after, run, rollback}` 清单 + 逆序回滚 + `verifyStartupAssembly()`） |
| ARC-4 | 子系统配置视图 | harness | P1 | 从 `AgentConfig` 生成 `ModelRegistryConfig`、`SessionStoreConfig`、`PluginManagerConfig`、`SummarizationConfig` 等只读配置对象，避免子系统各自读取总配置。 | 没必要，准寻高内聚，有些功能杂糅需要读完整配置更方便 |
| ARC-5 | 分阶段关闭和后台任务收敛 | harness、codex、dsh | P1 | `shutdownAsync` 依次停止输入、停定时器、停插件、刷盘；新增轻量 `TaskScope`，统一记录后台任务并在关闭时取消/等待。**不等待当前轮次结束**（等待会拖长退出时间、影响使用感受）。 | 可行（2026-10-04 核定：不等待轮次结束） |
| ARC-6 | 生效装配快照 | harness、openclaw、dsh | P1 | 增加 `get_diagnostics` 或扩展 `WireGetContext`，输出模型来源、中间件顺序、插件状态、工具清单、图定义、权限模式、作业和队列状态。CLI 增加 `--dump-config`。 | 可行（2026-10-04 核定，限定范围）：只做启动一次性装配快照写日志 + CLI `--dump-config`；不做 TUI 诊断页与 wire 侧 `get_diagnostics` |
| ARC-7 | 新能力不进入核心骨架 | codex、pi、openclaw | P0 | 文档明确：`lib/src/agent` 只负责会话生命周期、上下文、持久化骨架；新增能力优先放 `nodes/`、`middlewares/`、`tools/`、`plugins/` 或独立工具库。 | 现有（2026-10-04 核定：纪律已写入 `agent/lib/AGENTS.md`「新增能力的位置」节；已有千行文件的拆分另立 ARC-7b） |
| ARC-7b | 核心千行文件拆分（由 ARC-7 拆出） | opencode、本次核对 | P2 | 按 2026-10-04 复核的行数拆分大文件：`plugins/client_plugin_manager.cpp`（4278 行）、`protocol/mcp_client.cpp`（2381）、`protocol/openai_provider.cpp`（2226）、`agent/base_agent.cpp`（1718）、`protocol/mcp_server.cpp`（1707）、`agent/io/session_server_agent_io.cpp`（1688）。与纪律条目分开提交，不与功能改动混做。 | 不考虑（2026-10-04 核定）：纯重构、无功能收益，现有规模仍在可维护范围 |
| ARC-8 | 消费者使用窄接口 | harness、opencode | P2 | 为中间件和宿主适配 `SessionReader`、`PermissionCheck`、`ToolRegistrar` 等窄视图；不改变 C ABI 总体形状，先用于测试替身。 | 可行（2026-10-04 核定，限定范围）：先试点 2~3 个消费者（summarization/permission 中间件、subagent 工具），确认测试替身收益后再推广；插件 ABI 不变 |
| ARC-9 | 领域与策略分开 | opencode | P2 | 配置决策集中在装配层，领域代码只消费已解析策略；不新增 Effect 容器。 | 不考虑（2026-10-04 核定：概念过多、收益不足）；只保留评审纪律「新增配置项经策略对象注入」 |

### 1.3 不采用

- 不把 lib 拆成几十个动态库或 Rust crate。
- 不引入编译期 DI 取代运行时插件。
- 不同时维护第二套 agent runtime。
- 不让客户端直接调用 lib 私有实现来省一层接口。

---

## 2. Agent 循环、轮次和输入投递

### 2.1 当前情况与判断

`BaseAgent::runTurnAsync` 运行一次图和中断恢复循环；`SessionServerAgentIO` 用内存 `deque` 保存忙碌期间的输入，`queuePaused_` 和 `pendingInsert_` 两个状态控制取消后暂停和 `InterruptAndRunNext`。当前缺少：持久化收件箱、`next-step` steering、输入受理结果、幂等键、轮次 id/终态和显式队列状态。

### 2.2 适合融合的设计

| 编号 | 设计 | 来源 | 优先级 | 融合方式与落点 | 人工核定 |
|---|---|---|---|---|---|
| LOOP-1 | 持久化收件箱两段状态 | opencode、openclaw、pi、dsh | P0 | 会话库新增 `session_input`：`id/payload/delivery/admitted_seq/promoted_seq/status`。收到输入先落库，真正进入上下文时标记 promoted；重启后只恢复为“待确认”，不自动重放副作用。依赖 STO-1/STO-2。 | 可行 |
| LOOP-2 | `next-step`、`next-turn`、`inject` | codex、dsh、openclaw、pi | P0 | `WireUserInput` 增加 `delivery`；`next-turn` 保持队列，`next-step` 在下一个安全 modelcall 边界注入，`inject` 只进入下一次请求的动态注入队列并记录来源，不直接改写权威 transcript，也不唤醒会话。持久化关闭时保持相同语义，但只保存在内存。 | 可行 |
| LOOP-3 | 投递结果显式化 | codex、pi、opencode | P1 | 返回 `started/queued/steered/rejected`，拒绝原因结构化（会话不存在、等待中断、压缩中、队列暂停、内容为空）。客户端不再从 delta 猜测。 | 可行 |
| LOOP-4 | QueueState 状态机 | openclaw、dsh | P1 | 用 `idle/running/paused/draining` 替代两个 bool；定义每个 wire 操作的状态转移并做穷举测试。 | 可行 |
| LOOP-5 | 输入和副作用幂等键 | opencode、openclaw、harness、pi | P0 | `user_input`、`interrupt_and_run_next`、`compact_context` 等增加 `idempotencyKey`；同 key 同内容返回原结果，同 key 不同内容返回冲突。小型 TTL 表即可。 | 不考虑（2026-10-04 核定：客户端不引入发送确认/重试，重复执行风险不成立） |
| LOOP-6 | 轮次记录和结束原因 | pi、opencode、harness、dsh | P1 | 新增 `turn` 表，记录 `turnId/source/start/end/status/reason/model/usage`；`TurnResult` 增加 `completed/failed/cancelled/interrupted/max_steps/skipped`。 | 意义不大，view 消息列表中有记录错误和停止原因、时间点、耗时 |
| LOOP-7 | 请求抢占 | codex、dsh | P1 | 在 `next-step` 输入到达时取消当前 provider 流，保留已收到部分，下一次请求继续；先完成 LOOP-2 的安全边界和重试一致性，再实现。 | 不考虑（2026-10-04 核定：现有「立即中断 + 插入消息 + 重跑」已足够，响应快） |
| LOOP-8 | 步骤快照 | codex | P2 | 每次 modelcall 捕获模型、工具定义指纹、权限快照；结算只使用该快照，防止插件热切换后模型看到的工具和实际执行实现不一致。 | 不考虑，复杂度增加过多，想严格保证准确得保存大量额外字段和消息，后续增删除功能还得同步改 |
| LOOP-9 | 优雅取消和无进展断言 | codex、pi、harness | P1 | 取消后给工具一个可配置收敛时间，超时记录并强制收尾；`resume_async` 返回后若没有新中断、resume 值或终态，记录明确错误。 | 暂时不考虑，此举很影响用户感受，且超时时间难定，工具被强制收尾后续的修复也是大麻烦，跟直接立即中断没有解决根本问题 |
| LOOP-10 | 中断和失败结构化留痕 | codex、dsh、opencode | P1 | 保留现有 `[User canceled]` 占位，但同时写 `attempt`/`turn` 元数据，记录阶段和原因，不把诊断记录伪装成模型消息。 | 不需要，有必要的状态变更已通知模型，详细记录和告诉模型在大部分agent和网络错误上模型并没什么办法，反而污染上下文 |
| LOOP-11 | `collect` 合并投递 | openclaw、dsh、pi | P1 | 在可配置的短暂静默窗口内合并同一客户端的连续输入；保留每条输入的来源和幂等键，合并失败时逐条返回拒绝原因。 | 可行 |

### 2.3 现有行为必须保留

- 轮次异常或取消后，队列默认暂停，防止错误状态继续灌入；新用户输入可以明确解除暂停。
- `AgentRunner` 的主代理/子代理统一恢复循环。
- 未执行的 tool call 必须补合法占位，已完成的 tool call 必须按 id 复用。

---

## 3. 会话上下文、提示词、技能和记忆

### 3.1 当前情况与判断

会话消息写入口已经收敛，图状态不保存完整 messages，这是正确方向。但 `ModelCallWrapNode` 每次都重建 system prompt 并替换首条 system 消息；动态时间、记忆、技能、工具状态和权限状态会一起改变前缀。消息缺少统一来源标记，压缩和 UI 只能依赖角色或文本前缀。

### 3.2 适合融合的设计

| 编号 | 设计 | 来源 | 优先级 | 融合方式与落点 | 人工核定 |
|---|---|---|---|---|---|
| CTX-1 | 来源化上下文和 Context Epoch | opencode、codex、dsh、openclaw | P0 | 新增 `ContextSource`：`key/load/baseline/update/removed`；保存来源快照和基线文本。首轮或压缩后生成基线，变化进入请求装配的 dynamic suffix；只有 provider 明确支持该位置时才投影为追加的 system/developer 内容，不直接把任意 system 消息插入权威 transcript。 | 不考虑 |
| CTX-2 | 三态来源 | opencode | P0 | `unavailable` 表示暂时取不到，保留上次生效值；空值表示明确移除；有值表示更新。来源快照与请求动态段同步更新，避免技能/环境读取失败把旧事实静默清空。 | 不考虑 |
| CTX-3 | 消息来源、原因和可信状态 | codex、dsh、openclaw | P0 | 内部消息 metadata 或消息旁路记录 `source/category/reason/trust`；至少支持 `user/tool/plugin/summary/inject`。宿主统一打标，模型不能自行声明可信。来源字段用于压缩、审计和 UI，不得让不可信文本取得更高执行权限。 | 不考虑（2026-10-04 核定） |
| CTX-4 | 只读快照和一致读切面 | codex、dsh、pi | P1 | `SessionSnapshot{messagesVersion, messages, stats, configGeneration}`；wire 响应带同一版本，UI 不拼接来自不同时间点的数据。后续再做共享底层，先保证版本一致。 | 不考虑（2026-10-04 核定）：影响很小（LLM 上下文查看视图为人工排查用） |
| CTX-5 | 历史替换记录 | dsh、opencode、openclaw | P1 | 新增 `replacement` 记录：范围、操作、原因、摘要 id、前后 token；不改变当前 Session 读模型。 | 不考虑 |
| CTX-6 | 自定义条目与投影 | pi、opencode | P2 | 为插件私有状态、工具附加上下文、摘要信息提供不默认进模型的 custom 条目；投影器明确决定是否进入请求。 | 不考虑（2026-10-04 核定）：机制已存在（`ChatMessage.flags` / `extra` 不进 LLM 请求体；`history_contents` 保留旧版本；大内容走 share_store 定位符） |
| CTX-7 | 附件引用而不是反复内联 Base64 | codex、dsh、harness、openclaw | P1 | 展示历史已有剥离 dataUrl；上下文进一步使用引用 id + 校验元数据，provider 层按需读取。保留当前服务端路径读取和线程池卸载。 | 可行，但需进一步理解具体实施内容 |
| CTX-8 | 会话工作上下文和派生态失效 | opencode、harness | P2 | 收敛工作目录、worktree、权限基准和附件根为 `SessionWorkContext`；切换或解绑 worktree 时清空 prompt 基线、权限缓存和执行环境派生数据，避免旧目录状态继续生效。 | 不考虑（2026-10-04 核定）：现状已满足——worktree 绑定在 `Session`、工作目录统一经 `AgentContext::getSessionWorkDir/getSessionBaseWorkDir` 取值、权限隔离边界是权限中间件的按会话 map、系统提示词刻意取不含 worktree 的基准目录；没有需要失效的派生态缓存 |
| CTX-9 | 会话重建统一入口 | codex、harness、opencode | P2 | 把从 SQLite 恢复 typed messages、展示历史、事件游标和未闭合操作的步骤收进一个可测试入口；附件不重新上传，恢复只做本地校验。 | 不考虑（2026-10-04 核定）：恢复链已存在（`getOrCreateAsync` → `SessionStore::loadSession` → `Session::restore`）；"未闭合操作"随 durable inbox（不做）与作业（JOB 不做）已无对象 |
| PRM-1 | stablePrefix/dynamicSuffix | openclaw、opencode、dsh、pi | P0 | `AgentPrompt` 明确静态稳定段和动态段；动态段不得插入稳定段内部。动态内容优先在请求装配层作为稳定位置的 suffix 发送，不为缓存优化随意改写 Session transcript；provider 不支持该形态时才整体重建并记录缓存失效。 | 可行（2026-10-04 核定）：稳定段（`systemPrompt` + 静态附加段 + 工具 schema）+ 末尾追加的带来源动态消息 + 每轮稳定段哈希（哈希变化即标记缓存失效）；不做 provider 能力探测（LLM-1 已否）；LLM-8 缓存断点依赖本项 |
| PRM-2 | 段落排序号和固定槽位 | dsh、pi、codex | P1 | 把 `map<key,text>` 改为带 `order` 的条目；同层重复 key 失败。固定槽位建议 `persona/planning/capabilities/policy/skills/memory/dynamic`。 | 可行（2026-10-04 核定，限定范围）：只加 `order` 固定排序字段（同层重复 key 仍失败），**不引入固定槽位枚举**；槽位名作为推荐值写进文档 |
| PRM-3 | 指令变化通知 | opencode、dsh、openclaw | P1 | 技能集合、记忆文件或插件资源变化时更新来源快照，并在请求动态段中追加“替换此前集合”的内容；集合清空时追加撤销内容。不得为了通知而改写稳定 transcript。 | 暂不考虑，有必要的变动才通知 |
| PRM-4 | 记忆分层和按需检索 | openclaw | P0 | 常驻记忆有字符预算，检索记忆只提供目录和读取方法；超限警告，不把整个日记文件每轮注入。 | 可行（2026-10-04 核定，限定范围）：只加"记忆文件过大警告"（不做分层、不做按需检索、不做截断）；默认用户添加的记忆文件应当像 skill 一样简略/索引式，超限时提示用户精简 |
| PRM-5 | 技能优先级和同名裁决 | openclaw、opencode | P1 | 会话/项目 > 用户 > 插件/内置；同名取最高优先级，并把来源显示给模型和 UI。 | 可行 |
| PRM-6 | 提示词整体覆盖语义 | dsh | P2 | headless/嵌入场景可声明 `complete`；多个完整提示词同时生效时失败，不静默选一个。 | 不考虑，目前支持修改 systemPrompt，应当扩展支持修改 context |
| PRM-7 | 提示词和请求体快照 | codex、openclaw、pi | P1 | 默认提示词、来源列表、工具定义和最终请求 JSON 做快照/哈希测试。 | 可行（2026-10-04 核定，限定范围）：做"请求体结构断言 + 稳定段哈希断言"（与 PRM-1 配合），**不做整段提示词快照**（提示词改动频繁，快照维护成本高于收益） |

### 3.3 不能改变的边界

- 不把完整 messages 写入 NeoGraph state。
- 不让插件直接修改 `Session::messages`，只能通过受控入口。
- 动态上下文只在请求安全边界更新，不在流式请求中途改变同一个请求。

---

## 4. 持久化、事务和崩溃恢复

### 4.1 当前情况与判断

`SessionStore` 是每会话 SQLite 四表结构，带连接 LRU、预编译语句、链式哈希和 `msg_id` 迁移；但 `ensureSchema` 没有通用版本链，只有 `msg_id` 的一次性检查；没有跨进程写者锁；delta 重放主要依靠进程内环形缓冲；上下文是单行整体替换。

补充说明: 消息上下文记录分两部分，ViewMessage 只追加或修改单消息，一般不允许移除和覆盖大量消息，负责记录用户看到的消息列表；LLMContext 则记录要发送给 LLM 的上下文；分离设计可以让 上下文压缩、消息提示、自定义渲染 等互相隔离，不会影响显示和污染LLM上下文。且 LLMContext 由于模型上下文有限制不会很大，viewMessages 能一直积累，则可以通过分页、转移到硬盘存储

### 4.2 适合融合的设计

| 编号 | 设计 | 来源 | 优先级 | 融合方式与落点 | 人工核定 |
|---|---|---|---|---|---|
| STO-1 | 会话目录内核级写租约 | codex、dsh、harness、openclaw、opencode | P0 | 写句柄打开会话目录时获取稳定 `.writer.lock`；POSIX `flock`，Windows 命名内核对象；持有期等于写句柄生命周期，不设超时，崩溃由系统释放。读操作不需要写锁。 | 可行（已实施）：`agent/lib/{include/agentxx/agent/writer_lease.h,src/agent/writer_lease.cpp}`——POSIX `flock(LOCK_EX\|LOCK_NB)` / Windows 不共享方式 `CreateFileW`，进程内引用计数可重入、租约随写连接释放，读路径不取锁，失败原因经 `SessionStore::lastWriteError()` 暴露 |
| STO-2 | schema 版本和相邻迁移链 | dsh、harness、opencode、pi、openclaw | P0 | `meta.schema_version`；每次迁移独立事务、幂等、有迁移前备份；高版本数据库拒绝打开。现有 `msg_id` 补列成为一个历史迁移步骤。 | 可行（已实施）：`SessionStore::kSchemaVersion` + `ensureSchema` 相邻迁移（每步独立事务、幂等、迁移前备份 `session.db.bak.v{n}`、失败不推进版本）+ 高版本库拒绝读写；迁移只在写路径首次打开时发生（读操作不改磁盘结构） |
| STO-3 | 关键事实事件序列 | opencode、dsh、pi、openclaw、harness | P1 | 新增 `event(seq,type,version,payload,time)`，只记录消息追加、工具结算、轮次、压缩、权限决定、基线重建等事实；现有 `view_message` 和 `llm_context` 继续作为读模型。 | 不考虑，如有需求可直接写入到 view_messages 中，渲染决定隐藏，避免了多份数据且保证时间顺序 |
| STO-4 | durable 事件流与实时 delta 分开 | opencode、openclaw、dsh | P1 | `after/limit` 历史事件补拉和实时 token/思考片段分成两类。服务重启后用 durable 事件补齐关键状态，实时片段不承诺重放。 | 可行（2026-10-04 核定，限定范围）：只做 `view_message` 加 `seq` 列 + `hello.afterSeq` 增量补拉；不新增 `event` 表、不做类型化事件、不建投影器 |
| STO-5 | 持久化语义分级与 flush | codex、dsh | P1 | `persistNow(reason)` 用于用户输入、工具结算、压缩完成、轮次终态；`persistThrottled(reason)` 用于展示历史和统计。日志写出原因。 | 可行（2026-10-04 核定）：用户输入、工具结算、压缩完成、轮次终态立即落盘；展示历史与统计保持节流；两者都写原因日志 |
| STO-6 | turn/attempt/compaction 记录 | dsh、pi、codex、opencode | P1 | 不把失败尝试塞进模型上下文；在独立表记录 provider 失败、重试、取消、压缩 started/ended。 | 不考虑，不符合 agentxx 现有设计分析，目前为分离消息记录，失败重试不会记录在模型上下文 |
| STO-7 | 启动清账和崩溃配平 | opencode、dsh、codex | P1 | 恢复时发现未闭合轮次、running 工具或未完成压缩，补“被中断”终态；未执行 tool call 生成合法占位。 | 不考虑，不符合 agentxx 的现有设计分析，目前已经有自动修正上下文等兜底 |
| STO-8 | 用量账本 | pi、opencode、harness、openclaw | P1 | 每次模型结算记录 input/output/cache read/cache write/cost/provider/model/turnId；失败和重试也记 attempt，UI 从账本聚合。 | 可行（已实施）：`usage` 表已落地（time_ms/model/prompt/completion/total/cached/reasoning/ok/error_kind）；2026-10-04 核定**不记录 cost**（表内无该列，无需改代码），也不做 attempt 分类 |
| STO-9 | 持久化降级可见 | opencode、pi、harness | P1 | 展示历史写失败可继续运行但标记降级；上下文、轮次和工具结果写失败推送明确警告。 | 可行（2026-10-04 核定，限定范围）：只做"首次写失败推一条 `MessageTip` 警告 + 恢复后不再重复提示"；不做 Info 侧边栏与状态栏的降级标记 |
| STO-10 | 大写入不阻塞 io 线程 | openclaw、harness | P2 | 先保持短事务在 io 线程；超过阈值的整段上下文或 spill 写入线程池，完成后回 io 线程更新状态。不要照搬所有 DB worker。 | 不考虑（2026-10-04 核定）：短事务在 io 线程 + 轮末权威写是刻意设计，没有阻塞明显的证据；若将来实测出现卡顿再评估 |
| STO-11 | settings_db 乐观版本 | harness | P1 | `settings_db` 增加 version，冲突返回可识别错误，避免多个客户端静默覆盖。 | 可行（已实施）：`setting.version` 列（老库自动补列）+ `getVersioned` / `setVersioned`（期望版本不匹配返回 `Conflict` 且不改内容）；`set()` 保持无条件写入并递增版本 |
| STO-12 | 会话检索和标题 | codex、dsh、pi、harness | P1 | `meta.title/titleSource` 独立可改；视需要给 `view_message` 加 FTS5；先做当前会话/标题搜索，不先建跨库复杂索引。 | 可行（存储层已实施）：`meta.title` / `titleSource`（auto/user）+ `setSessionTitle` + `searchSessions`（标题或正文子串、通配符转义、最近活动排序、命中片段）；协议与 TUI 入口待接（与 RET-1a 同批） |
| STO-13 | 会话导出和取证包 | pi、harness | P2 | 导出展示历史、工具定位符、轮次和诊断；配置脱敏、API key 不进入报告。 | 可行 |

### 4.3 明确不采用

- 不把 SQLite 读模型改成 JSONL 权威日志 + 另一套投影器。
- 不对每个 token 持久化，避免写放大。
- 不以 PID/时间戳猜测“陈旧锁”并抢占写者。

---

## 5. 工具系统和工具结果

### 5.1 当前情况与判断

`XXToolBase` 已有自动摘要、延迟加载、重试、重复调用检查；`ToolRegistry` 已拒绝重名和内置工具冲突；`ToolcallWrapNode` 已处理权限、参数修正、取消占位、share store 和插件卸载保活，但同一 assistant 消息的工具调用仍逐个 `co_await`。

### 5.2 适合融合的设计

| 编号 | 设计 | 来源 | 优先级 | 融合方式与落点 | 人工核定 |
|---|---|---|---|---|---|
| TOOL-1 | 分阶段并行：prepare/dispatch/finalize | codex、dsh、openclaw、opencode、pi、harness | P0 | 参数解析、权限、询问按源顺序串行；只读且显式 `supportsParallel` 的执行体并发；写/交互工具形成屏障；结果按原始 `tool_call_id` 顺序写回。 | 可行 |
| TOOL-2 | 并发分类和上限 | harness、dsh、openclaw | P0 | `ConcurrencyClass{ReadOnly,Exclusive,Interactive}`，默认独占；配置全局和分类上限；使用 RAII 释放配额。 | 可行（2026-10-04 核定）：与 TOOL-1/TOOL-3 合并为"工具并行化"一项，分三次提交（拆 `execTool` 为 prepare/run/finalize → 分类并发与上限 → 取消收尾与按 id 顺序提交） |
| TOOL-3 | 并行取消和收尾 | codex、dsh、opencode | P0 | 已完成结果保留，未启动补取消结果，执行中请求取消；任何取消都保证每个 tool call 有对应结果，测试完成顺序和提交顺序不同的情况。 | 可行 |
| TOOL-4 | 分发层硬超时 | harness、codex、dsh | P0 | 工具只声明 `timeoutMs`，分发层竞速取消并返回结构化 timeout 状态；复用现有进程组/Job Object/CancelRegistry。 | 不考虑（2026-10-04 核定：各工具自带超时已够用，不做分发层统一超时） |
| TOOL-5 | 工具结果守卫 | openclaw、opencode、dsh、harness | P1 | 写入会话前检查 UTF-8、合法消息、tool_call 配对和硬上限。 | 不考虑，这个功能已经实现 |
| TOOL-6 | 聚合结果预算 | openclaw、opencode、harness | P1 | 保留现有单调用 offload；增加一次请求所有工具结果的总预算。超限时最旧结果优先换为 share_store 预览。 | 不考虑 |
| TOOL-7 | 结构化定位符和首尾预览 | openclaw、opencode、codex | P1 | 现有 share_store 文本增加 `spillId/bytes/lines/preview` 元数据；预览保留首尾，中间省略；不另建存储系统。 | 不考虑，已有行提示 |
| TOOL-8 | 结构化结果与模型投影分离 | dsh、opencode、pi | P1 | `ToolResult{value, content, meta, status}` 作为内部可选返回类型；先兼容现有字符串工具，插件 ABI 通过 JSON 扩展。 | 不考虑，旧版本实现过，徒增结构化和转义，若 utf8 处理不充足还会导致解析错误 |
| TOOL-9 | 单调安全 guard | dsh、opencode | P1 | 所有可改写输入的钩子后增加只能拒绝、不能重新放行的 guard，执行身份和 worktree 边界在此复验。 | 不考虑（2026-10-04 核定）：钩子由插件提供，插件视为与宿主等价的受信代码；权限限制的对象是模型运行期发起的工具调用，不是插件行为；真正的强制隔离应由沙箱提供（见 §8.0 设计原则与 SEC-12） |
| TOOL-10 | 错误状态和可重试性 | codex、harness、opencode | P1 | 区分 `invalid_args/respond_to_model/denied/timeout/cancelled/fatal`；模型可修正的错误才回模型，致命错误结束本轮。 | 不考虑，已实现 |
| TOOL-11 | 工具定义公告身份 | opencode、openclaw | P1 | 请求中记录工具名、定义哈希、插件实例 id；结算前不一致就返回“工具已变化，请重新调用”，不把旧调用打到新实现。 | 暂不考虑 |
| TOOL-12 | 工具可用性与授权分层 | openclaw、opencode、dsh | P1 | 可用性只描述模型能力、插件状态、附件能力和执行环境；授权仍由 Permission 中间件负责。提供诊断原因。 | 可行（2026-10-04 核定）：并入 ARC-6 的启动/装配快照（工具清单 + 来源 + 启用状态 + 被过滤原因）；授权仍由权限中间件负责，不新建分层对象 |
| TOOL-13 | 审批缓存和执行身份绑定 | codex、openclaw、opencode | P1 | 规范化目标作为缓存键；持久化后提供撤销。批准时保存可执行绝对路径、argv、cwd，执行前复验；若暂时不能绑定完整执行身份，先只实现审批缓存，不放行身份未复验的高风险执行。 | 不考虑（2026-10-04 核定：身份绑定对同进程插件无约束力，收益不足） |
| TOOL-14 | 工具注册表暴露等级 | codex、dsh | P1 | 保留当前冲突拒绝；增加 `always/deferred` 暴露级别和命名空间，延迟工具不占完整 schema 预算。 | 后续计划（2026-10-04 核定：现有延迟加载实现不完善，暂不启用；不作为当前实施项） |
| TOOL-15 | 工具后置上下文 | dsh、opencode | P2 | 提供 `deferContext(text, source)`，让工具把发现追加为独立消息，而不是塞进结果文本。 | 不考虑（2026-10-04 核定）：现有 `emit_message_tip` + 结果文本已覆盖需求 |
| TOOL-16 | 文件写串行化 | pi | P1 | 按执行世界和规范化路径加短生命周期 mutex，保护读-改-写；不做全局无限增长 map。 | 可行（2026-10-04 核定，修正形态）：不做互斥锁，改为"按规范化文件路径排队执行"（进程全局队列，与会话无关），保护读-改-写；与 TOOL-1 工具并行化同时落地 |
| TOOL-17 | 执行环境加固 | codex | P0 | `execute_command` 固定 `NO_COLOR=1`、`TERM=dumb`、清空 `PAGER`，注入会话和权限 profile 标识；在平台可用时设置 `LC_ALL=C.UTF-8`，否则使用明确的 UTF-8/C locale fallback，并记录实际环境策略。 | 可行（2026-10-04 核定）：`NO_COLOR`/`TERM=dumb`/`PAGER=cat`（含 `GIT_PAGER`）+ locale 策略 + 记录实际生效策略；**Windows 侧不动 locale**，两条执行路径（boost.process 与 popen 回退）都要覆盖 |

### 5.3 已实现、不要重复做

- share_store 全文外置、定位 id、按行分页。
- `autoFixArgsType`、连续重复调用询问、tool call 中断结果复用。
- 动态插件工具 `shared_ptr` 保活和工具名冲突拒绝。

---

## 6. LLM provider、流式、鉴权和成本

| 编号 | 设计 | 来源 | 优先级 | 融合方式与落点 | 人工核定 |
|---|---|---|---|---|---|
| LLM-1 | 模型能力元数据集中化 | codex、dsh、opencode、pi、openclaw | P0 | `ModelConfig` 增加上下文窗口、最大输出、模态、reasoning、并行工具、严格 schema、缓存能力和价格；provider 请求和压缩统一读取。 | 不考虑（2026-10-04 核定）：不补价格 / 并行工具 / 严格 schema / 缓存能力等元数据；用量账本也不记录 cost（`usage` 表本就没有 cost 字段，无需移除代码） |
| LLM-2 | 错误分类和重试策略 | dsh、opencode、pi、codex、openclaw | P0 | 识别 overflow/auth/rate-limit/timeout/server/invalid-request；读取 `retry-after`，使用有界指数退避和抖动；额度/计费错误不重试。策略数据和执行器分开。 | 可行，各家api错误内容不同，适配可参考这些项目的实现 |
| LLM-3 | 溢出一次性压缩重试 | opencode、openclaw、dsh、harness | P0 | provider 报上下文超限且本轮没有新副作用时压缩一次，复用已经完成的工具结果，最多重试一次，再走硬截断。 | 可行（2026-10-04 核定）：与 LLM-2 的错误分类共用入口；每轮最多一次，且只按明确关键词与状态码（如 `context_length_exceeded`、`maximum context length`）判定，命中才压缩重发 |
| LLM-4 | 静默看门狗 | openclaw、dsh、opencode | P0 | 记录最后 chunk 时间，默认 120 秒可按模型覆盖；只处理中途无输出，不替代整体执行预算。 | 不考虑，已实现 |
| LLM-5 | 假 provider | pi、opencode、harness、codex | P0 | provider 可注入固定流、错误、延迟和 tool call；覆盖重试、压缩、中断、取消和工具循环，不依赖真实网络或额度。录制回放在同一注入接缝中作为后续 P1 扩展。 | 可行，测试中似乎已经实现 |
| LLM-6 | 流式组装唯一实现 | dsh、opencode、codex | P1 | provider 只解析协议 chunk，统一组装文本、thinking、tool call 和结束状态；不让 TUI 和 stdio 各自拼接。 | 现有（2026-10-04 核对）：组装已在 provider 内完成（`processSseBuffer` 等累积 content/thinking/tool_calls 并返回完整 `ChatCompletion`），服务端 EventBridge 统一转 delta，客户端只渲染；仅补一句文档说明，避免后人误加第二份实现 |
| LLM-7 | 消费端退出取消 | codex、dsh、opencode | P1 | 流对象析构或消费方放弃时 RAII 取消 provider，避免连接继续占用。 | 可行 |
| LLM-8 | 缓存断点和缓存用量 | openclaw、opencode、codex | P1 | 先完成 PRM-1/CTX-1，再给 Anthropic 加可选 breakpoint；记录 cache read/write 到 usage ledger。OpenAI 侧只依赖稳定前缀和 provider 自身缓存能力，不假设存在可控断点。 | 可行 |
| LLM-9 | 轮次局部模型回退 | openclaw、dsh | P1 | fallback 只影响当前轮，不改会话选中的模型；结果记录实际模型。 | 后续计划（2026-10-04 核定：暂不考虑、降低优先级）：实施前需先定触发条件、候选模型来源（建议显式 fallback 列表）与能力兼容校验（多模态、上下文窗口） |
| LLM-10 | 凭据来源分层 | opencode、pi | P2 | API key、环境变量、设置库、可选 OAuth 刷新分离；不立即引入完整 OAuth 生态。 | 不考虑（2026-10-04 核定：与 CFG-6 同一项，保持 yaml + `${VAR}` 展开的现状） |
| LLM-11 | 会话级/轮次级连接状态 | codex | P2 | provider 粘性令牌和 websocket 增量状态只在同一轮复用；认证/回退状态按会话保存。 | 不考虑（2026-10-04 核定）：当前 provider 全部走 HTTP（keep-alive 连接池已按模型端点限流），无 WS/令牌/刷新需求 |
| LLM-12 | 结构化输出统一入口 | opencode、pi | P2 | 标题、摘要、计划等场景可通过强制工具式 schema 获得 JSON；不影响现有普通工具协议。 | 后续计划（2026-10-04 核定）：不在本计划实施；触发条件为"出现第一个需要强制 JSON 输出的场景（标题生成 / 计划校验 / 结构化抽取）" |
| LLM-13 | HTTP 录制回放 | opencode、harness | P1 | 新增 `agent/test/http_recorder` 或可注入传输层，按请求摘要和顺序保存/回放响应流；敏感 headers 和 API key 脱敏，固定装置不进入生产代码。 | 可行 |

---

## 7. 上下文压缩和预算

| 编号 | 设计 | 来源 | 优先级 | 融合方式与落点 | 人工核定 |
|---|---|---|---|---|---|
| CMP-1 | `ContextBudget` 单一口径 | openclaw、dsh、harness、opencode | P0 | 集中模型上限、工具 schema、输出预留、buffer、阈值、兜底阈值；统计、预检、压缩共用。 | 不考虑（2026-10-04 核定）：保持现状（口径已集中在一个中间件内，估算函数与阈值常量同处） |
| CMP-2 | 剪枝→度量→摘要 | dsh、openclaw、pi | P1 | 先做确定性工具结果剪枝和噪音清理，再测量，最后才调用摘要模型；每步记录前后 token 和消息范围。 | 不考虑，已实现部分 |
| CMP-3 | 结构化摘要和尾部原文 | opencode、pi、openclaw、codex | P0 | 摘要包含目标、完成、进行中、阻塞、下一步、关键路径/命令/错误；最近消息按显式 token 预算保留。 | 可行（2026-10-04 核定，**已实施**）：默认摘要提示词改为固定小节（Goal / Done / In progress / Blocked / Key facts / Next，见 `agent/lib/src/agent/prompt.cpp` 的 `appendSystemPrompts["summarization"]`）；结构化要点与"最近消息按 token 预算保留"（`recentTokenBudgetRatio = 0.20`）原已实现 |
| CMP-4 | 保留用户意图和原文可回取 | codex、pi、dsh | P1 | 摘要不把用户目标全部丢掉；被压缩原文保留为不可变历史或 share_store 定位符，必要时再投影回模型。 | 不考虑（2026-10-04 核定）：用户意图已由提示词要求近逐字保留；被压缩原文仍在展示历史（`viewMessages` append-only，永不压缩）中可查，模型侧不回取、也不做第二份存储 |
| CMP-5 | 压缩 started/ended 和幂等代次 | dsh、opencode、openclaw | P1 | 压缩开始先写标记，完成才替换上下文；失败或重启可识别；`compactionGeneration` 防同一段重复压缩。 | 不考虑 |
| CMP-6 | 压缩恢复元数据 | codex、opencode、pi | P1 | 记录区间、触发原因、模型、tokensBefore/After、保留段、摘要用量和时间。 | 不考虑 |
| CMP-7 | 压缩质量门 | openclaw、dsh | P2 | 非空、结构标题、关键标识符校验；失败重试一次，仍失败走确定性策略，不写坏摘要。 | 不考虑 |
| CMP-8 | 压缩请求缓存前缀重建 | dsh、codex | P2 | 系统提示、工具 schema、压缩区间先组成稳定前缀，压缩指令放末尾；依赖 provider 缓存能力。 | 可行（2026-10-04 核定）：现状已是"同上下文原始消息 + 末尾追加压缩指令"；CMP-1 已核定不做，故不与工具 schema 口径合并，保持现状即可（无需额外改动） |
| CMP-9 | 压缩前写记忆提醒 | openclaw | P2 | 仅在用户或配置启用时插入提示，不改变压缩正确性。 | 不考虑（2026-10-04 核定：目前还没有记忆设计，不做压缩前写记忆提醒） |

保留当前 `hardTruncate` 的“保证请求可发送”原则；不要用质量优化路径取代可靠兜底。

---

## 8. 权限、审批、出网和安全边界

### 8.0 设计原则（2026-10-04 人工核定确立）

- **权限系统约束的对象是"模型经工具发起的运行期动作"**，不是插件：c++ 插件与本进程同权，
  宿主无法限制同进程原生代码的行为；插件为自身工具注册的权限声明只是"哪些参数是受约束目标"，
  未声明即不参与判定（直接放行）。
- **插件视为与宿主等价的受信代码**：加载即信任，由配置显式指定路径；因此不设计
  "限制插件加载/运行/身份复验"这类门禁机制（已据此否决 TOOL-9、TOOL-13 与 PLG-11 的加载门禁形态）。
- **真正的强制隔离只能由沙箱或操作系统边界提供**（见 SEC-12）。路径规则、工作区隔离、
  三态路径检查属于"门禁判定正确性"措施，不是隔离机制，也不能作为安全承诺。
- 权限规则、声明与 UI 文案都必须避免暗示"可以约束插件"；面向用户的安全说明按此口径重写（SEC-9）。

### 8.1 适合融合的设计

| 编号 | 设计 | 来源 | 优先级 | 融合方式与落点 | 人工核定
|---|---|---|---|---|---|
| SEC-1 | 危险工具权限声明全覆盖 | harness、codex、openclaw | P0 | `ProcessExec/NetEgress/DeviceCapture/FsRead/FsWrite` 等 scope；补齐 execute_command、websearch、RAG、MCP、截图和桌面控制。过渡期未声明工具 warning，严格模式拒绝加载。 | 不考虑（2026-10-04 核定：暂不补声明——`execute_command` 这类工具难以界定可靠的权限目标；待沙箱方案（SEC-12）成熟后一并评估） |
| SEC-2 | PermissionDecision 和理由 | harness、dsh、openclaw | P0 | `decision/reason/rule/target/category`；区分隔离、配置拒绝、完全授权、规则命中、询问、未解析。日志、卡片、诊断共用。 | 可行 |
| SEC-3 | 审批记忆持久化和撤销 | codex、opencode、pi、openclaw | P0 | saved approval 写 `settings_db` 或会话库；提供列表、删除、来源和时间；区分一次性允许和记住。 | 不考虑（2026-10-04 核定：保持现在"仅本次运行有效"的语义，不持久化记住的选择） |
| SEC-4 | 出网策略 | harness、codex、openclaw | P0 | connect 前解析和分类公网、loopback、private、link-local、reserved；HTTP/WS/MCP/外部命令目标统一检查。默认阻止 agent 主动访问内网；明确配置的本地模型端点和本地 MCP 端点必须通过单独 allowlist 放行，错误不泄露内网可达性。 | 不考虑 |
| SEC-5 | 运行期权威复验和单调 guard | openclaw、dsh、codex | P0 | 询问完成后、真正副作用前再次检查；批准的命令身份和工具定义不一致即拒绝；后续插件不能翻转安全拒绝。 | 可行（2026-10-04 核定，表述收窄）：只做"执行前对**模型本次工具调用**的目标复验（批准目标 = 实际执行目标）"，避免"批准的是 A、执行的是 B"；删去"后续插件不能翻转安全拒绝"等对抗插件表述（插件视为受信代码，见 §8.0） |
| SEC-6 | 符号链接真实路径 | codex、harness、openclaw | P1 | 对存在路径优先 `weakly_canonical` 再判定，先覆盖写操作；同步 `decidePaths` 和文件系统工具，评估 Windows 语义。 | 未来计划（2026-10-04 核定）：门禁判定正确度问题（非隔离机制），当前不做；实施时先覆盖写操作，并在 SEC-9 文档中写明边界 |
| SEC-7 | 仅元数据审计 | harness、codex、openclaw、pi | P1 | 记录时间、opId/callId、工具、scope、target 摘要、decision、reason、duration、status，不复制参数/提示词/文件内容。 | 不考虑 |
| SEC-8 | 项目信任 | pi、opencode、openclaw | P1 | 首次加载项目级配置、skill、memory 前询问；不信任时不加载项目资源。 | 不考虑（2026-10-04 核定，与 CFG-7 一致）；保留为未来可选：首次在某项目路径启动时询问一次 |
| SEC-9 | 安全责任文档 | pi、opencode、codex、openclaw | P0 | 明确权限不是沙箱，宿主权限仍有效，worktree 只限制写边界，插件是同进程原生代码，列出软链接和出网覆盖范围。 | 可行（2026-10-04 核定）：按 §8.0 三条原则重写——① 权限只约束模型经工具发起的运行期动作；② 插件与宿主同权、不受权限系统约束（声明为约定，未声明即放行）；③ 门禁不是隔离，强制隔离只能靠沙箱（SEC-12）；并列出软链接与命令出网的覆盖边界 |
| SEC-10 | 规则定义与纯函数判定 | harness | P1 | `PermissionRuleDefinition` 与 `PermissionVerifier(Input)` 分离，输出机器可读 violation；方便导入导出和单测。 | 不考虑 |
| SEC-11 | 规则样例校验 | codex、harness | P1 | 规则可带正例/反例，加载时验证；错误带规则位置。 | 不考虑 |
| SEC-12 | 可选沙箱执行后端 | codex、harness、dsh | P2 | Linux bubblewrap/Landlock、macOS Seatbelt 作为“可用则收窄”的后端，不作为默认，不在 Windows/Android 假装提供等价隔离。 | 后续计划（2026-10-04 核定）：作为"唯一的真隔离路径"保留为后续计划，不在当前计划实施；实施前需先定平台能力与不做假承诺的文案 |
| SEC-13 | 先读后编辑 | dsh | P2 | 文件写工具可选要求先读目标，作为插件策略，不写进通用权限硬规则。 | 不考虑，已在提示词建议 |
| SEC-14 | 内容侧安全扫描 | harness | P2 | 写文件、提交或导出前可扫描常见凭据模式；扫描结果只给结构化告警，不把文件内容复制到审计记录。与权限判定分开，可按配置关闭。 | 不考虑 |
| SEC-15 | 通用动作权限 | opencode、dsh | P2 | 在保留路径规则的基础上，为进程终止、MCP 方法和设备动作提供 `action + target` 作用域；先由少数插件试用，不把所有动作一次性抽象成复杂 RBAC。 | 不考虑（2026-10-04 核定）：与 SEC-1 同族（动作类工具难以界定可靠目标），在"未声明即放行"的取向下只增加声明负担 |

### 8.2 已有能力不重复做

- worktree 隔离优先、配置拒绝优先、最长前缀规则、`check_paths` 三态过滤和完全授权广播已经存在。
- 不把 pi 的“没有权限系统”作为学习目标。

---

## 9. 子代理、后台作业和自动化（2026-10-04 核定：整节不考虑，留作未来计划）

| 编号 | 设计 | 来源 | 优先级 | 融合方式与落点 | 人工核定 |
|---|---|---|---|---|---|
| JOB-1 | 持久化作业表和状态机 | harness、dsh、opencode、codex、pi | P0 | `jobId/type/owner/state/scheduled/deadline/progress/output/error/attempt/group`；先落库再执行；启动回收超期 running；结束写终态。 | 不考虑（2026-10-04 核定：子代理整块改造较大，留作未来计划） |
| JOB-2 | 作业进度和卡点 | harness | P0 | 进度行、当前工具/命令/子代理、可跳转 detail（share id、文件位置、job id）；超时不能只显示一句失败。 | 不考虑（2026-10-04 核定：子代理整块改造较大，留作未来计划） |
| JOB-3 | 工具和命令后台模式 | dsh、openclaw、opencode、codex | P1 | execute_command 支持 `background`，轮次立即返回 job id；提供 list/read/stop/wait；owner 销毁取消并等待。 | 不考虑（2026-10-04 核定：子代理整块改造较大，留作未来计划） |
| JOB-4 | 子代理记录和 parent link | opencode、pi、dsh、openclaw | P1 | 记录 task、父子 session、状态、开始/结束、usage、摘要；UI 可查询上次未完成委派。 | 不考虑（2026-10-04 核定：子代理整块改造较大，留作未来计划） |
| JOB-5 | 配额和独立车道 | openclaw、codex、pi | P1 | 前台轮次、子代理、后台整理分开预算；RAII 释放配额；等待方不占执行槽位。 | 不考虑（2026-10-04 核定：子代理整块改造较大，留作未来计划） |
| JOB-6 | fork 父上下文 | codex、openclaw、dsh | P1 | 子代理可带父会话最近 N 轮/摘要，默认去掉危险工具结果；明确 `empty/inherit/fork` 三种上下文模式。 | 不考虑（2026-10-04 核定：子代理整块改造较大，留作未来计划） |
| JOB-7 | 子代理默认收窄工具 | openclaw、codex、dsh | P1 | 默认不提供会话写、子代理递归和跨会话工具，需要时显式启用；保留当前深度和取消级联。 | 不考虑（2026-10-04 核定：子代理整块改造较大，留作未来计划） |
| JOB-8 | 可继续子代理 | dsh、opencode、pi | P2 | 保留子会话和 inbox，支持追问、等待和冷恢复；先不做多提供方。 | 不考虑（2026-10-04 核定：子代理整块改造较大，留作未来计划） |
| JOB-9 | 轻量定时任务 | openclaw、pi、dsh | P2 | 任务表 + `steady_timer`，重启最多补偿一次；后台自主执行必须有范围、触发器、审批门、升级规则。 | 不考虑（2026-10-04 核定：子代理整块改造较大，留作未来计划） |
| JOB-10 | 跨会话观察者 | openclaw、dsh | P2 | 持久游标、合并通知、避免自身事件回送；依赖 STO-3。 | 不考虑（2026-10-04 核定：子代理整块改造较大，留作未来计划） |
| JOB-11 | 子代理完成与目标达成分开 | openclaw、pi | P2 | 子代理终态只表示委派已结束；结果中分别记录 `finished` 和 `goalStatus`，父代理不能仅凭完成事件把用户目标标为完成。 | 不考虑（2026-10-04 核定：子代理整块改造较大，留作未来计划） |
| JOB-12 | 后台记忆整理 | openclaw、dsh | P2 | 空闲时用独立小预算生成可人工复核的长期记忆候选，保留原始出处，不直接静默修改常驻记忆，也不占用回复路径。 | 不考虑（2026-10-04 核定：子代理整块改造较大，留作未来计划） |

保留现有 `AgentHost::spawnBatch` 批量委派、根 agent 工具/worktree 继承、深度预算和 `makeSubagentResumeKey`；不引入完整 pico 状态机或脚本工作流解释器。

---

## 10. 客户端、UI 和渲染（2026-10-04 核定：见下表，部分条目列为未来计划）

| 编号 | 设计 | 来源 | 优先级 | 融合方式与落点 | 人工核定 |
|---|---|---|---|---|---|
| UI-1 | 客户端模型层 | dsh、codex、opencode | P1 | 从 TUI 中抽出会话/消息窗口/队列/分页/重连/中断的状态镜像；UI 只读快照，方便无终端单测。 | 可行（2026-10-04 核定，限定范围）：只抽三块（历史分页窗口 / 消息队列镜像 / 重连与 seq 校验）到无 FTXUI 依赖的模型类 |
| UI-2 | 渲染和命中快照 | codex、openclaw、harness、opencode、pi | P0 | 固定尺寸输出纯文本和命中区列表；覆盖表格、树、差异、Markdown、表单、中断。 | 可行（2026-10-04 核定）：与 TST-5 合并为“UI 快照夹具”（固定尺寸文本 + 命中区基线 + 一键更新） |
| UI-3 | 未知组件宽容降级 | opencode、pi | P1 | 未知 kind、字段和缺失能力按 schema 适配为 Text/plainText；添加未来版本组件测试。 | 可行（2026-10-04 核定）：未知 kind 降级已实现，补“未知字段 + 高版本组件”两个测试 |
| UI-4 | 渲染层边界测试 | opencode、pi | P1 | 断言 `ui_components`、解析和命中不依赖网络、会话和插件管理器。 | 可行（2026-10-04 核定）：运行时（空注册表渲染）+ 静态边界检查各一条 |
| UI-5 | 原子刷新和 IME 光标 | pi | P1 | 帧提交包裹同步输出；输入栏提供硬件光标位置，终端不支持时降级。 | 可行（2026-10-04 核定，限定范围）：只做“输入栏硬件光标 + 终端不支持时降级”；帧提交包裹不做 |
| UI-6 | Markdown offload/虚拟终端 | opencode、pi | P1 | 长文本解析、高亮、测量可卸载；用轻量 VT 解析器验证最终屏幕。折行和测量继续使用同一函数。 | 不考虑（2026-10-04 核定） |
| UI-7 | 统一浮层管理器 | pi | P2 | 统一锚点、尺寸、焦点回退、层级和可见性；逐步迁移。 | 写入未来计划（2026-10-04 核定）：需要时再引入最小浮层模型，当前不实施 |
| UI-8 | 进度展示位和 UI slot | openclaw、dsh、opencode | P2 | 宿主维护可原地更新进度卡；为消息尾部、侧栏、工具卡片提供具名 slot，不让插件替换整个面板。 | 不考虑（2026-10-04 核定：作业整节不做，进度卡需求随之下降） |
| UI-9 | 能力和体验级别声明 | openclaw、dsh、pi | P2 | 除组件 kind 外声明表单多字段提交、取消、布局尺寸、终端能力。 | 可行（2026-10-04 核定，低成本）：往能力表补体验级别字段 |
| UI-10 | 客户端文案门禁 | dsh、openclaw | P2 | 新增界面文案走 i18n 字典；至少检查新增 UI 文件不出现硬编码面向用户文本。 | 不考虑（2026-10-04 核定） |

保持 `UiFormState`、`UiHitRegion`、`OwnedReflect`、测量/渲染共用实现和唯一 FTXUI 渲染器，不照搬多前端各自渲染。

---

## 11. Wire 协议、SDK 和服务形态

| 编号 | 设计 | 来源 | 优先级 | 融合方式与落点 | 人工核定 |
|---|---|---|---|---|---|
| PRO-1 | 消息往返测试 | codex、harness、pi、opencode | P0 | 为每个 MsgType 补序列化→反序列化字段测试；现有逐条中文注释继续保留。 | 可行 |
| PRO-2 | 幂等副作用请求 | openclaw、opencode、harness、pi | P0 | 与 LOOP-5 统一实现。 | 不考虑（2026-10-04 核定，与 LOOP-5 一致）：客户端不引入发送确认/重试 |
| PRO-3 | 协议版本和能力握手 | pi、opencode、openclaw | P1 | Hello/HelloAck 增加 protocolVersion、能力列表、可选消息；不支持时明确降级或拒绝。 | 可行 |
| PRO-4 | 单一定义生成 schema/文档 | codex、dsh、harness、opencode | P1 | 先生成 `wire-schema.json` 和字段文档，再考虑 C++ 编解码/TS 绑定；不把整个项目改成代码生成。 | 可行 |
| PRO-5 | 连接阶段和错误分类 | pi、opencode | P1 | `unhandshaken/unbound/ready/reconnecting/draining` 状态；明确 SessionNotFound、MessageNotFound、InvalidState。 | 可行 |
| PRO-6 | durable history after 游标 | opencode、dsh、pi | P1 | 与 STO-3 对接；实时 delta 仍走现有重放和全量 sync。 | 并入 STO-4（2026-10-04 核定）：由 `view_message.seq` + `hello.afterSeq` 增量补拉承载；实时片段不承诺重放、不单列本项 |
| PRO-7 | 请求级取消和 attachment 栅栏 | pi、harness、opencode | P1 | 长请求带 request id，取消只取消对应请求；会话重绑用 attachmentId 防旧界面写新会话。 | 可行（2026-10-04 核定，限定范围）：只做"端点入口统一校验 `req.sessionId` 与当前绑定会话不匹配即拒绝"（现在只有历史分页请求做了该校验）；**不做请求级取消**（会话内同时只有一个轮次，现有取消语义已够用，且穿透到 provider 流成本高） |
| PRO-8 | stdio JSONL 一次性运行 | codex、pi、opencode | P1 | 复用 Wire 结构和语义，只增加 JSONL 分帧；stdout 只输出协议记录，诊断到 stderr，响应不等于轮次完成。 | 可行 |
| PRO-9 | daemon/control socket | codex、pi、harness | P2 | server 后台常驻、PID/锁、健康检查、attach；依赖 writer lease 和连接阶段机。 | 不考虑（2026-10-04 核定）：本项目已支持 server 常驻后台、单进程合并启动 server+client、FFI 等多种形态，无需另做 daemon/control socket |
| PRO-10 | 轻量开放 SDK | pi、codex、opencode | P2 | 若外部需求明确，先用生成 schema 做一种语言；嵌入和远程保持同一 API 表面。 | 不考虑（2026-10-04 核定）：嵌入已支持 FFI 调用动态库，插件侧也有开发 SDK；另做语言绑定无需求 |
| PRO-11 | 统一错误对象和 wire 错误码 | harness、codex、opencode、pi | P0 | 在 `util/exception.h`、工具/节点边界和 `wire_protocol.h` 之间统一 `code/message/details/retryable`；未知码按 internal 处理，展示文本与机器错误码分开。保留现有工具结果文本投影，不把错误 JSON 直接当模型消息。 | 可行 |
| PRO-12 | 工具/轮次/作业结果状态元数据 | harness、dsh、openclaw | P1 | 统一 `success/denied/cancelled/timeout/error` 等终态，供 UI、统计和重试使用；不改变控制流异常仍按取消/中断传播的规则。 | 不考虑（2026-10-04 核定）：工具结果状态元数据与 TOOL-10（不考虑）重叠，作业部分随 JOB 整节不做，轮次终态枚举也被 LOOP-6 的裁定覆盖 |

不引入 REST/Swagger 作为核心交互协议，也不维护 CBOR RPC 和 JSONL 两套消息定义。

---

## 12. 插件和扩展机制

### 12.1 已有机制：不改 ABI，补周边

| 编号 | 设计 | 来源 | 优先级 | 融合方式与落点 | 人工核定 |
|---|---|---|---|---|---|
| PLG-1 | 注册可逆和清理审计 | dsh、opencode、openclaw | P1 | 生命周期框架已有撤销和 owner 清理；补一份统一注册清单、禁用/卸载后的基线断言，以及工具、权限、能力、订阅、UI、定时器和键位的清理测试，不改 ABI。 | 可行 |
| PLG-2 | 声明式贡献集合和重算 | opencode、dsh | P1 | 提示词、工具、资源、UI 描述采用“活动贡献集合”；启用/禁用后重新计算派生状态，减少逆向恢复遗漏。 | 可行 |
| PLG-3 | 细粒度变更事件和批量重算 | opencode、dsh | P1 | 工具集变更只重算工具视图，提示词变更只重算 prompt；批量启停结束只重算一次，同 id 重载保留顺序位。 | 不考虑（2026-10-04 核定）：当前没有会过期的派生缓存——工具定义每次请求现组装（`modelcall.cpp:248/256`），提示词由贡献表实时拼装，插件禁用/卸载时贡献被撤销；将来若为性能引入派生缓存再一并引入失效事件 |
| PLG-4 | 独占能力 slot | openclaw、opencode | P1 | 只为压缩器、context assembler、memory provider 等少数独占能力提供 slot；卸载自动回到内置。工具仍采用叠加模型。 | 可行 |
| PLG-5 | manifest 配置 schema 和静态能力 | openclaw、harness、dsh | P1 | plugin.yaml 声明 config schema、设置 UI 元数据、能力快照；宿主在实例化前校验。 | 不考虑（2026-10-04 核定）：宿主不解析插件 `args` 是刻意设计（参数语义完全由插件定义），声明式 schema 校验会与"原样透传"的取向冲突 |
| PLG-6 | 装配树和域视图查询 | dsh、opencode、harness | P1 | `--dump-config`/诊断页列出插件、接口、依赖、能力、提示词段、工具、图类型和来源。 | 可行 |
| PLG-7 | 教学式错误和信任声明 | dsh、openclaw | P1 | 错误告诉插件作者如何修正；文档明确原生插件同进程、无沙箱、只能加载可信代码。 | 可行 |
| PLG-8 | 清单和文档分页 | openclaw、pi、dsh | P1 | `plugins.md` 拆为入门、生命周期、SDK、宿主、client、规则；接口表数字由生成物或常量校验。 | 可行 |
| PLG-9 | 作用域过滤 | dsh、openclaw | P2 | 全局注册 + agent/subagent 过滤视图，schema 和执行同时不可见；不引入 Cordis realm。 | 不考虑（2026-10-04 核定）：子代理工具白名单（`enableToolFiltering` + `toolWhitelist`）已实现"schema 与执行同时不可见"，多租户式作用域叠加无实际需求 |
| PLG-10 | 插件错误和成本诊断 | openclaw | P2 | 记录插件/阶段/slot 的渲染错误、加载耗时和边际内存。 | 可行（2026-10-04 核定，限定范围）：记录"加载/启停耗时 + 接口协商结果 + 注册计数（工具/钩子/UI/定时器）"到日志与 ARC-6 装配快照；渲染错误计数可选；不做常驻内存/CPU 采样（benchmark 已覆盖边际内存） |
| PLG-11 | 高权限插件显式加载许可 | harness、openclaw | P2 | 插件声明 `ProcessExec`、`DeviceCapture` 或出网能力后，宿主可要求配置明确许可才加载；权限声明和加载许可分开记录。 | 移除（2026-10-04 核定）：与"插件 = 宿主等价受信代码"冲突，且宿主无法约束同进程原生代码；如保留信息价值，只作为"插件能力声明（仅展示/审计，不做加载门禁）"的展示项 |

### 12.2 必须保留

- start 注册事务、stop 可重复、destroy 只释放本地对象。
- `userDisabled` 和 `blockedByDependencies` 分离。
- lease/tombstone/inflight、exactly-once 完成通知和 driver ticket 规则。
- 未满足 optional/require 接口按“跳过并解释”，不是破坏整个宿主。

### 12.3 不采用

不引入依赖变化自动重启、模型写原生插件、第二套进程内插件模型或长期兼容层堆叠。

---

## 13. 配置、模型目录和鉴权

| 编号 | 设计 | 来源 | 优先级 | 融合方式与落点 | 人工核定 |
|---|---|---|---|---|---|
| CFG-1 | resolveConfig 回填和统一校验 | harness、openclaw、opencode | P0 | 集中处理 dataDir/workDir/session root、插件默认项、模型必填、权限组合和路径；按配置模式校验：未配置 dataDir 时保留明确的内存降级警告，启用持久化或指定 session root 时必须验证目录可用；错误带键路径。 | 可行（2026-10-04 核定，限定范围）：校验结果结构化（键路径 + 来源文件 + 致命/警告），补"路径存在性"与"权限组合"两类检查；不做配置对象树重建、不改 base/overlay 合并语义 |
| CFG-2 | config_version 迁移 | opencode、pi、harness、openclaw | P1 | 旧键从“告警并忽略”改为有版本迁移，改前备份，迁移测试；settings_db 也有 schema version。 | 不考虑（2026-10-04 核定）：不写版本、不迁移、不修改源文件也不生成新文件；改为“结构变化时告警建议 + 读取配置时自动修正适配（内存态）” |
| CFG-3 | 配置来源和诊断清单 | pi、harness、openclaw | P1 | 记录 base/overlay/env/默认来源、未知键、路径、插件和模型问题；TUI 可查看。 | 可行（2026-10-04 核定）：并入 ARC-6，由启动快照与 `--dump-config` 承载“来源清单 + 被忽略键”；不单独立项 |
| CFG-4 | 模型元数据和来源 | opencode、pi、codex、openclaw | P1 | 模型能力、窗口、输出、价格、模态和缓存能力；来源优先级默认 < yaml < 可选远程缓存 < 插件。 | 不考虑（2026-10-04 核定）：与 LLM-1 同判——不补模型能力元数据；`ModelConfig` 保持现有字段（窗口、多模态、thinking、超时、连接池），账本不记录 cost |
| CFG-5 | configGeneration 快照 | opencode、openclaw | P2 | 热更新时整体重建并替换 registry，递增 generation；日志和 wire 带 generation，禁止半更新读取。 | 不考虑（2026-10-04 核定）：当前热更新面仅“加模型 / 切模型 / TUI 设置”，世代快照收益不足 |
| CFG-6 | 凭据来源分层 | opencode、pi | P2 | env/config/settings/credential store/OAuth 接缝；先支持 API key 多来源和冷却，不立即接完整 OAuth。 | 不考虑（2026-10-04 核定） |
| CFG-7 | 项目信任 | pi、openclaw、opencode | P1 | 未信任项目不加载项目 yaml、skill、memory 和插件配置。 | 不考虑（2026-10-04 核定，与 SEC-8 一致）；保留为未来可选：首次在某项目路径启动时询问一次 |
| CFG-8 | 设置与配置边界 | pi、opencode、openclaw | P1 | YAML 是可版本化、可分发配置；settings_db 是本机偏好；定义覆盖方向，避免一项两处可配。 | 可行（2026-10-04 核定）：写清边界规则（可分发/团队共享→YAML；本机偏好→settings_db；安全状态→内存或专门表），消除 `tui.lang` 与 yaml `language` 的同名歧义 |
| CFG-9 | 生成式配置/插件目录 | dsh、opencode、openclaw | P2 | 从强类型结构、manifest 和 Wire 生成字段目录，CI 检查新鲜度。 | 可行（2026-10-04 核定）：从 `AgentConfig`/`YamlAppConfig` 生成键目录（键路径/类型/默认值），与 PRO-4 共用生成器骨架，CI 检查新鲜度 |

保留当前 base/overlay 的 merge/replace/remove 语义，不改成只有环境变量的配置系统。

---

## 14. 检索、附件、输出、诊断和可观测性

| 编号 | 设计 | 来源 | 优先级 | 融合方式与落点 | 人工核定 |
|---|---|---|---|---|---|
| RET-1 | 会话全文检索 | codex、dsh、pi、harness | P1 | 先给 view_message 建 FTS5 或轻量关键词索引，支持标题、命中片段和分页；不先做跨库复杂投影。 | 拆两条（2026-10-04 核定）：**RET-1a 协议 + TUI 入口**（可行，与 RET-2 的改名/搜索入口同批做，用户可见）；**RET-1b FTS5 索引**（不考虑：逐会话子串扫描在会话量不大时够用，真出现慢查询再加） |
| RET-2 | 独立标题元数据 | codex、dsh、pi | P1 | `meta.title/titleSource`，自动标题可被用户改名；列表和检索统一使用。 | 可行 |
| RET-3 | 附件校验元数据 | dsh、harness、openclaw | P1 | 当前已有大小和附件数量上限；补实际 mime/size/hash/像素验证、路径读取前权限、HTTP(S) 策略。 | 不考虑 |
| RET-4 | 结构化 spill 定位符 | dsh、codex、openclaw | P1 | 在现有 share_store 上补字节数、行数、首尾预览和读取提示；不复制完整内容到第二个存储。 | 不考虑 |
| RET-5 | 统一 opId/callId | harness、codex、dsh | P0 | 每轮、工具调用、插件操作和 wire 事件带关联 id；日志上下文统一。 | 不考虑（2026-10-04 核定：收益不明显） |
| OBS-1 | 诊断入口 | harness、openclaw、codex | P1 | `get_diagnostics` 输出内存、线程、连接、队列、作业、插件、模型、缓存、最近错误和持久化状态。 | 不考虑 |
| OBS-2 | telemetry 内容边界 | pi、dsh、harness | P1 | `turn/modelcall/tool/session.write` span；属性只允许 id、名称、计数、时长、状态和用量，禁止 prompt、参数、结果、路径内容、凭据和 headers。 | 写入未来设计（2026-10-04 核定）：作为设计约束文档条目保留，不立实施项、不排期（当前没有 telemetry 出口） |
| OBS-3 | 关键指标 | harness、codex、pi | P2 | 首 token、轮次耗时、工具成功/失败/超时、压缩次数、上下文 token、缓存命中；复用 benchmark 基础设施。 | 可行 |
| OBS-4 | 诊断包导出 | pi、harness | P2 | 会话摘要、日志尾部、脱敏配置、插件和环境，便于报障。 | 可行 |
| OBS-5 | 模块级日志开关 | harness | P2 | 按模块和插件前缀调整日志级别，避免全局 debug 破坏 TUI。 | 可行 |

当前日志规范、内存基准、smaps 和 benchmark 开关继续保留。

---

## 15. 测试和质量门禁

| 编号 | 设计 | 来源 | 优先级 | 融合方式与落点 | 人工核定 |
|---|---|---|---|---|---|
| TST-1 | 假 provider | pi、opencode、harness、codex | P0 | provider 可注入固定流、错误、延迟和 tool call；覆盖重试、压缩、中断、取消和工具循环，不依赖真实网络或额度。 | 可行 |
| TST-2 | Wire 往返和 schema 一致性 | codex、harness、pi、opencode | P0 | 每条消息 round-trip；生成 schema 与实现对比。 | 可行 |
| TST-3 | 持久化迁移/恢复测试 | dsh、opencode、pi、harness | P0 | 老库、升级失败、崩溃未闭合轮次、writer 冲突、事件 after 重放。 | 可行 |
| TST-4 | 并发和竞态清单 | pi、dsh、codex | P1 | 取消 vs 工具结算、取消 vs resume、插件卸载 vs 工具执行、持久化节流 vs 轮末、并行结果乱序。 | 可行 |
| TST-5 | UI 快照测试工具链 | codex、openclaw、harness、pi | P0 | 为 UI-2 提供固定尺寸基线、命中区序列化、一键更新和差异输出；覆盖窄终端、CJK、未来未知组件和超长内容。UI-2 定义覆盖范围，本项负责测试夹具与门禁接入。 | 合并进 UI-2（2026-10-04 核定）：作为"UI 快照夹具"一项实施（固定尺寸文本 + 命中区基线 + 一键更新 + 差异输出） |
| TST-6 | 一致性测试骨架 | pi、harness | P1 | SessionStore、share_store、settings_db 的替身/后端统一跑同一语义断言。 | 可行 |
| TST-7 | 边界/导出/清理门禁 | openclaw、dsh、opencode | P1 | 依赖方向、DSO 白名单、接口表集合、UI block 名、插件注册清理和文档路径。 | 可行 |
| TST-8 | CI 一键门禁 | harness、openclaw、opencode | P0 | Debug 构建、fail-fast、回放/协议、边界、负面编译、sanitizer；基准阈值可选。 | 可行 |
| TST-9 | 测试隔离、耗时和脱敏 | openclaw、pi | P1 | 临时 HOME/TMP/XDG、清凭据环境；每模块打印耗时/用例数；路径和 key 脱敏。 | 可行（部分已实施）：测试使用独立临时目录（如 `agentxx_ss_test_*`）、模块耗时与用例数已打印；脱敏与凭据环境清理待补 |
| TST-10 | 安全负面测试 | codex、harness、openclaw | P0 | 未声明危险工具、private URL、软链接越界、配置拒绝绕过、审批失效、陈旧工具身份。 | 可行（2026-10-04 核定，用例集收窄）：保留"未声明工具放行语义、软链接越界、配置拒绝优先于完全授权、工作区隔离优先于白名单"等**门禁正确性**用例；去掉"审批失效、陈旧工具身份"这类针对插件身份的用例（插件为受信代码，见 §8.0） |
| TST-11 | 构建产物级 e2e | dsh、harness、pi | P2 | 使用构建后的 CLI、子进程和 PTY，验证安装布局和插件加载。 | 现有（部分，2026-10-04 核对）：benchmark 已有真实两进程 + PTY 驱动 TUI 子进程场景（`benchmark/bench_resource_real.cpp`）；不新增独立 e2e |
| TST-12 | 覆盖率和基准门禁 | dsh、harness、openclaw | P2 | 核心目录覆盖率作为 review 信息；改动会话/上下文/插件时附 ΔRSS/ΔPSS。 | 不考虑（2026-10-04 核定）：脚本里没有任何覆盖率工具链（Windows/MSVC 成本高）；"改动会话/上下文/插件时附 ΔRSS/ΔPSS"本就是 `AGENTS.md` 约定，保留约定不立门禁 |
| TST-13 | 单一实现状态清单 | opencode、dsh、pi | P1 | `docs/zh-cn/design/roadmap.md` 记录已实现/部分/未实现、代码位置和验收测试；不要再把 TODO 分散在多份比较文档里。 | 可行（2026-10-04 核定，低成本；待二次确认）：现状 `docs/zh-cn/design/roadmap.md` 不存在，本计划收口后按"已实施 / 待实施 / 不做 / 未来计划"落成一份 |
| TST-14 | 安全守卫有效性测试 | dsh、harness | P2 | 安全改动必须包含一条先让回归测试失败、再验证修复的负面用例；避免只写无法证明边界的测试。 | 写入文档约定（2026-10-04 核定）：作为 `agent/lib/AGENTS.md` 的评审约定（不立实施项；实质用例已由 TST-10 覆盖） |
| TST-15 | 测试目录按领域分组 | opencode | P2 | 测试模块继续增长时按 `protocol/session/tools/middleware/ui` 分组，保持现有唯一 include 根和完整头路径规则。 | 不考虑（2026-10-04 核定）：现状已按 `core/ plugin/ client/` 分组，模块名扁平，够用 |

---

## 16. 统一落地批次和依赖

### 16.1 依赖关系

```text
护栏基础
  ARC-1/ARC-2/ARC-7 ──→ TST-7/TST-8
  CFG-1 ──────────────→ 启动诊断、插件配置校验
  PRO-1 ──────────────→ PRO-4、协议版本、SDK

持久化基础
  STO-1 writer lease ─┐
  STO-2 schema 迁移 ──┼─→ LOOP-1 inbox、STO-3 event、STO-6 turn/attempt、STO-8 usage
  STO-5 flush 语义 ──┘
  STO-3 event ────────→ STO-4 after 重放、LOOP-6 轮次、JOB-4 子代理记录、SEC-7 审计

上下文成本链
  PRM-1 stablePrefix ─→ CTX-1 sources/epoch ─→ LLM-8 cache breakpoint
  CTX-2 三态 ──────────→ PRM-3 变更通知
  CTX-3 source ────────→ CMP-2/CMP-4、审计和 UI
  CMP-1 budget ────────→ CMP-3 摘要/尾部 ─→ LLM-3 overflow retry

工具可靠性链
  TOOL-4 timeout + TOOL-2 limit + SEC-5 recheck ─→ TOOL-1 parallel
  TOOL-1 parallel ───────────────────────────────→ TOOL-3 cancel/order
  TOOL-6 aggregate budget ───────────────────────→ TOOL-7 structured spill
  TOOL-11 fingerprint ───────────────────────────→ plugin hot unload safety

安全链
  SEC-1 declarations ─→ SEC-2 decision reason ─→ SEC-3 audit
  SEC-3 saved approval ─→ TOOL-13 approval cache
  SEC-4 netpolicy 独立，可先做
  SEC-6 symlink 依赖 SEC-9 safety docs

作业链
  JOB-1 table ─→ JOB-2 progress ─→ JOB-3 background command
  JOB-1 ───────→ JOB-4 subagent record ─→ JOB-5 quotas/lanes
```

### 16.2 批次安排

#### 批次 A：护栏和可测试基础（P0/P1）

- ARC-1、ARC-2、ARC-3、ARC-7：**已实施/现有**（边界测试模块 `boundaries`、四份目录级 AGENTS.md、`InitStep` 装配清单 + `verifyStartupAssembly`、核心边界纪律）；拆分项 ARC-7b 待排期（P2）。
- PRO-1、PRO-2、PRO-4、PRO-11、LLM-13、TST-1、TST-2、TST-8、UI-2（含 TST-5 快照夹具）：wire 契约、幂等/错误码、录制回放、假 provider、快照和一键门禁。
- CFG-1：启动期配置回填和校验（已核定可行，限定范围：结构化校验 + 路径/权限组合检查；CFG-3 并入本批的 ARC-6 快照）。
- RET-5：opId/callId。
- SEC-9：安全责任边界文档。

完成标准：可以在干净环境运行一条门禁命令；故意破坏边界、协议 round-trip 或 UI 快照时门禁失败。

#### 批次 B：持久化和输入可靠性

- STO-1、STO-2、STO-4、STO-5、STO-8、STO-9、STO-11：**已实施 / 已核定可行**（写租约、schema 迁移链、用量账本、settings 乐观版本；STO-4 限定为 `view_message.seq` + `hello.afterSeq` 增量补拉；STO-5 落盘原因分级；STO-9 只推 `MessageTip` 警告）；STO-10 待定（先量长上下文写盘阻塞时长再决定）。
- LOOP-1、LOOP-2、LOOP-3、LOOP-4、LOOP-6、LOOP-10、LOOP-11：durable inbox、投递模式、状态机、回执、轮次记录和失败留痕。（LOOP-5 幂等键与 LOOP-7 请求抢占 2026-10-04 核定不做。）
- STO-3、STO-4、STO-6、STO-7：关键事件、after 补拉、尝试记录和启动清账。

完成标准：两个进程同时写同一会话时一个明确失败；重启后待处理输入不静默丢失；同一请求重发不重复执行；断线可按 after 补回关键事实；未闭合轮次能收尾。

#### 批次 C：上下文和 LLM 成本

- PRM-1、PRM-2、PRM-5、PRM-7、CTX-7，以及 PRM-4 的"记忆文件过大警告"：稳定段/动态段分离、段落 order、技能优先级与同名裁决、请求体结构 + 稳定段哈希断言、附件引用。（CTX-1 ~ CTX-6、CTX-8、CTX-9 与 PRM-3、PRM-6 已核定不做；PRM-4 不做分层与按需检索）
- CMP-1、CMP-2、CMP-3、CMP-4、CMP-5、CMP-6；LLM-2、LLM-3、LLM-4、LLM-5、LLM-8、LLM-13。（LLM-1 元数据集中化 2026-10-04 核定不做）

完成标准：连续两轮请求中稳定请求段保持前缀关系；动态来源只在变化时更新；来源读取失败不会抹掉旧值；超限只压缩一次且不重放已完成工具；摘要失败走确定性兜底；实际请求 token 和账本一致。

#### 批次 D：工具并发和安全

- TOOL-1、TOOL-2、TOOL-3、TOOL-16、TOOL-17：工具并行化（分类与上限并入）、按规范化路径排队串行写、执行环境加固。（TOOL-4 超时、TOOL-9 单调 guard、TOOL-13 审批缓存与身份绑定已核定不做；TOOL-14 延迟工具暂不启用）
- TOOL-5、TOOL-6、TOOL-7、TOOL-8、TOOL-10、TOOL-11、TOOL-12：按 id 结算、结果守卫、聚合预算、结构化定位、错误分类与可用性诊断（其中 TOOL-5/6/7/8/10 已核定"不考虑/已实现"，TOOL-12 待确认是否并入 ARC-6）。
- SEC-2、SEC-5、SEC-9：权限决定理由、执行前目标复验（按 §8.0 收窄表述）、安全责任文档重写。（SEC-1、SEC-3、SEC-4、SEC-7、SEC-8、SEC-10、SEC-11、SEC-13、SEC-14、SEC-15 已核定不做；SEC-6 软链接判定与 SEC-12 沙箱列为未来计划、不在当前计划实施）

完成标准：只读工具耗时接近最大值而不是总和；写/交互工具仍有序；取消不留悬挂 tool call；多个结果总量受限但原文可回取；未声明危险工具有明确策略；private/loopback 地址在连接前被拦截；每个权限分支有理由和审计记录。

#### 批次 E：后台任务、插件和配置演进

- JOB-1、JOB-2、JOB-3、JOB-4、JOB-5、JOB-6、JOB-7、JOB-11：作业表、进度、后台命令、子代理记录、配额、fork 和目标状态。（**整节 2026-10-04 核定不考虑，留作未来计划**：子代理整块改造较大）
- PLG-1、PLG-2、PLG-3、PLG-4、PLG-5、PLG-6、PLG-7、PLG-8：贡献重算、变更事件、批量启停、slot、清单 schema、诊断和文档。
- CFG-4、CFG-8、CFG-9（CFG-4 已裁定与 LLM-1 同判不做；CFG-2、CFG-5、CFG-6、CFG-7 已核定不做，CFG-3 已并入 ARC-6）；UI-1、UI-3、UI-4、UI-5、UI-9（UI-2 归批次 A；UI-6、UI-8、UI-10 已核定不做；UI-7 列入未来计划）；PRO-3、PRO-5、PRO-7（限"会话 ID 校验统一化"）、PRO-8（PRO-6 并入 STO-4；PRO-12 已核定不做）；RET-1a（协议 + TUI 搜索/改名入口，与 RET-2 同批；RET-1b FTS5 不做）。

完成标准：长命令不阻塞父轮次，进程重启可看到超期作业；插件卸载后所有注册恢复到基线；配置迁移有备份和回滚；客户端模型能独立测试重连/分页；老客户端按能力协商降级。

#### 批次 F：按需求扩展

- JOB-8、JOB-9、JOB-10、JOB-12、CTX-6、CTX-8、CTX-9、CMP-7、CMP-8、CMP-9、SEC-12、SEC-13、SEC-14、SEC-15、RET-2、RET-3、RET-4、OBS-3、OBS-4、OBS-5、TST-11、TST-12、TST-13、TST-14、TST-15。（2026-10-04 核定：SEC-8、CFG-7、CFG-6、CFG-5、CFG-2、JOB 整节、UI-6、UI-8、UI-10、PRO-2、PRO-9、PRO-10、PRO-12、PLG-3、PLG-5、PLG-9、PLG-11、ARC-7b、STO-10、SEC-15、TST-12、TST-15、RET-1b 不做；TST-5 并入 UI-2；UI-7、SEC-6、SEC-12 列入未来计划；TST-14 写入 AGENTS.md 约定；OBS-2 作为未来设计约束保留；LLM-9、LLM-12 列为后续计划、不在本计划实施。）
- 每项开始前先补“需求、成本、取消边界、权限、持久化和验收”设计，不因比较文档提到就自动实施。

---

## 17.1 P0 总表

P0 不是“所有重要功能”，而是第一阶段必须解决的风险和明显损失：

| 编号 | 设计 | 直接收益 |
|---|---|---|
| ARC-1 | 架构边界检查（已实施） | 防止依赖方向和插件 ABI 规则回退 |
| ARC-3 | 启动装配断言（已实施） | 配置和依赖错误尽早失败 |
| CFG-1 | 配置回填和校验 | 配置错误带键路径并在正确模式下尽早失败 |
| ARC-7 | 核心边界纪律（现有：已写入 `agent/lib/AGENTS.md`） | 防止核心继续膨胀 |
| LOOP-1 | durable inbox | 输入不因重启丢失 |
| LOOP-2 | next-step/next-turn/inject | 插话和插件注入有明确语义 |
| LOOP-5 | 幂等键（2026-10-04 核定不做） | 客户端不引入发送重试，重复执行风险不成立 |
| STO-1 | writer lease | 多进程不互相覆盖会话 |
| STO-2 | schema migration | 数据结构可安全演进 |
| PRM-1/PRM-2/PRM-7 | 稳定前缀、段落顺序、请求体断言 | 长会话前缀稳定、缓存有效 |
| CTX-1/CTX-2/CTX-3 | 来源、基线、三态 | 动态上下文可审计且不误清空 |
| PRM-4 | memory 过大警告 | 大记忆文件不吞上下文（只提示，不截断） |
| CMP-3 | 结构化摘要与固定小节（已实施） | 压缩后保留最近工作细节与下一步 |
| LLM-1/LLM-2/LLM-3/LLM-4/LLM-5 | provider 能力、错误分类、溢出恢复、看门狗和假 provider | provider 失败可分类、可测、可恢复（LLM-1 元数据集中化 2026-10-04 核定不做） |
| TOOL-1/TOOL-2/TOOL-3/TOOL-16/TOOL-17 | 并行、上限、取消、按路径排队写、环境 | 延迟降低且不会无界消耗资源（TOOL-4 超时、TOOL-9 guard、TOOL-13 身份绑定已核定不做） |
| SEC-2/SEC-5/SEC-9 | 权限决定理由、执行前目标复验、安全责任文档 | 让门禁判定可解释（SEC-1/SEC-3/SEC-4 已核定不做；SEC-6 未来计划） |
| JOB-1/JOB-2（2026-10-04 核定不做） | 作业表和进度 | 子代理整块改造较大，留作未来计划 |
| PRO-1/PRO-11 | 协议契约和错误码 | 远程交互可靠，错误可被机器和 UI 区分（PRO-2 幂等键 2026-10-04 核定不做） |
| UI-2 | UI 快照 | 渲染回归可自动发现 | 可行（2026-10-04 核定）：与 TST-5 合并为“UI 快照夹具”（固定尺寸文本 + 命中区基线 + 一键更新） |
| TST-1/TST-2/TST-3/TST-5/TST-8/TST-10 | 测试基础和门禁 | 后续重构有安全网 |
| RET-5 | opId/callId（2026-10-04 核定不做） | 收益不明显，仅在需要统一日志追踪时再评估 |

---

## 17.2 不做清单

以下内容经过六篇反向结论和 agentxx 定位审查，当前明确不做：

1. 完整事件溯源 + 全量投影读模型；只做关键事实事件和 after 补拉。
2. 第二套 agent runtime、双轨迁移和三代操作状态机。
3. 把 agent/lib 拆成大量独立动态库或引入 Effect/DI 容器。
4. 默认容器或系统沙箱；只提供可选执行后端接缝并明确“不等同沙箱”。
5. Redis、Postgres、远端服务等运行必需依赖。
6. REST/Swagger 取代现有 WS/Channel；只生成 schema 和轻量 SDK。
7. 多通道网关、设备节点、语音会议、浏览器、原生壳和插件市场进入核心。
8. 维护两套远程协议或富文本控制标记；附件和 UI 继续结构化。
9. 一次性支持几十个 provider 和 OAuth 全家桶；先补模型能力元数据和凭据接缝。
10. 允许模型直接定义和运行原生插件或无边界脚本编排。
11. 取消 agentxx 已有权限体系，改成“默认本机全权执行”。
12. 仅为追求形式统一而把所有后台工作改成重型任务状态机。

---

## 17.3 设计决策记录要求

每个 P0/P1 实施项完成后，新增或更新 `resource/history/<topic>/` 的记录，至少包含：

- 背景和可复现问题；
- 采用的对象、状态和线程归属；
- 不变量、取消和失败语义；
- 与比较项目不同的地方及原因；
- 测试模块、基准数字和已知限制；
- 实现与本计划的差异。

`plan.md` 只维护最终方向和状态，不再复制完整源码摘录。源码、测试与现行设计冲突时，以源码和失败测试为准，并在本文件的修订记录中说明。

---

## 附录 A：六篇文档和 agentxx 代码的核对范围

### A.1 比较文档

| 文档 | 规模（本次读取） | 重点 |
|---|---:|---|
| `compare-codex.md` | 1142 行 | app-server、轮次投递、rollout/写锁、工具编排、prompt/world state、沙箱、子代理、协议和 UI 快照 |
| `compare-dsh.md` | 1405 行 | Cordis/effect、turn/step/inbox、事件溯源、四段工具流水线、压缩锁、jobs、插件专题 |
| `compare-harness.md` | 1926 行 | 装配、配置、迁移、事件、jobs、权限/netpolicy、可观测、测试和插件边界 |
| `compare-openclaw.md` | 1985 行 | lane/投递、Context Engine、写者租约、结果预算、记忆、协议幂等、插件 slot、测试门禁 |
| `compare-opencode.md` | 1235 行 | durable inbox、Context Epoch、工具 materialize/settle、重试/录制、saved approvals、插件 transform |
| `compare-pi.md` | 1224 行 | operation/drive、entry/branch/lane、事务和账本、假 provider、一致性测试、TUI、RPC |

### A.2 agentxx 关键源码核对

| 模块 | 关键位置 | 已确认事实 |
|---|---|---|
| 会话存储 | `agent/lib/src/agent/session_store.cpp` | 四表 SQLite、连接 LRU、msg_id 就地迁移、无通用 schema version、无跨进程 writer lease |
| 会话循环 | `agent/lib/src/agent/base_agent.cpp`、`agent_runner.cpp` | 单轮图执行、中断恢复、轮首插件清理、上下文和展示历史分离 |
| 输入队列 | `agent/lib/src/agent/io/session_server_agent_io.cpp` | 内存 deque、暂停状态、打断跑队首、delta 环形缓冲、没有 durable inbox 和 ack |
| 上下文 | `agent/lib/include/agentxx/agent/conversation_types.h`、`context.cpp` | typed messages、版本和惰性 JSON、附件已有大小/数量上限 |
| 提示词 | `agent/lib/src/nodes/modelcall.cpp`、`agent/lib/src/agent/prompt.cpp` | 每次就地替换首条 system、动态段混入完整 prompt、按键拼接 |
| 工具 | `agent/lib/src/nodes/toolcall.cpp`、`tools/tool.h` | 参数修正、重试、重复询问、share_store offload、取消占位；同批工具顺序执行 |
| 权限 | `agent/lib/src/middlewares/permission.cpp`、`permission.h` | 声明式目标、三态路径检查、worktree 优先、配置拒绝优先、软链接 TODO、未声明工具放行 |
| provider | `agent/lib/src/nodes/modelcall.cpp`、`protocol/*provider.cpp` | 3 类协议、固定退避和关键字限速、API usage 统计、没有统一 overflow 分类 |
| 插件 | `cxx_pluginxx` + `agent/lib/src/plugins/*` | 生命周期、lease、tombstone、接口协商和多实例已经成熟 |
| UI | `agent/client/src/io/tui/ui_components.cpp`、`agent/lib/include/agentxx/ui/*` | 声明式组件、能力适配、表单宿主状态、命中和测量共用；缺整屏快照门禁 |
| Wire | `agent/lib/include/agentxx/agent/io/wire_protocol.h` | 消息逐条有注释和手写编解码；缺 round-trip、protocol version 和幂等键 |
| 测试/基准 | `agent/test`、`agent/benchmark` | 模块化测试、sanitizer、真实 TUI/两进程/PTY 和资源差异化基准已经存在 |

---

## 附录 B：模块融合后的一句话设计

| 模块 | 最终采用的设计 |
|---|---|
| 架构 | 单核心库 + 端点 transport + C ABI 插件，边界规则自动检查 |
| 轮次 | 图继续表达控制流，输入/投递/轮次结果另建显式状态 |
| 上下文 | Session 是唯一权威；来源快照和动态内容在请求装配的稳定位置更新，不随意插入权威 transcript |
| 持久化 | SQLite 读模型 + writer lease + schema migration + 关键事实事件 |
| 工具 | 有序准备、受限并行、按 id 结算、结构化状态和 share_store 保真 |
| LLM | provider 能力/错误/重试策略数据化，缓存和 usage 可观测 |
| 压缩 | 统一预算，确定性剪枝优先，结构化摘要保留尾部，失败可恢复 |
| 权限 | 工具声明全覆盖，决定带理由，审批可记住，连接前阻断内网 |
| 作业 | 长任务先落库，进度/输出/截止时间/owner 可查询 |
| UI | 描述层不变，补客户端模型、快照、命中和终端组合测试 |
| 协议 | Wire 单一语义，round-trip、版本、幂等和 durable after 补齐 |
| 插件 | ABI 不变，贡献重算、清单 schema、slot、诊断和清理测试补齐 |
| 配置 | base/overlay 保留，统一回填、校验、迁移、来源和模型目录 |
| 测试 | fake provider、回放、迁移/恢复、负面安全、UI 快照和 CI 门禁形成闭环 |

---

*本计划的目标不是让 agentxx 变成 codex、dsh、harness、openclaw、opencode 或 pi，而是把这些项目已经验证过的局部机制，融合到 agentxx 的单线程协程、图引擎、会话权威、C ABI 和声明式 UI 设计中。*

---

## 修订记录

### 2026-10-04：人工核定（第一批：§1 架构装配 + §2 输入投递）

- **已实施核对**（源码 + `work.md` 记录）：ARC-1（`agent/test/core/test_boundaries.cpp`）、
  ARC-2（四份目录级 AGENTS.md）、ARC-3（`InitStep` 清单 + `verifyStartupAssembly`）、
  STO-1/STO-2/STO-8/STO-11、STO-12（存储层）；ARC-7 规则已写入 `agent/lib/AGENTS.md`。
- **核定结果**：
    - ARC-5 可行（**不等待轮次结束**：只做停止输入、停定时器、停插件、刷盘 + 后台任务登记）。
    - ARC-6 可行（限定范围：启动一次性装配快照写日志 + CLI `--dump-config`，不做 TUI 诊断页与 wire 侧 `get_diagnostics`）。
    - ARC-7 标为"现有"，并拆出 **ARC-7b（P2）**：`session_server_agent_io.cpp` 等千行文件按职责拆分。
    - ARC-8 可行（限定范围：先试点 2~3 个消费者，确认测试替身收益后再推广）。
    - ARC-9 不考虑（概念过多、收益不足；只保留"新增配置项经策略对象注入"的评审纪律）。
    - LOOP-5 不考虑（客户端不做发送确认/重试，重复执行风险不成立）。
    - LOOP-7 不考虑（现有"立即中断 + 插入消息 + 重跑"已满足需求，响应快）。

### 2026-10-04：人工核定（第二批：§4 持久化 + §14 检索/可观测）

- **已实施核对**：STO-1（`writer_lease.*`）、STO-2（`kSchemaVersion` + 相邻迁移 + 备份）、
  STO-8（`usage` 表 + 记账点）、STO-11（`setting.version` + `setVersioned`）、
  STO-12（`meta.title`/`setSessionTitle`/`searchSessions`，界面入口待接）。
- **核定结果**：
    - STO-4 可行（限定范围）：`view_message` 加 `seq` 列 + `hello.afterSeq` 增量补拉；
      不新增 `event` 表、不做类型化事件、不建投影器。
    - STO-5 可行：用户输入、工具结算、压缩完成、轮次终态立即落盘；展示历史与统计保持节流；
      两者都写落盘原因日志。
    - STO-9 可行（限定范围）：只做"首次写失败推一条 `MessageTip` 警告 + 恢复后不重复提示"；
      不做 Info 侧边栏 / 状态栏的降级标记。
    - STO-10 待定：前置条件改为"先量出长上下文轮末写盘阻塞 io 线程的时长"。
    - RET-1 待定：存储层 `searchSessions` 已实现；建议拆分（RET-1a 协议/TUI 入口、RET-1b FTS5 索引），待确认。
    - RET-5 不考虑（收益不明显）。
    - OBS-2 写入未来设计：作为设计约束文档条目保留，不立实施项、不排期。

### 2026-10-04：人工核定（第四批：§5 工具系统）

- **核定结果**：
    - TOOL-2 可行：与 TOOL-1/TOOL-3 合并为"工具并行化"，分三次提交（拆 `execTool` → 分类并发与上限 → 取消收尾与按 id 顺序提交）。
    - TOOL-4 不考虑（各工具自带超时已够用）。
    - TOOL-9、TOOL-13 不考虑（插件视为与宿主等价的受信代码；权限限制的对象是模型运行期工具调用，不是插件行为；强制隔离应由沙箱实现，见新增 §8.0）。
    - TOOL-14 后续计划、暂不启用（现有延迟加载实现不完善）。
    - TOOL-15 不考虑（`emit_message_tip` + 结果文本已覆盖）。
    - TOOL-16 可行（形态修正）：不做互斥锁，改为**按规范化文件路径排队执行**（进程全局队列、与会话无关），与 TOOL-1 同时落地。
    - TOOL-17 可行：`NO_COLOR` / `TERM=dumb` / `PAGER=cat`（含 `GIT_PAGER`）+ locale 策略 + 记录实际策略；**Windows 侧不动 locale**；两条执行路径都要覆盖。
    - TOOL-12 未核定（待确认是否并入 ARC-6 的工具清单诊断）。
- **新增 §8.0「设计原则」**：权限系统只约束模型经工具发起的运行期动作；插件与宿主同权、
  不受权限系统约束（加载即信任）；门禁类措施不是隔离机制，真正的隔离只能靠沙箱（SEC-12）。
- **据此需要复查的既有条目**：PLG-11（高权限插件加载门禁，建议移除或改为纯声明）、
  SEC-5（删掉"插件不能翻转拒绝"的对抗表述）、SEC-9（按新口径重写）、TST-10（去掉针对插件身份的用例）、
  SEC-12（提升为唯一真隔离路径）。

### 2026-10-04：人工核定（权限设计复查，§8/§12/§15 逐条）

- PLG-11 **移除**（与 §8.0 冲突；信息价值可保留为"仅展示/审计的插件能力声明"）。
- SEC-5 可行（表述收窄）：只做"执行前对模型本次工具调用的目标复验（批准目标 = 实际执行目标）"。
- SEC-9 可行：按 §8.0 三条原则重写安全责任文档（权限只约束模型工具调用、插件与宿主同权不受限、门禁不是隔离）。
- SEC-12 沙箱：作为"唯一真隔离路径"列为**后续计划，不在当前计划实施**。
- SEC-3 不考虑：保持"记住的选择仅本次运行有效"，不做持久化与撤销入口。
- SEC-1 不考虑：暂不补工具权限声明（`execute_command` 等难以界定可靠目标），待沙箱方案成熟后一并评估。
- TST-10 可行（用例集收窄）：保留门禁正确性用例（未声明放行语义、软链接越界、配置拒绝优先、工作区隔离优先），去掉针对插件身份的用例。
- TOOL-12 可行：并入 ARC-6 快照（工具清单 + 来源 + 启用状态 + 被过滤原因）。
- SEC-15 仍待定（通用动作权限：进程终止 / MCP 方法 / 设备动作）。

### 2026-10-04：人工核定（第五批：§6 LLM）

- **核定结果**：
    - LLM-1 不考虑：不补价格 / 并行工具 / 严格 schema / 缓存能力等元数据；**用量账本也不记录 cost**
      （核对：`usage` 表字段仅 time_ms/model/prompt/completion/total/cached/reasoning/ok/error_kind，
      本就没有 cost 列，无需改代码）；CFG-4 随之改判不考虑（不再与 LLM-1 合并）。
    - LLM-3 可行：与 LLM-2 的错误分类共用入口；每轮最多一次，只按明确关键词与状态码判定 overflow。
    - LLM-6 现有：流式组装已在 provider 内完成（`processSseBuffer` / Responses 版本），
      服务端 EventBridge 统一转 delta、客户端只渲染；仅补文档说明。
    - LLM-9 后续计划（降低优先级）：暂不考虑，实施前需先定触发条件、候选模型来源与能力兼容校验。
    - LLM-11 不考虑：provider 全部走 HTTP，keep-alive 连接池已按模型端点限流，无 WS/令牌/刷新需求。
    - LLM-12 后续计划、不在本计划实施：触发条件为"出现第一个需要强制 JSON 输出的场景"。

### 2026-10-04：人工核定（第六批：§7 压缩与预算）+ 代码改动

- **核定结果**：
    - CMP-1 不考虑：保持现状（估算函数与阈值常量已集中在一个中间件内；不补工具 schema 与输出预留口径）。
    - CMP-3 可行，**已实施**：默认摘要提示词改为固定小节 Goal / Done / In progress / Blocked /
      Key facts / Next（`agent/lib/src/agent/prompt.cpp`）；结构化要点与尾部 token 预算（0.20）原已实现。
    - CMP-4 不考虑：原文仍在展示历史（`viewMessages` append-only）中可查，模型侧不回取、不做第二份存储。
    - CMP-9 不考虑：目前还没有记忆设计。
    - CMP-8 保持现状（现状已是"同上下文消息 + 末尾压缩指令"，CMP-1 不做后无需额外改动）。
- **代码改动 1（提示词微调）**：`agent/lib/src/agent/prompt.cpp` 的
  `appendSystemPrompts["summarization"]` 由编号列表改为固定小节；保留 `Summarize` 关键字与
  `{omitted_note}` / `{max_words}` 占位符（`test_summarization` 的断言依赖）。
- **代码改动 2（拼写修正）**：`ModelConfig::modelContenxtMaxToken` → `modelContextMaxToken`
  （yaml 键 `model_context_max_token` 与 FFI 键 `modelContextMaxToken` 原本就是正确拼写）。
  共 16 个代码文件、63 处；`resource/history/` 下的历史比较文档按约定未改动。
- **验证**：`cmake --build ... --target agentxx_test`（Debug + ASan/UBSan）通过；
  `agentxx_test` 模块 `boundaries` 7/0、`writer_lease` 25/0、`toolcall_args` 173/0、`agent` 192/0、
  `util_misc` 230/0、`session_schema` 73/0、`config_loader` 368/0、`tui_settings` 552/0、
  `summarization` 445/0、`usage_ledger` 21/0、`memgrowth` 15/0 —— 合计 2101 项断言全通过。

### 2026-10-04：人工核定（第七批：§9 子代理、后台作业和自动化）

- **整节不考虑，留作未来计划**（JOB-1 ~ JOB-12 全部）：子代理整块改造较大（作业表、进度与卡点、
  后台命令模式、配额车道、fork 上下文、可继续子代理…），本轮不引入作业模型。
- 现状保留：`AgentHost::spawnBatch` 批量委派、根 agent 工具/worktree 继承、`cfg_.maxDepth` 深度预算、
  取消级联、`EventSubagentProgress` 进度事件；长任务在进程重启后不可恢复，作为已知限制记录在案。

### 2026-10-04：人工核定（第八批：§10 客户端、UI 和渲染）

- **核定结果**：
    - UI-1 可行（限定范围）：只抽三块（历史分页窗口 / 消息队列镜像 / 重连与 seq 校验）到无 FTXUI 依赖的模型类。
    - UI-2 可行：与 TST-5 合并为"UI 快照夹具"（固定尺寸文本 + 命中区基线 + 一键更新 + 差异输出）。
    - UI-3 可行：未知 kind 降级已实现（`ui_components.cpp:1107`），补"未知字段 + 高版本组件"两个测试。
    - UI-4 可行：运行时（空注册表渲染）+ 静态边界检查各一条（`UiRenderCtx` 本身不引用 IO/会话/插件管理器）。
    - UI-5 可行（限定范围）：只做"输入栏硬件光标 + 终端不支持时降级"；帧提交包裹不做。
    - UI-6 不考虑；UI-8 不考虑（作业整节不做）；UI-10 不考虑。
    - UI-7 写入未来计划（需要时再引入最小浮层模型，当前不实施）。
    - UI-9 可行（低成本）：往能力表补体验级别字段（表单多字段提交/取消/布局尺寸/终端能力）。
- **现状记录**：TUI 端点仍是单类（`agent_tui.cpp` 3017 行）承担协议处理 + 状态镜像 + 渲染协调；
  `ui_components.cpp`（2376 行）与 `components/overlays.cpp`（3048 行）为另外两个大文件；
  `UiRenderCtx` 已是纯数据上下文（主题/宽度/缩进/归属/可选注册表快照/折叠查询/翻译/表单状态）。

### 2026-10-04：人工核定（第九批：§11 协议 + §12 插件）

- **核定结果（§11）**：
    - PRO-2 不考虑（与 LOOP-5 一致：客户端不引入发送确认/重试）。
    - PRO-6 并入 STO-4（由 `view_message.seq` + `hello.afterSeq` 承载，不单列）。
    - PRO-7 可行（限定范围）：只做"端点入口统一校验 `req.sessionId` 与当前绑定会话不匹配即拒绝"
      （现状只有历史分页请求做了该校验，`session_server_agent_io.cpp:1474`）；不做请求级取消。
    - PRO-9 不考虑：本项目已支持 server 常驻后台、单进程合并启动 server+client、FFI 等多种形态。
    - PRO-10 不考虑：嵌入已支持 FFI 调用动态库，插件侧也有开发 SDK。
    - PRO-12 不考虑：与 TOOL-10（不考虑）重叠，作业部分随 JOB 整节不做，轮次终态枚举被 LOOP-6 裁定覆盖。
- **核定结果（§12）**：
    - PLG-3 不考虑：当前没有会过期的派生缓存（工具定义每次请求现组装、提示词实时拼装）。
    - PLG-5 不考虑：宿主不解析插件 `args` 是刻意设计，声明式 schema 校验与"原样透传"取向冲突。
    - PLG-9 不考虑：子代理工具白名单（`enableToolFiltering` + `toolWhitelist`）已实现 schema 与执行同时不可见。
    - PLG-10 可行（限定范围）：记录加载/启停耗时、接口协商结果与注册计数（工具/钩子/UI/定时器）到日志与 ARC-6 快照。

### 2026-10-04：人工核定（第十批：§3 上下文、提示词、技能和记忆）

- **核定结果**：
    - CTX-3、CTX-4、CTX-6、CTX-8、CTX-9 **全部不考虑**（机制或需求已满足：`flags`/`extra` 不进 LLM 请求体、
      `history_contents` 保留旧版本、恢复链已存在、系统提示词刻意取不含 worktree 的基准目录、无可失效的派生态缓存）。
    - PRM-1 可行：稳定段（`systemPrompt` + 静态附加段 + 工具 schema）+ 末尾追加的带来源动态消息 +
      每轮稳定段哈希；不做 provider 能力探测；LLM-8 缓存断点依赖本项。
    - PRM-2 可行（限定）：只加 `order` 固定排序字段（同层重复 key 仍失败），**不引入固定槽位枚举**。
    - PRM-4 可行（限定）：**只加"记忆文件过大警告"**，不做分层、不做按需检索、不做截断；
      默认用户添加的记忆文件应当像 skill 一样简略/索引式。
    - PRM-7 可行（限定）：请求体结构断言 + 稳定段哈希断言；不做整段提示词快照。
- **现状记录**：`PRM-4` 的现状是"记忆文件整份内容每轮注入系统消息，无任何大小限制或截断"
  （`MemoryFileMiddleware`，懒加载 + 缓存 + 自愈重读）；本轮只补警告，不改注入方式。

### 2026-10-04：人工核定（第十一批：遗留项收口，全部条目核定完毕）

- **核定结果**：
    - ARC-7b 不考虑（纯重构、无功能收益；当前最大文件 4278 行仍可维护）。
    - STO-10 不考虑（短事务 + 轮末权威写是刻意设计，无阻塞证据）。
    - SEC-6 未来计划（符号链接真实路径判定，实施时先覆盖写操作）。
    - SEC-15 不考虑（与 SEC-1 同族：动作类工具难以界定可靠目标）。
    - RET-1 拆两条：**RET-1a 协议 + TUI 入口（做，与 RET-2 同批）**；RET-1b FTS5 索引（不做）。
    - TST-14 写入 `agent/lib/AGENTS.md` 文档约定（不立实施项）。
    - TST-13 可行（低成本，待二次确认）：`docs/zh-cn/design/roadmap.md` 现状不存在，按"已实施 / 待实施 / 不做 / 未来计划"落成一份。
- **收口结论**：六章比较文档的全部条目均已有明确裁定（可行 / 不考虑 / 未来计划），
  实施按 §16.2 批次 A→F 推进；每项实施要求仍按 §0.5「统一验收原则」（至少一条会失败的测试）执行。
- **最早可开工的批次**：批次 A（ARC-6 启动快照 + `--dump-config`、CFG-1 结构化校验、SEC-9 安全责任文档、
  UI-2 + TST-5 快照夹具、TST-13 roadmap.md）。
