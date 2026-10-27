/// 插件运行时: 根配置派生 (`AgentConfig` 覆盖 JSON + 插件挂载策略)
///
/// 插件经 `mount_root` 交出两份 JSON:
/// - `config_json`: `AgentConfig` 的字段覆盖 (提示词/工具白名单/模型/语言/工作目录/开关);
/// - `mount_plugins`: 该根的插件挂载策略 (与 yaml `plugin.mount` 同构)。
///
/// 两个来源分开是刻意的: 挂载策略决定"这个根有哪些工具", 与配置派生混在一处以后
/// 排查"为什么模型看不到文件工具"要翻两套规则。两处都给 `pluginMount` 视作参数错误。
///
/// 继承规则 (§6 派生规则表):
/// - 继承: 模型 / 语言 / 工作目录 / 权限路径 / MCP·Skill·Memory·RAG 列表 (按 `inherit.*`
///   开关, 其中 mcp/skill/memory 默认**不**继承);
/// - 不继承: 提示词 (默认用内置 coding 人设, 插件应显式给 `prompt.systemPrompt`)、
///   工具白名单、插件挂载策略;
/// - 会话目录: 每个根独占一份 (`{会话目录}/{rootId}`), 与"同一份磁盘会话只能由一个
///   agent 写"的约定一致。
#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/util/exception.h"
#include "fmt/format.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"

#include <filesystem>
#include <string>

namespace agentxx {
namespace plugin {

namespace {

/// 权限模式文本 -> 枚举 (与 client/FFI 的取值一致; 未知值按 Ask 处理)
agentxx::agent::PermissionMode permissionModeOf(std::string_view text) {
    if (text == "allask" || text == "all-ask") {
        return agentxx::agent::PermissionMode::AllAsk;
    }
    if (text == "pass") {
        return agentxx::agent::PermissionMode::Pass;
    }
    if (text == "deny") {
        return agentxx::agent::PermissionMode::Deny;
    }
    return agentxx::agent::PermissionMode::Ask;
}

/// 从 JSON 里读一个字符串数组 (缺失或类型不对返回空)
std::vector<std::string> stringArrayOf(const utilxx_base::Json& j, std::string_view key) {
    return utilxx_base::jsonGetStringArray(j, std::string{key});
}

/// 解析 `mount_plugins`: 与 yaml `plugin.mount` 同构
/// - `{"mode":"none|all|listed","list":[...],"args":{...}}`
/// - 空串 = `none` (根只有内置工具)
bool applyMountPolicy(
    agentxx::agent::AgentConfig& cfg,
    std::string_view             json,
    std::string&                 err
) {
    cfg.pluginMountPolicy = agentxx::agent::PluginMountPolicy::None;
    cfg.pluginMountList.clear();
    cfg.pluginMountArgs = utilxx_base::Json::object();
    if (json.empty()) {
        return true;
    }
    utilxx_base::Json j;
    try {
        j = utilxx_base::Json::parse(std::string{json});
    } catch (const std::exception& e) {
        err = fmt::format("mount_plugins 不是合法 JSON: {}", e.what());
        return false;
    }
    if (!j.is_object()) {
        err = "mount_plugins 必须是对象 {\"mode\":...,\"list\":[...],\"args\":{...}}";
        return false;
    }
    const std::string mode = j.value("mode", std::string{"none"});
    if (mode == "none") {
        cfg.pluginMountPolicy = agentxx::agent::PluginMountPolicy::None;
    } else if (mode == "all") {
        cfg.pluginMountPolicy = agentxx::agent::PluginMountPolicy::All;
    } else if (mode == "listed") {
        cfg.pluginMountPolicy = agentxx::agent::PluginMountPolicy::Listed;
        cfg.pluginMountList   = stringArrayOf(j, "list");
        if (cfg.pluginMountList.empty()) {
            err = "mount_plugins.mode=listed 时必须给非空 list";
            return false;
        }
    } else {
        err = fmt::format("mount_plugins.mode 取值非法: '{}' (只支持 none/all/listed)", mode);
        return false;
    }
    if (j.contains("args")) {
        if (!j["args"].is_object()) {
            err = "mount_plugins.args 必须是对象 (键 = 插件名)";
            return false;
        }
        cfg.pluginMountArgs = j["args"];
    }
    return true;
}

/// 解析 `prompt` 段: 主体 + 附加段 + 工具提示词覆盖
void applyPrompt(agentxx::agent::AgentConfig& cfg, const utilxx_base::Json& prompt, std::string_view owner) {
    if (prompt.contains("systemPrompt") && prompt["systemPrompt"].is_string()) {
        cfg.prompt.systemPrompt = prompt["systemPrompt"].get<std::string>();
    }
    if (prompt.contains("append")) {
        if (prompt["append"].is_object()) {
            for (const auto& [key, value] : prompt["append"].items()) {
                if (!value.is_string()) {
                    XX_LOGW("[runtime] prompt.append['{}'] 不是字符串, 已忽略", std::string{key});
                    continue;
                }
                cfg.prompt.setAppendSection(
                    key,
                    value.get<std::string>(),
                    0,
                    fmt::format("plugin:{}", owner)
                );
            }
        }
    }
    if (prompt.contains("toolPrompt") && prompt["toolPrompt"].is_object()) {
        for (const auto& [toolName, value] : prompt["toolPrompt"].items()) {
            if (!value.is_object()) {
                continue;
            }
            auto& item = cfg.prompt.toolPrompt[std::string{toolName}];
            if (value.contains("depict") && value["depict"].is_string()) {
                item.depict = value["depict"].get<std::string>();
            }
            if (value.contains("args") && value["args"].is_object()) {
                for (const auto& [argName, argValue] : value["args"].items()) {
                    if (argValue.is_string()) {
                        utilxx_base::insertOrAssignHeterogeneous(
                            item.args,
                            std::string{argName},
                            argValue.get<std::string>()
                        );
                    }
                }
            }
        }
    }
}

/// 继承开关 (缺省值见 §6 派生规则表)
struct InheritFlags {
    bool model      = true;
    bool language   = true;
    bool workDir    = true;
    bool permission = true;
    bool prompt     = false;
    bool mcp        = false;
    bool skill      = false;
    bool memory     = false;
    bool rag        = false;
};

InheritFlags inheritFlagsOf(const utilxx_base::Json& j) {
    InheritFlags flags;
    if (!j.contains("inherit") || !j["inherit"].is_object()) {
        return flags;
    }
    const auto& in       = j["inherit"];
    flags.model          = in.value("model", flags.model);
    flags.language       = in.value("language", flags.language);
    flags.workDir        = in.value("workDir", flags.workDir);
    flags.permission     = in.value("permission", flags.permission);
    flags.prompt         = in.value("prompt", flags.prompt);
    flags.mcp            = in.value("mcp", flags.mcp);
    flags.skill          = in.value("skill", flags.skill);
    flags.memory         = in.value("memory", flags.memory);
    flags.rag            = in.value("rag", flags.rag);
    return flags;
}

/// MCP 服务器 JSON: `{"ns": {"url": "...", "timeoutSec": 120}}`
void applyMcpServers(agentxx::agent::AgentConfig& cfg, const utilxx_base::Json& mcp) {
    if (!mcp.is_object()) {
        return;
    }
    for (const auto& [ns, value] : mcp.items()) {
        if (!value.is_object()) {
            continue;
        }
        agentxx::agent::McpServerConfig server;
        server.url         = value.value("url", std::string{});
        server.toolTimeout = std::chrono::milliseconds(value.value("timeoutSec", int64_t{120}) * 1000);
        if (server.url.empty()) {
            XX_LOGW("[runtime] mcpServers['{}'] 缺少 url, 已忽略", std::string{ns});
            continue;
        }
        cfg.mcpServerUrls[std::string{ns}] = std::move(server);
    }
}

} // namespace

std::shared_ptr<agentxx::agent::AgentConfig> PluginManager::deriveRootConfig(
    const AgentxxPluginMountRootSpec& spec,
    const std::string&                rootId,
    const PluginInstance&             inst,
    std::string&                      err
) {
    utilxx_base::Json j = utilxx_base::Json::object();
    const std::string configJson
        = spec.config_json.data ? std::string{spec.config_json.data, static_cast<size_t>(spec.config_json.size)}
                                : std::string{};
    if (!configJson.empty()) {
        try {
            j = utilxx_base::Json::parse(configJson);
        } catch (const std::exception& e) {
            err = fmt::format("config_json 不是合法 JSON: {}", e.what());
            return nullptr;
        }
        if (!j.is_object()) {
            err = "config_json 必须是 JSON 对象";
            return nullptr;
        }
    }
    // 挂载策略只有一个来源: 混在 config_json 里以后没人知道该信哪一个
    if (j.contains("pluginMount") || j.contains("pluginMountPolicy")) {
        err = "config_json 不要给 pluginMount; 该根的插件挂载请用 spec.mount_plugins";
        return nullptr;
    }

    auto primary = primaryAgent_.lock();
    auto base    = primary && primary->agentConfig ? primary->agentConfig : nullptr;
    auto cfg     = std::make_shared<agentxx::agent::AgentConfig>();
    if (base) {
        // 只拷"解释器/上下文级"的东西: 模型表、语言、目录、权限默认值;
        // 提示词、工具白名单、插件表一律不拷 (那些是内置会话的)
        cfg->dataDir    = base->dataDir;
        cfg->model      = base->model;
        cfg->availableModels = base->availableModels;
        cfg->currentModelName = base->currentModelName;
        cfg->websearchModel   = base->websearchModel;
        cfg->websearchApiUrl  = base->websearchApiUrl;
        cfg->devMode          = base->devMode;

        const auto flags = inheritFlagsOf(j);
        if (flags.language) {
            cfg->language         = base->language;
            cfg->languageExplicit = base->languageExplicit;
        }
        if (flags.workDir) {
            cfg->workDir = base->workDir;
        }
        if (flags.permission) {
            cfg->permissionMode       = base->permissionMode;
            cfg->permissionAllowPaths = base->permissionAllowPaths;
            cfg->permissionDenyPaths  = base->permissionDenyPaths;
        }
        if (flags.prompt) {
            cfg->prompt = base->prompt;
        }
        if (flags.mcp) {
            cfg->mcpServerUrls = base->mcpServerUrls;
        }
        if (flags.skill) {
            cfg->skillDirPaths = base->skillDirPaths;
        }
        if (flags.memory) {
            cfg->memoryFilePaths = base->memoryFilePaths;
        }
        if (flags.rag) {
            cfg->ragDocsPaths = base->ragDocsPaths;
        }
        cfg->enableSessionStore = base->enableSessionStore;
        cfg->enableSummarization = base->enableSummarization;
        cfg->enableToolFiltering = false; // 白名单不继承
        cfg->toolWhitelist.clear();
    }
    // 挂载策略与插件表: 根不加载插件 (加载是进程级), 只决定挂载哪些
    cfg->plugins.clear();
    if (!applyMountPolicy(*cfg, svToSv(spec.mount_plugins), err)) {
        return nullptr;
    }

    // ---- 展示名 ----
    const std::string name = svToStr(spec.name);
    cfg->agentName         = name.empty() ? fmt::format("{}-{}", rootId, inst.name)
                                          : fmt::format("{}", name);
    cfg->agentNameView     = cfg->agentName;

    // ---- 逐字段覆盖 ----
    if (j.contains("agentName") && j["agentName"].is_string()) {
        cfg->agentName     = j["agentName"].get<std::string>();
        cfg->agentNameView = cfg->agentName;
    }
    if (j.contains("dataDir") && j["dataDir"].is_string() && base == nullptr) {
        cfg->dataDir = j["dataDir"].get<std::string>();
    }
    if (j.contains("language") && j["language"].is_string()) {
        cfg->language         = agentxx::agent::normalizeLanguage(j["language"].get<std::string>());
        cfg->languageExplicit = true;
    }
    if (j.contains("workDir") && j["workDir"].is_string()) {
        // 相对路径按进程工作目录解析: 与 client/FFI 加载侧同一规则
        const auto raw = utilxx_base::expandUserHomePath(j["workDir"].get<std::string>());
        if (!raw.empty()) {
            const std::filesystem::path wp{raw};
            cfg->workDir = wp.is_absolute()
                               ? wp.lexically_normal().generic_string()
                               : (std::filesystem::current_path() / wp).lexically_normal().generic_string();
        }
    }
    if (j.contains("toolWhitelist")) {
        cfg->toolWhitelist = stringArrayOf(j, "toolWhitelist");
    }
    if (j.contains("enableToolFiltering")) {
        cfg->enableToolFiltering = j.value("enableToolFiltering", false);
    }
    if (j.contains("prompt") && j["prompt"].is_object()) {
        applyPrompt(*cfg, j["prompt"], inst.name);
    }
    if (j.contains("permissionMode") && j["permissionMode"].is_string()) {
        cfg->permissionMode = permissionModeOf(j["permissionMode"].get<std::string>());
    }
    if (j.contains("permissionAllowPaths")) {
        cfg->permissionAllowPaths = stringArrayOf(j, "permissionAllowPaths");
    }
    if (j.contains("permissionDenyPaths")) {
        cfg->permissionDenyPaths = stringArrayOf(j, "permissionDenyPaths");
    }
    if (j.contains("skills")) {
        cfg->skillDirPaths = stringArrayOf(j, "skills");
    }
    if (j.contains("memoryFiles")) {
        cfg->memoryFilePaths = stringArrayOf(j, "memoryFiles");
    }
    if (j.contains("mcpServers")) {
        applyMcpServers(*cfg, j["mcpServers"]);
    }
    if (j.contains("llmMaxRetry") && j["llmMaxRetry"].is_number()) {
        cfg->llmMaxRetry = static_cast<size_t>(j["llmMaxRetry"].get<int64_t>());
    }
    if (j.contains("enableSummarization")) {
        cfg->enableSummarization = j.value("enableSummarization", true);
    }
    // 对外应答的根通常不需要子代理
    cfg->enableSubagent = j.value("enableSubagent", false);
    // 会话持久化: 默认沿用主程序的开关
    if (j.contains("enableSessionStore")) {
        cfg->enableSessionStore = j.value("enableSessionStore", cfg->enableSessionStore);
    }

    // ---- 模型 ----
    if (j.contains("model") && j["model"].is_string()) {
        const std::string want = j["model"].get<std::string>();
        auto              it   = cfg->availableModels.find(want);
        if (it == cfg->availableModels.end()) {
            err = fmt::format(
                "model '{}' 不在可用模型列表里 (可用: {})",
                want,
                [&cfg]() {
                    std::string names;
                    for (const auto& [key, value] : cfg->availableModels) {
                        (void)value;
                        if (!names.empty()) {
                            names += ", ";
                        }
                        names += key;
                    }
                    return names.empty() ? std::string{"<空>"} : names;
                }()
            );
            return nullptr;
        }
        cfg->model            = it->second;
        cfg->currentModelName = want;
    }

    // ---- 会话目录: 每个根独占一份, 避免两个 agent 打开同一份磁盘会话 ----
    if (j.contains("sessionStoreDirectory") && j["sessionStoreDirectory"].is_string()) {
        cfg->sessionStoreDirectory = j["sessionStoreDirectory"].get<std::string>();
    }
    if (cfg->sessionStoreDirectory.empty() && cfg->enableSessionStore) {
        if (!cfg->dataDir.empty()) {
            cfg->sessionStoreDirectory = (
                std::filesystem::path(
                    agentxx::agent::AgentConfigStatic::getSessionsDir(cfg->dataDir)
                )
                / rootId
            ).generic_string();
        } else {
            // 没有数据目录就没有可共享的会话根: 根退化为内存会话
            cfg->enableSessionStore = false;
            XX_LOGW(
                "[runtime] root '{}': dataDir 未配置, 会话不落盘 (内存会话)",
                rootId
            );
        }
    }

    // ---- 校验 (与 client/FFI 同一套规则) ----
    const auto report = cfg->validate();
    if (!report.has_value()) {
        err = report.error();
        return nullptr;
    }
    return cfg;
}

} // namespace plugin
} // namespace agentxx
