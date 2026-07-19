#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/agent/stdin_reader.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/events.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "neograph/neograph.h"
#include <memory>
#include <string>

namespace agentxx {
namespace middleware {

/// 中断 HIL 处理器 (CLI 实现)
/// - 注册为 EventBus 上 service.interrupt 的 server
/// - 收到 ReqInterrupt 后, 解析为 InterruptHandleArg, 然后由
///   execInterruptHandle 处理
/// - 把结果包装为 RespInterrupt 回填
class CliInterruptHandler {
public:
  std::weak_ptr<agentxx::agent::AgentContext> agentContext;
  size_t serverId = 0;
  bool registered = false;

  explicit CliInterruptHandler(std::weak_ptr<agentxx::agent::AgentContext> ctx)
      : agentContext(std::move(ctx)) {}

  /// 注册到总线
  asio::awaitable<void> start() {
    if (registered) {
      co_return;
    }
    auto ctxPtr = agentContext.lock();
    if (!ctxPtr || !ctxPtr->bus) {
      XX_LOGE("CliInterruptHandler: AgentContext or bus is null");
      co_return;
    }
    interruptHandles.clear();
    registerInterruptHandles();

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

  /// 注销
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
  void registerInterruptHandles() {
    interruptHandles
        [agentxx::middleware::MiddlewareContext::interruptHandleName_default] =
            [](const agentxx::middleware::InterruptHandleArg &handleArg)
        -> asio::awaitable<neograph::json> {
      auto result = neograph::json::array();
      std::cout << "\n  ┏━━━━━━ Input ━━━━━━┓" << std::endl;
      bool haveWaitInput = false;

      // 异步读取 stdin
      auto &stdinReader = agentxx::agent::StdinReader::instance(
          co_await asio::this_coro::executor);

      for (const auto &input : handleArg.inputs) {
        bool inputSuccess = false;
        do {
          std::cout << fmt::format("  ┣━ ## {} : {}", input.label, input.depict)
                    << std::endl;

          if (input.type.empty()) {
            // 不需要输入
            inputSuccess = true;
          } else {
            std::cout << "  ┣━ Type | " << input.type << " | ";
            if ("bool" == input.type) {
              std::cout << "`yes/y` or `no/n`";
            } else if ("int" == input.type) {
            } else if ("double" == input.type) {
            } else if ("string" == input.type) {
            } else if ("enum" == input.type) {
              std::cout << "value of [";
              for (const auto &val : input.enumValues) {
                std::cout << val << ", ";
              }
              std::cout << "]";
            }
            std::cout << std::endl;
            std::cout << "  ┣━ Default Value: " << input.defaultValue
                      << std::endl;
            std::cout << "  ┣━ >>> " << std::flush;

            haveWaitInput = true;
            std::string line;
            auto lineOpt = co_await stdinReader.readLine();
            if (lineOpt.has_value()) {
              line = lineOpt.value();
            }
            if (line.empty()) {
              // 未输入，补充默认值
              line = input.defaultValue;
            }

            if ("bool" == input.type) {
              agentxx::util::toLowerSelf(line);
              if (line == "yes" || line == "y") {
                line = "true";
                inputSuccess = true;
              } else if (line == "no" || line == "n") {
                line = "false";
                inputSuccess = true;
              } else {
                inputSuccess = false;
              }
            } else if ("int" == input.type) {
              int64_t num = 0;
              auto result = std::from_chars(line.c_str(),
                                            line.c_str() + line.size(), num);
              inputSuccess = (result.ec == std::errc{});
            } else if ("double" == input.type) {
              double num;
              auto result = std::from_chars(line.c_str(),
                                            line.c_str() + line.size(), num);
              inputSuccess = (result.ec == std::errc{});
            } else if ("string" == input.type) {
              inputSuccess = true;
            } else if ("enum" == input.type) {
              for (const auto &val : input.enumValues) {
                if (val == line) {
                  inputSuccess = true;
                  break;
                }
              }
            }

            if (inputSuccess) {
              result.push_back(line);
            } else {
              std::cout << "  ┣━ Invalid Input, please try again." << std::endl;
            }
          }
        } while (false == inputSuccess);
      }
      if (false == haveWaitInput) {
        // 没有输入，等待用户确认
        std::cout << "  ┣━ Wait user review, `Enter` to continue." << std::endl;
        std::cout << "  ┣━ >>> " << std::flush;
        co_await stdinReader.readLine();
      }
      std::cout << "  ┗━━━━━━ Input ━━━━━━┛\n" << std::endl;
      co_return result;
    };
  }

  asio::awaitable<std::optional<neograph::json>>
  execInterruptHandle(std::string_view name,
                      const agentxx::middleware::InterruptHandleArg &arg) {
    auto handleIt = interruptHandles.find(arg.name);
    if (handleIt != interruptHandles.end()) {
      co_return co_await handleIt->second(arg);
    }
    co_return std::nullopt;
  }

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

    auto result = co_await execInterruptHandle(argOpt->name, argOpt.value());
    if (result.has_value()) {
      co_return events::RespInterrupt{
          .handled = true,
          .resultJson = result.value().dump(),
      };
    }
    // 无对应 handle, 未处理
    co_return events::RespInterrupt{.handled = false, .resultJson = "{}"};
  }

  /// <name, handle>
  std::map<std::string, std::function<asio::awaitable<neograph::json>(
                            const agentxx::middleware::InterruptHandleArg &)>>
      interruptHandles{};
};

} // namespace middleware
} // namespace agentxx
