# 插件协程体系收敛到 asio（并删除同步等待与同步工具体）

## 目标

插件侧的协程只有一套实现模型：`asio::awaitable` + kit 的锚定原语。删掉
`pluginxx::TableOpTicket` 这套自制协程记账，删掉 `tableOpSyncWait`（阻塞等表结果），
不再保留同步工具体。

## 范围

- `agent/third_party/cxx_pluginxx`（kit / runtime / host）
- `agent/lib/include/agentxx/plugin/api/*`（agent 侧与 client 侧 kit）
- `agent/plugins/*`（25 个插件）
- `agent/test/plugin/*`（用例与测试宿主）
- 文档：`AGENTS.md`、`docs/zh-cn/design/plugins.md`

## 状态

**第 1 步（工具体协程化）、第 2a/2b/2c 步（框架 asio 异步表调用 + chatbot 改 asio + 测试
探针去同步）、第 3 步（删同步等待与同步桥）、第 4 步（表调用与任务体统一到 asio）、
第 5 步（删掉自制协程记账 `Task` / `PromiseBase` / `BridgeRoot` / `TableOpTicket` 与
全部 ticket 形态入口）均已完成并全绿（2026-11-24）。** 具体进度、改动文件与验证数字见
`work.md`。

插件源码已经**不再使用** ticket 形态：25 个插件里 `toolAsync` 56 处、`tool(` 0 处，
其余表调用也全走 `*Async`（本轮把最后 7 处 `tool(` 也换成了 `co_await toolAsync`）。
因此第 5 步是**纯机制清理 + 用例整理**，不影响插件源码兼容性（实际做完确认：插件源码
一行未改，只改了 `example_plugin` 里的说明性注释）。

2026-11 上一次尝试直接删除 `tableOpSyncWait` 与同步桥时依赖链未拆完，已回滚；本轮按
“使用面先协程化 → 再删机制”的顺序重做，已不再有回滚点。

### 已完成的两步带来的结论

- 同步等待可以删掉的前提成立：插件内**业务层**（chatbot 的调度/任务/命令）已全部改成
  `asio::awaitable`，它的 11 处 `*Sync` 已清零；现在只剩 **kit 自己的封装**与**测试探针**
  还在用（`grep` 结果：plugins 目录 0 处）。
- 协程参数必须按值：一轮 ASan 报 `stack-buffer-overflow`，根因全是“协程帧只保存引用/视图，
  而发起方在协程恢复前就返回了”。

## 关键结论（本轮最重要的产出）

**"删除同步等待" 不是独立清理，它依赖 "插件里的非协程层先协程化"。** 具体链条：

1. `pluginxx::tableOpSyncWait` 的直接调用点只有 5 类：kit 的读取助手、agent 侧 kit 的
   `runtime*` 同步封装与 `addShareStore` 同步形态、client 侧 kit 的同步封装、
   `checkPathDecisions`、测试。
2. 这些封装的调用方**不在协程里**：
   - `agentxx_chatbot` 的平台回调层（20 处 `*Sync`：`mountRootSync` / `sendInputSync` /
     `submitAsyncWorkSync` …）必须同步回答平台的同步 C API，要改成协程就得先改
     平台层的调用形态（排队 + 异步回执）；
   - `agentxx_filesystem` 等 44 个工具体是**同步体**（跑在工作池线程上），
     `filterPathPermissions` / `checkPathDecisions` 在它们里面同步查询；
   - 测试里的探针（`start` / `mount` 回调）跑在插件执行线程上，靠同步桥的就地回调
     才能等到结果。
3. 所以要删掉同步等待，必须先让 1、2 里的调用点都进协程 —— 也就是必须先做
   "同步工具体 → 协程体"。两件事的先后顺序与最初设想相反。

## 下一步（顺序已按上述依赖调整）

1. ~~同步工具体 → 协程体（前置）~~ — 已完成（第 1 步）。
2. ~~插件里的非协程层协程化~~ — chatbot 已完成（第 2b 步）；其余插件暂无 `*Sync` 调用点。
3. ~~删除 `tableOpSyncWait` 与同步桥~~ — 已完成（第 3 步）：框架同步等待原语、
   `sync_bridge.h`、`OpCore` 就地回调、两个 kit 的 `*Sync`、`clientTableOp*` 全部删除；
   剩下的阻塞只在工作池/插件自有线程上（`blocking_tool` / `offload` / `netCall`）。
4. ~~表调用改成 asio 异步操作 + 任务体统一 `asio::awaitable`~~ — 已完成（第 4 步）：
   24 个插件（`agentxx_audio_stream` 本平台不编译）的任务体全部是 `asio::awaitable`，
   取消经 `asio::cancellation_signal` 传到表调用（`op_cancel`）；`spawn` 只收 asio 体。
5. ~~删除自制协程记账~~（第 5 步）— 已完成（2026-11-24）：`Task` / `PromiseBase` /
   `BridgeRoot` / `TableOpTicket` 与全部 ticket 形态 kit 入口删除；`kit.h` 6156 → 4227 行
   （-40 个整声明），`agent_plugin_kit.h` -1968 行，`plugin_kit.h` -203 行；用例只保留
   `test_plugin_bridge` 的受控轮询部分（§11~§14）。收尾见本文末节“结论”。

## 结论（2026-11-24）

### 最终的协程模型（只剩一套）

- **插件协程一律 `asio::awaitable`**：任务体、工具体、能力体、图节点体、后台任务体、
  注册事务（`finishLater(notify)` + `ctx.spawn(...)`），取消走 `asio::cancellation_signal`。
- **表调用两种形态**：`co_await *Async(...)`（`pluginxx::asyncTableOp` 包装）与
  `tableOpCallback(ctx, start, done)`（不在协程里时用）。文本参数一律按值。
- **锚定原语换名不换语义**：`sleepAsync` / `offloadAsync` / `onWorkerAsync` /
  `callToolAsync` / `callToolResultAsync` / `capabilityAsync` / `providePointAsync` /
  `graphNodeAsync`；推进仍由主程序的协程驱动桥（`PollOneBridge` + `PolledRoot`）负责，
  受控轮询规则（有进展立即续 / 无进展 10ms 退避 / 突发 256 步后让出 1ms）未变。
- **删掉的东西**：`Task` / `Task<void>` / `PromiseBase` / `BridgeRoot` / `spawnTaskImpl` /
  `resumePluginCoroutine` / `SleepTicket` / `OffloadTicket` / `TableOpTicket` /
  `InvokeCapAwaiter` / `yield` / `whenAll` / `tableOp` / `capability`（ticket 形态）与
  `PollOneBridge` 的根托管（`roots_` / `abandonedRoots_` 一族）。kit 里不再有第二套协程。

### 与计划的偏差

- `sessionMessagesCount` 只有 ticket 形态，没有 `*Async` 孪生 —— 直接改成
  `asio::awaitable<TableOpResult>`（而不是绕开）。
- `pluginxx::invoke_cap` 删除后**没有补 asio 孪生**：插件调别人的能力改为直接用
  `agentxx.capabilities` 表的 `invoke_capability`（包进自己的协程）。内置插件与 JS 引擎
  本来就这么调，所以没有新增 kit 入口。
- `test_plugin_bridge` 保留了受控轮询四段（§11~§14），其余 22 段连同它们测的记账一起删除；
  为了在假 host 上登记 awaitable 工具，用例里加了本地 `registerAwaitableTool`
  （不走公开的 `toolAsync`，后者会真的调表）。
- 顺带修了 `example_plugin.cpp` 与 `kit.h` 里描述旧模型的注释（`Task` 协程 /
  `polled_tool` / "driver 缺失时降级到 offload + 局部 io_context"——最后这条早已不存在）。

### 踩坑清单（给后来者）

1. 按锚点整段删除时，终点只认**列 0** 的 `}` / `};`；用 `Trim()` 判定会把嵌套 lambda 的
   `};` 当结尾（花括号计数 845/916，只能从备份重来）。
2. 删声明会留下孤立的 `template<...> requires(...)` 头与 `using`；用一个只 include 两个
   kit 头的小 TU（`cl /c /Zs`）比整棵树编译快得多。
3. 测试里的伪 scheduler 必须补 `sleep`：`asio::awaitable` 版 `sleepAsync` 全走
   `scheduler.sleep`（`plugin_sdk` 12 条用例报 `host scheduler timer unavailable`）。
4. 含大段中文的 `edit` 匹配会莫名失配；这类整段改写改用 PowerShell 按行号重写。
5. 一次性大改会陷入"越修越多的语法错"；按"删消费者（保留被删类型）→ 改用例 →
   删机制 → 收尾"四步走，每一步都停在可编译状态。

### 验证

- 顶层 Debug 构建 `EXIT=0`，25 个插件 DLL 全部重新链接。
- `boundaries` 20 / `plugin_bridge` 72 / `plugin_sdk` 148 / `plugin_runtime` 774 /
  `plugin_executor` 663 / `plugins` 706 / `client_plugins` 842 = **3225 条 failed=0**；
  `boundaries` 规则 16 的新禁名表（带 `pluginxx::` 前缀）没有误报。
- 全量 `agentxx_test --log-level warn`：**43682 passed / 0 failed**（比基线 43914 少的
  232 条是删除的用例：`plugin_bridge` 302 → 72、`plugin_executor` -2）。
