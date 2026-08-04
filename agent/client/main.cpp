#include "agentxx-client/config_loader.h"
#include "agentxx-client/io/stdio/agent_stdio.h"
#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/mode_runners.h"
#include "agentxx-client/train/train.h"
#include "agentxx-client/util/util.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/io/agent_server.h"
#include "agentxx/protocol/acp_server.h"
#include "agentxx/util/exception.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/signal_set.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

using namespace agentxx::client;

// ASan 默认选项 (仅在链接了 AddressSanitizer 时生效;  release/无 ASan 构建中为无害的弱符号)。
//
// 背景: TUI 全屏模式会切到备用屏 (alternate screen) + raw 模式 + 鼠标捕获。
// ASan 检测到错误后默认以 _exit() 结束进程, 绕过 ftxui 注册的退出/信号清理,
// 导致终端停留在备用屏/raw 模式: 备用屏无回滚缓冲、raw 模式滚轮失效 -> 终端"卡住",
// 且备用屏上的 ASan 报告无法滚动查看完整。
//
// 对策:
// - abort_on_error=1: ASan 改用 abort() 结束 -> 触发 SIGABRT -> ftxui 的信号处理
//   恢复终端 (退出备用屏/恢复 termios/显示光标), 终端不再卡住。
// - log_path=agentxx_asan: ASan 报告写入工作目录下 agentxx_asan.<pid> 文件
//   (恢复终端会退出备用屏, 屏上报告随之消失, 故必须落盘才能看完整报告)。
//   崩溃后用 `less agentxx_asan.*` 查看。
// 注: 环境变量 ASAN_OPTIONS 会整体覆盖此默认值。
extern "C" const char* __asan_default_options() {
    return "abort_on_error=1:log_path=agentxx_asan";
}

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

    /// 默认启动 stdio 作为日志输出，对于 tui 等自己拦截日志的可以移除后添加自己的日志拦截器
    auto defaultLogSink = std::make_shared<StderrLogSink>();
    agentxx::util::LogDispatcher::instance().addSink(defaultLogSink);

    std::string configPath = "agentxx-config.yaml";
    std::string overrideEnvPath;
    std::string mode = "tui";
    std::string agentUrl;
    std::string agentToken;
    std::string remoteModel;
    std::string srvHost = "127.0.0.1";
    uint16_t    srvPort = 7007;
    std::string sslCertFile;
    std::string sslKeyFile;
    for (int i = 1; i < argn; ++i) {
        auto arg = std::string_view{argv[i]};
        if (arg == "--help" || arg == "-h") {
            XX_OUT(R"_(
Usage: agentxx_cli [mode] [options]

Modes:
    tui                  TUI 交互模式 (默认)
    cli                  命令行 stdio 交互模式
    server               启动 WebSocket agent 服务
    acp                  ACP stdio 服务模式
    train                训练模式

Options:
    -h, --help           显示帮助信息
    --config <path>      配置文件路径 (默认: agentxx-config.yaml)
    --env <path>         覆盖式环境变量文件路径
    --agent <url>        远程 agent server 地址 (ws://host:port/agent)
    --token <token>      认证 token (也可通过 url 查询串携带)
    --model <model>      远程模型名称
    --host <host>        服务监听地址 (默认: 127.0.0.1)
    --port <port>        服务监听端口 (默认: 7007)
    --ssl-cert <file>    SSL 证书文件路径
    --ssl-key <file>     SSL 私钥文件路径
)_");
            return 0;
        } else if (arg == "--config" && i + 1 < argn) {
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
            // 容错解析: 非法值报错退出, 避免 std::stoi 抛异常崩溃
            auto portArg = std::string_view{argv[i]};
            if (auto r = agentxx::util::parseNumberFromString(portArg, srvPort);
                r.ec != std::errc{} || srvPort == 0) {
                XX_LOGE("Invalid --port value: `{}`", portArg);
                return 1;
            }
        } else if (arg == "--ssl-cert" && i + 1 < argn) {
            ++i;
            sslCertFile = argv[i];
        } else if (arg == "--ssl-key" && i + 1 < argn) {
            ++i;
            sslKeyFile = argv[i];
        } else if (arg == "tui" || arg == "cli" || arg == "server" || arg == "acp"
                   || arg == "train") {
            mode = arg;
        } else {
            XX_LOGE("Unknown arg: `{}`", arg);
        }
    }

    // token 可经 url 查询串携带: ws://host:port/path?token=xxx
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
    if (!configPath.empty() && std::filesystem::exists(configPath)) {
        auto code = agentxx::util::catchError<int>(
            [&]() -> int {
                yamlCfg = loadYamlConfig(configPath, dotEnvVars, overrideEnvVars);
                XX_OUT("[Config] Loaded config from: {}", configPath);
                return 0;
            },
            [](std::string errmsg) -> int {
                XX_LOGE("[Config] Failed to load config: {}", errmsg);
                return 1;
            }
        );
        if (0 != code) {
            return code;
        }
    }

    if (mode == "train") {
        auto config                                    = buildDefaultConfig();
        config->logPrintToolcall                       = false;
        config->logPrintMessagesBeforeLLM              = false;
        config->logPrintMessagesBeforeLLMWithSystemMsg = false;
        config->logPrintSummarizationResultTokenCount  = false;
        applyModelToConfig(config, yamlCfg.models, yamlCfg.useModelTrain);

        auto scorerConfig                                    = buildDefaultConfig();
        scorerConfig->logPrintToolcall                       = false;
        scorerConfig->logPrintMessagesBeforeLLM              = false;
        scorerConfig->logPrintMessagesBeforeLLMWithSystemMsg = false;
        scorerConfig->logPrintSummarizationResultTokenCount  = false;
        applyModelToConfig(scorerConfig, yamlCfg.models, yamlCfg.useModelTrainScorer);

        auto optimizerConfig                                    = buildDefaultConfig();
        optimizerConfig->logPrintToolcall                       = false;
        optimizerConfig->logPrintMessagesBeforeLLM              = false;
        optimizerConfig->logPrintMessagesBeforeLLMWithSystemMsg = false;
        optimizerConfig->logPrintSummarizationResultTokenCount  = false;
        applyModelToConfig(optimizerConfig, yamlCfg.models, yamlCfg.useModelTrainOptimizer);

        runTrainingMode(config, scorerConfig, optimizerConfig);
        return 0;
    }

    if (mode == "acp") {
        auto config                                   = buildDefaultConfig();
        config->logPrintToolcall                      = false;
        config->logPrintMessagesBeforeLLM             = false;
        config->logPrintSummarizationResultTokenCount = false;
        applyModelToConfig(config, yamlCfg.models, yamlCfg.useModelAcp);
        applySubagentModelToConfig(config, yamlCfg.models, yamlCfg.useModelSubagent);
        applyWebSearchModelToConfig(config, yamlCfg.models, yamlCfg.useModelWebSearch);
        auto agent = std::make_shared<agentxx::agent::CodeAgent>(config);
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

    // 解析路径：相对路径按程序工作目录解析为绝对路径
    auto resolvePath = [](std::string_view p) -> std::string {
        std::filesystem::path fp{p};
        if (fp.is_absolute()) {
            return fp.lexically_normal().string();
        }
        return (std::filesystem::current_path() / fp).lexically_normal().string();
    };

    for (const auto& p : yamlCfg.skillDirPaths) {
        config->skillDirPaths.push_back(resolvePath(p));
    }
    for (const auto& p : yamlCfg.memoryFilePaths) {
        config->memoryFilePaths.push_back(resolvePath(p));
    }

    // ======================== CodeAgent Websocket Server 服务模式 ========================
    if (mode == "server") {
        config->logPrintToolcall                       = false;
        config->logPrintMessagesBeforeLLM              = true;
        config->logPrintMessagesBeforeLLMWithSystemMsg = false;
        config->logPrintSummarizationResultTokenCount  = true;

        auto agent = std::make_shared<agentxx::agent::CodeAgent>(config);

        agentxx::agent::io::AgentServer::Config srvCfg;
        srvCfg.http.address     = srvHost;
        srvCfg.http.port        = srvPort;
        srvCfg.http.sslCertFile = sslCertFile;
        srvCfg.http.sslKeyFile  = sslKeyFile;
        srvCfg.token            = agentToken;
        auto server             = std::make_shared<agentxx::agent::io::AgentServer>(agent, srvCfg);

        asio::co_spawn(
            *agent->ioCtx,
            [agent, server]() -> asio::awaitable<void> {
                co_await agent->init();
                server->start(co_await asio::this_coro::executor);
                co_return;
            },
            asio::detached
        );
        asio::co_spawn(
            *agent->ioCtx,
            [agent, server]() -> asio::awaitable<void> {
                asio::signal_set         signals(*agent->ioCtx, SIGINT, SIGTERM);
                neograph_asio_error_code ec;
                co_await signals.async_wait(asio::redirect_error(asio::use_awaitable, ec));
                XX_OUT("[agent_server] signal received, shutting down ({})...", ec.message());
                server->stop();
                agent->ioCtx->stop();
                co_return;
            },
            asio::detached
        );
        agent->ioCtx->run();
        return 0;
    }

    // ======================== 远程 client + agent server模式 (--agent) ========================
    // client 和 agent 不在同一个进程中，使用网络交互
    if (!agentUrl.empty()) {
        agentxx::util::LogDispatcher::instance().removeSink(defaultLogSink);
        if (mode == "tui") {
            runRemoteTui(config, agentUrl, agentToken, remoteModel);
        } else {
            runRemoteCli(agentUrl, agentToken, remoteModel);
        }
        return 0;
    }

    // ======================== 同一进程内 client + agent 模式 ========================
    // client 和 agent 在同一个进程中，使用线程间数据交互
    if (mode == "tui") {
        agentxx::util::LogDispatcher::instance().removeSink(defaultLogSink);
        config->logPrintToolcall                       = false;
        config->logPrintMessagesBeforeLLM              = false;
        config->logPrintMessagesBeforeLLMWithSystemMsg = false;
        config->logPrintSummarizationResultTokenCount  = true;
        auto agent = std::make_shared<agentxx::agent::CodeAgent>(config);
        runLocalTuiUnified(agent, config);
    } else {
        agentxx::util::LogDispatcher::instance().removeSink(defaultLogSink);
        config->logPrintToolcall                       = false;
        config->logPrintMessagesBeforeLLM              = false;
        config->logPrintMessagesBeforeLLMWithSystemMsg = false;
        config->logPrintSummarizationResultTokenCount  = false;
        auto agent = std::make_shared<agentxx::agent::CodeAgent>(config);
        runLocalCliUnified(agent);
    }
    return 0;
}