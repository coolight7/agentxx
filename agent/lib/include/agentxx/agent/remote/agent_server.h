#pragma once

#include "agentxx/agent/deepagent.h"
#include "agentxx/util/http_server.h"
#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include <chrono>
#include <memory>
#include <string>

namespace agentxx {
namespace agent {
namespace remote {

/// DeepAgent WS 服务
/// - 单 io_context/单线程多协程: 复用 DeepAgent.ioCtx, 经 HttpServer::startAsync 启动
/// - 每个 WS 连接 -> 一个 RemoteServerAgentIO -> 驱动对话轮次
/// - 安全: 默认仅监听 127.0.0.1; 强制 token 鉴权 (未提供则自动生成); 可选 wss
class AgentServer {
public:

    struct Config {
        util::HttpServer::Config http; // address 缺省 "0.0.0.0" 时改为 "127.0.0.1"
        std::string              wsPath = "/agent";
        std::string              token;                       // 空则自动生成
        std::chrono::seconds     interruptTimeout{300};
        std::chrono::seconds     authTimeout{15};
    };

    AgentServer(std::shared_ptr<DeepAgent> agent, Config config);

    AgentServer(const AgentServer&)            = delete;
    AgentServer& operator=(const AgentServer&) = delete;

    ~AgentServer();

    /// 在指定 executor (应为 agent 的 io_context) 上启动 accept loop
    /// - 须在 co_await agent->init() 之后调用, 避免连接早于引擎初始化
    void start(asio::any_io_executor ex);

    void stop();

    uint16_t port() const;

    const std::string& token() const noexcept {
        return config_.token;
    }

    /// 生成随机 hex token
    static std::string generateToken(size_t bytes = 24);

private:

    asio::awaitable<void> handleWs(util::HttpServer::WsStream& ws);

    std::shared_ptr<DeepAgent>      agent_;
    Config                          config_;
    std::unique_ptr<util::HttpServer> http_;
};

} // namespace remote
} // namespace agent
} // namespace agentxx
