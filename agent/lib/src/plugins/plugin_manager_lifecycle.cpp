#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/resource_applier.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/plugin/plugin_common.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "asio/as_tuple.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

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

const void* AGENTXX_PLUGIN_CALL xx_query_interface(const AgentxxPluginHost*, const AgentxxPluginStringView* iid);

// =====================================================================
// NativeLoader
// =====================================================================

void* NativeLoader::open(const std::string& path, std::string& err) {
#if XX_IS_WIN_D
    std::wstring wpath;
    {
        int len = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        if (len > 0) {
            wpath.resize(static_cast<size_t>(len) - 1);
            ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), len);
        }
    }
    HMODULE h = ::LoadLibraryW(wpath.c_str());
    if (!h) {
        err = fmt::format("LoadLibrary failed: error {}", ::GetLastError());
        return nullptr;
    }
    return reinterpret_cast<void*>(h);
#else
    ::dlerror();
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
#if XX_IS_WIN_D
    FARPROC p = ::GetProcAddress(reinterpret_cast<HMODULE>(handle), name);
    if (!p) {
        err = fmt::format("GetProcAddress({}) failed: error {}", name, ::GetLastError());
        return nullptr;
    }
    return reinterpret_cast<void*>(p);
#else
    ::dlerror();
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
#if XX_IS_WIN_D
    ::FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    ::dlclose(handle);
#endif
}

void NativeLoader::addSearchPath(std::string_view dir) {
    (void)dir;
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
// PluginManager 核心生命周期
// =====================================================================

PluginManager::PluginManager(std::weak_ptr<agentxx::agent::AgentContext> agentContext) :
    agentContext_(std::move(agentContext)),
    capabilities_(std::make_shared<CapabilityRegistry>()) {
    if (auto ctx = agentContext_.lock()) {
        registry_ = ctx->toolRegistry ? ctx->toolRegistry : std::make_shared<ToolRegistry>();
    } else {
        registry_ = std::make_shared<ToolRegistry>();
    }
}

PluginManager::~PluginManager() {
    shutdownAll();
}

void PluginManager::shutdownAll() {
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
    for (const auto& dep :
         collectReverseRequiredDeps(plugins_, inst->name, /*onlyEnabled=*/false)) {
        auto depInst = find(dep);
        if (depInst && !depInst->unloadRequested) {
            shutdownPlugin(depInst);
        }
    }
    detachAll(inst.get());
    inst->tools.clear();
    eraseMiddleware(inst->middleware.get());
    inst->middleware = nullptr;
    if (auto c = agentContext_.lock()) {
        if (c->resourceApplier) {
            c->resourceApplier->removeAllOwned(inst->name);
        }
    }
    if (inst->dlHandle) {
        std::string err;
        auto        fn = reinterpret_cast<AgentxxPluginDestroyFn>(
            NativeLoader::sym(inst->dlHandle, AGENTXX_PLUGIN_AGENT_SYMBOL_DESTROY, err)
        );
        if (fn) {
            fn(inst->pluginCtx);
        }
    } else if (inst->builtinUnload) {
        inst->builtinUnload(inst->pluginCtx);
    }
    XX_LOGI("Plugin shutdown: {}", inst->name);
}

void PluginManager::detachAll(PluginInstance* inst) {
    if (!inst) {
        return;
    }

    for (auto& op : inst->outstandingOps) {
        if (op && !op->cancelled.load(std::memory_order_acquire)) {
            op->cancelled.store(true, std::memory_order_release);
            if (op->cancelFn) {
                try {
                    op->cancelFn();
                } catch (...) {
                }
            }
        }
    }
    inst->outstandingOps.clear();

    for (auto& [key, timer] : inst->sleepTimers) {
        (void)key;
        if (timer && timer->timer) {
            timer->timer->cancel();
        }
    }
    inst->sleepTimers.clear();

    for (const auto& name : inst->toolNames) {
        registry_->unregisterTool(name);
    }
    for (const auto& sub : inst->subscriptions) {
        if (sub && sub->bus && sub->subscriptionId != 0) {
            sub->bus->get<std::string>(sub->topic).unsubscribe(sub->subscriptionId);
            sub->subscriptionId = 0;
            sub->inst           = nullptr;
        }
    }
    inst->subscriptions.clear();

    for (const auto& cap : inst->capabilityRegistrations) {
        capabilities_->unregisterCapability(cap.name, inst->name);
    }

    restorePromptBackup(inst);

    if (inst->middleware) {
        inst->middleware->disabled = true;
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

void PluginManager::disable(std::string_view name) {
    auto inst = find(name);
    if (!inst || !inst->enabled) {
        return;
    }
    inst->enabled = false;
    for (const auto& dep : collectReverseRequiredDeps(plugins_, inst->name, /*onlyEnabled=*/true)) {
        auto depInst = find(dep);
        if (depInst && depInst->enabled) {
            depInst->enabled = false;
            detachAll(depInst.get());
            eraseMiddleware(depInst->middleware.get());
            depInst->middleware = nullptr;
            if (auto c = agentContext_.lock()) {
                if (c->resourceApplier) {
                    c->resourceApplier->setOwnerEnabled(depInst->name, false);
                }
            }
        }
    }
    detachAll(inst.get());
    eraseMiddleware(inst->middleware.get());
    inst->middleware = nullptr;
    if (auto c = agentContext_.lock()) {
        if (c->resourceApplier) {
            c->resourceApplier->setOwnerEnabled(inst->name, false);
        }
    }
}

void PluginManager::enable(std::string_view name) {
    auto inst = find(name);
    if (!inst || inst->enabled) {
        return;
    }
    inst->enabled = true;
    for (const auto& tool : inst->tools) {
        registry_->registerTool(tool->get_definition().name, tool);
    }
    if (!inst->hookRegistrations.empty()) {
        auto ctx = agentContext_.lock();
        if (ctx && ctx->middlewareHandleContext) {
            inst->middleware = std::make_shared<PluginMiddlewareHandle>(
                fmt::format("{}_middleware", inst->name),
                agentContext_,
                inst
            );
            ctx->middlewareHandleContext->handles.push_back(inst->middleware);
            for (const auto& hook : inst->hookRegistrations) {
                AgentxxPluginHookSpec spec{};
                spec.point       = hook.point;
                spec.hook_start  = hook.start;
                spec.hook_cancel = hook.cancel;
                spec.user_data   = hook.ud;
                inst->middleware->setHook(spec);
            }
        }
    }
    for (const auto& cap : inst->capabilityRegistrations) {
        if (cap.start) {
            capabilities_->registerCapability(cap.name, inst->name, cap.start, cap.cancel, cap.ctx);
        } else {
            capabilities_->registerCapability(cap.name, inst->name);
        }
    }
    if (auto c = agentContext_.lock()) {
        if (c->resourceApplier) {
            c->resourceApplier->setOwnerEnabled(inst->name, true);
        }
    }
}

void PluginManager::flushPendingCleanup() {
    for (auto& item : pendingCleanups_) {
        if (auto mw = item.mw.lock()) {
            eraseMiddleware(mw.get());
        }
    }
    pendingCleanups_.clear();
}

asio::awaitable<bool> PluginManager::unloadAsync(std::string_view name) {
    auto inst = find(name);
    if (!inst) {
        XX_LOGW("Plugin unload: `{}` not loaded", name);
        co_return false;
    }
    if (inst->unloadRequested) {
        co_return false;
    }
    inst->unloadRequested = true;

    for (const auto& dep :
         collectReverseRequiredDeps(plugins_, inst->name, /*onlyEnabled=*/false)) {
        auto depInst = find(dep);
        if (depInst && !depInst->unloadRequested) {
            co_await unloadAsync(depInst->name);
        }
    }

    detachAll(inst.get());
    inst->tools.clear();
    eraseMiddleware(inst->middleware.get());
    inst->middleware = nullptr;
    if (auto c = agentContext_.lock()) {
        if (c->resourceApplier) {
            c->resourceApplier->removeAllOwned(inst->name);
        }
    }

    bool ok = co_await waitInflightZero(inst, std::chrono::seconds(30));
    if (!ok) {
        XX_LOGE("Plugin `{}` unload timed out waiting for inflight callbacks", inst->name);
        co_return false;
    }

    if (inst->dlHandle) {
        std::string err;
        auto        fn = reinterpret_cast<AgentxxPluginDestroyFn>(
            NativeLoader::sym(inst->dlHandle, AGENTXX_PLUGIN_AGENT_SYMBOL_DESTROY, err)
        );
        if (fn) {
            fn(inst->pluginCtx);
        }
    } else if (inst->builtinUnload) {
        inst->builtinUnload(inst->pluginCtx);
    }

    plugins_.erase(std::string(name));
    XX_LOGI("Plugin `{}` unloaded", name);
    co_return true;
}

std::vector<PluginManager::PluginListView> PluginManager::list() const {
    std::vector<PluginListView> out;
    out.reserve(plugins_.size());
    for (const auto& [name, inst] : plugins_) {
        PluginListView view;
        view.name               = inst->name;
        view.version            = inst->version;
        view.description        = inst->description;
        view.path               = inst->path;
        view.configPath         = inst->configPath;
        view.enabled            = inst->enabled;
        view.inflight           = inst->inflight.load(std::memory_order_acquire);
        view.tools              = inst->toolNames;
        view.depends            = inst->depends;
        view.optionalDepends    = inst->optionalDepends;
        view.requiredInterfaces = inst->interfaces.require;
        view.optionalInterfaces = inst->interfaces.optional;
        for (const auto& cap : inst->capabilityRegistrations) {
            view.capabilities.push_back(cap.name);
        }
        out.push_back(std::move(view));
    }
    return out;
}

std::string PluginManager::listPluginsJson() {
    auto           views = list();
    neograph::json arr   = neograph::json::array();
    for (const auto& v : views) {
        neograph::json item;
        item["name"]                = v.name;
        item["version"]             = v.version;
        item["description"]         = v.description;
        item["path"]                = v.path;
        item["config"]              = v.configPath;
        item["enabled"]             = v.enabled;
        item["tools"]               = v.tools;
        item["capabilities"]        = v.capabilities;
        item["depends"]             = v.depends;
        item["optional_depends"]    = v.optionalDepends;
        item["required_interfaces"] = v.requiredInterfaces;
        item["optional_interfaces"] = v.optionalInterfaces;
        arr.push_back(std::move(item));
    }
    return arr.dump();
}

std::string PluginManager::getPluginJson(const std::string& name) {
    auto inst = find(name);
    if (!inst) {
        return {};
    }
    neograph::json item;
    item["name"]                = inst->name;
    item["version"]             = inst->version;
    item["description"]         = inst->description;
    item["path"]                = inst->path;
    item["config"]              = inst->configPath;
    item["enabled"]             = inst->enabled;
    item["tools"]               = inst->toolNames;
    item["depends"]             = inst->depends;
    item["optional_depends"]    = inst->optionalDepends;
    item["required_interfaces"] = inst->interfaces.require;
    item["optional_interfaces"] = inst->interfaces.optional;
    neograph::json caps         = neograph::json::array();
    for (const auto& c : inst->capabilityRegistrations) {
        caps.push_back(c.name);
    }
    item["capabilities"] = std::move(caps);
    return item.dump();
}

// ==================== 加载分支 (Native / Builtin / Configured) ====================

// ---------------------------------------------------------------------------
// 内置插件路径 helper (yaml `builtin://<name>` 简写)
// ---------------------------------------------------------------------------
static inline bool isBuiltinScheme(std::string_view p) noexcept {
    return p.size() > 10 && p.substr(0, 10) == "builtin://";
}

static inline std::string parseBuiltinName(std::string_view p) {
    return std::string(p.substr(10));
}

asio::awaitable<std::shared_ptr<PluginInstance>> PluginManager::loadNativeAsync(
    std::string                             path,
    const agentxx::agent::PluginConfig*     cfg,
    bool                                    allowClientOnlySkip,
    const plugin::PluginManifestResources&  resources,
    const plugin::PluginManifestInterfaces& interfaces
) {
    (void)allowClientOnlySkip;
    std::string err;
    void*       dl = NativeLoader::open(path, err);
    if (!dl) {
        XX_LOGE("Plugin load failed: {}: {}", path, err);
        co_return nullptr;
    }

    auto getInfoFn = reinterpret_cast<AgentxxPluginGetInfoFn>(
        NativeLoader::sym(dl, AGENTXX_PLUGIN_AGENT_SYMBOL_GET_INFO, err)
    );
    auto createFn = reinterpret_cast<AgentxxPluginCreateFn>(
        NativeLoader::sym(dl, AGENTXX_PLUGIN_AGENT_SYMBOL_CREATE, err)
    );

    if (!createFn) {
        NativeLoader::close(dl);
        if (allowClientOnlySkip) {
            XX_LOGW("Plugin `{}` skipped: no agent entry (client only)", path);
            co_return nullptr;
        }
        XX_LOGE("Plugin `{}` missing {}", path, AGENTXX_PLUGIN_AGENT_SYMBOL_CREATE);
        co_return nullptr;
    }

    const AgentxxPluginInfo* info = getInfoFn ? getInfoFn() : nullptr;
    if (info && info->api_version != AGENTXX_PLUGIN_API_VERSION) {
        NativeLoader::close(dl);
        XX_LOGE(
            "Plugin `{}` API version mismatch (got {}, host requires {})",
            path,
            info->api_version,
            AGENTXX_PLUGIN_API_VERSION
        );
        co_return nullptr;
    }

    std::string name = info && info->name.data ? std::string(info->name.data, info->name.size)
                                               : std::filesystem::path(path).stem().string();
    if (name.starts_with("lib")) {
        name = name.substr(3);
    }

    auto inst     = std::make_shared<PluginInstance>(name);
    inst->version = info && info->version.data ? std::string(info->version.data, info->version.size)
                                               : "1.0.0";
    inst->description = info && info->description.data
                            ? std::string(info->description.data, info->description.size)
                            : "";
    inst->path        = path;
    inst->dlHandle    = dl;
    inst->interfaces  = interfaces;
    inst->self        = inst;
    inst->manager     = shared_from_this();
    auto vtableSv = agentxx_plugin_sv_cstr("__vtable");
    inst->host.vtable
        = (const AgentxxHostVtable*)xx_query_interface(nullptr, &vtableSv);
    inst->host.opaque = inst.get();
    if (cfg) {
        inst->args       = cfg->args;
        inst->configPath = cfg->configPath;
    }

    plugins_[name] = inst;
    int rc         = createFn(&inst->host, &inst->pluginCtx);
    if (rc != 0) {
        plugins_.erase(name);
        NativeLoader::close(dl);
        inst->dlHandle = nullptr;
        XX_LOGE("Plugin `{}` create failed (code={})", name, rc);
        co_return nullptr;
    }

    applyDeclaredResources(*inst, resources);
    inst->resourcesFrozen = true;
    XX_LOGI("Plugin `{}` loaded successfully", name);
    co_return inst;
}

asio::awaitable<std::shared_ptr<PluginInstance>> PluginManager::loadBuiltinAsync(
    std::string                             name,
    std::string                             path,
    std::vector<std::string>                depends,
    std::vector<std::string>                optionalDepends,
    const agentxx::agent::PluginConfig*     cfg,
    const plugin::PluginManifestResources&  resources,
    const plugin::PluginManifestInterfaces& interfaces
) {
    auto entry = agentxx::plugin::findBuiltinPlugin(name);
    if (!entry) {
        XX_LOGE("Built-in plugin `{}` not found in registry", name);
        co_return nullptr;
    }

    const AgentxxPluginInfo* info = entry->get_info ? entry->get_info() : nullptr;
    if (info && info->api_version != AGENTXX_PLUGIN_API_VERSION) {
        XX_LOGE(
            "Builtin plugin `{}` API version mismatch (got {}, host requires {})",
            name,
            info->api_version,
            AGENTXX_PLUGIN_API_VERSION
        );
        co_return nullptr;
    }

    auto inst     = std::make_shared<PluginInstance>(name);
    inst->version = info && info->version.data ? std::string(info->version.data, info->version.size)
                                               : "1.0.0";
    inst->description     = info && info->description.data
                                ? std::string(info->description.data, info->description.size)
                                : "";
    inst->path            = path;
    inst->depends         = std::move(depends);
    inst->optionalDepends = std::move(optionalDepends);
    inst->interfaces      = interfaces;
    inst->self            = inst;
    inst->manager         = shared_from_this();
    auto vtableSv2 = agentxx_plugin_sv_cstr("__vtable");
    inst->host.vtable
        = (const AgentxxHostVtable*)xx_query_interface(nullptr, &vtableSv2);
    inst->host.opaque   = inst.get();
    inst->builtinUnload = entry->destroy;
    if (cfg) {
        inst->args       = cfg->args;
        inst->configPath = cfg->configPath;
    }

    plugins_[name] = inst;
    int rc         = entry->create(&inst->host, &inst->pluginCtx);
    if (rc != 0) {
        plugins_.erase(name);
        XX_LOGE("Builtin plugin `{}` create failed (code={})", name, rc);
        co_return nullptr;
    }

    applyDeclaredResources(*inst, resources);
    inst->resourcesFrozen = true;
    XX_LOGI("Builtin plugin `{}` loaded successfully", name);
    co_return inst;
}

asio::awaitable<std::shared_ptr<PluginInstance>> PluginManager::loadPluginAsync(
    std::string                         path,
    const agentxx::agent::PluginConfig* cfg,
    bool                                allowClientOnlySkip
) {
    // 内置简写: builtin://<name> 直接经内置注册表加载 (无需外部目录/文件)
    if (isBuiltinScheme(path)) {
        auto btName = parseBuiltinName(path);
        if (btName.empty()) {
            XX_LOGE("Plugin load failed: invalid builtin path `{}`", path);
            co_return nullptr;
        }
        // 尝试从默认插件目录解析 manifest 以获取 depends/interfaces/resources
        // (可选: 失败则按无依赖/无资源处理, 不影响内置核心加载)
        std::vector<std::string> depends, optionalDepends;
        PluginManifestResources  resources;
        PluginManifestInterfaces interfaces;
        // 按可执行目录与当前工作目录探测 manifest (与内置合并模式资源拷贝布局一致)
        bool        manifestFound = false;
        std::string dummyName, dummyEntry;
        // 优先内嵌清单 (单文件分发, 无需外部 plugin.yaml)
        if (parseBuiltinManifest(
                btName,
                dummyName,
                dummyEntry,
                depends,
                optionalDepends,
                &resources,
                &interfaces
            )) {
            manifestFound = true;
        } else {
            // 跨平台探测: 优先 exe 目录 (安装布局) 其次 cwd (开发布局)
            std::vector<std::filesystem::path> bases;
            {
                auto exeDir = getExecutableDirPath();
                if (!exeDir.empty()) {
                    bases.push_back(exeDir);
                }
            }
            bases.push_back(std::filesystem::current_path());
            for (auto& base : bases) {
                auto probe = base / "plugins" / btName / "plugin.yaml";
                if (std::filesystem::exists(probe)) {
                    if (parsePluginManifest(
                            probe.parent_path(),
                            dummyName,
                            dummyEntry,
                            depends,
                            optionalDepends,
                            &resources,
                            &interfaces
                        )) {
                        manifestFound = true;
                        break;
                    }
                }
            }
            if (!manifestFound) {
                (void)resources;
                (void)interfaces;
            }
        }
        // 若内置注册表中不存在, 回退为普通目录插件加载 (非合并编译时
        // builtin:// 仍可指向外部目录插件, 保持兼容)
        if (!agentxx::plugin::findBuiltinPlugin(btName)) {
            // 按目录插件路径重新进入常规加载分支 (跨平台: exe 目录优先)
            std::filesystem::path fallback;
            {
                auto exeDir = getExecutableDirPath();
                if (!exeDir.empty()) {
                    auto cand = exeDir / "plugins" / btName;
                    if (std::filesystem::is_directory(cand)) {
                        fallback = cand;
                    }
                }
            }
            if (fallback.empty()) {
                auto cand = std::filesystem::current_path() / "plugins" / btName;
                if (std::filesystem::is_directory(cand)) {
                    fallback = cand;
                }
            }
            if (!fallback.empty()) {
                XX_LOGI(
                    "Builtin plugin `{}` not in registry, fallback to directory `{}`",
                    btName,
                    fallback.string()
                );
                co_return co_await loadPluginAsync(fallback.string(), cfg, allowClientOnlySkip);
            }
        }
        for (const auto& dep : depends) {
            if (!find(dep)) {
                XX_LOGE(
                    "Builtin plugin `{}` load failed: required dependency `{}` not installed",
                    btName,
                    dep
                );
                co_return nullptr;
            }
        }
        for (const auto& dep : optionalDepends) {
            if (!find(dep)) {
                XX_LOGW("Builtin plugin `{}` optional dependency `{}` not installed", btName, dep);
            }
        }
        co_return co_await loadBuiltinAsync(
            btName,
            path,
            depends,
            optionalDepends,
            cfg,
            resources,
            interfaces
        );
    }
    namespace fs = std::filesystem;
    fs::path p(path);
    if (fs::is_directory(p)) {
        auto manifestPath = p / "plugin.yaml";
        if (fs::exists(manifestPath)) {
            std::string              manifestName, manifestEntry;
            std::vector<std::string> depends, optionalDepends;
            PluginManifestResources  resources;
            PluginManifestInterfaces interfaces;
            if (!parsePluginManifest(
                    p,
                    manifestName,
                    manifestEntry,
                    depends,
                    optionalDepends,
                    &resources,
                    &interfaces
                )) {
                XX_LOGE("Parse manifest `{}` failed", manifestPath.string());
                co_return nullptr;
            }

            for (const auto& dep : depends) {
                if (!find(dep)) {
                    XX_LOGE(
                        "Plugin `{}` load failed: required dependency `{}` not installed (load it first)",
                        manifestName,
                        dep
                    );
                    co_return nullptr;
                }
            }

            for (const auto& dep : optionalDepends) {
                if (!find(dep)) {
                    XX_LOGW(
                        "Plugin `{}` optional dependency `{}` not installed",
                        manifestName,
                        dep
                    );
                }
            }

            if (agentxx::plugin::findBuiltinPlugin(manifestName) != nullptr) {
                co_return co_await loadBuiltinAsync(
                    manifestName,
                    manifestPath.string(),
                    depends,
                    optionalDepends,
                    cfg,
                    resources,
                    interfaces
                );
            }

            std::string binPath = resolvePluginEntryPath(p, manifestEntry);
            if (!fs::exists(binPath)) {
#if XX_IS_WIN_D
                binPath = (p / (manifestName + ".dll")).string();
                if (!fs::exists(binPath)) {
                    binPath = (p / ("lib" + manifestName + ".dll")).string();
                }
#elif defined(__APPLE__)
                binPath = (p / ("lib" + manifestName + ".dylib")).string();
                if (!fs::exists(binPath)) {
                    binPath = (p / (manifestName + ".dylib")).string();
                }
#else
                binPath = (p / ("lib" + manifestName + ".so")).string();
                if (!fs::exists(binPath)) {
                    binPath = (p / (manifestName + ".so")).string();
                }
#endif
            }

            auto inst = co_await loadNativeAsync(
                binPath,
                cfg,
                allowClientOnlySkip,
                resources,
                interfaces
            );
            if (inst) {
                inst->depends         = depends;
                inst->optionalDepends = optionalDepends;
            }
            co_return inst;
        }
    }

    co_return co_await loadNativeAsync(path, cfg, allowClientOnlySkip);
}

asio::awaitable<void>
    PluginManager::loadConfiguredPlugins(const std::vector<agentxx::agent::PluginConfig>& plugins) {
    namespace fs = std::filesystem;

    struct SortItem {
        std::string                         name;
        std::vector<std::string>            depends;
        const agentxx::agent::PluginConfig* cfg = nullptr;
        std::string                         path;
    };

    std::vector<SortItem> items;
    items.reserve(plugins.size());

    for (const auto& pc : plugins) {
        if (!pc.enabled) {
            continue;
        }
        if (pc.sides == agentxx::agent::PluginSide::Client) {
            continue;
        }
        SortItem it;
        it.path = pc.path;
        it.cfg  = &pc;
        // 内置简写: builtin://<name> 直接以 name 作为标识
        // 优先内嵌清单取 depends, 回退文件系统
        if (isBuiltinScheme(pc.path)) {
            it.name = parseBuiltinName(pc.path);
            std::string              dummyName, dummyEntry;
            std::vector<std::string> optionalDepends;
            PluginManifestResources  resources;
            PluginManifestInterfaces interfaces;
            if (parseBuiltinManifest(
                    it.name,
                    dummyName,
                    dummyEntry,
                    it.depends,
                    optionalDepends,
                    &resources,
                    &interfaces
                )) {
                // 内嵌清单命中, 依赖已填入 it.depends
            } else {
                // 跨平台探测: exe 目录优先, 回退 cwd
                std::vector<std::filesystem::path> bases;
                {
                    auto exeDir = getExecutableDirPath();
                    if (!exeDir.empty()) {
                        bases.push_back(exeDir);
                    }
                }
                bases.push_back(std::filesystem::current_path());
                for (auto& base : bases) {
                    auto probe = base / "plugins" / it.name / "plugin.yaml";
                    if (std::filesystem::exists(probe)) {
                        if (parsePluginManifest(
                                probe.parent_path(),
                                dummyName,
                                dummyEntry,
                                it.depends,
                                optionalDepends,
                                &resources,
                                &interfaces
                            )) {
                            break;
                        }
                    }
                }
            }
        } else {
            fs::path        p(pc.path);
            std::error_code ec;
            if (fs::is_directory(p, ec)) {
                std::string              manifestName, manifestEntry;
                std::vector<std::string> optionalDepends;
                PluginManifestResources  resources;
                PluginManifestInterfaces interfaces;
                if (parsePluginManifest(
                        p,
                        manifestName,
                        manifestEntry,
                        it.depends,
                        optionalDepends,
                        &resources,
                        &interfaces
                    )) {
                    it.name = manifestName;
                } else {
                    it.name = pluginNameFromPath(pc.path);
                }
            } else {
                it.name = pluginNameFromPath(pc.path);
            }
        }
        items.push_back(std::move(it));
    }

    auto ordered = topoSortPlugins(std::move(items));

    for (const auto& it : ordered) {
        if (it.name.empty()) {
            continue;
        }
        if (plugins_.count(it.name) > 0) {
            continue;
        }
        co_await loadPluginAsync(it.path, it.cfg, /*allowClientOnlySkip=*/true);
    }
}

} // namespace plugin
} // namespace agentxx
