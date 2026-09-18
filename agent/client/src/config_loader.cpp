#include "agentxx-client/config_loader.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/util/exception.h"
#include "utilxx_base/container_util.h"
#include "utilxx_base/env.h"
#include "utilxx_base/string_util.h"
#include "yaml-cpp/yaml.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

namespace agentxx {
namespace client {

// ---------------------------------------------------------------------------
// .env loading
// ---------------------------------------------------------------------------

/// 打开文本文件输入流 (经 utf8ToPath: Windows 下非 ASCII 路径安全; 文本模式,
/// 与 yaml-cpp LoadFile / 原 std::ifstream 行为一致)
static std::ifstream openFileForRead(std::string_view path) {
    return std::ifstream(utilxx_base::utf8ToPath(path));
}

/// 加载 yaml 文件为根节点 (与 YAML::LoadFile 等价, 但打开文件走 utf8ToPath,
/// 兼容 Windows 非 ASCII 路径)
/// - 文件无法打开时抛出 std::runtime_error; 内容非法时抛出 yaml-cpp 解析异常
static YAML::Node loadYamlFile(std::string_view path) {
    auto file = openFileForRead(path);
    if (!file.is_open()) {
        throw std::runtime_error(fmt::format("yaml config file not found or not readable: {}", path)
        );
    }
    return YAML::Load(file);
}

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
    auto                               file = openFileForRead(path);
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
    auto                               file = openFileForRead(path);
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
// 内置变量存储已迁移至 utilxx_base::ApplicationEnv 单例 (全局预设变量, 优先级高于系统环境变量)
// - main 启动时经 setBuiltinEnvVar (= ApplicationEnv::instance().set) 注入 AGENTXX_WORK_DIR /
// AGENTXX_EXEC_DIR
// - 此处保留兼容层, 避免直接暴露 ApplicationEnv 细节给上层调用方

void setBuiltinEnvVar(std::string_view name, std::string value) {
    if (value.empty()) {
        utilxx_base::ApplicationEnv::instance().remove(name);
    } else {
        utilxx_base::ApplicationEnv::instance().set(name, std::move(value));
    }
}

/// 解析程序内置环境变量; 非内置变量返回 nullopt
/// - AGENTXX_WORK_DIR: 程序启动后的工作目录
///   (main 入口注入; 未注入时惰性回退 current_path())
/// - AGENTXX_EXEC_DIR: 可执行程序所在目录
///   (仅 main 入口注入; 未注入时无法惰性推导, 返回 nullopt 保留 ${VAR} 原样)
static std::optional<std::string> resolveBuiltinEnvVar(std::string_view varName) {
    // 已注入的内置变量: 直接取值 (经全局单例 ApplicationEnv 预设存储)
    if (auto preset = utilxx_base::ApplicationEnv::instance().getPreset(varName)) {
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
        // 系统环境变量 (经全局单例 ApplicationEnv 统一封装: 预设 -> 系统, Windows 使用 _dupenv_s
        // 消除 C4996)
        if (auto envVal = utilxx_base::ApplicationEnv::instance().get(varName)) {
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
static utilxx_base::Json yamlToJsonResolveEnv(
    const YAML::Node&                         node,
    const std::map<std::string, std::string>& dotEnvVars,
    const std::map<std::string, std::string>& overrideEnvVars
) {
    if (!node.IsDefined() || node.IsNull()) {
        return utilxx_base::Json{};
    }
    if (node.IsScalar()) {
        long long i;
        double    d;
        auto      s = resolveEnvVars(node.as<std::string>(), dotEnvVars, overrideEnvVars);
        if (s == "true") {
            return utilxx_base::Json(true);
        }
        if (s == "false") {
            return utilxx_base::Json(false);
        }
        if (utilxx_base::parseNumberFromString(s, i).ec == std::errc{}) {
            return utilxx_base::Json(i);
        }
        if (utilxx_base::parseNumberFromString(s, d).ec == std::errc{}) {
            return utilxx_base::Json(d);
        }
        return utilxx_base::Json(s);
    }
    if (node.IsSequence()) {
        utilxx_base::Json arr = utilxx_base::Json::array();
        for (const auto& item : node) {
            arr.push_back(yamlToJsonResolveEnv(item, dotEnvVars, overrideEnvVars));
        }
        return arr;
    }
    if (node.IsMap()) {
        utilxx_base::Json obj = utilxx_base::Json::object();
        for (const auto& kv : node) {
            obj[kv.first.as<std::string>()]
                = yamlToJsonResolveEnv(kv.second, dotEnvVars, overrideEnvVars);
        }
        return obj;
    }
    return utilxx_base::Json{};
}

static utilxx_base::Json yamlToJson(const YAML::Node& node) {
    if (!node.IsDefined() || node.IsNull()) {
        return utilxx_base::Json{};
    }
    if (node.IsScalar()) {
        long long i;
        double    d;
        // 注意: 必须用圆括号构造标量, 不能用花括号!
        // utilxx_base::Json 存在 json(std::initializer_list<json>) 构造函数,
        // C++ 花括号初始化优先匹配它, 导致标量被包成单元素数组:
        //   json{true} -> [true], json{"high"} -> ["high"]
        // 圆括号才能精确匹配 json(bool)/json(int)/json(double)/json(string) 标量构造。
        if (node.as<std::string>() == "true") {
            return utilxx_base::Json(true);
        }
        if (node.as<std::string>() == "false") {
            return utilxx_base::Json(false);
        }
        if (utilxx_base::parseNumberFromString(node.as<std::string>(), i).ec == std::errc{}) {
            return utilxx_base::Json(i);
        }
        if (utilxx_base::parseNumberFromString(node.as<std::string>(), d).ec == std::errc{}) {
            return utilxx_base::Json(d);
        }
        return utilxx_base::Json(node.as<std::string>());
    }
    if (node.IsSequence()) {
        utilxx_base::Json arr = utilxx_base::Json::array();
        for (const auto& item : node) {
            arr.push_back(yamlToJson(item));
        }
        return arr;
    }
    if (node.IsMap()) {
        utilxx_base::Json obj = utilxx_base::Json::object();
        for (const auto& kv : node) {
            obj[kv.first.as<std::string>()] = yamlToJson(kv.second);
        }
        return obj;
    }
    return utilxx_base::Json{};
}

// ---------------------------------------------------------------------------
// 配置节点访问辅助与段结构常量
// ---------------------------------------------------------------------------
// 只读访问一律经下方辅助函数: yaml-cpp 的非 const `operator[]` 会在键不存在时
// 就地插入空节点 (污染后续遍历), const 访问则返回未定义节点, 因此这里统一
// 使用 "取子节点失败返回未定义节点" 的只读语义。

/// 节点是否存在 (已定义且非 null; 默认构造的空节点同样视为不存在)
static bool isNodePresent(const YAML::Node& node) {
    return node.IsDefined() && !node.IsNull();
}

/// 读取映射子节点 (只读访问: 键不存在/非映射时返回未定义节点, 不修改传入节点)
/// - 必须先判 isNodePresent: 对未定义节点调用 IsMap()/Type() 会抛 InvalidNode
static YAML::Node mapChild(const YAML::Node& node, std::string_view key) {
    if (!isNodePresent(node) || !node.IsMap()) {
        return YAML::Node{};
    }
    return node[std::string{key}];
}

/// 取标量文本 (未定义/非标量返回空串)
static std::string nodeScalarText(const YAML::Node& node) {
    if (!isNodePresent(node) || !node.IsScalar()) {
        return {};
    }
    return node.as<std::string>();
}

/// 节点指纹 (列表去重用: 标量取文本, 其他取序列化文本; 未定义节点为空指纹)
static std::string nodeFingerprint(const YAML::Node& node) {
    if (!isNodePresent(node)) {
        return {};
    }
    if (node.IsScalar()) {
        return node.as<std::string>();
    }
    return YAML::Dump(node);
}

/// 列表段 (`list:`) 与策略块 (`overwrite:`) 的键名
inline constexpr std::string_view kSectionItemsKey     = "list";      ///< 段内条目
inline constexpr std::string_view kSectionOverwriteKey = "overwrite"; ///< 段内合并策略
inline constexpr std::string_view kSectionModeKey      = "mode";      ///< 策略: merge/replace
inline constexpr std::string_view kSectionRemoveKey    = "remove";    ///< 策略: 剔除条目
inline constexpr std::string_view kSectionUseKey       = "use"; ///< model 段: 用途模型名

// ---------------------------------------------------------------------------
// YAML config loading
// ---------------------------------------------------------------------------

/// 解析 yaml 根节点为应用配置 (单层; 分层合并见 loadYamlConfigLayered)
static YamlAppConfig parseYamlConfigNode(
    const YAML::Node&                         root,
    const std::map<std::string, std::string>& dotEnvVars,
    const std::map<std::string, std::string>& overrideEnvVars
) {
    YamlAppConfig cfg;

    // 模型列表 (归一化形状: `model.list`)
    const auto modelItems = mapChild(mapChild(root, "model"), kSectionItemsKey);
    if (modelItems.IsSequence()) {
        for (const auto& node : modelItems) {
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
                if (utilxx_base::parseNumberFromString(val, parsed).ec == std::errc{}) {
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
                if (utilxx_base::parseNumberFromString(val, parsed).ec == std::errc{}) {
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
                if (utilxx_base::parseNumberFromString(val, parsed).ec == std::errc{}) {
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
                if (utilxx_base::parseNumberFromString(val, parsed).ec == std::errc{}) {
                    mc.modelContenxtMaxToken = static_cast<size_t>(parsed);
                }
            }
            if (node["image_input"]) {
                mc.imageInput = utilxx_base::toLower(resolveEnvVars(
                                    (node["image_input"]).as<std::string>("false"),
                                    dotEnvVars,
                                    overrideEnvVars
                                ))
                                == "true";
            }
            if (node["audio_input"]) {
                mc.audioInput = utilxx_base::toLower(resolveEnvVars(
                                    (node["audio_input"]).as<std::string>("false"),
                                    dotEnvVars,
                                    overrideEnvVars
                                ))
                                == "true";
            }
            if (node["video_input"]) {
                mc.videoInput = utilxx_base::toLower(resolveEnvVars(
                                    (node["video_input"]).as<std::string>("false"),
                                    dotEnvVars,
                                    overrideEnvVars
                                ))
                                == "true";
            }
            if (node["extra_api_config"]) {
                mc.extraConfig = yamlToJson(node["extra_api_config"]);
            }
            if (!mc.name.empty()) {
                cfg.models[mc.name] = std::move(mc);
            }
        }
    }

    // 用途 → 模型名 (归一化形状: `model.use`; 原顶层 `use_model` 段)
    const auto useModelNode = mapChild(mapChild(root, "model"), kSectionUseKey);
    if (isNodePresent(useModelNode)) {
        auto useValue = [&](std::string_view key) {
            return resolveEnvVars(
                nodeScalarText(mapChild(useModelNode, key)),
                dotEnvVars,
                overrideEnvVars
            );
        };
        cfg.useModelDefault        = useValue("default");
        cfg.useModelSubagent       = useValue("subagent");
        cfg.useModelWebSearch      = useValue("web_search");
        cfg.useModelAcp            = useValue("acp");
        cfg.useModelTrain          = useValue("train");
        cfg.useModelTrainScorer    = useValue("train_scorer");
        cfg.useModelTrainOptimizer = useValue("train_optimizer");
    }

    // mcp 列表 (归一化形状: `mcp.list`)
    const auto mcpItems = mapChild(mapChild(root, "mcp"), kSectionItemsKey);
    if (mcpItems.IsSequence()) {
        for (const auto& node : mcpItems) {
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
                if (utilxx_base::parseNumberFromString(val, parsed).ec == std::errc{}) {
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

    // 技能目录 (归一化形状: `skill.list`)
    const auto skillItems = mapChild(mapChild(root, "skill"), kSectionItemsKey);
    if (skillItems.IsSequence()) {
        for (const auto& node : skillItems) {
            auto p = resolveEnvVars(node.as<std::string>(""), dotEnvVars, overrideEnvVars);
            if (!p.empty()) {
                cfg.skillDirPaths.push_back(std::move(p));
            }
        }
    }

    // 上下文文件 (归一化形状: `memory.list`)
    const auto memoryItems = mapChild(mapChild(root, "memory"), kSectionItemsKey);
    if (memoryItems.IsSequence()) {
        for (const auto& node : memoryItems) {
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

    // CodeGraph 参数已迁移到插件配置 (yaml `plugin.list` 段 agentxx_codegraph
    // 条目的 args): 宿主不解析其字段语义, 整体原样传递给插件

    // 权限配置 (permission 块: mode / whitelist / blacklist)
    // - mode:      询问处理模式 (ask/all_ask/pass/deny; 忽略大小写, 非法值警告回退 ask)
    // - whitelist/blacklist: 始终放行/拒绝的路径列表 (段结构 `list:` + `overwrite:`)
    const auto permissionNode = mapChild(root, "permission");
    if (isNodePresent(permissionNode)) {
        const auto permissionModeNode = mapChild(permissionNode, "mode");
        if (isNodePresent(permissionModeNode)) {
            auto val
                = resolveEnvVars(nodeScalarText(permissionModeNode), dotEnvVars, overrideEnvVars);
            if (utilxx_base::isIgnoreCaseEqual(val, "pass")) {
                cfg.permissionMode = agent::PermissionMode::Pass;
            } else if (utilxx_base::isIgnoreCaseEqual(val, "all_ask")) {
                cfg.permissionMode = agent::PermissionMode::AllAsk;
            } else if (utilxx_base::isIgnoreCaseEqual(val, "deny")) {
                cfg.permissionMode = agent::PermissionMode::Deny;
            } else if (utilxx_base::isIgnoreCaseEqual(val, "ask")) {
                cfg.permissionMode = agent::PermissionMode::Ask;
            } else {
                XX_LOGW("[Config] Warning: unknown permission.mode '{}', fallback to 'ask'", val);
            }
        }
        // 白/黑名单 (归一化形状: `permission.whitelist.list` / `blacklist.list`)
        const auto whitelistItems
            = mapChild(mapChild(permissionNode, "whitelist"), kSectionItemsKey);
        if (whitelistItems.IsSequence()) {
            for (const auto& node : whitelistItems) {
                auto p = resolveEnvVars(node.as<std::string>(""), dotEnvVars, overrideEnvVars);
                if (!p.empty()) {
                    cfg.permissionAllowPaths.push_back(std::move(p));
                }
            }
        }
        const auto blacklistItems
            = mapChild(mapChild(permissionNode, "blacklist"), kSectionItemsKey);
        if (blacklistItems.IsSequence()) {
            for (const auto& node : blacklistItems) {
                auto p = resolveEnvVars(node.as<std::string>(""), dotEnvVars, overrideEnvVars);
                if (!p.empty()) {
                    cfg.permissionDenyPaths.push_back(std::move(p));
                }
            }
        }
    }

    // 插件配置 (yaml `plugin.list` 条目: path / enabled / sides / args / config)
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
    // 插件列表 (归一化形状: `plugin.list`)
    const auto pluginItems = mapChild(mapChild(root, "plugin"), kSectionItemsKey);
    if (pluginItems.IsSequence()) {
        for (const auto& node : pluginItems) {
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
                XX_LOGW(
                    R"([Config] Warning: plugin entry missing required `path` (or `name` for builtin), skipped)"
                );
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
                if (utilxx_base::isIgnoreCaseEqual(val, "agent")) {
                    pc.sides = agent::PluginSide::Agent;
                } else if (utilxx_base::isIgnoreCaseEqual(val, "client")) {
                    pc.sides = agent::PluginSide::Client;
                } else if (!utilxx_base::isIgnoreCaseEqual(val, "auto")
                           && !utilxx_base::isIgnoreCaseEqual(val, "both")) {
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
                auto c = resolveEnvVars(
                    node["config"].as<std::string>(""),
                    dotEnvVars,
                    overrideEnvVars
                );
                pc.configPath = std::move(c);
            }
            cfg.plugins.push_back(std::move(pc));
        }
    }

    return cfg;
}

// ---------------------------------------------------------------------------
// 配置段合并 (列表段 `list:` + `overwrite:` 策略)
// ---------------------------------------------------------------------------
// 在 YAML 节点层合并 (而非 YamlAppConfig 结构层) 的原因: 节点层能区分
// "该层未配置此键" 与 "显式配置为默认值", 合并后再统一解析, 结构层的
// 默认值 (如 permissionMode 默认 Ask / enableSubagent 默认 true) 不会
// 误判为 overlay 已配置的值。

/// 列表段合并策略 (段内 `overwrite:` 块)
struct ListMergePolicy {
    bool replace = false;            ///< true: 整段替换 (不继承 base); false: 合并
    std::vector<std::string> remove; ///< 从合并结果中剔除的条目身份串
};

/// 归一化后的列表段读取结果
struct ListSection {
    ListMergePolicy policy;
    YAML::Node      items{YAML::NodeType::Sequence}; ///< 本层条目
    bool            present = false;                 ///< 本段在本层是否存在
};

/// 读取 `overwrite` 策略块 (非法写法记警告并回退默认: merge + 不删除)
static ListMergePolicy
    readListMergePolicy(const YAML::Node& overwriteNode, std::string_view sectionPath) {
    ListMergePolicy policy;
    if (!isNodePresent(overwriteNode)) {
        return policy;
    }
    if (!overwriteNode.IsMap()) {
        XX_LOGW(
            "[Config] Warning: `{}.overwrite` must be a map (`mode`/`remove`), ignored",
            sectionPath
        );
        return policy;
    }
    const auto modeNode = mapChild(overwriteNode, kSectionModeKey);
    if (isNodePresent(modeNode)) {
        auto mode = nodeScalarText(modeNode);
        if (mode == "replace") {
            policy.replace = true;
        } else if (mode == "merge" || mode.empty()) {
            policy.replace = false;
        } else {
            XX_LOGW(
                "[Config] Warning: unknown `{}.overwrite.mode` '{}', fallback to merge",
                sectionPath,
                mode
            );
        }
    }
    const auto removeNode = mapChild(overwriteNode, kSectionRemoveKey);
    if (isNodePresent(removeNode)) {
        if (!removeNode.IsSequence()) {
            XX_LOGW("[Config] Warning: `{}.overwrite.remove` must be a list, ignored", sectionPath);
        } else {
            for (const auto& item : removeNode) {
                auto text = nodeScalarText(item);
                if (!text.empty()) {
                    policy.remove.push_back(std::move(text));
                }
            }
        }
    }
    return policy;
}

/// 读取列表段 (段值必须是映射: `list:` 条目 + 可选 `overwrite:` 策略)
/// - 旧写法 (段值直接是列表, 如 `skill: [a, b]`) 与标量写法 (如 `skill: ""`)
///   已不再支持: 记警告并忽略该段
/// - `extraAllowedKeys`: 该段额外允许的键 (如 model 段的 `use`)
static ListSection readListSection(
    const YAML::Node&                    sectionNode,
    std::string_view                     sectionPath,
    const std::vector<std::string_view>& extraAllowedKeys = {}
) {
    ListSection section;
    if (!isNodePresent(sectionNode)) {
        return section;
    }
    if (!sectionNode.IsMap()) {
        XX_LOGW(
            "[Config] Warning: `{}` must be a map with `list:` (`overwrite:` optional); "
            "the plain list / scalar form is no longer supported, section ignored",
            sectionPath
        );
        return section;
    }
    section.present = true;
    section.policy  = readListMergePolicy(mapChild(sectionNode, kSectionOverwriteKey), sectionPath);

    // 未识别的键记警告 (帮助发现拼写错误, 如 `lis:`)
    for (const auto& kv : sectionNode) {
        auto key = nodeScalarText(kv.first);
        if (key == kSectionItemsKey || key == kSectionOverwriteKey) {
            continue;
        }
        bool allowed = false;
        for (const auto& extra : extraAllowedKeys) {
            if (key == extra) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            XX_LOGW("[Config] Warning: unknown key `{}.{}`, ignored", sectionPath, key);
        }
    }

    const auto itemsNode = mapChild(sectionNode, kSectionItemsKey);
    if (!isNodePresent(itemsNode)) {
        section.items = YAML::Node{YAML::NodeType::Sequence};
    } else if (itemsNode.IsSequence()) {
        section.items = itemsNode;
    } else {
        XX_LOGW("[Config] Warning: `{}.list` must be a list, treated as empty", sectionPath);
        section.items = YAML::Node{YAML::NodeType::Sequence};
    }
    return section;
}

/// 条目的身份标识串 (按键归并用): 第一个非空身份字段的值;
/// `builtinNameFallback` 时 `name` 视为 `builtin://<name>` (插件条目简写)
/// - 非映射条目 (如列表里的数字) 无法归并, 返回空串
static std::string nodeEntryId(
    const YAML::Node&               entry,
    const std::vector<std::string>& idKeys,
    bool                            builtinNameFallback
) {
    if (!entry.IsMap() || idKeys.empty()) {
        return {};
    }
    for (const auto& idKey : idKeys) {
        auto id = nodeScalarText(mapChild(entry, idKey));
        if (!id.empty()) {
            return id;
        }
    }
    if (builtinNameFallback) {
        auto name = nodeScalarText(mapChild(entry, "name"));
        if (!name.empty()) {
            return std::string{"builtin://"} + name;
        }
    }
    return {};
}

/// 条目的身份候选串 (用于 `overwrite.remove` 匹配): 标量条目取文本本身,
/// 映射条目取各身份字段值 (插件另接受 `builtin://<name>` 写法)
static std::vector<std::string> itemIdentities(
    const YAML::Node&               item,
    const std::vector<std::string>& idKeys,
    bool                            builtinNameFallback
) {
    std::vector<std::string> ids;
    if (item.IsScalar()) {
        ids.push_back(nodeScalarText(item));
        return ids;
    }
    if (!item.IsMap()) {
        return ids;
    }
    for (const auto& idKey : idKeys) {
        auto v = nodeScalarText(mapChild(item, idKey));
        if (!v.empty()) {
            ids.push_back(std::move(v));
        }
    }
    if (builtinNameFallback) {
        auto name = nodeScalarText(mapChild(item, "name"));
        if (!name.empty()) {
            // 插件条目: name 与 `builtin://<name>` 写法都能匹配到该条目
            ids.push_back(name);
            ids.push_back(std::string{"builtin://"} + name);
        }
    }
    return ids;
}

/// 通用节点合并 (标量/映射/列表): 映射逐键递归合并 (同键 overlay 覆盖),
/// 列表与标量整体覆盖
static YAML::Node mergeGenericNode(const YAML::Node& base, const YAML::Node& overlay) {
    if (!isNodePresent(overlay)) {
        return base;
    }
    if (!isNodePresent(base)) {
        return overlay;
    }
    if (base.IsMap() && overlay.IsMap()) {
        YAML::Node out{YAML::NodeType::Map};
        for (const auto& kv : base) {
            out[kv.first.as<std::string>()] = kv.second;
        }
        for (const auto& kv : overlay) {
            auto k = kv.first.as<std::string>();
            if (isNodePresent(out[k])) {
                out[k] = mergeGenericNode(out[k], kv.second);
            } else {
                out[k] = kv.second;
            }
        }
        return out;
    }
    return overlay;
}

/// 追加合并路径列表: base 项在前, overlay 中未出现过的项按原顺序追加在后
/// (按原始文本去重, 不展开 `${VAR}`)
static YAML::Node mergePathList(const YAML::Node& base, const YAML::Node& overlay) {
    YAML::Node               out{YAML::NodeType::Sequence};
    std::vector<std::string> seen;
    auto                     append = [&](const YAML::Node& item) {
        auto fp = nodeFingerprint(item);
        if (std::find(seen.begin(), seen.end(), fp) != seen.end()) {
            return;
        }
        seen.push_back(std::move(fp));
        out.push_back(item);
    };
    for (const auto& item : base) {
        append(item);
    }
    for (const auto& item : overlay) {
        append(item);
    }
    return out;
}

/// 按键归并列表: base 条目在前 (同键条目与 overlay 条目递归合并),
/// overlay 中未被取用的条目按原顺序追加在后
/// - 无标识的条目 (非映射/身份字段为空) 不参与归并, 各自保留原样
/// - overlay 自身出现重复键时全部保留 (与单层解析行为一致)
static YAML::Node mergeKeyedSequence(
    const YAML::Node&               base,
    const YAML::Node&               overlay,
    const std::vector<std::string>& idKeys,
    bool                            builtinNameFallback
) {
    std::vector<YAML::Node>  overlayItems;
    std::vector<std::string> overlayIds;
    std::vector<bool>        consumed;
    overlayItems.reserve(overlay.size());
    for (const auto& item : overlay) {
        overlayItems.push_back(item);
        overlayIds.push_back(nodeEntryId(item, idKeys, builtinNameFallback));
        consumed.push_back(false);
    }

    YAML::Node out{YAML::NodeType::Sequence};
    for (const auto& baseItem : base) {
        auto   bid = nodeEntryId(baseItem, idKeys, builtinNameFallback);
        size_t hit = overlayItems.size();
        if (!bid.empty()) {
            for (size_t i = 0; i < overlayItems.size(); ++i) {
                if (!consumed[i] && overlayIds[i] == bid) {
                    hit = i;
                    break;
                }
            }
        }
        if (hit == overlayItems.size()) {
            // overlay 无同键条目 (或无标识条目): 保留 base 条目
            out.push_back(baseItem);
            continue;
        }
        consumed[hit] = true;
        out.push_back(mergeGenericNode(baseItem, overlayItems[hit]));
    }
    for (size_t i = 0; i < overlayItems.size(); ++i) {
        if (!consumed[i]) {
            out.push_back(overlayItems[i]);
        }
    }
    return out;
}

/// 合并两个列表段的条目 (base 为底, overlay 覆盖) 并应用 overlay 的删除列表
/// - overlay 策略 `replace`: 只用 overlay 条目 (不继承 base)
/// - 否则按键归并 (idKeys 非空) 或追加去重 (idKeys 为空)
/// - `remove` 在两种模式下都从最终结果中剔除匹配项, 未匹配到的记警告
static YAML::Node mergeListSection(
    const ListSection&              base,
    const ListSection&              overlay,
    const std::vector<std::string>& idKeys,
    bool                            builtinNameFallback,
    std::string_view                sectionPath
) {
    YAML::Node merged{YAML::NodeType::Sequence};
    if (overlay.policy.replace) {
        for (const auto& item : overlay.items) {
            merged.push_back(item);
        }
        if (base.present && base.items.size() > 0) {
            XX_LOGI(
                "[Config] section `{}`: overwrite.mode=replace, {} inherited item(s) ignored",
                sectionPath,
                base.items.size()
            );
        }
    } else if (idKeys.empty()) {
        merged = mergePathList(base.items, overlay.items);
    } else {
        merged = mergeKeyedSequence(base.items, overlay.items, idKeys, builtinNameFallback);
    }

    const auto& remove = overlay.policy.remove;
    if (remove.empty()) {
        return merged;
    }

    YAML::Node        out{YAML::NodeType::Sequence};
    std::vector<bool> matched(remove.size(), false);
    for (const auto& item : merged) {
        auto   ids = itemIdentities(item, idKeys, builtinNameFallback);
        size_t hit = remove.size();
        for (size_t i = 0; i < remove.size() && hit == remove.size(); ++i) {
            for (const auto& id : ids) {
                if (id == remove[i]) {
                    hit = i;
                    break;
                }
            }
        }
        if (hit == remove.size()) {
            out.push_back(item);
            continue;
        }
        matched[hit] = true;
    }
    for (size_t i = 0; i < matched.size(); ++i) {
        if (!matched[i]) {
            XX_LOGW(
                "[Config] Warning: `{}.overwrite.remove` entry '{}' matched nothing",
                sectionPath,
                remove[i]
            );
        }
    }
    XX_LOGI(
        "[Config] section `{}`: remove applied, {} item(s) removed",
        sectionPath,
        merged.size() - out.size()
    );
    return out;
}

/// 列表段的合并 (返回归一化节点 `{list: [...]}`; 两层均无该段时返回空节点)
static YAML::Node mergeListSectionNode(
    const YAML::Node&                    baseSection,
    const YAML::Node&                    overlaySection,
    const std::vector<std::string>&      idKeys,
    bool                                 builtinNameFallback,
    std::string_view                     sectionPath,
    const std::vector<std::string_view>& extraAllowedKeys = {}
) {
    auto base    = readListSection(baseSection, sectionPath, extraAllowedKeys);
    auto overlay = readListSection(overlaySection, sectionPath, extraAllowedKeys);
    if (!base.present && !overlay.present) {
        return YAML::Node{};
    }
    YAML::Node out{YAML::NodeType::Map};
    out[std::string{kSectionItemsKey}]
        = mergeListSection(base, overlay, idKeys, builtinNameFallback, sectionPath);
    return out;
}

/// `model` 段合并: `list` 为模型列表 (按 `name` 归并), `use` 为各用途模型名 (逐键合并)
static YAML::Node
    mergeModelSection(const YAML::Node& baseSection, const YAML::Node& overlaySection) {
    const std::vector<std::string_view> extraAllowed{kSectionUseKey};
    auto                                base = readListSection(baseSection, "model", extraAllowed);
    auto       overlay    = readListSection(overlaySection, "model", extraAllowed);
    const auto baseUse    = mapChild(baseSection, kSectionUseKey);
    const auto overlayUse = mapChild(overlaySection, kSectionUseKey);
    if (!base.present && !overlay.present && !isNodePresent(baseUse)
        && !isNodePresent(overlayUse)) {
        return YAML::Node{};
    }
    YAML::Node out{YAML::NodeType::Map};
    if (base.present || overlay.present) {
        out[std::string{kSectionItemsKey}]
            = mergeListSection(base, overlay, {"name"}, false, "model");
    }
    auto use = mergeGenericNode(baseUse, overlayUse);
    if (isNodePresent(use)) {
        out[std::string{kSectionUseKey}] = use;
    }
    return out;
}

/// `permission` 段合并: `whitelist`/`blacklist` 走列表段规则, 其余键逐键合并
static YAML::Node
    mergePermissionSection(const YAML::Node& baseSection, const YAML::Node& overlaySection) {
    if (!isNodePresent(baseSection) && !isNodePresent(overlaySection)) {
        return YAML::Node{};
    }
    std::vector<std::string> keys;
    auto                     collectKeys = [&keys](const YAML::Node& node) {
        if (!isNodePresent(node) || !node.IsMap()) {
            return;
        }
        for (const auto& kv : node) {
            auto k = nodeScalarText(kv.first);
            if (!k.empty() && std::find(keys.begin(), keys.end(), k) == keys.end()) {
                keys.push_back(std::move(k));
            }
        }
    };
    collectKeys(baseSection);
    collectKeys(overlaySection);

    YAML::Node out{YAML::NodeType::Map};
    for (const auto& key : keys) {
        const auto baseChild    = mapChild(baseSection, key);
        const auto overlayChild = mapChild(overlaySection, key);
        YAML::Node merged;
        if (key == "whitelist" || key == "blacklist") {
            merged = mergeListSectionNode(
                baseChild,
                overlayChild,
                {},
                false,
                std::string{"permission."} + key
            );
        } else {
            merged = mergeGenericNode(baseChild, overlayChild);
        }
        if (isNodePresent(merged)) {
            out[key] = merged;
        }
    }
    return out;
}

/// 旧段名/旧位置提示 (models → model.list, plugins → plugin.list, use_model → model.use)
static void warnLegacySectionKeys(const YAML::Node& root) {
    if (!isNodePresent(root) || !root.IsMap()) {
        return;
    }
    if (isNodePresent(mapChild(root, "models"))) {
        XX_LOGW("[Config] Warning: legacy key `models` ignored (renamed): use `model.list`; "
                "`use_model` also moved to `model.use`");
    }
    if (isNodePresent(mapChild(root, "plugins"))) {
        XX_LOGW("[Config] Warning: legacy key `plugins` ignored (renamed): use `plugin.list`");
    }
    if (isNodePresent(mapChild(root, "use_model"))) {
        XX_LOGW("[Config] Warning: legacy key `use_model` ignored (moved): use `model.use`");
    }
}

/// 顶层键合并分派 (base 为底, overlay 覆盖)
static YAML::Node
    mergeTopLevelNode(std::string_view key, const YAML::Node& base, const YAML::Node& overlay) {
    if (key == "model") {
        return mergeModelSection(base, overlay);
    }
    if (key == "plugin") {
        return mergeListSectionNode(base, overlay, {"path"}, true, "plugin");
    }
    if (key == "mcp") {
        return mergeListSectionNode(base, overlay, {"namespace"}, false, "mcp");
    }
    if (key == "skill") {
        return mergeListSectionNode(base, overlay, {}, false, "skill");
    }
    if (key == "memory") {
        return mergeListSectionNode(base, overlay, {}, false, "memory");
    }
    if (key == "permission") {
        return mergePermissionSection(base, overlay);
    }
    return mergeGenericNode(base, overlay);
}

/// 合并两个配置文件的根节点 (base 为底, overlay 覆盖), 输出归一化节点
/// - 列表段统一归一化为 `{list: [...]}` (model 段另含 `use`), `overwrite` 策略不再保留
/// - 未识别的顶层键按通用规则合并 (标量覆盖 / 映射逐键合并 / 列表覆盖)
static YAML::Node mergeConfigRoots(const YAML::Node& base, const YAML::Node& overlay) {
    warnLegacySectionKeys(base);
    warnLegacySectionKeys(overlay);

    std::vector<std::string> keys;
    auto                     collectKeys = [&keys](const YAML::Node& node) {
        if (!isNodePresent(node) || !node.IsMap()) {
            return;
        }
        for (const auto& kv : node) {
            auto k = nodeScalarText(kv.first);
            if (!k.empty() && std::find(keys.begin(), keys.end(), k) == keys.end()) {
                keys.push_back(std::move(k));
            }
        }
    };
    collectKeys(base);
    collectKeys(overlay);

    YAML::Node out{YAML::NodeType::Map};
    for (const auto& key : keys) {
        if (key == "models" || key == "plugins" || key == "use_model") {
            continue; // 旧键: 已记迁移提示, 忽略
        }
        auto merged = mergeTopLevelNode(key, mapChild(base, key), mapChild(overlay, key));
        if (isNodePresent(merged)) {
            out[key] = merged;
        }
    }
    return out;
}

YamlAppConfig loadYamlConfig(
    std::string_view                          path,
    const std::map<std::string, std::string>& dotEnvVars,
    const std::map<std::string, std::string>& overrideEnvVars
) {
    // 单层加载: 与分层加载共用同一条归一化/合并路径 (base 层为空)
    return parseYamlConfigNode(
        mergeConfigRoots(YAML::Node{}, loadYamlFile(path)),
        dotEnvVars,
        overrideEnvVars
    );
}

YamlAppConfig loadYamlConfigLayered(
    std::string_view                          basePath,
    std::string_view                          overlayPath,
    const std::map<std::string, std::string>& dotEnvVars,
    const std::map<std::string, std::string>& overrideEnvVars
) {
    YAML::Node baseNode;
    YAML::Node overlayNode;
    if (!basePath.empty()) {
        baseNode = loadYamlFile(basePath);
    }
    if (!overlayPath.empty()) {
        overlayNode = loadYamlFile(overlayPath);
    }
    return parseYamlConfigNode(
        mergeConfigRoots(baseNode, overlayNode),
        dotEnvVars,
        overrideEnvVars
    );
}

// ---------------------------------------------------------------------------
// 分层配置加载 (base: data_dir 下的配置 + overlay: 工作目录/--config 指定的配置)
// ---------------------------------------------------------------------------

/// 判断路径是否指向已存在的文件
static bool pathIsFile(std::string_view path) {
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(utilxx_base::utf8ToPath(path), ec);
}

/// 判断两个路径是否指向同一位置 (展开 `~`、绝对化并词法规范化后比较;
/// 不解析符号链接, 对不存在的路径也安全)
static bool isSamePath(std::string_view a, std::string_view b) {
    if (a.empty() || b.empty()) {
        return false;
    }
    auto na = utilxx_base::toCurrentSystemAbsolutePath(a);
    auto nb = utilxx_base::toCurrentSystemAbsolutePath(b);
    return !na.empty() && na == nb;
}

std::string resolveDataDirValue(std::string_view raw) {
    if (raw.empty()) {
        return {};
    }
    if (raw == agent::AgentConfigStatic::kDefaultDataDirKey) {
        return agent::AgentConfigStatic::systemDataDir();
    }
    return utilxx_base::toCurrentSystemAbsolutePath(raw);
}

std::string readYamlDataDirValue(
    std::string_view                          path,
    const std::map<std::string, std::string>& dotEnvVars,
    const std::map<std::string, std::string>& overrideEnvVars
) {
    if (!pathIsFile(path)) {
        return {};
    }
    auto root = loadYamlFile(path);
    // 空文件/非映射根 (null、标量等) 视为未配置 data_dir
    if (!root.IsMap() || !root["data_dir"]) {
        return {};
    }
    return resolveEnvVars(root["data_dir"].as<std::string>(""), dotEnvVars, overrideEnvVars);
}

LayeredConfigLoad loadLayeredConfig(const LayeredConfigOptions& opts) {
    LayeredConfigLoad out;
    out.overlayConfigPath = opts.overlayConfigPath;

    // 1. --env 覆盖式环境变量 (最高优先级)
    if (!opts.overrideEnvPath.empty()) {
        out.overrideEnvVars = loadOverrideEnv(opts.overrideEnvPath);
    }
    // 2. overlay 的 .env (程序工作目录/配置所在目录; 后者覆盖前者)
    out.dotEnvVars = loadDotEnv(opts.overlayEnvPaths);

    // 3. 由 overlay 的 data_dir 定位 base 目录
    // - 此处只能用 overlay 自身的环境变量展开 (base .env 尚未加载);
    //   合并环境变量后的最终值在下面重新解析时生效
    // - 未配置 data_dir 或 overlay 配置不存在时使用系统数据目录:
    //   即"程序工作目录没有配置时, 加载数据目录下的配置"
    const bool  overlayExists = pathIsFile(opts.overlayConfigPath);
    std::string overlayDataDir;
    if (overlayExists) {
        overlayDataDir
            = readYamlDataDirValue(opts.overlayConfigPath, out.dotEnvVars, out.overrideEnvVars);
    }
    out.baseDir = overlayDataDir.empty() ? agent::AgentConfigStatic::systemDataDir()
                                         : resolveDataDirValue(overlayDataDir);
    // 路径拼接统一经 utf8ToPath/pathToUtf8Generic (Windows 下非 ASCII 路径安全),
    // 输出正斜杠格式
    out.baseConfigPath = utilxx_base::pathToUtf8Generic(
        utilxx_base::utf8ToPath(out.baseDir) / utilxx_base::utf8ToPath(kDefaultConfigFileName)
    );
    out.baseEnvPath = utilxx_base::pathToUtf8Generic(
        utilxx_base::utf8ToPath(out.baseDir) / utilxx_base::utf8ToPath(kDefaultEnvFileName)
    );

    // 4. base 与 overlay 指向同一文件时只加载一次 (如 --config <data_dir>/agentxx-config.yaml,
    //    或 data_dir 恰好指向 overlay 配置所在目录), 避免同一配置被当成两层重复合并
    const bool baseIsOverlay = isSamePath(out.baseConfigPath, opts.overlayConfigPath);

    // 5. base .env: 与 overlay .env 路径重叠时跳过 (已加载), 其余变量并入
    //    (同名变量舍弃 base 值, 保证 overlay 优先)
    if (!baseIsOverlay && pathIsFile(out.baseEnvPath)) {
        bool envAlreadyLoaded = false;
        for (const auto& p : opts.overlayEnvPaths) {
            if (isSamePath(p, out.baseEnvPath)) {
                envAlreadyLoaded = true;
                break;
            }
        }
        if (!envAlreadyLoaded) {
            auto baseEnv     = loadDotEnv(out.baseEnvPath);
            out.baseEnvTotal = baseEnv.size();
            for (auto& kv : baseEnv) {
                if (!out.dotEnvVars.try_emplace(kv.first, std::move(kv.second)).second) {
                    ++out.baseEnvDropped;
                }
            }
        }
    }

    // 6. 加载并合并配置: base 为底 (base 损坏时仅告警并忽略), overlay 覆盖
    const std::string basePath
        = (!baseIsOverlay && pathIsFile(out.baseConfigPath)) ? out.baseConfigPath : std::string{};
    out.baseLoaded    = !basePath.empty();
    out.overlayLoaded = overlayExists;

    YAML::Node baseNode;
    if (out.baseLoaded) {
        baseNode = agentxx::util::catchError<YAML::Node>(
            [&]() -> YAML::Node {
                return loadYamlFile(basePath);
            },
            [&](std::string errmsg) -> YAML::Node {
                // base 为可选层: 加载失败仅告警并跳过该层, 不影响 overlay 启动
                XX_LOGE(
                    "[Config] Failed to load base config: {}, {} (base layer skipped)",
                    basePath,
                    errmsg
                );
                out.baseLoaded = false;
                return YAML::Node{};
            }
        );
    }
    YAML::Node overlayNode;
    if (out.overlayLoaded) {
        // overlay 加载失败视为致命错误 (由调用方处理): 显式指定的配置不能用时
        // 不应静默回退到 base, 否则用户会误以为配置已生效
        overlayNode = loadYamlFile(opts.overlayConfigPath);
    }
    out.cfg = parseYamlConfigNode(
        mergeConfigRoots(baseNode, overlayNode),
        out.dotEnvVars,
        out.overrideEnvVars
    );
    return out;
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
