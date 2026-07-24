#pragma once

#include "agentxx/agent/deepagent.h"
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

/// DeepAgent WS 服务
/// - 单 io_context/单线程多协程: 复用 DeepAgent.ioCtx, 经 HttpServer::startAsync 启动
/// - 每个 threadId 一个 SessionController (与连接解耦, 支持断线 grace 重挂 + 增量重放)
/// - 每个 WS 连接 -> 一个 RemoteServerAgentIO (瘦 WS 泵) 绑定到 SessionController
/// - 安全: 默认仅监听 127.0.0.1; 强制 token 鉴权; 配置 SSL 证书则启用 wss
class AgentServer {
public:

    struct Config {
        util::HttpServer::Config http; // address 缺省 "0.0.0.0" 时改为 "127.0.0.1"
        std::string              wsPath = "/agent";
        std::string              token;                       // 空则自动生成
        std::chrono::seconds     interruptTimeout{300};
        std::chrono::seconds     permissionTimeout{300};
        std::chrono::seconds     gracePeriod{30};             // 断线重挂宽限期
        size_t                   deltaBufferCap = 4096;
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
    asio::awaitable<void> handleWss(util::HttpServer::WssStream& ws);

    /// 模板: 在已升级的 WS stream 上建立连接泵并绑定 SessionController
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
