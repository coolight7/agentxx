#ifndef AGENTXX_PLUGIN_PLUGIN_MOUNT_H
#define AGENTXX_PLUGIN_PLUGIN_MOUNT_H

/// 插件挂载: 一个 agent 上的插件实体集合
///
/// ## 为什么单独有一层
///
/// 插件管理器 ([PluginManager]) 是**进程一份**: 插件只 `dlopen`/`create`/`start` 一次,
/// 它的线程、连接、能力声明属于整个进程。但每一样"落在 agent 上的东西" —— 工具、
/// 工具权限、钩子处理器、提示词贡献、功能点、图节点类型/图定义、声明式资源、
/// 事件订阅 —— 都属于某个具体的 agent: 换一个 agent 就是另一套工具表、另一条中间件
/// 链、另一份提示词。
///
/// 因此把这些东西收进 [PluginMount]: 一个 agent 一个挂载, 挂载记录"哪个实例在这个
/// agent 上注册了什么", 拆挂载时按记录逐项撤销, 不影响同进程里别的 agent。
///
/// ## 生命周期
///
/// ```
/// mountAgentAsync(ctx)          建立挂载 (管理器为它分配挂载序号)
///   └─ mountOneAsync(inst)      每个实例: 应用清单资源 → 驱动插件 mount 回调
/// unmountOneAsync(inst)         拆一个实例 (调插件 unmount → 按记录撤销)
/// unmountAllAsync()             拆全部 (逆序)
/// ```
///
/// 插件引用 (`PluginxxScopeRef` = 挂载时下发的 `agent_id` + `mount_seq`) 就是指向本
/// 对象的凭据: 主程序用 [PluginManager::mountOf] 把引用解析成挂载, 解析不到一律拒绝
/// (注册类返回 `AGENTXX_PLUGIN_ERR_NO_AGENT`, 读取类返回 `-1`)。
///
/// ## 线程
///
/// 挂载的全部状态只在主程序 io 线程读写: 注册入口由 vtable 层投递, 轮次钩子与派发也在
/// 这条线程。插件回调 (mount/unmount) 同样由主程序在这条线程驱动。
#include "agentxx/agent/prompt.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/plugin_framework.h"
#include "agentxx/plugin/tool_registry.h"
#include "agentxx/tools/tool.h"
#include "asio/awaitable.hpp"
#include "utilxx_base/json.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace agent {
class AgentContext;
} // namespace agent

namespace middleware {
class PermissionMiddlewareHandle;
} // namespace middleware

namespace plugin {

class PluginInstance;
class PluginManager;
class PluginTool;
struct GraphTypeSlot;

/// 主程序级钩子派发器 (agent 的中间件链上只挂一个)
///
/// 挂钩子的中间件句柄从"每个插件一个"改成"每次挂载一个": 注册表 (见
/// [PluginMount::hookHandlers_]) 按 `(层, priority, 登记序号)` 排序后由本句柄依次
/// 执行处理器 —— 同一个点的处理器串行、都在主程序 io 线程、异常只记日志继续。
///
/// - 首次登记时插入中间件链, 没有处理器时移除 (轮次执行中先置 `disabled` 跳过,
///   轮末由 [PluginMount::flushPendingCleanup] 摘除);
/// - 处理器列表在派发开始时取快照: 处理器在派发过程中登记/撤销不影响本次派发
///   (撤销后已快照的处理器仍会执行一次, 但每次执行前仍检查实例是否可用)。
class PluginHookDispatchHandle
    : public agentxx::middleware::BaseMiddlewareHandle<agentxx::middleware::BaseMiddlewareState> {
public:

    PluginHookDispatchHandle(
        std::string_view                            name,
        std::weak_ptr<agentxx::agent::AgentContext> agentContext,
        std::weak_ptr<class PluginMount>            mount
    ) :
        BaseMiddlewareHandle(name, std::move(agentContext)),
        mount_(std::move(mount)) {}

    PluginHookDispatchHandle(const PluginHookDispatchHandle&)            = delete;
    PluginHookDispatchHandle& operator=(const PluginHookDispatchHandle&) = delete;

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

    std::weak_ptr<PluginMount> mount_;
};

/// 一个 agent 的插件挂载
class PluginMount : public std::enable_shared_from_this<PluginMount> {
public:

    /// 一个插件实例在本挂载上的记录
    ///
    /// - `mountUserData` / `pluginMounted` / `resourcesFrozen` 是插件 `mount` 回调的
    ///   产物与状态;
    /// - 其余是撤销记录: 注册时逐条追加, 拆挂载时按 §"撤销顺序" 逐类撤销
    ///   (事件订阅 → MCP → memory → skill → 图定义 → 图节点类型 → 提示词 →
    ///   功能点实现 → 功能点声明 → 钩子 → 工具权限 → 工具)。
    struct Entry {
        std::shared_ptr<PluginInstance> inst;
        /// 插件 `mount` 返回的 per-agent 状态 (卸载时原样交回)
        void* mountUserData = nullptr;
        /// 插件 `mount` 是否已成功 (没有 mount 入口 = true)
        bool pluginMounted = false;
        /// 插件 `unmount` 回调还欠着 (禁用时同步拆记录, 回调稍后在 IO 线程补)
        bool unmountPending = false;
        /// 资源声明是否已冻结: 挂载期间允许登记 skill/memory/mcp, 挂载收尾冻结
        bool resourcesFrozen = false;
        /// 本挂载的插件配置 JSON (加载表 args 与 `plugin.mount[].args` 的合并结果)
        std::string configJson;

        /// 本挂载上注册的工具名 (含对象, 保活执行中的调用)
        std::vector<std::string>                 tools;
        std::vector<std::shared_ptr<PluginTool>> toolObjects;
        /// 本挂载上声明的工具权限 (按工具名)
        std::vector<std::string> permissionTools;
        /// 本挂载登记的钩子处理器句柄
        std::vector<int64_t> hookHandles;
        /// 本挂载占用的提示词键 (`system` / `append:<key>` / `tool:<名>`)
        std::vector<std::string> promptKeys;
        /// 本挂载声明的功能点 id (`plugin.<自己>.*`)
        std::vector<std::string> featurePoints;
        /// 本挂载登记的功能点实现所在点 id
        std::vector<std::string> featureImpls;
        /// 本挂载注册的自定义图节点类型
        struct GraphNodeRegistration {
            std::string                       type;
            AgentxxPluginGraphNodeRunStartFn  run_start  = nullptr;
            AgentxxPluginGraphNodeRunCancelFn run_cancel = nullptr;
            void*                             user_data  = nullptr;
            std::string                       config_schema_json;
            std::shared_ptr<GraphTypeSlot>    slot;
        };
        std::vector<GraphNodeRegistration> graphNodes;
        /// 是否占用本 agent 的执行图定义 (独占 slot)
        bool ownsGraphDefinition = false;
    };

    PluginMount(
        std::shared_ptr<PluginManager>              host,
        std::weak_ptr<agentxx::agent::AgentContext> agent,
        std::string                                 agentId,
        std::string                                 agentName,
        uint64_t                                    mountSeq
    );
    ~PluginMount();

    PluginMount(const PluginMount&)            = delete;
    PluginMount& operator=(const PluginMount&) = delete;

    /// 管理器持有的 agent 上下文 (挂载期间不会失效: 主程序在 agent 关闭时先拆挂载)
    std::shared_ptr<agentxx::agent::AgentContext> agentContext() const {
        return agent_.lock();
    }

    const std::string& agentId() const noexcept {
        return agentId_;
    }

    const std::string& agentName() const noexcept {
        return agentName_;
    }

    /// 挂载序号 (主程序分配, 单调递增; 插件引用里的 `seq` 必须等于它)
    uint64_t mountSeq() const noexcept {
        return mountSeq_;
    }

    std::shared_ptr<PluginManager> host() const {
        return host_.lock();
    }

    // =====================================================================
    // 挂载 / 拆挂载 (仅主程序 io 线程)
    // =====================================================================

    /// 挂载一个实例 (幂等: 已挂过直接返回 true; 无 mount 入口视为成功)
    asio::awaitable<bool> mountOneAsync(std::shared_ptr<PluginInstance> inst);

    /// 拆一个实例: 调插件 unmount, 再按记录撤销本挂载上的实体
    asio::awaitable<void> unmountOneAsync(std::shared_ptr<PluginInstance> inst);

    /// 拆全部 (逆序)
    asio::awaitable<void> unmountAllAsync();

    /// 本挂载上的实例记录 (未挂载返回 nullptr)
    Entry* findEntry(const PluginInstance* inst);
    const Entry* findEntry(const PluginInstance* inst) const;

    /// 该实例是否还挂在别的挂载上 (不含本挂载; 用于判断进程级登记是否需要撤销)
    bool mountedElsewhere(const PluginInstance* inst) const;

    /// 已挂载的实例数
    size_t mountedCount() const;

    /// 挂载摘要 (挂载表 `list` 的一项)
    utilxx_base::Json infoJson();

    /// 本挂载上的实例名列表 (按挂载顺序)
    std::vector<std::string> mountedNames() const;

    /// 删掉一个实例在本挂载上的注册 (禁用 / 卸载路径)
    /// - 保留 entry 本身 (含 `mountUserData`), 拆挂载时还要用它调插件 `unmount`
    void detachInstance(PluginInstance* inst);

    /// 清空一个实例在本挂载上的注册记录 (插件 `stop` 成功后调用)
    void clearInstanceRecords(PluginInstance* inst);

    /// 释放一个实例在本挂载上的工具对象 (卸载路径)
    void releaseInstanceInstances(PluginInstance& inst);

    // =====================================================================
    // 轮次状态 (钩子派发器的摘除时机)
    // =====================================================================

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

    /// 轮末摘除排队中的中间件句柄 (钩子派发器)
    void flushPendingCleanup();

    // =====================================================================
    // 工具与工具权限 (agentxx.agent.tools / .permission)
    // =====================================================================

    int registerTool(PluginInstance* inst, const AgentxxPluginToolSpec* spec);
    int unregisterTool(PluginInstance* inst, PluginxxStringView name);

    int unregisterTool(PluginInstance* inst, std::string_view name) {
        return unregisterTool(inst, strToSv(name));
    }

    /// 声明工具权限限制 (插件在注册工具后调用)
    /// - 声明内容: 权限作用域 (读/写)、目标参数名、目标类型 (路径/文本/无)
    /// - 声明最终交给本 agent 加载的权限中间件 ([PermissionMiddlewareHandle]):
    ///   工具调用时的判定 (白/黑名单、permission.mode 默认、记住的选择、
    ///   工作区隔离、完全授权) 全部由该中间件执行, 插件只声明"哪些参数受约束"
    /// - 权限声明属于附加能力: 主程序未加载权限中间件时返回非 0, 插件可忽略
    /// `return`: 0 成功, 非 0 失败 (工具非本实例所有 / 中间件不可用 / 声明非法)
    int registerToolPermission(PluginInstance* inst, const AgentxxPluginToolPermissionSpec* spec);

    /// 撤销工具权限声明 (按工具名; 工具注销、插件禁用/卸载时由主程序自动撤销)
    /// `return`: 0 成功, 非 0 不存在
    int unregisterToolPermission(PluginInstance* inst, PluginxxStringView toolName);

    int unregisterToolPermission(PluginInstance* inst, std::string_view toolName) {
        return unregisterToolPermission(inst, strToSv(toolName));
    }

    /// 批量查询路径权限判定 (只读; 不发起询问/不产生中断)
    /// - 判定规则与工具调用权限检查一致, 仅把"应询问"以 ASK 返回
    /// - 供支持模式/前缀参数的插件工具在枚举出实际路径后逐项过滤 (glob/grep 等)
    ///
    /// - `args`:
    ///     - [inst]  调用方插件实例 (仅用于日志/归属)
    ///     - [scope] 权限作用域 (AGENTXX_PLUGIN_PERMISSION_SCOPE_*)
    ///     - [sessionId] 会话 (取会话工作目录/隔离边界; 可为空)
    ///     - [paths] 待判定路径 (绝对路径优先; 相对路径按会话工作目录解析)
    ///     - [outDecisions] 等长出参 (AGENTXX_PLUGIN_PERMISSION_DECISION_*)
    ///
    /// `return`: 0 成功; 非 0 不支持或失败 (权限中间件未加载时调用方应跳过过滤)
    int checkPermissionPaths(
        PluginInstance*                 inst,
        int32_t                         scope,
        std::string_view                sessionId,
        const std::vector<std::string>& paths,
        std::vector<int32_t>&           outDecisions
    );

    /// 调用本挂载上的一个工具 (插件互调)
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

    // =====================================================================
    // 声明式资源 (agentxx.agent.resources)
    // =====================================================================

    int registerSkillDir(PluginInstance* inst, PluginxxStringView path);
    int unregisterSkillDir(PluginInstance* inst, PluginxxStringView path);
    int registerMemoryFile(PluginInstance* inst, PluginxxStringView path);
    int unregisterMemoryFile(PluginInstance* inst, PluginxxStringView path);
    int registerMcpServer(PluginInstance* inst, PluginxxStringView specJson);
    int unregisterMcpServer(PluginInstance* inst, PluginxxStringView nameSpace);
    std::string ownResourcesJson(const PluginInstance* inst);

    /// 把清单声明的资源 (plugin.yaml 的 skill/memory/mcp) 应用到本 agent
    void applyManifestResourcesNow(PluginInstance& inst);

    // =====================================================================
    // 提示词 (agentxx.agent.prompt)
    // =====================================================================
    //
    // 插件对 prompt 的修改不用"备份后无条件写回", 而是记录为
    // (owner, key, sequence, value) 贡献:
    //   有效值 = 首次贡献前的基础值 ⊕ 按 sequence 顺序应用的全部存活贡献
    // 拆挂载/禁用只删除该 owner 的贡献并重新合成, 因此不会覆盖其他 owner 的贡献,
    // 也不会把已卸载 owner 的旧值写回; 用户或其他主程序代码之后写入的值通过
    // "基础值 rebase"保留。键名: `system` / `append:<key>` / `tool:<toolName>`。

    struct PromptValue {
        bool                                      isTool = false;
        std::string                               text; ///< isTool=false 时的值
        std::optional<agentxx::agent::ToolPrompt> tool; ///< isTool=true 时的值
    };

    struct PromptKeyState {
        /// 首次贡献前的基础值 (`nullopt` = 原本不存在)
        std::optional<PromptValue> base;
        /// 主程序上次合成写入的值 (用于发现外部修改并 rebase 基础值)
        std::optional<PromptValue> applied;
        /// owner -> (sequence, 贡献值), 按 sequence 升序应用
        std::map<std::string, std::pair<uint64_t, PromptValue>, std::less<>> contributions;
    };

    std::string getPromptJson();
    int         setPromptJson(PluginInstance* inst, PluginxxStringView prompt_json);

    int setPromptJson(PluginInstance* inst, std::string_view prompt_json) {
        return setPromptJson(inst, strToSv(prompt_json));
    }

    /// 删除 `owner` 的全部 prompt 贡献并重新合成受影响键
    void removePromptContributions(std::string_view owner);

    /// 重新合成单个 prompt 键的有效值 (内部使用; 见 PromptKeyState 说明)
    void recomposePromptKey(const std::string& key);

    // =====================================================================
    // 会话与 share store (agentxx.agent.session / .context)
    // =====================================================================

    PluginxxString getShareStore(PluginInstance* inst, PluginxxStringView session_id, int64_t id);

    int64_t addShareStore(
        PluginInstance*    inst,
        PluginxxStringView session_id,
        PluginxxStringView content
    );

    void emitMessageTip(
        PluginInstance*    inst,
        PluginxxStringView session_id,
        PluginxxStringView text,
        int32_t            level
    );

    /// 会话 LLM 上下文 (Json 数组文本; 主程序 alloc, 失败返回空串)
    PluginxxString getSessionMessages(PluginxxStringView session_id);

    /// 会话 LLM 上下文条数 (会话不存在返回 -1)
    int64_t sessionMessagesCount(PluginxxStringView session_id);

    /// 写会话 LLM 上下文 (插件图节点对 `messages` 通道的写入转发; 仅 io 线程)
    size_t writeSessionMessages(
        std::string_view         session_id,
        const utilxx_base::Json& messages,
        bool                     overwrite
    );

    // =====================================================================
    // 功能点 (agentxx.agent.feature)
    // =====================================================================

    /// 为某个点登记实现 (同一 `(点, 实例)` 重复登记 = 覆盖)
    int registerFeatureImpl(PluginInstance* inst, const AgentxxPluginFeatureImplSpec* spec);

    /// 撤销本实例在某点上的实现
    int unregisterFeatureImpl(PluginInstance* inst, PluginxxStringView pointId);

    int unregisterFeatureImpl(PluginInstance* inst, std::string_view pointId) {
        return unregisterFeatureImpl(inst, strToSv(pointId));
    }

    /// 声明本实例自己的功能点 (id 必须落在 `plugin.<本实例插件名>.*`)
    int defineFeaturePoint(PluginInstance* inst, const AgentxxPluginFeaturePointSpec* spec);

    /// 撤销本实例声明的点 (连带撤掉这些点上的全部实现)
    int undefineFeaturePoint(PluginInstance* inst, PluginxxStringView pointId);

    int undefineFeaturePoint(PluginInstance* inst, std::string_view pointId) {
        return undefineFeaturePoint(inst, strToSv(pointId));
    }

    /// 功能点清单 JSON (插件 `list_points`、加载快照与诊断共用同一份实现)
    std::string listFeaturePoints();

    /// 调用一个功能点 (异步; 完成回调经 notify 在主程序 IO 线程发布)
    PluginxxOperatorHandle* callFeatureAsync(
        PluginInstance*          caller,
        PluginxxStringView       pointId,
        PluginxxStringView       argsJson,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    );

    // =====================================================================
    // 执行图 (agentxx.agent.graph)
    // =====================================================================

    int registerGraphNodeType(PluginInstance* inst, const AgentxxPluginGraphNodeTypeSpec* spec);
    int unregisterGraphNodeType(PluginInstance* inst, PluginxxStringView type);

    int unregisterGraphNodeType(PluginInstance* inst, std::string_view type) {
        return unregisterGraphNodeType(inst, strToSv(type));
    }

    /// 本 agent 当前的执行图 JSON 定义
    std::string getGraphJson();

    /// 设置本 agent 的执行图 JSON 定义 (独占 slot; 见 [graphDefinitionOwner])
    int setGraphJson(PluginInstance* inst, PluginxxStringView graph_json);

    int setGraphJson(PluginInstance* inst, std::string_view graph_json) {
        return setGraphJson(inst, strToSv(graph_json));
    }

    /// 执行图定义的占用者 (空 = 无插件占用, 使用内置/主程序定义)
    const std::string& graphDefinitionOwner() const {
        return graphDefinitionOwner_;
    }

    // =====================================================================
    // 钩子处理器 (agentxx.agent.hooks)
    // =====================================================================

    /// 钩子清单里的一个处理器 (只读视图; 清单 JSON 与诊断共用同一份事实)
    struct HookHandlerView {
        int64_t     handle = 0; ///< 登记时分配的句柄
        int32_t     point  = 0; ///< AgentxxPluginHookPoint
        std::string layer;      ///< `plugin` / `core`
        std::string owner;      ///< `plugin:<插件名>` / `core:<模块>`
        std::string ownerTag;   ///< 展示归属标签 (可空)
        std::string depict;     ///< 一句话说明 (可空)
        std::string load;       ///< `dynamic` / `builtin` (只说明怎么加载, 不影响顺序)
        int32_t     priority = 0;    ///< 生效优先级 (越界已裁剪到上下限)
        uint64_t    seq      = 0;    ///< 登记序号 (同优先级时按它排)
        bool        enabled  = true; ///< 派发时是否生效 (实例已禁用 -> false)
    };

    /// 登记一个钩子处理器 (单一入口: 同一点可登记任意多个, 各自句柄)
    /// - `priority` 越界裁剪到上下限并记一条警告 (不拒绝登记)
    ///
    /// `return`: 0 成功 (`*outHandle` 收到句柄); 非 0 失败
    int registerHook(
        PluginInstance*                inst,
        const AgentxxPluginHookSpecEx* spec,
        int64_t*                       outHandle
    );

    /// 撤销一个处理器 (按句柄; 别人的句柄被拒绝)
    int unregisterHook(PluginInstance* inst, int64_t handle);

    /// 登记一个 core 层处理器 (库内自用; 与功能点的 addCoreImpl 同一规则)
    /// - `plugin` 层 (插件 / FFI 主程序) 之后才是 `core` 层, `priority` 只在层内比较
    /// - **必须是同步处理器** (在主程序 io 线程直接调用, 不走操作协议)
    ///
    /// `return` 登记句柄 (> 0); 0 = 失败 (点越界 / 回调为空)
    int64_t addCoreHookHandler(
        AgentxxPluginHookPoint point,
        int32_t                priority,
        std::string            module,
        AgentxxPluginHookSpec  spec
    );

    /// 撤销一个 core 层处理器 (按句柄)
    bool removeCoreHookHandler(int64_t handle);

    /// 钩子处理器清单 JSON
    /// - 插件 `agentxx.agent.hooks` 的 `list_hooks`、加载快照 `hooks` 段与
    ///   `--dump-diagnostics` 共用同一份实现
    /// - 派发记录 (`stat` 段) 只在开发者模式下出现
    std::string hooksJson();

    /// 某个点上当前生效的处理器 (按派发顺序)
    std::vector<HookHandlerView> handlersOf(AgentxxPluginHookPoint point) const;

    /// 本挂载上属于这个实例的全部处理器 (清单 / 加载快照用)
    std::vector<HookHandlerView> hookHandlersOf(const PluginInstance& inst) const;

    /// 派发一个钩子点 (中间件链上唯一派发器的入口)
    asio::awaitable<void>
        dispatchHook(AgentxxPluginHookPoint point, const neograph::graph::NodeInput& in);

    /// 注册清单 (本挂载上的生效计数; 0 = 已回到基线)
    ///
    /// 一个插件实例能向主程序贡献的东西分散在多处 (工具注册表、权限中间件、
    /// 中间件链、图注册表、事件总线、能力表、提示词、资源应用器)。拆挂载后
    /// "到底清干净了没有"需要一份可比较的基准, 因此这里把**主程序可撤销**的注册
    /// 计数集中成一个结构。
    struct RegistrationInventory {
        size_t tools               = 0; ///< 工具注册表内的工具
        size_t permissionTools     = 0; ///< 已声明的工具权限限制
        size_t hooks               = 0; ///< 钩子处理器 (注册表里本实例生效的条数)
        size_t graphNodeTypes      = 0; ///< 自定义图节点类型 (激活中)
        size_t eventSubscriptions  = 0; ///< 事件订阅
        size_t capabilities        = 0; ///< 能力声明
        size_t promptKeys          = 0; ///< 占用中的提示词键 (prompt 贡献)
        size_t skillDirs           = 0; ///< skill 扫描目录 (资源应用器)
        size_t memoryFiles         = 0; ///< memory 上下文文件 (资源应用器)
        size_t mcpNamespaces       = 0; ///< MCP 命名空间 (资源应用器)
        size_t featurePoints       = 0; ///< 本实例声明的功能点 (生效中)
        size_t featureImpls        = 0; ///< 本实例登记的功能点实现 (生效中)
        bool   ownsGraphDefinition = false; ///< 是否占用执行图定义 (独占 slot)

        /// 主程序可撤销注册的总数 (不含 [graphNodeTypes] 的注册表残留)
        size_t total() const {
            return tools + permissionTools + hooks + graphNodeTypes + eventSubscriptions
                   + capabilities + promptKeys + skillDirs + memoryFiles + mcpNamespaces
                   + featurePoints + featureImpls + (ownsGraphDefinition ? 1 : 0);
        }
    };

    /// 统计实例在本挂载上"主程序侧生效"的注册
    RegistrationInventory registrationInventory(const PluginInstance& inst) const;

private:

    friend class PluginManager;

    /// 本挂载的 agent 上下文 (空 = agent 已释放, 只允许拆挂载)
    std::shared_ptr<agentxx::agent::AgentContext> ctx() const {
        return agent_.lock();
    }

    /// 本 agent 的工具注册表 (agent 已释放时退化为空表, 不崩)
    std::shared_ptr<ToolRegistry> toolRegistry() const;

    /// 本挂载上某实例的记录 (注册路径用: 未挂载时补一条, 保证不会空指针)
    Entry& entryOf(PluginInstance* inst);

    /// 实例此刻是否允许登记 (存在 / 启用 / 未进入关闭流程)
    static bool acceptsRegistration(const PluginInstance* inst);

    /// 资源声明是否已冻结 (未挂在本挂载上的实例视为冻结, 拒绝登记)
    bool resourcesFrozen(const PluginInstance* inst) const;

    /// agent 加载的权限中间件 (未加载返回 nullptr)
    agentxx::middleware::PermissionMiddlewareHandle* permissionMiddleware();

    /// ==================== 钩子注册表 ====================
    ///
    /// 一个钩子点上的处理器按 `(层, priority 升序, 登记序号)` 排序执行:
    /// - 层: `plugin` (插件 / FFI 主程序登记的) 在前, `core` (库自己登记的) 在后;
    /// - 不声明 `priority` (= 0) 时顺序就是登记顺序 = 插件挂载顺序。
    struct HookHandlerEntry {
        int64_t                       handle    = 0;
        int32_t                       point     = 0;
        int32_t                       priority  = 0;
        uint64_t                      seq       = 0;
        bool                          coreLayer = false; ///< core 层 (排在 plugin 层之后)
        std::string                   owner;             ///< `plugin:<名>` / `core:<模块>`
        std::string                   ownerTag;
        std::string                   depict;
        std::string                   load; ///< `dynamic` / `builtin`
        std::weak_ptr<PluginInstance> inst; ///< plugin 层: 所属实例
        AgentxxPluginHookSpec         spec; ///< 执行体 (start / cancel / user_data)
    };

    /// 某点处理器的排序快照 (派发与清单都从它取数)
    std::vector<HookHandlerEntry> orderedHandlers(AgentxxPluginHookPoint point) const;

    /// 注册表里属于某实例的处理器条数
    size_t liveHookHandlersOf(const PluginInstance& inst) const;

    /// 注册表里属于某实例的全部处理器 (按点号、再按该点的派发顺序)
    std::vector<HookHandlerView> allHookHandlersOf(const PluginInstance& inst) const;

    /// 处理器视图 (清单 / 加载快照 / 测试共用)
    HookHandlerView viewOf(const HookHandlerEntry& entry) const;

    /// 插入一条处理器并挂上派发器 (登记入口的公共收尾)
    int64_t insertHookHandler(HookHandlerEntry entry);

    /// 从注册表移除一条 (按句柄)
    bool removeHookHandler(int64_t handle, PluginInstance* ownerInst, bool checkOwner);

    /// 摘除某个实例的全部处理器 (拆挂载时; 记录里的句柄置 0)
    void detachHookHandlers(PluginInstance* inst);

    /// 确保派发器挂在中间件链上 (首次登记时插入)
    void ensureHookDispatch();

    /// 没有处理器时摘除派发器 (轮次执行中先置 `disabled`, 轮末真正摘除)
    void retireHookDispatchIfIdle();

    /// 从中间件链摘除一个句柄
    void eraseHookDispatch(PluginHookDispatchHandle* handle);

    /// 一次派发的记录 (只在开发者模式下收集)
    struct HookPointStat {
        uint64_t    dispatches = 0; ///< 派发次数
        uint64_t    handlers   = 0; ///< 上次派发的处理器数
        uint64_t    lastMs     = 0; ///< 上次派发耗时 (毫秒)
        std::string lastOrder;      ///< 上次派发的执行顺序 (`owner#handle`, ", " 分隔)
    };

    /// 释放执行图定义 slot (占用者拆挂载时调用): 恢复基础定义并清空占用者
    void releaseGraphDefinitionSlot(PluginInstance* inst);

    /// 撤销本挂载上一个实例的全部实体 (拆挂载 / 挂载失败回滚)
    void revokeEntry(Entry& entry);

    std::weak_ptr<PluginManager>                host_;
    std::weak_ptr<agentxx::agent::AgentContext> agent_;
    std::string                                 agentId_;
    std::string                                 agentName_;
    uint64_t                                    mountSeq_ = 0;

    /// 本挂载的实例记录 (按挂载顺序; `Entry::inst` 是强引用)
    std::vector<Entry> entries_;

    /// 回收站: 拆挂载后仍需保活到本轮结束的工具对象 (执行中的调用)
    std::vector<std::shared_ptr<PluginTool>> retiredTools_;

    std::map<std::string, std::shared_ptr<GraphTypeSlot>, std::less<>> graphTypeSlots_;
    size_t                                                             runningTurns_ = 0;
    std::map<std::string, PromptKeyState, std::less<>>                 promptKeys_;
    uint64_t                                                           promptSequence_ = 0;

    /// 执行图定义独占 slot: 占用者 + 占用前的基础定义
    std::string graphDefinitionOwner_;
    std::string graphDefinitionBase_;
    bool        graphDefinitionBaseCaptured_ = false;

    std::vector<HookHandlerEntry>                        hookHandlers_;
    int64_t                                              nextHookHandle_ = 1;
    uint64_t                                             hookSeq_        = 0;
    std::shared_ptr<PluginHookDispatchHandle>            hookDispatch_;
    std::array<HookPointStat, AGENTXX_PLUGIN_HOOK_COUNT> hookStats_{};

    struct PendingMiddlewareCleanup {
        std::string                             name;
        std::weak_ptr<PluginHookDispatchHandle> handle;
    };

    std::vector<PendingMiddlewareCleanup> pendingCleanups_;
};

} // namespace plugin
} // namespace agentxx

#endif // AGENTXX_PLUGIN_PLUGIN_MOUNT_H
