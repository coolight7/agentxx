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
| LOOP-1 | 持久化收件箱两段状态 | P0 | 待完成 | — |
| LOOP-2 | `next-step` / `next-turn` / `inject` | P0 | 待完成 | — |
| LOOP-3 | 投递结果显式化 | P1 | 待完成 | — |
| LOOP-4 | QueueState 状态机 | P1 | 待完成 | — |
| LOOP-11 | `collect` 合并投递 | P1 | 待完成 | — |
| CTX-7 | 附件引用而不是反复内联 Base64 | P1 | 待完成（计划标注"需进一步理解实施内容"，先不动） | — |
| PRM-5 | 技能优先级和同名裁决 | P1 | 待完成 | — |
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

- LOOP-1/2/3/4/11：输入投递的持久化收件箱、`next-step|next-turn|inject|collect` 语义、
  投递结果回执与 `QueueState` 状态机（涉及 `wire_protocol`、`session_server_agent_io`、
  modelcall 请求装配）。
- PRM-5：技能优先级与同名裁决；STO-13：会话导出。CTX-7（附件引用）计划本身标注
  "需进一步理解具体实施内容"，暂缓。
- STO-12 的协议/界面入口（改名、搜索框）见"阶段 C"。
- TOOL-1/TOOL-3：工具分阶段并行与并行取消收尾（`nodes/toolcall.cpp`）——**仍未实施**，
  原因见下方"未实施项的原因与建议路径"。

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
