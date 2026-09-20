#include "bench_resource.h"
#include "bench_mem_logical.h"
#include "bench_resource_util.h"
#include "bench_util.h"

#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/channel_io_transport.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/ffi_api.h"
#include "utilxx/http_server.h"
#include "utilxx_base/env.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"

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

// ---------------------------------------------------------------------------
// 模拟 LLM HTTP 服务 (ResourceLlmSimServer)
// ---------------------------------------------------------------------------

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

    MemPhaseTracker phaseTracker(0, "self");
    phaseTracker.mark("process_base", "仅 mock LLM 服务; agent/client 未构建");

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
    phaseTracker.mark("agent_constructed", "CodeAgent + io_context 线程已创建");

    asio::io_context clientCtx;
    // 必须在任何 poll() 之前持 work_guard: poll() 一旦因"无工作"返回, io_context
    // 会被标记为 stopped, 后续 run() 立即返回 —— 客户端接收循环不会启动,
    // 发送/接收全部失效 (实测: TUI 侧消息数恒为 0)
    auto        clientWork = asio::make_work_guard(clientCtx);
    auto        clientEx   = clientCtx.get_executor();
    auto        io         = std::make_shared<agentxx::client::StdIOClientAgentIO>();
    std::string sessionId  = generateBenchSessionId();
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

    // 启动 client 线程运行 clientCtx (work guard 已在创建时持有)
    std::thread clientThread([&clientCtx]() {
        clientCtx.run();
    });

    // 等待 serverReady
    while (!serverReady.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    phaseTracker.mark("agent_init_done", "agent init + 5 插件加载完成");

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
    phaseTracker.mark("warmup_turn_done", "首轮预热完成 (稳态)");

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
    res0.cpu           = idleCpuWin.result;
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
    // 逻辑内存 (agent 侧容器数据结构字节数; 会话须在 agent io 线程取)
    auto collectLogical = [&](ResourceResult& target) {
        std::promise<void> p;
        asio::post(*agent->ioCtx, [&]() {
            target.logical = collectAgentLogicalMem(
                agent->agentContext,
                AgentLogicalOptions{true, {sessionId}}
            );
            p.set_value();
        });
        p.get_future().wait();
    };
    collectLogical(res0);
    fillResourceMemDetail(res0, 0, true, false);
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
    phaseTracker.mark("ctx100k_ready", "已注入 ~100K token 历史并完成同步");

    auto           mem1 = sampleMemoryMedian(0);
    ResourceResult res1;
    res1.mode          = "cli";
    res1.side          = "self";
    res1.point         = "ctx100k";
    res1.rssMB         = mem1.rssMB;
    res1.privateMB     = mem1.privateMB;
    res1.cpuIdlePct    = -1.0;
    res1.cpuBusyPct    = busyCpu1;
    res1.cpu           = busyWin1.result;
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
    collectLogical(res1);
    fillResourceMemDetail(res1, 0, true, false);
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
    phaseTracker.mark("ctx200k_ready", "已注入 ~200K token 历史并完成同步");

    auto           mem2 = sampleMemoryMedian(0);
    ResourceResult res2;
    res2.mode          = "cli";
    res2.side          = "self";
    res2.point         = "ctx200k";
    res2.rssMB         = mem2.rssMB;
    res2.privateMB     = mem2.privateMB;
    res2.cpuIdlePct    = -1.0;
    res2.cpuBusyPct    = busyCpu2;
    res2.cpu           = busyWin2.result;
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
    collectLogical(res2);
    fillResourceMemDetail(res2, 0, true, true);
    reporter.addResource(res2);
    printResourceResult(res2);

    // 分阶段内存表挂到报告 (按 mode+side 展示一次)
    phaseTracker.printTable("cli/tui 分阶段内存");
    reporter.attachPhases(res0.mode, res0.side, phaseTracker.samples());

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

    MemPhaseTracker phaseTracker(0, "self");
    phaseTracker.mark("process_base", "仅 mock LLM 服务; agent/client 未构建");

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
    phaseTracker.mark("agent_constructed", "CodeAgent + io_context 线程已创建");

    asio::io_context clientCtx;
    auto             clientWork = asio::make_work_guard(clientCtx); // 先于 poll(), 见 M1 说明
    auto             clientEx   = clientCtx.get_executor();
    std::string      sessionId  = generateBenchSessionId();

    // 注意: TUIClientAgentIO 绝不调用 start()! 仅作端点和共享堆存储
    auto tui = std::make_shared<agentxx::client::TUIClientAgentIO>(
        clientEx,
        sessionId,
        agentxx::client::TUITheme::darkTheme()
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

    // 启动 client 线程运行 clientCtx (work guard 已在创建 clientCtx 时持有)
    std::thread clientThread([&clientCtx]() {
        clientCtx.run();
    });

    while (!serverReady.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    phaseTracker.mark("agent_init_done", "agent init + 5 插件加载完成");

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
    phaseTracker.mark("warmup_turn_done", "首轮预热完成 (稳态)");

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
    res0.cpu           = idleCpuWin.result;
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
    auto collectLogicalTui = [&](ResourceResult&                                         target,
                                 const std::shared_ptr<agentxx::client::TUIRenderState>& snap) {
        std::promise<void> p;
        asio::post(*agent->ioCtx, [&]() {
            target.logical = collectAgentLogicalMem(
                agent->agentContext,
                AgentLogicalOptions{true, {sessionId}}
            );
            p.set_value();
        });
        p.get_future().wait();
        if (snap) {
            auto tuiRows = collectTuiLogicalMem(*snap);
            target.logical.insert(target.logical.end(), tuiRows.begin(), tuiRows.end());
        }
    };
    collectLogicalTui(res0, tui->sharedState().readSnapshot());
    fillResourceMemDetail(res0, 0, true, false);
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
    // 分页必须用当前窗口起始下标作为 beforeIndex: 请求区间需与已加载窗口
    // 严格连续, 否则客户端按"不连续"丢弃该页
    tui->sendToPeer(agent::WireGetViewMessages{
        sessionId,
        tui->sharedState().readSnapshot()->historyWindowStart,
        100
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    double busyCpu1 = cpuEnd(busyWin1);
    phaseTracker.mark("ctx100k_ready", "已注入 ~100K token 历史并完成同步");

    auto           mem1 = sampleMemoryMedian(0);
    ResourceResult res1;
    res1.mode          = "tui";
    res1.side          = "self";
    res1.point         = "ctx100k";
    res1.rssMB         = mem1.rssMB;
    res1.privateMB     = mem1.privateMB;
    res1.cpuIdlePct    = -1.0;
    res1.cpuBusyPct    = busyCpu1;
    res1.cpu           = busyWin1.result;
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
    collectLogicalTui(res1, tuiSnap1);
    fillResourceMemDetail(res1, 0, true, false);
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
    tui->sendToPeer(agent::WireGetViewMessages{
        sessionId,
        tui->sharedState().readSnapshot()->historyWindowStart,
        100
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    double busyCpu2 = cpuEnd(busyWin2);
    phaseTracker.mark("ctx200k_ready", "已注入 ~200K token 历史并完成同步");

    auto           mem2 = sampleMemoryMedian(0);
    ResourceResult res2;
    res2.mode          = "tui";
    res2.side          = "self";
    res2.point         = "ctx200k";
    res2.rssMB         = mem2.rssMB;
    res2.privateMB     = mem2.privateMB;
    res2.cpuIdlePct    = -1.0;
    res2.cpuBusyPct    = busyCpu2;
    res2.cpu           = busyWin2.result;
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
    collectLogicalTui(res2, tuiSnap2);
    fillResourceMemDetail(res2, 0, true, true);
    reporter.addResource(res2);
    printResourceResult(res2);

    // 分阶段内存表挂到报告 (按 mode+side 展示一次)
    phaseTracker.printTable("cli/tui 分阶段内存");
    reporter.attachPhases(res0.mode, res0.side, phaseTracker.samples());

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
    // 注意: 列表段必须使用新结构 (model.list / model.use / plugin.list),
    // 旧键 models/plugins/use_model 会被配置加载器告警忽略, server 将因缺少模型而启动失败
    RealRunConfigOptions serverCfgOpts;
    serverCfgOpts.dataDir    = (tmpDir / "data_server").string();
    serverCfgOpts.workDir    = tmpDir.string();
    serverCfgOpts.llmBaseUrl = fmt::format("http://127.0.0.1:{}/v1", sim.port);
    std::string serverYaml   = (tmpDir / "server.yaml").string();
    {
        std::ofstream ofs(serverYaml);
        ofs << buildAgentYamlConfig(serverCfgOpts);
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
    SpawnOptions serverSpawn;
    serverSpawn.workingDir     = tmpDir.string();
    serverSpawn.outputRedirect = (tmpDir / "server_stdout.log").string();
    serverSpawn.env            = {
        {"TERM", "xterm-256color"}
    };
    auto serverProc = spawnChildProcess(cliBin, serverArgs, serverSpawn);
    if (!serverProc.running) {
        std::cout << "  [resource][split_cli] failed to spawn server process" << std::endl;
        return;
    }

    // 轮询等待 server 端口就绪 (最长 15s)
    bool serverOk = waitForTcpPort("127.0.0.1", serverPort, 20000);
    if (!serverOk) {
        std::cout << "  [resource][split_cli] server failed to bind port within 15s" << std::endl;
        stopChildProcess(serverProc);
        return;
    }

    // 写入 client 配置文件 client.yaml (数据目录与 server 分离, 模拟真实两进程部署)
    RealRunConfigOptions clientCfgOpts;
    clientCfgOpts.dataDir    = (tmpDir / "data_client").string();
    clientCfgOpts.workDir    = tmpDir.string();
    clientCfgOpts.llmBaseUrl = fmt::format("http://127.0.0.1:{}/v1", sim.port);
    std::string clientYaml   = (tmpDir / "client.yaml").string();
    {
        std::ofstream ofs(clientYaml);
        ofs << buildClientYamlConfig(clientCfgOpts);
    }

    // 启动 client 子进程 (agentxx_cli cli --config client.yaml --agent ws://... --token ...)
    std::string              wsUrl = fmt::format("ws://127.0.0.1:{}/agent", serverPort);
    std::vector<std::string> clientArgs
        = {"cli", "--config", clientYaml, "--agent", wsUrl, "--token", token};
    SpawnOptions clientSpawn;
    clientSpawn.workingDir     = tmpDir.string();
    clientSpawn.outputRedirect = (tmpDir / "client_stdout.log").string();
    clientSpawn.env            = {
        {"TERM", "xterm-256color"}
    };
    auto clientProc = spawnChildProcess(cliBin, clientArgs, clientSpawn);
    if (!clientProc.running) {
        std::cout << "  [resource][split_cli] failed to spawn client process" << std::endl;
        stopChildProcess(serverProc);
        return;
    }

    // 等待 client 与 server 握手初始化完成
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    MemPhaseTracker serverTracker(serverProc.pid, "server");
    MemPhaseTracker clientTracker(clientProc.pid, "client");
    serverTracker.mark("ready", "真实 server 进程: init + 插件加载完成, 端口就绪");
    clientTracker.mark("ready", "真实 client 进程: 已连接并完成首屏");

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
    resServer0.cpu          = idleWinServer.result;
    fillResourceMemDetail(resServer0, serverProc.pid, true, false);
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
    resClient0.cpu           = idleWinClient.result;
    fillResourceMemDetail(resClient0, clientProc.pid, true, false);
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
    serverTracker.mark("ctx100k", "真实 WS 轮次驱动至 ~100K token 上下文");
    clientTracker.mark("ctx100k", "客户端完成 ~100K 上下文的接收/渲染");

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
    resServer1.cpu          = busyWinServer1.result;
    fillResourceMemDetail(resServer1, serverProc.pid, true, false);
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
    resClient1.cpu           = busyWinClient1.result;
    fillResourceMemDetail(resClient1, clientProc.pid, true, false);
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
    serverTracker.mark("ctx200k", "真实 WS 轮次驱动至 ~200K token 上下文");
    clientTracker.mark("ctx200k", "客户端完成 ~200K 上下文的接收/渲染");

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
    resServer2.cpu          = busyWinServer2.result;
    fillResourceMemDetail(resServer2, serverProc.pid, true, false);
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
    resClient2.cpu           = busyWinClient2.result;
    fillResourceMemDetail(resClient2, clientProc.pid, true, false);
    reporter.addResource(resClient2);
    printResourceResult(resClient2);

    serverTracker.printTable("split_cli server 分阶段内存");
    clientTracker.printTable("split_cli client 分阶段内存");
    reporter.attachPhases("split_cli", "server", serverTracker.samples());
    reporter.attachPhases("split_cli", "client", clientTracker.samples());

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
    // 注: counts 仅在编译了 client 支持时用于驱动轮次

    std::string cliBin = findAgentxxCliPath();
    if (cliBin.empty()) {
        std::cout << "  [resource][split_tui] skipped: agentxx_cli binary not found" << std::endl;
        return;
    }

    auto        sim      = startResourceLlmSimServer();
    auto        tmpDir   = createBenchTempDir("bench_m4_split_tui");
    auto&       reporter = BenchReporter::instance();
    const auto& counts   = getCalibratedCounts();
    (void)counts;

    uint16_t    serverPort = findFreeTcpPort();
    std::string token      = "bench_split_token_444";

    // 列表段使用新结构 (model.list / model.use / plugin.list)
    RealRunConfigOptions serverCfgOpts;
    serverCfgOpts.dataDir    = (tmpDir / "data_server").string();
    serverCfgOpts.workDir    = tmpDir.string();
    serverCfgOpts.llmBaseUrl = fmt::format("http://127.0.0.1:{}/v1", sim.port);
    std::string serverYaml   = (tmpDir / "server.yaml").string();
    {
        std::ofstream ofs(serverYaml);
        ofs << buildAgentYamlConfig(serverCfgOpts);
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
    SpawnOptions serverSpawn;
    serverSpawn.workingDir     = tmpDir.string();
    serverSpawn.outputRedirect = (tmpDir / "server_stdout.log").string();
    serverSpawn.env            = {
        {"TERM", "xterm-256color"}
    };
    auto serverProc = spawnChildProcess(cliBin, serverArgs, serverSpawn);
    if (!serverProc.running) {
        std::cout << "  [resource][split_tui] failed to spawn server process" << std::endl;
        return;
    }

    bool serverOk = waitForTcpPort("127.0.0.1", serverPort, 20000);
    if (!serverOk) {
        std::cout << "  [resource][split_tui] server failed to bind port within 15s" << std::endl;
        stopChildProcess(serverProc);
        return;
    }

    // 阶段追踪器 (跨 #ifdef 使用: client 部分可能未编译, 但表需照常输出)
    MemPhaseTracker serverTracker(serverProc.pid, "server");
    MemPhaseTracker clientTracker(0, "client");
    serverTracker.mark("ready", "真实 server 进程: 端口就绪");
    clientTracker.mark("ready", "bench 进程内的 headless TUI client 已连接");

#ifdef AGENTXX_BUILD_CLIENT
    // Client 使用 bench 进程内的 headless TUI 端点经 WS 直连该 Server
    asio::io_context clientCtx;
    auto             clientEx  = clientCtx.get_executor();
    std::string      sessionId = generateBenchSessionId();

    auto tui = std::make_shared<agentxx::client::TUIClientAgentIO>(
        clientEx,
        sessionId,
        agentxx::client::TUITheme::darkTheme()
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
    resServer0.cpu          = idleWinServer.result;
    fillResourceMemDetail(resServer0, serverProc.pid, true, false);
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
    resClient0.cpu           = idleWinClient.result;
    fillResourceMemDetail(resClient0, 0, true, false);
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
    serverTracker.mark("ctx100k", "真实 WS 轮次驱动至 ~100K token 上下文");
    clientTracker.mark("ctx100k", "客户端完成 ~100K 上下文的接收/渲染");

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
    resServer1.cpu          = busyWinServer1.result;
    fillResourceMemDetail(resServer1, serverProc.pid, true, false);
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
    resClient1.cpu           = busyWinClient1.result;
    fillResourceMemDetail(resClient1, 0, true, false);
    if (auto snap = tui->sharedState().readSnapshot()) {
        resClient1.logical = collectTuiLogicalMem(*snap);
    }
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
    serverTracker.mark("ctx200k", "真实 WS 轮次驱动至 ~200K token 上下文");
    clientTracker.mark("ctx200k", "客户端完成 ~200K 上下文的接收/渲染");

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
    resServer2.cpu          = busyWinServer2.result;
    fillResourceMemDetail(resServer2, serverProc.pid, true, false);
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
    resClient2.cpu           = busyWinClient2.result;
    fillResourceMemDetail(resClient2, 0, true, true);
    if (auto snap = tui->sharedState().readSnapshot()) {
        resClient2.logical = collectTuiLogicalMem(*snap);
    }
    reporter.addResource(resClient2);
    printResourceResult(resClient2);

    clientWork.reset();
    clientCtx.stop();
    if (clientThread.joinable()) {
        clientThread.join();
    }
#endif

    serverTracker.printTable("split_tui server 分阶段内存");
    clientTracker.printTable("split_tui client 分阶段内存");
    reporter.attachPhases("split_tui", "server", serverTracker.samples());
    reporter.attachPhases("split_tui", "client", clientTracker.samples());

    stopChildProcess(serverProc);
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
}

// ===========================================================================
// 对照组: libagentxx_shared 动态库资源测试
// ===========================================================================

void benchResourceFfi() {
    std::cout << "\n=== Resource Benchmark: FFI libagentxx_shared Control Group ===" << std::endl;

    MemPhaseTracker phaseTracker(0, "self");

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
    res0.cpu           = idleWin.result;
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

    phaseTracker.mark("ready", "FFI agent 已就绪 (动态库已加载)");
    fillResourceMemDetail(res0, 0, true, false);
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
    phaseTracker.mark("ctx100k_ready", "已注入 ~100K token 历史并完成同步");

    auto           mem1 = sampleMemoryMedian(0);
    ResourceResult res1;
    res1.mode          = "ffi";
    res1.side          = "self";
    res1.point         = "ctx100k";
    res1.rssMB         = mem1.rssMB;
    res1.privateMB     = mem1.privateMB;
    res1.cpuIdlePct    = -1.0;
    res1.cpuBusyPct    = busyCpu1;
    res1.cpu           = busyWin1.result;
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

    fillResourceMemDetail(res1, 0, true, false);
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
    phaseTracker.mark("ctx200k_ready", "已注入 ~200K token 历史并完成同步");

    auto           mem2 = sampleMemoryMedian(0);
    ResourceResult res2;
    res2.mode          = "ffi";
    res2.side          = "self";
    res2.point         = "ctx200k";
    res2.rssMB         = mem2.rssMB;
    res2.privateMB     = mem2.privateMB;
    res2.cpuIdlePct    = -1.0;
    res2.cpuBusyPct    = busyCpu2;
    res2.cpu           = busyWin2.result;
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

    fillResourceMemDetail(res2, 0, true, true);
    reporter.addResource(res2);
    printResourceResult(res2);

    phaseTracker.printTable("ffi 分阶段内存");
    reporter.attachPhases("ffi", "self", phaseTracker.samples());

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

namespace {

/// 聚合运行包含的全部场景模块名 (与 benchmark_main.cpp 注册表一致)
const std::vector<std::string>& resourceSceneModules() {
    static const std::vector<std::string> kScenes = {
        "resource_cli",
        "resource_tui",
        "resource_split_cli",
        "resource_split_tui",
        "resource_ffi",
        "resource_real_tui",
        "resource_server_only",
        "resource_real_tui_child",
        "resource_plugin_attrib",
    };
    return kScenes;
}

/// 场景模块名 -> 直接调用入口 (供 --no-isolate / 子进程直跑时使用)
void runResourceSceneInProcess(const std::string& module) {
    if (module == "resource_cli") {
        benchResourceCli();
    } else if (module == "resource_tui") {
        benchResourceTui();
    } else if (module == "resource_split_cli") {
        benchResourceSplitCli();
    } else if (module == "resource_split_tui") {
        benchResourceSplitTui();
    } else if (module == "resource_ffi") {
        benchResourceFfi();
    } else if (module == "resource_real_tui") {
        benchResourceRealTui();
    } else if (module == "resource_server_only") {
        benchResourceServerOnly();
    } else if (module == "resource_real_tui_child") {
        benchResourceRealTuiChild();
    } else if (module == "resource_plugin_attrib") {
        benchResourcePluginAttrib();
    }
}

} // namespace

void benchResourceAll() {
    // 聚合运行默认把每个场景放到独立子进程执行, 原因:
    // - 内存基准要求各场景从相同的干净进程基线开始; 同进程连续运行时, 前一场景
    //   的堆 arena/页驻留/峰值 RSS 会污染后一场景的 startup 数据 (实测同进程
    //   连跑时 plugin_attrib 的基线由 12MB 变成 41MB, 峰值 RSS 继承自前置场景)
    // - 单个场景崩溃/超时不影响其余场景, 长跑更稳
    // 环境变量 AGENTXX_BENCH_NO_ISOLATE=1 可退回同进程顺序运行 (快速冒烟用)
    const bool noIsolate = utilxx_base::ApplicationEnv::instance().has("AGENTXX_BENCH_NO_ISOLATE");
    if (noIsolate) {
        std::cout << "\n[resource] 同进程顺序运行全部场景 (AGENTXX_BENCH_NO_ISOLATE=1)\n";
        for (const auto& scene : resourceSceneModules()) {
            runResourceSceneInProcess(scene);
        }
        return;
    }

    std::string exePath = currentExecutablePath();
    if (exePath.empty() || !std::filesystem::exists(exePath)) {
        std::cout << "[resource] 无法定位基准可执行文件, 退回同进程顺序运行\n";
        for (const auto& scene : resourceSceneModules()) {
            runResourceSceneInProcess(scene);
        }
        return;
    }

    auto        tmpDir   = createBenchTempDir("bench_resource_scenes");
    auto&       reporter = BenchReporter::instance();
    size_t      okScenes = 0;
    std::string failNotes;

    std::cout << "\n[resource] 每个场景在独立子进程中执行 (输出目录: " << tmpDir.string() << ")\n";
    for (const auto& scene : resourceSceneModules()) {
        auto            sceneDir = tmpDir / scene;
        std::error_code ec;
        std::filesystem::create_directories(sceneDir, ec);

        std::cout << "\n-------- [resource] 子进程场景: " << scene << " --------\n";
        std::cout.flush();

        SpawnOptions opts;
        opts.workingDir      = std::filesystem::current_path(ec).string();
        opts.createStdinPipe = false; // 子进程不需要 stdin
        opts.env             = {
            {"AGENTXX_BENCH_CHILD",      "1"              },
            {"AGENTXX_BENCH_OUTPUT_DIR", sceneDir.string()},
        };
        // 透传负载缩放系数 (若有)
        if (auto scale = utilxx_base::ApplicationEnv::instance().get("AGENTXX_BENCH_SCALE")) {
            opts.env.emplace_back("AGENTXX_BENCH_SCALE", *scale);
        }

        auto child = spawnChildProcess(exePath, {scene}, opts);
        if (!child.running) {
            std::cout << "  [resource] 启动子进程失败, 跳过场景 " << scene << "\n";
            failNotes += scene + "(spawn失败) ";
            continue;
        }
        bool exited = waitChildProcess(child, 30 * 60 * 1000); // 单场景上限 30 分钟
        if (!exited) {
            std::cout << "  [resource] 场景超时, 终止子进程: " << scene << "\n";
            stopChildProcess(child);
            failNotes += scene + "(超时) ";
        } else {
            stopChildProcess(child); // 回收句柄/描述符
        }

        // 合并该场景写出的报告 (取目录内最新的 bench_*.json)
        std::string                     latest;
        std::filesystem::file_time_type latestTime{};
        for (auto it = std::filesystem::directory_iterator(sceneDir, ec);
             !ec && it != std::filesystem::directory_iterator();
             it.increment(ec)) {
            if (!it->is_regular_file(ec)) {
                continue;
            }
            auto name = it->path().filename().string();
            if (name.rfind("bench_", 0) != 0 || it->path().extension() != ".json") {
                continue;
            }
            auto t = it->last_write_time(ec);
            if (latest.empty() || t > latestTime) {
                latest     = it->path().string();
                latestTime = t;
            }
        }
        if (latest.empty()) {
            std::cout << "  [resource] 场景未产出报告: " << scene << "\n";
            failNotes += scene + "(无报告) ";
            continue;
        }
        size_t merged = reporter.mergeResourceResultsFromFile(latest);
        if (merged == 0) {
            std::cout << "  [resource] 场景报告无资源数据: " << scene << "\n";
            failNotes += scene + "(无数据) ";
        } else {
            ++okScenes;
            std::cout << "  [resource] 已合并场景 " << scene << " 的 " << merged << " 个采样点\n";
        }
    }

    std::cout << fmt::format(
        "\n[resource] 聚合完成: {}/{} 个场景有数据{}\n",
        okScenes,
        resourceSceneModules().size(),
        failNotes.empty() ? std::string{} : ("; 异常场景: " + failNotes)
    );

    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
}

} // namespace bench
} // namespace agentxx
