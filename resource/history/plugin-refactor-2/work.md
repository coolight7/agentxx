# 插件框架重构交接摘要

> 本文件是当前工作树的事实交接记录，供下一 session 继续实施。
> 最终设计事实来源仍为 `resource/history/plugin-refactor-2/plugin.md`。
> Reset-v1 尚未完成，本文件中“已实现”只表示代码中已经存在，不能等同于最终方案验收通过。

## 1. 当前结论

- 任务仍处于 **R1 Runtime / Operation 部分实现** 阶段，R2-R6 尚未完成。
- 在本 session 开始前，Linux Debug 构建成功，插件专项回归为 **829 passed / 0 failed**。
- 本 session 后又修改了生命周期、完成端点和析构相关代码；这些修改尚未通过构建和测试。
- 当前最后一次构建失败，失败原因是 `agent/lib/include/agentxx/plugin/op_driver.h` 中 `OpCore::wait()` 被放在 `private` 区域，而 `awaitPluginOp()` 仍从类外调用它。
- 因此，当前工作树不能宣称“已构建通过”或“测试通过”。下一 session 必须先恢复编译，再验证本 session 的新增修改。
- 没有创建本阶段提交。

## 2. 工作树保护规则

下一 session 开始先执行：

```bash
cd /home/coolight/program/agentxx
git status --short --branch
git diff --check
git diff --stat
git diff --cached --stat
```

必须保留、不得覆盖或回退的用户/无关修改：

```text
TODOS.md
agentxx-config.yaml
resource/history/plugin-refactor-2/index.md
```

当前工作树同时存在 staged 和 unstaged 修改。不要使用 `git reset --hard`、`git checkout --` 或清空整个 build 目录。下一 session 需要分别阅读 `git diff --cached` 与 `git diff`，确认哪些是前一 session 的重构内容、哪些是本 session 的未验证增量。

当前主要重构文件包括：

```text
agent/lib/include/agentxx/plugin/op_driver.h
agent/lib/include/agentxx/plugin/plugin_common.h
agent/lib/include/agentxx/plugin/plugin_manager.h
agent/lib/include/agentxx/plugin/plugin_manager_base.h
agent/lib/include/agentxx/plugin/plugin_runtime.h
agent/lib/include/agentxx/plugin/client_plugin_manager.h
agent/lib/src/plugins/plugin_manager_adapters.cpp
agent/lib/src/plugins/plugin_manager_capability.cpp
agent/lib/src/plugins/plugin_manager_lifecycle.cpp
agent/lib/src/plugins/plugin_manager_scheduler.cpp
agent/lib/src/plugins/plugin_manager_tasks.cpp
agent/lib/src/plugins/plugin_manager_vtable.cpp
agent/lib/src/plugins/client_plugin_manager.cpp
agent/test/plugin/test_plugin_runtime.cpp
agent/test/plugin/test_plugin_runtime.h
agent/test/plugin/test_plugins.cpp
agent/test/test.cpp
```

## 3. 可靠的验证记录

### 3.1 已成功验证的中间版本

在本 session 当前这批生命周期/完成端点修改之前，执行过：

```bash
cmake --build agent/build/linux-debug -j2
```

构建成功，相关日志：

```text
/tmp/agentxx-reset-v1-next-build.log
```

随后执行：

```bash
timeout 180s agent/build/linux-debug/exec/agentxx_test \
  plugin_runtime plugins plugin_multi_instance plugin_resources client_plugins --fail-fast
```

实际结果：

```text
plugin_runtime          89 passed / 0 failed
plugins                328 passed / 0 failed
plugin_resources        83 passed / 0 failed
plugin_multi_instance   29 passed / 0 failed
client_plugins         300 passed / 0 failed
合计                   829 passed / 0 failed
```

日志：

```text
/tmp/agentxx-reset-v1-next-regression.log
/tmp/agentxx-reset-v1-next-regression.exit  # 内容为 0
```

该结果不能代表当前工作树，因为之后追加的修改没有重新通过构建。

### 3.2 当前失败构建

最后执行：

```bash
cmake --build agent/build/linux-debug -j2
```

日志和退出码：

```text
/tmp/agentxx-reset-v1-continue-build-5.log
/tmp/agentxx-reset-v1-continue-build-5.exit  # 内容为 2
```

当前直接错误：

```text
agent/lib/include/agentxx/plugin/op_driver.h:283:
  OpCore::wait() is private within this context
agent/lib/include/agentxx/plugin/op_driver.h:423:
  co_await core->wait()
```

修复方式应是将 `wait()` 恢复为 `OpCore` 的 public 方法，或明确设计 friend；优先保持原有接口意图，移除误插入的 `private:` 边界。修复后先只重建，不要同时继续扩展功能。

### 3.3 静态检查

最近一次 `git diff --check` 通过。当前没有在本 session 的未验证修改上重新完成构建后的测试。

已知验证范围只有 Linux Debug 现有 ASan 配置；没有完成 UBSan、TSan、Windows、Android、全模块回归或最终 C ABI 检查。

## 4. 前一阶段已经实现的内容

### 4.1 Runtime / lifetime 初版

新增 `agent/lib/include/agentxx/plugin/plugin_runtime.h`，包含：

- `PluginInstanceState`：`Loading`、`Ready`、`Disabled`、`Closing`、`Closed`、`CloseFailed`。
- `InstanceLifetime`：executor、generation、原子状态、admission 位和 lease count。
- `InstanceLease`：移动式 RAII lease。
- `PluginRuntime`：executor、IO thread id、Operation map、Operation/generation 序号。
- admission 位与 lease count 使用一次 CAS，关闭与跨线程获取之间没有显式空窗。
- `waitIdleUntil()` 使用绝对 deadline 和事件式 idle 唤醒，不使用退避轮询。

`PluginInstanceBase::InflightGuard` 已优先使用 lifetime lease，同时维护兼容性的 `inflight` 计数；没有 lifetime 的测试伪实例保留旧计数路径。

### 4.2 OpCore 统一异步操作模型

`op_driver.h` 已将工具/能力/task/scheduler 的主要宿主操作统一到 `OpCore`：

- provider/caller 双方 guard。
- `Accepted`、`Running`、`Cancelling`、`Completed`、`Rejected` 状态。
- 拥有结果的 `CompletionPacket`，完成 payload 在进入 IO executor 前复制为 `std::string`。
- 完成提交、callback、内部 completion handler、Operation 登记和 lease 清理集中处理。
- 同步 `done + NULL` 可被接受；真正拒绝不进入 callback。
- 取消等待不会立即释放 runtime 对尚未完成 Operation 的持有。
- 外部 callback 抛异常不会跳过内部 completion handler。
- `cancelPluginOperation()` 会先获取 handle 的 shared ownership，再向 executor 投递取消请求。

当前 `op_driver.h` 仍需审查：

- `wait()` 的访问级别当前错误，导致编译失败。
- `submitMutex_` 仍为 `recursive_mutex`，cancel 可能持锁进入插件，worker done 也会获取该锁，潜在死锁协议尚未最终收敛。
- `asio::post()` 投递失败时，当前逻辑不能保证已接受 Operation 进入可观察终态。
- handle 的外部 raw 指针在 Operation 回收后仍可能被旧调用方使用，`shared_from_this()` 本身不能修复一个已失效的 raw handle。
- 当前完成端点增量尚未通过构建/测试，见第 5 节。

### 4.3 IO 投递基础设施

`plugin_manager_base.h` / `plugin_common.h` 已做以下改动：

- 删除基类二级 IO 任务队列及其 mutex。
- IO 投递闭包使用共享 runtime 和拥有的 callable。
- `postToIoAsync()` 恒异步；当前 IO 线程的 `postToIo()` 仍可直接执行，重入语义还未完全禁止。
- `ioCallSync` 使用 shared promise 和拥有的 callable，不再捕获栈上 promise/fn 引用。
- `waitInflightZero()` 改为使用 lifetime idle 事件。

尚未解决 executor 已停止时同步等待 future 可能永久等待的问题。

### 4.4 Scheduler / capability / task 适配

已修改：

- `plugin_manager_scheduler.cpp`：post、sleep、offload 接入 OpCore。
- sleep 使用活动 Operation handle 索引，完成和取消统一清理。
- offload 任务从排队到 worker 返回再到 IO callback 均持有 Operation 生命周期。
- 无 thread pool、worker 异常和投递异常会尝试提交失败完成，不再静默挂起。
- `plugin_manager_capability.cpp`：工具/能力 provider start 经 IO executor 执行，输入字符串拥有化，callback 可为空，provider/caller guard 覆盖排队及执行。
- `plugin_manager_tasks.cpp`：`registerTask` 接入统一 Operation，cancel_fn 和 cancel_ud 由 Operation 保护。
- `plugin_manager_vtable.cpp`：post/sleep/offload 等入口补充排队阶段 admission guard。

这些适配仍依赖旧 scheduler/task ABI；`pump_io`、`volatile cancel_flag`、旧 void 返回和旧 SDK awaiter 尚未移除。

### 4.5 注册、工具和 subscription

已有：

- tool registry 冲突检查失败时不写入实例工具记录。
- subscription 控制块保存 weak instance、alive 标志和订阅记录。
- callback 前检查 alive、实例、enabled、lifetime，再取得 guard。
- unsubscribe 先失效再退订，减少同轮回调和重复退订风险。
- `detachAll()` 只请求活动 Operation 取消，不提前清空活动记录；终态 commit 负责唯一回收。

还没有完成旧 ABI handle 的全生命周期安全失败语义，也没有完成 Client 同轮事件、generation 和 action 代次协议。

### 4.6 测试和图调用修正

新增：

```text
agent/test/plugin/test_plugin_runtime.cpp
agent/test/plugin/test_plugin_runtime.h
```

并注册到 `agent/test/test.cpp`。runtime 测试覆盖同步完成、拒绝、payload ownership、取消/重复完成、caller/provider lease、idle deadline、工具互调、并发 Operation、sleep 清理、无线程池失败和部分关闭竞速。

`test_plugins.cpp` 两处宿主协程内的同步 `engine->run()` 改成 `co_await engine->run_async()`，避免同步嵌套 executor 阻塞插件完成包投递。

此前通过的 `plugin_runtime` 数量是 89，不是更早版本的 1074 或 1814；后续新增/合并测试尚未在当前工作树上重新通过。

## 5. 本 session 新增但未验证的修改

本 session 为处理“同步 shutdown 可能绕过 lease 直接 destroy/dlclose”尝试加入以下代码：

### 5.1 destroy 状态字段

`PluginInstanceBase` 增加：

- `pluginCreated`
- `pluginDestroyed`
- `destroyDeferred`

加载 create 成功且产生 `pluginCtx` 时设置 `pluginCreated`。

### 5.2 Agent/Client destroyPlugin

Agent `PluginInstance` 和 Client `ClientPluginInstance` 增加 `destroyPlugin()`：

- 已 destroy 时直接返回。
- 有活动 lifetime lease 时不调用插件 destroy，记录 deferred。
- 无 create context 时标记为已处理。
- destroy 回调异常被捕获，不继续向上抛出。
- manager 的同步 shutdown 和异步 unload 改为通过该方法销毁。

### 5.3 completion endpoint 初版

尝试在 `plugin_manager.h` 增加宿主拥有的 `AgentxxPluginOperationCompletionEndpoint`：

- endpoint 由 Operation 创建，内部 weak 指向 `OpCore`。
- 实例保存 endpoint tombstone 到实例销毁。
- `notify.host_ud` 改为 endpoint 地址，endpoint 找不到 Operation 时只丢弃迟到 done。
- OpCore 仍保留一套旧 `onDone(OpCore*)` 静态函数，需下一 session 审查是否应删除，避免继续保留裸指针入口。

### 5.4 当前未验证增量中的明确缺陷/风险

这是下一 session 的优先修复清单，不能把本节当成已完成：

1. **当前首先编译失败**：`OpCore::wait()` 位于 private 区域。
2. **析构仍可能关闭活动 DSO**：`PluginInstance::~PluginInstance()` 和 `ClientPluginInstance::~ClientPluginInstance()` 调用 `destroyPlugin()` 后，无论返回值都继续 `NativeLoader::close(dlHandle)`。如果 lease 仍活动，`destroyPlugin()` 返回 false，但析构仍会 `dlclose`，P0 UAF 风险未解决。
3. **同步 manager 析构仍无 owner 等待协议**：`shutdownAll()` 发现活动 lease 时保留实例表，但 manager 自身随后析构，实例最终析构仍会触发上述问题；`unloadRequested` 也可能阻止后续重试。
4. **没有真正的 idle cleanup 接线**：`InstanceLifetime` 增加了一次性 `idleCleanup` 字段和 idle 回调执行逻辑，但 manager/实例尚未安全注册和使用它；不能据此认为同步关闭会在最后 lease 释放后自动完成。
5. **`operatorHandles` 字段当前未形成完整所有权协议**：它是本 session 的尝试性增量，尚未接入所有 Operation 路径，需要删除或完整设计后再保留。
6. **completion endpoint 的实例 tombstone 清理、跨 owner 的并发访问和 DSO 关闭顺序尚未测试**。endpoint 指针必须在所有插件可能调用 `notify.done` 的时间内有效；仅保存 `shared_ptr` 不足以保证插件代码本身未被 dlclose。
7. **create 返回 0 但 pluginCtx 为空的语义尚未确认**：当前以 `pluginCtx != nullptr` 判断 `pluginCreated`，需要按最终 Reset-v1 ABI 明确无上下文插件是否允许。
8. **destroy 回调缺失、destroy 抛异常以及 builtin unload 的行为仍需要统一契约**。

建议下一 session 先修正并重建，再决定保留还是撤销这些未验证增量；不要在当前编译失败状态继续迁移业务插件。

## 6. 核心未完成问题

### 6.1 P0 生命周期

当前仍存在：

```text
PluginManager::~PluginManager()
  -> shutdownAll()
  -> shutdownPlugin()
  -> 可能 destroy
AgentContext::~AgentContext()
  -> pluginManager->shutdownAll()
ClientPluginManager::~ClientPluginManager()
  -> shutdownAll()
PluginInstance::~PluginInstance()
  -> dlclose
```

需要实现真正的 `shutdownAsync()` / Client 对应接口，并调整 owner 顺序：

- 先停止接收新工作和新 admission。
- 取消/等待全部已接受 Operation、callback、worker、timer、JS/平台线程。
- 执行 stop，等待 stop 完成。
- 再调用 destroy，最后关闭动态库。
- owner 的 executor、IO context、blocking pool 在插件关闭完成前不能停止。
- 析构路径只能处理已经 Closed 的实例；无法安全关闭时保留 runtime/module 并记录 CloseFailed，不能强制 destroy/dlclose。

重点 owner：`AgentContext`、`BaseAgent`、`AgentHost::destroyAgent`、`mode_runners.cpp`、Client manager 和 Client runner。

### 6.2 R1 Operation / cancel

仍待完成：

- completion 投递失败时的可观察终态和关闭策略。
- raw handle 失效后的取消安全语义。
- cancel 与 done 的无死锁线性化协议。
- callback 返回前保持 caller/provider lease。
- done 不等于插件 worker/自建线程已经返回的场景。
- 排队取消、blocking pool 限额和 executor 停止。
- 错误状态的结构化传递。
- 故障注入、重复 done、取消后 done、关闭中排队等竞态测试。

### 6.3 R2 事务加载/异步关闭

尚未实现：

- Loading 名称预占和并发重复加载拒绝。
- `get_info/create/start/stop/destroy` 新生命周期。
- create/start 注册事务和失败回滚。
- tool/hook/capability/event/resource/prompt/graph/UI 的统一 transaction。
- Agent/Client `shutdownAsync`。
- 依赖关闭失败传播和可重试 CloseFailed。

### 6.4 R3 ABI/SDK

仍是旧实现，未完成：

- C ABI version/struct_size/短表/NULL 必需函数检查。
- opaque CancelToken，删除 volatile cancel、pump_io、cancel_sleep。
- 统一 scheduler/tasks Operation ABI。
- C17 layout/offsetof/调用约定测试。
- 类型安全 `Task<T>`、`Task<void>`、offload<void>、root adapter。
- 删除任意 PromiseBase/协程帧强转、unmanaged spawn、借用输入跨挂起。

### 6.5 R4-R6

尚未完成：

- 内置插件和 JS 迁移。
- GraphTypeSlot 代次间接层。
- Client semantic renderer/cache、action generation、事件同轮退订。
- 依赖恢复、prompt contribution。
- Windows/Android 平台迁移。
- 全量构建、ASan/UBSan/TSan、Windows、纯 C ABI、导出符号和文档审查。

## 7. 下一 session 推荐执行顺序

### 第一步：恢复编译

修复 `OpCore::wait()` public/private 误放置：

```bash
# 修复后
cmake --build agent/build/linux-debug -j2 \
  > /tmp/agentxx-reset-v1-next-build.log 2>&1
status=$?
printf '%s\n' "$status" > /tmp/agentxx-reset-v1-next-build.exit
```

若再次出现零字节 `.o` / `mold unknown file type`，只检查并删除对应零字节构建产物后重建，不要清空 build。

### 第二步：审查本 session 未验证增量

优先处理第 5.4 节：

- `destroyPlugin()` 返回 false 时禁止析构路径 dlclose。
- 解决 manager destructor 与 deferred instance 的所有权，不能只把实例留在即将析构的 map 中。
- 要么接通 idle cleanup 和 owner 保活，要么暂时撤掉不完整字段，按 R2 重新实现。
- completion endpoint 不能保留裸 OpCore completion 入口；完成端点、Operation、插件 DSO 的三者生命周期需要统一。
- 删除或完整接入未使用的 `operatorHandles`。

### 第三步：重新验证当前增量

```bash
timeout 90s agent/build/linux-debug/exec/agentxx_test \
  plugin_runtime --fail-fast

timeout 180s agent/build/linux-debug/exec/agentxx_test \
  plugin_runtime plugins plugin_multi_instance plugin_resources client_plugins --fail-fast
```

只有当前工作树重新构建并通过后，才能更新本文件中的验证数字。不要沿用 829/0 作为当前版本的结果。

### 第四步：继续 R1/R2

推荐顺序：

1. 完成 Operation completion/cancel/handle 协议。
2. 建立真正 async shutdown、stop 和 owner 退出顺序。
3. 加入真实 DSO unload 并发测试、CloseFailed retry、executor stop、依赖级联失败测试。
4. 再开始严格 ABI、SDK 和内置插件迁移。

## 8. 最终提交要求

本阶段未提交。后续若达到可提交阶段：

- 先构建、运行相关测试和 `git diff --check`。
- 再审查 staged/unstaged diff，确认 `TODOS.md`、`agentxx-config.yaml`、`index.md` 没有被纳入重构提交。
- 提交消息必须使用：

```text
重构插件框架-{修改内容总结}
```

- 若只是阶段性 R1/R2 提交，提交说明必须明确 Reset-v1 尚未完成，不要把阶段成果标记为整体重构完成。
