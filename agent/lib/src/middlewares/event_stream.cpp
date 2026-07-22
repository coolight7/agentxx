#include "agentxx/middlewares/event_stream.h"

namespace agentxx {
namespace middleware {

EventStreamInterface::EventStreamInterface(std::string_view in_name,
                                           const std::type_info &elementType)
    : name(in_name), elementType_(elementType) {}

EventBus::EventBus(asio::any_io_executor executor) : executor_(executor) {}

bool EventBus::remove(std::string_view topic) {
  auto it = streams_.find(std::string{topic});
  if (it == streams_.end()) {
    return false;
  }
  streams_.erase(it);
  return true;
}

neograph::graph::GraphStreamCallback
EventBridge::make(std::string agentName, std::string threadId,
                  std::weak_ptr<agentxx::agent::AgentContext> ctx,
                  neograph::graph::GraphStreamCallback origCb) {
  return [agentName = std::move(agentName), threadId = std::move(threadId),
          ctx = std::move(ctx), origCb = std::move(origCb)](
             const neograph::graph::GraphEvent &event) {
    // 先转发原始回调
    if (origCb) {
      origCb(event);
    }

    auto ctxPtr = ctx.lock();
    if (!ctxPtr || !ctxPtr->bus) {
      return; // 无 bus, 仅转发
    }
    // 捕获 bus shared_ptr 以保证发布协程期间总线存活
    auto busPtr = ctxPtr->bus;
    auto &bus = *busPtr;

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
      // fire-and-forget: GraphStreamCallback 是同步签名 void(...),
      // 无法 co_await, 只能 co_spawn 发布协程。
      // 单 io_context 下 co_spawn 帧轻量且按序完成 (publish 内部顺序派发
      // 订阅者, 无阻塞), 高频 token 流下可接受;
      // 若未来订阅者变重或需跨线程, 可改为 post + 专用读取协程批处理
      asio::co_spawn(
          bus.executor(),
          [busPtr, agentName, threadId, token = std::move(token),
           kind = std::move(kind)]() -> asio::awaitable<void> {
            co_await busPtr->publish<agentxx::events::EventModelToken>(
                agentxx::events::Topic::ModelToken,
                agentxx::events::EventModelToken{
                    .agentName = agentName,
                    .threadId = threadId,
                    .token = token,
                    .kind = kind,
                });
          },
          asio::detached);
    } break;
    case T::NODE_START:
      // 暂不发布细粒度节点事件; 后续按需扩展 ModelCallStart/ToolCallStart
      break;
    case T::NODE_END:
      break;
    case T::CHANNEL_WRITE:
      break;
    case T::INTERRUPT:
      break;
    case T::ERROR: {
      auto msg = event.data.is_string() ? event.data.get<std::string>()
                                        : event.data.dump();
      asio::co_spawn(
          bus.executor(),
          [busPtr, agentName, threadId, msg = std::move(msg),
           where = event.node_name]() -> asio::awaitable<void> {
            co_await busPtr->publish<agentxx::events::EventError>(
                agentxx::events::Topic::Error, agentxx::events::EventError{
                                                   .agentName = agentName,
                                                   .threadId = threadId,
                                                   .message = msg,
                                                   .where = where,
                                               });
          },
          asio::detached);
    } break;
    }
  };
}

} // namespace middleware
} // namespace agentxx
