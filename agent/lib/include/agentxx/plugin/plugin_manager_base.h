/*
 * agentxx/plugin/plugin_manager_base.h —— 插件管理器公共基类 (宿主侧, agent/client 共用)
 *
 * 背景: agent 侧 PluginManager (plugin_manager_lifecycle.cpp 等) 与 client 侧
 * ClientPluginManager (client_plugin_manager.cpp) 存在大量重复基建:
 * - 实例公共字段 (元信息/依赖/启用标志/inflight/host 句柄)
 * - io 线程投递 (isIoThread/postToIo/postToIoAsync + ioThreadId_)
 * - 级联卸载/禁用骨架 (collectReverseRequiredDeps + waitInflightZero)
 * - 可执行目录 helper (跨平台 GetModuleFileNameW /proc/self/exe)
 * - C ABI 内存三件套 (alloc/free/strdup)
 * 提取到本基类避免两侧行为漂移 (历史上 client 侧多次"漏掉 agent 侧已修
 * 的问题", 见 plugins.md 13.x 记录), 公共化后修复只做一次。
 *
 * 结构:
 * - PluginInstanceBase: 实例公共基类 (两侧 PluginInstance/ClientPluginInstance
 *   继承; 持有元信息/标志/inflight/宿主句柄/InflightGuard)
 * - PluginManagerBase<InstanceT>: 管理器公共基类 (CRTP/模板注入实例类型;
 *   持有 io executor/ioThreadId_/插件表, 提供 io 投递/查找/等待/级联收集)
 * - hostMemoryAlloc/hostMemoryFree/hostMemoryStrdup: C ABI 跨 CRT 堆三件套
 *   (两侧 vtable 共用同一实现)
 * - getExecutableDirPath: 跨平台可执行目录 helper (builtin:// 回退探测用)
 *
 * 线程约定: 与两侧一致 —— 注册表/插件表仅 io 线程读写; 本类不引入锁
 * (ioThreadId_ 为原子, inflight 为原子, 跨线程递增/递减)。
 */
#pragma once

#include "agentxx/plugin/api/plugin_api.h" /* AgentxxPluginHost 等 C ABI 类型 */
#include "agentxx/plugin/plugin_common.h"  /* collectReverseRequiredDeps (模板) */
#include "agentxx/util/log.h"
#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "asio/post.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "neograph/json.h"

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
#include <mutex>
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

namespace agentxx {
namespace plugin {

// =====================================================================
// 实例公共基类
// =====================================================================

/// 插件实例公共基类 (agent 侧 PluginInstance / client 侧 ClientPluginInstance 继承)
/// - 持有跨端一致的元信息/依赖/启用标志/在途计数/宿主句柄
/// - InflightGuard 为公共 RAII (事件 handler / 命令 execute / 异步 op 入口计数)
struct PluginInstanceBase {
    std::string name;        ///< 唯一标识 (与对端插件共用命名空间)
    std::string version;     ///< 版本号 (get_info 或默认)
    std::string description; ///< 描述
    std::string path;        ///< 加载的库路径/内置路径
    /// 插件配置参数 (yaml `plugins` 条目 args; 宿主原样保存, 经 vtable
    /// get_plugin_args 整体返回给插件, 不解析其字段语义)
    neograph::json args = neograph::json::object();
    /// 插件配置文件所在目录或文件路径 (yaml `config`, 归一化为绝对路径)
    std::string              configPath;
    std::vector<std::string> depends; ///< 必选依赖 (未安装加载失败; 卸载/禁用级联)
    std::vector<std::string> optionalDepends;     ///< 可选依赖 (未安装仅警告)
    void*                    dlHandle  = nullptr; ///< dlopen/LoadLibrary 句柄
    void*                    pluginCtx = nullptr; ///< entry 输出的插件私有上下文
    bool                     enabled   = true; ///< 是否启用 (禁用: 注册摘除/命令停用)
    bool userDisabled    = false; ///< 是否被用户显式禁用 (区别于级联禁用)
    bool unloadRequested = false; ///< 已请求卸载 (防重复)

    /// 在途回调计数 (原子, 跨线程: 事件 handler/命令 execute/异步 op)
    std::atomic<size_t> inflight{0};

    explicit PluginInstanceBase(std::string in_name) :
        name(std::move(in_name)) {}

    virtual ~PluginInstanceBase() = default;

    PluginInstanceBase(const PluginInstanceBase&)            = delete;
    PluginInstanceBase& operator=(const PluginInstanceBase&) = delete;

    /// 在途计数 RAII (事件 handler / 命令 execute 入口调用)
    struct InflightGuard {
        PluginInstanceBase* inst;

        explicit InflightGuard(PluginInstanceBase* i) :
            inst(i) {
            if (inst) {
                inst->inflight.fetch_add(1, std::memory_order_acq_rel);
            }
        }

        ~InflightGuard() {
            if (inst) {
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

    explicit PluginManagerBase(asio::any_io_executor ex = {}) :
        ioExecutor_(std::move(ex)) {
        if (ioExecutor_) {
            ioThreadId_.store(std::this_thread::get_id(), std::memory_order_release);
        }
    }

    virtual ~PluginManagerBase() = default;

    PluginManagerBase(const PluginManagerBase&)            = delete;
    PluginManagerBase& operator=(const PluginManagerBase&) = delete;

    // ==================== 查找 ====================

    InstancePtr find(std::string_view name) const {
        auto it = plugins_.find(name);
        return it == plugins_.end() ? nullptr : it->second;
    }

    // ==================== io 线程投递 ====================

    void setIoExecutor(asio::any_io_executor ex) {
        ioExecutor_ = std::move(ex);
        if (ioExecutor_) {
            ioThreadId_.store(std::this_thread::get_id(), std::memory_order_release);
        }
    }

    bool isIoThread() const {
        const auto tid = ioThreadId_.load(std::memory_order_acquire);
        return !ioExecutor_ || (tid != std::thread::id{} && tid == std::this_thread::get_id());
    }

    /// 投递到 io 线程 (调用方为 io 线程时同步执行)
    void postToIo(std::function<void()> fn) const {
        if (isIoThread()) {
            fn();
        } else if (ioExecutor_) {
            {
                std::lock_guard lk(ioTasksMtx_);
                ioTasks_.push_back(std::move(fn));
            }
            asio::post(ioExecutor_, [this]() {
                ioThreadId_.store(std::this_thread::get_id(), std::memory_order_release);
                runPendingIoTasks();
            });
        } else {
            XX_LOGW("PluginManagerBase::postToIo: no io executor, executing on caller thread");
            fn();
        }
    }

    /// 异步投递到 io 线程 (恒经 asio::post 入队, 禁止同步重入):
    /// 供 SchedulerIface::post_to_io / YieldAwaiter 等锚定协程恢复路径使用,
    /// 即使调用方已在 io 线程也一律异步, 避免 await_suspend 内的重入 UB
    void postToIoAsync(std::function<void()> fn) const {
        if (ioExecutor_) {
            {
                std::lock_guard lk(ioTasksMtx_);
                ioTasks_.push_back(std::move(fn));
            }
            asio::post(ioExecutor_, [this]() {
                ioThreadId_.store(std::this_thread::get_id(), std::memory_order_release);
                runPendingIoTasks();
            });
        } else {
            XX_LOGW("PluginManagerBase::postToIoAsync: no io executor, executing on caller thread");
            fn();
        }
    }

    void runPendingIoTasks() const {
        std::deque<std::function<void()>> tasks;
        {
            std::lock_guard lk(ioTasksMtx_);
            tasks.swap(ioTasks_);
        }
        for (auto& t : tasks) {
            if (t) {
                try {
                    t();
                } catch (...) {
                }
            }
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

    /// 等待插件在途计数归零 (io 线程协程轮询, 指数退避); 超时返回 false
    /// 两侧共享实现 (原 agent 侧固定 10ms 轮询, client 侧指数退避 —— 统一
    /// 采用指数退避 20ms→1s, 减少慢回调等待期间 io 线程定时器唤醒)
    asio::awaitable<bool>
        waitInflightZero(const InstancePtr& inst, std::chrono::milliseconds timeout) {
        if (!inst) {
            co_return true;
        }
        auto deadline = std::chrono::steady_clock::now() + timeout;
        auto backoff  = std::chrono::milliseconds{20};
        while (inst->inflight.load(std::memory_order_acquire) > 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                XX_LOGW(
                    "Plugin `{}` waitInflightZero timed out (inflight={})",
                    inst->name,
                    inst->inflight.load(std::memory_order_acquire)
                );
                co_return false;
            }
            auto timer = asio::steady_timer(co_await asio::this_coro::executor);
            timer.expires_after(backoff);
            co_await timer.async_wait(asio::use_awaitable);
            backoff = std::min(backoff * 2, std::chrono::milliseconds{1000});
        }
        co_return true;
    }

    const asio::any_io_executor& ioExecutor() const {
        return ioExecutor_;
    }

protected:

    mutable std::mutex                        ioTasksMtx_;
    mutable std::deque<std::function<void()>> ioTasks_;

    asio::any_io_executor                ioExecutor_{};
    mutable std::atomic<std::thread::id> ioThreadId_{};
};

// =====================================================================
// C ABI 内存三件套 (跨 CRT 堆边界; 两侧 vtable 共用)
// =====================================================================

inline void* hostMemoryAlloc(size_t size) {
    return ::malloc(size);
}

inline void hostMemoryFree(void* ptr) {
    ::free(ptr);
}

inline char* hostMemoryStrdup(const char* s) {
    if (!s) {
        return nullptr;
    }
    size_t n = std::strlen(s) + 1;
    char*  p = static_cast<char*>(::malloc(n));
    if (p) {
        std::memcpy(p, s, n);
    }
    return p;
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
