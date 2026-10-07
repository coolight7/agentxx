#include "agentxx-client/io/jsonl/jsonl_mode.h"

#include "agentxx-client/mode_runners.h"
#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/io/jsonl_io_transport.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/util/exception.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/redirect_error.hpp"
#include "asio/signal_set.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/log.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace agentxx {
namespace client {

namespace {

using agentxx::agent::JsonlAgentIOTransport;
using agentxx::agent::SessionServerAgentIO;

/// stdin EOF 后等待进行中轮次与排队输入跑完的时限
constexpr auto kDrainTimeout = std::chrono::seconds{300};
/// 收到中断信号 (SIGINT/SIGTERM) 后的收尾时限: 用户已经要退出, 不等长轮次跑完
constexpr auto kInterruptDrainTimeout = std::chrono::seconds{5};
/// 断线宽限期取长值: 一次性运行没有"多客户端重连"场景, 输入结束后仍要让进行中的
/// 轮次跑完 (默认 30 秒会把长时间轮次当成断线取消)
constexpr auto kGracePeriod = std::chrono::hours{1};

/// 等待一段时间 (轮询用; 被取消/出错时立即返回)
asio::awaitable<void> sleepFor(asio::any_io_executor ex, std::chrono::milliseconds ms) {
    asio::steady_timer timer(ex);
    timer.expires_after(ms);
    utilxx_base::AsioErrorCode ec;
    co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
}

/// 轮询等待条件成立; 超时返回 false (调用方据此决定是否强制收尾)
asio::awaitable<bool> waitFor(
    asio::any_io_executor    ex,
    const std::function<bool()>& cond,
    std::chrono::milliseconds    timeout,
    std::chrono::milliseconds    poll = std::chrono::milliseconds{50}
) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            co_return true;
        }
        co_await sleepFor(ex, poll);
    }
    co_return cond();
}

/// 一次性运行的实现 (全程在 agent io 线程上)
///
/// 顺序与进程内 TUI/CLI 模式保持一致: 接收循环 → init → 会话驱动循环;
/// 结束顺序为"输入结束 → 等轮次与队列收尾 → 停端点 → 关插件 → 关传输"。
asio::awaitable<void> runJsonlStdioAsync(
    std::shared_ptr<agent::CodeAgent>    agent,
    std::string                          sessionId,
    agent::JsonlAgentIOTransport::LineIo lineIo
) {
    auto ex = co_await asio::this_coro::executor;

    auto interrupted = std::make_shared<std::atomic<bool>>(false);

    // 传输: 读 stdin 行 / 写 stdout 行 (日志仍走日志系统 = stderr)
    auto transport = std::make_shared<JsonlAgentIOTransport>(ex, std::move(lineIo));

    SessionServerAgentIO::Config scCfg;
    scCfg.sessionId  = sessionId;
    scCfg.gracePeriod = kGracePeriod;
    // 一次性运行没有"向上滚动加载更早历史"的界面: 首次同步直接给完整历史
    scCfg.initialSyncTailCount = 0;
    auto serverIO = std::make_shared<SessionServerAgentIO>(ex, agent, scCfg);
    serverIO->setTransport(transport);

    // 接收循环先于 init 启动 (与非分离模式同一顺序): init (MCP/插件/组件加载) 可能
    // 耗时数秒, 期间对端已经能握手, 并处理 hello/get_model 这类不依赖执行引擎的请求
    asio::co_spawn(
        ex,
        [serverIO, transport]() -> asio::awaitable<void> {
            co_await serverIO->runTransportLoop(transport);
            co_return;
        },
        asio::detached
    );

    // 中断信号: 只置标记, 由主流程按"输入结束"路径收尾 (短时限)
    auto signals = std::make_shared<asio::signal_set>(ex, SIGINT, SIGTERM);
    asio::co_spawn(
        ex,
        [signals, interrupted]() -> asio::awaitable<void> {
            utilxx_base::AsioErrorCode ec;
            co_await signals->async_wait(asio::redirect_error(asio::use_awaitable, ec));
            if (!ec) {
                interrupted->store(true, std::memory_order_release);
                XX_LOGW("[jsonl] interrupt signal received, finishing up");
            }
            co_return;
        },
        asio::detached
    );

    co_await agent->init();

    // 子代理委派 (与进程内模式一致): 根 agent 挂到宿主总线上, 使 subagent 工具可用。
    // 宿主对象须活到会话结束, 故用局部 shared_ptr 持有 (与 TUI/CLI 模式同一做法)
    std::shared_ptr<agent::AgentHost> host;
    if (auto ctx = agent->agentContext; ctx && ctx->agentConfig && ctx->agentConfig->enableSubagent) {
        agent::AgentHost::Config hostCfg;
        hostCfg.ioCtx = agent->ioCtx;
        host = agent::AgentHost::create(hostCfg);
        host->attachRoot(agent);
    }

    XX_LOGI(
        "[jsonl] session `{}` ready: write one Wire message per line on stdin; "
        "turn completion is the `turn_result` line (an input ack only means accepted)",
        sessionId
    );

    // 会话驱动循环: 客户端输入 → 轮次 → 结果 (与 TUI/CLI 本地模式同一实现)
    asio::co_spawn(
        ex,
        [serverIO]() -> asio::awaitable<void> {
            co_await serverIO->run();
            co_return;
        },
        asio::detached
    );

    // 等待输入结束: stdin EOF (对端关闭写入端) 或收到中断信号
    while (transport->alive() && !transport->inputEnded() && !interrupted->load()) {
        co_await sleepFor(ex, std::chrono::milliseconds{50});
    }

    // 收尾: 等当前轮次与排队输入跑完; 超时则记日志并强制收尾 (不假装成功)
    const auto drainTimeout
        = interrupted->load() ? kInterruptDrainTimeout : kDrainTimeout;
    const bool drained = co_await waitFor(
        ex,
        [&]() {
            return !serverIO->turnActive() && serverIO->queueSizeForTest() == 0;
        },
        drainTimeout
    );
    if (drained) {
        XX_LOGI("[jsonl] input ended and no pending turn, exiting");
    } else {
        XX_LOGW(
            "[jsonl] drain timeout ({} ms) with work still pending, stopping anyway",
            std::chrono::duration_cast<std::chrono::milliseconds>(drainTimeout).count()
        );
    }

    serverIO->stop();
    (void)co_await waitFor(
        ex,
        [&]() {
            return !serverIO->running();
        },
        std::chrono::seconds{5}
    );
    // 停插件 + 刷盘 (与进程内模式一致; 不等待未完成的轮次, 取消语义维持现状)
    (void)co_await agent->shutdownAsync();
    transport->close();
    signals->cancel();

    // 停止本 io_context: agent 侧可能还挂着 keep-alive 连接池等长驻异步操作,
    // 等它自然空闲会让一次性运行迟迟不退出 (与进程内 TUI/CLI 模式的收尾一致:
    // 清理完成后直接停上下文, 进程随即退出)
    if (auto ctx = agent->ioCtx) {
        ctx->stop();
    }
    co_return;
}

} // namespace

void runJsonlStdio(
    std::shared_ptr<agent::CodeAgent>    agent,
    std::string                          sessionId,
    agent::JsonlAgentIOTransport::LineIo lineIo
) {
    if (!agent || !agent->ioCtx) {
        XX_LOGE("[jsonl] agent is not constructed, cannot start jsonl mode");
        return;
    }
    if (sessionId.empty()) {
        // 对端从 hello_ack 的 sessionId 字段读回该 id (也可一直留空:
        // 服务端把空会话 id 视为"按当前绑定会话处理")
        sessionId = generateUniqueSessionId();
    }
    auto ioCtx = agent->ioCtx;
    asio::co_spawn(
        *ioCtx,
        runJsonlStdioAsync(std::move(agent), std::move(sessionId), std::move(lineIo)),
        asio::detached
    );
    // 单 io_context 运行: 事件都由本进程内协程驱动 (输入端在独立线程读 stdin 并投递)
    ioCtx->run();
}

} // namespace client
} // namespace agentxx
