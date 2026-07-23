#include "agentxx-client/io/stdio/interrupt_handler.h"

#include "agentxx/agent/agent_io.h"
#include "agentxx/agent/context.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "fmt/format.h"
#include <charconv>
#include <cstdint>
#include <utility>

StdioInterruptHandler::StdioInterruptHandler(std::weak_ptr<agentxx::agent::AgentContext> ctx) :
    agentContext(std::move(ctx)) {}

asio::awaitable<void> StdioInterruptHandler::start() {
    if (registered) {
        co_return;
    }
    auto ctxPtr = agentContext.lock();
    if (!ctxPtr || !ctxPtr->bus) {
        XX_LOGE("StdioInterruptHandler: AgentContext or bus is null");
        co_return;
    }
    interruptHandles.clear();
    registerInterruptHandles();

    auto& rr = ctxPtr->bus->getRR<agentxx::events::ReqInterrupt, agentxx::events::RespInterrupt>(
        agentxx::events::Topic::Interrupt);
    serverId
        = rr.serve([this](const agentxx::events::ReqInterrupt& req,
                          size_t /*corrId*/) -> asio::awaitable<agentxx::events::RespInterrupt> {
              co_return co_await handle(req);
          });

    registered = true;
    co_return;
}

void StdioInterruptHandler::stop() {
    if (!registered) {
        return;
    }
    auto ctxPtr = agentContext.lock();
    if (ctxPtr && ctxPtr->bus) {
        auto& rr
            = ctxPtr->bus->getRR<agentxx::events::ReqInterrupt, agentxx::events::RespInterrupt>(
                agentxx::events::Topic::Interrupt);
        rr.removeServer(serverId);
    }
    registered = false;
}

StdioInterruptHandler::~StdioInterruptHandler() {
    stop();
}

void StdioInterruptHandler::registerInterruptHandles() {
    interruptHandles[agentxx::middleware::MiddlewareContext::interruptHandleName_default]
        = [this](const agentxx::middleware::InterruptHandleArg& handleArg,
                 const std::string& threadId) -> asio::awaitable<neograph::json> {
        auto ctxPtr  = agentContext.lock();
        auto session = ctxPtr ? ctxPtr->sessions->get(threadId) : nullptr;
        auto io      = session ? session->io : nullptr;
        if (!io) {
            co_return neograph::json::array();
        }

        auto result = neograph::json::array();
        io->onDisplay("interrupt", "\n  ┏━━━━━━ Input ━━━━━━┓\n");
        bool haveWaitInput = false;

        for (const auto& input : handleArg.inputs) {
            bool inputSuccess = false;
            do {
                io->onDisplay("interrupt",
                              fmt::format("  ┣━ ## {} : {}\n", input.label, input.depict));

                if (input.type.empty()) {
                    inputSuccess = true;
                } else {
                    std::string typeHint;
                    if ("bool" == input.type) {
                        typeHint = "  ┣━ Type | bool | `yes/y` or `no/n`\n";
                    } else if ("enum" == input.type) {
                        std::string vals;
                        for (const auto& val : input.enumValues) {
                            vals += val + ", ";
                        }
                        typeHint = fmt::format("  ┣━ Type | enum | value of [{}]\n", vals);
                    } else {
                        typeHint = fmt::format("  ┣━ Type | {}\n", input.type);
                    }
                    io->onDisplay("interrupt", typeHint);
                    io->onDisplay("interrupt",
                                  fmt::format("  ┣━ Default Value: {}\n", input.defaultValue));
                    io->onDisplay("interrupt", "  ┣━ >>> ");

                    haveWaitInput = true;
                    std::string inputValue;
                    auto        inputValueOpt = co_await io->getInput();
                    if (inputValueOpt.has_value()) {
                        inputValue = inputValueOpt.value();
                    }
                    if (inputValue.empty()) {
                        inputValue = input.defaultValue;
                    }

                    if ("bool" == input.type) {
                        agentxx::util::toLowerSelf(inputValue);
                        if (inputValue == "yes" || inputValue == "y") {
                            inputValue   = "true";
                            inputSuccess = true;
                        } else if (inputValue == "no" || inputValue == "n") {
                            inputValue   = "false";
                            inputSuccess = true;
                        } else {
                            inputSuccess = false;
                        }
                    } else if ("int" == input.type) {
                        int64_t num  = 0;
                        auto    r    = std::from_chars(inputValue.c_str(),
                                                 inputValue.c_str() + inputValue.size(),
                                                 num);
                        inputSuccess = (r.ec == std::errc{});
                    } else if ("double" == input.type) {
                        double num;
                        auto   r     = std::from_chars(inputValue.c_str(),
                                                 inputValue.c_str() + inputValue.size(),
                                                 num);
                        inputSuccess = (r.ec == std::errc{});
                    } else if ("string" == input.type) {
                        inputSuccess = true;
                    } else if ("enum" == input.type) {
                        for (const auto& val : input.enumValues) {
                            if (val == inputValue) {
                                inputSuccess = true;
                                break;
                            }
                        }
                    }

                    if (inputSuccess) {
                        result.push_back(inputValue);
                    } else {
                        io->onDisplay("interrupt", "  ┣━ Invalid Input, please try again.\n");
                    }
                }
            } while (false == inputSuccess);
        }
        if (false == haveWaitInput) {
            io->onDisplay("interrupt", "  ┣━ Wait user review, `Enter` to continue.\n");
            io->onDisplay("interrupt", "  ┣━ >>> ");
            co_await io->getInput();
        }
        io->onDisplay("interrupt", "  ┗━━━━━━ Input ━━━━━━┛\n\n");
        co_return result;
    };
}

asio::awaitable<std::optional<neograph::json>>
    StdioInterruptHandler::execInterruptHandle(std::string_view                               name,
                                               const agentxx::middleware::InterruptHandleArg& arg,
                                               const std::string& threadId) {
    auto handleIt = interruptHandles.find(arg.name);
    if (handleIt != interruptHandles.end()) {
        co_return co_await handleIt->second(arg, threadId);
    }
    co_return std::nullopt;
}

asio::awaitable<agentxx::events::RespInterrupt>
    StdioInterruptHandler::handle(const agentxx::events::ReqInterrupt& req) {
    auto ctxPtr = agentContext.lock();
    if (!ctxPtr || !ctxPtr->middlewareHandleContext) {
        co_return agentxx::events::RespInterrupt{.handled = false, .resultJson = "{}"};
    }

    // 解析单个 InterruptHandleArg
    auto argOpt = agentxx::middleware::InterruptHandleArg::fromJson(
        neograph::json::parse(req.interruptArgsJson));
    if (!argOpt.has_value()) {
        co_return agentxx::events::RespInterrupt{.handled = false, .resultJson = "{}"};
    }

    auto result = co_await execInterruptHandle(argOpt->name, argOpt.value(), req.threadId);
    if (result.has_value()) {
        co_return agentxx::events::RespInterrupt{
            .handled    = true,
            .resultJson = result.value().dump(),
        };
    }
    // 无对应 handle, 未处理
    co_return agentxx::events::RespInterrupt{.handled = false, .resultJson = "{}"};
}
