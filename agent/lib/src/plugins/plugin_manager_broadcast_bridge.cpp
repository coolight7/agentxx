/// 广播 ↔ 事件总线两向转发 (主程序侧)
///
/// 插件只认 `<应用名>.broadcasts` 表, 而 `agentxx.agent.*` / `agentxx.client.*`
/// 这类应用话题由主程序发在事件总线上; 客户端拿到的插件数据又只能经
/// `WirePluginData` (端点从总线的 `plugin.` 前缀转发)。两个方向都缺一跳时:
///
/// - agent 侧插件收不到 `agentxx.agent.host.client_attached` 一类约定事件
///   (双端插件靠它重发状态快照, 于是客户端永远停在初始占位);
/// - 插件发布的广播不发 `WirePluginData`, 客户端一个值都收不到。
///
/// 这里补上两跳, 前缀互不重叠 (`agentxx.agent.` / `agentxx.client.` 对 `plugin.`),
/// 因此不会形成环:
///
/// ```text
/// 总线 agentxx.agent.* / agentxx.client.*  ──[mirrorBusValue]──▶  广播频道
/// 插件发布 plugin.*  ──[forwardBroadcastToBuses]──▶  总线  ──▶  端点  ──▶  客户端
/// ```
///
/// 线程约定: 总线前缀回调在发布方线程同步执行 (本项目即 io 线程), 注册表发布本身加锁,
/// 插件回调沿用既有"快速返回"约定。
#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "utilxx_base/log.h"

#include <string>
#include <utility>
#include <vector>

namespace agentxx {
namespace plugin {

void PluginManager::attachEventBus(const std::shared_ptr<agentxx::events::EventBus>& bus) {
    if (!bus || !broadcasts()) {
        return;
    }
    std::lock_guard lock(busBridgeMutex_);
    for (const auto& entry : busBridges_) {
        if (entry.bus.lock() == bus) {
            return; // 同一总线只挂一次
        }
    }
    // 两族应用话题各订阅一次前缀: 载荷只取 `std::string` (约定事件与插件数据的
    // JSON 原文), 强类型事件 (display / delta / error 等) 类型不同, 天然跳过
    auto mirror = [this](std::string_view topic, const std::any& data) {
        if (data.type() != typeid(std::string)) {
            return;
        }
        mirrorBusValue(topic, std::any_cast<const std::string&>(data));
    };
    BusBridge entry;
    entry.bus            = bus;
    entry.agentPrefixId  = bus->listenPrefix("agentxx.agent.", mirror);
    entry.clientPrefixId = bus->listenPrefix("agentxx.client.", mirror);
    busBridges_.push_back(std::move(entry));
}

void PluginManager::mirrorBusValue(std::string_view topic, std::string_view payloadJson) {
    auto registry = broadcasts();
    if (!registry || topic.empty()) {
        return;
    }
    // 只镜像**已经声明**成广播频道的名字: 插件订阅时按需声明, 内置频道在构造时声明。
    // 其余话题 (没人订阅的约定事件、高频强类型话题) 一次查找就返回, 不建频道、不占缓存。
    if (!registry->declares(topic)) {
        return;
    }
    // 这两族都是应用频道, 以应用身份 (发布者为空) 写入
    std::string error;
    if (!registry->publish(topic, payloadJson, std::string{}, &error) && !error.empty()) {
        XX_LOGD("bus topic `{}` not mirrored to broadcast: {}", topic, error);
    }
}

void PluginManager::forwardBroadcastToBuses(std::string_view id, std::string_view payloadJson) {
    // 只有 `plugin.` 段要转发: 端点按这个前缀订阅总线 (见
    // SessionServerAgentIO::subscribePluginEvents), 应用频道本来就发在总线上
    if (id.empty() || !id.starts_with("plugin.")) {
        return;
    }
    std::vector<std::shared_ptr<agentxx::events::EventBus>> buses;
    auto collect = [&buses](const std::shared_ptr<agentxx::agent::AgentContext>& ctx) {
        if (!ctx || !ctx->bus) {
            return;
        }
        for (const auto& known : buses) {
            if (known == ctx->bus) {
                return;
            }
        }
        buses.push_back(ctx->bus);
    };
    for (const auto& mount : mounts()) {
        if (mount) {
            collect(mount->agentContext());
        }
    }
    collect(primaryAgent_.lock());
    for (const auto& bus : buses) {
        publishOnBus(bus, id, payloadJson);
    }
}

void PluginManager::publishOnBus(
    const std::shared_ptr<agentxx::events::EventBus>& bus,
    std::string_view                                  topic,
    std::string_view                                  payloadJson
) {
    if (!bus || topic.empty()) {
        return;
    }
    // 投递到总线 executor: 调用方可能来自插件线程 / 工作线程, 不能在别人的栈上跑总线派发
    asio::co_spawn(
        bus->executor(),
        [bus, topicCopy = std::string{topic}, payload = std::string{payloadJson}](
        ) -> asio::awaitable<void> {
            co_await bus->publish(topicCopy, payload);
            co_return;
        },
        asio::detached
    );
}

void PluginManager::detachEventBuses() {
    std::lock_guard lock(busBridgeMutex_);
    for (auto& entry : busBridges_) {
        if (auto bus = entry.bus.lock(); bus) {
            if (entry.agentPrefixId != 0) {
                bus->unlistenPrefix(entry.agentPrefixId);
            }
            if (entry.clientPrefixId != 0) {
                bus->unlistenPrefix(entry.clientPrefixId);
            }
        }
    }
    busBridges_.clear();
}

} // namespace plugin
} // namespace agentxx
