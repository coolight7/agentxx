#include "agentxx-client/io/stdio/agent_stdio.h"

#include "agentxx-client/io/stdio/stdin_reader.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/string_util.h"
#include "asio/this_coro.hpp"
#include "fmt/format.h"
#include <charconv>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <utility>

void AgentStdIO::onToken(const std::string& token, const std::string& kind) {
    if (kind == "thinking") {
        if (!isThinking_) {
            std::cout << std::endl << "[Thinking] ";
        }
        isThinking_ = true;
    } else {
        if (isThinking_) {
            std::cout << std::endl << "[Content] ";
        }
        isThinking_ = false;
    }
    std::cout << token << std::flush;
}

asio::awaitable<std::optional<std::string>> AgentStdIO::getInput() {
    auto& stdinReader = StdinReader::instance(co_await asio::this_coro::executor);
    co_return co_await stdinReader.readLine();
}

asio::awaitable<neograph::json> AgentStdIO::handleInterrupt(
    const std::string& threadId,
    const std::string& interruptNode,
    const std::string& interruptValue,
    const std::string& interruptArgJson
) {
    auto argOpt = agentxx::middleware::InterruptHandleArg::fromJson(
        neograph::json::parse(interruptArgJson)
    );
    if (!argOpt.has_value()) {
        co_return neograph::json::array();
    }
    const auto& handleArg = argOpt.value();

    // 显示中断通知
    std::cout << fmt::format(
        R"(
┏━━━━━━ Interrupted ━━━━━━┓
┣━ Interrupted at: {}
┣━ Value: {}
{}
┗━━━━━━ Interrupted ━━━━━━┛
)",
        interruptNode,
        interruptValue,
        (!handleArg.name.empty()) ? fmt::format("┣━ Interrupt Handle: {}", handleArg.name)
                                  : "┣━ Unknown InterruptHandleArg"
    ) << std::endl;

    auto result = neograph::json::array();
    std::cout << "\n  ┏━━━━━━ Input ━━━━━━┓\n" << std::flush;
    bool haveWaitInput = false;

    for (const auto& input : handleArg.inputs) {
        bool inputSuccess = false;
        do {
                std::cout << fmt::format("  ┣━ ## {} : {}\n", input.label, input.depict) << std::flush;

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
                std::cout << typeHint << std::flush;
                std::cout << fmt::format("  ┣━ Default Value: {}\n", input.defaultValue) << std::flush;
                std::cout << "  ┣━ >>> " << std::flush;

                haveWaitInput   = true;
                std::string inputValue;
                auto inputValueOpt = co_await getInput();
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
                    int64_t num = 0;
                    auto    r   = std::from_chars(
                        inputValue.c_str(),
                        inputValue.c_str() + inputValue.size(),
                        num
                    );
                    inputSuccess = (r.ec == std::errc{});
                } else if ("double" == input.type) {
                    double num;
                    auto   r = std::from_chars(
                        inputValue.c_str(),
                        inputValue.c_str() + inputValue.size(),
                        num
                    );
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
                    std::cout << "  ┣━ Invalid Input, please try again.\n" << std::flush;
                }
            }
        } while (false == inputSuccess);
    }
    if (false == haveWaitInput) {
        std::cout << "  ┣━ Wait user review, `Enter` to continue.\n" << std::flush;
        std::cout << "  ┣━ >>> " << std::flush;
        co_await getInput();
    }
    std::cout << "  ┗━━━━━━ Input ━━━━━━┛\n\n" << std::flush;
    co_return result;
}

void AgentStdIO::resetTokenState() {
    isThinking_ = false;
}