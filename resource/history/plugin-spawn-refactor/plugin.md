# 插件后台任务 (spawn) 纳入宿主管理 —— 重构方案

> 状态: 设计提案 (未实施)
> 关联: `agent/plugins/agentxx_system_monitor` 周期采集 spawn 协程在卸载时悬挂泄漏
> 的修复演进 —— 旧方案 采用 `agentxx_plugin_agent_prepare_unload` 可选符号 (已落地,
> 见 git 历史); 将 spawn 纳入宿主统一任务管理, 消除特例。

---

## 1. 背景与动机

### 1.1 现状: spawn 游离在宿主管理之外

kit (`agent/lib/include/agentxx/plugin/api/plugin_kit.h`) 提供 `spawn` 让插件
启动后台协作任务 (如 system_monitor 的周期采集: `while(!cancelled()) { offload;
sleep; }`)。对比"工具/能力 op"与 spawn, 能清楚看到 spawn 缺了什么:

| | 工具/能力 op | spawn (旧方案 前现状) |
|---|---|---|
| 宿主登记 | `outstandingOps` + `AgentxxPluginOperatorHandle` | ❌ 无 (状态全在 DSO 内 `spawns_`) |
| 取消入口 | `detachAll` 统一调 `handle->cancelFn` | ❌ 无 (只能靠 prepare_unload 让 DSO 自查自停) |
| 完成通知 | `notify.done` → 宿主回收句柄 | ❌ 无 (协程结束宿主不知道) |
| 存活标记 | `InflightGuard` (inflight+1, 完成 -1) | ❌ 无 (所以 `waitInflightZero` 不等它) |

### 1.2 后果: 卸载时协程帧悬挂

spawn 协程挂在宿主 sleep 定时器上, 插件卸载时无人取消 → 协程帧 + 内部对象
泄漏 (cpu_gpu 测试曾报 504B: 协程帧 456B + outstandingCancel_ std::function
24B + cancelFlag control block 24B)。若在协程挂起时直接 `delete ctx` 或
`dlclose`, 恢复执行还会访问已销毁上下文 → UAF。

### 1.3 旧方案 修复及其局限 (prepare_unload)

旧方案 引入可选符号 `agentxx_plugin_agent_prepare_unload`, 宿主卸载时在 destroy 前
调用, 插件停止 spawn 协程; 宿主随后 `drainUnloadIo` (8×1ms 轮询泵) 让协程退出。

局限:
1. **停止只能由插件自己做** —— spawn 状态 (`spawns_`/`cancelFlag`/协程句柄) 在
   kit 内、DSO 内, 宿主只有不透明 `plugin_ctx`, 无法直接访问 → 必须插件导出
   入口, 宿主才能触发。
2. **停止是异步的** —— 唤醒需 post 到 io 队列, 协程要等 io 泵几轮才 resume →
   退出。`shutdownPlugin` 是同步函数无法 drain; drain 只能在 `unloadAsync`
   (协程) 里做。
3. **drain 是猜测式轮询** (8×1ms 固定轮数), 不是精确等待 —— 若协程唤醒链
   更长 (嵌套 post) 或 io 繁忙, 可能不足; 反之多数卸载 (无 spawn 的插件)
   白付 8ms。
4. **prepare_unload 必须在 detachAll 之前** —— detachAll cancel sleepTimers 是
   唤醒协程的免费窗口; 错过 (放 destroy 里) 则协程又睡到无人唤醒的新定时器上。

## 2. 方案本质

让 spawn 注册时获得一个与工具 op **同构**的宿主句柄:
持 inflight (存活标记) + 可取消 + 完成通知。卸载时 detachAll 统一 cancel、
waitInflightZero 统一等待 —— prepare_unload + drainUnloadIo 均不再需要。

关键机制全部现成, 无需发明:
- `AgentxxPluginOperatorHandle` = `{caller, cancelFn, cancelled}` (plugin_manager.h)
- `OpCore` + `notify()` + `doneSignal` (op_driver.h; notify.done 实体 `OpCore::onDone`
  已内置: notified 原子 CAS 恰好一次 + guard.reset + 完成信号)
- **done 后句柄回收协程**: `callToolAsync`/`invokeCapabilityAsync` 已内建同款
  (等 doneSignal → 从调用方 outstandingOps erase; plugin_manager_capability.cpp) ——
  新方案直接**复用该现成模式**, 建议抽公共函数避免第三份拷贝
- `InflightGuard`: `waitInflightZero` 的计数来源
- `PromiseBase::notify_` / `opCleanup_` / `cancel_outstanding` (kit 已具备)

---

## 3. 接口设计 (ABI 增量, 向后兼容)

新增**独立接口表** (不动 `agentxx.agent.scheduler` 旧方案 布局 —— C 结构体加字段
须升版本; 独立新表最干净, 老插件查不到即降级):

```c
/* plugin_api.h */
#define AGENTXX_PLUGIN_IFACE_AGENT_TASKS         "agentxx.agent.tasks"
#define AGENTXX_PLUGIN_IFACE_AGENT_TASKS_VERSION 1

typedef struct AgentxxPluginTasksIface {
    int version; ///< == AGENTXX_PLUGIN_IFACE_AGENT_TASKS_VERSION

    /// 注册后台任务 (io 线程调用)。宿主记录句柄 (可取消/跟踪完成/持 inflight),
    /// 插件协程最终结束时经 *notify 上报 (恰好一次) → 宿主回收句柄。
    /// - cancel_fn/ud: 宿主卸载取消时回调 (io 线程, 协作式):
    ///   置 cancelFlag + 唤醒挂起的 sleep/offload
    /// - notify: 【出参】宿主填写的完成通知器 (AgentxxPluginOperatorNotify 值拷贝);
    ///   插件协程结束 (帧销毁后) 经 notify.done 恰好一次上报 → 宿主 guard.reset +
    ///   回收句柄。若以 const 指针形式入参则无法回填, 宿主只能自建一个无法
    ///   告知插件的 notify —— 与本表"插件上报完成"的语义矛盾, 必须为出参
    /// - notify.done 线程属性与既有 ABI 契约一致: 可从【任意线程】回调
    ///   (宿主 OpCore::onDone 内部 CAS + 投递回 io, 线程安全) —— spawn 协程
    ///   内若直接调用宿主回调形接口 (invoke_capability_async 等) 或经自管
    ///   线程收尾, 上报可能非 io 线程, 宿主必须按任意线程实现
    /// - 返回句柄 (失败 NULL + error_out)
    AgentxxPluginOperatorHandle* (*register_task)(
        const AgentxxPluginHost*               host,
        AgentxxPluginOperatorCancelFunction    cancel_fn,
        void*                                  cancel_ud,
        AgentxxPluginOperatorNotify*           notify,      ///< [out] 见上
        char**                                 error_out
    );
    /// 取消任务 (幂等; 仅限 io 线程调用, 或宿主内部经 ioCallSync 投递后调用)
    /// - 与宿主 detachAll 内部路径一致; 句柄由宿主托管, 跨线程主动取消
    ///   需经 scheduler.post_to_io/ioCallSync 回到 io 线程 (与注册类接口
    ///   线程约束一致), 避免 handle->caller 裸指针跨线程反查实例
    void (*cancel_task)(AgentxxPluginOperatorHandle* h);
} AgentxxPluginTasksIface;
```

**为什么用接口表而非新增入口符号 (dlsym)**:
- 接口表是进程级静态只读、可版本协商的现有机制 (COM 风格 `query_interface`),
  多实例契约天然满足;
- 入口符号是"每实例一对" (create/destroy), 后台任务注册是"一实例内多次"
  (一个插件可 spawn 多个), 接口表更合适;
- 与 tools/hooks/scheduler 等现有接口表一致, 插件侧 `AgentIfaces::query` 一次
  查询全部缓存。

---

## 4. 宿主侧实现 (复用现成模式)

### 4.1 注册实现 —— 复刻 callToolAsync 的句柄登记模式

宿主侧**现成模板**即 `callToolAsync` (plugin_manager_capability.cpp): 句柄推入
调用方 outstandingOps → guard(目标/自身实例) + OpCore → start 后挂清理协程
(等 doneSignal → erase 句柄)。registerTask 与其差别仅是**没有 start/cb**:
句柄登记后即"挂起", 完成完全由插件侧 notify 驱动。实现(示意, 与现成模式对齐):

```cpp
// plugin_manager_vtable.cpp 新增 g_ifaceTasks (含 xx_register_task 适配)
AgentxxPluginOperatorHandle* PluginManager::registerTask(
    PluginInstance* inst,
    AgentxxPluginOperatorCancelFunction cancel_fn, void* cancel_ud,
    AgentxxPluginOperatorNotify* notify_out, char** error_out
) {
    // 1. 句柄登记 (与工具 op 同列表, detachAll 统一取消; 生命周期由下述清理协程托管)
    auto handle = std::make_shared<AgentxxPluginOperatorHandle>();
    handle->caller = inst;
    inst->outstandingOps.push_back(handle);

    // 2. 任务存活期间持 inflight —— waitInflightZero 会等到任务结束
    //    (guard 以 shared_ptr 挂在 core 上, notify 到达才 reset —— 见下注)
    auto guard = std::make_shared<PluginInstance::InflightGuard>(inst);
    auto core  = std::make_shared<OpCore>(ioExecutor_, guard);

    // 3. 宿主侧等待 done 的清理协程: 照抄 callToolAsync 内建同款 (建议抽公共函数)
    std::weak_ptr<PluginInstance>              weakCaller = inst->self;
    std::weak_ptr<AgentxxPluginOperatorHandle> weakHandle = handle;
    asio::co_spawn(ioExecutor_, [core, weakCaller, weakHandle]() -> asio::awaitable<void> {
        if (!core->notified.load(std::memory_order_acquire)) {
            asio::steady_timer t(co_await asio::this_coro::executor);
            t.expires_at(std::chrono::steady_clock::time_point::max());
            co_await t.async_wait(asio::bind_cancellation_slot(
                core->doneSignal.slot(), asio::as_tuple(asio::use_awaitable)));
        }
        auto callerSp = weakCaller.lock();
        auto handleSp = weakHandle.lock();
        if (!callerSp || !handleSp) return;
        auto& vec = callerSp->outstandingOps;
        vec.erase(std::remove(vec.begin(), vec.end(), handleSp), vec.end());
    }, asio::detached);

    // 4. 宿主侧取消回调 → 转发给插件的 cancel_fn
    handle->cancelFn = [inst, cancel_fn, cancel_ud]() {
        if (cancel_fn) cancel_fn(cancel_ud, /*op=*/nullptr);
    };

    *notify_out = core->notify();  // 协程结束 → core.onDone → guard.reset + doneSignal
    return handle.get();
}
```

> **注 (inflight 归零与句柄回收的因果澄清)**: `waitInflightZero` 能等到 spawn,
> 靠的是步骤 2 的 guard 以 shared_ptr 挂在 core 上、`OpCore::onDone` 在收到
> notify 时 `guard.reset()` (inflight-1) —— 与步骤 3 的列表 erase **无因果
> 关系**。步骤 3 (清理协程) 只负责回收宿主侧句柄记录: 协程完成通知到达前,
> core 一直被该协程持 shared_ptr, 故 handle 即使仍在 outstandingOps 也安全
> (detachAll 里 cancelFn 转发给已完成的 spawn 无副作用; erase 与 detachAll 的
> clear 并发同线程串行无冲突)。该回收与工具 op 的现状语义一致 —— erase 是
> "收尾整洁"而非"等待的前提"。

取消/完成语义自动获得:
- **取消**: detachAll 经 `op->cancelled` CAS 后调 `cancelFn` → 插件 cancel_fn
  (置 cancelFlag + 唤醒挂起协程); `OpCore::cancelRequested` 仅工具 op 的
  awaitPluginOp 使用, 本表任务不设等待方, 无需 cancelRequested 路径
- **完成**: `notify.done` → `core.onDone`: notified CAS → `guard.reset()`
  (inflight-1) → `doneSignal.emit` → 清理协程把 handle 从 outstandingOps 移除

### 4.2 detachAll —— 结构性零改动, 存在一处已覆盖的边界

detachAll 主体**零改动**: spawn 句柄已混入 outstandingOps, 与工具 op 一起被
统一 cancel; 末尾 `inst->outstandingOps.clear()` 与清理协程的 erase 同在
io 线程串行, 无冲突; `sleepTimers` cancel 兜底唤醒 (与现状一致)。

```cpp
void PluginManager::detachAll(PluginInstance* inst) {
    for (auto& op : inst->outstandingOps) {   // spawn 句柄已在此列表
        if (op && !op->cancelled...) { op->cancelled = true; op->cancelFn(); }
    }
    inst->outstandingOps.clear();
    for (auto& [key, timer] : inst->sleepTimers) timer->timer->cancel();  // 唤醒挂起 sleep
    ...
}
```

**一处需注意的边界 (已被现成机制覆盖, 无需改代码)**: 若 spawn 任务恰在
detachAll **之前**自然完成, 其清理协程会把 handle 从列表 erase; 若在
detachAll **之后**才完成 (cancel 已触发、协程退出中), 此时列表已被 clear,
清理协程的 erase 作用于空列表 —— 无害。两种交错下 handle 均不会悬垂:
句柄 shared_ptr 由清理协程持有, detachAll 只丢列表不销毁。

spawn 协程挂在 sleep 上 → detachAll 的 `timer->cancel()` 唤醒它 (cancel handler
仍触发) → 协程 resume 后发现 cancelFlag 已置 → 退出 → notify → inflight-1。

### 4.3 unloadAsync —— 删掉 drain 特例, 回到原有闭环

```cpp
// prepareUnload(inst);       // ❌ 删除
detachAll(inst.get());        // 统一 cancel 工具 op + spawn 任务
...
bool ok = co_await waitInflightZero(inst, 30s);  // ✅ 现在会等到 spawn 协程退出
if (!ok) co_return false;
// co_await drainUnloadIo();  // ❌ 删除 (不再需要轮询泵 io)
destroy(...);                 // 此刻所有 spawn 协程已结束, dlclose 安全
```

`shutdownPlugin` (同步析构路径) 同样受益: detachAll cancel 后 inflight 计数
保证; 同步路径仍无法等待, 进程收尾场景 OS 回收即可 (与工具 op 现状一致,
不恶化)。

---

## 5. kit 侧改造 (plugin_kit.h 的 spawn)

> **注意**: kit 内 spawn 现有**两份实现** —— `PluginBase::spawn` (成员模板)
> 与自由函数 `spawn(Ctx&, Fn&&)` (system_monitor 实际使用后者), 二者都要
> 同步改造 (建议抽公共 helper, 避免双份漂移)。以下以自由函数版示意。

```cpp
template<typename Ctx, typename Fn>
inline void spawn(Ctx& ctx, Fn&& fn) {
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    auto rec = std::make_shared<SpawnRecord>();   // cancelFlag + coroAddr(挂起句柄)
    rec->cancelFlag = cancelFlag;

    // ① 查宿主任务接口; 老宿主无此表 → 降级 (不注册, 仅日志)
    auto tasksIface = ctx.iface.tasks;   // AgentIfaces 新增成员
    AgentxxPluginOperatorNotify hostNotify{nullptr, nullptr};
    if (tasksIface && tasksIface->register_task) {
        // cancel_fn: DSO 闭包, 置 cancelFlag + 唤醒挂起协程 (经 rec)
        AgentxxPluginOperatorNotify notify;
        auto* h = tasksIface->register_task(
            ctx.host,
            [](void* ud, void*) {                    // cancel_fn
                auto* r = static_cast<SpawnRecord*>(ud);
                r->cancelFlag->store(true, std::memory_order_release);
                if (r->coroAddr) {
                    auto handle = std::coroutine_handle<
                        detail::PromiseBase<void>>::from_address(r->coroAddr);
                    handle.promise().cancel_outstanding();  // cancelSleep / offload cancel
                }
            },
            rec.get(), &notify, &err);
        if (h) hostNotify = notify;
    }

    auto starter = [&ctx, fn, cancelFlag, rec, hostNotify]() {
        auto task = fn(ctx, ctl);          // 协程创建
        ...
        p.notify_     = hostNotify;        // ② finishIfDone 自动上报完成
        p.cancelFlag_ = cancelFlag;
        h.resume();
        if (!h.done()) rec->coroAddr = h.address();
        // ③ 协程完成时清 coroAddr (防 cancel_fn 悬空; 见竞态分析)
        p.opCleanup_  = [rec]() { rec->coroAddr = nullptr; };
        detail::finishIfDone(h);           // 结束时: 帧销毁 → notify.done → 宿主回收
    };
    ...
}
```

复用 `PromiseBase::notify_` (finishIfDone 已内置上报逻辑) + `opCleanup_`
(此前为 Job 泄漏加的字段) —— **工具协程与 spawn 协程走完全相同的完成协议**。

`ctx.spawns_` 可保留 (降级模式 + ctx 内自省), 新机制下宿主持有权威记录。

**与降级路径的对齐 (设计约束)**: `p.notify_ = hostNotify` 使"上报完成"仅发生在
**新宿主 (tasks 表存在) 且注册成功**时。若 register_task 失败/表缺失
(hostNotify 为空), spawn 退化为纯自管协程 —— 此时 finishIfDone 不触发宿主
回收, 与 旧方案 现状 (prepare_unload 自停) 相同, 不构成回归。

**线程约束注释 (实现时必须落注释)**: `opCleanup_` (清 coroAddr) 在 finishIfDone
内执行, 而 finishIfDone 的调用点 (各 awaiter 完成闭包/start 尾部) 恒在 io
线程 —— 与宿主侧 cancel_fn (io 线程) 无并发; 但 notify.done 本身可能被宿主
从任意线程回调 (见 §3), 二者是不同路径, 不构成竞态前提。

**降级/未注册时的泄漏口径**: 老宿主 + 新 kit spawn 不注册, 协程生命周期与
旧方案 相同 (依赖插件导出 prepare_unload 自停或进程收尾 OS 回收) —— 跨版本
固有限制, 仅日志警告, 不阻塞。

---

## 6. 完整时序

```
插件加载: spawn() → register_task → 宿主: outstandingOps+1, inflight+1
           → 协程挂起在 sleep (宿主 sleepTimers 有记录)

运行期:   协程每周期被 sleep 唤醒 → 工作 → 又挂起   (宿主无感, 句柄静默)

热卸载 unloadAsync:
  detachAll:
    ├─ cancel 工具/hook/cap op
    ├─ cancel spawn 句柄 → cancel_fn → 置 cancelFlag + cancel_outstanding
    │     → cancelSleep → 定时器回调 post → 协程被唤醒
    └─ cancel sleepTimers (兜底)
  waitInflightZero:
    ├─ 协程 resume → while(!cancelled()) 退出 → fn 返回 → finishIfDone
    │     → 帧销毁 → notify.done → guard.reset (inflight-1) → 宿主 erase 句柄
    └─ inflight 归零 → 返回 true
  destroy → delete ctx + dlclose        ← 所有协程帧已销毁, 无 UAF 无泄漏
```

**为什么 waitInflightZero 能等到**: spawn 任务从注册到结束**持续持 inflight**
(`InflightGuard` 挂在 core 上, notify 时才 reset) —— 这正是此前缺失的
"存活标记"。

**与工具 op 的一个本质差异 (已由 §4.1 设计覆盖)**: 工具 op 有宿主等待方
(awaitPluginOp/反向调用 cb), 其 core 上挂着 chan 接收协程; spawn 任务**没有
等待方** —— 唯一"等"它的是 unloadAsync 的 waitInflightZero (轮询 inflight
计数)。故 spawn 的 core 无需 chan 接收协程, 仅靠 doneSignal 唤醒清理协程
(§4.1 步骤 3) + onDone 的 guard.reset 计数。unloadAsync 不依赖该清理协程的
执行进度 (计数已由 onDone 原子完成), 只依赖 inflight 归零 —— 二者无耦合。

---

## 7. 边界与竞态分析

| 场景 | 处理 |
|---|---|
| 协程异常逃逸 | `Task::unhandled_exception` 捕获 → finishIfDone 以 FAILED 上报 → 正常回收 ✓ |
| cancel 与完成竞态 | 见下注: 安全性由 `OpCore::onDone` 的原子 notified CAS (恰好一次, 幂等) + `opCleanup_` 清 coroAddr 双保险保证, **而非依赖 io 线程串行**。实现注释须声明两点约束: ① kit 协程完成路径 (finishIfDone 调用点) 恒在 io 线程; ② notify.done 本身可任意线程 (宿主实现按此契约, 勿假设串行) |
| cancel 后协程不退出 (死循环不检查 cancelled) | 协作式取消, waitInflightZero 30s 超时 → unloadAsync 返回 false 不 destroy (与工具 op 现状一致, 不恶化) |
| 插件 spawn 多个任务 / 嵌套 spawn | 每个独立句柄, 全在 outstandingOps → detachAll 全 cancel, inflight 全等 ✓ |
| 多实例 | 句柄/guard 全挂 PluginInstance, kit 无全局状态 → 天然实例隔离 ✓ |
| 老宿主 + 新插件 | 查不到 `agentxx.agent.tasks` 表 → spawn 降级不注册 (协程照跑, 无法被宿主回收 —— 跨版本固有限制, 仅日志警告) |
| 新宿主 + 老插件 | 老 kit spawn 不注册 → 宿主无句柄。**保留 prepare_unload 的价值**: 第三方手写后台任务的插件仍可用它自停 |
| disable / 替换插件 | 走 detachAll 同一路径 ✓ |
| client 侧插件 | `ClientPluginManager` 继承同一 `PluginManagerBase`, 接口表加 client 版即可扩展 (当前无 client spawn 使用, 可延后) |

---

### 7.1 竞态注 (cancel vs done) —— 为什么是"原子双保险"而非"线程串行"

**错误假设 (须避免)**: "cancel_fn 与 notify 都在 io 线程, 故串行无竞态"。
核对 ABI 契约与宿主实现:
- `AgentxxPluginOperatorNotify.done` 明确**可从任意线程回调** (plugin_api.h;
  `OpCore::onDone` 用无锁 CAS + chan + post 回 io 派发, 正是为任意线程而设计)。
  spawn 协程体内若直接调用宿主回调形接口 (`invoke_capability_async` 等, 其
  done 可能由目标插件在任意线程上报后经宿主转发) 或插件自管线程收尾,
  通知路径可能不在 io 线程。
- 协程侧 `finishIfDone` 的**调用点** (各 awaiter 完成闭包/start 尾部) 恒在
  io 线程 —— 这是真实的串行面, 但不是通知路径的全部。

**真实的安全机制 (双层)**:
1. **宿主侧原子性**: `OpCore::onDone` 用 `notified.compare_exchange` 保证
   上报恰好一次 (重复/迟到 notify 直接释放 payload 返回); `doneSignal.emit`
   与 guard.reset 只在 CAS 成功方执行。cancel_fn 是否已调、何时调, 不影响
   onDone 的正确性 —— 取消是协作式, 完成总是合法终态。
2. **插件侧 coroAddr 防悬空**: cancel_fn (io 线程) 经 `rec->coroAddr` 取
   协程句柄唤醒; 协程完成路径 (io 线程) 在 finishIfDone 里先 destroy 帧、
   后经 `opCleanup_` 清 `rec->coroAddr`。若 cancel_fn 与完成交错: coroAddr
   若非空则协程帧必存活 (完成路径尚未 destroy); 若已清空则 cancel_fn 只置
   cancelFlag 不碰帧 —— 无 UAF。

**结论**: 双层均为"恰好一次 + 幂等"语义, 竞态只会导致"取消迟到/无效"
(协作式取消的固有语义), 不会导致 UAF/双重上报。实现注释应声明: ① kit
协程完成调用点恒在 io 线程; ② notify.done 宿主侧按任意线程实现; ③
coroAddr 仅在 io 线程读写 (cancel_fn 与完成路径都在 io 线程访问它)。

---

## 8. 与 prepare_unload (旧方案) 的关系

| | prepare_unload (旧方案 现状) | 宿主任务管理 (重构后) |
|---|---|---|
| 停止触发 | 插件自查自停 (可选符号) | 宿主统一 (outstandingOps) |
| 等待完成 | drain 轮询 8×1ms (猜测式) | waitInflightZero (精确计数) |
| 完成感知 | 无 | notify.done (精确) |
| 架构位置 | 特例挂钩 | 与工具 op 统一 |

新方案落地后:
1. **移除** unloadAsync 里的 `drainUnloadIo` 轮询与 `prepareUnload` 调用
   (system_monitor 改走新 spawn, 撤销其 prepare_unload 导出)
2. **保留** `agentxx_plugin_agent_prepare_unload` 符号与文档, 降级为
   "**第三方插件手写后台任务的兼容逃生口**" (它不依赖 kit spawn, 语义独立, 无害)
3. 新增文档章节描述 kit spawn 自动注册行为

---

## 9. 分步实施计划

1. **plugin_api.h**: 新增 `agentxx.agent.tasks` 接口表 + 语义注释
   (register_task 的 notify 为**出参** `AgentxxPluginOperatorNotify*`;
   notify.done 按**任意线程**契约注释; cancel_task 限定 io 线程或经投递调用)
2. **plugin_manager_vtable.cpp**: 实现 `g_ifaceTasks`
   (xx_register_task/xx_cancel_task 适配 + registerTask, 复用
   OpCore/InflightGuard/清理协程模式; 建议把 callToolAsync 内建句柄回收协程
   抽为公共函数, 与 registerTask 共用)
3. **plugin_iface_helper.h**: `AgentIfaces` 增加 tasks 查询
4. **plugin_kit.h**: spawn 注册 + `p.notify_`/`opCleanup_` 挂接 + 降级路径;
   **`PluginBase::spawn` 与自由函数 `spawn(Ctx&, Fn&&)` 两份实现都要改**
   (建议抽公共 helper); `PluginBase::stopSpawns` 标记废弃 (保留兼容)
5. **plugin_manager_lifecycle.cpp**: unloadAsync 删除 drain/prepare 特例,
   回归纯 waitInflightZero 
6. **system_monitor**: 撤销 prepare_unload 导出 (回归纯 spawn)
7. **测试**:
   - cpu_gpu 无泄漏 (现有用例即覆盖: load → spawn 挂起 → unload 快速回收;
     注: 现有 test_cpu_gpu_use.cpp **未直接断言 spawn 泄漏**, 仅覆盖
     工具/能力/卸载 —— 建议补显式泄漏断言或依赖下述新增用例)
   - 新增: 多 spawn + 挂起 sleep 的插件卸载回收测试; cancel 后协程退出确认
     (inflight 归零); **register_task 的 notify 从非 io 线程上报的竞态测试**
     (插件自管线程完成路径, 验证 OpCore 原子双保险)
   - 全量回归 (plugins/client_plugins/plugin_resources 等)
8. **文档**: plugins.md 更新 spawn 语义 + tasks 接口表说明
   (含线程约束: notify.done 任意线程 / cancel 与完成双保险)

## 10. 权衡与建议

**收益**: spawn 从 "kit 特例" 变成 "与工具 op 同构的一等公民", 卸载闭环精确
(不再猜测式 drain); 未来任何插件用 spawn 都自动安全; prepare_unload 可退居
兼容位。

**成本 (重估)**: 新接口表 + 宿主实现约 100~150 行 (registerTask + 清理协程
公共化/复用 + cancel_task 适配与线程投递语义); kit spawn 改造约 60~80 行
(两份实现同步 + notify_/opCleanup_ 挂接 + 降级路径); 另加测试与文档。
涉及 ABI 增量与多端 (agent/client) 同步考量。

**结论**: 目前 spawn 只有 system_monitor 一个使用者, prepare_unload (旧方案) 已解决
其泄漏 —— 新方案重构的**直接收益有限**, 价值在于**架构统一与消除特例** (为将来
更多插件使用后台任务打基础)。若倾向保守, 旧方案 已可用; 若认同 "统一优于特例",
按第 9 节步骤实施, 风险点主要在 kit 竞态约束注释与跨版本降级行为 (第 7 节
已给出处理)。
