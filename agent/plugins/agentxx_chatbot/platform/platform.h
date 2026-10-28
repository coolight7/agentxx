#pragma once

/// 平台适配层接口 (`IPlatform`)
///
/// 调度层与平台层之间只有两个数据结构: [InboundEvent] (平台 → 调度层) 与
/// [OutboundMessage] (调度层 → 平台)。加新平台只需实现本接口, 调度层一行不改
/// (见 `resource/history/chatbot/plan.md` §4.1 / §13)。
///
/// 线程约定: 平台实现与调度层共用**同一条插件 io 线程** (见 `ChatbotEngine`),
/// 因此回调可以直接改调度状态; 唯一需要 post 的是主程序 io 线程上的通道事件回调。
#include "../chatbot_config.h"
#include "../chatbot_types.h"

#include "asio/any_io_executor.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace asio = ::boost::asio;

namespace agentxx {
namespace chatbot {

/// 平台回调入口 (由调度层实现, 平台实现持有引用)
struct PlatformEventSink {
    /// 收到一条已归一化的消息
    std::function<void(InboundEvent)> onInbound;
    /// 连接状态变化 (原因用于日志与 `chatbot_status`)
    std::function<void(PlatformState, std::string)> onStateChanged;
};

/// 一次出站的结果回调
using SendResultFn = std::function<void(const SendResult&)>;

/// 一个平台账号的连接
class IPlatform {
public:

    virtual ~IPlatform() = default;

    /// 平台类型 (`qq` / `fake` / 将来的 `wechat`)
    virtual std::string type() const = 0;

    virtual PlatformCaps caps() const = 0;

    /// 建连 (异步; 结果经 `sink` 体现)。返回 false 表示参数不可用 (缺凭证等)
    ///
    /// - 只能在插件 io 线程调用; 实现内部用 `ex` 上的定时器/协程推进
    virtual bool start(const AccountConfig& cfg, PlatformEventSink& sink) = 0;

    /// 断开并释放自管资源 (幂等)
    virtual void stop() = 0;

    /// 串行提交一条出站 (同会话顺序由调度层保证)
    ///
    /// - `done` 可能同步调用, 也可能稍后在该 io 线程上调用
    virtual void send(OutboundMessage msg, SendResultFn done) = 0;

    /// 下载一个入站附件并回填 `localPath` (失败时保留 `url`, 由模型自己拉)
    ///
    /// - 默认空实现 = 一律直给 URL (平台能力表里没有下载实现时)
    virtual void downloadAttachment(const std::string& accountId, MediaRef& ref) {
        (void)accountId;
        (void)ref;
    }

    /// 连接状态与计数 JSON (`chatbot_status` 的数据源之一)
    virtual std::string statusJson() const = 0;

    /// 主动发一条健康检查 (仅假平台/调试用, 默认空实现)
    virtual void poke() {}
};

/// 平台工厂: 按 `accounts[].type` 造一个连接
///
/// - `ex`: 插件 io 执行器 (网络与定时器都挂在它上面)
/// - 不认识的类型返回 nullptr
std::unique_ptr<IPlatform>
    createPlatform(std::string_view type, asio::any_io_executor ex, std::string_view dataDir);

} // namespace chatbot
} // namespace agentxx
