# 实施计划：插件协程体系收敛（同步等待 / 同步工具体 / asio 统一）

## 1. 背景

插件侧现在有三条并行轨道：

- kit 自制的 `Task<T>` + `TableOpTicket` / `SleepTicket` / `OffloadTicket` 一类 awaiter，
  依赖 kit 自有 promise（`await_suspend` 里直接调 `h.promise().clear_outstanding()`）；
- `asio::awaitable`（受控轮询工具体、部分插件自带的 io 代码）；
- 同步工具体（`ToolBodyKind::Sync`，跑在主程序工作池线程上）与 `tableOpSyncWait`
  这条阻塞路径（经主程序"同步桥"就地在完成线程上唤醒）。

三条轨道混用，改动一件表调用就要在三处同时改，问题会越来越多。

## 2. 现状调查（文件 / 函数一级）

### 2.1 kit 的协程记账（`cxx_pluginxx/include/pluginxx/kit/kit.h`，5535 行）

- `PollOneBridge`（928）：私有 `asio::io_context localIo_`（1 线程提示）+ work guard，
  `local_executor()`（985）对外。主程序每次驱动票只执行一次 `poll_one()`。
- `BridgeRoot`（711）/ `PolledRoot`（850）：根收尾与"被放弃的帧"保活
  （`abandonedRoots_`、`shouldAdvance`、`finishIfDone`）。
- `PromiseBase<T>`（2877）/ `Task<T>`（2963）/ `Task<void>`（3053）：自制 promise，
  `bridgeRoot_`、`outstandingCancel_`、`opCleanup_`。
- `resumePluginCoroutine`（2505）、`startBridgedRoot`（2543）：恢复入口与根启动。
- 原语：`YieldAwaiter`（3584）、`SleepTicket`（3644）、`OffloadTicket`（3868）、
  `InvokeCapAwaiter`（4097）、`TableOpState`（4280）、`TableOpTicket`（4317）。
- `submitTableOp`（4423）、`tableOp`（4586）、`whenAll`、`capability`（4964）、
  `tableOpSyncWait`（4755 / ctx 形态 4843）、`detail::tableOpText` / `tableOpOk`。

### 2.2 同步桥（跨 bus 的就地回调）

- `api/sync_bridge.h`：`SyncBridgeCallback` + `invokeSyncBridge` + `isSyncBridgeCallback`。
- `runtime/op_driver.h`：`setCallback(cb, ud, mayBeSyncBridge)` 与 `commit()` 里
  "是同步桥就在完成线程上就地调用"的分支（`host_core.h` 两处、`table_op.h` 一处传 true）。
- kit 侧 `tableOpSyncWait` 用它 + 条件变量；插件执行线程上默认拒绝
  （`SyncWaitThreadPolicy::refuseOnPluginThread`），client 表与主程序侧表允许
  （`allowOnPluginThread`）。

### 2.3 使用面（实测计数）

| 位置 | 数量 |
|---|---|
| kit 四头里的 `TableOpTicket` | 72（kit.h 15、agent_plugin_kit.h 50、plugin_kit.h 6、client_plugin_kit.h 1） |
| kit 四头里的 `Task<` | 40 |
| `agent/plugins`（100 文件）`co_await` / `Task<` / `asio::awaitable` / `asio::` | 377 / 87 / 67 / 235 |
| `tableOpSyncWait` 调用点 | kit 内 18、plugins 2（`agentxx_javascript_engine`）、tests 约 30 |
| 同步工具体（`ToolBodyKind::Sync`）注册点 | 44（`tool(` 共 56，其中 6 `Task` 体、6 `awaitable` 体） |
| `*Sync` 运行时封装（`mountRootSync` 等） | 20 个函数；调用点只有 `agentxx_chatbot`（约 20 处） |

### 2.4 编排上的硬约束

- `asio::awaitable::await_suspend` 只接受 `coroutine_handle<awaitable_frame<U, Executor>>`
  （`boost/asio/awaitable.hpp:112`），所以 **kit 的 `Task` 体永远不能 `co_await` 一个
  `asio::awaitable`**；反方向可行，前提是表调用满足 `is_async_operation`
  （`async_result.hpp:846` 的 `is_async_operation` 判定：`T(Args..., token)` 能经
  `async_initiate` 展开）。
- `asio::awaitable` 的 `await_transform` 只认两类：`awaitable<T, Executor>` 与
  `is_async_operation`（`impl/awaitable.hpp:177/186`）。普通 awaiter 不行。
- 本地 reactor 上时间不会自己流动：`poll_one` 只跑"已就绪"的 handler，本地
  `steady_timer` 到期没有东西去 `wake`。所以 `sleep` 必须继续走主程序 `scheduler.sleep`；
  asio 原生等待要靠"有未完成本地等待时 pump"（现在只有 `polled_tool` 有这个规则）。

## 3. 方案

### 3.1 目标形态

- 工具体只有一种：协程（最终是 `asio::awaitable`）。阻塞工作写成
  `co_await offload(ctx, [&]{ ... })`。
- 表调用是标准 asio 异步操作：`co_await tableOp(ctx, ...)`，完成一律
  `asio::post(ctx.executor(), ...)` + `wake()`；取消接
  `asio::get_associated_cancellation_slot`。
- `ctx.executor()` = 桥的 `localIo_`（不新开线程、不 `run()`，仍由驱动票 `poll_one` 推进）。
- 删除：`TableOpTicket`、`SleepTicket`、`OffloadTicket`、`Task`、`PromiseBase`、
  `BridgeRoot`、`abandonedRoots_`、`startBridgedRoot`、`resumePluginCoroutine`、
  `tableOpSyncWait`、`sync_bridge.h`、`ToolBodyKind::Sync`。

### 3.2 顺序（按依赖排定）

1. **工具体协程化**（不依赖其它步骤，可单独验收）
   - `ToolBodyKind::Sync` 与 `registerToolWithSyncBody` 删除；
   - 44 个阻塞体改成协程体 + `co_await offload(...)`（保留 `execute_start_invoke` 提供的
     `work_dir`，把 `registerToolWithTaskBody` 也切到该入口）；
   - 同步体里对表结果的同步读取（`filterPathPermissions` 等）保持不变：协程体里直接调用
     也合法（仍在工作池线程上规则更宽松），但更推荐用 `co_await` 形态。
2. **插件非协程层协程化**
   - `agentxx_chatbot` 平台回执：`sendInput` 类同步入口改成"排队 + 异步回执"；
   - `agentxx_filesystem` 的权限查询改成协程形态；
   - 测试探针（`start` / `mount`）改成 `finishLater(notify)` + 协程。
3. **删除同步等待**：`tableOpSyncWait`、`detail::tableOpText` / `tableOpOk`、
   `SyncWaitThreadPolicy`、`sync_bridge.h`、`op_driver.h` 的就地回调分支、20 个 `*Sync`
   运行时封装（协程形态已有：`mountRoot` / `openChannel` / …）。
4. **统一到 asio**：表调用改 `async_initiate`；`ctx.spawn` = `co_spawn` + `register_task`
   lease + 恰好一次 notify；给 kit `Task` 的 promise 补 `await_transform`（过渡）或
   直接删除 `Task`；根收尾改成 per-root `cancellation_signal`；pump 规则推广到
   "任何有 asio 原生等待在飞的根"。
5. **收尾**：`whenAll` / `done()` / `forget()` 的替代（`&&` / `parallel_group` /
   `co_spawn`）、`boundaries` 规则、文档。

## 4. 边界与兼容

- C ABI 不变：表版本、`struct_size`、`plugin_spec` 都不动。改的只有插件侧头文件
  （header-only，静态链接进插件）→ 插件源码级不兼容，需整批重编。
- `scheduler.is_io_thread` 保留（语义是"当前线程是不是该插件的执行线程"）。
- `tableOpCallback`（提交 + 回调）与 `*NoWait`（提交即返回）保留，它们是异步形态。

## 5. 验证

- 每步单独编译：顶层 Debug 构建 + `agent/script/build_plugins.ps1` 全量插件。
- 测试：`plugin_bridge` / `plugin_sdk` / `plugin_executor` / `plugin_runtime` /
  `plugins` / `plugin_host_mount` / `plugin_multi_mount` / `plugin_agent_runtime` /
  `plugin_example_runtime` / `async_tools` / `chatbot_*`，再跑全量。
- 关窗与取消是重点：`plugin_runtime` 的 Closing 用例、`plugin_executor` 的租约用例。

## 6. 风险

- 关闭期收尾（帧所有权 / notify 恰好一次 / lease）最易出问题。
- pump 规则推广会改变 CPU 占用（`kPollIntervalMs` / `kPollBurstMax` 要重新标定）。
- 执行器亲和性：所有体必须在 `ctx.executor()` 上；`offload` 回来必须 post 到同一个执行器。
- 顺序敏感用例（`plugin_executor` 的步进与票数断言）预期要改。

## 7. 第 2c 步已完成（2026-11-22）

测试探针里 29 处 `tableOpSyncWait` 全部转完：新增测试专用头
`agent/test/include/agentxx-test/plugin/test_table_op.h`（`tableOpWait` 提交 + 完成回调 +
条件变量；`tableOpSubmit` 只提交），`test_plugin_runtime` 的 create / stop 回调改成
“提交 + 全部完成后回答 notify”（notify 按值存一份，失败原因走 payload）。
`test_plugin_executor` 删掉同步桥自身的用例（它测的就是要删的机制）。
验证：顶层构建 + 25 个插件全量 + 25 个插件相关模块 `passed=6235 failed=0`。

## 8. 第 3 步：执行清单（已完成 2026-11-23）

### 8.1 框架（`agent/third_party/cxx_pluginxx`）

| 位置 | 动作 |
| --- | --- |
| `pluginxx/api/sync_bridge.h` | 整个文件删 |
| `pluginxx/kit/kit.h` L32 | 删 `#include "pluginxx/api/sync_bridge.h"` |
| `pluginxx/kit/kit.h` L3272 / L4932 / L5020 | 删 `SyncWaitThreadPolicy` 与两个 `tableOpSyncWait` 重载（连同 `kMainSideWaitPolicy` 与 L5041~L5078 两个内部助手）|
| `pluginxx/kit/kit.h` L4880 | `tableOpCallback` 注释里去掉“同步桥标记”那段（标记机制没了，`Holder` 仍然要留：它保证 `user_data` ≥ 8 字节）|
| `pluginxx/runtime/op_driver.h` L27 / L750 | 删 include 与“同步桥就地回调”分支（`mayBeBridge && isSyncBridgeCallback(ud)`）|
| `pluginxx/api/tables.h` L412 / `runtime/runtime.h` L512 | 删/改文档里的“能不能阻塞等”说法 |
| `boundaries` 规则 16 | 从“拦回退到同步等待”改成“拦回退到 `tableOpSyncWait` 这个名字”（它已经不存在了）|

### 8.2 kit（`agent/lib/include/agentxx/plugin/api/`）

删：`plugin_kit.h` 的 `using pluginxx::tableOpSyncWait;` 与 4 个广播同步形态
（`declarePluginBroadcast` / `publishPluginBroadcast` / `unsubscribePluginBroadcast` /
`subscribePluginBroadcast` 的 host+broadcasts 重载，含 `PluginxxStringView` 版），
它们的作用已由 ticket 形态（`co_await declarePluginBroadcast(ctx, ...)`）与 `*NoWait`
形态覆盖；`agent_plugin_kit.h` 的 21 个 `*Sync` 封装、`detail::runtimeCall` /
`runtimeCallText`、`addShareStore` 同步形态；`client_plugin_kit.h` 的 `clientTableOp` /
`clientTableText` / `clientTableHandle`。

### 8.3 使用面（删前必须先改）

1. **`agentxx_execute_command`（4 处 `c.addShareStore`）**：`StoreFn` 现在是同步回调
   `std::function<long long(std::string_view)>`，四个调用点都在执行结束组装结果时
   （`makeTimeoutResult` / `bashExecute` / windows 版的超长输出）。改法：
   `StoreFn = std::function<asio::awaitable<long long>(std::string_view)>`，
   `makeTimeoutResult` 与调用它的组装段改成协程，调用点换 `co_await`；
   windows 那条 受控轮询体本来就是 `asio::awaitable`，直接用 `addShareStoreAsync`。
2. **`agentxx_filesystem`（2 处）**：`filterPathPermissions` / `checkPathDecisions`
   要新增协程形态（`co_await` 一个 `tableOpAsync`），并把 glob/grep/list 的权限过滤
   从那几个阻塞体搬到协程体（体从 `onWorker(...)` 改成 `asio::awaitable`）。
3. **`client_plugin_kit.h` 内部**：`clientRegisterCommand` / `registerToolRenderer`
   等 6 处用 `clientTableOp` 同步取结果，要改成 `clientTableOpAsync` + `co_await`
   （这些函数本身要变成 `TableOpTicket` 或 `awaitable`），调用方是 client 插件的
   `start`/`mount` 事务（已在协程里）。
4. **测试**：`test_plugins.cpp` / `test_client_plugins.cpp` 里用广播同步形态的地方
   改成 ticket 形态或 `agentxx::test::tableOpWait`；`boundaries` 里的禁用名清表跟着改。

### 8.4 验证

顶层 Debug 构建 + `build_plugins.ps1` 全量 25 个 + 上面 §5 那批模块 + `boundaries`。

## 9. 第 4 步：已完成（2026-11-23）

本节当初列出的三个前置机制（本地 reactor、取消通道、帧所有权）都已落地，实施细节、
三个帧生命周期约束与验证数字见 `work.md` 的"第 4 步"一节。下面是开工前查清的事实，
保留作为设计依据：

1. kit 的协程**已经有**自己的本地 reactor：`pluginxx::detail::PollOneBridge` 持有一个
   `asio::io_context`，由 `PluginxxDriver::runOnPlugin()`（`pluginxx/runtime/driver.h:246`）
   每轮 `driveOnce()` 抽。所以“给插件执行线程一个 io_context”不是从零做，而是把
   现有的 `PollOneBridge::postToLocal` 换成 `asio::post(io, ...)`、把 `driveOnce` 换成
   限时限量的 `io.poll()`。
2. `Task` 要换成 `asio::awaitable` 的前提是两个机制先有替代：
   - **取消**：`Task::promise_type::set_outstanding`（`cancel_outstanding()`）→ 在
     `spawn` 登记一条 `asymmetric` cancel 通道（`asio::cancellation_signal`），
     取消时 `emit(cancel_type::terminal)`；
   - **帧所有权 / 结束通知**（`BridgeRoot`）：`ctx.spawn` 改成 `co_spawn(io, ...)` +
     现有 `register_task` lease（`unloadAsync` 等的就是它），收尾在协程最后一帧发出。
3. 90 处 `Task<...>` 分布在 24 个插件文件；`tool()` / `capability` / `graph_node` 的
   “按返回类型分派”要同步改成“只认 `asio::awaitable` 与阻塞体（`onWorker`）”。
4. 表调用统一到 `asyncTableOp`（已就绪）；`whenAll(tickets)` → `co_spawn` + 收集，
   `.forget()` → `co_spawn(...)` 或 `tableOpCallback`。
5. 迁移顺序建议：先在 kit 里让 `Task` 成为 `asio::awaitable` 的薄别名并让
   `spawn`/`finishLater`/`offload`/`sleep` 全部换成 asio 原语（保持 `Task` 名字，
   插件源码不动），验证全绿；再把 90 处 `Task` 名字一次性换成 `asio::awaitable`；
   最后删 `PromiseBase` / `BridgeRoot` / `TableOpTicket`。这样每一轮都是可验证的小片。

## 10. 第 5 步：已完成（2026-11-24）

按"先删消费者 → 改用例 → 删 kit 机制 → 收尾"的顺序做完，`kit.h` 6156 → 4227 行
（40 个整声明），`agent_plugin_kit.h` -1968 行、`plugin_kit.h` -203 行；插件的协程只有
`asio::awaitable` 一种，`Task` / `PromiseBase` / `BridgeRoot` / `TableOpTicket` /
`SleepTicket` / `OffloadTicket` / `InvokeCapAwaiter` / `yield` / `whenAll` / `tableOp` /
`capability`（ticket 形态）全部下线，`PollOneBridge` 只保留驱动与受控轮询。

删后保留的入口与语义不变：`asyncTableOp` / `tableOpCallback` / `asyncOffload` /
`sleepAsync` / `offloadAsync` / `callToolAsync` / `callToolResultAsync` /
`capabilityAsync` / `providePointAsync` / `graphNodeAsync` / `finishLater` /
`PolledRoot` / `PollOneBridge` 的请求与退避机制。

实施记录（改了什么、踩了什么坑、验证数字）见 `work.md` 的"第 5 步第 3/4 小步"一节。
