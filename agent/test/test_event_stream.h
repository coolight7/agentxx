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

#include "test_framework.h"
#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_es_passed
#define XX_TEST_FAILED g_es_failed

namespace agentxx {
namespace test {

inline static int g_es_passed = 0;
inline static int g_es_failed = 0;

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
inline asio::awaitable<void> test_requestresponse_normal() {

  auto bus = agentxx::middleware::EventBus{co_await asio::this_coro::executor};
  auto &rr = bus.getRR<TestReq, TestResp>("test.qa");

  auto serverId = rr.serve(
      [](const TestReq &req, size_t corrId) -> asio::awaitable<TestResp> {
        XX_TEST_EXPECT_TRUE(corrId > 0);
        co_return TestResp{.answer = "echo:" + req.question};
      });
  (void)serverId;

  auto resp = co_await rr.request(TestReq{.question = "hello"},
                                  std::chrono::seconds(5));
  XX_TEST_EXPECT_TRUE(resp.has_value());
  XX_TEST_EXPECT_TRUE(resp.value().answer == "echo:hello");

  co_return;
}

/// 3. 请求-响应: 超时返回 nullopt
inline asio::awaitable<void> test_requestresponse_timeout() {

  auto bus = agentxx::middleware::EventBus{co_await asio::this_coro::executor};
  auto &rr = bus.getRR<TestReq, TestResp>("test.qa.slow");

  // server 永不 respond (sleep 久于 timeout)
  rr.serve([](const TestReq &, size_t) -> asio::awaitable<TestResp> {
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
  XX_TEST_EXPECT_FALSE(resp.has_value());
  XX_TEST_EXPECT_TRUE(elapsed >= 180 && elapsed < 2000);

  co_return;
}

/// 4. 请求-响应: 无 server 时返回 nullopt
inline asio::awaitable<void> test_requestresponse_noserver() {

  auto bus = agentxx::middleware::EventBus{co_await asio::this_coro::executor};
  auto &rr = bus.getRR<TestReq, TestResp>("test.qa.empty");

  auto resp =
      co_await rr.request(TestReq{.question = "x"}, std::chrono::seconds(2));
  XX_TEST_EXPECT_FALSE(resp.has_value());

  co_return;
}

/// 5. 定时器事件流: once 触发一次且不阻塞调用者
inline asio::awaitable<void> test_timer_once() {

  auto bus = agentxx::middleware::EventBus{co_await asio::this_coro::executor};
  std::atomic<int> fireCount{0};

  auto id = bus.timer<TestEvent>().once(
      std::chrono::milliseconds(50),
      [&](const TestEvent &) -> asio::awaitable<void> {
        fireCount++;
        co_return;
      },
      TestEvent{.msg = "tick", .value = 1});
  XX_TEST_EXPECT_TRUE(id > 0);

  XX_TEST_EXPECT_TRUE(fireCount.load() == 0); // 立即返回, 未触发
  // 等待定时器到期
  auto timer = asio::steady_timer(co_await asio::this_coro::executor,
                                  std::chrono::milliseconds(200));
  co_await timer.async_wait(asio::use_awaitable);
  XX_TEST_EXPECT_TRUE(fireCount.load() == 1);

  co_return;
}

/// 6. EventBus 便捷方法 publish/request 与复用同 topic
inline asio::awaitable<void> test_eventbus_convenience() {

  auto bus = agentxx::middleware::EventBus{co_await asio::this_coro::executor};

  std::atomic<int> seen{0};
  bus.get<TestEvent>("conv.topic")
      .subscribe([&](const TestEvent &e) -> asio::awaitable<void> {
        seen += e.value;
        co_return;
      });

  co_await bus.publish<TestEvent>("conv.topic",
                                  TestEvent{.msg = "z", .value = 7});
  XX_TEST_EXPECT_TRUE(seen.load() == 7);
  // 同 topic 复用, 应是同一个流
  co_await bus.publish<TestEvent>("conv.topic",
                                  TestEvent{.msg = "z2", .value = 3});
  XX_TEST_EXPECT_TRUE(seen.load() == 10);

  auto &rr = bus.getRR<TestReq, TestResp>("conv.rr");
  rr.serve([](const TestReq &req, size_t) -> asio::awaitable<TestResp> {
    co_return TestResp{.answer = req.question + "!"};
  });
  auto resp = co_await bus.request<TestReq, TestResp>(
      "conv.rr", TestReq{.question = "hi"}, std::chrono::seconds(5));
  XX_TEST_EXPECT_TRUE(resp.has_value());
  XX_TEST_EXPECT_TRUE(resp.value().answer == "hi!");

  co_return;
}

inline asio::awaitable<TestResult> run_event_stream_tests() {
  try {
    co_await test_eventstream_publish();
    co_await test_requestresponse_normal();
    co_await test_requestresponse_timeout();
    co_await test_requestresponse_noserver();
    co_await test_timer_once();
    co_await test_eventbus_convenience();
  } catch (const std::exception &e) {
    TEST_FAIL << "event_stream suite exception: " << e.what() << std::endl;
    g_es_failed++;
  }
  co_return TestResult{g_es_passed, g_es_failed};
}

} // namespace test
} // namespace agentxx
