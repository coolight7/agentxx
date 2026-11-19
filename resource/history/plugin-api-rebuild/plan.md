# 插件接口重构 —— 设计与实施方案（第二轮：通用化 + 入口协议统一）

## 1. 背景

第一轮方案（原 `plugin-async-api-unify`）解决的是"插件线程上还剩同步等 io"和"同一件事两种写法
导致静默降级"。本轮按评审意见扩大范围，把五件结构性问题一起处理：

1. **入口协议不统一**：`get_info` 由插件分配并返回指针；`create` 同步、`start` 异步，两段生命周期
   语义重叠；`create` 的"只构造、不起线程"契约在实际插件里被普遍违反；`destroy` 虽然约定"只释放
   本地对象"，仍在 io 线程上同步执行插件代码。
2. **框架夹带端概念**：`cxx_pluginxx` 是通用插件框架，但注释、示例、宏文档都按"agent 侧 / client 侧"
   写；通用表 IID 只有两段（`agentxx.log`），应用业务表是多段（`agentxx.agent.tools`），读者分不清
   "哪张是框架给的、哪张是应用自己的"。
3. **命名分组不一致**：入口符号是 `<应用名>_plugin_<端>_*`，业务表是 `<应用名>.<端>.*`，一个按
   "plugin 在前"、一个按"端在前"；表 IID 用点分，而 C 侧符号一律下划线。
4. **线程池分散**：`AgentHost` 一个池、每个 `AgentContext` 还带一个默认池、client 侧两个池
   （`pool_` 与 `workerPool_`）、插件管理器又有一层自己的回退池。
5. **构造时机不一致**：agent 侧管理器在 `loadPluginsAsync` 里懒建，client 侧在装配阶段建但线程与
   池仍懒启；没配插件的进程与配了插件的进程跑的是两套形态。
6. **工具体分了四档、两套协程驱动**：`tool`（`Task` 体，经桥 poll-one 驱动）、`polled_tool`
   （`asio::awaitable` 体，靠插件自持 reactor）、`fast_tool` / `blocking_tool`（同步体）。
   跑法与线程各不相同，还带着一套“插件私有 reactor + 主程序 `poll_one`”的旧机制（§2.15）。

前提：最低支持版本刚刚抬过，仓库外没有第三方插件，因此本次重构**不保留任何兼容层**。

## 2. 现状调查（文件与函数一级）

### 2.1 线程与执行器

| 线程 | 创建点 | 跑什么 |
|---|---|---|
| io 线程 | 主程序 | 全部主程序状态、表操作工作体、完成包提交（`PluginRuntime::executor`，`runtime.h`） |
| agent 插件线程 `agentxx-plugin` | `AgentHost::loadPluginsAsync`（`agent/lib/src/agent/agent_host.cpp:362`） | 插件代码（`dispatchToPlugin`，`instance_base.h:479`） |
| agent 工作线程池 | `AgentHost::threadPool`，经 `setWorkerThreadPool` 注入 | `dlopen`/`create`（`runLoaderTask`）、`blocking_tool` 的 offload、文件扫描 |
| client io 线程 | 客户端 | 客户端注册表/会话状态 |
| client 插件线程 `agentxx-client-plugin` | `ClientPluginManager::ensurePluginThread`（`client_plugin_manager.cpp:351`，首次加载插件时） | 客户端插件回调 |
| client UI 线程 | FTXUI | 只读 UI 注册表快照，不调插件代码 |
| 插件自建线程 | 插件 | JS 引擎的 JS 线程（`agentxx_javascript_engine.cpp:2035`）、chatbot 的 `ioThread`（`:237`）、框架 `NetHub` 网络线程 |

约定：io 线程从不阻塞等插件线程；插件线程可以同步等 io（只卡住插件自己）。本次要消掉后一半。

client 侧有两个池：`pool_`（1 线程，管理器构造即建，`client_plugin_manager.cpp:340`，用于
`dlopen`/`create`）与 `workerPool_`（2 线程，懒建，`client_plugin_manager_domain_services.cpp:66`，
供 `postToWorkerThread` → offload 表 / `runLoaderTask`）。

### 2.2 当前生命周期（create → start → mount）

`PluginHostLifecycle::loadNativeAsync`（`pluginxx/host/lifecycle.h:106`）：

1. 解析清单（io）；
2. `dlopen` → `runLoaderTask` → 工作线程；
3. 入口符号 + API 版本校验（io，`info = getInfoFn()`）；
4. `reservePluginName` + 入实例表；
5. `create` → `runLoaderTask` → 工作线程（`:240`）；
6. `start` → `awaitPluginLifecycle`（`op_driver.h:954`）→ `OpCore::startAsync`（`:298`）→
   插件线程；io 协程等完成包；
7. `finishLoad`（`:1045`）→ `Ready`。

挂载（agent 侧）：`mountAgentAsync` → `mountOneAsync` → `awaitPluginLifecycle(mount)` → 插件线程；
卸载：摘注册 → unmount → stop → `waitInflightZero`（有 deadline）→ `destroyPlugin` + `dlclose`
（**在 io 线程内联**）。

### 2.3 表调用的两种形态与同步桥

- 异步（主流）：入口返回 `PluginxxOperatorHandle*`，结果经 `PluginxxOperatorCallback` 恰好一次；
  工作体由 `startTableOp`（`pluginxx/host/table_op.h:82`）在 io 线程执行；完成时 `OpCore::commit`
  （`op_driver.h` 约 690 行）把回调投回插件线程。
- 同步：入口直接 `return`，需要 io 数据的走 `ioCallSyncKeep`（`abi_util.h:191`）——调用线程上
  `future.get()` 等 io。
- 同步桥：`SyncBridgeCallback`（`pluginxx/api/sync_bridge.h`）让 `tableOpSyncWait` 的完成回调在
  完成线程就地调用，避免"插件线程阻塞等自己"。

### 2.4 插件（及插件自建线程）上仍同步等 io 的入口

| # | 表成员 | 位置 | 触发 |
|---|---|---|---|
| 1 | `capabilities.invoke_capability` / `_ex` | `tables_impl.h` 的 `capabilityInvokeCommon`（`ioCallSyncKeep` 在 960 行） | 每次能力调用 |
| 2 | `scheduler.sleep` | `sleepEntry`（`:1059`） | 每次休眠（含 polled 轮询） |
| 3 | `scheduler.offload` | `offloadEntry`（`:1094`） | 每次 `blocking_tool` |
| 4 | `tasks.register_task` | `registerTaskEntry`（`:1203`） | **每次 `ctx.spawn`**（`kit.h:3147`） |
| 5 | `agentxx.agent.session.is_canceled` | `plugin_manager_vtable.cpp:828` | 插件线程轮询；JS 引擎在 **JS 线程**轮询（`agentxx_javascript_engine.cpp:6887`） |

全部 `future.get()`，无超时；io executor 停止时只入待重放队列，调用线程永久等待。

### 2.5 kit 阻塞形态与内置插件使用点

| kit 助手 | 位置 | 使用点 |
|---|---|---|
| `detail::tableOpText` / `tableOpOk` | `kit.h:4493 / :4517` | 配置/清单/参数的阻塞读：codegraph、example_js、example_js_execute_command、example_resources、llm_uncensored、planning（行号见 §2.6） |
| 阻塞广播助手（4 个） | `plugin_kit.h:208/…/289` | chatbot `chatbot_net.cpp:258`、text_selection_monitor `:245` |
| `PluginBaseT::config/argsJson/configPath/language` | `kit.h:3351/3367/3382/3397` | example_runtime `:60`（`argsJson`）、其余对外暴露 |
| `clientTableOp` / `clientTableCall` | `plugin_kit.h:751 / 765` | example_js `:232`（client `create`，用的是阻塞的 `clientTableText`） |
| `tableOpSyncWait` | `kit.h:4385` | 以上全部经它；**框架自己也用**：`blocking_tool` / `polled_tool` 的 `execute_start` 在插件线程上调 `workDir()` 与 `argsJson()` 取运行上下文（`:4778` / `:5263`） |

### 2.6 静默降级（第一轮确认的问题，仍然有效）

`tableOpSyncWait` 在插件执行线程上直接失败（`kit.h:4441`），`tableOpText` 把失败吞成空串：

| 插件 | 调用链 | 线上表现 |
|---|---|---|
| `agentxx_llm_uncensored` | `readArgs`（`:57`）← `uncensoredSetup`（`:91`，挂载协程） | `text`/`key` 读不到 → 永远用默认 |
| `agentxx_planning` | `hostDataDir`（`:191`）← `plansDir`（`:215`）← 工具体（`:345/:489/:534`） | `dataDir` 空 → 规划不落盘、不恢复 |
| `example_resources` | `ownInfoString`（`:51`）← `resAgentSetup`（挂载协程） | 插件路径空 → 运行时 skill 目录算错，注册失败 |
| `agentxx_chatbot` | `ChatbotNet::subscribeEvents`（`chatbot_net.cpp:258`）← `QqGateway::subscribeNetEvents`（`qq_gateway.cpp:62`） | 返回 `nullptr` → 网关收不到网络事件，只留一条 warn |
| `agentxx_text_selection_monitor` | `subscribePluginBroadcast`（`:245`） | 订阅失败 → 功能静默缺失 |
| `example_runtime` | `RuntimeCtx::args()`（`:60`）的 `argsJson()` | 参数恒为 `{}` |

测试没抓到：只有 `test_plugin_executor.cpp` 调了 `setPluginExecutor`（`:244`、`:665`）。

### 2.7 `get_info` 现状

```c
// pluginxx/api/abi.h
typedef struct PluginxxInfo {
    int32_t api_version;   // 插件填（编译期常量）
    uint32_t _reserved;    // 8 字节补齐
    PluginxxStringView name / version / description;
} PluginxxInfo;
```

- 插件侧：`get_info()` **返回指向自己内存的指针**（导出宏用 `static const`，25 个内置插件手写的也都是
  `static const`），框架只读一次；
- 框架侧：`lifecycle.h` 里 `info = getInfoFn()`，版本过低时先复制 `api_version` 再 `close(dl)`；
- client 侧另有一份结构完全相同的 `AgentxxClientPluginInfo`（`client_plugin_api.h:107`）；
- 无释放入口（`entry.h` 只有四个符号名）。**结论：当前不漏**（都用静态对象），但"谁分配、谁释放"没有
  约定，第三个插件若写 `new` 就漏；`_reserved` 字段也浪费了一个可扩展位。

### 2.8 `create` 现状（为什么要合并）

契约（`kit.h:4829` + `plugins.md` §4）：`create` 只构造上下文，不注册、不起线程。

实际：

| 插件 | `create` 里做什么 |
|---|---|
| `agentxx_javascript_engine` | `JS_NewRuntime` + 起 JS 线程（`:10875` → `engine->start()` → `:931`、`:2035`） |
| `agentxx_codegraph` | 构造 `CodeGraphManager`（打开 sqlite，`:867+`） |
| `agentxx_llm_uncensored` / `example_*` | 只查接口表 + 存参数（轻） |

框架靠"把 `create` 投到工作线程"兜住重活，代价：多一次线程切换、"create 里起线程"与契约文字冲突、
以及"create 期允许同步调表"这条特殊豁免（它正是 §2.6 的温床）。

### 2.9 命名现状

| 类别 | 现在 | 位置 |
|---|---|---|
| agent 入口符号 | `agentxx_plugin_agent_get_info/create/start/stop/mount/unmount/destroy` | `plugin_api.h` 的 `AGENTXX_PLUGIN_AGENT_EXPORT`、`PluginManager::entrySymbols()`（`plugin_manager_lifecycle.cpp`） |
| client 入口符号 | `agentxx_plugin_client_*` | `client_plugin_api.h`、`ClientPluginManager::entrySymbols()` |
| 通用表 IID | `<应用名>.<表名>`：`agentxx.log` / `agentxx.capabilities` / `agentxx.scheduler` … | `pluginxx/api/tables.h` 的 `PLUGINXX_APP_ID("log")` 等，应用名由 `PLUGINXX_APP_NAME` 给出（agentxx 设为 `agentxx`） |
| 业务表 IID | `agentxx.agent.*` / `agentxx.client.*` | `plugin_api.h` / `client_plugin_api.h` 的 `AGENTXX_IFACE_*` |
| client 元信息结构 | `AgentxxClientPluginInfo`（与 `PluginxxInfo` 布局相同） | `client_plugin_api.h:107` |

硬编码入口符号名的位置约 200 处，分布在 25 个插件的导出宏、两个管理器的 `entrySymbols()`、
`plugin_kit.h`（24 处）与少数测试（`test_client_plugins.cpp:1` 处）。

### 2.10 框架通用性现状

`cxx_pluginxx` 的**代码**已经是端无关的：`agent`/`client` 只出现在注释与示例里（`abi.h:259`、
`entry.h:7`、`sync_bridge.h:63/65`、`tables.h:7`、`capability_registry.h:13/19`、`lifecycle.h:100/913/955`、
`guard.h:13`、`kit.h:17/418/4378/4380/4826`、`instance_base.h:107`、`manager_base.h:3`、
`plugin_thread.h:8`、`op_driver.h:892`）。也就是说"框架预设了 agent 表和 client 表"是**文档给人的
印象**，不是代码事实；但通用表 IID 只有两段（`agentxx.log`），确实看不出"这是框架给的",
而业务表是三段（`agentxx.agent.tools`）。

### 2.11 kit 头文件现状

`agent/lib/include/agentxx/plugin/api/plugin_kit.h` 约 6100 行，一个文件里混了两端：
`AgentIfaces` / `AgentRef` / `schemaOf` / `fast_tool` / `blocking_tool` / `polled_tool` / 事件与资源便捷函数
（agent）＋ `ClientIfaces` / `ClientPluginBase` / `clientTableOpAsync` / `clientHandleOf` / UI·定时器·快捷键
便捷函数（client）。两端插件都 include 同一个头，编译期互相拖带。

### 2.12 线程池现状（分散的四处）

| 池 | 线程数 | 位置 | 谁用 |
|---|---|---|---|
| `AgentHost::threadPool_` | `threadPoolWorkers`，0 = `hardware_concurrency/2` | `agent_host.cpp:316-320` | 根 agent 与子代理的 `AgentContext::threadPool`（`:238`、`:554`、`:1207`）、插件管理器（`:357`） |
| `AgentContext::threadPool`（成员默认值） | `max(2, hardware_concurrency/2)` | `context.h:1080` | 直接构造 `AgentContext` 的地方（测试、嵌入、后台任务）——**每个上下文各起一个池** |
| client `pool_` | 1 | `client_plugin_manager.cpp:340` | `dlopen`/`create` |
| client `workerPool_` | 2（懒建） | `client_plugin_manager_domain_services.cpp:66-73` | offload 表 / `runLoaderTask` |
| `PluginManager::workerPool_`（回退） | `workers` | `plugin_manager_domain_services.cpp:239-249` | 没注入池时自建（agent 侧） |

除插件之外，这些池还被工具 offload（`toolcall.cpp:1495`）、会话加载（`context.cpp:933`）、
git worktree（`git_worktree.cpp:218/327/427/528`）、媒体附件加载（`media_attachment_loader.h:21`）
借用，因此"归谁所有"直接影响全部子系统。

### 2.13 构造时机现状

- agent 侧：`AgentHost::loadPluginsAsync` 里才 `make_shared<PluginManager>`（`agent_host.cpp:355`），
  然后起插件线程（`:362`）；`BaseAgent::init` 的 `plugin_system` 步骤在没有共享管理器时自建一份
  （`base_agent.cpp:519`，同时设置 `agentContext->threadPool` 作为池）。
- client 侧：`setupClientPlugins`（`mode_runners.cpp:95`）在装配阶段建管理器，但插件线程在
  `loadNativeAsync` 开头才由 `ensurePluginThread` 起（`client_plugin_manager.cpp:351/446`），
  `workerPool_` 又是首次 `postToWorkerThread` 才建。
- 结果：没配插件时既没有线程也没有池；配了插件时线程与池按需出现 —— 两种形态在测试里各覆盖一部分，
  线上只跑其中一种（这正是 §2.6 静默降级能长期存活的原因）。

### 2.14 补充审计：其它仍是同步的接口

把 `tables_impl.h` / 两个 vtable / kit / `plugin_kit.h` 逐个入口对了一遍，除 §2.4 的五个表入口外，
还有下面这些“同步面”。

**(a) 框架级阻塞接口**

`pluginxx::unsubscribePluginBroadcast(sub)`（`broadcast_registry.h:349-383`）：先把 `alive` 置 false，
然后往 io 线程投递簿记，并 **`fut.wait()` 同步等**（无超时）。插件线程 / JS 线程都可以调，
io 停止时永久等待。簿记本身只是“从表里摘除”，而 `alive=false` 已经保证不再回调，
因此这个等待可以改成提交即返回。

**(b) kit 里的阻塞形态（完整清单）**

| 类别 | 函数 |
|---|---|
| 广播 | `declarePluginBroadcast(host, …)` / `publishPluginBroadcast(host, …)` / `subscribePluginBroadcast(host, …)` / `unsubscribePluginBroadcast(host, …)` |
| 权限 | `checkPathDecisions(host, …)` |
| 提示词 / 模型 / 图 / 资源 | `setPromptJson(host, …)` / `modelConfigJson(host, …)` / `setGraphJson(host, …)` / `ownResourcesJson(host, …)` / `resourceOpSync(…)` |
| 工具 | `registerToolSync(…)`；`PluginBase::toolPrompt(tool)` 与 `schema(tool)` 的阻塞重载（`toolPromptOf` / `schemaOf` 是现有协程别名，收口时删掉，只留 `toolPrompt` / `schema` 返回 `Task<…>`） |
| 运行时 | `runtimeCall` / `runtimeCallText` / `takeRuntimeText` |
| client | `clientTableOp/Text/Handle`（底层）、`clientRegisterCommand`，以及 `ClientPluginBase` 的 `clientState` / `argsJson` / `configPath` / `language` / `setLanguage` / `sendUserInput` / `setToolDecor` / `clearToolDecor` / `isRegionVisible` |
| 上下文 | `PluginBaseT::config/argsJson/configPath/language`（`kit.h:3351/3367/3382/3397`） |

**(c) 仍然真阻塞的调用点（线上）**

| 位置 | 调用 | 线程 | 代价 |
|---|---|---|---|
| `agentxx_filesystem.cpp:252` | `checkPathDecisions`（`scheduler` 传 `null`） | **工作线程**（`blocking_tool` 体，move/copy 的 dst 复核，`:705` / `:744`） | 每次 move/copy 占住一条池线程等一次 io 往返；**且因 scheduler 为空绕过了拒绝守卫**（若这段以后改到 `fast_tool` 里就会直接卡插件线程） |
| `agentxx_javascript_engine.cpp:5971` | `publishPluginBroadcast(host, …)` | **JS 线程**（脚本 `agentxx.publish`） | 每次发布一次 io 往返 |
| `agentxx_javascript_engine.cpp:5514` | `unsubscribePluginBroadcast(host, …)` | **JS 线程**（脚本实例收尾） | 每条订阅一次 io 往返 |

**(d) 静默失败（插件线程上被拒，只记日志）**

阻塞形态带 scheduler 时在插件线程上直接失败，返回值被吞成空或 `nullptr`：

| 位置 | 调用 | 后果 |
|---|---|---|
| `agentxx_chatbot/chatbot_net.cpp:258`（← `qq_gateway.cpp:62`） | 阻塞 `subscribePluginBroadcast` | 返回 `nullptr`，网关收不到网络事件（只记一条 warn） |
| `agentxx_text_selection_monitor.cpp:245` | 阻塞 `subscribePluginBroadcast` | 订阅失败，功能静默缺失 |
| `example_runtime.cpp:60` | `argsJson()`（`clientTableText`） | 参数恒为 `{}` |
| codegraph `:46/:61`、`example_js:232/:240`、`example_js_execute_command:239`、`example_resources:61` | `get_config` / `get_plugin_args` / `get_own_info` 的阻塞读 | 已在 §2.6；合并 `create` 后仍在插件线程，同样被拒 |

**纠一处误判**：`agentxx_planning.cpp:1217` 的 `registerToolRenderer` **不是**阻塞调用 —— 它走
`clientTableCall`（`plugin_kit.h:5958`，提交即返回），任何线程都不等，也不受线程策略限制。
原先把它列为“静默失败”是错的，实际不受影响；真正要改的是上表四处。

**(e) 主程序侧 API：不可等的启停**

`PluginHostLifecycle::disable(name)` / `enable(name)`（`lifecycle.h:631/636`）是 fire-and-forget：
内部把停止/启动事务投成协程，调用方无法等，失败只记日志 —— 测试只能轮询（`waitFor`）。
**改为原名返回 `Task<bool>`（不再保留不可等的版本）**，调用方改 `co_spawn` / `co_await`（§3.5 第 8 条）。

**(f) 工具实现体的四档跑法（现状，收口见 §2.15/§3.5）**

工具体当前有四种跑法，与“插件里只能等 kit 原语”这个目标不一致：

- `fast_tool`：同步体，在 `execute_start` 里直接跑 → **插件线程**，既不能阻塞也不能 `co_await`；
- `blocking_tool`：同步体，经 `scheduler->offload` 跑到 **工作池线程** → 可以阻塞，但每次调用占一条池线程；
- `polled_tool`：`asio::awaitable` 体，靠插件自持 reactor + 主程序 `poll_one` 驱动（前提已不成立，见 §2.15）；
- `tool`：`pluginxx::Task` 协程体（已存在，3 处使用）—— 它就是收口后的目标形态。

收口决定（§3.5 / §3.9）：只留 `tool`，阻塞段在协程体里 `co_await offload(...)`；
另一种可选优化是把 dst 这类复核改成“权限声明支持第二个目标”，由框架在执行前统一问一次（§8）。

**(g) 守卫可被绕过**

`tableOpSyncWait` 的 `refuseOnPluginThread` 只在调用方**传了 scheduler** 时生效；`checkPathDecisions`
传 `null` 就绕过了（见 (c) 第一行）。改造后裸 host 形态应删除，阻塞形态只能从上下文取 scheduler。


### 2.15 现有 tool 注册类型（清单）

| # | 入口 | 定义 | 体类型 | 执行线程 | 内置插件使用 | 用途 |
|---|---|---|---|---|---|---|
| 1 | `tool(ctx, agent, name, depict, schema, fn, timeoutMs, flags)` | `plugin_kit.h:4460` | `pluginxx::Task<ToolResult>`（`fn(ctx, args, OpCtl)`） | 插件线程，挂起后不占线程 | 3 处（`example_plugin`） | **现有唯一的协程体入口**，带 `OpCtl` 取消 |
| 2 | `fast_tool` | `:4596` | 同步返回 `ToolResult` | 插件线程（不能阻塞、不能 `co_await`） | 10 处 / 6 插件（math、planning、system、example_*） | 纯计算 |
| 3 | `blocking_tool` | `:4702` | 同步返回 `ToolResult` | 工作池线程（经 `scheduler.offload`） | 37 处 / 12 插件（chatbot 10、filesystem 10、codegraph 5…） | 阻塞 IO、子进程、目录扫描 |
| 4 | `polled_tool` | `:5138` | `asio::awaitable<ToolResult>` | 插件线程，受控 `poll_one` 驱动（桥的本地执行器） | 6 处 / 3 插件（filesystem 3、execute_command 2、example_plugin 1） | 插件本地 reactor 的内核就绪等待 |
| — | `registerToolSync`（低层，直接传 `AgentxxPluginToolSpec`） | `:4410` | — | 调用方线程（阻塞） | 0（插件作者不用；kit 内部统一走 `registerToolTicket`） | 只能给特殊需求用 |

补充事实：

- 四个入口最终都调 `detail::registerToolTicket`，差别只在“体怎么跑、跑在哪条线程”；
- `polled_tool` 的体**已经是协程**（`asio::awaitable`），不是同步体；
- kit 的 `pluginxx::offload(ctx, work)`（`kit.h:4223`）已能把一段同步工作投到工作池并 `co_await`，
  因此 `blocking_tool` 的语义可以在协程体里完整表达；
- 图节点那边已经是“一个入口、按返回值选执行方式”（`invokeGraphNode`：快同步体返回 `std::string`，
  协程体返回 `Task<T>`），可作为工具侧的先例。

**为什么会有两个协程实现（ `tool` 与 `polled_tool` ）**：

| 项 | `tool` | `polled_tool` |
|---|---|---|
| 体的协程类型 | `pluginxx::Task<ToolResult>`（kit 自造 promise） | `asio::awaitable<ToolResult>` |
| 驱动 | `detail::startBridgedRoot(ctx->bridge(), …)`（`:4569`） | `asio::co_spawn(bridge->local_executor(), …)`（`:5283`） |
| 能 `co_await` 什么 | kit 原语（`sleep` / `offload` / `call_tool` / `invoke_cap`） | asio 原语（socket / 管道 / `steady_timer`） |

两者**都挂在 `PollOneBridge` 上**：桥给插件一段“本地 reactor”，由主程序经 `poll_one` + 退避定时器
推进。这个设计的前提是“插件代码跑在主程序 io 线程上、自己没有 reactor，主程序只看得到 `poll_one` 这一步”——
而 `PluginExecutorThread`（`pluginxx/runtime/plugin_thread.h:47-56`）**已经是一条完整 `io_context` 的
`run()` 线程**，插件代码在上面直接 `asio::co_spawn` / 定时器 / socket 都成立，因此 
“无可见唤醒源”这个前提不再存在——两个实现可以合并，桥也可以逐步退场。

## 3. 方案

### 3.0 对评审意见的判断

| 意见 | 判断 | 理由与注意事项 |
|---|---|---|
| 不保留旧版本兼容，彻底重构 | **采纳** | 外部插件为零，仓库内 25 个可一次迁移。注意：必须**分步、每步可运行**，但每步都不写兼容层；代次一次性抬到位；kit 也不留迁移期转发头 |
| 入口符号改 `agentxx_agent_plugin_*` / `agentxx_client_plugin_*` | **采纳** | 与业务表 `agentxx_agent_*` / `agentxx_client_*` 分组一致；改动集中在两个 `entrySymbols()` 与导出宏，约 200 处但基本都是宏与注释 |
| 表 IID 统一用下划线 | **采纳** | 通用表 `<应用名>_plugin_*`、业务表 `<应用名>_<端>_*`。注意：**能力 id 与广播 id 不动**（仍是点分 `agentxx.agent.*`，事件命名规则要求点分），因此框架要提供两个拼法：`PLUGINXX_TABLE_ID`（下划线）与现有的 `PLUGINXX_APP_ID`（点分）。业务表 IID 也一起改，改动面比"只加 plugin 段"大 |
| pluginxx 移除 agent 标记、两端复用代码各自一份 | **采纳（需限定范围）** | 代码层已无端逻辑，主要是：① 通用表编号加 `plugin` 前缀；② 注释/示例去端化；③ kit 按端拆三份。注意："作用域（scope ref）"要保留，它不等于 agent |
| `get_info` 改"框架给结构体、插件填写" | **采纳** | C ABI 常规做法，彻底消除"谁分配、谁释放"；顺带把 `_reserved` 改成 `min_api_version`；client 侧删掉重复的 `AgentxxClientPluginInfo` |
| 合并 `create` 与 `start`，且 `create` 返回 `void` | **采纳** | 两步语义重叠，`create` 的"轻量"契约被普遍违反；合并后只剩一条失败路径。三点配套：① create 在插件线程执行，注册就在里面 `co_await`；② `plugin_ctx` 走 `void**` 出参且**必须同步写出**，`notify` 可稍后；③ 失败时由框架调 `destroy` 收尾（插件不要自己 destroy） |
| 启用重新注册用**可重入 create** | **采纳** | 同一实例第二次 `create` 只做注册、不重建 ctx（看 `*plugin_ctx` 是否为空）；`stop` 只撤注册与自管资源，ctx 保留。比另加 `reload` 入口少一个符号，也不必再教"两个入口的区别" |
| `destroy` 也改异步 | **采纳** | 与 `create`/`stop` 同一协议；插件在插件线程释放本地对象并 notify，框架收到完成后再把 `dlclose` 丢到工作线程（不再占 io 线程，`dlclose` 也可能慢）。注意：destroy 之后 ctx 指针即失效，框架要先把所有引用清掉 |
| 两侧管理器启动即构造、每端一个池（默认 3 条，两端同值）、别处不再建池 | **采纳** | 线程形态只剩一种，测试与线上一致。四项连带工作：① 配置键改名 `plugin.thread_pool_workers`（默认 3，不分开配，同步 config-keys 与 `config_keys`）；② 删 `AgentHost::threadPool_`，`AgentContext::threadPool` 改为指向管理器的池且**默认值为空**；③ client 的 `pool_` + `workerPool_` 合成一个；④ `dlopen`/`create`/工具 offload/会话加载/git worktree/媒体附件全部改取管理器池 |
| kit 不保留兼容迁移头 | **采纳** | 三个头一次性切换，25 个插件同时改 include；不留"全部包含"的转发头，避免长期两个入口 |

### 3.1 总原则

1. **C API 只表达"提交 + 回调"**：任何成员都不阻塞调用线程；
2. **生命周期只有一个协议**："调用 → 恰好一次 notify"，异常由 SDK 兜住并转成 failed；
3. **框架不认识端**：只认"应用名 + 作用域（scope ref）"；
4. **端相关的代码各写一份**：业务表、便捷函数、句柄解析放在 `agentxx` 的 agent / client 两份头里；
5. **不做兼容层**：旧形态删除，API 代次抬到位，全部插件重编。
6. **名字不标异步**：默认就是异步 —— 要结果就是协程（原名返回 `pluginxx::Task<…>` / ticket），
   不要结果就是提交即返回（原名，`void`/`bool`）；**不新加 `*Async` 后缀**，现存的
   `configAsync` / `argsJsonAsync` / `clientTableOpAsync` / `registerToolSync` 这类成对命名一并收掉，
   同一件事只留一个名字；确实需要强调“只是发出去不等”的地方用已有的 `*NoWait` 标记；
   `Sync` / `Async` 只允许出现在**删除名单**里（`tableOpSyncWait` / `resourceOpSync` / `ioCallSync*`…）。

### 3.2 命名与符号（映射表）

| 现有 | 新的 |
|---|---|
| `agentxx_plugin_agent_get_info/create/start/stop/mount/unmount/destroy` | `agentxx_agent_plugin_get_info/create/mount/unmount/stop/destroy`（`start` 并入 `create`） |
| `agentxx_plugin_client_get_info/create/start/stop` | `agentxx_client_plugin_get_info/create/stop` |
| 宏 `AGENTXX_PLUGIN_AGENT_EXPORT` / `AGENTXX_PLUGIN_CLIENT_EXPORT` | `AGENTXX_AGENT_PLUGIN_EXPORT` / `AGENTXX_CLIENT_PLUGIN_EXPORT` |
| 宏 `AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT` / `..._CLIENT_...` | `AGENTXX_AGENT_PLUGIN_LIFECYCLE_EXPORT` / `AGENTXX_CLIENT_PLUGIN_LIFECYCLE_EXPORT` |
| 通用表编号 `agentxx.log` / `agentxx.capabilities` / `agentxx.scheduler` / `agentxx.tasks` / `agentxx.cancel` / `agentxx.config` / `agentxx.plugins` / `agentxx.broadcasts` / `agentxx.json` / `agentxx.net` | `agentxx_plugin_log` / `agentxx_plugin_capabilities` / … / `agentxx_plugin_net`（下划线） |
| 业务表编号 `agentxx.agent.*` / `agentxx.client.*` | `agentxx_agent_*` / `agentxx_client_*`（下划线，例 `agentxx_agent_tools` / `agentxx_client_ui`） |
| `AgentxxClientPluginInfo` | 删除，两端统一用 `PluginxxInfo` |

编号宏分两个：`PLUGINXX_TABLE_ID(name)` 拼下划线（`<应用名>_<name>`，通用表传 `"plugin_log"`
一类），能力与广播 id 继续用点分的 `PLUGINXX_APP_ID(name)`（`<应用名>.<name>`）；
agentxx 侧对应 `AGENTXX_TABLE_ID(name)`（`agentxx_agent_tools` 一类）。**能力/广播 id 不改**。

### 3.3 框架通用化

1. 表编号改下划线（见上表）：`tables.h` 的编号宏换成 `PLUGINXX_TABLE_ID("plugin_log")` 一类，
   适配宏与业务表头同步；点分的 `PLUGINXX_APP_ID` 只给能力 / 广播 id 用；
2. 注释与示例去端化：`<主程序前缀>_plugin_agent_*` → `<应用名>_<端>_plugin_*`；示例线程名 `agentxx-plugin`
   → `plugin`；`per-agent` → `per-scope`；"client 侧" → "界面侧/另一端"；
3. 去端化的写法写进 `plugins.md` 的框架约定（人工评审把关，不新增 `boundaries` 自动检查）：
   `cxx_pluginxx` 的源码与注释里不出现 `agent` / `client` 字样；`agentxx` 的两端表不反向进入 pluginxx；
4. `plugins.md` 把"框架负责 10 张通用表 + 应用负责业务表"写成明确的职责划分，并说明两端表是
   应用自己按端各写一份。

### 3.4 入口协议

**(a) `get_info` 改成填表**

```c
/* struct_size 由框架写; 插件只写 name/version/description 与 min_api_version */
typedef struct PluginxxInfo {
    int32_t            api_version;      /* 框架写: 主程序支持的版本, 插件只读 */
    int32_t            min_api_version;  /* 插件写: 要求的最低主程序版本 (0 = 不检查) */
    uint32_t           struct_size;      /* 框架写: sizeof(PluginxxInfo) */
    uint32_t           flags;            /* 插件写: 预留, 填 0 */
    PluginxxStringView name;             /* 插件写: 唯一标识 */
    PluginxxStringView version;          /* 插件写 */
    PluginxxStringView description;      /* 插件写 */
} PluginxxInfo;

int32_t PLUGINXX_CALL agentxx_agent_plugin_get_info(PluginxxInfo* out);
```

框架侧：栈上构造、填好 `api_version` / `struct_size`，调一次，校验
`rc == 0 && out->name 非空 && out->min_api_version <= PLUGINXX_API_VERSION`，随后**立即复制**
name/version/description 到自己的字符串里（插件侧的视图只保证本次调用期间有效）。

好处：插件不分配内存、没有 `static` 生命周期假设、插件能读 `api_version` 做运行时降级、
`struct_size` 支持后续追加字段。

**(b) `create` 与 `start` 合并**

```c
/* agent 侧: 构造 + 注册合为一体, 在插件线程执行; *plugin_ctx 必须同步写出, 返回 void */
void PLUGINXX_CALL agentxx_agent_plugin_create(
    const PluginxxHost*           host,
    void**                        plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
);
```

- 插件在入口里 `new Ctx` + `init(host)`，写出 `*plugin_ctx`，然后要么直接 `notify->done(OK)`（同步完成），
  要么 `finishLater(notify)` + `ctx.spawn(...)`，注册完再 `later.ok()` / `later.failed(原因)`；
- 框架在插件线程驱动它并协程等完成包；失败时调 `destroy(ctx)`、关库句柄；
- 返回值的用法：**只有 `mount` 返回 per-agent 的 user data**（否则 `unmount` 拿不到它）；
  `create` / `stop` / `destroy` 一律 `void`，`plugin_ctx` 走 `create` 的出参。

| 端 | 入口 | 签名要点 |
|---|---|---|
| 两端 | `get_info` | `int32_t f(PluginxxInfo* out)`（填表） |
| 两端 | `create` | `void f(host, void** ctx, notify, err)`；构造 + 注册，可重入 |
| 两端 | `stop` | `void f(void* ctx, notify, err)`；撤注册与自管资源，ctx 保留，可重复 |
| 两端 | `destroy` | `void f(void* ctx, notify, err)`；释放本地对象，完成后 ctx 失效 |
| agent | `mount` | `void* f(void* ctx, const MountSpec* spec, notify, err)`；返回 mount user data |
| agent | `unmount` | `void f(void* ctx, void* mount_ud, notify, err)` |

`dlclose` 不在 `destroy` 里做：框架收到 destroy 完成后，把关句柄投到管理器的工作线程（`dlclose`
也可能慢），io 侧协程等它 —— 与 `dlopen` 对称。

`OpCore` 的"provider handle / 取消句柄"概念在生命周期上不再需要：协议是 `void` + notify，
框架只等一个完成包。这条改动同时简化 `awaitPluginLifecycle` 与 `instance_base` 的 drive 结构。

**(c) 生命周期状态机**

```
(无实例) --create 成功--> Loading(构造 + 注册完成) --> Ready
Ready/Disabled --stop(撤注册, ctx 保留)--> Stopped --create(重入, 只做注册)--> Ready
Stopped/Ready --destroy(异步)--> Destroyed --[dlclose on 工作线程]--> (释放)
create 失败 / stop 失败: 统一回滚 = 撤销已生效注册 → destroy → dlclose
```

补充：禁用/启用仍走 `stop`（清注册、释放自管资源、ctx 保留）→ 再 `create`（重新注册）。
**已定：`create` 可重入，不另加 `reload` 入口。** 同一实例第二次 `create`：

1. 插件看 `*plugin_ctx` 是否为空：非空就跳过 `new Ctx` + `init(host)`，只做注册；
2. 框架在 `create` 前把 `*plugin_ctx` 置为已有值（不是 `nullptr`），插件据此区分"首次"与"重入"；
3. 重入失败（注册被拒）按普通失败处理： `notify->failed(原因)`，框架撤销本次已生效的注册，
   ctx 保留（保持 `Stopped`），下次启用可以再试。

### 3.5 表成员与阻塞助手收口（原名即协程）

目标：**插件线程与 JS 线程上没有任何一处会等 io**。清单来自 §2.4（表入口）与 §2.14（框架、kit、
线上调用点），按下表逐项处理，顺序即实施顺序。

**表入口**

1. 4 个“同步受理”入口改纯提交（`capabilityInvokeCommon` / `sleepEntry` / `offloadEntry` /
   `registerTaskEntry`）：受理动作在调用线程完成（`OpCore` 的操作编号原子化、实例登记线程安全化），
   工作体 `postToIo`；
2. `is_canceled` 改读快照：io 线程维护"会话 → 是否已取消"，插件任意线程直接读；
3. `ctx.spawn` 不再同步登记：`register_task` 改提交形态，取消靠本地 `cancelFlag` + 取消注册表。

**框架级与 kit**

4. `pluginxx::unsubscribePluginBroadcast` 去掉 `fut.wait()`：先把 `alive` 置 false（此后不会再回调），
   再提交簿记并立即返回；簿记是幂等摘表，重复摘不报错。JS 引擎 `:5514` 与各插件因此不再有往返；
5. kit 的阻塞形态整组删除（清单见 §2.14(b)），每个函数只留一个名字，**不新加 `*Async`**：
   原来成对出现的“阻塞版 + `*Async` 版”，统一成**原名即协程**（返回 `Task<…>` / ticket）；
   本来就是“只发出去不等”的保持原名；需要标明后者的用已有 `*NoWait`：

| 类别 | 现在的两写 | 收成一种 |
|---|---|---|
| 广播 | `declarePluginBroadcast(host,…)` / `publishPluginBroadcast(host,…)` / `subscribePluginBroadcast(host,…)` / `unsubscribePluginBroadcast(host,…)`（阻塞） | `declare`/`subscribe`/`unsubscribe` 同名返回 `Task<…>`；`publish` 本来就是提交即返回，保持 `void` |
| 权限 | `checkPathDecisions(host,…)` | 同名返回 `Task<std::vector<PathDecision>>` |
| 提示词 / 模型 / 图 / 资源 | `setPromptJson(host,…)` / `modelConfigJson(host,…)` / `setGraphJson(host,…)` / `ownResourcesJson(host,…)`；`resourceOpSync` + `resourceOpAsync` | 都叫原名，返回 `Task<…>`（`resourceOpSync` 删掉，`resourceOpAsync` 改回 `resourceOp`） |
| 工具 | `registerToolSync`（kit 内部另有一个 `detail::registerToolTicket`） | 只留 `registerTool`（`Task<int32_t>`）；`*NoWait` 形态需要时再补 |
| 工具提示词 / 模式 | `toolPrompt(tool)` 阻塞重载 + `toolPromptOf(ctx,…)`；`schema(tool)` + `schemaOf(ctx,…)` | 只留 `toolPrompt(tool)` / `schema(tool)`，返回 `Task<…>`；`*Of` 两个别名删掉 |
| 运行时 | `runtimeCall`（ticket）+ `runtimeCallText` / `takeRuntimeText`（阻塞助手） | `runtimeCall` 保持 ticket / `Task<…>`；两个文本助手删掉，结果文本从 `TableOpResult::text()` 取 |
| client | `clientTableOp/Text/Handle` + `clientTableOpAsync`；`clientRegisterCommand`（阻塞） | `clientTableOp`（返 ticket）/ `clientTableText` / `clientTableHandle` 就是协程；`clientTableOpAsync` 名字删掉；`clientRegisterCommand` 同名返回 `Task<bool>`；`clientTableCall`（提交）不动 |
| client 实例 | `ClientPluginBase` 的 `clientState` / `argsJson` / `configPath` / `language` / `setLanguage` / `sendUserInput` / `setToolDecor` / `clearToolDecor` / `isRegionVisible` 的阻塞重载 | 同名返回 `Task<…>`，现存的 `clientStateAsync` / `argsJsonAsync` 一类名字全收掉；`registerToolRenderer` 本来就是提交即返回（`clientTableCall`），不动 |
| 上下文 | `PluginBaseT::config/argsJson/configPath/language`（阻塞） + `*Async` 版 | 同名返回 `Task<std::string>`，`*Async` 名字删掉 |
| 会话 | `workDir(scope,…)`（阻塞） + `workDir(ctx,…)` | 只留 `workDir(ctx, agent,…)`（返 ticket） |

6. 阻塞守卫不能绕过：删掉“裸 `host` + `scheduler` 为 `null`”的入口（§2.14(g) 就是从这里绕过的），
   助手一律从上下文取 scheduler，插件源码写不出“不传 scheduler”的调用；这是代码层面的根除，
   **不加 `boundaries` 自动检查**（命名与入口数量都由代码评审把关）；
7. JS 引擎两处线上阻塞改掉：脚本 `agentxx.publish`（`:5971`）返回 Promise，发布完成后 resolve；
   实例收尾的退订（`:5514`）改提交即返回（本来就不看结果）。

**主程序侧**

8. `PluginHostLifecycle::disable` / `enable`（`lifecycle.h:631/636`）改成**原名返回 `Task<bool>`**：
   等停止/启动事务真的走完，失败能从返回值看到；不再保留不可等的版本，调用方改 `co_spawn` / `co_await`。

**删同步桥**

9. 删 `sync_bridge.h`、`OpCore::commit` 的 `isSyncBridgeCallback` 分支、`SyncWaitThreadPolicy`、
   `tableOpSyncWait` / `tableOpText` / `tableOpOk`、`abi_util.h` 的 `ioCallSync*`、
   `tables_impl.h` 的 `onInstanceIo/queryStringIo`（死代码）。这一步必须在 1~3 之后：
   同步桥正是它们（以及 create 期查询）当前的实现手段。

**插件迁移**

10. `agentxx_filesystem.cpp:252` 的 dst 复核改 `co_await checkPathDecisions(...)`（同名，已是协程）——
    工具体因此需要协程体，见下面的决定；
11. `agentxx_chatbot`（`chatbot_net.cpp:258` ← `qq_gateway.cpp:62`）与
    `agentxx_text_selection_monitor`（`:245`）的阻塞 `subscribePluginBroadcast` 改
    `co_await subscribePluginBroadcast(ctx, …)`：现在插件线程上被拒、返回 `nullptr`，网关收不到网络事件；
12. `example_runtime.cpp:60` 的 `argsJson()` 改 `co_await argsJson()`（同名，已是协程；现在恒为 `{}`）；
13. 剩下几处阻塞读全部改 `co_await`：`llm_uncensored` 的 `readArgs`（`:57`）、`planning` 的
    `hostDataDir`（`:191`）、`example_resources` 的 `ownInfoString`（`:51`），以及 codegraph
    `:46/:61`、`example_js:232/:240`、`example_js_execute_command:239`、`example_resources:61`
    的 `get_config` / `get_plugin_args` / `get_own_info` —— 它们今天在插件线程上就已经退化成默认值
    （§2.6），合并 `create` 后同样，必须改。
14. 框架自己也不再回查：工具派发前，框架现在在插件线程上调 `workDir()` / `argsJson()`（§2.5），
    改成由主程序派发时把这两项写进工具运行 spec（新增两个可选字段，读取侧用 `specHas` 判定），
    插件侧不再为拿运行上下文而查表。

> 通用写法：工具/回调里要的配置与清单值，一律在 `create` 里读一次存进实例上下文（`create` 已是协程），
> 不要每次调用再查一遍 —— 这既是 §2.6 那些“降级为默认值”的根除办法，也避开了“回调里没协程”的问题。

**工具体（已定）**：**只留一个 `tool`，体是 kit 的 `pluginxx::Task<ToolResult>`**，靠“登记完成源 +
完成回调里 resume”驱动（完整机制见 §3.9）。做法：

- `tool` 保持现有形状（协程体 + `OpCtl` 取消），不再经 `startBridgedRoot` 的 poll-one 路径；
- 删 `fast_tool`（10 处，迁到 `tool`：体改协程，末 `co_return` 结果）；
- 删 `blocking_tool`（37 处）：体改协程，阻塞段 `co_await pluginxx::offload(ex, …)`；
- 删 `polled_tool`（6 处）：它依赖插件自持 asio reactor，与“插件线程不跑 `io_context`”直接矛盾；
  三处文件读、两处子进程改为 `offload`，示例的定时器改为 `sleep`（表见 §3.9）；
- 删 `registerToolSync` / `registerToolTicket` 的对外暴露（kit 内部直接用表入口）。

关于“`blocking_tool` 同步返回是不是阻塞了调用线程”：**体不阻塞** —— `execute_start` 把工作交给
`scheduler.offload` 就 `return job`，体在工作池线程上跑；真正卡插件线程的是 `execute_start` 里那两次
`workDir()` / `argsJson()` 阻塞读（§3.5 第 14 条），改造后由主程序派发时带进来。改成协程体的意义是
“不再需要 `blocking` / `fast` / `polled` 三种跑法”，不是修复一个阻塞 bug。

`PollOneBridge` 的 **poll_one 循环部分**本次可以退：工具侧不再用它；剩下的图节点 / `call_tool` awaiter
在第二步搬到 `Task` 驱动后一并删掉（§3.9 末尾）。

`blocking_tool` 现有 37 处是机械改造（包一层 `co_await offload`），但不建议在一步里改完：
先改 filesystem（10 处 `blocking_tool` + 3 处 `polled_tool`，含 dst 复核）验证模式，再批量套。
另一种修法（把 dst 复核变成“权限声明支持第二个目标”，由框架执行前统一问一次）与之不冲突，
两项均可选。

### 3.6 线程与线程池模型（统一到管理器）

**每端一个管理器，启动即构造，各自持有一条插件执行线程 + 一个工作池（默认 3 条，可配）。**

```
agent 端:  AgentHost::create
             └─ PluginManager(primaryAgent)            // 构造即起
                  ├─ 插件执行线程  "agentxx-plugin"      (1 条)
                  └─ 工作池 asio::thread_pool(plugin.thread_pool_workers)  // 默认 3 条
client 端: 客户端装配阶段 (mode_runners)
             └─ ClientPluginManager(ex)                // 构造即起
                  ├─ 插件执行线程  "agentxx-client-plugin"
                  └─ 工作池 asio::thread_pool(3)
```

- **删**：`ClientPluginManager::ensurePluginThread`、client 的 `pool_` 与 `workerPool_`、
  `AgentHost::threadPool_`、`PluginManager` 的"回退池"（`plugin_manager_domain_services.cpp:249`）；
- **配置键改名并统一**：`AgentHost::Config::threadPoolWorkers` → `plugin.thread_pool_workers`
  （yaml 风格与其他段一致，如 `async_work.max_concurrent`），**只有一个值、两端共用**，默认 `3`；
  `0` 也按 `3` 处理（不再用 `hardware_concurrency/2`）；归属列标为 **agent + client 共用**
  （客户端建池时读同一个键），需同步 `config-keys.md` 与 `config_keys` 测试；
- **一个端只有一个池**：agent 端与 client 端的全部需要线程池的地方（插件框架、工具 offload、
  会话加载、git worktree、媒体附件、`dlopen`/`dlclose`）都取各自管理器的那一个池，数量不对端分开配；
- `AgentContext::threadPool` 默认值改为空，**不再自带池**；`AgentHost` 把管理器的池填进去，
  直接构造 `AgentContext` 的场景（测试、嵌入、后台任务的子上下文）一律从 `pluginManager` 取。
  现有借用方保持不变：`toolcall.cpp:1495`、`context.cpp:933`、`git_worktree.cpp` 四处、
  `media_attachment_loader`、`base_agent.cpp:1775`；
- 池的用途因此合并：`dlopen`/`dlclose`（`runLoaderTask`）、`offload` 表 / `blocking_tool`、
  `postToWorkerThread`、工具 offload、会话加载、git worktree、媒体附件；
- 保留 `setPluginExecutor` / `setWorkerThreadPool` 作为测试与嵌入方的注入点，但应用侧不再有
  "没有插件线程 / 没有池"的分支；
- 代价：没配插件的进程也常驻 1 条执行线程 + 1 个池（默认 3 条，这是"两端统一、启动即建"的直接成本）；
- 测试夹具统一带插件线程与池（新增 `agent/test/plugin/plugin_thread_fixture.h`），插件用例
  与线上形态一致 —— 这也是 §2.6 静默降级不再复发的保证。

### 3.7 kit 拆分与收口

| 文件 | 内容 |
|---|---|
| `agentxx/plugin/api/plugin_kit.h` | 通用：`PluginBase`、`finishLater`、`spawn`、`SchemaBuilder`、`ArgReader`、`PluginString`/`PluginStringView`、`guardCall`、错误码、`ScopeRef` |
| `agentxx/plugin/api/agent_plugin_kit.h` | agent 侧：`AgentIfaces`、`AgentRef`、工具（`tool`）、`schema`、事件/资源/提示词/图/运行时便捷函数、`AgentPluginBase` |
| `agentxx/plugin/api/client_plugin_kit.h` | client 侧：`ClientIfaces`、`ClientPluginBase`、`clientTableOp`/`clientHandleOf`、UI/定时器/快捷键便捷函数 |

形态只剩三种：`tableOp`（ticket / `co_await` / `whenAll`）、`tableOpCallback`、`*NoWait` 提交。

拆头**一步到位**：不保留 `plugin_kit.h` 作为"包含全部"的转发头，25 个插件的
`#include "agentxx/plugin/api/plugin_kit.h"` 同时改成按端包含（双端插件如 `example_js` 两个都包含）。
好处：从第一版代码就只有一套包含关系，不用在下一轮再拆一次；代价：拆头那一步的编译错误面最大，
单独一步提交。

### 3.8 版本与迁移

- `AGENTXX_PLUGIN_API_VERSION`（agent）7 → 8；`AGENTXX_CLIENT_PLUGIN_API_VERSION`（client）8 → 9；
  `PLUGINXX_API_VERSION` 仍按现有约定保持 1（agentxx 编译期覆盖）；
- 表编号全部换成下划线：通用表 10 张 + agent 业务表 10 张 + client 业务表 6 张，共 26 个编号；
  逐个抬 `tables.h` / `plugin_api.h` / `client_plugin_api.h` 的 `_VERSION`（成员未变时可只换 IID，
  但仍建议同步抬版本，避免“版本相同、编号不同”的静默不兼容）；
- **能力 id 与广播 id 不变**（仍点分）：`agentxx.agent.*` / `agentxx.client.*` / `plugin.<名>.*`；
- 不做双 IID、不做旧符号别名；全部插件重编（`agent/script/build_plugins.ps1`），测试与文档同步。

### 3.9 协程的切换与唤醒（合并后的 `tool` 怎么跑）

先回答一个前提问题：**插件里不应该出现 `io_context` / `co_spawn` / `run()`**。

- 插件线程（`PluginExecutorThread`）的循环归框架：`context->run()` 已在 `plugin_thread.h` 里；
- 插件里再建一个 `io_context` 并 `run()`，会**占住唯一一条插件线程**，框架派发、其它插件的回调全部饿死；
- 而且 `asio::awaitable` 的体必须由一个执行器 `co_spawn` 才能起步，而 **C ABI 传不了
  `asio::any_io_executor`** —— 插件只能自己建执行器，又回到上一条。

所以合并后的工具体是 **kit 的 `pluginxx::Task<ToolResult>`**（不是 `asio::awaitable`），驱动与唤醒全部
走“向主程序登记完成源 + 完成回调里 resume”，不靠插件自持 reactor。

**一条工具调用的完整时序**（以 filesystem 的 `agentxx_filesystem_read` 为例）：

```
主程序 io 线程            插件线程(框架循环)                     工作池(3 条)
─────────────────────────────────────────────────────────────────────
工具调用进来
  OpCore 建操作
  dispatchToPlugin ──────►  执行 spec.execute_start
                             └─ kit: startTaskRoot(body)   ← 原 `startBridgedRoot` 去掉 poll-one；首步直接跑
                                  co_await offload(读文件) ──┐
                             （帧挂起，execute_start 返回）    ├─ 工作线程读文件
   ◄───────────────────────────────────────────────────────┘
  offload 完成
  回调投回插件线程 ─────►  resume() 续跑帧
                             构造 ToolResult
                             notify->done(状态, 正文, 元数据)   ← 跨 ABI 就这一步
   ◄───────────────────────
  OpCore 收尾 → 回给 LLM
```

要点：

1. **挂起 = 向主程序登记一个完成源**：`sleep`（主程序计时器）/ `offload`（工作池）/ 任意表操作的
   完成回调（`request_driver` 票据）。全是 C ABI 的“函数指针 + user_data + 回调”。
2. **唤醒 = 完成回调里 `handle.resume()`**：完成回调由框架 `dispatchToPlugin` 投回插件线程（FIFO），
   所以 resume 一定在插件线程上跑，插件代码永远只看到一条线程。
3. **切换点只在 `co_await` kit 原语处**：帧一挂起，控制权就回到框架循环，循环去跑下一条就绪回调
   （别的插件、同一插件的另一条工具、广播/定时器/命令）—— 多个插件在同一条线程上自然交错，
   不靠任何插件内的事件循环。
4. **公平性**：一帧一次 resume 只推进有限步（现有的 driver 协议已这么规定，`request_driver` 注释里写着）；
   长倒在循环里的可观测信号是 `pluginCallbackStats` 的超预算告警（100 ms）。
5. **跨 C ABI 永远只有三样东西**：表操作入口（返回 handle）、完成回调（函数指针 + user_data +
   状态 + 两段字符串）、取消 token（函数指针 + user_data）。协程帧、asio 对象、std 容器都在插件内，
   不跨界。
6. **取消**：token 是 C 结构，kit 在挂起时把它交给主程序；取消发生时以 `Canceled` 完成回调，
   kit 在帧里抛 `CanceledException`（与今天的 `tool` 一致）。
7. **并发等待**：分两类说清楚（见下节）。

**能并发什么、不能并发什么**：

| 想做的事 | 现在支持吗 | 依据 |
|---|---|---|
| 同时等多条**表操作** | **支持**：`tableOp(...)` 调用即提交（返回 ticket），一批提交后 `co_await whenAll(std::move(tickets))`，总耗时 = 最慢那条（不是相加） | `kit.h:4302-4323` |
| 同时等多个 **`offload`** | **暂不支持**：`OffloadAwaiter::await_suspend` 才调 `sched->offload`，所以连写两个 `co_await` 是串行的 | `kit.h:3699-3701` |
| 同时等多个 **`sleep`** | **暂不支持**：同理，`SleepAwaiter::await_suspend` 才调 `sched->sleep` | `kit.h:3602-3607` |
| 自建内核对象（socket / 管道 / `steady_timer`）后 `co_await` | **不支持**（无 reactor，需插件自持 `io_context` + `run()`，会占住插件线程） | — |

也就是说：“并发能力”并不是被砍掉了 —— 表操作本来就能并发；要并行跑多段阻塞工作或多段延时，只需给
`offload` / `sleep` 也提供**“提交即返回 ticket”**的形态（与 `tableOp` 一致，工作量小，列为可选改进，
见 §8）。插件真正不能做的是“自己创建内核对象并等它”。

**延时原语**：只有 `sleep(ms)`（毫秒粒度）。它是**框架**提供的（`PluginxxHostCore::sleep`，
`host_core.h:1106`）：在 io 执行器上建一条 `asio::steady_timer`，到期用完成回调通知，插件帧挂起期间
线程去跑别的回调，**不占线程**。两点提醒：

- 今天它的“受理”这一步还是同步等待（`tables_impl.h:1064` 的 `ioCallSyncKeep`，§2.4 第 2 条），
  每次 `sleep` 会占插件线程一个 io 往返；改成纯提交后连这一步也不占；
- `yield(ctx)`（让出一轮）也在，今天靠桥投递 + 唤醒；拆桥时改用 `request_driver`（§3.9 末）。

**因此删掉的东西**：`polled_tool`（它的身体就是 asio 协程，依赖插件自持 reactor）、
`PollOneBridge` 的轮询部分（`poll_one` 循环 + 退避定时器 + `PolledRoot`）、以及
“插件私有 reactor 的内部等待对象接进主程序执行序列”这套协议。
`request_driver` / `cancel_driver` 保留（它就是“稍后在插件线程上 resume 我”的原语）。

**6 处 `polled_tool` 的去处**：

| 位置 | 现在等什么 | 改成 |
|---|---|---|
| filesystem `:355` | `asio` 异步文件读 | `co_await offload(同步读)`（该工具本来就有 blocking 分支） |
| filesystem `:449` / `:529` | 同上 | 同上 |
| execute_command `:129` / `:235` | boost.process v2 子进程管道 + 计时 | `co_await offload(同步跑子进程)`（已有 blocking 分支可对照） |
| example_plugin `:317` | 示例用的 `steady_timer` | `co_await sleep(…)`（改为表原语） |

**代价（写清楚，不回避）**：不能 `co_await` 插件自建的内核对象（socket / 管道 / 定时器对象）；
延时只用 `sleep`（毫秒粒度）；重 CPU/阻塞必须 `offload`，否则卡的是整条插件线程（所有插件一起卡）。
并行多段阻塞工作 / 多段延时需要先给 `offload` / `sleep` 补 ticket 形态（可选，见 §8）。
这几条会写进插件开发约定。

**分批**：第一步只把工具侧换成 `Task` + 删 `polled_tool`（桥的轮询部分只剩图节点 / `call_tool` awaiter
在用）；第二步把那两处也搬到 `Task` 驱动，然后删 `PollOneBridge` 与主程序侧 `poll_one`。

### 3.10 `offload` / `sleep` 的 ticket 形态（实施计划）

**目标**：让“并行多段阻塞工作 / 多段延时”与表操作一样可以“先提交一批、再一起等”，
且**不新增名字**（按 §3.1 第 6 条）：`offload(...)` / `sleep(...)` 直接返回 ticket，本身就能 `co_await`；
现有调用点 `co_await offload(...)` / `co_await sleep(...)` 写法不变。

**API 形状**（与 `tableOp` 对齐）：

```cpp
// 提交即开始（不等）
auto a = pluginxx::offload(ctx, [&] { return readFile(p1); });   // OffloadTicket<std::string>
auto b = pluginxx::offload(ctx, [&] { return readFile(p2); });
auto c = pluginxx::sleep(ctx, 200);                              // SleepTicket

// 单条：与今天写法一样
std::string text = co_await a;

// 一批：总耗时 = 最慢那条（不是相加）
auto r = co_await pluginxx::whenAll(std::move(a), std::move(b)); // Task<std::tuple<std::string, std::string>>
// 同类型批次也行：whenAll(std::vector<OffloadTicket<std::string>>) -> Task<std::vector<std::string>>
// 混合类型（含 tableOp）走变参版：whenAll(tableOpTicket, offloadTicket, sleepTicket)
```

**内部结构**（与现有 `TableOpTicket` 同一套）：

```cpp
struct OffloadState<R> {                 // shared_ptr 持有
    std::atomic<AwaiterState> state;     // SUBMITTED / COMPLETED
    std::optional<R>          value;
    std::exception_ptr        error;     // CanceledException / 业务异常
    int32_t                   status;
    std::string               errorText;
    PluginxxOperatorHandle*   handle;    // 取消用
    std::coroutine_handle<>   waiter;    // 挂起时登记；完成时直接 resume
};
```

关键点：

1. **提交在构造函数里**（调 `sched->offload` / `sched->sleep`），返回 ticket；没被 await 也不影响，
   完成后结果存在 state 里（同一个 `whenAll` 里总是先提交后等待）。
2. **完成回调在插件线程上**（`OpCore::commit` → `dispatchToPlugin`），所以可以**直接 `handle.resume()`**，
   不再需要 `PollOneBridge` 的投递路径 —— 这同时把这两个原语从桥里抽出来（与 §3.9 一致）。
3. **只 await 一次**：ticket 是移动型；完成后再 await 立即拿到结果（存好的），不会重跑。
   已提交未 await 的 ticket 析构 = “提交了不管”，结果丢弃（取消不自动发，靠 §2.14(g) 的取消入口）。
4. **取消**：ticket 提供 `cancel()`（`cancel->op_cancel(handle)`）；await 时得到 `CanceledException`，
   与现有 offload / sleep 语义一致；`whenAll` 里某条取消不影响其余条目。
5. **实例卸载**：完成前实例已卸载时，回调由框架按 closed 丢弃，ticket await 得到 `CanceledException`
   （与 tableOp 一致的收尾规则）。

**`whenAll` 泛化**：

- 现在是 `whenAll(std::vector<TableOpTicket>)`（`kit.h:4316`，内部就是按顺序 `co_await`，
  因为 ticket 已提交，所以总耗时 = 最慢）：
- 改成模板：`template<class T> Task<std::vector<typename T::result_type>> whenAll(std::vector<T>)`，
  对 `TableOpTicket` / `OffloadTicket<R>` / `SleepTicket` 都成立（`TableOpTicket` 补一个 `result_type = TableOpResult`）；
- 新增变参重载：`Task<std::tuple<...>> whenAll(Ts&&... ts)`，支持混合类型（含 `tableOp`）。

**分步**：

| 步 | 内容 | 验证 |
|---|---|---|
| 1 | `OffloadAwaiter` / `SleepAwaiter` 改成“构造即提交 + 共享 state + 直接 resume”的 ticket；`offload` / `sleep` 签名不变 | 现有插件编译不变（调用点零修改）；`plugin_executor` 同步桥用例 |
| 2 | `whenAll` 改模板 + `TableOpTicket::result_type` | `plugin_runtime`、`plugin_sdk` |
| 3 | 变参 `whenAll` + ticket `cancel()` | 新增用例（见下） |
| 4 | 文档：`plugins.md` 协程一节写明“能并发什么”（表操作 / offload / sleep 均可，自建内核对象不可） | — |

**验证用例**：

- 两个 `offload`（各 `sleep(80ms)` 后返回）并行 → 总耗时 < 150 ms（串行会是 ~160 ms 以上）；
- 两个 `sleep(80ms)` 并行 → 同上；
- `whenAll(tableOp, offload, sleep)` 混合三类都完成；
- 一条 `cancel()` → 该条抛 `CanceledException`，其余条目正常返回；
- 完成后再 `co_await` → 立即拿到结果（不重跑）；
- 实例卸载后 await → `CanceledException`。

**风险**：

- 改成“构造即提交”后，“写了一个 ticket 但忘了 await”的语义从“没发生”变成“会发生”——
  对 SLEEP/OFFLOAD 是幂等工作，风险低；但插件代码要养成“不该跑的不要建 ticket”的习惯（写进约定）。
- 共享 state + 直接 resume 要求完成回调一定在插件线程（现在是）；未注入插件执行器的嵌入/测试形态
  回退到 io 执行器，resume 仍在同一条线程上（与 §3.6 的注入点说明一致）。

## 4. 边界（本次不做）

- 不改"io 线程从不阻塞等插件线程"；
- 不重做广播/能力/图节点/网络的语义，只改"怎么等"；
- 不引入"每实例一条线程"；
- 不再新建任何线程池（包括将来的功能）；需要阻塞工作一律投管理器的池；
- 不做"按需构造插件框架"：启动就建，没配插件也建；
- 不保留任何旧签名、旧符号、旧编号的兼容层；
- **本次不拆整个 `PollOneBridge`**：工具侧不再经过它的轮询路径，但图节点与 `call_tool` awaiter 仍在用；
  第二步把那两处也搬到 `Task` 驱动后，再删桥与主程序侧 `poll_one`（§3.9 末）；
- **插件代码不得使用 `io_context` / `co_spawn` / `run()`，也不得自建 socket / 管道 / 定时器**：
  插件线程的循环归框架，插件要等就用 kit 原语（`sleep` / `offload` / 表操作）。

## 5. 分步实施

| 步 | 内容 | 验证 |
|---|---|---|
| 1 | 决策确认 + 基线记录（`pluginCallbackStats`、加载耗时、线程数、全量结果） | `plugin_executor`、`plugins`、`client_plugins` |
| 2 | 命名：入口符号与导出宏改名、`entrySymbols()` 更新、**全部表编号改下划线** | `plugin_runtime`、`plugin_multi_instance`、`plugin_multi_mount`、`boundaries` |
| 3 | `get_info` 填表化 + 删 `AgentxxClientPluginInfo` + 版本校验改 `min_api_version` | 25 个插件重建 + `plugin_multi_instance` |
| 4 | 框架通用化收尾：注释/示例去端化、`boundaries` 新规则 | `boundaries` |
| 5 | 两侧管理器**启动即构造**（执行线程构造即起）+ 删 `ensurePluginThread` | `plugin_executor`、客户端启动用例 |
| 6 | 线程池统一：管理器持一个池（默认 3 条，`plugin.thread_pool_workers`）；删 `AgentHost::threadPool_`、`AgentContext` 默认池、client 两个池、管理器回退池；借用方改取管理器池 | 线程数断言 + 全量（工具 offload / 会话 / worktree 路径） |
| 7 | 4 个同步受理入口改提交（含 `OpCore` 操作编号原子化与实例登记线程安全化） | `plugin_executor`（同步桥用例改造）、`plugin_runtime` |
| 8 | `is_canceled` 改快照 + JS 引擎轮询改本地读 | `plugins`、`plugin_executor`、`plugin_example_js` |
| 9 | 其余同步面收口：`unsubscribePluginBroadcast` 去等待、`disable`/`enable` 改返 `Task<bool>`、相关测试改等异步 | `plugin_executor`、`plugin_host_mount`、`plugin_multi_mount` |
| 10 | 删同步桥与全部阻塞助手（同名改成协程，不加 `Async`）；kit 拆三个头（不保留转发头）；插件侧阻塞调用点同批迁移（§3.5 第 10~14 条） | 编译期错误清单清零 + `plugin_dispatch`、`plugin_features`、`plugin_example_*`、`plugin_example_chatbot` |
| 10b | 工具体收成一个：`tool`（`pluginxx::Task` 体）不再走 poll-one；删 `fast_tool` / `blocking_tool` / `polled_tool` / `registerToolSync`（共 53 处调用点，先 filesystem 后批量） | 插件全组回归（尤其 filesystem 的 move/copy/read、execute_command、chatbot） |
| 10c | `offload` / `sleep` 改“提交即返回 ticket”（名字不变 + 直接 resume）；`whenAll` 泛化 + 变参（§3.10） | 并行耗时用例 + `plugin_runtime`、`plugin_sdk` |
| 11 | `create`/`start` 合并（返回 void、可重入）+ `destroy` 改异步 + 25 个插件迁移 | `plugin_multi_instance`、`plugin_multi_mount`、`plugin_*` 全组 |
| 12 | 测试夹具统一带插件线程与池 | 全部插件用例 |
| 13 | 修 §2.6 里列的静默降级（含 §2.14(d) 新发现的两处订阅）+ 文档/代次 + 全量回归 | 全量 `agentxx_test --log-level warn`；`plugin_llm_uncensored`、`plugin_example_chatbot` 等 |

## 6. 验证计划

- 每步：`plugin_executor` / `plugin_runtime` / `plugins` / `client_plugins` / `boundaries`；
- 新增用例：
  - 插件线程上调全部表成员都立即返回（用永不完成的假表验证不阻塞）；
  - `is_canceled` 高频轮询不产生 io 往返；
  - 慢 `create`（内部 sleep）不占 io 线程、加载协程能等；
  - 慢 `destroy` 同理；`dlclose` 不在 io 线程上跑；
  - 带插件执行线程的 `llm_uncensored` 参数覆盖（静默降级的回归防线）；
  - `get_info` 填表：框架只读一次并复制、插件改 `api_version` 会被框架忽略/拒绝；
  - `create` 可重入（禁用后启用只做注册，不重建 ctx）；
  - **启动即构造**：没配插件的进程里管理器、插件线程、池（默认 3 条）都已存在；
  - **线程池唯一**：`AgentContext::threadPool` 与管理器池是同一个对象；agent 进程与 client 进程
    各只有一个池（嵌入方注入除外）；`plugin.thread_pool_workers` 改值后两端池线程数同步变。
  - **表编号**：新编号能 `query_interface` 命中，旧点分编号返回空；
  - **退订不等**：io 线程忙时 `unsubscribePluginBroadcast` 立即返回，之后回调次数仍为 0；
  - **异步助手在插件线程上可用**：插件线程里 `co_await` `checkPathDecisions` /
    `subscribePluginBroadcast(ctx,…)` / `argsJson` 都能拿到真实结果（不是拒绝、不是空串）；
  - **迁移后的静默降级不再复发**：`chatbot` 的网关能订上网络事件、`text_selection_monitor` 能订上、
    `example_runtime` 能读到自己的 args、`llm_uncensored` / `planning` / `example_resources` 读到配置；
  - **工具体只剩一个**：`tool` 的 `Task` 体里 `co_await` 一次表查询 + 一次 `offload` + 一次 `sleep` 都正常；
    体抛异常转 failed；旧入口再出现属于回退（代码评审把关，不加自动检查）；
  - **交错不被阻塞**：一个插件挂着一个慢 `offload` 工具时，另一插件的工具/广播回调仍能及时执行
    （证明插件里没有自建 reactor/`run()`）；
  - **无插件执行器的嵌入形态**：未注入插件执行器时工具仍能跑完（回退到 io 执行器）并有一条告警；
  - **启停可等**：`disable` / `enable` 返回时状态已落定（测试不再轮询）；
  - **脚本 publish**：`await agentxx.publish(...)` 拿到受理结果，不 await 时也能送达；
  - **并发耗时**：两个各含 `sleep(80ms)` 的 `offload` 并行 → 总耗时 < 150 ms（串行会超过 160 ms），
    见 §3.10 的验证用例。

## 7. 风险与回退

| 风险 | 影响 | 处理 |
|---|---|---|
| 命名与表编号变更面大（约 200 处 + 26 个编号） | 漏改导致加载失败或查表拿到空 | 先改两个 `entrySymbols()` 与导出宏，编译期就会暴露插件；再用 `boundaries`/`plugin_runtime` 双向校验新旧编号 |
| `create` 合并后"启用"语义变化 | 禁用/启用路径回归 | 实现可重入 create，补"禁用→启用→再禁用"用例 |
| `get_info` 改填表后版本协商变弱 | 老主程序/新插件判定错 | `min_api_version` + 框架侧校验 + 加载失败原因写清楚 |
| 受理线程变更引入竞态 | 难查 | 先做"编号原子化 + 登记加锁"，ASan/UBSan 全量跑 |
| 完成回调全投插件线程，注册类延迟升高 | 挂载变慢 | 保持 FIFO + 回调预算告警；mount 内批量 `whenAll` |
| kit 拆头导致包含关系变化 | 编译大面积失败 | 按端拆、25 个插件同一提交改 include；单独一步，失败就只回退这一卡 |
| **池合并后饱和**（工具 offload + 会话加载 + `dlopen`/`dlclose` 抢同一个池） | 阻塞工具排队、加载变慢 | 默认 3 条、`plugin.thread_pool_workers` 可配；用 `pluginCallbackStats` 与延迟断言观察；必要时只调大，不新建池 |
| 删 `AgentContext` 默认池影响直接构造上下文的代码 | 空指针 | 默认置空 + 所有借用方统一从 `pluginManager` 取；编译期搜 `threadPool` 逐个改 |
| `destroy` 异步后 ctx 失效时机 | 悬垂指针 | 框架先清全部引用再调 destroy；destroy 完成后立即置空 ctx 与挂载 user data |
| 工具体从 4 个收到 1 个（改 53 处调用点） | 阻塞工作被误当“快工作”留在插件线程上，工具卡住整条插件线程 | 体里一律 `co_await offload(...)` 包阻塞段；先 filesystem 后批量；`blocking_tool` 删完后搜残留 |
| 工具体改由插件线程直接驱动（不再经 poll-one 桥） | 取消/超时/携带会话上下文的行为差异 | 取消仍走 `cancelFlag` + `CancelRegistry`（不变）；完成包仍由 `OpCore` 统一提交；未注入插件执行器时回退 io 并有告警 |
| 去掉插件自持 reactor 后能力变窄 | 插件不能再自建 socket/管道/定时器；不补 ticket 形态时，单个工具内不能并行等两个 offload | 跨端一律走 `agentxx.net`；并行用 ticket 形态 + `whenAll`（§3.10）；延时用 `sleep`；写进插件开发约定（§3.9 末） |
| `disable` / `enable` 改返 `Task<bool>` | 原有调用方（设置界面、测试）不能直接调 | 调用点改 `co_spawn` 包一层；只有两处（设置弹窗后端与测试），一次改完 |
| JS 引擎 `publish` 改 Promise 后脚本语义变化 | 脚本里 `await agentxx.publish(...)` 才等得到受理结果，不 await 时只保证提交 | 与其余表成员一致（都是 Promise）；示例脚本与文档同步写明 |

回退：按步提交；命名/编号与 `get_info` 两步可以一起回退；`create`+`destroy` 合并是最后的大步，单独提交。

## 8. 待确认

**已定（本轮及前一轮）**：

- 配置键改名：`threadPoolWorkers` → `plugin.thread_pool_workers`，默认 `3`，归属 agent + client 共用；
- 线程池数量**不分开配**：两端同一个值；agent 端整体一个池，client 端整体一个池；
- 子代理 / 后台任务的子上下文也共用根管理器的池；
- **不加 `boundaries` 新规则**（线程池唯一、命名后缀都由代码评审把关）；
- **工具体只留一个 `tool`，体是 kit 的 `pluginxx::Task<ToolResult>`**（挂起 = 登记完成源，唤醒 =
  完成回调投回插件线程后 `resume()`；插件里禁 `io_context`/`co_spawn`/`run()` 与自建 socket/管道/定时器）；
  删 `fast_tool` / `blocking_tool` / `polled_tool` / `registerToolSync`，共 53 处调用点；
- `blocking_tool` 的同步返回**不是阻塞 bug**：体本来就在工作池线程上跑，卡插件线程的是 `execute_start`
  里两次 `workDir/argsJson` 读（已列入 §3.5 第 14 条待改）；
- `offload` / `sleep` 改成“提交即返回 ticket”（本身可 await，名字不变），`whenAll` 泛化 + 变参，
  多段阻塞工作 / 多段延时可并行；实施与验证见 §3.10。

**可选项（唯一）**：dst 复核要不要改成“权限声明支持第二个目标”，由框架执行前统一问一次；
不改也不影响本次重构（dst 复核直接 `co_await checkPathDecisions(...)` 就行）。
