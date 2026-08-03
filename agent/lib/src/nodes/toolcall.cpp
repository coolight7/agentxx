#include "agentxx/nodes/toolcall.h"

#include "agentxx/middlewares/permission.h"
#include "agentxx/tools/tool.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "fmt/format.h"
#include <algorithm>
#include <cassert>
#include <charconv>
#include <iostream>
#include <map>
#include <sstream>

namespace agentxx {
namespace nodes {

ToolcallWrapNode::ToolcallWrapNode(
    std::string_view                            in_name,
    const neograph::graph::NodeContext&         in_ctx,
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    WrapHandleBaseNode<neograph::graph::ToolDispatchNode>(in_name, in_agentContext, in_ctx) {}

void ToolcallWrapNode::onHandleStartError(
    bool                                                errorRethrow,
    bool                                                isCurrentError,
    std::string_view                                    exceptionStr,
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    neograph::graph::NodeInput&                         in,
    neograph::graph::NodeOutput&                        result
) noexcept {
    // TODO: 更改为在 baseRun 拦截生成 message，这里不知道出错的 toolcall id，生成是不对的
    // 插入消息，保证消息顺序正确
    if (false == errorRethrow && isCurrentError) {
        auto msg = neograph::ChatMessage{
            .role    = "tool",
            .content = fmt::format(
                R"({{"error": "{}/Start call `{}` exception: {}"}})",
                nodeName,
                item.name,
                exceptionStr
            ),
            .flags = neograph::MessageFlag::AutoInserted,
        };
        // 回填 tool_call_id/tool_name，确保 ToolEnd 能正确关联
        auto  messages = in.state.get_messages();
        auto* assistant_msg
            = agentxx::middleware::BaseMiddlewareHandleInterface::getLastAssistantToolcallMessage(
                messages
            );
        if (assistant_msg && !assistant_msg->tool_calls.empty()) {
            msg.tool_call_id = assistant_msg->tool_calls.front().id;
            msg.tool_name    = assistant_msg->tool_calls.front().name;
        }
        auto msgJson = neograph::json{};
        neograph::to_json(msgJson, msg);
        result.writes.push_back(neograph::graph::ChannelWrite{
            "messages",
            neograph::json::array({msgJson}),
        });
    }
}

void ToolcallWrapNode::onHandleBaseRunError(
    bool                         errorRethrow,
    bool                         isCurrentError,
    std::string_view             exceptionStr,
    neograph::graph::NodeInput&  in,
    neograph::graph::NodeOutput& result
) noexcept {
    // 插入消息，保证消息顺序正确
    if (false == errorRethrow && isCurrentError) {
        auto msg = neograph::ChatMessage{
            .role    = "tool",
            .content = fmt::format("[Exception aborted: {}]", exceptionStr),
            .flags   = neograph::MessageFlag::AutoInserted,
        };
        // 回填 tool_call_id/tool_name，确保 ToolEnd 能正确关联
        auto  messages = in.state.get_messages();
        auto* assistant_msg
            = agentxx::middleware::BaseMiddlewareHandleInterface::getLastAssistantToolcallMessage(
                messages
            );
        if (assistant_msg && !assistant_msg->tool_calls.empty()) {
            msg.tool_call_id = assistant_msg->tool_calls.front().id;
            msg.tool_name    = assistant_msg->tool_calls.front().name;
        }
        auto msgJson = neograph::json{};
        neograph::to_json(msgJson, msg);
        result.writes.push_back(neograph::graph::ChannelWrite{
            "messages",
            neograph::json::array({msgJson}),
        });
    }
}

asio::awaitable<void> ToolcallWrapNode::onHandleStart(
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    neograph::graph::NodeInput&                         in
) {
    co_await item.onToolcallStartFunc(in);
}

asio::awaitable<void> ToolcallWrapNode::onHandleEnd(
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    const neograph::graph::NodeInput&                   in,
    neograph::graph::NodeOutput&                        result
) {
    co_await item.onToolcallEndFunc(in, result);
}

asio::awaitable<std::string>
    ToolcallWrapNode::execTool(neograph::Tool* tool, neograph::json& args) const {
    auto agentCtxPtr = agentContext.lock();
    {
        // 权限检查 (permissionMiddleware 默认 nullptr, 未配置权限中间件时跳过)
        if (agentCtxPtr && agentCtxPtr->permissionMiddleware) {
            auto it = agentCtxPtr->permissionMiddleware->handles.find(tool->get_name());
            if (it != agentCtxPtr->permissionMiddleware->handles.end()) {
                auto allow = co_await it->second(*tool, args);
                if (false == allow) {
                    co_return "[Permission denied]";
                }
            }
        }
    }

    size_t maxRetry = 0;
    {
        auto str    = tool->extra["maxRetry"];
        auto result = agentxx::util::parseNumberFromString(str, maxRetry);
        if (result.ec != std::errc{}) {
            maxRetry = 0;
        }
    }

    std::string result;
    size_t      retry = 0;
    do {
        bool               isCancel = false;
        std::string        errInfo;
        std::exception_ptr errorPtr;

        try {
            result = co_await tool->real_execute_async(args);
            break;
        } catch (const neograph::graph::CancelledException&) {
            isCancel = true;
            errorPtr = std::current_exception();
        } catch (const neograph::graph::NodeInterrupt&) {
            isCancel = true;
            errorPtr = std::current_exception();
        } catch (const std::exception& e) {
            errInfo  = e.what();
            errorPtr = std::current_exception();
        } catch (const boost::exception& e) {
            errInfo  = boost::diagnostic_information(e);
            errorPtr = std::current_exception();
        } catch (...) {
            errorPtr = std::current_exception();
        }

        // 触发异常
        if (retry >= maxRetry || isCancel) {
            std::rethrow_exception(errorPtr);
        }
        XX_LOGD("ToolCallNode {} retry: {}/{} | {}", tool->get_name(), retry, maxRetry, errInfo);
        retry++;
    } while (true);

    const size_t limitLength = agentCtxPtr->agentConfig->toolcallSummaryLimitOutputLength;
    if ("true" == tool->extra["autoSummaryOutput"] && result.size() >= limitLength) {
        // 字节数量超过，按 utf8 长度判断
        auto [targetIndex, lineCount, lastLineIndex]
            = agentxx::util::findIndexAndLastLineIndexByUtf8Length(result, limitLength);
        if (targetIndex > 0) {
            const auto thread_id = args.value("thread_id", std::string{});
            assert(false == thread_id.empty());
            // 超过限制长度，截断并存储原文
            auto storeId
                = agentCtxPtr->middlewareHandleContext->addShareStoreItemValue(thread_id, result);
            // - 如果超过总摘要 1/3，按行摘要，留出行数以便后续用
            // `share_store` 分页按行取值 否则取总摘要
            if (lastLineIndex >= targetIndex / 3) {
                co_return fmt::format(
                    R"([Content offloaded. Use the `share_store` tool to fetch the full content by ID {}. Summary:{} lines]
{}
...)",
                    storeId,
                    lineCount,
                    std::string_view{result}.substr(0, lastLineIndex)
                );
            } else {
                co_return fmt::format(
                    R"([Content offloaded. Use the `share_store` tool to fetch the full content by ID {}. Summary:]
{}
...)",
                    storeId,
                    std::string_view{result}.substr(0, targetIndex)
                );
            }
        }
    }
    co_return result;
}

asio::awaitable<void> ToolcallWrapNode::baseRun(
    std::vector<std::shared_ptr<agentxx::middleware::BaseMiddlewareHandleInterface>>& handles,
    neograph::graph::NodeInput&                                                       in,
    neograph::graph::NodeOutput&                                                      out
) {
    auto agentCtxPtr = agentContext.lock();

    auto toolcallsCache = std::map<std::string, std::string>{};
    {
        auto toolcallsCacheJson
            = agentCtxPtr->middlewareHandleContext->getGraphDataItemValue<neograph::json>(
                in.ctx.thread_id,
                agentxx::middleware::MiddlewareContext::graphDataKey_interruptToolcallCache
            );
        agentCtxPtr->middlewareHandleContext->removeGraphDataItem(
            in.ctx.thread_id,
            agentxx::middleware::MiddlewareContext::graphDataKey_interruptToolcallCache
        );
        if (toolcallsCacheJson.is_array()) {
            for (const auto& item : toolcallsCacheJson) {
                neograph::ChatMessage msg;
                neograph::from_json(item, msg);
                if (false == msg.tool_call_id.empty()
                    && false == neograph::hasFlag(msg.flags, neograph::MessageFlag::Interrupt)) {
                    toolcallsCache[msg.tool_call_id] = std::move(msg.content);
                }
            }
        }
    }

    auto messages = in.state.get_messages();
    if (messages.empty()) {
        out = neograph::graph::NodeOutput{};
        co_return;
    }

    // Find the last assistant message with tool_calls
    const neograph::ChatMessage* assistant_msg = nullptr;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == "assistant" && !it->tool_calls.empty()) {
            assistant_msg = &(*it);
            break;
        }
    }
    if (!assistant_msg) {
        out = neograph::graph::NodeOutput{};
        co_return;
    }

    bool isInterrupt   = false;
    auto interruptArgs = std::map<std::string, neograph::json>{};
    auto results       = neograph::json::array();

    auto onExecTool = [&](const neograph::ToolCall& tc) -> asio::awaitable<neograph::ChatMessage> {
        neograph::ChatMessage tool_msg;
        tool_msg.role         = "tool";
        tool_msg.tool_call_id = tc.id;
        tool_msg.tool_name    = tc.name;
        {
            // 尝试缓存
            auto cacheit = toolcallsCache.find(tc.id);
            if (cacheit != toolcallsCache.end()) {
                tool_msg.content = cacheit->second;
                co_return tool_msg;
            }
        }

        auto it = std::find_if(tools_.begin(), tools_.end(), [&](neograph::Tool* t) {
            return t->get_name() == tc.name;
        });
        if (it == tools_.end()) {
            tool_msg.content = R"({"error": "Tool not found: )" + tc.name + "\"}";
        } else {
            std::exception_ptr errorPtr;
            co_await agentxx::util::catchErrorAsync<bool>(
                [&]() -> asio::awaitable<bool> {
                    try {
                        auto args = neograph::json::parse(tc.arguments);
                        if (args.is_object()) {
                            // append arg `thread_id`
                            args["thread_id"] = in.ctx.thread_id;
                            // - 注入 tool_call_id 供 tool 使用 (如 subagent_switch 的中断
                            // resultId)
                            args["tool_call_id"] = tc.id;
                        }
                        tool_msg.content = co_await execTool(*it, args);
                    } catch (const neograph::graph::CancelledException&) {
                        // TODO: 保存已有的 toolcall 结果再重新抛出异常
                        errorPtr = std::current_exception();
                    } catch (const neograph::graph::NodeInterrupt&) {
                        // tool触发中断
                        // - 不应在这里提取中断参数，协程并发等 co_await
                        // 执行完成时可能参数数组已经不是单一值
                        isInterrupt       = true;
                        tool_msg.flags   |= neograph::MessageFlag::Interrupt;
                        tool_msg.content  = "[Interrupt]";
                    }
                    co_return true;
                },
                [&](std::string errinfo) -> asio::awaitable<bool> {
                    tool_msg.content = neograph::json{
                        {"error", std::move(errinfo)}
                    }.dump();
                    co_return true;
                }
            );
            if (errorPtr) {
                std::rethrow_exception(errorPtr);
            }
        }
        co_return tool_msg;
    };

    /// 执行 toolcall
    std::vector<asio::awaitable<neograph::ChatMessage>> toolcallResults{};
    for (const auto& tc : assistant_msg->tool_calls) {
        toolcallResults.emplace_back(onExecTool(tc));
    }
    for (auto& item : toolcallResults) {
        // TODO: 真正并行
        auto           msg = co_await std::move(item);
        neograph::json msg_json;
        neograph::to_json(msg_json, msg);
        results.push_back(msg_json);
    }

    if (isInterrupt) {
        // 暂存 toolcall list 结果到 graphData
        agentCtxPtr->middlewareHandleContext->setGraphDataItemValue<neograph::json>(
            in.ctx.thread_id,
            agentxx::middleware::MiddlewareContext::graphDataKey_interruptToolcallCache,
            results
        );
        // 保存当前 messages，供 handler 恢复
        auto messages = in.state.get("messages");
        // 重新抛出异常
        agentCtxPtr->middlewareHandleContext->throwNodeInterruptBase(in.ctx.thread_id, messages);
    }

    out.writes.push_back(neograph::graph::ChannelWrite{"messages", results});
    co_return;
}

void ToolcallWrapNode::defStdoutLogOnToolcallStart(
    neograph::graph::NodeInput& in,
    size_t                      limitOutput
) {
    auto messages = in.state.get_messages();
    auto assistant_msg
        = agentxx::middleware::BaseMiddlewareHandleInterface::getLastAssistantToolcallMessage(
            messages
        );

    std::ostringstream out{};
    if (assistant_msg) {
        size_t index = 0;
        for (auto& item : assistant_msg->tool_calls) {
            ++index;
            out << "┣━ Argument: " << index << ". " << item.name << "/" << item.id << std::endl;
            out << "             - "
                << ((0 == limitOutput) ? item.arguments
                                       : std::string_view{item.arguments}.substr(0, limitOutput))
                << std::endl;
        }
    } else {
        out << "┣━ Empty Argument List\n";
    }

    XX_LOGD(
        R"(
┏━━━━━━ Toolcall ━━━━━━┓
{}
)",
        out.str()
    );
}

void ToolcallWrapNode::defStdoutLogOnToolcallEnd(
    const neograph::graph::NodeInput& in,
    neograph::graph::NodeOutput&      result,
    size_t                            limitOutput
) {
    std::ostringstream out{};
    if (false == result.writes.empty()) {
        size_t index = 0;
        for (auto& item : result.writes) {
            ++index;
            auto str = item.value.dump();
            out << "┣━ Result  : " << index << ". " << item.channel << ": "
                << ((0 == limitOutput) ? str : std::string_view{str}.substr(0, limitOutput))
                << std::endl;
        }
    } else {
        out << "┣━ Empty Result List\n";
    }

    XX_LOGD(
        R"(
{}
┗━━━━━━ Toolcall ━━━━━━┛
)",
        out.str()
    );
}

} // namespace nodes
} // namespace agentxx
