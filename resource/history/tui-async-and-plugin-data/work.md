# 实施记录: TUI 分页滚动屏蔽 / async 工具行 / 插件数据目录

- 任务卡片: [index.md](index.md)

## 进度

| 项 | 状态 |
|---|---|
| 1 TUI 分页时屏蔽滚动 | 已完成 (测试 `tui_scroll` 1039 通过) |
| 2 async 工具行内容 | 待开始 |
| 3 插件数据目录规范 + 迁移 | 待开始 |

## 1 TUI 分页时屏蔽滚动

### 问题与做法

页请求在飞期间用户可以继续滚轮; 这期间攒下的行数都是在"前插区高度尚未补齐"
(新增条目的高度还是粗略估算、锚点以上高度和还没算上它们) 的状态下攒的。
页响应一到, 前插 + 锚点平移落实, 这些行数再落实一次就跨过刚加载出来的整段,
表现是"加载出来前面的消息后立刻跳到新加载消息的开头"。

做法: 请求在飞期间屏蔽滚轮, 页响应处理完再放开。

- `LazyScrollable::setScrollSuspended(bool)` / `scrollSuspended()`
    - 暂停期间滚轮事件被吞掉 (`OnEvent` 返回 true, 不累积 `pendingScrollRows_`)
    - 进入/退出暂停都把 `pendingScrollRows_` 清零 (暂停前攒的行数按旧内容算,
      恢复后内容已变)
    - 只影响滚轮: `scrollToIndex` / `scrollToBottom` / 吸附底部不受影响
- `MessageListComponent::setScrollSuspended` / `isScrollSuspended` 转发;
  `maybeRequestMoreHistory` 在暂停期间直接返回 (不重复发起分页)
- `TUIClientAgentIO`:
    - `requestOlderHistory` 受理成功后 `suspendHistoryScroll()` (经 UI 动作队列下发)
    - `onViewMessagesPage` 所有出口 (接受/空页/迟到/不连续) 都 `resumeHistoryScroll()`;
      接受路径里恢复动作排在前插动作之后 (队列 FIFO), 同一帧渲染时两者都已落实
    - `onSync` 整体替换、`switchToSession` 也会放开 (并作废看门狗)
    - 看门狗 `kHistoryPageTimeout` (8s): 页响应丢失 (断线等) 时放开滚动并
      `HistoryWindow::abortLoading`, 免得列表一直不响应滚轮; 回调只持弱引用 +
      代次 (`historyPageSeq_`), 旧定时器不会动新请求
- 新增用例 `testScrollSuspensionDuringHistoryLoad` (`tui_scroll`):
  ① 屏遮期间滚轮被吞、视口逐行不变、恢复后可继续滚;
  ② 屏遮期间不发起分页预取, 恢复后照常预取;
  ③ 屏遮期间进来一页 (前插 3 条) 后视口内容不变、偏移等量下移, 恢复后上滑一行的
  位移不超过 2 行 (估算校正允许 1 行偏差)

### 验证

- `agentxx` (client, Debug) 编译通过
- `agentxx_test tui_scroll --log-level warn`: passed=1039 failed=0
