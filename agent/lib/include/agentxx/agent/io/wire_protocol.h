#pragma once

#include "agentxx/agent/conversation_types.h"
#include "neograph/json.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace agent {
namespace io {

/// 双向 WS JSON 消息类型常量
/// - 约定: {"type": "<msgType>", "id": <opt requestId>, "thread": <threadId>, ...payload}
struct MsgType {
    // ===== Client -> Server =====
    inline static constexpr std::string_view Hello                  = "hello";
    inline static constexpr std::string_view UserInput              = "user_input";
    inline static constexpr std::string_view InterruptResponse      = "interrupt_response";
    inline static constexpr std::string_view Cancel                 = "cancel";
    inline static constexpr std::string_view SelectModel            = "select_model";
    inline static constexpr std::string_view GetModel               = "get_model";
    inline static constexpr std::string_view GetAppendComponentInfo = "get_append_component_info";
    inline static constexpr std::string_view Ping                   = "ping";

    // ===== Server -> Client =====
    inline static constexpr std::string_view HelloAck            = "hello_ack";
    inline static constexpr std::string_view DeltaMsg            = "delta";
    inline static constexpr std::string_view SyncMsg             = "sync";
    inline static constexpr std::string_view InterruptRequest    = "interrupt_request";
    inline static constexpr std::string_view TurnResult          = "turn_result";
    inline static constexpr std::string_view ContextStats        = "context_stats";
    inline static constexpr std::string_view ErrorMsg            = "error";
    inline static constexpr std::string_view LogMsg              = "log";
    inline static constexpr std::string_view ModelInfo           = "model_info";
    inline static constexpr std::string_view AppendComponentInfo = "append_component_info";
    inline static constexpr std::string_view GetContext          = "get_context";
    inline static constexpr std::string_view ContextMessages     = "context_messages";
    inline static constexpr std::string_view Pong                = "pong";
};

/// 中断/取消原因 (供 BaseAgent 区分中断来源)
struct CloseReason {
    inline static constexpr std::string_view UserCancel         = "user_cancel";
    inline static constexpr std::string_view ClientDisconnected = "client_disconnected";
    inline static constexpr std::string_view Timeout            = "timeout";
};

// ---------------------------------------------------------------------------
// Delta <-> json
// ---------------------------------------------------------------------------

inline std::string_view deltaTypeToString(Delta::Type t) noexcept {
    using T = Delta::Type;
    switch (t) {
        case T::TextToken:
            return "text_token";
        case T::ThinkingToken:
            return "thinking_token";
        case T::ToolStart:
            return "tool_start";
        case T::ToolEnd:
            return "tool_end";
        case T::TurnStart:
            return "turn_start";
        case T::TurnEnd:
            return "turn_end";
        case T::NodeStart:
            return "node_start";
        case T::NodeEnd:
            return "node_end";
    }
    return "text_token";
}

inline std::optional<Delta::Type> deltaTypeFromString(std::string_view s) noexcept {
    using T = Delta::Type;
    if (s == "text_token") {
        return T::TextToken;
    }
    if (s == "thinking_token") {
        return T::ThinkingToken;
    }
    if (s == "tool_start") {
        return T::ToolStart;
    }
    if (s == "tool_end") {
        return T::ToolEnd;
    }
    if (s == "turn_start") {
        return T::TurnStart;
    }
    if (s == "turn_end") {
        return T::TurnEnd;
    }
    if (s == "node_start") {
        return T::NodeStart;
    }
    if (s == "node_end") {
        return T::NodeEnd;
    }
    return std::nullopt;
}

inline neograph::json deltaToJson(const Delta& d) {
    neograph::json j = neograph::json::object();
    j["type"]        = std::string(deltaTypeToString(d.type));
    j["seq"]         = d.seq;
    if (!d.text.empty()) {
        j["text"] = d.text;
    }
    if (!d.msgId.empty()) {
        j["msg_id"] = d.msgId;
    }
    if (!d.toolName.empty()) {
        j["tool_name"] = d.toolName;
    }
    if (!d.toolCallId.empty()) {
        j["tool_call_id"] = d.toolCallId;
    }
    if (!d.arguments.empty()) {
        j["arguments"] = d.arguments;
    }
    if (!d.result.empty()) {
        j["result"] = d.result;
    }
    if (d.hasError) {
        j["has_error"] = d.hasError;
    }
    if (d.historyCount > 0) {
        j["history_count"] = d.historyCount;
    }
    if (!d.tailHash.empty()) {
        j["tail_hash"] = d.tailHash;
    }
    // 运行时统计字段 (TurnEnd 使用)
    if (d.startTimeMs > 0) {
        j["start_time_ms"] = d.startTimeMs;
    }
    if (d.durationMs > 0) {
        j["duration_ms"] = d.durationMs;
    }
    if (!d.nodeName.empty()) {
        j["node_name"] = d.nodeName;
    }
    return j;
}

inline std::optional<Delta> deltaFromJson(const neograph::json& j) {
    if (!j.is_object()) {
        return std::nullopt;
    }
    auto typeOpt = deltaTypeFromString(j.value("type", std::string{}));
    if (!typeOpt.has_value()) {
        return std::nullopt;
    }
    Delta d;
    d.type         = typeOpt.value();
    d.seq          = j.value("seq", uint64_t{0});
    d.text         = j.value("text", std::string{});
    d.msgId        = j.value("msg_id", std::string{});
    d.toolName     = j.value("tool_name", std::string{});
    d.toolCallId   = j.value("tool_call_id", std::string{});
    d.arguments    = j.value("arguments", std::string{});
    d.result       = j.value("result", std::string{});
    d.hasError     = j.value("has_error", false);
    d.historyCount = j.value("history_count", uint64_t{0});
    d.tailHash     = j.value("tail_hash", std::string{});
    d.startTimeMs  = j.value("start_time_ms", int64_t{0});
    d.durationMs   = j.value("duration_ms", int64_t{0});
    d.nodeName     = j.value("node_name", std::string{});
    return d;
}

// ---------------------------------------------------------------------------
// SyncPayload <-> json
// ---------------------------------------------------------------------------

inline neograph::json syncToJson(const SyncPayload& p) {
    neograph::json j   = neograph::json::object();
    j["from_index"]    = p.fromIndex;
    j["tail_hash"]     = p.tailHash;
    neograph::json arr = neograph::json::array();
    for (const auto& hm : p.messages) {
        arr.push_back(neograph::json{
            {"id",   hm.id  },
            {"data", hm.data}
        });
    }
    j["messages"] = std::move(arr);
    return j;
}

inline std::optional<SyncPayload> syncFromJson(const neograph::json& j) {
    if (!j.is_object()) {
        return std::nullopt;
    }
    SyncPayload p;
    p.fromIndex = j.value("from_index", uint64_t{0});
    p.tailHash  = j.value("tail_hash", std::string{});
    auto msgs   = j.value("messages", neograph::json::array());
    if (msgs.is_array()) {
        for (const auto& m : msgs) {
            HistoryMessage hm;
            hm.id   = m.value("id", std::string{});
            hm.data = m.value("data", neograph::json{});
            p.messages.push_back(std::move(hm));
        }
    }
    return p;
}

// ---------------------------------------------------------------------------
// 消息构造 (Client -> Server)
// ---------------------------------------------------------------------------

inline neograph::json makeHello(
    std::string_view threadId,
    std::string_view token,
    uint64_t         lastSeq  = 0,
    std::string_view tailHash = ""
) {
    neograph::json j = {
        {"type",   MsgType::Hello},
        {"thread", threadId      },
        {"token",  token         },
    };
    if (lastSeq > 0) {
        j["last_seq"] = lastSeq;
    }
    if (!tailHash.empty()) {
        j["tail_hash"] = tailHash;
    }
    return j;
}

inline neograph::json makeUserInput(std::string_view threadId, std::string_view text) {
    return neograph::json{
        {"type",   MsgType::UserInput},
        {"thread", threadId          },
        {"text",   text              },
    };
}

inline neograph::json makeInterruptResponse(int64_t id, const neograph::json& result) {
    return neograph::json{
        {"type",   MsgType::InterruptResponse},
        {"id",     id                        },
        {"result", result                    },
    };
}

inline neograph::json makeCancel(std::string_view threadId) {
    return neograph::json{
        {"type",   MsgType::Cancel},
        {"thread", threadId       },
    };
}

inline neograph::json makeSelectModel(std::string_view threadId, std::string_view model) {
    return neograph::json{
        {"type",   MsgType::SelectModel},
        {"thread", threadId            },
        {"model",  model               },
    };
}

inline neograph::json makePing(int64_t t) {
    return neograph::json{
        {"type", MsgType::Ping},
        {"t",    t            },
    };
}

// ---------------------------------------------------------------------------
// 消息构造 (Server -> Client)
// ---------------------------------------------------------------------------

inline neograph::json makeHelloAck(
    bool                            ok,
    std::string_view                threadId,
    std::string_view                tailHash,
    const std::vector<std::string>& models
) {
    neograph::json j = {
        {"type",   MsgType::HelloAck},
        {"ok",     ok               },
        {"thread", threadId         },
    };
    if (!tailHash.empty()) {
        j["tail_hash"] = tailHash;
    }
    if (!models.empty()) {
        j["models"] = models;
    }
    return j;
}

inline neograph::json makeDeltaMsg(const Delta& d) {
    neograph::json j = deltaToJson(d);
    // 复用 deltaToJson 的字段, 但信封 type 固定为 "delta"
    j["type"] = MsgType::DeltaMsg;
    j["kind"] = std::string(deltaTypeToString(d.type));
    return j;
}

/// 从 "delta" 信封还原 Delta (type 字段取自 "kind")
inline std::optional<Delta> deltaMsgFromJson(const neograph::json& j) {
    if (!j.is_object()) {
        return std::nullopt;
    }
    auto patched    = j;
    patched["type"] = j.value("kind", std::string{});
    return deltaFromJson(patched);
}

inline neograph::json makeSyncMsg(const SyncPayload& p, uint64_t deltaSeq = 0) {
    neograph::json j = syncToJson(p);
    j["type"]        = MsgType::SyncMsg;
    if (deltaSeq > 0) {
        j["delta_seq"] = deltaSeq;
    }
    return j;
}

inline std::optional<SyncPayload> syncMsgFromJson(const neograph::json& j) {
    return syncFromJson(j);
}

inline neograph::json makeInterruptRequest(
    int64_t          id,
    std::string_view threadId,
    std::string_view node,
    std::string_view value,
    std::string_view argJson
) {
    return neograph::json{
        {"type",     MsgType::InterruptRequest},
        {"id",       id                       },
        {"thread",   threadId                 },
        {"node",     node                     },
        {"value",    value                    },
        {"arg_json", argJson                  },
    };
}

inline neograph::json makeTurnResult(
    std::string_view threadId,
    bool             hasError,
    std::string_view errorMessage,
    bool             interrupted,
    int64_t          startTimeMs = 0,
    int64_t          durationMs  = 0
) {
    neograph::json j = {
        {"type",        MsgType::TurnResult},
        {"thread",      threadId           },
        {"has_error",   hasError           },
        {"interrupted", interrupted        },
    };
    if (!errorMessage.empty()) {
        j["error_message"] = errorMessage;
    }
    if (startTimeMs > 0) {
        j["start_time_ms"] = startTimeMs;
    }
    if (durationMs > 0) {
        j["duration_ms"] = durationMs;
    }
    return j;
}

inline neograph::json makeContextStats(uint64_t contextTokens, uint64_t maxContextTokens) {
    return neograph::json{
        {"type",               MsgType::ContextStats},
        {"context_tokens",     contextTokens        },
        {"max_context_tokens", maxContextTokens     },
    };
}

inline neograph::json makeError(int code, std::string_view message) {
    return neograph::json{
        {"type",    MsgType::ErrorMsg},
        {"code",    code             },
        {"message", message          },
    };
}

inline neograph::json makeGetModel(std::string_view threadId) {
    return neograph::json{
        {"type",   MsgType::GetModel},
        {"thread", threadId         },
    };
}

inline neograph::json
    makeModelInfo(std::string_view currentModel, const std::vector<std::string>& models) {
    neograph::json j = {
        {"type",          MsgType::ModelInfo},
        {"current_model", currentModel      },
    };
    if (!models.empty()) {
        j["models"] = models;
    }
    return j;
}

// ---------------------------------------------------------------------------
// AppendComponentNotification <-> json (客户端拉取 MCP/Skill/Memory 启动信息)
// ---------------------------------------------------------------------------

inline neograph::json appendComponentNotificationToJson(const AppendComponentNotification& n) {
    return neograph::json{
        {"type",          static_cast<int>(n.type)},
        {"name",          n.name                  },
        {"success",       n.success               },
        {"error_message", n.errorMessage          },
    };
}

inline AppendComponentNotification appendComponentNotificationFromJson(const neograph::json& j) {
    AppendComponentNotification n;
    n.type         = static_cast<AppendComponentNotification::Type>(j.value("type", 0));
    n.name         = j.value("name", std::string{});
    n.success      = j.value("success", true);
    n.errorMessage = j.value("error_message", std::string{});
    return n;
}

inline neograph::json makeGetAppendComponentInfo(std::string_view threadId) {
    return neograph::json{
        {"type",   MsgType::GetAppendComponentInfo},
        {"thread", threadId                       },
    };
}

inline neograph::json
    makeAppendComponentInfo(const std::vector<AppendComponentNotification>& notifications) {
    neograph::json j = {
        {"type", MsgType::AppendComponentInfo}
    };
    neograph::json arr = neograph::json::array();
    for (const auto& n : notifications) {
        arr.push_back(appendComponentNotificationToJson(n));
    }
    j["notifications"] = std::move(arr);
    return j;
}

inline std::vector<AppendComponentNotification> appendComponentInfoFromJson(const neograph::json& j
) {
    std::vector<AppendComponentNotification> out;
    auto arr = j.value("notifications", neograph::json::array());
    if (arr.is_array()) {
        for (const auto& item : arr) {
            out.push_back(appendComponentNotificationFromJson(item));
        }
    }
    return out;
}

inline neograph::json makePong(int64_t t) {
    return neograph::json{
        {"type", MsgType::Pong},
        {"t",    t            },
    };
}

inline neograph::json makeGetContext(std::string_view threadId) {
    return neograph::json{
        {"type",   MsgType::GetContext},
        {"thread", threadId           },
    };
}

inline neograph::json makeContextMessages(const neograph::json& messages) {
    return neograph::json{
        {"type",     MsgType::ContextMessages},
        {"messages", messages                },
    };
}

inline neograph::json makeLog(int level, std::string_view message) {
    return neograph::json{
        {"type",    MsgType::LogMsg},
        {"level",   level          },
        {"message", message        },
    };
}

// ---------------------------------------------------------------------------
// 通用字段读取
// ---------------------------------------------------------------------------

inline std::string msgType(const neograph::json& j) {
    return j.is_object() ? j.value("type", std::string{}) : std::string{};
}

} // namespace io
} // namespace agent
} // namespace agentxx
