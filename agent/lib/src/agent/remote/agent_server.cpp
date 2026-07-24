#include "agentxx/agent/remote/agent_server.h"

#include "agentxx/agent/remote/remote_server_io.h"
#include "agentxx/agent/remote/ws_transport.h"
#include "agentxx/util/log.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/this_coro.hpp"
#include <random>

namespace agentxx {
namespace agent {
namespace remote {

AgentServer::AgentServer(std::shared_ptr<DeepAgent> agent, Config config) :
    agent_(std::move(agent)),
    config_(std::move(config)) {
    // 安全默认: 未显式指定地址时仅监听回环
    if (config_.http.address == "0.0.0.0") {
        config_.http.address = "127.0.0.1";
    }
    if (config_.token.empty()) {
        config_.token = generateToken();
    }
    http_ = std::make_unique<util::HttpServer>(config_.http);
}

AgentServer::~AgentServer() {
    stop();
}

std::string AgentServer::generateToken(size_t bytes) {
    static const char* hex = "0123456789abcdef";
    std::random_device rd;
    std::mt19937_64    gen(rd());
    std::uniform_int_distribution<int> dist(0, 15);
    std::string                        token;
    token.reserve(bytes * 2);
    for (size_t i = 0; i < bytes * 2; ++i) {
        token.push_back(hex[dist(gen)]);
    }
    return token;
}

void AgentServer::start(asio::any_io_executor ex) {
    ex_      = ex;
    bool ssl = !config_.http.sslCertFile.empty() && !config_.http.sslKeyFile.empty();
    if (ssl) {
        http_->enableWebSocketSsl(config_.wsPath, [this](util::HttpServer::WssStream& ws) {
            return handleWss(ws);
        });
    } else {
        http_->enableWebSocket(config_.wsPath, [this](util::HttpServer::WsStream& ws) {
            return handleWs(ws);
        });
    }
    http_->startAsync(ex);
    XX_OUT(
        "[agent_server] deepagent {} service on {}:{} (path={}, token={})",
        ssl ? "WSS" : "WS",
        config_.http.address,
        port(),
        config_.wsPath,
        config_.token
    );
}

void AgentServer::stop() {
    if (http_) {
        http_->stop();
    }
    std::lock_guard<std::mutex> lock(controllersMutex_);
    for (auto& [id, ctrl] : controllers_) {
        ctrl->stop();
    }
    controllers_.clear();
}

uint16_t AgentServer::port() const {
    return http_ ? http_->port() : 0;
}

std::shared_ptr<SessionController> AgentServer::getOrCreateController(const std::string& threadId) {
    std::lock_guard<std::mutex> lock(controllersMutex_);
    auto                        it = controllers_.find(threadId);
    if (it != controllers_.end()) {
        return it->second;
    }

    SessionController::Config cfg;
    cfg.threadId          = threadId;
    cfg.interruptTimeout  = config_.interruptTimeout;
    cfg.permissionTimeout = config_.permissionTimeout;
    cfg.gracePeriod       = config_.gracePeriod;
    cfg.deltaBufferCap    = config_.deltaBufferCap;

    auto ctrl            = std::make_shared<SessionController>(ex_, agent_, cfg);
    controllers_[threadId] = ctrl;

    // 启动会话驱动循环 (独立于连接存在)
    asio::co_spawn(
        ex_,
        ctrl->run(),
        [ctrl, threadId](std::exception_ptr ep) {
            if (ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    XX_LOGE("[agent_server] controller '{}' error: {}", threadId, e.what());
                }
            }
        }
    );
    return ctrl;
}

asio::awaitable<void> AgentServer::handleWs(util::HttpServer::WsStream& ws) {
    co_await serveConnection(ws);
}

asio::awaitable<void> AgentServer::handleWss(util::HttpServer::WssStream& ws) {
    co_await serveConnection(ws);
}

template<typename WsStream>
asio::awaitable<void> AgentServer::serveConnection(WsStream& ws) {
    auto ex = co_await asio::this_coro::executor;

    auto transport = std::make_unique<ServerWsTransportT<WsStream>>(ws);

    RemoteServerAgentIO::Config ioCfg;
    ioCfg.token = config_.token;
    if (agent_ && agent_->agentContext && agent_->agentContext->agentConfig) {
        for (const auto& [name, mc] : agent_->agentContext->agentConfig->availableModels) {
            ioCfg.models.push_back(name);
        }
    }

    auto io = std::make_shared<RemoteServerAgentIO>(ex, std::move(transport), std::move(ioCfg));

    std::weak_ptr<RemoteServerAgentIO> weakIo = io;

    // 鉴权通过后绑定到对应 threadId 的 SessionController (含增量重放)
    io->setAuthHandler(
        [this, weakIo](
            const std::string& threadId,
            uint64_t           lastSeq,
            const std::string& tailHash,
            std::string&       outTailHash
        ) -> std::shared_ptr<SessionController> {
            auto conn = weakIo.lock();
            if (!conn) {
                return nullptr;
            }
            auto ctrl   = getOrCreateController(threadId);
            outTailHash = ctrl->currentTailHash();
            ctrl->attach(conn, lastSeq, tailHash);
            return ctrl;
        }
    );
    io->setSelectModelCallback([this, weakIo](const std::string& model) {
        auto conn = weakIo.lock();
        if (!conn || !agent_) {
            return;
        }
        agent_->selectModel(conn->threadId(), model);
    });

    co_await io->run();
}

// 显式实例化两种 WS stream 类型
template asio::awaitable<void> AgentServer::serveConnection(util::HttpServer::WsStream&);
template asio::awaitable<void> AgentServer::serveConnection(util::HttpServer::WssStream&);

} // namespace remote
} // namespace agent
} // namespace agentxx
