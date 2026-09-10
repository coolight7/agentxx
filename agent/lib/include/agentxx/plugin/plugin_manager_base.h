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
/// - hostMemoryAlloc/hostMemoryFree/hostMemoryStrdup: C ABI 跨 CRT 堆三件套
///   (两侧 vtable 共用同一实现)
/// - getExecutableDirPath: 跨平台可执行目录 helper (builtin:// 回退探测用)
///
/// 线程约定: 与两侧一致 —— 注册表/插件表仅 io 线程读写; 本类不引入锁
/// (ioThreadId_ 为原子, inflight 为原子, 跨线程递增/递减)。
#pragma once

#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/plugin_common.h"
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
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <set>
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
    bool userDisabled    = false; ///< 是否被用户显式禁用 (区别于级联禁用)
    bool blockedByDependencies = false; ///< 是否因必选依赖不可用而级联禁用
    bool unloadRequested = false; ///< 已请求卸载 (防重复)
    /// create 是否成功产出可销毁的 pluginCtx。
    bool pluginCreated   = false;
    /// 插件上下文是否已经调用 destroy。只在所属 IO 线程更新。
    bool pluginDestroyed = false;
    /// 同步关闭发现活动 lease 时，等待最后一个 lease 释放后再执行 destroy。
    bool destroyDeferred = false;
    /// Optional Reset-v1 lifecycle hooks discovered beside create/destroy.
    /// Legacy plugins keep these null and retain their create-time setup.
    AgentxxPluginStartFn lifecycleStart = nullptr;
    AgentxxPluginStopFn  lifecycleStop  = nullptr;
    /// 实例已经完全激活 (start 事务成功, 或该插件没有 start 导出)。
    /// 只有为 true 的实例才需要 (且必须) 先执行 stop 才能 destroy/dlclose。
    bool lifecycleStarted = false;
    bool lifecycleStopped = false;

    /// stop 事务仍待执行。同步关闭路径无法等待该事务，因此必须保留实例、
    /// 上下文与动态库，交由仍运行的异步 owner (unloadAsync/shutdownAsync) 收尾。
    bool lifecycleStopPending() const noexcept {
        return lifecycleStop != nullptr && lifecycleStarted && !lifecycleStopped;
    }

    std::vector<std::shared_ptr<::AgentxxPluginOperatorHandle>> operatorHandles;
    std::vector<std::shared_ptr<::AgentxxPluginOperationCompletionEndpoint>> completionEndpoints;
    std::vector<std::shared_ptr<::AgentxxPluginOperatorHandle>> outstandingOps;

    /// 由实例创建路径设置，供只拿到裸指针的宿主回调升级 owner。
    std::weak_ptr<PluginInstanceBase> ownerSelf;

    /// Reset-v1 宿主生命周期。实例对象本身只保存业务注册信息；所有跨线程
    /// 执行都通过 lifetime lease 保证 stop/destroy/dlclose 前已经返回。
    std::shared_ptr<InstanceLifetime> lifetime;

    /// 兼容查询字段：值与 lifetime->leaseCount() 同步更新，待所有调用方迁移
    /// 到 lifetime 后可移除。
    std::atomic<size_t> inflight{0};

    explicit PluginInstanceBase(std::string in_name) :
        name(std::move(in_name)) {}

    virtual ~PluginInstanceBase() = default;

    PluginInstanceBase(const PluginInstanceBase&)            = delete;
    PluginInstanceBase& operator=(const PluginInstanceBase&) = delete;

    /// 执行 lease RAII。优先使用 Reset-v1 lifetime；尚未装配 lifetime 的测试
    /// 伪实例仍更新兼容 inflight 字段。
    struct InflightGuard {
        PluginInstanceBase* inst = nullptr;
        std::shared_ptr<PluginInstanceBase> owner;
        InstanceLease       lease;
        bool                legacy = false;

        explicit InflightGuard(
            std::shared_ptr<PluginInstanceBase> i,
            bool allowClosing = false
        ) :
            inst(i.get()),
            owner(std::move(i)),
            lease(inst ? InstanceLease::acquire(inst->lifetime, allowClosing) : InstanceLease{}) {
            if (inst && inst->lifetime) {
                if (lease) {
                    inst->inflight.fetch_add(1, std::memory_order_acq_rel);
                }
            } else if (inst) {
                legacy = true;
                inst->inflight.fetch_add(1, std::memory_order_acq_rel);
            }
        }

        explicit InflightGuard(PluginInstanceBase* i, bool allowClosing = false) :
            inst(i),
            owner(i ? i->ownerSelf.lock() : nullptr),
            lease(i ? InstanceLease::acquire(i->lifetime, allowClosing) : InstanceLease{}) {
            if (inst && inst->lifetime) {
                if (lease) {
                    inst->inflight.fetch_add(1, std::memory_order_acq_rel);
                }
            } else if (inst) {
                legacy = true;
                inst->inflight.fetch_add(1, std::memory_order_acq_rel);
            }
        }

        explicit operator bool() const noexcept {
            return legacy || static_cast<bool>(lease);
        }

        ~InflightGuard() {
            if (inst && (legacy || lease)) {
                inst->inflight.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
    };
};

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
        // A stopped io_context cannot safely execute an inline operation even
        // when the last thread that bound the executor happens to be calling
        // now.  Treat it as unavailable so synchronous ABI callers fail fast
        // instead of running against a closed runtime.
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
        } else if (!enqueueRuntimeAction(runtime_, [runtime = runtime_, fn = std::move(fn)]() mutable {
                runtime->ioThreadId.store(std::this_thread::get_id(), std::memory_order_release);
                try {
                    fn();
                } catch (const std::exception& e) {
                    XX_LOGW("Plugin IO task threw: {}", e.what());
                } catch (...) {
                    XX_LOGW("Plugin IO task threw unknown exception");
                }
            }, false)) {
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
        if (!enqueueRuntimeAction(runtime_, [runtime = runtime_, fn = std::move(fn)]() mutable {
                runtime->ioThreadId.store(std::this_thread::get_id(), std::memory_order_release);
                try {
                    fn();
                } catch (const std::exception& e) {
                    XX_LOGW("Plugin asynchronous IO task threw: {}", e.what());
                } catch (...) {
                    XX_LOGW("Plugin asynchronous IO task threw unknown exception");
                }
            }, false)) {
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
            // 仅兼容未装配 runtime 的测试伪实例；生产实例始终有 lifetime。
            co_return inst->inflight.load(std::memory_order_acquire) == 0;
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

    const std::shared_ptr<PluginRuntime>& runtime() const noexcept { return runtime_; }

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

    std::shared_ptr<PluginRuntime> runtime_ = std::make_shared<PluginRuntime>();
    asio::any_io_executor& ioExecutor_ = runtime_->executor;
    std::atomic<std::thread::id>& ioThreadId_ = runtime_->ioThreadId;
    std::set<std::string, std::less<>> loadingNames_;
};

// =====================================================================
// C ABI 内存三件套 (跨 CRT 堆边界; 两侧 vtable 共用)
// =====================================================================

inline void* hostMemoryAlloc(uint64_t size) {
    return ::malloc(static_cast<size_t>(size));
}

inline void hostMemoryFree(void* ptr) {
    ::free(ptr);
}

inline char* hostMemoryStrdup(const AgentxxPluginStringView* s) {
    if (!s || (!s->data && s->size == 0)) {
        return nullptr;
    }
    char* p = static_cast<char*>(hostMemoryAlloc(s->size + 1));
    if (p) {
        if (s->size > 0 && s->data) {
            std::memcpy(p, s->data, static_cast<size_t>(s->size));
        }
        p[s->size] = '\0';
    }
    return p;
}

inline char* hostMemoryStrdup(AgentxxPluginStringView s) {
    return hostMemoryStrdup(&s);
}

inline char* hostMemoryStrdup(const char* s) {
    if (!s) {
        return nullptr;
    }
    auto sv = agentxx::plugin::PluginStringView::from(s, std::strlen(s));
    return hostMemoryStrdup(&sv);
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
