# 插件接口表异步化 —— 实施记录

> 计划见 [plan.md](plan.md)，任务卡片见 [index.md](index.md)。
> 本文件只记"实际做了什么 + 怎么验证 + 与计划的偏差"，每完成一步就写。

## 进度总表

| 阶段 | 内容 | 状态 |
|---|---|---|
| A | 全局 API 版本 / 异步表调用骨架 / kit 三形态 / `agentxx.agent.session` 试点 | 完成（编译通过、相关测试全绿） |
| C2-槽位清理 | 删 `scheduler.post_to_io` / `scheduler.op_cancel` / `capabilities.op_cancel`；`coroutine_runtime` 整张表并回 `scheduler`（通用表 10 → 9） | 完成（编译通过、全量插件测试全绿） |
| A2 | agent 侧 `dlopen` + `create` 挪到加载线程池 | 完成（编译通过、插件测试全绿） |
| B | `agentxx.agent` 业务表全部成员 | 完成（9 张表全部改异步操作契约） |
| C | `agentxx.agent.runtime` 全部成员 | 完成（21 个成员改异步；lib / test / client / 25 个插件编译通过，插件相关测试 3983 条全绿） |
| C2-表异步化 | 框架表其余成员（broadcasts / capabilities 登记 / config / plugins / `net.call`）改异步 | 进行中（第一批 `config` v4 / `plugins` v2 完成；`broadcasts` / `capabilities` 登记 / `net.call` 待做） |
| D | `agentxx.client.*` 六张表 + 三个回答型回调 | 待做 |
| E | 删残留同步路径与 client 侧 150 ms 同步桥、文档 | 部分完成（接口表数量与 §8.0 调用模型文档已同步） |

## 关键实现决定（与计划一致，落在代码里）

1. **版本**: 计划写的是 5 → 6，实际仓库里第 6 代已被"工具结果两段上报"占用，
   所以本次是 **6 → 7**（`agent/CMakeLists.txt` 的 `AGENTXX_PLUGIN_API_VERSION`、
   `client_plugin_api.h` 的 `AGENTXX_CLIENT_PLUGIN_API_VERSION`、`test_plugin_abi_c17.c`
   的 `_Static_assert` 三处一起抬）。
2. **完成回调复用 `PluginxxOperatorCallback`**，没有新增类型。
3. **主程序侧执行框架统一到一处**: 新增 `pluginxx/host/table_op.h` 的
   `startTableOp(mgr, cb, ud, error_out, label, work)`：
   - 工作体在本管理器 IO 线程执行；**调用方本来就在 IO 线程时内联执行**，
     所以"注册完立刻可见"的顺序与同步调用完全一致，也省掉一次投递；
   - 工作体返回结果正文，抛异常 = `PLUGINXX_OPERATOR_FAILED`（正文是原因）；
   - 受理失败（参数/作用域/实例关闭）由入口自己写 `error_out` 并返回 `NULL`。
4. **kit 三形态**放在 `pluginxx/kit/kit.h`（`agentxx::plugin` 逐条 `using` 引入）：
   - `co_await tableOp(ctx, start)` → `TableOpResult{status, payload, startError}`
     （业务失败**不抛异常**，取消是其中一种状态；挂起协议与 `InvokeCapAwaiter` 相同，
     经协程驱动桥恢复，不直接从主程序回调栈进插件协程）；
   - `tableOpCallback(ctx, start, done)`：任意线程，`done` 在 IO 线程恰好一次；
   - `tableOpBlocking(ctx, start, timeoutMs)` / `tableOpBlockingRaw(host, cancel,
     scheduler, start, timeoutMs)`：阻塞形态。**IO 线程上不等待**（先看是否已内联完成，
     没完成就返回 `accepted() == false`），所以 `mount`/`start` 里调用不会死锁。
5. **跨线程投递前先拷入参**: `PluginxxScopeRef.id` 与全部 `PluginxxStringView` 入参
   都是借用视图，请求排到 IO 线程后原内存可能已失效。主程序侧新增 `ScopeRefKeep`
   与"先拷字符串再投递"的写法（`plugin_manager_vtable.cpp` 的
   `asyncTableCall` / `asyncScopedTableCall`）。
6. **测试与脚本引擎里的同步调用**：脚本线程不是主程序 IO 线程，用
   `tableOpBlockingRaw` 保持同步语义（后续阶段 D 会改成 Promise）。

## 分步记录

### 第 1 步：骨架 + `agentxx.agent.session` 试点

改了哪些文件：

- `agent/third_party/cxx_pluginxx/include/pluginxx/host/table_op.h`（新增）
- `agent/third_party/cxx_pluginxx/include/pluginxx/kit/kit.h`
  （`TableOpResult` / `TableOpState` / `TableOpAwaiter` / `tableOp` /
  `tableOpCallback` / `tableOpBlocking` / `tableOpBlockingRaw`）
- `agent/lib/include/agentxx/plugin/plugin_framework.h`（包含与 `using`）
- `agent/lib/include/agentxx/plugin/api/plugin_kit.h`
  （`using` 引入三形态；`addShareStore` / `workDir` 改走阻塞形态并补
  `addShareStoreAsync` / `workDirAsync`）
- `agent/lib/include/agentxx/plugin/api/plugin_api.h`
  （文件头加"调用模型：表成员一律是异步操作契约"整段说明；
  `agentxx.agent.session` 表 v3 → v4，`get_share_store` / `emit_message_tip` /
  `add_share_store` / `get_session_work_dir` 改成异步，`is_canceled` 保持同步）
- `agent/lib/src/plugins/plugin_manager_vtable.cpp`
  （`asyncTableCall` / `asyncScopedTableCall` / `ScopeRefKeep` / `takePluginString`；
  四个 session 成员改写成异步实现）
- `agent/plugins/agentxx_javascript_engine/agentxx_javascript_engine.cpp`
  （四个桥调用改成 `tableOpBlockingRaw`，脚本侧行为不变）
- `agent/test/plugin/test_plugins.cpp`（`get_session_work_dir` 的用例改异步形态）
- `agent/CMakeLists.txt`（API 代次 6 → 7，注释补 v7）
- `agent/lib/include/agentxx/plugin/api/client_plugin_api.h`（client 侧代次 6 → 7）
- `agent/test/plugin/test_plugin_abi_c17.c`（`_Static_assert` 6 → 7）

验证：

- 内层 lib 构建目录 `cmake --build ... --config Debug`：`libagentxx_staticd.lib` 与
  全部插件 DLL 编译通过；
- `cmake --install` 刷新安装前缀后，client 与 test 构建目录均编译通过；
- `agentxx_test --log-level warn boundaries plugin_runtime plugin_sdk plugin_bridge plugin_cleanup`
  → `Total: passed=1166 failed=0`（`boundaries` 的表成员顺序清单与四件套校验仍然通过，
  因为本次只改签名、没改成员顺序与数量）。

遇到的问题（已解决）：

- 第一次构建报 `"bridge" 不是 TableOpState 的成员`：完成回调里误把驱动桥指针写在
  状态对象上。改成在 `await_suspend` 里把 `PollOneBridge*` 拷进回调闭包。
- 内层 lib 构建用**安装前缀**里的 `pluginxx` 头（`find_package(cxx_pluginxx)`），
  直接改源码目录后必须先刷新安装前缀，否则一直在编译旧头（表现为改了也没用）。
  快速迭代时手工把改过的头拷进 `{build}/agentxx-project-install/include/`，
  正式构建时由 `cxx_pluginxx_repo` 的 ExternalProject 重新安装。
- `test_plugin_abi_c17.c` 里的 `_Static_assert` 固定了代次，抬版本时必须同步。

### 第 2 步：槽位清理（`post_to_io` 删除、`coroutine_runtime` 并回 `scheduler`、`op_cancel` 合并）

计划 §5.6 / §5.7。改动：

- `cxx_pluginxx/include/pluginxx/api/tables.h`
  - `scheduler` v1 → v2：删 `post_to_io`、`op_cancel`，加入 `request_driver` / `cancel_driver`；
    基线随签名变化重置为 `sizeof`；驱动协议注释搬到本表头部；
  - 删除整段 `coroutine_runtime` 表（IID / 宏 / 结构体）；
  - `capabilities` v6 → v7：删 `op_cancel`（并入 `cancel.op_cancel`），基线重置；
  - 文件头注释：通用表 10 → 9 张、各表版本说明。
- `api/iface_meta.h`：删 `IfaceMeta<PluginxxCoroutineRuntimeIface>`。
- `api/abi.h`：`PluginxxDriver` 注释里的表名改成 `pluginxx.scheduler`。
- `host/tables_impl.h`：删 `postToIoEntry`；scheduler 入口按新成员顺序重排；删
  `coroutineRuntimeIface()` 与 `query_interface` 分派；`kGenericTableNames` 去掉
  `coroutine_runtime`；`opCancelEntry` 只留给 `cancel`（`tasks` 复用）。
- `kit/kit.h`：`PluginIfaceCore` 去掉 `coroutineRuntime`；`PollOneBridge` 改成
  `(host, scheduler, cancel)`，驱动两成员读 `sched_`、取消读 `cancel_`；
  `SleepAwaiter` / `OffloadAwaiter` / `InvokeCapAwaiter` 的取消改走
  `cancel.op_cancel`；`spawn` 不再借 `post_to_io`（`startBridgedRoot` 自己会排队）。
- agentxx 侧：`plugin_kit.h` / `plugin_interfaces.h` / `plugin_manager.h` /
  `client_plugin_manager.h` / 两个 domain_services / `plugin_manager_vtable.cpp`
  去掉 `coroutineRuntime` 与 `coroutine_runtime` 相关文字与成员。
- 插件：`agentxx_chatbot`（面板驱动票改用 `iface.scheduler->request_driver` /
  `cancel_driver`）、`example_plugin`（降级注释）。
- 测试：`boundaries`（成员顺序清单、通用表 9 / 合计 25、两个 `kInterfaceTableCount`
  期望值 19 / 16、文档数字）、`test_plugin_bridge.cpp` / `test_plugin_sdk.cpp`
  （伪主程序把驱动两成员挂在 scheduler 上）、`test_plugin_runtime.cpp`
  （驱动用例改查 scheduler；C17 值表换成 `request_driver` / `cancel_driver` 偏移）、
  `test_plugin_host_mount.cpp`（IID 清单少一项、期望 19）、`test_plugin_abi_c17.c`。
- 文档：`plugins.md` / `index.md` / 根 `AGENTS.md` 的接口表数量与列表。
- `cxx_pluginxx/src/api_abi_check.c`：capabilities / scheduler 重置基线后不再有追加段，
  把"必须真的追加了成员"改成"基线不得超过完整布局"。

验证：`windows_debug_build.bat` 全量通过（含 configure，API 代次 7 已重新下发到内层构建），
随后 `agentxx_test --log-level warn` 跑：

- `boundaries` 19/0、`plugin_runtime` 730/0、`plugin_sdk` 134/0、`plugin_bridge` 193/0、
  `plugin_cleanup` 90/0、`plugin_host_mount` 145/0、`plugin_multi_mount` 47/0、
  `plugins` 707/0 → 合计 2065 通过；
- `client_plugins` 835/0、`plugin_feature` 34/0、`plugin_point` 68/0、
  `plugin_broadcast` 90/0、`plugin_dispatch` 112/0、`plugin_agent_runtime` 221/0、
  `plugin_example_runtime` 50/0、`plugin_example_chatbot` 265/0 → 合计 1675 通过。

遇到的问题（已解决）：

- **"内联快路径"一开始不成立**：`OpCore::onDoneCore` 把提交排到 executor 上，因此即使
  工作体在 IO 线程上当场执行，`cb` 也要等下一个 executor 轮次。于是
  `tableOpBlocking` 在"自己就是 IO 线程"的调用方上拿不到结果，`plugins` 用例直接卡住。
  修法：`startTableOp` 在 `mgr->isIoThread()` 时**自己**当场回调（`cb` 不交给 OpCore），
  OpCore 只做记账；非 IO 线程仍由 OpCore 在 IO 线程派发。`cb` 依旧恰好一次。
- `test_plugins` 里那句 `tableOpBlockingRaw(..., nullptr, nullptr, ...)` 无法判断
  "我是不是 IO 线程"，改成从接口表查出 `cancel` / `scheduler` 再传进去。
- 内层构建目录的 `CMAKE_CXX_FLAGS` 缓存着上一代 `-DPLUGINXX_API_VERSION=6`：
  只改顶层默认值不会传导，必须重跑顶层 configure（跑一次 `windows_debug_build.bat`）。
- 增量刷新安装前缀（手拷头文件）能加快迭代，但**测试可执行文件**在
  `agentxx_test_repo-build/Debug/` 下，`exec/` 里那份要等顶层 install 才更新 ——
  曾经因此对着旧二进制排查了很久。

### 第 3 步：加载线程（A2）+ 设计文档的调用模型说明

计划 §6。改动：

- `cxx_pluginxx/include/pluginxx/host/lifecycle.h`：新增扩展点
  `virtual asio::awaitable<std::string> runLoaderTask(std::function<std::string()>)`
  （默认实现 = 调用线程直接执行，返回错误文本，空串表示成功）。
  `loadNativeAsync` 里把 `NativeLoader::open`（dlopen 会读盘、跑 DSO 静态构造）与
  `createFn(...)` 各包一次 `co_await runLoaderTask`；`create` 抛出的异常与返回码
  统一转成错误文本后走原来的 `rollbackLoad`。
- `agent/lib/include/agentxx/plugin/plugin_manager.h`：声明覆写，并补回被误删的
  `std::string language() override;`。
- `agent/lib/src/plugins/plugin_manager_domain_services.cpp`：实现
  `PluginManager::runLoaderTask` —— 用 `postToWorkerThread` 投递，
  `asio::steady_timer(max())` + `asio::as_tuple(use_awaitable)` 在结果写回后叫醒
  IO 线程；没有可用线程池时退回调用线程执行并 `XX_LOGW`；正常投递时
  `XX_LOGD` 记一条「caller on io: true/false」。

要点：工作线程里**只**跑 dlopen 与 `create`，插件那次调用拿不到主程序接口表
（`create` 契约本来就不许注册），所以不会出现「工作线程上调表」的跨线程问题。
返回的协程结果在 IO 线程上被读，之后的生命周期步骤（`start`、挂载、注册）仍在
IO 线程。

验证：

- `script\windows_debug_build.bat` 全量通过（含 configure）；
- `agentxx_test --log-level warn boundaries plugins plugin_runtime plugin_sdk
  plugin_bridge plugin_cleanup plugin_host_mount plugin_multi_mount` →
  `Total: passed=2065 failed=0`；
- `client_plugins` 835/0、`plugin_feature` 34/0、`plugin_point` 68/0、
  `plugin_broadcast` 90/0、`plugin_dispatch` 112/0、`plugin_agent_runtime` 221/0、
  `plugin_example_runtime` 50/0、`plugin_example_chatbot` 265/0 → 合计 1675 通过；
- A2 落地后复跑 `plugins` 707/0、`plugin_cleanup` 90/0、`plugin_host_mount` 145/0、
  `plugin_multi_mount` 47/0、`client_plugins` 835/0 → 1824 通过。

文档同步（阶段 E 的一部分，先做掉当前已成立的部分）：

- `docs/zh-cn/design/plugins.md` 新增 **§8.0 调用模型: 表成员是异步操作契约**：
  契约签名（返回值只表示受理，结果经 `PluginxxOperatorCallback` 交回，`NULL` =
  未受理 + `error_out`，`cb` 可空）、「什么时候必须异步」的判据（要不要等另一条
  线程/要不要读盘）、仍保持同步的五类成员（`is_io_thread` / `is_canceled` /
  `get_config` 等纯内存查询、注册类成员的返回值本身就是受理结果、`op_cancel`、
  广播发布）、FIFO 顺序与「调用方本来就在 IO 线程时当场回调」、kit 三形态对照表。
- §8.1 的 `agentxx.agent.session` 行改成 v4 并标注「v4 起异步」；
  `agentxx.capabilities` 行改成 v7（删 `op_cancel`）；删掉重复的 `agentxx.scheduler | 1` 旧行；
  §8.2 标题的通用表数量 10 → 9。
- `agentxx.agent.tools` 的 `call_tool_async` **没有**改名 —— 改名属于阶段 B，
  文档先写实际名称，等实现那一步再一起改（避免文档写不实的东西）。

### 第 4 步：阶段 B 第一批 —— `agentxx.agent.tools`

改动：

- `plugin_api.h`：`agentxx.agent.tools` v4 → v5（`MIN_VERSION` 同步、基线重置）；
  `register_tool` / `unregister_tool` 改成异步操作契约（尾部 `cb` / `ud` / `error_out`）；
  `call_tool_async` 更名为 `call_tool`（签名不变，异步已是默认，§7.4）。
- `plugin_manager_vtable.cpp`
    - 新增 `ToolSpecKeep`: `spec` 里三个字符串视图是借用视图，投递前拷成自有字符串并把
      视图指回它们；闭包按 `shared_ptr` 持有本对象（拷贝后视图不会指向旧对象）；
    - 两个成员改走 `asyncScopedTableCall`，失败用 `TableOpError(resultName(rc))` 交回；
    - **把 `asyncTableCall` / `ScopeRefKeep` / `asyncScopedTableCall` / `takePluginString`
      整块定义上移到 `mountCall` 之后**（原在文件中部，新入口在它上方会编译不过）。
- `asyncScopedTableCall` 的失败划分改了（见下面"与计划的偏差"）。
- `plugin_kit.h`: 新增 `detail::registerToolSync(ctx, agent, spec)`（阻塞形态），四个
  工具形态（`tool` / `fast_tool` / `blocking_tool` / `polled_tool`）的注册点改用它；
  `call_tool_async` 全部改名 `call_tool`。
- `agentxx_javascript_engine.cpp`: `registerTool` / `unregisterTool` / 撤销动作 lambda
  三处改走 `tableOpBlockingRaw`（脚本线程不是主程序 IO 线程，会真等）。
- 测试：`boundaries` 成员清单改名；`test_plugin_bridge.cpp` / `test_plugin_sdk.cpp`
  的 fake 表改成异步签名（回调内联发 OK，返回非空 handle）；
  `test_plugin_host_mount.cpp` / `test_plugin_runtime.cpp` 用 `tableOpBlockingRaw`
  包一层同步断言；`test_start_fail` DSO 插件在 `start` 里加 `RegProbe`（IO 线程上内联回调）。
- 文档：`plugins.md` 的 tools 行抬到 v5 并记变更、§8.0 补"作用域引用的两种失败"、
  JS 引擎那段 `call_tool_async` → `call_tool`。

验证：

- 内层 lib / test / client 三个构建目录均编译通过，25 个插件全部重编（首次
  `agentxx_javascript_engine` 因漏改一处 `unregister_tool` 而失败，改后通过）；
- `boundaries`、`plugins` 707、`plugin_host_mount` 147、`plugin_multi_mount` 47、
  `plugin_cleanup` 90、`plugin_runtime`、`plugin_sdk`、`plugin_bridge` → 2067 通过；
- `client_plugins` 835、`plugin_feature` 34、`plugin_point` 68、`plugin_broadcast` 90、
  `plugin_dispatch` 112、`plugin_agent_runtime` 221、`plugin_example_runtime` 50 → 2117 通过。

### 第 5 步：阶段 B 第二批 —— `permission` / `context` / `model` / `prompt`

改动：

- `plugin_api.h`：四张表抬版并改成异步操作契约
    - `agentxx.agent.permission` v2 → v3（三个成员；`check_paths` 的判定数组经
      `payload` 以 JSON 数组文本交回，如 `[1,0,2]`）；
    - `agentxx.agent.context` v2 → v3（`get_messages` 给 JSON 数组文本，
      `messages_count` 给十进制整数文本）；
    - `agentxx.agent.model` v2 → v3（`get_config` 给配置 JSON）；
    - `agentxx.agent.prompt` v3 → v4（`get_prompt` / `set_prompt` / `get_tool_prompt`）。
- `plugin_manager_vtable.cpp`
    - 新增 `asyncQueryCall`（只读查询：允许关闭中调用，空结果按失败交回，与旧
      `ERR_NOT_FOUND` 一致）与 `asyncScopedTableCall<AllowClosing>`；
    - `check_paths` 的入参（路径数组 + 会话）在受理阶段就拷成本地副本，工作体拼 JSON 数组；
    - 新增 `ToolPermissionSpecKeep`（三个借用字符串视图的自持拷贝，同 `ToolSpecKeep`）。
- `plugin_kit.h`
    - 新增业务表便捷入口 `promptJson` / `setPromptJson` / `modelConfigJson`
      （阻塞形态，给生命周期与工具体里的同步写法用）；
    - `toolPrompt` / `getSessionMessages` / `sessionMessagesCount` 改走阻塞形态；
    - `registerToolPermission` / `checkPathDecisions` 改走阻塞形态，
      `checkPathDecisions` 改为解析 `payload` 的 JSON 数组。
- 插件迁移：`agentxx_codegraph` / `agentxx_execute_command` / `agentxx_llm_uncensored` /
  `agentxx_planning` / `agentxx_rag_search` / `agentxx_websearch` / `example_plugin`
  改为调用 kit 便捷入口（提示词读写与模型配置）；JS 引擎的 `getPrompt` / `setPrompt` /
  `getToolPrompt` / `getMessages` / `messagesCount` / `getModelConfig` / 工具权限声明与
  路径查询改走阻塞形态（脚本线程不是主程序 IO 线程）。
- 测试：`test_plugins.cpp` 的提示词/权限用例改成阻塞形态断言（`check_paths` 解析 JSON 数组）；
  `test_plugin_abi_c17.c` 的 permission 版本断言 2 → 3。
- 文档：`plugins.md` §8.1 四张表抬版、§8.0 的当前进度。

验证：

- 内层 lib / test / client 三个构建目录编译通过，25 个插件全部重编（其中
  `example_plugin` 的一处替换脚本写坏了引号转义，修好后通过）；
- `boundaries` 19、`plugins` 709、`plugin_runtime` 730、`plugin_sdk` 134、
  `plugin_bridge` 193、`plugin_cleanup` 90、`plugin_host_mount` 147、
  `plugin_multi_mount` 47 → 2069 通过（首轮 `plugin_runtime` 有 1 条失败：
  `test_plugin_abi_c17.c` 里 permission 版本还写死 2，改后通过）；
- `client_plugins` 835、`plugin_feature` 34、`plugin_point` 68、`plugin_broadcast` 90、
  `plugin_dispatch` 112、`plugin_agent_runtime` 221、`plugin_example_runtime` 50 → 2140 通过。

### 第 6 步：阶段 B 第三批 —— `resources` / `graph` / `mount`

改动：

- `plugin_api.h`
    - `agentxx.agent.resources` v2 → v3（7 个成员全部异步；`get_own_resources` 的清单经
      `payload` 交回）；
    - `agentxx.agent.graph` v2 → v3（5 个成员；注册类完成时已生效，读取类经 `payload`）；
    - `agentxx.agent.mount` v1 → v2（`list` / `get`）。
- `plugin_manager_vtable.cpp`
    - 新增 `GraphNodeTypeSpecKeep`（`type` + `config_schema_json` 两个借用视图的自持拷贝）；
    - 新增 `resourceOp(...)`：六个资源登记/注销成员只差"调哪个挂载方法"，收成一处
      （受理阶段校验入参并拷字符串，工作体在 IO 线程执行），六个入口各三行；
    - `mount.list` / `mount.get` 走 `asyncTableCall<AllowClosing>`，`graph.get_*` 与
      `resources.get_own_resources` 走 `asyncQueryCall`。
- `plugin_kit.h`
    - 新增 `graphJson` / `graphName` / `setGraphJson` / `ownResourcesJson` /
      `registerSkillDir` / `unregisterSkillDir` / `registerMemoryFile` /
      `unregisterMemoryFile` / `registerMcpServer` / `unregisterMcpServer`
      （与 `promptJson` 等同一批阻塞形态便捷入口）；
    - `listMounts` / `mountInfoOf` 与图节点注册助手改走阻塞形态。
- 插件迁移：`example_graph_node`（`graphJson` / `setGraphJson`）、`example_resources`
  （`registerSkillDir`）、JS 引擎（图节点注册与撤销、技能/记忆/MCP 六处、图读写三处、
  挂载查询两处、资源清单一处）。
- 测试：`test_plugin_resources.cpp`（8 处登记/查询改阻塞形态断言）、
  `test_plugin_runtime.cpp`（生命周期里的图节点注册）、`test_plugin_sdk.cpp`
  （fake graph 表签名）。
- 文档：`plugins.md` §8.1 三张表抬版、§8.0 的当前进度。

验证：

- 内层 lib / test / client 三个构建目录编译通过，25 个插件全部重编
  （`agentxx_javascript_engine` 首轮因两处 `iface` 引用不在作用域内失败：一处改用
  `AgentIfaces::query(instanceHost)`、一处漏改的 `unregister_mcp_server`，改后通过）；
- `boundaries` 19、`plugins` 709、`plugin_runtime` 730、`plugin_sdk` 134、
  `plugin_bridge` 193、`plugin_cleanup` 90、`plugin_host_mount` 147、
  `plugin_multi_mount` 47、`plugin_resources` 101；
- `client_plugins` 835、`plugin_feature` 34、`plugin_point` 68、`plugin_broadcast` 90、
  `plugin_dispatch` 112、`plugin_agent_runtime` 221、`plugin_example_runtime` 50 → 1410 通过。

遇到的问题（已解决）：

- `plugin_resources` 在 ASan 下报 heap-use-after-free：测试里
  `PluginStringView::fromCstr(runtimeSkill.string().c_str())` 取的是临时串的指针，
  视图在语句结束就悬空（旧实现同样 UB，只是以前没被抓到）。改成先把
  `path.string()` 落到具名 `std::string` 再取视图。

### 第 7 步：阶段 C —— `agentxx.agent.runtime`

改动：

- `plugin_api.h`
    - `agentxx.agent.runtime` v2 → v3，`MIN_VERSION` 1 → 3，`BASELINE_SIZE` 改成
      `sizeof(AgentxxPluginRuntimeIface)`（整表一次改完，不需要"基线停在追加前"那套了）；
    - 21 个成员全部换成 `PluginxxOperatorHandle*(host, ..., cb, ud, error_out)`，
      去掉 `out_root_id` / `out_channel_id` / `out_trigger_id` / `out_timer_id` /
      `out_async_id` / `out_json` 六个出参；句柄 id 与查询结果都经完成回调的 `payload`
      交回（id 是十进制文本）。
- `plugin_manager_vtable.cpp`
    - 新增 `runtimeOp(...)`（容器级入口统一入口：受理 + 投递 + 交回）、
      `throwRuntimeFailure(what, rc, detail)`（业务失败 → `payload` 写
      `<成员名>: <resultName>`，有细节时附在括号里）、`rejectTableOp(error_out, msg)`
      （受理阶段参数错：同步返回 `NULL`）；
    - 新增四个"spec + 自持字符串"结构 `MountRootSpecKeep` / `ChannelSpecKeep` /
      `TriggerSpecKeep` / `TimerSpecKeep`：spec 里的字符串视图借用调用方内存，而工作体
      可能排到 IO 线程才执行，必须把字符串和 spec 放进同一个对象并由工作体捕获；
    - 21 个入口重写；`get_async_work` 对"不存在 / 非本实例提交"按 `not_found` 交回
      （旧实现返回非 OK 码，这里保持同样的可区分性）；
    - `submit_async_work` 补回 `kind=tool` 必须有 `tool_name` 的受理阶段检查。
- `plugin_kit.h`
    - 新增 `detail::runtimeCall` / `runtimeCallText` / `runtimeCallId` / `takeRuntimeText`
      四个阻塞形态助手（生命周期与工具体里的同步写法用）；
    - `mountRoot` / `unmountRoot` / `openChannel` / `closeChannel` / `sendInput` /
      `cancelChannel` / `selectModel` / `respondInterrupt` / `channelStateJson` /
      `channelContextMessages` / `listRuntimeJson` / `addTrigger` / `removeTrigger` /
      `emitEvent` / `addRuntimeTimer` / `removeRuntimeTimer` / `submitAsyncWork` /
      `cancelAsyncWork` / `getAsyncWorkJson` / `listAsyncWorkJson` 全部改走这些助手
      （对外签名不变，插件源码零改动）；
    - 顺手删掉 `resourceOpSync` 的未使用参数 `agent`（编译告警 C4100）。
- JS 引擎（`agentxx_javascript_engine.cpp`）：新增文件级 `callRuntimeOp(host, iface, start)`
  （`tableOpBlockingRaw` 的薄包装），19 个桥调用点（后台任务 4 个 + 运行时 15 个）全部改用它；
  脚本侧行为不变（`mountRoot`/`registerTrigger`/`registerTimer` 仍同步返回 id，
  `listRuntime`/`getChannelState` 仍同步返回 JSON）。`list_runtime` 一类的失败仍然降级成
  空数组 / `null`。
- 测试（`test_plugin_agent_runtime.cpp`）：
    - 新增 `RuntimeReply`（`accepted` / `ok` / `text` / `error`）与
      `callRuntime<&AgentxxPluginRuntimeIface::xxx>(host, runtime, args...)` 模板：
      成员指针 + 原样参数，完成回调由模板补在末尾，测试里仍保持"调用即得结果"的写法；
    - 新增 `failedWith(reply, "invalid_argument")`：业务失败的原因文本里带
      `resultName` 的名字，用它断言"被哪一类原因拒"（原来的 `AGENTXX_RESULT_ERR_*`
      数字在异步契约里不再作为返回值出现）；
    - 100 处调用点按新形态重写；`list_runtime(host, nullptr)` 那条"参数错"改成
      "没有完成回调也算受理"（`cb` 可空是契约的一部分）；
    - 禁用插件后的查询从"受理后空结果"改成"未受理"（框架不再发放 admission lease）。
- 文档：`plugins.md` §8.1 的 runtime 行（→ v3）、§8.0 当前进度、§19.2 入口表（去掉
  `out_*` 出参、改述返回码语义）；`AGENTS.md` 的 runtime 段落补 v3 说明。

验证：

- 内层 lib / test / client 三个构建目录编译通过；25 个插件全部重编
  （`[build_plugins] done in 480.2s (25/25 targets ok)`）；
- `boundaries` 19、`plugins` 709、`plugin_runtime` 730、`plugin_sdk` 134、
  `plugin_bridge` 193、`plugin_cleanup` 90、`plugin_host_mount` 147、
  `plugin_multi_mount` 47、`plugin_resources` 101、`plugin_agent_runtime` 221、
  `plugin_example_runtime` 50 → 2441 通过；
- `client_plugins` 835、`plugin_feature` 34、`plugin_point` 68、`plugin_broadcast` 90、
  `plugin_dispatch` 112、`chatbot_task` 138、`plugin_example_chatbot` 265 → 1542 通过。

遇到的问题（已解决）：

- **ASan 抓到 heap-use-after-free**（`plugins` 模块，JS 脚本在工具回调里调
  `registerTrigger`）：`spec` 的字符串视图指向 `shared_ptr<string>` 里的内存，而工作体
  lambda 只捕获了 `shared_ptr<Spec>`、没捕获那几个字符串 —— 工作体排在 IO 线程执行时
  字符串已经释放。首轮修改是把它们一起捕获，随后改成四个 `*SpecKeep` 结构（字符串与
  spec 同一对象，构造时重新指向），从结构上消除这个坑。`mount_root` / `open_channel` /
  `register_timer` 三处是同一个错。
- 首轮 `plugin_agent_runtime` 6 条失败，逐条对齐契约后通过：
  ① 没有容器时 `mount_root` 是**受理后**失败（`unsupported`），不是未受理；
  ② `kind=tool` 缺 `tool_name` 的检查在改异步时丢了，补回受理阶段；
  ③ `get_async_work` 对"别人的任务"旧实现返回非 OK 码，改异步后要显式抛
  `not_found`，否则空 `payload` 被当成成功；
  ④ 禁用后 `list_runtime` 也不再生效（未受理），不能再按"空清单"断言。

### 第 8 步：阶段 C2 第一批 —— 框架表 `config` / `plugins`

改动：

- `pluginxx/api/tables.h`：`config` v3 → v4（5 个成员）、`plugins` v1 → v2（3 个成员），
  `MIN_VERSION` 一起抬，基线重置为整表长度；成员签名改成
  `PluginxxOperatorHandle*(host, ..., cb, ud, error_out)`，配置 JSON / 语言码 /
  插件清单经完成回调的 `payload` 交回。
- `pluginxx/host/tables_impl.h`
    - 新增 `detail::asyncTableOp`（受理 + 投递 + 交回，与主程序业务表同一套骨架）与
      `detail::asyncQueryOp`（只读查询，空结果按 `not_found` 交回）；
    - 8 个入口重写：`config.get_config/get_plugin_args/get_plugin_config_path/get_language/
      set_language`、`plugins.list_plugins/get_plugin/get_own_info`；
    - `get_plugin` 在受理阶段检查名字（空名字 = 未受理）。
- `pluginxx/kit/kit.h`
    - 新增 `detail::tableOpText`（只取结果正文）与 `detail::tableOpOk`（只看成败），
      在实例上下文基类之前先声明、文件后半定义（`tableOpBlockingRaw` 同名前置声明放
      `namespace pluginxx`，不能放 `detail`，否则与真实定义构成两个重载）；
    - `PluginBaseT` 的 `config` / `argsJson` / `configPath` / `language` / `setLanguage`
      改走这两个助手（对外签名不变，插件源码零改动）。
- `agentxx` 侧
    - `plugin_kit.h`：`ClientIfaces` 补 `cancel` / `scheduler` 两个成员（client 侧脚本
      也要能按阻塞形态调表）；把 `tableOpText` / `tableOpOk` 引进 `agentxx::plugin::detail`；
    - JS 引擎：新增 `callTableText`（模板化，agent / client 两侧的接口表聚合都能用），
      14 个调用点（`listPlugins` / `getPlugin` / `getConfig` / `getPluginArgs` /
      `getLanguage` / `setLanguage` / `getOwnInfo` / `getConfigPath`，两侧各一套）改用它；
    - `agentxx_codegraph` / `agentxx_llm_uncensored` / `agentxx_planning` /
      `example_js` / `example_js_execute_command` / `example_resources` 里 9 个直接调用点改走
      `detail::tableOpText`。
- 测试：`test_plugins.cpp`（`config.get_language` / `set_language` 改阻塞形态）、
  `test_plugin_runtime.cpp`（`config.get_plugin_args` 改阻塞形态，失效实例断言改成
  "未受理/失败"而不是返回非 0 码）。
- 文档：`plugins.md` §8.2 两张表抬版、§8.0 当前进度。

**顺带修掉的框架 bug**（ASan 抓到，非本次改动引入）：`PluginHostLifecycle::
clearPluginOwnedRegistrations` 原先连 `inst->subscriptionHandles` 一起清空，而那份
句柄的契约是"随实例析构释放"。插件在 stop/disable 之后（或自己的线程上）对旧句柄调
`unsubscribe_broadcast` 就成了 use-after-free —— `client_plugins` 里 JS 脚本回滚路径
实测崩在 `PluginxxSubscription::alive.compare_exchange_strong`。现在只清
`subscriptions`（注册表侧簿记，撤销时已置 `alive=false`），句柄本体留到实例析构；
再传旧句柄进来走幂等空操作分支。

验证：

- 框架库 / 内层 lib / client / 25 个插件全部编译通过（`[build_plugins] done in 471.6s`）；
- `boundaries` 19、`plugins` 704、`plugin_runtime` 728、`plugin_sdk` 134、
  `plugin_bridge` 193、`plugin_cleanup` 90、`plugin_host_mount` 147、
  `plugin_multi_mount` 47、`plugin_resources` 101、`plugin_agent_runtime` 221、
  `plugin_example_runtime` 50、`client_plugins` 835、`plugin_feature` 34、
  `plugin_point` 68、`plugin_broadcast` 90、`plugin_dispatch` 112、`chatbot_task` 138
  → **3711 通过、0 失败**。

遇到的问题（已解决）：

- `tableOpBlockingRaw` 的定义在 `namespace pluginxx`、返回 `detail::TableOpResult`：
  我最初把前置声明放进 `detail`，于是同一调用点上出现两个重载（`detail::` 与 `pluginxx::`）
  → C2668 调用不明确。改成"前置声明放 `pluginxx`、`detail` 里的助手显式限定
  `pluginxx::tableOpBlockingRaw`"。
- `ClientIfaces` 原本没有 `cancel` / `scheduler`：client 侧脚本的阻塞形态拿不到这两张表，
  在聚合里补上（同一个 `query()` 里按 IID 查）。
- 插件 DLL 与框架表布局不匹配时症状是**运行期崩溃**（旧签名调用新 vtable）：抬高
  通用表版本后必须重编全部插件，不能只编测试。
- `libagentxx_staticd.lib` 出现过一次 `LNK1243: COMDAT ... 关联`：删掉对应
  `.obj` 重编即可（增量构建产物不一致，与代码无关）。

## 与计划的偏差

- 计划 §7.3 写"全局版本 5 → 6"：实际第 6 代已被占用，改为 6 → 7。
- 计划 §7.1 把"作用域失效"一律算**未受理**。实现上只能拆两半: 引用的**形状**无效
  (空指针 / `id` 为空 / `struct_size` 越界) 同步返回 `NULL`; 引用指向的挂载不存在或
  序号过期要读挂载表, 只能在 IO 线程上判, 按**受理后失败**交回。这样既不跨线程读表,
  也不把错误退化成"永远成功"。已写进 `plugins.md` §8.0。
- `tableOpBlocking` 在计划里写的是"io 线程直接返回错误"；实现上先看主程序有没有
  内联完成（表操作在调用方就是 IO 线程时是内联的），只有确实没完成才返回错误。
  这样 `mount`/`start` 回调里原有的同步写法（`workDir` / `addShareStore`）行为不变，
  同时不会死锁。

## 剩余事项

- 阶段 C2 还剩框架表 `broadcasts`（含 `subscribe` 的句柄如何经 `payload` 交回）、
  `capabilities` 登记、`net.call` 合并 `call_async`；阶段 D（`agentxx.client.*` 六张 +
  三个回答型回调）待做。
- 阶段 E 还剩：删残留同步实现路径、client 侧 150 ms 同步桥改 Promise、
  `AGENTS.md` 的调用模型说明。
- `KeyMetrics` 的表调用耗时统计（计划 §10）尚未加。
- **子模块改动要在 `cxx_pluginxx` 仓库里单独提交**：`agent/third_party/cxx_pluginxx`
  是子模块，父仓库只能记录指针。前两个 commit 因此不含它的改动（只含 agent/docs），
  指针在本次提交里一并对齐。
