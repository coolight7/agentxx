#include "ffi_client_io.h"

#include "agentxx/util/exception.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "utilxx_base/log.h"
#include <cstring>
#include <thread>
#include <utility>

namespace agentxx {
namespace ffi {

namespace {

/// 去掉应答里的信封字段 (`type`) 与相关性字段 (`reqId`)
///
/// 回执里的 `result` 是"应答本体": 调用方已经在 `EVT_RESULT` 的信封上看到了
/// op 与 reqId, 结果里再带一份没有意义。
utilxx_base::Json stripEnvelope(const utilxx_base::Json& j) {
    utilxx_base::Json out = utilxx_base::Json::object();
    if (!j.is_object()) {
        return j;
    }
    for (const auto& [key, value] : j.items()) {
        const std::string_view k{key};
        if (k == "type" || k == "reqId") {
            continue;
        }
        out[key] = value;
    }
    return out;
}

} // namespace

void emitOwnedEvent(const AgentxxFFICallbacks& cb, int32_t type, std::string json) {
    if (cb.on_event == nullptr) {
        return; // headless: 不序列化, 不分配
    }
    // 结构体与正文放在同一块内存里: 回调可能延后执行 (Dart 的 NativeCallable.listener
    // 把调用投进 isolate 消息队列), 结构体本身也必须活到回调真正跑完, 不能放栈上
    const size_t total = sizeof(AgentxxString) + json.size() + 1;
    auto* holder = static_cast<AgentxxString*>(agentxx_ffi_malloc(total));
    if (holder == nullptr) {
        XX_LOGW("[ffi] event payload allocation failed ({} bytes)", json.size());
        return;
    }
    auto* buf = reinterpret_cast<char*>(holder + 1);
    if (!json.empty()) {
        std::memcpy(buf, json.data(), json.size());
    }
    buf[json.size()] = '\0';
    holder->data     = buf;
    holder->size     = static_cast<uint64_t>(json.size());
    // 所有权交给回调: 这里不回收 (回调取完正文后 agentxx_ffi_free(payload) 释放整块)
    // 回调抛异常时不补释放: 它可能已经释放过, 再释放就是双重释放 (宁可漏一块内存)
    try {
        cb.on_event(type, holder, cb.user_data);
    } catch (const std::exception& e) {
        XX_LOGE("[ffi] on_event callback threw: {}", e.what());
    } catch (...) {
        XX_LOGE("[ffi] on_event callback threw unknown exception");
    }
}

FfiClientAgentIO::FfiClientAgentIO(asio::any_io_executor ex, AgentxxFFICallbacks callbacks) :
    agent::EmbeddedClientAgentIO(std::move(ex)),
    callbacks_(callbacks) {}

FfiClientAgentIO::~FfiClientAgentIO() {
    failAllPendingInterrupts();
}

void FfiClientAgentIO::notifyError(int code, std::string name, std::string message) {
    postToClientThread([this, code, name = std::move(name), message = std::move(message)]() {
        if (!wantsEvents()) {
            return;
        }
        utilxx_base::Json j = utilxx_base::Json::object();
        j["code"]           = code;
        j["name"]           = name;
        j["message"]        = message;
        emitEvent(AGENTXX_FFI_EVT_ERROR, dump(j));
    });
}

// ---------------------------------------------------------------------------
// 事件发出
// ---------------------------------------------------------------------------

void FfiClientAgentIO::emitEvent(int32_t type, std::string json) {
    emitOwnedEvent(callbacks_, type, std::move(json));
}

void FfiClientAgentIO::emitCallResult(
    uint64_t                 reqId,
    std::string_view         op,
    bool                     ok,
    int32_t                  code,
    std::string_view         errorName,
    std::string_view         message,
    const utilxx_base::Json& result
) {
    if (!wantsEvents()) {
        return;
    }
    utilxx_base::Json j = utilxx_base::Json::object();
    j["reqId"]           = reqId;
    j["op"]              = op;
    j["ok"]              = ok;
    if (ok) {
        j["result"] = result.is_null() ? utilxx_base::Json::object() : result;
    } else {
        utilxx_base::Json err = utilxx_base::Json::object();
        err["code"]           = code;
        err["name"]           = errorName.empty() ? std::string{"rejected"} : std::string{errorName};
        if (!message.empty()) {
            err["message"] = message;
        }
        j["error"] = std::move(err);
    }
    emitEvent(AGENTXX_FFI_EVT_RESULT, dump(j));
}

// ---------------------------------------------------------------------------
// FFI 应答通道
// ---------------------------------------------------------------------------

void FfiClientAgentIO::failAllPendingInterrupts() {
    // 先让基类收掉中断通道与合批缓冲, 再补 FFI 专有的"嵌入方程序工具调用已取消"通知
    agent::EmbeddedClientAgentIO::failAllPendingInterrupts();
    for (int64_t id : pendingHostToolCalls_) {
        if (!wantsEvents()) {
            break;
        }
        utilxx_base::Json j = {
            {"callId", id         },
            {"reason", "Canceled"},
        };
        emitEvent(AGENTXX_FFI_EVT_HOST_TOOL_CANCELED, dump(j));
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

void FfiClientAgentIO::emitDeltaText(
    std::string_view msgId,
    std::string_view kind,
    std::string_view text
) {
    if (!wantsEvents()) {
        return;
    }
    utilxx_base::Json j = {
        {"msgId", msgId},
        {"kind",  kind },
        {"text",  text },
    };
    emitEvent(AGENTXX_FFI_EVT_DELTA_TEXT_BATCH, dump(j));
}

void FfiClientAgentIO::emitDeltaEvent(const agent::WireDelta& delta) {
    if (!wantsEvents()) {
        return;
    }
    emitEvent(AGENTXX_FFI_EVT_DELTA, dump(agent::io::makeDeltaMsg(delta)));
}

void FfiClientAgentIO::onSync(const agent::WireSyncPayload& payload) {
    if (!wantsEvents()) {
        return;
    }
    emitEvent(AGENTXX_FFI_EVT_SYNC, dump(agent::io::syncToJson(payload)));
}

void FfiClientAgentIO::onTurnResult(const agent::WireTurnResult& result) {
    flushDeltaBatch();
    if (!wantsEvents()) {
        return;
    }
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
    if (!wantsEvents()) {
        return;
    }
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

void FfiClientAgentIO::onRuntimeStopping() {
    // 基类: 失败挂起的中断 + 冲刷合批缓冲
    agent::EmbeddedClientAgentIO::onRuntimeStopping();
    // 运行期: 把仍在等待的调用按取消收尾 (这些回执必须排在 EVT_STOPPED 之前)
    if (onStopping) {
        onStopping();
    }
}

void FfiClientAgentIO::dispatchReply(
    uint64_t          reqId,
    utilxx_base::Json json,
    bool              ok,
    std::string       reason,
    std::string       message
) {
    if (reqId == 0) {
        // reqId == 0: 不是回执 (别的客户端的请求结果 / 服务端主动广播)
        XX_LOGD("[ffi] reply without reqId ignored (type=`{}`)", json.value("type", std::string{}));
        return;
    }
    if (onCallReply) {
        onCallReply(reqId, std::move(json), ok, std::move(reason), std::move(message));
    }
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

                // 挂起等待嵌入方程序 `interrupt.respond`
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
            } else if constexpr (std::is_same_v<T, agent::WireAck>) {
                // 通用回执: result 取回执里的 data (与 op 表的"成功结果 = data"一致)
                utilxx_base::Json data = utilxx_base::Json::object();
                if (!m.data.is_null()) {
                    data = m.data;
                }
                dispatchReply(m.reqId, std::move(data), m.ok, m.reason, m.error);
            } else if constexpr (std::is_same_v<T, agent::WireModelInfo>) {
                auto j = agent::io::toJson(m);
                if (m.reqId != 0) {
                    dispatchReply(m.reqId, stripEnvelope(j), true, {}, {});
                } else {
                    // 模型状态广播 (切换会话/增删模型后): 嵌入方据此刷新选择框
                    emitEvent(AGENTXX_FFI_EVT_MODEL_INFO, dump(j));
                }
            } else if constexpr (std::is_same_v<T, agent::WireContextMessages>) {
                auto j = agent::io::toJson(m);
                dispatchReply(m.reqId, stripEnvelope(j), true, {}, {});
            } else if constexpr (std::is_same_v<T, agent::WireSessionList>) {
                auto j = agent::io::toJson(m);
                dispatchReply(m.reqId, stripEnvelope(j), true, {}, {});
            } else if constexpr (std::is_same_v<T, agent::WireViewMessagesPage>) {
                auto j = agent::io::toJson(m);
                dispatchReply(m.reqId, stripEnvelope(j), true, {}, {});
            } else if constexpr (std::is_same_v<T, agent::WireAppendComponentInfo>) {
                auto j = agent::io::toJson(m);
                if (m.reqId != 0) {
                    dispatchReply(m.reqId, stripEnvelope(j), true, {}, {});
                } else {
                    emitEvent(AGENTXX_FFI_EVT_COMPONENTS, dump(j));
                }
            } else if constexpr (std::is_same_v<T, agent::WirePermissionState>) {
                auto j = agent::io::toJson(m);
                if (m.reqId != 0) {
                    dispatchReply(m.reqId, stripEnvelope(j), true, {}, {});
                } else {
                    utilxx_base::Json ev = utilxx_base::Json::object();
                    ev["fullAuth"]       = m.fullAuth;
                    emitEvent(AGENTXX_FFI_EVT_PERMISSION_STATE, dump(ev));
                }
            } else if constexpr (std::is_same_v<T, agent::WireAddModelResult>) {
                auto j = agent::io::toJson(m);
                if (m.reqId != 0) {
                    if (m.ok) {
                        dispatchReply(m.reqId, stripEnvelope(j), true, {}, {});
                    } else {
                        dispatchReply(m.reqId, {}, false, "bad_config", m.error);
                    }
                } else {
                    XX_LOGD("[ffi] add_model_result without reqId ignored");
                }
            } else if constexpr (std::is_same_v<T, agent::WireRemoveModelResult>) {
                auto j = agent::io::toJson(m);
                if (m.reqId != 0) {
                    if (m.ok) {
                        dispatchReply(m.reqId, stripEnvelope(j), true, {}, {});
                    } else {
                        dispatchReply(m.reqId, {}, false, "bad_config", m.error);
                    }
                } else {
                    XX_LOGD("[ffi] remove_model_result without reqId ignored");
                }
            } else if constexpr (std::is_same_v<T, agent::WireRenameSessionResult>) {
                auto j = agent::io::toJson(m);
                if (m.reqId != 0) {
                    if (m.ok) {
                        dispatchReply(m.reqId, stripEnvelope(j), true, {}, {});
                    } else {
                        dispatchReply(m.reqId, {}, false, "rejected", m.error);
                    }
                } else {
                    XX_LOGD("[ffi] rename_session_result without reqId ignored");
                }
            } else if constexpr (std::is_same_v<T, agent::WireListDirResult>) {
                auto j = agent::io::toJson(m);
                if (m.reqId != 0) {
                    if (m.ok) {
                        dispatchReply(m.reqId, stripEnvelope(j), true, {}, {});
                    } else {
                        dispatchReply(m.reqId, {}, false, "rejected", m.error);
                    }
                } else {
                    XX_LOGD("[ffi] list_dir_result without reqId ignored");
                }
            } else if constexpr (std::is_same_v<T, agent::WireInputAck>) {
                // input.send 用自己的 reqId 当 requestId, 所以这里直接按 reqId 回执
                auto j = agent::io::toJson(m);
                if (m.requestId == 0) {
                    XX_LOGD("[ffi] input_ack without requestId ignored");
                    return;
                }
                if (m.status == agent::InputStatus::Rejected) {
                    const std::string reason
                        = m.reason.empty() ? std::string{"rejected"} : m.reason;
                    dispatchReply(
                        m.requestId,
                        {},
                        false,
                        reason,
                        m.detail.empty() ? fmt::format("input rejected: {}", reason) : m.detail
                    );
                } else {
                    dispatchReply(m.requestId, stripEnvelope(j), true, {}, {});
                }
            } else if constexpr (std::is_same_v<T, agent::WireMessageQueueUpdate>) {
                if (!wantsEvents()) {
                    return;
                }
                emitEvent(AGENTXX_FFI_EVT_MESSAGE_QUEUE, dump(agent::io::toJson(m)));
            } else if constexpr (std::is_same_v<T, agent::WirePluginData>) {
                if (!wantsEvents()) {
                    return;
                }
                emitEvent(AGENTXX_FFI_EVT_PLUGIN_DATA, dump(agent::io::makePluginData(m)));
            } else if constexpr (std::is_same_v<T, agent::WireError>) {
                if (!wantsEvents()) {
                    return;
                }
                utilxx_base::Json j = utilxx_base::Json::object();
                j["code"]           = m.code;
                j["message"]        = m.message;
                emitEvent(AGENTXX_FFI_EVT_ERROR, dump(j));
            } else if constexpr (std::is_same_v<T, agent::WireHostToolCall>) {
                pendingHostToolCalls_.insert(m.callId);
                if (!wantsEvents()) {
                    return;
                }
                utilxx_base::Json j = {
                    {"callId",     m.callId    },
                    {"name",       m.name      },
                    {"argsJson",   m.argsJson  },
                    {"sessionId",  m.sessionId },
                    {"timeoutSec", m.timeoutSec},
                };
                emitEvent(AGENTXX_FFI_EVT_HOST_TOOL_CALL, dump(j));
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

std::string FfiClientAgentIO::dump(const utilxx_base::Json& j) {
    return j.dump();
}

} // namespace ffi
} // namespace agentxx
