# 插件接口表异步化 —— 实施记录

> 计划见 [plan.md](plan.md)，任务卡片见 [index.md](index.md)。
> 本文件只记"实际做了什么 + 怎么验证 + 与计划的偏差"，每完成一步就写。

## 进度总表

| 阶段 | 内容 | 状态 |
|---|---|---|
| A | 全局 API 版本 / 异步表调用骨架 / kit 三形态 / `agentxx.agent.session` 试点 | 完成（编译通过、相关测试全绿） |
| C2-槽位清理 | 删 `scheduler.post_to_io` / `scheduler.op_cancel` / `capabilities.op_cancel`；`coroutine_runtime` 整张表并回 `scheduler`（通用表 10 → 9） | 完成（编译通过、全量插件测试全绿） |
| A2 | agent 侧 `dlopen` + `create` 挪到加载线程池 | 完成（编译通过、插件测试全绿） |
| B | `agentxx.agent` 业务表全部成员 | 待做 |
| C | `agentxx.agent.runtime` 全部成员 | 待做 |
| C2-表异步化 | 框架表其余成员（broadcasts / capabilities 登记 / config / plugins / `net.call`）改异步 | 待做 |
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

## 与计划的偏差

- 计划 §7.3 写"全局版本 5 → 6"：实际第 6 代已被占用，改为 6 → 7。
- `tableOpBlocking` 在计划里写的是"io 线程直接返回错误"；实现上先看主程序有没有
  内联完成（表操作在调用方就是 IO 线程时是内联的），只有确实没完成才返回错误。
  这样 `mount`/`start` 回调里原有的同步写法（`workDir` / `addShareStore`）行为不变，
  同时不会死锁。

## 剩余事项

- 阶段 B / C / C2-表异步化 / D 全部待做（见进度总表），即 104 个成员里已改异步的
  只有 4 个（`agentxx.agent.session`）+ 骨架与三形态。
- 阶段 E 还剩：删残留同步实现路径、client 侧 150 ms 同步桥改 Promise、
  `AGENTS.md` 的调用模型说明。
- `KeyMetrics` 的表调用耗时统计（计划 §10）尚未加。
- **子模块改动要在 `cxx_pluginxx` 仓库里单独提交**：`agent/third_party/cxx_pluginxx`
  是子模块，父仓库只能记录指针。前两个 commit 因此不含它的改动（只含 agent/docs），
  指针在本次提交里一并对齐。
