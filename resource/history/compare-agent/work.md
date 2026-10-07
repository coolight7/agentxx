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
| PRM-5 | 技能优先级和同名裁决 | P1 | 完成（已构建 + 测试通过） | `middlewares/skill.{h,cpp}`（`SkillDirEntry` 优先级 + 同名裁决 + 来源展示） |
| CTX-7 | 附件引用而不是反复内联 Base64 | P1 | 待完成（计划标注"需进一步理解实施内容"，先不动） | — |
| STO-12 | 会话检索和标题 | P1 | 完成（含协议/TUI 入口，见阶段 W） | `session_store` 的 `sessionTitle`/`setSessionTitle`/`searchSessions`；`WireRenameSession` 等 |
| STO-12b / RET-1a | 会话检索与改名的协议 + TUI 入口 | P1 | 完成（已构建 + 测试通过） | `wire_protocol`（`keyword`/重命名消息/`snippet`）、`session_server_agent_io`（`listSessionsFor`/`handleRenameSession`）、TUI 会话弹窗检索行 + `Ctrl+R` 改名；模块 `session_admin`、`tui_surface` |
| PRO-4 | Wire 协议字段清单（生成物 + 新鲜度门禁） | P1 | 完成（已构建 + 测试通过） | `agent/schema/wire-schema.json` + `docs/zh-cn/design/wire-protocol-fields.md`；模块 `wire_schema` |
| PRO-5 | 连接阶段与错误分类 | P1 | 完成（已构建 + 测试通过） | `WireConnectionStage` + 传输 `stage()/setStage()` + 端点未握手拒绝 + `MessageNotFound`；模块 `input_delivery`、`remote_agent` |
| TST-2 | Wire 往返与 schema 一致性 | P0 | 完成（已构建 + 测试通过） | 模块 `wire_roundtrip` + `wire_schema`（实现一致性、覆盖度） |
| LLM-7 | 消费端退出取消 | P1 | 完成（已构建 + 测试通过） | `agentxx/nodes/provider_call_scope.h` + `nodes/modelcall.cpp`；模块 `provider_call_scope` |
| TST-9 | 测试隔离、耗时与脱敏 | P1 | 完成（已构建 + 测试通过） | `test.cpp`（每模块耗时 + 清凭据环境）、`test_framework.h`（`redactSecret`） |
| STO-13 | 会话导出和取证包 | P2 | 待完成 | — |
| TOOL-1 | 分阶段并行：prepare/dispatch/finalize | P0 | 完成（已构建 + 测试通过） | `nodes/toolcall.cpp`；模块 `toolcall_parallel` |
| TOOL-2 | 并发分类与上限 | P0 | 完成（已构建 + 测试通过） | `tools/tool.h`、`plugin_api.h`、`config.h` |
| TOOL-3 | 并行取消和收尾 | P0 | 完成（已构建 + 测试通过） | `nodes/toolcall.cpp`；模块 `toolcall_parallel` |
| TOOL-12 | 工具可用性诊断（并入 ARC-6） | P1 | 完成（已构建 + 测试通过） | `context.h`（`ToolAssemblyRecord`/`toolAssembly`）、`base_agent.cpp`、`assembly_snapshot.cpp` |
| ARC-6 | 生效装配快照 + `--dump-config`（限定范围） | P1 | 完成（已构建 + 测试通过） | `include/agentxx/agent/assembly_snapshot.h` + `src/agent/assembly_snapshot.cpp`、`client/main.cpp` |
| CFG-3 | 配置来源与诊断清单（并入 ARC-6） | P1 | 完成（已构建 + 测试通过） | 同上（配置侧快照） |
| CFG-1 | 结构化配置校验（限定范围） | P0 | 完成（已构建 + 测试通过） | `include/agentxx/agent/config_validation.h` + `src/agent/config_validation.cpp`、`config.cpp`、`client/main.cpp` |
| UI-2 | UI 快照夹具（含 TST-5） | P0 | 完成（已构建 + 测试通过） | `test/client/test_ui_snapshot.cpp` + `include/agentxx-test/client/ui_snapshot.h` + 基线 `test/snapshots/ui/`（模块 `ui_snapshot`） |
| TST-8 | 一键质量门禁 | P0 | 完成 | `agent/script/gate.sh`、`agent/script/gate.ps1` |
| TOOL-16 | 按规范化路径排队执行 | P1 | 完成（已构建 + 测试通过） | `plugins/agentxx_filesystem/filesystem_impl.h`（`PathLockTable`/`lockPathBlocking`/`lockPathAsync`）；模块 `filesystem` |
| ARC-5 | 分阶段关闭与后台任务收敛（不等待轮次） | P1 | 完成（已构建 + 测试通过） | `util/task_scope.{h,cpp}`、`BaseAgent::shutdownAsync`、`AgentContext::markShuttingDown`、`io/session_server_agent_io.cpp`；模块 `task_scope`、`shutdown_stages` |
| UI-9 | 能力与体验级别声明 | P2 | 完成（已构建 + 测试通过） | `client/include/agentxx-client/io/tui/tui_plugin_adapter.h`（`uiCapabilitiesJson` 补 form/layout/terminal 段）；模块 `ui_capabilities` |
| PLG-10 | 插件装载耗时与注册计数（限定范围） | P2 | 完成（已构建 + 测试通过） | `plugin_manager.h`（`PluginListView` 诊断字段）、`plugin_manager_lifecycle.cpp` |
| SEC-9 | 安全责任与边界文档 | P0 | 完成 | `docs/zh-cn/design/security.md` |
| TST-13 | 单一实施状态清单 | P1 | 完成 | `docs/zh-cn/design/roadmap.md` |
| CFG-8 | 配置与设置边界（含会话语言接线） | P1 | 完成（已构建 + 测试通过） | `docs/zh-cn/design/configuration.md`、`AgentConfig::languageExplicit` |
| TOOL-17 | 执行环境加固 | P0 | 完成（已构建 + 测试通过） | `plugins/agentxx_execute_command/execute_command_impl.h` |
| PRM-4 | 记忆文件过大警告 | P0 | 完成（已构建 + 测试通过） | `middlewares/memory_file.*` |
| UI-3 | 未知组件宽容降级（补测试） | P1 | 完成（测试通过） | `test_tui_ui_items.cpp` 未知字段/高版本组件 |
| UI-4 | 渲染层边界测试 | P1 | 完成（测试通过） | `test_tui_ui_items.cpp` 空注册表渲染 + `boundaries` 渲染层规则 |
| TST-10 | 安全负面测试（门禁正确性） | P0 | 完成（用例集收窄，见阶段 I） | 模块 `permission`（配置拒绝优先、工作区隔离优先、未声明权限放行、执行前目标复验）；软链接越界用例随 SEC-6 未来计划 |
| TST-3 | 持久化迁移/恢复测试（迁移中断） | P0 | 完成（已构建 + 测试通过） | 模块 `session_schema`（D2 段：迁移失败不推进版本、数据不丢、排除故障后续做） |
| TST-1 / LLM-5 | 假 provider 接缝 | P0 | 完成（已构建 + 测试通过） | `test/include/agentxx-test/core/fake_provider.h` + 模块 `fake_provider` |
| TST-4 | 并发与竞态清单 | P1 | 完成（已构建 + 测试通过） | 模块 `race_guards`（取消 vs 结算/中断应答/节流、乱序提交、执行中注销） |
| TST-6 | 存储一致性测试骨架 | P1 | 完成（已构建 + 测试通过） | 模块 `storage_consistency`（同一份键值语义跑 4 个后端） |
| TST-7 | 门禁扩展（UI 组件名 / 接口表名集合 / 文档路径） | P1 | 完成（部分，见阶段 Z） | `agent/test/core/test_boundaries.cpp`；插件注册清理随 PLG-1 |
| PLG-1 | 注册可逆与清理审计 | P1 | 完成（已构建 + 测试通过） | `plugin_manager.h`（`RegistrationInventory`）、`plugin_manager_adapters.cpp`；模块 `plugin_cleanup` |
| PLG-2 | 声明式贡献集合与重算 | P1 | 完成（提示词贡献模型 + 测试 + 文档，见阶段 AA） | `plugin_manager_vtable.cpp`（既有实现）+ 模块 `plugin_cleanup` + `plugins.md` §15.7 |
| PLG-4 | 独占能力 slot（执行图定义） | P1 | 完成（已构建 + 测试通过） | `plugin_manager.{h,cpp}`（占用/拒绝/释放回内置）；模块 `plugin_cleanup` |
| PLG-7 | 教学式错误与信任声明 | P1 | 完成（已构建 + 测试通过） | `PluginManager::diagnosePluginPath` + 装载入口 WARN + `plugins.md` §15.7 |
| UI-1 | 客户端模型层 | P1 | 完成（已构建 + 测试通过） | `client/.../io/tui/model/{history_window,queue_mirror}.{h,cpp}` + TUI 接线；模块 `tui_model` |
| UI-5 | 输入栏硬件光标 | P1 | 完成（已构建 + 测试通过） | `tuiHardwareCursorSupported()`（`ui_components.*`）+ 能力段 `terminal.hardware_cursor` |
| OBS-3 | 关键指标 | P2 | 完成（已构建 + 测试通过） | `include/agentxx/util/observability.h` + 打点（base_agent/modelcall/toolcall）；模块 `observability` |
| OBS-4 | 诊断包导出 | P2 | 完成（已构建 + 测试通过） | `include/agentxx/util/diagnostics.h`、CLI `--dump-diagnostics`（含 STO-13 的导出需求） |
| STO-13 | 会话导出与取证包 | P2 | 完成（并入 OBS-4：会话摘要段 + 可选消息正文） | `diagnostics.cpp` 的 session 段 |
| OBS-5 | 模块级日志开关 | P2 | 完成（已构建 + 全量回归通过） | `utilxx_base/log.h|cpp`（`LogEntry::module` / `logModuleOf` / `setModuleLevel` / `applyLogModuleLevelSpec`）+ 内核 `pluginxx/host/tables_impl.h`（`plugin.<名字>`）+ CLI `AGENTXX_LOG_MODULES`；模块 `log_modules` |
| PRO-8 | stdio JSONL 一次性运行（复用 Wire 结构 + JSONL 分帧） | P1 | 完成（已构建 + 测试通过 + 真实进程手工验证） | `lib/.../io/jsonl_io_transport.{h,cpp}`、`client/src/io/jsonl/jsonl_mode.cpp`；模块 `jsonl_mode`、`jsonl_runner` |
| LLM-8 | Anthropic 缓存断点与缓存用量 | P1 | 完成（已构建 + 测试通过） | `ModelConfig::cacheControl` + `AnthropicProvider::applyCacheBreakpoints` / `applyUsage`；账本 `usage.cache_write_prompt_tokens`（schema v3）；模块 `anthropic_provider`、`fake_provider`、`session_schema`、`config_loader` |
| LLM-13 | HTTP 录制回放夹具 | P1 | 完成（已构建 + 测试通过） | `test/include/agentxx-test/core/http_recorder.h`（`HttpRecorder`/`HttpPlayer`/`HttpFixture` + 脱敏）；模块 `http_recorder` |
| CFG-9 | 配置键目录生成与新鲜度门禁 | P1 | 完成（已构建 + 测试通过） | `test/core/test_config_keys.cpp`（键目录表 + 真实加载校验 + 源码扫描）、生成物 `agent/schema/config-keys.json` 与 `docs/zh-cn/design/config-keys.md`、共用骨架 `test/include/agentxx-test/core/schema_artifact.h`；模块 `config_keys` |

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

> 本节在 2026-10-07 按实施进度刷新（此前列出的 TOOL-16、CFG-1、UI-2+TST-5、TST-8、
> PLG-6、PLG-10、CFG-3、TOOL-12、UI-9、ARC-5、ARC-6 均已完成；本轮的 TST-3、LLM-5/TST-1、
> PRO-4、PRO-5、TST-2、RET-1a/STO-12b、LLM-7、TST-9 也已完成，见阶段 U~X）。

- **P0 余项**：无。计划 §17.1 的 P0 条目已全部落地或经人工核定不做
  （LOOP-5、JOB-1/JOB-2、LLM-1、RET-5 等见 `plan.md` 各条目"人工核定"列）。
- **P1 余项（按计划核定可行、本轮未做）**：
  - PRO-8（stdio JSONL 一次性运行）：需要新的客户端运行模式（复用 Wire 结构 + JSONL 分帧）
    与配套用例，属独立功能块（本轮未动，建议单独一轮做：先定 stdout 协议帧格式与
    "响应 ≠ 轮次完成"的语义，再接入 `mode_runners`）；
  - LLM-8（Anthropic 缓存断点 + 缓存用量入账）：需要 ① 请求体加可选 `cache_control` 断点
    （新配置项 + Anthropic 请求装配改动 + 请求体断言用例）、② 解析 Anthropic
    `cache_read_input_tokens` / `cache_creation_input_tokens` 并落到账本
    （账本当前只有 `cached_prompt_tokens` 一列，"写入量"需要 schema v3 迁移 + 新列）；
  - LLM-13（HTTP 录制回放）：需要在测试侧新增"可注入传输层/录制服务器 + 回放服务器"
    夹具（按请求摘要与顺序保存响应流、敏感 headers 脱敏），与 LLM-5 的 provider 级注入互补；
  - PLG-1（注册可逆与清理审计）：统一注册清单 + 禁用/卸载后的基线断言（工具/权限/能力/
    订阅/UI/定时器/键位逐项清理用例，不改 ABI）；
  - PLG-2（声明式贡献集合与重算）：把提示词/工具/资源/UI 贡献做成"活动集合"并在启停后重算；
  - PLG-4（独占能力 slot）：为压缩器/记忆提供者等少数独占能力建 slot（卸载回落到内置）；
  - PLG-7（教学式错误与信任声明）：插件装载失败原因补"怎么改"的指引 + 文档写明原生插件
    同进程、无沙箱、只能加载可信代码（后者已在 `security.md` 写明，SDK 侧文案待补）；
  - PLG-8（插件文档分页与接口表数字校验）：`plugins.md` 拆分 + 接口表数量由常量校验
    （agent 侧 19 张 / client 侧 9 张已在文档中，缺自动校验）；
  - CFG-9（生成式配置键目录）：从 `AgentConfig`/`YamlAppConfig` 生成"键路径/类型/默认值"
    目录并与 PRO-4 共用生成器骨架 + 新鲜度门禁；
  - P2/ROM：ARC-8（消费者窄接口试点 2~3 处）、OBS-5（模块级日志开关，需改日志库信道格式）；
  - PLG-8 的文档拆分（`plugins.md` 入门/生命周期/SDK/宿主/client/规则）：接口表数量校验已完成，
    纯文档重组留待需要时再做。
    （TST-4 并发竞态清单、TST-6 存储一致性骨架已完成，见阶段 Z；TST-7 的
    "插件注册清理"那一半随 PLG-1 做。）
- **暂缓**：CTX-7（附件引用）——计划本身标注"需进一步理解具体实施内容"，需要先明确
  "引用 id + 校验元数据"在 provider 侧的具体形态再动手。
- **已核定不做**：见 `plan.md` 各条目的"人工核定"列与 `docs/zh-cn/design/roadmap.md` §15。
## 阶段 Z：竞态清单、存储一致性骨架与门禁扩展（TST-4 / TST-6 / TST-7，2026-10-07）

计划依据：`plan.md` §15 TST-4（"取消 vs 工具结算、取消 vs resume、插件卸载 vs 工具执行、
持久化节流 vs 轮末、并行结果乱序"）、TST-6（"SessionStore、share_store、settings_db 的
替身/后端统一跑同一语义断言"）、TST-7（"依赖方向、DSO 白名单、接口表集合、UI block 名、
插件注册清理和文档路径"）。

已完成：

- **TST-7 门禁扩展**（`agent/test/core/test_boundaries.cpp` 新增三条规则，模块 8→11 断言）：
  - 规则 9「UI 组件名」：解析客户端 `ui_components.cpp` 的 `kTuiBlockNames`，要求
    ① 每个名字都能在描述层组件表（`pluginxx::ui::gen::kBlockTable`）里找到（拼错/自造名
    立即失败）；② 不得重复；③ 组件表里 `BlockLevel::Core` 的组件必须全部声明（核心组件
    不支持 = 大面积降级）。只读源码文本，不依赖本次是否构建 client，避免关掉 client 后
    规则静默失效。
  - 规则 10「接口表名集合」：从三份 SDK 头读出接口表**名字**（内核 `tables.h` 的
    `PLUGINXX_IFACE_*` 10 张、`plugin_api.h` 的 `AGENTXX_PLUGIN_IFACE_AGENT_*` 9 张、
    `client_plugin_api.h` 的 `AGENTXX_IFACE_CLIENT_*` 9 张），校验数量、唯一性与前缀，
    并要求 18 张领域表名都出现在 `docs/zh-cn/design/plugins.md`（文档漏写新表 = 插件作者
    查不到该 IID）。数量常量只能发现"少一张表"，本规则能发现"名字写错"。
  - 规则 11「文档路径」：扫描根 `AGENTS.md` 与四份目录级 `AGENTS.md` 里出现的
    `docs/zh-cn/**.md` 路径，逐个按仓库根解析并要求文件存在（文档改名后引用会变死链）；
    一处都没解析到即判失败（防止规则本身失效）。
- **TST-6 存储一致性骨架**（新模块 `storage_consistency`，260 项断言）：
  - 定义薄适配层 `KvBackend{name, durable, put, get, reopen, failureVisibleOnReadBack,
    makeWritesFail, restoreWrites, lastError}` 与一份共享语义用例 `runKvContract`，
    同一组断言跑四个后端：`settings_db`、`SessionStore` 的 `store` 表、share store 纯内存
    替身、share store「缓存 + 回库」；
  - 契约条目：① 未写入的键读回无值；② 往返 + 覆盖写读到最新值；③ 空串 = "存在但内容为空"；
    ④ 多条目隔离（顺序读 + 逆序读，穿过 LRU 缓存容量上限）；⑤ 256 KB 大值往返；
    ⑥ 重开（= 进程重启）：持久化后端保留、纯内存替身明确清空；⑦ 写失败可感知 +
    恢复可写后继续工作；
  - 写失败的两种可观察形态被显式区分（并各自断言）：`settings_db`（父目录被文件占住）
    与 `SessionStore`（外部持锁）**读回立即能看出失败**；注入持久化的 share store
    则是"内存副本先可见、落盘失败"，靠 `lastWriteError()` 诊断，并断言"恢复可写后重新
    打开确实看不到那次写入"；
  - 顺带记录一处既有语义：`SessionStore::lastWriteError()` 是**粘性**的（成功写入不清理），
    所以适配层的"写失败"判据用读回结果 + 外部锁，而不是该字段的瞬时值。
- **TST-4 竞态清单**（新模块 `race_guards`，68 项断言，5 组用例）：
  - R1 取消 vs 工具结算：快工具已结算、中工具执行中、独占工具未启动时取消 —— 断言
    每条声明恰好一条结果、顺序按声明顺序、已结算结果保留真实值、未启动的执行体确实没跑；
  - R2 并行结果乱序：5 个并行安全调用睡眠时长递减（完成顺序与声明顺序完全相反），
    结果仍按声明顺序写回，并断言最先结束的确是最后声明的那个；
  - R3 中断应答在途 vs 取消：中断已问出、应答还在路上时取消 —— 断言轮次一定收敛
    （< 6s 不挂死）、中断只问一次、执行不超过两次；两条合法结局分别断言
    （取消结束 / 应答落地后 resume 返回真实应答）；取消分支允许留下至多 1 条悬挂
    tool_call，并**再跑一轮**验证下一次请求前悬挂被修正（`danglingToolCallCount == 0`）；
  - R4 工具执行中被注销（插件工具的动态注册表路径）：执行体保活、调用正常返回真实结果、
    注册表已摘除、调用结束后保活引用释放（`weak_ptr expired`）；
  - R5 持久化节流 vs 轮末：节流窗口内连续追加 + 中途节流刷盘 + 轮末 `persistNow`，
    断言库内消息数量/顺序/序号与内存一致（不重复、不丢失）。
  - 说明：TST-4 的"插件卸载 vs 工具执行"里"卸载与 inflight 租约"那一半已由
    `plugin_runtime` / `plugin_multi_instance` 覆盖（卸载先欠着、lease 归零后再 destroy 可重试），
    本模块补的是"工具侧保活"这一半。

验证：

- 构建：`agentxx_test` exit=0（新增测试源文件后重跑了一次 CMake 配置）。
- 测试：`boundaries` 11/0、`storage_consistency` 260/0、`race_guards` 68/0、
  `toolcall_parallel` 48/0、`cancel` 45/0、`message_supplement` 95/0、
  `persist_semantics` 25/0、`session_schema` 104/0、`settings_db` 73/0、`writer_lease` 25/0。
- 负面验证（门禁确实会红）：规则 9/10/11 各自依赖的"数据源"缺失或改名即失败
  （文件缺失、数组解析失败、引用数为 0 都走失败分支）。

注意事项 / 与计划的差异：

- TST-6 的"替身"落在 share store 的**纯内存模式**上（`MiddlewareContext` 未注入持久化时
  内存是唯一副本），而不是另写一套内存桩 —— 它与持久化后端跑同一组断言，差异只有
  "重开后是否保留"，这条差异被写成断言而不是被适配层抹掉。
- TST-4 不断言"谁先谁后"（那会变成看调度运气），只断言不变量（结果唯一性、顺序、
  收敛时间、悬挂上限与后续修复）。R3 的取消分支实测会留下 1 条悬挂 tool_call，
  属该轮工具调用尚未定稿的既定形态，由下一次请求前的 `repairMessages` 修正。

## 阶段 AB：客户端模型层与输入栏硬件光标（UI-1 / UI-5，2026-10-07）

计划依据：`plan.md` §10 UI-1（"只抽三块（历史分页窗口 / 消息队列镜像 / 重连与 seq 校验）
到无 FTXUI 依赖的模型类"）、UI-5（"只做'输入栏硬件光标 + 终端不支持时降级'"）。

已完成：

- **UI-1 客户端模型层**（新增 `agent/client/.../io/tui/model/{history_window,queue_mirror}.{h,cpp}`）：
  - 两块状态从 `TUIClientAgentIO` 抽成**可拷贝的值类型**，随 `TUIRenderState` 渲染快照一起
    复制（UI 线程每帧读到的是同一份状态），端点只负责发请求/渲染/提示：
    - `HistoryWindow`：已加载区间 `[windowStart, +loadedCount)`、"上方是否还有更早历史"、
      分页请求去重与在途标记、页响应判定（接受 / 空页 / 迟到会话 / 不连续页 / 无在途请求）、
      断线增量补拉的尾部**序号连续性**（首个 / 连续 / 断号 / 重复）与切换会话失效；
    - `MessageQueueMirror`：服务端排队输入的镜像（整体快照为唯一权威）、按 id 删除/查找/清空、
      队列状态（idle/running/paused/draining，未知文本按 idle，空值按 idle）、**投递回执记账**
      （同一 requestId 只记一次 → 界面不重复提示，容量上限按最旧淘汰）。
  - TUI 接线：`TUIRenderState` 的 `historyWindowStart/historyTotal/historyLoading` 三个标量
    由 `HistoryWindow history` 取代（`hasMoreHistory()` 改为委托），
    `onViewMessagesPage` / `requestOlderHistory` / `onSync` / `onIncrementalSync` /
    `switchToSession` / `onMessageQueueUpdate` 全部改为调用模型；
    队列镜像另使 `onMessageQueueUpdate` 的"保留界面展开态"逻辑只写一处；
  - 增量补拉在追加消息时用 `observeTailSeq(payload.lastViewSeq)` 做一次连续性观察，
    断号记 WARN（下一步重连的 `afterViewSeq` 补拉会补齐，这里留诊断线索）；
  - 边界纪律：新增 `boundaries` 规则「客户端模型层边界」—— `model/` 目录下的文件不得包含
    ftxui/*、TUI 端点/组件/渲染状态、插件管理器实现、网络头；目录为空也判失败
    （防止抽取被挪走后规则静默失效）。模型层可依赖协议数据结构（要镜像 wire 上的队列/消息）。
- **UI-5 输入栏硬件光标**（`tuiHardwareCursorSupported()` + 能力段）：
  - 输入元素聚焦时使用**可见光标形状**（竖条/方块），终端据此把系统光标画在光标格上 ——
    中文输入法候选框跟随真实光标位置；终端对光标处理有已知问题时（FTXUI 的终端特性检测
    `Quirks::CursorHiding()` 为假，如部分老终端会把光标下的字符吃掉）不请求硬件光标，
    由输入栏自绘的光标格（反色空格）承担指示作用（功能不受影响，只是看不到系统光标）；
  - 该判定成为能力段 `terminal.hardware_cursor` 的**唯一来源**（此前硬编码 false +
    "待实施"注释，属于虚报缺失能力）；
  - `docs/zh-cn/design/tui.md` 新增 §2.12（模型层 + 输入栏光标两节）。

测试：

- 新模块 `tui_model`（134 项断言，同步，无终端依赖）：历史窗口的同步/分页/序号全流程
  （含不连续页、迟到会话、空页、重复请求、序号断号与恢复、切换会话后旧页失效），
  队列镜像的快照/状态/删除/查找/清空/回执去重与容量淘汰。
- `ui_capabilities` 改为断言能力段取值与 `tuiHardwareCursorSupported()` 一致（单一来源）；
- `tui_scroll`（772）与 `tui_stream`（134）同步改用模型 API 后回归通过；
- `boundaries` 12/0（新增模型层边界规则）。

验证：

- 构建：`agentxx_cli`（含 TUI）与 `agentxx_test` 均 exit=0。
- 测试：`tui_model` 134/0、`tui_scroll` 772/0、`tui_stream` 134/0、`tui_surface` 673/0、
  `ui_capabilities` 26/0、`tui_input` 124/0、`boundaries` 12/0。

注意事项 / 与计划的差异：

- 计划的"重连与 seq 校验"落在 `HistoryWindow::observeTailSeq` 上：客户端的重连补拉由
  WS 传输层（`hello.afterViewSeq` + delta 重放）负责，模型这一层只做**连续性观察与记录**，
  不重复实现补拉决策（避免两处判断漂移）；断号时记 WARN 供诊断，实际补齐仍走传输层。
- UI-5 的"降级"没有做成开关项：FTXUI 的输入元素内部决定光标形状装饰器，宿主无法在
  不重写输入渲染的前提下切换成"只画自绘光标"；当前降级路径 = 终端不定位光标时输入栏
  自绘光标格仍然可见（能力位如实反映"是否请求硬件光标"）。

## 阶段 AA：插件注册可逆、独占 slot 与教学式错误（PLG-1 / PLG-2 / PLG-4 / PLG-7，2026-10-07）

计划依据：`plan.md` §12（"生命周期框架已有撤销和 owner 清理；补一份统一注册清单、
禁用/卸载后的基线断言"；"提示词、工具、资源、UI 描述采用'活动贡献集合'，启用/禁用后
重新计算派生状态"；"只为压缩器、context assembler、memory provider 等少数独占能力提供
slot；卸载自动回到内置"；"错误告诉插件作者如何修正；文档明确原生插件同进程、无沙箱"）。

已完成：

- **PLG-1 统一注册清单**（`plugin_manager.h` 的 `PluginManager::RegistrationInventory` +
  `registrationInventory(inst)`）：
  - 一个插件能贡献的东西分散在工具注册表/权限中间件/中间件链/图注册表/事件总线/能力表/
    提示词/资源应用器里；本结构把**宿主可撤销**的注册集中成一份可比较口径：
    `tools / permissionTools / hooks / graphNodeTypes / eventSubscriptions / capabilities /
    promptKeys / skillDirs / memoryFiles / mcpNamespaces / middlewareAttached /
    ownsGraphDefinition`，加 `total()`（0 = 已回到基线）；
  - **按"生效事实"取数，不按记录条数**：禁用时实例仍保留注册记录（供 enable 重新声明），
    因此工具按"是否还在注册表里"、钩子按"中间件是否还在链上"、能力按
    `hasCapability(name)`、图节点按 slot 是否 active 计数，资源和提示词按应用器/贡献表；
  - `PluginListView` 增 `registrationTotal / promptKeyCount / skillDirCount / memoryFileCount /
    mcpNamespaceCount / ownsGraphDefinition`，装配快照（ARC-6）按插件输出这些字段。
- **PLG-2 贡献集合与重算（现状核对 + 测试 + 文档）**：
  - 提示词贡献模型（`PromptKeyState`：base + 按 sequence 应用的全部存活贡献；
    删除 owner 贡献后**重新合成**）在源码中已实现，但此前没有任何测试；
  - 本阶段补了针对它的用例（多 owner、卸载只删自己的贡献、外部直写 rebase 基础值），
    并把三类贡献的撤销方式写进 `docs/zh-cn/design/plugins.md` §15.7。
- **PLG-4 独占 slot（执行图定义）**（`PluginManager::setGraphJson` +
  `releaseGraphDefinitionSlot`）：
  - 原来 `set_graph_json` 是**无条件覆盖**：两个插件交替设置后，任何一方卸载都无法判断
    该恢复成哪一份定义，而且没有任何恢复动作。现在改为独占 slot：
    首次占用记录"占用前的定义"（内置图或宿主自定义图）并成为占用者；同一占用者可继续修改；
    **其他实例占用中被拒绝**（返回非 0，日志给出占用者与"先卸载/禁用它"的建议）；
  - 占用者禁用/卸载时（`detachDomainRegistrations`）恢复占用前的定义 = **回到内置**，
    并清空占用者；
  - 装配快照的执行图段输出 `definition_owner`，渲染文本在非空时标注
    `(definition from plugin 'x')`。
- **PLG-7 教学式错误**（`PluginManager::diagnosePluginPath` + 三个装载入口）：
  - 只读巡检常见装载失败原因并返回"照什么改"的建议：路径为空 / `builtin://` 缺名字 /
    内置名未注册 / 路径不存在 / 目录下没有 `plugin.yaml` / 清单 YAML 非法或缺字段 /
    清单 `entry` 指向的库文件不存在 / 库缺少 `agentxx_plugin_agent_{create,start,stop}`
    入口符号（提示用 `AGENTXX_PLUGIN_AGENT_EXPORT` 或 `..._LIFECYCLE_EXPORT`，并检查导出
    白名单）；
  - 装载入口（`loadNativeAsync` / `loadBuiltinAsync` / `loadPluginAsync`）在**失败后**
    记一条 WARN 建议（`allowClientOnlySkip` 的"另一端插件"跳过不记），成功路径零开销；
  - `plugins.md` §15.7 写明信任声明（原生插件与宿主同进程、加载即信任、权限系统约束的是
    模型经工具发起的运行期动作）。

测试（新模块 `plugin_cleanup`，96 项断言，5 组用例）：

| 用例 | 覆盖点 |
|---|---|
| 注册清单基线 | 装载后各分类计数与插件的实际注册一致（tools=5/hooks=1/caps=1/…，total=12）；`list()` 的诊断字段与清单同源；禁用后宿主侧全部为 0（实例记录保留）；启用后由 start 重新声明（工具/hook/能力恢复）；卸载后实例消失、注册与提示词条目无残留 |
| 提示词贡献多 owner | A 贡献 → B 贡献后写者生效；卸载 A 后 B 的取值仍在；外部直写立即生效但触发合成后成为新基础值（B 的贡献在其上重新应用）；卸载 B 后外部值保留（不会清成"不存在"）；`removeAppendSection` 收尾 |
| 执行图独占 slot | A 占用成功（占用者/图内容/清单 `ownsGraphDefinition`）；B 被拒绝且不覆盖；占用者自身可改；A 卸载后恢复到占用前定义且 slot 释放；随后 B 可占用；禁用同样释放 |
| 真实插件路径 | `example_graph_node` 在其 `start` 里 `set_graph_json` → 占用者即它；卸载后定义回到 base |
| 装载失败建议 | 空路径 / 路径不存在 / 内置名不存在 / 无 `plugin.yaml` / YAML 非法 / `entry` 库缺失 / 库缺入口符号（用主库本体验证）；另在 `plugin_resources` 的失败场景里能看到真实告警文本 |

验证：

- 构建：lib `INSTALL`（含插件重建）、`agentxx_test` 均 exit=0，无新增 error。
- 测试：`plugin_cleanup` 96/0、`plugins` 541/0、`plugin_resources` 89/0、
  `plugin_runtime` 672/0、`plugin_multi_instance` 80/0、`assembly_snapshot` 37/0、
  `assembly_snapshot_io` 27/0、`boundaries` 11/0。

注意事项 / 与计划的差异：

- 计划的 PLG-2 提到"UI 描述"也走活动贡献集合：客户端的 UI 注册（面板/Info/定时器/键位）
  本来就是"注册即记账、撤销即摘除"的叠加模型，且已有 `client_plugins` 用例覆盖，
  本轮未改动；
- 计划的 PLG-4 举例是压缩器 / context assembler / memory provider 这类"实现替换型"能力：
  本项目这些点目前没有"替换内置实现"的需求与消费者，强行加桩会变成没人用的死代码；
  因此本轮把"独占 slot"落在**已经存在替换语义的执行图定义**上（它正是"多个贡献者
  互相覆盖时无法判断恢复谁"的真实场景），机制与拒绝/恢复语义都实现并测到了；
- `diagnosePluginPath` 只在失败路径调用，不做装载前的强制校验 —— 避免与内核已有的
  路径/清单解析逻辑重复并在边缘情形（内置回退、另一端插件）误判。

## 本轮全量回归（2026-10-07，阶段 Z~AC 完成后）

- 全量 `agentxx_test`（Debug + ASan/UBSan，全部模块）：**35,985 项断言 0 失败**，
  进程 exit=0，无 AddressSanitizer 报告（上一轮基线 35,328 → 本轮 +657）。
- 新增模块：`storage_consistency` 260、`race_guards` 68、`plugin_cleanup` 96、
  `tui_model` 134、`observability` 96；既有模块增量：`boundaries` 9→12（UI 组件名 /
  接口表名集合 / 文档路径 / 客户端模型层边界）、`tui_scroll` 与 `tui_stream` 改用模型 API、
  `ui_capabilities` 改为断言能力段与唯一来源一致。
- 产物构建：lib `INSTALL`、`agentxx_test`、`agentxx_cli` 均 exit=0。
- 手工验证：`agentxx_cli --dump-diagnostics` 输出完整报障包（环境/指标/装配含配置 JSON/
  插件逐项/日志尾部 67 行）并退出码 0；`agentxx_cli --dump-config` 不受影响。

### 本轮之后仍待实施（按价值排序）

1. **PRO-8 stdio JSONL 一次性运行**（P1，独立功能块）：新运行模式 + JSONL 分帧 + 用例。
2. **LLM-8 Anthropic 缓存断点与缓存用量**（P1）：① 请求体可选 `cache_control` 断点
   （新配置项 + Anthropic 请求装配 + 请求体断言）；② 解析 `cache_read_input_tokens` /
   `cache_creation_input_tokens` 落账本（"缓存写入量"需要 schema v3 迁移 + 新列）。
3. **LLM-13 HTTP 录制回放**（P1）：测试侧"可注入传输层 + 录制/回放服务器"夹具（按请求摘要与
   顺序保存响应流、敏感 headers 脱敏），与已完成的假 provider 接缝互补。
4. **CFG-9 生成式配置键目录**（P1）：从 `YamlAppConfig`/`AgentConfig` 生成"键路径/类型/
   默认值"目录 + 与 PRO-4 共用生成器骨架 + 新鲜度门禁。
5. **ARC-8 窄接口试点**（P2）：先试点 2~3 个消费者（summarization/permission 中间件、
   subagent 工具），确认测试替身收益后再推广。
6. **OBS-5 模块级日志开关**（P2，需改日志库信道格式）。
7. 已核定"暂缓/不考虑"的条目见 `plan.md` 各行的"人工核定"列（CTX-7 附件引用仍在暂缓）。

## 阶段 AC：关键指标与诊断包导出（OBS-3 / OBS-4 / STO-13，2026-10-07）

计划依据：`plan.md` §14 OBS-3（"首 token、轮次耗时、工具成功/失败/超时、压缩次数、
上下文 token、缓存命中；复用 benchmark 基础设施"）、OBS-4（"会话摘要、日志尾部、
脱敏配置、插件和环境，便于报障"）、§4 STO-13（"导出展示历史、工具定位符、轮次和诊断；
配置脱敏、API key 不进入报告"）。

已完成：

- **OBS-3 关键指标**（新增 `agent/lib/{include/agentxx/util,src/util}/observability.{h,cpp}`）：
  - `KeyMetrics`：轮次 (总数 + 正常/出错/取消/中断)、**首 token 延迟 (TTFT)** 的样本数/
    累计/最大值、轮次耗时累计/最大、模型调用次数与 prompt/completion/cached token、
    模型错误次数与最近一次错误分类、工具终态 (成功/失败/取消/中断)、压缩次数与压缩前后 token；
  - 打点位置 (每处只做几个原子操作)：
    - 轮次起止与终态分类：`BaseAgent::runTurnAsync`（起：`noteTurnStart`；止：按
      `hasError`/`errorMessage`/`interrupted` 归类后 `noteTurnEnd(durationMs)`）；
    - 首 token：`ModelCallWrapNode` 的流式回调（同一轮只记第一次，DONE/空 chunk 不计）；
    - 模型调用与用量：`recordUsage` 旁的成功路径（沿用同一次 usage 结算，不再多算一次）；
    - 模型错误：provider 异常路径按 `classifyLlmError` 分类记录（与重试策略同一分类器）；
    - 工具终态：`ToolcallWrapNode` 的结算点（短路未执行、取消、中断、异常、正常产出各一处）；
  - 指标对象挂在 `AgentContext::metrics`（`BaseAgent::init` 的 `event_bus` 步骤创建，
    与事件总线同一生命周期，回滚时释放）；装配快照新增 `metrics` 段并在渲染文本给一行摘要。
- **OBS-4 诊断包**（新增 `agent/lib/{include/agentxx/util,src/util}/diagnostics.{h,cpp}`）：
  - `buildDiagnosticsText(ctx, sessionId, options)` 按段输出 Markdown 风格文本：
    头部 (时间/版本/平台/核数)、关键指标 (摘要 + JSON)、装配 (配置侧快照 + 运行侧
    快照 + **配置快照 JSON**)、会话 (标题/消息计数/上下文条数/序号/账本聚合/持久化计数)、
    后台任务 (TaskScope 待办与名称)、插件逐项 (各分类生效计数 + 图定义占用者)、日志尾部；
  - 内容边界：默认**不含消息正文** (计数与用量足够定位问题)，需要时 `includeMessages`
    显式打开 (按 `maxMessageChars` 截断)；配置只输出 `api_key_set` 这类布尔与键名；
    日志尾部与消息正文都过 `redactSecrets`；
  - `redactSecrets`：`key=value`/`key: value` (含引号、`Bearer/Basic` 前缀)、裸 token
    (`sk-`/`ghp_`/`github_pat_`)、URL userinfo、URL 查询参数五类形态 (普通文本不受影响，
    避免"什么都屏蔽"让诊断失去价值)；
  - 日志捕获：`enableLogCapture(capacity)` / `recentLogLines(limit)` / `clearCapturedLogs()`，
    基于 `ThreadedLogSink` 的环形缓冲 (自带后台线程；取快照前 `flush`，刚写的日志也在包里)；
  - CLI：`--dump-diagnostics` 与 `--dump-config` 同一位置/同一装配路径，打印诊断包后退出
    (只读导出：不发网络请求、不写会话库；无可用模型时仍给出配置侧信息与失败原因)。
- **STO-13 并入**：会话段即"导出展示历史摘要 + 轮次/用量 + 诊断"的取证口径；
  `includeMessages=true` 时按行导出最近 50 条展示消息 (带序号与 id, 已截断/脱敏)，
  不含 share_store 全文 (那是工具的定位符存储, 由会话库自身管理)。

测试（新模块 `observability`，96 项断言）：

| 组 | 覆盖点 |
|---|---|
| KeyMetrics | 轮次终态四分类与耗时累计/最大值、TTFT 只记一次、模型调用用量与错误分类、工具四终态、压缩、JSON/summary 字段、reset |
| 凭据脱敏 | `api_key=...` / JSON `"apiKey":"..."` / `Authorization: Bearer ...` / 裸 `sk-...` / URL userinfo / 查询参数；普通日志文本不被误伤 |
| 日志捕获 | 未启用时读不到 (不隐式占内存)、容量上限按最旧淘汰、级别前缀、limit、清空、重装与关闭 |
| 诊断包 | 段落齐全 (版本/平台/指标/装配/会话/日志尾部)、`config_json` 含 `api_key_set` 但不含 Key 取值、默认不含消息正文 (会话段)、日志尾部已脱敏、`includeMessages=true` 时出现截断正文、`logTailLines=0` 时无日志段、无配置/无会话/无指标对象的降级路径不崩 |

验证：

- 构建：lib `INSTALL`、`agentxx_test`、`agentxx_cli` 均 exit=0；手工运行
  `agentxx_cli --dump-diagnostics` 输出完整诊断包 (12 核/13 模型/7 插件/日志尾部 67 行) 并退出码 0。
- 测试：`observability` 96/0、`boundaries` 12/0、`session_schema` 104/0、`agent` 198/0、
  `usage_ledger` 21/0、`assembly_snapshot` 37/0。

注意事项 / 与计划的差异：

- OBS-3 的"工具超时"计数未单列：`execute_command` 等工具超时是**工具自身**的超时
  (返回文本)，分发层硬超时按 TOOL-4 的裁定不做，因此指标里只区分成功/失败/取消/中断；
- OBS-3 的"缓存命中"只落 `cached_tokens` (prompt 缓存读)；缓存**写**量需要
  LLM-8 的 Anthropic `cache_creation_input_tokens` (账本当前无该列)，本轮不引入；
- **OBS-5（模块级日志开关）未实施**：`utilxx_base::LogEntry` 目前只有 level/seq/时间/正文，
  不带模块或来源；按模块过滤只能在**记录点**带上模块标签 (改日志宏与调用点) 或在 sink 侧
  按消息文本猜前缀（不可靠）。这项要动日志库的信道格式，属于独立改动，本轮不做（保持现状：
  全局级别 + TUI 日志窗口级别过滤）。

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
- 阶段 M 补充（PRM-5）完成后提交：`技能优先级与同名裁决 (PRM-5)`。
- 全量回归与测试夹具修正完成后提交：`修正测试夹具内存生命周期与只读工具自动摘要预期`。
- 阶段 N（ARC-6 / TOOL-12 / CFG-3 / PLG-10）完成后提交：`启动装配快照与 --dump-config、插件装载耗时与注册计数 (ARC-6/TOOL-12/CFG-3/PLG-10)`。
- 阶段 O（CFG-1）完成后提交：`配置结构化校验: 键路径/严重级别/来源, 路径与权限组合检查 (CFG-1)`。
- 阶段 P（UI-2 / TST-5 / TST-8）完成后提交：`UI 快照夹具与一键门禁脚本 (UI-2/TST-5/TST-8)`。
- 阶段 Q（TOOL-16）完成后提交：`按规范化文件路径排队执行写/改操作 (TOOL-16)`。
- 阶段 U（LLM-5 / TST-1 / TST-3）完成后提交：`假 provider 接缝与迁移中断用例 (LLM-5/TST-1/TST-3)`。
- 阶段 V（PRO-4 / PRO-5 / TST-2）完成后提交：`Wire 协议字段清单生成物与连接阶段状态机 (PRO-4/PRO-5/TST-2)`。
- 阶段 W（RET-1a / STO-12b）完成后提交：`会话检索与改名: 协议消息、端点处理与 TUI 入口 (RET-1a/STO-12b)`。
- 阶段 X（LLM-7 / TST-9）完成后提交：`单次 provider 调用取消域、测试耗时打印与凭据环境清理 (LLM-7/TST-9)`。

## 阶段 P：UI 快照夹具与一键门禁（UI-2 / TST-5 / TST-8，2026-10-06）

计划依据：`plan.md` §10 UI-2（与 TST-5 合并为"UI 快照夹具"：固定尺寸文本 + 命中区基线 +
一键更新 + 差异输出）、§15 TST-8（Debug 构建、fail-fast、边界、负面编译、sanitizer；基准可选）。

已完成：

- **UI 快照夹具**（新增 `agent/test/include/agentxx-test/client/ui_snapshot.h` +
  `agent/test/client/test_ui_snapshot.cpp`）：
  - `renderRowsToGrid(res, w, h)`：把渲染行模型铺到固定尺寸字符网格（每格一个字符，
    宽字符只占首格、续格为空格，因此网格文本与显示宽度一一对应）；
  - `serializeHitRegions(res)`：命中区清单一行一条（`row/kind/id/sub/x/y/w/h/arg/owner/plugin`），
    顺序稳定；
  - `checkUiSnapshot(name, actual)`：与 `agent/test/snapshots/ui/<name>.snap` 比较；
    基线缺失判失败并提示生成命令；内容不同时打印**首个不同的行/列 + 两侧整行内容**
    （含两侧行数）；`AGENTXX_UPDATE_UI_SNAPSHOTS=1` 时写回基线（一键更新）；
    未注入基线目录（独立构建）时退化为"只渲染不比较"；
  - 夹具的失败路径本身有自测（基线缺失判失败、内容不同判失败且 `diff` 带 `line/col`）。
- **快照用例**（模块 `ui_snapshot`，22 项断言）：Markdown（标题/列表/行内代码/围栏代码折行）、
  表格（CJK 双宽 + 右对齐 + 超宽截断 + 可点单元格）、树（展开 / 宿主折叠）、差异、
  表单（buttons/text/switch/number + 提交取消行，含"已交互"状态）、窄终端（24 列）、
  超长内容裁剪、未知组件与未知字段降级。每个用例比较"文本画面 + 命中区清单"两份基线。
- **基线入库**：`agent/test/snapshots/ui/*.snap`（10 个画面 + 对应 `.hits.snap`），
  已在 `agent/test/AGENTS.md` 写明更新流程（改渲染后人工 review diff 再提交）。
- **一键门禁**：
  - `agent/script/gate.sh`（Linux/macOS）：可选构建 → 全模块 fail-fast 测试
    （含 `boundaries` 边界检查、`wire_roundtrip` 协议往返、`config_validation` 配置校验、
    `ui_snapshot` 快照）→ 插件导出白名单（`check_plugin_exports.sh`）→ SDK 反例编译
    （`check_sdk_negative_compile.sh`）→ 可选基准；末尾打印 PASS/SKIP/FAIL 汇总，任一失败退出码 1；
  - `agent/script/gate.ps1`（Windows）：同一份流程（构建 + fail-fast 测试 + 可选基准），
    导出白名单与 SDK 反例编译在 Windows 上显式记为 SKIP（需要 nm/python3/bash）；
  - Debug 构建默认带 ASan（+ 非 MSVC 的 UBSan），因此"跑 Debug 测试"即 sanitizer 门禁。

验证：

- 构建：`agentxx_test` exit=0，无新增 error。
- 测试：`ui_snapshot` 22/0（含 2 条夹具失败路径自测）、`tui_ui_items` 203/0、`ui_items` 117/0。
- 门禁自测：`pwsh -File agent/script/gate.ps1 -Modules "boundaries,ui_snapshot,config_validation"`
  → 三个模块全通过、汇总输出 `[gate] OK`（全量 fail-fast 见"全量回归"一节）。
- 更新流程自测：`AGENTXX_UPDATE_UI_SNAPSHOTS=1` 生成基线 → 比较模式再次运行全部通过。

注意事项 / 与计划的差异：

- 基线用的是"逐格字符网格"而不是 `Screen::ToString()`：后者会把颜色转义序列写进文件，
  子串与列位断言都会被转义码打断；颜色差异不在本夹具范围内（需要颜色断言时用现有
  `renderScreen` 那类接口）。
- 一键更新只重写基线，不做自动"接受全部差异"的判断：更新后必须人工 review git diff
  （已在 `agent/test/AGENTS.md` 写明）。
- Windows 门禁脚本不跑导出白名单与 SDK 反例编译（工具链不同），这两项由 Linux/macOS 的
  `gate.sh` 负责；CI 上应当至少跑一份 `gate.sh`。

## 阶段 O：配置结构化校验（CFG-1，2026-10-06）

计划依据：`plan.md` §13 CFG-1（限定范围：校验结果结构化 (键路径 + 来源文件 + 致命/警告)，
补"路径存在性"与"权限组合"两类检查；不做配置对象树重建、不改 base/overlay 合并语义）。

已完成：

- **新增 `agentxx/agent/config_validation.{h,cpp}`**：
  - `ConfigIssueLevel`（Warning / Fatal）、`ConfigIssue{level, keyPath, message, source}`、
    `ConfigValidationReport{issues, hasFatal(), count(), render(), toJson(), setSource()}`
    （`render()` 一行一条：`[fatal] work_dir: ... (agentxx-config.yaml)`）；
  - `validateAgentConfig(cfg)`：**不访问文件系统**的结构/语义校验 —— 模型（无可用模型、
    `model.use` 指向不存在的模型、可用模型条目缺 base_url/api_key）、路径形态（`work_dir`
    必须绝对、`data_dir`/会话根相对路径提示）、权限组合（`deny` + 白名单、`pass` 模式提示、
    白黑名单同路径说明"黑名单优先"、空条目）、会被夹取或无效的取值（并行上限越界、
    开启工具过滤但白名单为空、重复调用阈值 0、`llm.max_retry` 过大）、插件声明
    （空 path、`config` 必须绝对、同路径重复声明）、MCP（空命名空间、空 url）、
    持久化组合（开启但无目录 = 内存降级；配了根但关掉持久化 = 根未使用）；
  - `validateConfigPaths(cfg)`：**访问文件系统** —— `data_dir`/会话根目录可建/可写
    （要求持久化时不可用 = 致命，否则警告）、skill/memory/rag 路径存在性（警告，
    启动时记为加载失败）、插件文件是否存在（警告，启动时跳过）、MCP 端点协议前缀；
  - `validateAgentConfigWithPaths(cfg)`：两者合并（启动装配侧统一入口）。
- **`AgentConfig::validate()` 改为同一套规则**（取第一条致命问题返回），消除两份实现漂移；
  之前该函数无调用点（"client 启动时调用"只是注释）。
- **CLI 接线**（`agent/client/main.cpp`）：`validateStartupConfig()` 在启动路径
  （ACP 模式 / 本地 tui·cli·server·远程 client 共用路径）执行校验：致命问题逐条
  `XX_LOGE`（键路径 + 来源）后终止启动返回 1；警告逐条 `XX_LOGW` 后照常启动并汇总条数。
  `--dump-config` 打印 `config issues[N]:` 段（与快照同一份校验结果），有致命问题时
  仍打印完整快照再以返回码 1 退出（可当配置门禁使用）。

测试：

- 新同步模块 `config_validation`（47 项断言，`agent/test/core/test_config_validation.cpp`）：
  正常配置无致命；无模型 / `model.use` 不存在 / `work_dir` 相对 / 插件 `config` 相对 → 致命；
  `deny` + 白名单、`pass`、白黑名单同路径、并行上限越界、空白名单 + 过滤、会话根未生效、
  MCP 空命名空间/非 http 端点、skill/memory/rag 路径缺失、插件文件缺失与同路径重复 →
  警告且不致命；内存模式（无 data_dir）为警告；会话根被同名文件占用 → 致命；
  `toJson()` 的 level/key/source 字段与 `render()` 的来源标注；
  `AgentConfig::validate()` 与同一规则同源（相对 work_dir 时返回错误）。

验证：

- 构建：lib `INSTALL`、`agentxx_test`、`agentxx_cli` 均 exit=0，无新增 error。
- 测试：`config_validation` 47/0、`assembly_snapshot` 37/0、`boundaries` 8/0、`agent` 198/0、
  `config_loader` 375/0、`session_schema` 91/0、`plugin_resources` 89/0、`remote_agent` 453/0。
- 手工验证：`agentxx_cli --dump-config` 在真实配置下输出 2 条警告（`memory.paths[0]` 缺失、
  来源标注为配置文件），退出码 0。

注意事项 / 与计划的差异：

- "来源文件"用"配置链标签"表示（overlay 路径，`setSource()` 统一打标），没有做**逐键**
  来源追踪：`YamlAppConfig` 的 base/overlay 合并是按键覆盖，逐键来源需要改合并层数据结构，
  属计划明确不做的"配置对象树重建"。键路径足以定位到配置项。
- `ask` 模式在 `work_dir` 为空时回退进程 cwd 属既定行为，不产生警告（避免每个默认配置
  都刷警告）；只报"配置之间互相矛盾或无效"的组合。
- 校验只在 CLI 装配侧调用；库使用方（FFI/嵌入）仍按需自行调用
  `validateAgentConfigWithPaths()`，lib 不替调用方决定终止启动。

## 阶段 N：启动装配快照与工具清单诊断（ARC-6 / TOOL-12 / CFG-3 / PLG-10，2026-10-06）

计划依据：`plan.md` §1.2 ARC-6（限定范围：启动一次性装配快照写日志 + CLI `--dump-config`；
不做 TUI 诊断页与 wire 侧 `get_diagnostics`）、§5.2 TOOL-12（并入 ARC-6 的工具清单诊断）、
§13 CFG-3（并入 ARC-6 的来源清单）、§12 PLG-10（限定范围：装载耗时 + 接口声明 + 注册计数）。

已完成：

- **装配快照模块**（新增 `agent/lib/{include/agentxx/agent,src/agent}/assembly_snapshot.{h,cpp}`）：
  - `buildConfigSnapshot(AgentConfig&)`（配置侧，不需要构造 agent）、
    `buildRuntimeSnapshot(AgentContext&)`（init() 之后）、`mergeAssemblySnapshot`、
    `renderAssemblySnapshot`（人工可读文本，日志与 `--dump-config` 同源）、
    `logAssemblySnapshot`（Info 一行计数 + Debug 完整 JSON）、
    `resolveToolSource`（工具来源推断）；
  - **配置侧**：模型（含可用模型逐项、`api_key_set` 布尔、`extra_headers`/`extra_config`
    只输出键名）、路径（data_dir / work_dir / session_root）、权限（mode + 白黑名单）、
    特性开关与工具白名单、上限（重试/摘要阈值/重复调用阈值/并行上限）、资源
    （skill/memory/rag/mcp）、插件声明（path/enabled/sides/config/args 键名）、语言与来源标记、
    提示词段落与哈希。**凭据与参数值一律不落快照**（有断言）。
  - **运行侧**：模型注册表（默认 + 全部名称 + 数量）、中间件顺序（名称/是否禁用/会话数）、
    工具清单（装配项：来源/行为开关/是否被白名单过滤 + 原因；动态插件项：来源）、
    插件条目（启用状态/用户禁用/依赖阻塞/装载耗时/注册计数/接口声明/工具名）、
    执行图（名称/节点数/节点类型/边数）、持久化（是否开启/根目录/schema 版本/写租约）、
    组件加载信息（skill/memory/mcp 与失败项及原因）。
- **工具装配记录**（`AgentContext::ToolAssemblyRecord` + `toolAssembly` + `toolSourceHints`）：
  - 中间件贡献的工具在收集时写入来源提示（`middleware:<中间件名>`）；
  - 白名单过滤改为 `stable_partition`，被过滤的工具保留一条 `filtered=true` 记录
    （原因 `not in toolWhitelist (enableToolFiltering)`），被移出集合的工具对象不再丢信息；
  - `bind_tools` 步骤在工具被 move 进图引擎之前收集来源/开关（并行安全、延迟加载、
    自动摘要、重复调用检查、重试次数）。
- **启动日志**（`BaseAgent::init()` 末尾，`verifyStartupAssembly()` 之后）：
  `assembly snapshot (startup): models=.. middlewares=.. tools=.. plugins=.. graph=.. persistence=..`
  （Info）+ 完整 JSON（Debug）。正常启动不打全量。
- **CLI `--dump-config`**（`agent/client/main.cpp`）：
  - 不进入交互模式、不监听端口；先打印配置侧快照（**无可用模型时也能打印**，这正是排查
    配置问题的场景），有可用模型时再构造一次 `CodeAgent`、`init()` 并打印运行侧快照；
  - `init()` 失败打印 `[init failed] <原因>` 并返回 1；打印完退出（插件经 AgentContext →
    PluginManager 析构同步关闭）；`--help` 已补该选项说明。
- **PLG-10 插件装载诊断**（`plugin_manager.{h}` + `plugin_manager_lifecycle.cpp`）：
  - `PluginListView` 增加 `userDisabled` / `blockedByDependencies` / `loadMs` / `hookCount` /
    `graphNodeCount` / `eventSubCount` / `permissionToolCount`（`list()` 填充）；
  - 三个装载入口（native/builtin/plugin）统一记录**装载总耗时**（dlopen + create + start +
    注册收尾）并打印一行装载摘要：`Plugin `x` loaded in N ms: tools=.. hooks=.. graphNodes=..
    events=.. capabilities=.. permissionTools=.. (enable=..)`；
  - 接口声明（require/optional）随 `list()` 输出（协商结果本身在 client 侧判定，
    agent 侧只记录"插件声明了什么"）。
- **存储小接口**：`SessionStore::writerLeaseEnabled()` / `SessionStore::schemaVersion()`
  （快照展示用，均为只读）。

测试：

- 新同步模块 `assembly_snapshot`（37 项断言，`agent/test/core/test_assembly_snapshot.cpp`）：
  配置侧字段齐全、API key 与 header 取值不外泄、插件参数只出键名、渲染文本包含各小节、
  合并快照运行侧键优先。
- 新异步模块 `assembly_snapshot_io`（27 项断言）：真实 `CodeAgent::init()` 后的运行侧快照
  ——模型注册表、中间件顺序（含 `PermissionMiddlewareHandle` 与末尾 `LogPrint`）、
  工具来源（`agentxx_share_store`=builtin、至少一个 `middleware:*`）、MCP 命名空间前缀推断、
  未知名回退 builtin、执行图名称/节点数、持久化关闭时的 `persistence: OFF`、
  白名单过滤场景下被过滤记录的 `filter_reason`、渲染文本与日志摘要调用。

验证：

- 构建：lib `INSTALL`、`agentxx_test`、`agentxx_cli` 均 exit=0，无新增 error。
- 测试：`assembly_snapshot` 37/0、`assembly_snapshot_io` 27/0、`boundaries` 8/0。
- 手工验证：`agentxx_cli.exe --dump-config` 输出 13 个模型、6 层中间件顺序、17 个插件工具
  （来源逐项标注）、7 个插件的装载耗时与注册计数、图 `agentxx.default`（4 节点 5 边）、
  持久化根目录与 schema v2、AGENTS.md 缺失记入 `components.failed`。

注意事项 / 与计划的差异：

- 计划 ARC-6 提到"作业和队列状态"：作业整节不做（JOB 核定不考虑），队列状态是运行期
  按会话变化的量（`SessionQueueState`），不属于启动装配快照；未纳入。
- 计划 ARC-6 的"TUI 诊断页 / wire `get_diagnostics`"按限定范围不做；快照只走日志与 CLI。
- 工具来源中的 `dynamic`（插件动态注册表）与 `assembled`（进入执行图的静态工具）分开列出：
  插件工具不进图引擎静态工具集，混在一起会误导"哪个集合在参与冲突检测"。
- PLG-10 的"渲染错误计数"未做（快照不含 UI 渲染路径）；"接口协商结果"在 agent 侧只输出
  插件声明，因为 agent 侧装载不按接口集合拒绝（client 侧才做协商）。

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
### 阶段 M 补充：技能优先级与同名裁决（PRM-5，2026-10-06）

- **扫描目录带优先级与来源**（`SkillDirEntry{path, priority, source}`）：
  - 构造时传入的配置目录（项目/用户）= 优先级 0、来源 `config`；
    `addSkillDirs(paths)` 兼容旧调用方，按插件语义（优先级 100、来源 `plugin`）；
    新增 `addSkillDirs(paths, priority, source)` 供会话/项目级（0）、内置（200）等使用；
  - 目录列表按优先级稳定排序（数字小的优先），子目录扫描继承根目录的优先级与来源。
- **同名裁决**：加载完成后按优先级顺序遍历，同名技能只保留优先级最高的一份，
  其余从缓存移除并记入 `shadowedSkills_`（`{技能名, 被遮蔽目录, 生效目录}`）与警告日志；
  摘除高优先级目录后（重扫）由次高优先级接管。
- **来源展示**：技能清单每条新增 `source:` 行（无来源时显示 `builtin`），
  模型与 UI 都能看到技能来自哪一级；`skillDirPathList()` 保持原返回值形态（供测试/诊断）。
- 测试：`prompt_stability_io` 补用例（项目级 0 与插件级 100 同名技能并发；
  断言清单里高优先级正文出现、低优先级正文不出现、被遮蔽记录内容正确、
  摘除高优先级目录后低优先级接管），模块 32/0。

验证：`prompt_stability` 19/0、`prompt_stability_io` 32/0、`plugins` 541/0、
`plugin_resources` 89/0、`agent` 198/0、`summarization` 445/0。

## 全量回归与测试夹具修正（2026-10-06）

- 全量 `agentxx_test`（Debug + ASan/UBSan，全部模块）：**33868 项断言 0 失败**，进程 exit=0，
  无 AddressSanitizer 报告。
- 顺带修正两个与本次改动无关、但会污染全量回归的问题：
  1. `toolcall_parallel` 取消用例（T4）：被取消的慢工具（`test_can_slow`，原 5000ms 睡眠）
     内部计时器不随取消信号中止，用例返回后其协程继续运行并触碰已析构的工具对象
     （ASan: heap-use-after-free，`test_toolcall_parallel.cpp:107`）。修正：睡眠改为 800ms，
     并在用例结束前等待该工具执行体真正结束（上限 3s）。
  2. `filesystem` 模块 `autoSummaryOutput` 断言：读取类工具（list/read/glob/grep）在
     TOOL-2 起统一带只读 flags（自动摘要 + 并行安全），测试仍期望 `read` 为 `false`
     （旧预期），改为 `true` 与插件注册一致。

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

## 阶段 Q：按规范化路径排队执行（TOOL-16，2026-10-06）

计划依据：`plan.md` §5.2 TOOL-16（核定形态修正：**不做互斥锁对象**，改为"按规范化文件路径
排队执行（进程全局队列，与会话无关），保护读-改-写"，与 TOOL-1 工具并行化配套落地）。

已完成（`agent/plugins/agentxx_filesystem/filesystem_impl.h` 的 `detail` 段）：

- **路径门闩表 `PathLockTable`**（进程内按动态库共享）：
  - 表项 = `mutex + locked 标记 + 引用计数`；`acquireEntry` 取用（引用计数 +1），
    `release` 归还（引用计数归零且未加锁时立即删除表项）—— 不会随"历史上访问过的
    路径数"增长，`entryCount()` 供测试断言"用完即删"；
  - `Lease` 为 RAII 凭证：析构即解锁并归还表项，异常/取消路径同样释放。
- **规范化路径键 `pathLockKey`**：`weakly_canonical`（解析已存在部分的软链接与 `..`）+
  词法归一，使 `a/b/../x.txt`、`a//x.txt`、绝对路径等写法落到同一把门闩。
- **两种获取方式共用同一 `locked` 标记**：
  - `lockPathBlocking`（同步实现用：1ms 间隔重试，单文件操作毫秒级，等待时间可忽略）；
  - `lockPathAsync`（协程实现用：`steady_timer` 1ms 间隔重试，不阻塞 io 线程）。
- **接入点 4 处**：`fileWriteExecuteImpl` / `fileEditExecuteImpl`（同步兜底路径）与
  `fileWriteExecuteAsyncImpl` / `fileEditExecuteAsyncImpl`（协程路径）在执行前获取门闩，
  覆盖"存在性检查 + 写入"与"读-改-写整段"；只读工具（list/read/glob/grep）不获取门闩
  （它们本就声明并行安全，门闩只解决写-写 / 写-改冲突）。
- **与"多实例三铁律"的关系**（注释中写明）：门闩不是插件实例状态，而是"同一文件的互斥"
  —— 同一动态库内多个插件实例（多个 agent 宿主）写同一文件时**本就应当**排队，
  因此这里刻意共用一张表；表内只存瞬时状态、用完即删。

测试（`agent/test/core/test_filesystem_tools.cpp` 新增 `test_path_queue_serializes_read_modify_write`，模块 `filesystem` 147 项断言）：

- 路径键归一：三种等价写法同键、不同文件不同键；
- 门闩互斥：持有期间第二次获取不完成（等待 100ms 仍被挡住），释放后立即完成；
- 端到端：两次并发 `edit` 抢同一文件 → **恰好一次成功、一次报 `No match`**，
  文件内容等于其中一次的替换结果（不出现两次都成功而互相覆盖的丢失更新）；
- 不同路径互不阻塞（两个文件各写一次都成功）；同步实现共用同一门闩；
- 全部操作结束后 `entryCount() == 0`（表已清空）。

验证：

- 构建：`agentxx_test` 与插件目标 `agentxx_filesystem`（含内置合并形态）均 exit=0。
- 测试：`filesystem` 147/0、`command` 49/0、`boundaries` 8/0。
- **负面验证（断言确实能失败）**：临时注释掉协程 edit 的门闩获取后重跑，模块报
  `passed=145 failed=2`（`successCount == 2`、`noMatchCount == 0`），恢复后回到 147/0
  —— 即"去掉该机制时测试会红"。

注意事项 / 与计划的差异：

- 计划原文写"加短生命周期 mutex …不做全局无限增长 map"：实现保持"短生命周期表项 +
  引用计数归零即删"，但**同步与协程两种执行体共用同一个 `locked` 标记**（不是两套锁），
  因此同步路径与协程路径同时使用时也不会互相越过。
- 等待策略是"1ms 间隔重试"而非严格 FIFO 唤醒：单文件写操作在毫秒级，重试次数极小且
  行为更简单；不引入"队首票据 + 定时器唤醒"那套机制（避免取消路径下的悬挂票据问题）。
- 门闩只覆盖内置 filesystem 插件的写/改工具；其他写文件的路径（如命令执行）不在其内，
  属已知边界（它们不共享同一实现）。
## 阶段 R：分阶段关闭与后台任务收敛（ARC-5，2026-10-06）

计划依据：`plan.md` §1.2 ARC-5（人工核定：**不等待当前轮次结束**）：`shutdownAsync` 依次
停止输入、停定时器、停插件、刷盘；新增轻量 `TaskScope` 统一记录后台任务并在关闭时取消/等待。

已完成：

- **`TaskScope`**（新增 `agent/lib/{include/agentxx/util/task_scope.h,src/util/task_scope.cpp}`）：
  - `spawn(name, awaitable)`：登记并启动一个 fire-and-forget 后台任务（每个任务一个
    `asio::cancellation_signal`，取消信号绑定到任务）；完成处理器摘除登记项，并把
    后台任务的异常在边界内分类记录（**不让异常逃逸到 detached 处理器终止进程**）；
  - `pending()` / `pendingNames()` / `totalSpawned()`（诊断与测试可观测）、
    `cancelAll()`（向全部运行中任务发取消信号，返回条数）、
    `awaitIdle(timeout)`（2ms 轮询等待收敛，超时返回 false —— 不假装成功）；
  - 边界刻意收窄（见头文件说明）：**不接管当前轮次**（轮次由调用方 await，等待会拖长退出）、
    **不接管插件任务**（由插件 `stop` 负责）；只收 fire-and-forget 的短任务。
- **接入点**（真实后台任务的登记）：
  - `EventBridge::publishModelToken` / `publishError`（`lib/src/event/event_stream.cpp`）：
    有登记表时经 `spawn("event-publish", ...)`，否则退回原 `co_spawn(detached)`；
  - `AgentHost::publishProgress`（`lib/src/agent/agent_host.cpp`）：经根 agent 的登记表
    `spawn("host-progress", ...)`。
- **`AgentContext`**：新增 `taskScope`（init 的 `event_bus` 步骤创建，与事件总线同生命周期）
  与 `markShuttingDown()` / `isShuttingDown()`（原子标志，端点可能在任意线程读）。
- **分阶段关闭 `BaseAgent::shutdownAsync(timeout)`**（`lib/src/agent/base_agent.cpp`）：
  1. **停止受理新输入**：置位"正在关闭"；`SessionServerAgentIO::handleUserInput` 在该标志
     置位时回 `WireInputAck(rejected, server_stopped, "agent is shutting down")`
     （与端点自身 `stop()` 的拒绝相互独立）；
  2. **后台任务取消并收敛**：`cancelAll()` + `awaitIdle(剩余预算)`，日志给出
     `pending/cancelled` 与"是否仍在跑"；
  3. **关闭插件**：沿用既有契约（stop → lease 归零 → destroy/dlclose，失败保留 CloseFailed）；
  4. **刷盘**：对每个已加载会话 `persistNow("shutdown")`（上下文 + 展示历史），
     经插件管理器记录的 io executor 执行（会话状态只在 agent io 线程访问）。
  总耗时受 `timeout` 约束（每阶段用"剩余预算"）；**全过程不等待也不取消正在跑的轮次**。
- 新增 `SessionsManager::all()` / `size()`（刷盘遍历与诊断；`SessionsManager` 原本只有 get）。

测试：

- 新同步模块 `task_scope`（12 项断言）：立即完成任务的摘除与计数、长定时器任务取消后
  `operation_aborted` 收尾、`awaitIdle` 的"已空闲返回 true / 未收敛超时返回 false"两条路径、
  `pendingNames` 清空、累计发起计数。
- 新异步模块 `shutdown_stages`（17 项断言，真实 `CodeAgent` + 会话端点 + 本地 LLM 模拟器）：
  - **停止输入 + 刷盘**：跑一轮真实对话 → `shutdownAsync(3s)` 返回 true 且耗时远小于超时 →
    `isShuttingDown()` 为真 → 关闭后新输入收到 `rejected` + `server_stopped` 回执 →
    用**另一个 `SessionStore`** 打开同一目录能读到关闭前的展示历史与 LLM 上下文
    （证明阶段 ④ 真的落盘，而不是只改了内存）；
  - **不等待当前轮次**：模型延迟 1200ms 期间调用 `shutdownAsync(500ms)`，返回耗时 < 900ms
    且此刻**尚无轮次结果**；随后轮次自行结束（未被取消）—— 证明关闭不等轮次也不打断它。

验证：

- 构建：lib `INSTALL`、`agentxx_test`、`agentxx_cli` 均 exit=0，无新增 error。
- 测试：`task_scope` 12/0、`shutdown_stages` 17/0、`agent` 198/0、`plugins` 541/0、
  `plugin_resources` 89/0、`remote_agent` 453/0、`input_delivery` 78/0、`persist_semantics` 25/0、
  `session_persistence` 621/0、`boundaries` 8/0。

注意事项 / 与计划的差异：

- 计划的"停定时器"在本项目里没有 agent 自持的定时器需要单独停止：会话落盘是同步写入
  （节流是时间戳判断而非定时器），定时器都属插件（由插件 `stop` 随阶段 ③ 处理）或
  工具调用内的短计时。因此阶段 ② 的职责落在"后台任务取消与收敛"上，未单列停定时器。
- 计划提到"插件卸载/取消等待"在阶段 ③ 以前已有实现（`PluginManager` 生命周期骨架），
  本次只是把它排进分阶段序列并加上剩余时间预算。
- 后台任务的覆盖是**渐进**的：目前登记的是事件发布与宿主进度通知两类；子代理轮次
  （`AgentHost::spawnBatch` 的派生协程）仍由调用方/取消级联负责，未迁入登记表
  （它们不是"可丢弃的短任务"，迁入需要单独设计取消语义）。
## 阶段 S：界面能力段的体验级别字段（UI-9，2026-10-06）

计划依据：`plan.md` §10 UI-9（低成本可做）："除组件 kind 外声明表单多字段提交、取消、
布局尺寸、终端能力"。

已完成（`agent/client/include/agentxx-client/io/tui/tui_plugin_adapter.h`）：

- `uiCapabilitiesJson()` 在描述层 `capabilitiesToJson(tuiUiCapabilities())` 的基础上
  **追加**三段体验级别字段（只增不改，未知键插件忽略即可，描述层解析不受影响）：
  - `form`: `multi_field`（一次提交多个控件值）/ `submit_cancel`（`__submit`、`__cancel`）
    / `commit_on_pick`（中断一问一答点击即提交）/ `validation`（控件下方校验提示）；
  - `layout`: `auto` / `fixed` / `scroll`（内容超高可滚动）；
  - `terminal`: `truecolor` / `mouse` / `wide_chars`（CJK 双宽测量）/ `hardware_cursor`
    —— 最后一项**如实上报 false**（UI-5 输入栏硬件光标待实施，不虚报能力）。
- 组件/控件能力仍只有一处来源（`tuiUiCapabilities()`），插件读到的能力与渲染前
  `adaptItems` 使用的能力不会分叉（测试断言两者一致）。

测试（新同步模块 `ui_capabilities`，26 项断言，`agent/test/client/test_ui_capabilities.cpp`）：

- 描述层字段与 `tuiUiCapabilities()` 同源（apiVersion / kind / blocks / controls /
  cell / gap，及 `colsOf` 换算口径）；
- 三段体验字段取值齐全；`hardware_cursor` 为 false；
- `capabilitiesFromJson` 能解析含新增字段的 JSON（字段只增不改、不破坏描述层）。

验证：

- 构建：`agentxx_test` exit=0；测试：`ui_capabilities` 26/0、`client_plugins` 657/0、
  `ui_kit` 167/0、`tui_widget` 129/0、`ui_snapshot` 22/0。

注意事项 / 与计划的差异：

- 体验字段加在**客户端的 JSON 输出**里，没有改 `cxx_pluginxx_ui` 的 `Capabilities` 结构
  （第三方库不宜为宿主体验字段改动；描述层只解析自己认识的键）。
- `hardware_cursor` 目前固定 false；UI-5 实施后改为按终端能力上报。
## 本轮全量回归（2026-10-06，阶段 N~S 完成后）

- 一键门禁（`pwsh -File agent/script/gate.ps1`，Debug + ASan）：
  **34,049 项断言 0 失败**，汇总输出 `[gate] OK`（上一轮基线 33,868）。
- 新增/改动模块单独复核：`assembly_snapshot` 37/0、`assembly_snapshot_io` 27/0、
  `config_validation` 47/0、`boundaries` 8/0、`ui_snapshot` 22/0、`filesystem` 147/0、
  `task_scope` 12/0、`shutdown_stages` 17/0、`ui_capabilities` 26/0。
- 受影响模块回归：`agent` 198/0、`plugins` 541/0、`plugin_resources` 89/0、
  `remote_agent` 453/0、`input_delivery` 78/0、`persist_semantics` 25/0、
  `session_persistence` 621/0、`session_schema` 91/0、`config_loader` 375/0、
  `client_plugins` 657/0、`ui_kit` 167/0、`tui_widget` 129/0、`tui_ui_items` 203/0、
  `ui_items` 117/0、`command` 49/0、`worktree` 0/0（须在工作目录下运行）。
- 产物构建：lib `INSTALL`、`agentxx_test`、`agentxx_cli`、`agentxx_filesystem` 插件目标均 exit=0。
- 手工验证：`agentxx_cli --dump-config` 输出配置侧 + 运行侧装配快照（13 模型 / 6 中间件 /
  17 插件工具 / 7 插件装载耗时 / 图 / 持久化 / 配置问题 2 条），退出码 0。
## 阶段 T：协议版本与能力握手（PRO-3，2026-10-06）

计划依据：`plan.md` §11 PRO-3（"Hello/HelloAck 增加 protocolVersion、能力列表、可选消息；
不支持时明确降级或拒绝"）。

已完成：

- **协议版本与能力常量**（`agent/lib/include/agentxx/agent/io/agent_io_transport.h`）：
  - `WireProtocol`：`kVersion = 1` / `kMinVersion = 0`，以及能力名常量
    （`input.delivery`、`input.ack`、`queue.state`、`view.after_seq`、`session.search`、
    `ui.items`、`ui.form`、`ui.panels`）；`serverWireCapabilities()` 给出服务端声明集；
  - `WireHello` 增 `protocolVersion`（默认当前版本）与 `capabilities`；
  - `WireHelloAck` 增 `protocolVersion`、`capabilities` 与 `error`（拒绝原因，老客户端忽略）。
  - 语义约定写进注释：**版本只在破坏性变更时递增；对端更高时明确拒绝而不是静默降级**
    （静默降级会让新客户端误以为老服务端支持某能力）；缺失字段 = 老客户端（版本 0），
    服务端按最低兼容版本继续（向后兼容）。
- **JSON 编解码**（`lib/src/agent/wire_protocol.cpp`、`lib/include/.../wire_protocol.h`）：
  `makeHello` / `makeHelloAck` 增参数（默认值保持旧调用点零改动），`helloFromJson`
  缺字段 → 版本 0 / 空能力，`helloAckFromJson` 同样按缺省解析。
- **服务端握手**（`lib/src/agent/io/session_server_agent_io.cpp`）：
  `handleHello` 开头做版本检查：`> kVersion` 时回 `WireHelloAck{ok=false, error="client
  protocol version N is newer than server version M; please update the server side",
  protocolVersion=kVersion, capabilities=serverWireCapabilities()}` 并直接返回（不建立同步）；
  成功回执同样携带版本与能力声明。
- **客户端**：
  - `WsAgentIOTransport`：首次 `connect` 与**重连**都携带协议版本与能力（重连复用首次
    连接时的声明）；收到 `ack.ok == false` 时握手按失败处理（`connect()` 返回 false，
    原因经 `lastHelloAck().error` 暴露）—— 之前只认"收到 HelloAck"就当作成功，
    鉴权失败/版本不匹配时会让调用方在错误前提下继续工作；
  - TUI/CLI 的 `WireHello`（`client/src/mode_runners.cpp`）声明版本与各自能力
    （TUI 额外声明 ui.items / ui.form / ui.panels）。

测试：

- `wire_roundtrip`（+16 项断言，模块 210/0）：`WireHello` 版本+能力往返；`WireHelloAck`
  版本+能力+error 往返；**老客户端形态**（删掉 `protocolVersion` / `capabilities` 字段的
  JSON）解析为版本 0 与空能力；拒绝回执的 `error`/版本/能力逐项比对。
- `remote_agent`（+21 项断言，模块 474/0）：
  - 假服务端升级为"与真实服务端同语义"（版本检查 + 回执声明能力）；
  - 客户端侧：正常握手回执带版本与能力、老客户端（version 0）照常成功、
    版本高于服务端时 `connect()` 返回 false 且 `lastHelloAck()` 给出 `newer` 原因；
  - 真实 `AgentServer`（channel transport）：成功回执带版本与能力；版本更高的
    Hello 被明确拒绝且**不影响其它已连客户端**。

验证：

- 构建：lib `INSTALL`、`agentxx_test`、`agentxx_cli` 均 exit=0。
- 测试：`wire_roundtrip` 210/0、`remote_agent` 474/0、`websocket` 230/0、`mcp` 391/0、
  `acp` 56/0、`a2a` 177/0、`cancel` 45/0、`boundaries` 8/0。

注意事项 / 与计划的差异：

- 计划的"可选消息"能力没有单列：能力列表本身就是"对端可选支持什么"的声明，
  未声明即按最保守路径工作（如不请求 `afterViewSeq` 补拉）。
- 版本检查只在服务端做（客户端对服务端版本更低的情况没有额外处理：老服务端缺字段
  解析为 0，客户端按"对端未声明能力"降级，不拒绝）。
- 客户端 `connect()` 现在会在 `ack.ok == false` 时返回 false；这是行为变化（此前
  "收到 HelloAck 即视为连接成功"），属修正：鉴权失败不再表现为"连上了但没有响应"。
- 阶段 R~T 提交后再跑一次全量门禁：**34,115 项断言 0 失败**，`[gate] OK`
  （相对阶段 P 时的 34,049 又新增 66 项：ARC-5 29、UI-9 26、PRO-3 37，去重后为 66）。

## 阶段 U：假 provider 接缝与迁移中断用例（LLM-5 / TST-1 / TST-3，2026-10-07）

计划依据：`plan.md` §6 LLM-5（"provider 可注入固定流、错误、延迟和 tool call；覆盖重试、
压缩、中断、取消和工具循环，不依赖真实网络或额度"）、§15 TST-1（同一目标的测试项）、
§15 TST-3（持久化迁移/恢复测试余项）。这两项是 work.md"待完成"里剩下的 P0 条目。

已完成：

- **假 provider 夹具**（新增 `agent/test/include/agentxx-test/core/fake_provider.h`）：
  - `FakeStep` 脚本条目: `Text` / `ToolCalls` / `Error` 三类, 可带 `delayMs`（可被取消中断）、
    `chunkChars`（流式分片, 按 UTF-8 边界切）、`thinking`（思考片段）、用量字段;
  - `FakeProvider` 实现 `neograph::Provider`: `invoke_format_data` 按脚本产出（思考片段走
    `TYPE_THINKING`、正文走 `TYPE_CONTENT`）、`complete_async` 复用同一路径、`get_name()`
    返回 `fake`; 脚本用尽后回 `defaultText`（默认 `fake response`）;
  - 记录能力: `requestCount()` / `requestAt(i)` / `requests()` / `lastRequestRoles()` /
    `cancelObserved()`, 供请求体结构、重试次数与取消断言;
  - 注入方式 `FakeProvider::inject(agent, modelName, provider)`（内部走
    `ModelProviderRegistry::setProvider`, 与真实 provider 同一调用路径, 需在 `init()` 之后注入）;
  - 等待期间轮询 `params.cancel_token->is_cancelled()` 并捕获协程取消（`operation_aborted`）,
    命中即抛 `neograph::graph::CancelledException` —— 与真实 provider 的"取消中断在途请求"同语义。
- **假 provider 用例**（新模块 `fake_provider`, 36 项断言, 6 组）：
  - 固定流: 正文分片拼接结果与脚本完全一致（含 CJK/多字节字符）、思考片段不混入正文、
    请求体首条是 system、工具 schema 随请求下发、模型名正确;
  - 工具循环: `tool_call` → 自定义回显工具真实执行 → 第二次请求带回 `echo:hello` 的 tool 消息
    → 以最终回答结束;
  - 错误分类: 401 只请求一次即结束（不可重试）; 500 + `retry_after` 按退避等待约 1 秒后第二次成功;
  - 溢出压缩: 400 + `context_length_exceeded` 触发一次压缩后重试成功（请求数 ≤ 4 而非重试耗尽）,
    且摘要正文确实进入会话语义上下文;
  - 取消: 5 秒延迟的响应在取消后 < 2 秒结束, 且假 provider 确实在等待期间观察到取消;
  - 用量记账: 假 provider 上报的用量（123/45/7/3）进入会话账本 `usage` 表汇总。
- **迁移中断用例**（`agent/test/core/test_session_schema.cpp` 新增 D2 段, 模块 91→104 项断言）：
  用"与 `session_input` 同名的视图"制造迁移失败（SQLite: views may not be indexed），验证
  ① 写路径明确失败（`lastWriteError()` 非空）且不落数据；② `schema_version` 未推进（仍是老库）、
  `usage` 表与 `msg_id` 列未留半成品（事务回滚）；③ 老数据仍在；④ 排除故障（删视图）后重新
  打开可续做, 迁移完成且历史与新写入都保留。

验证：

- 构建：`agentxx_test` exit=0（新增测试源文件后需重跑一次 CMake 配置, 见"注意事项"）。
- 测试：`fake_provider` 36/0、`session_schema` 104/0、`agent` 198/0、`usage_ledger` 21/0、
  `cancel` 45/0、`boundaries` 8/0。

注意事项 / 与计划的差异：

- 计划 LLM-5 提到"录制回放在同一注入接缝中作为后续 P1 扩展": 本次只做**注入**这一半,
  HTTP 录制回放仍是 LLM-13（待实施）。
- 假 provider 不替换本地 HTTP 模拟服务器（`DaSimServer`）: 后者覆盖 provider 内部的
  SSE/JSON 解析与错误结构, 两者互补; 新用例覆盖的是"provider 之上"的行为（重试策略、
  压缩、取消、工具循环、记账）。
- 新增 `agent/test/core/*.cpp` 或 `agent/test/client/*.cpp` 后必须重跑一次 CMake 配置
  （测试工程按 `file(GLOB ...)` 收集源码）:
  `cmake -S agent/test -B <build>/agentxx_test_repo-prefix/src/agentxx_test_repo-build`,
  否则表现为 `LNK2019 无法解析的外部符号`。

## 阶段 V：Wire 协议字段清单与连接阶段（PRO-4 / PRO-5 / TST-2，2026-10-07）

计划依据：`plan.md` §11 PRO-4（"先生成 `wire-schema.json` 和字段文档，再考虑 C++ 编解码/TS
绑定；不把整个项目改成代码生成"）、PRO-5（"`unhandshaken/unbound/ready/reconnecting/draining`
状态；明确 SessionNotFound、MessageNotFound、InvalidState"）、§15 TST-2（生成 schema 与实现对比）。

已完成：

- **PRO-4 协议字段清单（生成物 + 新鲜度门禁，新模块 `wire_schema`，945 项断言）**：
  - 每个 `WireMessage` 变体成员准备一个示例实例（顺序必须与变体一致），序列化后按
    "字段名 → JSON 类型" 生成两份生成物：
    `agent/schema/wire-schema.json`（协议版本 / 消息数 / 每条消息的 type、字段类型、示例值）
    与 `docs/zh-cn/design/wire-protocol-fields.md`（人工阅读的字段表 + 错误码/连接阶段说明）；
  - 门禁: 生成结果与仓库内生成物**逐字节比对**, 不一致即失败并打印首个不同行
    （提示 `AGENTXX_UPDATE_WIRE_SCHEMA=1` 重新生成）; 与 UI 快照同一套"一键更新 + 人工 review"做法;
  - 覆盖度断言: 示例数量必须等于 `std::variant_size_v<WireMessage>` 且每条示例的变体下标
    与其位置一致 —— 新增消息类型却忘记补示例时立即失败; 另断言每条消息序列化后 `type` 非空、
    互不重复、反序列化回到同一变体成员 (TST-2: 生成 schema 与实现一致性);
  - 生成物已纳入 `docs/zh-cn/design/index.md` 的相关文档入口, 并在 CMake 里用
    `AGENTXX_WIRE_SCHEMA_PATH` / `AGENTXX_WIRE_SCHEMA_DOC_PATH` 注入路径。
- **PRO-5 连接阶段与错误分类**：
  - 新增 `WireConnectionStage`（`Unhandshaken` / `Unbound` / `Ready` / `Reconnecting` /
    `Draining`）与 `wireConnectionStageText` / `wireConnectionStageFromText`（未知文本返回
    `nullopt`, 由调用方兜底; 认不出阶段时按最保守的"未就绪"看待）;
  - `AgentIOTransportBase` 新增 `stage()` / `setStage()`: 进程内 Channel 传输默认恒为
    `Ready`（建立即可用）, 因此同进程宿主与既有用例行为不变;
  - `WsAgentIOTransport` 实现真实流转: 构造即 `Unhandshaken` → 握手成功带 sessionId 为
    `Ready`（未带 sessionId 为 `Unbound`）→ 断线 `Reconnecting` → 重连成功后收到 HelloAck
    回 `Ready` → `close()` 为 `Draining`; 阶段变化记 Info 日志（含原因）;
  - 端点（`SessionServerAgentIO`）: `onPeerMessage` 入口对**未握手**的传输拒绝除 `hello`
    之外的业务消息并回 `WireError(InvalidState)`（不再按正常流程处理）; `handleHello` 成功
    时把该传输推进到 `Ready`, 版本被拒时保持 `Unhandshaken`;
  - 新增错误码 `WireErrorCode::MessageNotFound = 5`: 删除不存在的消息队列条目时明确回错误
    （此前静默成功, 客户端无法区分"删掉了"与"条目已不在"）。
- **测试**：
  - 新模块 `wire_schema`（945 项断言）: 覆盖度、type 唯一性与往返、生成物比对（含更新模式）;
  - `input_delivery` 新增 `test_connection_stage_guard`（78→97）: 未握手时业务消息被拒
    （回 `InvalidState` 且不入队）、hello 后传输被推进到 `Ready`、之后同一传输的输入被受理
    并回带 requestId 的受理回执、删除不存在条目回 `MessageNotFound`、阶段文本往返与未知文本兜底;
  - `remote_agent` 的 WS 回环用例补 3 项断言（474→477）: 连接前 `Unhandshaken`、握手后
    `Ready`、关闭后 `Draining`。

验证：

- 构建：lib `INSTALL`、`agentxx_test` 均 exit=0，无新增 error。
- 测试：`wire_schema` 945/0、`input_delivery` 97/0、`remote_agent` 477/0、`wire_roundtrip` 210/0、
  `session_sync` 30/0、`boundaries` 8/0、`fake_provider` 36/0、`session_schema` 104/0。
- 生成物自检：`AGENTXX_UPDATE_WIRE_SCHEMA=1` 生成后, 比较模式再次运行全部通过。

注意事项 / 与计划的差异：

- 计划提到"再考虑 C++ 编解码/TS 绑定": 本次只做**生成物 + 新鲜度门禁**, 不引入代码生成
  （编解码仍是手写实现, 生成物只是可读契约）;
- 生成物暴露了几处历史命名不一致（`delta` 的 `tool_name`/`tool_call_id`、`sync` 的
  `message_queue`/`queue_state` 为下划线命名, 其余为驼峰）: 属既有线上格式, 改动会破坏
  兼容性, 仅记录在字段清单里, 不改协议;
- `MessageNotFound` 只用在删除队列条目这一条路径（其他"按 id 定位"的请求目前都是幂等语义
  或回空页), 后续新增按 id 定位的请求可复用该码。

## 阶段 W：会话检索与改名的协议 / 端点 / TUI 入口（RET-1a / STO-12b，2026-10-07）

计划依据：`plan.md` §14 RET-1（拆条结果中的 **RET-1a：协议 + TUI 入口**，与 RET-2 同批）、
§13 STO-12（存储层已实施，界面入口待接）、§16.2 批次 E（"RET-1a（协议 + TUI 搜索/改名入口，
与 RET-2 同批）"）。

已完成：

- **协议（lib）**：
  - `WireListSessions` 增 `keyword` 字段（空 = 普通分页/全量列举；非空 = 检索）；
  - 新增两条消息：`WireRenameSession{sessionId, title}`（客户端→服务端）与
    `WireRenameSessionResult{sessionId, ok, title, error}`（服务端→客户端），
    含 `MsgType` 常量、`make*/fromJson` 编解码、变体成员（**追加在变体末尾**，既有下标不变）
    与 `serialize`/`deserialize` 分派；
  - `SessionInfo` 增 `snippet` 字段（检索命中片段；普通列表为空时不出现在 JSON 里，
    老客户端不受影响）；
  - `AgentIOBase` 增 `requestRenameSession(sessionId, title)`，`requestSessionListPage`
    增可选 `keyword` 参数（默认空，旧调用点零改动）。
- **服务端（SessionServerAgentIO）**：
  - 会话列表取数收敛为 `listSessionsFor(req, store)` 一处：关键词检索（`searchSessions`，
    命中片段随 `snippet` 回传、不参与分页续取）→ keyset 分页 → 全量列举；阻塞 I/O 仍卸载到
    线程池；
  - 新增 `handleRenameSession`：标题规范化（去首尾空白、换行折叠为空格、按 UTF-8 边界限长
    120 字符）→ 校验（空标题 / 会话不存在 / 无持久化各回可读原因）→ 写
    `SessionStore::setSessionTitle`（同时把来源标为 `user`，此后不被自动标题覆盖）→
    回执；**会话不存在时不建目录**（改名不是新建会话的入口）。
- **客户端 / TUI**：
  - `TUIRenderState` 增 `sessionListKeyword`，`TUICtx` 增 `requestSessionSearch` /
    `renameSession` 回调；`TUIClientAgentIO` 增 `requestSessionSearch`（重置列表为 loading
    并按关键词重发请求）、`onSessionRenameResult`（成功就地改本地列表标题、失败 toast 原因）、
    以及在握手回执分支旁新增的 `WireRenameSessionResult` 分发；
  - `SessionSelectorOverlay`：列表上方新增检索行（**直接输入字符即检索**，Backspace 删除，
    Esc 先清空关键词再关闭）；`Ctrl+R` 进入/退出**改名编辑态**（输入标题，Enter 提交，
    Esc 取消，编辑态独占键盘不触发检索）；条目第二行在正文命中时显示命中片段而不是时间；
    检索状态下不预取下一页。i18n 补 `session.searchPrompt` / `session.searchHint` /
    `session.renamePrompt` / `session.renameFailed` 与底栏提示（中英双语）。
- **测试**：
  - 新模块 `session_admin`（38 项断言）：真实 agent + 会话端点 + 客户端传输，
    覆盖改名成功（落库 `meta.title` + 来源 `user`）、空标题被拒且保留原标题、不存在的会话
    被拒且**不建目录**、标题检索（只回命中项、无片段）、正文检索（片段含关键词）、
    协议入口检索、空关键词回到普通列表（标题为改名后的值）、无持久化时改名明确失败；
  - `wire_roundtrip` 210→233 项：`WireRenameSession` / `WireRenameSessionResult`（成功与
    失败两种形态）、`WireListSessions.keyword`、`SessionInfo.snippet`（含缺省形态）；
  - `tui_surface` 用例新增检索/改名交互断言（打开时沿用上次关键词、输入触发检索回调、
    Backspace 按 UTF-8 退格、Esc 先清空再关闭、Ctrl+R 进入编辑态且编辑态不触发检索、
    Esc 取消不改名、Enter 提交回调收到 (sessionId, 新标题)），模块 673/0;
  - `wire_schema` 生成物随新消息重新生成（门禁要求，一并提交）。

验证：

- 构建：lib `INSTALL`、`agentxx_test`、`agentxx_cli` 均 exit=0。
- 测试：`session_admin` 38/0、`wire_roundtrip` 233/0、`wire_schema` 1034/0、`tui_surface` 673/0、
  `input_delivery` 97/0、`remote_agent` 477/0、`session_schema` 104/0、`session_persistence` 621/0、
  `tui_settings` 552/0、`tui_widget` 129/0、`ui_snapshot` 22/0、`client_plugins` 657/0。

注意事项 / 与计划的差异：

- 检索**不做分页续取**：`SessionStore::searchSessions` 一次返回上限内的命中项
  （默认 50，可按请求 limit 调整），命中数很大时靠关键词收敛；真出现慢查询再按
  RET-1b（FTS5，已核定不做）评估。
- 改名只改 `meta.title`：不改会话目录名/sessionId（目录名是会话身份，改名会打断已建立的
  连接与前端缓存）；标题来源标 `user` 后自动标题不再覆盖（存储层原有语义）。
- 本地模拟器按**字节**切片推送 SSE 片段：用例里的 LLM 回复正文改用 ASCII（多字节字符会被
  切在中间），用户输入仍用中文 —— 这是测试夹具限制，不是产品行为（真实 provider 不会
  在字符中间切片）。
- TUI 的改名入口是 `Ctrl+R`（而不是可打印字符键）：检索行直接吃字符输入，用组合键避免
  与检索输入冲突。

## 阶段 X：单次 provider 调用取消域与测试隔离收尾（LLM-7 / TST-9，2026-10-07）

计划依据：`plan.md` §6 LLM-7（"流对象析构或消费方放弃时 RAII 取消 provider，避免连接继续
占用"）、§15 TST-9（"临时 HOME/TMP/XDG、清凭据环境；每模块打印耗时/用例数；路径和 key 脱敏"）。

已完成：

- **LLM-7 单次调用取消域**（新增 `agent/lib/include/agentxx/nodes/provider_call_scope.h`）：
  - `ProviderCallScope`：用 `CancelToken::fork()` 为**每次** provider 调用建立子令牌 ——
    运行级取消照常级联到子令牌（用户取消/关停语义不变），而子令牌单独取消只中止本次调用；
  - RAII 契约：调用正常收尾（成功或错误路径）必须 `markDone()`；未标记就析构（消费方放弃、
    协程帧被销毁）时析构函数取消子令牌，让在途请求尽快结束而不是把响应读完；
  - `nodes/modelcall.cpp` 接线：`params.cancel_token` 由"运行令牌"改为本次调用的子令牌，
    `co_await` 后与异常路径都标记完成（错误路径不算放弃）；
  - 边界：不做超时竞速（TOOL-4 分发层硬超时经人工核定不做），只提供取消域。
- **TST-9 测试隔离与脱敏**：
  - 每模块打印耗时（`--- 模块 done: passed=N failed=M (T ms) ---`，同步与异步两条运行器都改）；
  - 启动时清除常见模型 API 凭据与端点覆盖环境变量（`clearCredentialEnv`，仅清凭据类，
    不动 PATH/HOME 等运行必需变量），使"意外联网"直接失败而不是悄悄产生费用；清理结果打印
    条数与变量名（不打印取值）；
  - 新增 `redactSecret` 脱敏助手（保留首尾少量字符，其余以 `*` 代替），供测试日志/断言信息
    打印可能含凭据的值；测试临时目录隔离（独立 `agentxx_*_test_*` 目录）此前已实施。
- **测试**：新同步模块 `provider_call_scope`（18 项断言）：父令牌取消级联、调用域单独取消
  不影响父令牌、同一轮次多个调用域互不影响、`markDone` 后析构不取消、未 `markDone` 析构即取消、
  父令牌为空时的独立令牌、父令牌已取消时新建调用域立即处于取消态。

验证：

- 构建：lib `INSTALL`、`agentxx_test` 均 exit=0。
- 测试：`provider_call_scope` 18/0、`fake_provider` 36/0、`agent` 198/0、`cancel` 45/0、
  `toolcall_parallel` 48/0、`summarization` 445/0、`usage_ledger` 21/0、`shutdown_stages` 17/0。

注意事项 / 与计划的差异：

- 计划把 LLM-7 描述为"流对象析构或消费方放弃时 RAII 取消 provider"：本项目的 provider 调用
  没有独立"流对象"（就是一次 `co_await`），因此实现落在**调用域**上；协程帧被销毁时 asio
  本身也会取消其挂起操作，这里的 RAII 取消是与之并列的一道保险（并覆盖"帧仍活着但消费方
  不再等待"的情形）。
- TST-9 的"路径脱敏"未做：测试失败信息里的路径都是临时目录/仓库路径，不是敏感信息；
  真正需要脱敏的是凭据取值（已有 `redactSecret`，且 `assembly_snapshot` 断言不落凭据）。

## 本轮全量回归（2026-10-07，阶段 U~X 完成后）

- 全量 `agentxx_test`（Debug + ASan/UBSan，全部模块）：**35,327 项断言 0 失败**，进程 exit=0，
  无 AddressSanitizer 报告（阶段 P 基线 34,049 → 阶段 T 34,115 → 本轮 35,327）。
- 新增模块：`fake_provider` 36、`wire_schema` 1034、`session_admin` 38、`provider_call_scope` 18；
  既有模块增量：`session_schema` 91→104（迁移中断）、`wire_roundtrip` 210→233（重命名/检索/
  片段）、`input_delivery` 78→97（连接阶段校验）、`remote_agent` 474→477（WS 连接阶段）、
  `tui_surface` 630→673（会话弹窗检索/改名交互）。
- 产物构建：lib `INSTALL`、`agentxx_test`、`agentxx_cli` 均 exit=0（无新增 error）。
- 生成物新鲜度：`wire_schema` 比较模式通过（`agent/schema/wire-schema.json` 与
  `docs/zh-cn/design/wire-protocol-fields.md` 与实现一致，改动协议时忘记重新生成会直接失败）。
- 阶段 Y（接口表数量门禁）之后复跑全量：**35,328 项断言 0 失败**（`boundaries` 8→9）。
- 阶段 Z（TST-4 / TST-6 / TST-7）完成后提交：
  `竞态清单、存储一致性骨架与门禁扩展 (TST-4/TST-6/TST-7)`（`boundaries` 9→11、
  新增模块 `storage_consistency` 260 项、`race_guards` 68 项）。
- 阶段 AA（PLG-1 / PLG-2 / PLG-4 / PLG-7）完成后提交：
  `插件注册清单、执行图独占 slot 与装载失败建议 (PLG-1/PLG-2/PLG-4/PLG-7)`
  （新增模块 `plugin_cleanup` 96 项）。
- 阶段 AB（UI-1 / UI-5）完成后提交：
  `客户端模型层（历史窗口/队列镜像）与输入栏硬件光标 (UI-1/UI-5)`
  （新增模块 `tui_model` 134 项、`boundaries` 12 项）。
- 阶段 AC（OBS-3 / OBS-4 / STO-13）完成后提交：
  `关键指标与诊断包导出 (OBS-3/OBS-4/STO-13)`（新增模块 `observability` 96 项）。
- 阶段 AD（PRO-8）完成后提交：
  `JSONL stdio 一次性运行模式 (PRO-8)`（新增模块 `jsonl_mode` 65 项、`jsonl_runner` 16 项）。
- 阶段 AE（LLM-8）完成后提交：
  `Anthropic prompt 缓存断点与缓存用量入账 (LLM-8)`（模块 `anthropic_provider` 286、
  `fake_provider` 46、`session_schema` 121、`config_loader` 381）。
- 阶段 AF（LLM-13）完成后提交：
  `HTTP 录制回放夹具 (LLM-13)`（新增模块 `http_recorder` 81 项）。
- 阶段 AG（CFG-9）完成后提交：
  `配置键目录生成与新鲜度门禁 (CFG-9)`（新增模块 `config_keys` 157 项；顺带修掉
  `config_loader` 的小数截断问题并补 26 项断言）。
- 阶段 AH（OBS-5）完成后提交：
  `按模块日志级别 (OBS-5)`（新增模块 `log_modules` 35 项；全量回归 36,438 项断言 0 失败）。

## 阶段 Y：接口表数量与文档一致性校验（PLG-8 部分 / TST-7，2026-10-07）

计划依据：`plan.md` §12 PLG-8（"插件文档分页与接口表数字校验"）、§15 TST-7（"边界/导出/清理
门禁：依赖方向、DSO 白名单、**接口表集合**、UI block 名、插件注册清理和文档路径"）。

已完成：

- **数量常量**：`PluginManager::kInterfaceTableCount`（19 = 10 张通用表 `pluginxx.*` + 9 张
  agent 领域表 `agentxx.agent.*`）与 `ClientPluginManager::kInterfaceTableCount`
  （9 = 7 张基础表 `agentxx.client.*` + 2 张交互表 timer/keybind），注释写明"增删接口表时
  要同步文档"。
- **门禁规则**（`boundaries` 模块新增规则 8「接口表数量」）：数量常量必须等于 19 / 9，且
  ① `docs/zh-cn/design/plugins.md` 必须写明 `<n> 张 agent` 与 `<n> 张 client`；
  ② 仓库根 `AGENTS.md` 必须写明两侧数量；文档文件缺失同样判失败（避免规则静默失效）。
  扫描量下限与失败输出沿用既有 `reportViolations`（打印条目数 + 首若干条）。
- **文档澄清**：`plugins.md` §8 补一段说明 —— 通用表的**查询 IID 就是 `pluginxx.<名>`**
  （`pluginxx.kit` 正在用这组名字查询），表格里按宿主命名空间列出的 `agentxx.agent.log`
  等只是排版并列，按该名查询不会命中（此前表格容易让人误以为 IID 带宿主前缀）。

验证：

- 构建：lib `INSTALL`、`agentxx_test` 均 exit=0。
- 测试：`boundaries` 9/0（新增规则通过；文档路径解析、数量匹配均被实际执行）。

注意事项 / 与计划的差异：

- 计划 PLG-8 还含"`plugins.md` 拆为入门/生命周期/SDK/宿主/client/规则"：本轮只做**数量校验
  与文档澄清**（拆分是纯文档重组，收益低于改动成本，留待需要时再做）；
- "接口表集合"的另一半（逐张表的 vtable 可用性/版本）由现有 `plugin_runtime` /
  `plugin_multi_instance` 用例覆盖（真实插件查询每一张表并断言版本与结构尺寸）。



## 阶段 AD：JSONL stdio 一次性运行模式（PRO-8，2026-10-08）

计划依据：`plan.md` §11 PRO-8（"复用 Wire 结构和语义，只增加 JSONL 分帧；stdout 只输出协议
记录，诊断到 stderr，响应不等于轮次完成"）、§16.2 批次 E。

已完成：

- **传输层**（新增 `agent/lib/include/agentxx/agent/io/jsonl_io_transport.h` +
  `agent/lib/src/agent/io/jsonl_io_transport.cpp`）：
  - `JsonlAgentIOTransport`（`AgentIOTransportBase` 实现）: 一行一条 Wire 消息;
    写 = `serialize` + 换行 (与 WS 传输同一份报文编码, 只换分帧方式), 写后立即 flush,
    写操作加锁 (可从任意线程发送); 读 = 默认用后台线程阻塞 `std::getline(stdin)` 后投递到
    asio 通道 (`recv()` 在 io 线程异步等待), 与 client 侧 `StdinReader` 同一做法,
    线程只持有共享状态 (通道 + 停止标记), 不持有传输对象 (析构后不会访问已释放对象);
    Windows 下 stdin/stdout 切二进制模式, 避免 CRLF 转换破坏分帧;
  - **输入结束与关闭分开**: stdin EOF 只置"输入结束"(`inputEnded()`), `recv()` 返回
    nullopt 让端点接收循环退出, 但 `send()` 继续写 —— 对端关掉写入端不等于不想读结果
    (否则 EOF 之后的 `turn_result` 会被丢弃);
  - 非法行不中断会话: 回一条 `WireError(InvalidArgs)` 后继续读下一行 (空行跳过);
    单行长度上限 64 MiB (附件 Base64 内联仍够用, 畸形输入不占无限内存);
  - 起始连接阶段 `Unhandshaken`, 握手后由端点推进到 `Ready` (复用 PRO-5 的阶段校验);
  - 单行编解码 `jsonlEncodeMessage` / `jsonlDecodeLine` (纯函数, 区分"不是 JSON 对象"
    /"缺 type"/"类型不认识"三类错误提示); 另提供 `stats()` (读入/写出/非法行计数)。
- **运行模式**（新增 `agent/client/include/agentxx-client/io/jsonl/jsonl_mode.h` +
  `agent/client/src/io/jsonl/jsonl_mode.cpp`,`agentxx_cli jsonl`）：
  - 与 `server` 模式共用同一套会话驱动实现: 接收循环 → `init()` → 端点 `run()` →
    轮次 → 结果; 装配顺序与进程内 TUI/CLI 模式一致 (接收循环先于 `init` 启动,
    子代理宿主 `AgentHost::attachRoot` 一并挂载);
  - **一次性运行收尾**: 输入结束 (或 SIGINT/SIGTERM) 后等待当前轮次与排队输入跑完
    (EOF 上限 300 秒, 信号上限 5 秒), 然后停端点 → 关插件刷盘 → 关传输 → 停 io_context;
    超时按"未跑完"记警告收尾 (不假装成功);
  - 断线宽限期取长值 (1 小时): 一次性运行没有"多客户端重连", 默认 30 秒会把长轮次当断线取消;
  - `--session-id <id>` 可选参数 (默认自动生成, 对端从 `hello_ack.sessionId` 读回;
    空 `sessionId` 的请求按"当前绑定会话"处理, 因此脚本也可以完全不关心该 id);
  - 行读写注入点 `LineIo` (默认 stdin/stdout, 测试注入内存缓冲) —— 运行器本身不写 stdout,
    stdout 只有传输写出的协议行, 日志走日志系统 (默认 stderr)。

测试：

- 新模块 `jsonl_mode`（65 项断言, `agent/test/core/test_jsonl_mode.cpp`）：
  ① 分帧编解码: 编码恰好一行 (`'\n'` 结尾、内部无裸换行)、`\r\n` 与前后空白容错、空行跳过、
  非法 JSON / 根节点非对象 / 缺 `type` / 未知类型 / 超长行各自的错误文本;
  ② 传输与真实 agent 联调 (本地 LLM 模拟器 + 会话端点 + JSONL 传输, 注入内存行缓冲):
  首条输出必为 `hello_ack` (带会话 id 与能力声明)、第一条输入的 `input_ack` 状态为
  `started` **且此刻 `turn_result` 尚未出现** (响应 ≠ 轮次完成)、两条输入各跑完一轮、
  第二条输入在轮次进行中到达走 `queued`、非法行回 `WireError(InvalidArgs)` 且会话继续可用
  (第二轮照常受理与执行)、输出行全部可解析 (stdout 只有协议行)、读入/非法行计数;
  ③ 未握手阶段只接受 `hello` (握手前的业务消息 → `InvalidState`, 且拒绝排在受理回执之前,
  握手后阶段推进到 `ready`)。
- 新模块 `jsonl_runner`（16 项断言, `agent/test/client/test_jsonl_runner.cpp`）：
  端到端跑 `runJsonlStdio`（内存 IO）—— 输入只剩 EOF 时仍写出 `turn_result` 并自动收尾返回,
  输出顺序为 `hello_ack` → `input_ack` → deltas → `turn_result`, 模型正文出现在流式增量里,
  会话上下文里有该轮输入; 完全无输入时立即收尾返回、不空转。

验证：

- 构建：lib `INSTALL`、`agentxx_test`、`agentxx_cli` 均 exit=0，无新增 error。
- 测试：`jsonl_mode` 65/0、`jsonl_runner` 16/0（合计 81 项断言）。
- 真实进程手工验证 (`agentxx_cli jsonl` + 重定向 stdin/stdout/stderr, 假模型端点):
  - hello (不带 sessionId) → `hello_ack` (自动生成会话 id); 空 `sessionId` 的 `user_input`
    被受理 (`started`); 随后 delta (turn_start / node_start / node_end / message_tip)、
    `context_stats`、`turn_result` (模型连不上 → `hasError=true`, 错误文本在协议里);
  - 24 行 stdout 全部是可解析的协议行, 日志与关停阶段信息全在 stderr (4 个阶段 + 刷盘);
  - 写错 `sessionId` 时按 PRO-7 规则回 `session_mismatch` (错误对象) + `input_ack`
    `rejected`, 非法行走 JSONL 分帧错误对象, 进程照常收尾退出 (exit=0)。

注意事项 / 与计划的差异：

- 计划写"stdout 只输出协议记录，诊断到 stderr": 实现上没有另建诊断通道 —— 传输只写
  stdout, 其余全部走日志系统 (宿主默认 sink 即 stderr), `agentxx_cli jsonl` 不移除该 sink。
- 计划写"响应不等于轮次完成": 该语义由协议本身承载 (`input_ack` 只报受理状态),
  本轮把它写进 `index.md` 的 JSONL 模式一节与 `jsonl_mode.h` 说明, 并有用例断言
  "回执已到、轮次结果未到"。
- 未做 `jsonl` 模式下的多轮交互服务形态: 输入结束即收尾 (一次性运行);
  需要常驻多客户端时用 `server` 模式 (同一套协议, WS 传输)。
- 会话 id 绑定沿用端点的既有语义 (开机绑定 + 空 id = 当前会话), 未新增"由对端指定会话 id"
  的消息 (那属于 `switch_session` 的既有能力, 一次性运行不需要)。

## 阶段 AE：Anthropic 缓存断点与缓存用量（LLM-8，2026-10-08）

计划依据：`plan.md` §6 LLM-8（"先完成 PRM-1/CTX-1，再给 Anthropic 加可选 breakpoint；
记录 cache read/write 到 usage ledger。OpenAI 侧只依赖稳定前缀和 provider 自身缓存能力，
不假设存在可控断点"）。PRM-1（稳定段/动态段分离）已在阶段 M 完成，因此本阶段落地两件事：
请求体断点 + 缓存用量入账。

已完成：

- **可选缓存断点**（`ModelConfig::cacheControl` + `AnthropicProvider::applyCacheBreakpoints`）：
  - 新增 `ModelConfig::cacheControl`（yaml `cache_control`，默认 **false**）：
    仅 `type: anthropic` 生效; 默认关闭的理由是部分 Anthropic 兼容网关不认识
    `cache_control` 字段 (打开会被拒), 确认服务端支持后按模型打开;
  - 断点加在**稳定前缀**末尾 (Anthropic 单请求最多 4 个断点, 这里用 3 个):
    ① 系统提示 (`system` 由字符串转内容块数组, 末块带断点);
    ② 工具定义 (最后一个工具带断点);
    ③ 最后一条**非请求期插入**的消息 —— 请求末尾的动态段
    (`<dynamic_context ...>`, PRM-1 每轮按来源拼装, 内容可能变化) 之前的那条, 否则每轮
    都会写新缓存而读不到旧缓存; 动态段与工具应答被合并到同一条消息时整条跳过该断点;
  - `markCacheBreakpoint` 从末尾往前找第一个可带断点的块 (text / tool_use / tool_result /
    image / document): thinking 块 (带 signature) 不接受该字段, 命中时继续往前找;
  - 关闭时不改写请求体 (用例断言请求体里不出现 `cache_control` 子串)。
- **缓存用量入账**（口径折算 + 账本新列）：
  - 上游语义与 OpenAI 不同: Anthropic 的 `input_tokens` **只统计未命中缓存的输入**,
    命中缓存的读取量与写入量分别是 `cache_read_input_tokens` / `cache_creation_input_tokens`。
    新增 `AnthropicProvider::applyUsage` 作为两条路径 (非流式 `parseResponse` 与流式
    `message_start` / `message_delta`) 的**唯一**用量组装入口:
    `prompt_tokens = input + cache_read + cache_creation`, `total = prompt + completion`,
    `cached_prompt_tokens = cache_read`; 流式路径缺失字段沿用已知值 (message_delta 只带
    output_tokens 时不会把 prompt 计数清零, 这是改造中顺手修掉的一个隐患);
  - 缓存写入量经**补充用量旁路**上报: `neograph::ChatCompletion::Usage` 没有对应字段
    (第三方结构, 不改动), 因此在 `provider_common.h` 增加
    `kUsageDetailExtraKey` 旁路 (provider 写入 `message.extra`, 模型调用节点
    `takeUsageDetail` 取出后把该键置空) —— 记账字段不进入会话消息, 也不进入下一轮请求体;
  - 账本: `UsageRecord/UsageSummary` 增加 `cacheWritePromptTokens`, 会话库 schema
    **v3** 迁移补列 `usage.cache_write_prompt_tokens` (老库 ALTER 补列, 历史记录按 0 保留,
    不猜测回填; 迁移前照常备份 `session.db.bak.v2`); `addUsage` / `usageSummary` /
    `recentUsage` 三处 SQL 同步;
  - 关键指标 (OBS-3): `KeyMetrics::noteModelCall` 增加缓存写入量参数与累计
    (`cache_write_tokens` 进 JSON), 诊断包的用量段输出 `cache_write=...`。

测试：

- `anthropic_provider`（+3 组用例, 模块 280→286 项断言）：
  ① 断点定位: system 转内容块数组且末块带断点、工具只给最后一个加、断点落在动态段
  之前那条消息上 (动态段那条不带断点、正文原样保留); 关闭开关时请求体无该字段;
  ② 非流式缓存用量: `usage{input 100, output 20, cache_read 900, cache_creation 512}`
  → `prompt_tokens=1512`、`cached_prompt_tokens=900`、旁路 detail 读/写量正确、
  `takeUsageDetail` 取出后旁路置空;
  ③ 流式缓存用量: `message_start` 报输入与缓存量、`message_delta` 只报输出量
  → 折算结果同上 (缺失字段沿用已知值)。
- `fake_provider`（模块 36→46 项断言）：假 provider 上报 `cache_write_tokens` 后,
  真实一轮会话的账本聚合与逐行记录都带缓存写入量, `KeyMetrics` 累计一致,
  且会话消息里没有内部记账键 (取出即置空)。
- `session_schema`（模块 104→121 项断言）：新增 v2 → v3 迁移用例 —— 造一个 v2 结构库
  (usage 表无 cache_write 列, `schema_version=2`), 写路径迁移后列已补、版本推进、
  备份 `session.db.bak.v2` 生成; 新记录带缓存写入量, 老记录该列为 0 且其余字段不变;
  聚合把两者都算进去。
- `config_loader`（模块 375→381 项断言）：`cache_control` 未配置默认 false、
  显式 true / "TRUE" / false 三种写法解析正确。

验证：

- 构建：lib `INSTALL`、`agentxx_test`、`agentxx_cli` 均 exit=0，无新增 error。
- 测试：`anthropic_provider` 286/0、`fake_provider` 46/0、`session_schema` 121/0、
  `config_loader` 381/0、`usage_ledger` 21/0、`observability` 96/0；
  回归：`agent` 198/0、`session_persistence` 621/0、`session_sync` 30/0、
  `summarization` 445/0、`prompt_stability_io` 32/0、`remote_agent` 477/0、`boundaries` 13/0。

注意事项 / 与计划的差异：

- 计划提到 "OpenAI 侧只依赖稳定前缀和 provider 自身缓存能力": 未改动 OpenAI / Responses
  provider (其用量解析保持原样, 已按 `prompt_tokens_details.cached_tokens` 记读取量);
  缓存断点是 Anthropic 专有的可选开关。
- 计划的 "记录 cache read/write" 落在账本上: 读取量复用既有 `cached_prompt_tokens` 口径,
  写入量是**新增列** (schema v3)。之所以走 `message.extra` 旁路而不是给
  `neograph::ChatCompletion::Usage` 加字段: 该结构属于第三方库 (独立仓库维护),
  按仓库约定尽量不改; 旁路键在模型调用节点取出后立即置空, 不落进会话消息。
- 未把 `cache_control` 加到 TUI 的"添加模型配置"表单与 `WireAddModel` 协议
  (需要改表单/协议/校验三处): 需要该开关的用户按 yaml `model.list[].cache_control: true`
  打开即可; 后续要表单化时再补。
- 未做缓存命中率的自动诊断/告警: 指标与账本已经把 cache read/write 都记下来
  (OBS-3 的 `cache_write_tokens` 与诊断包会话段), 是否命中由用户按数字判断。

## 阶段 AF：HTTP 录制回放夹具（LLM-13，2026-10-08）

计划依据：`plan.md` §6 LLM-13（"新增 `agent/test/http_recorder` 或可注入传输层，按请求摘要和
顺序保存/回放响应流；敏感 headers 和 API key 脱敏，固定装置不进入生产代码"）。

已完成（新增测试侧夹具 `agent/test/include/agentxx-test/core/http_recorder.h`，仅测试使用）：

- **录制器 `HttpRecorder`**: 本地 HTTP 服务, 把收到的请求**转发给上游** (origin + 原路径,
  透传头, 排除逐跳头), 同时把 (请求摘要 + 响应状态/类型/正文) 记入固定装置;
  这样"跑一次真实/模拟上游"就能得到可重复的固定装置。
- **回放器 `HttpPlayer`**: 本地 HTTP 服务, 按**顺序**回放固定装置里的响应;
  - 每条请求都要与装置里下一条对得上 (方法 + 路径 + 请求体摘要), 不符时回 400 并把
    `expectedIndex/expectedDigest/actualDigest` 与可读原因写进响应体, 同时记入
    `mismatches()` 供用例断言 (游标不推进);
  - 装置用尽后再来请求同样被拒 (`no more recorded interaction`), 并在 `mismatches()`
    留下 `unexpected extra request`。
- **固定装置 `HttpFixture`**: `version / note / interactions[]`, 可 `toJson` / `fromJson` /
  `saveToFile` / `loadFromFile` (文件不存在明确抛错)。`HttpInteraction` 含方法、路径、
  请求摘要 (FNV-1a 64 十六进制)、请求字节数、脱敏后的请求体、头快照、状态码、content-type、
  响应正文 (SSE 原样保存, 因此流式也可回放)。
- **脱敏** (固定装置要能入库):
  - 请求头只落白名单 (`content-type` / `accept` / `anthropic-version` / `user-agent`);
  - 凭据类头 (`authorization` / `x-api-key` / `api-key` / `cookie` / `proxy-authorization`)
    只记 `<名>_present: true`, **不记取值**;
  - 请求体与响应体先按调用方给的 `secrets` 字面替换, 再过
    `agentxx::util::redactSecrets` (复用诊断包那套凭据形态匹配, 只有一处实现);
  - 请求摘要基于**原始**请求体计算, 脱敏不影响匹配。
- **与既有替身的分工** (写入 `agent/test/AGENTS.md` "测试替身" 节): 假 provider 在 provider
  之上 (重试/压缩/取消/工具循环), 本夹具在 provider 之下 (真实 HTTP 编解码与 SSE 解析),
  手工 mock 保留覆盖个别响应形态。

测试（新模块 `http_recorder`，81 项断言）：

| 用例 | 覆盖点 |
|---|---|
| 摘要与装置往返 | 摘要对同输入稳定 (16 位十六进制)、方法/路径/正文任一变化即不同; 装置 JSON 与文件往返 (含目录自动创建); 文件不存在明确抛错 |
| 脱敏 | 头白名单取值保留、凭据只记 `*_present` 且取值不出现在装置文本里; 正文按 `secrets` 与通用凭据形态屏蔽, 无凭据文本原样保留 |
| 录制 → 回放 | 真实 OpenAI provider → 录制器 → 本地 LLM 模拟器: 装置记录 1 条 (路径/状态/响应正文/摘要/凭据标记), 装置文本不含 API Key; 落盘后停上游, 回放器再跑同一次调用 → 结果一致、命中 1 条、无不匹配 |
| 顺序/摘要校验 | 请求体与装置不符 → 回放器 400 + provider 抛错 + `mismatches()` 记 `request #0 mismatch` 且游标不推进; 装置用尽后的额外请求 → 400 + `extra request` |
| 流式回放 | 模拟器的 SSE 响应原样录制 (正文含 `data:`), 回放后 provider 解析出的正文与流式分片与首次完全一致 |

验证：

- 构建：`agentxx_test` exit=0，无新增 error。
- 测试：`http_recorder` 81/0（含 3 处 ASan 复查：首版用例把 `mismatches()` 返回的临时
  容器元素绑成引用, ASan 直接报 heap-use-after-free, 已改为先取快照再引用）。
- 收尾: 用例结束调用 `HttpClient::clearConnectionPool()`, 释放指向已停服务的空闲连接。

注意事项 / 与计划的差异：

- 计划写"`agent/test/http_recorder` 或可注入传输层": 选**独立夹具 + 本地服务**形态 ——
  provider 的 HTTP 走 `utilxx::HttpClient` (没有传输层接口), 注入点若放在 provider 内部
  就要改生产代码; 本地服务既不改生产代码, 又能把真实网络路径完整跑一遍。
- 只支持 POST 转发 (三家 provider 的补全请求都是 POST); GET/其他方法回 502 并在装置里
  记下原因。若将来要录制 GET (如模型列表), 在此处补分支即可。
- 路由是精确匹配, 夹具把常见 LLM 路径 (`/chat/completions` / `/v1/chat/completions` /
  `/messages` / `/v1/messages` / `/responses` / `/v1/responses` / `/`) 都注册了一遍。
- 本阶段未录制真实线上流量并入库 (不引入任何外部服务依赖): 夹具本身可用作"本地录一次、
  之后离线回放"的工具, 入库的固定装置应按需人工挑选并 review 脱敏结果。

## 阶段 AG：配置键目录生成与新鲜度门禁（CFG-9，2026-10-08）

计划依据：`plan.md` §13 CFG-9（"从 `AgentConfig`/`YamlAppConfig` 生成键目录（键路径/类型/
默认值），与 PRO-4 共用生成器骨架，CI 检查新鲜度"）。

已完成：

- **共用生成器骨架**（新增 `agent/test/include/agentxx-test/core/schema_artifact.h`）：
  `checkGeneratedArtifact(path, actual, update, artifact, updateEnv)` + `readArtifactFile` /
  `writeArtifactFile` / `artifactDiffLines` / `artifactUpdateMode`；`wire_schema` 模块改成
  调用同一实现（原来那份读写/比对代码删除），两处生成物的门禁行为与失败信息一致
  （首个差异行 + 行的两侧内容 + 重新生成命令）。
- **键目录表**（新增 `agent/test/core/test_config_keys.cpp`，模块 `config_keys`，49 个键）：
  每个键登记 键路径 / 类型 / 默认值 / 样例 yaml / 默认用例文档 / 期望默认值 / 期望样例值 /
  观测函数，逐键做**两次真实加载**：
  - 默认: 顶层键用"只写 `data_dir` 的最小文档", 条目级键（`model.list[]` / `plugin.list[]` /
    `mcp.list[]`）用"只写必需字段的最小条目", 比对默认值;
  - 样例: 覆盖层文档（`overwrite.*` 用两层加载，base = 两个模型 + 两个技能目录）比对样例值。
  被覆盖的范围: 顶层 (`data_dir` / `work_dir` / `language` / `subagent.enable` /
  `worktree.enable`) · 权限 (`mode` / `whitelist` / `blacklist`) · 技能与记忆列表 ·
  `model.use.*` 七个用途 · 模型条目 20 个字段 (含 `extra_api_config` / `extra_headers` /
  `cache_control`) · 两层合并策略 (`overwrite.mode` / `overwrite.remove`) ·
  插件条目 6 个字段 · MCP 条目 3 个字段 · 已废弃旧键 3 个 (`models` / `plugins` /
  `use_model`, 断言"识别但不生效")。
- **源码扫描门禁**：从 `client/src/config_loader.cpp` 扫出被解析的键名
  (`["键名"]` / `mapChild(..., "键")` / `useValue("键")` / 段常量定义值), 要求每个名字都能在
  目录里找到 —— 加载器新增键却忘记登记时直接失败 (扫描量下限 30, 防止规则本身失效)。
- **生成物**：
  - `agent/schema/config-keys.json`（机器可读: path/type/default/sample）;
  - `docs/zh-cn/design/config-keys.md`（人工阅读: 键表 + 每键样例 + 列表段/`overwrite`/
    废弃键/`${VAR}` 的说明）;
  - 新鲜度门禁与一键更新 `AGENTXX_UPDATE_CONFIG_KEYS=1`（与 PRO-4 同一套做法）。
- **顺带修掉一个真实缺陷**（由键目录的样例校验发现）：
  `config_loader.cpp` 里数值标量一律用 `utilxx_base::parseNumberFromString(...).ec == std::errc{}`
  判定成功, 但 `std::from_chars` 对 `"0.7"` / `"16abc"` 这类文本会**部分消费**且返回空 `ec` ——
  于是 `extra_api_config: {temperature: 0.7}` 被写成 `0`、插件 `args` 里的小数同样被截断成整数,
  非法数字键值也被静默当合法值。改法: 新增 `parseFullNumber()` 辅助 (要求消费到字符串末尾),
  替换 9 处调用点; 数值键解析失败时补一条 `Warning: invalid xxx ... keeping default` 告警。
  校验方式: 键目录里 `extra_api_config` 的期望值就是 `{"temperature":0.7}` (修前必然失败),
  `config_loader` 模块另加 26 项断言覆盖小数保留 / 插件 args 小数 / 非法值落回默认 / 合法值生效。

测试：

- 新模块 `config_keys`（157 项断言）：49 键 ×（默认 + 样例）+ 路径唯一性 + 数量下限 +
  源码扫描比对 + 生成物比对与形状断言。
- `config_loader`（模块 381→407 项断言)：小数与非法数值的解析行为。
- `wire_schema`（模块 1034 项）回归: 改用共用骨架后生成物比对行为不变。

验证：

- 构建：lib `INSTALL`、`agentxx_test`、`agentxx_cli` 均 exit=0，无新增 error。
- 测试：`config_keys` 157/0、`config_loader` 407/0、`config_validation` 47/0、
  `wire_schema` 1034/0。
- 生成物自检：`AGENTXX_UPDATE_CONFIG_KEYS=1` 生成后, 比较模式再次运行全部通过。
- 负面验证：把 `parseFullNumber` 换回旧的 `ec` 判定, `config_keys` 与 `config_loader`
  的小数用例都会失败 (期望 0.7 实得 0)。

注意事项 / 与计划的差异：

- 计划写"从 `AgentConfig`/`YamlAppConfig` **生成**键目录": C++ 没有反射, 真正的自动生成要么
  改数据结构加反射, 要么维护一份与解析代码平行的表。这里选**表 + 真实加载校验 + 源码扫描**:
  表提供类型/默认值/样例 (文档价值), 校验保证表与加载器一致 (含"加载器新增键必须登记"),
  等价于计划要的"新鲜度检查"而不引入反射或代码生成。
- 目录只覆盖 **yaml 应用配置** (`YamlAppConfig` 一层); `AgentConfig` 的其余字段由库使用方
  (client/FFI) 填充, 不属于用户可写的配置面, 因此不列入 (计划里的 `AgentConfig` 部分按
  "用户可配置键 = yaml 键" 收窄)。
- 未做 CI 接入: 门禁随 `agentxx_test`（`config_keys` 模块）运行, `agent/script/gate.sh` 的
  全模块 fail-fast 已经覆盖; 另开 CI 配置属于外部集成, 不在本项目仓库范围。
- `overwrite` 只在 `model` 段做了两层加载用例: 其余列表段共用同一实现 (`readListSection`
  与合并函数), 机制一致, 逐段重复用例收益低。

## 阶段 AH：按模块日志级别（OBS-5，2026-10-08）

计划依据：`plan.md` §14 OBS-5（"按模块和插件前缀调整日志级别，避免全局 debug 破坏 TUI"）。

已完成：

- **日志条目带模块名**（`third_party/cxx_utilxx_base`）：
  - `LogEntry` 增加 `std::string module`（空 = 未指定）；
  - 新增 `constexpr logModuleOf(__FILE__)`：由源文件路径取基名去扩展名（兼容 `/` 与 `\`），
    编译期求值、无运行期分配；`XX_LOG*` 宏经 `XX_LOG_MODULE` 自动带上它 —— **调用点零改动**；
  - `xxLogPrint` 增加三参数重载（级别 + 模块 + 正文），**保留原两参数版本**：老调用点 /
    按旧头文件编译的插件动态库继续可用（不因签名变化链接失败）。
- **按模块过滤**（`LogDispatcher`）：
  - `setModuleLevel(前缀, 最低级别)` / `clearModuleLevels()` / `moduleLevelCount()`；
    表用 copy-on-write 快照，dispatch 热路径只做一次小表线性查找，**未命中任何条目时
    与改动前完全等价**；
  - 匹配规则: 最长前缀生效（长度相同后注册者胜）；命中后低于该级别的日志**直接丢弃**
    （连 `LogEntry` 都不构造）；`Out` 级别恒通过（`XX_OUT` 是"必须展示"的输出）。
- **配置入口**：
  - `applyLogModuleLevelSpec("前缀=级别,...")` 解析器（空白/大小写容错、空段跳过、
    非法段写回 `invalidEntries` 供调用方告警）；
  - CLI 启动时读取环境变量 `AGENTXX_LOG_MODULES`（`client/main.cpp`，所有模式共用），
    应用后打印一行 `[Log] module levels applied: N`，非法段逐条提示；
  - 宿主/嵌入方也可直接调 `LogDispatcher::setModuleLevel`。
- **插件日志记为 `plugin.<插件名>`**：内核 `pluginxx/host/tables_impl.h` 的 log 表入口
  经 `enterHost(host, allowClosing=true)` 取实例名作为模块名（`plugin.<name>`），
  于是 `plugin.agentxx_codegraph=debug` 这类按插件的级别设置直接可用；实例取不到时
  退化为 `plugin`。客户端侧插件宿主有独立的 log 入口（`client_plugin_manager.cpp` 的
  `xx_clog`），那里暂未接实例名（沿用 `client_plugin_manager` 模块），已在"差异"里记录。

测试（新模块 `log_modules`，35 项断言）：

| 组 | 覆盖点 |
|---|---|
| `logModuleOf` | `__FILE__` 推导、`/` 与 `\` 两种分隔符、无扩展名、多点文件名、空串 |
| 规格解析 | 多段/空白/大小写；缺 `=`、空前缀、级别名非法各自进 `invalidEntries`；结尾逗号与空段不算错误；同前缀重复设置是覆盖而非叠加；`clearModuleLevels` 归零 |
| 过滤语义 | 默认不过滤（行为与改动前一致）；命中模块的低级别被丢弃而其它模块不受影响；`Out` 恒通过；`plugin.` 命中 `plugin.foo` 但不命中 `plugins_other`（前缀语义）；最长前缀优先（`plugin.noisy` 放宽后仅它通过）；清空后恢复全量 |

验证：

- 构建：删除 `cxx_utilxx_base_repo-prefix` / `cxx_pluginxx_repo-prefix` 让工具库与插件全部重编
  （改的是共享头，按仓库约定必须让 cmake 重新编译），`agentxx_lib_repo`、`agentxx_test`、
  `agentxx_cli` 三个目标均 exit=0，无新增 error；内置插件动态库与最终程序一起重建成功。
- 测试：`log_modules` 35/0；**全量回归**（`agentxx_test` 全模块, Debug + ASan/UBSan）
  **36,438 项断言 0 失败**, 进程 exit=0, 无 ASan 报告（上一轮基线 35,985）。

注意事项 / 与计划的差异：

- 计划的"按模块"指模块名，实现取**源文件基名**（`modelcall` / `session_store` / ...）:
  与文件一一对应、无需人工维护映射表；要按"逻辑模块"分组时用前缀匹配即可
  （例如 `plugin.` 覆盖全部插件、`client_plugin` 覆盖客户端插件管理器）。
- 客户端侧插件日志暂未带插件名（`client_plugin_manager` 的 `xx_clog` 没有实例解析路径，
  需要补一层 host→实例 的查找）; 需要时按同一做法补，属独立小改动。
- 未做 TUI 设置项: 级别设置走环境变量与 API（部署/排查场景更常用）; 进设置弹窗需要
  新的界面条目与持久化键，收益低，未列入本轮。
- `LogEntry` 加了字段（导出结构）: 宿主与工具库在同一 superbuild 下同时重建，
  插件侧按旧头文件编译也能工作（两参数入口保留）；跨版本混用时插件日志不带模块名，
  过滤规则对它不生效（按"未命中不过滤"处理）。
