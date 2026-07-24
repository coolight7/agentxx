#pragma once

#include "agentxx/util/ws_client.h"
#include "asio/awaitable.hpp"
#include <expected>
#include <string>
#include <string_view>

namespace agentxx {
namespace agent {
namespace remote {

/// 传输抽象: 双向 JSON 文本消息通道
/// - 实现: ServerWsTransport (beast websocket stream) / ClientWsTransport (WsClient)
/// - 后续可扩展 ChannelTransport (进程内线程间, 免序列化)
/// - 语义: 至多一个 outstanding recv + 一个 outstanding send (与 beast websocket 一致)
class MessageTransport {
public:

    virtual ~MessageTransport() = default;

    /// 发送一条 JSON 文本帧
    virtual asio::awaitable<std::expected<void, std::string>> send(std::string_view jsonText) = 0;

    /// 接收一条消息 (数据帧); 连接关闭/错误时返回错误
    virtual asio::awaitable<std::expected<util::WsMessage, std::string>> recv() = 0;

    /// 关闭底层连接 (线程安全; 会使挂起的 recv/send 以错误返回)
    virtual void close() = 0;

    virtual bool isOpen() const noexcept = 0;
};

} // namespace remote
} // namespace agent
} // namespace agentxx
