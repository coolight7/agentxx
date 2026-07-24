#pragma once

#include "agentxx/agent/remote/message_transport.h"
#include "agentxx/util/ws_client.h"
#include "asio/awaitable.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace agentxx {
namespace agent {
namespace remote {

/// 服务端 WS 传输: 包裹 http_server 升级得到的 websocket stream
/// - 构造时配置 keepalive ping (用于检测 client 断线)
/// - recv/send 分别由读/写协程独占调用 (beast 允许 1 读 + 1 写 并发)
class ServerWsTransport : public MessageTransport {
public:

    using WsStream = boost::beast::websocket::stream<boost::beast::tcp_stream>;

    explicit ServerWsTransport(
        WsStream&              ws,
        std::chrono::seconds   keepAliveInterval = std::chrono::seconds{30}
    );

    asio::awaitable<std::expected<void, std::string>> send(std::string_view jsonText) override;

    asio::awaitable<std::expected<util::WsMessage, std::string>> recv() override;

    void close() override;

    bool isOpen() const noexcept override;

private:

    WsStream* ws_;
};

/// 客户端 WS 传输: 包裹 util::WsClient
class ClientWsTransport : public MessageTransport {
public:

    explicit ClientWsTransport(std::unique_ptr<util::WsClient> client);

    asio::awaitable<std::expected<void, std::string>> send(std::string_view jsonText) override;

    asio::awaitable<std::expected<util::WsMessage, std::string>> recv() override;

    void close() override;

    bool isOpen() const noexcept override;

private:

    std::unique_ptr<util::WsClient> client_;
};

} // namespace remote
} // namespace agent
} // namespace agentxx
