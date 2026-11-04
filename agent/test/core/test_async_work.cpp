#include "agentxx-test/core/test_async_work.h"
#include "agentxx-test/core/test_agent.h"
#include "agentxx-test/test_coro_util.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/async_work.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/util/observability.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace {
// 本模块测试计数器 (仅本编译单元可见)
int g_aw_passed = 0;
int g_aw_failed = 0;
} // namespace

#define XX_TEST_PASSED g_aw_passed
#define XX_TEST_FAILED g_aw_failed

namespace agentxx {
namespace test {

using namespace utilxx_base;
using Json = utilxx_base::Json;

namespace {

void pumpIoUntil(const std::shared_ptr<asio::io_context>& io, const std::atomic<bool>& done) {
    while (!done.load()) {
        asio::steady_timer t(*io);
        t.expires_after(std::chrono::milliseconds(5));
        t.async_wait([](const utilxx_base::AsioErrorCode&) {});
        io->run();
    }
    io->run();
}

template<typename Fn>
bool runOnIo(const std::shared_ptr<asio::io_context>& io, Fn fn) {
    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};
    asio::co_spawn(
        *io,
        keepAliveRun([fn, &done, &ok]() mutable -> asio::awaitable<void> {
            ok   = co_await fn();
            done = true;
        }),
        asio::detached
    );
    pumpIoUntil(io, done);
    return ok.load();
}

asio::awaitable<void> sleepMs(int64_t ms) {
    asio::steady_timer timer(co_await asio::this_coro::executor, std::chrono::milliseconds{ms});
    AsioErrorCode      ec;
    co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
}

/// 轮询等待条件成立 (最多 timeoutMs)
asio::awaitable<bool> waitUntil(std::function<bool()> cond, int64_t timeoutMs = 8000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMs};
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            co_return true;
        }
        co_await sleepMs(10);
    }
    co_return cond();
}

/// 异步任务夹具: 模拟 LLM + AgentHost + 一个根 CodeAgent
struct AwFixture {
    DaSimServer                                sim;
    std::shared_ptr<asio::io_context>          io;
    std::shared_ptr<agentxx::agent::AgentHost> host;
    std::shared_ptr<agentxx::agent::CodeAgent> agent;
    std::shared_ptr<agentxx::agent::AgentConfig> cfg;
    std::string                                sessionId = "parent-session";

    ~AwFixture() {
        sim.stop();
    }

    std::shared_ptr<agentxx::agent::AgentContext> ctx() const {
        return agent ? agent->getContext() : nullptr;
    }
};

/// 建夹具 + 挂根 (maxConcurrent = 每队列额度)
bool buildFixture(const std::shared_ptr<AwFixture>& fx, size_t maxConcurrent = 3) {
    fx->sim = startDaSimServer();
    g_da_sim_response_content     = "async work result";
    g_da_sim_tool_calls           = Json::array();
    g_da_sim_tool_calls_remaining = -1;
    g_da_sim_delay_ms             = 0;
    g_da_sim_fail_count           = 0;
    g_da_sim_request_count        = 0;

    fx->io                   = std::make_shared<asio::io_context>();
    fx->cfg                  = std::make_shared<agentxx::agent::AgentConfig>();
    fx->cfg->model.baseUrl   = "http://127.0.0.1:" + std::to_string(fx->sim.port);
    fx->cfg->model.apiKey    = "EMPTY";
    fx->cfg->model.modelName = "test-sim";
    fx->cfg->prompt.systemPrompt = "You are a helpful assistant.";
    fx->cfg->permissionMode      = agentxx::agent::PermissionMode::Pass;
    fx->cfg->llmMaxRetry         = 1;
    fx->cfg->asyncWorkMaxConcurrent = maxConcurrent;

    auto              io = fx->io;
    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};
    asio::co_spawn(
        *io,
        keepAliveRun([fx, &done, &ok]() -> asio::awaitable<void> {
            agentxx::agent::AgentHost::Config hostCfg;
            hostCfg.ioCtx = fx->io;
            fx->host      = agentxx::agent::AgentHost::create(hostCfg);
            fx->agent     = std::make_shared<agentxx::agent::CodeAgent>(fx->cfg);
            co_await fx->agent->init();
            fx->host->attachRoot(fx->agent);
            ok   = true;
            done = true;
        }),
        asio::detached
    );
    pumpIoUntil(io, done);
    return ok.load();
}

/// 一个后台任务提交参数 (目标为内置 `subagent_task`)
agentxx::agent::AsyncWorkSpec makeSpec(
    const std::shared_ptr<AwFixture>&  fx,
    std::string                        name,
    std::string                        queueKey = {},
    std::shared_ptr<agentxx::agent::AgentConfig> base     = nullptr
) {
    agentxx::agent::AsyncWorkSpec spec;
    spec.name             = std::move(name);
    spec.systemPrompt     = "You are a worker.";
    spec.message          = "do the async work";
    spec.creatorSessionId = fx->sessionId;
    spec.queueKey         = std::move(queueKey);
    spec.baseConfig       = base ? std::move(base) : fx->cfg;
    return spec;
}

} // namespace

/// 1. 每队列额度 + FIFO: 额度 2 时同时只跑 2 个, 其余排队按提交顺序起跑
asio::awaitable<void> test_quota_and_fifo() {
    auto fx = std::make_shared<AwFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx, /*maxConcurrent=*/2));

    // 慢一点, 才看得出"同时只跑 2 个"
    g_da_sim_delay_ms = 150;

    std::vector<std::string> ids;
    bool                     allAccepted = true;
    runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        auto ctx = fx->ctx();
        for (int i = 0; i < 4; ++i) {
            auto r = fx->host->submitAsyncWork(makeSpec(fx, "worker"), ctx);
            allAccepted &= r.accepted;
            ids.push_back(r.asyncId);
        }
        // 等前两个起跑, 后两个仍在排队
        co_return co_await waitUntil([&] {
            return fx->host->runningAsyncWork() == 2 && fx->host->queuedAsyncWork() == 2;
        });
    });
    XX_TEST_EXPECT_TRUE(allAccepted);
    XX_TEST_EXPECT_EQ(ids.size(), size_t{4});
    XX_TEST_EXPECT_EQ(fx->host->queuedAsyncWork("root"), size_t{2});

    // 全部跑完
    const bool done = runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        co_return co_await waitUntil([&] {
            return fx->host->runningAsyncWork() == 0 && fx->host->queuedAsyncWork() == 0
                   && fx->host->asyncWorksJson().size() == 4;
        });
    });
    XX_TEST_EXPECT_TRUE(done);

    // FIFO: 起跑时刻按 asyncId 递增 (asyncId 按提交顺序分配)
    const auto list = fx->host->asyncWorksJson({}, {}, true, 50);
    XX_TEST_EXPECT_EQ(list.size(), size_t{4});
    int64_t prevStart = -1;
    int     ordered   = 0;
    if (list.is_array()) {
        for (const auto& item : list) {
            const auto start = item.value("startedAtMs", int64_t{0});
            if (start >= prevStart) {
                ++ordered;
            }
            prevStart = start;
            XX_TEST_EXPECT_EQ(item.value("state", std::string{}), std::string{"done"});
        }
    }
    XX_TEST_EXPECT_EQ(ordered, 4);

    // 指标: 受理/收尾都记到了创建者的关键指标上
    if (auto ctx = fx->ctx(); ctx && ctx->metrics) {
        XX_TEST_EXPECT_EQ(ctx->metrics->asyncWorkSubmitted(), uint64_t{4});
        XX_TEST_EXPECT_EQ(ctx->metrics->asyncWorkFinished(), uint64_t{4});
        XX_TEST_EXPECT_EQ(ctx->metrics->asyncWorkFailed(), uint64_t{0});
        XX_TEST_EXPECT_TRUE(ctx->metrics->asyncWorkQueuedPeak() >= 2);
    }

    g_da_sim_delay_ms = 0;
    co_return;
}

/// 2. 队列之间互不影响: 两条队列各 1 个任务时同时在跑
asio::awaitable<void> test_queues_independent() {
    auto fx = std::make_shared<AwFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx, /*maxConcurrent=*/1));

    g_da_sim_delay_ms = 400;

    // 每条队列额度 1, 但队列键不同 → 两条各起 1 个
    auto queueCfg = std::make_shared<agentxx::agent::AgentConfig>(*fx->cfg);
    queueCfg->asyncWorkMaxConcurrent = 1;

    bool ok = false;
    ok = runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        auto ctx = fx->ctx();
        auto a   = fx->host->submitAsyncWork(makeSpec(fx, "qa-task", "qa", queueCfg), ctx);
        auto b   = fx->host->submitAsyncWork(makeSpec(fx, "qb-task", "qb", queueCfg), ctx);
        if (!a.accepted || !b.accepted) {
            co_return false;
        }
        // 两个都在跑 = 队列之间不互相挡
        co_return co_await waitUntil([&] {
            return fx->host->runningAsyncWork() == 2 && fx->host->queuedAsyncWork() == 0;
        });
    });
    XX_TEST_EXPECT_TRUE(ok);

    const bool finished = runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        co_return co_await waitUntil([&] {
            return fx->host->runningAsyncWork() == 0 && fx->host->asyncWorksJson().size() == 2;
        });
    });
    XX_TEST_EXPECT_TRUE(finished);

    g_da_sim_delay_ms = 0;
    co_return;
}

/// 3. 不限排队条数: 一次提交 200 个全部受理, 取消后队列立刻清空
asio::awaitable<void> test_unbounded_queue() {
    auto fx = std::make_shared<AwFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx, /*maxConcurrent=*/1));

    g_da_sim_delay_ms = 300;

    bool accepted = false;
    runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        auto   ctx = fx->ctx();
        size_t okCount = 0;
        for (int i = 0; i < 200; ++i) {
            auto r = fx->host->submitAsyncWork(makeSpec(fx, "bulk"), ctx);
            if (r.accepted) {
                ++okCount;
            }
        }
        accepted = okCount == 200;
        co_return co_await waitUntil([&] {
            return fx->host->runningAsyncWork() == 1 && fx->host->queuedAsyncWork() == 199;
        });
    });
    XX_TEST_EXPECT_TRUE(accepted);
    XX_TEST_EXPECT_EQ(fx->host->queuedAsyncWork(), size_t{199});
    XX_TEST_EXPECT_EQ(fx->host->asyncWorksJson({}, {}, true, 500).size(), size_t{200});

    // 按创建者整批取消 (agent 关停路径)
    const size_t canceled = fx->host->cancelAsyncWorks(fx->ctx());
    XX_TEST_EXPECT_EQ(canceled, size_t{200});
    XX_TEST_EXPECT_EQ(fx->host->queuedAsyncWork(), size_t{0});

    const bool cleared = runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        co_return co_await waitUntil([&] {
            return fx->host->runningAsyncWork() == 0;
        });
    });
    XX_TEST_EXPECT_TRUE(cleared);

    g_da_sim_delay_ms = 0;
    co_return;
}

/// 4. 台账 linger 清扫: linger=0 时收尾的条目在下一次提交时被清掉
asio::awaitable<void> test_linger_sweep() {
    auto fx = std::make_shared<AwFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx, /*maxConcurrent=*/2));

    auto cfg = std::make_shared<agentxx::agent::AgentConfig>(*fx->cfg);
    cfg->asyncWorkLingerMs = 0;

    bool firstDone = false;
    runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        auto ctx = fx->ctx();
        auto r   = fx->host->submitAsyncWork(makeSpec(fx, "one-shot", {}, cfg), ctx);
        if (!r.accepted) {
            co_return false;
        }
        firstDone = co_await waitUntil([&] {
            const auto ledger = fx->host->asyncWorkJson(r.asyncId);
            return ledger.value("state", std::string{}) == "done";
        });
        co_return firstDone;
    });
    XX_TEST_EXPECT_TRUE(firstDone);
    XX_TEST_EXPECT_EQ(fx->host->asyncWorksJson().size(), size_t{1});

    // linger=0 → 下一次提交触发清扫, 已收尾的旧条目消失
    bool secondSubmitted = false;
    runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        auto ctx = fx->ctx();
        auto r   = fx->host->submitAsyncWork(makeSpec(fx, "next-one", {}, cfg), ctx);
        secondSubmitted = r.accepted;
        co_return true;
    });
    XX_TEST_EXPECT_TRUE(secondSubmitted);
    XX_TEST_EXPECT_EQ(fx->host->asyncWorksJson().size(), size_t{1});
    co_return;
}

/// 5. 任务 agent 的 sessionId 就是 asyncId (一个值, 不另造)
asio::awaitable<void> test_session_id_is_async_id() {
    auto fx = std::make_shared<AwFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx, /*maxConcurrent=*/2));

    g_da_sim_delay_ms = 500;

    std::string asyncId;
    runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        auto r = fx->host->submitAsyncWork(makeSpec(fx, "sessionprobe"), fx->ctx());
        if (!r.accepted) {
            co_return false;
        }
        asyncId = r.asyncId;
        co_return co_await waitUntil([&] {
            return fx->host->asyncWorkJson(asyncId).value("state", std::string{}) == "running";
        });
    });
    XX_TEST_EXPECT_FALSE(asyncId.empty());

    // 运行期节点: agentId 与 sessionId 都在台账/节点里
    const auto ledger  = fx->host->asyncWorkJson(asyncId);
    const auto agentId = ledger.value("agentId", std::string{});
    XX_TEST_EXPECT_EQ(ledger.value("state", std::string{}), std::string{"running"});
    if (!agentId.empty()) {
        auto node = fx->host->registry().get(agentId);
        XX_TEST_EXPECT_TRUE(node != nullptr);
        if (node) {
            XX_TEST_EXPECT_EQ(node->sessionId, asyncId);
            XX_TEST_EXPECT_EQ(node->parentAgentId, std::string{"root"});
        }
    }

    runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        co_return co_await waitUntil([&] {
            return fx->host->runningAsyncWork() == 0;
        });
    });
    g_da_sim_delay_ms = 0;
    co_return;
}

/// 6. 取消: 排队中直接出队; 运行中置令牌, 终态仍落台账
asio::awaitable<void> test_cancel_queued_and_running() {
    auto fx = std::make_shared<AwFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx, /*maxConcurrent=*/1));

    g_da_sim_delay_ms = 800;

    std::string runningId;
    std::string queuedId;
    runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        auto a = fx->host->submitAsyncWork(makeSpec(fx, "first"), fx->ctx());
        auto b = fx->host->submitAsyncWork(makeSpec(fx, "second"), fx->ctx());
        if (!a.accepted || !b.accepted) {
            co_return false;
        }
        runningId = a.asyncId;
        queuedId  = b.asyncId;
        co_return co_await waitUntil([&] {
            return fx->host->asyncWorkJson(runningId).value("state", std::string{})
                       == "running"
                   && fx->host->queuedAsyncWork() == 1;
        });
    });

    // 排队中的: 直接出队并按取消收尾
    XX_TEST_EXPECT_TRUE(fx->host->cancelAsyncWork(queuedId));
    XX_TEST_EXPECT_EQ(
        fx->host->asyncWorkJson(queuedId).value("state", std::string{}),
        std::string{"canceled"}
    );
    // 运行中的: 置取消令牌, 收尾后仍是 canceled
    XX_TEST_EXPECT_TRUE(fx->host->cancelAsyncWork(runningId));

    const bool settled = runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        co_return co_await waitUntil([&] {
            return fx->host->runningAsyncWork() == 0
                   && fx->host->asyncWorkJson(runningId).value("state", std::string{})
                          == "canceled";
        });
    });
    XX_TEST_EXPECT_TRUE(settled);
    // 不存在的 id 返回 false
    XX_TEST_EXPECT_FALSE(fx->host->cancelAsyncWork("async-9999"));

    g_da_sim_delay_ms = 0;
    co_return;
}

/// 7. `kind=tool` 本期未实现: 明确拒绝 (结构已预留)
asio::awaitable<void> test_tool_kind_rejected() {
    auto fx = std::make_shared<AwFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx));

    auto spec   = makeSpec(fx, "as-tool");
    spec.kind   = agentxx::agent::AsyncWorkKind::Tool;
    auto result = fx->host->submitAsyncWork(std::move(spec), fx->ctx());
    XX_TEST_EXPECT_FALSE(result.accepted);
    XX_TEST_EXPECT_TRUE(result.error.find("not supported") != std::string::npos);

    // 参数缺失: message 与 messages 都为空时拒绝
    auto bad = makeSpec(fx, "no-message");
    bad.message.clear();
    auto rejected = fx->host->submitAsyncWork(std::move(bad), fx->ctx());
    XX_TEST_EXPECT_FALSE(rejected.accepted);
    XX_TEST_EXPECT_TRUE(rejected.error.find("message") != std::string::npos);
    co_return;
}

asio::awaitable<TestResult> run_async_work_tests() {
    TestResult result;

    co_await test_quota_and_fifo();
    co_await test_queues_independent();
    co_await test_unbounded_queue();
    co_await test_linger_sweep();
    co_await test_session_id_is_async_id();
    co_await test_cancel_queued_and_running();
    co_await test_tool_kind_rejected();

    result.passed = g_aw_passed;
    result.failed = g_aw_failed;
    co_return result;
}

} // namespace test
} // namespace agentxx
