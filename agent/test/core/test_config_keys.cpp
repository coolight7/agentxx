/// 配置键目录与新鲜度校验 (计划 CFG-9)
///
/// 背景: yaml 配置的键只有 `client/src/config_loader.cpp` 一处权威实现, 用户/运维
/// 没有可读的键清单, 键名改动也没有检查。C++ 没有反射, 因此这里用"表 + 真实加载校验
/// + 源码扫描"三件套保证目录不会漂移:
///
/// 1. **表** (`configKeyEntries()`): 每个键登记 键路径 / 类型 / 默认值 / 样例 yaml /
///    观测函数; 校验时先按默认 (空配置或给定 base 层) 加载一次比对默认值, 再把样例
///    作为覆盖层加载一次比对样例值 —— 键被改名、默认值被改、解析失效都会失败;
/// 2. **源码扫描**: 从 `config_loader.cpp` 里扫出 `["键名"]` / `mapChild(..., "键")` /
///    `useValue("键")` / 段常量 (`list` / `overwrite` / `mode` / `remove` / `use`) 的键名
///    集合, 要求每个名字都能在目录里找到 (新增键必须登记), 并检查扫描量下限 (规则失效
///    时不能"零命中全通过");
/// 3. **生成物**: `agent/schema/config-keys.json` (机器可读) 与
///    `docs/zh-cn/design/config-keys.md` (人工阅读), 逐字节比对做检查,
///    `AGENTXX_UPDATE_CONFIG_KEYS=1` 一键更新 (更新后人工 review diff)。
#include "agentxx-test/core/test_config_keys.h"

#include "agentxx-test/core/schema_artifact.h"
#include "agentxx-client/config_loader.h"
#include "agentxx/agent/config.h"
#include "utilxx_base/json.h"
#include "utilxx_base/string_util.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见)
int g_ck_passed = 0;
int g_ck_failed = 0;
} // namespace

#define XX_TEST_PASSED g_ck_passed
#define XX_TEST_FAILED g_ck_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

using agentxx::client::YamlAppConfig;

namespace {

/// 观测函数: 把配置里的某个键"读成一个字符串"参与比对
using KeyProbe = std::function<std::string(const YamlAppConfig&)>;

/// 目录条目
///
/// 校验方式: 先用 [defaultDocYaml] (空 = 一份只有 `data_dir` 的最小文档) 加载一次
/// 比对默认值, 再把 [sampleYaml] 作为覆盖层或独立文档加载一次比对样例值。
struct ConfigKeyEntry {
    std::string keyPath;      ///< 键路径 (点分; 列表项用 `list[]`)
    std::string type;         ///< 类型 (文档用)
    std::string defaultValue; ///< 默认值 (文档用; 与 expectDefault 一致)
    std::string sampleYaml;   ///< 覆盖该键的 yaml 文档
    /// 默认值用例的文档; 空 = `data_dir: {}` (只有顶层键才有意义); 条目级键
    /// 用"最小条目"文档 (如只写 `- name: m1`), 这样读到的是该键省略时的取值
    std::string defaultDocYaml;
    /// true = 样例作为 overlay 叠在 [defaultDocYaml] 上 (两层合并语义, 如 `overwrite.*`)
    bool        sampleIsOverlay = false;
    std::string expectDefault;
    std::string expectSample;
    KeyProbe    probe;
};

std::string boolText(bool v) {
    return v ? "true" : "false";
}

/// 去掉首尾空白 (生成物里嵌入样例 yaml 时用)
std::string trimText(std::string_view text) {
    auto isBlank = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    size_t begin = 0;
    size_t end   = text.size();
    while (begin < end && isBlank(text[begin])) {
        ++begin;
    }
    while (end > begin && isBlank(text[end - 1])) {
        --end;
    }
    return std::string{text.substr(begin, end - begin)};
}

std::string joinList(const std::vector<std::string>& items, std::string_view sep = "|") {
    std::string out;
    for (const auto& item : items) {
        if (!out.empty()) {
            out += sep;
        }
        out += item;
    }
    return out;
}

/// 取指定名字的模型条目字段 (缺失返回 `<missing>`)
template<typename F>
std::string modelField(const YamlAppConfig& cfg, std::string_view name, F&& get) {
    const auto it = cfg.models.find(std::string{name});
    if (it == cfg.models.end()) {
        return "<missing>";
    }
    return get(it->second);
}

/// 模型条目字段探针 (固定取模型名 `m1` 的条目)
template<typename F>
KeyProbe modelProbe(F&& get) {
    return [get](const YamlAppConfig& cfg) -> std::string {
        return modelField(cfg, "m1", get);
    };
}

/// 取指定路径的插件条目字段
template<typename F>
KeyProbe pluginProbe(std::string_view path, F&& get) {
    return [path, get](const YamlAppConfig& cfg) -> std::string {
        for (const auto& p : cfg.plugins) {
            if (p.path == path) {
                return get(p);
            }
        }
        return "<missing>";
    };
}

/// 插件运行侧文本
std::string pluginSideText(agentxx::agent::PluginSide side) {
    switch (side) {
        case agentxx::agent::PluginSide::Agent:
            return "agent";
        case agentxx::agent::PluginSide::Client:
            return "client";
        case agentxx::agent::PluginSide::Auto:
        default:
            return "auto";
    }
}

/// MCP 超时 (毫秒)
template<typename F>
KeyProbe mcpProbe(std::string_view ns, F&& get) {
    return [ns, get](const YamlAppConfig& cfg) -> std::string {
        const auto it = cfg.mcpServers.find(std::string{ns});
        if (it == cfg.mcpServers.end()) {
            return "<missing>";
        }
        return get(it->second);
    };
}

// ---------------------------------------------------------------------------
// 键目录
// ---------------------------------------------------------------------------

/// 两层合并用例的 base 层 (给 `overwrite.*` 用): 两个模型 + 两个技能目录
const char* const kMergeBaseYaml = R"(model:
  list:
    - name: m1
      type: "openai"
    - name: m2
      type: "openai"
skill:
  list: ["./skills-a", "./skills-b"]
)";

std::vector<ConfigKeyEntry> configKeyEntries() {
    std::vector<ConfigKeyEntry> out;

    // 条目级键的"最小条目"文档: 只写必需字段, 用来观察其余键省略时的取值
    const std::string kMinimalModelYaml  = "model:\n  list:\n    - name: m1\n";
    const std::string kMinimalPluginYaml
        = "plugin:\n  list:\n    - path: \"builtin://agentxx_filesystem\"\n";
    const std::string kMinimalMcpYaml
        = "mcp:\n  list:\n    - namespace: \"tools\"\n      url: \"http://127.0.0.1:9/sse\"\n";

    // ---- 顶层标量 ----
    out.push_back(
        {"data_dir",
         "string",
         "空 (不持久化: 数据仅存内存)",
         "data_dir: cfg-keys-data\n",
         "",
         false,
         "",
         "cfg-keys-data",
         [](const YamlAppConfig& cfg) { return cfg.dataDir; }}
    );
    out.push_back(
        {"work_dir",
         "string",
         "空 (用进程当前工作目录)",
         "work_dir: cfg-keys-work\n",
         "",
         false,
         "",
         "cfg-keys-work",
         [](const YamlAppConfig& cfg) { return cfg.workDir; }}
    );
    out.push_back(
        {"language",
         "string (zh-cn | en)",
         "空 (由客户端界面语言决定)",
         "language: ZH-CN\n",
         "",
         false,
         "",
         "zh-cn",
         [](const YamlAppConfig& cfg) { return cfg.language; }}
    );
    out.push_back(
        {"subagent.enable",
         "bool",
         "true",
         "subagent:\n  enable: false\n",
         "",
         false,
         "true",
         "false",
         [](const YamlAppConfig& cfg) { return boolText(cfg.enableSubagent); }}
    );
    out.push_back(
        {"worktree.enable",
         "bool",
         "false",
         "worktree:\n  enable: true\n",
         "",
         false,
         "false",
         "true",
         [](const YamlAppConfig& cfg) { return boolText(cfg.worktreeEnable); }}
    );
    out.push_back(
        {"permission.mode",
         "enum (ask | all_ask | pass | deny)",
         "ask",
         "permission:\n  mode: deny\n",
         "",
         false,
         "ask",
         "deny",
         [](const YamlAppConfig& cfg) {
             return std::string{client::permissionModeName(cfg.permissionMode)};
         }}
    );
    out.push_back(
        {"permission.whitelist.list",
         "list<string>",
         "空",
         "permission:\n  whitelist:\n    list: [\"/data/a\", \"./b\"]\n",
         "",
         false,
         "",
         "/data/a|./b",
         [](const YamlAppConfig& cfg) { return joinList(cfg.permissionAllowPaths); }}
    );
    out.push_back(
        {"permission.blacklist.list",
         "list<string>",
         "空",
         "permission:\n  blacklist:\n    list: [\"./secret\"]\n",
         "",
         false,
         "",
         "./secret",
         [](const YamlAppConfig& cfg) { return joinList(cfg.permissionDenyPaths); }}
    );
    out.push_back(
        {"skill.list",
         "list<string>",
         "空",
         "skill:\n  list: [\"./skills\", \"./more\"]\n",
         "",
         false,
         "",
         "./skills|./more",
         [](const YamlAppConfig& cfg) { return joinList(cfg.skillDirPaths); }}
    );
    out.push_back(
        {"memory.list",
         "list<string>",
         "空",
         "memory:\n  list: [\"./AGENTS.md\"]\n",
         "",
         false,
         "",
         "./AGENTS.md",
         [](const YamlAppConfig& cfg) { return joinList(cfg.memoryFilePaths); }}
    );

    // ---- model 段: 用途模型名 ----
    out.push_back(
        {"model.use.default",
         "string",
         "空",
         "model:\n  use:\n    default: m1\n",
         "",
         false,
         "",
         "m1",
         [](const YamlAppConfig& cfg) { return cfg.useModelDefault; }}
    );
    out.push_back(
        {"model.use.subagent",
         "string",
         "空",
         "model:\n  use:\n    subagent: m1\n",
         "",
         false,
         "",
         "m1",
         [](const YamlAppConfig& cfg) { return cfg.useModelSubagent; }}
    );
    out.push_back(
        {"model.use.web_search",
         "string",
         "空",
         "model:\n  use:\n    web_search: m1\n",
         "",
         false,
         "",
         "m1",
         [](const YamlAppConfig& cfg) { return cfg.useModelWebSearch; }}
    );
    out.push_back(
        {"model.use.acp",
         "string",
         "空",
         "model:\n  use:\n    acp: m1\n",
         "",
         false,
         "",
         "m1",
         [](const YamlAppConfig& cfg) { return cfg.useModelAcp; }}
    );
    out.push_back(
        {"model.use.train",
         "string",
         "空",
         "model:\n  use:\n    train: m1\n",
         "",
         false,
         "",
         "m1",
         [](const YamlAppConfig& cfg) { return cfg.useModelTrain; }}
    );
    out.push_back(
        {"model.use.train_scorer",
         "string",
         "空",
         "model:\n  use:\n    train_scorer: m1\n",
         "",
         false,
         "",
         "m1",
         [](const YamlAppConfig& cfg) { return cfg.useModelTrainScorer; }}
    );
    out.push_back(
        {"model.use.train_optimizer",
         "string",
         "空",
         "model:\n  use:\n    train_optimizer: m1\n",
         "",
         false,
         "",
         "m1",
         [](const YamlAppConfig& cfg) { return cfg.useModelTrainOptimizer; }}
    );

    // ---- model 段: 条目字段 ----
    out.push_back(
        {"model.list[].name",
         "string (必填)",
         "无 (缺 name 的条目被跳过)",
         "model:\n  list:\n    - name: m9\n      type: \"openai\"\n",
         kMinimalModelYaml,
         false,
         "m1",
         "m9",
         [](const YamlAppConfig& cfg) {
             std::vector<std::string> names;
             for (const auto& [name, mc] : cfg.models) {
                 names.push_back(name);
             }
             std::sort(names.begin(), names.end());
             return joinList(names, ",");
         }}
    );
    out.push_back(
        {"model.list[].type",
         "string (openai | anthropic | openai-responses)",
         "openai",
         "model:\n  list:\n    - name: m1\n      type: \"anthropic\"\n",
         kMinimalModelYaml,
         false,
         "openai",
         "anthropic",
         modelProbe([](const agentxx::agent::ModelConfig& m) { return m.type; })}
    );
    out.push_back(
        {"model.list[].base_url",
         "string",
         "空 (用 provider 默认官方地址)",
         "model:\n  list:\n    - name: m1\n      base_url: \"https://api.example.com\"\n",
         kMinimalModelYaml,
         false,
         "",
         "https://api.example.com",
         modelProbe([](const agentxx::agent::ModelConfig& m) { return m.baseUrl; })}
    );
    out.push_back(
        {"model.list[].api_key",
         "string (支持 ${VAR} 展开)",
         "空",
         "model:\n  list:\n    - name: m1\n      api_key: \"EMPTY\"\n",
         kMinimalModelYaml,
         false,
         "",
         "EMPTY",
         modelProbe([](const agentxx::agent::ModelConfig& m) { return m.apiKey; })}
    );
    out.push_back(
        {"model.list[].model_name",
         "string",
         "空",
         "model:\n  list:\n    - name: m1\n      model_name: \"gpt-4\"\n",
         kMinimalModelYaml,
         false,
         "",
         "gpt-4",
         modelProbe([](const agentxx::agent::ModelConfig& m) { return m.modelName; })}
    );
    out.push_back(
        {"model.list[].api_path",
         "string",
         "空 (按 type 用默认路径)",
         "model:\n  list:\n    - name: m1\n      api_path: \"/v1/chat/completions\"\n",
         kMinimalModelYaml,
         false,
         "",
         "/v1/chat/completions",
         modelProbe([](const agentxx::agent::ModelConfig& m) { return m.apiPath; })}
    );
    out.push_back(
        {"model.list[].send_thinking",
         "bool",
         "false",
         "model:\n  list:\n    - name: m1\n      send_thinking: true\n",
         kMinimalModelYaml,
         false,
         "false",
         "true",
         modelProbe([](const agentxx::agent::ModelConfig& m) { return boolText(m.sendThinking); })}
    );
    out.push_back(
        {"model.list[].request_reasoning_summary",
         "bool",
         "true",
         "model:\n  list:\n    - name: m1\n      request_reasoning_summary: false\n",
         kMinimalModelYaml,
         false,
         "true",
         "false",
         modelProbe([](const agentxx::agent::ModelConfig& m) {
             return boolText(m.requestReasoningSummary);
         })}
    );
    out.push_back(
        {"model.list[].cache_control",
         "bool (仅 anthropic 生效)",
         "false",
         "model:\n  list:\n    - name: m1\n      cache_control: true\n",
         kMinimalModelYaml,
         false,
         "false",
         "true",
         modelProbe([](const agentxx::agent::ModelConfig& m) { return boolText(m.cacheControl); })}
    );
    out.push_back(
        {"model.list[].connect_timeout",
         "int (秒)",
         "16",
         "model:\n  list:\n    - name: m1\n      connect_timeout: 3\n",
         kMinimalModelYaml,
         false,
         "16",
         "3",
         modelProbe([](const agentxx::agent::ModelConfig& m) {
             return std::to_string(m.connectTimeoutSeconds);
         })}
    );
    out.push_back(
        {"model.list[].read_chunk_timeout",
         "int (秒)",
         "60",
         "model:\n  list:\n    - name: m1\n      read_chunk_timeout: 7\n",
         kMinimalModelYaml,
         false,
         "60",
         "7",
         modelProbe([](const agentxx::agent::ModelConfig& m) {
             return std::to_string(m.readChunkTimeoutSeconds);
         })}
    );
    out.push_back(
        {"model.list[].ssl_verify",
         "bool (省略 = 未指定, 用全局默认策略)",
         "未指定 (nullopt)",
         "model:\n  list:\n    - name: m1\n      ssl_verify: false\n",
         kMinimalModelYaml,
         false,
         "unset",
         "false",
         modelProbe([](const agentxx::agent::ModelConfig& m) {
             if (!m.sslVerify.has_value()) {
                 return std::string{"unset"};
             }
             return boolText(*m.sslVerify);
         })}
    );
    out.push_back(
        {"model.list[].max_concurrent_connections",
         "int (0 = 不限制)",
         "5",
         "model:\n  list:\n    - name: m1\n      max_concurrent_connections: 2\n",
         kMinimalModelYaml,
         false,
         "5",
         "2",
         modelProbe([](const agentxx::agent::ModelConfig& m) {
             return std::to_string(m.maxConcurrentConnections);
         })}
    );
    out.push_back(
        {"model.list[].model_context_max_token",
         "int (0 = 未指定)",
         "0",
         "model:\n  list:\n    - name: m1\n      model_context_max_token: 128000\n",
         kMinimalModelYaml,
         false,
         "0",
         "128000",
         modelProbe([](const agentxx::agent::ModelConfig& m) {
             return std::to_string(m.modelContextMaxToken);
         })}
    );
    out.push_back(
        {"model.list[].image_input",
         "bool",
         "false",
         "model:\n  list:\n    - name: m1\n      image_input: true\n",
         kMinimalModelYaml,
         false,
         "false",
         "true",
         modelProbe([](const agentxx::agent::ModelConfig& m) { return boolText(m.imageInput); })}
    );
    out.push_back(
        {"model.list[].audio_input",
         "bool",
         "false",
         "model:\n  list:\n    - name: m1\n      audio_input: true\n",
         kMinimalModelYaml,
         false,
         "false",
         "true",
         modelProbe([](const agentxx::agent::ModelConfig& m) { return boolText(m.audioInput); })}
    );
    out.push_back(
        {"model.list[].video_input",
         "bool",
         "false",
         "model:\n  list:\n    - name: m1\n      video_input: true\n",
         kMinimalModelYaml,
         false,
         "false",
         "true",
         modelProbe([](const agentxx::agent::ModelConfig& m) { return boolText(m.videoInput); })}
    );
    out.push_back(
        {"model.list[].extra_headers",
         "map<string, string>",
         "空",
         "model:\n  list:\n    - name: m1\n      extra_headers:\n        x-custom: v\n",
         kMinimalModelYaml,
         false,
         "",
         "x-custom=v",
         modelProbe([](const agentxx::agent::ModelConfig& m) {
             std::string text;
             for (const auto& [key, value] : m.extraHeaders) {
                 if (!text.empty()) {
                     text += "|";
                 }
                 text += key + "=" + value;
             }
             return text;
         })}
    );
    out.push_back(
        {"model.list[].extra_api_config",
         "map<string, any> (合并进请求体)",
         "空",
         "model:\n  list:\n    - name: m1\n      extra_api_config:\n        temperature: 0.7\n",
         kMinimalModelYaml,
         false,
         "",
         R"({"temperature":0.7})",
         modelProbe([](const agentxx::agent::ModelConfig& m) {
             return m.extraConfig.is_null() ? std::string{} : m.extraConfig.dump();
         })}
    );

    // ---- model 段: 两层合并策略 (只在覆盖层生效) ----
    out.push_back(
        {"model.overwrite.mode",
         "enum (merge | replace)",
         "merge (继承并叠加 base 层)",
         "model:\n  overwrite:\n    mode: replace\n  list:\n    - name: m3\n      type: "
         "\"openai\"\n",
         kMergeBaseYaml,
         true,
         "m1,m2",
         "m3",
         [](const YamlAppConfig& cfg) {
             std::vector<std::string> names;
             for (const auto& [name, mc] : cfg.models) {
                 names.push_back(name);
             }
             std::sort(names.begin(), names.end());
             return joinList(names, ",");
         }}
    );
    out.push_back(
        {"model.overwrite.remove",
         "list<string> (按 name 剔除 base 项)",
         "空 (不剔除)",
         "model:\n  overwrite:\n    remove: [m1]\n",
         kMergeBaseYaml,
         true,
         "m1,m2",
         "m2",
         [](const YamlAppConfig& cfg) {
             std::vector<std::string> names;
             for (const auto& [name, mc] : cfg.models) {
                 names.push_back(name);
             }
             std::sort(names.begin(), names.end());
             return joinList(names, ",");
         }}
    );

    // ---- plugin 段 ----
    out.push_back(
        {"plugin.list[].path",
         "string (必填; 支持 builtin://<名字>)",
         "无 (缺 path/name 的条目被跳过)",
         "plugin:\n  list:\n    - path: \"builtin://agentxx_filesystem\"\n",
         kMinimalPluginYaml,
         false,
         "builtin://agentxx_filesystem",
         "builtin://agentxx_filesystem",
         [](const YamlAppConfig& cfg) {
             return cfg.plugins.empty() ? std::string{"<empty>"} : cfg.plugins.front().path;
         }}
    );
    out.push_back(
        {"plugin.list[].name",
         "string (内置插件简写, 等价 builtin://<name>)",
         "空 (缺 path 时按内置名补齐)",
         "plugin:\n  list:\n    - name: agentxx_filesystem\n",
         "",
         false,
         "<empty>",
         "builtin://agentxx_filesystem",
         [](const YamlAppConfig& cfg) {
             return cfg.plugins.empty() ? std::string{"<empty>"} : cfg.plugins.front().path;
         }}
    );
    out.push_back(
        {"plugin.list[].enabled",
         "bool",
         "true",
         "plugin:\n  list:\n    - path: \"builtin://agentxx_filesystem\"\n      enabled: false\n",
         kMinimalPluginYaml,
         false,
         "true",
         "false",
         pluginProbe("builtin://agentxx_filesystem", [](const agentxx::agent::PluginConfig& p) {
             return boolText(p.enabled);
         })}
    );
    out.push_back(
        {"plugin.list[].sides",
         "enum (auto | agent | client)",
         "auto",
         "plugin:\n  list:\n    - path: \"builtin://agentxx_filesystem\"\n      sides: agent\n",
         kMinimalPluginYaml,
         false,
         "auto",
         "agent",
         pluginProbe("builtin://agentxx_filesystem", [](const agentxx::agent::PluginConfig& p) {
             return pluginSideText(p.sides);
         })}
    );
    out.push_back(
        {"plugin.list[].args",
         "map<string, any> (原样传给插件, 标量递归展开 ${VAR})",
         "空",
         "plugin:\n  list:\n    - path: \"builtin://agentxx_filesystem\"\n      args:\n"
         "        index_root: \"/repo\"\n        depth: 0.5\n",
         kMinimalPluginYaml,
         false,
         "",
         R"({"index_root":"/repo","depth":0.5})",
         pluginProbe("builtin://agentxx_filesystem", [](const agentxx::agent::PluginConfig& p) {
             return p.args.is_null() ? std::string{} : p.args.dump();
         })}
    );
    out.push_back(
        {"plugin.list[].config",
         "string (插件配置文件/目录; 支持 ~ 与 ${VAR})",
         "空",
         "plugin:\n  list:\n    - path: \"builtin://agentxx_filesystem\"\n      config: \"./conf\"\n",
         kMinimalPluginYaml,
         false,
         "",
         "./conf",
         pluginProbe("builtin://agentxx_filesystem", [](const agentxx::agent::PluginConfig& p) {
             return p.configPath;
         })}
    );

    // ---- mcp 段 ----
    out.push_back(
        {"mcp.list[].namespace",
         "string (必填; 命名空间)",
         "无 (缺 namespace/url 的条目被跳过)",
         "mcp:\n  list:\n    - namespace: \"tools\"\n      url: \"http://127.0.0.1:9/sse\"\n",
         kMinimalMcpYaml,
         false,
         "http://127.0.0.1:9/sse",
         "http://127.0.0.1:9/sse",
         [](const YamlAppConfig& cfg) {
             const auto it = cfg.mcpServers.find("tools");
             return it == cfg.mcpServers.end() ? std::string{"<missing>"} : it->second.url;
         }}
    );
    out.push_back(
        {"mcp.list[].url",
         "string",
         "空 (缺 url 的条目被跳过)",
         "mcp:\n  list:\n    - namespace: \"tools\"\n      url: \"http://127.0.0.1:9/sse\"\n",
         kMinimalMcpYaml,
         false,
         "http://127.0.0.1:9/sse",
         "http://127.0.0.1:9/sse",
         mcpProbe("tools", [](const agentxx::agent::McpServerConfig& m) { return m.url; })}
    );
    out.push_back(
        {"mcp.list[].timeout",
         "int (秒; 0 = 不限制)",
         "120 (秒)",
         "mcp:\n  list:\n    - namespace: \"tools\"\n      url: \"http://127.0.0.1:9/sse\"\n"
         "      timeout: 5\n",
         kMinimalMcpYaml,
         false,
         "120000",
         "5000",
         mcpProbe("tools", [](const agentxx::agent::McpServerConfig& m) {
             return std::to_string(m.toolTimeout.count());
         })}
    );

    // ---- 已废弃的旧键 (识别但不生效, 只记一次迁移提示) ----
    out.push_back(
        {"models",
         "已废弃 (改名为 `model.list`)",
         "不生效 (整段忽略)",
         "models:\n  - name: legacy\n    type: \"openai\"\n",
         "",
         false,
         "",
         "",
         [](const YamlAppConfig& cfg) {
             std::vector<std::string> names;
             for (const auto& [name, mc] : cfg.models) {
                 names.push_back(name);
             }
             std::sort(names.begin(), names.end());
             return joinList(names, ",");
         }}
    );
    out.push_back(
        {"plugins",
         "已废弃 (改名为 `plugin.list`)",
         "不生效 (整段忽略)",
         "plugins:\n  - path: \"builtin://agentxx_filesystem\"\n",
         "",
         false,
         "0",
         "0",
         [](const YamlAppConfig& cfg) { return std::to_string(cfg.plugins.size()); }}
    );
    out.push_back(
        {"use_model",
         "已废弃 (移到 `model.use`)",
         "不生效 (整段忽略)",
         "use_model:\n  default: legacy-model\n",
         "",
         false,
         "",
         "",
         [](const YamlAppConfig& cfg) { return cfg.useModelDefault; }}
    );

    return out;
}

// ---------------------------------------------------------------------------
// 加载与比对
// ---------------------------------------------------------------------------

/// 临时目录 (用例内所有 yaml 文件放这里; 路径统一经 pathToUtf8Generic 转 UTF-8)
fs::path makeTempRoot() {
    auto dir = fs::temp_directory_path()
               / fmt::format(
                   "agentxx_ck_test_{}",
                   std::chrono::steady_clock::now().time_since_epoch().count()
               );
    fs::create_directories(dir);
    return dir;
}

/// 把 yaml 文本写到文件 (键目录用例需要真实文件路径)
void writeYaml(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

YamlAppConfig loadSingle(const fs::path& yamlPath) {
    return agentxx::client::loadYamlConfig(utilxx_base::pathToUtf8Generic(yamlPath), {}, {});
}

YamlAppConfig loadLayered(const fs::path& basePath, const fs::path& overlayPath) {
    return agentxx::client::loadYamlConfigLayered(
        utilxx_base::pathToUtf8Generic(basePath),
        utilxx_base::pathToUtf8Generic(overlayPath),
        {},
        {}
    );
}

/// 单条键校验: 默认值与样例值各比对一次
void verifyEntry(const ConfigKeyEntry& entry, const fs::path& root, size_t index) {
    const auto baseFile = root / fmt::format("base-{}.yaml", index);
    const auto caseFile = root / fmt::format("case-{}.yaml", index);

    auto observedDefault = std::string{};
    auto observedSample  = std::string{};
    try {
        // 默认: 该键缺席时的取值 (顶层键 = 只写 data_dir 的最小文档;
        // 条目级键 = 只写必需字段的最小条目)
        writeYaml(
            baseFile,
            entry.defaultDocYaml.empty() ? std::string{"data_dir: {}\n"} : entry.defaultDocYaml
        );
        observedDefault = entry.probe(loadSingle(baseFile));

        writeYaml(caseFile, entry.sampleYaml);
        observedSample = entry.sampleIsOverlay ? entry.probe(loadLayered(baseFile, caseFile))
                                               : entry.probe(loadSingle(caseFile));
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << entry.keyPath << ": load threw: " << e.what() << std::endl;
        return;
    }

    if (observedDefault == entry.expectDefault) {
        XX_TEST_PASSED++;
    } else {
        XX_TEST_FAILED++;
        TEST_FAIL << entry.keyPath << ": default expected `" << entry.expectDefault << "`, got `"
                  << observedDefault << "`" << std::endl;
    }
    if (observedSample == entry.expectSample) {
        XX_TEST_PASSED++;
    } else {
        XX_TEST_FAILED++;
        TEST_FAIL << entry.keyPath << ": sample expected `" << entry.expectSample << "`, got `"
                  << observedSample << "`" << std::endl;
    }
}

// ---------------------------------------------------------------------------
// 源码扫描: 加载器里被解析的键名必须都在目录里登记
// ---------------------------------------------------------------------------

/// 从 config_loader.cpp 扫出被解析的 yaml 键名
/// - `["键名"]` (节点下标) / `mapChild(..., "键名")` / `useValue("键名")`
/// - 段常量 `kSection* = "键名"` 的定义值 (list / overwrite / mode / remove / use)
/// - 跳过整行注释, 避免把注释里的示例当键
std::set<std::string> scanLoaderKeys(const std::string& loaderPath) {
    std::set<std::string> out;
    std::ifstream         in(utilxx_base::utf8ToPath(loaderPath), std::ios::binary);
    if (!in) {
        return out;
    }
    // 原始字符串用自定义分隔符 (模式里含 `)"` 序列)
    const std::regex kBracket{R"rx(\["([a-z_]+)"\])rx"};
    const std::regex kMapChild{R"rx(mapChild\([^,]*,\s*"([a-z_]+)")rx"};
    const std::regex kUseValue{R"rx(useValue\("([a-z_]+)"\))rx"};
    const std::regex kConstant{R"rx(kSection[A-Za-z]+\s*=\s*"([a-z_]+)")rx"};

    std::string line;
    while (std::getline(in, line)) {
        auto trimmed = trimText(line);
        if (trimmed.starts_with("//") || trimmed.starts_with("*") || trimmed.starts_with("/*")) {
            continue;
        }
        for (const auto* re : {&kBracket, &kMapChild, &kUseValue, &kConstant}) {
            for (auto it = std::sregex_iterator(line.begin(), line.end(), *re);
                 it != std::sregex_iterator();
                 ++it) {
                out.insert((*it)[1].str());
            }
        }
    }
    return out;
}

/// 目录里出现的全部键名 (键路径的所有段 + 列表项字段名)
std::set<std::string> catalogKeyNames(const std::vector<ConfigKeyEntry>& entries) {
    std::set<std::string> out;
    for (const auto& entry : entries) {
        std::string_view path = entry.keyPath;
        size_t           pos  = 0;
        while (pos <= path.size()) {
            auto dot = path.find('.', pos);
            auto seg = path.substr(pos, dot == std::string_view::npos ? std::string_view::npos
                                                                     : dot - pos);
            // 去掉列表标记: `list[]` -> `list`
            if (auto bracket = seg.find('['); bracket != std::string_view::npos) {
                seg = seg.substr(0, bracket);
            }
            if (!seg.empty()) {
                out.insert(std::string{seg});
            }
            if (dot == std::string_view::npos) {
                break;
            }
            pos = dot + 1;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 生成物
// ---------------------------------------------------------------------------

std::string buildKeysJson(const std::vector<ConfigKeyEntry>& entries) {
    auto keys = utilxx_base::Json::array();
    for (const auto& entry : entries) {
        keys.push_back(
            utilxx_base::Json{
                {"path",    entry.keyPath    },
                {"type",    entry.type       },
                {"default", entry.defaultValue},
                {"sample",  entry.sampleYaml.empty()
                                ? std::string{}
                                : trimText(entry.sampleYaml)},
            }
        );
    }
    utilxx_base::Json root;
    root["version"] = 1;
    root["note"]
        = "生成物, 请勿手工编辑; 重新生成: AGENTXX_UPDATE_CONFIG_KEYS=1 agentxx_test "
          "config_keys";
    root["source"] = "agent/client/src/config_loader.cpp";
    root["count"]  = static_cast<int64_t>(entries.size());
    root["keys"]   = std::move(keys);
    return root.dump(2) + "\n";
}

std::string buildKeysDoc(const std::vector<ConfigKeyEntry>& entries) {
    std::string out;
    out += "# 配置键目录\n\n";
    out += "> 本文是**生成物**, 请勿手工编辑。机器可读版本见 `agent/schema/config-keys.json`。\n";
    out += ">\n";
    out += "> 重新生成: 设 `AGENTXX_UPDATE_CONFIG_KEYS=1` 运行测试模块 `config_keys`\n";
    out += "> (生成后请人工 review diff 再提交)。\n>\n";
    out += "> 权威实现是 `agent/client/src/config_loader.cpp`; 本目录的每个键都由\n";
    out += "> `config_keys` 真实加载校验 (默认值与样例值各一次), 并且加载器里出现的新键\n";
    out += "> 若没登记进来, 该模块会直接失败。\n>\n";
    out += "> 相关文档: [index.md](index.md) · [配置与设置边界](configuration.md)\n\n";
    out += fmt::format("键数: {}\n\n", entries.size());
    out += "| 键路径 | 类型 | 默认值 |\n";
    out += "|---|---|---|\n";
    for (const auto& entry : entries) {
        out += fmt::format("| `{}` | {} | {} |\n", entry.keyPath, entry.type, entry.defaultValue);
    }
    out += "\n## 样例\n\n";
    out += "每个键的最小覆盖写法 (示例值仅用于说明形态):\n\n";
    for (const auto& entry : entries) {
        out += fmt::format("### `{}`\n\n", entry.keyPath);
        if (!entry.sampleYaml.empty()) {
            out += "```yaml\n";
            out += trimText(entry.sampleYaml);
            out += "\n```\n\n";
        }
    }
    out += "## 说明\n\n";
    out += "- 列表段 (`model` / `plugin` / `mcp` / `skill` / `memory` / `permission.whitelist` /\n";
    out += "  `permission.blacklist`) 统一是 `list:` + 可选 `overwrite:` 两键结构;\n";
    out += "  旧写法 (段值直接给列表/字符串) 已不再支持, 会记警告并忽略该段。\n";
    out += "- `overwrite` 只在**上层** (overlay) 生效: `mode: merge` (默认, 继承并叠加 base)\n";
    out += "  或 `replace` (整段只用本层); `remove` 按身份剔除 base 项 (model 按 name,\n";
    out += "  plugin 按 path/name, mcp 按 namespace, 名单与路径列表按字符串本身)。\n";
    out += "- 已废弃的旧键 (不再生效, 只记一次迁移提示): `models` -> `model.list`,\n";
    out += "  `plugins` -> `plugin.list`, `use_model` -> `model.use`。\n";
    out += "- 键值都支持 `${VAR}` 展开 (查找顺序: 程序内置变量 > `--env` > `.env` >\n";
    out += "  系统环境变量); 路径类键的 `~` 展开与相对路径绝对化由装配侧完成。\n";
    return out;
}

} // namespace

TestResult testConfigKeys() {
    const int passedBefore = g_ck_passed;
    const int failedBefore = g_ck_failed;

    const auto entries = configKeyEntries();
    // 扫描量下限: 目录被打回原形时不能"零键全通过"
    XX_TEST_EXPECT_GE(entries.size(), size_t{40});

    // 键路径不重复
    {
        std::set<std::string> seen;
        for (const auto& entry : entries) {
            XX_TEST_EXPECT_TRUE(seen.insert(entry.keyPath).second);
        }
    }

    // 逐键真实加载校验
    const auto rootDir = makeTempRoot();
    for (size_t i = 0; i < entries.size(); ++i) {
        verifyEntry(entries[i], rootDir, i);
    }

    // 源码扫描: 加载器解析的键名必须都在目录里
    auto loaderPath = fs::path{__FILE__}.parent_path().parent_path().parent_path()
                      / "client" / "src" / "config_loader.cpp";
    const auto scanned
        = scanLoaderKeys(utilxx_base::pathToUtf8Generic(loaderPath));
    const auto catalog = catalogKeyNames(entries);
    XX_TEST_EXPECT_GE(scanned.size(), size_t{30}); // 规则失效 (扫不到东西) 也要失败
    std::vector<std::string> missing;
    for (const auto& name : scanned) {
        if (!catalog.contains(name)) {
            missing.push_back(name);
        }
    }
    if (missing.empty()) {
        XX_TEST_PASSED++;
    } else {
        XX_TEST_FAILED++;
        TEST_FAIL << "config_loader.cpp parses key(s) not registered in config_keys: ";
        for (size_t i = 0; i < missing.size(); ++i) {
            TEST_FAIL << (i == 0 ? "" : ", ") << "`" << missing[i] << "`";
        }
        TEST_FAIL << " (add them to configKeyEntries() and regenerate the artifacts)"
                  << std::endl;
    }

    // 生成物比对
    const auto jsonText = buildKeysJson(entries);
    const auto docText  = buildKeysDoc(entries);
    const bool update   = agentxx::test::artifactUpdateMode("AGENTXX_UPDATE_CONFIG_KEYS");
#ifdef AGENTXX_CONFIG_KEYS_PATH
    {
        const auto check = agentxx::test::checkGeneratedArtifact(
            std::string{AGENTXX_CONFIG_KEYS_PATH},
            jsonText,
            update,
            "config-keys.json",
            "AGENTXX_UPDATE_CONFIG_KEYS"
        );
        XX_TEST_EXPECT_TRUE(check.ok);
        if (!check.ok) {
            TEST_FAIL << check.message << std::endl;
        } else if (update) {
            TEST_INFO << check.message << std::endl;
        }
    }
#endif
#ifdef AGENTXX_CONFIG_KEYS_DOC_PATH
    {
        const auto check = agentxx::test::checkGeneratedArtifact(
            std::string{AGENTXX_CONFIG_KEYS_DOC_PATH},
            docText,
            update,
            "config-keys.md",
            "AGENTXX_UPDATE_CONFIG_KEYS"
        );
        XX_TEST_EXPECT_TRUE(check.ok);
        if (!check.ok) {
            TEST_FAIL << check.message << std::endl;
        } else if (update) {
            TEST_INFO << check.message << std::endl;
        }
    }
#endif

    // 生成内容本身的形状断言 (路径未注入时也校验)
    XX_TEST_EXPECT_TRUE(jsonText.find("\"keys\"") != std::string::npos);
    XX_TEST_EXPECT_TRUE(docText.find("| 键路径 | 类型 | 默认值 |") != std::string::npos);

    // 临时文件清理
    std::error_code ec;
    fs::remove_all(rootDir, ec);

    return TestResult{g_ck_passed - passedBefore, g_ck_failed - failedBefore};
}

} // namespace test
} // namespace agentxx
