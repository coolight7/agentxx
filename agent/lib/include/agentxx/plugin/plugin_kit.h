/*
 * agentxx/plugin/plugin_kit.h —— 插件开发 SDK (C++ header-only)
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

#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_guard.h"
#include "agentxx/plugin/plugin_iface_helper.h"
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
namespace kit {

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

/* ==================== Task<T> 锚定协程与完成协议 ==================== */

template<typename T = void>
struct Task;

namespace detail {

template<typename Promise>
void finishIfDone(std::coroutine_handle<Promise> h) {
    if (!h || !h.done()) {
        return;
    }
    auto&                       p           = h.promise();
    AgentxxPluginOperatorNotify notify      = p.notify_;
    const AgentxxPluginHost*    host        = p.host_;
    int                         status      = AGENTXX_PLUGIN_OPERATOR_OK;
    char*                       payload_out = nullptr;

    if (p.has_exception()) {
        status = AGENTXX_PLUGIN_OPERATOR_FAILED;
        try {
            std::rethrow_exception(p.exception());
        } catch (const CancelledException&) {
            status = AGENTXX_PLUGIN_OPERATOR_CANCELLED;
        } catch (const std::exception& e) {
            payload_out = host && host->vtable && host->vtable->strdup
                              ? host->vtable->strdup(e.what())
                              : ::strdup(e.what());
        } catch (...) {
            payload_out = host && host->vtable && host->vtable->strdup
                              ? host->vtable->strdup("unknown task error")
                              : ::strdup("unknown task error");
        }
    } else {
        if constexpr (!std::is_void_v<typename Promise::value_type>) {
            std::string res = p.result_string();
            payload_out     = host && host->vtable && host->vtable->strdup
                                  ? host->vtable->strdup(res.c_str())
                                  : ::strdup(res.c_str());
        }
    }

    h.destroy();

    if (notify.done) {
        notify.done(notify.host_ud, status, payload_out);
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

    std::string workDir(AgentxxPluginStringView tid = {}) const {
        if (!host || !iface.config) {
            return "";
        }
        if (iface.config->get_session_work_dir && !agentxx_plugin_sv_empty(tid)) {
            char* p = iface.config->get_session_work_dir(host, tid);
            if (p) {
                std::string res(p);
                host->vtable->free(p);
                if (!res.empty()) {
                    return res;
                }
            }
        }
        if (iface.config->get_work_dir) {
            char* p = iface.config->get_work_dir(host);
            if (p) {
                std::string res(p);
                host->vtable->free(p);
                return res;
            }
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

    char* strdup(const char* s) const {
        if (!host || !host->vtable || !host->vtable->strdup || !s) {
            return nullptr;
        }
        return host->vtable->strdup(s);
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

    // 后台任务记录
    // 堆稳定地址：spawn 时以 shared_ptr 存于 vector，post_to_io 传递堆地址
    // 避免 vector 扩容导致 &spawns_.back() 悬垂（A）
    struct SpawnRecord {
        std::function<void()>              starter;
        std::shared_ptr<std::atomic<bool>> cancelFlag;
    };

    std::vector<std::shared_ptr<SpawnRecord>> spawns_;

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
            [](void* ud, void* res, char* err) {
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
            [](void* ud, int cbSt, char* pl) {
                auto* hp = static_cast<std::shared_ptr<CallToolState>*>(ud);
                auto  s  = *hp;
                delete hp;
                s->status = cbSt;
                if (pl) {
                    s->payload.assign(pl);
                    if (s->host && s->host->vtable && s->host->vtable->free) {
                        s->host->vtable->free(pl);
                    }
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
                            struct ResumeData { std::coroutine_handle<Promise> h; };
                            auto* d = new ResumeData{handle};
                            ifs.scheduler->post_to_io(
                                s->host,
                                [](void* ud) {
                                    auto* d = static_cast<ResumeData*>(ud);
                                    try { d->h.resume(); } catch (...) { d->h.promise().set_exception(std::current_exception()); }
                                    detail::finishIfDone(d->h); delete d;
                                }, d);
                        } else {
                            try { handle.resume(); } catch (...) { prom.set_exception(std::current_exception()); }
                            finishIfDone(handle);
                        }
                    } else {
                        try { handle.resume(); } catch (...) { prom.set_exception(std::current_exception()); }
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
            [](void* ud, int cbSt, char* pl) {
                auto* hp = static_cast<std::shared_ptr<InvokeCapState>*>(ud);
                auto  s  = *hp;
                delete hp;
                s->status = cbSt;
                if (pl) {
                    s->payload.assign(pl);
                    if (s->host && s->host->vtable && s->host->vtable->free) {
                        s->host->vtable->free(pl);
                    }
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
                            struct ResumeData { std::coroutine_handle<Promise> h; };
                            auto* d = new ResumeData{handle};
                            ifs.scheduler->post_to_io(
                                s->host,
                                [](void* ud) {
                                    auto* d = static_cast<ResumeData*>(ud);
                                    try { d->h.resume(); } catch (...) { d->h.promise().set_exception(std::current_exception()); }
                                    detail::finishIfDone(d->h); delete d;
                                }, d);
                        } else {
                            try { handle.resume(); } catch (...) { prom.set_exception(std::current_exception()); }
                            finishIfDone(handle);
                        }
                    } else {
                        try { handle.resume(); } catch (...) { prom.set_exception(std::current_exception()); }
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

template<typename Fn>
void PluginBase::spawn(Fn&& fn) {
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    auto starter    = [this, fn, cancelFlag]() {
        OpCtl ctl{cancelFlag, this->host, this->iface.cancel, ""};
        auto  task = fn(*this, ctl);
        if (task.handle_) {
            auto h        = task.handle_;
            task.handle_  = nullptr;
            auto& p       = h.promise();
            p.host_       = this->host;
            p.cancelFlag_ = cancelFlag;
            try {
                h.resume();
            } catch (...) {
                p.set_exception(std::current_exception());
            }
            detail::finishIfDone(h);
        }
    };

    auto rec = std::make_shared<SpawnRecord>(SpawnRecord{starter, cancelFlag});
    spawns_.push_back(rec);
    if (iface.scheduler && iface.scheduler->post_to_io) {
        SpawnRecord* raw = rec.get();
        iface.scheduler->post_to_io(
            host,
            [](void* ud) {
                auto* rec = static_cast<SpawnRecord*>(ud);
                if (rec && rec->starter) {
                    rec->starter();
                }
            },
            raw
        );
    }
}

template<typename Ctx, typename Fn>
inline void spawn(Ctx& ctx, Fn&& fn) {
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    auto starter    = [&ctx, fn, cancelFlag]() {
        OpCtl ctl{cancelFlag, ctx.host, ctx.iface.cancel, ""};
        auto  task = fn(ctx, ctl);
        if (task.handle_) {
            auto h        = task.handle_;
            task.handle_  = nullptr;
            auto& p       = h.promise();
            p.host_       = ctx.host;
            p.cancelFlag_ = cancelFlag;
            try {
                h.resume();
            } catch (...) {
                p.set_exception(std::current_exception());
            }
            detail::finishIfDone(h);
        }
    };

    auto rec
        = std::make_shared<PluginBase::SpawnRecord>(PluginBase::SpawnRecord{starter, cancelFlag});
    ctx.spawns_.push_back(rec);
    if (ctx.iface.scheduler && ctx.iface.scheduler->post_to_io) {
        PluginBase::SpawnRecord* raw = rec.get();
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

            char* payload = shim->ctx->strdup(res.c_str());
            if (notify && notify->done) {
                notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, payload);
            }
        } catch (const std::exception& e) {
            if (notify && notify->done) {
                notify->done(
                    notify->host_ud,
                    AGENTXX_PLUGIN_OPERATOR_FAILED,
                    shim->ctx->strdup(e.what())
                );
            } else if (error_out) {
                *error_out = shim->ctx->strdup(e.what());
            }
        } catch (...) {
            if (notify && notify->done) {
                notify->done(
                    notify->host_ud,
                    AGENTXX_PLUGIN_OPERATOR_FAILED,
                    shim->ctx->strdup("unknown error")
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
        // B3: 预取 workDir（io 线程），避免阻塞池线程跨线程 ioCallSync
        std::string workDirCache;
        if (!tidStr.empty() && shim->ctx) {
            workDirCache = shim->ctx->workDir(agentxx_plugin_sv(tidStr.data(), tidStr.size()));
        } else if (shim->ctx) {
            workDirCache = shim->ctx->workDir();
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
                [](void* ud, void* res, char* err) {
                    auto* j       = static_cast<Job*>(ud);
                    int   st      = AGENTXX_PLUGIN_OPERATOR_OK;
                    char* payload = static_cast<char*>(res);

                    if (err) {
                        st      = AGENTXX_PLUGIN_OPERATOR_FAILED;
                        payload = err;
                    } else if (!payload) {
                        st = AGENTXX_PLUGIN_OPERATOR_CANCELLED;
                    }

                    if (j->notify.done) {
                        j->notify.done(j->notify.host_ud, st, payload);
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
                notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
            }
        } catch (const std::exception& e) {
            if (notify && notify->done) {
                notify->done(
                    notify->host_ud,
                    AGENTXX_PLUGIN_OPERATOR_FAILED,
                    shim->ctx->strdup(e.what())
                );
            }
        } catch (...) {
            if (notify && notify->done) {
                notify->done(
                    notify->host_ud,
                    AGENTXX_PLUGIN_OPERATOR_FAILED,
                    shim->ctx->strdup("hook error")
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
                    char* payload = shim->ctx->strdup(res.c_str());
                    if (notify && notify->done) {
                        notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, payload);
                    }
                } catch (const std::exception& e) {
                    if (notify && notify->done) {
                        notify->done(
                            notify->host_ud,
                            AGENTXX_PLUGIN_OPERATOR_FAILED,
                            shim->ctx->strdup(e.what())
                        );
                    }
                } catch (...) {
                    if (notify && notify->done) {
                        notify->done(
                            notify->host_ud,
                            AGENTXX_PLUGIN_OPERATOR_FAILED,
                            shim->ctx->strdup("capability error")
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
            *error_out = host->vtable->strdup("tools iface not available");
        }
        return nullptr;
    }
    if (sched && sched->is_io_thread && sched->is_io_thread(host)) {
        if (error_out && host->vtable && host->vtable->strdup) {
            *error_out = host->vtable->strdup(
                "call_tool_blocking cannot be called on io thread; use co_await call_tool instead"
            );
        }
        return nullptr;
    }

    struct SyncState {
        std::mutex              mtx;
        std::condition_variable cv;
        bool                    done    = false;
        int                     status  = AGENTXX_PLUGIN_OPERATOR_OK;
        char*                   payload = nullptr;
    } state;

    AgentxxPluginOperatorHandle* handle = tools->call_tool_async(
        host,
        agentxx_plugin_sv(name.data(), name.size()),
        agentxx_plugin_sv(args_json.data(), args_json.size()),
        agentxx_plugin_sv(thread_id.data(), thread_id.size()),
        [](void* ud, int st, char* pl) {
            auto*           s = static_cast<SyncState*>(ud);
            std::lock_guard lk(s->mtx);
            s->done    = true;
            s->status  = st;
            s->payload = pl;
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
        if (error_out && state.payload) {
            *error_out = state.payload;
        } else if (state.payload && host && host->vtable && host->vtable->free) {
            host->vtable->free(state.payload);
        }
        return nullptr;
    }

    return state.payload;
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
            *error_out = host->vtable->strdup("capabilities iface not available");
        }
        return nullptr;
    }
    if (sched && sched->is_io_thread && sched->is_io_thread(host)) {
        if (error_out && host->vtable && host->vtable->strdup) {
            *error_out = host->vtable->strdup(
                "invoke_capability_blocking cannot be called on io thread; use co_await invoke_cap instead"
            );
        }
        return nullptr;
    }

    struct SyncState {
        std::mutex              mtx;
        std::condition_variable cv;
        bool                    done    = false;
        int                     status  = AGENTXX_PLUGIN_OPERATOR_OK;
        char*                   payload = nullptr;
    } state;

    AgentxxPluginOperatorHandle* handle = caps->invoke_capability_async(
        host,
        agentxx_plugin_sv(capability.data(), capability.size()),
        agentxx_plugin_sv(method.data(), method.size()),
        agentxx_plugin_sv(args_json.data(), args_json.size()),
        [](void* ud, int st, char* pl) {
            auto*           s = static_cast<SyncState*>(ud);
            std::lock_guard lk(s->mtx);
            s->done    = true;
            s->status  = st;
            s->payload = pl;
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
        if (error_out && state.payload) {
            *error_out = state.payload;
        } else if (state.payload && host && host->vtable && host->vtable->free) {
            host->vtable->free(state.payload);
        }
        return nullptr;
    }

    return state.payload;
}

} // namespace kit
} // namespace agentxx
