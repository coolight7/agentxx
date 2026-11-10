/// 插件网络能力: 管理器侧的适配 (`<应用名>.net` 表的落点)
///
/// - 属主 id = 插件实例名 (`inst->name`), 与其余业务表一致;
/// - 网络事件发到广播频道 `plugin.<插件 id>.net` (由主程序代发, 插件无需自己声明);
/// - 插件禁用/卸载时 [PluginManager::netShutdownPlugin] 收掉它的服务、连接与未完成请求。
#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/plugin/api/agentxx_broadcast_ids.h"

#include "agentxx/event/hub.h"

#include "utilxx/net_hub.h"

#include "fmt/format.h"

#include <asio/post.hpp>

namespace agentxx {
namespace plugin {

namespace {

/// 网络事件频道 (`plugin.<插件 id>.net`)
std::string netTopicOf(std::string_view owner) {
    return fmt::format("plugin.{}.net", owner);
}

} // namespace

std::shared_ptr<PluginNetService> PluginManager::netService() {
    std::lock_guard<std::mutex> lock(netMutex_);
    if (netService_) {
        return netService_;
    }
    std::weak_ptr<PluginManager> weakSelf = weak_from_this();
    PluginNetService::Hooks     hooks;
    hooks.publishEvent = [weakSelf](std::string_view owner, std::string_view payloadJson) {
        if (auto self = weakSelf.lock()) {
            self->publishNetEvent(owner, payloadJson);
        }
    };
    hooks.postToCaller = [weakSelf](std::function<void()> fn) {
        if (auto self = weakSelf.lock()) {
            self->postToPluginIo(std::move(fn));
        }
    };
    std::string dataDir;
    if (auto ctx = primaryAgent_.lock(); ctx && ctx->agentConfig) {
        dataDir = ctx->agentConfig->dataDir;
    }
    netService_ = PluginNetService::create(std::move(hooks), dataDir);
    return netService_;
}

void PluginManager::postToPluginIo(std::function<void()> fn) {
    if (!fn) {
        return;
    }
    auto ex = ioExecutor();
    if (!ex) {
        // 没有 io 线程时就地执行 (单线程测试夹具); 回调语义不变, 只是不等投递
        fn();
        return;
    }
    try {
        asio::post(ex, std::move(fn));
    } catch (const std::exception& e) {
        XX_LOGW("Plugin net callback could not be posted: {}", e.what());
    }
}

void PluginManager::publishNetEvent(std::string_view owner, std::string_view payloadJson) {
    if (owner.empty()) {
        return;
    }
    const std::string topic = netTopicOf(owner);
    postToPluginIo([this, topic, ownerCopy = std::string{owner}, payload = std::string{payloadJson}] {
        // 频道归插件 (它自己也能发); 未声明时按名字补一条
        ensureTopicBroadcast(topic);
        std::string error;
        // 主程序代发: 发布者就是属主本身 (插件已卸载也不要紧 — 事件本来就没处去了)
        if (!broadcasts()->publish(topic, payload, ownerCopy, &error)) {
            XX_LOGD("Plugin `{}` net event not published: {}", ownerCopy, error);
            return;
        }
        // 等这个事件的人走中枢 (与其它插件事件同一条路径)
        if (auto ctx = primaryAgent_.lock(); ctx && ctx->eventHub) {
            ctx->eventHub->publishPluginValue(topic, payload, ownerCopy);
        }
    });
}

int32_t PluginManager::netCall(
    std::string_view           owner,
    std::string_view           op,
    std::string_view           argsJson,
    std::string*               outJson,
    std::weak_ptr<PluginInstance> guard
) {
    (void)guard;
    auto service = netService();
    if (!service) {
        return utilxx::net::ErrState;
    }
    return service->call(owner, op, argsJson, outJson);
}

int32_t PluginManager::netCallAsync(
    std::string_view              owner,
    std::string_view              op,
    std::string_view              argsJson,
    PluginxxOperatorCallback      cb,
    void*                         ud,
    std::string*                  outJson,
    int64_t*                      outRequestId,
    std::weak_ptr<PluginInstance> guard
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

int32_t PluginManager::netCancel(
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

void PluginManager::netShutdownPlugin(std::string_view owner) {
    std::shared_ptr<PluginNetService> service;
    {
        std::lock_guard<std::mutex> lock(netMutex_);
        service = netService_;
    }
    if (service) {
        service->shutdownOwner(owner);
    }
}

void PluginManager::netShutdownAll() {
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
