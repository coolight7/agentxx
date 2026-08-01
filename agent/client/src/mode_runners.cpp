#include "agentxx-client/mode_runners.h"

#include "agentxx-client/io/stdio/agent_stdio.h"
#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx/agent/channel_io_transport.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/agent/remote/agent_server.h"
#include "agentxx/agent/remote/session_controller.h"
#include "agentxx/agent/ws_io_transport.h"
#include "agentxx/middlewares/subagent_supervisor.h"
#include "agentxx/util/log.h"
#include "agentxx/util/ws_client.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/executor_work_guard.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include <iostream>
#include <thread>

namespace agentxx {
namespace client {

// ---------------------------------------------------------------------------
// Local unified DIRECT (ChannelAgentIOTransport 直连 TUI ↔ SessionController)
// ---------------------------------------------------------------------------

/// TUI 持有 client transport, SessionController 持有 server transport
/// 无 RemoteClientAgentIO / RemoteServerAgentIO 中间层
static std::shared_ptr<agent::SessionController> setupLocalUnifiedDirect(
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

    // 服务端: SessionController 持有 server transport
    agent::SessionController::Config scCfg;
    scCfg.threadId  = threadId;
    auto controller = std::make_shared<agent::SessionController>(agentEx, agent, scCfg);
    controller->setTransport(std::shared_ptr<agent::AgentIOTransportBase>(std::move(serverTransport)
    ));

    // 在 agent 线程启动: init -> supervisor -> SessionController 驱动循环 + transport 接收循环
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
    auto io       = std::make_shared<AgentStdIO>();
    XX_OUT("======= Agentxx Client (CLI, in-process unified) =======");
    setupLocalUnifiedDirect(clientEx, agent, io, "session");
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
        io->sendToPeer(agent::WireUserInput{"session", *input});
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
    const std::string threadId = "session";

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
    auto tui = std::make_shared<AgentTUI>(clientEx, ctx, threadId);
    tui->start();

    setupLocalUnifiedDirect(clientEx, agent, tui, threadId);

    // TUI 模式下输入由 FTXUI 事件循环驱动 (sendUserInputLocked 经 transport 发送)
    // 此处等待 TUI 停止
    while (tui->running()) {
        asio::steady_timer timer(clientEx);
        timer.expires_after(std::chrono::milliseconds(200));
        co_await timer.async_wait(asio::use_awaitable);
    }
    tui->stop();
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
    auto io = std::make_shared<AgentStdIO>();

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
    agent::WireHello hello{"session", token, 0, ""};
    bool             ok = co_await transport->connect(hello);
    if (!ok) {
        XX_LOGE("[remote_cli] connection failed");
        co_return;
    }

    // 指定模型 (经独立的模型选择通道, 而非随每条输入发送)
    if (!model.empty()) {
        io->requestSelectModel("session", model);
    }

    // 启动接收循环
    asio::co_spawn(ex, io->runTransportLoop(), asio::detached);

    // 客户端启动后拉取一次启动信息 (MCP/Skill/Memory)
    io->requestAppendComponentInfo("session");

    // 输入循环
    for (;;) {
        auto input = co_await io->getInput();
        if (!input.has_value()) {
            break;
        }
        if (input->empty()) {
            continue;
        }
        io->sendToPeer(agent::WireUserInput{"session", *input});
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
    auto io          = std::make_shared<AgentTUI>(ex, ctx, "session");
    io->setRemoteUrl(url);
    io->start();

    agent::WsAgentIOTransport::Config transportCfg;
    util::WsClientConfig              wsCfg;
    wsCfg.recvTimeout = std::chrono::seconds{60};

    auto transport
        = std::make_shared<agent::WsAgentIOTransport>(ex, url, token, transportCfg, wsCfg);
    io->setTransport(transport);

    // 连接并握手
    agent::WireHello hello{"session", token, 0, ""};
    bool             ok = co_await transport->connect(hello);
    if (!ok) {
        XX_LOGE("[remote_tui] connection failed");
        io->stop();
        co_return;
    }

    // 指定模型 (经独立的模型选择通道); 先于 GetModel 发送, 使其返回所选模型
    if (!model.empty()) {
        io->requestSelectModel("session", model);
    }

    // 请求服务端当前模型信息, 待 onPeerMessage 收到 WireModelInfo 后更新显示
    io->sendToPeer(agent::WireGetModel{"session"});

    // 客户端启动后拉取一次启动信息 (MCP/Skill/Memory)
    io->requestAppendComponentInfo("session");

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
