#include "agentxx-client/config_loader.h"

#include "agentxx/util/string_util.h"
#include "yaml-cpp/yaml.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>

namespace agentxx {
namespace client {

// ---------------------------------------------------------------------------
// .env loading
// ---------------------------------------------------------------------------

static bool parseEnvLine(std::string& line, std::string& key, std::string& value) {
    auto trim = [](std::string& s) {
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
    };
    trim(line);
    if (line.empty() || line[0] == '#') {
        return false;
    }
    if (line.rfind("export ", 0) == 0) {
        line = line.substr(7);
    }

    auto eq = line.find('=');
    if (eq == std::string::npos) {
        return false;
    }

    key = line.substr(0, eq);
    trim(key);

    value = line.substr(eq + 1);
    trim(value);

    if (value.size() >= 2
        && ((value[0] == '"' && value.back() == '"') || (value[0] == '\'' && value.back() == '\'')
        )) {
        value = value.substr(1, value.size() - 2);
    }
    return !key.empty();
}

std::map<std::string, std::string> loadDotEnv(std::string_view path) {
    std::map<std::string, std::string> vars;
    std::ifstream                      file(std::string{path});
    if (!file.is_open()) {
        return vars;
    }

    std::string line;
    while (std::getline(file, line)) {
        std::string key, value;
        if (!parseEnvLine(line, key, value)) {
            continue;
        }
        const char* existing = std::getenv(key.c_str());
        vars[key]            = existing ? std::string(existing) : value;
    }
    return vars;
}

std::map<std::string, std::string> loadOverrideEnv(std::string_view path) {
    std::map<std::string, std::string> vars;
    std::ifstream                      file(std::string{path});
    if (!file.is_open()) {
        return vars;
    }

    std::string line;
    while (std::getline(file, line)) {
        std::string key, value;
        if (!parseEnvLine(line, key, value)) {
            continue;
        }
        vars[key] = value;
    }
    return vars;
}

std::map<std::string, std::string> loadDotEnv(const std::vector<std::string>& paths) {
    std::map<std::string, std::string> merged;
    for (const auto& p : paths) {
        auto vars = loadDotEnv(p);
        for (const auto& kv : vars) {
            merged[kv.first] = kv.second;
        }
    }
    return merged;
}

// ---------------------------------------------------------------------------
// ${VAR} resolution
// ---------------------------------------------------------------------------

std::string resolveEnvVars(
    std::string_view                          input,
    const std::map<std::string, std::string>& dotEnvVars,
    const std::map<std::string, std::string>& overrideEnvVars
) {
    std::string result;
    result.reserve(input.size());
    size_t pos = 0;

    while (pos < input.size()) {
        auto start = input.find("${", pos);
        if (start == std::string::npos) {
            result.append(input, pos, std::string::npos);
            break;
        }
        result.append(input, pos, start - pos);

        auto close = input.find('}', start + 2);
        if (close == std::string::npos) {
            result.append(input, start, std::string::npos);
            break;
        }

        std::string varName{input.substr(start + 2, close - start - 2)};
        pos = close + 1;

        auto ovIt = overrideEnvVars.find(varName);
        if (ovIt != overrideEnvVars.end()) {
            result.append(ovIt->second);
            continue;
        }
        const char* envVal = std::getenv(varName.c_str());
        if (envVal != nullptr) {
            result.append(envVal);
            continue;
        }
        auto dotIt = dotEnvVars.find(varName);
        if (dotIt != dotEnvVars.end()) {
            result.append(dotIt->second);
            continue;
        }
        XX_LOGW("[config] model.key with `${{}}` but not value in .env: {}", varName);
        result.append(input, start, close - start + 1);
    }

    return result;
}

// ---------------------------------------------------------------------------
// YAML → JSON
// ---------------------------------------------------------------------------

static neograph::json yamlToJson(const YAML::Node& node) {
    if (!node.IsDefined() || node.IsNull()) {
        return neograph::json{};
    }
    if (node.IsScalar()) {
        int    i;
        double d;
        // 注意: 必须用圆括号构造标量, 不能用花括号!
        // neograph::json 存在 json(std::initializer_list<json>) 构造函数,
        // C++ 花括号初始化优先匹配它, 导致标量被包成单元素数组:
        //   json{true} -> [true], json{"high"} -> ["high"]
        // 圆括号才能精确匹配 json(bool)/json(int)/json(double)/json(string) 标量构造。
        if (node.as<std::string>() == "true") {
            return neograph::json(true);
        }
        if (node.as<std::string>() == "false") {
            return neograph::json(false);
        }
        if (util::parseNumberFromString(node.as<std::string>(), i).ec == std::errc{}) {
            return neograph::json(i);
        }
        if (util::parseNumberFromString(node.as<std::string>(), d).ec == std::errc{}) {
            return neograph::json(d);
        }
        return neograph::json(node.as<std::string>());
    }
    if (node.IsSequence()) {
        neograph::json arr = neograph::json::array();
        for (const auto& item : node) {
            arr.push_back(yamlToJson(item));
        }
        return arr;
    }
    if (node.IsMap()) {
        neograph::json obj = neograph::json::object();
        for (const auto& kv : node) {
            obj[kv.first.as<std::string>()] = yamlToJson(kv.second);
        }
        return obj;
    }
    return neograph::json{};
}

// ---------------------------------------------------------------------------
// YAML config loading
// ---------------------------------------------------------------------------

YamlAppConfig loadYamlConfig(
    std::string_view                          path,
    const std::map<std::string, std::string>& dotEnvVars,
    const std::map<std::string, std::string>& overrideEnvVars
) {
    YamlAppConfig cfg;
    auto          root = YAML::LoadFile(std::string{path});

    if (root["models"] && root["models"].IsSequence()) {
        for (const auto& node : root["models"]) {
            agent::ModelConfig mc;
            mc.name = resolveEnvVars(node["name"].as<std::string>(""), dotEnvVars, overrideEnvVars);
            mc.type = resolveEnvVars(
                node["type"].as<std::string>("openai"),
                dotEnvVars,
                overrideEnvVars
            );
            mc.baseUrl
                = resolveEnvVars(node["base_url"].as<std::string>(""), dotEnvVars, overrideEnvVars);
            mc.apiKey
                = resolveEnvVars(node["api_key"].as<std::string>(""), dotEnvVars, overrideEnvVars);
            mc.modelName = resolveEnvVars(
                node["model_name"].as<std::string>(""),
                dotEnvVars,
                overrideEnvVars
            );
            mc.apiPath
                = resolveEnvVars(node["api_path"].as<std::string>(""), dotEnvVars, overrideEnvVars);
            if (node["send_thinking"]) {
                mc.sendThinking = resolveEnvVars(
                                      (node["send_thinking"]).as<std::string>("false"),
                                      dotEnvVars,
                                      overrideEnvVars
                                  )
                                  == "true";
            }
            if (node["extra_headers"] && node["extra_headers"].IsMap()) {
                for (const auto& kv : node["extra_headers"]) {
                    auto k = kv.first.as<std::string>("");
                    if (k.empty()) {
                        continue;
                    }
                    mc.extraHeaders[k] = resolveEnvVars(
                        kv.second.as<std::string>(""),
                        dotEnvVars,
                        overrideEnvVars
                    );
                }
            }
            if (node["connect_timeout"]) {
                // std::stoi 对非法值抛异常会导致启动崩溃; 用容错解析, 非法时保留默认
                auto val = resolveEnvVars(
                    node["connect_timeout"].as<std::string>("16"),
                    dotEnvVars,
                    overrideEnvVars
                );
                int parsed = mc.connectTimeoutSeconds;
                if (util::parseNumberFromString(val, parsed).ec == std::errc{}) {
                    mc.connectTimeoutSeconds = parsed;
                }
            }
            if (node["read_chunk_timeout"]) {
                // 同上: 容错解析, 避免非法配置导致 std::stoi 抛异常崩溃
                auto val = resolveEnvVars(
                    node["read_chunk_timeout"].as<std::string>("60"),
                    dotEnvVars,
                    overrideEnvVars
                );
                int parsed = mc.readChunkTimeoutSeconds;
                if (util::parseNumberFromString(val, parsed).ec == std::errc{}) {
                    mc.readChunkTimeoutSeconds = parsed;
                }
            }
            if (node["ssl_verify"]) {
                auto val = resolveEnvVars(
                    node["ssl_verify"].as<std::string>(""),
                    dotEnvVars,
                    overrideEnvVars
                );
                if (val == "true") {
                    mc.sslVerify = true;
                } else if (val == "false") {
                    mc.sslVerify = false;
                }
            }
            if (node["model_context_max_token"]) {
                // 同上: 容错解析, 避免非法配置导致 std::stoull 抛异常崩溃
                auto val = resolveEnvVars(
                    node["model_context_max_token"].as<std::string>("0"),
                    dotEnvVars,
                    overrideEnvVars
                );
                unsigned long long parsed = 0;
                if (util::parseNumberFromString(val, parsed).ec == std::errc{}) {
                    mc.modelContenxtMaxToken = static_cast<size_t>(parsed);
                }
            }
            if (node["extra_api_config"]) {
                mc.extra_config = yamlToJson(node["extra_api_config"]);
            }
            if (!mc.name.empty()) {
                cfg.models[mc.name] = std::move(mc);
            }
        }
    }

    if (root["use_model"]) {
        cfg.useModelDefault = resolveEnvVars(
            root["use_model"]["default"].as<std::string>(""),
            dotEnvVars,
            overrideEnvVars
        );
        cfg.useModelSubagent = resolveEnvVars(
            root["use_model"]["subagent"].as<std::string>(""),
            dotEnvVars,
            overrideEnvVars
        );
        cfg.useModelWebSearch = resolveEnvVars(
            root["use_model"]["web_search"].as<std::string>(""),
            dotEnvVars,
            overrideEnvVars
        );
        cfg.useModelAcp = resolveEnvVars(
            root["use_model"]["acp"].as<std::string>(""),
            dotEnvVars,
            overrideEnvVars
        );
        cfg.useModelTrain = resolveEnvVars(
            root["use_model"]["train"].as<std::string>(""),
            dotEnvVars,
            overrideEnvVars
        );
        cfg.useModelTrainScorer = resolveEnvVars(
            root["use_model"]["train_scorer"].as<std::string>(""),
            dotEnvVars,
            overrideEnvVars
        );
        cfg.useModelTrainOptimizer = resolveEnvVars(
            root["use_model"]["train_optimizer"].as<std::string>(""),
            dotEnvVars,
            overrideEnvVars
        );
    }

    if (root["mcp"] && root["mcp"].IsSequence()) {
        for (const auto& node : root["mcp"]) {
            auto ns = resolveEnvVars(
                node["namespace"].as<std::string>(""),
                dotEnvVars,
                overrideEnvVars
            );
            auto url = resolveEnvVars(node["url"].as<std::string>(""), dotEnvVars, overrideEnvVars);
            if (ns.empty() || url.empty()) {
                XX_LOGW(R"([Config] Warning: mcp entry missing `namespace` or `url`, skipped)");
                continue;
            }
            agent::McpServerConfig mcpCfg;
            mcpCfg.url = url;
            // 工具调用超时 (秒, 0=不限制, 默认 120); 容错解析, 非法时保留默认
            if (node["timeout"]) {
                auto val = resolveEnvVars(
                    node["timeout"].as<std::string>("120"),
                    dotEnvVars,
                    overrideEnvVars
                );
                int parsed = 120;
                if (util::parseNumberFromString(val, parsed).ec == std::errc{}) {
                    mcpCfg.toolTimeout = std::chrono::seconds{std::max(parsed, 0)};
                }
            }
            if (cfg.mcpServers.contains(ns)) {
                XX_LOGW(
                    R"([Config] Warning: duplicate mcp namespace '{}', overriding its config)",
                    ns
                );
            }
            cfg.mcpServers[ns] = std::move(mcpCfg);
        }
    }

    if (root["skill"] && root["skill"].IsSequence()) {
        for (const auto& node : root["skill"]) {
            auto p = resolveEnvVars(node.as<std::string>(""), dotEnvVars, overrideEnvVars);
            if (!p.empty()) {
                cfg.skillDirPaths.push_back(std::move(p));
            }
        }
    }

    if (root["memory"] && root["memory"].IsSequence()) {
        for (const auto& node : root["memory"]) {
            auto p = resolveEnvVars(node.as<std::string>(""), dotEnvVars, overrideEnvVars);
            if (!p.empty()) {
                cfg.memoryFilePaths.push_back(std::move(p));
            }
        }
    }

    // 统一数据根目录 (全局设置/会话/codegraph 索引等数据存放根)
    // - 为空使用默认 ~/.agentxx/; 支持 ${VAR} 展开, `~` 展开由调用方完成
    if (root["data_dir"]) {
        cfg.dataDir
            = resolveEnvVars(root["data_dir"].as<std::string>(""), dotEnvVars, overrideEnvVars);
    }

    // CodeGraph 代码分析
    if (root["enable_codegraph"]) {
        cfg.enableCodeGraph = resolveEnvVars(
                                  root["enable_codegraph"].as<std::string>("false"),
                                  dotEnvVars,
                                  overrideEnvVars
                              )
                              == "true";
    }

    // 权限配置 (permission 块: mode / whitelist / blacklist)
    // - mode:      询问处理模式 (ask/all_ask/pass/deny; 忽略大小写, 非法值警告回退 ask)
    // - whitelist: 始终放行 (ALLOW) 的路径列表
    // - blacklist: 始终拒绝 (DENY) 的路径列表
    if (root["permission"]) {
        if (root["permission"]["mode"]) {
            auto val = resolveEnvVars(
                root["permission"]["mode"].as<std::string>("ask"),
                dotEnvVars,
                overrideEnvVars
            );
            if (util::isIgnoreCaseEqual(val, "pass")) {
                cfg.permissionMode = agent::PermissionMode::Pass;
            } else if (util::isIgnoreCaseEqual(val, "all_ask")) {
                cfg.permissionMode = agent::PermissionMode::AllAsk;
            } else if (util::isIgnoreCaseEqual(val, "deny")) {
                cfg.permissionMode = agent::PermissionMode::Deny;
            } else if (util::isIgnoreCaseEqual(val, "ask")) {
                cfg.permissionMode = agent::PermissionMode::Ask;
            } else {
                XX_LOGW("[Config] Warning: unknown permission.mode '{}', fallback to 'ask'", val);
            }
        }
        if (root["permission"]["whitelist"] && root["permission"]["whitelist"].IsSequence()) {
            for (const auto& node : root["permission"]["whitelist"]) {
                auto p = resolveEnvVars(node.as<std::string>(""), dotEnvVars, overrideEnvVars);
                if (!p.empty()) {
                    cfg.permissionAllowPaths.push_back(std::move(p));
                }
            }
        }
        if (root["permission"]["blacklist"] && root["permission"]["blacklist"].IsSequence()) {
            for (const auto& node : root["permission"]["blacklist"]) {
                auto p = resolveEnvVars(node.as<std::string>(""), dotEnvVars, overrideEnvVars);
                if (!p.empty()) {
                    cfg.permissionDenyPaths.push_back(std::move(p));
                }
            }
        }
    }

    return cfg;
}

// ---------------------------------------------------------------------------
// Model config helpers
// ---------------------------------------------------------------------------

agent::ModelConfig resolveModelConfig(
    const std::map<std::string, agent::ModelConfig>& models,
    std::string_view                                 modelName
) {
    if (modelName.empty()) {
        return agent::ModelConfig{};
    }
    auto it = models.find(std::string{modelName});
    if (it == models.end()) {
        XX_LOGE("[Config] Warning: model '{}' not found in config", modelName);
        return agent::ModelConfig{};
    }
    return it->second;
}

void applyModelToConfig(
    std::shared_ptr<agent::AgentConfig>              agentConfig,
    const std::map<std::string, agent::ModelConfig>& models,
    std::string_view                                 modelName
) {
    auto mc = resolveModelConfig(models, modelName);
    if (mc.isValid()) {
        agentConfig->model = std::move(mc);
    }
}

void applySubagentModelToConfig(
    std::shared_ptr<agent::AgentConfig>              agentConfig,
    const std::map<std::string, agent::ModelConfig>& models,
    std::string_view                                 modelName
) {
    auto mc = resolveModelConfig(models, modelName);
    if (mc.isValid()) {
        agentConfig->subagentModel = std::move(mc);
    }
}

void applyWebSearchModelToConfig(
    std::shared_ptr<agent::AgentConfig>              agentConfig,
    const std::map<std::string, agent::ModelConfig>& models,
    std::string_view                                 modelName
) {
    auto mc = resolveModelConfig(models, modelName);
    if (mc.isValid()) {
        agentConfig->websearchModel = std::move(mc);
    }
}

void applyAvailableModelsToConfig(
    std::shared_ptr<agent::AgentConfig>              agentConfig,
    const std::map<std::string, agent::ModelConfig>& models,
    std::string_view                                 currentModelName
) {
    for (const auto& [name, entry] : models) {
        auto mc = resolveModelConfig(models, name);
        if (mc.isValid()) {
            agentConfig->availableModels[name] = std::move(mc);
        }
    }
    agentConfig->currentModelName = currentModelName;
}

} // namespace client
} // namespace agentxx
