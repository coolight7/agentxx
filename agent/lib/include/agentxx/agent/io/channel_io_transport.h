#pragma once

#include "agentxx/agent/io/agent_io_transport.h"
#include "asio/any_io_executor.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include <atomic>
#include <memory>
#include <utility>

namespace agentxx {
namespace agent {

/// 进程内传输: 经 concurrent_channel 直接传递 WireMessage 对象
/// - 零序列化: 消息以 C++ 对象 move 语义传递, 无 JSON 编解码开销
/// - 线程安全: 可跨线程/跨 executor 使用
/// - makePair 创建互连的两端 (client 端 / server 端)
class ChannelAgentIOTransport : public AgentIOTransportBase {
public:

    using Chan
        = asio::experimental::concurrent_channel<void(neograph_asio_error_code, WireMessage)>;

    ChannelAgentIOTransport(std::shared_ptr<Chan> outgoing, std::shared_ptr<Chan> incoming);

    /// 创建互连的一对传输: first=client 端, second=server 端
    static std::
        pair<std::unique_ptr<ChannelAgentIOTransport>, std::unique_ptr<ChannelAgentIOTransport>>
        makePair(asio::any_io_executor clientEx, asio::any_io_executor serverEx, size_t cap = 4096);

    void send(WireMessage msg) override;

    asio::awaitable<std::optional<WireMessage>> recv() override;

    void close() override;

    bool alive() const noexcept override;

private:

    std::shared_ptr<Chan> outgoing_;
    std::shared_ptr<Chan> incoming_;
    std::atomic<bool>     closed_{false};
};

} // namespace agent
} // namespace agentxx
