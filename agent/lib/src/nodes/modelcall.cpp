#include "agentxx/nodes/modelcall.h"

#include "agentxx/agent/model_registry.h"
#include "agentxx/plugin/tool_registry.h"
#include "agentxx/protocol/openai_provider.h"
#include "agentxx/util/aho_corasick.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include <algorithm>
#include <chrono>
#include <random>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace agentxx {
namespace nodes {

inline static constexpr std::string_view defaultExceptionTip{"[Exception aborted]"};
inline static constexpr std::string_view defaultUserCancelTip{"[User cancelled]"};
inline static constexpr std::string_view defaultContinueTip{"[Please continue]"};

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

// 生成唯一的 tool_call id: 毫秒时间戳 + 32 位随机数
// - 无需与已有 id 比较, 碰撞概率 ~2^-32 (同一毫秒内), 跨毫秒必然不同
// - 相比按下标回填 call_{i}, 不会与 LLM 返回的 call_N 形式 id 冲突
inline static std::string makeUniqueToolCallId(size_t i) {
    thread_local std::mt19937_64 rng{
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())
    };
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
    )
                        .count();
    return fmt::format("call_{}_{}_{:08x}", ts, i, static_cast<uint32_t>(rng()));
}

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
    ModelCallWrapNode::resolveCurrentProvider(std::string_view sessionId) {
    if (useDynamicModel_) {
        auto ctxPtr = agentContext.lock();
        if (ctxPtr && ctxPtr->modelRegistry) {
            std::string selected;
            if (auto session = ctxPtr->sessions->get(sessionId)) {
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

std::string ModelCallWrapNode::resolveCurrentModelName(std::string_view sessionId) const {
    if (useDynamicModel_) {
        auto ctxPtr = agentContext.lock();
        if (ctxPtr && ctxPtr->modelRegistry) {
            std::string selected;
            if (auto session = ctxPtr->sessions->get(sessionId)) {
                selected = session->getModelName();
            }
            const auto& modelName = ctxPtr->modelRegistry->getModelConfig(selected).modelName;
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
    std::string_view                   sessionId
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

    // 动态插件工具: 追加到 LLM 侧工具 schema (热插拔工具对模型可见的关键)
    // - 插件工具注册后, 下一轮 modelcall 即随请求发送给 LLM
    // - 插件工具卸载后, 定义从列表消失, 模型不再选择调用
    if (auto ctxPtr = agentContext.lock()) {
        if (ctxPtr->toolRegistry) {
            ctxPtr->toolRegistry->appendDefinitions(tool_defs);
        }
    }

    neograph::CompletionParams params;
    params.model    = resolveCurrentModelName(sessionId);
    params.messages = std::move(messages);
    params.tools    = std::move(tool_defs);
    return params;
}

asio::awaitable<neograph::graph::NodeOutput>
    ModelCallWrapNode::callLLM(neograph::graph::NodeInput& in) {
    XX_LOGT("ModelCallWrapNode::callLLM START");
    auto params         = build_params(in.state, in.ctx.thread_id);
    params.cancel_token = in.ctx.cancel_token;

    auto completion = co_await onReceiveToken(params, in);
    neograph::graph::record_usage(in.ctx, completion); // #88

    // 部分 OpenAI 兼容 API (如 Ollama) 流式响应不返回 tool_call id，
    // 此处补充合成 ID，确保下游 ToolStart/ToolEnd 能正确关联；
    // 时间戳+随机数生成, 天然唯一, 不会与 LLM 返回的 id 冲突
    size_t index = 0;
    for (auto& tc : completion.message.tool_calls) {
        if (tc.id.empty()) {
            tc.id = makeUniqueToolCallId(++index);
        }
    }

    neograph::json msg_json;
    neograph::to_json(msg_json, completion.message);

    neograph::graph::NodeOutput out;
    out.writes.push_back(
        neograph::graph::ChannelWrite{"messages", neograph::json::array({msg_json})}
    );
    XX_LOGT("ModelCallWrapNode::callLLM END");
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

void ModelCallWrapNode::repairMessages(neograph::graph::NodeInput& in) {
    // 最后一条消息应当是 system/user/tool
    auto lastMsg = agentxx::middleware::BaseMiddlewareHandleInterface::getLastMessage(in);
    if (lastMsg.has_value()) {
        const auto& role = lastMsg.value().role;
        if ("system" == role || "user" == role || "tool" == role) {
            // 无需修复
        } else {
            // 插入 user msg 修正消息顺序
            auto userMsg = neograph::ChatMessage{
                .role    = "user",
                .content = std::string{defaultContinueTip},
                .flags   = neograph::MessageFlag::AutoInserted,
            };
            auto userMsgJson = neograph::json{};
            neograph::to_json(userMsgJson, userMsg);
            in.state.write("messages", neograph::json::array({userMsgJson}));
            // 无需通知 [CHANNEL_WRITE]，避免在 UI 层插入该消息
        }
    }

    auto agentCtxPtr = agentContext.lock();
    if (agentCtxPtr->agentConfig->repairMessages) {
        // - 最终兜底处理，一般生成 message 的代码应该自己处理异常、补充消息
        // - 这里作为最终的预防处理
        // - TODO: 优化仅对末尾消息进行修正
        auto msgs = in.state.get_messages();

        bool haveChange = false;
        // 清理悬挂的 assistant tool_calls:
        // - 取消/异常后重新从图开始节点执行时, 上下文可能以
        //   "assistant(tool_calls) 但无对应 tool 结果" 结尾 (如 toolcall 节点执行前被
        //   取消), 悬挂的 tool_calls 会让 LLM 误以为工具已被调用, 这里清空其 tool_calls
        // - 若所有声明的 tool_call_id 都已有 tool 结果消息 (如 toolcall 内取消时已由
        //   ToolcallWrapNode 补齐 [User canceled]), 则不清空
        {
            // 找最后一条含 tool_calls 的 assistant 消息
            int64_t assistantIndex = -1;
            for (int64_t i = static_cast<int64_t>(msgs.size()) - 1; i >= 0; --i) {
                if ("assistant" == msgs[i].role && !msgs[i].tool_calls.empty()) {
                    assistantIndex = i;
                    break;
                }
            }
            if (assistantIndex >= 0) {
                std::set<std::string> ids;
                for (const auto& tc : msgs[assistantIndex].tool_calls) {
                    if (!tc.id.empty()) {
                        ids.insert(tc.id);
                    }
                }
                std::set<std::string> replied;
                for (size_t i = static_cast<size_t>(assistantIndex) + 1; i < msgs.size(); ++i) {
                    if ("tool" == msgs[i].role && !msgs[i].tool_call_id.empty()) {
                        replied.insert(msgs[i].tool_call_id);
                    }
                }
                const bool allReplied
                    = std::all_of(ids.begin(), ids.end(), [&](const std::string& id) {
                          return replied.count(id) > 0;
                      });
                if (!ids.empty() && !allReplied) {
                    // 存在悬挂的 tool_calls, 清空, 保证上下文角色顺序和内容完整
                    msgs[assistantIndex].tool_calls.clear();
                    XX_LOGD("RepairMessages: clear dangling toolcalls before LLM call");
                    haveChange = true;
                }
            }
        }

        // 检查并修复 tool_call_id 重复:
        // - LLM 可能返回重复的 tool_call id (重试/多轮时复用), provider 自动回填的
        //   call_{i} 也可能与 LLM 返回的 id 冲突, 重复 id 会让 tool 结果消息无法
        //   精确匹配对应的 assistant tool_call
        // - 重命名重复出现的 assistant tool_call id, 并按出现顺序同步更新对应
        //   tool 结果消息的 tool_call_id (第 1 个调用对应第 1 个结果, 依次类推)
        {
            // 重复项位置: (消息索引, tool_calls 内索引, 新 id)
            struct DupToolCall {
                size_t      msgIdx;
                size_t      tcIdx;
                std::string newId;
            };

            // old id -> 重复项列表 (第 2 次及以后出现), 按 assistant 出现顺序精确配对 tool 结果
            std::unordered_map<std::string, std::vector<DupToolCall>> dupMap;
            // old id -> 对应 tool 结果消息索引 (按出现顺序, 仅在第二遍收集)
            std::unordered_map<std::string, std::vector<size_t>> toolResultIdxs;
            // 全局已见 id (assistant tool_calls + tool 结果), 生成新 id 时避开碰撞
            std::unordered_set<std::string> seenIds;
            // 全局去重序号, 避免同毫秒内 makeUniqueToolCallId(ti) 碰撞
            static std::atomic<uint64_t> dupSeq{0};

            // 第一遍: 按 assistant 声明顺序收集重复项, 新 id 用全局 seq 保证唯一且不与 seenIds 碰撞
            for (size_t mi = 0; mi < msgs.size(); ++mi) {
                auto& msg = msgs[mi];
                if ("assistant" == msg.role) {
                    for (size_t ti = 0; ti < msg.tool_calls.size(); ++ti) {
                        const auto& id = msg.tool_calls[ti].id;
                        if (id.empty()) {
                            continue;
                        }
                        if (!seenIds.insert(id).second) {
                            // 第 2 次及以后出现: 生成全局唯一 id, 循环直到与 seenIds 无碰撞
                            std::string newId;
                            do {
                                newId = makeUniqueToolCallId(dupSeq.fetch_add(1));
                            } while (!seenIds.insert(newId).second);
                            // 回退一次插入(上面已插入), 下面 dupMap 持有 newId, seenIds
                            // 已占位防后续碰撞
                            dupMap[id].push_back(DupToolCall{mi, ti, std::move(newId)});
                        }
                    }
                } else if ("tool" == msg.role && !msg.tool_call_id.empty()) {
                    // tool 结果 id 纳入已见, 防止新 id 与之碰撞
                    seenIds.insert(msg.tool_call_id);
                }
            }

            if (!dupMap.empty()) {
                // 第二遍: 收集 tool 结果索引 (按消息顺序)
                for (size_t mi = 0; mi < msgs.size(); ++mi) {
                    if ("tool" == msgs[mi].role && !msgs[mi].tool_call_id.empty()) {
                        toolResultIdxs[msgs[mi].tool_call_id].push_back(mi);
                    }
                }
                // 第三遍: 按 assistant 声明顺序一一回填 tool 结果 (k+1 偏移: 第1个结果对应原始调用)
                // - 若 tool 结果缺失/乱序, 仅对存在的 k+1 位置回填, 不会错配到其他 id
                for (auto& [oldId, infos] : dupMap) {
                    auto                       it = toolResultIdxs.find(oldId);
                    const std::vector<size_t>* resultIdxs
                        = (it == toolResultIdxs.end()) ? nullptr : &it->second;
                    for (size_t k = 0; k < infos.size(); ++k) {
                        auto& info                                  = infos[k];
                        msgs[info.msgIdx].tool_calls[info.tcIdx].id = info.newId;
                        if (resultIdxs && k + 1 < resultIdxs->size()) {
                            msgs[(*resultIdxs)[k + 1]].tool_call_id = info.newId;
                        } else if (resultIdxs && resultIdxs->size() == 1 && k == 0) {
                            // 边界: 仅1个 tool 结果但出现重复 assistant(重试场景),
                            // 不回填保持原结果对应首次调用
                        }
                        XX_LOGW(
                            "RepairMessages: duplicate tool_call_id '{}' -> '{}' (msg {} tc {})",
                            oldId,
                            info.newId,
                            info.msgIdx,
                            info.tcIdx
                        );
                    }
                }
                haveChange = true;
            }
        }

        // 合并连续的 user 消息:
        // - 部分 Provider (如 Anthropic/部分 OpenAI 兼容网关) 要求 user/assistant 交替,
        //   连续 user 会导致 400 ("consecutive user messages" / "unexpected role").
        //   如历史中因重试/中断/客户端竞态出现 user+user, 这里在发送前合并为一条.
        // - 合并策略: content / reasoning_content 以 "\n\n" 拼接, 多模态附件数组直接追加,
        //   history_contents/tool_calls 追加, flags 按位或, extra 对象字段合并(后者覆盖).
        {
            if (msgs.size() >= 2) {
                std::vector<neograph::ChatMessage> merged;
                merged.reserve(msgs.size());
                size_t mergedCount = 0;
                for (auto& m : msgs) {
                    if (!merged.empty() && merged.back().role == "user" && m.role == "user") {
                        auto& prev = merged.back();
                        // content: "\n\n" 拼接, 避免空串产生多余分隔符
                        if (!m.content.empty()) {
                            if (!prev.content.empty()) {
                                prev.content += "\n\n";
                                prev.content += m.content;
                            } else {
                                prev.content = m.content;
                            }
                        }
                        // reasoning_content 同理
                        if (!m.reasoning_content.empty()) {
                            if (!prev.reasoning_content.empty()) {
                                prev.reasoning_content += "\n\n";
                                prev.reasoning_content += m.reasoning_content;
                            } else {
                                prev.reasoning_content = m.reasoning_content;
                            }
                        }
                        // 多模态附件
                        if (!m.image_urls.empty()) {
                            prev.image_urls.insert(
                                prev.image_urls.end(),
                                m.image_urls.begin(),
                                m.image_urls.end()
                            );
                        }
                        if (!m.audio_urls.empty()) {
                            prev.audio_urls.insert(
                                prev.audio_urls.end(),
                                m.audio_urls.begin(),
                                m.audio_urls.end()
                            );
                        }
                        if (!m.video_urls.empty()) {
                            prev.video_urls.insert(
                                prev.video_urls.end(),
                                m.video_urls.begin(),
                                m.video_urls.end()
                            );
                        }
                        if (!m.history_contents.empty()) {
                            prev.history_contents.insert(
                                prev.history_contents.end(),
                                m.history_contents.begin(),
                                m.history_contents.end()
                            );
                        }
                        // user 正常不应带 tool_calls, 但若异常携带则追加保留
                        if (!m.tool_calls.empty()) {
                            prev.tool_calls.insert(
                                prev.tool_calls.end(),
                                m.tool_calls.begin(),
                                m.tool_calls.end()
                            );
                        }
                        // tool_call_id/tool_name 不处理 (user 角色无此字段)
                        // flags 合并
                        prev.flags = prev.flags | m.flags;
                        // extra 合并: 对象则字段级合并, 否则有值时覆盖
                        if (!m.extra.is_null() && !m.extra.empty()) {
                            if (prev.extra.is_null() || prev.extra.empty()) {
                                prev.extra = m.extra;
                            } else if (prev.extra.is_object() && m.extra.is_object()) {
                                for (auto kv : m.extra.items()) {
                                    prev.extra[kv.first] = kv.second;
                                }
                            } else {
                                // 非对象 extra 直接以新值覆盖旧值
                                prev.extra = m.extra;
                            }
                        }
                        ++mergedCount;
                    } else {
                        // 拷贝而非移动: 保持原 msgs 完整, 仅在真正合并时才替换
                        merged.push_back(m);
                    }
                }
                if (merged.size() != msgs.size()) {
                    XX_LOGD(
                        "RepairMessages: merged {} consecutive user message(s): {} -> {}",
                        mergedCount,
                        msgs.size(),
                        merged.size()
                    );
                    msgs       = std::move(merged);
                    haveChange = true;
                }
            }
        }

        auto checkInfo
            = agentCtxPtr->middlewareHandleContext->getGraphDataItemValue<neograph::json>(
                in.ctx.thread_id,
                agentxx::middleware::MiddlewareContext::graphDataKey_messageCheckInfo
            );

        if (checkInfo.contains("message_length") && checkInfo["message_length"].is_number_integer()
            && checkInfo.value<size_t>("message_length", 0) > msgs.size()) {
            XX_LOGE(
                "LLM Messages length reduce: old({}) -> current({})",
                checkInfo.value<size_t>("message_length", 0),
                msgs.size()
            );
        }
        checkInfo["message_length"] = msgs.size();

        for (auto& msg : msgs) {
            bool doPrint = false;
            if (msg.role == "system") {
                if (checkInfo.contains("system_message_length")
                    && checkInfo["system_message_length"].is_number_integer()
                    && checkInfo.value<size_t>("system_message_length", 0) != msg.content.size()) {
                    XX_LOGE(
                        "LLM System-Message content length changed: old({}) -> current({})",
                        checkInfo.value<size_t>("system_message_length", 0),
                        msg.content.size()
                    );
                    doPrint = true;
                }
                checkInfo["system_message_length"] = msg.content.size();
            }
            // 检查消息非空
            if (msg.reasoning_content.empty() && msg.content.empty() && msg.tool_calls.empty()) {
                // 修正
                msg.content = "[Empty]";
                XX_LOGE("  - Message is Empty: ");
                doPrint = true;
            }
            // 检查是否符合 utf8
            if (agentxx::util::utf8Repair(msg.reasoning_content)) {
                XX_LOGE("  - Message.reasoning_content is not utf8 available: ");
                doPrint = true;
            }
            if (agentxx::util::utf8Repair(msg.content)) {
                XX_LOGE("  - Message.content is not utf8 available: ");
                doPrint = true;
            }
            for (auto& tool : msg.tool_calls) {
                if (agentxx::util::utf8Repair(tool.arguments)) {
                    XX_LOGE(
                        "  - Message.toolcall is not utf8 available: {}/{}",
                        tool.name,
                        tool.id
                    );
                    doPrint = true;
                }
            }
            if (doPrint) {
                haveChange = true;
                agentxx::middleware::BaseMiddlewareHandleInterface::printMessage(msg);
            }
        }
        agentCtxPtr->middlewareHandleContext->setGraphDataItemValue<neograph::json>(
            in.ctx.thread_id,
            agentxx::middleware::MiddlewareContext::graphDataKey_messageCheckInfo,
            std::move(checkInfo)
        );

        if (haveChange) {
            // 覆盖回 state
            auto msglist = neograph::json::array();
            neograph::to_json(msglist, msgs);
            in.state.overwrite("messages", std::move(msglist));
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
                          agentxx::middleware::MiddlewareContext::graphDataKey_appendSystemMessage
                      );

            // 组装最终 system prompt:
            // systemPrompt + appendSystemPrompts(有序) + appendSystemMessage(动态 skill/memory)
            // - appendSystemPrompts 为通用扩展点 (planning/skill/codegraph
            // 等均经此注入，键字典序拼接)
            // - 空段不占位，避免无对应工具时误导模型
            std::string combined         = agentCtxPtr->agentConfig->prompt.systemPrompt;
            auto        appendIfNonEmpty = [&](const std::string& seg) {
                if (seg.empty()) {
                    return;
                }
                if (!combined.empty() && combined.back() != '\n') {
                    combined += "\n";
                }
                // 段间用一个空行分隔, 保持可读性
                if (!combined.empty() && combined.size() >= 2
                    && combined.compare(combined.size() - 2, 2, "\n\n") != 0) {
                    combined += "\n";
                }
                combined += seg;
            };
            // 通用附加提示词按固定优先级拼接：planning -> skill -> codegraph -> 其他(字典序)
            // summarization 为压缩模板，不参与 system 拼接
            const auto& appendMap   = agentCtxPtr->agentConfig->prompt.appendSystemPrompts;
            auto        appendByKey = [&](const std::string& key) {
                auto it = appendMap.find(key);
                if (it != appendMap.end()) {
                    appendIfNonEmpty(it->second);
                }
            };
            appendByKey("planning");
            appendByKey("skill");
            appendByKey("codegraph");
            for (const auto& kv : appendMap) {
                if (kv.first == "planning" || kv.first == "skill" || kv.first == "codegraph"
                    || kv.first == "summarization") {
                    continue;
                }
                appendIfNonEmpty(kv.second);
            }
            if (!appendSystemMsgList.empty()) {
                std::string appendJoined = fmt::format("{}", fmt::join(appendSystemMsgList, "\n"));
                appendIfNonEmpty(appendJoined);
            }
            newSystemMsg.content = std::move(combined);
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

    auto ctxPtr = agentContext.lock()->middlewareHandleContext;
    auto timer  = asio::steady_timer(co_await asio::this_coro::executor);
    // 连续重试次数 (用于退避延时计算): 达到配置上限后停止重试, 不因部分输出重置,
    // 保证总失败次数严格不超过 llmMaxRetry, 避免消息无限堆积
    size_t retry = 0;

    // 插入 assistant 兜底消息 (保留部分输出, 或插入异常/取消提示):
    // - 保证消息上下文完整, 用户能看到本次 LLM 调用失败
    // - 保证末尾消息角色为 assistant 且无 tool_calls, 使 [has_tool_calls] 条件
    //   路由到 agent_end 结束本轮, 而不是把悬挂的 tool_calls 误路由回 tools
    //   节点重复执行
    auto appendAbortMessage = [&](const std::string& content, const std::string& thinking) {
        auto msg = neograph::ChatMessage{
            .role              = "assistant",
            .content           = content,
            .reasoning_content = thinking,
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
    };

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

        // 取消埋点: 进入 LLM 调用前检查 (重试路径可能已经处于取消状态)
        if (in.ctx.cancel_token) {
            in.ctx.cancel_token->throw_if_cancelled("before llm call");
        }

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
        } catch (const boost::exception& e) {
            // 注意: boost::exception 需在 std::exception 之前捕获,
            // 同时继承两者的异常 (如 boost::system::system_error) 才能取到完整诊断信息
            errInfo  = agentxx::util::autoTryConvertToUtf8(boost::diagnostic_information(e));
            errorPtr = std::current_exception();
        } catch (const std::exception& e) {
            errInfo  = agentxx::util::autoTryConvertToUtf8(e.what());
            errorPtr = std::current_exception();
        } catch (...) {
            errInfo  = "unknown";
            errorPtr = std::current_exception();
        }

        // 触发异常: 本次调用失败
        auto lastMsgThinking = ctxPtr->getGraphDataItemValue<std::string>(
            in.ctx.thread_id,
            agentxx::middleware::MiddlewareContext::graphDataKey_tempLLMThinking
        );
        auto lastMsgContent = ctxPtr->getGraphDataItemValue<std::string>(
            in.ctx.thread_id,
            agentxx::middleware::MiddlewareContext::graphDataKey_tempLLMContent
        );

        // - 取消 或 连续重试达到配置上限: 停止重试, 抛出原始异常结束本轮执行
        if (isCancel || retry >= agentCtxPtr->agentConfig->llmMaxRetry) {
            // 抛出前检查末尾消息角色: 若末尾不是 assistant, 或末尾 assistant
            // 仍带 tool_calls (悬挂), 插入兜底提示消息
            // - 覆盖 无输出/短输出 失败的情况 (上轮未插入过)
            // - 已有 ≥512 部分输出时, 上轮已插入 assistant 保留消息, 无需重复插入
            // - 保证 [has_tool_calls] 条件路由到 agent_end, 不会把悬挂的
            //   tool_calls 误路由回 tools 节点重复执行
            auto lastMsg = agentxx::middleware::BaseMiddlewareHandleInterface::getLastMessage(in);
            const bool lastIsAssistant  = lastMsg.has_value() && lastMsg->role == "assistant";
            const bool lastHasToolCalls = lastMsg.has_value() && !lastMsg->tool_calls.empty();
            if (false == lastIsAssistant || lastHasToolCalls) {
                appendAbortMessage(
                    fmt::format(
                        "{}\n{}",
                        lastMsgContent,
                        isCancel ? defaultUserCancelTip : defaultExceptionTip
                    ),
                    lastMsgThinking
                );
            }
            std::rethrow_exception(errorPtr);
        }

        // 有部分响应成功 (≥512 字符): 保留已有的 llm 消息而不是丢弃, 插入
        // assistant 消息后继续重试; retry 不重置, 达到配置上限即停止, 避免
        // "部分输出 -> 重置 retry" 导致无限重试与消息无限堆积
        if (lastMsgThinking.size() + lastMsgContent.size() >= 512) {
            appendAbortMessage(
                fmt::format("{}\n{}", lastMsgContent, defaultExceptionTip),
                lastMsgThinking
            );
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
        // 通知 UI 层: LLM API 调用失败, 即将自动重试 (经 base_agent 转为 WireDelta::MessageUITip,
        // 由 client 端 (TUI/stdio) 插入提示消息)
        if (nullptr != in.stream_cb) {
            // 实际等待时长: retry*3 秒 + 限速附加延时 (appendDelay 单位: 秒)
            const auto delaySec = retry * 3 + appendDelay;
            auto       tipJson  = neograph::json{
                       {"channel", "message_tip"},
                       {"value",
                        neograph::json{
                            {"tipType", "warning"},
                            {"text",
                             fmt::format(
                          "LLM API 请求失败，{} 秒后自动重试 ({}/{})，错误: {}",
                          delaySec,
                          retry,
                          agentCtxPtr->agentConfig->llmMaxRetry,
                          errInfo
                      )},
                 }                              },
            };
            (*in.stream_cb)(neograph::graph::GraphEvent{
                neograph::graph::GraphEvent::Type::CHANNEL_WRITE,
                nodeName,
                std::move(tipJson),
            });
        }
        // 逐渐延长延时等待 (appendDelay 单位: 秒, 与 UI 提示 delaySec 一致)
        timer.expires_after(std::chrono::seconds(retry * 3) + std::chrono::seconds(appendDelay));
        co_await timer.async_wait(asio::use_awaitable);
    } while (true);
}

} // namespace nodes
} // namespace agentxx
