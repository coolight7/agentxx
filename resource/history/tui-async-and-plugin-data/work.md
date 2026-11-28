# 实施记录: TUI 分页滚动屏蔽 / async 工具行 / 插件数据目录

- 任务卡片: [index.md](index.md)

## 进度

| 项 | 状态 |
|---|---|
| 1 TUI 分页时屏蔽滚动 | 已完成 (测试 `tui_scroll` 1039 通过) |
| 2 async 工具行内容 | 已完成 (测试 `tui_tool_header` 179 通过) |
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

## 2 async 系列工具的工具行内容

### 问题

`agentxx_async_*` 是 lib 内置工具, 但 `registerBuiltinToolRows` 只给
`agentxx_share_store` / `agentxx_subagent` 注册了显示名与折叠内容。没内容的工具行
在运行中只能画默认文案 (`正在调用工具...`), 而后台任务的参数 (哪个工具、哪个任务 id、
取消哪几条) 恰恰是用户最想看到的信息。

### 做法

`agent/lib/src/plugins/builtin_tool_renderers.cpp` 新增五个内置折叠内容 (与 share_store /
subagent 同一个注册表、同一条渲染路径; 插件内容仍可覆盖内置):

| 工具 | 显示名 | 折叠内容 |
|---|---|---|
| `agentxx_async_run` | `AsyncRun` | `<目标工具>` / `subagent <名字>`, 有 `delayMs` 补 `+2s`, 完成后用结果里的 `asyncId` 补 `(async-3)` |
| `agentxx_async_query` | `AsyncQuery` | `id=async-3` 或 `group=running` (缺省 `all`) |
| `agentxx_async_await` | `AsyncAwait` | `async-3 (30s)` (只有显式给 `timeoutMs` 时才画时长) |
| `agentxx_async_manager` | `AsyncManager` | `cancel async-3, async-5` / `delay async-3 +5s` (id 超过 3 个补 `, ...`) |
| `agentxx_async_emit` | `AsyncEmit` | `<事件名>` / `<事件名> (end)` |

- 参数还没到、解析失败、必要字段缺失 → 返回空串, 界面照旧走默认渲染
  (参数流式接收期间仍是占位行, 这是真实状态)
- 结果里的任务 id 只在 `finished` 时取 (受理成功才有); 失败行本来就走错误回退, 不冲突

### 验证

- `test_tui_tool_header` 新增后台任务族断言 (运行中/受理完成/子代理+延时/查询两种形态/
  等待/取消+改延时/发布+end), 含一条“运行中不再出现 `receiving tool call`”的回归断言;
  `tui_tool_header` passed=179 failed=0
- 测试注册表 (`makeTestToolRegistry`) 同步补上五条内置行内容
