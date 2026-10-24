///
/// 挂载的生命周期: 建立挂载 (逐实例驱动插件 `mount`)、拆挂载 (驱动 `unmount` 并按
/// 记录撤销本 agent 上的实体)、清单资源的应用与撤销、以及挂载级诊断。
///
/// - 一个 agent 一个挂载 ([PluginMount]), 同进程里可以有多个;
/// - 全部动作只在主程序 io 线程执行 (注册入口由 vtable 层投递, 插件回调由主程序驱动);
/// - 撤销顺序固定 (见 [PluginMount::revokeEntry]): 先断事件与资源, 再恢复图定义、
///   摘注册, 最后摘工具 —— 单一栈式回滚会漏掉类别之间的先后依赖。
#include "agentxx/plugin/plugin_mount.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/resource_applier.h"
#include "agentxx/feature/registry.h"
#include "agentxx/middlewares/permission.h"
#include "agentxx/plugin/plugin_graph_node.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/util/exception.h"
#include "asio/this_coro.hpp"
#include "fmt/format.h"
#include "pluginxx/host/event_bus.h"
#include "pluginxx/runtime/op_driver.h"
#include "utilxx_base/log.h"

#include <algorithm>

namespace agentxx {
namespace plugin {

// =====================================================================
// 构造 / 析构
// =====================================================================

PluginMount::PluginMount(
    std::shared_ptr<PluginManager>              host,
    std::weak_ptr<agentxx::agent::AgentContext> agent,
    std::string                                 agentId,
    std::string                                 agentName,
    uint64_t                                    mountSeq
) :
    host_(std::move(host)),
    agent_(std::move(agent)),
    agentId_(std::move(agentId)),
    agentName_(std::move(agentName)),
    mountSeq_(mountSeq) {}

PluginMount::~PluginMount() {
    if (!entries_.empty()) {
        // 正常路径下 agent 关闭会先走 unmountAllAsync; 走到这里说明挂载被直接释放,
        // 插件侧没有收到 unmount 通知 (记一条便于定位)
        XX_LOGW("插件挂载 '{}' 析构时仍有 {} 个实例未拆挂载", agentId_, entries_.size());
    }
}

std::shared_ptr<ToolRegistry> PluginMount::toolRegistry() const {
    if (auto ctx = agent_.lock(); ctx && ctx->toolRegistry) {
        return ctx->toolRegistry;
    }
    static const std::shared_ptr<ToolRegistry> kEmpty = std::make_shared<ToolRegistry>();
    return kEmpty;
}

agentxx::middleware::PermissionMiddlewareHandle* PluginMount::permissionMiddleware() {
    auto ctx = agent_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        return nullptr;
    }
    for (const auto& handle : ctx->middlewareHandleContext->handles) {
        if (auto* permission
            = dynamic_cast<agentxx::middleware::PermissionMiddlewareHandle*>(handle.get())) {
            return permission;
        }
    }
    return nullptr;
}

bool PluginMount::acceptsRegistration(const PluginInstance* inst) {
    if (inst == nullptr || !inst->enabled) {
        return false;
    }
    if (!inst->lifetime) {
        return true;
    }
    // 与插件框架同一判定: 只有 Loading / Ready 两个状态接受登记
    return inst->lifetime->acceptsRegistration();
}

bool PluginMount::resourcesFrozen(const PluginInstance* inst) const {
    const auto* entry = findEntry(inst);
    // 没挂在本 agent 上的实例: 视为冻结 (拒绝登记), 调用点返回失败
    return entry == nullptr ? true : entry->resourcesFrozen;
}

// =====================================================================
// 实例记录
// =====================================================================

PluginMount::Entry* PluginMount::findEntry(const PluginInstance* inst) {
    if (inst == nullptr) {
        return nullptr;
    }
    for (auto& entry : entries_) {
        if (entry.inst.get() == inst) {
            return &entry;
        }
    }
    return nullptr;
}

const PluginMount::Entry* PluginMount::findEntry(const PluginInstance* inst) const {
    return const_cast<PluginMount*>(this)->findEntry(inst);
}

PluginMount::Entry& PluginMount::entryOf(PluginInstance* inst) {
    auto* entry = findEntry(inst);
    if (entry != nullptr) {
        return *entry;
    }
    entries_.push_back(Entry{});
    entry       = &entries_.back();
    entry->inst = std::static_pointer_cast<PluginInstance>(inst->self.lock());
    if (!entry->inst) {
        entry->inst = std::shared_ptr<PluginInstance>(inst, [](PluginInstance*) {});
    }
    // 反向登记: 实例的只读视图 (allToolNames/allGraphSlots 等) 按挂载汇总,
    // 漏了这一步的记录在实例侧看不到 (测试与 attachMountSync 后的登记走这里)
    if (std::find_if(
            inst->mounts.begin(),
            inst->mounts.end(),
            [this](const std::weak_ptr<PluginMount>& item) {
                return item.lock().get() == this;
            }
        )
        == inst->mounts.end()) {
        inst->mounts.push_back(weak_from_this());
    }
    return *entry;
}

std::vector<PluginMount::HookHandlerView> PluginMount::hookHandlersOf(const PluginInstance& inst) const {
    return allHookHandlersOf(inst);
}

size_t PluginMount::mountedCount() const {
    return static_cast<size_t>(
        std::count_if(entries_.begin(), entries_.end(), [](const Entry& entry) {
            return entry.pluginMounted;
        })
    );
}

bool PluginMount::mountedElsewhere(const PluginInstance* inst) const {
    if (inst == nullptr) {
        return false;
    }
    for (const auto& weak : inst->mounts) {
        auto other = weak.lock();
        if (!other || other.get() == this) {
            continue;
        }
        const auto* entry = other->findEntry(inst);
        if (entry != nullptr && entry->pluginMounted) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> PluginMount::mountedNames() const {
    std::vector<std::string> out;
    for (const auto& entry : entries_) {
        if (entry.inst && entry.pluginMounted) {
            out.push_back(entry.inst->name);
        }
    }
    return out;
}

utilxx_base::Json PluginMount::infoJson() {
    utilxx_base::Json item;
    item["agentId"]   = agentId_;
    item["agentName"] = agentName_;
    utilxx_base::Json names = utilxx_base::Json::array();
    for (const auto& name : mountedNames()) {
        names.push_back(name);
    }
    item["plugins"] = std::move(names);
    item["mountSeq"] = mountSeq_;
    if (auto ctx = agent_.lock()) {
        item["workDir"]  = ctx->getSessionBaseWorkDir(std::string{});
        item["language"] = ctx->agentConfig ? ctx->agentConfig->language : std::string{};
    }
    return item;
}

// =====================================================================
// 建立挂载
// =====================================================================

asio::awaitable<bool> PluginMount::mountOneAsync(std::shared_ptr<PluginInstance> inst) {
    if (!inst) {
        co_return false;
    }
    if (auto* existing = findEntry(inst.get()); existing != nullptr && existing->pluginMounted) {
        co_return true; // 幂等
    }
    // 实例已在关闭流程里 (卸载/主程序关闭): 不再挂载, 否则会把刚摘掉的注册又加回去
    if (inst->lifetime && inst->lifetime->closeRequested()) {
        co_return false;
    }
    auto host = host_.lock();
    auto ctx  = agent_.lock();
    if (!host || !ctx) {
        co_return false;
    }

    // 记录先建出来: 挂载期间的注册都要往它上面记账 (entryOf 同时登记反向引用)
    auto* entry            = &entryOf(inst.get());
    entry->mountUserData   = nullptr;
    entry->pluginMounted   = false;
    entry->unmountPending  = false;
    entry->resourcesFrozen = false;
    entry->configJson      = inst->args.dump();

    // 清单声明的资源 (skill/memory/mcp) 属于本挂载: 先应用, 插件的 mount 回调里
    // 还能继续声明 (资源声明在挂载收尾才冻结)
    applyManifestResourcesNow(*inst);

    if (!inst->mountFn) {
        // 无挂载入口 = 本插件无 agent 侧能力; 清单资源仍然按挂载应用
        entry->pluginMounted   = true;
        entry->resourcesFrozen = true;
        co_return true;
    }

    const std::string workDir   = ctx->getSessionBaseWorkDir(std::string{});
    const std::string language  = ctx->agentConfig ? ctx->agentConfig->language : std::string{};
    const std::string modelJson = host->getModelConfigJson();

    AgentxxPluginAgentMountSpec spec{};
    spec.struct_size = sizeof(AgentxxPluginAgentMountSpec);
    spec.flags       = 0;
    spec.agent_id    = PluginStringView::from(agentId_);
    spec.agent_name  = PluginStringView::from(agentName_);
    spec.work_dir    = PluginStringView::from(workDir);
    spec.language    = PluginStringView::from(language);
    spec.model_json  = PluginStringView::from(modelJson);
    spec.config_json = PluginStringView::from(entry->configJson);
    spec.mount_seq   = mountSeq_;

    std::string mountError;
    auto        instPtr     = inst;
    void*       mountResult = nullptr;
    const bool  ok          = co_await pluginxx::awaitPluginLifecycle(
        host->runtime(),
        instPtr,
        instPtr->pluginCtx,
        [instPtr, &spec, &mountResult](
            void* /*ctx*/,
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
        if (auto* failed = findEntry(inst.get()); failed != nullptr) {
            revokeEntry(*failed);
            failed->mountUserData   = nullptr;
            failed->pluginMounted   = false;
            failed->resourcesFrozen = false; // 允许重新挂载时再次声明
        }
        std::erase_if(entries_, [&inst](const Entry& item) {
            return item.inst.get() == inst.get();
        });
        // 能力声明是进程级的 (不按挂载分开记): 首次挂载就失败时整体撤销。
        // 实例还挂在别的 agent 上时保留 —— 那份挂载仍在用这个能力。
        if (!mountedElsewhere(inst.get())) {
            if (auto host = host_.lock(); host != nullptr) {
                const auto caps = inst->capabilityRegistrations; // 拷贝: 撤销会改表
                for (const auto& cap : caps) {
                    host->unregisterCapability(inst.get(), cap.name);
                }
            }
        }
        co_return false;
    }
    entry->mountUserData   = mountResult;
    entry->pluginMounted   = true;
    entry->resourcesFrozen = true; // 挂载收尾: 资源声明固定 (运行时注册会被拒)
    // 挂载期间实例被请求关闭: 立刻拆掉, 否则注册会在关闭流程之后残留
    if (inst->lifetime && inst->lifetime->closeRequested()) {
        co_await unmountOneAsync(inst);
        co_return false;
    }
    co_return true;
}

/// 该实例是否还挂在别的挂载上 (不含本挂载)
asio::awaitable<void> PluginMount::unmountOneAsync(std::shared_ptr<PluginInstance> inst) {
    if (!inst) {
        co_return;
    }
    auto* entry = findEntry(inst.get());
    // 禁用时已同步拆掉记录 (pluginMounted=false), 但插件 unmount 回调还欠着
    // (它需要 await, 只能在 IO 线程补): 两种状态都要走进来。
    if (entry == nullptr || (!entry->pluginMounted && !entry->unmountPending)) {
        co_return;
    }
    auto host = host_.lock();
    if (!host) {
        co_return;
    }
    // 拆挂载后该 agent 上的资源声明也一并撤销; 重新挂载时插件会再次声明
    const bool callUnmount = inst->unmountFn != nullptr;
    entry->resourcesFrozen = false;
    entry->unmountPending  = false;
    entry->pluginMounted   = false;
    if (callUnmount) {
        std::string unmountError;
        auto        instPtr = inst;
        void*       mountUd = entry->mountUserData;
        const bool  ok      = co_await pluginxx::awaitPluginLifecycle(
            host->runtime(),
            instPtr,
            instPtr->pluginCtx,
            [instPtr, mountUd](
                void* /*ctx*/,
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
    if (auto* live = findEntry(inst.get()); live != nullptr) {
        revokeEntry(*live);
    }
    std::erase_if(entries_, [&inst](const Entry& item) {
        return item.inst.get() == inst.get();
    });
    std::erase_if(inst->mounts, [this](const std::weak_ptr<PluginMount>& item) {
        return item.lock().get() == this;
    });
    co_return;
}

asio::awaitable<void> PluginMount::unmountAllAsync() {
    if (entries_.empty()) {
        co_return;
    }
    // 逆序卸载 (后挂的先拆)
    auto names = mountedNames();
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        auto inst = it->inst;
        if (inst) {
            co_await unmountOneAsync(inst);
        }
    }
    XX_LOGI("agent '{}' 已拆全部插件挂载", agentId_);
    co_return;
}

// =====================================================================
// 撤销
// =====================================================================

void PluginMount::revokeEntry(Entry& entry) {
    auto inst = entry.inst;
    if (!inst) {
        return;
    }
    auto ctx = agent_.lock();

    // ① 事件订阅: 只撤销订阅在本挂载总线上的那些 (空 agentId = 主程序总线, 保留)
    std::vector<PluginxxSubscription*> mine;
    for (const auto& handle : inst->subscriptions) {
        if (handle && handle->agentId == agentId_) {
            mine.push_back(handle.get());
        }
    }
    for (auto* sub : mine) {
        pluginxx::unsubscribePluginSubscription(sub);
    }

    // ② 声明式资源 (MCP / memory / skill): 按所有权整体摘除 (MCP 连接随挂载断开)
    if (ctx && ctx->resourceApplier) {
        ctx->resourceApplier->removeAllOwned(inst->name);
    }

    // ③ 执行图定义: 恢复占用前的定义 ("回到内置")
    if (entry.ownsGraphDefinition) {
        releaseGraphDefinitionSlot(inst.get());
        entry.ownsGraphDefinition = false;
    }

    // ④ 图节点类型: 槽位失活 (类型名保留在注册表里, 不会再创建实例)
    for (const auto& graph : entry.graphNodes) {
        if (graph.slot) {
            graph.slot->invalidate(inst.get());
        }
    }
    entry.graphNodes.clear();

    // ⑤ 提示词: 删除本实例的贡献并重新合成 (不写回旧值, 不顶掉别的实例)
    removePromptContributions(inst->name);
    entry.promptKeys.clear();

    // ⑥ 功能点: 先撤实现, 再撤自己声明的点
    if (ctx && ctx->features) {
        const std::string owner = fmt::format("plugin:{}", inst->name);
        for (const auto& pointId : entry.featureImpls) {
            ctx->features->removeImpl(pointId, owner);
        }
        for (const auto& pointId : entry.featurePoints) {
            ctx->features->undefinePluginPoint(pointId);
        }
    }
    entry.featureImpls.clear();
    entry.featurePoints.clear();

    // ⑦ 钩子处理器: 从本挂载的注册表摘除 (记录保留, 句柄置 0)
    detachHookHandlers(inst.get());

    // ⑧ 工具权限声明: 工具不再可调用, 声明无需保留
    if (auto* permission = permissionMiddleware(); permission != nullptr) {
        for (const auto& name : entry.permissionTools) {
            permission->unregisterToolPermission(name);
        }
    }
    entry.permissionTools.clear();

    // ⑨ 工具: 从注册表摘除; 工具对象移入回收站, 保活仍在执行中的调用
    for (const auto& name : entry.tools) {
        toolRegistry()->unregisterTool(name);
    }
    entry.tools.clear();
    for (auto& tool : entry.toolObjects) {
        retiredTools_.push_back(std::move(tool));
    }
    entry.toolObjects.clear();
}

void PluginMount::detachInstance(PluginInstance* inst) {
    if (inst == nullptr) {
        return;
    }
    auto* entry = findEntry(inst);
    if (entry == nullptr) {
        return;
    }
    revokeEntry(*entry);
    // 禁用 = 拆挂载 (设计: disable → 全部挂载 unmount; enable → 重新 mount):
    // 注册同步摘干净, 插件 unmount 回调记为欠着, 由 [unmountOneAsync] 在 IO 线程补。
    // entry 本身保留 —— 回调还要用它上面的 `mountUserData`。
    entry->pluginMounted   = false;
    entry->unmountPending  = inst->unmountFn != nullptr;
    entry->resourcesFrozen = false;
}

void PluginMount::clearInstanceRecords(PluginInstance* inst) {
    if (inst == nullptr) {
        return;
    }
    if (auto* entry = findEntry(inst); entry != nullptr) {
        entry->tools.clear();
        entry->toolObjects.clear();
        entry->permissionTools.clear();
        entry->hookHandles.clear();
        entry->featurePoints.clear();
        entry->featureImpls.clear();
        entry->graphNodes.clear();
        entry->promptKeys.clear();
    }
    // 记录清空后, 本挂载上的钩子属于"已撤登记但实例仍挂着"的中间态; 句柄置 0
    detachHookHandlers(inst);
}

void PluginMount::releaseInstanceInstances(PluginInstance& inst) {
    if (auto* entry = findEntry(&inst); entry != nullptr) {
        for (auto& tool : entry->toolObjects) {
            retiredTools_.push_back(std::move(tool));
        }
        entry->toolObjects.clear();
        entry->tools.clear();
    }
}

// =====================================================================
// 清单资源
// =====================================================================

void PluginMount::applyManifestResourcesNow(PluginInstance& inst) {
    const auto& resources = inst.manifestResources;
    if (resources.skillDirs.empty() && resources.memoryFiles.empty()
        && resources.mcpServers.empty()) {
        return;
    }
    auto ctx = agent_.lock();
    if (!ctx || !ctx->resourceApplier) {
        XX_LOGW("Plugin `{}` declared resources skipped (no resource applier)", inst.name);
        return;
    }
    agentxx::agent::PluginResourceDecls decls;
    decls.skillDirs   = resources.skillDirs;
    decls.memoryFiles = resources.memoryFiles;
    for (const auto& [ns, d] : resources.mcpServers) {
        agentxx::agent::McpServerConfig cfg;
        cfg.url              = d.url;
        cfg.toolTimeout      = std::chrono::milliseconds{d.timeoutMs};
        decls.mcpServers[ns] = cfg;
    }
    ctx->resourceApplier->applyDecls(inst.name, decls);
}

// =====================================================================
// 轮次状态与中间件收尾
// =====================================================================

void PluginMount::flushPendingCleanup() {
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

} // namespace plugin
} // namespace agentxx
