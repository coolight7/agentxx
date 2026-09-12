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
#include "asio/awaitable.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/executor_work_guard.hpp"
#include "asio/io_context.hpp"
#include "asio/post.hpp"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include <type_traits>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdio>
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
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

/// asio 命名空间别名 (header-only SDK 不引入 util 的 asio_error.h, 避免插件多带依赖)
namespace asio = ::boost::asio;

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
 * - queryInterface<Iface>: 接口表查询模板
 *
 * 说明: 这些能力放在 C++ 头而非纯 C ABI 头 plugin_api.h, 因为按值返回含
 * C++ 成员函数的 struct 会触发 MSVC C4190。
 */

/// 字符串视图便捷工具 (纯静态函数; 不构造对象)
struct PluginStringView {
    /// 从 (指针, 长度) 构造 ABI 视图
    static AgentxxPluginStringView from(const char* s, uint64_t n) noexcept {
        return AgentxxPluginStringView{s, n};
    }

    /// 从 NUL 结尾 C 串构造 ABI 视图 (自动 strlen)
    static AgentxxPluginStringView fromCstr(const char* s) noexcept {
        return AgentxxPluginStringView{s, s ? static_cast<uint64_t>(std::strlen(s)) : 0};
    }

    /// 从 std::string_view 构造 ABI 视图
    static AgentxxPluginStringView from(std::string_view s) noexcept {
        return AgentxxPluginStringView{s.data(), static_cast<uint64_t>(s.size())};
    }

    /// ABI 视图是否为空
    static bool empty(const AgentxxPluginStringView& sv) noexcept {
        return sv.data == nullptr || sv.size == 0;
    }

    /// 指针重载 (NULL 视为空)
    static bool empty(const AgentxxPluginStringView* sv) noexcept {
        return sv == nullptr || sv->data == nullptr || sv->size == 0;
    }

    /// 宿主堆字符串是否为空
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

    /// 宿主堆字符串 → ABI 视图
    static AgentxxPluginStringView toSv(const AgentxxPluginString& s) noexcept {
        return AgentxxPluginStringView{s.data, s.size};
    }

    /// 指针重载 (NULL 视为空视图)
    static AgentxxPluginStringView toSv(const AgentxxPluginString* s) noexcept {
        return s ? AgentxxPluginStringView{s->data, s->size} : AgentxxPluginStringView{};
    }
};

/// 宿主堆字符串 RAII (析构自动释放; move-only)
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

    /// 经宿主 alloc 拷贝视图 → ABI 宿主串;
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

    /// 覆盖写入 ABI 字符串出参: 先释放已有宿主分配, 再经宿主 alloc 写入新内容
    static void
        set(const AgentxxPluginHost* h, AgentxxPluginString* out, std::string_view sv) noexcept {
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

    /// 经宿主 alloc 拷贝 C 串 → ABI 宿主串
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

    /// 经宿主 alloc 拷贝视图为裸 char* (调用方负责 free)
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

    /// C 串重载
    static char* strdup(const AgentxxPluginHost* h, const char* s) {
        if (!h || !s) {
            return nullptr;
        }
        return strdup(h, PluginStringView::fromCstr(s));
    }

    /// 释放宿主堆串 (幂等并清空)
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

/// 查询宿主接口表并转型 (IID → 接口表)
template<typename Iface>
const Iface* validateInterface(const void* raw) noexcept {
    if (!raw) {
        return nullptr;
    }
    const auto* iface = static_cast<const Iface*>(raw);
    // 接口表契约: 用准确的 version 与完整的字节大小标识自己;
    // 版本不匹配或表长度不足的表一律视为不可用。
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

/* ==================== 接口表聚合 ==================== */

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
    /// "agentxx.agent.coroutine_runtime": 协程驱动 (host driver/wake 协议)。
    const AgentxxPluginCoroutineRuntimeIface* coroutineRuntime = nullptr;

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
        f.coroutineRuntime = queryInterface<AgentxxPluginCoroutineRuntimeIface>(
            host,
            AGENTXX_PLUGIN_IFACE_COROUTINE_RUNTIME
        );
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

/* ==================== 协程驱动桥 (PollOneBridge) ====================
 *
 * 定位: 让插件协程与宿主协程在**同一宿主 IO 执行序列**中交错推进的适配层。
 *
 * 协议 (与协程库无关):
 * - 插件侧适配器把本地协程的推进单位记为"一次有限步骤" (poll_one 一个就绪 handler);
 * - 只要本地有**已知**可运行工作 (新 root 首步 / 宿主回调完成投递的 continuation /
 *   本地 post), 就调用 [wake]; wake 合并重复请求后向宿主申请一次驱动请求;
 * - 宿主把请求异步投递到自己的 IO 线程, 回调里只做一次 `poll_one()`; 完成后若又
 *   有新 wake 才申请下一次请求; 没有新工作时**不再申请**, 因此空闲时不消耗宿主
 *   任务队列, 也不自旋;
 * - **绝不在宿主回调栈内直接恢复插件协程**: 宿主回调只复制结果并
 *   `postToLocal + wake`, continuation 仍由下一次 driver 推进。
 *
 * 状态机 (三个竞态窗口都必须覆盖: driver 前 / driver 执行中 / driver 返回后):
 * - `readySteps_`: 已投递但尚未执行的本地步骤数 (每一步需要一次 poll_one);
 * - `wakePending_`: 显式 wake 尚未被请求覆盖 (覆盖"插件直接向 local_executor 投递,
 *   kit 看不到该投递"的用法; 每次 wake 至多多花一次请求, 不会自旋);
 * - `driverQueued_`: 已申请请求 (含 request_driver 正在返回的窗口);
 * - `driverRunning_`: 回调正在执行;
 * 单个 `bool scheduled` 会在"回调收尾清标志"与"外部投递写标志"之间丢通知: 例如
 * 并发启动 N 个根时, N 次 wake 若被合并成一次请求, 就只有 1 个根会被推进。因此
 * 这里用"步骤计数 + 显式 wake 标记 + 两个在途标志 + epoch"组合, 保证任何窗口中的
 * 投递最终都会被推进, 且没有新工作时请求数不再增长。
 *
 * 线程: `wake()` 可从外部完成回调线程调用; `driveOnce()` 在宿主 IO 线程执行;
 * `stop()` 从 stop/destroy 路径调用。三者用一把**短**临界区互斥线性化 —— 临界区内
 * 不调用插件业务代码、不投递、不等待, 因此既不会与宿主形成死锁, 也不影响
 * "插件状态只在宿主 IO 线程访问"这一无锁前提 (真正的插件代码只在 driveOnce 的
 * `poll_one` 里跑)。
 */
namespace detail {

/// 单实例协程驱动桥 (定义在下方; 这里前置声明供 [BridgeRoot] 引用)。
class PollOneBridge;

/// 根协程帧的销毁函数 (类型擦除到具体 promise)。
template<typename Promise>
inline void destroyBridgeFrame(void* frame) noexcept {
    if (!frame) {
        return;
    }
    auto handle = std::coroutine_handle<Promise>::from_address(frame);
    if (handle) {
        handle.destroy();
    }
}

/// 根操作在桥接中的仲裁对象: 完成/放弃的 exactly-once 与协程帧的销毁责任。
///
/// 为什么不直接用 promise 里的 notify:
/// - 根协程的推进由 host driver 触发, 因此"根仍在本地排队"与"宿主拒绝提供驱动"
///   可能并发出现; 本对象用原子 CAS 保证 `notify.done` 至多一次, 并保证 op 句柄
///   等资源只释放一次;
/// - 宿主拒绝再提供驱动 (实例关闭 / 无 IO executor) 时, 根必须被终结为失败, 否则
///   宿主持有的 Operation 永远不完成、卸载等待必然超时;
/// - 协程帧的销毁责任跟着本对象的生命周期: 排队中的本地任务 (或桥析构释放队列)
///   持有它的强引用, 最后一个引用释放时销毁仍挂起的帧。放弃路径只置标志, 因此
///   **不会有跨线程销毁一个仍可能被恢复的协程帧**。
class BridgeRoot : public std::enable_shared_from_this<BridgeRoot> {
public:

    using DestroyFrameFn = void (*)(void* frame) noexcept;

    BridgeRoot(
        const AgentxxPluginOperatorNotify& notify,
        void*                              frame,
        DestroyFrameFn                     destroyFrame
    ) :
        notify_(notify),
        frame_(frame),
        destroyFrame_(destroyFrame) {}

    /// 绑定所属桥 (由 [startBridgedRoot] 设置): 放弃的根需要由桥保活协程帧。
    void setOwner(PollOneBridge* owner) noexcept {
        owner_ = owner;
    }

    BridgeRoot(const BridgeRoot&)            = delete;
    BridgeRoot& operator=(const BridgeRoot&) = delete;

    ~BridgeRoot() {
        // 最后一个引用 (排队中的本地任务 / 桥登记 / 桥析构) 释放: 销毁仍挂起的帧。
        // 已正常完成的帧在 [finishIfDone] 中已销毁 (frame_ 置空), 这里是幂等空操作。
        destroyFrame();
    }

    /// 认领"根已终结"的所有权 (CAS); 只有第一个调用者能走完成/放弃流程。
    bool claimFinish() noexcept {
        bool expected = false;
        return claimed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }

    /// 向宿主上报终态 (仅在 [claimFinish] 成功后调用, 保证 exactly-once)。
    void notifyHost(int32_t status, std::string_view payload) noexcept {
        if (!notify_.done) {
            return;
        }
        auto sv = PluginStringView::from(payload.data(), payload.size());
        try {
            notify_.done(notify_.host_ud, status, &sv);
        } catch (...) {
            // 宿主回调不得抛异常; 即便抛了也已经认领过完成权, 不重复派发。
        }
    }

    /// 宿主不再提供驱动: 终结为失败 (幂等)。
    ///
    /// 帧的处置: 不能在这里销毁 —— 该根可能有仍在外部的宿主回调 (例如已排队或正在
    /// 执行的 sleep/offload 完成), 它们随后会尝试恢复本帧; 因此把根交给桥托管
    /// (`owner_`), 由桥在**销毁时** (此时插件上下文已无任何未完成宿主操作) 统一销毁,
    /// 期间迟到的回调会因 `shouldAdvance()==false` 而安全跳过。
    /// (定义在 [PollOneBridge] 之后: 需要完整的桥类型来登记托管。)
    void abandon(std::string_view reason) noexcept;

    /// 释放 op 侧资源 (幂等)。
    void runCleanup() noexcept {
        if (cleanup_) {
            auto cleanup = std::move(cleanup_);
            cleanup_     = nullptr;
            try {
                cleanup();
            } catch (...) {
            }
        }
    }

    /// 本地任务实际推进前检查: 被放弃的根不再推进 (帧由引用释放路径销毁)。
    bool shouldAdvance() const noexcept {
        return !abandoned_.load(std::memory_order_acquire) && hasFrame();
    }

    bool hasFrame() const noexcept {
        return frame_.load(std::memory_order_acquire) != nullptr;
    }

    bool finished() const noexcept {
        return claimed_.load(std::memory_order_acquire);
    }

    bool abandoned() const noexcept {
        return abandoned_.load(std::memory_order_acquire);
    }

    /// 完成权被认领时释放 op 侧资源 (op 句柄对象等)。
    void setCleanup(std::function<void()> cleanup) {
        cleanup_ = std::move(cleanup);
    }

    /// 销毁仍然挂起的协程帧 (幂等) 并释放 op 侧资源。
    ///
    /// 调用点都满足"协程已终止或不可能再被恢复":
    /// - [finishIfDone] 的桥接分支 (协程刚结束, 挂在 final_suspend);
    /// - 桥销毁 / 最后一个引用释放 (此后不存在宿主回调访问该帧)。
    /// op 资源在这里释放, 而不是在 [abandon]: 被放弃的根可能正在 host driver 内
    /// 执行, 其输入 (Request/OpCtl) 仍被协程以引用使用。
    void destroyFrame() noexcept {
        void* frame = frame_.exchange(nullptr, std::memory_order_acq_rel);
        if (frame && destroyFrame_) {
            destroyFrame_(frame);
        }
        runCleanup();
    }

private:

    AgentxxPluginOperatorNotify notify_{nullptr, nullptr};
    std::atomic<void*>          frame_{nullptr};
    DestroyFrameFn              destroyFrame_ = nullptr;
    PollOneBridge*              owner_        = nullptr;
    std::function<void()>       cleanup_;
    std::atomic<bool>           claimed_{false};
    std::atomic<bool>           abandoned_{false};
};

/// 受控轮询根 (asio 协程) 的仲裁对象。
///
/// 与 [BridgeRoot] 的区别: `asio::awaitable` 的协程帧由 asio 自己的 completion
/// handler 持有 (本地 reactor 销毁时统一释放), 因此本对象**不负责销毁帧**, 只负责:
/// - 终态上报与资源释放的 **exactly-once** 仲裁 (正常完成 vs 桥停止时放弃);
/// - 被放弃时执行一次类型擦除的清理 (回收 Job 等宿主可见资源)。
///
/// (完成/放弃两条路径都可能释放同一个 Job, 因此用一次 CAS 决定谁来做。)
class PolledRoot : public std::enable_shared_from_this<PolledRoot> {
public:

    explicit PolledRoot(const AgentxxPluginOperatorNotify& notify) :
        notify_(notify) {}

    PolledRoot(const PolledRoot&)            = delete;
    PolledRoot& operator=(const PolledRoot&) = delete;

    ~PolledRoot() {
        runCleanup();
    }

    /// 认领"根已终结"的所有权 (CAS); 只有第一个调用者能走完成/放弃流程。
    bool claimFinish() noexcept {
        bool expected = false;
        return claimed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }

    bool finished() const noexcept {
        return claimed_.load(std::memory_order_acquire);
    }

    /// 向宿主上报终态 (仅在 [claimFinish] 成功后调用, 保证 exactly-once)。
    void notifyHost(int32_t status, std::string_view payload) noexcept {
        if (!notify_.done) {
            return;
        }
        auto sv = PluginStringView::from(payload.data(), payload.size());
        try {
            notify_.done(notify_.host_ud, status, &sv);
        } catch (...) {
            // 宿主回调不得抛异常; 即便抛了也已经认领过完成权, 不重复派发。
        }
    }

    /// 设置资源清理回调 (回收 Job); 由 [claimFinish] 的赢家执行恰好一次。
    void setCleanup(std::function<void()> cleanup) {
        cleanup_ = std::move(cleanup);
    }

    /// 执行清理 (幂等)。先把回调移出再调用, 避免回调内部释放本对象导致
    /// "在成员函数内自销毁"。
    void runCleanup() noexcept {
        if (!cleanup_) {
            return;
        }
        auto cleanup = std::move(cleanup_);
        cleanup_     = nullptr;
        try {
            cleanup();
        } catch (...) {
        }
    }

private:

    AgentxxPluginOperatorNotify notify_{nullptr, nullptr};
    std::function<void()>       cleanup_;
    std::atomic<bool>           claimed_{false};
};

/// 单实例协程驱动桥 (每个插件实例独立一份; 无任何进程级可变状态)。
///
/// kit 内部用法:
/// - 根操作启动: `postToLocal(首步)` + `wake()`, 并用 [addRoot] 登记 [BridgeRoot],
///   以便宿主拒绝驱动时终结它;
/// - 宿主回调完成: `postToLocal(continuation)` + `wake()`;
/// - stop/destroy: [stop] (拒绝新 wake、取消排队请求、停止本地 reactor、终结活跃根)。
///
/// 两类根:
/// - **事件驱动根** ([BridgeRoot]): 等的都是"宿主可见唤醒源" (宿主回调/宿主计时器),
///   空闲时不自旋;
/// - **受控轮询根** ([PolledRoot], 声明式 `polled_tool`): 等的是插件本地 reactor 上
///   的内核就绪事件 (socket/管道/文件/本地 timer)。`poll_one` 只能"执行已就绪
///   handler", 不会让等待对象到期, 因此这类根需要"有在途操作时轮询":
///   有进展立即续票、无进展退避 `kPollIntervalMs`, 空闲 (无在途 polled 操作) 时
///   不申请请求、不建定时器, 与事件驱动路径共享同一次请求/同一次 `poll_one`。
class PollOneBridge {
public:

    using LocalExecutor = asio::io_context::executor_type;

    /// 连续多少次 driver 无进展后提示"等待没有宿主可见的唤醒源"。
    /// 取 8 是为了容忍合理的偶发空转 (同一轮 wake 与已被消费的 continuation 重叠),
    /// 又不至于让真正的私有 reactor 等待被静默掩盖。
    static constexpr int kNoProgressWarnStreak = 8;

    /// ---- 受控轮询 (pump) 参数: 只作用于声明式 `polled_tool` 的根 ----
    /// 无进展时的退避量子 (ms): 等待内核就绪期间约 100 次/秒驱动,
    /// 每次 ≈1 个宿主 post + 1 次 `epoll_wait(0)`, 空闲时不产生任何驱动。
    static constexpr int64_t kPollIntervalMs = 10;
    /// 连续"有进展"多少步后强制让出一次 (给宿主其它任务与同实例其它操作机会),
    /// 避免插件自循环 (自己给自己 post) 长期独占 IO 线程。
    static constexpr int kPollBurstMax = 256;
    /// 突发上限触发后的让出时长 (ms)。
    static constexpr int64_t kPollBurstYieldMs = 1;

    PollOneBridge(
        const AgentxxPluginHost*                  host,
        const AgentxxPluginCoroutineRuntimeIface* runtime,
        const AgentxxPluginSchedulerIface*        scheduler = nullptr
    ) :
        localIo_(1),
        work_(asio::make_work_guard(localIo_)),
        host_(host),
        runtime_(runtime),
        sched_(scheduler) {}

    PollOneBridge(const PollOneBridge&)            = delete;
    PollOneBridge& operator=(const PollOneBridge&) = delete;

    ~PollOneBridge() {
        stop();
        // 桥销毁 = 插件上下文销毁的最后边界: 此时不会再有宿主回调访问插件帧
        // (实例 lease 已归零), 可以安全销毁被放弃仍挂起的根。
        std::vector<std::shared_ptr<BridgeRoot>> abandoned;
        {
            std::lock_guard lock(rootsMutex_);
            abandoned = std::move(abandonedRoots_);
            abandonedRoots_.clear();
        }
        abandoned.clear();
        // 被放弃的受控轮询根: 协程帧由 asio 随本地 reactor 释放 (见 [stop]),
        // 这里回收它们的 Job (清理回调幂等, 正常完成时已执行过)。
        std::vector<std::function<void()>> cleanups;
        {
            std::lock_guard lock(rootsMutex_);
            cleanups = std::move(abandonedCleanups_);
            abandonedCleanups_.clear();
        }
        cleanups.clear();
    }

    /// 本地执行器: 插件协程首步/continuation 的排队目标 (由 host driver 推进)。
    LocalExecutor local_executor() noexcept {
        return localIo_.get_executor();
    }

    const AgentxxPluginHost* host() const noexcept {
        return host_;
    }

    const AgentxxPluginCoroutineRuntimeIface* runtime() const noexcept {
        return runtime_;
    }

    /// 是否仍有已知可运行工作 (显式 wake 未被请求覆盖, 或有已投递未执行的步骤)。
    bool hasPendingWake() const noexcept {
        std::lock_guard lock(mutex_);
        return wakePending_ || readySteps_.load(std::memory_order_acquire) > 0;
    }

    /// 已投递但尚未执行的本地步骤数 (诊断)。
    uint64_t readySteps() const noexcept {
        return readySteps_.load(std::memory_order_acquire);
    }

    bool isDriverQueued() const noexcept {
        std::lock_guard lock(mutex_);
        return driverQueued_;
    }

    bool isDriverRunning() const noexcept {
        std::lock_guard lock(mutex_);
        return driverRunning_;
    }

    bool isStopping() const noexcept {
        std::lock_guard lock(mutex_);
        return stopping_;
    }

    /// 已申请的请求总数 (诊断: 用于验证"空闲时请求数不再增长")。
    uint64_t ticketsIssued() const noexcept {
        return ticketsIssued_.load(std::memory_order_acquire);
    }

    /// 已完成的驱动步数 (诊断)。
    uint64_t driverSteps() const noexcept {
        return driverSteps_.load(std::memory_order_acquire);
    }

    /// 活跃 (未终结) 根数量 (诊断)。
    size_t activeRootCount() const {
        std::lock_guard lock(rootsMutex_);
        return roots_.size();
    }

    /// 在途受控轮询操作数 (诊断: 0 表示"不轮询、不建定时器")。
    uint64_t polledRootCount() const noexcept {
        return polledRoots_.load(std::memory_order_acquire);
    }

    /// 已发生的"无进展退避"次数 (诊断: 验证退避确实发生且空闲时不增长)。
    uint64_t idlePollCount() const noexcept {
        return idlePollCount_.load(std::memory_order_acquire);
    }

    /// 是否有在途的退避定时器 (诊断)。
    bool isPumpWaitScheduled() const {
        std::lock_guard lock(mutex_);
        return pumpWaitScheduled_;
    }

    /// 是否正在受控轮询 (有在途 polled 操作)。
    bool isPumping() const noexcept {
        return polledRoots_.load(std::memory_order_acquire) > 0;
    }

    /// 当前线程是否宿主 IO 线程 (仅诊断; 不得据此内联执行 driver 回调)。
    bool onHostIoThread() const noexcept {
        if (!host_ || !runtime_ || !runtime_->is_io_thread) {
            return false;
        }
        return runtime_->is_io_thread(host_) != 0;
    }

    /// 把一段插件侧代码投递到本地执行器, 由后续 host driver 推进。
    /// - 恒异步 (即使调用者就在本地执行上下文内): 这是"一轮只推进一个 continuation"
    ///   的实现基础;
    /// - 不自动 wake: 调用方投递后必须调用 [wake] (语义上"投递 = 有工作")。
    void postToLocal(std::function<void()> fn) {
        if (!fn) {
            return;
        }
        readySteps_.fetch_add(1, std::memory_order_acq_rel);
        auto* self = this;
        asio::post(localIo_, [self, fn = std::move(fn)]() mutable {
            // 本 handler 已被 host driver 选中执行: 消费投递时记下的那一步。
            self->readySteps_.fetch_sub(1, std::memory_order_acq_rel);
            try {
                fn();
            } catch (const std::exception& e) {
                logFallback(fmt::format("plugin local task threw: {}", e.what()));
            } catch (...) {
                logFallback("plugin local task threw unknown exception");
            }
        });
    }

    /// 登记一个根 (**桥持有强引用**; 宿主拒绝驱动时统一终结)。
    ///
    /// 为什么必须持有强引用: 挂起中的根除了"下一次恢复任务"之外没有任何持有者 ——
    /// 排队任务一旦执行完就会释放它, 若桥不持有, 帧会被立刻销毁, 之后到达的宿主
    /// 回调就会访问已释放的协程帧。根对象因此由桥保活到 [removeRoot] (正常完成)
    /// 或桥销毁 (放弃路径)。
    void addRoot(const std::shared_ptr<BridgeRoot>& root) {
        if (!root) {
            return;
        }
        std::lock_guard lock(rootsMutex_);
        roots_.push_back(root);
    }

    /// 注销一个根 (正常完成时调用; 释放桥对它的强引用)。
    void removeRoot(const std::shared_ptr<BridgeRoot>& root) {
        std::lock_guard lock(rootsMutex_);
        std::erase_if(roots_, [&root](const std::shared_ptr<BridgeRoot>& alive) {
            return !alive || alive == root;
        });
    }

    /// 登记一个受控轮询根 (声明式 `polled_tool` 的根)。
    ///
    /// 必须在把该协程排进本地执行器之前调用: 登记之后 `polledRoots_ > 0`,
    /// 桥才知道"等待期间需要按受控轮询策略继续申请请求"。
    void addPolledRoot(const std::shared_ptr<PolledRoot>& root) {
        if (!root) {
            return;
        }
        std::lock_guard lock(rootsMutex_);
        polledRootList_.push_back(root);
        polledRoots_.store(polledRootList_.size(), std::memory_order_release);
    }

    /// 注销一个受控轮询根 (协程结束后调用)。
    /// 计数归零时停止轮询: 取消在途退避定时器, 之后不再申请请求。
    void removePolledRoot(const std::shared_ptr<PolledRoot>& root) noexcept {
        bool                         last   = false;
        AgentxxPluginOperatorHandle* waitOp = nullptr;
        {
            std::lock_guard lock(rootsMutex_);
            const auto      it = std::find(polledRootList_.begin(), polledRootList_.end(), root);
            if (it == polledRootList_.end()) {
                // 已被 [failAllPolledRoots] 清理 (停桥路径): 只保留终态仲裁语义,
                // 不再触碰 pump 状态 (Job 的回收由该路径负责)。
                return;
            }
            polledRootList_.erase(it);
            polledRoots_.store(polledRootList_.size(), std::memory_order_release);
            last = polledRootList_.empty();
            if (last) {
                std::lock_guard pumpLock(mutex_);
                waitOp = takePumpWaitLocked();
            }
        }
        if (last) {
            cancelSchedulerOp(waitOp);
        }
    }

    /// 立即结束在途退避 (取消退避定时器, 让下一次驱动马上到来)。
    /// 用于 `execute_cancel`: 插件不必等满一个退避量子就能看到取消并收束根。
    void kickPumpWait() noexcept {
        AgentxxPluginOperatorHandle* waitOp = nullptr;
        {
            std::lock_guard lock(mutex_);
            waitOp = takePumpWaitLocked();
        }
        cancelSchedulerOp(waitOp);
    }

    /// 宿主拒绝再提供驱动 / 桥停止时, 终结全部在途受控轮询根 (幂等)。
    ///
    /// 帧不在这里销毁 (由 asio 随本地 reactor 释放); 这里只负责:
    /// - 每个根按 FAILED 上报一次 (`claimFinish` 仲裁, 与正常完成路径互斥);
    /// - 执行类型擦除清理 (回收 Job), 避免放弃路径泄漏插件侧资源。
    void failAllPolledRoots(std::string_view reason) noexcept {
        std::vector<std::shared_ptr<PolledRoot>> roots;
        {
            std::lock_guard lock(rootsMutex_);
            roots = std::move(polledRootList_);
            polledRootList_.clear();
            polledRoots_.store(0, std::memory_order_release);
        }
        for (const auto& root : roots) {
            if (!root || !root->claimFinish()) {
                continue;
            }
            root->notifyHost(AGENTXX_PLUGIN_OPERATOR_FAILED, reason);
            // `roots` 在循环期间持有强引用: 清理回调释放 Job 时不会被自销毁打断。
            root->runCleanup();
        }
    }

    /// 本地已有可运行工作 (新 root 首步 / 宿主回调 continuation / 本地 post 之后)。
    ///
    /// 幂等且可合并: 同一实例同时最多登记一次请求; 没有新工作时**不会**继续申请,
    /// 因此空闲时不消耗宿主任务队列, 也不自旋。
    void wake() noexcept {
        bool needRequest = false;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
            wakePending_ = true;
            needRequest  = prepareScheduleLocked();
        }
        if (needRequest) {
            requestDriverNow();
        }
    }

    /// 停止接入 (stop/destroy 路径):
    /// - 拒绝新的 wake 与请求申请;
    /// - 取消**尚未开始**的请求 (正在执行的回调不打断, 由插件自己的 root 收束协议收尾);
    /// - 停止本地 reactor 并释放 work guard: 之后即便有迟到请求也不会再执行插件代码;
    /// - 活跃根统一终结为失败 (宿主不会再有驱动来推进它们); 本地排队的 continuation
    ///   随桥析构释放, 其持有的帧由 [BridgeRoot] 的引用释放路径销毁。
    void stop() noexcept {
        AgentxxPluginDriver*         toCancel = nullptr;
        AgentxxPluginOperatorHandle* waitOp   = nullptr;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
            stopping_    = true;
            wakePending_ = false;
            pumpPending_ = false;
            if (driverQueued_) {
                // driver_ 为空表示 request_driver 正在返回窗口: 该分支由
                // [requestDriverNow] 观察到停止状态后自行取消请求。
                toCancel      = driver_;
                driver_       = nullptr;
                driverQueued_ = false;
                pendingEpoch_ = 0;
            }
            waitOp = takePumpWaitLocked();
        }
        cancelTicket(toCancel);
        cancelSchedulerOp(waitOp);
        work_.reset();
        localIo_.stop();
        failAllRoots("plugin coroutine bridge stopped");
        failAllPolledRoots("plugin coroutine bridge stopped");
    }

    /// 托管一个被放弃的根: 帧必须活到桥销毁 (之后不会再有宿主回调访问它)。
    /// 由 [BridgeRoot::abandon] 调用 (任意线程)。
    void retainAbandonedRoot(const std::shared_ptr<BridgeRoot>& root) {
        if (!root) {
            return;
        }
        std::lock_guard lock(rootsMutex_);
        abandonedRoots_.push_back(root);
    }

    /// 宿主拒绝再提供驱动 (实例关闭/无 executor) 时, 终结全部活跃根。
    ///
    /// 顺序很关键: 先从 `roots_` 摘下 (桥不再持有), 再逐个 [BridgeRoot::abandon] ——
    /// abandon 会把根登记进 `abandonedRoots_`, 帧因此活到桥销毁为止。
    void failAllRoots(std::string_view reason) noexcept {
        std::vector<std::shared_ptr<BridgeRoot>> roots;
        {
            std::lock_guard lock(rootsMutex_);
            roots = std::move(roots_);
            roots_.clear();
        }
        for (const auto& root : roots) {
            root->abandon(reason);
        }
    }

    void logWarn(std::string_view message) const noexcept {
        logToHost(host_, 3, message);
    }

    void logError(std::string_view message) const noexcept {
        logToHost(host_, 4, message);
    }

    /// 静态上下文 (asio handler 内部) 使用的兜底日志入口。
    static void logFallback(std::string_view message) noexcept {
        std::fputs("[plugin kit] ", stderr);
        std::fwrite(message.data(), 1, message.size(), stderr);
        std::fputc('\n', stderr);
    }

    /// 经宿主日志接口表输出 (缺失时退回 stderr); 不保存跨实例状态。
    static void
        logToHost(const AgentxxPluginHost* host, int32_t level, std::string_view message) noexcept {
        if (!host || !host->vtable || !host->vtable->query_interface) {
            logFallback(message);
            return;
        }
        AgentxxPluginStringView iid = PluginStringView::fromCstr(AGENTXX_PLUGIN_IFACE_AGENT_LOG);
        const auto*             logIface
            = static_cast<const AgentxxPluginLogIface*>(host->vtable->query_interface(host, &iid));
        if (!logIface || !logIface->log) {
            logFallback(message);
            return;
        }
        try {
            AgentxxPluginStringView msg = PluginStringView::from(message.data(), message.size());
            logIface->log(host, level, &msg);
        } catch (...) {
        }
    }

private:

    /// 申请请求前的状态迁移 (调用方须持有 [mutex_])。
    /// `return`: true = 调用方应立刻调用 [requestDriverNow]
    bool prepareScheduleLocked() noexcept {
        if (stopping_ || driverQueued_ || driverRunning_) {
            return false;
        }
        // 只在"确实还有可运行工作"时申请: 已投递未执行的步骤, 或一次尚未被覆盖的
        // 显式 wake (覆盖插件直接向 local_executor 投递、kit 看不到的用法), 或
        // 受控轮询判定"该继续驱动" (pumpPending_, 见 [pumpNextLocked])。
        if (readySteps_.load(std::memory_order_acquire) == 0 && !wakePending_ && !pumpPending_) {
            return false;
        }
        wakePending_  = false;
        pumpPending_  = false;
        driverQueued_ = true;
        pendingEpoch_ = ++nextEpoch_;
        ticketsIssued_.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    /// 向宿主申请请求 (可在任意线程执行; 宿主保证异步投递回调, 且每张至多一次)。
    void requestDriverNow() noexcept {
        uint64_t epoch = 0;
        {
            std::lock_guard lock(mutex_);
            epoch = pendingEpoch_;
        }
        if (epoch == 0) {
            return;
        }
        AgentxxPluginString  error{nullptr, 0};
        AgentxxPluginDriver* ticket
            = runtime_->request_driver(host_, &PollOneBridge::driveOnceTrampoline, this, &error);
        if (!ticket) {
            std::string message = "host refused coroutine driver";
            if (error.data) {
                message.assign(error.data, static_cast<size_t>(error.size));
                PluginString::free(host_, &error);
            }
            {
                std::lock_guard lock(mutex_);
                if (pendingEpoch_ == epoch) {
                    pendingEpoch_ = 0;
                    driverQueued_ = false;
                }
            }
            logError(fmt::format("coroutine driver request failed: {}", message));
            failAllRoots(message);
            return;
        }
        bool cancelImmediately = false;
        {
            std::lock_guard lock(mutex_);
            // 该请求仍归本轮所有 (回调尚未消费它) 时才记录句柄; 否则说明回调已经
            // 跑完 (或实例已停止): 此时请求已收束, 取消是幂等空操作。
            if (pendingEpoch_ == epoch && driverQueued_ && !stopping_) {
                driver_ = ticket;
            } else {
                cancelImmediately = true;
            }
        }
        if (cancelImmediately) {
            cancelTicket(ticket);
        }
    }

    void cancelTicket(AgentxxPluginDriver* ticket) noexcept {
        if (ticket && runtime_ && runtime_->cancel_driver) {
            runtime_->cancel_driver(ticket);
        }
    }

    static void AGENTXX_PLUGIN_CALL driveOnceTrampoline(void* ud) {
        auto* self = static_cast<PollOneBridge*>(ud);
        if (!self) {
            return;
        }
        self->driveOnce();
    }

    /// host driver 回调 (宿主 IO 线程): 严格推进**一个**有限步骤。
    void driveOnce() noexcept {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                // 停止后到达的迟到请求: 不执行任何插件代码 (请求由宿主自行收束)。
                driverQueued_ = false;
                driver_       = nullptr;
                pendingEpoch_ = 0;
                return;
            }
            driverQueued_  = false;
            driver_        = nullptr; // 请求已消费, 不再需要取消
            pendingEpoch_  = 0;
            driverRunning_ = true;
        }

        if (!ranOnIoThread_.exchange(true, std::memory_order_acq_rel) && !onHostIoThread()) {
            // 首次驱动做一次线程断言 (仅诊断; 不影响语义)。
            logWarn("coroutine driver is not running on the host IO thread; plugin state must not "
                    "be shared with other threads");
        }

        std::size_t progressed = 0;
        try {
            // 严格只调用一次: 一个 driver 对应一个局部 continuation, 宿主与插件任务
            // 因此自然轮换, 单个插件无法长时间独占宿主 executor。
            progressed = localIo_.poll_one();
        } catch (const std::exception& e) {
            logError(fmt::format("plugin local runtime step threw: {}", e.what()));
        } catch (...) {
            logError("plugin local runtime step threw unknown exception");
        }

        // 受控轮询期间"本轮无进展"是常态 (等待内核就绪 / 退避), 不算异常; 因此
        // 警告只在"既没有 polled 操作在轮询、又持续空转且仍有活跃根"时出现 ——
        // 那正是"插件依赖私有 reactor 却没有声明 polled"的误用信号。
        const bool pumping = polledRoots_.load(std::memory_order_acquire) > 0;
        if (progressed == 0 && !pumping && activeRootCount() > 0) {
            if (++noProgressStreak_ >= kNoProgressWarnStreak) {
                noProgressStreak_ = 0;
                logWarn(
                    "coroutine driver made no progress while roots are active: the awaited work "
                    "has no host-visible wake source (private reactor waits are not driven by "
                    "this bridge); use host callbacks, the host timer adapter, or declare the "
                    "root as a polled tool"
                );
            }
        } else {
            noProgressStreak_ = 0;
        }
        driverSteps_.fetch_add(1, std::memory_order_acq_rel);

        bool    needRequest = false;
        bool    backoff     = false;
        int64_t backoffMs   = 0;
        {
            std::lock_guard lock(mutex_);
            driverRunning_ = false;
            // 只消费本轮之前或本轮中出现的真实 wake; 没有新 wake 就停止, 外部事件
            // 到达时会自己调用 [wake]。
            needRequest = prepareScheduleLocked();
            if (!needRequest) {
                // 受控轮询: 有在途 polled 操作时按"有进展立即续, 无进展退避"继续，
                // 否则不申请请求 (空闲零开销)。
                switch (pumpNextLocked(progressed, backoffMs)) {
                    case PumpDecision::Immediate:
                        needRequest = prepareScheduleLocked();
                        break;
                    case PumpDecision::Backoff:
                        backoff = true;
                        break;
                    case PumpDecision::Stop:
                        break;
                }
            }
        }
        if (needRequest) {
            requestDriverNow();
        } else if (backoff) {
            schedulePumpWait(backoffMs);
        }
    }

    /// 下一轮驱动的决策 (调用方持 [mutex_]; 不在此处调用宿主接口)。
    enum class PumpDecision {
        Stop,      ///< 不轮询: 无在途 polled 操作, 或已停止
        Immediate, ///< 有进展: 立即申请下一次请求 (不等退避)
        Backoff,   ///< 无进展或达到突发上限: 安排一次退避后再驱动
    };

    /// 受控轮询的调度策略 (调用方持 [mutex_])。
    ///
    /// - 无在途 polled 操作 (`polledRoots_ == 0`): 不轮询, 突发计数归零;
    /// - 本轮 `poll_one` 执行到了 handler (有进展) 且未达突发上限: 立即续票,
    ///   等待中的网络/子进程/文件就能在就绪的下一个瞬间被处理 (最坏延迟 = 退避量子);
    /// - 否则: 安排一次退避 (无进展 = `kPollIntervalMs`; 达到突发上限 = 让出
    ///   `kPollBurstYieldMs` 给宿主与同实例其它操作), 已有在途退避时不重复安排。
    PumpDecision pumpNextLocked(std::size_t progressed, int64_t& backoffMs) noexcept {
        backoffMs = 0;
        if (stopping_ || polledRoots_.load(std::memory_order_acquire) == 0) {
            pollBurst_ = 0;
            return PumpDecision::Stop;
        }
        const bool advanced = progressed > 0;
        if (advanced && pollBurst_ < kPollBurstMax) {
            ++pollBurst_;
            pumpPending_ = true;
            return PumpDecision::Immediate;
        }
        if (advanced) {
            // 突发上限: 连续快进后强制让出, 避免同实例自循环长期独占 IO 线程。
            pollBurst_ = 0;
            backoffMs  = kPollBurstYieldMs;
        } else {
            pollBurst_ = 0;
            backoffMs  = kPollIntervalMs;
        }
        if (pumpWaitScheduled_) {
            return PumpDecision::Stop; // 已有在途退避: 到点后会自己续票
        }
        pumpWaitScheduled_ = true;
        idlePollCount_.fetch_add(1, std::memory_order_acq_rel);
        return PumpDecision::Backoff;
    }

    /// 安排一次"退避后驱动" (宿主计时器适配; 只能在宿主 IO 线程调用)。
    ///
    /// - 到期回调 [pumpWaitDone] 只做"请求下一次请求", 不恢复插件协程;
    /// - 必须在 [mutex_] 之外调用宿主 `sleep` (回调可能同步到达);
    /// - 宿主计时器不可用时无法退避: 记一次错误并终结在途 polled 根,
    ///   避免"根永远不被推进"这类静默悬挂。
    void schedulePumpWait(int64_t ms) noexcept {
        if (!sched_ || !sched_->sleep) {
            {
                std::lock_guard lock(mutex_);
                pumpWaitScheduled_ = false;
            }
            logError("polled coroutine needs the host scheduler timer, but it is unavailable: "
                     "terminating the polled operations instead of hanging");
            failAllPolledRoots("host scheduler timer unavailable for polled coroutine");
            return;
        }
        AgentxxPluginString error{nullptr, 0};
        auto* op = sched_->sleep(host_, ms, &PollOneBridge::pumpWaitDone, this, &error);
        if (!op) {
            std::string message = "scheduler timer rejected";
            if (error.data) {
                message.assign(error.data, static_cast<size_t>(error.size));
                PluginString::free(host_, &error);
            }
            {
                std::lock_guard lock(mutex_);
                pumpWaitScheduled_ = false;
            }
            logError(fmt::format("polled coroutine backoff timer failed: {}", message));
            failAllPolledRoots(message);
            return;
        }
        bool keep = false;
        {
            std::lock_guard lock(mutex_);
            // 回调可能在 sleep 返回前同步到达 (立即取消): 此时它已把
            // pumpWaitScheduled_ 清掉, 句柄收束完毕, 这里只需取消该请求。
            if (pumpWaitScheduled_ && !stopping_) {
                pumpWaitOp_ = op;
                keep        = true;
            }
        }
        if (!keep) {
            cancelSchedulerOp(op);
        }
    }

    /// 退避定时器到期 (或被 `op_cancel` 取消) 的回调: 请求下一次请求。
    ///
    /// 被取消也算"退避结束" —— `execute_cancel` 正是靠取消退避来让插件立刻获得一次
    /// 驱动, 从而不必等满整个退避量子才看到取消。
    static void AGENTXX_PLUGIN_CALL
        pumpWaitDone(void* ud, int32_t status, const AgentxxPluginStringView* payload) noexcept {
        (void)status;
        (void)payload;
        auto* self = static_cast<PollOneBridge*>(ud);
        if (!self) {
            return;
        }
        bool needRequest = false;
        {
            std::lock_guard lock(self->mutex_);
            self->pumpWaitScheduled_ = false;
            self->pumpWaitOp_        = nullptr;
            if (!self->stopping_ && self->polledRoots_.load(std::memory_order_acquire) > 0) {
                self->pumpPending_ = true;
                needRequest        = self->prepareScheduleLocked();
            }
        }
        if (needRequest) {
            self->requestDriverNow();
        }
    }

    /// 取出在途退避句柄并复位调度状态 (调用方持 [mutex_]; 取消动作在锁外执行)。
    AgentxxPluginOperatorHandle* takePumpWaitLocked() noexcept {
        auto* op           = pumpWaitOp_;
        pumpWaitOp_        = nullptr;
        pumpWaitScheduled_ = false;
        return op;
    }

    /// 取消一个宿主调度句柄 (幂等; 已收束的句柄由宿主按空操作处理)。
    void cancelSchedulerOp(AgentxxPluginOperatorHandle* op) noexcept {
        if (op && sched_ && sched_->op_cancel) {
            sched_->op_cancel(op);
        }
    }

    /// 状态机锁: 保护下面前五组状态 (wake 可从任意线程调用)。
    /// `mutable` 是为了让只读诊断方法 (hasPendingWake 等) 也能加锁。
    mutable std::mutex                                         mutex_;
    asio::io_context                                           localIo_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    const AgentxxPluginHost*                                   host_    = nullptr;
    const AgentxxPluginCoroutineRuntimeIface*                  runtime_ = nullptr;
    /// 宿主计时器/卸载接口表 (`scheduler.sleep` 用于受控轮询的退避量子,
    /// `op_cancel` 用于取消在途退避); 缺失时为 nullptr。
    const AgentxxPluginSchedulerIface* sched_ = nullptr;

    bool                 stopping_      = false;
    bool                 wakePending_   = false;
    bool                 driverQueued_  = false;
    bool                 driverRunning_ = false;
    AgentxxPluginDriver* driver_        = nullptr;
    uint64_t             nextEpoch_     = 0;
    uint64_t             pendingEpoch_  = 0;

    /// 受控轮询的下一次请求理由 (见 [pumpNextLocked]/[prepareScheduleLocked])。
    bool pumpPending_ = false;
    /// 在途退避定时器 (调度状态与其句柄同属 [mutex_])。
    bool                         pumpWaitScheduled_ = false;
    AgentxxPluginOperatorHandle* pumpWaitOp_        = nullptr;
    /// 连续"有进展"步数 (受控轮询突发计数)。
    int pollBurst_ = 0;

    std::atomic<bool> ranOnIoThread_{false};
    /// 已投递但尚未执行的本地步骤数 (见 [postToLocal] 与文件头的状态机说明)。
    std::atomic<uint64_t> readySteps_{0};
    std::atomic<uint64_t> ticketsIssued_{0};
    std::atomic<uint64_t> driverSteps_{0};
    /// 在途受控轮询操作数 (与 `polledRootList_` 同步; 读端不加锁)。
    std::atomic<uint64_t> polledRoots_{0};
    /// 已发生的"无进展退避"次数 (诊断)。
    std::atomic<uint64_t> idlePollCount_{0};
    int                   noProgressStreak_ = 0;

    /// 根登记表用的锁。**锁序**: 允许 `rootsMutex_` → `mutex_` (见
    /// [removePolledRoot]), 反向嵌套不存在, 因此不会死锁。
    mutable std::mutex rootsMutex_;
    /// 活跃根 (桥持有强引用; 见 [addRoot] 说明)。
    std::vector<std::shared_ptr<BridgeRoot>> roots_;
    /// 被放弃 (宿主拒绝驱动/桥停止) 的根: 保活到桥销毁, 期间迟到回调只会安全跳过。
    std::vector<std::shared_ptr<BridgeRoot>> abandonedRoots_;
    /// 在途受控轮询根 (与 `polledRoots_` 计数一致)。
    std::vector<std::shared_ptr<PolledRoot>> polledRootList_;
    /// 被放弃的受控轮询根留下的清理回调 (回收 Job); 桥销毁时执行。
    std::vector<std::function<void()>> abandonedCleanups_;
};

inline void BridgeRoot::abandon(std::string_view reason) noexcept {
    if (!claimFinish()) {
        return;
    }
    abandoned_.store(true, std::memory_order_release);
    notifyHost(AGENTXX_PLUGIN_OPERATOR_FAILED, reason);
    // op 资源释放延后到 [destroyFrame]: 此刻该根可能正由 host driver 执行, 其输入
    // (Request/OpCtl) 仍被协程以引用使用, 现在释放就是 use-after-free。
    if (owner_) {
        // 帧交给桥保活到桥销毁 (见函数声明处的说明)。
        owner_->retainAbandonedRoot(shared_from_this());
    }
}

} // namespace detail

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
    auto root                           = std::move(p.bridgeRoot_);

    // 终态 payload: OK=结果, FAILED=错误信息, CANCELLED=空 (与既有契约一致)
    std::string_view payload;
    if (status == AGENTXX_PLUGIN_OPERATOR_FAILED) {
        payload = errPayload;
    } else if (status == AGENTXX_PLUGIN_OPERATOR_OK) {
        payload = resPayload;
    }

    if (auto bridgeRoot = root.lock()) {
        // 桥接根: 完成/放弃的仲裁与帧销毁都收敛在 [BridgeRoot] 上。
        // - 先认领完成权: 宿主可能在放弃路径 (failAllRoots) 已上报过 FAILED,
        //   此时这里不得重复上报;
        // - 再销毁帧 (幂等): 排队中的本地任务之后不会再恢复一个已销毁的帧;
        // - 最后释放 op 句柄等资源 (与 notify 一样恰好一次)。
        const bool claimed = bridgeRoot->claimFinish();
        // destroyFrame 同时释放 op 侧资源 (幂等); 协程已挂在 final_suspend, 销毁安全。
        bridgeRoot->destroyFrame();
        if (!claimed) {
            return; // 宿主已在放弃路径上报过终态, 不重复上报
        }
        bridgeRoot->notifyHost(status, payload);
        return;
    }

    h.destroy();

    if (notify.done) {
        auto sv = PluginStringView::from(payload.data(), payload.size());
        notify.done(notify.host_ud, status, &sv);
    }

    if (cleanup) {
        cleanup();
    }
}

/* ==================== 桥接 (host driver) 路径的公共辅助 ==================== */

/// 桥接根推进一次: 恢复协程帧并收束终态 (仅在 host driver 的 `poll_one` 内调用)。
template<typename Promise>
inline void advanceRootOnce(std::coroutine_handle<Promise> h) noexcept {
    if (!h) {
        return;
    }
    if (!h.done()) {
        try {
            h.resume();
        } catch (...) {
            h.promise().set_exception(std::current_exception());
        }
    }
    finishIfDone(h);
}

/// 恢复插件协程的统一入口: 先 `postToLocal` 再 `wake`, continuation 由**下一次
/// host driver** 的 `poll_one` 执行 —— 因此绝不会从宿主回调栈内重入插件协程,
/// 也不会占用宿主 IO 线程做插件工作 (只投递一个小闭包)。
///
/// 被放弃的根 (宿主拒绝驱动) 不再恢复: 直接返回, 帧由引用释放路径销毁。
template<typename Promise>
inline void resumePluginCoroutine(
    detail::PollOneBridge* bridge, std::coroutine_handle<Promise> handle
) noexcept {
    std::weak_ptr<detail::BridgeRoot> weakRoot = handle.promise().bridgeRoot_;
    bridge->postToLocal([handle, weakRoot] {
        if (auto root = weakRoot.lock()) {
            if (!root->shouldAdvance()) {
                return; // 已放弃: 不恢复, 帧由引用释放路径销毁
            }
        }
        try {
            handle.resume();
        } catch (...) {
            handle.promise().set_exception(std::current_exception());
        }
        finishIfDone(handle);
    });
    bridge->wake();
}

/// 启动一个桥接根协程 (plugin.md 6.2):
/// - 首步经本地执行器排队, 由**下一次 host driver** 执行; `execute_start` 因此不会
///   在宿主 IO 线程上同步跑插件业务代码 (也不会有 completion 重入);
/// - 帧的销毁责任交给 [BridgeRoot]: 排队中的本地任务持强引用, 最后一个引用释放
///   时销毁仍挂起的帧;
/// - `cleanup` 在"根终结恰好一次"时执行 (释放 op 句柄 / 注销根登记)。
///
/// `return`: 该根的仲裁对象 (调用方通常不再需要, 资源释放已由 cleanup 覆盖)。
template<typename Promise>
inline std::shared_ptr<detail::BridgeRoot> startBridgedRoot(
    detail::PollOneBridge&             bridge,
    const AgentxxPluginOperatorNotify& notify,
    std::coroutine_handle<Promise>     h,
    std::function<void()>              cleanup
) {
    auto root
        = std::make_shared<detail::BridgeRoot>(notify, h.address(), &destroyBridgeFrame<Promise>);
    root->setOwner(&bridge);
    {
        auto  userCleanup = std::move(cleanup);
        auto* bridgePtr   = &bridge;
        // 注销登记用 weak 引用: cleanup 本身保存在 root 内部, 若捕获 shared_ptr 就会
        // 形成自引用环 (被放弃的根将永不析构, 帧与 op 句柄一起泄漏)。
        std::weak_ptr<detail::BridgeRoot> weakRoot = root;
        root->setCleanup([bridgePtr, weakRoot, userCleanup] {
            if (auto alive = weakRoot.lock()) {
                bridgePtr->removeRoot(alive);
            }
            if (userCleanup) {
                userCleanup();
            }
        });
    }
    h.promise().bridgeRoot_ = root;
    bridge.addRoot(root);
    bridge.postToLocal([h, root] {
        if (!root->shouldAdvance()) {
            return; // 已放弃: 帧由引用释放路径销毁
        }
        advanceRootOnce(h);
    });
    bridge.wake();
    return root;
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
    std::string argsJson;   ///< 工具参数 / 能力参数 JSON（空则为 "{}"）
    std::string sessionId;  ///< ABI thread_id / session_id
    std::string callId;     ///< 工具 tool_call_id；能力方法名放 method
    std::string method;     ///< 能力方法名（非能力操作为空）
    std::string nodeName;   ///< 图节点实例名（仅图节点操作）
    std::string configJson; ///< 图节点 config JSON（仅图节点操作）
    std::string stateJson;  ///< GraphState::serialize() 只读快照（仅图节点操作）

    const AgentxxPluginHost* host = nullptr;
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
    static RootRequest
        forHook(const AgentxxPluginHost* host, const AgentxxPluginStringView* nodeInputJson) {
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

    /// 图节点：node_name/config_json/state_json/thread_id
    static RootRequest forGraphNode(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* nodeName,
        const AgentxxPluginStringView* configJson,
        const AgentxxPluginStringView* stateJson,
        const AgentxxPluginStringView* threadId
    ) {
        RootRequest req;
        req.host       = host;
        req.nodeName   = copyView(nodeName);
        req.configJson = copyView(configJson, "{}");
        req.stateJson  = copyView(stateJson, "{}");
        req.sessionId  = copyView(threadId);
        return req;
    }

    std::string_view args() const noexcept {
        return argsJson;
    }

    std::string_view session() const noexcept {
        return sessionId;
    }

    std::string_view call() const noexcept {
        return callId;
    }

    std::string_view capMethod() const noexcept {
        return method;
    }

    std::string_view node() const noexcept {
        return nodeName;
    }

    std::string_view config() const noexcept {
        return configJson;
    }

    std::string_view state() const noexcept {
        return stateJson;
    }
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
    /// 桥接根仲裁对象 (仅"根操作"帧持有; 子 Task 帧为空)。
    /// - 弱引用: 强引用由排队中的本地任务持有, 最后一个引用释放时销毁仍挂起的帧,
    ///   因此不会和帧本身形成引用环;
    /// - [finishIfDone] 经它做 exactly-once 上报, awaiter 的恢复任务经它判断
    ///   "根是否已被放弃"。
    std::weak_ptr<detail::BridgeRoot> bridgeRoot_{};

    std::suspend_always initial_suspend() noexcept {
        return {};
    }

    struct FinalAwaiter {
        bool await_ready() const noexcept {
            return false;
        }

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

/// 根操作拥有型输入的公开别名：插件业务签名可直接写 `const RootRequest&`
/// （tool/hook/capability/graph 通用；异步路径由 Job 持有到协程结束）。
using RootRequest = detail::RootRequest;

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

        bool await_ready() const noexcept {
            return !handle || handle.done();
        }

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

        bool await_ready() const noexcept {
            return !handle || handle.done();
        }

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
        // 桥 (协程驱动) 必须最后销毁: 它会取消排队请求并终结仍在排队的根, 而根的
        // 收束可能仍需访问上面的实例状态。
        stopBridge();
    }

    std::shared_ptr<std::atomic<bool>> lifeToken() const {
        return lifeToken_;
    }

    /// 本实例的协程驱动桥 (延迟创建; 每实例一份, 无进程级可变状态)。
    /// - 线程: 只应在宿主 IO 线程调用 (与实例状态同一串行上下文)
    detail::PollOneBridge& bridge() const {
        if (!bridge_) {
            bridge_ = std::make_unique<detail::PollOneBridge>(
                host,
                iface.coroutineRuntime,
                iface.scheduler
            );
        }
        return *bridge_;
    }

    /// 桥 (协程驱动) 必须最后销毁: 它会取消排队请求并终结仍在排队的根, 而根的
    /// 收束可能仍需访问上面的实例状态。
    void stopBridge() noexcept {
        if (bridge_) {
            bridge_->stop();
        }
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
    /// 协程驱动桥 (延迟创建; 见 [bridge])。声明在最后: 析构顺序需要它在其它成员
    /// 之后销毁 (它在 stop 时会终结仍在排队的根)。
    mutable std::unique_ptr<detail::PollOneBridge> bridge_;
};

/* ==================== 锚定原语 awaiter 族 ==================== */

namespace detail {

struct SleepAwaiter {
    const AgentxxPluginHost*           host;
    const AgentxxPluginSchedulerIface* sched;
    /// 协程驱动桥 (宿主支持时为非空): 完成回调经它投递 continuation 并唤醒 driver。
    PollOneBridge*               bridge = nullptr;
    int64_t                      ms;
    void*                        coroAddr  = nullptr;
    AgentxxPluginOperatorHandle* operation = nullptr;
    std::string                  error;

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
        auto& p  = h.promise();
        coroAddr = h.address();
        AgentxxPluginString errorOut{};
        operation = sched->sleep(
            host,
            ms,
            [](void* ud, int32_t status, const AgentxxPluginStringView* payload) {
                auto* self   = static_cast<SleepAwaiter*>(ud);
                auto  handle = std::coroutine_handle<Promise>::from_address(self->coroAddr);
                auto& prom   = handle.promise();
                prom.clear_outstanding();
                if (status == AGENTXX_PLUGIN_OPERATOR_CANCELLED) {
                    prom.set_exception(std::make_exception_ptr(CancelledException("sleep cancelled")
                    ));
                } else if (status == AGENTXX_PLUGIN_OPERATOR_FAILED) {
                    auto message = PluginStringView::str(payload);
                    prom.set_exception(std::make_exception_ptr(
                        std::runtime_error(message.empty() ? "sleep failed" : std::string(message))
                    ));
                }
                // 桥接路径不在宿主回调栈内恢复协程 (见 [resumePluginCoroutine])。
                resumePluginCoroutine(self->bridge, handle);
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

    void await_resume() const {
        if (!error.empty()) {
            throw std::runtime_error(error);
        }
    }
};

struct YieldAwaiter {
    const AgentxxPluginHost* host;
    /// 协程驱动桥: "让出" = 投递到本地执行器 + 唤醒 driver, 由下一次请求推进,
    /// 因此宿主与插件任务按轮次交替。
    PollOneBridge* bridge = nullptr;

    bool await_ready() noexcept {
        return false;
    }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        resumePluginCoroutine(bridge, h);
        return true;
    }

    void await_resume() const {}
};

template<typename WorkFn>
struct OffloadAwaiter {
    using ResultType = std::decay_t<std::invoke_result_t<WorkFn, const AgentxxPluginCancelToken*>>;

    const AgentxxPluginHost*           host;
    const AgentxxPluginSchedulerIface* sched;
    WorkFn                             work;
    /// 协程驱动桥。**注意**: offload 的工作体本身仍运行在宿主工作线程池
    /// (显式声明的例外), 只有"完成后的恢复"经桥回到 driver 序列。
    PollOneBridge*     bridge = nullptr;
    std::exception_ptr exPtr  = nullptr;
    std::conditional_t<std::is_void_v<ResultType>, std::monostate, std::optional<ResultType>>
                                 result;
    AgentxxPluginOperatorHandle* operation = nullptr;
    int32_t                      status    = AGENTXX_PLUGIN_OPERATOR_OK;
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
            [](void* ud, const AgentxxPluginCancelToken* token, AgentxxPluginString* error_out
            ) -> void* {
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
                self->error  = PluginStringView::str(err);
                auto  handle = std::coroutine_handle<Promise>::from_address(self->coroAddr_);
                auto& prom   = handle.promise();
                prom.clear_outstanding();
                // 桥接路径: 恢复走 driver 序列 (不在宿主完成回调栈内重入插件协程)。
                resumePluginCoroutine(self->bridge, handle);
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

struct CallToolState {
    const AgentxxPluginHost*       host  = nullptr;
    const AgentxxPluginToolsIface* tools = nullptr;
    /// 协程驱动桥 (可空): 完成回调经它投递 continuation 并唤醒 driver。
    PollOneBridge*               bridge = nullptr;
    std::string                  name;
    std::string                  argsJson;
    std::string                  threadId;
    AgentxxPluginOperatorHandle* opHandle = nullptr;
    int32_t                      status   = AGENTXX_PLUGIN_OPERATOR_OK;
    std::string                  payload;
    std::string                  startError;
    std::atomic<AwaiterState>    state{AwaiterState::INIT};
    void*                        coroAddr = nullptr;
};

struct CallToolAwaiter {
    std::shared_ptr<CallToolState> st;

    CallToolAwaiter(
        const AgentxxPluginHost*       in_host,
        const AgentxxPluginToolsIface* in_tools,
        std::string_view               in_name,
        std::string_view               in_args,
        std::string_view               in_tid,
        PollOneBridge*                 in_bridge = nullptr
    ) :
        st(std::make_shared<CallToolState>()) {
        st->host     = in_host;
        st->tools    = in_tools;
        st->name     = std::string(in_name);
        st->argsJson = std::string(in_args);
        st->threadId = std::string(in_tid);
        st->bridge   = in_bridge;
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

                // 桥接路径不在宿主回调栈内恢复插件协程 (见 [resumePluginCoroutine])。
                resumePluginCoroutine(s->bridge, handle);
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
    /// 协程驱动桥 (可空): 完成回调经它投递 continuation 并唤醒 driver。
    PollOneBridge*               bridge = nullptr;
    std::string                  capability;
    std::string                  method;
    std::string                  argsJson;
    AgentxxPluginOperatorHandle* opHandle = nullptr;
    int32_t                      status   = AGENTXX_PLUGIN_OPERATOR_OK;
    std::string                  payload;
    std::string                  startError;
    std::atomic<AwaiterState>    state{AwaiterState::INIT};
    void*                        coroAddr = nullptr;
};

struct InvokeCapAwaiter {
    std::shared_ptr<InvokeCapState> st;

    InvokeCapAwaiter(
        const AgentxxPluginHost*              in_host,
        const AgentxxPluginCapabilitiesIface* in_caps,
        std::string_view                      in_cap,
        std::string_view                      in_method,
        std::string_view                      in_args,
        PollOneBridge*                        in_bridge = nullptr
    ) :
        st(std::make_shared<InvokeCapState>()) {
        st->host       = in_host;
        st->caps       = in_caps;
        st->capability = std::string(in_cap);
        st->method     = std::string(in_method);
        st->argsJson   = std::string(in_args);
        st->bridge     = in_bridge;
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

                // 桥接路径不在宿主回调栈内恢复插件协程 (见 [resumePluginCoroutine])。
                resumePluginCoroutine(s->bridge, handle);
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

/// 宿主计时器适配 (非轮询): 计时器到期时宿主回调 adapter, adapter 把 continuation
/// 投递到本地执行器并 [detail::PollOneBridge::wake]; continuation 由下一次 driver
/// 推进, 因此既不需要额外线程, 也不占用宿主 IO 线程执行插件工作。
inline detail::SleepAwaiter sleep(const PluginBase& ctx, int64_t ms) noexcept {
    return detail::SleepAwaiter{ctx.host, ctx.iface.scheduler, &ctx.bridge(), ms};
}

/// 让出一轮 (等价于"下一位"): 有桥时投递 continuation + 唤醒 driver,
/// 由下一次请求推进; 无桥时沿用宿主 post_to_io。
inline detail::YieldAwaiter yield(const PluginBase& ctx) noexcept {
    return detail::YieldAwaiter{ctx.host, &ctx.bridge()};
}

/// 阻塞工作委托 (宿主工作线程池): **显式例外**, 工作体本身不在 driver 序列里运行;
/// 完成后的恢复仍回到 driver 序列 (见 [detail::OffloadAwaiter])。
template<typename WorkFn>
inline auto offload(const PluginBase& ctx, WorkFn&& work) {
    return detail::OffloadAwaiter<std::decay_t<WorkFn>>{
        ctx.host,
        ctx.iface.scheduler,
        std::forward<WorkFn>(work),
        &ctx.bridge()
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
        &ctx.bridge()
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
        &ctx.bridge()
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

    AgentxxPluginOperatorNotify  hostNotify{nullptr, nullptr};
    AgentxxPluginOperatorHandle* taskHandle = nullptr;
    if (ctx.iface.tasks && ctx.iface.tasks->register_task) {
        AgentxxPluginOperatorNotify notify{nullptr, nullptr};
        AgentxxPluginString         err{nullptr, 0};
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
            rec->ctl = std::make_shared<OpCtl>(OpCtl{cancelFlag, ctx.host, ctx.iface.cancel, ""});
        }
        auto task = fn(ctx, *rec->ctl);
        if (task.handle_) {
            auto h        = task.handle_;
            task.handle_  = nullptr;
            auto& p       = h.promise();
            p.host_       = ctx.host;
            p.cancelFlag_ = cancelFlag;
            p.notify_     = hostNotify;
            if (!h.done()) {
                if (auto recSp = recWeak.lock()) {
                    recSp->coroAddr = h.address();
                }
            }

            // 任务首步由 host driver 推进, 因此 spawn 不会在调用方栈里同步跑任务体
            // (调用方可能正处在宿主 start/注册事务中)。桥停止时会经 [BridgeRoot]
            // 把任务上报为失败, 帧也随之销毁。
            detail::startBridgedRoot(ctx.bridge(), hostNotify, h, [recWeak] {
                if (auto recSp = recWeak.lock()) {
                    recSp->coroAddr = nullptr;
                }
            });
        }
    };
    rec->starter = starter;
    ctx.spawns_.push_back(rec);
    // 首步仍在宿主 IO 线程排队执行 (与既有语义一致: spawn 不在 start 调用栈内跑任务体)
    auto*      raw    = rec.get();
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

        job->coroAddr = h.address();

        // 根的首步由 host driver 推进 (不在 execute_start 里同步跑插件协程,
        // 因此不会有 completion 重入, 也不会占住宿主 IO 线程)。
        detail::startBridgedRoot(shim->ctx->bridge(), p.notify_, h, [job] {
            delete job;
        });
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
        BlockShim*                   shim = nullptr;
        AgentxxPluginOperatorNotify  notify{};
        std::string                  args;
        std::string                  tid;
        std::string                  tcid;
        std::string                  workDir;
        std::string                  argsJson;
        std::string                  resultPayload;
        std::string                  errorPayload;
        bool                         isCancelled   = false;
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
                [](void* ud, const AgentxxPluginCancelToken* token, AgentxxPluginString* err_out
                ) -> void* {
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
                        *error_out    = scheduleError;
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
        auto* job = static_cast<Job*>(op);
        if (job->offloadHandle && job->shim && job->shim->ctx && job->shim->ctx->iface.scheduler
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

/* ==================== 受控轮询工具 (polled_tool) ====================
 *
 * 适用: 业务体是 **asio 协程**、等待的是**插件本地 reactor 上的内核就绪事件**
 * (socket 收发 / 子进程管道 / 文件 IO / 本地 steady_timer) 的工具。
 *
 * 为什么需要它: 这类等待没有"宿主可见唤醒源" —— driver 里的 `poll_one` 只会执行
 * 已经就绪的 handler, 不会让私有 reactor 的等待对象到期。因此插件在注册时就
 * **声明**"该工具需要受控轮询驱动", 桥据此在有在途操作时继续申请请求:
 * - 有进展 (本轮 `poll_one` 执行到了 handler) → 立即续下一次请求;
 * - 无进展 → 退避 `PollOneBridge::kPollIntervalMs` (宿主计时器) 后再驱动;
 * - 连续有进展超过 `PollOneBridge::kPollBurstMax` 步 → 强制让出一次, 交给宿主;
 * - **没有在途 polled 操作时不申请请求、不建定时器** (空闲零开销)。
 *
 * 业务签名与 [blocking_tool] 完全一致 (只把返回值换成 `asio::awaitable<std::string>`),
 * 因此迁移通常只是换一个注册函数名:
 * ```cpp
 * polled_tool(ctx, name, depict, schema,
 *     [](Ctx& c, std::string_view args, std::string_view tid, std::string_view workDir,
 *        const AgentxxPluginCancelToken* cancel) -> asio::awaitable<std::string> {
 *         ArgReader reader(args);
 *         co_return co_await doAsync(reader.raw(), c.workDir? ...);
 *     });
 * ```
 *
 * 约束 (与实现一起遵守):
 * - **不要**在 polled 协程里做重 CPU/阻塞工作: 它运行在宿主 IO 线程上
 *   (目录遍历 / 全文件扫描 / 向量相似度这类继续用 `blocking_tool`);
 * - **不要**在 polled 协程里 `co_await` kit 的 `Task` 型原语 (`sleep`/`call_tool`/
 *   `invoke_cap`): 它们的 awaiter 依赖 kit 自有 promise 接口, 而这里是
 *   `asio::awaitable`。需要等待时用 asio 原生 `steady_timer` (pump 下可用) 或经
 *   `bridge().local_executor()` 投递后续步骤;
 */

namespace detail {

/// 受控轮询工具的 Job 侧取消查询 (kit 自造的会话级令牌, 不是宿主 token):
/// 只读 Job 的取消标志与实例 CancelRegistry, 生命周期与该 polled 根相同。
/// 模板参数是 `polled_tool` 内的局部 Job 类型 (在其完整之后实例化)。
template<typename Job>
inline int32_t AGENTXX_PLUGIN_CALL
    polledJobTokenIsRequestedAbi(const AgentxxPluginCancelToken* token) {
    auto* job = (token && token->host_ud) ? static_cast<const Job*>(token->host_ud) : nullptr;
    return (job && job->cancelled()) ? 1 : 0;
}

/// 受控轮询工具的业务体包装 (两条路径共用): 运行业务协程并把终态写回 Job。
///
/// - 异常映射: `CancelledException` → CANCELLED, 其它异常 → FAILED;
/// - 业务体正常返回但期间已请求取消 → CANCELLED (payload 为空, 与
///   `blocking_tool` 的取消语义一致);
/// - 本协程自身不决定"由谁上报/释放 Job": 由 [runPolledPumpJob] 收尾。
template<typename Job>
inline asio::awaitable<void> runPolledToolBody(Job* job) {
    try {
        if (job->cancelled()) {
            throw CancelledException("polled tool cancelled before execution");
        }
        job->payload = co_await job->shim
                           ->fn(*job->shim->ctx, job->args, job->tid, job->workDir, &job->token);
        job->status = AGENTXX_PLUGIN_OPERATOR_OK;
    } catch (const CancelledException& e) {
        job->status  = AGENTXX_PLUGIN_OPERATOR_CANCELLED;
        job->payload = e.what();
    } catch (const std::exception& e) {
        job->status  = AGENTXX_PLUGIN_OPERATOR_FAILED;
        job->payload = e.what();
    } catch (...) {
        job->status  = AGENTXX_PLUGIN_OPERATOR_FAILED;
        job->payload = "unknown polled tool error";
    }
    if (job->status == AGENTXX_PLUGIN_OPERATOR_OK && job->cancelled()) {
        job->status = AGENTXX_PLUGIN_OPERATOR_CANCELLED;
        job->payload.clear();
    }
}

/// 受控轮询工具的业务体包装 (泵路径): 在桥的本地执行器上跑完业务协程, 然后
/// 上报终态并回收 Job (泵路径下 Job 由本协程回收)。
///
/// 停止/关闭路径 (桥 `stop` → [PollOneBridge::failAllPolledRoots]) 可能先一步
/// 认领完成权并回收 Job: 那时 `claimFinish()` 返回 false, 本协程只做退出。
template<typename Job>
inline asio::awaitable<void> runPolledPumpJob(Job* job) {
    co_await runPolledToolBody(job);
    // 先取本地副本: 收尾 (cleanup) 会释放 Job 本身。
    auto          root    = job->root;
    auto*         bridge  = job->bridge;
    const int32_t status  = job->status;
    std::string   payload = job->payload;
    if (!root || !root->claimFinish()) {
        co_return;
    }
    if (bridge) {
        bridge->removePolledRoot(root);
    }
    root->notifyHost(status, payload);
    // cleanup 释放 Job (幂等); 本地 `root` 保证 PolledRoot 自身活到本语句之后。
    root->runCleanup();
}

} // namespace detail

template<typename Ctx, typename PolledFn>
inline void polled_tool(
    Ctx&             ctx,
    std::string_view name,
    std::string_view depict,
    std::string_view schema,
    PolledFn&&       fn,
    int64_t          default_timeout_ms = 0,
    int32_t          flags              = 0
) {
    using PolledFnType = std::decay_t<PolledFn>;
    static_assert(
        std::is_invocable_v<
            PolledFnType,
            Ctx&,
            std::string_view,
            std::string_view,
            std::string_view,
            const AgentxxPluginCancelToken*>,
        "polled_tool 业务体签名必须是 (Ctx&, std::string_view args, std::string_view tid, "
        "std::string_view workDir, const AgentxxPluginCancelToken*) -> asio::awaitable<std::string>"
    );

    auto&       storage     = ctx.storage_;
    std::string finalDepict = ctx.toolPrompt(name).depict;
    if (finalDepict.empty()) {
        finalDepict = depict;
    }
    storage.push_back(std::move(finalDepict));
    storage.push_back(std::string(schema));

    struct PolledShim {
        Ctx*         ctx = nullptr;
        PolledFnType fn;
    };

    auto shim
        = ctx.storeShim(std::make_unique<PolledShim>(PolledShim{&ctx, std::forward<PolledFn>(fn)}));

    /// 一次 polled 根操作的拥有型状态。
    /// 生命周期: 业务协程帧 + [detail::runPolledPumpJob] 的收尾;
    /// 回收由 [detail::PolledRoot] 的 cleanup 兜底 (停止/关闭时会提前接管)。
    struct Job {
        PolledShim*                         shim   = nullptr;
        detail::PollOneBridge*              bridge = nullptr;
        std::shared_ptr<detail::PolledRoot> root;
        AgentxxPluginOperatorNotify         notify{};
        detail::RootRequest                 request;
        std::string                         tid;     ///< 会话标识 (= request.sessionId)
        std::string                         args;    ///< 工具参数 JSON
        std::string                         workDir; ///< 会话工作目录 (IO 线程预取)
        std::shared_ptr<std::atomic<bool>>  cancelFlag;
        /// 传给业务体的取消令牌: kit 自造 (is_requested 读取消标志 + CancelRegistry)。
        AgentxxPluginCancelToken token{nullptr, nullptr};
        /// 终态 (由业务体包装写入; 上报方见上)。
        int32_t     status = AGENTXX_PLUGIN_OPERATOR_OK;
        std::string payload;

        /// 是否已请求取消 (本实例取消标志 + 会话级 CancelRegistry)。
        bool cancelled() const noexcept {
            if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
                return true;
            }
            if (shim && shim->ctx && !tid.empty() && shim->ctx->cancelRegistry.isCancelled(tid)) {
                return true;
            }
            return false;
        }
    };

    // 自造取消令牌的查询函数: 按 host_ud 还原 Job (Job 为局部类型, 因此用
    // 上面的模板实例化; 此处 Job 已完整)。
    constexpr auto kTokenIsRequested = &detail::polledJobTokenIsRequestedAbi<Job>;

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
        auto* s = static_cast<PolledShim*>(user_data);
        if (!s || !s->ctx) {
            if (error_out) {
                *error_out = PluginString::fromCstr(nullptr, "polled tool: context released");
            }
            return nullptr;
        }
        auto& c = *s->ctx;
        // 拥有型 Request: args/session/tool_call_id 在整个 polled 根期间有效
        // (协程会挂起并继续读取它们)。
        auto request    = detail::RootRequest::forTool(c.host, args_json, thread_id, tool_call_id);
        auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
        if (!request.sessionId.empty() && c.cancelRegistry.isCancelled(request.sessionId)) {
            cancelFlag->store(true, std::memory_order_release);
        }

        auto* job       = new Job();
        job->shim       = s;
        job->notify     = notify ? *notify : AgentxxPluginOperatorNotify{nullptr, nullptr};
        job->request    = std::move(request);
        job->tid        = job->request.sessionId;
        job->args       = job->request.argsJson;
        job->workDir    = c.workDir(job->tid);
        job->cancelFlag = cancelFlag;
        job->root       = std::make_shared<detail::PolledRoot>(job->notify);
        job->token      = AgentxxPluginCancelToken{kTokenIsRequested, job};
        {
            // 兜底回收: 停止/关闭路径 (failAllPolledRoots) 会执行它; 正常完成时
            // 由各自的上报方先执行, 这里保持幂等。
            auto* jobPtr = job;
            job->root->setCleanup([jobPtr] {
                delete jobPtr;
            });
        }

        auto* bridge = &c.bridge();
        job->bridge  = bridge;
        bridge->addPolledRoot(job->root);
        try {
            // 业务协程跑在**桥的本地执行器**上: 它的 socket/管道/文件等待因此
            // 注册到桥的 reactor, 由受控轮询推进 (首步恒异步, 不在本调用内执行)。
            asio::co_spawn(bridge->local_executor(), detail::runPolledPumpJob(job), asio::detached);
        } catch (...) {
            // 首步都无法排队: 按**拒绝**处理 (未产生任何宿主可见副作用),
            // 回收 Job 并返回 NULL + error。
            bridge->removePolledRoot(job->root);
            auto keep = job->root;
            job->root->claimFinish();
            job->root->runCleanup();
            if (error_out) {
                *error_out = PluginString::fromCstr(
                    c.host,
                    "polled tool: failed to schedule coroutine"
                );
            }
            return nullptr;
        }
        // co_spawn 已把首步投递到本地执行器, 但 kit 看不到这次投递
        // (不计入 readySteps_), 因此必须显式 wake —— 否则首步不会被驱动。
        bridge->wake();
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
        if (job->shim && job->shim->ctx) {
            if (!job->tid.empty()) {
                job->shim->ctx->cancelRegistry.cancel(job->tid);
            }
            // 取消在途退避定时器, 让插件立刻得到一次驱动 (不必等满一个退避量子),
            // 从而尽快在阶段边界看到取消并收束根。
            if (job->bridge) {
                job->bridge->kickPumpWait();
            }
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

        using HookRet = decltype(detail::invokeHook(shim->fn, *shim->ctx, pt, std::string_view{}));
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
            auto h        = task.handle_;
            task.handle_  = nullptr;
            auto& p       = h.promise();
            p.notify_     = notify ? *notify : AgentxxPluginOperatorNotify{nullptr, nullptr};
            p.host_       = job->request.host;
            p.cancelFlag_ = job->cancelFlag;
            job->coroAddr = h.address();

            // 根的首步由 host driver 推进 (不在 start 里同步跑插件协程,
            // 因此没有 completion 重入, 也不占住宿主 IO 线程)。
            detail::startBridgedRoot(shim->ctx->bridge(), p.notify_, h, [job] {
                delete job;
            });
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
    CapFn&                   fn,
    Ctx&                     ctx,
    const AgentxxPluginHost* caller,
    std::string_view         method,
    std::string_view         args
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
                auto request
                    = detail::RootRequest::forCapability(shim->ctx->host, method, args_json);

                using CapRet = decltype(detail::invokeCap(
                    shim->fn,
                    *shim->ctx,
                    caller_host,
                    std::string_view{},
                    std::string_view{}
                ));
                // 同步能力: void 或字符串类返回值；其余 (Task<T>) 走异步路径。
                constexpr bool kSyncCap
                    = std::is_void_v<CapRet> || std::is_convertible_v<CapRet, std::string_view>;
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
                    auto h       = task.handle_;
                    task.handle_ = nullptr;
                    auto& p      = h.promise();
                    p.notify_    = notify ? *notify : AgentxxPluginOperatorNotify{nullptr, nullptr};
                    p.host_      = job->request.host;
                    p.cancelFlag_ = job->cancelFlag;
                    job->coroAddr = h.address();

                    // 根的首步由 host driver 推进 (与工具/钩子同一模型)。
                    detail::startBridgedRoot(shim->ctx->bridge(), p.notify_, h, [job] {
                        delete job;
                    });
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

namespace detail {

/// 按可调用性选择图节点业务签名：fn(ctx, request, ctl) / fn(ctx, request)。
/// 返回类型原样转发：快同步节点返回 `std::string`（节点输出 JSON），
/// 锚定协程节点返回 `Task<T>`（同上，result 为节点输出 JSON）。
template<typename NodeFn, typename Ctx>
inline decltype(auto) invokeGraphNode(NodeFn& fn, Ctx& ctx, const RootRequest& req, OpCtl ctl) {
    if constexpr (std::is_invocable_v<NodeFn, Ctx&, const RootRequest&, OpCtl>) {
        return fn(ctx, req, std::move(ctl));
    } else {
        return fn(ctx, req);
    }
}

} // namespace detail

/// 注册插件自定义图节点类型（统一 root adapter）。
///
/// 与 tool/hook/capability 使用同一套生命周期与完成协议：
/// - 输入（node/config/state/thread_id）由 [RootRequest] 拥有，跨挂起点有效（F13）；
/// - 返回 `std::string`：快同步节点，调用返回即完成（异常 → FAILED）；
/// - 返回 `Task<std::string>`：锚定协程节点，宿主获得可取消的 provider 句柄，
///   完成通知在协程真正结束后 exactly-once 发出；
/// - `run_cancel` 置取消标志并取消嵌套 awaiter。
///
/// `return`: 0 = 注册成功；非 0 = 类型名冲突或宿主不支持（调用方应使 start 事务失败）。
template<typename Ctx, typename NodeFn>
inline int32_t
    graph_node(Ctx& ctx, std::string_view type, std::string_view configSchema, NodeFn&& fn) {
    if (!ctx.iface.graph || !ctx.iface.graph->register_node_type) {
        return -1;
    }

    struct NodeShim {
        Ctx*                 ctx = nullptr;
        std::decay_t<NodeFn> fn;
    };

    /// 异步节点的 provider 句柄：拥有输入 Request，由 promise.opCleanup_ 回收。
    struct NodeJob {
        NodeShim*                          shim = nullptr;
        std::shared_ptr<std::atomic<bool>> cancelFlag;
        void*                              coroAddr = nullptr;
        detail::RootRequest                request;
    };

    auto shim = ctx.storeShim(std::make_unique<NodeShim>(NodeShim{&ctx, std::forward<NodeFn>(fn)}));

    AgentxxPluginGraphNodeTypeSpec spec{};
    spec.type               = PluginStringView::from(type.data(), type.size());
    spec.config_schema_json = PluginStringView::from(configSchema.data(), configSchema.size());
    spec.user_data          = shim;

    spec.run_start = [](void*                              user_data,
                        const AgentxxPluginStringView*     node_name,
                        const AgentxxPluginStringView*     config_json,
                        const AgentxxPluginStringView*     state_json,
                        const AgentxxPluginStringView*     thread_id,
                        const AgentxxPluginOperatorNotify* notify,
                        AgentxxPluginString*               error_out) -> void* {
        auto* shim = static_cast<NodeShim*>(user_data);
        (void)error_out;
        if (!shim || !shim->ctx) {
            detail::CompletionGuard guard(notify);
            guard.failed("graph node context released");
            return nullptr;
        }
        // 输入纳入拥有型 Request：快同步节点在调用期间有效，异步节点由 Job
        // 持有到协程真正结束（F13）。
        auto request = detail::RootRequest::forGraphNode(
            shim->ctx->host,
            node_name,
            config_json,
            state_json,
            thread_id
        );
        auto  cancelFlag = std::make_shared<std::atomic<bool>>(false);
        OpCtl ctl{
            cancelFlag,
            shim->ctx->host,
            shim->ctx->iface.cancel,
            request.sessionId,
            &shim->ctx->cancelRegistry
        };

        using NodeRet = decltype(detail::invokeGraphNode(shim->fn, *shim->ctx, request, ctl));
        if constexpr (std::is_void_v<NodeRet> || std::is_convertible_v<NodeRet, std::string_view>) {
            /// 快同步节点：调用返回即完成；异常统一映射为终态。
            detail::CompletionGuard guard(notify);
            try {
                if constexpr (std::is_void_v<NodeRet>) {
                    detail::invokeGraphNode(shim->fn, *shim->ctx, request, std::move(ctl));
                    guard.ok();
                } else {
                    guard.ok(detail::invokeGraphNode(shim->fn, *shim->ctx, request, std::move(ctl))
                    );
                }
            } catch (...) {
                guard.fromCurrentException();
            }
            return nullptr;
        } else {
            /// Task<T> 节点：由 promise 在协程结束后收束完成通知，
            /// 返回 Job 作为宿主可取消的 provider 句柄。
            auto* job = new NodeJob{shim, std::move(cancelFlag), nullptr, std::move(request)};
            auto task = detail::invokeGraphNode(shim->fn, *shim->ctx, job->request, std::move(ctl));
            if (!task.handle_) {
                delete job;
                detail::CompletionGuard guard(notify);
                guard.failed("graph node returned an empty task");
                return nullptr;
            }
            auto h        = task.handle_;
            task.handle_  = nullptr;
            auto& p       = h.promise();
            p.notify_     = notify ? *notify : AgentxxPluginOperatorNotify{nullptr, nullptr};
            p.host_       = job->request.host;
            p.cancelFlag_ = job->cancelFlag;
            job->coroAddr = h.address();

            // 根的首步由 host driver 推进 (不在 start 里同步跑插件协程,
            // 因此没有 completion 重入, 也不占住宿主 IO 线程)。
            detail::startBridgedRoot(shim->ctx->bridge(), p.notify_, h, [job] {
                delete job;
            });
            return job;
        }
    };

    spec.run_cancel = [](void* user_data, void* op) {
        (void)user_data;
        if (!op) {
            return;
        }
        auto* job = static_cast<NodeJob*>(op);
        if (job->cancelFlag) {
            job->cancelFlag->store(true, std::memory_order_release);
        }
        if (job->coroAddr) {
            auto handle
                = std::coroutine_handle<detail::PromiseBase<void>>::from_address(job->coroAddr);
            handle.promise().cancel_outstanding();
        }
    };

    return ctx.iface.graph->register_node_type(ctx.host, &spec);
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

/* ==================== 一键式插件入口导出宏 ==================== */

namespace detail {

/// 生命周期入口公共守卫 (导出宏内部使用):
/// 捕获跨 C ABI 的异常并写入 `err` (宿主回滚依据), 统一返回 NULL 表示"本次事务未同步成功"。
template<typename Fn>
inline void* callLifecycleEntry(
    const AgentxxPluginHost*           host,
    void*                              plugin_ctx,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               err,
    const char*                        label,
    Fn&&                               fn
) noexcept {
    try {
        if (!plugin_ctx) {
            PluginString::set(host, err, fmt::format("{}: null plugin context", label));
            return nullptr;
        }
        if (!notify || !notify->done) {
            PluginString::set(host, err, fmt::format("{}: null completion notify", label));
            return nullptr;
        }
        return fn();
    } catch (const std::exception& e) {
        PluginString::set(host, err, e.what());
    } catch (...) {
        PluginString::set(host, err, fmt::format("{}: unknown exception", label));
    }
    return nullptr;
}

} // namespace detail

/// create 阶段异常上报 (上下文可能尚未构造成功, 直接经宿主日志接口输出)
inline void logCreateFailure(
    const AgentxxPluginHost* host, std::string_view plugin, std::string_view msg
) noexcept {
    if (!host || !host->vtable || !host->vtable->query_interface) {
        return;
    }
    auto  iid   = PluginStringView::fromCstr(AGENTXX_PLUGIN_IFACE_AGENT_LOG);
    auto* iface = static_cast<const AgentxxPluginLogIface*>(
        host->vtable->query_interface(host, &iid)
    );
    if (!iface || !iface->log) {
        return;
    }
    std::string text = fmt::format("[{}] create failed: {}", plugin, msg);
    auto        sv   = PluginStringView::from(text.data(), text.size());
    iface->log(host, 4, &sv);
}

/// client 侧 create 阶段异常上报 (client 日志接口表为独立类型)
inline void logClientCreateFailure(
    const AgentxxPluginHost* host, std::string_view plugin, std::string_view msg
) noexcept {
    if (!host || !host->vtable || !host->vtable->query_interface) {
        return;
    }
    auto  iid   = PluginStringView::fromCstr(AGENTXX_IFACE_CLIENT_LOG);
    auto* iface = static_cast<const AgentxxClientLogIface*>(
        host->vtable->query_interface(host, &iid)
    );
    if (!iface || !iface->log) {
        return;
    }
    std::string text = fmt::format("[{}] create failed: {}", plugin, msg);
    auto        sv   = PluginStringView::from(text.data(), text.size());
    iface->log(host, 4, &sv);
}

/// Agent 侧插件入口导出 (生成 get_info / create / start / stop / destroy 五个符号)。
///
/// 宿主按以下顺序调用, 插件必须遵守 (见 docs/zh-cn/design/plugins.md):
/// - `create`: 只构造上下文 (`new CtxType` + `init(host)`), 不注册任何工具/钩子/能力/
///   订阅, 不启动线程;
/// - `start`: **注册事务**, 在宿主 IO 线程执行; 同步完成时 `notify->done(OK)` 后返回 NULL,
///   失败时返回 NULL 并写出 `error_out` (宿主回滚本次注册并撤销实例);
/// - `stop`: 撤销自管资源 (线程/定时器/订阅), 可重复调用; 完成后宿主才调用 destroy;
/// - `destroy`: 只释放本地对象, 不调用宿主接口、不创建异步工作。
///
/// `StartFn` / `StopFn` 形如
/// `void*(CtxType&, const AgentxxPluginOperatorNotify*, AgentxxPluginString* error_out)`。
#define AGENTXX_PLUGIN_AGENT_EXPORT(CtxType, Name, Ver, Desc, StartFn, StopFn)                    \
    extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void \
    ) {                                                                                           \
        static const AgentxxPluginInfo info{                                                      \
            AGENTXX_PLUGIN_API_VERSION,                                                           \
            0,                                                                                    \
            agentxx::plugin::PluginStringView::fromCstr(Name),                                    \
            agentxx::plugin::PluginStringView::fromCstr(Ver),                                     \
            agentxx::plugin::PluginStringView::fromCstr(Desc),                                    \
        };                                                                                        \
        return &info;                                                                             \
    }                                                                                             \
    extern "C" AGENTXX_PLUGIN_EXPORT int32_t                                                      \
        agentxx_plugin_agent_create(const AgentxxPluginHost* host, void** plugin_ctx) {           \
        if (!host || !plugin_ctx) {                                                               \
            return -1;                                                                            \
        }                                                                                         \
        try {                                                                                     \
            auto ctx = std::make_unique<CtxType>();                                               \
            ctx->init(host);                                                                      \
            *plugin_ctx = ctx.release();                                                          \
            return 0;                                                                             \
        } catch (const std::exception& e) {                                                       \
            agentxx::plugin::logCreateFailure(host, Name, e.what());                              \
        } catch (...) {                                                                           \
            agentxx::plugin::logCreateFailure(host, Name, "unknown exception");                   \
        }                                                                                         \
        return -1;                                                                                \
    }                                                                                             \
    extern "C" AGENTXX_PLUGIN_EXPORT void* agentxx_plugin_agent_start(                            \
        void*                              plugin_ctx,                                            \
        const AgentxxPluginOperatorNotify* notify,                                                \
        AgentxxPluginString*               err                                                    \
    ) {                                                                                           \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                            \
        return agentxx::plugin::detail::callLifecycleEntry(                                       \
            ctx ? ctx->host : nullptr,                                                            \
            plugin_ctx,                                                                           \
            notify,                                                                               \
            err,                                                                                  \
            "plugin start",                                                                       \
            [&]() -> void* { return (StartFn)(*ctx, notify, err); }                               \
        );                                                                                        \
    }                                                                                             \
    extern "C" AGENTXX_PLUGIN_EXPORT void* agentxx_plugin_agent_stop(                             \
        void*                              plugin_ctx,                                            \
        const AgentxxPluginOperatorNotify* notify,                                                \
        AgentxxPluginString*               err                                                    \
    ) {                                                                                           \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                            \
        return agentxx::plugin::detail::callLifecycleEntry(                                       \
            ctx ? ctx->host : nullptr,                                                            \
            plugin_ctx,                                                                           \
            notify,                                                                               \
            err,                                                                                  \
            "plugin stop",                                                                        \
            [&]() -> void* { return (StopFn)(*ctx, notify, err); }                                \
        );                                                                                        \
    }                                                                                             \
    extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {        \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                            \
        if (ctx) {                                                                                \
            delete ctx;                                                                           \
        }                                                                                         \
    }

/// 只导出 start/stop 两个入口 (供手写 create/destroy 的插件使用)。
///
/// 少数插件需要自己控制实例构造或销毁 (如按平台条件创建运行时对象、加自定义日志),
/// 它们手写 `agentxx_plugin_agent_create` / `agentxx_plugin_agent_destroy`, 但仍用本宏
/// 生成带异常兜底的 start/stop trampoline —— 与 [AGENTXX_PLUGIN_AGENT_EXPORT] 的生命周期
/// 部分同语义。
#define AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT(CtxType, StartFn, StopFn)                         \
    extern "C" AGENTXX_PLUGIN_EXPORT void* agentxx_plugin_agent_start(                          \
        void*                              plugin_ctx,                                          \
        const AgentxxPluginOperatorNotify* notify,                                              \
        AgentxxPluginString*               err                                                  \
    ) {                                                                                         \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                          \
        return agentxx::plugin::detail::callLifecycleEntry(                                     \
            ctx ? ctx->host : nullptr,                                                          \
            plugin_ctx,                                                                         \
            notify,                                                                             \
            err,                                                                                \
            "plugin start",                                                                     \
            [&]() -> void* { return (StartFn)(*ctx, notify, err); }                             \
        );                                                                                      \
    }                                                                                           \
    extern "C" AGENTXX_PLUGIN_EXPORT void* agentxx_plugin_agent_stop(                           \
        void*                              plugin_ctx,                                          \
        const AgentxxPluginOperatorNotify* notify,                                              \
        AgentxxPluginString*               err                                                  \
    ) {                                                                                         \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                          \
        return agentxx::plugin::detail::callLifecycleEntry(                                     \
            ctx ? ctx->host : nullptr,                                                          \
            plugin_ctx,                                                                         \
            notify,                                                                             \
            err,                                                                                \
            "plugin stop",                                                                      \
            [&]() -> void* { return (StopFn)(*ctx, notify, err); }                              \
        );                                                                                      \
    }

/// Client 侧只导出 start/stop 两个入口 (与
/// [AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT] 对称)。
#define AGENTXX_PLUGIN_CLIENT_LIFECYCLE_EXPORT(CtxType, StartFn, StopFn)                        \
    extern "C" AGENTXX_PLUGIN_EXPORT void* agentxx_plugin_client_start(                         \
        void*                              plugin_ctx,                                          \
        const AgentxxPluginOperatorNotify* notify,                                              \
        AgentxxPluginString*               err                                                  \
    ) {                                                                                         \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                          \
        return agentxx::plugin::detail::callLifecycleEntry(                                     \
            ctx ? ctx->host : nullptr,                                                          \
            plugin_ctx,                                                                         \
            notify,                                                                             \
            err,                                                                                \
            "client plugin start",                                                              \
            [&]() -> void* { return (StartFn)(*ctx, notify, err); }                             \
        );                                                                                      \
    }                                                                                           \
    extern "C" AGENTXX_PLUGIN_EXPORT void* agentxx_plugin_client_stop(                          \
        void*                              plugin_ctx,                                          \
        const AgentxxPluginOperatorNotify* notify,                                              \
        AgentxxPluginString*               err                                                  \
    ) {                                                                                         \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                          \
        return agentxx::plugin::detail::callLifecycleEntry(                                     \
            ctx ? ctx->host : nullptr,                                                          \
            plugin_ctx,                                                                         \
            notify,                                                                             \
            err,                                                                                \
            "client plugin stop",                                                               \
            [&]() -> void* { return (StopFn)(*ctx, notify, err); }                              \
        );                                                                                      \
    }

/// Client 侧插件入口导出 (与 [AGENTXX_PLUGIN_AGENT_EXPORT] 对称)。
///
/// `StartFn` / `StopFn` 形如
/// `void*(CtxType&, const AgentxxPluginOperatorNotify*, AgentxxPluginString* error_out)`;
/// start 里做 UI 项/命令/订阅注册, stop 里撤销插件自管资源 (线程/定时器/订阅)。
/// 纯 UI 插件 (无 agent 侧入口) 只导出 get_info + 下列四个入口即可。
#define AGENTXX_PLUGIN_CLIENT_EXPORT(CtxType, Name, Ver, Desc, StartFn, StopFn)               \
    extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxClientPluginInfo*                           \
        agentxx_plugin_client_get_info(void) {                                                \
        static const AgentxxClientPluginInfo info{                                            \
            AGENTXX_CLIENT_PLUGIN_API_VERSION,                                                \
            0,                                                                                \
            agentxx::plugin::PluginStringView::fromCstr(Name),                                \
            agentxx::plugin::PluginStringView::fromCstr(Ver),                                 \
            agentxx::plugin::PluginStringView::fromCstr(Desc),                                \
        };                                                                                    \
        return &info;                                                                         \
    }                                                                                         \
    extern "C" AGENTXX_PLUGIN_EXPORT int32_t                                                  \
        agentxx_plugin_client_create(const AgentxxPluginHost* host, void** plugin_ctx) {      \
        if (!host || !plugin_ctx) {                                                           \
            return -1;                                                                        \
        }                                                                                     \
        try {                                                                                 \
            auto ctx = std::make_unique<CtxType>();                                           \
            ctx->init(host);                                                                  \
            *plugin_ctx = ctx.release();                                                      \
            return 0;                                                                         \
        } catch (const std::exception& e) {                                                   \
            agentxx::plugin::logClientCreateFailure(host, Name, e.what());                    \
        } catch (...) {                                                                       \
            agentxx::plugin::logClientCreateFailure(host, Name, "unknown exception");         \
        }                                                                                     \
        return -1;                                                                            \
    }                                                                                         \
    extern "C" AGENTXX_PLUGIN_EXPORT void* agentxx_plugin_client_start(                       \
        void*                              plugin_ctx,                                        \
        const AgentxxPluginOperatorNotify* notify,                                            \
        AgentxxPluginString*               err                                                \
    ) {                                                                                       \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                        \
        return agentxx::plugin::detail::callLifecycleEntry(                                   \
            ctx ? ctx->host : nullptr,                                                        \
            plugin_ctx,                                                                       \
            notify,                                                                           \
            err,                                                                              \
            "client plugin start",                                                            \
            [&]() -> void* { return (StartFn)(*ctx, notify, err); }                           \
        );                                                                                    \
    }                                                                                         \
    extern "C" AGENTXX_PLUGIN_EXPORT void* agentxx_plugin_client_stop(                        \
        void*                              plugin_ctx,                                        \
        const AgentxxPluginOperatorNotify* notify,                                            \
        AgentxxPluginString*               err                                                \
    ) {                                                                                       \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                        \
        return agentxx::plugin::detail::callLifecycleEntry(                                   \
            ctx ? ctx->host : nullptr,                                                        \
            plugin_ctx,                                                                       \
            notify,                                                                           \
            err,                                                                              \
            "client plugin stop",                                                             \
            [&]() -> void* { return (StopFn)(*ctx, notify, err); }                            \
        );                                                                                    \
    }                                                                                         \
    extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_client_destroy(void* plugin_ctx) {   \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                        \
        if (ctx) {                                                                            \
            delete ctx;                                                                       \
        }                                                                                     \
    }

} // namespace plugin
} // namespace agentxx
