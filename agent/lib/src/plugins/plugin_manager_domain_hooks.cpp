/// 插件宿主领域钩子实现 (pluginxx::DomainHooks)
///
/// 背景: 通用表 (log/json/config/plugins/events/scheduler/coroutine_runtime/tasks/
/// cancel/capabilities) 的实现位于 cxx_pluginxx (见 pluginxx/host/host_core.h); 其中
/// 需要宿主数据的入口经 [pluginxx::DomainHooks] 取数。本文件把这些钩子接到
/// agentxx 的 AgentContext / EventBus / 工作线程池上, 另含事件表的事件后端适配
/// (AgentEventBusSource)。
///
/// 线程约定: 钩子都在宿主 IO 线程被调用 (通用表入口先投递到 IO 线程); 只有
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

#include <string>
#include <utility>

namespace agentxx {
namespace plugin {
namespace {

/// 插件事件表的宿主后端: agentxx::events::EventBus 适配
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

std::shared_ptr<EventSource> PluginManager::eventSource() {
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->bus) {
        return nullptr;
    }
    // 发布走本管理器的 IO executor (与既有行为一致: publish 不阻塞插件调用线程);
    // 未装配 executor 时退回事件总线自身的执行器。
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

bool PluginManager::postToWorkerThread(std::function<void()> fn) {
    if (!fn) {
        return false;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->threadPool) {
        return false;
    }
    try {
        asio::post(*ctx->threadPool, std::move(fn));
        return true;
    } catch (const std::exception& e) {
        XX_LOGW("Plugin offload could not be queued: {}", e.what());
        return false;
    } catch (...) {
        XX_LOGW("Plugin offload could not be queued: unknown exception");
        return false;
    }
}

// =====================================================================
// config 表
// =====================================================================

std::string PluginManager::configJson() {
    auto c = agentContext_.lock();
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
    auto c = agentContext_.lock();
    if (!c) {
        return "en";
    }
    return c->getLanguage();
}

std::string PluginManager::language() {
    return getLanguage();
}

void PluginManager::setLanguage(std::string_view lang) {
    auto c = agentContext_.lock();
    if (c) {
        c->setLanguage(lang);
    }
}

std::string PluginManager::getToolPromptJson(const std::string& toolName) {
    auto c = agentContext_.lock();
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

std::string PluginManager::getSessionWorkDir() {
    auto c = agentContext_.lock();
    if (!c || !c->agentConfig) {
        return {};
    }
    return c->agentConfig->resolvedWorkDir();
}

std::string PluginManager::getSessionWorkDir(std::string_view sessionId) {
    auto c = agentContext_.lock();
    if (!c) {
        return {};
    }
    auto session = c->getSession(sessionId);
    if (session && !session->getWorktreeBinding().path.empty()) {
        return session->getWorktreeBinding().path;
    }
    return getSessionWorkDir();
}

std::string PluginManager::sessionWorkDir(std::string_view sessionId) {
    return getSessionWorkDir(sessionId);
}

std::string PluginManager::getModelConfigJson() {
    auto c = agentContext_.lock();
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
// cancel 表
// =====================================================================

bool PluginManager::isSessionCancelled(std::string_view sessionId) {
    auto c = agentContext_.lock();
    if (!c || sessionId.empty()) {
        return false;
    }
    auto session = c->getSession(sessionId);
    if (!session || !session->getCancelToken()) {
        return false;
    }
    return session->getCancelToken()->is_cancelled();
}

bool PluginManager::isSessionCancelled(std::string_view sessionId) {
    return isSessionCancelled(std::string{sessionId});
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
