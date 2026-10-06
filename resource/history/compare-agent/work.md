# compare-agent 计划实施记录（第 1–5 章，人工核定可行项）

- 计划原文：[plan.md](plan.md)
- 范围：第 1–5 章中"人工核定"列出**可行**的条目（P0 优先），逐阶段实施并提交
- 本文件按阶段记录：已完成内容、待完成内容、验证方式、注意事项、与计划的差异

## 状态总览

| 编号 | 内容 | 优先级 | 状态 | 落点 |
|---|---|---|---|---|
| ARC-1 | 核心边界可执行检查 | P0 | 完成（已构建 + 测试通过） | `agent/test/core/test_boundaries.cpp`（测试模块 `boundaries`） |
| ARC-2 | 目录级规则文件 | P1 | 完成 | `agent/{lib,client,plugins,test}/AGENTS.md` |
| ARC-3 | 装配清单和启动断言 | P0 | 完成（已构建 + 测试通过） | `agent/lib/src/agent/base_agent.cpp`（`InitStep`/`runInitSteps`/`verifyStartupAssembly`） |
| STO-1 | 会话目录内核级写租约 | P0 | 完成（已构建 + 测试通过） | `agent/lib/include/agentxx/agent/writer_lease.h` + `src/agent/writer_lease.cpp`、`session_store.*` |
| STO-2 | schema 版本和相邻迁移链 | P0 | 完成（已构建 + 测试通过） | `agent/lib/src/agent/session_store.cpp`（`kSchemaVersion`/`applyMigrationStep`/备份） |
| STO-8 | 用量账本 | P1 | 完成（已构建 + 测试通过） | `session_store`（usage 表/聚合）、`nodes/modelcall.cpp`（记录点） |
| STO-11 | settings_db 乐观版本 | P1 | 完成（已构建 + 测试通过） | `agent/lib/{include/agentxx/util,src/util}/settings_db.*` |
| STO-4 | durable 事件流与实时 delta 分开（限定） | P1 | 完成（已构建 + 测试通过） | `view_message.seq` 显式序号 + `hello.afterViewSeq` 增量补拉；模块 `session_sync` |
| STO-5 | 持久化语义分级与 flush | P1 | 完成（已构建 + 测试通过） | `Session::persistNow` / `persistThrottled`（用户输入/工具结算/压缩完成/轮次终态立即落盘） |
| STO-9 | 持久化降级可见（限定） | P1 | 完成（已构建 + 测试通过） | `SessionStore` 写失败原因 + 会话侧 `persistNow` 提示一次（`MessageUITip`） |
| LOOP-1 | 持久化收件箱两段状态 | P0 | 完成（已构建 + 测试通过） | `session_store`（`session_input` 表 / schema v2）、`session_server_agent_io`（受理落库 + 启动恢复） |
| LOOP-2 | `next-step` / `next-turn` / `inject` | P0 | 完成（已构建 + 测试通过） | `wire_protocol`（`delivery`）、`context`（待注入输入）、`session_context`（`drainPendingSessionInputs`）、`modelcall` |
| LOOP-3 | 投递结果显式化 | P1 | 完成（已构建 + 测试通过） | `WireInputAck` + `InputStatus` / `InputRejectReason`；TUI 回执提示 |
| LOOP-4 | QueueState 状态机 | P1 | 完成（已构建 + 测试通过） | `SessionQueueState`（idle/running/paused/draining）+ 队列同步携带状态 |
| LOOP-11 | `collect` 合并投递 | P1 | 完成（已构建 + 测试通过） | `SessionServerAgentIO::Config::collectWindow` + `flushCollectWindow` |
| PRM-1 | stablePrefix/dynamicSuffix | P0 | 完成（已构建 + 测试通过） | `context.cpp`（稳定段/动态段分离）、`modelcall.cpp`（末尾动态消息 + 稳定段哈希） |
| PRM-2 | 段落排序号和固定槽位（限定：只加 order） | P1 | 完成（已构建 + 测试通过） | `prompt.{h,cpp}`（`PromptSectionMeta` / `setAppendSection` / `orderedAppendSections`） |
| PRM-7 | 提示词和请求体快照（限定：结构断言 + 稳定段哈希） | P1 | 完成（已构建 + 测试通过） | 测试模块 `prompt_stability` / `prompt_stability_io` |
| PRM-5 | 技能优先级和同名裁决 | P1 | 待完成 | — |
| CTX-7 | 附件引用而不是反复内联 Base64 | P1 | 待完成（计划标注"需进一步理解实施内容"，先不动） | — |
| STO-12 | 会话检索和标题 | P1 | 存储层完成（界面入口待接） | `session_store` 的 `sessionTitle`/`setSessionTitle`/`searchSessions` |
| STO-13 | 会话导出和取证包 | P2 | 待完成 | — |
| TOOL-1 | 分阶段并行：prepare/dispatch/finalize | P0 | 完成（已构建 + 测试通过） | `nodes/toolcall.cpp`；模块 `toolcall_parallel` |
| TOOL-2 | 并发分类与上限 | P0 | 完成（已构建 + 测试通过） | `tools/tool.h`、`plugin_api.h`、`config.h` |
| TOOL-3 | 并行取消和收尾 | P0 | 完成（已构建 + 测试通过） | `nodes/toolcall.cpp`；模块 `toolcall_parallel` |
| SEC-9 | 安全责任与边界文档 | P0 | 完成 | `docs/zh-cn/design/security.md` |
| TST-13 | 单一实施状态清单 | P1 | 完成 | `docs/zh-cn/design/roadmap.md` |
| CFG-8 | 配置与设置边界（含会话语言接线） | P1 | 完成（已构建 + 测试通过） | `docs/zh-cn/design/configuration.md`、`AgentConfig::languageExplicit` |
| TOOL-17 | 执行环境加固 | P0 | 完成（已构建 + 测试通过） | `plugins/agentxx_execute_command/execute_command_impl.h` |
| PRM-4 | 记忆文件过大警告 | P0 | 完成（已构建 + 测试通过） | `middlewares/memory_file.*` |
| UI-3 | 未知组件宽容降级（补测试） | P1 | 完成（测试通过） | `test_tui_ui_items.cpp` 未知字段/高版本组件 |
| UI-4 | 渲染层边界测试 | P1 | 完成（测试通过） | `test_tui_ui_items.cpp` 空注册表渲染 + `boundaries` 渲染层规则 |
| TST-10 | 安全负面测试（门禁正确性） | P0 | 待完成 | — |

## 阶段 A：护栏与目录规则（ARC-1、ARC-2）

已完成：

- **ARC-1 边界检查**（`boundaries` 测试模块，随 `agentxx_test` 运行）：
  - client 不得引用 `agent/lib/src/**`；`agentxx/*` 引用必须能落到 `agent/lib/include`
    或 `agent/client/include`（防止引用被删/改名的头）。
  - 插件只允许引用 SDK 公开面 `agentxx/plugin/api/*` 与 `agentxx/util/exception.h`，
    其余 `agentxx/*`（agent/middlewares/nodes/tools/event/ui/protocol/plugin 内部头）一律拒绝。
  - lib 不得反向依赖 client（`agentxx-client/*` 或 client 目录路径）。
  - 插件导出白名单：`agent/plugins/CMakeLists.txt` 必须保留 `local: *` +
    `--version-script` + `exported_symbols_list`，且不得打开
    `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS`；插件源码不得手写 `__declspec(dllexport)` /
    `visibility("default")`（必须走 SDK 导出宏）。
  - 扫描量下限断言（client ≥100 / plugin ≥50 / lib ≥100 个源文件），避免目录改名后
    "零文件全通过"的假通过。
- **ARC-2 目录级规则**：新增 `agent/lib/AGENTS.md`、`agent/client/AGENTS.md`、
  `agent/plugins/AGENTS.md`、`agent/test/AGENTS.md`，只写本目录硬约束（引用边界、
  不变量、测试约定），跨目录规则仍留在仓库根 `AGENTS.md`。

注意事项 / 与计划的差异：

- 计划里 ARC-1 写的是"新增 `agent/script/check_boundaries.py` **或**测试模块"，这里选测试模块：
  边界规则只保留一份实现，直接进入项目唯一门禁（`agentxx_test`），不再维护第二份 Python 规则
  （避免两处规则漂移）。
- 二进制级导出校验仍由现有 `agent/script/check_plugin_exports.sh`（Linux/macOS，`nm`）负责；
  测试模块只做源码/构建配置级检查（跨平台、无需构建产物）。

## 阶段 B：持久化基础（STO-1、STO-2、STO-8、STO-11）

已完成：

- **STO-1 写租约**（`SessionWriterLease`）：
  - 写连接（`SessionStore::dbs`）打开会话目录时获取 `.writer.lock`：POSIX `flock(LOCK_EX|LOCK_NB)`；
    Windows 以"不共享"方式 `CreateFileW`（其他进程再次打开拿到 sharing violation）。
  - 进程内可重入（引用计数），跨进程互斥；租约随写连接 LRU 淘汰/对象析构释放；
    内核对象随进程退出释放，因此不设超时、不按 PID/时间戳猜测陈旧锁。
  - 读路径（`loadSession`）不再经过写连接，允许"另一进程在写、本进程只读"。
  - 写路径失败时记录原因（`SessionStore::lastWriteError()`）并记 Error 日志，
    宿主可据此明确告知用户"消息没有保存"，而不是静默丢数据。
- **STO-2 schema 版本与迁移链**：
  - `meta.schema_version` + `SessionStore::kSchemaVersion`；`ensureSchema` 按相邻步骤迁移
    （每步独立事务、幂等、失败不推进版本）。
  - 迁移前备份 `session.db.bak.v{n}`（先 `PRAGMA wal_checkpoint(FULL)`，失败只告警不阻断）。
  - 老库（无 `msg_id` 列、无 `schema_version`、无 `usage` 表）在写路径首次打开时补齐，
    并回填 `msg_id`；重复打开不重复迁移（版本不变、备份不重写）。
  - 高版本库（`schema_version > kSchemaVersion`）读写都拒绝：读返回空并记 Error，
    写失败并在 `lastWriteError()` 给出原因，避免旧程序按老结构改坏新数据。
- **STO-8 用量账本**：
  - `session.db` 新增 `usage` 表（时间/模型/prompt/completion/total/cached/reasoning/ok/errorKind）。
  - `SessionStore::addUsage` / `usageSummary` / `recentUsage`；聚合不依赖内存中的最后一次统计。
  - `ModelCallWrapNode::onReceiveToken` 在每次 provider 调用后记账：成功记用量，
    失败（含取消/中断）记失败行与原因，并保持原有异常/取消传播语义不变。
- **STO-11 settings_db 版本**：
  - `setting` 表新增 `version` 列（老库自动补列，已有数据版本从 0 起算）。
  - `set()` 为无条件写入并递增版本；新增 `getVersioned`/`version`/`setVersioned`
    （乐观写入：期望版本不匹配返回 `Conflict` 且不改内容）。

测试（随 `agentxx_test`）：

- 新模块 `boundaries`、`writer_lease`、`session_schema`、`usage_ledger`；
  `settings_db` 模块增加 `testVersionedWrite`。
- 覆盖：正常/边界/失败路径；老库迁移与幂等；高版本拒绝；外部持锁时写失败且不落数据；
  账本聚合与最近记录；双句柄乐观写入冲突；租约引用计数与内核级互斥（用平台原生 API 复核）。

注意事项：

- 迁移只发生在**写路径**（打开写连接时）。只读打开老库不迁移，历史按老结构照读；
  这是刻意的：读操作不该改磁盘结构，也不该取写租约。
- 写租约默认开启；`SessionStore(root, false)` 可关闭（仅嵌入方明确不需要互斥时使用）。
- 第二个进程写同一会话会失败并记日志。宿主界面提示尚未接入（`lastWriteError()` 已可读），
  列入后续项。
- 用量账本暂不记录成本（`cost`）与 attempt/重试分类：模型价格元数据（LLM-1）尚未落地，
  重试信息目前在 modelcall 内部；账本字段留好，等 LLM-1 完成后补。

## 阶段 C：会话标题与检索（STO-12，存储层）

已完成：

- 标题独立可改：`meta.title` + 新增 `meta.titleSource`（`auto` = 首条用户消息预览，
  `user` = 用户改名）。`setSessionTitle` 一次事务写入标题与来源；自动标题仍然只在
  标题不存在时写入，因此用户改名不会被后续消息覆盖；空标题被忽略。
- 只读查询：`sessionTitle` / `sessionTitleSource` 用临时只读连接，不取写租约、不建目录。
- 会话检索：`searchSessions(keyword, limit)` —— 标题或展示历史正文（`json_extract(json,'$.text')`）
  的子串匹配，大小写不敏感（SQL LIKE），关键词中的 `%` `_` `\` 自动转义；结果按最近活动
  时间降序并按 `limit`（默认 50）截断；正文命中返回关键词附近的片段。
- 实现取舍：按会话目录逐个用只读连接查询，不建 FTS5 索引（计划 STO-12 的"先做标题/
  当前会话搜索，不先建跨库复杂索引"）。无法读取的会话库按跳过处理并记 Debug 日志。
- 测试：`session_schema` 模块新增自动标题/来源、用户改名、改名不被覆盖、空标题忽略、
  未持久化会话不建目录、标题命中、正文命中与片段、通配符转义、空关键词、limit 截断
  （模块 73 用例全通过）。

待接（不属于本次范围）：

- 协议与界面入口：`WireListSessions`/会话弹窗还没有"改名"和"搜索"操作；需要新增
  协议消息（或扩展已有的会话列表请求）并在 TUI 会话弹窗接输入框。存储层 API 已就绪。

## 阶段 J：LLM 错误分类与溢出压缩重试（LLM-2、LLM-3，2026-10-05）

已完成：

- **LLM-2 错误分类与重试策略**（新增 `agent/lib/include/agentxx/nodes/llm_error.h` +
  `src/nodes/llm_error.cpp`）：
  - `LlmErrorKind`：Unknown / Auth / Quota / InvalidRequest / ContextOverflow / RateLimit /
    Timeout / Server；按"HTTP 状态码 + 常见关键词"双口径识别（provider 错误结构各家不同，
    关键词覆盖 OpenAI / Anthropic / 中文错误文本）；
  - `isLlmErrorRetryable`：鉴权、额度/计费、请求非法三类**不重试**（重试无意义，
    直接结束本轮，省掉无用等待与请求）；其余可重试；
  - `llmRetryDelaySeconds`：**有界指数退避 + 抖动**（限流基数 5 秒、其余 2 秒，按次数翻倍，
    夹到 60 秒上限，抖动 0~2 秒由错误文本哈希决定），错误文本里带 `retry-after` /
    `retry_after`（JSON 或 HTTP 头）时**优先采用**该值；
  - `modelcall.cpp` 的重试循环改为按分类决策：不可重试立即走失败路径；日志与 UI 提示
    都带分类文本（`llmErrorKindText`）；原先的 `defaultRateLimitTag`（Aho-Corasick 关键词）
    与 `retry*3 + appendDelay` 固定公式被分类器取代。
- **LLM-3 溢出一次性压缩重试**（`nodes/modelcall.cpp`）：
  - 分类为 `ContextOverflow` 时，**首次**触发 `service.summarization.compact`
    （`EventCompactContext`）并**立即重试**（不等待退避），同时向 UI 推一条 warning 提示；
  - 每轮只压缩重试一次（`overflowCompactUsed`）：第二次仍溢出即按失败结束
    （由上层按需硬截断），避免反复压缩；
  - 无事件总线（嵌入式无压缩服务）时按不可恢复处理，直接结束本轮。

测试：

- 新模块 `llm_error`（`test/core/test_llm_error.cpp`，65 项断言）：八个分类的代表性错误文本、
  可重试性、溢出识别、退避策略（指数增长、上限、限流基数、`retry-after` 优先与夹取、
  抖动不为 0）。
- `agent` 模块新增 `test_agent_llm_error_policy`（本地模拟器新增失败状态码与响应体两个开关）：
  ① 401 鉴权失败 → 只请求 1 次（不重试）；② 400 + `context_length_exceeded` → 压缩后立即
  重试，第二次成功且请求数远小于重试上限；③ 持续溢出 → 恰好 2 次请求后结束。

验证：

- 构建：lib `INSTALL` 与 `agentxx_test` 均 exit=0，无新增 error/warning。
- 测试：`llm_error` 65/0、`agent` 198/0（含新增 3 组错误策略用例）。

注意事项 / 与计划的差异：

- 计划 LLM-2 提到"策略数据与执行器分开"：这里把**策略**放在 `llm_error.{h,cpp}`
  （分类 + 退避计算，纯函数、可单测），**执行器**仍是 modelcall 的重试循环，
  未引入策略对象树。
- 计划 LLM-5（假 provider）要求"可注入固定流、错误、延迟和 tool call"：本地 LLM 模拟器
  已具备这些能力（本次又补了失败状态码与响应体），但尚未抽出为独立"假 provider"
  接缝；仍按"待实施"记录。

## 阶段 I：权限判定理由与执行前目标复验（SEC-2、SEC-5、TST-10，2026-10-05）

已完成：

- **SEC-2 判定理由**（`middlewares/permission.{h,cpp}`）：
  - 新增 `PermissionReason`（`Unresolved` / `WorktreeIsolation` / `ConfigDeny` / `FullAuth` /
    `Rule` / `NoRuleDefault`）与 `PermissionDecision{decision, reason, rule, target, describe()}`；
  - 原 `decideTarget` 内联的所有分支改由新的 `explainTarget` 单一实现给出（`decideTarget`
    退化为取其 `decision`），因此**判定与理由不可能不一致**；
  - 工具权限检查服务（`service.permission.check`）的响应回填 `reason`
    （`checkToolPermission(..., std::string* reasonOut)`），工具被拒时的结果文本由
    `[Permission denied]` 变为 `[Permission denied] <理由 + 命中规则/目标>` —— 模型与用户
    都能看到"为什么被拒"；
  - 每次判定记 Debug 日志（工具/目标/会话/结论/理由）。
- **SEC-5 执行前目标复验**：
  - 判定通过时按 (工具名, `tool_call_id`) 记录"已批准目标"（`approvedTargets_`，
    与判定共用 `resolveDeclaredTargets` 的同一口径：路径目标按会话工作目录规范化、
    数组逐项、空目标跳过）；
  - 新增总线服务 `service.permission.reverify`
    （`ReqPermissionReverify` / `RespPermissionReverify`，topic `PermissionReverify`）：
    用**当前参数**重新解析目标并与已批准集合比对；不一致返回
    `permission target changed between check and execution (approved: [...], executing: [...])`；
  - `ToolcallWrapNode::prepareToolCall` 末尾（执行体启动前）调用该服务，失败即把该调用
    置为 `[Permission denied] <reason>` 短路结果，不执行执行体；纯校验、不重复询问用户，
    未声明权限的工具（无批准记录）视为通过。
- **TST-10 门禁正确性用例**（新模块 `permission`，`agent/test/core/test_permission.cpp`，
  45 项断言）：判定理由全分支（未解析 / 未命中按默认 / 命中规则允许·拒绝·询问 /
  完全授权 / 配置拒绝优先于完全授权 / 工作区隔离写拒绝优先于白名单、读不受限）；
  执行前复验（参数未变通过、目标被改写拒绝且理由含两边目标、未声明权限无约束、
  被拒的调用不产生批准记录）；批量路径三态（`decidePaths`：允许 / 拒绝 / 未获批准）。

验证：

- 构建：lib `INSTALL` 与 `agentxx_test` 均 exit=0，无新增 error/warning。
- 测试：`permission` 45/0、`agent` 192/0、`toolcall_parallel` 48/0、`wire_roundtrip` 165/0、
  `plugin_resources` 88/0、`message_supplement` 95/0 全部通过。

注意事项 / 与计划的差异：

- 计划 SEC-5 表述为"询问完成后、真正副作用前再次检查"；实现为**判定阶段记录 + 执行前
  按同一口径复验目标集合**，不重复发起询问（重复询问会打扰用户，且"记住本次选择"未开启时
  会二次弹窗）。复验能拦下"判定与执行之间参数被改写/规则变化"的情况。
- 计划的 TST-10 还含"软链接越界"用例：软链接真实路径判定（SEC-6）本身是未来计划，
  该用例随 SEC-6 一起做。

## 阶段 H：协议往返测试、会话 ID 校验、wire 错误码（PRO-1、PRO-7、PRO-11，2026-10-05）

已完成：

- **PRO-1 / TST-2 消息往返测试**（新模块 `wire_roundtrip`，`agent/test/core/test_wire_roundtrip.cpp`，
  165 项断言）：对每条 Wire 消息做 `serialize → deserialize → 再 serialize`，断言
  ① 回到同一变体成员、② 两份 JSON 完全一致（字段稳定）、③ 关键字段逐一比对：
  HelloAck（含 plugins/deviceId/workDir/中文会话 id）、TurnResult、ContextStats、WireError、
  ModelInfo（含多模态能力表）、GetModel/SelectModel、GetContext/CompactContext/ContextMessages、
  GetViewMessages/ViewMessagesPage（含 ViewMessage 角色与耗时字段）、ListSessions/SessionList/
  SwitchSession、MessageQueueUpdate/ClearMessageQueue/RemoveQueueItem/InterruptAndRunNext/Cancel、
  InterruptRequest/Response/Expired、PluginData/PluginDataUp、GetAppendComponentInfo/
  AppendComponentInfo（成功与失败条目）、GetPermissionState/SetFullAuth/PermissionState、
  ListDir/ListDirResult（含 WireDirEntry）、AddModel/AddModelResult、Log；心跳（裸 JSON）另测。
  兼容性用例：未知字段被忽略、未知消息类型返回 `nullopt`（不抛异常）。
- **PRO-11 wire 错误码**（`agent_io_transport.h`）：新增 `WireErrorCode`（`Internal` /
  `InvalidState` / `SessionNotFound` / `SessionMismatch` / `InvalidArgs`），并写明"未知码按
  `Internal` 处理"；文本与机器码分开（`message` 给人看、`code` 给程序判断）。
- **PRO-7 会话 ID 统一校验**（`session_server_agent_io.cpp` / `.h`）：新增
  `acceptSessionScope(sessionId, sender, what)`，在 `onPeerMessage` 入口对
  user_input / cancel / interrupt_and_run_next / clear_message_queue / remove_queue_item /
  select_model / get_model / get_context / compact_context / get_append_component_info
  统一校验；不匹配时回 `WireError(SessionMismatch)` 并记警告日志，空 `sessionId`
  视为未指定（按绑定会话处理，兼容旧客户端）。历史分页保持原有"回空页"处理
  （客户端按页解析，回错误会打断其分页状态机）。`WireListDir` 不带 sessionId，不参与校验。

测试（`remote_agent` 模块新增 `session scope validation` 用例）：

- 不匹配的 `user_input` → 收到 `WireError`（码 = `SessionMismatch`，文本非空）；
- 空 sessionId 与匹配 sessionId 的 `clear_message_queue` → 正常受理并回队列快照。

验证：

- 构建：lib `INSTALL` 与 `agentxx_test` 均 exit=0，无新增 error/warning。
- 测试：`wire_roundtrip` 165/0、`remote_agent` 453/0、`boundaries` 8/0、
  `toolcall_parallel` 48/0 全部通过。

注意事项：

- 新增测试源文件后必须重跑一次 CMake 配置（测试工程用 `file(GLOB ...)` 收集源码），
  否则新模块不会参与链接（表现为 `LNK2019 无法解析的外部符号`）：
  `cmake -S test -B <build>/agentxx_test_repo-prefix/src/agentxx_test_repo-build`。
- `run_remote_agent_tests` 等模块的 `recv()` 无超时：测试里"期待某条消息"的断言必须确保
  该消息真的会被发送（例如空闲且队列为空时的 `user_input` 不推送队列更新，会导致等待挂起）。

## 阶段 G：工具三段式执行与受限并行（TOOL-1、TOOL-2、TOOL-3，2026-10-05）

已完成：

- **TOOL-1 分阶段执行**（`agent/lib/src/nodes/toolcall.cpp`）：把原来的 `execTool`
  一次做完的流程拆成三段，语义等价：
  - `prepareToolCall`：工具查找（静态表 → 动态插件注册表，插件工具 `shared_ptr` 保活）、
    参数解析与 `sessionId`/`tool_call_id` 注入、参数类型修正、权限检查、连续重复调用确认；
    产出 `PreparedToolCall`（含 `args`/`repeatKey`/`shortCircuit`/`startMs`/`parallelSafe`）。
  - `runToolCallBody`：只执行执行体（含 `maxRetry` 重试），返回原始结果文本。
  - `finalizeToolCall`：结果定稿（超限时经 share_store 卸载并给定位提示）。
  - `execTool` 保留为"单次调用完整路径"（prepare → run → finalize），行为与旧实现一致。
- **TOOL-2 并发分类与上限**：
  - `XXToolBase` 新增 `supportsParallel`（构造参数 + `extra["supportsParallel"]`，默认 false=独占）；
  - 插件 API 新增 `AGENTXX_PLUGIN_TOOL_FLAG_PARALLEL_SAFE`（`plugin_api.h`），
    `PluginTool` 按该位设置；`agentxx_filesystem` 的 list/read/glob/grep 声明并行安全
    （`kReadOnlyFlags`），写/编辑保持独占；
  - `AgentConfig::toolParallelMaxConcurrency`（默认 4，夹到 [1, 32]）；
  - 分批规则：把**连续**的并行安全调用合成一批（不超过上限），遇到独占调用即断批 ——
    独占工具与其前后调用形成顺序屏障（写文件/命令执行/交互询问的先后关系不变）。
- **TOOL-3 并行取消与收尾**：
  - 结果按 `declaredToolCalls` 顺序写回（完成顺序 ≠ 提交顺序时仍按声明顺序入上下文）；
  - 批内并发用 `co_spawn` + `experimental::channel` 收集完成信号（与 `AgentHost::spawnBatch`
    同一套做法），子协程捕获共享状态（batch 拷贝进 `shared_ptr`，不引用父协程栈）；
  - 批次自带 `asio::cancellation_signal`，子协程经 `bind_cancellation_slot` 绑定：
    会话取消时先发取消信号让子协程的挂起等待尽快结束，再让调用方按取消路径收尾
    （已完成结果保留、未完成补 `[User canceled]`、每条 tool_call 都有回复）；
  - 所有逃逸异常在 `runPreparedToolCallGuarded` 收口（`NodeInterrupt` → 中断语义；
    其余 → 取消语义）：派生协程里的未捕获异常会走到 detached 处理器并终止进程，必须兜住。

测试（新模块 `toolcall_parallel`，`agent/test/core/test_toolcall_parallel.cpp`，48 项断言）：

| 用例 | 覆盖点 |
|---|---|
| T1 | 3 个并行安全工具区间重叠（b/c 在 a 结束前已开始）、完成顺序与声明顺序不同、结果仍按声明顺序 |
| T2 | 独占工具与前后调用互不重叠且顺序严格（p1 → w → p2），结果顺序正确 |
| T3 | 并发上限 2 生效（3 个调用分成 2 + 1 批，同时在跑峰值 = 2） |
| T4 | 取消：快工具结果保留、执行中的与未启动的补 `[User canceled]`、未启动的执行体确实未运行 |

验证：

- 构建：`agentxx_lib_repo-build` 的 `INSTALL` + `agentxx_test` 均 exit=0，无新增 error/warning。
- 测试：`toolcall_parallel` 48/0、`toolcall_args` 173/0、`agent` 192/0、`boundaries` 8/0、
  `message_supplement` 95/0、`cancel` 45/0、`session_persistence` 621/0、
  `plugin_resources` 88/0、`usage_ledger` 21/0 —— 全部 0 失败。

注意事项 / 与计划的差异：

- 计划把 TOOL-1/2/3 拆成三次提交（先纯重构、再加并发、最后取消收尾）。本次一次完成：
  三段式拆分与并发调度共用同一批 `PreparedToolCall` 结构，分三次提交会产生两轮中间态
  改动与回归成本；改为一次性提交并用 `toolcall_parallel` + `agent`/`message_supplement`/
  `cancel` 三组回归覆盖旧行为（顺序执行路径、中断、取消收尾）。
- 并发只在**同一条 assistant 消息声明的调用**之间发生；跨轮次、跨会话不引入新并发。
- 独占工具不再"等待前一个独占工具完成才准备下一个"：prepare 阶段仍然全部串行（权限询问与
  HIL 顺序稳定），只是执行阶段按批次推进。
- 内置工具目前只有插件工具声明了并行安全；`XXToolBase` 派生类可用构造参数声明。
- `PreparedToolCall` 只持有 `id`/`name` 两个自有字符串，不持有模型声明的指针：取消后派生的
  子协程可能比本轮协程的局部变量活得更久，持指针会造成悬垂（首次实现用了指针，复查时改为拷贝）。

## 阶段 F：执行环境加固、记忆体积告警、UI 兼容与渲染边界（TOOL-17、PRM-4、UI-3、UI-4，2026-10-05）

已完成：

- **TOOL-17 执行环境加固**（`plugins/agentxx_execute_command/execute_command_impl.h`）：
  子进程固定下发 `NO_COLOR=1`、`TERM=dumb`、`PAGER=cat`、`GIT_PAGER=cat`；POSIX 侧
  `LC_ALL` 取"当前环境已是 UTF-8 的取值，否则 `C.UTF-8`"，Windows 侧**不动 locale**。
  两条执行路径都覆盖：boost.process 路径经新的 `detail::applyExecEnvPolicy()` 改写子进程
  环境表；popen 回退路径无法指定环境，改用 `detail::execEnvCommandPrefix()` 在命令字符串
  前加赋值语句（POSIX `export ...;` / Windows `set ...&&`）。插件 start 时经
  `XX_LOGI` 记录本次实际生效的策略（含 locale 实际取值），便于排查"工具里跑命令与手敲不同"。
- **PRM-4 记忆文件过大警告**（`middlewares/memory_file.{h,cpp}`）：新增阈值
  `MemoryFileMiddlewareHandle::kOversizeWarnChars = 8000`（UTF-8 字符数）。超过阈值的记忆
  文件在加载时记 `XX_LOGW` 并在加载日志里带 ⚠️ 行，提示"保持简略/索引式，详细内容按需读取"。
  **只告警**：内容照原样注入、不截断、不改变注入方式（计划核定的限定范围）。
  新增只读查询 `oversizeMemoryFiles()` 供测试/诊断。
- **UI-3 未知组件宽容降级测试**（`test/client/test_tui_ui_items.cpp`）：补三类用例 ——
  ① 已知组件带未知字段（忽略未知字段，其余照常渲染）；② 高版本才有的组件（走 fallback，
  未知字段一并忽略；混在已知组件中只降级自己）；③ 已知字段取未知枚举值（退回默认取值，不崩）。
- **UI-4 渲染层边界测试**：
  - 运行时：`tui_ui_items` 新增"空注册表渲染"用例（`UiRenderCtx::registry = nullptr` 时
    文本/表格等内置组件照常渲染，缺渲染器的自定义组件降级为 fallback）；
  - 静态：`boundaries` 新增规则 7「渲染层边界」—— `ui_components.{h,cpp}` 与
    `framework/ui_hit.h` 的直接 `#include` 不得出现端点/会话/传输/插件管理器实现/网络头
    （`agent_tui.h`、`components/`、`client_plugin_manager.h`、`agentxx/agent/io/`、
    `agentxx/agent/session*`、`agent_host.h`、`http_client.h`、`ws_client.h`）；
    文件缺失即判失败，避免改名后规则静默失效。

验证：

- 构建：先 `agentxx_lib_repo-build` 的 `INSTALL` 目标（刷新 `agentxx-project-install/include`），
  再构建 `agentxx_test` → 两者 exit=0，无新增 error/warning。
- 测试：`boundaries` 8/0（+1 渲染层规则）、`config_loader` 375/0、`tui_ui_items` 203/0
  （+14 兼容与空注册表断言）、`plugin_resources` 88/0（+5 记忆体积断言）。

注意事项：

- **测试构建使用的是安装后的头文件**（`{build}/agentxx-project-install/include`）+
  客户端源码头（`agent/client/include`）。因此修改 `agent/lib/include/**` 后，必须先构建
  `agentxx_lib_repo-build` 的 `INSTALL` 目标，测试构建才能看到新头文件（否则报"不是成员"）。
  单文件验证的完整流程：
  1. `cmake --build <build>/agentxx_lib_repo-prefix/src/agentxx_lib_repo-build --config Debug --target INSTALL`
  2. `cmake --build <build>/agentxx_test_repo-prefix/src/agentxx_test_repo-build --config Debug --target agentxx_test --parallel 8`
  3. 把 `.../agentxx_test_repo-build/Debug/agentxx_test.exe` 复制到 `<build>/exec/`（ASan 运行库在那里）
- popen 回退路径的环境前缀不改变命令语义，但会在命令文本前加一段赋值语句；该路径仅用于
  关闭 `AGENTXX_ENABLE_BOOST_PROCESS` 的构建（默认可执行文件走 boost.process 路径）。

## 阶段 E：安全责任文档、实施状态清单、配置边界（SEC-9、TST-13、CFG-8，2026-10-05）

已完成：

- **SEC-9 安全责任与边界文档**（新增 `docs/zh-cn/design/security.md`）：按计划 §8.0 的三条原则
  重写并明确 —— ① 权限系统约束的是"模型经工具发起的运行期动作"，不是进程隔离；
  ② 插件与宿主同权、加载即信任，权限声明只是"哪些参数是受约束目标"的约定，未声明即放行；
  ③ 门禁判定不是隔离，真隔离只能靠沙箱（SEC-12 列未来计划）。文档另含：判定顺序、
  已知覆盖边界表（符号链接未解析 / 命令执行无路径目标 / 无出网策略 / 设备采集 / 附件不抓 URL /
  插件无限制）、工具权限声明现状（filesystem 读/写/编辑已声明，命令/搜索/设备类未声明）、
  配置项语义表和部署建议。从 `index.md` 头部"相关文档"进入。
- **TST-13 实施状态清单**（新增 `docs/zh-cn/design/roadmap.md`）：按架构/轮次/上下文/持久化/工具/
  LLM/压缩/权限/UI/协议/插件/配置/检索/测试 14 个分组，逐条记录"已实施 / 部分实施 / 待实施 /
  不做 / 未来计划"、代码位置与验收模块；作为后续唯一的状态清单（取代散落 TODO）。
- **CFG-8 配置与设置边界**（新增 `docs/zh-cn/design/configuration.md`）：
  - 明确"可分发配置 → YAML / 本机偏好 → settings_db / 运行期安全状态 → 内存"三分归属与判定规则；
  - 明确 `language`（YAML，会话与模型提示词语言）与 `tui.lang`（settings_db，界面显示语言）
    的分工与优先级：YAML 显式配置 > 客户端 hello 携带的界面语言 > 默认 `en`；
  - **代码接线**（消除同名歧义）：
    - `AgentConfig` 新增 `languageExplicit`（yaml / FFI 配置 JSON 显式给出 `language` 时置位）；
    - `YamlAppConfig` 新增 `language`，`config_loader.cpp` 解析（支持 `${VAR}` 展开与
      大小写归一化，`auto`/空串按未配置处理）；`main.cpp` 的 `applySharedRuntimeConfig`
      写入 AgentConfig 并置位 `languageExplicit`；
    - `AgentContext::isLanguageExplicit()` / `BaseAgent::isLanguageExplicit()` 新增；
      `SessionServerAgentIO::handleHello` 在配置显式指定语言时忽略客户端界面语言（记 Debug 日志）；
    - FFI 配置 JSON 显式给出 `language` 时同样视为配置指定（`set_language` 仍可在运行期改写）。
  - `index.md` 示例配置补 `language` 键说明（注释形式）。

验证：

- 构建：`cmake --build agent/build/windows-debug/agentxx_test_repo-prefix/src/agentxx_test_repo-build
  --config Debug --target agentxx_test --parallel 8` → exit=0，无新增 error/warning。
- 测试：`config_loader` 375/0（新增 3 个用例、7 项断言：未配置保持空、解析与归一化
  （`ZH-CN`→`zh-cn`、`auto`→`en`、空串视为未配置）、`${VAR}` 展开）；
  `boundaries`/`agent`/`session_schema` 合计 272/0 回归通过。

注意事项：

- 构建产物位置：`agentxx_test` 的**最新** exe 在
  `{build}/agentxx_test_repo-prefix/src/agentxx_test_repo-build/Debug/`，
  `{build}/exec/agentxx_test.exe` 是 install 步骤复制过去的副本（可能滞后）；验证前先确认时间戳，
  需要时把 Debug 目录的新 exe 复制到 `exec/`（`exec/` 内有 ASan 运行库，直接在 Debug 目录运行会缺 DLL）。
- 界面显示语言仍完全独立（`tui.lang`），只影响 TUI 文案；会话语言由 YAML 或客户端 hello 决定。

## 验证结果（阶段 A/B/C）

构建：`agent/script/windows_debug_build.bat`（Debug，MSVC，ASan+UBSan 插桩）通过，无 error/warning 新增。

测试（`build/windows-debug/exec/agentxx_test.exe <模块>`，全部 0 失败）：

| 模块 | 结果 | 覆盖点 |
|---|---|---|
| `boundaries` | 7 / 0 | client 私有头、插件 SDK 白名单、lib 反向依赖、导出白名单配置、扫描量下限 |
| `writer_lease` | 25 / 0 | 进程内可重入、原生独占复核、释放回收、多目录独立、目录自动创建、失败返回原因 |
| `session_schema` | 53 / 0 | 新库建表 + 版本、老库迁移（补列/补表/回填/备份）、幂等、高版本拒绝读写、账本聚合与最近记录、外部持锁时写失败且不落数据 |
| `usage_ledger` | 21 / 0 | 真实轮次经 modelcall 记账（流式用量 137/29/166）、失败轮次留痕、会话间互不影响 |
| `settings_db` | 73 / 0 | 版本自增、按期望版本提交、冲突不改内容、双句柄竞争、重启延续、老版本列迁移 |
| `session_persistence` | 621 / 0 | 回归：会话库读写、重启恢复（受本次存储改动影响） |
| `agent` | 192 / 0 | 回归：装配清单重构后的启动/多轮/工具/中断路径 |
| `summarization` | 445 / 0 | 回归：压缩链路 |
| `plugins` / `plugin_resources` / `plugin_multi_instance` / `client_plugins` | 541 / 83 / 80 / 657 全 0 失败 | 回归：插件加载/资源/多实例（受装配清单重构影响） |

顺带修好的测试夹具问题：本地 LLM 模拟器（`test/core/test_agent.cpp`）的 SSE 流此前没有返回
`usage`，而 provider 默认请求 `stream_options.include_usage`，导致流式请求拿不到用量统计；
现在结束前补一个 `choices: []` + `usage` 的 chunk（与真实 OpenAI 行为一致）。

## 待完成（后续阶段）

- PRM-5：技能优先级与同名裁决（会话/项目 > 用户 > 插件/内置，同名取最高优先级并显示来源）。
- TOOL-16：按规范化路径排队执行（与 TOOL-1 并行化配套）。
- 批次 A 余项：CFG-1（结构化配置校验）、UI-2 + TST-5（UI 快照夹具）、PRO-4（schema 生成）、
  TST-8（一键门禁）、LLM-13（HTTP 录制回放）。
- 批次 E：PLG-1/2/4/6/7/8、CFG-9、UI-1/5/9、PRO-3/5/8、RET-1a/STO-12 界面入口、STO-13。
- CTX-7（附件引用）计划本身标注"需进一步理解具体实施内容"，暂缓。
- STO-12 的协议/界面入口（改名、搜索框）见"阶段 C"。

## 与计划的差异（记录用）

- ARC-1：计划写"`check_boundaries.py` **或**测试模块"，这里选测试模块（`boundaries`），
  规则只保留一份实现并进入 `agentxx_test` 门禁；二进制导出白名单仍由现有
  `agent/script/check_plugin_exports.sh` 负责。
- STO-1：租约持有期是"写连接的存活期"（连接 LRU 淘汰/对象析构时释放），并在**进程内可重入**
  （引用计数）——同一进程的多个实例/测试不会互相冲突，跨进程才互斥。读路径（`loadSession`、
  标题读取、检索）不取写租约，只读连接直接读文件。
- STO-2：迁移只在**写路径**首次打开库时执行（读操作不改磁盘结构）；迁移前备份文件名为
  `session.db.bak.v{旧版本}`，备份失败只告警不阻断。
- STO-8：账本字段包含 prompt/completion/total/cached/reasoning 与失败原因，暂不含 cost 与
  attempt 分类（模型价格元数据 LLM-1 未落地）；`ModelCallWrapNode` 的记账失败只记日志，
  不影响对话流程。
- STO-11：`set()` 保持"无条件写入"语义并递增版本，需要读-改-写的调用方用
  `getVersioned` + `setVersioned`（冲突返回 `Conflict` 且不改内容）。
- STO-12：只做存储层与检索（不建 FTS5、无协议/界面入口）。
- ARC-3：装配步骤的回滚动作只做"释放本步骤创建的对象"（模型注册表/事件总线/中间件上下文/
  插件管理器/图注册表/引擎/工具列表）；插件卸载走框架的 `shutdownAll()`。启动断言失败抛
  `std::runtime_error`（带失败原因），由调用方决定如何上报。

## 阶段 D：压缩摘要提示词固定小节 + 配置字段拼写修正（2026-10-05）

- 计划原文：[plan.md](plan.md) 修订记录「第六批：§7 压缩与预算」。

已完成：

- **CMP-3 提示词微调**（计划核定"进行提示词微调"）：`agent/lib/src/agent/prompt.cpp` 的默认
  压缩提示词（`appendSystemPrompts["summarization"]`）由"编号 MUST keep / MAY discard 列表"
  改为固定小节：`## Goal` / `## Done` / `## In progress` / `## Blocked` / `## Key facts` / `## Next`，
  末尾保留 `Rules:`（合并旧摘要 + 可丢弃项）与 `{omitted_note}` / `{max_words}` 占位符。
  语义未变（保留要点集合一致），只是把"必须保留"的信息组织成固定小节，便于模型续跑与人工阅读。
  约束：`test_summarization` 断言默认模板含 `Summarize` 与两个占位符，改写时保留。
- **配置字段拼写修正**：`ModelConfig::modelContenxtMaxToken` → `modelContextMaxToken`
  （yaml 键 `model_context_max_token` 与 FFI JSON 键 `modelContextMaxToken` 原本就是正确拼写，
  只有 C++ 字段名写错）。共 **16 个代码文件、63 处**（lib / client / test / benchmark）。
  `resource/history/` 下的历史比较文档按仓库约定**未改动**，其中仍保留旧字段名。

验证：

- 构建：`cmake --build <build>/agentxx_test_repo-prefix/src/agentxx_test_repo-build --config Debug
  --target agentxx_test --parallel 8` → exit=0，仅有原有的编译警告（C4834/C4100/C4456/C4702），
  与本次改动无关。
- 测试（`agentxx_test`，Debug + ASan/UBSan）：`boundaries` 7/0、`writer_lease` 25/0、
  `toolcall_args` 173/0、`agent` 192/0、`util_misc` 230/0、`session_schema` 73/0、
  `config_loader` 368/0、`tui_settings` 552/0、`summarization` 445/0、`usage_ledger` 21/0、
  `memgrowth` 15/0 —— 合计 **2101 项断言全通过**。

注意事项 / 与计划的差异：

- CMP-1（`ContextBudget` 单一口径）、CMP-4（原文可回取）、CMP-9（压缩前写记忆提醒）经人工核定
  **不做**，故未引入预算结构、压缩内容外置与记忆提醒；`summarization` 的阈值常量保持原处。
- 提示词改用固定小节后，摘要文本比原来多几个小节标题行（约 40 token），对长会话成本影响可忽略。
- 构建脚本 `windows_debug_build.bat` 整体执行时间较长（superbuild），验证时直接构建
  `agentxx_test` 目标；`agentxx_cli` 已由脚本先行构建完成。

## 修订记录

- 阶段 A/B 完成后提交：`添加目录级规则文件 (计划 ARC-2)`、
  `实现计划 1-5 章可行项: 架构边界检查、装配清单、会话写租约、schema 迁移链、用量账本、
  设置乐观版本 (ARC-1/ARC-3/STO-1/STO-2/STO-8/STO-11)`。
- 阶段 C（STO-12 存储层）完成后提交：见 git 记录 `实现会话标题与检索 (计划 STO-12)`。
- 阶段 E（SEC-9 / TST-13 / CFG-8）完成后提交：`新增安全责任、配置边界与实施状态文档, 会话语言与界面语言解耦 (SEC-9/TST-13/CFG-8)`。
- 阶段 F（TOOL-17 / PRM-4 / UI-3 / UI-4）完成后提交：`命令执行环境加固、记忆文件体积告警、UI 兼容与渲染边界测试 (TOOL-17/PRM-4/UI-3/UI-4)`。
- 阶段 G（TOOL-1 / TOOL-2 / TOOL-3）完成后提交：`工具调用三段式执行与受限并行 (TOOL-1/TOOL-2/TOOL-3)`。
- 阶段 H（PRO-1 / PRO-7 / PRO-11）完成后提交：`wire 协议往返测试、会话 ID 统一校验、wire 错误码 (PRO-1/PRO-7/PRO-11)`。
- 阶段 I（SEC-2 / SEC-5 / TST-10）完成后提交：`权限判定理由与执行前目标复验 (SEC-2/SEC-5/TST-10)`。
- 阶段 J（LLM-2 / LLM-3）完成后提交：`LLM 错误分类、退避策略与溢出压缩重试 (LLM-2/LLM-3)`。
- 阶段 K（LOOP-1 / LOOP-2 / LOOP-3 / LOOP-4 / LOOP-11）完成后提交：`输入投递: 持久化收件箱、投递模式与队列状态机 (LOOP-1/2/3/4/11)`。
- 阶段 L（STO-4 / STO-5 / STO-9）完成后提交：`展示历史序号与增量补拉、落盘分级、写失败降级提示 (STO-4/STO-5/STO-9)`。
- 阶段 M（PRM-1 / PRM-2 / PRM-7）完成后提交：`提示词稳定段与动态段分离、段落排序、请求体结构断言 (PRM-1/PRM-2/PRM-7)`。

## 阶段 M：提示词稳定段与动态段（PRM-1/PRM-2/PRM-7，2026-10-06）

计划依据：`plan.md` §3（会话上下文、提示词、技能和记忆）、§16.2 批次 C。

已完成：

- **PRM-2 段落排序号**（限定：只加 `order`，不引入固定槽位枚举）：
  - `AgentPrompt` 新增 `appendSystemPromptMeta`（键 → `{order, source}`）与
    `setAppendSection(key, text, order, source)` / `removeAppendSection` /
    `appendSectionKeys` / `orderedAppendSections`；
  - 拼接顺序改为 `order` 升序 + 同 order 按键名字典序（**去掉**原先硬编码的
    planning → skill → codegraph → 其余 的顺序）；`appendSystemPrompts` 仍是
    `map<string,string>`，旧调用方（插件 iface / 训练 / 测试）零改动；
  - 序列化：段落写成 `{text, order, source}` 对象，读取时兼容旧的纯字符串形态与
    `null`(删除)；`promptHash()` 计入 order（且"有无元数据"不影响哈希，保证
    JSON 往返后哈希一致 —— 训练模块的往返断言依赖此性质）。
- **PRM-1 稳定段 / 动态段分离**：
  - `AgentContext::buildSystemPromptStable()`：只含 `systemPrompt` + 静态附加段
    （按 order 排序），即进 system 消息的部分；
  - `AgentContext::buildDynamicContextSections()`：中间件每轮产出的片段
    （记忆文件内容 / 技能清单），**按来源键写入 `graphDataKey_appendSystemMessage`**
    （键排序 map，同来源覆盖）。这同时修掉一个既有缺陷：旧实现用
    `vector<string>` 追加，`onAgentcallStartFunc` 每轮 push 一次且无人清理，
    系统提示词会逐轮累积重复片段（长会话上下文与成本持续膨胀）；
  - 请求装配（`ModelCallProcessNode`/`build_params`）：动态段作为**请求末尾的独立
    user 消息**附加，用 `<dynamic_context source="...">` 包裹并标 `AutoInserted`；
    不写会话上下文（权威 transcript 只存稳定段）。选 user 角色而非 system 的原因：
    openai-responses 与 anthropic provider 会把 system 消息归并到系统字段（请求最前），
    那样动态内容会进入前缀、破坏前缀缓存；
  - 执行流：`baseRun` 写入 system 消息时改用稳定段；
    `buildSystemPrompt()`（完整段，供 UI/上下文查看）保持不变。
- **PRM-7 请求体结构 + 稳定段哈希断言**：
  - `Session::noteStablePrefixHash(hash)` / `stablePrefixHash()` / `stablePrefixChanges()`：
    请求装配时对 "system 消息 + 工具 schema" 取指纹，变化即记 Info 日志并计数
    （排查"provider 前缀缓存命中率下降"）；
  - 测试断言请求体结构（system 只含稳定段、动态段在末尾带来源、工具 schema 非空、
    记忆内容整条请求只出现一次）与连续两轮稳定段字节一致 + 变化次数为 0。

测试：

- 新模块 `prompt_stability`（19 项断言，同步）：段落排序（含同 order 键名裁决、
  空段落不参与）、键列表、JSON 往返（新对象形态 + 旧字符串形态 + null 删除）、
  order 影响哈希。
- 新模块 `prompt_stability_io`（18 项断言，异步）：真实一轮 + 第二轮请求体断言
  ——system 不含记忆内容、末尾是 `<dynamic_context source="memory">` 消息、
  记忆标记全请求只出现 2 次（来源包裹内的正文）、工具 schema 随请求下发、
  第二轮 system 字节一致、稳定段变化次数 0、会话上下文首条 system 也不含记忆内容。
- 回归同步调整：`test_summarization`（段落 JSON 读 `text` 字段）、
  `test_plugin_resources`（动态段按来源 map 读取）、`training.cpp` 的补丁提取
  兼容两种段落形态。

验证：

- 构建：lib `INSTALL`、`agentxx_test`、`agentxx_cli`（含 TUI）均 exit=0，无新增 error/warning。
- 测试：`prompt_stability` 19/0、`prompt_stability_io` 18/0、`agent` 198/0、
  `summarization` 445/0、`training` 97/0、`plugin_resources` 89/0、`plugin_runtime` 672/0、
  `plugins` 541/0、`remote_agent` 453/0、`session_sync` 30/0、`input_delivery` 78/0、
  `persist_semantics` 25/0、`boundaries` 8/0。

注意事项 / 与计划的差异：

- 计划 PRM-1 提到"provider 不支持该形态时才整体重建并记录缓存失效"：本项目三家 provider
  都接受末尾的 user 消息，因此不做 provider 能力探测（LLM-1 已核定不补能力元数据元数据），
  统一按"稳定段 + 末尾动态消息"装配；稳定段变化经 `stablePrefixChanges` 计数 + 日志暴露。
- 动态段的 `source` 目前由中间件固定写入（`memory` / `skills`）；插件若需要贡献动态段，
  仍应经 `appendSystemPrompts`（静态段）或后续扩展该 map（未在本轮开放插件接口）。
- PRM-5（技能优先级与同名裁决）本轮未实施，仍列为待完成项。

## 阶段 L：持久化语义与断线增量补拉（STO-4/STO-5/STO-9，2026-10-06）

计划依据：`plan.md` §4（持久化、事务和崩溃恢复）、§16.2 批次 B 的 STO 项。

已完成：

- **STO-5 落盘分级**（`Session::persistNow` / `Session::persistThrottled`）：
  - `persistNow(reason)`：上下文 + 展示历史待落盘队列一起立即写；
    `persistThrottled(reason)`：上下文按既有节流窗口（首次/超窗口立即写），展示历史只补刷窗口外的积压；
    两者都记 Debug/Trace 日志（含原因），内存模式（无持久化回调）为 no-op、不产日志噪音。
  - 调用点：**用户输入**（`BaseAgent::runTurnAsync` 写入 user 消息后, reason `user-input`）、
    **工具结算**（EventBridge 的 channel 写入批次含 tool 角色时, reason `tool-settle`；纯模型输出仍按节流
    `llm-output`）、**压缩完成**（`SummarizationMiddlewareHandle` 轮内压缩 `compaction` 与手动压缩
    `manual-compaction`）、**轮次终态**（`turn-end`）、恢复前落盘（`before-resume`）。
- **STO-9 持久化降级可见**（限定范围）：
  - `SessionStore` 在消息写路径失败时记录根因到 `lastWriteError()`（只记根因、不带操作名前缀，
    使同一故障下"追加消息"与"保存上下文"两条路径的原因一致, 便于去重）；
  - 会话持久化回调（`makeSessionStoreHooks`）每次写入后检查写失败, 首次失败推一条
    `WireDelta::MessageUITip`（Warning, 文案 `Persistence degraded: ...`）, 同一原因只提示一次,
    恢复后不重复提示; 提示走增量通道而不改写会话历史（避免与持久化回调递归）。
- **STO-4 展示历史序号与增量补拉**（限定范围：只做序号 + `hello.afterViewSeq`）：
  - 展示消息序号改为**会话分配**并显式落库：`Session::lastViewSeq()` 单调递增，
    `view_message.seq` 显式写入（老调用方 seq=0 时仍走库内自增），meta 增加 `viewSeqCounter`
    （老库无记录时按 `MAX(seq)` 兜底）；`SessionStore::LoadedSession.lastViewSeq` 供恢复续编号；
  - `SessionStore::loadViewMessagesAfter(sessionId, afterSeq, limit)`：只读连接按序号取差量
    （含解析失败跳过单行、高版本库拒绝、limit 截断）；
  - `WireHello.afterViewSeq` + `WireSyncPayload{lastViewSeq, incremental}`：
    服务端握手同步策略按代价排序 —— ① delta 重放（客户端 lastSeq 仍在缓冲内，传输最小）
    → ② 增量补拉（序号落在服务端历史范围内且差量 ≤ `kIncrementalReplayMaxMessages`=512）
    → ③ 全量/尾窗同步兜底（首次接入、序号超前、内存模式、差量过大）；
    delta 缓冲溢出（原实现无条件全量）现在也优先走增量补拉；老数据（无 msg id）无法去重时回退全量；
  - 客户端：WS 传输在收到 Sync 时记录 `lastViewSeq` 并在重连 hello 中回传（切会话时复位），
    TUI 收到 `incremental` 载荷时按 `msg.id` 去重后追加到本地历史尾部（不重置窗口与其它界面状态）。

测试：

- 新模块 `persist_semantics`（25 项断言）：落盘分级（首条立即写、窗口内合并、`persistNow` 不受窗口限制、
  内存模式 no-op）、序号恢复与增量查询联调、写失败提示一次（外部持锁模拟，`ForeignWriterLock`）+
  同原因不重复 + 恢复后不提示 + 失败期间的消息确实未落盘。
- 新模块 `session_sync`（30 项断言）：增量补拉（序号命中只补差量 / 已最新回空增量 / 序号超前回退常规同步 /
  未提供序号保持旧行为 / delta 有效时优先 delta 重放不发快照）、内存模式无持久化时回退常规同步、
  重启后（会话重建）序号继续且只回补新消息。
- `session_schema` 增补序号与增量查询用例（显式序号写入、`lastViewSeq` 恢复、`loadViewMessagesAfter`
  的区间/limit/更新不改序号/老库兜底/不存在会话不建目录），模块 91/0；
  `session_persistence` 同步 `SessionStoreHooks` 新签名（621/0）。

验证：

- 构建：lib `INSTALL`、`agentxx_test`、`agentxx_cli`（含 TUI）均 exit=0，无新增 error/warning。
- 测试：`persist_semantics` 25/0、`session_sync` 30/0、`session_schema` 91/0、`session_persistence` 621/0、
  `input_delivery` 78/0、`wire_roundtrip` 191/0、`remote_agent` 453/0、`agent` 198/0、
  `summarization` 445/0、`toolcall_parallel` 48/0、`cancel` 45/0、`message_supplement` 95/0、
  `usage_ledger` 21/0、`settings_db` 73/0、`permission` 45/0、`llm_error` 65/0、`boundaries` 8/0。

注意事项 / 与计划的差异：

- 计划 STO-4 写"`view_message` 加 seq 列"：库内 `seq` 列（AUTOINCREMENT 主键）原本就存在，
  本次把它改为**会话分配并显式写入**，并补 `meta.viewSeqCounter` 与 `hello.afterViewSeq` 补拉路径；
  不新增 `event` 表、不做类型化事件、不建投影器（计划的限定范围）。
- 增量补拉的安全性依赖"消息 id 稳定且 append-only"：客户端按 `msg.id` 去重，服务端遇到无 id 的老数据
  直接回退全量同步；已被就地更新的历史消息（tool 结果回填）不改序号，客户端若持有旧内容则不会收到补发
  （与 delta 重放路径同一限制，属已知取舍）。
- STO-9 只做"首次写失败提示一次"，不做 Info 侧边栏/状态栏的降级标记（计划的限定范围）；
  写失败提示是增量提示消息，不写会话历史（避免与持久化回调递归）。

## 阶段 K：输入投递（LOOP-1/2/3/4/11，2026-10-06）

计划依据：`plan.md` §2（轮次与输入投递）、§16.2 批次 B。

已完成：

- **LOOP-4 队列状态机**（`session_server_agent_io.{h,cpp}`）：
  - 用 `SessionQueueState`（`idle` / `running` / `paused` / `draining`）取代原来的
    `queuePaused_` / `pendingInsert_` 两个 bool；所有转移集中到 `setQueueState()` 一处，
    并打印 Debug 日志（旧状态 → 新状态 + 原因），便于排查"消息为什么不执行"。
  - 转移规则：轮次正常结束 → 队列非空为 `running`、空为 `idle`；轮次取消/异常/中断结束 →
    `paused`（不自动继续）；`paused` 下收到新用户输入 → `draining`（消化积压）→ `idle`；
    "打断并运行下一条"→ `draining` 并取消当前轮次。
  - 状态随 `WireMessageQueueUpdate.state` 与 `WireSyncPayload.queueState` 同步给客户端
    （未知文本按 `idle` 处理，老客户端忽略该字段）。
- **LOOP-3 投递结果显式化**：
  - 新消息 `WireInputAck`（`requestId/sessionId/delivery/status/reason/detail/itemId`）+
    常量表 `InputStatus`（started/queued/steered/rejected）与 `InputRejectReason`
    （empty_content/session_not_found/session_mismatch/server_stopped/bad_delivery/queue_cleared）；
  - `WireUserInput` 增加 `requestId`（0 = 不要回执）：**只有 requestId > 0 才回执**，
    避免老客户端收到不认识的类型（WS 解码遇未知类型会断开连接）；
  - TUI 每次发送输入自增 `requestId`，收到 `rejected` 回执时弹提示
    （`toast.inputRejected`，中英文案已补）。
- **LOOP-2 投递模式**（`WireUserInput.delivery`）：
  - `next-turn`（默认，空值归一化到它）：进入消息队列（旧行为）；
  - `next-step`：轮次进行中登记为"待注入"，在**下一个 modelcall 请求装配前**（安全边界）
    作为 user 消息写入会话权威上下文 + 展示历史（EventBridge 不展开 user 角色消息，
    因此由 `appendUserViewMessage` 自行追加展示消息并广播 `InsertMessage`）；
    空闲时按 `next-turn` 立即开轮；
  - `inject`：只在下一次请求里追加（请求级可见），**不改写权威上下文、不唤醒会话**；
    来源写入消息 `extra.input_source`（非 user 来源在正文前加 `[来源]` 前缀）；
  - 待注入输入存放在 `Session::pendingInputs_`（`enqueuePendingInput` / `takePendingInputs`），
    取用入口是 `agentxx::nodes::drainPendingSessionInputs()`（`session_context.{h,cpp}`），
    由 `ModelCallWrapNode::callLLM` 在 `build_params` 之前调用；
  - 带附件的 `next-step`/`inject` 回落为 `next-turn` 排队（附件只在本轮启动路径加载），
    回执 detail 明确说明。
- **LOOP-11 `collect` 合并投递**：`SessionServerAgentIO::Config::collectWindow`（默认 400ms）
  静默窗口内的连续输入合并为一条（正文按输入顺序换行拼接、附件累加、模型取最后一个非空），
  窗口到期后按 `next-turn` 提交；每个请求都拿到带同一合并条目 id 的回执；
  清空队列会丢弃窗口内暂存输入并逐条回 `queue_cleared`（避免客户端一直等回执）。
- **LOOP-1 持久化收件箱**：
  - 会话库新增 `session_input` 表（schema **v2**，迁移步骤 2：老库补表，`IF NOT EXISTS` 幂等）；
    字段 `id / payload / delivery / status / admitted_seq / promoted_seq / created_ms`；
  - `SessionStore` 新增 `addSessionInput` / `markSessionInputPromoted` / `setSessionInputStatus`
    / `listSessionInputs`（状态常量见 `SessionStore::SessionInputStatus`）；
  - 受理即落库（`admitted`），真正进入上下文或本次请求时标记 `promoted`
    （`drainPendingSessionInputs` 内完成），用户删除或清空队列标记 `dropped`；
    载荷只存元数据（文本/模型/投递模式/附件元数据），附件 Base64 不进库；
  - 端点启动（`run()` 预热会话之后）`recoverPendingInputs()`：只恢复 `admitted` 条目，
    作为 `recovered=true` 的排队项进入队列并**置 `paused`**——用户确认（发新输入）或删除后才执行，
    进程重启不重放副作用；日志给出恢复条数。

测试：

- 新模块 `input_delivery`（`agent/test/core/test_input_delivery.cpp`，78 项断言，7 组用例）：
  ① 回执状态与拒绝原因（空内容 / 会话不匹配 / 未知投递模式 / started / queued / 旧客户端不回执）；
  ② 状态机（初始 idle → 排队 queued → 取消 paused + 队列同步状态 → 新输入解除暂停并消化积压 → idle）；
  ③ next-step 注入（回执 steered、内容进入权威上下文与展示历史）；
  ④ inject（空闲不唤醒会话、只出现在下一次请求体、不进上下文与展示历史）；
  ⑤ collect 合并（三条输入合成一轮，请求体含全部文本）；
  ⑥ 收件箱存储 API（写入 / 按状态过滤 / promoted / dropped / 同 id 覆盖 + schema v2 迁移）；
  ⑦ 重启恢复（未投递条目恢复为待确认且不自动执行；删除/清空的条目不恢复）。
- `wire_roundtrip` 模块补 26 项断言：`WireInputAck` 全字段往返、`WireUserInput` 的
  `delivery`/`requestId` 往返与"老客户端缺字段"解析、队列状态文本往返与未知文本兜底、
  `MessageQueueItem` 新字段往返。

验证：

- 构建：lib `INSTALL`、`agentxx_test`、`agentxx_cli`（含 TUI）均 exit=0，无新增 error/warning。
- 测试：`input_delivery` 78/0、`wire_roundtrip` 191/0、`remote_agent` 453/0、`agent` 198/0、
  `session_persistence` 621/0、`session_schema` 73/0、`settings_db` 73/0、`permission` 45/0、
  `boundaries` 8/0、`cancel` 45/0、`message_supplement` 95/0 —— 全部 0 失败。

注意事项 / 与计划的差异：

- 计划 LOOP-1 字段写成 `admitted_seq/promoted_seq`，实现一致（记录受理/投递时的会话 delta seq）；
  恢复语义按计划"只恢复为待确认，不自动重放副作用"：恢复项进入消息队列并置 `paused`，
  界面上表现为可查看、可删除、需用户确认的积压消息。
- 计划 LOOP-2 要求"持久化关闭时保持相同语义，但只保存在内存"：收件箱写入在无
  `SessionStore`（未配置 dataDir 或 `enableSessionStore=false`）时为 no-op，其余语义完全一致。
- LOOP-5/LOOP-7 经人工核定不做；LOOP-6/LOOP-10 亦不做（理由见 plan.md 修订记录）。
- 队列状态已随消息同步，但 TUI 侧暂只用"进行中/排队"既有展示，未单独渲染
  `paused`/`recovered` 文案（接口与字段已就绪，后续按需要补展示）。

## 未实施项的原因与建议路径（下次继续）

### TOOL-1 / TOOL-3（工具并行与取消收尾，P0）

现状：`ToolcallWrapNode::baseRun` 对同一条 assistant 消息里的 tool_calls 逐个 `co_await`；
`execTool` 把"参数修正 → 权限检查（可能询问用户）→ 连续重复调用确认（HIL 中断）→ 执行 →
摘要"串在一起，取消/中断通过异常与控制流占位消息收尾（`insertAbortedToolResults`、
`completedToolcallIds`、`interruptToolcallCache`）。

要做的事（建议顺序）：

1. 先把 `execTool` 拆成三段：`prepareToolCall`（参数修正 + 权限 + 重复确认，全部串行、保持
   源顺序）、`runToolBody`（纯执行，可并发）、`finalizeToolCall`（摘要/结果文本/日志）。
   这一段是纯重构，行为不变，可单独提交并用现有 `agent`/`toolcall_args` 测试回归。
2. 加上"只读 + 声明支持并行"的分类（`XXToolBase` 侧新增 `supportsParallel` 标记，
   默认独占），只对这批调用并发执行（`asio::experimental::awaitable_operators` 或
   `co_spawn` + 收集），写/交互工具仍然形成屏障顺序执行。
3. 结果一律按原始 `tool_call_id` 顺序写回（并发完成顺序 ≠ 提交顺序），取消时已完成结果保留、
   未启动的补取消占位、执行中的请求取消——现有 `insertAbortedToolResults` 逻辑需要改成
   "按 id 补齐"，测试要覆盖"完成顺序与提交顺序不同"的场景。

风险提示：`execTool` 的 HIL/取消语义很细（中断缓存、插件工具保活、`tool_call_id` 复用），
并行化必须在第 1 步重构完成后单独提交，避免和并发改动混在一起导致难以定位回归。

### LOOP-1 / LOOP-2 / LOOP-3 / LOOP-4 / LOOP-11（输入投递，P0/P1）

现状：`SessionServerAgentIO` 用内存 `deque` + `queuePaused_`/`pendingInsert_` 两个 bool
管理输入；`WireUserInput` 只有 text/model/attachments；重连靠 delta 环形缓冲，没有 durable inbox。

建议路径：先做 LOOP-4（把两个 bool 换成 `idle/running/paused/draining` 状态机 + 转移测试，
纯内部改动），再做 LOOP-3（`WireUserInput` 增加 `delivery`、新增投递回执消息），
最后做 LOOP-1（`session_input` 表 + 恢复时不自动重放副作用）。`next-step`（下一个安全
modelcall 边界注入）需要 modelcall 请求装配侧提供一个"待注入队列"入口，建议与 CTX/PRM
的动态段一起设计，避免二次改请求装配。
