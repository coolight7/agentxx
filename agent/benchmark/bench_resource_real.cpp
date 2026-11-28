// 真实运行场景的资源基准测试:
// - resource_real_tui       : 真实 TUI (FTXUI 界面线程运行, 真实渲染帧) + 同进程 agent/server
// - resource_server_only    : 真实 server 子进程单独运行 (无客户端常驻, bench 内 WS 客户端驱动负载)
// - resource_real_tui_child : 真实 TUI 子进程 (伪终端驱动, 等同用户敲键盘) + 真实 server 子进程
// - resource_plugin_attrib  : 逐插件加载/卸载的边际内存占用与回收量
//
// 目标: 在尽量贴近真实使用的运行形态下采集内存/CPU/渲染性能数据, 并给出
// 各部分模块的内存归属 (smaps 模块分解 + 逻辑内存统计 + 分阶段增量), 供优化前后对比。

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/channel_io_transport.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/util/observability.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "bench_mem_logical.h"
#include "bench_resource.h"
#include "bench_resource_util.h"
#include "bench_util.h"
#include "fmt/format.h"
#include "utilxx_base/env.h"
#include "utilxx_base/log.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef AGENTXX_BUILD_CLIENT
#include "agentxx-client/io/stdio/agent_stdio.h"
#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/tui_plugin_adapter.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx/agent/io/ws_io_transport.h"
#include "agentxx/plugin/client_plugin_manager.h"
#endif

#if XX_IS_WIN_D
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace agentxx {
namespace bench {

namespace {

// ---------------------------------------------------------------------------
// 标准输出捕获 (真实 TUI 会把界面绘制到 fd 1, 需要隔离并统计渲染字节)
// ---------------------------------------------------------------------------

class StdoutCapture {
public:

    /// captureToFile=false 时输出丢弃到 /dev/null (仅隔离, 不统计)
    explicit StdoutCapture(bool captureToFile, std::string path) :
        path_(std::move(path)) {
        std::fflush(stdout);
        std::fflush(stderr);
#if XX_IS_WIN_D
        savedFd_ = ::_dup(1);
        int fd   = -1;
        if (captureToFile
            && ::_sopen_s(
                   &fd,
                   path_.c_str(),
                   _O_CREAT | _O_TRUNC | _O_WRONLY,
                   _SH_DENYNO,
                   _S_IREAD | _S_IWRITE
               ) == 0) {
            fileFd_ = fd;
        }
        if (fileFd_ < 0) {
            fileFd_ = ::_open("NUL", _O_WRONLY);
            path_.clear();
        }
        if (fileFd_ >= 0) {
            ::_dup2(fileFd_, 1);
        }
#else
        savedFd_ = ::dup(STDOUT_FILENO);
        if (captureToFile) {
            fileFd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        }
        if (fileFd_ < 0) {
            fileFd_ = ::open("/dev/null", O_WRONLY);
            path_.clear();
        }
        if (fileFd_ >= 0) {
            ::dup2(fileFd_, STDOUT_FILENO);
        }
#endif
    }

    ~StdoutCapture() {
        restore();
    }

    StdoutCapture(const StdoutCapture&)            = delete;
    StdoutCapture& operator=(const StdoutCapture&) = delete;

    /// 恢复原始 stdout 并统计捕获字节数
    void restore() {
        if (restored_) {
            return;
        }
        restored_ = true;
        std::fflush(stdout);
#if XX_IS_WIN_D
        if (savedFd_ >= 0) {
            ::_dup2(savedFd_, 1);
            ::_close(savedFd_);
            savedFd_ = -1;
        }
        if (fileFd_ >= 0) {
            ::_close(fileFd_);
            fileFd_ = -1;
        }
#else
        if (savedFd_ >= 0) {
            ::dup2(savedFd_, STDOUT_FILENO);
            ::close(savedFd_);
            savedFd_ = -1;
        }
        if (fileFd_ >= 0) {
            ::close(fileFd_);
            fileFd_ = -1;
        }
#endif
    }

    /// 已捕获字节数 (文件大小)
    size_t capturedBytes() const {
        if (path_.empty()) {
            return 0;
        }
        std::error_code ec;
        auto            size = std::filesystem::file_size(path_, ec);
        return ec ? 0 : static_cast<size_t>(size);
    }

private:

    std::string path_;
    int         savedFd_  = -1;
    int         fileFd_   = -1;
    bool        restored_ = false;
};

// ---------------------------------------------------------------------------
// TUI 渲染性能测量
// ---------------------------------------------------------------------------

struct FrameMeasure {
    uint64_t frames    = 0;
    double   totalMs   = 0.0;
    double   maxMs     = 0.0;
    double   avgMs     = -1.0;
    double   wallMs    = 0.0;
    double   cpuUserMs = 0.0;
    double   cpuSysMs  = 0.0;
};

/// 连续请求 N 帧并统计渲染耗时 (含本进程 CPU 增量)
/// - 仅对真实运行的 TUI (start() 后) 有意义
template<typename TuiT>
FrameMeasure
    measureTuiFrames(const std::shared_ptr<TuiT>& tui, int frames, int perFrameWaitMs = 8) {
    FrameMeasure out;
    tui->resetFrameStats();
    auto win = cpuBeginDetail(0);
    auto t0  = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
        tui->requestRedraw();
        std::this_thread::sleep_for(std::chrono::milliseconds(perFrameWaitMs));
    }
    out.wallMs
        = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    auto stats    = tui->frameStats();
    auto cpuDelta = cpuEndDetailByWindow(win);
    out.frames    = stats.frames;
    out.totalMs   = stats.totalRenderMs;
    out.maxMs     = stats.maxRenderMs;
    out.avgMs
        = (stats.frames > 0) ? (stats.totalRenderMs / static_cast<double>(stats.frames)) : -1.0;
    out.cpuUserMs = cpuDelta.userMs;
    out.cpuSysMs  = cpuDelta.sysMs;
    return out;
}

/// 等待渲染稳定: 帧数在 stableMs 内不再增长 (或超时)
/// - 同步/分页到达后, TUI 会在若干帧内把新内容渲染完; 等稳定后再读帧统计才能
///   覆盖"新消息首次渲染"(冷缓存) 的开销
template<typename TuiT>
void waitTuiFramesSettled(
    const std::shared_ptr<TuiT>& tui,
    int                          stableMs  = 250,
    int                          timeoutMs = 5000
) {
    auto     deadline   = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    auto     lastChange = std::chrono::steady_clock::now();
    uint64_t last       = tui->frameStats().frames;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        uint64_t now = tui->frameStats().frames;
        if (now != last) {
            last       = now;
            lastChange = std::chrono::steady_clock::now();
        } else if (std::chrono::steady_clock::now() - lastChange
                   > std::chrono::milliseconds(stableMs)) {
            break;
        }
    }
}

/// 等待 TUI 渲染出至少 frameCount 帧 (超时返回 false)
template<typename TuiT>
bool waitTuiFrames(const std::shared_ptr<TuiT>& tui, uint64_t frameCount, int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (tui->frameStats().frames >= frameCount) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return tui->frameStats().frames >= frameCount;
}

/// 过滤噪声模块行: 去掉系统库/数据文件 (数量多且与插件加载无关),
/// 保留可执行文件/项目库/插件库/堆/匿名等能反映"插件把内存带到哪里"的行
inline void filterNoisyModules(std::vector<ModuleMemRow>& rows) {
    std::vector<ModuleMemRow> kept;
    for (auto& row : rows) {
        if (row.kind == MemRegionKind::SystemLib || row.kind == MemRegionKind::DataFile
            || row.kind == MemRegionKind::Vdso || row.kind == MemRegionKind::Other) {
            continue;
        }
        kept.push_back(row);
    }
    rows = std::move(kept);
}

#ifdef AGENTXX_BUILD_CLIENT

/// bench 进程内的 headless TUI 端点 (不调用 start(), 仅作协议端点): 用于驱动真实 server
struct HeadlessWsDriver {
    asio::io_context                                                            ctx;
    std::shared_ptr<agentxx::client::TUIClientAgentIO>                          io;
    std::shared_ptr<agent::WsAgentIOTransport>                                  transport;
    std::shared_ptr<agentxx::plugin::ClientPluginManager>                       pluginMgr;
    std::shared_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work;
    std::shared_ptr<std::atomic<bool>> connected = std::make_shared<std::atomic<bool>>(false);
    std::thread                        thread;
    std::string                        sessionId;
};

/// 建立 headless WS 客户端 (返回 nullptr 表示连接失败)
inline std::shared_ptr<HeadlessWsDriver> startHeadlessWsClient(
    std::string_view                        wsUrl,
    std::string_view                        token,
    const std::vector<agent::PluginConfig>& plugins = {}
) {
    auto drv       = std::make_shared<HeadlessWsDriver>();
    drv->sessionId = generateBenchSessionId();
    auto ex        = drv->ctx.get_executor();
    drv->io        = std::make_shared<agentxx::client::TUIClientAgentIO>(
        ex,
        drv->sessionId,
        agentxx::client::TUITheme::darkTheme()
    );
    drv->pluginMgr = std::make_shared<agentxx::plugin::ClientPluginManager>(ex);
    drv->pluginMgr->setUiAdapter(std::make_shared<agentxx::client::TuiPluginAdapter>(drv->io));
    drv->pluginMgr->setSessionId(drv->sessionId);
    drv->io->setPluginManager(drv->pluginMgr);
    drv->io->setEventSink(drv->pluginMgr);

    drv->transport = std::make_shared<agent::WsAgentIOTransport>(
        ex,
        std::string{wsUrl},
        std::string{token},
        agent::WsAgentIOTransport::Config{}
    );
    drv->io->setTransport(drv->transport);

    auto connected = drv->connected;
    asio::co_spawn(
        ex,
        [drv, token = std::string{token}, connected]() -> asio::awaitable<void> {
            agent::WireHello hello{drv->sessionId, token, 0, "", "en"};
            bool             ok = co_await drv->transport->connect(hello);
            if (ok) {
                connected->store(true);
                co_await drv->io->runTransportLoop();
            }
        },
        asio::detached
    );

    drv->work = std::make_shared<asio::executor_work_guard<asio::io_context::executor_type>>(
        drv->ctx.get_executor()
    );
    drv->thread = std::thread([drv]() {
        drv->ctx.run();
    });
    for (int i = 0; i < 250 && !connected->load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!connected->load()) {
        drv->transport->close();
        if (drv->work) {
            drv->work->reset();
        }
        drv->ctx.stop();
        if (drv->thread.joinable()) {
            drv->thread.join();
        }
        return nullptr;
    }
    return drv;
}

inline void stopHeadlessWsClient(const std::shared_ptr<HeadlessWsDriver>& drv) {
    if (!drv) {
        return;
    }
    if (drv->pluginMgr) {
        std::promise<void> p;
        asio::co_spawn(
            drv->ctx,
            [&]() -> asio::awaitable<void> {
                co_await drv->pluginMgr->shutdownAsync();
                p.set_value();
                co_return;
            },
            asio::detached
        );
        p.get_future().wait();
    }
    if (drv->transport) {
        drv->transport->close();
    }
    if (drv->work) {
        drv->work->reset();
    }
    drv->ctx.stop();
    if (drv->thread.joinable()) {
        drv->thread.join();
    }
}

#endif // AGENTXX_BUILD_CLIENT

/// 传输层计数代理 (基准测试观测用): 统计基准场景中客户端/服务端各自
/// 收发消息条数, 用于确认"客户端真的参与了同步/流式接收"而不是只测了服务端
struct CountingTransport : agent::AgentIOTransportBase {
    std::shared_ptr<agent::AgentIOTransportBase> inner;
    std::shared_ptr<std::atomic<uint64_t>>       sends = std::make_shared<std::atomic<uint64_t>>(0);
    std::shared_ptr<std::atomic<uint64_t>>       recvs = std::make_shared<std::atomic<uint64_t>>(0);
    std::shared_ptr<std::atomic<uint64_t>> recvCalls   = std::make_shared<std::atomic<uint64_t>>(0);

    explicit CountingTransport(std::shared_ptr<agent::AgentIOTransportBase> in) :
        inner(std::move(in)) {}

    void send(agent::WireMessage msg) override {
        sends->fetch_add(1);
        inner->send(std::move(msg));
    }

    asio::awaitable<std::optional<agent::WireMessage>> recv() override {
        recvCalls->fetch_add(1);
        auto m = co_await inner->recv();
        if (m.has_value()) {
            recvs->fetch_add(1);
        }
        co_return m;
    }

    void close() override {
        inner->close();
    }

    bool alive() const noexcept override {
        return inner->alive();
    }
};

/// 打印两侧阶段表并挂到报告 (真实两进程场景: server + client)
inline void trackerDumpAndAttach(
    BenchReporter&     reporter,
    const std::string& mode,
    MemPhaseTracker&   serverTracker,
    MemPhaseTracker&   clientTracker
) {
    serverTracker.printTable(mode + " server 分阶段内存");
    clientTracker.printTable(mode + " client 分阶段内存");
    reporter.attachPhases(mode, "server", serverTracker.samples());
    reporter.attachPhases(mode, "client", clientTracker.samples());
}

} // namespace

// ===========================================================================
// 场景 1: 真实运行的 TUI (FTXUI 界面线程) + 同进程 server
// ===========================================================================

void benchResourceRealTui() {
#ifndef AGENTXX_BUILD_CLIENT
    std::cout << "  [resource][real_tui] skipped: AGENTXX_BUILD_CLIENT not enabled" << std::endl;
    return;
#else
    std::cout << "\n=== Resource Benchmark: 真实 TUI (FTXUI 运行中) + 同进程 server ==="
              << std::endl;

    auto        sim      = startResourceLlmSimServer();
    auto        tmpDir   = createBenchTempDir("bench_real_tui");
    auto&       reporter = BenchReporter::instance();
    const auto& counts   = getCalibratedCounts();

    MemPhaseTracker phaseTracker(0, "self");
    phaseTracker.mark("process_base", "仅 mock LLM 服务; agent/TUI 未构建");

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
    agentConfig->plugins                  = bench5PluginConfigs();

    auto        agent     = std::make_shared<agent::CodeAgent>(agentConfig);
    auto        agentWork = asio::make_work_guard(*agent->ioCtx);
    std::thread agentThread([agent]() {
        agent->ioCtx->run();
    });
    phaseTracker.mark("agent_constructed", "CodeAgent + io_context 线程已创建");

    asio::io_context clientCtx;
    // poll() 会因"无工作"把 io_context 标记为 stopped, 之后 run() 立即返回 →
    // 客户端接收循环永不启动, 收发全部失效; 故先持有 work_guard 再进入任何轮询
    auto        clientWork = asio::make_work_guard(clientCtx);
    auto        clientEx   = clientCtx.get_executor();
    std::string sessionId  = generateBenchSessionId();

    // 真实 TUI: start() 会创建组件树并在独立线程跑 FTXUI 循环 (真实渲染帧)
    auto tui = std::make_shared<agentxx::client::TUIClientAgentIO>(
        clientEx,
        sessionId,
        agentxx::client::TUITheme::darkTheme()
    );
    tui->setDataDir((tmpDir / "data").string());
    tui->setWorkDir(tmpDir.string());

    auto pluginMgr = std::make_shared<agentxx::plugin::ClientPluginManager>(clientEx);
    pluginMgr->setUiAdapter(std::make_shared<agentxx::client::TuiPluginAdapter>(tui));
    pluginMgr->setSessionId(sessionId);
    tui->setPluginManager(pluginMgr);
    tui->setEventSink(pluginMgr);

    // TUI 直接绘制到 fd 1: 重定向到捕获文件, 既隔离 TUI 输出, 又统计渲染字节;
    // 期间基准自身的打印改写到延迟缓冲 (console), 场景结束后统一输出到 stdout
    std::ostringstream console;
    StdoutCapture      capture(true, (tmpDir / "tui_render.out").string());
    tui->start();
    bool firstFrameOk = waitTuiFrames(tui, 1, 5000);
    phaseTracker.mark(
        "tui_started",
        fmt::format("FTXUI 界面线程启动, 首帧{}", firstFrameOk ? "已渲染" : "超时")
    );

    std::atomic<bool> pluginsLoaded{false};
    asio::co_spawn(
        clientEx,
        [&]() -> asio::awaitable<void> {
            co_await pluginMgr->loadConfiguredClientPlugins(agentConfig->plugins);
            pluginsLoaded.store(true);
            co_return;
        },
        asio::detached
    );
    while (!pluginsLoaded.load()) {
        clientCtx.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Channel 直连 (与真实本地模式一致)
    auto agentEx = agent->ioCtx->get_executor();

    auto [clientTransport, serverTransport]
        = agent::ChannelAgentIOTransport::makePair(clientEx, agentEx);
    auto countingClient = std::make_shared<CountingTransport>(
        std::shared_ptr<agent::AgentIOTransportBase>(std::move(clientTransport))
    );
    auto countingServer = std::make_shared<CountingTransport>(
        std::shared_ptr<agent::AgentIOTransportBase>(std::move(serverTransport))
    );
    tui->setTransport(countingClient);

    agent::SessionServerAgentIO::Config scCfg;
    scCfg.sessionId            = sessionId;
    scCfg.initialSyncTailCount = 100; // TUI 尾窗 100 条 + 向上分页
    auto serverIO = std::make_shared<agent::SessionServerAgentIO>(agentEx, agent, scCfg);
    serverIO->setTransport(countingServer);

    std::atomic<bool> serverReady{false};
    if (auto ctx = agent->agentContext) {
        ctx->initNotifier = [tui](std::string_view step) {
            tui->onServerProgress(step);
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

    tui->sendToPeer(agent::WireHello{sessionId, "", 0, "", "en"});
    tui->sendToPeer(agent::WireGetModel{sessionId});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 首轮预热 (真实走 LLM + 工具 + 流式渲染)
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
    phaseTracker.mark("warmup_turn_done", "首轮真实对话完成 (流式渲染)");

    // 逻辑内存采集 (agent 侧须在 agent io 线程取会话)
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
        if (auto snap = tui->sharedState().readSnapshot()) {
            auto tuiRows = collectTuiLogicalMem(*snap);
            target.logical.insert(target.logical.end(), tuiRows.begin(), tuiRows.end());
        }
    };

    // 渲染性能: 连续 40 帧的耗时与 CPU 增量 (真实 TUI 的主要运行开销)
    auto frameMeasure = measureTuiFrames(tui, 40);
    console << fmt::format(
        "  [real_tui] 渲染测量: {} 帧, 平均 {:.3f} ms/帧, 最大 {:.3f} ms, CPU {:.0f}ms(user+sys)\n",
        frameMeasure.frames,
        frameMeasure.avgMs,
        frameMeasure.maxMs,
        frameMeasure.cpuUserMs + frameMeasure.cpuSysMs
    );

    // ---------------- P0: Startup ----------------
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    auto idleCpuWin = cpuBegin(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    double idleCpu = cpuEnd(idleCpuWin);

    ResourceResult res0;
    res0.mode          = "real_tui";
    res0.side          = "self";
    res0.point         = "startup";
    res0.cpuIdlePct    = idleCpu;
    res0.cpuBusyPct    = 0.0;
    res0.cpu           = idleCpuWin.result;
    res0.pluginsAgent  = agent->agentContext->pluginManager->list().size();
    res0.pluginsClient = pluginMgr->list().size();
    res0.frames        = tui->frameStats().frames;
    res0.frameAvgMs    = frameMeasure.avgMs;
    res0.note          = fmt::format(
        "真实 TUI (FTXUI 运行中), Channel, tail=100; 累计渲染{}帧, 空转窗口{}帧/平均{:.3f}ms",
        res0.frames,
        frameMeasure.frames,
        frameMeasure.avgMs
    );
    {
        std::promise<void> p;
        asio::post(*agent->ioCtx, [&]() {
            if (auto sess = agent->agentContext->getSession(sessionId)) {
                res0.viewCount = sess->viewWindow().size();
                res0.viewBytes = estimateViewMessagesBytes(sess->viewWindow());
                res0.llmCount  = sess->llmMessagesJson().size();
                res0.llmBytes  = estimateLlmMessagesBytes(sess->llmMessagesJson());
            }
            p.set_value();
        });
        p.get_future().wait();
    }
    collectLogical(res0);
    fillResourceMemDetail(res0, 0, true, false);
    reporter.addResource(res0);
    printResourceResult(res0, console);
    {
        ModuleMemBreakdown bd;
        bd.rows       = res0.modules;
        bd.valid      = true;
        bd.totalRssMB = res0.rssMB;
        bd.totalPssMB = res0.mem.pssMB;
        printKindSummary(bd, "real_tui startup", console);
    }

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

    // TUI 触发同步 (尾窗 100) + 分页拉取更早历史; 该窗口内的帧统计即"新内容首次渲染"成本
    // 历史同步 + 分页拉取 (模拟用户滚动查看更早历史):
    // - 服务端仅同步末尾 initialSyncTailCount 条; 更早历史由客户端按
    //   historyWindowStart 逐页拉取 (benchmark 此处复刻 TUI 里 requestOlderHistory 的做法)
    auto syncAndPullHistory = [&](int pages) {
        tui->sendToPeer(agent::WireHello{sessionId, "", 0, "", "en"});
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        for (int p = 0; p < pages; ++p) {
            uint64_t start = 0;
            if (auto snap = tui->sharedState().readSnapshot()) {
                start = snap->history.windowStart();
            }
            if (start == 0) {
                break; // 窗口已到顶
            }
            tui->sendToPeer(agent::WireGetViewMessages{sessionId, start, 100});
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
    };

    tui->resetFrameStats();
    syncAndPullHistory(2);
    waitTuiFramesSettled(tui, 250, 5000);
    auto   stats1   = tui->frameStats();
    double busyCpu1 = cpuEnd(busyWin1);
    phaseTracker.mark("ctx100k_ready", "已注入 ~100K token 历史并完成同步/渲染");

    auto res1         = res0;
    res1.point        = "ctx100k";
    res1.cpuIdlePct   = -1.0;
    res1.cpuBusyPct   = busyCpu1;
    res1.cpu          = busyWin1.result;
    res1.tokens       = counts.actualTokens100;
    res1.frames       = stats1.frames;
    res1.frameAvgMs   = (stats1.frames > 0) ? (stats1.totalRenderMs / stats1.frames) : -1.0;
    size_t tuiLoaded1 = 0;
    if (auto snap = tui->sharedState().readSnapshot()) {
        tuiLoaded1 = snap->messages().size();
    }
    res1.note = fmt::format(
        "注入固定组 + 尾窗同步/分页({}条); 同步窗口渲染{}帧/平均{:.2f}ms/最大{:.2f}ms; "
        "传输(client→server {}/server→client {})",
        tuiLoaded1,
        res1.frames,
        res1.frameAvgMs,
        stats1.maxRenderMs,
        countingClient->sends->load(),
        countingClient->recvs->load()
    );
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
    res1.logical.clear();
    collectLogical(res1);
    fillResourceMemDetail(res1, 0, true, false);
    reporter.addResource(res1);
    printResourceResult(res1, console);
    {
        ModuleMemBreakdown bd;
        bd.rows       = res1.modules;
        bd.valid      = true;
        bd.totalRssMB = res1.rssMB;
        bd.totalPssMB = res1.mem.pssMB;
        printKindSummary(bd, "real_tui ctx100k", console);
    }

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

    tui->resetFrameStats();
    syncAndPullHistory(2);
    waitTuiFramesSettled(tui, 250, 5000);
    auto   stats2   = tui->frameStats();
    double busyCpu2 = cpuEnd(busyWin2);
    phaseTracker.mark("ctx200k_ready", "已注入 ~200K token 历史并完成同步/渲染");

    auto res2         = res0;
    res2.point        = "ctx200k";
    res2.cpuIdlePct   = -1.0;
    res2.cpuBusyPct   = busyCpu2;
    res2.cpu          = busyWin2.result;
    res2.tokens       = counts.actualTokens200;
    res2.frames       = stats2.frames;
    res2.frameAvgMs   = (stats2.frames > 0) ? (stats2.totalRenderMs / stats2.frames) : -1.0;
    size_t tuiLoaded2 = 0;
    if (auto snap = tui->sharedState().readSnapshot()) {
        tuiLoaded2 = snap->messages().size();
    }
    res2.note = fmt::format(
        "注入固定组 + 尾窗同步/分页({}条); 同步窗口渲染{}帧/平均{:.2f}ms/最大{:.2f}ms; "
        "传输(client→server {}/server->client {})",
        tuiLoaded2,
        res2.frames,
        res2.frameAvgMs,
        stats2.maxRenderMs,
        countingClient->sends->load(),
        countingClient->recvs->load()
    );
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
    res2.logical.clear();
    collectLogical(res2);
    fillResourceMemDetail(res2, 0, true, true); // 含 malloc_trim 可回收量
    {
        ModuleMemBreakdown bd;
        bd.rows       = res2.modules;
        bd.valid      = true;
        bd.totalRssMB = res2.rssMB;
        bd.totalPssMB = res2.mem.pssMB;
        printKindSummary(bd, "real_tui ctx200k", console);
    }
    res2.renderBytes = static_cast<double>(capture.capturedBytes());
    res2.note += fmt::format(", 渲染输出 {:.1f} MB", res2.renderBytes / (1024.0 * 1024.0));
    reporter.addResource(res2);
    printResourceResult(res2, console);

    phaseTracker.printTable("real_tui 分阶段内存", console);
    reporter.attachPhases("real_tui", "self", phaseTracker.samples());

    // 退出: 先停止 TUI (退出全屏/raw 模式并加入 UI 线程), 再恢复 stdout 并打印延迟缓冲
    tui->stop();
    capture.restore();
    std::cout << console.str();
    std::cout << fmt::format(
        "  [real_tui] 累计渲染 {} 帧, 渲染输出 {:.2f} MB\n",
        tui->frameStats().frames,
        static_cast<double>(capture.capturedBytes()) / (1024.0 * 1024.0)
    );

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
// 场景 2: 真实 server 子进程单独运行 (无客户端常驻)
// ===========================================================================

void benchResourceServerOnly() {
    std::cout << "\n=== Resource Benchmark: 真实 server 子进程单独运行 ===" << std::endl;

    std::string cliBin = findAgentxxExePath();
    if (cliBin.empty()) {
        std::cout << "  [resource][server_only] skipped: agentxx binary not found" << std::endl;
        return;
    }

    auto        sim      = startResourceLlmSimServer();
    auto        tmpDir   = createBenchTempDir("bench_server_only");
    auto&       reporter = BenchReporter::instance();
    const auto& counts   = getCalibratedCounts();
    (void)counts; // 仅在编译了 client 支持时用于驱动轮次

    uint16_t    serverPort = findFreeTcpPort();
    std::string token      = "bench_server_only_token";

    RealRunConfigOptions cfgOpts;
    cfgOpts.dataDir        = (tmpDir / "data_server").string();
    cfgOpts.workDir        = tmpDir.string();
    cfgOpts.llmBaseUrl     = fmt::format("http://127.0.0.1:{}/v1", sim.port);
    std::string serverYaml = (tmpDir / "server.yaml").string();
    {
        std::ofstream ofs(serverYaml);
        ofs << buildAgentYamlConfig(cfgOpts);
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
    auto serverProc            = spawnChildProcess(cliBin, serverArgs, serverSpawn);
    if (!serverProc.running) {
        std::cout << "  [resource][server_only] failed to spawn server process" << std::endl;
        return;
    }
    if (!waitForTcpPort("127.0.0.1", serverPort, 20000)) {
        std::cout << "  [resource][server_only] server 未在 20s 内监听端口, 详见 "
                  << serverSpawn.outputRedirect << std::endl;
        stopChildProcess(serverProc);
        return;
    }

    MemPhaseTracker tracker(serverProc.pid, "server");
    tracker.mark("listening", "server 已监听端口 (无客户端)");

    // 空载采样 (含真实配置文件解析/插件加载/存储初始化后的稳态)
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    ResourceResult resIdle;
    resIdle.mode         = "server_only";
    resIdle.side         = "server";
    resIdle.point        = "idle";
    resIdle.pluginsAgent = 5;
    resIdle.note         = "真实 server 子进程, 无客户端常驻, 空载稳态";
    fillResourceMemDetail(resIdle, serverProc.pid, true, false);
    reporter.addResource(resIdle);
    printResourceResult(resIdle);

    // 空载波动观测 (15s 内是否有后台自发增长)
    auto idleCpuWin = cpuBegin(serverProc.pid);
    auto idleRss    = resIdle.rssMB;
    std::this_thread::sleep_for(std::chrono::seconds(5));
    auto   driftSample = sampleProcMemDetailMedian(serverProc.pid, 3, 30);
    double idleCpu     = cpuEnd(idleCpuWin);
    tracker.mark(
        "idle_5s",
        fmt::format("空载 5s 后 (ΔRSS {:+.2f} MB)", driftSample.rssMB - idleRss)
    );
    ResourceResult resDrift;
    resDrift.mode         = "server_only";
    resDrift.side         = "server";
    resDrift.point        = "idle_5s";
    resDrift.cpuIdlePct   = idleCpu;
    resDrift.cpu          = idleCpuWin.result;
    resDrift.pluginsAgent = 5;
    resDrift.note = fmt::format("空载 5s 漂移 ΔRSS {:+.2f} MB", driftSample.rssMB - idleRss);
    fillResourceMemDetail(resDrift, serverProc.pid, true, false);
    reporter.addResource(resDrift);
    printResourceResult(resDrift);

#ifdef AGENTXX_BUILD_CLIENT
    // bench 进程内的 headless WS 客户端 (仅用于驱动负载; 不统计其内存)
    std::string wsUrl = fmt::format("ws://127.0.0.1:{}/agent", serverPort);
    auto        drv   = startHeadlessWsClient(wsUrl, token);
    if (!drv) {
        std::cout << "  [resource][server_only] 无法建立 WS 连接, 跳过负载阶段" << std::endl;
        tracker.printTable("server_only 分阶段内存");
        reporter.attachPhases("server_only", "server", tracker.samples());
        stopChildProcess(serverProc);
        std::error_code ec;
        std::filesystem::remove_all(tmpDir, ec);
        return;
    }
    tracker.mark("client_attached", "bench WS 客户端接入 (服务端会话已建立)");

    // 预热一轮
    {
        size_t startTurn = sim.turnCounter->load();
        drv->io->sendToPeer(agent::WireUserInput{drv->sessionId, "hello"});
        for (int w = 0; w < 300; ++w) {
            if (sim.turnCounter->load() > startTurn) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    const double scale = benchLoadScale();
    size_t       n100  = std::max<size_t>(1, static_cast<size_t>(counts.n100 * scale));
    size_t       n200  = std::max<size_t>(n100, static_cast<size_t>(counts.n200 * scale));

    auto driveTurns = [&](size_t from, size_t to, bool first) -> size_t {
        size_t startTurn = sim.turnCounter->load();
        size_t done      = 0;
        for (size_t i = from; i < to; ++i) {
            std::string userText = fmt::format(
                "RES-BENCH user turn {:06d} | The quick brown fox jumps over the lazy dog. 请列出当前目录并读取 README 前 40 行。 #FIXED-9f3a",
                i + 1
            );
            drv->io->sendToPeer(agent::WireUserInput{drv->sessionId, userText});
            size_t target = startTurn + (i - from) + 1;
            bool   ok     = false;
            for (int w = 0; w < 300; ++w) {
                if (sim.turnCounter->load() >= target) {
                    ok = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!ok) {
                break;
            }
            ++done;
        }
        (void)first;
        return done;
    };

    // ---------------- P1: ~100K ----------------
    auto   busyWin1 = cpuBegin(serverProc.pid);
    size_t done1    = driveTurns(0, n100, true);
    double busyCpu1 = cpuEnd(busyWin1);
    tracker.mark(
        "ctx100k",
        fmt::format(
            "真实 WS 轮次驱动 {} 轮 (负载缩放 {:.2f}, 目标组数 {}, 单组约 {} token)",
            done1,
            scale,
            n100,
            counts.groupTokens
        )
    );

    ResourceResult res1;
    res1.mode         = "server_only";
    res1.side         = "server";
    res1.point        = "ctxScaled1";
    res1.cpuBusyPct   = busyCpu1;
    res1.cpu          = busyWin1.result;
    res1.tokens       = done1 * counts.groupTokens;
    res1.llmCount     = done1 * 3;
    res1.llmBytes     = done1 * 3800;
    res1.pluginsAgent = 5;
    res1.note         = fmt::format(
        "真实 WS 轮次 {} 轮 (scale={:.2f}); 目标 token {}",
        done1,
        scale,
        n100 * counts.groupTokens
    );
    fillResourceMemDetail(res1, serverProc.pid, true, false);
    reporter.addResource(res1);
    printResourceResult(res1);

    // ---------------- P2: ~200K ----------------
    auto   busyWin2 = cpuBegin(serverProc.pid);
    size_t done2    = driveTurns(n100, n200, false);
    double busyCpu2 = cpuEnd(busyWin2);
    tracker.mark("ctx200k", fmt::format("再驱动 {} 轮", done2));

    ResourceResult res2;
    res2.mode         = "server_only";
    res2.side         = "server";
    res2.point        = "ctxScaled2";
    res2.cpuBusyPct   = busyCpu2;
    res2.cpu          = busyWin2.result;
    res2.tokens       = (done1 + done2) * counts.groupTokens;
    res2.llmCount     = (done1 + done2) * 3;
    res2.llmBytes     = (done1 + done2) * 3800;
    res2.pluginsAgent = 5;
    res2.note         = fmt::format("累计 {} 轮 (scale={:.2f})", done1 + done2, scale);
    fillResourceMemDetail(res2, serverProc.pid, true, false);
    reporter.addResource(res2);
    printResourceResult(res2);

    // ---------------- 客户端断开后的回收情况 ----------------
    double rssBeforeDisconnect = res2.rssMB;
    stopHeadlessWsClient(drv);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    auto memAfter = sampleProcMemDetailMedian(serverProc.pid, 3, 30);
    tracker.mark(
        "client_detached",
        fmt::format("客户端断开 2s 后 (ΔRSS {:+.2f} MB)", memAfter.rssMB - rssBeforeDisconnect)
    );
    ResourceResult res3;
    res3.mode         = "server_only";
    res3.side         = "server";
    res3.point        = "after_disconnect";
    res3.pluginsAgent = 5;
    res3.note         = fmt::format(
        "客户端断开后; ΔRSS {:+.2f} MB (未释放的会话/缓冲)",
        memAfter.rssMB - rssBeforeDisconnect
    );
    fillResourceMemDetail(res3, serverProc.pid, true, false);
    reporter.addResource(res3);
    printResourceResult(res3);
#else
    std::cout << "  [resource][server_only] 未编译 client 支持, 跳过负载阶段" << std::endl;
#endif

    tracker.printTable("server_only 分阶段内存");
    reporter.attachPhases("server_only", "server", tracker.samples());

    stopChildProcess(serverProc);
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
}

// ===========================================================================
// 场景 3: 真实 TUI 子进程 (伪终端驱动) + 真实 server 子进程
// ===========================================================================

void benchResourceRealTuiChild() {
#if XX_IS_WIN_D
    std::cout << "  [resource][real_tui_child] skipped: 当前平台无伪终端支持 (Windows)"
              << std::endl;
    return;
#else
    std::cout << "\n=== Resource Benchmark: 真实 TUI 子进程 (伪终端) + 真实 server 子进程 ==="
              << std::endl;

    std::string cliBin = findAgentxxExePath();
    if (cliBin.empty()) {
        std::cout << "  [resource][real_tui_child] skipped: agentxx binary not found"
                  << std::endl;
        return;
    }

    auto        sim      = startResourceLlmSimServer();
    auto        tmpDir   = createBenchTempDir("bench_real_tui_child");
    auto&       reporter = BenchReporter::instance();
    const auto& counts   = getCalibratedCounts();

    uint16_t    serverPort = findFreeTcpPort();
    std::string token      = "bench_tui_child_token";

    RealRunConfigOptions serverCfg;
    serverCfg.dataDir      = (tmpDir / "data_server").string();
    serverCfg.workDir      = tmpDir.string();
    serverCfg.llmBaseUrl   = fmt::format("http://127.0.0.1:{}/v1", sim.port);
    std::string serverYaml = (tmpDir / "server.yaml").string();
    {
        std::ofstream ofs(serverYaml);
        ofs << buildAgentYamlConfig(serverCfg);
    }

    RealRunConfigOptions clientCfg;
    clientCfg.dataDir      = (tmpDir / "data_client").string();
    clientCfg.workDir      = tmpDir.string();
    clientCfg.llmBaseUrl   = serverCfg.llmBaseUrl;
    std::string clientYaml = (tmpDir / "client.yaml").string();
    {
        std::ofstream ofs(clientYaml);
        ofs << buildClientYamlConfig(clientCfg);
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
    auto serverProc            = spawnChildProcess(cliBin, serverArgs, serverSpawn);
    if (!serverProc.running) {
        std::cout << "  [resource][real_tui_child] failed to spawn server" << std::endl;
        return;
    }
    if (!waitForTcpPort("127.0.0.1", serverPort, 20000)) {
        std::cout << "  [resource][real_tui_child] server 未监听端口, 详见 "
                  << serverSpawn.outputRedirect << std::endl;
        stopChildProcess(serverProc);
        return;
    }

    // 真实 TUI 客户端: 分配伪终端 (等同用户在一个终端里启动 TUI)
    std::string              wsUrl = fmt::format("ws://127.0.0.1:{}/agent", serverPort);
    std::vector<std::string> clientArgs
        = {"tui", "--config", clientYaml, "--agent", wsUrl, "--token", token};
    SpawnOptions clientSpawn;
    clientSpawn.workingDir = tmpDir.string();
    clientSpawn.usePty     = true;
    clientSpawn.ptyCols    = 200;
    clientSpawn.ptyRows    = 50;
    clientSpawn.env        = {
        {"TERM", "xterm-256color"},
        {"LANG", "zh_CN.UTF-8"   }
    };
    auto clientProc = spawnChildProcess(cliBin, clientArgs, clientSpawn);
    if (!clientProc.running) {
        std::cout << "  [resource][real_tui_child] failed to spawn tui client" << std::endl;
        stopChildProcess(serverProc);
        return;
    }

    MemPhaseTracker serverTracker(serverProc.pid, "server");
    MemPhaseTracker clientTracker(clientProc.pid, "client");
    serverTracker.mark("ready", "server 已监听端口");
    clientTracker.mark(
        "spawn",
        fmt::format("TUI 客户端已启动 (PTY {})", clientProc.isPty ? "on" : "off")
    );

    // 等待 TUI 首屏 (排空伪终端输出, 避免子进程阻塞在写操作上)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (std::chrono::steady_clock::now() < deadline) {
            drainChildPtyOutput(clientProc);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    size_t ptyBytesReady = clientProc.ptyBytes;
    clientTracker.mark(
        "first_screen",
        fmt::format("首屏渲染输出 {:.0f} KB", static_cast<double>(ptyBytesReady) / 1024.0)
    );

    if (!childProcessAlive(clientProc)) {
        std::cout << "  [resource][real_tui_child] TUI 客户端已退出, 跳过负载阶段" << std::endl;
        trackerDumpAndAttach(reporter, "real_tui_child", serverTracker, clientTracker);
        stopChildProcess(serverProc);
        std::error_code ec;
        std::filesystem::remove_all(tmpDir, ec);
        return;
    }

    // ---------------- 空载采样 ----------------
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto idleWinServer = cpuBegin(serverProc.pid);
    auto idleWinClient = cpuBegin(clientProc.pid);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    double idleCpuServer = cpuEnd(idleWinServer);
    double idleCpuClient = cpuEnd(idleWinClient);

    ResourceResult resServer0;
    resServer0.mode         = "real_tui_child";
    resServer0.side         = "server";
    resServer0.point        = "startup";
    resServer0.cpuIdlePct   = idleCpuServer;
    resServer0.cpu          = idleWinServer.result;
    resServer0.pluginsAgent = 5;
    resServer0.note         = "真实 server 进程, 客户端为真实 TUI 进程";
    fillResourceMemDetail(resServer0, serverProc.pid, true, false);
    reporter.addResource(resServer0);
    printResourceResult(resServer0);

    ResourceResult resClient0;
    resClient0.mode          = "real_tui_child";
    resClient0.side          = "client";
    resClient0.point         = "startup";
    resClient0.cpuIdlePct    = idleCpuClient;
    resClient0.cpu           = idleWinClient.result;
    resClient0.pluginsClient = 4;
    resClient0.renderBytes   = static_cast<double>(ptyBytesReady);
    resClient0.note          = fmt::format(
        "真实 TUI 进程 (PTY, {}x{}) 空载; 首屏输出 {:.0f} KB",
        clientSpawn.ptyCols,
        clientSpawn.ptyRows,
        static_cast<double>(ptyBytesReady) / 1024.0
    );
    fillResourceMemDetail(resClient0, clientProc.pid, true, false);
    reporter.addResource(resClient0);
    printResourceResult(resClient0);

    const double scale = benchLoadScale();
    size_t       n100  = std::max<size_t>(1, static_cast<size_t>(counts.n100 * scale));
    size_t       n200  = std::max<size_t>(n100, static_cast<size_t>(counts.n200 * scale));

    // 通过伪终端"打字"驱动轮次: 写入一行文本 + 回车 (等同用户输入)
    auto typeTurns = [&](size_t from, size_t to) -> size_t {
        size_t startTurn = sim.turnCounter->load();
        size_t done      = 0;
        for (size_t i = from; i < to; ++i) {
            if (!childProcessAlive(clientProc)) {
                break;
            }
            std::string line = fmt::format(
                "RES-BENCH user turn {:06d} | 请列出当前目录并读取 README 前 40 行。 #FIXED-9f3a\r",
                i + 1
            );
            if (!writeToChildStdin(clientProc, line)) {
                break;
            }
            size_t target = startTurn + (i - from) + 1;
            bool   ok     = false;
            for (int w = 0; w < 400; ++w) {
                drainChildPtyOutput(clientProc);
                if (sim.turnCounter->load() >= target) {
                    ok = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!ok) {
                break;
            }
            ++done;
        }
        drainChildPtyOutput(clientProc);
        return done;
    };

    // ---------------- P1: 缩放的 100K 轮次 ----------------
    size_t ptyBytes1   = clientProc.ptyBytes;
    auto   busyWinSrv1 = cpuBegin(serverProc.pid);
    auto   busyWinCli1 = cpuBegin(clientProc.pid);
    size_t done1       = typeTurns(0, n100);
    double busyCpuSrv1 = cpuEnd(busyWinSrv1);
    double busyCpuCli1 = cpuEnd(busyWinCli1);
    serverTracker.mark("ctx100k", fmt::format("真实轮次 {} 轮", done1));
    clientTracker.mark(
        "ctx100k",
        fmt::format(
            "真实轮次 {} 轮; PTY 渲染输出 {:.1f} MB",
            done1,
            static_cast<double>(clientProc.ptyBytes - ptyBytes1) / (1024.0 * 1024.0)
        )
    );

    ResourceResult resServer1;
    resServer1.mode         = "real_tui_child";
    resServer1.side         = "server";
    resServer1.point        = "ctxScaled1";
    resServer1.cpuBusyPct   = busyCpuSrv1;
    resServer1.cpu          = busyWinSrv1.result;
    resServer1.pluginsAgent = 5;
    resServer1.tokens       = done1 * counts.groupTokens;
    resServer1.llmCount     = done1 * 3;
    resServer1.llmBytes     = done1 * 3800;
    resServer1.note         = fmt::format("真实轮次 {} (scale={:.2f})", done1, scale);
    fillResourceMemDetail(resServer1, serverProc.pid, true, false);
    reporter.addResource(resServer1);
    printResourceResult(resServer1);

    ResourceResult resClient1;
    resClient1.mode          = "real_tui_child";
    resClient1.side          = "client";
    resClient1.point         = "ctxScaled1";
    resClient1.cpuBusyPct    = busyCpuCli1;
    resClient1.cpu           = busyWinCli1.result;
    resClient1.pluginsClient = 4;
    resClient1.tokens        = done1 * counts.groupTokens;
    resClient1.renderBytes   = static_cast<double>(clientProc.ptyBytes - ptyBytes1);
    resClient1.note          = fmt::format(
        "真实 TUI 进程: {} 轮; 渲染输出 {:.1f} MB",
        done1,
        resClient1.renderBytes / (1024.0 * 1024.0)
    );
    fillResourceMemDetail(resClient1, clientProc.pid, true, false);
    reporter.addResource(resClient1);
    printResourceResult(resClient1);

    // ---------------- P2: 再一轮缩放负载 ----------------
    size_t ptyBytes2   = clientProc.ptyBytes;
    auto   busyWinSrv2 = cpuBegin(serverProc.pid);
    auto   busyWinCli2 = cpuBegin(clientProc.pid);
    size_t done2       = typeTurns(n100, n200);
    double busyCpuSrv2 = cpuEnd(busyWinSrv2);
    double busyCpuCli2 = cpuEnd(busyWinCli2);
    serverTracker.mark("ctx200k", fmt::format("累计 {} 轮", done1 + done2));
    clientTracker.mark("ctx200k", fmt::format("客户端累计接收 {} 轮", done1 + done2));

    ResourceResult resServer2;
    resServer2.mode         = "real_tui_child";
    resServer2.side         = "server";
    resServer2.point        = "ctxScaled2";
    resServer2.cpuBusyPct   = busyCpuSrv2;
    resServer2.cpu          = busyWinSrv2.result;
    resServer2.pluginsAgent = 5;
    resServer2.tokens       = (done1 + done2) * counts.groupTokens;
    resServer2.llmCount     = (done1 + done2) * 3;
    resServer2.llmBytes     = (done1 + done2) * 3800;
    resServer2.note = fmt::format("累计真实轮次 {} (scale={:.2f})", done1 + done2, scale);
    fillResourceMemDetail(resServer2, serverProc.pid, true, false);
    reporter.addResource(resServer2);
    printResourceResult(resServer2);

    ResourceResult resClient2;
    resClient2.mode          = "real_tui_child";
    resClient2.side          = "client";
    resClient2.point         = "ctxScaled2";
    resClient2.cpuBusyPct    = busyCpuCli2;
    resClient2.cpu           = busyWinCli2.result;
    resClient2.pluginsClient = 4;
    resClient2.tokens        = (done1 + done2) * counts.groupTokens;
    resClient2.renderBytes   = static_cast<double>(clientProc.ptyBytes - ptyBytes2);
    resClient2.note          = fmt::format(
        "真实 TUI 进程: 累计 {} 轮; 本阶段渲染输出 {:.1f} MB",
        done1 + done2,
        resClient2.renderBytes / (1024.0 * 1024.0)
    );
    fillResourceMemDetail(resClient2, clientProc.pid, true, false);
    reporter.addResource(resClient2);
    printResourceResult(resClient2);

    trackerDumpAndAttach(reporter, "real_tui_child", serverTracker, clientTracker);

    // 清理
    stopChildProcess(clientProc);
    stopChildProcess(serverProc);
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
#endif
}

// ===========================================================================
// 场景 4: 逐插件加载的内存边际成本与卸载回收
// ===========================================================================

void benchResourcePluginAttrib() {
    std::cout << "\n=== Resource Benchmark: 逐插件内存边际成本 (plugin_attrib) ===" << std::endl;

    auto  tmpDir   = createBenchTempDir("bench_plugin_attrib");
    auto& reporter = BenchReporter::instance();

    MemPhaseTracker tracker(0, "self");
    tracker.mark("process_base", "基准进程基线 (未构建 agent)");

    auto agentConfig            = std::make_shared<agent::AgentConfig>();
    agentConfig->dataDir        = (tmpDir / "data").string();
    agentConfig->workDir        = tmpDir.string();
    agentConfig->permissionMode = agent::PermissionMode::Pass;
    // 不预加载插件: 由本场景逐个加载以测量边际成本
    agentConfig->enableSessionStore = false;
    agentConfig->enableSubagent     = false;
    agentConfig->enableWorktree     = false;

    auto        agent     = std::make_shared<agent::CodeAgent>(agentConfig);
    auto        agentWork = asio::make_work_guard(*agent->ioCtx);
    std::thread agentThread([agent]() {
        agent->ioCtx->run();
    });

    std::atomic<bool> initDone{false};
    asio::co_spawn(
        *agent->ioCtx,
        [&]() -> asio::awaitable<void> {
            co_await agent->init();
            initDone.store(true);
            co_return;
        },
        asio::detached
    );
    for (int i = 0; i < 600 && !initDone.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!initDone.load()) {
        std::cout << "  [plugin_attrib] agent init 超时, 跳过" << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    tracker.mark("agent_no_plugin", "agent 初始化完成, 未加载任何插件");

    auto collectLogicalGlobal = [&](ResourceResult& target) {
        std::promise<void> p;
        asio::post(*agent->ioCtx, [&]() {
            target.logical = collectAgentLogicalMem(agent->agentContext, AgentLogicalOptions{});
            p.set_value();
        });
        p.get_future().wait();
    };

    // 基线点 (无插件)
    {
        ResourceResult base;
        base.mode  = "plugin_attrib";
        base.side  = "self";
        base.point = "none";
        base.note  = "未加载任何插件 (仅 agent 核心)";
        collectLogicalGlobal(base);
        fillResourceMemDetail(base, 0, true, false);
        filterNoisyModules(base.modules);
        reporter.addResource(base);
        printResourceResult(base);
    }

    // 逐个插件加载: 每次都测量相对上一次的增量
    for (const auto& name : getBench5PluginNames()) {
        std::string dir = resolveBenchPluginDir(name);

        std::promise<bool> loaded;
        asio::co_spawn(
            *agent->ioCtx,
            [&]() -> asio::awaitable<void> {
                bool ok = false;
                try {
                    auto inst = co_await agent->agentContext->pluginManager
                                    ->loadPluginAsync(dir, nullptr, true);
                    ok = (inst != nullptr);
                } catch (const std::exception& e) {
                    XX_LOGW("[plugin_attrib] 加载 {} 失败: {}", name, e.what());
                }
                loaded.set_value(ok);
                co_return;
            },
            asio::detached
        );
        bool ok = loaded.get_future().get();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        const auto item = tracker.mark("plugin_" + name, ok ? "" : "加载失败");

        ResourceResult r;
        r.mode         = "plugin_attrib";
        r.side         = "self";
        r.point        = name;
        r.pluginsAgent = agent->agentContext->pluginManager->list().size();
        r.note         = fmt::format(
            "{}: 边际 ΔRSS {:+.2f} MB / ΔPSS {:+.2f} MB / Δ堆 {:+.2f} MB{}",
            name,
            item.deltaRssMB,
            item.deltaPssMB,
            item.deltaHeapInUseMB,
            ok ? "" : " (加载失败)"
        );
        collectLogicalGlobal(r);
        fillResourceMemDetail(r, 0, true, false);
        filterNoisyModules(r.modules);
        reporter.addResource(r);
        printResourceResult(r);
    }

    // 全部卸载: 观察动态库与堆是否归还
    // 用异步卸载 (等 stop 事务 + inflight 归零 + dlclose), 同步 shutdownAll 在
    // stop 未完成时只会标记 CloseFailed 并保留实例与动态库, 测不到真实回收
    bool unloadOk = false;
    {
        std::promise<bool> p;
        asio::co_spawn(
            *agent->ioCtx,
            [&]() -> asio::awaitable<void> {
                bool ok = false;
                if (agent->agentContext && agent->agentContext->pluginManager) {
                    ok = co_await agent->agentContext->pluginManager->shutdownAsync(
                        std::chrono::seconds{20}
                    );
                }
                p.set_value(ok);
                co_return;
            },
            asio::detached
        );
        unloadOk = p.get_future().get();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const auto unloaded = tracker.mark(
        "unloaded",
        unloadOk ? "全部插件卸载 (dlclose 完成)" : "卸载未在超时内完成 (可能有执行中的回调)"
    );

    size_t remainingPlugins = 0;
    {
        std::promise<void> p;
        asio::post(*agent->ioCtx, [&]() {
            if (agent->agentContext && agent->agentContext->pluginManager) {
                remainingPlugins = agent->agentContext->pluginManager->list().size();
            }
            p.set_value();
        });
        p.get_future().wait();
    }

    ResourceResult resUnload;
    resUnload.mode         = "plugin_attrib";
    resUnload.side         = "self";
    resUnload.point        = "unloaded";
    resUnload.pluginsAgent = remainingPlugins;
    resUnload.note         = fmt::format(
        "卸载全部插件后: ΔRSS {:+.2f} MB (相对上一个插件); 未卸载完成实例 {} 个",
        unloaded.deltaRssMB,
        remainingPlugins
    );
    collectLogicalGlobal(resUnload);
    fillResourceMemDetail(resUnload, 0, true, true); // 含 malloc_trim 可回收量
    filterNoisyModules(resUnload.modules);
    reporter.addResource(resUnload);
    printResourceResult(resUnload);

    tracker.printTable("plugin_attrib 分阶段内存");
    reporter.attachPhases("plugin_attrib", "self", tracker.samples());

    agentWork.reset();
    agent->ioCtx->stop();
    if (agentThread.joinable()) {
        agentThread.join();
    }
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
}

// ===========================================================================
// 场景 5: 真实 TUI 长上下文 —— 真实流式回复 + 逐字渲染 + 超限后的自动压缩
//
// 与其它场景的区别:
// - 不用"注入固定历史", 而是**逐轮真实对话**: 每轮发一条用户消息, mock LLM 以
//   真实流式返回 (每个 SSE 帧单独写出、帧间停顿), TUI 收到 delta 就重绘;
// - 累计内容 token (所有用户消息 + 回复) 达到采样点时才采样: 100K/200K 在上限内,
//   1M/2M 已经远远超过模型上下文上限, 看到的稳态是"自动压缩之后"的样子;
// - 开会话库 (与真实使用一致), 因此展示历史按窗口淘汰;
// - 走主程序路径 (AgentHost) 建根 agent: 上下文压缩要经 `service.subagent` 起
//   压缩子代理, 没有主程序时只会退化成硬截断, 量不到真实压缩的内存行为。
// ===========================================================================

namespace {

/// 流式 mock LLM: 每个 SSE 帧单独写成 chunk, 帧之间停顿, 模拟真实供应商的逐字输出
struct StreamingLlmSim {
    std::unique_ptr<utilxx::HttpServer>  svr;
    std::thread                          thr;
    uint16_t                             port         = 0;
    std::shared_ptr<std::atomic<size_t>> turnCount    = std::make_shared<std::atomic<size_t>>(0);
    std::shared_ptr<std::atomic<size_t>> compactCount = std::make_shared<std::atomic<size_t>>(0);

    StreamingLlmSim() = default;

    StreamingLlmSim(StreamingLlmSim&& o) noexcept :
        svr(std::move(o.svr)),
        thr(std::move(o.thr)),
        port(o.port),
        turnCount(std::move(o.turnCount)),
        compactCount(std::move(o.compactCount)) {
        o.port = 0;
    }

    StreamingLlmSim& operator=(StreamingLlmSim&& o) noexcept {
        if (this != &o) {
            stop();
            svr          = std::move(o.svr);
            thr          = std::move(o.thr);
            port         = o.port;
            turnCount    = std::move(o.turnCount);
            compactCount = std::move(o.compactCount);
            o.port       = 0;
        }
        return *this;
    }

    StreamingLlmSim(const StreamingLlmSim&)            = delete;
    StreamingLlmSim& operator=(const StreamingLlmSim&) = delete;

    ~StreamingLlmSim() {
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

/// 起流式 mock LLM
/// - `replyBytes`: 普通轮次回复正文长度 (按完整句子截断到该长度)
/// - `streamChunks`: 回复切成几个 SSE 帧 (每帧一个 data 事件)
/// - `chunkDelayMs`: 帧间停顿 (毫秒, 0 = 连续写)
inline StreamingLlmSim
    startStreamingLlmSim(size_t replyBytes, size_t streamChunks, int chunkDelayMs) {
    StreamingLlmSim sim;

    utilxx::HttpServer::Config cfg;
    cfg.address          = "127.0.0.1";
    cfg.port             = 0;
    cfg.ioThreads        = 1;
    cfg.accessLogEnabled = false;
    cfg.maxConnections   = 32;
    // 300K token 上下文的请求体约 1 MiB; 留足余量避免 mock 侧提前拒绝
    cfg.maxRequestBody          = 32 * 1024 * 1024;
    cfg.sseWriteTimeout         = std::chrono::seconds(120);

    sim.svr      = std::make_unique<utilxx::HttpServer>(cfg);
    auto* rawSvr = sim.svr.get();
    auto  turns  = sim.turnCount;
    auto  comps  = sim.compactCount;

    // 回复正文与压缩摘要 (压缩摘要要短, 模拟真实摘要的体积)
    std::string longReply;
    {
        static constexpr std::string_view kUnit
            = "The assistant keeps the reply text stable so every turn costs the same. ";
        longReply.reserve(replyBytes + kUnit.size());
        while (longReply.size() < replyBytes) {
            longReply += kUnit;
        }
        longReply.resize(replyBytes);
    }
    const std::string summaryReply
        = "[Previous conversation]: 摘要: 早前若干探针轮次已完成, 关键结论与待办都保留在这里, "
          "可以继续后面的工作。";

    auto handler =
        [longReply, summaryReply, streamChunks, chunkDelayMs, turns, comps](
            utilxx::HttpServer::Request&                   req,
            std::shared_ptr<utilxx::HttpServer::SseWriter> writer
        ) -> asio::awaitable<void> {
        namespace http = boost::beast::http;

        const std::string_view body = req.body();
        // 压缩请求靠压缩指令识别, 回复只要一段摘要
        // - 只能认模板首句: 内置系统提示词里就写着 `[Please describe previous
        //   conversation]` ("Context Markers" 说明段), 用它判定会把每个普通请求都当成压缩
        // - `[Previous conversation]:` 同理: 压缩后的会话每轮都带着这段摘要
        const bool isCompact
            = body.find("The conversation above will be compacted to free context space")
              != std::string_view::npos;
        const bool isStream
            = body.find("\"stream\":true") != std::string_view::npos
              || body.find("\"stream\": true") != std::string_view::npos;
        // 请求体字节 / 4 ≈ prompt token, 与基准其它场景同一套量级估算
        const int64_t promptTokens = static_cast<int64_t>(body.size() / 4);

        const std::string& reply = isCompact ? summaryReply : longReply;
        const int64_t      completionTokens = static_cast<int64_t>(reply.size() / 4);

        if (!isStream) {
            // 非流式: 直接回完整 JSON (带 usage), 走 SseWriter 的一次性响应
            utilxx_base::Json msg;
            msg["role"]    = "assistant";
            msg["content"] = reply;
            utilxx_base::Json choice;
            choice["index"]         = 0;
            choice["message"]       = msg;
            choice["finish_reason"] = "stop";
            utilxx_base::Json doc;
            doc["id"]      = "chatcmpl-tui-longctx";
            doc["object"]  = "chat.completion";
            doc["created"] = 1750000000;
            doc["model"]   = "bench-tui-longctx";
            doc["choices"] = utilxx_base::Json::array({choice});
            doc["usage"]   = {
                {"prompt_tokens",     promptTokens                },
                {"completion_tokens", completionTokens            },
                {"total_tokens",      promptTokens + completionTokens}
            };
            (isCompact ? comps : turns)->fetch_add(1);
            co_await writer->writeResponse(http::status::ok, "application/json", doc.dump());
            co_return;
        }

        (isCompact ? comps : turns)->fetch_add(1);

        auto chunkFrame = [](const utilxx_base::Json& delta, std::string_view finishReason) {
            utilxx_base::Json ev;
            ev["id"]      = "chatcmpl-tui-longctx";
            ev["object"]  = "chat.completion.chunk";
            ev["created"] = 1750000000;
            ev["model"]   = "bench-tui-longctx";
            utilxx_base::Json choice;
            choice["index"] = 0;
            choice["delta"] = delta;
            if (finishReason.empty()) {
                choice["finish_reason"] = nullptr;
            } else {
                choice["finish_reason"] = std::string{finishReason};
            }
            ev["choices"] = utilxx_base::Json::array({choice});
            return fmt::format("data: {}\n\n", ev.dump());
        };

        // 1) role 帧
        // 注意: writeChunk 在第一个帧里会先写响应头再 await, 临时串会悬空,
        // 因此每一帧都先落到具名局部变量再交出去。
        {
            utilxx_base::Json d;
            d["role"]                = "assistant";
            const std::string frame = chunkFrame(d, "");
            if (!co_await writer->writeChunk(frame)) {
                co_return;
            }
        }

        // 2) 正文按 streamChunks 切片逐帧推送 (帧间停顿, 与真实流式的间隔同量级)
        //    切点必须落在 utf8 字符边界上: 摘要等文本含多字节字符, 按字节随便切会产生非法
        //    utf8 分片 (Json::dump 直接报错, 整个请求失败 → 主程序重试)
        const size_t chunks = streamChunks == 0 ? 1 : streamChunks;
        const size_t step   = reply.size() / chunks;
        size_t       pos    = 0;
        for (size_t i = 0; i < chunks; ++i) {
            size_t len = (i + 1 == chunks) ? (reply.size() - pos) : step;
            if (len == 0) {
                continue;
            }
            // 切点落在续字节 (0b10xxxxxx) 上时往后挪到下一个字符开头
            while (pos + len < reply.size()
                   && (static_cast<unsigned char>(reply[pos + len]) & 0xC0) == 0x80) {
                ++len;
            }
            utilxx_base::Json d;
            d["content"]             = reply.substr(pos, len);
            pos                     += len;
            const std::string frame = chunkFrame(d, "");
            if (!co_await writer->writeChunk(frame)) {
                co_return;
            }
            if (chunkDelayMs > 0) {
                auto                      ex = co_await asio::this_coro::executor;
                asio::steady_timer        timer(ex);
                timer.expires_after(std::chrono::milliseconds(chunkDelayMs));
                utilxx_base::AsioErrorCode ec;
                co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            }
        }

        // 3) finish 帧 + usage 帧 (OpenAI 在 stream_options.include_usage 时给最后一帧)
        {
            const std::string frame = chunkFrame(utilxx_base::Json::object(), "stop");
            if (!co_await writer->writeChunk(frame)) {
                co_return;
            }
        }
        {
            utilxx_base::Json ev;
            ev["id"]      = "chatcmpl-tui-longctx";
            ev["object"]  = "chat.completion.chunk";
            ev["created"] = 1750000000;
            ev["model"]   = "bench-tui-longctx";
            ev["choices"] = utilxx_base::Json::array();
            ev["usage"]   = {
                {"prompt_tokens",     promptTokens                },
                {"completion_tokens", completionTokens            },
                {"total_tokens",      promptTokens + completionTokens}
            };
            const std::string frame = fmt::format("data: {}\n\n", ev.dump());
            if (!co_await writer->writeChunk(frame)) {
                co_return;
            }
        }
        co_await writer->writeChunk(std::string_view{"data: [DONE]\n\n"});
        co_await writer->close();
        co_return;
    };

    rawSvr->addSsePostRoute("/v1/chat/completions", handler);
    rawSvr->addSsePostRoute("/chat/completions", handler);

    sim.thr = std::thread([rawSvr]() {
        rawSvr->start();
    });
    for (int i = 0; i < 200; ++i) {
        sim.port = rawSvr->port();
        if (sim.port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return sim;
}

/// 解析逗号分隔的采样点, 单位是**累计内容字节** (空项忽略)
/// - 支持 `K` / `M` / `G` 后缀 (1024 进制, 大小写不敏感): `20M` = 20 MiB
/// - 纯数字按字节解释
inline std::vector<size_t> parseSizePoints(std::string_view text) {
    std::vector<size_t> out;
    size_t              start = 0;
    while (start <= text.size()) {
        size_t comma = text.find(',', start);
        if (comma == std::string_view::npos) {
            comma = text.size();
        }
        std::string item{text.substr(start, comma - start)};
        // 去掉空白
        item.erase(std::remove_if(item.begin(), item.end(), [](unsigned char c) {
                       return std::isspace(c) != 0;
                   }),
                   item.end());
        if (!item.empty()) {
            size_t scale = 1;
            switch (item.back()) {
                case 'K':
                case 'k':
                    scale = 1024ull;
                    break;
                case 'M':
                case 'm':
                    scale = 1024ull * 1024ull;
                    break;
                case 'G':
                case 'g':
                    scale = 1024ull * 1024ull * 1024ull;
                    break;
                default:
                    break;
            }
            if (scale != 1) {
                item.pop_back();
            }
            out.push_back(static_cast<size_t>(std::strtoull(item.c_str(), nullptr, 10)) * scale);
        }
        if (comma == text.size()) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

/// 采样点名字: 20971520 -> ctx20mb, 2147483648 -> ctx2gb
/// - 一律带 `b` 后缀: 本场景的量是**字节**, 与旧批次按 token 命名的 `ctx2m` 区分开
inline std::string sizePointName(size_t bytes) {
    constexpr size_t kKib = 1024ull;
    constexpr size_t kMib = 1024ull * 1024ull;
    constexpr size_t kGib = 1024ull * 1024ull * 1024ull;
    if (bytes != 0 && bytes % kGib == 0) {
        return fmt::format("ctx{}gb", bytes / kGib);
    }
    if (bytes != 0 && bytes % kMib == 0) {
        return fmt::format("ctx{}mb", bytes / kMib);
    }
    if (bytes != 0 && bytes % kKib == 0) {
        return fmt::format("ctx{}kb", bytes / kKib);
    }
    return fmt::format("ctx{}b", bytes);
}

/// 采集逻辑内存 (agent 会话 + TUI 侧 + 会话库磁盘), 与 real_tui 场景同一套统计口径
/// - `dataDir` 非空时额外给出会话库的磁盘字节 (`__store.*` 行不计入逻辑内存合计):
///   GB 级累计内容的场景里, 它就是"被转存到磁盘"的那一份
inline void collectLogicalForTuiLongCtx(
    ResourceResult&                                          res,
    const std::shared_ptr<agent::CodeAgent>&                 agent,
    const std::shared_ptr<agentxx::client::TUIClientAgentIO>& tui,
    const std::string&                                       sessionId,
    const std::string&                                       dataDir
) {
    std::promise<void> p;
    asio::post(*agent->ioCtx, [&]() {
        res.logical = collectAgentLogicalMem(
            agent->agentContext,
            AgentLogicalOptions{true, {sessionId}}
        );
        p.set_value();
    });
    p.get_future().wait();
    if (auto snap = tui->sharedState().readSnapshot()) {
        auto rows = collectTuiLogicalMem(*snap);
        res.logical.insert(res.logical.end(), rows.begin(), rows.end());
    }
    if (!dataDir.empty()) {
        auto storeRows = collectStoreLogicalMem(dataDir, sessionId);
        res.logical.insert(res.logical.end(), storeRows.begin(), storeRows.end());
    }
}

} // namespace

/// 场景 5: 真实 TUI 长上下文 (真实流式回复 + 逐字渲染 + 超限自动压缩后的稳态)
///
/// 装配与 `real_tui` 一致 (AgentHost + 插件 + Channel + 真实 FTXUI + 会话库),
/// 区别是逐轮真实对话: 每轮发若干条 `msgBytes` 用户消息, mock LLM 把回复切成多帧流式
/// 吐回, TUI 逐帧渲染; 累计内容字节到采样点就采一次内存/渲染/上下文数据。
///
/// **累计内容怎么来到 GB 级** (两个模式):
/// - `history` (默认): 先用真实轮次累积到 `REAL_BYTES`, 之后每个采样点把差额通过
///   展示历史的写入入口 (`Session::appendViewMessage`, 与真实轮次同一条落库路径)
///   按条注入, 再跑一轮真实对话后采样。回答的是"会话历史 N GB 时常驻内存多少"。
/// - `turns`: 全部用真实轮次累积 (不注入历史)。真实轮次有约 140 ms/轮的地板
///   (见 benchmark.md 第 19 节), 单条 8 KB 到 2 GB 需要 26 万轮 (几小时), 所以只适合
///   小型采样点; 它另外用来核对"逐轮累积"与"历史注入"两条路径的内存差异。
///
/// - 环境变量 (均可选, 括号内为默认值):
///     - `AGENTXX_TUI_CTX_POINTS` (20M,100M,200M,1G,2G) 采样点: **累计内容字节**
///       (支持 K/M/G 后缀, 1024 进制); 累计量 = 用户消息字节 + 回复字节 + 注入历史字节
///     - `AGENTXX_TUI_CTX_MODE` (history) `history` = 超出真实轮次预算的部分注入历史;
///       `turns` = 全部真实轮次
///     - `AGENTXX_TUI_CTX_REAL_BYTES` (2M) `history` 模式下用真实轮次累积多少内容
///     - `AGENTXX_TUI_CTX_RESYNC` (1) 注入历史后让客户端重新尾窗同步一次
///       (使客户端持有的是"最近 100 条", 而不是注入前那批)
///     - `AGENTXX_TUI_CTX_MSG_BYTES` (8192) 单条用户消息字节数 (也决定注入历史单条字节数)
///     - `AGENTXX_TUI_CTX_INPUTS_PER_TURN` (1) 一轮注入多少条消息
///     - `AGENTXX_TUI_CTX_REPLY_BYTES` (600) mock 回复正文字节数
///     - `AGENTXX_TUI_CTX_MODEL_MAX` (300000) 模型上下文上限 (达 75% 触发自动压缩)
///     - `AGENTXX_TUI_CTX_STREAM_CHUNKS` (48) 回复切成多少个 SSE 帧
///     - `AGENTXX_TUI_CTX_CHUNK_DELAY_MS` (1) 帧间停顿 (毫秒)
///     - `AGENTXX_TUI_CTX_PLUGINS` (1) 是否加载 5 个基准插件
///     - `AGENTXX_TUI_CTX_SESSION_STORE` (1) 是否启用会话库 (关掉后展示历史不淘汰)
///     - `AGENTXX_TUI_CTX_MAX_ROUNDS` (20000) 驱动批次上限 (一批 = 一轮)
///     - `AGENTXX_TUI_CTX_ROUND_TIMEOUT_MS` (180000) 单批等待上限
void benchResourceTuiLongCtx() {
#ifndef AGENTXX_BUILD_CLIENT
    std::cout << "  [resource][tui_longctx] skipped: AGENTXX_BUILD_CLIENT not enabled"
              << std::endl;
    return;
#else
    std::cout << "\n=== Resource Benchmark: 真实 TUI 长上下文 (流式回复 + 自动压缩) ==="
              << std::endl;

    auto envSize = [](std::string_view name, size_t def) -> size_t {
        if (auto v = utilxx_base::ApplicationEnv::instance().get(name)) {
            return static_cast<size_t>(std::strtoull(v->c_str(), nullptr, 10));
        }
        return def;
    };
    auto envText = [](std::string_view name, std::string_view def) -> std::string {
        if (auto v = utilxx_base::ApplicationEnv::instance().get(name)) {
            return *v;
        }
        return std::string{def};
    };

    const auto  points    = parseSizePoints(
        envText("AGENTXX_TUI_CTX_POINTS", "20M,100M,200M,1G,2G")
    );
    const std::string mode = envText("AGENTXX_TUI_CTX_MODE", "history");
    const size_t msgBytes  = envSize("AGENTXX_TUI_CTX_MSG_BYTES", 8192);
    const size_t inputsPerTurn = std::max<size_t>(1, envSize("AGENTXX_TUI_CTX_INPUTS_PER_TURN", 1));
    const size_t realBytes = (mode == "turns")
                                 ? std::numeric_limits<size_t>::max()
                                 : envSize("AGENTXX_TUI_CTX_REAL_BYTES", 2 * 1024 * 1024);
    const bool   resyncAfterSeed = envSize("AGENTXX_TUI_CTX_RESYNC", 1) != 0;
    const size_t modelMax  = envSize("AGENTXX_TUI_CTX_MODEL_MAX", 300000);
    const size_t chunks    = envSize("AGENTXX_TUI_CTX_STREAM_CHUNKS", 48);
    const int    chunkMs   = static_cast<int>(envSize("AGENTXX_TUI_CTX_CHUNK_DELAY_MS", 1));
    const size_t replyBytes = envSize("AGENTXX_TUI_CTX_REPLY_BYTES", 600);
    const bool   usePlugins = envSize("AGENTXX_TUI_CTX_PLUGINS", 1) != 0;
    const bool   useStore   = envSize("AGENTXX_TUI_CTX_SESSION_STORE", 1) != 0;
    const size_t maxRounds  = envSize("AGENTXX_TUI_CTX_MAX_ROUNDS", 20000);
    const int    roundTimeoutMs = static_cast<int>(envSize("AGENTXX_TUI_CTX_ROUND_TIMEOUT_MS", 180000));

    if (points.empty()) {
        std::cout << "  [resource][tui_longctx] 采样点为空, 跳过" << std::endl;
        return;
    }
    if (msgBytes == 0) {
        std::cout << "  [resource][tui_longctx] 单条消息字节数为 0, 跳过" << std::endl;
        return;
    }

    auto  sim      = startStreamingLlmSim(replyBytes, chunks, chunkMs);
    auto  tmpDir   = createBenchTempDir("bench_tui_longctx");
    auto& reporter = BenchReporter::instance();

    MemPhaseTracker phaseTracker(0, "self");
    // 阶段常驻分解: 看清每个阶段新增的 RSS 是代码页 (exe/系统 DLL/插件) 还是堆
    auto dumpPhaseBreakdown = [](std::ostream& os, const std::string& label) {
        auto bd = sampleModuleBreakdown(0, 0);
        if (!bd.valid) {
            os << fmt::format("  [tui_longctx] {} 分解不可用: {}\n", label, bd.note);
            return;
        }
        printKindSummary(bd, "阶段 " + label, os);
    };
    phaseTracker.mark("process_base", "仅流式 mock LLM 服务; agent/TUI 未构建");
    dumpPhaseBreakdown(std::cout, "process_base");

    auto agentConfig                = std::make_shared<agent::AgentConfig>();
    agentConfig->dataDir            = (tmpDir / "data").string();
    agentConfig->workDir            = tmpDir.string();
    agentConfig->permissionMode     = agent::PermissionMode::Pass;
    agentConfig->enableSessionStore = useStore;
    agentConfig->enableSubagent     = false; // 只控制工具注入; 压缩委派由 AgentHost 提供
    agentConfig->enableWorktree     = false;
    // 关闭单条输入超长卸载 (默认 8K 字符): 本场景要的就是每轮正文完整进上下文,
    // 否则被卸载后上下文只留开头片段 + `agentxx_share_store` 提示, token 数对不上
    agentConfig->userInputSummaryLimitLength = 0;

    agent::ModelConfig mc;
    mc.name                 = "bench-sim";
    mc.type                 = "openai";
    mc.baseUrl              = fmt::format("http://127.0.0.1:{}/v1", sim.port);
    mc.apiKey               = "EMPTY";
    mc.modelName            = "bench-sim";
    mc.modelContextMaxToken = modelMax;
    agentConfig->model                    = mc;
    agentConfig->availableModels[mc.name] = mc;
    agentConfig->currentModelName         = mc.name;
    if (usePlugins) {
        agentConfig->plugins = bench5PluginConfigs();
    }

    auto        agent     = std::make_shared<agent::CodeAgent>(agentConfig);
    auto        agentWork = asio::make_work_guard(*agent->ioCtx);
    std::thread agentThread([agent]() {
        agent->ioCtx->run();
    });
    phaseTracker.mark("agent_constructed", "CodeAgent + io_context 线程已创建");
    dumpPhaseBreakdown(std::cout, "agent_constructed");

    // 主程序容器: 插件只加载一次, 压缩经它起子代理 (没有它只能硬截断)
    auto host = agent::AgentHost::create({
        .ioCtx        = agent->ioCtx,
        .pluginList   = agentConfig->plugins,
        .primaryAgent = agent->agentContext,
    });

    asio::io_context clientCtx;
    auto        clientWork = asio::make_work_guard(clientCtx);
    auto        clientEx   = clientCtx.get_executor();
    std::string sessionId  = generateBenchSessionId();

    auto tui = std::make_shared<agentxx::client::TUIClientAgentIO>(
        clientEx,
        sessionId,
        agentxx::client::TUITheme::darkTheme()
    );
    tui->setDataDir((tmpDir / "data").string());
    tui->setWorkDir(tmpDir.string());

    auto pluginMgr = std::make_shared<agentxx::plugin::ClientPluginManager>(clientEx);
    pluginMgr->setUiAdapter(std::make_shared<agentxx::client::TuiPluginAdapter>(tui));
    pluginMgr->setSessionId(sessionId);
    tui->setPluginManager(pluginMgr);
    tui->setEventSink(pluginMgr);

    // TUI 绘制到 fd 1: 本次只看内存/帧数, 输出直接丢弃 (否则长跑会写出 GB 级转义序列)
    std::ostringstream console;
    StdoutCapture      capture(false, "");
    tui->start();
    bool firstFrameOk = waitTuiFrames(tui, 1, 5000);
    phaseTracker.mark(
        "tui_started",
        fmt::format("FTXUI 界面线程启动, 首帧{}", firstFrameOk ? "已渲染" : "超时")
    );
    dumpPhaseBreakdown(console, "tui_started");

    std::atomic<bool> pluginsLoaded{false};
    asio::co_spawn(
        clientEx,
        [&]() -> asio::awaitable<void> {
            co_await pluginMgr->loadConfiguredClientPlugins(agentConfig->plugins);
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
    auto countingClient = std::make_shared<CountingTransport>(
        std::shared_ptr<agent::AgentIOTransportBase>(std::move(clientTransport))
    );
    auto countingServer = std::make_shared<CountingTransport>(
        std::shared_ptr<agent::AgentIOTransportBase>(std::move(serverTransport))
    );
    tui->setTransport(countingClient);

    agent::SessionServerAgentIO::Config scCfg;
    scCfg.sessionId            = sessionId;
    scCfg.initialSyncTailCount = 100;
    auto serverIO = std::make_shared<agent::SessionServerAgentIO>(agentEx, agent, scCfg);
    serverIO->setTransport(countingServer);

    std::atomic<bool> serverReady{false};
    if (auto ctx = agent->agentContext) {
        ctx->initNotifier = [tui](std::string_view step) {
            tui->onServerProgress(step);
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
            co_await host->loadPluginsAsync();
            if (auto ctx = agent->agentContext; ctx && host->pluginHost()) {
                ctx->pluginManager      = host->pluginHost();
                ctx->pluginManagerOwned = false; // 进程共享管理器
            }
            co_await agent->init();
            host->attachRoot(agent);
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

    std::thread clientThread([&clientCtx]() {
        clientCtx.run();
    });
    while (!serverReady.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    phaseTracker.mark(
        "agent_init_done",
        fmt::format("AgentHost + agent init + {} 插件挂载完成", usePlugins ? 5 : 0)
    );
    dumpPhaseBreakdown(console, "agent_init_done");

    tui->sendToPeer(agent::WireHello{sessionId, "", 0, "", "en"});
    tui->sendToPeer(agent::WireGetModel{sessionId});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ---- 会话状态读取 (必须在 agent io 线程) ----
    // 轻量快照: 轮次轮询每几十毫秒取一次, **不得**生成消息 JSON
    // (100K+ token 上下文的 JSON dump 每轮几百 KB, 随轮询频率连续分配会拖慢本轮并推高内存)
    struct SessionSnap {
        size_t    liveContextTokens = 0;
        size_t    maxContextTokens  = 0;
        size_t    viewWindowCount   = 0;
        size_t    viewTotalCount    = 0;
        int64_t   usageTotal        = 0;
        uint64_t  turnsStarted      = 0;
        /// 轮次终态总数 (完成/失败/取消/中断四种之和)
        uint64_t  turnsEnded        = 0;
        /// 服务端消息队列条数 (next-turn 投递排队中, 还没开成轮)
        size_t    queueSize         = 0;
        /// 待注入输入条数 (next-step 已受理, 还没进入上下文)
        size_t    pendingInputs     = 0;
        /// 端点认为还有轮次在跑 (含队列里有条目)
        bool      turnActive        = false;
        std::string turnEndDetail;
        bool      idle              = true;
        // 以下只在采样点填 (见 fillDetail)
        size_t    viewWindowBytes = 0;
        size_t    llmCount        = 0;
        size_t    llmBytes        = 0;
    };
    auto readSession = [&]() -> SessionSnap {
        SessionSnap        snap;
        std::promise<void> p;
        asio::post(*agent->ioCtx, [&]() {
            if (auto sess = agent->agentContext->getSession(sessionId)) {
                snap.liveContextTokens = sess->contextStats->contextTokens;
                snap.maxContextTokens  = sess->contextStats->maxContextTokens;
                snap.viewWindowCount   = sess->viewWindow().size();
                snap.viewTotalCount    = sess->viewMessageCount();
                snap.usageTotal        = sess->contextStats->usage.totalTokens;
                snap.pendingInputs     = sess->pendingInputCount();
                snap.idle              = (sess->activity == agent::SessionActivity::Idle);
            }
            // 队列/轮次状态在 io 线程上读 (messageQueue_ 只允许 io 线程访问)
            snap.queueSize  = serverIO->queueSizeForTest();
            snap.turnActive = serverIO->turnActive();
            if (auto m = agent->agentContext->metrics) {
                snap.turnsStarted   = m->turns();
                snap.turnsEnded     = m->turnsCompleted() + m->turnsFailed() + m->turnsCanceled()
                                  + m->turnsInterrupted();
                snap.turnEndDetail  = fmt::format(
                    "完成 {} / 失败 {} / 取消 {} / 中断 {}",
                    m->turnsCompleted(),
                    m->turnsFailed(),
                    m->turnsCanceled(),
                    m->turnsInterrupted()
                );
            }
            p.set_value();
        });
        p.get_future().wait();
        return snap;
    };
    // 采样点才需要的内容类尺寸 (会生成消息 JSON, 每次调用只做一次)
    auto fillSessionDetail = [&](SessionSnap& snap) {
        std::promise<void> p;
        asio::post(*agent->ioCtx, [&]() {
            if (auto sess = agent->agentContext->getSession(sessionId)) {
                snap.viewWindowBytes = estimateViewMessagesBytes(sess->viewWindow());
                snap.llmCount        = sess->llmMessagesJson().size();
                snap.llmBytes        = estimateLlmMessagesBytes(sess->llmMessagesJson());
            }
            p.set_value();
        });
        p.get_future().wait();
    };

    /// 等一轮真正开起来 (第 1 条输入已被受理, 后面的才能被并入本轮)
    /// - 判据用端点的 turnActive (含"消息队列非空"): 第一条入队后即为真
    auto waitTurnActive = [&](int timeoutMs) -> bool {
        const auto deadline
            = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (readSession().turnActive) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };

    /// 等一批消息全部处理完、会话完全静默
    /// - 判据: 轮次终态计数增长 + 消息队列空 + 待注入输入空 + 端点不认为还有轮次在跑,
    ///   然后再睡 10 ms 复查一次 (防止刚好落在两轮之间, 后面还有排队的轮次没开起来)
    /// - 不能用"模型用量增长"判断: 压缩子代理的用量也计入本会话, 它会在本轮正文还没
    ///   叫模型时就先增长, 于是驱动方提前发下一条, 输入被排到下轮
    /// - 也不能只看 idle: 本轮刚开始时用户消息已入队但还没进入流式, 同样是假空闲
    /// - 四种终态都要算 (不能只看"完成"): 压缩等路径上本轮可能以中断结束
    auto waitBurstIdle = [&](const SessionSnap& prev, int timeoutMs) -> bool {
        auto settled = [&](const SessionSnap& s) {
            return s.queueSize == 0 && s.pendingInputs == 0 && !s.turnActive;
        };
        const auto deadline
            = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            auto snap = readSession();
            if (snap.idle && snap.turnsEnded > prev.turnsEnded && settled(snap)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                auto again = readSession();
                if (again.idle && again.turnsEnded >= snap.turnsEnded && settled(again)) {
                    return true;
                }
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        auto snap = readSession();
        std::cerr << fmt::format(
            "  [tui_longctx] 等待轮次终态超时 ({} ms): 轮次={} 终态={} ({}) idle={} 队列={} "
            "待注入={} 上下文={}\n",
            timeoutMs,
            snap.turnsStarted,
            snap.turnsEnded,
            snap.turnEndDetail,
            snap.idle ? 1 : 0,
            snap.queueSize,
            snap.pendingInputs,
            snap.liveContextTokens
        );
        return false;
    };

    /// 发送一条 `msgBytes` 用户消息 (首条走默认投递, 其余并入本轮)
    auto sendProbe = [&](size_t index, bool steer) {
        std::string text = fmt::format("CTX-PROBE-{:08d}: ", index + 1);
        if (text.size() < msgBytes) {
            text.append(msgBytes - text.size(), 'x');
        }
        agent::WireUserInput input{
            .sessionId = sessionId,
            .text      = std::move(text),
        };
        if (steer) {
            input.delivery = std::string{agent::InputDelivery::NextStep};
        }
        tui->sendToPeer(std::move(input));
    };

    /// 发一批: 第 1 条开新一轮, 等它真的开起来后, 其余按 next-step 并入本轮
    /// - `return` 实际发出的条数 (早退时为已发数)
    auto sendBurst = [&](size_t index, size_t count) -> size_t {
        size_t sent = 0;
        for (size_t i = 0; i < count; ++i) {
            sendProbe(index + i, i > 0);
            ++sent;
            if (i == 0) {
                waitTurnActive(2000);
            }
        }
        return sent;
    };

    /// 已注入的展示历史条数 (种子文本序号用)
    size_t histRows = 0;

    /// 注入展示历史: 按条写入会话的展示历史 (与真实轮次同一条写入/落库路径)
    /// - 用途: 把"会话历史"做大到 GB 级。真实轮次有时间地板 (约 140 ms/轮),
    ///   单条 8 KB 到 2 GB 要几个小时, 不具可行性 (见 benchmark.md 第 19 节)
    /// - 分批投递到 io 线程 (单批 [kSeedChunk] 条), 每批结束 flush 一次:
    ///   ① 待落盘队列不积压 (窗口只在队列空时才真正淘汰, 否则内容全堆在内存里)
    ///   ② io 线程不会被长时间占住
    /// - `return` 实际写入条数
    auto seedHistory = [&](size_t bytes) -> size_t {
        constexpr size_t     kSeedChunk = 1024;
        const size_t         rows       = bytes / msgBytes;
        size_t               done       = 0;
        while (done < rows) {
            const size_t         chunk = std::min(kSeedChunk, rows - done);
            std::promise<size_t> p;
            asio::post(*agent->ioCtx, [&, chunk]() {
                size_t appended = 0;
                if (auto sess = agent->agentContext->getSession(sessionId)) {
                    const auto nowMs = static_cast<int64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()
                        )
                            .count()
                    );
                    for (size_t i = 0; i < chunk; ++i) {
                        std::string text = fmt::format("HIST-{:09d}: ", histRows + i + 1);
                        if (text.size() < msgBytes) {
                            text.append(msgBytes - text.size(), 'z');
                        }
                        sess->appendViewMessage(agent::ViewMessage::makeText(
                            agent::ViewMessage::Role::User,
                            std::move(text),
                            nowMs
                        ));
                        ++appended;
                    }
                    // 落盘 + 让窗口淘汰跟上来 (淘汰只在待落盘队列为空时真正删除)
                    sess->flushViewMessages();
                }
                p.set_value(appended);
            });
            const size_t appended = p.get_future().get();
            if (appended == 0) {
                break;
            }
            done     += appended;
            histRows += appended;
        }
        return done;
    };

    // ---- 预热轮 (真实走流式) ----
    {
        auto before = readSession();
        tui->sendToPeer(agent::WireUserInput{sessionId, "hello"});
        if (!waitBurstIdle(before, roundTimeoutMs)) {
            console << "  [tui_longctx] 预热轮超时, 继续\n";
        }
    }
    phaseTracker.mark("warmup_turn_done", "首轮流式对话完成");
    dumpPhaseBreakdown(console, "warmup_turn_done");

    // ---- 每轮的 token 估算 (用户消息 + 回复, 用主程序同一份估算器) ----
    size_t userTokensPerRound  = 0;
    size_t replyTokensPerRound = 0;
    {
        std::string probe = fmt::format("CTX-PROBE-000001: ");
        if (probe.size() < msgBytes) {
            probe.append(msgBytes - probe.size(), 'x');
        }
        neograph::ChatMessage um;
        um.role    = "user";
        um.content = probe;
        userTokensPerRound = countLlmTokens({um});

        neograph::ChatMessage am;
        am.role    = "assistant";
        am.content = std::string(replyBytes, 'y');
        replyTokensPerRound = countLlmTokens({am});
    }
    // 每批 (一轮) 进入内容 = 一批用户消息字节 + 一次回复字节; 回复很小 (600 B), 影响 < 1%
    const size_t bytesPerBurst = inputsPerTurn * msgBytes + replyBytes;

    /// 采样点名字用的可读尺寸 (G/M/K, 1024 进制)
    auto fmtSize = [](size_t bytes) -> std::string {
        constexpr double kKiB = 1024.0;
        const double     v    = static_cast<double>(bytes);
        if (v >= kKiB * kKiB * kKiB) {
            return fmt::format("{:.2f}GB", v / (kKiB * kKiB * kKiB));
        }
        if (v >= kKiB * kKiB) {
            return fmt::format("{:.1f}MB", v / (kKiB * kKiB));
        }
        if (v >= kKiB) {
            return fmt::format("{:.1f}KB", v / kKiB);
        }
        return fmt::format("{}B", bytes);
    };

    console << fmt::format(
        "  [tui_longctx] 模式 {} (真实轮次预算 {}), 每轮 {} 条 x {} 字节 ({} token/条) + 回复 {} "
        "字节 ({} token) = {} /轮; 模型上限 {} token ({}% 压缩), SSE {} 帧/回复, 帧间隔 {} ms, "
        "会话库 {}\n",
        mode,
        realBytes == std::numeric_limits<size_t>::max() ? std::string{"不限"}
                                                        : fmtSize(realBytes),
        inputsPerTurn,
        msgBytes,
        userTokensPerRound,
        replyBytes,
        replyTokensPerRound,
        fmtSize(bytesPerBurst),
        modelMax,
        75,
        chunks,
        chunkMs,
        useStore ? "开" : "关"
    );

    if (userTokensPerRound == 0) {
        console << "  [tui_longctx] token 估算异常 (0), 结束\n";
        tui->stop();
        capture.restore();
        std::cout << console.str();
        return;
    }

    // ---- 逐轮驱动, 到点采样 ----
    // `round` = 已完成的批次数 (一批 = 一轮), `msgsSent` = 已发送的用户消息条数
    size_t              round           = 0;
    size_t              msgsSent        = 0;
    size_t              cumulative      = 0;
    size_t              nextPoint       = 0;
    CpuWindow           cpuWin          = cpuBegin(0);
    uint64_t            lastFrames      = 0;
    auto                lastFrameStats  = tui->frameStats();
    uint64_t            lastCompactions = 0;
    // 区间统计 (上次采样到本次采样之间): 轮数、活上下文峰值、耗时
    size_t intervalRounds  = 0;
    size_t intervalMaxLive = 0;
    auto   intervalStart   = std::chrono::steady_clock::now();

    // 只读活上下文 token 数 (每轮调一次, 不生成消息 JSON)
    auto readLiveTokens = [&]() -> size_t {
        size_t             value = 0;
        std::promise<void> p;
        asio::post(*agent->ioCtx, [&]() {
            if (auto sess = agent->agentContext->getSession(sessionId)) {
                value = sess->contextStats->contextTokens;
            }
            p.set_value();
        });
        p.get_future().wait();
        return value;
    };

    /// 会话库磁盘字节 (该会话单独一个库目录; 这就是"转存到磁盘"的那一份)
    auto readStoreBytes = [&]() -> size_t {
        for (const auto& row : collectStoreLogicalMem(agentConfig->dataDir, sessionId)) {
            if (row.name == "__store.session_db") {
                return row.bytes;
            }
        }
        return 0;
    };

    /// 每个采样点额外记录的"内存 vs 磁盘"对比项
    struct SampleExtra {
        size_t storeBytes      = 0; ///< 会话库磁盘字节
        size_t memContentBytes = 0; ///< 内存里的内容 (展示窗口 + LLM 上下文 + 客户端消息)
        size_t clientMsgs      = 0;
        size_t clientBytes     = 0;
        size_t liveContext     = 0;
        size_t compactions     = 0;
        size_t bursts          = 0;
        size_t msgsSent        = 0;
        size_t seededBytes     = 0; ///< 其中来自注入展示历史的内容
        size_t realBytes       = 0; ///< 其中来自真实轮次的内容
        int64_t seedMs         = 0; ///< 本次采样前注入历史耗时 (毫秒)
    };

    std::vector<ResourceResult> samples;
    std::vector<SampleExtra>    extras;
    auto                        driveStart  = std::chrono::steady_clock::now();
    size_t                      seededTotal = 0;

    /// 跑一轮真实对话 (一批消息) 并把内容计入累计量
    /// - `return` false = 本轮没跑完 (超时/取消), 调用方应停止驱动
    auto driveOneBurst = [&]() -> bool {
        const auto   before = readSession();
        const size_t sent   = sendBurst(msgsSent, inputsPerTurn);
        if (sent == 0) {
            console << "  [tui_longctx] 一批消息未发出, 停止驱动\n";
            return false;
        }
        msgsSent += sent;
        if (!waitBurstIdle(before, roundTimeoutMs)) {
            console << fmt::format(
                "  [tui_longctx] 第 {} 轮 ({} 条消息) {} ms 未完成, 停止驱动\n",
                round + 1,
                sent,
                roundTimeoutMs
            );
            return false;
        }
        ++round;
        cumulative += sent * msgBytes + replyBytes;
        ++intervalRounds;
        intervalMaxLive = std::max(intervalMaxLive, readLiveTokens());
        // 长跑进度: 直接写 stderr (stdout 交给 console 缓冲, 结束才输出)
        if (round <= 3 || round % 50 == 0) {
            std::cerr << fmt::format(
                "  [tui_longctx] 第 {} 轮完成 ({} 条消息), 累计内容 {}\n",
                round,
                msgsSent,
                fmtSize(cumulative)
            );
        }
        return true;
    };

    while (nextPoint < points.size() && round < maxRounds) {
        const size_t target = points[nextPoint];

        // (1) 真实轮次累积: 到采样点或真实轮次预算为止
        while (cumulative < target && cumulative < realBytes && round < maxRounds) {
            if (!driveOneBurst()) {
                break;
            }
        }
        // (2) 差额注入展示历史 (history 模式)
        int64_t seedMs = 0;
        if (cumulative < target) {
            const auto   seedStart = std::chrono::steady_clock::now();
            const size_t need      = target - cumulative;
            const size_t rows      = seedHistory(need);
            const size_t seeded    = rows * msgBytes;
            seedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - seedStart
            )
                         .count();
            seededTotal += seeded;
            cumulative  += seeded;
            console << fmt::format(
                "  [tui_longctx] 注入展示历史 {} ({} 条, 耗时 {:.1f} s), 累计 {}\n",
                fmtSize(seeded),
                rows,
                static_cast<double>(seedMs) / 1000.0,
                fmtSize(cumulative)
            );
            // 注入后让客户端重新同步一次: 让客户端持有"最近 100 条展示历史"
            // (注入的历史没有经 wire 下发, 不重新同步的话客户端还拿着注入前的窗口)
            if (resyncAfterSeed) {
                tui->sendToPeer(agent::WireHello{sessionId, "", 0, "", "en", 0, 100});
                // 等客户端消息数稳定 (尾部同步 + 本地窗口裁剪完成)
                size_t lastCount = std::numeric_limits<size_t>::max();
                for (int i = 0; i < 200; ++i) {
                    size_t now = 0;
                    if (auto tuiSnap = tui->sharedState().readSnapshot()) {
                        now = tuiSnap->messages().size();
                    }
                    if (now == lastCount) {
                        break;
                    }
                    lastCount = now;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            // 注入后再跑一轮真实对话: 让每个采样点都落在"轮次刚结束"的同一状态,
            // 同时核实注入的历史不影响后续真实轮次
            if (round < maxRounds && !driveOneBurst()) {
                break;
            }
        }
        if (cumulative < target) {
            console << fmt::format(
                "  [tui_longctx] 累计 {} 未达到采样点 {}, 停止驱动\n",
                fmtSize(cumulative),
                fmtSize(target)
            );
            break;
        }

        {
            auto snap = readSession();
            fillSessionDetail(snap);

            SampleExtra extra;
            extra.storeBytes  = readStoreBytes();
            extra.liveContext = snap.liveContextTokens;
            extra.bursts      = round;
            extra.msgsSent    = msgsSent;
            extra.seededBytes = seededTotal;
            extra.realBytes   = cumulative > seededTotal ? cumulative - seededTotal : 0;
            extra.seedMs      = seedMs;
            if (agent->agentContext->metrics) {
                extra.compactions = agent->agentContext->metrics->compactions();
            }
            if (auto tuiSnap = tui->sharedState().readSnapshot()) {
                extra.clientMsgs = tuiSnap->messages().size();
                for (const auto& m : tuiSnap->messages()) {
                    if (m) {
                        extra.clientBytes += estimateViewMessageBytes(*m);
                    }
                }
            }
            extra.memContentBytes
                = snap.viewWindowBytes + snap.llmBytes + extra.clientBytes;

            ResourceResult res;
            res.mode          = "tui_longctx";
            res.side          = "self";
            res.point         = sizePointName(target);
            res.tokens        = cumulative;
            res.pluginsAgent  = agent->agentContext->pluginManager
                                    ? agent->agentContext->pluginManager->list().size()
                                    : 0;
            res.pluginsClient = pluginMgr->list().size();
            res.cpuIdlePct    = -1.0;
            res.cpuBusyPct    = cpuEnd(cpuWin);
            res.cpu           = cpuWin.result;
            cpuWin            = cpuBegin(0);

            const auto fs = tui->frameStats();
            res.frames     = fs.frames - lastFrames;
            res.frameAvgMs = (res.frames > 0)
                                 ? (fs.totalRenderMs - lastFrameStats.totalRenderMs) / res.frames
                                 : -1.0;
            lastFrames     = fs.frames;
            lastFrameStats = fs;

            const uint64_t compactionDelta = extra.compactions - lastCompactions;
            lastCompactions                = extra.compactions;

            res.viewCount = snap.viewWindowCount;
            res.viewBytes = snap.viewWindowBytes;
            res.llmCount  = snap.llmCount;
            res.llmBytes  = snap.llmBytes;
            const auto intervalMs
                = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - intervalStart
                )
                      .count();
            const double avgRoundMs
                = intervalRounds > 0
                      ? static_cast<double>(std::max<int64_t>(0, intervalMs - extra.seedMs))
                            / intervalRounds
                      : -1.0;
            res.note = fmt::format(
                "累计内容 {} (真实轮次 {} + 注入历史 {}; 第 {} 轮/共 {} 条); 服务端回报用量 {}"
                " token; 活上下文 {} / 上限 {} (区间峰值 {}); 压缩 {} 次 (累计 {}); 展示窗口 {}"
                " 条/{}/总 {} 条; 客户端 {} 条/{}; 内存内容合计 {}; 会话库磁盘 {}; 区间 {} 轮 + 注入"
                " {:.1f} s (每轮均 {:.0f} ms); 渲染 {} 帧/平均 {:.3f} ms",
                fmtSize(cumulative),
                fmtSize(extra.realBytes),
                fmtSize(extra.seededBytes),
                round,
                msgsSent,
                snap.usageTotal,
                snap.liveContextTokens,
                snap.maxContextTokens,
                std::max(intervalMaxLive, snap.liveContextTokens),
                compactionDelta,
                extra.compactions,
                snap.viewWindowCount,
                fmtSize(snap.viewWindowBytes),
                snap.viewTotalCount,
                extra.clientMsgs,
                fmtSize(extra.clientBytes),
                fmtSize(extra.memContentBytes),
                fmtSize(extra.storeBytes),
                intervalRounds,
                static_cast<double>(extra.seedMs) / 1000.0,
                avgRoundMs,
                res.frames,
                res.frameAvgMs
            );

            collectLogicalForTuiLongCtx(res, agent, tui, sessionId, agentConfig->dataDir);
            // 不在采样点做 malloc_trim/_heapmin: 它会提前把空闲页归还系统,
            // 后面的采样点就量不到真实稳态 (端点之间的回收行为也会被抹平)
            fillResourceMemDetail(res, 0, true, false);
            reporter.addResource(res);
            printResourceResult(res, console);
            // 常驻内存归属 (不过滤系统库): 说明 RSS 里除会话/上下文数据外的部分是什么
            {
                ModuleMemBreakdown full;
                full.rows       = res.modules;
                full.valid      = !res.modules.empty();
                full.totalRssMB = res.rssMB;
                full.totalPssMB = res.mem.pssMB;
                full.heap       = res.heapStat;
                printKindSummary(full, fmt::format("tui_longctx {} 常驻分解", res.point), console);
                printModuleBreakdown(
                    full,
                    fmt::format("tui_longctx {} 模块", res.point),
                    14,
                    console
                );
            }
            phaseTracker.mark(
                res.point,
                fmt::format(
                    "累计 {}, 活上下文 {} token, 压缩 {} 次, 会话库 {}",
                    fmtSize(cumulative),
                    snap.liveContextTokens,
                    extra.compactions,
                    fmtSize(extra.storeBytes)
                )
            );
            samples.push_back(res);
            extras.push_back(extra);
            intervalRounds  = 0;
            intervalMaxLive = snap.liveContextTokens;
            intervalStart   = std::chrono::steady_clock::now();
            ++nextPoint;
        }
    }

    phaseTracker.printTable("tui_longctx 分阶段内存", console);
    reporter.attachPhases("tui_longctx", "self", phaseTracker.samples());

    // ---- 汇总表: 各采样点的规模/磁盘/内存/渲染 ----
    console << fmt::format(
        "\n  [tui_longctx] 采样汇总 (模式 {}; 累计内容 {} = 真实轮次 {} + 注入历史 {}; 共 {} 轮 / "
        "{} 条消息, 驱动总耗时 {:.1f} s, mock 普通轮次 {} / 压缩 {} 次)\n",
        mode,
        fmtSize(cumulative),
        fmtSize(cumulative > seededTotal ? cumulative - seededTotal : 0),
        fmtSize(seededTotal),
        round,
        msgsSent,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - driveStart).count(),
        sim.turnCount->load(),
        sim.compactCount->load()
    );
    console << fmt::format(
        "    {:<9} {:>10} {:>11} {:>11} {:>10} {:>8} {:>8} {:>8} {:>6} {:>7}\n",
        "point",
        "累计内容",
        "会话库(磁盘)",
        "内存内容",
        "活上下文",
        "RSS",
        "私有",
        "峰值RSS",
        "线程",
        "帧ms"
    );
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto& r = samples[i];
        const auto& e = extras[i];
        console << fmt::format(
            "    {:<9} {:>10} {:>11} {:>11} {:>10} {:>8.2f} {:>8.2f} {:>8.2f} {:>6} {:>7.3}\n",
            r.point,
            fmtSize(r.tokens),
            fmtSize(e.storeBytes),
            fmtSize(e.memContentBytes),
            e.liveContext,
            r.rssMB,
            r.privateMB,
            r.mem.peakRssMB,
            r.mem.threads,
            r.frameAvgMs
        );
    }

    tui->stop();
    capture.restore();
    std::cout << console.str();

    serverIO->stop();
    clientWork.reset();
    clientCtx.stop();
    if (clientThread.joinable()) {
        clientThread.join();
    }
    // 先投递根的异步销毁 (摘挂载/插件卸载还在 io 线程上跑), 再停 io
    host->unmountRoot("root");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    agentWork.reset();
    agent->ioCtx->stop();
    if (agentThread.joinable()) {
        agentThread.join();
    }
    host.reset();
    agent.reset();
    sim.stop();
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
#endif
}

void benchResourceRealAll() {
    benchResourceRealTui();
    benchResourceServerOnly();
    benchResourceRealTuiChild();
    benchResourcePluginAttrib();
}

} // namespace bench
} // namespace agentxx
