#include "agentxx-client/mode_runners.h"

#include "agentxx-client/io/stdio/agent_stdio.h"
#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/agent/remote/agent_server.h"
#include "agentxx/agent/remote/channel_transport.h"
#include "agentxx/agent/remote/remote_client_io.h"
#include "agentxx/util/log.h"
#include "agentxx/util/ws_client.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/executor_work_guard.hpp"
#include "asio/io_context.hpp"
#include <thread>

namespace agentxx {
namespace client {

// ---------------------------------------------------------------------------
// Local unified (in-process ChannelTransport)
// ---------------------------------------------------------------------------

static std::shared_ptr<agent::remote::RemoteClientAgentIO> setupLocalUnified(
    asio::any_io_executor               clientEx,
    std::shared_ptr<agent::DeepAgent>   agent,
    std::shared_ptr<agent::AgentIOBase> io
) {
    auto agentEx = agent->ioCtx->get_executor();
    auto [clientTransport, serverTransport]
        = agent::remote::ChannelTransport::makePair(clientEx, agentEx);

    agent::remote::AgentServer::Config srvCfg;
    srvCfg.autoGenerateToken = false;
    srvCfg.token             = "";
    auto server              = std::make_shared<agent::remote::AgentServer>(agent, srvCfg);

    asio::co_spawn(
        *agent->ioCtx,
        [agent, server, st = std::move(serverTransport)]() mutable -> asio::awaitable<void> {
            co_await agent->init();
            auto supervisor = middleware::SubagentSupervisor{agent->agentContext};
            co_await supervisor.start();
            co_await server->serveTransport(std::move(st));
            co_return;
        },
        asio::detached
    );

    agent::remote::RemoteClientAgentIO::Config cliCfg;
    return std::make_shared<agent::remote::RemoteClientAgentIO>(
        clientEx,
        std::move(clientTransport),
        std::move(io),
        cliCfg
    );
}

template<typename Coro>
static void runLocalUnifiedMain(std::shared_ptr<agent::DeepAgent> agent, Coro coro) {
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

static asio::awaitable<void> runLocalCliUnifiedAsync(std::shared_ptr<agent::DeepAgent> agent) {
    auto clientEx = co_await asio::this_coro::executor;
    auto io       = std::make_shared<AgentStdIO>();
    auto remote   = setupLocalUnified(clientEx, agent, io);
    XX_OUT("======= Agentxx Client (CLI, in-process unified) =======");
    co_await remote->runSession("session", "");
    co_await remote->shutdown();
}

void runLocalCliUnified(std::shared_ptr<agent::DeepAgent> agent) {
    runLocalUnifiedMain(agent, runLocalCliUnifiedAsync(agent));
}

static asio::awaitable<void> runLocalTuiUnifiedAsync(
    std::shared_ptr<agent::DeepAgent>   agent,
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

    auto remote = setupLocalUnified(clientEx, agent, tui);

    tui->setControlTarget(remote);
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
    std::shared_ptr<agent::DeepAgent>   agent,
    std::shared_ptr<agent::AgentConfig> config
) {
    runLocalUnifiedMain(agent, runLocalTuiUnifiedAsync(agent, config));
}

// ---------------------------------------------------------------------------
// Remote client (WS connection to deepagent server)
// ---------------------------------------------------------------------------

static asio::awaitable<void>
    runRemoteCliAsync(std::string url, std::string token, std::string model) {
    auto ex = co_await asio::this_coro::executor;
    auto io = std::make_shared<AgentStdIO>();

    agent::remote::RemoteClientAgentIO::Config cfg;
    util::WsClientConfig                       wsCfg;
    wsCfg.recvTimeout = std::chrono::seconds{60};

    auto remote = std::make_shared<agent::remote::RemoteClientAgentIO>(
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

void runRemoteCli(std::string_view url, std::string_view token, std::string_view model) {
    asio::io_context ctx;
    asio::co_spawn(ctx, runRemoteCliAsync(std::string{url}, std::string{token}, std::string{model}), asio::detached);
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

    agent::remote::RemoteClientAgentIO::Config cfg;
    util::WsClientConfig                       wsCfg;
    wsCfg.recvTimeout = std::chrono::seconds{60};

    auto remote = std::make_shared<agent::remote::RemoteClientAgentIO>(
        ex,
        io,
        std::move(url),
        std::move(token),
        cfg,
        wsCfg
    );

    io->setControlTarget(remote);

    co_await remote->runSession("session", model);
    co_await remote->shutdown();
    io->stop();
}

void runRemoteTui(
    std::shared_ptr<agent::AgentConfig> config,
    std::string_view                    url,
    std::string_view                    token,
    std::string_view                    model
) {
    asio::io_context ctx;
    asio::co_spawn(ctx, runRemoteTuiAsync(config, std::string{url}, std::string{token}, std::string{model}), asio::detached);
    ctx.run();
}

} // namespace client
} // namespace agentxx
