#include "train.h"
#include "util.h"
#include "yaml-cpp/yaml.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <vector>

// ======================== .env 文件加载 ========================

/// 解析单行 key=value，去除引号和空白
static bool parseEnvLine(std::string &line, std::string &key,
                         std::string &value) {
  auto trim = [](std::string &s) {
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
  };
  trim(line);
  if (line.empty() || line[0] == '#')
    return false;
  if (line.rfind("export ", 0) == 0)
    line = line.substr(7);

  auto eq = line.find('=');
  if (eq == std::string::npos)
    return false;

  key = line.substr(0, eq);
  trim(key);

  value = line.substr(eq + 1);
  trim(value);

  if (value.size() >= 2 && ((value[0] == '"' && value.back() == '"') ||
                            (value[0] == '\'' && value.back() == '\''))) {
    value = value.substr(1, value.size() - 2);
  }
  return !key.empty();
}

/// 从指定路径加载 .env 文件，返回变量名->值映射
/// 已存在的环境变量不会被覆盖（.env 优先级低于真实环境变量）
static std::map<std::string, std::string> loadDotEnv(const std::string &path) {
  std::map<std::string, std::string> vars;
  std::ifstream file(path);
  if (!file.is_open())
    return vars;

  std::string line;
  while (std::getline(file, line)) {
    std::string key, value;
    if (!parseEnvLine(line, key, value))
      continue;
    const char *existing = std::getenv(key.c_str());
    vars[key] = existing ? std::string(existing) : value;
  }
  return vars;
}

/// 加载覆盖式 env 文件：始终以文件值为准（无视系统环境变量）
static std::map<std::string, std::string>
loadOverrideEnv(const std::string &path) {
  std::map<std::string, std::string> vars;
  std::ifstream file(path);
  if (!file.is_open())
    return vars;

  std::string line;
  while (std::getline(file, line)) {
    std::string key, value;
    if (!parseEnvLine(line, key, value))
      continue;
    vars[key] = value;
  }
  return vars;
}

/// 从多个候选路径加载 .env 文件，后加载的优先级更高
static std::map<std::string, std::string>
loadDotEnv(const std::vector<std::string> &paths) {
  std::map<std::string, std::string> merged;
  for (const auto &p : paths) {
    auto vars = loadDotEnv(p);
    for (const auto &kv : vars) {
      merged[kv.first] = kv.second;
    }
  }
  return merged;
}

/// 替换字符串中的 ${VAR} 占位符
/// 查找顺序：--env 文件变量 > 真实环境变量 > .env 文件变量 > 保留原样
static std::string
resolveEnvVars(const std::string &input,
               const std::map<std::string, std::string> &dotEnvVars,
               const std::map<std::string, std::string> &overrideEnvVars) {
  static const std::regex pattern(R"(\$\{([^}]+)\})");
  std::string result;
  std::sregex_iterator it(input.begin(), input.end(), pattern);
  std::sregex_iterator end;
  size_t lastPos = 0;

  for (; it != end; ++it) {
    result.append(input, lastPos, it->position() - lastPos);
    lastPos = it->position() + it->length();

    std::string varName = (*it)[1].str();

    // 1) --env 文件变量（最高优先级）
    auto ovIt = overrideEnvVars.find(varName);
    if (ovIt != overrideEnvVars.end()) {
      result.append(ovIt->second);
      continue;
    }
    // 2) 真实环境变量
    const char *envVal = std::getenv(varName.c_str());
    if (envVal != nullptr) {
      result.append(envVal);
      continue;
    }
    // 3) .env 文件变量
    auto dotIt = dotEnvVars.find(varName);
    if (dotIt != dotEnvVars.end()) {
      result.append(dotIt->second);
      continue;
    }
    XX_LOGW("[config] model.key with `${{}}` but not value in .env: {}",
            it->str());
    // 4) 保留原样
    result.append(it->str());
  }

  result.append(input, lastPos, std::string::npos);
  return result;
}

// ======================== YAML 配置模型 ========================

struct ModelEntry {
  std::string name;
  std::string baseUrl;
  std::string key;
  std::string modelname;
  YAML::Node extraApiConfig;
};

struct YamlAppConfig {
  std::map<std::string, ModelEntry> models;
  std::string useModelDefault;
  std::string useModelAcp;
  std::string useModelTrain;
  std::string useModelTrainScorer;
  std::string useModelTrainOptimizer;
};

static YamlAppConfig
loadYamlConfig(const std::string &path,
               const std::map<std::string, std::string> &dotEnvVars,
               const std::map<std::string, std::string> &overrideEnvVars) {
  YamlAppConfig cfg;
  auto root = YAML::LoadFile(path);

  if (root["models"] && root["models"].IsSequence()) {
    for (const auto &node : root["models"]) {
      ModelEntry me;
      me.name = resolveEnvVars(node["name"].as<std::string>(""), dotEnvVars,
                               overrideEnvVars);
      me.baseUrl = resolveEnvVars(node["base_url"].as<std::string>(""),
                                  dotEnvVars, overrideEnvVars);
      me.key = resolveEnvVars(node["key"].as<std::string>(""), dotEnvVars,
                              overrideEnvVars);
      me.modelname = resolveEnvVars(node["modelname"].as<std::string>(""),
                                    dotEnvVars, overrideEnvVars);
      me.extraApiConfig = node["extra_api_config"];
      if (!me.name.empty()) {
        cfg.models[me.name] = std::move(me);
      }
    }
  }

  if (root["use_model"]) {
    cfg.useModelDefault =
        resolveEnvVars(root["use_model"]["default"].as<std::string>(""),
                       dotEnvVars, overrideEnvVars);
    cfg.useModelAcp =
        resolveEnvVars(root["use_model"]["acp"].as<std::string>(""), dotEnvVars,
                       overrideEnvVars);
    cfg.useModelTrain =
        resolveEnvVars(root["use_model"]["train"].as<std::string>(""),
                       dotEnvVars, overrideEnvVars);
    cfg.useModelTrainScorer =
        resolveEnvVars(root["use_model"]["train_scorer"].as<std::string>(""),
                       dotEnvVars, overrideEnvVars);
    cfg.useModelTrainOptimizer =
        resolveEnvVars(root["use_model"]["train_optimizer"].as<std::string>(""),
                       dotEnvVars, overrideEnvVars);
  }

  return cfg;
}

static void
applyModelToConfig(std::shared_ptr<agentxx::agent::AgentConfig> agentConfig,
                   const std::map<std::string, ModelEntry> &models,
                   const std::string &modelName) {
  if (modelName.empty()) {
    return;
  }
  auto it = models.find(modelName);
  if (it == models.end()) {
    std::cerr << "[Config] Warning: model '" << modelName
              << "' not found in config" << std::endl;
    return;
  }
  const auto &entry = it->second;
  if (!entry.baseUrl.empty()) {
    agentConfig->modelOpenAIBaseUrl = entry.baseUrl;
  }
  if (!entry.key.empty()) {
    agentConfig->modelOpenAIApiKey = entry.key;
  }
  if (!entry.modelname.empty()) {
    agentConfig->modelOpenAIModelName = entry.modelname;
  }
}

int main(int argn, char **argv) {
#if XX_IS_WIN_D
  SetConsoleOutputCP(CP_UTF8);
#endif
#if XX_IS_DEBUG_D && XX_IS_LINUX_D
  agentxx::util::signalError(argv[0]);
#endif

  // 解析命令行参数：支持 --config <path> 和 --env <path> 任意位置，
  // 剩余第一个非选项参数为 mode
  std::string configPath = "agentxx-config.yaml";
  std::string overrideEnvPath;
  std::string mode = "cli";
  for (int i = 1; i < argn; ++i) {
    std::string arg(argv[i]);
    if (arg == "--config" && i + 1 < argn) {
      configPath = argv[++i];
    } else if (arg == "--env" && i + 1 < argn) {
      overrideEnvPath = argv[++i];
    } else if (mode == "cli") {
      mode = arg;
    }
  }

  // 加载覆盖式 env 文件（--env，最高优先级）
  std::map<std::string, std::string> overrideEnvVars;
  if (!overrideEnvPath.empty()) {
    overrideEnvVars = loadOverrideEnv(overrideEnvPath);
    XX_OUT("[Config] Loaded {} override variables from: {}",
           overrideEnvVars.size(), overrideEnvPath);
  }

  // 加载 .env 文件（从当前目录和配置文件所在目录，优先级低于系统环境变量）
  std::map<std::string, std::string> dotEnvVars;
  {
    std::vector<std::string> envPaths;
    envPaths.push_back(".env");
    if (!configPath.empty()) {
      auto configDir = std::filesystem::path(configPath).parent_path();
      if (!configDir.empty()) {
        envPaths.push_back((configDir / ".env").string());
      }
    }
    dotEnvVars = loadDotEnv(envPaths);
    if (!dotEnvVars.empty()) {
      XX_OUT("[Config] Loaded {} variables from .env", dotEnvVars.size());
    }
  }

  // 加载 YAML 配置
  YamlAppConfig yamlCfg;
  if (!configPath.empty()) {
    try {
      yamlCfg = loadYamlConfig(configPath, dotEnvVars, overrideEnvVars);
      XX_OUT("[Config] Loaded config from: {}", configPath);
    } catch (const std::exception &e) {
      std::cerr << "[Config] Failed to load config: " << e.what() << std::endl;
      return 1;
    }
  }

  if (mode == "train") {
    auto config = buildDefaultConfig();
    config->logPringToolcall = false;
    config->logPrintMessagesBeforeLLM = false;
    config->logPrintMessagesBeforeLLMWithSystemMsg = false;
    config->logPrintSummarizationResultTokenCount = false;
    applyModelToConfig(config, yamlCfg.models, yamlCfg.useModelTrain);

    auto scorerConfig = buildDefaultConfig();
    scorerConfig->logPringToolcall = false;
    scorerConfig->logPrintMessagesBeforeLLM = false;
    scorerConfig->logPrintMessagesBeforeLLMWithSystemMsg = false;
    scorerConfig->logPrintSummarizationResultTokenCount = false;
    applyModelToConfig(scorerConfig, yamlCfg.models,
                       yamlCfg.useModelTrainScorer);

    auto optimizerConfig = buildDefaultConfig();
    optimizerConfig->logPringToolcall = false;
    optimizerConfig->logPrintMessagesBeforeLLM = false;
    optimizerConfig->logPrintMessagesBeforeLLMWithSystemMsg = false;
    optimizerConfig->logPrintSummarizationResultTokenCount = false;
    applyModelToConfig(optimizerConfig, yamlCfg.models,
                       yamlCfg.useModelTrainOptimizer);

    runTrainingMode(config, scorerConfig, optimizerConfig);
    return 0;
  }

  if (mode == "acp") {
    auto config = buildDefaultConfig();
    config->logPringToolcall = false;
    config->logPrintMessagesBeforeLLM = false;
    config->logPrintSummarizationResultTokenCount = false;
    applyModelToConfig(config, yamlCfg.models, yamlCfg.useModelAcp);
    auto agent = std::make_shared<agentxx::agent::DeepAgent>(config);
    asio::co_spawn(
        *agent->ioCtx,
        [agent]() -> asio::awaitable<void> {
          co_await agent->init();
          agentxx::server::StdioAcpServer server(agent,
                                                 neograph::json::object());
          server.run();
          co_return;
        },
        asio::detached);
    agent->ioCtx->run();
    return 0;
  }

  // 默认 CLI 交互模式
  XX_OUT("======= Agentxx Client =======");
  auto config = buildDefaultConfig();
  applyModelToConfig(config, yamlCfg.models, yamlCfg.useModelDefault);
  config->mcpServerUrls.push_back("http://172.29.48.1:17001/mcp");
  // config->mcpServerUrls.push_back("https://mcp.exa.ai");
  config->skillDirPaths = std::vector<std::string>{
      "/home/coolight/program/agentxx/isolation/skills/"};
  config->logPringToolcall = true;
  config->logPrintMessagesBeforeLLM = true;
  config->logPrintSummarizationResultTokenCount = true;
  auto agent = agentxx::agent::DeepAgent{config};
  agent.runCli();
  return 0;
}
