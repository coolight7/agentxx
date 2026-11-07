- 难度: C
- 类型: bug修复 + 行为调整
- 时间: 2026-11-08
- 需求:
```md
请修复机器人插件，当模型执行较久的任务完成后，调用tool回复消息时引用的msg_id可能过期，
导致回复失败，请修改tool自动处理如果引用的msg_id已经过期、发送请求失败提示msg_id过期，
则自动改为@发送，并在tool的返回中留下发送成功和警告提示；修改 chatbot 接收消息时，
如果正在执行会话则插入到 next-step，以便仍在执行会话时，消息可以快速在 modelcall 时
插入被及时了解、处理。
```

## 现象

1. **长任务之后的第一句回不出来。** 模型跑完一个很长的工具链（或后台任务回到会话）再调
   `chatbot_send_message`，出站带着本轮那条消息的 `msg_id`（被动回复）。QQ 的被动窗口是
   **群 5 分钟 / 单聊 60 分钟**，超了平台直接拒绝，当前实现只记一条 `failed` 日志：
   消息**彻底丢了**，模型还以为发出去了（工具返回的是「已排队」）。
2. **一轮跑着的时候新消息没人管。** `flush()` 发现通道 `busy` 就把整批消息留在聚合缓冲里，
   等 `TURN_END` 才投。于是「群里 @ 机器人问一句」在上一轮结束前完全不可见 —— 模型这一轮
   做的所有事都可能已经过时（比如它正在按旧要求干活）。

## 根因

- 出站只有「被动回复次数用尽」这一条降级路径（`ChatbotOutbound::pump` 里按
  `kMaxPassivePerMessage` 计数），**平台判定的窗口过期没有处理**：`SendResult` 里没有
  「这是被动回复过期」这个信息，失败就只是失败；
- `chatbot_send_message` 是「提交即返回」：`submit()` 把消息入队就回 `queued`，
  平台结果要等回调才知道 —— 工具没法把真实结果（含降级）告诉模型；
- `flush()` 的 `busy` 分支写死「攒到 TURN_END」，没有用主程序的 `delivery: next-step`
  （`Session::pendingInputs_` + modelcall 边界注入）这条现成的通道。

## 修复

### 1. 被动回复过期 → 自动改发 @ 的主动消息

- 类型层：`SendResult::passiveExpired`（平台认得出来就置位）、`OutboundMessage::mentionOpenId`
  / `mentionName`（主动消息里 @ 谁）、`PlatformCaps::mentionInContent`；
  入站侧补 `InboundEvent::senderMentionId`（QQ 群 = `member_openid`、单聊 = `user_openid`）；
- 出站层（`ChatbotOutbound::pump` 的结果回调）：被动回复失败且
  `passiveExpired`（或失败原因像窗口过期）时，按 `overflowPolicy`
  - `active`（默认）：把同一条消息**插回队首**改发主动消息，清 `msg_id`/`msg_seq`，
    带上刚才那条消息说话人的 openid 让平台 @ 他；计入新计数 `passiveFallback`；
  - `truncate` / `fail`：沿用原来的语义；
- 平台层（QQ）：群聊里「@用户」只在 **markdown 消息**里生效（官方文档：`@用户`
  嵌入 `<qqbot-at-user id="member_openid" />`，支持的消息类型 = markdown 消息），
  所以带 `mentionOpenId` 的群消息强制 `msg_type=2` 并在正文前加该标记；
  拿不到 openid 时退化成正文前缀 `@昵称 `；
- 工具返回：`chatbot_send_message` 现在会**等工作线程上的发送结果**（`OutboundWait`，
  上限 `behavior.sendResultWaitMs`，默认 4000ms），把
  `{"queued":1,"sent":1,"warning":"…msg_id 已过期, 已改为 @… 的主动消息发送"}` 交给模型；
  等不到就仍是 `queued`（不阻塞工具），彻底失败则按工具失败上报原因。

### 2. 会话正在执行时按 next-step 插进本轮

- `flush()` 不再在 `busy` 时直接返回：按 `delivery: next-step` 投递，主程序在**下一次
  modelcall 边界**把它写进本轮上下文（`drainPendingSessionInputs`），模型立刻能看到；
- 本轮记账（`TurnContext`）跟着延续：`[#n]` 编号接着上一段往下排，
  `sentCount` / `modelSent` / `startedAtMs` 不清零，`lastPassiveMsgId` 更新到最新那条，
  输入块里多一句「这段是中途插进来的」；
- 投递失败（运行时错误码）时把这批消息**放回缓冲**，等 TURN_END 再投，内容不丢。

## 验证

- `chatbot_core`：新增出站用例 —— 被动回复被判过期后自动改 @ 主动消息（第二次发送
  `passiveReplyTo` 空、`mentionOpenId` 保留）、`passiveFallback` 计数、等待器拿到 warning；
- `chatbot_qq`：入站补的 `senderMentionId` 取值；
- `plugin_example_chatbot`（真实插件全流程）：① 一轮在跑时注入新消息 → 第二次 modelcall
  的请求里就有它（不再等 TURN_END）；② 假平台判 `msg_id` 过期 → 出站出现「无被动 id +
  带 @」的第二条，工具返回里带警告；
- 编译：顶层 debug 构建 + `agentxx_test plugin_example_chatbot chatbot_core chatbot_qq`。

## 结论

两件事都做完了，代价与取舍写在 `work.md` 第 7 节：

- **被动回复过期**：平台判过期（或失败文本像过期）时，出站层把同一条消息插回队首、
  清掉 `msg_id` 改发主动消息，群里带上 `<qqbot-at-user id="…" />` 把人 @ 回来；
  工具改成「提交 + 等工作线程上的结果」，返回里给出 `sent` 与 `warning`
  （`behavior.sendResultWaitMs` 默认 4 s，等不到就退回「已排队」）。
- **会话执行中收到的消息**：`flush()` 不再攒到 `TURN_END`，按 `delivery: next-step`
  插进当前这一轮（主程序在下一个 modelcall 边界写进上下文）；本轮记账延续
  （编号接着排、额度不重置），投递失败则放回缓冲。

验证：`chatbot_core` 872 / `chatbot_qq` 136 / `chatbot_media` 69 / `chatbot_media_io` 54 /
`chatbot_task` 138 / `plugin_example_chatbot` 265 项全部通过（debug + ASan）。

## 偏差

- 任务里没有要求「工具等发送结果」，但要在返回里给出「成功 + 警告」就必须等这一次结果，
  所以加了 `behavior.sendResultWaitMs`（可设 0 关掉，回到纯排队语义）；
- 为了能在端到端用例里造出「平台判过期」的情形，新增调试工具 `chatbot_debug_platform`
  （仅 `behavior.debugTools` 打开时注册，生产不出现）；
- 另外顺手补了 QQ 群的 @ 说明：官方文档里「@用户」在群聊只在 markdown 消息里生效，
  所以降级那条会发成 markdown。

## 涉及文件

- `agent/plugins/agentxx_chatbot/chatbot_types.h`、`chatbot_outbound.{h,cpp}`、
  `chatbot_engine.{h,cpp}`、`chatbot_tools.{h,cpp}`、`chatbot_config.{h,cpp}`、
  `agentxx_chatbot.cpp`
- `agent/plugins/agentxx_chatbot/platform/qq/qq_normalize.cpp`、
  `platform/qq/qq_platform.cpp`、`platform/fake/fake_platform.{h,cpp}`
- `agent/test/core/test_chatbot_core.cpp`、`agent/test/core/test_chatbot_qq.cpp`、
  `agent/test/plugin/test_plugin_example_chatbot.cpp`
- 文档：`agent/plugins/agentxx_chatbot/README.md` §2.1 / §4 / §5
