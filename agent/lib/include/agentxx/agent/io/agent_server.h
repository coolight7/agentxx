#pragma once

#include "agentxx/agent/base_agent.h"
#include "agentxx/agent/io/agent_io_transport.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/util/http_server.h"
#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace agentxx {
namespace agent {
namespace io {

/// Agent 服务
/// - 单 io_context/单线程多协程: 复用 BaseAgent.ioCtx
/// - 每个 threadId 一个 SessionServerAgentIO (与连接解耦, 支持断线 grace 重挂 + 增量重放)
/// - 两种接入方式:
///   - WS/WSS 服务: start(ex) 启动 HttpServer, 每个 WS 连接 -> serveTransport
///   - 进程内: 直接 serveTransport(ChannelAgentIOTransport) 服务单个进程内连接
/// - 安全: WS 模式默认强制 token 鉴权 (可经 autoGenerateToken 关闭, 仅进程内用)
class AgentServer {
public:

    struct Config {
        inline static const std::string defaultBasePath = "/agent";

        util::HttpServer::Config http;  // address
        std::string              token; // 空且 autoGenerateToken 时自动生成
        /// 进程内可信连接可关闭鉴权 (token 留空即不校验)
        bool                 autoGenerateToken = true;
        /// 中断/权限等待客户端响应的超时; <=0 表示不限制 (无限等待用户响应)
        std::chrono::milliseconds interruptTimeout{0};
        std::chrono::seconds      gracePeriod{30}; // 断线重挂等待期
        size_t                    deltaBufferCap = 4096;
    };

    AgentServer(std::shared_ptr<BaseAgent> agent, Config config);

    AgentServer(const AgentServer&)            = delete;
    AgentServer& operator=(const AgentServer&) = delete;

    ~AgentServer();

    /// WS/WSS 模式: 在指定 executor (应为 agent 的 io_context) 上启动 accept loop
    /// - 须在 co_await agent->init() 之后调用, 避免连接早于引擎初始化
    void start(asio::any_io_executor ex);

    void stop();

    uint16_t port() const;

    const std::string& token() const noexcept {
        return config_.token;
    }

    /// 在给定传输上服务一个连接 (WS handler 与进程内 ChannelAgentIOTransport 共用)
    /// - 须在 agent 的 executor 上 co_await
    asio::awaitable<void> serveTransport(std::shared_ptr<AgentIOTransportBase> transport);

    /// 生成随机 hex token
    static std::string generateToken(size_t bytes = 24);

private:

    asio::awaitable<void> handleWs(util::HttpServer::WsStream& ws);
    asio::awaitable<void> handleWss(util::HttpServer::WssStream& ws);

    /// 取/建指定 threadId 的 SessionServerAgentIO (并启动其驱动循环)
    std::shared_ptr<SessionServerAgentIO> getOrCreateController(std::string_view threadId);

    std::shared_ptr<BaseAgent>        agent_;
    Config                            config_;
    std::unique_ptr<util::HttpServer> http_;
    asio::any_io_executor             ex_;

    // 控制器映射：单线程访问，无需锁
    std::map<std::string, std::shared_ptr<SessionServerAgentIO>, std::less<>> controllers_;
};

} // namespace io
} // namespace agent
} // namespace agentxx
