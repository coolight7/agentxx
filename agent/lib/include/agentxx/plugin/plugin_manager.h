#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/builtin_plugin.h"
#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/tool_registry.h"
#include "agentxx/tools/tool.h"
#include "asio/awaitable.hpp"
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
    void*                    dlHandle = nullptr; ///< dlopen/LoadLibrary 句柄 (内置插件为空)
    /// 内置插件卸载回调 (编译进 libagentxx 的插件; dlHandle 为空时使用,
    /// 无需 dlsym 查符号)
    AgentxxPluginUnloadFn builtinUnload = nullptr;
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
        AgentxxHookFn    fn;
        void*            ud;
    };

    /// 能力注册记录 (含方法回调; enable 恢复完整能力, 而非降级为无方法哑能力)
    struct CapabilityRegistration {
        std::string               name;
        AgentxxCapabilityInvokeFn invoke = nullptr;
        void*                     ctx    = nullptr;
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

/// 插件工具: C ABI spec → XXToolBase 适配
/// - execute_async 经 offloadCancellableAsync 卸载到宿主线程池调用 C 回调
///   (取消/超时语义天然接入 toolcall 链路)
/// - 持有 PluginInstance shared_ptr: 工具执行期间插件不会被卸载
/// - 字符串字段 (name/description/parameters_json) 在构造时从 string_view
///   拷贝进成员 (spec_ 指针指向本对象成员, 生命周期与工具一致, 不依赖
///   插件侧内存存活)
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

    void setHook(AgentxxHookPoint point, AgentxxHookFn fn, void* user_data);

    void clearHook(AgentxxHookPoint point);

    // 7 钩子覆写: 转发到 C 回调 (io 线程同步, 快速返回约定)
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
        AgentxxHookFn fn  = nullptr;
        void*         ud  = nullptr;
        bool          set = false;
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
    };

    explicit PluginManager(std::weak_ptr<agentxx::agent::AgentContext> agentContext);
    ~PluginManager();

    PluginManager(const PluginManager&)            = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // ==================== 生命周期 (须 io 线程) ====================

    /// 加载原生 C++ 插件动态库 (io 线程调用; dlopen 卸载到线程池执行)
    /// - cfg: 插件配置 (yaml `plugins` 条目; 传 args 给插件, 不解析字段语义);
    ///   为 nullptr 时 args 为空对象 (测试/直连路径)
    /// - 返回插件实例; 加载失败返回 nullptr (错误记日志)
    asio::awaitable<std::shared_ptr<PluginInstance>> loadNativeAsync(
        std::string path,
        const agentxx::agent::PluginConfig* cfg = nullptr
    );

    /// 加载内置插件 (编译进 libagentxx, 无动态库文件; io 线程调用)
    /// - 仅当同名插件已内置 (agentxx_get_builtin_plugins 注册表) 时可用;
    ///   name/path/depends 语义与 loadPluginAsync 目录分支一致 (path 为
    ///   manifest 入口文件路径, 用于 get_own_info 的资源推导; depends 来自
    ///   plugin.yaml 解析)
    /// - entry 调用卸载到线程池执行 (与 loadNativeAsync 相同, 避免 io↔引擎
    ///   互等死锁); 返回插件实例; 失败返回 nullptr (错误记日志)
    asio::awaitable<std::shared_ptr<PluginInstance>> loadBuiltinAsync(
        std::string              name,
        std::string              path,
        std::vector<std::string> depends,
        std::vector<std::string> optionalDepends,
        const agentxx::agent::PluginConfig* cfg = nullptr
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
    asio::awaitable<std::shared_ptr<PluginInstance>> loadPluginAsync(
        std::string path,
        const agentxx::agent::PluginConfig* cfg = nullptr
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

    /// 轮次进入/退出计数 (BaseAgent::runConversationTurnAsync 调用)
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
    /// 注册钩子 (push 中间件到 handles, 栈式执行)
    int registerHook(PluginInstance* inst, AgentxxHookPoint point, AgentxxHookFn fn, void* ud);
    int unregisterHook(PluginInstance* inst, AgentxxHookPoint point, AgentxxHookFn fn, void* ud);
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

    /// 声明能力 (无回调; 仅标记/互查)
    int registerCapability(PluginInstance* inst, const char* capability);
    /// 注册能力并附带方法回调 (通用插件间通信; 如 JS 引擎 "interpreter.js"
    /// 提供 load/unload 方法)
    int registerCapabilityEx(
        PluginInstance*           inst,
        const char*               capability,
        AgentxxCapabilityInvokeFn invoke,
        void*                     ctx
    );
    int unregisterCapability(PluginInstance* inst, const char* capability);
    int hasCapability(const char* capability) const;

    /// 调用能力提供者的方法 (io 线程约束; 跨线程经 post 同步等待)
    /// - 提供者回调在调用方线程执行 (回调内部自行跳转引擎线程)
    /// - 返回结果 JSON (host->alloc); 失败返回 NULL 并 error_out
    char* invokeCapability(
        PluginInstance* caller,
        const char*     capability,
        const char*     method,
        const char*     args_json,
        char**          error_out
    );

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
    /// 本插件配置参数 JSON (io 线程; 未配置返回 "{}")
    /// - 直接读取实例保存的 args (加载时随配置传入, 宿主不解析字段语义)
    std::string getPluginArgsJson(PluginInstance* inst);

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
        std::string                                    name; ///< 插件名 (日志/去重用)
        std::weak_ptr<PluginMiddlewareHandle>          middleware;
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
/// - 能力可附带方法回调: 能力调用 = 通用插件间通信通道 (invoke_capability)
class CapabilityRegistry {
public:

    struct Entry {
        std::string               provider;         ///< 提供者插件名
        AgentxxCapabilityInvokeFn invoke = nullptr; ///< 方法回调 (可空)
        void*                     ctx    = nullptr; ///< 提供者私有上下文
    };

    bool registerCapability(
        std::string_view          name,
        std::string_view          providerPlugin,
        AgentxxCapabilityInvokeFn invoke = nullptr,
        void*                     ctx    = nullptr
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
