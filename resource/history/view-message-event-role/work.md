# 实施记录: viewMessage 事件类型与用户消息折叠

## 进度

- [x] 模型层: `ViewMessage::Role::Event` + `roleOfInputSource` + 序列化 + 默认折叠 + 老数据归一化
- [x] 服务端写入: `BaseAgent::runTurnAsync`、`appendUserViewMessage` 按来源决定角色（并补写 `source`）
- [x] 会话标题: `onAppendViewMessage` / `SessionStore::appendViewMessage` 把 Event 当作输入消息
- [x] 客户端: TurnStart 回显按 `delta.source` 推导角色，Think 回溯边界把 Event 计入
- [x] TUI 渲染: `Role::Event` 折叠头（仿工具配色）、`Role::User` 折叠、可折叠命中登记、
      `quickHeight`、`isBlankMessage`
- [x] CLI: `onSync` 按用户消息回放 Event
- [x] 测试: 更新 `tui_scroll` / `tui_stream` / `event_delivery`，新增事件折叠头与用户消息折叠用例
- [x] 文档: `docs/zh-cn/design/tui.md`（§2.17 重写、新增 §2.18、§2.14 判据补充）、
      `docs/zh-cn/design/events.md` §5.2、`AGENTS.md` 事件段一句

## 改动文件

| 文件 | 改动 |
|------|------|
| `agent/lib/include/agentxx/agent/conversation_types.h` | `Role::Event`（枚举末尾追加）、`roleOfInputSource`、`makeText` 默认折叠、`hasVisibleContent`、`toJson`/`fromJson`（`"event"` + 老数据归一化）、注释 |
| `agent/lib/src/agent/base_agent.cpp` | `runTurnAsync` 用来源决定展示角色 |
| `agent/lib/src/nodes/session_context.cpp` | `appendUserViewMessage` 同上 + 补写 `vm.source` |
| `agent/lib/src/agent/context.cpp` | 自动标题把 Event 当输入消息 |
| `agent/lib/src/agent/session_store.cpp` | 落库自动标题同上 |
| `agent/client/src/io/tui/agent_tui.cpp` | TurnStart 角色推导；Think 回溯边界包含 Event |
| `agent/client/src/io/tui/components/message_list.cpp` | `isBlankMessage` / 折叠命中登记 / `quickHeight` / `buildMessageBlock`（User 折叠 + 新 Event 分支） |
| `agent/client/src/io/stdio/agent_stdio.cpp` | `onSync` 把 Event 与 User 一起回放 |
| `agent/test/core/test_event_delivery.cpp` | 展示消息按 Role::Event 查找 + 断言默认折叠 |
| `agent/test/client/test_tui_stream.cpp` | 场景 16 改为断言角色/折叠/JSON 往返/老数据归一化 |
| `agent/test/client/test_tui_scroll.cpp` | 事件折叠头场景重写、新增用户消息折叠场景、跳转按钮断言前缀改 `- ` |
| `docs/zh-cn/design/tui.md` / `events.md` / `AGENTS.md` | 见上 |

## 验证

构建: `agent/script/windows_debug_build.bat`（Debug + ASan/UBSan）通过；
产物在 `agent/build/windows-debug/exec/`（复制到 `windows-debug-output` 因为本机 agentxx
正在运行而失败，属已知情况，不影响运行测试）。

测试（`agentxx_test.exe`，`--log-level error`）:

| 模块 | 结果 |
|------|------|
| `tui_scroll` | 926 通过 / 0 失败（含新增的事件折叠头与用户消息折叠用例） |
| `tui_stream` | 193 / 0 |
| `tui_widget` | 194 / 0 |
| `tui_form` / `tui_surface` / `tui_tool_header` / `tui_lazy_view` / `tui_interrupt` | 全部通过 |
| `event_delivery` / `event_hub` / `event_stream` / `event_subagent` | 全部通过 |
| `session_schema` / `persist_semantics` / `session_persistence` / `storage_consistency` | 全部通过 |
| `wire_roundtrip` / `wire_schema` / `config_keys` / `test_modules` | 全部通过 |
| `async_work` / `async_tools` / `event_tools` / `observability` / `misc_fixes` / `chatbot_task` | 全部通过 |
| 全量套件（`agentxx_test --log-level error`） | **41,948 通过 / 1 失败**，唯一失败为下行的 `boundaries` 既有问题 |
| `boundaries` | 1 项失败：规则 15 在 `agent/third_party/cxx_pluginxx_ui/.../item.h` 的用词（既有问题，与本次无关） |

## 与计划不一致 / 需要留意的点

- `appendUserViewMessage` 之前**没有**把 `input.source` 写进展示消息，本次一并补上；
  否则 next-step/inject 注入的事件在界面上仍是一条普通用户消息。
- `agent/lib/src/agent/base_agent.cpp` / `context.cpp` 里还有本工作区之前就存在的改动
  （默认模型解析走 `AgentConfig::defaultModelName()`），不属于本任务，未做修改。
- `event:pending-rollover`（轮末把残留 next-step 输入提升为新一轮时使用）也会显示成
  `[事件] pending-rollover`：它本身就是"事件投递的输入被合并进新轮"，折叠展示仍然成立，
  只是名字是框架内部标记。若日后觉得别扭，可在 `rolloverLeftoverNextStepInputs` 里换成
  更中性的来源。
- 折叠状态是客户端本地状态（不落库、不随 Sync 保留），与 Think/Tool 的现状一致；
  事件消息每次重连/切换会话回到默认折叠。
- 旧客户端收到 `"role": "event"` 时 `viewMessageRoleFromString` 不认识 → 回退 `Role::User`
  （按原来的用户消息展示，长一点但不崩）。
