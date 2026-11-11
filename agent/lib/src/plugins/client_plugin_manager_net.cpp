/// 客户端插件的网络能力: 管理器侧的适配 (`<应用名>.net` 表的落点)
///
/// - 与 agent 侧同一个 [PluginNetService] (`utilxx::NetHub`), 属主 id = client 插件实例名;
/// - 客户端暂时没有广播表, 所以 `plugin.<插件 id>.net` 频道还不存在: 网络事件当前只记
///   调试日志 (是谁、什么 `kind`), HTTP 请求/服务端这类按请求与按服务的工作不受影响;
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
    // client 侧还没有广播表: 事件没有去处, 只留一条可查的日志
    // (接上广播后这里改成 declare + publish, 与 agent 侧同一份写法)
    std::string kind;
    try {
        auto parsed = utilxx_base::Json::parse(payloadJson);
        if (parsed.is_object()) {
            kind = parsed.value("kind", "");
        }
    } catch (...) {
    }
    XX_LOGD("Client plugin `{}` net event dropped (no broadcast table yet): {}", owner, kind);
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
