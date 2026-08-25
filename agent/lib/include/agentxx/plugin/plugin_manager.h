#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/builtin_plugin.h"
#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_common.h"
#include "agentxx/plugin/tool_registry.h"
#include "agentxx/tools/tool.h"
#include "asio/awaitable.hpp"
#include "asio/steady_timer.hpp"
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace plugin {

class PluginManager;
class CapabilityRegistry;
class PluginMiddlewareHandle;
class PluginTool;
class PluginInstance;

/// 宿主定时器状态 (vtable add_timer 登记; detachAll 统一取消; 仅 io 线程)
/// - inst 为裸指针: 生命周期由"实例析构前 handler 链必已终结"保证
///   (detachAll 取消 + unload 等 inflight 归零后才 dlclose/析构)
/// - handler 链自持有 state (shared_ptr), 取消/错误后不再重新排程 → 链终结
///   释放 state (无自引用环)
struct PluginTimer {
    PluginInstance*                     inst = nullptr;
    std::shared_ptr<asio::steady_timer> timer;
    long                                intervalMs = 0;
    void (*fn)(void* ud)                           = nullptr;
    void* ud                                       = nullptr;
    bool  cancelled                                = false;
};

/// 已加载插件实例 (宿主侧状态)
/// - **所有插件统一为 C++ 插件**: 每个插件都有 dlHandle/entry; 脚本能力是
///   插件内部实现的一部分 —— 经 manifest 依赖声明 (depends 引擎插件) +
///   能力调用 (invoke_capability 插件间通信) 把脚本代码交给 interpreter
///   引擎执行, 宿主不特判任何脚本类型 (未来 py/lua 同理, 宿主零改动)
/// - 所有注册残留 (工具/钩子/订阅/能力) 记录于此, 卸载时统一清理
/// - inflight: 在途回调计数 (工具 execute / 钩子 / 事件 handler),
///   卸载流程须等其为 0 后才能调 unload 回调并 dlclose
/// - 仅 io 线程读写 (inflight 为原子, 跨线程递增/递减)
class PluginInstance {
public:

    std::string name;
    std::string version;
    std::string description;
    std::string path; ///< 加载的库路径
    /// 插件配置参数 (yaml `plugins` 条目 args; 宿主原样保存, 经 vtable
    /// get_plugin_args 整体返回给插件, 不解析其字段语义)
    neograph::json args = neograph::json::object();
    /// 必选依赖 (插件名): 未安装则加载失败; 卸载/禁用时级联 (依赖方先于被依赖方)
    std::vector<std::string> depends;
    /// 可选依赖 (插件名): 未安装仅警告, 不影响加载
    std::vector<std::string> optionalDepends;
    /// 接口声明 (plugin.yaml `interfaces`; 加载时随 manifest 解析传入,
    /// 直连库路径为空) —— 门禁依据, 经 list()/list_plugins JSON 暴露
    PluginManifestInterfaces interfaces;
    void*                    dlHandle = nullptr; ///< dlopen/LoadLibrary 句柄 (内置插件为空)
    /// 内置插件卸载回调 (编译进 libagentxx 的插件; dlHandle 为空时使用,
    /// 无需 dlsym 查符号)
    AgentxxPluginDestroyFn builtinUnload = nullptr;
    void*                 pluginCtx     = nullptr; ///< entry 输出的插件私有上下文
    bool                  enabled = true; ///< 是否启用 (禁用: 工具摘除/钩子停用)
    bool userDisabled = false; ///< 是否被用户显式禁用 (区别于级联禁用; enable 级联不复活)
    bool unloadRequested = false; ///< 已请求卸载 (防重复)

    /// 本插件专属宿主句柄 (vtable 为宿主静态函数表, opaque 指向本实例)
    AgentxxHost host{};

    /// 在途回调计数 (原子, 跨线程)
    std::atomic<size_t> inflight{0};

    /// 钩子注册记录 (enable 重建中间件时恢复; 仅 io 线程)
    struct HookRegistration {
        AgentxxHookPoint point;
        void* (*start)(void*, AgentxxHookPoint, AgentxxPluginStringView, const AgentxxOpNotify*, char**);
        int (*poll)(void*, void*);
        void (*cancel)(void*, void*);
        void* ud;
    };

    /// 能力注册记录 (含异步方法处理器三件套; enable 恢复完整能力)
    struct CapabilityRegistration {
        std::string       name;
        AgentxxCapStartFn start  = nullptr;
        AgentxxOpPollFn   poll   = nullptr;
        AgentxxOpCancelFn cancel = nullptr;
        void*             ctx    = nullptr;
    };

    /// 提示词修改备份 (set_prompt 写入前记录, 卸载时回滚)
    /// - 插件加载期间经 vtable set_prompt 写入的提示词, 卸载时自动恢复
    ///   为加载前状态 (原值或不存在), 保证"提示词归插件"的干净剥离
    /// - 仅 io 线程读写 (与 toolPrompt 同一无锁模型)
    struct PromptBackup {
        /// 原本是否不存在 (nullopt = 原本无此字段, 回滚时删除)
        std::optional<std::string> systemPrompt;
        std::optional<std::string> systemPlanningPrompt;
        std::optional<std::string> systemSkillPrompt;
        /// 原本的 toolPrompt 条目 (name → 原值; nullopt = 原本不存在, 回滚时删除)
        std::map<std::string, std::optional<agentxx::agent::ToolPrompt>, std::less<>> toolPrompt;
        /// 已记录备份的工具名 (set_prompt 首次写入某条目前备份一次, 重复写入不覆盖备份)
        std::vector<std::string> backedUpTools;
        /// 是否已备份过 system 提示词 (首次写入前备份一次)
        bool backedUpSystem = false;
    };

    /// 注册残留 (卸载时统一清理; 仅 io 线程)
    /// - 工具/钩子/能力【注册信息】在 disable 后保留 (enable 可恢复),
    ///   实际注册状态 (registry/handles/能力表) 由 disable/detachAll 摘除
    std::vector<std::string>      toolNames;         ///< 已注册工具名
    std::vector<HookRegistration> hookRegistrations; ///< 已注册钩子记录 (含函数指针)
    std::vector<std::shared_ptr<AgentxxSubscription>> subscriptions; ///< 已订阅事件句柄
    std::vector<CapabilityRegistration> capabilityRegistrations;     ///< 已声明能力记录
    /// 宿主定时器句柄 (vtable add_timer 登记; detachAll 统一取消; 仅 io 线程)
    std::vector<std::shared_ptr<PluginTimer>> timers;
    /// 提示词修改备份 (set_prompt 写入前记录; 卸载时回滚, 见 detachAll)
    PromptBackup promptBackup;

    /// 本插件的中间件句柄 (懒创建; 挂 handles 栈)
    std::shared_ptr<PluginMiddlewareHandle> middleware = nullptr;
    /// 已注册的插件工具对象 (disable 摘除后保留, enable 重注册用)
    std::vector<std::shared_ptr<PluginTool>> tools;

    /// 插件上下文弱引用 (工具/中间件执行时保活用)
    std::weak_ptr<PluginInstance> self{};
    /// 管理器弱引用 (host vtable 回调取用)
    std::weak_ptr<PluginManager> manager{};

    explicit PluginInstance(std::string in_name) :
        name(std::move(in_name)) {}

    ~PluginInstance();

    /// 在途计数 RAII (execute/hook/event handler 入口调用)
    struct InflightGuard {
        PluginInstance* inst;

        explicit InflightGuard(PluginInstance* i) :
            inst(i) {
            if (inst) {
                inst->inflight.fetch_add(1, std::memory_order_acq_rel);
            }
        }

        ~InflightGuard() {
            if (inst) {
                inst->inflight.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
    };
};

/// 插件工具: C ABI spec → XXToolBase 适配 (统一异步操作模型)
/// - execute_async 经 op_driver awaitPluginOp 在【io 线程】驱动插件三件套
///   (start/poll/cancel), 与内置工具协程同线程交错执行; 不再卸载线程池
/// - 取消: 会话 CancelToken 联动 execute_cancel (协作式); 超时经
///   asyncWithTimeout, 超时放弃后由收割协程继续推进直至插件终结
///   (inflight 保活转移, 卸载安全语义与旧模型一致)
/// - 持有 PluginInstance shared_ptr: 工具执行期间插件不会被卸载
/// - 字符串字段 (name/description/parameters_json) 在构造时从 string_view
///   拷贝进成员 (spec_ 指针指向本对象成员, 生命周期与工具一致)
class PluginTool : public agentxx::tools::XXToolBase {
public:

    /// name 取自 spec.name (构造时拷贝进成员), 不再单独传参避免双来源不一致
    PluginTool(
        std::weak_ptr<agentxx::agent::AgentContext> agentContext,
        std::shared_ptr<PluginInstance>             instance,
        AgentxxToolSpec                             spec
    );

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;

    /// 从保存的 spec 重建 (插件 enable 时重新注册)
    std::shared_ptr<PluginInstance> instance() const {
        return instance_.lock();
    }

    /// 原始 C ABI spec (call_tool 同步互调用; 字符串指针指向本对象成员)
    const AgentxxToolSpec& spec() const {
        return spec_;
    }

private:

    std::string     name_;           ///< 拷贝的 name (稳定地址)
    std::string     description_;    ///< 拷贝的 description (稳定地址)
    std::string     parametersJson_; ///< 拷贝的 parameters_json (稳定地址)
    AgentxxToolSpec spec_;           ///< 拷贝的 spec (字符串指针指向上面成员)
    neograph::json  parameters_;     ///< 解析缓存的参数 schema (避免每轮重复 parse)
    std::weak_ptr<PluginInstance> instance_;
};

/// 插件中间件句柄: 7 个 C 钩子 → 现有 handles 栈式执行
/// - 注册进 middlewareHandleContext->handles; 禁用/卸载时置 disabled 位
///   (WrapHandleBaseNode 遍历跳过), 由 PluginManager 在轮末安全摘除
class PluginMiddlewareHandle
    : public agentxx::middleware::BaseMiddlewareHandle<agentxx::middleware::BaseMiddlewareState> {
public:

    PluginMiddlewareHandle(
        std::string_view                            name,
        std::weak_ptr<agentxx::agent::AgentContext> agentContext,
        std::shared_ptr<PluginInstance>             instance
    );

    /// 设置钩子 (异步三件套规格拷贝存储; 每插件每钩子点至多一个)
    void setHook(const AgentxxHookSpec& spec);

    void clearHook(AgentxxHookPoint point);

    // 7 钩子覆写: 经 op_driver 在 io 线程驱动钩子三件套 (与工具同模型)
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
        void* (*start)(void*, AgentxxHookPoint, AgentxxPluginStringView, const AgentxxOpNotify*, char**)
            = nullptr;
        int (*poll)(void*, void*)      = nullptr;
        void (*cancel)(void*, void*)   = nullptr;
        void*         ud               = nullptr;
        bool          set              = false;
    };

    asio::awaitable<void> dispatch(AgentxxHookPoint point, const neograph::graph::NodeInput& in);

    /// 插件实例弱引用: 与 instance->middleware 互不持有, 消除循环引用
    /// (中间件被摘除/实例析构时, 弱引用自动失效; dispatch 处 lock 保活)
    std::weak_ptr<PluginInstance>             instance_;
    std::array<HookEntry, AGENTXX_HOOK_COUNT> hooks_{};
};

/// 插件管理器 (全局唯一, 挂 AgentContext)
/// - 生命周期: load → (tools/hooks 生效) → disable/enable → unload
/// - 所有写操作须在 io 线程 (注册修改 handles/registry, 满足 assertIoThread
///   无锁模型); 非 io 线程调用请自行 post
/// - 卸载顺序: 摘除全部注册 → 置中间件 disabled → 等 inflight==0 →
///   调 unload 回调 → dlclose
/// - 中间件真正从 handles 摘除延迟到轮次边界 (flushPendingCleanup),
///   避免执行中下标错位
class PluginManager : public std::enable_shared_from_this<PluginManager> {
public:

    struct PluginListView {
        std::string              name;
        std::string              version;
        std::string              description;
        std::string              path;
        bool                     enabled  = true;
        size_t                   inflight = 0;
        std::vector<std::string> tools;
        std::vector<std::string> capabilities;
        std::vector<std::string> depends;
        std::vector<std::string> optionalDepends;
        /// 接口声明 (plugin.yaml `interfaces`; 空 = 未声明)
        std::vector<std::string> requiredInterfaces;
        std::vector<std::string> optionalInterfaces;
    };

    explicit PluginManager(std::weak_ptr<agentxx::agent::AgentContext> agentContext);
    ~PluginManager();

    PluginManager(const PluginManager&)            = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // ==================== 生命周期 (须 io 线程) ====================

    /// 加载原生 C++ 插件动态库 (io 线程调用; dlopen 卸载到线程池执行)
    /// - cfg: 插件配置 (yaml `plugins` 条目; 传 args 给插件, 不解析字段语义);
    ///   为 nullptr 时 args 为空对象 (测试/直连路径)
    /// - allowClientOnlySkip: sides==Auto 时无 agent 入口视为纯 client 插件,
    ///   跳过并警告 (而非报错); 显式 sides==agent 的加载保持错误。
    ///   例外: manifest interfaces.require 声明了 agent 侧接口 → 缺入口为
    ///   明确错误 (声明意图优先于 Auto 容忍, 见接口协商设计)
    /// - resources: 插件清单资源声明 (skill/memory/mcp; 目录插件经
    ///   parsePluginManifest 解析后传入) —— entry 成功后经 resourceApplier
    ///   应用; 加载失败不应用 ("失败不生效")
    /// - interfaces: 插件清单接口声明 (require/optional; 见
    ///   PluginManifestInterfaces) —— require 未满足时跳过加载
    /// - 返回插件实例; 加载失败返回 nullptr (错误记日志)
    asio::awaitable<std::shared_ptr<PluginInstance>> loadNativeAsync(
        std::string                                path,
        const agentxx::agent::PluginConfig*        cfg                 = nullptr,
        bool                                       allowClientOnlySkip = false,
        const plugin::PluginManifestResources&     resources           = {},
        const plugin::PluginManifestInterfaces&    interfaces          = {}
    );

    /// 加载内置插件 (编译进 libagentxx, 无动态库文件; io 线程调用)
    /// - 仅当同名插件已内置 (agentxx_get_builtin_plugins 注册表) 时可用;
    ///   name/path/depends 语义与 loadPluginAsync 目录分支一致 (path 为
    ///   manifest 入口文件路径, 用于 get_own_info 的资源推导; depends 来自
    ///   plugin.yaml 解析)
    /// - entry 调用卸载到线程池执行 (与 loadNativeAsync 相同, 避免 io↔引擎
    ///   互等死锁); 返回插件实例; 失败返回 nullptr (错误记日志)
    /// - resources/interfaces: 插件清单声明 (语义同 loadNativeAsync)
    asio::awaitable<std::shared_ptr<PluginInstance>> loadBuiltinAsync(
        std::string                            name,
        std::string                            path,
        std::vector<std::string>               depends,
        std::vector<std::string>               optionalDepends,
        const agentxx::agent::PluginConfig*    cfg        = nullptr,
        const plugin::PluginManifestResources& resources  = {},
        const plugin::PluginManifestInterfaces& interfaces = {}
    );

    /// 卸载插件 (按名称; 等全部在途回调完成后才 dlclose)
    asio::awaitable<bool> unloadAsync(std::string_view name);

    /// 禁用插件 (工具摘除/钩子停用; 轮次边界生效)
    /// - 若当前无轮次执行则立即生效; 否则标记 pending, 轮末 flushPendingCleanup 生效
    /// - 级联: 必选依赖本插件的插件一同禁用 (依赖者先禁用)
    /// - 被级联禁用的插件不置 userDisabled (用户显式 enable 依赖方时可级联恢复)
    void disable(std::string_view name);

    /// 启用插件 (重新注册保存的工具/钩子/能力)
    /// - 级联: 必选依赖本插件的插件一同启用 (被级联禁用且未被用户显式禁用的)
    void enable(std::string_view name);

    /// 轮末清理: 摘除 pending 的中间件 (io 线程, 无执行中, 安全 erase)
    void flushPendingCleanup();

    /// 加载配置中启用的插件 (BaseAgent::init 装配后调用)
    /// - 原生插件: 直接库路径
    /// - 插件目录 (含 plugin.yaml): 按清单分派 (type: native/js)
    /// - 按 manifest depends 拓扑排序加载 (依赖者排在被依赖者之后),
    ///   避免配置顺序导致依赖缺失加载失败
    asio::awaitable<void>
        loadConfiguredPlugins(const std::vector<agentxx::agent::PluginConfig>& plugins);

    /// 加载插件 (支持 库路径 或 插件目录; 目录含 plugin.yaml 时按清单分派)
    /// - **所有插件统一为 C++ 插件**: manifest entry 总是指向动态库,
    ///   dlopen 后调 entry 函数; 脚本能力由插件内部经能力调用委派给
    ///   interpreter 引擎 (宿主不参与)
    /// - 依赖检查: 必选依赖未安装 → 加载失败; 可选依赖未安装 → 警告;
    ///   依赖环 → 拒绝
    /// - cfg: 插件配置 (yaml `plugins` 条目; args 随加载直接传给插件实例,
    ///   不再事后按名回查配置 —— manifest name 与目录/文件名不一致时也能
    ///   正确拿到 args); 为 nullptr 时 args 为空对象
    /// - allowClientOnlySkip: sides==Auto 时无 agent 入口视为纯 client 插件,
    ///   跳过并警告 (与 client 侧 Auto 无 client 入口静默跳过对称);
    ///   显式 sides==agent 的加载缺失入口仍为错误
    asio::awaitable<std::shared_ptr<PluginInstance>> loadPluginAsync(
        std::string                         path,
        const agentxx::agent::PluginConfig* cfg                 = nullptr,
        bool                                allowClientOnlySkip = false
    );

    /// 同步卸载全部插件 (AgentContext 析构前调用, 断开中间件↔实例循环引用)
    /// - 按依赖图逆序 (先子后父): 脚本插件先卸载, 引擎插件最后 (脚本插件
    ///   unload 回调需经 invoke_capability 通知引擎, 引擎必须存活到最后)
    /// - 每个插件: detachAll → unload 回调 → dlclose; unload 回调用于业务清理
    ///   (如引擎插件 join 内部线程, 保证 dlclose 前代码段无执行者)
    /// - 不等在途回调: 调用方须保证无在途插件回调 (进程正常退出/上下文销毁
    ///   路径满足); 否则 unload 回调可能等待在途回调完成而阻塞
    void shutdownAll();

    // ==================== 查询 ====================

    std::vector<PluginListView>     list() const;
    std::shared_ptr<PluginInstance> find(std::string_view name) const;

    /// 当前是否有轮次在图引擎执行 (disable 立即/延迟生效判定)
    bool hasRunningTurn() const {
        return runningTurns_ > 0;
    }

    /// 轮次进入/退出计数 (BaseAgent::runTurnAsync 调用)
    void onTurnBegin() {
        ++runningTurns_;
    }

    void onTurnEnd() {
        if (runningTurns_ > 0) {
            --runningTurns_;
        }
    }

    // ==================== 内部 (host vtable 回调) ====================

    /// 注册工具 (plugin_api.h register_tool 实现入口)
    int registerTool(PluginInstance* inst, const AgentxxToolSpec* spec);
    /// 注销工具
    int unregisterTool(PluginInstance* inst, const char* name);

    // ==================== 会话资源注册 (v8: Skill/Memory/MCP; io 线程) ====================
    /// 以下均转发到 AgentContext::resourceApplier (未装配时返回非 0/空串,
    /// 如 BaseAgent 场景); 所有权按 inst->name 记录于 applier —— 卸载时
    /// detachAll 统一摘除, 禁用/启用时摘除/恢复 (与工具行为一致)。
    /// 冲突规则: 主程序 yaml 配置优先, 插件之间先到先得。
    int registerSkillDir(PluginInstance* inst, const char* path);
    int unregisterSkillDir(PluginInstance* inst, const char* path);
    int registerMemoryFile(PluginInstance* inst, const char* path);
    int unregisterMemoryFile(PluginInstance* inst, const char* path);
    /// spec_json: {"namespace":"...","url":"...","timeout":60(秒,可选)}
    /// → McpServerConfig; 异步连接, 查重通过即返回 0
    int registerMcpServer(PluginInstance* inst, const char* specJson);
    int unregisterMcpServer(PluginInstance* inst, const char* nameSpace);
    /// 本插件资源快照 JSON {"skills":[],"memory":[],"mcp":[]}; 未装配返回空串
    std::string ownResourcesJson(const PluginInstance* inst);

    /// 注册钩子 (异步三件套规格拷贝; push 中间件到 handles, 栈式执行)
    int registerHook(PluginInstance* inst, const AgentxxHookSpec* spec);
    /// 注销钩子 (按 point 匹配 —— 每插件每钩子点至多一个)
    int unregisterHook(PluginInstance* inst, AgentxxHookPoint point);
    /// 事件订阅/发布 (topic 加 "plugin." 前缀, 载荷 JSON 字符串)
    AgentxxSubscription* subscribe(
        PluginInstance* inst,
        const char*     topic,
        void (*handler)(AgentxxPluginStringView event_json, void* ud),
        void* ud
    );
    void unsubscribe(AgentxxSubscription* sub);
    int  publish(const char* topic, const char* event_json);
    /// 读取会话级 share_store 条目 (io 线程)
    char* getShareStore(PluginInstance* inst, const char* thread_id, long long id);
    void  emitMessageTip(PluginInstance* inst, const char* thread_id, const char* text, int level);

    /// 创建周期定时器 (vtable add_timer 实现入口; io 线程)
    /// - 返回句柄 (PluginTimer shared_ptr 裸指针); 失败返回 nullptr
    /// - 回调循环内自行重新 expires_after; cancelTimer 置 cancelled + cancel
    void* addTimer(PluginInstance* inst, long intervalMs, void (*fn)(void* ud), void* ud);
    /// 取消定时器 (io 线程; 从实例容器移除 + cancelled 标记)
    void cancelTimer(PluginInstance* inst, void* timer);
    /// 阻塞池卸载执行 (vtable offload 实现入口; 任意线程可调用)
    /// - work 在 AgentContext::threadPool 线程执行, done 投递回 io 线程
    /// - cancel_flag: 调用方持有 (done 返回前须保持有效); 宿主不主动置位
    /// - 全程 inflight 计数保活插件代码段 (work/done 执行期间可安全卸载等待)
    void offload(
        PluginInstance* inst,
        volatile int*   cancel_flag,
        void* (*work)(void* ud, volatile int* cancel_flag, char** error_out),
        void (*done)(void* ud, void* result, char* error),
        void* ud
    );

    // ==================== 插件互调 / 能力调用 (异步原语) ====================
    // 目标插件三件套由宿主在 io 线程自动驱动; 调用方任意线程经 AgentxxHostOp
    // 轮询结果。阻塞便捷版内部轮询实现, 禁止 io 线程调用 (fail-fast)。

    /// 异步调用插件工具 (call_tool_async vtable 实现; 任意线程可调用)
    /// - 失败返回 NULL 并 error_out (工具不存在/非插件工具/插件禁用等)
    AgentxxHostOp* callToolAsync(
        PluginInstance* caller,
        const char*     name,
        const char*     args_json,
        const char*     thread_id,
        char**          error_out
    );
    /// 阻塞便捷版 call_tool (内部轮询 callToolAsync); io 线程调用 fail-fast
    char* callToolBlocking(
        PluginInstance* caller,
        const char*     name,
        const char*     args_json,
        const char*     thread_id,
        char**          error_out
    );
    /// 异步调用能力方法 (invoke_capability_async vtable 实现; 任意线程可调用)
    AgentxxHostOp* invokeCapabilityAsync(
        PluginInstance* caller,
        const char*     capability,
        const char*     method,
        const char*     args_json,
        char**          error_out
    );
    /// 阻塞便捷版 invoke_capability (内部轮询 invokeCapabilityAsync);
    /// io 线程调用 fail-fast
    char* invokeCapability(
        PluginInstance* caller,
        const char*     capability,
        const char*     method,
        const char*     args_json,
        char**          error_out
    );

    /// 工具注册表 (供 ToolcallWrapNode/ModelCallWrapNode 查表)
    std::shared_ptr<ToolRegistry> registry() const {
        return registry_;
    }

    /// 能力注册表
    std::shared_ptr<CapabilityRegistry> capabilities() const {
        return capabilities_;
    }

    // ==================== io 线程投递 (vtable 跨线程调用支撑) ====================

    /// 装配 io executor (BaseAgent::init 调用; 未装配时 vtable 的跨线程
    /// 调用按 io 线程直接执行处理)
    void setIoExecutor(asio::any_io_executor ex) {
        ioExecutor_ = std::move(ex);
        if (ioExecutor_) {
            ioThreadId_ = std::this_thread::get_id(); // init 在 io 线程执行
        }
    }

    /// 当前线程是否为 io 线程
    bool isIoThread() const {
        return !ioExecutor_ || ioThreadId_ == std::this_thread::get_id();
    }

    /// 投递任务到 io 线程异步执行 (线程安全; 非阻塞)
    void postToIo(std::function<void()> fn) const {
        if (isIoThread()) {
            fn();
        } else if (ioExecutor_) {
            asio::post(ioExecutor_, std::move(fn));
        } else {
            // 理论不可达 (ioExecutor_ 为空时 isIoThread() 恒 true); 防御兜底
            XX_LOGW("PluginManager::postToIo: no io executor, executing on caller thread");
            fn();
        }
    }

    // ==================== 能力注册/调用 (插件间通信) ====================

    /// 声明能力 (无方法处理器; 仅标记/互查)
    int registerCapability(PluginInstance* inst, const char* capability);
    /// 注册能力并附带异步方法处理器三件套 (通用插件间通信; 如 JS 引擎
    /// "interpreter.js" 提供 load/unload 方法)
    int registerCapabilityEx(
        PluginInstance*           inst,
        const char*               capability,
        AgentxxCapStartFn         start,
        AgentxxOpPollFn           poll,
        AgentxxOpCancelFn         cancel,
        void*                     ctx
    );
    int unregisterCapability(PluginInstance* inst, const char* capability);
    int hasCapability(const char* capability) const;

    // ==================== 插件互查 (vtable list_plugins/get_plugin) ====================

    /// 全部插件信息 JSON 数组 (std::string; io 线程)
    std::string listPluginsJson();
    /// 单个插件信息 JSON (未安装返回空串; io 线程)
    std::string getPluginJson(const std::string& name);

    // ==================== 宿主配置访问 (vtable get_config/get_tool_prompt) ====================

    /// 宿主 AgentConfig 关键字段 JSON (io 线程; 供插件装配期读取)
    /// {"dataDir","projectRoot","platform"}
    /// - 通用宿主信息; 插件业务参数经 get_plugin_args (宿主不解析 args)
    /// - agentContext/agentConfig 未装配时返回空串
    std::string getConfigJson();
    /// 宿主 toolPrompt 配置 JSON (io 线程):
    /// {"depict": "...", "args": {"参数名": "说明"}}
    /// - 工具未配置 prompt 时返回空串 (插件回退内置默认描述)
    std::string getToolPromptJson(const std::string& toolName);
    /// 宿主完整提示词 JSON (io 线程; 未装配 AgentConfig 返回空串):
    /// {"systemPrompt","systemPlanningPrompt","systemSkillPrompt","toolPrompt"}
    /// - vtable get_prompt 实现入口
    std::string getPromptJson();
    /// 合并更新宿主提示词 (io 线程; vtable set_prompt 实现入口)
    /// - 仅覆盖 JSON 中出现的字段 (与 AgentPrompt::mergeFromJson 语义一致)
    /// - 写入前记录备份到 inst->promptBackup (首次写入某条目前备份原值),
    ///   插件卸载时由 detachAll 回滚 (恢复加载前状态)
    /// - 返回 0 成功; JSON 非法/宿主未就绪返回非 0
    int setPromptJson(PluginInstance* inst, const char* prompt_json);
    /// 回滚插件加载期间写入的提示词 (恢复加载前状态; 卸载路径调用)
    /// - 仅删除/恢复该插件写入过的字段, 不影响其他提示词内容
    void restorePromptBackup(PluginInstance* inst);
    /// 应用插件清单声明的会话资源 (skill/memory/mcp; entry 成功后调用)
    /// - 经 AgentContext::resourceApplier 分发; 未装配时警告跳过
    /// - 加载失败路径不会调用本函数 → "失败不生效"由调用时序保证
    void applyDeclaredResources(
        PluginInstance&                        inst,
        const plugin::PluginManifestResources& resources
    );
    /// 本插件配置参数 JSON (io 线程; 未配置返回 "{}")
    /// - 直接读取实例保存的 args (加载时随配置传入, 宿主不解析字段语义)
    std::string getPluginArgsJson(PluginInstance* inst);

    // ==================== 会话状态访问 (vtable 新增接口表实现入口) ====================
    /// 以下方法均须在 io 线程调用 (C ABI 回调经 ioCallSync 投递), 内部直接
    /// 访问 agentContext_ (私有) 并转发到 AgentContext/中间件

    /// 解析后的会话工作目录 (AgentConfig::resolvedWorkDir; 未装配返回空串)
    /// - vtable agentxx.agent.config v2 get_work_dir 实现入口
    std::string getSessionWorkDir();
    /// 指定会话生效的工作目录 (worktree 绑定优先, 回退 getSessionWorkDir;
    /// 未装配/会话不存在返回空串)
    /// - vtable agentxx.agent.config v3 get_session_work_dir 实现入口
    std::string getSessionWorkDir(const std::string& threadId);
    /// 宿主主模型及关联配置 JSON (未装配返回空串):
    /// {"baseUrl","apiKey","modelName","websearchApiUrl",
    ///  "websearchConvertHtml2markdown","websearchModel","ragDocsPaths"}
    /// - vtable agentxx.agent.model get_config 实现入口
    ///   (apiKey 透出仅限本项目内置插件使用, 见 plugin_api.h 注释)
    std::string getModelConfigJson();
    /// 查询会话当前轮次是否已取消 (会话不存在/无取消令牌返回 false)
    /// - vtable agentxx.agent.cancel is_cancelled 实现入口
    bool isSessionCancelled(const std::string& threadId);
    /// 写入指定会话的两层规划 + 备忘录 (PlanningMiddlewareHandle state)
    /// - todosJson 为 todo 数组 JSON 文本 (空串跳过); notes 空串跳过
    /// - 返回 0 成功; 未装配 PlanningMiddleware/todos 非法 JSON 返回非 0
    /// - vtable agentxx.agent.planning set_planning 实现入口
    int setSessionPlanning(
        const std::string& threadId,
        const std::string& roadmap,
        const std::string& todosJson,
        const std::string& notes
    );

private:

    friend class PluginInstance;

    /// 等待插件在途计数归零 (io 线程协程轮询); 超时返回 false
    /// - 超时说明有插件回调长时间未返回 (慢/恶意插件): 调用方放弃卸载
    ///   (插件保持已 detach 状态, 可稍后重试), 避免无限阻塞 io 线程
    asio::awaitable<bool> waitInflightZero(
        const std::shared_ptr<PluginInstance>& inst,
        std::chrono::milliseconds              timeout
    );

    /// 插件卸载清理: 摘除注册/退订/置 disabled (io 线程)
    /// - 保留注册信息 (工具对象/hook 记录/能力记录), disable 后 enable 可恢复;
    ///   仅插件实例析构 (unload/进程销毁) 时随实例释放
    void detachAll(PluginInstance* inst);

    /// 从 handles 摘除中间件 (仅 flushPendingCleanup/unload/shutdown/加载失败清理调用)
    /// - 按中间件指针摘除, 不依赖实例存活 (加载失败路径实例已移除时仍可清理)
    void eraseMiddleware(PluginMiddlewareHandle* mw);

    /// 待轮末摘除的中间件 (弱引用: flush 时不依赖实例存活 —— 加载失败
    /// 路径实例已从插件表移除, 仅中间件持实例弱引用, 摘除后实例自然析构)
    struct PendingMiddlewareCleanup {
        std::string                           name; ///< 插件名 (日志/去重用)
        std::weak_ptr<PluginMiddlewareHandle> middleware;
    };

    /// 登记待轮末清理的中间件 (按 name 去重)
    void addPendingCleanup(const PluginInstance* inst);

    /// 禁用/启用内部实现 (级联递归用; userInitiated=false 表示级联, 不改 userDisabled)
    void disableImpl(std::string_view name, bool userInitiated);
    void enableImpl(std::string_view name, bool userInitiated);

    /// 卸载单个插件 (shutdownAll 用; 先递归卸载必选依赖者, 再处理自己)
    void shutdownPlugin(const std::shared_ptr<PluginInstance>& inst);

    /// 依赖检查: 必选依赖是否全部已安装; 可选缺失仅警告 (返回 false = 加载失败)
    /// - 顺带做循环依赖检测 (DFS 访问链)
    bool checkDependencies(
        const std::string&              name,
        const std::vector<std::string>& depends,
        const std::vector<std::string>& optionalDepends
    );

    /// 递归检测依赖环 (visiting 为当前 DFS 访问链)
    bool hasDependencyCycle(const std::string& name, std::vector<std::string>& visiting) const;

    std::weak_ptr<agentxx::agent::AgentContext> agentContext_;
    std::shared_ptr<ToolRegistry>               registry_;
    std::shared_ptr<CapabilityRegistry>         capabilities_;
    /// 插件表 <name, instance>
    std::map<std::string, std::shared_ptr<PluginInstance>, std::less<>> plugins_{};

    /// 进行中轮次计数 (io 线程)
    size_t runningTurns_ = 0;
    /// 待轮末生效的禁用/卸载列表 (io 线程)
    std::vector<PendingMiddlewareCleanup> pendingCleanup_{};

    /// io executor (BaseAgent::init 装配; 空 = 未装配)
    asio::any_io_executor ioExecutor_{};
    std::thread::id       ioThreadId_{};
};

/// 能力注册表 (插件互查/委派; 如 JS 解释器能力 "interpreter.js")
/// - 能力可附带异步方法处理器三件套: 能力调用 = 通用插件间通信通道
///   (invoke_capability / invoke_capability_async)
class CapabilityRegistry {
public:

    struct Entry {
        std::string       provider;                 ///< 提供者插件名
        AgentxxCapStartFn start  = nullptr;         ///< 方法启动 (可空 = 无方法)
        AgentxxOpPollFn   poll   = nullptr;         ///< 推进 (可空)
        AgentxxOpCancelFn cancel = nullptr;         ///< 取消 (可空)
        void*             ctx    = nullptr;         ///< 提供者私有上下文
    };

    bool registerCapability(
        std::string_view  name,
        std::string_view  providerPlugin,
        AgentxxCapStartFn start  = nullptr,
        AgentxxOpPollFn   poll   = nullptr,
        AgentxxOpCancelFn cancel = nullptr,
        void*             ctx    = nullptr
    );
    bool unregisterCapability(std::string_view name, std::string_view providerPlugin);
    bool has(std::string_view name) const;
    /// 能力条目 (不存在返回 nullptr)
    const Entry* get(std::string_view name) const;
    /// 提供某能力的插件名 (不存在返回空)
    std::string              providerOf(std::string_view name) const;
    std::vector<std::string> names() const;

private:

    std::map<std::string, Entry, std::less<>> caps_{}; ///< <capability, Entry>
};

/// 平台动态库加载封装 (dlopen ↔ LoadLibrary)
class NativeLoader {
public:

    /// 打开动态库; 失败返回 nullptr 并填充 err
    static void* open(const std::string& path, std::string& err);
    /// 查找符号; 失败返回 nullptr 并填充 err
    static void* sym(void* handle, const char* name, std::string& err);
    /// 关闭动态库
    static void close(void* handle);
    /// 附加目录搜索路径 (加载前调用; 库相对路径解析用)
    static void addSearchPath(std::string_view dir);
};

} // namespace plugin
} // namespace agentxx
