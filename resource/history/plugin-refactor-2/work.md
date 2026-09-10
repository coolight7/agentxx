# 插件框架 Reset-v1 重构交接摘要（进度 / 提交边界 / 验证事实）

> 事实来源：设计定稿是 `resource/history/plugin-refactor-2/plugin.md`（Reset-v1 方案、R0-R6 阶段、F/P 问题编号、测试矩阵）。本文件只记录进度、提交边界、验证结果和待办；与 plugin.md 冲突时以 plugin.md 为准。
>
> 本文件更新时间：2026-09-11。**状态：Reset-v1 未完成。** 当前仓库 = 两个重构提交（`3a4497ba`、`f861bcf9`）+ 一份较大的未提交工作树增量。提交与未提交内容的分工见第 3、4 节；已完成/待完成对照见第 5、6 节。

---

## 0. 新会话执行须知

### 0.1 第一步：确认状态

```bash
cd /home/coolight/program/agentxx
git status --short --branch
git log --oneline -6
git diff --stat
git diff --cached --stat
git diff --check
```

阅读顺序：

1. `plugin.md`（方案、R1-R6 验收标准、第 11 节测试矩阵）
2. 本文件第 1 节（总览）、第 4 节（未提交增量）、第 5 节（已完成）、第 6 节（待完成）、第 9 节（下一步）
3. `git show 3a4497ba`、`git show f861bcf9`；未提交增量用 `git diff`（当前 `git diff --cached` 为空）

### 0.2 工作树保护规则（不得违反）

- 禁止 `git reset --hard`、`git checkout --`、清空 build 目录或批量删除测试。
- 当前未提交增量是继续中的重构成果，不是脏数据；不要为了“干净”而丢弃。
- 用户相关文件：`TODOS.md` 仍是未提交的用户修改（不要顺手提交或回退）；`agentxx-config.yaml`、`resource/history/plugin-refactor-2/index.md` 已随 `3a4497ba` 入库。
- 提交时只包含本任务相关文件，不要把 `TODOS.md` 等用户改动一起提交。

### 0.3 构建与测试基线

```bash
# 构建（高并行；GCC 16.1 偶发 ICE 时直接重试同一构建目录，不要清空 build）
cmake --build agent/build/linux-debug --target agentxx_test_repo -j12

# 插件专项
timeout 600s agent/build/linux-debug/exec/agentxx_test \
  plugin_runtime plugins plugin_resources plugin_multi_instance client_plugins --fail-fast

# 含 ffi / host / subagent 的扩展回归（ASan + LSan）
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 timeout 1500s \
  agent/build/linux-debug/exec/agentxx_test \
  ffi_c_api agent_host subagent_tool subagent_bus plugin_runtime plugins \
  plugin_resources plugin_multi_instance client_plugins agent memgrowth --fail-fast
```

当前结果与日志见第 7 节；二进制 `agent/build/linux-debug/exec/agentxx_test` 比最新源码新，构建是最新的。

---

## 1. 状态总览（对照 plugin.md 第 10 节的 R0-R6）

| 阶段 | 状态 | 事实依据 |
|---|---|---|
| R0 契约冻结 | 完成 | `plugin.md` 定稿；本文件只做进度记录 |
| R1 Runtime / Operation | 基本完成，待收尾 | `plugin_runtime.h`/`op_driver.h` 已重写并提交（`3a4497ba`）；未提交增量补齐完成端点、executor 停止重放、`ioCallSync` 快速失败、idle/lease 守卫；仍缺 cancel/done 线性化收敛、完成投递失败路径、故障注入矩阵 |
| R2 加载事务 / 异步关闭 | 部分完成 | 名称预占、`Loading/Ready/Closing/CloseFailed`、`create/start/stop/destroy`、`shutdownAsync`、owner 顺序、`GraphTypeSlot` 已落地（未提交）；仍缺 Client semantic renderer cache、prompt contribution、enable/disable 事务化、注册事务全覆盖与回滚测试 |
| R3 ABI v1 / SDK | 大幅推进，未完成 | 接口表 `struct_size` + SDK 严格校验、opaque CancelToken、scheduler v1（删 `pump_io`/`cancel_sleep`/`volatile`）、tasks handle 语义、SDK scheduler/offload 迁移已落地（未提交）；仍缺 SDK `Request` 输入所有权、统一 root adapter、hook `Task<void>` 区分、C17/ABI layout 与正反例编译测试、导出符号检查 |
| R4 内置插件 / JS / 平台 | 少量迁移 | 4 个内置插件已迁到 CancelToken 新签名；`f861bcf9` 修了平台插件构建；仍缺其余插件迁移、JS 事务/Promise、Windows 平台 gate |
| R5 Client / 依赖 / prompt | 部分完成 | 事件逐 callback 复查 alive、renderer lease、`blockedByDependencies`、动作派发校验已落地；仍缺语义 renderer cache 与代次、动作代次、prompt 多 owner 合成、enable/disable 事务与依赖恢复测试 |
| R6 验证 / 文档 / 发布审查 | 未开始 | 未跑全模块/UBSan/TSan/Windows；未做导出符号与 C17 ABI 检查；`docs/zh-cn/design/plugins.md` 未更新 |

结论：不能把当前状态写成“Reset-v1 完成”。下一阶段建议见第 9 节。

---

## 2. 提交边界与工作树状态

```text
基线（重构前）   a805f9cb  --
提交 1          3a4497ba  重构插件框架-R1-1 Runtime / Operation 部分实现   (2026-09-09 14:00 +0800)
提交 2          f861bcf9  重构插件框架-fix-build                          (2026-09-09 18:32 +0800)
工作树          未提交增量（31 文件，+2676/-585），详见第 4 节
```

当前 `git status`：`TODOS.md` + 重构相关文件为未 staged 修改；`git diff --cached` 为空；无 untracked 文件。

---

## 3. 已提交内容明细

### 3.1 `3a4497ba` 重构插件框架-R1-1 Runtime / Operation 部分实现

21 文件，+2193/-1121。核心是把插件异步执行收敛到统一的 Operation/Lifetime：

- 新增 `agent/lib/include/agentxx/plugin/plugin_runtime.h`：`PluginInstanceState`（Loading/Ready/Disabled/Closing/Closed/CloseFailed）、`InstanceLifetime`（原子 state + admission 位 + lease count + `waitIdleUntil` 事件式等待）、`InstanceLease`、`PluginRuntime`（executor、IO thread id、Operation 表）。
- 重写 `agent/lib/include/agentxx/plugin/op_driver.h`：`OpCore` 统一 start/done/cancel/句柄回收，provider/caller 双 lease，完成 payload 拥有化后再进 IO 线程发布。
- `plugin_manager_base.h` / `plugin_common.h`：删除 `ioTasks_` 二级队列与捕获裸 `this` 的投递，`waitInflightZero` 改为 idle 事件等待。
- `plugin_manager_scheduler.cpp`（post/sleep/offload 接入 OpCore）、`plugin_manager_tasks.cpp`（`register_task` 返回宿主托管 handle + `cancel_task`）、`plugin_manager_capability.cpp`（provider start 在 IO 线程、输入拥有化）、`plugin_manager_adapters.cpp`、`plugin_manager_lifecycle.cpp`（`pluginCreated/pluginDestroyed/destroyDeferred` + lease 守卫）、`plugin_manager_vtable.cpp`、`client_plugin_manager.{h,cpp}` 同步接入。
- 新测试 `agent/test/plugin/test_plugin_runtime.{h,cpp}`（472 行）并注册到 `agent/test/test.cpp`；`test_plugins.cpp` 两处宿主协程内同步 `engine->run()` 改为 `co_await engine->run_async()`。
- 同提交还包含既有用户文件改动：`TODOS.md`、`agentxx-config.yaml`、`resource/history/plugin-refactor-2/index.md`，以及本文件的初版（407 行）。

注意：R1-1 之后没有再产生提交；旧文档中“未创建提交”的说法已过期。

### 3.2 `f861bcf9` 重构插件框架-fix-build

8 文件，+100/-23，全部是构建/平台修复，不含框架语义变化：

- `plugin_kit.h`：`util::insertOrAssignHeterogeneous` 替代异构 map 直接下标赋值，并补 include。
- `util/json.h`：补 `<ostream>`。
- `agentxx_rag_search/CMakeLists.txt`：补 `html2md` 依赖（含 builtin 传递）。
- `agentxx_audio_stream.cpp`：`AudioDataSource source_` 改为实例成员，`start` 失败不再记录为运行中。
- `agentxx_computer_use/ui_control.cpp`、`agentxx_execute_command.cpp`（setup 抽函数）、`agentxx_screen_capture.cpp`：平台入口/编译修复。
- `agent/test/plugin/test_text_selection_monitor.cpp`：+33 行测试。

---

## 4. 未提交增量明细（按阶段归类）

范围：31 文件，+2676/-585。以下按 plugin.md 的阶段归类，便于继续推进。

### 4.1 R1 收尾（Operation / Runtime 可靠性）

- `plugin_runtime.h`：新增 `RuntimeAction` 待办队列与 `enqueueRuntimeAction()`；executor 停止期间的完成/取消/idle 动作不丢失，executor 重绑后重放；`runtimeExecutorStopped()` 直接识别 `asio::io_context::executor_type`（不能用 `dynamic_cast`，Boost Asio 的 `execution_context` 不是多态类型）。
- `op_driver.h`：新增 `AgentxxPluginOperationCompletionEndpoint`（宿主 tombstone，迟到 `done` 只丢弃不 UAF）；完成提交 exactly-once；callback 与内部 completion handler 异常隔离；取消请求在 executor 停止时可重放。
- `plugin_manager_base.h`：`isIoThread()` 在 direct io_context 已停止时返回 false；`ioCallSync()` 在 runtime 不可用时快速失败（不再永久等 future）；新增 `ioExecutor()`、`hasPendingClose()`、`lifecycleStopPending()`。
- `plugin_manager_vtable.cpp` / `plugin_manager_scheduler.cpp` / `plugin_manager_adapters.cpp`：适配新 scheduler/tasks ABI、填充 `struct_size`、GraphTypeSlot 注册与冲突拒绝。
- 测试：`agent/test/plugin/test_plugin_runtime.cpp` +249 行（1000 并发完成、done/cancel 竞速、executor 停止重放、exactly-once、`ioCallSync` 快速失败、stop 未完成时禁止 destroy/dlclose、client 侧镜像守卫）。

### 4.2 R2 加载事务 / 关闭 / owner 顺序

- 生命周期入口：`AgentxxPluginStartFn/StopFn`、`agentxx_plugin_agent_start/stop`、`agentxx_plugin_client_start/stop`、内置插件 `start/stop` 字段；`plugin_manager_lifecycle.cpp` 与 `client_plugin_manager.cpp` 在 create 后调 start、Closing 时调 stop；`lifecycleStarted` 语义为“实例已激活”（无 start 导出的 legacy 插件在 Ready 前置位）。
- 名称预占：`reservePluginName()/releasePluginName()/isPluginNameLoading()`（`plugin_manager_base.h`），重复加载在 Loading 阶段被拒绝。
- 关闭：`unloadAsyncUntil()` 共享绝对 deadline；agent/client `shutdownAsync()`；失败置 `CloseFailed` 且可重试；同步 `shutdownAll()`/idle cleanup/实例析构在 stop 未完成或 lease 非零时不再 destroy/dlclose（保留 ctx 与 DSO 并报错）。
- owner 顺序：`BaseAgent::shutdownAsync()`（`base_agent.cpp:1112`，投递到插件管理器自己的 executor）、`AgentContext::shutdownPluginsAsync()` 与析构自检、`AgentHost::destroyAgentAsync()`（`agent_host.cpp:975`，先子后父）与同步 `destroyAgent` 告警、`FfiAgentRuntime::stopInternal()` 在停止 agent ioCtx 前 await 关闭（`ffi_runtime.cpp:540`）、`mode_runners.cpp` 本地 CLI/TUI 退出前 `shutdownAgentPlugins()` + client `shutdownAsync()`。
- Graph：`GraphTypeSlot` + `PluginGraphNode` 代次校验（`plugin_graph_node.{h,cpp}`），旧节点在卸载/重载后返回“插件已关闭/代次失效”，不调用插件回调。
- Client：`unloadAsyncUntil`/`shutdownAsync`/renderer lease/`dispatchEvent` 逐 callback 复查。

本轮发现的真实缺陷并已修复：`BaseAgent::shutdownAsync` 原先投递到 `ioCtx`，而宿主持有调用方 executor 直跑的子代理（engine 直跑）其自身 `ioCtx` 从未 `run()`，导致 `agent_host` 模块永久挂起；改为投递 `pluginManager->ioExecutor()` 后 `agent_host` 95/0 通过。

### 4.3 R3 ABI v1 / SDK（推进最多的一块）

- 接口表：全部 `_reserved` 改为 `struct_size`（`plugin_api.h`、`client_plugin_api.h`），宿主 vtable 填充 `sizeof(表)`，SDK 侧严格校验 `version == 1 && struct_size >= sizeof(Iface)`（`plugin_kit.h:356`）；插件 `get_info().api_version` 要求精确相等（agent/client 加载路径）。
- 取消：新增 opaque `AgentxxPluginCancelToken` + `agentxx_plugin_cancel_is_requested()`；删除 ABI 层的 `volatile int32_t* cancel_flag`。
- Scheduler v1：`post_to_io` 返回状态、`sleep` 返回 Operation handle、通用 `op_cancel`、`offload` 工作函数接收 CancelToken，删除 `pump_io` / `cancel_sleep`。
- Tasks：`register_task` 返回宿主托管 handle + `cancel_task`（handle 语义在 `3a4497ba` 已入库）；SDK `spawn` 在无 tasks 表时明确失败，不再 unmanaged 降级。
- SDK：scheduler/offload/sleep/yield/call/cap 迁移到新 ABI；`OffloadAwaiter` 用 `std::monostate` 支持 `offload<void>`；`Task` 子任务 continuation 已接通。

### 4.4 R4 内置插件 / 平台

- 已迁移到 CancelToken/新 offload 签名：`agentxx_filesystem`、`agentxx_codegraph`、`agentxx_execute_command`、`agentxx_system_monitor`。
- 平台构建修复已入库（见 3.2 节）；JS 系列、example_*、websearch/rag/planning 等尚未迁移。

### 4.5 R5 Client / 依赖

- `dispatchEvent`：快照后逐 callback 复查 `alive`/`enabled`/lifetime 并持 lease（同轮退订安全）。
- renderer：`ClientToolRendererLease`（alive + weak instance），UI 路径调用前复查 lease/实例状态。
- 动作派发：命中绑定前复查插件存在/启用/绑定快照一致。
- `blockedByDependencies` 字段落地，disable 级联标记、enable 清除。

### 4.6 测试

- `test_plugin_runtime.cpp`：109 → 128（本工作树最新）
- `test_client_plugins.cpp`：300 → 309
- 其余模块数量见第 7 节。

---

## 5. 已完成任务对照（plugin.md R1-R6 / F、P 编号）

| 编号 / 条目 | 状态 | 位置 / 证据 |
|---|---|---|
| F01 互调 start 拒绝不回调、登记回滚 | 完成 | `op_driver.h` + `test_plugin_runtime` 拒绝用例 |
| F05 `shutdownAll` 不等待后台任务 | 结构层完成 | `InstanceLifetime` lease + idle 事件 + `shutdownAsync`；专项“shutdown 中后台 Task 挂起”测试仍缺 |
| F06 互调 start 在 IO 线程 | 完成 | `plugin_manager_capability.cpp` + `postToIo`/`ioCallSync` |
| F07 caller/provider 双 lease | 完成 | `OpCore` 的 provider_/caller_ guard |
| F08 sleep/post/offload 纳入 Operation 与回收 | 完成 | `plugin_manager_scheduler.cpp` + 测试 |
| F10 工具冲突不得写入实例记录 | 完成 | `plugin_manager_adapters.cpp` registerTool 返回值为唯一依据 |
| F13 SDK 借用参数跨挂起 | 未完成 | 仍无 `Request` 拥有模型（R3 待办） |
| F14 完成后再 cancel 不调用插件 | 完成 | `handle->completed` + `cancelFn` 失效 |
| F15 管理器销毁后队列 lambda 不访问裸 this | 部分完成 | 业务投递已改为拥有闭包；vtable 闭包仍捕获裸 `inst`/`mgr`（见第 8 节） |
| F18 Client 同轮派发复查 alive | 完成 | `client_plugin_manager.cpp:1511` dispatchEvent |
| P0 完成协议（拥有完成包、IO 线程一次性提交） | 完成 | `op_driver.h` commit 路径 + 完成端点 tombstone |
| P0 opaque CancelToken | 完成 | `plugin_api.h` + SDK 调用方 |
| P1-A 接口表严格协商 | agent/client 接口表完成 | `struct_size` 填充 + SDK 校验 + `api_version` 精确匹配 |
| P1-B 名称预占 / Loading 不可调用 | 完成 | `reservePluginName` + state 门禁 |
| P1-C GraphTypeSlot | 首版完成，缺测试 | `plugin_graph_node.h` + `plugin_manager_adapters.cpp`；缺旧节点/重载专项测试 |
| R2 owner 顺序（BaseAgent / AgentHost / Client runner） | 完成 | 见 4.2 节 |
| R2 Client semantic renderer cache | 未完成 | renderer 仍在 UI 线程同步调用（有 lease 保护） |
| R2/R5 prompt contribution | 未完成 | 仍是“备份后无条件写回”模型 |
| F02/F03/F04/F16/F17/F19/F20/F21、P0-C | 未完成或仅部分 | 见第 6、8 节 |
| R6 验证 / 文档 | 未开始 | 见第 6、7 节 |

---

## 6. 待完成任务（按优先级，含验收要求）

### P0-1 宿主控制块：让迟到插件调用安全失败（R2/R3 交叉）

现状：宿主交给插件的 `host` 结构位于 `PluginInstance` 对象内部，`host->opaque` 直接是实例裸指针（`plugin_manager_vtable.cpp:17`、`client_plugin_manager.cpp:1566`）；vtable 闭包还捕获裸 `inst`/`mgr`（agent 侧 `instOf/mgrOf` 共 84 处调用点）。

要求：

- `host` 视图移到宿主控制块（进程级稳定地址），`opaque` 改为一次性令牌；注销后查询返回“实例不存在”，vtable 一律安全失败（返回非 0 / NULL + error）。
- vtable 投递闭包改为持有 `shared_ptr<Instance/Manager>` 与 admission lease，使 unload 的 `waitInflightZero` 覆盖排队阶段。
- 注册类入口在执行时复查实例状态（Closing 后不再登记）。
- 回归：卸载/重载后调用旧 host 指针不崩溃；排队中的注册在卸载后被拒绝。

### P0-2 Operation 终态与取消线性化（R1 收尾）

- `OpCore::submitMutex_` 仍为 `recursive_mutex`：cancel 持锁调用插件、worker done 同时取锁，需要无死锁线性化协议（或明确单线程化）。
- 完成投递失败（executor 停止且无法重放）时的可观察终态尚未定义。
- 裸 handle 失效语义（旧调用方持 handle 迟到调用）需要与 P0-1 一并收敛。
- 验收：plugin.md 第 11.2 节的 1-10 全部成为自动化测试（当前覆盖第 1/2/3/4/7 条的一部分）。

### P1-1 SDK Request 与统一 root adapter（R3）

- 建立每次 root 操作的拥有型 `Request`（args/session/call_id/method + token 视图 + ctx lifetime），`std::string_view` 只在 Request 生命周期内有效（F13）。
- tool/hook/capability/graph/spawn 复用同一 root adapter（当前各自一套 Job/shim）。
- hook helper 按 callable 返回类型严格区分同步 `void` 与 `Task<void>`（F19）。
- `Task<T>`/`Task<void>`/`offload<void>` 组合、异常与取消传播的正反例编译测试。

### P1-2 C ABI v1 编译期与运行期检查（R3/R6）

- C17 `-pedantic-errors` 包含两个 ABI 头：`sizeof`/`offsetof`/对齐/调用约定断言。
- 未知/短接口表、NULL 必需函数、API 版本不匹配的安全拒绝测试。
- 导出符号只包含规定入口；第三方静态依赖符号保持隐藏。

### P1-3 Client semantic renderer cache 与动作代次（R2/R5）

- renderer 结果改为 Client IO 线程计算的宿主语义快照（displayName/summary/items + generation），UI 只读 cache；旧 snapshot 失效直接通用回退（F03、plugin.md 第 8.2 节）。
- 动作点击携带 plugin/generation/owner，IO 线程复查后决定丢弃或派发（plugin.md 第 8.3 节）。
- 回归：保留旧 renderer snapshot、旧 action 点击、重载同名插件。

### P1-4 注册事务与启停事务（R2/R5）

- create/start 失败必须回滚工具/hook/capability/event/resource/prompt/graph/UI 全部注册；当前只覆盖工具与部分资源。
- `enable/disable` 改为 start/stop 事务（现在仍是“重新 register 已保存工具”的旧模型）。
- prompt contribution 改为 owner + generation + key 合成（F20）。
- 依赖：三级/菱形禁用-恢复、userDisabled 与 blockedByDependencies 区分测试（F09）。

### P2-1 R4 插件迁移收尾

按 plugin.md 第 9 节顺序：example_plugin / example_resources / example_graph_node → string/math/system → websearch/rag/planning → system_monitor/codegraph 多实例 → JS（`callTool` Promise、删除 1ms 轮询、顶层异常事务、rejection/timeout 映射）→ Windows 平台 gate。

### P2-2 R6 验证与文档

- 全模块回归、ASan/UBSan/TSan 定向、Windows 编译与专项、多实例矩阵。
- 更新 `docs/zh-cn/design/plugins.md` 与测试说明。
- 明确“已验证平台”，不得用 Linux 结果替代 Windows/Android。

---

## 7. 验证记录（本工作树最新）

构建：

```bash
cmake --build agent/build/linux-debug --target agentxx_test_repo -j12
# 成功；二进制 agent/build/linux-debug/exec/agentxx_test 比最新源码更新
```

扩展回归（ASan + LSan 开启，`--fail-fast`）：

```bash
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 timeout 1500s \
  agent/build/linux-debug/exec/agentxx_test \
  ffi_c_api agent_host subagent_tool subagent_bus plugin_runtime plugins \
  plugin_resources plugin_multi_instance client_plugins agent memgrowth --fail-fast
```

```text
ffi_c_api             117 passed / 0 failed
plugin_runtime        128 passed / 0 failed
subagent_bus           21 passed / 0 failed
subagent_tool         122 passed / 0 failed
agent_host             95 passed / 0 failed
plugins               328 passed / 0 failed
plugin_resources       83 passed / 0 failed
plugin_multi_instance  29 passed / 0 failed
client_plugins        309 passed / 0 failed
agent                  91 passed / 0 failed
memgrowth              15 passed / 0 failed
合计                 1338 passed / 0 failed
```

无 ASan/LSan 报告；`git diff --check` 通过。日志：

```text
/tmp/agentxx-reset-v1-sweep-2.log        # 上述 1338/0 总回归（交接前最后一次复跑）
/tmp/agentxx-reset-v1-sweep-1.log        # 同命令的一次更早运行，结果相同
/tmp/agentxx-reset-v1-dtor-asan-1.log    # 插件专项 ASan（877/0，BaseAgent 修复前）
/tmp/agentxx-reset-v1-dtor-client-1.log  # client 守卫回归
/tmp/agentxx-reset-v1-agenthost-2.log    # agent_host 95/0（BaseAgent executor 修复后）
```

未验证（不得当作已通过）：

- 全模块 `agentxx_test` 全量运行；UBSan/TSan；Windows/Android 构建与专项；
- C17 ABI layout、短表/NULL 表、导出符号检查；
- Client semantic renderer / 动作代次 / prompt contribution / 依赖恢复；
- `docs/zh-cn/design/plugins.md` 未更新。

---

## 8. 已知风险与遗留缺陷（开工前必读）

1. host opaque / vtable 裸指针（最高优先级）：见 P0-1。当前卸载后迟到调用仍可能 UAF。
2. 同步析构兜底会保留 DSO：stop 未完成或 lease 非零时实例保持 `CloseFailed` 并保留 ctx/DSO（日志明确报错）。这是安全兜底而非最终形态，依赖 owner 先 await `shutdownAsync`。`AgentHost::destroyAgent`（同步）只告警不阻断，新代码应使用 `destroyAgentAsync`。
3. renderer 仍在调用线程同步执行：有 lease/alive 复查，但不符合 plugin.md 第 8.2 节的语义 cache 模型。
4. Operation 取消/终态未收敛：`recursive_mutex` 协议、投递失败终态、裸 handle 失效语义。
5. 注册/启停事务不完整：enable/disable 旧模型、prompt contribution、注册失败回滚覆盖面。
6. 无 ABI 编译期检查：任何 ABI 改动目前只靠运行期测试兜底。

---

## 9. 下一步执行顺序（建议）

1. 先按 0.3 节复跑构建 + 专项回归，确认基线（应仍是 1338/0）。
2. P0-1 宿主控制块（agent 侧先，client 侧随后）：收益最大，且是 F02/F15 与“迟到调用安全失败”的共同前置。
3. P0-2 Operation 终态/取消线性化：补 plugin.md 第 11.2 节的故障注入测试。
4. P1-1 SDK Request + 统一 root adapter（F13/F19），随后 P1-2 C17 ABI 测试。
5. P1-3 / P1-4 Client 语义模型与注册、启停事务。
6. P2-1 / P2-2 内置插件与 JS 迁移、平台与文档收尾。

每一步完成后：跑对应模块回归，更新本文件第 1、5、6、7 节，再提交。

---

## 10. 提交要求

- 提交信息使用 `重构插件框架-<修改内容总结>`。
- 阶段提交说明必须写明“Reset-v1 未完成”，不要把阶段成果写成整体完成。
- 提交前：`git diff --check`、构建、相关模块回归；确认 `TODOS.md`、`agentxx-config.yaml`、`index.md` 等用户文件不被误纳入。
- 建议的下一步提交边界（对应 plugin.md 第 10 节）：

```text
1. 重构插件框架-R1-2 Operation 终态与取消收敛
2. 重构插件框架-R2-1 宿主控制块与安全失败
3. 重构插件框架-R2-2 注册事务与异步关闭补全
4. 重构插件框架-R3-1 SDK Request 与统一 root adapter
5. 重构插件框架-R4-1 内置插件迁移
```
