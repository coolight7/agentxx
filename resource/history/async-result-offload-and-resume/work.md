# 实施记录

## 进度

- [x] 工具输出压缩抽成共用实现
- [x] 前台工具节点 / 后台工具任务改用它
- [x] 事件中枢: `kind=tool` 结果不再二次 offload
- [x] 投递: 后台结果在空闲 + 暂停态也开新一轮
- [x] 轮末兜底: 后台结果来源的残留输入不置暂停
- [x] `agentxx_async_await` 打开 `autoSummaryOutput`
- [x] 结果正文只进会话一次（await 与自动通知去重，台账记投递权）
- [x] 测试（`event_hub` / `async_tools` / `event_delivery` 新增用例）
- [x] 文档（events.md §4/§5/§6/§8、config.h、hub.h、async_work.h、agent_host.h 注释）

## 改了什么（文件 → 内容）

- `agent/lib/include/agentxx/util/share_store_offload.h` / `src/util/share_store_offload.cpp`
  新增 `offloadToolResultIfOversized(ctx, toolExtra, sessionId, ToolResult&)`：
  `autoSummaryOutput == "true"` + 阈值判定 + 复用 `offloadShareStoreIfOversized`。
- `agent/lib/src/nodes/toolcall.cpp` `finalizeToolCall`：改为调用上面的函数
  （保留 `sessionId` 非空断言；删掉本地重复的阈值/开关判断）。
- `agent/lib/src/agent/async_work.cpp` `runToolWork`：
  - 在把 `target` 移交后台协程前拷贝 `tool->extra`；
  - 等待结束后把 `state->result` 拷一份、按同一份实现压缩，再填 `content`/`errorMessage`。
- `agent/lib/src/event/hub.cpp`
  - 新增 `isAsyncResultEvent()` / `asyncValuesAreToolKind()`（匿名命名空间）；
  - `contentText`：`agentxx.agent.async.*` + 全 `kind=tool` → 不整段卸载；
  - `deliver` 空闲分支：async 结果事件 `unpause = true`。
- `agent/lib/include/agentxx/agent/conversation_types.h`：
  `MessageQueueItem::resumeRequested`（服务端内部，不进 wire）。
- `agent/lib/include/agentxx/agent/io/session_server_agent_io.h` / `src/.../session_server_agent_io.cpp`：
  `pushMessageQueueItem` 多 `resumeIfPaused`（默认 false）；`submitTurnInput` 传
  `resumeIfPaused = unpausePaused`；`run()` 轮末按 `any_of(resumeRequested)` 决定不置暂停。
- `agent/lib/src/agent/base_agent.cpp` `rolloverLeftoverNextStepInputs`：
  遍历来源，命中 `event:agentxx.agent.async.*` 时以 `unpausePaused=true` 提交。
- `agent/lib/include/agentxx/event/naming.h`：新增 `kAgentAsyncDoneEvent`
  （`async_work.cpp` / `async_tools.cpp` 的事件名字面量与测试统一用它）。
- `agent/lib/src/tools/async_tools.cpp`：`AsyncAwaitTool` 打开 `autoSummaryOutput`。
- 测试：
  - `agent/test/core/test_event_hub.cpp`：`test_hub_async_tool_result_not_offloaded`、
    `test_hub_async_result_unpauses_idle_session`；
  - `agent/test/core/test_async_tools.cpp`：`AtBigOutputTool` + `test_tool_result_offloaded_once`；
  - `agent/test/core/test_event_delivery.cpp`：`makeDeliveryFixture` 增 `llmMaxRetry` 参数、
    `test_delivery_async_result_resumes_paused_queue`、
    `test_delivery_rollover_async_result_resumes`。
- 文档：`docs/zh-cn/design/events.md`（§4 暂停态例外、§5 不卸载的一段、§6 "结果只进会话一次"、
  §8 结果回到会话）、`agent/lib/include/agentxx/agent/config.h`（两个阈值的说明）、
  `agent/lib/include/agentxx/event/hub.h`（`contentText` / `contentLimitLength` /
  `unpausePaused` / `DeliverResult::Suppressed` 注释）。

### 第二轮：结果正文只进会话一次（await vs 自动通知）

- `agent/lib/include/agentxx/agent/async_work.h` / `src/agent/async_work.cpp`：
  `AsyncWorkEntry::resultTaken`(布尔) + `AsyncWorkManager::claimResult` / `releaseResult`；
  `entryJson` 输出 `resultTaken`。
- `agent/lib/include/agentxx/agent/agent_host.h` / `src/agent/agent_host.cpp`：
  `claimAsyncResult` / `releaseAsyncResult` 转发。
- `agent/lib/include/agentxx/event/naming.h`：新增 `kAgentAsyncNotifyOwner`（框架自动通知
  等待的归属标记，`async_work.cpp` 的 `kAsyncOwner` 改为引用它）。
- `agent/lib/include/agentxx/event/hub.h`：`EventDelivery::owner`、
  `DeliverResult::Suppressed`、`EventHubStats::suppressed`。
- `agent/lib/src/event/hub.cpp`：`flushWait` 带上 owner；`deliver` 投递前先领投递权
  （领不到 → `Suppressed` + info 日志 + 计数），没进会话（丢弃/被拒）时归还；
  `statsJson` 输出 `suppressed`。
- `agent/lib/src/tools/async_tools.cpp`：`AsyncResultClaimGuard`、
  `renderEntryDetail(entry, withResult)`、`renderResultAlreadyDelivered`；await 在拿正文前
  领一次投递权（终态与等待共用一处），超时/取消归还。
- `agent/lib/src/agent/prompt.cpp`：`agentxx_async_await` 的 depict 增加"结果只进会话一次 /
  已投递时给回执"一条。
- 测试 `test_async_tools.cpp`：`countEventInputs` 辅助 + `test_await_single_delivery` +
  `test_await_after_notify_returns_receipt`。
- 文档：`docs/zh-cn/design/events.md` §6（"结果只进会话一次"）、§8（结果回到会话）、
  §12 测试模块表。

### 第三轮：简化机制 + 术语换名

- **简化**（用户反馈"有点复杂了"）: 把"谁领了正文"从字符串（`resultConsumer`，取值
  `await` / `async_work`）改成任务表条目上的一个布尔标记 `AsyncWorkEntry::resultTaken`：
  - `AsyncWorkManager::claimResult(asyncId)` / `releaseResult(asyncId)`（不再传消费者）；
    `AgentHost::claimAsyncResult` / `releaseAsyncResult` 转发；
  - `entryJson` 的 `resultClaimedBy` → `resultTaken`（仅在 true 时输出）；
  - `EventHub::deliver` 改成**投递前先领**（领不到 → `Suppressed`），没进会话（丢弃/被拒）
    才归还 —— 去掉了原实现"先查再补领"的双次查询与字符串比较；
  - await 不再分两处领（终态分支 + 等待分支），统一在拿正文前领一次；回执文案
    （`renderResultAlreadyDelivered`）不再区分是谁先给的，改成一句"已由自动通知或更早的一次
    `agentxx_async_await` 交付"；删除 `kAwaitConsumer`。
- **术语**：`台账` → 任务表/条目（异步任务这一套）；同批把仓库里其余 `台账` 按语境统一为
  用量记录（`usage` 表与管理用量）、任务记录（chatbot 插件自己的业务记录）、成员顺序清单
  （`boundaries` 的接口表成员顺序）。涉及 `agent/lib` `agent/client` `agent/test`
  `agent/plugins` `docs` `AGENTS.md`；`resource/history/` 其他历史记录不改。
- 模型可见文案里的 `task ledger` 一并改成 `task table`（`prompt.cpp`、`async_tools.cpp`、
  `subagent.h`），与中文术语一致。

## 验证

- 构建：`agent/script/windows_debug_build.bat`（Debug，含 client/test）通过。
- 测试（`--log-level error`）：
  - `event_hub` 288 / `event_tools` 97 / `event_delivery` 62 / `async_tools` 181（首轮 159 +
    第二轮新增 22）/ `async_work` 138 / `share_store` 63 —— 全绿；
  - `input_delivery` 139 / `session_admin` 38 / `session_sync` 30 / `interrupt_ui` 338 /
    `event_subagent` 52 / `message_supplement` 95 / `view_replica` 114 / `misc_fixes` 114 /
    `assembly_snapshot` 38 / `plugin_runtime` 726 / `prompt_stability` 19 —— 全绿。
- 第三轮重验（简化布尔标记 + 术语换名后，构建 exit=0）：
  `async_tools` 181 / `event_hub` 288 / `event_tools` 97 / `event_delivery` 62 /
  `async_work` 138 / `share_store` 63 / `input_delivery` 139 / `boundaries` 19 /
  `chatbot_task` 138 = 1125 全绿；`config_keys` / `config_loader` / `event_subagent` /
  `message_supplement` / `view_replica` / `session_admin` / `session_sync` = 990 全绿。

## 遇到的问题 / 注意

- `session_persistence` 有 1 个与本任务无关的既有失败（`test_session_persistence.cpp:1341`
  `utf8IsAvail(loaded.viewMessages[1].text)`，用例 `testPersistenceResilience` 的非法 UTF-8
  修复断言）。该模块只测 `SessionStore`，本任务改动的文件都不在该路径上；工作区里
  `agent/third_party/cxx_utilxx*` 本来就有未提交改动，疑似来自那边的 `utf8Repair`/读取侧行为。
- 写第一个轮末兜底用例时踩到时间窗口问题：模拟 LLM 的**失败**响应不带 `delay_ms`
  直接返回，`waitFor(request_count)` 之后残留输入还没来得及入队本轮就结束了 ——
  改成"慢成功响应 + `WireCancel` 取消本轮"来制造暂停态，见
  `test_delivery_rollover_async_result_resumes`。
- 只传 `unpausePaused` 不够：轮末兜底发生在"按结果置暂停"之前，那一刻队列尚未暂停，
  所以必须把意向记在条目上（`resumeRequested`），由轮末统一判断。
- Windows 上构建脚本末尾 `copy exec` 失败属正常（agentxx 进程占用），不影响产物。
- 批量改文案时踩到的坑（已修复）: PowerShell 里 `@(@('旧','新'))` 会把单元素嵌套数组摊平成
  两项，循环里 `$pair[0]`/`$pair[1]` 取到的是**单个字符**，于是变成逐字替换
  （`业`→`务`、`主`→`程`、`台`→`账` 之类），13 个文件被改坏。修法判断条件: 先从 HEAD 回滚，
  用"文件|旧串|新串"管道分隔的字符串做规则（每条按索引取字段），只对回滚后的文件重放。
  教训: 批量替换前后都要比一次 `git diff --numstat` 的改动行数是否与预期一致。
- 已知取舍: 若 await 正好在"自动通知已投递完成（被 `Suppressed`）"之后超时，本次等待会拿
  超时文本并归还标记，正文只能靠 `agentxx_async_query` 取；窗口极小，回执里已给取回指引。
