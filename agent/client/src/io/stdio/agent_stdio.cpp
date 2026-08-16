#include "agentxx-client/io/stdio/agent_stdio.h"

#include "agentxx-client/io/stdio/stdin_reader.h"
#include "agentxx/agent/conversation_types.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "asio/this_coro.hpp"
#include "fmt/format.h"
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <utility>

// 首次会话启动通知的计数器 (仅用于控制台输出, 单会话场景下无需严格同步)
static bool g_firstSessionDone = false;
static int  g_mcpCount         = 0;
static int  g_skillCount       = 0;
static int  g_memoryCount      = 0;

StdIOClientAgentIO::StdIOClientAgentIO() :
    logSink_(std::make_shared<StderrLogSink>()) {
    agentxx::util::LogDispatcher::instance().addSink(logSink_);
}

StdIOClientAgentIO::~StdIOClientAgentIO() {
    if (logSink_) {
        agentxx::util::LogDispatcher::instance().removeSink(logSink_);
    }
}

void StdIOClientAgentIO::onDelta(const agentxx::agent::Delta& delta) {
    using Type = agentxx::agent::Delta::Type;
    switch (delta.type) {
        case Type::TextToken:
            if (isThinking_) {
                std::cout << std::endl << "[Content] ";
            }
            isThinking_ = false;
            std::cout << delta.text << std::flush;
            break;
        case Type::ThinkingToken:
            if (!isThinking_) {
                std::cout << std::endl << "[Think] ";
            }
            isThinking_ = true;
            std::cout << delta.text << std::flush;
            break;
        case Type::ToolStart:
            std::cout << std::endl
                      << fmt::format("[Tool] {} running...", delta.toolName) << std::endl;
            break;
        case Type::ToolEnd:
            std::cout
                << std::endl
                << fmt::format("[Tool] {} {}", delta.toolName, delta.hasError ? "error" : "done")
                << std::endl;
            break;
        case Type::MessageUITip: {
            std::string prefix;
            using TipType = agentxx::agent::Delta::TipType;
            switch (delta.tipType) {
                case TipType::Warning:
                    prefix = "[Warning] ";
                    break;
                case TipType::Error:
                    prefix = "[Error] ";
                    break;
                default:
                    prefix = "[Info] ";
                    break;
            }
            std::cout << std::endl << prefix << delta.text << std::endl;
            break;
        }
        case Type::TurnStart:
            isThinking_ = false;
            break;
        case Type::TurnEnd: {
            std::cout << "\n>>> " << std::flush;
            isThinking_ = false;

            // 首次会话结束时输出汇总信息
            if (!g_firstSessionDone) {
                if (g_mcpCount > 0 || g_skillCount > 0 || g_memoryCount > 0) {
                    g_firstSessionDone = true;
                    std::cout << fmt::format(
                        R"_(
┏━━━━━━ Session Startup ━━━━━━┓
┣━ MCP Tools: {}
┣━ Skills: {}
┣━ Memory Files: {}
┗━━━━━━ Session Startup ━━━━━━┛
)_",
                        g_mcpCount,
                        g_skillCount,
                        g_memoryCount
                    ) << std::endl;
                }
            }
            break;
        }
        default:
            break;
    }
}

void StdIOClientAgentIO::onSync(const agentxx::agent::SyncPayload& payload) {
    for (const auto& vm : payload.messages) {
        if (vm.role == agentxx::agent::ViewMessage::Role::User) {
            std::cout << "> " << vm.text << std::endl;
        } else if (vm.role == agentxx::agent::ViewMessage::Role::Assistant) {
            std::cout << vm.text << std::endl;
        }
    }
}

void StdIOClientAgentIO::onPeerMessage(agentxx::agent::WireMessage msg) {
    // 拦截启动信息响应: 整批统计 MCP/Skill/Memory, 其余消息委托基类分发
    if (auto* info = std::get_if<agentxx::agent::WireAppendComponentInfo>(&msg)) {
        using Type = agentxx::agent::AppendComponentNotification::Type;
        for (const auto& notif : info->notifications) {
            switch (notif.type) {
                case Type::Mcp:
                    ++g_mcpCount;
                    break;
                case Type::Skill:
                    ++g_skillCount;
                    break;
                case Type::Memory:
                    ++g_memoryCount;
                    break;
            }
        }
        XX_LOGI(
            "AppendComponentInfo: MCP={}, Skill={}, Memory={}",
            g_mcpCount,
            g_skillCount,
            g_memoryCount
        );
        return;
    }
    agentxx::agent::AgentIOBase::onPeerMessage(std::move(msg));
}

asio::awaitable<std::optional<std::string>> StdIOClientAgentIO::getInput() {
    auto& stdinReader = StdinReader::instance(co_await asio::this_coro::executor);
    co_return co_await stdinReader.readLine();
}

// ---------------------------------------------------------------------------
// 插件适配器接口 (CliPluginAdapter 在 client io 线程调用)
// ---------------------------------------------------------------------------

void StdIOClientAgentIO::sendPluginUserInput(const std::string& text) {
    if (text.empty() || threadId_.empty()) {
        return;
    }
    // 与用户输入同路径: 发送 WireUserInput (发送后通知事件接收器)
    sendUserInput(threadId_, text);
}

bool StdIOClientAgentIO::sendPluginDataUp(
    const std::string& plugin,
    const std::string& event,
    const std::string& json
) {
    if (!transport_ || !transport_->alive()) {
        XX_LOGW("[stdio] sendPluginDataUp dropped (no transport): {}.{}", plugin, event);
        return false;
    }
    agentxx::agent::WirePluginDataUp up;
    up.plugin = plugin;
    up.event  = event;
    up.data   = json;
    sendToPeer(std::move(up));
    return true;
}

asio::awaitable<neograph::json> StdIOClientAgentIO::handleInterrupt(
    std::string_view threadId,
    std::string_view interruptNode,
    std::string_view interruptValue,
    std::string_view interruptArgJson
) {
    // 与 TUI 版一致: 容错解析中断参数 JSON, 避免非法数据抛异常使整个中断请求失败
    std::optional<agentxx::middleware::InterruptHandleArg> argOpt;
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            argOpt = agentxx::middleware::InterruptHandleArg::fromJson(
                neograph::json::parse(interruptArgJson)
            );
            return true;
        },
        [](std::string errinfo) -> bool {
            XX_LOGE("StdIOClientAgentIO::handleInterrupt json::parse failed: {}", errinfo);
            return true;
        }
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
                        vals += fmt::format("{}, ", val);
                    }
                    typeHint = fmt::format("  ┣━ Type | enum | value of [{}]\n", vals);
                } else {
                    typeHint = fmt::format("  ┣━ Type | {}\n", input.type);
                }
                std::cout << typeHint << std::flush;
                std::cout << fmt::format("  ┣━ Default Value: {}\n", input.defaultValue)
                          << std::flush;
                std::cout << "  ┣━ >>> " << std::flush;

                haveWaitInput = true;
                std::string inputValue;
                auto        inputValueOpt = co_await getInput();
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
                    auto    r    = agentxx::util::parseNumberFromString(inputValue, num);
                    inputSuccess = (r.ec == std::errc{});
                } else if ("double" == input.type) {
                    double num;
                    auto   r     = agentxx::util::parseNumberFromString(inputValue, num);
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
