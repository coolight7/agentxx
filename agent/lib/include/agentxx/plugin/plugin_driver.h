/// 宿主侧协程驱动请求 (agentxx.agent.coroutine_runtime 接口表的实现载体)
///
/// 背景: 插件协程要与宿主协程在同一宿主 IO 执行序列中交错推进, 但插件可以用任意
/// 协程库/事件循环, 宿主也绝不能把插件私有 reactor 接进自己的执行序列。因此两端
/// 只经两类动作协作 (见 docs/zh-cn/design/plugins.md):
/// - **driver/pump**: 插件申请宿主异步执行一次有界回调 (`AgentxxPluginDriveOnceFn`);
/// - **wake 合并**: 插件侧适配器自行合并重复唤醒, 再申请下一次 ticket。
///
/// 本文件是"driver"这一侧的宿主实现:
/// - 一次 ticket 至多执行一次回调, 且**永不内联** (即使申请者就在 IO 线程, 也经
///   `enqueueRuntimeAction` 异步投递), 避免 root start/completion/cancel 重入;
/// - 回调排队与执行期间持有实例执行 lease, 因此实例关闭/卸载的 idle 等待必然
///   覆盖它, `dlclose` 不会越过仍在执行的插件代码;
/// - ticket 的终态 (已执行 / 已取消) 只能成功一次, 由 CAS 仲裁, 保证"取消之后
///   不再执行回调"与"lease 恰好释放一次";
/// - `cancel_driver` 的 ABI 形态不含 host 参数, 因此句柄校验走进程级地址注册表:
///   伪造/过期指针只会被安全忽略 (不解引用)。
///
/// 实例 lease 采用 `lifecycle` 模式: 实例进入 `Closing` 后仍需允许驱动, 因为关闭
/// 的第一步就是取消全部 Operation, 而插件的取消收束 (取消回调 → 唤醒 → 下一个
/// 有限步骤) 必须能继续跑完, 否则关闭必然超时。禁止新工作由 `acceptsOperations`
/// 与各注册入口把关 (不在这里), 本文件只保证 ticket 的排队/执行/取消语义。
///
/// 命名空间: 请求类型本身位于**全局命名空间** (与 `AgentxxPluginOperatorHandle`
/// 一致), 因为它是 C ABI 的不透明句柄; 它在内部使用 `agentxx::plugin` 的运行时类型。
#pragma once

#include "agentxx/plugin/plugin_runtime.h"
#include "agentxx/util/log.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

/// 驱动请求: 一次票 = 一次有界回调 (`AgentxxPluginDriveOnceFn`)。
///
/// 句柄与生命周期:
/// - 裸指针句柄在"已排队 / 正在执行"期间有效 (由实例的请求表与投递闭包持有), 插件
///   只能在该窗口内 `cancel_driver`; 实例侧另保留最近 16 张已收束请求作为墓碑窗口,
///   吸收"刚收束就取消"的迟到调用;
/// - 句柄校验由进程级地址注册表兜底 ([cancelByHandle]): 过期/伪造指针只被忽略;
/// - 出队执行 (`runOnIo`) 或取消 (`cancel`) 后释放实例 lease 并摘除登记。
struct AgentxxPluginDriver : std::enable_shared_from_this<AgentxxPluginDriver> {
    /// 状态机的唯一所有权令牌: Idle 由"执行"与"取消"两方 CAS 竞争, 胜者负责
    /// 收束 (释放 lease)。
    enum class State : uint32_t {
        Idle     = 0, ///< 已排队, 尚未执行
        Running  = 1, ///< 回调正在执行 (不会被取消强行中断)
        Finished = 2, ///< 已执行完成或已取消, lease 已释放
    };

    /// 创建请求 (不自动排队; 调用方负责 `schedule()`)。
    ///
    /// - `runtime`: 宿主运行时 (提供 IO executor 与投递通道)
    /// - `lifetime`: 实例生命周期控制块; 空指针或实例已 Closed 时创建失败
    /// - `drive`: 插件回调 (C ABI 函数指针), `ud` 为其 user_data
    /// - `return`: 失败返回 nullptr (实例已关闭 / 参数缺失)
    static std::shared_ptr<AgentxxPluginDriver> create(
        std::shared_ptr<agentxx::plugin::PluginRuntime>    runtime,
        std::shared_ptr<agentxx::plugin::InstanceLifetime> lifetime,
        AgentxxPluginDriveOnceFn                           drive,
        void*                                              ud,
        const std::string&                                 label
    ) {
        if (!runtime || !lifetime || !drive) {
            return nullptr;
        }
        // lifecycle lease: Closing 期间仍允许驱动 (见文件头说明), Closed 拒绝。
        auto lease = agentxx::plugin::InstanceLease::acquire(lifetime, /*lifecycle=*/true);
        if (!lease) {
            return nullptr;
        }
        auto driver       = std::shared_ptr<AgentxxPluginDriver>(new AgentxxPluginDriver());
        driver->runtime_  = std::move(runtime);
        driver->drive_    = drive;
        driver->userData_ = ud;
        driver->label_    = label;
        driver->lease_    = std::move(lease);
        registerHandle(driver);
        return driver;
    }

    /// 按地址校验并取消请求 (**任意线程可调用**; 无效/已收束句柄安全忽略)。
    ///
    /// 为什么不能直接解引用: `cancel_driver` 的 ABI 形态不含 host 参数, 宿主无法
    /// 从实例反查合法句柄, 因此用一个**进程级地址注册表**兜底 —— 先按地址查表,
    /// 命中才经 weak_ptr 升级为强引用并调用 cancel; 未命中 (伪造/过期指针) 只记录
    /// 日志, 绝不解引用。表内只存 weak_ptr 且请求收束时按地址摘除, 因此内存有界,
    /// 也不存在"表项持有最后一个强引用导致自毁"的风险。
    static bool cancelByHandle(AgentxxPluginDriver* driver) noexcept {
        if (!driver) {
            return false;
        }
        std::shared_ptr<AgentxxPluginDriver> ticket;
        {
            auto&           registry = handleRegistry();
            std::lock_guard lock(registry.mutex);
            auto            it = registry.tickets.find(driver);
            if (it == registry.tickets.end()) {
                return false;
            }
            ticket = it->second.lock();
            if (!ticket) {
                registry.tickets.erase(it);
                return false;
            }
        }
        ticket->cancel();
        return true;
    }

    AgentxxPluginDriver(const AgentxxPluginDriver&)            = delete;
    AgentxxPluginDriver& operator=(const AgentxxPluginDriver&) = delete;

    State state() const noexcept {
        return static_cast<State>(state_.load(std::memory_order_acquire));
    }

    bool finished() const noexcept {
        return state() == State::Finished;
    }

    bool running() const noexcept {
        return state() == State::Running;
    }

    const std::string& label() const noexcept {
        return label_;
    }

    /// 异步排队本次驱动 (**任意线程可调用**; 恒异步, 永不内联)。
    ///
    /// - `return`: true = 已成功入队; false = 入队失败 (runtime 无 executor 或已
    ///   停止), 此时请求已经收束 (lease 已释放), 调用方必须按申请失败处理: 把
    ///   受影响的操作以失败/取消终结, 不得静默丢弃。
    bool schedule() noexcept {
        if (!runtime_) {
            return false;
        }
        auto       self   = shared_from_this();
        const bool queued = agentxx::plugin::enqueueRuntimeAction(
            runtime_,
            [self] {
                self->runOnIo();
            },
            /*retainWhenStopped=*/false
        );
        if (!queued) {
            XX_LOGW(
                "Plugin driver `{}` could not be queued: runtime IO executor unavailable",
                label_
            );
            // 入队失败等价于"宿主不再提供驱动": 立刻收束请求 (释放 lease)。
            uint32_t expected = kIdle;
            if (state_.compare_exchange_strong(
                    expected,
                    static_cast<uint32_t>(State::Finished),
                    std::memory_order_acq_rel
                )) {
                release();
            }
        }
        return queued;
    }

    /// 取消尚未执行的请求 (**幂等、非阻塞**; 任意线程可调用)。
    /// - 尚未执行: 之后不再执行回调, 立刻释放 lease;
    /// - 正在执行: 不强行中断 (调用方需靠 root 收束协议收尾), 回调返回后收束;
    /// - 已结束: 空操作。
    void cancel() noexcept {
        uint32_t expected = kIdle;
        if (state_.compare_exchange_strong(
                expected,
                static_cast<uint32_t>(State::Finished),
                std::memory_order_acq_rel
            )) {
            release();
        }
    }

private:

    AgentxxPluginDriver() = default;

    /// 请求地址注册表 (仅宿主内部; 与 PluginHostControl 的进程级注册表同思路)。
    /// 只做"地址校验 + 生命周期升级", 不携带任何跨实例业务状态, 因此不违反多实例约定。
    struct HandleRegistry {
        std::mutex                                                               mutex;
        std::map<const AgentxxPluginDriver*, std::weak_ptr<AgentxxPluginDriver>> tickets;
    };

    static HandleRegistry& handleRegistry() noexcept {
        static HandleRegistry registry;
        return registry;
    }

    static void registerHandle(const std::shared_ptr<AgentxxPluginDriver>& driver) noexcept {
        if (!driver) {
            return;
        }
        try {
            auto&           registry = handleRegistry();
            std::lock_guard lock(registry.mutex);
            registry.tickets.emplace(driver.get(), driver);
        } catch (...) {
            // 登记失败只影响"伪造句柄的安全忽略", 不影响驱动本身。
        }
    }

    static void unregisterHandle(const AgentxxPluginDriver* driver) noexcept {
        if (!driver) {
            return;
        }
        try {
            auto&           registry = handleRegistry();
            std::lock_guard lock(registry.mutex);
            registry.tickets.erase(driver);
        } catch (...) {
        }
    }

    /// 收束 (释放 lease + 摘除句柄登记); 只由胜出的状态迁移调用一次。
    void release() noexcept {
        lease_.reset();
        unregisterHandle(this);
    }

    static constexpr uint32_t kIdle = static_cast<uint32_t>(State::Idle);

    /// 唯一执行入口 (IO 线程): CAS Idle→Running 抢到所有权才调用插件回调。
    void runOnIo() noexcept {
        uint32_t expected = kIdle;
        if (!state_.compare_exchange_strong(
                expected,
                static_cast<uint32_t>(State::Running),
                std::memory_order_acq_rel
            )) {
            // 已被取消 (Finished): 宿主承诺不再执行回调。
            return;
        }
        try {
            drive_(userData_);
        } catch (const std::exception& e) {
            // 跨 C ABI 抛异常是插件违约; 宿主兜底记账, 不让异常逃出 IO 线程。
            XX_LOGE("Plugin driver `{}` callback threw: {}", label_, e.what());
        } catch (...) {
            XX_LOGE("Plugin driver `{}` callback threw unknown exception", label_);
        }
        uint32_t running = static_cast<uint32_t>(State::Running);
        if (state_.compare_exchange_strong(
                running,
                static_cast<uint32_t>(State::Finished),
                std::memory_order_acq_rel
            )) {
            release();
        }
    }

    std::shared_ptr<agentxx::plugin::PluginRuntime> runtime_;
    AgentxxPluginDriveOnceFn                        drive_    = nullptr;
    void*                                           userData_ = nullptr;
    std::string                                     label_;
    std::atomic<uint32_t>                           state_{kIdle};
    /// 实例执行 lease: 覆盖排队与执行, 关闭等待 (waitInflightZero) 必然包含它。
    agentxx::plugin::InstanceLease lease_;
};
