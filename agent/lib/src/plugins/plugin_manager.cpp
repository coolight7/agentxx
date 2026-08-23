#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/agent/io/agent_io.h"
#include "agentxx/agent/io/agent_io_transport.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/plugin/plugin_common.h"
#include "agentxx/util/async_offload.h"
#include "agentxx/util/container_util.h"
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
    std::shared_ptr<agentxx::event::EventBus> bus;
    std::string                               topic; ///< 完整 topic (含 plugin. 前缀)
    size_t                                    subscriptionId      = 0;
    agentxx::plugin::PluginInstance*          inst                = nullptr;
    void (*handler)(AgentxxPluginStringView event_json, void* ud) = nullptr;
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
    auto it = caps_.find(name);
    if (it != caps_.end()) {
        XX_LOGW(
            "CapabilityRegistry: capability `{}` already registered by `{}`",
            name,
            it->second.provider
        );
        return false;
    }
    util::insertHeterogeneous(caps_, std::string{name}, Entry{std::string{provider}, invoke, ctx});
    XX_LOGI("CapabilityRegistry: `{}` registered by plugin `{}`", name, provider);
    return true;
}

bool CapabilityRegistry::unregisterCapability(std::string_view name, std::string_view provider) {
    auto it = caps_.find(name);
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
    return caps_.contains(name);
}

const CapabilityRegistry::Entry* CapabilityRegistry::get(std::string_view name) const {
    auto it = caps_.find(name);
    if (it == caps_.end()) {
        return nullptr;
    }
    return &it->second;
}

std::string CapabilityRegistry::providerOf(std::string_view name) const {
    auto it = caps_.find(name);
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
    std::weak_ptr<agentxx::agent::AgentContext> agentContext,
    std::shared_ptr<PluginInstance>             instance,
    AgentxxToolSpec                             spec
) :
    XXToolBase(
        std::string{spec.name.data ? spec.name.data : "", spec.name.size},
        std::move(agentContext),
        /*autoSummaryOutput=*/(spec.flags & AGENTXX_TOOL_FLAG_AUTO_SUMMARY) != 0,
        /*canDelayLoad=*/false, // 插件工具全量注册, 不延迟加载
        /*maxRetry=*/0
    ),
    name_{spec.name.data ? spec.name.data : "", spec.name.size},
    description_{spec.description.data ? spec.description.data : "", spec.description.size},
    parametersJson_{
        spec.parameters_json.data ? spec.parameters_json.data : "",
        spec.parameters_json.size
    },
    parameters_(neograph::json::object()),
    instance_(instance) {
    // 字符串字段已拷贝进成员 (不依赖插件侧内存存活); spec_ 指针指向本对象成员
    spec_                 = spec;
    spec_.name            = agentxx_plugin_sv(name_.data(), name_.size());
    spec_.description     = agentxx_plugin_sv(description_.data(), description_.size());
    spec_.parameters_json = agentxx_plugin_sv(parametersJson_.data(), parametersJson_.size());

    // 注册时解析一次参数 schema 并缓存 (ModelCallWrapNode 每轮组装工具定义,
    // 避免对同一工具反复 parse JSON)
    if (!parametersJson_.empty()) {
        try {
            auto params = neograph::json::parse(parametersJson_);
            if (params.is_object()) {
                parameters_ = std::move(params);
            }
        } catch (const std::exception& e) {
            XX_LOGW("PluginTool `{}`: invalid parameters_json: {}", name_, e.what());
        }
    }
}

neograph::ChatTool PluginTool::get_definition() const {
    neograph::ChatTool def;
    def.name        = name_;
    def.description = description_;
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

    // 参数: toolcall 分发路径已注入 session_id/tool_call_id; call_tool 路径由调用方提供
    std::string argsJson   = arguments.dump();
    std::string sessionId  = arguments.value("sessionId", std::string{});
    std::string toolCallId = arguments.value("tool_call_id", std::string{});

    // 取消令牌 (经 Session 按 session_id 取; 无会话时为空)
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
           inst      = std::move(inst),
           argsJson  = std::move(argsJson),
           sessionId = std::move(sessionId),
           toolCallId
           = std::move(toolCallId)](std::atomic<bool>& cancelFlag) -> asio::awaitable<std::string> {
        (void)cancelFlag; // 插件回调为黑盒, 无法协作式中止; 等待方取消后线程自然释放
        PluginInstance::InflightGuard guard(inst.get());
        std::string                   result;
        char*                         err = nullptr;
        char*                         out = spec.execute(
            spec.user_data,
            agentxx_plugin_sv(argsJson.data(), argsJson.size()),
            agentxx_plugin_sv(sessionId.data(), sessionId.size()),
            agentxx_plugin_sv(toolCallId.data(), toolCallId.size()),
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
        if (agentCtx && agentCtx->threadPool) {
            co_return co_await agentxx::util::offloadCancellableAsync<std::string>(
                *agentCtx->threadPool,
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
    instance_(instance) {} // 存弱引用: 与实例互不持有, 消除循环引用

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
    // 弱引用临时 lock: dispatch 期间实例保活 (实例已析构则跳过, 此时
    // 中间件必然已被摘除, 防御性检查)
    auto inst = instance_.lock();
    if (!inst || !inst->enabled) {
        co_return;
    }
    PluginInstance::InflightGuard guard(inst.get());

    // 节点输入摘要 JSON (观测用途; out_json 一期预留)
    neograph::json summary = neograph::json::object();
    summary["sessionId"]   = in.ctx.thread_id;
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
    int   rc  = hook.fn(
        hook.ud,
        point,
        agentxx_plugin_sv(summaryStr.data(), summaryStr.size()),
        &out,
        &err
    );
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
    for (const auto& dep :
         collectReverseRequiredDeps(plugins_, inst->name, /*onlyEnabled=*/false)) {
        auto depInst = find(dep);
        if (depInst && !depInst->unloadRequested) {
            shutdownPlugin(depInst);
        }
    }
    // 摘除全部注册 → 释放工具对象 → 摘除中间件
    detachAll(inst.get());
    inst->tools.clear();
    eraseMiddleware(inst->middleware.get());
    inst->middleware = nullptr;
    // unload 回调 (业务清理, 如引擎 join 内部线程); 宿主已自动反注册全部残留
    // - 内置插件无 dlHandle, 直接调用加载时保存的回调
    if (inst->dlHandle) {
        std::string err;
        auto        fn = reinterpret_cast<AgentxxPluginUnloadFn>(
            NativeLoader::sym(inst->dlHandle, AGENTXX_PLUGIN_SYMBOL_UNLOAD, err)
        );
        if (fn) {
            fn(inst->pluginCtx);
        }
    } else if (inst->builtinUnload) {
        inst->builtinUnload(inst->pluginCtx);
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

/// C ABI 边界异常兜底宏见 plugin_common.h (XX_PLUGIN_CATCH_*)

static int xx_register_tool(const AgentxxHost* host, const AgentxxToolSpec* spec) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !spec || agentxx_plugin_sv_empty(spec->name)) {
        return -1;
    }
    // io 线程约束操作: 非 io 线程调用 (JS 线程等) 经 post 同步等待
    // - spec 字符串字段为视图 (借用): ioCallSync 为同步等待, 调用方 (插件
    //   entry 等) 在等待期间不会释放内存; PluginTool 构造时已拷贝字符串,
    //   同步返回后 specCopy 即可废弃
    auto            mgrPtr   = mgr;
    auto            instPtr  = inst;
    AgentxxToolSpec specCopy = *spec;
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, specCopy]() {
        return mgrPtr->registerTool(instPtr, &specCopy);
    });
    XX_PLUGIN_CATCH_END(-1)
}

static int xx_unregister_tool(const AgentxxHost* host, AgentxxPluginStringView name) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || agentxx_plugin_sv_empty(name)) {
        return -1;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string toolName{name.data, name.size};
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
    const AgentxxHost*      host,
    AgentxxPluginStringView topic,
    void (*handler)(AgentxxPluginStringView event_json, void* ud),
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
    std::string topicStr{topic.data ? topic.data : "", topic.size};
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

static int xx_publish(
    const AgentxxHost*      host,
    AgentxxPluginStringView topic,
    AgentxxPluginStringView event_json
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return -1;
    }
    // 禁用插件不得发布事件 (与订阅侧 enabled 检查对称, 防停用插件继续外发)
    if (!inst->enabled) {
        XX_LOGW("Plugin `{}` publish ignored (disabled)", inst->name);
        return -1;
    }
    std::string topicStr{topic.data ? topic.data : "", topic.size};
    std::string payload{event_json.data ? event_json.data : "", event_json.size};
    return mgr->publish(topicStr.c_str(), payload.c_str());
    XX_PLUGIN_CATCH_END(-1)
}

static int xx_register_capability(const AgentxxHost* host, AgentxxPluginStringView capability) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || agentxx_plugin_sv_empty(capability)) {
        return -1;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string cap{capability.data, capability.size};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, cap]() {
        return mgrPtr->registerCapability(instPtr, cap.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

static int xx_unregister_capability(const AgentxxHost* host, AgentxxPluginStringView capability) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || agentxx_plugin_sv_empty(capability)) {
        return -1;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string cap{capability.data, capability.size};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, cap]() {
        return mgrPtr->unregisterCapability(instPtr, cap.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

static int xx_has_capability(const AgentxxHost* host, AgentxxPluginStringView capability) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = mgrOf(host);
    if (!mgr || agentxx_plugin_sv_empty(capability)) {
        return 0;
    }
    // caps_ 为 io 线程数据结构: 跨线程调用经 post 到 io 线程查询
    std::string cap{capability.data, capability.size};
    auto        mgrPtr = mgr;
    return ioCallSync<int>(mgrPtr, [mgrPtr, cap]() {
        return mgrPtr->hasCapability(cap.c_str()) ? 1 : 0;
    });
    XX_PLUGIN_CATCH_END(0)
}

static int xx_register_capability_ex(
    const AgentxxHost*        host,
    AgentxxPluginStringView   capability,
    AgentxxCapabilityInvokeFn invoke,
    void*                     ctx
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || agentxx_plugin_sv_empty(capability)) {
        return -1;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string cap{capability.data, capability.size};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, cap, invoke, ctx]() {
        return mgrPtr->registerCapabilityEx(instPtr, cap.c_str(), invoke, ctx);
    });
    XX_PLUGIN_CATCH_END(-1)
}

static char* xx_invoke_capability(
    const AgentxxHost*      host,
    AgentxxPluginStringView capability,
    AgentxxPluginStringView method,
    AgentxxPluginStringView args_json,
    char**                  error_out
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || agentxx_plugin_sv_empty(capability) || agentxx_plugin_sv_empty(method)) {
        return nullptr;
    }
    std::string cap{capability.data, capability.size};
    std::string m{method.data, method.size};
    std::string args{args_json.data ? args_json.data : "", args_json.size};
    if (args.empty()) {
        args = "{}";
    }
    // 查表在 io 线程 (invokeCapability 内部), 提供者回调在调用方线程执行
    return mgr->invokeCapability(inst, cap.c_str(), m.c_str(), args.c_str(), error_out);
    XX_PLUGIN_CATCH_END(nullptr)
}

static char* xx_call_tool(
    const AgentxxHost*      host,
    AgentxxPluginStringView name,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView session_id,
    char**                  error_out
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
    std::string toolName{name.data ? name.data : "", name.size};
    std::string args{args_json.data ? args_json.data : "", args_json.size};
    std::string tid{session_id.data ? session_id.data : "", session_id.size};
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
        parsed["sessionId"]    = tid;
        parsed["tool_call_id"] = fmt::format("plugin_call_{}", ++g_pluginCallSeq);

        const auto& spec    = pluginTool->spec();
        char*       err     = nullptr;
        std::string argsStr = parsed.dump();
        char*       out     = spec.execute(
            spec.user_data,
            agentxx_plugin_sv(argsStr.data(), argsStr.size()),
            agentxx_plugin_sv(tid.data(), tid.size()),
            agentxx_plugin_sv("", 0),
            &err
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

static char* xx_get_plugin(const AgentxxHost* host, AgentxxPluginStringView name) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = mgrOf(host);
    if (!mgr || agentxx_plugin_sv_empty(name)) {
        return nullptr;
    }
    auto        mgrPtr = mgr;
    std::string pluginName{name.data, name.size};
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

static char*
    xx_get_share_store(const AgentxxHost* host, AgentxxPluginStringView session_id, long long id) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    // shareStore 仅 io 线程访问 (无锁模型): 跨线程经 post 同步等待
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string tid{session_id.data ? session_id.data : "", session_id.size};
    return ioCallSync<char*>(mgrPtr, [mgrPtr, instPtr, tid, id]() {
        return mgrPtr->getShareStore(instPtr, tid.c_str(), id);
    });
    XX_PLUGIN_CATCH_END(nullptr)
}

static void xx_emit_message_tip(
    const AgentxxHost*      host,
    AgentxxPluginStringView session_id,
    AgentxxPluginStringView text,
    int                     level
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string tid{session_id.data ? session_id.data : "", session_id.size};
    std::string msg{text.data ? text.data : "", text.size};
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

static void xx_log(const AgentxxHost* host, int level, AgentxxPluginStringView msg) {
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
    agentxx::util::xxLogPrint(lv, std::string{msg.data ? msg.data : "", msg.size});
}

/// JSON 辅助: 提取字符串字段 (线程安全, 纯函数; 供插件替代手写 JSON 解析)
static char* xx_json_get_string(
    const AgentxxHost*      host,
    AgentxxPluginStringView json,
    AgentxxPluginStringView key
) {
    auto inst = instOf(host);
    if (!inst || agentxx_plugin_sv_empty(json) || agentxx_plugin_sv_empty(key)) {
        return nullptr;
    }
    try {
        std::string jsonStr{json.data, json.size};
        std::string keyStr{key.data, key.size};
        auto        j = neograph::json::parse(jsonStr);
        if (j.is_object() && j.contains(keyStr) && j[keyStr].is_string()) {
            return inst->host.vtable->strdup(j[keyStr].get<std::string>().c_str());
        }
    } catch (const std::exception&) {
        // JSON 非法: 视为无此字段
    }
    return nullptr;
}

/// JSON 辅助: 字符串 → JSON 字符串字面量 (含引号与转义; 供插件拼 JSON 时转义值)
static char* xx_json_escape(const AgentxxHost* host, AgentxxPluginStringView s) {
    auto inst = instOf(host);
    if (!inst || agentxx_plugin_sv_empty(s)) {
        return nullptr;
    }
    std::string out;
    out.reserve(s.size + 2);
    out += '"';
    for (size_t i = 0; i < s.size; ++i) {
        const unsigned char c = static_cast<unsigned char>(s.data[i]);
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

/// 宿主 AgentConfig 关键字段 → JSON (io 线程; 供插件装配期读取)
static char* xx_get_config(const AgentxxHost* host) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = mgrOf(host);
    if (!mgr) {
        return nullptr;
    }
    auto mgrPtr = mgr;
    auto json   = ioCallSync<std::string>(mgrPtr, [mgrPtr]() {
        return mgrPtr->getConfigJson();
    });
    if (json.empty()) {
        return nullptr;
    }
    return host->vtable->strdup(json.c_str());
    XX_PLUGIN_CATCH_END(nullptr)
}

/// 本插件配置参数 → JSON (io 线程; 未配置返回 "{}")
static char* xx_get_plugin_args(const AgentxxHost* host) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    auto mgrPtr  = mgr;
    auto instPtr = inst;
    auto json    = ioCallSync<std::string>(mgrPtr, [mgrPtr, instPtr]() {
        return mgrPtr->getPluginArgsJson(instPtr);
    });
    return host->vtable->strdup(json.c_str());
    XX_PLUGIN_CATCH_END(nullptr)
}

/// 宿主 toolPrompt 配置 → JSON (io 线程; 未配置返回 NULL)
static char* xx_get_tool_prompt(const AgentxxHost* host, AgentxxPluginStringView tool_name) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = mgrOf(host);
    if (!mgr || agentxx_plugin_sv_empty(tool_name)) {
        return nullptr;
    }
    auto        mgrPtr = mgr;
    std::string name{tool_name.data, tool_name.size};
    auto        json = ioCallSync<std::string>(mgrPtr, [mgrPtr, name]() {
        return mgrPtr->getToolPromptJson(name);
    });
    if (json.empty()) {
        return nullptr;
    }
    return host->vtable->strdup(json.c_str());
    XX_PLUGIN_CATCH_END(nullptr)
}

/// 宿主完整提示词 → JSON (io 线程; 未装配 AgentConfig 返回 NULL)
static char* xx_get_prompt(const AgentxxHost* host) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = mgrOf(host);
    if (!mgr) {
        return nullptr;
    }
    auto mgrPtr = mgr;
    auto json   = ioCallSync<std::string>(mgrPtr, [mgrPtr]() {
        return mgrPtr->getPromptJson();
    });
    if (json.empty()) {
        return nullptr;
    }
    return host->vtable->strdup(json.c_str());
    XX_PLUGIN_CATCH_END(nullptr)
}

/// 合并更新宿主提示词 (io 线程; 插件卸载时自动回滚, 见 PluginManager::setPromptJson)
static int xx_set_prompt(const AgentxxHost* host, AgentxxPluginStringView prompt_json) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || agentxx_plugin_sv_empty(prompt_json)) {
        return -1;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string json{prompt_json.data, prompt_json.size};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, json]() {
        return mgrPtr->setPromptJson(instPtr, json.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

/// 周期定时器 (io 线程约束; 跨线程经 post 同步等待; 回调内快速返回约定)
static void*
    xx_add_timer(const AgentxxHost* host, long interval_ms, void (*fn)(void* ud), void* ud) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || interval_ms <= 0 || !fn) {
        return nullptr;
    }
    auto mgrPtr  = mgr;
    auto instPtr = inst;
    return ioCallSync<void*>(mgrPtr, [mgrPtr, instPtr, interval_ms, fn, ud]() {
        return mgrPtr->addTimer(instPtr, interval_ms, fn, ud);
    });
    XX_PLUGIN_CATCH_END(nullptr)
}

static void xx_cancel_timer(const AgentxxHost* host, void* timer) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !timer) {
        return;
    }
    auto mgrPtr  = mgr;
    auto instPtr = inst;
    ioCallSyncVoid(mgrPtr, [mgrPtr, instPtr, timer]() {
        mgrPtr->cancelTimer(instPtr, timer);
    });
    XX_PLUGIN_CATCH_END_VOID()
}

/// 阻塞池卸载执行 (任意线程可调用; work/done 期间插件代码段保活)
static void xx_offload(
    const AgentxxHost* host,
    void* (*work)(void* ud, char** error_out),
    void (*done)(void* ud, void* result, char* error),
    void* ud
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !work) {
        return;
    }
    mgr->offload(inst, work, done, ud);
    XX_PLUGIN_CATCH_END_VOID()
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
    xx_get_config,
    xx_get_plugin_args,
    xx_get_tool_prompt,
    xx_get_prompt,
    xx_set_prompt,
    xx_add_timer,
    xx_cancel_timer,
    xx_offload,
};

// ==================== 工具注册/注销 ====================

int PluginManager::registerTool(PluginInstance* inst, const AgentxxToolSpec* spec) {
    auto shared = inst->self.lock();
    if (!shared) {
        return -1;
    }
    auto tool = std::make_shared<PluginTool>(agentContext_, std::move(shared), *spec);
    if (!registry_->registerTool(tool->get_name(), tool)) {
        XX_LOGW("Plugin `{}` register tool `{}` failed (conflict?)", inst->name, tool->get_name());
        return -1;
    }
    inst->toolNames.push_back(tool->get_name());
    inst->tools.push_back(std::move(tool));
    XX_LOGI(
        "Plugin `{}` registered tool `{}`",
        inst->name,
        std::string_view{spec->name.data ? spec->name.data : "", spec->name.size}
    );
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
    void (*handler)(AgentxxPluginStringView event_json, void* ud),
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
                sub->handler(agentxx_plugin_sv(data.data(), data.size()), sub->ud);
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
    return entry.invoke(
        entry.ctx,
        caller ? &caller->host : nullptr,
        agentxx_plugin_sv_cstr(method),
        agentxx_plugin_sv_cstr(args_json),
        error_out
    );
}

// ==================== 会话/上下文访问 ====================

char* PluginManager::getShareStore(PluginInstance* inst, const char* session_id, long long id) {
    if (!inst || !session_id) {
        return nullptr;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        return nullptr;
    }
    auto value
        = ctx->middlewareHandleContext->getShareStoreItemValue(session_id, static_cast<size_t>(id));
    if (!value) {
        return nullptr;
    }
    return inst->host.vtable->strdup(value->c_str());
}

void PluginManager::emitMessageTip(
    PluginInstance* inst,
    const char*     session_id,
    const char*     text,
    int             level
) {
    if (!inst || !session_id || !text) {
        return;
    }
    auto ctx = agentContext_.lock();
    if (!ctx) {
        return;
    }
    auto session = ctx->getSession(session_id);
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

// ==================== 宿主任务调度 (vtable add_timer/cancel_timer/offload) ====================

namespace {

/// 周期定时器触发循环 (静态函数 + bind 自持有 state; 避免 std::function
/// 自引用导致悬垂/循环引用):
/// - async_wait 完成(超时) → 触发回调 → 重新 expires_after + async_wait
/// - 取消/错误 → 不再重新排程 → handler 链终结, state 随之释放
/// - 回调执行期间 InflightGuard 保活插件代码段 (unload 等计数归零后才 dlclose)
void pluginTimerLoop(
    const neograph_asio_error_code&     ec,
    const std::shared_ptr<PluginTimer>& state
) {
    if (ec || state->cancelled || !state->inst) {
        return; // 取消 (operation_aborted) 或宿主销毁
    }
    PluginInstance* inst = state->inst;
    {
        PluginInstance::InflightGuard guard(inst);
        if (state->fn && !state->cancelled) {
            state->fn(state->ud); // io 线程; 快速返回约定
        }
    }
    // 重新排程
    state->timer->expires_after(std::chrono::milliseconds(state->intervalMs));
    state->timer->async_wait(std::bind(&pluginTimerLoop, std::placeholders::_1, state));
}

} // namespace

void* PluginManager::addTimer(
    PluginInstance* inst,
    long            intervalMs,
    void (*fn)(void* ud),
    void* ud
) {
    if (!inst || !fn || intervalMs <= 0) {
        return nullptr;
    }
    if (!ioExecutor_) {
        XX_LOGW("Plugin `{}` add_timer: no io executor", inst->name);
        return nullptr;
    }
    auto state        = std::make_shared<PluginTimer>();
    state->inst       = inst;
    state->intervalMs = intervalMs;
    state->fn         = fn;
    state->ud         = ud;
    state->timer      = std::make_shared<asio::steady_timer>(ioExecutor_);

    state->timer->expires_after(std::chrono::milliseconds(intervalMs));
    state->timer->async_wait(std::bind(&pluginTimerLoop, std::placeholders::_1, state));

    inst->timers.push_back(state);
    return state.get();
}

void PluginManager::cancelTimer(PluginInstance* inst, void* timer) {
    if (!inst || !timer) {
        return;
    }
    auto it = std::find_if(
        inst->timers.begin(),
        inst->timers.end(),
        [timer](const std::shared_ptr<PluginTimer>& t) {
            return t.get() == timer;
        }
    );
    if (it == inst->timers.end()) {
        XX_LOGW("Plugin `{}` cancel_timer: handle not owned", inst->name);
        return;
    }
    (*it)->cancelled = true;
    // 本 asio 版本 timer::cancel() 仅无参形式 (可能抛异常), 调用处非析构, 捕获吞掉
    try {
        (*it)->timer->cancel(); // 中断在途 async_wait (handler 以 aborted 到达退出)
    } catch (...) {
    }
    inst->timers.erase(it); // 释放一侧持有; 在途 handler 链自持有到终结
}

void PluginManager::offload(
    PluginInstance* inst,
    void* (*work)(void* ud, char** error_out),
    void (*done)(void* ud, void* result, char* error),
    void* ud
) {
    if (!inst || !work) {
        return;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->threadPool) {
        XX_LOGW("Plugin `{}` offload: blocking pool not ready", inst->name);
        return;
    }
    // inflight 计数贯穿 work+done: 卸载流程等计数归零后才调 unload 回调,
    // 保证 work/done 执行期间插件代码段存活 (与 PluginTool 线程池路径一致)
    // - fetch_add 一次 (offload 入口), fetch_sub 一次 (done 执行完毕后),
    //   done 执行期间计数保持 >0 保活代码段; 不得再用 RAII guard (重复递减)
    inst->inflight.fetch_add(1, std::memory_order_acq_rel);
    auto ex = ioExecutor_;
    asio::post(*ctx->threadPool, [inst, work, done, ud, ex]() {
        // ---- 阻塞池线程: 执行 work ----
        char* error  = nullptr;
        void* result = work(ud, &error);
        // ---- 投递回 io 线程执行 done (快速返回约定) ----
        asio::post(ex, [inst, done, ud, result, error]() {
            if (done) {
                done(ud, result, error);
            }
            inst->inflight.fetch_sub(1, std::memory_order_acq_rel);
        });
    });
}

// ==================== 生命周期 ====================

// =====================================================================
// 内置插件注册表 (可选合并编译进 libagentxx, 见 builtin_plugin.h)
// =====================================================================

/// 查询内置插件 (编译进 libagentxx 的插件; 未内置返回 nullptr)
/// - 跳过 name 为空的占位条目 (空表时 agentxx_get_builtin_plugins 返回占位)
static const AgentxxBuiltinPluginInfo* findBuiltinPlugin(std::string_view name) {
    size_t                          count = 0;
    const AgentxxBuiltinPluginInfo* arr   = agentxx_get_builtin_plugins(&count);
    for (size_t i = 0; i < count; ++i) {
        if (arr[i].name && name == arr[i].name) {
            return &arr[i];
        }
    }
    return nullptr;
}

asio::awaitable<std::shared_ptr<PluginInstance>>
PluginManager::loadNativeAsync(
    std::string                         path,
    const agentxx::agent::PluginConfig* cfg,
    bool                                allowClientOnlySkip
) {
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->threadPool) {
        XX_LOGE("PluginManager: agent context not ready");
        co_return nullptr;
    }

    // dlopen 卸载到线程池 (避免阻塞 io 线程)
    std::string dlErr;
    void*       handle = co_await agentxx::util::offloadAsync<void*>(
        *ctx->threadPool,
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
            name    = std::string{info->name.data ? info->name.data : "", info->name.size};
            version = std::string{info->version.data ? info->version.data : "", info->version.size};
            desc    = std::string{
                info->description.data ? info->description.data : "",
                info->description.size
            };
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
        if (allowClientOnlySkip) {
            // sides==Auto: 无 agent 入口视为纯 client 插件, 跳过并警告
            // (与 client 侧 Auto 无 client 入口静默跳过对称; 显式
            // sides==agent 的加载缺失入口仍为错误 —— 配置写明了期望)
            XX_LOGW(
                "Plugin `{}` has no agent entry `{}`, skipped on agent side "
                "(client-only plugin? check sides config)",
                path,
                AGENTXX_PLUGIN_SYMBOL_ENTRY
            );
        } else {
            XX_LOGE(
                "Plugin `{}` missing entry symbol `{}`: {}",
                path,
                AGENTXX_PLUGIN_SYMBOL_ENTRY,
                err
            );
        }
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

    util::insertOrAssignHeterogeneous(plugins_, name, inst);

    // 插件配置参数 (yaml `plugins` 条目 args) 随加载直接传入 (C2):
    // - 宿主不解析字段语义, 插件经 vtable get_plugin_args 整体读取
    // - 必须【在 entry 调用之前】写入 inst->args —— 插件 entry 装配期
    //   (readHostConfig) 即经 get_plugin_args 读取 args; 若延迟到 entry
    //   之后赋值, 插件读到的是空 {} (回退默认配置, 如 codegraph 的
    //   load_cwd=true / loadPaths 丢失), 动态库插件的 yaml 配置参数
    //   全部不生效 (回归: codegraph 测试传 paths 索引临时项目失败)
    // - 不再事后按"配置路径推导名 == 插件名"回查: manifest name 与目录/
    //   文件名不一致时也能正确拿到 args (直接加载路径 cfg 为 nullptr → {})
    if (cfg) {
        inst->args = cfg->args;
    }

    // entry 调用卸载到线程池 (不阻塞 io 线程):
    // - 插件 entry 内的注册动作 (register_tool/hook/capability) 经 vtable
    //   ioCallSync 回 io 线程执行 (契约不变: 注册始终在 io 线程串行)
    // - 关键: 脚本插件的 entry 会经 invoke_capability 同步等待 JS 线程
    //   加载脚本, 而 JS 线程内脚本注册又要回 io 线程 —— 若 entry 在 io
    //   线程执行则 io↔引擎互等死锁 (见 plugins.md 11.5.2); 线程池执行时
    //   io 线程保持空闲, 可服务注册回调
    int rc = co_await agentxx::util::offloadAsync<int>(
        *ctx->threadPool,
        [inst, entry]() -> asio::awaitable<int> {
            co_return entry(&inst->host, &inst->pluginCtx);
        }
    );
    if (rc != 0) {
        XX_LOGE("Plugin `{}` entry returned {}", name, rc);
        detachAll(inst.get());
        // 加载失败清理 (A4): 已注册的中间件必须摘除/延迟摘除 —— 中间件持有
        // 实例弱引用, 若其仍挂 handles 栈, 实例无法析构 → dlHandle 不释放。
        // - 无轮次执行: 立即摘除 (erase 安全), 实例析构 → dlclose
        // - 轮次执行中: 已由 detachAll 置 disabled + 登记待轮末摘除
        //   (弱引用记录, flush 不依赖实例存活)
        if (!hasRunningTurn()) {
            eraseMiddleware(inst->middleware.get());
            inst->middleware = nullptr;
        }
        util::eraseHeterogeneous(plugins_, name);
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

asio::awaitable<std::shared_ptr<PluginInstance>> PluginManager::loadBuiltinAsync(
    std::string                         name,
    std::string                         path,
    std::vector<std::string>            depends,
    std::vector<std::string>            optionalDepends,
    const agentxx::agent::PluginConfig* cfg
) {
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->threadPool) {
        XX_LOGE("PluginManager: agent context not ready");
        co_return nullptr;
    }

    // 内置注册表查找 (编译进 libagentxx 的插件; 无 dlopen)
    const AgentxxBuiltinPluginInfo* entry = findBuiltinPlugin(name);
    if (!entry || !entry->entry) {
        XX_LOGE(
            "Builtin plugin `{}` not found (not merged into libagentxx; "
            "rebuild with AGENTXX_ENABLE_PLUGIN_BUILTIN=ON and this plugin enabled)",
            name
        );
        co_return nullptr;
    }
    if (plugins_.contains(name)) {
        XX_LOGE("Plugin `{}` already loaded", name);
        co_return nullptr;
    }

    // 元信息 (内置插件直接调用 get_info, 与 loadNativeAsync 的 dlsym 可选符号同语义)
    std::string version;
    std::string desc;
    if (entry->get_info) {
        auto info = entry->get_info();
        if (info && info->api_version != AGENTXX_PLUGIN_API_VERSION) {
            XX_LOGE(
                "Plugin `{}` api_version {} mismatch (host expects {})",
                name,
                info->api_version,
                AGENTXX_PLUGIN_API_VERSION
            );
            co_return nullptr;
        }
        if (info) {
            version = std::string{info->version.data ? info->version.data : "", info->version.size};
            desc    = std::string{
                info->description.data ? info->description.data : "",
                info->description.size
            };
        }
    }

    auto inst         = std::make_shared<PluginInstance>(name);
    inst->version     = std::move(version);
    inst->description = std::move(desc);
    // path 传 manifest 入口文件路径 (与动态加载同形态, 供插件 get_own_info
    // 按"库路径所在目录"推导资源文件, 如 example_js 壳的同目录 plugin.js;
    // 传配置目录会误推导到上一级, 见 loadPluginAsync 内置回退)
    inst->path            = std::move(path);
    inst->dlHandle        = nullptr; // 内置插件无动态库句柄
    inst->builtinUnload   = entry->unload;
    inst->depends         = std::move(depends);
    inst->optionalDepends = std::move(optionalDepends);
    inst->self            = inst;
    inst->manager         = shared_from_this();
    inst->host.vtable     = &g_hostVtable;
    inst->host.opaque     = inst.get();

    util::insertOrAssignHeterogeneous(plugins_, name, inst);

    // 插件配置参数随加载直接传入 (同 loadNativeAsync, 见 C2): 在 entry
    // 调用【之前】写入 inst->args —— 插件 entry 装配期经 get_plugin_args
    // 读取 args (与 loadNativeAsync 修复同因)
    if (cfg) {
        inst->args = cfg->args;
    }

    // entry 调用卸载到线程池 (与 loadNativeAsync 相同: 注册动作经 vtable
    // ioCallSync 回 io 线程; 脚本插件的 entry 会经 invoke_capability 同步
    // 等待引擎线程, entry 在 io 线程执行会 io↔引擎互等死锁)
    int rc = co_await agentxx::util::offloadAsync<int>(
        *ctx->threadPool,
        [inst, entry]() -> asio::awaitable<int> {
            co_return entry->entry(&inst->host, &inst->pluginCtx);
        }
    );
    if (rc != 0) {
        XX_LOGE("Plugin `{}` entry returned {}", name, rc);
        detachAll(inst.get());
        // 与 loadNativeAsync 失败路径一致: 已注册中间件必须摘除/延迟摘除
        if (!hasRunningTurn()) {
            eraseMiddleware(inst->middleware.get());
            inst->middleware = nullptr;
        }
        util::eraseHeterogeneous(plugins_, name);
        co_return nullptr;
    }

    XX_LOGI(
        "Builtin plugin loaded: {} v{} ({} tools, {} hooks, {} capabilities)",
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
    // 指数退避轮询 (10ms → 1s 上限): 慢回调等待期间减少 io 线程定时器唤醒
    auto backoff = std::chrono::milliseconds(10);
    while (inst->inflight.load(std::memory_order_acquire) > 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            XX_LOGW(
                "Plugin `{}` unload: inflight not zero within {}ms, abort unload",
                inst->name,
                timeout.count()
            );
            co_return false;
        }
        timer.expires_after(backoff);
        co_await timer.async_wait(asio::use_awaitable);
        backoff = std::min(backoff * 2, std::chrono::milliseconds(1000));
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
    // - 待摘除记录存中间件弱引用: 加载失败路径实例随后从插件表移除,
    //   flush 时仍能按弱引用定位并摘除, 不依赖实例存活 (摘除后中间件
    //   不再持有实例, 实例自然析构 → dlclose, 无循环引用泄漏)
    if (inst->middleware) {
        inst->middleware->disabled = true;
        for (int p = 0; p < AGENTXX_HOOK_COUNT; ++p) {
            inst->middleware->clearHook(static_cast<AgentxxHookPoint>(p));
        }
        addPendingCleanup(inst);
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
    // 定时器: 统一取消 (vtable add_timer 登记; 在途 async_wait 以 aborted 退出,
    // 回调不再触发; 在途 offload 由 inflight 计数等待, 此处无需处理)
    for (const auto& t : inst->timers) {
        t->cancelled = true;
        try {
            t->timer->cancel(); // 本 asio 版本仅无参形式 (可能抛异常)
        } catch (...) {
        }
    }
    inst->timers.clear();
    // 提示词: 回滚插件加载期间经 set_prompt 写入的修改 (恢复加载前状态)
    // - detachAll 仅被卸载路径调用 (unloadAsync/shutdownPlugin/entry 失败清理),
    //   disable 不经过此路径 (禁用时提示词条目保留, enable 后仍可用)
    restorePromptBackup(inst);
}

void PluginManager::addPendingCleanup(const PluginInstance* inst) {
    if (!inst || !inst->middleware) {
        return;
    }
    if (std::find_if(
            pendingCleanup_.begin(),
            pendingCleanup_.end(),
            [&](const PendingMiddlewareCleanup& p) {
                return p.name == inst->name;
            }
        )
        == pendingCleanup_.end()) {
        pendingCleanup_.push_back(PendingMiddlewareCleanup{inst->name, inst->middleware});
    }
}

void PluginManager::eraseMiddleware(PluginMiddlewareHandle* mw) {
    if (!mw) {
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
            [mw](const std::shared_ptr<agentxx::middleware::BaseMiddlewareHandleInterface>& h) {
                return h.get() == mw;
            }
        ),
        handles.end()
    );
}

void PluginManager::flushPendingCleanup() {
    if (pendingCleanup_.empty()) {
        return;
    }
    auto pending = std::move(pendingCleanup_);
    pendingCleanup_.clear();
    for (const auto& p : pending) {
        auto mw = p.middleware.lock();
        if (!mw) {
            continue; // 已摘除/实例已释放
        }
        eraseMiddleware(mw.get());
        // 实例可能已从插件表移除 (加载失败路径): 仅当实例仍存活时断
        // instance->middleware 引用 (enable 重建逻辑依赖其为空)
        if (auto inst = find(p.name); inst && inst->middleware.get() == mw.get()) {
            inst->middleware = nullptr;
        }
        XX_LOGI("Plugin `{}` middleware removed from stack", p.name);
    }
}

asio::awaitable<bool> PluginManager::unloadAsync(std::string_view name) {
    auto it = plugins_.find(name);
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
    for (const auto& dep : collectReverseRequiredDeps(plugins_, inst->name, /*onlyEnabled=*/true)) {
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
    eraseMiddleware(inst->middleware.get());
    inst->middleware = nullptr;
    // 从待轮末清理列表移除 (已立即摘除)
    pendingCleanup_.erase(
        std::remove_if(
            pendingCleanup_.begin(),
            pendingCleanup_.end(),
            [&](const PendingMiddlewareCleanup& p) {
                return p.name == inst->name;
            }
        ),
        pendingCleanup_.end()
    );

    // 插件 unload 回调 (业务清理; 宿主已自动反注册全部残留; 内置插件无
    // dlHandle, 直接调用加载时保存的回调, 不 dlsym)
    if (inst->dlHandle) {
        std::string err;
        auto        fn = reinterpret_cast<AgentxxPluginUnloadFn>(
            NativeLoader::sym(inst->dlHandle, AGENTXX_PLUGIN_SYMBOL_UNLOAD, err)
        );
        if (fn) {
            fn(inst->pluginCtx);
        }
    } else if (inst->builtinUnload) {
        inst->builtinUnload(inst->pluginCtx);
    }
    plugins_.erase(it); // 析构 → dlclose (inst 局部 shared_ptr 保活到函数结束)
    XX_LOGI("Plugin unloaded: {}", inst->name);
    co_return true;
}

/// 收集必选依赖 target 的插件名 → 公共 collectReverseRequiredDeps (plugin_common.h)

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
    for (const auto& dep : collectReverseRequiredDeps(plugins_, inst->name, /*onlyEnabled=*/true)) {
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
            addPendingCleanup(inst.get());
        } else {
            // 无轮次执行: 立即摘除 (无执行中遍历, erase 安全)
            eraseMiddleware(inst->middleware.get());
            inst->middleware = nullptr;
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
            std::remove_if(
                pendingCleanup_.begin(),
                pendingCleanup_.end(),
                [&](const PendingMiddlewareCleanup& p) {
                    return p.name == inst->name;
                }
            ),
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
    for (const auto& dep :
         collectReverseRequiredDeps(plugins_, inst->name, /*onlyEnabled=*/false)) {
        auto depInst = find(dep);
        if (depInst && !depInst->enabled && !depInst->userDisabled) {
            XX_LOGI("Enable `{}` cascades enable of dependent plugin `{}`", inst->name, dep);
            enableImpl(dep, /*userInitiated=*/false);
        }
    }
    XX_LOGI("Plugin enabled: {}", inst->name);
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

asio::awaitable<std::shared_ptr<PluginInstance>>
    PluginManager::loadPluginAsync(
    std::string                         path,
    const agentxx::agent::PluginConfig* cfg,
    bool                                allowClientOnlySkip
) {
    namespace fs = std::filesystem;

    // 显式内置路径 (builtin://<插件名>): 直接从内置注册表加载, 不解析文件
    // - 常规内置用法仍推荐插件目录路径 (依赖 plugin.yaml 推导 depends/资源);
    //   本形态适合无资源需求的插件或测试直连
    constexpr std::string_view kBuiltinScheme = "builtin://";
    if (path.starts_with(kBuiltinScheme)) {
        auto name = path.substr(kBuiltinScheme.size());
        if (name.empty()) {
            XX_LOGE("Plugin path `{}`: empty builtin name", path);
            co_return nullptr;
        }
        co_return co_await loadBuiltinAsync(std::string{name}, std::move(path), {}, {}, cfg);
    }

    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        // ---- 插件目录: 按 plugin.yaml 清单分派 ----
        std::string              name, entry;
        std::vector<std::string> depends, optionalDepends;
        if (!parsePluginManifest(fs::path(path), name, entry, depends, optionalDepends)) {
            XX_LOGW("Plugin dir `{}` manifest invalid or missing", path);
            co_return nullptr;
        }
        // 依赖检查 (必选缺失/可选警告/环检测)
        if (!checkDependencies(name, depends, optionalDepends)) {
            co_return nullptr;
        }
        // 所有插件统一为 C++ 插件: entry 总是指向动态库
        // (脚本能力由插件内部经能力调用委派给 interpreter 引擎, 宿主不参与)
        // - entry 平台化 + 多配置生成器 (MSVC Debug/Release) 配置子目录回退
        //   见公共 resolvePluginEntryPath (plugin_common.h)
        auto            entryPath = resolvePluginEntryPath(fs::path(path), entry);
        std::error_code ec2;
        if (fs::exists(entryPath, ec2)) {
            auto inst = co_await loadNativeAsync(std::move(entryPath), cfg, allowClientOnlySkip);
            if (inst) {
                inst->depends         = std::move(depends);
                inst->optionalDepends = std::move(optionalDepends);
            }
            co_return inst;
        }
        // 入口文件缺失: 内置模式 (AGENTXX_ENABLE_PLUGIN_BUILTIN=ON) 下插件
        // 编译进 libagentxx, 无动态库文件 → 回退内置注册表
        // (depends/optionalDepends 已由 plugin.yaml 解析, 级联卸载/拓扑排序
        // 与动态加载完全一致; inst->path 传 manifest 入口文件路径 (与动态加载
        // 同形态) —— 插件侧按"库路径所在目录"推导资源 (如 example_js 壳的
        // dirOf 取同目录 plugin.js), 传目录会误推导到上一级)
        if (findBuiltinPlugin(name)) {
            XX_LOGI(
                "Plugin `{}` entry file not found, fallback to builtin "
                "(merged into libagentxx)",
                name
            );
            co_return co_await loadBuiltinAsync(
                name,
                std::move(entryPath),
                std::move(depends),
                std::move(optionalDepends),
                cfg
            );
        }
        // 非内置模式: 保持原行为 (loadNativeAsync 报告 dlopen 失败)
        co_return co_await loadNativeAsync(std::move(entryPath), cfg, allowClientOnlySkip);
    }

    // ---- 文件: 视为原生库路径 ----
    // 文件不存在且同名插件已内置 → 回退内置注册表 (支持直接写库路径的配置形态;
    // 文件存在但 dlopen 失败则保留原错误, 不回退, 避免掩盖真实故障)
    std::error_code ec3;
    if (!fs::exists(path, ec3)) {
        auto builtinName = pluginNameFromPath(path);
        if (findBuiltinPlugin(builtinName)) {
            XX_LOGI("Plugin file `{}` not found, fallback to builtin `{}`", path, builtinName);
            co_return co_await loadBuiltinAsync(builtinName, std::move(path), {}, {}, cfg);
        }
    }
    co_return co_await loadNativeAsync(std::move(path), cfg, allowClientOnlySkip);
}

asio::awaitable<void>
    PluginManager::loadConfiguredPlugins(const std::vector<agentxx::agent::PluginConfig>& plugins) {
    namespace fs = std::filesystem;

    // 预解析各配置项依赖 (目录插件读 plugin.yaml depends; 库路径按文件名
    // 推导插件名参与排序 —— libagentxx_javascript_engine.so → agentxx_javascript_engine)
    // - sides == Client 的配置项仅属 client 侧, agent 侧跳过 (A7)
    // - cfg 指针指向入参 vector 元素, 生命周期覆盖本函数 (co_await 挂起时
    //   入参仍存活)
    struct Item {
        std::string                         path;
        std::string                         name; ///< 空 = 无法推导 (不影响排序)
        std::vector<std::string>            depends;
        const agentxx::agent::PluginConfig* cfg = nullptr;
    };

    std::vector<Item> items;
    for (const auto& cfg : plugins) {
        if (!cfg.enabled) {
            continue;
        }
        if (cfg.sides == agentxx::agent::PluginSide::Client) {
            XX_LOGI("[Config] plugin `{}` sides=client, skip on agent side", cfg.path);
            continue;
        }
        // 所有插件统一经 path 外置指定 (必填; 不区分内置/外置插件)
        std::error_code ec;
        if (fs::is_directory(cfg.path, ec)) {
            std::string              name, entry;
            std::vector<std::string> depends, optionalDepends;
            if (parsePluginManifest(fs::path(cfg.path), name, entry, depends, optionalDepends)) {
                items.push_back(Item{cfg.path, name, depends, &cfg});
                continue;
            }
        }
        items.push_back(Item{cfg.path, pluginNameFromPath(cfg.path), {}, &cfg});
    }

    // 拓扑排序 (Kahn): 依赖者排在被依赖者之后, 避免配置顺序导致必选依赖缺失
    // (公共 topoSortPlugins, plugin_common.h; 无进展项附后由加载路径报错)
    auto ordered = topoSortPlugins(std::move(items));

    for (const auto& item : ordered) {
        // sides==Auto 的配置项: 无 agent 入口时视为纯 client 插件跳过并警告
        // (显式 sides==agent 缺入口仍为错误; sides==client 已在上方过滤)
        bool allowClientOnly
            = item.cfg && item.cfg->sides == agentxx::agent::PluginSide::Auto;
        co_await loadPluginAsync(item.path, item.cfg, allowClientOnly);
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

// ==================== 宿主配置访问 (vtable get_config/get_tool_prompt) ====================

std::string PluginManager::getConfigJson() {
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->agentConfig) {
        return {};
    }
    const auto&    cfg = *ctx->agentConfig;
    neograph::json j   = neograph::json::object();
    j["dataDir"]       = cfg.dataDir;
    // 当前工作路径 (插件索引项目根用; 失败为空串, 插件自行回退)
    j["projectRoot"] = agentxx::agent::AgentConfigStatic::getCurrentWorkPath();
#if XX_IS_WIN_D
    j["platform"] = "windows";
#elif XX_IS_MACOS_D
    j["platform"] = "macos";
#else
    j["platform"] = "linux";
#endif
    return j.dump();
}

std::string PluginManager::getToolPromptJson(const std::string& toolName) {
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->agentConfig) {
        return {};
    }
    const auto& prompts = ctx->agentConfig->prompt.toolPrompt;
    auto        it      = prompts.find(toolName);
    if (it == prompts.end()) {
        return {};
    }
    neograph::json j = neograph::json::object();
    j["depict"]      = it->second.depict;
    j["args"]        = neograph::json::object();
    for (const auto& [name, desc] : it->second.args) {
        j["args"][name] = desc;
    }
    return j.dump();
}

std::string PluginManager::getPromptJson() {
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->agentConfig) {
        return {};
    }
    return ctx->agentConfig->prompt.toJson().dump();
}

int PluginManager::setPromptJson(PluginInstance* inst, const char* prompt_json) {
    auto ctx = agentContext_.lock();
    if (!inst || !ctx || !ctx->agentConfig || !prompt_json || !*prompt_json) {
        return -1;
    }
    neograph::json j;
    try {
        j = neograph::json::parse(prompt_json);
    } catch (const std::exception& e) {
        XX_LOGW("Plugin `{}` set_prompt: invalid json: {}", inst->name, e.what());
        return -1;
    }
    if (!j.is_object()) {
        XX_LOGW("Plugin `{}` set_prompt: json must be an object", inst->name);
        return -1;
    }

    auto& prompt = ctx->agentConfig->prompt;

    // ---- 备份 (仅首次写入某条目前记录原值; 重复写入不覆盖备份, 保证回滚到
    //      插件加载前状态而非插件上次写入值) ----
    if (!inst->promptBackup.backedUpSystem) {
        inst->promptBackup.backedUpSystem       = true;
        inst->promptBackup.systemPrompt         = prompt.systemPrompt;
        inst->promptBackup.systemPlanningPrompt = prompt.systemPlanningPrompt;
        inst->promptBackup.systemSkillPrompt    = prompt.systemSkillPrompt;
    }
    if (j.contains("toolPrompt") && j["toolPrompt"].is_object()) {
        for (const auto& item : j["toolPrompt"].items()) {
            const auto& name = item.first;
            if (std::find(
                    inst->promptBackup.backedUpTools.begin(),
                    inst->promptBackup.backedUpTools.end(),
                    name
                )
                == inst->promptBackup.backedUpTools.end()) {
                inst->promptBackup.backedUpTools.push_back(name);
                auto it = prompt.toolPrompt.find(name);
                if (it == prompt.toolPrompt.end()) {
                    inst->promptBackup.toolPrompt[name] = std::nullopt; // 原本不存在
                } else {
                    inst->promptBackup.toolPrompt[name] = it->second; // 原值
                }
            }
        }
    }

    // ---- 合并应用 (仅覆盖 JSON 中出现的字段) ----
    prompt.mergeFromJson(j);
    XX_LOGI(
        "Plugin `{}` set_prompt applied (system: {}, toolPrompt entries: {})",
        inst->name,
        j.contains("systemPrompt") || j.contains("systemPlanningPrompt")
                || j.contains("systemSkillPrompt")
            ? "yes"
            : "no",
        j.contains("toolPrompt") && j["toolPrompt"].is_object() ? j["toolPrompt"].size() : 0
    );
    return 0;
}

void PluginManager::restorePromptBackup(PluginInstance* inst) {
    if (!inst) {
        return;
    }
    auto& backup = inst->promptBackup;
    if (!backup.backedUpSystem && backup.toolPrompt.empty()) {
        return; // 插件未写过提示词
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->agentConfig) {
        XX_LOGW("Plugin `{}` prompt rollback skipped: agent config not available", inst->name);
        return;
    }
    auto& prompt = ctx->agentConfig->prompt;
    // 恢复 system 提示词 (三个 system 字段在 AgentPrompt 中恒有默认值, 仅当备份
    // 有值时覆盖; 备份无值理论上不会发生, 保守跳过)
    if (backup.systemPrompt.has_value()) {
        prompt.systemPrompt = *backup.systemPrompt;
    }
    if (backup.systemPlanningPrompt.has_value()) {
        prompt.systemPlanningPrompt = *backup.systemPlanningPrompt;
    }
    if (backup.systemSkillPrompt.has_value()) {
        prompt.systemSkillPrompt = *backup.systemSkillPrompt;
    }
    // 恢复 toolPrompt 条目: 原本存在 → 恢复原值; 原本不存在 → 删除
    for (const auto& [name, original] : backup.toolPrompt) {
        if (original.has_value()) {
            prompt.toolPrompt[name] = *original;
        } else {
            prompt.toolPrompt.erase(name);
        }
    }
    backup = PluginInstance::PromptBackup{};
    XX_LOGI("Plugin `{}` prompt rolled back to load-time state", inst->name);
}

std::string PluginManager::getPluginArgsJson(PluginInstance* inst) {
    if (!inst) {
        return "{}";
    }
    // 直接读取实例保存的 args (加载时随配置传入); 宿主不解析字段语义,
    // 插件经 vtable get_plugin_args 整体读取
    return inst->args.dump();
}

std::shared_ptr<PluginInstance> PluginManager::find(std::string_view name) const {
    auto it = plugins_.find(name);
    if (it == plugins_.end()) {
        return nullptr;
    }
    return it->second;
}

} // namespace plugin
} // namespace agentxx
