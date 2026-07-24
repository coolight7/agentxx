#include "agentxx/agent/remote/agent_server.h"

#include "agentxx/agent/remote/remote_server_io.h"
#include "agentxx/agent/remote/ws_transport.h"
#include "agentxx/util/log.h"
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
    http_->enableWebSocket(config_.wsPath, [this](util::HttpServer::WsStream& ws) {
        return handleWs(ws);
    });
    http_->startAsync(ex);
    XX_OUT(
        "[agent_server] deepagent WS service on {}:{} (path={}, token={})",
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
}

uint16_t AgentServer::port() const {
    return http_ ? http_->port() : 0;
}

asio::awaitable<void> AgentServer::handleWs(util::HttpServer::WsStream& ws) {
    auto ex = co_await asio::this_coro::executor;

    auto transport = std::make_unique<ServerWsTransport>(ws);

    RemoteServerAgentIO::Config ioCfg;
    ioCfg.token            = config_.token;
    ioCfg.interruptTimeout = config_.interruptTimeout;
    ioCfg.authTimeout      = config_.authTimeout;
    if (agent_ && agent_->agentContext && agent_->agentContext->agentConfig) {
        for (const auto& [name, mc] : agent_->agentContext->agentConfig->availableModels) {
            ioCfg.models.push_back(name);
        }
    }

    auto io = std::make_shared<RemoteServerAgentIO>(ex, std::move(transport), std::move(ioCfg));

    std::weak_ptr<RemoteServerAgentIO> weakIo = io;
    auto                               agent  = agent_;
    io->setCancelCallback([weakIo, agent]() {
        auto self = weakIo.lock();
        if (!self || !agent || !agent->agentContext) {
            return;
        }
        auto session = agent->agentContext->getSession(self->threadId());
        if (session) {
            if (auto tok = session->getCancelToken()) {
                tok->cancel();
            }
        }
    });
    io->setSelectModelCallback([weakIo, agent](const std::string& model) {
        auto self = weakIo.lock();
        if (!self || !agent) {
            return;
        }
        agent->selectModel(self->threadId(), model);
    });

    co_await io->run(agent_);
}

} // namespace remote
} // namespace agent
} // namespace agentxx
