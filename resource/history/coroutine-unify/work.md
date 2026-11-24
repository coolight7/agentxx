# 实施记录：插件协程体系收敛

## 进度

- [x] 现状调查（kit / 同步桥 / 使用面 / 编排硬约束）
- [x] 依赖关系梳理（发现"删同步等待"必须先做"工具体协程化"）
- [x] 文档：`index.md` / `plan.md` / 本文
- [x] 第 1 步：工具体协程化（`ToolBodyKind::Sync` 删除 + 44 个阻塞体改成 `co_await offload`）
- [x] 第 2a 步：框架加 **asio 异步操作形态的表调用** + kit 的 `*Async` 入口 + 用例
- [x] 第 2b 步：chatbot 业务层改 asio 协程（本轮执行）
- [x] 第 2c 步：测试探针的 `tableOpSyncWait` 调用点（29 处 / 7 个用例文件）
- [x] 第 3 步：删除 `tableOpSyncWait` / 同步桥 / `*Sync` 运行时封装（清单见 `plan.md` §8）
- [x] 第 4 步：表调用与任务体统一到 `asio::awaitable`（24 个插件全转换、取消槽、帧保活）
- [x] 第 5 步：删掉自制协程记账（`Task`/`PromiseBase`/`BridgeRoot`/`TableOpTicket`）与
      ticket 形态入口（2026-11-24 完成；2026-11-23 那次一次性删除已回滚，教训见末节）
  - [x] 第 1 步 (2026-11-24)：删 `agent_plugin_kit.h` / `plugin_kit.h` 的 ticket 消费者
        （`agent_plugin_kit.h` -1968 行、`plugin_kit.h` -203 行；`Task`/`TableOpTicket` 仍留在
        kit.h，靠编译暴露遗漏的调用点）；顺带把 `sessionMessagesCount` 改成 asio 形态
  - [x] 第 2 步 (2026-11-24)：改用例 —— `test_plugin_bridge` 删 §1~10 / §15~23（-978 行，
        保留 §11~14 受控轮询，4 处注册改本地 `registerAwaitableTool`）；`test_plugin_sdk`
        6 处 ticket 入口改 `*Async` + `runAwaitable`/`awaitTick`/`pumpUntil`（删掉 `Gate`
        与 `finishRoot`，伪调度器补 `sleep` + 到期计时器）；`test_plugins` 5 处 `tool`/`onWorker`
        改 `toolAsync`/`onWorkerAsync`、2 行按句柄退订删除
  - [x] 第 3 步 (2026-11-24)：删 kit.h 机制（`kit.h` 6156 → 4227 行，40 个整声明）；
        `PollOneBridge` 去掉根托管，`SpawnRecord` 只留取消相关字段
  - [x] 第 4 步 (2026-11-24)：`boundaries` 禁用名 + `AGENTS.md` + `plugins.md` 收尾
- [x] 第 6 步：`boundaries` 规则、`AGENTS.md` 与 `docs/zh-cn/design/plugins.md` 收尾
      （与第 5 步第 4 小步合并完成）

## 第 2c / 3 步开工前的现状（grep 结果，2026-11-22）

`tableOpSyncWait` / `*Sync` 的调用点现在只剩两类：

1. **kit 自己的封装**（这些就是第 3 步要改/删的）：`plugin_kit.h` 的 4 个广播同步入口与
   `using pluginxx::tableOpSyncWait`、`agent_plugin_kit.h` 的 `addShareStore` 同步形态 /
   `runtimeCall*` 系 / `checkPathDecisions` / 文档示例、`client_plugin_kit.h` 的 `clientTableOp`。
2. **测试探针**（第 2c 步）：`test_plugin_runtime`(6) `test_plugins`(8) `test_plugin_host_mount`(4)
   `test_plugin_resources`(3) `test_plugin_executor`(2) `test_plugin_agent_runtime`(1)
   `test_plugin_broadcast`(1)。它们分三种处境：
   - C ABI 生命周期钩子（`start`/`stop` 里就地注册，`allowOnPluginThread`）—— 要换成
     `tableOpCallback` + 全部完成后再 `notify->done(...)`（或 `finishLater` + `ctx.spawn`）；
   - `std::thread` 里的探针 —— 可用“提交 + 完成回调 + 回调里 notify”避开等待；
   - 主程序 IO 线程上的同步用例（现在靠同步桥就地回调）—— 要么改成协程里
     `co_await asyncTableOp`，要么加一个**测试专用**的“提交 + 完成回调 + 条件变量” helper
     （生产代码不留同步等待）。

插件源码（`agent/plugins/**`）已经没有 `*Sync` 调用点，因此第 3 步的阻力主要在 kit 自身与用例。


## 第 1 步：工具体协程化 (已完成)

### 框架侧 (`agent/lib/include/agentxx/plugin/api/agent_plugin_kit.h`)

| 改动 | 位置 | 说明 |
| --- | --- | --- |
| `ToolBodyKind` 改成 `{Task, Awaitable, Invalid}` | `detail` | 去掉 `Sync`, 同步体在 `tool()` 里 `static_assert` 报错 |
| `detail::invokeToolBody(...)` | `invokeToolBody` | 统一"按可调用形状试 10 种"的入口, `decltype(auto)` 转发返回类型; 协程体与阻塞体(经 `onWorker` 包装后)共用 |
| `detail::toolBodyKindOf<Fn,Ctx>()` | 同上 | 用 `invokeToolBody` 的推导结果判定 Task/Awaitable/Invalid |
| 入口 `execute_start` → `execute_start_invoke` | `registerToolWithTaskBody` | 只有 invoke 形态带 `work_dir`(见事实 4); 需要时回退 `execute_start` 会丢会话工作目录 |
| `Job` 增加 `workDir` / `PluginxxCancelToken token` | 同上 | `token` 由 `detail::taskJobTokenIsRequestedAbi<Job>`(读 `job->ctl.canceled()`) 提供, 供按令牌形状写的体查询取消 |
| `detail::runTaskToolBody` | 同上 | 只在体返回 `Task<ToolResult>` 时用: 拆出正文/状态, `Canceled` 抛 `CanceledException`、其它非 Ok 抛 `runtime_error`、Ok 只回正文 |
| `ToolBodyKind::Sync` 分支与 `registerToolWithSyncBody` 删除 | 同上 | 类型层面不再有"同步体"这回事 |
| `agentxx::plugin::onWorker(fn)` | `detail` 之后、`tool()` 之前 | 把阻塞函数包成 `Task<Result>`: 内部 `co_await pluginxx::offload(c, ...)`, 工作体在 worker 池上跑, 插件执行线程不被占住 |

### 插件侧 (17 个文件)

全部是"原 lambda 外面套一层 `agentxx::plugin::onWorker(...)`", 体内容未动:
`agentxx_audio_stream`(1) / `agentxx_chatbot`(5 inline + 5 命名 lambda: `sendMessage` `recall`
`task` `debugInject` `debugPlatform`) / `agentxx_codegraph`(5) / `agentxx_computer_use`(1) /
`agentxx_execute_command`(2) / `agentxx_filesystem`(10) / `agentxx_math`(1) /
`agentxx_planning`(1) / `agentxx_rag_search`(1) / `agentxx_screen_capture`(1) /
`agentxx_string`(2) / `agentxx_system`(1) / `agentxx_system_monitor`(1) /
`agentxx_text_selection_monitor`(1) / `agentxx_websearch`(2) / `example_multi_mount`(2) /
`example_plugin`(2)。

### 测试侧

- `test_plugin_sdk.cpp`: `spec.execute_start` 断言改成 `execute_start_invoke`, 调用时构造
  `AgentxxPluginToolInvoke`(带 `work_dir`)。
- `test_plugins.cpp`: 5 处阻塞体用 `onWorker` 包装。

### 验证

- 顶层 Debug 构建: 通过 (仅既有告警)。
- 插件全量: `build_plugins.ps1` 25/25 ok (96.8s)。
- 测试: `plugins` `plugin_sdk` `plugin_bridge` `plugin_runtime` `plugin_executor`
  `plugin_host_mount` `plugin_multi_mount` `plugin_cleanup` `plugin_point` `plugin_feature`
  `plugin_dispatch` `plugin_broadcast` `plugin_agent_runtime` `plugin_example_runtime`
  `plugin_example_chatbot` `plugin_resources` `plugin_multi_instance` `plugin_net`
  `client_plugins` `async_tools` `chatbot_task` → **passed=5148 failed=0**。

### 踩过的坑

- 包装脚本把命名空间写成 `pluginxx::onWorker`, 应为 `agentxx::plugin::onWorker`。
- 批量补收尾括号时要区分 `}` 与 `},`: 只按"同缩进的 `}`"匹配会串到下一个函数。
- 误包 4 处本来就返回 `Task` 的体 (`example_plugin` 214/234/263、`system_monitor` 264),
  已撤掉; 也误包过 `test_plugin_bridge.cpp` 里 4 处 `tableOp` start 回调, 已还原。
- `co_await` 一个返回 `Task<ToolResult>` 的体需要拆状态 (`runTaskToolBody`), 但
  `Task<std::string>` 体(如 `example_sleep`/`example_caller`)**不能**经包装层 —— 包装会让
  `execute_cancel` 的 `cancel_outstanding()` 落在外层帧上, 取消传不到业务协程挂起的原语,
  表现为卸载/取消超时。现已按 `if constexpr` 二选一, 两型各走直达/包装。

## 第 2a 步：asio 异步操作形态的表调用 (已完成)

### 为什么加这一层

第 2 步拆开看才发现: `*Sync` 封装的实际调用方分布在**两类非协程代码**里 ——
插件自有 io 线程上的业务层 (chatbot 的调度/任务/命令) 与测试探针。而插件自有
`io_context` 上的协程只能是 `asio::awaitable`, 它**只认 `asio::awaitable` 与 asio 异步操作
两类可等待对象** (`agent/third_party/boost/.../impl/awaitable.hpp:177/186`), 既拿不到
kit 的 `Task`, 也拿不到 `TableOpTicket` (它的 awaiter 要写协程帧的 promise)。
所以先把"表调用"包成标准 asio 异步操作, 插件自有线程才有东西可 `co_await`。

### 框架侧 (`agent/third_party/cxx_pluginxx/include/pluginxx/kit/kit.h`)

| 新增 | 说明 |
| --- | --- |
| `detail::AsyncTableOpState<Handler, Executor>` | 处理器 + 执行器 + 一次性标记 (`std::atomic`, 保证处理器只调一次) |
| `detail::finishAsyncTableOp` | 完成时 **只 `asio::post(ex, ...)`**: 完成回调可能来自主程序 IO 线程/插件执行线程甚至 `start` 内部就地完成, 处理器恢复的是调用方执行器上的协程, 不能就地恢复 |
| `detail::AsyncTableOpInitiation<Executor, StartFn>` | 调用处即提交; `start` 返回 NULL = 未受理, 自己给 `startError` |
| `detail::AsyncTableOp<Executor, StartFn>` | 实现 asio 异步操作协议 |
| `pluginxx::asyncTableOp(ctx, ex, start)` | 公开入口, 返回上面那个操作对象 |

**踩过的两个坑** (写在这里免得以后重犯):

1. `asio::async_initiate<CompletionToken, Signatures...>` 的显式模板参数顺序在不同 Boost
   版本里不一致 (这个版本里有一份重载把 `Signatures...` 放在最前), 传参会被解释成两个
   签名, 编译报"找不到匹配的 `async_initiate`"。现在**不用它**: 直接实现协议需要的两件事
   —— `op(completion_signature_probe)` 交回 `void(TableOpResult)` 签名,
   `op(handler)` 直接发起。
2. `asio::awaitable` 的 `await_transform` 还有一份"awaitable 函数"重载, 它对
   `result_of<Function(awaitable_frame_base<Executor>*)>` 做 SFINAE。如果 `operator()` 是
   无约束的模板, 帧指针会被当成完成处理器传进来 (编译报"调用需要 1 个参数的函数不存在")。
   现在用 `requires` 限定: 只接受两个 probe 类型与**能用 `TableOpResult` 调用**的处理器。

### kit 侧 (`agent/lib/include/agentxx/plugin/api/agent_plugin_kit.h`)

新增一节 "asio 异步操作形态 (插件自有 io 线程用)", 全部返回 `asio::awaitable`
(执行器 = `asio::any_io_executor`, 与调用方协程一致才能 `co_await`):
`tableOpAsync` / `openChannelAsync` / `closeChannelAsync` / `sendInputAsync` /
`cancelChannelAsync` / `respondInterruptAsync` / `channelStateJsonAsync` /
`listRuntimeJsonAsync` / `submitAsyncWorkAsync` / `cancelAsyncWorkAsync` /
`getAsyncWorkJsonAsync`; 未受理时统一给 `detail::tableOpUnsupported()` (即
`accepted() == false` + `startError` 有文本), 与凭据形态的语义一致。
`plugin_kit.h` 补了 `asio/this_coro.hpp` (取协程自身执行器)。

语义差别 (实测确认): 这些入口本身是协程, **提交发生在开始 `co_await` 那一刻**,
不是调用那一行 (凭据形态则是调用即提交)。

### 用例 (`agent/test/plugin/test_plugin_sdk.cpp`)

- `compileCheckAsyncTableHelpers`: 只取地址, 让全部 `*Async` 入口实例化 (类型/参数形状/
  `co_await` 兼容性在编译期检查);
- `runAsyncTableOpCase`: 伪 runtime 表的 `send_input` 在**另一条线程**、延迟 20ms 完成,
  验证 ① 完成经 post 回到 io 线程 (协程恢复线程 == io 线程, 且 != 回调线程);
  ② 结果正文送达; ③ 表里没有的成员给 `accepted() == false` + `startError`。
- 验证: `plugin_sdk` 150 通过 / 0 失败; 同批 `plugin_bridge` `plugin_runtime`
  `plugin_executor` `plugin_host_mount` `plugins` 全绿。
## 第 2b 步：chatbot 业务层改 asio 协程 (已完成)

目标：把这个插件里 11 处 `*Sync` 表调用全部换成 `co_await`，它是 2c/3 步的前置
（`*Sync` 的删掉必须先让真实使用面清零）。

### 改了哪些文件

| 文件 | 改动 |
| --- | --- |
| `chatbot_router.h/.cpp` | `Hooks::open` / `acquire` → `asio::awaitable`（开通道是表调用）|
| `chatbot_engine.h/.cpp` | `Runtime` 四个入口改协程形态；`onInbound` / `flush` / `handleCommand` → `awaitable`；新增私有 `spawn(awaitable<void>)` 与 `sendTaskResultToChannel` |
| `chatbot_task.h/.cpp` | `TaskRuntime{submit,cancel}` → `awaitable`；`submit` / `cancel` → `awaitable`；`onEvent` 不再调表，新增 `setResultHead` |
| `chatbot_tools.h/.cpp` | `toolTask` / `toolDebugInject` → `awaitable<std::string>` |
| `agentxx_chatbot.cpp` | 新增 `runIoCoroutineBody` / `ChatbotCtx::runOnIoCoroutine` / `spawnIo` / `detachedLog`；9 个工具内层体改 awaitable；`Runtime` 接线改 `*Async`；`start`/`stop` 主体改插件 `Task`（`mountRoot`/`unmountRoot` 改 `co_await`）|

### 关键约定（后续步骤直接沿用）

1. **协程参数一律按值**：`asio::awaitable` 的帧只在参数上做复制，引用/视图保留下来；
   而“发起后不立即 `co_await`”的写法到处都是（`spawn` / `spawnIo` / `co_spawn`）。
   本轮实测到三处悬空：
   - `spawn(flush(chatKey, ...))`：`chatKey` 是普通函数/定时器回调的局部量，
     协程挂起后发起方就返回了；
   - `spawnIo(engine.onInbound(event))`：`event` 是平台回调的 `const&`；
   - `respondInterruptAsync(ctx, id, iid, values)`：`values` 来自 `values.dump()` 临时量。
   ASan 报 `stack-buffer-overflow`（parseChatKey 读 27 字节）就是这个。
   现在这些入口的参数都是 `std::string`（或整个结构体按值），调用方直接传临时量也安全。
2. **返回 void 的入口仍是同步函数**：实现方内部自己 `spawnIo` 投递（`cancel` /
   `respondInterrupt` / `closeChannel`），调用方不等待。
3. **`std::string` 参数不能隐式吃 `std::string_view`**（`string` 的视图构造是
   `explicit`），调用点要写 `std::string{view}`。
4. 协程体不要写内联 lambda 调用（帧只存闭包指针）：插件侧统一用
   `runIoCoroutineBody(state, shared_ptr<Fn>)`，共用代码用普通函数包（`detachedLog`）。

### 流程形状（一层不能少）

```
工具调用 (插件执行线程) → onWorker(阻塞体) → 工作池线程
  → runToolOnIo → runOnIoCoroutine (cv 等) → 插件 io 线程上的 awaitable
      → co_await sendInputAsync / openChannelAsync / ... → 主程序表 → 回到 io 线程
```

### 测试侧改动

- `test_chatbot_task.cpp`：加 `runInto` / `runAwaitable`（本地 `io_context` 跑完取值）、
  `submitOf` / `cancelOf`；假运行时的 `submit`/`cancel` 改协程；`resultOf` 删掉，
  改用 `setResultHead` 补结果正文。
- `test_chatbot_core.cpp`：加 `runInto` / `runAwaitable` / `acquireOf`；`hooks.open` 改 awaitable。
- `test_plugin_sdk.cpp`：`cancelAsyncWorkAsync(ctx, std::string{...})`（参数改按值后要显式转换）。

### 验证

- `build_plugins.ps1 agentxx_chatbot` ok；全部 25 个插件 `25/25 ok`（我改了 `*Async` 参数的
  按值形式，其他插件也引用了这些入口，必须全部重编才能确认）; 顶层 Debug 构建 ok。
- `chatbot_task` 138 / `chatbot_core` 874 / `chatbot_qq` 136 / `chatbot_media` 69 / `plugin_sdk` 150
  全绿；`plugin_example_chatbot` 266 全绿。
- 插件相关 24 个模块整批跑：**passed=6236 failed=0**（包含 `plugin_runtime` 774 / `plugins` 706 /
  `plugin_executor` 684 / `client_plugins` 842）；`boundaries` 20 通过（新增入口不破分层规则）。

### 待查：`plugin_example_chatbot` 的单次工具调用要 1~4 秒

临时埋点（测试侧的 `[SLOW]`、插件侧的 `io 协程慢`）结论：
- 插件 io 侧协程从不超 200ms（无任何 `io 协程慢` 日志）；
- 慢在工具调用到达工具体之前，即主程序的插件派发 / 工作池排队；
- **不是本轮引入的**：改造前的基线日志（`r3_plugin_example_chatbot.log`，158s / 265 通过）
  与改造后（161s / 266 通过，连跑两次）一致；模块内 30+ 个 `waitUntil` 都是秒级轮询，
  整体耗时本就 2~3 分钟，机器忙时还会波动到 300s（基线日志 `r2` 在负载下曾 34 个失败）。
埋点已全部删除。

## 第 2c 步：测试探针去掉同步等待 (已完成)

### 新增

`agent/test/include/agentxx-test/plugin/test_table_op.h`（只给测试目标）:
- `tableOpWait(host, cancel, scheduler, start, timeout=15s)` / `tableOpWait(ctx, start, ...)`:
  `tableOpCallback` + 条件变量，同步取结果；等超时返回 `accepted()==false` 且
  `startError` 写明“测试等超时”（与“主程序没收”分开）；
- `tableOpSubmit(host, cancel, scheduler, start, done)`: 只提交，完成时回调。

### 改了哪些用例

| 文件 | 改动 |
| --- | --- |
| `test_plugin_runtime.cpp` | create / stop 回调改成“提交 + 全部完成后回答 notify”（notify 按值存、失败原因走 payload）；只用一次往返的 3 处（`argsOf` / `std::thread` 里的注册 / `broadcastOp`）改 `tableOpWait` |
| `test_plugins.cpp` (8) `test_plugin_host_mount.cpp` (4) `test_plugin_resources.cpp` (3) | 换成 `tableOpWait`；原本传 `5000` 超时的位置改 `std::chrono::milliseconds{5000}` |
| `test_plugin_agent_runtime.cpp` (1) `test_plugin_broadcast.cpp` (1) | `callRuntime` / `broadcastOp` 换成 `tableOpWait` |
| `test_plugin_executor.cpp` (2) | 删掉“同步桥自身”的用例（它测的就是要删的机制）与“插件线程上拒绝同步等待”的用例；“完成回调落在插件线程”的覆盖由 `test_operation_on_plugin_thread` 已经在管 |

### 碰到的两个坑

- 等超时参数原来是裸 `5000`（`tableOpSyncWait` 的 `int64_t timeoutMs`），换成
  `std::chrono::milliseconds` 后所有裸数字都要显式包一层（`milliseconds` 的整型构造是 `explicit`）。
- `boundaries` 规则 15 禁“钩子”这个词（用“回调 / 通知 / 接入点”），我第一版注释里用了，
  已改。

### 验证

- 顶层 Debug 构建 ok；
- `boundaries` 20 / `plugin_runtime` 774 / `plugins` 706 / `plugin_sdk` 150 /
  `plugin_bridge` 302 / `plugin_executor` 663 / `plugin_host_mount` 148 /
  `plugin_resources` 102 / `plugin_agent_runtime` 222 / `plugin_broadcast` 102 /
  `client_plugins` 842 / `plugin_example_chatbot` 266 / `chatbot_*` …
  整批 25 个模块：**passed=6235 failed=0**。

## 第 3 步 Phase A：使用面先改成协程（已完成，2026-11-23）

思路：先给每个"同步等表调用"的位置准备异步替代并改过去（保持可构建、可测），
最后一次性删掉 `tableOpSyncWait` / `*Sync` / 同步桥（Phase C）。

### 已完成

| 位置 | 改动 |
| --- | --- |
| `agentxx_execute_command` | `StoreFn` 改成 `std::function<asio::awaitable<long long>(std::string_view)>`;`makeTimeoutResult` / `runProcPipeline` 的写 store 点改 `co_await`;插件注册处用新的 `addShareStoreAsync`。**popen 回退**分支（`AGENTXX_ENABLE_BOOST_PROCESS=OFF`）去掉 `StoreFn`:阻塞体做不了表调用, 该路径超限时只截断、提示不带条目 id（已写在注释里）,测试里对应的 `/*storeFn=*/nullptr` 参数同步删掉 |
| `agent_plugin_kit.h` | 新增 `addShareStoreAsync`（会话共享存储 write 的 asio 形态）与 `offloadAsync`（阻塞工作交给工作池的 asio 形态;工作体可带 `const PluginxxCancelToken*` 或不带参数）;另加 `checkPathDecisionsAsync` / `filterPathPermissionsAsync`（放在同步形态之后, 因为它们依赖 `PathDecision`） |
| `cxx_pluginxx` `kit.h` | 新增 `pluginxx::asyncOffload(ctx, ex, work)`（asio 异步操作形态的 offload）+ `detail::AsyncOffloadState` / `OffloadOutcome` / `AsyncOffload`。**关键**：`asio::awaitable` 的 `await_transform` 只认 `awaitable` / asio 异步操作 / `this_coro` 标签，自定义 awaiter 不能用（`error C2665`），所以只能是 asio 异步操作 + 结果结构（完成处理器里不抛异常） |
| `agentxx_filesystem` | `list` / `glob` / `grep` / `move` / `copy` / `read_media` 六个体从 `onWorker` 改成 `asio::awaitable`:阻塞段经 `offloadAsync` 交给工作池, 权限查询在协程里 `co_await`;`makeReadPathFilter` 换成 **两段式**（`PathRecorder` 先记候选路径并全部判拒绝 → 查权限 → 用结果当过滤器跑第二遍）,`destinationAllowed` 换成协程 `destinationProblem` |

### 两段式的取舍（重要）

阻塞体不能 `co_await` 表调用，而 `list/get` 的权限复核**发生在枚举过程中**（拿到路径才知道
要查什么）。两条路：

1. 把三个执行体拆成“收集 / 格式化”两段（改动大，`list` 的 `limit` 早停语义会变）；
2. 让体跑两遍：第一遍用“记录 + 全拒绝”的过滤器（体在权限过滤处就结束，不读文件），
   第二遍带查询结果（`PathRecorder` + `makeDecidedPathFilter`）。

选 2：执行体一行未动，消息文本/上限语义完全不变；代价是模式类工具多一次**枚举**
（`grep` 不会多读文件内容── 第一遍在候选过滤处就返回）。候选超过 8192 条时抛
`CollectEnoughPaths` 中止第一遍并退回“不做逐项复核”（与“查询不可用”同一种降级）。

### 顺带修的回归

`read_media` 之前是 `onWorker`（Task 体），而 Task 体只上报正文（`textOnlyNotify`），
多模态附件会丢 —— `filesystem` 用例里 `res.attachments.size()` 得到 0。改成 asio 体 +
`offloadAsync` 后附件正常（该用例恢复通过）。**Task 型工具不能返回附件**，这条要写进
`plan.md`/文档。

### 验证

- 顶层 Debug 构建 ok；`agentxx_execute_command` / `agentxx_filesystem` 插件编译 ok；
- `command` 72 / `filesystem` 285 全绿（含中文路径、media 附件、move/copy 权限复核）。

## 第 3 步 Phase B / C：client 侧去同步 + 删同步机制（已完成，2026-11-23）

### Phase B：client kit 去同步

- `client_plugin_kit.h` 删 `clientTableOp` / `clientTableText` / `clientTableHandle`;
  保留 `clientTableCall`（提交即返回）、`clientTableOpAsync`、`clientHandleOf`、`tableStatus`;
  新增协程版 `clientRegisterStatusItemAsync` / `clientRegisterPanelAsync` /
  `clientRegisterInfoSectionAsync` / `clientRegisterCommandAsync`;
  `ClientPluginBase` 的 `clientState` / `argsJson` / `configPath` / `language` / `setLanguage` /
  `sendUserInput` / `regionSize` / `isRegionVisible` / `keybindListJson` / `registerTimer` /
  `registerKeybind` 全部改成 `*Async`（`registerTimerAsync` / `registerKeybindAsync` 返回
  `pluginxx::Task<std::shared_ptr<...Handle>>`）；`hostSupports` / `uiCapabilities` /
  `supportsBlock` / `kitEnv` 改成读缓存快照（从表拉一次存起来，不再每次调表）。

### Phase C：删掉同步机制

| 位置 | 删了什么 |
| --- | --- |
| `cxx_pluginxx` `kit/kit.h` | `tableOpSyncWait`（ctx / host 两种形态）、`detail::tableOpText` / `tableOpOk`、`kMainSideWaitPolicy`；`PluginBaseT` 的 `config()` / `argsJson()` / `configPath()` / `language()` / `setLanguage()` 改 `*Async()` |
| `cxx_pluginxx` `api/sync_bridge.h` | 整个文件删除（固定标记 + 唤醒体 + ud） |
| `cxx_pluginxx` `runtime/op_driver.h` | `mayBeSyncBridge` / `isSyncBridgeCallback` / 就地回调分支；`setCallback(cb, ud, mayBeBridge)` 去掉第三参数 |
| `cxx_pluginxx` `host/host_core.h` `.host/table_op.h` | 三处 `setCallback(cb, ud, true)` 收成两参 |
| `agentxx` `plugin_kit.h` | `using pluginxx::tableOpSyncWait;`、4 个 host 形态广播入口（`declarePluginBroadcast(host,…)` / `publishPluginBroadcast(host,…)` / `subscribePluginBroadcast(host,…)` / `unsubscribePluginBroadcast(host,…)`） |
| `agentxx` `agent_plugin_kit.h` | `addShareStore` 同步形态、`detail::runtimeCall` / `runtimeCallText` / `runtimeCallId` / `takeRuntimeText`、21 个运行时 `*Sync`、4 个后台任务 `*Sync`、`checkPathDecisions` / `checkPathDecision` / `filterPathPermissions` 同步形态、`registerToolWithSyncBody` 与 `ToolBodyKind::Sync` 分支 |
| 主程序 | `client_plugin_manager.cpp` / `plugin_manager_vtable.cpp` 的 `setCallback(..., true)` 改两参 |
| 测试 | `subscribePluginBroadcast` → `test_table_op.h` 新增的 `subscribeBroadcastWait`（枚举句柄只能经完成回交回）；`boundaries` 规则 16 改成扫已删除名字拦回退 |

### 踩到的坑（关键）

删同步代码时**按行范围删过头**，把三类并非同步的东西一并删了（编译时报
`TableOpTicket` / `PluginString` / `RuntimeChannelShim` 未声明才发现）：

1. `CallToolAwaiter::await_resume()` 尾部 + `};` + `} // namespace detail`，连同
   `call_tool` / `call_tool_result` 两个自由函数与“功能埋点”段（`PointImplOptions` /
   `pointAnswer` / `PointImplAdapter`）；
2. 会话运行时的**协程形态**整套（`unmountRoot` / `openChannel` / `closeChannel` /
   `sendInput` / `sendText` / `cancelChannel` / `selectChannelModel` / `respondInterrupt` /
   `channelStateJson` / `channelContextMessages` / `listRuntimeJson` / `addTrigger` /
   `removeTrigger` / `emitEvent` / `addRuntimeTimer` / `removeRuntimeTimer`）；
3. 后台任务的 `cancelAsyncWork` / `getAsyncWorkJson`。

恢复方式：从 `git show HEAD:` 抽出这些段落，用一个脚本按“`*Sync` 函数整块剔除”过滤
（花括号配对，含它上面的 `///` 文档与 `template<` 行），再拼回文件。两个教训：

- 删除类改动做完后**先跑一遍括号平衡扫描**（按行扫 `{`/`}` 看末深度，本头应为 0），
  再构建；本次就是靠它定到出问题的区域；
- 拼回大段代码别用 `WriteAllLines`（写 CRLF），要 `WriteAllText` + 自行拼 `"`n"`。

另一处标题也挤错了位：`/* ==================== asio 异步操作形态 … */` 被插到了
文件头“工具结果状态”注释中间，导致它后面一段 ` * …` 落在注释外（编译报
`C2018` 字符 `U+60`）。按段落头语义把两段标题各归位即可。

### `subscribePluginBroadcastNoWait` 为什么返回 bool

v4 起订阅句柄只经完成回调的 `payload` 交回（`pluginxxSubscriptionFromText`），
提交即返回的 C 调用拿不到句柄，所以这个助手只能报“受理了没”。需要句柄时要么
`co_await subscribePluginBroadcast(ctx, …)` + `subscriptionOf`，要么（仅测试）用
`test_table_op.h` 的 `subscribeBroadcastWait`。

### 验证（本步完成时）

- 顶层 Debug 构建 `EXIT=0`；25 个插件全部重新链接 ok；
- 三批用例共 **8129 条全绿**：
  - `command` / `filesystem` / `plugins` / `plugin_sdk` / `plugin_bridge` / `plugin_runtime` / `plugin_executor`（2953）
  - `plugin_host_mount` / `plugin_multi_mount` / `plugin_cleanup` / `plugin_point` / `plugin_feature` / `plugin_dispatch` / `plugin_broadcast` / `plugin_agent_runtime` / `plugin_example_runtime` / `plugin_resources` / `plugin_multi_instance` / `plugin_net`（1117）
  - `client_plugins` / `async_tools` / `chatbot_*` / `boundaries` / `task_scope` / `string_util` / `ui_*` / `tui_*` / `event_*`（4059）

## 第 4 步开工前的现场盘点（2026-11-23，供下次直接开工）

第 4 步 = 让 `Task` 变成 `asio::awaitable` 的薄别名 → 插件源码改名 → 删
`PromiseBase` / `BridgeRoot` / `PollOneBridge` / `TableOpTicket`。开工前先把“已经有的
asio 形态”和“还缺的”列清楚：

### 已经就绪的 asio 形态

| 类别 | 入口 |
| --- | --- |
| 通用表调用 | `pluginxx::asyncTableOp(ctx, ex, start)`；agent kit 的 `tableOpAsync(ctx, start)` |
| 工作池 | `pluginxx::asyncOffload(ctx, ex, work)`；agent kit 的 `offloadAsync(ctx, work)` |
| 会话运行时 | `mountRootAsync` / `unmountRootAsync` / `openChannelAsync` / `closeChannelAsync` / `sendInputAsync` / `sendTextAsync` / `cancelChannelAsync` / `respondInterruptAsync` / `channelStateJsonAsync` / `channelContextMessagesAsync` / `listRuntimeJsonAsync` / `addTriggerAsync` / `removeTriggerAsync` / `addRuntimeTimerAsync` / `removeRuntimeTimerAsync` / `emitEventAsync` |
| 后台任务 | `submitAsyncWorkAsync` / `cancelAsyncWorkAsync` / `getAsyncWorkJsonAsync` / `listAsyncWorkJsonAsync` |
| 路径权限 | `checkPathDecisionsAsync` / `filterPathPermissionsAsync` |
| 共享存储 | `addShareStoreAsync` |

### 还缺的（第 4a 步要补的 kit 入口）

- `sleep` 目前只有 ticket 形态（`detail::SleepTicket`），asio 协程里无法 `co_await`；
  需要 asio 形态（用 `scheduler.sleep` 包成 async operation，不能再新开定时器 ——
  本地 reactor 不自己流动，见 `plan.md` §9 的事实 3）；
- `call_tool` / `call_tool_result`（`detail::CallToolAwaiter`）与 `invoke_cap`
  （`detail::InvokeCapAwaiter`）、`toolPromptOf` / `schemaOf`（`ToolPromptAwaiter` /
  `ToolSchemaAwaiter`）都是**自定义 awaiter**，而 `asio::awaitable` 只认
  `awaitable` / asio 异步操作（实测 `error C2665`），必须改成 async operation；
- 注册类：`tool` / `capability` / `providePoint` / `registerToolPermission` /
  `graph_node` / `subscribePluginBroadcast` 返回 `TableOpTicket`。通用兜底已有
  `tableOpAsync(ctx, start)`，但逐个包成 `*Async` 更省事（参数按值带进协程）；
- `ctx.spawn(...)` / `finishLater(...)` 的替代：`co_spawn(bridge_io, ...)` + 现有
  `register_task` lease；取消用 `asio::cancellation_signal`（`asymmetric` 通道）
  替掉 `Task::promise_type::set_outstanding`；
- 执行上下文：`PollOneBridge` 已经持有本地 `io_context`（由主程序 driver 分片 `poll_one`），
  直接把它当 `awaitable` 的执行器就能保住“插件协程只在主程序 io 线程的驱动片里跑”
  这一现有语义，不必给插件另起一条 io 线程。

### 迁移切片（每一片都可单独验证）

1. 补上面“还缺的” kit 入口（纯新增，不删不改）；
2. kit 里让 `Task` 变成 `asio::awaitable` 薄别名，`spawn` / `finishLater` 改成
   `co_spawn(bridge_io, …)`，24 个插件的 `Task<...>` 返回类型不改名；
3. 全绿后把 90 处 `Task<...>` 改成 `asio::awaitable<...>`（纯改名）；
4. 删 `PromiseBase` / `BridgeRoot` / `PollOneBridge` / `TableOpTicket` 与
   `boundaries` 里的对应规则。

## 第 3 / 4 步：现状与开工清单

见 `plan.md` §8（第 3 步逐步清单）与 §9（第 4 步已查清的事实与迁移顺序）。
本轮到此为止：第 3 步一旦开始就是“框架 + 两个 kit + 两个插件 + 用例”的连锁删除，
不做完会让整棵树的构建失败，所以留到下一轮整块做完。

## 本轮做过又回滚的事（留档，避免重复踩）

本轮按"先删同步等待"的顺序动手，改了以下文件后**全部回滚**（工作区保持可构建）：

- `cxx_pluginxx`：`kit.h`（删 `tableOpSyncWait` 与 `detail::tableOpText` / `tableOpOk`，
  把 `PluginBaseT` 的读取助手改成协程）、`api/sync_bridge.h`（删）、`runtime/op_driver.h`
  （去掉 `mayBeSyncBridge` 与就地回调分支）、`host/host_core.h`、`host/table_op.h`、
  `api/tables.h` / `runtime/runtime.h` / `runtime/manager_base.h`（`is_io_thread` 注释）。
- `agentxx`：`plugin_kit.h`（删 4 个阻塞广播入口与 `broadcastCallIfaces`）、
  `agent_plugin_kit.h`（删 `runtimeCall*` 与 20 个 `*Sync` 封装、`addShareStore` 同步形态）。

回滚原因：这些封装的实际调用方都在**非协程**代码里（chatbot 平台回调层 20 处、
filesystem 等 44 个同步工具体、测试探针），删掉它们要么编译不过，要么把
"插件执行线程上阻塞等表结果"变成死锁。结论见 `index.md` 的"关键结论"。

## 本轮确认的事实（后续步骤直接可用）

1. `asio::awaitable::await_suspend` 只接受 `awaitable_frame` 的 handle
   （`agent/third_party/boost/libs/asio/include/boost/asio/awaitable.hpp:112`）——
   kit 的 `Task` 体无法 `co_await` `asio::awaitable`，收敛方向只能是 kit → asio。
2. `asio::awaitable` 只 `await_transform` 两类：`awaitable<T, Executor>` 与
   `is_async_operation`（`impl/awaitable.hpp:177/186`）；`is_async_operation` 判定见
   `async_result.hpp:846`（`T(Args..., token)` 能经 `async_initiate` 展开）。
   所以表调用要能 `co_await`，必须做成 async operation，而不是普通 awaiter。
3. 本地 reactor 不会自己流动：`poll_one` 只跑已就绪 handler，本地 `steady_timer` 到期
   没有人 `wake`。`sleep` 必须继续走 `scheduler.sleep`；asio 原生等待必须靠 pump 续票。
4. 同步工具体现在通过 `execute_start_invoke` 拿到 `work_dir` 并在
   `scheduler.offload` 上跑；协程体入口（`execute_start`）**没有** `work_dir`。
   工具体协程化时要把协程体入口也切到 `execute_start_invoke`，否则会话工作目录丢失。
5. `agentxx_javascript_engine` 里的 `readBroadcast` / `endBroadcast` / `addShareStore`
   是 JS 绑定名（`def("readBroadcast", …)`），不是 C++ 的 kit 调用 —— 迁移时不要误改。


## 第 4 步 第 1 批：asio 形态补齐 + `spawn` 收 asio 体 + `agentxx_math` 全转换（已完成并验证）

### 新增的 asio 形态入口（纯新增，旧 ticket 形态不变）

`cxx_pluginxx/include/pluginxx/kit/kit.h`
- `sleepAsync(ctx, ms)`：`asio::awaitable<void>`，走主程序 `scheduler.sleep`
  （包成 `asyncTableOp`，失败/取消抛异常，与 `sleep` 一致）。

`agent/lib/include/agentxx/plugin/api/agent_plugin_kit.h`
- `detail::tableOpAwaitAsync(ctx, start)`：取当前协程执行器后提交表调用（asio 形态公共件）。
- 业务表：`promptJsonAsync` / `setPromptJsonAsync` / `modelConfigJsonAsync` / `graphJsonAsync` /
  `graphNameAsync` / `setGraphJsonAsync` / `ownResourcesJsonAsync` /
  `registerSkillDirAsync` / `unregisterSkillDirAsync` / `registerMemoryFileAsync` /
  `unregisterMemoryFileAsync` / `registerMcpServerAsync` / `unregisterMcpServerAsync` /
  `listMountsAsync` / `mountInfoOfAsync`。
- 提示词与模式：`toolPromptOfAsync` / `schemaOfAsync`（直接给结果类型，不是 ticket）。
- 广播：`declarePluginBroadcastAsync` / `publishPluginBroadcastAsync`（先补声明）/
  `subscribePluginBroadcastAsync` / `unsubscribePluginBroadcastAsync`。
- 工具权限：`registerToolPermissionAsync` / `registerReadPathPermissionAsync` /
  `registerWritePathPermissionAsync`。
- 工具注册：把 `registerToolWith{Task,Awaitable}Body` 拆成
  `buildToolSpecWith{Task,Awaitable}Body`（只造 spec） + 两个薄消费者
  （`registerToolTicket` / 新的 `registerToolAwaitable`），于是有了
  `registerToolWithTaskBodyAsync` / `registerToolWithAwaitableBodyAsync` 与新入口
  `toolAsync(...)`（参数与 `tool` 完全一致）。
- `onWorkerAsync(fn)`：与 `onWorker` 对应，产出 `asio::awaitable` 体，内部 `co_await offloadAsync`。
- `offloadAsync` 的结果类型改成惰性 trait（`OffloadAsyncTraits`）：原来签名里写死
  `invoke_result_t<WorkFn&>`，收取消令牌的工作体直接编译失败（两条 `invoke_result` 不能都实例化）。

`cxx_pluginxx/include/pluginxx/kit/kit.h`（`spawn`）
- 成员 `PluginBaseT::spawn` 与自由函数 `spawn` 都按任务体返回类型分派：
  `asio::awaitable<T>` 走新的 `detail::spawnAwaitableImpl`，其余走原 `spawnTaskImpl`。
- `spawnAwaitableImpl` 用 `asio::co_spawn(桥的本地执行器, ...)`，完成处理器上报一次通知；
  取消只置 `OpCtl` 的标记（`asio::awaitable` 的挂起不接 kit 的取消）。

### 两个踩坑（后续转换必须按这个来）

1. **asio 协程的首步没人推进**：本地执行器只在主程序 driver 请求时被 poll。`co_spawn`
   只把首步 post 进去，不 wake → 首步永远不跑（现象：`math_tools` 启动即挂住，30 分钟不退出）。
   修法：`spawnAwaitableImpl` 在 `co_spawn` 之前 `addPolledRoot(...)`（让桥在任务存活期间持续
   申请驱动，≤10 ms 一次）、之后显式 `bridge().wake()`；完成处理器里 `removePolledRoot`。
   同理，任何"把协程排进桥的本地执行器"的新路径都要自己 wake。
2. **`PluginBaseT::spawn` 是成员函数**：只改自由函数 `spawn` 不生效（插件写的是 `ctx.spawn(...)`），
   两处都要分派。

### `agentxx_math` 的转换（第一批样板）

- `mathSetupAsync`：`pluginxx::Task<void>` → `asio::awaitable<void>`；
  `schemaOf` → `schemaOfAsync`；`tool` → `toolAsync`；体从 `onWorker(...)` 换成
  `onWorkerAsync(...)`（工具体因此是 awaitable，注册走 `registerToolWithAwaitableBodyAsync`）。
- `mathMount` 的 `ctx.spawn` 体返回类型改 `asio::awaitable<void>`。

### 验证

- 顶层 Debug 构建 `EXIT=0`。
- `math_tools` 82 / `agent_host` 190 / `plugins` 706 / `plugin_runtime` 774 / `plugin_sdk` 151 /
  `plugin_executor` 663 / `plugin_host_mount` 148 / `client_plugins` 842 / `boundaries` 20 全绿；
  合并批次共 **3584 条 failed=0**（含 `async_tools` / `event_subagent` / `chatbot_task` /
  `plugin_agent_runtime` / `plugin_broadcast` / `plugin_point` / `plugin_feature`）。
- 注意：`client_plugins` 在高负载合并跑时出现过 2 条 toast 断言超时，单跑必过（时序抖动）。

### 剩余（第 4 步后续批次）

- 其余 23 个插件按 `agentxx_math` 的样板转换（每个先补它用到的 `*Async` 形态）：
  `agentxx_string` / `agentxx_filesystem` / `agentxx_execute_command` 已有 asio 工具体，
  只差 setup 协程；`example_plugin` 还用到 `call_tool`、`sleep`、`tableOp`、广播订阅，
  需要 `callToolAsync`（下一个要补的入口）。
- 全部转完后才能删 `Task` / `PromiseBase` / `BridgeRoot` / `TableOpTicket`，
  并把 `tool` / `whenAll` 等 ticket 形态一起下掉（同时改测试与文档）。

## 第 4 步：任务体与表调用统一到 asio（已完成）

### 转换范围

- **24 个插件**（`agentxx_audio_stream` 本平台不编译）的任务体全部
  `pluginxx::Task<T>` → `asio::awaitable<T>`：`ctx.spawn` 体、`onHostReady` 体、工具体、
  能力体、图节点体、客户端命令/定时器回调里的协程都换完。
- kit 补的 asio 孪生入口（命名统一 `*Async`，插件按需使用）：
  `mountRootAsync` / `unmountRootAsync` / `sendInputAsync` / `sendTextAsync` /
  `cancelChannelAsync` / `selectChannelModelAsync` / `respondInterruptAsync` /
  `openChannelAsync` / `closeChannelAsync` / `channelStateJsonAsync` /
  `channelContextMessagesAsync` / `listRuntimeJsonAsync` / `addTriggerAsync` /
  `removeTriggerAsync` / `emitEventAsync` / `addRuntimeTimerAsync` /
  `removeRuntimeTimerAsync` / `submitAsyncWorkAsync` / `cancelAsyncWorkAsync` /
  `getAsyncWorkJsonAsync` / `listAsyncWorkJsonAsync` / `callToolAsync` /
  `callToolResultAsync` / `graphNodeAsync` / `capabilityAsync` / `providePointAsync` /
  `addShareStoreAsync` / `workDirAsync` / `getSessionMessagesAsync` /
  `subscribePluginBroadcastAsync` / `subscribePluginBroadcastExAsync`。

### 取消与帧保活（三个必须遵守的约束）

1. **取消信号必须由协程帧持有**。`co_spawn` 的完成令牌用
   `asio::bind_cancellation_slot(signal->slot(), handler)` 之后，asio 会在**内层异步操作
   完成时**调 `awaitable_frame_base::clear_cancellation_slot()`，那一次调用读的是信号
   内部（槽上的回调）：信号已被释放就是 `heap-use-after-free`（ASan 实测，栈落在
   `cancellation_slot::clear` ← `finishAsyncTableOp` ← 桥的 `driveOnce`）。因此
   `spawnAwaitableImpl` 把 `cancelSignal` **按值**传进协程体
   （`detail::invokeSpawnAwaitableOwned`），闭包与 `OpCtl` 同样按值进帧 ——
   `SpawnRecord` 在 `stopSpawns()` 里会被提前释放，帧里只存引用就会悬垂。
2. **受控轮询工具的 Job 改共享持有**。清理路径（`failAllPolledRoots`）可能在业务协程
   仍挂起时执行 cleanup，之后协程恢复还要读 `job->status/payload`，`delete job` 就是
   UAF。现在 Job 用 `std::shared_ptr<Job>`（协程帧与 `execute_start_invoke` 局部各持一份），
   `setCleanup` 只留空标记；`runPolledPumpJob` 另按值收一份 `cancelSignal`。
3. **取消 → 终态**：`execute_cancel` 与 `stopSpawns` 都
   `emit(cancellation_type::terminal)`，业务体里正在等的表调用被 `op_cancel` 撤掉；
   被撤掉的等待以 `boost::system::system_error(operation_aborted)` 出现，受控轮询体把它
   映射成 `PLUGINXX_OPERATOR_CANCELED`（正文清空），与 kit 协程体的取消语义一致。

### 本轮同时修掉的两处用例假设

- `client_plugins` 的 `sysinfo` 命令断言：命令回调改成**异步操作契约**后，插件要先
  `co_await` 一次表调用（查询服务端插件清单）才回传 toast，`drainPlugin` 返回时 toast
  可能还没到。用例改成 `waitUntil(...)` 等 toast 出现（5 s 上限）。
- 定时器同周期合并用例的观测窗口 700 ms → 1500 ms：首个回调要在插件执行线程上阻塞
  370 ms，机器负载高（并行构建 / 同进程其它模块）时 700 ms 只够 2~3 次触发，
  `ticks >= 4` 会随负载抖动（全量跑时实测偶发失败一次）。

### 验证（本轮）

- 顶层 Debug 构建 `EXIT=0`（lib + 25 个插件 + test + client 全部重链）。
- 全量测试 `agentxx_test --log-level warn`：**43914 passed / 1 failed**，唯一失败是上面
  那条定时器断言（已修）；其后单跑 `client_plugins` 842/0，批量
  `plugins` 706 / `plugin_bridge` 302 / `plugin_sdk` 151 / `plugin_example_runtime` 51 /
  `plugin_multi_mount` 48 / `plugin_cleanup` 95 / `math_tools` 82 / `client_plugins` 842
  共 **2277 条 failed=0**。
- 本轮另把两个插件剩下的 ticket 形态注册改成 asio 形态
  （`agentxx_filesystem` 6 处、`agentxx_websearch` 1 处 `tool(` → `co_await toolAsync`），
  重编后 `plugins` 模块 706/0。

## 第 5 步：删掉自制协程记账（未开工，执行清单）

### 前提（已验证）

- **插件源码零使用**：25 个插件里 `toolAsync` 56 处 / `tool(` 0 处 / `onWorker(` 0 处，
  `tableOp` / `whenAll` / `sleep` / `offload`(ticket) 也 0 处。删掉这些入口不改插件源码。
- **用例里的 ticket 形态只剩 4 个文件**：`test_plugin_bridge.cpp`（主体）、
  `test_plugin_sdk.cpp`（6 处 + 2 段 `Gate` 异步体）、`test_plugins.cpp`（2 处
  `unsubscribePluginBroadcast`）、`negative_compile/*`（3 个不被编译的说明性样例）。

### 框架 `agent/third_party/cxx_pluginxx/include/pluginxx/kit/kit.h`

| 删除项 | 位置（行号为本轮快照） |
| --- | --- |
| `destroyBridgeFrame` + `BridgeRoot` 类 | 689~849 |
| `PollOneBridge` 的根托管（`addRoot` / `removeRoot` / `retainAbandonedRoot` / `failAllRoots` / 析构里的 abandoned 分支 / `BridgeRoot::abandon` 定义） | 900~1300、1648~1700 |
| `Task` 前置声明 → `detail::finishIfDone` / `advanceRootOnce` / `resumePluginCoroutine` / `startBridgedRoot` | 2392~2600 |
| `PromiseBase` | 2879~2957 |
| `Task` / `Task<void>` | 2965~3130 |
| `spawnTaskImpl` + `PluginBaseT::spawn` 的分派（只留 asio 路径）+ `SpawnRecord` 的 `starter`/`coroAddr`/`closureKeepAlive` | 3134~3238、3740~3790 |
| `YieldAwaiter`（`yield`） | 3811~3849 |
| `SleepState` / `SleepTicket` / `sleep` | 3850~4041 |
| `OffloadWorkBase` / `OffloadState` / `OffloadWork` / `OffloadTicket` / `offload` | 4042~4299 |
| `InvokeCapState` / `InvokeCapAwaiter` / `invoke_cap` | 4456~4610 |
| `TableOpTicket` + `submitTableOp` | 4709~4883 |
| `using TableOpTicket = ...` + `tableOp` | 5172~5230 |
| `detail::takeTicket` / `takeAllInto` / 两个 `whenAll` | 5287~5385 |
| `capability` 的 ticket 形态 + `CapabilityJob` + `buildCapabilitySpec` 的 `Task` 分支 | 5488~5700 |

保留：`PolledRoot`、`PollOneBridge` 的请求/票/退避机制、`asyncTableOp` / `asyncOffload` /
`sleepAsync` / `tableOpCallback` / `finishLater` / `ProviderRoot` / `CancelableProvider`。

### kit 头 `agent/lib/include/agentxx/plugin/api/`

- `agent_plugin_kit.h`：删掉 45 个返回 `pluginxx::TableOpTicket` 的入口（全都已有
  `*Async` 孪生）、`tool()` / `graph_node()` / `capability()` / `providePoint()` 的 ticket
  形态、`onWorker()`（Task 包装，已由 `onWorkerAsync` 取代）、
  `registerToolWithTaskBody`(+Async) / `runTaskToolBody` / `IsTaskToolResult` /
  `registerToolTicket`、`graph_node` 与 `buildCapabilitySpec` 的 `Task` 分支。
- `plugin_kit.h`：删 `using pluginxx::{TableOpTicket, Task, whenAll}` 与 4 个
  `detail::{BridgeRoot, PromiseBase, spawnTaskImpl, startBridgedRoot}` 别名、5 个广播
  ticket 形态（都有 `*Async` 孪生）。
- `client_plugin_kit.h`：删 1 处 `Task` 残留。

### 用例整理

- `test_plugin_bridge.cpp`（1900 行）：整段删 §1~§10（623~1098）与 §15~§23（1410~1912）
  —— 它们测的就是被删的记账；保留 §11~§14（受控轮询，全绿）。§11~§14 里 4 处
  `agentxx::plugin::tool(...)` 改成测试内的同步登记助手：
  ```cpp
  template<typename Body>
  void registerAwaitableTool(BridgeCtx& ctx, std::string_view name, Body&& body) {
      auto spec = agentxx::plugin::detail::buildToolSpecWithAwaitableBody(
          ctx, g_agentRef, name, "depict", "{}", std::forward<Body>(body));
      auto ref = g_agentRef;
      (void)ctx.iface.tools->register_tool(ctx.host, &ref, &spec, nullptr, nullptr, nullptr);
  }
  ```
  （假 host 的 `register_tool` 同步填 `g_capturedTool` 且容忍 `cb == nullptr`，已核对。）
- `test_plugin_sdk.cpp`：`providePoint` / `graph_node` / `capability` 的 6 处改
  `co_await *Async(...)` + `.ok()`；两段 `Gate` 异步体（能力 / 图节点）必须重写 ——
  `asio::awaitable` 只认 awaitable 与 asio 异步操作，普通 awaiter 不能用，改用
  `asio::steady_timer` 制造挂起，并把"手工 `handle.resume()` + `finishRoot<...>`"换成
  驱动桥（`runDriver()`）直到完成。
- `test_plugins.cpp` 2 处 `unsubscribePluginBroadcast` → `co_await ...Async`；
  `negative_compile/*` 3 个样例同步改（不被编译，但要对得上当前 API）。
- `boundaries`：规则 16 的禁用名清单加入 `TableOpTicket` / `whenAll` / `pluginxx::Task`
  一类名字（删完之后再出现即为回退）。

### 收尾

`AGENTS.md`（删掉"kit 协程体 `Task<T>`"相关表述，改成 asio 唯一形态）、
`docs/zh-cn/design/plugins.md`（§16 协程与表调用、§4 完成协议），以及本目录三份文档。

## 第 5 步：一次未完成的尝试（2026-11-23，已回滚）

按上面的清单直接动刀做了一轮"一次性删除"，结果**整体回滚**（四份 kit 头 + 三个用例文件
从备份恢复，重新构建 `EXIT=0`、`boundaries` / `plugin_bridge` / `plugin_sdk` /
`client_plugins` / `math_tools` 共 1397 条 failed=0）。记录踩到的坑，后续按新顺序做：

### 实际做了什么（已回滚）

- `kit.h`：按标记行批量删掉 20 个区段（BridgeRoot、Task/PromiseBase、`spawnTaskImpl`、
  ticket 一族、`capability` 的 Task 分支等），约 1550 行。
- `agent_plugin_kit.h`：脚本删掉 46 个返回 `TableOpTicket`/`Task` 的入口 + SpawnRecord/
  ToolBodyKind 等；`plugin_kit.h`：删 5 个广播 ticket 形态 + 12 个 `using`。
- `test_plugin_bridge.cpp`：删 §1~§10、§15~§23（测的正是被删机制），§11~§14 的 4 处
  `tool(...)` 改成本地 `registerAwaitableTool`。
- `test_plugin_sdk.cpp`：ticket 入口改 `runAwaitable(*Async(...))`，3 处 `Gate` 改
  `awaitTick()`，手工 `handle.resume()` 改 `pumpUntil(...)`。

### 三个卡点（务必按新顺序做）

1. **按行区间批量删除会吃掉下一段的 `template<...>` 头**。`Cut-Region(start → end)` 里
   `end` 是"下一个声明的名字行"，而它的模板参数行写在名字行**之前**，因此被一并删掉：
   `AsyncTableOpState`、`OffloadOutcome`、`OffloadWorkBase` 都变成"用了未声明的
   `Handler`/`ResultT`"。删完必须回头补 `template<...>` 行。
2. **命名空间收尾会被算错**。删 `Task<T>` 段时把 `namespace detail {` 的开头一起吃掉了，
   于是后面多出一个 `} // namespace detail`（8 开 9 关），编译器报的是若干行之后的
   `detail 不是命名空间`。改完要数一遍 `namespace detail {` 与 `} // namespace detail`。
3. **用例里还有一批依赖 Task await 的辅助类型**：`detail::CallToolAwaiter`
   （`callTool` / `callToolResult` 的 ticket 形态）、`ToolPromptAwaiter`/`ToolSchemaAwaiter`、
   `agent_plugin_kit` 里 4 处 `resumePluginCoroutine`、`test_plugin_sdk` 的
   `finishRoot<...>`/`Gate`。它们不在"46 个 ticket 入口"名单里，得单独逐个处理。

### 下次按这个顺序（每步都能单独编译验证）

1. **先删消费者，保留被删类型**：`agent_plugin_kit.h` 的 ticket 入口、`tool()`/`graph_node()`/
   `capability()`/`providePoint()` 的 ticket 形态、`onWorker`、`CallToolAwaiter` 一族、
   `runTaskToolBody`/`buildToolSpecWithTaskBody`、`ToolPromptAwaiter`/`ToolSchemaAwaiter`；
   `plugin_kit.h` 的 5 个广播 ticket 形态与 `using`。此步 `Task`/`TableOpTicket` 仍在
   kit.h 里，编译必然通过 —— 用它验证"没有遗漏的调用点"。
2. **改用例**：`test_plugin_bridge` 删 §1~§10/§15~§23 + `registerAwaitableTool`；
   `test_plugin_sdk` 用 `runAwaitable`/`awaitTick`/`pumpUntil`；`test_plugins` 的两处
   订阅撤销直接删（卸载时管理器统一撤销）。构建 + 跑这批模块。
3. **最后删 kit.h 机制**：此时它是死代码，删的时候用"整段读取再重写"而不是按行删
   （或每删 2~3 段就单独编一个只 include kit.h 的小 TU 验证），重点检查
   `template<...>` 头与 `namespace detail` 配对。
4. 收尾：`boundaries` 禁用名、`AGENTS.md`、`plugins.md`。

按这个顺序，任何一步中断都停在可编译状态；一次性大改则会出现"越修越多的语法错"
（本轮就是这样停下来的）。

## 第 5 步第 3/4 小步：删 kit 机制与收尾（已完成，2026-11-24）

### 第 3 小步：`pluginxx/kit/kit.h` 删掉自制协程记账

`kit.h` 6156 → 4227 行（-1929），共删除 40 个整声明：

| 类别 | 删除项 |
| --- | --- |
| 桥根 | `destroyBridgeFrame`、`BridgeRoot`（含 `abandon`） |
| 任务 | `Task` / `Task<void>`、`PromiseBase`、`detail::finishIfDone` / `advanceRootOnce` / `resumePluginCoroutine` / `startBridgedRoot`、`spawnTaskImpl` |
| 原语 | `YieldAwaiter`、`SleepState` / `SleepTicket` / `submitSleep`、`OffloadWorkBase` / `OffloadState` / `OffloadWork` / `OffloadTicket` / `submitOffload`、`InvokeCapState` / `InvokeCapAwaiter`、`TableOpState` / `TableOpTicket` / `submitTableOp` |
| 自由函数 | `sleep` / `yield` / `offload` / 2×`invoke_cap` / `tableOp` / 2×`whenAll` / `takeTicket` / `takeAllInto` / `capability` / `CapabilityJob` |

同时：`PollOneBridge` 去掉根托管（`addRoot` / `removeRoot` / `retainAbandonedRoot` /
`failAllRoots` / `activeRootCount` / `roots_` / `abandonedRoots_`），"驱动被拒"改走
`failAllPolledRoots`，无进展警告条件简化为只看 `polledRoots_`；`SpawnRecord` 只留
`cancelFlag` / `ctl` / `cancelSignal`；`PluginBaseT::spawn` 只走 awaitable 分支；
`buildCapabilitySpec` 的 `Task` 分支改 `static_assert`（提示只能写 `asio::awaitable`）；
`spawnReturnsAwaitable` 删除。

### 第 4 小步：收尾

| 位置 | 改动 |
| --- | --- |
| `test_boundaries.cpp` 规则 16 | 文档改成"两种形态"，新增禁名表 `kKitCoroutineHelpers`（17 个 `pluginxx::*` 名字）+ `checkNames(...)`，名字一律带 `pluginxx::` 前缀，避免误伤 `scheduler->sleep(...)` 这类合法调用 |
| `test_plugin_bridge.h` / `test_plugin_bridge.cpp` | 顶部文档去掉 `BridgeRoot`；用例只保留 §11~§14（受控轮询） |
| `test/plugin/negative_compile/*.cpp` | 4 个说明性样例改成 `co_await *Async(...)` 形态 |
| `AGENTS.md` | 插件表调用段：只剩两种形态；kit 自制协程与它们的 awaiter 类型记为"已全部删除"；`fast_tool`/`blocking_tool`/`polled_tool` 改为 `toolAsync` / `onWorkerAsync` |
| `docs/zh-cn/design/plugins.md` | §5.1 kit 行、§6 概览与 SDK 表、§6 示例代码、§8 异步模型段、§16.4 桥接表、§16.5 约束、§16.6 迁移状态与样例、§7 阻塞段清单统一改成 asio 形态（`toolAsync` / `onWorkerAsync` / `offloadAsync` / `sleepAsync` / `callToolAsync`） |
| `example_plugin.cpp` / `kit.h` | 说明性注释里的 `Task` 协程 / `polled_tool` / "offload 降级"表述改为当前实际行为 |
| `agent_plugin_kit.h` | 一处指向已删名字的文档链接（`[registerToolTicket]`）改指向 `toolAsync` |

### 本轮新踩的坑

1. **按锚点整段删除时，终点只能认列 0 的 `}` / `};`**。第一版用 `Trim()` 判终点，把嵌套
   lambda 的 `};` 当成了声明结尾，删完花括号计数 `{`845 / `}`916（差 71），只能从备份重来。
   改成"只认列 0"后 713/713 一致。
2. **残留的 `template<...> requires(...)` 头与 `using` 只能靠编译暴露**：删声明时漏掉的
   模板头让 `tableOpCallback` 报"模板参数过多"；`plugin_kit.h` 还留了
   `advanceRootOnce` / `finishIfDone` 两个 `using`。用一个只 include 两个 kit 头的小 TU
   （`cl /c /Zs`）比跑整棵树快得多。
3. **测试里的伪调度器要跟着补 `sleep`**：`test_plugin_sdk` 的假 scheduler 原本只有
   `request_driver`，`asio::awaitable` 版的 `sleepAsync` 全走 `scheduler.sleep` 之后
   12 条用例报 `host scheduler timer unavailable`。补 `fakeSleep` + 到期计时器表
   （`runDriver()` 先触发到期项）后全绿。
4. **含大段中文的 `edit` 匹配会失配**（本轮两次）：同样的文字肉眼一致仍然 `No match`。
   这类整段改写改用 PowerShell 按行号重写（`ReadAllLines` → 按下标替换 → `-join "`n"` →
   `UTF8Encoding($false)`），一次成功。

### 验证

- 顶层 Debug 构建 `EXIT=0`（含 `boundaries` 规则改动与插件头改动后的重编）；25 个插件 DLL
  全部重新链接。
- `boundaries` 20 / `plugin_bridge` 72 / `plugin_sdk` 148 / `plugin_runtime` 774 /
  `plugin_executor` 663 / `plugins` 706 / `client_plugins` 842 → **passed=3225 failed=0**。
  规则 16 的新禁名表在 `agent/lib` 与 `agent/plugins` 上没有误报。
- 同批改动的插件模块（`math_tools` 82 / `command` 72 / `filesystem` 285）与前次一致。
- 全量 `agentxx_test --log-level warn`：**43682 passed / 0 failed**（`EXIT=0`）。与基线
  43914 的差值是本轮有意删除的用例：`plugin_bridge` 302 → 72（删掉测自制记账的 22 段）、
  `plugin_executor` -2；基线里那条定时器断言在上轮已修，本轮无失败。

### 顺带改动（不属于删除本身，但一起提交）

- `sessionMessagesCount` 改成 `asio::awaitable<TableOpResult>`（原来是 ticket 形态的
  唯一读取入口）。
- `test_plugin_bridge.cpp` 新增本地 `registerAwaitableTool`（走
  `buildToolSpecWithAwaitableBody` + `iface.tools->register_tool(..., nullptr)`），
  因为公开的 `toolAsync` 会真的去调表。
- `test_plugin_sdk.cpp` 的 `ToolNotifyProbe` 增 `doneFrame`，覆盖两段帧的上报路径。
