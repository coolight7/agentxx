#include "agentxx-test/core/test_agent.h" // 本地 LLM 模拟器 startDaSimServer/g_da_sim_*
#include "agentxx-test/core/test_event_hub.h"
#include "agentxx-test/core/test_event_naming.h"
#include "agentxx-test/test_coro_util.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/session_input_sink.h"
#include "agentxx/event/events.h"
#include "agentxx/event/hub.h"
#include "agentxx/tools/subagent.h"
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
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_es_passed = 0;
int g_es_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_es_passed
#define XX_TEST_FAILED g_es_failed

namespace agentxx {
namespace test {

using namespace utilxx_base;
using Json = utilxx_base::Json;

namespace {

/// 假驱动端: 记录事件投递进来的输入 (异步子代理结果落到会话时用)
class FakeSink : public agentxx::agent::SessionInputSink {
public:

    bool                     busy   = false;
    bool                     accept = true;
    std::vector<std::string> texts;
    std::vector<std::string> sources;

    bool turnActive() const override {
        return busy;
    }

    bool submitTurnInput(std::string_view text, std::string_view source, bool /*unpausePaused*/)
        override {
        if (!accept) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mu);
        texts.emplace_back(text);
        sources.emplace_back(source);
        return true;
    }

    std::string lastText() {
        std::lock_guard<std::mutex> lock(mu);
        return texts.empty() ? std::string{} : texts.back();
    }

    std::string lastSource() {
        std::lock_guard<std::mutex> lock(mu);
        return sources.empty() ? std::string{} : sources.back();
    }

    size_t count() {
        std::lock_guard<std::mutex> lock(mu);
        return texts.size();
    }

private:

    std::mutex mu;
};

/// 异步子代理夹具: 模拟 LLM + AgentHost + 一个根 CodeAgent
///
/// 整个用例的协程都跑在 `io` 上, 由 pumpIoUntil 泵送 (子代理运行期间事件队列
/// 可能为空, io_context::run() 会提前返回)
struct SubagentFixture {
    DaSimServer                                sim;
    std::shared_ptr<asio::io_context>          io;
    std::shared_ptr<agentxx::agent::AgentHost> host;
    std::shared_ptr<agentxx::agent::CodeAgent> agent;
    std::shared_ptr<FakeSink>                  sink;
    std::string                                sessionId = "parent-session";

    ~SubagentFixture() {
        sim.stop();
    }

    agentxx::agent::AgentContext* ctx() const {
        auto c = agent ? agent->getContext() : nullptr;
        return c.get();
    }

    agentxx::events::EventHub* hub() const {
        auto* c = ctx();
        return c && c->eventHub ? c->eventHub.get() : nullptr;
    }
};

/// 驱动 io 直到 done 置位 (5ms 定时器保活, 见 test_agent_host.cpp)
void pumpIoUntil(const std::shared_ptr<asio::io_context>& io, const std::atomic<bool>& done) {
    while (!done.load()) {
        asio::steady_timer t(*io);
        t.expires_after(std::chrono::milliseconds(5));
        t.async_wait([](const utilxx_base::AsioErrorCode&) {});
        io->run();
    }
    io->run();
}

/// 建夹具 + 挂根 (在 io 上完成, 内部自己泵 io)
bool buildFixture(const std::shared_ptr<SubagentFixture>& fx) {
    fx->sim = startDaSimServer();
    g_da_sim_response_content     = "async child result";
    g_da_sim_tool_calls           = Json::array();
    g_da_sim_tool_calls_remaining = -1;
    g_da_sim_delay_ms             = 0;
    g_da_sim_fail_count           = 0;
    g_da_sim_request_count        = 0;

    fx->io = std::make_shared<asio::io_context>();
    auto cfg                 = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl       = "http://127.0.0.1:" + std::to_string(fx->sim.port);
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "test-sim";
    cfg->prompt.systemPrompt = "You are a helpful assistant.";
    cfg->permissionMode      = agentxx::agent::PermissionMode::Pass;
    cfg->llmMaxRetry         = 1;

    auto        io = fx->io;
    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};
    asio::co_spawn(
        *io,
        keepAliveRun([fx, cfg, &done, &ok]() -> asio::awaitable<void> {
            agentxx::agent::AgentHost::Config hostCfg;
            hostCfg.ioCtx = fx->io;
            fx->host      = agentxx::agent::AgentHost::create(hostCfg);
            fx->agent     = std::make_shared<agentxx::agent::CodeAgent>(cfg);
            co_await fx->agent->init();
            fx->host->attachRoot(fx->agent);
            // 父会话 (事件投递与子代理继承总线都要用到)
            auto session   = fx->agent->getContext()->getSession(fx->sessionId);
            fx->sink       = std::make_shared<FakeSink>();
            session->inputSink = fx->sink;
            ok                 = true;
            done               = true;
        }),
        asio::detached
    );
    pumpIoUntil(io, done);
    return ok.load();
}

/// 在 io 上跑一段协程并等它结束 (其余用例都这么用, 避免线程切换)
template<typename Fn>
bool runOnIo(const std::shared_ptr<asio::io_context>& io, Fn fn) {
    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};
    asio::co_spawn(
        *io,
        keepAliveRun([fn, &done, &ok]() mutable -> asio::awaitable<void> {
            ok = co_await fn();
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
asio::awaitable<bool> waitUntil(std::function<bool()> cond, int64_t timeoutMs = 4000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMs};
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            co_return true;
        }
        co_await sleepMs(10);
    }
    co_return cond();
}

bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

/// 异步任务结果事件名 (单一事件名, 靠值里的 `asyncId` 区分任务)
constexpr std::string_view kAsyncDone = "agentxx.agent.async.done";

/// 构造一个异步任务 (目标 subagent 名固定为内置 `subagent_task`)
agentxx::events::SubagentBatchItem asyncTask(std::string resultId, std::string eventName = {}) {
    agentxx::events::SubagentBatchItem task;
    task.subagentName = "subagent_task";
    task.systemPrompt = "You are a worker.";
    task.message      = "do the async work";
    task.resultId     = std::move(resultId);
    task.async        = true;
    task.eventName    = std::move(eventName);
    return task;
}

} // namespace

/// 1. 异步派发立即返回受理结果, 子代理稍后把结果发到事件中枢
asio::awaitable<void> test_async_dispatch_returns_immediately() {
    auto fx = std::make_shared<SubagentFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx));
    auto* hub = fx->hub();
    XX_TEST_EXPECT_TRUE(hub != nullptr);

    // 子代理慢一点: 受理响应必须先回来, 结果后到
    g_da_sim_delay_ms = 300;

    bool                accepted = false;
    Json                acceptedValue;
    int64_t             acceptCostMs = 0;
    std::string         acceptedEvent;
    std::atomic<bool>   valueArrived{false};
    Json                value;

    runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        agentxx::events::ReqSubagentBatch req{
            .parentAgentName = "root",
            .parentSessionId = fx->sessionId,
            .cancelToken     = nullptr,
        };
        req.tasks.push_back(asyncTask("job1"));

        const auto begin = std::chrono::steady_clock::now();
        auto       resp  = co_await fx->agent->getContext()
                         ->bus->request<
                             agentxx::events::ReqSubagentBatch,
                             agentxx::events::RespSubagentBatch>(
                             agentxx::events::Topic::Subagent,
                             std::move(req),
                             std::chrono::seconds(30)
                         );
        acceptCostMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - begin
        )
                           .count();
        if (!resp.has_value() || resp->results.size() != 1) {
            co_return false;
        }
        accepted      = true;
        acceptedEvent = resp->results[0].eventName;
        acceptedValue = Json{
            {"started", resp->results[0].started},
            {"hasError", resp->results[0].hasError},
        };
        // 结果此时还没到 (子代理仍在跑)
        valueArrived = hub->queryEvent(acceptedEvent).has_value();
        co_return true;
    });

    XX_TEST_EXPECT_TRUE(accepted);
    XX_TEST_EXPECT_TRUE(acceptedValue["started"].get<bool>());
    XX_TEST_EXPECT_EQ(acceptedEvent, std::string{kAsyncDone});
    // 受理不等子代理: 300ms 的延迟下必须快速返回
    XX_TEST_EXPECT_TRUE(acceptCostMs < 250);
    XX_TEST_EXPECT_FALSE(valueArrived.load());

    runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        const auto ok = co_await waitUntil([&] {
            auto rec = hub->queryEvent(kAsyncDone);
            return rec.has_value() && !rec->history.empty()
                   && rec->state == agentxx::events::EventState::Ended;
        });
        if (!ok) {
            co_return false;
        }
        auto rec = hub->queryEvent(kAsyncDone);
        value    = rec->history.front();
        co_return true;
    });

    // 值里只有一个标识 `asyncId`, 不再有 `resultId`/`subagent`/`agentId`
    const auto asyncId = value.value("asyncId", std::string{});
    XX_TEST_EXPECT_FALSE(asyncId.empty());
    XX_TEST_EXPECT_EQ(value.value("kind", std::string{}), std::string{"agent"});
    XX_TEST_EXPECT_EQ(value.value("name", std::string{}), std::string{"subagent_task"});
    XX_TEST_EXPECT_FALSE(value.value("hasError", true));
    XX_TEST_EXPECT_FALSE(value.value("canceled", true));
    XX_TEST_EXPECT_EQ(value.value("content", std::string{}), std::string{"async child result"});
    // 任务表里能按 asyncId 查回来, 且 asyncId 就是该任务的 sessionId (一个值, 不另造)
    const auto ledger = fx->host->asyncWorkJson(asyncId);
    XX_TEST_EXPECT_EQ(ledger.value("state", std::string{}), std::string{"done"});
    XX_TEST_EXPECT_EQ(ledger.value("name", std::string{}), std::string{"subagent_task"});
    XX_TEST_EXPECT_TRUE(ledger.value("delivered", false));
    // 任务收尾后不再处于运行态
    XX_TEST_EXPECT_EQ(fx->host->runningAsyncWork(), size_t{0});

    g_da_sim_delay_ms = 0;
    co_return;
}

/// 2. 结果事件把父会话叫醒 (delivery=next-step, 会话空闲 → 新轮输入)
asio::awaitable<void> test_async_result_wakes_session() {
    auto fx = std::make_shared<SubagentFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx));
    auto* hub = fx->hub();
    XX_TEST_EXPECT_TRUE(hub != nullptr);

    // 父会话自己盯这个事件: 子代理结果到达时被唤醒
    auto watched = hub->watch(agentxx::events::EventWaitSpec{
        .event         = std::string{kAsyncDone},
        .mode          = "first",
        .delivery      = "next-step",
        .originSession = fx->sessionId,
        .owner         = "test",
    });
    XX_TEST_EXPECT_TRUE(watched.has_value());

    std::string acceptedEvent;
    runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        agentxx::events::ReqSubagentBatch req{
            .parentAgentName = "root",
            .parentSessionId = fx->sessionId,
            .cancelToken     = nullptr,
        };
        req.tasks.push_back(asyncTask("job2"));
        auto resp = co_await fx->agent->getContext()
                        ->bus->request<
                            agentxx::events::ReqSubagentBatch,
                            agentxx::events::RespSubagentBatch>(
                            agentxx::events::Topic::Subagent,
                            std::move(req),
                            std::chrono::seconds(30)
                        );
        if (!resp.has_value() || resp->results.empty()) {
            co_return false;
        }
        acceptedEvent = resp->results[0].eventName;
        co_return resp->results[0].started;
    });
    XX_TEST_EXPECT_EQ(acceptedEvent, std::string{kAsyncDone});

    runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        co_return co_await waitUntil([&] {
            return fx->sink->count() >= 1;
        });
    });

    const auto text = fx->sink->lastText();
    XX_TEST_EXPECT_TRUE(contains(text, "[Event] " + std::string{kAsyncDone}));
    XX_TEST_EXPECT_TRUE(contains(text, "async child result"));
    XX_TEST_EXPECT_EQ(
        fx->sink->lastSource(),
        std::string{"event:"} + std::string{kAsyncDone}
    );
    XX_TEST_EXPECT_EQ(hub->queryWait(watched->waitId)->state, agentxx::events::WaitState::Satisfied);
    co_return;
}

/// 3. 工具层: 父轮次拿到 `[Async started]` 文本, 结果经事件随之回到会话
/// - 子代理本身已回到同步语义, 后台执行只能经 `agentxx_async_run` (参数与直接调用一致)
asio::awaitable<void> test_async_tool_result_text() {
    auto fx = std::make_shared<SubagentFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx));

    // 模型派发后会盯这个事件 (事件名固定), 结果到达时把会话叫醒
    auto* hub     = fx->hub();
    auto  watched = hub->watch(agentxx::events::EventWaitSpec{
         .event         = std::string{kAsyncDone},
         .mode          = "first",
         .delivery      = "next-step",
         .originSession = fx->sessionId,
         .owner         = "test",
    });
    XX_TEST_EXPECT_TRUE(watched.has_value());

    // 第一次请求让模型发起后台子代理任务, 之后返回纯文本
    g_da_sim_tool_calls_remaining = 1;
    g_da_sim_tool_calls           = Json::array({
        Json{
             {"index", 0},
             {"id", "call_async_1"},
             {"type", "function"},
             {"function",
              Json{
                  {"name", "agentxx_async_run"},
                  {"arguments",
                   R"({"tool":"agentxx_subagent","args":{"subagent":"subagent_task","message":"work"}})"},
              }},
         },
    });

    agentxx::agent::BaseAgent::TurnResult turnResult;
    runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        turnResult = co_await fx->agent->runTurnAsync(fx->sessionId, "dispatch async", nullptr);
        co_return true;
    });

    XX_TEST_EXPECT_FALSE(turnResult.hasError);

    // 工具的返回文本 (模型看到的) 是受理提示, 不是子代理结果
    bool sawStarted = false;
    for (const auto& req : g_da_sim_requests.get()) {
        if (contains(req.dump(), "[Async started]")) {
            sawStarted = true;
        }
    }
    XX_TEST_EXPECT_TRUE(sawStarted);
    XX_TEST_EXPECT_TRUE(contains(g_da_sim_last_request.get().dump(), "agentxx_subagent"));

    // 结果事件随后到达中枢, 会话被叫醒
    runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        co_return co_await waitUntil([&] {
            auto rec = hub->queryEvent(kAsyncDone);
            return rec.has_value() && !rec->history.empty();
        });
    });
    auto rec = hub->queryEvent(kAsyncDone);
    XX_TEST_EXPECT_TRUE(rec.has_value());
    if (rec.has_value() && !rec->history.empty()) {
        XX_TEST_EXPECT_EQ(
            rec->history.front().value("content", std::string{}),
            std::string{"async child result"}
        );
    }
    // 事件把父会话叫醒: 结果要么已作为输入投给驱动端 (轮次已结束), 要么已排成
    // 待注入输入 (任务跑得快、父轮次还在跑 -> next-step 排队)
    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        co_return co_await waitUntil([&] {
            auto session = fx->agent->getContext()->getSession(fx->sessionId);
            return fx->sink->count() >= 1
                   || (session && session->pendingInputCount("event:") >= 1);
        });
    }));
    const auto pending = [&]() -> size_t {
        auto session = fx->agent->getContext()->getSession(fx->sessionId);
        return session ? session->pendingInputCount("event:") : 0;
    };
    XX_TEST_EXPECT_TRUE(fx->sink->count() >= 1 || pending() >= 1);
    if (fx->sink->count() >= 1) {
        XX_TEST_EXPECT_TRUE(
            contains(fx->sink->lastText(), "[Event] " + std::string{kAsyncDone})
        );
    }

    g_da_sim_tool_calls_remaining = -1;
    co_return;
}

/// 4. 单一事件名: 同一批多个任务不再需要各给一个事件名; 非法事件名仍直接报错
asio::awaitable<void> test_async_event_name_errors() {
    auto fx = std::make_shared<SubagentFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx));

    // 两条任务都不给 `event_name`: 都受理 (同一事件名, 靠值里的 asyncId 区分)
    agentxx::events::RespSubagentBatch resp;
    runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        agentxx::events::ReqSubagentBatch req{
            .parentAgentName = "root",
            .parentSessionId = fx->sessionId,
            .cancelToken     = nullptr,
        };
        req.tasks.push_back(asyncTask("dup1"));
        req.tasks.push_back(asyncTask("dup2"));
        auto r = co_await fx->agent->getContext()
                     ->bus->request<
                         agentxx::events::ReqSubagentBatch,
                         agentxx::events::RespSubagentBatch>(
                         agentxx::events::Topic::Subagent,
                         std::move(req),
                         std::chrono::seconds(30)
                     );
        if (r.has_value()) {
            resp = std::move(*r);
        }
        co_return true;
    });

    XX_TEST_EXPECT_EQ(resp.results.size(), size_t{2});
    if (resp.results.size() == 2) {
        XX_TEST_EXPECT_TRUE(resp.results[0].started);
        XX_TEST_EXPECT_TRUE(resp.results[1].started);
        XX_TEST_EXPECT_EQ(resp.results[0].eventName, std::string{kAsyncDone});
        XX_TEST_EXPECT_EQ(resp.results[1].eventName, std::string{kAsyncDone});
    }

    // 非法事件名 (不在事件族里): 直接报错
    agentxx::events::RespSubagentBatch badName;
    runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        agentxx::events::ReqSubagentBatch req{
            .parentAgentName = "root",
            .parentSessionId = fx->sessionId,
            .cancelToken     = nullptr,
        };
        req.tasks.push_back(asyncTask("bad1", "agent.reserved"));
        auto r = co_await fx->agent->getContext()
                     ->bus->request<
                         agentxx::events::ReqSubagentBatch,
                         agentxx::events::RespSubagentBatch>(
                         agentxx::events::Topic::Subagent,
                         std::move(req),
                         std::chrono::seconds(30)
                     );
        if (r.has_value()) {
            badName = std::move(*r);
        }
        co_return true;
    });
    XX_TEST_EXPECT_EQ(badName.results.size(), size_t{1});
    if (!badName.results.empty()) {
        XX_TEST_EXPECT_TRUE(badName.results[0].hasError);
        XX_TEST_EXPECT_TRUE(contains(badName.results[0].errorMessage, "invalid"));
    }
    co_return;
}

/// 6. 取消: 还在跑的异步作业被取消, 父会话仍能收到 {Canceled:true}
asio::awaitable<void> test_async_cancel() {
    auto fx = std::make_shared<SubagentFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx));
    auto* hub = fx->hub();
    XX_TEST_EXPECT_TRUE(hub != nullptr);

    // 子代理慢: 留出取消的时间窗
    g_da_sim_delay_ms = 1200;

    bool started = false;
    runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        agentxx::events::ReqSubagentBatch req{
            .parentAgentName = "root",
            .parentSessionId = fx->sessionId,
            .cancelToken     = nullptr,
        };
        req.tasks.push_back(asyncTask("job6"));
        auto resp = co_await fx->agent->getContext()
                        ->bus->request<
                            agentxx::events::ReqSubagentBatch,
                            agentxx::events::RespSubagentBatch>(
                            agentxx::events::Topic::Subagent,
                            std::move(req),
                            std::chrono::seconds(30)
                        );
        started = resp.has_value() && !resp->results.empty() && resp->results[0].started;
        co_return started;
    });
    XX_TEST_EXPECT_TRUE(started);
    XX_TEST_EXPECT_EQ(fx->host->runningAsyncWork(), size_t{1});

    // agent 关停路径: 按父上下文取消 (不误伤容器里别的根)
    XX_TEST_EXPECT_EQ(fx->host->cancelAsyncWorks(fx->agent->getContext()), size_t{1});

    // 取消后仍要发布终态 (否则父会话只能等到超时)
    bool ok = false;
    runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        ok = co_await waitUntil([&] {
            auto rec = hub->queryEvent(kAsyncDone);
            return rec.has_value() && !rec->history.empty()
                   && rec->state == agentxx::events::EventState::Ended;
        });
        co_return ok;
    });
    XX_TEST_EXPECT_TRUE(ok);
    if (auto rec = hub->queryEvent(kAsyncDone); rec.has_value() && !rec->history.empty()) {
        const auto value = rec->history.front();
        XX_TEST_EXPECT_TRUE(value.value("canceled", false) || value.value("hasError", false));
    }
    XX_TEST_EXPECT_EQ(fx->host->runningAsyncWork(), size_t{0});

    g_da_sim_delay_ms = 0;
    co_return;
}

asio::awaitable<TestResult> run_event_subagent_tests() {
    TestResult result;

    co_await test_async_dispatch_returns_immediately();
    co_await test_async_result_wakes_session();
    co_await test_async_tool_result_text();
    co_await test_async_event_name_errors();
    co_await test_async_cancel();

    result.passed = g_es_passed;
    result.failed = g_es_failed;
    co_return result;
}

} // namespace test
} // namespace agentxx
