# 实施记录

## 进度

- [x] 核心：`delay` 字段 + pump 扫描/跳延时 + 唤醒定时器 + `startAsyncWorkNow`
- [x] 内置工具索引（`context.h` / `base_agent.cpp`）
- [x] 工具面：拦截规则、`delayMs`、`start_now`、文案
- [x] 提示词（`prompt.cpp`）
- [x] 界面续推过滤（`hasActiveAsyncWorks`）
- [x] 测试（`async_work` / `async_tools` 新增用例）
- [x] 文档（events.md §8 / AGENTS.md / tui.md 停用词）

## 每步改动与验证

### 1. 核心：延时与提前开始

改 `agent/lib/include/agentxx/agent/async_work.h` 与 `src/agent/async_work.cpp`：

- `AsyncWorkSpec::delay`（毫秒，<=0 = 不延时）；`AsyncWorkEntry::delayMs`（原值）与
  `delayUntilSteadyMs`（到期时刻，steady 时钟）；`AsyncWorkSubmitResult::delayMs`。
- `submit`：算出到期时刻，状态保持 `Queued`；`Queued` 回调 payload 带 `delayMs`。
- `pump` 重写：一次把名额允许的条目都拉起；**延时未到的条目留在队列里被跳过**
  （不占名额、不挡后面的任务）；起跑时把 `delayUntilSteadyMs` 清零。
- 新增 `pumpAllQueues` / `armDelayTimer` / `onDelayTimerFired` / `queueKeysSnapshot`：
  管理器一条共享 `asio::steady_timer`，取所有排队条目里最早的到期时刻；句柄只持
  `weak_ptr<AgentHost>`（避免 host → manager → timer → handler → host 成环）。
  定时器只在容器 io 线程访问（pump 的调用点都是 post 过来的）。
- 新增 `startNow(asyncId)` + `AgentHost::startAsyncWorkNow`：清剩余延时并 pump 一次。
- `entryJson` / `listJson`：排队项追加 `delayMs` / `delayRemainingMs`。
- `cancel` 排队项时把 `delayUntilSteadyMs` 清零。

验证：`agentxx_test async_work`（新增 2b 用例）全绿。

### 2. 内置工具可后台跑

- `AgentContext` 追加 `builtinToolIndex`（名称 → `neograph::Tool*`，只借不持有）与
  `engine`（图引擎弱引用）；`context.h` 只做前向声明，重头文件留在 cpp。
- `BaseAgent::init` 的 `bind_tools` 步骤在 `engine->own_tools(...)` 前填索引（unique_ptr
  转移不挪对象地址），并写 `agentContext->engine`；`engine_build` 步骤回滚时一并清空。
- `async_work.cpp` 新增 `ToolTarget` 与 `resolveCreatorTool()`：先锁引擎查内置索引
  （任务期间引擎被锁住 = 工具寿命有保证），再查 `toolRegistry`；`XXToolBase` 走 Json
  主接口，原生 `neograph::Tool` 走图桥接（与 `ToolcallWrapNode::runToolCallBody` 一致）。
  两处都查不到时运行期按失败收尾，文案保留 "not found"（旧测试依赖该词）。

验证：`agentxx_test async_tools` 的 `test_start_builtin_and_async_tools`（`agentxx_share_store`
与 `agentxx_async_query` 都在后台跑成功）、`async_work` 的 `test_tool_kind_*` 全绿。

### 3. 工具面

- `AsyncStartTool`：拦截名单只剩 `agentxx_async_start` 自己；其余工具要求"内置索引或
  动态注册表里有"，都没有才报 `not available`；新增 `delayMs`（也接受 `delay_ms`），
  受理文案带 `Starts in ...` 与提前开始提示。
- `AsyncAwaitTool`：新增 `start_now`；剩余延时 > `timeout_ms` 且没给 `start_now` 时
  直接回说明文本（不挂到必然超时）。
- `renderEntryDetail` / `renderTaskList`：排队项补 `ahead=` / `delay=... left`。

验证：`agentxx_test async_tools` 127 passed / 0 failed。

### 4. 界面续推

`SessionServerAgentIO::hasActiveAsyncWorks`：只有 `running` 或"没有剩余延时的 `queued`"
才算活动，延时中的排队项不再触发每秒续推（到期由管理器通知一次）。

验证：`agentxx_test tui_widget`（Info 侧边栏后台任务段）208 passed / 0 failed。

### 5. 文档

- `docs/zh-cn/design/events.md` §8：拦截规则、起跑延时、`kind=tool` 的来源与引擎弱引用、
  续推过滤。
- `AGENTS.md`：模型侧工具族参数、后台任务段。
- `docs/zh-cn/design/tui.md`：把 "通知链路" 改成 "通知流程"（`boundaries` 规则 15 的停用词，
  该违规是工作区原有内容，顺手修掉让规则恢复绿色）。

## 验证清单

| 模块 | 结果 |
|---|---|
| `async_work` | 138 passed / 0 failed |
| `async_tools` | 127 passed / 0 failed |
| `event_tools` / `event_delivery` / `event_subagent` / `plugin_event` | 321 passed / 0 failed |
| `plugin_sdk` / `plugin_agent_runtime` / `boundaries` | 373 passed / 0 failed |
| `boundaries` / `tui_widget` / `chatbot_task` | 365 passed / 0 failed |

新增用例：

- `async_tools::test_start_builtin_and_async_tools`：内置工具（`agentxx_share_store`）与
  后台任务管理工具（`agentxx_async_query`）都能后台跑，结果进台账。
- `async_tools::test_delay`：延时项在延时内保持 `queued`、工具没有被调用、台账带
  `delayMs` 与 `delayRemainingMs`、查询文本带 `delay=`；同队列的普通任务先跑完；
  到期后自己起跑并 `done`。
- `async_tools::test_await_start_now`：剩余延时远大于 `timeout_ms` 时直接回说明
  （< 2 秒返回、不是超时文本）；`start_now:true` 抹掉延时立即跑完拿结果。
- `async_work::test_delay_and_start_now`：`delayMs` 期间 `queuedCount()==1` 且不挡
  队列里后面的任务；`startAsyncWorkNow` 起跑；终态后再调返回 `false`。
- `async_tools::test_start_errors` 调整：`agentxx_async_start` 仍被拒（文案 "itself"）、
  不存在的工具名被拒（"not available"）、`agentxx_share_store` 不再被拒。

## 遇到的问题

- 只编内层 lib 目录后测试二进制还是旧的：`agentxx_test` 链的是安装前缀里的
  `libagentxx_staticd.lib`，改完库必须 `cmake --install <lib-build> --config Debug`
  再编测试（否则现象是"改了文案但断言还是旧结果"）。
- `boundaries` 规则 15 命中 `docs/zh-cn/design/tui.md` 的 "通知链路"（该行属上一提交，
  与本次改动无关）；改成 "通知流程" 后恢复绿色。

## 可选限制（未实现，等用户决定）

- `delayMs` 上限（如 24h）：现在不设上限，模型给多少就排多久。
- 插件侧 `submit_async_work` 的 `delay_ms`：要动接口表（表尾追加 + `specHas`）与
  `plugin_runtime` 的布局断言。
- 修改排队中任务的延时（现在只能"抹掉全部"或取消）。
- 批量等待（等一组 / 任一），以及"等延时到期"而不起跑。
