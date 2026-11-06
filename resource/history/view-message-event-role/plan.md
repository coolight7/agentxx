# 设计方案: viewMessage 事件类型与用户消息折叠

## 1. 背景

事件中枢（`EventHub`）投递进会话的输入，在 LLM 上下文里必须是 `user` 消息（模型只认
user/assistant/tool），因此展示历史（`Session::viewMessages`）里当初也直接用了
`Role::User` + `source = "event:<事件名>"`，界面在用户消息正文前补一行
`| [事件] xxx` 做区分。

问题：

- 事件正文是中枢渲染好的多行文本（`[Event] <事件名> (<模式>)` + 各字段 + 值），
  一次投递常常十几行，直接铺满消息列表；
- 用户消息不可折叠，只能一直占着屏幕；
- 事件消息与用户自己敲的消息在列表里长得几乎一样，只能靠那行小字区分。

## 2. 现状调查（文件 / 函数）

| 位置 | 作用 |
|------|------|
| `agent/lib/include/agentxx/agent/conversation_types.h` `ViewMessage` | 展示消息模型；`Role` 枚举 / `toJson` / `fromJson` / `hasVisibleContent` / `makeText` |
| 同上 `kInputSourceEventPrefix` / `eventNameOfInputSource` | 来源标记的唯一取值与解析 |
| `agent/lib/src/agent/base_agent.cpp` `BaseAgent::runTurnAsync` | 用户输入/事件输入落展示历史 + 发 TurnStart |
| `agent/lib/src/nodes/session_context.cpp` `appendUserViewMessage` | next-step/inject 注入输入落展示历史（未写 `source`） |
| `agent/lib/src/agent/context.cpp` `onAppendViewMessage` | 首条用户消息同步会话统计标题 |
| `agent/lib/src/agent/session_store.cpp` `SessionStore::appendViewMessage` | 会话库落库 + `meta.title` 自动标题 |
| `agent/client/src/io/tui/agent_tui.cpp` `onDelta` (TurnStart) | 客户端回显用户消息（取 `delta.source`） |
| `agent/client/src/io/tui/components/message_list.cpp` | 消息列表：`isBlankMessage` / `quickHeight` / `buildMessageBlock` / 折叠命中登记 / 跳转按钮 |
| `agent/client/src/io/tui/tui_i18n.cpp` `msg.eventSource` | ` [Event] {}` / ` [事件] {}` |
| `agent/client/src/io/stdio/agent_stdio.cpp` `onSync` | CLI 历史回放（只看 `Role::User`） |

## 3. 方案

### 3.1 模型层（conversation_types.h）

- `Role` 末尾追加 `Event`（枚举值追加在最后，已有取值不变）。
- 新增 `static Role roleOfInputSource(std::string_view source)`：
  `eventNameOfInputSource(source)` 非空 → `Event`，否则 `User`。
- `makeText`：`Role::Event` 与 `Role::Tip` 一样默认 `collapsed = true`。
- `hasVisibleContent`：`Event` 恒为有内容（头部始终渲染出一行）。
- `viewMessageRoleToString` → `"event"`；`viewMessageRoleFromString("event")` → `Role::Event`。
- `fromJson`：
  - 新角色的专属子结构分组（与 User/System/Assistant 一样没有子结构）；
  - 兼容归一化：解析出的 role 是 `User` 且 `source` 是事件来源时改成 `Event`。

### 3.2 服务端写入

- `BaseAgent::runTurnAsync`：`makeText(ViewMessage::roleOfInputSource(inputSource), ...)`。
- `appendUserViewMessage`：同样用 `roleOfInputSource(input.source)`，并补写 `vm.source`。
- 标题：`context.cpp` 的 `onAppendViewMessage` 与 `session_store.cpp` 的自动标题把
  `Role::Event` 与 `Role::User` 一并当作"输入消息"。

### 3.3 客户端渲染（TUI）

- `agent_tui.cpp` 的 TurnStart：`makeText(roleOfInputSource(delta.source), ...)`。
- 反向扫描更新 Think 消息的边界判定（`onDelta` 的 ThinkToken 空文本分支）把 `Event`
  也算作输入边界。
- `message_list.cpp`：
  - `isBlankMessage`：`Event` 头部始终可见 → false；
  - 折叠命中登记：新增 `User` / `Event`（点击整块切换 `collapsed`）；
  - `quickHeight`：`User` / `Event` 的折叠/展开两态；
  - `buildMessageBlock`：
    - `User`：开头标记 `- `（展开）/ `+ `（折叠，后接 dim 单行预览，不加分隔符），
      正文仍是 `renderPlainText`（纯文本折行，不解析 markdown）；附件卡片照旧在最上方；
    - `Event`：头部 `+ [事件] <事件名>`（折叠时再补 `· <单行预览>`），展开态下一行显示
      全文；颜色统一取 `theme.toolColor`（与工具消息一致），正文用 `renderPlainText`
      保留硬换行。
- 右侧跳转按钮（上一条/下一条用户消息）仍只认 `Role::User`：事件不是用户敲的。

### 3.4 CLI

`agent_stdio.cpp` 的 `onSync` 把 `Role::Event` 与 `Role::User` 一起按 `> ` 回放
（CLI 没有折叠概念，保持原样输出）。

## 4. 边界与兼容

- 旧客户端收到 `"role": "event"`：`viewMessageRoleFromString` 不认识的取值 →
  `fromJson` 回退 `Role::User`，按原来的用户消息显示（长一点但不崩）。
- 旧会话库 / 旧服务端：`role=user` + `source=event:*` 在 `fromJson` 归一化成 `Event`。
- 链式哈希：`restore` 用加载后的消息重算，归一化对两端一致，不影响同步判定。
- LLM 上下文、`wire` 结构与 `Session::messages` 不变。

## 5. 验证

- 编译：`agent/script/windows_debug_build.bat`（Debug + ASan/UBSan）。
- 测试：
  - `agentxx_test tui_scroll`（渲染/折叠/来源标记/跳转按钮）；
  - `agentxx_test tui_stream`（TurnStart 角色、ViewMessage JSON 往返）；
  - `agentxx_test event_delivery`（事件投递落展示历史的角色与来源）；
  - `agentxx_test session_persistence` / `session_schema`（序列化往返）。
- 新增用例：事件消息默认折叠、点击展开显示全文；用户消息折叠/展开与 `+` / `-` 标记。

## 6. 风险

- 用户消息变成可折叠后，点击消息体会切换折叠；拖选复制走全局拖选判定
  （`agent_tui.cpp` 的拖选松开分支先返回），不受影响。
- 折叠态是客户端本地状态（不落库），Sync 后回到默认（用户消息展开、事件折叠），
  与 Think/Tool 的现状一致。
