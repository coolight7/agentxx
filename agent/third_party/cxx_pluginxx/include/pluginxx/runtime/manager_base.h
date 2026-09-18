/// pluginxx 插件管理器公共基类 (宿主侧, 与宿主领域无关)
///
/// 背景: agent 侧 PluginManager 与 client 侧 ClientPluginManager 存在大量重复基建
/// (插件表查找/名称预占、io 线程投递、实例 lease 归零等待、反向必选依赖收集)。
/// 提取到本基类避免两侧行为漂移。
///
/// 内容:
/// - [PluginHostCall] / [enterPluginHost]: vtable 入口公共上下文 (解析宿主控制块 →
///   实例/管理器强引用 + admission lease), 使"已排队但尚未执行"的请求也被卸载的
///   idle 等待覆盖;
/// - [PluginManagerBase<InstanceT>]: 插件表 / io executor / io 线程投递 /
///   inflight 归零等待 / 名称预占 / 反向依赖收集;
/// - [collectReverseRequiredDeps]: 反向必选依赖收集 (模板)。
///
/// InstanceT 须继承 [pluginxx::PluginInstanceBase]; 具体加载/卸载/注册动作由派生类
/// 实现 (本类不持有宿主领域字段)。
///
/// 线程约定: 插件表仅 io 线程读写; `ioThreadId_` 与 inflight 计数为原子 (跨线程读写)。
#pragma once

#include "pluginxx/runtime/instance_base.h"
#include "utilxx_base/container_util.h"
#include "utilxx_base/log.h"
#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "asio/post.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace pluginxx {
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

    /// 逐个卸载全部插件, 共享同一超时时刻 (agent/client 两侧 shutdownAsync 的公共实现)
    ///
    /// - 先快照插件名再逐个卸载: 卸载过程会改动插件表, 不能边遍历边卸载
    /// - 每个实例调用派生类的 [unloadUntil] (name, deadline) 完成实际卸载
    /// - `return` 全部实例已关闭且插件表已空; 任一实例未关闭/超时返回 false
    ///
    /// - `args`:
    ///     - [timeout] 整体关闭超时 (各实例共享同一截止时刻, 不是每实例各自计时)
    ///     - [unloadUntil] 派生类的单实例卸载协程: (name, deadline) -> 是否已关闭
    template<typename UnloadUntilFn>
    asio::awaitable<bool>
        shutdownAllAsync(std::chrono::milliseconds timeout, UnloadUntilFn unloadUntil) {
        std::vector<std::string> names;
        names.reserve(plugins_.size());
        for (const auto& [name, inst] : plugins_) {
            (void)inst;
            names.push_back(name);
        }

        const auto deadline  = std::chrono::steady_clock::now() + timeout;
        bool       allClosed = true;
        for (const auto& name : names) {
            if (!find(name)) {
                continue; // 已被前序卸载级联移除
            }
            const bool closed = co_await unloadUntil(name, deadline);
            allClosed         = closed && allClosed;
        }
        co_return allClosed&& plugins_.empty();
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
        // 异构删除复用 utilxx_base::eraseHeterogeneous (libc++ 无 C++23 异构 erase)
        utilxx_base::eraseHeterogeneous(loadingNames_, name);
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
            pluginxx::replayRuntimeActions(runtime_);
        } else {
            ioThreadId_.store(std::thread::id{}, std::memory_order_release);
        }
    }

    bool isIoThread() const {
        const auto tid = ioThreadId_.load(std::memory_order_acquire);
        // io_context 已停止时, 即使当前调用线程正是最后绑定 executor 的线程,
        // 也不能内联执行: 视为"不可用", 让同步 ABI 调用快速失败而不是在已关闭的
        // runtime 上执行。
        if (!ioExecutor_ || pluginxx::runtimeExecutorStopped(ioExecutor_)) {
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
        } else if (!pluginxx::enqueueRuntimeAction(
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
        if (!pluginxx::enqueueRuntimeAction(
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
        return collectReverseRequiredDeps(plugins_, target, onlyEnabled);
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

    const std::shared_ptr<pluginxx::PluginRuntime>& runtime() const noexcept {
        return runtime_;
    }

    const asio::any_io_executor& ioExecutor() const {
        return ioExecutor_;
    }

protected:

    uint64_t nextGeneration() noexcept {
        return runtime_->nextGeneration++;
    }

    /// 为实例装配生命周期控制块 (状态机 + 执行 lease), 并把本管理器的运行时
    /// 一并写入实例 ([PluginInstanceBase::runtime])。
    ///
    /// 运行时经弱引用登记, 使 Operation 驱动器与驱动请求无需回查管理器类型即可
    /// 拿到 io executor 与线程标识 (框架内核因此不依赖宿主的管理器具体类型)。
    std::shared_ptr<pluginxx::InstanceLifetime>
        makeLifetime(const std::shared_ptr<PluginInstanceBase>& inst) {
        if (inst) {
            inst->runtime = runtime_;
        }
        std::weak_ptr<pluginxx::PluginRuntime> runtime = runtime_;
        return std::make_shared<pluginxx::InstanceLifetime>(
            ioExecutor_,
            inst ? inst->name : std::string{},
            nextGeneration(),
            [runtime = std::move(runtime)](std::function<void()> fn) mutable {
                if (auto state = runtime.lock()) {
                    return pluginxx::enqueueRuntimeAction(state, std::move(fn), true);
                }
                return false;
            }
        );
    }

    std::shared_ptr<pluginxx::PluginRuntime>     runtime_    = std::make_shared<pluginxx::PluginRuntime>();
    asio::any_io_executor&             ioExecutor_ = runtime_->executor;
    std::atomic<std::thread::id>&      ioThreadId_ = runtime_->ioThreadId;
    std::set<std::string, std::less<>> loadingNames_;
};

// =====================================================================
// 反向必选依赖收集
// =====================================================================

/// 收集必选依赖 target 的插件名 (io 线程)
/// - onlyEnabled=true: 仅统计 enabled 的插件 (卸载/禁用级联)
/// - onlyEnabled=false: 全部统计 (启用级联: 需恢复被级联禁用的插件)
/// - PluginMap 元素须含 name(键)/depends/enabled 成员
template<typename PluginMap>
std::vector<std::string> collectReverseRequiredDeps(
    const PluginMap&   plugins,
    const std::string& target,
    bool               onlyEnabled
) {
    std::vector<std::string> out;
    for (const auto& [name, inst] : plugins) {
        if (name == target || (onlyEnabled && !inst->enabled)) {
            continue;
        }
        if (std::find(inst->depends.begin(), inst->depends.end(), target) != inst->depends.end()) {
            out.push_back(name);
        }
    }
    return out;
}

} // namespace pluginxx