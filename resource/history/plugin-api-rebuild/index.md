# 插件接口重构：框架通用化 + 入口协议统一 + 调用面异步化

## 目标

插件体系一次彻底重构（不保留旧版本兼容：最低支持版本已抬过，仓库外没有第三方插件）。六件事：

1. **调用面只剩一种形态**：C API 只表达"提交 + 回调"，kit 只包协程；插件线程（以及插件自建的
   线程）上不再有"同步等 io"的入口，也不再有"同一件事两种写法"；名字不带 `Async`（异步是默认）。
2. **框架通用化**：`cxx_pluginxx` 只认"应用 + 作用域"，不认"agent / client"两端；通用表挂在
   `<应用名>_plugin_*` 编号下；两端的复用代码各自放在 `agentxx` 里。
3. **入口协议统一**：`get_info` 改成"框架给结构体、插件填写"；`create` 与 `start` 合并成一个异步
   `create`（返回 `void`，上下文走出参）；`destroy` 也改成异步；所有生命周期入口同一套
   "恰好一次 notify"协议。
4. **命名统一**：入口符号 `agentxx_agent_plugin_*` / `agentxx_client_plugin_*`，表 IID 一律下划线
   （`agentxx_plugin_log` / `agentxx_agent_tools` / `agentxx_client_ui`）。
5. **线程模型收敛**：agent 端与 client 端各自一个插件框架管理器，**程序启动就构造**；每个管理器
   自带一条插件执行线程 + **一个工作池**（默认 3 条，`plugin.thread_pool_workers` 两端共用一个值），
   一个端只有一个池：agent 上下文、工具 offload、`dlopen`、会话加载全部复用它；
6. **工具只有一种形态**：只保留 `tool`，体是 kit 的 `pluginxx::Task<ToolResult>`（靠“登记完成源 +
   完成回调里 resume”驱动）；`fast_tool` / `blocking_tool` / `polled_tool` / `registerToolSync` 全删，
   阻塞段 `co_await offload(...)`；插件代码不自建 reactor，切换与唤醒模型见 [plan.md](plan.md) §3.9。

## 范围

- `agent/third_party/cxx_pluginxx`：入口协议、表编号宏、生命周期、`OpCore`、同步桥、kit 通用层；
- `agent/lib`：agent 侧 `PluginManager`、client 侧 `ClientPluginManager`、两份入口符号与接口表头、
  `AgentHost` / `AgentContext` 的线程池归属、kit 按端拆分；
- `agent/plugins`：25 个内置插件一次性迁移（入口签名、命名、配置读取、`ctx.spawn`、`isCanceled`），
  含 53 处工具体调用点（`fast_tool` 10 / `blocking_tool` 37 / `polled_tool` 6）；
- `agent/client`：客户端启动装配（管理器在启动时构造）与插件线程/池的启动时机；
- 文档与代次：`plugins.md`、根 `AGENTS.md`、`plugin_api.h` / `client_plugin_api.h` 的版本常量。

## 状态

**已完成**（13 步全部落地；两轮全量回归，最终 `passed=43905 failed=0`）。逐项记录见
[work.md](work.md)，当前实现形态见 [plugins.md](../../../docs/zh-cn/design/plugins.md)。

落地结果（与上面 6 条目标对应）：

1. 调用面：C API 只"提交 + 回调"；kit 默认异步（原名即协程），只剩 `*NoWait` 提交形态；
2. 框架通用化：`cxx_pluginxx` 只认"应用 + 作用域"，通用表挂在 `agentxx_plugin_*` 编号下，
   两端复用代码各一份；
3. 入口协议：`get_info` 填表、`create` 合并 start（可重入）、`destroy` 异步，同一套
   "恰好一次 notify"；
4. 命名：`agentxx_agent_plugin_*` / `agentxx_client_plugin_*`，表 IID 全下划线；
5. 线程模型：两端管理器启动即构造，各一条插件执行线程 + 一个池
   （`plugin.thread_pool_workers`，默认 3，两端同值），`AgentContext` / 工具 offload /
   `dlopen` / 会话加载都借用它；
6. 工具只剩 `tool`（`pluginxx::Task<ToolResult>` 体），`fast_tool` / `blocking_tool` /
   `polled_tool` / `registerToolSync` 全删。

代次：`AGENTXX_PLUGIN_API_VERSION` = 8、`AGENTXX_CLIENT_PLUGIN_API_VERSION` = 9。

### 与决策的偏差（实现时定下来的）

| 决策 | 偏差 | 原因 |
|---|---|---|
| D10 / D13 删同步桥与全部阻塞助手 | 保留 `pluginxx::tableOpSyncWait` + `SyncWaitThreadPolicy`（默认 `refuseOnPluginThread`，runtime 表与 client 表显式 `allowOnPluginThread`）作为唯一同步等待原语；`tableOpText` / `tableOpOk` / `ioCallSync*` 已删，kit 里只剩 4 处使用 | 生命周期入口（`stop` / `destroy`）与 `create` 期的实例级查询确实需要当场拿结果，同步桥就地唤醒不会自己等自己 |
| D13 插件源码零同步调用 | 两处 `*Sync` 白名单：`agentxx_chatbot`（engine / 任务运行时注入点跑在自己的线程上）、`example_runtime`（`stop` 入口） | 卸载期间主程序不再推进新的协程根，`spawn` 的清理任务不会启动；`boundaries` 规则 16 静态拦截并写明理由 |
| plan §3.7 用 `AgentPluginBase` | 保留 `PluginBase` 原名字，放进 agent 头 | 它本就是 `PluginBaseT<AgentIfaces>`，改名要动 20 个插件源码且无功能收益 |

### 附带产出

- `agent/test/include/agentxx-test/plugin/plugin_thread_fixture.h`：插件执行线程 + 工作池夹具，
  14 个插件模块接入（插件用例第一次在"生产同款线程模型"下跑）；
- SDK 按端拆三个头：`plugin_kit.h`（两端共用，782 行）/ `agent_plugin_kit.h`（agent，4989 行）/
  `client_plugin_kit.h`（client，1683 行），55 处包含点同时切换，不保留转发头。

## 关键决策

| 编号 | 决策 | 说明 |
|---|---|---|
| D1 | **不做兼容层**，一次性换代 | 旧形态直接删除，SDK 不保留双签名，kit 不保留迁移期转发头 |
| D2 | 入口符号改为 `agentxx_agent_plugin_*` / `agentxx_client_plugin_*` | 与业务表 `agentxx_agent_*` / `agentxx_client_*` 分组一致；由 `entrySymbols()` 一处给出 |
| D3 | 表 IID 一律下划线：通用表 `<应用名>_plugin_*`，业务表 `<应用名>_<端>_*` | 例 `agentxx_plugin_log` / `agentxx_agent_tools` / `agentxx_client_ui`；能力与广播 id 仍是点分（`agentxx.agent.*`），不受影响 |
| D4 | `get_info` 改"填表"：`int32_t get_info(PluginxxInfo* out)` | 框架构造并初始化，插件只写 name/version/description 与 `min_api_version`；插件不分配内存 |
| D5 | `create` 与 `start` 合并为一个异步 `create`，**返回 `void`** | 上下文走 `void**` 出参且必须同步写出；注册在同一个协程里 `co_await`；同一 `create` 可重入（启用时只做重新注册） |
| D6 | `destroy` 也改成异步（与 `create`/`stop` 同一协议） | 插件在插件线程释放本地对象并 notify；框架收到完成后再在工作线程 `dlclose`（不占 io 线程） |
| D7 | 插件执行线程固定创建（管理器构造即起），删 `ensurePluginThread` | 只剩一种线程形态；测试与线上一致 |
| D8 | 两端管理器**启动即构造**，不等"有插件要加载" | agent：`AgentHost::create`；client：客户端装配阶段；代价是空载也有 1 条执行线程 + 1 个池（默认 3 条） |
| D9 | 每个端**只有一个工作池**，数量两端共用一个配置键 | 键名 `threadPoolWorkers` → `plugin.thread_pool_workers`（默认 3，不分开配）；`AgentHost::threadPool_` 与 `AgentContext` 默认池、client 的 `pool_`/`workerPool_` 全部取消，统一取管理器上的池 |
| D10 | 删同步桥与全部阻塞助手（`tableOpSyncWait` / `tableOpText` / `ioCallSync*` 等） | kit 只剩“原名即协程 + `*NoWait` 提交”两种形态，不再有阻塞版 |
| D11 | 通用表 4 个“同步受理”入口改纯提交，`is_canceled` 改读 io 维护的快照 | 插件线程与插件自建线程上零跨线程等待 |
| D12 | kit 拆成通用 / agent 侧 / client 侧三个头，不保留兼容转发头 | 两端各一份，框架层不含端相关代码 |
| D13 | 框架级与 kit 其余阻塞面一并收口 | `pluginxx::unsubscribePluginBroadcast` 去掉 `fut.wait()`；kit 的阻塞便捷函数（广播 / 权限 / 提示词·模型·图·资源 / 工具 / 运行时 / client / 上下文）全部改成同名协程；阻塞形态的入口（`tableOpSyncWait` / `ioCallSync*`）直接删除，不再保留（代码评审把关，不加自动检查） |
| D14 | **工具体只留 `tool`，体是 kit 的 `pluginxx::Task<ToolResult>`**；`disable` / `enable` 原名返 `Task<bool>` | 挂起 = 向主程序登记完成源（`sleep` / `offload` / 表操作），唤醒 = 完成回调投回插件线程后 `resume()`；插件代码**不自建 asio reactor、不写 `run()`**（否则占住唯一插件线程，且 C ABI 传不了执行器）。删 `fast_tool`（10）、`blocking_tool`（37，阻塞段 `co_await offload`）、`polled_tool`（6）、`registerToolSync`（0） |
| D15 | **名字不标异步**：默认就是异步，不新加 `*Async` 后缀 | 要结果 = 原名返回 `Task<…>` / ticket；不要结果 = 原名提交即返回；需要强调“只发不等”用已有 `*NoWait`。现存的 `configAsync` / `argsJsonAsync` / `clientTableOpAsync` / `resourceOpSync` / `registerToolSync` 一律收成原名（不加自动检查，评审把关） |
| D16 | 工具侧先脱离 `PollOneBridge` 的轮询路径，桥分两步拆 | 第一步：工具不再走 poll-one；第二步：图节点 / `call_tool` awaiter 搬到 `Task` 驱动后，删 `PollOneBridge` / `PolledRoot` / 主程序侧 `poll_one` |
| D17 | `offload` / `sleep` 改成“提交即返回 ticket”（可 await、名字不变），`whenAll` 泛化 + 变参 | 与 `tableOp` 一致，让“并行多段阻塞工作 / 多段延时”也能 `whenAll`；不再需要第二个 `*Ticket` 名字；完成回调在插件线程上直接 `resume()`（不依赖桥）。实施与验证见 [plan.md](plan.md) §3.10 |

## 结论

现状调查与改造点见 [plan.md](plan.md) §2（含 §2.14 的补审计）/§3，进度见 [work.md](work.md)。

一句话概括这次重构的取舍：
**框架只提供"提交 + 回调"的原语与通用表；端相关的表、便捷函数、句柄解析由应用按端各写一份；
生命周期收敛成"异步 create（含注册）→ 挂载事务 → 异步 stop → 异步 destroy"，因此不再需要
"create 不能读表/起线程"这类无法执行的规定；线程模型收敛成"每端一个管理器（启动即建）+ 一条
插件线程 + 一个池（默认 3 条，两端同值）"，其余子系统一律借用这个池。**
已实现但仍有遗留的事项写在 [work.md](work.md) 文末"剩余工作"：
`plugin_runtime` 的并发压测断言偶发抖动（未处理）、kit 内部 4 处同步入口（有意保留）。
