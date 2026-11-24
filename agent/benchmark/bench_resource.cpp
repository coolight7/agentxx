#include "bench_resource.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/channel_io_transport.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/ffi_api.h"
#include "agentxx/plugin/plugin_manager.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/ip/tcp.hpp"
#include "asio/post.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "bench_mem_logical.h"
#include "bench_resource_util.h"
#include "bench_util.h"
#include "fmt/format.h"
#include "utilxx/http_server.h"
#include "utilxx_base/env.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
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
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

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

#if XX_IS_WIN_D
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
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
// 插件加载辅助: 构建 5 常用插件配置并校验
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

    // 环境变量旋钮 (用于按需加强/减弱负载; 详细说明见 docs/zh-cn/design/benchmark.md):
    // - AGENTXX_BENCH_STREAM_CHUNKS: mock LLM 每个响应的 SSE 事件数 (0 = 默认单事件),
    //   用来测量逐事件 SSE 解析路径的开销
    // - AGENTXX_BENCH_ROUNDS / AGENTXX_BENCH_ROUND_BYTES: P3 逐轮往返的轮数与每轮消息字节数
    auto envSize = [](std::string_view name, size_t def) -> size_t {
        if (auto v = utilxx_base::ApplicationEnv::instance().get(name)) {
            return static_cast<size_t>(std::strtoull(v->c_str(), nullptr, 10));
        }
        return def;
    };
    auto        sim      = startResourceLlmSimServer(envSize("AGENTXX_BENCH_STREAM_CHUNKS", 0));
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
    mc.modelContextMaxToken               = 8 << 20; // 8M 确保不触发压缩/截断
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

    // 加载 Channel 直连
    auto agentEx = agent->ioCtx->get_executor();
    auto [clientTransport, serverTransport]
        = agent::ChannelAgentIOTransport::makePair(clientEx, agentEx);
    io->setTransport(std::shared_ptr<agent::AgentIOTransportBase>(std::move(clientTransport)));

    agent::SessionServerAgentIO::Config scCfg;
    scCfg.sessionId            = sessionId;
    scCfg.initialSyncTailCount = 0; // CLI: 完整同步
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
                res0.viewCount = sess->viewWindow().size();
                res0.viewBytes = estimateViewMessagesBytes(sess->viewWindow());
                res0.llmCount  = sess->llmMessagesJson().size();
                res0.llmBytes  = estimateLlmMessagesBytes(sess->llmMessagesJson());
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

                sess->appendSettledLlmMessages(utilxx_base::Json::array({
                    utilxx_base::Json{{"role", "user"}, {"content", g.userMsg.content}},
                    utilxx_base::Json{
                                      {"role", "assistant"},
                                      {"content", nullptr},
                                      {"tool_calls",
                         {{{"id", g.assistMsg.tool_calls[0].id},
                           {"type", "function"},
                           {"function",
                            {{"name", g.assistMsg.tool_calls[0].name},
                             {"arguments", g.assistMsg.tool_calls[0].arguments}}}}}}
                    },
                    utilxx_base::Json{
                                      {"role", "tool"},
                                      {"tool_call_id", g.toolMsg.tool_call_id},
                                      {"name", g.toolMsg.tool_name},
                                      {"content", g.toolMsg.content}
                    },
                }));
            }
        }
        p1Injected.store(true);
    });
    while (!p1Injected.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // CLI 完整 Sync
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
                res1.viewCount = sess->viewWindow().size();
                res1.viewBytes = estimateViewMessagesBytes(sess->viewWindow());
                res1.llmCount  = sess->llmMessagesJson().size();
                res1.llmBytes  = estimateLlmMessagesBytes(sess->llmMessagesJson());
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

                sess->appendSettledLlmMessages(utilxx_base::Json::array({
                    utilxx_base::Json{{"role", "user"}, {"content", g.userMsg.content}},
                    utilxx_base::Json{
                                      {"role", "assistant"},
                                      {"content", nullptr},
                                      {"tool_calls",
                         {{{"id", g.assistMsg.tool_calls[0].id},
                           {"type", "function"},
                           {"function",
                            {{"name", g.assistMsg.tool_calls[0].name},
                             {"arguments", g.assistMsg.tool_calls[0].arguments}}}}}}
                    },
                    utilxx_base::Json{
                                      {"role", "tool"},
                                      {"tool_call_id", g.toolMsg.tool_call_id},
                                      {"name", g.toolMsg.tool_name},
                                      {"content", g.toolMsg.content}
                    },
                }));
            }
        }
        p2Injected.store(true);
    });
    while (!p2Injected.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // CLI 完整 Sync
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
                res2.viewCount = sess->viewWindow().size();
                res2.viewBytes = estimateViewMessagesBytes(sess->viewWindow());
                res2.llmCount  = sess->llmMessagesJson().size();
                res2.llmBytes  = estimateLlmMessagesBytes(sess->llmMessagesJson());
            }
            p.set_value();
        });
        p.get_future().wait();
    }
    collectLogical(res2);
    fillResourceMemDetail(res2, 0, true, true);
    reporter.addResource(res2);
    printResourceResult(res2);

    // ---------------- P3: 真实逐轮往返 (长上下文下的每轮开销) ----------------
    // 与 P1/P2 的"注入历史"不同: 这里每轮都走完整流程 —— 用户输入 → LLM 请求
    // (SSE) → 工具调用与结果回写 → 消息落库 → 事件回传, 与真实对话一致。
    // 用于观察长上下文下的每轮固定开销、稳态波动与单轮耗时; 轮数与消息字节数
    // 由 AGENTXX_BENCH_ROUNDS / AGENTXX_BENCH_ROUND_BYTES 控制 (默认 20 轮 × 8 KB)。
    {
        const size_t roundCount = envSize("AGENTXX_BENCH_ROUNDS", 20);
        const size_t roundBytes = envSize("AGENTXX_BENCH_ROUND_BYTES", 8192);

        // 会话是否已回到空闲 (与预热轮相同的轮询方式)
        auto sessionIdle = [&]() {
            bool idle = false;
            asio::post(*agent->ioCtx, [&]() {
                if (auto sess = agent->agentContext->getSession(sessionId)) {
                    idle = (sess->activity == agent::SessionActivity::Idle);
                }
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            return idle;
        };

        auto   roundsWin = cpuBegin(0);
        double sumMs     = 0.0;
        double maxMs     = 0.0;
        size_t served    = 0;
        // 每轮结束 (assistant 汇总) 时 mock 的 turnCounter 前进 1
        const size_t turnsBefore = sim.turnCounter->load();
        for (size_t r = 0; r < roundCount; ++r) {
            std::string msg = fmt::format("ROUND-{:04d}-", static_cast<unsigned>(r + 1));
            if (roundBytes > msg.size()) {
                msg.append(roundBytes - msg.size(), 'x');
            }
            const auto t0 = std::chrono::steady_clock::now();
            io->sendToPeer(agent::WireUserInput{sessionId, msg});
            const size_t wantTurns = turnsBefore + r + 1;
            for (int i = 0; i < 4000; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (sim.turnCounter->load() >= wantTurns && sessionIdle()) {
                    break;
                }
            }
            const double ms
                = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                      .count();
            sumMs += ms;
            maxMs  = std::max(maxMs, ms);
            ++served;
        }
        const double roundsCpu = cpuEnd(roundsWin);
        phaseTracker.mark("rounds_done", "真实逐轮往返完成");
        auto mem3 = sampleMemoryMedian(0);

        ResourceResult res3;
        res3.mode          = "cli";
        res3.side          = "self";
        res3.point         = fmt::format("rounds{}x{}", roundCount, roundBytes);
        res3.rssMB         = mem3.rssMB;
        res3.privateMB     = mem3.privateMB;
        res3.cpuIdlePct    = -1.0;
        res3.cpuBusyPct    = roundsCpu;
        res3.cpu           = roundsWin.result;
        res3.pluginsAgent  = res0.pluginsAgent;
        res3.pluginsClient = res0.pluginsClient;
        res3.note          = fmt::format(
            "real per-round turns (no injected history); {} rounds, avg {:.1f} ms/round, max {:.1f} ms",
            served,
            served != 0 ? sumMs / static_cast<double>(served) : 0.0,
            maxMs
        );
        {
            std::promise<void> p;
            asio::post(*agent->ioCtx, [&]() {
                if (auto sess = agent->agentContext->getSession(sessionId)) {
                    res3.viewCount = sess->viewWindow().size();
                    res3.viewBytes = estimateViewMessagesBytes(sess->viewWindow());
                    res3.llmCount  = sess->llmMessagesJson().size();
                    res3.llmBytes  = estimateLlmMessagesBytes(sess->llmMessagesJson());
                }
                p.set_value();
            });
            p.get_future().wait();
        }
        collectLogical(res3);
        fillResourceMemDetail(res3, 0, true, false);
        reporter.addResource(res3);
        printResourceResult(res3);
    }

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
    mc.modelContextMaxToken               = 8 << 20;
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
                res0.viewCount = sess->viewWindow().size();
                res0.viewBytes = estimateViewMessagesBytes(sess->viewWindow());
                res0.llmCount  = sess->llmMessagesJson().size();
                res0.llmBytes  = estimateLlmMessagesBytes(sess->llmMessagesJson());
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

                sess->appendSettledLlmMessages(utilxx_base::Json::array({
                    utilxx_base::Json{{"role", "user"}, {"content", g.userMsg.content}},
                    utilxx_base::Json{
                                      {"role", "assistant"},
                                      {"content", nullptr},
                                      {"tool_calls",
                         {{{"id", g.assistMsg.tool_calls[0].id},
                           {"type", "function"},
                           {"function",
                            {{"name", g.assistMsg.tool_calls[0].name},
                             {"arguments", g.assistMsg.tool_calls[0].arguments}}}}}}
                    },
                    utilxx_base::Json{
                                      {"role", "tool"},
                                      {"tool_call_id", g.toolMsg.tool_call_id},
                                      {"name", g.toolMsg.tool_name},
                                      {"content", g.toolMsg.content}
                    },
                }));
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
        tui->sharedState().readSnapshot()->history.windowStart(),
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
        if (!tui->sharedState().readSnapshot()->messages().empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    auto tuiSnap1 = tui->sharedState().readSnapshot();
    res1.note     = fmt::format("tail=100 + pagePull, tuiMsgCount={}", tuiSnap1->messages().size());

    {
        std::promise<void> p;
        asio::post(*agent->ioCtx, [&]() {
            if (auto sess = agent->agentContext->getSession(sessionId)) {
                res1.viewCount = sess->viewWindow().size();
                res1.viewBytes = estimateViewMessagesBytes(sess->viewWindow());
                res1.llmCount  = sess->llmMessagesJson().size();
                res1.llmBytes  = estimateLlmMessagesBytes(sess->llmMessagesJson());
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

                sess->appendSettledLlmMessages(utilxx_base::Json::array({
                    utilxx_base::Json{{"role", "user"}, {"content", g.userMsg.content}},
                    utilxx_base::Json{
                                      {"role", "assistant"},
                                      {"content", nullptr},
                                      {"tool_calls",
                         {{{"id", g.assistMsg.tool_calls[0].id},
                           {"type", "function"},
                           {"function",
                            {{"name", g.assistMsg.tool_calls[0].name},
                             {"arguments", g.assistMsg.tool_calls[0].arguments}}}}}}
                    },
                    utilxx_base::Json{
                                      {"role", "tool"},
                                      {"tool_call_id", g.toolMsg.tool_call_id},
                                      {"name", g.toolMsg.tool_name},
                                      {"content", g.toolMsg.content}
                    },
                }));
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
        tui->sharedState().readSnapshot()->history.windowStart(),
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
        if (tui->sharedState().readSnapshot()->messages().size() > 100) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    auto tuiSnap2 = tui->sharedState().readSnapshot();
    res2.note     = fmt::format("tail=100 + pagePull, tuiMsgCount={}", tuiSnap2->messages().size());

    {
        std::promise<void> p;
        asio::post(*agent->ioCtx, [&]() {
            if (auto sess = agent->agentContext->getSession(sessionId)) {
                res2.viewCount = sess->viewWindow().size();
                res2.viewBytes = estimateViewMessagesBytes(sess->viewWindow());
                res2.llmCount  = sess->llmMessagesJson().size();
                res2.llmBytes  = estimateLlmMessagesBytes(sess->llmMessagesJson());
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

    std::string cliBin = findAgentxxExePath();
    if (cliBin.empty()) {
        std::cout << "  [resource][split_cli] skipped: agentxx binary not found" << std::endl;
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

    // 启动 client 子进程 (agentxx cli --config client.yaml --agent ws://... --token ...)
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

    std::string cliBin = findAgentxxExePath();
    if (cliBin.empty()) {
        std::cout << "  [resource][split_tui] skipped: agentxx binary not found" << std::endl;
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
    size_t tuiMsgs1   = (tuiSnap1 != nullptr) ? tuiSnap1->messages().size() : 0;

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
    size_t tuiMsgs2   = (tuiSnap2 != nullptr) ? tuiSnap2->messages().size() : 0;

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

namespace {

/// dlsym / GetProcAddress 解析到的 FFI 导出函数 (与 ffi_symbols.map 的 15 个符号一一对应)
///
/// - 类型全部取自 `agentxx/ffi_api.h` 的声明 (`decltype(&::...)`), 不手写签名 ——
///   手写容易与头文件不一致 (曾把 stop/destroy 的 log 出参漏掉: 声明成一参后调用
///   依然编译通过, 但被调函数仍会写这个出参, 于是写到寄存器里的野指针上,
///   Windows 上表现为访问违例)
/// - 少一个符号就说明导出面被改过: `load()` 会打印缺哪些并让场景跳过
struct FfiBenchLib {
#if XX_IS_WIN_D
    HMODULE handle = nullptr;
#else
    void* handle = nullptr;
#endif
    decltype(&::agentxx_ffi_malloc)           fnMalloc       = nullptr;
    decltype(&::agentxx_ffi_free)             fnFree         = nullptr;
    // string_free 在头文件里有重载 (指针版 + AgentxxString& 便捷版), decltype 取不到
    // 唯一类型, 这里显式写出指针版并保留调用约定
    void(AGENTXX_FFI_CALL* fnStringFree)(AgentxxString*) = nullptr;
    decltype(&::agentxx_ffi_strdup_n)         fnStrdup       = nullptr;
    decltype(&::agentxx_ffi_api_version)      fnApiVersion   = nullptr;
    decltype(&::agentxx_ffi_library_version)  fnLibVersion   = nullptr;
    decltype(&::agentxx_ffi_strerror)         fnStrError     = nullptr;
    decltype(&::agentxx_ffi_get_capabilities) fnCapabilities = nullptr;
    decltype(&::agentxx_ffi_create)           fnCreate       = nullptr;
    decltype(&::agentxx_ffi_start)            fnStart        = nullptr;
    decltype(&::agentxx_ffi_stop)             fnStop         = nullptr;
    decltype(&::agentxx_ffi_destroy)          fnDestroy      = nullptr;
    decltype(&::agentxx_ffi_call)             fnCall         = nullptr;
    decltype(&::agentxx_ffi_call_cancel)      fnCallCancel   = nullptr;
    decltype(&::agentxx_ffi_drain_logs)       fnDrainLogs    = nullptr;

    FfiBenchLib() = default;

    ~FfiBenchLib() {
        close();
    }

    FfiBenchLib(const FfiBenchLib&)            = delete;
    FfiBenchLib& operator=(const FfiBenchLib&) = delete;

    /// 载入动态库并解析全部 15 个导出符号
    bool load(const std::string& path);
    void close();

    /// 释放库分配的字符串 (空串 / 空指针安全)
    void freeString(AgentxxString& s) const {
        if (s.data != nullptr && fnStringFree != nullptr) {
            fnStringFree(&s);
        }
        s.data = nullptr;
        s.size = 0;
    }
};

bool FfiBenchLib::load(const std::string& path) {
#if XX_IS_WIN_D
    handle = ::LoadLibraryA(path.c_str());
    if (handle == nullptr) {
        std::cout << "  [resource][ffi] LoadLibrary failed: " << ::GetLastError() << std::endl;
        return false;
    }
#define FFI_BENCH_RESOLVE(field, sym) \
    field = reinterpret_cast<decltype(field)>(::GetProcAddress(handle, sym))
#else
    handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        std::cout << "  [resource][ffi] dlopen failed: " << dlerror() << std::endl;
        return false;
    }
#define FFI_BENCH_RESOLVE(field, sym) field = reinterpret_cast<decltype(field)>(dlsym(handle, sym))
#endif
    FFI_BENCH_RESOLVE(fnMalloc, "agentxx_ffi_malloc");
    FFI_BENCH_RESOLVE(fnFree, "agentxx_ffi_free");
    FFI_BENCH_RESOLVE(fnStringFree, "agentxx_ffi_string_free");
    FFI_BENCH_RESOLVE(fnStrdup, "agentxx_ffi_strdup_n");
    FFI_BENCH_RESOLVE(fnApiVersion, "agentxx_ffi_api_version");
    FFI_BENCH_RESOLVE(fnLibVersion, "agentxx_ffi_library_version");
    FFI_BENCH_RESOLVE(fnStrError, "agentxx_ffi_strerror");
    FFI_BENCH_RESOLVE(fnCapabilities, "agentxx_ffi_get_capabilities");
    FFI_BENCH_RESOLVE(fnCreate, "agentxx_ffi_create");
    FFI_BENCH_RESOLVE(fnStart, "agentxx_ffi_start");
    FFI_BENCH_RESOLVE(fnStop, "agentxx_ffi_stop");
    FFI_BENCH_RESOLVE(fnDestroy, "agentxx_ffi_destroy");
    FFI_BENCH_RESOLVE(fnCall, "agentxx_ffi_call");
    FFI_BENCH_RESOLVE(fnCallCancel, "agentxx_ffi_call_cancel");
    FFI_BENCH_RESOLVE(fnDrainLogs, "agentxx_ffi_drain_logs");
#undef FFI_BENCH_RESOLVE

    std::vector<std::string> missing;
    const auto               need = [&missing](const void* fn, const char* name) {
        if (fn == nullptr) {
            missing.emplace_back(name);
        }
    };
    need(reinterpret_cast<const void*>(fnMalloc), "agentxx_ffi_malloc");
    need(reinterpret_cast<const void*>(fnFree), "agentxx_ffi_free");
    need(reinterpret_cast<const void*>(fnStringFree), "agentxx_ffi_string_free");
    need(reinterpret_cast<const void*>(fnStrdup), "agentxx_ffi_strdup_n");
    need(reinterpret_cast<const void*>(fnApiVersion), "agentxx_ffi_api_version");
    need(reinterpret_cast<const void*>(fnLibVersion), "agentxx_ffi_library_version");
    need(reinterpret_cast<const void*>(fnStrError), "agentxx_ffi_strerror");
    need(reinterpret_cast<const void*>(fnCapabilities), "agentxx_ffi_get_capabilities");
    need(reinterpret_cast<const void*>(fnCreate), "agentxx_ffi_create");
    need(reinterpret_cast<const void*>(fnStart), "agentxx_ffi_start");
    need(reinterpret_cast<const void*>(fnStop), "agentxx_ffi_stop");
    need(reinterpret_cast<const void*>(fnDestroy), "agentxx_ffi_destroy");
    need(reinterpret_cast<const void*>(fnCall), "agentxx_ffi_call");
    need(reinterpret_cast<const void*>(fnCallCancel), "agentxx_ffi_call_cancel");
    need(reinterpret_cast<const void*>(fnDrainLogs), "agentxx_ffi_drain_logs");
    if (!missing.empty()) {
        std::cout << "  [resource][ffi] missing exported symbol(s):";
        for (const auto& name : missing) {
            std::cout << ' ' << name;
        }
        std::cout << std::endl;
        close();
        return false;
    }
    return true;
}

void FfiBenchLib::close() {
    if (handle == nullptr) {
        return;
    }
#if XX_IS_WIN_D
    ::FreeLibrary(handle);
#else
    dlclose(handle);
#endif
    handle = nullptr;
}

/// 事件接收端: 回调里立刻释放 payload, 只留下基准需要的计数与回执
///
/// 事件 payload 归回调所有 (v4): 结构体与正文同在一块堆内存上, 用 agentxx_ffi_free
/// 释放整块; 这里也顺便验证了"回调 free"这条契约在长跑里是稳定的 (上千条事件)
struct FfiBenchEvents {
    /// 必须在 create 之前填好 (payload 由回调负责释放)
    void(AGENTXX_FFI_CALL* memFree)(const void*) = nullptr;

    std::mutex              mutex;
    std::condition_variable cv;

    bool   ready   = false;
    bool   stopped = false;
    size_t turnEnd = 0;

    /// 回执: reqId → (ok, error.name, result JSON 文本)
    struct Reply {
        bool        ok = false;
        std::string errorName;
        std::string errorMessage;
        std::string resultJson;
    };

    std::map<uint64_t, Reply> replies;

    /// 是否保留 `result` 段: 1k 次吞吐测量时关掉, 免得 JSON 转存成本盖过边界成本
    std::atomic<bool> keepResult{true};

    /// 增量事件计数 (事件吞吐子场景)
    std::atomic<size_t> deltaEvents{0};
    std::atomic<size_t> deltaBytes{0};

    /// 完整/尾窗同步: 每来一条 +1, 记下体积与条数
    size_t syncGeneration = 0;
    size_t syncBytes      = 0;
    size_t syncCount      = 0;

    AgentxxFFICallbacks callbacks() {
        AgentxxFFICallbacks cb{};
        cb.struct_size = sizeof(AgentxxFFICallbacks);
        cb.reserved    = 0;
        cb.on_event    = &FfiBenchEvents::onEvent;
        cb.user_data   = this;
        return cb;
    }

    /// 等 EVT_READY
    bool waitReady(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, timeout, [this] {
            return ready;
        });
    }

    /// 等 EVT_STOPPED (最后一个事件)
    bool waitStopped(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, timeout, [this] {
            return stopped;
        });
    }

    /// 等 `turnEnd` 超过 `baseline` (一轮结束)
    bool waitTurnEndBeyond(size_t baseline, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, timeout, [this, baseline] {
            return turnEnd > baseline;
        });
    }

    /// 等某条调用的回执
    bool waitReply(uint64_t reqId, std::chrono::milliseconds timeout, Reply& out) {
        std::unique_lock<std::mutex> lock(mutex);
        const bool                   hit = cv.wait_for(lock, timeout, [this, reqId] {
            return replies.find(reqId) != replies.end();
        });
        if (!hit) {
            return false;
        }
        out = replies[reqId];
        return true;
    }

    /// 等一条新的同步 (返回等待前后的代次差)
    bool waitSyncBeyond(size_t baseline, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, timeout, [this, baseline] {
            return syncGeneration > baseline;
        });
    }

    size_t turnEndCount() {
        std::lock_guard<std::mutex> lock(mutex);
        return turnEnd;
    }

    size_t syncGenerationCount() {
        std::lock_guard<std::mutex> lock(mutex);
        return syncGeneration;
    }

    void snapSync(size_t& bytes, size_t& count) {
        std::lock_guard<std::mutex> lock(mutex);
        bytes = syncBytes;
        count = syncCount;
    }

    void resetDeltaCounters() {
        deltaEvents.store(0, std::memory_order_relaxed);
        deltaBytes.store(0, std::memory_order_relaxed);
    }

    static void AGENTXX_FFI_CALL onEvent(
        int32_t        type,
        AgentxxString* payload,
        void*          ud
    );
};

/// C 回调: 按事件类型取计数/回执, 然后无条件释放 payload
void AGENTXX_FFI_CALL FfiBenchEvents::onEvent(
    int32_t        type,
    AgentxxString* payload,
    void*          ud
) {
    auto* ev = static_cast<FfiBenchEvents*>(ud);
    if (ev == nullptr) {
        return;
    }
    std::string_view text{};
    if (payload != nullptr && payload->data != nullptr) {
        text = std::string_view{payload->data, static_cast<size_t>(payload->size)};
    }

    if (type == AGENTXX_FFI_EVT_DELTA || type == AGENTXX_FFI_EVT_DELTA_TEXT_BATCH) {
        ev->deltaEvents.fetch_add(1, std::memory_order_relaxed);
        ev->deltaBytes.fetch_add(text.size(), std::memory_order_relaxed);
    } else if (type == AGENTXX_FFI_EVT_RESULT) {
        uint64_t     reqId = 0;
        bool         ok    = false;
        Reply        reply;
        try {
            auto j = neograph::json::parse(text);
            reqId  = j.value("reqId", uint64_t{0});
            ok     = j.value("ok", false);
            if (ok) {
                if (ev->keepResult.load(std::memory_order_relaxed)) {
                    if (j.contains("result")) {
                        reply.resultJson = j.at("result").dump();
                    }
                }
            } else if (j.contains("error")) {
                const auto err     = j.at("error");
                reply.errorName    = err.value("name", std::string{});
                reply.errorMessage = err.value("message", std::string{});
            }
        } catch (...) {
            // 解析失败也照常按 reqId=0 记一条 (不会命中任何等待)
        }
        reply.ok = ok;
        {
            std::lock_guard<std::mutex> lock(ev->mutex);
            ev->replies[reqId] = std::move(reply);
        }
        ev->cv.notify_all();
    } else if (type == AGENTXX_FFI_EVT_SYNC) {
        size_t msgs = 0;
        try {
            auto j = neograph::json::parse(text);
            if (j.contains("messages")) {
                const auto arr = j.at("messages");
                if (arr.is_array()) {
                    msgs = arr.size();
                }
            }
        } catch (...) {
        }
        {
            std::lock_guard<std::mutex> lock(ev->mutex);
            ++ev->syncGeneration;
            ev->syncBytes = text.size();
            ev->syncCount = msgs;
        }
        ev->cv.notify_all();
    } else if (type == AGENTXX_FFI_EVT_READY) {
        {
            std::lock_guard<std::mutex> lock(ev->mutex);
            ev->ready = true;
        }
        ev->cv.notify_all();
    } else if (type == AGENTXX_FFI_EVT_STOPPED) {
        {
            std::lock_guard<std::mutex> lock(ev->mutex);
            ev->stopped = true;
        }
        ev->cv.notify_all();
    } else if (type == AGENTXX_FFI_EVT_TURN_END) {
        {
            std::lock_guard<std::mutex> lock(ev->mutex);
            ++ev->turnEnd;
        }
        ev->cv.notify_all();
    }

    if (ev->memFree != nullptr) {
        ev->memFree(payload);
    }
}

/// 环境变量旋钮 (整数; 缺失或非法时取默认值)
std::size_t ffiEnvSize(std::string_view name, std::size_t def) {
    if (auto v = utilxx_base::ApplicationEnv::instance().get(name)) {
        return static_cast<std::size_t>(std::strtoull(v->c_str(), nullptr, 10));
    }
    return def;
}

/// 一次 FFI 句柄: 提交 + 等回执 + 停止/销毁 (全部经 15 个导出符号调用)
class FfiBenchHandle {
public:
    FfiBenchHandle(const FfiBenchLib& lib, FfiBenchEvents& events) :
        lib_(lib),
        events_(events) {}

    ~FfiBenchHandle() {
        destroy();
    }

    FfiBenchHandle(const FfiBenchHandle&)            = delete;
    FfiBenchHandle& operator=(const FfiBenchHandle&) = delete;

    /// create + start + 等 EVT_READY
    bool open(
        const std::string&        cfgJson,
        const std::string&        modelJson,
        std::chrono::milliseconds readyTimeout = std::chrono::seconds{20}
    ) {
        events_.memFree = lib_.fnFree;

        AgentxxStringView   cfgSv{cfgJson.data(), static_cast<uint64_t>(cfgJson.size())};
        AgentxxStringView   modelSv{modelJson.data(), static_cast<uint64_t>(modelJson.size())};
        AgentxxFFICallbacks cb = events_.callbacks();

        AgentxxString log{nullptr, 0};
        handle_ = lib_.fnCreate(&cfgSv, &modelSv, &cb, &log);
        noteLog(log);
        if (handle_ == nullptr) {
            return false;
        }
        AgentxxString startLog{nullptr, 0};
        const int32_t rc = lib_.fnStart(handle_, &startLog);
        noteLog(startLog);
        if (rc != AGENTXX_FFI_OK) {
            lastError_ = fmt::format("start failed: code {} ({})", rc, lastError_);
            return false;
        }
        if (!events_.waitReady(readyTimeout)) {
            lastError_ = "EVT_READY timeout";
            return false;
        }
        return true;
    }

    /// 拼请求 JSON: `{"op":..,"args":..[,"timeoutMs":..]}`
    static std::string requestJson(
        std::string_view      op,
        const neograph::json& args,
        int32_t               timeoutMs = 0
    ) {
        std::string out = "{\"op\":\"";
        out += op;
        out += "\",\"args\":";
        out += args.dump();
        if (timeoutMs > 0) {
            out += ",\"timeoutMs\":";
            out += std::to_string(timeoutMs);
        }
        out += "}";
        return out;
    }

    /// 只提交, 返回 reqId (0 = 同步失败)
    uint64_t submit(std::string_view op, const neograph::json& args, int32_t timeoutMs = 0) {
        if (handle_ == nullptr) {
            return 0;
        }
        const std::string reqJson = requestJson(op, args, timeoutMs);
        AgentxxStringView reqSv{reqJson.data(), static_cast<uint64_t>(reqJson.size())};
        uint64_t          reqId = 0;
        AgentxxString     log{nullptr, 0};
        const int32_t     rc = lib_.fnCall(handle_, &reqSv, &reqId, &log);
        noteLog(log);
        if (rc != AGENTXX_FFI_OK) {
            lastError_ = fmt::format("{}: submit failed: code {}", op, rc);
            return 0;
        }
        return reqId;
    }

    /// 提交并等回执 (超时也算失败)
    bool callOp(
        std::string_view            op,
        const neograph::json&       args,
        FfiBenchEvents::Reply&      reply,
        std::chrono::milliseconds   timeout = std::chrono::seconds{30},
        int32_t                     timeoutMs = 0
    ) {
        const uint64_t reqId = submit(op, args, timeoutMs);
        if (reqId == 0) {
            return false;
        }
        if (!events_.waitReply(reqId, timeout, reply)) {
            lastError_ = fmt::format("{}: EVT_RESULT timeout (reqId={})", op, reqId);
            return false;
        }
        return true;
    }

    /// 等一轮结束 (baseline = 调用前 `events_.turnEndCount()`)
    bool waitTurnEnd(size_t baseline, std::chrono::milliseconds timeout = std::chrono::seconds{30}) {
        return events_.waitTurnEndBeyond(baseline, timeout);
    }

    /// stop + 等 EVT_STOPPED
    bool close(std::chrono::milliseconds timeout = std::chrono::seconds{60}) {
        if (handle_ == nullptr) {
            return true;
        }
        AgentxxString log{nullptr, 0};
        const int32_t rc = lib_.fnStop(handle_, &log);
        noteLog(log);
        if (rc != AGENTXX_FFI_OK) {
            lastError_ = fmt::format("stop failed: code {}", rc);
            return false;
        }
        return events_.waitStopped(timeout);
    }

    /// destroy (只在 Created / Stopped 成功; 失败不释放句柄)
    void destroy() {
        if (handle_ == nullptr) {
            return;
        }
        AgentxxString log{nullptr, 0};
        const int32_t rc = lib_.fnDestroy(handle_, &log);
        noteLog(log);
        if (rc == AGENTXX_FFI_OK) {
            handle_ = nullptr;
        } else {
            XX_LOGW("[resource][ffi] destroy failed: code {}", rc);
        }
    }

    AgentxxFFIAgent* raw() const {
        return handle_;
    }

    FfiBenchEvents& events() {
        return events_;
    }

    const std::string& lastError() const {
        return lastError_;
    }

private:
    void noteLog(AgentxxString& log) {
        if (log.data == nullptr) {
            return;
        }
        lastError_.assign(log.data, static_cast<size_t>(log.size));
        lib_.freeString(log);
    }

    const FfiBenchLib& lib_;
    FfiBenchEvents&    events_;
    AgentxxFFIAgent*   handle_ = nullptr;
    std::string        lastError_;
};

} // namespace

void benchResourceFfi() {
    std::cout << "\n=== Resource Benchmark: FFI libagentxx_shared Control Group ===" << std::endl;

    MemPhaseTracker phaseTracker(0, "self");

    std::string libPath = findSharedLibPath();
    if (libPath.empty()) {
        std::cout << "  [resource][ffi] skipped: libagentxx shared library not found" << std::endl;
        return;
    }

    std::cout << "  [resource][ffi] loading library: " << libPath << std::endl;

    FfiBenchLib lib;
    if (!lib.load(libPath)) {
        return;
    }

    // 导出面自检: 15 个符号全部解析 + 版本号与头文件一致
    const int32_t     apiVer = lib.fnApiVersion();
    AgentxxStringView verSv{};
    std::string       libVer;
    if (lib.fnLibVersion(&verSv) == AGENTXX_FFI_OK && verSv.data != nullptr) {
        libVer.assign(verSv.data, static_cast<size_t>(verSv.size));
    }
    std::cout << "  [resource][ffi] apiVersion=" << apiVer << " library=" << libVer << std::endl;
    if (apiVer != AGENTXX_FFI_API_VERSION) {
        std::cout << "  [resource][ffi] skipped: library api version " << apiVer
                  << " != header " << AGENTXX_FFI_API_VERSION << std::endl;
        return;
    }
    {
        // 能力清单 (ops 全集 / features) 记进日志, 便于对照库版本
        AgentxxString caps{nullptr, 0};
        AgentxxString capsLog{nullptr, 0};
        if (lib.fnCapabilities(nullptr, &caps, &capsLog) == AGENTXX_FFI_OK && caps.data != nullptr) {
            std::cout << "  [resource][ffi] capabilities: "
                      << std::string_view{caps.data, static_cast<size_t>(caps.size)} << std::endl;
        }
        lib.freeString(caps);
        lib.freeString(capsLog);
    }

    auto        sim = startResourceLlmSimServer();
    // 事件吞吐子场景用的分块流式服务器: 一个回复切成多段 SSE, 逼近逐 token 流
    auto        simChunked = startResourceLlmSimServer(
        ffiEnvSize("AGENTXX_BENCH_FFI_STREAM_CHUNKS", 64)
    );
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

    FfiBenchEvents  events;
    FfiBenchHandle  ffi(lib, events);
    if (!ffi.open(cfgJson.dump(), modelJson.dump())) {
        std::cout << "  [resource][ffi] create/start failed: " << ffi.lastError() << std::endl;
        std::error_code ec;
        std::filesystem::remove_all(tmpDir, ec);
        return;
    }

    // 预热一轮
    {
        const size_t baseline = events.turnEndCount();
        FfiBenchEvents::Reply reply;
        if (!ffi.callOp("input.send", neograph::json{{"text", "hello"}}, reply)
            || !reply.ok) {
            std::cout << "  [resource][ffi] warmup turn rejected: " << reply.errorName
                      << " (" << reply.errorMessage << ")" << std::endl;
        }
        ffi.waitTurnEnd(baseline, std::chrono::seconds{10});
    }

    /// 一轮真实对话: 提交输入 (等受理回执) → 等轮次结束
    const auto runTurn = [&](const std::string& label) -> bool {
        const size_t baseline = events.turnEndCount();
        FfiBenchEvents::Reply reply;
        if (!ffi.callOp("input.send", neograph::json{{"text", label}}, reply)) {
            return false;
        }
        if (!reply.ok) {
            // 主会话已在跑时会被拒 (status=rejected): 等当前轮次结束再算完成
            return ffi.waitTurnEnd(baseline, std::chrono::seconds{10});
        }
        return ffi.waitTurnEnd(baseline, std::chrono::seconds{10});
    };

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

    {
        // 上下文体积: 走 call("context.get") 的回执 (v3 的同步查询已删除)
        FfiBenchEvents::Reply reply;
        if (ffi.callOp("context.get", neograph::json::object(), reply) && reply.ok) {
            res0.llmBytes = reply.resultJson.size();
        } else {
            std::cout << "  [resource][ffi] context.get failed: " << ffi.lastError() << std::endl;
        }
    }

    phaseTracker.mark("ready", "FFI agent 已就绪 (动态库已加载)");
    fillResourceMemDetail(res0, 0, true, false);
    reporter.addResource(res0);
    printResourceResult(res0);

    // ---------------- P1: ~100K ----------------
    auto busyWin1 = cpuBegin(0);
    for (size_t i = 0; i < counts.n100; ++i) {
        runTurn(fmt::format(
            "RES-BENCH user turn {:06d} | The quick brown fox jumps over the lazy dog. 请列出当前目录并读取 README 前 40 行。 #FIXED-9f3a",
            i + 1
        ));
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
    res1.note          = "real turns driven via call(\"input.send\")";

    {
        FfiBenchEvents::Reply reply;
        if (ffi.callOp("context.get", neograph::json::object(), reply) && reply.ok) {
            res1.llmBytes = reply.resultJson.size();
            try {
                auto j = neograph::json::parse(reply.resultJson);
                if (j.contains("messages") && j.at("messages").is_array()) {
                    const size_t n = j.at("messages").size();
                    res1.llmCount  = n;
                    res1.viewCount = n;
                    res1.viewBytes = res1.llmBytes;
                }
            } catch (...) {
            }
        }
    }

    fillResourceMemDetail(res1, 0, true, false);
    reporter.addResource(res1);
    printResourceResult(res1);

    // ---------------- P2: ~200K ----------------
    auto busyWin2 = cpuBegin(0);
    for (size_t i = counts.n100; i < counts.n200; ++i) {
        runTurn(fmt::format(
            "RES-BENCH user turn {:06d} | The quick brown fox jumps over the lazy dog. 请列出当前目录并读取 README 前 40 行。 #FIXED-9f3a",
            i + 1
        ));
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
    res2.note          = "real turns driven via call(\"input.send\")";

    {
        FfiBenchEvents::Reply reply;
        if (ffi.callOp("context.get", neograph::json::object(), reply) && reply.ok) {
            res2.llmBytes = reply.resultJson.size();
            try {
                auto j = neograph::json::parse(reply.resultJson);
                if (j.contains("messages") && j.at("messages").is_array()) {
                    const size_t n = j.at("messages").size();
                    res2.llmCount  = n;
                    res2.viewCount = n;
                    res2.viewBytes = res2.llmBytes;
                }
            } catch (...) {
            }
        }
    }

    fillResourceMemDetail(res2, 0, true, true);
    reporter.addResource(res2);
    printResourceResult(res2);

    // ---------------- P3: 调用吞吐 (C 边界 + 回执 + 事件回调) ----------------
    {
        constexpr size_t kCalls   = 1000;
        constexpr size_t kWarmup  = 20;
        const auto       args     = neograph::json::object();
        size_t           okCount  = 0;
        // 预热: 让首调 (懒创建/日志) 不落进测量
        for (size_t i = 0; i < kWarmup; ++i) {
            FfiBenchEvents::Reply reply;
            ffi.callOp("model.list", args, reply, std::chrono::seconds{10});
        }
        events.keepResult.store(false, std::memory_order_relaxed);
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < kCalls; ++i) {
            FfiBenchEvents::Reply reply;
            if (ffi.callOp("model.list", args, reply, std::chrono::seconds{10}) && reply.ok) {
                ++okCount;
            }
        }
        const auto   t1 = std::chrono::steady_clock::now();
        events.keepResult.store(true, std::memory_order_relaxed);

        const double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double perOpUs = totalMs * 1000.0 / static_cast<double>(kCalls);
        const double opsPerS = (totalMs > 0.0) ? (static_cast<double>(okCount) * 1000.0 / totalMs)
                                               : 0.0;
        const std::string note = fmt::format(
            "{} x model.list (submit+ack) in {:.1f} ms: {:.1f} us/op, {:.0f} op/s, ok={}",
            kCalls,
            totalMs,
            perOpUs,
            opsPerS,
            okCount
        );
        std::cout << "  [resource][ffi] call throughput: " << note << std::endl;

        auto memCall = sampleMemoryMedian(0);
        ResourceResult resCall;
        resCall.mode       = "ffi";
        resCall.side       = "self";
        resCall.point      = "call_throughput";
        resCall.rssMB      = memCall.rssMB;
        resCall.privateMB  = memCall.privateMB;
        resCall.cpuIdlePct = -1.0;
        resCall.note       = note;
        phaseTracker.mark("call_throughput", "1k 次 model.list 提交+回执完成");
        reporter.addResource(resCall);
        printResourceResult(resCall);
    }

    // ---------------- P4: 事件吞吐 (合批关 / 24ms) ----------------
    {
        // 加一个分块流式模型并切过去 (顺带覆盖 model.add / model.select 两个 op)
        neograph::json model2 = modelJson;
        model2["name"]        = "bench-sim-chunked";
        model2["baseUrl"]     = fmt::format("http://127.0.0.1:{}/v1", simChunked.port);

        FfiBenchEvents::Reply reply;
        const bool            added
            = ffi.callOp("model.add", model2, reply, std::chrono::seconds{30}) && reply.ok;
        const bool selected
            = added
              && ffi.callOp(
                     "model.select",
                     neograph::json{{"name", "bench-sim-chunked"}},
                     reply,
                     std::chrono::seconds{10}
                 )
              && reply.ok;
        if (!added || !selected) {
            std::cout << "  [resource][ffi] delta batch scenario skipped: " << ffi.lastError()
                      << std::endl;
        } else {
            // 一轮: 设合批窗口 → 提交输入 → 等轮次结束, 返回耗时与该轮的增量事件数
            struct DeltaRun {
                double ms     = 0.0;
                size_t events = 0;
                size_t bytes  = 0;
            };
            const auto runDeltaTurn = [&](int batchMs) -> DeltaRun {
                FfiBenchEvents::Reply setReply;
                ffi.callOp(
                    "delta.set_batch",
                    neograph::json{{"maxDelayMs", batchMs}},
                    setReply,
                    std::chrono::seconds{10}
                );
                events.resetDeltaCounters();
                const size_t baseline = events.turnEndCount();
                const auto   t0       = std::chrono::steady_clock::now();
                FfiBenchEvents::Reply inputReply;
                ffi.callOp(
                    "input.send",
                    neograph::json{{"text", "RES-BENCH delta batch probe"}},
                    inputReply,
                    std::chrono::seconds{10}
                );
                ffi.waitTurnEnd(baseline, std::chrono::seconds{30});
                const auto t1 = std::chrono::steady_clock::now();
                DeltaRun   run;
                run.ms     = std::chrono::duration<double, std::milli>(t1 - t0).count();
                run.events = events.deltaEvents.load(std::memory_order_relaxed);
                run.bytes  = events.deltaBytes.load(std::memory_order_relaxed);
                return run;
            };

            const DeltaRun off = runDeltaTurn(0);
            const DeltaRun on  = runDeltaTurn(24);

            const auto reportDelta = [&](const char* point, const DeltaRun& run, int batchMs) {
                const std::string note = fmt::format(
                    "delta batching {}: {} events, {} bytes, turn {:.1f} ms",
                    batchMs > 0 ? fmt::format("{}ms", batchMs) : std::string{"off"},
                    run.events,
                    run.bytes,
                    run.ms
                );
                std::cout << "  [resource][ffi] " << note << std::endl;
                auto           memD = sampleMemoryMedian(0);
                ResourceResult resD;
                resD.mode       = "ffi";
                resD.side       = "self";
                resD.point      = point;
                resD.rssMB      = memD.rssMB;
                resD.privateMB  = memD.privateMB;
                resD.cpuIdlePct = -1.0;
                resD.note       = note;
                reporter.addResource(resD);
                printResourceResult(resD);
            };
            phaseTracker.mark("delta_batch", "合批关 / 合批 24ms 各跑一轮");
            reportDelta("delta_batch_off", off, 0);
            reportDelta("delta_batch_24ms", on, 24);

            // 切回主模型, 后面的子场景用默认的对话链路
            ffi.callOp(
                "model.select",
                neograph::json{{"name", "bench-sim"}},
                reply,
                std::chrono::seconds{10}
            );
        }
    }

    // ---------------- P5: 首屏尾窗 (syncTailCount 0 vs 200) ----------------
    //
    // 尾窗只在"接入一个已有长历史的会话"时起作用: 先用一个临时句柄 (会话落盘打开)
    // 跑出长会话, 再用两个只有 syncTailCount 不同的句柄切到同一会话, 量同步体积与耗时
    {
        const auto tailDir = tmpDir / "tail";
        {
            // 会话库目录必须存在: sqlite 不会自己建父目录, 缺失时 store 建不起来,
            // session.list 会空 (尾窗子场景就拿不到会话 id)
            std::error_code ecDir;
            std::filesystem::create_directories(tailDir / "data", ecDir);
        }
        neograph::json tailCfg;
        tailCfg["dataDir"]            = (tailDir / "data").string();
        tailCfg["workDir"]            = tailDir.string();
        tailCfg["permissionMode"]     = "pass";
        tailCfg["enableSessionStore"] = true;
        tailCfg["enableSubagent"]     = false;
        tailCfg["enableWorktree"]     = false;
        tailCfg["llmMaxRetry"]        = 0;
        // 尾窗子场景不需要插件: 少几条加载路径, 建句柄更快
        tailCfg["syncTailCount"] = 200;

        FfiBenchEvents  tailEvents;
        FfiBenchHandle  builder(lib, tailEvents);
        bool            ok = builder.open(tailCfg.dump(), modelJson.dump());
        std::string     longSessionId;

        // 60 轮 ≈ 240 条展示消息 (每轮 user / assistant(tool_call) / tool / assistant):
        // 必须多于 syncTailCount 默认的 200, 否则尾窗就是全部历史, 两个点量不出差别
        constexpr size_t kTailTurns = 60;
        if (ok) {
            for (size_t i = 0; i < kTailTurns; ++i) {
                const size_t baseline = tailEvents.turnEndCount();
                FfiBenchEvents::Reply reply;
                builder.callOp(
                    "input.send",
                    neograph::json{{"text", fmt::format("RES-BENCH tail seed turn {:03d}", i + 1)}},
                    reply,
                    std::chrono::seconds{10}
                );
                if (!builder.waitTurnEnd(baseline, std::chrono::seconds{20})) {
                    std::cout << "  [resource][ffi] tail scenario: seed turn timeout" << std::endl;
                    ok = false;
                    break;
                }
            }
        }
        if (ok) {
            FfiBenchEvents::Reply reply;
            if (builder.callOp("session.list", neograph::json::object(), reply, std::chrono::seconds{10})
                && reply.ok) {
                try {
                    auto j = neograph::json::parse(reply.resultJson);
                    if (j.contains("sessions") && j.at("sessions").is_array()
                        && j.at("sessions").size() > 0) {
                        const auto first = j.at("sessions").at(0);
                        longSessionId    = first.value("sessionId", std::string{});
                    }
                } catch (...) {
                }
            }
            if (longSessionId.empty()) {
                std::cout << "  [resource][ffi] tail scenario: session id not found "
                          << "(session.list ok=" << reply.ok << ", reason=" << reply.errorName
                          << ", result="
                          << reply.resultJson.substr(0, std::min<size_t>(200, reply.resultJson.size()))
                          << ")" << std::endl;
                // 排障: 把句柄里积压的日志打出来 (会话库打不开时只能看到日志)
                AgentxxString logs{nullptr, 0};
                if (lib.fnDrainLogs(builder.raw(), &logs, nullptr) == AGENTXX_FFI_OK
                    && logs.data != nullptr) {
                    std::cout << "  [resource][ffi] builder logs: "
                              << std::string_view{logs.data, static_cast<size_t>(logs.size)}
                              << std::endl;
                }
                lib.freeString(logs);
                ok = false;
            }
        }

        if (ok) {
            // 在指定句柄上切到长会话, 量"提交 → 收到 EVT_SYNC"的耗时与体积
            struct SyncRun {
                double ms    = 0.0;
                size_t bytes = 0;
                size_t msgs  = 0;
            };
            const auto measureSwitch = [&](FfiBenchHandle& h, const char* label) -> SyncRun {
                auto&  ev       = h.events();
                size_t baseline = ev.syncGenerationCount();
                const auto t0   = std::chrono::steady_clock::now();
                const uint64_t reqId
                    = h.submit("session.switch", neograph::json{{"sessionId", longSessionId}});
                ev.waitSyncBeyond(baseline, std::chrono::seconds{30});
                const auto t1 = std::chrono::steady_clock::now();
                SyncRun    run;
                run.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                ev.snapSync(run.bytes, run.msgs);
                if (reqId != 0) {
                    FfiBenchEvents::Reply reply;
                    ev.waitReply(reqId, std::chrono::seconds{30}, reply);
                }
                std::cout << "  [resource][ffi] sync tail " << label << ": "
                          << fmt::format("{:.1f} ms, {} bytes, {} messages", run.ms, run.bytes, run.msgs)
                          << std::endl;
                return run;
            };

            // 两个新句柄各自"首次接入这个长会话", 只差 syncTailCount
            const auto openAndMeasure
                = [&](int64_t syncTailCount, const char* label) -> std::optional<SyncRun> {
                neograph::json cfg = tailCfg;
                cfg["syncTailCount"] = syncTailCount;
                FfiBenchEvents ev;
                FfiBenchHandle h(lib, ev);
                if (!h.open(cfg.dump(), modelJson.dump())) {
                    std::cout << "  [resource][ffi] sync tail " << label
                              << ": handle failed to start: " << h.lastError() << std::endl;
                    return std::nullopt;
                }
                SyncRun run = measureSwitch(h, label);
                h.close();
                h.destroy();
                return run;
            };

            phaseTracker.mark("sync_tail", "尾窗 200 / 完整同步各接入一次");
            const auto tail200 = openAndMeasure(200, "200");
            const auto tail0   = openAndMeasure(0, "0 (full)");

            const auto reportSync
                = [&](const char* point, const SyncRun& run, const char* label) {
                const std::string note = fmt::format(
                    "session.switch -> EVT_SYNC with {}: {} messages, {} bytes, {:.1f} ms",
                    label,
                    run.msgs,
                    run.bytes,
                    run.ms
                );
                auto           memS = sampleMemoryMedian(0);
                ResourceResult resS;
                resS.mode       = "ffi";
                resS.side       = "self";
                resS.point      = point;
                resS.rssMB      = memS.rssMB;
                resS.privateMB  = memS.privateMB;
                resS.cpuIdlePct = -1.0;
                resS.viewCount  = run.msgs;
                resS.viewBytes  = run.bytes;
                resS.note       = note;
                reporter.addResource(resS);
                printResourceResult(resS);
            };
            if (tail200.has_value()) {
                reportSync("sync_tail_200", *tail200, "syncTailCount=200");
            }
            if (tail0.has_value()) {
                reportSync("sync_tail_full", *tail0, "syncTailCount=0 (full)");
            }
        } else {
            std::cout << "  [resource][ffi] tail scenario skipped" << std::endl;
        }
        // 种子句柄必须先关掉: 会话库对每个会话目录取写租约, 同一会话在被它占着时
        // 第二个句柄打开/切进去会直接失败 ("session dir is busy")
        builder.close();
        builder.destroy();
        std::error_code ecTail;
        std::filesystem::remove_all(tailDir, ecTail);
    }

    phaseTracker.printTable("ffi 分阶段内存");
    reporter.attachPhases("ffi", "self", phaseTracker.samples());

    // 停止 + 等 EVT_STOPPED, 再 destroy (v4 的 stop 是异步的, destroy 只认 Stopped)
    if (!ffi.close()) {
        std::cout << "  [resource][ffi] stop/EVT_STOPPED failed: " << ffi.lastError() << std::endl;
    }
    ffi.destroy();
    simChunked.stop();
    sim.stop();
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
    // 环境变量 AGENTXX_BENCH_NO_ISOLATE=1 可退回同进程顺序运行 (快速验证用)
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
        // 原样转发负载缩放系数 (若有)
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
