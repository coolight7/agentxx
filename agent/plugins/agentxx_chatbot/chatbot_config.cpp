#include "chatbot_config.h"

#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#if __has_include("yaml-cpp/yaml.h")
#include "yaml-cpp/yaml.h"
#define AGENTXX_CHATBOT_HAS_YAML 1
#else
#define AGENTXX_CHATBOT_HAS_YAML 0
#endif

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace agentxx {
namespace chatbot {

namespace {

using utilxx_base::Json;

/// 日志标签 (与插件名一致, 便于在主程序日志里筛)
constexpr const char* kLogTag = "chatbot";

/// 键名字符集检查 (`[A-Za-z0-9_-]`)
bool isIdentifier(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    for (char c : text) {
        const auto uc = static_cast<unsigned char>(c);
        const bool ok = (uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z')
                        || (uc >= '0' && uc <= '9') || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

void setError(ConfigError& err, std::string where, std::string message) {
    err.where   = std::move(where);
    err.message = std::move(message);
}

/// `${VAR}` 展开 (主程序只对 plugin.args 展开, 配置文件里的需要插件自己做)
std::string expandEnv(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '$' && i + 1 < text.size() && text[i + 1] == '{') {
            const auto close = text.find('}', i + 2);
            if (close != std::string_view::npos) {
                const std::string name{text.substr(i + 2, close - i - 2)};
                const char*       value = name.empty() ? nullptr : std::getenv(name.c_str());
                if (value != nullptr) {
                    out += value;
                    i = close + 1;
                    continue;
                }
            }
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

/// 读一个可能为数字的标识值
///
/// 主程序展开 `${VAR}` 后会**按标量重新判类型** (见设计文档 §5.3), 于是纯数字的
/// `appId` 会变成 JSON 数字。这里接受 string / 整数, 并把浮点当作"精度已丢失"拒绝。
bool readIdText(const Json& value, std::string& out, std::string& why) {
    if (value.is_string()) {
        out = value.get<std::string>();
        why.clear();
        return true;
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
        out = std::to_string(value.get<long long>());
        why.clear();
        return true;
    }
    if (value.is_number_float()) {
        why = "该值被解析成了浮点数 (数字太长, 精度可能已丢失): 请改用 appIdFile 或在 "
              "yaml 里给它一个非数字前缀";
        return false;
    }
    why = "该值不是字符串也不是整数";
    return false;
}

std::string jsonStringOf(const Json& obj, std::string_view key, std::string_view fallback = {}) {
    if (!obj.is_object() || !obj.contains(key)) {
        return std::string{fallback};
    }
    const auto& value = obj[std::string{key}];
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return std::to_string(value.get<long long>());
    }
    return std::string{fallback};
}

int64_t jsonIntOf(const Json& obj, std::string_view key, int64_t fallback) {
    if (!obj.is_object() || !obj.contains(key)) {
        return fallback;
    }
    const auto& value = obj[std::string{key}];
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return value.get<long long>();
    }
    if (value.is_number_float()) {
        return static_cast<int64_t>(value.get<double>());
    }
    if (value.is_string()) {
        try {
            return std::stoll(value.get<std::string>());
        } catch (const std::exception&) {
            return fallback;
        }
    }
    return fallback;
}

bool jsonBoolOf(const Json& obj, std::string_view key, bool fallback) {
    if (!obj.is_object() || !obj.contains(key)) {
        return fallback;
    }
    const auto& value = obj[std::string{key}];
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return value.get<long long>() != 0;
    }
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        return text == "true" || text == "1" || text == "yes";
    }
    return fallback;
}

/// 深度合并两个 JSON 对象: `overlay` 覆盖 `base`
/// - 映射逐键递归合并; 列表整体替换; 其余标量直接替换
Json mergeJson(Json base, const Json& overlay) {
    if (!base.is_object() || !overlay.is_object()) {
        return overlay;
    }
    for (const auto& [key, value] : overlay.items()) {
        if (base.contains(key) && base[key].is_object() && value.is_object()) {
            base[key] = mergeJson(base[key], value);
        } else {
            base[key] = value;
        }
    }
    return base;
}

/* ==================== yaml → JSON ==================== */

#if AGENTXX_CHATBOT_HAS_YAML

Json yamlNodeToJson(const YAML::Node& node) {
    if (!node || node.IsNull()) {
        return Json{};
    }
    if (node.IsSequence()) {
        Json arr = Json::array();
        for (const auto& item : node) {
            arr.push_back(yamlNodeToJson(item));
        }
        return arr;
    }
    if (node.IsMap()) {
        Json obj = Json::object();
        for (const auto& kv : node) {
            std::string key;
            try {
                key = kv.first.as<std::string>();
            } catch (const std::exception&) {
                key = kv.first.Scalar();
            }
            obj[key] = yamlNodeToJson(kv.second);
        }
        return obj;
    }
    // 标量: 先按整数 / 浮点 / 布尔试, 失败当字符串
    // 注意用圆括号构造: `Json{x}` 会命中 `initializer_list<Json>` 构造而变成
    // 单元素数组 (`["123456"]`), 不是标量
    const auto scalar = node.Scalar();
    if (node.Tag() != "!") {
        try {
            size_t          used  = 0;
            const long long asInt = std::stoll(scalar, &used);
            if (used == scalar.size()) {
                return Json(asInt);
            }
        } catch (const std::exception&) {
        }
        try {
            size_t       used    = 0;
            const double asFloat = std::stod(scalar, &used);
            if (used == scalar.size() && scalar.find('.') != std::string::npos) {
                return Json(asFloat);
            }
        } catch (const std::exception&) {
        }
        if (scalar == "true") {
            return Json(true);
        }
        if (scalar == "false") {
            return Json(false);
        }
    }
    return Json(expandEnv(scalar));
}

#endif

/// 逐项展开 JSON 里的 `${VAR}` (只处理字符串值, 递归)
Json expandEnvInJson(const Json& value) {
    if (value.is_string()) {
        return Json(expandEnv(value.get<std::string>()));
    }
    if (value.is_array()) {
        Json arr = Json::array();
        for (const auto& item : value) {
            arr.push_back(expandEnvInJson(item));
        }
        return arr;
    }
    if (value.is_object()) {
        Json obj = Json::object();
        for (const auto& [key, item] : value.items()) {
            obj[key] = expandEnvInJson(item);
        }
        return obj;
    }
    return value;
}

std::string readWholeFile(std::string_view path) {
    std::ifstream in(std::string{path}, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/* ==================== 分段解析 ==================== */

bool parseAccounts(const Json& root, ChatbotConfig& out, ConfigError& err) {
    if (!root.contains("accounts")) {
        return true;
    }
    const auto& arr = root["accounts"];
    if (!arr.is_array()) {
        setError(err, "accounts", "必须是数组");
        return false;
    }
    int index = 0;
    for (const auto& item : arr) {
        const std::string where = "accounts[" + std::to_string(index) + "]";
        ++index;
        if (!item.is_object()) {
            setError(err, where, "必须是对象");
            return false;
        }
        AccountConfig account;
        account.id   = jsonStringOf(item, "id");
        account.type = jsonStringOf(item, "type", "qq");
        account.sandbox = jsonBoolOf(item, "sandbox", false);
        account.apiBase = jsonStringOf(item, "apiBase");

        std::vector<int32_t> shard;
        if (item.contains("wsShard") && item["wsShard"].is_array()) {
            for (const auto& one : item["wsShard"]) {
                if (one.is_number_integer() || one.is_number_unsigned()) {
                    shard.push_back(static_cast<int32_t>(one.get<long long>()));
                }
            }
        }
        if (shard.empty()) {
            shard = {0, 1};
        }
        account.shard = std::move(shard);

        if (item.contains("rateLimit") && item["rateLimit"].is_object()) {
            account.groupPerMinute  = static_cast<int32_t>(
                jsonIntOf(item["rateLimit"], "groupPerMinute", account.groupPerMinute)
            );
            account.singlePerMinute = static_cast<int32_t>(
                jsonIntOf(item["rateLimit"], "singlePerMinute", account.singlePerMinute)
            );
        }

        // 凭证: 显式键优先, `*File` 只补齐缺失项
        std::string why;
        if (item.contains("appId")) {
            if (!readIdText(item["appId"], account.appId, why)) {
                setError(err, where + ".appId", why);
                return false;
            }
        }
        if (item.contains("appSecret")) {
            if (!readIdText(item["appSecret"], account.appSecret, why)) {
                setError(err, where + ".appSecret", why);
                return false;
            }
        }
        if (account.appId.empty()) {
            const auto appIdFile = jsonStringOf(item, "appIdFile");
            if (!appIdFile.empty()) {
                account.appId = readKeyFromEnvFile(appIdFile, "QQBOT_APPID");
                if (account.appId.empty()) {
                    setError(
                        err,
                        where + ".appIdFile",
                        "文件里找不到 QQBOT_APPID (需要 KEY=VALUE 每行一条)"
                    );
                    return false;
                }
            }
        }
        if (account.appSecret.empty()) {
            const auto secretFile = jsonStringOf(item, "appSecretFile");
            if (!secretFile.empty()) {
                account.appSecret = readKeyFromEnvFile(secretFile, "QQBOT_APPSERCRET");
                if (account.appSecret.empty()) {
                    account.appSecret = readKeyFromEnvFile(secretFile, "QQBOT_APPSECRET");
                }
                if (account.appSecret.empty()) {
                    setError(
                        err,
                        where + ".appSecretFile",
                        "文件里找不到 QQBOT_APPSERCRET (需要 KEY=VALUE 每行一条)"
                    );
                    return false;
                }
            }
        }
        out.accounts.push_back(std::move(account));
    }
    return true;
}

bool parseBindRule(
    const Json&        item,
    const std::string& where,
    BindRule&          rule,
    ConfigError&       err
) {
    if (!item.is_object()) {
        setError(err, where, "必须是对象");
        return false;
    }
    // account 由调用方保证 (chats 形态从外层继承, 简化写法自己带)
    rule.accountId  = jsonStringOf(item, "account");
    const std::string typeText = jsonStringOf(item, "type", "group");
    if (typeText == "any" || typeText == "*") {
        rule.type     = ChatType::Group;
        rule.wildcardId = true; // type: any + id: "*" 覆盖该账号下所有会话
    } else if (!chatTypeFromString(typeText, rule.type)) {
        setError(err, where + ".type", "只能是 group / private / any");
        return false;
    }
    rule.chatId = jsonStringOf(item, "id");
    if (rule.chatId.empty()) {
        setError(err, where + ".id", "必填 (要覆盖全部用 \"*\")");
        return false;
    }
    if (rule.chatId == "*") {
        rule.wildcardId = true;
    }
    const std::string modeText = jsonStringOf(item, "mode", "mention");
    if (!chatModeFromString(modeText, rule.mode)) {
        setError(err, where + ".mode", "只能是 mention / always / off");
        return false;
    }
    return true;
}

bool parsePersonas(const Json& root, ChatbotConfig& out, ConfigError& err) {
    if (!root.contains("personas")) {
        return true;
    }
    const auto& arr = root["personas"];
    if (!arr.is_array()) {
        setError(err, "personas", "必须是数组");
        return false;
    }
    int index = 0;
    for (const auto& item : arr) {
        const std::string where = "personas[" + std::to_string(index) + "]";
        ++index;
        if (!item.is_object()) {
            setError(err, where, "必须是对象");
            return false;
        }
        PersonaConfig persona;
        persona.name  = jsonStringOf(item, "name");
        persona.model = jsonStringOf(item, "model");
        persona.enableSessionStore = jsonBoolOf(item, "enableSessionStore", true);
        if (item.contains("enableSummarization")) {
            persona.enableSummarization = jsonBoolOf(item, "enableSummarization", true);
        }

        if (item.contains("prompt") && item["prompt"].is_object()) {
            const auto& prompt = item["prompt"];
            persona.prompt.systemPrompt = jsonStringOf(prompt, "systemPrompt");
            if (prompt.contains("append") && prompt["append"].is_object()) {
                for (const auto& [key, value] : prompt["append"].items()) {
                    if (value.is_string()) {
                        persona.prompt.append.emplace_back(key, value.get<std::string>());
                    } else {
                        XX_LOGW(
                            "[{}] {}: prompt.append['{}'] 不是字符串, 已忽略",
                            kLogTag,
                            where,
                            key
                        );
                    }
                }
            }
        }
        if (item.contains("tools") && item["tools"].is_array()) {
            for (const auto& one : item["tools"]) {
                if (one.is_string()) {
                    persona.tools.push_back(one.get<std::string>());
                }
            }
        }
        if (item.contains("mount") && item["mount"].is_object()) {
            const auto& mount = item["mount"];
            persona.mount.mode = jsonStringOf(mount, "mode", "none");
            if (mount.contains("list") && mount["list"].is_array()) {
                for (const auto& one : mount["list"]) {
                    if (one.is_string()) {
                        persona.mount.list.push_back(one.get<std::string>());
                    }
                }
            }
            if (mount.contains("args") && mount["args"].is_object()) {
                persona.mount.argsJson = mount["args"].dump();
            }
        }
        if (item.contains("bind")) {
            const auto& binds = item["bind"];
            if (!binds.is_array()) {
                setError(err, where + ".bind", "必须是数组");
                return false;
            }
            int bindIndex = 0;
            for (const auto& bindItem : binds) {
                const std::string bindWhere
                    = where + ".bind[" + std::to_string(bindIndex) + "]";
                ++bindIndex;
                BindRule rule;
                if (!bindItem.is_object()) {
                    setError(err, bindWhere, "必须是对象");
                    return false;
                }
                const std::string accountId = jsonStringOf(bindItem, "account");
                if (accountId.empty()) {
                    setError(err, bindWhere + ".account", "必填");
                    return false;
                }
                if (bindItem.contains("chats")) {
                    const auto& chats = bindItem["chats"];
                    if (!chats.is_array()) {
                        setError(err, bindWhere + ".chats", "必须是数组");
                        return false;
                    }
                    int chatIndex = 0;
                    for (const auto& chatItem : chats) {
                        const std::string chatWhere
                            = bindWhere + ".chats[" + std::to_string(chatIndex) + "]";
                        ++chatIndex;
                        BindRule chatRule;
                        if (!parseBindRule(chatItem, chatWhere, chatRule, err)) {
                            return false;
                        }
                        chatRule.accountId = accountId;
                        persona.binds.push_back(std::move(chatRule));
                    }
                } else {
                    // 简化写法: bind 项本身就是一条规则
                    BindRule single;
                    if (!parseBindRule(bindItem, bindWhere, single, err)) {
                        return false;
                    }
                    single.accountId = accountId;
                    persona.binds.push_back(std::move(single));
                }
            }
        }
        out.personas.push_back(std::move(persona));
    }
    return true;
}

void parseBehavior(const Json& root, ChatbotConfig& out) {
    if (!root.contains("behavior") || !root["behavior"].is_object()) {
        return;
    }
    const auto& j   = root["behavior"];
    auto&       cfg = out.behavior;
    cfg.aggregateWindowMs = static_cast<int32_t>(
        jsonIntOf(j, "aggregateWindowMs", cfg.aggregateWindowMs)
    );
    cfg.sendIntervalMs = static_cast<int32_t>(
        jsonIntOf(j, "sendIntervalMs", cfg.sendIntervalMs)
    );
    cfg.messageSplitLimit = static_cast<int32_t>(
        jsonIntOf(j, "messageSplitLimit", cfg.messageSplitLimit)
    );
    cfg.splitMarker = jsonBoolOf(j, "splitMarker", cfg.splitMarker);
    cfg.maxSplitParts = static_cast<int32_t>(jsonIntOf(j, "maxSplitParts", cfg.maxSplitParts));
    cfg.markdown       = jsonBoolOf(j, "markdown", cfg.markdown);
    cfg.maxRepliesPerTurn = static_cast<int32_t>(
        jsonIntOf(j, "maxRepliesPerTurn", cfg.maxRepliesPerTurn)
    );
    cfg.maxChatsPerPersona = static_cast<int32_t>(
        jsonIntOf(j, "maxChatsPerPersona", cfg.maxChatsPerPersona)
    );
    cfg.historyKeep = static_cast<int32_t>(jsonIntOf(j, "historyKeep", cfg.historyKeep));
    cfg.attachmentsFetch = jsonStringOf(j, "attachments.fetch", cfg.attachmentsFetch);
    if (j.contains("attachments") && j["attachments"].is_object()) {
        const auto& att             = j["attachments"];
        cfg.attachmentsFetch        = jsonStringOf(att, "fetch", cfg.attachmentsFetch);
        cfg.attachmentsMaxPerMessage = static_cast<int32_t>(
            jsonIntOf(att, "maxPerMessage", cfg.attachmentsMaxPerMessage)
        );
    }
    cfg.exposeToolsOnHost = jsonBoolOf(j, "exposeToolsOnHost", cfg.exposeToolsOnHost);
    cfg.overflowPolicy    = jsonStringOf(j, "overflowPolicy", cfg.overflowPolicy);
    cfg.fallbackReply     = jsonStringOf(j, "fallbackReply", cfg.fallbackReply);
    cfg.injectContextHint = jsonBoolOf(j, "injectContextHint", cfg.injectContextHint);
    cfg.flushStaleOnNextMessage
        = jsonBoolOf(j, "flushStaleOnNextMessage", cfg.flushStaleOnNextMessage);
    cfg.humanInLoop = jsonStringOf(j, "humanInLoop", cfg.humanInLoop);
    cfg.aggregateMaxItems = static_cast<int32_t>(
        jsonIntOf(j, "aggregateMaxItems", cfg.aggregateMaxItems)
    );
    cfg.aggregateMaxChars = static_cast<int32_t>(
        jsonIntOf(j, "aggregateMaxChars", cfg.aggregateMaxChars)
    );
    cfg.outboundQueueMax = static_cast<int32_t>(
        jsonIntOf(j, "outboundQueueMax", cfg.outboundQueueMax)
    );
    cfg.dedupeCapacity = static_cast<int32_t>(jsonIntOf(j, "dedupeCapacity", cfg.dedupeCapacity));
    cfg.inboundMaxChars = static_cast<int32_t>(
        jsonIntOf(j, "inboundMaxChars", cfg.inboundMaxChars)
    );
    cfg.debugTools = jsonBoolOf(j, "debugTools", cfg.debugTools);
}

void parseCommands(const Json& root, ChatbotConfig& out) {
    if (!root.contains("commands") || !root["commands"].is_array()) {
        return;
    }
    for (const auto& item : root["commands"]) {
        if (!item.is_object()) {
            continue;
        }
        CommandDef def;
        def.name = jsonStringOf(item, "name");
        CommandMode mode = CommandMode::Local;
        if (commandModeFromString(jsonStringOf(item, "mode", "local"), mode)) {
            def.mode = mode;
        }
        def.reply  = jsonStringOf(item, "reply");
        def.prompt = jsonStringOf(item, "prompt");
        def.action = jsonStringOf(item, "action");
        out.commands.push_back(std::move(def));
    }
}

} // namespace

/* ==================== ChatbotConfig ==================== */

const AccountConfig* ChatbotConfig::findAccount(std::string_view id) const {
    for (const auto& account : accounts) {
        if (account.id == id) {
            return &account;
        }
    }
    return nullptr;
}

const PersonaConfig* ChatbotConfig::findPersona(std::string_view name) const {
    for (const auto& persona : personas) {
        if (persona.name == name) {
            return &persona;
        }
    }
    return nullptr;
}

/* ==================== .env 形态的键值 ==================== */

std::string readKeyFromEnvText(std::string_view text, std::string_view key) {
    size_t pos = 0;
    while (pos <= text.size()) {
        const auto nl  = text.find('\n', pos);
        const auto end = (nl == std::string_view::npos) ? text.size() : nl;
        std::string line{text.substr(pos, end - pos)};
        if (nl == std::string_view::npos) {
            pos = text.size() + 1;
        } else {
            pos = nl + 1;
        }
        // 去掉 `\r` 与两端空白
        line = trimCopy(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line.rfind("export ", 0) == 0) {
            line = trimCopy(std::string_view{line}.substr(7));
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        if (trimCopy(std::string_view{line}.substr(0, eq)) != key) {
            continue;
        }
        std::string value = trimCopy(std::string_view{line}.substr(eq + 1));
        if (value.size() >= 2
            && ((value.front() == '"' && value.back() == '"')
                || (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        return value;
    }
    return {};
}

std::string readKeyFromEnvFile(std::string_view path, std::string_view key) {
    const auto text = readWholeFile(path);
    if (text.empty()) {
        return {};
    }
    return readKeyFromEnvText(text, key);
}

/* ==================== 解析 ==================== */

bool parseConfig(std::string_view argsJson, ChatbotConfig& out, ConfigError& err) {
    out = ChatbotConfig{};
    Json root;
    try {
        root = argsJson.empty() ? Json::object() : Json::parse(std::string{argsJson});
    } catch (const std::exception& e) {
        setError(err, "args", std::string{"不是合法 JSON: "} + e.what());
        return false;
    }
    if (!root.is_object()) {
        setError(err, "args", "必须是 JSON 对象");
        return false;
    }
    // 环境变量在 args 里通常已由主程序展开, 这里再兜一次 (config 文件合并进来的值需要)
    root = expandEnvInJson(root);

    if (!parseAccounts(root, out, err)) {
        return false;
    }
    if (!parsePersonas(root, out, err)) {
        return false;
    }
    parseBehavior(root, out);
    parseCommands(root, out);
    return true;
}

bool mergeConfigFile(std::string_view filePath, std::string& argsJsonInOut, ConfigError& err) {
    if (filePath.empty()) {
        return true;
    }
    const auto text = readWholeFile(filePath);
    if (text.empty()) {
        setError(err, std::string{filePath}, "配置文件读不到或为空");
        return false;
    }
    Json fileJson;
    bool parsed = false;
    // 先按 JSON 试 (便于用 .json 写同一份四段), 失败再按 YAML
    try {
        fileJson = Json::parse(text);
        parsed   = fileJson.is_object();
    } catch (const std::exception&) {
        parsed = false;
    }
#if AGENTXX_CHATBOT_HAS_YAML
    if (!parsed) {
        try {
            const auto node = YAML::Load(text);
            fileJson        = yamlNodeToJson(node);
            parsed          = fileJson.is_object();
        } catch (const std::exception& e) {
            setError(err, std::string{filePath}, std::string{"解析失败: "} + e.what());
            return false;
        }
    }
#endif
    if (!parsed) {
        setError(err, std::string{filePath}, "解析失败: 既不是 JSON 也不是可用 YAML 映射");
        return false;
    }

    Json args;
    try {
        args = argsJsonInOut.empty() ? Json::object() : Json::parse(argsJsonInOut);
    } catch (const std::exception&) {
        args = Json::object();
    }
    if (!args.is_object()) {
        args = Json::object();
    }
    // args 覆盖文件: 映射逐键合并, 列表整体覆盖
    const auto merged = mergeJson(fileJson, args);
    argsJsonInOut     = merged.dump();
    err               = ConfigError{};
    return true;
}

/* ==================== 校验 ==================== */

bool validateConfig(ChatbotConfig& config, int32_t maxRoots, ConfigError& err) {
    // ---- accounts ----
    if (config.accounts.empty()) {
        setError(err, "accounts", "至少要声明一个平台账号");
        return false;
    }
    for (size_t i = 0; i < config.accounts.size(); ++i) {
        const auto&       account = config.accounts[i];
        const std::string where   = "accounts[" + std::to_string(i) + "]";
        if (!isIdentifier(account.id)) {
            setError(
                err,
                where + ".id",
                "必填且只能包含 [A-Za-z0-9_-] (人设按它对接)"
            );
            return false;
        }
        for (size_t k = 0; k < i; ++k) {
            if (config.accounts[k].id == account.id) {
                setError(err, where + ".id", "重复的账号标识 '" + account.id + "'");
                return false;
            }
        }
        // 平台类型: `qq` 需要凭证, `fake` 是内存假平台 (测试与本地演示)
        if (account.type != "qq" && account.type != "fake") {
            setError(
                err,
                where + ".type",
                "暂不支持平台 '" + account.type + "' (当前只实现 qq / fake)"
            );
            return false;
        }
        if (account.type == "fake") {
            continue; // 假平台不需要凭证
        }
        if (account.appId.empty()) {
            setError(err, where + ".appId", "取不到值 (可用 appId / appIdFile)");
            return false;
        }
        if (account.appSecret.empty()) {
            setError(err, where + ".appSecret", "取不到值 (可用 appSecret / appSecretFile)");
            return false;
        }
    }

    // ---- personas ----
    if (config.personas.empty()) {
        setError(err, "personas", "至少要声明一个人设");
        return false;
    }
    for (size_t i = 0; i < config.personas.size(); ++i) {
        auto&             persona = config.personas[i];
        const std::string where   = "personas[" + std::to_string(i) + "]";
        if (!isIdentifier(persona.name)) {
            setError(err, where + ".name", "必填且只能包含 [A-Za-z0-9_-]");
            return false;
        }
        for (size_t k = 0; k < i; ++k) {
            if (config.personas[k].name == persona.name) {
                setError(err, where + ".name", "重复的人设名 '" + persona.name + "'");
                return false;
            }
        }
        if (persona.mount.mode != "none" && persona.mount.mode != "all"
            && persona.mount.mode != "listed") {
            setError(err, where + ".mount.mode", "只能是 none / all / listed");
            return false;
        }
        if (persona.mount.mode == "listed" && persona.mount.list.empty()) {
            setError(err, where + ".mount.mode", "listed 时必须给非空 list");
            return false;
        }
        // 每条 bind 必须指向已声明账号; 同一 (账号, 类型, 会话) 只能属于一个人设
        for (const auto& rule : persona.binds) {
            if (config.findAccount(rule.accountId) == nullptr) {
                setError(
                    err,
                    where + ".bind",
                    "account '" + rule.accountId + "' 不在 accounts 里"
                );
                return false;
            }
        }
    }
    // 同群归属唯一 + 每 (账号, 类型) 最多一条通配
    {
        std::map<std::string, std::string, std::less<>> preciseOwner;
        std::map<std::string, std::string, std::less<>> wildcardOwner;
        for (const auto& persona : config.personas) {
            for (const auto& rule : persona.binds) {
                if (rule.wildcardId) {
                    const std::string scope
                        = rule.accountId + "|" + chatTypeName(rule.type);
                    auto [it, inserted] = wildcardOwner.emplace(scope, persona.name);
                    if (!inserted) {
                        setError(
                            err,
                            "personas[" + persona.name + "].bind",
                            "账号 '" + rule.accountId + "' 的 "
                                + chatTypeName(rule.type) + " 会话通配项重复 (已由 '"
                                + it->second + "' 声明)"
                        );
                        return false;
                    }
                } else {
                    const std::string scope = rule.accountId + "|" + chatTypeName(rule.type)
                                              + "|" + rule.chatId;
                    auto [it, inserted] = preciseOwner.emplace(scope, persona.name);
                    if (!inserted) {
                        setError(
                            err,
                            "personas[" + persona.name + "].bind",
                            "会话 '" + scope + "' 已由人设 '" + it->second
                                + "' 接管 (同一会话只能归一个人设)"
                        );
                        return false;
                    }
                }
            }
        }
    }

    // ---- 根预算 ----
    if (maxRoots > 0
        && static_cast<int32_t>(config.personas.size()) + 1 > maxRoots) {
        setError(
            err,
            "personas",
            "人设数 " + std::to_string(config.personas.size()) + " 超出上限 (最多 "
                + std::to_string(std::max(0, maxRoots - 1))
                + " 个: 进程内根数上限 " + std::to_string(maxRoots) + " 含内置根)"
        );
        return false;
    }

    // ---- commands ----
    for (size_t i = 0; i < config.commands.size(); ++i) {
        auto&             command = config.commands[i];
        const std::string where   = "commands[" + std::to_string(i) + "]";
        if (command.name.empty() || command.name[0] != '/') {
            setError(err, where + ".name", "必须以 '/' 开头");
            return false;
        }
        for (size_t k = 0; k < i; ++k) {
            if (config.commands[k].name == command.name) {
                setError(err, where + ".name", "重复的命令名 '" + command.name + "'");
                return false;
            }
        }
        if (command.mode == CommandMode::Action && command.action != "cancel"
            && command.action != "clear") {
            setError(err, where + ".action", "只能是 cancel / clear");
            return false;
        }
        if (command.mode == CommandMode::Local && command.reply.empty()) {
            setError(err, where + ".reply", "local 命令必须给回复模板");
            return false;
        }
        if (command.mode == CommandMode::Model && command.prompt.empty()) {
            setError(err, where + ".prompt", "model 命令必须给提示词模板");
            return false;
        }
    }

    // ---- behavior: 越界裁剪到合法范围 ----
    auto& behavior = config.behavior;
    auto  clampInt = [&err](
                        int32_t&       value,
                        int32_t        low,
                        int32_t        high,
                        const char*    name
                    ) {
        const auto before = value;
        value             = std::clamp(value, low, high);
        if (before != value) {
            XX_LOGW(
                "[{}] behavior.{} = {} 超出合法范围 [{}, {}], 已裁剪为 {}",
                kLogTag,
                name,
                before,
                low,
                high,
                value
            );
        }
    };
    clampInt(behavior.aggregateWindowMs, 0, 60000, "aggregateWindowMs");
    clampInt(behavior.sendIntervalMs, 0, 60000, "sendIntervalMs");
    clampInt(behavior.messageSplitLimit, 0, 20000, "messageSplitLimit");
    if (behavior.messageSplitLimit > 0 && behavior.messageSplitLimit < 100) {
        XX_LOGW(
            "[{}] behavior.messageSplitLimit = {} 太小, 已提升到 100",
            kLogTag,
            behavior.messageSplitLimit
        );
        behavior.messageSplitLimit = 100;
    }
    clampInt(behavior.maxSplitParts, 1, 64, "maxSplitParts");
    clampInt(behavior.maxRepliesPerTurn, 1, 64, "maxRepliesPerTurn");
    clampInt(behavior.maxChatsPerPersona, 1, 64, "maxChatsPerPersona");
    clampInt(behavior.historyKeep, 0, 1000, "historyKeep");
    clampInt(behavior.attachmentsMaxPerMessage, 0, 5, "attachmentsMaxPerMessage");
    clampInt(behavior.aggregateMaxItems, 1, 500, "aggregateMaxItems");
    clampInt(behavior.aggregateMaxChars, 100, 2000000, "aggregateMaxChars");
    clampInt(behavior.outboundQueueMax, 1, 10000, "outboundQueueMax");
    clampInt(behavior.dedupeCapacity, 16, 65536, "dedupeCapacity");
    clampInt(behavior.inboundMaxChars, 100, 200000, "inboundMaxChars");

    if (behavior.attachmentsFetch != "auto" && behavior.attachmentsFetch != "url"
        && behavior.attachmentsFetch != "download") {
        setError(err, "behavior.attachments.fetch", "只能是 auto / url / download");
        return false;
    }
    if (behavior.overflowPolicy != "active" && behavior.overflowPolicy != "truncate"
        && behavior.overflowPolicy != "fail") {
        setError(err, "behavior.overflowPolicy", "只能是 active / truncate / fail");
        return false;
    }
    if (behavior.fallbackReply != "mention" && behavior.fallbackReply != "always"
        && behavior.fallbackReply != "never") {
        setError(err, "behavior.fallbackReply", "只能是 mention / always / never");
        return false;
    }
    if (behavior.humanInLoop != "auto-allow" && behavior.humanInLoop != "auto-deny") {
        setError(err, "behavior.humanInLoop", "只能是 auto-allow / auto-deny");
        return false;
    }
    err = ConfigError{};
    return true;
}

/* ==================== 路由表 ==================== */

void RouterTable::build(const ChatbotConfig& config) {
    precise_.clear();
    wildcard_.clear();
    overrides_.clear();
    preciseKeys_.clear();
    for (const auto& persona : config.personas) {
        for (const auto& rule : persona.binds) {
            RouteResult result;
            result.found       = true;
            result.personaName = persona.name;
            result.mode        = rule.mode;
            result.wildcard    = rule.wildcardId;
            if (rule.wildcardId) {
                const std::string scope = rule.accountId + "|" + chatTypeName(rule.type);
                wildcard_[scope]        = result;
            } else {
                const std::string scope
                    = rule.accountId + "|" + chatTypeName(rule.type) + "|" + rule.chatId;
                precise_[scope] = PreciseEntry{result};
                preciseKeys_.push_back(scope);
            }
        }
    }
    std::sort(preciseKeys_.begin(), preciseKeys_.end());
}

RouteResult RouterTable::route(const ChatKey& key) const {
    if (!key.valid()) {
        return RouteResult{};
    }
    const std::string preciseScope
        = key.accountId + "|" + chatTypeName(key.type) + "|" + key.chatId;
    auto applyOverride = [this, &key](RouteResult result) {
        // 运行期改过的模式优先生效 (不写回配置)
        if (auto it = overrides_.find(key.key()); it != overrides_.end()) {
            result.mode = it->second;
        }
        return result;
    };
    if (auto it = precise_.find(preciseScope); it != precise_.end()) {
        return applyOverride(it->second.result);
    }
    const std::string wildcardScope = key.accountId + "|" + chatTypeName(key.type);
    if (auto it = wildcard_.find(wildcardScope); it != wildcard_.end()) {
        return applyOverride(it->second);
    }
    return RouteResult{};
}

void RouterTable::setModeOverride(std::string_view chatKey, ChatMode mode) {
    overrides_[std::string{chatKey}] = mode;
}

ChatMode RouterTable::effectiveMode(const ChatKey& key, const RouteResult& base) const {
    if (auto it = overrides_.find(key.key()); it != overrides_.end()) {
        return it->second;
    }
    return base.mode;
}

std::vector<std::string> RouterTable::chatKeysOf(std::string_view personaName) const {
    std::vector<std::string> out;
    for (const auto& [scope, entry] : precise_) {
        if (entry.result.personaName == personaName) {
            out.push_back(scope);
        }
    }
    return out;
}

} // namespace chatbot
} // namespace agentxx
