#pragma once

#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/api/plugin_api.h"
// 宿主侧 vtable/管理器实现使用 SDK 提供的跨边界字符串工具 (PluginStringView /
// PluginString); 该头后续会拆为 pluginxx/kit/kit.h (通用部分) + 本头的领域 helper
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/plugin_framework.h"
#include "agentxx/plugin/plugin_interfaces.h"

#include "agentxx/plugin/tool_registry.h"
#include "agentxx/tools/tool.h"
#include "asio/awaitable.hpp"
#include "asio/steady_timer.hpp"
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

struct AgentxxPluginSubscription {
    std::shared_ptr<agentxx::events::EventBus>     bus;
    std::string                                    topic;
    size_t                                         subscriptionId = 0;
    std::weak_ptr<agentxx::plugin::PluginInstance> inst;
    void(AGENTXX_PLUGIN_CALL* handler)(const AgentxxPluginStringView* event_json, void* ud)
        = nullptr;
    void*             ud = nullptr;
    std::atomic<bool> alive{true};
};

namespace agentxx {
namespace plugin {

class PluginInstance : public PluginInstanceBase {
public:

    /// 继承 PluginInstanceBase 的公共字段 (name/version/path/configPath/args/depends/
    /// dlHandle/pluginCtx/enabled/inflight 等), 见
    /// [pluginxx/runtime/instance_base.h](/agent/third_party/cxx_pluginxx/include/pluginxx/runtime/instance_base.h)
    /// 接口声明 (plugin.yaml `interfaces`; 加载时随 manifest 解析传入,
    /// 直连库路径为空) —— 经 list() 暴露供展示/排查
    PluginManifestInterfaces interfaces;
    AgentxxPluginDestroyFn   builtinUnload = nullptr;
    /// 资源冻结标志: 插件初始化阶段 (create 内) 允许注册 skill/memory/mcp,
    /// 初始化完成后冻结，后续固定不可变以防上下文变化 (仅 yaml 声明与初始化追加生效)
    bool resourcesFrozen = false;

    struct HookRegistration {
        int32_t point;
        void*(AGENTXX_PLUGIN_CALL*
                  start)(void*, int32_t, const AgentxxPluginStringView*, const AgentxxPluginOperatorNotify*, AgentxxPluginString*);
        void(AGENTXX_PLUGIN_CALL* cancel)(void*, void*);
        void* ud;
    };

    struct CapabilityRegistration {
        std::string                          name;
        AgentxxPluginCapabilityStartFunction start  = nullptr;
        AgentxxPluginOperatorCancelFunction  cancel = nullptr;
        void*                                ctx    = nullptr;
    };

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
    std::vector<std::string>                                permissionToolNames;
    std::vector<HookRegistration>                           hookRegistrations;
    std::vector<std::shared_ptr<AgentxxPluginSubscription>> subscriptions;
    std::vector<std::shared_ptr<AgentxxPluginSubscription>> subscriptionHandles;
    std::vector<CapabilityRegistration>                     capabilityRegistrations;
    std::vector<GraphNodeTypeRegistration>                  graphNodeTypes;
    /// 活跃 sleep 使用 Operation 句柄索引；完成回调开始前移除，取消查询 O(1)。
    std::unordered_map<void*, std::shared_ptr<AgentxxPluginOperatorHandle>> sleepTimers;
    PromptBackup                                                            promptBackup;

    std::shared_ptr<PluginMiddlewareHandle>  middleware = nullptr;
    std::vector<std::shared_ptr<PluginTool>> tools;

    std::weak_ptr<PluginInstance> self{};
    std::weak_ptr<PluginManager>  manager{};

    explicit PluginInstance(std::string in_name) :
        PluginInstanceBase(std::move(in_name)) {}

    ~PluginInstance();

    /// 在所有活动 lease 归零后销毁插件上下文；析构时也作为最后一道安全收尾。
    /// 返回 false 表示仍有活动 lease，调用方不得关闭动态库。
    bool destroyPlugin() noexcept;
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
    utilxx_base::Json           parameters_;
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
        void*(AGENTXX_PLUGIN_CALL*
                  start)(void*, int32_t, const AgentxxPluginStringView*, const AgentxxPluginOperatorNotify*, AgentxxPluginString*)
            = nullptr;
        void(AGENTXX_PLUGIN_CALL* cancel)(void*, void*) = nullptr;
        void* ud                                        = nullptr;
        bool  set                                       = false;
    };

    asio::awaitable<void>
        dispatch(AgentxxPluginHookPoint point, const neograph::graph::NodeInput& in);

    std::weak_ptr<PluginInstance>                    instance_;
    std::array<HookEntry, AGENTXX_PLUGIN_HOOK_COUNT> hooks_{};
};

class PluginManager : public PluginManagerBase<PluginInstance>,
                      public std::enable_shared_from_this<PluginManager> {
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

    asio::awaitable<bool> unloadAsync(
        std::string_view          name,
        std::chrono::milliseconds timeout = std::chrono::seconds{30}
    );
    /// 在所属 IO executor 仍运行时等待所有实例安全关闭。
    /// 失败实例保留 context/DSO，可再次调用本方法重试。
    asio::awaitable<bool>
         shutdownAsync(std::chrono::milliseconds timeout = std::chrono::seconds{30});
    void disable(std::string_view name);
    void enable(std::string_view name);
    void flushPendingCleanup();

    asio::awaitable<void>
        loadConfiguredPlugins(const std::vector<agentxx::agent::PluginConfig>& plugins);

    asio::awaitable<std::shared_ptr<PluginInstance>> loadPluginAsync(
        std::string                         path,
        const agentxx::agent::PluginConfig* cfg                 = nullptr,
        bool                                allowClientOnlySkip = false
    );

    void shutdownAll();

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
    int unregisterTool(PluginInstance* inst, AgentxxPluginStringView name);

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
    int unregisterToolPermission(PluginInstance* inst, AgentxxPluginStringView toolName);

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

    int registerSkillDir(PluginInstance* inst, AgentxxPluginStringView path);

    int registerSkillDir(PluginInstance* inst, std::string_view path) {
        return registerSkillDir(inst, strToSv(path));
    }

    int unregisterSkillDir(PluginInstance* inst, AgentxxPluginStringView path);

    int unregisterSkillDir(PluginInstance* inst, std::string_view path) {
        return unregisterSkillDir(inst, strToSv(path));
    }

    int registerMemoryFile(PluginInstance* inst, AgentxxPluginStringView path);

    int registerMemoryFile(PluginInstance* inst, std::string_view path) {
        return registerMemoryFile(inst, strToSv(path));
    }

    int unregisterMemoryFile(PluginInstance* inst, AgentxxPluginStringView path);

    int unregisterMemoryFile(PluginInstance* inst, std::string_view path) {
        return unregisterMemoryFile(inst, strToSv(path));
    }

    int registerMcpServer(PluginInstance* inst, AgentxxPluginStringView specJson);

    int registerMcpServer(PluginInstance* inst, std::string_view specJson) {
        return registerMcpServer(inst, strToSv(specJson));
    }

    int unregisterMcpServer(PluginInstance* inst, AgentxxPluginStringView nameSpace);

    int unregisterMcpServer(PluginInstance* inst, std::string_view nameSpace) {
        return unregisterMcpServer(inst, strToSv(nameSpace));
    }

    std::string ownResourcesJson(const PluginInstance* inst);

    int registerHook(PluginInstance* inst, const AgentxxPluginHookSpec* spec);
    int unregisterHook(PluginInstance* inst, AgentxxPluginHookPoint point);

    /// 注册插件节点类型到 per-agent GraphRegistry (插件 graph 接口表)
    int registerGraphNodeType(PluginInstance* inst, const AgentxxPluginGraphNodeTypeSpec* spec);
    /// 注销插件节点类型 (按类型名; 卸载时宿主自动清理)
    int unregisterGraphNodeType(PluginInstance* inst, AgentxxPluginStringView type);

    int unregisterGraphNodeType(PluginInstance* inst, std::string_view type) {
        return unregisterGraphNodeType(inst, strToSv(type));
    }

    /// 获取当前执行图 JSON 定义 (host->alloc 语义由 vtable 层处理)
    std::string getGraphJson();
    /// 设置执行图 JSON 定义 (覆盖; 非法 JSON 返回非 0)
    int setGraphJson(PluginInstance* inst, AgentxxPluginStringView graph_json);

    int setGraphJson(PluginInstance* inst, std::string_view graph_json) {
        return setGraphJson(inst, strToSv(graph_json));
    }

    AgentxxPluginSubscription* subscribe(
        PluginInstance*         inst,
        AgentxxPluginStringView topic,
        void(AGENTXX_PLUGIN_CALL* handler)(const AgentxxPluginStringView* event_json, void* ud),
        void* ud
    );

    AgentxxPluginSubscription* subscribe(
        PluginInstance*  inst,
        std::string_view topic,
        void(AGENTXX_PLUGIN_CALL* handler)(const AgentxxPluginStringView* event_json, void* ud),
        void* ud
    ) {
        return subscribe(inst, strToSv(topic), handler, ud);
    }

    void unsubscribe(AgentxxPluginSubscription* sub);
    int  publish(AgentxxPluginStringView topic, AgentxxPluginStringView event_json);

    int publish(std::string_view topic, std::string_view event_json) {
        return publish(strToSv(topic), strToSv(event_json));
    }

    AgentxxPluginString
        getShareStore(PluginInstance* inst, AgentxxPluginStringView session_id, int64_t id);

    AgentxxPluginString
        getShareStore(PluginInstance* inst, std::string_view session_id, int64_t id) {
        return getShareStore(inst, strToSv(session_id), id);
    }

    int64_t addShareStore(
        PluginInstance*         inst,
        AgentxxPluginStringView session_id,
        AgentxxPluginStringView content
    );

    int64_t
        addShareStore(PluginInstance* inst, std::string_view session_id, std::string_view content) {
        return addShareStore(inst, strToSv(session_id), strToSv(content));
    }

    void emitMessageTip(
        PluginInstance*         inst,
        AgentxxPluginStringView session_id,
        AgentxxPluginStringView text,
        int32_t                 level
    );

    void emitMessageTip(
        PluginInstance*  inst,
        std::string_view session_id,
        std::string_view text,
        int32_t          level
    ) {
        emitMessageTip(inst, strToSv(session_id), strToSv(text), level);
    }

    AgentxxPluginOperatorHandle*
        postCallback(PluginInstance* inst, void(AGENTXX_PLUGIN_CALL* fn)(void*), void* ud);

    AgentxxPluginOperatorHandle* sleep(
        PluginInstance*               inst,
        int64_t                       ms,
        AgentxxPluginOperatorCallback cb,
        void*                         ud,
        AgentxxPluginString*          error_out
    );
    AgentxxPluginOperatorHandle* offload(
        PluginInstance* inst,
        void*(AGENTXX_PLUGIN_CALL* work)(
            void*                           ud,
            const AgentxxPluginCancelToken* token,
            AgentxxPluginString*            error_out
        ),
        void(AGENTXX_PLUGIN_CALL* done)(
            void*                          ud,
            int32_t                        status,
            void*                          result,
            const AgentxxPluginStringView* error
        ),
        void*                ud,
        AgentxxPluginString* error_out
    );

    AgentxxPluginOperatorHandle* callToolAsync(
        PluginInstance*               caller,
        AgentxxPluginStringView       name,
        AgentxxPluginStringView       args_json,
        AgentxxPluginStringView       session_id,
        AgentxxPluginOperatorCallback cb,
        void*                         ud,
        AgentxxPluginString*          error_out
    );

    AgentxxPluginOperatorHandle* callToolAsync(
        PluginInstance*               caller,
        std::string_view              name,
        std::string_view              args_json,
        std::string_view              session_id,
        AgentxxPluginOperatorCallback cb,
        void*                         ud,
        AgentxxPluginString*          error_out
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

    AgentxxPluginOperatorHandle* invokeCapabilityAsync(
        PluginInstance*               caller,
        AgentxxPluginStringView       capability,
        AgentxxPluginStringView       method,
        AgentxxPluginStringView       args_json,
        AgentxxPluginOperatorCallback cb,
        void*                         ud,
        AgentxxPluginString*          error_out
    );

    AgentxxPluginOperatorHandle* invokeCapabilityAsync(
        PluginInstance*               caller,
        std::string_view              capability,
        std::string_view              method,
        std::string_view              args_json,
        AgentxxPluginOperatorCallback cb,
        void*                         ud,
        AgentxxPluginString*          error_out
    ) {
        return invokeCapabilityAsync(
            caller,
            strToSv(capability),
            strToSv(method),
            strToSv(args_json),
            cb,
            ud,
            error_out
        );
    }

    /// 注册后台任务 (spawn 宿主托管; agentxx.agent.tasks 接口表)
    /// - cancel_fn/cancel_ud: 卸载取消时宿主回调 (io 线程, 协作式)
    /// - notify: 【出参】插件协程结束 (帧销毁后) 经 notify.done 恰好一次上报
    /// - 返回宿主托管句柄 (失败 NULL + error_out); 句柄仅用于 cancel_task,
    ///   宿主在任务 done 后自动回收 (与 callToolAsync/invokeCapabilityAsync
    ///   的同款清理协程模式一致)
    AgentxxPluginOperatorHandle* registerTask(
        PluginInstance*                     inst,
        AgentxxPluginOperatorCancelFunction cancel_fn,
        void*                               cancel_ud,
        AgentxxPluginOperatorNotify*        notify,
        AgentxxPluginString*                error_out
    );

    std::shared_ptr<ToolRegistry> registry() const {
        return registry_;
    }

    std::shared_ptr<CapabilityRegistry> capabilities() const {
        return capabilities_;
    }

    int registerCapability(PluginInstance* inst, AgentxxPluginStringView capability);

    int registerCapability(PluginInstance* inst, std::string_view capability) {
        return registerCapability(inst, strToSv(capability));
    }

    int registerCapabilityEx(
        PluginInstance*                      inst,
        AgentxxPluginStringView              capability,
        AgentxxPluginCapabilityStartFunction start,
        AgentxxPluginOperatorCancelFunction  cancel,
        void*                                ctx
    );

    int registerCapabilityEx(
        PluginInstance*                      inst,
        std::string_view                     capability,
        AgentxxPluginCapabilityStartFunction start,
        AgentxxPluginOperatorCancelFunction  cancel,
        void*                                ctx
    ) {
        return registerCapabilityEx(inst, strToSv(capability), start, cancel, ctx);
    }

    int unregisterCapability(PluginInstance* inst, AgentxxPluginStringView capability);

    int unregisterCapability(PluginInstance* inst, std::string_view capability) {
        return unregisterCapability(inst, strToSv(capability));
    }

    int hasCapability(AgentxxPluginStringView capability) const;

    int hasCapability(std::string_view capability) const {
        return hasCapability(strToSv(capability));
    }

    std::string listPluginsJson();
    std::string getPluginJson(const std::string& name);

    std::string getConfigJson();
    std::string getToolPromptJson(const std::string& toolName);
    std::string getPromptJson();
    int         setPromptJson(PluginInstance* inst, AgentxxPluginStringView prompt_json);

    int setPromptJson(PluginInstance* inst, std::string_view prompt_json) {
        return setPromptJson(inst, strToSv(prompt_json));
    }

    void restorePromptBackup(PluginInstance* inst);
    void applyDeclaredResources(
        PluginInstance&                        inst,
        const plugin::PluginManifestResources& resources
    );
    std::string getPluginArgsJson(PluginInstance* inst);
    std::string getPluginConfigPath(PluginInstance* inst);
    std::string getLanguage();
    void        setLanguage(std::string_view lang);

    std::string getSessionWorkDir();
    std::string getSessionWorkDir(const std::string& threadId);
    std::string getModelConfigJson();
    bool        isSessionCancelled(const std::string& threadId);

    void detachAll(PluginInstance* inst);

    /// 禁用/启用事务的内部实现（级联递归用）：
    /// - `userInitiated=true` 表示用户显式操作，会更新 `userDisabled`；
    /// - 级联（false）只维护 `blockedByDependencies`，不覆盖用户显式禁用标记。
    /// 依赖级联按直接依赖者递归，覆盖三级/菱形依赖。
    void disableImpl(std::string_view name, bool userInitiated);
    void enableImpl(std::string_view name, bool userInitiated);

    /// 摘除实例在宿主侧的注册（工具/工具权限/hook/capability/graph/订阅/prompt 贡献），
    /// 但保留实例内的注册记录；启用时由 start 事务重新声明。
    void detachInstanceRegistrations(PluginInstance* inst);

    /// 清空"由插件 start 事务重新声明"的注册记录（工具/工具权限/hook/capability/graph）。
    /// stop 成功后调用，避免下次 start 在旧记录上重复累积。
    void clearPluginOwnedRegistrations(PluginInstance* inst);

    /// 按需投递禁用/启用事务到本管理器 IO executor（同步入口的异步收尾）。
    void requestStopForDisable(const std::shared_ptr<PluginInstance>& inst);
    void requestStartForEnable(const std::shared_ptr<PluginInstance>& inst);

    /// 禁用/启用事务的异步收尾（仅 IO 线程）：
    /// - `stopForDisable`：调用插件 stop 导出，撤销插件自管资源（订阅/线程/定时器）；
    ///   失败只记录日志并保持 Disabled（可再次 disable/enable 重试）。
    /// - `startForEnable`：调用插件 start 导出重新注册；成功后状态回到 Ready。
    asio::awaitable<void> stopForDisable(std::shared_ptr<PluginInstance> inst);
    asio::awaitable<void> startForEnable(std::shared_ptr<PluginInstance> inst);

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

    void shutdownPlugin(const std::shared_ptr<PluginInstance>& inst);

    /// 加载/启动失败时的统一回滚 (摘除注册 → 销毁上下文 → 移出插件表 → 释放名称预占)
    void rollbackLoad(const std::shared_ptr<PluginInstance>& inst, bool closeHandle);

    /// 装配插件实例 (元信息/生命周期入口/宿主控制块; 两种加载路径共用)
    std::shared_ptr<PluginInstance> makeInstance(
        std::string                name,
        const AgentxxPluginInfo*   info,
        std::string                path,
        const AgentxxPluginStartFn startFn,
        const AgentxxPluginStopFn  stopFn
    );

    /// create + start 成功后的公共收尾 (应用声明式资源 → 冻结 → Ready)
    void finishLoad(
        const std::shared_ptr<PluginInstance>& inst,
        const plugin::PluginManifestResources& resources
    );

    asio::awaitable<bool>
        unloadAsyncUntil(std::string name, std::chrono::steady_clock::time_point deadline);

    std::weak_ptr<agentxx::agent::AgentContext>                        agentContext_;
    std::shared_ptr<ToolRegistry>                                      registry_;
    std::shared_ptr<CapabilityRegistry>                                capabilities_;
    std::map<std::string, std::shared_ptr<GraphTypeSlot>, std::less<>> graphTypeSlots_;
    size_t                                                             runningTurns_ = 0;
    std::map<std::string, PromptKeyState, std::less<>>                 promptKeys_;
    uint64_t                                                           promptSequence_ = 0;
};

} // namespace plugin
} // namespace agentxx
