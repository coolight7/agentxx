#include "agentxx/agent/agent_io.h"

#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/events.h"
#include "agentxx/middlewares/middleware.h"

namespace agentxx {
namespace agent {

void AgentIOBase::registerOnBus(std::shared_ptr<agentxx::middleware::EventBus> sessionBus) {
    if (!sessionBus) {
        return;
    }

    // 注册 interrupt 处理器
    // 注意: 调用者须保证 IO 对象存活时间长于 sessionBus (Session 中 io 在 bus 之后销毁)
    auto& interruptRR
        = sessionBus->getRR<events::ReqInterrupt, events::RespInterrupt>(events::Topic::Interrupt);
    interruptRR.serve(
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
    permRR.serve(
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
