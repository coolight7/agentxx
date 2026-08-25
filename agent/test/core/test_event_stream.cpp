#include "test_event_stream.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/util/log.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>

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

/// 1. 单向事件流: 多订阅者派发 + execHit 自动移除 + 异常隔离
asio::awaitable<void> test_eventstream_publish() {
    auto  bus    = agentxx::event::EventBus{co_await asio::this_coro::executor};
    auto& stream = bus.get<TestEvent>("test.ping");

    std::atomic<int> permanentCount{0};
    std::atomic<int> onceCount{0};
    std::atomic<int> twiceCount{0};
    std::atomic<int> throwerCount{0};

    auto idPermanent = stream.subscribe(
        [&](const TestEvent& e) -> asio::awaitable<void> {
            permanentCount += e.value;
            co_return;
        },
        0
    );
    auto idOnce = stream.subscribe(
        [&](const TestEvent& e) -> asio::awaitable<void> {
            onceCount += e.value;
            co_return;
        },
        1
    );
    auto idTwice = stream.subscribe(
        [&](const TestEvent& e) -> asio::awaitable<void> {
            twiceCount += e.value;
            co_return;
        },
        2
    );
    auto idThrower = stream.subscribe(
        [&](const TestEvent&) -> asio::awaitable<void> {
            throwerCount++;
            throw std::runtime_error("intentional listener failure");
            co_return;
        },
        0
    );
    (void)idPermanent;
    (void)idOnce;
    (void)idTwice;
    (void)idThrower;

    // 第 1 次发布: 所有 4 个订阅者都应触发; thrower 抛异常但不应中断其他
    co_await stream.publish(TestEvent{.msg = "a", .value = 10});
    XX_TEST_EXPECT_TRUE(permanentCount.load() == 10);
    XX_TEST_EXPECT_TRUE(onceCount.load() == 10);
    XX_TEST_EXPECT_TRUE(twiceCount.load() == 10);
    XX_TEST_EXPECT_TRUE(throwerCount.load() == 1);

    // 第 2 次发布: once 已自动移除, twice 剩 1 次, thrower 仍常驻(异常被吞)
    co_await stream.publish(TestEvent{.msg = "b", .value = 20});
    XX_TEST_EXPECT_TRUE(permanentCount.load() == 30);
    XX_TEST_EXPECT_TRUE(onceCount.load() == 10); // 不再触发
    XX_TEST_EXPECT_TRUE(twiceCount.load() == 30);
    XX_TEST_EXPECT_TRUE(throwerCount.load() == 2);

    // 第 3 次发布: twice 也已自动移除
    co_await stream.publish(TestEvent{.msg = "c", .value = 40});
    XX_TEST_EXPECT_TRUE(permanentCount.load() == 70);
    XX_TEST_EXPECT_TRUE(onceCount.load() == 10);
    XX_TEST_EXPECT_TRUE(twiceCount.load() == 30);
    XX_TEST_EXPECT_TRUE(throwerCount.load() == 3);

    // 手动取消常驻订阅后再发布
    XX_TEST_EXPECT_TRUE(stream.unsubscribe(idPermanent));
    XX_TEST_EXPECT_TRUE(stream.unsubscribe(idThrower));
    co_await stream.publish(TestEvent{.msg = "d", .value = 80});
    XX_TEST_EXPECT_TRUE(permanentCount.load() == 70); // 已取消
    XX_TEST_EXPECT_TRUE(throwerCount.load() == 3);
    XX_TEST_EXPECT_FALSE(stream.unsubscribe(99999)); // 不存在

    co_return;
}

/// 2. 请求-响应: 正常响应 + correlationId 关联
asio::awaitable<void> test_requestresponse_normal() {
    auto  bus = agentxx::event::EventBus{co_await asio::this_coro::executor};
    auto& rr  = bus.getRR<TestReq, TestResp>("test.qa");

    auto serverId
        = rr.registerServer([](const TestReq& req, size_t corrId) -> asio::awaitable<TestResp> {
              XX_TEST_EXPECT_TRUE(corrId > 0);
              co_return TestResp{.answer = "echo:" + req.question};
          });
    (void)serverId;

    auto resp = co_await rr.request(TestReq{.question = "hello"}, std::chrono::seconds(5));
    XX_TEST_EXPECT_TRUE(resp.has_value());
    XX_TEST_EXPECT_TRUE(resp.value().answer == "echo:hello");

    co_return;
}

/// 3. 请求-响应: 超时返回 nullopt
asio::awaitable<void> test_requestresponse_timeout() {
    auto  bus = agentxx::event::EventBus{co_await asio::this_coro::executor};
    auto& rr  = bus.getRR<TestReq, TestResp>("test.qa.slow");

    // server 永不 respond (sleep 久于 timeout)
    rr.registerServer([](const TestReq&, size_t) -> asio::awaitable<TestResp> {
        auto timer
            = asio::steady_timer(co_await asio::this_coro::executor, std::chrono::seconds(1));
        co_await timer.async_wait(asio::use_awaitable);
        co_return TestResp{.answer = "too late"};
    });

    auto start   = std::chrono::steady_clock::now();
    auto resp    = co_await rr.request(TestReq{.question = "ping"}, std::chrono::milliseconds(200));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start
    )
                       .count();
    XX_TEST_EXPECT_FALSE(resp.has_value());
    XX_TEST_EXPECT_TRUE(elapsed >= 180 && elapsed < 2000);

    co_return;
}

/// 4. 请求-响应: 无 server 时返回 nullopt
asio::awaitable<void> test_requestresponse_noserver() {
    auto  bus = agentxx::event::EventBus{co_await asio::this_coro::executor};
    auto& rr  = bus.getRR<TestReq, TestResp>("test.qa.empty");

    auto resp = co_await rr.request(TestReq{.question = "x"}, std::chrono::seconds(2));
    XX_TEST_EXPECT_FALSE(resp.has_value());

    co_return;
}

/// 回归: server 处理器抛异常时, request 须返回 unexpected(错误),
/// 而非默认构造的"成功"响应 (修复前 catch 仅记日志, out 保持默认成功值 → 误判成功)
asio::awaitable<void> test_requestresponse_server_exception() {
    auto  bus = agentxx::event::EventBus{co_await asio::this_coro::executor};
    auto& rr  = bus.getRR<TestReq, TestResp>("test.qa.throw");

    // server 处理器抛异常 -> 经 channel_cancelled 传回, 请求方 waitResp 抛异常
    rr.registerServer([](const TestReq&, size_t) -> asio::awaitable<TestResp> {
        throw std::runtime_error("handler boom");
        co_return TestResp{.answer = "unreachable"};
    });

    auto resp = co_await rr.request(TestReq{.question = "x"}, std::chrono::seconds(5));
    XX_TEST_EXPECT_FALSE(resp.has_value());

    co_return;
}

/// 5. 定时器事件流: once 触发一次且不阻塞调用者
asio::awaitable<void> test_timer_once() {
    auto             bus = agentxx::event::EventBus{co_await asio::this_coro::executor};
    std::atomic<int> fireCount{0};

    auto id = bus.timer<TestEvent>().once(
        std::chrono::milliseconds(50),
        [&](const TestEvent&) -> asio::awaitable<void> {
            fireCount++;
            co_return;
        },
        TestEvent{.msg = "tick", .value = 1}
    );
    XX_TEST_EXPECT_TRUE(id > 0);

    XX_TEST_EXPECT_TRUE(fireCount.load() == 0); // 立即返回, 未触发
    // 等待定时器到期
    auto timer
        = asio::steady_timer(co_await asio::this_coro::executor, std::chrono::milliseconds(200));
    co_await timer.async_wait(asio::use_awaitable);
    XX_TEST_EXPECT_TRUE(fireCount.load() == 1);

    co_return;
}

/// 6. EventBus 便捷方法 publish/request 与复用同 topic
asio::awaitable<void> test_eventbus_convenience() {
    auto bus = agentxx::event::EventBus{co_await asio::this_coro::executor};

    std::atomic<int> seen{0};
    bus.get<TestEvent>("conv.topic").subscribe([&](const TestEvent& e) -> asio::awaitable<void> {
        seen += e.value;
        co_return;
    });

    co_await bus.publish<TestEvent>("conv.topic", TestEvent{.msg = "z", .value = 7});
    XX_TEST_EXPECT_TRUE(seen.load() == 7);
    // 同 topic 复用, 应是同一个流
    co_await bus.publish<TestEvent>("conv.topic", TestEvent{.msg = "z2", .value = 3});
    XX_TEST_EXPECT_TRUE(seen.load() == 10);

    auto& rr = bus.getRR<TestReq, TestResp>("conv.rr");
    rr.registerServer([](const TestReq& req, size_t) -> asio::awaitable<TestResp> {
        co_return TestResp{.answer = req.question + "!"};
    });
    auto resp = co_await bus.request<TestReq, TestResp>(
        "conv.rr",
        TestReq{.question = "hi"},
        std::chrono::seconds(5)
    );
    XX_TEST_EXPECT_TRUE(resp.has_value());
    XX_TEST_EXPECT_TRUE(resp.value().answer == "hi!");

    co_return;
}

/// 8. EventBus 前缀订阅: 匹配 topic 前缀的事件经 any 转发, 取消后不再触发
asio::awaitable<void> test_eventbus_prefix_subscribe() {
    auto bus       = agentxx::event::EventBus{co_await asio::this_coro::executor};
    int  matched   = 0;
    int  unrelated = 0;
    auto id = bus.listenPrefix("plugin.", [&](std::string_view topic, const std::any& payload) {
        if (payload.type() != typeid(std::string)) {
            return;
        }
        const auto& data = std::any_cast<const std::string&>(payload);
        if (topic == "plugin.agentxx_codegraph.progress") {
            matched += (data == "{\"p\":1}") ? 1 : 0;
        } else {
            unrelated++;
        }
    });

    // 匹配前缀的事件 → 回调
    co_await bus.publish<std::string>("plugin.agentxx_codegraph.progress", "{\"p\":1}");
    co_await bus.publish<std::string>("plugin.agentxx_codegraph.status", "{\"loaded\":true}");
    // 不匹配前缀 → 不触发
    co_await bus.publish<std::string>("other.topic", "x");
    // 非 string 载荷 → 类型不匹配跳过 (不会崩溃)
    co_await bus.publish<int>("plugin.int_event", 42);
    XX_TEST_EXPECT_TRUE(matched == 1);
    XX_TEST_EXPECT_TRUE(unrelated == 1);

    // 取消订阅 → 不再触发
    bus.unlistenPrefix(id);
    co_await bus.publish<std::string>("plugin.agentxx_codegraph.progress", "{\"p\":2}");
    XX_TEST_EXPECT_TRUE(matched == 1);

    co_return;
}

asio::awaitable<TestResult> run_event_stream_tests() {
    try {
        co_await test_eventstream_publish();
        co_await test_requestresponse_normal();
        co_await test_requestresponse_timeout();
        co_await test_requestresponse_noserver();
        co_await test_requestresponse_server_exception();
        co_await test_timer_once();
        co_await test_eventbus_convenience();
        co_await test_eventbus_prefix_subscribe();
    } catch (const std::exception& e) {
        TEST_FAIL << "event_stream suite exception: " << e.what() << std::endl;
        g_es_failed++;
    }
    co_return TestResult{g_es_passed, g_es_failed};
}

} // namespace test
} // namespace agentxx
