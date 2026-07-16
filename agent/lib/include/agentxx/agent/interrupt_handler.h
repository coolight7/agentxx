#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/events.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/log.h"
#include "neograph/neograph.h"
#include <memory>
#include <string>

namespace agentxx {
namespace agent {

/// 中断 HIL 处理器 (CLI 实现)
/// - 注册为 EventBus 上 service.interrupt 的 server
/// - 收到 ReqInterrupt 后, 解析为 InterruptHandleArg, 复用
///   MiddlewareContext::execInterruptHandle 的 stdin 输入逻辑
/// - 把结果包装为 RespInterrupt 回填
class CliInterruptHandler {
public:
  std::weak_ptr<AgentContext> agentContext;
  size_t serverId = 0;
  bool registered = false;

  explicit CliInterruptHandler(std::weak_ptr<AgentContext> ctx)
      : agentContext(std::move(ctx)) {}

  /// 注册到总线; 幂等
  asio::awaitable<void> start() {
    if (registered) {
      co_return;
    }
    auto ctxPtr = agentContext.lock();
    if (!ctxPtr || !ctxPtr->bus) {
      XX_LOGE("CliInterruptHandler: AgentContext or bus is null");
      co_return;
    }
    auto &rr = ctxPtr->bus->getRR<events::ReqInterrupt, events::RespInterrupt>(
        events::Topic::Interrupt);
    serverId = rr.serve(
        [this](const events::ReqInterrupt &req,
               size_t /*corrId*/) -> asio::awaitable<events::RespInterrupt> {
          co_return co_await handle(req);
        });
    registered = true;
    co_return;
  }

  /// 注销; 幂等
  void stop() {
    if (!registered) {
      return;
    }
    auto ctxPtr = agentContext.lock();
    if (ctxPtr && ctxPtr->bus) {
      auto &rr =
          ctxPtr->bus->getRR<events::ReqInterrupt, events::RespInterrupt>(
              events::Topic::Interrupt);
      rr.removeServer(serverId);
    }
    registered = false;
  }

  ~CliInterruptHandler() { stop(); }

private:
  asio::awaitable<events::RespInterrupt>
  handle(const events::ReqInterrupt &req) {
    auto ctxPtr = agentContext.lock();
    if (!ctxPtr || !ctxPtr->middlewareHandleContext) {
      co_return events::RespInterrupt{.handled = false, .resultJson = "{}"};
    }

    // 解析单个 InterruptHandleArg
    auto argOpt = middleware::InterruptHandleArg::fromJson(
        neograph::json::parse(req.interruptArgsJson));
    if (!argOpt.has_value()) {
      co_return events::RespInterrupt{.handled = false, .resultJson = "{}"};
    }

    // TODO: execInterruptHandle 内部使用阻塞式 std::getline(std::cin),
    //       会阻塞 io_context 直至用户输入;
    //       后续迁移到 StdinReader 异步读取, 但需同步重构
    //       MiddlewareContext::registerInterruptHandles 的 lambda
    auto result = co_await ctxPtr->middlewareHandleContext->execInterruptHandle(
        argOpt->name, argOpt.value());
    if (result.has_value()) {
      co_return events::RespInterrupt{
          .handled = true,
          .resultJson = result.value().dump(),
      };
    }
    // 无对应 handle, 未处理
    co_return events::RespInterrupt{.handled = false, .resultJson = "{}"};
  }
};

} // namespace agent
} // namespace agentxx
