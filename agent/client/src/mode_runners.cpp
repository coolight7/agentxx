#include "agentxx-client/mode_runners.h"

#include "agentxx-client/io/stdio/agent_stdio.h"
#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx/agent/io/agent_server.h"
#include "agentxx/agent/io/channel_io_transport.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/io/ws_io_transport.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/middlewares/subagent_supervisor.h"
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
#include <windows.h>  // GetCurrentProcessId
#else
#include <unistd.h>  // getpid
#endif

namespace agentxx {
namespace client {

// ---------------------------------------------------------------------------
// 会话 threadId 生成
// ---------------------------------------------------------------------------

/// 随机数兜底: random_device 在极端环境 (部分旧 Android/嵌入式) 可能抛异常,
/// 退回以时钟为种子, 保证生成函数绝不失败
static uint32_t randomSeed() {
    try {
        std::random_device rd;
        return rd();
    } catch (...) {
        return static_cast<uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
    }
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
    scCfg.threadId  = threadId;
    auto controller = std::make_shared<agent::SessionServerAgentIO>(agentEx, agent, scCfg);
    controller->setTransport(std::shared_ptr<agent::AgentIOTransportBase>(std::move(serverTransport)
    ));

    // 在 agent 线程启动: init -> supervisor -> SessionServerAgentIO 驱动循环 + transport 接收循环
    asio::co_spawn(
        *agent->ioCtx,
        [agent, controller]() -> asio::awaitable<void> {
            co_await agent->init();
            // 须经 shared_ptr 持有: 总线 handler 以 weak_ptr 捕获, 避免悬空 this
            auto supervisor = std::make_shared<middleware::SubagentSupervisor>(agent->agentContext);
            co_await supervisor->start();
            // 并发运行: transport 接收循环 + 会话驱动循环
            co_await controller->runTransportLoop();
        },
        asio::detached
    );
    asio::co_spawn(
        *agent->ioCtx,
        [controller]() -> asio::awaitable<void> {
            co_await controller->run();
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

    // 客户端启动后拉取一次启动信息 (MCP/Skill/Memory)
    clientIO->requestAppendComponentInfo(threadId);

    return controller;
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
    XX_OUT("======= Agentxx Client (CLI, in-process unified) =======");
    auto controller = setupLocalUnifiedDirect(clientEx, agent, io, threadId);
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
    controller->stop();
    while (controller->running()) {
        asio::steady_timer timer(clientEx);
        timer.expires_after(std::chrono::milliseconds(20));
        co_await timer.async_wait(asio::use_awaitable);
    }
}

void runLocalCliUnified(std::shared_ptr<agent::CodeAgent> agent) {
    runLocalUnifiedMain(agent, runLocalCliUnifiedAsync(agent));
}

static asio::awaitable<void> runLocalTuiUnifiedAsync(
    std::shared_ptr<agent::CodeAgent>   agent,
    std::shared_ptr<agent::AgentConfig> config
) {
    auto              clientEx = co_await asio::this_coro::executor;
    // 每次启动生成唯一会话 id, 避免多实例/多次启动共用 "session" 导致会话串扰
    const std::string threadId = generateUniqueThreadId();

    auto ctx         = std::make_shared<agent::AgentContext>();
    ctx->agentConfig = config;
    {
        auto registry = std::make_shared<agent::ModelProviderRegistry>();
        for (const auto& [name, mc] : config->availableModels) {
            registry->registerModel(name, mc);
        }
        if (config->availableModels.empty()) {
            registry->registerModel(config->model.modelName, config->model);
            registry->setDefaultModel(config->model.modelName);
        } else if (!config->currentModelName.empty()
                   && registry->hasModel(config->currentModelName)) {
            registry->setDefaultModel(config->currentModelName);
        }
        ctx->modelRegistry = std::move(registry);
    }
    auto tui = std::make_shared<TUIClientAgentIO>(clientEx, ctx, threadId);
    tui->start();

    auto controller = setupLocalUnifiedDirect(clientEx, agent, tui, threadId);

    // TUI 模式下输入由 FTXUI 事件循环驱动 (sendUserInputLocked 经 transport 发送)
    // 此处等待 TUI 停止
    while (tui->running()) {
        asio::steady_timer timer(clientEx);
        timer.expires_after(std::chrono::milliseconds(200));
        co_await timer.async_wait(asio::use_awaitable);
    }
    tui->stop();

    // 停止服务端点并等待其驱动循环退出, 使其协程释放对 controller 的持有,
    // 避免 controller(及其 channel transport) 在 agent io_context 析构时才被销毁
    // 而触发 channel 的 use-after-free
    controller->stop();
    while (controller->running()) {
        asio::steady_timer timer(clientEx);
        timer.expires_after(std::chrono::milliseconds(20));
        co_await timer.async_wait(asio::use_awaitable);
    }
}

void runLocalTuiUnified(
    std::shared_ptr<agent::CodeAgent>   agent,
    std::shared_ptr<agent::AgentConfig> config
) {
    runLocalUnifiedMain(agent, runLocalTuiUnifiedAsync(agent, config));
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

    XX_OUT("======= Agentxx Remote Client (CLI, auto-reconnect) =======");

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
    std::shared_ptr<agent::AgentConfig> config,
    std::string                         url,
    std::string                         token,
    std::string                         model
) {
    auto ex = co_await asio::this_coro::executor;

    auto ctx         = std::make_shared<agent::AgentContext>();
    ctx->agentConfig = config;
    // 每次启动生成唯一会话 id: 服务端按 threadId 区分会话,
    // 共用 "session" 会使多个客户端实例挂到同一会话上互相串扰
    const std::string threadId = generateUniqueThreadId();
    auto              io       = std::make_shared<TUIClientAgentIO>(ex, ctx, threadId);
    io->setRemoteUrl(url);
    io->start();

    agent::WsAgentIOTransport::Config transportCfg;
    util::WsClientConfig              wsCfg;
    wsCfg.recvTimeout = std::chrono::seconds{60};

    auto transport
        = std::make_shared<agent::WsAgentIOTransport>(ex, url, token, transportCfg, wsCfg);
    io->setTransport(transport);

    // 连接并握手
    agent::WireHello hello{threadId, token, 0, ""};
    bool             ok = co_await transport->connect(hello);
    if (!ok) {
        XX_LOGE("[remote_tui] connection failed");
        io->stop();
        co_return;
    }

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

    // 等待 TUI 退出
    while (io->running()) {
        asio::steady_timer timer(ex);
        timer.expires_after(std::chrono::milliseconds(200));
        co_await timer.async_wait(asio::use_awaitable);
    }
    transport->close();
    io->stop();
}

void runRemoteTui(
    std::shared_ptr<agent::AgentConfig> config,
    std::string_view                    url,
    std::string_view                    token,
    std::string_view                    model
) {
    asio::io_context ctx;
    asio::co_spawn(
        ctx,
        runRemoteTuiAsync(config, std::string{url}, std::string{token}, std::string{model}),
        asio::detached
    );
    ctx.run();
}

} // namespace client
} // namespace agentxx
