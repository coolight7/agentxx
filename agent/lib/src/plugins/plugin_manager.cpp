#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/agent/io/agent_io.h"
#include "agentxx/agent/io/agent_io_transport.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/util/async_offload.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "neograph/graph/cancel.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace agentxx {
namespace plugin {

using agentxx::agent::AgentContext;

/// 插件互调 tool_call_id 生成 (仅用于标识, 可读性要求不高)
static std::atomic<size_t> g_pluginCallSeq{0};

} // namespace plugin
} // namespace agentxx

/// 事件订阅句柄实现 (全局命名空间, 与 plugin_api.h 的 C 类型一致)
struct AgentxxSubscription {
    std::shared_ptr<agentxx::middleware::EventBus> bus;
    std::string                                    topic;          ///< 完整 topic (含 plugin. 前缀)
    size_t                                         subscriptionId = 0;
    agentxx::plugin::PluginInstance*               inst = nullptr;
    void (*handler)(const char* event_json, void* ud) = nullptr;
    void*                                          ud = nullptr;
};

namespace agentxx {
namespace plugin {

// =====================================================================
// NativeLoader (平台动态库封装)
// =====================================================================

void* NativeLoader::open(const std::string& path, std::string& err) {
#if defined(_WIN32)
    // LoadLibraryW: 支持宽字符路径 (UTF-8 → UTF-16)
    std::wstring wpath;
    {
        int len = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        if (len > 0) {
            wpath.resize(static_cast<size_t>(len) - 1);
            ::MultiByteToWideChar(
                CP_UTF8, 0, path.c_str(), -1, wpath.data(), static_cast<int>(wpath.size())
            );
        }
    }
    HMODULE h = ::LoadLibraryW(wpath.c_str());
    if (!h) {
        err = fmt::format("LoadLibrary failed: error {}", ::GetLastError());
        return nullptr;
    }
    return reinterpret_cast<void*>(h);
#else
    void* h = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* d = ::dlerror();
        err = d ? d : "dlopen failed";
        return nullptr;
    }
    return h;
#endif
}

void* NativeLoader::sym(void* handle, const char* name, std::string& err) {
#if defined(_WIN32)
    FARPROC p = ::GetProcAddress(reinterpret_cast<HMODULE>(handle), name);
    if (!p) {
        err = fmt::format("GetProcAddress({}) failed: error {}", name, ::GetLastError());
        return nullptr;
    }
    return reinterpret_cast<void*>(p);
#else
    ::dlerror(); // 清空错误
    void* p = ::dlsym(handle, name);
    const char* d = ::dlerror();
    if (d) {
        err = fmt::format("dlsym({}) failed: {}", name, d);
        return nullptr;
    }
    return p;
#endif
}

void NativeLoader::close(void* handle) {
    if (!handle) {
        return;
    }
#if defined(_WIN32)
    ::FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    ::dlclose(handle);
#endif
}

void NativeLoader::addSearchPath(std::string_view dir) {
    // 一期: 由调用方 (config_loader) 解析为绝对路径, 无需全局搜索路径
    (void)dir;
}

// =====================================================================
// CapabilityRegistry
// =====================================================================

bool CapabilityRegistry::registerCapability(std::string_view name, std::string_view provider) {
    if (name.empty()) {
        return false;
    }
    // 同名能力重复注册: 拒绝 (能力委派需唯一 provider)
    if (caps_.contains(std::string{name})) {
        XX_LOGW("CapabilityRegistry: capability `{}` already registered by `{}`", name,
                caps_.at(std::string{name}));
        return false;
    }
    caps_[std::string{name}] = std::string{provider};
    XX_LOGI("CapabilityRegistry: `{}` registered by plugin `{}`", name, provider);
    return true;
}

bool CapabilityRegistry::unregisterCapability(std::string_view name, std::string_view provider) {
    auto it = caps_.find(std::string{name});
    if (it == caps_.end()) {
        return false;
    }
    if (it->second != provider) {
        XX_LOGW("CapabilityRegistry: capability `{}` owned by `{}`, cannot unregister by `{}`",
                name, it->second, provider);
        return false;
    }
    caps_.erase(it);
    return true;
}

bool CapabilityRegistry::has(std::string_view name) const {
    return caps_.contains(std::string{name});
}

std::string CapabilityRegistry::providerOf(std::string_view name) const {
    auto it = caps_.find(std::string{name});
    if (it == caps_.end()) {
        return {};
    }
    return it->second;
}

std::vector<std::string> CapabilityRegistry::names() const {
    std::vector<std::string> out;
    out.reserve(caps_.size());
    for (const auto& [name, provider] : caps_) {
        (void)provider;
        out.push_back(name);
    }
    return out;
}

// =====================================================================
// PluginInstance
// =====================================================================

PluginInstance::~PluginInstance() {
    if (dlHandle) {
        NativeLoader::close(dlHandle);
        dlHandle = nullptr;
    }
}

// =====================================================================
// PluginTool (C ABI spec → XXToolBase 适配)
// =====================================================================

PluginTool::PluginTool(
    std::string_view                            name,
    std::weak_ptr<agentxx::agent::AgentContext> agentContext,
    std::shared_ptr<PluginInstance>             instance,
    AgentxxToolSpec                             spec
) :
    XXToolBase(
        name,
        std::move(agentContext),
        /*autoSummaryOutput=*/(spec.flags & AGENTXX_TOOL_FLAG_AUTO_SUMMARY) != 0,
        /*canDelayLoad=*/false, // 插件工具全量注册, 不延迟加载
        /*maxRetry=*/0
    ),
    spec_(spec),
    instance_(instance) {}

neograph::ChatTool PluginTool::get_definition() const {
    neograph::ChatTool def;
    def.name        = spec_.name ? spec_.name : "";
    def.description = spec_.description ? spec_.description : "";
    if (spec_.parameters_json && *spec_.parameters_json) {
        try {
            auto params = neograph::json::parse(spec_.parameters_json);
            if (params.is_object()) {
                def.parameters = std::move(params);
            }
        } catch (const std::exception& e) {
            XX_LOGW("PluginTool `{}`: invalid parameters_json: {}", def.name, e.what());
        }
    }
    if (!def.parameters.is_object()) {
        def.parameters = neograph::json::object();
    }
    return def;
}

asio::awaitable<std::string> PluginTool::execute_async(const neograph::json& arguments) {
    auto inst = instance_.lock();
    if (!inst) {
        throw std::runtime_error("plugin instance released");
    }
    if (!inst->enabled) {
        throw std::runtime_error("plugin disabled");
    }
    if (!spec_.execute) {
        throw std::runtime_error("plugin tool has null execute callback");
    }

    PluginInstance::InflightGuard guard(inst.get());

    // 参数: toolcall 分发路径已注入 thread_id/tool_call_id; call_tool 路径由调用方提供
    std::string argsJson   = arguments.dump();
    std::string threadId   = arguments.value("thread_id", std::string{});
    std::string toolCallId = arguments.value("tool_call_id", std::string{});

    // 取消令牌 (经 Session 按 thread_id 取; 无会话时为空)
    auto agentCtx = agentContext.lock();
    std::shared_ptr<neograph::graph::CancelToken> cancelToken;
    if (agentCtx) {
        cancelToken = agentxx::tools::getSessionCancelToken(agentCtx, arguments);
    }

    auto spec  = spec_; // 拷贝 (跨线程)
    auto instPtr = inst.get();

    // 同步 C 回调经线程池卸载执行 (不阻塞 io 线程); 取消/超时语义经
    // offloadCancellableAsync (CancelToken watcher) + asyncWithTimeout 接入
    // - std::function 而非 lambda: 可拷贝, 避免 move 后复用移后状态
    // - cancelToken 可为 null (offloadCancellableAsync 内部判空跳过 watcher)
    std::function<asio::awaitable<std::string>(std::atomic<bool>&)> run =
        [spec, instPtr, argsJson = std::move(argsJson), threadId = std::move(threadId),
         toolCallId = std::move(toolCallId)](std::atomic<bool>& cancelFlag)
            -> asio::awaitable<std::string> {
        (void)cancelFlag; // 插件回调为黑盒, 无法协作式中止; 等待方取消后线程自然释放
        std::string result;
        char*       err = nullptr;
        char*       out = spec.execute(
            spec.user_data, argsJson.c_str(), threadId.c_str(), toolCallId.c_str(), &err
        );
        if (err) {
            std::string errStr = err;
            instPtr->host.vtable->free(err);
            throw std::runtime_error(
                fmt::format("plugin tool error: {}", agentxx::util::autoTryConvertToUtf8(errStr))
            );
        }
        if (!out) {
            throw std::runtime_error("plugin tool returned null");
        }
        result = out;
        instPtr->host.vtable->free(out);
        co_return result;
    };

    // 协程 lambda 对象 (调用 exec() 才得到 awaitable)
    auto exec = [&]() -> asio::awaitable<std::string> {
        if (agentCtx && agentCtx->blockingPool) {
            co_return co_await agentxx::util::offloadCancellableAsync<std::string>(
                *agentCtx->blockingPool, cancelToken, run
            );
        }
        // 无线程池 (异常环境): 直接执行 (会阻塞 io 线程, 仅兜底)
        auto flag = std::make_shared<std::atomic<bool>>(false);
        co_return co_await run(*flag);
    };

    if (spec.default_timeout_ms > 0) {
        auto timeout = std::chrono::milliseconds{spec.default_timeout_ms};
        co_return co_await agentxx::util::asyncWithTimeout<std::string>(
            [&exec]() -> asio::awaitable<std::string> {
                co_return co_await std::move(exec)();
            },
            timeout,
            []() -> std::string {
                return "[Plugin tool timeout]";
            }
        );
    }
    co_return co_await std::move(exec)();
}

// =====================================================================
// PluginMiddlewareHandle (7 钩子 → C 回调转发)
// =====================================================================

PluginMiddlewareHandle::PluginMiddlewareHandle(
    std::string_view                            name,
    std::weak_ptr<agentxx::agent::AgentContext> agentContext,
    std::shared_ptr<PluginInstance>             instance
) :
    BaseMiddlewareHandle<agentxx::middleware::BaseMiddlewareState>(name, std::move(agentContext)),
    instance_(std::move(instance)) {}

void PluginMiddlewareHandle::setHook(AgentxxHookPoint point, AgentxxHookFn fn, void* user_data) {
    if (point >= 0 && point < AGENTXX_HOOK_COUNT) {
        hooks_[point] = HookEntry{fn, user_data, true};
    }
}

void PluginMiddlewareHandle::clearHook(AgentxxHookPoint point) {
    if (point >= 0 && point < AGENTXX_HOOK_COUNT) {
        hooks_[point] = HookEntry{};
    }
}

asio::awaitable<void>
    PluginMiddlewareHandle::dispatch(AgentxxHookPoint point, const neograph::graph::NodeInput& in) {
    const auto& hook = hooks_[point];
    if (!hook.set || !hook.fn) {
        co_return;
    }
    auto inst = instance_;
    if (!inst || !inst->enabled) {
        co_return;
    }
    PluginInstance::InflightGuard guard(inst.get());

    // 节点输入摘要 JSON (观测用途; out_json 一期预留)
    neograph::json summary = neograph::json::object();
    summary["thread_id"]   = in.ctx.thread_id;
    summary["point"]       = static_cast<int>(point);
    try {
        auto msgs       = in.state.get_messages();
        summary["messages_count"] = msgs.size();
        bool hasToolCalls = false;
        for (const auto& m : msgs) {
            if (!m.tool_calls.empty()) {
                hasToolCalls = true;
                break;
            }
        }
        summary["has_tool_calls"] = hasToolCalls;
    } catch (const std::exception&) {
        summary["messages_count"] = -1;
    }
    auto summaryStr = summary.dump();

    char* out   = nullptr;
    char* err   = nullptr;
    int   rc    = hook.fn(hook.ud, point, summaryStr.c_str(), &out, &err);
    if (out) {
        // 一期忽略 out_json (预留节点输入修改能力)
        inst->host.vtable->free(out);
    }
    if (err) {
        std::string errStr = err;
        inst->host.vtable->free(err);
        XX_LOGW("Plugin hook `{}` point={} error: {}", inst->name, static_cast<int>(point), errStr);
    }
    if (rc != 0) {
        XX_LOGW("Plugin hook `{}` point={} returned {}", inst->name, static_cast<int>(point), rc);
    }
    co_return;
}

#define XX_PLUGIN_HOOK_IMPL(name, point)                                                          \
    asio::awaitable<void> PluginMiddlewareHandle::name(neograph::graph::NodeInput& in) override { \
        co_await dispatch(point, in);                                                             \
    }

asio::awaitable<void> PluginMiddlewareHandle::onAgentcallStartFunc(neograph::graph::NodeInput& in
) {
    co_await dispatch(AGENTXX_HOOK_AGENT_START, in);
}

asio::awaitable<void> PluginMiddlewareHandle::onAgentcallEndFunc(
    const neograph::graph::NodeInput& in, neograph::graph::NodeOutput&
) {
    co_await dispatch(AGENTXX_HOOK_AGENT_END, in);
}

asio::awaitable<void> PluginMiddlewareHandle::onModelcallStartFunc(neograph::graph::NodeInput& in
) {
    co_await dispatch(AGENTXX_HOOK_MODEL_START, in);
}

asio::awaitable<void> PluginMiddlewareHandle::onModelcallRunFunc(neograph::graph::NodeInput& in) {
    co_await dispatch(AGENTXX_HOOK_MODEL_RUN, in);
}

asio::awaitable<void> PluginMiddlewareHandle::onModelcallEndFunc(
    const neograph::graph::NodeInput& in, neograph::graph::NodeOutput&
) {
    co_await dispatch(AGENTXX_HOOK_MODEL_END, in);
}

asio::awaitable<void> PluginMiddlewareHandle::onToolcallStartFunc(neograph::graph::NodeInput& in
) {
    co_await dispatch(AGENTXX_HOOK_TOOL_START, in);
}

asio::awaitable<void> PluginMiddlewareHandle::onToolcallEndFunc(
    const neograph::graph::NodeInput& in, neograph::graph::NodeOutput&
) {
    co_await dispatch(AGENTXX_HOOK_TOOL_END, in);
}

// =====================================================================
// PluginManager
// =====================================================================

PluginManager::PluginManager(std::weak_ptr<agentxx::agent::AgentContext> agentContext) :
    agentContext_(std::move(agentContext)),
    capabilities_(std::make_shared<CapabilityRegistry>()) {
    // 复用 AgentContext::toolRegistry (若已装配), 保证 ToolcallWrapNode/
    // ModelCallWrapNode 查询与插件注册落到同一注册表; 未装配时自建 (测试等场景)
    if (auto ctx = agentContext_.lock()) {
        registry_ = ctx->toolRegistry ? ctx->toolRegistry : std::make_shared<ToolRegistry>();
    } else {
        registry_ = std::make_shared<ToolRegistry>();
    }
}

PluginManager::~PluginManager() {
    // 析构时强制清理全部插件 (卸载顺序: 摘除注册 → dlclose)
    shutdownAll();
}

void PluginManager::shutdownAll() {
    for (auto& [name, inst] : plugins_) {
        (void)name;
        detachAll(inst.get());
        inst->tools.clear();
        // 立即摘除中间件 (进程/上下文销毁场景, 无轮次执行)
        eraseMiddleware(inst.get());
    }
    plugins_.clear();
}

// ==================== host vtable (C ABI) ====================

static PluginInstance* instOf(const AgentxxHost* host) {
    return (host && host->opaque) ? static_cast<PluginInstance*>(host->opaque) : nullptr;
}

static PluginManager* mgrOf(const AgentxxHost* host) {
    auto inst = instOf(host);
    return inst ? inst->manager.lock().get() : nullptr;
}

static void* xx_alloc(size_t size) {
    return ::malloc(size);
}

static void xx_free(void* ptr) {
    ::free(ptr);
}

static char* xx_strdup(const char* s) {
    if (!s) {
        return nullptr;
    }
    size_t  n   = std::strlen(s) + 1;
    char*   p   = static_cast<char*>(::malloc(n));
    if (p) {
        std::memcpy(p, s, n);
    }
    return p;
}

static int xx_register_tool(const AgentxxHost* host, const AgentxxToolSpec* spec) {
    auto mgr = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !spec || !spec->name || !*spec->name) {
        return -1;
    }
    return mgr->registerTool(inst, spec);
}

static int xx_unregister_tool(const AgentxxHost* host, const char* name) {
    auto mgr = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !name) {
        return -1;
    }
    return mgr->unregisterTool(inst, name);
}

static int xx_register_hook(const AgentxxHost* host, AgentxxHookPoint point, AgentxxHookFn fn,
                            void* user_data) {
    auto mgr = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return -1;
    }
    return mgr->registerHook(inst, point, fn, user_data);
}

static int xx_unregister_hook(const AgentxxHost* host, AgentxxHookPoint point, AgentxxHookFn fn,
                              void* user_data) {
    auto mgr = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return -1;
    }
    return mgr->unregisterHook(inst, point, fn, user_data);
}

static AgentxxSubscription* xx_subscribe(const AgentxxHost* host, const char* topic,
                                         void (*handler)(const char* event_json, void* ud),
                                         void* ud) {
    auto mgr = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    return mgr->subscribe(inst, topic, handler, ud);
}

static void xx_unsubscribe(AgentxxSubscription* sub) {
    if (!sub) {
        return;
    }
    auto mgr = sub->inst ? sub->inst->manager.lock().get() : nullptr;
    if (mgr) {
        mgr->unsubscribe(sub);
    } else {
        delete sub;
    }
}

static int xx_publish(const AgentxxHost* host, const char* topic, const char* event_json) {
    auto mgr = mgrOf(host);
    if (!mgr) {
        return -1;
    }
    return mgr->publish(topic, event_json);
}

static int xx_register_capability(const AgentxxHost* host, const char* capability) {
    auto mgr = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !capability) {
        return -1;
    }
    return mgr->registerCapability(inst, capability);
}

static int xx_unregister_capability(const AgentxxHost* host, const char* capability) {
    auto mgr = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !capability) {
        return -1;
    }
    return mgr->unregisterCapability(inst, capability);
}

static int xx_has_capability(const AgentxxHost* host, const char* capability) {
    auto mgr = mgrOf(host);
    if (!mgr || !capability) {
        return 0;
    }
    return mgr->hasCapability(capability) ? 1 : 0;
}

static char* xx_call_tool(const AgentxxHost* host, const char* name, const char* args_json,
                          const char* thread_id, char** error_out) {
    auto mgr = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    return mgr->callTool(inst, name, args_json, thread_id, error_out);
}

static char* xx_get_share_store(const AgentxxHost* host, const char* thread_id, long long id) {
    auto mgr = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    return mgr->getShareStore(inst, thread_id, id);
}

static void xx_emit_message_tip(const AgentxxHost* host, const char* thread_id, const char* text,
                                int level) {
    auto mgr = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return;
    }
    mgr->emitMessageTip(inst, thread_id, text, level);
}

static void xx_log(const AgentxxHost* host, int level, const char* msg) {
    (void)host;
    using agentxx::util::LogLevel;
    LogLevel lv = LogLevel::Info;
    switch (level) {
        case 0:
            lv = LogLevel::Trace;
            break;
        case 1:
            lv = LogLevel::Debug;
            break;
        case 2:
            lv = LogLevel::Info;
            break;
        case 3:
            lv = LogLevel::Warn;
            break;
        case 4:
            lv = LogLevel::Error;
            break;
        default:
            break;
    }
    agentxx::util::xxLogPrint(lv, msg ? msg : "");
}

static const AgentxxHostVtable g_hostVtable = {
    xx_alloc,
    xx_free,
    xx_strdup,
    xx_register_tool,
    xx_unregister_tool,
    xx_register_hook,
    xx_unregister_hook,
    xx_subscribe,
    xx_unsubscribe,
    xx_publish,
    xx_register_capability,
    xx_unregister_capability,
    xx_has_capability,
    xx_call_tool,
    xx_get_share_store,
    xx_emit_message_tip,
    xx_log,
};

// ==================== 工具注册/注销 ====================

int PluginManager::registerTool(PluginInstance* inst, const AgentxxToolSpec* spec) {
    auto shared = inst->self.lock();
    if (!shared) {
        return -1;
    }
    auto tool = std::make_shared<PluginTool>(spec->name, agentContext_, std::move(shared), *spec);
    if (!registry_->registerTool(tool->get_name(), tool)) {
        XX_LOGW("Plugin `{}` register tool `{}` failed (conflict?)", inst->name, spec->name);
        return -1;
    }
    inst->toolNames.push_back(tool->get_name());
    inst->tools.push_back(std::move(tool));
    XX_LOGI("Plugin `{}` registered tool `{}`", inst->name, spec->name);
    return 0;
}

int PluginManager::unregisterTool(PluginInstance* inst, const char* name) {
    if (!name) {
        return -1;
    }
    auto it = std::find(inst->toolNames.begin(), inst->toolNames.end(), std::string{name});
    if (it == inst->toolNames.end()) {
        XX_LOGW("Plugin `{}` unregister tool `{}` not owned by this plugin", inst->name, name);
        return -1;
    }
    inst->toolNames.erase(it);
    registry_->unregisterTool(name);
    XX_LOGI("Plugin `{}` unregistered tool `{}`", inst->name, name);
    return 0;
}

// ==================== 钩子注册/注销 ====================

int PluginManager::registerHook(PluginInstance* inst, AgentxxHookPoint point, AgentxxHookFn fn,
                                void* ud) {
    if (!inst || !fn || point < 0 || point >= AGENTXX_HOOK_COUNT) {
        return -1;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        return -1;
    }
    // 懒创建中间件句柄并挂到 handles 栈 (io 线程 push_back 安全)
    if (!inst->middleware) {
        auto shared = inst->self.lock();
        if (!shared) {
            return -1;
        }
        inst->middleware = std::make_shared<PluginMiddlewareHandle>(
            fmt::format("{}_middleware", inst->name), agentContext_, std::move(shared)
        );
        inst->middleware->disabled = !inst->enabled;
        ctx->middlewareHandleContext->handles.push_back(inst->middleware);
    }
    inst->middleware->setHook(point, fn, ud);
    inst->hookPoints.push_back(point);
    XX_LOGI("Plugin `{}` registered hook point={}", inst->name, static_cast<int>(point));
    return 0;
}

int PluginManager::unregisterHook(PluginInstance* inst, AgentxxHookPoint point, AgentxxHookFn fn,
                                  void* ud) {
    (void)fn;
    (void)ud;
    if (!inst || point < 0 || point >= AGENTXX_HOOK_COUNT) {
        return -1;
    }
    if (inst->middleware) {
        inst->middleware->clearHook(point);
    }
    inst->hookPoints.erase(
        std::remove(inst->hookPoints.begin(), inst->hookPoints.end(), point),
        inst->hookPoints.end()
    );
    return 0;
}

// ==================== 事件订阅/发布 ====================

AgentxxSubscription*
    PluginManager::subscribe(PluginInstance* inst, const char* topic,
                             void (*handler)(const char* event_json, void* ud), void* ud) {
    if (!inst || !topic || !*topic || !handler) {
        return nullptr;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->bus) {
        XX_LOGW("Plugin `{}` subscribe: bus not ready", inst->name);
        return nullptr;
    }
    auto fullTopic = fmt::format("plugin.{}", topic);
    auto sub       = new AgentxxSubscription{
        ctx->bus, fullTopic, 0, inst, handler, ud,
    };
    sub->subscriptionId = ctx->bus->get<std::string>(fullTopic).subscribe(
        [sub](const std::string& data) -> asio::awaitable<void> {
            if (!sub->inst || !sub->inst->enabled) {
                co_return;
            }
            PluginInstance::InflightGuard guard(sub->inst);
            if (sub->handler) {
                sub->handler(data.c_str(), sub->ud);
            }
            co_return;
        }
    );
    inst->subscriptions.push_back(sub);
    XX_LOGI("Plugin `{}` subscribed `{}`", inst->name, fullTopic);
    return sub;
}

void PluginManager::unsubscribe(AgentxxSubscription* sub) {
    if (!sub) {
        return;
    }
    if (sub->bus) {
        try {
            sub->bus->get<std::string>(sub->topic).unsubscribe(sub->subscriptionId);
        } catch (const std::exception& e) {
            XX_LOGW("Plugin unsubscribe `{}` error: {}", sub->topic, e.what());
        }
    }
    if (sub->inst) {
        auto& subs = sub->inst->subscriptions;
        subs.erase(std::remove(subs.begin(), subs.end(), sub), subs.end());
    }
    delete sub;
}

int PluginManager::publish(const char* topic, const char* event_json) {
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->bus) {
        return -1;
    }
    auto fullTopic = fmt::format("plugin.{}", topic ? topic : "");
    auto data      = std::string{event_json ? event_json : ""};
    auto bus       = ctx->bus;
    asio::co_spawn(
        bus->executor(),
        [bus, fullTopic = std::move(fullTopic), data = std::move(data)]() -> asio::awaitable<void> {
            co_await bus->publish<std::string>(fullTopic, data);
        },
        asio::detached
    );
    return 0;
}

// ==================== 能力注册 ====================

int PluginManager::registerCapability(PluginInstance* inst, const char* capability) {
    if (!inst || !capability || !*capability) {
        return -1;
    }
    if (!capabilities_->registerCapability(capability, inst->name)) {
        return -1;
    }
    inst->capabilities.push_back(capability);
    return 0;
}

int PluginManager::unregisterCapability(PluginInstance* inst, const char* capability) {
    if (!inst || !capability) {
        return -1;
    }
    auto it = std::find(inst->capabilities.begin(), inst->capabilities.end(), std::string{capability}
    );
    if (it == inst->capabilities.end()) {
        return -1;
    }
    inst->capabilities.erase(it);
    capabilities_->unregisterCapability(capability, inst->name);
    return 0;
}

int PluginManager::hasCapability(const char* capability) const {
    return capability && capabilities_->has(capability) ? 1 : 0;
}

// ==================== 会话/上下文访问 ====================

char* PluginManager::callTool(PluginInstance* inst, const char* name, const char* args_json,
                              const char* thread_id, char** error_out) {
    if (!inst || !name) {
        return nullptr;
    }
    auto setErr = [&](const std::string& msg) {
        if (error_out) {
            *error_out = inst->host.vtable->strdup(msg.c_str());
        }
    };
    // 仅允许调用插件注册的工具 (不暴露宿主内置工具)
    auto tool = registry_->find(name);
    if (!tool) {
        setErr(fmt::format("plugin call_tool: tool `{}` not found", name));
        return nullptr;
    }
    auto pluginTool = std::dynamic_pointer_cast<PluginTool>(tool);
    if (!pluginTool) {
        setErr(fmt::format("plugin call_tool: tool `{}` is not a plugin tool", name));
        return nullptr;
    }
    PluginInstance::InflightGuard guard(inst);

    neograph::json args = neograph::json::object();
    if (args_json && *args_json) {
        try {
            auto parsed = neograph::json::parse(args_json);
            if (parsed.is_object()) {
                args = std::move(parsed);
            }
        } catch (const std::exception& e) {
            setErr(fmt::format("plugin call_tool: invalid args_json: {}", e.what()));
            return nullptr;
        }
    }
    args["thread_id"]    = thread_id ? thread_id : "";
    args["tool_call_id"] = fmt::format("plugin_call_{}", ++g_pluginCallSeq);

    const auto& spec = pluginTool->spec();
    char*       err  = nullptr;
    char*       out  = spec.execute(
        spec.user_data, args.dump().c_str(), thread_id ? thread_id : "", "", &err
    );
    if (err) {
        std::string errStr = err;
        inst->host.vtable->free(err);
        setErr(agentxx::util::autoTryConvertToUtf8(errStr));
        return nullptr;
    }
    if (!out) {
        setErr("plugin call_tool: null result");
        return nullptr;
    }
    return out; // 交由调用方 free
}

char* PluginManager::getShareStore(PluginInstance* inst, const char* thread_id, long long id) {
    if (!inst || !thread_id) {
        return nullptr;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        return nullptr;
    }
    auto value = ctx->middlewareHandleContext->getShareStoreItemValue(thread_id, static_cast<size_t>(id));
    if (!value) {
        return nullptr;
    }
    return inst->host.vtable->strdup(value->c_str());
}

void PluginManager::emitMessageTip(PluginInstance* inst, const char* thread_id, const char* text,
                                   int level) {
    if (!inst || !thread_id || !text) {
        return;
    }
    auto ctx = agentContext_.lock();
    if (!ctx) {
        return;
    }
    auto session = ctx->getSession(thread_id);
    if (!session || !session->io) {
        return;
    }
    agentxx::agent::Delta delta;
    delta.type    = agentxx::agent::Delta::Type::MessageUITip;
    delta.text    = text;
    delta.tipType = level >= 2 ? agentxx::agent::Delta::TipType::Error
                               : (level == 1 ? agentxx::agent::Delta::TipType::Warning
                                             : agentxx::agent::Delta::TipType::Info);
    delta.seq     = session->nextDeltaSeq();
    session->io->sendToPeer(delta);
}

// ==================== 生命周期 ====================

/// 从库文件名推断插件名 (libfoo.so → foo; foo.dll → foo)
static std::string pluginNameFromPath(const std::string& path) {
    auto base = std::filesystem::path(path).filename().string();
    // 去 lib 前缀
    if (base.size() > 3 && base.compare(0, 3, "lib") == 0) {
        base.erase(0, 3);
    }
    // 去扩展名
    auto dot = base.find_last_of('.');
    if (dot != std::string::npos) {
        base.erase(dot);
    }
    return base;
}

asio::awaitable<std::shared_ptr<PluginInstance>> PluginManager::loadNativeAsync(std::string path) {
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->blockingPool) {
        XX_LOGE("PluginManager: agent context not ready");
        co_return nullptr;
    }

    // dlopen 卸载到线程池 (避免阻塞 io 线程)
    std::string dlErr;
    void* handle = co_await agentxx::util::offloadAsync<void*>(
        *ctx->blockingPool,
        [path, &dlErr]() -> asio::awaitable<void*> {
            co_return NativeLoader::open(path, dlErr);
        }
    );
    if (!handle) {
        XX_LOGE("Plugin load failed: {}: {}", path, dlErr);
        co_return nullptr;
    }

    // 元信息 (可选符号)
    std::string name;
    std::string version;
    std::string desc;
    std::string err;
    if (auto getInfo = reinterpret_cast<AgentxxPluginGetInfoFn>(
            NativeLoader::sym(handle, AGENTXX_PLUGIN_SYMBOL_GET_INFO, err)
        )) {
        auto info = getInfo();
        if (info) {
            if (info->api_version != AGENTXX_PLUGIN_API_VERSION) {
                XX_LOGE(
                    "Plugin `{}` api_version {} mismatch (host expects {})", path,
                    info->api_version, AGENTXX_PLUGIN_API_VERSION
                );
                NativeLoader::close(handle);
                co_return nullptr;
            }
            name    = info->name ? info->name : "";
            version = info->version ? info->version : "";
            desc    = info->description ? info->description : "";
        }
    }
    if (name.empty()) {
        name = pluginNameFromPath(path);
    }
    if (plugins_.contains(name)) {
        XX_LOGE("Plugin `{}` already loaded", name);
        NativeLoader::close(handle);
        co_return nullptr;
    }

    auto entry = reinterpret_cast<AgentxxPluginEntryFn>(
        NativeLoader::sym(handle, AGENTXX_PLUGIN_SYMBOL_ENTRY, err)
    );
    if (!entry) {
        XX_LOGE("Plugin `{}` missing entry symbol `{}`: {}", path, AGENTXX_PLUGIN_SYMBOL_ENTRY,
                err);
        NativeLoader::close(handle);
        co_return nullptr;
    }

    auto inst          = std::make_shared<PluginInstance>(name);
    inst->version      = std::move(version);
    inst->description  = std::move(desc);
    inst->path         = path;
    inst->dlHandle     = handle;
    inst->self         = inst;
    inst->manager      = shared_from_this();
    inst->host.vtable  = &g_hostVtable;
    inst->host.opaque  = inst.get();

    plugins_[name] = inst;

    // entry 在 io 线程同步调用 (插件注册动作须在 io 线程)
    int rc = entry(&inst->host, &inst->pluginCtx);
    if (rc != 0) {
        XX_LOGE("Plugin `{}` entry returned {}", name, rc);
        detachAll(inst.get());
        plugins_.erase(name);
        co_return nullptr;
    }

    XX_LOGI(
        "Plugin loaded: {} v{} ({} tools, {} hooks, {} capabilities)", name, inst->version,
        inst->toolNames.size(), inst->hookPoints.size(), inst->capabilities.size()
    );
    co_return inst;
}

asio::awaitable<void> PluginManager::waitInflightZero(
    const std::shared_ptr<PluginInstance>& inst
) {
    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer timer(ex);
    while (inst->inflight.load(std::memory_order_acquire) > 0) {
        timer.expires_after(std::chrono::milliseconds(10));
        co_await timer.async_wait(asio::use_awaitable);
    }
}

void PluginManager::detachAll(PluginInstance* inst) {
    if (!inst) {
        return;
    }
    inst->enabled = false;
    // 工具: 从注册表摘除 (对象保留于 inst->tools, enable 可恢复)
    for (const auto& tool : inst->tools) {
        registry_->unregisterTool(tool->get_name());
    }
    inst->toolNames.clear();
    // 钩子: 停用中间件 (disabled 位) + 记录待轮末摘除
    if (inst->middleware) {
        inst->middleware->disabled = true;
        for (int p = 0; p < AGENTXX_HOOK_COUNT; ++p) {
            inst->middleware->clearHook(static_cast<AgentxxHookPoint>(p));
        }
        if (std::find(pendingCleanup_.begin(), pendingCleanup_.end(), inst->name)
            == pendingCleanup_.end()) {
            pendingCleanup_.push_back(inst->name);
        }
    }
    inst->hookPoints.clear();
    // 订阅: 全部退订
    for (auto* sub : inst->subscriptions) {
        unsubscribe(sub);
    }
    inst->subscriptions.clear();
    // 能力
    for (const auto& cap : inst->capabilities) {
        capabilities_->unregisterCapability(cap, inst->name);
    }
    inst->capabilities.clear();
}

void PluginManager::eraseMiddleware(const PluginInstance* inst) {
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        return;
    }
    auto& handles = ctx->middlewareHandleContext->handles;
    handles.erase(
        std::remove_if(
            handles.begin(),
            handles.end(),
            [inst](const std::shared_ptr<agentxx::middleware::BaseMiddlewareHandleInterface>& h) {
                return h.get() == inst->middleware.get();
            }
        ),
        handles.end()
    );
    // 摘除后清掉指针 (防误用)
    const_cast<PluginInstance*>(inst)->middleware = nullptr;
}

void PluginManager::flushPendingCleanup() {
    if (pendingCleanup_.empty()) {
        return;
    }
    auto pending = std::move(pendingCleanup_);
    pendingCleanup_.clear();
    for (const auto& name : pending) {
        auto inst = find(name);
        if (inst && inst->middleware) {
            eraseMiddleware(inst.get());
            XX_LOGI("Plugin `{}` middleware removed from stack", name);
        }
    }
}

asio::awaitable<bool> PluginManager::unloadAsync(std::string_view name) {
    auto it = plugins_.find(std::string{name});
    if (it == plugins_.end()) {
        XX_LOGW("Plugin unload: `{}` not loaded", name);
        co_return false;
    }
    auto inst = it->second;
    if (inst->unloadRequested) {
        co_return false;
    }
    inst->unloadRequested = true;
    detachAll(inst.get());
    // 卸载后不再恢复: 释放插件工具对象 (enable 仅对未卸载插件有意义)
    inst->tools.clear();

    // 等全部在途回调完成 (工具执行/钩子/事件 handler) 后才允许 dlclose
    co_await waitInflightZero(inst);

    // 等待进行中的轮次结束 (避免 erase 破坏执行中的 handles 下标遍历;
    // 禁用路径走轮末 flushPendingCleanup, 卸载路径必须彻底摘除以断开
    // 中间件↔实例循环引用)
    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer timer(ex);
    while (runningTurns_ > 0) {
        timer.expires_after(std::chrono::milliseconds(10));
        co_await timer.async_wait(asio::use_awaitable);
    }
    eraseMiddleware(inst.get());
    // 从待轮末清理列表移除 (已立即摘除)
    pendingCleanup_.erase(
        std::remove(pendingCleanup_.begin(), pendingCleanup_.end(), std::string{name}),
        pendingCleanup_.end()
    );

    // 插件 unload 回调 (业务清理; 宿主已自动反注册全部残留)
    if (inst->dlHandle) {
        std::string err;
        auto fn = reinterpret_cast<AgentxxPluginUnloadFn>(
            NativeLoader::sym(inst->dlHandle, AGENTXX_PLUGIN_SYMBOL_UNLOAD, err)
        );
        if (fn) {
            fn(inst->pluginCtx);
        }
    }

    plugins_.erase(it); // 析构 → dlclose (inst 局部 shared_ptr 保活到函数结束)
    XX_LOGI("Plugin unloaded: {}", inst->name);
    co_return true;
}

void PluginManager::disable(std::string_view name) {
    auto inst = find(name);
    if (!inst || !inst->enabled) {
        return;
    }
    inst->enabled = false;
    // 工具摘除 (对象保留, enable 恢复)
    for (const auto& tool : inst->tools) {
        registry_->unregisterTool(tool->get_name());
    }
    inst->toolNames.clear();
    // 钩子停用: disabled 位 → 下一节点执行即跳过; 轮末 flushPendingCleanup 摘除
    if (inst->middleware) {
        inst->middleware->disabled = true;
        if (std::find(pendingCleanup_.begin(), pendingCleanup_.end(), inst->name)
            == pendingCleanup_.end()) {
            pendingCleanup_.push_back(inst->name);
        }
    }
    XX_LOGI("Plugin disabled: {}", inst->name);
}

void PluginManager::enable(std::string_view name) {
    auto inst = find(name);
    if (!inst) {
        return;
    }
    inst->enabled = true;
    // 重新注册工具
    for (auto& tool : inst->tools) {
        if (!registry_->contains(tool->get_name())) {
            if (registry_->registerTool(tool->get_name(), tool)) {
                inst->toolNames.push_back(tool->get_name());
            }
        }
    }
    // 钩子恢复: 若 middleware 仍在 handles 中 (未被摘除), 恢复 disabled
    if (inst->middleware) {
        inst->middleware->disabled = false;
        pendingCleanup_.erase(
            std::remove(pendingCleanup_.begin(), pendingCleanup_.end(), std::string{name}),
            pendingCleanup_.end()
        );
    }
    // 能力重新声明
    for (const auto& cap : inst->capabilities) {
        capabilities_->registerCapability(cap, inst->name);
    }
    XX_LOGI("Plugin enabled: {}", inst->name);
}

asio::awaitable<void> PluginManager::loadConfiguredPlugins(
    const std::vector<agentxx::agent::PluginConfig>& plugins
) {
    for (const auto& cfg : plugins) {
        if (!cfg.enabled) {
            continue;
        }
        co_await loadNativeAsync(cfg.path);
    }
}

std::vector<PluginManager::PluginListView> PluginManager::list() const {
    std::vector<PluginListView> out;
    out.reserve(plugins_.size());
    for (const auto& [name, inst] : plugins_) {
        out.push_back(PluginListView{
            name,
            inst->version,
            inst->description,
            inst->path,
            inst->enabled,
            inst->inflight.load(std::memory_order_acquire),
            inst->toolNames,
            inst->capabilities,
        });
    }
    return out;
}

std::shared_ptr<PluginInstance> PluginManager::find(std::string_view name) const {
    auto it = plugins_.find(std::string{name});
    if (it == plugins_.end()) {
        return nullptr;
    }
    return it->second;
}

} // namespace plugin
} // namespace agentxx
