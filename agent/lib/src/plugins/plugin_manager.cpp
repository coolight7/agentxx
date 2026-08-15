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
    std::string                                    topic; ///< 完整 topic (含 plugin. 前缀)
    size_t                                         subscriptionId = 0;
    agentxx::plugin::PluginInstance*               inst           = nullptr;
    void (*handler)(const char* event_json, void* ud)             = nullptr;
    void* ud                                                      = nullptr;
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
                CP_UTF8,
                0,
                path.c_str(),
                -1,
                wpath.data(),
                static_cast<int>(wpath.size())
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
        err           = d ? d : "dlopen failed";
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
    void*       p = ::dlsym(handle, name);
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

bool CapabilityRegistry::registerCapability(
    std::string_view          name,
    std::string_view          provider,
    AgentxxCapabilityInvokeFn invoke,
    void*                     ctx
) {
    if (name.empty()) {
        return false;
    }
    // 同名能力重复注册: 拒绝 (能力委派需唯一 provider)
    if (caps_.contains(std::string{name})) {
        XX_LOGW(
            "CapabilityRegistry: capability `{}` already registered by `{}`",
            name,
            caps_.at(std::string{name}).provider
        );
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
        XX_LOGW(
            "CapabilityRegistry: capability `{}` owned by `{}`, cannot unregister by `{}`",
            name,
            it->second.provider,
            provider
        );
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
    parameters_(neograph::json::object()),
    instance_(instance) {
    // 注册时解析一次参数 schema 并缓存 (ModelCallWrapNode 每轮组装工具定义,
    // 避免对同一工具反复 parse JSON)
    if (spec_.parameters_json && *spec_.parameters_json) {
        try {
            auto params = neograph::json::parse(spec_.parameters_json);
            if (params.is_object()) {
                parameters_ = std::move(params);
            }
        } catch (const std::exception& e) {
            XX_LOGW(
                "PluginTool `{}`: invalid parameters_json: {}",
                spec_.name ? spec_.name : "",
                e.what()
            );
        }
    }
}

neograph::ChatTool PluginTool::get_definition() const {
    neograph::ChatTool def;
    def.name        = spec_.name ? spec_.name : "";
    def.description = spec_.description ? spec_.description : "";
    def.parameters  = parameters_; // 拷贝缓存 (浅层拷贝, 与解析结果独立)
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

    // 参数: toolcall 分发路径已注入 thread_id/tool_call_id; call_tool 路径由调用方提供
    std::string argsJson   = arguments.dump();
    std::string threadId   = arguments.value("thread_id", std::string{});
    std::string toolCallId = arguments.value("tool_call_id", std::string{});

    // 取消令牌 (经 Session 按 thread_id 取; 无会话时为空)
    auto                                          agentCtx = agentContext.lock();
    std::shared_ptr<neograph::graph::CancelToken> cancelToken;
    if (agentCtx) {
        cancelToken = agentxx::tools::getSessionCancelToken(agentCtx, arguments);
    }

    auto spec = spec_; // 拷贝 (跨线程)
    // 按值捕获 shared_ptr: 即使宿主等待方被取消/超时提前返回, 线程池中的
    // C 回调执行期间插件实例仍被引用计数保活, 不会走到 dlclose (卸载安全)
    // - inflight 计数在【线程池入口】递增 (而非协程帧): 超时取消销毁协程帧
    //   不会提前释放计数, unloadAsync 会等到 C 回调真正返回后才 dlclose,
    //   消除 "超时后卸载 → 执行已卸载代码段" 竞态 (见 plugins.md 11.2)
    std::function<asio::awaitable<std::string>(std::atomic<bool>&)> run
        = [spec,
           inst     = std::move(inst),
           argsJson = std::move(argsJson),
           threadId = std::move(threadId),
           toolCallId
           = std::move(toolCallId)](std::atomic<bool>& cancelFlag) -> asio::awaitable<std::string> {
        (void)cancelFlag; // 插件回调为黑盒, 无法协作式中止; 等待方取消后线程自然释放
        PluginInstance::InflightGuard guard(inst.get());
        std::string                   result;
        char*                         err = nullptr;
        char*                         out = spec.execute(
            spec.user_data,
            argsJson.c_str(),
            threadId.c_str(),
            toolCallId.c_str(),
            &err
        );
        if (err) {
            std::string errStr = err;
            inst->host.vtable->free(err);
            throw std::runtime_error(
                fmt::format("plugin tool error: {}", agentxx::util::autoTryConvertToUtf8(errStr))
            );
        }
        if (!out) {
            throw std::runtime_error("plugin tool returned null");
        }
        result = out;
        inst->host.vtable->free(out);
        co_return result;
    };

    // 协程 lambda 对象 (调用 exec() 才得到 awaitable)
    auto exec = [&]() -> asio::awaitable<std::string> {
        if (agentCtx && agentCtx->blockingPool) {
            co_return co_await agentxx::util::offloadCancellableAsync<std::string>(
                *agentCtx->blockingPool,
                cancelToken,
                run
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
        auto msgs                 = in.state.get_messages();
        summary["messages_count"] = msgs.size();
        bool hasToolCalls         = false;
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

    char* out = nullptr;
    char* err = nullptr;
    int   rc  = hook.fn(hook.ud, point, summaryStr.c_str(), &out, &err);
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

asio::awaitable<void> PluginMiddlewareHandle::onAgentcallStartFunc(neograph::graph::NodeInput& in) {
    co_await dispatch(AGENTXX_HOOK_AGENT_START, in);
}

asio::awaitable<void> PluginMiddlewareHandle::
    onAgentcallEndFunc(const neograph::graph::NodeInput& in, neograph::graph::NodeOutput&) {
    co_await dispatch(AGENTXX_HOOK_AGENT_END, in);
}

asio::awaitable<void> PluginMiddlewareHandle::onModelcallStartFunc(neograph::graph::NodeInput& in) {
    co_await dispatch(AGENTXX_HOOK_MODEL_START, in);
}

asio::awaitable<void> PluginMiddlewareHandle::onModelcallRunFunc(neograph::graph::NodeInput& in) {
    co_await dispatch(AGENTXX_HOOK_MODEL_RUN, in);
}

asio::awaitable<void> PluginMiddlewareHandle::
    onModelcallEndFunc(const neograph::graph::NodeInput& in, neograph::graph::NodeOutput&) {
    co_await dispatch(AGENTXX_HOOK_MODEL_END, in);
}

asio::awaitable<void> PluginMiddlewareHandle::onToolcallStartFunc(neograph::graph::NodeInput& in) {
    co_await dispatch(AGENTXX_HOOK_TOOL_START, in);
}

asio::awaitable<void> PluginMiddlewareHandle::
    onToolcallEndFunc(const neograph::graph::NodeInput& in, neograph::graph::NodeOutput&) {
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
    // 按依赖图逆序 (先子后父) 逐个卸载: 脚本插件先卸载 (unload 回调经
    // invoke_capability 通知引擎释放脚本运行时), 引擎插件最后卸载
    // (join 内部线程后 dlclose, 代码段无执行者)
    // - 不等在途回调: 调用方 (AgentContext 析构) 须保证无在途插件回调,
    //   否则引擎类插件的 unload 回调 (join 线程) 可能等待在途回调而阻塞
    std::vector<std::string> names;
    names.reserve(plugins_.size());
    for (const auto& [name, inst] : plugins_) {
        (void)inst;
        names.push_back(name);
    }
    for (const auto& name : names) {
        auto inst = find(name);
        if (inst) {
            shutdownPlugin(inst);
        }
    }
    plugins_.clear();
}

void PluginManager::shutdownPlugin(const std::shared_ptr<PluginInstance>& inst) {
    if (!inst || inst->unloadRequested) {
        return;
    }
    inst->unloadRequested = true;
    // 先递归卸载必选依赖本插件的插件 (先子后父)
    for (const auto& dep : reverseRequiredDeps(inst->name, /*onlyEnabled=*/false)) {
        auto depInst = find(dep);
        if (depInst && !depInst->unloadRequested) {
            shutdownPlugin(depInst);
        }
    }
    // 摘除全部注册 → 释放工具对象 → 摘除中间件
    detachAll(inst.get());
    inst->tools.clear();
    eraseMiddleware(inst.get());
    // unload 回调 (业务清理, 如引擎 join 内部线程); 宿主已自动反注册全部残留
    if (inst->dlHandle) {
        std::string err;
        auto        fn = reinterpret_cast<AgentxxPluginUnloadFn>(
            NativeLoader::sym(inst->dlHandle, AGENTXX_PLUGIN_SYMBOL_UNLOAD, err)
        );
        if (fn) {
            fn(inst->pluginCtx);
        }
    }
    // 实例随 plugins_.clear()/局部引用释放 → dlclose
    XX_LOGI("Plugin shutdown: {}", inst->name);
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
    size_t n = std::strlen(s) + 1;
    char*  p = static_cast<char*>(::malloc(n));
    if (p) {
        std::memcpy(p, s, n);
    }
    return p;
}

/// C ABI 边界异常兜底宏: vtable 函数内部不得让 C++ 异常逃逸 (跨边界 UB),
/// 统一捕获转日志并按失败返回值返回。用法:
///   XX_PLUGIN_CATCH_BEGIN
///   ... 函数体 ...
///   XX_PLUGIN_CATCH_END(失败返回值)     // 有返回值函数
///   XX_PLUGIN_CATCH_END_VOID()          // void 函数
#define XX_PLUGIN_CATCH_BEGIN try {
#define XX_PLUGIN_CATCH_END(ret)                          \
    }                                                     \
    catch (const std::exception& e) {                     \
        XX_LOGE("plugin vtable exception: {}", e.what()); \
        return (ret);                                     \
    }                                                     \
    catch (...) {                                         \
        XX_LOGE("plugin vtable unknown exception");       \
        return (ret);                                     \
    }
#define XX_PLUGIN_CATCH_END_VOID()                        \
    }                                                     \
    catch (const std::exception& e) {                     \
        XX_LOGE("plugin vtable exception: {}", e.what()); \
        return;                                           \
    }                                                     \
    catch (...) {                                         \
        XX_LOGE("plugin vtable unknown exception");       \
        return;                                           \
    }

static int xx_register_tool(const AgentxxHost* host, const AgentxxToolSpec* spec) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !spec || !spec->name || !*spec->name) {
        return -1;
    }
    // io 线程约束操作: 非 io 线程调用 (JS 线程等) 经 post 同步等待
    auto            mgrPtr   = mgr;
    auto            instPtr  = inst;
    AgentxxToolSpec specCopy = *spec;
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, specCopy]() {
        return mgrPtr->registerTool(instPtr, &specCopy);
    });
    XX_PLUGIN_CATCH_END(-1)
}

static int xx_unregister_tool(const AgentxxHost* host, const char* name) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !name) {
        return -1;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string toolName{name};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, toolName]() {
        return mgrPtr->unregisterTool(instPtr, toolName.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

static int xx_register_hook(
    const AgentxxHost* host,
    AgentxxHookPoint   point,
    AgentxxHookFn      fn,
    void*              user_data
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return -1;
    }
    auto mgrPtr  = mgr;
    auto instPtr = inst;
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, point, fn, user_data]() {
        return mgrPtr->registerHook(instPtr, point, fn, user_data);
    });
    XX_PLUGIN_CATCH_END(-1)
}

static int xx_unregister_hook(
    const AgentxxHost* host,
    AgentxxHookPoint   point,
    AgentxxHookFn      fn,
    void*              user_data
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return -1;
    }
    auto mgrPtr  = mgr;
    auto instPtr = inst;
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, point, fn, user_data]() {
        return mgrPtr->unregisterHook(instPtr, point, fn, user_data);
    });
    XX_PLUGIN_CATCH_END(-1)
}

static AgentxxSubscription* xx_subscribe(
    const AgentxxHost* host,
    const char*        topic,
    void (*handler)(const char* event_json, void* ud),
    void* ud
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string topicStr{topic ? topic : ""};
    return ioCallSync<AgentxxSubscription*>(mgrPtr, [mgrPtr, instPtr, topicStr, handler, ud]() {
        return mgrPtr->subscribe(instPtr, topicStr.c_str(), handler, ud);
    });
    XX_PLUGIN_CATCH_END(nullptr)
}

static void xx_unsubscribe(AgentxxSubscription* sub) {
    XX_PLUGIN_CATCH_BEGIN
    if (!sub) {
        return;
    }
    // 已退订 (inst 已断) 或插件已卸载: 对象由 shared_ptr 管理, 无需也不得 delete
    if (!sub->inst) {
        return;
    }
    auto mgr = sub->inst->manager.lock().get();
    if (mgr) {
        ioCallSyncVoid(mgr, [mgr, sub]() {
            mgr->unsubscribe(sub);
        });
    }
    // mgr 已释放 (进程销毁路径): 订阅对象随 PluginInstance 生命周期释放,
    // 此处仅断 inst 防止后续误用
    sub->inst = nullptr;
    XX_PLUGIN_CATCH_END_VOID()
}

static int xx_publish(const AgentxxHost* host, const char* topic, const char* event_json) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = mgrOf(host);
    if (!mgr) {
        return -1;
    }
    return mgr->publish(topic, event_json);
    XX_PLUGIN_CATCH_END(-1)
}

static int xx_register_capability(const AgentxxHost* host, const char* capability) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !capability) {
        return -1;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string cap{capability};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, cap]() {
        return mgrPtr->registerCapability(instPtr, cap.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

static int xx_unregister_capability(const AgentxxHost* host, const char* capability) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !capability) {
        return -1;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string cap{capability};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, cap]() {
        return mgrPtr->unregisterCapability(instPtr, cap.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

static int xx_has_capability(const AgentxxHost* host, const char* capability) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = mgrOf(host);
    if (!mgr || !capability) {
        return 0;
    }
    // caps_ 为 io 线程数据结构: 跨线程调用经 post 到 io 线程查询
    std::string cap{capability};
    auto        mgrPtr = mgr;
    return ioCallSync<int>(mgrPtr, [mgrPtr, cap]() {
        return mgrPtr->hasCapability(cap.c_str()) ? 1 : 0;
    });
    XX_PLUGIN_CATCH_END(0)
}

static int xx_register_capability_ex(
    const AgentxxHost*        host,
    const char*               capability,
    AgentxxCapabilityInvokeFn invoke,
    void*                     ctx
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !capability || !*capability) {
        return -1;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string cap{capability};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, cap, invoke, ctx]() {
        return mgrPtr->registerCapabilityEx(instPtr, cap.c_str(), invoke, ctx);
    });
    XX_PLUGIN_CATCH_END(-1)
}

static char* xx_invoke_capability(
    const AgentxxHost* host,
    const char*        capability,
    const char*        method,
    const char*        args_json,
    char**             error_out
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !capability || !method) {
        return nullptr;
    }
    std::string cap{capability};
    std::string m{method};
    std::string args{args_json ? args_json : "{}"};
    // 查表在 io 线程 (invokeCapability 内部), 提供者回调在调用方线程执行
    return mgr->invokeCapability(inst, cap.c_str(), m.c_str(), args.c_str(), error_out);
    XX_PLUGIN_CATCH_END(nullptr)
}

static char* xx_call_tool(
    const AgentxxHost* host,
    const char*        name,
    const char*        args_json,
    const char*        thread_id,
    char**             error_out
) {
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    auto setErr = [&](const std::string& msg) {
        if (error_out) {
            *error_out = inst->host.vtable->strdup(msg.c_str());
        }
    };
    std::string toolName{name ? name : ""};
    std::string args{args_json ? args_json : ""};
    std::string tid{thread_id ? thread_id : ""};
    try {
        // 1. 查表在 io 线程 (短临界区; 查表/执行分离, 见 plugins.md 11.5.2):
        //    shared_ptr 保活目标插件代码段 —— 执行期间即使目标插件被卸载,
        //    引用计数阻止 ~PluginInstance → dlclose, 无悬垂执行
        std::shared_ptr<agentxx::tools::XXToolBase> tool;
        bool found = ioCallSync<bool>(mgr, [mgr, &tool, &toolName]() {
            tool = mgr->registry()->find(toolName);
            return tool != nullptr;
        });
        if (!found) {
            setErr(fmt::format("plugin call_tool: tool `{}` not found", toolName));
            return nullptr;
        }
        auto pluginTool = std::dynamic_pointer_cast<PluginTool>(tool);
        if (!pluginTool) {
            setErr(fmt::format("plugin call_tool: tool `{}` is not a plugin tool", toolName));
            return nullptr;
        }
        // 2. 目标工具 execute 在【调用方线程】执行 (线程池/JS 线程内安全,
        //    不阻塞 io 线程; io 线程内调用会阻塞 io 线程, 插件应避免)
        PluginInstance::InflightGuard guard(inst);
        neograph::json                parsed = neograph::json::object();
        if (!args.empty()) {
            try {
                auto j = neograph::json::parse(args);
                if (j.is_object()) {
                    parsed = std::move(j);
                }
            } catch (const std::exception& e) {
                setErr(fmt::format("plugin call_tool: invalid args_json: {}", e.what()));
                return nullptr;
            }
        }
        parsed["thread_id"]    = tid;
        parsed["tool_call_id"] = fmt::format("plugin_call_{}", ++g_pluginCallSeq);

        const auto& spec = pluginTool->spec();
        char*       err  = nullptr;
        char* out = spec.execute(spec.user_data, parsed.dump().c_str(), tid.c_str(), "", &err);
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
    } catch (const std::exception& e) {
        setErr(fmt::format("plugin call_tool: {}", e.what()));
        return nullptr;
    } catch (...) {
        setErr("plugin call_tool: unknown exception");
        return nullptr;
    }
}

static char* xx_list_plugins(const AgentxxHost* host) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = mgrOf(host);
    if (!mgr) {
        return nullptr;
    }
    // plugins_ 为 io 线程数据结构: 整体在 io 线程执行; 字符串跨边界经 host->alloc
    auto mgrPtr = mgr;
    auto json   = ioCallSync<std::string>(mgrPtr, [mgrPtr]() {
        return mgrPtr->listPluginsJson();
    });
    return host->vtable->strdup(json.c_str());
    XX_PLUGIN_CATCH_END(nullptr)
}

static char* xx_get_plugin(const AgentxxHost* host, const char* name) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = mgrOf(host);
    if (!mgr || !name) {
        return nullptr;
    }
    auto        mgrPtr = mgr;
    std::string pluginName{name};
    auto        json = ioCallSync<std::string>(mgrPtr, [mgrPtr, pluginName]() {
        return mgrPtr->getPluginJson(pluginName);
    });
    if (json.empty()) {
        return nullptr; // 未安装
    }
    return host->vtable->strdup(json.c_str());
    XX_PLUGIN_CATCH_END(nullptr)
}

static char* xx_get_own_info(const AgentxxHost* host) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    auto        mgrPtr  = mgr;
    std::string ownName = inst->name;
    auto        json    = ioCallSync<std::string>(mgrPtr, [mgrPtr, ownName]() {
        return mgrPtr->getPluginJson(ownName);
    });
    if (json.empty()) {
        return nullptr;
    }
    return host->vtable->strdup(json.c_str());
    XX_PLUGIN_CATCH_END(nullptr)
}

static char* xx_get_share_store(const AgentxxHost* host, const char* thread_id, long long id) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    // shareStore 仅 io 线程访问 (无锁模型): 跨线程经 post 同步等待
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string tid{thread_id ? thread_id : ""};
    return ioCallSync<char*>(mgrPtr, [mgrPtr, instPtr, tid, id]() {
        return mgrPtr->getShareStore(instPtr, tid.c_str(), id);
    });
    XX_PLUGIN_CATCH_END(nullptr)
}

static void xx_emit_message_tip(
    const AgentxxHost* host,
    const char*        thread_id,
    const char*        text,
    int                level
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string tid{thread_id ? thread_id : ""};
    std::string msg{text ? text : ""};
    ioCallSyncVoid(mgrPtr, [mgrPtr, instPtr, tid, msg, level]() {
        mgrPtr->emitMessageTip(instPtr, tid.c_str(), msg.c_str(), level);
    });
    XX_PLUGIN_CATCH_END_VOID()
}

static int xx_is_io_thread(const AgentxxHost* host) {
    auto mgr = mgrOf(host);
    return (mgr && mgr->isIoThread()) ? 1 : 0;
}

static void xx_post_to_io(const AgentxxHost* host, void (*fn)(void* ud), void* ud) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = mgrOf(host);
    if (!mgr || !fn) {
        return;
    }
    mgr->postToIo([fn, ud]() {
        fn(ud);
    });
    XX_PLUGIN_CATCH_END_VOID()
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

/// JSON 辅助: 提取字符串字段 (线程安全, 纯函数; 供插件替代手写 JSON 解析)
static char* xx_json_get_string(const AgentxxHost* host, const char* json, const char* key) {
    auto inst = instOf(host);
    if (!inst || !json || !key) {
        return nullptr;
    }
    try {
        auto j = neograph::json::parse(json);
        if (j.is_object() && j.contains(key) && j[key].is_string()) {
            return inst->host.vtable->strdup(j[key].get<std::string>().c_str());
        }
    } catch (const std::exception&) {
        // JSON 非法: 视为无此字段
    }
    return nullptr;
}

/// JSON 辅助: 字符串 → JSON 字符串字面量 (含引号与转义; 供插件拼 JSON 时转义值)
static char* xx_json_escape(const AgentxxHost* host, const char* s) {
    auto inst = instOf(host);
    if (!inst || !s) {
        return nullptr;
    }
    std::string out;
    out.reserve(std::strlen(s) + 2);
    out += '"';
    for (const char* p = s; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    out += fmt::format("\\u{:04x}", c);
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    out += '"';
    return inst->host.vtable->strdup(out.c_str());
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
    xx_json_get_string,
    xx_json_escape,
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

int PluginManager::registerHook(
    PluginInstance*  inst,
    AgentxxHookPoint point,
    AgentxxHookFn    fn,
    void*            ud
) {
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
            fmt::format("{}_middleware", inst->name),
            agentContext_,
            std::move(shared)
        );
        inst->middleware->disabled = !inst->enabled;
        ctx->middlewareHandleContext->handles.push_back(inst->middleware);
    }
    inst->middleware->setHook(point, fn, ud);
    // 记录注册信息 (enable 重建中间件时恢复; 同点重复注册覆盖旧记录)
    inst->hookRegistrations.erase(
        std::remove_if(
            inst->hookRegistrations.begin(),
            inst->hookRegistrations.end(),
            [point](const PluginInstance::HookRegistration& h) {
                return h.point == point;
            }
        ),
        inst->hookRegistrations.end()
    );
    inst->hookRegistrations.push_back(PluginInstance::HookRegistration{point, fn, ud});
    XX_LOGI("Plugin `{}` registered hook point={}", inst->name, static_cast<int>(point));
    return 0;
}

int PluginManager::unregisterHook(
    PluginInstance*  inst,
    AgentxxHookPoint point,
    AgentxxHookFn    fn,
    void*            ud
) {
    if (!inst || point < 0 || point >= AGENTXX_HOOK_COUNT) {
        return -1;
    }
    if (inst->middleware) {
        inst->middleware->clearHook(point);
    }
    inst->hookRegistrations.erase(
        std::remove_if(
            inst->hookRegistrations.begin(),
            inst->hookRegistrations.end(),
            [point, fn, ud](const PluginInstance::HookRegistration& h) {
                return h.point == point && (fn == nullptr || (h.fn == fn && h.ud == ud));
            }
        ),
        inst->hookRegistrations.end()
    );
    return 0;
}

// ==================== 事件订阅/发布 ====================

AgentxxSubscription* PluginManager::subscribe(
    PluginInstance* inst,
    const char*     topic,
    void (*handler)(const char* event_json, void* ud),
    void* ud
) {
    if (!inst || !topic || !*topic || !handler) {
        return nullptr;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->bus) {
        XX_LOGW("Plugin `{}` subscribe: bus not ready", inst->name);
        return nullptr;
    }
    auto fullTopic = fmt::format("plugin.{}", topic);
    // 聚合类型: 先构造再包装 shared_ptr (make_shared 花括号调用兼容性差)
    auto sub = std::shared_ptr<AgentxxSubscription>(
        new AgentxxSubscription{ctx->bus, fullTopic, 0, inst, handler, ud}
    );
    // handler lambda 捕获 shared_ptr: EventStream 派发快照期间即使被
    // unsubscribe/插件卸载, 订阅对象也不会提前释放 (避免派发中 UAF)
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
    return sub.get();
}

void PluginManager::unsubscribe(AgentxxSubscription* sub) {
    if (!sub) {
        return;
    }
    // 从所属插件实例的订阅表移除 (shared_ptr 释放); 先断 inst 引用:
    // 派发中的 handler lambda 仍持有 shared_ptr, 看到 inst==nullptr 跳过
    if (sub->inst) {
        auto& subs = sub->inst->subscriptions;
        subs.erase(
            std::remove_if(
                subs.begin(),
                subs.end(),
                [sub](const std::shared_ptr<AgentxxSubscription>& s) {
                    return s.get() == sub;
                }
            ),
            subs.end()
        );
        sub->inst = nullptr;
    }
    if (sub->bus) {
        try {
            sub->bus->get<std::string>(sub->topic).unsubscribe(sub->subscriptionId);
        } catch (const std::exception& e) {
            XX_LOGW("Plugin unsubscribe `{}` error: {}", sub->topic, e.what());
        }
    }
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
    // 记录完整注册信息 (enable 恢复时保留方法回调, 避免降级为哑能力)
    inst->capabilityRegistrations.erase(
        std::remove_if(
            inst->capabilityRegistrations.begin(),
            inst->capabilityRegistrations.end(),
            [capability](const PluginInstance::CapabilityRegistration& c) {
                return c.name == capability;
            }
        ),
        inst->capabilityRegistrations.end()
    );
    inst->capabilityRegistrations.push_back(
        PluginInstance::CapabilityRegistration{capability, nullptr, nullptr}
    );
    return 0;
}

int PluginManager::unregisterCapability(PluginInstance* inst, const char* capability) {
    if (!inst || !capability) {
        return -1;
    }
    auto it = std::find_if(
        inst->capabilityRegistrations.begin(),
        inst->capabilityRegistrations.end(),
        [capability](const PluginInstance::CapabilityRegistration& c) {
            return c.name == capability;
        }
    );
    if (it == inst->capabilityRegistrations.end()) {
        return -1;
    }
    inst->capabilityRegistrations.erase(it);
    capabilities_->unregisterCapability(capability, inst->name);
    return 0;
}

int PluginManager::hasCapability(const char* capability) const {
    return capability && capabilities_->has(capability) ? 1 : 0;
}

int PluginManager::registerCapabilityEx(
    PluginInstance*           inst,
    const char*               capability,
    AgentxxCapabilityInvokeFn invoke,
    void*                     ctx
) {
    if (!inst || !capability || !*capability || !invoke) {
        return -1;
    }
    if (!capabilities_->registerCapability(capability, inst->name, invoke, ctx)) {
        return -1;
    }
    // 记录完整注册信息 (enable 恢复时保留方法回调)
    inst->capabilityRegistrations.erase(
        std::remove_if(
            inst->capabilityRegistrations.begin(),
            inst->capabilityRegistrations.end(),
            [capability](const PluginInstance::CapabilityRegistration& c) {
                return c.name == capability;
            }
        ),
        inst->capabilityRegistrations.end()
    );
    inst->capabilityRegistrations.push_back(
        PluginInstance::CapabilityRegistration{capability, invoke, ctx}
    );
    return 0;
}

char* PluginManager::invokeCapability(
    PluginInstance* caller,
    const char*     capability,
    const char*     method,
    const char*     args_json,
    char**          error_out
) {
    auto setErr = [&](const std::string& msg) {
        if (error_out && caller) {
            *error_out = caller->host.vtable->strdup(msg.c_str());
        }
    };
    // 1. io 线程查表并拷贝 (短临界区)
    CapabilityRegistry::Entry entry;
    bool                      found = false;
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

char* PluginManager::getShareStore(PluginInstance* inst, const char* thread_id, long long id) {
    if (!inst || !thread_id) {
        return nullptr;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        return nullptr;
    }
    auto value
        = ctx->middlewareHandleContext->getShareStoreItemValue(thread_id, static_cast<size_t>(id));
    if (!value) {
        return nullptr;
    }
    return inst->host.vtable->strdup(value->c_str());
}

void PluginManager::emitMessageTip(
    PluginInstance* inst,
    const char*     thread_id,
    const char*     text,
    int             level
) {
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

/// 从库文件名推断插件名 (libfoo.so → foo; foo.dll → foo; libfoo.so.1.2 → foo)
/// - 去 lib 前缀; 从第一个已知扩展名 (.so/.dylib/.dll) 截断, 兼容版本号后缀
static std::string pluginNameFromPath(const std::string& path) {
    auto base = std::filesystem::path(path).filename().string();
    // 去 lib 前缀
    if (base.size() > 3 && base.compare(0, 3, "lib") == 0) {
        base.erase(0, 3);
    }
    // 去扩展名及其后版本号 (libfoo.so.1.2 → foo; my.plugin.so → my.plugin)
    for (const char* ext : {".dylib", ".so", ".dll"}) {
        auto pos = base.find(ext);
        if (pos != std::string::npos) {
            base.erase(pos);
            break;
        }
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
    void*       handle = co_await agentxx::util::offloadAsync<void*>(
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
                    "Plugin `{}` api_version {} mismatch (host expects {})",
                    path,
                    info->api_version,
                    AGENTXX_PLUGIN_API_VERSION
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
        XX_LOGE(
            "Plugin `{}` missing entry symbol `{}`: {}",
            path,
            AGENTXX_PLUGIN_SYMBOL_ENTRY,
            err
        );
        NativeLoader::close(handle);
        co_return nullptr;
    }

    auto inst         = std::make_shared<PluginInstance>(name);
    inst->version     = std::move(version);
    inst->description = std::move(desc);
    inst->path        = path;
    inst->dlHandle    = handle;
    inst->self        = inst;
    inst->manager     = shared_from_this();
    inst->host.vtable = &g_hostVtable;
    inst->host.opaque = inst.get();

    plugins_[name] = inst;

    // entry 调用卸载到线程池 (不阻塞 io 线程):
    // - 插件 entry 内的注册动作 (register_tool/hook/capability) 经 vtable
    //   ioCallSync 回 io 线程执行 (契约不变: 注册始终在 io 线程串行)
    // - 关键: 脚本插件的 entry 会经 invoke_capability 同步等待 JS 线程
    //   加载脚本, 而 JS 线程内脚本注册又要回 io 线程 —— 若 entry 在 io
    //   线程执行则 io↔引擎互等死锁 (见 plugins.md 11.5.2); 线程池执行时
    //   io 线程保持空闲, 可服务注册回调
    int rc = co_await agentxx::util::offloadAsync<int>(
        *ctx->blockingPool,
        [inst, entry]() -> asio::awaitable<int> {
            co_return entry(&inst->host, &inst->pluginCtx);
        }
    );
    if (rc != 0) {
        XX_LOGE("Plugin `{}` entry returned {}", name, rc);
        detachAll(inst.get());
        plugins_.erase(name);
        co_return nullptr;
    }

    XX_LOGI(
        "Plugin loaded: {} v{} ({} tools, {} hooks, {} capabilities)",
        name,
        inst->version,
        inst->toolNames.size(),
        inst->hookRegistrations.size(),
        inst->capabilityRegistrations.size()
    );
    co_return inst;
}

/// 卸载总时限 (毫秒): 在途回调等待/轮次等待的上限, 防止慢/恶意插件无限
/// 阻塞 io 线程; 超时后放弃卸载 (插件保持已 detach 状态, 可稍后重试)
static constexpr std::chrono::milliseconds kPluginUnloadTimeoutMs{30000};

asio::awaitable<bool> PluginManager::waitInflightZero(
    const std::shared_ptr<PluginInstance>& inst,
    std::chrono::milliseconds              timeout
) {
    auto               ex = co_await asio::this_coro::executor;
    asio::steady_timer timer(ex);
    auto               deadline = std::chrono::steady_clock::now() + timeout;
    while (inst->inflight.load(std::memory_order_acquire) > 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            XX_LOGW(
                "Plugin `{}` unload: inflight not zero within {}ms, abort unload",
                inst->name,
                timeout.count()
            );
            co_return false;
        }
        timer.expires_after(std::chrono::milliseconds(10));
        co_await timer.async_wait(asio::use_awaitable);
    }
    co_return true;
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
    // 订阅: 全部退订
    // - 先 move 出容器再遍历: 避免 unsubscribe 内部 erase 修改正被 range-for
    //   遍历的容器 (迭代器失效 UB); 退订后 inst 引用断开, 派发中的 handler
    //   经 shared_ptr 保活且看到 inst==nullptr 跳过
    auto subs = std::move(inst->subscriptions);
    inst->subscriptions.clear();
    for (const auto& sub : subs) {
        unsubscribe(sub.get());
    }
    // 能力: 摘除能力表注册 (注册信息保留于 capabilityRegistrations, enable 恢复)
    for (const auto& c : inst->capabilityRegistrations) {
        capabilities_->unregisterCapability(c.name, inst->name);
    }
}

void PluginManager::eraseMiddleware(PluginInstance* inst) {
    if (!inst) {
        return;
    }
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
    // 摘除后清掉指针 (防误用; enable 时按需重建)
    inst->middleware = nullptr;
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
    // - 总时限: 慢/恶意插件回调不返回时放弃卸载 (插件保持已 detach 状态),
    //   避免无限阻塞 io 线程; unloadRequested 复位允许稍后重试
    if (!co_await waitInflightZero(inst, kPluginUnloadTimeoutMs)) {
        inst->unloadRequested = false;
        co_return false;
    }

    // 等待进行中的轮次结束 (避免 erase 破坏执行中的 handles 下标遍历;
    // 禁用路径走轮末 flushPendingCleanup, 卸载路径必须彻底摘除以断开
    // 中间件↔实例循环引用) —— 同样带总时限
    auto               ex = co_await asio::this_coro::executor;
    asio::steady_timer timer(ex);
    auto               turnDeadline = std::chrono::steady_clock::now() + kPluginUnloadTimeoutMs;
    while (runningTurns_ > 0) {
        if (std::chrono::steady_clock::now() >= turnDeadline) {
            XX_LOGW(
                "Plugin `{}` unload: running turn not finished within {}ms, abort unload",
                inst->name,
                kPluginUnloadTimeoutMs.count()
            );
            inst->unloadRequested = false;
            co_return false;
        }
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
        auto        fn = reinterpret_cast<AgentxxPluginUnloadFn>(
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
std::vector<std::string>
    PluginManager::reverseRequiredDeps(const std::string& target, bool onlyEnabled) const {
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
    disableImpl(name, /*userInitiated=*/true);
}

void PluginManager::disableImpl(std::string_view name, bool userInitiated) {
    auto inst = find(name);
    if (!inst || !inst->enabled) {
        return;
    }
    if (userInitiated) {
        inst->userDisabled = true; // 用户显式禁用: enable 级联不复活
    }
    // 依赖图级联: 先禁用必选依赖本插件的插件 (脚本插件随引擎一起停用)
    for (const auto& dep : reverseRequiredDeps(inst->name, /*onlyEnabled=*/true)) {
        XX_LOGI("Disable `{}` cascades disable of dependent plugin `{}`", inst->name, dep);
        disableImpl(dep, /*userInitiated=*/false);
    }
    inst->enabled = false;
    // 工具摘除 (对象保留, enable 恢复)
    for (const auto& tool : inst->tools) {
        registry_->unregisterTool(tool->get_name());
    }
    inst->toolNames.clear();
    // 钩子停用: disabled 位 → 下一节点执行即跳过
    if (inst->middleware) {
        inst->middleware->disabled = true;
        if (hasRunningTurn()) {
            // 轮次执行中: 轮末 flushPendingCleanup 摘除 (防执行中下标错位)
            if (std::find(pendingCleanup_.begin(), pendingCleanup_.end(), inst->name)
                == pendingCleanup_.end()) {
                pendingCleanup_.push_back(inst->name);
            }
        } else {
            // 无轮次执行: 立即摘除 (无执行中遍历, erase 安全)
            eraseMiddleware(inst.get());
        }
    }
    XX_LOGI("Plugin disabled: {}", inst->name);
}

void PluginManager::enable(std::string_view name) {
    enableImpl(name, /*userInitiated=*/true);
}

void PluginManager::enableImpl(std::string_view name, bool userInitiated) {
    auto inst = find(name);
    if (!inst) {
        return;
    }
    if (userInitiated) {
        inst->userDisabled = false;
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
    // 钩子恢复:
    // - middleware 仍挂 handles 中 (disable 后未跨轮次): 仅复位 disabled
    // - middleware 已被轮末摘除 (disable → flushPendingCleanup → null):
    //   按注册记录重建中间件并重新挂栈, 恢复全部钩子 (热启用不丢钩子)
    auto ctx = agentContext_.lock();
    if (inst->middleware) {
        inst->middleware->disabled = false;
        pendingCleanup_.erase(
            std::remove(pendingCleanup_.begin(), pendingCleanup_.end(), std::string{name}),
            pendingCleanup_.end()
        );
    } else if (!inst->hookRegistrations.empty()) {
        if (ctx && ctx->middlewareHandleContext) {
            auto shared = inst->self.lock();
            if (shared) {
                auto mw = std::make_shared<PluginMiddlewareHandle>(
                    fmt::format("{}_middleware", inst->name),
                    agentContext_,
                    std::move(shared)
                );
                mw->disabled = false;
                for (const auto& h : inst->hookRegistrations) {
                    mw->setHook(h.point, h.fn, h.ud);
                }
                ctx->middlewareHandleContext->handles.push_back(mw);
                inst->middleware = std::move(mw);
                XX_LOGI(
                    "Plugin `{}` middleware rebuilt with {} hooks",
                    inst->name,
                    inst->hookRegistrations.size()
                );
            }
        } else {
            XX_LOGW(
                "Plugin `{}` enable: middleware context unavailable, hooks not restored",
                inst->name
            );
        }
    }
    // 能力重新声明 (用保存的完整注册信息, 保留方法回调)
    for (const auto& c : inst->capabilityRegistrations) {
        capabilities_->registerCapability(c.name, inst->name, c.invoke, c.ctx);
    }
    // 依赖图级联: 再启用必选依赖本插件的插件 (仅未被用户显式禁用的)
    for (const auto& dep : reverseRequiredDeps(inst->name, /*onlyEnabled=*/false)) {
        auto depInst = find(dep);
        if (depInst && !depInst->enabled && !depInst->userDisabled) {
            XX_LOGI("Enable `{}` cascades enable of dependent plugin `{}`", inst->name, dep);
            enableImpl(dep, /*userInitiated=*/false);
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
    auto            yamlPath = dir / "plugin.yaml";
    std::error_code ec;
    if (!std::filesystem::exists(yamlPath, ec)) {
        XX_LOGW("Plugin dir `{}` has no plugin.yaml", dir.string());
        return false;
    }
    try {
        auto node = YAML::LoadFile(yamlPath.string());
        name      = node["name"] ? node["name"].as<std::string>() : std::string{};
        entry     = node["entry"] ? node["entry"].as<std::string>() : std::string{};
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
                auto s  = node["enabled"].as<std::string>();
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

bool PluginManager::hasDependencyCycle(const std::string& name, std::vector<std::string>& visiting)
    const {
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
    const std::string&              name,
    const std::vector<std::string>& depends,
    const std::vector<std::string>& optionalDepends
) {
    bool ok = true;
    for (const auto& d : depends) {
        if (!plugins_.contains(d)) {
            XX_LOGE(
                "Plugin `{}` load failed: required dependency `{}` not installed "
                "(load it first)",
                name,
                d
            );
            ok = false;
        }
    }
    for (const auto& d : optionalDepends) {
        if (!plugins_.contains(d)) {
            XX_LOGW(
                "Plugin `{}`: optional dependency `{}` not installed (skip enhancement)",
                name,
                d
            );
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
        std::string              name, entry;
        std::vector<std::string> depends, optionalDepends;
        bool                     enabled = true;
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
        auto inst      = co_await loadNativeAsync(std::move(entryPath));
        if (inst) {
            inst->depends         = std::move(depends);
            inst->optionalDepends = std::move(optionalDepends);
        }
        co_return inst;
    }
    // ---- 文件: 视为原生库路径 ----
    co_return co_await loadNativeAsync(std::move(path));
}

asio::awaitable<void>
    PluginManager::loadConfiguredPlugins(const std::vector<agentxx::agent::PluginConfig>& plugins) {
    namespace fs = std::filesystem;

    // 预解析各配置项依赖 (目录插件读 plugin.yaml depends; 库路径按文件名
    // 推导插件名参与排序 —— libagentxx_plugin_js.so → agentxx_plugin_js)
    struct Item {
        std::string              path;
        std::string              name; ///< 空 = 无法推导 (不影响排序)
        std::vector<std::string> depends;
    };

    std::vector<Item> items;
    for (const auto& cfg : plugins) {
        if (!cfg.enabled) {
            continue;
        }
        std::error_code ec;
        if (fs::is_directory(cfg.path, ec)) {
            std::string              name, entry;
            std::vector<std::string> depends, optionalDepends;
            bool                     enabled = true;
            if (parsePluginManifest(
                    fs::path(cfg.path),
                    name,
                    entry,
                    depends,
                    optionalDepends,
                    enabled
                )) {
                items.push_back(Item{cfg.path, name, depends});
                continue;
            }
        }
        items.push_back(Item{cfg.path, pluginNameFromPath(cfg.path), {}});
    }

    // 拓扑排序 (Kahn): 依赖者排在被依赖者之后, 避免配置顺序导致必选依赖缺失
    // - 配置列表中且未放置的依赖 → 未满足; 不在配置列表的依赖视为已满足 (已加载)
    // - 无进展 (环/缺失) 时剩余项按原序附后, 由 loadPluginAsync 的依赖检查报错
    std::vector<Item> ordered;
    ordered.reserve(items.size());
    std::vector<bool> placed(items.size(), false);
    size_t            placedCount = 0;
    while (placedCount < items.size()) {
        size_t progress = 0;
        for (size_t i = 0; i < items.size(); ++i) {
            if (placed[i]) {
                continue;
            }
            bool depsOk = true;
            for (const auto& d : items[i].depends) {
                for (size_t j = 0; j < items.size(); ++j) {
                    if (!placed[j] && items[j].name == d) {
                        depsOk = false;
                        break;
                    }
                }
                if (!depsOk) {
                    break;
                }
            }
            if (depsOk) {
                ordered.push_back(items[i]);
                placed[i] = true;
                ++progress;
            }
        }
        if (progress == 0) {
            // 环或依赖缺失: 剩余项附后 (加载时 checkDependencies 报错)
            for (size_t i = 0; i < items.size(); ++i) {
                if (!placed[i]) {
                    ordered.push_back(items[i]);
                    placed[i] = true;
                }
            }
            break;
        }
        placedCount += progress;
    }

    for (const auto& item : ordered) {
        co_await loadPluginAsync(item.path);
    }
}

std::vector<PluginManager::PluginListView> PluginManager::list() const {
    std::vector<PluginListView> out;
    out.reserve(plugins_.size());
    for (const auto& [name, inst] : plugins_) {
        std::vector<std::string> capNames;
        capNames.reserve(inst->capabilityRegistrations.size());
        for (const auto& c : inst->capabilityRegistrations) {
            capNames.push_back(c.name);
        }
        out.push_back(PluginListView{
            name,
            inst->version,
            inst->description,
            inst->path,
            inst->enabled,
            inst->inflight.load(std::memory_order_acquire),
            inst->toolNames,
            std::move(capNames),
            inst->depends,
            inst->optionalDepends,
        });
    }
    return out;
}

// ==================== 插件互查 (list_plugins / get_plugin) ====================

/// 单插件信息 → JSON (io 线程)
static neograph::json pluginInfoToJson(const PluginManager::PluginListView& p) {
    neograph::json j = neograph::json::object();
    j["name"]        = p.name;
    j["version"]     = p.version;
    j["description"] = p.description;
    j["path"]        = p.path;
    j["enabled"]     = p.enabled;
    j["inflight"]    = p.inflight;
    j["tools"]       = neograph::json::array();
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
    j["optional_depends"] = neograph::json::array();
    for (const auto& d : p.optionalDepends) {
        j["optional_depends"].push_back(d);
    }
    return j;
}

std::string PluginManager::listPluginsJson() {
    auto           pluginList = list();
    neograph::json arr        = neograph::json::array();
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
    std::vector<std::string> capNames;
    capNames.reserve(inst->capabilityRegistrations.size());
    for (const auto& c : inst->capabilityRegistrations) {
        capNames.push_back(c.name);
    }
    PluginListView p{
        inst->name,
        inst->version,
        inst->description,
        inst->path,
        inst->enabled,
        inst->inflight.load(std::memory_order_acquire),
        inst->toolNames,
        std::move(capNames),
        inst->depends,
        inst->optionalDepends,
    };
    return pluginInfoToJson(p).dump();
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
