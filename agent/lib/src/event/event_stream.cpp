#include "agentxx/event/event_stream.h"
#include "agentxx/util/invisible_text.h"
#include "agentxx/util/neograph_json_bridge.h"
#include "agentxx/util/tool_args_fix.h"

#include "agentxx/agent/io/agent_io_transport.h"
#include "agentxx/agent/io/context_stats_wire.h"
#include "agentxx/middlewares/summarization.h"
#include "agentxx/plugin/tool_registry.h"
#include "agentxx/protocol/provider_common.h"
#include "agentxx/util/task_scope.h"
#include "fmt/format.h"
#include "utilxx_base/container_util.h"
#include "utilxx_base/log.h"

namespace agentxx {
namespace events {

using ViewMessage = agentxx::agent::ViewMessage;
using WireDelta   = agentxx::agent::WireDelta;

namespace {

/// 当前系统时钟毫秒 (与 ViewMessage 的 startTimeMs/durationMs 同一时钟)
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()
    )
        .count();
}

/// 合并思考元数据 (加密标记取或, token 统计取大)
void mergeThinkMeta(ViewMessage::ThinkData& dst, const ViewMessage::ThinkData& src) {
    dst.isEncrypted     = dst.isEncrypted || src.isEncrypted;
    dst.reasoningTokens = std::max(dst.reasoningTokens, src.reasoningTokens);
}

/// 工具展示行的参数文本: 按工具参数 schema 把参数修正成声明的类型
/// - 与执行前同一份实现 (`agentxx::util::autoFixToolArgsTypes`): 特化渲染器与
///   展开体看到的参数形态和工具实际收到的完全一致, 渲染器不用再写类型兼容分支
/// - 无 agent 上下文 / 无该工具 schema / 参数不是 JSON 对象时原样返回
std::string toolRowArgsText(
    const std::shared_ptr<agentxx::plugin::ToolRegistry>& registry,
    std::string_view                                      toolName,
    std::string_view                                      argsJson
) {
    if (!registry || toolName.empty() || argsJson.empty()) {
        return std::string{argsJson};
    }
    utilxx_base::Json parameters;
    if (!registry->parametersOf(toolName, parameters)) {
        return std::string{argsJson};
    }
    return agentxx::util::autoFixedToolArgsJson(parameters, argsJson);
}

/// 该角色在展示历史上的默认折叠态 (折叠只是默认值, 运行时的展开/折叠在客户端)
bool defaultCollapsedOf(ViewMessage::Role role) noexcept {
    using R = ViewMessage::Role;
    switch (role) {
        case R::Think:
        case R::Tool:
        case R::Tip:
        case R::Event:
            return true;
        default:
            return false;
    }
}

} // namespace

EventStreamInterface::EventStreamInterface(
    std::string_view      in_name,
    const std::type_info& elementType
) :
    name(in_name),
    elementType_(elementType) {}

EventBus::EventBus(asio::any_io_executor executor) :
    executor_(executor) {}

bool EventBus::remove(std::string_view topic) {
    return utilxx_base::eraseHeterogeneous(streams_, topic);
}

// ---------------------------------------------------------------------------
// EventBridge: GraphEvent -> 会话增量 WireDelta + EventBus 发布
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
    // (会话增量 WireDelta/历史) 处理, 避免同一事件在多处维护
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
            // (tps 计时重置; 已打开的流式段不受影响, 由定稿路径收尾)
            lastChatChunkType_ = neograph::ChatStreamChunk::TYPE_UNKNOWN;
            break;
    }
}

void EventBridge::emitDelta(agentxx::agent::WireDelta delta) {
    // WireDelta 流序号: 会话级单调递增 (服务端增量重放缓冲依赖 seq 单调性)
    // 统一经 Session::nextDeltaSeq 分配 (与 SessionServerAgentIO 的新产出
    // WireDelta 共用同一入口, 保证所有新 WireDelta 都携带有效 seq 入重放缓冲)
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
        // 生成阶段标记 (见 protocol/provider_common.h): 不是模型输出文本,
        // 只更新 UI 占位提示, 不参与正文/思考流段与 tps 统计
        if (agentxx::protocol::chunk_type::isStatusChunk(chunk.type)) {
            handleGenStatusChunk(chunk);
            return;
        }
        token              = std::move(chunk.data);
        lastChatChunkType_ = chunk.type;
        if (chunk.type == neograph::ChatStreamChunk::TYPE_THINKING) {
            kind = "thinking";
        }
    } else {
        token = event.data.dump();
    }

    // 消息成型 (agent 是唯一写者):
    // - 类型切换 (思考 <-> 正文): 该角色在本步骤已出现时并回那条行, 否则新起一段;
    //   同一段里不把两个角色的文字混在一条行上 (一条行只有一个正文槽)
    // - 思考类型且文本为空 = 加密思考首包/元数据包: 只带元数据, 不追加文本
    const bool isThinking = (lastChatChunkType_ == neograph::ChatStreamChunk::TYPE_THINKING);
    if (isThinking && token.empty()) {
        routeStreamThink(ViewMessage::ThinkData{
            .reasoningTokens = 0,
            .isEncrypted     = true,
        });
    } else {
        routeStreamText(token, isThinking);
    }

    // 累计估算 token 数: 批量估算 (每16 token推送窗口再估, 减少 countTokens 遍历)
    // 此处累加文本, 推送时统一经 countTokens 折算 token, 与 summarization 保持一致
    tpsPendingText_ += token;
    // 每 16 token (约 64 字节) 或窗口到期再做一次 countTokens 批量折算
    constexpr size_t kBatchChars = 64;
    if (tpsPendingText_.size() >= kBatchChars) {
        tpsTokenCount_ += countTokens(tpsPendingText_);
        tpsPendingText_.clear();
    }
    // 定时推送一次平均速度 (token/s) - push 时会把 pending 一并结算
    pushTpsIfDue();

    // 总线发布
    publishModelToken(token, kind);
}

const EventBridge::StepRow* EventBridge::findStepRow(ViewMessage::Role role) const {
    // 只看本步骤的行: stepRows_ 在 NODE_START 清空, 上一模型调用的行不会命中
    for (auto it = stepRows_.rbegin(); it != stepRows_.rend(); ++it) {
        if (it->role == role) {
            return &*it;
        }
    }
    return nullptr;
}

void EventBridge::routeStreamText(std::string_view text, bool thinking) {
    if (text.empty()) {
        return;
    }
    const auto role = thinking ? ViewMessage::Role::Think : ViewMessage::Role::Assistant;
    const auto kind = thinking ? OpenStreamKind::Think : OpenStreamKind::Assistant;
    if (openKind_ == kind) {
        appendStreamText(text, thinking);
        return;
    }
    // 上游把思考/正文分多段交替送出 (思考 → 正文 → 思考 → 正文): 切回已出现过的
    // 角色时把文字并回那条已定稿的行, 而不是再切一条 (否则一次模型调用会被拆成
    // 多条思考行与正文行, 与重开会话后的历史、与 LLM 上下文都对应不上)
    if (const auto* row = findStepRow(role); row != nullptr) {
        appendToStepRow(*row, text, thinking);
        return;
    }
    // 该角色在本步骤还没出现: 先定稿当前段 (另一个角色), 再新开一段
    beginStream(role);
    appendStreamText(text, thinking);
}

void EventBridge::routeStreamThink(const ViewMessage::ThinkData& data) {
    if (openKind_ == OpenStreamKind::Think) {
        mergeStreamThink(data);
        return;
    }
    const auto* row = findStepRow(ViewMessage::Role::Think);
    if (row == nullptr) {
        beginStream(ViewMessage::Role::Think, data);
        return;
    }
    // 思考行已定稿 (元数据包比正文晚到): 合并后整条覆盖下发 (低频, 不是文本增量)
    const auto* current = session_ ? session_->viewMessageById(row->msgId) : nullptr;
    if (current == nullptr) {
        return;
    }
    ViewMessage merged = *current;
    auto        think  = merged.think.value_or(ViewMessage::ThinkData{});
    mergeThinkMeta(think, data);
    if (merged.think.has_value() && merged.think->isEncrypted == think.isEncrypted
        && merged.think->reasoningTokens == think.reasoningTokens) {
        return;
    }
    merged.think = think;
    emitDelta(WireDelta{
        .message = std::make_shared<ViewMessage>(merged),
        .type    = agentxx::agent::WireDelta::Type::UpdateMessage,
    });
    session_->updateViewMessage(std::move(merged));
}

void EventBridge::appendToStepRow(
    const StepRow&   row,
    std::string_view text,
    bool             thinking
) {
    const auto* current = session_ ? session_->viewMessageById(row.msgId) : nullptr;
    if (current == nullptr || text.empty()) {
        return;
    }
    ViewMessage updated = *current;
    updated.text.append(text);
    // 耗时按"该角色从第一段到本次追加"的跨度重算 (交错的各段合起来是一段)
    if (updated.startTimeMs > 0) {
        updated.durationMs = std::max<int64_t>(0, nowMs() - updated.startTimeMs);
    }
    session_->updateViewMessage(std::move(updated));
    emitDelta(WireDelta{
        .text  = std::string{text},
        .msgId = row.msgId,
        .type  = thinking ? agentxx::agent::WireDelta::Type::ThinkToken
                          : agentxx::agent::WireDelta::Type::TextToken,
    });
}

void EventBridge::beginStream(
    ViewMessage::Role                            role,
    const std::optional<ViewMessage::ThinkData>& meta
) {
    // 角色切换/上一段结束: 先把上一段定稿 (可见则落行并广播)
    flushOpenStream();
    openKind_    = OpenStreamKind::None;
    openId_.clear();
    // headless (无对端) 会话不建直播槽: 不逐 token 累积, 也不发 stream_begin/token
    // (定稿路径不变: channel write 照常写历史)
    if (!session_ || !session_->io) {
        return;
    }
    ViewMessage skeleton;
    skeleton.role        = role;
    skeleton.startTimeMs = nowMs();
    skeleton.think       = meta;
    skeleton.collapsed   = defaultCollapsedOf(role);
    openStartMs_         = skeleton.startTimeMs;
    openId_              = session_->beginStreamingView(std::move(skeleton));
    openKind_            = (role == ViewMessage::Role::Think) ? OpenStreamKind::Think
                                                             : OpenStreamKind::Assistant;
    emitDelta(agentxx::agent::WireDelta{
        .msgId       = openId_,
        .startTimeMs = openStartMs_,
        .streamRole  = role,
        .think       = meta,
        .type        = agentxx::agent::WireDelta::Type::StreamBegin,
    });
}

void EventBridge::appendStreamText(std::string_view text, bool thinking) {
    if (openKind_ == OpenStreamKind::None || !session_ || text.empty()) {
        return;
    }
    session_->appendStreamingText(text);
    emitDelta(agentxx::agent::WireDelta{
        .text  = std::string{text},
        .msgId = openId_,
        .type  = thinking ? agentxx::agent::WireDelta::Type::ThinkToken
                          : agentxx::agent::WireDelta::Type::TextToken,
    });
}

void EventBridge::mergeStreamThink(const ViewMessage::ThinkData& data) {
    if (openKind_ == OpenStreamKind::None || !session_) {
        return;
    }
    session_->mergeStreamingThink(data);
}

std::string EventBridge::commitStreamRow(ViewMessage row, bool usedStreamingSlot) {
    // 发出去的副本必须先带好 id (下面 move 进了会话)
    ViewMessage out = row;
    const auto  id  = usedStreamingSlot ? session_->finalizeStreamingView(std::move(row))
                                        : session_->appendViewMessage(std::move(row));
    out.id          = id;
    emitDelta(agentxx::agent::WireDelta{
        .message = std::make_shared<ViewMessage>(std::move(out)),
        .type    = agentxx::agent::WireDelta::Type::InsertMessage,
    });
    return id;
}

void EventBridge::flushOpenStream() {
    const auto kind = openKind_;
    openKind_       = OpenStreamKind::None;
    const auto id   = openId_;
    openId_.clear();
    if (kind == OpenStreamKind::None || !session_) {
        return;
    }
    const auto role = (kind == OpenStreamKind::Think) ? ViewMessage::Role::Think
                                                      : ViewMessage::Role::Assistant;
    ViewMessage row;
    row.role = role;
    if (session_->streamingView.has_value()) {
        auto& slot      = *session_->streamingView;
        row.text        = std::move(slot.text);
        row.think       = slot.think;
        row.startTimeMs = slot.startTimeMs > 0 ? slot.startTimeMs : openStartMs_;
    } else {
        // 直播槽不在 (headless/已被清掉): 仍按段起止算耗时
        row.startTimeMs = openStartMs_;
    }
    if (row.startTimeMs <= 0) {
        row.startTimeMs = openStartMs_;
    }
    row.durationMs = std::max<int64_t>(0, nowMs() - (row.startTimeMs > 0 ? row.startTimeMs : 0));
    row.collapsed  = defaultCollapsedOf(role);

    if (row.hasVisibleContent()) {
        // 定稿成正式行 (取消/出错的半截内容走同一路径: 用户看到过就进历史)
        const auto id = commitStreamRow(std::move(row), true);
        stepRows_.push_back(StepRow{role, id});
        return;
    }
    // 整段视觉为空 (模型只吐了零宽占位符之类): 不进历史, 让客户端丢掉流式项
    session_->clearStreamingView();
    emitDelta(agentxx::agent::WireDelta{
        .msgId = id,
        .type  = agentxx::agent::WireDelta::Type::StreamEnd,
    });
}

void EventBridge::finishTurn() {
    flushOpenStream();
    stepRows_.clear();
}

void EventBridge::handleGenStatusChunk(const neograph::ChatStreamChunk& chunk) {
    using Phase = agentxx::agent::WireDelta::GenPhase;
    if (chunk.type == agentxx::protocol::chunk_type::kGenerationStart) {
        // 模型节点开始执行 (请求已发出): 提示 UI "等待响应"
        emitGenStatus(Phase::Waiting);
        return;
    }
    // 工具调用开始 (参数未接收完, 内容无法展示): 提示 UI "正在调用工具"
    // - 思考/正文流此时已结束 (工具调用紧接其后), 先定稿当前段,
    //   保证 client 先收到行 (含耗时) 再展示占位提示
    flushOpenStream();
    emitGenStatus(Phase::ToolCall, chunk.data);
}

void EventBridge::emitGenStatus(
    agentxx::agent::WireDelta::GenPhase phase,
    std::string_view                    toolName
) {
    // 生成结束: 占位提示清除 (client 侧对重复的 Done 无副作用)
    genStatusActive_ = (phase != agentxx::agent::WireDelta::GenPhase::Done);
    emitDelta(agentxx::agent::WireDelta{
        .toolName = std::string{toolName},
        .type     = agentxx::agent::WireDelta::Type::GenStatus,
        .genPhase = phase,
    });
}

void EventBridge::handleChannelWrite(const neograph::graph::GraphEvent& event) {
    using agentxx::agent::ViewMessage;
    using agentxx::agent::WireDelta;

    // 任何通道写入前先收尾当前流式段: 保证"发提示/落行"之前没有未定稿的段
    // (重试提示来自 messages/message_tip 通道写入, 天然满足)
    flushOpenStream();

    auto chan = event.data.value("channel", std::string{});
    // event.data 为图边界类型 (neograph::json): value 整体转业务 Json
    auto value = event.data.contains("value") ? agentxx::util::fromNeographJson(event.data["value"])
                                              : utilxx_base::Json{};

    // 通用提示消息: 转发为 WireDelta::MessageUITip, 由 client 端插入提示消息
    if (chan == "message_tip" && value.is_object()) {
        auto       tipType = WireDelta::TipType::Info;
        const auto tip     = value.value("tipType", std::string{"info"});
        if (tip == "warning") {
            tipType = WireDelta::TipType::Warning;
        } else if (tip == "error") {
            tipType = WireDelta::TipType::Error;
        }
        emitDelta(WireDelta{
            .text    = value.value("text", std::string{}),
            .type    = WireDelta::Type::MessageUITip,
            .tipType = tipType,
        });
        return;
    }

    if (chan != "messages" || !value.is_array()) {
        return;
    }
    bool hasLLMOutput = false;
    // 本批是否含已结算的工具结果 (工具结算属"事实", 立即落盘)
    bool hasToolResult = false;
    for (const auto& jm : value) {
        auto role = jm.value("role", std::string{});
        if (role == "assistant") {
            hasLLMOutput = true;
            // 展开: reasoning_content 非空 / 加密思考 → Think (折叠); content 非空 →
            // Assistant 消息; 每条 tool_call → Tool 消息 (未完成, 历史默认折叠)。
            // 顺序与渲染端解析一致: Think 在前, Assistant 在中, Tool 在后。
            // 展开语义与渲染端 (TUI) 同步一致: 历史消息直接就是渲染消息,
            // client 端无需再按 json 解析
            auto reasoning       = jm.value("reasoning_content", std::string{});
            auto content         = jm.value("content", std::string{});
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
            // 定稿元数据 (0 = 该字段未提供): 提供时整条覆盖到已有行上, 未提供时保留
            // 流式段自己记的起止; 只有"本步骤没有流式段"(非流式/网关缓冲)才按节点
            // 计时回退, 避免定稿把流式行的起始时间改成节点开始时间 (多一条无意义增量)
            const int64_t jsonStartMs    = jm.value("startTimeMs", int64_t{0});
            const int64_t jsonDurationMs = jm.value("durationMs", int64_t{0});
            std::optional<ViewMessage::ThinkData> thinkMeta;
            if (isEncrypted || reasoningTokens > 0) {
                thinkMeta = ViewMessage::ThinkData{
                    .reasoningTokens = reasoningTokens,
                    .isEncrypted     = isEncrypted,
                };
            }
            // 单段时把定稿元数据并到该行上 (整条覆盖, 同 id)
            auto mergeMetaInto = [&](const StepRow& seg, const ViewMessage::ThinkData& meta) {
                const auto* existing
                    = session_ ? session_->viewMessageById(seg.msgId) : nullptr;
                if (existing == nullptr) {
                    return;
                }
                ViewMessage row = *existing;
                auto        merged = row.think.value_or(ViewMessage::ThinkData{});
                mergeThinkMeta(merged, meta);
                if (row.think.has_value() && row.think->isEncrypted == merged.isEncrypted
                    && row.think->reasoningTokens == merged.reasoningTokens) {
                    return; // 完全一致: 不发 op (省带宽且幂等)
                }
                row.think = merged;
                session_->updateViewMessage(row);
                emitDelta(WireDelta{
                    .message = std::make_shared<ViewMessage>(std::move(row)),
                    .type    = WireDelta::Type::UpdateMessage,
                });
            };
            // 定稿对账 (只用本步骤内已定稿的段):
            // - 无段   → 最终文本可见则新建一行 (非流式/网关缓冲)
            // - 单段   → 最终文本/元数据/耗时不一致才整条覆盖 (同 id)
            // - 多段   → 文本保持分段原样 (与实时所见一致), 只把元数据补到第一段
            auto reconcileSegment = [&](ViewMessage::Role                            segRole,
                                        const std::string&                        finalText,
                                        const std::optional<ViewMessage::ThinkData>&
                                            meta) {
                std::vector<const StepRow*> segs;
                for (const auto& row : stepRows_) {
                    if (row.role == segRole) {
                        segs.push_back(&row);
                    }
                }
                if (segs.empty()) {
                    // 无流式段: 按节点计时回退 (时间点/耗时都只能从节点推断)
                    const int64_t startMs = jsonStartMs > 0 ? jsonStartMs : nodeStartTimeMs_;
                    int64_t       durMs   = jsonDurationMs;
                    if (durMs == 0 && nodeStartTimeMs_ > 0) {
                        durMs = std::max<int64_t>(0, nowMs() - nodeStartTimeMs_);
                    }
                    ViewMessage row = ViewMessage::makeText(segRole, finalText, startMs, durMs);
                    // 折叠默认值由 agent 决定 (思考/工具/提示/事件默认折叠)
                    row.collapsed = defaultCollapsedOf(segRole);
                    if (meta) {
                        row.think = meta;
                    }
                    if (!row.hasVisibleContent()) {
                        return;
                    }
                    commitStreamRow(std::move(row), false);
                    return;
                }
                if (segs.size() > 1) {
                    XX_LOGD(
                        "[event_bridge] step has {} segments (role={}); keep segment texts, "
                        "merge metadata into the first one",
                        segs.size(),
                        static_cast<int>(segRole)
                    );
                    if (meta) {
                        mergeMetaInto(*segs.front(), *meta);
                    }
                    return;
                }
                const StepRow& seg = *segs.front();
                const auto*    existing
                    = session_ ? session_->viewMessageById(seg.msgId) : nullptr;
                if (existing == nullptr) {
                    return;
                }
                ViewMessage row     = *existing;
                bool        changed = false;
                if (!finalText.empty() && row.text != finalText) {
                    row.text = finalText;
                    changed  = true;
                }
                if (jsonStartMs > 0 && row.startTimeMs != jsonStartMs) {
                    row.startTimeMs = jsonStartMs;
                    changed         = true;
                }
                if (jsonDurationMs > 0 && row.durationMs != jsonDurationMs) {
                    row.durationMs = jsonDurationMs;
                    changed        = true;
                }
                if (meta) {
                    auto merged = row.think.value_or(ViewMessage::ThinkData{});
                    mergeThinkMeta(merged, *meta);
                    if (!row.think.has_value() || !(row.think->isEncrypted == merged.isEncrypted
                                                    && row.think->reasoningTokens
                                                           == merged.reasoningTokens)) {
                        row.think = merged;
                        changed   = true;
                    }
                }
                if (!changed || !row.hasVisibleContent()) {
                    return;
                }
                session_->updateViewMessage(row);
                emitDelta(WireDelta{
                    .message = std::make_shared<ViewMessage>(std::move(row)),
                    .type    = WireDelta::Type::UpdateMessage,
                });
            };

            reconcileSegment(ViewMessage::Role::Think, reasoning, thinkMeta);
            reconcileSegment(ViewMessage::Role::Assistant, content, std::nullopt);
            if (jm.contains("tool_calls") && jm["tool_calls"].is_array()) {
                // 工具行参数按工具 schema 自愈 (与执行前同一份实现): 渲染器历史/实时
                // 两条路径都拿到声明的类型
                auto agentCtx = ctx_.lock();
                auto registry = agentCtx ? agentCtx->toolRegistry : nullptr;
                for (const auto& tc : jm["tool_calls"]) {
                    const auto toolName   = tc.value("name", std::string{});
                    const auto toolCallId = tc.value("id", std::string{});
                    const auto arguments  = tc.value("arguments", std::string{});

                    const int64_t startMs = nowMs();
                    ViewMessage   m;
                    m.role      = ViewMessage::Role::Tool;
                    m.text      = toolRowArgsText(registry, toolName, arguments);
                    m.collapsed = true; // 历史重连默认折叠 (与 onDelta 实时展开不同)
                    m.tool      = ViewMessage::ToolData{};
                    m.tool->toolName   = toolName;
                    m.tool->toolCallId = toolCallId;
                    m.startTimeMs      = startMs;
                    const auto msgId   = session_->appendViewMessage(m);
                    m.id               = msgId;
                    // 登记 toolCallId → 工具行的 msgId, 供 tool 结果回填定位
                    // (按 id 而非下标: 展示历史窗口会淘汰, 下标会变)
                    if (!toolCallId.empty()) {
                        toolCallMsgIds_[toolCallId] = msgId;
                    }
                    // 工具行就是普通历史行: 整条 insert_message (未完成状态)
                    emitDelta(WireDelta{
                        .message = std::make_shared<ViewMessage>(std::move(m)),
                        .type    = WireDelta::Type::InsertMessage,
                    });
                }
            }
        } else if (role == "tool") {
            hasToolResult = true;
            auto content  = jm.value("content", std::string{});
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
            // 终态由工具执行层写入 (见 nodes/toolcall.cpp): 界面不看结果文本
            auto toolStatus = agentxx::util::ToolStatus::Ok;
            if (jm.contains("extra") && jm["extra"].is_object()) {
                toolStartTimeMs = jm["extra"].value("startTimeMs", int64_t{0});
                toolDurationMs  = jm["extra"].value("durationMs", int64_t{0});
                if (jm["extra"].contains("status") && jm["extra"]["status"].is_string()) {
                    toolStatus = agentxx::util::toolStatusFromName(
                        jm["extra"]["status"].get<std::string>()
                    );
                }
            }
            if (toolStartTimeMs == 0) {
                toolStartTimeMs = jm.value("startTimeMs", int64_t{0});
            }
            if (toolDurationMs == 0) {
                toolDurationMs = jm.value("durationMs", int64_t{0});
            }
            // 回填对应的 Tool 消息 (assistant tool_calls 展开时已登记 msgId):
            // - 优先按 msgId 定位 (本轮已登记)
            // - 未命中时在内存窗口里回扫补充 (如历史来自更早轮次/恢复中断场景)
            const ViewMessage* source = nullptr;
            if (auto idIt = toolCallMsgIds_.find(toolCallId); idIt != toolCallMsgIds_.end()) {
                source = session_->viewMessageById(idIt->second);
            }
            if (source == nullptr) {
                const auto& window = session_->viewWindow();
                for (auto it = window.rbegin(); it != window.rend(); ++it) {
                    if (it->role == ViewMessage::Role::Tool && it->tool
                        && it->tool->toolCallId == toolCallId && !it->tool->toolFinished) {
                        source = &*it;
                        break;
                    }
                }
            }
            if (source != nullptr) {
                ViewMessage row = *source;
                if (!row.tool) {
                    row.tool = ViewMessage::ToolData{}; // 防御: 类型保证下不应为空
                }
                row.tool->toolResult   = content;
                row.tool->toolFinished = true;
                row.tool->status       = toolStatus;
                row.collapsed          = true;
                if (toolDurationMs > 0) {
                    row.durationMs = toolDurationMs;
                } else if (row.startTimeMs > 0) {
                    row.durationMs = std::max<int64_t>(0, nowMs() - row.startTimeMs);
                    toolDurationMs = row.durationMs;
                }
                if (toolStartTimeMs > 0) {
                    row.startTimeMs = toolStartTimeMs;
                }
                // 回填后同步持久化: 库内该行仍是未完成的 Tool 消息 (tool_finished
                // 缺失), 若不更新, 重启恢复/会话切换后再展示的 tool 结果会一直
                // 显示未完成状态。经 Session::updateViewMessage 触发 onUpdateViewMessage
                // 回调覆盖库内对应行 (按 msg.id 定位)。
                if (!row.id.empty()) {
                    session_->updateViewMessage(row);
                }
                // 工具结果就是普通历史行的整条覆盖 (成败/耗时看行字段)
                emitDelta(WireDelta{
                    .message = std::make_shared<ViewMessage>(std::move(row)),
                    .type    = WireDelta::Type::UpdateMessage,
                });
                // (edit 工具参数 unified diff: 渲染端自行计算, 无处理者, 不再生成;
                //  ToolData::diff 字段保留供未来)
            } else {
                // 找不到目标行 (历史已被清掉/恢复场景): 没有"凭空造一行"的 op,
                // 只记日志 —— 旧实现会发 ToolEnd 让客户端自建一条工具消息
                XX_LOGW(
                    "[event_bridge] tool result for `{}` has no matching row (call_id={})",
                    toolName,
                    toolCallId
                );
            }
        }
    }
    // ---- 请求落盘已结算的上下文 (增量持久化的核心挂点) ----
    // 节点对该批消息 (assistant 回复 / tool 结果) 定稿时发出本事件, 此时消息
    // 已写入会话上下文 (会话为唯一权威)。这里只需按节流窗口请求一次落盘,
    // 使进程在轮次中途被杀/崩溃时已结算的上下文最多丢一个节流窗口
    // (Session::kPersistThrottleMs), 而非整轮。
    // - 上下文写入 (含系统提示词更新 / 压缩回写) 不产生本事件, 因此不会重复落盘
    // - 分级: 工具结算立即落盘 (结果体积大且是后续推理的输入),
    //   纯模型输出保持节流 (窗口内合并, 轮末权威保存作为保证)
    if (hasToolResult) {
        session_->persistNow("tool-settle");
    } else {
        session_->persistThrottled("llm-output");
    }

    // llm node 执行完成，推送上下文统计更新 (占用 + 会话累计用量 + 后台任务段)
    if (hasLLMOutput && io_ && session_->contextStats) {
        io_->sendToPeer(agentxx::agent::io::makeWireContextStats(
            ctx_.lock(),
            sessionId_,
            *session_->contextStats
        ));
    }
}

void EventBridge::handleTurnStart() {
    // 重置轮级 tps 统计 (上一轮残留: 流已结算, 计数归零)
    turnTpsTokenCount_  = 0.0;
    turnTpsDurationSec_ = 0.0;
    // 换轮即无未定稿的段 (正常已在轮末收尾)
    finishTurn();
    // 重置流级统计 (防御: 上轮异常结束可能未结算)
    tpsStartTime_  = {};
    tpsTokenCount_ = 0.0;
    tpsPendingText_.clear();
    tpsLastPushSec_   = 0.0;
    tpsLastPushToken_ = 0.0;
    // 新轮次开始: 上一轮的生成占位提示已随 client 侧 TurnEnd 清除, 此处只重置标记
    // (不补发 Done: 补发会在新轮次的用户消息之后插入一条无意义的清除指令)
    genStatusActive_ = false;
}

void EventBridge::settleCurrentStream() {
    // 结算 pending 文本
    if (!tpsPendingText_.empty()) {
        tpsTokenCount_ += countTokens(tpsPendingText_);
        tpsPendingText_.clear();
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
    tpsStartTime_  = {};
    tpsTokenCount_ = 0.0;
    tpsPendingText_.clear();
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
    // 新节点 = 新的一步: 防御性地收尾上一节点遗留的未定稿段, 清空本步骤行索引
    flushOpenStream();
    stepRows_.clear();
    lastChatChunkType_ = neograph::ChatStreamChunk::TYPE_UNKNOWN;
    nodeStartTime_     = std::chrono::system_clock::now();
    nodeStartTimeMs_   = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(nodeStartTime_.time_since_epoch())
            .count()
    );
    emitDelta(agentxx::agent::WireDelta{
        .nodeName    = event.node_name,
        .startTimeMs = nodeStartTimeMs_,
        .type        = agentxx::agent::WireDelta::Type::NodeStart,
    });
}

void EventBridge::handleNodeEnd(const neograph::graph::GraphEvent& event) {
    // 结算当前 LLM 流: 将流耗时累加到轮级 tps 统计 (ModelCall 节点结束即流结束)
    settleCurrentStream();
    // 定稿未收尾的流式段 (思考后无正文直接结束的流, 如纯思考/仅 tool_calls):
    // 定稿行先于 NodeEnd WireDelta 发送, client 先拿到行 (含耗时) 再收节点结束事件
    flushOpenStream();
    // 生成阶段提示收尾: 节点结束即生成结束, 清除 client 侧占位提示
    // (正常路径下正文/工具消息已先到达并让 client 清除; 此处补充处理无输出即结束的场景)
    if (genStatusActive_) {
        emitGenStatus(agentxx::agent::WireDelta::GenPhase::Done);
    }
    lastChatChunkType_ = neograph::ChatStreamChunk::TYPE_UNKNOWN;
    // 计算持续时间
    const int64_t duration_ms
        = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now() - nodeStartTime_
        )
                                   .count());
    emitDelta(agentxx::agent::WireDelta{
        .nodeName    = event.node_name,
        .startTimeMs = nodeStartTimeMs_,
        .durationMs  = duration_ms,
        .type        = agentxx::agent::WireDelta::Type::NodeEnd,
    });
}

void EventBridge::handleError(const neograph::graph::GraphEvent& event) {
    // 错误不产出会话增量 WireDelta (由 WireTurnResult 统一报告), 仅发布总线事件
    // 结算当前 LLM 流 (错误/取消可能跳过节点结束, 轮级统计需及时结算)
    settleCurrentStream();
    // 定稿未收尾的流式段: 取消/出错时已产出的半截内容照常成行并落盘
    // (用户看到过就进历史; 与 LLM 上下文相比这里会多一条未说完的 assistant 消息)
    flushOpenStream();
    // 生成阶段提示收尾: 出错/取消时清除 client 侧占位提示 (不再有后续输出)
    if (genStatusActive_) {
        emitGenStatus(agentxx::agent::WireDelta::GenPhase::Done);
    }
    lastChatChunkType_ = neograph::ChatStreamChunk::TYPE_UNKNOWN;
    auto msg           = event.data.is_string() ? event.data.get<std::string>() : event.data.dump();
    publishError(std::move(msg), event.node_name);
}

double EventBridge::countTokens(std::string_view text) {
    // 优先使用 EventBus 上注册的 token 计算服务 (与上下文压缩/上下文统计一致)
    if (auto ctxPtr = ctx_.lock(); ctxPtr && ctxPtr->bus) {
        auto count
            = ctxPtr->bus->callService<size_t(std::string_view)>(events::Topic::TokenCount, text);
        if (count.has_value()) {
            return static_cast<double>(*count);
        }
    }
    if (session_ && session_->bus) {
        auto count
            = session_->bus->callService<size_t(std::string_view)>(events::Topic::TokenCount, text);
        if (count.has_value()) {
            return static_cast<double>(*count);
        }
    }

    // 回退: 无 summarization (测试/裸 EventBridge) 时的内置估算
    // 规则与 SummarizationMiddlewareHandle::countTokensForUtf8Str / utilxx_base::estimateTokenCount
    // 完全一致:
    // - 0xF8-0xFF (无效 UTF-8 前导, 5/6 字节编码已被 RFC 3629 废弃) 按 ascii
    //   单字节处理, 避免吞掉后续字节少计
    // - ascii ≈ 4 字符/token, 非 ascii ≈ 1.1 字符/token (分别折算后相加)
    return utilxx_base::estimateTokenCount(text, 1.1, 4.0);
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
    // 结算 pending 文本再计算窗口 tps
    if (!tpsPendingText_.empty()) {
        tpsTokenCount_ += countTokens(tpsPendingText_);
        tpsPendingText_.clear();
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
    // 统一下发: 除占用/用量/速度外还带上后台任务段 (界面按整份覆盖, 缺了就显示成空)
    io_->sendToPeer(agentxx::agent::io::makeWireContextStats(
        ctx_.lock(),
        sessionId_,
        *session_->contextStats,
        tps
    ));
}

void EventBridge::publishModelToken(const std::string& token, std::string_view kind) {
    auto ctxPtr = ctx_.lock();
    if (!ctxPtr || !ctxPtr->bus) {
        return; // 无 bus, 跳过
    }
    auto  busPtr = ctxPtr->bus;
    auto& bus    = *busPtr;
    // 无订阅者时跳过: 避免每个 token 创建一次无处理者的协程
    // (ModelToken topic 当前无生产订阅者, 该检查使热路径零开销)
    if (false
        == bus.hasListeners<agentxx::events::EventModelToken>(agentxx::events::Topic::ModelToken)) {
        return;
    }
    auto task = [busPtr,
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
    };
    // 后台任务登记: 有登记表时由它统一在关闭阶段取消并等待结束
    if (ctxPtr->taskScope) {
        ctxPtr->taskScope->spawn("event-publish", std::move(task));
        return;
    }
    asio::co_spawn(bus.executor(), std::move(task), asio::detached);
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
    auto task = [busPtr,
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
    };
    if (ctxPtr->taskScope) {
        ctxPtr->taskScope->spawn("event-publish", std::move(task));
        return;
    }
    asio::co_spawn(bus.executor(), std::move(task), asio::detached);
}

} // namespace events
} // namespace agentxx
