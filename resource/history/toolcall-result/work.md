# 工具结果状态重构 (toolcall-result) 实施记录

> 设计见同目录 [plan.md](plan.md)。本文件记录**实际执行顺序、已完成内容、待完成内容与注意事项**,
> 每完成一个阶段就更新一次并单独提交。

## 总体进度

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | 依赖库拼写统一 (`Canceled`) + `awaitXXXOp` 改名 | 已完成 |
| P1 | `util/tool_result.h` + `XXToolBase` 签名 + lib 内工具 + 执行层状态机 | 已完成 |
| P2 | Wire 字段与版本 + `ToolData`/`ViewMessage` + 客户端渲染 | 已完成 |
| P3 | 插件边界 (`PluginTool` 映射/超时) + API 4 + tools 表 v3 + SDK + JS 引擎 | 已完成 |
| P4 | 内置插件改造 (filesystem / math / execute_command + 审计其余) | 已完成 |
| P5 | 测试补齐 + 拼写扫描 + 全量回归 | 已完成 |
| P6 | 文档与生成物 | 已完成 |

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

## P3 已完成 (插件 API 代次 4 + SDK + JS 引擎 + 示例)

- 全局代次: `agent/CMakeLists.txt` 的 `AGENTXX_PLUGIN_API_VERSION` 3 -> 4
  (它经 `_AGENTXX_COMMON_CMAKE_ARGS` 下发成 `-DPLUGINXX_API_VERSION=4 -DPLUGINXX_MIN_API_VERSION=4`);
  client 侧 `client_plugin_api.h` 的 `AGENTXX_CLIENT_PLUGIN_API_VERSION` /
  `AGENTXX_CLIENT_PLUGIN_MIN_API_VERSION` 一并 3 -> 4 (两侧同一代头文件重编)
- `plugin_api.h` 的 `agentxx.agent.tools` 表 v2 -> v3 (**布局不变, 基线不变**):
  工具成败只由最终通知的状态位表达 (`PLUGINXX_OPERATOR_OK`/`FAILED`/`CANCELED`),
  结果文本只给模型读 —— 插件不再把 `[Error: ...]` 之类前缀写进 payload, 失败时只给错误内容
- `plugin_kit.h`: 引入 `agentxx/util/tool_result.h`, `using ToolResult/ToolStatus`;
  `fast_tool` / `blocking_tool` / `polled_tool` 的业务体都可以返回 `ToolResult`
  (返回 `std::string` 仍是"成功", 老写法不用改); 新增
  `detail::toolBodyResultTo(...)` (把两种返回形态归一成 `{text,status}`) 与
  `detail::toolStatusOfOperator(...)` (状态 -> 跨边界操作码)
- `plugin_manager_lifecycle.cpp` 的 `diagnosePluginPath`: 静态检查都通过时提示
  "多为插件与主程序的 API 代次不匹配 —— 插件需要按 API v4 重新编译"
- JS 引擎 (`agentxx_javascript_engine.cpp`): `ToolExecReq` 增 `status`, JS 侧返回
  `{status, text}` 时识别 (`failed`/`interrupted` -> FAILED, `canceled` -> CANCELED, 未知 -> OK),
  非 OK 时按状态上报
- 示例: `example_plugin` 增 `example_fail_demo` (`ToolResult` 对照, `example_echo` 仍返回字符串);
  `example_js` 增 `js_fail_demo`; `example_js_execute_command` 的空 code/超时改为 `{status:"failed",text}`
- `test_plugin_abi_c17.c` 的 `_Static_assert` 跟着抬到 4 (C17 侧与 C++ 侧看到同一代次)

## P4 已完成 (内置插件工具返回 `ToolResult`)

- `agentxx_filesystem`: 9 个 `*Execute` + 6 个 `*ExecuteImpl` + 3 个 async 实现
  (含无 async 时的回退) 全部返回 `ToolResult`; `detail::okText/failText` 便于构造;
  `asErrorText` 用 `if constexpr` 兼容两种返回; 12 处 `args.errorMessage()` 改
  `ToolResult::fail(...)`; `destinationAllowed` 的文案去掉前缀 (前缀由执行层统一加);
  polled lambda 返回类型同步
- `agentxx_math`: `mathCalculateExecute` 返回 `ToolResult` (去掉 `[Error] ` 前缀)
- `agentxx_execute_command`: `runProcPipeline` / `bashExecuteAsync` / `windowsExecuteAsync` /
  popen 版 `bashExecute` / `windowsExecute` 全部返回 `ToolResult`; `makeTimeoutResult` 去掉内嵌
  `[Error]` 行 (块内只剩事实); 取消 -> `ToolResult::canceled("Command canceled before execution.")`;
  顺带修掉"超时/取消结果以 `[ExitCode]` 开头被当成功"的既有缺陷
- 其余插件逐个审计: `rag_search` / `audio_stream` / `text_selection_monitor`
  (`args.errorMessage()` -> 失败, 其它 return 包成功)、`screen_capture`
  (4 处 `{"ok":false}` -> 失败)、`computer_use` (`uiControlExecute`)
- 测试适配: `test_framework.h` 新增 `agentxx::test::displayText(ToolResult)`
  (转发 `util::toolResultTextForModel`, 测试断言文本时与执行层同一份实现);
  `test_filesystem_tools.cpp` 加 `agentxx_fs_test` facade (22 处调用点)、
  `test_command_tools.cpp` 加变参模板 facade (boost.process v2 / popen 两套)、
  `test_math_tools.cpp` 用 `displayText` 包一层

## P5 已完成 (用例补齐 + 握手判定抽取)

- 文本格式化收敛: `util/tool_result.h` 新增 `toolResultTextForModel(const ToolResult&)`
  (全项目唯一实现), `nodes/toolcall.cpp` 的 `formatToolResultText` 只转发;
  测试也用它断言 (状态与文本不可能走偏)
- 握手版本判定抽成可单测的一份: `WireProtocol::VersionCheck{Ok,TooNew,TooOld}` +
  `checkVersion(remoteVersion)` + `versionRejectReason(check, remoteVersion)`
  - 服务端 `handleHello`: 用 `checkVersion` 判, 原因用 `versionRejectReason` (过老/过新都拒)
  - 客户端 `ws_io_transport.cpp`: 同一套判定, 只拒"过老服务端" —— 更新的服务端接受了本端
    Hello 说明其 `kMinVersion` 覆盖本端版本, 会按兼容方式对待本端 (本端不认识的新消息忽略即可)
  - 修掉"`kMinVersion` 定义了却没人用"的既有缺陷 (老客户端 `protocolVersion=0` 现在被拒)
- 新用例: `wire_roundtrip` 加版本协商断言 (`checkVersion` 三态 + 原因文案要点);
  `remote_agent` 把 fake 服务端改成同一套判定, 补"老客户端被拒"与"服务端过老客户端自己拒"
  (新增 `/agent-old` 假服务端); `test_mcp` 的 stdio 服务端加 `fail` 工具 (`isError:true`)
  断言 `Failed`; `test_plugins` 31.5b 插件工具超时 (Failed + `timed out after`)、
  31.6b "文本像错误但状态成功" (文本原样, 不再包 `[Error]`)

### P3/P4/P5 回归结果 (Windows debug)

| 模块 | 结果 |
|---|---|
| `wire_roundtrip` | 249 / 0 (新增版本协商断言) |
| `remote_agent` | 524 / 0 (新增老客户端被拒 / 服务端过老被拒) |
| `plugins` | 609 / 0 |
| `client_plugins` | 746 / 0 |
| `filesystem` | 220 / 0 |
| `command` | 60 / 0 |
| `math_tools` | 82 / 0 |
| `rag_search` | 85 / 0 |

### 抬升 API 代次时的构建注意事项

- `AGENTXX_PLUGIN_API_VERSION` 是 `set(... CACHE STRING)`, **cache 有值时新默认值不生效**:
  抬代次必须显式 `cmake -DAGENTXX_PLUGIN_API_VERSION=4 -B agent/build/windows-debug -S agent`
  (顶层随后会把 `CMAKE_*_FLAGS` 里的旧 `-DPLUGINXX_API_VERSION=3` 剥掉再追加新的)
- 编译参数一变, MSVC 的 FileTracker 判定全部源文件过期 —— 整棵树 (含 fmt/ftxui/curl 等依赖)
  重编一次, 这是抬代次的固有代价
- **插件动态库不随顶层的 ALL 构建**: `plugins/` 是 lib 的子项目 (独立 slnx, 输出直接写到
  `${EXEC}/plugins/<插件名>/`), 顶层 `cmake --build` 只会编它们的 `.obj`。
  抬代次后必须再显式编一次:
  `cmake --build agent/build/windows-debug/agentxx_lib_repo-prefix/src/agentxx_lib_repo-build/agentxx_plugins_builtin --config Debug --parallel 6`
  否则 `exec/plugins` 里还是上一代 DLL, 测试会报 "API 代次不匹配" (`plugins` 模块 10 处断言)
- 自写用例踩到的坑: 31.5b 最初用"永不完成也不上报"的工具验证超时, 超时后插件侧那次调用
  一直不结算 -> `unloadAsync("example_plugin")` 等未完成调用超时失败, 之后 `example_plugin`
  一直处于已加载状态, 又连锁出 "duplicate or currently loading name" 等 9 处失败 (该模块耗时
  从 15s 涨到 150s)。改成 `blocking_tool` + 业务体睡得比 `default_timeout_ms` 久, 断言后
  等业务体跑完再注销, 回归干净

## P5 收尾 (全量回归暴露的问题与修法)

首轮全量跑出 10 处失败 (41170 passed / 10 failed), 逐个定位后修掉 9 处, 最终只剩与本任务
无关的 `boundaries`:

1. `codegraph` (1 处): "空 query 抛异常" 的断言已跟新契约不符 —— 插件工具的参数错误经 SDK
   边界变成失败状态, 不再向调用方抛异常。改成断言 `status == Failed` + 正文含 `` `query` is empty ``。
   之后又跑出 1 处是 `.swift` 夹具入库超时 (索引 warmup 等待), 重跑即过 —— 环境抖动。
2. `screen_capture` (`suite exception`): 工具失败时正文不再包 `{"ok":false,...}` 的 JSON,
   而旧用例无条件 `Json::parse(...)`, 解析所以直接抛进套件 (整模块 0 通过)。改成
   成败看 `status`: 成功才解析 JSON, 失败断言正文给出原因 (无桌面会话的机器照样能跑完),
   越界屏幕下标那条改为断言 `Failed` + 原因。
3. `mcp` (1 处): 我在 P5 加的 `fail` 假工具把 `isError` 写在 handler 返回值里, 而测试用
   `McpServer::handleToolsCall` 会把 handler 返回值整体包成一个 content 项并**强制**
   `isError=false` —— 该假服务端表达工具失败的方式是 handler 抛异常。改成抛
   `std::runtime_error("boom from mcp")` 后 `isError=true`, 客户端映射成 `Failed`。
4. `plugin_example_chatbot` (5 处): 用例里 `callTool` 自己拼 `[Error: ...]` 文本再用
   `rfind("[Error")` 判成败 —— 典型的"看文本判成败"。新增 `callToolResult` (返回
   `ToolResult`), `callTool` 改为它的 `toolResultTextForModel` 视图; 10 处判成败的断言
   改看 `status` (失败时正文仍校验原因, 例如 `找不到` / `超出可撤回范围`)。
5. `assembly_snapshot_io` (1 处, 属既有): 断言 "assembled_count = enabled_count + 1"
   写死了"恰好一个工具被过滤"; 事件中枢给每个 agent 注册了 `agentxx_event` 之后被过滤的
   工具变成两个。改成把循环里数出来的 `filteredCount` 加起来 (断言关系本身, 不写死条数)。

其余模块 (含 `toolcall_parallel` / `message_supplement` / `summarization` / `cancel` /
`race_guards` / `tui_*` / `event_*` / `plugin_*` / `client_plugins` / `jsonl_mode` / `agent`)
全部 0 失败。

### 全量回归结果 (Windows debug, 41232 passed / 1 failed)

唯一失败是 `boundaries` 的规则 15 (`agent/third_party/cxx_pluginxx_ui` 里 5 处 `载体`),
子模块未改动, 与本任务无关 (改动前就是这样)。

## P6 已完成 (文档与生成物)

- `AGENTS.md` 的"工具错误约定"整段重写成"工具结果与错误约定": `ToolResult{text, status}`、
  成败只看状态、执行层唯一排版处、插件边界 (`tools` 表 v3) 都写清; 删除旧的
  "上述失败文本也算失败"与 `hasError` 描述
- `docs/zh-cn/design/plugins.md`: §8 的"工具错误与参数检查约定"重写 (状态位 + 正文分工、
  `toolResultTextForModel` 唯一实现、`awaitPluginOpResult` 不再抛业务失败、JS 插件 `{status,text}`);
  API 代次段 (3 -> 4) 与 `agentxx.agent.tools` 表版本 (2 -> 3) 同步; 运行时通道事件表把
  `hasError` 改成 `status`
- `docs/zh-cn/design/index.md`: delta 公共字段与 `ToolEnd` 说明改成 `status`
  (并说明"正文里的 `[Error: ...]` 不代表失败")
- `docs/zh-cn/design/events.md`: §5 的"首行前缀限制"改成"状态与正文的分工"
  (那套前缀清单与 `tool_result_text.h` 已删除)
- `docs/zh-cn/design/tui.md`: 工具折叠头/装饰按 `status` 着色 (失败红 / 中断黄 / 取消普通)
- `docs/en/design/index.md`: delta 字段同步为 `status`
- 生成物: `agent/schema/wire-schema.json` 与 `docs/zh-cn/design/wire-protocol-fields.md`
  已在 P2 重生成 (`protocolVersion` 2)。注意 `status` 是**条件字段** (只在 `ToolEnd` 且
  非 `ok` 时写), 所以它不会出现在字段导出表里 —— 想让它出现只能额外加一个 `ToolEnd` 样本,
  而样本表要求"一条消息一个样本且顺序与变体一致", 加样本会破坏该断言, 因此这里不做
- 拼写收尾 (文档侧): `is_cancelled` -> `is_canceled` (会话表成员)、`OpCtl::throw_if_cancelled()` ->
  `throw_if_canceled()`、`ctl.cancelled()` -> `ctl.canceled()`、subagent.done 载荷键按代码写成
  `Canceled`; 指 neograph 的地方统一写成 `neograph::graph::CancelledException`
  (它的拼写按约定保留), 涉及 `index.md` / `develop.md` 的中英四份文档

## 复查修复 (实施后代码审查发现的问题)

对 P0~P6 的成品逐层核对后修掉 5 组问题 (都在原计划的验收标准范围内, 属实现没跟上设计)。

### R1 短路径结果漏掉执行层排版 (模型可见的回归)

现象: `tool not found: X` / `permission denied: R` / `repeated identical call denied ...` /
参数 JSON 解析异常 / prepare 阶段异常, 模型读到的是**裸错误内容**, 没有 `[Error: ...]`。
原因: `prepareToolCall` 产出的是纯内容 (`ToolResult::fail(...)`), 而写消息处按"缓存命中的
正文已经排版过"的假设原样照抄 (`toolcall.cpp` 的 shortCircuit 分支)。中断缓存命中那条确实
已排版, 新建的失败结果没有 —— 两者走了同一行。原计划 §2.3/§2.6 与 `prompt.cpp` 都写着这些
情况带 `[Error: ...]`, 测试一个都没覆盖 (全测试目录搜 `permission denied` / `tool not found`
无断言)。

修法 (`nodes/toolcall.cpp`):
- 新增 `formattedShortCircuit(ToolResult)`: 产生短路径结果时就套 `toolResultTextForModel`,
  维持 "shortCircuit.text 已排版" 的不变量 (头文件注释同步说明)
- 6 处产生点全部经它: 工具不存在 / 参数 JSON 错 / `prepareToolInvocation` 返回 (权限拒绝与
  重复调用被拒) / 权限复验拒绝 / prepare 异常 (两处) / 中断占位
- `execTool()` (目前无调用者) 同一分支一并收口
- `insertAbortedToolResults` (中间件阶段异常中止整批) 改为经 `formatToolResultText` 生成正文,
  不再手写 `[Error: ...]`

### R2 `[Interrupt]` 占位文本变空

重复调用确认触发的中断占位用 `ToolResult::interrupted()` (文本为空), 加上 R1 的"原样照抄",
`interruptToolcallCache` 里的 `content` 变成空串 (旧值是 `"[Interrupt]"`, 计划 D6 要求保留)。
修法: 占位产生时经 `formattedShortCircuit`, 空文本被补成 `[Interrupt]`。

### R3 `agentxx_execute_command` 的"执行前取消检查"成了死代码

`execute_command_impl.h` 里两个重复的 `if (已取消) { ... }` 块, 块内是
`// 说明\n        co_return ToolResult::canceled(...)` —— 一个**字面 `\n`** 把 `co_return`
注释掉了 (编辑事故), 于是已取消的调用照样起子进程, 再由 watcher 杀掉, 返回 `[ExitCode] ...`
且状态是成功。修法: 合成一个块并把 `co_return` 恢复; 顺带让 `runProcPipeline` 的终态符合
计划 D5 (超时 -> `failed`, 运行中被取消 -> `canceled`, 其余含非零退出码仍是 `ok`)。

### R4 审计漏掉的 "整次失败但报成功"

- `agentxx_filesystem` 的 list: 取消/超时原先作为 `[Error: Canceled]` / `[Error: timed out ...]`
  **行**拼进条目列表, 整次调用仍 `ok`。改为记录终止原因 (`StopReason`), 由 `finishResult()`
  给终态: 取消 -> `canceled` (正文首行说明"列表不完整"), 超时 -> `failed` (首行写原因与
  建议), 正常结束 -> `ok` (与原来一致; 空目录仍补 `[Empty]`), 不再往结果里塞 `[Error: ...]`。
- `agentxx_math`: `MathEvalException::what()` 自带 `[Error] ` 前缀, 执行处再 `starts_with` 剥掉。
  改为异常文本只写错误内容 (不再自己贴标记), 剥离逻辑删除。

### R5 收尾遗漏

- TUI 折叠头预览: 中断 (`interrupted`) 之前只有展开体是警告色, 折叠头仍是普通色;
  与 `tui.md` 的说法对齐 (失败红 / 中断黄 / 取消与成功普通色)
- `markToolStatus` 注释写"只有非 Ok 才写", 实现却每条都写 (成功消息也多带一个
  `status: "ok"`); 改为非 Ok 才写 (缺字段 = ok, 读侧本来就是这么判的)
- 文档/注释残留 `[User canceled]` (工具占位语义): `index.md`、`roadmap.md`、`agent_tui.cpp`
- `prompt.cpp` 的 `[Canceled]` 说明改成"本轮被停掉 (用户取消/客户端断开), 结果未知",
  不再与 `[Interrupt]` 混用 "interruption" 一词

### 新增/调整用例

| 模块 | 用例 | 断言 |
|---|---|---|
| `toolcall_parallel` | T6 `test_short_circuit_failure_text` | 工具不存在 -> `[Error: tool not found: ...]` + `extra.status=failed`; 参数 JSON 错 -> 以 `[Error: ` 开头 + `failed`; 正常执行 -> 正文不加标记且不写 `extra.status` |
| `command` | `test_command_cancel_registry_preCanceled` (改) | 预取消不再起子进程: 状态 `Canceled`、正文 `Command canceled before execution.`、< 2s、注册表无残留 |
| `filesystem` | `test_list_canceled_status` | 列表取消: 状态 `Canceled`、正文以 `Canceled while listing` 开头且不含 `[Error` |

### 回归 (Windows debug, ASan)

| 模块 | 结果 |
|---|---|
| `toolcall_parallel` | 58 / 0 (原 48, 新增 T6) |
| `command` | 60 / 0 |
| `filesystem` | 221 / 0 (原 220) |
| `math_tools` | 82 / 0 |
| `cancel` / `race_guards` / `message_supplement` | 45 / 68 / 95, 0 失败 |
| `plugins` / `client_plugins` | 609 / 746, 0 失败 |
| `wire_roundtrip` / `wire_schema` / `summarization` | 249 / 1325 / 454, 0 失败 |
| `event_tools` / `event_hub` / `permission` / `subagent_tool` / `toolcall_args` | 全 0 失败 |
| `tui_tool_header` / `tui_stream` / `tui_ui_items` / `tui_form` / `tui_widget` / `interrupt_ui` | 全 0 失败 |
| `remote_agent` / `agent` / `jsonl_mode` / `assembly_snapshot_io` / `config_keys` | 全 0 失败 |
| `boundaries` | 18 / 1 (仍是子模块 `cxx_pluginxx_ui` 的规则 15, 与本任务无关) |
