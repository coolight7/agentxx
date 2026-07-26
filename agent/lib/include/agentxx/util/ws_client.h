#pragma once

#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "asio/awaitable.hpp"
#include "asio/cancel_after.hpp"
#include "asio/ip/tcp.hpp"
#include "asio/ssl/context.hpp"
#include "asio/ssl/stream.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <chrono>
#include <expected>
#include <memory>
#include <neograph/api.h>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace util {

struct WsMessage {
    enum class Type : uint8_t {
        Text,
        Binary,
        Close,
    };

    Type        type = Type::Text;
    std::string payload;
    uint16_t    closeCode = 0;
};

struct WsClientConfig {
    std::chrono::milliseconds connectTimeout = std::chrono::seconds{10};
    std::chrono::milliseconds recvTimeout    = std::chrono::seconds{60};
    std::chrono::milliseconds sendTimeout    = std::chrono::seconds{30};
    bool                      sslVerify      = false;
    size_t                    maxMessageSize = 16 * 1024 * 1024;
};

class WsClient {
public:

    struct Impl;

    ~WsClient();
    WsClient(const WsClient&)            = delete;
    WsClient& operator=(const WsClient&) = delete;
    WsClient(WsClient&&) noexcept;
    WsClient& operator=(WsClient&&) noexcept;

    explicit WsClient(std::unique_ptr<Impl> impl);

    asio::awaitable<std::expected<void, std::string>> sendText(std::string_view payload);

    asio::awaitable<std::expected<void, std::string>> sendBinary(std::string_view payload);

    asio::awaitable<std::expected<void, std::string>> sendPing(std::string_view payload = "");

    asio::awaitable<std::expected<void, std::string>>
        sendClose(uint16_t code = 1000, std::string_view reason = "");

    asio::awaitable<std::expected<WsMessage, std::string>> recv();

    bool isOpen() const noexcept;

    void setRecvTimeout(std::chrono::milliseconds timeout) noexcept;

    /// 关闭底层 socket, 使挂起的 recv/send 以错误返回
    /// - 须在 WsClient 绑定的 executor 上调用 (与 recv/send 同线程)
    /// - 调用后 recv 会立即以错误返回, 此后方可安全析构 WsClient
    void abort() noexcept;

private:

    std::unique_ptr<Impl> impl_;
};

asio::awaitable<std::expected<std::unique_ptr<WsClient>, std::string>> wsConnect(
    asio::any_io_executor                            executor,
    std::string_view                                 url,
    std::vector<std::pair<std::string, std::string>> headers = {},
    WsClientConfig                                   config  = {}
);

/// 从已 accept 的服务端 WS stream 创建 WsClient (供 AgentServer 使用)
std::unique_ptr<WsClient> wrapAcceptedWs(
    asio::any_io_executor                                              ex,
    boost::beast::websocket::stream<boost::beast::tcp_stream>          ws,
    WsClientConfig                                                     config = {}
);

std::unique_ptr<WsClient> wrapAcceptedWss(
    asio::any_io_executor ex,
    boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>> wss,
    WsClientConfig        config = {}
);

} // namespace util
} // namespace agentxx
