#include "agentxx/agent/io/agent_io.h"

#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/events.h"
#include "agentxx/middlewares/middleware.h"

namespace agentxx {
namespace agent {

AgentIOBase::~AgentIOBase() {
    unregisterFromBus();
}

// ---------------------------------------------------------------------------
// Transport 管理
// ---------------------------------------------------------------------------

void AgentIOBase::setTransport(std::shared_ptr<AgentIOTransportBase> transport) {
    transport_ = std::move(transport);
}

std::shared_ptr<AgentIOTransportBase> AgentIOBase::transport() const noexcept {
    return transport_;
}

asio::awaitable<void> AgentIOBase::runTransportLoop() {
    // 捕获局部 transport 引用: 服务端同一 threadId 的新连接替换旧连接
    // (AgentServer::serveTransport 中 setTransport) 时, 本协程应继续消费旧
    // transport 直至其关闭自然退出, 而不是跟随成员 transport_ 切换到新
    // transport 上发起第二个接收循环 (消息被两个循环瓜分 / 协程泄漏)
    auto transport = transport_;
    if (!transport) {
        co_return;
    }
    while (transport->alive()) {
        auto msg = co_await transport->recv();
        if (!msg.has_value()) {
            break;
        }
        onPeerMessage(std::move(*msg));
    }
}

void AgentIOBase::sendToPeer(WireMessage msg) {
    if (!transport_) {
        // 端点间通信强制要求 transport; 走到这里说明装配遗漏 (如未 setTransport),
        // 记录错误便于定位, 避免静默丢消息
        XX_LOGE(
            "[io] sendToPeer without transport, message dropped (variant index {})",
            msg.index()
        );
        return;
    }
    transport_->send(std::move(msg));
}

// ---------------------------------------------------------------------------
// 默认命令实现 (经 transport 发送)
// ---------------------------------------------------------------------------

void AgentIOBase::requestCancel(std::string threadId) {
    sendToPeer(WireCancel{std::move(threadId)});
}

void AgentIOBase::requestSelectModel(std::string threadId, std::string model) {
    sendToPeer(WireSelectModel{std::move(threadId), std::move(model)});
}

void AgentIOBase::requestAppendComponentInfo(std::string threadId) {
    sendToPeer(WireGetAppendComponentInfo{std::move(threadId)});
}

void AgentIOBase::sendUserInput(std::string threadId, std::string text) {
    sendToPeer(WireUserInput{std::move(threadId), std::move(text)});
}

// ---------------------------------------------------------------------------
// 默认消息分发 (子类覆写以扩展)
// ---------------------------------------------------------------------------

void AgentIOBase::onPeerMessage(WireMessage msg) {
    std::visit(
        [this](auto&& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, Delta>) {
                onDelta(m);
            } else if constexpr (std::is_same_v<T, SyncPayload>) {
                onSync(m);
            } else if constexpr (std::is_same_v<T, WireTurnResult>) {
                onTurnResult(m);
            } else if constexpr (std::is_same_v<T, WireContextStats>) {
                onContextStats(m);
            }
        },
        std::move(msg)
    );
}

void AgentIOBase::unregisterFromBus() {
    auto bus = registeredBus_.lock();
    if (!bus) {
        return;
    }
    if (interruptServerId_ != 0) {
        bus->getRR<events::ReqInterrupt, events::RespInterrupt>(events::Topic::Interrupt)
            .removeServer(interruptServerId_);
        interruptServerId_ = 0;
    }
    if (permissionServerId_ != 0) {
        bus->getRR<events::ReqPermission, events::RespPermission>(events::Topic::Permission)
            .removeServer(permissionServerId_);
        permissionServerId_ = 0;
    }
}

void AgentIOBase::registerOnBus(std::shared_ptr<agentxx::middleware::EventBus> sessionBus) {
    if (!sessionBus) {
        return;
    }

    // 重复注册时先移除旧处理器, 避免 handler 累积/泄漏, 以及旧 IO 销毁后悬空 this
    unregisterFromBus();
    registeredBus_ = sessionBus;

    // 注册 interrupt 处理器
    auto& interruptRR
        = sessionBus->getRR<events::ReqInterrupt, events::RespInterrupt>(events::Topic::Interrupt);
    interruptServerId_ = interruptRR.serve(
        [this](const events::ReqInterrupt& req, size_t /*corrId*/)
            -> asio::awaitable<events::RespInterrupt> {
            auto result = co_await this->handleInterrupt(
                req.threadId,
                req.interruptNode,
                req.interruptValue,
                req.interruptArgsJson
            );
            co_return events::RespInterrupt{
                .handled    = true,
                .resultJson = result.dump(),
            };
        }
    );

    // 注册 permission 处理器
    auto& permRR
        = sessionBus->getRR<events::ReqPermission, events::RespPermission>(events::Topic::Permission
        );
    permissionServerId_ = permRR.serve(
        [this](const events::ReqPermission& req, size_t /*corrId*/)
            -> asio::awaitable<events::RespPermission> {
            auto inputItem   = agentxx::middleware::InterruptHandleArg::InterruptHandleInputItem{};
            inputItem.label  = fmt::format("{} {}", req.toolName, req.category);
            inputItem.depict = req.target;
            inputItem.type   = "bool";
            inputItem.defaultValue = "no";

            auto arg     = agentxx::middleware::InterruptHandleArg{};
            arg.name     = "permission";
            arg.inputs   = {std::move(inputItem)};
            arg.resultId = "";
            // 透传权限上下文给客户端 (记住权限选择时使用):
            // - category: 权限分类 ("filesystem_read" / "filesystem_write")
            // - target:   受约束目标 (已标准化的绝对路径, 与中间件规则匹配口径一致)
            arg.arg = neograph::json{
                {"category", req.category},
                {"target",   req.target  },
            };

            auto result = co_await this->handleInterrupt(
                req.threadId,
                "permission",
                req.argumentsJson,
                arg.toJson().dump()
            );

            bool allowed = false;
            if (result.is_array() && !result.empty()) {
                auto val = result[0];
                if (val.is_string()) {
                    allowed = (val.get<std::string>() == "true" || val.get<std::string>() == "yes");
                } else if (val.is_boolean()) {
                    allowed = val.get<bool>();
                }
            }
            co_return events::RespPermission{
                .decision = allowed ? events::RespPermission::Decision::Allow
                                    : events::RespPermission::Decision::Deny,
                .reason   = allowed ? "" : "user denied",
            };
        }
    );
}

} // namespace agent
} // namespace agentxx
