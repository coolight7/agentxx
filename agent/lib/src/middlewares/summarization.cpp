#include "agentxx/middlewares/summarization.h"

#include "agentxx/agent/model_registry.h"
#include "agentxx/tools/sub_agent.h"
#include "fmt/format.h"
#include <algorithm>
#include <sstream>

namespace agentxx {
namespace middleware {

SummarizationMiddlewareHandle::SummarizationMiddlewareHandle(
    agentxx::tools::SubAgentManagerTool*        in_subagentManager,
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext,
    size_t                                      in_defaultModelSupportMaxToken,
    double                                      in_asciiCharsPerToken,
    double                                      in_unicodeCharsPerToken,
    double                                      in_tokensPerImage,
    double                                      in_extraTokensPerMessage
) :
    BaseMiddlewareHandle<_SummarizationMiddlewareState>(
        "SummarizationMiddlewareHandle",
        std::move(in_agentContext)
    ),
    subagentManager(in_subagentManager),
    modelSupportMaxTokenDefault(in_defaultModelSupportMaxToken),
    asciiCharsPerToken(in_asciiCharsPerToken),
    unicodeCharsPerToken(in_unicodeCharsPerToken),
    tokensPerImage(in_tokensPerImage),
    extraTokensPerMessage(in_extraTokensPerMessage) {}

size_t SummarizationMiddlewareHandle::countTokensForUtf8Str(std::string_view in_str) {
    size_t unicodeCount = 0, asciiCount = 0;
    for (size_t i = 0, step = 0; i < in_str.size(); i += step) {
        unsigned char byte = in_str[i];
        // lenght 6
        if (byte >= 0xFC) {
            step = 6;
        } else if (byte >= 0xF8) {
            step = 5;
        } else if (byte >= 0xF0) {
            step = 4;
        } else if (byte >= 0xE0) {
            step = 3;
        } else if (byte >= 0xC0) {
            step = 2;
        } else {
            step = 1;
            ++asciiCount;
            continue;
        }
        ++unicodeCount;
    }
    return unicodeCount / unicodeCharsPerToken + asciiCount / asciiCharsPerToken;
}

size_t SummarizationMiddlewareHandle::countTokens(
    const std::vector<std::string>&           systemMsgs,
    const std::vector<neograph::ChatMessage>& messages
) {
    size_t count = 0;
    for (const auto& msg : systemMsgs) {
        count += extraTokensPerMessage + countTokensForUtf8Str(msg);
    }
    for (const auto& item : messages) {
        count += extraTokensPerMessage + countTokensForUtf8Str(item.role)
                 + countTokensForUtf8Str(item.content);
        for (const auto& tool : item.tool_calls) {
            count += countTokensForUtf8Str(tool.id) + countTokensForUtf8Str(tool.name)
                     + countTokensForUtf8Str(tool.arguments);
        }
        count += tokensPerImage * item.image_urls.size();
    }
    return count;
}

std::string SummarizationMiddlewareHandle::messagesToText(
    const std::vector<neograph::ChatMessage>& msgs,
    bool                                      includeSystem
) {
    std::ostringstream oss;
    for (const auto& m : msgs) {
        if (!includeSystem && m.role == "system") {
            continue;
        }
        oss << fmt::format("[{}]: ", m.role) << m.content << std::endl;
        if (!m.tool_calls.empty()) {
            for (const auto& tc : m.tool_calls) {
                oss << fmt::format("  - [toolcall:{}] {}", tc.name, tc.arguments) << std::endl;
            }
        }
    }
    return oss.str();
}

asio::awaitable<std::string> SummarizationMiddlewareHandle::doSummarizeWithLLM(
    const std::vector<neograph::ChatMessage>& messages
) {
    if (nullptr == subagentManager) {
        co_return std::string{};
    }
    auto prompt = messagesToText(messages, false);
    if (prompt.empty()) {
        co_return std::string{};
    }

    try {
        auto args = neograph::json{
            {"subagent", "subagent_task"},
            {"system_prompt",
             R"(
You are a conversation summarizer. 
Summarize the following conversation messages into a concise summary. 
Preserve key decisions, action items, file paths, and important context. 
Output ONLY the summary text, no meta-commentary.
)"},
            {"message", fmt::format("Summarize the following conversation messages:\n\n{}", prompt)
            },
        };
        // TODO: 剥离 tool /manager，避免直接调用 tool
        co_return co_await subagentManager->execute_async(args);
    } catch (const std::exception& e) {
        XX_LOGE("SummarizationMiddlewareHandle llm 压缩失败: {}", e.what());
    }
    co_return std::string{};
}

void SummarizationMiddlewareHandle::offloadLongContentToTempStore(
    neograph::ChatMessage&                    msg,
    const std::shared_ptr<MiddlewareContext>& ctx,
    std::string_view                          thread_id
) {
    if (msg.content.size() <= longContentByteThreshold) {
        return;
    }
    auto id                  = ctx->addShareStoreItemValue(thread_id, msg.content);
    msg.summaryContent       = msg.content;
    msg.content              = fmt::format("[Content offloaded to `share_store`, id={}]", id);
    msg.flags               |= neograph::MessageFlag::ContentOffloaded;
    msg.extra["offload_id"]  = id;
}

void SummarizationMiddlewareHandle::doSummarizeToolcall(std::vector<neograph::ChatMessage>& messages
) {
    auto                          agentCtxPtr = agentContext.lock();
    std::map<std::string, size_t> lastWriteIndex{};
    for (int64_t i = static_cast<int64_t>(messages.size()) - 1; i > 0; --i) {
        auto& msg = messages[i];
        if ("tool" == msg.role) {
            auto itemHandleIt = summarizationToolHandles.find(msg.tool_name);
            if (itemHandleIt != summarizationToolHandles.end()
                && itemHandleIt->second.generateDeduplicationKey
                && itemHandleIt->second.truncateResponse) {
                // 寻找 llm toolcall message
                int lastMsgIndex  = i - 1;
                int toolcallIndex = -1;
                for (; lastMsgIndex > 0; --lastMsgIndex) {
                    for (size_t j = 0; j < messages[lastMsgIndex].tool_calls.size(); ++j) {
                        if (msg.tool_call_id == messages[lastMsgIndex].tool_calls[j].id) {
                            toolcallIndex = j;
                            break;
                        }
                    }
                    if (toolcallIndex >= 0) {
                        break;
                    }
                }

                neograph::json args;
                if (toolcallIndex >= 0) {
                    args = neograph::json::parse(
                        messages[lastMsgIndex].tool_calls[toolcallIndex].arguments
                    );
                }

                auto key = itemHandleIt->second.generateDeduplicationKey(args);
                if (key.has_value()) {
                    if (lastWriteIndex.contains(*key)) {
                        itemHandleIt->second.truncateResponse(msg);
                    } else {
                        lastWriteIndex[*key] = i;
                    }
                }
            }
        } else {
            // assistant
            for (auto& tc : msg.tool_calls) {
                auto itemHandleIt = summarizationToolHandles.find(tc.name);
                if (itemHandleIt != summarizationToolHandles.end()
                    && itemHandleIt->second.generateDeduplicationKey
                    && itemHandleIt->second.truncateRequest) {
                    auto args = neograph::json::parse(tc.arguments);
                    auto key  = itemHandleIt->second.generateDeduplicationKey(args);
                    if (key.has_value()) {
                        if (lastWriteIndex.contains(*key)) {
                            itemHandleIt->second.truncateRequest(tc);
                        } else {
                            lastWriteIndex[*key] = i;
                        }
                    }
                }
            }
        }
    }
}

asio::awaitable<void>
    SummarizationMiddlewareHandle::onModelcallRunFunc(neograph::graph::NodeInput& in) {
    auto agentCtxPtr = agentContext.lock();
    auto messages    = in.state.get_messages();
    if (messages.empty()) {
        co_return;
    }
    // - 接口返回的 token usage，可能不准确，因为 llm node
    // 重试时可能会额外附加消息、也可能是上一轮的 api 返回的，本轮开始已经添加了
    // toolcall / userInput 等消息
    size_t apiTokenUsage = 0;
    {
        auto& apiTokenUsageJson
            = agentCtxPtr->middlewareHandleContext->getGraphDataItemValue<neograph::json>(
                in.ctx.thread_id,
                agentxx::middleware::MiddlewareContext::graphDataKey_LLMTokenUsage
            );
        if (apiTokenUsageJson.is_number_integer()) {
            apiTokenUsage     = apiTokenUsageJson.get<size_t>();
            apiTokenUsageJson = 0;
        }
    }

    const auto& appendSystemPromptList
        = agentCtxPtr->middlewareHandleContext
              ->getGraphDataItemValue<neograph::json>(
                  in.ctx.thread_id,
                  agentxx::middleware::MiddlewareContext::graphDataKey_systemMessage
              )
              .get<std::vector<std::string>>();

    const auto countTokenUsage = countTokens(appendSystemPromptList, messages);
    const auto tokenUsage      = std::max(static_cast<size_t>(apiTokenUsage), countTokenUsage);

    const auto& thread_id = in.ctx.thread_id;

    // 从会话的模型配置提取模型支持的最大 token, 模型配置未指定时使用默认值
    const size_t modelSupportMaxToken = [&]() {
        if (agentCtxPtr->modelRegistry) {
            std::string modelName;
            if (auto session = agentCtxPtr->sessions->get(thread_id)) {
                modelName = session->getModelName();
            }
            auto mc = agentCtxPtr->modelRegistry->getModelConfig(modelName);
            if (mc.modelSupportMaxToken > 0) {
                return mc.modelSupportMaxToken;
            }
        }
        return modelSupportMaxTokenDefault;
    }();

    // 发布上下文统计到对应会话, 供 UI 显示上下文占用百分比
    if (auto session = agentCtxPtr->sessions->get(thread_id)) {
        if (session->contextStats) {
            session->contextStats->contextTokens.store(tokenUsage);
            session->contextStats->maxContextTokens.store(modelSupportMaxToken);
        }
    }

    neograph::json newMsgsJson;
    if (tokenUsage >= modelSupportMaxToken * 0.65) {
        doSummarizeToolcall(messages);
        {
            for (auto& msg : messages) {
                offloadLongContentToTempStore(msg, agentCtxPtr->middlewareHandleContext, thread_id);
            }
        }
        neograph::to_json(newMsgsJson, messages);
    }

    if (tokenUsage >= modelSupportMaxToken * 0.9) {
        const size_t systemCount = (!messages.empty() && messages[0].role == "system") ? 1 : 0;
        // TODO: 如果最近消息+system 已经超过，则无法压缩
        if (messages.size() > keepRecentMessageCount + systemCount) {
            size_t oldEnd = messages.size() - keepRecentMessageCount;
            for (size_t i = oldEnd; i > 0; i--) {
                // - 如果 llm summary 压缩成功，则末尾消息为 assistant，因此需要追加
                // tool/user 类型.
                // - 如果 llm 未压缩，则仍为原消息顺序，截取到哪里都可以.
                const auto& role = messages[i].role;
                if ("tool" == role || "user" == role) {
                    break;
                }
            }

            const size_t oldStart = systemCount;
            const size_t oldCount = oldEnd - oldStart;
            if (oldCount > 0) {
                auto oldMessages = std::vector<neograph::ChatMessage>{
                    messages.begin() + oldStart,
                    messages.begin() + oldEnd
                };
                auto recentMessages
                    = std::vector<neograph::ChatMessage>{messages.begin() + oldEnd, messages.end()};

                /// llm 压缩
                auto summary = co_await doSummarizeWithLLM(oldMessages);

                /// 记录压缩前的历史消息
                auto statePtr = co_await getStateItem(in.ctx.thread_id);
                statePtr->summarizationContext.oldMessagesHistory.push_back(oldMessages);

                std::vector<neograph::ChatMessage> newMessages{};
                if (systemCount > 0) {
                    // 系统消息
                    newMessages.push_back(messages[0]);
                }
                if (!summary.empty()) {
                    // 追加压缩后的信息
                    // system | user | assistant | [user/tool]recentMessages
                    newMessages.push_back(neograph::ChatMessage{
                        .role    = "user",
                        .content = "[Please compact context to save space]",
                        .flags
                        = neograph::MessageFlag::AutoInserted | neograph::MessageFlag::Summarized,
                    });
                    newMessages.push_back(neograph::ChatMessage{
                        .role    = "assistant",
                        .content = "[Previous conversation summary]: \n" + summary,
                        .flags
                        = neograph::MessageFlag::AutoInserted | neograph::MessageFlag::Summarized,
                    });
                } else {
                    // system | oldMessages | recentMessages
                    newMessages.insert(
                        newMessages.end(),
                        std::move_iterator(oldMessages.begin()),
                        std::move_iterator(oldMessages.end())
                    );
                }

                // 添加最近消息
                newMessages.insert(
                    newMessages.end(),
                    std::move_iterator(recentMessages.begin()),
                    std::move_iterator(recentMessages.end())
                );

                neograph::to_json(newMsgsJson, newMessages);
            }
        }
    }

    if (newMsgsJson.is_array() && false == newMsgsJson.empty()) {
        in.state.overwrite("messages", std::move(newMsgsJson));
        if (agentCtxPtr->agentConfig->logPrintSummarizationResultTokenCount) {
            XX_OUT(
                R"_(
┏━━━━━━ Summary ━━━━━━┓
┣━ MAX Token Limit: {}
┣━ Api TokenUsage: {}
┣━ Count Messages Token: {}/{}
┣━ Summary To: {}
┗━━━━━━ Summary ━━━━━━┛)_",
                modelSupportMaxToken,
                apiTokenUsage,
                tokenUsage,
                countTokenUsage,
                countTokens(appendSystemPromptList, in.state.get_messages())
            );
        }
    } else {
        if (agentCtxPtr->agentConfig->logPrintSummarizationResultTokenCount) {
            XX_OUT(
                R"_(
┏━━━━━━ Summary ━━━━━━┓
┣━ MAX Token Limit: {}
┣━ Api TokenUsage: {}
┣━ Count Messages Token: {}/{}
┣━ Not Need Summary
┗━━━━━━ Summary ━━━━━━┛)_",
                modelSupportMaxToken,
                apiTokenUsage,
                tokenUsage,
                countTokenUsage,
                countTokens(appendSystemPromptList, in.state.get_messages())
            );
        }
    }

    co_return;
}

} // namespace middleware
} // namespace agentxx
