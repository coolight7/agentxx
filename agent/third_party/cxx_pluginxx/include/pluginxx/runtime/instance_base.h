/// pluginxx 插件实例基类与宿主视图控制块 (宿主侧, 与宿主领域无关)
///
/// 内容:
/// - [PluginInstanceBase]: 实例公共基类 (元信息/依赖/启用标志/宿主句柄/驱动登记表/
///   执行 lease), agent 侧与 client 侧实例各自继承;
/// - [PluginHostControl]: 交给插件的 `AgentxxPluginHost*` 视图所在的控制块 —— 地址
///   永不失效, 实例关闭后只清空实例引用 (tombstone), 因此插件跨卸载持有旧 host 指针
///   时各 vtable 入口只会安全失败, 既不访问已释放对象也不误指新实例;
/// - [resolvePluginHostControl]: 反查插件传入的 host 视图对应控制块;
/// - [hostMemoryAlloc] / [hostMemoryFree] / [hostMemoryCreateString] /
///   [hostMemorySetString]: C ABI 跨 CRT 堆内存操作与宿主堆字符串构造;
/// - [getExecutableDirPath]: 跨平台可执行目录 helper (builtin:// 回退探测用)。
///
/// 线程约定: 实例字段 (注册记录/依赖表) 仅 IO 线程读写; `enabled` 为普通布尔
/// (调用方遵守线程约定), 驱动登记表用独立互斥 (允许任意线程取消)。
#pragma once

#include "pluginxx/api/abi.h"
#include "pluginxx/host/capability_registry.h"
#include "pluginxx/host/event_bus.h"
#include "pluginxx/runtime/driver.h"
#include "pluginxx/runtime/runtime.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "asio/any_io_executor.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#if XX_IS_WIN_D
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

struct AgentxxPluginOperatorHandle;
struct AgentxxPluginOperationCompletionEndpoint;

namespace pluginxx {

class PluginHostControl;
// =====================================================================
// 实例公共基类
// =====================================================================

/// 插件实例公共基类 (agent 侧 PluginInstance / client 侧 ClientPluginInstance 继承)
/// - 持有跨端一致的元信息/依赖/启用标志/执行中计数/宿主句柄
/// - InflightGuard 为公共 RAII (事件 handler / 命令 execute / 异步 op 入口计数)
struct PluginInstanceBase {
    std::string name;        ///< 唯一标识 (与对端插件共用命名空间)
    std::string version;     ///< 版本号 (get_info 或默认)
    std::string description; ///< 描述
    std::string path;        ///< 加载的库路径/内置路径
    /// 插件配置参数 (yaml `plugins` 条目 args; 宿主原样保存, 经 vtable
    /// get_plugin_args 整体返回给插件, 不解析其字段语义)
    utilxx_base::Json args = utilxx_base::Json::object();
    /// 插件配置文件所在目录或文件路径 (yaml `config`, 归一化为绝对路径)
    std::string              configPath;
    std::vector<std::string> depends; ///< 必选依赖 (未安装加载失败; 卸载/禁用级联)
    std::vector<std::string> optionalDepends;     ///< 可选依赖 (未安装仅警告)
    void*                    dlHandle  = nullptr; ///< dlopen/LoadLibrary 句柄
    void*                    pluginCtx = nullptr; ///< entry 输出的插件私有上下文
    bool                     enabled   = true; ///< 是否启用 (禁用: 注册摘除/命令停用)
    bool userDisabled          = false; ///< 是否被用户显式禁用 (区别于级联禁用)
    bool blockedByDependencies = false; ///< 是否因必选依赖不可用而级联禁用
    bool unloadRequested       = false; ///< 已请求卸载 (防重复)
    /// create 是否成功产出可销毁的 pluginCtx。
    bool pluginCreated = false;
    /// 插件上下文是否已经调用 destroy。只在所属 IO 线程更新。
    bool pluginDestroyed = false;
    /// 同步关闭发现活动 lease 时，等待最后一个 lease 释放后再执行 destroy。
    bool destroyDeferred = false;
    /// 实例生命周期入口 (加载成功的插件必有这两个符号, 见 plugin_api.h):
    /// - start: 注册事务 (工具/钩子/能力/订阅/自管线程), 在宿主 IO 线程执行;
    /// - stop: 撤销自管资源, destroy 之前必须先完成。
    AgentxxPluginStartFn lifecycleStart = nullptr;
    AgentxxPluginStopFn  lifecycleStop  = nullptr;
    /// start 事务是否已成功完成 (加载成功即置位)。
    bool lifecycleStarted = false;
    /// stop 事务是否已执行完成 (destroy 的前提)。
    bool lifecycleStopped = false;

    /// stop 事务仍未执行: 同步关闭路径无法等待该事务，因此必须保留实例、
    /// 上下文与动态库，交由仍运行的异步 owner (unloadAsync/shutdownAsync) 收尾。
    /// - 加载成功的实例 start/stop 都在, `lifecycleStarted` 即"stop 欠着"的判据;
    /// - start 失败/未 start 的实例无需 stop (宿主回滚已声明注册), 可直接 destroy。
    bool lifecycleStopPending() const noexcept {
        return lifecycleStop != nullptr && lifecycleStarted && !lifecycleStopped;
    }

    std::vector<std::shared_ptr<::AgentxxPluginOperatorHandle>>              operatorHandles;
    std::vector<std::shared_ptr<::AgentxxPluginOperationCompletionEndpoint>> completionEndpoints;
    std::vector<std::shared_ptr<::AgentxxPluginOperatorHandle>>              outstandingOps;

    /// ==================== 通用表相关登记 (仅宿主 IO 线程读写) ====================
    ///
    /// 这些记录由通用表 (事件 / 调度 / 能力) 的实现维护, 与宿主领域无关, 因此直接
    /// 放在基类: 宿主核心 (见 `pluginxx/host/host_core.h`) 只依赖基类即可完成
    /// 撤销与清理, 新增宿主无需重复实现同一套登记。

    /// 活跃事件订阅 (句柄本体由 [subscriptionHandles] 保活; 撤销时从此表移除)
    std::vector<std::shared_ptr<::AgentxxPluginSubscription>> subscriptions;
    /// 事件订阅句柄保活表: 插件持有的裸指针在实例析构前始终有效
    std::vector<std::shared_ptr<::AgentxxPluginSubscription>> subscriptionHandles;
    /// 活跃 sleep 使用 Operation 句柄索引；完成回调开始前移除，取消查询 O(1)。
    std::unordered_map<void*, std::shared_ptr<::AgentxxPluginOperatorHandle>> sleepTimers;
    /// 已声明能力 (随工具注销/实例禁用卸载一并撤销)
    std::vector<PluginCapabilityRegistration> capabilityRegistrations;

    /// 驱动请求登记表 (`agentxx.agent.coroutine_runtime` 的 ticket 句柄)。
    ///
    /// 为什么需要这张表:
    /// - 插件桥接持有宿主发放的**裸指针**句柄, 它的有效性必须由宿主兜底: 宿主
    ///   只把指针当**查表键**使用, 命中才解引用 (`shared_ptr` 保活), 因此即使
    ///   插件违约传入已收束/无效的句柄, 宿主也只是安全地忽略, 不会解引用悬垂内存;
    /// - 关闭超时需要"最后防线": 插件自身没能撤销的排队请求会一直持有实例 lease,
    ///   必须由宿主撤销 (见 [cancelPendingDrivers])。
    ///
    /// 内存有界: 只保留**未收束**请求 + 最近 [kFinishedDriverRetention] 张已收束
    /// 请求 (墓碑)。墓碑窗口保证"刚收束就取消"这类迟到 cancel 能按地址命中并
    /// 观察终态, 而不会命中"地址刚被回收给新请求"的旧句柄; 每次登记新请求时清理
    /// 超出窗口的墓碑, 因此长期运行不会无限增长。
    ///
    /// 线程: `request_driver`/`cancel_driver` 允许任意线程调用 (与其它注册表只由
    /// IO 线程访问不同), 因此本表用独立互斥保护。
    static constexpr size_t kFinishedDriverRetention = 16;

    mutable std::mutex                                 driversMutex;
    std::deque<std::shared_ptr<::AgentxxPluginDriver>> drivers;

    /// 登记请求句柄 (任意线程; 由 request_driver 在排队前调用)。
    void retainDriverHandle(const std::shared_ptr<::AgentxxPluginDriver>& driver) {
        if (!driver) {
            return;
        }
        std::lock_guard lock(driversMutex);
        pruneFinishedDriversLocked();
        drivers.push_back(driver);
    }

    /// 按地址取消一次请求 (**任意线程可调用; 幂等**)。
    /// - `return`: true = 命中了登记表 (无论请求是否已收束, 都会调一次 cancel())
    /// - 未命中表示该句柄不属于本实例的有效窗口 (已收束且墓碑已过期, 或无效句柄):
    ///   安全忽略并记日志, 绝不解引用。
    bool cancelDriver(const ::AgentxxPluginDriver* driver) noexcept {
        if (!driver) {
            return false;
        }
        std::shared_ptr<::AgentxxPluginDriver> target;
        {
            std::lock_guard lock(driversMutex);
            for (const auto& entry : drivers) {
                if (entry.get() == driver) {
                    target = entry;
                    break;
                }
            }
        }
        if (!target) {
            XX_LOGW("Late plugin driver cancellation ignored (handle is not registered)");
            return false;
        }
        // 在锁外调用: cancel 可能触发请求收束后的收尾, 不能持表锁进入。
        target->cancel();
        return true;
    }

    /// 取消该实例全部**尚未开始**的请求 (关闭超时/最终收尾的安全网)。
    ///
    /// 正常关闭**不依赖**这里: 插件桥接在实例上下文销毁时自行 `cancel_driver`,
    /// 且 root 的取消收束依赖驱动继续流动 (见 plugin_driver.h 文件头)。宿主只在
    /// 已判定实例关闭失败 (关闭超时、lease 未归零) 时调用它, 避免 lease 永久残留。
    ///
    /// - `return`: 本次实际取消的请求数量 (0 表示没有排队中的请求)
    size_t cancelPendingDrivers() noexcept {
        std::vector<std::shared_ptr<::AgentxxPluginDriver>> pending;
        {
            std::lock_guard lock(driversMutex);
            pending.reserve(drivers.size());
            for (const auto& driver : drivers) {
                if (driver && !driver->finished() && !driver->running()) {
                    pending.push_back(driver);
                }
            }
        }
        for (const auto& driver : pending) {
            driver->cancel();
        }
        return pending.size();
    }

    /// 尚未收束 (排队或执行中) 的请求数量, 供诊断与测试观察。
    size_t activeDriverCount() const noexcept {
        std::lock_guard lock(driversMutex);
        size_t          count = 0;
        for (const auto& driver : drivers) {
            if (driver && !driver->finished()) {
                ++count;
            }
        }
        return count;
    }

private:

    /// 从最旧一端清理已收束请求, 只保留最近 [kFinishedDriverRetention] 张作为墓碑。
    /// 调用方须持有 [driversMutex]。
    void pruneFinishedDriversLocked() {
        size_t finished = 0;
        for (const auto& driver : drivers) {
            if (!driver || driver->finished()) {
                ++finished;
            }
        }
        while (finished > kFinishedDriverRetention && !drivers.empty()) {
            const auto& oldest = drivers.front();
            if (oldest && !oldest->finished()) {
                // 未收束请求仍在排队/执行: 不能丢弃 (它持有实例 lease), 也不能
                // 越过它去回收后面的墓碑 (保持时间顺序, 避免误回收窗口内的句柄)。
                return;
            }
            drivers.pop_front();
            --finished;
        }
    }

public:

    /// 由实例创建路径设置，供只拿到裸指针的宿主回调升级 owner。
    std::weak_ptr<PluginInstanceBase> ownerSelf;

    /// 取实例自有的强引用
    /// - 供只拿到基类指针的宿主代码 (Operation 驱动器 / 完成回调) 升级为派生类型
    /// - `return` 空表示实例已析构，或创建路径未设置自引用
    template<typename InstanceT>
    std::shared_ptr<InstanceT> sharedSelf() const noexcept {
        if (auto base = ownerSelf.lock()) {
            return std::static_pointer_cast<InstanceT>(base);
        }
        return nullptr;
    }

    /// 宿主生命周期控制块 (状态机 + 执行 lease)。实例对象本身只保存业务注册信息；
    /// 所有跨线程执行都通过 lease 保证 stop/destroy/dlclose 前已经返回。
    std::shared_ptr<pluginxx::InstanceLifetime> lifetime;

    /// 实例所属宿主运行时 (io executor / Operation 表 / 投递通道)。
    ///
    /// 由管理器在装配 [lifetime] 时一并写入 (见 [PluginManagerBase::makeLifetime]);
    /// 弱引用是因为运行时由管理器持有且比实例长命, 实例不应延长其生命周期。
    /// Operation 驱动器与驱动请求据此拿到 io executor 与线程标识, 无需回查管理器
    /// 具体类型 (框架内核因此不依赖宿主的管理器类型)。
    std::weak_ptr<PluginRuntime> runtime;

    /// 宿主控制块：交给插件的 `AgentxxPluginHost` 视图保存在控制块内（进程级
    /// 稳定地址），插件在实例卸载后继续使用旧 host 指针时只会安全失败。
    /// 见 [PluginHostControl]。
    std::shared_ptr<PluginHostControl> hostControl;

    explicit PluginInstanceBase(std::string in_name) :
        name(std::move(in_name)) {}

    virtual ~PluginInstanceBase() = default;

    PluginInstanceBase(const PluginInstanceBase&)            = delete;
    PluginInstanceBase& operator=(const PluginInstanceBase&) = delete;

    /// 执行 lease RAII: 把一段可能进入插件代码的执行登记到实例生命周期,
    /// 卸载路径的 `waitIdleUntil` 因此必然覆盖它, dlclose 不会越过仍在运行的插件代码。
    /// - `allowClosing=false` (默认): "开始新动作", 实例进入 Closing/Disabled 后获取失败;
    /// - `allowClosing=true`: 只读查询 / 取消 / 完成清理, 关闭过程中仍需执行。
    ///
    /// 未装配 lifetime 的实例 (单元测试直接构造的伪实例) 视为无租约约束。
    struct InflightGuard {
        PluginInstanceBase*                 inst = nullptr;
        std::shared_ptr<PluginInstanceBase> owner;
        pluginxx::InstanceLease                       lease;

        explicit InflightGuard(std::shared_ptr<PluginInstanceBase> i, bool allowClosing = false) :
            inst(i.get()),
            owner(std::move(i)),
            lease(inst ? pluginxx::InstanceLease::acquire(inst->lifetime, allowClosing) : pluginxx::InstanceLease{}) {}

        explicit InflightGuard(PluginInstanceBase* i, bool allowClosing = false) :
            inst(i),
            owner(i ? i->ownerSelf.lock() : nullptr),
            lease(i ? pluginxx::InstanceLease::acquire(i->lifetime, allowClosing) : pluginxx::InstanceLease{}) {}

        explicit operator bool() const noexcept {
            return inst == nullptr || inst->lifetime == nullptr || static_cast<bool>(lease);
        }

        ~InflightGuard() = default;
    };

    /// 交给插件的宿主视图（控制块内地址，永不失效）；未装配控制块返回 nullptr。
    /// 插件保存该指针跨卸载继续调用时，各 vtable 入口会安全失败。
    const AgentxxPluginHost* hostView() const noexcept;

    /// 插件上下文销毁后调用：旧 host 指针之后按“实例不存在”安全失败。
    void retireHostControl() noexcept;
};

// =====================================================================
// 宿主控制块 (交给插件的 host 视图)
// =====================================================================

/// 宿主控制块：插件持有的 `const AgentxxPluginHost*` 必须指向进程级稳定地址。
///
/// 背景：插件在 create 时收到 host 指针，可能把它保存在实例字段、工作线程或
/// 延迟任务里；实例卸载（destroy + dlclose）之后插件仍可能调用宿主 vtable。
/// 若 host 视图位于 PluginInstance 对象内部，这类迟到调用就是 use-after-free。
///
/// 解决方式：
/// - 每个实例创建一块**永不释放**的控制块，host 视图放在其中，因此插件保存的
///   地址始终有效；实例关闭时只清空实例引用（tombstone）。
/// - `host.opaque` 是控制块地址，作为一次性令牌（地址永不复用），经进程级
///   注册表解析；已关闭实例的旧令牌解析成功但实例为空，所有入口安全失败，
///   既不会访问已释放对象，也不会把调用转交给后来加载的同名实例。
///
/// 控制块数量等于进程内累计加载过的插件实例数（每块约 100 字节）；这是保证
/// “旧 host 指针安全失败且绝不指向新实例”所付出的固定代价。
namespace detail {

/// 进程级控制块注册表。这里保存强引用的 tombstone 集合，不做回收：
/// 控制块必须比插件的引用更长命，地址才可能永不复用。
struct PluginHostControlRegistry {
    std::mutex                                                       mutex;
    std::map<void*, std::shared_ptr<PluginHostControl>, std::less<>> controls;
};

inline PluginHostControlRegistry& pluginHostControlRegistry() {
    static PluginHostControlRegistry registry;
    return registry;
}

} // namespace detail

class PluginHostControl {
public:

    /// 创建并注册控制块。`vtable` 为本端宿主静态函数表（agent/client 各自一份）。
    static std::shared_ptr<PluginHostControl> create(
        const std::shared_ptr<PluginInstanceBase>& instance,
        const AgentxxHostVtable*                   vtable
    ) {
        std::shared_ptr<PluginHostControl> control(new PluginHostControl(instance, vtable));
        registerControl(control);
        return control;
    }

    /// 交给插件的 host 视图地址（控制块内，永不失效）。
    const AgentxxPluginHost* host() const noexcept {
        return &host_;
    }

    /// 一次性令牌（= 控制块地址，不复用）。
    void* token() const noexcept {
        return host_.opaque;
    }

    uint64_t generation() const noexcept {
        return generation_;
    }

    /// 实例仍在时返回强引用；已关闭/已释放返回空。
    std::shared_ptr<PluginInstanceBase> instance() const noexcept {
        return instance_.lock();
    }

    /// 实例关闭后调用：之后所有 vtable 入口按“实例不存在”安全失败。
    void retire() noexcept {
        retired_.store(true, std::memory_order_release);
        instance_.reset();
    }

    bool retired() const noexcept {
        return retired_.load(std::memory_order_acquire);
    }

private:

    PluginHostControl(
        const std::shared_ptr<PluginInstanceBase>& instance,
        const AgentxxHostVtable*                   vtable
    ) :
        instance_(instance),
        generation_(instance ? instance->lifetime ? instance->lifetime->generation() : 0 : 0) {
        host_.vtable = vtable;
        // 令牌即控制块地址：永不释放 => 永不复用，不会与后续实例混淆。
        host_.opaque = const_cast<PluginHostControl*>(this);
    }

    PluginHostControl(const PluginHostControl&)            = delete;
    PluginHostControl& operator=(const PluginHostControl&) = delete;

    /// 进程级注册表：保存控制块强引用的 tombstone 集合。
    /// 控制块必须比插件的引用更长命，因此这里不做回收。
    static void registerControl(const std::shared_ptr<PluginHostControl>& control) {
        if (!control) {
            return;
        }
        auto&           registry = detail::pluginHostControlRegistry();
        std::lock_guard lock(registry.mutex);
        registry.controls.emplace(control->token(), control);
    }

    AgentxxPluginHost                 host_{};
    std::weak_ptr<PluginInstanceBase> instance_;
    uint64_t                          generation_ = 0;
    std::atomic<bool>                 retired_{false};
};

inline const AgentxxPluginHost* PluginInstanceBase::hostView() const noexcept {
    return hostControl ? hostControl->host() : nullptr;
}

inline void PluginInstanceBase::retireHostControl() noexcept {
    if (hostControl) {
        hostControl->retire();
    }
}

namespace detail {

/// 事件订阅撤销的簿记动作 (见 pluginxx/host/event_bus.h 的声明)
/// - 调用方已把句柄的 `alive` 置 false, 这里只做后端退订与实例订阅表清理;
/// - 幂等: `subscriptionId` 置 0 后重复调用是空操作。
inline void revokeSubscription(AgentxxPluginSubscription* sub) noexcept {
    if (!sub) {
        return;
    }
    try {
        if (sub->source && sub->subscriptionId != 0) {
            sub->source->unsubscribe(sub->topic, sub->subscriptionId);
            sub->subscriptionId = 0;
        }
    } catch (const std::exception& e) {
        XX_LOGW("Plugin subscription revoke failed: {}", e.what());
    } catch (...) {
        XX_LOGW("Plugin subscription revoke failed: unknown exception");
    }
    try {
        if (auto inst = sub->inst.lock()) {
            auto& subs = inst->subscriptions;
            subs.erase(
                std::remove_if(
                    subs.begin(),
                    subs.end(),
                    [sub](const std::shared_ptr<AgentxxPluginSubscription>& entry) {
                        return entry.get() == sub;
                    }
                ),
                subs.end()
            );
        }
        sub->inst.reset();
    } catch (...) {
        XX_LOGW("Plugin subscription bookkeeping failed: unknown exception");
    }
}

} // namespace detail

/// 解析插件传入的 host 视图对应的控制块。
/// - 未注册的令牌（含插件复制的 host 结构被篡改、旧内存被复用后的垃圾值）返回空；
/// - 已关闭实例返回控制块本身，调用方据此区分“实例不存在”与“参数非法”。
inline std::shared_ptr<PluginHostControl> resolvePluginHostControl(const AgentxxPluginHost* host
) noexcept {
    if (!host || !host->opaque) {
        return nullptr;
    }
    try {
        auto&           registry = detail::pluginHostControlRegistry();
        std::lock_guard lock(registry.mutex);
        auto            it = registry.controls.find(host->opaque);
        return it == registry.controls.end() ? nullptr : it->second;
    } catch (...) {
        return nullptr;
    }
}

// =====================================================================
// C ABI 内存操作 + 宿主堆字符串构造 (跨 CRT 堆边界; 两侧 vtable 共用)
// =====================================================================

inline void* hostMemoryAlloc(uint64_t size) {
    return ::malloc(static_cast<size_t>(size));
}

inline void hostMemoryFree(void* ptr) {
    ::free(ptr);
}

inline AgentxxPluginString hostMemoryCreateString(AgentxxPluginStringView s) {
    AgentxxPluginString res{nullptr, 0};
    if (!s.data && s.size == 0) {
        return res;
    }
    char* p = static_cast<char*>(hostMemoryAlloc(s.size + 1));
    if (p) {
        if (s.size > 0 && s.data) {
            std::memcpy(p, s.data, static_cast<size_t>(s.size));
        }
        p[s.size] = '\0';
        res.data  = p;
        res.size  = s.size;
    }
    return res;
}

inline AgentxxPluginString hostMemoryCreateString(std::string_view sv) {
    return hostMemoryCreateString(AgentxxPluginStringView{sv.data(), static_cast<uint64_t>(sv.size())});
}

inline void hostMemorySetString(AgentxxPluginString* out, std::string_view sv) {
    if (!out) {
        return;
    }
    *out = hostMemoryCreateString(sv);
}

inline AgentxxPluginString hostMemoryCreateString(const char* s) {
    if (!s) {
        return AgentxxPluginString{nullptr, 0};
    }
    return hostMemoryCreateString(
        AgentxxPluginStringView{s, static_cast<uint64_t>(std::strlen(s))}
    );
}

// =====================================================================
// 可执行目录 helper (跨平台: Windows GetModuleFileNameW / Linux /proc/self/exe)
// 供 builtin:// 回退探测使用 (agent/client 两侧共用, 原两份实现合并)
// =====================================================================

inline std::filesystem::path getExecutableDirPath() noexcept {
#if XX_IS_WIN_D
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return {};
        }
        if (len < buf.size()) {
            buf.resize(len);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return std::filesystem::path(buf).parent_path();
#else
    std::error_code ec;
    auto            exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return {};
    }
    return exe.parent_path();
#endif
}

} // namespace pluginxx
