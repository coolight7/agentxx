/// 插件主程序业务数据入口实现 (pluginxx::DomainServices) + 广播接口
///
/// 背景: 通用表 (log/json/config/plugins/broadcasts/scheduler/coroutine_runtime/
/// tasks/cancel/capabilities) 的实现位于 cxx_pluginxx (见 pluginxx/host/host_core.h);
/// 其中需要主程序数据的入口经 [pluginxx::DomainServices] 取数。本文件把它们接到
/// agentxx 的 AgentContext / 工作线程池上, 另含主程序自己的广播发布/结束入口。
///
/// 线程约定: 钩子都在主程序 IO 线程被调用 (通用表入口先投递到 IO 线程); 只有
/// `postToWorkerThread` 例外, 它把一段阻塞工作投递到工作线程池。
#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/agent/context.h"
#include "agentxx/event/hub.h"
#include "agentxx/event/naming.h"
#include "agentxx/plugin/api/agentxx_broadcast_ids.h"
#include "agentxx/plugin/plugin_mount.h"
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

// =====================================================================
// 委员会 / 广播主题
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

std::string PluginManager::qualifyEventTopic(std::string_view topic) {
    // 不补前缀: 命名约定要求插件自己写全名 (`plugin.<插件名>.*` / `agentxx.*`),
    // 框架只在写侧校验 (见 [publishBroadcastValue] 与业务表入口), 读侧接受三族全名。
    return std::string{topic};
}

void PluginManager::ensureBroadcast(
    std::string_view           id,
    pluginxx::BroadcastCache_e cache,
    bool                       anyonePublish,
    bool                       pushOnSubscribe
) {
    if (id.empty() || !broadcasts_ || broadcasts_->declares(id)) {
        return;
    }
    pluginxx::BroadcastDecl decl;
    decl.id              = std::string{id};
    decl.cache           = cache;
    decl.publish         = anyonePublish ? pluginxx::BroadcastPublisher_e::Anyone
                                         : pluginxx::BroadcastPublisher_e::Owner;
    decl.pushOnSubscribe = pushOnSubscribe && cache == pluginxx::BroadcastCache_e::Latest;
    decl.scope           = pluginxx::BroadcastScope_e::App;
    std::string err;
    if (!broadcasts_->declare(decl, &err)) {
        XX_LOGW("broadcast `{}` could not be declared: {}", id, err);
    }
}

/// 从频道 id 里取出插件名 (`plugin.<插件名>.rest` → `<插件名>`; 其他形式返回空)
std::string PluginManager::pluginNameOfBroadcast(std::string_view id) {
    constexpr std::string_view prefix = "plugin.";
    if (id.rfind(prefix, 0) != 0) {
        return {};
    }
    const auto rest = id.substr(prefix.size());
    const auto dot  = rest.find('.');
    if (dot == std::string_view::npos || dot == 0) {
        return {};
    }
    return std::string{rest.substr(0, dot)};
}

void PluginManager::ensureTopicBroadcast(std::string_view id) {
    if (id.empty() || !broadcasts_ || broadcasts_->declares(id)) {
        return;
    }
    if (const auto owner = pluginNameOfBroadcast(id); !owner.empty()) {
        pluginxx::BroadcastDecl decl;
        decl.id      = std::string{id};
        decl.owner   = owner;
        decl.cache   = pluginxx::BroadcastCache_e::Latest;
        decl.publish = pluginxx::BroadcastPublisher_e::Owner;
        decl.scope   = pluginxx::BroadcastScope_e::App;
        std::string err;
        if (!broadcasts_->declare(decl, &err)) {
            XX_LOGD("broadcast `{}` could not be declared: {}", id, err);
        }
        return;
    }
    ensureBroadcast(id, pluginxx::BroadcastCache_e::Latest, /*anyonePublish=*/true);
}

void PluginManager::declareBuiltinBroadcasts() {
    // 插件在加载阶段 (start) 就可能订阅这些频道, 而事件中枢要晚到 `init()` 才建出来,
    // 中枢的"按需声明"来不及 —— 未声明的频道订阅会被拒, 于是这里先声明好 (幂等)。
    // 缓存策略与中枢按需声明保持一致 (`cache: latest`), 免得两边对同一频道给出不同属性。
    for (const auto id : {
             broadcasts::AgentRoundStart,
             broadcasts::AgentAsyncDone,
             broadcasts::AgentClientAttached,
         }) {
        ensureBroadcast(id, pluginxx::BroadcastCache_e::Latest);
    }
}

void PluginManager::publishBroadcastValue(std::string_view id, std::string_view payloadJson) {
    if (id.empty()) {
        return;
    }
    ensureBroadcast(id, pluginxx::BroadcastCache_e::Latest);
    std::string err;
    if (!broadcasts_->publish(id, payloadJson, std::string{}, &err)) {
        XX_LOGD("broadcast `{}` not published: {}", id, err);
    }
}

void PluginManager::endBroadcastValue(std::string_view id, std::string_view reasonJson) {
    if (id.empty() || !broadcasts_ || !broadcasts_->declares(id)) {
        return;
    }
    std::string err;
    if (!broadcasts_->end(id, reasonJson, std::string{}, &err) && !err.empty()) {
        XX_LOGD("broadcast `{}` not ended: {}", id, err);
    }
}

int PluginManager::publishBroadcastFrom(
    PluginInstance*  inst,
    std::string_view broadcastId,
    std::string_view payloadJson,
    std::string*     error
) {
    // 写侧只要求落在插件族里 (`plugin.*`); 不限定后面的名字是不是本实例的 ——
    // 同一组织的多个插件可能共用一段, 建议仍用自己的插件名。
    std::string naming;
    if (!agentxx::events::naming::validatePluginWriteTopic(broadcastId, &naming)) {
        if (error) {
            *error = naming;
        }
        XX_LOGW("Plugin publish rejected: `{}` ({})", broadcastId, naming);
        return PLUGINXX_RESULT_ERR_PERMISSION;
    }
    const int rc
        = pluginxx::PluginHostCore<PluginInstance>::publishBroadcastFrom(
            inst,
            broadcastId,
            payloadJson,
            error
        );
    if (rc == PLUGINXX_RESULT_OK) {
        // 中枢仍保留"记录 + 等待 + 投递"的职责: 没人等也没声明过的名字它自己会滤掉
        for (const auto& mount : mounts()) {
            auto ctx = mount != nullptr ? mount->agentContext() : nullptr;
            if (ctx && ctx->eventHub) {
                ctx->eventHub->publishPluginValue(
                    broadcastId,
                    payloadJson,
                    inst != nullptr ? inst->name : std::string{}
                );
            }
        }
        if (auto ctx = primaryAgent_.lock(); ctx && ctx->eventHub) {
            ctx->eventHub->publishPluginValue(
                broadcastId,
                payloadJson,
                inst != nullptr ? inst->name : std::string{}
            );
        }
    }
    return rc;
}

PluginxxSubscription* PluginManager::subscribeBroadcast(
    PluginInstance*          inst,
    const PluginxxScopeRef*  scope,
    std::string_view         broadcastId,
    void(PLUGINXX_CALL* handler)(const PluginxxStringView* payloadJson, void* ud),
    void*                    ud,
    std::string_view         optionsJson,
    void(PLUGINXX_CALL* onEnd)(const PluginxxStringView* reasonJson, void* ud)
) {
    // 订阅一条还没人声明过的频道时先补声明 (框架要求"先声明再订阅"):
    // `agentxx.*` 按"任何人可发"补一条 (跨端频道由另一端声明);
    // `plugin.<插件名>.*` 的属主按名字推断, 发布权仍属那个插件
    // (JS 脚本一类调用方无法替别的插件声明, 只能由主程序补);
    // 其他名字不代劳 (订阅会带原因失败)。
    ensureTopicBroadcast(broadcastId);
    return pluginxx::PluginHostCore<PluginInstance>::subscribeBroadcast(
        inst,
        scope,
        broadcastId,
        handler,
        ud,
        optionsJson,
        onEnd
    );
}


// =====================================================================
// 调度表
// =====================================================================

void PluginManager::setWorkerThreadPool(std::shared_ptr<asio::thread_pool> pool) {
    workerPool_ = std::move(pool);
}

/// 备用线程池: 只在没有任何可用池时创建 (每次管理器一份, 不复用全局)
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
    // 3. 备用: 管理器自建池
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

bool PluginManager::isSessionCanceled(const PluginxxScopeRef* agent, std::string_view sessionId) {
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
