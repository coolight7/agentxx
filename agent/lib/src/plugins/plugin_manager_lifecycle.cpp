/// agent 侧插件主程序 —— 业务部分 (工具/权限/钩子/图/提示词/资源) 与生命周期扩展点
///
/// 加载/启停/禁用启用/卸载/级联依赖的骨架在 cxx_pluginxx
/// (见
/// [pluginxx/host/lifecycle.h](/agent/third_party/cxx_pluginxx/include/pluginxx/host/lifecycle.h)),
/// 本文件只保留:
/// 1. 实例的析构 (stop 欠账时拒绝 destroy/dlclose 的守卫);
/// 2. 管理器构造/析构与业务注册表初始化;
/// 3. 骨架的主程序扩展点覆写 —— "把主程序配置类型转成插件框架加载参数"以及
///    "把业务注册(工具/权限/钩子/图/提示词/资源)摘除或清空";
/// 4. agentxx 自己的查询与配置驱动加载 (list / JSON / loadConfiguredPlugins)。
#include "agentxx/plugin/plugin_graph_node.h"
#include "agentxx/plugin/plugin_manager.h"
#include "pluginxx/runtime/op_driver.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/resource_applier.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/feature/registry.h"
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
#include "utilxx_base/string_util.h"

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
    // 通用表 (events/scheduler/tasks/capabilities 等) 需要主程序数据的入口经本类取数
    // (见 pluginxx/host/domain_hooks.h): 在任何 vtable 入口被调用之前注入。
    setDomainHooks(this);
    if (auto ctx = agentContext_.lock()) {
        registry_ = ctx->toolRegistry ? ctx->toolRegistry : std::make_shared<ToolRegistry>();
        // 本挂载的 agent 标识: 单根用法下就是 "root" (AgentHost 可覆写成 "sub:N")
        agentId_ = "root";
    } else {
        registry_ = std::make_shared<ToolRegistry>();
    }
}

// =====================================================================
// 挂载 (本管理器 = 一个 agent 的挂载)
// =====================================================================

bool PluginManager::mountOf(const PluginxxScopeRef* ref) const {
    if (!pluginxx_scope_ref_ok(ref) || ref == nullptr) {
        return false;
    }
    if (ref->id.data == nullptr || ref->id.size == 0 || agentId_.empty()) {
        return false;
    }
    if (agentId_.size() != static_cast<size_t>(ref->id.size)
        || agentId_.compare(0, agentId_.size(), ref->id.data, agentId_.size()) != 0) {
        return false;
    }
    // seq = 0 表示不校验 (FFI / 测试自行构造引用时可用)
    return ref->seq == 0 || ref->seq == mountSeq_;
}

namespace {

/// 把 C 字符串视图转成自有字符串
std::string mountViewToStr(const PluginxxStringView& sv) {
    if (sv.data == nullptr || sv.size == 0) {
        return {};
    }
    return std::string{sv.data, static_cast<size_t>(sv.size)};
}

} // namespace

asio::awaitable<bool> PluginManager::mountOneAsync(std::shared_ptr<PluginInstance> inst) {
    if (!inst) {
        co_return false;
    }
    if (inst->pluginMounted) {
        co_return true; // 幂等
    }
    // 实例已在关闭流程里 (卸载/主程序关闭): 不再挂载, 否则会把刚摘掉的注册又加回去
    if (inst->lifetime && inst->lifetime->closeRequested()) {
        co_return false;
    }
    // 挂载期间允许登记资源 (skill/memory/mcp): 它们属于 agent 级注册,
    // 与工具/钩子一样只能在 mount 里声明; 挂载完成后才冻结
    inst->resourcesFrozen = false;
    if (!inst->mountFn) {
        // 无挂载入口 = 本插件无 agent 侧能力; 清单资源仍然按挂载应用
        applyManifestResourcesNow(*inst);
        inst->pluginMounted   = true;
        inst->resourcesFrozen = true;
        co_return true;
    }
    auto ctx = agentContext_.lock();
    if (!ctx) {
        co_return false;
    }

    // 挂载序号在第一次建立挂载时分配; 之后运行期新加载的插件沿用同一个序号
    // (它们是同一个 agent 的挂载, 引用里的 seq 必须一致才能通过校验)
    if (mountSeq_ == 0) {
        ++mountSeq_;
    }

    // 挂载参数: agent 标识/工作目录/语言/模型/挂载配置
    const std::string workDir   = getSessionWorkDir();
    const std::string language  = getLanguage();
    const std::string modelJson = getModelConfigJson();
    const std::string agentName = ctx->agentConfig ? ctx->agentConfig->agentName : std::string{};

    inst->mountSeq      = mountSeq_;
    inst->mountUserData = nullptr;

    // 清单声明的资源 (skill/memory/mcp) 属于本挂载: 先应用, 插件的 mount 回调里
    // 还能继续声明 (资源声明在挂载收尾才冻结)
    applyManifestResourcesNow(*inst);

    const std::string           configJson = inst->args.dump();
    AgentxxPluginAgentMountSpec spec{};
    spec.struct_size = sizeof(AgentxxPluginAgentMountSpec);
    spec.flags       = 0;
    spec.agent_id    = PluginStringView::from(agentId_);
    spec.agent_name  = PluginStringView::from(agentName);
    spec.work_dir    = PluginStringView::from(workDir);
    spec.language    = PluginStringView::from(language);
    spec.model_json  = PluginStringView::from(modelJson);
    spec.config_json = PluginStringView::from(configJson);
    spec.mount_seq   = mountSeq_;

    std::string mountError;
    auto        instPtr     = inst;
    void*       mountResult = nullptr;
    const bool  ok          = co_await pluginxx::awaitPluginLifecycle(
        this->runtime(),
        instPtr,
        instPtr->pluginCtx,
        [instPtr, &spec, &mountResult](
            void*                         /*ctx*/,
            const PluginxxOperatorNotify* notify,
            PluginxxString*               err
        ) {
            mountResult = instPtr->mountFn(instPtr->pluginCtx, &spec, notify, err);
            return mountResult;
        },
        "plugin mount",
        mountError
    );
    if (!ok) {
        XX_LOGE("agent '{}' 挂载插件 `{}` 失败: {}", agentId_, inst->name, mountError);
        // 回滚该插件在本 agent 上已经生效的注册 (工具/权限/钩子/提示词/功能点/
        // 图类型/订阅/能力/事件订阅), 不影响其他实例与其他 agent
        this->detachInstanceRegistrations(inst.get());
        // 资源 (skill/memory/mcp) 的所有权记录一并清掉: 这不是"禁用", 是根本没有挂上
        if (auto c = agentContext_.lock(); c && c->resourceApplier) {
            c->resourceApplier->removeAllOwned(inst->name);
        }
        inst->mountUserData   = nullptr;
        inst->pluginMounted   = false;
        inst->resourcesFrozen = false; // 允许重新挂载时再次声明
        co_return false;
    }
    inst->mountUserData   = mountResult;
    inst->pluginMounted   = true;
    inst->resourcesFrozen = true; // 挂载收尾: 资源声明固定 (运行时注册会被拒)
    // 挂载期间实例被请求关闭: 立刻拆掉, 否则注册会在关闭流程之后残留
    if (inst->lifetime && inst->lifetime->closeRequested()) {
        co_await unmountOneAsync(inst);
        co_return false;
    }
    co_return true;
}

asio::awaitable<void> PluginManager::unmountOneAsync(std::shared_ptr<PluginInstance> inst) {
    if (!inst || !inst->pluginMounted) {
        co_return;
    }
    // 拆挂载后该 agent 上的资源声明也一并撤销 (记录由挂载撤销路径摘除);
    // 重新挂载时插件会再次声明, 因此这里解除冻结
    inst->resourcesFrozen = false;
    if (inst->unmountFn) {
        std::string unmountError;
        auto        instPtr = inst;
        void*       mountUd = inst->mountUserData;
        const bool  ok      = co_await pluginxx::awaitPluginLifecycle(
            this->runtime(),
            instPtr,
            instPtr->pluginCtx,
            [instPtr, mountUd](
                void*                         /*ctx*/,
                const PluginxxOperatorNotify* notify,
                PluginxxString*               err
            ) {
                return instPtr->unmountFn(instPtr->pluginCtx, mountUd, notify, err);
            },
            "plugin unmount",
            unmountError
        );
        if (!ok) {
            XX_LOGW("agent '{}' 卸载插件 `{}` 失败: {}", agentId_, inst->name, unmountError);
        }
    }
    // 摘掉本 agent 上的实体: 业务注册 (工具/权限/钩子/提示词/功能点/图类型)
    // 与资源 (skill/memory/mcp) 所有权记录。
    // - **不**动能力声明与主程序总线订阅: 它们属于进程级 (`start`/`stop`), 拆挂载时
    //   仍然存活 (否则重新启用后能力就丢了)
    this->detachDomainRegistrations(inst.get());
    this->detachDomainOwnedResources(inst.get());
    if (auto c = agentContext_.lock(); c && c->resourceApplier) {
        c->resourceApplier->removeAllOwned(inst->name);
    }
    inst->pluginMounted = false;
    inst->mountUserData = nullptr;
    co_return;
}

asio::awaitable<void> PluginManager::mountAgentAsync() {
    if (!agentContext_.lock()) {
        co_return;
    }
    for (auto& [name, inst] : this->plugins_) {
        (void)name;
        if (!inst || inst->pluginMounted) {
            continue; // 幂等: 本次调用只挂新加入的实例
        }
        co_await mountOneAsync(inst);
    }
    hasMount_ = true;
    XX_LOGI(
        "agent '{}' 完成插件挂载 (seq {}, {} 个实例)",
        agentId_,
        mountSeq_,
        this->plugins_.size()
    );
    co_return;
}

asio::awaitable<void> PluginManager::unmountAgentAsync() {
    if (!hasMount_) {
        co_return;
    }
    // 逆序卸载
    for (auto it = this->plugins_.rbegin(); it != this->plugins_.rend(); ++it) {
        co_await unmountOneAsync(it->second);
    }
    hasMount_ = false;
    XX_LOGI("agent '{}' 已拆全部插件挂载", agentId_);
    co_return;
}

// =====================================================================
// 禁用/启用与挂载的联动 (骨架完成 stop/start 后由本类补齐挂载动作)
// =====================================================================

asio::awaitable<void> PluginManager::onInstanceDisabledAsync(
    const std::shared_ptr<PluginInstance>& inst
) {
    // 禁用 = 该 agent 不再挂这个插件: 先拆挂载 (调插件 unmount 释放 per-agent 状态),
    // 实例本身不销毁、不卸载; 注册已由骨架的 detachInstanceRegistrations 摘除。
    //
    // 等待期间用户又启用了它 (disable/enable 紧邻): 本次卸载作废 —— 否则会把
    // 启用事务刚刚重建的注册与能力又摘一遍。
    if (!inst || inst->enabled) {
        co_return;
    }
    co_await unmountOneAsync(inst);
    co_return;
}

asio::awaitable<void> PluginManager::onInstanceEnabledAsync(
    const std::shared_ptr<PluginInstance>& inst
) {
    // 启用 = 重新挂到本 agent: 注册全部由插件在自己的 mount 回调里重新声明
    //
    // 先幂等地拆一次挂载: `disable` 的拆挂载事务可能与本次启用交错 (它发现实例
    // 已重新启用时会跳过), 这样无论两者谁先完成, 最后都恰好挂载一次。
    if (!inst || !inst->enabled || !hasMount_) {
        co_return;
    }
    co_await unmountOneAsync(inst);
    if (!inst->enabled) {
        co_return; // 拆挂载期间又被禁用了
    }
    co_await mountOneAsync(inst);
    co_return;
}

std::string PluginManager::listMountsJson() {
    utilxx_base::Json arr = utilxx_base::Json::array();
    if (!hasMount_ || agentId_.empty()) {
        return arr.dump();
    }
    auto              ctx = agentContext_.lock();
    utilxx_base::Json item;
    item["agentId"]   = agentId_;
    item["agentName"] = ctx && ctx->agentConfig ? ctx->agentConfig->agentName : std::string{};
    item["workDir"]   = getSessionWorkDir();
    item["language"]  = getLanguage();
    item["mountSeq"]  = mountSeq_;
    utilxx_base::Json names = utilxx_base::Json::array();
    for (const auto& [name, inst] : this->plugins_) {
        if (inst && inst->pluginMounted) {
            names.push_back(name);
        }
    }
    item["plugins"] = std::move(names);
    arr.push_back(std::move(item));
    return arr.dump();
}

std::string PluginManager::mountInfoJson(std::string_view agentId) {
    if (agentId != agentId_ || !hasMount_) {
        return {};
    }
    auto text = listMountsJson();
    try {
        auto arr = utilxx_base::Json::parse(text);
        if (arr.is_array() && !arr.empty()) {
            return arr[0].dump();
        }
    } catch (...) {
    }
    return {};
}

PluginManager::~PluginManager() {
    // 同步卸载全部实例 (骨架实现: 仍有 lease / 欠 stop 的实例保留上下文与 DSO)
    shutdownAll();
    warnPendingCloseOnDestroy("agent");
}

void PluginManager::flushPendingCleanup() {
    for (auto& item : pendingCleanups_) {
        auto handle = item.handle.lock();
        if (!handle) {
            continue;
        }
        eraseHookDispatch(handle.get());
        // 排队摘除的句柄就是当前派发器时一并释放 (没有处理器才会排队, 这里再确认一次)
        if (hookHandlers_.empty() && hookDispatch_.get() == handle.get()) {
            hookDispatch_ = nullptr;
        }
    }
    pendingCleanups_.clear();
}

// =====================================================================
// pluginxx::PluginHostLifecycle 的扩展点 (由本类覆写)
// =====================================================================

/// 生成 agent 侧实例对象
/// - 只做业务自引用 (self) 与所属管理器 (manager) 的设置;
/// - 基类自引用 (ownerSelf)、元信息/描述/路径、生命周期入口、生命周期控制块与
///   交给自己插件的 host 控制块由骨架的 makeInstance 统一加载。
std::shared_ptr<PluginInstance> PluginManager::createInstance(std::string name) {
    auto inst     = std::make_shared<PluginInstance>(std::move(name));
    inst->self    = inst;
    inst->manager = shared_from_this();
    return inst;
}

/// 摘除实例的业务注册 (工具 / 工具权限 / 图节点类型 / 提示词贡献 / 功能点 / 钩子处理器)
/// - 只摘除主程序侧生效的注册, 保留实例内的注册记录 (启用时由 start 事务重新声明);
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

    // 功能点: 撤销本实例登记的实现, 并摘除它自己声明的点
    if (auto ctx = agentContext_.lock(); ctx && ctx->features) {
        const std::string owner = fmt::format("plugin:{}", inst->name);
        for (const auto& pointId : inst->featurePointImpls) {
            ctx->features->removeImpl(pointId, owner);
        }
        for (const auto& pointId : inst->featurePoints) {
            ctx->features->undefinePluginPoint(pointId);
        }
    }

    for (const auto& graph : inst->graphNodeTypes) {
        if (graph.slot) {
            graph.slot->invalidate(inst);
        }
    }

    // 执行图定义独占 slot: 占用者卸载时恢复占用前的定义 ("回到内置")
    releaseGraphDefinitionSlot(inst);

    // 提示词: 删除本实例的全部贡献并重新合成受影响键 (不写回旧值, 不顶掉别人)
    // - 贡献的 owner 键就是实例名 (见 setPromptJson 的 record)
    removePromptContributions(inst->name);

    // 钩子处理器: 从注册表摘除本实例的全部处理器 (记录保留, 句柄置 0);
    // 没有处理器时派发器按需摘除 (轮次执行中先停用, 轮末真正摘除)
    detachHookHandlers(inst);
}

/// 摘除实例专属资源的所有权: 资源应用器上的启用标记 (钩子派发器是主程序级单例,
/// 不在实例名下, 处理器由 [detachHookHandlers] 从注册表摘除)
void PluginManager::detachDomainOwnedResources(PluginInstance* inst) {
    if (!inst) {
        return;
    }
    if (auto c = agentContext_.lock()) {
        if (c->resourceApplier) {
            c->resourceApplier->setOwnerEnabled(inst->name, false);
        }
    }
}

/// 清空"由插件 start 事务重新声明"的业务记录 (工具/权限/hook/图/功能点)
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
    inst->featurePoints.clear();
    inst->featurePointImpls.clear();
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

/// 实例加载完成: 解析插件侧的挂载/卸载入口 (`agentxx_plugin_agent_{mount,unmount}`)
///
/// - 两个符号都是**可选能力**: 缺失说明本插件不提供 agent 侧能力,
///   它的 `start` 里也不应该注册任何 agent 级实体 (会被 `ERR_NO_AGENT` 拒);
/// - 内置插件由框架下发的 `PluginxxBuiltinInfo` 直接给出函数指针
///   (合并编译时符号已改名, 不能按名查找)。
void PluginManager::onInstanceLoaded(PluginInstance& inst) {
    inst.mountFn   = nullptr;
    inst.unmountFn = nullptr;

    if (inst.dlHandle) {
        std::string symErr;
        void*       mountSym
            = pluginxx::NativeLoader::sym(inst.dlHandle, AGENTXX_PLUGIN_AGENT_SYMBOL_MOUNT, symErr);
        symErr.clear();
        void* unmountSym = pluginxx::NativeLoader::sym(
            inst.dlHandle,
            AGENTXX_PLUGIN_AGENT_SYMBOL_UNMOUNT,
            symErr
        );
        inst.mountFn   = reinterpret_cast<AgentxxPluginAgentMountFn>(mountSym);
        inst.unmountFn = reinterpret_cast<AgentxxPluginAgentUnmountFn>(unmountSym);
        return;
    }

    // 内置插件: 按名查内置描述
    uint64_t                   count = 0;
    const PluginxxBuiltinInfo* table = get_builtin_plugins(&count);
    for (uint64_t i = 0; i < count; ++i) {
        const auto& entry = table[i];
        if (entry.name.data == nullptr) {
            continue;
        }
        if (std::string_view{entry.name.data, static_cast<size_t>(entry.name.size)} != inst.name) {
            continue;
        }
        inst.mountFn   = reinterpret_cast<AgentxxPluginAgentMountFn>(entry.mount);
        inst.unmountFn = reinterpret_cast<AgentxxPluginAgentUnmountFn>(entry.unmount);
        break;
    }
}

// =====================================================================
// 加加载口 (旧签名 → 插件框架加载参数)
// =====================================================================

namespace {

/// 主程序插件配置 → 插件框架加载参数 (骨架只认 args 与 configPath 两个字段)
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

/// 记录加载耗时与注册计数
///
/// 在加加载口收尾时调用: 写回 [PluginInstance::loadMs] 并把"加载完成后到底注册了
/// 什么"打到日志, 便于回答"插件装了但模型看不到工具"一类问题;
/// 完整清单另见加载快照 (`--dump-config`)。
void notePluginLoaded(
    const std::shared_ptr<PluginInstance>& inst,
    std::chrono::steady_clock::time_point  begin
) {
    if (!inst) {
        return;
    }
    inst->loadMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - begin
    )
                                             .count());
    XX_LOGI(
        "Plugin `{}` loaded in {} ms: tools={} hooks={} graphNodes={} events={} capabilities={} "
        "permissionTools={} (enable={})",
        inst->name,
        inst->loadMs,
        inst->toolNames.size(),
        inst->hookRegistrations.size(),
        inst->graphNodeTypes.size(),
        inst->subscriptions.size(),
        inst->capabilityRegistrations.size(),
        inst->permissionToolNames.size(),
        inst->enabled
    );
}

/// 加载失败后记一句"怎么改"的建议
/// - 插件框架日志已经说明"哪一步失败了"; 这里补的是"照什么改" (路径/清单/导出符号),
///   只在失败路径调用, 正常加载不产生额外开销
/// - 加载失败本身不是错误用法 (例如插件只是没装), 因此按 WARN 记录
void logLoadAdvice(const PluginManager& mgr, std::string_view path) {
    const auto advice = mgr.diagnosePluginPath(path);
    if (!advice.empty()) {
        XX_LOGW("Plugin load failed: {}", advice);
    }
}

} // namespace

std::string PluginManager::diagnosePluginPath(std::string_view path) const {
    namespace fs = std::filesystem;
    const std::string pathStr{path};
    if (pathStr.empty()) {
        return "插件 path 为空: 在 agentxx-config.yaml 的 plugin.list[].path 里写插件目录、"
               "动态库路径或 builtin://<name>";
    }

    if (isBuiltinScheme(pathStr)) {
        const auto builtinName = parseBuiltinName(pathStr);
        if (builtinName.empty()) {
            return "builtin:// 后面没有插件名: 写法应为 builtin://<name> (如 builtin://agentxx_math)";
        }
        if (!findBuiltinPlugin(builtinName)) {
            return fmt::format(
                "内置注册表里没有插件 `{}`: 检查名字拼写, 或确认该插件参与了本次构建"
                "(内置插件以静态库形式合并进主程序, 见 plugins.md)",
                builtinName
            );
        }
        if (!findBuiltinManifest(builtinName)) {
            return fmt::format(
                "内置插件 `{}` 没有清单: 该插件未参与内置清单收集 (plugins/<name>/plugin.yaml)",
                builtinName
            );
        }
        return {};
    }

    std::error_code ec;
    fs::path        p{utilxx_base::utf8ToPath(pathStr)};
    if (!fs::exists(p, ec)) {
        return fmt::format(
            "插件路径不存在: `{}` (相对路径按进程当前工作目录解析; 安装布局一般在 "
            "<可执行文件目录>/plugins/<name>)",
            pathStr
        );
    }

    std::string libPath;
    if (fs::is_directory(p, ec)) {
        const auto manifestPath = p / "plugin.yaml";
        if (!fs::exists(manifestPath, ec)) {
            return fmt::format(
                "插件目录 `{}` 下没有 plugin.yaml: 清单至少要写 name 与 entry "
                "(entry 指向 agent 侧动态库文件名; 见 plugins.md 的清单字段)",
                pathStr
            );
        }
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
            return fmt::format(
                "plugin.yaml 解析失败或不完整: `{}` (检查 YAML 语法缩进与 name/entry 字段; "
                "解析失败时加载会被拒绝且不产生任何注册)",
                manifestPath.string()
            );
        }
        libPath = resolvePluginEntryPath(p, manifestEntry);
        if (!fs::exists(utilxx_base::utf8ToPath(libPath), ec)) {
            const auto fallback = p / manifestName;
            return fmt::format(
                "清单 entry `{}` 指向的库文件不存在: `{}` (该目录下应放插件动态库, "
                "如 `{}` + 平台后缀)",
                manifestEntry,
                libPath,
                fallback.string()
            );
        }
    } else {
        libPath = pathStr;
    }

    // 导出符号检查: 缺少入口符号是"插件编译方式不对"的最常见原因,
    // 这里直接把缺哪个符号、应该怎么导出说清楚 (短暂打开库文件后立即关闭)
    std::string openErr;
    void*       handle = pluginxx::NativeLoader::open(libPath, openErr);
    if (!handle) {
        return fmt::format("无法打开插件动态库 `{}`: {}", libPath, openErr);
    }
    std::vector<std::string> missing;
    {
        std::string symErr;
        if (!pluginxx::NativeLoader::sym(
                handle,
                std::string{AGENTXX_PLUGIN_AGENT_SYMBOL_CREATE}.c_str(),
                symErr
            )) {
            missing.push_back(std::string{AGENTXX_PLUGIN_AGENT_SYMBOL_CREATE});
        }
        symErr.clear();
        if (!pluginxx::NativeLoader::sym(
                handle,
                std::string{AGENTXX_PLUGIN_AGENT_SYMBOL_START}.c_str(),
                symErr
            )) {
            missing.push_back(std::string{AGENTXX_PLUGIN_AGENT_SYMBOL_START});
        }
        symErr.clear();
        if (!pluginxx::NativeLoader::sym(
                handle,
                std::string{AGENTXX_PLUGIN_AGENT_SYMBOL_STOP}.c_str(),
                symErr
            )) {
            missing.push_back(std::string{AGENTXX_PLUGIN_AGENT_SYMBOL_STOP});
        }
    }
    pluginxx::NativeLoader::close(handle);

    if (!missing.empty()) {
        std::string list;
        for (size_t i = 0; i < missing.size(); ++i) {
            list += (i == 0 ? "" : ", ") + missing[i];
        }
        return fmt::format(
            "动态库 `{}` 缺少主程序入口符号: {} (插件必须导出 create/destroy/start/stop 五个入口; "
            "C++ 插件用 AGENTXX_PLUGIN_AGENT_EXPORT 宏一次生成, 手写 create/destroy 的用 "
            "AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT 补 start/stop; 另外检查导出白名单是否把入口"
            "符号也隐藏了)",
            libPath,
            list
        );
    }
    return {};
}

asio::awaitable<std::shared_ptr<PluginInstance>> PluginManager::loadNativeAsync(
    std::string                             path,
    const agentxx::agent::PluginConfig*     cfg,
    bool                                    allowClientOnlySkip,
    const plugin::PluginManifestResources&  resources,
    const plugin::PluginManifestInterfaces& interfaces
) {
    const auto                  begin = std::chrono::steady_clock::now();
    pluginxx::PluginLoadOptions options;
    bool                        hasOptions = false;
    toLoadOptions(cfg, options, hasOptions);
    auto inst = co_await pluginxx::PluginHostLifecycle<PluginInstance>::loadNativeAsync(
        std::move(path),
        hasOptions ? &options : nullptr,
        allowClientOnlySkip,
        resources,
        interfaces
    );
    notePluginLoaded(inst, begin);
    if (!inst && !allowClientOnlySkip) {
        logLoadAdvice(*this, path);
    }
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
    const auto                  begin = std::chrono::steady_clock::now();
    pluginxx::PluginLoadOptions options;
    bool                        hasOptions = false;
    toLoadOptions(cfg, options, hasOptions);
    const std::string pathForAdvice = path;
    auto inst = co_await pluginxx::PluginHostLifecycle<PluginInstance>::loadBuiltinAsync(
        std::move(name),
        std::move(path),
        std::move(depends),
        std::move(optionalDepends),
        hasOptions ? &options : nullptr,
        resources,
        interfaces
    );
    notePluginLoaded(inst, begin);
    if (!inst) {
        logLoadAdvice(*this, pathForAdvice);
    }
    co_return inst;
}

asio::awaitable<std::shared_ptr<PluginInstance>> PluginManager::loadPluginAsync(
    std::string                         path,
    const agentxx::agent::PluginConfig* cfg,
    bool                                allowClientOnlySkip
) {
    const auto                  begin = std::chrono::steady_clock::now();
    pluginxx::PluginLoadOptions options;
    bool                        hasOptions = false;
    toLoadOptions(cfg, options, hasOptions);
    const std::string pathForAdvice = path;
    auto inst = co_await pluginxx::PluginHostLifecycle<PluginInstance>::loadPluginAsync(
        std::move(path),
        hasOptions ? &options : nullptr,
        allowClientOnlySkip
    );
    notePluginLoaded(inst, begin);
    if (!inst && !allowClientOnlySkip) {
        logLoadAdvice(*this, pathForAdvice);
    }
    // 本 agent 已经建立挂载时, 运行期新加载的插件直接挂到它身上
    // (先加载后挂载的正常路径由 init 的 `plugin_mount` 步骤统一驱动)
    if (inst && hasMount_) {
        co_await mountAgentAsync();
    }
    co_return inst;
}

// =====================================================================
// 查询
// =====================================================================

std::vector<PluginManager::PluginListView> PluginManager::list() const {
    std::vector<PluginListView> out;
    out.reserve(plugins_.size());
    for (const auto& [name, inst] : plugins_) {
        PluginListView view;
        view.name                  = inst->name;
        view.version               = inst->version;
        view.description           = inst->description;
        view.path                  = inst->path;
        view.configPath            = inst->configPath;
        view.enabled               = inst->enabled;
        view.inflight              = inst->lifetime ? inst->lifetime->leaseCount() : 0;
        view.tools                 = inst->toolNames;
        view.depends               = inst->depends;
        view.optionalDepends       = inst->optionalDepends;
        view.requiredInterfaces    = inst->interfaces.require;
        view.optionalInterfaces    = inst->interfaces.optional;
        view.userDisabled          = inst->userDisabled;
        view.blockedByDependencies = inst->blockedByDependencies;
        view.loadMs                = inst->loadMs;
        // 钩子: 注册表里生效中的条数 + 各处理器优先级 (按派发顺序, 一眼看出谁先跑)
        view.hookCount = liveHookHandlersOf(*inst);
        for (const auto& handler : allHookHandlersOf(*inst)) {
            view.hookPriorities.push_back(handler.priority);
        }
        view.graphNodeCount      = inst->graphNodeTypes.size();
        view.eventSubCount       = inst->subscriptions.size();
        view.permissionToolCount = inst->permissionToolNames.size();
        {
            const auto inventory     = registrationInventory(*inst);
            view.registrationTotal   = inventory.total();
            view.promptKeyCount      = inventory.promptKeys;
            view.skillDirCount       = inventory.skillDirs;
            view.memoryFileCount     = inventory.memoryFiles;
            view.mcpNamespaceCount   = inventory.mcpNamespaces;
            view.ownsGraphDefinition = inventory.ownsGraphDefinition;
            view.featurePointCount   = inventory.featurePoints;
            view.featureImplCount    = inventory.featureImpls;
        }
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
// 配置驱动加载 (agentxx 自有: 输入是主程序配置条目, 与插件框架加载参数解耦)
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
