#pragma once

#include "agentxx/agent/conversation_types.h"
#include "agentxx/agent/io/agent_io_transport.h"
#include "agentxx/util/json.h"
#include "agentxx/util/json_view.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace agent {
namespace io {

/// 双向 WS JSON 消息类型常量
/// - 约定: {"type": "<msgType>", "id": <opt requestId>, "sessionId": <sessionId>, ...payload}
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
    /// 客户端请求压缩当前会话上下文
    inline static constexpr std::string_view CompactContext = "compact_context";
    /// 客户端记住权限选择: 注册路径规则到服务端权限中间件
    inline static constexpr std::string_view SetPermission = "set_permission";
    /// 客户端请求持久化会话列表 (会话选择弹窗数据源)
    inline static constexpr std::string_view ListSessions = "list_sessions";
    /// 客户端请求切换当前连接的会话 (重新绑定 sessionId 并回推历史)
    inline static constexpr std::string_view SwitchSession = "switch_session";
    /// 客户端请求清空消息队列
    inline static constexpr std::string_view ClearMessageQueue = "clear_message_queue";
    /// 客户端请求删除单条排队消息
    inline static constexpr std::string_view RemoveQueueItem = "remove_queue_item";
    /// 客户端请求打断当前会话并立即执行队列首条 (insert 按钮)
    inline static constexpr std::string_view InterruptAndRunNext = "interrupt_and_run_next";
    /// 客户端请求 viewMessages 历史分页 (恢复长会话时初始仅同步末尾窗口,
    /// 用户向上滚动时按页拉取更早历史; 见 WireGetViewMessages)
    inline static constexpr std::string_view GetViewMessages = "get_view_messages";

    // ===== Server -> Client =====
    inline static constexpr std::string_view HelloAck         = "hello_ack";
    inline static constexpr std::string_view DeltaMsg         = "delta";
    inline static constexpr std::string_view SyncMsg          = "sync";
    inline static constexpr std::string_view InterruptRequest = "interrupt_request";
    /// 服务端通知中断已过期 (超时/会话取消): 客户端应将对应未操作的中断消息标记为过期
    inline static constexpr std::string_view InterruptExpired    = "interrupt_expired";
    inline static constexpr std::string_view TurnResult          = "turn_result";
    inline static constexpr std::string_view ContextStats        = "context_stats";
    inline static constexpr std::string_view ErrorMsg            = "error";
    inline static constexpr std::string_view LogMsg              = "log";
    inline static constexpr std::string_view ModelInfo           = "model_info";
    inline static constexpr std::string_view AppendComponentInfo = "append_component_info";
    inline static constexpr std::string_view GetContext          = "get_context";
    inline static constexpr std::string_view ContextMessages     = "context_messages";
    /// 服务端持久化会话列表响应 (ListSessions 的结果)
    inline static constexpr std::string_view SessionList = "session_list";
    inline static constexpr std::string_view Pong        = "pong";
    /// 服务端插件事件转发 (WirePluginData: 插件名 + 事件名 + JSON 载荷)
    inline static constexpr std::string_view PluginData = "plugin_data";
    /// client 插件事件上行 (WirePluginDataUp: 插件名 + 事件名 + JSON 载荷;
    /// 服务端发布到事件总线 topic `client.{插件名}.{事件名}`)
    inline static constexpr std::string_view PluginDataUp = "plugin_data_up";
    /// 服务端消息队列同步 (WireMessageQueueUpdate: 会话 ID + 排队消息列表)
    inline static constexpr std::string_view MessageQueueUpdate = "message_queue_update";
    /// 服务端 viewMessages 历史分页响应 (WireViewMessagesPage)
    inline static constexpr std::string_view ViewMessagesPage = "view_messages_page";
};

/// 中断/取消原因 (供 BaseAgent 区分中断来源)
struct CloseReason {
    inline static constexpr std::string_view UserCancel         = "user_cancel";
    inline static constexpr std::string_view ClientDisconnected = "client_disconnected";
    inline static constexpr std::string_view Timeout            = "timeout";
};

// ---------------------------------------------------------------------------
// WireDelta <-> json
// ---------------------------------------------------------------------------

inline std::string_view deltaTypeToString(WireDelta::Type t) noexcept {
    using T = WireDelta::Type;
    switch (t) {
        case T::TextToken:
            return "text_token";
        case T::ThinkToken:
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
        case T::MessageUITip:
            return "message_tip";
        case T::InsertMessage:
            return "insert_message";
        case T::UpdateMessage:
            return "update_message";
    }
    return "text_token";
}

inline std::optional<WireDelta::Type> deltaTypeFromString(std::string_view s) noexcept {
    using T = WireDelta::Type;
    if (s == "text_token") {
        return T::TextToken;
    }
    if (s == "thinking_token") {
        return T::ThinkToken;
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
    if (s == "message_tip") {
        return T::MessageUITip;
    }
    if (s == "insert_message") {
        return T::InsertMessage;
    }
    if (s == "update_message") {
        return T::UpdateMessage;
    }
    return std::nullopt;
}

inline agentxx::util::Json deltaToJson(const WireDelta& d) {
    agentxx::util::Json j = agentxx::util::Json::object();
    j["type"]             = std::string(deltaTypeToString(d.type));
    j["seq"]              = d.seq;
    if (!d.text.empty()) {
        j["text"] = d.text;
    }
    if (!d.msgId.empty()) {
        j["msgId"] = d.msgId;
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
        j["hasError"] = d.hasError;
    }
    if (d.historyCount > 0) {
        j["historyCount"] = d.historyCount;
    }
    if (!d.tailHash.empty()) {
        j["tailHash"] = d.tailHash;
    }
    // 运行时统计字段 (TurnEnd 使用)
    if (d.startTimeMs > 0) {
        j["startTimeMs"] = d.startTimeMs;
    }
    if (d.durationMs > 0) {
        j["durationMs"] = d.durationMs;
    }
    if (d.tps > 0.0) {
        j["tps"] = d.tps;
    }
    if (!d.nodeName.empty()) {
        j["nodeName"] = d.nodeName;
    }
    // TurnStart 即时回显附件元数据（不含 dataUrl，见 WireDelta::attachments 注释）
    if (d.type == WireDelta::Type::TurnStart && !d.attachments.empty()) {
        agentxx::util::Json arr = agentxx::util::Json::array();
        for (const auto& a : d.attachments) {
            MediaAttachment meta = a;
            meta.dataUrl.clear();
            arr.push_back(meta.toJson());
        }
        j["attachments"] = std::move(arr);
    }
    if (d.think) {
        agentxx::util::Json th = agentxx::util::Json::object();
        if (d.think->reasoningTokens > 0) {
            th["reasoning_tokens"] = d.think->reasoningTokens;
        }
        if (d.think->isEncrypted) {
            th["is_encrypted"] = true;
        }
        if (!th.empty()) {
            j["think"] = std::move(th);
        }
    }
    if (d.message) {
        j["message"] = d.message->toJson();
    }
    // MessageUITip: 提示级别
    if (d.type == WireDelta::Type::MessageUITip) {
        switch (d.tipType) {
            case WireDelta::TipType::Warning:
                j["tipType"] = "warning";
                break;
            case WireDelta::TipType::Error:
                j["tipType"] = "error";
                break;
            default:
                j["tipType"] = "info";
                break;
        }
    }
    return j;
}

inline std::optional<WireDelta> deltaFromJson(const agentxx::util::Json& j) {
    if (!j.is_object()) {
        return std::nullopt;
    }
    auto typeOpt = deltaTypeFromString(j.value("type", std::string{}));
    if (!typeOpt.has_value()) {
        return std::nullopt;
    }
    WireDelta d;
    d.type         = typeOpt.value();
    d.seq          = j.value("seq", uint64_t{0});
    d.text         = j.value("text", std::string{});
    d.msgId        = j.value("msgId", std::string{});
    d.toolName     = j.value("tool_name", std::string{});
    d.toolCallId   = j.value("tool_call_id", std::string{});
    d.arguments    = j.value("arguments", std::string{});
    d.result       = j.value("result", std::string{});
    d.hasError     = j.value("hasError", false);
    d.historyCount = j.value("historyCount", uint64_t{0});
    d.tailHash     = j.value("tailHash", std::string{});
    d.startTimeMs  = j.value("startTimeMs", int64_t{0});
    d.durationMs   = j.value("durationMs", int64_t{0});
    d.tps          = j.value("tps", 0.0);
    d.nodeName     = j.value("nodeName", std::string{});
    if (j.contains("attachments") && j["attachments"].is_array()
        && d.type == WireDelta::Type::TurnStart) {
        for (const auto& a : j["attachments"]) {
            d.attachments.push_back(MediaAttachment::fromJson(a));
        }
    }
    if (j.contains("think") && j["think"].is_object()) {
        ViewMessage::ThinkData th;
        th.reasoningTokens = j["think"].value("reasoning_tokens", 0);
        th.isEncrypted     = j["think"].value("is_encrypted", false);
        d.think            = std::move(th);
    }
    if (j.contains("message") && j["message"].is_object()) {
        d.message = std::make_shared<ViewMessage>(ViewMessage::fromJson(j["message"]));
    }
    if (d.type == WireDelta::Type::MessageUITip) {
        const auto tip = j.value("tipType", std::string{"info"});
        if (tip == "warning") {
            d.tipType = WireDelta::TipType::Warning;
        } else if (tip == "error") {
            d.tipType = WireDelta::TipType::Error;
        } else {
            d.tipType = WireDelta::TipType::Info;
        }
    }
    return d;
}

// ---------------------------------------------------------------------------
// WireSyncPayload <-> json
// ---------------------------------------------------------------------------

inline agentxx::util::Json messageQueueItemToJson(const MessageQueueItem& item) {
    agentxx::util::Json j = {
        {"id",          item.id         },
        {"text",        item.text       },
        {"createdAtMs", item.createdAtMs},
    };
    if (!item.model.empty()) {
        j["model"] = item.model;
    }
    if (!item.attachments.empty()) {
        agentxx::util::Json arr = agentxx::util::Json::array();
        for (const auto& att : item.attachments) {
            arr.push_back(att.toJson());
        }
        j["attachments"] = std::move(arr);
    }
    return j;
}

inline MessageQueueItem messageQueueItemFromJson(const agentxx::util::Json& j) {
    MessageQueueItem item;
    item.id          = j.value("id", std::string{});
    item.text        = j.value("text", std::string{});
    item.model       = j.value("model", std::string{});
    item.createdAtMs = j.value("createdAtMs", int64_t{0});
    if (j.contains("attachments") && j["attachments"].is_array()) {
        for (const auto& att : j["attachments"]) {
            item.attachments.push_back(MediaAttachment::fromJson(att));
        }
    }
    return item;
}

inline agentxx::util::Json syncToJson(const WireSyncPayload& p) {
    agentxx::util::Json j = agentxx::util::Json::object();
    j["fromIndex"]        = p.fromIndex;
    j["tailHash"]         = p.tailHash;
    // 历史分页元数据 (尾窗同步时 fromIndex>0 / totalMessages>0; 全量同步
    // 时 totalMessages == messages.size(), 字段冗余但便于客户端统一判断)
    j["totalMessages"]      = p.totalMessages;
    agentxx::util::Json arr = agentxx::util::Json::array();
    for (const auto& vm : p.messages) {
        arr.push_back(vm.toJson());
    }
    j["messages"] = std::move(arr);
    if (!p.messageQueue.empty()) {
        agentxx::util::Json qArr = agentxx::util::Json::array();
        for (const auto& item : p.messageQueue) {
            qArr.push_back(messageQueueItemToJson(item));
        }
        j["message_queue"] = std::move(qArr);
    }
    return j;
}

inline std::optional<WireSyncPayload> syncFromJson(const agentxx::util::Json& j) {
    if (!j.is_object()) {
        return std::nullopt;
    }
    WireSyncPayload p;
    p.fromIndex     = j.value("fromIndex", uint64_t{0});
    p.tailHash      = j.value("tailHash", std::string{});
    p.totalMessages = j.value("totalMessages", uint64_t{0});
    auto msgs       = j.value("messages", agentxx::util::Json::array());
    if (msgs.is_array()) {
        for (const auto& m : msgs) {
            p.messages.push_back(ViewMessage::fromJson(m));
        }
    }
    if (j.contains("message_queue") && j["message_queue"].is_array()) {
        for (const auto& qm : j["message_queue"]) {
            p.messageQueue.push_back(messageQueueItemFromJson(qm));
        }
    }
    return p;
}

// ---------------------------------------------------------------------------
// 消息构造 (Client -> Server)
// ---------------------------------------------------------------------------

inline agentxx::util::Json makeHello(
    std::string_view sessionId,
    std::string_view token,
    uint64_t         lastSeq  = 0,
    std::string_view tailHash = "",
    std::string_view language = ""
) {
    agentxx::util::Json j = {
        {"type",      MsgType::Hello},
        {"sessionId", sessionId     },
        {"token",     token         },
    };
    if (lastSeq > 0) {
        j["lastSeq"] = lastSeq;
    }
    if (!tailHash.empty()) {
        j["tailHash"] = tailHash;
    }
    if (!language.empty()) {
        j["language"] = language;
    }
    return j;
}

inline agentxx::util::Json makeUserInput(
    std::string_view                    sessionId,
    std::string_view                    text,
    std::string_view                    model       = "",
    const std::vector<MediaAttachment>& attachments = {}
) {
    agentxx::util::Json j = {
        {"type",      MsgType::UserInput},
        {"sessionId", sessionId         },
        {"text",      text              },
    };
    if (!model.empty()) {
        j["model"] = model;
    }
    if (!attachments.empty()) {
        agentxx::util::Json arr = agentxx::util::Json::array();
        for (const auto& att : attachments) {
            arr.push_back(att.toJson());
        }
        j["attachments"] = std::move(arr);
    }
    return j;
}

inline agentxx::util::Json makeInterruptResponse(int64_t id, const agentxx::util::Json& result) {
    return agentxx::util::Json{
        {"type",   MsgType::InterruptResponse},
        {"id",     id                        },
        {"result", result                    },
    };
}

inline agentxx::util::Json makeCancel(std::string_view sessionId) {
    return agentxx::util::Json{
        {"type",      MsgType::Cancel},
        {"sessionId", sessionId      },
    };
}

inline agentxx::util::Json makeSelectModel(std::string_view sessionId, std::string_view model) {
    return agentxx::util::Json{
        {"type",      MsgType::SelectModel},
        {"sessionId", sessionId           },
        {"model",     model               },
    };
}

inline agentxx::util::Json makePing(int64_t t) {
    return agentxx::util::Json{
        {"type", MsgType::Ping},
        {"t",    t            },
    };
}

// ---------------------------------------------------------------------------
// 消息构造 (Server -> Client)
// ---------------------------------------------------------------------------

inline agentxx::util::Json makeHelloAck(
    bool                                         ok,
    std::string_view                             sessionId,
    std::string_view                             tailHash,
    const std::vector<std::string>&              models,
    const std::vector<WireHelloAck::PluginInfo>& plugins = {}
) {
    agentxx::util::Json j = {
        {"type",      MsgType::HelloAck},
        {"ok",        ok               },
        {"sessionId", sessionId        },
    };
    if (!tailHash.empty()) {
        j["tailHash"] = tailHash;
    }
    if (!models.empty()) {
        j["models"] = models;
    }
    // 服务端已加载插件结构化列表 (名字+版本+声明接口, client 插件据此判断
    // 对端可用性与能力); 空时不携带 (缺字段按"服务端未提供"处理)
    if (!plugins.empty()) {
        auto arr = agentxx::util::Json::array();
        for (const auto& p : plugins) {
            arr.push_back({
                {"name",       p.name      },
                {"version",    p.version   },
                {"interfaces", p.interfaces}
            });
        }
        j["plugins"] = std::move(arr);
    }
    return j;
}

inline agentxx::util::Json makeDeltaMsg(const WireDelta& d) {
    agentxx::util::Json j = deltaToJson(d);
    // 复用 deltaToJson 的字段, 但信封 type 固定为 "delta"
    j["type"] = MsgType::DeltaMsg;
    j["kind"] = std::string(deltaTypeToString(d.type));
    return j;
}

/// 从 "delta" 信封还原 WireDelta (type 字段取自 "kind")
inline std::optional<WireDelta> deltaMsgFromJson(const agentxx::util::Json& j) {
    if (!j.is_object()) {
        return std::nullopt;
    }
    auto patched    = j;
    patched["type"] = j.value("kind", std::string{});
    return deltaFromJson(patched);
}

inline agentxx::util::Json makeSyncMsg(const WireSyncPayload& p, uint64_t deltaSeq = 0) {
    agentxx::util::Json j = syncToJson(p);
    j["type"]             = MsgType::SyncMsg;
    if (deltaSeq > 0) {
        j["deltaSeq"] = deltaSeq;
    }
    return j;
}

inline std::optional<WireSyncPayload> syncMsgFromJson(const agentxx::util::Json& j) {
    return syncFromJson(j);
}

inline agentxx::util::Json makeInterruptRequest(
    int64_t          id,
    std::string_view sessionId,
    std::string_view node,
    std::string_view value,
    std::string_view argJson
) {
    return agentxx::util::Json{
        {"type",      MsgType::InterruptRequest},
        {"id",        id                       },
        {"sessionId", sessionId                },
        {"node",      node                     },
        {"value",     value                    },
        {"argJson",   argJson                  },
    };
}

/// 服务端通知中断已过期 (超时/取消): 对应 WireInterruptRequest 的 id
inline agentxx::util::Json makeInterruptExpired(int64_t id, std::string_view sessionId) {
    return agentxx::util::Json{
        {"type",      MsgType::InterruptExpired},
        {"id",        id                       },
        {"sessionId", sessionId                },
    };
}

inline agentxx::util::Json makeTurnResult(
    std::string_view sessionId,
    bool             hasError,
    std::string_view errorMessage,
    bool             interrupted,
    int64_t          startTimeMs = 0,
    int64_t          durationMs  = 0
) {
    agentxx::util::Json j = {
        {"type",        MsgType::TurnResult},
        {"sessionId",   sessionId          },
        {"hasError",    hasError           },
        {"interrupted", interrupted        },
    };
    if (!errorMessage.empty()) {
        j["errorMessage"] = errorMessage;
    }
    if (startTimeMs > 0) {
        j["startTimeMs"] = startTimeMs;
    }
    if (durationMs > 0) {
        j["durationMs"] = durationMs;
    }
    return j;
}

inline agentxx::util::Json
    makeContextStats(uint64_t contextTokens, uint64_t maxContextTokens, double tps = 0.0) {
    agentxx::util::Json j = {
        {"type",             MsgType::ContextStats},
        {"contextTokens",    contextTokens        },
        {"maxContextTokens", maxContextTokens     },
    };
    if (tps > 0.0) {
        j["tps"] = tps;
    }
    return j;
}

inline agentxx::util::Json makeError(int code, std::string_view message) {
    return agentxx::util::Json{
        {"type",    MsgType::ErrorMsg},
        {"code",    code             },
        {"message", message          },
    };
}

inline agentxx::util::Json makeGetModel(std::string_view sessionId) {
    return agentxx::util::Json{
        {"type",      MsgType::GetModel},
        {"sessionId", sessionId        },
    };
}

inline agentxx::util::Json makeModelInfo(
    std::string_view                        currentModel,
    const std::vector<std::string>&         models,
    const std::vector<ModelCapabilityInfo>& capabilities = {}
) {
    agentxx::util::Json j = {
        {"type",         MsgType::ModelInfo},
        {"currentModel", currentModel      },
    };
    if (!models.empty()) {
        j["models"] = models;
    }
    if (!capabilities.empty()) {
        agentxx::util::Json caps = agentxx::util::Json::array();
        for (const auto& cap : capabilities) {
            caps.push_back({
                {"name",        cap.name      },
                {"image_input", cap.imageInput},
                {"audio_input", cap.audioInput},
                {"video_input", cap.videoInput},
            });
        }
        j["capabilities"] = std::move(caps);
    }
    return j;
}

// ---------------------------------------------------------------------------
// AppendComponentNotification <-> json (客户端拉取 MCP/Skill/Memory 启动信息)
// ---------------------------------------------------------------------------

inline agentxx::util::Json appendComponentNotificationToJson(const AppendComponentNotification& n) {
    return agentxx::util::Json{
        {"type",         static_cast<int>(n.type)},
        {"name",         n.name                  },
        {"success",      n.success               },
        {"errorMessage", n.errorMessage          },
    };
}

inline AppendComponentNotification appendComponentNotificationFromJson(const agentxx::util::Json& j
) {
    AppendComponentNotification n;
    n.type         = static_cast<AppendComponentNotification::Type>(j.value("type", 0));
    n.name         = j.value("name", std::string{});
    n.success      = j.value("success", true);
    n.errorMessage = j.value("errorMessage", std::string{});
    return n;
}

inline agentxx::util::Json makeGetAppendComponentInfo(std::string_view sessionId) {
    return agentxx::util::Json{
        {"type",      MsgType::GetAppendComponentInfo},
        {"sessionId", sessionId                      },
    };
}

inline agentxx::util::Json
    makeAppendComponentInfo(const std::vector<AppendComponentNotification>& notifications) {
    agentxx::util::Json j = {
        {"type", MsgType::AppendComponentInfo}
    };
    agentxx::util::Json arr = agentxx::util::Json::array();
    for (const auto& n : notifications) {
        arr.push_back(appendComponentNotificationToJson(n));
    }
    j["notifications"] = std::move(arr);
    return j;
}

inline std::vector<AppendComponentNotification>
    appendComponentInfoFromJson(const agentxx::util::Json& j) {
    std::vector<AppendComponentNotification> out;
    auto arr = j.value("notifications", agentxx::util::Json::array());
    if (arr.is_array()) {
        for (const auto& item : arr) {
            out.push_back(appendComponentNotificationFromJson(item));
        }
    }
    return out;
}

inline agentxx::util::Json makePong(int64_t t) {
    return agentxx::util::Json{
        {"type", MsgType::Pong},
        {"t",    t            },
    };
}

inline agentxx::util::Json makeGetContext(std::string_view sessionId) {
    return agentxx::util::Json{
        {"type",      MsgType::GetContext},
        {"sessionId", sessionId          },
    };
}

inline agentxx::util::Json makeCompactContext(std::string_view sessionId) {
    return agentxx::util::Json{
        {"type",      MsgType::CompactContext},
        {"sessionId", sessionId              },
    };
}

/// 客户端记住权限选择 (Client -> Server): 注册路径规则到服务端权限中间件
inline agentxx::util::Json
    makeSetPermission(std::string_view sessionId, std::string_view path, bool allow, size_t index) {
    return agentxx::util::Json{
        {"type",      MsgType::SetPermission},
        {"sessionId", sessionId             },
        {"path",      path                  },
        {"allow",     allow                 },
        {"index",     index                 },
    };
}

inline WireSetPermission setPermissionFromJson(const agentxx::util::Json& j) {
    WireSetPermission m;
    m.sessionId = j.value("sessionId", std::string{});
    m.path      = j.value("path", std::string{});
    m.allow     = j.value("allow", true);
    m.index     = j.value("index", size_t{0});
    return m;
}

inline agentxx::util::Json makeContextMessages(const agentxx::util::Json& messages) {
    return agentxx::util::Json{
        {"type",     MsgType::ContextMessages},
        {"messages", messages                },
    };
}

// ---------------------------------------------------------------------------
// 会话列表 / 会话切换 (TUI 会话选择弹窗)
// ---------------------------------------------------------------------------

/// 客户端请求持久化会话列表 (无载荷; 列举全部持久化会话)
/// - 分页字段可选: beforeMs/beforeId/limit 均缺省时为旧行为 (全量列举),
///   与旧版服务端互通
inline agentxx::util::Json
    makeListSessions(int64_t beforeMs = 0, std::string_view beforeId = "", uint32_t limit = 0) {
    agentxx::util::Json j = {
        {"type", MsgType::ListSessions},
    };
    if (beforeMs > 0) {
        j["beforeMs"] = beforeMs;
    }
    if (!beforeId.empty()) {
        j["beforeId"] = beforeId;
    }
    if (limit > 0) {
        j["limit"] = limit;
    }
    return j;
}

inline WireListSessions listSessionsFromJson(const agentxx::util::Json& j) {
    WireListSessions m;
    m.beforeMs = j.value("beforeMs", int64_t{0});
    m.beforeId = j.value("beforeId", std::string{});
    m.limit    = j.value("limit", uint32_t{0});
    return m;
}

inline agentxx::util::Json sessionInfoToJson(const SessionInfo& s) {
    agentxx::util::Json j = {
        {"sessionId",    s.sessionId   },
        {"lastActiveMs", s.lastActiveMs},
    };
    if (!s.title.empty()) {
        j["title"] = s.title;
    }
    return j;
}

inline SessionInfo sessionInfoFromJson(const agentxx::util::Json& j) {
    SessionInfo s;
    s.sessionId    = j.value("sessionId", std::string{});
    s.title        = j.value("title", std::string{});
    s.lastActiveMs = j.value("lastActiveMs", int64_t{0});
    return s;
}

inline agentxx::util::Json makeSessionList(
    const std::vector<SessionInfo>& sessions,
    uint64_t                        totalCount = 0,
    bool                            hasMore    = false
) {
    agentxx::util::Json j = {
        {"type", MsgType::SessionList}
    };
    agentxx::util::Json arr = agentxx::util::Json::array();
    for (const auto& s : sessions) {
        arr.push_back(sessionInfoToJson(s));
    }
    j["sessions"] = std::move(arr);
    // 分页元数据 (可选携带; 缺省时旧版客户端按全量列表处理)
    if (totalCount > 0) {
        j["totalCount"] = totalCount;
    }
    if (hasMore) {
        j["hasMore"] = true;
    }
    return j;
}

inline WireSessionList sessionListFromJson(const agentxx::util::Json& j) {
    WireSessionList out;
    auto            arr = j.value("sessions", agentxx::util::Json::array());
    if (arr.is_array()) {
        for (const auto& item : arr) {
            out.sessions.push_back(sessionInfoFromJson(item));
        }
    }
    // 分页元数据 (旧版服务端不携带 → 0/false, 客户端按全量响应处理)
    out.totalCount = j.value("totalCount", uint64_t{0});
    out.hasMore    = j.value("hasMore", false);
    return out;
}

/// 客户端请求切换当前连接的会话
inline agentxx::util::Json makeSwitchSession(std::string_view sessionId) {
    return agentxx::util::Json{
        {"type",      MsgType::SwitchSession},
        {"sessionId", sessionId             },
    };
}

inline WireSwitchSession switchSessionFromJson(const agentxx::util::Json& j) {
    WireSwitchSession m;
    m.sessionId = j.value("sessionId", std::string{});
    return m;
}

// ---------------------------------------------------------------------------
// 插件事件转发 (Server -> Client): 插件经事件总线发布的事件原样转发
// - data 为 JSON 载荷字符串 (语义由插件定义); 频率由插件自身控制
// ---------------------------------------------------------------------------

/// 插件事件转发 (Server -> Client): 插件经事件总线发布的事件原样转发
/// - data 为 JSON 载荷字符串 (语义由插件定义); 频率由插件自身控制
inline agentxx::util::Json makePluginData(const WirePluginData& p) {
    agentxx::util::Json j = {
        {"type",   MsgType::PluginData},
        {"plugin", p.plugin           },
        {"event",  p.event            },
        {"data",   p.data             },
    };
    return j;
}

inline WirePluginData pluginDataFromJson(const agentxx::util::Json& j) {
    WirePluginData p;
    p.plugin = j.value("plugin", std::string{});
    p.event  = j.value("event", std::string{});
    p.data   = j.value("data", std::string{});
    return p;
}

/// client 插件事件上行 (Client -> Server): WirePluginDataUp
/// - 载荷 JSON 原样透传 (语义由插件定义); 服务端发布到事件总线
///   topic `client.{插件名}.{事件名}`
inline agentxx::util::Json makePluginDataUp(const WirePluginDataUp& p) {
    agentxx::util::Json j = {
        {"type",   MsgType::PluginDataUp},
        {"plugin", p.plugin             },
        {"event",  p.event              },
        {"data",   p.data               },
    };
    return j;
}

inline WirePluginDataUp pluginDataUpFromJson(const agentxx::util::Json& j) {
    WirePluginDataUp p;
    p.plugin = j.value("plugin", std::string{});
    p.event  = j.value("event", std::string{});
    p.data   = j.value("data", std::string{});
    return p;
}

// ---------------------------------------------------------------------------
// 消息队列相关 (Client <-> Server)
// ---------------------------------------------------------------------------

inline agentxx::util::Json
    makeMessageQueueUpdate(std::string_view sessionId, const std::vector<MessageQueueItem>& items) {
    agentxx::util::Json j = {
        {"type",      MsgType::MessageQueueUpdate},
        {"sessionId", sessionId                  },
    };
    agentxx::util::Json arr = agentxx::util::Json::array();
    for (const auto& item : items) {
        arr.push_back(messageQueueItemToJson(item));
    }
    j["items"] = std::move(arr);
    return j;
}

inline WireMessageQueueUpdate messageQueueUpdateFromJson(const agentxx::util::Json& j) {
    WireMessageQueueUpdate u;
    u.sessionId = j.value("sessionId", std::string{});
    if (j.contains("items") && j["items"].is_array()) {
        for (const auto& item : j["items"]) {
            u.items.push_back(messageQueueItemFromJson(item));
        }
    }
    return u;
}

inline agentxx::util::Json makeClearMessageQueue(std::string_view sessionId) {
    return agentxx::util::Json{
        {"type",      MsgType::ClearMessageQueue},
        {"sessionId", sessionId                 },
    };
}

inline WireClearMessageQueue clearMessageQueueFromJson(const agentxx::util::Json& j) {
    WireClearMessageQueue q;
    q.sessionId = j.value("sessionId", std::string{});
    return q;
}

inline agentxx::util::Json
    makeRemoveQueueItem(std::string_view sessionId, std::string_view itemId) {
    return agentxx::util::Json{
        {"type",      MsgType::RemoveQueueItem},
        {"sessionId", sessionId               },
        {"itemId",    itemId                  },
    };
}

inline WireRemoveQueueItem removeQueueItemFromJson(const agentxx::util::Json& j) {
    WireRemoveQueueItem q;
    q.sessionId = j.value("sessionId", std::string{});
    q.itemId    = j.value("itemId", std::string{});
    return q;
}

inline agentxx::util::Json makeInterruptAndRunNext(std::string_view sessionId) {
    return agentxx::util::Json{
        {"type",      MsgType::InterruptAndRunNext},
        {"sessionId", sessionId                   },
    };
}

inline WireInterruptAndRunNext interruptAndRunNextFromJson(const agentxx::util::Json& j) {
    WireInterruptAndRunNext q;
    q.sessionId = j.value("sessionId", std::string{});
    return q;
}

// ---------------------------------------------------------------------------
// viewMessages 历史分页 (Client <-> Server)
// ---------------------------------------------------------------------------

/// 客户端请求历史分页 (Client -> Server): [max(0, beforeIndex-count), beforeIndex)
/// - beforeIndex == 0 表示"从末尾向前取 count 条"; count == 0 用服务端默认页大小
inline agentxx::util::Json
    makeGetViewMessages(std::string_view sessionId, uint64_t beforeIndex, uint32_t count) {
    agentxx::util::Json j = {
        {"type",        MsgType::GetViewMessages},
        {"sessionId",   sessionId               },
        {"beforeIndex", beforeIndex             },
        {"count",       count                   },
    };
    return j;
}

inline WireGetViewMessages getViewMessagesFromJson(const agentxx::util::Json& j) {
    WireGetViewMessages m;
    m.sessionId   = j.value("sessionId", std::string{});
    m.beforeIndex = j.value("beforeIndex", uint64_t{0});
    m.count       = j.value("count", uint32_t{0});
    return m;
}

/// 服务端历史分页响应 (Server -> Client): 绝对下标区间
/// [startIndex, startIndex + messages.size())
inline agentxx::util::Json makeViewMessagesPage(
    std::string_view                sessionId,
    uint64_t                        startIndex,
    uint64_t                        totalCount,
    const std::vector<ViewMessage>& messages
) {
    agentxx::util::Json j = {
        {"type",       MsgType::ViewMessagesPage},
        {"sessionId",  sessionId                },
        {"startIndex", startIndex               },
        {"totalCount", totalCount               },
    };
    agentxx::util::Json arr = agentxx::util::Json::array();
    for (const auto& vm : messages) {
        arr.push_back(vm.toJson());
    }
    j["messages"] = std::move(arr);
    return j;
}

inline std::optional<WireViewMessagesPage> viewMessagesPageFromJson(const agentxx::util::Json& j) {
    if (!j.is_object()) {
        return std::nullopt;
    }
    WireViewMessagesPage p;
    p.sessionId  = j.value("sessionId", std::string{});
    p.startIndex = j.value("startIndex", uint64_t{0});
    p.totalCount = j.value("totalCount", uint64_t{0});
    auto msgs    = j.value("messages", agentxx::util::Json::array());
    if (msgs.is_array()) {
        for (const auto& m : msgs) {
            p.messages.push_back(ViewMessage::fromJson(m));
        }
    }
    return p;
}

inline agentxx::util::Json makeLog(int level, std::string message) {
    return agentxx::util::Json{
        {"type",    MsgType::LogMsg   },
        {"level",   level             },
        {"message", std::move(message)},
    };
}

// ---------------------------------------------------------------------------
// 通用字段读取
// ---------------------------------------------------------------------------

inline std::string msgType(const agentxx::util::Json& j) {
    return j.is_object() ? j.value("type", std::string{}) : std::string{};
}

/// 高频路由: JsonView 零拷贝提取 type (§4.3, ws_io_transport 收包路径先命中再物化)
inline std::string msgTypeView(const agentxx::util::JsonView& jv) {
    if (!jv.is_object()) {
        return {};
    }
    auto v = jv["type"];
    if (!v.valid() || !v.is_string()) {
        return {};
    }
    try {
        return std::string(v.get_string_view());
    } catch (...) {
        return {};
    }
}

// ---------------------------------------------------------------------------
// 统一的 toJson / fromJson 声明与顶层序列化接口 (方案 1)
// ---------------------------------------------------------------------------

agentxx::util::Json toJson(const WireHello& msg);
WireHello           helloFromJson(const agentxx::util::Json& j);

agentxx::util::Json toJson(const WireHelloAck& msg);
WireHelloAck        helloAckFromJson(const agentxx::util::Json& j);

agentxx::util::Json toJson(const WireUserInput& msg);
WireUserInput       userInputFromJson(const agentxx::util::Json& j);

agentxx::util::Json toJson(const WireCancel& msg);
WireCancel          cancelFromJson(const agentxx::util::Json& j);

agentxx::util::Json toJson(const WireSelectModel& msg);
WireSelectModel     selectModelFromJson(const agentxx::util::Json& j);

agentxx::util::Json  toJson(const WireInterruptRequest& msg);
WireInterruptRequest interruptRequestFromJson(const agentxx::util::Json& j);

agentxx::util::Json   toJson(const WireInterruptResponse& msg);
WireInterruptResponse interruptResponseFromJson(const agentxx::util::Json& j);

agentxx::util::Json  toJson(const WireInterruptExpired& msg);
WireInterruptExpired interruptExpiredFromJson(const agentxx::util::Json& j);

agentxx::util::Json toJson(const WireDelta& msg);

agentxx::util::Json toJson(const WireSyncPayload& msg);

agentxx::util::Json toJson(const WireTurnResult& msg);
WireTurnResult      turnResultFromJson(const agentxx::util::Json& j);

agentxx::util::Json toJson(const WireContextStats& msg);
WireContextStats    contextStatsFromJson(const agentxx::util::Json& j);

agentxx::util::Json toJson(const WireError& msg);
WireError           errorFromJson(const agentxx::util::Json& j);

agentxx::util::Json toJson(const WireLog& msg);
WireLog             logFromJson(const agentxx::util::Json& j);

agentxx::util::Json toJson(const WireGetModel& msg);
WireGetModel        getModelFromJson(const agentxx::util::Json& j);

agentxx::util::Json toJson(const WireModelInfo& msg);
WireModelInfo       modelInfoFromJson(const agentxx::util::Json& j);

agentxx::util::Json        toJson(const WireGetAppendComponentInfo& msg);
WireGetAppendComponentInfo getAppendComponentInfoFromJson(const agentxx::util::Json& j);

agentxx::util::Json     toJson(const WireAppendComponentInfo& msg);
WireAppendComponentInfo appendComponentInfoMessageFromJson(const agentxx::util::Json& j);

agentxx::util::Json toJson(const WireGetContext& msg);
WireGetContext      getContextFromJson(const agentxx::util::Json& j);

agentxx::util::Json toJson(const WireCompactContext& msg);
WireCompactContext  compactContextFromJson(const agentxx::util::Json& j);

agentxx::util::Json toJson(const WireContextMessages& msg);
WireContextMessages contextMessagesFromJson(const agentxx::util::Json& j);

agentxx::util::Json toJson(const WireListSessions& msg);

agentxx::util::Json toJson(const WireSessionList& msg);

agentxx::util::Json toJson(const WireSwitchSession& msg);

agentxx::util::Json toJson(const WireSetPermission& msg);

agentxx::util::Json toJson(const WirePluginData& msg);

agentxx::util::Json toJson(const WirePluginDataUp& msg);

agentxx::util::Json toJson(const WireMessageQueueUpdate& msg);

agentxx::util::Json toJson(const WireClearMessageQueue& msg);

agentxx::util::Json toJson(const WireRemoveQueueItem& msg);

agentxx::util::Json toJson(const WireInterruptAndRunNext& msg);

agentxx::util::Json toJson(const WireGetViewMessages& msg);

agentxx::util::Json toJson(const WireViewMessagesPage& msg);

/// 统一序列化为 JSON 字符串
std::string serialize(const WireMessage& msg);

/// 统一从 JSON 字符串反序列化为 WireMessage
std::optional<WireMessage> deserialize(std::string_view jsonText);

} // namespace io
} // namespace agent
} // namespace agentxx
