#pragma once

#include "agentxx/middlewares/event_stream.h"
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

namespace agentxx {
namespace test {

inline static int g_es_passed = 0;
inline static int g_es_failed = 0;

#define ES_TEST(name) std::cout << "  [" << (name) << "]" << std::endl;
#define ES_EXPECT_TRUE(expr)                                                    \
  do {                                                                          \
    if (expr) {                                                                 \
      g_es_passed++;                                                            \
    } else {                                                                    \
      g_es_failed++;                                                            \
      std::cerr << "    FAIL at line " << __LINE__ << ": expected true"         \
                << std::endl;                                                   \
    }                                                                           \
  } while (0)
#define ES_EXPECT_FALSE(expr)                                                   \
  do {                                                                          \
    if (!(expr)) {                                                              \
      g_es_passed++;                                                            \
    } else {                                                                    \
      g_es_failed++;                                                            \
      std::cerr << "    FAIL at line " << __LINE__ << ": expected false"        \
                << std::endl;                                                   \
    }                                                                           \
  } while (0)

struct TestEvent {
  std::string msg;
  int value;
};

struct TestReq {
  std::string question;
};
struct TestResp {
  std::string answer;
};

/// 1. 单向事件流: 多订阅者派发 + execHit 自动移除 + 异常隔离
inline asio::awaitable<void> test_eventstream_publish() {
  ES_TEST("EventStream publish / multi-subscriber / execHit / exception-isolation");

  auto bus = agentxx::middleware::EventBus{co_await asio::this_coro::executor};
  auto &stream = bus.get<TestEvent>("test.ping");

  std::atomic<int> permanentCount{0};
  std::atomic<int> onceCount{0};
  std::atomic<int> twiceCount{0};
  std::atomic<int> throwerCount{0};

  auto idPermanent = stream.subscribe(
      [&](const TestEvent &e) -> asio::awaitable<void> {
        permanentCount += e.value;
        co_return;
      },
      0);
  auto idOnce = stream.subscribe(
      [&](const TestEvent &e) -> asio::awaitable<void> {
        onceCount += e.value;
        co_return;
      },
      1);
  auto idTwice = stream.subscribe(
      [&](const TestEvent &e) -> asio::awaitable<void> {
        twiceCount += e.value;
        co_return;
      },
      2);
  auto idThrower = stream.subscribe(
      [&](const TestEvent &) -> asio::awaitable<void> {
        throwerCount++;
        throw std::runtime_error("intentional listener failure");
        co_return;
      },
      0);
  (void)idPermanent;
  (void)idOnce;
  (void)idTwice;
  (void)idThrower;

  // 第 1 次发布: 所有 4 个订阅者都应触发; thrower 抛异常但不应中断其他
  co_await stream.publish(TestEvent{.msg = "a", .value = 10});
  ES_EXPECT_TRUE(permanentCount.load() == 10);
  ES_EXPECT_TRUE(onceCount.load() == 10);
  ES_EXPECT_TRUE(twiceCount.load() == 10);
  ES_EXPECT_TRUE(throwerCount.load() == 1);

  // 第 2 次发布: once 已自动移除, twice 剩 1 次, thrower 仍常驻(异常被吞)
  co_await stream.publish(TestEvent{.msg = "b", .value = 20});
  ES_EXPECT_TRUE(permanentCount.load() == 30);
  ES_EXPECT_TRUE(onceCount.load() == 10); // 不再触发
  ES_EXPECT_TRUE(twiceCount.load() == 30);
  ES_EXPECT_TRUE(throwerCount.load() == 2);

  // 第 3 次发布: twice 也已自动移除
  co_await stream.publish(TestEvent{.msg = "c", .value = 40});
  ES_EXPECT_TRUE(permanentCount.load() == 70);
  ES_EXPECT_TRUE(onceCount.load() == 10);
  ES_EXPECT_TRUE(twiceCount.load() == 30);
  ES_EXPECT_TRUE(throwerCount.load() == 3);

  // 手动取消常驻订阅后再发布
  ES_EXPECT_TRUE(stream.unsubscribe(idPermanent));
  ES_EXPECT_TRUE(stream.unsubscribe(idThrower));
  co_await stream.publish(TestEvent{.msg = "d", .value = 80});
  ES_EXPECT_TRUE(permanentCount.load() == 70); // 已取消
  ES_EXPECT_TRUE(throwerCount.load() == 3);
  ES_EXPECT_FALSE(stream.unsubscribe(99999)); // 不存在

  co_return;
}

/// 2. 请求-响应: 正常响应 + correlationId 关联
inline asio::awaitable<void> test_requestresponse_normal() {
  ES_TEST("RequestResponseStream request / respond normal");

  auto bus = agentxx::middleware::EventBus{co_await asio::this_coro::executor};
  auto &rr = bus.getRR<TestReq, TestResp>("test.qa");

  auto serverId = rr.serve(
      [](const TestReq &req,
         size_t corrId) -> asio::awaitable<TestResp> {
        ES_EXPECT_TRUE(corrId > 0);
        co_return TestResp{.answer = "echo:" + req.question};
      });
  (void)serverId;

  auto resp = co_await rr.request(TestReq{.question = "hello"}, std::chrono::seconds(5));
  ES_EXPECT_TRUE(resp.has_value());
  ES_EXPECT_TRUE(resp.value().answer == "echo:hello");

  co_return;
}

/// 3. 请求-响应: 超时返回 nullopt
inline asio::awaitable<void> test_requestresponse_timeout() {
  ES_TEST("RequestResponseStream request timeout");

  auto bus = agentxx::middleware::EventBus{co_await asio::this_coro::executor};
  auto &rr = bus.getRR<TestReq, TestResp>("test.qa.slow");

  // server 永不 respond (sleep 久于 timeout)
  rr.serve([](const TestReq &,
              size_t) -> asio::awaitable<TestResp> {
    auto timer = asio::steady_timer(co_await asio::this_coro::executor,
                                    std::chrono::seconds(1));
    co_await timer.async_wait(asio::use_awaitable);
    co_return TestResp{.answer = "too late"};
  });

  auto start = std::chrono::steady_clock::now();
  auto resp = co_await rr.request(TestReq{.question = "ping"},
                                  std::chrono::milliseconds(200));
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count();
  ES_EXPECT_FALSE(resp.has_value());
  ES_EXPECT_TRUE(elapsed >= 180 && elapsed < 2000);

  co_return;
}

/// 4. 请求-响应: 无 server 时返回 nullopt
inline asio::awaitable<void> test_requestresponse_noserver() {
  ES_TEST("RequestResponseStream request no-server");

  auto bus = agentxx::middleware::EventBus{co_await asio::this_coro::executor};
  auto &rr = bus.getRR<TestReq, TestResp>("test.qa.empty");

  auto resp = co_await rr.request(TestReq{.question = "x"},
                                  std::chrono::seconds(2));
  ES_EXPECT_FALSE(resp.has_value());

  co_return;
}

/// 5. 定时器事件流: once 触发一次且不阻塞调用者
inline asio::awaitable<void> test_timer_once() {
  ES_TEST("TimerEventStream once");

  auto bus = agentxx::middleware::EventBus{co_await asio::this_coro::executor};
  std::atomic<int> fireCount{0};

  auto id = bus.timer<TestEvent>().once(
      std::chrono::milliseconds(50),
      [&](const TestEvent &) -> asio::awaitable<void> {
        fireCount++;
        co_return;
      },
      TestEvent{.msg = "tick", .value = 1});
  ES_EXPECT_TRUE(id > 0);

  ES_EXPECT_TRUE(fireCount.load() == 0); // 立即返回, 未触发
  // 等待定时器到期
  auto timer = asio::steady_timer(co_await asio::this_coro::executor,
                                  std::chrono::milliseconds(200));
  co_await timer.async_wait(asio::use_awaitable);
  ES_EXPECT_TRUE(fireCount.load() == 1);

  co_return;
}

/// 6. EventBus 便捷方法 publish/request 与复用同 topic
inline asio::awaitable<void> test_eventbus_convenience() {
  ES_TEST("EventBus convenience publish/request + topic reuse");

  auto bus = agentxx::middleware::EventBus{co_await asio::this_coro::executor};

  std::atomic<int> seen{0};
  bus.get<TestEvent>("conv.topic")
      .subscribe([&](const TestEvent &e) -> asio::awaitable<void> {
        seen += e.value;
        co_return;
      });

  co_await bus.publish<TestEvent>("conv.topic", TestEvent{.msg = "z", .value = 7});
  ES_EXPECT_TRUE(seen.load() == 7);
  // 同 topic 复用, 应是同一个流
  co_await bus.publish<TestEvent>("conv.topic", TestEvent{.msg = "z2", .value = 3});
  ES_EXPECT_TRUE(seen.load() == 10);

  auto &rr = bus.getRR<TestReq, TestResp>("conv.rr");
  rr.serve([](const TestReq &req,
              size_t) -> asio::awaitable<TestResp> {
    co_return TestResp{.answer = req.question + "!"};
  });
  auto resp = co_await bus.request<TestReq, TestResp>(
      "conv.rr", TestReq{.question = "hi"}, std::chrono::seconds(5));
  ES_EXPECT_TRUE(resp.has_value());
  ES_EXPECT_TRUE(resp.value().answer == "hi!");

  co_return;
}

inline asio::awaitable<void> run_event_stream_tests() {
  std::cout << "=== event_stream Tests ===" << std::endl;
  try {
    co_await test_eventstream_publish();
    co_await test_requestresponse_normal();
    co_await test_requestresponse_timeout();
    co_await test_requestresponse_noserver();
    co_await test_timer_once();
    co_await test_eventbus_convenience();
  } catch (const std::exception &e) {
    std::cout << "[FAIL] event_stream suite exception: " << e.what()
              << std::endl;
    g_es_failed++;
  }
  std::cout << "=== event_stream Tests DONE (pass=" << g_es_passed
            << " fail=" << g_es_failed << ") ===" << std::endl;
  co_return;
}

} // namespace test
} // namespace agentxx
