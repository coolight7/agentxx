#include "chatbot_panel.h"

#include "chatbot_core.h"

#include "utilxx_base/json.h"

#include <algorithm>

namespace agentxx {
namespace chatbot {

namespace {

using utilxx_base::Json;

std::string textOf(const Json& object, std::string_view key) {
    return object.value(key, std::string{});
}

size_t sizeOf(const Json& object, std::string_view key) {
    if (!object.is_object() || !object.contains(key)) {
        return 0;
    }
    return static_cast<size_t>(object[key].get<int64_t>());
}

int64_t int64Of(const Json& object, std::string_view key) {
    if (!object.is_object() || !object.contains(key)) {
        return 0;
    }
    return object[key].get<int64_t>();
}

Json sessionToJson(const PanelSessionView& session) {
    Json item = Json::object();
    if (!session.chatKey.empty()) {
        item["key"] = session.chatKey;
    }
    item["title"]    = session.title;
    item["persona"]  = session.persona;
    item["account"]  = session.account;
    item["type"]     = session.chatType;
    item["mode"]     = session.mode;
    item["sessionId"]     = session.sessionId;
    item["channelId"]     = session.channelId;
    item["ready"]         = session.ready;
    item["running"]       = session.running;
    item["queued"]        = static_cast<int64_t>(session.queued);
    item["turns"]         = static_cast<int64_t>(session.turns);
    item["sent"]          = static_cast<int64_t>(session.sent);
    item["lastTurnMs"]    = session.lastTurnMs;
    item["lastActiveMs"]  = session.lastActiveMs;
    return item;
}

PanelSessionView sessionFromJson(const Json& item) {
    PanelSessionView session;
    session.chatKey      = textOf(item, "key");
    session.title        = textOf(item, "title");
    session.persona      = textOf(item, "persona");
    session.account      = textOf(item, "account");
    session.chatType     = textOf(item, "type");
    session.mode         = textOf(item, "mode");
    session.sessionId    = textOf(item, "sessionId");
    session.channelId    = int64Of(item, "channelId");
    session.ready        = item.is_object() && item.contains("ready") && item["ready"].get<bool>();
    session.running
        = item.is_object() && item.contains("running") && item["running"].get<bool>();
    session.queued       = sizeOf(item, "queued");
    session.turns        = static_cast<uint64_t>(int64Of(item, "turns"));
    session.sent         = static_cast<uint64_t>(int64Of(item, "sent"));
    session.lastTurnMs   = int64Of(item, "lastTurnMs");
    session.lastActiveMs = int64Of(item, "lastActiveMs");
    return session;
}

Json personaToJson(const PanelPersonaView& persona) {
    Json item = Json::object();
    item["name"]       = persona.name;
    item["model"]      = persona.model;
    item["promptHead"] = persona.promptHead;
    item["rootId"]     = persona.rootId;
    item["mounted"]    = persona.mounted;
    item["chats"]      = static_cast<int64_t>(persona.chats);
    item["budget"]     = static_cast<int64_t>(persona.chatBudget);
    item["binds"]      = static_cast<int64_t>(persona.binds);
    item["tools"]      = static_cast<int64_t>(persona.tools);
    return item;
}

PanelPersonaView personaFromJson(const Json& item) {
    PanelPersonaView persona;
    persona.name       = textOf(item, "name");
    persona.model      = textOf(item, "model");
    persona.promptHead = textOf(item, "promptHead");
    persona.rootId     = textOf(item, "rootId");
    persona.mounted
        = item.is_object() && item.contains("mounted") && item["mounted"].get<bool>();
    persona.chats      = sizeOf(item, "chats");
    persona.chatBudget = sizeOf(item, "budget");
    persona.binds      = sizeOf(item, "binds");
    persona.tools      = sizeOf(item, "tools");
    return persona;
}

Json accountToJson(const PanelAccountView& account) {
    Json item = Json::object();
    item["id"]     = account.id;
    item["type"]   = account.type;
    item["state"]  = account.state;
    item["reason"] = account.reason;
    return item;
}

PanelAccountView accountFromJson(const Json& item) {
    PanelAccountView account;
    account.id     = textOf(item, "id");
    account.type   = textOf(item, "type");
    account.state  = textOf(item, "state");
    account.reason = textOf(item, "reason");
    return account;
}

} // namespace

std::string panelSnapshotJson(const PanelSnapshot& snapshot) {
    Json out = Json::object();
    out["v"]           = kPanelSnapshotVersion;
    out["tick"]        = static_cast<int64_t>(snapshot.tick);
    out["startupTime"] = snapshot.startupTime;

    Json accounts = Json::array();
    for (const auto& account : snapshot.accounts) {
        accounts.push_back(accountToJson(account));
    }
    out["accounts"] = std::move(accounts);

    Json personas = Json::array();
    for (const auto& persona : snapshot.personas) {
        personas.push_back(personaToJson(persona));
    }
    out["personas"] = std::move(personas);

    Json sessions = Json::array();
    for (const auto& session : snapshot.sessions) {
        sessions.push_back(sessionToJson(session));
    }
    out["sessions"] = std::move(sessions);

    Json stats = Json::object();
    stats["received"] = static_cast<int64_t>(snapshot.received);
    stats["sent"]     = static_cast<int64_t>(snapshot.sent);
    stats["failed"]   = static_cast<int64_t>(snapshot.failed);
    out["stats"]      = std::move(stats);

    return out.dump();
}

bool parsePanelSnapshot(std::string_view json, PanelSnapshot& out) {
    if (json.empty()) {
        return false;
    }
    Json root;
    try {
        root = Json::parse(json);
    } catch (const std::exception&) {
        return false;
    }
    if (!root.is_object()) {
        return false;
    }
    // 老客户端遇到新版本时不要乱猜字段含义: 直接当作"没有数据"
    const int version = static_cast<int>(int64Of(root, "v"));
    if (version > kPanelSnapshotVersion) {
        return false;
    }

    PanelSnapshot snapshot;
    snapshot.tick        = static_cast<uint32_t>(int64Of(root, "tick"));
    snapshot.startupTime = textOf(root, "startupTime");

    if (root.contains("accounts") && root["accounts"].is_array()) {
        for (const auto& item : root["accounts"]) {
            if (item.is_object()) {
                snapshot.accounts.push_back(accountFromJson(item));
            }
        }
    }
    if (root.contains("personas") && root["personas"].is_array()) {
        for (const auto& item : root["personas"]) {
            if (item.is_object()) {
                snapshot.personas.push_back(personaFromJson(item));
            }
        }
    }
    if (root.contains("sessions") && root["sessions"].is_array()) {
        for (const auto& item : root["sessions"]) {
            if (item.is_object()) {
                snapshot.sessions.push_back(sessionFromJson(item));
            }
        }
    }
    if (root.contains("stats") && root["stats"].is_object()) {
        const auto& stats  = root["stats"];
        snapshot.received  = static_cast<uint64_t>(int64Of(stats, "received"));
        snapshot.sent      = static_cast<uint64_t>(int64Of(stats, "sent"));
        snapshot.failed    = static_cast<uint64_t>(int64Of(stats, "failed"));
    }
    out = std::move(snapshot);
    return true;
}

/* ==================== 展示文案 ==================== */

std::string panelTruncate(std::string_view text, size_t maxCodePoints) {
    if (maxCodePoints == 0 || utf8CodePointCount(text) <= maxCodePoints) {
        return std::string{text};
    }
    if (maxCodePoints <= 1) {
        return "…";
    }
    return std::string{utf8Truncate(text, maxCodePoints - 1)} + "…";
}

std::string panelSessionSubtitle(const PanelSessionView& session) {
    std::string out;
    if (!session.persona.empty()) {
        out += session.persona;
    } else {
        out += "未归属人设";
    }
    out += " · ";
    out += session.chatType == "private" ? "私聊" : "群聊";
    if (!session.mode.empty()) {
        out += " · ";
        out += session.mode;
    }
    return out;
}

std::string panelPersonaSubtitle(const PanelPersonaView& persona) {
    std::string out = "会话 " + std::to_string(persona.chats) + "/"
                      + std::to_string(persona.chatBudget) + " · 绑定 "
                      + std::to_string(persona.binds);
    if (!persona.mounted) {
        out += " · 未挂载";
    }
    return out;
}

std::string panelPersonaModelText(const PanelPersonaView& persona) {
    return persona.model.empty() ? std::string{"默认模型"} : persona.model;
}

std::string panelPersonaDetail(const PanelPersonaView& persona) {
    // 窄栏里把模型与用量接在一行, 不在右侧单独开一列
    return panelPersonaModelText(persona) + " · " + panelPersonaSubtitle(persona);
}

std::string panelSessionQueueText(const PanelSessionView& session) {
    return std::to_string(session.queued);
}

std::string panelAccountItemText(const PanelAccountView& account) {
    std::string out = account.id.empty() ? std::string{"?"} : account.id;
    if (!account.state.empty()) {
        out += " " + account.state;
    }
    // 平台给了原因 (断线/被踢等) 就一并显示, 否则只留状态
    if (!account.reason.empty()) {
        out += " (" + account.reason + ")";
    }
    return out;
}

} // namespace chatbot
} // namespace agentxx
