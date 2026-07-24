#include "agentxx-client/io/stdio/agent_stdio.h"
#include "agentxx-client/train/train.h"
#include "agentxx-client/util/util.h"
#include "agentxx/agent/remote/agent_server.h"
#include "agentxx/agent/remote/channel_transport.h"
#include "agentxx/agent/remote/remote_client_io.h"
#include "agentxx/agent/remote/ws_transport.h"
#include "agentxx/protocol/acp_server.h"
#include "agentxx/util/ws_client.h"
#include "asio/executor_work_guard.hpp"
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
#include <thread>
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
        && ((value[0] == '"' && value.back() == '"') || (value[0] == '\'' && value.back() == '\'')
        )) {
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
static std::string resolveEnvVars(
    const std::string&                        input,
    const std::map<std::string, std::string>& dotEnvVars,
    const std::map<std::string, std::string>& overrideEnvVars
) {
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
        if (std::from_chars(
                node.as<std::string>().data(),
                node.as<std::string>().data() + node.as<std::string>().size(),
                i
            )
                .ec
            == std::errc{}) {
            return neograph::json{i};
        }
        if (std::from_chars(
                node.as<std::string>().data(),
                node.as<std::string>().data() + node.as<std::string>().size(),
                d
            )
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

static YamlAppConfig loadYamlConfig(
    const std::string&                        path,
    const std::map<std::string, std::string>& dotEnvVars,
    const std::map<std::string, std::string>& overrideEnvVars
) {
    YamlAppConfig cfg;
    auto          root = YAML::LoadFile(path);

    if (root["models"] && root["models"].IsSequence()) {
        for (const auto& node : root["models"]) {
            agentxx::agent::ModelConfig mc;
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
            if (node["send_thinking"]) {
                mc.sendThinking = resolveEnvVars(
                                      (node["send_thinking"]).as<std::string>("false"),
                                      dotEnvVars,
                                      overrideEnvVars
                                  )
                                  == "true";
            }
            if (node["connect_timeout"]) {
                mc.connectTimeoutSeconds = std::stoi(resolveEnvVars(
                    node["connect_timeout"].as<std::string>("16"),
                    dotEnvVars,
                    overrideEnvVars
                ));
            }
            if (node["read_timeout"]) {
                mc.readTimeoutSeconds = std::stoi(resolveEnvVars(
                    node["read_timeout"].as<std::string>("24"),
                    dotEnvVars,
                    overrideEnvVars
                ));
            }
            if (node["model_support_max_token"]) {
                mc.modelSupportMaxToken = static_cast<size_t>(std::stoull(resolveEnvVars(
                    node["model_support_max_token"].as<std::string>("0"),
                    dotEnvVars,
                    overrideEnvVars
                )));
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

    if (root["mcp_servers"] && root["mcp_servers"].IsSequence()) {
        for (const auto& node : root["mcp_servers"]) {
            auto ns = resolveEnvVars(
                node["namespace"].as<std::string>(""),
                dotEnvVars,
                overrideEnvVars
            );
            auto url = resolveEnvVars(node["url"].as<std::string>(""), dotEnvVars, overrideEnvVars);
            if (ns.empty() || url.empty()) {
                XX_LOGW(
                    R"([Config] Warning: mcp_servers entry missing `namespace` or `url`, skipped)"
                );
                continue;
            }
            if (cfg.mcpServers.contains(ns)) {
                XX_LOGW(
                    R"([Config] Warning: duplicate mcp namespace '{}', overriding its url)",
                    ns
                );
            }
            cfg.mcpServers[ns] = url;
        }
    }

    return cfg;
}

static agentxx::agent::ModelConfig resolveModelConfig(
    const std::map<std::string, agentxx::agent::ModelConfig>& models,
    const std::string&                                        modelName
) {
    if (modelName.empty()) {
        return agentxx::agent::ModelConfig{};
    }
    auto it = models.find(modelName);
    if (it == models.end()) {
        XX_LOGE("[Config] Warning: model '{}' not found in config", modelName);
        return agentxx::agent::ModelConfig{};
    }
    return it->second;
}

static void applyModelToConfig(
    std::shared_ptr<agentxx::agent::AgentConfig>              agentConfig,
    const std::map<std::string, agentxx::agent::ModelConfig>& models,
    const std::string&                                        modelName
) {
    auto mc = resolveModelConfig(models, modelName);
    if (mc.isValid()) {
        agentConfig->model = std::move(mc);
    }
}

static void applySubagentModelToConfig(
    std::shared_ptr<agentxx::agent::AgentConfig>              agentConfig,
    const std::map<std::string, agentxx::agent::ModelConfig>& models,
    const std::string&                                        modelName
) {
    auto mc = resolveModelConfig(models, modelName);
    if (mc.isValid()) {
        agentConfig->subagentModel = std::move(mc);
    }
}

static void applyWebSearchModelToConfig(
    std::shared_ptr<agentxx::agent::AgentConfig>              agentConfig,
    const std::map<std::string, agentxx::agent::ModelConfig>& models,
    const std::string&                                        modelName
) {
    auto mc = resolveModelConfig(models, modelName);
    if (mc.isValid()) {
        agentConfig->websearchModel = std::move(mc);
    }
}

/// 填充可用模型列表 (供 TUI 运行时切换模型)
static void applyAvailableModelsToConfig(
    std::shared_ptr<agentxx::agent::AgentConfig>              agentConfig,
    const std::map<std::string, agentxx::agent::ModelConfig>& models,
    const std::string&                                        currentModelName
) {
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

    bool       isFirstMsg = true;
    const auto thread_id  = "session";

    agentxx::middleware::SubagentSupervisor subagentSupervisor{agent.agentContext};
    co_await subagentSupervisor.start();

    std::cout << ">>> " << std::flush;

    for (;;) {
        auto inputOpt = co_await io->getInput();
        if (!inputOpt.has_value()) {
            break;
        }
        auto input = std::move(inputOpt.value());
        if (!input.empty()) {
            std::cout << agent.agentContext->agentConfig->agentNameView << ": " << std::flush;

            co_await agent.runConversationTurnAsync(thread_id, input, isFirstMsg, io);
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
        asio::detached
    );
    agent.ioCtx->run();
}

#ifdef AGENTXX_ENABLE_CLIENT_TUI
asio::awaitable<void> runTuiAsync(agentxx::agent::DeepAgent& agent) {
    const auto thread_id = std::string{"session"};
    auto       io        = std::make_shared<AgentTUI>(
        co_await asio::this_coro::executor,
        agent.agentContext,
        thread_id
    );
    io->start();

    bool isFirstMsg = true;

    agentxx::middleware::SubagentSupervisor subagentSupervisor{agent.agentContext};
    co_await subagentSupervisor.start();

    for (;;) {
        auto inputOpt = co_await io->getInput();
        if (!inputOpt.has_value()) {
            break;
        }
        auto input = std::move(inputOpt.value());
        if (!input.empty()) {
            co_await agent.runConversationTurnAsync(thread_id, input, isFirstMsg, io);
            isFirstMsg = false;
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
        asio::detached
    );
    agent.ioCtx->run();
}
#endif

// ======================== 本地统一模式 (进程内 ChannelTransport, 与远程同路径)
// ========================

/// 建立进程内统一会话: 创建 channel 对 + 服务端协程 + 客户端; 返回客户端供调用方接线并 runSession
static std::shared_ptr<agentxx::agent::remote::RemoteClientAgentIO> setupLocalUnified(
    asio::any_io_executor                        clientEx,
    std::shared_ptr<agentxx::agent::DeepAgent>   agent,
    std::shared_ptr<agentxx::agent::AgentIOBase> io
) {
    auto agentEx         = agent->ioCtx->get_executor();
    auto pair            = agentxx::agent::remote::ChannelTransport::makePair(clientEx, agentEx);
    auto clientTransport = std::move(pair.first);
    auto serverTransport = std::move(pair.second);

    // 进程内可信连接: 关闭鉴权
    agentxx::agent::remote::AgentServer::Config srvCfg;
    srvCfg.autoGenerateToken = false;
    srvCfg.token             = "";
    auto server              = std::make_shared<agentxx::agent::remote::AgentServer>(agent, srvCfg);

    // 服务端在 agent 的 io_context 上: init -> 启动 subagent supervisor -> 服务该传输
    asio::co_spawn(
        *agent->ioCtx,
        [agent, server, st = std::move(serverTransport)]() mutable -> asio::awaitable<void> {
            co_await agent->init();
            agentxx::middleware::SubagentSupervisor supervisor{agent->agentContext};
            co_await supervisor.start();
            co_await server->serveTransport(std::move(st));
            co_return;
        },
        asio::detached
    );

    agentxx::agent::remote::RemoteClientAgentIO::Config cliCfg;
    return std::make_shared<agentxx::agent::remote::RemoteClientAgentIO>(
        clientEx,
        std::move(clientTransport),
        std::move(io),
        cliCfg
    );
}

/// 在独立线程运行 agent 的 io_context, 主线程运行客户端会话
template<typename Coro>
static void runLocalUnifiedMain(std::shared_ptr<agentxx::agent::DeepAgent> agent, Coro coro) {
    auto        work = asio::make_work_guard(*agent->ioCtx);
    std::thread agentThread([&agent]() {
        agent->ioCtx->run();
    });

    asio::io_context clientCtx;
    asio::co_spawn(clientCtx, std::move(coro), asio::detached);
    clientCtx.run();

    work.reset();
    agent->ioCtx->stop();
    if (agentThread.joinable()) {
        agentThread.join();
    }
}

asio::awaitable<void> runLocalCliUnifiedAsync(std::shared_ptr<agentxx::agent::DeepAgent> agent) {
    auto clientEx = co_await asio::this_coro::executor;
    auto io       = std::make_shared<AgentStdIO>();
    auto remote   = setupLocalUnified(clientEx, agent, io);
    XX_OUT("======= Agentxx Client (CLI, in-process unified) =======");
    co_await remote->runSession("session", "");
    co_await remote->shutdown();
}

void runLocalCliUnified(std::shared_ptr<agentxx::agent::DeepAgent> agent) {
    runLocalUnifiedMain(agent, runLocalCliUnifiedAsync(agent));
}

#ifdef AGENTXX_ENABLE_CLIENT_TUI
asio::awaitable<void> runLocalTuiUnifiedAsync(
    std::shared_ptr<agentxx::agent::DeepAgent>   agent,
    std::shared_ptr<agentxx::agent::AgentConfig> config
) {
    auto              clientEx = co_await asio::this_coro::executor;
    const std::string threadId = "session";

    // 最小 AgentContext 供 TUI 渲染 (会话实际状态在 server 侧, 经 delta/sync/统计推送)
    auto ctx         = std::make_shared<agentxx::agent::AgentContext>();
    ctx->agentConfig = config;
    auto tui         = std::make_shared<AgentTUI>(clientEx, ctx, threadId);
    tui->start();

    auto remote = setupLocalUnified(clientEx, agent, tui);

    // 取消键路由到远程 (发送 cancel 消息)
    std::weak_ptr<agentxx::agent::remote::RemoteClientAgentIO> weakRemote = remote;
    tui->setCancelCallback([weakRemote, threadId]() {
        if (auto r = weakRemote.lock()) {
            r->cancel(threadId);
        }
    });
    // 远程上下文统计 -> 更新 TUI 会话显示
    remote->setContextStatsCallback([ctx, threadId](uint64_t c, uint64_t m) {
        auto s = ctx->getSession(threadId);
        if (s && s->contextStats) {
            s->contextStats->contextTokens.store(c, std::memory_order_relaxed);
            s->contextStats->maxContextTokens.store(m, std::memory_order_relaxed);
        }
    });

    co_await remote->runSession(threadId, "");
    co_await remote->shutdown();
    tui->stop();
}

void runLocalTuiUnified(
    std::shared_ptr<agentxx::agent::DeepAgent>   agent,
    std::shared_ptr<agentxx::agent::AgentConfig> config
) {
    runLocalUnifiedMain(agent, runLocalTuiUnifiedAsync(agent, config));
}
#endif

// ======================== 远程客户端 (连接 deepagent WS 服务) ========================

asio::awaitable<void> runRemoteCliAsync(std::string url, std::string token, std::string model) {
    auto ex = co_await asio::this_coro::executor;
    auto io = std::make_shared<AgentStdIO>();

    agentxx::agent::remote::RemoteClientAgentIO::Config cfg;
    agentxx::util::WsClientConfig                       wsCfg;
    wsCfg.recvTimeout = std::chrono::seconds{60};

    auto remote = std::make_shared<agentxx::agent::remote::RemoteClientAgentIO>(
        ex,
        io,
        std::move(url),
        std::move(token),
        cfg,
        wsCfg
    );

    XX_OUT("======= Agentxx Remote Client (CLI, auto-reconnect) =======");
    co_await remote->runSession("session", model);
    co_await remote->shutdown();
}

void runRemoteCli(const std::string& url, const std::string& token, const std::string& model) {
    asio::io_context ctx;
    asio::co_spawn(ctx, runRemoteCliAsync(url, token, model), asio::detached);
    ctx.run();
}

#ifdef AGENTXX_ENABLE_CLIENT_TUI
asio::awaitable<void> runRemoteTuiAsync(
    std::shared_ptr<agentxx::agent::AgentConfig> config,
    std::string                                  url,
    std::string                                  token,
    std::string                                  model
) {
    auto ex = co_await asio::this_coro::executor;

    // 最小 AgentContext (仅供 TUI 渲染状态; 不启动本地引擎/MCP)
    auto ctx         = std::make_shared<agentxx::agent::AgentContext>();
    ctx->agentConfig = config;
    auto io          = std::make_shared<AgentTUI>(ex, ctx, "session");
    io->start();

    agentxx::agent::remote::RemoteClientAgentIO::Config cfg;
    agentxx::util::WsClientConfig                       wsCfg;
    wsCfg.recvTimeout = std::chrono::seconds{60};

    auto remote = std::make_shared<agentxx::agent::remote::RemoteClientAgentIO>(
        ex,
        io,
        std::move(url),
        std::move(token),
        cfg,
        wsCfg
    );

    co_await remote->runSession("session", model);
    co_await remote->shutdown();
    io->stop();
}

void runRemoteTui(
    std::shared_ptr<agentxx::agent::AgentConfig> config,
    const std::string&                           url,
    const std::string&                           token,
    const std::string&                           model
) {
    asio::io_context ctx;
    asio::co_spawn(ctx, runRemoteTuiAsync(config, url, token, model), asio::detached);
    ctx.run();
}
#endif

/// 从 url 查询串提取并移除 token (ws://host:port/path?token=xxx)
static std::string extractTokenFromUrl(std::string& url) {
    auto q = url.find('?');
    if (q == std::string::npos) {
        return "";
    }
    std::string query = url.substr(q + 1);
    url               = url.substr(0, q);
    std::string token;
    size_t      pos = 0;
    while (pos < query.size()) {
        auto        amp = query.find('&', pos);
        std::string kv
            = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        auto eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == "token") {
            token = kv.substr(eq + 1);
        }
        if (amp == std::string::npos) {
            break;
        }
        pos = amp + 1;
    }
    return token;
}

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
    // 远程客户端: --agent ws://host:port/path 连接远程 deepagent 服务
    std::string agentUrl;
    std::string agentToken;
    std::string remoteModel;
    // deepagent 服务: 监听地址/端口/路径
    std::string srvHost = "127.0.0.1";
    std::string wsPath  = "/agent";
    uint16_t    srvPort = 17000;
    std::string sslCertFile;
    std::string sslKeyFile;
    for (int i = 1; i < argn; ++i) {
        std::string arg(argv[i]);
        if (arg == "--config" && i + 1 < argn) {
            ++i;
            configPath = argv[i];
        } else if (arg == "--env" && i + 1 < argn) {
            ++i;
            overrideEnvPath = argv[i];
        } else if (arg == "--agent" && i + 1 < argn) {
            ++i;
            agentUrl = argv[i];
        } else if (arg == "--token" && i + 1 < argn) {
            ++i;
            agentToken = argv[i];
        } else if (arg == "--model" && i + 1 < argn) {
            ++i;
            remoteModel = argv[i];
        } else if (arg == "--host" && i + 1 < argn) {
            ++i;
            srvHost = argv[i];
        } else if (arg == "--port" && i + 1 < argn) {
            ++i;
            srvPort = static_cast<uint16_t>(std::stoi(argv[i]));
        } else if (arg == "--ws-path" && i + 1 < argn) {
            ++i;
            wsPath = argv[i];
        } else if (arg == "--ssl-cert" && i + 1 < argn) {
            ++i;
            sslCertFile = argv[i];
        } else if (arg == "--ssl-key" && i + 1 < argn) {
            ++i;
            sslKeyFile = argv[i];
        } else if (mode == "tui") {
            mode = arg;
        } else if (mode == "cli") {
            mode = arg;
        }
    }

    // token 亦可经 url 查询串携带: ws://host:port/path?token=xxx
    if (!agentUrl.empty() && agentToken.empty()) {
        agentToken = extractTokenFromUrl(agentUrl);
    }

    // 加载覆盖式 env 文件（--env，最高优先级）
    std::map<std::string, std::string> overrideEnvVars;
    if (!overrideEnvPath.empty()) {
        overrideEnvVars = loadOverrideEnv(overrideEnvPath);
        XX_OUT(
            "[Config] Loaded {} override variables from: {}",
            overrideEnvVars.size(),
            overrideEnvPath
        );
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
            XX_LOGE("[Config] Failed to load config: {}", e.what());
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
            asio::detached
        );
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

    // ======================== deepagent WS 服务模式 ========================
    if (mode == "deepagent") {
        config->logPringToolcall                       = false;
        config->logPrintMessagesBeforeLLM              = true;
        config->logPrintMessagesBeforeLLMWithSystemMsg = false;
        config->logPrintSummarizationResultTokenCount  = true;

        auto agent = std::make_shared<agentxx::agent::DeepAgent>(config);

        agentxx::agent::remote::AgentServer::Config srvCfg;
        srvCfg.http.address     = srvHost;
        srvCfg.http.port        = srvPort;
        srvCfg.http.sslCertFile = sslCertFile;
        srvCfg.http.sslKeyFile  = sslKeyFile;
        srvCfg.wsPath           = wsPath;
        srvCfg.token            = agentToken; // 空则自动生成
        auto server = std::make_shared<agentxx::agent::remote::AgentServer>(agent, srvCfg);

        asio::co_spawn(
            *agent->ioCtx,
            [agent, server]() -> asio::awaitable<void> {
                co_await agent->init();
                server->start(co_await asio::this_coro::executor);
                co_return;
            },
            asio::detached
        );
        // 优雅退出: SIGINT/SIGTERM -> 停止 accept 并退出 io_context
        asio::co_spawn(
            *agent->ioCtx,
            [agent, server]() -> asio::awaitable<void> {
                asio::signal_set          signals(*agent->ioCtx, SIGINT, SIGTERM);
                boost::system::error_code ec;
                co_await signals.async_wait(asio::redirect_error(asio::use_awaitable, ec));
                XX_OUT("[agent_server] signal received, shutting down...");
                server->stop();
                agent->ioCtx->stop();
                co_return;
            },
            asio::detached
        );
        agent->ioCtx->run();
        return 0;
    }

    // ======================== 远程客户端模式 (--agent) ========================
    if (!agentUrl.empty()) {
        if (mode == "tui") {
#if AGENTXX_ENABLE_CLIENT_TUI
            runRemoteTui(config, agentUrl, agentToken, remoteModel);
#else
            XX_LOGE(
                R"(TUI not supported; recompile with `AGENTXX_ENABLE_CLIENT_TUI=1` or use cli mode)"
            );
#endif
        } else {
            runRemoteCli(agentUrl, agentToken, remoteModel);
        }
        return 0;
    }

    if (mode == "tui") {
#if AGENTXX_ENABLE_CLIENT_TUI
        config->logPringToolcall                       = false;
        config->logPrintMessagesBeforeLLM              = true;
        config->logPrintMessagesBeforeLLMWithSystemMsg = false;
        config->logPrintSummarizationResultTokenCount  = true;
        auto agent = std::make_shared<agentxx::agent::DeepAgent>(config);
        runLocalTuiUnified(agent, config);
#else
        XX_LOGE(
            R"(TUI is not support! Please set `AGENTXX_ENABLE_CLIENT_TUI=1` and recomplie agentxx_cli)"
        );
#endif
        return 0;
    }

    {
        // 默认 CLI 交互模式 (进程内统一路径)
        config->logPringToolcall                      = true;
        config->logPrintMessagesBeforeLLM             = true;
        config->logPrintSummarizationResultTokenCount = true;
        auto agent = std::make_shared<agentxx::agent::DeepAgent>(config);
        runLocalCliUnified(agent);
    }
    return 0;
}
