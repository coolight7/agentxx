#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/tool_registry.h"
#include "agentxx/tools/tool.h"
#include "asio/awaitable.hpp"
#include <array>
#include <atomic>
#include <map>
#include <memory>
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
/// - 所有注册残留 (工具/钩子/订阅/能力) 记录于此, 卸载时统一清理
/// - inflight: 在途回调计数 (工具 execute / 钩子 / 事件 handler),
///   卸载流程须等其为 0 后才能调 unload 回调并 dlclose
/// - 仅 io 线程读写 (inflight 为原子, 跨线程递增/递减)
class PluginInstance {
public:

    std::string name;
    std::string version;
    std::string description;
    std::string path;                    ///< 加载的库路径
    void*       dlHandle = nullptr;      ///< dlopen/LoadLibrary 句柄
    void*       pluginCtx = nullptr;     ///< entry 输出的插件私有上下文
    bool        enabled = true;          ///< 是否启用 (禁用: 工具摘除/钩子停用)
    bool        unloadRequested = false; ///< 已请求卸载 (防重复)

    /// 本插件专属宿主句柄 (vtable 为宿主静态函数表, opaque 指向本实例)
    AgentxxHost host{};

    /// 在途回调计数 (原子, 跨线程)
    std::atomic<size_t> inflight{0};

    /// 注册残留 (卸载时统一清理; 仅 io 线程)
    std::vector<std::string>           toolNames;      ///< 已注册工具名
    std::vector<AgentxxHookPoint>      hookPoints;     ///< 已注册钩子点
    std::vector<AgentxxSubscription*>  subscriptions;  ///< 已订阅事件句柄
    std::vector<std::string>           capabilities;   ///< 已声明能力

    /// 本插件的中间件句柄 (懒创建; 挂 handles 栈)
    std::shared_ptr<PluginMiddlewareHandle> middleware = nullptr;
    /// 已注册的插件工具对象 (disable 摘除后保留, enable 重注册用)
    std::vector<std::shared_ptr<PluginTool>> tools;

    /// 插件上下文弱引用 (工具/中间件执行时保活用)
    std::weak_ptr<PluginInstance> self{};
    /// 管理器弱引用 (host vtable 回调取用)
    std::weak_ptr<PluginManager> manager{};

    explicit PluginInstance(std::string in_name) : name(std::move(in_name)) {}

    ~PluginInstance();

    /// 在途计数 RAII (execute/hook/event handler 入口调用)
    struct InflightGuard {
        PluginInstance* inst;
        explicit InflightGuard(PluginInstance* i) : inst(i) {
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
class PluginTool : public agentxx::tools::XXToolBase {
public:

    PluginTool(
        std::string_view                          name,
        std::weak_ptr<agentxx::agent::AgentContext> agentContext,
        std::shared_ptr<PluginInstance>           instance,
        AgentxxToolSpec                           spec
    );

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;

    /// 从保存的 spec 重建 (插件 enable 时重新注册)
    std::shared_ptr<PluginInstance> instance() const {
        return instance_.lock();
    }

    /// 原始 C ABI spec (call_tool 同步互调用)
    const AgentxxToolSpec& spec() const {
        return spec_;
    }

private:

    AgentxxToolSpec                   spec_;      ///< 拷贝的 spec (含函数指针)
    std::weak_ptr<PluginInstance>     instance_;
};

/// 插件中间件句柄: 7 个 C 钩子 → 现有 handles 栈式执行
/// - 注册进 middlewareHandleContext->handles; 禁用/卸载时置 disabled 位
///   (WrapHandleBaseNode 遍历跳过), 由 PluginManager 在轮末安全摘除
class PluginMiddlewareHandle : public agentxx::middleware::BaseMiddlewareHandle<
                                   agentxx::middleware::BaseMiddlewareState> {
public:

    PluginMiddlewareHandle(
        std::string_view                 name,
        std::weak_ptr<agentxx::agent::AgentContext> agentContext,
        std::shared_ptr<PluginInstance>  instance
    );

    void setHook(AgentxxHookPoint point, AgentxxHookFn fn, void* user_data);

    void clearHook(AgentxxHookPoint point);

    // 7 钩子覆写: 转发到 C 回调 (io 线程同步, 快速返回约定)
    asio::awaitable<void> onAgentcallStartFunc(neograph::graph::NodeInput& in) override;
    asio::awaitable<void> onAgentcallEndFunc(
        const neograph::graph::NodeInput& in, neograph::graph::NodeOutput& result
    ) override;
    asio::awaitable<void> onModelcallStartFunc(neograph::graph::NodeInput& in) override;
    asio::awaitable<void> onModelcallRunFunc(neograph::graph::NodeInput& in) override;
    asio::awaitable<void> onModelcallEndFunc(
        const neograph::graph::NodeInput& in, neograph::graph::NodeOutput& result
    ) override;
    asio::awaitable<void> onToolcallStartFunc(neograph::graph::NodeInput& in) override;
    asio::awaitable<void> onToolcallEndFunc(
        const neograph::graph::NodeInput& in, neograph::graph::NodeOutput& result
    ) override;

private:

    struct HookEntry {
        AgentxxHookFn fn  = nullptr;
        void*         ud  = nullptr;
        bool          set = false;
    };

    asio::awaitable<void> dispatch(AgentxxHookPoint point, const neograph::graph::NodeInput& in);

    std::shared_ptr<PluginInstance> instance_;
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
        bool                     enabled = true;
        size_t                   inflight = 0;
        std::vector<std::string> tools;
        std::vector<std::string> capabilities;
    };

    explicit PluginManager(std::weak_ptr<agentxx::agent::AgentContext> agentContext);
    ~PluginManager();

    PluginManager(const PluginManager&)            = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // ==================== 生命周期 (须 io 线程) ====================

    /// 加载原生 C++ 插件动态库 (io 线程调用; dlopen 卸载到线程池执行)
    /// - 返回插件实例; 加载失败返回 nullptr (错误记日志)
    asio::awaitable<std::shared_ptr<PluginInstance>> loadNativeAsync(std::string path);

    /// 卸载插件 (按名称; 等全部在途回调完成后才 dlclose)
    asio::awaitable<bool> unloadAsync(std::string_view name);

    /// 禁用插件 (工具摘除/钩子停用; 轮次边界生效)
    /// - 若当前无轮次执行则立即生效; 否则标记 pending, 轮末 flushPendingCleanup 生效
    void disable(std::string_view name);

    /// 启用插件 (重新注册保存的工具/钩子)
    void enable(std::string_view name);

    /// 轮末清理: 摘除 pending 的中间件 (io 线程, 无执行中, 安全 erase)
    void flushPendingCleanup();

    /// 加载配置中启用的插件 (BaseAgent::init 装配后调用)
    asio::awaitable<void> loadConfiguredPlugins(
        const std::vector<agentxx::agent::PluginConfig>& plugins
    );

    /// 同步卸载全部插件 (AgentContext 析构前调用, 断开中间件↔实例循环引用;
    /// 不等在途回调, 适用于进程/上下文销毁场景)
    void shutdownAll();

    // ==================== 查询 ====================

    std::vector<PluginListView> list() const;
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
        PluginInstance* inst, const char* topic,
        void (*handler)(const char* event_json, void* ud), void* ud
    );
    void unsubscribe(AgentxxSubscription* sub);
    int  publish(const char* topic, const char* event_json);
    /// 能力注册
    int registerCapability(PluginInstance* inst, const char* capability);
    int unregisterCapability(PluginInstance* inst, const char* capability);
    int hasCapability(const char* capability) const;
    /// 插件工具互调 (线程安全, 内部独立线程池执行)
    char* callTool(PluginInstance* inst, const char* name, const char* args_json,
                   const char* thread_id, char** error_out);
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

private:

    friend class PluginInstance;

    /// 等待插件在途计数归零 (io 线程协程轮询)
    asio::awaitable<void> waitInflightZero(const std::shared_ptr<PluginInstance>& inst);

    /// 插件卸载清理: 摘除注册/退订/置 disabled (io 线程)
    void detachAll(PluginInstance* inst);

    /// 从 handles 摘除某插件的全部中间件 (仅 flushPendingCleanup 调用)
    void eraseMiddleware(const PluginInstance* inst);

    std::weak_ptr<agentxx::agent::AgentContext> agentContext_;
    std::shared_ptr<ToolRegistry>               registry_;
    std::shared_ptr<CapabilityRegistry>         capabilities_;
    /// 插件表 <name, instance>
    std::map<std::string, std::shared_ptr<PluginInstance>, std::less<>> plugins_{};
    /// 进行中轮次计数 (io 线程)
    size_t runningTurns_ = 0;
    /// 待轮末生效的禁用/卸载列表 (io 线程)
    std::vector<std::string> pendingCleanup_{};
};

/// 能力注册表 (插件互查/委派; 如 JS 解释器能力 "interpreter.js")
class CapabilityRegistry {
public:

    bool registerCapability(std::string_view name, std::string_view providerPlugin);
    bool unregisterCapability(std::string_view name, std::string_view providerPlugin);
    bool has(std::string_view name) const;
    /// 提供某能力的插件名 (不存在返回空)
    std::string providerOf(std::string_view name) const;
    std::vector<std::string> names() const;

private:

    std::map<std::string, std::string, std::less<>> caps_{}; ///< <capability, providerPlugin>
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
