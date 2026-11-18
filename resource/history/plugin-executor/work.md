# 插件独立线程（插件执行器）—— 实施记录

> 计划见 [plan.md](plan.md)，任务卡片见 [index.md](index.md)。
> 本文件只记"实际做了什么 + 怎么验证 + 与计划的偏差"，每完成一步就写。

## 进度总表

| 阶段 | 内容 | 状态 |
|---|---|---|
| A | 框架双执行器地基（`pluginExecutor` + `dispatchToPlugin` + 生命周期派发） | 完成 |
| B | 工具 / 图节点 / 能力行（`OpCore` 的 start / cancel / 完成回调）派发 | 完成 |
| C | 通知类（驱动票、通道事件、后台任务事件、广播订阅）派发 | 完成 |
| D | agent 侧与 client 侧各自起插件线程，client 派发点迁移 | 完成（agent 侧默认起；client 侧派发点已迁、线程开关可选，原因见第 5 步） |
| E | 收尾：`is_io_thread` 语义、文档、全量回归 | 进行中（文档与注释已改；全量回归待跑） |

## 关键实现决定（开工前定下）

1. **`pluginExecutor` 为空 = 行为完全不变**。框架新增的派发一律走
   `PluginRuntime::pluginExecutor`，为空时退回 `executor`（io）。这样：
   - 直接 `make_shared<PluginManager>(ctx)` 的测试与嵌入方不受影响；
   - 先落派发机制、再开插件线程，出问题时能分清是"契约改坏"还是"线程改坏"。
2. **lease 在派发时取，不在执行体里取**（计划写的是执行体里取）。差别：排队中的
   派发项也算"在途"，关闭会等它跑完（`waitInflightZero`），dlclose 更安全；
   执行体里取的话，空闲通知可能早于执行体拿 lease，存在一个窗口。
3. **插件执行器上不再内联**：所有"主程序 → 插件"的调用一律投递（同实例单队列
   FIFO）。主程序 io 线程**永不阻塞**等插件线程；反方向（插件线程阻塞等 io）在
   过渡期允许，`ioCallSyncKeep` 保持原样。
4. **`scheduler.is_io_thread` 语义改为"当前线程是不是该插件的执行线程"**：名字不变
   （避免 ABI 换代），但它现在回答的是"在这里阻塞会不会卡死插件" —— kit 的
   `tableOpSyncWait` 用它拒绝在插件线程上阻塞。
5. **不做实例自带执行器**（计划 §5.4 的 JS 引擎优化）。JS 引擎保持"io → 插件线程 →
   JS 线程"三跳：它自己的同步桥（`tableOpSyncWait`）必须跑在**不是**插件执行线程的
   线程上，声明自己的执行器反而会与同步桥互等。框架留 `PluginExecutionBinding` 字段
   与判定入口，留给将来"自带 asio 执行器且不用同步桥"的插件。

## 分步记录

### 第 1 步：现状调查结论（对应 plan.md §4/§6）

"主程序 → 插件"的调用点收敛后其实只有 6 个原语，这是本任务可控的关键：

| 原语 | 位置 | 现状 |
|---|---|---|
| `OpCore::start`（`drive.start`） | `pluginxx/runtime/op_driver.h` | io 线程内联调用插件 |
| `OpCore::cancelOnIo`（`drive.cancel`） | 同上 | io 线程内联 |
| `OpCore::commit`（插件完成回调 `callback_`） | 同上 | io 线程内联 |
| `PluginxxDriver::runOnIo`（驱动票） | `pluginxx/runtime/driver.h` | `enqueueRuntimeAction` → io |
| `PluginHostCore::postCallback/sleep/offload` 的插件回调 | `pluginxx/host/host_core.h` | io 线程内联 |
| `BroadcastRegistry` 订阅处理器 | `pluginxx/host/broadcast_registry.h` | 发布线程直接调插件 |

agent 侧的业务派发点**已经全部走 `OpCore`**（工具 `PluginTool::execute_async`、
图节点 `PluginGraphNode::run`、挂载 `mountOneAsync`/`unmountOneAsync` 都经
`awaitHostPluginOpResult` / `awaitPluginLifecycle`），因此改 `OpCore` 一处即覆盖大半。

剩余的 agent 侧直调点：通道事件 `PluginManager::deliverChannelEvent`（
`plugin_manager_runtime.cpp`）、后台任务事件（`plugin_manager_runtime*.cpp`）。

### 第 2 步：阶段 A——框架双执行器地基

改了哪些文件（插件框架 `cxx_pluginxx`）：

- `runtime/runtime.h`：`PluginRuntime` 增加 `pluginExecutor` / `pluginThreadId` /
  `callbackStats` 与插件侧待重放队列；新增 `hasPluginExecutor` /
  `pluginExecutionExecutor` / `isRuntimePluginThread` / `bindPluginExecutor` /
  `enqueuePluginAction(On)` / `replayPluginActions`。两条队列的登记/摘除/重放逻辑
  合并成 `enqueueRuntimeActionOn` + `runRuntimeAction(queue)`，只差落哪个成员。
- `runtime/plugin_thread.h`（新增）：`PluginExecutorThread`（io_context + work guard +
  一条线程，平台线程名），`stop()` **不释放 io_context**（见下面“踩到的坑”）。
- `runtime/instance_base.h`：新增 `PluginDispatchOptions` / `PluginDispatchContext` /
  `runPluginDispatch` / `dispatchToPlugin`——这是“主程序调插件”的唯一入口。
- `runtime/manager_base.h`：`setPluginExecutor` / `hasPluginExecutor` / `isPluginThread` /
  `pluginCallbackStats` / `setPluginCallbackBudget`。
- `runtime/op_driver.h`：`OpCore` 拆成 `prepareStart`（io）+ `invokePluginStart`（插件线程）
  + `finishStart`（io）；新增 `startAsync`（io 协程，投递到插件线程并等结论）；
  cancel 与插件完成回调各自派发；`createDetached` 增加 `callbackOwner`（表调用的完成
  回调属于调用方插件，要回到它的执行器并持有 lease）。
- `runtime/driver.h`：驱动票 `schedule()` 改投插件执行器，`runOnIo` → `runOnPlugin`。
- `host/host_core.h`：能力链 `startNext` 改成协程 `advance`（每行一个协程帧，不递归）；
  `subscribeBroadcast` 的 handler / onEnd 派发；`offload` 的 `done` 派发；删掉
  `postCallback`（scheduler 表已无 `post_to_io`，此函数无调用者）。
- `host/table_op.h` + `host/tables_impl.h`：`startTableOp` 增加 `callbackOwner`（表调用的
  完成回调投回插件线程）；`scheduler.is_io_thread` 入口改成 `isPluginThread` 语义。

主程序侧（agent）：

- `agent_host.h/.cpp`：`AgentHost` 拥有 `pluginxx::PluginExecutorThread`（**声明在
  `pluginHost_` 之前**，逆序析构保证管理器先卸载、线程后停），`loadPluginsAsync` 里启动
  并 `setPluginExecutor`；起不来只记警告并退回旧行为。
- `plugin_manager_runtime.cpp`：通道事件 `deliverChannelEvent` 与后台任务事件
  `onEvent` 改为 `dispatchToPlugin`（`allowClosing=true`：关闭/禁用时那条 CLOSED 事件
  仍必须能送到）；`RuntimeChannel` 增加 `inst` 弱引用，建通道时填好。

测试：新增 `plugin_executor` 模块（8 组、651 条断言）：派发线程与 FIFO、lease 与关闭后
丢弃、预算统计、`OpCore` 的 start/cancel/完成回调线程、生命周期入口、驱动票、广播
处理器、执行器停止后的丢弃。

验证（`agentxx_test --log-level warn`）：`plugin_runtime` 742、`plugins` 704、
`plugin_sdk`/`plugin_bridge`/`plugin_cleanup` 502、`plugin_host_mount` 147、
`plugin_multi_mount` 47、`plugin_multi_instance` 80、`plugin_feature` 34、`plugin_point` 68、
`plugin_broadcast` 96、`plugin_dispatch` 112、`plugin_agent_runtime` 221、
`plugin_example_runtime` 50、`plugin_example_chatbot` 265、`async_work` 138、
`async_tools` 178、`event_tools` 97、`chatbot_task` 138、`agent_host` 190（这条走真实
`AgentHost::loadPluginsAsync` + `agentxx_math`，即插件线程的真实端到端路径）、
`plugin_executor` 651 —— 全部 0 失败。

踩到的坑：

1. `std::function` 要求目标可拷贝，而闭包里捕了 move-only 的 `InstanceLease` 编译不过
   （C2338）。改成“载荷放进 `shared_ptr<PluginDispatchContext>`，lambda 只捕这个指针”。
2. `PluginExecutorThread::stop()` 一开始把 io_context 也释放了，于是运行时里那份
   executor 副本变成悬垂执行器，停线程后再派发就是 heap-use-after-free（ASan 实测，
   由 `plugin_executor` 第 8 组抓出来）。现在 `stop()` 只停线程不释放 context，
   `context` 留到对象析构。
3. `plugin_broadcast` / `plugins` 里的伪实例没有 `runtime`，而旧行为是“投递即同步调用”
   插件处理器。于是 `dispatchToPlugin` 在**没有独立插件执行器**时直接就地执行（与改造
   前完全一致），只有真配了插件线程才真的投递。
4. 通道 CLOSED 事件是在实例已经进入 Disabled/Closing 之后才发出的，按常规 lease 会被
   丢弃（`plugin_agent_runtime` 因此卡 30 s 超时）。事件类派发统一用 `allowClosing=true`。
5. 手工构造的测试实例没有 `self` 自引用，`channel.inst` 就取不到实例；建通道时改成
   “自引用优先、否则从实例表 `find(name)`”。

### 第 3 步：阶段 D——两条插件线程（agent 侧默认、client 侧迁派发点）

agent 侧（已默认生效）：`AgentHost` 持有 `pluginxx::PluginExecutorThread`（声明在
`pluginHost_` 之前，逆序析构保证“先卸插件后停线程”），`loadPluginsAsync` 里
`start("agentxx-plugin")` + `setPluginExecutor`；起不来只记 WARN 并退回旧行为。

client 侧（`ClientPluginManager`）：

- 新增 `bool startPluginThread()`（公开，可重复调用）：起线程 + `setPluginExecutor`；
  **默认不调用**（构造函数里留注释指向它）；
- 六个派发点全部改成 `dispatchToPlugin`：UI 动作 / 定时器 / 快捷键 / 工具渲染器 /
  命令 `execute` / 广播订阅（后者本来就经框架广播表，已派发）；
- 为什么默认不开：内置 client 插件的 `start` / `stop` 里还在用同步表助手
  （`clientRegisterStatusItem` / `clientRegisterCommand` / `clientUnregister*` 这些
  尽头是 `clientTableOp` → `tableOpSyncWait`）。表操作的完成回调要投回**插件执行线程**，
  而同步等待正把这条线程占住 —— 实测现象是 `example_plugin` 的 start 报
  `registration transaction failed`，`client_plugins` 第 838 行断言失败。
  `tableOpSyncWait` 在插件执行线程上直接拒绝（返回原因文本）而不是死锁，这是故意的。
  把这些注册事务改成异步形态（`finishLater(notify)` + 回调里 `notify`）后才能默认打开。

### 第 4 步：连带修 JS 引擎 start 与自己暴露的测试问题

1. `agentxx_javascript_engine` 的 `jsEngineStart` 原来用 `capabilitySyncWait` 登记
   `interpreter.js` 能力行。agent 侧默认起插件线程后，这行代码跑在插件线程上 → 同步等待
   被拒绝 → 引擎加载失败（真机上就是“JS 插件全废”）。现改为异步形态：入口
   `register_capability` 带完成回调并返回堆状态对象作为 provider 句柄，结论在
   `jsCapRegDone` 里交回（成功记位 + 交 OK；失败停掉刚起的引擎线程 + 交 FAILED）。
   堆状态里只存 `notify` 拷贝（不存 `LifecycleFinisher`），所以“入口最后未受理”时不会
   提前发一次 done。
2. 新增用例 `plugin_executor` 第 9 组：真实 `AgentHost::loadPluginsAsync` 加载
   `agentxx_javascript_engine` + `example_js`（插件线程真在跑），断言
   `hasPluginExecutor()`、能力行 `plugin.agentxx_javascript_engine.interpreter.js`
   登记成功、实例 enabled 且 capabilities 非空，最后 `shutdownAsync()`。
3. `plugin_runtime` 的 `offload` 完成用例原来断言“回调时 lease 恰好 1 份”，派发项自己也
   持一份 lease → 改为 `!= 0`（与原有意图“回调期间实例受保护”一致），“done 在 io 调用”
   在无插件执行器的夹具上仍成立。

验证（新鲜 exe + `--log-level warn`，0 失败）：`plugin_executor` 655（+4）、
`plugin_runtime` 743、`plugins` 704、`plugin_sdk` 141、`plugin_bridge` 271、
`plugin_cleanup` 90、`plugin_host_mount` 147、`plugin_multi_mount` 47、
`plugin_multi_instance` 80、`plugin_feature` 34、`plugin_point` 68、`plugin_broadcast` 96、
`plugin_dispatch` 112、`plugin_agent_runtime` 221、`plugin_example_runtime` 50、
`plugin_example_chatbot` 265、`client_plugins` 839、`agent_host` 190。
JS 引擎插件重建（`build_plugins.ps1 agentxx_javascript_engine`）通过。

问题：`plugin_executor` 第 9 组卸载顺序是由 `shutdownAsync()` 决定的，`example_js` 在引擎
已经 closed 后才调能力行，日志会有一条 WARN：
`capability ...: cannot start provider ...: provider is closed or disabled`
（不影响断言）；要给“先卸脚本后卸引擎”的严格顺序，应在用例里手动按依赖卸载。

### 第 5 步：阶段 E——语义与文档收尾

- `scheduler.is_io_thread` 保留成员名（避免 ABI 换代），语义改为“当前线程是不是**该插件**
  的执行线程”，并在 `pluginxx/api/tables.h`、`pluginxx/kit/kit.h`（`tableOpSyncWait` 线程规则与
  拒绝文案）、`agentxx/plugin/api/plugin_api.h` / `client_plugin_api.h`（新增“线程模型”一段，
  逐条把“client io 线程”改成“插件执行线程”）、`plugin_manager.h` / `client_plugin_manager.h`
  类注释、`client_event_sink.h`、`plugin_kit.h`（`clientTableOp` 助手段）里同步更新。
- `docs/zh-cn/design/plugins.md`：重写 §15.3（四角色 + 派发规则 + 预算 + 同步桥禁令）、
  §15.1 的 start 线程、§15.4 的 disable、§8.0 完成回调线程与同步桥补充、§9 的 cb 线程与
  同步助手限制、§9.5 / §9.6 回调线程、§16.5 的 polled 协程线程、§19.7 的事件回调线程、
  §22.2 的“不得在插件执行线程上等”。
- `AGENTS.md`：新增“插件执行线程 (2026-11)”条目（派发入口 / lease / FIFO / 预算 /
  `is_io_thread` 语义 / client 默认不开的原因），并修正表调用形态与 `agentxx.net` 两处的
  线程描述、测试模块清单加 `plugin_executor`。
- 不做的事（与 plan 的差异）：不删 `is_io_thread`（语义已变，仍是同步桥的安全闸）；
  `ioCallSync` / `ioCallSyncKeep` 只剩两处（`call_tool` 与 `mount` 表入口），它们把 body
  拉到 io 后立即返回，属于“插件线程等 io”的允许方向，因此**不删**。

### 第 6 步：全量回归暴露的问题与修正

1. **`stop` 事务变成异步后，关机路径必须等**：`shutdownAll()` 是同步入口，现在只能
   “开始关”而无法等完，未等的实例按 `CloseFailed` 收尾（日志：kept context and DSO）。
   因此三处在释放容器前补 `co_await pluginHost->shutdownAsync()`：
   `main.cpp` 的 server 模式信号协程、`jsonl_mode.cpp` 的收尾、
   `session_runtime.cpp` 的 `runMain`（只在它自建容器时执行，用新增的 `ownsHost_` 判）。
   client 侧原有的 tui/cli/remote 模式本来就在 await，未改。
2. **加载快照新增 `plugin_executor` 段**（`assembly_snapshot.cpp` 的
   `buildAssemblySnapshot`）：`enabled` / `callback_budget_ms` / `callbacks` /
   `total_micros` / `max_micros` / `over_budget` / `dropped` / `unschedulable`；文本形态
   输出一行 `plugin_executor: plugin-thread callbacks=… avg=…us max=…us over_budget=…`。
3. **`boundaries` 规则 15（称呼与用词）** 拓出 26 处“宿主”与 2 处“落地”（都在框架头与
   新写的文档里），全改成“主程序 / 嵌入方程序 / 执行”。修正后 `boundaries` 20/0。
4. **`test-modules.md` 过期**：`plugin_executor` 模块登记了但文档未重生（143→144）。
   用 `AGENTXX_UPDATE_TEST_MODULES=1` 重生并提交。
5. `input_delivery` 在全量回归里报了 2 处失败（第 1184 / 1203 行，注入输入的分页），
   单独重跑 139/0 通过 —— 高负载下的时序抖动，与本任务改动无关（未改输入分页路径）。

阶段 E 收尾回归（新鲜 exe + `--log-level warn`，0 失败）：boundaries 20、assembly_snapshot 38、
test_modules 5、agent_host 190、session_runtime 55、shutdown_stages 17、plugin_executor 655、
plugin_agent_runtime 221；client 侧 `agentxx.exe` 与 JS 引擎插件均重新编译通过。

### 第 7 步：全量回归与 JS 端到端补充用例

全量 `agentxx_test --log-level warn`：**passed=43792 failed=0**（全 144 个模块）。

补一组“JS 脚本工具在插件线程开启时能真实调用”的用例（`plugin_executor` 第 10 组）：
前面第 9 组只验证了“能力行登记成功”，而脚本真正跑起来时是
`io → 插件线程 → JS 线程` 三跳，脚本里的表调用还会在 JS 线程上同步等完成（完成回调投回
插件线程）。本组用真实 DSO 验证：能力行登记、脚本异步注册 `js_hello` 工具、工具真实执行
（结果包含入参）、然后按依赖顺序 (脚本 → 引擎) 卸载。只剩两条可接受的 WARN：
`寄存器图节点类型` 与“provider is closed or disabled”（测试容器没有图注册表 /
卸顺序由 shutdownAsync 决定）。

最终计数：`plugin_executor` 663/0。
