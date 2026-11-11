/// 客户端插件主程序的业务数据入口 (pluginxx::DomainServices) + 广播发布
///
/// 背景: 通用表 (log/json/config/plugins/broadcasts/scheduler/coroutine_runtime/
/// tasks/cancel/capabilities) 的实现位于 cxx_pluginxx (见 pluginxx/host/host_core.h);
/// 其中需要主程序数据的入口经 [pluginxx::DomainServices] 取数。本文件把它们接到
/// client 插件管理器上, 并声明/发布客户端自己的频道 (`agentxx.client.*`)。
///
/// 线程约定: 除 [postToWorkerThread] 外都在 client io 线程执行 (通用表入口先投递);
/// 广播注册表内部有锁, 因此 UI 线程直接发布也不会与插件侧订阅冲突。
#include "agentxx/plugin/client_plugin_manager.h"

#include "agentxx/event/naming.h"
#include "agentxx/plugin/api/agentxx_broadcast_ids.h"

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
// 调度表 (阻塞工作的工作线程池)
// =====================================================================

/// 工作线程数: 客户端只服务界面与少量网络/文件工作, 固定 2 个线程即可
/// (agent 侧有注入池时用注入的, 客户端插件管理器没有注入方, 自建一份)
static constexpr size_t kClientWorkerThreads = 2;

bool ClientPluginManager::postToWorkerThread(std::function<void()> fn) {
    if (!fn) {
        return false;
    }
    // 只由 client io 线程调用 (见 domain_services.h 的线程约定), 懒建不需要锁
    if (!workerPool_) {
        try {
            workerPool_ = std::make_shared<asio::thread_pool>(kClientWorkerThreads);
        } catch (const std::exception& e) {
            XX_LOGW("[client_plugin] offload pool could not be created: {}", e.what());
            return false;
        }
    }
    // 工作体包在 shared_ptr 里: 投递失败时不会被移走, 便于日志里说明
    auto task = std::make_shared<std::function<void()>>(std::move(fn));
    try {
        asio::post(*workerPool_, [task]() { (*task)(); });
        return true;
    } catch (const std::exception& e) {
        XX_LOGW("[client_plugin] offload could not be queued: {}", e.what());
        return false;
    } catch (...) {
        XX_LOGW("[client_plugin] offload could not be queued: unknown exception");
        return false;
    }
}

// =====================================================================
// config 表
// =====================================================================

std::string ClientPluginManager::configJson() {
    // 客户端进程自己的配置: 界面语言 + 平台 (路径类配置属于 agent 侧, 界面进程
    // 不参与 agent 决策, 见 docs/zh-cn/design/configuration.md 第 5 节)
    utilxx_base::Json out;
    out["language"] = getLanguage();
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

std::string ClientPluginManager::language() {
    return getLanguage();
}

// =====================================================================
// plugins 表
// =====================================================================

std::string ClientPluginManager::pluginsJson() {
    utilxx_base::Json arr = utilxx_base::Json::array();
    for (const auto& v : list()) {
        utilxx_base::Json item;
        item["name"]                = v.name;
        item["version"]             = v.version;
        item["description"]         = v.description;
        item["path"]                = v.path;
        item["config"]              = v.configPath;
        item["enabled"]             = v.enabled;
        item["status_items"]        = v.statusItems;
        item["panels"]              = v.panels;
        item["info_sections"]       = v.infoSections;
        item["commands"]            = v.commands;
        item["depends"]             = v.depends;
        item["optional_depends"]    = v.optionalDepends;
        item["required_interfaces"] = v.requiredInterfaces;
        item["optional_interfaces"] = v.optionalInterfaces;
        arr.push_back(std::move(item));
    }
    return arr.dump();
}

std::string ClientPluginManager::pluginJson(std::string_view name) {
    for (const auto& v : list()) {
        if (v.name != name) {
            continue;
        }
        utilxx_base::Json item;
        item["name"]                = v.name;
        item["version"]             = v.version;
        item["description"]         = v.description;
        item["path"]                = v.path;
        item["config"]              = v.configPath;
        item["enabled"]             = v.enabled;
        item["inflight"]            = v.inflight;
        item["status_items"]        = v.statusItems;
        item["panels"]              = v.panels;
        item["info_sections"]       = v.infoSections;
        item["commands"]            = v.commands;
        item["depends"]             = v.depends;
        item["optional_depends"]    = v.optionalDepends;
        item["required_interfaces"] = v.requiredInterfaces;
        item["optional_interfaces"] = v.optionalInterfaces;
        return item.dump();
    }
    return {};
}

// =====================================================================
// 广播 (客户端频道 + 跨端插件数据 + 网络事件)
// =====================================================================

void ClientPluginManager::ensureClientBroadcast(
    std::string_view           id,
    pluginxx::BroadcastCache_e cache,
    bool                       anyonePublish,
    bool                       pushOnSubscribe
) {
    auto registry = broadcasts();
    if (id.empty() || !registry || registry->declares(id)) {
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
    if (!registry->declare(decl, &err)) {
        XX_LOGW("[client_plugin] broadcast `{}` could not be declared: {}", id, err);
    }
}

void ClientPluginManager::declareClientChannels() {
    // 插件在 start 阶段 (管理器构造之后、连接之前) 就会订阅, 因此构造时就声明好 (幂等)。
    // 状态类频道保留当前值 (`cache: latest`): 插件上线时能读到最新状态;
    // 事件类频道不留值 (`cache: none`): 错过就是错过, 避免把旧增量当成新内容。
    for (const auto id : {
             broadcasts::ClientReady,
             broadcasts::ClientConnectionState,
             broadcasts::ClientSessionSwitch,
             broadcasts::ClientUiLayout,
         }) {
        ensureClientBroadcast(id, pluginxx::BroadcastCache_e::Latest);
    }
    for (const auto id : {
             broadcasts::ClientUserInput,
             broadcasts::ClientDelta,
             broadcasts::ClientTurnEnd,
         }) {
        ensureClientBroadcast(id, pluginxx::BroadcastCache_e::None);
    }
}

void ClientPluginManager::publishClientChannelValue(
    std::string_view id,
    std::string_view payloadJson
) {
    publishClientChannel(id, std::string{payloadJson});
}

void ClientPluginManager::ensureClientChannelDeclared(std::string_view id) {
    auto registry = broadcasts();
    if (id.empty() || !registry || registry->declares(id)) {
        return;
    }
    pluginxx::BroadcastDecl decl;
    decl.id    = std::string{id};
    decl.scope = pluginxx::BroadcastScope_e::App;
    if (const auto owner = agentxx::events::naming::clientPluginNameOfTopic(id); !owner.empty()) {
        // 跨端插件数据 (`agentxx.client.plugin.<插件名>.<事件>`): 两端都可发, 不缓存
        decl.owner   = std::string{owner};
        decl.cache   = pluginxx::BroadcastCache_e::None;
        decl.publish = pluginxx::BroadcastPublisher_e::Anyone;
    } else if (const auto owner = agentxx::events::naming::pluginNameOfTopic(id);
               !owner.empty()) {
        // 插件自建频道 / 网络事件 (`plugin.<插件名>.<名字>`): 属主发布,
        // 与 agent 侧 [PluginManager::ensureTopicBroadcast] 同一份约定
        decl.owner   = std::string{owner};
        decl.cache   = pluginxx::BroadcastCache_e::Latest;
        decl.publish = pluginxx::BroadcastPublisher_e::Owner;
    } else {
        // 应用频道 (`agentxx.client.*` 等): 跨端时由另一端声明, 谁都能发
        decl.cache   = pluginxx::BroadcastCache_e::Latest;
        decl.publish = pluginxx::BroadcastPublisher_e::Anyone;
    }
    std::string err;
    if (!registry->declare(decl, &err)) {
        XX_LOGD("[client_plugin] broadcast `{}` could not be declared: {}", id, err);
    }
}

PluginxxSubscription* ClientPluginManager::subscribeBroadcast(
    ClientPluginInstance*    inst,
    const PluginxxScopeRef*  scope,
    std::string_view         broadcastId,
    void(PLUGINXX_CALL* handler)(const PluginxxStringView* payloadJson, void* ud),
    void*                    ud,
    std::string_view         optionsJson,
    void(PLUGINXX_CALL* onEnd)(const PluginxxStringView* reasonJson, void* ud)
) {
    // 跨进程频道在本进程常常还没人声明 (另一端才声明) —— 先补一条, 否则订阅被拒
    ensureClientChannelDeclared(broadcastId);
    return pluginxx::PluginHostCore<ClientPluginInstance>::subscribeBroadcast(
        inst,
        scope,
        broadcastId,
        handler,
        ud,
        optionsJson,
        onEnd
    );
}

void ClientPluginManager::publishClientChannel(
    std::string_view   channel,
    const std::string& payloadJson
) {
    if (channel.empty() || payloadJson.empty()) {
        return;
    }
    auto registry = broadcasts();
    if (!registry) {
        return;
    }

    // 兜底声明 (内置频道已在构造时声明, 这里不改它们)
    ensureClientChannelDeclared(channel);

    /// 发布者身份: 空 = 应用 (客户端主程序自己); 非空 = 代哪个插件发
    std::string publisher;
    if (const auto owner = agentxx::events::naming::clientPluginNameOfTopic(channel);
        !owner.empty()) {
        publisher = std::string{owner};
    } else if (const auto owner = agentxx::events::naming::pluginNameOfTopic(channel);
               !owner.empty()) {
        // 插件自建频道 (网络事件等): 主程序以该插件身份代发
        publisher = std::string{owner};
    }

    std::string err;
    if (!registry->publish(channel, payloadJson, publisher, &err)) {
        XX_LOGD("[client_plugin] broadcast `{}` not published: {}", channel, err);
    }
}

} // namespace plugin
} // namespace agentxx
