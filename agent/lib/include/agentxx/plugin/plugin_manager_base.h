/// 插件管理器公共基类 (宿主侧, agent/client 共用)
///
/// 背景: agent 侧 PluginManager
/// ([plugin_manager_lifecycle.cpp](/agent/lib/src/plugins/plugin_manager_lifecycle.cpp) 等)
/// 与 client 侧
/// [ClientPluginManager](/agent/lib/src/plugins/client_plugin_manager.cpp)
/// 存在大量重复基建:
/// - 实例公共字段 (元信息/依赖/启用标志/inflight/host 句柄)
/// - io 线程投递 (isIoThread/postToIo/postToIoAsync + ioThreadId_)
/// - 级联卸载/禁用骨架 (collectReverseRequiredDeps + waitInflightZero)
/// - 可执行目录 helper (跨平台 GetModuleFileNameW /proc/self/exe)
/// - C ABI 内存三件套 (alloc/free/strdup)
/// 提取到本基类避免两侧行为漂移
///
/// 结构:
/// - PluginInstanceBase: 实例公共基类 (两侧 PluginInstance/ClientPluginInstance
///   继承; 持有元信息/标志/inflight/宿主句柄/InflightGuard)
/// - PluginManagerBase<InstanceT>: 管理器公共基类 (CRTP/模板注入实例类型;
///   持有 io executor/ioThreadId_/插件表, 提供 io 投递/查找/等待/级联收集)
/// - hostMemoryAlloc/hostMemoryFree: C ABI 跨 CRT 堆内存操作
///   (两侧 vtable 共用同一实现)
/// - hostMemoryCreateString/hostMemorySetString: 经上述内存操作构造宿主堆字符串
/// - getExecutableDirPath: 跨平台可执行目录 helper (builtin:// 回退探测用)
///
/// 线程约定: 与两侧一致 —— 注册表/插件表仅 io 线程读写; 本类不引入锁
/// (ioThreadId_ 为原子, inflight 为原子, 跨线程递增/递减)。
#pragma once

#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/plugin_common.h"
#include "agentxx/plugin/plugin_driver.h"
#include "agentxx/plugin/plugin_runtime.h"
#include "agentxx/util/json.h"
#include "agentxx/util/log.h"
#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "asio/post.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
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

namespace agentxx {
namespace plugin {

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
    agentxx::util::Json args = agentxx::util::Json::object();
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

    /// 宿主生命周期控制块 (状态机 + 执行 lease)。实例对象本身只保存业务注册信息；
    /// 所有跨线程执行都通过 lease 保证 stop/destroy/dlclose 前已经返回。
    std::shared_ptr<InstanceLifetime> lifetime;

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
        InstanceLease                       lease;

        explicit InflightGuard(std::shared_ptr<PluginInstanceBase> i, bool allowClosing = false) :
            inst(i.get()),
            owner(std::move(i)),
            lease(inst ? InstanceLease::acquire(inst->lifetime, allowClosing) : InstanceLease{}) {}

        explicit InflightGuard(PluginInstanceBase* i, bool allowClosing = false) :
            inst(i),
            owner(i ? i->ownerSelf.lock() : nullptr),
            lease(i ? InstanceLease::acquire(i->lifetime, allowClosing) : InstanceLease{}) {}

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
// vtable 入口公共上下文
// =====================================================================

/// vtable 入口的公共上下文：解析宿主控制块，并持有实例/管理器强引用与
/// admission lease。
///
/// - `ok()` 为 false 时入口必须安全失败（返回非 0 / NULL + error）：
///   实例已卸载、已关闭、正在关闭（`allowClosing=false`）或参数不是本宿主
///   发放的 host 视图。
/// - `guard` 是 admission lease。投递到 IO 线程的闭包按值捕获本对象即可让
///   卸载的 idle 等待覆盖“已排队但尚未执行”的阶段，避免 dlclose 越过闭包。
/// - 本对象可拷贝（只含 shared_ptr），因此能放进 `std::function` 闭包。
template<typename InstanceT, typename ManagerT>
struct PluginHostCall {
    std::shared_ptr<InstanceT>                         inst;
    std::shared_ptr<ManagerT>                          mgr;
    std::shared_ptr<PluginInstanceBase::InflightGuard> guard;

    bool ok() const noexcept {
        return inst && mgr && guard && static_cast<bool>(*guard);
    }

    InstanceT* instance() const noexcept {
        return inst.get();
    }

    ManagerT* manager() const noexcept {
        return mgr.get();
    }
};

/// 构造 vtable 入口上下文。
/// - `allowClosing=false`：注册、投递新工作等“开始新动作”的入口，实例进入
///   Closing/Disabled 后直接拒绝。
/// - `allowClosing=true`：只读查询、取消、完成清理等入口，实例关闭过程中仍允许
///   执行（由 lease 保证 unload 等待其返回），但不产生新注册。
template<typename InstanceT, typename ManagerT>
inline PluginHostCall<InstanceT, ManagerT>
    enterPluginHost(const AgentxxPluginHost* host, bool allowClosing = false) {
    PluginHostCall<InstanceT, ManagerT> call;
    auto                                control = resolvePluginHostControl(host);
    if (!control) {
        return call;
    }
    auto base = control->instance();
    if (!base) {
        return call;
    }
    auto inst = std::dynamic_pointer_cast<InstanceT>(base);
    if (!inst) {
        return call;
    }
    auto mgr = inst->manager.lock();
    if (!mgr) {
        return call;
    }
    auto guard = std::make_shared<PluginInstanceBase::InflightGuard>(base, allowClosing);
    if (!guard || !static_cast<bool>(*guard)) {
        return call;
    }
    call.inst  = std::move(inst);
    call.mgr   = std::move(mgr);
    call.guard = std::move(guard);
    return call;
}

// =====================================================================
// 管理器公共基类 (CRTP: Derived 提供实例类型与具体能力)
// =====================================================================

/// 插件管理器公共基类 (agent 侧 PluginManager / client 侧 ClientPluginManager 继承)
/// - 公共状态: 插件表 / io executor / ioThreadId_
/// - 公共操作: io 线程投递 (isIoThread/postToIo/postToIoAsync)、查找、等待
///   in-flight 归零 (waitInflightZero)、反向必选依赖收集 (reverseRequiredDeps)
/// - InstanceT 须继承 PluginInstanceBase; 具体加载/卸载/注册动作由 Derived
///   实现 (本类不持有 agent/client 特有字段)
template<typename InstanceT>
class PluginManagerBase {
public:

    using InstancePtr = std::shared_ptr<InstanceT>;

    /// 插件表 <name, instance> (仅 io 线程读写)
    std::map<std::string, InstancePtr, std::less<>> plugins_{};

    explicit PluginManagerBase(asio::any_io_executor ex = {}) {
        setIoExecutor(std::move(ex));
    }

    virtual ~PluginManagerBase() = default;

    PluginManagerBase(const PluginManagerBase&)            = delete;
    PluginManagerBase& operator=(const PluginManagerBase&) = delete;

    // ==================== 查找 ====================

    InstancePtr find(std::string_view name) const {
        auto it = plugins_.find(name);
        return it == plugins_.end() ? nullptr : it->second;
    }

    /// 是否仍有未安全关闭的实例 (stop 未完成 / lease 未归零 / destroy 未执行)。
    /// 用于 owner 在停止 executor、销毁 agent 或进程退出前自检关闭链路是否走完。
    bool hasPendingClose() const {
        for (const auto& [name, inst] : plugins_) {
            (void)name;
            if (!inst || !inst->pluginDestroyed) {
                return true;
            }
        }
        return false;
    }

    /// 预占插件名称，覆盖 Loading 期间的并发重复加载。
    /// 调用方必须在加载成功或失败时调用 releasePluginName()。
    bool reservePluginName(std::string_view name) {
        if (name.empty() || plugins_.find(name) != plugins_.end()
            || loadingNames_.find(name) != loadingNames_.end()) {
            return false;
        }
        loadingNames_.emplace(name);
        return true;
    }

    void releasePluginName(std::string_view name) {
        loadingNames_.erase(name);
    }

    bool isPluginNameLoading(std::string_view name) const {
        return loadingNames_.find(name) != loadingNames_.end();
    }

    /// 注册类入口的执行期复查（仅 IO 线程调用）。
    ///
    /// vtable 入口在调用方线程已取到 admission lease，但请求可能排在 IO 线程
    /// 队列里、等真正执行时实例已经进入 Closing/Disabled。此时注册必须被拒绝，
    /// 否则会在撤销注册之后又留下工具/hook/能力等残留。
    /// - 未装配 lifetime 的测试伪实例按"允许"处理；
    /// - 实例被显式禁用（enabled=false）时不再接受注册。
    bool acceptsRegistration(const PluginInstanceBase* inst) const {
        if (!inst || !inst->enabled) {
            return false;
        }
        if (!inst->lifetime) {
            return true;
        }
        return inst->lifetime->acceptsRegistration();
    }

    // ==================== io 线程投递 ====================

    void setIoExecutor(asio::any_io_executor ex) {
        ioExecutor_ = std::move(ex);
        if (ioExecutor_) {
            ioThreadId_.store(std::this_thread::get_id(), std::memory_order_release);
            replayRuntimeActions(runtime_);
        } else {
            ioThreadId_.store(std::thread::id{}, std::memory_order_release);
        }
    }

    bool isIoThread() const {
        const auto tid = ioThreadId_.load(std::memory_order_acquire);
        // io_context 已停止时, 即使当前调用线程正是最后绑定 executor 的线程,
        // 也不能内联执行: 视为"不可用", 让同步 ABI 调用快速失败而不是在已关闭的
        // runtime 上执行。
        if (!ioExecutor_ || runtimeExecutorStopped(ioExecutor_)) {
            return false;
        }
        return tid != std::thread::id{} && tid == std::this_thread::get_id();
    }

    /// 投递到所属 IO executor。闭包自身必须拥有执行所需状态；这里不再维护
    /// 捕获 manager 裸指针的二级队列。
    void postToIo(std::function<void()> fn) const {
        if (!fn) {
            return;
        }
        if (isIoThread()) {
            fn();
        } else if (!enqueueRuntimeAction(
                       runtime_,
                       [runtime = runtime_, fn = std::move(fn)]() mutable {
                           runtime->ioThreadId.store(
                               std::this_thread::get_id(),
                               std::memory_order_release
                           );
                           try {
                               fn();
                           } catch (const std::exception& e) {
                               XX_LOGW("Plugin IO task threw: {}", e.what());
                           } catch (...) {
                               XX_LOGW("Plugin IO task threw unknown exception");
                           }
                       },
                       false
                   )) {
            if (!ioExecutor_) {
                throw std::runtime_error("plugin runtime has no IO executor");
            }
            throw std::runtime_error("plugin runtime IO executor is stopped");
        }
    }

    /// 恒异步投递，防止 await_suspend 内同步重入。
    void postToIoAsync(std::function<void()> fn) const {
        if (!fn) {
            return;
        }
        if (!enqueueRuntimeAction(
                runtime_,
                [runtime = runtime_, fn = std::move(fn)]() mutable {
                    runtime->ioThreadId.store(
                        std::this_thread::get_id(),
                        std::memory_order_release
                    );
                    try {
                        fn();
                    } catch (const std::exception& e) {
                        XX_LOGW("Plugin asynchronous IO task threw: {}", e.what());
                    } catch (...) {
                        XX_LOGW("Plugin asynchronous IO task threw unknown exception");
                    }
                },
                false
            )) {
            if (!ioExecutor_) {
                throw std::runtime_error("plugin runtime has no IO executor");
            }
            throw std::runtime_error("plugin runtime IO executor is stopped");
        }
    }

    // ==================== 等待与依赖收集 ====================

    /// 收集反向必选依赖 (depends 含 target 的插件名; io 线程)
    /// - onlyEnabled=true: 仅统计 enabled 的插件 (卸载/禁用级联)
    /// - onlyEnabled=false: 全部统计 (启用级联: 需恢复被级联禁用的插件)
    std::vector<std::string>
        reverseRequiredDeps(const std::string& target, bool onlyEnabled) const {
        return agentxx::plugin::collectReverseRequiredDeps(plugins_, target, onlyEnabled);
    }

    /// 事件式等待插件执行 lease 归零；不使用定时轮询。
    asio::awaitable<bool>
        waitInflightZero(const InstancePtr& inst, std::chrono::milliseconds timeout) {
        if (!inst) {
            co_return true;
        }
        if (!inst->lifetime) {
            // 未装配运行时控制块的测试伪实例: 没有租约可等, 直接视为已归零。
            co_return true;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        const bool idle     = co_await inst->lifetime->waitIdleUntil(deadline);
        if (!idle) {
            XX_LOGW(
                "Plugin `{}` wait idle timed out (leases={})",
                inst->name,
                inst->lifetime->leaseCount()
            );
        }
        co_return idle;
    }

    const std::shared_ptr<PluginRuntime>& runtime() const noexcept {
        return runtime_;
    }

    const asio::any_io_executor& ioExecutor() const {
        return ioExecutor_;
    }

protected:

    uint64_t nextGeneration() noexcept {
        return runtime_->nextGeneration++;
    }

    std::shared_ptr<InstanceLifetime> makeLifetime(std::string name) {
        std::weak_ptr<PluginRuntime> runtime = runtime_;
        return std::make_shared<InstanceLifetime>(
            ioExecutor_,
            std::move(name),
            nextGeneration(),
            [runtime = std::move(runtime)](std::function<void()> fn) mutable {
                if (auto state = runtime.lock()) {
                    return enqueueRuntimeAction(state, std::move(fn), true);
                }
                return false;
            }
        );
    }

    std::shared_ptr<PluginRuntime>     runtime_    = std::make_shared<PluginRuntime>();
    asio::any_io_executor&             ioExecutor_ = runtime_->executor;
    std::atomic<std::thread::id>&      ioThreadId_ = runtime_->ioThreadId;
    std::set<std::string, std::less<>> loadingNames_;
};

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
    return hostMemoryCreateString(agentxx::plugin::PluginStringView::from(sv.data(), sv.size()));
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
    return hostMemoryCreateString(agentxx::plugin::PluginStringView::from(s, std::strlen(s)));
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

} // namespace plugin
} // namespace agentxx
