# 实施记录

## 进度

- [x] 类型 + QQ 归一化/发送 + 假平台
- [x] 出站降级与等待器
- [x] 调度层 next-step
- [x] 工具与插件入口接线
- [x] 测试
- [x] 文档

## 1. 类型与平台层

改了什么：

- `agent/plugins/agentxx_chatbot/chatbot_types.h`
  - `InboundEvent::senderMentionId`（出站 @ 这个人要用的 id）
  - `OutboundMessage::mentionOpenId` / `mentionName`
  - `SendResult::passiveExpired`
  - `PlatformCaps::mentionInContent`
- `platform/qq/qq_normalize.cpp`：按 `member_openid` → `user_openid` → `senderId` 填
  `senderMentionId`（`senderId` 优先取 `author.id`，与 @ 需要的 id 不一定相同）
- `platform/qq/qq_platform.cpp`
  - `caps().mentionInContent = true`
  - `doSend`：`mentionOpenId` 非空时群消息强制 `msg_type=2` 并加
    `<qqbot-at-user id="…" /> ` 前缀；单聊忽略；`msg_type=7` 不加（没有正文位）；
    拿不到 openid 但有名字 → 正文前缀 `@昵称 `
  - 失败时按 4xx + 关键词（`msg_id` / `msg_seq` / 过期 / invalid / 不存在…）置
    `passiveExpired`
- `platform/fake/fake_platform.{h,cpp}`：`setExpirePassive(bool)`、`mentionInContent = true`、
  `debugOutbox` 输出 `mentionOpenId` / `mentionName`

验证：插件单独编译通过（`build_plugins.ps1 agentxx_chatbot`）。

## 2. 出站降级与等待器

- `chatbot_outbound.h`：新增 `OutboundRequest::mentionOpenId/mentionName`、
  `OutboundCounters::passiveFallback`、`OutboundWait`（mutex + cv，第一个有结论的结果结束等待）、
  `submit(..., wait)`、`Job::wait`
- `chatbot_outbound.cpp`
  - `OutboundWait` 实现（`resolveSuccess` / `resolveSkipped` / `resolveFailure` /
    `noteWarning` / `waitFor`）
  - `pump()` 结果回调：被动失败（平台标记或文本兜底判定）→ 按 `overflowPolicy`
    改发主动消息（清 passive、带 @、插回队首、计 `passiveFallback`）；
    成功/失败都通知等待器
  - 被动回复本身**不带** @（否则每条回复都会变成 @ + markdown）：@ 目标只在重发时才清掉/保留，
    也就是先在 `send_` 之前把 `mentionOpenId`/`mentionName` 从这条被动消息上抹掉，
    重发那份（`retryMessage` 拷贝）仍留着
  - `submit()` 一条都没入队时给等待器一个结论（避免工具白等）

## 3. 调度层 next-step

- `chatbot_engine.h`：`TurnMessage::senderMentionId/senderName`、
  `ChatCounters::injected`、`assembleInput(..., delivery)`、`mentionTargetOf(...)`
- `chatbot_engine.cpp`
  - `flush()`：通道在跑一轮 → `delivery = "next-step"`（不再攒到 TURN_END）；投递失败时
    把这批消息放回缓冲队首（内容不丢）
  - `assembleInput()`：`midTurn` = next-step 且本轮在跑 → 编号接着上一段、
    `turn.messages` 追加、`sentCount`/`modelSent`/`startedAtMs` 不动、`lastPassiveMsgId` 更新；
    输入块加一句「中途插进来的新消息」
  - `mentionTargetOf()`：按 msgId 找回说话人的 @ 目标
  - `statusJson()` / `chatInfoJson()`：`outbound.passiveFallback`、`counters.injected`
  - 命令触发的一轮：`TurnMessage` 也补 @ 目标（结构体加字段后原聚合初始化会错位）

## 4. 工具与插件入口

- `chatbot_tools.{h,cpp}`
  - `toolSendMessage(..., wait)`：按被动 id 找回 @ 目标；提交时把等待器交给出站层
  - `mergeSendOutcome(json, wait, timeoutMs)`：等到结果 → 并 `sent`/`messageId`/`warning`；
    最终失败抛 `runtime_error`（工具失败）；超时保留「已排队」
  - `toolDebugPlatform(expire_passive)`：假平台开关（只给测试/演示）
  - `toolDebugInject` 支持 `senderMentionId`
- `agentxx_chatbot.cpp`：`chatbot_send_message` 工作体「io 线程提交 → 工作线程等结果」；
  注册 `chatbot_debug_platform`（仅 `debugTools`）
- `chatbot_config.{h,cpp}`：`behavior.sendResultWaitMs`（默认 4000，0 = 不等）

## 5. 测试

新增/改动：

- `agent/test/core/test_chatbot_core.cpp`：`testOutboundPassiveFallback`
  （群聊降级 + @、单聊不带 @、非过期失败、`truncate` 策略）
- `agent/test/core/test_chatbot_qq.cpp`：`senderMentionId`（member_openid / user_openid /
  退回 senderId）
- `agent/test/plugin/test_plugin_example_chatbot.cpp`
  - 12b：一轮在跑时注入新消息 → 这一轮下一次 modelcall 的请求里就有它，
    `buffered = 0`、`counters.injected ≥ 1`
  - 12c：假平台 `expire_passive` → 平台只收到「无被动 id + 带 @」的那条，
    工具返回里带 `msg_id 已过期` / `已改为主动消息重发` 警告，`outbound.passiveFallback ≥ 1`
  - 新增按 `sessionId` 找通道 / 等通道忙闲 / 等 `injected` 计数的辅助协程
    （`channelOfPrefix` 在同一个人设下有多个通道后会取错，第一次跑就是栽在这）
- `agent/test/CMakeLists.txt`：把 `chatbot_outbound.cpp` 编进测试（出站现在有单元测试）

运行结果（debug / ASan 构建）：

```
agentxx_test --log-level warn chatbot_core chatbot_qq chatbot_media chatbot_media_io chatbot_task
  chatbot_core     passed=872  failed=0
  chatbot_qq       passed=136  failed=0
  chatbot_media    passed=69   failed=0
  chatbot_media_io passed=54   failed=0
  chatbot_task     passed=138  failed=0
agentxx_test --log-level warn plugin_example_chatbot
  passed=265 failed=0 (约 3 分钟)
```

## 6. 文档

- `agent/plugins/agentxx_chatbot/README.md`
  - §2.1 新增「一轮还在跑时来的新消息：插进这一轮（`next-step`）」
  - §3 输入块说明补「中途插进来的那段」
  - §4 工具表：`chatbot_send_message` 的返回、`chatbot_debug_platform`、计数项
  - §4.2 新增「发送结果与『被动回复过期』的降级」（含平台 @ 语法与返回示例）
  - §5 FAQ：被动窗口过期改 @ 那条日志怎么读、怎么少踩
  - 配置样例：`sendResultWaitMs`、`overflowPolicy` 注释
- `docs/zh-cn/design/plugins.md` §13.1
  - 改写「一轮里多条消息一起带上去」：通道忙时按 `next-step` 插进本轮（含编号/额度延续、
    `counters.injected`）
  - 新增「发送结果会回到工具」一段：等待上限、被动窗口过期改 @ 主动消息、`passiveFallback`、
    markdown 只在群里才能 @
  - 调试工具两个 → 三个（补 `chatbot_debug_platform`），插件表补 `_task`

## 7. 遗留与注意

- QQ 群的 @ 只在 markdown 消息里生效，所以降级那条消息会变成 markdown（正文按 markdown
  渲染）；`msg_type=7`（富媒体）那条不加 @，附件消息保持原样；
- 工具等结果的上限默认 4 s：平台慢/队列长时会退回「已排队」的说法（不阻塞工具）；
- 被动过期的文本兜底判定（`msg_id` / `过期` / `invalid` 等关键词）对其它平台也生效，
  判错只可能多发一条主动消息（被动回复失败本身就说明那条没发出去）；
- 端到端用例的模型延迟是共享的（`daSimSetDelayMs`），用例结束已复位为 0。
