#include "agentxx/agent/io/wire_protocol.h"
#include <unordered_map>

namespace agentxx {
namespace agent {
namespace io {

// ---------------------------------------------------------------------------
// 对称 toJson 实现
// ---------------------------------------------------------------------------

agentxx::util::Json toJson(const WireHello& msg) {
    return makeHello(msg.sessionId, msg.token, msg.lastSeq, msg.tailHash, msg.language);
}

agentxx::util::Json toJson(const WireHelloAck& msg) {
    return makeHelloAck(msg.ok, msg.sessionId, msg.tailHash, msg.models, msg.plugins);
}

agentxx::util::Json toJson(const WireUserInput& msg) {
    return makeUserInput(msg.sessionId, msg.text, msg.model, msg.attachments);
}

agentxx::util::Json toJson(const WireCancel& msg) {
    return makeCancel(msg.sessionId);
}

agentxx::util::Json toJson(const WireSelectModel& msg) {
    return makeSelectModel(msg.sessionId, msg.model);
}

agentxx::util::Json toJson(const WireInterruptRequest& msg) {
    return makeInterruptRequest(msg.id, msg.sessionId, msg.node, msg.value, msg.argJson);
}

agentxx::util::Json toJson(const WireInterruptResponse& msg) {
    return makeInterruptResponse(msg.id, msg.result);
}

agentxx::util::Json toJson(const WireInterruptExpired& msg) {
    return makeInterruptExpired(msg.id, msg.sessionId);
}

agentxx::util::Json toJson(const WireDelta& msg) {
    return makeDeltaMsg(msg);
}

agentxx::util::Json toJson(const WireSyncPayload& msg) {
    return makeSyncMsg(msg);
}

agentxx::util::Json toJson(const WireTurnResult& msg) {
    return makeTurnResult(
        msg.sessionId,
        msg.hasError,
        msg.errorMessage,
        msg.interrupted,
        msg.startTimeMs,
        msg.durationMs
    );
}

agentxx::util::Json toJson(const WireContextStats& msg) {
    return makeContextStats(msg.contextTokens, msg.maxContextTokens, msg.tps);
}

agentxx::util::Json toJson(const WireError& msg) {
    return makeError(msg.code, msg.message);
}

agentxx::util::Json toJson(const WireLog& msg) {
    return makeLog(msg.level, msg.message);
}

agentxx::util::Json toJson(const WireGetModel& msg) {
    return makeGetModel(msg.sessionId);
}

agentxx::util::Json toJson(const WireModelInfo& msg) {
    return makeModelInfo(msg.currentModel, msg.models, msg.capabilities);
}

agentxx::util::Json toJson(const WireGetAppendComponentInfo& msg) {
    return makeGetAppendComponentInfo(msg.sessionId);
}

agentxx::util::Json toJson(const WireAppendComponentInfo& msg) {
    return makeAppendComponentInfo(msg.notifications);
}

agentxx::util::Json toJson(const WireGetContext& msg) {
    return makeGetContext(msg.sessionId);
}

agentxx::util::Json toJson(const WireCompactContext& msg) {
    return makeCompactContext(msg.sessionId);
}

agentxx::util::Json toJson(const WireContextMessages& msg) {
    return makeContextMessages(msg.messages);
}

agentxx::util::Json toJson(const WireListSessions& msg) {
    return makeListSessions(msg.beforeMs, msg.beforeId, msg.limit);
}

agentxx::util::Json toJson(const WireSessionList& msg) {
    return makeSessionList(msg.sessions, msg.totalCount, msg.hasMore);
}

agentxx::util::Json toJson(const WireSwitchSession& msg) {
    return makeSwitchSession(msg.sessionId);
}

agentxx::util::Json toJson(const WireSetPermission& msg) {
    return makeSetPermission(msg.sessionId, msg.path, msg.allow, msg.index);
}

agentxx::util::Json toJson(const WirePluginData& msg) {
    return makePluginData(msg);
}

agentxx::util::Json toJson(const WirePluginDataUp& msg) {
    return makePluginDataUp(msg);
}

agentxx::util::Json toJson(const WireMessageQueueUpdate& msg) {
    return makeMessageQueueUpdate(msg.sessionId, msg.items);
}

agentxx::util::Json toJson(const WireClearMessageQueue& msg) {
    return makeClearMessageQueue(msg.sessionId);
}

agentxx::util::Json toJson(const WireRemoveQueueItem& msg) {
    return makeRemoveQueueItem(msg.sessionId, msg.itemId);
}

agentxx::util::Json toJson(const WireInterruptAndRunNext& msg) {
    return makeInterruptAndRunNext(msg.sessionId);
}

agentxx::util::Json toJson(const WireGetViewMessages& msg) {
    return makeGetViewMessages(msg.sessionId, msg.beforeIndex, msg.count);
}

agentxx::util::Json toJson(const WireViewMessagesPage& msg) {
    return makeViewMessagesPage(msg.sessionId, msg.startIndex, msg.totalCount, msg.messages);
}

// ---------------------------------------------------------------------------
// 对称 fromJson 实现
// ---------------------------------------------------------------------------

WireHello helloFromJson(const agentxx::util::Json& j) {
    WireHello hello;
    hello.sessionId = j.value("sessionId", std::string{});
    hello.token     = j.value("token", std::string{});
    hello.lastSeq   = j.value("lastSeq", uint64_t{0});
    hello.tailHash  = j.value("tailHash", std::string{});
    hello.language  = j.value("language", std::string{});
    return hello;
}

WireHelloAck helloAckFromJson(const agentxx::util::Json& j) {
    WireHelloAck ack;
    ack.ok        = j.value("ok", false);
    ack.sessionId = j.value("sessionId", std::string{});
    ack.tailHash  = j.value("tailHash", std::string{});
    if (j.contains("models") && j["models"].is_array()) {
        for (const auto& m : j["models"]) {
            if (m.is_string()) {
                ack.models.push_back(m.get<std::string>());
            }
        }
    }
    if (j.contains("plugins") && j["plugins"].is_array()) {
        for (const auto& p : j["plugins"]) {
            if (!p.is_object() || !p.contains("name") || !p["name"].is_string()) {
                continue;
            }
            WireHelloAck::PluginInfo info{.name = p["name"].get<std::string>()};
            if (p.contains("version") && p["version"].is_string()) {
                info.version = p["version"].get<std::string>();
            }
            if (p.contains("interfaces") && p["interfaces"].is_array()) {
                for (const auto& n : p["interfaces"]) {
                    if (n.is_string()) {
                        info.interfaces.push_back(n.get<std::string>());
                    }
                }
            }
            ack.plugins.push_back(std::move(info));
        }
    }
    return ack;
}

WireUserInput userInputFromJson(const agentxx::util::Json& j) {
    WireUserInput input;
    input.sessionId = j.value("sessionId", std::string{});
    input.text      = j.value("text", std::string{});
    input.model     = j.value("model", std::string{});
    if (j.contains("attachments") && j["attachments"].is_array()) {
        for (const auto& item : j["attachments"]) {
            input.attachments.push_back(MediaAttachment::fromJson(item));
        }
    }
    return input;
}

WireCancel cancelFromJson(const agentxx::util::Json& j) {
    WireCancel cancel;
    cancel.sessionId = j.value("sessionId", std::string{});
    return cancel;
}

WireSelectModel selectModelFromJson(const agentxx::util::Json& j) {
    WireSelectModel sm;
    sm.sessionId = j.value("sessionId", std::string{});
    sm.model     = j.value("model", std::string{});
    return sm;
}

WireInterruptRequest interruptRequestFromJson(const agentxx::util::Json& j) {
    WireInterruptRequest req;
    req.id        = j.value("id", int64_t{0});
    req.sessionId = j.value("sessionId", std::string{});
    req.node      = j.value("node", std::string{});
    req.value     = j.value("value", std::string{});
    req.argJson   = j.value("argJson", std::string{});
    return req;
}

WireInterruptResponse interruptResponseFromJson(const agentxx::util::Json& j) {
    WireInterruptResponse resp;
    resp.id     = j.value("id", int64_t{0});
    resp.result = j.value("result", agentxx::util::Json{});
    return resp;
}

WireInterruptExpired interruptExpiredFromJson(const agentxx::util::Json& j) {
    WireInterruptExpired expired;
    expired.id        = j.value("id", int64_t{0});
    expired.sessionId = j.value("sessionId", std::string{});
    return expired;
}

WireTurnResult turnResultFromJson(const agentxx::util::Json& j) {
    WireTurnResult res;
    res.sessionId    = j.value("sessionId", std::string{});
    res.hasError     = j.value("hasError", false);
    res.errorMessage = j.value("errorMessage", std::string{});
    res.interrupted  = j.value("interrupted", false);
    res.startTimeMs  = j.value("startTimeMs", int64_t{0});
    res.durationMs   = j.value("durationMs", int64_t{0});
    return res;
}

WireContextStats contextStatsFromJson(const agentxx::util::Json& j) {
    WireContextStats stats;
    stats.contextTokens    = j.value("contextTokens", uint64_t{0});
    stats.maxContextTokens = j.value("maxContextTokens", uint64_t{0});
    stats.tps              = j.value("tps", 0.0);
    return stats;
}

WireError errorFromJson(const agentxx::util::Json& j) {
    WireError err;
    err.code    = j.value("code", 0);
    err.message = j.value("message", std::string{});
    return err;
}

WireLog logFromJson(const agentxx::util::Json& j) {
    WireLog log;
    log.level   = j.value("level", 0);
    log.message = j.value("message", std::string{});
    return log;
}

WireGetModel getModelFromJson(const agentxx::util::Json& j) {
    WireGetModel req;
    req.sessionId = j.value("sessionId", std::string{});
    return req;
}

WireModelInfo modelInfoFromJson(const agentxx::util::Json& j) {
    WireModelInfo info;
    info.currentModel = j.value("currentModel", std::string{});
    if (j.contains("models") && j["models"].is_array()) {
        for (const auto& m : j["models"]) {
            if (m.is_string()) {
                info.models.push_back(m.get<std::string>());
            }
        }
    }
    if (j.contains("capabilities") && j["capabilities"].is_array()) {
        for (const auto& c : j["capabilities"]) {
            ModelCapabilityInfo cap;
            cap.name       = c.value("name", std::string{});
            cap.imageInput = c.value("image_input", false);
            cap.audioInput = c.value("audio_input", false);
            cap.videoInput = c.value("video_input", false);
            info.capabilities.push_back(std::move(cap));
        }
    }
    return info;
}

WireGetAppendComponentInfo getAppendComponentInfoFromJson(const agentxx::util::Json& j) {
    WireGetAppendComponentInfo req;
    req.sessionId = j.value("sessionId", std::string{});
    return req;
}

WireAppendComponentInfo appendComponentInfoMessageFromJson(const agentxx::util::Json& j) {
    WireAppendComponentInfo info;
    info.notifications = appendComponentInfoFromJson(j);
    return info;
}

WireGetContext getContextFromJson(const agentxx::util::Json& j) {
    WireGetContext req;
    req.sessionId = j.value("sessionId", std::string{});
    return req;
}

WireCompactContext compactContextFromJson(const agentxx::util::Json& j) {
    WireCompactContext req;
    req.sessionId = j.value("sessionId", std::string{});
    return req;
}

WireContextMessages contextMessagesFromJson(const agentxx::util::Json& j) {
    WireContextMessages resp;
    resp.messages = j.value("messages", agentxx::util::Json::array());
    return resp;
}

// ---------------------------------------------------------------------------
// 顶层序列化与反序列化
// ---------------------------------------------------------------------------

std::string serialize(const WireMessage& msg) {
    return std::visit(
        [](const auto& m) -> std::string {
            return toJson(m).dump();
        },
        msg
    );
}

using DeserializerFn = std::optional<WireMessage> (*)(const agentxx::util::Json&);

static const std::unordered_map<std::string_view, DeserializerFn>& getDeserializerMap() {
    static const std::unordered_map<std::string_view, DeserializerFn> s_map = {
        {MsgType::Hello,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return helloFromJson(j);
         }},
        {MsgType::HelloAck,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return helloAckFromJson(j);
         }},
        {MsgType::UserInput,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return userInputFromJson(j);
         }},
        {MsgType::Cancel,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return cancelFromJson(j);
         }},
        {MsgType::SelectModel,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return selectModelFromJson(j);
         }},
        {MsgType::InterruptRequest,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return interruptRequestFromJson(j);
         }},
        {MsgType::InterruptResponse,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return interruptResponseFromJson(j);
         }},
        {MsgType::InterruptExpired,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return interruptExpiredFromJson(j);
         }},
        {MsgType::DeltaMsg,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             auto d = deltaMsgFromJson(j);
             return d.has_value() ? std::optional<WireMessage>{std::move(d.value())} : std::nullopt;
         }},
        {MsgType::SyncMsg,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             auto s = syncMsgFromJson(j);
             return s.has_value() ? std::optional<WireMessage>{std::move(s.value())} : std::nullopt;
         }},
        {MsgType::TurnResult,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return turnResultFromJson(j);
         }},
        {MsgType::ContextStats,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return contextStatsFromJson(j);
         }},
        {MsgType::ErrorMsg,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return errorFromJson(j);
         }},
        {MsgType::LogMsg,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return logFromJson(j);
         }},
        {MsgType::GetModel,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return getModelFromJson(j);
         }},
        {MsgType::ModelInfo,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return modelInfoFromJson(j);
         }},
        {MsgType::GetAppendComponentInfo,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return getAppendComponentInfoFromJson(j);
         }},
        {MsgType::AppendComponentInfo,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return appendComponentInfoMessageFromJson(j);
         }},
        {MsgType::GetContext,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return getContextFromJson(j);
         }},
        {MsgType::CompactContext,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return compactContextFromJson(j);
         }},
        {MsgType::ContextMessages,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return contextMessagesFromJson(j);
         }},
        {MsgType::ListSessions,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return listSessionsFromJson(j);
         }},
        {MsgType::SessionList,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return sessionListFromJson(j);
         }},
        {MsgType::SwitchSession,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return switchSessionFromJson(j);
         }},
        {MsgType::SetPermission,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return setPermissionFromJson(j);
         }},
        {MsgType::PluginData,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return pluginDataFromJson(j);
         }},
        {MsgType::PluginDataUp,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return pluginDataUpFromJson(j);
         }},
        {MsgType::MessageQueueUpdate,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return messageQueueUpdateFromJson(j);
         }},
        {MsgType::ClearMessageQueue,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return clearMessageQueueFromJson(j);
         }},
        {MsgType::RemoveQueueItem,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return removeQueueItemFromJson(j);
         }},
        {MsgType::InterruptAndRunNext,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return interruptAndRunNextFromJson(j);
         }},
        {MsgType::GetViewMessages,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             return getViewMessagesFromJson(j);
         }},
        {MsgType::ViewMessagesPage,
         [](const agentxx::util::Json& j) -> std::optional<WireMessage> {
             auto p = viewMessagesPageFromJson(j);
             return p.has_value() ? std::optional<WireMessage>{std::move(p.value())} : std::nullopt;
         }},
    };
    return s_map;
}

std::optional<WireMessage> deserialize(std::string_view jsonText) {
    // 高频路径: 先经 JsonView 零拷贝路由 type (§4.3), 未知类型直接丢弃不物化;
    // 命中后再全量物化一次走既有 fromJson (各消息体需完整 DOM, 物化一次不可避免)
    agentxx::util::JsonView jv;
    try {
        jv = agentxx::util::JsonView::parse(jsonText);
    } catch (...) {
        return std::nullopt;
    }
    if (!jv.is_object()) {
        return std::nullopt;
    }
    std::string t = msgTypeView(jv);
    if (t.empty()) {
        return std::nullopt;
    }
    const auto& mapProbe = getDeserializerMap();
    if (mapProbe.find(t) == mapProbe.end()) {
        return std::nullopt;
    }
    agentxx::util::Json j;
    try {
        j = jv.to_json();
    } catch (...) {
        return std::nullopt;
    }
    const auto& map = getDeserializerMap();
    auto        it  = map.find(t);
    if (it != map.end()) {
        return it->second(j);
    }
    return std::nullopt;
}

} // namespace io
} // namespace agent
} // namespace agentxx
