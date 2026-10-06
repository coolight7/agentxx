#include "agentxx-test/core/test_session_sync.h"
#include "agentxx-test/core/test_agent.h" // 本地 LLM 模拟器 startDaSimServer/g_da_sim_*

#include "agentxx/agent/base_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/channel_io_transport.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
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
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_sy_passed = 0;
int g_sy_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_sy_passed
#define XX_TEST_FAILED g_sy_failed

namespace agentxx {
namespace test {

using namespace utilxx_base;
namespace fs = std::filesystem;

namespace {

/// 客户端收到的消息记录 (由测试协程从传输层读入)
struct Recorder {
    std::mutex                                          mu;
    std::vector<agentxx::agent::WireSyncPayload>        syncs;
    std::vector<agentxx::agent::WireHelloAck>           acks;
    std::atomic<int>                                    turnResults{0};

    void handle(agentxx::agent::WireMessage msg) {
        std::visit(
            [this](auto&& m) {
                using T = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<T, agentxx::agent::WireSyncPayload>) {
                    std::lock_guard<std::mutex> lock(mu);
                    syncs.push_back(m);
                } else if constexpr (std::is_same_v<T, agentxx::agent::WireHelloAck>) {
                    std::lock_guard<std::mutex> lock(mu);
                    acks.push_back(m);
                } else if constexpr (std::is_same_v<T, agentxx::agent::WireTurnResult>) {
                    turnResults.fetch_add(1);
                }
            },
            std::move(msg)
        );
    }

    size_t syncCount() {
        std::lock_guard<std::mutex> lock(mu);
        return syncs.size();
    }

    std::optional<agentxx::agent::WireSyncPayload> sync(size_t index) {
        std::lock_guard<std::mutex> lock(mu);
        return index < syncs.size() ? std::optional<agentxx::agent::WireSyncPayload>{syncs[index]}
                                    : std::nullopt;
    }
};

/// 测试夹具: 模拟器 + BaseAgent + 会话端点 + 客户端传输
struct Fixture {
    DaSimServer                                              sim;
    std::shared_ptr<agentxx::agent::AgentConfig>             cfg;
    std::shared_ptr<agentxx::agent::BaseAgent>               agent;
    std::shared_ptr<agentxx::agent::SessionServerAgentIO>    endpoint;
    std::shared_ptr<Recorder>                                recorder;
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

    void sendInput(std::string text) {
        agentxx::agent::WireUserInput input;
        input.sessionId = sessionId;
        input.text      = std::move(text);
        clientT->send(agentxx::agent::WireMessage{std::move(input)});
    }

    /// 发送握手 (afterViewSeq = 客户端已持有的展示历史序号)
    void sendHello(uint64_t afterViewSeq, uint64_t lastSeq = 0) {
        agentxx::agent::WireHello hello;
        hello.sessionId     = sessionId;
        hello.lastSeq       = lastSeq;
        hello.afterViewSeq  = afterViewSeq;
        hello.language      = "en";
        clientT->send(agentxx::agent::WireMessage{std::move(hello)});
    }

    uint64_t lastViewSeq() {
        auto session = agent->agentContext->sessions->get(sessionId);
        return session ? session->lastViewSeq() : 0;
    }
};

asio::awaitable<bool> waitFor(std::function<bool()> cond, int timeoutMs = 8000, int pollMs = 15) {
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

std::string makeTempRoot() {
    auto dir = fs::temp_directory_path()
               / fmt::format(
                   "agentxx_sy_test_{}",
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

/// 创建夹具 (dataDir 非空时开启会话持久化)
asio::awaitable<std::shared_ptr<Fixture>> makeFixture(
    std::string sessionId,
    std::string dataDir = {},
    size_t      tailCount = 0
) {
    auto fx                   = std::make_shared<Fixture>();
    fx->sim                   = startDaSimServer();
    g_da_sim_response_content = "sync test response";
    g_da_sim_tool_calls       = utilxx_base::Json::array();
    g_da_sim_fail_count       = 0;
    g_da_sim_delay_ms         = 0;

    const auto baseUrl       = "http://127.0.0.1:" + std::to_string(fx->sim.port);
    fx->cfg                  = std::make_shared<agentxx::agent::AgentConfig>();
    fx->cfg->model.baseUrl   = baseUrl;
    fx->cfg->model.apiKey    = "EMPTY";
    fx->cfg->model.modelName = "default-model";
    fx->cfg->llmMaxRetry     = 1;
    if (!dataDir.empty()) {
        fx->cfg->dataDir            = dataDir;
        fx->cfg->enableSessionStore = true;
    }

    fx->agent = std::make_shared<agentxx::agent::BaseAgent>(fx->cfg);
    co_await fx->agent->init();

    auto ex = co_await asio::this_coro::executor;

    agentxx::agent::SessionServerAgentIO::Config scCfg;
    scCfg.sessionId             = sessionId;
    scCfg.initialSyncTailCount  = tailCount;
    fx->sessionId               = sessionId;
    fx->endpoint = std::make_shared<agentxx::agent::SessionServerAgentIO>(ex, fx->agent, scCfg);

    auto [clientOwn, serverT] = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);
    fx->endpoint
        ->setTransport(std::shared_ptr<agentxx::agent::AgentIOTransportBase>(std::move(serverT)));

    fx->recorder = std::make_shared<Recorder>();
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
        [ep = fx->endpoint]() -> asio::awaitable<void> {
            co_await ep->runTransportLoop();
        },
        asio::detached
    );
    asio::co_spawn(
        ex,
        [ep = fx->endpoint]() -> asio::awaitable<void> {
            co_await ep->run();
        },
        asio::detached
    );
    co_return fx;
}

/// 跑一轮会话 (等到轮次结果), 保证展示历史有新消息
asio::awaitable<void> runOneTurn(const std::shared_ptr<Fixture>& fx, std::string text) {
    const int before = fx->recorder->turnResults.load();
    fx->sendInput(std::move(text));
    co_await waitFor([&] {
        return fx->recorder->turnResults.load() > before;
    });
}

// ---------------------------------------------------------------------------
// 1. 增量补拉: 序号在服务端历史范围内只补差量 (计划 STO-4)
// ---------------------------------------------------------------------------

asio::awaitable<void> test_incremental_replay() {
    const auto root = makeTempRoot();
    auto       fx   = co_await makeFixture("sync-inc-session", root);

    co_await runOneTurn(fx, "first turn");
    const uint64_t seqAfterTurn = fx->lastViewSeq();
    XX_TEST_EXPECT_TRUE(seqAfterTurn > 0);

    // ① 客户端持有 seqAfterTurn-1: 只补最后一条
    fx->sendHello(seqAfterTurn - 1);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->syncCount() >= 1;
    }));
    if (auto sync = fx->recorder->sync(0)) {
        XX_TEST_EXPECT_TRUE(sync->incremental);
        XX_TEST_EXPECT_EQ(sync->messages.size(), size_t{1});
        XX_TEST_EXPECT_EQ(sync->lastViewSeq, seqAfterTurn);
        if (!sync->messages.empty()) {
            XX_TEST_EXPECT_FALSE(sync->messages[0].id.empty());
        }
    }

    // ② 客户端已是最新: 空增量 (仍标记 incremental, 客户端保持本地历史)
    fx->sendHello(seqAfterTurn);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->syncCount() >= 2;
    }));
    if (auto sync = fx->recorder->sync(1)) {
        XX_TEST_EXPECT_TRUE(sync->incremental);
        XX_TEST_EXPECT_EQ(sync->messages.size(), size_t{0});
    }

    // ③ 序号超前 (另一份历史): 回退常规同步 (整体替换语义)
    fx->sendHello(seqAfterTurn + 5);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->syncCount() >= 3;
    }));
    if (auto sync = fx->recorder->sync(2)) {
        XX_TEST_EXPECT_FALSE(sync->incremental);
        XX_TEST_EXPECT_TRUE(!sync->messages.empty());
    }

    // ④ 未提供序号: 常规同步 (旧客户端行为不变)
    fx->sendHello(0);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->syncCount() >= 4;
    }));
    if (auto sync = fx->recorder->sync(3)) {
        XX_TEST_EXPECT_FALSE(sync->incremental);
    }

    // ⑤ delta 序号有效时优先 delta 重放 (更省): 不再下发 Sync 快照
    const size_t syncsBeforeDelta = fx->recorder->syncCount();
    fx->sendHello(fx->lastViewSeq(), fx->lastViewSeq());
    co_await waitFor([&] {
        return fx->recorder->syncCount() > syncsBeforeDelta;
    }, 300);
    XX_TEST_EXPECT_EQ(fx->recorder->syncCount(), syncsBeforeDelta);

    removeTempRoot(root);
}

// ---------------------------------------------------------------------------
// 2. 记忆模式回退: 无会话库 (内存模式) 时不走增量补拉
// ---------------------------------------------------------------------------

asio::awaitable<void> test_incremental_requires_store() {
    auto fx = co_await makeFixture("sync-memory-session");
    co_await runOneTurn(fx, "memory turn");
    const uint64_t seq = fx->lastViewSeq();
    XX_TEST_EXPECT_TRUE(seq > 0);

    fx->sendHello(seq > 0 ? seq - 1 : 0);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->syncCount() >= 1;
    }));
    if (auto sync = fx->recorder->sync(0)) {
        // 内存模式无持久化序号: 回退常规同步 (整体替换)
        XX_TEST_EXPECT_FALSE(sync->incremental);
    }
}

// ---------------------------------------------------------------------------
// 3. 重启后增量补拉: 序号跨进程 (会话重建) 仍然有效
// ---------------------------------------------------------------------------

asio::awaitable<void> test_incremental_after_restart() {
    const auto        root = makeTempRoot();
    const std::string sid  = "sync-restart-session";
    uint64_t          seqAfterFirst = 0;
    {
        auto fx = co_await makeFixture(sid, root);
        co_await runOneTurn(fx, "before restart");
        seqAfterFirst = fx->lastViewSeq();
        XX_TEST_EXPECT_TRUE(seqAfterFirst > 0);
    }
    {
        // 新端点 (= 重启): 会话从库内恢复, 序号继续
        auto fx = co_await makeFixture(sid, root);
        // 会话加载是异步预热: 等它完成后再断言 (等价"重启后首次读取")
        XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
            return fx->lastViewSeq() == seqAfterFirst;
        }));

        // 已持有全部历史: 空增量
        fx->sendHello(seqAfterFirst);
        XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
            return fx->recorder->syncCount() >= 1;
        }));
        if (auto sync = fx->recorder->sync(0)) {
            XX_TEST_EXPECT_TRUE(sync->incremental);
            XX_TEST_EXPECT_EQ(sync->messages.size(), size_t{0});
            XX_TEST_EXPECT_EQ(sync->lastViewSeq, seqAfterFirst);
        }

        // 再跑一轮后补拉: 只回补新消息
        co_await runOneTurn(fx, "after restart");
        const uint64_t seqAfterSecond = fx->lastViewSeq();
        XX_TEST_EXPECT_TRUE(seqAfterSecond > seqAfterFirst);
        fx->sendHello(seqAfterFirst);
        XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
            return fx->recorder->syncCount() >= 2;
        }));
        if (auto sync = fx->recorder->sync(1)) {
            XX_TEST_EXPECT_TRUE(sync->incremental);
            XX_TEST_EXPECT_EQ(sync->lastViewSeq, seqAfterSecond);
            XX_TEST_EXPECT_TRUE(!sync->messages.empty());
            // 补拉内容只含序号更大的消息 (不含客户端已持有的部分)
            bool onlyNew = true;
            for (const auto& m : sync->messages) {
                if (m.text.find("before restart") != std::string::npos) {
                    onlyNew = false;
                }
            }
            XX_TEST_EXPECT_TRUE(onlyNew);
        }
    }
    removeTempRoot(root);
}

} // namespace

asio::awaitable<TestResult> run_session_sync_tests() {
    co_await test_incremental_replay();
    co_await test_incremental_requires_store();
    co_await test_incremental_after_restart();
    co_return TestResult{g_sy_passed, g_sy_failed};
}

} // namespace test
} // namespace agentxx
