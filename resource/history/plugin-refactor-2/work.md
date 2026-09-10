# 插件框架 Reset-v1 重构交接摘要（进度 / 提交边界 / 验证事实）

> 事实来源：设计定稿是 `resource/history/plugin-refactor-2/plugin.md`（Reset-v1 方案、R0-R6 阶段、F/P 问题编号、测试矩阵）。本文件只记录进度、提交边界、验证结果和待办；与 plugin.md 冲突时以 plugin.md 为准。
>
> 本文件更新时间：2026-09-11（P0-2 提交，即当前 HEAD）。**状态：Reset-v1 未完成。**
> 当前重构进度 = 五个提交：`3a4497ba`（R1-1 Runtime / Operation）、`f861bcf9`（fix-build）、
> `b2b5114a`（Operation/Runtime 可靠性、加载事务与关闭、owner 顺序、ABI v1 / SDK 推进）、
> `c2869f07`（P0-1 宿主控制块 / 迟到调用安全失败 / 注册执行期复查，见第 3.4 节）、
> P0-2 提交（Operation 终态与取消线性化，见第 3.5 节）。
> 工作树在该提交后是**干净的**；本文档自身也已包含在最新提交中。
> 已完成/待完成对照见第 5、6 节，内容明细见第 3、4 节。

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
2. 本文件第 1 节（总览）、第 3.4/3.5 节（P0-1 / P0-2 明细）、第 4 节（`b2b5114a` 明细）、
   第 5 节（已完成）、第 6 节（待完成）、第 9 节（下一步）
3. `git show 3a4497ba`、`git show f861bcf9`、`git show b2b5114a`、`git show c2869f07`、`git show HEAD`

预期看到的状态：`main` 比 `origin/main` 领先 3（`b2b5114a`、P0-1、P0-2 均尚未推送），
工作树无修改、无 untracked 文件。

### 0.2 工作树保护规则（不得违反）

- 禁止 `git reset --hard`、`git checkout --`、清空 build 目录或批量删除测试。
- 当前基线提交是 P0-1 提交（见第 2 节）；不要重写、回退或压缩已存在的重构提交。
- `TODOS.md` 已在 `b2b5114a` 中入库（含 3 行用户改动），`agentxx-config.yaml`、
  `resource/history/plugin-refactor-2/index.md` 随 `3a4497ba` 入库：这些文件已成为历史的一部分，
  既不要回退，也不要在后续重构提交中顺手修改。
- 若新会话期间又出现用户新改动，保留它们并只提交本任务相关文件。

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

当前结果与日志见第 7 节（对应 P0-1 提交）；二进制 `agent/build/linux-debug/exec/agentxx_test`
比源码新，提交后复跑构建为 up-to-date。

---

## 1. 状态总览（对照 plugin.md 第 10 节的 R0-R6）

| 阶段 | 状态 | 事实依据 |
|---|---|---|
| R0 契约冻结 | 完成 | `plugin.md` 定稿；本文件只做进度记录 |
| R1 Runtime / Operation | 基本完成（P0-2 已落地） | `plugin_runtime.h`/`op_driver.h` 已重写并提交（`3a4497ba`）；`b2b5114a` 补齐完成端点、executor 停止重放、`ioCallSync` 快速失败、idle/lease 守卫；`c2869f07` 把 vtable 投递闭包纳入 admission lease；P0-2 提交把 cancel/done 的线性化协议写成显式约束（普通 mutex）、定义完成投递失败的可观察终态并补竞速/重放回归；仍缺 plugin.md 第 11.2 节中 5/8/9 条（caller 卸载保护、shutdown 中后台 Task、超时后立即 unload）的独立用例 |
| R2 加载事务 / 异步关闭 | 部分完成（P0-1 已落地） | 名称预占、`Loading/Ready/Closing/CloseFailed`、`create/start/stop/destroy`、`shutdownAsync`、owner 顺序、`GraphTypeSlot` 已在 `b2b5114a` 落地；P0-1 提交补齐宿主控制块、迟到调用安全失败、注册执行期复查；仍缺 Client semantic renderer cache、prompt contribution、enable/disable 事务化、注册事务全覆盖与回滚测试 |
| R3 ABI v1 / SDK | 大幅推进，未完成 | 接口表 `struct_size` + SDK 严格校验、opaque CancelToken、scheduler v1（删 `pump_io`/`cancel_sleep`/`volatile`）、tasks handle 语义、SDK scheduler/offload 迁移已在 `b2b5114a` 落地；P0-1 提交统一了 `host.opaque` 令牌语义（令牌=控制块地址，永不复用）；仍缺 SDK `Request` 输入所有权、统一 root adapter、hook `Task<void>` 区分、C17/ABI layout 与正反例编译测试、导出符号检查 |
| R4 内置插件 / JS / 平台 | 少量迁移 | 4 个内置插件已迁到 CancelToken 新签名；`f861bcf9` 修了平台插件构建；仍缺其余插件迁移、JS 事务/Promise、Windows 平台 gate |
| R5 Client / 依赖 / prompt | 部分完成 | 事件逐 callback 复查 alive、renderer lease、`blockedByDependencies`、动作派发校验已落地；仍缺语义 renderer cache 与代次、动作代次、prompt 多 owner 合成、enable/disable 事务与依赖恢复测试 |
| R6 验证 / 文档 / 发布审查 | 未开始 | 未跑全模块/UBSan/TSan/Windows；未做导出符号与 C17 ABI 检查；`docs/zh-cn/design/plugins.md` 未更新 |

结论：不能把当前状态写成“Reset-v1 完成”。下一阶段建议见第 9 节。

本节与后面第 4-8 节的判定，对应的代码状态是最新提交（P0-1，见第 3.4 节）；
`b2b5114a` 之前的判定仍按第 3.1-3.3 节成立。

---

## 2. 提交边界与工作树状态

```text
基线（重构前）   a805f9cb  --
提交 1          3a4497ba  重构插件框架-R1-1 Runtime / Operation 部分实现   (2026-09-09 14:00 +0800)
提交 2          f861bcf9  重构插件框架-fix-build                          (2026-09-09 18:32 +0800)
提交 3          b2b5114a  重构插件框架-推进 Operation / Runtime 可靠性、加载事务 / 关闭 / owner 顺序、ABI v1 / SDK
                          (2026-09-11 02:15 +0800, 31 文件, +2779/-835)
提交 4（P0-1）  重构插件框架-P0-1 宿主控制块与迟到调用安全失败
                          `c2869f07` (2026-09-11, 16 文件；见第 3.4 节)
提交 5（P0-2）  重构插件框架-P0-2 Operation 终态与取消线性化
                          (2026-09-11, 5 文件；见第 3.5 节)
工作树          干净（无修改、无 untracked）；origin/main 停在 f861bcf9，提交 3/4/5 均未推送
```

`b2b5114a` 就是此前工作树里的全部增量，代码与验证记录一一对应（未做任何额外改动）；
它同时包含 `TODOS.md`（+3）与本文档的重写（work.md 计 535 行变更）。详见第 3.3 节和第 4 节。

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

注意：R1-1 之后还有 `f861bcf9`（fix-build）与 `b2b5114a`（主要增量）；旧文档中“未创建提交”的说法已过期。

### 3.2 `f861bcf9` 重构插件框架-fix-build

8 文件，+100/-23，全部是构建/平台修复，不含框架语义变化：

- `plugin_kit.h`：`util::insertOrAssignHeterogeneous` 替代异构 map 直接下标赋值，并补 include。
- `util/json.h`：补 `<ostream>`。
- `agentxx_rag_search/CMakeLists.txt`：补 `html2md` 依赖（含 builtin 传递）。
- `agentxx_audio_stream.cpp`：`AudioDataSource source_` 改为实例成员，`start` 失败不再记录为运行中。
- `agentxx_computer_use/ui_control.cpp`、`agentxx_execute_command.cpp`（setup 抽函数）、`agentxx_screen_capture.cpp`：平台入口/编译修复。
- `agent/test/plugin/test_text_selection_monitor.cpp`：+33 行测试。

### 3.3 `b2b5114a` 重构插件框架-推进 Operation / Runtime 可靠性、加载事务 / 关闭 / owner 顺序、ABI v1 / SDK

31 文件，+2779/-835。这是当前进度的主要载体，跨越 R1/R2/R3 并触及 R4/R5：

- R1 收尾：Runtime action 队列与 executor 停止重放、完成端点 tombstone（迟到 `done` 安全丢弃）、
  `ioCallSync` 快速失败、`recursive_mutex` 之外的取消重放、exactly-once 提交与异常隔离。
- R2：`start/stop` 生命周期入口（两端 ABI + 内置插件 + SDK 导出宏）、名称预占、
  `unloadAsyncUntil` 共享截止时间、agent/client `shutdownAsync`、`CloseFailed` 可重试、
  同步析构不再绕过 stop/lease、owner 顺序（`BaseAgent`/`AgentHost`/`AgentContext`/
  `FfiAgentRuntime`/`mode_runners`）、`GraphTypeSlot` 与节点代次校验。
- R3：接口表 `struct_size` + SDK 严格校验、opaque CancelToken、scheduler v1
  （删除 `pump_io`/`cancel_sleep`/`volatile`，`sleep`/`offload` 返回 Operation handle）、
  tasks handle 语义、SDK scheduler/offload（含 `offload<void>`）迁移、`Task` continuation。
- R4/R5：4 个内置插件迁移 CancelToken；Client 事件逐 callback 复查、renderer lease、
  动作派发校验、`blockedByDependencies`。
- 文档与用户文件：本文档重写、`TODOS.md`（+3 行，用户改动）。

按阶段归类的文件级明细见第 4 节；完成/未完成对照见第 5、6 节；验证结果见第 7 节。

### 3.4 P0-1 提交：宿主控制块与迟到调用安全失败（R2/R3 交叉）

15 文件（见第 2 节；代码文件 12 个 + 测试 3 个 + 本文档）。核心是让"插件保存的
旧 `const AgentxxPluginHost*`"在实例卸载后安全失败，并让 vtable 投递的请求不会
跨过 `dlclose`：

- 新增宿主控制块（`plugin_manager_base.h`）：
  - `PluginHostControl` 持有真正交给插件的 `AgentxxPluginHost` 视图（**进程级稳定
    地址，永不释放**）；`host.opaque` = 控制块地址，作为一次性令牌（地址永不复用）；
  - 进程级注册表 `detail::pluginHostControlRegistry()` 保存控制块强引用（tombstone），
    `resolvePluginHostControl(host)` 按令牌解析；未注册令牌（垃圾值）返回空，
    已关闭实例返回控制块但 `instance()` 为空 -> 全部入口安全失败。
  - 控制块只保存 `weak_ptr<PluginInstanceBase>`，不阻止实例释放；实例析构后引用
    自然失效。
- 实例不再把 host 视图放在自己对象内部：`PluginInstance::host` /
  `ClientPluginInstance::host` 成员删除，改为 `hostView()`（交给插件的地址）+
  `retireHostControl()`（destroy 成功后退休）；`destroyPlugin()` 的所有成功出口
  都调用退休，destroy 失败/延后路径保持原状。
- vtable 入口统一前置 `enterPluginHost<InstanceT, ManagerT>()`（agent 侧
  `enterHost()`、client 侧 `enterClientHost()`）：解析控制块，取实例/管理器
  `shared_ptr` 与 admission lease；任一环节失败即按"实例不存在"返回失败值
  （非 0 / NULL + error），不再有 `instOf(host)` 裸指针强转。
  - `allowClosing=false`（默认）：注册、投递、Operation 创建等"开始新动作"的入口，
    Closing/Disabled 后直接拒绝；
  - `allowClosing=true`：只读查询入口（`get_plugin_args`/`get_language`/`list_plugins`/
    `json_*`/`is_io_thread` 等），关闭过程中仍可执行，由 lease 保证卸载等待其返回。
- 投递保活：新增 `ioCallSyncKeep` / `ioCallSyncVoidKeep`（`plugin_common.h`）。闭包
  按值捕获 vtable 上下文（实例/管理器强引用 + admission lease），卸载的
  `waitInflightZero` 因此覆盖"已排队但尚未在 IO 线程执行"的阶段；agent 侧 82 处、
  client 侧 57 处调用点全部改走该路径（`xx_unsubscribe`/`xx_cunsubscribe` 保持
  句柄语义，改为闭包持有管理器强引用，修掉原先 `.lock().get()` 悬垂）。
- 注册执行期复查：`PluginManagerBase::acceptsRegistration()` 统一检查
  `enabled` + `lifetime->acceptsRegistration()`；agent 侧 `registerTool`/`registerHook`/
  `registerCapability`/`registerCapabilityEx`/`registerGraphNodeType`/`registerTask`/
  `subscribe`/`registerSkillDir`/`registerMemoryFile`/`registerMcpServer`/`setPromptJson`，
  client 侧 `registerStatusItem`/`registerPanel`/`registerInfoSection`/`registerCommand`/
  `registerToolDecor`（update 路径）/`registerToolRenderer`/`bindActionHandler`/`subscribe`/
  `openOverlay` 全部在执行点复查，排队期间发生关闭时拒绝且不产生注册残留。
- 测试：`test_plugin_runtime.cpp` 新增 3 组用例（旧 host 指针安全失败且不转交同名新
  实例、注册执行期复查、工作线程注册 + 关闭的真实 vtable 路径）；`RuntimeFixture`
  改为装配真实宿主 vtable 的控制块；其余插件测试改用 `hostView()`。

未做（仍属 P0-1 之外或未覆盖）：prompt contribution、renderer 语义 cache、
Operation cancel/done 线性化（P0-2）、SDK `Request` 所有权（P1-1）。

### 3.5 P0-2 提交：Operation 终态与取消线性化（R1 收尾）

5 文件（`op_driver.h`、`plugin_runtime.h`、`plugin_manager_lifecycle.cpp`、
`test_plugin_runtime.cpp`、本文档）。要点：

- 取消/完成线性化协议显式化（`op_driver.h`）：`submitMutex_` 由
  `std::recursive_mutex` 改为普通 `std::mutex`，并把三条约束写成 `cancelOnIo`
  上方的契约注释：
    1. `completionSubmitted_` 是"done 已被接受"的唯一切换点，任意线程只在持锁时
       读取/置位，因此重复 done 与 done/cancel/reject 竞争只有一个赢家；
    2. 持锁期间绝不调用插件或调用方代码（插件可能在 cancel 里同步 `notify.done`，
       锁内进入插件就是自锁；这也是普通 mutex 足够的原因）；
    3. 终态（Completed/Rejected）与调用方回调只在 IO 线程的 `commit()`/`reject()`
       中生效，`cancelOnIo()` 只把状态推进到 Cancelling，不产生终态。
- 完成投递失败的可观察终态：新增 `OpCore::completionPending()`（完成包已产生、
  尚未在 IO 线程提交）与 `PluginRuntime::pendingOperationSummary()`（`label#id` 列表，
  定义放在 `plugin_manager_lifecycle.cpp`，因为 `OpCore` 在 `plugin_runtime.h`
  里只有前置声明）。`unloadAsyncUntil` 在等待 lease 归零超时（CloseFailed）时
  输出该摘要，明确"哪几个 Operation 阻塞了关闭"：完成包在 executor 停止期间保留
  在待重放队列，Operation 保持未终结且继续持有 caller/provider lease，实例因此
  进入 `CloseFailed` 并保留 ctx/DSO（可重试），而不是静默泄漏；executor 重新绑定后
  完成包重放并只提交一次。
- 回归：
    - 扩展"executor 停止期间完成包必须保留"用例：断言 `completionPending()`、两侧
      lease 仍持有（provider 1 + caller 1）、`pendingOperationSummary()` 含该操作
      标签；重放后断言完成一次、摘要为空、两侧 lease 归零。
    - 新增 32 轮 cancel/done 并发竞速用例（`std::barrier` 对齐两个线程）：只产生
      一个终态、回调恰好一次且状态为 OK、竞速中的插件 cancel 最多被调用一次、
      终态之后再 cancel 不再进入插件、`runtime()->operations` 与 lease 全部回收。

仍未覆盖（下一阶段）：plugin.md 第 11.2 节第 5/8/9 条（caller 卸载保护、
shutdown 中挂起后台 Task、超时后立即 unload 的独立用例）。

---

## 4. `b2b5114a` 内容明细（已提交，按阶段归类）

以下全部内容已在 `b2b5114a` 中；工作树在该提交后没有额外代码改动。
（P0-1 提交的明细见第 3.4 节。）

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

本轮发现的真实缺陷并已修复（见 `b2b5114a`）：`BaseAgent::shutdownAsync` 原先投递到 `ioCtx`，而宿主持有调用方 executor 直跑的子代理（engine 直跑）其自身 `ioCtx` 从未 `run()`，导致 `agent_host` 模块永久挂起；改为投递 `pluginManager->ioExecutor()` 后 `agent_host` 95/0 通过。

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

- `test_plugin_runtime.cpp`：109 → 128（`b2b5114a` 最新）
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
| F15 管理器销毁后队列 lambda 不访问裸 this | 完成 | 业务投递与 vtable 投递都持有 `shared_ptr<Instance/Manager>` + admission lease（P0-1 落地），不再捕获裸 `inst`/`mgr` |
| F18 Client 同轮派发复查 alive | 完成 | `client_plugin_manager.cpp:1511` dispatchEvent |
| P0 完成协议（拥有完成包、IO 线程一次性提交） | 完成 | `op_driver.h` commit 路径 + 完成端点 tombstone |
| P0 opaque CancelToken | 完成 | `plugin_api.h` + SDK 调用方 |
| P1-A 接口表严格协商 | agent/client 接口表完成 | `struct_size` 填充 + SDK 校验 + `api_version` 精确匹配 |
| P1-B 名称预占 / Loading 不可调用 | 完成 | `reservePluginName` + state 门禁；P0-1 补齐注册类入口的执行期复查 |
| P1-C GraphTypeSlot | 首版完成，缺测试 | `plugin_graph_node.h` + `plugin_manager_adapters.cpp`；缺旧节点/重载专项测试 |
| P0-1 宿主控制块 / 迟到调用安全失败 / 注册执行期复查 | 完成 | `PluginHostControl`（`plugin_manager_base.h`）+ `hostView()`/`retireHostControl()` + `enterPluginHost()` + `ioCallSyncKeep`；回归见 `test_plugin_runtime` 新增 3 组用例（第 3.4、7 节） |
| P0-2 cancel/done 线性化协议 | 完成 | `op_driver.h` 普通 mutex + 契约注释；32 轮并发竞速用例（第 3.5 节） |
| P0-2 完成投递失败的可观察终态 | 完成 | `OpCore::completionPending()` + `PluginRuntime::pendingOperationSummary()`；CloseFailed 日志输出阻塞操作（第 3.5 节） |
| P0-2 裸 handle 失效语义 | 完成 | `AgentxxPluginOperatorHandle` tombstone（`completed` 置位后 cancel 空操作）+ 终态后重复 cancel 用例 |
| R2 owner 顺序（BaseAgent / AgentHost / Client runner） | 完成 | 见 4.2 节 |
| R2 Client semantic renderer cache | 未完成 | renderer 仍在 UI 线程同步调用（有 lease 保护） |
| R2/R5 prompt contribution | 未完成 | 仍是“备份后无条件写回”模型 |
| F02/F03/F04/F16/F17/F19/F20/F21、P0-C | 未完成或仅部分 | 见第 6、8 节 |
| R6 验证 / 文档 | 未开始 | 见第 6、7 节 |

---

## 6. 待完成任务（按优先级，含验收要求）

### P0-1 宿主控制块：让迟到插件调用安全失败（R2/R3 交叉）—— 已完成

已于第 3.4 节的提交落地（详见该节）。遗留的收尾项（下一阶段可做）：

- `PluginHostControl` 目前按实例累计、永不回收（tombstone 语义所需）。若将来出现
  "进程内反复加载/卸载上万次"的真实场景，需要在**保证地址不复用**的前提下评估
  池化或压缩（不要为了省内存改为复用地址）。
- `host.opaque` 现在是控制块地址；SDK 侧（`plugin_kit.h`）仍把它当作不透明值透传，
  没有额外断言。若后续给 ABI 加"令牌合法性"自检，需同步更新该处文档。

### P0-2 Operation 终态与取消线性化（R1 收尾）—— 已完成

已于第 3.5 节的提交落地（契约注释、可观察终态、竞速回归）。遗留：

- 验收要求 plugin.md 第 11.2 节 1-10 全部自动化。当前覆盖 1/2/3/4/6/7/10 的大部分；
  仍缺 5（caller 卸载与 provider 未完成互调）、8（shutdown 中挂起后台 Task 的
  cancel→resume→done→destroy→dlclose 顺序）、9（timeout 后立即 unload）的独立用例。
- 关闭超时目前只输出未终结 Operation 摘要（`label#id`）。若要更强的取证，可在
  摘要里带上每个 Operation 的 `completionPending()` 标记与等待时长。

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

## 7. 验证记录（最新：P0-2 提交；P0-1 见 7.1，`b2b5114a` 见 7.3）

构建：

```bash
cmake --build agent/build/linux-debug --target agentxx_test_repo -j12
# 成功（GCC 16.1.0，Linux x86_64 / WSL）；二进制 agent/build/linux-debug/exec/agentxx_test
# 比最新源码更新，提交后复跑为 up-to-date
```

### 7.1 P0-1 提交的回归（ASan + LSan 开启，`--fail-fast`）

```bash
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 timeout 1500s \
  agent/build/linux-debug/exec/agentxx_test \
  ffi_c_api agent_host subagent_tool subagent_bus plugin_runtime plugins \
  plugin_resources plugin_multi_instance client_plugins agent memgrowth --fail-fast
```

```text
ffi_c_api             117 passed / 0 failed
plugin_runtime        151 passed / 0 failed     # 128 -> 151（P0-1 新增 23 项断言用例）
subagent_bus           21 passed / 0 failed
subagent_tool         122 passed / 0 failed
agent_host             95 passed / 0 failed
plugins               328 passed / 0 failed
plugin_resources       83 passed / 0 failed
plugin_multi_instance  29 passed / 0 failed
client_plugins        309 passed / 0 failed
agent                  91 passed / 0 failed
memgrowth              15 passed / 0 failed
合计                 1361 passed / 0 failed
```

插件专项（同一提交，另一次运行）：

```text
plugin_runtime plugins plugin_resources plugin_multi_instance client_plugins
合计 900 passed / 0 failed（改动后第三次复跑，含 sed 清理行内空格后的重编译）
```

无 ASan/LSan 报告；`git diff --check` 通过。日志：

```text
/tmp/agentxx-p01-sweep-1.log   # 1361/0（P0-1 第一次全量扩展回归）
/tmp/agentxx-p01-sweep-2.log   # 1361/0（脚本重建源码后复跑，结果一致）
/tmp/agentxx-p01-sweep-3.log   # 900/0（插件专项）
/tmp/agentxx-build-baseline.log  # 基线构建（up-to-date）
```

### 7.2 P0-2 提交的回归

```bash
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 timeout 1500s \
  agent/build/linux-debug/exec/agentxx_test \
  ffi_c_api agent_host subagent_tool subagent_bus plugin_runtime plugins \
  plugin_resources plugin_multi_instance client_plugins agent memgrowth --fail-fast
```

```text
ffi_c_api             117 passed / 0 failed
plugin_runtime        415 passed / 0 failed     # 151 -> 415（P0-2 竞速/重放断言）
subagent_bus           21 passed / 0 failed
subagent_tool         122 passed / 0 failed
agent_host             95 passed / 0 failed
plugins               328 passed / 0 failed
plugin_resources       83 passed / 0 failed
plugin_multi_instance  29 passed / 0 failed
client_plugins        309 passed / 0 failed
agent                  91 passed / 0 failed
memgrowth              15 passed / 0 failed
合计                 1625 passed / 0 failed
```

插件专项（同提交）：`plugin_runtime plugins plugin_resources plugin_multi_instance
client_plugins` 合计 1164 passed / 0 failed。日志：

```text
/tmp/agentxx-p02-sweep-1.log   # 413/1（发现 provider/caller lease 计数断言写错）
/tmp/agentxx-p02-sweep-2.log   # 1164/0（修正断言后插件专项）
/tmp/agentxx-p02-sweep-3.log   # 1625/0（扩展回归）
```

构建期间遇到一次 GCC 16.1 ICE（`agent/client/src/io/tui/tui_log_sink.cpp`），
按 AGENTS.md 的约定重跑同一构建即通过（未改动该文件）。

### 7.3 `b2b5114a` 的历史验证结果

以下结果测自 `b2b5114a`（同样为 ASan + LSan、`--fail-fast` 的扩展回归）：

```text
ffi_c_api 117 / plugin_runtime 128 / subagent_bus 21 / subagent_tool 122 /
agent_host 95 / plugins 328 / plugin_resources 83 / plugin_multi_instance 29 /
client_plugins 309 / agent 91 / memgrowth 15   合计 1338 passed / 0 failed
```

日志：

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

1. ~~host opaque / vtable 裸指针~~：P0-1 已解决（控制块 + 令牌 + 租约 + 注册执行期复查）。
   仍存在的是"控制块 tombstone 永不回收"的固定内存代价（每实例约 100 字节，见第 3.4 节）。
2. 同步析构兜底会保留 DSO：stop 未完成或 lease 非零时实例保持 `CloseFailed` 并保留 ctx/DSO（日志明确报错）。这是安全兜底而非最终形态，依赖 owner 先 await `shutdownAsync`。`AgentHost::destroyAgent`（同步）只告警不阻断，新代码应使用 `destroyAgentAsync`。
3. renderer 仍在调用线程同步执行：有 lease/alive 复查，但不符合 plugin.md 第 8.2 节的语义 cache 模型。
4. ~~Operation 取消/终态未收敛~~：P0-2 已收敛（普通 mutex + 契约注释、`completionPending()`/
   `pendingOperationSummary()` 可观察终态、终态后 cancel 空操作）。仍缺的是 11.2 节
   第 5/8/9 条独立用例（见第 3.5、6 节）。
5. 注册/启停事务不完整：enable/disable 旧模型、prompt contribution、注册失败回滚覆盖面。
6. 无 ABI 编译期检查：任何 ABI 改动目前只靠运行期测试兜底。

---

## 9. 下一步执行顺序（建议）

1. 以当前 HEAD（P0-2 提交）为起点，按 0.3 节复跑构建 + 扩展回归，确认基线（应仍是 1625/0）。
2. ~~P0-1 宿主控制块~~、~~P0-2 Operation 终态~~：已完成（第 3.4 / 3.5 节）。
   可选收尾：client 侧"旧 host 指针在卸载后安全失败"的专项用例；订阅句柄与
   GraphTypeSlot 旧节点的"代次失效 + 迟到调用"独立用例（机制已具备）。
3. plugin.md 第 11.2 节剩余用例：5（caller 卸载与 provider 未完成互调）、
   8（shutdown 中挂起后台 Task 的顺序）、9（timeout 后立即 unload）。
4. P1-1 SDK Request + 统一 root adapter（F13/F19），随后 P1-2 C17 ABI 与导出符号检查。
5. P1-3 / P1-4 Client 语义模型（renderer cache/动作代次）与注册、启停事务。
6. P2-1 / P2-2 内置插件与 JS 迁移、平台与文档收尾（含 `docs/zh-cn/design/plugins.md`）。

每一步完成后：跑对应模块回归，更新本文件第 1、5、6、7 节，再提交。

---

## 10. 提交要求

- 提交信息使用 `重构插件框架-<修改内容总结>`。
- 阶段提交说明必须写明“Reset-v1 未完成”，不要把阶段成果写成整体完成。
- 新提交一律建立在 `b2b5114a` 之上；不要修改或回退 `TODOS.md`、`agentxx-config.yaml`、`index.md`
  这些已入库的既有改动，除非用户明确要求。
- 提交前：`git diff --check`、构建、相关模块回归；`git status` 确认只包含本任务文件。
- 建议的下一步提交边界（对应 plugin.md 第 10 节）：

```text
1. 重构插件框架-R1-2 Operation 终态与取消收敛
2. 重构插件框架-R2-1 宿主控制块与安全失败
3. 重构插件框架-R2-2 注册事务与异步关闭补全
4. 重构插件框架-R3-1 SDK Request 与统一 root adapter
5. 重构插件框架-R4-1 内置插件迁移
```
