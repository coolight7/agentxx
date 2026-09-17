# 插件框架 Reset-v2：通用协程桥接与 PollOneBridge 设计

> **状态：设计方案，尚未开始产品代码重构。**
>
> 核心是一个与协程库无关、实例隔离的 C ABI 驱动协议；C++ kit 在其上提供 Asio `PollOneBridge`。当前内置插件和历史 ABI 可以一起重置，不保留二进制兼容分支；C ABI 规则和 COM 风格 `query_interface` 查询模型仍保留。

## 1. 目标和边界

### 1.1 目标

1. 插件异步函数与宿主 Asio 协程在同一宿主 IO 执行序列中交错推进。启动插件协程后，宿主等待其完成时不占用额外工作线程，也不阻塞 IO 线程。
2. 插件框架对任何协程库提供相同的基础：插件申请一次有界驱动、宿主异步投递该驱动、插件在本地运行时有新工作时显式唤醒宿主。
3. C++ kit 允许 Asio 插件直接写 `asio::awaitable<T>` 风格的协程；kit 负责将其接入 C ABI 协议，插件业务代码不需要手写 `poll_one` 状态机。
4. 宿主等待结果、取消、停用、卸载和迟到回调均为事件驱动状态转换，不通过定时轮询检查完成与否。
5. 不让 C++ ABI 穿过插件边界：不传 Asio executor、`std::coroutine_handle`、STL 容器、异常、原子对象或 C++ 类布局。
6. 一个 DSO 可以被多个宿主实例加载。驱动、待执行回调、任务表、取消状态和唤醒状态都归属单个插件实例。

## 2. 为什么核心选择通用 pump/wake 协议

插件可选自己的协程实现、事件循环和第三方库。

因此核心只定义两类动作：

- **driver / pump**：插件请求宿主异步执行一次有界回调；该回调只推进插件本地运行时一个有限步骤，不能阻塞、不能等待事件、不能同步调用宿主业务。
- **wake**：插件适配器知道本地运行时已有可运行 continuation，或它所拥有的外部完成回调已经到达时，向宿主请求一个 driver。宿主把重复 wake 合并为有限数量的 driver ticket。

宿主无需知道插件使用哪一种 coroutine、future、actor 或 reactor；插件也无需得到宿主 executor。两端只经稳定 C 函数指针、opaque handle 和受宿主管理的操作句柄协作。

```text
宿主 Asio 协程                插件 C ABI / Adapter              插件本地运行时
    | start(tool)                    |                                  |
    |------------------------------->| create root task                 |
    |                                | request_driver()                 |
    |<-------------------------------| accepted operation               |
    | await completion               |                                  |
    |        host post driver        |                                  |
    |-------------------------------->| drive_once() ------------------->| advance once
    |                                |                                  | suspend / finish
    |                                | <---------- external completion -|
    |                                | wake()                           |
    |        host post driver        |                                  |
    |-------------------------------->| drive_once()                     |
    |                                | done(result)                     |
    |<-------------------------------| completion packet                |
```

`request_driver()` 永远异步投递。即使调用者本来就在宿主 IO 线程，也不可内联调用插件 `drive_once`，否则 root start、completion 和 cancel 会形成意外重入，且失去交错执行的公平性。

## 3. `poll_one()` 的适用范围和限制

`asio::io_context::poll_one()` 很适合作为 Asio 特化的单步推进函数：它不阻塞，最多执行一个已就绪 handler，因此一个 host driver 对应一个局部 continuation，宿主和插件任务能自然轮换。

但下面的循环是错误实现：

```c++
void drive_once() {
    local_io.poll_one();
    host.request_driver(drive_once); // 错误：即使没有新工作也持续排队
}
```

它在 local reactor 没有 ready handler 时仍消耗宿主任务队列和 CPU，只是把 polling 隐藏在 `post` 中。

此外，一个私有 `asio::io_context` 没有跨平台公共 API 能把其内部 epoll/kqueue/IOCP 等待对象安全注册给另一个 `io_context`。因此下列推理不成立："本地 `poll_one()` 返回 0，等 OS 事件到达时宿主会自动再调用它"。若没有适配器可观察的 wake source，宿主无从得知何时再驱动本地 reactor。

第一版支持以下真实 wake 来源：

1. 第三方异步库在完成时调用插件提供的回调；回调向 local runtime 投递 continuation 并调用 `wake()`。
2. 宿主已有的通用 scheduler timer 回调。计时器到期时 adapter 将 continuation 投递到本地 runtime 并调用 `wake()`；这不是轮询。
3. 某个库以稳定、可公开使用的方式暴露 POSIX fd 或 Windows waitable handle，且其文档允许外部等待。后续可选 `wait_source` 接口把 ready 通知投递到 adapter，adapter 再 `wake()`。
4. 必须使用且不能导出 wake source 的私有 reactor 时，使用显式声明、受生命周期控制的专用 reactor 线程，作为该插件的例外实现。不能伪装为单线程协程交错执行。

不能满足其中任一条件的原始私有 Asio socket/process/file 操作，在第一版不得接入 `PollOneBridge` 后宣称完全事件驱动；应保留为受控线程实现、等待后续宿主服务，或改变底层库。

## 4. Reset-v2 C ABI

### 4.1 兼容性和基本规则

- 全局 `AGENTXX_PLUGIN_API_VERSION` 与所有接口表版本重置为新的 v1。宿主要求精确版本和最小 `struct_size`，不保留历史布局分支。
- 延续 `#pragma pack(push, 8)`、`int32_t/int64_t/uint64_t`、`AGENTXX_PLUGIN_CALL`、结构体指针传参/出参和 `alloc/free` 跨堆规则。
- `AgentxxPluginHost` 保留最小 `AgentxxHostVtable { alloc, free, query_interface }`。可选能力都通过字符串 IID 的 COM 风格 `query_interface` 取得。
- 所有 C 函数指针边界有 `noexcept` trampoline。异常、插件借用数据、插件 C++ 对象均不可越过边界；宿主收到的 payload 首先复制为宿主拥有的数据。

### 4.2 新的 coroutine runtime 接口

建议 IID 为 `agentxx.agent.coroutine_runtime`，接口表为：

```c
#define AGENTXX_PLUGIN_IFACE_COROUTINE_RUNTIME "agentxx.agent.coroutine_runtime"
#define AGENTXX_PLUGIN_IFACE_COROUTINE_RUNTIME_VERSION 1

typedef struct AgentxxPluginDriver AgentxxPluginDriver;
typedef void(AGENTXX_PLUGIN_CALL* AgentxxPluginDriveOnceFn)(void* user_data);

typedef struct AgentxxPluginCoroutineRuntimeIface {
    int32_t version;
    uint32_t struct_size;

    // 成功时返回宿主管理的 ticket。宿主只会异步调用 drive_once，且每张
    // ticket 至多调用一次。失败时返回 NULL 并写 error_out。
    AgentxxPluginDriver*(AGENTXX_PLUGIN_CALL* request_driver)(
        const AgentxxPluginHost* host,
        AgentxxPluginDriveOnceFn drive_once,
        void* user_data,
        AgentxxPluginString* error_out);

    // 幂等、非阻塞。尚未开始的 ticket 不再执行；正在执行的 driver 不会被
    // 强行中断，取消完成后由实例 lease/idle 协议收束。
    void(AGENTXX_PLUGIN_CALL* cancel_driver)(AgentxxPluginDriver* driver);

    // 仅用于断言、诊断和 kit 的误用检查，不允许插件据此内联执行 driver。
    int32_t(AGENTXX_PLUGIN_CALL* is_io_thread)(const AgentxxPluginHost* host);
} AgentxxPluginCoroutineRuntimeIface;
```

`request_driver` 由任意线程调用，宿主先取得该实例的 lifetime lease，再把拥有 ticket 的闭包 `asio::post` 到所属 executor。回调运行前检查 ticket、实例代次和 Closing 状态；回调返回后释放执行 lease。它不得调用 DSO 以外的业务回调，不得等待锁、future 或条件变量。

该接口不要求插件把每个 wake 都映射成一个 driver。通用 adapter 必须自己合并 wake，确保同一实例同时只有一个已登记或正在执行的 driver；宿主仍保留 ticket 去重和关闭时取消的最后防线。

### 4.3 可选能力

现有 scheduler 可保留 `post_to_io`、`sleep` 和 operation cancellation 等通用能力，但 `offload` 只是明确的工作线程设施，不能成为本设计的隐式依赖。

## 5. 与协程库无关的 adapter 协议

每个插件实例的 adapter 至少维护以下状态，全部由其单一串行执行上下文修改；跨线程入口只投递回该上下文：

```text
Stopping: bool
Generation: uint64
DriverQueued: bool
DriverRunning: bool
WakePending: bool
DriverTicket: opaque pointer | null
ActiveRoots: map<operation-id, root-state>
```

协议如下：

1. 启动 root task 时，adapter 把输入复制到 root state，登记取消钩子，启动或准备本地 coroutine，并调用 `wake()`。
2. `wake()` 仅表示 adapter 已知存在可运行工作；它设置 `WakePending`。若没有 queued/running driver，清除该标志、设置 `DriverQueued`，调用 `request_driver`。若调用失败，所有受影响 root 以失败终结。
3. driver 回调先把 `DriverQueued=false`、`DriverRunning=true`，然后只调用一次适配器的 `advance_once()`。完成后清除 `DriverRunning`。
4. `advance_once()` 可执行一个 runnable continuation、递送一个外部 completion，或完成一个 root。它不等待事件；若没有工作，立即返回。
5. driver 返回前若 `WakePending` 已再次设置，则申请下一次 ticket；否则停止。外部回调在之后到达时会自行调用 `wake()`。
6. cancel 先在线性化状态中标记 root cancelled，再调用库的取消机制。库完成、同步取消回调、正常结果和异常都通过同一个 once-only completion path 交给宿主。
7. Closing 后拒绝新 root 与新 wake；已登记 ticket 取消，已运行 callback 允许返回；等 active root、driver lease 和 completion callback lease 全部归零后才 destroy plugin context 和 dlclose。

唤醒必须覆盖三个竞态窗口：driver 前、driver 执行中、driver 返回后。`WakePending + DriverQueued + DriverRunning` 的状态机使任何窗口中的 wake 最终都会留下一个 ticket；不能仅使用单个 `bool scheduled`，否则容易在 driver 清标志与外部 callback 写标志之间丢失通知。

## 6. C++ Asio kit：`PollOneBridge`

### 6.1 实例结构

`PollOneBridge` 只在 C++ kit 内部存在，不放进 C ABI。每个 `PluginBase` 实例拥有独立 bridge：

```c++
class PollOneBridge {
    asio::io_context local_io_{1};
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    const AgentxxPluginHost* host_;
    const AgentxxPluginCoroutineRuntimeIface* runtime_;
    std::unordered_map<uint64_t, RootState> roots_;
    bool stopping_ = false;
    bool driver_queued_ = false;
    bool driver_running_ = false;
    bool wake_pending_ = false;
    AgentxxPluginDriver* driver_ = nullptr;
    uint64_t generation_ = 1;
};
```

`Task<T>` 可以直接是 `asio::awaitable<T, asio::any_io_executor>`，或是 kit 提供的等价包装；它只在插件 DSO 内编译和销毁。宿主只观察其最终 `done(status, copied payload)`，从不接触 `Task<T>` 本身。

### 6.2 启动 root task

工具的 `execute_start` 通过 kit helper 变为类似：

```c++
tool(ctx, "name", schema,
    [](PluginCtx& ctx, Request request) -> asio::awaitable<std::string> {
        auto reply = co_await some_adapter_awaitable(ctx.bridge(), request);
        co_return reply;
    });
```

kit 为该次调用复制 request，建立 root state，执行：

```c++
asio::co_spawn(
    bridge.local_executor(),
    [root = root_state.get(), task = make_task()]() mutable -> asio::awaitable<void> {
        try {
            root->finish_ok(co_await std::move(task));
        } catch (const CancelledException&) {
            root->finish_cancelled();
        } catch (const std::exception& e) {
            root->finish_failed(e.what());
        } catch (...) {
            root->finish_failed("plugin coroutine failed");
        }
    },
    asio::detached);
bridge.wake();
return root->opaque_operation_handle();
```

这里 `co_spawn` 只把 root 放入 `local_io_` 的 ready queue；实际代码不在 `execute_start` 中长时间运行。首次 host driver 调用 `poll_one()` 后才执行一个 handler。

### 6.3 单次驱动和合并 wake

伪代码如下，实际实现须用单一串行上下文或同等的互斥线性化，且所有 C ABI trampoline 最外层捕获异常：

```c++
void PollOneBridge::wake() noexcept {
    if (stopping_) return;
    wake_pending_ = true;
    if (driver_queued_ || driver_running_) return;

    wake_pending_ = false;
    driver_queued_ = true;
    driver_ = runtime_->request_driver(host_, &driveOnceTrampoline, this, &error);
    if (!driver_) fail_all_roots(error);
}

void PollOneBridge::drive_once() noexcept {
    if (stopping_) return;
    driver_queued_ = false;
    driver_running_ = true;
    local_io_.poll_one();             // 严格只调用一次
    driver_running_ = false;

    // 只消费在本轮之前或本轮中出现的真实 wake；不能因为 poll_one 返回 0
    // 自行再排队。若外部事件此后到达，它的 callback 会调用 wake()。
    if (wake_pending_) wake();
}
```

上例说明意图，生产代码须避免 `wake_pending_` 在 `wake()` 内立刻被错误清除的递归问题：可将 `schedule_driver_if_needed()` 独立出来，用 generation/epoch 记录已消费 wake。关键可验证的不变量是：每个 host ticket 最多一次 `poll_one()`；没有 root 启动、没有 local ready handler、没有外部 completion，ticket 数不会继续增长。

### 6.4 跨宿主异步调用的 awaitable

`call_tool_async`、`sleep` 等宿主回调在宿主 IO 线程到达。kit callback 不直接恢复 local coroutine；它要：

1. 复制 payload/status 到 root 或 awaiter 自己拥有的状态；
2. `asio::post(bridge.local_executor(), continuation)`；
3. 调用 `bridge.wake()`。

因此 continuation 仍由下一次 host driver 中的一次 `poll_one()` 执行，避免从宿主回调栈内重入插件协程。插件使用的第三方 callback 同样应遵守此规则。

取消时，kit 在 local executor 投递取消/关闭 continuation 并 `wake()`；对于可取消的宿主 operation，调用其 C ABI cancel 函数。最终结果先由 root 的 once-only state 仲裁，后到的完成仅做诊断，不能第二次调用 `notify.done`。

### 6.5 支持的 Asio 用法

第一版可安全支持：纯计算后 `co_await` 宿主 callback adapter、宿主 sleep adapter、能在完成时显式回调 adapter 的第三方库，以及通过后续 `wait_source` 接口适配的公开原生等待对象。

不自动支持（**未声明时**）：创建在 `local_io_` 上的普通 Asio socket/timer/process/file 后仅依赖其私有 reactor 等待。`poll_one()` 在没有 host 再次调用时无法让这些完成继续运行，因此这类业务必须**显式声明**自己需要受控轮询（`polled_tool`，见第二轮方案与 work.md），或者改用宿主回调式完成 / 宿主计时器 / 显式受限工作线程。未声明的误用由 kit 运行期诊断提示。

## 7. 宿主操作、取消和卸载

宿主继续以内部 `PluginRuntime`、`InstanceLifetime` 和 `PluginOperation` 管理操作，但重构时应去除当前为私有线程池和轮询服务的路径。

每个 driver ticket、工具 root、跨插件 callback 和 completion packet 都持有实例 lease。lease 覆盖排队、宿主回调进入、插件 driver 执行、完成回调返回和最终清理。实例状态至少为：

```text
Loading -> Ready -> Closing -> Closed
                 \-> Disabled -> Closing
```

关闭过程：

1. 进入 `Closing`，停止接受新工具、注册、driver request 和 wake。
2. 取消所有可取消 root 及尚未开始的 driver ticket；通知 kit 释放 work guard 并取消本地任务。
3. 等待 lease 计数由完成事件降至零。等待者注册一次性 idle continuation，不能用 timer/backoff 轮询。
4. idle 后调用插件 stop/destroy；仅当所有 DSO callback 都已返回、plugin context 无引用时 `dlclose`。

完成、cancel、driver callback 和关闭相互竞争时，终态只能由宿主 IO 线程提交一次。任意线程收到的 C ABI `done` 首先复制成宿主拥有 completion packet，再异步投递到 IO；不得在插件线程中恢复宿主 coroutine 或读取已释放 plugin context。

## 8. 线程、公平性和禁止轮询规则

- 默认所有 adapter 状态和插件业务协程在宿主所属 IO 线程交错运行，因此访问插件实例数据不需要额外锁。
- host `request_driver`、third-party completion、native wait callback 可以从其他线程到达，但只能投递小型拥有闭包；不得直接碰 plugin context 的非线程安全状态。
- 一个 driver 只能执行一个 `advance_once` / `poll_one`。如果一个 root 在本轮产生多个 ready continuation，它们通过后续真实 wake 或 bridge 的已知 ready queue 状态逐轮运行，从而避免单插件长时间独占 host executor。
- `poll_one()==0`、没有 active roots、没有 wake source 都不是**无条件**重新投递的理由。
  **唯一例外是"声明式受控轮询"**（`polled_tool`）：插件在注册时声明该操作等待的是
  插件本地 reactor 上的内核就绪事件，桥据此采用固定的、可观测的策略（只在有在途操作时
  轮询；有进展立即续票；无进展退避 `kPollIntervalMs = 10ms`；连续有进展超过
  `kPollBurstMax = 256` 步后让出 `kPollBurstYieldMs = 1ms`；无在途操作时零请求、零定时器）。
  禁止的是**隐藏**轮询（在无工作/无声明时持续申请请求或自旋），不是上述声明式驱动。
- 需要阻塞 CPU 或同步系统 API 的插件，可显式使用受限、可取消、可等待的工作线程设施；这与桥接协程不同，必须在插件能力说明中标明。

## 9. 内置插件迁移策略

### 9.1 先迁移框架和 kit

1. 把 API 表重置为新 v1，增加 `coroutine_runtime` IID，实现严格 version/size 校验。
2. 在 `PluginRuntime` 上实现 driver ticket、instance lease、关闭取消和一次性 idle 通知；删除依赖 `ioTasks_`、私有 `thread_pool(1)` 或定时检查来维持插件协程的路径。
3. 在 kit 实现 `PollOneBridge`、root adapter、host callback awaitable、错误/取消/late callback 诊断，并提供一个最小样例插件。
4. 使用一个不依赖私有 reactor 的演示：host sleep 或回调式 mock I/O，使宿主任务与插件 task 每次各推进一轮，验证不阻塞、不额外开线程。

### 9.2 filesystem 与 execute_command

`agentxx_filesystem` 的 `read`/`write`/`edit` 实现体本就是 asio 协程（`asio::stream_file`），已按第二轮方案迁移到 `polled_tool`（受控轮询驱动本地 reactor）；`list`/`glob`/`grep` 是同步文件遍历、扫描和计算 —— 不会因为包装为 coroutine 就变成非阻塞 I/O，继续使用 `blocking_tool + scheduler.offload`。
- 迁移判据：实现体是 asio 协程 + 等待内核就绪事件 → `polled_tool`；同步/CPU/阻塞 IO → `blocking_tool`。
- 禁止的是**隐藏**轮询（未声明时的自旋）；`polled_tool` 是声明式且参数可观测的受控轮询。

`agentxx_execute_command` 的 Boost.Process v2 管道、子进程退出和 timeout 已迁移到 `polled_tool`：管道/进程/计时器绑定协程 executor（桥的本地 reactor），由受控轮询推进，取消经 `CancelRegistry` 事件驱动 kill 进程组 + 关闭管道；非 Boost.Process v2 的 popen 回退分支仍是同步实现，保留 `blocking_tool`。同理，`agentxx_websearch` 的 HTTP 等待也已迁移。后续可由独立 host process 服务或 `wait_source` 把退避换成就绪通知，业务代码与 ABI 形态不变。

`ClientPluginManager` 的 `asio::thread_pool(1)` 也要逐用途审查。若仅用于给插件 coroutine 提供执行器，应迁到 host IO + driver ticket；如果用于不可避免的阻塞动态库工作，应改为显式、可关闭的后台设施并单独计数。

## 10. 测试计划

以下测试是 API/kit 重构的完成条件，至少覆盖 Linux Debug，关键 ABI 和生命周期用例同时在 Windows 编译运行：

1. C17 头文件独立编译、8 字节布局、调用约定、IID/version/`struct_size` 严格协商和 `query_interface` 失败路径。
2. `request_driver` 永不内联回调；start 返回后才可观察到第一次 plugin handler。
3. 每一 driver ticket 最多执行一次 `advance_once`；Asio bridge 每 ticket 恰好一次 `poll_one()`。
4. 无 root、无 local ready handler、无外部 completion 时不新增 ticket，空闲 CPU 不自旋。
5. wake 在 driver 前、driver 中、driver 后到达均不丢失；多个并发 wake 被合并，且不会漏掉最后一次。
6. host callback awaitable 完成后，continuation 先 post 到 local context，再由下一次 driver 恢复；验证没有同步重入。
7. 取消在 start 前、local task 挂起、host callback 已排队、driver 正在运行时都只产生一个终态；迟到 callback 不访问销毁的 root。
8. disable/unload 取消未开始 ticket，等待运行中的 ticket 和完成 callback 返回；`dlclose` 前 lease 必须为零。
9. 多实例同时运行，wake、operation id、取消和关闭互不影响；没有可变全局 bridge 状态。
10. mock callback source、host timer adapter 和未来 native wait-source adapter 分别验证 ready 后只产生必要 driver。
11. thread identity、ASan/UBSan/TSan 定向回归，以及长时间 idle 的 CPU 使用检查。
12. 明确反例测试：私有 `io_context` 上没有外部 wake 的 async operation 不得由 bridge 悄悄自旋推进；kit 输出可诊断失败或测试适配器拒绝该用法。

## 11. 分阶段落地与验收

### 阶段 A：ABI 和宿主运行时

落地新的 `coroutine_runtime` 表、driver ticket 状态机、lease/idle 关闭协议，并删除旧 ABI 兼容层。完成时可用纯 C mock plugin 验证 request/cancel/close，不依赖 Asio kit。

### 阶段 B：kit 和样例

实现 `PollOneBridge`、`Task` root adapter、宿主 callback awaiter 和取消桥接。样例插件使用 callback/mock event 或 host timer，证明 host 与 plugin coroutine 在一个 IO 线程交错执行。

### 阶段 C：内置插件逐个迁移（已完成，见 work.md 第二轮）

已迁移到 `polled_tool`：`agentxx_websearch`（3 工具）、`agentxx_execute_command`（bash/windows，Boost.Process v2 分支）、`agentxx_filesystem`（read/write/edit）。
保留 `blocking_tool`（显式例外）：filesystem 的 list/glob/grep、popen 回退分支、无 `BOOST_ASIO_HAS_FILE` 平台、`agentxx_rag_search` 的 CPU 段与 embedding 网络段（二期）。
每个迁移都附带测试：无自旋（空闲请求不增长）、有进展立即续/无进展退避、取消只产生一个终态、在途卸载、多实例隔离、宿主 IO 线程不被独占。

### 阶段 D：可选增强

验收标准是：普通插件协程与宿主协程可在同一 IO 执行序列交错推进；等待和取消均由完成/唤醒事件触发；空闲时没有 plugin driver 自旋；不需要为 plugin coroutine 创建专用线程；C ABI/COM 查询规则完整保留；无法得到 wake source 的原始异步库被明确拒绝或使用显式例外，而非以隐藏轮询伪装支持。
