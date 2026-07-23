#include "agentxx-client/io/stdio/agent_stdio.h"
#include "agentxx-client/io/stdio/interrupt_handler.h"
#include "agentxx-client/io/stdio/permission_handler.h"
#include "agentxx-client/train/train.h"
#include "agentxx-client/util/util.h"
#include "agentxx/protocol/acp_server.h"
#include "yaml-cpp/yaml.h"
#ifdef AGENTXX_ENABLE_CLIENT_TUI
#include "agentxx-client/io/tui/agent_tui.h"
#endif
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
        && ((value[0] == '"' && value.back() == '"')
            || (value[0] == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }
    return !key.empty();
}

/// 从指定路径加载 .env 文件，返回变量名->值映射
/// 已存在的环境变量不会被覆盖（.env 优先级低于真实环境变量）
static std::map<std::string, std::string> loadDotEnv(const std::string& path) {
    std::map<std::string, std::string> vars;
    std::ifstream                      file(path);
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

/// 加载覆盖式 env 文件：始终以文件值为准（无视系统环境变量）
static std::map<std::string, std::string> loadOverrideEnv(const std::string& path) {
    std::map<std::string, std::string> vars;
    std::ifstream                      file(path);
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

/// 从多个候选路径加载 .env 文件，后加载的优先级更高
static std::map<std::string, std::string> loadDotEnv(const std::vector<std::string>& paths) {
    std::map<std::string, std::string> merged;
    for (const auto& p : paths) {
        auto vars = loadDotEnv(p);
        for (const auto& kv : vars) {
            merged[kv.first] = kv.second;
        }
    }
    return merged;
}

/// 替换字符串中的 ${VAR} 占位符
/// 查找顺序：--env 文件变量 > 真实环境变量 > .env 文件变量 > 保留原样
static std::string resolveEnvVars(const std::string&                        input,
                                  const std::map<std::string, std::string>& dotEnvVars,
                                  const std::map<std::string, std::string>& overrideEnvVars) {
    static const std::regex pattern(R"(\$\{([^}]+)\})");
    std::string             result;
    std::sregex_iterator    it(input.begin(), input.end(), pattern);
    std::sregex_iterator    end;
    size_t                  lastPos = 0;

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
        const char* envVal = std::getenv(varName.c_str());
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
        XX_LOGW("[config] model.key with `${{}}` but not value in .env: {}", it->str());
        // 4) 保留原样
        result.append(it->str());
    }

    result.append(input, lastPos, std::string::npos);
    return result;
}

// ======================== YAML → JSON 转换 ========================

/// 递归将 YAML::Node 转换为 neograph::json
static neograph::json yamlToJson(const YAML::Node& node) {
    if (!node.IsDefined() || node.IsNull()) {
        return neograph::json{};
    }
    if (node.IsScalar()) {
        int    i;
        double d;
        if (node.as<std::string>() == "true") {
            return neograph::json{true};
        }
        if (node.as<std::string>() == "false") {
            return neograph::json{false};
        }
        if (std::from_chars(node.as<std::string>().data(),
                            node.as<std::string>().data() + node.as<std::string>().size(),
                            i)
                .ec
            == std::errc{}) {
            return neograph::json{i};
        }
        if (std::from_chars(node.as<std::string>().data(),
                            node.as<std::string>().data() + node.as<std::string>().size(),
                            d)
                .ec
            == std::errc{}) {
            return neograph::json{d};
        }
        return neograph::json{node.as<std::string>()};
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

// ======================== YAML 配置模型 ========================

struct YamlAppConfig {
    std::map<std::string, agentxx::agent::ModelConfig> models;
    /// MCP 服务器配置: key=命名空间(唯一), value=服务器 URL
    std::map<std::string, std::string> mcpServers;
    std::string                        useModelDefault;
    std::string                        useModelSubagent;
    std::string                        useModelWebSearch;
    std::string                        useModelAcp;
    std::string                        useModelTrain;
    std::string                        useModelTrainScorer;
    std::string                        useModelTrainOptimizer;
};

static YamlAppConfig loadYamlConfig(const std::string&                        path,
                                    const std::map<std::string, std::string>& dotEnvVars,
                                    const std::map<std::string, std::string>& overrideEnvVars) {
    YamlAppConfig cfg;
    auto          root = YAML::LoadFile(path);

    if (root["models"] && root["models"].IsSequence()) {
        for (const auto& node : root["models"]) {
            agentxx::agent::ModelConfig mc;
            mc.name = resolveEnvVars(node["name"].as<std::string>(""), dotEnvVars, overrideEnvVars);
            mc.type = resolveEnvVars(node["type"].as<std::string>("openai"),
                                     dotEnvVars,
                                     overrideEnvVars);
            mc.baseUrl
                = resolveEnvVars(node["base_url"].as<std::string>(""), dotEnvVars, overrideEnvVars);
            mc.apiKey
                = resolveEnvVars(node["api_key"].as<std::string>(""), dotEnvVars, overrideEnvVars);
            mc.modelName = resolveEnvVars(node["model_name"].as<std::string>(""),
                                          dotEnvVars,
                                          overrideEnvVars);
            if (node["send_thinking"]) {
                mc.sendThinking = resolveEnvVars((node["send_thinking"]).as<std::string>("false"),
                                                 dotEnvVars,
                                                 overrideEnvVars)
                                  == "true";
            }
            if (node["connect_timeout"]) {
                mc.connectTimeoutSeconds
                    = std::stoi(resolveEnvVars(node["connect_timeout"].as<std::string>("16"),
                                               dotEnvVars,
                                               overrideEnvVars));
            }
            if (node["read_timeout"]) {
                mc.readTimeoutSeconds
                    = std::stoi(resolveEnvVars(node["read_timeout"].as<std::string>("24"),
                                               dotEnvVars,
                                               overrideEnvVars));
            }
            if (node["model_support_max_token"]) {
                mc.modelSupportMaxToken = static_cast<size_t>(
                    std::stoull(resolveEnvVars(node["model_support_max_token"].as<std::string>("0"),
                                               dotEnvVars,
                                               overrideEnvVars)));
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
        cfg.useModelDefault   = resolveEnvVars(root["use_model"]["default"].as<std::string>(""),
                                             dotEnvVars,
                                             overrideEnvVars);
        cfg.useModelSubagent  = resolveEnvVars(root["use_model"]["subagent"].as<std::string>(""),
                                              dotEnvVars,
                                              overrideEnvVars);
        cfg.useModelWebSearch = resolveEnvVars(root["use_model"]["web_search"].as<std::string>(""),
                                               dotEnvVars,
                                               overrideEnvVars);
        cfg.useModelAcp       = resolveEnvVars(root["use_model"]["acp"].as<std::string>(""),
                                         dotEnvVars,
                                         overrideEnvVars);
        cfg.useModelTrain     = resolveEnvVars(root["use_model"]["train"].as<std::string>(""),
                                           dotEnvVars,
                                           overrideEnvVars);
        cfg.useModelTrainScorer
            = resolveEnvVars(root["use_model"]["train_scorer"].as<std::string>(""),
                             dotEnvVars,
                             overrideEnvVars);
        cfg.useModelTrainOptimizer
            = resolveEnvVars(root["use_model"]["train_optimizer"].as<std::string>(""),
                             dotEnvVars,
                             overrideEnvVars);
    }

    if (root["mcp_servers"] && root["mcp_servers"].IsSequence()) {
        for (const auto& node : root["mcp_servers"]) {
            auto ns  = resolveEnvVars(node["namespace"].as<std::string>(""),
                                     dotEnvVars,
                                     overrideEnvVars);
            auto url = resolveEnvVars(node["url"].as<std::string>(""), dotEnvVars, overrideEnvVars);
            if (ns.empty() || url.empty()) {
                std::cerr << "[Config] Warning: mcp_servers entry missing `namespace` "
                             "or `url`, skipped"
                          << std::endl;
                continue;
            }
            if (cfg.mcpServers.contains(ns)) {
                std::cerr << "[Config] Warning: duplicate mcp namespace '" << ns
                          << "', overriding its url" << std::endl;
            }
            cfg.mcpServers[ns] = url;
        }
    }

    return cfg;
}

static agentxx::agent::ModelConfig
    resolveModelConfig(const std::map<std::string, agentxx::agent::ModelConfig>& models,
                       const std::string&                                        modelName) {
    if (modelName.empty()) {
        return agentxx::agent::ModelConfig{};
    }
    auto it = models.find(modelName);
    if (it == models.end()) {
        std::cerr << "[Config] Warning: model '" << modelName << "' not found in config"
                  << std::endl;
        return agentxx::agent::ModelConfig{};
    }
    return it->second;
}

static void applyModelToConfig(std::shared_ptr<agentxx::agent::AgentConfig> agentConfig,
                               const std::map<std::string, agentxx::agent::ModelConfig>& models,
                               const std::string& modelName) {
    auto mc = resolveModelConfig(models, modelName);
    if (mc.isValid()) {
        agentConfig->model = std::move(mc);
    }
}

static void
    applySubagentModelToConfig(std::shared_ptr<agentxx::agent::AgentConfig> agentConfig,
                               const std::map<std::string, agentxx::agent::ModelConfig>& models,
                               const std::string& modelName) {
    auto mc = resolveModelConfig(models, modelName);
    if (mc.isValid()) {
        agentConfig->subagentModel = std::move(mc);
    }
}

static void
    applyWebSearchModelToConfig(std::shared_ptr<agentxx::agent::AgentConfig> agentConfig,
                                const std::map<std::string, agentxx::agent::ModelConfig>& models,
                                const std::string& modelName) {
    auto mc = resolveModelConfig(models, modelName);
    if (mc.isValid()) {
        agentConfig->websearchModel = std::move(mc);
    }
}

/// 填充可用模型列表 (供 TUI 运行时切换模型)
static void
    applyAvailableModelsToConfig(std::shared_ptr<agentxx::agent::AgentConfig> agentConfig,
                                 const std::map<std::string, agentxx::agent::ModelConfig>& models,
                                 const std::string& currentModelName) {
    for (const auto& [name, entry] : models) {
        auto mc = resolveModelConfig(models, name);
        if (mc.isValid()) {
            agentConfig->availableModels[name] = std::move(mc);
        }
    }
    agentConfig->currentModelName = currentModelName;
}

asio::awaitable<void> runCliAsync(agentxx::agent::DeepAgent& agent) {
    auto io = std::make_shared<AgentStdIO>();

    const auto cliEventCallback = [&io](const neograph::graph::GraphEvent& event) {
        switch (event.type) {
        case neograph::graph::GraphEvent::Type::NODE_START:
        case neograph::graph::GraphEvent::Type::NODE_END:
            break;
        case neograph::graph::GraphEvent::Type::LLM_TOKEN:
            {
                std::string token;
                std::string kind = "content";
                if (event.data.is_string()) {
                    token = event.data.get<std::string>();
                } else if (event.data.is_object()) {
                    neograph::ChatStreamChunk chunk;
                    neograph::from_json(event.data, chunk);
                    token = std::move(chunk.data);
                    if (chunk.type == neograph::ChatStreamChunk::TYPE_THINKING) {
                        kind = "thinking";
                    }
                }
                io->onToken(token, kind);
            }
            break;
        case neograph::graph::GraphEvent::Type::CHANNEL_WRITE:
        case neograph::graph::GraphEvent::Type::INTERRUPT:
        case neograph::graph::GraphEvent::Type::ERROR:
            break;
        }
    };
    const auto cliInterruptCallback
        = [&io](const std::string& interruptNode,
                const std::string& interruptValue,
                const std::string& interruptHandleName) -> asio::awaitable<void> {
        io->onInterrupt(interruptNode, interruptValue, interruptHandleName);
        co_return;
    };

    bool       isFirstMsg = true;
    const auto thread_id  = "session";
    auto       messages   = neograph::json::array();

    StdioInterruptHandler                   cliInterruptHandler{agent.agentContext};
    StdioPermissionPrompter                 cliPermissionPrompter{agent.agentContext};
    agentxx::middleware::SubagentSupervisor subagentSupervisor{agent.agentContext};
    co_await cliInterruptHandler.start();
    co_await cliPermissionPrompter.start();
    co_await subagentSupervisor.start();

    std::cout << ">>> " << std::flush;

    for (;;) {
        auto inputOpt = co_await io->getInput();
        if (!inputOpt.has_value()) {
            break;
        }
        auto input = std::move(inputOpt.value());
        if (!input.empty()) {
            io->resetTokenState();
            std::cout << agent.agentContext->agentConfig->agentNameView << ": " << std::flush;

            auto turnResult = co_await agent.runConversationTurnAsync(
                thread_id,
                input,
                isFirstMsg,
                std::move(messages),
                io,
                agentxx::middleware::EventBridge::make(agent.agentContext->agentConfig->agentName,
                                                       thread_id,
                                                       agent.agentContext,
                                                       cliEventCallback),
                cliInterruptCallback);
            messages   = std::move(turnResult.messages);
            isFirstMsg = false;
        }
        std::cout << "\n\n>>> " << std::flush;
    }
}

void runCli(agentxx::agent::DeepAgent& agent) {
    asio::co_spawn(
        *agent.ioCtx,
        [&]() -> asio::awaitable<void> {
            co_await agent.init();
            co_return co_await runCliAsync(agent);
        },
        asio::detached);
    agent.ioCtx->run();
}

#ifdef AGENTXX_ENABLE_CLIENT_TUI
asio::awaitable<void> runTuiAsync(agentxx::agent::DeepAgent& agent) {
    const auto thread_id = std::string{"session"};
    auto       io        = std::make_shared<AgentTUI>(co_await asio::this_coro::executor,
                                         agent.agentContext,
                                         thread_id);
    io->start();

    const auto tuiEventCallback = [&io](const neograph::graph::GraphEvent& event) {
        switch (event.type) {
        case neograph::graph::GraphEvent::Type::NODE_START:
        case neograph::graph::GraphEvent::Type::NODE_END:
            break;
        case neograph::graph::GraphEvent::Type::LLM_TOKEN:
            {
                std::string token;
                std::string kind = "content";
                if (event.data.is_string()) {
                    token = event.data.get<std::string>();
                } else if (event.data.is_object()) {
                    neograph::ChatStreamChunk chunk;
                    neograph::from_json(event.data, chunk);
                    token = std::move(chunk.data);
                    if (chunk.type == neograph::ChatStreamChunk::TYPE_THINKING) {
                        kind = "thinking";
                    }
                }
                io->onToken(token, kind);
            }
            break;
        case neograph::graph::GraphEvent::Type::CHANNEL_WRITE:
        case neograph::graph::GraphEvent::Type::INTERRUPT:
        case neograph::graph::GraphEvent::Type::ERROR:
            break;
        }
    };
    const auto tuiInterruptCallback
        = [&io](const std::string& interruptNode,
                const std::string& interruptValue,
                const std::string& interruptHandleName) -> asio::awaitable<void> {
        io->onInterrupt(interruptNode, interruptValue, interruptHandleName);
        co_return;
    };

    bool isFirstMsg = true;
    auto messages   = neograph::json::array();

    StdioInterruptHandler                   tuiInterruptHandler{agent.agentContext};
    StdioPermissionPrompter                 tuiPermissionPrompter{agent.agentContext};
    agentxx::middleware::SubagentSupervisor subagentSupervisor{agent.agentContext};
    co_await tuiInterruptHandler.start();
    co_await tuiPermissionPrompter.start();
    co_await subagentSupervisor.start();

    for (;;) {
        auto inputOpt = co_await io->getInput();
        if (!inputOpt.has_value()) {
            break;
        }
        auto input = std::move(inputOpt.value());
        if (!input.empty()) {
            io->resetTokenState();
            auto turnResult = co_await agent.runConversationTurnAsync(
                thread_id,
                input,
                isFirstMsg,
                std::move(messages),
                io,
                agentxx::middleware::EventBridge::make(agent.agentContext->agentConfig->agentName,
                                                       thread_id,
                                                       agent.agentContext,
                                                       tuiEventCallback),
                tuiInterruptCallback);
            messages   = std::move(turnResult.messages);
            isFirstMsg = false;
            io->resetTokenState();
        }
    }
    io->stop();
}

void runTui(agentxx::agent::DeepAgent& agent) {
    asio::co_spawn(
        *agent.ioCtx,
        [&]() -> asio::awaitable<void> {
            co_await agent.init();
            co_return co_await runTuiAsync(agent);
        },
        asio::detached);
    agent.ioCtx->run();
}
#endif

int main(int argn, char** argv) {
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
    std::string mode = "tui";
    for (int i = 1; i < argn; ++i) {
        std::string arg(argv[i]);
        if (arg == "--config" && i + 1 < argn) {
            ++i;
            configPath = argv[i];
        } else if (arg == "--env" && i + 1 < argn) {
            ++i;
            overrideEnvPath = argv[i];
        } else if (mode == "tui") {
            mode = arg;
        } else if (mode == "cli") {
            mode = arg;
        }
    }

    // 加载覆盖式 env 文件（--env，最高优先级）
    std::map<std::string, std::string> overrideEnvVars;
    if (!overrideEnvPath.empty()) {
        overrideEnvVars = loadOverrideEnv(overrideEnvPath);
        XX_OUT("[Config] Loaded {} override variables from: {}",
               overrideEnvVars.size(),
               overrideEnvPath);
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
        } catch (const std::exception& e) {
            std::cerr << "[Config] Failed to load config: " << e.what() << std::endl;
            return 1;
        }
    }

    if (mode == "train") {
        auto config                                    = buildDefaultConfig();
        config->logPringToolcall                       = false;
        config->logPrintMessagesBeforeLLM              = false;
        config->logPrintMessagesBeforeLLMWithSystemMsg = false;
        config->logPrintSummarizationResultTokenCount  = false;
        applyModelToConfig(config, yamlCfg.models, yamlCfg.useModelTrain);

        auto scorerConfig                                    = buildDefaultConfig();
        scorerConfig->logPringToolcall                       = false;
        scorerConfig->logPrintMessagesBeforeLLM              = false;
        scorerConfig->logPrintMessagesBeforeLLMWithSystemMsg = false;
        scorerConfig->logPrintSummarizationResultTokenCount  = false;
        applyModelToConfig(scorerConfig, yamlCfg.models, yamlCfg.useModelTrainScorer);

        auto optimizerConfig                                    = buildDefaultConfig();
        optimizerConfig->logPringToolcall                       = false;
        optimizerConfig->logPrintMessagesBeforeLLM              = false;
        optimizerConfig->logPrintMessagesBeforeLLMWithSystemMsg = false;
        optimizerConfig->logPrintSummarizationResultTokenCount  = false;
        applyModelToConfig(optimizerConfig, yamlCfg.models, yamlCfg.useModelTrainOptimizer);

        runTrainingMode(config, scorerConfig, optimizerConfig);
        return 0;
    }

    if (mode == "acp") {
        auto config                                   = buildDefaultConfig();
        config->logPringToolcall                      = false;
        config->logPrintMessagesBeforeLLM             = false;
        config->logPrintSummarizationResultTokenCount = false;
        applyModelToConfig(config, yamlCfg.models, yamlCfg.useModelAcp);
        applySubagentModelToConfig(config, yamlCfg.models, yamlCfg.useModelSubagent);
        applyWebSearchModelToConfig(config, yamlCfg.models, yamlCfg.useModelWebSearch);
        auto agent = std::make_shared<agentxx::agent::DeepAgent>(config);
        asio::co_spawn(
            *agent->ioCtx,
            [agent]() -> asio::awaitable<void> {
                co_await agent->init();
                agentxx::server::StdioAcpServer server(agent, neograph::json::object());
                server.run();
                co_return;
            },
            asio::detached);
        agent->ioCtx->run();
        return 0;
    }

    auto config = buildDefaultConfig();
    applyModelToConfig(config, yamlCfg.models, yamlCfg.useModelDefault);
    applySubagentModelToConfig(config, yamlCfg.models, yamlCfg.useModelSubagent);
    applyWebSearchModelToConfig(config, yamlCfg.models, yamlCfg.useModelWebSearch);
    applyAvailableModelsToConfig(config, yamlCfg.models, yamlCfg.useModelDefault);
    // MCP 服务器 (来自 config.yaml 的 mcp_servers, key 为命名空间)
    config->mcpServerUrls = yamlCfg.mcpServers;
    // config->mcpServerUrls["local"] = "http://172.29.48.1:17001/mcp";
    // config->mcpServerUrls["exa"] = "https://mcp.exa.ai";
    config->skillDirPaths
        = std::vector<std::string>{"/home/coolight/program/agentxx/isolation/skills/"};

    if (mode == "tui") {
#if AGENTXX_ENABLE_CLIENT_TUI
        config->logPringToolcall                       = false;
        config->logPrintMessagesBeforeLLM              = false;
        config->logPrintMessagesBeforeLLMWithSystemMsg = false;
        config->logPrintSummarizationResultTokenCount  = false;
        auto agent                                     = agentxx::agent::DeepAgent{config};
        runTui(agent);
#else
        XX_LOGE(
            R"(TUI is not support! Please set `AGENTXX_ENABLE_CLIENT_TUI=1` and recomplie agentxx_cli)");
#endif
        return 0;
    }

    {
        // 默认 CLI 交互模式
        XX_OUT("======= Agentxx Client =======");
        config->logPringToolcall                      = true;
        config->logPrintMessagesBeforeLLM             = true;
        config->logPrintSummarizationResultTokenCount = true;
        auto agent                                    = agentxx::agent::DeepAgent{config};
        runCli(agent);
    }
    return 0;
}
