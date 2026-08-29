#include "agentxx/event/event_stream.h"

#include "agentxx/agent/io/agent_io_transport.h"
#include "agentxx/middlewares/summarization.h"
#include "agentxx/util/container_util.h"
#include "agentxx/util/log.h"
#include "fmt/format.h"

namespace agentxx {
namespace event {

EventStreamInterface::EventStreamInterface(
    std::string_view      in_name,
    const std::type_info& elementType
) :
    name(in_name),
    elementType_(elementType) {}

EventBus::EventBus(asio::any_io_executor executor) :
    executor_(executor) {}

bool EventBus::remove(std::string_view topic) {
    return util::eraseHeterogeneous(streams_, topic);
}

// ---------------------------------------------------------------------------
// EventBridge: GraphEvent -> 会话增量 Delta + EventBus 发布
// ---------------------------------------------------------------------------

EventBridge::EventBridge(
    std::string                                  agentName,
    std::string                                  sessionId,
    std::weak_ptr<agentxx::agent::AgentContext>  ctx,
    std::shared_ptr<agentxx::agent::Session>     session,
    std::shared_ptr<agentxx::agent::AgentIOBase> io,
    neograph::graph::GraphStreamCallback         origCb
) :
    agentName_(std::move(agentName)),
    sessionId_(std::move(sessionId)),
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
            // 未知/未处理事件: 重置 chunk 类型, 使下一个 token 视为新流开始
            // (tps 计时重置; THINKING 段由 thinkSegActive_ 跟踪, 不受影响)
            lastChatChunkType_ = neograph::ChatStreamChunk::TYPE_UNKNOWN;
            break;
    }
}

void EventBridge::emitDelta(agentxx::agent::Delta delta) {
    // Delta 流序号: 会话级单调递增 (服务端增量重放缓冲依赖 seq 单调性)
    // 统一经 Session::nextDeltaSeq 分配 (与 SessionServerAgentIO 的新产出
    // Delta 共用同一入口, 保证所有新 Delta 都携带有效 seq 入重放缓冲)
    delta.seq = session_->nextDeltaSeq();
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
    std::string kind = "content";
    // 进入前的 chunk 类型: 用于检测 THINKING 流段的进入/离开
    const bool prevWasThinking = (lastChatChunkType_ == neograph::ChatStreamChunk::TYPE_THINKING);

    // tps 统计: 新 ModelCall 流开始 (节点开始/结束后的首个 token) 时重置计时与计数
    if (lastChatChunkType_ == neograph::ChatStreamChunk::TYPE_UNKNOWN) {
        tpsStartTime_     = std::chrono::steady_clock::now();
        tpsTokenCount_    = 0.0;
        tpsLastPushSec_   = 0.0;
        tpsLastPushToken_ = 0.0;
    }

    if (event.data.is_string()) {
        token              = event.data.get<std::string>();
        lastChatChunkType_ = neograph::ChatStreamChunk::TYPE_CONTENT;
    } else if (event.data.is_object()) {
        neograph::ChatStreamChunk chunk;
        neograph::from_json(event.data, chunk);
        token              = std::move(chunk.data);
        lastChatChunkType_ = chunk.type;
        if (chunk.type == neograph::ChatStreamChunk::TYPE_THINKING) {
            kind = "thinking";
        }
    } else {
        token = event.data.dump();
    }

    // THINKING 流段跟踪:
    // - 进入 THINKING: 记录段起点 (think 耗时从此刻起算, 不随 token 携带时长,
    //   避免"流式刚开始就显示耗时")
    // - 离开 THINKING (切换到正文): 先结算 think 段耗时 —— 在正文首个 token 的
    //   Delta 之前发送空文本 ThinkToken 结算包, client 收到后为已落盘的 Think
    //   消息回填最终时长 ("输出完成时才计算并显示")
    const bool curIsThinking = (lastChatChunkType_ == neograph::ChatStreamChunk::TYPE_THINKING);
    if (!prevWasThinking && curIsThinking) {
        thinkSegActive_  = true;
        thinkSegStart_   = std::chrono::system_clock::now();
        thinkSegStartMs_ = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(thinkSegStart_.time_since_epoch())
                .count()
        );
    } else if (prevWasThinking && !curIsThinking && thinkSegActive_) {
        finalizeThinkSegment();
    }

    // 累计估算 token 数: 批量估算 (每16 token推送窗口再估, 减少 countTokens 遍历)
    // 此处仅累加字符数, 推送时统一折算 token, 降低每 token 的估算开销
    tpsPendingChars_ += token.size();
    // 每 16 token (估算 len * 4) 或窗口到期再做一次 countTokens 批量折算
    constexpr size_t kBatchChars = 64;
    if (tpsPendingChars_ >= kBatchChars) {
        // 近似折算: 批量按比例估算, 避免逐 token 遍历
        tpsTokenCount_   += static_cast<double>(tpsPendingChars_) / 4.0;
        tpsPendingChars_  = 0;
    }
    // 定时推送一次平均速度 (token/s) - push 时会把 pending 一并结算
    pushTpsIfDue();

    // 总线发布
    publishModelToken(token, kind);

    std::optional<agentxx::agent::ViewMessage::ThinkData> thinkData;
    if (lastChatChunkType_ == neograph::ChatStreamChunk::TYPE_THINKING) {
        if (token.empty()) {
            thinkData = agentxx::agent::ViewMessage::ThinkData{
                .reasoningTokens = 0,
                .isEncrypted     = true,
            };
        }
    }

    emitDelta(agentxx::agent::Delta{
        .type        = (lastChatChunkType_ == neograph::ChatStreamChunk::TYPE_THINKING)
                           ? agentxx::agent::Delta::Type::ThinkToken
                           : agentxx::agent::Delta::Type::TextToken,
        .text        = std::move(token),
        .think       = std::move(thinkData),
        .startTimeMs = nodeStartTimeMs_,
        // token Delta 不再携带 durationMs: think 耗时由 finalizeThinkSegment()
        // 在段落完成时以独立结算包发送 (见 handleLLMToken 内 THINKING 流段跟踪)
    });
}

void EventBridge::finalizeThinkSegment() {
    if (!thinkSegActive_) {
        return;
    }
    thinkSegActive_ = false;
    // 结算包: 空文本 ThinkToken, 仅携带该思考段的开始时间与总耗时。
    // client (TUI) 对空文本 ThinkToken 的处理 = 先落盘累积中的 Think 流文本,
    // 再向前回溯最近一条 Think 消息回填 startTimeMs/durationMs —— 即"输出完成时
    // 才计算这条消息的耗时并显示"
    const int64_t durationMs
        = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now() - thinkSegStart_
        )
                                   .count());
    emitDelta(agentxx::agent::Delta{
        .type        = agentxx::agent::Delta::Type::ThinkToken,
        .text        = {},
        .startTimeMs = thinkSegStartMs_,
        .durationMs  = durationMs,
    });
}

void EventBridge::handleChannelWrite(const neograph::graph::GraphEvent& event) {
    using agentxx::agent::Delta;
    using agentxx::agent::ViewMessage;

    // 最终 assistant 消息写出即生成结束: 先结算未闭合的 THINKING 流段
    // (典型场景: 思考后直接发起 tool_calls, 无正文 token 触发类型切换,
    // 结算包必须先于 ToolStart Delta 到达, client 才能回填 Think 时长)
    finalizeThinkSegment();

    auto chan  = event.data.value("channel", std::string{});
    auto value = event.data.value("value", neograph::json{});

    // 通用提示消息: 转发为 Delta::MessageUITip, 由 client 端插入提示消息
    if (chan == "message_tip" && value.is_object()) {
        auto       tipType = Delta::TipType::Info;
        const auto tip     = value.value("tipType", std::string{"info"});
        if (tip == "warning") {
            tipType = Delta::TipType::Warning;
        } else if (tip == "error") {
            tipType = Delta::TipType::Error;
        }
        emitDelta(Delta{
            .type    = Delta::Type::MessageUITip,
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
        if (role == "assistant") {
            hasLLMOutput = true;
            // 展开: reasoning_content 非空 / 加密思考 → Think (折叠); content 非空 →
            // Assistant 消息; 每条 tool_call → Tool 消息 (未完成, 历史默认折叠)。
            // 顺序与渲染端拆解一致: Think 在前, Assistant 在中, Tool 在后。
            // 展开语义与渲染端 (TUI) 同步一致: 历史消息直接就是渲染消息,
            // client 端无需再按 json 拆解
            auto reasoning       = jm.value("reasoning_content", std::string{});
            int  reasoningTokens = 0;
            bool isEncrypted     = false;
            if (jm.contains("extra") && jm["extra"].is_object()) {
                reasoningTokens = jm["extra"].value("reasoning_tokens", 0);
                if (jm["extra"].contains("responses_reasoning_items")
                    && jm["extra"]["responses_reasoning_items"].is_array()
                    && !jm["extra"]["responses_reasoning_items"].empty()) {
                    isEncrypted = true;
                }
            }
            if (!reasoning.empty() || isEncrypted || reasoningTokens > 0) {
                auto m = ViewMessage::makeText(
                    ViewMessage::Role::Think,
                    reasoning,
                    jm.value("startTimeMs", int64_t{0}),
                    jm.value("durationMs", int64_t{0})
                );
                if (isEncrypted || reasoningTokens > 0) {
                    m.think = ViewMessage::ThinkData{
                        .reasoningTokens = reasoningTokens,
                        .isEncrypted     = isEncrypted,
                    };
                }
                m.collapsed = true;
                session_->appendViewMessage(std::move(m));
                if (isEncrypted || reasoningTokens > 0) {
                    emitDelta(Delta{
                        .type        = Delta::Type::ThinkToken,
                        .text        = "",
                        .think       = ViewMessage::ThinkData{
                            .reasoningTokens = reasoningTokens,
                            .isEncrypted     = isEncrypted,
                        },
                        .startTimeMs = jm.value("startTimeMs", int64_t{0}),
                        .durationMs  = jm.value("durationMs", int64_t{0}),
                    });
                }
            }
            auto content = jm.value("content", std::string{});
            if (!content.empty()) {
                auto m = ViewMessage::makeText(
                    ViewMessage::Role::Assistant,
                    content,
                    jm.value("startTimeMs", int64_t{0}),
                    jm.value("durationMs", int64_t{0})
                );
                session_->appendViewMessage(std::move(m));
            }
            if (jm.contains("tool_calls") && jm["tool_calls"].is_array()) {
                for (const auto& tc : jm["tool_calls"]) {
                    const auto toolName   = tc.value("name", std::string{});
                    const auto toolCallId = tc.value("id", std::string{});
                    const auto arguments  = tc.value("arguments", std::string{});

                    const int64_t startMs = static_cast<int64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()
                        )
                            .count()
                    );
                    ViewMessage m;
                    m.role      = ViewMessage::Role::Tool;
                    m.text      = arguments;
                    m.collapsed = true; // 历史重连默认折叠 (与 onDelta 实时展开不同)
                    m.tool      = ViewMessage::ToolData{};
                    m.tool->toolName   = toolName;
                    m.tool->toolCallId = toolCallId;
                    m.startTimeMs      = startMs;
                    const auto msgId   = session_->appendViewMessage(std::move(m));
                    // 登记 toolCallId → viewMessages 索引, 供 tool 结果回填 O(1) 定位
                    // (viewMessages append-only, 索引不失效)
                    const size_t historyIndex = session_->viewMessages.size() - 1;
                    if (!toolCallId.empty()) {
                        toolCallHistoryIndex_[toolCallId] = historyIndex;
                    }
                    emitDelta(Delta{
                        .type        = Delta::Type::ToolStart,
                        .msgId       = msgId,
                        .toolName    = toolName,
                        .toolCallId  = toolCallId,
                        .arguments   = arguments,
                        .startTimeMs = startMs,
                    });
                }
            }
        } else if (role == "tool") {
            auto content = jm.value("content", std::string{});
            // 注意: 消息 JSON 由 neograph::ChatMessage::to_json 序列化, tool 结果
            // 的字段名为 snake_case (tool_name/tool_call_id); 兼容读取 camelCase
            // (手工构造 JSON 写入 channel 时可能使用), 避免 ToolEnd 关联失败
            auto toolName   = jm.value("tool_name", jm.value("toolName", std::string{}));
            auto toolCallId = jm.value("tool_call_id", jm.value("toolCallId", std::string{}));
            if (toolCallId.empty()) {
                continue;
            }
            int64_t toolStartTimeMs = 0;
            int64_t toolDurationMs  = 0;
            if (jm.contains("extra") && jm["extra"].is_object()) {
                toolStartTimeMs = jm["extra"].value("startTimeMs", int64_t{0});
                toolDurationMs  = jm["extra"].value("durationMs", int64_t{0});
            }
            if (toolStartTimeMs == 0) {
                toolStartTimeMs = jm.value("startTimeMs", int64_t{0});
            }
            if (toolDurationMs == 0) {
                toolDurationMs = jm.value("durationMs", int64_t{0});
            }
            // 回填对应的 Tool 消息 (assistant tool_calls 展开时已登记索引):
            // - 优先 O(1) 索引定位 (本轮已登记)
            // - 未命中时回扫兜底 (如历史来自更早轮次/恢复中断场景)
            ViewMessage* target = nullptr;
            if (auto idxIt = toolCallHistoryIndex_.find(toolCallId);
                idxIt != toolCallHistoryIndex_.end()
                && idxIt->second < session_->viewMessages.size()) {
                target = &session_->viewMessages[idxIt->second];
            }
            if (!target) {
                for (auto it = session_->viewMessages.rbegin(); it != session_->viewMessages.rend();
                     ++it) {
                    if (it->role == ViewMessage::Role::Tool && it->tool
                        && it->tool->toolCallId == toolCallId && !it->tool->toolFinished) {
                        target = &*it;
                        break;
                    }
                }
            }
            if (target) {
                if (!target->tool) {
                    target->tool = ViewMessage::ToolData{}; // 防御: 类型保证下不应为空
                }
                target->tool->toolResult   = content;
                target->tool->toolFinished = true;
                target->collapsed          = true;
                if (toolDurationMs > 0) {
                    target->durationMs = toolDurationMs;
                } else if (target->startTimeMs > 0) {
                    const int64_t nowMs = static_cast<int64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()
                        )
                            .count()
                    );
                    target->durationMs = std::max(int64_t{0}, nowMs - target->startTimeMs);
                    toolDurationMs     = target->durationMs;
                }
                if (toolStartTimeMs > 0) {
                    target->startTimeMs = toolStartTimeMs;
                }
                // 回填后同步持久化: 库内该行仍是未完成的 Tool 消息 (tool_finished
                // 缺失), 若不更新, 重启恢复/会话切换后再展示的 tool 结果会一直
                // 显示未完成状态。经 Session::updateViewMessage 触发 onUpdateViewMessage
                // 回调覆盖库内对应行 (按 msg.id 定位)。
                if (!target->id.empty()) {
                    session_->updateViewMessage(*target);
                }
                // (edit 工具参数 unified diff: 渲染端自行计算, 无消费者, 不再生成;
                //  ToolData::diff 字段保留供未来)
            }
            emitDelta(Delta{
                .type        = Delta::Type::ToolEnd,
                .toolName    = toolName,
                .toolCallId  = toolCallId,
                .result      = content,
                .hasError    = false,
                .startTimeMs = target ? target->startTimeMs : toolStartTimeMs,
                .durationMs  = target ? target->durationMs : toolDurationMs,
            });
        }
    }
    // ---- 结算 LLM 上下文消息 (增量持久化的核心挂点) ----
    // 节点对 messages channel 的写入即该批消息定稿: assistant 回复完成 /
    // tool 结果写回 (每节点一批, 非流式 token 粒度)。追加进会话 llm 上下文
    // 副本并触发节流落盘, 使进程在轮次中途被杀/崩溃时, 已结算的上下文最多
    // 丢失一个节流窗口 (Session::kPersistThrottleMs), 而非整轮。
    // - input 注入 / 节点内 overwrite (system 注入、压缩) / cancel 直写不产生
    //   本事件, 不会重复追加; 与引擎最终状态可能存在的短暂漂移由轮末
    //   BaseAgent 以 result.channel_raw("messages") 整体覆盖收敛 (权威同步)
    session_->appendSettledLlmMessages(value);

    // llm node 执行完成，推送上下文统计更新
    if (hasLLMOutput && io_ && session_->contextStats) {
        io_->sendToPeer(agentxx::agent::WireContextStats{
            session_->contextStats->contextTokens,
            session_->contextStats->maxContextTokens,
        });
    }
}

void EventBridge::handleTurnStart() {
    // 重置轮级 tps 统计 (上一轮残留: 流已结算, 计数归零)
    turnTpsTokenCount_  = 0.0;
    turnTpsDurationSec_ = 0.0;
    // 重置流级统计 (防御: 上轮异常结束可能未结算)
    tpsStartTime_     = {};
    tpsTokenCount_    = 0.0;
    tpsPendingChars_  = 0;
    tpsLastPushSec_   = 0.0;
    tpsLastPushToken_ = 0.0;
}

void EventBridge::settleCurrentStream() {
    // 结算 pending 字符
    if (tpsPendingChars_ > 0) {
        tpsTokenCount_   += static_cast<double>(tpsPendingChars_) / 4.0;
        tpsPendingChars_  = 0;
    }
    // 无进行中的流 (当前流无 token 输出) 时跳过
    if (tpsTokenCount_ <= 0.0) {
        return;
    }
    // 将当前流的累计估算 token 与流式耗时累加到轮级统计
    // (耗时仅计 LLM 流式期间, 从首个 token 到节点结束)
    const auto elapsedSec
        = std::chrono::duration<double>(std::chrono::steady_clock::now() - tpsStartTime_).count();
    turnTpsTokenCount_ += tpsTokenCount_;
    if (elapsedSec > 0.0) {
        turnTpsDurationSec_ += elapsedSec;
    }
    // 重置流级计数 (下一个流重新开始计时)
    tpsStartTime_     = {};
    tpsTokenCount_    = 0.0;
    tpsPendingChars_  = 0;
    tpsLastPushSec_   = 0.0;
    tpsLastPushToken_ = 0.0;
}

double EventBridge::takeTurnTps() {
    // 结算可能仍在进行中的流 (异常/取消路径可能未触发节点结束)
    settleCurrentStream();
    // 无 LLM 流式输出时返回 0 (如纯工具错误轮)
    if (turnTpsDurationSec_ <= 0.0) {
        turnTpsTokenCount_  = 0.0;
        turnTpsDurationSec_ = 0.0;
        return 0.0;
    }
    const double tps = turnTpsTokenCount_ / turnTpsDurationSec_;
    // 取走后重置, 下一轮重新统计
    turnTpsTokenCount_  = 0.0;
    turnTpsDurationSec_ = 0.0;
    return tps;
}

void EventBridge::handleNodeStart(const neograph::graph::GraphEvent& event) {
    // 防御: 上一节点遗留未结算的 THINKING 段 (正常应已在节点结束/出错时结算)
    finalizeThinkSegment();
    lastChatChunkType_ = neograph::ChatStreamChunk::TYPE_UNKNOWN;
    nodeStartTime_     = std::chrono::system_clock::now();
    nodeStartTimeMs_   = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(nodeStartTime_.time_since_epoch())
            .count()
    );
    emitDelta(agentxx::agent::Delta{
        .type        = agentxx::agent::Delta::Type::NodeStart,
        .nodeName    = event.node_name,
        .startTimeMs = nodeStartTimeMs_,
    });
}

void EventBridge::handleNodeEnd(const neograph::graph::GraphEvent& event) {
    // 结算当前 LLM 流: 将流耗时累加到轮级 tps 统计 (ModelCall 节点结束即流结束)
    settleCurrentStream();
    // 结算未闭合的 THINKING 段 (思考后无正文直接结束的流, 如纯思考/仅 tool_calls):
    // 结算包先于 NodeEnd Delta 发送, client 先回填 Think 时长再收节点结束事件
    finalizeThinkSegment();
    lastChatChunkType_ = neograph::ChatStreamChunk::TYPE_UNKNOWN;
    // 计算持续时间
    const int64_t duration_ms
        = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now() - nodeStartTime_
        )
                                   .count());
    emitDelta(agentxx::agent::Delta{
        .type        = agentxx::agent::Delta::Type::NodeEnd,
        .nodeName    = event.node_name,
        .startTimeMs = nodeStartTimeMs_,
        .durationMs  = duration_ms,
    });
}

void EventBridge::handleError(const neograph::graph::GraphEvent& event) {
    // 错误不产出会话增量 Delta (由 WireTurnResult 统一报告), 仅发布总线事件
    // 结算当前 LLM 流 (错误/取消可能跳过节点结束, 轮级统计需及时结算)
    settleCurrentStream();
    // 结算未闭合的 THINKING 段: 错误/取消中断思考流时同样回填已耗时长,
    // 使 client 已落盘的 Think 消息携带中断前的真实耗时
    finalizeThinkSegment();
    lastChatChunkType_ = neograph::ChatStreamChunk::TYPE_UNKNOWN;
    auto msg           = event.data.is_string() ? event.data.get<std::string>() : event.data.dump();
    publishError(std::move(msg), event.node_name);
}

double EventBridge::countTokens(std::string_view text) {
    // 优先使用 summarization 中间件的 token 计算 (与上下文压缩/上下文统计同口径)
    // TODO: 由 eventbus 解耦
    if (auto ctxPtr = ctx_.lock()) {
        if (ctxPtr->summarizationMiddleware) {
            return static_cast<double>(ctxPtr->summarizationMiddleware->countTokensForUtf8Str(text)
            );
        }
    }

    // 回退: 无 summarization (测试/裸 EventBridge) 时的内置估算
    // 口径与 SummarizationMiddlewareHandle::countTokensForUtf8Str 完全一致:
    // - 0xF8-0xFF (无效 UTF-8 前导, 5/6 字节编码已被 RFC 3629 废弃) 按 ascii
    //   单字节处理, 避免吞掉后续字节少计
    // - ascii ≈ 4 字符/token, 非 ascii ≈ 1.1 字符/token (分别折算后相加)
    size_t unicodeCount = 0, asciiCount = 0;
    for (size_t i = 0, step = 0; i < text.size(); i += step) {
        unsigned char byte = static_cast<unsigned char>(text[i]);
        if (byte >= 0xF8) {
            step = 1;
            ++asciiCount;
            continue;
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
    return static_cast<double>(unicodeCount) / 1.1 + static_cast<double>(asciiCount) / 4.0;
}

void EventBridge::pushTpsIfDue() {
    // 无对端 (headless) 或无会话统计时不推送
    if (!io_ || !session_->contextStats) {
        return;
    }
    const auto nowSec
        = std::chrono::duration<double>(std::chrono::steady_clock::now() - tpsStartTime_).count();
    // 每 [tpsPushIntervalSec_] 秒更新一次平均速度; 流式期间 token 持续到达, 越界即推送
    if (nowSec - tpsLastPushSec_ < tpsPushIntervalSec_) {
        return;
    }
    // 结算 pending 字符再计算窗口 tps
    if (tpsPendingChars_ > 0) {
        tpsTokenCount_   += static_cast<double>(tpsPendingChars_) / 4.0;
        tpsPendingChars_  = 0;
    }
    // 最近一个窗口 (推送周期) 内的平均生成速度:
    // 窗口内 token 增量 / 窗口实际时长, 而非自流开始以来的累计平均
    // (累计平均会被早期慢速段平滑, 无法反映当前实际速度)
    const double windowSec = nowSec - tpsLastPushSec_;
    const double tps       = (windowSec > 0.0 && tpsTokenCount_ > tpsLastPushToken_)
                                 ? (tpsTokenCount_ - tpsLastPushToken_) / windowSec
                                 : 0.0;
    tpsLastPushSec_        = nowSec;
    tpsLastPushToken_      = tpsTokenCount_;
    io_->sendToPeer(agentxx::agent::WireContextStats{
        session_->contextStats->contextTokens,
        session_->contextStats->maxContextTokens,
        tps,
    });
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
        == bus.hasListeners<agentxx::events::EventModelToken>(agentxx::events::Topic::ModelToken)) {
        return;
    }
    asio::co_spawn(
        bus.executor(),
        [busPtr,
         agentName = agentName_,
         sessionId = sessionId_,
         token, // 捕获副本, 协程生命周期独立于本对象
         kind = std::string{kind}]() -> asio::awaitable<void> {
            co_await busPtr->publish<agentxx::events::EventModelToken>(
                agentxx::events::Topic::ModelToken,
                agentxx::events::EventModelToken{
                    .agentName = agentName,
                    .sessionId = sessionId,
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
    if (false == bus.hasListeners<agentxx::events::EventError>(agentxx::events::Topic::Error)) {
        return;
    }
    asio::co_spawn(
        bus.executor(),
        [busPtr,
         agentName = agentName_,
         sessionId = sessionId_,
         message   = std::move(message),
         where     = std::move(where)]() -> asio::awaitable<void> {
            co_await busPtr->publish<agentxx::events::EventError>(
                agentxx::events::Topic::Error,
                agentxx::events::EventError{
                    .agentName = agentName,
                    .sessionId = sessionId,
                    .message   = message,
                    .where     = where,
                }
            );
        },
        asio::detached
    );
}

} // namespace event
} // namespace agentxx
