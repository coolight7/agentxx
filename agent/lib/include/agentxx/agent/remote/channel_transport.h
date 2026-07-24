#pragma once

#include "agentxx/agent/remote/message_transport.h"
#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include <atomic>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace agentxx {
namespace agent {
namespace remote {

/// 进程内传输: 经一对线程安全 concurrent_channel 双向通信 (免网络/免序列化拷贝之外的开销)
/// - 用于统一本地(client+agent 同进程)与远程路径: 本地用 ChannelTransport, 远程用 WS 传输
/// - makePair 创建互连的两端 (client 端 / server 端), 可跨线程/跨 executor 使用
class ChannelTransport : public MessageTransport {
public:

    using Chan
        = asio::experimental::concurrent_channel<void(boost::system::error_code, std::string)>;

    ChannelTransport(std::shared_ptr<Chan> outgoing, std::shared_ptr<Chan> incoming);

    /// 创建互连的一对传输: first=client 端, second=server 端
    /// - clientEx: client 端接收方 executor; serverEx: server 端接收方 executor
    static std::pair<std::unique_ptr<ChannelTransport>, std::unique_ptr<ChannelTransport>>
        makePair(asio::any_io_executor clientEx, asio::any_io_executor serverEx, size_t cap = 4096);

    asio::awaitable<std::expected<void, std::string>> send(std::string_view jsonText) override;

    asio::awaitable<std::expected<util::WsMessage, std::string>> recv() override;

    void close() override;

    bool isOpen() const noexcept override;

private:

    std::shared_ptr<Chan> outgoing_; // 本端写 -> 对端读
    std::shared_ptr<Chan> incoming_; // 对端写 -> 本端读
    std::atomic<bool>     closed_{false};
};

} // namespace remote
} // namespace agent
} // namespace agentxx
