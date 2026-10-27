#include "ffi_client_io.h"

#include "agentxx/util/exception.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "utilxx_base/log.h"
#include <thread>
#include <utility>

namespace agentxx {
namespace ffi {

FfiClientAgentIO::FfiClientAgentIO(asio::any_io_executor ex, AgentxxFFICallbacks callbacks) :
    agent::EmbeddedClientAgentIO(std::move(ex)),
    callbacks_(callbacks) {}

FfiClientAgentIO::~FfiClientAgentIO() {
    failAllPendingInterrupts();
}

void FfiClientAgentIO::notifyServerReady() {
    postToClientThread([this]() {
        onServerReady();
    });
}

void FfiClientAgentIO::notifyError(int code, std::string message) {
    postToClientThread([this, code, message = std::move(message)]() {
        utilxx_base::Json j = utilxx_base::Json::object();
        j["code"]           = code;
        j["message"]        = message;
        emitEvent(AGENTXX_FFI_EVT_ERROR, dump(j));
    });
}

// ---------------------------------------------------------------------------
// FFI 应答通道
// ---------------------------------------------------------------------------

void FfiClientAgentIO::failAllPendingInterrupts() {
    // 先让基类收掉中断通道与合批缓冲, 再补 FFI 专有的"嵌入方程序工具调用已取消"通知
    agent::EmbeddedClientAgentIO::failAllPendingInterrupts();
    for (int64_t id : pendingHostToolCalls_) {
        utilxx_base::Json j = {
            {"callId", id         },
            {"reason", "cancelled"},
        };
        emitEvent(AGENTXX_FFI_EVT_HOST_TOOL_CANCELLED, dump(j));
    }
    pendingHostToolCalls_.clear();
}

bool FfiClientAgentIO::submitHostToolResponse(
    int64_t     callId,
    int32_t     isError,
    std::string resultJson
) {
    pendingHostToolCalls_.erase(callId);
    sendToPeer(agent::WireHostToolResult{
        .callId       = callId,
        .ok           = (isError == 0),
        .resultJson   = (isError == 0 ? std::move(resultJson) : ""),
        .errorMessage = (isError != 0 ? std::move(resultJson) : ""),
    });
    return true;
}

// ---------------------------------------------------------------------------
// 被动接收回调
// ---------------------------------------------------------------------------

void FfiClientAgentIO::emitDeltaText(std::string_view kind, std::string_view text) {
    utilxx_base::Json j = {
        {"kind", kind},
        {"text", text},
    };
    emitEvent(AGENTXX_FFI_EVT_DELTA_TEXT_BATCH, dump(j));
}

void FfiClientAgentIO::emitDeltaEvent(const agent::WireDelta& delta) {
    emitEvent(AGENTXX_FFI_EVT_DELTA, dump(agent::io::makeDeltaMsg(delta)));
}

void FfiClientAgentIO::onSync(const agent::WireSyncPayload& payload) {
    emitEvent(AGENTXX_FFI_EVT_SYNC, dump(agent::io::syncToJson(payload)));
}


void FfiClientAgentIO::onTurnResult(const agent::WireTurnResult& result) {
    flushDeltaBatch();
    emitEvent(
        AGENTXX_FFI_EVT_TURN_END,
        dump(agent::io::makeTurnResult(
            result.sessionId,
            result.hasError,
            result.errorMessage,
            result.interrupted,
            result.startTimeMs,
            result.durationMs
        ))
    );
}

void FfiClientAgentIO::onContextStats(const agent::WireContextStats& stats) {
    if (stats.contextTokens == 0 && stats.maxContextTokens == 0 && stats.tps <= 0.0) {
        return;
    }
    emitEvent(AGENTXX_FFI_EVT_CONTEXT_STATS, dump(agent::io::makeContextStats(stats)));
}

void FfiClientAgentIO::onServerReady() {
    utilxx_base::Json j = utilxx_base::Json::object();
    j["sessionId"]      = sessionId();
    emitEvent(AGENTXX_FFI_EVT_READY, dump(j));
}

void FfiClientAgentIO::onPeerMessage(agent::WireMessage msg) {
    std::visit(
        [this](auto&& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, agent::WireInterruptRequest>) {
                int64_t id = m.id;
                auto    ch = beginInterrupt(id);

                // 事件: 完整中断信息
                utilxx_base::Json j = utilxx_base::Json::object();
                j["interruptId"]    = id;
                j["sessionId"]      = m.sessionId;
                j["node"]           = m.node;
                j["value"]          = m.value;
                j["argJson"]        = m.argJson;
                emitEvent(AGENTXX_FFI_EVT_INTERRUPT_REQ, dump(j));

                // 挂起等待嵌入方程序 agentxx_ffi_interrupt_respond
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
                utilxx_base::Json j = utilxx_base::Json::object();
                j["interruptId"]    = m.id;
                emitEvent(AGENTXX_FFI_EVT_INTERRUPT_EXPIRED, dump(j));
            } else if constexpr (std::is_same_v<T, agent::WireModelInfo>) {
                // 能力清单 (各模型多模态输入支持) 一并下发: 嵌入方程序据此判断
                // 是否展示图片/音频/视频输入入口 (与 TUI 的附件按钮判定同源)
                auto j = agent::io::makeModelInfo(m.currentModel, m.models, m.capabilities);
                emitEvent(AGENTXX_FFI_EVT_MODEL_INFO, dump(j));
                if (onSyncReply) {
                    onSyncReply(SyncKind::ModelInfo, std::move(j));
                }
            } else if constexpr (std::is_same_v<T, agent::WireContextMessages>) {
                auto j = agent::io::makeContextMessages(m.messages);
                if (onSyncReply) {
                    onSyncReply(SyncKind::ContextMessages, std::move(j));
                }
            } else if constexpr (std::is_same_v<T, agent::WireSessionList>) {
                auto j = agent::io::makeSessionList(m.sessions, m.totalCount, m.hasMore);
                if (onSyncReply) {
                    onSyncReply(SyncKind::SessionList, std::move(j));
                }
            } else if constexpr (std::is_same_v<T, agent::WireAppendComponentInfo>) {
                emitEvent(
                    AGENTXX_FFI_EVT_COMPONENTS,
                    dump(agent::io::makeAppendComponentInfo(m.notifications))
                );
            } else if constexpr (std::is_same_v<T, agent::WirePluginData>) {
                emitEvent(AGENTXX_FFI_EVT_PLUGIN_DATA, dump(agent::io::makePluginData(m)));
            } else if constexpr (std::is_same_v<T, agent::WireError>) {
                utilxx_base::Json j = utilxx_base::Json::object();
                j["code"]           = m.code;
                j["message"]        = m.message;
                emitEvent(AGENTXX_FFI_EVT_ERROR, dump(j));
            } else if constexpr (std::is_same_v<T, agent::WireHostToolCall>) {
                pendingHostToolCalls_.insert(m.callId);
                utilxx_base::Json j = {
                    {"callId",     m.callId    },
                    {"name",       m.name      },
                    {"argsJson",   m.argsJson  },
                    {"sessionId",  m.sessionId },
                    {"timeoutSec", m.timeoutSec},
                };
                emitEvent(AGENTXX_FFI_EVT_HOST_TOOL_CALL, dump(j));
            } else if constexpr (std::is_same_v<T, agent::WireAddModelResult>) {
                // 模型增删回执: 走同步查询应答路由 (嵌入方程序 agentxx_ffi_add_model/
                // remove_model 在等待), 同时作为 EVT_WIRE 原样转给嵌入方程序
                auto j = agent::io::toJson(m);
                if (onSyncReply) {
                    onSyncReply(SyncKind::AddModelResult, j);
                }
                emitEvent(AGENTXX_FFI_EVT_WIRE, dump(j));
            } else if constexpr (std::is_same_v<T, agent::WireRemoveModelResult>) {
                auto j = agent::io::toJson(m);
                if (onSyncReply) {
                    onSyncReply(SyncKind::RemoveModelResult, j);
                }
                emitEvent(AGENTXX_FFI_EVT_WIRE, dump(j));
            } else if constexpr (std::is_same_v<T, agent::WireListDirResult>
                                 || std::is_same_v<T, agent::WireViewMessagesPage>
                                 || std::is_same_v<T, agent::WireMessageQueueUpdate>
                                 || std::is_same_v<T, agent::WirePermissionState>
                                 || std::is_same_v<T, agent::WireRenameSessionResult>
                                 || std::is_same_v<T, agent::WireInputAck>) {
                // 输入受理回执也走这条: 嵌入方程序据此区分"已开始 / 已排队 / 被拒绝"
                // (被拒绝时 reason 给出结构化原因, 如 attachment_too_large)
                emitEvent(AGENTXX_FFI_EVT_WIRE, dump(agent::io::toJson(m)));
            } else {
                agent::AgentIOBase::onPeerMessage(agent::WireMessage{std::move(m)});
            }
        },
        std::move(msg)
    );
}

// ---------------------------------------------------------------------------
// 内部
// ---------------------------------------------------------------------------

void FfiClientAgentIO::emitEvent(AgentxxFFIEventType type, std::string json) {
    if (callbacks_.on_event == nullptr) {
        return; // headless
    }
    AgentxxStringView sv{json.data(), static_cast<uint64_t>(json.size())};
    try {
        callbacks_.on_event(static_cast<int32_t>(type), &sv, callbacks_.user_data);
    } catch (const std::exception& e) {
        XX_LOGE("[ffi] on_event callback threw: {}", e.what());
    } catch (...) {
        XX_LOGE("[ffi] on_event callback threw unknown exception");
    }
}

std::string FfiClientAgentIO::dump(const utilxx_base::Json& j) {
    return j.dump();
}

} // namespace ffi
} // namespace agentxx
