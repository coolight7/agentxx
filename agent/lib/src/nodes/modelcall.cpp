#include "agentxx/nodes/modelcall.h"

#include "agentxx/agent/model_registry.h"
#include "agentxx/protocol/openai_provider.h"
#include "agentxx/util/aho_corasick.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"

namespace agentxx {
namespace nodes {

// 限速
inline static const auto defaultRateLimitTag = agentxx::util::AhoCorasick<char>{
    std::vector<std::string>{
                             "429", "rate limit",
                             "has been exhausted", "insufficient",
                             "速率限制", "限速",
                             "请求频率", "已耗尽",
                             "已用完"
    },
    true
};

ModelCallWrapNode::ModelCallWrapNode(
    std::string_view                            name,
    const neograph::graph::NodeContext&         ctx,
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    WrapHandleBaseNode<neograph::graph::LLMCallNode>(name, in_agentContext, ctx) {
    if (ctx.extra_config.is_object() && ctx.extra_config.contains(defUseModelRegistryKey)) {
        useDynamicModel_ = ctx.extra_config.at(defUseModelRegistryKey).get<bool>();
    }
}

std::shared_ptr<neograph::Provider>
    ModelCallWrapNode::resolveCurrentProvider(std::string_view threadId) {
    if (useDynamicModel_) {
        auto ctxPtr = agentContext.lock();
        if (ctxPtr && ctxPtr->modelRegistry) {
            std::string selected;
            if (auto session = ctxPtr->sessions->get(threadId)) {
                selected = session->getModelName();
            }
            auto provider = ctxPtr->modelRegistry->getProvider(selected);
            if (provider) {
                return provider;
            }
        }
    }
    return provider_;
}

std::string ModelCallWrapNode::resolveCurrentModelName(std::string_view threadId) const {
    if (useDynamicModel_) {
        auto ctxPtr = agentContext.lock();
        if (ctxPtr && ctxPtr->modelRegistry) {
            std::string selected;
            if (auto session = ctxPtr->sessions->get(threadId)) {
                selected = session->getModelName();
            }
            auto modelName = ctxPtr->modelRegistry->getModelConfig(selected).modelName;
            if (false == modelName.empty()) {
                return modelName;
            }
        }
    }
    return model_;
}

asio::awaitable<neograph::ChatCompletion> ModelCallWrapNode::onReceiveToken(
    neograph::CompletionParams& params,
    neograph::graph::NodeInput& input
) {
    auto ctxPtr = agentContext.lock()->middlewareHandleContext;

    auto                               callback = input.stream_cb;
    neograph::FormatDataStreamCallback onToken;
    if (nullptr != callback) {
        onToken = [&input, callback, ctxPtr, this](const neograph::ChatStreamChunk& token) {
            switch (token.type) {
                case neograph::ChatStreamChunk::TYPE_CONTENT: {
                    // 记录 本次请求的临时LLM消息，以便触发异常时处理
                    ctxPtr->modifyGraphDataItemValue<std::string>(
                        input.ctx.thread_id,
                        agentxx::middleware::MiddlewareContext::graphDataKey_tempLLMContent,
                        [&token](std::string& msg) {
                            msg += token.data;
                        }
                    );
                } break;
                case neograph::ChatStreamChunk::TYPE_THINKING: {
                    ctxPtr->modifyGraphDataItemValue<std::string>(
                        input.ctx.thread_id,
                        agentxx::middleware::MiddlewareContext::graphDataKey_tempLLMThinking,
                        [&token](std::string& msg) {
                            msg += token.data;
                        }
                    );
                } break;
            }

            if (nullptr != callback) {
                neograph::json json;
                neograph::to_json(json, token);
                (*callback)(neograph::graph::GraphEvent{
                    neograph::graph::GraphEvent::Type::LLM_TOKEN,
                    nodeName,
                    json,
                });
            }
        };
    }

    auto completion
        = co_await resolveCurrentProvider(input.ctx.thread_id)->invoke_format_data(params, onToken);

    // 记录 token使用量
    ctxPtr->setGraphDataItemValue<int>(
        input.ctx.thread_id,
        agentxx::middleware::MiddlewareContext::graphDataKey_LLMTokenUsage,
        completion.usage.total_tokens
    );
    co_return completion.message;
}

neograph::CompletionParams ModelCallWrapNode::build_params(
    const neograph::graph::GraphState& state,
    std::string_view                   threadId
) const {
    auto messages = state.get_messages();

    // Ensure exactly one system message, carrying `instructions_` (issue #93).
    //
    // The contract: when a node is configured with instructions, the model sees
    // those instructions as its single system prompt. State that already holds
    // a system message — seeded by the caller, or restored from a checkpoint
    // written when instructions_ was different — is replaced, not stacked on
    // top of. Two system messages is malformed for a single-system-prompt API
    // such as Anthropic's, and undefined for the OpenAI family.
    //
    // Replacing (rather than deferring to the state's message) is also what the
    // previous code effectively did: it inserted instructions_ at position 0,
    // ahead of any existing system message, so instructions already won on
    // precedence. Only the duplicate goes away.
    if (!instructions_.empty()) {
        // [@coolight] 当存在 system 消息时不再添加
        bool has_system = !messages.empty() && messages[0].role == "system";
        if (!has_system) {
            neograph::ChatMessage sys;
            sys.role    = "system";
            sys.content = instructions_;
            messages.insert(messages.begin(), sys);
        }
    }

    // Build tool definitions
    std::vector<neograph::ChatTool> tool_defs;
    tool_defs.reserve(tools_.size());
    for (auto* tool : tools_) {
        tool_defs.push_back(tool->get_definition());
    }

    neograph::CompletionParams params;
    params.model    = resolveCurrentModelName(threadId);
    params.messages = std::move(messages);
    params.tools    = std::move(tool_defs);
    return params;
}

asio::awaitable<neograph::graph::NodeOutput>
    ModelCallWrapNode::callLLM(neograph::graph::NodeInput& in) {
    auto params         = build_params(in.state, in.ctx.thread_id);
    params.cancel_token = in.ctx.cancel_token;

    auto completion = co_await onReceiveToken(params, in);
    neograph::graph::record_usage(in.ctx, completion); // #88

    // 部分 OpenAI 兼容 API (如 Ollama) 流式响应不返回 tool_call id，
    // 此处补充合成 ID，确保下游 ToolStart/ToolEnd 能正确关联
    for (size_t i = 0; i < completion.message.tool_calls.size(); ++i) {
        auto& tc = completion.message.tool_calls[i];
        if (tc.id.empty()) {
            tc.id = "call_" + std::to_string(i);
        }
    }

    neograph::json msg_json;
    neograph::to_json(msg_json, completion.message);

    neograph::graph::NodeOutput out;
    out.writes.push_back(
        neograph::graph::ChannelWrite{"messages", neograph::json::array({msg_json})}
    );
    co_return out;
}

asio::awaitable<void> ModelCallWrapNode::onHandleStart(
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    neograph::graph::NodeInput&                         in
) {
    co_await item.onModelcallStartFunc(in);
}

asio::awaitable<void> ModelCallWrapNode::onHandleEnd(
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    const neograph::graph::NodeInput&                   in,
    neograph::graph::NodeOutput&                        result
) {
    co_await item.onModelcallEndFunc(in, result);
}

void ModelCallWrapNode::onHandleStartError(
    bool                                                errorRethrow,
    bool                                                isCurrentError,
    std::string_view                                    exceptionStr,
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    neograph::graph::NodeInput&                         in,
    neograph::graph::NodeOutput&                        result
) noexcept {
    // 插入消息，保证消息顺序正确
    // 不会记录 toolcall
    if (false == errorRethrow && isCurrentError) {
        auto msg = neograph::ChatMessage{
            .role    = "assistant",
            .content = neograph::json{
                           {"error",
                            fmt::format(
                                "{}/Start call `{}` exception: {}",
                                nodeName,
                                item.name,
                                exceptionStr
                            )},
            }
                           .dump(),
        };
        auto msgJson = neograph::json{};
        neograph::to_json(msgJson, msg);
        result.writes.push_back(neograph::graph::ChannelWrite{
            "messages",
            neograph::json::array({msgJson}),
        });
    }
}

void ModelCallWrapNode::onHandleBaseRunError(
    bool                         errorRethrow,
    bool                         isCurrentError,
    std::string_view             exceptionStr,
    neograph::graph::NodeInput&  in,
    neograph::graph::NodeOutput& result
) noexcept {
    // 插入消息，保证消息顺序正确
    if (false == errorRethrow && isCurrentError) {
        auto msg = neograph::ChatMessage{
            .role    = "assistant",
            .content = fmt::format("[Exception aborted]"),
            .flags   = neograph::MessageFlag::AutoInserted,
        };
        auto msgJson = neograph::json{};
        neograph::to_json(msgJson, msg);
        result.writes.push_back(neograph::graph::ChannelWrite{
            "messages",
            neograph::json::array({msgJson}),
        });
    }
}

void ModelCallWrapNode::repairMessages(neograph::graph::NodeInput& in) {
    // 最后一条消息应当是 system/user/toolcall
    auto lastMsg = agentxx::middleware::BaseMiddlewareHandleInterface::getLastMessage(in);
    if (lastMsg.has_value()) {
        const auto& role = lastMsg.value().role;
        if ("system" == role || "user" == role || "tool" == role) {
            return;
        }
        // 插入 user msg 修正消息顺序
        auto userMsg = neograph::ChatMessage{
            .role    = "user",
            .content = "[Please continue]",
            .flags   = neograph::MessageFlag::AutoInserted,
        };
        auto userMsgJson = neograph::json{};
        neograph::to_json(userMsgJson, userMsg);
        in.state.write("messages", neograph::json::array({userMsgJson}));
        // 无需通知 [CHANNEL_WRITE]，避免在 UI 层插入该消息
    }

    auto agentCtxPtr = agentContext.lock();
    if (agentCtxPtr->agentConfig->checkMessagesBeforeLLM) {
        auto msgs = in.state.get_messages();
        for (const auto& msg : msgs) {
            bool doPrint = false;
            // 检查消息非空
            if (msg.reasoning_content.empty() && msg.content.empty() && msg.tool_calls.empty()) {
                XX_LOGE("  - Message is Empty: ");
                doPrint = true;
            }
            // 检查是否符合 utf8
            if (false == agentxx::util::utf8IsAvail(msg.reasoning_content)) {
                XX_LOGE("  - Message.reasoning_content is not utf8 available: ");
                doPrint = true;
            }
            if (false == agentxx::util::utf8IsAvail(msg.content)) {
                XX_LOGE("  - Message.content is not utf8 available: ");
                doPrint = true;
            }
            for (const auto& tool : msg.tool_calls) {
                if (false == agentxx::util::utf8IsAvail(tool.arguments)) {
                    XX_LOGE(
                        "  - Message.toolcall is not utf8 available: {}/{}",
                        tool.name,
                        tool.id
                    );
                    doPrint = true;
                }
            }
            if (doPrint) {
                agentxx::middleware::BaseMiddlewareHandleInterface::printMessage(msg);
            }
        }
    }
}

asio::awaitable<void> ModelCallWrapNode::baseRun(
    std::vector<std::shared_ptr<agentxx::middleware::BaseMiddlewareHandleInterface>>& handles,
    neograph::graph::NodeInput&                                                       in,
    neograph::graph::NodeOutput&                                                      result
) {
    auto agentCtxPtr = agentContext.lock();

    {
        // 添加 system Msg
        auto msglist       = in.state.get("messages");
        bool haveSystemMsg = false;
        auto newSystemMsg  = neograph::ChatMessage{.role = "system"};
        if (msglist.is_array() && false == msglist.empty()) {
            auto systemMsg = neograph::ChatMessage{};
            neograph::from_json(msglist.front(), systemMsg);
            if (systemMsg.role == "system") {
                haveSystemMsg = true;
                newSystemMsg  = std::move(systemMsg);
            }
        }

        {
            const auto& appendSystemMsgList
                = agentCtxPtr->middlewareHandleContext
                      ->getGraphDataItemValue<std::vector<std::string>>(
                          in.ctx.thread_id,
                          agentxx::middleware::MiddlewareContext::graphDataKey_systemMessage
                      );

            // 清空原本的 content
            newSystemMsg.content = "";
            std::ostringstream oss;
            oss << agentCtxPtr->agentConfig->prompt.systemPrompt;

            for (const auto& item : appendSystemMsgList) {
                oss << item << "\n";
            }

            newSystemMsg.content = oss.str();
        }

        neograph::json sysMsgJson;
        neograph::to_json(sysMsgJson, newSystemMsg);
        if (haveSystemMsg) {
            // 替换 system msg
            msglist[0] = std::move(sysMsgJson);
        } else {
            // 缺少 system msg，在开头插入
            auto newlist = neograph::json::array();
            newlist.push_back(std::move(sysMsgJson));
            for (auto item : msglist.items()) {
                newlist.push_back(std::move(item.second));
            }
            msglist = std::move(newlist);
        }
        in.state.overwrite("messages", std::move(msglist));
    }

    auto   ctxPtr = agentContext.lock()->middlewareHandleContext;
    auto   timer  = asio::steady_timer(co_await asio::this_coro::executor);
    size_t retry  = 0;

    do {
        // 清理过时的 临时 LLM 消息
        ctxPtr->removeGraphDataItem(
            in.ctx.thread_id,
            agentxx::middleware::MiddlewareContext::graphDataKey_tempLLMThinking
        );
        ctxPtr->removeGraphDataItem(
            in.ctx.thread_id,
            agentxx::middleware::MiddlewareContext::graphDataKey_tempLLMContent
        );
        // 重试时 messages 可能已经发生更改，因此允许在重试时再次执行
        // 但要实现 [handle->onModelcallRunFunc] 的地方自己保证重复执行没有问题
        for (auto& handle : handles) {
            co_await handle->onModelcallRunFunc(in);
        }
        // 修正上下文角色顺序
        repairMessages(in);

        bool               isCancel = false;
        std::string        errInfo;
        std::exception_ptr errorPtr;

        try {
            // 触发异常时，本次 LLM 消息不会添加到 result 中，因此需要额外处理
            result = co_await callLLM(in);
            co_return;
        } catch (const neograph::graph::CancelledException&) {
            isCancel = true;
            errInfo  = "Cancelled";
            errorPtr = std::current_exception();
            // } catch (const neograph::graph::NodeInterrupt&) {
            // isCancel = true;
            // llm node 无 Interrupt
        } catch (const std::exception& e) {
            errInfo  = e.what();
            errorPtr = std::current_exception();
        } catch (const boost::exception& e) {
            errInfo  = boost::diagnostic_information(e);
            errorPtr = std::current_exception();
        } catch (...) {
            errInfo  = "unknown";
            errorPtr = std::current_exception();
        }

        // 触发异常
        auto lastMsgThinking = ctxPtr->getGraphDataItemValue<std::string>(
            in.ctx.thread_id,
            agentxx::middleware::MiddlewareContext::graphDataKey_tempLLMThinking
        );
        auto lastMsgContent = ctxPtr->getGraphDataItemValue<std::string>(
            in.ctx.thread_id,
            agentxx::middleware::MiddlewareContext::graphDataKey_tempLLMContent
        );
        if (lastMsgThinking.size() + lastMsgContent.size() >= 512) {
            // - 保留已有的 llm 消息，而不是丢弃
            // - 插入 assistant 消息，此时末尾消息为 assistant, 将在下一次进入
            // baseRun 时自动修复上下文角色顺序 [repairMessages]
            // TODO: 修正消息上下文，应当与客户端同步信息
            auto msg = neograph::ChatMessage{
                .role    = "assistant",
                .content = fmt::format(
                    "{}\n{}",
                    std::move(lastMsgContent),
                    isCancel ? "[User cancelled]" : "[Exception aborted]"
                ),
                .reasoning_content = std::move(lastMsgThinking),
                .flags             = neograph::MessageFlag::AutoInserted,
            };
            auto msgJson = neograph::json{};
            neograph::to_json(msgJson, msg);
            auto appendMsgJsons = neograph::json::array({msgJson});
            in.state.write("messages", appendMsgJsons);
            if (nullptr != in.stream_cb) {
                (*in.stream_cb)(neograph::graph::GraphEvent{
                    neograph::graph::GraphEvent::Type::CHANNEL_WRITE,
                    nodeName,
                    std::move(appendMsgJsons),
                });
            }
        }

        if (isCancel || retry >= agentCtxPtr->agentConfig->llmMaxRetry) {
            std::rethrow_exception(errorPtr);
        }

        // 自动重试
        retry++;
        size_t appendDelay = 0;
        if (defaultRateLimitTag.contains(errInfo)) {
            // 限速，增加延时
            appendDelay = retry * 5;
        }
        XX_LOGD(
            "LLMCallNode Retry delay {} seconds: {}/{} | {}",
            retry + appendDelay,
            retry,
            agentCtxPtr->agentConfig->llmMaxRetry,
            errInfo
        );
        // 逐渐延长延时等待
        timer.expires_after(std::chrono::milliseconds((retry + appendDelay) * 1000));
        co_await timer.async_wait(asio::use_awaitable);
    } while (true);
}

} // namespace nodes
} // namespace agentxx
