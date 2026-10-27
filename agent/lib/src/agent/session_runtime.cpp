#include "agentxx/agent/session_runtime.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/base_agent.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/channel_io_transport.h"
#include "agentxx/util/exception.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"
#include "fmt/format.h"
#include "utilxx_base/log.h"
#include <utility>

namespace agentxx {
namespace agent {

namespace {

/// 统一按"投递到目标执行器"的方式启动一段协程
/// - 调用方可能就在该执行器上: 直接 co_spawn, 少一次排队
/// - 否则 post 过去再 co_spawn (执行器可能还没开始 run)
template<typename Fn>
void spawnOn(asio::any_io_executor ex, Fn&& fn) {
    asio::post(ex, [ex, fn = std::forward<Fn>(fn)]() mutable {
        asio::co_spawn(ex, std::move(fn), asio::detached);
    });
}

} // namespace

std::shared_ptr<AgentSessionRuntime> AgentSessionRuntime::create(
    std::shared_ptr<BaseAgent>   agent,
    std::shared_ptr<AgentIOBase> clientIO,
    Config                       cfg
) {
    if (!agent || !clientIO || !agent->ioCtx || !cfg.clientExecutor || cfg.sessionId.empty()) {
        XX_LOGE(
            "[runtime] create: invalid arguments (agent={} clientIO={} ioCtx={} clientExecutor={} "
            "sessionId='{}')",
            agent != nullptr,
            clientIO != nullptr,
            agent && agent->ioCtx != nullptr,
            static_cast<bool>(cfg.clientExecutor),
            cfg.sessionId
        );
        return nullptr;
    }
    return std::shared_ptr<AgentSessionRuntime>(
        new AgentSessionRuntime(std::move(agent), std::move(clientIO), std::move(cfg))
    );
}

AgentSessionRuntime::AgentSessionRuntime(
    std::shared_ptr<BaseAgent>   agent,
    std::shared_ptr<AgentIOBase> clientIO,
    Config                       cfg
) :
    cfg_(std::move(cfg)),
    agent_(std::move(agent)),
    clientIO_(std::move(clientIO)) {
    const auto agentEx = agent_->ioCtx->get_executor();

    // 进程内传输对: client 端交给客户端端点, 服务端交给会话驱动端点
    auto [clientTrans, serverTrans]
        = ChannelAgentIOTransport::makePair(cfg_.clientExecutor, agentEx);

    clientIO_->setTransport(std::shared_ptr<AgentIOTransportBase>(std::move(clientTrans)));

    SessionServerAgentIO::Config scCfg;
    scCfg.sessionId             = cfg_.sessionId;
    scCfg.interruptTimeout      = cfg_.interruptTimeout;
    scCfg.deltaBufferCap        = cfg_.deltaBufferCap;
    scCfg.initialSyncTailCount  = cfg_.initialSyncTailCount;
    scCfg.collectWindow         = cfg_.collectWindow;
    serverIO_                   = std::make_shared<SessionServerAgentIO>(agentEx, agent_, scCfg);
    serverIO_->setTransport(std::shared_ptr<AgentIOTransportBase>(std::move(serverTrans)));

    host_ = std::move(cfg_.host);
}

AgentSessionRuntime::~AgentSessionRuntime() = default;

void AgentSessionRuntime::start() {
    if (started_.exchange(true)) {
        return;
    }
    const auto agentEx = agent_->ioCtx->get_executor();

    // init 进度转发必须在 init 协程启动前注册, 否则前期步骤会丢
    if (cfg_.forwardInitProgress && !cfg_.skipInit) {
        if (auto ctx = agent_->getContext()) {
            auto clientIO   = clientIO_;
            ctx->initNotifier = [clientIO](std::string_view step) {
                clientIO->onServerProgress(step);
            };
        }
    }

    // 客户端: 接收循环 + hello (先于 init 启动, 让 init 期间的查询不被排队;
    // 各类 wire 查询在 init 前处理都安全, 见本地直连模式的同段说明)
    auto clientEx = cfg_.clientExecutor;
    spawnOn(clientEx, [self = shared_from_this()]() -> asio::awaitable<void> {
        co_await self->clientIO_->runTransportLoop();
    });
    asio::post(clientEx, [self = shared_from_this()]() {
        self->clientIO_->sendToPeer(WireHello{
            .sessionId = self->cfg_.sessionId,
            .token     = "",
            .lastSeq   = self->cfg_.lastSeq,
            .tailHash  = self->cfg_.tailHash,
            .model     = "",
            .language  = self->cfg_.language,
        });
        if (self->cfg_.onTransportStarted) {
            self->cfg_.onTransportStarted();
        }
    });

    // 服务端: 接收循环先起 (hello 等请求不排队), 再跑 init → 就绪 → 驱动循环
    spawnOn(agentEx, [self = shared_from_this()]() -> asio::awaitable<void> {
        co_await self->serverIO_->runTransportLoop();
    });
    spawnOn(agentEx, [self = shared_from_this()]() -> asio::awaitable<void> {
        co_await self->runMain();
    });
}

asio::awaitable<void> AgentSessionRuntime::runMain() {
    auto agent = agent_;

    if (!cfg_.skipInit) {
        // ---- agent 节点容器 (进程级: 共享 ioCtx / 线程池 / 事件总线) ----
        if (!host_) {
            AgentHost::Config hostCfg;
            hostCfg.ioCtx = agent->ioCtx;
            if (cfg_.loadPluginsInHost) {
                if (auto ctx = agent->getContext(); ctx && ctx->agentConfig) {
                    hostCfg.pluginList   = ctx->agentConfig->plugins;
                    hostCfg.primaryAgent = ctx;
                }
            }
            host_ = AgentHost::create(std::move(hostCfg));
        }

        // ---- 插件加载 (进程一份; 各 agent 只在 init 里挂载) ----
        if (cfg_.loadPluginsInHost && host_) {
            co_await host_->loadPluginsAsync();
            if (auto pluginHost = host_->pluginHost()) {
                if (auto ctx = agent->getContext()) {
                    ctx->pluginManager      = pluginHost;
                    ctx->pluginManagerOwned = false; // 进程共享管理器: 不随本 agent 析构卸载
                }
            }
        }

        // ---- agent 初始化 (含按策略挂载插件) ----
        const bool initOk = co_await agentxx::util::catchErrorAsync<bool>(
            [agent]() -> asio::awaitable<bool> {
                co_await agent->init();
                co_return true;
            },
            [self = shared_from_this()](std::string errmsg) -> asio::awaitable<bool> {
                XX_LOGE("[runtime] agent init failed: {}", errmsg);
                self->failed_.store(true, std::memory_order_release);
                if (self->cfg_.onInitError) {
                    self->cfg_.onInitError(errmsg);
                }
                // 停掉驱动循环: runMain 返回后 finishMain 会把 running 置 false
                if (self->serverIO_) {
                    self->serverIO_->stop();
                }
                co_return false;
            }
        );
        if (!initOk) {
            finishMain();
            co_return;
        }

        // ---- 登记根 (子代理委派服务 + 节点注册) ----
        if (host_) {
            host_->attachRoot(agent);
        }
    }

    // ---- 就绪 ----
    // 先把调用方状态置好, 再通知客户端: 客户端就绪回调里可能立刻发起操作
    if (cfg_.onServerReady) {
        cfg_.onServerReady();
    }
    // 拉取启动信息须在 init 之后: init 前组件尚未加载, 拉到的清单是空的
    if (cfg_.autoRequestComponents) {
        clientIO_->requestAppendComponentInfo(cfg_.sessionId);
    }
    clientIO_->onServerReady();

    // ---- 会话驱动循环 (取输入 → 跑一轮 → 推事件) ----
    co_await serverIO_->run();
    finishMain();
}

void AgentSessionRuntime::finishMain() {
    // 关掉客户端遗留的交互 (失败挂起中断、冲刷合批), 再关传输
    if (clientIO_) {
        clientIO_->onRuntimeStopping();
        if (auto t = clientIO_->transport()) {
            t->close();
        }
    }
    if (cfg_.onStopped) {
        cfg_.onStopped();
    }
}

void AgentSessionRuntime::stop() {
    if (stopping_.exchange(true)) {
        return;
    }
    // 客户端侧先收: 失败挂起的中断 + 冲刷合批, 然后关传输
    if (clientIO_) {
        clientIO_->onRuntimeStopping();
        if (auto t = clientIO_->transport()) {
            t->close();
        }
    }
    if (serverIO_) {
        serverIO_->stop();
    }
}

bool AgentSessionRuntime::running() const {
    return serverIO_ && serverIO_->running();
}

void AgentSessionRuntime::sendInput(WireUserInput input) {
    if (input.sessionId.empty()) {
        input.sessionId = cfg_.sessionId;
    }
    if (input.requestId == 0) {
        // 服务端只在 requestId > 0 时回受理回执; 自动分配让调用方拿得到结果
        input.requestId = inputRequestId_.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    auto clientIO = clientIO_;
    asio::post(cfg_.clientExecutor, [clientIO, input = std::move(input)]() mutable {
        clientIO->sendToPeer(std::move(input));
    });
}

void AgentSessionRuntime::cancel() {
    auto clientIO = clientIO_;
    auto sid      = cfg_.sessionId;
    asio::post(cfg_.clientExecutor, [clientIO, sid = std::move(sid)]() mutable {
        clientIO->sendToPeer(WireCancel{std::move(sid)});
    });
}

void AgentSessionRuntime::selectModel(std::string modelName) {
    auto clientIO = clientIO_;
    auto sid      = cfg_.sessionId;
    asio::post(
        cfg_.clientExecutor,
        [clientIO, sid = std::move(sid), modelName = std::move(modelName)]() mutable {
            clientIO->sendToPeer(WireSelectModel{std::move(sid), std::move(modelName)});
        }
    );
}

void AgentSessionRuntime::requestContextMessages() {
    auto clientIO = clientIO_;
    auto sid      = cfg_.sessionId;
    asio::post(cfg_.clientExecutor, [clientIO, sid = std::move(sid)]() mutable {
        clientIO->sendToPeer(WireGetContext{std::move(sid)});
    });
}

} // namespace agent
} // namespace agentxx
