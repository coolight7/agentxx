#pragma once

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
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace agentxx {
namespace agent {
class AgentContext;
class AgentResourceApplier;
} // namespace agent

namespace event {
class EventBus;
}

namespace plugin {

class PluginManager;
class CapabilityRegistry;
class PluginMiddlewareHandle;
class PluginTool;
class PluginInstance;

struct PluginSleepTimer {
    std::weak_ptr<PluginInstance>       inst;
    std::shared_ptr<asio::steady_timer> timer;
    void (*cb)(void* ud) = nullptr;
    void*             ud = nullptr;
    std::atomic<bool> triggered{false};
};

} // namespace plugin
} // namespace agentxx

struct AgentxxPluginOperatorHandle {
    agentxx::plugin::PluginInstance* caller = nullptr;
    std::function<void()>            cancelFn;
    std::atomic<bool>                cancelled{false};
};

struct AgentxxPluginSubscription {
    std::shared_ptr<agentxx::event::EventBus> bus;
    std::string                               topic;
    size_t                                    subscriptionId      = 0;
    agentxx::plugin::PluginInstance*          inst                = nullptr;
    void (*handler)(AgentxxPluginStringView event_json, void* ud) = nullptr;
    void* ud                                                      = nullptr;
};

namespace agentxx {
namespace plugin {

class PluginInstance {
public:

    std::string              name;
    std::string              version;
    std::string              description;
    std::string              path;
    neograph::json           args = neograph::json::object();
    /// 插件配置文件所在目录或文件路径 (yaml `config`, 归一化为绝对路径)
    /// - 可指向文件或目录; 为空表示未指定
    std::string configPath;
    std::vector<std::string> depends;
    std::vector<std::string> optionalDepends;
    PluginManifestInterfaces interfaces;
    void*                    dlHandle        = nullptr;
    AgentxxPluginDestroyFn   builtinUnload   = nullptr;
    void*                    pluginCtx       = nullptr;
    bool                     enabled         = true;
    bool                     userDisabled    = false;
    bool                     unloadRequested = false;
    /// 资源冻结标志: 插件初始化阶段 (create 内) 允许注册 skill/memory/mcp,
    /// 初始化完成后冻结，后续固定不可变以防上下文变化 (仅 yaml 声明与初始化追加生效)
    bool resourcesFrozen = false;

    AgentxxPluginHost   host{};
    std::atomic<size_t> inflight{0};

    struct HookRegistration {
        AgentxxPluginHookPoint point;
        void* (*start)(void*, AgentxxPluginHookPoint, AgentxxPluginStringView, const AgentxxPluginOperatorNotify*, char**);
        void (*cancel)(void*, void*);
        void* ud;
    };

    struct CapabilityRegistration {
        std::string                          name;
        AgentxxPluginCapabilityStartFunction start  = nullptr;
        AgentxxPluginOperatorCancelFunction  cancel = nullptr;
        void*                                ctx    = nullptr;
    };

    struct PromptBackup {
        std::optional<std::string> systemPrompt;
        std::map<std::string, std::optional<std::string>, std::less<>> appendSystemPrompts;
        std::map<std::string, std::optional<agentxx::agent::ToolPrompt>, std::less<>> toolPrompt;
        std::vector<std::string>                                                      backedUpTools;
        bool backedUpSystem = false;
    };

    std::vector<std::string>                                toolNames;
    std::vector<HookRegistration>                           hookRegistrations;
    std::vector<std::shared_ptr<AgentxxPluginSubscription>> subscriptions;
    std::vector<CapabilityRegistration>                     capabilityRegistrations;
    // B6: sleep 定时器改为哈希表 O(1) 取消，避免 vector 线性查找 O(n) 与卸载时 O(n²)
    std::unordered_map<void*, std::shared_ptr<PluginSleepTimer>> sleepTimers;
    std::vector<std::shared_ptr<AgentxxPluginOperatorHandle>>    outstandingOps;
    PromptBackup                                                 promptBackup;

    std::shared_ptr<PluginMiddlewareHandle>  middleware = nullptr;
    std::vector<std::shared_ptr<PluginTool>> tools;

    std::weak_ptr<PluginInstance> self{};
    std::weak_ptr<PluginManager>  manager{};

    explicit PluginInstance(std::string in_name) :
        name(std::move(in_name)) {}

    ~PluginInstance();

    struct InflightGuard {
        std::shared_ptr<PluginInstance> inst;

        explicit InflightGuard(std::shared_ptr<PluginInstance> i) :
            inst(std::move(i)) {
            if (inst) {
                inst->inflight.fetch_add(1, std::memory_order_acq_rel);
            }
        }

        explicit InflightGuard(PluginInstance* i) :
            inst(i ? i->self.lock() : nullptr) {
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

class PluginTool : public agentxx::tools::XXToolBase {
public:

    PluginTool(
        std::weak_ptr<agentxx::agent::AgentContext> agentContext,
        std::shared_ptr<PluginInstance>             instance,
        AgentxxPluginToolSpec                       spec
    );

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;

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
    neograph::json                parameters_;
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
        void* (*start)(void*, AgentxxPluginHookPoint, AgentxxPluginStringView, const AgentxxPluginOperatorNotify*, char**)
            = nullptr;
        void (*cancel)(void*, void*) = nullptr;
        void* ud                     = nullptr;
        bool  set                    = false;
    };

    asio::awaitable<void>
        dispatch(AgentxxPluginHookPoint point, const neograph::graph::NodeInput& in);

    std::weak_ptr<PluginInstance>                    instance_;
    std::array<HookEntry, AGENTXX_PLUGIN_HOOK_COUNT> hooks_{};
};

class PluginManager : public std::enable_shared_from_this<PluginManager> {
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

    asio::awaitable<bool> unloadAsync(std::string_view name);
    void                  disable(std::string_view name);
    void                  enable(std::string_view name);
    void                  flushPendingCleanup();

    asio::awaitable<void>
        loadConfiguredPlugins(const std::vector<agentxx::agent::PluginConfig>& plugins);

    asio::awaitable<std::shared_ptr<PluginInstance>> loadPluginAsync(
        std::string                         path,
        const agentxx::agent::PluginConfig* cfg                 = nullptr,
        bool                                allowClientOnlySkip = false
    );

    void shutdownAll();

    std::vector<PluginListView>     list() const;
    std::shared_ptr<PluginInstance> find(std::string_view name) const;

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
    int unregisterTool(PluginInstance* inst, const char* name);

    int         registerSkillDir(PluginInstance* inst, const char* path);
    int         unregisterSkillDir(PluginInstance* inst, const char* path);
    int         registerMemoryFile(PluginInstance* inst, const char* path);
    int         unregisterMemoryFile(PluginInstance* inst, const char* path);
    int         registerMcpServer(PluginInstance* inst, const char* specJson);
    int         unregisterMcpServer(PluginInstance* inst, const char* nameSpace);
    std::string ownResourcesJson(const PluginInstance* inst);

    int registerHook(PluginInstance* inst, const AgentxxPluginHookSpec* spec);
    int unregisterHook(PluginInstance* inst, AgentxxPluginHookPoint point);
    AgentxxPluginSubscription* subscribe(
        PluginInstance* inst,
        const char*     topic,
        void (*handler)(AgentxxPluginStringView event_json, void* ud),
        void* ud
    );
    void      unsubscribe(AgentxxPluginSubscription* sub);
    int       publish(const char* topic, const char* event_json);
    char*     getShareStore(PluginInstance* inst, const char* session_id, long long id);
    long long addShareStore(PluginInstance* inst, const char* session_id, const char* content);
    void emitMessageTip(PluginInstance* inst, const char* session_id, const char* text, int level);

    void* sleep(PluginInstance* inst, long ms, void (*cb)(void* ud), void* ud);
    void  cancelSleep(PluginInstance* inst, void* timer);
    void  offload(
         PluginInstance* inst,
         volatile int*   cancel_flag,
         void* (*work)(void* ud, volatile int* cancel_flag, char** error_out),
         void (*done)(void* ud, void* result, char* error),
         void* ud
     );

    AgentxxPluginOperatorHandle* callToolAsync(
        PluginInstance*               caller,
        const char*                   name,
        const char*                   args_json,
        const char*                   session_id,
        AgentxxPluginOperatorCallback cb,
        void*                         ud,
        char**                        error_out
    );

    AgentxxPluginOperatorHandle* invokeCapabilityAsync(
        PluginInstance*               caller,
        const char*                   capability,
        const char*                   method,
        const char*                   args_json,
        AgentxxPluginOperatorCallback cb,
        void*                         ud,
        char**                        error_out
    );

    std::shared_ptr<ToolRegistry> registry() const {
        return registry_;
    }

    std::shared_ptr<CapabilityRegistry> capabilities() const {
        return capabilities_;
    }

    void setIoExecutor(asio::any_io_executor ex) {
        ioExecutor_ = std::move(ex);
        if (ioExecutor_) {
            ioThreadId_.store(std::this_thread::get_id(), std::memory_order_release);
        }
    }

    bool isIoThread() const {
        const auto tid = ioThreadId_.load(std::memory_order_acquire);
        return !ioExecutor_ || (tid != std::thread::id{} && tid == std::this_thread::get_id());
    }

    mutable std::mutex                        ioTasksMtx_;
    mutable std::deque<std::function<void()>> ioTasks_;

    void postToIo(std::function<void()> fn) const {
        if (isIoThread()) {
            fn();
        } else if (ioExecutor_) {
            {
                std::lock_guard lk(ioTasksMtx_);
                ioTasks_.push_back(std::move(fn));
            }
            asio::post(ioExecutor_, [this]() {
                ioThreadId_.store(std::this_thread::get_id(), std::memory_order_release);
                runPendingIoTasks();
            });
        } else {
            XX_LOGW("PluginManager::postToIo: no io executor, executing on caller thread");
            fn();
        }
    }

    /// 异步投递到 io 线程（恒经 asio::post 入队，禁止同步重入）：
    /// 供 SchedulerIface::post_to_io / YieldAwaiter 等锚定协程恢复路径使用，
    /// 即使调用方已在 io 线程也一律异步，避免 await_suspend 内的重入 UB
    void postToIoAsync(std::function<void()> fn) const {
        if (ioExecutor_) {
            {
                std::lock_guard lk(ioTasksMtx_);
                ioTasks_.push_back(std::move(fn));
            }
            asio::post(ioExecutor_, [this]() {
                ioThreadId_.store(std::this_thread::get_id(), std::memory_order_release);
                runPendingIoTasks();
            });
        } else {
            XX_LOGW("PluginManager::postToIoAsync: no io executor, executing on caller thread");
            fn();
        }
    }

    void runPendingIoTasks() const {
        std::deque<std::function<void()>> tasks;
        {
            std::lock_guard lk(ioTasksMtx_);
            tasks.swap(ioTasks_);
        }
        for (auto& t : tasks) {
            if (t) {
                try {
                    t();
                } catch (...) {
                }
            }
        }
    }

    int registerCapability(PluginInstance* inst, const char* capability);
    int registerCapabilityEx(
        PluginInstance*                      inst,
        const char*                          capability,
        AgentxxPluginCapabilityStartFunction start,
        AgentxxPluginOperatorCancelFunction  cancel,
        void*                                ctx
    );
    int unregisterCapability(PluginInstance* inst, const char* capability);
    int hasCapability(const char* capability) const;

    std::string listPluginsJson();
    std::string getPluginJson(const std::string& name);

    std::string getConfigJson();
    std::string getToolPromptJson(const std::string& toolName);
    std::string getPromptJson();
    int         setPromptJson(PluginInstance* inst, const char* prompt_json);
    void        restorePromptBackup(PluginInstance* inst);
    void        applyDeclaredResources(
               PluginInstance&                        inst,
               const plugin::PluginManifestResources& resources
           );
    std::string getPluginArgsJson(PluginInstance* inst);
    std::string getPluginConfigPath(PluginInstance* inst);

    std::string getSessionWorkDir();
    std::string getSessionWorkDir(const std::string& threadId);
    std::string getModelConfigJson();
    bool        isSessionCancelled(const std::string& threadId);

private:

    friend class PluginInstance;

    asio::awaitable<bool> waitInflightZero(
        const std::shared_ptr<PluginInstance>& inst,
        std::chrono::milliseconds              timeout
    );

    void detachAll(PluginInstance* inst);
    void eraseMiddleware(PluginMiddlewareHandle* mw);

    struct PendingMiddlewareCleanup {
        std::string                           name;
        std::weak_ptr<PluginMiddlewareHandle> mw;
    };

    std::vector<PendingMiddlewareCleanup> pendingCleanups_;

    void shutdownPlugin(const std::shared_ptr<PluginInstance>& inst);

    std::weak_ptr<agentxx::agent::AgentContext>                         agentContext_;
    std::shared_ptr<ToolRegistry>                                       registry_;
    std::shared_ptr<CapabilityRegistry>                                 capabilities_;
    std::map<std::string, std::shared_ptr<PluginInstance>, std::less<>> plugins_;
    size_t                                                              runningTurns_ = 0;
    asio::any_io_executor                                               ioExecutor_{};
    mutable std::atomic<std::thread::id>                                ioThreadId_{};
};

struct NativeLoader {
    static void* open(const std::string& path, std::string& err);
    static void* sym(void* handle, const char* name, std::string& err);
    static void  close(void* handle);
    static void  addSearchPath(std::string_view dir);
};

class CapabilityRegistry {
public:

    struct Entry {
        std::string                          provider;
        AgentxxPluginCapabilityStartFunction start  = nullptr;
        AgentxxPluginOperatorCancelFunction  cancel = nullptr;
        void*                                ctx    = nullptr;
    };

    bool registerCapability(
        std::string_view                     name,
        std::string_view                     provider,
        AgentxxPluginCapabilityStartFunction start  = nullptr,
        AgentxxPluginOperatorCancelFunction  cancel = nullptr,
        void*                                ctx    = nullptr
    );

    bool                     unregisterCapability(std::string_view name, std::string_view provider);
    bool                     has(std::string_view name) const;
    const Entry*             get(std::string_view name) const;
    std::string              providerOf(std::string_view name) const;
    std::vector<std::string> names() const;

private:

    std::map<std::string, Entry, std::less<>> caps_;
};

} // namespace plugin
} // namespace agentxx
