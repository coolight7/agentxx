#include "agentxx-client/config_loader.h"

#include "agentxx/util/container_util.h"
#include "agentxx/util/env.h"
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
        // .env 文件变量直接生效 (不查询系统环境变量):
        // 查找顺序为 内置 > --env > .env > 系统环境变量, .env 优先于系统环境变量
        vars[key] = value;
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
// 程序内置环境变量 (main 启动时注入; yaml ${VAR} 展开时优先解析)
// ---------------------------------------------------------------------------
// 内置变量存储已迁移至 agentxx::util::ApplicationEnv 单例 (全局预设变量, 优先级高于系统环境变量)
// - main 启动时经 setBuiltinEnvVar (= ApplicationEnv::instance().set) 注入 AGENTXX_WORK_DIR / AGENTXX_EXEC_DIR
// - 此处保留兼容层, 避免直接暴露 ApplicationEnv 细节给上层调用方

void setBuiltinEnvVar(std::string_view name, std::string value) {
    if (value.empty()) {
        agentxx::util::ApplicationEnv::instance().remove(name);
    } else {
        agentxx::util::ApplicationEnv::instance().set(name, std::move(value));
    }
}

/// 解析程序内置环境变量; 非内置变量返回 nullopt
/// - AGENTXX_WORK_DIR: 程序启动后的工作目录
///   (main 入口注入; 未注入时惰性回退 current_path())
/// - AGENTXX_EXEC_DIR: 可执行程序所在目录
///   (仅 main 入口注入; 未注入时无法惰性推导, 返回 nullopt 保留 ${VAR} 原样)
static std::optional<std::string> resolveBuiltinEnvVar(std::string_view varName) {
    // 已注入的内置变量: 直接取值 (经全局单例 ApplicationEnv 预设存储)
    if (auto preset = agentxx::util::ApplicationEnv::instance().getPreset(varName)) {
        return preset;
    }
    // 未注入 (测试/嵌入场景): 各内置变量按自身语义惰性解析
    if (varName == kBuiltinWorkDirEnv) {
        std::error_code ec;
        auto            cwd = std::filesystem::current_path(ec);
        if (ec) {
            XX_LOGW("[Config] resolve ${} failed: {}", varName, ec.message());
            return std::nullopt;
        }
        // 统一使用正斜杠 (generic_string): yaml 中 `${AGENTXX_WORK_DIR}/sub` 拼接
        // 不产生反斜杠转义问题 (Windows 原生路径含 `\`, 双引号 yaml 字符串中会转义)
        return cwd.generic_string();
    }
    return std::nullopt;
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

        // 程序内置环境变量优先 (如 AGENTXX_WORK_DIR: 程序启动后的工作目录)
        // - 内置变量由程序自身定义, 值恒定, 不应被环境变量/.env 覆盖
        if (auto builtinVal = resolveBuiltinEnvVar(varName)) {
            result.append(*builtinVal);
            continue;
        }
        // --env 覆盖式文件变量 (命令行显式指定, 最高优先级)
        auto ovIt = overrideEnvVars.find(varName);
        if (ovIt != overrideEnvVars.end()) {
            result.append(ovIt->second);
            continue;
        }
        // .env 文件变量 (优先于系统环境变量)
        auto dotIt = dotEnvVars.find(varName);
        if (dotIt != dotEnvVars.end()) {
            result.append(dotIt->second);
            continue;
        }
        // 系统环境变量 (经全局单例 ApplicationEnv 统一封装: 预设 -> 系统, Windows 使用 _dupenv_s 消除 C4996)
        if (auto envVal = agentxx::util::ApplicationEnv::instance().get(varName)) {
            result.append(*envVal);
            continue;
        }
        // 均未找到: 保留 ${VAR} 原样
        XX_LOGW("[config] model.key with `${{}}` but not value in .env: {}", varName);
        result.append(input, start, close - start + 1);
    }

    return result;
}

// ---------------------------------------------------------------------------
// YAML → JSON
// ---------------------------------------------------------------------------

/// YAML → JSON 并递归展开 ${VAR} (插件 args 专用)
/// - 标量先经 resolveEnvVars 展开再判断类型 (true/false/数字/字符串)
/// - 与 yamlToJson 语义一致, 仅多了 env 展开步骤
static neograph::json yamlToJsonResolveEnv(
    const YAML::Node&                         node,
    const std::map<std::string, std::string>& dotEnvVars,
    const std::map<std::string, std::string>& overrideEnvVars
) {
    if (!node.IsDefined() || node.IsNull()) {
        return neograph::json{};
    }
    if (node.IsScalar()) {
        int    i;
        double d;
        auto   s = resolveEnvVars(node.as<std::string>(), dotEnvVars, overrideEnvVars);
        if (s == "true") {
            return neograph::json(true);
        }
        if (s == "false") {
            return neograph::json(false);
        }
        if (util::parseNumberFromString(s, i).ec == std::errc{}) {
            return neograph::json(i);
        }
        if (util::parseNumberFromString(s, d).ec == std::errc{}) {
            return neograph::json(d);
        }
        return neograph::json(s);
    }
    if (node.IsSequence()) {
        neograph::json arr = neograph::json::array();
        for (const auto& item : node) {
            arr.push_back(yamlToJsonResolveEnv(item, dotEnvVars, overrideEnvVars));
        }
        return arr;
    }
    if (node.IsMap()) {
        neograph::json obj = neograph::json::object();
        for (const auto& kv : node) {
            obj[kv.first.as<std::string>()]
                = yamlToJsonResolveEnv(kv.second, dotEnvVars, overrideEnvVars);
        }
        return obj;
    }
    return neograph::json{};
}

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
            // sendThinking 开启时是否请求上游返回思考摘要 (Responses API 的 include
            // 参数): opencode-muse-spark 等网关不支持 reasoning.summary_text 变体,
            // 需设 false, 否则 API 400 (unknown variant reasoning.summary_text)
            if (node["request_reasoning_summary"]) {
                mc.requestReasoningSummary
                    = resolveEnvVars(
                          (node["request_reasoning_summary"]).as<std::string>("true"),
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
            if (node["max_concurrent_connections"]) {
                // 同上: 容错解析, 避免非法配置导致 std::stoull 抛异常崩溃
                auto val = resolveEnvVars(
                    node["max_concurrent_connections"].as<std::string>("5"),
                    dotEnvVars,
                    overrideEnvVars
                );
                unsigned long long parsed = 0;
                if (util::parseNumberFromString(val, parsed).ec == std::errc{}) {
                    mc.maxConcurrentConnections = static_cast<size_t>(parsed);
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
                mc.extraConfig = yamlToJson(node["extra_api_config"]);
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

    // 会话工作目录 (AgentConfig::workDir; 为空 = 使用进程当前工作目录)
    // - 相对路径的解析 (按程序工作目录) 与 `~` 展开由调用方完成, 与 data_dir 一致
    if (root["work_dir"]) {
        cfg.workDir
            = resolveEnvVars(root["work_dir"].as<std::string>(""), dotEnvVars, overrideEnvVars);
    }

    // subagent 开关 (yaml `subagent.enable`, 默认 true)
    if (root["subagent"] && root["subagent"].IsMap() && root["subagent"]["enable"]) {
        auto val = resolveEnvVars(
            root["subagent"]["enable"].as<std::string>("true"),
            dotEnvVars,
            overrideEnvVars
        );
        // 兼容 true/false, 1/0, yes/no, on/off
        std::string low = val;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) {
            return std::tolower(c);
        });
        if (low == "false" || low == "0" || low == "no" || low == "off") {
            cfg.enableSubagent = false;
        } else if (low == "true" || low == "1" || low == "yes" || low == "on") {
            cfg.enableSubagent = true;
        } else {
            XX_LOGW("[Config] Warning: unknown subagent.enable '{}', fallback to true", val);
        }
    }

    // git worktree 模式开关 (yaml `worktree.enable`, 默认 false)
    if (root["worktree"] && root["worktree"].IsMap() && root["worktree"]["enable"]) {
        auto val = resolveEnvVars(
            root["worktree"]["enable"].as<std::string>("false"),
            dotEnvVars,
            overrideEnvVars
        );
        std::string low = val;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) {
            return std::tolower(c);
        });
        if (low == "true" || low == "1" || low == "yes" || low == "on") {
            cfg.worktreeEnable = true;
        } else if (low == "false" || low == "0" || low == "no" || low == "off") {
            cfg.worktreeEnable = false;
        } else {
            XX_LOGW("[Config] Warning: unknown worktree.enable '{}', fallback to false", val);
        }
    }

    // CodeGraph 参数已迁移到插件配置 (yaml `plugins` 段 agentxx_codegraph
    // 条目的 args): 宿主不解析其字段语义, 整体原样传递给插件

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

    // 插件配置 (yaml `plugins` 列表项: path / enabled / sides / args / config)
    // - path: 插件动态库路径 或 插件目录 (含 plugin.yaml 时按清单分派加载);
    //   必填 (所有插件统一经 path 外置指定, 不区分内置/外置)
    //   特殊前缀 `builtin://<name>` 表示内置编译插件 (无需外部文件,
    //   直接经内置注册表加载; 如 `builtin://agentxx_filesystem`);
    //   兼容 `name` 字段简写: `{name: foo}` 等价于 `{path:
    //   "builtin://foo"}` (仅当 path 缺省且 name 对应内置插件时)
    // - enabled: 默认 true; sides: 运行侧 (auto/agent/client, 默认 auto)
    // - args: 插件参数 (整体传递给插件, 宿主不解析)
    // - config: 插件配置文件所在目录或文件路径 (可指向文件/目录;
    //   支持 `~`/`${VAR}`/相对路径, 由装配侧解析为绝对路径后透传给插件)
    if (root["plugins"] && root["plugins"].IsSequence()) {
        for (const auto& node : root["plugins"]) {
            if (!node.IsMap()) {
                XX_LOGW(R"([Config] Warning: plugins entry must be a map, skipped)");
                continue;
            }
            auto p = resolveEnvVars(node["path"].as<std::string>(""), dotEnvVars, overrideEnvVars);
            auto n = resolveEnvVars(node["name"].as<std::string>(""), dotEnvVars, overrideEnvVars);
            // 内置简写兼容: 仅 name 无 path 时, 若 name 对应内置插件则自动补为
            // builtin://<name> (与 path: builtin://<name> 等价)
            if (p.empty() && !n.empty()) {
                p = std::string("builtin://") + n;
            }
            agent::PluginConfig pc;
            pc.path = std::move(p);
            if (pc.path.empty()) {
                XX_LOGW(R"([Config] Warning: plugin entry missing required `path` (or `name` for builtin), skipped)");
                continue;
            }
            if (node["enabled"]) {
                auto val = resolveEnvVars(
                    node["enabled"].as<std::string>("true"),
                    dotEnvVars,
                    overrideEnvVars
                );
                pc.enabled = val == "true";
            }
            // sides: 插件运行侧 (auto/agent/client; 忽略大小写, 非法值警告回退 auto)
            // - auto:   按导出符号自动决定 (client 侧: 有 agentxx_plugin_client_create 才加载)
            // - agent:  仅 agent 侧加载 (client 侧跳过)
            // - client: 仅 client 侧加载 (agent 侧跳过)
            if (node["sides"]) {
                auto val = resolveEnvVars(
                    node["sides"].as<std::string>("auto"),
                    dotEnvVars,
                    overrideEnvVars
                );
                if (util::isIgnoreCaseEqual(val, "agent")) {
                    pc.sides = agent::PluginSide::Agent;
                } else if (util::isIgnoreCaseEqual(val, "client")) {
                    pc.sides = agent::PluginSide::Client;
                } else if (!util::isIgnoreCaseEqual(val, "auto")
                           && !util::isIgnoreCaseEqual(val, "both")) {
                    XX_LOGW(
                        R"([Config] Warning: plugin `{}` invalid sides `{}`, fallback to auto)",
                        pc.path,
                        val
                    );
                }
            }
            if (node["args"]) {
                // 插件参数整体传递 (宿主不解析字段语义); 标量递归展开 ${VAR}
                pc.args = yamlToJsonResolveEnv(node["args"], dotEnvVars, overrideEnvVars);
            }
            if (node["config"]) {
                // 插件配置文件所在目录或文件路径 (可指向文件/目录);
                // 支持 ${VAR} 展开 (路径归一化与绝对化由装配侧完成)
                auto c = resolveEnvVars(node["config"].as<std::string>(""), dotEnvVars, overrideEnvVars);
                pc.configPath = std::move(c);
            }
            cfg.plugins.push_back(std::move(pc));
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
