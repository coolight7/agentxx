# plugin-async-unify —— 插件侧接口统一异步（彻底重构）

> 计划见 [plan.md](plan.md)，任务卡片见 [index.md](index.md)。
> 本文件记"实际做了什么 + 怎么验证 + 与计划的偏差"，每完成一步就写。

## 进度总表

| 阶段 | 内容 | 状态 |
|---|---|---|
| A | 框架层：ticket（调用即提交）+ `whenAll` + 顺序契约写进头文件 | 已完成 |
| B | kit 改名与补形态：协程（ticket）形态补齐，同步重复形态删除 | 已完成 |
| C | 迁移 25 个内部插件（含 chatbot 10 个工具） | 已完成（JS 引擎除外，只能走同步桥） |
| D | 生命周期异步完成（`finishLater`；不设时限、不加配置键） | 已完成 |
| E | 删除全部阻塞形态 + `boundaries` 规则 16 + 文档 | 已完成 |
| G | 收尾：文档、全量回归、卸载路径 ASan 复核 | 进行中 |

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

### 批 D（部分）：kit 的“稍后完成”凭据 `finishLater`（已完成）

生命周期四个入口（`start` / `mount` / `stop` / `unmount`）都是“受理 + 完成后 notify”的契约；
插件里要把注册类调用改成 `co_await`，就需要一个能跨挂起持有 notify 的凭据。新增：

- `detail::LifecycleFinisher`（对外别名 `pluginxx::LifecycleFinisher`）：内部持
  `shared_ptr<CompletionGuard>`，`ok` / `failed` / `canceled` / `fromCurrentException` 恰好一次；
  **所有副本都销毁时仍未完成就自动补 `FAILED`**（复用 `CompletionGuard` 的析构语义），
  主程序不会因为插件忘了通知而永远等下去。
- `pluginxx::finishLater(notify)` 便捷构造；`agentxx::plugin` 补 `using pluginxx::finishLater;`
  与 `using pluginxx::LifecycleFinisher;`。
- 测试 `plugin_bridge` 第 20 段：入口把活交给协程后立刻返回、协程 `co_await yield` 后
  `later.ok("started")` → 主程序收到一次 `OK`；重复 `ok()` 不再回调；忘了完成 → 一次 `FAILED`
  （正文含 `did not complete`）。为让 `spawn` 在伪主程序里可用，夹具补了伪 `pluginxx.tasks` 表
  （`register_task` 只返回一个非空句柄）。

结果：`plugin_bridge` 270 条全绿（原 259 + 新增 11）。

### 批 B（第一批）：kit 的协程形态（已完成）

给常用的表便捷入口补了“**调用即提交** + `co_await` 拿结果”的同名重载（第一个参数
是插件实例上下文），阻塞形态继续保留，两条形态并存，插件可以逐处迁移：

- 广播：`declarePluginBroadcast` / `publishPluginBroadcast` /
  `subscribePluginBroadcast` / `unsubscribePluginBroadcast`，另有
  `subscriptionOf(result)` 从结果正文取订阅句柄；
- 提示词 / 模型 / 图 / 自身资源：`promptJson` / `setPromptJson` / `modelConfigJson` /
  `graphJson` / `graphName` / `setGraphJson` / `ownResourcesJson`；
- 资源登记：`registerSkillDir` / `unregisterSkillDir` / `registerMemoryFile` /
  `unregisterMemoryFile` / `registerMcpServer` / `unregisterMcpServer`（公共实现
  `resourceOpAsync`）；
- 挂载：`listMounts` / `mountInfoOf`（阻塞形态改名 `listMountsBlocking` /
  `mountInfoOfBlocking`，无插件调用点）；
- `PluginBase` 成员：`addShareStore` / `workDir`（原 `xxxAsync` 改名，现在就是协程形态）、
  `getSessionMessages` / `sessionMessagesCount` / `toolPrompt` 的协程形态。

验证：`plugin_bridge` 270 / `plugin_sdk` 134 / `plugin_host_mount` 147 /
`plugin_runtime` 742 / `plugin_multi_mount` 47 / `boundaries` 19 全绿。

剩下未做（下一批）：权限（`registerToolPermission` 等，签名冲突需先改名）、会话运行时与
后台任务、工具/图节点注册（`registerToolSync` / `tool` / `graph_node`）的协程形态；
之后是插件调用点迁移、删阻塞形态、禁令与文档。

### 批 C/D/E：插件迁移 + 删除阻塞形态（已完成）

**D 的形态**（每个带注册事务的入口都按同一套写）：

```cpp
static pluginxx::Task<void> xxxSetupAsync(Ctx& ctx, AgentRef ref) {
    co_await xxx(ctx, ref, ...);      // 每条注册都是“提交 + co_await”
    co_return;
}
// 入口
if (!ctx.iface.tasks || !ctx.iface.tasks->register_task) { /* error + nullptr */ }
auto later = finishLater(notify);
const AgentRef agentRef(*spec);
ctx.spawn([later, agentRef](Ctx& c, OpCtl&) -> pluginxx::Task<void> {
    try { co_await xxxSetupAsync(c, agentRef); later.ok(); }
    catch (const std::exception& e) { later.failed(e.what()); }
    co_return;
});
return later.provider();   // 非空句柄 = 已受理
```

- 挂载带**挂载级状态**时，入口返回自己的状态对象（example_event 的
  `EventMountState`、rag_search 的 `RagAgentState`、example_multi_mount 的
  `shared_ptr<MountState>` 持有块），unmount 收到的就是它；完成通知仍走 `later`。
  挂载失败时主程序不调 unmount，所以这些状态在协程的 `catch` 里释放
- 没有 `pluginxx.tasks` 的宿主：入口直接报错（不再退回“同步注册”，那等于同一份注册
  写两遍）；因此各插件的 `xxxSetupBlocking` 同批删除

**已迁移的插件**（25 个内部插件全部）：

| 批 | 插件 |
|---|---|
| D1 | `agentxx_filesystem`（21 处 `co_await`）、`agentxx_math`、`agentxx_string`、`agentxx_execute_command`、`agentxx_system_monitor`、`agentxx_system`、`agentxx_websearch` |
| D2 | `agentxx_codegraph`（工具提示词读改写 + 5 个工具 + 事件订阅）、`example_plugin`（工具/广播/能力/提示词）、`agentxx_llm_uncensored`、`example_feature`、`example_graph_node`、`example_event`、`agentxx_rag_search`、`agentxx_planning` |
| D3 | `agentxx_chatbot`（10 个工具 + 面板订阅）、`agentxx_computer_use`、`agentxx_screen_capture`、`agentxx_audio_stream`（当前平台不编译）、`agentxx_text_selection_monitor`、`example_resources`、`example_multi_mount`、`example_runtime`、`agentxx_system_monitor` 的 start 侧 |
| 不动 | `agentxx_javascript_engine` / `example_js` / `example_js_execute_command`：QuickJS 是同步 C API，脚本回调必须同步拿结果，只能走同步桥原语 |

**kit 同时补的协程形态**（原阻塞形态同批删）：`toolPromptOf` / `schemaOf` /
`parseToolPromptJson`（工具提示词与模式构建）、`subscribePluginBroadcastEx`、
`providePoint` / `capability` / `graph_node` 全部改成返回 `TableOpTicket`。

**E：删阻塞形态**

- 框架 `kit.h`：删除 `tableOpBlocking(ctx, ...)`、`invoke_capability_blocking`、
  `call_tool_blocking`；`tableOpBlockingRaw` 改名 **`tableOpSyncWait`** 并写清用途
  （只给必须同步回答外部同步 API 的桥，加上 `tableOpSyncWait(ctx, ...)` 重载）。
- `plugin_kit.h`：删 `call_tool_blocking`、`registerToolPermissionBlocking` /
  `registerReadPathPermissionBlocking` / `registerWritePathPermissionBlocking` /
  `listMountsBlocking` / `mountInfoOfBlocking`。
- `example_js` / `example_js_execute_command`：start 里那一次同步 `capability_info` 查
  改为“直接 invoke —— 未注册就不受理”，不再需要同步等待。
- `boundaries` 新增**规则 16「插件表调用形态」**：插件/lib 源码出现已删除的四个名字即失败；
  `tableOpSyncWait` 只允许出现在 JS 运行时插件（同步桥）与 kit 内部。

**验证**：`boundaries` 20 / `plugins` 704 / `plugin_host_mount` 147 / `plugin_bridge` 271 /
`plugin_runtime` 742 / `plugin_sdk` 141 / `plugin_resources` 101 / `plugin_broadcast` 96 /
`plugin_point` 68 / `plugin_feature` 34 / `plugin_cleanup` 90 / `plugin_multi_mount` 47 /
`plugin_agent_runtime` 221 / `plugin_example_runtime` 50 / `plugin_example_chatbot` 265 /
`client_plugins` 841 / `event_hub` 288 全绿（Debug + ASan）。

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

- 全量回归（全部测试模块）与卸载路径 ASan 复核
- `docs/zh-cn/design/plugins.md` §8.0 已改成两形态 + 同步桥原语说明; §15（实例生命周期）
  里 “mount 里同步注册后 `notify->done`” 的示例已由本节模板取代, 阅读时以 8.0 为准

### 批 B 开工前的两个决定（实施时按这个来）

1. **没有 ctx 的便捷函数**（`promptJson` / `graphJson` / `registerSkillDir` / 权限 /
   广播等自由函数）拿不到协程驱动桥，做不出 ticket。因此每个都补一个 `Ctx&` 形态：
   `template<typename Ctx> TableOpTicket xxx(Ctx& ctx, const ScopeRef& agent, ...)`，
   内部 `return tableOp(ctx, ...)`；`host` / `iface` 从 `ctx` 取，参数比现在更短。
2. **改名与插件调用点同一批提交**：`xxx(...)` → `xxxBlocking(...)` 会让 25 个插件的
   100 多个调用点立刻编译不过，所以 B（kit 改名 + 补 ctx 形态）与 C（插件迁移）必须
   在同一个编译循环里完成；生命周期回调里的调用点先继续用 `*Blocking`，等批 D 把
   生命周期改成可异步完成后再迁。
