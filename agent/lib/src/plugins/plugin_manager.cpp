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
#include "asio/post.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "neograph/graph/cancel.h"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
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

bool CapabilityRegistry::registerCapability(std::string_view name, std::string_view provider,
                                            AgentxxCapabilityInvokeFn invoke, void* ctx) {
    if (name.empty()) {
        return false;
    }
    // 同名能力重复注册: 拒绝 (能力委派需唯一 provider)
    if (caps_.contains(std::string{name})) {
        XX_LOGW("CapabilityRegistry: capability `{}` already registered by `{}`", name,
                caps_.at(std::string{name}).provider);
        return false;
    }
    caps_[std::string{name}] = Entry{std::string{provider}, invoke, ctx};
    XX_LOGI("CapabilityRegistry: `{}` registered by plugin `{}`", name, provider);
    return true;
}

bool CapabilityRegistry::unregisterCapability(std::string_view name, std::string_view provider) {
    auto it = caps_.find(std::string{name});
    if (it == caps_.end()) {
        return false;
    }
    if (it->second.provider != provider) {
        XX_LOGW("CapabilityRegistry: capability `{}` owned by `{}`, cannot unregister by `{}`",
                name, it->second.provider, provider);
        return false;
    }
    caps_.erase(it);
    return true;
}

bool CapabilityRegistry::has(std::string_view name) const {
    return caps_.contains(std::string{name});
}

const CapabilityRegistry::Entry* CapabilityRegistry::get(std::string_view name) const {
    auto it = caps_.find(std::string{name});
    if (it == caps_.end()) {
        return nullptr;
    }
    return &it->second;
}

std::string CapabilityRegistry::providerOf(std::string_view name) const {
    auto it = caps_.find(std::string{name});
    if (it == caps_.end()) {
        return {};
    }
    return it->second.provider;
}

std::vector<std::string> CapabilityRegistry::names() const {
    std::vector<std::string> out;
    out.reserve(caps_.size());
    for (const auto& [name, entry] : caps_) {
        (void)entry;
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
    // 全部插件统一处理: 摘除注册 → dlclose
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

/// 在 io 线程执行并同步等待结果 (调用方为 io 线程时直接执行)
/// - 供 vtable 的 io 线程约束操作跨线程调用 (JS 线程/宿主线程池) 使用;
///   调用方线程阻塞等待, io 线程为事件循环 (挂起而非忙等), 无死锁风险
template<typename T>
static T ioCallSync(PluginManager* mgr, std::function<T()> fn) {
    if (!mgr) {
        throw std::runtime_error("plugin manager released");
    }
    if (mgr->isIoThread()) {
        return fn();
    }
    std::promise<T> p;
    auto            fut = p.get_future();
    mgr->postToIo([&p, fn = std::move(fn)]() mutable {
        try {
            p.set_value(fn());
        } catch (...) {
            p.set_exception(std::current_exception());
        }
    });
    return fut.get();
}

/// void 特化
static void ioCallSyncVoid(PluginManager* mgr, std::function<void()> fn) {
    if (!mgr) {
        return;
    }
    if (mgr->isIoThread()) {
        fn();
        return;
    }
    std::promise<void> p;
    auto               fut = p.get_future();
    mgr->postToIo([&p, fn = std::move(fn)]() mutable {
        try {
            fn();
            p.set_value();
        } catch (...) {
            p.set_exception(std::current_exception());
        }
    });
    fut.get();
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
    // io 线程约束操作: 非 io 线程调用 (JS 线程等) 经 post 同步等待
    auto mgrPtr  = mgr;
    auto instPtr = inst;
    AgentxxToolSpec specCopy = *spec;
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, specCopy]() {
        return mgrPtr->registerTool(instPtr, &specCopy);
    });
}

static int xx_unregister_tool(const AgentxxHost* host, const char* name) {
    auto mgr = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !name) {
        return -1;
    }
    auto mgrPtr = mgr;
    auto instPtr = inst;
    std::string toolName{name};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, toolName]() {
        return mgrPtr->unregisterTool(instPtr, toolName.c_str());
    });
}

static int xx_register_hook(const AgentxxHost* host, AgentxxHookPoint point, AgentxxHookFn fn,
                            void* user_data) {
    auto mgr = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return -1;
    }
    auto mgrPtr = mgr;
    auto instPtr = inst;
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, point, fn, user_data]() {
        return mgrPtr->registerHook(instPtr, point, fn, user_data);
    });
}

static int xx_unregister_hook(const AgentxxHost* host, AgentxxHookPoint point, AgentxxHookFn fn,
                              void* user_data) {
    auto mgr = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return -1;
    }
    auto mgrPtr = mgr;
    auto instPtr = inst;
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, point, fn, user_data]() {
        return mgrPtr->unregisterHook(instPtr, point, fn, user_data);
    });
}

static AgentxxSubscription* xx_subscribe(const AgentxxHost* host, const char* topic,
                                         void (*handler)(const char* event_json, void* ud),
                                         void* ud) {
    auto mgr = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    auto mgrPtr = mgr;
    auto instPtr = inst;
    std::string topicStr{topic ? topic : ""};
    return ioCallSync<AgentxxSubscription*>(mgrPtr, [mgrPtr, instPtr, topicStr, handler, ud]() {
        return mgrPtr->subscribe(instPtr, topicStr.c_str(), handler, ud);
    });
}

static void xx_unsubscribe(AgentxxSubscription* sub) {
    if (!sub) {
        return;
    }
    auto mgr = sub->inst ? sub->inst->manager.lock().get() : nullptr;
    if (mgr) {
        ioCallSyncVoid(mgr, [mgr, sub]() { mgr->unsubscribe(sub); });
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
    auto mgrPtr = mgr;
    auto instPtr = inst;
    std::string cap{capability};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, cap]() {
        return mgrPtr->registerCapability(instPtr, cap.c_str());
    });
}

static int xx_unregister_capability(const AgentxxHost* host, const char* capability) {
    auto mgr = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !capability) {
        return -1;
    }
    auto mgrPtr = mgr;
    auto instPtr = inst;
    std::string cap{capability};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, cap]() {
        return mgrPtr->unregisterCapability(instPtr, cap.c_str());
    });
}

static int xx_has_capability(const AgentxxHost* host, const char* capability) {
    auto mgr = mgrOf(host);
    if (!mgr || !capability) {
        return 0;
    }
    return mgr->hasCapability(capability) ? 1 : 0;
}

static int xx_register_capability_ex(const AgentxxHost* host, const char* capability,
                                     AgentxxCapabilityInvokeFn invoke, void* ctx) {
    auto mgr = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !capability || !*capability) {
        return -1;
    }
    auto mgrPtr = mgr;
    auto instPtr = inst;
    std::string cap{capability};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, cap, invoke, ctx]() {
        return mgrPtr->registerCapabilityEx(instPtr, cap.c_str(), invoke, ctx);
    });
}

static char* xx_invoke_capability(const AgentxxHost* host, const char* capability,
                                  const char* method, const char* args_json, char** error_out) {
    auto mgr = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !capability || !method) {
        return nullptr;
    }
    std::string cap{capability};
    std::string m{method};
    std::string args{args_json ? args_json : "{}"};
    // 查表在 io 线程 (invokeCapability 内部), 提供者回调在调用方线程执行
    return mgr->invokeCapability(inst, cap.c_str(), m.c_str(), args.c_str(), error_out);
}

static char* xx_call_tool(const AgentxxHost* host, const char* name, const char* args_json,
                          const char* thread_id, char** error_out) {
    auto mgr = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    // registry 查表为 io 线程数据结构: 整体在 io 线程执行 (含 spec.execute 同步
    // 回调; call_tool 为低频互调路径, 阻塞 io 线程可接受; 若目标是本引擎的
    // JS 工具, JS 侧桥已内联处理, 不会走到这里)
    auto mgrPtr = mgr;
    auto instPtr = inst;
    std::string toolName{name ? name : ""};
    std::string args{args_json ? args_json : ""};
    std::string tid{thread_id ? thread_id : ""};
    // error_out 由调用方栈持有, 调用方在 fut.get() 等待期间有效;
    // 内存可见性由 future 保证
    return ioCallSync<char*>(mgrPtr, [mgrPtr, instPtr, toolName, args, tid, error_out]() {
        return mgrPtr->callTool(instPtr, toolName.c_str(), args.c_str(), tid.c_str(), error_out);
    });
}

static char* xx_list_plugins(const AgentxxHost* host) {
    auto mgr = mgrOf(host);
    if (!mgr) {
        return nullptr;
    }
    // plugins_ 为 io 线程数据结构: 整体在 io 线程执行; 字符串跨边界经 host->alloc
    auto mgrPtr = mgr;
    auto json   = ioCallSync<std::string>(mgrPtr, [mgrPtr]() { return mgrPtr->listPluginsJson(); });
    return host->vtable->strdup(json.c_str());
}

static char* xx_get_plugin(const AgentxxHost* host, const char* name) {
    auto mgr = mgrOf(host);
    if (!mgr || !name) {
        return nullptr;
    }
    auto mgrPtr = mgr;
    std::string pluginName{name};
    auto json = ioCallSync<std::string>(mgrPtr, [mgrPtr, pluginName]() {
        return mgrPtr->getPluginJson(pluginName);
    });
    if (json.empty()) {
        return nullptr; // 未安装
    }
    return host->vtable->strdup(json.c_str());
}

static char* xx_get_own_info(const AgentxxHost* host) {
    auto mgr = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    auto mgrPtr = mgr;
    std::string ownName = inst->name;
    auto json = ioCallSync<std::string>(mgrPtr, [mgrPtr, ownName]() {
        return mgrPtr->getOwnPluginJson(ownName);
    });
    if (json.empty()) {
        return nullptr;
    }
    return host->vtable->strdup(json.c_str());
}

static char* xx_get_share_store(const AgentxxHost* host, const char* thread_id, long long id) {
    auto mgr = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    // shareStore 仅 io 线程访问 (无锁模型): 跨线程经 post 同步等待
    auto mgrPtr = mgr;
    auto instPtr = inst;
    std::string tid{thread_id ? thread_id : ""};
    return ioCallSync<char*>(mgrPtr, [mgrPtr, instPtr, tid, id]() {
        return mgrPtr->getShareStore(instPtr, tid.c_str(), id);
    });
}

static void xx_emit_message_tip(const AgentxxHost* host, const char* thread_id, const char* text,
                                int level) {
    auto mgr = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return;
    }
    auto mgrPtr = mgr;
    auto instPtr = inst;
    std::string tid{thread_id ? thread_id : ""};
    std::string msg{text ? text : ""};
    ioCallSyncVoid(mgrPtr, [mgrPtr, instPtr, tid, msg, level]() {
        mgrPtr->emitMessageTip(instPtr, tid.c_str(), msg.c_str(), level);
    });
}

static int xx_is_io_thread(const AgentxxHost* host) {
    auto mgr = mgrOf(host);
    return (mgr && mgr->isIoThread()) ? 1 : 0;
}

static void xx_post_to_io(const AgentxxHost* host, void (*fn)(void* ud), void* ud) {
    auto mgr = mgrOf(host);
    if (!mgr || !fn) {
        return;
    }
    mgr->postToIo([fn, ud]() { fn(ud); });
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
    xx_register_capability_ex,
    xx_unregister_capability,
    xx_has_capability,
    xx_invoke_capability,
    xx_is_io_thread,
    xx_post_to_io,
    xx_call_tool,
    xx_list_plugins,
    xx_get_plugin,
    xx_get_own_info,
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

int PluginManager::registerCapabilityEx(PluginInstance* inst, const char* capability,
                                        AgentxxCapabilityInvokeFn invoke, void* ctx) {
    if (!inst || !capability || !*capability || !invoke) {
        return -1;
    }
    if (!capabilities_->registerCapability(capability, inst->name, invoke, ctx)) {
        return -1;
    }
    inst->capabilities.push_back(capability);
    return 0;
}

char* PluginManager::invokeCapability(PluginInstance* caller, const char* capability,
                                      const char* method, const char* args_json, char** error_out) {
    auto setErr = [&](const std::string& msg) {
        if (error_out && caller) {
            *error_out = caller->host.vtable->strdup(msg.c_str());
        }
    };
    // 1. io 线程查表并拷贝 (短临界区)
    CapabilityRegistry::Entry entry;
    bool                       found = false;
    if (isIoThread()) {
        if (const auto* e = capabilities_->get(capability)) {
            entry = *e;
            found = true;
        }
    } else {
        found = ioCallSync<bool>(this, [this, capability, &entry]() {
            const auto* e = capabilities_->get(capability);
            if (!e) {
                return false;
            }
            entry = *e;
            return true;
        });
    }
    if (!found) {
        setErr(fmt::format("invoke_capability: capability `{}` not registered", capability));
        return nullptr;
    }
    if (!entry.invoke) {
        setErr(fmt::format("invoke_capability: capability `{}` has no method handler", capability));
        return nullptr;
    }
    // 2. 提供者回调在【调用方线程】执行 (关键: 引擎的 load 会阻塞等待其引擎
    //    线程, 引擎线程内脚本注册回调又要回 io 线程 —— 若回调在 io 线程执行
    //    则 io↔引擎互等死锁; 调用方线程执行则 io 线程保持空闲可服务注册回调)
    return entry.invoke(entry.ctx, caller ? &caller->host : nullptr, method, args_json, error_out);
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

    // ---- 依赖图级联: 先卸载必选依赖本插件的插件 (先子后父) ----
    // - 例如卸载 JS 引擎插件 → 先卸载全部 depends 它的脚本插件,
    //   保证引擎 dlclose 前无任何脚本插件残留 (binding 永不悬垂)
    for (const auto& dep : reverseRequiredDeps(inst->name, /*onlyEnabled=*/true)) {
        XX_LOGI("Unload `{}` cascades unload of dependent plugin `{}`", inst->name, dep);
        co_await unloadAsync(dep);
    }
    // 可选依赖者: 仅警告 (插件运行时用互查 API 感知后自行降级)

    // ---- 摘除全部注册 (插件内部委托的脚本运行时由插件自身的 unload 回调清理) ----
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

    // 插件 unload 回调 (业务清理; 宿主已自动反注册全部残留; 脚本插件无 dlHandle)
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

/// 收集必选依赖 target 的插件名 (io 线程)
std::vector<std::string> PluginManager::reverseRequiredDeps(const std::string& target,
                                                            bool               onlyEnabled) const {
    std::vector<std::string> out;
    for (const auto& [name, inst] : plugins_) {
        if (name == target || (onlyEnabled && !inst->enabled)) {
            continue;
        }
        if (std::find(inst->depends.begin(), inst->depends.end(), target) != inst->depends.end()) {
            out.push_back(name);
        }
    }
    return out;
}

void PluginManager::disable(std::string_view name) {
    auto inst = find(name);
    if (!inst || !inst->enabled) {
        return;
    }
    // 依赖图级联: 先禁用必选依赖本插件的插件 (脚本插件随引擎一起停用)
    for (const auto& dep : reverseRequiredDeps(inst->name, /*onlyEnabled=*/true)) {
        XX_LOGI("Disable `{}` cascades disable of dependent plugin `{}`", inst->name, dep);
        disable(dep);
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
    // 依赖图级联: 再启用必选依赖本插件的插件 (含被级联禁用的)
    for (const auto& dep : reverseRequiredDeps(inst->name, /*onlyEnabled=*/false)) {
        auto depInst = find(dep);
        if (depInst && !depInst->enabled) {
            XX_LOGI("Enable `{}` cascades enable of dependent plugin `{}`", inst->name, dep);
            enable(dep);
        }
    }
    XX_LOGI("Plugin enabled: {}", inst->name);
}

/// 从插件目录 plugin.yaml 解析清单 (name/entry/depends/optional_depends/enabled)
/// - 返回 false 表示解析失败 (记录日志)
static bool parsePluginManifest(
    const std::filesystem::path& dir,
    std::string&                 name,
    std::string&                 entry,
    std::vector<std::string>&    depends,
    std::vector<std::string>&    optionalDepends,
    bool&                        enabled
) {
    auto yamlPath = dir / "plugin.yaml";
    std::error_code ec;
    if (!std::filesystem::exists(yamlPath, ec)) {
        XX_LOGW("Plugin dir `{}` has no plugin.yaml", dir.string());
        return false;
    }
    try {
        auto node = YAML::LoadFile(yamlPath.string());
        name    = node["name"] ? node["name"].as<std::string>() : std::string{};
        entry   = node["entry"] ? node["entry"].as<std::string>() : std::string{};
        if (node["depends"] && node["depends"].IsSequence()) {
            for (const auto& d : node["depends"]) {
                if (d.IsScalar()) {
                    depends.push_back(d.as<std::string>());
                }
            }
        }
        if (node["optional_depends"] && node["optional_depends"].IsSequence()) {
            for (const auto& d : node["optional_depends"]) {
                if (d.IsScalar()) {
                    optionalDepends.push_back(d.as<std::string>());
                }
            }
        }
        if (node["enabled"] && node["enabled"].IsScalar()) {
            try {
                enabled = node["enabled"].as<bool>();
            } catch (const std::exception&) {
                auto s = node["enabled"].as<std::string>();
                enabled = !(s == "false" || s == "0" || s == "no");
            }
        }
    } catch (const std::exception& e) {
        XX_LOGE("Parse plugin manifest `{}` failed: {}", yamlPath.string(), e.what());
        return false;
    }
    if (name.empty() || entry.empty()) {
        XX_LOGE("Plugin manifest `{}` invalid: name/entry required", yamlPath.string());
        return false;
    }
    return true;
}

bool PluginManager::hasDependencyCycle(const std::string& name,
                                       std::vector<std::string>& visiting) const {
    // 当前 DFS 链上再次遇到 → 环
    if (std::find(visiting.begin(), visiting.end(), name) != visiting.end()) {
        return true;
    }
    auto it = plugins_.find(name);
    if (it == plugins_.end()) {
        return false; // 未安装的依赖不参与环检测 (加载时必选缺失已报错)
    }
    visiting.push_back(name);
    for (const auto& d : it->second->depends) {
        if (hasDependencyCycle(d, visiting)) {
            return true;
        }
    }
    visiting.pop_back();
    return false;
}

bool PluginManager::checkDependencies(
    const std::string& name, const std::vector<std::string>& depends,
    const std::vector<std::string>& optionalDepends
) {
    bool ok = true;
    for (const auto& d : depends) {
        if (!plugins_.contains(d)) {
            XX_LOGE("Plugin `{}` load failed: required dependency `{}` not installed "
                    "(load it first)", name, d);
            ok = false;
        }
    }
    for (const auto& d : optionalDepends) {
        if (!plugins_.contains(d)) {
            XX_LOGW("Plugin `{}`: optional dependency `{}` not installed (skip enhancement)",
                    name, d);
        }
    }
    if (!ok) {
        return false;
    }
    // 循环依赖检测 (含新插件的依赖链 DFS)
    std::vector<std::string> visiting;
    for (const auto& d : depends) {
        if (hasDependencyCycle(d, visiting)) {
            std::string chain;
            for (size_t i = 0; i < visiting.size(); ++i) {
                if (i) {
                    chain += " -> ";
                }
                chain += visiting[i];
            }
            XX_LOGE("Plugin `{}` load failed: dependency cycle detected (chain: {})", name, chain);
            return false;
        }
    }
    return true;
}

asio::awaitable<std::shared_ptr<PluginInstance>> PluginManager::loadPluginAsync(std::string path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        // ---- 插件目录: 按 plugin.yaml 清单分派 ----
        std::string                 name, entry;
        std::vector<std::string>    depends, optionalDepends;
        bool                        enabled = true;
        if (!parsePluginManifest(fs::path(path), name, entry, depends, optionalDepends, enabled)) {
            co_return nullptr;
        }
        if (!enabled) {
            XX_LOGI("Plugin `{}` disabled by manifest, skip", name);
            co_return nullptr;
        }
        // 依赖检查 (必选缺失/可选警告/环检测)
        if (!checkDependencies(name, depends, optionalDepends)) {
            co_return nullptr;
        }
        // 所有插件统一为 C++ 插件: entry 总是指向动态库
        // (脚本能力由插件内部经能力调用委派给 interpreter 引擎, 宿主不参与)
        auto entryPath = (fs::path(path) / entry).lexically_normal().string();
        auto inst = co_await loadNativeAsync(std::move(entryPath));
        if (inst) {
            inst->depends         = std::move(depends);
            inst->optionalDepends = std::move(optionalDepends);
        }
        co_return inst;
    }
    // ---- 文件: 视为原生库路径 ----
    co_return co_await loadNativeAsync(std::move(path));
}

asio::awaitable<void> PluginManager::loadConfiguredPlugins(
    const std::vector<agentxx::agent::PluginConfig>& plugins
) {
    for (const auto& cfg : plugins) {
        if (!cfg.enabled) {
            continue;
        }
        co_await loadPluginAsync(cfg.path);
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
            inst->depends,
        });
    }
    return out;
}

// ==================== 插件互查 (list_plugins / get_plugin) ====================

/// 单插件信息 → JSON (io 线程)
static neograph::json pluginInfoToJson(const PluginManager::PluginListView& p) {
    neograph::json j = neograph::json::object();
    j["name"]         = p.name;
    j["version"]      = p.version;
    j["description"]  = p.description;
    j["path"]         = p.path;
    j["enabled"]      = p.enabled;
    j["tools"]        = neograph::json::array();
    for (const auto& t : p.tools) {
        j["tools"].push_back(t);
    }
    j["capabilities"] = neograph::json::array();
    for (const auto& c : p.capabilities) {
        j["capabilities"].push_back(c);
    }
    j["depends"] = neograph::json::array();
    for (const auto& d : p.depends) {
        j["depends"].push_back(d);
    }
    return j;
}

std::string PluginManager::listPluginsJson() {
    auto pluginList = list();
    neograph::json arr = neograph::json::array();
    for (const auto& p : pluginList) {
        arr.push_back(pluginInfoToJson(p));
    }
    return arr.dump();
}

std::string PluginManager::getPluginJson(const std::string& name) {
    auto inst = find(name);
    if (!inst) {
        return {};
    }
    PluginListView p{
        inst->name, inst->version, inst->description, inst->path,
        inst->enabled, inst->inflight.load(std::memory_order_acquire),
        inst->toolNames, inst->capabilities, inst->depends,
    };
    return pluginInfoToJson(p).dump();
}

std::string PluginManager::getOwnPluginJson(const std::string& name) {
    return getPluginJson(name);
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
