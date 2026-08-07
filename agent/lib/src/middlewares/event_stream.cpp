#include "agentxx/middlewares/event_stream.h"

namespace agentxx {
namespace middleware {

EventStreamInterface::EventStreamInterface(
    std::string_view      in_name,
    const std::type_info& elementType
) :
    name(in_name),
    elementType_(elementType) {}

EventBus::EventBus(asio::any_io_executor executor) :
    executor_(executor) {}

bool EventBus::remove(std::string_view topic) {
    auto it = streams_.find(std::string{topic});
    if (it == streams_.end()) {
        return false;
    }
    streams_.erase(it);
    return true;
}

neograph::graph::GraphStreamCallback EventBridge::make(
    std::string                                 agentName,
    std::string                                 threadId,
    std::weak_ptr<agentxx::agent::AgentContext> ctx,
    neograph::graph::GraphStreamCallback        origCb
) {
    return [agentName = std::move(agentName),
            threadId  = std::move(threadId),
            ctx       = std::move(ctx),
            origCb    = std::move(origCb)](const neograph::graph::GraphEvent& event) {
        // 先转发原始回调
        if (origCb) {
            origCb(event);
        }

        auto ctxPtr = ctx.lock();
        if (!ctxPtr || !ctxPtr->bus) {
            return; // 无 bus, 仅转发
        }
        auto  busPtr = ctxPtr->bus;
        auto& bus    = *busPtr;

        using T = neograph::graph::GraphEvent::Type;
        switch (event.type) {
            case T::LLM_TOKEN: {
                std::string token;
                std::string kind = "content";
                if (event.data.is_string()) {
                    token = event.data.get<std::string>();
                } else if (event.data.is_object()) {
                    neograph::ChatStreamChunk chunk;
                    neograph::from_json(event.data, chunk);
                    token = std::move(chunk.data);
                    if (chunk.type == neograph::ChatStreamChunk::TYPE_THINKING) {
                        kind = "thinking";
                    }
                } else {
                    token = event.data.dump();
                }
                // 无订阅者时跳过发布: 避免每个 token 创建一次无消费者的协程
                // (ModelToken topic 当前无生产订阅者, 该检查使热路径零开销)
                if (false
                    == bus.hasSubscribers<agentxx::events::EventModelToken>(
                        agentxx::events::Topic::ModelToken
                    )) {
                    break;
                }
                asio::co_spawn(
                    bus.executor(),
                    [busPtr, agentName, threadId, token = std::move(token), kind = std::move(kind)](
                    ) -> asio::awaitable<void> {
                        co_await busPtr->publish<agentxx::events::EventModelToken>(
                            agentxx::events::Topic::ModelToken,
                            agentxx::events::EventModelToken{
                                .agentName = agentName,
                                .threadId  = threadId,
                                .token     = token,
                                .kind      = kind,
                            }
                        );
                    },
                    asio::detached
                );
            } break;
            case T::NODE_START:
            case T::NODE_END:
                break;
            case T::CHANNEL_WRITE:
                break;
            case T::INTERRUPT:
                break;
            case T::ERROR: {
                auto msg
                    = event.data.is_string() ? event.data.get<std::string>() : event.data.dump();
                // 无订阅者时跳过发布 (与 LLM_TOKEN 一致, 避免无效协程创建)
                if (false
                    == bus.hasSubscribers<agentxx::events::EventError>(
                        agentxx::events::Topic::Error
                    )) {
                    break;
                }
                asio::co_spawn(
                    bus.executor(),
                    [busPtr, agentName, threadId, msg = std::move(msg), where = event.node_name](
                    ) -> asio::awaitable<void> {
                        co_await busPtr->publish<agentxx::events::EventError>(
                            agentxx::events::Topic::Error,
                            agentxx::events::EventError{
                                .agentName = agentName,
                                .threadId  = threadId,
                                .message   = msg,
                                .where     = where,
                            }
                        );
                    },
                    asio::detached
                );
            } break;
        }
    };
}

} // namespace middleware
} // namespace agentxx
