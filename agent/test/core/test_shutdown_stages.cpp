/// 分阶段关闭测试 (计划 ARC-5)
///
/// 覆盖:
/// - `TaskScope` 语义: 登记/取消/等待收敛/超时返回 false/累计计数
/// - `BaseAgent::shutdownAsync` 四个阶段: ① 停止受理新输入 (端点回
///   `rejected` + `server_stopped`) ② 后台任务取消收敛 ③ 关闭插件 ④ 会话刷盘
/// - **不等待当前轮次结束**: 模型响应延迟期间调用 shutdown, 它在超时内返回,
///   此时轮次尚未产出结果 (轮次随后自行结束)
#include "agentxx-test/core/test_shutdown_stages.h"

#include "agentxx-test/core/test_agent.h" // 本地 LLM 模拟器

#include "agentxx/agent/base_agent.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/channel_io_transport.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/util/task_scope.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见)
int g_sd_passed = 0;
int g_sd_failed = 0;
} // namespace

#define XX_TEST_PASSED g_sd_passed
#define XX_TEST_FAILED g_sd_failed

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

namespace {

std::string makeTempRoot() {
    auto dir = fs::temp_directory_path()
               / fmt::format(
                   "agentxx_sd_test_{}",
                   std::chrono::steady_clock::now().time_since_epoch().count()
               );
    fs::create_directories(dir);
    return dir.string();
}

void removeTempRoot(const std::string& root) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        std::error_code ec;
        fs::remove_all(utilxx_base::utf8ToPath(root), ec);
        if (!ec) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
}

/// 非阻塞等待 (推进 io 轮转)
asio::awaitable<void> spin(std::chrono::milliseconds total) {
    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer t(ex);
    t.expires_after(total);
    utilxx_base::AsioErrorCode ec;
    co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
}

/// 等待条件成立
asio::awaitable<bool> waitFor(std::function<bool()> cond, int timeoutMs = 8000, int pollMs = 10) {
    auto ex       = co_await asio::this_coro::executor;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMs};
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            co_return true;
        }
        asio::steady_timer t(ex);
        t.expires_after(std::chrono::milliseconds{pollMs});
        utilxx_base::AsioErrorCode ec;
        co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }
    co_return cond();
}

/// 客户端侧记录器 (只关心回执与轮次结果)
class AckRecorder {
public:

    struct Ack {
        uint64_t    requestId = 0;
        std::string status;
        std::string reason;
        std::string detail;
    };

    void handle(agentxx::agent::WireMessage msg) {
        std::visit(
            [this](auto&& m) {
                using T = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<T, agentxx::agent::WireInputAck>) {
                    std::lock_guard<std::mutex> lock(mu_);
                    acks_.push_back(Ack{m.requestId, m.status, m.reason, m.detail});
                } else if constexpr (std::is_same_v<T, agentxx::agent::WireTurnResult>) {
                    turnResults_.fetch_add(1);
                }
            },
            std::move(msg)
        );
    }

    size_t ackCount() {
        std::lock_guard<std::mutex> lock(mu_);
        return acks_.size();
    }

    std::optional<Ack> ackAt(size_t index) {
        std::lock_guard<std::mutex> lock(mu_);
        return (index < acks_.size()) ? std::optional<Ack>{acks_[index]} : std::nullopt;
    }

    int turnResults() const {
        return turnResults_.load();
    }

private:

    std::mutex           mu_;
    std::vector<Ack>     acks_;
    std::atomic<int>     turnResults_{0};
};

/// 夹具: 模拟器 + CodeAgent + 会话端点 + 客户端传输
struct Fixture {
    DaSimServer                                              sim;
    std::shared_ptr<agentxx::agent::AgentConfig>             cfg;
    std::shared_ptr<agentxx::agent::CodeAgent>               agent;
    std::shared_ptr<agentxx::agent::SessionServerAgentIO>    endpoint;
    std::shared_ptr<AckRecorder>                             recorder;
    std::shared_ptr<agentxx::agent::ChannelAgentIOTransport> clientT;
    std::string                                              sessionId;

    ~Fixture() {
        if (clientT) {
            clientT->close();
        }
        if (endpoint) {
            endpoint->stop();
        }
        sim.stop();
    }

    void send(std::string text, uint64_t requestId = 0) {
        agentxx::agent::WireUserInput input;
        input.sessionId = sessionId;
        input.text      = std::move(text);
        input.requestId = requestId;
        clientT->send(agentxx::agent::WireMessage{std::move(input)});
    }
};

asio::awaitable<std::shared_ptr<Fixture>>
    makeFixture(std::string sessionId, std::string dataDir = {}) {
    auto fx                       = std::make_shared<Fixture>();
    fx->sim                       = startDaSimServer();
    g_da_sim_response_content     = "shutdown-test-response";
    g_da_sim_tool_calls           = utilxx_base::Json::array();
    g_da_sim_fail_count           = 0;
    g_da_sim_delay_ms             = 0;
    g_da_sim_tool_calls_remaining = -1;

    fx->cfg                  = std::make_shared<agentxx::agent::AgentConfig>();
    fx->cfg->model.baseUrl   = "http://127.0.0.1:" + std::to_string(fx->sim.port);
    fx->cfg->model.apiKey    = "EMPTY";
    fx->cfg->model.modelName = "default-model";
    fx->cfg->llmMaxRetry     = 1;
    if (!dataDir.empty()) {
        fx->cfg->dataDir            = dataDir;
        fx->cfg->enableSessionStore = true;
    }

    fx->agent = std::make_shared<agentxx::agent::CodeAgent>(fx->cfg);
    co_await fx->agent->init();

    auto ex = co_await asio::this_coro::executor;

    agentxx::agent::SessionServerAgentIO::Config scCfg;
    scCfg.sessionId = sessionId;
    fx->sessionId   = sessionId;
    fx->endpoint    = std::make_shared<agentxx::agent::SessionServerAgentIO>(ex, fx->agent, scCfg);

    auto [clientOwn, serverT] = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);
    fx->endpoint
        ->setTransport(std::shared_ptr<agentxx::agent::AgentIOTransportBase>(std::move(serverT)));

    fx->recorder = std::make_shared<AckRecorder>();
    fx->clientT  = std::move(clientOwn);

    asio::co_spawn(
        ex,
        [clientT = fx->clientT, recorder = fx->recorder]() -> asio::awaitable<void> {
            while (clientT->alive()) {
                auto msg = co_await clientT->recv();
                if (!msg) {
                    break;
                }
                recorder->handle(std::move(*msg));
            }
        },
        asio::detached
    );
    asio::co_spawn(
        ex,
        [ep = fx->endpoint]() -> asio::awaitable<void> { co_await ep->runTransportLoop(); },
        asio::detached
    );
    asio::co_spawn(
        ex,
        [ep = fx->endpoint]() -> asio::awaitable<void> { co_await ep->run(); },
        asio::detached
    );
    co_return fx;
}

} // namespace

// ---------------------------------------------------------------------------
// TaskScope 语义 (同步)
// ---------------------------------------------------------------------------

TestResult testTaskScopeSemantics() {
    const int passedBefore = g_sd_passed;
    const int failedBefore = g_sd_failed;

    asio::io_context io;
    auto             scope = std::make_shared<agentxx::util::TaskScope>(io.get_executor());

    // 立即完成的任务: 完成后登记项被摘除
    bool ran = false;
    scope->spawn("instant", [&ran]() -> asio::awaitable<void> {
        ran = true;
        co_return;
    }());
    XX_TEST_EXPECT_EQ(scope->totalSpawned(), uint64_t{1});
    io.run_for(std::chrono::milliseconds{50});
    io.restart();
    XX_TEST_EXPECT_TRUE(ran);
    XX_TEST_EXPECT_EQ(scope->pending(), size_t{0});

    // 取消: 挂起在长定时器上的任务收到取消信号后结束等待 (协程退出, 登记项摘除)
    bool cancelledObserved = false;
    scope->spawn("wait-cancel", [&cancelledObserved]() -> asio::awaitable<void> {
        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer timer{ex};
        timer.expires_after(std::chrono::seconds{30});
        utilxx_base::AsioErrorCode ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            cancelledObserved = true; // operation_aborted
        }
        co_return;
    }());
    io.run_for(std::chrono::milliseconds{20});
    io.restart();
    XX_TEST_EXPECT_EQ(scope->pending(), size_t{1});
    XX_TEST_EXPECT_EQ(scope->cancelAll(), size_t{1});
    io.run_for(std::chrono::milliseconds{50});
    io.restart();
    XX_TEST_EXPECT_EQ(scope->pending(), size_t{0});
    XX_TEST_EXPECT_TRUE(cancelledObserved);
    XX_TEST_EXPECT_EQ(scope->totalSpawned(), uint64_t{2});

    // 等待收敛: 全部结束返回 true; 超时返回 false
    bool idle = true;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            idle = co_await scope->awaitIdle(std::chrono::milliseconds{500});
            co_return;
        },
        asio::detached
    );
    io.run_for(std::chrono::milliseconds{50});
    io.restart();
    XX_TEST_EXPECT_TRUE(idle);

    // 超时路径: 任务不响应取消 (忽略取消信号, 自己等 200ms 自然结束)
    scope->spawn("stubborn", []() -> asio::awaitable<void> {
        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer timer{ex};
        timer.expires_after(std::chrono::milliseconds{200});
        utilxx_base::AsioErrorCode ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        // 取消后仍继续走完 (演示"取消不改语义"的任务)
        co_return;
    }());
    bool idleQuick = true;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            idleQuick = co_await scope->awaitIdle(std::chrono::milliseconds{20});
            co_return;
        },
        asio::detached
    );
    io.run_for(std::chrono::milliseconds{60});
    io.restart();
    XX_TEST_EXPECT_FALSE(idleQuick);
    // 等它自然结束
    io.run_for(std::chrono::milliseconds{300});
    io.restart();
    XX_TEST_EXPECT_EQ(scope->pending(), size_t{0});
    XX_TEST_EXPECT_TRUE(scope->pendingNames().empty());

    return TestResult{g_sd_passed - passedBefore, g_sd_failed - failedBefore};
}

// ---------------------------------------------------------------------------
// 分阶段关闭 (异步)
// ---------------------------------------------------------------------------

namespace {

/// 用例 1: 停止受理输入 + 刷盘 (持久化开启)
asio::awaitable<void> test_stages_input_stop_and_flush() {
    const auto root = makeTempRoot();
    auto       fx   = co_await makeFixture("shutdown-session", root);

    // 跑一轮, 让会话里有真实消息
    fx->send("hello before shutdown", 1);
    const bool gotTurn = co_await waitFor([&]() { return fx->recorder->turnResults() > 0; }, 10000);
    XX_TEST_EXPECT_TRUE(gotTurn);

    // 执行分阶段关闭
    const auto begin = std::chrono::steady_clock::now();
    const bool closed = co_await fx->agent->shutdownAsync(std::chrono::milliseconds{3000});
    const auto costMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - begin
    )
                            .count();
    XX_TEST_EXPECT_TRUE(closed);
    XX_TEST_EXPECT_TRUE(costMs < 3000);
    XX_TEST_EXPECT_TRUE(fx->agent->agentContext->isShuttingDown());

    // 关闭后新输入被拒 (rejected + server_stopped)
    const size_t acksBefore = fx->recorder->ackCount();
    fx->send("after shutdown", 2);
    const bool gotAck = co_await waitFor([&]() { return fx->recorder->ackCount() > acksBefore; }, 3000);
    XX_TEST_EXPECT_TRUE(gotAck);
    auto ack = fx->recorder->ackAt(acksBefore);
    XX_TEST_EXPECT_HAS_VALUE(ack);
    if (ack.has_value()) {
        XX_TEST_EXPECT_EQ(ack->status, std::string{"rejected"});
        XX_TEST_EXPECT_EQ(ack->reason, std::string{"server_stopped"});
    }

    // 刷盘: 用另一个 SessionStore 打开同一目录, 能读到关闭前写入的消息
    fx->endpoint->stop();
    fx->clientT->close();
    fx->agent.reset();

    auto store  = std::make_shared<agentxx::agent::SessionStore>(root + "/sqlite/sessions", false);
    auto loaded = store->loadSession("shutdown-session");
    XX_TEST_EXPECT_GE(loaded.viewMessages.size(), size_t{2}); // 至少 user + assistant
    bool sawUserText = false;
    for (const auto& msg : loaded.viewMessages) {
        if (msg.text.find("hello before shutdown") != std::string::npos) {
            sawUserText = true;
        }
    }
    XX_TEST_EXPECT_TRUE(sawUserText);
    XX_TEST_EXPECT_FALSE(loaded.llmMessages.is_null() || loaded.llmMessages.empty());

    removeTempRoot(root);
    co_return;
}

/// 用例 2: 不等待当前轮次结束 (模型延迟期间关闭)
asio::awaitable<void> test_shutdown_does_not_wait_turn() {
    auto fx = co_await makeFixture("shutdown-running-turn");
    g_da_sim_delay_ms = 1200; // 模型响应延迟, 保证关闭时轮次仍在跑

    fx->send("slow turn");
    const bool requestArrived
        = co_await waitFor([&]() { return g_da_sim_request_count.load() > 0; }, 3000);
    XX_TEST_EXPECT_TRUE(requestArrived);
    XX_TEST_EXPECT_EQ(fx->recorder->turnResults(), 0);

    const auto begin  = std::chrono::steady_clock::now();
    const bool closed = co_await fx->agent->shutdownAsync(std::chrono::milliseconds{500});
    const auto costMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - begin
    )
                            .count();
    XX_TEST_EXPECT_TRUE(closed);
    // 关闭不等轮次: 返回耗时明显小于模型延迟, 且此刻轮次还没有结果
    XX_TEST_EXPECT_TRUE(costMs < 900);
    XX_TEST_EXPECT_EQ(fx->recorder->turnResults(), 0);

    // 轮次随后自行结束 (关闭不取消它)
    const bool turnFinished
        = co_await waitFor([&]() { return fx->recorder->turnResults() > 0; }, 10000);
    XX_TEST_EXPECT_TRUE(turnFinished);
    g_da_sim_delay_ms = 0;
    co_return;
}

} // namespace

asio::awaitable<TestResult> run_shutdown_stage_tests() {
    const int passedBefore = g_sd_passed;
    const int failedBefore = g_sd_failed;

    co_await test_stages_input_stop_and_flush();
    co_await test_shutdown_does_not_wait_turn();

    co_return TestResult{g_sd_passed - passedBefore, g_sd_failed - failedBefore};
}

} // namespace test
} // namespace agentxx
