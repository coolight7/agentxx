#pragma once

#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/plugin_framework.h"
#include "agentxx/plugin/plugin_interfaces.h"
#include "agentxx/plugin/tool_registry.h"
#include "agentxx/tools/tool.h"
#include "asio/awaitable.hpp"
#include "asio/steady_timer.hpp"
#include "pluginxx/host/domain_hooks.h"
#include "pluginxx/host/host_core.h"
#include "pluginxx/host/manifest.h"
#include "utilxx_base/json.h"
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace agentxx {
namespace agent {
class AgentContext;
class AgentResourceApplier;
} // namespace agent

namespace feature {
class Registry;
}

namespace events {
class EventBus;
}

namespace middleware {
class PermissionMiddlewareHandle;
}

namespace plugin {

class PluginManager;
// CapabilityRegistry 由 cxx_pluginxx 提供 (见 agentxx/plugin/plugin_framework.h 的 using 引入)
class PluginMiddlewareHandle;
class PluginTool;
class PluginInstance;
struct GraphTypeSlot;

} // namespace plugin
} // namespace agentxx

// 事件订阅句柄的实现体 (C ABI 不透明句柄 `PluginxxSubscription*`) 由
// cxx_pluginxx 提供 (事件表是通用表): 见 `pluginxx/host/event_bus.h`。
// 句柄字段为内核类型 (EventSource / 插件实例基类弱引用), 因此事件表的订阅与撤销
// 实现整体位于内核; agentxx 只提供事件后端适配 (AgentEventBusSource, 见
// plugin_manager_adapters.cpp) 与主题命名规则。

namespace agentxx {
namespace plugin {

/* ==================== 内置插件清单 (跨 TU 导出声明, 保持 C 链接) ==================== */
/// - 由 agent/plugins/builtin_plugins.cpp.in 编译时收集内置插件列表，并添加实现函数
const PluginxxBuiltinInfo*     get_builtin_plugins(uint64_t* count);
const PluginxxBuiltinManifest* get_builtin_manifests(uint64_t* count);

class PluginInstance : public PluginInstanceBase {
public:

    /// 继承 PluginInstanceBase 的公共字段 (name/version/path/configPath/args/depends/
    /// dlHandle/pluginCtx/enabled/inflight 等), 见
    /// [pluginxx/runtime/instance_base.h](/agent/third_party/cxx_pluginxx/include/pluginxx/runtime/instance_base.h)
    /// 接口声明 (plugin.yaml `interfaces`; 加载时随 manifest 解析传入,
    /// 直连库路径为空) —— 经 list() 暴露供展示/排查
    PluginManifestInterfaces interfaces;
    /// 资源冻结标志: 插件初始化阶段 (create 内) 允许注册 skill/memory/mcp,
    /// 初始化完成后冻结，后续固定不可变以防上下文变化 (仅 yaml 声明与初始化追加生效)
    bool resourcesFrozen = false;

    struct HookRegistration {
        int32_t point;
        void*(PLUGINXX_CALL*
                  start)(void*, int32_t, const PluginxxStringView*, const PluginxxOperatorNotify*, PluginxxString*);
        void(PLUGINXX_CALL* cancel)(void*, void*);
        void* ud;
    };

    /// 已声明能力记录 (名称 / 启动回调 / 取消回调 / 上下文)
    /// - 类型本体在 cxx_pluginxx (能力表是通用表), 登记与撤销由宿主核心统一维护
    ///   (见 [pluginxx::PluginHostCore])
    using CapabilityRegistration = pluginxx::PluginCapabilityRegistration;

    struct GraphNodeTypeRegistration {
        std::string                       type;
        AgentxxPluginGraphNodeRunStartFn  run_start  = nullptr;
        AgentxxPluginGraphNodeRunCancelFn run_cancel = nullptr;
        void*                             user_data  = nullptr;
        std::string                       config_schema_json;
        std::shared_ptr<GraphTypeSlot>    slot;
    };

    struct PromptBackup {
        std::optional<std::string>                                     systemPrompt;
        std::optional<std::string>                                     appliedSystemPrompt;
        std::map<std::string, std::optional<std::string>, std::less<>> appendSystemPrompts;
        std::map<std::string, std::optional<std::string>, std::less<>> appliedAppendSystemPrompts;
        std::map<std::string, std::optional<agentxx::agent::ToolPrompt>, std::less<>> toolPrompt;
        std::map<std::string, std::optional<std::string>, std::less<>> appliedToolPromptJson;
        std::vector<std::string>                                       backedUpTools;
        bool                                                           backedUpSystem = false;
    };

    std::vector<std::string> toolNames;
    /// 已声明权限限制的工具名 (随工具注销/实例禁用卸载一并撤销)
    std::vector<std::string>               permissionToolNames;
    std::vector<HookRegistration>          hookRegistrations;
    std::vector<GraphNodeTypeRegistration> graphNodeTypes;
    /// 本实例声明的功能点 id (`plugin.<自己>.*`; 禁用摘生效, 卸载全摘)
    std::vector<std::string> featurePoints;
    /// 本实例登记的功能点实现所在点 id (同一实例同一个点只保留一个实现)
    std::vector<std::string> featurePointImpls;
    PromptBackup             promptBackup;

    /// 装载总耗时 (毫秒; 由 PluginManager 装载入口记录, 见 PLG-10)
    uint64_t loadMs = 0;

    /// 通用表相关登记 (事件订阅 / 睡眠句柄 / 能力声明) 由基类持有, 见
    /// [pluginxx::PluginInstanceBase]: 通用表实现只依赖基类, 新增宿主无需重复实现。

    std::shared_ptr<PluginMiddlewareHandle>  middleware = nullptr;
    std::vector<std::shared_ptr<PluginTool>> tools;

    std::weak_ptr<PluginInstance> self{};
    std::weak_ptr<PluginManager>  manager{};

    explicit PluginInstance(std::string in_name) :
        PluginInstanceBase(std::move(in_name)) {}

    ~PluginInstance();

    /// 本端 destroy 入口符号名 (agent 侧)
    const char* pluginDestroySymbol() const noexcept override {
        return AGENTXX_PLUGIN_AGENT_SYMBOL_DESTROY;
    }
};

class PluginTool : public agentxx::tools::XXToolBase {
public:

    PluginTool(
        std::weak_ptr<agentxx::agent::AgentContext> agentContext,
        std::shared_ptr<PluginInstance>             instance,
        AgentxxPluginToolSpec                       spec
    );

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const utilxx_base::Json& arguments) override;

    std::shared_ptr<PluginInstance> instance() const {
        return instance_.lock();
    }

    const AgentxxPluginToolSpec& spec() const {
        return spec_;
    }

private:

    std::string                   name_;
    std::string                   description_;
    std::string                   parametersJson_;
    AgentxxPluginToolSpec         spec_;
    utilxx_base::Json             parameters_;
    std::weak_ptr<PluginInstance> instance_;
};

class PluginMiddlewareHandle
    : public agentxx::middleware::BaseMiddlewareHandle<agentxx::middleware::BaseMiddlewareState> {
public:

    PluginMiddlewareHandle(
        std::string_view                            name,
        std::weak_ptr<agentxx::agent::AgentContext> agentContext,
        std::shared_ptr<PluginInstance>             instance
    );

    void setHook(const AgentxxPluginHookSpec& spec);
    void clearHook(AgentxxPluginHookPoint point);

    asio::awaitable<void> onAgentcallStartFunc(neograph::graph::NodeInput& in) override;
    asio::awaitable<void> onAgentcallEndFunc(
        const neograph::graph::NodeInput& in,
        neograph::graph::NodeOutput&      result
    ) override;
    asio::awaitable<void> onModelcallStartFunc(neograph::graph::NodeInput& in) override;
    asio::awaitable<void> onModelcallRunFunc(neograph::graph::NodeInput& in) override;
    asio::awaitable<void> onModelcallEndFunc(
        const neograph::graph::NodeInput& in,
        neograph::graph::NodeOutput&      result
    ) override;
    asio::awaitable<void> onToolcallStartFunc(neograph::graph::NodeInput& in) override;
    asio::awaitable<void> onToolcallEndFunc(
        const neograph::graph::NodeInput& in,
        neograph::graph::NodeOutput&      result
    ) override;

private:

    struct HookEntry {
        void*(PLUGINXX_CALL*
                  start)(void*, int32_t, const PluginxxStringView*, const PluginxxOperatorNotify*, PluginxxString*)
            = nullptr;
        void(PLUGINXX_CALL* cancel)(void*, void*) = nullptr;
        void* ud                                  = nullptr;
        bool  set                                 = false;
    };

    asio::awaitable<void>
        dispatch(AgentxxPluginHookPoint point, const neograph::graph::NodeInput& in);

    std::weak_ptr<PluginInstance>                    instance_;
    std::array<HookEntry, AGENTXX_PLUGIN_HOOK_COUNT> hooks_{};
};

/// 插件管理器 (agent 侧宿主)
///
/// - 通用部分 (log/json/config/plugins/events/scheduler/coroutine_runtime/tasks/
///   cancel/capabilities 十张通用表的状态与实现) 继承自
///   [pluginxx::PluginHostCore], 领域数据经 [pluginxx::DomainHooks] 提供;
/// - 装载/启停/禁用启用/卸载/级联依赖骨架继承自
///   [pluginxx::PluginHostLifecycle] (与宿主领域无关, 见 pluginxx/host/lifecycle.h);
/// - 本类只保留领域部分: 工具/权限/钩子/会话/模型/提示词/资源/图 的注册与实现,
///   以及上面两层需要宿主数据的接缝 (见下方 protected 段的接缝覆写)。
///
/// 因此 `unloadAsync` / `shutdownAsync` / `shutdownAll` / `disable` / `enable` /
/// `detachAll` / `detachInstanceRegistrations` / `clearPluginOwnedRegistrations` /
/// `stopForDisable` / `startForEnable` / `hasPendingClose` 都是继承来的, 调用点不变。
class PluginManager : public pluginxx::PluginHostLifecycle<PluginInstance>,
                      public std::enable_shared_from_this<PluginManager>,
                      public pluginxx::DomainHooks {
public:

    /// 本宿主向插件提供的接口表数量 (计划 PLG-8: 文档里的数字有常量可校验)
    /// - 10 张通用表 (`pluginxx.*`, 由插件框架内核实现: log/json/config/plugins/events/
    ///   scheduler/coroutine_runtime/tasks/cancel/capabilities)
    /// - 10 张 agent 领域表 (`agentxx.agent.*`: tools/permission/hooks/session/context/
    ///   model/prompt/resources/graph/feature)
    /// - 增删接口表时同时更新本常量与 `docs/zh-cn/design/plugins.md` §8 与根 `AGENTS.md`
    ///   (测试模块 `boundaries` 会校验三者一致)
    inline static constexpr size_t kInterfaceTableCount = 10 + 10;

    struct PluginListView {
        std::string              name;
        std::string              version;
        std::string              description;
        std::string              path;
        std::string              configPath;
        bool                     enabled  = true;
        size_t                   inflight = 0;
        std::vector<std::string> tools;
        std::vector<std::string> capabilities;
        std::vector<std::string> depends;
        std::vector<std::string> optionalDepends;
        std::vector<std::string> requiredInterfaces;
        std::vector<std::string> optionalInterfaces;

        // ---- 诊断字段 (计划 PLG-10 / ARC-6 装配快照) ----
        /// 是否被用户显式禁用 (区别于依赖级联禁用)
        bool userDisabled = false;
        /// 是否因必选依赖不可用被级联禁用
        bool blockedByDependencies = false;
        /// 装载总耗时 (dlopen + create + start + 注册收尾), 毫秒; 0 表示未记录
        uint64_t loadMs = 0;
        /// 钩子登记数 (中间件钩子点)
        size_t hookCount = 0;
        /// 自定义图节点类型登记数
        size_t graphNodeCount = 0;
        /// 事件订阅数
        size_t eventSubCount = 0;
        /// 已声明权限限制的工具数
        size_t permissionToolCount = 0;
        /// 本实例声明的功能点数 / 登记的实现数
        size_t featurePointCount = 0;
        size_t featureImplCount  = 0;
        /// 统一注册清单 (计划 PLG-1): 各项的合计 (0 = 已回到基线)
        size_t registrationTotal = 0;
        /// 其中的提示词键占用数
        size_t promptKeyCount = 0;
        /// 其中的 skill 目录 / memory 文件 / MCP 命名空间数 (资源应用器)
        size_t skillDirCount     = 0;
        size_t memoryFileCount   = 0;
        size_t mcpNamespaceCount = 0;
        /// 是否占用执行图定义 (独占 slot; 计划 PLG-4)
        bool ownsGraphDefinition = false;
    };

    explicit PluginManager(std::weak_ptr<agentxx::agent::AgentContext> agentContext);
    ~PluginManager();

    PluginManager(const PluginManager&)            = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    /// 统一注册清单 (计划 PLG-1)
    ///
    /// 一个插件实例能向宿主贡献的东西分散在多处 (工具注册表、权限中间件、
    /// 中间件链、图注册表、事件总线、能力表、提示词、资源应用器)。禁用/卸载后
    /// "到底清干净了没有"需要一份可比较的基准, 因此这里把**宿主可撤销**的注册
    /// 计数集中成一个结构: 全部为 0 = 已回到基线。
    ///
    /// - 只统计本实例名下的注册 (不统计内置工具/主配置资源);
    /// - `graphTypeNameResidual`: `GraphRegistry` 没有删除类型名的 API, 注销后
    ///   类型名仍留在注册表里 (工厂持弱引用, 实例释放后创建即失败)。这项恒为
    ///   已注册的图节点类型数, 供诊断说明"残留的只是名字, 不会再创建实例"。
    struct RegistrationInventory {
        size_t tools               = 0; ///< 工具注册表内的工具
        size_t permissionTools     = 0; ///< 已声明的工具权限限制
        size_t hooks               = 0; ///< 钩子点登记
        size_t graphNodeTypes      = 0; ///< 自定义图节点类型 (激活中)
        size_t eventSubscriptions  = 0; ///< 事件订阅
        size_t capabilities        = 0; ///< 能力声明
        size_t promptKeys          = 0; ///< 占用中的提示词键 (prompt 贡献)
        size_t skillDirs           = 0; ///< skill 扫描目录 (资源应用器)
        size_t memoryFiles         = 0; ///< memory 上下文文件 (资源应用器)
        size_t mcpNamespaces       = 0; ///< MCP 命名空间 (资源应用器)
        size_t featurePoints       = 0; ///< 本实例声明的功能点 (生效中)
        size_t featureImpls        = 0; ///< 本实例登记的功能点实现 (生效中)
        bool   middlewareAttached  = false; ///< 钩子中间件是否挂在中间件链上
        bool   ownsGraphDefinition = false; ///< 是否占用执行图定义 (独占 slot)

        /// 宿主可撤销注册的总数 (不含 [graphNodeTypes] 的注册表残留)
        size_t total() const {
            return tools + permissionTools + hooks + graphNodeTypes + eventSubscriptions
                   + capabilities + promptKeys + skillDirs + memoryFiles + mcpNamespaces
                   + featurePoints + featureImpls + (middlewareAttached ? 1 : 0)
                   + (ownsGraphDefinition ? 1 : 0);
        }
    };

    /// 统计实例当前"宿主侧生效"的注册 (禁用/卸载后的基线断言与诊断)
    RegistrationInventory registrationInventory(const PluginInstance& inst) const;

    /// 执行图定义的占用者 (独占 slot; 空 = 无插件占用, 使用内置/宿主定义)
    /// - 见 [setGraphJson]: 同一时刻至多一个插件实例占用, 占用者卸载/禁用后
    ///   自动恢复占用前的定义 ("回到内置")
    const std::string& graphDefinitionOwner() const {
        return graphDefinitionOwner_;
    }

    // =====================================================================
    // 装载 (旧签名; 内部转成内核的 PluginLoadOptions 后交给宿主生命周期骨架)
    // =====================================================================
    //
    // 注意: 这里声明的是"宿主配置类型 → 内核装载参数"的适配层, 因此与骨架里
    // 同名但形参类型不同的装载入口是重载关系 (未加 using 引入, 骨架版本只在
    // 本类内部经限定名调用)。

    asio::awaitable<std::shared_ptr<PluginInstance>> loadNativeAsync(
        std::string                             path,
        const agentxx::agent::PluginConfig*     cfg                 = nullptr,
        bool                                    allowClientOnlySkip = false,
        const plugin::PluginManifestResources&  resources           = {},
        const plugin::PluginManifestInterfaces& interfaces          = {}
    );

    asio::awaitable<std::shared_ptr<PluginInstance>> loadBuiltinAsync(
        std::string                             name,
        std::string                             path,
        std::vector<std::string>                depends,
        std::vector<std::string>                optionalDepends,
        const agentxx::agent::PluginConfig*     cfg        = nullptr,
        const plugin::PluginManifestResources&  resources  = {},
        const plugin::PluginManifestInterfaces& interfaces = {}
    );

    asio::awaitable<std::shared_ptr<PluginInstance>> loadPluginAsync(
        std::string                         path,
        const agentxx::agent::PluginConfig* cfg                 = nullptr,
        bool                                allowClientOnlySkip = false
    );

    asio::awaitable<void>
        loadConfiguredPlugins(const std::vector<agentxx::agent::PluginConfig>& plugins);

    void flushPendingCleanup();

    std::vector<PluginListView> list() const;

    bool hasRunningTurn() const {
        return runningTurns_ > 0;
    }

    void onTurnBegin() {
        ++runningTurns_;
    }

    void onTurnEnd() {
        if (runningTurns_ > 0) {
            --runningTurns_;
        }
    }

    int registerTool(PluginInstance* inst, const AgentxxPluginToolSpec* spec);
    int unregisterTool(PluginInstance* inst, PluginxxStringView name);

    int unregisterTool(PluginInstance* inst, std::string_view name) {
        return unregisterTool(inst, strToSv(name));
    }

    /// 声明工具权限限制 (插件在注册工具后调用)
    /// - 声明内容: 权限作用域 (读/写)、目标参数名、目标类型 (路径/文本/无)
    /// - 声明最终交给 agent 装配的权限中间件 ([PermissionMiddlewareHandle]):
    ///   工具调用时的判定 (白/黑名单、permission.mode 默认、记住的选择、
    ///   工作区隔离、完全授权) 全部由该中间件执行, 插件只声明"哪些参数受约束"
    /// - 权限声明属于附加能力: 宿主未装配权限中间件时返回非 0, 插件可忽略
    /// `return`: 0 成功, 非 0 失败 (工具非本实例所有 / 中间件不可用 / 声明非法)
    int registerToolPermission(PluginInstance* inst, const AgentxxPluginToolPermissionSpec* spec);

    /// 撤销工具权限声明 (按工具名; 工具注销、插件禁用/卸载时由宿主自动撤销)
    /// `return`: 0 成功, 非 0 不存在
    int unregisterToolPermission(PluginInstance* inst, PluginxxStringView toolName);

    int unregisterToolPermission(PluginInstance* inst, std::string_view toolName) {
        return unregisterToolPermission(inst, strToSv(toolName));
    }

    /// 批量查询路径权限判定 (只读; 不发起询问/不产生中断)
    /// - 判定规则与工具调用权限检查一致, 仅把"应询问"以 ASK 返回 (见 agentxx.agent.permission)
    /// - 供支持模式/前缀参数的插件工具在枚举出实际路径后逐项过滤 (glob/grep 等)
    ///
    /// - `args`:
    ///     - [inst]  调用方插件实例 (仅用于日志/归属)
    ///     - [scope] 权限作用域 (AGENTXX_PLUGIN_PERMISSION_SCOPE_*)
    ///     - [sessionId] 会话 (取会话工作目录/隔离边界; 可为空)
    ///     - [paths] 待判定路径 (绝对路径优先; 相对路径按会话工作目录解析)
    ///     - [outDecisions] 等长出参 (AGENTXX_PLUGIN_PERMISSION_DECISION_*)
    ///
    /// `return`: 0 成功; 非 0 不支持或失败 (权限中间件未装配时调用方应跳过过滤)
    int checkPermissionPaths(
        PluginInstance*                 inst,
        int32_t                         scope,
        std::string_view                sessionId,
        const std::vector<std::string>& paths,
        std::vector<int32_t>&           outDecisions
    );

    int registerSkillDir(PluginInstance* inst, PluginxxStringView path);

    int registerSkillDir(PluginInstance* inst, std::string_view path) {
        return registerSkillDir(inst, strToSv(path));
    }

    int unregisterSkillDir(PluginInstance* inst, PluginxxStringView path);

    int unregisterSkillDir(PluginInstance* inst, std::string_view path) {
        return unregisterSkillDir(inst, strToSv(path));
    }

    int registerMemoryFile(PluginInstance* inst, PluginxxStringView path);

    int registerMemoryFile(PluginInstance* inst, std::string_view path) {
        return registerMemoryFile(inst, strToSv(path));
    }

    int unregisterMemoryFile(PluginInstance* inst, PluginxxStringView path);

    int unregisterMemoryFile(PluginInstance* inst, std::string_view path) {
        return unregisterMemoryFile(inst, strToSv(path));
    }

    int registerMcpServer(PluginInstance* inst, PluginxxStringView specJson);

    int registerMcpServer(PluginInstance* inst, std::string_view specJson) {
        return registerMcpServer(inst, strToSv(specJson));
    }

    int unregisterMcpServer(PluginInstance* inst, PluginxxStringView nameSpace);

    int unregisterMcpServer(PluginInstance* inst, std::string_view nameSpace) {
        return unregisterMcpServer(inst, strToSv(nameSpace));
    }

    std::string ownResourcesJson(const PluginInstance* inst);

    int registerHook(PluginInstance* inst, const AgentxxPluginHookSpec* spec);
    int unregisterHook(PluginInstance* inst, AgentxxPluginHookPoint point);

    // ==================== 功能点 (agentxx.agent.feature) ====================

    /// 为某个点登记实现 (插件的功能点表入口)
    /// - 点必须已声明 (宿主点在装配期声明; 插件点先经 [defineFeaturePoint])
    /// - 同一 `(点, 实例)` 重复登记 = 覆盖; 数量不设上限
    /// - `priority` 越界裁剪到上下限并记一条警告 (不拒绝登记)
    /// - 实现体经操作协议驱动 (与工具同一套), 完成回调在宿主 IO 线程发布
    ///
    /// `return`: 0 成功; 非 0 失败 (点未声明 / 回调为空 / 实例不可用)
    int registerFeatureImpl(PluginInstance* inst, const AgentxxPluginFeatureImplSpec* spec);

    /// 撤销本实例在某点上的实现
    ///
    /// `return`: 0 成功; 非 0 不存在
    int unregisterFeatureImpl(PluginInstance* inst, PluginxxStringView pointId);

    int unregisterFeatureImpl(PluginInstance* inst, std::string_view pointId) {
        return unregisterFeatureImpl(inst, strToSv(pointId));
    }

    /// 声明本实例自己的功能点 (id 必须落在 `plugin.<本实例插件名>.*`)
    /// - 本轮只允许 `provide` 类型; 重复声明 = 覆盖 (记一条日志)
    /// - 声明期间恒可被调用; 值缓存固定不缓存 (调用方按结果里的 identity 自理)
    ///
    /// `return`: 0 成功; 非 0 失败 (id 非法 / 类型不支持 / 与宿主点冲突)
    int defineFeaturePoint(PluginInstance* inst, const AgentxxPluginFeaturePointSpec* spec);

    /// 撤销本实例声明的点 (连带撤掉这些点上的全部实现)
    ///
    /// `return`: 0 成功; 非 0 失败 (点不存在 / 不是本实例声明的点)
    int undefineFeaturePoint(PluginInstance* inst, PluginxxStringView pointId);

    int undefineFeaturePoint(PluginInstance* inst, std::string_view pointId) {
        return undefineFeaturePoint(inst, strToSv(pointId));
    }

    /// 功能点清单 JSON (插件 `list_points`、装配快照与诊断共用同一份实现)
    std::string listFeaturePoints();

    /// 调用一个功能点 (异步; 完成回调经 notify 在宿主 IO 线程发布)
    /// - 只拿数据: 只读值缓存、不记置空、不写调用方会话 / 不发提示 / 不落盘
    /// - 受理失败返回 NULL 并写 error_out; 调用本身的失败 (not_callable / bad_args /
    ///   no_impl / disabled / busy / failed) 经回调的结果 JSON 回
    PluginxxOperatorHandle* callFeatureAsync(
        PluginInstance*          caller,
        PluginxxStringView       pointId,
        PluginxxStringView       argsJson,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    );

    /// 注册插件节点类型到 per-agent GraphRegistry (插件 graph 接口表)
    int registerGraphNodeType(PluginInstance* inst, const AgentxxPluginGraphNodeTypeSpec* spec);
    /// 注销插件节点类型 (按类型名; 卸载时宿主自动清理)
    int unregisterGraphNodeType(PluginInstance* inst, PluginxxStringView type);

    int unregisterGraphNodeType(PluginInstance* inst, std::string_view type) {
        return unregisterGraphNodeType(inst, strToSv(type));
    }

    /// 装载失败时的"怎么改"提示 (计划 PLG-7)
    ///
    /// 内核只报"哪一步失败了", 不解释"你该怎么改"。本函数按路径形态巡检常见的
    /// 装载失败原因, 返回一句可直接照做的建议 (没发现问题返回空串):
    /// - `path` 为空;
    /// - `builtin://<name>` 的内置清单不存在;
    /// - 路径不存在 (相对路径按进程当前工作目录解析);
    /// - 目录下没有 `plugin.yaml` 清单 / 清单 YAML 语法错误 / 清单 `entry`
    ///   指向的库文件不存在;
    /// - 库文件缺少宿主入口符号 (agent 侧 `agentxx_plugin_agent_{create,start,stop}`;
    ///   `destroy` 由实例析构入口单独查找)。
    ///
    /// - 只读检查 (不改变任何状态); 需要读导出符号时会短暂打开库文件,
    ///   失败路径上由装载入口调用, 正常装载不调用;
    /// - 装载入口在失败后把这句建议记 WARN 日志, 便于插件作者定位。
    std::string diagnosePluginPath(std::string_view path) const;

    /// 获取当前执行图 JSON 定义 (host->alloc 语义由 vtable 层处理)
    std::string getGraphJson();
    /// 设置执行图 JSON 定义 (独占 slot; 见 [graphDefinitionOwner])
    ///
    /// - 无占用者时: 记录当前定义为"基础定义"并成为占用者;
    /// - 本实例已是占用者: 允许继续修改 (插件热更新自己的图);
    /// - 其他实例占用中: 拒绝 (返回非 0 并给出占用者), 避免两个插件交替覆盖
    ///   彼此的图定义 —— 卸载其中一方时无法判断该恢复成哪一份;
    /// - 占用者禁用/卸载时恢复基础定义 (内置或宿主自定义的图)。
    int setGraphJson(PluginInstance* inst, PluginxxStringView graph_json);

    int setGraphJson(PluginInstance* inst, std::string_view graph_json) {
        return setGraphJson(inst, strToSv(graph_json));
    }

    PluginxxString getShareStore(PluginInstance* inst, PluginxxStringView session_id, int64_t id);

    PluginxxString getShareStore(PluginInstance* inst, std::string_view session_id, int64_t id) {
        return getShareStore(inst, strToSv(session_id), id);
    }

    int64_t addShareStore(
        PluginInstance*    inst,
        PluginxxStringView session_id,
        PluginxxStringView content
    );

    int64_t
        addShareStore(PluginInstance* inst, std::string_view session_id, std::string_view content) {
        return addShareStore(inst, strToSv(session_id), strToSv(content));
    }

    void emitMessageTip(
        PluginInstance*    inst,
        PluginxxStringView session_id,
        PluginxxStringView text,
        int32_t            level
    );

    /// 会话 LLM 上下文 (Json 数组文本; 宿主 alloc, 失败返回空串)
    /// - 上下文由会话持有 (图状态不含 messages 通道), 插件经 agentxx.agent.context 表查询
    /// - 仅 io 线程调用 (vtable 经 ioCallSync 投递)
    PluginxxString getSessionMessages(PluginxxStringView session_id);

    /// 会话 LLM 上下文条数 (会话不存在返回 -1)
    int64_t sessionMessagesCount(PluginxxStringView session_id);

    /// 写会话 LLM 上下文 (插件图节点对 `messages` 通道的写入转发; 仅 io 线程)
    /// - overwrite=false 追加, true 整体替换; 空的 overwrite 视为误写被忽略
    ///   (按旧契约读不到消息的插件可能以为上下文为空, 直接覆盖会清空上下文)
    /// - 返回本次写入的消息条数 (0 表示未写入)
    size_t writeSessionMessages(
        std::string_view         session_id,
        const utilxx_base::Json& messages,
        bool                     overwrite
    );

    void emitMessageTip(
        PluginInstance*  inst,
        std::string_view session_id,
        std::string_view text,
        int32_t          level
    ) {
        emitMessageTip(inst, strToSv(session_id), strToSv(text), level);
    }

    PluginxxOperatorHandle* callToolAsync(
        PluginInstance*          caller,
        PluginxxStringView       name,
        PluginxxStringView       args_json,
        PluginxxStringView       session_id,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    );

    PluginxxOperatorHandle* callToolAsync(
        PluginInstance*          caller,
        std::string_view         name,
        std::string_view         args_json,
        std::string_view         session_id,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    ) {
        return callToolAsync(
            caller,
            strToSv(name),
            strToSv(args_json),
            strToSv(session_id),
            cb,
            ud,
            error_out
        );
    }

    // ==================== 通用表方法 (继承自 pluginxx::PluginHostCore) ====================
    //
    // 以下方法由宿主核心提供, 本类不再重复声明 (调用点无需改动):
    // - capabilities 表: registerCapability / registerCapabilityEx /
    //   unregisterCapability / hasCapability / invokeCapabilityAsync / capabilities();
    // - events 表: subscribe / unsubscribe / publish;
    // - scheduler 表: postCallback / sleep / offload;
    // - tasks 表: registerTask。

    std::shared_ptr<ToolRegistry> registry() const {
        return registry_;
    }

    std::string listPluginsJson();
    std::string getPluginJson(const std::string& name);

    std::string getConfigJson();
    std::string getToolPromptJson(const std::string& toolName);
    std::string getPromptJson();
    int         setPromptJson(PluginInstance* inst, PluginxxStringView prompt_json);

    int setPromptJson(PluginInstance* inst, std::string_view prompt_json) {
        return setPromptJson(inst, strToSv(prompt_json));
    }

    void        restorePromptBackup(PluginInstance* inst);
    std::string getPluginArgsJson(PluginInstance* inst);
    std::string getPluginConfigPath(PluginInstance* inst);
    std::string getLanguage();
    /// 指定语言 (pluginxx::DomainHooks 要求; 同时服务 config 表的 set_language)
    void setLanguage(std::string_view lang) override;

    std::string getSessionWorkDir();
    std::string getSessionWorkDir(std::string_view sessionId);
    std::string getModelConfigJson();
    bool        isSessionCancelled(std::string_view sessionId);

    // ==================== pluginxx::DomainHooks 实现 ====================
    // 通用表 (log/json/config/plugins/events/scheduler/coroutine_runtime/tasks/cancel/
    // capabilities) 的实现位于 cxx_pluginxx, 需要宿主数据的入口经这些方法取数
    // (见 pluginxx/host/domain_hooks.h)。实现体在 plugin_manager_adapters.cpp。

    /// 事件后端 (包装 agentxx::events::EventBus; 无总线时返回 nullptr)
    std::shared_ptr<pluginxx::EventSource> eventSource() override;
    /// 事件主题命名空间补齐 (不以 `plugin.`/`client.` 开头时补 `plugin.`)
    std::string qualifyEventTopic(std::string_view topic) override;
    /// 阻塞工作委托到 AgentContext 的工作线程池; 未装配时返回 false (offload 失败)
    bool postToWorkerThread(std::function<void()> fn) override;

    std::string configJson() override;
    std::string toolPromptJson(std::string_view toolName) override;
    std::string sessionWorkDir(std::string_view sessionId) override;
    std::string language() override;

    std::string pluginsJson() override;
    std::string pluginJson(std::string_view name) override;

    // ==================== prompt 贡献模型 ====================
    //
    // 插件对 prompt 的修改不再用"备份后无条件写回"，而是记录为
    // (owner, key, sequence, value) 贡献：
    //   有效值 = 首次贡献前的基础值 ⊕ 按 sequence 顺序应用的全部存活贡献
    // 卸载/禁用只删除该 owner 的贡献并重新合成，因此不会覆盖其他 owner 的贡献，
    // 也不会把已卸载 owner 的旧值写回；用户或其他宿主代码之后写入的值通过
    // "基础值 rebase"保留。键名：`system` / `append:<key>` / `tool:<toolName>`。
    struct PromptValue {
        bool                                      isTool = false;
        std::string                               text; ///< isTool=false 时的值
        std::optional<agentxx::agent::ToolPrompt> tool; ///< isTool=true 时的值
    };

    struct PromptKeyState {
        /// 首次贡献前的基础值（`nullopt` = 原本不存在）
        std::optional<PromptValue> base;
        /// 宿主上次合成写入的值（用于发现外部修改并 rebase 基础值）
        std::optional<PromptValue> applied;
        /// owner -> (sequence, 贡献值)，按 sequence 升序应用
        std::map<std::string, std::pair<uint64_t, PromptValue>, std::less<>> contributions;
    };

    /// 删除 `owner` 的全部 prompt 贡献并重新合成受影响键（卸载/禁用路径）。
    void removePromptContributions(std::string_view owner);

    /// 重新合成单个 prompt 键的有效值（内部使用；见 PromptKeyState 说明）。
    void recomposePromptKey(const std::string& key);

protected:

    // =====================================================================
    // pluginxx::PluginHostLifecycle 宿主接缝
    // =====================================================================
    //
    // 装载/启停/禁用启用/卸载/级联依赖骨架在 cxx_pluginxx (见
    // pluginxx/host/lifecycle.h); 内核不认识"工具/权限/钩子/图/提示词/资源"，
    // 因此这些领域动作经下列覆写注入。实现体在 plugin_manager_lifecycle.cpp 与
    // plugin_manager_vtable.cpp。

    /// 管理器自引用 (骨架的异步事务与空闲收尾需要在此期间保活管理器)
    std::shared_ptr<pluginxx::PluginHostLifecycle<PluginInstance>> selfRef() override {
        return shared_from_this();
    }

    /// 生成 agent 侧实例对象 (骨架随后补齐元信息/生命周期入口/宿主控制块)
    std::shared_ptr<PluginInstance> createInstance(std::string name) override;

    /// 交给插件的宿主 vtable (进程内稳定静态表; 定义在 plugin_manager_vtable.cpp)
    const PluginxxHostVtable* hostVtable() override;

    /// agent 侧插件入口符号名 (内核不硬编码宿主专名, 见 pluginxx/api/entry.h)
    ///
    /// 与插件侧导出宏 AGENTXX_PLUGIN_AGENT_EXPORT 生成的符号一致; `destroy` 不在此列
    /// (由 PluginInstance::pluginDestroySymbol 给出)。
    pluginxx::PluginEntrySymbols entrySymbols() const override {
        return {
            AGENTXX_PLUGIN_AGENT_SYMBOL_GET_INFO,
            AGENTXX_PLUGIN_AGENT_SYMBOL_CREATE,
            AGENTXX_PLUGIN_AGENT_SYMBOL_START,
            AGENTXX_PLUGIN_AGENT_SYMBOL_STOP,
        };
    }

    /// 摘除领域注册: 工具/工具权限/图节点类型/prompt 贡献/中间件停用
    void detachDomainRegistrations(PluginInstance* inst) override;

    /// 摘除实例专属资源所有权: 中间件句柄 + 资源应用器的启用标记
    void detachDomainOwnedResources(PluginInstance* inst) override;

    /// 清空由插件 start 事务重新声明的领域记录 (工具/权限/hook/图)
    void clearDomainRegistrations(PluginInstance* inst) override;

    /// 应用清单声明的资源 (skill/memory/mcp) 并冻结资源声明
    void applyDeclaredResources(
        PluginInstance&                        inst,
        const plugin::PluginManifestResources& resources
    ) override;

    /// 卸载时释放实例级资源: 工具对象列表 + 清单资源所有权
    void releaseInstanceResources(PluginInstance& inst) override;

    /// 启用状态变化通知 (资源应用器的启用标记)
    void onInstanceEnabledChanged(PluginInstance& inst, bool enabled) override;

private:

    friend class PluginInstance;

    void eraseMiddleware(PluginMiddlewareHandle* mw);

    /// 释放执行图定义 slot (占用者禁用/卸载时调用): 恢复基础定义并清空占用者
    void releaseGraphDefinitionSlot(PluginInstance* inst);

    /// agent 装配的权限中间件 (插件工具权限声明实际生效处; 未装配返回 nullptr)
    /// - 在中间件链中查找; 权限中间件由 BaseAgent::initMiddleware 装配, 插件
    ///   加载 (create/start 事务) 在其后执行, 正常运行期可查到
    agentxx::middleware::PermissionMiddlewareHandle* permissionMiddleware();

    struct PendingMiddlewareCleanup {
        std::string                           name;
        std::weak_ptr<PluginMiddlewareHandle> mw;
    };

    std::vector<PendingMiddlewareCleanup> pendingCleanups_;

    std::weak_ptr<agentxx::agent::AgentContext>                        agentContext_;
    std::shared_ptr<ToolRegistry>                                      registry_;
    std::map<std::string, std::shared_ptr<GraphTypeSlot>, std::less<>> graphTypeSlots_;
    size_t                                                             runningTurns_ = 0;
    std::map<std::string, PromptKeyState, std::less<>>                 promptKeys_;
    uint64_t                                                           promptSequence_ = 0;

    /// 执行图定义独占 slot (计划 PLG-4): 占用者 + 占用前的基础定义
    /// - [graphDefinitionBase] 存 JSON 文本, 恢复时解析回 neograph json
    ///   (避免在头文件里带 neograph 类型)
    std::string    graphDefinitionOwner_;
    std::string    graphDefinitionBase_;
    bool           graphDefinitionBaseCaptured_ = false;
};

} // namespace plugin
} // namespace agentxx
