# 插件接口表异步化 —— 实施记录

> 计划见 [plan.md](plan.md)，任务卡片见 [index.md](index.md)。
> 本文件只记"实际做了什么 + 怎么验证 + 与计划的偏差"，每完成一步就写。

## 进度总表

| 阶段 | 内容 | 状态 |
|---|---|---|
| A | 全局 API 版本 / 异步表调用骨架 / kit 三形态 / `agentxx.agent.session` 试点 | 进行中（`session` 表 4 个成员已改完并编译通过、相关测试全绿） |
| A2 | agent 侧 `dlopen` + `create` 挪到加载线程池 | 待做 |
| B | `agentxx.agent` 业务表全部成员 | 待做 |
| C | `agentxx.agent.runtime` 全部成员 | 待做 |
| C2 | 框架表（`cxx_pluginxx`）：`coroutine_runtime` 并回 `scheduler`、删 `post_to_io`、`op_cancel` 合并、`net.call` 统一异步、其余通用表异步 | 待做 |
| D | `agentxx.client.*` 六张表 + 三个回答型回调 | 待做 |
| E | 删残留同步路径与 client 侧 150 ms 同步桥、文档 | 待做 |

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

## 与计划的偏差

- 计划 §7.3 写"全局版本 5 → 6"：实际第 6 代已被占用，改为 6 → 7。
- `tableOpBlocking` 在计划里写的是"io 线程直接返回错误"；实现上先看主程序有没有
  内联完成（表操作在调用方就是 IO 线程时是内联的），只有确实没完成才返回错误。
  这样 `mount`/`start` 回调里原有的同步写法（`workDir` / `addShareStore`）行为不变，
  同时不会死锁。

## 剩余事项

- 阶段 A2 起全部待做（见进度总表）。
- `KeyMetrics` 的表调用耗时统计（计划 §10）尚未加。
- `plugins.md` / `AGENTS.md` 的接口表数量与调用模型说明尚未更新（阶段 E）。
