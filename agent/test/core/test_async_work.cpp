#include "agentxx-test/core/test_async_work.h"
#include "agentxx-test/core/test_agent.h"
#include "agentxx-test/test_coro_util.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/async_work.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/context_stats_wire.h"
#include "agentxx/agent/session_input_sink.h"
#include "agentxx/plugin/tool_registry.h"
#include "agentxx/tools/tool.h"
#include "agentxx/util/tool_result.h"
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
#include <mutex>
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

/// 测试用工具: 按参数睡一段再返回固定文本, 并记下收到的参数 (验证 sessionId 注入)
class AwTestSleepTool final : public agentxx::tools::XXToolBase {
public:

    explicit AwTestSleepTool(std::weak_ptr<agentxx::agent::AgentContext> in_ctx) :
        XXToolBase(
            "aw_test_sleep",
            std::move(in_ctx),
            /*autoSummaryOutput=*/false,
            /*canDelayLoad=*/false,
            /*maxRetry=*/0,
            /*repeatCallCheck=*/false,
            /*supportsParallel=*/false
        ) {}

    neograph::ChatTool get_definition() const override {
        return {
            get_name(),
            "Sleep, then answer with a fixed text.",
            neograph::json::parse(R"({"type":"object","properties":{}})")
        };
    }

    asio::awaitable<agentxx::util::ToolResult>
        execute_async(const utilxx_base::Json& arguments) override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            lastArgs = arguments;
            ++calls;
        }
        const auto sleepMs
            = arguments.is_object() ? arguments.value("sleepMs", int64_t{0}) : int64_t{0};
        if (sleepMs > 0) {
            asio::steady_timer timer(
                co_await asio::this_coro::executor,
                std::chrono::milliseconds{sleepMs}
            );
            AsioErrorCode ec;
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        }
        if (arguments.is_object() && arguments.value("fail", false)) {
            co_return agentxx::util::ToolResult::fail("tool said no");
        }
        co_return agentxx::util::ToolResult::ok("tool says hi");
    }

    utilxx_base::Json lastArgs = utilxx_base::Json::object();
    int               calls    = 0;
    std::mutex        mutex;
};

/// 探针: 模拟"驱动会话的端点" (生产里是 SessionServerAgentIO), 只关心任务表变化通知
class AwProbeSink : public agentxx::agent::SessionInputSink {
public:

    bool turnActive() const override {
        return false;
    }

    bool submitTurnInput(std::string_view, std::string_view, bool) override {
        return true;
    }

    void onAsyncWorksChanged() override {
        notifications.fetch_add(1, std::memory_order_acq_rel);
    }

    std::atomic<int> notifications{0};
};

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

/// 2b. 起跑延时: `delayMs` 期间一直是排队中, 且不挡后面能跑的任务;
///     `startAsyncWorkNow` 能抹掉剩余延时
asio::awaitable<void> test_delay_and_start_now() {
    auto fx = std::make_shared<AwFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx, /*maxConcurrent=*/1));

    bool        accepted = false;
    std::string delayedId;
    std::string plainId;
    bool        stateKeptQueued = false;
    bool        plainRanFirst   = false;
    bool        startedNow      = false;

    runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        auto ctx          = fx->ctx();
        auto delayedSpec  = makeSpec(fx, "delayed-task");
        delayedSpec.delay = std::chrono::milliseconds{30000};
        auto delayed      = fx->host->submitAsyncWork(std::move(delayedSpec), ctx);
        auto plain        = fx->host->submitAsyncWork(makeSpec(fx, "plain-task"), ctx);
        accepted          = delayed.accepted && plain.accepted && delayed.delayMs == 30000;
        delayedId         = delayed.asyncId;
        plainId           = plain.asyncId;
        if (!accepted) {
            co_return false;
        }
        // 延时项不占名额: 普通任务先跑 (额度 1 也不被挡住)
        plainRanFirst = co_await waitUntil([&] {
            return fx->host->asyncWorkJson(plainId).value("state", std::string{}) == "done";
        }, 30000);
        // 此刻延时项仍在排队中
        const auto entry = fx->host->asyncWorkJson(delayedId);
        stateKeptQueued  = entry.value("state", std::string{}) == "queued"
                          && entry.value("delayMs", int64_t{0}) == 30000
                          && entry.value("delayRemainingMs", int64_t{0}) > 0
                          && fx->host->queuedAsyncWork() == 1;
        // 抹掉剩余延时: 立即起跑并完成
        startedNow = fx->host->startAsyncWorkNow(delayedId);
        co_return co_await waitUntil([&] {
            return fx->host->asyncWorkJson(delayedId).value("state", std::string{}) == "done";
        }, 30000);
    });

    XX_TEST_EXPECT_TRUE(accepted);
    XX_TEST_EXPECT_TRUE(plainRanFirst);
    XX_TEST_EXPECT_TRUE(stateKeptQueued);
    XX_TEST_EXPECT_TRUE(startedNow);
    XX_TEST_EXPECT_EQ(
        fx->host->asyncWorkJson(delayedId).value("state", std::string{}),
        std::string{"done"}
    );
    // 已终态的任务不能再"提前开始"
    XX_TEST_EXPECT_FALSE(fx->host->startAsyncWorkNow(delayedId));
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

/// 4. 任务表 linger 清扫: linger=0 时收尾的条目在下一次提交时被清掉
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

    // 运行期节点: agentId 与 sessionId 都在任务表/节点里
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

/// 6. 取消: 排队中直接出队; 运行中置令牌, 终态仍落任务表
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

/// 7. `kind=tool`: 在创建者工具表里执行同名工具, 结果写入任务表/事件
asio::awaitable<void> test_tool_kind_runs() {
    auto fx = std::make_shared<AwFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx));

    auto ctx  = fx->ctx();
    auto tool = std::make_shared<AwTestSleepTool>(ctx);
    XX_TEST_EXPECT_TRUE(ctx->toolRegistry->registerTool(tool->get_name(), tool));

    agentxx::agent::AsyncWorkSpec spec;
    spec.kind             = agentxx::agent::AsyncWorkKind::Tool;
    spec.name             = "sleepy";
    spec.toolName         = "aw_test_sleep";
    spec.toolArgs         = Json{{"sleepMs", 30}};
    spec.creatorSessionId = fx->sessionId;
    spec.baseConfig       = fx->cfg;
    // 工具任务不需要 message: 不写也算受理
    XX_TEST_EXPECT_TRUE(spec.message.empty());

    auto submitted = fx->host->submitAsyncWork(std::move(spec), ctx);
    XX_TEST_EXPECT_TRUE(submitted.accepted);

    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        co_return co_await waitUntil([&] {
            const auto state
                = fx->host->asyncWorkJson(submitted.asyncId).value("state", std::string{});
            return state == "done" || state == "failed" || state == "canceled";
        });
    }));

    const auto entry = fx->host->asyncWorkJson(submitted.asyncId);
    XX_TEST_EXPECT_EQ(entry.value("state", std::string{}), std::string{"done"});
    XX_TEST_EXPECT_EQ(entry.value("kind", std::string{}), std::string{"tool"});
    XX_TEST_EXPECT_EQ(entry.value("name", std::string{}), std::string{"sleepy"});
    XX_TEST_EXPECT_FALSE(entry.value("hasError", true));
    XX_TEST_EXPECT_EQ(entry.value("result", std::string{}), std::string{"tool says hi"});

    // 工具真的被调用了, 且执行层补齐了 sessionId (与前台调用一致)
    {
        std::lock_guard<std::mutex> lock(tool->mutex);
        XX_TEST_EXPECT_EQ(tool->calls, 1);
        XX_TEST_EXPECT_EQ(tool->lastArgs.value("sleepMs", int64_t{0}), int64_t{30});
        XX_TEST_EXPECT_EQ(tool->lastArgs.value("sessionId", std::string{}), fx->sessionId);
    }
    co_return;
}

/// 8. `kind=tool` 的拒绝与失败路径
asio::awaitable<void> test_tool_kind_failures() {
    auto fx = std::make_shared<AwFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx));

    auto ctx  = fx->ctx();
    auto tool = std::make_shared<AwTestSleepTool>(ctx);
    XX_TEST_EXPECT_TRUE(ctx->toolRegistry->registerTool(tool->get_name(), tool));

    // 缺 toolName: 直接拒
    agentxx::agent::AsyncWorkSpec noName;
    noName.kind = agentxx::agent::AsyncWorkKind::Tool;
    auto r1     = fx->host->submitAsyncWork(std::move(noName), ctx);
    XX_TEST_EXPECT_FALSE(r1.accepted);
    XX_TEST_EXPECT_TRUE(r1.error.find("toolName") != std::string::npos);

    // 缺创建者上下文 (工具表在它身上): 直接拒
    agentxx::agent::AsyncWorkSpec noCtx;
    noCtx.kind       = agentxx::agent::AsyncWorkKind::Tool;
    noCtx.toolName   = "aw_test_sleep";
    noCtx.baseConfig = fx->cfg;
    auto r2          = fx->host->submitAsyncWork(std::move(noCtx), nullptr);
    XX_TEST_EXPECT_FALSE(r2.accepted);
    XX_TEST_EXPECT_TRUE(r2.error.find("creator") != std::string::npos);

    // 工具不存在: 受理 (提交时只校形状), 跑起来后按失败收尾
    agentxx::agent::AsyncWorkSpec unknown;
    unknown.kind             = agentxx::agent::AsyncWorkKind::Tool;
    unknown.toolName         = "aw_no_such_tool";
    unknown.creatorSessionId = fx->sessionId;
    unknown.baseConfig       = fx->cfg;
    auto r3                  = fx->host->submitAsyncWork(std::move(unknown), ctx);
    XX_TEST_EXPECT_TRUE(r3.accepted);

    // 工具自己返回失败: 任务表按 failed 记, 正文进 error
    agentxx::agent::AsyncWorkSpec failing;
    failing.kind             = agentxx::agent::AsyncWorkKind::Tool;
    failing.toolName         = "aw_test_sleep";
    failing.toolArgs         = Json{{"fail", true}};
    failing.creatorSessionId = fx->sessionId;
    failing.baseConfig       = fx->cfg;
    auto r4                  = fx->host->submitAsyncWork(std::move(failing), ctx);
    XX_TEST_EXPECT_TRUE(r4.accepted);

    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        auto settled = [&](const std::string& id) {
            const auto state = fx->host->asyncWorkJson(id).value("state", std::string{});
            return state == "done" || state == "failed" || state == "canceled";
        };
        co_return co_await waitUntil([&] { return settled(r3.asyncId) && settled(r4.asyncId); });
    }));

    const auto missing = fx->host->asyncWorkJson(r3.asyncId);
    XX_TEST_EXPECT_EQ(missing.value("state", std::string{}), std::string{"failed"});
    XX_TEST_EXPECT_TRUE(
        missing.value("error", std::string{}).find("not found") != std::string::npos
    );

    const auto failed = fx->host->asyncWorkJson(r4.asyncId);
    XX_TEST_EXPECT_EQ(failed.value("state", std::string{}), std::string{"failed"});
    XX_TEST_EXPECT_TRUE(
        failed.value("error", std::string{}).find("tool said no") != std::string::npos
    );

    // message 为空时仍要拒 (agent 任务那条规则不能被放宽)
    auto bad = makeSpec(fx, "no-message");
    bad.message.clear();
    auto rejected = fx->host->submitAsyncWork(std::move(bad), ctx);
    XX_TEST_EXPECT_FALSE(rejected.accepted);
    XX_TEST_EXPECT_TRUE(rejected.error.find("message") != std::string::npos);
    co_return;
}

/// 9. `kind=tool` 的取消: 慢工具被取消后按 canceled 收尾 (工具本身尽力而为)
asio::awaitable<void> test_tool_kind_cancel() {
    auto fx = std::make_shared<AwFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx));

    auto ctx  = fx->ctx();
    auto tool = std::make_shared<AwTestSleepTool>(ctx);
    XX_TEST_EXPECT_TRUE(ctx->toolRegistry->registerTool(tool->get_name(), tool));

    agentxx::agent::AsyncWorkSpec spec;
    spec.kind             = agentxx::agent::AsyncWorkKind::Tool;
    spec.toolName         = "aw_test_sleep";
    spec.toolArgs         = Json{{"sleepMs", 3000}};
    spec.creatorSessionId = fx->sessionId;
    spec.baseConfig       = fx->cfg;
    auto submitted        = fx->host->submitAsyncWork(std::move(spec), ctx);
    XX_TEST_EXPECT_TRUE(submitted.accepted);

    // 等它真的跑起来再取消 (排队中取消是出队, 走的是另一条分支)
    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        co_return co_await waitUntil([&] {
            return fx->host->asyncWorkJson(submitted.asyncId).value("state", std::string{})
                   == "running";
        });
    }));
    XX_TEST_EXPECT_TRUE(fx->host->cancelAsyncWork(submitted.asyncId));

    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        co_return co_await waitUntil([&] {
            return fx->host->asyncWorkJson(submitted.asyncId).value("state", std::string{})
                   == "canceled";
        });
    }));
    const auto entry = fx->host->asyncWorkJson(submitted.asyncId);
    XX_TEST_EXPECT_TRUE(entry.value("canceled", false));
    XX_TEST_EXPECT_EQ(fx->host->runningAsyncWork(), size_t{0});
    co_return;
}

/// 10. 中断策略: 未显式指定时跟随配置, 显式指定时覆盖配置
asio::awaitable<void> test_interrupt_policy_from_config() {
    auto fx = std::make_shared<AwFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx));

    // 配置 deny: 不指定策略的任务解析为 deny
    fx->cfg->asyncWorkInterruptPolicy = "deny";
    auto fromConfig                   = fx->host->submitAsyncWork(makeSpec(fx, "deny-default"), fx->ctx());
    XX_TEST_EXPECT_TRUE(fromConfig.accepted);
    XX_TEST_EXPECT_EQ(
        fx->host->asyncWorkJson(fromConfig.asyncId).value("interruptPolicy", std::string{}),
        std::string{"deny"}
    );

    // 单次提交显式给 allow: 覆盖配置
    auto explicitSpec            = makeSpec(fx, "allow-explicit");
    explicitSpec.interruptPolicy = agentxx::agent::AsyncInterruptPolicy::Allow;
    auto explicitRes             = fx->host->submitAsyncWork(std::move(explicitSpec), fx->ctx());
    XX_TEST_EXPECT_TRUE(explicitRes.accepted);
    XX_TEST_EXPECT_EQ(
        fx->host->asyncWorkJson(explicitRes.asyncId).value("interruptPolicy", std::string{}),
        std::string{"allow"}
    );

    // 配置改成 allow: 后续未指定的任务跟随
    fx->cfg->asyncWorkInterruptPolicy = "allow";
    auto cfgAllow                     = fx->host->submitAsyncWork(makeSpec(fx, "allow-default"), fx->ctx());
    XX_TEST_EXPECT_TRUE(cfgAllow.accepted);
    XX_TEST_EXPECT_EQ(
        fx->host->asyncWorkJson(cfgAllow.asyncId).value("interruptPolicy", std::string{}),
        std::string{"allow"}
    );

    // 配置里的非法取值: 按默认 bubble (加载期已校验, 这里只保证回退不崩)
    fx->cfg->asyncWorkInterruptPolicy = "bogus";
    auto cfgBogus                     = fx->host->submitAsyncWork(makeSpec(fx, "bogus-default"), fx->ctx());
    XX_TEST_EXPECT_TRUE(cfgBogus.accepted);
    XX_TEST_EXPECT_EQ(
        fx->host->asyncWorkJson(cfgBogus.asyncId).value("interruptPolicy", std::string{}),
        std::string{"bubble"}
    );

    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        co_return co_await waitUntil([&] {
            return fx->host->runningAsyncWork() == 0 && fx->host->queuedAsyncWork() == 0;
        });
    }));
    co_return;
}

/// 9. 固定应答器 (`deny` / `allow`): 权限询问与 HIL 中断都直接作答
/// - `deny`: 权限一律拒绝, HIL 按取消收尾 (handled=false + `__Canceled__`)
/// - `allow`: 权限一律允许, HIL 用空结果恢复 (handled=true + `{}`)
/// - `bubble`: 不注册任何应答器 (询问冒泡到创建者会话)
asio::awaitable<void> test_interrupt_responders() {
    auto io  = std::make_shared<asio::io_context>();
    auto run = [&](std::function<void()> fn) {
        bool ok = runOnIo(io, [&]() -> asio::awaitable<bool> {
            fn();
            co_return true;
        });
        XX_TEST_EXPECT_TRUE(ok);
    };
    (void)run;

    // --- deny ---
    {
        auto bus = std::make_shared<agentxx::events::EventBus>(io->get_executor());
        agentxx::agent::installAsyncInterruptResponders(
            bus,
            agentxx::agent::AsyncInterruptPolicy::Deny
        );
        std::optional<agentxx::events::RespPermission> permResp;
        std::optional<agentxx::events::RespInterrupt>  intResp;
        XX_TEST_EXPECT_TRUE(runOnIo(io, [&]() -> asio::awaitable<bool> {
            auto p = co_await bus->request<agentxx::events::ReqPermission, agentxx::events::RespPermission>(
                agentxx::events::Topic::Permission,
                agentxx::events::ReqPermission{.toolName = "t"},
                std::chrono::milliseconds{2000}
            );
            if (p.has_value()) {
                permResp = *p;
            }
            auto i = co_await bus->request<agentxx::events::ReqInterrupt, agentxx::events::RespInterrupt>(
                agentxx::events::Topic::Interrupt,
                agentxx::events::ReqInterrupt{.interruptNode = "ask"},
                std::chrono::milliseconds{2000}
            );
            if (i.has_value()) {
                intResp = *i;
            }
            co_return true;
        }));
        XX_TEST_EXPECT_TRUE(permResp.has_value());
        if (permResp.has_value()) {
            XX_TEST_EXPECT_TRUE(
                permResp->decision == agentxx::events::RespPermission::Decision::Deny
            );
            XX_TEST_EXPECT_TRUE(permResp->reason.find("deny") != std::string::npos);
            XX_TEST_EXPECT_FALSE(permResp->remember);
            XX_TEST_EXPECT_FALSE(permResp->fullAuth);
        }
        XX_TEST_EXPECT_TRUE(intResp.has_value());
        if (intResp.has_value()) {
            XX_TEST_EXPECT_FALSE(intResp->handled);
            XX_TEST_EXPECT_TRUE(
                intResp->resultJson.find("__Canceled__") != std::string::npos
            );
        }
    }

    // --- allow ---
    {
        auto bus = std::make_shared<agentxx::events::EventBus>(io->get_executor());
        agentxx::agent::installAsyncInterruptResponders(
            bus,
            agentxx::agent::AsyncInterruptPolicy::Allow
        );
        std::optional<agentxx::events::RespPermission> permResp;
        std::optional<agentxx::events::RespInterrupt>  intResp;
        XX_TEST_EXPECT_TRUE(runOnIo(io, [&]() -> asio::awaitable<bool> {
            auto p = co_await bus->request<agentxx::events::ReqPermission, agentxx::events::RespPermission>(
                agentxx::events::Topic::Permission,
                agentxx::events::ReqPermission{.toolName = "t"},
                std::chrono::milliseconds{2000}
            );
            if (p.has_value()) {
                permResp = *p;
            }
            auto i = co_await bus->request<agentxx::events::ReqInterrupt, agentxx::events::RespInterrupt>(
                agentxx::events::Topic::Interrupt,
                agentxx::events::ReqInterrupt{.interruptNode = "ask"},
                std::chrono::milliseconds{2000}
            );
            if (i.has_value()) {
                intResp = *i;
            }
            co_return true;
        }));
        XX_TEST_EXPECT_TRUE(permResp.has_value());
        if (permResp.has_value()) {
            XX_TEST_EXPECT_TRUE(
                permResp->decision == agentxx::events::RespPermission::Decision::Allow
            );
            XX_TEST_EXPECT_TRUE(permResp->reason.find("allow") != std::string::npos);
        }
        XX_TEST_EXPECT_TRUE(intResp.has_value());
        if (intResp.has_value()) {
            XX_TEST_EXPECT_TRUE(intResp->handled);
            XX_TEST_EXPECT_EQ(intResp->resultJson, std::string{"{}"});
        }
    }

    // --- bubble: 不注册应答器, 请求拿不到结果 (询问冒泡到创建者会话) ---
    {
        auto bus        = std::make_shared<agentxx::events::EventBus>(io->get_executor());
        bool noServerOk = false;
        agentxx::agent::installAsyncInterruptResponders(
            bus,
            agentxx::agent::AsyncInterruptPolicy::Bubble
        );
        XX_TEST_EXPECT_TRUE(runOnIo(io, [&]() -> asio::awaitable<bool> {
            auto p = co_await bus->request<agentxx::events::ReqPermission, agentxx::events::RespPermission>(
                agentxx::events::Topic::Permission,
                agentxx::events::ReqPermission{.toolName = "t"},
                std::chrono::milliseconds{500}
            );
            noServerOk = !p.has_value();
            co_return true;
        }));
        XX_TEST_EXPECT_TRUE(noServerOk);
    }

    co_return;
}

/// 10. 会话落盘 (`session_store`): 提交时独占登记存储根+会话, 任务结束后释放
/// - 未给目录且创建者没有 dataDir/sessionStoreDirectory 时明确拒绝
/// - 目录 = 调用方指定的存储根; 会话 id = asyncId (同一根下多个任务互不冲突)
asio::awaitable<void> test_session_store_claim() {
    auto fx = std::make_shared<AwFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx));

    // 未给目录、创建者也没有数据目录: 拒绝 (不能默默不落盘)
    {
        auto spec            = makeSpec(fx, "store-no-dir");
        spec.sessionStore    = true;
        spec.sessionStoreDir.clear();
        auto rejected        = fx->host->submitAsyncWork(std::move(spec), fx->ctx());
        XX_TEST_EXPECT_FALSE(rejected.accepted);
        XX_TEST_EXPECT_TRUE(rejected.error.find("store") != std::string::npos);
    }

    // 指定临时存储根: 提交即占用 (键为 存储根|会话Id), 任务结束后释放
    const auto baseDir = std::filesystem::temp_directory_path()
                         / fmt::format("agentxx_aw_store_{}", static_cast<uint64_t>(::GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(baseDir, ec);
    const std::string root = (baseDir / "agent").string();
    // 保留天数 = 1: 预先放一个 3 天前的任务会话目录, 提交时顺便清掉
    fx->cfg->asyncWorkSessionStoreKeepDays = 1;
    const auto staleDir = std::filesystem::path{root} / "async-stale";
    std::filesystem::create_directories(staleDir, ec);
    const auto staleTime
        = std::filesystem::file_time_type::clock::now() - std::chrono::hours{24 * 3};
    std::filesystem::last_write_time(staleDir, staleTime, ec);
    XX_TEST_EXPECT_TRUE(std::filesystem::is_directory(staleDir));

    std::string asyncId;
    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        auto spec            = makeSpec(fx, "store-job");
        spec.sessionStore    = true;
        spec.sessionStoreDir = root;
        auto r               = fx->host->submitAsyncWork(std::move(spec), fx->ctx());
        asyncId              = r.asyncId;
        co_return r.accepted && !r.asyncId.empty();
    }));
    XX_TEST_EXPECT_FALSE(asyncId.empty());

    const auto ownsSession = [&] {
        const auto occ = fx->host->sessionOccupancyJson();
        for (const auto& [key, value] : occ.items()) {
            if (key.find(asyncId) != std::string::npos) {
                return value.is_string()
                       && value.get<std::string>().find(asyncId) != std::string::npos;
            }
        }
        return false;
    };
    XX_TEST_EXPECT_TRUE(ownsSession());

    // 等任务结束: 占用释放 (磁盘上的会话目录保留, 供事后回看)
    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        co_return co_await waitUntil([&] {
            return fx->host->runningAsyncWork() == 0 && fx->host->queuedAsyncWork() == 0;
        });
    }));
    XX_TEST_EXPECT_FALSE(ownsSession());
    // 过期的任务会话目录已被清掉 (保留天数 1, 那个目录是 3 天前的)
    XX_TEST_EXPECT_FALSE(std::filesystem::exists(staleDir));
    XX_TEST_EXPECT_EQ(
        fx->host->asyncWorkJson(asyncId).value("state", std::string{}),
        std::string{"done"}
    );

    std::filesystem::remove_all(baseDir, ec);
    co_return;
}

/// 11. 创建者已销毁: 只靠提交快照继续跑完, 结果只落任务表 (无中枢可投递)
/// - `creator` 传空 + 快照里带配置: 仍能派生并跑完
/// - 没有创建者上下文就没有中枢: `delivered=false`, 正文仍可在任务表查到
asio::awaitable<void> test_creator_gone_still_runs() {
    auto fx = std::make_shared<AwFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx));

    auto snapshot = std::make_shared<agentxx::agent::AgentConfig>(*fx->cfg);
    std::string asyncId;
    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        auto spec             = makeSpec(fx, "orphan-job", {}, snapshot);
        spec.creatorSessionId = "gone-session";
        auto r                = fx->host->submitAsyncWork(std::move(spec), nullptr);
        asyncId               = r.asyncId;
        co_return r.accepted;
    }));
    XX_TEST_EXPECT_FALSE(asyncId.empty());

    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        co_return co_await waitUntil([&] {
            const auto state
                = fx->host->asyncWorkJson(asyncId).value("state", std::string{});
            return state == "done" || state == "failed" || state == "canceled";
        });
    }));
    const auto entry = fx->host->asyncWorkJson(asyncId);
    XX_TEST_EXPECT_EQ(entry.value("state", std::string{}), std::string{"done"});
    XX_TEST_EXPECT_FALSE(entry.value("delivered", true));
    XX_TEST_EXPECT_TRUE(entry.value("result", std::string{}).find("async work result") != std::string::npos);
    // 启动用的会话 id 就是 asyncId (不另造)
    co_return;
}

/// 12. 任务表变化通知驱动端点 (界面「后台任务」段靠它刷新)
///
/// - 提交 (入队) / 起跑 / 终态 / 排队中被取消都经
///   `SessionInputSink::onAsyncWorksChanged` 通知创建者会话的端点
/// - 组装下发条目时运行中的排在已完成的之前 (界面按状态分组, 上限 10 条也不会
///   被已完成的条目挤掉)
/// - 端点已解绑 (会话切走 / 端点停止) 后不再通知
asio::awaitable<void> test_works_changed_notification() {
    auto fx = std::make_shared<AwFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx, /*maxConcurrent=*/1));

    auto sink = std::make_shared<AwProbeSink>();
    auto ctx  = fx->ctx();
    XX_TEST_EXPECT_TRUE(ctx != nullptr && ctx->sessions != nullptr);
    if (!ctx || !ctx->sessions) {
        co_return;
    }
    // 端点登记为这个会话的驱动者 (生产里由 SessionServerAgentIO 在 run() 时挂上)
    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        auto session = ctx->sessions->getOrCreate(fx->sessionId);
        if (!session) {
            co_return false;
        }
        session->inputSink = sink;
        co_return true;
    }));

    g_da_sim_delay_ms = 250;

    // 额度 1 提交两个: 第一个起跑, 第二个排队; 端点至少被通知一次
    std::vector<std::string> ids;
    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        for (int i = 0; i < 2; ++i) {
            auto r = fx->host->submitAsyncWork(makeSpec(fx, "notify-job"), ctx);
            if (!r.accepted) {
                co_return false;
            }
            ids.push_back(r.asyncId);
        }
        co_return co_await waitUntil([&] {
            return fx->host->runningAsyncWork() == 1 && fx->host->queuedAsyncWork() == 1
                   && sink->notifications.load() > 0;
        });
    }));
    XX_TEST_EXPECT_EQ(ids.size(), size_t{2});

    // 下发的条目: 运行中的排在最前 (已完成的 linger 条目不能把运行中的挤掉)
    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        auto session = ctx->sessions->get(fx->sessionId);
        if (!session) {
            co_return false;
        }
        const auto stats
            = agentxx::agent::io::makeWireContextStats(ctx, fx->sessionId, *session->contextStats);
        co_return !stats.asyncWorks.empty() && stats.asyncWorks.front().state == "running";
    }));

    // 排队中被取消: 也是一次状态变化 (这条没入过运行态, 终态推送只在这里发)
    const int beforeCancel = sink->notifications.load();
    XX_TEST_EXPECT_TRUE(fx->host->cancelAsyncWork(ids[1]));
    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        co_return co_await waitUntil(
            [&] {
                return sink->notifications.load() > beforeCancel;
            },
            3000
        );
    }));

    // 终态同样通知: 提交×2 + 起跑×1 + 取消×1 + 终态×1 = 5 次 (至少 4 次)
    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        co_return co_await waitUntil(
            [&] {
                return fx->host->runningAsyncWork() == 0 && fx->host->queuedAsyncWork() == 0
                       && sink->notifications.load() >= 4;
            },
            8000
        );
    }));

    // 端点解绑 (会话切走 / 端点停止) 后不再通知
    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        auto session = ctx->sessions->get(fx->sessionId);
        if (!session) {
            co_return false;
        }
        session->inputSink.reset();
        const int before = sink->notifications.load();
        auto      r      = fx->host->submitAsyncWork(makeSpec(fx, "silent-job"), ctx);
        if (!r.accepted) {
            co_return false;
        }
        const bool idle = co_await waitUntil(
            [&] {
                return fx->host->runningAsyncWork() == 0 && fx->host->queuedAsyncWork() == 0;
            },
            8000
        );
        co_return idle && sink->notifications.load() == before;
    }));

    g_da_sim_delay_ms = 0;
    co_return;
}

asio::awaitable<TestResult> run_async_work_tests() {
    TestResult result;

    co_await test_quota_and_fifo();
    co_await test_delay_and_start_now();
    co_await test_queues_independent();
    co_await test_unbounded_queue();
    co_await test_linger_sweep();
    co_await test_session_id_is_async_id();
    co_await test_cancel_queued_and_running();
    co_await test_tool_kind_runs();
    co_await test_tool_kind_failures();
    co_await test_tool_kind_cancel();
    co_await test_interrupt_policy_from_config();
    co_await test_interrupt_responders();
    co_await test_session_store_claim();
    co_await test_creator_gone_still_runs();
    co_await test_works_changed_notification();

    result.passed = g_aw_passed;
    result.failed = g_aw_failed;
    co_return result;
}

} // namespace test
} // namespace agentxx
