#pragma once

#include "agentxx/agent/deepagent.h"
#include "agentxx/agent/remote/message_transport.h"
#include "agentxx/agent/remote/session_controller.h"
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
namespace remote {

/// DeepAgent 服务
/// - 单 io_context/单线程多协程: 复用 DeepAgent.ioCtx
/// - 每个 threadId 一个 SessionController (与连接解耦, 支持断线 grace 重挂 + 增量重放)
/// - 两种接入方式:
///   - WS/WSS 服务: start(ex) 启动 HttpServer, 每个 WS 连接 -> serveTransport
///   - 进程内: 直接 serveTransport(ChannelTransport) 服务单个进程内连接 (统一本地/远程路径)
/// - 安全: WS 模式默认仅监听 127.0.0.1; 强制 token 鉴权 (可经 autoGenerateToken 关闭, 仅进程内用)
class AgentServer {
public:

    struct Config {
        util::HttpServer::Config http; // address 缺省 "0.0.0.0" 时改为 "127.0.0.1"
        std::string              wsPath = "/agent";
        std::string              token;                       // 空且 autoGenerateToken 时自动生成
        /// 进程内可信连接可关闭鉴权 (token 留空即不校验)
        bool                     autoGenerateToken = true;
        std::chrono::seconds     interruptTimeout{300};
        std::chrono::seconds     permissionTimeout{300};
        std::chrono::seconds     gracePeriod{30};             // 断线重挂宽限期
        size_t                   deltaBufferCap = 4096;
    };

    AgentServer(std::shared_ptr<DeepAgent> agent, Config config);

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

    /// 在给定传输上服务一个连接 (WS handler 与进程内 ChannelTransport 共用)
    /// - 须在 agent 的 executor 上 co_await
    asio::awaitable<void> serveTransport(std::unique_ptr<MessageTransport> transport);

    /// 生成随机 hex token
    static std::string generateToken(size_t bytes = 24);

private:

    asio::awaitable<void> handleWs(util::HttpServer::WsStream& ws);
    asio::awaitable<void> handleWss(util::HttpServer::WssStream& ws);

    /// 模板: 在已升级的 WS stream 上建立传输并交由 serveTransport
    template<typename WsStream>
    asio::awaitable<void> serveConnection(WsStream& ws);

    /// 取/建指定 threadId 的 SessionController (并启动其驱动循环)
    std::shared_ptr<SessionController> getOrCreateController(const std::string& threadId);

    std::shared_ptr<DeepAgent>        agent_;
    Config                            config_;
    std::unique_ptr<util::HttpServer> http_;
    asio::any_io_executor             ex_;

    std::mutex                                                  controllersMutex_;
    std::map<std::string, std::shared_ptr<SessionController>>   controllers_;
};

} // namespace remote
} // namespace agent
} // namespace agentxx
