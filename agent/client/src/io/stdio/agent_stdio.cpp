#include "agentxx-client/io/stdio/agent_stdio.h"

#include "agentxx-client/io/stdio/stdin_reader.h"
#include "agentxx/agent/conversation_types.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/exception.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include "asio/this_coro.hpp"
#include "fmt/format.h"
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <utility>

namespace agentxx::client {

// 首次会话启动通知的计数器 (仅用于控制台输出, 单会话场景下无需严格同步)
static bool g_firstSessionDone = false;
static int  g_mcpCount         = 0;
static int  g_skillCount       = 0;
static int  g_memoryCount      = 0;
static int  g_pluginCount      = 0;

StdIOClientAgentIO::StdIOClientAgentIO() :
    logSink_(std::make_shared<StderrLogSink>()) {
    utilxx_base::LogDispatcher::instance().addSink(logSink_);
}

StdIOClientAgentIO::~StdIOClientAgentIO() {
    if (logSink_) {
        utilxx_base::LogDispatcher::instance().removeSink(logSink_);
    }
}

void StdIOClientAgentIO::onDelta(const agentxx::agent::WireDelta& delta) {
    using Type = agentxx::agent::WireDelta::Type;
    switch (delta.type) {
        case Type::TextToken:
            if (isThinking_) {
                std::cout << std::endl << "[Content] ";
            }
            isThinking_ = false;
            std::cout << delta.text << std::flush;
            break;
        case Type::ThinkToken:
            if (!delta.text.empty()) {
                if (!isThinking_) {
                    std::cout << std::endl << "[Think] ";
                    isThinking_ = true;
                }
                std::cout << delta.text << std::flush;
            } else if (delta.think && delta.think->isEncrypted
                       && delta.think->reasoningTokens == 0) {
                // 加密思考首包: 开始思考提示 (元数据更新包静默处理, 避免正文后重复打印)
                if (!isThinking_) {
                    std::cout << std::endl << "[Think] (思考内容被加密)" << std::flush;
                    isThinking_ = true;
                }
            }
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
            using TipType = agentxx::agent::WireDelta::TipType;
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
        case Type::InsertMessage:
        case Type::UpdateMessage: {
            if (delta.message) {
                using namespace agentxx::agent;
                const auto& msg = *delta.message;
                std::string prefix;
                if (msg.role == ViewMessage::Role::Tip && msg.tip) {
                    switch (msg.tip->tipLevel) {
                        case ViewMessage::TipLevel::Warning:
                            prefix = "[Warning] ";
                            break;
                        case ViewMessage::TipLevel::Error:
                            prefix = "[Error] ";
                            break;
                        default:
                            prefix = "[Info] ";
                            break;
                    }
                }
                std::cout << std::endl << prefix << msg.text << std::endl;
            }
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
                if (g_mcpCount > 0 || g_skillCount > 0 || g_memoryCount > 0 || g_pluginCount > 0) {
                    g_firstSessionDone = true;
                    std::cout << fmt::format(
                        R"_(
┏━━━━━━ Session Startup ━━━━━━┓
┣━ MCP Tools: {}
┣━ Skills: {}
┣━ Memory Files: {}
┣━ Plugins: {}
┗━━━━━━ Session Startup ━━━━━━┛
)_",
                        g_mcpCount,
                        g_skillCount,
                        g_memoryCount,
                        g_pluginCount
                    ) << std::endl;
                }
            }
            break;
        }
        default:
            break;
    }
}

void StdIOClientAgentIO::onSync(const agentxx::agent::WireSyncPayload& payload) {
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
                case Type::Plugin:
                    ++g_pluginCount;
                    break;
            }
        }
        XX_LOGI(
            "AppendComponentInfo: MCP={}, Skill={}, Memory={}, Plugins={}",
            g_mcpCount,
            g_skillCount,
            g_memoryCount,
            g_pluginCount
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
    if (text.empty() || sessionId_.empty()) {
        return;
    }
    // 与用户输入同路径: 发送 WireUserInput (发送后通知事件接收器)
    sendUserInput(sessionId_, text);
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

asio::awaitable<utilxx_base::Json> StdIOClientAgentIO::handleInterrupt(
    std::string_view sessionId,
    std::string_view interruptNode,
    std::string_view interruptValue,
    std::string_view interruptArgJson
) {
    // 与 TUI 版一致: 容错解析中断参数 JSON, 避免非法数据抛异常使整个中断请求失败
    std::optional<agentxx::middleware::InterruptHandleArg> argOpt;
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            argOpt = agentxx::middleware::InterruptHandleArg::fromJson(
                utilxx_base::Json::parse(interruptArgJson)
            );
            return true;
        },
        [](std::string errinfo) -> bool {
            XX_LOGE("StdIOClientAgentIO::handleInterrupt json::parse failed: {}", errinfo);
            return true;
        }
    );
    if (!argOpt.has_value()) {
        co_return utilxx_base::Json::object();
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

    // 内容 + 控件全部来自描述 (无 inputs[] 参数类型声明; 行式前端按控件形态问答)
    const auto&       ui    = handleArg.ui;
    const std::string plain = agentxx::middleware::interruptUiPlainText(ui, 0);
    if (!plain.empty()) {
        // 内容块降级为纯文本 (markdown 打印原文; diff 统一 diff 文本;
        // 控件块的候选/默认值说明也在其中)
        std::cout << "\n  ┏━━━━━━ Prompt ━━━━━━┓\n"
                  << plain << "\n"
                  << "  ┗━━━━━━ Prompt ━━━━━━┛\n"
                  << std::flush;
    }

    namespace mw                      = agentxx::middleware;
    utilxx_base::Json values        = utilxx_base::Json::object();
    bool                haveWaitInput = false;
    std::cout << "\n  ┏━━━━━━ Input ━━━━━━┓\n" << std::flush;

    /// 候选项标签 (label 优先, 空则用原始值文本)
    auto optionLabelOf = [](const mw::InterruptUiOption& opt) -> std::string {
        if (!opt.label.empty()) {
            return opt.label;
        }
        if (opt.value.is_string()) {
            return opt.value.get<std::string>();
        }
        return opt.value.is_null() ? std::string{"-"} : opt.value.dump();
    };

    for (const auto& block : ui.blocks) {
        if (block.kind != "control") {
            continue;
        }
        const std::string id    = block.id.empty() ? std::string{"value"} : block.id;
        std::string       title = block.label.empty() ? id : block.label;
        if (!block.help.empty()) {
            title += fmt::format(" ({})", block.help);
        }

        // 候选项为空的可选控件 (buttons/select): 无可取值, 跳过并提示
        if ((block.control == "buttons" || block.control == "select") && block.options.empty()) {
            std::cout << fmt::format("  ┣━ ## {} : (no options, skipped)\n", title) << std::flush;
            continue;
        }

        bool inputSuccess = false;
        do {
            std::cout << fmt::format("  ┣━ ## {}\n", title) << std::flush;
            // 控件形态提示 (与候选列表)
            if (block.control == "buttons" || block.control == "select") {
                for (size_t i = 0; i < block.options.size(); ++i) {
                    std::cout
                        << fmt::format("  ┣━ [{}] {}\n", i + 1, optionLabelOf(block.options[i]))
                        << std::flush;
                }
                std::cout << "  ┣━ Type | option index or value\n";
            } else if (block.control == "checkbox") {
                std::cout << "  ┣━ Type | checkbox | `yes/y` or `no/n`\n";
            } else if (block.control == "number") {
                std::cout << fmt::format(
                    "  ┣━ Type | {}{}{}\n",
                    block.integer ? "integer" : "number",
                    block.hasMin ? fmt::format(", min {}", block.minValue) : "",
                    block.hasMax ? fmt::format(", max {}", block.maxValue) : ""
                );
            } else {
                std::cout << "  ┣━ Type | string\n";
            }
            // 默认值提示
            std::string defaultText;
            if (block.control == "buttons" || block.control == "select") {
                if (!block.defaultValue.is_null()) {
                    defaultText = block.defaultValue.is_string()
                                      ? block.defaultValue.get<std::string>()
                                      : block.defaultValue.dump();
                }
            } else if (block.control == "checkbox") {
                defaultText = (block.defaultValue.is_boolean() && block.defaultValue.get<bool>())
                                  ? "yes"
                                  : "no";
            } else if (block.control == "number") {
                defaultText = block.defaultValue.is_number()
                                  ? fmt::format("{}", block.defaultValue.get<double>())
                                  : "0";
            } else if (!block.defaultValue.is_null()) {
                defaultText = block.defaultValue.is_string() ? block.defaultValue.get<std::string>()
                                                             : block.defaultValue.dump();
            }
            std::cout << fmt::format("  ┣━ Default Value: {}\n", defaultText) << std::flush;
            std::cout << "  ┣━ >>> " << std::flush;

            haveWaitInput = true;
            std::string inputValue;
            auto        inputValueOpt = co_await getInput();
            if (inputValueOpt.has_value()) {
                inputValue = inputValueOpt.value();
            }

            if (block.control == "buttons" || block.control == "select") {
                // 空输入: 取默认值 (默认值即候选项 value; 缺失取首项)
                if (inputValue.empty()) {
                    size_t index = 0;
                    if (!block.defaultValue.is_null()) {
                        for (size_t i = 0; i < block.options.size(); ++i) {
                            if (block.options[i].value.dump() == block.defaultValue.dump()) {
                                index = i;
                                break;
                            }
                        }
                    }
                    values[id]   = block.options[index].value;
                    inputSuccess = true;
                } else {
                    // 输入序号 (1-based) 或候选项值
                    int64_t    index = 0;
                    const bool isIndex
                        = utilxx_base::parseNumberFromString(inputValue, index).ec == std::errc{}
                          && index >= 1 && index <= static_cast<int64_t>(block.options.size());
                    if (isIndex) {
                        values[id]   = block.options[static_cast<size_t>(index - 1)].value;
                        inputSuccess = true;
                    } else {
                        for (const auto& opt : block.options) {
                            if (opt.value.is_string()
                                && opt.value.get<std::string>() == inputValue) {
                                values[id]   = opt.value;
                                inputSuccess = true;
                                break;
                            }
                            if (optionLabelOf(opt) == inputValue) {
                                values[id]   = opt.value;
                                inputSuccess = true;
                                break;
                            }
                        }
                    }
                }
            } else if (block.control == "checkbox") {
                std::string v = inputValue;
                utilxx_base::toLowerSelf(v);
                if (v.empty()) {
                    values[id] = block.defaultValue.is_boolean() && block.defaultValue.get<bool>();
                    inputSuccess = true;
                } else if (v == "yes" || v == "y" || v == "true" || v == "1") {
                    values[id]   = true;
                    inputSuccess = true;
                } else if (v == "no" || v == "n" || v == "false" || v == "0") {
                    values[id]   = false;
                    inputSuccess = true;
                }
            } else if (block.control == "number") {
                double num = 0.0;
                if (inputValue.empty()) {
                    num = block.defaultValue.is_number() ? block.defaultValue.get<double>() : 0.0;
                    if (block.integer) {
                        num = std::trunc(num);
                    }
                    inputSuccess = true;
                } else if (utilxx_base::parseNumberFromString(inputValue, num).ec == std::errc{}
                           && (!block.integer || num == std::trunc(num))
                           && (!block.hasMin || num >= block.minValue)
                           && (!block.hasMax || num <= block.maxValue)) {
                    inputSuccess = true;
                }
                if (inputSuccess) {
                    // 括号构造: 单元素花括号会生成数组而非数值
                    values[id] = block.integer ? utilxx_base::Json(static_cast<int64_t>(num))
                                               : utilxx_base::Json(num);
                }
            } else if (block.control == "text") {
                // 文本控件: 空输入取默认值 (默认值可为空串)
                if (inputValue.empty()) {
                    values[id] = block.defaultValue.is_string()
                                     ? block.defaultValue.get<std::string>()
                                     : std::string{};
                } else {
                    values[id] = inputValue;
                }
                inputSuccess = true;
            } else {
                // 未知控件形态: 跳过 (不参与结果; 与 TUI 诊断行口径一致)
                std::cout << "  ┣━ Unsupported control, skipped.\n" << std::flush;
                inputSuccess = true;
                continue;
            }

            if (inputSuccess) {
                std::cout << fmt::format(
                    "  ┣━ Value: {}\n",
                    values[id].is_string() ? values[id].get<std::string>() : values[id].dump()
                ) << std::flush;
            } else {
                std::cout << "  ┣━ Invalid Input, please try again.\n" << std::flush;
            }
        } while (false == inputSuccess);
    }

    if (false == haveWaitInput) {
        std::cout << "  ┣━ Wait user review, `Enter` to continue.\n" << std::flush;
        std::cout << "  ┣━ >>> " << std::flush;
        co_await getInput();
    }
    std::cout << "  ┗━━━━━━ Input ━━━━━━┛\n\n" << std::flush;
    // 结果恒为对象形态 {"values": {控件 id: 值}} (与客户端契约一致;
    // 空对象 = 未提交/取消, 消费端按未应答处理)
    co_return agentxx::middleware::makeInterruptResult(values);
}

} // namespace agentxx::client
