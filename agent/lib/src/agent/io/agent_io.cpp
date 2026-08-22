#include "agentxx/agent/io/agent_io.h"

#include "agentxx/event/event_stream.h"
#include "agentxx/event/events.h"
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

void AgentIOBase::setEventSink(std::shared_ptr<ClientEventSink> sink) {
    eventSink_ = std::move(sink);
}

asio::awaitable<void> AgentIOBase::runTransportLoop() {
    // 捕获局部 transport 引用: 服务端同一 sessionId 的新连接替换旧连接
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

void AgentIOBase::requestCancel(std::string sessionId) {
    sendToPeer(WireCancel{std::move(sessionId)});
}

void AgentIOBase::requestSelectModel(std::string sessionId, std::string model) {
    sendToPeer(WireSelectModel{std::move(sessionId), std::move(model)});
}

void AgentIOBase::requestAppendComponentInfo(std::string sessionId) {
    sendToPeer(WireGetAppendComponentInfo{std::move(sessionId)});
}

void AgentIOBase::requestViewMessagesPage(
    std::string sessionId,
    uint64_t    beforeIndex,
    uint32_t    count
) {
    sendToPeer(WireGetViewMessages{std::move(sessionId), beforeIndex, count});
}

void AgentIOBase::sendUserInput(std::string sessionId, std::string text) {
    // 通知事件接收器 (client 插件系统订阅用户输入事件)
    emitEventSink([&](ClientEventSink& sink) {
        sink.onUserInput(sessionId, text);
    });
    sendToPeer(WireUserInput{std::move(sessionId), std::move(text)});
}

void AgentIOBase::onServerReady() {
    // 基类默认实现通知事件接收器 (client 插件系统据此开始注册 UI/订阅事件)
    emitEventSink([&](ClientEventSink& sink) {
        sink.onReady();
    });
}

// ---------------------------------------------------------------------------
// 默认消息分发 (子类覆写以扩展)
// ---------------------------------------------------------------------------

void AgentIOBase::onPeerMessage(WireMessage msg) {
    std::visit(
        [this](auto&& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, Delta>) {
                emitEventSink([&](ClientEventSink& sink) {
                    sink.onDelta(m);
                });
                onDelta(m);
            } else if constexpr (std::is_same_v<T, SyncPayload>) {
                onSync(m);
            } else if constexpr (std::is_same_v<T, WireTurnResult>) {
                emitEventSink([&](ClientEventSink& sink) {
                    sink.onTurnResult(m);
                });
                onTurnResult(m);
            } else if constexpr (std::is_same_v<T, WireContextStats>) {
                onContextStats(m);
            } else if constexpr (std::is_same_v<T, WirePluginData>) {
                // 插件事件转发: 通知事件接收器 (client 插件系统据此分发到
                // 订阅 EVT_PLUGIN_DATA 的插件回调)
                emitEventSink([&](ClientEventSink& sink) {
                    sink.onPluginData(m);
                });
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
            .unregisterServer(interruptServerId_);
        interruptServerId_ = 0;
    }
    if (permissionServerId_ != 0) {
        bus->getRR<events::ReqPermission, events::RespPermission>(events::Topic::Permission)
            .unregisterServer(permissionServerId_);
        permissionServerId_ = 0;
    }
}

void AgentIOBase::registerOnBus(std::shared_ptr<agentxx::event::EventBus> sessionBus) {
    if (!sessionBus) {
        return;
    }

    // 重复注册时先移除旧处理器, 避免 handler 累积/泄漏, 以及旧 IO 销毁后悬空 this
    unregisterFromBus();
    registeredBus_ = sessionBus;

    // 注册 interrupt 处理器
    auto& interruptRR
        = sessionBus->getRR<events::ReqInterrupt, events::RespInterrupt>(events::Topic::Interrupt);
    interruptServerId_ = interruptRR.registerServer(
        [this](const events::ReqInterrupt& req, size_t /*corrId*/)
            -> asio::awaitable<events::RespInterrupt> {
            auto result = co_await this->handleInterrupt(
                req.sessionId,
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
    permissionServerId_ = permRR.registerServer(
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
                req.sessionId,
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
