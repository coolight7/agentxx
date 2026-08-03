#include "agentxx/agent/agent_io.h"

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
    if (!transport_) {
        co_return;
    }
    while (transport_->alive()) {
        auto msg = co_await transport_->recv();
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
        XX_LOGE("[io] sendToPeer without transport, message dropped (variant index {})", msg.index());
        return;
    }
    transport_->send(std::move(msg));
}

// ---------------------------------------------------------------------------
// 默认命令实现 (经 transport 发送)
// ---------------------------------------------------------------------------

void AgentIOBase::requestCancel(std::string_view threadId) {
    sendToPeer(WireCancel{std::string{threadId}});
}

void AgentIOBase::requestSelectModel(std::string_view threadId, std::string_view model) {
    sendToPeer(WireSelectModel{std::string{threadId}, std::string{model}});
}

void AgentIOBase::requestAppendComponentInfo(std::string_view threadId) {
    sendToPeer(WireGetAppendComponentInfo{std::string{threadId}});
}

void AgentIOBase::sendUserInput(std::string_view threadId, std::string_view text) {
    sendToPeer(WireUserInput{std::string{threadId}, std::string{text}});
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
            inputItem.label  = req.toolName + " " + req.category;
            inputItem.depict = req.target;
            inputItem.type   = "bool";
            inputItem.defaultValue = "no";

            auto arg     = agentxx::middleware::InterruptHandleArg{};
            arg.name     = "permission";
            arg.inputs   = {std::move(inputItem)};
            arg.resultId = "";

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
