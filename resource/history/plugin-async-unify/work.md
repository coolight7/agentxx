# plugin-async-unify —— 插件侧接口统一异步（彻底重构）

> 计划见 [plan.md](plan.md)，任务卡片见 [index.md](index.md)。
> 本文件记"实际做了什么 + 怎么验证 + 与计划的偏差"，每完成一步就写。

## 进度总表

| 阶段 | 内容 | 状态 |
|---|---|---|
| A | 框架层：ticket（调用即提交）+ `whenAll` + 顺序契约写进头文件 | 已完成 |
| B | kit 改名与补形态：同步形态先改 `*Blocking`，主名让给 ticket / callback 形态 | 待做 |
| C | 迁移 25 个插件（含 chatbot 10 个工具改 `polled_tool`）+ 删 4 个私有 `netCallWait` | 待做 |
| D | 生命周期异步完成（`finishLater`；不设时限、不加配置键） | 待做 |
| E | 删除全部阻塞形态 + `boundaries` 禁令 + 文档 | 待做 |
| G | 收尾：文档、全量回归、卸载路径 ASan 复核、性能对比 | 待做 |

不做：client 侧回答型回调与 150 ms 同步桥（留给 `plugin-api-async` 的阶段 D/E）。

## 分步记录

（每完成一批补一节：改了哪些文件 / 怎么验证 / 遇到的问题）

### 批 A：框架层 ticket + `whenAll`（已完成）

**改动**

1. `agent/third_party/cxx_pluginxx/include/pluginxx/kit/kit.h`
   - `detail::TableOpState` 增加 `coroAddr` / `resumeEntry`（类型擦除的恢复入口）、`bridge`、
     `consumed` 原子标记，并把 `INIT / CALLING / SUSPENDED / COMPLETED` 四态的含义写在注释里
     （提交早于 await，所以完成回调可能在任意时刻到达）。
   - 删除 `detail::TableOpAwaiter`（原来提交发生在 `await_suspend` 里，无法“先提交后 await”），
     换成 `detail::TableOpTicket` + `detail::submitTableOp(...)`：
     - 构造 ticket 时就调用 `start(cb, ud, &err)`（**调用即提交**），未受理时填 `startError`
       并把状态直接置 `COMPLETED`（等价于“有结果了，原因是没受理”）；
     - `co_await ticket`：`await_ready` 看 `done()`；`await_suspend` 装恢复入口 → `INIT→CALLING`
       →（回调若在此窗口完成则置 `COMPLETED` 不恢复）→ `CALLING→SUSPENDED` → 登记
       `set_outstanding(op_cancel)`；
     - `accepted()` / `done()` / `cancel()` / `forget()`，结果只能取一次（`consumed` CAS，
       第二次拿到 `accepted()==false` + `table op result was already taken`）；
     - ticket 可复制（共享同一份状态，语义同 `std::shared_future`），方便放进容器 / `whenAll`。
   - `tableOp(ctx, start)` 改为返回 `TableOpTicket`；导出别名 `pluginxx::TableOpTicket`。
   - 新增 `whenAll(std::vector<TableOpTicket>)` → `Task<std::vector<TableOpResult>>`：ticket 提交时
     就都在跑，逐个 await 收结果，总耗时 = 最慢那条，结果与传入顺序一致。
2. `agent/third_party/cxx_pluginxx/include/pluginxx/host/table_op.h`：把顺序契约写全 ——
   IO 线程内联（注册立刻可见）/ 其它线程按受理顺序 FIFO / **不同线程各自提交只保证受理顺序**，
   要严格先后就在上一条的结果里再发下一条。
3. `agent/lib/include/agentxx/plugin/api/plugin_kit.h`：补 `using pluginxx::TableOpTicket;` /
   `using pluginxx::whenAll;`（插件侧不写 `pluginxx::` 前缀）。

**顺带修掉的三处框架缺陷**（都是“子 Task 帧由外部回调恢复”这条路径上的问题，`whenAll` 是第一个
真正用它的地方）：

1. `finishIfDone` 的根完成正文对 `Task<T>` 用了无条件的 `fmt::format("{}", result)`，
   `Task<std::vector<TableOpResult>>` 直接编译失败（`type_is_unformattable_for`）。改为
   `fmt::is_formattable` 判定，不可格式化时正文留空（这种 Task 只作为子协程用）。
   注意 `requires { fmt::format(...) }` 不行：要求表达式只看声明，fmt 的不可格式化是函数体里的
   不完整类型，照样实例化报错。
2. `resumePluginCoroutine` 原来恢复完一律调 `finishIfDone(handle)`；子 Task 帧结束时父协程会
   在 `await_resume` 里销毁它，于是这句就是 use-after-free（ASan 实测 `coroutine_handle<...>::done()`）。
   现在只有**根帧**（`continuation_` 为空）在这里收尾。
3. 只跳过还不够：根也可能在**子帧这次 resume 里**结束（子帧 `final_suspend` 把控制权交回父链），
   那条路径上没有根帧句柄，根就永远不会上报完成（实测 `probe.calls==0`）。补法：
   - `PromiseBase::bridgeRoot_` 由子帧继承（`Task<T>` / `Task<void>` 的 `Awaiter::await_suspend`
     改成模板，父协程是插件帧时把弱引用传下去）；
   - `BridgeRoot` 增加类型擦除的 `finishFrame_`（`startBridgedRoot` 里绑成 `finishIfDone<Promise>`）
     与 `finishFrameIfDone()`；
   - `resumePluginCoroutine` 恢复子帧后调 `root->finishFrameIfDone()`（幂等：完成权 CAS + 帧指针置空）。

**测试**：`agent/test/plugin/test_plugin_bridge.cpp` 新增 15~19 五段（`PendingTableCalls` 伪表调用
记录 + `Harness::newOpHandle` / `canceledHandles`）：

- 15：调用即提交（构造后 `accepted()==true`、`done()==false`）、多条同时跑、`forget()`、
  乱序完成、`cancel()` 走 `op_cancel`、正文只能取一次、未受理时 `accepted()==false` + `done()==true`；
- 16：协程里稍后 await（首步提交 → 挂起 → 完成回调只 post+wake → 下一次请求恢复）；
- 17：`whenAll` 三条逆序完成，后完成的两条**不产生驱动请求**，结果按传入顺序 `a,b,c`；
- 18：协程被取消时正在等的表调用收到 `op_cancel`，终态仍由表调用给出；
- 19：提交即完成（内联）时 `await_ready()` 直接为真，一次请求内跑完、不再申请驱动。

**验证**（`agent/build/windows-debug`，Debug + ASan）：

- 重建链路：cxx_pluginxx（build+install）→ `agentxx_static`（含 install）→ test（拷 exe 到 `exec/`）→
  `build_plugins.ps1 -KeepGoing` 全部插件；
- `plugin_bridge` 259 条全绿；`plugin_sdk` / `plugin_runtime` / `plugin_host_mount` / `plugin_broadcast` /
  `plugin_feature` / `plugin_point` / `plugin_dispatch` / `plugin_cleanup` 共 1411 条全绿；
  `boundaries` / `plugin_multi_mount` / `event_subagent` / `async_tools` 共 296 条全绿。

## 与计划的偏差

- 计划里批 A 写的是“`tableOpBlocking*` 加 `[[deprecated]]`（暂留）”，实际**没有加**：这些阻塞形态
  在本任务的批 E 就会整体删除，中间加属性只会让 110 多个调用点的编译输出被警告淹没。删除按原计划在
  批 E 做。
- 计划草图里 `whenAll` 返回 `asio::awaitable`，实际返回 `pluginxx::Task<std::vector<TableOpResult>>`：
  asio 的 awaitable 在 `await_suspend` 里要父协程的 executor，而插件协程帧（`pluginxx::Task`）没有
  `get_executor()`，在插件协程里 `co_await` 它编译不过。`Task` 是本包自己的协程类型，插件里到处在用。
- 为了让 `whenAll` 能工作，批 A 顺带修了上面“三处框架缺陷”，比计划的“只加 ticket”范围大一点，
  但都属于同一条路径（子 Task 帧由外部回调恢复）的必要条件。

## 剩余事项

（收尾时补）
