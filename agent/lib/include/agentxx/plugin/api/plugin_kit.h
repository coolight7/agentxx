/*
 * agentxx/plugin/api/plugin_kit.h —— 插件开发 SDK (C++ header-only)
 *
 * 命名空间: 全部位于 agentxx::plugin (与宿主侧 plugin 命名空间统一;
 * 历史命名空间 agentxx::kit 已并入, 见 git 历史)
 *
 * 锚定协程模型 SDK:
 * - PluginBase: 实例上下文基类, 集中常用宿主操作 (workDir / toolPrompt / log 等)
 * - Logger: 实例级日志闭包, 消除进程级全局
 * - Task<T>: 极简锚定协程类型 (无外部执行器依赖, 帧先销毁后 done 上报)
 * - 锚定原语 awaiter 族: sleep / yield / offload / call_tool / invoke_cap
 * - 注册族: tool (Task协程) / fast_tool (快同步内联) / blocking_tool (阻塞池委托) / hook /
 * capability
 * - spawn: 后台协作任务 (sleep 循环, 卸载取消)
 * - 阻塞便捷助手: 供 JS 引擎及非 io 线程使用 (基于 condvar)
 */
#pragma once

#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_iface_helper.h"
#include "fmt/format.h"
#include "neograph/json.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace agentxx {
namespace plugin {

/* ==================== 取消异常 ==================== */

class CancelledException : public std::exception {
    std::string msg_;

public:

    explicit CancelledException(std::string msg = "operation cancelled") :
        msg_(std::move(msg)) {}

    const char* what() const noexcept override {
        return msg_.c_str();
    }
};

/* ==================== 实例级 Logger ==================== */

struct Logger {
    const AgentxxPluginHost*     host     = nullptr;
    const AgentxxPluginLogIface* logIface = nullptr;

    void log(int level, std::string_view msg) const noexcept {
        if (host && logIface && logIface->log) {
            logIface->log(host, level, agentxx_plugin_sv(msg.data(), msg.size()));
        }
    }

    void trace(std::string_view msg) const noexcept {
        log(0, msg);
    }

    void debug(std::string_view msg) const noexcept {
        log(1, msg);
    }

    void info(std::string_view msg) const noexcept {
        log(2, msg);
    }

    void warn(std::string_view msg) const noexcept {
        log(3, msg);
    }

    void error(std::string_view msg) const noexcept {
        log(4, msg);
    }
};

/* ==================== 操作控制对象 (OpCtl) ==================== */

struct OpCtl {
    std::shared_ptr<std::atomic<bool>> cancelFlag;
    const AgentxxPluginHost*           host        = nullptr;
    const AgentxxPluginCancelIface*    cancelIface = nullptr;
    std::string                        threadId;

    bool cancelled() const noexcept {
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
            return true;
        }
        if (host && cancelIface && cancelIface->is_cancelled && !threadId.empty()) {
            return cancelIface->is_cancelled(
                       host,
                       agentxx_plugin_sv(threadId.data(), threadId.size())
                   )
                   != 0;
        }
        return false;
    }

    void throw_if_cancelled() const {
        if (cancelled()) {
            throw CancelledException("operation cancelled");
        }
    }
};

/* ==================== 提示词描述解析结构 ==================== */

struct ToolPromptText {
    std::string                                     depict;
    std::map<std::string, std::string, std::less<>> args;
};

inline std::string
    toolPromptArgDesc(const ToolPromptText& p, std::string_view key, std::string_view fallback) {
    auto it = p.args.find(key);
    if (it != p.args.end() && !it->second.empty()) {
        return it->second;
    }
    return std::string{fallback};
}

/* ==================== Task<T> 锚定协程与完成协议 ==================== */

template<typename T = void>
struct Task;

namespace detail {

/// strdup 的跨平台 C++ 通用替代 (malloc + memcpy, 与宿主 hostMemoryStrdup 同构):
/// - 规避 MSVC C4996 (POSIX strdup 弃用警告), GCC/Clang 行为不变
/// - 仅作宿主 vtable 不可用时的兜底分配; malloc 分配与宿主 ::free 释放配对,
///   正常路径 host->vtable->strdup 分配同样兼容宿主 ::free
inline char* strdupFallback(AgentxxPluginStringView s) {
    if (!s.data && s.size == 0) {
        return nullptr;
    }
    char* p = static_cast<char*>(std::malloc(s.size + 1));
    if (p) {
        if (s.size > 0 && s.data) {
            std::memcpy(p, s.data, s.size);
        }
        p[s.size] = '\0';
    }
    return p;
}

inline char* strdupFallback(const char* s) {
    if (!s) {
        return nullptr;
    }
    return strdupFallback(agentxx_plugin_sv_cstr(s));
}

template<typename Promise>
void finishIfDone(std::coroutine_handle<Promise> h) {
    if (!h || !h.done()) {
        return;
    }
    auto&                       p      = h.promise();
    AgentxxPluginOperatorNotify notify = p.notify_;
    int                         status = AGENTXX_PLUGIN_OPERATOR_OK;
    std::string                 errPayload;
    std::string                 resPayload;

    if (p.has_exception()) {
        status = AGENTXX_PLUGIN_OPERATOR_FAILED;
        try {
            std::rethrow_exception(p.exception());
        } catch (const CancelledException&) {
            status = AGENTXX_PLUGIN_OPERATOR_CANCELLED;
        } catch (const std::exception& e) {
            errPayload = e.what();
        } catch (...) {
            errPayload = "unknown task error";
        }
    } else {
        if constexpr (!std::is_void_v<typename Promise::value_type>) {
            resPayload = p.result_string();
        }
    }

    // 协程完成 (帧将销毁) 时挂接的资源清理 (如锚定协程工具的 op 句柄 Job):
    // - 必须在 h.destroy() 之前取出 (destroy 后 promise 内存已失效)
    // - 执行时机: destroy + done 通知【之后】—— 宿主收到 done 后不再访问 op
    //   句柄 (取消仅在 notified 置位前发生), 此时删除 Job 无竞态
    auto cleanup = std::move(p.opCleanup_);
    p.opCleanup_ = nullptr;

    h.destroy();

    if (notify.done) {
        AgentxxPluginStringView sv = agentxx_plugin_sv(nullptr, 0);
        if (status == AGENTXX_PLUGIN_OPERATOR_FAILED) {
            sv = agentxx_plugin_sv(errPayload.data(), errPayload.size());
        } else if (status == AGENTXX_PLUGIN_OPERATOR_OK) {
            sv = agentxx_plugin_sv(resPayload.data(), resPayload.size());
        }
        notify.done(notify.host_ud, status, sv);
    }

    if (cleanup) {
        cleanup();
    }
}

template<typename T>
struct PromiseBase {
    using value_type = T;

    AgentxxPluginOperatorNotify        notify_{nullptr, nullptr};
    const AgentxxPluginHost*           host_{nullptr};
    std::shared_ptr<std::atomic<bool>> cancelFlag_{nullptr};
    std::function<void()>              outstandingCancel_{nullptr};
    std::exception_ptr                 exception_{nullptr};
    /// 协程帧完成销毁后的资源清理 (见 finishIfDone): 锚定协程工具的 op 句柄
    /// (Job) 释放挂接于此 —— Job 仅在协程挂起期间被宿主 execute_cancel 访问,
    /// 完成通知发出后宿主不再触碰 op, 此时删除安全
    std::function<void()> opCleanup_{nullptr};

    std::suspend_always initial_suspend() noexcept {
        return {};
    }

    std::suspend_always final_suspend() noexcept {
        return {};
    }

    void unhandled_exception() noexcept {
        exception_ = std::current_exception();
    }

    bool has_exception() const noexcept {
        return exception_ != nullptr;
    }

    std::exception_ptr exception() const noexcept {
        return exception_;
    }

    void set_exception(std::exception_ptr ep) noexcept {
        exception_ = ep;
    }

    void set_outstanding(std::function<void()> c) {
        outstandingCancel_ = std::move(c);
    }

    void clear_outstanding() noexcept {
        outstandingCancel_ = nullptr;
    }

    void cancel_outstanding() {
        if (outstandingCancel_) {
            auto fn            = std::move(outstandingCancel_);
            outstandingCancel_ = nullptr;
            try {
                fn();
            } catch (...) {
            }
        }
    }
};

} // namespace detail

template<typename T>
struct Task {
    struct promise_type : detail::PromiseBase<T> {
        std::optional<T> result_;

        Task get_return_object() noexcept {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        template<typename U>
        void return_value(U&& val) {
            result_.emplace(std::forward<U>(val));
        }

        std::string result_string() const {
            if constexpr (std::is_same_v<std::decay_t<T>, std::string>) {
                return result_.value_or("");
            } else {
                return neograph::json(result_.value_or(T{})).dump();
            }
        }
    };

    std::coroutine_handle<promise_type> handle_;

    explicit Task(std::coroutine_handle<promise_type> h) noexcept :
        handle_(h) {}

    Task(Task&& o) noexcept :
        handle_(std::exchange(o.handle_, nullptr)) {}

    Task& operator=(Task&& o) noexcept {
        if (this != &o) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(o.handle_, nullptr);
        }
        return *this;
    }

    ~Task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    Task(const Task&)            = delete;
    Task& operator=(const Task&) = delete;
};

template<>
struct Task<void> {
    struct promise_type : detail::PromiseBase<void> {
        Task get_return_object() noexcept {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        void return_void() noexcept {}
    };

    std::coroutine_handle<promise_type> handle_;

    explicit Task(std::coroutine_handle<promise_type> h) noexcept :
        handle_(h) {}

    Task(Task&& o) noexcept :
        handle_(std::exchange(o.handle_, nullptr)) {}

    Task& operator=(Task&& o) noexcept {
        if (this != &o) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(o.handle_, nullptr);
        }
        return *this;
    }

    ~Task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    Task(const Task&)            = delete;
    Task& operator=(const Task&) = delete;
};

/* ==================== PluginBase 实例上下文基类 ==================== */

struct PluginBase {
    const AgentxxPluginHost*     host = nullptr;
    agentxx::plugin::AgentIfaces iface{};
    Logger                       log{};

    virtual ~PluginBase() = default;

    void init(const AgentxxPluginHost* in_host) {
        host = in_host;
        if (host) {
            iface = agentxx::plugin::AgentIfaces::query(host);
            log   = Logger{host, iface.log};
        }
    }

    /// 会话工作目录 (io 线程): tid 非空时返回该会话生效目录
    /// (worktree 绑定优先, 依次回退会话覆写 / AgentConfig); tid 为空时
    /// 返回默认会话工作目录 (等价旧 get_work_dir 语义)
    std::string workDir(AgentxxPluginStringView tid = {}) const {
        if (!host || !iface.config || !iface.config->get_session_work_dir) {
            return "";
        }
        char* p = iface.config->get_session_work_dir(host, tid);
        if (p) {
            std::string res(p);
            host->vtable->free(p);
            return res;
        }
        return "";
    }

    ToolPromptText toolPrompt(std::string_view tool) const {
        ToolPromptText res;
        if (!host || !iface.config || !iface.config->get_tool_prompt) {
            return res;
        }
        char* p = iface.config->get_tool_prompt(host, agentxx_plugin_sv(tool.data(), tool.size()));
        if (!p) {
            return res;
        }
        std::string jsonStr(p);
        host->vtable->free(p);
        try {
            auto j = neograph::json::parse(jsonStr);
            if (j.contains("depict") && j["depict"].is_string()) {
                res.depict = j["depict"].get<std::string>();
            }
            if (j.contains("args") && j["args"].is_object()) {
                for (const auto& [k, v] : j["args"].items()) {
                    if (v.is_string()) {
                        res.args[k] = v.get<std::string>();
                    }
                }
            }
        } catch (...) {
        }
        return res;
    }

    std::string argsJson() const {
        if (!host || !iface.config || !iface.config->get_plugin_args) {
            return "{}";
        }
        char* p = iface.config->get_plugin_args(host);
        if (!p) {
            return "{}";
        }
        std::string res(p);
        host->vtable->free(p);
        return res;
    }

    /// 插件配置文件所在目录或文件路径 (yaml `config`, 归一化绝对路径)
    /// - 为空表示未指定; 可指向文件或目录 (由插件自行判断)
    std::string configPath() const {
        if (!host || !iface.config || !iface.config->get_plugin_config_path) {
            return "";
        }
        char* p = iface.config->get_plugin_config_path(host);
        if (!p) {
            return "";
        }
        std::string res(p);
        host->vtable->free(p);
        return res;
    }

    bool sessionCancelled(AgentxxPluginStringView tid) const {
        if (!host || !iface.cancel || !iface.cancel->is_cancelled) {
            return false;
        }
        return iface.cancel->is_cancelled(host, tid) != 0;
    }

    long long addShareStore(AgentxxPluginStringView tid, std::string_view content) const {
        if (!host || !iface.session || !iface.session->add_share_store) {
            return -1;
        }
        return iface.session
            ->add_share_store(host, tid, agentxx_plugin_sv(content.data(), content.size()));
    }

    char* strdup(AgentxxPluginStringView sv) const {
        if (!host || !host->vtable || !host->vtable->strdup) {
            return nullptr;
        }
        return host->vtable->strdup(sv);
    }

    char* strdup(const char* s) const {
        if (!s) {
            return nullptr;
        }
        return strdup(agentxx_plugin_sv_cstr(s));
    }

    void free(char* p) const {
        if (host && host->vtable && host->vtable->free && p) {
            host->vtable->free(p);
        }
    }

    std::string jsonEscape(AgentxxPluginStringView sv) const {
        if (!host || !iface.json || !iface.json->json_escape || agentxx_plugin_sv_empty(sv)) {
            return "\"\"";
        }
        char* esc = iface.json->json_escape(host, sv);
        if (!esc) {
            return "\"\"";
        }
        std::string res(esc);
        host->vtable->free(esc);
        return res;
    }

    // ---- 注册 RAII 存储 ----
    std::vector<std::string>                            storage_;
    std::vector<std::unique_ptr<void, void (*)(void*)>> shims_;

    template<typename T>
    T* storeShim(std::unique_ptr<T> p) {
        T* raw = p.get();
        shims_.emplace_back(p.release(), [](void* ptr) {
            delete static_cast<T*>(ptr);
        });
        return raw;
    }

    // 后台任务记录 (spawn 宿主托管: 见 agentxx.agent.tasks 接口表)
    // 堆稳定地址: spawn 时以 shared_ptr 存于 vector, post_to_io 传递堆地址
    // 避免 vector 扩容导致 &spawns_.back() 悬垂
    //
    // 生命周期与并发 (对应 docs/agent/plugins.md spawn 节; 实现约束见
    // 自由函数 spawnTaskImpl):
    // - spawns_ 持 rec: 保证 starter post 任务期间 (注册 → 首挂起) rec 存活;
    //   宿主 register_task 后协程由宿主清理协程托底 (句柄 shared_ptr),
    //   spawns_ 里的 rec 在协程完成/实例析构时被清除 (rec->starter 置空)
    // - rec->coroAddr: 仅在 io 线程读写 (cancel_fn 与 finishIfDone 完成路径
    //   都在 io 线程访问它; 见 opCleanup_ 清空注释)
    // - cancel_fn (宿主卸载取消时回调, io 线程): 置 cancelFlag + 经 coroAddr
    //   访问 promise 的 outstandingCancel_ 唤醒挂起的 sleep/offload; 若
    //   coroAddr 已清空 (协程已完成/帧已销毁) 则只置 flag 不碰帧 —— 无 UAF
    struct SpawnRecord {
        std::function<void()>              starter;
        std::shared_ptr<std::atomic<bool>> cancelFlag;
        /// 协程挂起后的句柄地址 (由 starter 在协程首次挂起时记录; 协程完成
        /// 路径 (finishIfDone 的 opCleanup_) 清空)。宿主 cancel_fn 经它访问
        /// promise 的 outstandingCancel_ 唤醒挂起操作
        void* coroAddr = nullptr;
    };

    std::vector<std::shared_ptr<SpawnRecord>> spawns_;

    /// 【已废弃】停止全部后台 spawn 协程 —— 不再需要: spawn 自 API v1 起注册
    /// 到宿主 agentxx.agent.tasks 接口表 (outstandingOps), 插件卸载时宿主
    /// detachAll 统一 cancel (置 cancelFlag + 唤醒挂起操作) + waitInflightZero
    /// 精确等待协程退出, 无协程帧悬挂。保留方法仅为旧第三方插件手写后台任务
    /// 的兼容参考 (宿主无 tasks 表时 spawn 不注册, 见 spawnTaskImpl)。
    void stopSpawns() {
        for (auto& rec : spawns_) {
            if (!rec || !rec->cancelFlag) {
                continue;
            }
            rec->cancelFlag->store(true, std::memory_order_release);
            if (rec->coroAddr) {
                auto handle
                    = std::coroutine_handle<detail::PromiseBase<void>>::from_address(
                        rec->coroAddr
                    );
                handle.promise().cancel_outstanding();
            }
        }
    }

    template<typename Fn>
    void spawn(Fn&& fn);
};

/* ==================== 锚定原语 awaiter 族 ==================== */

namespace detail {

struct SleepAwaiter {
    const AgentxxPluginHost*           host;
    const AgentxxPluginSchedulerIface* sched;
    long                               ms;
    void*                              timer = nullptr;

    bool await_ready() const noexcept {
        return ms <= 0 || !sched || !sched->sleep;
    }

    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> h) {
        auto& p = h.promise();
        timer   = sched->sleep(
            host,
            ms,
            [](void* ud) {
                auto  handle = std::coroutine_handle<Promise>::from_address(ud);
                auto& prom   = handle.promise();
                prom.clear_outstanding();
                try {
                    handle.resume();
                } catch (...) {
                    prom.set_exception(std::current_exception());
                }
                finishIfDone(handle);
            },
            h.address()
        );
        p.set_outstanding([host = this->host, sched = this->sched, timer = this->timer]() {
            if (sched && sched->cancel_sleep && timer) {
                sched->cancel_sleep(host, timer);
            }
        });
    }

    void await_resume() const noexcept {}
};

struct YieldAwaiter {
    const AgentxxPluginHost*           host;
    const AgentxxPluginSchedulerIface* sched;

    bool await_ready() const noexcept {
        return !sched || !sched->post_to_io;
    }

    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> h) {
        sched->post_to_io(
            host,
            [](void* ud) {
                auto  handle = std::coroutine_handle<Promise>::from_address(ud);
                auto& prom   = handle.promise();
                try {
                    handle.resume();
                } catch (...) {
                    prom.set_exception(std::current_exception());
                }
                finishIfDone(handle);
            },
            h.address()
        );
    }

    void await_resume() const noexcept {}
};

template<typename WorkFn>
struct OffloadAwaiter {
    using ResultType = std::decay_t<std::invoke_result_t<WorkFn, volatile int*>>;

    const AgentxxPluginHost*           host;
    const AgentxxPluginSchedulerIface* sched;
    WorkFn                             work;
    volatile int                       cancelFlag = 0;
    std::exception_ptr                 exPtr      = nullptr;
    std::optional<ResultType>          result;

    bool await_ready() const noexcept {
        return !sched || !sched->offload;
    }

    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> h) {
        auto& p   = h.promise();
        coroAddr_ = h.address();
        p.set_outstanding([this]() {
            this->cancelFlag = 1;
        });

        sched->offload(
            host,
            &cancelFlag,
            [](void* ud, volatile int* cflag, char** error_out) -> void* {
                auto* self = static_cast<OffloadAwaiter*>(ud);
                try {
                    if constexpr (std::is_void_v<ResultType>) {
                        self->work(cflag);
                    } else {
                        self->result = self->work(cflag);
                    }
                } catch (...) {
                    self->exPtr = std::current_exception();
                }
                (void)error_out;
                return nullptr;
            },
            [](void* ud, void* res, AgentxxPluginStringView err) {
                (void)res;
                (void)err;
                auto* self   = static_cast<OffloadAwaiter*>(ud);
                auto  handle = std::coroutine_handle<Promise>::from_address(self->coroAddr_);
                auto& prom   = handle.promise();
                prom.clear_outstanding();
                try {
                    handle.resume();
                } catch (...) {
                    prom.set_exception(std::current_exception());
                }
                finishIfDone(handle);
            },
            this
        );
    }

    ResultType await_resume() {
        if (exPtr) {
            std::rethrow_exception(exPtr);
        }
        if constexpr (!std::is_void_v<ResultType>) {
            return std::move(*result);
        }
    }

    void* coroAddr_ = nullptr;
};

struct CallToolState {
    const AgentxxPluginHost*       host  = nullptr;
    const AgentxxPluginToolsIface* tools = nullptr;
    std::string                    name;
    std::string                    argsJson;
    std::string                    threadId;
    AgentxxPluginOperatorHandle*   opHandle = nullptr;
    int                            status   = AGENTXX_PLUGIN_OPERATOR_OK;
    std::string                    payload;
    std::string                    startError;
    std::atomic<bool>              suspended{false};
    std::atomic<bool>              callbackFired{false};
    void*                          coroAddr            = nullptr;
    void (*schedPost)(const AgentxxPluginHost*, void*) = nullptr;
    std::atomic<bool> destroyed{false};
};

struct CallToolAwaiter {
    std::shared_ptr<CallToolState> st;

    CallToolAwaiter(
        const AgentxxPluginHost*       in_host,
        const AgentxxPluginToolsIface* in_tools,
        std::string_view               in_name,
        std::string_view               in_args,
        std::string_view               in_tid,
        void (*in_post)(const AgentxxPluginHost*, void*) = nullptr
    ) :
        st(std::make_shared<CallToolState>()) {
        st->host      = in_host;
        st->tools     = in_tools;
        st->name      = std::string(in_name);
        st->argsJson  = std::string(in_args);
        st->threadId  = std::string(in_tid);
        st->schedPost = in_post;
    }

    ~CallToolAwaiter() {
        if (st) {
            st->destroyed.store(true, std::memory_order_release);
        }
    }

    bool await_ready() const noexcept {
        return !st || !st->tools || !st->tools->call_tool_async;
    }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        st->coroAddr = h.address();
        st->suspended.store(false, std::memory_order_release);
        st->callbackFired.store(false, std::memory_order_release);
        st->destroyed.store(false, std::memory_order_release);
        auto* holder = new std::shared_ptr<CallToolState>(st);
        char* err    = nullptr;
        st->opHandle = st->tools->call_tool_async(
            st->host,
            agentxx_plugin_sv(st->name.data(), st->name.size()),
            agentxx_plugin_sv(st->argsJson.data(), st->argsJson.size()),
            agentxx_plugin_sv(st->threadId.data(), st->threadId.size()),
            [](void* ud, int cbSt, AgentxxPluginStringView pl) {
                auto* hp = static_cast<std::shared_ptr<CallToolState>*>(ud);
                auto  s  = *hp;
                delete hp;
                s->status = cbSt;
                if (pl.data && pl.size > 0) {
                    s->payload.assign(pl.data, pl.size);
                }
                if (!s->suspended.load(std::memory_order_acquire)) {
                    s->callbackFired.store(true, std::memory_order_release);
                    return;
                }
                if (s->destroyed.load(std::memory_order_acquire)) {
                    return;
                }
                auto  handle = std::coroutine_handle<Promise>::from_address(s->coroAddr);
                auto& prom   = handle.promise();
                prom.clear_outstanding();
                {
                    bool needPost = false;
                    if (s->host) {
                        auto ifs = agentxx::plugin::AgentIfaces::query(s->host);
                        if (ifs.scheduler && ifs.scheduler->is_io_thread) {
                            needPost = !ifs.scheduler->is_io_thread(s->host);
                        }
                    }
                    if (needPost) {
                        auto ifs = agentxx::plugin::AgentIfaces::query(s->host);
                        if (ifs.scheduler && ifs.scheduler->post_to_io) {
                            struct ResumeData {
                                std::coroutine_handle<Promise> h;
                            };
                            auto* d = new ResumeData{handle};
                            ifs.scheduler->post_to_io(
                                s->host,
                                [](void* ud) {
                                    auto* d = static_cast<ResumeData*>(ud);
                                    try {
                                        d->h.resume();
                                    } catch (...) {
                                        d->h.promise().set_exception(std::current_exception());
                                    }
                                    detail::finishIfDone(d->h);
                                    delete d;
                                },
                                d
                            );
                        } else {
                            try {
                                handle.resume();
                            } catch (...) {
                                prom.set_exception(std::current_exception());
                            }
                            finishIfDone(handle);
                        }
                    } else {
                        try {
                            handle.resume();
                        } catch (...) {
                            prom.set_exception(std::current_exception());
                        }
                        finishIfDone(handle);
                    }
                }
            },
            holder,
            &err
        );

        if (st->callbackFired.load(std::memory_order_acquire)) {
            return false;
        }

        if (!st->opHandle && err) {
            st->startError.assign(err);
            if (st->host && st->host->vtable && st->host->vtable->free) {
                st->host->vtable->free(err);
            }
            delete holder;
            return false;
        }

        if (st->opHandle) {
            h.promise().set_outstanding([st = this->st]() {
                if (st->tools && st->tools->op_cancel && st->opHandle) {
                    st->tools->op_cancel(st->opHandle);
                }
            });
        }

        st->suspended.store(true, std::memory_order_release);
        // 竞态窗口：回调可能在上一次 callbackFired 检查之后、suspended 置 true 之前触发，
        // 此时回调已将 callbackFired 置 true 但未恢复协程（因 suspended 当时为 false）。
        // 再次检查，若已触发则不应挂起，由本线程直接返回 false 让 await_resume 处理结果，
        // 并清理 outstanding（回调未清理）。
        if (st->callbackFired.load(std::memory_order_acquire)) {
            h.promise().clear_outstanding();
            return false;
        }
        return true;
    }

    std::string await_resume() {
        if (!st->startError.empty()) {
            throw std::runtime_error(st->startError);
        }
        if (st->status == AGENTXX_PLUGIN_OPERATOR_CANCELLED) {
            throw CancelledException(fmt::format("tool `{}` cancelled", st->name));
        }
        if (st->status != AGENTXX_PLUGIN_OPERATOR_OK) {
            throw std::runtime_error(
                st->payload.empty() ? fmt::format("tool `{}` failed", st->name) : st->payload
            );
        }
        return std::move(st->payload);
    }
};

struct InvokeCapState {
    const AgentxxPluginHost*              host = nullptr;
    const AgentxxPluginCapabilitiesIface* caps = nullptr;
    std::string                           capability;
    std::string                           method;
    std::string                           argsJson;
    AgentxxPluginOperatorHandle*          opHandle = nullptr;
    int                                   status   = AGENTXX_PLUGIN_OPERATOR_OK;
    std::string                           payload;
    std::string                           startError;
    std::atomic<bool>                     suspended{false};
    std::atomic<bool>                     callbackFired{false};
    void*                                 coroAddr     = nullptr;
    void (*schedPost)(const AgentxxPluginHost*, void*) = nullptr;
    std::atomic<bool> destroyed{false};
};

struct InvokeCapAwaiter {
    std::shared_ptr<InvokeCapState> st;

    InvokeCapAwaiter(
        const AgentxxPluginHost*              in_host,
        const AgentxxPluginCapabilitiesIface* in_caps,
        std::string_view                      in_cap,
        std::string_view                      in_method,
        std::string_view                      in_args,
        void (*in_post)(const AgentxxPluginHost*, void*) = nullptr
    ) :
        st(std::make_shared<InvokeCapState>()) {
        st->host       = in_host;
        st->caps       = in_caps;
        st->capability = std::string(in_cap);
        st->method     = std::string(in_method);
        st->argsJson   = std::string(in_args);
        st->schedPost  = in_post;
    }

    ~InvokeCapAwaiter() {
        if (st) {
            st->destroyed.store(true, std::memory_order_release);
        }
    }

    bool await_ready() const noexcept {
        return !st || !st->caps || !st->caps->invoke_capability_async;
    }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        st->coroAddr = h.address();
        st->suspended.store(false, std::memory_order_release);
        st->callbackFired.store(false, std::memory_order_release);
        st->destroyed.store(false, std::memory_order_release);
        auto* holder = new std::shared_ptr<InvokeCapState>(st);
        char* err    = nullptr;
        st->opHandle = st->caps->invoke_capability_async(
            st->host,
            agentxx_plugin_sv(st->capability.data(), st->capability.size()),
            agentxx_plugin_sv(st->method.data(), st->method.size()),
            agentxx_plugin_sv(st->argsJson.data(), st->argsJson.size()),
            [](void* ud, int cbSt, AgentxxPluginStringView pl) {
                auto* hp = static_cast<std::shared_ptr<InvokeCapState>*>(ud);
                auto  s  = *hp;
                delete hp;
                s->status = cbSt;
                if (pl.data && pl.size > 0) {
                    s->payload.assign(pl.data, pl.size);
                }
                if (!s->suspended.load(std::memory_order_acquire)) {
                    s->callbackFired.store(true, std::memory_order_release);
                    return;
                }
                if (s->destroyed.load(std::memory_order_acquire)) {
                    return;
                }
                auto  handle = std::coroutine_handle<Promise>::from_address(s->coroAddr);
                auto& prom   = handle.promise();
                prom.clear_outstanding();
                {
                    bool needPost = false;
                    if (s->host) {
                        auto ifs = agentxx::plugin::AgentIfaces::query(s->host);
                        if (ifs.scheduler && ifs.scheduler->is_io_thread) {
                            needPost = !ifs.scheduler->is_io_thread(s->host);
                        }
                    }
                    if (needPost) {
                        auto ifs = agentxx::plugin::AgentIfaces::query(s->host);
                        if (ifs.scheduler && ifs.scheduler->post_to_io) {
                            struct ResumeData {
                                std::coroutine_handle<Promise> h;
                            };
                            auto* d = new ResumeData{handle};
                            ifs.scheduler->post_to_io(
                                s->host,
                                [](void* ud) {
                                    auto* d = static_cast<ResumeData*>(ud);
                                    try {
                                        d->h.resume();
                                    } catch (...) {
                                        d->h.promise().set_exception(std::current_exception());
                                    }
                                    detail::finishIfDone(d->h);
                                    delete d;
                                },
                                d
                            );
                        } else {
                            try {
                                handle.resume();
                            } catch (...) {
                                prom.set_exception(std::current_exception());
                            }
                            finishIfDone(handle);
                        }
                    } else {
                        try {
                            handle.resume();
                        } catch (...) {
                            prom.set_exception(std::current_exception());
                        }
                        finishIfDone(handle);
                    }
                }
            },
            holder,
            &err
        );

        if (st->callbackFired.load(std::memory_order_acquire)) {
            return false;
        }

        if (!st->opHandle && err) {
            st->startError.assign(err);
            if (st->host && st->host->vtable && st->host->vtable->free) {
                st->host->vtable->free(err);
            }
            delete holder;
            return false;
        }

        if (st->opHandle) {
            h.promise().set_outstanding([st = this->st]() {
                if (st->caps && st->caps->op_cancel && st->opHandle) {
                    st->caps->op_cancel(st->opHandle);
                }
            });
        }

        st->suspended.store(true, std::memory_order_release);
        if (st->callbackFired.load(std::memory_order_acquire)) {
            h.promise().clear_outstanding();
            return false;
        }
        return true;
    }

    std::string await_resume() {
        if (!st->startError.empty()) {
            throw std::runtime_error(st->startError);
        }
        if (st->status == AGENTXX_PLUGIN_OPERATOR_CANCELLED) {
            throw CancelledException(fmt::format("cap `{}` cancelled", st->capability));
        }
        if (st->status != AGENTXX_PLUGIN_OPERATOR_OK) {
            throw std::runtime_error(
                st->payload.empty() ? fmt::format("cap `{}` failed", st->capability) : st->payload
            );
        }
        return std::move(st->payload);
    }
};

} // namespace detail

/* ==================== 锚定原语快捷调用 ==================== */

inline detail::SleepAwaiter sleep(const PluginBase& base, long ms) {
    return detail::SleepAwaiter{base.host, base.iface.scheduler, ms};
}

inline detail::YieldAwaiter yield(const PluginBase& base) {
    return detail::YieldAwaiter{base.host, base.iface.scheduler};
}

template<typename Fn>
inline auto offload(const PluginBase& base, Fn&& fn) {
    detail::OffloadAwaiter<std::decay_t<Fn>> awaiter{
        base.host,
        base.iface.scheduler,
        std::forward<Fn>(fn)
    };
    return awaiter;
}

inline detail::CallToolAwaiter call_tool(
    const PluginBase& base,
    std::string_view  name,
    std::string_view  argsJson,
    std::string_view  threadId = {}
) {
    return detail::CallToolAwaiter(
        base.host,
        base.iface.tools,
        name,
        argsJson,
        threadId,
        [](const AgentxxPluginHost* h, void* addr) {
            auto ifaces = agentxx::plugin::AgentIfaces::query(h);
            if (ifaces.scheduler && ifaces.scheduler->post_to_io) {
                ifaces.scheduler->post_to_io(
                    h,
                    [](void* ud) {
                        auto handle
                            = std::coroutine_handle<detail::PromiseBase<void>>::from_address(ud);
                        handle.resume();
                    },
                    addr
                );
            }
        }
    );
}

inline detail::InvokeCapAwaiter invoke_cap(
    const PluginBase& base,
    std::string_view  cap,
    std::string_view  method,
    std::string_view  argsJson
) {
    return detail::InvokeCapAwaiter(
        base.host,
        base.iface.capabilities,
        cap,
        method,
        argsJson,
        [](const AgentxxPluginHost* h, void* addr) {
            auto ifaces = agentxx::plugin::AgentIfaces::query(h);
            if (ifaces.scheduler && ifaces.scheduler->post_to_io) {
                ifaces.scheduler->post_to_io(
                    h,
                    [](void* ud) {
                        auto handle
                            = std::coroutine_handle<detail::PromiseBase<void>>::from_address(ud);
                        handle.resume();
                    },
                    addr
                );
            }
        }
    );
}

/* ==================== 后台协作任务 spawn 实现 ==================== */
/* spawn 自 API v1 起注册到宿主 agentxx.agent.tasks 接口表 (宿主托管):
 * - 注册: register_task (io 线程) → 宿主句柄入实例 outstandingOps + inflight+1
 * - 运行: 协程照常 (sleep/offload 挂起于宿主); 宿主无感, 句柄静默
 * - 取消/回收: 插件卸载时宿主 detachAll 统一 cancel (→ 本表 cancel_fn:
 *   置 cancelFlag + 唤醒挂起协程) → 协程 while(!cancelled()) 退出 →
 *   finishIfDone (帧销毁后经 notify.done 上报) → 宿主 guard.reset
 *   (inflight-1) + 回收句柄 → waitInflightZero 精确等待归零 → dlclose 安全
 * - 与工具 op 完全同构的完成协议: 复用 PromiseBase::notify_ (finishIfDone
 *   内置上报逻辑) + opCleanup_ (清 coroAddr, 防 cancel_fn 悬空)
 *
 * 线程约束 (实现时须遵守, 对应 docs/agent/plugins.md spawn 节):
 * ① kit 协程完成路径 (finishIfDone 调用点: 各 awaiter 完成闭包/start 尾部)
 *    恒在 io 线程; ② notify.done 宿主侧按任意线程实现 (OpCore::onDone 原子
 *    CAS); ③ rec->coroAddr 仅在 io 线程读写 (cancel_fn 与完成路径都在 io 线程)
 */

namespace detail {

/// spawn 公共实现 (PluginBase::spawn 成员模板与自由函数 spawn 共用, 避免
/// 双份实现漂移)。模板参数:
/// - Ctx: 插件实例上下文 (须含 host/iface/spawns_ 成员)
/// - Fn:  任务协程函数, 签名 (Ctx&, OpCtl) -> Task<void>
template<typename Ctx, typename Fn>
inline void spawnTaskImpl(Ctx& ctx, Fn&& fn) {
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    // rec 先于 starter 创建: starter 在协程挂起后向 rec 记录句柄 (宿主
    // cancel_fn 经它唤醒协程); spawns_ 持 rec 保证 starter post 任务期间存活。
    // 注意: starter 捕获 rec 的 weak_ptr (rec->starter 持有 starter, 若捕获
    // shared_ptr 构成 rec → starter → rec 循环引用, rec 永不释放)
    auto rec = std::make_shared<PluginBase::SpawnRecord>();
    rec->cancelFlag = cancelFlag;
    std::weak_ptr<PluginBase::SpawnRecord> recWeak = rec;

    // 宿主侧任务注册 (io 线程; tasks 接口表缺失/注册失败 → 降级):
    // - 表存在且注册成功: hostNotify 非空, 协程完成 (finishIfDone) 时上报 →
    //   宿主回收句柄 + inflight-1 (卸载闭环)
    // - 表缺失/注册失败: spawn 退化为纯自管协程 (宿主无句柄, 无法被回收)
    //   —— 跨版本固有限制: 插件只能靠自身在 destroy 前停协程 (或进程收尾
    //   OS 回收)。记录 WARN 提示 (不阻塞任务运行)
    AgentxxPluginOperatorNotify hostNotify{nullptr, nullptr};
    if (ctx.iface.tasks && ctx.iface.tasks->register_task) {
        AgentxxPluginOperatorNotify notify;
        char*                       err = nullptr;
        AgentxxPluginOperatorHandle* h   = ctx.iface.tasks->register_task(
            ctx.host,
            [](void* ud, void*) { // cancel_fn: 置 cancelFlag + 唤醒挂起协程
                auto* r = static_cast<PluginBase::SpawnRecord*>(ud);
                if (!r || !r->cancelFlag) {
                    return;
                }
                r->cancelFlag->store(true, std::memory_order_release);
                // coroAddr 仅在 io 线程读写: 完成路径 (opCleanup_) 先 destroy
                // 帧后清空它; 若非空则协程帧必存活 (完成路径尚未 destroy),
                // 唤醒安全 —— 见竞态注 (原子双保险)
                if (r->coroAddr) {
                    auto handle = std::coroutine_handle<detail::PromiseBase<void>>::from_address(
                        r->coroAddr
                    );
                    handle.promise().cancel_outstanding();
                }
            },
            rec.get(),
            &notify,
            &err
        );
        if (h) {
            hostNotify = notify;
        } else {
            if (err) {
                ctx.log.warn(fmt::format(
                    "spawn: register_task failed (task runs unmanaged): {}",
                    err ? err : "?"
                ));
                if (ctx.host && ctx.host->vtable && ctx.host->vtable->free) {
                    ctx.host->vtable->free(err);
                }
            } else {
                ctx.log.warn("spawn: register_task failed (task runs unmanaged)");
            }
        }
    } else {
        ctx.log.warn("spawn: host has no agentxx.agent.tasks iface (task runs unmanaged)");
    }

    auto starter = [&ctx, fn, cancelFlag, recWeak, hostNotify]() {
        OpCtl ctl{cancelFlag, ctx.host, ctx.iface.cancel, ""};
        auto  task = fn(ctx, ctl);
        if (task.handle_) {
            auto h        = task.handle_;
            task.handle_  = nullptr;
            auto& p       = h.promise();
            p.host_       = ctx.host;
            p.cancelFlag_ = cancelFlag;
            p.notify_     = hostNotify; // 完成上报 (仅宿主托管时非空)
            try {
                h.resume();
            } catch (...) {
                p.set_exception(std::current_exception());
            }
            if (!h.done()) {
                // 协程挂起 (等待 sleep/offload 等): 记录句柄供宿主 cancel_fn
                // 唤醒; 此后协程生命周期由各 awaiter 完成闭包接管 (resume +
                // finishIfDone), 实例卸载 (detachAll cancel) 后协程被唤醒 →
                // 感知 cancelFlag 退出, coroAddr 在完成路径清空
                if (auto recSp = recWeak.lock()) {
                    recSp->coroAddr = h.address();
                }
            }
            // 协程完成 (帧将销毁) 时清 coroAddr, 防宿主 cancel_fn 访问已销毁
            // 帧 (cancel_fn 与完成路径同在 io 线程, 无并发; 双保险见竞态注)
            p.opCleanup_ = [recWeak]() {
                if (auto recSp = recWeak.lock()) {
                    recSp->coroAddr = nullptr;
                }
            };
            detail::finishIfDone(h); // 结束时: 帧销毁 → notify.done → 宿主回收
        }
    };
    rec->starter = starter;
    ctx.spawns_.push_back(rec);
    if (ctx.iface.scheduler && ctx.iface.scheduler->post_to_io) {
        auto* raw = rec.get();
        ctx.iface.scheduler->post_to_io(
            ctx.host,
            [](void* ud) {
                auto* rec = static_cast<PluginBase::SpawnRecord*>(ud);
                if (rec && rec->starter) {
                    rec->starter();
                }
            },
            raw
        );
    }
}

} // namespace detail

template<typename Fn>
void PluginBase::spawn(Fn&& fn) {
    detail::spawnTaskImpl(*this, std::forward<Fn>(fn));
}

template<typename Ctx, typename Fn>
inline void spawn(Ctx& ctx, Fn&& fn) {
    detail::spawnTaskImpl(ctx, std::forward<Fn>(fn));
}

/// ==================== (kit::tool / fast_tool / blocking_tool / hook / capability)
/// ====================

template<typename Ctx, typename TaskFn>
inline void tool(
    Ctx&             ctx,
    std::string_view name,
    std::string_view depict,
    std::string_view schema,
    TaskFn&&         fn,
    long             default_timeout_ms = 0,
    int              flags              = 0
) {
    auto&       storage     = ctx.storage_;
    std::string finalDepict = ctx.toolPrompt(name).depict;
    if (finalDepict.empty()) {
        finalDepict = depict;
    }
    storage.push_back(std::move(finalDepict));
    storage.push_back(std::string(schema));

    struct ToolShim {
        Ctx*                               ctx = nullptr;
        std::decay_t<TaskFn>               fn;
        std::shared_ptr<std::atomic<bool>> cancelFlag;
    };

    auto shim = ctx.storeShim(std::make_unique<ToolShim>(
        ToolShim{&ctx, std::forward<TaskFn>(fn), std::make_shared<std::atomic<bool>>(false)}
    ));

    struct Job {
        ToolShim*                          shim;
        std::shared_ptr<std::atomic<bool>> cancelFlag;
        void*                              coroAddr = nullptr;
    };

    AgentxxPluginToolSpec spec{};
    spec.name = agentxx_plugin_sv(name.data(), name.size());
    spec.description
        = agentxx_plugin_sv(storage[storage.size() - 2].data(), storage[storage.size() - 2].size());
    spec.parameters_json    = agentxx_plugin_sv(storage.back().data(), storage.back().size());
    spec.user_data          = shim;
    spec.default_timeout_ms = default_timeout_ms;
    spec.flags              = flags;

    spec.execute_start = [](void*                              user_data,
                            AgentxxPluginStringView            args_json,
                            AgentxxPluginStringView            thread_id,
                            AgentxxPluginStringView            tool_call_id,
                            const AgentxxPluginOperatorNotify* notify,
                            char**                             error_out) -> void* {
        auto* shim = static_cast<ToolShim*>(user_data);
        (void)tool_call_id;
        (void)error_out;
        auto  cancelFlag = std::make_shared<std::atomic<bool>>(false);
        OpCtl ctl{
            cancelFlag,
            shim->ctx->host,
            shim->ctx->iface.cancel,
            std::string(thread_id.data ? thread_id.data : "", thread_id.size)
        };

        std::string_view args(args_json.data ? args_json.data : "{}", args_json.size);
        auto             task = shim->fn(*shim->ctx, args, ctl);
        if (!task.handle_) {
            return nullptr;
        }

        auto h        = task.handle_;
        task.handle_  = nullptr;
        auto& p       = h.promise();
        p.notify_     = notify ? *notify : AgentxxPluginOperatorNotify{nullptr, nullptr};
        p.host_       = shim->ctx->host;
        p.cancelFlag_ = cancelFlag;

        try {
            h.resume();
        } catch (...) {
            p.set_exception(std::current_exception());
        }

        if (h.done()) {
            detail::finishIfDone(h);
            return nullptr;
        }

        auto* job = new Job{shim, cancelFlag, h.address()};
        // op 句柄 (Job) 生命周期绑定到协程: 协程完成 (finishIfDone 销毁帧并
        // 上报 done) 后宿主不再访问 op, 由 opCleanup_ 释放 —— 挂起期间
        // (协程未完成) 宿主 execute_cancel 仍经 op 指针访问 Job, 必须存活
        p.opCleanup_ = [job]() {
            delete job;
        };
        return job;
    };

    spec.execute_cancel = [](void* user_data, void* op) {
        (void)user_data;
        if (!op) {
            return;
        }
        auto* job = static_cast<Job*>(op);
        if (job->cancelFlag) {
            job->cancelFlag->store(true, std::memory_order_release);
        }
        if (job->coroAddr) {
            auto handle
                = std::coroutine_handle<detail::PromiseBase<void>>::from_address(job->coroAddr);
            handle.promise().cancel_outstanding();
        }
    };

    if (ctx.iface.tools && ctx.iface.tools->register_tool) {
        ctx.iface.tools->register_tool(ctx.host, &spec);
    }
}

template<typename Ctx, typename SyncFn>
inline void fast_tool(
    Ctx&             ctx,
    std::string_view name,
    std::string_view depict,
    std::string_view schema,
    SyncFn&&         fn,
    long             default_timeout_ms = 0,
    int              flags              = 0
) {
    auto&       storage     = ctx.storage_;
    std::string finalDepict = ctx.toolPrompt(name).depict;
    if (finalDepict.empty()) {
        finalDepict = depict;
    }
    storage.push_back(std::move(finalDepict));
    storage.push_back(std::string(schema));

    struct FastShim {
        Ctx*                 ctx = nullptr;
        std::decay_t<SyncFn> fn;
    };

    auto shim = ctx.storeShim(std::make_unique<FastShim>(FastShim{&ctx, std::forward<SyncFn>(fn)}));

    AgentxxPluginToolSpec spec{};
    spec.name = agentxx_plugin_sv(name.data(), name.size());
    spec.description
        = agentxx_plugin_sv(storage[storage.size() - 2].data(), storage[storage.size() - 2].size());
    spec.parameters_json    = agentxx_plugin_sv(storage.back().data(), storage.back().size());
    spec.user_data          = shim;
    spec.default_timeout_ms = default_timeout_ms;
    spec.flags              = flags;

    spec.execute_start = [](void*                              user_data,
                            AgentxxPluginStringView            args_json,
                            AgentxxPluginStringView            thread_id,
                            AgentxxPluginStringView            tool_call_id,
                            const AgentxxPluginOperatorNotify* notify,
                            char**                             error_out) -> void* {
        auto* shim = static_cast<FastShim*>(user_data);
        (void)tool_call_id;
        try {
            std::string_view args(args_json.data ? args_json.data : "{}", args_json.size);
            std::string_view tid(thread_id.data ? thread_id.data : "", thread_id.size);
            std::string      res;
            if constexpr (std::is_invocable_v<SyncFn, Ctx&, std::string_view, std::string_view>) {
                res = shim->fn(*shim->ctx, args, tid);
            } else if constexpr (std::is_invocable_v<SyncFn, Ctx&, std::string_view>) {
                res = shim->fn(*shim->ctx, args);
            } else {
                res = shim->fn(args);
            }

            if (notify && notify->done) {
                notify->done(
                    notify->host_ud,
                    AGENTXX_PLUGIN_OPERATOR_OK,
                    agentxx_plugin_sv(res.data(), res.size())
                );
            }
        } catch (const std::exception& e) {
            if (notify && notify->done) {
                std::string what = e.what();
                notify->done(
                    notify->host_ud,
                    AGENTXX_PLUGIN_OPERATOR_FAILED,
                    agentxx_plugin_sv(what.data(), what.size())
                );
            } else if (error_out) {
                *error_out = shim->ctx->strdup(e.what());
            }
        } catch (...) {
            if (notify && notify->done) {
                notify->done(
                    notify->host_ud,
                    AGENTXX_PLUGIN_OPERATOR_FAILED,
                    agentxx_plugin_sv_cstr("unknown error")
                );
            } else if (error_out) {
                *error_out = shim->ctx->strdup("unknown error in fast_tool");
            }
        }
        return nullptr;
    };

    spec.execute_cancel = nullptr;

    if (ctx.iface.tools && ctx.iface.tools->register_tool) {
        ctx.iface.tools->register_tool(ctx.host, &spec);
    }
}

template<typename Ctx, typename BlockFn>
inline void blocking_tool(
    Ctx&             ctx,
    std::string_view name,
    std::string_view depict,
    std::string_view schema,
    BlockFn&&        fn,
    long             default_timeout_ms = 0,
    int              flags              = 0
) {
    auto&       storage     = ctx.storage_;
    std::string finalDepict = ctx.toolPrompt(name).depict;
    if (finalDepict.empty()) {
        finalDepict = depict;
    }
    storage.push_back(std::move(finalDepict));
    storage.push_back(std::string(schema));

    struct BlockShim {
        Ctx*                  ctx = nullptr;
        std::decay_t<BlockFn> fn;
    };

    auto shim
        = ctx.storeShim(std::make_unique<BlockShim>(BlockShim{&ctx, std::forward<BlockFn>(fn)}));

    struct Job {
        BlockShim*                  shim;
        AgentxxPluginOperatorNotify notify;
        std::string                 args;
        std::string                 tid;
        std::string                 tcid;
        std::string workDir; ///< 预取的会话工作目录（io 线程解析，避免 worker 跨线程 ioCallSync）
        volatile int cancelFlag = 0;
    };

    AgentxxPluginToolSpec spec{};
    spec.name = agentxx_plugin_sv(name.data(), name.size());
    spec.description
        = agentxx_plugin_sv(storage[storage.size() - 2].data(), storage[storage.size() - 2].size());
    spec.parameters_json    = agentxx_plugin_sv(storage.back().data(), storage.back().size());
    spec.user_data          = shim;
    spec.default_timeout_ms = default_timeout_ms;
    spec.flags              = flags;

    spec.execute_start = [](void*                              user_data,
                            AgentxxPluginStringView            args_json,
                            AgentxxPluginStringView            thread_id,
                            AgentxxPluginStringView            tool_call_id,
                            const AgentxxPluginOperatorNotify* notify,
                            char**                             error_out) -> void* {
        auto* shim = static_cast<BlockShim*>(user_data);
        (void)error_out;
        std::string tidStr(thread_id.data ? thread_id.data : "", thread_id.size);
        // 预取 workDir（io 线程），避免阻塞池线程跨线程 ioCallSync;
        // 空 tid 时 get_session_work_dir 返回默认会话工作目录
        std::string workDirCache;
        if (shim->ctx) {
            workDirCache = shim->ctx->workDir(agentxx_plugin_sv(tidStr.data(), tidStr.size()));
        }
        auto* job = new Job{
            shim,
            notify ? *notify : AgentxxPluginOperatorNotify{nullptr, nullptr},
            std::string(args_json.data ? args_json.data : "{}", args_json.size),
            std::move(tidStr),
            std::string(tool_call_id.data ? tool_call_id.data : "", tool_call_id.size),
            std::move(workDirCache),
            0
        };

        if (shim->ctx->iface.scheduler && shim->ctx->iface.scheduler->offload) {
            shim->ctx->iface.scheduler->offload(
                shim->ctx->host,
                &job->cancelFlag,
                [](void* ud, volatile int* cflag, char** err_out) -> void* {
                    auto* j = static_cast<Job*>(ud);
                    try {
                        std::string res;
                        // 优先匹配含 workDir 的新签名，避免 worker 再次 ioCallSync
                        if constexpr (std::is_invocable_v<
                                          BlockFn,
                                          Ctx&,
                                          std::string_view,
                                          std::string_view,
                                          std::string_view,
                                          volatile int*>) {
                            res = j->shim->fn(*j->shim->ctx, j->args, j->tid, j->workDir, cflag);
                        } else if constexpr (std::is_invocable_v<
                                                 BlockFn,
                                                 Ctx&,
                                                 std::string_view,
                                                 std::string_view,
                                                 volatile int*>) {
                            res = j->shim->fn(*j->shim->ctx, j->args, j->tid, cflag);
                        } else if constexpr (std::is_invocable_v<
                                                 BlockFn,
                                                 Ctx&,
                                                 std::string_view,
                                                 std::string_view,
                                                 std::string_view>) {
                            res = j->shim->fn(*j->shim->ctx, j->args, j->tid, j->workDir);
                        } else if constexpr (std::is_invocable_v<
                                                 BlockFn,
                                                 Ctx&,
                                                 std::string_view,
                                                 volatile int*>) {
                            res = j->shim->fn(*j->shim->ctx, j->args, cflag);
                        } else if constexpr (std::is_invocable_v<
                                                 BlockFn,
                                                 std::string_view,
                                                 volatile int*>) {
                            res = j->shim->fn(j->args, cflag);
                        } else if constexpr (std::is_invocable_v<
                                                 BlockFn,
                                                 Ctx&,
                                                 std::string_view,
                                                 std::string_view>) {
                            res = j->shim->fn(*j->shim->ctx, j->args, j->tid);
                        } else if constexpr (std::is_invocable_v<BlockFn, Ctx&, std::string_view>) {
                            res = j->shim->fn(*j->shim->ctx, j->args);
                        } else {
                            res = j->shim->fn(j->args);
                        }
                        return j->shim->ctx->strdup(res.c_str());
                    } catch (const CancelledException&) {
                        return nullptr;
                    } catch (const std::exception& e) {
                        if (err_out) {
                            *err_out = j->shim->ctx->strdup(e.what());
                        }
                        return nullptr;
                    } catch (...) {
                        if (err_out) {
                            *err_out = j->shim->ctx->strdup("unknown blocking tool error");
                        }
                        return nullptr;
                    }
                },
                [](void* ud, void* res, AgentxxPluginStringView err) {
                    auto* j       = static_cast<Job*>(ud);
                    int   st      = AGENTXX_PLUGIN_OPERATOR_OK;
                    AgentxxPluginStringView payload = agentxx_plugin_sv(nullptr, 0);

                    if (!agentxx_plugin_sv_empty(err)) {
                        st      = AGENTXX_PLUGIN_OPERATOR_FAILED;
                        payload = err;
                    } else if (res) {
                        char* resCstr = static_cast<char*>(res);
                        payload = agentxx_plugin_sv_cstr(resCstr);
                    } else {
                        st = AGENTXX_PLUGIN_OPERATOR_CANCELLED;
                    }

                    if (j->notify.done) {
                        j->notify.done(j->notify.host_ud, st, payload);
                    }
                    if (res && j->shim && j->shim->ctx) {
                        j->shim->ctx->free(static_cast<char*>(res));
                    }
                    delete j;
                },
                job
            );
        }
        return job;
    };

    spec.execute_cancel = [](void* user_data, void* op) {
        (void)user_data;
        if (!op) {
            return;
        }
        auto* job       = static_cast<Job*>(op);
        job->cancelFlag = 1;
    };

    if (ctx.iface.tools && ctx.iface.tools->register_tool) {
        ctx.iface.tools->register_tool(ctx.host, &spec);
    }
}

template<typename Ctx, typename HookFn>
inline void hook(Ctx& ctx, AgentxxPluginHookPoint point, HookFn&& fn) {
    struct HookShim {
        Ctx*                 ctx = nullptr;
        std::decay_t<HookFn> fn;
    };

    auto shim = ctx.storeShim(std::make_unique<HookShim>(HookShim{&ctx, std::forward<HookFn>(fn)}));

    AgentxxPluginHookSpec spec{};
    spec.point     = point;
    spec.user_data = shim;

    spec.hook_start = [](void*                              user_data,
                         AgentxxPluginHookPoint             pt,
                         AgentxxPluginStringView            node_input_json,
                         const AgentxxPluginOperatorNotify* notify,
                         char**                             error_out) -> void* {
        auto* shim = static_cast<HookShim*>(user_data);
        (void)error_out;
        try {
            std::string_view input(
                node_input_json.data ? node_input_json.data : "{}",
                node_input_json.size
            );
            if constexpr (std::is_invocable_v<
                              HookFn,
                              Ctx&,
                              AgentxxPluginHookPoint,
                              std::string_view>) {
                shim->fn(*shim->ctx, pt, input);
            } else if constexpr (std::is_invocable_v<HookFn, Ctx&, std::string_view>) {
                shim->fn(*shim->ctx, input);
            } else {
                shim->fn(input);
            }
            if (notify && notify->done) {
                notify->done(
                    notify->host_ud,
                    AGENTXX_PLUGIN_OPERATOR_OK,
                    agentxx_plugin_sv(nullptr, 0)
                );
            }
        } catch (const std::exception& e) {
            if (notify && notify->done) {
                std::string what = e.what();
                notify->done(
                    notify->host_ud,
                    AGENTXX_PLUGIN_OPERATOR_FAILED,
                    agentxx_plugin_sv(what.data(), what.size())
                );
            }
        } catch (...) {
            if (notify && notify->done) {
                notify->done(
                    notify->host_ud,
                    AGENTXX_PLUGIN_OPERATOR_FAILED,
                    agentxx_plugin_sv_cstr("hook error")
                );
            }
        }
        return nullptr;
    };

    spec.hook_cancel = nullptr;

    if (ctx.iface.hooks && ctx.iface.hooks->register_hook) {
        ctx.iface.hooks->register_hook(ctx.host, &spec);
    }
}

template<typename Ctx, typename CapFn>
inline void capability(Ctx& ctx, std::string_view capName, CapFn&& fn) {
    struct CapShim {
        Ctx*                ctx = nullptr;
        std::decay_t<CapFn> fn;
    };

    auto shim = ctx.storeShim(std::make_unique<CapShim>(CapShim{&ctx, std::forward<CapFn>(fn)}));

    if (ctx.iface.capabilities && ctx.iface.capabilities->register_capability_ex) {
        ctx.iface.capabilities->register_capability_ex(
            ctx.host,
            agentxx_plugin_sv(capName.data(), capName.size()),
            [](void*                              user_data,
               const AgentxxPluginHost*           caller_host,
               AgentxxPluginStringView            method,
               AgentxxPluginStringView            args_json,
               const AgentxxPluginOperatorNotify* notify,
               char**                             error_out) -> void* {
                auto* shim = static_cast<CapShim*>(user_data);
                (void)error_out;
                try {
                    std::string_view meth(method.data ? method.data : "", method.size);
                    std::string_view args(args_json.data ? args_json.data : "{}", args_json.size);
                    std::string      res;
                    if constexpr (std::is_invocable_v<
                                      CapFn,
                                      Ctx&,
                                      const AgentxxPluginHost*,
                                      std::string_view,
                                      std::string_view>) {
                        res = shim->fn(*shim->ctx, caller_host, meth, args);
                    } else if constexpr (std::is_invocable_v<
                                             CapFn,
                                             Ctx&,
                                             std::string_view,
                                             std::string_view>) {
                        res = shim->fn(*shim->ctx, meth, args);
                    } else {
                        res = shim->fn(meth, args);
                    }
                    if (notify && notify->done) {
                        notify->done(
                            notify->host_ud,
                            AGENTXX_PLUGIN_OPERATOR_OK,
                            agentxx_plugin_sv(res.data(), res.size())
                        );
                    }
                } catch (const std::exception& e) {
                    if (notify && notify->done) {
                        std::string what = e.what();
                        notify->done(
                            notify->host_ud,
                            AGENTXX_PLUGIN_OPERATOR_FAILED,
                            agentxx_plugin_sv(what.data(), what.size())
                        );
                    }
                } catch (...) {
                    if (notify && notify->done) {
                        notify->done(
                            notify->host_ud,
                            AGENTXX_PLUGIN_OPERATOR_FAILED,
                            agentxx_plugin_sv_cstr("capability error")
                        );
                    }
                }
                return nullptr;
            },
            nullptr,
            shim
        );
    }
}

/* ==================== 阻塞便捷函数 (基于 condvar) ==================== */

inline char* call_tool_blocking(
    const AgentxxPluginHost*           host,
    const AgentxxPluginToolsIface*     tools,
    const AgentxxPluginSchedulerIface* sched,
    std::string_view                   name,
    std::string_view                   args_json,
    std::string_view                   thread_id,
    char**                             error_out
) {
    if (!host || !tools || !tools->call_tool_async) {
        if (error_out && host && host->vtable && host->vtable->strdup) {
            *error_out = host->vtable->strdup(agentxx_plugin_sv_cstr("tools iface not available"));
        }
        return nullptr;
    }
    if (sched && sched->is_io_thread && sched->is_io_thread(host)) {
        if (error_out && host->vtable && host->vtable->strdup) {
            *error_out = host->vtable->strdup(agentxx_plugin_sv_cstr(
                "call_tool_blocking cannot be called on io thread; use co_await call_tool instead"
            ));
        }
        return nullptr;
    }

    struct SyncState {
        std::mutex              mtx;
        std::condition_variable cv;
        bool                    done   = false;
        int                     status = AGENTXX_PLUGIN_OPERATOR_OK;
        std::string             payload;
    } state;

    AgentxxPluginOperatorHandle* handle = tools->call_tool_async(
        host,
        agentxx_plugin_sv(name.data(), name.size()),
        agentxx_plugin_sv(args_json.data(), args_json.size()),
        agentxx_plugin_sv(thread_id.data(), thread_id.size()),
        [](void* ud, int st, AgentxxPluginStringView pl) {
            auto*           s = static_cast<SyncState*>(ud);
            std::lock_guard lk(s->mtx);
            s->done   = true;
            s->status = st;
            if (pl.data && pl.size > 0) {
                s->payload.assign(pl.data, pl.size);
            }
            s->cv.notify_one();
        },
        &state,
        error_out
    );

    if (!handle) {
        return nullptr;
    }

    {
        std::unique_lock lk(state.mtx);
        state.cv.wait(lk, [&]() {
            return state.done;
        });
    }

    if (state.status != AGENTXX_PLUGIN_OPERATOR_OK) {
        if (error_out && host && host->vtable && host->vtable->strdup) {
            *error_out = host->vtable->strdup(
                agentxx_plugin_sv(state.payload.data(), state.payload.size())
            );
        }
        return nullptr;
    }

    return host && host->vtable && host->vtable->strdup
               ? host->vtable->strdup(agentxx_plugin_sv(state.payload.data(), state.payload.size()))
               : detail::strdupFallback(agentxx_plugin_sv(state.payload.data(), state.payload.size()));
}

inline char* invoke_capability_blocking(
    const AgentxxPluginHost*              host,
    const AgentxxPluginCapabilitiesIface* caps,
    const AgentxxPluginSchedulerIface*    sched,
    std::string_view                      capability,
    std::string_view                      method,
    std::string_view                      args_json,
    char**                                error_out
) {
    if (!host || !caps || !caps->invoke_capability_async) {
        if (error_out && host && host->vtable && host->vtable->strdup) {
            *error_out = host->vtable->strdup(agentxx_plugin_sv_cstr("capabilities iface not available"));
        }
        return nullptr;
    }
    if (sched && sched->is_io_thread && sched->is_io_thread(host)) {
        if (error_out && host->vtable && host->vtable->strdup) {
            *error_out = host->vtable->strdup(agentxx_plugin_sv_cstr(
                "invoke_capability_blocking cannot be called on io thread; use co_await invoke_cap instead"
            ));
        }
        return nullptr;
    }

    struct SyncState {
        std::mutex              mtx;
        std::condition_variable cv;
        bool                    done   = false;
        int                     status = AGENTXX_PLUGIN_OPERATOR_OK;
        std::string             payload;
    } state;

    AgentxxPluginOperatorHandle* handle = caps->invoke_capability_async(
        host,
        agentxx_plugin_sv(capability.data(), capability.size()),
        agentxx_plugin_sv(method.data(), method.size()),
        agentxx_plugin_sv(args_json.data(), args_json.size()),
        [](void* ud, int st, AgentxxPluginStringView pl) {
            auto*           s = static_cast<SyncState*>(ud);
            std::lock_guard lk(s->mtx);
            s->done   = true;
            s->status = st;
            if (pl.data && pl.size > 0) {
                s->payload.assign(pl.data, pl.size);
            }
            s->cv.notify_one();
        },
        &state,
        error_out
    );

    if (!handle) {
        return nullptr;
    }

    {
        std::unique_lock lk(state.mtx);
        state.cv.wait(lk, [&]() {
            return state.done;
        });
    }

    if (state.status != AGENTXX_PLUGIN_OPERATOR_OK) {
        if (error_out && host && host->vtable && host->vtable->strdup) {
            *error_out = host->vtable->strdup(
                agentxx_plugin_sv(state.payload.data(), state.payload.size())
            );
        }
        return nullptr;
    }

    return host && host->vtable && host->vtable->strdup
               ? host->vtable->strdup(agentxx_plugin_sv(state.payload.data(), state.payload.size()))
               : detail::strdupFallback(agentxx_plugin_sv(state.payload.data(), state.payload.size()));
}

} // namespace plugin
} // namespace agentxx
