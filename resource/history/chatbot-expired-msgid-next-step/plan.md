# 被动回复过期改 @ + 会话执行中按 next-step 插入

## 1. 背景

两件事都出在「一轮跑得久」这个场景：

1. 模型跑完长任务（工具链长，或后台任务结果回到会话）再回复，出站带的 `msg_id`
   已经超出平台被动回复窗口（QQ 群 5 分钟 / 单聊 60 分钟）。平台拒绝，当前实现只
   记 `failed`，消息丢掉，模型还以为发出去了。
2. 一轮在跑的时候群里又来了消息，`ChatbotEngine::flush()` 见通道 `busy` 就攒在
   聚合缓冲里等 `TURN_END`；期间模型完全看不到新消息。

## 2. 现状调查（文件 + 函数）

| 位置 | 现状 |
|---|---|
| `chatbot_outbound.cpp` `ChatbotOutbound::submit` | 切分 → 每段建 `Job` → 入该会话 FIFO → `pump()` |
| `chatbot_outbound.cpp` `ChatbotOutbound::pump` | 分配 `msg_seq`（`nextPassiveSeq`，只有「次数用尽」这一条降级）；`send_()` 异步，结果在回调里只记 `sent`/`failed` |
| `chatbot_types.h` `SendResult` | 只有 `ok` / `reason` / `platformMessageId` / `refIdx`，没有「被动窗口过期」这个信息 |
| `chatbot_types.h` `OutboundMessage` | 没有「主动消息里 @ 谁」的字段 |
| `platform/qq/qq_platform.cpp` `QqPlatform::doSend` | `msgType` 由 `msg.markdown` 决定；正文只拼附件提示，不会 @ 人 |
| `platform/qq/qq_normalize.cpp` `normalize` | `senderId` 取 `author.id` → `member_openid` → `user_openid`；没留「@ 他要用哪个 id」 |
| `chatbot_tools.cpp` `toolSendMessage` | `submit()` 后立刻返回 `{"queued":N,...}`，平台结果与它无关 |
| `chatbot_engine.cpp` `ChatbotEngine::flush` | `isBusy(channel)` → 记 debug 日志 + `state.stale = true` → 返回，等 `TURN_END` |
| `chatbot_engine.cpp` `ChatbotEngine::assembleInput` | 固定 `input["delivery"] = "next-turn"`；每次都**重置** `TurnContext`（编号从 1、`sentCount = 0`） |
| `agentxx_chatbot.cpp` `registerToolsOn` / `runToolOnIo` | 工具体在插件 io 线程执行（`runOnIoBlocking`），返回字符串给 `blocking_tool` |
| 主程序侧 | `SessionServerAgentIO` 对 `next-step`：轮次在跑 → 进 `Session::pendingInputs_`，`drainPendingSessionInputs` 在 modelcall 边界写进上下文；轮末残留由 `rolloverLeftoverNextStepInputs` 提升为下一轮 |

平台事实（QQ 官方文档「文本交互」）：

- `@用户` 嵌入 `<qqbot-at-user id="member_openid" />`，**群聊里只支持 markdown 消息**
  （`msg_type=2`）；频道（文字子频道）里文本 / markdown 都行，id 用 `user_openid`；
- `@全部成员` 是 `<qqbot-at-everyone />`（本任务不用）；
- 被动回复：带 `msg_id` + `msg_seq`，群 5 分钟 / 单聊 60 分钟，超窗或序号重复都会失败。

## 3. 方案

### 3.1 类型（`chatbot_types.h`）

```cpp
struct InboundEvent {
    ...
    /// 在出站消息里 @ 这个人时平台要的 id（QQ 群 = member_openid，单聊 = user_openid）
    std::string senderMentionId;
};

struct OutboundMessage {
    ...
    /// 主动消息里要 @ 的人（平台句柄；空 = 不 @）
    std::string mentionOpenId;
    /// 被 @ 的人的显示名（平台不支持真 @ 时写进正文）
    std::string mentionName;
};

struct SendResult {
    ...
    /// 平台判定「被动回复窗口已过期 / msg_id 无效」——重发主动消息能成功
    bool passiveExpired = false;
};

struct PlatformCaps {
    ...
    /// 能在出站正文里 @ 指定用户（QQ 群：markdown 消息里可以）
    bool mentionInContent = false;
};
```

`TurnMessage`（`chatbot_engine.h`）补 `senderMentionId` / `senderName`：工具据此知道
「这条消息是谁说的、要 @ 他得用什么 id」。

### 3.2 出站（`chatbot_outbound.{h,cpp}`）

- `OutboundRequest` 补 `mentionOpenId` / `mentionName`；
- `Job` 补 `std::shared_ptr<OutboundWait> wait`；
- `submit(..., std::shared_ptr<OutboundWait> wait = nullptr)`；
- `pump()` 结果回调：

```
result.ok                     → 记 sent / 平台 id；wait: resolveSuccess
被动失败 (passiveExpired 或    → overflowPolicy == active:
  reason 像窗口过期)              - 清 passiveReplyTo/passiveSeq
  && 有 passiveReplyTo            - 带上 mentionOpenId（平台不支持就清掉，
                                   留下 mentionName 供平台写成文本 @）
                                  - 同一条消息插回队首, 计入 passiveFallback
                                  - wait: noteWarning("…msg_id 已过期, 已改为 @… 发送")
                                truncate / fail: 沿用原语义
其它失败                      → 记 failed；wait: resolveFailure(reason)
```

- 新增计数 `OutboundCounters::passiveFallback`（全局 + 每会话），`chatbot_status` 的
  `outbound` 段与 `chatbot_chat_info` 的计数段带上它；
- 新增 `agentxx::chatbot::OutboundWait`：

```cpp
class OutboundWait {
    void resolveSuccess(std::string platformMessageId);
    void resolveFailure(std::string reason);
    void noteWarning(std::string warning);   // 只记提示, 不结束等待
    bool waitFor(int32_t timeoutMs);         // 工作线程上用 (mutex + cv)
    // 读取: ok / sent / warning / failure / platformMessageId
};
```

  语义：**第一个有结论的结果**结束等待（成功，或最终失败）；被动过期改 @ 那次只记
  `warning`，等重发的结果。这样工具调用通常只多一个 HTTP 往返的延迟。

### 3.3 平台（QQ / 假平台）

`QqPlatform`：

- `caps().mentionInContent = true`；
- `doSend`：`msg.mentionOpenId` 非空时
  - 群聊 → 强制 `msgType = 2`，正文前缀 `<qqbot-at-user id="<openid>" /> `；
  - 单聊 → 忽略（本来就是直接对他说），清掉 id；
  - 富媒体消息（`msg_type=7`）不加前缀（没有正文位）；
  - 拿不到 openid但有显示名 → 正文前缀 `@昵称 `；
- 失败分类：HTTP 4xx 且响应体里出现 `msg_id` / `msg_seq` / `过期` / `expired` /
  `无效` / `invalid` / `不存在` 之类关键词时置 `SendResult::passiveExpired = true`
  （出站层另有一份文本兜底判定，其它平台也能享受这套降级）。

`FakePlatform`：

- `caps().mentionInContent = true`；
- 新增 `setExpirePassive(bool)`：开着时，带 `passiveReplyTo` 的发送一律按
  「`msg_id` 已过期」失败（`passiveExpired = true`）；
- `debugOutbox` 输出 `mentionOpenId` / `mentionName`，测试据此核对。

### 3.4 调度层（`chatbot_engine.{h,cpp}`）

`assembleInput(chatKey, batch, reason, hasAttachments, delivery)`：

- `delivery = "next-step"` 且 `state.turn.active` → **中途插入**：
  - `[#n]` 从上一段最后一个编号往后排；
  - `turn.messages` 追加（不清空），`sentCount` / `modelSent` / `startedAtMs` 保持；
  - `hadMention` 取或，`lastPassiveMsgId` 更新到最新（被 @ 的优先）；
  - 输入块 `hintExtra` 加一句「这段是本轮中途插进来的新消息」；
- 否则沿用原来的「重置本轮」语义；
- `input["delivery"] = delivery`。

`flush(chatKey, reason)`：

- 通道正在跑一轮 → `delivery = "next-step"`，照常组装并投递（不再攒到 TURN_END）；
- 否则 `delivery = "next-turn"`（原行为）；
- 投递失败（`rc != 0`）且是中途插入 → 把这批消息**放回缓冲队首**、`stale = true`，
  等 `TURN_END` 再投（内容不丢）；空闲时的失败语义不变；
- 新计数 `ChatCounters::injected`（全局 + 每会话），`chatbot_status` / `chatbot_chat_info`
  与侧边栏面板不需要新增字段，只在 JSON 计数段里给出。

### 3.5 工具（`chatbot_tools.{h,cpp}`）

`toolSendMessage(deps, threadId, argsJson, std::shared_ptr<OutboundWait> wait = {})`：

- `replyTo = #n` → 用第 n 条的 `platformMessageId` 作被动 id，`senderMentionId` /
  `senderName` 作为 @ 目标；
- 没给 `replyTo` → 用 `turn.lastPassiveMsgId`，@ 目标取它在 `turn.messages` 里的那条；
- 新增 `mergeSendOutcome(std::string json, const OutboundWait* wait, int32_t timeoutMs)`：
  等工作结果，把 `sent` / `warning` 并进 JSON；最终失败时抛 `std::runtime_error`
  （SDK 边界会变成工具失败）。

### 3.6 插件入口（`agentxx_chatbot.cpp`）

`chatbot_send_message` 的工作体：先在插件 io 线程提交（拿回 `queued` 摘要），
再在工作线程上等 `OutboundWait`（`behavior.sendResultWaitMs`，默认 4000ms，0 = 不等），
把结果合并后返回。

### 3.7 配置（`chatbot_config.{h,cpp}`）

新增 `behavior.sendResultWaitMs`（默认 4000，0 = 不等发送结果），README 里说明。

## 4. 边界与兼容

- 被动过期降级只在 `overflowPolicy = active` 时改发主动消息；`truncate` / `fail` 的行为不变；
- 主动消息被用户关闭（平台报「主动消息被限制」）时仍是 `activeBlocked` 计数 + 失败上报，
  不会无限重试（一条消息只降级一次）；
- 富媒体消息（`msg_type=7`）不能带 @ 前缀：附件那条仍按原样发，文本段才有 @；
- `next-step` 带附件的输入在主程序侧会退化为「排队成新一轮」（`SessionServerAgentIO`
  的实现），插件不做特殊处理，内容不丢；
- 一轮中途插入不会重置 `maxRepliesPerTurn` 额度与 `sentCount`（同一轮还是同一份额度）。

## 5. 分步实施

1. 类型 + QQ 归一化/发送 + 假平台（能造出「过期」与「@」两种出站）；
2. 出站等待器与降级路径（含计数）；
3. 调度层 next-step 投递与中途记账；
4. 工具与插件入口接线（等待 + 合并返回）；
5. 单测（`chatbot_core` 出站、`chatbot_qq` 归一化）+ 端到端（`plugin_example_chatbot`）；
6. 文档（README §2.1 / §4 / §5）+ 本任务记录。

## 6. 验证

- `agentxx_test chatbot_core` / `chatbot_qq` / `plugin_example_chatbot`（debug 构建）；
- 端到端两条新用例：
  - 通道在跑一轮时注入新消息 → 该轮第二次 modelcall 的请求体里包含它；`chatbot_chat_info`
    的 `buffered` 不因它增长；计数 `injected` 增加；
  - 假平台开 `setExpirePassive(true)` → 一次 `chatbot_send_message` 产生两条出站
    （第一条带 `passiveTo`，第二条为空且带 `mentionOpenId`），模型下一次请求里能看到
    工具返回中的警告文本。

## 7. 风险

| 风险 | 处理 |
|---|---|
| 平台 @ 语法写错导致整条消息被拒 | 只对群聊文本消息拼 `<qqbot-at-user id="…" /> `，语法取自官方文档；拿不到 id 时退化成文本前缀 |
| 工具等发送结果拖慢调用 | 默认上限 4s，且「第一个有结论的结果」就返回；等不到就当 `queued`，不影响发送 |
| next-step 让长轮次被不断续命 | 消息仍按聚合窗口/被 @ 才投；额度仍按「一轮」算，模型自己决定何时收尾 |
| 中途插入的编号与上一段冲突 | 编号接着上一段排，`turn.messages` 追加而非重置 |
