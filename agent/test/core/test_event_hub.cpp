#include "agentxx-test/core/test_event_hub.h"

#include "agentxx/agent/context.h"
#include "agentxx/agent/session_input_sink.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/event/hub.h"
#include "neograph/graph/cancel.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "utilxx_base/asio_error.h"
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_eh_passed = 0;
int g_eh_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_eh_passed
#define XX_TEST_FAILED g_eh_failed

namespace agentxx {
namespace test {

using namespace utilxx_base;
using agentxx::events::DeliverResult;
using agentxx::events::EventDefineSpec;
using agentxx::events::EventDelivery;
using agentxx::events::EventHub;
using agentxx::events::EventKind;
using agentxx::events::EventState;
using agentxx::events::EventWaitSpec;
using agentxx::events::WaitState;
using Json = utilxx_base::Json;

namespace {

/// 假驱动端: 记录投递进来的输入, 可切换"轮次在跑"
class FakeSink : public agentxx::agent::SessionInputSink {
public:

    bool                     busy   = false;
    bool                     accept = true;
    std::vector<std::string> texts;
    std::vector<std::string> sources;
    std::vector<bool>        unpaused;

    bool turnActive() const override {
        return busy;
    }

    bool submitTurnInput(std::string_view text, std::string_view source, bool unpausePaused)
        override {
        if (!accept) {
            return false;
        }
        texts.emplace_back(text);
        sources.emplace_back(source);
        unpaused.push_back(unpausePaused);
        return true;
    }
};

/// 测试夹具: AgentContext + 一个会话 + 假驱动端 + 事件中枢
struct HubFixture {
    std::shared_ptr<agentxx::agent::AgentContext> ctx;
    std::shared_ptr<agentxx::agent::Session>      session;
    std::shared_ptr<FakeSink>                     sink;
    std::shared_ptr<EventHub>                     hub;
    std::string                                   sessionId = "s1";
};

asio::awaitable<std::shared_ptr<HubFixture>> makeFixture(EventHub::Config cfg = {}) {
    auto fx          = std::make_shared<HubFixture>();
    auto ex          = co_await asio::this_coro::executor;
    fx->ctx          = std::make_shared<agentxx::agent::AgentContext>();
    fx->ctx->agentConfig = std::make_shared<agentxx::agent::AgentConfig>();
    fx->session      = fx->ctx->sessions->getOrCreate(fx->sessionId);
    fx->sink         = std::make_shared<FakeSink>();
    fx->session->inputSink = fx->sink;
    fx->hub = std::make_shared<EventHub>(ex, fx->ctx, std::move(cfg));
    co_return fx;
}

asio::awaitable<void> sleepMs(int64_t ms) {
    asio::steady_timer timer(co_await asio::this_coro::executor, std::chrono::milliseconds{ms});
    AsioErrorCode      ec;
    co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
}

/// 轮询等待条件成立 (最多 timeoutMs)
asio::awaitable<bool> waitUntil(std::function<bool()> cond, int64_t timeoutMs = 3000) {
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

/// 一次性取走会话里待注入的事件输入正文
std::vector<std::string> takeEventInputs(const std::shared_ptr<agentxx::agent::Session>& session) {
    std::vector<std::string> out;
    if (!session) {
        return out;
    }
    for (auto& input : session->takePendingInputs(agentxx::agent::InputDelivery::NextStep)) {
        out.push_back(std::move(input.text));
    }
    return out;
}

} // namespace

/// 1. 记录状态机: 流式多值 + end + 幂等 + end 后再来值重新打开
asio::awaitable<void> test_hub_record_statemachine() {
    auto fx = co_await makeFixture();
    auto& hub = *fx->hub;

    EventDefineSpec spec;
    spec.name         = "build.done";
    spec.kind         = EventKind::Stream;
    spec.owner        = "test";
    spec.description  = "build finished";
    spec.historyLimit = 2;
    std::string err;
    XX_TEST_EXPECT_TRUE(hub.define(spec, &err));

    XX_TEST_EXPECT_TRUE(hub.eventCount() == 1);

    XX_TEST_EXPECT_TRUE(hub.publishValue("build.done", Json{{"ok", true}}, &err));
    {
        auto rec = hub.queryEvent("build.done");
        XX_TEST_EXPECT_TRUE(rec.has_value());
        XX_TEST_EXPECT_TRUE(rec->published == 1);
        XX_TEST_EXPECT_TRUE(rec->history.size() == 1);
        XX_TEST_EXPECT_TRUE(rec->state == EventState::Open);
        XX_TEST_EXPECT_TRUE(rec->description == "build finished");
        XX_TEST_EXPECT_TRUE(rec->owner == "test");
    }

    XX_TEST_EXPECT_TRUE(hub.publishValue("build.done", Json(2), &err));
    XX_TEST_EXPECT_TRUE(hub.publishValue("build.done", Json(3), &err));
    {
        auto rec = hub.queryEvent("build.done");
        // historyLimit=2: 最旧被丢, 计数可见
        XX_TEST_EXPECT_TRUE(rec->history.size() == 2);
        XX_TEST_EXPECT_TRUE(rec->droppedForHistory == 1);
        XX_TEST_EXPECT_TRUE(rec->published == 3);
    }

    XX_TEST_EXPECT_TRUE(hub.endEvent("build.done", Json{{"summary", "done"}}, &err));
    {
        auto rec = hub.queryEvent("build.done");
        XX_TEST_EXPECT_TRUE(rec->state == EventState::Ended);
        XX_TEST_EXPECT_TRUE(rec->endedAtMs > 0);
    }
    // 幂等: 第二次 end 是 no-op
    XX_TEST_EXPECT_TRUE(hub.endEvent("build.done", Json{}, &err));
    // 未知事件: 明确报错
    XX_TEST_EXPECT_FALSE(hub.endEvent("no.such.event", Json{}, &err));
    XX_TEST_EXPECT_TRUE(contains(err, "not found"));

    // end 之后再来的值把记录重新打开 (流式事件允许续上)
    XX_TEST_EXPECT_TRUE(hub.publishValue("build.done", Json(4), &err));
    XX_TEST_EXPECT_TRUE(hub.queryEvent("build.done")->state == EventState::Open);

    XX_TEST_EXPECT_TRUE(hub.removeEvent("build.done"));
    XX_TEST_EXPECT_FALSE(hub.queryEvent("build.done").has_value());
    co_return;
}

/// 2. 单值事件: 首个值自动结束, 再来值覆盖并计数
asio::awaitable<void> test_hub_single_event() {
    auto  fx  = co_await makeFixture();
    auto& hub = *fx->hub;

    EventDefineSpec spec;
    spec.name = "job.result";
    spec.kind = EventKind::Single;
    std::string err;
    XX_TEST_EXPECT_TRUE(hub.define(spec, &err));
    XX_TEST_EXPECT_TRUE(hub.publishValue("job.result", Json{{"n", 1}}, &err));
    {
        auto rec = hub.queryEvent("job.result");
        XX_TEST_EXPECT_TRUE(rec->state == EventState::Ended);
        XX_TEST_EXPECT_TRUE(rec->published == 1);
    }
    XX_TEST_EXPECT_TRUE(hub.publishValue("job.result", Json{{"n", 2}}, &err));
    {
        auto rec = hub.queryEvent("job.result");
        XX_TEST_EXPECT_TRUE(rec->overwritten == 1);
        XX_TEST_EXPECT_TRUE(rec->history.size() == 1);
        XX_TEST_EXPECT_TRUE(rec->history.back()["n"].get<int>() == 2);
    }

    // 同名不同 kind: 拒绝, 且不影响已有记录
    EventDefineSpec conflict;
    conflict.name = "job.result";
    conflict.kind = EventKind::Stream;
    XX_TEST_EXPECT_FALSE(hub.define(conflict, &err));
    XX_TEST_EXPECT_TRUE(contains(err, "different kind"));
    co_return;
}

/// 3. 名字校验: 空 / 保留前缀 / 非法字符
asio::awaitable<void> test_hub_name_validation() {
    auto  fx  = co_await makeFixture();
    auto& hub = *fx->hub;

    std::string err;
    XX_TEST_EXPECT_FALSE(hub.publishValue("", Json{}, &err));
    XX_TEST_EXPECT_TRUE(contains(err, "empty"));
    XX_TEST_EXPECT_FALSE(hub.publishValue("agent.turn.start", Json{}, &err));
    XX_TEST_EXPECT_TRUE(contains(err, "reserved"));
    XX_TEST_EXPECT_FALSE(hub.publishValue("has space", Json{}, &err));
    XX_TEST_EXPECT_TRUE(contains(err, "may only contain"));
    XX_TEST_EXPECT_FALSE(hub.publishValue(std::string(200, 'a'), Json{}, &err));
    XX_TEST_EXPECT_TRUE(contains(err, "too long"));
    XX_TEST_EXPECT_TRUE(hub.publishValue("ok.name_1-x", Json{}, &err));
    co_return;
}

/// 4. 回放: watch 之前到达的值也能拿到 (first / all / replay=false)
asio::awaitable<void> test_hub_replay() {
    auto  fx  = co_await makeFixture();
    auto& hub = *fx->hub;
    std::string err;
    XX_TEST_EXPECT_TRUE(hub.publishValue("chat.msg", Json("hello"), &err));

    // first: 立即满足并投递 (轮次在跑 → 待注入输入)
    fx->sink->busy = true;
    EventWaitSpec spec;
    spec.event         = "chat.msg";
    spec.mode          = "first";
    spec.originSession = fx->sessionId;
    auto wait = hub.watch(spec);
    XX_TEST_EXPECT_TRUE(wait.has_value());
    XX_TEST_EXPECT_TRUE(wait->state == WaitState::Satisfied);
    XX_TEST_EXPECT_TRUE(wait->deliveries == 1);
    {
        auto inputs = takeEventInputs(fx->session);
        XX_TEST_EXPECT_TRUE(inputs.size() == 1);
        XX_TEST_EXPECT_TRUE(contains(inputs.front(), "[Event] chat.msg (first)"));
        XX_TEST_EXPECT_TRUE(contains(inputs.front(), "wait: w-"));
        XX_TEST_EXPECT_TRUE(contains(inputs.front(), "status: value"));
        XX_TEST_EXPECT_TRUE(contains(inputs.front(), "value: hello"));
    }

    // replay=false: 不消费历史, 保持挂起
    EventWaitSpec noReplay = spec;
    noReplay.replay        = false;
    auto wait2             = hub.watch(noReplay);
    XX_TEST_EXPECT_TRUE(wait2.has_value());
    XX_TEST_EXPECT_TRUE(wait2->state == WaitState::Armed);
    co_return;
}

/// 5. mode=each: 每个值各投递一条; 事件结束后静默收尾
asio::awaitable<void> test_hub_mode_each() {
    auto  fx  = co_await makeFixture();
    auto& hub = *fx->hub;
    fx->sink->busy   = true;
    std::string err;

    EventWaitSpec spec;
    spec.event         = "chat.stream";
    spec.mode          = "each";
    spec.originSession = fx->sessionId;
    auto wait          = hub.watch(spec);
    XX_TEST_EXPECT_TRUE(wait.has_value());
    XX_TEST_EXPECT_TRUE(wait->state == WaitState::Armed);

    XX_TEST_EXPECT_TRUE(hub.publishValue("chat.stream", Json("a"), &err));
    XX_TEST_EXPECT_TRUE(hub.publishValue("chat.stream", Json("b"), &err));
    {
        auto inputs = takeEventInputs(fx->session);
        XX_TEST_EXPECT_TRUE(inputs.size() == 2);
        XX_TEST_EXPECT_TRUE(contains(inputs[0], "value: a"));
        XX_TEST_EXPECT_TRUE(contains(inputs[1], "value: b"));
    }
    {
        auto stored = hub.queryWait(wait->waitId);
        XX_TEST_EXPECT_TRUE(stored->state == WaitState::Armed);
        XX_TEST_EXPECT_TRUE(stored->deliveries == 2);
    }
    XX_TEST_EXPECT_TRUE(hub.endEvent("chat.stream", Json{}, &err));
    {
        auto stored = hub.queryWait(wait->waitId);
        XX_TEST_EXPECT_TRUE(stored->state == WaitState::Satisfied);
        XX_TEST_EXPECT_TRUE(stored->closeReason == "event-ended");
    }
    co_return;
}

/// 6. mode=all: 攒到流结束一次投递, 或达到 max_items 先投一次
asio::awaitable<void> test_hub_mode_all() {
    auto  fx  = co_await makeFixture();
    auto& hub = *fx->hub;
    fx->sink->busy   = true;
    std::string err;

    EventWaitSpec spec;
    spec.event         = "batch.done";
    spec.mode          = "all";
    spec.originSession = fx->sessionId;
    spec.maxItems      = 4;
    auto wait          = hub.watch(spec);
    XX_TEST_EXPECT_TRUE(wait->state == WaitState::Armed);

    XX_TEST_EXPECT_TRUE(hub.publishValue("batch.done", Json(1), &err));
    XX_TEST_EXPECT_TRUE(hub.publishValue("batch.done", Json(2), &err));
    XX_TEST_EXPECT_TRUE(takeEventInputs(fx->session).empty());

    XX_TEST_EXPECT_TRUE(hub.endEvent("batch.done", Json{}, &err));
    {
        auto inputs = takeEventInputs(fx->session);
        XX_TEST_EXPECT_TRUE(inputs.size() == 1);
        XX_TEST_EXPECT_TRUE(contains(inputs.front(), "status: stream-ended"));
        XX_TEST_EXPECT_TRUE(contains(inputs.front(), "values:"));
        XX_TEST_EXPECT_TRUE(contains(inputs.front(), "1. 1"));
        XX_TEST_EXPECT_TRUE(contains(inputs.front(), "2. 2"));
        XX_TEST_EXPECT_TRUE(contains(inputs.front(), "note: stream ended with 2 value(s)"));
    }

    // 达到 max_items 时先投一次, 之后继续累积
    EventWaitSpec capped = spec;
    capped.event         = "batch.capped";
    capped.maxItems      = 2;
    auto wait2           = hub.watch(capped);
    XX_TEST_EXPECT_TRUE(wait2->state == WaitState::Armed);
    XX_TEST_EXPECT_TRUE(hub.publishValue("batch.capped", Json(1), &err));
    XX_TEST_EXPECT_TRUE(hub.publishValue("batch.capped", Json(2), &err));
    {
        auto inputs = takeEventInputs(fx->session);
        XX_TEST_EXPECT_TRUE(inputs.size() == 1);
        XX_TEST_EXPECT_TRUE(contains(inputs.front(), "status: values"));
    }
    XX_TEST_EXPECT_TRUE(hub.queryWait(wait2->waitId)->state == WaitState::Armed);
    co_return;
}

/// 7. 事件在 watch 时已经结束: ended-empty / stream-ended 直接收尾
asio::awaitable<void> test_hub_watch_ended_event() {
    auto  fx  = co_await makeFixture();
    auto& hub = *fx->hub;
    fx->sink->busy   = true;
    std::string err;

    XX_TEST_EXPECT_TRUE(hub.define(EventDefineSpec{.name = "empty.stream"}, &err));
    XX_TEST_EXPECT_TRUE(hub.endEvent("empty.stream", Json{}, &err));

    EventWaitSpec spec;
    spec.event         = "empty.stream";
    spec.mode          = "first";
    spec.originSession = fx->sessionId;
    spec.replay        = false;
    auto wait          = hub.watch(spec);
    XX_TEST_EXPECT_TRUE(wait->state == WaitState::Satisfied);
    XX_TEST_EXPECT_TRUE(wait->closeReason == "event-ended");
    {
        auto inputs = takeEventInputs(fx->session);
        XX_TEST_EXPECT_TRUE(inputs.size() == 1);
        XX_TEST_EXPECT_TRUE(contains(inputs.front(), "status: ended-empty"));
        XX_TEST_EXPECT_TRUE(contains(inputs.front(), "the event ended without any value"));
    }

    // 流式带值: stream-ended
    XX_TEST_EXPECT_TRUE(hub.publishValue("empty.stream", Json("x"), &err));
    XX_TEST_EXPECT_TRUE(hub.endEvent("empty.stream", Json{}, &err));
    EventWaitSpec allSpec = spec;
    allSpec.mode          = "all";
    allSpec.replay        = false;
    auto waitAll          = hub.watch(allSpec);
    XX_TEST_EXPECT_TRUE(waitAll->state == WaitState::Satisfied);
    {
        auto inputs = takeEventInputs(fx->session);
        XX_TEST_EXPECT_TRUE(inputs.size() == 1);
        if (!inputs.empty()) {
            // replay=false: 不看历史值, 只知道"事件已结束"
            XX_TEST_EXPECT_TRUE(contains(inputs.front(), "status: ended-empty"));
        }
    }
    co_return;
}

/// 8. 预算: 事件数 / 等待总数 / 单会话等待数
asio::awaitable<void> test_hub_budgets() {
    EventHub::Config cfg;
    cfg.maxEvents          = 1;
    cfg.maxWaits           = 2;
    cfg.maxWaitsPerSession = 1;
    auto  fx  = co_await makeFixture(cfg);
    auto& hub = *fx->hub;
    fx->sink->busy   = true;
    std::string err;

    XX_TEST_EXPECT_TRUE(hub.define(EventDefineSpec{.name = "one.event"}, &err));
    XX_TEST_EXPECT_FALSE(hub.define(EventDefineSpec{.name = "two.event"}, &err));
    XX_TEST_EXPECT_TRUE(contains(err, "too many events"));

    EventWaitSpec spec;
    spec.event         = "one.event";
    spec.originSession = fx->sessionId;
    spec.delivery      = "none";
    XX_TEST_EXPECT_TRUE(hub.watch(spec).has_value());
    // 同会话超过每会话上限
    auto secondSessionSpec = spec;
    secondSessionSpec.originSession = "s2";
    // maxWaits=2: 本会话 1 条 + s2 1 条 已达上限
    XX_TEST_EXPECT_TRUE(hub.watch(secondSessionSpec).has_value());
    auto third = hub.watch(spec);
    XX_TEST_EXPECT_FALSE(third.has_value());
    XX_TEST_EXPECT_TRUE(
        contains(third.error(), "too many waits") || contains(third.error(), "for this session")
    );

    // 每会话上限: 视上一条是否被拒而定, 单独再验一次
    EventHub::Config cfg2;
    cfg2.maxWaitsPerSession = 1;
    auto  fx2  = co_await makeFixture(cfg2);
    auto& hub2 = *fx2->hub;
    EventWaitSpec spec2;
    spec2.event         = "a.event";
    spec2.originSession = fx2->sessionId;
    spec2.delivery      = "none";
    XX_TEST_EXPECT_TRUE(hub2.watch(spec2).has_value());
    auto rejected = hub2.watch(spec2);
    XX_TEST_EXPECT_FALSE(rejected.has_value());
    XX_TEST_EXPECT_TRUE(contains(rejected.error(), "for this session"));
    co_return;
}

/// 9. 取消与关闭: cancelWait / cancelWaitsOfSession / closeAll
asio::awaitable<void> test_hub_cancel_and_close() {
    auto  fx  = co_await makeFixture();
    auto& hub = *fx->hub;
    std::string err;

    EventWaitSpec spec;
    spec.event         = "cancel.me";
    spec.originSession = fx->sessionId;
    spec.delivery      = "none";

    auto w1 = hub.watch(spec);
    auto w2 = hub.watch(spec);
    XX_TEST_EXPECT_TRUE(w1.has_value() && w2.has_value());

    XX_TEST_EXPECT_TRUE(hub.cancelWait(w1->waitId));
    XX_TEST_EXPECT_TRUE(hub.queryWait(w1->waitId)->state == WaitState::Cancelled);
    // 已终态再取消: false
    XX_TEST_EXPECT_FALSE(hub.cancelWait(w1->waitId));
    XX_TEST_EXPECT_TRUE(hub.cancelWaitsOfSession(fx->sessionId, "test") == 1);
    XX_TEST_EXPECT_TRUE(hub.queryWait(w2->waitId)->state == WaitState::Cancelled);

    auto w3 = hub.watch(spec);
    XX_TEST_EXPECT_TRUE(w3.has_value());
    hub.closeAll("test-close");
    XX_TEST_EXPECT_TRUE(hub.closed());
    XX_TEST_EXPECT_TRUE(hub.queryWait(w3->waitId)->state == WaitState::Cancelled);
    // 关闭后一切发布都被拒
    XX_TEST_EXPECT_FALSE(hub.publishValue("cancel.me", Json(1), &err));
    XX_TEST_EXPECT_TRUE(contains(err, "closed"));
    XX_TEST_EXPECT_FALSE(hub.watch(spec).has_value());
    co_return;
}

/// 10. 投递决策表: 空闲开新轮 / 无会话丢弃 / 无驱动端延迟 / 关停丢弃
asio::awaitable<void> test_hub_delivery_table() {
    auto  fx  = co_await makeFixture();
    auto& hub = *fx->hub;

    // 空闲 + 有驱动端 + next-step → 提升为新一轮输入
    fx->sink->busy = false;
    EventDelivery idle;
    idle.sessionId = fx->sessionId;
    idle.text      = "[Event] x (first)\nstatus: value\nvalue: 1";
    idle.source    = "event:x";
    idle.delivery  = "next-step";
    XX_TEST_EXPECT_TRUE(hub.deliver(idle) == DeliverResult::Queued);
    XX_TEST_EXPECT_TRUE(fx->sink->texts.size() == 1);
    XX_TEST_EXPECT_TRUE(fx->sink->sources.front() == "event:x");

    // 轮次在跑 + next-step → 待注入输入
    fx->sink->busy = true;
    XX_TEST_EXPECT_TRUE(hub.deliver(idle) == DeliverResult::Queued);
    {
        auto inputs = takeEventInputs(fx->session);
        XX_TEST_EXPECT_TRUE(inputs.size() == 1);
    }

    // 轮次在跑 + inject → 待注入输入 (投递模式保留 inject)
    auto inject    = idle;
    inject.delivery = "inject";
    XX_TEST_EXPECT_TRUE(hub.deliver(inject) == DeliverResult::Queued);
    {
        auto inputs = fx->session->takePendingInputs(agentxx::agent::InputDelivery::Inject);
        XX_TEST_EXPECT_TRUE(inputs.size() == 1);
        XX_TEST_EXPECT_TRUE(inputs.front().source == "event:x");
    }

    // 轮次在跑 + dropIfBusy → 丢弃并计数
    auto busyDrop      = idle;
    busyDrop.dropIfBusy = true;
    XX_TEST_EXPECT_TRUE(hub.deliver(busyDrop) == DeliverResult::Dropped);
    XX_TEST_EXPECT_TRUE(hub.stats().dropped == 1);

    // 无会话 → 丢弃 (不新建会话)
    auto missing      = idle;
    missing.sessionId = "no-such-session";
    XX_TEST_EXPECT_TRUE(hub.deliver(missing) == DeliverResult::Dropped);
    XX_TEST_EXPECT_TRUE(hub.stats().droppedByReason.count("no-session") == 1);

    // 无驱动端 + 无 headless: 写待注入输入, 记 deferred
    fx->session->inputSink.reset();
    fx->sink->busy = false;
    XX_TEST_EXPECT_TRUE(hub.deliver(idle) == DeliverResult::Deferred);
    {
        auto inputs = takeEventInputs(fx->session);
        XX_TEST_EXPECT_TRUE(inputs.size() == 1);
    }

    // 关停中 → 丢弃
    fx->ctx->markShuttingDown();
    XX_TEST_EXPECT_TRUE(hub.deliver(idle) == DeliverResult::Dropped);
    XX_TEST_EXPECT_TRUE(hub.stats().droppedByReason.count("shutting-down") == 1);
    co_return;
}

/// 11. 背压: 同一会话的事件输入达到上限后丢弃并计数
asio::awaitable<void> test_hub_backlog_limit() {
    EventHub::Config cfg;
    cfg.deliveryBacklogPerSession = 3;
    auto  fx  = co_await makeFixture(cfg);
    auto& hub = *fx->hub;
    fx->sink->busy = true;

    EventDelivery delivery;
    delivery.sessionId = fx->sessionId;
    delivery.text      = "[Event] flood\nstatus: value\nvalue: 1";
    delivery.source    = "event:flood";
    delivery.delivery  = "next-step";

    size_t queued = 0;
    for (int i = 0; i < 10; ++i) {
        if (hub.deliver(delivery) == DeliverResult::Queued) {
            ++queued;
        }
    }
    XX_TEST_EXPECT_TRUE(queued == 3);
    XX_TEST_EXPECT_TRUE(takeEventInputs(fx->session).size() == 3);
    XX_TEST_EXPECT_TRUE(hub.stats().droppedByReason.count("backlog") == 1);
    co_return;
}

/// 12. awaitOnce: 值到达 / 超时 / 取消
asio::awaitable<void> test_hub_await_once() {
    auto  fx  = co_await makeFixture();
    auto& hub = *fx->hub;
    std::string err;

    // 值到达
    asio::co_spawn(
        co_await asio::this_coro::executor,
        [hubPtr = fx->hub]() -> asio::awaitable<void> {
            co_await sleepMs(60);
            std::string ignored;
            hubPtr->publishValue("await.me", Json{{"v", 7}}, &ignored);
            co_return;
        },
        asio::detached
    );
    EventWaitSpec spec;
    spec.event         = "await.me";
    spec.mode          = "first";
    spec.originSession = fx->sessionId;
    spec.timeout       = std::chrono::milliseconds{2000};
    auto wait          = co_await hub.awaitOnce(spec, nullptr);
    XX_TEST_EXPECT_TRUE(wait.state == WaitState::Satisfied);
    XX_TEST_EXPECT_TRUE(wait.values.size() == 1);
    if (!wait.values.empty()) {
        XX_TEST_EXPECT_TRUE(wait.values.front()["v"].get<int>() == 7);
    }
    // 临时等待不留在表里
    XX_TEST_EXPECT_FALSE(hub.queryWait(wait.waitId).has_value());

    // 超时
    EventWaitSpec timeoutSpec;
    timeoutSpec.event         = "never.arrives";
    timeoutSpec.originSession = fx->sessionId;
    timeoutSpec.timeout       = std::chrono::milliseconds{120};
    const auto begin          = std::chrono::steady_clock::now();
    auto       timedOut       = co_await hub.awaitOnce(timeoutSpec, nullptr);
    const auto elapsedMs      = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - begin
    )
                               .count();
    XX_TEST_EXPECT_TRUE(timedOut.state == WaitState::TimedOut);
    XX_TEST_EXPECT_TRUE(elapsedMs >= 100);
    XX_TEST_EXPECT_FALSE(hub.queryWait(timedOut.waitId).has_value());

    // 取消令牌
    auto token = std::make_shared<neograph::graph::CancelToken>();
    asio::co_spawn(
        co_await asio::this_coro::executor,
        [token]() -> asio::awaitable<void> {
            co_await sleepMs(60);
            token->cancel();
            co_return;
        },
        asio::detached
    );
    EventWaitSpec cancelSpec;
    cancelSpec.event         = "cancel.me.out";
    cancelSpec.originSession = fx->sessionId;
    cancelSpec.timeout       = std::chrono::milliseconds{5000};
    bool cancelled           = false;
    try {
        auto ignored  = co_await hub.awaitOnce(cancelSpec, token);
        (void)ignored;
    } catch (const neograph::graph::CancelledException&) {
        cancelled = true;
    }
    XX_TEST_EXPECT_TRUE(cancelled);
    XX_TEST_EXPECT_TRUE(hub.waitCount() == 0);
    co_return;
}

/// 13. 定时源: armTimer 到点发布 `timer.<名字>`
asio::awaitable<void> test_hub_timer_source() {
    auto  fx  = co_await makeFixture();
    auto& hub = *fx->hub;
    fx->sink->busy = true;

    EventWaitSpec spec;
    spec.event         = "timer.tick";
    spec.mode          = "each";
    spec.originSession = fx->sessionId;
    auto wait          = hub.watch(spec);
    XX_TEST_EXPECT_TRUE(wait.has_value());

    XX_TEST_EXPECT_TRUE(hub.armTimer("tick", 40, 0));
    const bool fired = co_await waitUntil([&]() {
        return fx->session->pendingInputCount("event:") >= 1;
    });
    XX_TEST_EXPECT_TRUE(fired);
    {
        auto inputs = takeEventInputs(fx->session);
        XX_TEST_EXPECT_TRUE(inputs.size() == 1);
        XX_TEST_EXPECT_TRUE(contains(inputs.front(), "[Event] timer.tick"));
        XX_TEST_EXPECT_TRUE(contains(inputs.front(), "firedAtMs"));
    }
    // 一次性定时源触发后自行移除
    XX_TEST_EXPECT_FALSE(hub.cancelTimer("tick"));
    // 非法参数
    XX_TEST_EXPECT_FALSE(hub.armTimer("", 10, 0));
    XX_TEST_EXPECT_FALSE(hub.armTimer("bad", 0, 0));
    co_return;
}

/// 14. ttl 到期收尾: 未结束的事件按 Expired 把挂着的等待收掉
asio::awaitable<void> test_hub_ttl_and_linger() {
    auto  fx  = co_await makeFixture();
    auto& hub = *fx->hub;
    fx->sink->busy   = true;
    std::string err;

    EventDefineSpec spec;
    spec.name = "ttl.event";
    spec.ttl  = std::chrono::milliseconds{30};
    spec.linger = std::chrono::milliseconds{20};
    XX_TEST_EXPECT_TRUE(hub.define(spec, &err));

    EventWaitSpec waitSpec;
    waitSpec.event         = "ttl.event";
    waitSpec.mode          = "first";
    waitSpec.originSession = fx->sessionId;
    waitSpec.replay        = false;
    auto wait              = hub.watch(waitSpec);
    XX_TEST_EXPECT_TRUE(wait->state == WaitState::Armed);

    co_await sleepMs(80);
    hub.sweepExpired();
    {
        auto stored = hub.queryWait(wait->waitId);
        // ttl 到期 → 事件按"结束且无值"投递并收尾
        XX_TEST_EXPECT_TRUE(stored->state == WaitState::Satisfied);
        XX_TEST_EXPECT_TRUE(stored->closeReason == "event-ended");
    }
    co_await sleepMs(60);
    hub.sweepExpired();
    // linger 到期后记录被回收
    XX_TEST_EXPECT_FALSE(hub.queryEvent("ttl.event").has_value());
    co_return;
}

/// 15. 列表与统计的输出字段
asio::awaitable<void> test_hub_queries() {
    auto  fx  = co_await makeFixture();
    auto& hub = *fx->hub;
    fx->sink->busy   = true;
    std::string err;
    EventDefineSpec spec;
    spec.name        = "q.event";
    spec.description = "for queries";
    XX_TEST_EXPECT_TRUE(hub.define(spec, &err));
    XX_TEST_EXPECT_TRUE(hub.publishValue("q.event", Json(1), &err));

    EventWaitSpec waitSpec;
    waitSpec.event         = "q.event";
    waitSpec.mode          = "all";
    waitSpec.originSession = fx->sessionId;
    waitSpec.replay        = false;
    auto wait              = hub.watch(waitSpec);
    XX_TEST_EXPECT_TRUE(wait->state == WaitState::Armed);

    {
        auto events = hub.listEventsJson();
        XX_TEST_EXPECT_TRUE(events["count"].get<int>() == 1);
        XX_TEST_EXPECT_TRUE(events["events"][0]["name"].get<std::string>() == "q.event");
        XX_TEST_EXPECT_TRUE(events["events"][0]["waits"].get<int>() == 1);
        XX_TEST_EXPECT_TRUE(events["events"][0]["description"].get<std::string>() == "for queries");
    }
    {
        auto waits = hub.listWaitsJson(fx->sessionId);
        XX_TEST_EXPECT_TRUE(waits["count"].get<int>() == 1);
        XX_TEST_EXPECT_TRUE(waits["waits"][0]["event"].get<std::string>() == "q.event");
        XX_TEST_EXPECT_TRUE(waits["waits"][0]["state"].get<std::string>() == "armed");
        XX_TEST_EXPECT_TRUE(hub.listWaitsJson("other-session")["count"].get<int>() == 0);
    }
    {
        auto stats = hub.statsJson();
        XX_TEST_EXPECT_TRUE(stats["events"].get<int>() == 1);
        XX_TEST_EXPECT_TRUE(stats["values"].get<int>() == 1);
        XX_TEST_EXPECT_TRUE(stats["closed"].get<bool>() == false);
        XX_TEST_EXPECT_TRUE(stats.contains("droppedByReason"));
    }
    // wants: 有记录或有挂起等待
    XX_TEST_EXPECT_TRUE(hub.wants("q.event"));
    XX_TEST_EXPECT_FALSE(hub.wants("unknown.event"));
    co_return;
}

/// 16. 事件名 → 总线主题的换算与校验 (hub 前缀订阅用的同一套规则)
asio::awaitable<void> test_hub_naming() {
    using agentxx::events::naming::busTopicOf;
    using agentxx::events::naming::eventNameOfTopic;
    using agentxx::events::naming::validateEventName;

    XX_TEST_EXPECT_TRUE(busTopicOf("build.done") == "evt.build.done");
    XX_TEST_EXPECT_TRUE(eventNameOfTopic("evt.build.done").value_or("") == "build.done");
    // 插件事件经 qualifyEventTopic 补 `plugin.` 前缀后仍能还原
    XX_TEST_EXPECT_TRUE(eventNameOfTopic("plugin.evt.build.done").value_or("") == "build.done");
    // 不是值主题: 不认
    XX_TEST_EXPECT_FALSE(eventNameOfTopic("agent.turn.start").has_value());
    XX_TEST_EXPECT_FALSE(eventNameOfTopic("evt.").has_value());
    XX_TEST_EXPECT_FALSE(eventNameOfTopic("plugin.agent.turn.start").has_value());

    std::string err;
    XX_TEST_EXPECT_TRUE(validateEventName("ok.name_1-2"));
    XX_TEST_EXPECT_FALSE(validateEventName("", &err));
    XX_TEST_EXPECT_TRUE(contains(err, "empty"));
    XX_TEST_EXPECT_FALSE(validateEventName("hub.value", &err));
    XX_TEST_EXPECT_TRUE(contains(err, "reserved"));
    XX_TEST_EXPECT_FALSE(validateEventName("service.x", &err));
    XX_TEST_EXPECT_FALSE(validateEventName("evt.x", &err));
    XX_TEST_EXPECT_FALSE(validateEventName("has space", &err));
    XX_TEST_EXPECT_FALSE(validateEventName(std::string(200, 'a'), &err));
    XX_TEST_EXPECT_TRUE(validateEventName(std::string(agentxx::events::naming::kMaxEventNameLength, 'a')));
    co_return;
}
asio::awaitable<TestResult> run_event_hub_tests() {
    TestResult result;

    co_await test_hub_record_statemachine();
    co_await test_hub_single_event();
    co_await test_hub_name_validation();
    co_await test_hub_replay();
    co_await test_hub_mode_each();
    co_await test_hub_mode_all();
    co_await test_hub_watch_ended_event();
    co_await test_hub_budgets();
    co_await test_hub_cancel_and_close();
    co_await test_hub_delivery_table();
    co_await test_hub_backlog_limit();
    co_await test_hub_await_once();
    co_await test_hub_timer_source();
    co_await test_hub_ttl_and_linger();
    co_await test_hub_queries();
    co_await test_hub_naming();

    result.passed = g_eh_passed;
    result.failed = g_eh_failed;
    co_return result;
}

} // namespace test
} // namespace agentxx