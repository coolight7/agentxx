- 难度: C
- 类型: bug修复
- 时间: 2026-10-31
- 需求:
```md
请分析 agentxx_chatbot 插件的 @触发判断，我发现在有些群聊中，即使@别人，
模型也经常误以为是@自己，然后触发回复。
（保持 @全体成员 不包括 bot 的判断；不加兼容开关，直接做严格判定；
  让模型拿到的上下文信息更直观全面；增加唤醒原因可行。）
```

## 现象

群里有人 @ 的是别的成员（尤其是另一个机器人，比如元宝），机器人却当成在叫自己：
立刻开始一轮、输入里那条消息标成「@机器人」，模型据此回话；`always` 群更明显。

关键线索是「**有些群聊**」——群里还有别的机器人的才出问题。

## 根因

`qq_normalize.cpp` 在全量消息（`GROUP_MESSAGE_CREATE`）里判 @ 用的是：

```cpp
if (item.value("bot", false)) {
    botMentioned = true;          // mentions[] 里任意一项 bot: true
}
...
if (out.fromFullMode && botMentioned) {
    out.mentionedMe = true;       // 当成“@ 了我”
}
```

`mentions[]` 里的 `bot: true` 只说明「被 @ 的是某个机器人」，不说明是**本**机器人。
群里只要还有别的机器人，@ 它就必然命中这条规则。代码注释当时写的假设是
「群里通常只有本机器人一个」，设计文档（`plan.md` §12.1）记的恰恰相反：
`mentions[]` 是「被 @ 的用户（**不含机器人自身**）」。

误判之后还有两处放大：

1. `chatbot_engine.cpp` 的 `flush(..., "mentioned")` —— 被 @ 立刻唤醒，不等聚合窗口；
2. `turn.hadMention = true` → `fallbackSend`（`behavior.fallbackReply: mention`）
   把模型没调工具的正文也发出去，于是「没说发也发出去了」。

## 修复

### 1. @ 判定改严（`platform/qq/qq_normalize.{h,cpp}`）

删掉 `bot: true` 这条依据，只认三种来源，并记进 `InboundEvent::mentionSelf`
（`MentionSelfSource`：`None < Nickname < Openid < EventType`，合并时取较大者）：

| 依据 | 来源 | 说明 |
|------|------|------|
| `EventType` | `GROUP_AT_MESSAGE_CREATE` / 单聊 | 平台直接给的 @ 语义，最可靠 |
| `Openid` | `mentions[]` / 正文 `<@openid>` 命中已知自身 openid | 硬证据 |
| `Nickname` | `mentions[]` 里的昵称等于机器人昵称（READY 的 `d.user.username`） | 自身 openid 还没学到时的兜底 |

补强：

- 自身 openid 现在**按群**记（`QqSelfIdentity::groupOpenids`），昵称命中时顺手把该群的
  openid 学下来，之后就是精确判定（只对平台标了 `bot: true` 的项学，免得同名成员被当成自己）；
- 正文标记的自检用「这条消息之前」的身份（`beforeEvent` 快照），否则同一条消息里刚学到的
  openid 会让判定依据从 `nickname` 变成 `openid`，日志与计数就分不清靠什么认出来的；
- 认不出是谁时记 `mentionOthers` / `mentionBots`（渲染成 `@别人(...)` / `@别的机器人(...)`），
  正文里认不出昵称的 `@` 写 `@其他成员`（原来的「@成员」太含糊，模型会当成自己），
  认得出是自己但没昵称的写 `@你`；
- `@全体成员`（`<@all>` 与 `mentions[]` 里的 `all`）仍然**不算** @ 了我。

### 2. 同一条消息两份事件改成合并（`chatbot_core` 的 `mergeSameMessage` + engine）

全量模式下「@ 了我」会按 `GROUP_MESSAGE_CREATE` 与 `GROUP_AT_MESSAGE_CREATE` 各推一次，
以前按 `msgId` 去重直接丢后到的那份 —— 若先到的是没有 @ 语义的那份，就会出现
「被 @ 了却不回」。现在把新信息并进缓冲里那条（只补空、@ 取更可靠来源），因此从
「没 @ 我」变成「@ 了我」时立刻唤醒。

### 3. 让模型拿到的上下文更直观（`chatbot_core` 的 `renderInputBlock` + 人设提示词）

- 每行发送者后面标出「这条 @ 了谁」：`@机器人` / `@别人(小郑、小刘)` / `@别的机器人(元宝)`；
- 消息之前插入一行「本轮唤醒原因」（`wakeReasonNote`）：被 @ 唤醒（并点名 `[#n]`）、
  `always` 窗口到期、缓冲攒满、通道刚就绪…，让模型知道这轮为什么开始；
- 人设提示词新增强制附加段 `chatbot-input-guide`（`prompt.append` 同名键可覆盖），
  把上面几种标记的读法直接写进系统提示词，不再指望模型从正文猜；
- 诊断：`chatbot_status` 新增 `mention.byEventType/byOpenid/byNickname/selfUnknown`
  与 `selfOpenidByGroup`，`counters.wakeMentioned`（按会话也有），
  全量消息的 @ 判定在 debug 日志里打出依据与 mentions 逐项摘要。

## 验证

- `chatbot_qq`：新增/改写 12/12b/12c/12d/13/14b/15 用例 —— @ 别的机器人不算我、
  身份未知时不算我、昵称兜底能认出来并学会该群 openid、同名人类成员不会被学进身份表、
  `all` 不算我、@ 别人时 `mentionOthers` 有值；正文渲染 `@其他成员` 与 `@你`；
- `chatbot_core`：新增 `testMergeSameMessage`（补空不覆盖、@ 取更可靠来源、列表去重、
  只在升级时返回 true）与 `testWakeReasonNote`（各原因码文案、点名 `[#n]`、不缺行），
  输入块用例覆盖 `@别人(...)` / `@别的机器人(...)` 与唤醒原因行的位置；
- `plugin_example_chatbot`（真实插件全流程）201 条通过。

## 涉及文件

- `agent/plugins/agentxx_chatbot/chatbot_types.{h,cpp}`：`MentionSelfSource`、`mentionSelf`、
  `mentionOthers` / `mentionBots`、`mentionedMe()`；
- `agent/plugins/agentxx_chatbot/platform/qq/qq_normalize.{h,cpp}`：严格判定、按群身份、渲染；
- `agent/plugins/agentxx_chatbot/platform/qq/qq_platform.{h,cpp}`：身份写回、计数、status；
- `agent/plugins/agentxx_chatbot/chatbot_core.{h,cpp}`：`mergeSameMessage`、`wakeReasonNote`、
  输入块标记与唤醒原因；
- `agent/plugins/agentxx_chatbot/chatbot_engine.{h,cpp}`：重复事件合并、唤醒原因传递、计数；
- `agent/plugins/agentxx_chatbot/agentxx_chatbot.cpp`：`chatbot-input-guide` 段、调试注入参数；
- `agent/plugins/agentxx_chatbot/chatbot_tools.cpp`、`platform/fake/fake_platform.cpp`：跟随改字段；
- `agent/test/core/test_chatbot_qq.cpp`、`agent/test/core/test_chatbot_core.cpp`；
- 文档：`agent/plugins/agentxx_chatbot/README.md` §2.5 / §3。
