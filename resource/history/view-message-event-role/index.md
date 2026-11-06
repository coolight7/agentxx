# 任务卡片: viewMessage 增加事件类型 (Role::Event)

## 目标

1. `ViewMessage` 增加专属的 `Role::Event`：事件中枢投递进会话的输入不再借用 `Role::User`
   做展示（LLM 上下文中它仍是 `user` 消息，只改展示角色）；
2. TUI 渲染成与工具消息同款的可折叠头 `+ [事件] <事件名>`，配色仿照工具消息，
   折叠态带单行预览，展开态显示全文 —— 事件正文通常很长，默认折叠，不再一屏塞满；
3. 普通用户消息也支持展开/折叠，开头符号统一成 `+` / `-`（原来是 `> `）。

## 状态

已完成（实现 + 测试 + 文档）。

## 关键决策

1. **角色只管展示**：`Role::Event` 只影响展示历史/客户端渲染，`Session::messages`
   （LLM 上下文）仍是 `user` 消息，处理路径完全不变。
2. **角色由来源推导**：唯一入口 `ViewMessage::roleOfInputSource(source)`
   （`event:<事件名>` → `Event`，其余 → `User`），服务端两处写入点
   （`BaseAgent::runTurnAsync` / `nodes/session_context.cpp` 的 `appendUserViewMessage`）
   与客户端 TurnStart 回显共用，避免各自判断。
3. **默认折叠**：`ViewMessage::makeText(Role::Event, ...)` 自动 `collapsed = true`
   （与 Tip 同一处理），客户端 `Sync` 恢复的历史也带 `collapsed`。
4. **老数据兼容**：`ViewMessage::fromJson` 里 `role=user` 且 `source=event:*` 归一化为
   `Role::Event`（老会话库、老服务端 Sync 都能直接按新样式显示）；新角色字符串 `"event"`
   在旧客户端上不认识 → 回退 `Role::User`，不会崩。
5. **标题保持原样**：会话自动标题（首条用户消息预览）把 `Event` 与 `User` 同等对待，
   事件开头的会话仍有标题。
6. **顺带补上遗漏**：`appendUserViewMessage`（next-step/inject 输入落展示历史）此前没写
   `source`，事件注进去的输入在界面上没有任何标记；本次一并写入。

## 结论

- `ViewMessage::Role` 现有 8 种：User / Assistant / Think / System / Tool / Interrupt / Tip / Event；
  事件输入在展示历史里是 `Event`，在 LLM 上下文里照旧是 `user`；
- TUI 折叠头统一为 `+` / `-`（Think / Tool / System / Tip / Event / **User**），
  用户消息前缀 `> ` 改为 `- `（展开）/ `+ `（折叠，带 dim 单行预览）；
- 事件消息头 `+ [事件] chat.message · <单行预览>`（i18n 键 `msg.eventSource`），
  正文按纯文本渲染、颜色取 `theme.toolColor`，点击整块切换折叠；
- 回归：全量套件 **41,948 项通过 / 1 项失败**，唯一失败是 `boundaries` 规则 15 在
  `agent/third_party/cxx_pluginxx_ui` 里的用词（既有问题，与本次无关）；
  `tui_scroll` / `tui_stream` / `event_delivery` 等受影响模块全部通过。
