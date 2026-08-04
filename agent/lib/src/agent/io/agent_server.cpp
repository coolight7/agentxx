#include "agentxx/agent/io/agent_server.h"

#include "agentxx/agent/io/ws_io_transport.h"
#include "agentxx/util/log.h"
#include "agentxx/util/ws_client.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/this_coro.hpp"
#include <random>

namespace agentxx {
namespace agent {
namespace io {

/// 将服务端日志经 transport 转发给远程客户端的 LogSink
/// 继承 ThreadedLogSink: 后台线程串行消费, send 无需额外加锁
class TransportLogSink : public util::ThreadedLogSink {
public:

    explicit TransportLogSink(std::shared_ptr<AgentIOTransportBase> transport) :
        transport_(std::move(transport)) {}

    void onLog(const util::LogEntry& entry) override {
        if (auto t = transport_.lock()) {
            t->send(WireLog{static_cast<int>(entry.level), entry.message});
        }
    }

private:

    std::weak_ptr<AgentIOTransportBase> transport_;
};

AgentServer::AgentServer(std::shared_ptr<BaseAgent> agent, Config config) :
    agent_(std::move(agent)),
    config_(std::move(config)) {
    if (config_.http.address.empty()) {
        config_.http.address = "0.0.0.0";
    }
    if (config_.token.empty() && config_.autoGenerateToken) {
        config_.token = generateToken();
    }
}

AgentServer::~AgentServer() {
    stop();
}

std::string AgentServer::generateToken(size_t bytes) {
    static const char* hex = "0123456789abcdef";
    std::random_device rd;
    std::string        token;
    token.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        unsigned r = rd();
        token.push_back(hex[(r >> 4) & 0xFu]);
        token.push_back(hex[r & 0xFu]);
    }
    return token;
}

void AgentServer::start(asio::any_io_executor ex) {
    ex_      = ex;
    http_    = std::make_unique<util::HttpServer>(config_.http);
    bool ssl = !config_.http.sslCertFile.empty() && !config_.http.sslKeyFile.empty();
    if (ssl) {
        http_->enableWebSocketSsl(config_.defaultBasePath, [this](util::HttpServer::WssStream& ws) {
            return handleWss(ws);
        });
    } else {
        http_->enableWebSocket(config_.defaultBasePath, [this](util::HttpServer::WsStream& ws) {
            return handleWs(ws);
        });
    }
    http_->startAsync(ex);
    XX_OUT(
        "[agent_server] BaseAgent {} service on {}:{}{} (token={})",
        ssl ? "WSS" : "WS",
        config_.http.address,
        port(),
        config_.defaultBasePath,
        config_.token
    );
}

void AgentServer::stop() {
    if (http_) {
        http_->stop();
    }
    // 无锁遍历关闭
    for (auto& [id, ctrl] : controllers_) {
        ctrl->stop();
    }
    controllers_.clear();
}

uint16_t AgentServer::port() const {
    return http_ ? http_->port() : 0;
}

std::shared_ptr<SessionServerAgentIO> AgentServer::getOrCreateController(std::string_view threadId
) {
    auto it = controllers_.find(threadId); // 无锁查找
    if (it != controllers_.end()) {
        return it->second;
    }

    SessionServerAgentIO::Config cfg;
    cfg.threadId         = std::string{threadId};
    cfg.interruptTimeout = config_.interruptTimeout;
    cfg.gracePeriod      = config_.gracePeriod;
    cfg.deltaBufferCap   = config_.deltaBufferCap;

    auto ctrl                           = std::make_shared<SessionServerAgentIO>(ex_, agent_, cfg);
    controllers_[std::string{threadId}] = ctrl;

    // threadId 以 std::string 值捕获: string_view 参数引用的原始字符串
    // (serveTransport 的局部 WireHello) 可能在本协程完成前已析构
    asio::co_spawn(ex_, ctrl->run(), [ctrl, threadId = std::string{threadId}](std::exception_ptr ep) {
        if (ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                XX_LOGE("[agent_server] controller '{}' error: {}", threadId, e.what());
            }
        }
    });
    return ctrl;
}

asio::awaitable<void> AgentServer::handleWs(util::HttpServer::WsStream& ws) {
    auto ex     = co_await asio::this_coro::executor;
    auto client = util::wrapAcceptedWs(ex, std::move(ws));
    auto transport
        = std::make_shared<WsAgentIOTransport>(ex, std::move(client), WsAgentIOTransport::Config{});
    co_await serveTransport(std::move(transport));
}

asio::awaitable<void> AgentServer::handleWss(util::HttpServer::WssStream& ws) {
    auto ex     = co_await asio::this_coro::executor;
    auto client = util::wrapAcceptedWss(ex, std::move(ws));
    auto transport
        = std::make_shared<WsAgentIOTransport>(ex, std::move(client), WsAgentIOTransport::Config{});
    co_await serveTransport(std::move(transport));
}

asio::awaitable<void> AgentServer::serveTransport(std::shared_ptr<AgentIOTransportBase> transport) {
    // 服务端模式初始化: 启动读写循环, 不发送 hello
    WireHello dummyHello;
    bool      initOk = co_await transport->connect(dummyHello);
    if (!initOk) {
        co_return;
    }

    // 等待客户端 hello
    auto msg = co_await transport->recv();
    if (!msg) {
        co_return;
    }
    auto* hello = std::get_if<WireHello>(&*msg);
    if (!hello) {
        co_return;
    }

    // 鉴权
    bool authOk = config_.token.empty() || hello->token == config_.token;
    if (!authOk) {
        transport->send(WireHelloAck{.ok = false, .threadId = hello->threadId});
        co_return;
    }

    auto ctrl = getOrCreateController(hello->threadId);

    // 收集可用模型
    std::vector<std::string> models;
    if (agent_ && agent_->agentContext && agent_->agentContext->agentConfig) {
        for (const auto& [name, mc] : agent_->agentContext->agentConfig->availableModels) {
            models.push_back(name);
        }
    }

    // 同一 threadId 已有旧连接时, 先关闭旧 transport:
    // 否则旧连接的 runTransportLoop 协程继续存活, 两个接收循环并发处理
    // 同一会话的消息 (输入重复执行/消息交错), 且旧 transport 持续占用资源
    if (auto oldTransport = ctrl->transport(); oldTransport && oldTransport.get() != transport.get()) {
        oldTransport->close();
    }

    // 绑定 transport 到 controller, 发送 helloAck + 增量重放
    ctrl->setTransport(transport);
    ctrl->handleHello(*hello, std::move(models));

    // 注册日志转发 sink: 将服务端日志经 transport 推送给远程客户端
    auto logSink = std::make_shared<TransportLogSink>(transport);
    util::LogDispatcher::instance().addSink(logSink);

    // 运行接收循环 (直到 transport 关闭)
    co_await ctrl->runTransportLoop();

    // 连接断开
    util::LogDispatcher::instance().removeSink(logSink);
    ctrl->onDisconnect();
}

} // namespace io
} // namespace agent
} // namespace agentxx
