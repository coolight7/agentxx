/// agent 侧插件宿主 —— 领域部分 (工具/权限/钩子/图/提示词/资源) 与生命周期接缝
///
/// 装载/启停/禁用启用/卸载/级联依赖的骨架在 cxx_pluginxx
/// (见
/// [pluginxx/host/lifecycle.h](/agent/third_party/cxx_pluginxx/include/pluginxx/host/lifecycle.h)),
/// 本文件只保留:
/// 1. 实例的析构 (stop 欠账时拒绝 destroy/dlclose 的守卫);
/// 2. 管理器构造/析构与领域注册表初始化;
/// 3. 骨架的宿主接缝覆写 —— "把宿主配置类型转成内核装载参数"以及
///    "把领域注册(工具/权限/钩子/图/提示词/资源)摘除或清空";
/// 4. agentxx 自己的查询与配置驱动装载 (list / JSON / loadConfiguredPlugins)。
#include "agentxx/plugin/plugin_graph_node.h"
#include "agentxx/plugin/plugin_manager.h"
#include "pluginxx/runtime/op_driver.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/resource_applier.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/middlewares/permission.h"
#include "agentxx/plugin/plugin_framework.h"
#include "agentxx/plugin/plugin_interfaces.h"
#include "agentxx/util/exception.h"
#include "asio/as_tuple.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace agentxx {
namespace plugin {

const void* PLUGINXX_CALL xx_query_interface(const PluginxxHost*, const PluginxxStringView* iid);

// =====================================================================
// PluginInstance
// =====================================================================

PluginInstance::~PluginInstance() {
    if (lifecycleStopPending()) {
        // stop 从未执行：此时 destroy 会看到不完整的插件状态。析构无法“保留”
        // 对象本身，因此宁可泄漏上下文与 DSO，也不能调用插件 destroy/dlclose。
        XX_LOGE(
            "Plugin `{}` destroyed with lifecycle stop pending; keeping plugin context "
            "and DSO loaded (owner must call shutdownAsync before destruction)",
            name
        );
        return;
    }
    if (!destroyPlugin()) {
        // 析构不能绕过仍在运行的插件代码。此处宁可保留动态库句柄，
        // 也不能在 worker/callback 仍可能执行时 dlclose。
        XX_LOGE(
            "Plugin `{}` destroyed while {} lease(s) remain; refusing to dlclose",
            name,
            lifetime ? lifetime->leaseCount() : 0
        );
        return;
    }
    if (dlHandle) {
        NativeLoader::close(dlHandle);
        dlHandle = nullptr;
    }
}

// =====================================================================
// 管理器构造 / 析构
// =====================================================================

PluginManager::PluginManager(std::weak_ptr<agentxx::agent::AgentContext> agentContext) :
    agentContext_(std::move(agentContext)) {
    // 通用表 (events/scheduler/tasks/capabilities 等) 需要宿主数据的入口经本类取数
    // (见 pluginxx/host/domain_hooks.h): 在任何 vtable 入口被调用之前注入。
    setDomainHooks(this);
    if (auto ctx = agentContext_.lock()) {
        registry_ = ctx->toolRegistry ? ctx->toolRegistry : std::make_shared<ToolRegistry>();
    } else {
        registry_ = std::make_shared<ToolRegistry>();
    }
}

PluginManager::~PluginManager() {
    // 同步卸载全部实例 (骨架实现: 仍有 lease / 欠 stop 的实例保留上下文与 DSO)
    shutdownAll();
    warnPendingCloseOnDestroy("agent");
}

void PluginManager::flushPendingCleanup() {
    for (auto& item : pendingCleanups_) {
        if (auto mw = item.mw.lock()) {
            eraseMiddleware(mw.get());
        }
    }
    pendingCleanups_.clear();
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

// =====================================================================
// pluginxx::PluginHostLifecycle 宿主接缝
// =====================================================================

/// 生成 agent 侧实例对象
/// - 只做领域自引用 (self) 与所属管理器 (manager) 的设置;
/// - 基类自引用 (ownerSelf)、元信息/描述/路径、生命周期入口、生命周期控制块与
///   交给自己插件的 host 控制块由骨架的 makeInstance 统一装配。
std::shared_ptr<PluginInstance> PluginManager::createInstance(std::string name) {
    auto inst     = std::make_shared<PluginInstance>(std::move(name));
    inst->self    = inst;
    inst->manager = shared_from_this();
    return inst;
}

/// 摘除实例的领域注册 (工具 / 工具权限 / 图节点类型 / prompt 贡献 / 中间件停用)
/// - 只摘除宿主侧生效的注册, 保留实例内的注册记录 (启用时由 start 事务重新声明);
/// - 事件订阅与能力声明的撤销属通用部分, 由骨架的 detachAll 处理。
void PluginManager::detachDomainRegistrations(PluginInstance* inst) {
    if (!inst) {
        return;
    }
    for (const auto& name : inst->toolNames) {
        registry_->unregisterTool(name);
    }
    // 工具权限声明随工具一并撤销 (工具不再可调用, 声明无需保留)
    if (auto* permission = permissionMiddleware()) {
        for (const auto& name : inst->permissionToolNames) {
            permission->unregisterToolPermission(name);
        }
    }
    inst->permissionToolNames.clear();

    for (const auto& graph : inst->graphNodeTypes) {
        if (graph.slot) {
            graph.slot->invalidate(inst);
        }
    }

    restorePromptBackup(inst);

    if (inst->middleware) {
        inst->middleware->disabled = true;
    }
}

/// 摘除实例专属资源的所有权: 中间件句柄 + 资源应用器上的启用标记
void PluginManager::detachDomainOwnedResources(PluginInstance* inst) {
    if (!inst) {
        return;
    }
    eraseMiddleware(inst->middleware.get());
    inst->middleware = nullptr;
    if (auto c = agentContext_.lock()) {
        if (c->resourceApplier) {
            c->resourceApplier->setOwnerEnabled(inst->name, false);
        }
    }
}

/// 清空"由插件 start 事务重新声明"的领域记录 (工具/权限/hook/图)
/// - 通用记录 (事件订阅/能力声明) 由骨架的 clearPluginOwnedRegistrations 清空;
/// - stop 成功后调用, 避免下次 start 在旧记录上重复累积。
void PluginManager::clearDomainRegistrations(PluginInstance* inst) {
    if (!inst) {
        return;
    }
    inst->toolNames.clear();
    inst->permissionToolNames.clear();
    inst->tools.clear();
    inst->hookRegistrations.clear();
    inst->graphNodeTypes.clear();
}

/// 卸载时释放实例级资源: 工具对象列表 + 清单资源所有权
void PluginManager::releaseInstanceResources(PluginInstance& inst) {
    inst.tools.clear();
    if (auto c = agentContext_.lock()) {
        if (c->resourceApplier) {
            c->resourceApplier->removeAllOwned(inst.name);
        }
    }
}

/// 启用状态变化通知: 同步资源应用器上的启用标记
void PluginManager::onInstanceEnabledChanged(PluginInstance& inst, bool enabled) {
    if (auto c = agentContext_.lock()) {
        if (c->resourceApplier) {
            c->resourceApplier->setOwnerEnabled(inst.name, enabled);
        }
    }
}

// =====================================================================
// 装载入口 (旧签名 → 内核装载参数)
// =====================================================================

namespace {

/// 宿主插件配置 → 内核装载参数 (骨架只认 args 与 configPath 两个字段)
void toLoadOptions(
    const agentxx::agent::PluginConfig* cfg,
    pluginxx::PluginLoadOptions&        options,
    bool&                               hasOptions
) {
    hasOptions = (cfg != nullptr);
    if (cfg) {
        options.args       = cfg->args;
        options.configPath = cfg->configPath;
    }
}

} // namespace

asio::awaitable<std::shared_ptr<PluginInstance>> PluginManager::loadNativeAsync(
    std::string                             path,
    const agentxx::agent::PluginConfig*     cfg,
    bool                                    allowClientOnlySkip,
    const plugin::PluginManifestResources&  resources,
    const plugin::PluginManifestInterfaces& interfaces
) {
    pluginxx::PluginLoadOptions options;
    bool                        hasOptions = false;
    toLoadOptions(cfg, options, hasOptions);
    co_return co_await pluginxx::PluginHostLifecycle<PluginInstance>::loadNativeAsync(
        std::move(path),
        hasOptions ? &options : nullptr,
        allowClientOnlySkip,
        resources,
        interfaces
    );
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
    pluginxx::PluginLoadOptions options;
    bool                        hasOptions = false;
    toLoadOptions(cfg, options, hasOptions);
    co_return co_await pluginxx::PluginHostLifecycle<PluginInstance>::loadBuiltinAsync(
        std::move(name),
        std::move(path),
        std::move(depends),
        std::move(optionalDepends),
        hasOptions ? &options : nullptr,
        resources,
        interfaces
    );
}

asio::awaitable<std::shared_ptr<PluginInstance>> PluginManager::loadPluginAsync(
    std::string                         path,
    const agentxx::agent::PluginConfig* cfg,
    bool                                allowClientOnlySkip
) {
    pluginxx::PluginLoadOptions options;
    bool                        hasOptions = false;
    toLoadOptions(cfg, options, hasOptions);
    co_return co_await pluginxx::PluginHostLifecycle<PluginInstance>::loadPluginAsync(
        std::move(path),
        hasOptions ? &options : nullptr,
        allowClientOnlySkip
    );
}

// =====================================================================
// 查询
// =====================================================================

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
        view.inflight           = inst->lifetime ? inst->lifetime->leaseCount() : 0;
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
    auto              views = list();
    utilxx_base::Json arr   = utilxx_base::Json::array();
    for (const auto& v : views) {
        utilxx_base::Json item;
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
    utilxx_base::Json item;
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
    utilxx_base::Json caps      = utilxx_base::Json::array();
    for (const auto& c : inst->capabilityRegistrations) {
        caps.push_back(c.name);
    }
    item["capabilities"] = std::move(caps);
    return item.dump();
}

// =====================================================================
// 配置驱动装载 (agentxx 自有: 输入是宿主配置条目, 与内核装载参数解耦)
// =====================================================================

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
