/// 插件开发 SDK (C++ header-only)
///
/// 命名空间: 全部位于 agentxx::plugin
///
/// 锚定协程模型 SDK:
/// - PluginBase: 实例上下文基类, 集中常用宿主操作 (workDir / toolPrompt / log 等)
/// - Logger: 实例级日志闭包, 消除进程级全局
/// - Task<T>: 极简锚定协程类型 (无外部执行器依赖, 帧先销毁后 done 上报)
/// - 锚定原语 awaiter 族: sleep / yield / offload / call_tool / invoke_cap
/// - 注册族: tool (Task协程) / fast_tool (快同步内联) / blocking_tool (阻塞池委托) / hook /
/// capability
/// - spawn: 后台协作任务 (sleep 循环, 卸载取消)
/// - 阻塞便捷助手: 供 JS 引擎及非 io 线程使用 (基于 condvar)
#pragma once
#include "agentxx/plugin/api/client_plugin_api.h"

#include "agentxx/plugin/api/plugin_api.h"
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

namespace agentxx {
namespace plugin {

/* ==================== C++ 字符串/接口便捷工具 (非 ABI) ====================
 *
 * [plugin_api.h](/agent/lib/include/agentxx/plugin/api/plugin_api.h) 为纯 C ABI
 * (跨边界契约), 其结构体在 C++ 下仅带最小便捷成员
 * (构造/empty, 不改变布局)。面向宿主/插件 C++ 源码的字符串与接口操作集中
 * 在本命名空间:
 * - PluginStringView: 字符串视图便捷工具 (纯静态函数集合, 不持有状态;
 *   操作/返回跨边界 ABI 类型 AgentxxPluginStringView/AgentxxPluginString)
 * - PluginString: 宿主堆字符串 RAII (接管 AgentxxPluginString 生命周期;
 *   析构自动经 host->vtable->free 释放)
 * - queryInterface<Iface>: 接口表查询模板 (替代旧 AGENTXX_PLUGIN_QUERY_IFACE 宏)
 *
 * 历史: 这些能力曾以全局函数/宏形式内联于纯 C ABI 头 plugin_api.h, 因按值
 * 返回含 C++ 成员函数的 struct 触发 MSVC C4190;
 */

/// 字符串视图便捷工具 (纯静态函数; 不构造对象)
struct PluginStringView {
    /// 从 (指针, 长度) 构造 ABI 视图 (原 agentxx_plugin_sv)
    static AgentxxPluginStringView from(const char* s, uint64_t n) noexcept {
        return AgentxxPluginStringView{s, n};
    }

    /// 从 NUL 结尾 C 串构造 ABI 视图 (自动 strlen; 原 agentxx_plugin_sv_cstr)
    static AgentxxPluginStringView fromCstr(const char* s) noexcept {
        return AgentxxPluginStringView{s, s ? static_cast<uint64_t>(std::strlen(s)) : 0};
    }

    /// 从 std::string_view 构造 ABI 视图
    static AgentxxPluginStringView from(std::string_view s) noexcept {
        return AgentxxPluginStringView{s.data(), static_cast<uint64_t>(s.size())};
    }

    /// ABI 视图是否为空 (原 agentxx_plugin_sv_empty)
    static bool empty(const AgentxxPluginStringView& sv) noexcept {
        return sv.data == nullptr || sv.size == 0;
    }

    /// 指针重载 (兼容旧调用形态; NULL 视为空)
    static bool empty(const AgentxxPluginStringView* sv) noexcept {
        return sv == nullptr || sv->data == nullptr || sv->size == 0;
    }

    /// 宿主堆字符串是否为空 (原 agentxx_plugin_string_empty)
    static bool empty(const AgentxxPluginString& s) noexcept {
        return s.data == nullptr || s.size == 0;
    }

    /// 指针重载 (NULL 视为空)
    static bool empty(const AgentxxPluginString* s) noexcept {
        return s == nullptr || s->data == nullptr || s->size == 0;
    }

    /// ABI 视图 → std::string_view (零拷贝; NULL data 视为空串)
    static std::string_view str(const AgentxxPluginStringView& sv) noexcept {
        return sv.data ? std::string_view{sv.data, static_cast<size_t>(sv.size)}
                       : std::string_view{};
    }

    /// 宿主堆字符串 → ABI 视图 (原 agentxx_plugin_string_to_sv)
    static AgentxxPluginStringView toSv(const AgentxxPluginString& s) noexcept {
        return AgentxxPluginStringView{s.data, s.size};
    }

    /// 指针重载 (NULL 视为空视图)
    static AgentxxPluginStringView toSv(const AgentxxPluginString* s) noexcept {
        return s ? AgentxxPluginStringView{s->data, s->size} : AgentxxPluginStringView{};
    }
};

/// 宿主堆字符串 RAII (原 OwnedString; 析构自动释放; move-only)
class PluginString {
    const AgentxxPluginHost* host_ = nullptr;
    AgentxxPluginString      str_{nullptr, 0};

public:

    PluginString() = default;

    PluginString(const AgentxxPluginHost* h, AgentxxPluginString s) noexcept :
        host_(h),
        str_(s) {}

    ~PluginString() {
        reset();
    }

    PluginString(const PluginString&)            = delete;
    PluginString& operator=(const PluginString&) = delete;

    PluginString(PluginString&& o) noexcept :
        host_(o.host_),
        str_(o.str_) {
        o.host_ = nullptr;
        o.str_  = {nullptr, 0};
    }

    PluginString& operator=(PluginString&& o) noexcept {
        if (this != &o) {
            reset();
            host_   = o.host_;
            str_    = o.str_;
            o.host_ = nullptr;
            o.str_  = {nullptr, 0};
        }
        return *this;
    }

    /// 接管宿主出参 (fn 以 AgentxxPluginString* 出参填充后接管所有权)
    template<typename Fn>
    static PluginString acquire(const AgentxxPluginHost* h, Fn&& fn) {
        AgentxxPluginString s{nullptr, 0};
        fn(&s);
        return PluginString(h, s);
    }

    /// 经宿主 alloc 拷贝视图 → ABI 宿主串 (原 agentxx_plugin_string_from_sv;
    /// 返回裸 ABI 串, 调用方负责释放 (PluginString::free / 移入 PluginString RAII))
    static AgentxxPluginString from(const AgentxxPluginHost* h, const AgentxxPluginStringView* sv) {
        AgentxxPluginString res{nullptr, 0};
        if (!h || !h->vtable || !h->vtable->alloc || !sv || (!sv->data && sv->size == 0)) {
            return res;
        }
        char* p = static_cast<char*>(h->vtable->alloc(sv->size + 1));
        if (p) {
            if (sv->size > 0 && sv->data) {
                std::memcpy(p, sv->data, static_cast<size_t>(sv->size));
            }
            p[sv->size] = '\0';
            res.data    = p;
            res.size    = sv->size;
        }
        return res;
    }

    /// 引用重载
    static AgentxxPluginString from(const AgentxxPluginHost* h, const AgentxxPluginStringView& sv) {
        return from(h, &sv);
    }

    /// std::string_view 重载
    static AgentxxPluginString from(const AgentxxPluginHost* h, std::string_view sv) {
        auto svAbi = PluginStringView::from(sv.data(), sv.size());
        return from(h, &svAbi);
    }

    /// 从 std::string_view 经宿主 alloc 构造 RAII 对象
    static PluginString create(const AgentxxPluginHost* h, std::string_view sv) {
        return PluginString(h, from(h, sv));
    }

    /// 从 ABI 视图经宿主 alloc 构造 RAII 对象
    static PluginString create(const AgentxxPluginHost* h, const AgentxxPluginStringView& sv) {
        return PluginString(h, from(h, &sv));
    }

    /// 经宿主 alloc 拷贝 C 串 → ABI 宿主串 (原 agentxx_plugin_string_from_cstr)
    static AgentxxPluginString fromCstr(const AgentxxPluginHost* h, const char* s) {
        if (!h || !s) {
            return AgentxxPluginString{nullptr, 0};
        }
        return from(h, PluginStringView::fromCstr(s));
    }

    /// 从 C 串经宿主 alloc 构造 RAII 对象
    static PluginString createCstr(const AgentxxPluginHost* h, const char* s) {
        return PluginString(h, fromCstr(h, s));
    }

    /// 经宿主 alloc 拷贝视图为裸 char* (原 agentxx_plugin_strdup; 调用方负责 free)
    static char* strdup(const AgentxxPluginHost* h, const AgentxxPluginStringView* sv) {
        if (!h || !h->vtable || !h->vtable->alloc || !sv || (!sv->data && sv->size == 0)) {
            return nullptr;
        }
        char* p = static_cast<char*>(h->vtable->alloc(sv->size + 1));
        if (p) {
            if (sv->size > 0 && sv->data) {
                std::memcpy(p, sv->data, static_cast<size_t>(sv->size));
            }
            p[sv->size] = '\0';
        }
        return p;
    }

    /// 引用重载
    static char* strdup(const AgentxxPluginHost* h, const AgentxxPluginStringView& sv) {
        return strdup(h, &sv);
    }

    /// std::string_view 重载
    static char* strdup(const AgentxxPluginHost* h, std::string_view sv) {
        auto svAbi = PluginStringView::from(sv.data(), sv.size());
        return strdup(h, &svAbi);
    }

    /// C 串重载 (原宏 AGENTXX_PLUGIN_STRDUP)
    static char* strdup(const AgentxxPluginHost* h, const char* s) {
        if (!h || !s) {
            return nullptr;
        }
        return strdup(h, PluginStringView::fromCstr(s));
    }

    /// 释放宿主堆串 (原 agentxx_plugin_string_free; 幂等并清空)
    static void free(const AgentxxPluginHost* h, AgentxxPluginString* s) noexcept {
        if (s && s->data) {
            if (h && h->vtable && h->vtable->free) {
                h->vtable->free(s->data);
            }
            s->data = nullptr;
            s->size = 0;
        }
    }

    /// 释放本对象持有的串并复位
    void reset() noexcept {
        if (host_ && str_.data) {
            PluginString::free(host_, &str_);
        }
        host_ = nullptr;
        str_  = {nullptr, 0};
    }

    const char* c_str() const noexcept {
        return str_.data ? str_.data : "";
    }

    const char* data() const noexcept {
        return str_.data;
    }

    size_t size() const noexcept {
        return static_cast<size_t>(str_.size);
    }

    bool empty() const noexcept {
        return PluginStringView::empty(str_);
    }

    std::string_view view() const noexcept {
        return PluginStringView::str(PluginStringView::toSv(str_));
    }

    std::string str() const {
        return std::string(view());
    }

    AgentxxPluginStringView to_sv() const noexcept {
        return PluginStringView::toSv(str_);
    }

    AgentxxPluginString release() noexcept {
        AgentxxPluginString tmp = str_;
        str_                    = {nullptr, 0};
        host_                   = nullptr;
        return tmp;
    }

    const AgentxxPluginString& raw() const noexcept {
        return str_;
    }
};

/// 查询宿主接口表并转型 (原 AGENTXX_PLUGIN_QUERY_IFACE 宏)
template<typename Iface>
const Iface* queryInterface(const AgentxxPluginHost* host, std::string_view iid) noexcept {
    if (!host || !host->vtable || !host->vtable->query_interface || iid.empty()) {
        return nullptr;
    }
    AgentxxPluginStringView sv = PluginStringView::from(iid.data(), iid.size());
    return static_cast<const Iface*>(host->vtable->query_interface(host, &sv));
}

template<typename Iface>
const Iface*
    queryInterface(const AgentxxPluginHost* host, const AgentxxPluginStringView& iid) noexcept {
    if (!host || !host->vtable || !host->vtable->query_interface || iid.empty()) {
        return nullptr;
    }
    return static_cast<const Iface*>(host->vtable->query_interface(host, &iid));
}

template<typename Iface>
const Iface* queryInterface(const AgentxxPluginHost* host, const char* iid) noexcept {
    if (!host || !host->vtable || !host->vtable->query_interface || !iid) {
        return nullptr;
    }
    AgentxxPluginStringView sv = PluginStringView::fromCstr(iid);
    return static_cast<const Iface*>(host->vtable->query_interface(host, &sv));
}

/* ==================== 接口表聚合 (原 plugin_iface_helper.h 实体, 并入 kit) ==================== */

/// agent 侧接口表聚合 (一次查询; 成员为 NULL 表示宿主未实现该接口)
struct AgentIfaces {
    const AgentxxPluginToolsIface*        tools        = nullptr; ///< "agentxx.agent.tools"
    const AgentxxPluginHooksIface*        hooks        = nullptr; ///< "agentxx.agent.hooks"
    const AgentxxPluginEventsIface*       events       = nullptr; ///< "agentxx.agent.events"
    const AgentxxPluginCapabilitiesIface* capabilities = nullptr; ///< "agentxx.agent.capabilities"
    const AgentxxPluginSchedulerIface*    scheduler    = nullptr; ///< "agentxx.agent.scheduler"
    const AgentxxPluginSessionIface*      session      = nullptr; ///< "agentxx.agent.session"
    const AgentxxPluginsIface*            plugins      = nullptr; ///< "agentxx.agent.plugins"
    const AgentxxPluginConfigIface*       config       = nullptr; ///< "agentxx.agent.config"
    const AgentxxPluginPromptIface*       prompt       = nullptr; ///< "agentxx.agent.prompt"
    const AgentxxPluginJsonIface*         json         = nullptr; ///< "agentxx.agent.json"
    const AgentxxPluginLogIface*          log          = nullptr; ///< "agentxx.agent.log"
    const AgentxxPluginResourcesIface*    resources    = nullptr; ///< "agentxx.agent.resources"
    const AgentxxPluginModelIface*        model        = nullptr; ///< "agentxx.agent.model"
    const AgentxxPluginCancelIface*       cancel       = nullptr; ///< "agentxx.agent.cancel"
    const AgentxxPluginGraphIface*        graph        = nullptr; ///< "agentxx.agent.graph"
    const AgentxxPluginTasksIface*        tasks        = nullptr; ///< "agentxx.agent.tasks"

    /// 从宿主查询全部已知 agent 侧接口表 (host 为空时返回全 NULL 聚合)
    static AgentIfaces query(const AgentxxPluginHost* host) {
        AgentIfaces f;
        if (!host || !host->vtable || !host->vtable->query_interface) {
            return f;
        }
        f.tools = queryInterface<AgentxxPluginToolsIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_TOOLS);
        f.hooks = queryInterface<AgentxxPluginHooksIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_HOOKS);
        f.events
            = queryInterface<AgentxxPluginEventsIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_EVENTS);
        f.capabilities = queryInterface<AgentxxPluginCapabilitiesIface>(
            host,
            AGENTXX_PLUGIN_IFACE_AGENT_CAPABILITIES
        );
        f.scheduler = queryInterface<AgentxxPluginSchedulerIface>(
            host,
            AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER
        );
        f.session
            = queryInterface<AgentxxPluginSessionIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_SESSION);
        f.plugins = queryInterface<AgentxxPluginsIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_PLUGINS);
        f.config
            = queryInterface<AgentxxPluginConfigIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_CONFIG);
        f.prompt
            = queryInterface<AgentxxPluginPromptIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_PROMPT);
        f.json      = queryInterface<AgentxxPluginJsonIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_JSON);
        f.log       = queryInterface<AgentxxPluginLogIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_LOG);
        f.resources = queryInterface<AgentxxPluginResourcesIface>(
            host,
            AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES
        );
        f.model = queryInterface<AgentxxPluginModelIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_MODEL);
        f.cancel
            = queryInterface<AgentxxPluginCancelIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_CANCEL);
        f.graph = queryInterface<AgentxxPluginGraphIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_GRAPH);
        f.tasks = queryInterface<AgentxxPluginTasksIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_TASKS);
        return f;
    }
};

/// client 侧接口表聚合 (一次查询; 成员为 NULL 表示宿主未实现该接口)
struct ClientIfaces {
    const AgentxxClientUiIface*      ui      = nullptr; ///< "agentxx.client.ui"
    const AgentxxClientEventsIface*  events  = nullptr; ///< "agentxx.client.events"
    const AgentxxClientSessionIface* session = nullptr; ///< "agentxx.client.session"
    const AgentxxClientWireIface*    wire    = nullptr; ///< "agentxx.client.wire"
    const AgentxxClientSelfIface*    self    = nullptr; ///< "agentxx.client.self"
    const AgentxxClientJsonIface*    json    = nullptr; ///< "agentxx.client.json"
    const AgentxxClientLogIface*     log     = nullptr; ///< "agentxx.client.log"

    /// 从宿主查询全部已知 client 侧接口表 (host 为空时返回全 NULL 聚合)
    static ClientIfaces query(const AgentxxPluginHost* host) {
        ClientIfaces f;
        if (!host || !host->vtable || !host->vtable->query_interface) {
            return f;
        }
        f.ui      = queryInterface<AgentxxClientUiIface>(host, AGENTXX_IFACE_CLIENT_UI);
        f.events  = queryInterface<AgentxxClientEventsIface>(host, AGENTXX_IFACE_CLIENT_EVENTS);
        f.session = queryInterface<AgentxxClientSessionIface>(host, AGENTXX_IFACE_CLIENT_SESSION);
        f.wire    = queryInterface<AgentxxClientWireIface>(host, AGENTXX_IFACE_CLIENT_WIRE);
        f.self    = queryInterface<AgentxxClientSelfIface>(host, AGENTXX_IFACE_CLIENT_SELF);
        f.json    = queryInterface<AgentxxClientJsonIface>(host, AGENTXX_IFACE_CLIENT_JSON);
        f.log     = queryInterface<AgentxxClientLogIface>(host, AGENTXX_IFACE_CLIENT_LOG);
        return f;
    }
};

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

    void log(int32_t level, std::string_view msg) const noexcept {
        if (host && logIface && logIface->log) {
            auto sv = PluginStringView::from(msg.data(), msg.size());
            logIface->log(host, level, &sv);
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
            auto sv = PluginStringView::from(threadId.data(), threadId.size());
            return cancelIface->is_cancelled(host, &sv) != 0;
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

inline char* strdupFallback(const AgentxxPluginStringView* s) {
    if (!s || (!s->data && s->size == 0)) {
        return nullptr;
    }
    char* p = static_cast<char*>(std::malloc(static_cast<size_t>(s->size + 1)));
    if (p) {
        if (s->size > 0 && s->data) {
            std::memcpy(p, s->data, static_cast<size_t>(s->size));
        }
        p[s->size] = '\0';
    }
    return p;
}

template<typename Promise>
inline void finishIfDone(std::coroutine_handle<Promise> h) {
    if (!h.done()) {
        return;
    }
    auto& p = h.promise();

    int32_t     status = AGENTXX_PLUGIN_OPERATOR_OK;
    std::string errPayload;
    std::string resPayload;
    if (p.has_exception()) {
        status = AGENTXX_PLUGIN_OPERATOR_FAILED;
        try {
            std::rethrow_exception(p.exception());
        } catch (const CancelledException& e) {
            status     = AGENTXX_PLUGIN_OPERATOR_CANCELLED;
            errPayload = e.what();
        } catch (const std::exception& e) {
            errPayload = e.what();
        } catch (...) {
            errPayload = "unknown exception in coroutine";
        }
    } else if (p.cancelFlag_ && p.cancelFlag_->load(std::memory_order_acquire)) {
        status = AGENTXX_PLUGIN_OPERATOR_CANCELLED;
    } else {
        if constexpr (!std::is_void_v<typename Promise::value_type>) {
            if constexpr (std::is_same_v<typename Promise::value_type, std::string>) {
                resPayload = p.result();
            } else {
                resPayload = fmt::format("{}", p.result());
            }
        }
    }

    AgentxxPluginOperatorNotify notify  = p.notify_;
    auto                        cleanup = std::move(p.opCleanup_);
    p.opCleanup_                        = nullptr;

    h.destroy();

    if (notify.done) {
        AgentxxPluginStringView sv = PluginStringView::from(nullptr, 0);
        if (status == AGENTXX_PLUGIN_OPERATOR_FAILED) {
            sv = PluginStringView::from(errPayload.data(), errPayload.size());
        } else if (status == AGENTXX_PLUGIN_OPERATOR_OK) {
            sv = PluginStringView::from(resPayload.data(), resPayload.size());
        }
        notify.done(notify.host_ud, status, &sv);
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
    std::function<void()>              opCleanup_{nullptr};

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
        std::optional<T> res_;

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        template<typename U>
        void return_value(U&& v) {
            res_.emplace(std::forward<U>(v));
        }

        T& result() {
            return *res_;
        }
    };

    std::coroutine_handle<promise_type> handle_;

    explicit Task(std::coroutine_handle<promise_type> h) :
        handle_(h) {}

    Task(Task&& o) noexcept :
        handle_(std::exchange(o.handle_, {})) {}

    Task& operator=(Task&& o) noexcept {
        if (this != &o) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(o.handle_, {});
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
        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        void return_void() noexcept {}
    };

    std::coroutine_handle<promise_type> handle_;

    explicit Task(std::coroutine_handle<promise_type> h) :
        handle_(h) {}

    Task(Task&& o) noexcept :
        handle_(std::exchange(o.handle_, {})) {}

    Task& operator=(Task&& o) noexcept {
        if (this != &o) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(o.handle_, {});
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

/* ==================== 插件实例上下文基类 ==================== */

class PluginBase {
public:

    const AgentxxPluginHost* host = nullptr;
    AgentIfaces              iface;
    Logger                   log;

    virtual ~PluginBase() = default;

    void init(const AgentxxPluginHost* h) {
        host         = h;
        iface        = AgentIfaces::query(h);
        log.host     = h;
        log.logIface = iface.log;
    }

    std::string config() const {
        if (!host || !iface.config || !iface.config->get_config) {
            return "{}";
        }
        AgentxxPluginString s{nullptr, 0};
        iface.config->get_config(host, &s);
        if (!s.data) {
            return "{}";
        }
        std::string res(s.data, static_cast<size_t>(s.size));
        PluginString::free(host, &s);
        return res;
    }

    std::string workDir(AgentxxPluginStringView tid = {}) const {
        if (!host || !iface.config || !iface.config->get_session_work_dir) {
            return "";
        }
        AgentxxPluginString s{nullptr, 0};
        iface.config->get_session_work_dir(host, &tid, &s);
        if (s.data) {
            std::string res(s.data, static_cast<size_t>(s.size));
            PluginString::free(host, &s);
            return res;
        }
        return "";
    }

    std::string workDir(std::string_view tid) const {
        return workDir(PluginStringView::from(tid.data(), tid.size()));
    }

    ToolPromptText toolPrompt(std::string_view tool) const {
        ToolPromptText res;
        if (!host || !iface.config || !iface.config->get_tool_prompt) {
            return res;
        }
        auto                toolSv = PluginStringView::from(tool.data(), tool.size());
        AgentxxPluginString s{nullptr, 0};
        iface.config->get_tool_prompt(host, &toolSv, &s);
        if (!s.data) {
            return res;
        }
        std::string jsonStr(s.data, static_cast<size_t>(s.size));
        PluginString::free(host, &s);
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
        AgentxxPluginString s{nullptr, 0};
        iface.config->get_plugin_args(host, &s);
        if (!s.data) {
            return "{}";
        }
        std::string res(s.data, static_cast<size_t>(s.size));
        PluginString::free(host, &s);
        return res;
    }

    std::string configPath() const {
        if (!host || !iface.config || !iface.config->get_plugin_config_path) {
            return "";
        }
        AgentxxPluginString s{nullptr, 0};
        iface.config->get_plugin_config_path(host, &s);
        if (!s.data) {
            return "";
        }
        std::string res(s.data, static_cast<size_t>(s.size));
        PluginString::free(host, &s);
        return res;
    }

    std::string language() const {
        if (!host || !iface.config || !iface.config->get_language) {
            return "en";
        }
        AgentxxPluginString s{nullptr, 0};
        if (iface.config->get_language(host, &s) == 0 && s.data) {
            std::string res(s.data, static_cast<size_t>(s.size));
            PluginString::free(host, &s);
            return res;
        }
        return "en";
    }

    bool setLanguage(std::string_view lang) const {
        if (!host || !iface.config || !iface.config->set_language) {
            return false;
        }
        auto langSv = PluginStringView::from(lang.data(), lang.size());
        return iface.config->set_language(host, &langSv) == 0;
    }

    bool sessionCancelled(AgentxxPluginStringView tid) const {
        if (!host || !iface.cancel || !iface.cancel->is_cancelled) {
            return false;
        }
        return iface.cancel->is_cancelled(host, &tid) != 0;
    }

    bool sessionCancelled(std::string_view tid) const {
        return sessionCancelled(PluginStringView::from(tid.data(), tid.size()));
    }

    int64_t addShareStore(AgentxxPluginStringView tid, std::string_view content) const {
        if (!host || !iface.session || !iface.session->add_share_store) {
            return -1;
        }
        auto contentSv = PluginStringView::from(content.data(), content.size());
        return iface.session->add_share_store(host, &tid, &contentSv);
    }

    int64_t addShareStore(std::string_view tid, std::string_view content) const {
        return addShareStore(PluginStringView::from(tid.data(), tid.size()), content);
    }

    char* strdup(AgentxxPluginStringView sv) const {
        return PluginString::strdup(host, &sv);
    }

    char* strdup(std::string_view sv) const {
        return PluginString::strdup(host, sv);
    }

    char* strdup(const char* s) const {
        if (!s) {
            return nullptr;
        }
        auto sv = PluginStringView::fromCstr(s);
        return PluginString::strdup(host, &sv);
    }

    AgentxxPluginString createString(AgentxxPluginStringView sv) const {
        return PluginString::from(host, &sv);
    }

    AgentxxPluginString createString(std::string_view sv) const {
        return PluginString::from(host, sv);
    }

    PluginString createPluginString(AgentxxPluginStringView sv) const {
        return PluginString::create(host, sv);
    }

    PluginString createPluginString(std::string_view sv) const {
        return PluginString::create(host, sv);
    }

    /// 字符串 → JSON 字符串字面量 (经宿主 agentxx.agent.json 接口表; 含引号包裹与转义)
    std::string jsonEscape(AgentxxPluginStringView s) const {
        if (!host || !iface.json || !iface.json->json_escape || (!s.data && s.size == 0)) {
            return "\"\"";
        }
        AgentxxPluginString esc{nullptr, 0};
        auto                sSv = PluginStringView::from(s.data, s.size);
        iface.json->json_escape(host, &sSv, &esc);
        if (!esc.data) {
            return "\"\"";
        }
        std::string out(esc.data, static_cast<size_t>(esc.size));
        PluginString::free(host, &esc);
        return out;
    }

    std::string jsonEscape(std::string_view s) const {
        return jsonEscape(PluginStringView::from(s.data(), s.size()));
    }

    std::string jsonEscape(const char* s) const {
        if (!s) {
            return "\"\"";
        }
        return jsonEscape(std::string_view{s});
    }

    /// 从 JSON 中提取 key 的字符串值 (经宿主 json 接口表; 不存在返回空)
    std::string jsonGetString(std::string_view json, std::string_view key) const {
        if (!host || !iface.json || !iface.json->json_get_string) {
            return {};
        }
        auto                jsonSv = PluginStringView::from(json.data(), json.size());
        auto                keySv  = PluginStringView::from(key.data(), key.size());
        AgentxxPluginString out{nullptr, 0};
        iface.json->json_get_string(host, &jsonSv, &keySv, &out);
        if (!out.data) {
            return {};
        }
        std::string res(out.data, static_cast<size_t>(out.size));
        PluginString::free(host, &out);
        return res;
    }

    std::vector<std::string> storage_;

    std::vector<std::unique_ptr<void, void (*)(void*)>> shims_;

    template<typename T>
    T* storeShim(std::unique_ptr<T> shim) {
        T* raw = shim.get();
        shims_.emplace_back(shim.release(), [](void* ptr) {
            delete static_cast<T*>(ptr);
        });
        return raw;
    }

    struct SpawnRecord {
        std::function<void()>              starter;
        std::shared_ptr<std::atomic<bool>> cancelFlag;
        void*                              coroAddr = nullptr;
    };

    std::vector<std::shared_ptr<SpawnRecord>> spawns_;

    void stopSpawns() {
        for (auto& rec : spawns_) {
            if (!rec || !rec->cancelFlag) {
                continue;
            }
            rec->cancelFlag->store(true, std::memory_order_release);
            if (rec->coroAddr) {
                auto handle
                    = std::coroutine_handle<detail::PromiseBase<void>>::from_address(rec->coroAddr);
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
    int64_t                            ms;
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
                auto handle = std::coroutine_handle<Promise>::from_address(ud);
                try {
                    handle.resume();
                } catch (...) {
                    handle.promise().set_exception(std::current_exception());
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
    using ResultType = std::decay_t<std::invoke_result_t<WorkFn, volatile int32_t*>>;

    const AgentxxPluginHost*           host;
    const AgentxxPluginSchedulerIface* sched;
    WorkFn                             work;
    volatile int32_t                   cancelFlag = 0;
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
            [](void* ud, volatile int32_t* cflag, AgentxxPluginString* error_out) -> void* {
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
            [](void* ud, void* res, const AgentxxPluginStringView* err) {
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
    int32_t                        status   = AGENTXX_PLUGIN_OPERATOR_OK;
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
        auto*               holder = new std::shared_ptr<CallToolState>(st);
        AgentxxPluginString err{nullptr, 0};
        auto                nameSv = PluginStringView::from(st->name.data(), st->name.size());
        auto argsSv  = PluginStringView::from(st->argsJson.data(), st->argsJson.size());
        auto tidSv   = PluginStringView::from(st->threadId.data(), st->threadId.size());
        st->opHandle = st->tools->call_tool_async(
            st->host,
            &nameSv,
            &argsSv,
            &tidSv,
            [](void* ud, int32_t cbSt, const AgentxxPluginStringView* pl) {
                auto* hp = static_cast<std::shared_ptr<CallToolState>*>(ud);
                auto  s  = *hp;
                delete hp;
                s->status = cbSt;
                if (pl && pl->data && pl->size > 0) {
                    s->payload.assign(pl->data, static_cast<size_t>(pl->size));
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

        if (!st->opHandle) {
            delete holder;
            if (err.data) {
                st->startError.assign(err.data, static_cast<size_t>(err.size));
                PluginString::free(st->host, &err);
            }
            return false;
        }

        if (st->callbackFired.load(std::memory_order_acquire)) {
            return false;
        }

        st->suspended.store(true, std::memory_order_release);
        h.promise().set_outstanding([st = this->st]() {
            if (st->tools && st->tools->op_cancel && st->opHandle) {
                st->tools->op_cancel(st->opHandle);
            }
        });
        return true;
    }

    std::string await_resume() {
        if (!st->startError.empty()) {
            throw std::runtime_error("call_tool start failed: " + st->startError);
        }
        if (st->status == AGENTXX_PLUGIN_OPERATOR_CANCELLED) {
            throw CancelledException(st->payload.empty() ? "call_tool cancelled" : st->payload);
        }
        if (st->status != AGENTXX_PLUGIN_OPERATOR_OK) {
            throw std::runtime_error(st->payload.empty() ? "call_tool failed" : st->payload);
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
    int32_t                               status   = AGENTXX_PLUGIN_OPERATOR_OK;
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
        auto*               holder = new std::shared_ptr<InvokeCapState>(st);
        AgentxxPluginString err{nullptr, 0};
        auto capSv   = PluginStringView::from(st->capability.data(), st->capability.size());
        auto methSv  = PluginStringView::from(st->method.data(), st->method.size());
        auto argsSv  = PluginStringView::from(st->argsJson.data(), st->argsJson.size());
        st->opHandle = st->caps->invoke_capability_async(
            st->host,
            &capSv,
            &methSv,
            &argsSv,
            [](void* ud, int32_t cbSt, const AgentxxPluginStringView* pl) {
                auto* hp = static_cast<std::shared_ptr<InvokeCapState>*>(ud);
                auto  s  = *hp;
                delete hp;
                s->status = cbSt;
                if (pl && pl->data && pl->size > 0) {
                    s->payload.assign(pl->data, static_cast<size_t>(pl->size));
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

        if (!st->opHandle) {
            delete holder;
            if (err.data) {
                st->startError.assign(err.data, static_cast<size_t>(err.size));
                PluginString::free(st->host, &err);
            }
            return false;
        }

        if (st->callbackFired.load(std::memory_order_acquire)) {
            return false;
        }

        st->suspended.store(true, std::memory_order_release);
        h.promise().set_outstanding([st = this->st]() {
            if (st->caps && st->caps->op_cancel && st->opHandle) {
                st->caps->op_cancel(st->opHandle);
            }
        });
        return true;
    }

    std::string await_resume() {
        if (!st->startError.empty()) {
            throw std::runtime_error("invoke_capability start failed: " + st->startError);
        }
        if (st->status == AGENTXX_PLUGIN_OPERATOR_CANCELLED) {
            throw CancelledException(
                st->payload.empty() ? "invoke_capability cancelled" : st->payload
            );
        }
        if (st->status != AGENTXX_PLUGIN_OPERATOR_OK) {
            throw std::runtime_error(
                st->payload.empty() ? "invoke_capability failed" : st->payload
            );
        }
        return std::move(st->payload);
    }
};

} // namespace detail

inline detail::SleepAwaiter sleep(const PluginBase& ctx, int64_t ms) noexcept {
    return detail::SleepAwaiter{ctx.host, ctx.iface.scheduler, ms};
}

inline detail::YieldAwaiter yield(const PluginBase& ctx) noexcept {
    return detail::YieldAwaiter{ctx.host, ctx.iface.scheduler};
}

template<typename WorkFn>
inline auto offload(const PluginBase& ctx, WorkFn&& work) {
    return detail::OffloadAwaiter<std::decay_t<WorkFn>>{
        ctx.host,
        ctx.iface.scheduler,
        std::forward<WorkFn>(work)
    };
}

inline detail::CallToolAwaiter call_tool(
    const PluginBase& ctx,
    std::string_view  name,
    std::string_view  argsJson,
    std::string_view  threadId = {}
) {
    return detail::CallToolAwaiter{
        ctx.host,
        ctx.iface.tools,
        name,
        argsJson,
        threadId,
        [](const AgentxxPluginHost* h, void* addr) {
            auto ifs = agentxx::plugin::AgentIfaces::query(h);
            if (ifs.scheduler && ifs.scheduler->post_to_io) {
                ifs.scheduler->post_to_io(
                    h,
                    [](void* ud) {
                        auto handle
                            = std::coroutine_handle<detail::PromiseBase<std::string>>::from_address(
                                ud
                            );
                        handle.resume();
                    },
                    addr
                );
            }
        }
    };
}

inline detail::InvokeCapAwaiter invoke_cap(
    const PluginBase& ctx,
    std::string_view  capability,
    std::string_view  method,
    std::string_view  argsJson = "{}"
) {
    return detail::InvokeCapAwaiter{
        ctx.host,
        ctx.iface.capabilities,
        capability,
        method,
        argsJson,
        [](const AgentxxPluginHost* h, void* addr) {
            auto ifs = agentxx::plugin::AgentIfaces::query(h);
            if (ifs.scheduler && ifs.scheduler->post_to_io) {
                ifs.scheduler->post_to_io(
                    h,
                    [](void* ud) {
                        auto handle
                            = std::coroutine_handle<detail::PromiseBase<std::string>>::from_address(
                                ud
                            );
                        handle.resume();
                    },
                    addr
                );
            }
        }
    };
}

/* ==================== 后台协作任务 spawn 实现 ==================== */

namespace detail {

template<typename Ctx, typename Fn>
inline void spawnTaskImpl(Ctx& ctx, Fn&& fn) {
    auto cancelFlag                                = std::make_shared<std::atomic<bool>>(false);
    auto rec                                       = std::make_shared<PluginBase::SpawnRecord>();
    rec->cancelFlag                                = cancelFlag;
    std::weak_ptr<PluginBase::SpawnRecord> recWeak = rec;

    AgentxxPluginOperatorNotify hostNotify{nullptr, nullptr};
    if (ctx.iface.tasks && ctx.iface.tasks->register_task) {
        AgentxxPluginOperatorNotify  notify{nullptr, nullptr};
        AgentxxPluginString          err{nullptr, 0};
        AgentxxPluginOperatorHandle* h = ctx.iface.tasks->register_task(
            ctx.host,
            [](void* ud, void*) {
                auto* r = static_cast<PluginBase::SpawnRecord*>(ud);
                if (!r || !r->cancelFlag) {
                    return;
                }
                r->cancelFlag->store(true, std::memory_order_release);
                if (r->coroAddr) {
                    auto handle
                        = std::coroutine_handle<PromiseBase<void>>::from_address(r->coroAddr);
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
            if (err.data) {
                ctx.log.warn(fmt::format(
                    "spawn: register_task failed (task runs unmanaged): {}",
                    std::string_view{err.data, static_cast<size_t>(err.size)}
                ));
                if (ctx.host) {
                    PluginString::free(ctx.host, &err);
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
            p.notify_     = hostNotify;
            try {
                h.resume();
            } catch (...) {
                p.set_exception(std::current_exception());
            }
            if (!h.done()) {
                if (auto recSp = recWeak.lock()) {
                    recSp->coroAddr = h.address();
                }
            }
            p.opCleanup_ = [recWeak]() {
                if (auto recSp = recWeak.lock()) {
                    recSp->coroAddr = nullptr;
                }
            };
            detail::finishIfDone(h);
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
    int64_t          default_timeout_ms = 0,
    int32_t          flags              = 0
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
    spec.name        = PluginStringView::from(name.data(), name.size());
    spec.description = PluginStringView::from(
        storage[storage.size() - 2].data(),
        storage[storage.size() - 2].size()
    );
    spec.parameters_json    = PluginStringView::from(storage.back().data(), storage.back().size());
    spec.user_data          = shim;
    spec.default_timeout_ms = default_timeout_ms;
    spec.flags              = flags;
    spec._reserved          = 0;

    spec.execute_start = [](void*                              user_data,
                            const AgentxxPluginStringView*     args_json,
                            const AgentxxPluginStringView*     thread_id,
                            const AgentxxPluginStringView*     tool_call_id,
                            const AgentxxPluginOperatorNotify* notify,
                            AgentxxPluginString*               error_out) -> void* {
        auto* shim = static_cast<ToolShim*>(user_data);
        (void)tool_call_id;
        (void)error_out;
        auto  cancelFlag = std::make_shared<std::atomic<bool>>(false);
        OpCtl ctl{
            cancelFlag,
            shim->ctx->host,
            shim->ctx->iface.cancel,
            std::string(
                thread_id && thread_id->data ? thread_id->data : "",
                thread_id ? static_cast<size_t>(thread_id->size) : 0
            )
        };

        std::string_view args(
            args_json && args_json->data ? args_json->data : "{}",
            args_json ? static_cast<size_t>(args_json->size) : 0
        );
        auto task = shim->fn(*shim->ctx, args, ctl);
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

        auto* job    = new Job{shim, cancelFlag, h.address()};
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
    int64_t          default_timeout_ms = 0,
    int32_t          flags              = 0
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
    spec.name        = PluginStringView::from(name.data(), name.size());
    spec.description = PluginStringView::from(
        storage[storage.size() - 2].data(),
        storage[storage.size() - 2].size()
    );
    spec.parameters_json    = PluginStringView::from(storage.back().data(), storage.back().size());
    spec.user_data          = shim;
    spec.default_timeout_ms = default_timeout_ms;
    spec.flags              = flags;
    spec._reserved          = 0;

    spec.execute_start = [](void*                              user_data,
                            const AgentxxPluginStringView*     args_json,
                            const AgentxxPluginStringView*     thread_id,
                            const AgentxxPluginStringView*     tool_call_id,
                            const AgentxxPluginOperatorNotify* notify,
                            AgentxxPluginString*               error_out) -> void* {
        auto* shim = static_cast<FastShim*>(user_data);
        (void)tool_call_id;
        try {
            std::string_view args(
                args_json && args_json->data ? args_json->data : "{}",
                args_json ? static_cast<size_t>(args_json->size) : 0
            );
            std::string_view tid(
                thread_id && thread_id->data ? thread_id->data : "",
                thread_id ? static_cast<size_t>(thread_id->size) : 0
            );
            std::string res;
            if constexpr (std::is_invocable_v<SyncFn, Ctx&, std::string_view, std::string_view>) {
                res = shim->fn(*shim->ctx, args, tid);
            } else if constexpr (std::is_invocable_v<SyncFn, Ctx&, std::string_view>) {
                res = shim->fn(*shim->ctx, args);
            } else {
                res = shim->fn(args);
            }

            if (notify && notify->done) {
                auto resSv = PluginStringView::from(res.data(), res.size());
                notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, &resSv);
            }
        } catch (const std::exception& e) {
            if (notify && notify->done) {
                std::string what  = e.what();
                auto        errSv = PluginStringView::from(what.data(), what.size());
                notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &errSv);
            } else if (error_out) {
                *error_out = PluginString::fromCstr(shim->ctx->host, e.what());
            }
        } catch (...) {
            if (notify && notify->done) {
                auto errSv = PluginStringView::fromCstr("unknown error");
                notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &errSv);
            } else if (error_out) {
                *error_out = PluginString::fromCstr(shim->ctx->host, "unknown error in fast_tool");
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
    int64_t          default_timeout_ms = 0,
    int32_t          flags              = 0
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
        std::string                 workDir;
        volatile int32_t            cancelFlag = 0;
        AgentxxPluginString         resultStr{nullptr, 0};
    };

    AgentxxPluginToolSpec spec{};
    spec.name        = PluginStringView::from(name.data(), name.size());
    spec.description = PluginStringView::from(
        storage[storage.size() - 2].data(),
        storage[storage.size() - 2].size()
    );
    spec.parameters_json    = PluginStringView::from(storage.back().data(), storage.back().size());
    spec.user_data          = shim;
    spec.default_timeout_ms = default_timeout_ms;
    spec.flags              = flags;
    spec._reserved          = 0;

    spec.execute_start = [](void*                              user_data,
                            const AgentxxPluginStringView*     args_json,
                            const AgentxxPluginStringView*     thread_id,
                            const AgentxxPluginStringView*     tool_call_id,
                            const AgentxxPluginOperatorNotify* notify,
                            AgentxxPluginString*               error_out) -> void* {
        auto* shim = static_cast<BlockShim*>(user_data);
        (void)error_out;
        std::string tidStr(
            thread_id && thread_id->data ? thread_id->data : "",
            thread_id ? static_cast<size_t>(thread_id->size) : 0
        );
        std::string workDirCache;
        if (shim->ctx) {
            auto tidSv   = PluginStringView::from(tidStr.data(), tidStr.size());
            workDirCache = shim->ctx->workDir(tidSv);
        }
        auto* job = new Job{
            shim,
            notify ? *notify : AgentxxPluginOperatorNotify{nullptr, nullptr},
            std::string(
                args_json && args_json->data ? args_json->data : "{}",
                args_json ? static_cast<size_t>(args_json->size) : 0
            ),
            std::move(tidStr),
            std::string(
                tool_call_id && tool_call_id->data ? tool_call_id->data : "",
                tool_call_id ? static_cast<size_t>(tool_call_id->size) : 0
            ),
            std::move(workDirCache),
            0
        };

        if (shim->ctx->iface.scheduler && shim->ctx->iface.scheduler->offload) {
            shim->ctx->iface.scheduler->offload(
                shim->ctx->host,
                &job->cancelFlag,
                [](void* ud, volatile int32_t* cflag, AgentxxPluginString* err_out) -> void* {
                    auto* j = static_cast<Job*>(ud);
                    try {
                        std::string res;
                        if constexpr (std::is_invocable_v<
                                          BlockFn,
                                          Ctx&,
                                          std::string_view,
                                          std::string_view,
                                          std::string_view,
                                          volatile int32_t*>) {
                            res = j->shim->fn(*j->shim->ctx, j->args, j->tid, j->workDir, cflag);
                        } else if constexpr (std::is_invocable_v<
                                                 BlockFn,
                                                 Ctx&,
                                                 std::string_view,
                                                 std::string_view,
                                                 volatile int32_t*>) {
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
                                                 volatile int32_t*>) {
                            res = j->shim->fn(*j->shim->ctx, j->args, cflag);
                        } else if constexpr (std::is_invocable_v<
                                                 BlockFn,
                                                 std::string_view,
                                                 volatile int32_t*>) {
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
                        if (j->shim->ctx && j->shim->ctx->host) {
                            j->resultStr = j->shim->ctx->createString(std::string_view{res});
                            return &j->resultStr;
                        }
                        return nullptr;
                    } catch (const CancelledException&) {
                        return nullptr;
                    } catch (const std::exception& e) {
                        if (err_out) {
                            *err_out = PluginString::fromCstr(j->shim->ctx->host, e.what());
                        }
                        return nullptr;
                    } catch (...) {
                        if (err_out) {
                            *err_out = PluginString::fromCstr(
                                j->shim->ctx->host,
                                "unknown blocking tool error"
                            );
                        }
                        return nullptr;
                    }
                },
                [](void* ud, void* res, const AgentxxPluginStringView* err) {
                    auto* j = static_cast<Job*>(ud);
                    (void)res;
                    int32_t                 st      = AGENTXX_PLUGIN_OPERATOR_OK;
                    AgentxxPluginStringView payload = PluginStringView::from(nullptr, 0);

                    if (!PluginStringView::empty(err)) {
                        st      = AGENTXX_PLUGIN_OPERATOR_FAILED;
                        payload = *err;
                    } else if (j->resultStr.data) {
                        payload = PluginStringView::toSv(j->resultStr);
                    } else {
                        st = AGENTXX_PLUGIN_OPERATOR_CANCELLED;
                    }

                    if (j->notify.done) {
                        j->notify.done(j->notify.host_ud, st, &payload);
                    }

                    if (j->resultStr.data && j->shim->ctx && j->shim->ctx->host) {
                        PluginString::free(j->shim->ctx->host, &j->resultStr);
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
    spec._reserved = 0;
    spec.user_data = shim;

    spec.hook_start = [](void*                              user_data,
                         int32_t                            pt,
                         const AgentxxPluginStringView*     node_input_json,
                         const AgentxxPluginOperatorNotify* notify,
                         AgentxxPluginString*               error_out) -> void* {
        auto* shim = static_cast<HookShim*>(user_data);
        (void)error_out;
        try {
            std::string_view input(
                node_input_json && node_input_json->data ? node_input_json->data : "{}",
                node_input_json ? static_cast<size_t>(node_input_json->size) : 0
            );
            if constexpr (std::is_invocable_v<
                              HookFn,
                              Ctx&,
                              AgentxxPluginHookPoint,
                              std::string_view>) {
                shim->fn(*shim->ctx, static_cast<AgentxxPluginHookPoint>(pt), input);
            } else if constexpr (std::is_invocable_v<HookFn, Ctx&, std::string_view>) {
                shim->fn(*shim->ctx, input);
            } else {
                shim->fn(input);
            }
            if (notify && notify->done) {
                auto okSv = PluginStringView::from(nullptr, 0);
                notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, &okSv);
            }
        } catch (const std::exception& e) {
            if (notify && notify->done) {
                std::string what  = e.what();
                auto        errSv = PluginStringView::from(what.data(), what.size());
                notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &errSv);
            }
        } catch (...) {
            if (notify && notify->done) {
                auto errSv = PluginStringView::fromCstr("hook error");
                notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &errSv);
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
        auto capSv = PluginStringView::from(capName.data(), capName.size());
        ctx.iface.capabilities->register_capability_ex(
            ctx.host,
            &capSv,
            [](void*                              user_data,
               const AgentxxPluginHost*           caller_host,
               const AgentxxPluginStringView*     method,
               const AgentxxPluginStringView*     args_json,
               const AgentxxPluginOperatorNotify* notify,
               AgentxxPluginString*               error_out) -> void* {
                auto* shim = static_cast<CapShim*>(user_data);
                (void)error_out;
                try {
                    std::string_view meth(
                        method && method->data ? method->data : "",
                        method ? static_cast<size_t>(method->size) : 0
                    );
                    std::string_view args(
                        args_json && args_json->data ? args_json->data : "{}",
                        args_json ? static_cast<size_t>(args_json->size) : 0
                    );
                    std::string res;
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
                        auto resSv = PluginStringView::from(res.data(), res.size());
                        notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, &resSv);
                    }
                } catch (const std::exception& e) {
                    if (notify && notify->done) {
                        std::string what  = e.what();
                        auto        errSv = PluginStringView::from(what.data(), what.size());
                        notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &errSv);
                    }
                } catch (...) {
                    if (notify && notify->done) {
                        auto errSv = PluginStringView::fromCstr("capability error");
                        notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &errSv);
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

inline AgentxxPluginString call_tool_blocking(
    const AgentxxPluginHost*           host,
    const AgentxxPluginToolsIface*     tools,
    const AgentxxPluginSchedulerIface* sched,
    std::string_view                   name,
    std::string_view                   args_json,
    std::string_view                   thread_id,
    AgentxxPluginString*               error_out
) {
    if (!host || !tools || !tools->call_tool_async) {
        if (error_out) {
            *error_out = PluginString::fromCstr(host, "tools iface not available");
        }
        return AgentxxPluginString{nullptr, 0};
    }
    if (sched && sched->is_io_thread && sched->is_io_thread(host)) {
        if (error_out) {
            *error_out = PluginString::fromCstr(
                host,
                "call_tool_blocking cannot be called on io thread; use co_await call_tool instead"
            );
        }
        return AgentxxPluginString{nullptr, 0};
    }

    struct SyncState {
        std::mutex              mtx;
        std::condition_variable cv;
        bool                    done   = false;
        int32_t                 status = AGENTXX_PLUGIN_OPERATOR_OK;
        std::string             payload;
    } state;

    auto nameSv = PluginStringView::from(name.data(), name.size());
    auto argsSv = PluginStringView::from(args_json.data(), args_json.size());
    auto tidSv  = PluginStringView::from(thread_id.data(), thread_id.size());

    AgentxxPluginOperatorHandle* handle = tools->call_tool_async(
        host,
        &nameSv,
        &argsSv,
        &tidSv,
        [](void* ud, int32_t st, const AgentxxPluginStringView* pl) {
            auto*           s = static_cast<SyncState*>(ud);
            std::lock_guard lk(s->mtx);
            s->done   = true;
            s->status = st;
            if (pl && pl->data && pl->size > 0) {
                s->payload.assign(pl->data, static_cast<size_t>(pl->size));
            }
            s->cv.notify_one();
        },
        &state,
        error_out
    );

    if (!handle) {
        return AgentxxPluginString{nullptr, 0};
    }

    {
        std::unique_lock lk(state.mtx);
        state.cv.wait(lk, [&]() {
            return state.done;
        });
    }

    if (state.status != AGENTXX_PLUGIN_OPERATOR_OK) {
        if (error_out) {
            auto paySv = PluginStringView::from(state.payload.data(), state.payload.size());
            *error_out = PluginString::from(host, &paySv);
        }
        return AgentxxPluginString{nullptr, 0};
    }

    auto paySv = PluginStringView::from(state.payload.data(), state.payload.size());
    return PluginString::from(host, &paySv);
}

inline AgentxxPluginString invoke_capability_blocking(
    const AgentxxPluginHost*              host,
    const AgentxxPluginCapabilitiesIface* caps,
    const AgentxxPluginSchedulerIface*    sched,
    std::string_view                      capability,
    std::string_view                      method,
    std::string_view                      args_json,
    AgentxxPluginString*                  error_out
) {
    if (!host || !caps || !caps->invoke_capability_async) {
        if (error_out) {
            *error_out = PluginString::fromCstr(host, "capabilities iface not available");
        }
        return AgentxxPluginString{nullptr, 0};
    }
    if (sched && sched->is_io_thread && sched->is_io_thread(host)) {
        if (error_out) {
            *error_out = PluginString::fromCstr(
                host,
                "invoke_capability_blocking cannot be called on io thread; use co_await invoke_cap instead"
            );
        }
        return AgentxxPluginString{nullptr, 0};
    }

    struct SyncState {
        std::mutex              mtx;
        std::condition_variable cv;
        bool                    done   = false;
        int32_t                 status = AGENTXX_PLUGIN_OPERATOR_OK;
        std::string             payload;
    } state;

    auto capSv  = PluginStringView::from(capability.data(), capability.size());
    auto methSv = PluginStringView::from(method.data(), method.size());
    auto argsSv = PluginStringView::from(args_json.data(), args_json.size());

    AgentxxPluginOperatorHandle* handle = caps->invoke_capability_async(
        host,
        &capSv,
        &methSv,
        &argsSv,
        [](void* ud, int32_t st, const AgentxxPluginStringView* pl) {
            auto*           s = static_cast<SyncState*>(ud);
            std::lock_guard lk(s->mtx);
            s->done   = true;
            s->status = st;
            if (pl && pl->data && pl->size > 0) {
                s->payload.assign(pl->data, static_cast<size_t>(pl->size));
            }
            s->cv.notify_one();
        },
        &state,
        error_out
    );

    if (!handle) {
        return AgentxxPluginString{nullptr, 0};
    }

    {
        std::unique_lock lk(state.mtx);
        state.cv.wait(lk, [&]() {
            return state.done;
        });
    }

    if (state.status != AGENTXX_PLUGIN_OPERATOR_OK) {
        if (error_out) {
            auto paySv = PluginStringView::from(state.payload.data(), state.payload.size());
            *error_out = PluginString::from(host, &paySv);
        }
        return AgentxxPluginString{nullptr, 0};
    }

    auto paySv = PluginStringView::from(state.payload.data(), state.payload.size());
    return PluginString::from(host, &paySv);
}

/* ==================== 同步/内联工具适配器 (原 plugin_tool_sync.h, 并入 kit) ====================
 *
 * 把插件本地同步函数 / 内联快函数适配成宿主异步工具/钩子契约 (execute_start 两件套)
 * 并注册。本区全部为插件侧 C++ 适配设施:
 * - 类型仅供插件侧使用, 宿主不解析 → 无需 ABI pack(8); 结构体默认对齐即可
 * - 函数指针 typedef (AgentxxSyncToolFn 等) 是插件本地同步函数形状, 不跨 DLL
 *   被宿主直接调用 → 不加 AGENTXX_PLUGIN_CALL (本地默认调用约定)
 * - 适配器函数 (agentxx_sync_tool_start / agentxx_sync_job_work / agentxx_sync_job_done /
 *   agentxx_sync_tool_cancel / agentxx_inline_tool_start / agentxx_sync_hook_start) 会被填入
 *   ABI spec (AgentxxPluginToolSpec.execute_start 等) 或传给 scheduler->offload 由宿主跨 DLL
 *   调用 → 必须保留 AGENTXX_PLUGIN_CALL
 */

/// 插件本地同步工具函数形状 (offload 线程池执行体; 宿主不直接调用)
using SyncToolFn = AgentxxPluginString(
    void*                          user_data,
    const AgentxxPluginStringView* args_json,
    const AgentxxPluginStringView* session_id,
    const AgentxxPluginStringView* tool_call_id,
    volatile int32_t*              cancel_flag,
    AgentxxPluginString*           error_out
);

/// 同步工具注册规格 (插件侧)
struct SyncToolSpec {
    AgentxxPluginStringView name;
    AgentxxPluginStringView description;
    AgentxxPluginStringView parameters_json;
    SyncToolFn*             execute;
    void*                   user_data;
    int64_t                 default_timeout_ms;
    int32_t                 flags;
    uint32_t                _reserved;
};

/// 同步工具适配状态 (作为 user_data 传给宿主 execute_start/cancel)
struct SyncToolShim {
    const AgentxxPluginHost*           host;
    const AgentxxPluginSchedulerIface* sched;
    SyncToolFn*                        fn;
    void*                              ud;
};

/// 同步工具 offload 任务 (堆分配; 生命周期: start → offload work/done → done 内释放)
struct SyncJob {
    SyncToolShim                shim;
    AgentxxPluginOperatorNotify notify;
    AgentxxPluginStringView     args;
    AgentxxPluginStringView     tid;
    AgentxxPluginStringView     tcid;
    volatile int32_t            cancelFlag;
    uint32_t                    _reserved;
    AgentxxPluginString         resultStr{nullptr, 0};
};

inline AgentxxPluginString shimErrDup(const AgentxxPluginHost* host, std::string_view msg) {
    if (!host || !host->vtable || msg.empty()) {
        return AgentxxPluginString{nullptr, 0};
    }
    return PluginString::from(host, msg);
}

inline AgentxxPluginString shimErrDup(const AgentxxPluginHost* host, const char* msg) {
    if (!host || !host->vtable || !msg) {
        return AgentxxPluginString{nullptr, 0};
    }
    return PluginString::fromCstr(host, msg);
}

/// offload work 执行体 (跨 DLL: 由宿主 scheduler->offload 在 io 线程外调用)
inline void* AGENTXX_PLUGIN_CALL
    syncJobWork(void* ud, volatile int32_t* cancel_flag, AgentxxPluginString* error_out) {
    SyncJob* job = static_cast<SyncJob*>(ud);
    try {
        job->resultStr
            = job->shim.fn(job->shim.ud, &job->args, &job->tid, &job->tcid, cancel_flag, error_out);
        return &job->resultStr;
    } catch (const std::exception& e) {
        if (error_out && !error_out->data) {
            *error_out = shimErrDup(job->shim.host, e.what());
        }
        return nullptr;
    } catch (...) {
        if (error_out && !error_out->data) {
            *error_out = shimErrDup(job->shim.host, "sync tool threw unknown exception");
        }
        return nullptr;
    }
}

/// offload done 回调 (跨 DLL: 由宿主 scheduler 在 io 线程调用; 释放 job)
inline void AGENTXX_PLUGIN_CALL
    syncJobDone(void* ud, void* result, const AgentxxPluginStringView* error) {
    (void)result;
    SyncJob*                job     = static_cast<SyncJob*>(ud);
    int32_t                 st      = AGENTXX_PLUGIN_OPERATOR_OK;
    AgentxxPluginStringView payload = PluginStringView::from(nullptr, 0);

    if (!PluginStringView::empty(error)) {
        st      = AGENTXX_PLUGIN_OPERATOR_FAILED;
        payload = *error;
    } else if (job->resultStr.data) {
        payload = PluginStringView::toSv(job->resultStr);
    } else {
        st = AGENTXX_PLUGIN_OPERATOR_CANCELLED;
    }

    if (job->notify.done) {
        job->notify.done(job->notify.host_ud, st, &payload);
    }

    if (job->resultStr.data && job->shim.host) {
        PluginString::free(job->shim.host, &job->resultStr);
    }

    if (job->args.data) {
        std::free(static_cast<void*>(const_cast<char*>(job->args.data)));
    }
    if (job->tid.data) {
        std::free(static_cast<void*>(const_cast<char*>(job->tid.data)));
    }
    if (job->tcid.data) {
        std::free(static_cast<void*>(const_cast<char*>(job->tcid.data)));
    }
    std::free(job);
}

/// 同步工具 execute_start 适配器 (跨 DLL: 填入 AgentxxPluginToolSpec.execute_start)
inline void* AGENTXX_PLUGIN_CALL syncToolStart(
    void*                              user_data,
    const AgentxxPluginStringView*     args_json,
    const AgentxxPluginStringView*     session_id,
    const AgentxxPluginStringView*     tool_call_id,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               error_out
) {
    SyncToolShim* shim = static_cast<SyncToolShim*>(user_data);
    if (!shim || !shim->sched || !shim->sched->offload) {
        if (error_out) {
            *error_out = shimErrDup(shim ? shim->host : nullptr, "scheduler iface not available");
        }
        return nullptr;
    }

    SyncJob* job = static_cast<SyncJob*>(std::malloc(sizeof(SyncJob)));
    if (!job) {
        if (error_out) {
            *error_out = shimErrDup(shim->host, "out of memory allocating job");
        }
        return nullptr;
    }
    job->shim       = *shim;
    job->notify     = *notify;
    job->cancelFlag = 0;
    job->args       = PluginStringView::from(nullptr, 0);
    job->tid        = PluginStringView::from(nullptr, 0);
    job->tcid       = PluginStringView::from(nullptr, 0);

    if (args_json && args_json->size) {
        char* buf = static_cast<char*>(std::malloc(static_cast<size_t>(args_json->size)));
        if (!buf) {
            std::free(job);
            if (error_out) {
                *error_out = shimErrDup(shim->host, "out of memory allocating args");
            }
            return nullptr;
        }
        std::memcpy(buf, args_json->data, static_cast<size_t>(args_json->size));
        job->args = PluginStringView::from(buf, args_json->size);
    }

    if (session_id && session_id->size) {
        char* buf = static_cast<char*>(std::malloc(static_cast<size_t>(session_id->size)));
        if (!buf) {
            if (job->args.data) {
                std::free(const_cast<char*>(job->args.data));
            }
            std::free(job);
            if (error_out) {
                *error_out = shimErrDup(shim->host, "out of memory allocating session_id");
            }
            return nullptr;
        }
        std::memcpy(buf, session_id->data, static_cast<size_t>(session_id->size));
        job->tid = PluginStringView::from(buf, session_id->size);
    }

    if (tool_call_id && tool_call_id->size) {
        char* buf = static_cast<char*>(std::malloc(static_cast<size_t>(tool_call_id->size)));
        if (!buf) {
            if (job->args.data) {
                std::free(const_cast<char*>(job->args.data));
            }
            if (job->tid.data) {
                std::free(const_cast<char*>(job->tid.data));
            }
            std::free(job);
            if (error_out) {
                *error_out = shimErrDup(shim->host, "out of memory allocating tool_call_id");
            }
            return nullptr;
        }
        std::memcpy(buf, tool_call_id->data, static_cast<size_t>(tool_call_id->size));
        job->tcid = PluginStringView::from(buf, tool_call_id->size);
    }

    shim->sched->offload(shim->host, &job->cancelFlag, &syncJobWork, &syncJobDone, job);
    return job;
}

/// execute_cancel 适配器 (跨 DLL: 填入 AgentxxPluginToolSpec.execute_cancel)
inline void AGENTXX_PLUGIN_CALL syncToolCancel(void* user_data, void* op) {
    (void)user_data;
    if (!op) {
        return;
    }
    SyncJob* job    = static_cast<SyncJob*>(op);
    job->cancelFlag = 1;
}

/// 注册同步工具 (内部: 查询接口表 + 填充 ABI spec; 返回 register_tool 状态)
inline int32_t registerSyncTool(
    const AgentxxPluginHost* host,
    const SyncToolSpec*      sync_spec,
    SyncToolShim*            out_shim
) {
    if (!host || !host->vtable || !sync_spec || !out_shim || !sync_spec->execute) {
        return -1;
    }
    const AgentxxPluginToolsIface* tools
        = queryInterface<AgentxxPluginToolsIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_TOOLS);
    const AgentxxPluginSchedulerIface* sched
        = queryInterface<AgentxxPluginSchedulerIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER);
    if (!tools || !tools->register_tool || !sched) {
        return -1;
    }

    out_shim->host  = host;
    out_shim->sched = sched;
    out_shim->fn    = sync_spec->execute;
    out_shim->ud    = sync_spec->user_data;

    AgentxxPluginToolSpec spec;
    spec.name               = sync_spec->name;
    spec.description        = sync_spec->description;
    spec.parameters_json    = sync_spec->parameters_json;
    spec.execute_start      = &syncToolStart;
    spec.execute_cancel     = &syncToolCancel;
    spec.user_data          = out_shim;
    spec.default_timeout_ms = sync_spec->default_timeout_ms;
    spec.flags              = sync_spec->flags;
    spec._reserved          = 0;

    return tools->register_tool(host, &spec);
}

/// 插件本地内联工具函数形状 (宿主 io 线程直接调用; 不跨 DLL)
using InlineToolFn = AgentxxPluginString(
    void*                          user_data,
    const AgentxxPluginStringView* args_json,
    const AgentxxPluginStringView* session_id,
    const AgentxxPluginStringView* tool_call_id,
    AgentxxPluginString*           error_out
);

/// 内联工具注册规格 (插件侧)
struct InlineToolSpec {
    AgentxxPluginStringView name;
    AgentxxPluginStringView description;
    AgentxxPluginStringView parameters_json;
    InlineToolFn*           execute;
    void*                   user_data;
    int64_t                 default_timeout_ms;
    int32_t                 flags;
    uint32_t                _reserved;
};

/// 内联工具适配状态 (作为 user_data 传给宿主 execute_start)
struct InlineToolShim {
    const AgentxxPluginHost* host;
    InlineToolFn*            fn;
    void*                    ud;
};

/// 内联工具 execute_start 适配器 (跨 DLL: 填入 AgentxxPluginToolSpec.execute_start;
/// io 线程同步执行后立即 done)
inline void* AGENTXX_PLUGIN_CALL inlineToolStart(
    void*                              user_data,
    const AgentxxPluginStringView*     args_json,
    const AgentxxPluginStringView*     session_id,
    const AgentxxPluginStringView*     tool_call_id,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               error_out
) {
    InlineToolShim* shim = static_cast<InlineToolShim*>(user_data);
    if (!shim || !shim->fn) {
        if (error_out) {
            *error_out = shimErrDup(shim ? shim->host : nullptr, "invalid inline tool shim");
        }
        return nullptr;
    }

    AgentxxPluginString result{nullptr, 0};
    try {
        result = shim->fn(shim->ud, args_json, session_id, tool_call_id, error_out);
    } catch (const std::exception& e) {
        if (error_out && !error_out->data) {
            *error_out = shimErrDup(shim->host, e.what());
        }
        result = {nullptr, 0};
    } catch (...) {
        if (error_out && !error_out->data) {
            *error_out = shimErrDup(shim->host, "inline tool threw unknown exception");
        }
        result = {nullptr, 0};
    }

    if (error_out && error_out->data) {
        if (notify && notify->done) {
            AgentxxPluginString errPayload = *error_out;
            error_out->data                = nullptr;
            error_out->size                = 0;
            AgentxxPluginStringView errSv  = PluginStringView::toSv(errPayload);
            notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &errSv);
            if (shim->host) {
                PluginString::free(shim->host, &errPayload);
            }
        }
        if (result.data && shim->host) {
            PluginString::free(shim->host, &result);
        }
    } else {
        if (notify && notify->done) {
            AgentxxPluginStringView resSv = PluginStringView::toSv(result);
            notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, &resSv);
            if (result.data && shim->host) {
                PluginString::free(shim->host, &result);
            }
        }
    }
    return nullptr;
}

/// 注册内联工具
inline int32_t registerInlineTool(
    const AgentxxPluginHost* host,
    const InlineToolSpec*    inline_spec,
    InlineToolShim*          out_shim
) {
    if (!host || !host->vtable || !inline_spec || !out_shim || !inline_spec->execute) {
        return -1;
    }
    const AgentxxPluginToolsIface* tools
        = queryInterface<AgentxxPluginToolsIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_TOOLS);
    if (!tools || !tools->register_tool) {
        return -1;
    }

    out_shim->host = host;
    out_shim->fn   = inline_spec->execute;
    out_shim->ud   = inline_spec->user_data;

    AgentxxPluginToolSpec spec;
    spec.name               = inline_spec->name;
    spec.description        = inline_spec->description;
    spec.parameters_json    = inline_spec->parameters_json;
    spec.execute_start      = &inlineToolStart;
    spec.execute_cancel     = nullptr;
    spec.user_data          = out_shim;
    spec.default_timeout_ms = inline_spec->default_timeout_ms;
    spec.flags              = inline_spec->flags;
    spec._reserved          = 0;

    return tools->register_tool(host, &spec);
}

/// 插件本地同步钩子函数形状 (offload 线程池执行体; 宿主不直接调用)
using SyncHookFn = int32_t(
    void*                          user_data,
    int32_t                        point,
    const AgentxxPluginStringView* node_input_json,
    AgentxxPluginString*           error_out
);

/// 同步钩子适配状态 (作为 user_data 传给宿主 hook_start)
struct SyncHookShim {
    const AgentxxPluginHost* host;
    SyncHookFn*              fn;
    void*                    ud;
};

/// 同步钩子 hook_start 适配器 (跨 DLL: 填入 AgentxxPluginHookSpec.hook_start)
inline void* AGENTXX_PLUGIN_CALL syncHookStart(
    void*                              user_data,
    int32_t                            point,
    const AgentxxPluginStringView*     node_input_json,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               error_out
) {
    SyncHookShim* shim = static_cast<SyncHookShim*>(user_data);
    if (!shim || !shim->fn) {
        if (error_out) {
            *error_out = shimErrDup(shim ? shim->host : nullptr, "invalid hook shim");
        }
        return nullptr;
    }
    int32_t rc = 0;
    try {
        rc = shim->fn(shim->ud, point, node_input_json, error_out);
    } catch (const std::exception& e) {
        if (error_out && !error_out->data) {
            *error_out = shimErrDup(shim->host, e.what());
        }
        rc = -1;
    } catch (...) {
        if (error_out && !error_out->data) {
            *error_out = shimErrDup(shim->host, "hook threw unknown exception");
        }
        rc = -1;
    }

    if (notify && notify->done) {
        AgentxxPluginStringView errSv = PluginStringView::from(nullptr, 0);
        if (error_out && error_out->data) {
            errSv = PluginStringView::toSv(error_out);
        }
        notify->done(
            notify->host_ud,
            rc == 0 ? AGENTXX_PLUGIN_OPERATOR_OK : AGENTXX_PLUGIN_OPERATOR_FAILED,
            &errSv
        );
        if (error_out && error_out->data && shim->host) {
            PluginString::free(shim->host, error_out);
        }
    }
    return nullptr;
}

/// 注册同步钩子
inline int32_t registerSyncHook(
    const AgentxxPluginHost* host,
    int32_t                  point,
    SyncHookFn*              fn,
    void*                    user_data,
    SyncHookShim*            out_shim
) {
    if (!host || !host->vtable || !fn || !out_shim || point < 0
        || point >= AGENTXX_PLUGIN_HOOK_COUNT) {
        return -1;
    }
    const AgentxxPluginHooksIface* hooks
        = queryInterface<AgentxxPluginHooksIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_HOOKS);
    if (!hooks || !hooks->register_hook) {
        return -1;
    }

    out_shim->host = host;
    out_shim->fn   = fn;
    out_shim->ud   = user_data;

    AgentxxPluginHookSpec spec;
    spec.point       = point;
    spec._reserved   = 0;
    spec.hook_start  = &syncHookStart;
    spec.hook_cancel = nullptr;
    spec.user_data   = out_shim;

    return hooks->register_hook(host, &spec);
}

/* ==================== Client 侧工具特化渲染适配器 ==================== */

struct ToolRenderInput {
    std::string_view toolCallId;
    std::string_view toolName;
    std::string_view argsJson;
    std::string_view resultText;
    bool             isFinished = false;
    bool             isError    = false;
    int              maxWidth   = 0;
};

struct ToolRenderOutput {
    std::string    displayName;
    std::string    summary;
    neograph::json items = neograph::json::array();
};

/// 注册基于回调函数的工具特化渲染器 (<key, 渲染func>)
template<typename Fn>
inline int32_t registerToolRenderer(
    const AgentxxPluginHost*                             host,
    const AgentxxClientUiIface*                          ui,
    std::string_view                                     toolName,
    Fn&&                                                 fn,
    std::vector<std::unique_ptr<void, void (*)(void*)>>& shimStorage
) {
    if (!host || !ui || !ui->register_tool_renderer) {
        return -1;
    }
    using DecayedFn = std::decay_t<Fn>;

    struct RenderShim {
        const AgentxxPluginHost* host = nullptr;
        DecayedFn                fn;
    };

    auto* shim = new RenderShim{host, std::forward<Fn>(fn)};
    shimStorage.emplace_back(shim, [](void* p) {
        delete static_cast<RenderShim*>(p);
    });

    auto renderCb
        = [](void* user_data, const AgentxxToolRenderInput* input, AgentxxToolRenderOutput* output
          ) -> int32_t {
        if (!user_data || !input || !output) {
            return -1;
        }
        auto*           shim = static_cast<RenderShim*>(user_data);
        ToolRenderInput in{
            .toolCallId = PluginStringView::str(input->tool_call_id),
            .toolName   = PluginStringView::str(input->tool_name),
            .argsJson   = PluginStringView::str(input->args_json),
            .resultText = PluginStringView::str(input->result_text),
            .isFinished = input->is_finished != 0,
            .isError    = input->is_error != 0,
            .maxWidth   = input->max_width,
        };
        ToolRenderOutput out;
        try {
            shim->fn(in, out);
        } catch (...) {
            return -1;
        }
        if (!out.displayName.empty()) {
            output->displayName = PluginString::from(shim->host, out.displayName);
        }
        if (!out.summary.empty()) {
            output->summary = PluginString::from(shim->host, out.summary);
        }
        if (!out.items.empty()) {
            output->items_json = PluginString::from(shim->host, out.items.dump());
        }
        return 0;
    };

    AgentxxToolRenderSpec spec{};
    spec.version       = 1;
    spec.tool_name     = PluginStringView::from(toolName.data(), toolName.size());
    spec.render_fn     = renderCb;
    spec.user_data     = shim;
    spec.template_json = PluginStringView::from(nullptr, 0);

    return ui->register_tool_renderer(host, &spec);
}

/// 注册基于预设模版的工具特化渲染器
inline int32_t registerToolTemplate(
    const AgentxxPluginHost*    host,
    const AgentxxClientUiIface* ui,
    std::string_view            toolName,
    std::string_view            displayName,
    std::string_view            summaryKey
) {
    if (!host || !ui || !ui->register_tool_renderer) {
        return -1;
    }
    neograph::json j;
    j["displayName"]    = std::string(displayName);
    j["summaryKey"]     = std::string(summaryKey);
    std::string jsonStr = j.dump();

    AgentxxToolRenderSpec spec{};
    spec.version       = 1;
    spec.tool_name     = PluginStringView::from(toolName.data(), toolName.size());
    spec.render_fn     = nullptr;
    spec.user_data     = nullptr;
    spec.template_json = PluginStringView::from(jsonStr.data(), jsonStr.size());

    return ui->register_tool_renderer(host, &spec);
}

} // namespace plugin
} // namespace agentxx
