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
// 实例的挂载记录视图
// =====================================================================

bool PluginInstance::anyMounted() const {
    for (const auto& weak : mounts) {
        auto mount = weak.lock();
        if (mount == nullptr) {
            continue;
        }
        if (const auto* entry = mount->findEntry(this); entry != nullptr && entry->pluginMounted) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> PluginInstance::allToolNames() const {
    std::vector<std::string> out;
    for (const auto& weak : mounts) {
        auto mount = weak.lock();
        if (mount == nullptr) {
            continue;
        }
        if (const auto* entry = mount->findEntry(this); entry != nullptr) {
            out.insert(out.end(), entry->tools.begin(), entry->tools.end());
        }
    }
    return out;
}

std::vector<std::shared_ptr<PluginTool>> PluginInstance::allToolObjects() const {
    std::vector<std::shared_ptr<PluginTool>> out;
    for (const auto& weak : mounts) {
        auto mount = weak.lock();
        if (mount == nullptr) {
            continue;
        }
        if (const auto* entry = mount->findEntry(this); entry != nullptr) {
            out.insert(out.end(), entry->toolObjects.begin(), entry->toolObjects.end());
        }
    }
    return out;
}

std::vector<int64_t> PluginInstance::allHookHandles() const {
    std::vector<int64_t> out;
    for (const auto& weak : mounts) {
        auto mount = weak.lock();
        if (mount == nullptr) {
            continue;
        }
        if (const auto* entry = mount->findEntry(this); entry != nullptr) {
            out.insert(out.end(), entry->hookHandles.begin(), entry->hookHandles.end());
        }
    }
    return out;
}

std::vector<std::shared_ptr<GraphTypeSlot>> PluginInstance::allGraphSlots() const {
    std::vector<std::shared_ptr<GraphTypeSlot>> out;
    for (const auto& weak : mounts) {
        auto mount = weak.lock();
        if (mount == nullptr) {
            continue;
        }
        if (const auto* entry = mount->findEntry(this); entry != nullptr) {
            for (const auto& graph : entry->graphNodes) {
                out.push_back(graph.slot);
            }
        }
    }
    return out;
}

// =====================================================================
// 管理器构造 / 析构
// =====================================================================

PluginManager::PluginManager(std::weak_ptr<agentxx::agent::AgentContext> primaryAgent) :
    primaryAgent_(std::move(primaryAgent)) {
    // 通用表 (events/scheduler/tasks/capabilities 等) 需要主程序数据的入口经本类取数
    // (见 pluginxx/host/domain_hooks.h): 在任何 vtable 入口被调用之前注入。
    setDomainHooks(this);
}

// =====================================================================
// 挂载表 (一个 agent 一个挂载)
// =====================================================================

std::vector<std::shared_ptr<PluginMount>> PluginManager::mounts() const {
    return mounts_;
}

std::shared_ptr<PluginMount> PluginManager::findMountById(std::string_view agentId) const {
    if (agentId.empty()) {
        return nullptr;
    }
    for (const auto& mount : mounts_) {
        if (mount && mount->agentId() == agentId) {
            return mount;
        }
    }
    return nullptr;
}

PluginMount* PluginManager::mountOf(const PluginxxScopeRef* ref) const {
    if (!pluginxx_scope_ref_ok(ref) || ref == nullptr) {
        return nullptr;
    }
    if (ref->id.data == nullptr || ref->id.size == 0) {
        return nullptr;
    }
    const std::string_view agentId{ref->id.data, static_cast<size_t>(ref->id.size)};
    auto                   mount = findMountById(agentId);
    if (!mount) {
        return nullptr;
    }
    // seq = 0 表示不校验 (FFI / 测试自行构造引用时可用)
    if (ref->seq != 0 && ref->seq != mount->mountSeq()) {
        return nullptr;
    }
    return mount.get();
}

std::shared_ptr<PluginMount>
    PluginManager::attachMountSync(std::shared_ptr<agentxx::agent::AgentContext> ctx) {
    if (!ctx) {
        return nullptr;
    }
    const std::string agentId = ctx->agentId.empty() ? std::string{"root"} : ctx->agentId;
    if (auto existing = findMountById(agentId); existing != nullptr) {
        return existing;
    }
    const std::string agentName = ctx->agentConfig ? ctx->agentConfig->agentName : std::string{};
    auto mount = std::make_shared<PluginMount>(
        shared_from_this(),
        ctx,
        agentId,
        agentName,
        nextMountSeq_++
    );
    mounts_.push_back(mount);
    return mount;
}

asio::awaitable<std::shared_ptr<PluginMount>>
    PluginManager::mountAgentAsync(std::shared_ptr<agentxx::agent::AgentContext> ctx) {
    if (!ctx) {
        co_return nullptr;
    }
    // agent 标识: 由 AgentHost / 启动方在上下文里指定; 未指定时按根 agent 处理
    const std::string agentId = ctx->agentId.empty() ? std::string{"root"} : ctx->agentId;
    auto              mount   = findMountById(agentId);
    if (mount) {
        // 同一 agent 已经在挂载表里: 只补挂新加载的实例 (幂等)
        if (mount->agentContext().get() != ctx.get()) {
            XX_LOGE(
                "agent '{}' 已有插件挂载, 但传入的上下文不是同一个 agent; 拒绝重复挂载",
                agentId
            );
            co_return nullptr;
        }
    } else {
        const std::string agentName
            = ctx->agentConfig ? ctx->agentConfig->agentName : std::string{};
        mount = std::make_shared<PluginMount>(
            shared_from_this(),
            ctx,
            agentId,
            agentName,
            nextMountSeq_++
        );
        mounts_.push_back(mount);
    }

    // 逐实例驱动插件的 mount 回调 (顺序 = 实例表顺序; 单个实例失败只影响它自己)
    // 挂载范围由本 agent 的策略决定 (yaml `plugin.mount`)
    const auto policy = ctx->agentConfig ? ctx->agentConfig->pluginMountPolicy
                                         : agentxx::agent::PluginMountPolicy::All;
    const auto& listed = ctx->agentConfig ? ctx->agentConfig->pluginMountList
                                          : std::vector<std::string>{};
    for (const auto& [name, inst] : this->plugins_) {
        (void)name;
        if (!inst) {
            continue;
        }
        if (policy == agentxx::agent::PluginMountPolicy::None) {
            break;
        }
        if (policy == agentxx::agent::PluginMountPolicy::Listed
            && std::find(listed.begin(), listed.end(), inst->name) == listed.end()) {
            continue;
        }
        co_await mount->mountOneAsync(inst);
    }
    XX_LOGI(
        "agent '{}' 完成插件挂载 (seq {}, {} 个实例)",
        mount->agentId(),
        mount->mountSeq(),
        mount->mountedCount()
    );
    co_return mount;
}

asio::awaitable<void> PluginManager::unmountAgentAsync(std::string_view agentId) {
    auto mount = findMountById(agentId);
    if (!mount) {
        co_return;
    }
    co_await mount->unmountAllAsync();
    std::erase(mounts_, mount);
    XX_LOGI("agent '{}' 已拆全部插件挂载", mount->agentId());
    co_return;
}

asio::awaitable<void> PluginManager::unmountAllAgentsAsync() {
    auto snapshot = mounts_;
    for (const auto& mount : snapshot) {
        if (mount) {
            co_await mount->unmountAllAsync();
        }
    }
    mounts_.clear();
    co_return;
}

asio::awaitable<bool> PluginManager::mountOneAsync(
    const std::shared_ptr<PluginMount>& mount,
    std::shared_ptr<PluginInstance>     inst
) {
    if (!mount) {
        co_return false;
    }
    co_return co_await mount->mountOneAsync(std::move(inst));
}

asio::awaitable<void> PluginManager::unmountOneAsync(
    const std::shared_ptr<PluginMount>& mount,
    std::shared_ptr<PluginInstance>     inst
) {
    if (!mount) {
        co_return;
    }
    co_await mount->unmountOneAsync(std::move(inst));
    co_return;
}

PluginMount* PluginManager::soleMount() const {
    if (mounts_.size() != 1 || !mounts_.front()) {
        return nullptr;
    }
    return mounts_.front().get();
}

std::string PluginManager::agentId() const {
    auto* mount = soleMount();
    return mount != nullptr ? mount->agentId() : std::string{};
}

uint64_t PluginManager::mountSequence() const {
    auto* mount = soleMount();
    return mount != nullptr ? mount->mountSeq() : 0;
}

std::shared_ptr<ToolRegistry> PluginManager::registry() const {
    if (auto ctx = primaryAgent_.lock(); ctx && ctx->toolRegistry) {
        return ctx->toolRegistry;
    }
    static const std::shared_ptr<ToolRegistry> kEmpty = std::make_shared<ToolRegistry>();
    return kEmpty;
}

PluginMount::RegistrationInventory
    PluginManager::registrationInventory(const PluginInstance& inst) const {
    PluginMount::RegistrationInventory out;
    for (const auto& mount : mounts_) {
        if (!mount || mount->findEntry(&inst) == nullptr) {
            continue;
        }
        const auto one = mount->registrationInventory(inst);
        out.tools += one.tools;
        out.permissionTools += one.permissionTools;
        out.hooks += one.hooks;
        out.graphNodeTypes += one.graphNodeTypes;
        out.eventSubscriptions += one.eventSubscriptions;
        out.capabilities += one.capabilities;
        out.promptKeys += one.promptKeys;
        out.skillDirs += one.skillDirs;
        out.memoryFiles += one.memoryFiles;
        out.mcpNamespaces += one.mcpNamespaces;
        out.featurePoints += one.featurePoints;
        out.featureImpls += one.featureImpls;
        out.ownsGraphDefinition = out.ownsGraphDefinition || one.ownsGraphDefinition;
    }
    return out;
}

std::string PluginManager::graphDefinitionOwner() const {
    for (const auto& mount : mounts_) {
        if (mount && !mount->graphDefinitionOwner().empty()) {
            return mount->graphDefinitionOwner();
        }
    }
    return {};
}

std::string PluginManager::hooksJson() {
    if (auto* mount = soleMount(); mount != nullptr) {
        return mount->hooksJson();
    }
    utilxx_base::Json arr = utilxx_base::Json::array();
    for (const auto& mount : mounts_) {
        if (mount) {
            arr.push_back(utilxx_base::Json::parse(mount->hooksJson()));
        }
    }
    return arr.dump();
}

std::vector<PluginMount::HookHandlerView>
    PluginManager::handlersOf(AgentxxPluginHookPoint point) const {
    std::vector<PluginMount::HookHandlerView> out;
    for (const auto& mount : mounts_) {
        if (!mount) {
            continue;
        }
        const auto one = mount->handlersOf(point);
        out.insert(out.end(), one.begin(), one.end());
    }
    return out;
}

int64_t PluginManager::addCoreHookHandler(
    AgentxxPluginHookPoint point,
    int32_t                priority,
    std::string            module,
    AgentxxPluginHookSpec  spec
) {
    auto* mount = soleMount();
    if (mount == nullptr) {
        XX_LOGW(
            "addCoreHookHandler: 需要恰好一份挂载 (当前 {} 份), 本次登记被忽略",
            mounts_.size()
        );
        return 0;
    }
    return mount->addCoreHookHandler(point, priority, std::move(module), spec);
}

bool PluginManager::removeCoreHookHandler(int64_t handle) {
    for (const auto& mount : mounts_) {
        if (mount && mount->removeCoreHookHandler(handle)) {
            return true;
        }
    }
    return false;
}

void PluginManager::onTurnBegin() {
    for (const auto& mount : mounts_) {
        if (mount) {
            mount->onTurnBegin();
        }
    }
}

void PluginManager::onTurnEnd() {
    for (const auto& mount : mounts_) {
        if (mount) {
            mount->onTurnEnd();
        }
    }
}

void PluginManager::flushPendingCleanup() {
    for (const auto& mount : mounts_) {
        if (mount) {
            mount->flushPendingCleanup();
        }
    }
}

std::string PluginManager::listFeaturePoints() {
    if (auto* mount = soleMount(); mount != nullptr) {
        return mount->listFeaturePoints();
    }
    utilxx_base::Json arr = utilxx_base::Json::array();
    for (const auto& mount : mounts_) {
        if (mount) {
            arr.push_back(utilxx_base::Json::parse(mount->listFeaturePoints()));
        }
    }
    return arr.dump();
}

std::string PluginManager::listMountsJson() {
    utilxx_base::Json arr = utilxx_base::Json::array();
    for (const auto& mount : mounts_) {
        if (mount) {
            arr.push_back(mount->infoJson());
        }
    }
    return arr.dump();
}

std::string PluginManager::mountInfoJson(std::string_view agentId) {
    auto mount = findMountById(agentId);
    if (!mount) {
        return {};
    }
    return mount->infoJson().dump();
}

// =====================================================================
// 禁用/启用与挂载的联动 (骨架完成 stop/start 后由本类补齐挂载动作)
// =====================================================================

asio::awaitable<void> PluginManager::onInstanceDisabledAsync(
    const std::shared_ptr<PluginInstance>& inst
) {
    // 禁用 = 所有 agent 都不再挂这个插件: 逐个挂载拆掉 (调插件 unmount 释放
    // per-agent 状态), 实例本身不销毁、不卸载; 注册已由骨架的
    // detachInstanceRegistrations 摘除。
    //
    // 等待期间用户又启用了它 (disable/enable 紧邻): 本次卸载作废 —— 否则会把
    // 启用事务刚刚重建的注册与能力又摘一遍。
    if (!inst || inst->enabled) {
        co_return;
    }
    auto snapshot = mounts_;
    for (const auto& mount : snapshot) {
        if (mount) {
            co_await mount->unmountOneAsync(inst);
        }
    }
    co_return;
}

asio::awaitable<void> PluginManager::onInstanceEnabledAsync(
    const std::shared_ptr<PluginInstance>& inst
) {
    // 启用 = 重新挂到全部挂载 (agent 级注册都在插件的 mount 回调里)
    //
    // 只做"没挂就挂": 禁用时已经拆过挂载 (框架的 detachInstanceRegistrations →
    // [PluginMount::detachInstance]), 这里再拆一次会把 start 事务刚登记的注册又摘掉。
    if (!inst || !inst->enabled || mounts_.empty()) {
        co_return;
    }
    auto snapshot = mounts_;
    for (const auto& mount : snapshot) {
        if (!mount) {
            continue;
        }
        co_await mount->mountOneAsync(inst); // 幂等: 已挂载时直接返回
        if (!inst->enabled) {
            co_return; // 挂载期间又被禁用了
        }
    }
    co_return;
}

PluginManager::~PluginManager() {
    // 同步卸载全部实例 (骨架实现: 仍有 lease / 欠 stop 的实例保留上下文与 DSO)
    shutdownAll();
    warnPendingCloseOnDestroy("agent");
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

/// 摘除实例的业务注册: 让本实例从**全部挂载**上摘掉
///
/// 禁用 / 卸载路径进入这里: 只摘除主程序侧生效的注册, 保留每个挂载上的记录
/// (启用时插件会在自己的 mount 回调里重新登记);
/// 事件订阅与能力声明的撤销属通用部分, 由骨架的 detachAll 处理。
void PluginManager::detachDomainRegistrations(PluginInstance* inst) {
    if (!inst) {
        return;
    }
    for (const auto& mount : mounts_) {
        if (mount) {
            mount->detachInstance(inst);
        }
    }
}

/// 摘除实例专属资源的所有权: 各挂载资源应用器上的启用标记 (钩子派发器属于具体
/// 挂载, 处理器由 [PluginMount::detachInstance] 从该挂载的注册表摘除)
void PluginManager::detachDomainOwnedResources(PluginInstance* inst) {
    if (!inst) {
        return;
    }
    for (const auto& mount : mounts_) {
        if (!mount) {
            continue;
        }
        if (auto ctx = mount->agentContext(); ctx && ctx->resourceApplier) {
            ctx->resourceApplier->setOwnerEnabled(inst->name, false);
        }
    }
}

/// 清空"由插件 start 事务重新声明"的业务记录 (各挂载上的注册记录)
/// - 通用记录 (事件订阅/能力声明) 由骨架的 clearPluginOwnedRegistrations 清空;
/// - stop 成功后调用, 避免下次 start 在旧记录上重复累积。
void PluginManager::clearDomainRegistrations(PluginInstance* inst) {
    if (!inst) {
        return;
    }
    for (const auto& mount : mounts_) {
        if (mount) {
            mount->clearInstanceRecords(inst);
        }
    }
}

/// 卸载时释放实例级资源: 各挂载上的工具对象 + 清单资源所有权
void PluginManager::releaseInstanceResources(PluginInstance& inst) {
    for (const auto& mount : mounts_) {
        if (!mount) {
            continue;
        }
        mount->releaseInstanceInstances(inst);
        if (auto ctx = mount->agentContext(); ctx && ctx->resourceApplier) {
            ctx->resourceApplier->removeAllOwned(inst.name);
        }
    }
}

/// 启用状态变化通知: 同步各挂载资源应用器上的启用标记
void PluginManager::onInstanceEnabledChanged(PluginInstance& inst, bool enabled) {
    for (const auto& mount : mounts_) {
        if (!mount) {
            continue;
        }
        if (auto ctx = mount->agentContext(); ctx && ctx->resourceApplier) {
            ctx->resourceApplier->setOwnerEnabled(inst.name, enabled);
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

/// 记录加载耗时与进程级注册计数
///
/// 在加载入口收尾时调用: 写回 [PluginInstance::loadMs] 并把"加载完成后到底注册了
/// 什么"打到日志 —— 此时还没有任何挂载, 因此只可能有事件订阅/能力这类进程级登记;
/// agent 级注册 (工具/钩子/图/资源) 在插件 `mount` 回调里才发生。
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
        "Plugin `{}` loaded in {} ms: events={} capabilities={} mount={} (enable={})",
        inst->name,
        inst->loadMs,
        inst->subscriptions.size(),
        inst->capabilityRegistrations.size(),
        inst->mountFn != nullptr,
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
    // 已经建立挂载时, 运行期新加载的插件直接挂到这些 agent 上
    // (先加载后挂载的正常路径由 init 的 `plugin_mount` 步骤统一驱动)
    if (inst && !mounts_.empty()) {
        auto snapshot = mounts_;
        for (const auto& mount : snapshot) {
            if (mount) {
                co_await mount->mountOneAsync(inst);
            }
        }
    }
    co_return inst;
}

// =====================================================================
// 查询
// =====================================================================

namespace {

/// 与 agent 无关的清单字段 (加载元信息 + 进程级登记)
void fillInstanceView(PluginManager::PluginListView& view, const PluginInstance& inst) {
    view.name                  = inst.name;
    view.version               = inst.version;
    view.description           = inst.description;
    view.path                  = inst.path;
    view.configPath            = inst.configPath;
    view.enabled               = inst.enabled;
    view.inflight              = inst.lifetime ? inst.lifetime->leaseCount() : 0;
    view.depends               = inst.depends;
    view.optionalDepends       = inst.optionalDepends;
    view.requiredInterfaces    = inst.interfaces.require;
    view.optionalInterfaces    = inst.interfaces.optional;
    view.userDisabled          = inst.userDisabled;
    view.blockedByDependencies = inst.blockedByDependencies;
    view.loadMs                = inst.loadMs;
    view.eventSubCount         = inst.subscriptions.size();
    view.manifestResourceCount = inst.manifestResources.entries.size();
    for (const auto& cap : inst.capabilityRegistrations) {
        view.capabilities.push_back(cap.name);
    }
}

/// 把一个挂载上的计数并入清单行 (没有该挂载的记录 = 跳过)
void mergeMountView(
    PluginManager::PluginListView& view,
    const PluginMount&            mount,
    const PluginInstance&         inst
) {
    const auto* entry = mount.findEntry(&inst);
    if (entry == nullptr) {
        return;
    }
    view.mountedAgents.push_back(mount.agentId());
    for (const auto& name : entry->tools) {
        if (std::find(view.tools.begin(), view.tools.end(), name) == view.tools.end()) {
            view.tools.push_back(name);
        }
    }
    const auto handlers = mount.hookHandlersOf(inst);
    view.hookCount += handlers.size();
    for (const auto& handler : handlers) {
        view.hookPriorities.push_back(handler.priority);
    }
    view.graphNodeCount += entry->graphNodes.size();
    view.permissionToolCount += entry->permissionTools.size();
    const auto inventory       = mount.registrationInventory(inst);
    view.registrationTotal += inventory.total();
    view.promptKeyCount += inventory.promptKeys;
    view.skillDirCount += inventory.skillDirs;
    view.memoryFileCount += inventory.memoryFiles;
    view.mcpNamespaceCount += inventory.mcpNamespaces;
    view.featurePointCount += inventory.featurePoints;
    view.featureImplCount += inventory.featureImpls;
    view.ownsGraphDefinition = view.ownsGraphDefinition || inventory.ownsGraphDefinition;
}

} // namespace

std::vector<PluginManager::PluginListView> PluginManager::list() const {
    std::vector<PluginListView> out;
    out.reserve(plugins_.size());
    for (const auto& [name, inst] : plugins_) {
        (void)name;
        if (!inst) {
            continue;
        }
        PluginListView view;
        fillInstanceView(view, *inst);
        for (const auto& mount : mounts_) {
            if (mount) {
                mergeMountView(view, *mount, *inst);
            }
        }
        view.mountCount = view.mountedAgents.size();
        out.push_back(std::move(view));
    }
    return out;
}

std::vector<PluginManager::PluginListView> PluginManager::listForMount(const PluginMount& mount) const {
    std::vector<PluginListView> out;
    for (const auto& [name, inst] : plugins_) {
        (void)name;
        if (!inst || mount.findEntry(inst.get()) == nullptr) {
            continue;
        }
        PluginListView view;
        fillInstanceView(view, *inst);
        mergeMountView(view, mount, *inst);
        view.mountCount = view.mountedAgents.size();
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
        // 挂载情况 (进程一份实例, 可以挂到多个 agent 上; 也为 0 表示尚未挂载)
        item["mount_count"]  = v.mountCount;
        item["mount_agents"] = v.mountedAgents;
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
    // 工具按挂载汇总 (与 listPluginsJson 同一来源: 实例本身的只读视图不带工具)
    PluginListView view;
    fillInstanceView(view, *inst);
    for (const auto& mount : mounts_) {
        if (mount) {
            mergeMountView(view, *mount, *inst);
        }
    }
    item["tools"]               = view.tools;
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
