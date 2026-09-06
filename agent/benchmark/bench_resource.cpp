#include "bench_resource.h"
#include "bench_resource_util.h"
#include "bench_util.h"

#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/channel_io_transport.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/ffi_api.h"
#include "agentxx/util/env.h"
#include "agentxx/util/http_server.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"

#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/ip/tcp.hpp"
#include "asio/post.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if XX_IS_WIN_D
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "agentxx/plugin/plugin_manager.h"

#ifdef AGENTXX_BUILD_CLIENT
#include "agentxx-client/io/stdio/agent_stdio.h"
#include "agentxx-client/io/stdio/cli_plugin_adapter.h"
#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/tui_plugin_adapter.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx/agent/io/ws_io_transport.h"
#include "agentxx/plugin/client_plugin_manager.h"
#endif

namespace agentxx {
namespace bench {

namespace {

// ---------------------------------------------------------------------------
// 辅助函数: 生成唯一 session id / 临时测试目录 / 端口查找
// ---------------------------------------------------------------------------

std::string generateBenchSessionId() {
    static std::atomic<uint64_t> seq{0};
    auto                         now = std::chrono::steady_clock::now().time_since_epoch().count();
    return fmt::format("bench_sess_{}_{}", now, seq.fetch_add(1));
}

std::filesystem::path createBenchTempDir(const std::string& prefix) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto            base = fs::temp_directory_path(ec);
    if (ec) {
        base = fs::current_path(ec);
    }
    static std::atomic<uint32_t> seq{0};
    auto                         name = fmt::format(
        "{}_{}_{}",
        prefix,
        static_cast<long>(
#if XX_IS_WIN_D
            ::GetCurrentProcessId()
#else
            getpid()
#endif
        ),
        seq.fetch_add(1)
    );
    fs::path dir = base / name;
    fs::create_directories(dir, ec);

    // 在临时目录下写入 README.md, 内容即固定 512B tool 结果载荷,
    // 保证后续 agentxx_filesystem_read 工具真实执行时结果与固定模板一致
    std::ofstream ofs(dir / "README.md", std::ios::binary | std::ios::trunc);
    if (ofs.is_open()) {
        auto payload = getFixedToolResultPayload();
        ofs.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        ofs.close();
    }
    return dir;
}

uint16_t findFreeTcpPort() {
    asio::io_context        ctx;
    asio::ip::tcp::acceptor acceptor(ctx);
    asio::ip::tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), 0);
    acceptor.open(ep.protocol());
    acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor.bind(ep);
    uint16_t port = acceptor.local_endpoint().port();
    acceptor.close();
    return port;
}

// ---------------------------------------------------------------------------
// 模拟 LLM HTTP 服务 (ResourceLlmSimServer)
// ---------------------------------------------------------------------------

struct ResourceLlmSimServer {
    std::unique_ptr<agentxx::util::HttpServer> svr;
    std::thread                                thr;
    uint16_t                                   port  = 0;
    std::shared_ptr<std::atomic<size_t>> turnCounter = std::make_shared<std::atomic<size_t>>(0);

    ResourceLlmSimServer() = default;

    ResourceLlmSimServer(ResourceLlmSimServer&& o) noexcept :
        svr(std::move(o.svr)),
        thr(std::move(o.thr)),
        port(o.port),
        turnCounter(std::move(o.turnCounter)) {
        o.port = 0;
    }

    ResourceLlmSimServer& operator=(ResourceLlmSimServer&& o) noexcept {
        if (this != &o) {
            stop();
            svr         = std::move(o.svr);
            thr         = std::move(o.thr);
            port        = o.port;
            turnCounter = std::move(o.turnCounter);
            o.port      = 0;
        }
        return *this;
    }

    ResourceLlmSimServer(const ResourceLlmSimServer&)            = delete;
    ResourceLlmSimServer& operator=(const ResourceLlmSimServer&) = delete;

    ~ResourceLlmSimServer() {
        stop();
    }

    void stop() {
        if (svr) {
            svr->stop();
        }
        if (thr.joinable()) {
            thr.join();
        }
        svr.reset();
        port = 0;
    }
};

ResourceLlmSimServer startResourceLlmSimServer() {
    ResourceLlmSimServer sim;

    agentxx::util::HttpServer::Config cfg;
    cfg.address          = "127.0.0.1";
    cfg.port             = 0;
    cfg.ioThreads        = 1;
    cfg.accessLogEnabled = false;
    cfg.maxConnections   = 128;
    cfg.maxRequestBody   = 10 * 1024 * 1024;

    sim.svr           = std::make_unique<agentxx::util::HttpServer>(cfg);
    auto* rawSvr      = sim.svr.get();
    auto  turnCounter = sim.turnCounter;

    // GET /health
    rawSvr->router().add(
        "/health",
        1,
        std::make_shared<agentxx::util::HttpServer::Handler>(
            [](agentxx::util::HttpServer::Request&,
               agentxx::util::HttpServer::Response& resp,
               std::string_view) -> asio::awaitable<void> {
                namespace http = boost::beast::http;
                resp.result(http::status::ok);
                resp.set(http::field::content_type, "application/json");
                resp.body() = "{\"status\":\"ok\"}";
                resp.prepare_payload();
                co_return;
            }
        )
    );

    auto chatHandler = std::make_shared<agentxx::util::HttpServer::Handler>(
        [turnCounter](
            agentxx::util::HttpServer::Request&  req,
            agentxx::util::HttpServer::Response& resp,
            std::string_view
        ) -> asio::awaitable<void> {
            namespace http = boost::beast::http;

            std::string_view body = req.body();
            bool             stream
                = (body.find("\"stream\":true") != std::string_view::npos
                   || body.find("\"stream\": true") != std::string_view::npos);

            // 判断是否为预热轮次
            bool isWarmup = (body.find("RES-BENCH") == std::string_view::npos);

            bool lastIsTool  = false;
            auto lastRolePos = body.rfind("\"role\"");
            if (lastRolePos != std::string_view::npos) {
                auto roleSub
                    = body.substr(lastRolePos, std::min<size_t>(body.size() - lastRolePos, 40));
                if (roleSub.find("\"tool\"") != std::string_view::npos) {
                    lastIsTool = true;
                }
            }

            neograph::json toolCalls = neograph::json::array();
            std::string    replyContent;

            if (isWarmup) {
                replyContent = "Hello! Ready for benchmarking.";
                turnCounter->fetch_add(1);
            } else if (lastIsTool) {
                // tool 结果回来, assistant 返回摘要 (本轮正式结束)
                replyContent
                    = "RES-BENCH assist summary | 已成功读取 README.md 前 40 行内容，并完成分析任务。";
                turnCounter->fetch_add(1);
            } else {
                // user 请求, assistant 返回 tool_call: agentxx_filesystem_read
                neograph::json tc;
                tc["id"]       = "call-000001";
                tc["type"]     = "function";
                tc["function"] = {
                    {"name",      "agentxx_filesystem_read"                                     },
                    {"arguments", "{\"path\":\"README.md\",\"line_offset\":0,\"line_limit\":40}"}
                };
                toolCalls.push_back(tc);
            }

            bool hasToolCalls = !toolCalls.empty();

            if (stream) {
                std::string sseBody;
                auto append = [&](const neograph::json& delta, const std::string& finishReason) {
                    neograph::json ev;
                    ev["id"]      = "chatcmpl-bench-sim";
                    ev["object"]  = "chat.completion.chunk";
                    ev["created"] = 1234567890;
                    ev["model"]   = "bench-sim";

                    neograph::json choice;
                    choice["index"] = 0;
                    choice["delta"] = delta;
                    if (finishReason.empty()) {
                        choice["finish_reason"] = nullptr;
                    } else {
                        choice["finish_reason"] = finishReason;
                    }
                    ev["choices"]  = neograph::json::array({choice});
                    sseBody       += "data: " + ev.dump() + "\n\n";
                };

                neograph::json d;
                d["role"] = "assistant";
                if (hasToolCalls) {
                    d["content"] = nullptr;
                    append(d, "");
                    neograph::json dTc;
                    dTc["tool_calls"] = toolCalls;
                    append(dTc, "");
                    append(neograph::json::object(), "tool_calls");
                } else {
                    d["content"] = replyContent;
                    append(d, "");
                    append(neograph::json::object(), "stop");
                }

                sseBody += "data: [DONE]\n\n";
                resp.result(http::status::ok);
                resp.set(http::field::content_type, "text/event-stream");
                resp.set(http::field::cache_control, "no-cache");
                resp.body() = std::move(sseBody);
                resp.prepare_payload();
            } else {
                neograph::json msg;
                msg["role"] = "assistant";
                if (hasToolCalls) {
                    msg["content"]    = nullptr;
                    msg["tool_calls"] = toolCalls;
                } else {
                    msg["content"] = replyContent;
                }

                neograph::json choice;
                choice["index"]         = 0;
                choice["message"]       = msg;
                choice["finish_reason"] = hasToolCalls ? "tool_calls" : "stop";

                neograph::json respJson;
                respJson["id"]      = "chatcmpl-bench-sim";
                respJson["object"]  = "chat.completion";
                respJson["created"] = 1234567890;
                respJson["model"]   = "bench-sim";
                respJson["choices"] = neograph::json::array({choice});
                respJson["usage"]   = {
                    {"prompt_tokens",     100},
                    {"completion_tokens", 50 },
                    {"total_tokens",      150}
                };

                resp.result(http::status::ok);
                resp.set(http::field::content_type, "application/json");
                resp.body() = respJson.dump();
                resp.prepare_payload();
            }
            co_return;
        }
    );

    // 兼顾带 /v1 与不带 /v1 的请求路径
    rawSvr->router().add("/v1/chat/completions", 2, chatHandler);
    rawSvr->router().add("/chat/completions", 2, chatHandler);

    sim.thr = std::thread([rawSvr]() {
        rawSvr->start();
    });

    for (int i = 0; i < 100; ++i) {
        sim.port = rawSvr->port();
        if (sim.port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return sim;
}

// ---------------------------------------------------------------------------
// 跨平台子进程执行器 (用于 M3 / M4)
// ---------------------------------------------------------------------------

struct ProcessHandle {
#if XX_IS_WIN_D
    HANDLE hProcess    = nullptr;
    HANDLE hThread     = nullptr;
    HANDLE hStdinWrite = nullptr;
    DWORD  pid         = 0;
#else
    pid_t pid          = 0;
    int   stdinWriteFd = -1;
#endif
    bool running = false;
};

ProcessHandle spawnChildProcess(
    const std::string&              exePath,
    const std::vector<std::string>& args,
    const std::string&              workingDir = ""
) {
    ProcessHandle ph;
#if XX_IS_WIN_D
    HANDLE              hStdinRead  = nullptr;
    HANDLE              hStdinWrite = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (::CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0)) {
        ::SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0);
        ph.hStdinWrite = hStdinWrite;
    }

    std::string cmd = "\"" + exePath + "\"";
    for (const auto& a : args) {
        cmd += " \"" + a + "\"";
    }
    STARTUPINFOA si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = hStdinRead ? hStdinRead : ::GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = INVALID_HANDLE_VALUE;
    si.hStdError  = INVALID_HANDLE_VALUE;

    PROCESS_INFORMATION pi{};
    BOOL                ok = ::CreateProcessA(
        nullptr,
        cmd.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        workingDir.empty() ? nullptr : workingDir.c_str(),
        &si,
        &pi
    );
    if (hStdinRead) {
        ::CloseHandle(hStdinRead);
    }
    if (ok) {
        ph.hProcess = pi.hProcess;
        ph.hThread  = pi.hThread;
        ph.pid      = pi.dwProcessId;
        ph.running  = true;
    }
#else
    int pfd[2] = {-1, -1};
    if (pipe(pfd) == 0) {
        ph.stdinWriteFd = pfd[1];
    }

    pid_t p = fork();
    if (p == 0) {
        if (!workingDir.empty()) {
            if (chdir(workingDir.c_str()) != 0) {
                // ignore
            }
        }
        if (pfd[0] >= 0) {
            dup2(pfd[0], STDIN_FILENO);
            close(pfd[0]);
            if (pfd[1] >= 0) {
                close(pfd[1]);
            }
        }
        int devNull = open("/dev/null", O_WRONLY);
        if (devNull >= 0) {
            dup2(devNull, STDOUT_FILENO);
            dup2(devNull, STDERR_FILENO);
            close(devNull);
        }
        std::vector<char*> cargs;
        cargs.push_back(const_cast<char*>(exePath.c_str()));
        for (const auto& a : args) {
            cargs.push_back(const_cast<char*>(a.c_str()));
        }
        cargs.push_back(nullptr);
        execvp(exePath.c_str(), cargs.data());
        _exit(127);
    } else if (p > 0) {
        if (pfd[0] >= 0) {
            close(pfd[0]);
        }
        ph.pid     = p;
        ph.running = true;
    }
#endif
    return ph;
}

void stopChildProcess(ProcessHandle& ph) {
    if (!ph.running) {
        return;
    }
#if XX_IS_WIN_D
    if (ph.hStdinWrite) {
        ::CloseHandle(ph.hStdinWrite);
        ph.hStdinWrite = nullptr;
    }
    if (ph.hProcess) {
        ::TerminateProcess(ph.hProcess, 0);
        ::WaitForSingleObject(ph.hProcess, 3000);
        ::CloseHandle(ph.hProcess);
        ::CloseHandle(ph.hThread);
        ph.hProcess = nullptr;
        ph.hThread  = nullptr;
    }
#else
    if (ph.stdinWriteFd >= 0) {
        close(ph.stdinWriteFd);
        ph.stdinWriteFd = -1;
    }
    if (ph.pid > 0) {
        kill(ph.pid, SIGTERM);
        for (int i = 0; i < 20; ++i) {
            int   status = 0;
            pid_t res    = waitpid(ph.pid, &status, WNOHANG);
            if (res != 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        kill(ph.pid, SIGKILL);
        int status = 0;
        waitpid(ph.pid, &status, WNOHANG);
    }
#endif
    ph.running = false;
}

std::string findAgentxxCliPath() {
    namespace fs = std::filesystem;
    std::error_code       ec;
    std::vector<fs::path> candidates;
#if XX_IS_WIN_D
    const std::string exeName = "agentxx_cli.exe";
    wchar_t           buf[MAX_PATH];
    if (::GetModuleFileNameW(nullptr, buf, MAX_PATH) > 0) {
        auto parent = fs::path(buf).parent_path();
        candidates.push_back(parent / exeName);
    }
#else
    const std::string exeName = "agentxx_cli";
    if (auto p = fs::read_symlink("/proc/self/exe", ec); !ec) {
        auto parent = p.parent_path();
        candidates.push_back(parent / exeName);
    }
#endif
    auto cwd = fs::current_path(ec);
    candidates.push_back(cwd / exeName);
    candidates.push_back(cwd / "exec" / exeName);
    candidates.push_back(cwd / "agent" / "build" / "linux-release" / "exec" / exeName);
    candidates.push_back(cwd / "agent" / "build" / "linux-debug" / "exec" / exeName);

    for (const auto& c : candidates) {
        if (fs::exists(c, ec) && !fs::is_directory(c, ec)) {
            return c.string();
        }
    }
    return "";
}

// ---------------------------------------------------------------------------
// 插件装配辅助: 构建 5 常用插件配置并校验
// ---------------------------------------------------------------------------

struct PluginsLoadPlan {
    std::vector<agent::PluginConfig> agentConfigs;
    size_t                           foundCount = 0;
};

PluginsLoadPlan prepare5Plugins() {
    PluginsLoadPlan plan;
    const auto&     names = getBench5PluginNames();
    for (const auto& name : names) {
        std::string         dir = resolveBenchPluginDir(name);
        agent::PluginConfig pc;
        pc.path    = dir;
        pc.enabled = true;
        pc.sides   = agent::PluginSide::Auto;
        plan.agentConfigs.push_back(pc);
        if (dir.rfind("builtin://", 0) != 0 || std::filesystem::exists(dir)) {
            ++plan.foundCount;
        }
    }
    return plan;
}

} // namespace

// ===========================================================================
// M1: 同一进程 CLI 资源测试
// ===========================================================================

void benchResourceCli() {
#ifndef AGENTXX_BUILD_CLIENT
    std::cout << "  [resource][cli] skipped: AGENTXX_BUILD_CLIENT not enabled" << std::endl;
    return;
#else
    std::cout << "\n=== Resource Benchmark: M1 In-Process CLI ===" << std::endl;

    auto        sim      = startResourceLlmSimServer();
    auto        tmpDir   = createBenchTempDir("bench_m1_cli");
    auto&       reporter = BenchReporter::instance();
    const auto& counts   = getCalibratedCounts();

    auto agentConfig                = std::make_shared<agent::AgentConfig>();
    agentConfig->dataDir            = (tmpDir / "data").string();
    agentConfig->workDir            = tmpDir.string();
    agentConfig->permissionMode     = agent::PermissionMode::Pass;
    agentConfig->enableSessionStore = false;
    agentConfig->enableSubagent     = false;
    agentConfig->enableWorktree     = false;

    // 模型配置: 指向本地 mock LLM
    agent::ModelConfig mc;
    mc.name                               = "bench-sim";
    mc.type                               = "openai";
    mc.baseUrl                            = fmt::format("http://127.0.0.1:{}/v1", sim.port);
    mc.apiKey                             = "EMPTY";
    mc.modelName                          = "bench-sim";
    mc.modelContenxtMaxToken              = 8 << 20; // 8M 确保不触发压缩/截断
    agentConfig->model                    = mc;
    agentConfig->availableModels[mc.name] = mc;
    agentConfig->currentModelName         = mc.name;

    // 5 常用插件
    auto pluginPlan      = prepare5Plugins();
    agentConfig->plugins = pluginPlan.agentConfigs;

    auto        agent     = std::make_shared<agent::CodeAgent>(agentConfig);
    auto        agentWork = asio::make_work_guard(*agent->ioCtx);
    std::thread agentThread([agent]() {
        agent->ioCtx->run();
    });

    asio::io_context clientCtx;
    auto             clientEx  = clientCtx.get_executor();
    auto             io        = std::make_shared<StdIOClientAgentIO>();
    std::string      sessionId = generateBenchSessionId();
    io->setSessionId(sessionId);

    // Client 插件管理器
    auto clientPlugins = agentConfig->plugins;
    auto pluginMgr     = std::make_shared<agentxx::plugin::ClientPluginManager>(clientEx);
    pluginMgr->setUiAdapter(std::make_shared<agentxx::client::CliPluginAdapter>(io));
    pluginMgr->setSessionId(sessionId);
    io->setEventSink(pluginMgr);

    // 加载 client 插件 (同步等待完成)
    std::atomic<bool> pluginsLoaded{false};
    asio::co_spawn(
        clientEx,
        [&]() -> asio::awaitable<void> {
            co_await pluginMgr->loadConfiguredClientPlugins(clientPlugins);
            pluginsLoaded.store(true);
            co_return;
        },
        asio::detached
    );
    while (!pluginsLoaded.load()) {
        clientCtx.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // 装配 Channel 直连
    auto agentEx = agent->ioCtx->get_executor();
    auto [clientTransport, serverTransport]
        = agent::ChannelAgentIOTransport::makePair(clientEx, agentEx);
    io->setTransport(std::shared_ptr<agent::AgentIOTransportBase>(std::move(clientTransport)));

    agent::SessionServerAgentIO::Config scCfg;
    scCfg.sessionId            = sessionId;
    scCfg.initialSyncTailCount = 0; // CLI: 全量同步
    auto serverIO = std::make_shared<agent::SessionServerAgentIO>(agentEx, agent, scCfg);
    serverIO->setTransport(std::shared_ptr<agent::AgentIOTransportBase>(std::move(serverTransport))
    );

    std::atomic<bool> serverReady{false};
    if (auto ctx = agent->agentContext) {
        ctx->initNotifier = [io](std::string_view step) {
            io->onServerProgress(step);
        };
    }

    asio::co_spawn(
        *agent->ioCtx,
        [serverIO]() -> asio::awaitable<void> {
            co_await serverIO->runTransportLoop();
        },
        asio::detached
    );

    asio::co_spawn(
        *agent->ioCtx,
        [&]() -> asio::awaitable<void> {
            co_await agent->init();
            io->requestAppendComponentInfo(sessionId);
            io->onServerReady();
            serverReady.store(true);
            co_await serverIO->run();
        },
        asio::detached
    );

    asio::co_spawn(
        clientEx,
        [io]() -> asio::awaitable<void> {
            co_await io->runTransportLoop();
        },
        asio::detached
    );

    // 启动 client 线程运行 clientCtx
    auto        clientWork = asio::make_work_guard(clientCtx);
    std::thread clientThread([&clientCtx]() {
        clientCtx.run();
    });

    // 等待 serverReady
    while (!serverReady.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 发送 hello 建立初始同步
    io->sendToPeer(agent::WireHello{sessionId, "", 0, ""});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 首轮短 turn 预热
    std::atomic<bool> turnDone{false};
    auto              origSink = io->eventSink();
    // 监听 turnEnd 事件
    io->sendToPeer(agent::WireUserInput{sessionId, "hello"});
    // 等待首轮执行完毕
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        bool idle = false;
        asio::post(*agent->ioCtx, [&]() {
            if (auto sess = agent->agentContext->getSession(sessionId)) {
                idle = (sess->activity == agent::SessionActivity::Idle);
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (idle && i >= 2) {
            break;
        }
    }

    // ---------------- P0: Startup ----------------
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto idleCpuWin = cpuBegin(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    double idleCpu = cpuEnd(idleCpuWin);
    auto   mem0    = sampleMemoryMedian(0);

    ResourceResult res0;
    res0.mode          = "cli";
    res0.side          = "self";
    res0.point         = "startup";
    res0.rssMB         = mem0.rssMB;
    res0.privateMB     = mem0.privateMB;
    res0.cpuIdlePct    = idleCpu;
    res0.cpuBusyPct    = 0.0;
    res0.pluginsAgent  = agent->agentContext->pluginManager->list().size();
    res0.pluginsClient = pluginMgr->list().size();
    res0.note          = "headless-cli, Channel, tail=0";

    {
        std::promise<void> p;
        asio::post(*agent->ioCtx, [&]() {
            if (auto sess = agent->agentContext->getSession(sessionId)) {
                res0.viewCount = sess->viewMessages.size();
                res0.viewBytes = estimateViewMessagesBytes(sess->viewMessages);
                res0.llmCount  = sess->llmMessages.size();
                res0.llmBytes  = estimateLlmMessagesBytes(sess->llmMessages);
                res0.tokens    = 150; // 预热痕量
            }
            p.set_value();
        });
        p.get_future().wait();
    }
    reporter.addResource(res0);
    printResourceResult(res0);

    // ---------------- P1: ~100K 上下文 ----------------
    auto              busyWin1 = cpuBegin(0);
    std::atomic<bool> p1Injected{false};
    asio::post(*agent->ioCtx, [&]() {
        if (auto sess = agent->agentContext->getSession(sessionId)) {
            for (size_t i = 0; i < counts.n100; ++i) {
                auto g = makeFixedGroup(i + 1);
                sess->appendViewMessage(g.viewUser);
                sess->appendViewMessage(g.viewTool);
                sess->appendViewMessage(g.viewAssist);

                sess->llmMessages.push_back({
                    {"role",    "user"           },
                    {"content", g.userMsg.content}
                });
                sess->llmMessages.push_back({
                    {"role",       "assistant"                                 },
                    {"content",    nullptr                                     },
                    {"tool_calls",
                     {{{"id", g.assistMsg.tool_calls[0].id},
                       {"type", "function"},
                       {"function",
                        {{"name", g.assistMsg.tool_calls[0].name},
                         {"arguments", g.assistMsg.tool_calls[0].arguments}}}}}}
                });
                sess->llmMessages.push_back({
                    {"role",         "tool"                },
                    {"tool_call_id", g.toolMsg.tool_call_id},
                    {"name",         g.toolMsg.tool_name   },
                    {"content",      g.toolMsg.content     }
                });
            }
        }
        p1Injected.store(true);
    });
    while (!p1Injected.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // CLI 全量 Sync
    io->sendToPeer(agent::WireHello{sessionId, "", 0, ""});
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    double busyCpu1 = cpuEnd(busyWin1);

    auto           mem1 = sampleMemoryMedian(0);
    ResourceResult res1;
    res1.mode          = "cli";
    res1.side          = "self";
    res1.point         = "ctx100k";
    res1.rssMB         = mem1.rssMB;
    res1.privateMB     = mem1.privateMB;
    res1.cpuIdlePct    = -1.0;
    res1.cpuBusyPct    = busyCpu1;
    res1.tokens        = counts.actualTokens100;
    res1.pluginsAgent  = res0.pluginsAgent;
    res1.pluginsClient = res0.pluginsClient;
    res1.note          = "injected fixed groups";

    {
        std::promise<void> p;
        asio::post(*agent->ioCtx, [&]() {
            if (auto sess = agent->agentContext->getSession(sessionId)) {
                res1.viewCount = sess->viewMessages.size();
                res1.viewBytes = estimateViewMessagesBytes(sess->viewMessages);
                res1.llmCount  = sess->llmMessages.size();
                res1.llmBytes  = estimateLlmMessagesBytes(sess->llmMessages);
            }
            p.set_value();
        });
        p.get_future().wait();
    }
    reporter.addResource(res1);
    printResourceResult(res1);

    // ---------------- P2: ~200K 上下文 ----------------
    auto              busyWin2 = cpuBegin(0);
    std::atomic<bool> p2Injected{false};
    asio::post(*agent->ioCtx, [&]() {
        if (auto sess = agent->agentContext->getSession(sessionId)) {
            for (size_t i = counts.n100; i < counts.n200; ++i) {
                auto g = makeFixedGroup(i + 1);
                sess->appendViewMessage(g.viewUser);
                sess->appendViewMessage(g.viewTool);
                sess->appendViewMessage(g.viewAssist);

                sess->llmMessages.push_back({
                    {"role",    "user"           },
                    {"content", g.userMsg.content}
                });
                sess->llmMessages.push_back({
                    {"role",       "assistant"                                 },
                    {"content",    nullptr                                     },
                    {"tool_calls",
                     {{{"id", g.assistMsg.tool_calls[0].id},
                       {"type", "function"},
                       {"function",
                        {{"name", g.assistMsg.tool_calls[0].name},
                         {"arguments", g.assistMsg.tool_calls[0].arguments}}}}}}
                });
                sess->llmMessages.push_back({
                    {"role",         "tool"                },
                    {"tool_call_id", g.toolMsg.tool_call_id},
                    {"name",         g.toolMsg.tool_name   },
                    {"content",      g.toolMsg.content     }
                });
            }
        }
        p2Injected.store(true);
    });
    while (!p2Injected.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // CLI 全量 Sync
    io->sendToPeer(agent::WireHello{sessionId, "", 0, ""});
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    double busyCpu2 = cpuEnd(busyWin2);

    auto           mem2 = sampleMemoryMedian(0);
    ResourceResult res2;
    res2.mode          = "cli";
    res2.side          = "self";
    res2.point         = "ctx200k";
    res2.rssMB         = mem2.rssMB;
    res2.privateMB     = mem2.privateMB;
    res2.cpuIdlePct    = -1.0;
    res2.cpuBusyPct    = busyCpu2;
    res2.tokens        = counts.actualTokens200;
    res2.pluginsAgent  = res0.pluginsAgent;
    res2.pluginsClient = res0.pluginsClient;
    res2.note          = "injected fixed groups";

    {
        std::promise<void> p;
        asio::post(*agent->ioCtx, [&]() {
            if (auto sess = agent->agentContext->getSession(sessionId)) {
                res2.viewCount = sess->viewMessages.size();
                res2.viewBytes = estimateViewMessagesBytes(sess->viewMessages);
                res2.llmCount  = sess->llmMessages.size();
                res2.llmBytes  = estimateLlmMessagesBytes(sess->llmMessages);
            }
            p.set_value();
        });
        p.get_future().wait();
    }
    reporter.addResource(res2);
    printResourceResult(res2);

    // 优雅退出
    serverIO->stop();
    clientWork.reset();
    clientCtx.stop();
    if (clientThread.joinable()) {
        clientThread.join();
    }
    agentWork.reset();
    agent->ioCtx->stop();
    if (agentThread.joinable()) {
        agentThread.join();
    }
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
#endif
}

// ===========================================================================
// M2: 同一进程 TUI 资源测试
// ===========================================================================

void benchResourceTui() {
#ifndef AGENTXX_BUILD_CLIENT
    std::cout << "  [resource][tui] skipped: AGENTXX_BUILD_CLIENT not enabled" << std::endl;
    return;
#else
    std::cout << "\n=== Resource Benchmark: M2 In-Process TUI ===" << std::endl;

    auto        sim      = startResourceLlmSimServer();
    auto        tmpDir   = createBenchTempDir("bench_m2_tui");
    auto&       reporter = BenchReporter::instance();
    const auto& counts   = getCalibratedCounts();

    auto agentConfig                = std::make_shared<agent::AgentConfig>();
    agentConfig->dataDir            = (tmpDir / "data").string();
    agentConfig->workDir            = tmpDir.string();
    agentConfig->permissionMode     = agent::PermissionMode::Pass;
    agentConfig->enableSessionStore = false;
    agentConfig->enableSubagent     = false;
    agentConfig->enableWorktree     = false;

    agent::ModelConfig mc;
    mc.name                               = "bench-sim";
    mc.type                               = "openai";
    mc.baseUrl                            = fmt::format("http://127.0.0.1:{}/v1", sim.port);
    mc.apiKey                             = "EMPTY";
    mc.modelName                          = "bench-sim";
    mc.modelContenxtMaxToken              = 8 << 20;
    agentConfig->model                    = mc;
    agentConfig->availableModels[mc.name] = mc;
    agentConfig->currentModelName         = mc.name;

    auto pluginPlan      = prepare5Plugins();
    agentConfig->plugins = pluginPlan.agentConfigs;

    auto        agent     = std::make_shared<agent::CodeAgent>(agentConfig);
    auto        agentWork = asio::make_work_guard(*agent->ioCtx);
    std::thread agentThread([agent]() {
        agent->ioCtx->run();
    });

    asio::io_context clientCtx;
    auto             clientEx  = clientCtx.get_executor();
    std::string      sessionId = generateBenchSessionId();

    // 注意: TUIClientAgentIO 绝不调用 start()! 仅作端点和共享堆存储
    auto tui = std::make_shared<TUIClientAgentIO>(
        clientEx,
        sessionId,
        TUITheme::darkTheme(),
        agent::PermissionMode::Pass
    );

    auto clientPlugins = agentConfig->plugins;
    auto pluginMgr     = std::make_shared<agentxx::plugin::ClientPluginManager>(clientEx);
    pluginMgr->setUiAdapter(std::make_shared<agentxx::client::TuiPluginAdapter>(tui));
    pluginMgr->setSessionId(sessionId);
    tui->setPluginManager(pluginMgr);
    tui->setEventSink(pluginMgr);

    std::atomic<bool> pluginsLoaded{false};
    asio::co_spawn(
        clientEx,
        [&]() -> asio::awaitable<void> {
            co_await pluginMgr->loadConfiguredClientPlugins(clientPlugins);
            pluginsLoaded.store(true);
            co_return;
        },
        asio::detached
    );
    while (!pluginsLoaded.load()) {
        clientCtx.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    auto agentEx = agent->ioCtx->get_executor();
    auto [clientTransport, serverTransport]
        = agent::ChannelAgentIOTransport::makePair(clientEx, agentEx);
    tui->setTransport(std::shared_ptr<agent::AgentIOTransportBase>(std::move(clientTransport)));

    agent::SessionServerAgentIO::Config scCfg;
    scCfg.sessionId            = sessionId;
    scCfg.initialSyncTailCount = 100; // TUI: 尾窗 100
    auto serverIO = std::make_shared<agent::SessionServerAgentIO>(agentEx, agent, scCfg);
    serverIO->setTransport(std::shared_ptr<agent::AgentIOTransportBase>(std::move(serverTransport))
    );

    std::atomic<bool> serverReady{false};
    asio::co_spawn(
        *agent->ioCtx,
        [serverIO]() -> asio::awaitable<void> {
            co_await serverIO->runTransportLoop();
        },
        asio::detached
    );

    asio::co_spawn(
        *agent->ioCtx,
        [&]() -> asio::awaitable<void> {
            co_await agent->init();
            tui->requestAppendComponentInfo(sessionId);
            tui->onServerReady();
            serverReady.store(true);
            co_await serverIO->run();
        },
        asio::detached
    );

    asio::co_spawn(
        clientEx,
        [tui]() -> asio::awaitable<void> {
            co_await tui->runTransportLoop();
        },
        asio::detached
    );

    auto        clientWork = asio::make_work_guard(clientCtx);
    std::thread clientThread([&clientCtx]() {
        clientCtx.run();
    });

    while (!serverReady.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    tui->sendToPeer(agent::WireHello{sessionId, "", 0, ""});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 首轮预热
    tui->sendToPeer(agent::WireUserInput{sessionId, "hello"});
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        bool idle = false;
        asio::post(*agent->ioCtx, [&]() {
            if (auto sess = agent->agentContext->getSession(sessionId)) {
                idle = (sess->activity == agent::SessionActivity::Idle);
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (idle && i >= 2) {
            break;
        }
    }

    // ---------------- P0: Startup ----------------
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto idleCpuWin = cpuBegin(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    double idleCpu = cpuEnd(idleCpuWin);
    auto   mem0    = sampleMemoryMedian(0);

    ResourceResult res0;
    res0.mode          = "tui";
    res0.side          = "self";
    res0.point         = "startup";
    res0.rssMB         = mem0.rssMB;
    res0.privateMB     = mem0.privateMB;
    res0.cpuIdlePct    = idleCpu;
    res0.cpuBusyPct    = 0.0;
    res0.pluginsAgent  = agent->agentContext->pluginManager->list().size();
    res0.pluginsClient = pluginMgr->list().size();
    res0.note          = "headless-tui, Channel, tail=100";

    {
        std::promise<void> p;
        asio::post(*agent->ioCtx, [&]() {
            if (auto sess = agent->agentContext->getSession(sessionId)) {
                res0.viewCount = sess->viewMessages.size();
                res0.viewBytes = estimateViewMessagesBytes(sess->viewMessages);
                res0.llmCount  = sess->llmMessages.size();
                res0.llmBytes  = estimateLlmMessagesBytes(sess->llmMessages);
                res0.tokens    = 150;
            }
            p.set_value();
        });
        p.get_future().wait();
    }
    reporter.addResource(res0);
    printResourceResult(res0);

    // ---------------- P1: ~100K 上下文 ----------------
    auto              busyWin1 = cpuBegin(0);
    std::atomic<bool> p1Injected{false};
    asio::post(*agent->ioCtx, [&]() {
        if (auto sess = agent->agentContext->getSession(sessionId)) {
            for (size_t i = 0; i < counts.n100; ++i) {
                auto g = makeFixedGroup(i + 1);
                sess->appendViewMessage(g.viewUser);
                sess->appendViewMessage(g.viewTool);
                sess->appendViewMessage(g.viewAssist);

                sess->llmMessages.push_back({
                    {"role",    "user"           },
                    {"content", g.userMsg.content}
                });
                sess->llmMessages.push_back({
                    {"role",       "assistant"                                 },
                    {"content",    nullptr                                     },
                    {"tool_calls",
                     {{{"id", g.assistMsg.tool_calls[0].id},
                       {"type", "function"},
                       {"function",
                        {{"name", g.assistMsg.tool_calls[0].name},
                         {"arguments", g.assistMsg.tool_calls[0].arguments}}}}}}
                });
                sess->llmMessages.push_back({
                    {"role",         "tool"                },
                    {"tool_call_id", g.toolMsg.tool_call_id},
                    {"name",         g.toolMsg.tool_name   },
                    {"content",      g.toolMsg.content     }
                });
            }
        }
        p1Injected.store(true);
    });
    while (!p1Injected.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // TUI 触发 Sync (尾窗 100) + 分页拉取更早历史
    tui->sendToPeer(agent::WireHello{sessionId, "", 0, ""});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    tui->sendToPeer(agent::WireGetViewMessages{sessionId, 100, 100});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    double busyCpu1 = cpuEnd(busyWin1);

    auto           mem1 = sampleMemoryMedian(0);
    ResourceResult res1;
    res1.mode          = "tui";
    res1.side          = "self";
    res1.point         = "ctx100k";
    res1.rssMB         = mem1.rssMB;
    res1.privateMB     = mem1.privateMB;
    res1.cpuIdlePct    = -1.0;
    res1.cpuBusyPct    = busyCpu1;
    res1.tokens        = counts.actualTokens100;
    res1.pluginsAgent  = res0.pluginsAgent;
    res1.pluginsClient = res0.pluginsClient;
    for (int w = 0; w < 20; ++w) {
        if (!tui->sharedState().readSnapshot()->messages.empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    auto tuiSnap1 = tui->sharedState().readSnapshot();
    res1.note     = fmt::format("tail=100 + pagePull, tuiMsgCount={}", tuiSnap1->messages.size());

    {
        std::promise<void> p;
        asio::post(*agent->ioCtx, [&]() {
            if (auto sess = agent->agentContext->getSession(sessionId)) {
                res1.viewCount = sess->viewMessages.size();
                res1.viewBytes = estimateViewMessagesBytes(sess->viewMessages);
                res1.llmCount  = sess->llmMessages.size();
                res1.llmBytes  = estimateLlmMessagesBytes(sess->llmMessages);
            }
            p.set_value();
        });
        p.get_future().wait();
    }
    reporter.addResource(res1);
    printResourceResult(res1);

    // ---------------- P2: ~200K 上下文 ----------------
    auto              busyWin2 = cpuBegin(0);
    std::atomic<bool> p2Injected{false};
    asio::post(*agent->ioCtx, [&]() {
        if (auto sess = agent->agentContext->getSession(sessionId)) {
            for (size_t i = counts.n100; i < counts.n200; ++i) {
                auto g = makeFixedGroup(i + 1);
                sess->appendViewMessage(g.viewUser);
                sess->appendViewMessage(g.viewTool);
                sess->appendViewMessage(g.viewAssist);

                sess->llmMessages.push_back({
                    {"role",    "user"           },
                    {"content", g.userMsg.content}
                });
                sess->llmMessages.push_back({
                    {"role",       "assistant"                                 },
                    {"content",    nullptr                                     },
                    {"tool_calls",
                     {{{"id", g.assistMsg.tool_calls[0].id},
                       {"type", "function"},
                       {"function",
                        {{"name", g.assistMsg.tool_calls[0].name},
                         {"arguments", g.assistMsg.tool_calls[0].arguments}}}}}}
                });
                sess->llmMessages.push_back({
                    {"role",         "tool"                },
                    {"tool_call_id", g.toolMsg.tool_call_id},
                    {"name",         g.toolMsg.tool_name   },
                    {"content",      g.toolMsg.content     }
                });
            }
        }
        p2Injected.store(true);
    });
    while (!p2Injected.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    tui->sendToPeer(agent::WireHello{sessionId, "", 0, ""});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    tui->sendToPeer(agent::WireGetViewMessages{sessionId, 100, 100});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    double busyCpu2 = cpuEnd(busyWin2);

    auto           mem2 = sampleMemoryMedian(0);
    ResourceResult res2;
    res2.mode          = "tui";
    res2.side          = "self";
    res2.point         = "ctx200k";
    res2.rssMB         = mem2.rssMB;
    res2.privateMB     = mem2.privateMB;
    res2.cpuIdlePct    = -1.0;
    res2.cpuBusyPct    = busyCpu2;
    res2.tokens        = counts.actualTokens200;
    res2.pluginsAgent  = res0.pluginsAgent;
    res2.pluginsClient = res0.pluginsClient;
    for (int w = 0; w < 20; ++w) {
        if (tui->sharedState().readSnapshot()->messages.size() > 100) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    auto tuiSnap2 = tui->sharedState().readSnapshot();
    res2.note     = fmt::format("tail=100 + pagePull, tuiMsgCount={}", tuiSnap2->messages.size());

    {
        std::promise<void> p;
        asio::post(*agent->ioCtx, [&]() {
            if (auto sess = agent->agentContext->getSession(sessionId)) {
                res2.viewCount = sess->viewMessages.size();
                res2.viewBytes = estimateViewMessagesBytes(sess->viewMessages);
                res2.llmCount  = sess->llmMessages.size();
                res2.llmBytes  = estimateLlmMessagesBytes(sess->llmMessages);
            }
            p.set_value();
        });
        p.get_future().wait();
    }
    reporter.addResource(res2);
    printResourceResult(res2);

    // 优雅退出
    serverIO->stop();
    clientWork.reset();
    clientCtx.stop();
    if (clientThread.joinable()) {
        clientThread.join();
    }
    agentWork.reset();
    agent->ioCtx->stop();
    if (agentThread.joinable()) {
        agentThread.join();
    }
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
#endif
}

// ===========================================================================
// M3: 拆分两进程 CLI+Server 资源测试
// ===========================================================================

void benchResourceSplitCli() {
    std::cout << "\n=== Resource Benchmark: M3 Split CLI + Server ===" << std::endl;

    std::string cliBin = findAgentxxCliPath();
    if (cliBin.empty()) {
        std::cout << "  [resource][split_cli] skipped: agentxx_cli binary not found" << std::endl;
        return;
    }

    auto        sim      = startResourceLlmSimServer();
    auto        tmpDir   = createBenchTempDir("bench_m3_split_cli");
    auto&       reporter = BenchReporter::instance();
    const auto& counts   = getCalibratedCounts();

    uint16_t    serverPort = findFreeTcpPort();
    std::string token      = "bench_split_token_333";

    // 写入 server 配置文件 server.yaml
    std::string serverYaml = (tmpDir / "server.yaml").string();
    {
        std::ofstream ofs(serverYaml);
        ofs << "data_dir: " << (tmpDir / "data_server").string() << "\n"
            << "work_dir: " << tmpDir.string() << "\n"
            << "permission:\n"
            << "  mode: pass\n"
            << "enable_session_store: false\n"
            << "enable_subagent: false\n"
            << "enable_worktree: false\n"
            << "models:\n"
            << "  - name: bench-sim\n"
            << "    type: openai\n"
            << "    base_url: http://127.0.0.1:" << sim.port << "/v1\n"
            << "    api_key: EMPTY\n"
            << "    model_name: bench-sim\n"
            << "    model_context_max_token: 8388608\n"
            << "use_model:\n"
            << "  default: bench-sim\n"
            << "plugins:\n";
        for (const auto& name : getBench5PluginNames()) {
            ofs << "  - " << resolveBenchPluginDir(name) << "\n";
        }
    }

    // 启动 server 子进程
    std::vector<std::string> serverArgs
        = {"server",
           "--config",
           serverYaml,
           "--host",
           "127.0.0.1",
           "--port",
           std::to_string(serverPort),
           "--token",
           token};
    auto serverProc = spawnChildProcess(cliBin, serverArgs, tmpDir.string());
    if (!serverProc.running) {
        std::cout << "  [resource][split_cli] failed to spawn server process" << std::endl;
        return;
    }

    // 轮询等待 server 端口就绪 (最长 15s)
    bool serverOk = false;
    for (int i = 0; i < 150; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        asio::io_context          testCtx;
        asio::ip::tcp::socket     sock(testCtx);
        boost::system::error_code ec;
        sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), serverPort), ec);
        if (!ec) {
            sock.close();
            serverOk = true;
            break;
        }
    }
    if (!serverOk) {
        std::cout << "  [resource][split_cli] server failed to bind port within 15s" << std::endl;
        stopChildProcess(serverProc);
        return;
    }

    // 写入 client 配置文件 client.yaml
    std::string clientYaml = (tmpDir / "client.yaml").string();
    {
        std::ofstream ofs(clientYaml);
        ofs << "data_dir: " << (tmpDir / "data_client").string() << "\n"
            << "work_dir: " << tmpDir.string() << "\n"
            << "permission:\n"
            << "  mode: pass\n"
            << "plugins:\n";
        for (const auto& name : getBench5PluginNames()) {
            ofs << "  - " << resolveBenchPluginDir(name) << "\n";
        }
    }

    // 启动 client 子进程 (agentxx_cli cli --config client.yaml --agent ws://... --token ...)
    std::string              wsUrl = fmt::format("ws://127.0.0.1:{}/agent", serverPort);
    std::vector<std::string> clientArgs
        = {"cli", "--config", clientYaml, "--agent", wsUrl, "--token", token};
    auto clientProc = spawnChildProcess(cliBin, clientArgs, tmpDir.string());
    if (!clientProc.running) {
        std::cout << "  [resource][split_cli] failed to spawn client process" << std::endl;
        stopChildProcess(serverProc);
        return;
    }

    // 等待 client 与 server 握手初始化完成
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // ---------------- P0: Startup ----------------
    auto idleWinServer = cpuBegin(serverProc.pid);
    auto idleWinClient = cpuBegin(clientProc.pid);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    double idleCpuServer = cpuEnd(idleWinServer);
    double idleCpuClient = cpuEnd(idleWinClient);
    auto   memServer0    = sampleMemoryMedian(serverProc.pid);
    auto   memClient0    = sampleMemoryMedian(clientProc.pid);

    ResourceResult resServer0;
    resServer0.mode         = "split_cli";
    resServer0.side         = "server";
    resServer0.point        = "startup";
    resServer0.rssMB        = memServer0.rssMB;
    resServer0.privateMB    = memServer0.privateMB;
    resServer0.cpuIdlePct   = idleCpuServer;
    resServer0.pluginsAgent = 5;
    resServer0.note         = "real server process, WebSocket";
    reporter.addResource(resServer0);
    printResourceResult(resServer0);

    ResourceResult resClient0;
    resClient0.mode          = "split_cli";
    resClient0.side          = "client";
    resClient0.point         = "startup";
    resClient0.rssMB         = memClient0.rssMB;
    resClient0.privateMB     = memClient0.privateMB;
    resClient0.cpuIdlePct    = idleCpuClient;
    resClient0.pluginsClient = 4;
    resClient0.note          = "real cli process, WebSocket";
    reporter.addResource(resClient0);
    printResourceResult(resClient0);

    // ---------------- P1: ~100K 真实驱动与采样 ----------------
    auto   busyWinServer1 = cpuBegin(serverProc.pid);
    auto   busyWinClient1 = cpuBegin(clientProc.pid);
    size_t startTurn1     = sim.turnCounter->load();
    for (size_t i = 0; i < counts.n100; ++i) {
        std::string userLine = fmt::format(
            "RES-BENCH user turn {:06d} | The quick brown fox jumps over the lazy dog. 请列出当前目录并读取 README 前 40 行。 #FIXED-9f3a\n",
            i + 1
        );
#if XX_IS_WIN_D
        DWORD written = 0;
        ::WriteFile(
            clientProc.hStdinWrite,
            userLine.data(),
            static_cast<DWORD>(userLine.size()),
            &written,
            nullptr
        );
#else
        if (clientProc.stdinWriteFd >= 0) {
            ssize_t w = write(clientProc.stdinWriteFd, userLine.data(), userLine.size());
            (void)w;
        }
#endif
        size_t targetTurn = startTurn1 + i + 1;
        for (int w = 0; w < 300; ++w) {
            if (sim.turnCounter->load() >= targetTurn) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    double busyCpuServer1 = cpuEnd(busyWinServer1);
    double busyCpuClient1 = cpuEnd(busyWinClient1);

    auto memServer1 = sampleMemoryMedian(serverProc.pid);
    auto memClient1 = sampleMemoryMedian(clientProc.pid);

    ResourceResult resServer1;
    resServer1.mode         = "split_cli";
    resServer1.side         = "server";
    resServer1.point        = "ctx100k";
    resServer1.rssMB        = memServer1.rssMB;
    resServer1.privateMB    = memServer1.privateMB;
    resServer1.cpuBusyPct   = busyCpuServer1;
    resServer1.tokens       = counts.actualTokens100;
    resServer1.pluginsAgent = 5;
    resServer1.llmCount     = counts.n100 * 3;
    resServer1.llmBytes     = counts.n100 * 3800;
    resServer1.note         = "real server process, real WS turns";
    reporter.addResource(resServer1);
    printResourceResult(resServer1);

    ResourceResult resClient1;
    resClient1.mode          = "split_cli";
    resClient1.side          = "client";
    resClient1.point         = "ctx100k";
    resClient1.rssMB         = memClient1.rssMB;
    resClient1.privateMB     = memClient1.privateMB;
    resClient1.cpuBusyPct    = busyCpuClient1;
    resClient1.tokens        = counts.actualTokens100;
    resClient1.pluginsClient = 4;
    resClient1.viewCount     = counts.n100 * 3;
    resClient1.viewBytes     = counts.n100 * 4000;
    resClient1.note          = "real cli process, WebSocket";
    reporter.addResource(resClient1);
    printResourceResult(resClient1);

    // ---------------- P2: ~200K 真实驱动与采样 ----------------
    auto   busyWinServer2 = cpuBegin(serverProc.pid);
    auto   busyWinClient2 = cpuBegin(clientProc.pid);
    size_t startTurn2     = sim.turnCounter->load();
    for (size_t i = counts.n100; i < counts.n200; ++i) {
        std::string userLine = fmt::format(
            "RES-BENCH user turn {:06d} | The quick brown fox jumps over the lazy dog. 请列出当前目录并读取 README 前 40 行。 #FIXED-9f3a\n",
            i + 1
        );
#if XX_IS_WIN_D
        DWORD written = 0;
        ::WriteFile(
            clientProc.hStdinWrite,
            userLine.data(),
            static_cast<DWORD>(userLine.size()),
            &written,
            nullptr
        );
#else
        if (clientProc.stdinWriteFd >= 0) {
            ssize_t w = write(clientProc.stdinWriteFd, userLine.data(), userLine.size());
            (void)w;
        }
#endif
        size_t targetTurn = startTurn2 + (i - counts.n100) + 1;
        for (int w = 0; w < 300; ++w) {
            if (sim.turnCounter->load() >= targetTurn) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    double busyCpuServer2 = cpuEnd(busyWinServer2);
    double busyCpuClient2 = cpuEnd(busyWinClient2);

    auto memServer2 = sampleMemoryMedian(serverProc.pid);
    auto memClient2 = sampleMemoryMedian(clientProc.pid);

    ResourceResult resServer2;
    resServer2.mode         = "split_cli";
    resServer2.side         = "server";
    resServer2.point        = "ctx200k";
    resServer2.rssMB        = memServer2.rssMB;
    resServer2.privateMB    = memServer2.privateMB;
    resServer2.cpuBusyPct   = busyCpuServer2;
    resServer2.tokens       = counts.actualTokens200;
    resServer2.pluginsAgent = 5;
    resServer2.llmCount     = counts.n200 * 3;
    resServer2.llmBytes     = counts.n200 * 3800;
    resServer2.note         = "real server process, real WS turns";
    reporter.addResource(resServer2);
    printResourceResult(resServer2);

    ResourceResult resClient2;
    resClient2.mode          = "split_cli";
    resClient2.side          = "client";
    resClient2.point         = "ctx200k";
    resClient2.rssMB         = memClient2.rssMB;
    resClient2.privateMB     = memClient2.privateMB;
    resClient2.cpuBusyPct    = busyCpuClient2;
    resClient2.tokens        = counts.actualTokens200;
    resClient2.pluginsClient = 4;
    resClient2.viewCount     = counts.n200 * 3;
    resClient2.viewBytes     = counts.n200 * 4000;
    resClient2.note          = "real cli process, WebSocket";
    reporter.addResource(resClient2);
    printResourceResult(resClient2);

    // 清理子进程与临时文件
    stopChildProcess(clientProc);
    stopChildProcess(serverProc);
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
}

// ===========================================================================
// M4: 拆分两进程 TUI+Server 资源测试
// ===========================================================================

void benchResourceSplitTui() {
    std::cout << "\n=== Resource Benchmark: M4 Split TUI + Server ===" << std::endl;

    std::string cliBin = findAgentxxCliPath();
    if (cliBin.empty()) {
        std::cout << "  [resource][split_tui] skipped: agentxx_cli binary not found" << std::endl;
        return;
    }

    auto        sim      = startResourceLlmSimServer();
    auto        tmpDir   = createBenchTempDir("bench_m4_split_tui");
    auto&       reporter = BenchReporter::instance();
    const auto& counts   = getCalibratedCounts();

    uint16_t    serverPort = findFreeTcpPort();
    std::string token      = "bench_split_token_444";

    std::string serverYaml = (tmpDir / "server.yaml").string();
    {
        std::ofstream ofs(serverYaml);
        ofs << "data_dir: " << (tmpDir / "data_server").string() << "\n"
            << "work_dir: " << tmpDir.string() << "\n"
            << "permission:\n"
            << "  mode: pass\n"
            << "enable_session_store: false\n"
            << "enable_subagent: false\n"
            << "enable_worktree: false\n"
            << "models:\n"
            << "  - name: bench-sim\n"
            << "    type: openai\n"
            << "    base_url: http://127.0.0.1:" << sim.port << "/v1\n"
            << "    api_key: EMPTY\n"
            << "    model_name: bench-sim\n"
            << "    model_context_max_token: 8388608\n"
            << "use_model:\n"
            << "  default: bench-sim\n"
            << "plugins:\n";
        for (const auto& name : getBench5PluginNames()) {
            ofs << "  - " << resolveBenchPluginDir(name) << "\n";
        }
    }

    std::vector<std::string> serverArgs
        = {"server",
           "--config",
           serverYaml,
           "--host",
           "127.0.0.1",
           "--port",
           std::to_string(serverPort),
           "--token",
           token};
    auto serverProc = spawnChildProcess(cliBin, serverArgs, tmpDir.string());
    if (!serverProc.running) {
        std::cout << "  [resource][split_tui] failed to spawn server process" << std::endl;
        return;
    }

    bool serverOk = false;
    for (int i = 0; i < 150; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        asio::io_context          testCtx;
        asio::ip::tcp::socket     sock(testCtx);
        boost::system::error_code ec;
        sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), serverPort), ec);
        if (!ec) {
            sock.close();
            serverOk = true;
            break;
        }
    }
    if (!serverOk) {
        std::cout << "  [resource][split_tui] server failed to bind port within 15s" << std::endl;
        stopChildProcess(serverProc);
        return;
    }

#ifdef AGENTXX_BUILD_CLIENT
    // Client 使用 bench 进程内的 headless TUI 端点经 WS 直连该 Server
    asio::io_context clientCtx;
    auto             clientEx  = clientCtx.get_executor();
    std::string      sessionId = generateBenchSessionId();

    auto tui = std::make_shared<TUIClientAgentIO>(
        clientEx,
        sessionId,
        TUITheme::darkTheme(),
        agent::PermissionMode::Pass
    );
    std::string wsUrl       = fmt::format("ws://127.0.0.1:{}/agent", serverPort);
    auto        wsTransport = std::make_shared<agent::WsAgentIOTransport>(
        clientEx,
        wsUrl,
        token,
        agent::WsAgentIOTransport::Config{}
    );
    tui->setTransport(wsTransport);

    std::atomic<bool> connected{false};
    asio::co_spawn(
        clientEx,
        [&]() -> asio::awaitable<void> {
            agent::WireHello hello{sessionId, token, 0, ""};
            bool             ok = co_await wsTransport->connect(hello);
            if (ok) {
                connected.store(true);
                co_await tui->runTransportLoop();
            }
        },
        asio::detached
    );

    auto        clientWork = asio::make_work_guard(clientCtx);
    std::thread clientThread([&clientCtx]() {
        clientCtx.run();
    });

    for (int i = 0; i < 100; ++i) {
        if (connected.load()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // P0: Startup
    auto idleWinServer = cpuBegin(serverProc.pid);
    auto idleWinClient = cpuBegin(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    double idleCpuServer = cpuEnd(idleWinServer);
    double idleCpuClient = cpuEnd(idleWinClient);
    auto   memServer0    = sampleMemoryMedian(serverProc.pid);
    auto   memClient0    = sampleMemoryMedian(0);

    ResourceResult resServer0;
    resServer0.mode         = "split_tui";
    resServer0.side         = "server";
    resServer0.point        = "startup";
    resServer0.rssMB        = memServer0.rssMB;
    resServer0.privateMB    = memServer0.privateMB;
    resServer0.cpuIdlePct   = idleCpuServer;
    resServer0.pluginsAgent = 5;
    resServer0.note         = "real server process, WebSocket";
    reporter.addResource(resServer0);
    printResourceResult(resServer0);

    ResourceResult resClient0;
    resClient0.mode          = "split_tui";
    resClient0.side          = "client";
    resClient0.point         = "startup";
    resClient0.rssMB         = memClient0.rssMB;
    resClient0.privateMB     = memClient0.privateMB;
    resClient0.cpuIdlePct    = idleCpuClient;
    resClient0.pluginsClient = 4;
    resClient0.note          = "headless-TUI over WS client, real server process";
    reporter.addResource(resClient0);
    printResourceResult(resClient0);

    // P1: ~100K 驱动与采样
    auto   busyWinServer1 = cpuBegin(serverProc.pid);
    auto   busyWinClient1 = cpuBegin(0);
    size_t startTurn1     = sim.turnCounter->load();
    for (size_t i = 0; i < counts.n100; ++i) {
        std::string userText = fmt::format(
            "RES-BENCH user turn {:06d} | The quick brown fox jumps over the lazy dog. 请列出当前目录并读取 README 前 40 行。 #FIXED-9f3a",
            i + 1
        );
        tui->sendToPeer(agent::WireUserInput{sessionId, userText});
        size_t targetTurn = startTurn1 + i + 1;
        for (int w = 0; w < 300; ++w) {
            if (sim.turnCounter->load() >= targetTurn) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    double busyCpuServer1 = cpuEnd(busyWinServer1);
    double busyCpuClient1 = cpuEnd(busyWinClient1);

    auto   memServer1 = sampleMemoryMedian(serverProc.pid);
    auto   memClient1 = sampleMemoryMedian(0);
    auto   tuiSnap1   = tui->sharedState().readSnapshot();
    size_t tuiMsgs1   = (tuiSnap1 != nullptr) ? tuiSnap1->messages.size() : 0;

    ResourceResult resServer1;
    resServer1.mode         = "split_tui";
    resServer1.side         = "server";
    resServer1.point        = "ctx100k";
    resServer1.rssMB        = memServer1.rssMB;
    resServer1.privateMB    = memServer1.privateMB;
    resServer1.cpuBusyPct   = busyCpuServer1;
    resServer1.tokens       = counts.actualTokens100;
    resServer1.pluginsAgent = 5;
    resServer1.llmCount     = counts.n100 * 3;
    resServer1.llmBytes     = counts.n100 * 3800;
    resServer1.note         = "real server process, real WS turns";
    reporter.addResource(resServer1);
    printResourceResult(resServer1);

    ResourceResult resClient1;
    resClient1.mode          = "split_tui";
    resClient1.side          = "client";
    resClient1.point         = "ctx100k";
    resClient1.rssMB         = memClient1.rssMB;
    resClient1.privateMB     = memClient1.privateMB;
    resClient1.cpuBusyPct    = busyCpuClient1;
    resClient1.tokens        = counts.actualTokens100;
    resClient1.pluginsClient = 4;
    resClient1.viewCount     = tuiMsgs1;
    resClient1.viewBytes     = tuiMsgs1 * 800;
    resClient1.note          = "headless-TUI over WS client";
    reporter.addResource(resClient1);
    printResourceResult(resClient1);

    // P2: ~200K 驱动与采样
    auto   busyWinServer2 = cpuBegin(serverProc.pid);
    auto   busyWinClient2 = cpuBegin(0);
    size_t startTurn2     = sim.turnCounter->load();
    for (size_t i = counts.n100; i < counts.n200; ++i) {
        std::string userText = fmt::format(
            "RES-BENCH user turn {:06d} | The quick brown fox jumps over the lazy dog. 请列出当前目录并读取 README 前 40 行。 #FIXED-9f3a",
            i + 1
        );
        tui->sendToPeer(agent::WireUserInput{sessionId, userText});
        size_t targetTurn = startTurn2 + (i - counts.n100) + 1;
        for (int w = 0; w < 300; ++w) {
            if (sim.turnCounter->load() >= targetTurn) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    double busyCpuServer2 = cpuEnd(busyWinServer2);
    double busyCpuClient2 = cpuEnd(busyWinClient2);

    auto   memServer2 = sampleMemoryMedian(serverProc.pid);
    auto   memClient2 = sampleMemoryMedian(0);
    auto   tuiSnap2   = tui->sharedState().readSnapshot();
    size_t tuiMsgs2   = (tuiSnap2 != nullptr) ? tuiSnap2->messages.size() : 0;

    ResourceResult resServer2;
    resServer2.mode         = "split_tui";
    resServer2.side         = "server";
    resServer2.point        = "ctx200k";
    resServer2.rssMB        = memServer2.rssMB;
    resServer2.privateMB    = memServer2.privateMB;
    resServer2.cpuBusyPct   = busyCpuServer2;
    resServer2.tokens       = counts.actualTokens200;
    resServer2.pluginsAgent = 5;
    resServer2.llmCount     = counts.n200 * 3;
    resServer2.llmBytes     = counts.n200 * 3800;
    resServer2.note         = "real server process, real WS turns";
    reporter.addResource(resServer2);
    printResourceResult(resServer2);

    ResourceResult resClient2;
    resClient2.mode          = "split_tui";
    resClient2.side          = "client";
    resClient2.point         = "ctx200k";
    resClient2.rssMB         = memClient2.rssMB;
    resClient2.privateMB     = memClient2.privateMB;
    resClient2.cpuBusyPct    = busyCpuClient2;
    resClient2.tokens        = counts.actualTokens200;
    resClient2.pluginsClient = 4;
    resClient2.viewCount     = tuiMsgs2;
    resClient2.viewBytes     = tuiMsgs2 * 800;
    resClient2.note          = "headless-TUI over WS client";
    reporter.addResource(resClient2);
    printResourceResult(resClient2);

    clientWork.reset();
    clientCtx.stop();
    if (clientThread.joinable()) {
        clientThread.join();
    }
#endif

    stopChildProcess(serverProc);
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
}

// ===========================================================================
// 对照组: libagentxx_shared 动态库资源测试
// ===========================================================================

void benchResourceFfi() {
    std::cout << "\n=== Resource Benchmark: FFI libagentxx_shared Control Group ===" << std::endl;

    std::string libPath = findSharedLibPath();
    if (libPath.empty()) {
        std::cout << "  [resource][ffi] skipped: libagentxx shared library not found" << std::endl;
        return;
    }

    std::cout << "  [resource][ffi] loading library: " << libPath << std::endl;

#if XX_IS_WIN_D
    HMODULE hLib = ::LoadLibraryA(libPath.c_str());
    if (!hLib) {
        std::cout << "  [resource][ffi] LoadLibrary failed: " << ::GetLastError() << std::endl;
        return;
    }
#define RESOLVE_SYM(name) (name = reinterpret_cast<decltype(name)>(::GetProcAddress(hLib, #name)))
#else
    void* hLib = dlopen(libPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!hLib) {
        std::cout << "  [resource][ffi] dlopen failed: " << dlerror() << std::endl;
        return;
    }
#define RESOLVE_SYM(name) (name = reinterpret_cast<decltype(name)>(dlsym(hLib, #name)))
#endif

    // 函数指针定义
    AgentxxFFIAgent* (*agentxx_ffi_create)(const AgentxxStringView*, const AgentxxStringView*, const AgentxxFFICallbacks*, AgentxxString*)
        = nullptr;
    int32_t (*agentxx_ffi_start)(AgentxxFFIAgent*, AgentxxString*) = nullptr;
    int32_t (*agentxx_ffi_stop)(AgentxxFFIAgent*)                  = nullptr;
    void (*agentxx_ffi_destroy)(AgentxxFFIAgent*)                  = nullptr;
    int32_t (*agentxx_ffi_send_input)(AgentxxFFIAgent*, const AgentxxStringView*, const AgentxxStringView*, AgentxxString*)
        = nullptr;
    int32_t (*agentxx_ffi_get_context_messages)(AgentxxFFIAgent*, AgentxxString*, AgentxxString*)
        = nullptr;
    void (*agentxx_ffi_string_free)(AgentxxString*) = nullptr;

    bool symsOk = RESOLVE_SYM(agentxx_ffi_create) && RESOLVE_SYM(agentxx_ffi_start)
                  && RESOLVE_SYM(agentxx_ffi_stop) && RESOLVE_SYM(agentxx_ffi_destroy)
                  && RESOLVE_SYM(agentxx_ffi_send_input)
                  && RESOLVE_SYM(agentxx_ffi_get_context_messages)
                  && RESOLVE_SYM(agentxx_ffi_string_free);

    if (!symsOk) {
        std::cout << "  [resource][ffi] failed to resolve required FFI symbols" << std::endl;
#if XX_IS_WIN_D
        ::FreeLibrary(hLib);
#else
        dlclose(hLib);
#endif
        return;
    }

    auto        sim      = startResourceLlmSimServer();
    auto        tmpDir   = createBenchTempDir("bench_ffi");
    auto&       reporter = BenchReporter::instance();
    const auto& counts   = getCalibratedCounts();

    // 配置 JSON
    neograph::json cfgJson;
    cfgJson["dataDir"]            = (tmpDir / "data").string();
    cfgJson["workDir"]            = tmpDir.string();
    cfgJson["permissionMode"]     = "pass";
    cfgJson["enableSessionStore"] = false;
    cfgJson["enableSubagent"]     = false;
    cfgJson["enableWorktree"]     = false;
    cfgJson["llmMaxRetry"]        = 0;

    neograph::json pluginsArr = neograph::json::array();
    for (const auto& name : getBench5PluginNames()) {
        neograph::json p;
        p["path"]    = resolveBenchPluginDir(name);
        p["enabled"] = true;
        p["sides"]   = "auto";
        pluginsArr.push_back(p);
    }
    cfgJson["plugins"] = pluginsArr;

    // 模型 JSON
    neograph::json modelJson;
    modelJson["name"]                 = "bench-sim";
    modelJson["type"]                 = "openai";
    modelJson["baseUrl"]              = fmt::format("http://127.0.0.1:{}/v1", sim.port);
    modelJson["apiKey"]               = "EMPTY";
    modelJson["modelName"]            = "bench-sim";
    modelJson["modelContextMaxToken"] = 8 << 20;

    std::string cfgStr   = cfgJson.dump();
    std::string modelStr = modelJson.dump();

    AgentxxStringView cfgSv{cfgStr.data(), static_cast<uint64_t>(cfgStr.size())};
    AgentxxStringView modelSv{modelStr.data(), static_cast<uint64_t>(modelStr.size())};

    struct FfiEventTracker {
        std::mutex              m;
        std::condition_variable cv;
        bool                    ready    = false;
        bool                    turnDone = false;
    } tracker;

    AgentxxFFICallbacks cb{};
    cb.user_data = &tracker;
    cb.on_event  = [](int32_t type, const AgentxxStringView*, void* ud) {
        auto* tr = static_cast<FfiEventTracker*>(ud);
        if (!tr) {
            return;
        }
        std::lock_guard<std::mutex> lock(tr->m);
        if (type == AGENTXX_FFI_EVT_READY) {
            tr->ready = true;
            tr->cv.notify_all();
        } else if (type == AGENTXX_FFI_EVT_TURN_END) {
            tr->turnDone = true;
            tr->cv.notify_all();
        }
    };

    AgentxxString    logOut{nullptr, 0};
    AgentxxFFIAgent* ffiAgent = agentxx_ffi_create(&cfgSv, &modelSv, &cb, &logOut);
    if (logOut.data) {
        agentxx_ffi_string_free(&logOut);
    }
    if (!ffiAgent) {
        std::cout << "  [resource][ffi] agentxx_ffi_create failed" << std::endl;
#if XX_IS_WIN_D
        ::FreeLibrary(hLib);
#else
        dlclose(hLib);
#endif
        return;
    }

    int32_t startCode = agentxx_ffi_start(ffiAgent, &logOut);
    if (logOut.data) {
        agentxx_ffi_string_free(&logOut);
    }
    if (startCode != 0) {
        std::cout << "  [resource][ffi] agentxx_ffi_start failed, code: " << startCode << std::endl;
        agentxx_ffi_destroy(ffiAgent);
#if XX_IS_WIN_D
        ::FreeLibrary(hLib);
#else
        dlclose(hLib);
#endif
        return;
    }

    // 等待就绪 EVT_READY
    {
        std::unique_lock<std::mutex> lock(tracker.m);
        tracker.cv.wait_for(lock, std::chrono::seconds(10), [&]() {
            return tracker.ready;
        });
    }

    // 预热一轮
    std::string       warmupInput = "hello";
    AgentxxStringView wInputSv{warmupInput.data(), static_cast<uint64_t>(warmupInput.size())};
    tracker.turnDone = false;
    agentxx_ffi_send_input(ffiAgent, &wInputSv, nullptr, &logOut);
    if (logOut.data) {
        agentxx_ffi_string_free(&logOut);
    }
    {
        std::unique_lock<std::mutex> lock(tracker.m);
        tracker.cv.wait_for(lock, std::chrono::seconds(5), [&]() {
            return tracker.turnDone;
        });
    }

    // ---------------- P0: Startup (加载后稳态) ----------------
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto idleWin = cpuBegin(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    double idleCpu = cpuEnd(idleWin);
    auto   mem0    = sampleMemoryMedian(0);

    ResourceResult res0;
    res0.mode          = "ffi";
    res0.side          = "self";
    res0.point         = "startup";
    res0.rssMB         = mem0.rssMB;
    res0.privateMB     = mem0.privateMB;
    res0.cpuIdlePct    = idleCpu;
    res0.cpuBusyPct    = 0.0;
    res0.pluginsAgent  = 5;
    res0.pluginsClient = 4;
    res0.tokens        = 150;
    res0.note          = "FfiClientAgentIO over in-process Channel, dll loaded";

    AgentxxString ctxOut{nullptr, 0};
    if (agentxx_ffi_get_context_messages(ffiAgent, &ctxOut, &logOut) == 0 && ctxOut.data) {
        res0.llmBytes = ctxOut.size;
        agentxx_ffi_string_free(&ctxOut);
    }
    if (logOut.data) {
        agentxx_ffi_string_free(&logOut);
    }

    reporter.addResource(res0);
    printResourceResult(res0);

    // ---------------- P1: ~100K ----------------
    auto busyWin1 = cpuBegin(0);
    for (size_t i = 0; i < counts.n100; ++i) {
        tracker.turnDone     = false;
        std::string userText = fmt::format(
            "RES-BENCH user turn {:06d} | The quick brown fox jumps over the lazy dog. 请列出当前目录并读取 README 前 40 行。 #FIXED-9f3a",
            i + 1
        );
        AgentxxStringView inputSv{userText.data(), static_cast<uint64_t>(userText.size())};
        agentxx_ffi_send_input(ffiAgent, &inputSv, nullptr, &logOut);
        if (logOut.data) {
            agentxx_ffi_string_free(&logOut);
        }
        std::unique_lock<std::mutex> lock(tracker.m);
        tracker.cv.wait_for(lock, std::chrono::seconds(10), [&]() {
            return tracker.turnDone;
        });
    }
    double busyCpu1 = cpuEnd(busyWin1);

    auto           mem1 = sampleMemoryMedian(0);
    ResourceResult res1;
    res1.mode          = "ffi";
    res1.side          = "self";
    res1.point         = "ctx100k";
    res1.rssMB         = mem1.rssMB;
    res1.privateMB     = mem1.privateMB;
    res1.cpuIdlePct    = -1.0;
    res1.cpuBusyPct    = busyCpu1;
    res1.tokens        = counts.actualTokens100;
    res1.pluginsAgent  = 5;
    res1.pluginsClient = 4;
    res1.note          = "real turns driven via agentxx_ffi_send_input";

    AgentxxString ctxOut1{nullptr, 0};
    if (agentxx_ffi_get_context_messages(ffiAgent, &ctxOut1, &logOut) == 0 && ctxOut1.data) {
        res1.llmBytes = ctxOut1.size;
        try {
            auto j = neograph::json::parse(
                std::string_view{ctxOut1.data, static_cast<size_t>(ctxOut1.size)}
            );
            if (j.is_array()) {
                res1.llmCount  = j.size();
                res1.viewCount = j.size();
                res1.viewBytes = res1.llmBytes;
            }
        } catch (...) {
        }
        agentxx_ffi_string_free(&ctxOut1);
    }
    if (logOut.data) {
        agentxx_ffi_string_free(&logOut);
    }

    reporter.addResource(res1);
    printResourceResult(res1);

    // ---------------- P2: ~200K ----------------
    auto busyWin2 = cpuBegin(0);
    for (size_t i = counts.n100; i < counts.n200; ++i) {
        tracker.turnDone     = false;
        std::string userText = fmt::format(
            "RES-BENCH user turn {:06d} | The quick brown fox jumps over the lazy dog. 请列出当前目录并读取 README 前 40 行。 #FIXED-9f3a",
            i + 1
        );
        AgentxxStringView inputSv{userText.data(), static_cast<uint64_t>(userText.size())};
        agentxx_ffi_send_input(ffiAgent, &inputSv, nullptr, &logOut);
        if (logOut.data) {
            agentxx_ffi_string_free(&logOut);
        }
        std::unique_lock<std::mutex> lock(tracker.m);
        tracker.cv.wait_for(lock, std::chrono::seconds(10), [&]() {
            return tracker.turnDone;
        });
    }
    double busyCpu2 = cpuEnd(busyWin2);

    auto           mem2 = sampleMemoryMedian(0);
    ResourceResult res2;
    res2.mode          = "ffi";
    res2.side          = "self";
    res2.point         = "ctx200k";
    res2.rssMB         = mem2.rssMB;
    res2.privateMB     = mem2.privateMB;
    res2.cpuIdlePct    = -1.0;
    res2.cpuBusyPct    = busyCpu2;
    res2.tokens        = counts.actualTokens200;
    res2.pluginsAgent  = 5;
    res2.pluginsClient = 4;
    res2.note          = "real turns driven via agentxx_ffi_send_input";

    AgentxxString ctxOut2{nullptr, 0};
    if (agentxx_ffi_get_context_messages(ffiAgent, &ctxOut2, &logOut) == 0 && ctxOut2.data) {
        res2.llmBytes = ctxOut2.size;
        try {
            auto j = neograph::json::parse(
                std::string_view{ctxOut2.data, static_cast<size_t>(ctxOut2.size)}
            );
            if (j.is_array()) {
                res2.llmCount  = j.size();
                res2.viewCount = j.size();
                res2.viewBytes = res2.llmBytes;
            }
        } catch (...) {
        }
        agentxx_ffi_string_free(&ctxOut2);
    }
    if (logOut.data) {
        agentxx_ffi_string_free(&logOut);
    }

    reporter.addResource(res2);
    printResourceResult(res2);

    // 销毁
    agentxx_ffi_stop(ffiAgent);
    agentxx_ffi_destroy(ffiAgent);

#if XX_IS_WIN_D
    ::FreeLibrary(hLib);
#else
    dlclose(hLib);
#endif
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
}

void benchResourceAll() {
    benchResourceCli();
    benchResourceTui();
    benchResourceSplitCli();
    benchResourceSplitTui();
    benchResourceFfi();
}

} // namespace bench
} // namespace agentxx
