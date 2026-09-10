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
#include "agentxx/util/container_util.h"
#include "agentxx/util/json.h"
#include "agentxx/util/json_view.h"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include <type_traits>

#include <algorithm>
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
#include <variant>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace agentxx {
namespace plugin {

/// 插件作用域 JSON 别名 (自主 Json 体系, 不再依赖 neograph)
using Json     = agentxx::util::Json;
using JsonView = agentxx::util::JsonView;

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

    /// 指针重载
    static std::string_view str(const AgentxxPluginStringView* sv) noexcept {
        return (sv && sv->data) ? std::string_view{sv->data, static_cast<size_t>(sv->size)}
                                : std::string_view{};
    }

    /// 宿主堆字符串 → std::string_view (零拷贝; NULL data 视为空串)
    static std::string_view str(const AgentxxPluginString& s) noexcept {
        return s.data ? std::string_view{s.data, static_cast<size_t>(s.size)} : std::string_view{};
    }

    /// 指针重载
    static std::string_view str(const AgentxxPluginString* s) noexcept {
        return (s && s->data) ? std::string_view{s->data, static_cast<size_t>(s->size)}
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

    /// Replace an ABI string and release an existing host allocation first.
    static void set(
        const AgentxxPluginHost* h, AgentxxPluginString* out, std::string_view sv
    ) noexcept {
        if (!out) {
            return;
        }
        if (out->data) {
            free(h, out);
        }
        *out = from(h, sv);
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
const Iface* validateInterface(const void* raw) noexcept {
    if (!raw) {
        return nullptr;
    }
    const auto* iface = static_cast<const Iface*>(raw);
    // Reset-v1 interface tables all use exact version 1 and expose their
    // complete byte size. A short or newer-incompatible table is unusable.
    if (iface->version != 1 || iface->struct_size < sizeof(Iface)) {
        return nullptr;
    }
    return iface;
}

template<typename Iface>
const Iface* queryInterface(const AgentxxPluginHost* host, std::string_view iid) noexcept {
    if (!host || !host->vtable || !host->vtable->query_interface || iid.empty()) {
        return nullptr;
    }
    AgentxxPluginStringView sv = PluginStringView::from(iid.data(), iid.size());
    return validateInterface<Iface>(host->vtable->query_interface(host, &sv));
}

template<typename Iface>
const Iface*
    queryInterface(const AgentxxPluginHost* host, const AgentxxPluginStringView& iid) noexcept {
    if (!host || !host->vtable || !host->vtable->query_interface || PluginStringView::empty(iid)) {
        return nullptr;
    }
    return validateInterface<Iface>(host->vtable->query_interface(host, &iid));
}

template<typename Iface>
const Iface* queryInterface(const AgentxxPluginHost* host, const char* iid) noexcept {
    if (!host || !host->vtable || !host->vtable->query_interface || !iid) {
        return nullptr;
    }
    AgentxxPluginStringView sv = PluginStringView::fromCstr(iid);
    return validateInterface<Iface>(host->vtable->query_interface(host, &sv));
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
    void(AGENTXX_PLUGIN_CALL* logFn)(
        const AgentxxPluginHost*       host,
        int32_t                        level,
        const AgentxxPluginStringView* msg
    ) = nullptr;

    void log(int32_t level, std::string_view msg) const noexcept {
        if (!host) {
            return;
        }
        if (logFn) {
            auto sv = PluginStringView::from(msg.data(), msg.size());
            logFn(host, level, &sv);
        } else if (logIface && logIface->log) {
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

/* ==================== 插件框架事件驱动取消注册表 (CancelRegistry) ==================== */

/// 插件框架事件驱动取消注册表
/// - 职责: 管理会话级别与操作级别的取消事件注册、注销与原子通知
/// - 线程安全: 完全支持多线程并发注册、注销与触发
/// - 内存自治: 纯堆内存实例，无任何全局/静态状态，严格契合多实例契约
/// - 零悬挂保证: unregisterCallback
/// 会等待并排他锁定正在执行中的回调，确保调用栈上的对象不会在回调执行中析构
class CancelRegistry {
public:

    using CancelCallback = std::function<void()>;
    using RegId          = uint64_t;

    /// 回调实体封装：支持并发排他保护与生命周期状态标记
    struct CallbackEntry {
        std::recursive_mutex mu;
        bool                 disposed{false};
        CancelCallback       cb;
    };

    CancelRegistry() = default;

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
            std::lock_guard<std::recursive_mutex> elock(entry->mu);
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
    /// - 排他性防悬挂保证: 若此时 cancel 正在另一线程执行该回调，elock
    /// 会阻塞等待其执行完毕，避免回调访问已被销毁的对象
    void unregisterCallback(RegId id) {
        if (id == 0) {
            return;
        }
        std::shared_ptr<CallbackEntry> entry;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto                        it = entries_.find(id);
            if (it != entries_.end()) {
                entry = std::move(it->second);
                entries_.erase(it);
            } else {
                auto ait = activeInvocations_.find(id);
                if (ait != activeInvocations_.end()) {
                    entry = ait->second;
                }
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
            std::lock_guard<std::recursive_mutex> elock(entry->mu);
            entry->disposed = true;
            entry->cb       = nullptr;
        }
    }

    /// 触发指定 key 的取消通知
    /// - 将 key 标记为已取消；提取该 key 下所有未注销的回调并在全局锁外安全执行
    void cancel(std::string_view key) {
        if (key.empty()) {
            return;
        }
        std::vector<std::pair<RegId, std::shared_ptr<CallbackEntry>>> toInvoke;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cancelledKeys_.insert(std::string(key));
            auto kit = keyToIds_.find(std::string(key));
            if (kit != keyToIds_.end()) {
                for (RegId id : kit->second) {
                    auto eit = entries_.find(id);
                    if (eit != entries_.end()) {
                        toInvoke.push_back({id, eit->second});
                        activeInvocations_[id] = eit->second;
                        entries_.erase(eit);
                    }
                    idToKey_.erase(id);
                }
                keyToIds_.erase(kit);
            }
        }
        // 在全局锁外执行回调，避免回调内部加锁导致死锁
        for (auto& [id, entry] : toInvoke) {
            {
                std::lock_guard<std::recursive_mutex> elock(entry->mu);
                if (!entry->disposed && entry->cb) {
                    try {
                        entry->cb();
                    } catch (...) {
                    }
                    entry->cb = nullptr;
                }
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                activeInvocations_.erase(id);
            }
        }
    }

    /// 触发所有正在运行任务的取消 (插件卸载或实例销毁时兜底调用)
    void cancelAll() {
        std::vector<std::pair<RegId, std::shared_ptr<CallbackEntry>>> toInvoke;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& [id, entry] : entries_) {
                toInvoke.push_back({id, entry});
                activeInvocations_[id] = entry;
            }
            entries_.clear();
            keyToIds_.clear();
            idToKey_.clear();
        }
        for (auto& [id, entry] : toInvoke) {
            {
                std::lock_guard<std::recursive_mutex> elock(entry->mu);
                if (!entry->disposed && entry->cb) {
                    try {
                        entry->cb();
                    } catch (...) {
                    }
                    entry->cb = nullptr;
                }
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                activeInvocations_.erase(id);
            }
        }
    }

    /// 查询指定 key 是否已被标记取消
    /// - 纯内存无跨线程调用，避免向宿主 IO 线程高频查询
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
            reg_(reg),
            id_(id) {}

        ~ScopedRegistration() {
            if (reg_ && id_ != 0) {
                reg_->unregisterCallback(id_);
            }
        }

        ScopedRegistration(ScopedRegistration&& o) noexcept :
            reg_(o.reg_),
            id_(o.id_) {
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
    std::unordered_map<RegId, std::shared_ptr<CallbackEntry>> activeInvocations_;
    std::unordered_map<std::string, std::vector<RegId>>       keyToIds_;
    std::unordered_map<RegId, std::string>                    idToKey_;
    std::unordered_set<std::string>                           cancelledKeys_;
};

/* ==================== 操作控制对象 (OpCtl) ==================== */

struct OpCtl {
    std::shared_ptr<std::atomic<bool>> cancelFlag;
    const AgentxxPluginHost*           host        = nullptr;
    const AgentxxPluginCancelIface*    cancelIface = nullptr;
    std::string                        threadId;
    CancelRegistry*                    cancelRegistry = nullptr;

    bool cancelled() const noexcept {
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
            return true;
        }
        if (cancelRegistry && !threadId.empty() && cancelRegistry->isCancelled(threadId)) {
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

/* ==================== 声明式模式构建器 ToolSchemaBuilder ==================== */

class ToolSchemaBuilder {
public:

    explicit ToolSchemaBuilder(ToolPromptText prompt = {}) :
        prompt_(std::move(prompt)) {}

    ToolSchemaBuilder& string(
        std::string_view           name,
        std::string_view           desc,
        bool                       required = false,
        std::optional<std::string> defVal   = std::nullopt
    ) {
        agentxx::util::Json prop;
        prop["type"]        = "string";
        prop["description"] = toolPromptArgDesc(prompt_, name, desc);
        if (defVal.has_value()) {
            prop["default"] = *defVal;
        }
        properties_[std::string(name)] = std::move(prop);
        if (required) {
            required_.push_back(std::string(name));
        }
        return *this;
    }

    ToolSchemaBuilder& integer(
        std::string_view       name,
        std::string_view       desc,
        bool                   required = false,
        std::optional<int64_t> defVal   = std::nullopt
    ) {
        agentxx::util::Json prop;
        prop["type"]        = "integer";
        prop["description"] = toolPromptArgDesc(prompt_, name, desc);
        if (defVal.has_value()) {
            prop["default"] = *defVal;
        }
        properties_[std::string(name)] = std::move(prop);
        if (required) {
            required_.push_back(std::string(name));
        }
        return *this;
    }

    ToolSchemaBuilder& number(
        std::string_view      name,
        std::string_view      desc,
        bool                  required = false,
        std::optional<double> defVal   = std::nullopt
    ) {
        agentxx::util::Json prop;
        prop["type"]        = "number";
        prop["description"] = toolPromptArgDesc(prompt_, name, desc);
        if (defVal.has_value()) {
            prop["default"] = *defVal;
        }
        properties_[std::string(name)] = std::move(prop);
        if (required) {
            required_.push_back(std::string(name));
        }
        return *this;
    }

    ToolSchemaBuilder& boolean(
        std::string_view    name,
        std::string_view    desc,
        bool                required = false,
        std::optional<bool> defVal   = std::nullopt
    ) {
        agentxx::util::Json prop;
        prop["type"]        = "boolean";
        prop["description"] = toolPromptArgDesc(prompt_, name, desc);
        if (defVal.has_value()) {
            prop["default"] = *defVal;
        }
        properties_[std::string(name)] = std::move(prop);
        if (required) {
            required_.push_back(std::string(name));
        }
        return *this;
    }

    ToolSchemaBuilder&
        stringArray(std::string_view name, std::string_view desc, bool required = false) {
        return array(name, desc, "string", required);
    }

    ToolSchemaBuilder& array(
        std::string_view name,
        std::string_view desc,
        std::string_view itemType = "string",
        bool             required = false
    ) {
        agentxx::util::Json prop;
        prop["type"]        = "array";
        prop["description"] = toolPromptArgDesc(prompt_, name, desc);
        prop["items"]       = agentxx::util::Json{
                  {"type", std::string(itemType)}
        };
        properties_[std::string(name)] = std::move(prop);
        if (required) {
            required_.push_back(std::string(name));
        }
        return *this;
    }

    ToolSchemaBuilder& enumString(
        std::string_view           name,
        std::string_view           desc,
        std::vector<std::string>   options,
        bool                       required = false,
        std::optional<std::string> defVal   = std::nullopt
    ) {
        agentxx::util::Json prop;
        prop["type"]        = "string";
        prop["description"] = toolPromptArgDesc(prompt_, name, desc);
        prop["enum"]        = options;
        if (defVal.has_value()) {
            prop["default"] = *defVal;
        }
        properties_[std::string(name)] = std::move(prop);
        if (required) {
            required_.push_back(std::string(name));
        }
        return *this;
    }

    std::string build() const {
        agentxx::util::Json schema;
        schema["type"]       = "object";
        schema["properties"] = properties_;
        if (!required_.empty()) {
            schema["required"] = required_;
        } else {
            schema["required"] = agentxx::util::Json::array();
        }
        return schema.dump();
    }

private:

    ToolPromptText           prompt_;
    agentxx::util::Json      properties_ = agentxx::util::Json::object();
    std::vector<std::string> required_;
};

/* ==================== 强类型参数提取器 ArgReader ==================== */

namespace detail {
template<typename T>
inline T jsonGet(const agentxx::util::Json& j) {
    if constexpr (std::is_same_v<T, std::string>) {
        return j.get<std::string>();
    } else if constexpr (std::is_same_v<T, bool>) {
        return j.get<bool>();
    } else if constexpr (std::is_same_v<T, double>) {
        return j.get<double>();
    } else if constexpr (std::is_same_v<T, float>) {
        return j.get<float>();
    } else if constexpr (std::is_same_v<T, long long>) {
        return j.get<long long>();
    } else if constexpr (std::is_same_v<T, unsigned long long>) {
        return j.get<unsigned long long>();
    } else if constexpr (std::is_same_v<T, long>) {
        return j.get<long>();
    } else if constexpr (std::is_same_v<T, unsigned long>) {
        return j.get<unsigned long>();
    } else if constexpr (std::is_same_v<T, int>) {
        return j.get<int>();
    } else if constexpr (std::is_same_v<T, unsigned int>) {
        return j.get<unsigned>();
    } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
        return j.get<std::vector<std::string>>();
    } else if constexpr (std::is_same_v<T, agentxx::util::Json>) {
        return j.get<agentxx::util::Json>();
    } else {
        return j.get<T>();
    }
}
} // namespace detail

class ArgReader {
public:

    explicit ArgReader(std::string_view jsonStr) {
        if (!jsonStr.empty()) {
            try {
                root_ = agentxx::util::Json::parse(jsonStr);
                if (!root_.is_object()) {
                    root_ = agentxx::util::Json::object();
                }
            } catch (...) {
                hasParseError_ = true;
                root_          = agentxx::util::Json::object();
            }
        } else {
            root_ = agentxx::util::Json::object();
        }
    }

    bool hasParseError() const noexcept {
        return hasParseError_;
    }

    template<typename T>
    std::optional<T> get(std::string_view key) const {
        if (hasParseError_ || !root_.is_object()) {
            return std::nullopt;
        }
        if (!root_.contains(key)) {
            return std::nullopt;
        }
        auto val = root_[key];
        if (val.is_null()) {
            return std::nullopt;
        }

        try {
            if constexpr (std::is_same_v<T, agentxx::util::Json>) {
                return val;
            } else if constexpr (std::is_same_v<T, std::string>) {
                if (val.is_string()) {
                    return detail::jsonGet<std::string>(val);
                }
                return val.dump();
            } else if constexpr (std::is_same_v<T, bool>) {
                if (val.is_boolean()) {
                    return detail::jsonGet<bool>(val);
                }
                if (val.is_number()) {
                    return detail::jsonGet<long long>(val) != 0;
                }
                if (val.is_string()) {
                    auto s = detail::jsonGet<std::string>(val);
                    return s == "true" || s == "1" || s == "yes";
                }
            } else if constexpr (std::is_integral_v<T>) {
                if (val.is_number_integer()) {
                    return static_cast<T>(detail::jsonGet<long long>(val));
                }
                if (val.is_number()) {
                    return static_cast<T>(detail::jsonGet<double>(val));
                }
                if (val.is_string()) {
                    return static_cast<T>(std::stoll(detail::jsonGet<std::string>(val)));
                }
            } else if constexpr (std::is_floating_point_v<T>) {
                if (val.is_number()) {
                    return static_cast<T>(detail::jsonGet<double>(val));
                }
                if (val.is_string()) {
                    return static_cast<T>(std::stod(detail::jsonGet<std::string>(val)));
                }
            } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
                if (val.is_array()) {
                    std::vector<std::string> res;
                    for (const auto& elem : val) {
                        if (elem.is_string()) {
                            res.push_back(detail::jsonGet<std::string>(elem));
                        } else {
                            res.push_back(elem.dump());
                        }
                    }
                    return res;
                }
                if (val.is_string()) {
                    return std::vector<std::string>{detail::jsonGet<std::string>(val)};
                }
            } else {
                return detail::jsonGet<T>(val);
            }
        } catch (...) {
            return std::nullopt;
        }
        return std::nullopt;
    }

    template<typename T>
    T value(std::string_view key, const T& fallback) const {
        auto opt = get<T>(key);
        return opt.has_value() ? *opt : fallback;
    }

    std::string value(std::string_view key, const char* fallback) const {
        auto opt = get<std::string>(key);
        return opt.has_value() ? *opt : std::string(fallback ? fallback : "");
    }

    template<typename T>
    T require(std::string_view key) {
        auto opt = get<T>(key);
        if (!opt.has_value()) {
            errors_.push_back(fmt::format("Missing or invalid required argument: '{}'", key));
            return T{};
        }
        return *opt;
    }

    bool ok() const noexcept {
        return errors_.empty() && !hasParseError_;
    }

    std::string errorMessage() const {
        if (hasParseError_) {
            return "Failed to parse arguments JSON";
        }
        if (errors_.empty()) {
            return {};
        }
        return fmt::format("Argument error: {}", fmt::join(errors_, "; "));
    }

    const agentxx::util::Json& raw() const noexcept {
        return root_;
    }

private:

    agentxx::util::Json      root_          = agentxx::util::Json::object();
    bool                     hasParseError_ = false;
    std::vector<std::string> errors_;
};

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
    // 子 Task 在 final_suspend 已通过 continuation 恢复父协程；父协程的
    // await_resume 负责读取结果并销毁子帧。这里不能走 root notify/destroy。
    if (p.continuation_) {
        return;
    }

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

/* ==================== 根操作 Request 与完成守卫 ====================
 *
 * tool / hook / capability / graph / spawn 这些"根操作"共享同一套协议：
 * 宿主把 args/session/call_id/method 以**只读借用视图**传入 start，插件必须在
 * 本次调用内复制需要跨挂起保留的数据；接受之后必须 exactly-once 完成。
 * 下面两个类型把这套协议集中在一处，避免每个 helper 各写一遍。
 */

/// 根操作拥有型输入（plugin.md 6.2 / F13）。
///
/// - C ABI 传入的视图只在 start 调用期间有效，本结构先把它们复制成自己的
///   `std::string`，业务代码拿到的 `std::string_view` 因此在整个根操作期间有效；
/// - 同步路径的 Request 在栈上、异步路径的 Request 在 Job 里持有，随完成回调
///   一起释放；
/// - 业务需要跨操作保留时仍要自己复制（Request 之外不保证）。
struct RootRequest {
    std::string argsJson;  ///< 工具参数 / 能力参数 JSON（空则为 "{}"）
    std::string sessionId; ///< ABI thread_id / session_id
    std::string callId;    ///< 工具 tool_call_id；能力方法名放 method
    std::string method;    ///< 能力方法名（非能力操作为空）

    const AgentxxPluginHost*        host        = nullptr;
    const AgentxxPluginCancelToken* cancelToken = nullptr; ///< offload/worker 内的取消令牌视图

    static std::string copyView(const AgentxxPluginStringView* sv, const char* fallback = "") {
        if (!sv || (!sv->data && sv->size == 0)) {
            return std::string{fallback};
        }
        return std::string(sv->data ? sv->data : "", static_cast<size_t>(sv->size));
    }

    /// 工具：args/session/tool_call_id
    static RootRequest forTool(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* args,
        const AgentxxPluginStringView* session,
        const AgentxxPluginStringView* call
    ) {
        RootRequest req;
        req.host      = host;
        req.argsJson  = copyView(args, "{}");
        req.sessionId = copyView(session);
        req.callId    = copyView(call);
        return req;
    }

    /// 钩子：node_input_json（args 为空时按 "{}"）
    static RootRequest forHook(
        const AgentxxPluginHost* host, const AgentxxPluginStringView* nodeInputJson
    ) {
        RootRequest req;
        req.host     = host;
        req.argsJson = copyView(nodeInputJson, "{}");
        return req;
    }

    /// 能力：method/args
    static RootRequest forCapability(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* method,
        const AgentxxPluginStringView* args
    ) {
        RootRequest req;
        req.host     = host;
        req.method   = copyView(method);
        req.argsJson = copyView(args, "{}");
        return req;
    }

    std::string_view args() const noexcept { return argsJson; }
    std::string_view session() const noexcept { return sessionId; }
    std::string_view call() const noexcept { return callId; }
    std::string_view capMethod() const noexcept { return method; }
};

/// 根操作完成守卫：保证 notify.done 恰好一次，并把异常统一映射为终态。
///
/// 规则（plugin.md 不可变原则 5）：
/// - 业务代码显式调用 ok()/failed()/cancelled() 表示完成；
/// - 作用域结束时仍未完成（提前 return 等）时析构补一次 FAILED，绝不留下
///   "已接受但永远不 done"的操作；
/// - 重复完成只记录，不重复回调宿主。
///
/// 异步路径（Task）由 `PromiseBase` 的 notify_/cancelFlag_ 在同一处收束，
/// 语义与这里的同步路径一致。
class CompletionGuard {
public:

    explicit CompletionGuard(const AgentxxPluginOperatorNotify* notify) :
        notify_(notify ? *notify : AgentxxPluginOperatorNotify{nullptr, nullptr}) {}

    CompletionGuard(const CompletionGuard&)            = delete;
    CompletionGuard& operator=(const CompletionGuard&) = delete;

    bool completed() const noexcept {
        return done_.load(std::memory_order_acquire);
    }

    /// 完成一次（`status` 为 AGENTXX_PLUGIN_OPERATOR_*）；重复调用返回 false。
    bool complete(int32_t status, std::string_view payload) noexcept {
        if (!notify_.done) {
            done_.store(true, std::memory_order_release);
            return false;
        }
        bool expected = false;
        if (!done_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return false;
        }
        try {
            auto sv = PluginStringView::from(payload.data(), payload.size());
            notify_.done(notify_.host_ud, status, &sv);
        } catch (...) {
            // 宿主回调不得抛异常；即便抛了也已经置位，不重复派发。
        }
        return true;
    }

    void ok(std::string_view payload = {}) {
        complete(AGENTXX_PLUGIN_OPERATOR_OK, payload);
    }

    void failed(std::string_view error) {
        complete(AGENTXX_PLUGIN_OPERATOR_FAILED, error);
    }

    void cancelled(std::string_view reason = {}) {
        complete(AGENTXX_PLUGIN_OPERATOR_CANCELLED, reason);
    }

    /// 在 catch 块中调用：把当前异常映射为终态（取消异常映射为 CANCELLED）。
    void fromCurrentException() noexcept {
        try {
            throw;
        } catch (const CancelledException& e) {
            cancelled(e.what());
        } catch (const std::exception& e) {
            failed(e.what());
        } catch (...) {
            failed("unknown plugin error");
        }
    }

    ~CompletionGuard() {
        if (!completed()) {
            complete(AGENTXX_PLUGIN_OPERATOR_FAILED, "plugin did not complete the operation");
        }
    }

private:

    AgentxxPluginOperatorNotify notify_{nullptr, nullptr};
    std::atomic<bool>           done_{false};
};

template<typename T>
struct PromiseBase {
    using value_type = T;

    AgentxxPluginOperatorNotify        notify_{nullptr, nullptr};
    const AgentxxPluginHost*           host_{nullptr};
    std::shared_ptr<std::atomic<bool>> cancelFlag_{nullptr};
    std::function<void()>              outstandingCancel_{nullptr};
    std::exception_ptr                 exception_{nullptr};
    std::function<void()>              opCleanup_{nullptr};
    std::coroutine_handle<>            continuation_{};

    std::suspend_always initial_suspend() noexcept {
        return {};
    }

    struct FinalAwaiter {
        bool await_ready() const noexcept { return false; }

        template<typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) const noexcept {
            auto continuation = h.promise().continuation_;
            return continuation ? continuation : std::noop_coroutine();
        }

        void await_resume() const noexcept {}
    };

    FinalAwaiter final_suspend() noexcept {
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

    struct Awaiter {
        std::coroutine_handle<promise_type> handle;

        bool await_ready() const noexcept { return !handle || handle.done(); }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent) {
            handle.promise().continuation_ = parent;
            return handle;
        }

        T await_resume() {
            auto h = std::exchange(handle, {});
            if (!h) {
                return T{};
            }
            if (h.promise().has_exception()) {
                auto exception = h.promise().exception();
                h.destroy();
                std::rethrow_exception(exception);
            }
            T result = std::move(h.promise().result());
            h.destroy();
            return result;
        }
    };

    Awaiter operator co_await() && noexcept {
        return Awaiter{std::exchange(handle_, {})};
    }
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

    struct Awaiter {
        std::coroutine_handle<promise_type> handle;

        bool await_ready() const noexcept { return !handle || handle.done(); }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent) {
            handle.promise().continuation_ = parent;
            return handle;
        }

        void await_resume() {
            auto h = std::exchange(handle, {});
            if (h && h.promise().has_exception()) {
                auto exception = h.promise().exception();
                h.destroy();
                std::rethrow_exception(exception);
            }
            if (h) {
                h.destroy();
            }
        }
    };

    Awaiter operator co_await() && noexcept {
        return Awaiter{std::exchange(handle_, {})};
    }
};

/* ==================== 插件实例上下文基类 ==================== */

class PluginBase {
public:

    const AgentxxPluginHost* host = nullptr;
    AgentIfaces              iface;
    Logger                   log;
    CancelRegistry cancelRegistry; ///< 框架级事件驱动取消注册表 (每个实例独立一份)

    PluginBase() = default;

    virtual ~PluginBase() {
        if (lifeToken_) {
            lifeToken_->store(false, std::memory_order_release);
        }
        cancelRegistry.cancelAll();
        stopSpawns();
    }

    std::shared_ptr<std::atomic<bool>> lifeToken() const {
        return lifeToken_;
    }

    void init(const AgentxxPluginHost* h) {
        host         = h;
        iface        = AgentIfaces::query(h);
        log.host     = h;
        log.logIface = iface.log;
        log.logFn    = (iface.log && iface.log->log) ? iface.log->log : nullptr;

        // 挂钩会话轮次开始：自动为当前会话重置 cancelRegistry
        if (iface.events && iface.events->subscribe) {
            auto topicSv   = PluginStringView::fromCstr("plugin.agentxx.round_start");
            roundStartSub_ = iface.events->subscribe(
                host,
                &topicSv,
                [](const AgentxxPluginStringView* ev, void* ud) {
                    auto* self = static_cast<PluginBase*>(ud);
                    if (!self || !ev || !ev->data) {
                        return;
                    }
                    try {
                        auto j = agentxx::util::Json::parse(
                            std::string_view{ev->data, static_cast<size_t>(ev->size)}
                        );
                        std::string sid = j.value("sessionId", "");
                        if (!sid.empty()) {
                            self->cancelRegistry.clearCancelled(sid);
                        }
                    } catch (...) {
                    }
                },
                this
            );
        }
    }

    ToolSchemaBuilder schema(std::string_view toolName) const {
        return ToolSchemaBuilder(toolPrompt(toolName));
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
            auto j = agentxx::util::Json::parse(jsonStr);
            if (j.contains("depict") && j["depict"].is_string()) {
                res.depict = j["depict"].get<std::string>();
            }
            if (j.contains("args") && j["args"].is_object()) {
                for (const auto& [k, v] : j["args"].items()) {
                    if (v.is_string()) {
                        util::insertOrAssignHeterogeneous(res.args, k, v.get<std::string>());
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

    /// 会话是否已取消 (优化版: 优先本地无抖动查询)
    /// - 宿主下发 cancel 时已通过 execute_cancel 写入 cancelRegistry
    /// - 故本地为 true 时必然已取消，直接返回 true，避免任何跨线程通信
    bool sessionCancelled(AgentxxPluginStringView tid) const {
        if (!tid.data || tid.size == 0) {
            return false;
        }
        std::string_view sv(tid.data, static_cast<size_t>(tid.size));
        if (cancelRegistry.isCancelled(sv)) {
            return true;
        }
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
        /// 后台任务的 OpCtl 由记录持有：任务协程以引用接收它，必须在挂起后仍然
        /// 有效（放在 starter 栈上会悬垂）。
        std::shared_ptr<OpCtl> ctl;
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

    template<typename Self, typename Fn>
    void spawn(this Self& self, Fn&& fn);

private:

    std::shared_ptr<std::atomic<bool>> lifeToken_     = std::make_shared<std::atomic<bool>>(true);
    AgentxxPluginSubscription*         roundStartSub_ = nullptr;
};

/* ==================== 锚定原语 awaiter 族 ==================== */

namespace detail {

struct SleepAwaiter {
    const AgentxxPluginHost*           host;
    const AgentxxPluginSchedulerIface* sched;
    int64_t                            ms;
    AgentxxPluginOperatorHandle*       operation = nullptr;
    std::string                        error;

    bool await_ready() noexcept {
        if (ms <= 0) {
            return true;
        }
        if (!sched || !sched->sleep) {
            error = "scheduler sleep is unavailable";
            return true;
        }
        return false;
    }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        auto& p = h.promise();
        AgentxxPluginString errorOut{};
        operation = sched->sleep(
            host,
            ms,
            [](void* ud, int32_t status, const AgentxxPluginStringView* payload) {
                auto handle = std::coroutine_handle<Promise>::from_address(ud);
                auto& prom   = handle.promise();
                prom.clear_outstanding();
                if (status == AGENTXX_PLUGIN_OPERATOR_CANCELLED) {
                    prom.set_exception(std::make_exception_ptr(
                        CancelledException("sleep cancelled")
                    ));
                } else if (status == AGENTXX_PLUGIN_OPERATOR_FAILED) {
                    auto message = PluginStringView::str(payload);
                    prom.set_exception(std::make_exception_ptr(std::runtime_error(
                        message.empty() ? "sleep failed" : std::string(message)
                    )));
                }
                try {
                    handle.resume();
                } catch (...) {
                    prom.set_exception(std::current_exception());
                }
                finishIfDone(handle);
            },
            h.address(),
            &errorOut
        );
        if (!operation) {
            error = PluginStringView::str(&errorOut);
            PluginString::free(host, &errorOut);
            return false;
        }
        p.set_outstanding([sched = this->sched, operation = this->operation]() {
            if (sched && sched->op_cancel && operation) {
                sched->op_cancel(operation);
            }
        });
        return true;
    }

    void await_resume() const {
        if (!error.empty()) {
            throw std::runtime_error(error);
        }
    }
};

struct YieldAwaiter {
    const AgentxxPluginHost*           host;
    const AgentxxPluginSchedulerIface* sched;
    std::string                        error;

    bool await_ready() noexcept {
        if (!sched || !sched->post_to_io) {
            error = "scheduler yield is unavailable";
            return true;
        }
        return false;
    }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        auto status = sched->post_to_io(
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
        if (status != 0) {
            error = "scheduler yield: failed to post to IO";
            return false;
        }
        return true;
    }

    void await_resume() const {
        if (!error.empty()) {
            throw std::runtime_error(error);
        }
    }
};

template<typename WorkFn>
struct OffloadAwaiter {
    using ResultType = std::decay_t<
        std::invoke_result_t<WorkFn, const AgentxxPluginCancelToken*>>;

    const AgentxxPluginHost*           host;
    const AgentxxPluginSchedulerIface* sched;
    WorkFn                             work;
    std::exception_ptr                 exPtr      = nullptr;
    std::conditional_t<std::is_void_v<ResultType>, std::monostate, std::optional<ResultType>>
        result;
    AgentxxPluginOperatorHandle* operation = nullptr;
    int32_t                      status = AGENTXX_PLUGIN_OPERATOR_OK;
    std::string                  error;

    bool await_ready() noexcept {
        if (!sched || !sched->offload) {
            error = "scheduler offload is unavailable";
            return true;
        }
        return false;
    }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        auto& p   = h.promise();
        coroAddr_ = h.address();
        AgentxxPluginString errorOut{};
        operation = sched->offload(
            host,
            [](void* ud, const AgentxxPluginCancelToken* token, AgentxxPluginString* error_out)
                -> void* {
                auto* self = static_cast<OffloadAwaiter*>(ud);
                try {
                    if constexpr (std::is_void_v<ResultType>) {
                        self->work(token);
                    } else {
                        self->result = self->work(token);
                    }
                } catch (const CancelledException&) {
                    self->exPtr = std::current_exception();
                } catch (...) {
                    self->exPtr = std::current_exception();
                }
                (void)error_out;
                return nullptr;
            },
            [](void* ud, int32_t status, void*, const AgentxxPluginStringView* err) {
                auto* self   = static_cast<OffloadAwaiter*>(ud);
                self->status = status;
                self->error = PluginStringView::str(err);
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
            this,
            &errorOut
        );
        if (!operation) {
            error = PluginStringView::str(&errorOut);
            PluginString::free(host, &errorOut);
            return false;
        }
        p.set_outstanding([sched = this->sched, operation = this->operation]() {
            if (sched && sched->op_cancel && operation) {
                sched->op_cancel(operation);
            }
        });
        return true;
    }

    ResultType await_resume() {
        if (!error.empty() && status == AGENTXX_PLUGIN_OPERATOR_FAILED) {
            throw std::runtime_error(error);
        }
        if (exPtr) {
            std::rethrow_exception(exPtr);
        }
        if (status == AGENTXX_PLUGIN_OPERATOR_CANCELLED) {
            throw CancelledException("offload cancelled");
        }
        if constexpr (!std::is_void_v<ResultType>) {
            return std::move(*result);
        }
    }

    void* coroAddr_ = nullptr;
};

enum class AwaiterState : uint32_t {
    INIT      = 0,
    CALLING   = 1,
    SUSPENDED = 2,
    COMPLETED = 3
};

template<typename Promise>
inline void resumeCoroutine(const AgentxxPluginHost* host, std::coroutine_handle<Promise> handle) {
    bool needPost = false;
    if (host) {
        auto ifs = AgentIfaces::query(host);
        if (ifs.scheduler && ifs.scheduler->is_io_thread) {
            needPost = !ifs.scheduler->is_io_thread(host);
        }
    }
    if (needPost) {
        auto ifs = AgentIfaces::query(host);
        if (ifs.scheduler && ifs.scheduler->post_to_io) {
            struct ResumeData {
                std::coroutine_handle<Promise> h;
            };

            auto* d = new ResumeData{handle};
            const auto status = ifs.scheduler->post_to_io(
                host,
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
            if (status == 0) {
                return;
            }
            delete d;
        }
    }
    try {
        handle.resume();
    } catch (...) {
        handle.promise().set_exception(std::current_exception());
    }
    detail::finishIfDone(handle);
}

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
    std::atomic<AwaiterState>      state{AwaiterState::INIT};
    void*                          coroAddr            = nullptr;
    void (*schedPost)(const AgentxxPluginHost*, void*) = nullptr;
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

    bool await_ready() const noexcept {
        return !st || !st->tools || !st->tools->call_tool_async;
    }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        st->coroAddr = h.address();
        st->state.store(AwaiterState::CALLING, std::memory_order_release);

        auto*               holder = new std::shared_ptr<CallToolState>(st);
        AgentxxPluginString err{nullptr, 0};
        auto                nameSv = PluginStringView::from(st->name.data(), st->name.size());
        auto argsSv = PluginStringView::from(st->argsJson.data(), st->argsJson.size());
        auto tidSv  = PluginStringView::from(st->threadId.data(), st->threadId.size());

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

                auto expected = AwaiterState::CALLING;
                if (s->state.compare_exchange_strong(
                        expected,
                        AwaiterState::COMPLETED,
                        std::memory_order_acq_rel
                    )) {
                    return;
                }

                auto handle = std::coroutine_handle<Promise>::from_address(s->coroAddr);
                handle.promise().clear_outstanding();

                resumeCoroutine(s->host, handle);
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

        auto expected = AwaiterState::CALLING;
        if (st->state.compare_exchange_strong(
                expected,
                AwaiterState::SUSPENDED,
                std::memory_order_acq_rel
            )) {
            h.promise().set_outstanding([st = this->st]() {
                if (st->tools && st->tools->op_cancel && st->opHandle) {
                    st->tools->op_cancel(st->opHandle);
                }
            });
            return true;
        }

        return false;
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
    std::atomic<AwaiterState>             state{AwaiterState::INIT};
    void*                                 coroAddr     = nullptr;
    void (*schedPost)(const AgentxxPluginHost*, void*) = nullptr;
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

    bool await_ready() const noexcept {
        return !st || !st->caps || !st->caps->invoke_capability_async;
    }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        st->coroAddr = h.address();
        st->state.store(AwaiterState::CALLING, std::memory_order_release);

        auto*               holder = new std::shared_ptr<InvokeCapState>(st);
        AgentxxPluginString err{nullptr, 0};
        auto capSv  = PluginStringView::from(st->capability.data(), st->capability.size());
        auto methSv = PluginStringView::from(st->method.data(), st->method.size());
        auto argsSv = PluginStringView::from(st->argsJson.data(), st->argsJson.size());

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

                auto expected = AwaiterState::CALLING;
                if (s->state.compare_exchange_strong(
                        expected,
                        AwaiterState::COMPLETED,
                        std::memory_order_acq_rel
                    )) {
                    return;
                }

                auto handle = std::coroutine_handle<Promise>::from_address(s->coroAddr);
                handle.promise().clear_outstanding();

                resumeCoroutine(s->host, handle);
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

        auto expected = AwaiterState::CALLING;
        if (st->state.compare_exchange_strong(
                expected,
                AwaiterState::SUSPENDED,
                std::memory_order_acq_rel
            )) {
            h.promise().set_outstanding([st = this->st]() {
                if (st->caps && st->caps->op_cancel && st->opHandle) {
                    st->caps->op_cancel(st->opHandle);
                }
            });
            return true;
        }

        return false;
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
    AgentxxPluginOperatorHandle* taskHandle = nullptr;
    if (ctx.iface.tasks && ctx.iface.tasks->register_task) {
        AgentxxPluginOperatorNotify  notify{nullptr, nullptr};
        AgentxxPluginString          err{nullptr, 0};
        taskHandle = ctx.iface.tasks->register_task(
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
        if (taskHandle) {
            hostNotify = notify;
        } else {
            if (err.data) {
                ctx.log.warn(fmt::format(
                    "spawn: register_task failed: {}",
                    std::string_view{err.data, static_cast<size_t>(err.size)}
                ));
                if (ctx.host) {
                    PluginString::free(ctx.host, &err);
                }
            } else {
                ctx.log.warn("spawn: register_task failed");
            }
            return;
        }
    } else {
        ctx.log.warn("spawn: host has no agentxx.agent.tasks iface");
        return;
    }

    auto starter = [&ctx, fn, cancelFlag, rec, recWeak, hostNotify]() {
        // 任务协程以引用接收 ctl：必须由 SpawnRecord 持有到任务结束（不能放在
        // starter 的栈上，否则任务一挂起就悬垂）。
        if (!rec->ctl) {
            rec->ctl = std::make_shared<OpCtl>(
                OpCtl{cancelFlag, ctx.host, ctx.iface.cancel, ""}
            );
        }
        auto  task = fn(ctx, *rec->ctl);
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
        const auto status = ctx.iface.scheduler->post_to_io(
            ctx.host,
            [](void* ud) {
                auto* rec = static_cast<PluginBase::SpawnRecord*>(ud);
                if (rec && rec->starter) {
                    rec->starter();
                }
            },
            raw
        );
        if (status != 0 && hostNotify.done) {
            auto message = PluginStringView::fromCstr("spawn: failed to post task to IO");
            hostNotify.done(hostNotify.host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &message);
        }
    } else if (hostNotify.done) {
        auto message = PluginStringView::fromCstr("spawn: scheduler unavailable");
        hostNotify.done(hostNotify.host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &message);
    }
}

} // namespace detail

template<typename Self, typename Fn>
void PluginBase::spawn(this Self& self, Fn&& fn) {
    detail::spawnTaskImpl(self, std::forward<Fn>(fn));
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
        /// 拥有 args/session/tool_call_id：业务协程可能挂起后继续读取这些视图
        /// （F13），因此它们必须比 start 调用活得久。
        detail::RootRequest request;
        /// OpCtl 同样必须由 Job 拥有：业务协程以引用接收它，挂起后仍会读取
        /// （例如 ctl.throw_if_cancelled()），放在 start 栈上会悬垂。
        OpCtl ctl;
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
        (void)error_out;
        // 先建立拥有型 Request，再把视图交给业务代码：协程挂起期间 args/
        // session/tool_call_id 由 Job 持有，不再指向宿主借用缓冲区。
        auto request = detail::RootRequest::forTool(
            shim && shim->ctx ? shim->ctx->host : nullptr,
            args_json,
            thread_id,
            tool_call_id
        );
        CancelRegistry* cancelReg = nullptr;
        if (shim->ctx) {
            cancelReg = &shim->ctx->cancelRegistry;
        }
        auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
        if (cancelReg && !request.sessionId.empty() && cancelReg->isCancelled(request.sessionId)) {
            cancelFlag->store(true, std::memory_order_release);
        }

        OpCtl ctl{
            cancelFlag,
            shim->ctx ? shim->ctx->host : nullptr,
            shim->ctx ? shim->ctx->iface.cancel : nullptr,
            request.sessionId,
            cancelReg
        };

        auto* job = new Job{shim, cancelFlag, nullptr, std::move(request), std::move(ctl)};

        auto task = shim->fn(*shim->ctx, job->request.args(), job->ctl);
        if (!task.handle_) {
            delete job;
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
            delete job;
            return nullptr;
        }

        job->coroAddr = h.address();
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
        if (job->shim && job->shim->ctx && !job->request.sessionId.empty()) {
            job->shim->ctx->cancelRegistry.cancel(job->request.sessionId);
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
        BlockShim*                  shim = nullptr;
        AgentxxPluginOperatorNotify notify{};
        std::string                 args;
        std::string                 tid;
        std::string                 tcid;
        std::string                 workDir;
        std::string                 argsJson;
        std::string                 resultPayload;
        std::string                 errorPayload;
        bool                        isCancelled = false;
        AgentxxPluginOperatorHandle* offloadHandle = nullptr;
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
        std::string argsJsonCache;
        if (shim && shim->ctx) {
            workDirCache  = shim->ctx->workDir(tidStr);
            argsJsonCache = shim->ctx->argsJson();
            if (!tidStr.empty() && shim->ctx->cancelRegistry.isCancelled(tidStr)) {
                shim->ctx->cancelRegistry.cancel(tidStr);
            }
        }
        auto* job = new Job{
            .shim   = shim,
            .notify = notify ? *notify : AgentxxPluginOperatorNotify{nullptr, nullptr},
            .args   = std::string(
                args_json && args_json->data ? args_json->data : "{}",
                args_json ? static_cast<size_t>(args_json->size) : 0
            ),
            .tid  = std::move(tidStr),
            .tcid = std::string(
                tool_call_id && tool_call_id->data ? tool_call_id->data : "",
                tool_call_id ? static_cast<size_t>(tool_call_id->size) : 0
            ),
            .workDir       = std::move(workDirCache),
            .argsJson      = std::move(argsJsonCache),
            .resultPayload = {},
            .errorPayload  = {},
            .isCancelled   = false
        };

        AgentxxPluginString scheduleError{};
        if (shim && shim->ctx && shim->ctx->iface.scheduler
            && shim->ctx->iface.scheduler->offload) {
            job->offloadHandle = shim->ctx->iface.scheduler->offload(
                shim->ctx->host,
                [](void* ud, const AgentxxPluginCancelToken* token, AgentxxPluginString* err_out)
                    -> void* {
                    (void)err_out;
                    auto* j = static_cast<Job*>(ud);
                    try {
                        if constexpr (std::is_invocable_v<
                                          BlockFn,
                                          Ctx&,
                                          std::string_view,
                                          std::string_view,
                                          std::string_view,
                                          const AgentxxPluginCancelToken*>) {
                            j->resultPayload
                                = j->shim->fn(*j->shim->ctx, j->args, j->tid, j->workDir, token);
                        } else if constexpr (std::is_invocable_v<
                                                 BlockFn,
                                                 Ctx&,
                                                 std::string_view,
                                                 std::string_view,
                                                 const AgentxxPluginCancelToken*>) {
                            j->resultPayload = j->shim->fn(*j->shim->ctx, j->args, j->tid, token);
                        } else if constexpr (std::is_invocable_v<
                                                 BlockFn,
                                                 Ctx&,
                                                 std::string_view,
                                                 std::string_view,
                                                 std::string_view>) {
                            j->resultPayload
                                = j->shim->fn(*j->shim->ctx, j->args, j->tid, j->workDir);
                        } else if constexpr (std::is_invocable_v<
                                                 BlockFn,
                                                 Ctx&,
                                                 std::string_view,
                                                 const AgentxxPluginCancelToken*>) {
                            j->resultPayload = j->shim->fn(*j->shim->ctx, j->args, token);
                        } else if constexpr (std::is_invocable_v<
                                                 BlockFn,
                                                 std::string_view,
                                                 const AgentxxPluginCancelToken*>) {
                            j->resultPayload = j->shim->fn(j->args, token);
                        } else if constexpr (std::is_invocable_v<
                                                 BlockFn,
                                                 Ctx&,
                                                 std::string_view,
                                                 std::string_view>) {
                            j->resultPayload = j->shim->fn(*j->shim->ctx, j->args, j->tid);
                        } else if constexpr (std::is_invocable_v<BlockFn, Ctx&, std::string_view>) {
                            j->resultPayload = j->shim->fn(*j->shim->ctx, j->args);
                        } else {
                            j->resultPayload = j->shim->fn(j->args);
                        }
                    } catch (const CancelledException& e) {
                        j->isCancelled  = true;
                        j->errorPayload = e.what();
                    } catch (const std::exception& e) {
                        j->errorPayload = e.what();
                    } catch (...) {
                        j->errorPayload = "unknown blocking tool error";
                    }
                    return nullptr;
                },
                [](void* ud, int32_t status, void* res, const AgentxxPluginStringView* err) {
                    (void)res;
                    auto*                   j       = static_cast<Job*>(ud);
                    int32_t                 st      = status;
                    AgentxxPluginStringView payload = PluginStringView::from(nullptr, 0);

                    if (!PluginStringView::empty(err)) {
                        st      = AGENTXX_PLUGIN_OPERATOR_FAILED;
                        payload = *err;
                    } else if (!j->errorPayload.empty()) {
                        if (j->isCancelled) {
                            st = AGENTXX_PLUGIN_OPERATOR_CANCELLED;
                        } else {
                            st = AGENTXX_PLUGIN_OPERATOR_FAILED;
                        }
                        payload = PluginStringView::from(
                            j->errorPayload.data(),
                            j->errorPayload.size()
                        );
                    } else if (st == AGENTXX_PLUGIN_OPERATOR_CANCELLED || j->isCancelled) {
                        st = AGENTXX_PLUGIN_OPERATOR_CANCELLED;
                    } else {
                        st      = AGENTXX_PLUGIN_OPERATOR_OK;
                        payload = PluginStringView::from(
                            j->resultPayload.data(),
                            j->resultPayload.size()
                        );
                    }

                    if (j->notify.done) {
                        j->notify.done(j->notify.host_ud, st, &payload);
                    }
                    // OpCore::onEndpointDone 先在当前调用内线性化
                    // completionSubmitted_，因此 notify.done 返回后，宿主
                    // cancel 入口不会再把这个 provider handle 交回插件。
                    // 这使得 Job 可以在这里直接回收，不依赖第二个异步
                    // post，也不会留下自引用控制块泄漏。
                    delete j;
                },
                job,
                &scheduleError
            );
            if (!job->offloadHandle) {
                if (scheduleError.data) {
                    if (error_out) {
                        *error_out = scheduleError;
                        scheduleError = {};
                    } else {
                        PluginString::free(shim->ctx->host, &scheduleError);
                    }
                } else if (error_out) {
                    *error_out = PluginString::fromCstr(
                        shim->ctx->host,
                        "blocking tool: scheduler offload rejected"
                    );
                }
                delete job;
                return nullptr;
            }
        } else {
            if (error_out && shim && shim->ctx) {
                *error_out = PluginString::fromCstr(
                    shim->ctx->host,
                    "blocking tool: scheduler offload unavailable"
                );
            }
            delete job;
            return nullptr;
        }
        return job;
    };

    spec.execute_cancel = [](void* user_data, void* op) {
        (void)user_data;
        if (!op) {
            return;
        }
        auto* job       = static_cast<Job*>(op);
        if (job->offloadHandle && job->shim && job->shim->ctx
            && job->shim->ctx->iface.scheduler
            && job->shim->ctx->iface.scheduler->op_cancel) {
            job->shim->ctx->iface.scheduler->op_cancel(job->offloadHandle);
        }
        if (job->shim && job->shim->ctx && !job->tid.empty()) {
            job->shim->ctx->cancelRegistry.cancel(job->tid);
        }
    };

    if (ctx.iface.tools && ctx.iface.tools->register_tool) {
        ctx.iface.tools->register_tool(ctx.host, &spec);
    }
}

/* ==================== 钩子业务签名分发 (同步/异步共用) ==================== */

namespace detail {

/// 按可调用性选择钩子业务签名：fn(ctx, point, input) / fn(ctx, input) / fn(input)。
/// 返回类型原样转发：同步钩子通常是 void，异步钩子返回 Task<T>（F19 用返回类型
/// 严格区分同步与异步，签名不匹配时在 if constexpr 分支给出明确错误）。
template<typename HookFn, typename Ctx>
inline decltype(auto) invokeHook(HookFn& fn, Ctx& ctx, int32_t pt, std::string_view input) {
    if constexpr (std::is_invocable_v<HookFn, Ctx&, AgentxxPluginHookPoint, std::string_view>) {
        return fn(ctx, static_cast<AgentxxPluginHookPoint>(pt), input);
    } else if constexpr (std::is_invocable_v<HookFn, Ctx&, std::string_view>) {
        return fn(ctx, input);
    } else {
        return fn(input);
    }
}

} // namespace detail

template<typename Ctx, typename HookFn>
inline void hook(Ctx& ctx, AgentxxPluginHookPoint point, HookFn&& fn) {
    struct HookShim {
        Ctx*                 ctx = nullptr;
        std::decay_t<HookFn> fn;
    };

    /// 异步钩子的 provider 句柄：拥有输入 Request，并由 promise.opCleanup_ 回收。
    struct HookJob {
        HookShim*                          shim = nullptr;
        std::shared_ptr<std::atomic<bool>> cancelFlag;
        void*                              coroAddr = nullptr;
        detail::RootRequest                request;
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
        if (!shim || !shim->ctx) {
            detail::CompletionGuard guard(notify);
            guard.failed("hook context released");
            return nullptr;
        }

        /// 输入纳入拥有型 Request：同步钩子在调用期间有效，异步 Task 由 HookJob
        /// 持有到协程真正结束（F13）。
        auto request = detail::RootRequest::forHook(shim->ctx->host, node_input_json);

        using HookRet = decltype(
            detail::invokeHook(shim->fn, *shim->ctx, pt, std::string_view{})
        );
        if constexpr (std::is_void_v<HookRet>) {
            /// 同步 void 钩子：调用返回即完成；异常统一映射为终态。
            detail::CompletionGuard guard(notify);
            try {
                detail::invokeHook(shim->fn, *shim->ctx, pt, request.args());
                guard.ok();
            } catch (...) {
                guard.fromCurrentException();
            }
            return nullptr;
        } else {
            /// Task<T> 钩子（通常 Task<void>）：由 promise 在协程结束后收束完成
            /// 通知，返回 Job 作为宿主可取消的 provider 句柄（F19）。
            auto* job = new HookJob{
                shim,
                std::make_shared<std::atomic<bool>>(false),
                nullptr,
                std::move(request)
            };
            auto task = detail::invokeHook(shim->fn, *shim->ctx, pt, job->request.args());
            if (!task.handle_) {
                delete job;
                detail::CompletionGuard guard(notify);
                guard.failed("hook returned an empty task");
                return nullptr;
            }
            auto  h      = task.handle_;
            task.handle_ = nullptr;
            auto& p      = h.promise();
            p.notify_     = notify ? *notify : AgentxxPluginOperatorNotify{nullptr, nullptr};
            p.host_       = job->request.host;
            p.cancelFlag_ = job->cancelFlag;
            try {
                h.resume();
            } catch (...) {
                p.set_exception(std::current_exception());
            }
            if (h.done()) {
                detail::finishIfDone(h);
                delete job;
                return nullptr;
            }
            job->coroAddr = h.address();
            p.opCleanup_  = [job]() { delete job; };
            return job;
        }
    };

    spec.hook_cancel = [](void* user_data, void* op) {
        (void)user_data;
        if (!op) {
            return;
        }
        auto* job = static_cast<HookJob*>(op);
        if (job->cancelFlag) {
            job->cancelFlag->store(true, std::memory_order_release);
        }
        if (job->coroAddr) {
            auto handle
                = std::coroutine_handle<detail::PromiseBase<void>>::from_address(job->coroAddr);
            handle.promise().cancel_outstanding();
        }
    };

    if (ctx.iface.hooks && ctx.iface.hooks->register_hook) {
        ctx.iface.hooks->register_hook(ctx.host, &spec);
    }
}

namespace detail {

/// 按可调用性选择能力业务签名：fn(ctx, caller, method, args) /
/// fn(ctx, method, args) / fn(method, args)。
/// 返回类型原样转发：同步能力返回字符串，异步能力返回 `Task<T>`
/// （与 [invokeHook] 相同的严格分发策略）。
template<typename CapFn, typename Ctx>
inline decltype(auto) invokeCap(
    CapFn&                    fn,
    Ctx&                      ctx,
    const AgentxxPluginHost*  caller,
    std::string_view          method,
    std::string_view          args
) {
    if constexpr (std::is_invocable_v<
                      CapFn,
                      Ctx&,
                      const AgentxxPluginHost*,
                      std::string_view,
                      std::string_view>) {
        return fn(ctx, caller, method, args);
    } else if constexpr (std::is_invocable_v<CapFn, Ctx&, std::string_view, std::string_view>) {
        return fn(ctx, method, args);
    } else {
        return fn(method, args);
    }
}

} // namespace detail

template<typename Ctx, typename CapFn>
inline void capability(Ctx& ctx, std::string_view capName, CapFn&& fn) {
    struct CapShim {
        Ctx*                ctx = nullptr;
        std::decay_t<CapFn> fn;
    };

    /// 异步能力的 provider 句柄：拥有输入 Request，并由 promise.opCleanup_ 回收。
    struct CapJob {
        CapShim*                           shim = nullptr;
        std::shared_ptr<std::atomic<bool>> cancelFlag;
        void*                              coroAddr = nullptr;
        detail::RootRequest                request;
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
                if (!shim || !shim->ctx) {
                    detail::CompletionGuard guard(notify);
                    guard.failed("capability context released");
                    return nullptr;
                }
                // 能力入参纳入拥有型 Request：业务只读到 Request 拥有的
                // method/args（F13），不再依赖宿主借用缓冲区。
                auto request = detail::RootRequest::forCapability(
                    shim->ctx->host,
                    method,
                    args_json
                );

                using CapRet = decltype(detail::invokeCap(
                    shim->fn,
                    *shim->ctx,
                    caller_host,
                    std::string_view{},
                    std::string_view{}
                ));
                // 同步能力: void 或字符串类返回值；其余 (Task<T>) 走异步路径。
                constexpr bool kSyncCap = std::is_void_v<CapRet>
                                          || std::is_convertible_v<CapRet, std::string_view>;
                if constexpr (kSyncCap) {
                    /// 同步能力：调用返回即完成；异常统一映射为终态。
                    detail::CompletionGuard guard(notify);
                    try {
                        if constexpr (std::is_void_v<CapRet>) {
                            detail::invokeCap(
                                shim->fn,
                                *shim->ctx,
                                caller_host,
                                request.capMethod(),
                                request.args()
                            );
                            guard.ok();
                        } else {
                            guard.ok(detail::invokeCap(
                                shim->fn,
                                *shim->ctx,
                                caller_host,
                                request.capMethod(),
                                request.args()
                            ));
                        }
                    } catch (...) {
                        guard.fromCurrentException();
                    }
                    return nullptr;
                } else {
                    /// Task<T> 能力：由 promise 在协程结束后收束完成通知，
                    /// 返回 Job 作为宿主可取消的 provider 句柄。
                    auto* job = new CapJob{
                        shim,
                        std::make_shared<std::atomic<bool>>(false),
                        nullptr,
                        std::move(request)
                    };
                    auto task = detail::invokeCap(
                        shim->fn,
                        *shim->ctx,
                        caller_host,
                        job->request.capMethod(),
                        job->request.args()
                    );
                    if (!task.handle_) {
                        delete job;
                        detail::CompletionGuard guard(notify);
                        guard.failed("capability returned an empty task");
                        return nullptr;
                    }
                    auto  h      = task.handle_;
                    task.handle_ = nullptr;
                    auto& p      = h.promise();
                    p.notify_ = notify ? *notify : AgentxxPluginOperatorNotify{nullptr, nullptr};
                    p.host_   = job->request.host;
                    p.cancelFlag_ = job->cancelFlag;
                    try {
                        h.resume();
                    } catch (...) {
                        p.set_exception(std::current_exception());
                    }
                    if (h.done()) {
                        detail::finishIfDone(h);
                        delete job;
                        return nullptr;
                    }
                    job->coroAddr = h.address();
                    p.opCleanup_  = [job]() { delete job; };
                    return job;
                }
            },
            [](void* user_data, void* op) {
                (void)user_data;
                if (!op) {
                    return;
                }
                auto* job = static_cast<CapJob*>(op);
                if (job->cancelFlag) {
                    job->cancelFlag->store(true, std::memory_order_release);
                }
                if (job->coroAddr) {
                    auto handle = std::coroutine_handle<detail::PromiseBase<void>>::from_address(
                        job->coroAddr
                    );
                    handle.promise().cancel_outstanding();
                }
            },
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
    std::string         displayName;
    std::string         summary;
    agentxx::util::Json items = agentxx::util::Json::array();
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
    agentxx::util::Json j;
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

/* ==================== Client 侧通用交互: ActionController (header-only) ====================
 *
 * 插件侧 Lambda 风格的动作绑定设施 (实例内存 map<action_id, handler>):
 * - JSON 只传 action_id 字符串 (函数指针不可序列化, 见三铁律), 映射留在插件侧内存
 * - 只在 IO 线程 on/dispatch (与事件 handler 同约定), 无需锁
 * - dispatch 为 C 回调 (填入 bind_action_handler), 空指针守卫 + 参数解析失败给 {}
 *   + handler 异常吞掉记日志, 不外泄 C 边界
 */

namespace kit {

class ActionController {
public:

    using Handler = std::function<void(const agentxx::util::Json& args)>;

    /// 注册动作处理器 (IO 线程; 同 actionId 覆盖)
    void on(std::string actionId, Handler h) {
        handlers_[std::move(actionId)] = std::move(h);
    }

    /// 注销动作处理器 (不存在忽略)
    void off(const std::string& actionId) {
        handlers_.erase(actionId);
    }

    /// 生成 button JSON (action_id 自增 act_N; args 缺省 {}; role 缺省 normal)
    /// - onClick 为空时仍生成可点按钮 (固定 id 由调用方另行 on() 绑定, 如 planning 常量)
    agentxx::util::Json makeButton(
        std::string         label,
        Handler             onClick = nullptr,
        std::string         prefix  = "",
        std::string         role    = "normal",
        agentxx::util::Json args    = agentxx::util::Json::object()
    ) {
        const std::string id = "act_" + std::to_string(++counter_);
        if (onClick) {
            handlers_[id] = std::move(onClick);
        }
        agentxx::util::Json btn = agentxx::util::Json::object();
        btn["kind"]             = "button";
        btn["label"]            = std::move(label);
        if (!prefix.empty()) {
            btn["prefix"] = std::move(prefix);
        }
        btn["action_id"] = id;
        btn["args"]      = std::move(args);
        btn["role"]      = std::move(role);
        return btn;
    }

    /// C 回调 (填入 bind_action_handler 的 on_action; ud = 本实例指针)
    static void AGENTXX_PLUGIN_CALL dispatch(const AgentxxUiActionContext* ctx, void* ud) {
        auto* self = static_cast<ActionController*>(ud);
        if (!self || !ctx) {
            return;
        }
        try {
            const std::string actionId(
                ctx->action_id.data ? ctx->action_id.data : "",
                static_cast<size_t>(ctx->action_id.size)
            );
            auto it = self->handlers_.find(actionId);
            if (it == self->handlers_.end() || !it->second) {
                return;
            }
            agentxx::util::Json args = agentxx::util::Json::object();
            if (ctx->action_args.data && ctx->action_args.size > 0) {
                try {
                    auto parsed = agentxx::util::Json::parse(std::string_view{
                        ctx->action_args.data,
                        static_cast<size_t>(ctx->action_args.size)
                    });
                    if (parsed.is_object()) {
                        args = std::move(parsed);
                    }
                } catch (...) {
                    // 解析失败给 {} (约定)
                }
            }
            it->second(args);
        } catch (...) {
            // handler 异常不外泄 C 边界 (宿主另有兜底, 此处静默吞掉)
        }
    }

    bool empty() const noexcept {
        return handlers_.empty();
    }

    size_t size() const noexcept {
        return handlers_.size();
    }

private:

    std::unordered_map<std::string, Handler> handlers_;
    uint64_t                                 counter_ = 0;
};

} // namespace kit

/* ==================== 客户端插件实例上下文基类 ==================== */

class ClientPluginBase {
public:

    const AgentxxPluginHost* host = nullptr;
    ClientIfaces             iface{};
    Logger                   log;
    kit::ActionController    actions;

    ClientPluginBase() = default;

    virtual ~ClientPluginBase() {
        if (lifeToken_) {
            lifeToken_->store(false, std::memory_order_release);
        }
    }

    std::shared_ptr<std::atomic<bool>> lifeToken() const {
        return lifeToken_;
    }

    void init(const AgentxxPluginHost* h) {
        host      = h;
        iface     = ClientIfaces::query(h);
        log.host  = h;
        log.logFn = (iface.log && iface.log->log) ? iface.log->log : nullptr;
    }

    std::string clientState() const {
        if (!host || !iface.session || !iface.session->get_client_state) {
            return "{}";
        }
        AgentxxPluginString s{nullptr, 0};
        iface.session->get_client_state(host, &s);
        if (!s.data) {
            return "{}";
        }
        std::string res(s.data, static_cast<size_t>(s.size));
        PluginString::free(host, &s);
        return res;
    }

    std::string argsJson() const {
        if (!host || !iface.self || !iface.self->get_plugin_args) {
            return "{}";
        }
        AgentxxPluginString s{nullptr, 0};
        iface.self->get_plugin_args(host, &s);
        if (!s.data) {
            return "{}";
        }
        std::string res(s.data, static_cast<size_t>(s.size));
        PluginString::free(host, &s);
        return res;
    }

    std::string configPath() const {
        if (!host || !iface.self || !iface.self->get_plugin_config_path) {
            return "";
        }
        AgentxxPluginString s{nullptr, 0};
        iface.self->get_plugin_config_path(host, &s);
        if (!s.data) {
            return "";
        }
        std::string res(s.data, static_cast<size_t>(s.size));
        PluginString::free(host, &s);
        return res;
    }

    std::string language() const {
        if (!host || !iface.self || !iface.self->get_language) {
            return "en";
        }
        AgentxxPluginString s{nullptr, 0};
        if (iface.self->get_language(host, &s) == 0 && s.data) {
            std::string res(s.data, static_cast<size_t>(s.size));
            PluginString::free(host, &s);
            return res;
        }
        return "en";
    }

    bool setLanguage(std::string_view lang) const {
        if (!host || !iface.self || !iface.self->set_language) {
            return false;
        }
        auto sv = PluginStringView::from(lang.data(), lang.size());
        return iface.self->set_language(host, &sv) == 0;
    }

    void showToast(std::string_view text, int32_t level = 0) const {
        if (!host || !iface.ui || !iface.ui->show_toast) {
            return;
        }
        auto sv = PluginStringView::from(text.data(), text.size());
        iface.ui->show_toast(host, &sv, level);
    }

    int32_t sendUserInput(std::string_view sessionId, std::string_view text) const {
        if (!host || !iface.session || !iface.session->send_user_input) {
            return -1;
        }
        auto sidSv = PluginStringView::from(sessionId.data(), sessionId.size());
        auto txtSv = PluginStringView::from(text.data(), text.size());
        return iface.session->send_user_input(host, &sidSv, &txtSv);
    }

    void requestCancel(std::string_view sessionId) const {
        if (!host || !iface.session || !iface.session->request_cancel) {
            return;
        }
        auto sidSv = PluginStringView::from(sessionId.data(), sessionId.size());
        iface.session->request_cancel(host, &sidSv);
    }

    void registerTemplate(std::string_view tool, std::string_view display, std::string_view key) {
        registerToolTemplate(host, iface.ui, tool, display, key);
    }

    template<typename Fn>
    void registerRenderer(std::string_view tool, Fn&& fn) {
        registerToolRenderer(host, iface.ui, tool, std::forward<Fn>(fn), shims_);
    }

    template<typename T>
    T* storeShim(std::unique_ptr<T> shim) {
        T* raw = shim.get();
        shims_.emplace_back(shim.release(), [](void* ptr) {
            delete static_cast<T*>(ptr);
        });
        return raw;
    }

private:

    std::shared_ptr<std::atomic<bool>> lifeToken_ = std::make_shared<std::atomic<bool>>(true);
    std::vector<std::unique_ptr<void, void (*)(void*)>> shims_;
};

/* ==================== 一键式插件导出宏族 ==================== */

#define AGENTXX_PLUGIN_AGENT_EXPORT(CtxType, Name, Ver, Desc, ...)                               \
    extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void \
    ) {                                                                                          \
        static const AgentxxPluginInfo info{                                                     \
            AGENTXX_PLUGIN_API_VERSION,                                                          \
            0,                                                                                   \
            agentxx::plugin::PluginStringView::fromCstr(Name),                                   \
            agentxx::plugin::PluginStringView::fromCstr(Ver),                                    \
            agentxx::plugin::PluginStringView::fromCstr(Desc),                                   \
        };                                                                                       \
        return &info;                                                                            \
    }                                                                                            \
    extern "C" AGENTXX_PLUGIN_EXPORT int32_t                                                     \
        agentxx_plugin_agent_create(const AgentxxPluginHost* host, void** plugin_ctx) {          \
        if (!host || !plugin_ctx)                                                                \
            return -1;                                                                           \
        auto ctx = std::make_unique<CtxType>();                                                  \
        ctx->init(host);                                                                         \
        try {                                                                                    \
            auto    setup = (__VA_ARGS__);                                                       \
            int32_t rc    = setup(*ctx);                                                         \
            if (rc != 0)                                                                         \
                return rc;                                                                       \
        } catch (const std::exception& e) {                                                      \
            ctx->log.error(fmt::format("Plugin setup exception: {}", e.what()));                 \
            return -1;                                                                           \
        } catch (...) {                                                                          \
            ctx->log.error("Plugin setup unknown exception");                                    \
            return -1;                                                                           \
        }                                                                                        \
        *plugin_ctx = ctx.release();                                                             \
        return 0;                                                                                \
    }                                                                                            \
    extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {       \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                           \
        if (ctx)                                                                                 \
            delete ctx;                                                                          \
    }

/// Optional lifecycle export helpers. The setup expression is intentionally
/// separate from the legacy create macro so existing plugins keep their
/// create-time registration behavior while new plugins can opt into a
/// start/stop transaction without hand-writing ABI trampolines.
#define AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT(CtxType, StartFn, StopFn)                         \
    extern "C" AGENTXX_PLUGIN_EXPORT void* agentxx_plugin_agent_start(                       \
        void* plugin_ctx, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString* err  \
    ) {                                                                                         \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                          \
        try {                                                                                   \
            if (!ctx) {                                                                         \
                if (err) agentxx::plugin::PluginString::set(nullptr, err, "plugin start: null context"); \
                return nullptr;                                                                \
            }                                                                                   \
            return (StartFn)(*ctx, notify, err);                                                \
        } catch (const std::exception& e) {                                                     \
            if (err) agentxx::plugin::PluginString::set(ctx ? ctx->host : nullptr, err, e.what()); \
        } catch (...) {                                                                         \
            if (err) agentxx::plugin::PluginString::set(ctx ? ctx->host : nullptr, err, "plugin start threw"); \
        }                                                                                       \
        return nullptr;                                                                         \
    }                                                                                           \
    extern "C" AGENTXX_PLUGIN_EXPORT void* agentxx_plugin_agent_stop(                        \
        void* plugin_ctx, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString* err  \
    ) {                                                                                         \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                          \
        try {                                                                                   \
            if (!ctx) {                                                                         \
                if (err) agentxx::plugin::PluginString::set(nullptr, err, "plugin stop: null context"); \
                return nullptr;                                                                \
            }                                                                                   \
            return (StopFn)(*ctx, notify, err);                                                 \
        } catch (const std::exception& e) {                                                     \
            if (err) agentxx::plugin::PluginString::set(ctx ? ctx->host : nullptr, err, e.what()); \
        } catch (...) {                                                                         \
            if (err) agentxx::plugin::PluginString::set(ctx ? ctx->host : nullptr, err, "plugin stop threw"); \
        }                                                                                       \
        return nullptr;                                                                         \
    }

#define AGENTXX_PLUGIN_CLIENT_EXPORT(CtxType, Name, Ver, Desc, ...)                         \
    extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxClientPluginInfo*                         \
        agentxx_plugin_client_get_info(void) {                                              \
        static const AgentxxClientPluginInfo info{                                          \
            AGENTXX_CLIENT_PLUGIN_API_VERSION,                                              \
            0,                                                                              \
            agentxx::plugin::PluginStringView::fromCstr(Name),                              \
            agentxx::plugin::PluginStringView::fromCstr(Ver),                               \
            agentxx::plugin::PluginStringView::fromCstr(Desc),                              \
        };                                                                                  \
        return &info;                                                                       \
    }                                                                                       \
    extern "C" AGENTXX_PLUGIN_EXPORT int32_t                                                \
        agentxx_plugin_client_create(const AgentxxPluginHost* host, void** plugin_ctx) {    \
        if (!host || !plugin_ctx)                                                           \
            return -1;                                                                      \
        auto ctx = std::make_unique<CtxType>();                                             \
        ctx->init(host);                                                                    \
        try {                                                                               \
            auto    setup = (__VA_ARGS__);                                                  \
            int32_t rc    = setup(*ctx);                                                    \
            if (rc != 0)                                                                    \
                return rc;                                                                  \
        } catch (const std::exception& e) {                                                 \
            ctx->log.error(fmt::format("Client plugin setup exception: {}", e.what()));     \
            return -1;                                                                      \
        } catch (...) {                                                                     \
            ctx->log.error("Client plugin setup unknown exception");                        \
            return -1;                                                                      \
        }                                                                                   \
        *plugin_ctx = ctx.release();                                                        \
        return 0;                                                                           \
    }                                                                                       \
    extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_client_destroy(void* plugin_ctx) { \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                      \
        if (ctx)                                                                            \
            delete ctx;                                                                     \
    }

/// Client 侧的可选 start/stop 导出 (与
/// [AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT] 对称):
/// - `StartFn` / `StopFn` 形如 `void*(CtxType&, const AgentxxPluginOperatorNotify*,
///   AgentxxPluginString*)`;
/// - 只导出 client 入口的插件用它把 UI 注册事务放进 start、撤销放进 stop;
///   使用 `AGENTXX_PLUGIN_CLIENT_EXPORT` 的插件不导出这两个符号 (legacy 路径)。
#define AGENTXX_PLUGIN_CLIENT_LIFECYCLE_EXPORT(CtxType, StartFn, StopFn)                   \
    extern "C" AGENTXX_PLUGIN_EXPORT void* agentxx_plugin_client_start(                    \
        void* plugin_ctx, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString* err \
    ) {                                                                                     \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                      \
        try {                                                                               \
            if (!ctx) {                                                                     \
                if (err)                                                                    \
                    agentxx::plugin::PluginString::set(                                     \
                        nullptr, err, "client plugin start: null context"                   \
                    );                                                                      \
                return nullptr;                                                             \
            }                                                                               \
            return (StartFn)(*ctx, notify, err);                                            \
        } catch (const std::exception& e) {                                                 \
            if (err)                                                                        \
                agentxx::plugin::PluginString::set(                                         \
                    ctx ? ctx->host : nullptr, err, e.what()                                \
                );                                                                          \
        } catch (...) {                                                                     \
            if (err)                                                                        \
                agentxx::plugin::PluginString::set(                                         \
                    ctx ? ctx->host : nullptr, err, "client plugin start threw"             \
                );                                                                          \
        }                                                                                   \
        return nullptr;                                                                     \
    }                                                                                       \
    extern "C" AGENTXX_PLUGIN_EXPORT void* agentxx_plugin_client_stop(                     \
        void* plugin_ctx, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString* err \
    ) {                                                                                     \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                      \
        try {                                                                               \
            if (!ctx) {                                                                     \
                if (err)                                                                    \
                    agentxx::plugin::PluginString::set(                                     \
                        nullptr, err, "client plugin stop: null context"                    \
                    );                                                                      \
                return nullptr;                                                             \
            }                                                                               \
            return (StopFn)(*ctx, notify, err);                                             \
        } catch (const std::exception& e) {                                                 \
            if (err)                                                                        \
                agentxx::plugin::PluginString::set(                                         \
                    ctx ? ctx->host : nullptr, err, e.what()                                \
                );                                                                          \
        } catch (...) {                                                                     \
            if (err)                                                                        \
                agentxx::plugin::PluginString::set(                                         \
                    ctx ? ctx->host : nullptr, err, "client plugin stop threw"              \
                );                                                                          \
        }                                                                                   \
        return nullptr;                                                                     \
    }

} // namespace plugin
} // namespace agentxx
