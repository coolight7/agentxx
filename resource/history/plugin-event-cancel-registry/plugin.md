# 插件框架级事件驱动取消检测架构方案 (CancelRegistry Framework Integration)

> **目标**：将原先仅在 `agentxx_execute_command` 内部验证成熟的事件驱动取消注册表（`CancelRegistry`）提升为插件 SDK 框架（`plugin_kit.h`）的通用基础设施，彻底终结插件工作线程向宿主 IO 线程高频跨线程轮询查询取消状态（`c.sessionCancelled` / `ioCallSync`）所引起的持续系统抖动，同时为所有持有外部实体句柄或长周期阻塞调用的插件提供微秒级响应的主动中断（Active Interruption）能力。
>
> **核心决策摘要**：
> 1. **提升至框架基类**：在 `agent/lib/include/agentxx/plugin/api/plugin_kit.h` 中将 `CancelRegistry` 确立为通用标准组件，内嵌于 `PluginBase` 中，天然满足“多实例三铁律”且零全局/静态状态。
> 2. **C ABI 零改动与零入侵**：宿主底层跨边界协议（`AgentxxPluginToolSpec::execute_cancel`）完全保持向下兼容，由框架胶水层（`blocking_tool` / `tool`）无缝拦截取消调用并向 `ctx.cancelRegistry` 派发事件。
> 3. **分类治理取消模型**：明确划分并标准化“主动外部中断型”、“密集循环本地检查型”以及“原生协程挂起型”三大取消应用场景。
> 4. **终结 IO 线程抖动隐患**：对 `PluginBase::sessionCancelled` 进行内存化优化，优先读取 `CancelRegistry` 内部原子缓存，使 `filesystem`、`rag_search` 等密集遍历插件彻底摆脱每步循环都同步 `post` 到宿主 IO 线程的严重性能陷阱。

---

## 1. 背景与现状深度剖析

### 1.1 为什么只有 `execute_command` 曾写出 20ms 轮询协程？

在旧实现中，`agentxx_execute_command` 的协程管线内部包含如下循环：
```cpp
// 旧实现: agent/plugins/agentxx_execute_command/execute_command_impl.h
while (false == isCancelled()) {
    timer.expires_after(std::chrono::milliseconds(20));
    auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
    if (ec) co_return;
}
detail::killProcGroup(proc, winJob);
```
之所以在这里出现了 20ms 挂起轮询，而其他插件看似没有，根源在于**托管实体的生命周期边界与自治性截然不同**：

| 维度 | 普通纯计算/API 插件 (`string`, `math`, `planning`) | 操作系统子进程 (`execute_command`) |
| :--- | :--- | :--- |
| **执行实体** | 宿主进程内的用户态线程 / 协程帧 | **操作系统独立进程树（Process Tree）** |
| **中断机制** | 协程销毁帧，或 CPU 函数提前 `return` | 必须显式由外部发送系统信号（`kill(-pgid, SIGKILL)` 或 `TerminateJobObject`） |
| **管道/资源依赖** | 纯内存计算，无外部描述符死等 | 孙进程持有管道写端，**若不主动 kill，`async_read` 永远无法得到 EOF，工具调用永久卡死** |
| **取消感知需求** | 下一次循环检查即可退出 | **必须有常驻监听者在取消发生的第一时间执行强制斩杀** |

由于此前插件框架未提供“取消事件推送”通道，`execute_command` 只能自建协程，以 20ms 为周期反复查询 `isCancelled()`。

### 1.2 为什么其他插件看似“没有轮询”，却暗藏更严重的系统抖动隐患？

检查当前 `agentxx_filesystem`（`agent/plugins/agentxx_filesystem/agentxx_filesystem.cpp`）的实现：
```cpp
// agentxx_filesystem.cpp 中的经典取消回调
auto isCancelled = [&c, tid, cancel_flag]() -> bool {
    if (cancel_flag && *cancel_flag != 0) return true;
    return c.sessionCancelled(agentxx::plugin::PluginStringView::from(tid.data(), tid.size()));
};
```
在 `filesystem` 执行 `glob`（大目录递归）或 `grep`（多文件多模式正则匹配）时，内部循环长这样：
```cpp
for (const auto& pattern : file_patterns) {
    if (checkCancel() || deadline.expired()) { // 每次迭代均调用 isCancelled
        return "[Error] Cancelled or timed out";
    }
    // ... 遍历文件树
}
```
跟踪 `c.sessionCancelled(tid)` 的底层执行链路：
```
c.sessionCancelled(tid)
  └─> host->iface.cancel->is_cancelled(host, &tid)
        └─> xx_cancel_is_cancelled()  (plugin_manager_vtable.cpp)
              └─> ioCallSync<bool>(mgrPtr, [mgrPtr, tid]() {
                    // 同步构造 promise/future
                    // asio::post(ioExecutor, promise.set_value(...))
                    // worker 线程挂起等待 future.get() !!
                    return mgrPtr->isSessionCancelled(tid);
                  })
```
**严重隐患揭示**：
虽然 `filesystem` 没有写 20ms 的定时器轮询，但它在处理包含成千上万个文件的代码库时，**每一次文件遍历、每一批正则扫描都在调用 `c.sessionCancelled`**。
这意味着工作线程每秒钟数百上千次向宿主主 IO 线程投递同步任务并强制等待上下文切换：
1. **主 IO 线程被严重抖动拖垮**：主 IO 线程负责 websocket 通信、TUI 交互、LLM 流式消息接收；密集 `ioCallSync` 导致网络包积压与 TUI 界面严重卡顿掉帧。
2. **工作线程自身吞吐量断崖式下跌**：大部分时间浪费在跨线程同步等待与调度让步上。

### 1.3 插件操作模型的分类学（Taxonomy）

为了统一框架级取消设计，插件运行模式可系统性划分为三类：

```
                           ┌──────────────────────────────────────────────┐
                           │               插件操作分类模型                │
                           └──────────────────────┬───────────────────────┘
                                                  │
          ┌───────────────────────────────────────┼───────────────────────────────────────┐
          ▼                                       ▼                                       ▼
┌──────────────────────────┐            ┌──────────────────────────┐            ┌──────────────────────────┐
│  模式 1: 原生协程挂起型   │            │ 模式 2: 密集循环检查型   │            │  模式 3: 外部实体中断型   │
│   (Native Coroutine)     │            │   (Cooperative Loop)     │            │   (Active Interruption)  │
├──────────────────────────┤            ├──────────────────────────┤            ├──────────────────────────┤
│ 代表: example, monitor   │            │ 代表: filesystem, rag    │            │ 代表: execute_command,   │
│ 特征: 运行于宿主 PollLoop│            │ 特征: 线程池跑 CPU 密集  │            │       audio_stream       │
│ 原语: kit::sleep/tool    │            │ 原语: 阶段边界检查标志   │            │ 特征: 外部进程/阻塞socket│
│ 诉求: 宿主销毁帧即可     │            │ 诉求: 零跨线程本地原子读 │            │ 诉求: 事件驱动秒级斩杀   │
└──────────────────────────┘            └──────────────────────────┘            └──────────────────────────┘
```

**核心结论**：
插件框架迫切需要一套通用的 **事件驱动取消注册表（`CancelRegistry`）**，既满足“模式 3”对**异步主动中断回调**的刚需，又满足“模式 2”对**零开销本地原子取消状态**的诉求。

---

## 2. 约束分析与多实例三铁律

框架级通用设计必须严格满足项目既定的插件架构规范（见 `AGENTS.md` 及 `docs/zh-cn/plugins.md` 4.2 节）：

### 2.1 多实例三铁律严格恪守
1. **禁止可变全局/函数级 static 缓存**：`CancelRegistry` 必须作为 `PluginBase` 的普通成员，其生命周期随具体宿主在堆上分配的 `PluginCtx`（`*plugin_ctx`）创建与销毁。同进程内由不同 Agent 宿主创建的多个插件实例拥有互不干扰的独立注册表。
2. **状态绑定宿主实例**：宿主取消通知通过 C ABI 的 `spec.user_data`（即 `BlockShim*` 或 `ToolShim*`）精确还原对应的插件实例指针 `ctx`。
3. **接口表依赖就地隔离**：注册表仅在插件端内存调度，不向宿主泄漏非标准 C++ 类型。

### 2.2 线程模型安全性
* **并发调用方**：
  - **触发线程**：宿主主 IO 线程（用户点击中断 / HTTP客户端断开 / 会话超时，触发 `spec.execute_cancel`）。
  - **任务线程**：宿主调度器线程池（`scheduler->offload` 工作线程，运行具体的工具实现代码）。
  - **命令内部协程**：在任务线程私有 `io_context` 或线程池中运行的子协程。
* **竞态防护诉求**：
  - 取消回调的注册与注销高频交织；
  - `cancel(key)` 执行时，绝不能因持有全局互斥锁执行外部业务回调而发生死锁；
  - **零悬挂保证（Anti-UAF Protocol）**：当命令自然结束退出其作用域、栈上对象（如 `proc`, `pipes`, `sockets`）即将析构时，必须保证正在并发执行的取消回调彻底退出后，才允许完成注销并释放资源。

---

## 3. 总体架构设计

### 3.1 端到端取消事件传播链路

```
[ 用户在客户端触发取消 / 会话轮次超时 ]
                 │
                 ▼
[ 宿主 IO 线程: Session CancelToken 取消信号发射 ]
                 │
                 ▼
[ 宿主 awaitPluginOp (op_driver.h): cancelOp->slot() 监听到信号 ]
                 │
                 ▼ (安全调用一次 safeCancelOnce)
[ C ABI 契约: spec.execute_cancel(user_data, op) ]
                 │
                 ▼ (plugin_kit.h SDK 统一拦截层)
[ 插件实例上下文: ctx->cancelRegistry.cancel(tid) ]
                 │
                 ├───────────────────────────────────────────────────────┐
                 │                                                       │
                 ▼                                                       ▼
  【主动中断型: execute_command 等】                        【循环检查型: filesystem 等】
   触发通过 bind() 注册的取消回调:                          本地内存原子标志已生效:
   1. ::kill(-pid, SIGKILL) / TerminateJobObject          1. ctx.cancelRegistry.isCancelled(tid)
   2. post(ex, closePipes); cancelTimer.cancel()              == true
   3. 挂起协程立即被唤醒                                   2. 循环下次迭代 0ns 本地内存判断命中
   4. 主工作管线立刻读取到 EOF 并安全收尾                   3. 立即提前退出循环并返回
   (全程 0 次 20ms 轮询, <50μs 响应)                       (全程 0 次 ioCallSync, 0 线程抖动)
```

---

## 4. 框架组件核心设计与实现规约

### 4.1 通用 `CancelRegistry` 类设计

将 `CancelRegistry` 迁移至 `agent/lib/include/agentxx/plugin/api/plugin_kit.h` 的 `namespace agentxx::plugin` 中。

```cpp
namespace agentxx::plugin {

/// 插件框架事件驱动取消注册表
/// - 职责: 管理会话级别与操作级别的取消事件注册、注销与原子通知
/// - 线程安全: 完全支持多线程并发注册、注销与触发
/// - 内存自治: 纯堆内存实例，无任何全局/静态状态，严格契合多实例契约
class CancelRegistry {
public:
    using CancelCallback = std::function<void()>;
    using RegId          = uint64_t;

    /// 回调实体封装：支持并发排他保护与生命周期状态标记
    struct CallbackEntry {
        std::mutex     mu;
        bool           disposed{false};
        CancelCallback cb;
    };

    CancelRegistry()  = default;
    ~CancelRegistry() {
        cancelAll();
    }

    CancelRegistry(const CancelRegistry&)            = delete;
    CancelRegistry& operator=(const CancelRegistry&) = delete;

    /// 注册取消回调
    /// - `key`: 会话标识 (sessionId / thread_id)，为空表示未绑定会话的独立操作
    /// - `cb`: 取消触发时的回调动作
    /// - `return`: 注册凭证 ID (0 表示由于已经处于取消态而直接同步触发，无需反注册)
    RegId registerCallback(std::string_view key, CancelCallback cb) {
        if (!cb) {
            return 0;
        }
        auto entry = std::make_shared<CallbackEntry>();
        entry->cb  = std::move(cb);

        RegId id               = nextId_.fetch_add(1, std::memory_order_relaxed);
        bool  alreadyCancelled = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!key.empty() && cancelledKeys_.count(std::string(key)) > 0) {
                alreadyCancelled = true;
            } else {
                entries_[id] = entry;
                if (!key.empty()) {
                    keyToIds_[std::string(key)].push_back(id);
                    idToKey_[id] = std::string(key);
                }
            }
        }

        // 若该 key 之前已由宿主下发过取消，锁外直接同步执行回调
        if (alreadyCancelled) {
            std::lock_guard<std::mutex> elock(entry->mu);
            if (entry->cb) {
                try {
                    entry->cb();
                } catch (...) {
                }
            }
            return 0;
        }
        return id;
    }

    /// 注销回调 (命令/操作正常结束退出作用域时调用)
    /// 【排他性防 UAF 保证】: 若此时 cancel 正在另一线程执行该回调，elock 会阻塞等待
    /// 其执行完毕，彻底避免回调访问已被调用栈销毁的对象。
    void unregisterCallback(RegId id) {
        if (id == 0) {
            return;
        }
        std::shared_ptr<CallbackEntry> entry;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = entries_.find(id);
            if (it != entries_.end()) {
                entry = std::move(it->second);
                entries_.erase(it);
            }
            auto kit = idToKey_.find(id);
            if (kit != idToKey_.end()) {
                auto vkit = keyToIds_.find(kit->second);
                if (vkit != keyToIds_.end()) {
                    auto& vec = vkit->second;
                    vec.erase(std::remove(vec.begin(), vec.end(), id), vec.end());
                    if (vec.empty()) {
                        keyToIds_.erase(vkit);
                    }
                }
                idToKey_.erase(kit);
            }
        }
        if (entry) {
            std::lock_guard<std::mutex> elock(entry->mu);
            entry->disposed = true;
            entry->cb       = nullptr;
        }
    }

    /// 触发指定 key 的取消通知
    /// - 将 key 标记为已取消；提取该 key 下所有未注销的回调并于全局锁外安全执行
    void cancel(std::string_view key) {
        if (key.empty()) {
            return;
        }
        std::vector<std::shared_ptr<CallbackEntry>> toInvoke;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cancelledKeys_.insert(std::string(key));
            auto kit = keyToIds_.find(std::string(key));
            if (kit != keyToIds_.end()) {
                for (RegId id : kit->second) {
                    auto eit = entries_.find(id);
                    if (eit != entries_.end()) {
                        toInvoke.push_back(eit->second);
                        entries_.erase(eit);
                    }
                    idToKey_.erase(id);
                }
                keyToIds_.erase(kit);
            }
        }
        // 关键点: 必须在全局锁外执行回调，严防回调内部加锁发生死锁
        for (auto& entry : toInvoke) {
            std::lock_guard<std::mutex> elock(entry->mu);
            if (!entry->disposed && entry->cb) {
                try {
                    entry->cb();
                } catch (...) {
                }
                entry->cb = nullptr;
            }
        }
    }

    /// 触发所有正在运行任务的取消 (插件卸载或实例销毁时兜底调用)
    void cancelAll() {
        std::vector<std::shared_ptr<CallbackEntry>> toInvoke;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& [id, entry] : entries_) {
                toInvoke.push_back(entry);
            }
            entries_.clear();
            keyToIds_.clear();
            idToKey_.clear();
        }
        for (auto& entry : toInvoke) {
            std::lock_guard<std::mutex> elock(entry->mu);
            if (!entry->disposed && entry->cb) {
                try {
                    entry->cb();
                } catch (...) {
                }
                entry->cb = nullptr;
            }
        }
    }

    /// 查询指定 key 是否已被标记取消
    /// - 100% 内存原子/无锁或短互斥查询，严禁跨线程 post 到宿主 IO 线程
    bool isCancelled(std::string_view key) const {
        if (key.empty()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mu_);
        return cancelledKeys_.count(std::string(key)) > 0;
    }

    /// 重置指定 key 的取消标记 (新轮次开始时可选调用)
    void clearCancelled(std::string_view key) {
        if (key.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mu_);
        cancelledKeys_.erase(std::string(key));
    }

    /// 查询当前注册的活跃回调总数
    size_t activeCount() const {
        std::lock_guard<std::mutex> lock(mu_);
        return entries_.size();
    }

    /// RAII 守卫：离开作用域自动安全注销
    class [[nodiscard]] ScopedRegistration {
    public:
        ScopedRegistration() = default;
        ScopedRegistration(CancelRegistry* reg, RegId id) :
            reg_(reg), id_(id) {}
        ~ScopedRegistration() {
            if (reg_ && id_ != 0) {
                reg_->unregisterCallback(id_);
            }
        }
        ScopedRegistration(ScopedRegistration&& o) noexcept :
            reg_(o.reg_), id_(o.id_) {
            o.reg_ = nullptr;
            o.id_  = 0;
        }
        ScopedRegistration& operator=(ScopedRegistration&& o) noexcept {
            if (this != &o) {
                if (reg_ && id_ != 0) {
                    reg_->unregisterCallback(id_);
                }
                reg_   = o.reg_;
                id_    = o.id_;
                o.reg_ = nullptr;
                o.id_  = 0;
            }
            return *this;
        }
        ScopedRegistration(const ScopedRegistration&)            = delete;
        ScopedRegistration& operator=(const ScopedRegistration&) = delete;

        RegId id() const noexcept {
            return id_;
        }
        void release() noexcept {
            reg_ = nullptr;
            id_  = 0;
        }

    private:
        CancelRegistry* reg_ = nullptr;
        RegId           id_  = 0;
    };

    /// 快捷绑定接口，返回 ScopedRegistration
    ScopedRegistration bind(std::string_view key, CancelCallback cb) {
        RegId id = registerCallback(key, std::move(cb));
        return ScopedRegistration(this, id);
    }

private:
    mutable std::mutex                                        mu_;
    std::atomic<RegId>                                        nextId_{1};
    std::unordered_map<RegId, std::shared_ptr<CallbackEntry>> entries_;
    std::unordered_map<std::string, std::vector<RegId>>       keyToIds_;
    std::unordered_map<RegId, std::string>                    idToKey_;
    std::unordered_set<std::string>                           cancelledKeys_;
};

} // namespace agentxx::plugin
```

### 4.2 框架 `PluginBase` 与胶水层深度集成

#### 1. 集成于 `PluginBase`
在 `agent/lib/include/agentxx/plugin/api/plugin_kit.h` 的 `PluginBase` 中内嵌 `cancelRegistry`，并改造 `sessionCancelled`：

```cpp
class PluginBase {
public:
    const AgentxxPluginHost* host = nullptr;
    AgentIfaces              iface;
    Logger                   log;
    CancelRegistry           cancelRegistry; ///< 框架级事件驱动取消注册表 (每个实例独立一份)

    virtual ~PluginBase() {
        cancelRegistry.cancelAll();
    }

    /// 会话是否已取消 (优化版: 优先本地无抖动查询)
    /// - 宿主下发 cancel 时已通过 execute_cancel 写入 cancelRegistry
    /// - 故本地为 true 时必然已取消，直接返回 true，避免任何跨线程通信
    bool sessionCancelled(AgentxxPluginStringView tid) const {
        if (!tid.data || tid.size == 0) return false;
        std::string_view sv(tid.data, static_cast<size_t>(tid.size));
        if (cancelRegistry.isCancelled(sv)) {
            return true;
        }
        // 兜底 advisory: 仅当本地未记录且有接口表时才走宿主查询 (通常首次进入时判断)
        if (!host || !iface.cancel || !iface.cancel->is_cancelled) {
            return false;
        }
        return iface.cancel->is_cancelled(host, &tid) != 0;
    }

    bool sessionCancelled(std::string_view tid) const {
        return sessionCancelled(PluginStringView::from(tid.data(), tid.size()));
    }
    // ...
};
```

#### 2. `blocking_tool` 胶水层的标准派发
在 `blocking_tool` 注册的 `spec.execute_cancel` 中：
```cpp
spec.execute_cancel = [](void* user_data, void* op) {
    if (!op) return;
    auto* job       = static_cast<Job*>(op);
    job->cancelFlag = 1;
    // 自动通知所属实例的 CancelRegistry
    if (job->shim && job->shim->ctx) {
        job->shim->ctx->cancelRegistry.cancel(job->tid);
    }
};
```
无论插件是否显式使用 `CancelRegistry`，只要宿主触发取消，**`ctx->cancelRegistry` 必然被自动驱动置位与广播**。

---

## 5. 典型插件场景迁移落地指南

### 5.1 场景一：主动外部实体中断型 (`execute_command`, `audio_stream`)

针对必须在取消触发瞬间主动调用操作系统关闭句柄的插件：

```cpp
// 在插件执行体内
asio::awaitable<std::string> mySubprocessExecuteAsync(...) {
    auto ex          = co_await asio::this_coro::executor;
    auto cancelTimer = std::make_shared<asio::steady_timer>(ex);
    auto cancelled   = std::make_shared<std::atomic<bool>>(false);

    // 1. 使用 ScopedRegistration 绑定中断动作
    auto regGuard = c.cancelRegistry.bind(sessionKey, [&proc, ex, cancelTimer, cancelled]() {
        bool expected = false;
        if (!cancelled->compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        // 瞬间系统级斩杀子进程
        ::kill(-pid, SIGKILL);
        // 调度回本执行器关闭管道读端、唤醒等待定时器
        asio::post(ex, [&pipe, cancelTimer]() {
            pipe.close();
            cancelTimer->cancel();
        });
    });

    // 2. 协程挂起等待 max()，零定时器轮询
    cancelTimer->expires_at(std::chrono::steady_clock::time_point::max());
    co_await cancelTimer->async_wait(asio::as_tuple(asio::use_awaitable));

    // 3. regGuard 随协程生命周期结束自动 unregister，内部排他锁杜绝 UAF
}
```

### 5.2 场景二：密集计算与循环检查型 (`filesystem`, `rag_search`)

针对递归遍历目录或大批量正则匹配的插件：

```cpp
// agentxx_filesystem.cpp 改造方案:
// 原实现:
// auto isCancelled = [&c, tid]() { return c.sessionCancelled(tid); }; // 每次迭代都在同步 post 到 IO 线程!

// 改造后:
auto isCancelled = [&c, tidStr, cancel_flag]() -> bool {
    if (cancel_flag && *cancel_flag != 0) return true;
    // 纯本地内存原子判断，开销 < 5ns，零跨线程交互！
    return c.cancelRegistry.isCancelled(tidStr);
};
```
在 `grep` 处理 50,000 个文件的压力场景下，原本产生的 50,000 次 `ioCallSync` 跨线程投递直接骤降为 **0 次**。

---

## 6. 验证、基准度量与兼容性保障

### 6.1 单元测试矩阵

在框架提升过程中，需确保覆盖以下维度的测试验证：

1. **基础功能与幂等性验证**：
   - 多 Key 注册独立触发测试；
   - 同一 Key 注册多回调级联触发；
   - 重复 `cancel(key)` 幂等性；
   - 预取消状态下新注册立即触发；
   - `cancelAll()` 批量清理与解绑；
   - `ScopedRegistration` 离开作用域时反注册有效性。
2. **多线程并发竞态压力测试**：
   - 8 线程高频并发 `registerCallback`、`unregisterCallback` 与 `cancel`，验证无数据损坏、无锁竞争崩溃、无野指针访问。
3. **防 UAF 排空验证**：
   - 模拟回调函数执行期间注销，验证 `unregisterCallback` 严格等待回调执行完成后才返回并释放对象。
4. **子进程与会话隔离验证**：
   - 并发执行 Session A 与 Session B，取消 Session A 时 Session B 持续正常执行至完成。

### 6.2 性能对比预期指标

| 指标 | 改造前 (20ms 轮询 + `ioCallSync`) | 改造后 (`CancelRegistry` 框架版) |
| :--- | :--- | :--- |
| **取消响应延迟 (Cancellation Latency)** | 0 ~ 20ms（平均 10ms） | **< 50μs（微秒级）** |
| **运行期每命令 IO 线程中断频率** | **50 次/秒/命令** | **0 次/秒** |
| **并发 10 条命令时的 IO 线程上下文切换** | **500 次/秒** | **0 次/秒** |
| **密集循环检查开销 (Grep 50k 文件)** | > 350ms (纯跨线程开销) | **< 0.5ms (纯内存哈希+原子读)** |
| **多实例安全性** | 依赖业务层闭包正确性 | **框架级保证，多实例天然隔离** |

---

## 7. 实施路线图

1. **Phase 1: 框架层落地**
   - 在 `agent/lib/include/agentxx/plugin/api/plugin_kit.h` 定义 `agentxx::plugin::CancelRegistry`。
   - 在 `PluginBase` 中声明 `CancelRegistry cancelRegistry;`。
   - 增强 `PluginBase::sessionCancelled` 为本地优先读取。
   - 完善 `blocking_tool` 的 `execute_cancel` 实现，自动派发到 `ctx->cancelRegistry.cancel(tid)`。
2. **Phase 2: 插件端解耦与收敛**
   - 移除 `agent/plugins/agentxx_execute_command/execute_command_impl.h` 内局部的 `CancelRegistry` 定义，改用框架提供的 `agentxx::plugin::CancelRegistry`。
   - 优化 `agentxx_filesystem` 等其他内置插件的取消检查逻辑，消除跨线程 `ioCallSync` 隐患。
3. **Phase 3: 框架测试与基准固化**
   - 将 `CancelRegistry` 独立单元测试合入 `agent/test/plugin/test_plugins.cpp`，确保全平台持续集成覆盖。
