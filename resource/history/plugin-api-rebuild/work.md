# 实施记录

## 当前进度

| 步 | 内容 | 状态 |
|---|---|---|
| 0 | 现状调研（线程/线程池/生命周期/表调用/阻塞点/命名/构造时机/通用性/get_info/create/kit 布局/其它同步面） | 完成（记录在 [plan.md](plan.md) §2，含 §2.14 补审计） |
| 1 | 决策确认 + 基线记录 | 待开始 |
| 2 | 命名：入口符号与导出宏、`entrySymbols()`、全部表编号改下划线 | 待开始 |
| 3 | `get_info` 填表化 + 删 `AgentxxClientPluginInfo` | 待开始 |
| 4 | 框架通用化收尾（注释/示例去端化 + `boundaries` 规则） | 待开始 |
| 5 | 两侧管理器启动即构造 + 删 `ensurePluginThread` | 待开始 |
| 6 | 线程池统一（管理器持一个池，默认 3 条，`plugin.thread_pool_workers`，两端同值；删其余四个池） | 待开始 |
| 7 | 4 个同步受理入口改提交（含 `OpCore` 操作编号原子化与登记线程安全化） | 待开始 |
| 8 | `is_canceled` 改快照 + JS 引擎轮询 | 待开始 |
| 9 | 其余同步面收口（退订去等待 + `disable`/`enable` 返 `Task<bool>`） | 待开始 |
| 10 | 删同步桥与阻塞助手（同名改协程）+ kit 拆三个头 + 插件侧阻塞点迁移 | 待开始 |
| 10b | 工具体收成一个：`tool`（`pluginxx::Task` 体）+ 删 `fast_tool` / `blocking_tool` / `polled_tool` / `registerToolSync`（53 处） | 待开始 |
| 10c | `offload` / `sleep` 改“提交即返回 ticket” + `whenAll` 泛化/变参 | 待开始 |
| 11 | `create`/`start` 合并（返回 void、可重入）+ `destroy` 改异步 + 插件迁移 | 待开始 |
| 12 | 测试夹具统一带插件线程与池 | 待开始 |
| 13 | 修静默降级（§2.6 六处 + 两处订阅）+ 文档/代次 + 全量回归 | 待开始 |

## 设计过程

- 第一轮（原任务名 `plugin-async-api-unify`）：只处理"插件线程上还剩同步等 io"与"同一件事两种写法
  导致的静默降级"。
- 第二轮：加入"不保留兼容"、"入口符号改名"、"框架去端化与表命名"、"`get_info` 填表化"、
  "`create` 与 `start` 合并"，任务目录改名为 `plugin-api-rebuild`。
- 第三轮（已确认）：
  1. `create` 返回 `void`（上下文走 `void**` 出参）；
  2. 启用重新注册 = **可重入 `create`**（不另加 `reload` 入口）；
  3. 表编号一律下划线：通用表 `agentxx_plugin_*`、业务表 `agentxx_agent_*` / `agentxx_client_*`；
     能力/广播 id 仍点分不变；
  4. `destroy` 也改成异步（`dlclose` 投管理器工作线程）；
  5. kit 拆头**一步到位**，不保留迁移期转发头；
  6. 两端管理器**程序启动就构造**（不等加载插件）；
  7. 每端管理器**只持有一个工作池**（默认 3 条），进程内不再有别的线程池，一律共用它。
- 第四轮：补审计“除那 5 个表入口外还有没有别的同步接口”，结论写进 plan.md §2.14，
  并把方案接到 §3.5 / §5 / §6 / §7 / §8（新增 D13 / D14，步骤表从 12 步变 13 步）。
- 第五轮：按“默认异步、名字不标 `Async`”定规则（D15）：不新加 `*Async` 版本，原名直接改成协程；
  现存的 `configAsync` / `argsJsonAsync` / `clientTableOpAsync` / `resourceOpSync` / `registerToolSync`
  一并收成原名；`disable` / `enable` 也原名返 `Task<bool>`（不再问“要不要保留同步壳”）。
- 第六轮（5 条决策）：① 配置键改名 `plugin.thread_pool_workers`；② 池数量不分开配（两端同值）；
  ③ 池共用（agent 端整体一个、client 端整体一个，子上下文用根的）；④ 不加 `boundaries` 新规则；
  ⑤ 默认的 `tool` 就是异步（协程体）。
- 第七轮：① `fast_tool` / `registerToolSync` 确定删；② `blocking_tool` 的体改协程体
  （澄清：它的同步返回**不阻塞调用方** —— 体跑在工作池线程，卡插件线程的是 `execute_start` 里的两次
  `workDir/argsJson` 读）；③ 查证 `PluginExecutorThread` 本来就是一条 `io_context` 的 `run()` 线程，
  `tool` 与 `polled_tool` 都挂在 `PollOneBridge` 上，所以两者可合并。
- 第八轮（修正）：否定了“改成 `asio::awaitable` + `co_spawn`”的方案 —— `asio::awaitable` 必须有个
  执行器 `co_spawn` 才能起步，而 C ABI 传不了 `asio::any_io_executor`；插件只能自建 `io_context` 再
  `run()`，而 `run()` 会占住唯一插件线程，其它插件与框架派发全部饿死。**最终设计**：工具体 = kit 的
  `pluginxx::Task`，挂起 = 登记完成源（`sleep` / `offload` / 表操作），唤醒 = 完成回调投回插件线程后
  `handle.resume()`；插件里禁止 `io_context`/`co_spawn`/`run()` 与自建 socket/管道/定时器。
  `polled_tool` 删除，6 处改成 `offload` / `sleep`；`PollOneBridge` 的 poll_one 循环分两步拆。
- 第九轮：① 新增 D17 与 plan.md §3.10：`offload` / `sleep` 改“提交即返回 ticket”（名字不变、
  完成回调里直接 `resume()`），`whenAll` 泛化 + 变参；② 三份文档通读整理（表格断行、行号、
  与已定决策不一致的旧说法、重复条目、`co_spawn` 残留等）。

## 调研确认的事实（本次没有改代码）

1. **插件线程（含插件自建线程）上仍有 5 个同步等 io 的入口**：`capabilities.invoke*`、
   `scheduler.sleep`、`scheduler.offload`、`tasks.register_task`（`ctx.spawn` 每条一次）、
   `agentxx.agent.session.is_canceled`（JS 引擎在 JS 线程轮询）。全部无超时。
2. **六处静默降级**：`llm_uncensored`（`text`/`key`）、`planning`（`dataDir`）、`example_resources`
   （插件路径）、`chatbot` 与 `text_selection_monitor`（网络事件订阅）、`example_runtime`（`argsJson`）——
   在插件线程上用阻塞形态读表 → 线上读不到，回落默认值或直接失败。测试盲区：只有
   `test_plugin_executor.cpp` 配了插件执行线程。
3. **`create` 契约与实际不符**：JS 引擎在 `create` 里 `JS_NewRuntime` + 起 JS 线程；codegraph 在
   `create` 里开 sqlite；框架靠"投工作线程"兜住。
4. **`get_info` 不漏但无约定**：导出宏与 25 个内置插件都返回 `static const`，框架没有释放入口。
5. **`cxx_pluginxx` 代码层已端无关**：`agent`/`client` 只出现在 17 处注释/示例里。
6. **线程池分散在四处**：`AgentHost::threadPool_`、`AgentContext` 的默认池（`context.h:1080`）、
   client 的 `pool_` 与 `workerPool_`、`PluginManager` 的回退池（`plugin_manager_domain_services.cpp:249`）；
   借用方还有工具 offload、会话加载、git worktree、媒体附件。
7. **构造时机**：agent 管理器在 `loadPluginsAsync` 懒建；client 管理器在装配阶段建，但插件线程
   （`ensurePluginThread`）与池仍懒启。
8. **框架级还有一处阻塞**：`pluginxx::unsubscribePluginBroadcast`（`broadcast_registry.h:349`）
   置 `alive=false` 后 `fut.wait()` 等 io 簿记，无超时。
9. **kit 阻塞形态共 7 类**（plan.md §2.14b）：广播 4 个 / 权限 / 提示词·模型·图·资源 / 工具 /
   运行时 / client 便捷函数 / `PluginBaseT` 的 `config|argsJson|configPath|language`。
10. **线上真阻塞点四处**：filesystem `:252` 的 dst 复核（在 `blocking_tool` 的工作线程上等 io，
    `scheduler` 传 null → **绕过拒绝守卫**）、JS 引擎 `:5971` 的 `publish`、`:5514` 的退订
    （后两处在 JS 线程）、`checkPathDecisions` 自己也是阻塞形态（`plugin_kit.h:5653`）。
11. **线上真静默失败四处**：chatbot `chatbot_net.cpp:258`（← `qq_gateway.cpp:62` 的
    `subscribeNetEvents`）与 text_selection_monitor `:245` 的阻塞 `subscribePluginBroadcast`（插件线程上被拒 →
    返回 `nullptr`，网关收不到网络事件）、example_runtime `:60` 的 `argsJson()`（恒 `{}`）、
    以及 §2.6 那几处配置读。
12. **合并 create 后仍被拒的四处**：codegraph `:46/:61`、`example_js:232/:240`、
    `example_js_execute_command:239`、`example_resources:61`。
13. **主程序启停不可等**：`lifecycle.h:631/636` 的 `disable`/`enable` 是 fire-and-forget，测试只能轮询。
14. **工具体有四种跑法**：`fast_tool`（插件线程同步体，不能阻塞、不能 `co_await`）、`blocking_tool`
    （工作池同步体，可以阻塞但占一条池线程）、`polled_tool`（`asio::awaitable` 体）、`tool`
    （`pluginxx::Task` 协程体，已是目标形态）。filesystem 的 10 个工具全是 `blocking_tool` 或 `polled_tool`。
15. **一处误判已更正**：planning `:1217` 的 `registerToolRenderer` 走的是 `clientTableCall`
    （`plugin_kit.h:5958`，提交即返回），任何线程都不等、也不会被拒 —— 它不属于阻塞面。
16. **现有 tool 类型共 4 个入口**（详见 plan.md §2.15）：`tool`（协程体，已被 example_plugin 使用 3 处）、
    `fast_tool`（10 处 / 6 插件，插件线程同步体）、`blocking_tool`（37 处 / 12 插件，工作池同步体）、
    `polled_tool`（6 处 / 3 插件，`asio::awaitable` 体 + 受控 `poll_one`）；另有低层
    `registerToolSync`（插件 0 处）。
17. **两个协程工具实现同源**：`tool` 走 `startBridgedRoot(bridge…)`、`polled_tool` 走
    `co_spawn(bridge->local_executor()…)`，都靠 `PollOneBridge`；而 `PluginExecutorThread`
    （`plugin_thread.h:47-56`）已是完整 `io_context` 的 `run()` 线程 —— 桥的前提（插件跑在主程序 io 线程、
    无自己的 reactor）已不存在，故 `polled_tool` 可删、`tool` 改成直接 resume 的驱动（不经 poll-one）。
18. **`blocking_tool` 不是同步阻塞调用方**：`execute_start` 提交 `scheduler.offload` 后立即 `return job`，
    体在工作池线程；卡插件线程的是那两次 `workDir/argsJson` 阻塞读。
19. **唤醒原语已经有了**：`PluginxxSchedulerIface`（v2）里 `is_io_thread` / `sleep` / `offload` /
    `request_driver` / `cancel_driver` 都是 C ABI 回调式，“稍后在插件线程上 resume 我”就是
    `request_driver`；因此 kit 不需要任何 asio 执行器就能驱动协程帧（C ABI 也传不了执行器）。
20. **并发能力分两层**：`tableOp` 调用即提交，一批 ticket 可 `co_await whenAll(...)`（总耗时 = 最慢那条，
    `kit.h:4302`）；但 `offload` / `sleep` 是 `await_suspend` 时才提交（`kit.h:3699` / `:3606`），
    连写两个 `co_await` 是串行 —— 已定改成 ticket 形态（plan.md §3.10），不需要 reactor。
21. **`sleep` 由框架实现**（`PluginxxHostCore::sleep`，`host_core.h:1106`）：io 执行器上的 `steady_timer`
    + 完成回调，**不占插件线程**；但今天它的表受理还是 `ioCallSyncKeep`（`tables_impl.h:1064`），
    每次占插件线程一个 io 往返（§2.4 第 2 条）。延时时长单位为毫秒；`yield` 存在但靠桥投递 + 唤醒。

## 备注

- 入口符号改名涉及约 200 处，其中绝大多数在导出宏、`entrySymbols()` 与 `plugin_kit.h` 的转发里，
  按"先改宏与两个 `entrySymbols()` → 编译报错定位 → 逐处修"的顺序做。
- 表编号变更共 26 个（通用 10 + agent 业务 10 + client 业务 6），与 `boundaries` 的逐表校验、
  `plugin_runtime` 的编号判定同步改。
- `create` 合并后，"禁用/启用"路径要能只做重新注册（`create` 可重入），否则启用会把 ctx 重建、
  per-mount 状态全丢 —— 这是本任务最容易回归的一处，见 plan.md §3.4(c)。
- 池从 4 处并成 1 处之后，`dlopen`/`dlclose`、`blocking_tool`、工具 offload、会话加载、worktree、
  媒体附件会抢同一个池（默认 3 条，`plugin.thread_pool_workers` 可调）；观察 `pluginCallbackStats`
  与加载耗时，**只调大池，不新建池**。
- 第四轮补审计把步骤表从 12 步加到 13 步：新增第 9 步“其余同步面收口”，原第 9 步（删同步桥 + kit 拆头）
  并入“插件侧阻塞点同批迁移”（否则删完助手插件就编不过）。
- 工具体直接收成 `tool`（已有，`pluginxx::Task` 体）：先改 filesystem（10 处 `blocking_tool` + 3 处 `polled_tool`）
  验证模式，再批量套 37 处 `blocking_tool` 与 10 处 `fast_tool`；每处阻塞段包 `co_await pluginxx::offload(ex, …)`。
- `polled_tool` 的 6 处调用点按 plan.md §3.9 的表改：文件读 / 子进程 → `offload`（两边都有 blocking 分支
  可对照）；示例的定时器 → `sleep`。
- `PollOneBridge` / `PolledRoot` / `startBridgedRoot` / 主程序侧 `poll_one` 循环**分两步拆**：
  第一步工具不再走它；第二步图节点 / `call_tool` awaiter 改 `Task` 驱动后整块删。
- 池配置键改名后要同步 `docs/zh-cn/design/config-keys.md` 与 `config_keys` 测试（归属列 = agent + client
  共用）；旧的 `threadPoolWorkers` 直接删，不做别名。
- 命名统一按 D15 执行：删的是**第二形态的名字**（`*Async`）与**阻塞形态的名字**（`*Sync`），
  保留下的是原名 + `*NoWait`；不加 `boundaries` 自动检查，靠代码评审把关。
- JS 引擎的 `publish` 改 Promise 后，脚本侧写法不变（`await` 才有受理结果），但文档与示例
  （`agent/plugins/example_js/plugin.js`）要补一句说明。
