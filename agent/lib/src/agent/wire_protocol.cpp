#include "agentxx/agent/io/wire_protocol.h"
#include <unordered_map>

namespace agentxx {
namespace agent {
namespace io {

// ---------------------------------------------------------------------------
// 对称 toJson 实现
// ---------------------------------------------------------------------------

neograph::json toJson(const WireHello& msg) {
    return makeHello(msg.sessionId, msg.token, msg.lastSeq, msg.tailHash, msg.language);
}

neograph::json toJson(const WireHelloAck& msg) {
    return makeHelloAck(msg.ok, msg.sessionId, msg.tailHash, msg.models, msg.plugins);
}

neograph::json toJson(const WireUserInput& msg) {
    return makeUserInput(msg.sessionId, msg.text, msg.model);
}

neograph::json toJson(const WireCancel& msg) {
    return makeCancel(msg.sessionId);
}

neograph::json toJson(const WireSelectModel& msg) {
    return makeSelectModel(msg.sessionId, msg.model);
}

neograph::json toJson(const WireInterruptRequest& msg) {
    return makeInterruptRequest(msg.id, msg.sessionId, msg.node, msg.value, msg.argJson);
}

neograph::json toJson(const WireInterruptResponse& msg) {
    return makeInterruptResponse(msg.id, msg.result);
}

neograph::json toJson(const WireInterruptExpired& msg) {
    return makeInterruptExpired(msg.id, msg.sessionId);
}

neograph::json toJson(const WireDelta& msg) {
    return makeDeltaMsg(msg);
}

neograph::json toJson(const WireSyncPayload& msg) {
    return makeSyncMsg(msg);
}

neograph::json toJson(const WireTurnResult& msg) {
    return makeTurnResult(
        msg.sessionId,
        msg.hasError,
        msg.errorMessage,
        msg.interrupted,
        msg.startTimeMs,
        msg.durationMs
    );
}

neograph::json toJson(const WireContextStats& msg) {
    return makeContextStats(msg.contextTokens, msg.maxContextTokens, msg.tps);
}

neograph::json toJson(const WireError& msg) {
    return makeError(msg.code, msg.message);
}

neograph::json toJson(const WireLog& msg) {
    return makeLog(msg.level, msg.message);
}

neograph::json toJson(const WireGetModel& msg) {
    return makeGetModel(msg.sessionId);
}

neograph::json toJson(const WireModelInfo& msg) {
    return makeModelInfo(msg.currentModel, msg.models);
}

neograph::json toJson(const WireGetAppendComponentInfo& msg) {
    return makeGetAppendComponentInfo(msg.sessionId);
}

neograph::json toJson(const WireAppendComponentInfo& msg) {
    return makeAppendComponentInfo(msg.notifications);
}

neograph::json toJson(const WireGetContext& msg) {
    return makeGetContext(msg.sessionId);
}

neograph::json toJson(const WireCompactContext& msg) {
    return makeCompactContext(msg.sessionId);
}

neograph::json toJson(const WireContextMessages& msg) {
    return makeContextMessages(msg.messages);
}

neograph::json toJson(const WireListSessions& msg) {
    return makeListSessions(msg.beforeMs, msg.beforeId, msg.limit);
}

neograph::json toJson(const WireSessionList& msg) {
    return makeSessionList(msg.sessions, msg.totalCount, msg.hasMore);
}

neograph::json toJson(const WireSwitchSession& msg) {
    return makeSwitchSession(msg.sessionId);
}

neograph::json toJson(const WireSetPermission& msg) {
    return makeSetPermission(msg.sessionId, msg.path, msg.allow, msg.index);
}

neograph::json toJson(const WirePluginData& msg) {
    return makePluginData(msg);
}

neograph::json toJson(const WirePluginDataUp& msg) {
    return makePluginDataUp(msg);
}

neograph::json toJson(const WireMessageQueueUpdate& msg) {
    return makeMessageQueueUpdate(msg.sessionId, msg.items);
}

neograph::json toJson(const WireClearMessageQueue& msg) {
    return makeClearMessageQueue(msg.sessionId);
}

neograph::json toJson(const WireRemoveQueueItem& msg) {
    return makeRemoveQueueItem(msg.sessionId, msg.itemId);
}

neograph::json toJson(const WireInterruptAndRunNext& msg) {
    return makeInterruptAndRunNext(msg.sessionId);
}

neograph::json toJson(const WireGetViewMessages& msg) {
    return makeGetViewMessages(msg.sessionId, msg.beforeIndex, msg.count);
}

neograph::json toJson(const WireViewMessagesPage& msg) {
    return makeViewMessagesPage(msg.sessionId, msg.startIndex, msg.totalCount, msg.messages);
}

// ---------------------------------------------------------------------------
// 对称 fromJson 实现
// ---------------------------------------------------------------------------

WireHello helloFromJson(const neograph::json& j) {
    WireHello hello;
    hello.sessionId = j.value("sessionId", std::string{});
    hello.token     = j.value("token", std::string{});
    hello.lastSeq   = j.value("lastSeq", uint64_t{0});
    hello.tailHash  = j.value("tailHash", std::string{});
    hello.language  = j.value("language", std::string{});
    return hello;
}

WireHelloAck helloAckFromJson(const neograph::json& j) {
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

WireUserInput userInputFromJson(const neograph::json& j) {
    WireUserInput input;
    input.sessionId = j.value("sessionId", std::string{});
    input.text      = j.value("text", std::string{});
    input.model     = j.value("model", std::string{});
    return input;
}

WireCancel cancelFromJson(const neograph::json& j) {
    WireCancel cancel;
    cancel.sessionId = j.value("sessionId", std::string{});
    return cancel;
}

WireSelectModel selectModelFromJson(const neograph::json& j) {
    WireSelectModel sm;
    sm.sessionId = j.value("sessionId", std::string{});
    sm.model     = j.value("model", std::string{});
    return sm;
}

WireInterruptRequest interruptRequestFromJson(const neograph::json& j) {
    WireInterruptRequest req;
    req.id        = j.value("id", int64_t{0});
    req.sessionId = j.value("sessionId", std::string{});
    req.node      = j.value("node", std::string{});
    req.value     = j.value("value", std::string{});
    req.argJson   = j.value("argJson", std::string{});
    return req;
}

WireInterruptResponse interruptResponseFromJson(const neograph::json& j) {
    WireInterruptResponse resp;
    resp.id     = j.value("id", int64_t{0});
    resp.result = j.value("result", neograph::json{});
    return resp;
}

WireInterruptExpired interruptExpiredFromJson(const neograph::json& j) {
    WireInterruptExpired expired;
    expired.id        = j.value("id", int64_t{0});
    expired.sessionId = j.value("sessionId", std::string{});
    return expired;
}

WireTurnResult turnResultFromJson(const neograph::json& j) {
    WireTurnResult res;
    res.sessionId    = j.value("sessionId", std::string{});
    res.hasError     = j.value("hasError", false);
    res.errorMessage = j.value("errorMessage", std::string{});
    res.interrupted  = j.value("interrupted", false);
    res.startTimeMs  = j.value("startTimeMs", int64_t{0});
    res.durationMs   = j.value("durationMs", int64_t{0});
    return res;
}

WireContextStats contextStatsFromJson(const neograph::json& j) {
    WireContextStats stats;
    stats.contextTokens    = j.value("contextTokens", uint64_t{0});
    stats.maxContextTokens = j.value("maxContextTokens", uint64_t{0});
    stats.tps              = j.value("tps", 0.0);
    return stats;
}

WireError errorFromJson(const neograph::json& j) {
    WireError err;
    err.code    = j.value("code", 0);
    err.message = j.value("message", std::string{});
    return err;
}

WireLog logFromJson(const neograph::json& j) {
    WireLog log;
    log.level   = j.value("level", 0);
    log.message = j.value("message", std::string{});
    return log;
}

WireGetModel getModelFromJson(const neograph::json& j) {
    WireGetModel req;
    req.sessionId = j.value("sessionId", std::string{});
    return req;
}

WireModelInfo modelInfoFromJson(const neograph::json& j) {
    WireModelInfo info;
    info.currentModel = j.value("currentModel", std::string{});
    if (j.contains("models") && j["models"].is_array()) {
        for (const auto& m : j["models"]) {
            if (m.is_string()) {
                info.models.push_back(m.get<std::string>());
            }
        }
    }
    return info;
}

WireGetAppendComponentInfo getAppendComponentInfoFromJson(const neograph::json& j) {
    WireGetAppendComponentInfo req;
    req.sessionId = j.value("sessionId", std::string{});
    return req;
}

WireAppendComponentInfo appendComponentInfoMessageFromJson(const neograph::json& j) {
    WireAppendComponentInfo info;
    info.notifications = appendComponentInfoFromJson(j);
    return info;
}

WireGetContext getContextFromJson(const neograph::json& j) {
    WireGetContext req;
    req.sessionId = j.value("sessionId", std::string{});
    return req;
}

WireCompactContext compactContextFromJson(const neograph::json& j) {
    WireCompactContext req;
    req.sessionId = j.value("sessionId", std::string{});
    return req;
}

WireContextMessages contextMessagesFromJson(const neograph::json& j) {
    WireContextMessages resp;
    resp.messages = j.value("messages", neograph::json::array());
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

using DeserializerFn = std::optional<WireMessage> (*)(const neograph::json&);

static const std::unordered_map<std::string_view, DeserializerFn>& getDeserializerMap() {
    static const std::unordered_map<std::string_view, DeserializerFn> s_map = {
        {MsgType::Hello,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return helloFromJson(j);
         }},
        {MsgType::HelloAck,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return helloAckFromJson(j);
         }},
        {MsgType::UserInput,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return userInputFromJson(j);
         }},
        {MsgType::Cancel,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return cancelFromJson(j);
         }},
        {MsgType::SelectModel,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return selectModelFromJson(j);
         }},
        {MsgType::InterruptRequest,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return interruptRequestFromJson(j);
         }},
        {MsgType::InterruptResponse,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return interruptResponseFromJson(j);
         }},
        {MsgType::InterruptExpired,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return interruptExpiredFromJson(j);
         }},
        {MsgType::DeltaMsg,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             auto d = deltaMsgFromJson(j);
             return d.has_value() ? std::optional<WireMessage>{std::move(d.value())} : std::nullopt;
         }},
        {MsgType::SyncMsg,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             auto s = syncMsgFromJson(j);
             return s.has_value() ? std::optional<WireMessage>{std::move(s.value())} : std::nullopt;
         }},
        {MsgType::TurnResult,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return turnResultFromJson(j);
         }},
        {MsgType::ContextStats,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return contextStatsFromJson(j);
         }},
        {MsgType::ErrorMsg,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return errorFromJson(j);
         }},
        {MsgType::LogMsg,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return logFromJson(j);
         }},
        {MsgType::GetModel,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return getModelFromJson(j);
         }},
        {MsgType::ModelInfo,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return modelInfoFromJson(j);
         }},
        {MsgType::GetAppendComponentInfo,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return getAppendComponentInfoFromJson(j);
         }},
        {MsgType::AppendComponentInfo,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return appendComponentInfoMessageFromJson(j);
         }},
        {MsgType::GetContext,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return getContextFromJson(j);
         }},
        {MsgType::CompactContext,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return compactContextFromJson(j);
         }},
        {MsgType::ContextMessages,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return contextMessagesFromJson(j);
         }},
        {MsgType::ListSessions,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return listSessionsFromJson(j);
         }},
        {MsgType::SessionList,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return sessionListFromJson(j);
         }},
        {MsgType::SwitchSession,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return switchSessionFromJson(j);
         }},
        {MsgType::SetPermission,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return setPermissionFromJson(j);
         }},
        {MsgType::PluginData,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return pluginDataFromJson(j);
         }},
        {MsgType::PluginDataUp,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return pluginDataUpFromJson(j);
         }},
        {MsgType::MessageQueueUpdate,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return messageQueueUpdateFromJson(j);
         }},
        {MsgType::ClearMessageQueue,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return clearMessageQueueFromJson(j);
         }},
        {MsgType::RemoveQueueItem,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return removeQueueItemFromJson(j);
         }},
        {MsgType::InterruptAndRunNext,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return interruptAndRunNextFromJson(j);
         }},
        {MsgType::GetViewMessages,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             return getViewMessagesFromJson(j);
         }},
        {MsgType::ViewMessagesPage,
         [](const neograph::json& j) -> std::optional<WireMessage> {
             auto p = viewMessagesPageFromJson(j);
             return p.has_value() ? std::optional<WireMessage>{std::move(p.value())} : std::nullopt;
         }},
    };
    return s_map;
}

std::optional<WireMessage> deserialize(std::string_view jsonText) {
    neograph::json j;
    try {
        j = neograph::json::parse(jsonText);
    } catch (...) {
        return std::nullopt;
    }
    if (!j.is_object()) {
        return std::nullopt;
    }
    std::string t   = msgType(j);
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
