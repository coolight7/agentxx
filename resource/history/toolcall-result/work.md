# 工具结果状态重构 (toolcall-result) 实施记录

> 设计见同目录 [plan.md](plan.md)。本文件记录**实际执行顺序、已完成内容、待完成内容与注意事项**,
> 每完成一个阶段就更新一次并单独提交。

## 总体进度

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | 依赖库拼写统一 (`Canceled`) + `awaitXXXOp` 改名 | 已完成 |
| P1 | `util/tool_result.h` + `XXToolBase` 签名 + lib 内工具 + 执行层状态机 | 已完成 |
| P2 | Wire 字段与版本 + `ToolData`/`ViewMessage` + 客户端渲染 | 已完成 |
| P3 | 插件边界 (`PluginTool` 映射/超时) + API 4 + tools 表 v3 + SDK + JS 引擎 | 未开始 |
| P4 | 内置插件改造 (filesystem / math / execute_command + 审计其余) | 未开始 |
| P5 | 测试补齐 + 拼写扫描 + 全量回归 | 未开始 |
| P6 | 文档与生成物 | 未开始 |

## 环境

- Windows debug 构建: `agent/build/windows-debug` (Debug + ASan), 用
  `cmake --build agent/build/windows-debug --config Debug --target <target>` 增量编译
  (环境变量 `VSLANG=1033` / `VCPKG_ROOT=` / `VCPkgLocalAppDataDisabled=1`;
  需要 VS 环境时走 `agent/script/windows_debug_build.bat`)
- 目标名: `cxx_utilxx_base_repo` / `cxx_utilxx_repo` / `cxx_pluginxx_repo`(依赖库, 改头文件后先编它们)、
  `agentxx_lib_repo`、`agentxx_test_repo`、`agentxx_client_repo`
- 测试: `agent/build/windows-debug/exec/agentxx_test.exe <模块...>`

## P0 已完成 (依赖库拼写统一 + awaitXXXOp 改名)

改名的公共契约:
- `utilxx::CancelledException` -> `utilxx::CanceledException`
- `utilxx::CancelToken::isCancelled()` -> `isCanceled()`, `throwIfCancelled()` -> `throwIfCanceled()`
- `utilxx_base::ControlFlowKind::Cancelled` -> `Canceled`
- `pluginxx::CancelledException` -> `pluginxx::CanceledException`; `OpCtl::throw_if_cancelled()` ->
  `throw_if_canceled()`; `CancelRegistry::isCancelled/isCancelledKey` 一族同步改成 `canceled`
- `PLUGINXX_OPERATOR_CANCELLED` -> `PLUGINXX_OPERATOR_CANCELED`
- `pluginxx::awaitPluginOp` -> `awaitPluginOpResult`, 返回新的 `pluginxx::PluginOpResult{status, payload}`
  (**不再为业务失败抛异常**; 等待本身被取消仍按原样抛), 旧名直接删除
- `agentxx::util::awaitHostPluginOpResult` 返回 `PluginOpResult`, 把 `utilxx::CanceledException`
  换算成 `PLUGINXX_OPERATOR_CANCELED`; 新增 `agentxx::util::requirePluginOpOk()` 给
  "非 OK 就抛" 的调用点 (图节点 / 钩子派发, 取消仍抛 `neograph::graph::CancelledException`)
- agentxx 侧 `KeyMetrics::TurnOutcome::Cancelled`/`ToolOutcome::Cancelled`/`turnsCancelled()`/
  `toolCancelled()` 等一并改成 `Canceled`; `agentxx.agent.session` 表的 `is_cancelled` 成员改名为
  `is_canceled` (仅名字, 布局不变)

保留原拼写的地方 (按要求不动): `neograph::graph::CancelledException`、`CancelToken::is_cancelled()`、
`throw_if_cancelled()`, 以及 asio 自带的 `asio::experimental::channel_errc::channel_cancelled`。

依赖库子模块已各自提交 (`cxx_utilxx_base` f5e0c6e / `cxx_utilxx` aacfbe6 / `cxx_pluginxx` 0a8fc82)。

验证: `cxx_utilxx_base_repo`/`cxx_utilxx_repo`/`cxx_pluginxx_repo`/`agentxx_lib_repo`/
`agentxx_test_repo`/`agentxx_client_repo` 全部编译通过;
`agentxx_test plugin_runtime plugin_sdk plugins` = passed 1451 / failed 0。

## 实施中的注意事项 (踩过的坑)

- **PowerShell `-replace` 默认大小写不敏感**: 用它做 "只改大小写" 的替换会连带命中大写形式,
  且 `-ne` 比较也是大小写不敏感 —— 用 `-creplace` + `-cne` 才能可靠判定是否需要写回。
  批量改标识符时先用占位符保护第三方标识符 (`neograph::graph::CancelledException`、
  `is_cancelled`、`channel_cancelled`), 占位符本身不能含被替换的词。
- 改 `agent/third_party/cxx_*` 的头文件后, 要先编 `cxx_*_repo` 目标 (它们把头文件 install 到
  `agent/build/windows-debug/agentxx-project-install/include`), 否则插件编到的是旧头。
- `AGENTS.md` 在工作区里原本就有一处与本任务无关的未提交改动 (注释示例被改写),
  本次只在 P6 追加自己的段落, 不回滚它。
- 阶段结束时只 `git add` 本阶段相关文件, 不碰 `TODOS.md` / `README.md` 与
  `agent/third_party` 里无关的改动。

## P1 已完成 (`ToolResult` + 工具签名 + 执行层状态机)

新增 [tool_result.h](../../../agent/lib/include/agentxx/util/tool_result.h) (全项目唯一的状态定义):

- `ToolStatus{Ok, Failed, Interrupted, Canceled}` + `toolStatusName` / `toolStatusFromName`
  (名字是小写 `ok`/`failed`/`interrupted`/`canceled`; **未知取值按 `Ok`**, 老代码读到新状态不会误判失败)
- `ToolResult{text, status}` + `ok/fail/interrupted/canceled` 四个构造入口; `text` 只写正文,
  失败时写**错误内容** (不带 `[Error: ...]` 包裹)
- 删除 `agent/lib/include/agentxx/util/tool_result_text.h` (文本前缀判定表; 全项目不再看文本判成败)

`XXToolBase` 与工具改造:

- `XXToolBase::execute_async(const Json&)` 返回 `asio::awaitable<util::ToolResult>`
  (`tools/tool.h` / `tool.cpp`); neograph 桥接仍返回 `std::string` (取 `.text`),
  `XXToolWrap` 包成 `ToolResult::ok`
- 逐个工具改成返回状态: `share_store`、`subagent`、`event`、`git_worktree`(去掉 `[Error] ` 前缀,
  CreateOutcome/RemoveOutcome 增 `canceled` 字段)、`host_tool`(handler 抛异常=失败, 返回 ok 文本)、
  `mcp_client`(读 `isError` -> `fail`)、`plugin_manager_adapters.cpp`(新增 `mapPluginOpResult`,
  超时文本 `timed out after <n> seconds`)、`middlewares/subagent_manager.cpp`

执行层 (`nodes/toolcall.cpp`, 全项目唯一的文本格式化处):

- `formatToolResultText()`: 只有 `Failed` 改变文本 —— 单行 `[Error: <text>]`, 多行首行 `[Error]`
  后接原文, 空文本只有 `[Error]`; `Canceled`/`Interrupted` 为空时补 `[Canceled]`/`[Interrupt]` 占位;
  `Ok` 原样透传
- `markToolStatus()`: 只在非 `Ok` 时写消息 `extra["status"]` (值取 `toolStatusName`)
- `toolResultFromCachedMessage()`: 从中断缓存的 Json 消息还原 `{text, status}`
- 权限拒绝 -> `permission denied[: <reason>]`、重复调用 -> `repeated identical call denied by the
  user: ...`、工具不存在 -> `tool not found: <name>`、参数 JSON 解析失败 -> 异常原文, 都走
  `ToolResult::fail` 由执行层统一加前缀
- 取消占位文本由 `[User canceled]` 改成 `[Canceled]`; 关键指标按终态记 (`Ok`/`Failed`/`Interrupted`/`Canceled`)
- `PreparedToolCall::shortCircuit` 改 `std::optional<ToolResult>`;
  `runToolCallBody`/`finalizeToolCall`/`execTool` 返回 `ToolResult`

## P1 期间修掉的一个真实缺陷 (取消路径会丢 tool 结果消息)

现象: `message_supplement` 的取消用例失败 —— 取消后上下文只剩 3 条 (`system`/`user`/`assistant`),
两条 `[Canceled]` 占位消息没写进会话, 也就是 assistant 的 `tool_calls` 没有对应结果
(下一次请求会被上游 API 拒掉)。

定位过程 (临时 `fprintf(stderr)` 打点, 事后已移除): `runPreparedToolCallGuarded` 记录到取消之后,
末尾的 `co_await publishToolEvent(...)` (发 `tool.end`) 在取消瞬间抛异常, 该异常从
`runPreparedToolCallGuarded` -> `runToolBatch` -> `baseRun` 一路逃逸, 于是 `baseRun` 里
"未完成调用补 `[Canceled]` 占位" 的分支根本没执行。

两处修复 (都在 `nodes/toolcall.cpp`):

1. `publishToolEvent` 整段包进 `agentxx::util::catchErrorAsync`, 普通异常与取消/中断都只记
   `XX_LOGD` —— 事件发布只是元信息, 不能打断工具执行与结果写入 (取消有各自的检查点)
2. `baseRun` 的整段批次执行包进 `catchErrorAsync`: 取消从任一 await 点逃出来时都转成
   "取消收尾" (`isCancel` + `cancelErrorPtr`), 让占位补写在**结构上**一定执行; 中断仍是
   `onRethrow` 返回 `nullopt` -> 原异常重抛 (中断恢复靠缓存, 语义不变)

## P2 已完成 (Wire / 界面 / CLI)

- `agent/conversation_types.h`: `ToolData.hasError` -> `status` (键 `status`, 旧 `has_error` 只读兼容);
  `WireDelta.hasError` -> `status`
- `agent/io/wire_protocol.h`: delta 的 `hasError` -> `status` (只有 ToolEnd 且非 `Ok` 才写)
- `agent/io/agent_io_transport.h`: `kWireProtocol::kVersion` = 2, `kMinVersion` = 2
- `session_server_agent_io.cpp`: 握手同时拒绝"过新"与"过老"客户端; `ws_io_transport.cpp`:
  客户端拒绝过老服务端 (`lastHelloAck_->ok=false` + 可读原因)
- `event/event_stream.cpp` + `event/events.h` (`EventToolCallEnd::status`):
  `tool.end`/`tool.end.<工具名>` 载荷的 `hasError` 换成状态名
- `agent_stdio.cpp` (`done`/`error`/`interrupted`/`canceled`)、`message_list.cpp`
  (`ToolVisual` + `toolVisualOf()`: 失败红 / 中断黄 / 取消普通)、`agent_tui.cpp`
  (`m->tool->status = delta.status`)
- `agent/prompt.cpp`: Context Markers 段改为三类标记 `[Error: ...]` / `[Canceled]` / `[Interrupt]`,
  删掉 `[Permission denied]` / `[Repeated call denied]` / `[Plugin tool timeout]` 三条,
  补充"多行错误以 `[Error]` 开头"; 轮次级标记仍是 `[User canceled]`
- 生成物重生成: `AGENTXX_UPDATE_WIRE_SCHEMA=1 agentxx_test wire_schema` ->
  `agent/schema/wire-schema.json`(`protocolVersion` 2) 与 `docs/zh-cn/design/wire-protocol-fields.md`

### P1/P2 回归结果 (Windows debug)

| 模块 | 结果 |
|---|---|
| `toolcall_args` | 200 / 0 |
| `toolcall_parallel` | 48 / 0 |
| `message_supplement` | 95 / 0 |
| `summarization` | 454 / 0 |
| `cancel` | 45 / 0 |
| `race_guards` | 68 / 0 |
| `wire_roundtrip` | 237 / 0 |
| `wire_schema` | 1325 / 0 (重生成后) |
| `tui_stream` / `tui_tool_header` / `tui_ui_items` / `tui_form` / `tui_widget` / `tui_interrupt` | 全 0 失败 |
| `remote_agent` | 519 / 0 |
| `event_hub` / `event_tools` / `interrupt_ui` / `interrupt_bus` | 全 0 失败 |
| `subagent_tool` / `share_store` / `mcp` / `permission` / `input_delivery` | 全 0 失败 |
| `jsonl_mode` / `agent` | 全 0 失败 |

已知与本任务无关的既有失败: `boundaries` 的"规则 15 称呼与用词"在
`agent/third_party/cxx_pluginxx_ui` 的 `README.md` / `docs/integration.md` / `include/pluginxx/ui/item.h`
里有 5 处 `载体` (子模块文件, 本次未改动, 改动前就是这样); 本任务自身的用词违规已按该规则改掉
(`承载`/`口径`)。

补充: `boundaries` 与 `config_keys` 这类按源码自检的模块会读工作区源码, 所以改完注释即可直接跑,
不必先重编; 但**库代码改动要同时编 `agentxx_lib_repo` 与 `agentxx_test_repo`** ——
测试程序静态链接 `libagentxx_staticd.lib`, 只编 lib 不重链 test, 跑起来还是旧代码 (本次踩到,
表现为临时打点一行都不打印)。
