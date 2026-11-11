/// 客户端插件的网络能力: 管理器侧的适配 (`<应用名>.net` 表的落点)
///
/// - 与 agent 侧同一个 [PluginNetService] (`utilxx::NetHub`), 属主 id = client 插件实例名;
/// - 网络事件发到 `plugin.<插件 id>.net` 频道 (主程序以插件身份代发), 插件订阅即可;
/// - 插件禁用/卸载时 [ClientPluginManager::netShutdownPlugin] 收掉它的服务、连接与
///   未完成请求, 未完成的按"已取消"结算。
#include "agentxx/plugin/client_plugin_manager.h"

#include "utilxx/net_hub.h"

#include "fmt/format.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

namespace agentxx {
namespace plugin {

std::shared_ptr<PluginNetService> ClientPluginManager::netService() {
    std::lock_guard<std::mutex> lock(netMutex_);
    if (netService_) {
        return netService_;
    }
    std::weak_ptr<ClientPluginManager> weakSelf = weak_from_this();
    PluginNetService::Hooks            hooks;
    hooks.publishEvent = [weakSelf](std::string_view owner, std::string_view payloadJson) {
        if (auto self = weakSelf.lock()) {
            self->publishNetEvent(owner, payloadJson);
        }
    };
    hooks.postToCaller = [weakSelf](std::function<void()> fn) {
        auto self = weakSelf.lock();
        if (!self) {
            return;
        }
        self->postToIo(std::move(fn));
    };
    netService_ = PluginNetService::create(std::move(hooks), std::string{});
    return netService_;
}

void ClientPluginManager::publishNetEvent(std::string_view owner, std::string_view payloadJson) {
    if (owner.empty() || payloadJson.empty()) {
        return;
    }
    // 频道归插件 (`plugin.<插件名>.net`), 由主程序以该插件身份代发
    // (与 agent 侧同一份写法; 名单以 `kind` 区分事件类型)
    const std::string topic = fmt::format("plugin.{}.net", owner);
    publishClientChannel(topic, std::string{payloadJson});
}

int32_t ClientPluginManager::netCall(
    std::string_view                    owner,
    std::string_view                    op,
    std::string_view                    argsJson,
    std::string*                        outJson,
    std::weak_ptr<ClientPluginInstance> guard
) {
    (void)guard;
    auto service = netService();
    if (!service) {
        return utilxx::net::ErrState;
    }
    return service->call(owner, op, argsJson, outJson);
}

int32_t ClientPluginManager::netCallAsync(
    std::string_view                    owner,
    std::string_view                    op,
    std::string_view                    argsJson,
    PluginxxOperatorCallback            cb,
    void*                               ud,
    std::string*                        outJson,
    int64_t*                            outRequestId,
    std::weak_ptr<ClientPluginInstance> guard
) {
    auto service = netService();
    if (!service) {
        return utilxx::net::ErrState;
    }
    std::weak_ptr<void> voidGuard;
    if (auto instance = guard.lock()) {
        voidGuard = std::weak_ptr<void>(instance);
    }
    return service->callAsync(owner, op, argsJson, cb, ud, outJson, outRequestId, voidGuard);
}

int32_t ClientPluginManager::netCancel(
    std::string_view owner,
    int64_t          requestId,
    std::string*     outJson
) {
    auto service = netService();
    if (!service) {
        return utilxx::net::ErrState;
    }
    return service->cancel(owner, requestId, outJson);
}

void ClientPluginManager::netShutdownPlugin(std::string_view owner) {
    std::shared_ptr<PluginNetService> service;
    {
        std::lock_guard<std::mutex> lock(netMutex_);
        service = netService_;
    }
    if (service) {
        service->shutdownOwner(owner);
    }
}

void ClientPluginManager::netShutdownAll() {
    std::shared_ptr<PluginNetService> service;
    {
        std::lock_guard<std::mutex> lock(netMutex_);
        service = netService_;
        netService_.reset();
    }
    if (service) {
        service->stop();
    }
}

} // namespace plugin
} // namespace agentxx
