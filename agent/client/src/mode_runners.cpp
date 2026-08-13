#include "agentxx-client/mode_runners.h"

#include "agentxx-client/io/stdio/agent_stdio.h"
#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx/agent/io/agent_server.h"
#include "agentxx/agent/io/channel_io_transport.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/io/ws_io_transport.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/middlewares/subagent_supervisor.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "agentxx/util/ws_client.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/executor_work_guard.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <thread>

// 获取当前进程 PID: 标准库与 asio 均无公开接口 (asio 内部实现也直接调用
// ::getpid()/::GetCurrentProcessId(), 如 connect_pipe.ipp), 无更通用的替代;
// 此为跨平台标准做法, 与 boost.process 内部实现一致。
#ifdef _WIN32
#include <windows.h> // GetCurrentProcessId
#else
#include <unistd.h> // getpid
#endif

namespace agentxx {
namespace client {

// ---------------------------------------------------------------------------
// 会话 threadId 生成
// ---------------------------------------------------------------------------

/// 随机数兜底: random_device 在极端环境 (部分旧 Android/嵌入式) 可能抛异常,
/// 退回以时钟为种子, 保证生成函数绝不失败
static uint32_t randomSeed() {
    return agentxx::util::catchError<uint32_t>(
        []() -> uint32_t {
            std::random_device rd;
            return rd();
        },
        [](std::string) -> uint32_t {
            return static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()
            );
        }
    );
}

/// 生成尽量唯一的会话 threadId:
/// - 高精度时间戳: 区分不同时刻启动的会话 (纳秒级)
/// - 进程 PID:     区分同一主机上的不同进程 (多次启动 agentxx)
/// - 随机数:       增加不可预测性, 避免并发启动碰撞
/// - 自增序号:     同进程内极端同纳秒多次调用亦唯一
std::string generateUniqueThreadId() {
    const auto ts = std::chrono::high_resolution_clock::now().time_since_epoch().count();
#ifdef _WIN32
    const long pid = static_cast<long>(::GetCurrentProcessId());
#else
    const long pid = static_cast<long>(::getpid());
#endif
    static std::atomic<uint32_t> seq{0};
    const uint32_t               cnt = seq.fetch_add(1, std::memory_order_relaxed);
    return fmt::format("sess-{:x}-{}-{:08x}-{:04x}", ts, pid, randomSeed(), cnt);
}

/// 启动时解析 TUI 主题: 从全局设置恢复上次保存的主题 (tui.theme),
/// 未设置/库不可用时默认 Dark。须在 main 中 attachDb() 之后调用。
static TUITheme resolveTuiTheme() {
    return TUISettings::instance().themeKind() == TUISettings::kThemeLight ? TUITheme::lightTheme()
                                                                           : TUITheme::darkTheme();
}

// ---------------------------------------------------------------------------
// Local unified DIRECT (ChannelAgentIOTransport 直连 TUI ↔ SessionServerAgentIO)
// ---------------------------------------------------------------------------

/// TUI 持有 client transport, SessionServerAgentIO 持有 server transport
/// 两端点经 Channel 直连, 无中间包装层
static std::shared_ptr<agent::SessionServerAgentIO> setupLocalUnifiedDirect(
    asio::any_io_executor               clientEx,
    std::shared_ptr<agent::CodeAgent>   agent,
    std::shared_ptr<agent::AgentIOBase> clientIO,
    const std::string&                  threadId
) {
    auto agentEx = agent->ioCtx->get_executor();
    auto [clientTransport, serverTransport]
        = agent::ChannelAgentIOTransport::makePair(clientEx, agentEx);

    // 客户端 IO 持有 client transport
    clientIO->setTransport(std::shared_ptr<agent::AgentIOTransportBase>(std::move(clientTransport))
    );

    // 服务端: SessionServerAgentIO 持有 server transport
    agent::SessionServerAgentIO::Config scCfg;
    scCfg.threadId = threadId;
    auto serverIO  = std::make_shared<agent::SessionServerAgentIO>(agentEx, agent, scCfg);
    serverIO->setTransport(std::shared_ptr<agent::AgentIOTransportBase>(std::move(serverTransport))
    );

    // 注册 agent 启动进度通知: init() 各启动阶段 (agent 线程) → 客户端端点
    // (TUI 在"启动中"banner 逐步展示当前执行的操作, 如加载 MCP/Skill/RAG 等)
    // - 必须在 init() 协程启动前注册, 否则 init 前期的步骤会丢失
    // - 回调内 clientIO 为 shared_ptr 捕获 (TUI 存活期安全); StdIO 端点
    //   不覆写 onServerProgress, 回调为 no-op
    if (auto ctx = agent->agentContext) {
        ctx->startupNotifier = [clientIO](std::string_view step) {
            clientIO->onServerProgress(step);
        };
    }

    // transport 接收循环先于 init() 启动:
    // init() 中 MCP 连接等网络操作可能耗时数秒, 若等 init 完成才启动接收循环,
    // 客户端在 init 期间发出的请求 (如模型选择弹窗的 WireGetModel / WireHello) 会
    // 一直排队, 导致弹窗首次打开要等 init 完成才有数据。
    // 各 Wire 消息处理在 init 前均安全:
    // - WireGetModel: getCurrentModelName 在 modelRegistry 为空时回退 agentConfig
    // - WireHello: 历史为空时全量同步为空; Session 未绑定 io 线程时 assertIoThread 为 no-op
    // - WireUserInput: 仅入队, 由 init 之后启动的 run() 循环消费 (run 依赖 engine)
    asio::co_spawn(
        *agent->ioCtx,
        [serverIO]() -> asio::awaitable<void> {
            co_await serverIO->runTransportLoop();
        },
        asio::detached
    );

    // 在 agent 线程启动: init -> supervisor -> 会话驱动循环
    // (run 依赖 init 产出的 engine, 须在 init 完成之后启动;
    //   init 前的用户输入会先缓存在 inputChannel, run 启动后正常消费)
    asio::co_spawn(
        *agent->ioCtx,
        [agent, serverIO, clientIO, threadId]() -> asio::awaitable<void> {
            co_await agent->init();
            // 须经 shared_ptr 持有: 总线 handler 以 weak_ptr 捕获, 避免悬空 this
            auto supervisor = std::make_shared<middleware::SubagentSupervisor>(agent->agentContext);
            co_await supervisor->start();
            // 拉取启动信息 (MCP/Skill/Memory): 须在 init 完成后发送 —— transport 接收循环
            // 已先于 init 启动, 若此时 (init 前) 发送会立即得到空列表 (组件尚未加载),
            // 客户端将永远显示不出已加载的组件
            clientIO->requestAppendComponentInfo(threadId);
            // 通知客户端: agent-server 就绪 (init/组件加载完成, 会话驱动循环即将
            // 启动并开始消费用户输入)。客户端 (TUI) 据此解除"启动中"输入限制,
            // 刷新连接前排队输入 (此时发送的输入由 server 端 inputChannel 缓存,
            // run() 启动后正常消费)
            clientIO->onServerReady();
            co_await serverIO->run();
        },
        asio::detached
    );

    // 客户端: 启动 transport 接收循环 (在 client 线程)
    asio::co_spawn(
        clientEx,
        [clientIO]() -> asio::awaitable<void> {
            co_await clientIO->runTransportLoop();
        },
        asio::detached
    );

    // 发送 hello 触发服务端重放/同步
    clientIO->sendToPeer(agent::WireHello{threadId, "", 0, ""});

    return serverIO;
}

template<typename Coro>
static void runLocalUnifiedMain(std::shared_ptr<agent::CodeAgent> agent, Coro coro) {
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

static asio::awaitable<void> runLocalCliUnifiedAsync(std::shared_ptr<agent::CodeAgent> agent) {
    auto clientEx = co_await asio::this_coro::executor;
    auto io       = std::make_shared<StdIOClientAgentIO>();
    // 每次启动生成唯一会话 id, 避免多实例/多次启动共用 "session" 导致会话串扰
    const std::string threadId = generateUniqueThreadId();
    XX_LOGI("======= Agentxx Client (CLI, in-process unified) =======");
    auto serverIO = setupLocalUnifiedDirect(clientEx, agent, io, threadId);
    // CLI 输入循环: 从 stdin 读取并发送
    std::cout << "\n>>> " << std::flush;
    for (;;) {
        auto input = co_await io->getInput();
        if (!input.has_value()) {
            break;
        }
        if (input->empty()) {
            continue;
        }
        io->sendToPeer(agent::WireUserInput{threadId, *input});
    }
    // 停止服务端点并等待驱动循环退出 (避免 io_context 析构时销毁其协程导致 use-after-free)
    serverIO->stop();
    while (serverIO->running()) {
        asio::steady_timer timer(clientEx);
        timer.expires_after(std::chrono::milliseconds(20));
        co_await timer.async_wait(asio::use_awaitable);
    }
}

void runLocalCliUnified(std::shared_ptr<agent::CodeAgent> agent) {
    runLocalUnifiedMain(agent, runLocalCliUnifiedAsync(agent));
}

static asio::awaitable<void> runLocalTuiUnifiedAsync(
    std::shared_ptr<agent::CodeAgent> agent,
    agent::PermissionMode             permissionMode
) {
    auto clientEx = co_await asio::this_coro::executor;
    // 每次启动生成唯一会话 id, 避免多实例/多次启动共用 "session" 导致会话串扰
    const std::string threadId = generateUniqueThreadId();

    // 注意: TUI 不持有 AgentContext/Session (属于 agent-server 线程), 所有
    // agent 侧信息 (模型列表/上下文统计/LLM 上下文) 均经 Wire 消息由服务端获取
    auto tui
        = std::make_shared<TUIClientAgentIO>(clientEx, threadId, resolveTuiTheme(), permissionMode);
    tui->start();

    auto serverIO = setupLocalUnifiedDirect(clientEx, agent, tui, threadId);

    // 请求服务端当前模型信息 (WireModelInfo 回填状态栏模型名/弹窗列表):
    // 本地模式无上下文可解析默认模型, 必须显式请求; 发送时机在 transport 就绪后
    // 即可 (init 前处理 WireGetModel 安全: modelRegistry 为空时回退 agentConfig)
    tui->sendToPeer(agent::WireGetModel{threadId});

    // TUI 模式下输入由 FTXUI 事件循环驱动 (sendUserInputLocked 经 transport 发送)
    // 此处等待 TUI 停止
    while (tui->running()) {
        asio::steady_timer timer(clientEx);
        timer.expires_after(std::chrono::milliseconds(200));
        co_await timer.async_wait(asio::use_awaitable);
    }
    tui->stop();

    // 停止服务端点并等待其驱动循环退出, 使其协程释放对 serverIO 的持有,
    // 避免 serverIO(及其 channel transport) 在 agent io_context 析构时才被销毁
    // 而触发 channel 的 use-after-free
    serverIO->stop();
    while (serverIO->running()) {
        asio::steady_timer timer(clientEx);
        timer.expires_after(std::chrono::milliseconds(20));
        co_await timer.async_wait(asio::use_awaitable);
    }
}

void runLocalTuiUnified(
    std::shared_ptr<agent::CodeAgent> agent,
    agent::PermissionMode             permissionMode
) {
    runLocalUnifiedMain(agent, runLocalTuiUnifiedAsync(agent, permissionMode));
}

// ---------------------------------------------------------------------------
// Remote client (WS connection to agent server, WsAgentIOTransport 直连)
// ---------------------------------------------------------------------------

static asio::awaitable<void>
    runRemoteCliAsync(std::string url, std::string token, std::string model) {
    auto ex = co_await asio::this_coro::executor;
    auto io = std::make_shared<StdIOClientAgentIO>();

    // 每次启动生成唯一会话 id: 服务端按 threadId 区分会话,
    // 共用 "session" 会使多个客户端实例挂到同一会话上互相串扰
    const std::string threadId = generateUniqueThreadId();

    agent::WsAgentIOTransport::Config transportCfg;
    util::WsClientConfig              wsCfg;
    wsCfg.recvTimeout = std::chrono::seconds{60};

    auto transport = std::make_shared<agent::WsAgentIOTransport>(
        ex,
        std::move(url),
        std::move(token),
        transportCfg,
        wsCfg
    );
    io->setTransport(transport);

    XX_LOGI("======= Agentxx Remote Client (CLI, auto-reconnect) =======");

    // 连接并握手
    agent::WireHello hello{threadId, token, 0, ""};
    bool             ok = co_await transport->connect(hello);
    if (!ok) {
        XX_LOGE("[remote_cli] connection failed");
        co_return;
    }

    // 指定模型 (经独立的模型选择通道, 而非随每条输入发送)
    if (!model.empty()) {
        io->requestSelectModel(threadId, model);
    }

    // 启动接收循环
    asio::co_spawn(ex, io->runTransportLoop(), asio::detached);

    // 客户端启动后拉取一次启动信息 (MCP/Skill/Memory)
    io->requestAppendComponentInfo(threadId);

    // 输入循环
    std::cout << "\n>>> " << std::flush;
    for (;;) {
        auto input = co_await io->getInput();
        if (!input.has_value()) {
            break;
        }
        if (input->empty()) {
            continue;
        }
        io->sendToPeer(agent::WireUserInput{threadId, *input});
    }
    transport->close();
}

void runRemoteCli(std::string_view url, std::string_view token, std::string_view model) {
    asio::io_context ctx;
    asio::co_spawn(
        ctx,
        runRemoteCliAsync(std::string{url}, std::string{token}, std::string{model}),
        asio::detached
    );
    ctx.run();
}

static asio::awaitable<void> runRemoteTuiAsync(
    std::string           url,
    std::string           token,
    std::string           model,
    agent::PermissionMode permissionMode
) {
    auto ex = co_await asio::this_coro::executor;

    // 每次启动生成唯一会话 id: 服务端按 threadId 区分会话,
    // 共用 "session" 会使多个客户端实例挂到同一会话上互相串扰
    const std::string threadId = generateUniqueThreadId();
    // 注意: TUI 不持有 AgentContext/Session (属于 agent-server 线程),
    // 模型名/上下文统计等均经 Wire 消息由服务端获取
    auto io = std::make_shared<TUIClientAgentIO>(ex, threadId, resolveTuiTheme(), permissionMode);
    io->setRemoteUrl(url);
    io->start();

    // TUI 立即启动: 初始 connState=Connecting, banner 显示"agent-server 正在
    // 启动中", 用户输入进入待发送队列 (不发送); 连接成败均不阻塞 TUI 界面

    // 连接流程 (失败可重试, 不退出 TUI):
    // - 每次尝试创建全新 transport (失败后的 transport 已 stopped/半初始化,
    //   复用会出现握手/写队列状态残留; 新建更干净)
    // - transport->connect 内部有限次重试 (maxReconnectAttempts=2), 均失败则
    //   返回 false → banner 显示"连接失败 + [重试]"等待用户点击
    // - 用户点击重试 → requestRetry() 置 Connecting 并唤醒 waitRetry 重新循环
    // - TUI 退出 (running_=false) 时流程尽快终止
    agent::WireHello hello{threadId, token, 0, ""};
    bool             connected = false;
    while (!connected) {
        agent::WsAgentIOTransport::Config transportCfg;
        // 有限次尝试后返回失败 (默认 0=无限内部重连, 用户永远等不到失败提示)
        transportCfg.maxReconnectAttempts = 2;
        util::WsClientConfig wsCfg;
        wsCfg.recvTimeout = std::chrono::seconds{60};

        auto transport
            = std::make_shared<agent::WsAgentIOTransport>(ex, url, token, transportCfg, wsCfg);
        io->setTransport(transport);

        connected = co_await transport->connect(hello);
        if (connected) {
            break;
        }

        XX_LOGE("[remote_tui] connection failed, waiting for user retry");
        transport->close();
        if (!io->running()) {
            break;
        }
        io->setConnState(ConnState::Failed);
        // 等待用户点击 banner 上的"重试" (TUI 退出时尽快返回, 见 waitRetry)
        co_await io->waitRetry();
        if (!io->running()) {
            break;
        }
        io->setConnState(ConnState::Connecting);
    }

    if (!connected) {
        // 用户在连接失败 (或等待重试) 期间退出了 TUI: 停止并退出, 不进入会话
        XX_LOGW("[remote_tui] quit before connection established");
        io->stop();
        co_return;
    }

    io->setConnState(ConnState::Connected);

    // 指定模型 (经独立的模型选择通道); 先于 GetModel 发送, 使其返回所选模型
    if (!model.empty()) {
        io->requestSelectModel(threadId, model);
    }

    // 请求服务端当前模型信息, 待 onPeerMessage 收到 WireModelInfo 后更新显示
    io->sendToPeer(agent::WireGetModel{threadId});

    // 客户端启动后拉取一次启动信息 (MCP/Skill/Memory)
    io->requestAppendComponentInfo(threadId);

    // 启动接收循环
    asio::co_spawn(ex, io->runTransportLoop(), asio::detached);

    // 连接建立后刷新待发送队列: 发送用户连接前排队的输入
    // (置 isStreaming 并经 transport 发送; 后续排队输入由 TurnEnd 依次分发)
    io->flushPendingInput();

    // 等待 TUI 退出
    while (io->running()) {
        asio::steady_timer timer(ex);
        timer.expires_after(std::chrono::milliseconds(200));
        co_await timer.async_wait(asio::use_awaitable);
    }
    io->transport()->close();
    io->stop();
}

void runRemoteTui(
    std::string_view      url,
    std::string_view      token,
    std::string_view      model,
    agent::PermissionMode permissionMode
) {
    asio::io_context ctx;
    asio::co_spawn(
        ctx,
        runRemoteTuiAsync(std::string{url}, std::string{token}, std::string{model}, permissionMode),
        asio::detached
    );
    ctx.run();
}

} // namespace client
} // namespace agentxx
