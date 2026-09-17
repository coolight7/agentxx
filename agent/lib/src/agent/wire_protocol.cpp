#include "agentxx/agent/io/wire_protocol.h"
#include "utilxx_base/json.h"
#include "utilxx_base/json_view.h"
#include <unordered_map>

namespace agentxx {
namespace agent {
namespace io {

// ---------------------------------------------------------------------------
// 对称 toJson 实现
// ---------------------------------------------------------------------------

utilxx_base::Json toJson(const WireHello& msg) {
    return makeHello(msg.sessionId, msg.token, msg.lastSeq, msg.tailHash, msg.language);
}

utilxx_base::Json toJson(const WireHelloAck& msg) {
    return makeHelloAck(
        msg.ok,
        msg.sessionId,
        msg.tailHash,
        msg.models,
        msg.plugins,
        msg.deviceId,
        msg.workDir
    );
}

utilxx_base::Json toJson(const WireUserInput& msg) {
    return makeUserInput(msg.sessionId, msg.text, msg.model, msg.attachments);
}

utilxx_base::Json toJson(const WireCancel& msg) {
    return makeCancel(msg.sessionId);
}

utilxx_base::Json toJson(const WireSelectModel& msg) {
    return makeSelectModel(msg.sessionId, msg.model);
}

utilxx_base::Json toJson(const WireInterruptRequest& msg) {
    return makeInterruptRequest(msg.id, msg.sessionId, msg.node, msg.value, msg.argJson);
}

utilxx_base::Json toJson(const WireInterruptResponse& msg) {
    return makeInterruptResponse(msg.id, msg.result);
}

utilxx_base::Json toJson(const WireInterruptExpired& msg) {
    return makeInterruptExpired(msg.id, msg.sessionId);
}

utilxx_base::Json toJson(const WireDelta& msg) {
    return makeDeltaMsg(msg);
}

utilxx_base::Json toJson(const WireSyncPayload& msg) {
    return makeSyncMsg(msg);
}

utilxx_base::Json toJson(const WireTurnResult& msg) {
    return makeTurnResult(
        msg.sessionId,
        msg.hasError,
        msg.errorMessage,
        msg.interrupted,
        msg.startTimeMs,
        msg.durationMs
    );
}

utilxx_base::Json toJson(const WireContextStats& msg) {
    return makeContextStats(msg.contextTokens, msg.maxContextTokens, msg.tps);
}

utilxx_base::Json toJson(const WireError& msg) {
    return makeError(msg.code, msg.message);
}

utilxx_base::Json toJson(const WireLog& msg) {
    return makeLog(msg.level, msg.message);
}

utilxx_base::Json toJson(const WireGetModel& msg) {
    return makeGetModel(msg.sessionId);
}

utilxx_base::Json toJson(const WireModelInfo& msg) {
    return makeModelInfo(msg.currentModel, msg.models, msg.capabilities);
}

utilxx_base::Json toJson(const WireGetAppendComponentInfo& msg) {
    return makeGetAppendComponentInfo(msg.sessionId);
}

utilxx_base::Json toJson(const WireAppendComponentInfo& msg) {
    return makeAppendComponentInfo(msg.notifications);
}

utilxx_base::Json toJson(const WireGetContext& msg) {
    return makeGetContext(msg.sessionId);
}

utilxx_base::Json toJson(const WireCompactContext& msg) {
    return makeCompactContext(msg.sessionId);
}

utilxx_base::Json toJson(const WireContextMessages& msg) {
    return makeContextMessages(msg.messages);
}

utilxx_base::Json toJson(const WireListSessions& msg) {
    return makeListSessions(msg.beforeMs, msg.beforeId, msg.limit);
}

utilxx_base::Json toJson(const WireSessionList& msg) {
    return makeSessionList(msg.sessions, msg.totalCount, msg.hasMore);
}

utilxx_base::Json toJson(const WireSwitchSession& msg) {
    return makeSwitchSession(msg.sessionId);
}

utilxx_base::Json toJson(const WirePluginData& msg) {
    return makePluginData(msg);
}

utilxx_base::Json toJson(const WirePluginDataUp& msg) {
    return makePluginDataUp(msg);
}

utilxx_base::Json toJson(const WireMessageQueueUpdate& msg) {
    return makeMessageQueueUpdate(msg.sessionId, msg.items);
}

utilxx_base::Json toJson(const WireClearMessageQueue& msg) {
    return makeClearMessageQueue(msg.sessionId);
}

utilxx_base::Json toJson(const WireRemoveQueueItem& msg) {
    return makeRemoveQueueItem(msg.sessionId, msg.itemId);
}

utilxx_base::Json toJson(const WireInterruptAndRunNext& msg) {
    return makeInterruptAndRunNext(msg.sessionId);
}

utilxx_base::Json toJson(const WireGetViewMessages& msg) {
    return makeGetViewMessages(msg.sessionId, msg.beforeIndex, msg.count);
}

utilxx_base::Json toJson(const WireViewMessagesPage& msg) {
    return makeViewMessagesPage(msg.sessionId, msg.startIndex, msg.totalCount, msg.messages);
}

utilxx_base::Json toJson(const WireListDir& msg) {
    return makeListDir(msg.reqId, msg.path, msg.allowedExtensions);
}

utilxx_base::Json toJson(const WireListDirResult& msg) {
    return makeListDirResult(
        msg.reqId,
        msg.ok,
        msg.currentDir,
        msg.parentDir,
        msg.entries,
        msg.error
    );
}

// ---------------------------------------------------------------------------
// 对称 fromJson 实现
// ---------------------------------------------------------------------------

WireHello helloFromJson(const utilxx_base::Json& j) {
    WireHello hello;
    hello.sessionId = j.value("sessionId", std::string{});
    hello.token     = j.value("token", std::string{});
    hello.lastSeq   = j.value("lastSeq", uint64_t{0});
    hello.tailHash  = j.value("tailHash", std::string{});
    hello.language  = j.value("language", std::string{});
    return hello;
}

WireHelloAck helloAckFromJson(const utilxx_base::Json& j) {
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
    ack.deviceId = j.value("deviceId", std::string{});
    ack.workDir  = j.value("workDir", std::string{});
    return ack;
}

WireUserInput userInputFromJson(const utilxx_base::Json& j) {
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

WireCancel cancelFromJson(const utilxx_base::Json& j) {
    WireCancel cancel;
    cancel.sessionId = j.value("sessionId", std::string{});
    return cancel;
}

WireSelectModel selectModelFromJson(const utilxx_base::Json& j) {
    WireSelectModel sm;
    sm.sessionId = j.value("sessionId", std::string{});
    sm.model     = j.value("model", std::string{});
    return sm;
}

WireInterruptRequest interruptRequestFromJson(const utilxx_base::Json& j) {
    WireInterruptRequest req;
    req.id        = j.value("id", int64_t{0});
    req.sessionId = j.value("sessionId", std::string{});
    req.node      = j.value("node", std::string{});
    req.value     = j.value("value", std::string{});
    req.argJson   = j.value("argJson", std::string{});
    return req;
}

WireInterruptResponse interruptResponseFromJson(const utilxx_base::Json& j) {
    WireInterruptResponse resp;
    resp.id     = j.value("id", int64_t{0});
    resp.result = j.value("result", utilxx_base::Json{});
    return resp;
}

WireInterruptExpired interruptExpiredFromJson(const utilxx_base::Json& j) {
    WireInterruptExpired expired;
    expired.id        = j.value("id", int64_t{0});
    expired.sessionId = j.value("sessionId", std::string{});
    return expired;
}

WireTurnResult turnResultFromJson(const utilxx_base::Json& j) {
    WireTurnResult res;
    res.sessionId    = j.value("sessionId", std::string{});
    res.hasError     = j.value("hasError", false);
    res.errorMessage = j.value("errorMessage", std::string{});
    res.interrupted  = j.value("interrupted", false);
    res.startTimeMs  = j.value("startTimeMs", int64_t{0});
    res.durationMs   = j.value("durationMs", int64_t{0});
    return res;
}

WireContextStats contextStatsFromJson(const utilxx_base::Json& j) {
    WireContextStats stats;
    stats.contextTokens    = j.value("contextTokens", uint64_t{0});
    stats.maxContextTokens = j.value("maxContextTokens", uint64_t{0});
    stats.tps              = j.value("tps", 0.0);
    return stats;
}

WireError errorFromJson(const utilxx_base::Json& j) {
    WireError err;
    err.code    = j.value("code", 0);
    err.message = j.value("message", std::string{});
    return err;
}

WireLog logFromJson(const utilxx_base::Json& j) {
    WireLog log;
    log.level   = j.value("level", 0);
    log.message = j.value("message", std::string{});
    return log;
}

WireGetModel getModelFromJson(const utilxx_base::Json& j) {
    WireGetModel req;
    req.sessionId = j.value("sessionId", std::string{});
    return req;
}

WireModelInfo modelInfoFromJson(const utilxx_base::Json& j) {
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

WireGetAppendComponentInfo getAppendComponentInfoFromJson(const utilxx_base::Json& j) {
    WireGetAppendComponentInfo req;
    req.sessionId = j.value("sessionId", std::string{});
    return req;
}

WireAppendComponentInfo appendComponentInfoMessageFromJson(const utilxx_base::Json& j) {
    WireAppendComponentInfo info;
    info.notifications = appendComponentInfoFromJson(j);
    return info;
}

WireGetContext getContextFromJson(const utilxx_base::Json& j) {
    WireGetContext req;
    req.sessionId = j.value("sessionId", std::string{});
    return req;
}

WireCompactContext compactContextFromJson(const utilxx_base::Json& j) {
    WireCompactContext req;
    req.sessionId = j.value("sessionId", std::string{});
    return req;
}

WireContextMessages contextMessagesFromJson(const utilxx_base::Json& j) {
    WireContextMessages resp;
    resp.messages = j.value("messages", utilxx_base::Json::array());
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

using DeserializerFn = std::optional<WireMessage> (*)(const utilxx_base::Json&);

static const std::unordered_map<std::string_view, DeserializerFn>& getDeserializerMap() {
    static const std::unordered_map<std::string_view, DeserializerFn> s_map = {
        {MsgType::Hello,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return helloFromJson(j);
         }},
        {MsgType::HelloAck,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return helloAckFromJson(j);
         }},
        {MsgType::UserInput,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return userInputFromJson(j);
         }},
        {MsgType::Cancel,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return cancelFromJson(j);
         }},
        {MsgType::SelectModel,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return selectModelFromJson(j);
         }},
        {MsgType::InterruptRequest,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return interruptRequestFromJson(j);
         }},
        {MsgType::InterruptResponse,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return interruptResponseFromJson(j);
         }},
        {MsgType::InterruptExpired,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return interruptExpiredFromJson(j);
         }},
        {MsgType::DeltaMsg,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             auto d = deltaMsgFromJson(j);
             return d.has_value() ? std::optional<WireMessage>{std::move(d.value())} : std::nullopt;
         }},
        {MsgType::SyncMsg,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             auto s = syncMsgFromJson(j);
             return s.has_value() ? std::optional<WireMessage>{std::move(s.value())} : std::nullopt;
         }},
        {MsgType::TurnResult,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return turnResultFromJson(j);
         }},
        {MsgType::ContextStats,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return contextStatsFromJson(j);
         }},
        {MsgType::ErrorMsg,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return errorFromJson(j);
         }},
        {MsgType::LogMsg,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return logFromJson(j);
         }},
        {MsgType::GetModel,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return getModelFromJson(j);
         }},
        {MsgType::ModelInfo,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return modelInfoFromJson(j);
         }},
        {MsgType::GetAppendComponentInfo,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return getAppendComponentInfoFromJson(j);
         }},
        {MsgType::AppendComponentInfo,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return appendComponentInfoMessageFromJson(j);
         }},
        {MsgType::GetContext,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return getContextFromJson(j);
         }},
        {MsgType::CompactContext,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return compactContextFromJson(j);
         }},
        {MsgType::ContextMessages,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return contextMessagesFromJson(j);
         }},
        {MsgType::ListSessions,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return listSessionsFromJson(j);
         }},
        {MsgType::SessionList,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return sessionListFromJson(j);
         }},
        {MsgType::SwitchSession,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return switchSessionFromJson(j);
         }},
        {MsgType::PluginData,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return pluginDataFromJson(j);
         }},
        {MsgType::PluginDataUp,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return pluginDataUpFromJson(j);
         }},
        {MsgType::MessageQueueUpdate,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return messageQueueUpdateFromJson(j);
         }},
        {MsgType::ClearMessageQueue,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return clearMessageQueueFromJson(j);
         }},
        {MsgType::RemoveQueueItem,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return removeQueueItemFromJson(j);
         }},
        {MsgType::InterruptAndRunNext,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return interruptAndRunNextFromJson(j);
         }},
        {MsgType::GetViewMessages,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return getViewMessagesFromJson(j);
         }},
        {MsgType::ViewMessagesPage,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             auto p = viewMessagesPageFromJson(j);
             return p.has_value() ? std::optional<WireMessage>{std::move(p.value())} : std::nullopt;
         }},
        {MsgType::ListDir,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return listDirFromJson(j);
         }},
        {MsgType::ListDirResult,
         [](const utilxx_base::Json& j) -> std::optional<WireMessage> {
             return listDirResultFromJson(j);
         }},
    };
    return s_map;
}

std::optional<WireMessage> deserialize(std::string_view jsonText) {
    // 高频路径: 先经 JsonView 零拷贝路由 type (§4.3), 未知类型直接丢弃不物化;
    // 命中后再全量物化一次走既有 fromJson (各消息体需完整 DOM, 物化一次不可避免)
    utilxx_base::JsonView jv;
    try {
        jv = utilxx_base::JsonView::parse(jsonText);
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
    utilxx_base::Json j;
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
