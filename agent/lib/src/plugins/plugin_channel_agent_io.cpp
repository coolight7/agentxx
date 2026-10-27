#include "plugin_channel_agent_io.h"

#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/util/exception.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "fmt/format.h"
#include "utilxx_base/log.h"
#include <utility>

namespace agentxx {
namespace plugin {

PluginChannelAgentIO::PluginChannelAgentIO(asio::any_io_executor ex, int64_t channelId) :
    agent::EmbeddedClientAgentIO(std::move(ex)),
    channelId_(channelId) {}

// ---------------------------------------------------------------------------
// 管理器发起的事件
// ---------------------------------------------------------------------------

void PluginChannelAgentIO::notifyReady(std::string_view rootId, std::string_view model) {
    // 投递期间持有自身: 这类通知由管理器发起, 投递出去之后通道可能已被关闭并摘掉
    // 记录 —— 只捕 `this` 时闭包会在对象销毁后执行
    auto self = shared_from_this();
    postToClientThread([self, rootId = std::string{rootId}, model = std::string{model}]() {
        utilxx_base::Json payload = utilxx_base::Json::object();
        payload["rootId"]         = rootId;
        payload["sessionId"]      = self->sessionId();
        payload["model"]          = model;
        self->emit(AGENTXX_PLUGIN_CHANNEL_EVT_READY, 0, {}, &payload);
    });
}

void PluginChannelAgentIO::notifyError(std::string code, std::string message) {
    auto self = shared_from_this();
    postToClientThread([self, code = std::move(code), message = std::move(message)]() {
        utilxx_base::Json payload = utilxx_base::Json::object();
        payload["code"]           = code;
        payload["message"]        = message;
        self->emit(AGENTXX_PLUGIN_CHANNEL_EVT_ERROR, 0, message, &payload);
    });
}

void PluginChannelAgentIO::notifyClosed(std::string_view reason) {
    auto self = shared_from_this();
    postToClientThread([self, reason = std::string{reason}]() {
        utilxx_base::Json payload = utilxx_base::Json::object();
        payload["reason"]         = reason;
        self->emit(AGENTXX_PLUGIN_CHANNEL_EVT_CLOSED, 0, {}, &payload);
    });
}

// ---------------------------------------------------------------------------
// 中断应答
// ---------------------------------------------------------------------------

int64_t PluginChannelAgentIO::pendingInterruptId() const {
    return pendingInterruptId_.load(std::memory_order_acquire);
}

bool PluginChannelAgentIO::submitInterrupt(int64_t interruptId, utilxx_base::Json values) {
    const bool ok = submitInterruptResponse(interruptId, std::move(values));
    if (ok) {
        int64_t expected = interruptId;
        pendingInterruptId_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// 被动回调
// ---------------------------------------------------------------------------

void PluginChannelAgentIO::emit(
    int32_t                  kind,
    int64_t                  interruptId,
    std::string_view         text,
    const utilxx_base::Json* payload
) {
    if (!eventFn_) {
        return;
    }
    try {
        eventFn_(kind, interruptId, text, payload);
    } catch (const std::exception& e) {
        XX_LOGE("[runtime] channel #{} event callback threw: {}", channelId_, e.what());
    } catch (...) {
        XX_LOGE("[runtime] channel #{} event callback threw unknown exception", channelId_);
    }
}

void PluginChannelAgentIO::emitDeltaText(std::string_view kind, std::string_view text) {
    const bool isText = (kind != "thinking");
    if (isText) {
        turnText_ += text;
        segmentText_ += text;
    }
    emit(
        isText ? AGENTXX_PLUGIN_CHANNEL_EVT_DELTA_TEXT : AGENTXX_PLUGIN_CHANNEL_EVT_DELTA_THINKING,
        0,
        text,
        nullptr
    );
}

void PluginChannelAgentIO::emitDeltaEvent(const agent::WireDelta& delta) {
    using T = agent::WireDelta::Type;
    switch (delta.type) {
        case T::TurnStart: {
            turnText_.clear();
            segmentText_.clear();
            lastSegment_.clear();
            utilxx_base::Json payload = utilxx_base::Json::object();
            payload["msgId"]          = delta.msgId;
            payload["startTimeMs"]    = delta.startTimeMs;
            if (!delta.attachments.empty()) {
                utilxx_base::Json atts = utilxx_base::Json::array();
                for (const auto& att : delta.attachments) {
                    utilxx_base::Json one = att.toJson();
                    // dataUrl 体积大: 通道事件只给元数据 (需要内容时走附件路径)
                    one.erase("data_url");
                    atts.push_back(std::move(one));
                }
                payload["attachments"] = std::move(atts);
            }
            emit(AGENTXX_PLUGIN_CHANNEL_EVT_TURN_START, 0, delta.text, &payload);
            break;
        }
        case T::TextToken:
        case T::ThinkToken: {
            // 合批关闭时逐 token 走到这里 (合批开启时这些增量在 [onDelta] 里就
            // 被攒起来, 由 [emitDeltaText] 统一交出)
            const bool isText = (delta.type == T::TextToken);
            if (isText) {
                turnText_ += delta.text;
                segmentText_ += delta.text;
            }
            emit(
                isText ? AGENTXX_PLUGIN_CHANNEL_EVT_DELTA_TEXT
                       : AGENTXX_PLUGIN_CHANNEL_EVT_DELTA_THINKING,
                0,
                delta.text,
                nullptr
            );
            break;
        }
        case T::ToolStart: {
            // 工具调用打断正文段: 记住已产出的那一段 (作为 TURN_END.text 候选)
            if (!segmentText_.empty()) {
                lastSegment_ = segmentText_;
                segmentText_.clear();
            }
            utilxx_base::Json payload = utilxx_base::Json::object();
            payload["toolName"]       = delta.toolName;
            payload["toolCallId"]     = delta.toolCallId;
            payload["arguments"]      = delta.arguments;
            emit(AGENTXX_PLUGIN_CHANNEL_EVT_TOOL_START, 0, {}, &payload);
            break;
        }
        case T::ToolEnd: {
            utilxx_base::Json payload = utilxx_base::Json::object();
            payload["toolName"]       = delta.toolName;
            payload["toolCallId"]     = delta.toolCallId;
            payload["result"]         = delta.result;
            payload["hasError"]       = delta.hasError;
            emit(AGENTXX_PLUGIN_CHANNEL_EVT_TOOL_END, 0, {}, &payload);
            break;
        }
        case T::GenStatus: {
            utilxx_base::Json payload = utilxx_base::Json::object();
            payload["phase"]          = std::string{agent::io::genPhaseToString(delta.genPhase)};
            payload["toolName"]       = delta.toolName;
            emit(AGENTXX_PLUGIN_CHANNEL_EVT_GEN_STATUS, 0, {}, &payload);
            break;
        }
        case T::MessageUITip: {
            utilxx_base::Json payload = utilxx_base::Json::object();
            switch (delta.tipType) {
                case agent::WireDelta::TipType::Warning:
                    payload["level"] = "warning";
                    break;
                case agent::WireDelta::TipType::Error:
                    payload["level"] = "error";
                    break;
                default:
                    payload["level"] = "info";
                    break;
            }
            emit(AGENTXX_PLUGIN_CHANNEL_EVT_TIP, 0, delta.text, &payload);
            break;
        }
        default:
            // NodeStart / NodeEnd / InsertMessage / UpdateMessage 属界面内部事件,
            // 通道不转 (插件关心的是对话内容与工具, 不是渲染细节)
            break;
    }
}

void PluginChannelAgentIO::onTurnResult(const agent::WireTurnResult& result) {
    // 先把积压的正文交出去, 再交本轮结束 (顺序对插件有意义)
    flushDeltaBatch();
    if (!segmentText_.empty()) {
        lastSegment_ = segmentText_;
        segmentText_.clear();
    }

    // text = 本轮最后一个非空正文段 (工具调用之后那段), payload.fullText 是整轮拼接
    utilxx_base::Json payload = utilxx_base::Json::object();
    payload["hasError"]       = result.hasError;
    payload["errorMessage"]   = result.errorMessage;
    payload["interrupted"]    = result.interrupted;
    payload["startTimeMs"]    = result.startTimeMs;
    payload["durationMs"]     = result.durationMs;
    payload["tps"]            = lastTps_;
    payload["fullText"]       = turnText_;
    emit(AGENTXX_PLUGIN_CHANNEL_EVT_TURN_END, 0, lastSegment_, &payload);

    turnText_.clear();
    segmentText_.clear();
    lastSegment_.clear();
}

void PluginChannelAgentIO::onContextStats(const agent::WireContextStats& stats) {
    utilxx_base::Json payload = utilxx_base::Json::object();
    payload["contextTokens"]    = stats.contextTokens;
    payload["maxContextTokens"] = stats.maxContextTokens;
    payload["tps"]              = stats.tps;
    payload["usage"]            = utilxx_base::Json{
        {"promptTokens",       stats.promptTokens      },
        {"completionTokens",   stats.completionTokens  },
        {"totalTokens",        stats.totalTokens       },
        {"cachedPromptTokens", stats.cachedPromptTokens},
    };
    lastTps_ = stats.tps;
    emit(AGENTXX_PLUGIN_CHANNEL_EVT_CONTEXT_STATS, 0, {}, &payload);
}

void PluginChannelAgentIO::onServerReady() {
    // 就绪事件由管理器发 (它知道根 id 与模型名): 这里不重复发
}

void PluginChannelAgentIO::onPeerMessage(agent::WireMessage msg) {
    std::visit(
        [this](auto&& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, agent::WireInterruptRequest>) {
                const int64_t id = m.id;
                auto          ch = beginInterrupt(id);
                pendingInterruptId_.store(id, std::memory_order_release);

                utilxx_base::Json payload = utilxx_base::Json::object();
                payload["id"]             = id;
                payload["node"]           = m.node;
                payload["value"]          = m.value;
                payload["argJson"]        = m.argJson;
                emit(AGENTXX_PLUGIN_CHANNEL_EVT_INTERRUPT, id, {}, &payload);

                auto self = shared_from_this();
                asio::co_spawn(
                    ex_,
                    [self, id, ch]() -> asio::awaitable<void> {
                        co_await self->awaitInterruptResponse(id, ch);
                    },
                    asio::detached
                );
            } else if constexpr (std::is_same_v<T, agent::WireInterruptExpired>) {
                expireInterrupt(m.id);
                int64_t expected = m.id;
                pendingInterruptId_.compare_exchange_strong(
                    expected,
                    0,
                    std::memory_order_acq_rel
                );
                utilxx_base::Json payload = utilxx_base::Json::object();
                payload["id"]             = m.id;
                emit(AGENTXX_PLUGIN_CHANNEL_EVT_INTERRUPT_EXPIRED, m.id, {}, &payload);
            } else if constexpr (std::is_same_v<T, agent::WireInputAck>) {
                utilxx_base::Json payload = agent::io::makeInputAck(m);
                emit(AGENTXX_PLUGIN_CHANNEL_EVT_INPUT_ACK, 0, {}, &payload);
            } else if constexpr (std::is_same_v<T, agent::WireMessageQueueUpdate>) {
                utilxx_base::Json payload = utilxx_base::Json::object();
                payload["state"]          = m.state;
                utilxx_base::Json items   = utilxx_base::Json::array();
                for (const auto& item : m.items) {
                    items.push_back(utilxx_base::Json{
                        {"id",          item.id         },
                        {"text",        item.text       },
                        {"createdAtMs", item.createdAtMs},
                    });
                }
                payload["items"] = std::move(items);
                emit(AGENTXX_PLUGIN_CHANNEL_EVT_QUEUE_STATE, 0, {}, &payload);
            } else {
                agent::AgentIOBase::onPeerMessage(agent::WireMessage{std::move(m)});
            }
        },
        std::move(msg)
    );
}

} // namespace plugin
} // namespace agentxx
