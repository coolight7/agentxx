# 异步结果: 只裁剪一次 + 一定进上下文

## 目标

1. 后台**工具**任务 (`agentxx_async_run` 的 `kind=tool`) 的结果不再在事件投递层
   二次 offload —— 它的正文在工具执行层就按工具自己的 `autoSummaryOutput` 声明
   裁剪过; 其余事件结果（子代理回答、插件/模型发布的值）保持原有的长度限制转
   share store。
2. 查清并修掉"异步结果必须等会话在跑、或用户再发一条消息才进上下文"的现象。
3. 同一条结果只进会话一次：`agentxx_async_await` 拿到结果时，`notify` 的自动通知
   不再把同一段正文插一份（反之亦然）。

## 范围

- `agent/lib`: 工具输出压缩抽成唯一实现（前台 `ToolcallWrapNode::finalizeToolCall`
  与后台 `runToolWork` 共用）、事件中枢的内容段与投递、轮末兜底、会话端点队列
- 测试: `event_hub` / `event_delivery` / `async_tools`
- 文档: `docs/zh-cn/design/events.md` §4/§5, `AgentConfig` 与中枢配置头注释

## 状态

已完成（实现 + 测试 + 文档）。未改动对外 wire 协议、插件接口表、配置键。

收尾又做了两件事：把"谁给正文"的记账从字符串消费者简化为任务条目上的一个布尔标记
（`resultTaken`），并把仓库里的 `台账` 按语境换成任务表 / 用量记录 / 任务记录 /
成员顺序清单（模型可见英文文案 `task ledger` → `task table`）。

## 关键决策

- **裁剪只在一处**：新增 `agentxx::util::offloadToolResultIfOversized`，是否压缩由
  工具自己的 `autoSummaryOutput` 决定，与前台/后台执行路径无关；
- **事件层例外只认 `kind=tool`**：值里没有该字段、混合来源、`kind=agent` 仍按长度卸载；
- **`agentxx_async_await` 声明 `autoSummaryOutput`**：它的返回值与自动投递同一份渲染，
  失去事件层兜底后由工具层按同一阈值压缩（子代理回答行为不变）；
- **暂停态对后台结果不生效**：投递时（会话空闲）主动解除暂停；轮内提交的条目带
  `MessageQueueItem::resumeRequested`，轮末因此不置暂停。

## 结论

> 注：本轮的“投递权”仲裁（`resultTaken` + claim/release + `Suppressed`）后来被
> `resource/history/async-result-delivery/` 换掉了 —— 现在改成“票据 + 任务收尾直接交付”，
> await 是读任务表不再与通知抢正文。本文件保留当时的判断与验证记录。

现象根因是会话端点的 `Paused`（上一轮失败/取消留下）只由用户输入解除，而事件输入
按"积压消息不自动继续"的默认不解除暂停；结果有两条落点（空闲投递、轮末兜底提升），
两条都要带"必须进上下文"的意向。确定性复现：取消一轮后投递 `agentxx.agent.async.done`
的值，修复前不会开新一轮。

重复正文的根因是"一条结果两个出口"：`notify` 在提交时就登记自动通知等待，
`agentxx_async_await` 又直接读任务表给结果，两边都会把正文交给模型。修法是在任务表条
上记一个"正文已经放进会话"的布尔标记（`resultTaken`，`AgentHost::claimAsyncResult`）：
谁先要放正文谁领，领不到的那个只给回执；await 超时/取消时归还，自动通知领不到就不
投递（`DeliverResult::Suppressed`）。
