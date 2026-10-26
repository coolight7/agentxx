/// 插件主程序业务数据入口实现 (pluginxx::DomainHooks)
///
/// 背景: 通用表 (log/json/config/plugins/events/scheduler/coroutine_runtime/tasks/
/// cancel/capabilities) 的实现位于 cxx_pluginxx (见 pluginxx/host/host_core.h); 其中
/// 需要主程序数据的入口经 [pluginxx::DomainHooks] 取数。本文件把这些钩子接到
/// agentxx 的 AgentContext / EventBus / 工作线程池上, 另含事件表的事件后端适配
/// (AgentEventBusSource)。
///
/// 线程约定: 钩子都在主程序 IO 线程被调用 (通用表入口先投递到 IO 线程); 只有
/// `postToWorkerThread` 例外, 它把一段阻塞工作投递到工作线程池。
#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <string>
#include <thread>
#include <utility>

namespace agentxx {
namespace plugin {
namespace {

/// 插件事件表的主程序后端: agentxx::events::EventBus 适配
///
/// - 主题到事件流的映射: `bus->get<std::string>(topic)` (插件事件载荷为 JSON 文本);
/// - 订阅的 handler 由 EventBus 在其所属执行器 (agent IO 线程) 上调用, 本适配器
///   只做类型转换与转发, 不做线程切换与实例生命周期保护 —— 后者由通用表实现
///   ([pluginxx::PluginHostCore::subscribe]) 经实例执行 lease 完成;
/// - 发布为异步 (与 agentxx 既有行为一致: 发布不阻塞插件 IO 线程)。
class AgentEventBusSource : public EventSource {
public:

    AgentEventBusSource(std::shared_ptr<agentxx::events::EventBus> bus, asio::any_io_executor ex) :
        bus_(std::move(bus)),
        executor_(std::move(ex)) {}

    size_t
        subscribe(std::string_view topic, std::function<void(std::string_view)> handler) override {
        if (!bus_ || !handler) {
            return 0;
        }
        return bus_->get<std::string>(topic).subscribe(
            [handler = std::move(handler)](const std::string& data) -> asio::awaitable<void> {
                try {
                    handler(std::string_view{data});
                } catch (const std::exception& e) {
                    XX_LOGW("Plugin event handler threw: {}", e.what());
                } catch (...) {
                    XX_LOGW("Plugin event handler threw unknown exception");
                }
                co_return;
            }
        );
    }

    void unsubscribe(std::string_view topic, size_t subscriptionId) override {
        if (!bus_ || subscriptionId == 0) {
            return;
        }
        bus_->get<std::string>(topic).unsubscribe(subscriptionId);
    }

    int publish(std::string_view topic, std::string_view eventJson) override {
        if (!bus_ || !executor_) {
            return -1;
        }
        asio::co_spawn(
            executor_,
            [bus = bus_, fullTopic = std::string{topic}, payload = std::string{eventJson}](
            ) -> asio::awaitable<void> {
                co_await bus->publish(fullTopic, payload);
            },
            asio::detached
        );
        return 0;
    }

private:

    std::shared_ptr<agentxx::events::EventBus> bus_;
    asio::any_io_executor                      executor_;
};

} // namespace

// =====================================================================
// 事件表
// =====================================================================

std::shared_ptr<agentxx::agent::AgentContext>
    PluginManager::agentContextOf(const PluginxxScopeRef* agent) const {
    if (agent == nullptr) {
        // 空引用 = 主程序级调用: 取主 agent
        return primaryAgent_.lock();
    }
    auto* mount = mountOf(agent);
    if (mount == nullptr) {
        return nullptr;
    }
    return mount->agentContext();
}

std::shared_ptr<EventSource> PluginManager::eventSource(const PluginxxScopeRef* agent) {
    // 事件落在具体 agent 上: 带引用时用该挂载的总线, 空引用才用主 agent 的总线
    auto ctx = agentContextOf(agent);
    if (!ctx || !ctx->bus) {
        return nullptr;
    }
    // 发布走本管理器的 IO executor (与既有行为一致: publish 不阻塞插件调用线程);
    // 未加载 executor 时退回事件总线自身的执行器。
    auto executor = ioExecutor() ? ioExecutor() : ctx->bus->executor();
    return std::make_shared<AgentEventBusSource>(ctx->bus, std::move(executor));
}

std::string PluginManager::qualifyEventTopic(std::string_view topic) {
    std::string full{topic};
    if (!full.starts_with("plugin.") && !full.starts_with("client.")) {
        full = "plugin." + full;
    }
    return full;
}

// =====================================================================
// 调度表
// =====================================================================

void PluginManager::setWorkerThreadPool(std::shared_ptr<asio::thread_pool> pool) {
    workerPool_ = std::move(pool);
}

/// 兜底线程池: 只在没有任何可用池时创建 (每次管理器一份, 不复用全局)
void PluginManager::ensureWorkerPool() {
    if (workerPool_) {
        return;
    }
    auto workers = std::max(2u, std::thread::hardware_concurrency() / 2);
    workerPool_  = std::make_shared<asio::thread_pool>(workers);
    XX_LOGW(
        "Plugin offload: no injected worker pool, created a private pool ({} thread(s))",
        workers
    );
}

bool PluginManager::postToWorkerThread(std::function<void()> fn) {
    if (!fn) {
        return false;
    }
    // 工作体包在 shared_ptr 里: 投递失败时不会被移走, 可以换下一个池重试
    auto task   = std::make_shared<std::function<void()>>(std::move(fn));
    auto submit = [&task](asio::thread_pool& pool) -> bool {
        try {
            asio::post(pool, [task]() { (*task)(); });
            return true;
        } catch (const std::exception& e) {
            XX_LOGW("Plugin offload could not be queued: {}", e.what());
            return false;
        } catch (...) {
            XX_LOGW("Plugin offload could not be queued: unknown exception");
            return false;
        }
    };

    // 1. 注入的池 (AgentHost 的进程共享池 / agent 自己的池)
    if (workerPool_ && submit(*workerPool_)) {
        return true;
    }
    // 2. 主 agent 上下文的池 (未注入时的默认来源)
    if (auto ctx = primaryAgent_.lock(); ctx && ctx->threadPool) {
        if (submit(*ctx->threadPool)) {
            return true;
        }
    }
    // 3. 兜底: 管理器自建池
    //    agent 上下文可能已释放、从未挂载, 或其池已停止; 插件阻塞工作仍应能执行
    //    (否则阻塞工具全部报 offload 失败)
    ensureWorkerPool();
    if (workerPool_ && submit(*workerPool_)) {
        return true;
    }
    XX_LOGW("Plugin offload is unavailable: no usable worker thread pool");
    return false;
}

// =====================================================================
// config 表
// =====================================================================

std::string PluginManager::configJson() {
    auto c = primaryAgent_.lock();
    if (!c || !c->agentConfig) {
        return {};
    }
    utilxx_base::Json out;
    out["dataDir"]     = c->agentConfig->dataDir;
    out["projectRoot"] = c->agentConfig->workDir;
    out["language"]    = getLanguage();
#if XX_IS_WIN_D
    out["platform"] = "windows";
#elif XX_IS_MACOS_D
    out["platform"] = "macos";
#elif XX_IS_LINUX_D
    out["platform"] = "linux";
#elif XX_IS_IOS_D
    out["platform"] = "ios";
#elif XX_IS_ANDROID_D
    out["platform"] = "android";
#endif
    return out.dump();
}

std::string PluginManager::getLanguage() {
    auto c = primaryAgent_.lock();
    if (!c) {
        return "en";
    }
    return c->getLanguage();
}

std::string PluginManager::language() {
    return getLanguage();
}

void PluginManager::setLanguage(std::string_view lang) {
    auto c = primaryAgent_.lock();
    if (c) {
        c->setLanguage(lang);
    }
}

std::string PluginManager::getModelConfigJson() {
    auto c = primaryAgent_.lock();
    if (!c || !c->agentConfig) {
        return {};
    }
    const auto&       cfg = *c->agentConfig;
    utilxx_base::Json out;
    out["baseUrl"]                       = cfg.model.baseUrl;
    out["apiKey"]                        = cfg.model.apiKey;
    out["modelName"]                     = cfg.model.modelName;
    out["websearchApiUrl"]               = cfg.websearchApiUrl;
    out["websearchConvertHtml2markdown"] = cfg.websearchConvertHtml2markdown;
    if (cfg.websearchModel) {
        utilxx_base::Json wm;
        wm["baseUrl"]                 = cfg.websearchModel->baseUrl;
        wm["apiKey"]                  = cfg.websearchModel->apiKey;
        wm["modelName"]               = cfg.websearchModel->modelName;
        wm["readChunkTimeoutSeconds"] = cfg.websearchModel->maxConcurrentConnections;
        out["websearchModel"]         = wm;
    } else {
        out["websearchModel"] = nullptr;
    }
    out["ragDocsPaths"] = cfg.ragDocsPaths;
    return out.dump();
}

// =====================================================================
// agentxx 业务表的数据来源 (会话工作目录 / 取消查询 / 工具提示词)
// =====================================================================
// 这三项都是主程序业务概念 (会话、轮次取消、工具提示词), 不属于插件框架的
// 通用能力, 因此由 agentxx 自己的接口表对外提供:
// - 会话工作目录 / 取消查询 -> agentxx.agent.session (见 plugin_manager_vtable.cpp)
// - 工具提示词 -> agentxx.agent.prompt

std::string PluginManager::sessionWorkDir(const PluginxxScopeRef* agent, std::string_view sessionId) {
    // 会话属于具体 agent: 带引用时取该挂载的 agent (主 agent 的会话可能根本没有该 sessionId)
    auto c = agentContextOf(agent);
    if (!c || !c->agentConfig) {
        return {};
    }
    if (!sessionId.empty()) {
        if (auto session = c->getSession(sessionId);
            session && !session->getWorktreeBinding().path.empty()) {
            return session->getWorktreeBinding().path;
        }
    }
    return c->agentConfig->resolvedWorkDir();
}

bool PluginManager::isSessionCancelled(const PluginxxScopeRef* agent, std::string_view sessionId) {
    if (sessionId.empty()) {
        return false;
    }
    // 取消状态属于具体 agent 的会话: 带引用时取该挂载的 agent
    auto c = agentContextOf(agent);
    if (!c) {
        return false;
    }
    auto session = c->getSession(sessionId);
    if (!session || !session->getCancelToken()) {
        return false;
    }
    return session->getCancelToken()->is_cancelled();
}

std::string PluginManager::getToolPromptJson(const std::string& toolName) {
    auto c = primaryAgent_.lock();
    if (!c || !c->agentConfig) {
        return {};
    }
    const auto& prompts = c->agentConfig->prompt.toolPrompt;
    auto        it      = prompts.find(toolName);
    if (it == prompts.end()) {
        return {};
    }
    utilxx_base::Json out;
    out["depict"]          = it->second.depict;
    utilxx_base::Json args = utilxx_base::Json::object();
    for (const auto& [k, v] : it->second.args) {
        args[k] = v;
    }
    out["args"] = std::move(args);
    return out.dump();
}

std::string PluginManager::toolPromptJson(std::string_view toolName) {
    return getToolPromptJson(std::string{toolName});
}

// =====================================================================
// plugins 表
// =====================================================================

std::string PluginManager::pluginsJson() {
    return listPluginsJson();
}

std::string PluginManager::pluginJson(std::string_view name) {
    return getPluginJson(std::string{name});
}

} // namespace plugin
} // namespace agentxx
