#include "agentxx/agent/agent_io.h"

#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/events.h"
#include "agentxx/middlewares/middleware.h"

namespace agentxx {
namespace agent {

AgentIOBase::~AgentIOBase() {
    // IO 销毁前移除总线上的处理器, 避免 handler 持有悬空 this
    unregisterFromBus();
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
