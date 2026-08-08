#include "agentxx/middlewares/event_stream.h"

#include "agentxx/agent/io/agent_io_transport.h"
#include "agentxx/util/diff_util.h"
#include "agentxx/util/log.h"
#include "fmt/format.h"

namespace agentxx {
namespace middleware {

EventStreamInterface::EventStreamInterface(
    std::string_view      in_name,
    const std::type_info& elementType
) :
    name(in_name),
    elementType_(elementType) {}

EventBus::EventBus(asio::any_io_executor executor) :
    executor_(executor) {}

bool EventBus::remove(std::string_view topic) {
    auto it = streams_.find(std::string{topic});
    if (it == streams_.end()) {
        return false;
    }
    streams_.erase(it);
    return true;
}

// ---------------------------------------------------------------------------
// EventBridge: GraphEvent -> 会话增量 Delta + EventBus 发布
// ---------------------------------------------------------------------------

EventBridge::EventBridge(
    std::string                                  agentName,
    std::string                                  threadId,
    std::weak_ptr<agentxx::agent::AgentContext>  ctx,
    std::shared_ptr<agentxx::agent::Session>     session,
    std::shared_ptr<agentxx::agent::AgentIOBase> io,
    neograph::graph::GraphStreamCallback         origCb
) :
    agentName_(std::move(agentName)),
    threadId_(std::move(threadId)),
    ctx_(std::move(ctx)),
    session_(std::move(session)),
    io_(std::move(io)),
    origCb_(std::move(origCb)) {}

void EventBridge::operator()(const neograph::graph::GraphEvent& event) {
    // 先转发原始回调
    if (origCb_) {
        origCb_(event);
    }

    // 统一处理: 每个事件类型仅一个 handler, 同时负责对内 (总线发布) 与对外
    // (会话增量 Delta/历史) 处理, 避免同一事件在多处维护
    using T = neograph::graph::GraphEvent::Type;
    switch (event.type) {
        case T::LLM_TOKEN:
            handleLLMToken(event);
            break;
        case T::CHANNEL_WRITE:
            handleChannelWrite(event);
            break;
        case T::NODE_START:
            handleNodeStart(event);
            break;
        case T::NODE_END:
            handleNodeEnd(event);
            break;
        case T::ERROR:
            handleError(event);
            break;
        default:
            // 未知/未处理事件: 重置 chunk 类型, 使下一个 token 携带时长
            lastChatChunkType_ = neograph::ChatStreamChunk::TYPE_UNKNOWN;
            break;
    }
}

void EventBridge::emitDelta(agentxx::agent::Delta delta) {
    // Delta 流序号: 会话级单调递增 (服务端增量重放缓冲依赖 seq 单调性)
    delta.seq = ++session_->deltaSeq;
    if (io_) {
        io_->sendToPeer(std::move(delta));
    }
}

neograph::graph::GraphStreamCallback EventBridge::makeCallback() {
    auto self = shared_from_this();
    return [self](const neograph::graph::GraphEvent& event) {
        (*self)(event);
    };
}

void EventBridge::handleLLMToken(const neograph::graph::GraphEvent& event) {
    std::string token;
    bool        sendDuration = false;
    std::string kind         = "content";

    if (event.data.is_string()) {
        token              = event.data.get<std::string>();
        lastChatChunkType_ = neograph::ChatStreamChunk::TYPE_CONTENT;
    } else if (event.data.is_object()) {
        neograph::ChatStreamChunk chunk;
        neograph::from_json(event.data, chunk);
        token              = std::move(chunk.data);
        sendDuration       = (lastChatChunkType_ != chunk.type);
        lastChatChunkType_ = chunk.type;
        if (chunk.type == neograph::ChatStreamChunk::TYPE_THINKING) {
            kind = "thinking";
        }
    } else {
        token = event.data.dump();
    }

    // 总线发布 (无订阅者时内部跳过, 零开销)
    publishModelToken(token, kind);

    emitDelta(agentxx::agent::Delta{
        .type        = (lastChatChunkType_ == neograph::ChatStreamChunk::TYPE_THINKING)
                           ? agentxx::agent::Delta::Type::ThinkingToken
                           : agentxx::agent::Delta::Type::TextToken,
        .text        = std::move(token),
        .startTimeMs = nodeStartTimeMs_,
        .durationMs  = sendDuration
                           ? static_cast<int64_t>(
                                 std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now() - nodeStartTime_
                                 )
                                     .count()
                             )
                           : 0,
    });
}

void EventBridge::handleChannelWrite(const neograph::graph::GraphEvent& event) {
    using agentxx::agent::Delta;

    auto chan  = event.data.value("channel", std::string{});
    auto value = event.data.value("value", neograph::json{});

    // 通用提示消息: 转发为 Delta::MessageTip, 由 client 端插入提示消息
    if (chan == "message_tip" && value.is_object()) {
        auto        tipType = Delta::TipType::Info;
        const auto  tip     = value.value("tip_type", std::string{"info"});
        if (tip == "warning") {
            tipType = Delta::TipType::Warning;
        } else if (tip == "error") {
            tipType = Delta::TipType::Error;
        }
        emitDelta(Delta{
            .type    = Delta::Type::MessageTip,
            .text    = value.value("text", std::string{}),
            .tipType = tipType,
        });
        return;
    }

    if (chan != "messages" || !value.is_array()) {
        return;
    }
    bool hasLLMOutput = false;
    for (const auto& jm : value) {
        auto role = jm.value("role", std::string{});
        if (role == "assistant" && jm.contains("tool_calls")) {
            hasLLMOutput = true;
            auto msgId   = session_->appendHistory(jm);
            // 登记 toolCallId → fullHistory 索引, 供 tool 结果 diff 渲染 O(1) 定位
            // (fullHistory append-only, 索引不失效)
            const size_t historyIndex = session_->fullHistory.size() - 1;
            for (const auto& tc : jm["tool_calls"]) {
                const auto tcId = tc.value("id", std::string{});
                if (!tcId.empty()) {
                    toolCallHistoryIndex_[tcId] = historyIndex;
                }
                emitDelta(Delta{
                    .type       = Delta::Type::ToolStart,
                    .msgId      = msgId,
                    .toolName   = tc.value("name", std::string{}),
                    .toolCallId = tcId,
                    .arguments  = tc.value("arguments", std::string{}),
                });
            }
        } else if (role == "tool") {
            auto content    = jm.value("content", std::string{});
            auto toolName   = jm.value("tool_name", std::string{});
            auto toolCallId = jm.value("tool_call_id", std::string{});
            if (toolCallId.empty()) {
                continue;
            }
            auto historyMsg = jm;
            if (toolName == "agentxx_filesystem_edit_text_file") {
                // 生成 diff 记录
                // - 优先 O(1) 索引定位 (本轮 assistant(tool_calls) 已登记),
                //   未命中时回扫兜底 (如历史来自更早轮次)
                const auto tryAppendDiff = [&](const neograph::json& assistantData) -> bool {
                    const auto& tcs
                        = assistantData.value("tool_calls", neograph::json::array());
                    for (const auto& tc : tcs) {
                        if (tc.value("id", std::string{}) != toolCallId) {
                            continue;
                        }
                        // 解析失败的参数 (如非法 JSON) 跳过 diff 渲染
                        agentxx::util::catchError<bool>(
                            [&]() -> bool {
                                auto args = neograph::json::parse(
                                    tc.value("arguments", std::string{})
                                );
                                historyMsg["diff"] = agentxx::util::makeUnifiedDiff(
                                    args.value("old_str", std::string{}),
                                    args.value("new_str", std::string{}),
                                    args.value("path", std::string{})
                                );
                                return true;
                            },
                            [](std::string) -> bool { return false; }
                        );
                        return true;
                    }
                    return false;
                };

                bool found = false;
                if (auto idxIt = toolCallHistoryIndex_.find(toolCallId);
                    idxIt != toolCallHistoryIndex_.end()
                    && idxIt->second < session_->fullHistory.size()) {
                    found = tryAppendDiff(session_->fullHistory[idxIt->second].data);
                }
                if (!found) {
                    for (auto it = session_->fullHistory.rbegin();
                         it != session_->fullHistory.rend();
                         ++it) {
                        const auto& hd = it->data;
                        if (hd.value("role", std::string{}) != "assistant"
                            || !hd.contains("tool_calls")) {
                            continue;
                        }
                        if (tryAppendDiff(hd)) {
                            break;
                        }
                    }
                }
            }
            session_->appendHistory(historyMsg);
            emitDelta(Delta{
                .type       = Delta::Type::ToolEnd,
                .toolName   = toolName,
                .toolCallId = toolCallId,
                .result     = content,
                .hasError   = false,
            });
        } else if (role == "assistant") {
            hasLLMOutput = true;
            session_->appendHistory(jm);
        }
    }
    // llm node 执行完成，推送上下文统计更新
    if (hasLLMOutput && io_ && session_->contextStats) {
        io_->sendToPeer(agentxx::agent::WireContextStats{
            session_->contextStats->contextTokens.load(std::memory_order_relaxed),
            session_->contextStats->maxContextTokens.load(std::memory_order_relaxed),
        });
    }
}

void EventBridge::handleNodeStart(const neograph::graph::GraphEvent& event) {
    lastChatChunkType_ = neograph::ChatStreamChunk::TYPE_UNKNOWN;
    nodeStartTime_     = std::chrono::system_clock::now();
    nodeStartTimeMs_   = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            nodeStartTime_.time_since_epoch()
        )
            .count()
    );
    emitDelta(agentxx::agent::Delta{
        .type        = agentxx::agent::Delta::Type::NodeStart,
        .nodeName    = event.node_name,
        .startTimeMs = nodeStartTimeMs_,
    });
}

void EventBridge::handleNodeEnd(const neograph::graph::GraphEvent& event) {
    lastChatChunkType_ = neograph::ChatStreamChunk::TYPE_UNKNOWN;
    // 计算持续时间
    const int64_t duration_ms = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - nodeStartTime_
        )
            .count()
    );
    emitDelta(agentxx::agent::Delta{
        .type        = agentxx::agent::Delta::Type::NodeEnd,
        .nodeName    = event.node_name,
        .startTimeMs = nodeStartTimeMs_,
        .durationMs  = duration_ms,
    });
}

void EventBridge::handleError(const neograph::graph::GraphEvent& event) {
    // 错误不产出会话增量 Delta (由 WireTurnResult 统一报告), 仅发布总线事件
    lastChatChunkType_ = neograph::ChatStreamChunk::TYPE_UNKNOWN;
    auto msg           = event.data.is_string() ? event.data.get<std::string>() : event.data.dump();
    publishError(std::move(msg), event.node_name);
}

void EventBridge::publishModelToken(const std::string& token, std::string_view kind) {
    auto ctxPtr = ctx_.lock();
    if (!ctxPtr || !ctxPtr->bus) {
        return; // 无 bus, 跳过
    }
    auto  busPtr = ctxPtr->bus;
    auto& bus    = *busPtr;
    // 无订阅者时跳过: 避免每个 token 创建一次无消费者的协程
    // (ModelToken topic 当前无生产订阅者, 该检查使热路径零开销)
    if (false
        == bus.hasSubscribers<agentxx::events::EventModelToken>(
            agentxx::events::Topic::ModelToken
        )) {
        return;
    }
    asio::co_spawn(
        bus.executor(),
        [busPtr,
         agentName = agentName_,
         threadId  = threadId_,
         token, // 捕获副本, 协程生命周期独立于本对象
         kind     = std::string{kind}]() -> asio::awaitable<void> {
            co_await busPtr->publish<agentxx::events::EventModelToken>(
                agentxx::events::Topic::ModelToken,
                agentxx::events::EventModelToken{
                    .agentName = agentName,
                    .threadId  = threadId,
                    .token     = token,
                    .kind      = kind,
                }
            );
        },
        asio::detached
    );
}

void EventBridge::publishError(std::string message, std::string where) {
    auto ctxPtr = ctx_.lock();
    if (!ctxPtr || !ctxPtr->bus) {
        return; // 无 bus, 跳过
    }
    auto  busPtr = ctxPtr->bus;
    auto& bus    = *busPtr;
    // 无订阅者时跳过 (与 ModelToken 一致, 避免无效协程创建)
    if (false
        == bus.hasSubscribers<agentxx::events::EventError>(agentxx::events::Topic::Error)) {
        return;
    }
    asio::co_spawn(
        bus.executor(),
        [busPtr,
         agentName = agentName_,
         threadId  = threadId_,
         message   = std::move(message),
         where     = std::move(where)]() -> asio::awaitable<void> {
            co_await busPtr->publish<agentxx::events::EventError>(
                agentxx::events::Topic::Error,
                agentxx::events::EventError{
                    .agentName = agentName,
                    .threadId  = threadId,
                    .message   = message,
                    .where     = where,
                }
            );
        },
        asio::detached
    );
}

} // namespace middleware
} // namespace agentxx
