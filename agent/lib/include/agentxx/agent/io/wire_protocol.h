#pragma once

#include "agentxx/agent/conversation_types.h"
#include "agentxx/agent/io/agent_io_transport.h"
#include "utilxx_base/json.h"
#include "utilxx_base/json_view.h"
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
    /// 客户端请求列举服务端目录 (用于跨设备附件选择)
    inline static constexpr std::string_view ListDir = "list_dir";

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
    /// 服务端列举目录响应
    inline static constexpr std::string_view ListDirResult = "list_dir_result";
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

inline utilxx_base::Json deltaToJson(const WireDelta& d) {
    utilxx_base::Json j = utilxx_base::Json::object();
    j["type"]           = std::string(deltaTypeToString(d.type));
    j["seq"]            = d.seq;
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
        utilxx_base::Json arr = utilxx_base::Json::array();
        for (const auto& a : d.attachments) {
            MediaAttachment meta = a;
            meta.dataUrl.clear();
            arr.push_back(meta.toJson());
        }
        j["attachments"] = std::move(arr);
    }
    if (d.think) {
        utilxx_base::Json th = utilxx_base::Json::object();
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

inline std::optional<WireDelta> deltaFromJson(const utilxx_base::Json& j) {
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

inline utilxx_base::Json messageQueueItemToJson(const MessageQueueItem& item) {
    utilxx_base::Json j = {
        {"id",          item.id         },
        {"text",        item.text       },
        {"createdAtMs", item.createdAtMs},
    };
    if (!item.model.empty()) {
        j["model"] = item.model;
    }
    if (!item.attachments.empty()) {
        utilxx_base::Json arr = utilxx_base::Json::array();
        for (const auto& att : item.attachments) {
            arr.push_back(att.toJson());
        }
        j["attachments"] = std::move(arr);
    }
    return j;
}

inline MessageQueueItem messageQueueItemFromJson(const utilxx_base::Json& j) {
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

inline utilxx_base::Json syncToJson(const WireSyncPayload& p) {
    utilxx_base::Json j = utilxx_base::Json::object();
    j["fromIndex"]      = p.fromIndex;
    j["tailHash"]       = p.tailHash;
    // 快照对应的服务端 delta 水位 (0 = 未提供): 客户端据此复位去重水位,
    // 避免服务端 seq 重新计数后客户端旧水位把新增量全部判为重复 (见结构体注释)
    j["deltaSeq"] = p.deltaSeq;
    // 历史分页元数据 (尾窗同步时 fromIndex>0 / totalMessages>0; 全量同步
    // 时 totalMessages == messages.size(), 字段冗余但便于客户端统一判断)
    j["totalMessages"]    = p.totalMessages;
    utilxx_base::Json arr = utilxx_base::Json::array();
    for (const auto& vm : p.messages) {
        arr.push_back(vm.toJson());
    }
    j["messages"] = std::move(arr);
    if (!p.messageQueue.empty()) {
        utilxx_base::Json qArr = utilxx_base::Json::array();
        for (const auto& item : p.messageQueue) {
            qArr.push_back(messageQueueItemToJson(item));
        }
        j["message_queue"] = std::move(qArr);
    }
    return j;
}

inline std::optional<WireSyncPayload> syncFromJson(const utilxx_base::Json& j) {
    if (!j.is_object()) {
        return std::nullopt;
    }
    WireSyncPayload p;
    p.fromIndex     = j.value("fromIndex", uint64_t{0});
    p.tailHash      = j.value("tailHash", std::string{});
    p.totalMessages = j.value("totalMessages", uint64_t{0});
    p.deltaSeq      = j.value("deltaSeq", uint64_t{0});
    auto msgs       = j.value("messages", utilxx_base::Json::array());
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

inline utilxx_base::Json makeHello(
    std::string_view sessionId,
    std::string_view token,
    uint64_t         lastSeq  = 0,
    std::string_view tailHash = "",
    std::string_view language = ""
) {
    utilxx_base::Json j = {
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

inline utilxx_base::Json makeUserInput(
    std::string_view                    sessionId,
    std::string_view                    text,
    std::string_view                    model       = "",
    const std::vector<MediaAttachment>& attachments = {}
) {
    utilxx_base::Json j = {
        {"type",      MsgType::UserInput},
        {"sessionId", sessionId         },
        {"text",      text              },
    };
    if (!model.empty()) {
        j["model"] = model;
    }
    if (!attachments.empty()) {
        utilxx_base::Json arr = utilxx_base::Json::array();
        for (const auto& att : attachments) {
            arr.push_back(att.toJson());
        }
        j["attachments"] = std::move(arr);
    }
    return j;
}

inline utilxx_base::Json makeInterruptResponse(int64_t id, const utilxx_base::Json& result) {
    return utilxx_base::Json{
        {"type",   MsgType::InterruptResponse},
        {"id",     id                        },
        {"result", result                    },
    };
}

inline utilxx_base::Json makeCancel(std::string_view sessionId) {
    return utilxx_base::Json{
        {"type",      MsgType::Cancel},
        {"sessionId", sessionId      },
    };
}

inline utilxx_base::Json makeSelectModel(std::string_view sessionId, std::string_view model) {
    return utilxx_base::Json{
        {"type",      MsgType::SelectModel},
        {"sessionId", sessionId           },
        {"model",     model               },
    };
}

inline utilxx_base::Json makePing(int64_t t) {
    return utilxx_base::Json{
        {"type", MsgType::Ping},
        {"t",    t            },
    };
}

// ---------------------------------------------------------------------------
// 消息构造 (Server -> Client)
// ---------------------------------------------------------------------------

inline utilxx_base::Json makeHelloAck(
    bool                                         ok,
    std::string_view                             sessionId,
    std::string_view                             tailHash,
    const std::vector<std::string>&              models,
    const std::vector<WireHelloAck::PluginInfo>& plugins  = {},
    std::string_view                             deviceId = "",
    std::string_view                             workDir  = ""
) {
    utilxx_base::Json j = {
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
    if (!deviceId.empty()) {
        j["deviceId"] = deviceId;
    }
    if (!workDir.empty()) {
        j["workDir"] = workDir;
    }
    // 服务端已加载插件结构化列表 (名字+版本+声明接口, client 插件据此判断
    // 对端可用性与能力); 空时不携带 (缺字段按"服务端未提供"处理)
    if (!plugins.empty()) {
        auto arr = utilxx_base::Json::array();
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

inline utilxx_base::Json makeDeltaMsg(const WireDelta& d) {
    utilxx_base::Json j = deltaToJson(d);
    // 复用 deltaToJson 的字段, 但信封 type 固定为 "delta"
    j["type"] = MsgType::DeltaMsg;
    j["kind"] = std::string(deltaTypeToString(d.type));
    return j;
}

/// 从 "delta" 信封还原 WireDelta (type 字段取自 "kind")
inline std::optional<WireDelta> deltaMsgFromJson(const utilxx_base::Json& j) {
    if (!j.is_object()) {
        return std::nullopt;
    }
    auto patched    = j;
    patched["type"] = j.value("kind", std::string{});
    return deltaFromJson(patched);
}

inline utilxx_base::Json makeSyncMsg(const WireSyncPayload& p, uint64_t deltaSeq = 0) {
    utilxx_base::Json j = syncToJson(p);
    j["type"]           = MsgType::SyncMsg;
    if (deltaSeq > 0) {
        j["deltaSeq"] = deltaSeq;
    }
    return j;
}

inline std::optional<WireSyncPayload> syncMsgFromJson(const utilxx_base::Json& j) {
    return syncFromJson(j);
}

inline utilxx_base::Json makeInterruptRequest(
    int64_t          id,
    std::string_view sessionId,
    std::string_view node,
    std::string_view value,
    std::string_view argJson
) {
    return utilxx_base::Json{
        {"type",      MsgType::InterruptRequest},
        {"id",        id                       },
        {"sessionId", sessionId                },
        {"node",      node                     },
        {"value",     value                    },
        {"argJson",   argJson                  },
    };
}

/// 服务端通知中断已过期 (超时/取消): 对应 WireInterruptRequest 的 id
inline utilxx_base::Json makeInterruptExpired(int64_t id, std::string_view sessionId) {
    return utilxx_base::Json{
        {"type",      MsgType::InterruptExpired},
        {"id",        id                       },
        {"sessionId", sessionId                },
    };
}

inline utilxx_base::Json makeTurnResult(
    std::string_view sessionId,
    bool             hasError,
    std::string_view errorMessage,
    bool             interrupted,
    int64_t          startTimeMs = 0,
    int64_t          durationMs  = 0
) {
    utilxx_base::Json j = {
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

inline utilxx_base::Json
    makeContextStats(uint64_t contextTokens, uint64_t maxContextTokens, double tps = 0.0) {
    utilxx_base::Json j = {
        {"type",             MsgType::ContextStats},
        {"contextTokens",    contextTokens        },
        {"maxContextTokens", maxContextTokens     },
    };
    if (tps > 0.0) {
        j["tps"] = tps;
    }
    return j;
}

inline utilxx_base::Json makeError(int code, std::string_view message) {
    return utilxx_base::Json{
        {"type",    MsgType::ErrorMsg},
        {"code",    code             },
        {"message", message          },
    };
}

inline utilxx_base::Json makeGetModel(std::string_view sessionId) {
    return utilxx_base::Json{
        {"type",      MsgType::GetModel},
        {"sessionId", sessionId        },
    };
}

inline utilxx_base::Json makeModelInfo(
    std::string_view                        currentModel,
    const std::vector<std::string>&         models,
    const std::vector<ModelCapabilityInfo>& capabilities = {}
) {
    utilxx_base::Json j = {
        {"type",         MsgType::ModelInfo},
        {"currentModel", currentModel      },
    };
    if (!models.empty()) {
        j["models"] = models;
    }
    if (!capabilities.empty()) {
        utilxx_base::Json caps = utilxx_base::Json::array();
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

inline utilxx_base::Json appendComponentNotificationToJson(const AppendComponentNotification& n) {
    return utilxx_base::Json{
        {"type",         static_cast<int>(n.type)},
        {"name",         n.name                  },
        {"success",      n.success               },
        {"errorMessage", n.errorMessage          },
    };
}

inline AppendComponentNotification appendComponentNotificationFromJson(const utilxx_base::Json& j) {
    AppendComponentNotification n;
    n.type         = static_cast<AppendComponentNotification::Type>(j.value("type", 0));
    n.name         = j.value("name", std::string{});
    n.success      = j.value("success", true);
    n.errorMessage = j.value("errorMessage", std::string{});
    return n;
}

inline utilxx_base::Json makeGetAppendComponentInfo(std::string_view sessionId) {
    return utilxx_base::Json{
        {"type",      MsgType::GetAppendComponentInfo},
        {"sessionId", sessionId                      },
    };
}

inline utilxx_base::Json
    makeAppendComponentInfo(const std::vector<AppendComponentNotification>& notifications) {
    utilxx_base::Json j = {
        {"type", MsgType::AppendComponentInfo}
    };
    utilxx_base::Json arr = utilxx_base::Json::array();
    for (const auto& n : notifications) {
        arr.push_back(appendComponentNotificationToJson(n));
    }
    j["notifications"] = std::move(arr);
    return j;
}

inline std::vector<AppendComponentNotification>
    appendComponentInfoFromJson(const utilxx_base::Json& j) {
    std::vector<AppendComponentNotification> out;
    auto arr = j.value("notifications", utilxx_base::Json::array());
    if (arr.is_array()) {
        for (const auto& item : arr) {
            out.push_back(appendComponentNotificationFromJson(item));
        }
    }
    return out;
}

inline utilxx_base::Json makePong(int64_t t) {
    return utilxx_base::Json{
        {"type", MsgType::Pong},
        {"t",    t            },
    };
}

inline utilxx_base::Json makeGetContext(std::string_view sessionId) {
    return utilxx_base::Json{
        {"type",      MsgType::GetContext},
        {"sessionId", sessionId          },
    };
}

inline utilxx_base::Json makeCompactContext(std::string_view sessionId) {
    return utilxx_base::Json{
        {"type",      MsgType::CompactContext},
        {"sessionId", sessionId              },
    };
}

inline utilxx_base::Json makeContextMessages(const utilxx_base::Json& messages) {
    return utilxx_base::Json{
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
inline utilxx_base::Json
    makeListSessions(int64_t beforeMs = 0, std::string_view beforeId = "", uint32_t limit = 0) {
    utilxx_base::Json j = {
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

inline WireListSessions listSessionsFromJson(const utilxx_base::Json& j) {
    WireListSessions m;
    m.beforeMs = j.value("beforeMs", int64_t{0});
    m.beforeId = j.value("beforeId", std::string{});
    m.limit    = j.value("limit", uint32_t{0});
    return m;
}

inline utilxx_base::Json sessionInfoToJson(const SessionInfo& s) {
    utilxx_base::Json j = {
        {"sessionId",    s.sessionId   },
        {"lastActiveMs", s.lastActiveMs},
    };
    if (!s.title.empty()) {
        j["title"] = s.title;
    }
    return j;
}

inline SessionInfo sessionInfoFromJson(const utilxx_base::Json& j) {
    SessionInfo s;
    s.sessionId    = j.value("sessionId", std::string{});
    s.title        = j.value("title", std::string{});
    s.lastActiveMs = j.value("lastActiveMs", int64_t{0});
    return s;
}

inline utilxx_base::Json makeSessionList(
    const std::vector<SessionInfo>& sessions,
    uint64_t                        totalCount = 0,
    bool                            hasMore    = false
) {
    utilxx_base::Json j = {
        {"type", MsgType::SessionList}
    };
    utilxx_base::Json arr = utilxx_base::Json::array();
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

inline WireSessionList sessionListFromJson(const utilxx_base::Json& j) {
    WireSessionList out;
    auto            arr = j.value("sessions", utilxx_base::Json::array());
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
inline utilxx_base::Json makeSwitchSession(std::string_view sessionId) {
    return utilxx_base::Json{
        {"type",      MsgType::SwitchSession},
        {"sessionId", sessionId             },
    };
}

inline WireSwitchSession switchSessionFromJson(const utilxx_base::Json& j) {
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
inline utilxx_base::Json makePluginData(const WirePluginData& p) {
    utilxx_base::Json j = {
        {"type",   MsgType::PluginData},
        {"plugin", p.plugin           },
        {"event",  p.event            },
        {"data",   p.data             },
    };
    return j;
}

inline WirePluginData pluginDataFromJson(const utilxx_base::Json& j) {
    WirePluginData p;
    p.plugin = j.value("plugin", std::string{});
    p.event  = j.value("event", std::string{});
    p.data   = j.value("data", std::string{});
    return p;
}

/// client 插件事件上行 (Client -> Server): WirePluginDataUp
/// - 载荷 JSON 原样透传 (语义由插件定义); 服务端发布到事件总线
///   topic `client.{插件名}.{事件名}`
inline utilxx_base::Json makePluginDataUp(const WirePluginDataUp& p) {
    utilxx_base::Json j = {
        {"type",   MsgType::PluginDataUp},
        {"plugin", p.plugin             },
        {"event",  p.event              },
        {"data",   p.data               },
    };
    return j;
}

inline WirePluginDataUp pluginDataUpFromJson(const utilxx_base::Json& j) {
    WirePluginDataUp p;
    p.plugin = j.value("plugin", std::string{});
    p.event  = j.value("event", std::string{});
    p.data   = j.value("data", std::string{});
    return p;
}

// ---------------------------------------------------------------------------
// 消息队列相关 (Client <-> Server)
// ---------------------------------------------------------------------------

inline utilxx_base::Json
    makeMessageQueueUpdate(std::string_view sessionId, const std::vector<MessageQueueItem>& items) {
    utilxx_base::Json j = {
        {"type",      MsgType::MessageQueueUpdate},
        {"sessionId", sessionId                  },
    };
    utilxx_base::Json arr = utilxx_base::Json::array();
    for (const auto& item : items) {
        arr.push_back(messageQueueItemToJson(item));
    }
    j["items"] = std::move(arr);
    return j;
}

inline WireMessageQueueUpdate messageQueueUpdateFromJson(const utilxx_base::Json& j) {
    WireMessageQueueUpdate u;
    u.sessionId = j.value("sessionId", std::string{});
    if (j.contains("items") && j["items"].is_array()) {
        for (const auto& item : j["items"]) {
            u.items.push_back(messageQueueItemFromJson(item));
        }
    }
    return u;
}

inline utilxx_base::Json makeClearMessageQueue(std::string_view sessionId) {
    return utilxx_base::Json{
        {"type",      MsgType::ClearMessageQueue},
        {"sessionId", sessionId                 },
    };
}

inline WireClearMessageQueue clearMessageQueueFromJson(const utilxx_base::Json& j) {
    WireClearMessageQueue q;
    q.sessionId = j.value("sessionId", std::string{});
    return q;
}

inline utilxx_base::Json makeRemoveQueueItem(std::string_view sessionId, std::string_view itemId) {
    return utilxx_base::Json{
        {"type",      MsgType::RemoveQueueItem},
        {"sessionId", sessionId               },
        {"itemId",    itemId                  },
    };
}

inline WireRemoveQueueItem removeQueueItemFromJson(const utilxx_base::Json& j) {
    WireRemoveQueueItem q;
    q.sessionId = j.value("sessionId", std::string{});
    q.itemId    = j.value("itemId", std::string{});
    return q;
}

inline utilxx_base::Json makeInterruptAndRunNext(std::string_view sessionId) {
    return utilxx_base::Json{
        {"type",      MsgType::InterruptAndRunNext},
        {"sessionId", sessionId                   },
    };
}

inline WireInterruptAndRunNext interruptAndRunNextFromJson(const utilxx_base::Json& j) {
    WireInterruptAndRunNext q;
    q.sessionId = j.value("sessionId", std::string{});
    return q;
}

// ---------------------------------------------------------------------------
// viewMessages 历史分页 (Client <-> Server)
// ---------------------------------------------------------------------------

/// 客户端请求历史分页 (Client -> Server): [max(0, beforeIndex-count), beforeIndex)
/// - beforeIndex == 0 表示"从末尾向前取 count 条"; count == 0 用服务端默认页大小
inline utilxx_base::Json
    makeGetViewMessages(std::string_view sessionId, uint64_t beforeIndex, uint32_t count) {
    utilxx_base::Json j = {
        {"type",        MsgType::GetViewMessages},
        {"sessionId",   sessionId               },
        {"beforeIndex", beforeIndex             },
        {"count",       count                   },
    };
    return j;
}

inline WireGetViewMessages getViewMessagesFromJson(const utilxx_base::Json& j) {
    WireGetViewMessages m;
    m.sessionId   = j.value("sessionId", std::string{});
    m.beforeIndex = j.value("beforeIndex", uint64_t{0});
    m.count       = j.value("count", uint32_t{0});
    return m;
}

/// 服务端历史分页响应 (Server -> Client): 绝对下标区间
/// [startIndex, startIndex + messages.size())
inline utilxx_base::Json makeViewMessagesPage(
    std::string_view                sessionId,
    uint64_t                        startIndex,
    uint64_t                        totalCount,
    const std::vector<ViewMessage>& messages
) {
    utilxx_base::Json j = {
        {"type",       MsgType::ViewMessagesPage},
        {"sessionId",  sessionId                },
        {"startIndex", startIndex               },
        {"totalCount", totalCount               },
    };
    utilxx_base::Json arr = utilxx_base::Json::array();
    for (const auto& vm : messages) {
        arr.push_back(vm.toJson());
    }
    j["messages"] = std::move(arr);
    return j;
}

inline std::optional<WireViewMessagesPage> viewMessagesPageFromJson(const utilxx_base::Json& j) {
    if (!j.is_object()) {
        return std::nullopt;
    }
    WireViewMessagesPage p;
    p.sessionId  = j.value("sessionId", std::string{});
    p.startIndex = j.value("startIndex", uint64_t{0});
    p.totalCount = j.value("totalCount", uint64_t{0});
    auto msgs    = j.value("messages", utilxx_base::Json::array());
    if (msgs.is_array()) {
        for (const auto& m : msgs) {
            p.messages.push_back(ViewMessage::fromJson(m));
        }
    }
    return p;
}

inline utilxx_base::Json makeLog(int level, std::string message) {
    return utilxx_base::Json{
        {"type",    MsgType::LogMsg   },
        {"level",   level             },
        {"message", std::move(message)},
    };
}

// ---------------------------------------------------------------------------
// 通用字段读取
// ---------------------------------------------------------------------------

inline std::string msgType(const utilxx_base::Json& j) {
    return j.is_object() ? j.value("type", std::string{}) : std::string{};
}

// ---------------------------------------------------------------------------
// 服务端目录列举 (跨设备附件选择)
// ---------------------------------------------------------------------------

inline utilxx_base::Json wireDirEntryToJson(const WireDirEntry& e) {
    return utilxx_base::Json{
        {"name",      e.name                       },
        {"fullPath",  e.fullPath                   },
        {"isDir",     e.isDir                      },
        {"supported", e.supported                  },
        {"sizeBytes", e.sizeBytes                  },
        {"mediaType", static_cast<int>(e.mediaType)},
    };
}

inline WireDirEntry wireDirEntryFromJson(const utilxx_base::Json& j) {
    WireDirEntry e;
    e.name      = j.value("name", std::string{});
    e.fullPath  = j.value("fullPath", std::string{});
    e.isDir     = j.value("isDir", false);
    e.supported = j.value("supported", true);
    e.sizeBytes = j.value("sizeBytes", uint64_t{0});
    e.mediaType = static_cast<MediaType>(j.value("mediaType", 0));
    return e;
}

inline utilxx_base::Json makeListDir(
    uint64_t                        reqId,
    std::string_view                path,
    const std::vector<std::string>& allowedExtensions = {}
) {
    utilxx_base::Json j = {
        {"type",  MsgType::ListDir},
        {"reqId", reqId           },
        {"path",  path            },
    };
    if (!allowedExtensions.empty()) {
        utilxx_base::Json arr = utilxx_base::Json::array();
        for (const auto& ext : allowedExtensions) {
            arr.push_back(ext);
        }
        j["allowedExtensions"] = std::move(arr);
    }
    return j;
}

inline WireListDir listDirFromJson(const utilxx_base::Json& j) {
    WireListDir m;
    m.reqId = j.value("reqId", uint64_t{0});
    m.path  = j.value("path", std::string{});
    if (j.contains("allowedExtensions") && j["allowedExtensions"].is_array()) {
        for (const auto& ext : j["allowedExtensions"]) {
            if (ext.is_string()) {
                m.allowedExtensions.push_back(ext.get<std::string>());
            }
        }
    }
    return m;
}

inline utilxx_base::Json makeListDirResult(
    uint64_t                         reqId,
    bool                             ok,
    std::string_view                 currentDir,
    std::string_view                 parentDir,
    const std::vector<WireDirEntry>& entries,
    std::string_view                 error = ""
) {
    utilxx_base::Json j = {
        {"type",       MsgType::ListDirResult},
        {"reqId",      reqId                 },
        {"ok",         ok                    },
        {"currentDir", currentDir            },
        {"parentDir",  parentDir             },
    };
    if (!error.empty()) {
        j["error"] = error;
    }
    utilxx_base::Json arr = utilxx_base::Json::array();
    for (const auto& e : entries) {
        arr.push_back(wireDirEntryToJson(e));
    }
    j["entries"] = std::move(arr);
    return j;
}

inline WireListDirResult listDirResultFromJson(const utilxx_base::Json& j) {
    WireListDirResult r;
    r.reqId      = j.value("reqId", uint64_t{0});
    r.ok         = j.value("ok", false);
    r.currentDir = j.value("currentDir", std::string{});
    r.parentDir  = j.value("parentDir", std::string{});
    r.error      = j.value("error", std::string{});
    if (j.contains("entries") && j["entries"].is_array()) {
        for (const auto& item : j["entries"]) {
            r.entries.push_back(wireDirEntryFromJson(item));
        }
    }
    return r;
}

/// 高频路由: JsonView 零拷贝提取 type (§4.3, ws_io_transport 收包路径先命中再物化)
inline std::string msgTypeView(const utilxx_base::JsonView& jv) {
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

utilxx_base::Json toJson(const WireHello& msg);
WireHello         helloFromJson(const utilxx_base::Json& j);

utilxx_base::Json toJson(const WireHelloAck& msg);
WireHelloAck      helloAckFromJson(const utilxx_base::Json& j);

utilxx_base::Json toJson(const WireUserInput& msg);
WireUserInput     userInputFromJson(const utilxx_base::Json& j);

utilxx_base::Json toJson(const WireCancel& msg);
WireCancel        cancelFromJson(const utilxx_base::Json& j);

utilxx_base::Json toJson(const WireSelectModel& msg);
WireSelectModel   selectModelFromJson(const utilxx_base::Json& j);

utilxx_base::Json    toJson(const WireInterruptRequest& msg);
WireInterruptRequest interruptRequestFromJson(const utilxx_base::Json& j);

utilxx_base::Json     toJson(const WireInterruptResponse& msg);
WireInterruptResponse interruptResponseFromJson(const utilxx_base::Json& j);

utilxx_base::Json    toJson(const WireInterruptExpired& msg);
WireInterruptExpired interruptExpiredFromJson(const utilxx_base::Json& j);

utilxx_base::Json toJson(const WireDelta& msg);

utilxx_base::Json toJson(const WireSyncPayload& msg);

utilxx_base::Json toJson(const WireTurnResult& msg);
WireTurnResult    turnResultFromJson(const utilxx_base::Json& j);

utilxx_base::Json toJson(const WireContextStats& msg);
WireContextStats  contextStatsFromJson(const utilxx_base::Json& j);

utilxx_base::Json toJson(const WireError& msg);
WireError         errorFromJson(const utilxx_base::Json& j);

utilxx_base::Json toJson(const WireLog& msg);
WireLog           logFromJson(const utilxx_base::Json& j);

utilxx_base::Json toJson(const WireGetModel& msg);
WireGetModel      getModelFromJson(const utilxx_base::Json& j);

utilxx_base::Json toJson(const WireModelInfo& msg);
WireModelInfo     modelInfoFromJson(const utilxx_base::Json& j);

utilxx_base::Json          toJson(const WireGetAppendComponentInfo& msg);
WireGetAppendComponentInfo getAppendComponentInfoFromJson(const utilxx_base::Json& j);

utilxx_base::Json       toJson(const WireAppendComponentInfo& msg);
WireAppendComponentInfo appendComponentInfoMessageFromJson(const utilxx_base::Json& j);

utilxx_base::Json toJson(const WireGetContext& msg);
WireGetContext    getContextFromJson(const utilxx_base::Json& j);

utilxx_base::Json  toJson(const WireCompactContext& msg);
WireCompactContext compactContextFromJson(const utilxx_base::Json& j);

utilxx_base::Json   toJson(const WireContextMessages& msg);
WireContextMessages contextMessagesFromJson(const utilxx_base::Json& j);

utilxx_base::Json toJson(const WireListSessions& msg);

utilxx_base::Json toJson(const WireSessionList& msg);

utilxx_base::Json toJson(const WireSwitchSession& msg);

utilxx_base::Json toJson(const WirePluginData& msg);

utilxx_base::Json toJson(const WirePluginDataUp& msg);

utilxx_base::Json toJson(const WireMessageQueueUpdate& msg);

utilxx_base::Json toJson(const WireClearMessageQueue& msg);

utilxx_base::Json toJson(const WireRemoveQueueItem& msg);

utilxx_base::Json toJson(const WireInterruptAndRunNext& msg);

utilxx_base::Json toJson(const WireGetViewMessages& msg);

utilxx_base::Json toJson(const WireViewMessagesPage& msg);

utilxx_base::Json toJson(const WireListDir& msg);

utilxx_base::Json toJson(const WireListDirResult& msg);

/// 统一序列化为 JSON 字符串
std::string serialize(const WireMessage& msg);

/// 统一从 JSON 字符串反序列化为 WireMessage
std::optional<WireMessage> deserialize(std::string_view jsonText);

} // namespace io
} // namespace agent
} // namespace agentxx
