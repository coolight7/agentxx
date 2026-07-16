#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/events.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/util/log.h"
#include "neograph/graph/types.h"
#include <memory>
#include <string>

namespace agentxx {
namespace agent {

/// GraphEvent -> EventBus 适配器
/// - 把 neograph 的 GraphStreamCallback 翻译成强类型总线事件发布
/// - 同时可选转发到原始 callback
/// - 调用者保证 AgentContext 及其 bus 在回调期间存活
class EventBridge {
public:
  /// 创建一个 GraphStreamCallback, 将 GraphEvent 发布到 bus 并转发 origCb
  /// - agentName: 当前 agent 名 (事件 source)
  /// - threadId: 当前会话 id
  /// - ctx: AgentContext (取 bus; 若 bus 为空则只转发 origCb)
  /// - origCb: 原始回调 (可空, 用于过渡期保留旧输出行为)
  static neograph::graph::GraphStreamCallback
  make(std::string agentName, std::string threadId,
       std::weak_ptr<AgentContext> ctx,
       neograph::graph::GraphStreamCallback origCb = nullptr) {
    return [agentName = std::move(agentName), threadId = std::move(threadId),
            ctx = std::move(ctx), origCb = std::move(origCb)](
               const neograph::graph::GraphEvent &event) {
      // 先转发原始回调 (保留旧行为, 如 CLI 的 std::cout)
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
        auto token = event.data.is_string() ? event.data.get<std::string>()
                                            : event.data.dump();
        // fire-and-forget: GraphStreamCallback 是同步签名 void(...),
        // 无法 co_await, 只能 co_spawn 发布协程。
        // 单 io_context 下 co_spawn 帧轻量且按序完成 (publish 内部顺序派发
        // 订阅者, 无阻塞), 高频 token 流下可接受;
        // 若未来订阅者变重或需跨线程, 可改为 post + 专用读取协程批处理
        asio::co_spawn(
            bus.executor(),
            [busPtr, agentName, threadId,
             token = std::move(token)]() -> asio::awaitable<void> {
              co_await busPtr->publish<agentxx::events::EventModelToken>(
                  agentxx::events::Topic::ModelToken,
                  agentxx::events::EventModelToken{
                      .agentName = agentName,
                      .threadId = threadId,
                      .token = token,
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
};

} // namespace agent
} // namespace agentxx
