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
    PromptBackup                           promptBackup;

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
    };

    explicit PluginManager(std::weak_ptr<agentxx::agent::AgentContext> agentContext);
    ~PluginManager();

    PluginManager(const PluginManager&)            = delete;
    PluginManager& operator=(const PluginManager&) = delete;

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
    /// - 落地点为 agent 装配的权限中间件 ([PermissionMiddlewareHandle]):
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
    /// - 判定口径与工具调用权限检查一致, 仅把"应询问"以 ASK 返回 (见 agentxx.agent.permission)
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

    /// 注册插件节点类型到 per-agent GraphRegistry (插件 graph 接口表)
    int registerGraphNodeType(PluginInstance* inst, const AgentxxPluginGraphNodeTypeSpec* spec);
    /// 注销插件节点类型 (按类型名; 卸载时宿主自动清理)
    int unregisterGraphNodeType(PluginInstance* inst, PluginxxStringView type);

    int unregisterGraphNodeType(PluginInstance* inst, std::string_view type) {
        return unregisterGraphNodeType(inst, strToSv(type));
    }

    /// 获取当前执行图 JSON 定义 (host->alloc 语义由 vtable 层处理)
    std::string getGraphJson();
    /// 设置执行图 JSON 定义 (覆盖; 非法 JSON 返回非 0)
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
    std::string getSessionWorkDir(const std::string& threadId);
    std::string getModelConfigJson();
    bool        isSessionCancelled(const std::string& threadId);

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
    bool        isSessionCancelled(std::string_view sessionId) override;

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

    /// agent 装配的权限中间件 (插件工具权限声明的落地处; 未装配返回 nullptr)
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
};

} // namespace plugin
} // namespace agentxx
