#include "test_lockless.h"

#include "agentxx/agent/context.h"
#include <atomic>
#include <thread>
#include <vector>
#include <fmt/format.h>

int g_lockless_passed = 0;
int g_lockless_failed = 0;

namespace agentxx {
namespace test {

TestResult testSessionStoreLockless() {
    g_lockless_passed = 0;
    g_lockless_failed = 0;

    // 测试 1: 单线程场景下的基本操作（符合设计保证）
    {
        using namespace agentxx::agent;
        auto store = std::make_shared<SessionStore>();

        auto s1 = store->getOrCreate("thread_1");
        auto s2 = store->get("thread_1");
        auto s3 = store->getOrCreate("thread_2");

        XX_TEST_EXPECT_TRUE(s1 && s2 && s3);
        XX_TEST_EXPECT_TRUE(s1 == s2);
        XX_TEST_EXPECT_TRUE(s1 != s3);

        store->remove("thread_1");
        XX_TEST_EXPECT_TRUE(store->get("thread_1") == nullptr);
        XX_TEST_EXPECT_TRUE(store->get("thread_2") != nullptr);
    }

    // 测试 2: 多线程并发创建/获取 - 验证无竞争条件
    {
        using namespace agentxx::agent;
        auto store = std::make_shared<SessionStore>();

        constexpr int kThreads = 8;
        constexpr int kPerThread = 100;

        std::atomic<bool> start{false};
        std::vector<std::thread> threads;
        std::vector<std::shared_ptr<Session>> results(kThreads);

        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t]() {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                std::shared_ptr<Session> sess;
                for (int i = 0; i < kPerThread; ++i) {
                    sess = store->getOrCreate("shared_thread");
                }
                results[t] = sess;
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& th : threads) {
            th.join();
        }

        XX_TEST_EXPECT_TRUE(results[0] != nullptr);
        for (int t = 1; t < kThreads; ++t) {
            XX_TEST_EXPECT_TRUE(results[t] == results[0]);
        }
    }

    return TestResult{g_lockless_passed, g_lockless_failed};
}

TestResult testSessionDoubleBuffer() {
    g_lockless_passed = 0;
    g_lockless_failed = 0;

    using namespace agentxx::agent;

    // 测试 1: 单线程写入后读取快照一致性
    {
        auto session = std::make_shared<Session>();

        for (int i = 0; i < 100; ++i) {
            neograph::json msg = {
                {"role", "user"},
                {"content", fmt::format("msg_{}", i)}
            };
            session->appendHistory(std::move(msg));
        }

        auto snapshot1 = session->getFullHistoryCopy();
        auto snapshot2 = session->getFullHistoryCopy();

        XX_TEST_EXPECT_EQ(snapshot1.size(), size_t{100});
        XX_TEST_EXPECT_EQ(snapshot2.size(), size_t{100});
        XX_TEST_EXPECT_EQ(snapshot1[0].data.value("content", ""), "msg_0");
        XX_TEST_EXPECT_EQ(snapshot2.back().data.value("content", ""), "msg_99");
    }

    // 测试 2: 真正的并发读写 - writer 持续 append，reader 持续读取快照
    // 验证: 快照 size 单调递增、内容连续完整（不会读到半写状态）
    {
        auto session = std::make_shared<Session>();
        std::atomic<bool> done{false};
        std::atomic<bool> consistent{true};

        std::thread writer([&]() {
            for (int i = 0; i < 1000; ++i) {
                neograph::json msg = {
                    {"role", "assistant"},
                    {"content", fmt::format("m{}", i)}
                };
                session->appendHistory(std::move(msg));
            }
            done.store(true, std::memory_order_release);
        });

        std::thread reader([&]() {
            size_t lastSize = 0;
            while (!done.load(std::memory_order_acquire)) {
                auto snap = session->getFullHistoryCopy();
                if (snap.size() < lastSize) {
                    consistent.store(false, std::memory_order_relaxed);
                    break;
                }
                for (size_t i = 0; i < snap.size(); ++i) {
                    auto expected = fmt::format("m{}", i);
                    if (snap[i].data.value("content", "") != expected) {
                        consistent.store(false, std::memory_order_relaxed);
                        break;
                    }
                }
                lastSize = snap.size();
            }
        });

        writer.join();
        reader.join();

        XX_TEST_EXPECT_TRUE(consistent.load());
        XX_TEST_EXPECT_EQ(session->getFullHistoryCopy().size(), size_t{1000});
    }

    // 测试 3: 多 reader 并发读取同一 session 快照
    {
        auto session = std::make_shared<Session>();
        for (int i = 0; i < 50; ++i) {
            session->appendHistory(neograph::json{{"role", "user"}, {"content", fmt::format("x{}", i)}});
        }

        constexpr int kReaders = 4;
        std::atomic<bool> allOk{true};
        std::vector<std::thread> readers;

        for (int r = 0; r < kReaders; ++r) {
            readers.emplace_back([&]() {
                for (int iter = 0; iter < 200; ++iter) {
                    auto snap = session->getFullHistoryCopy();
                    if (snap.size() != 50) {
                        allOk.store(false, std::memory_order_relaxed);
                        return;
                    }
                }
            });
        }
        for (auto& th : readers) {
            th.join();
        }

        XX_TEST_EXPECT_TRUE(allOk.load());
    }

    return TestResult{g_lockless_passed, g_lockless_failed};
}

TestResult testContextStatsAtomic() {
    g_lockless_passed = 0;
    g_lockless_failed = 0;

    using namespace agentxx::agent;

    // ContextStats 内部使用 std::atomic，验证多线程并发读写安全
    {
        auto stats = std::make_shared<ContextStats>();

        constexpr int kWriters = 4;
        constexpr int kPerWriter = 10000;
        std::atomic<bool> start{false};
        std::vector<std::thread> threads;

        for (int w = 0; w < kWriters; ++w) {
            threads.emplace_back([&, w]() {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (int i = 0; i < kPerWriter; ++i) {
                    stats->contextTokens.store(
                        stats->contextTokens.load(std::memory_order_relaxed) + 1,
                        std::memory_order_relaxed);
                }
            });
        }

        // 并发 reader 不应崩溃或读到撕裂值
        std::atomic<bool> readOk{true};
        std::thread reader([&]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < 50000; ++i) {
                size_t val = stats->contextTokens.load(std::memory_order_relaxed);
                if (val > size_t{kWriters * kPerWriter}) {
                    readOk.store(false, std::memory_order_relaxed);
                    break;
                }
            }
        });

        start.store(true, std::memory_order_release);
        for (auto& th : threads) {
            th.join();
        }
        reader.join();

        XX_TEST_EXPECT_TRUE(readOk.load());
        XX_TEST_EXPECT_TRUE(stats->contextTokens.load() <= size_t{kWriters * kPerWriter});
    }

    // Activity 原子状态转换
    {
        auto session = std::make_shared<Session>();
        XX_TEST_EXPECT_TRUE(session->activity.load() == Activity::Idle);

        std::atomic<bool> stateOk{true};
        std::thread writer([&]() {
            for (int i = 0; i < 10000; ++i) {
                session->activity.store(Activity::Streaming, std::memory_order_relaxed);
                session->activity.store(Activity::ExecutingTool, std::memory_order_relaxed);
                session->activity.store(Activity::Idle, std::memory_order_relaxed);
            }
        });

        std::thread reader([&]() {
            for (int i = 0; i < 10000; ++i) {
                auto a = session->activity.load(std::memory_order_relaxed);
                if (a != Activity::Idle && a != Activity::Streaming &&
                    a != Activity::ExecutingTool && a != Activity::WaitingInput) {
                    stateOk.store(false, std::memory_order_relaxed);
                    break;
                }
            }
        });

        writer.join();
        reader.join();

        XX_TEST_EXPECT_TRUE(stateOk.load());
    }

    return TestResult{g_lockless_passed, g_lockless_failed};
}

TestResult testSessionMutexFields() {
    g_lockless_passed = 0;
    g_lockless_failed = 0;

    using namespace agentxx::agent;

    // cancelToken / modelName 使用 mutex 保护，验证多线程并发 set/get 安全
    {
        auto session = std::make_shared<Session>();

        constexpr int kThreads = 4;
        constexpr int kIter = 5000;
        std::atomic<bool> start{false};
        std::vector<std::thread> threads;

        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t]() {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                auto name = fmt::format("model_{}", t);
                for (int i = 0; i < kIter; ++i) {
                    session->setModelName(name);
                    auto got = session->getModelName();
                    if (got.substr(0, 6) != "model_") {
                        return;
                    }
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& th : threads) {
            th.join();
        }

        auto final_ = session->getModelName();
        XX_TEST_EXPECT_TRUE(final_.substr(0, 6) == "model_");
    }

    // getHashInfo 边界: 空 history
    {
        auto session = std::make_shared<Session>();
        auto info = session->getHashInfo();
        XX_TEST_EXPECT_EQ(info.count, size_t{0});
        XX_TEST_EXPECT_TRUE(info.tailHex.empty());
    }

    // getHashInfo 写入后一致性
    {
        auto session = std::make_shared<Session>();
        for (int i = 0; i < 10; ++i) {
            session->appendHistory(neograph::json{{"role", "user"}, {"content", fmt::format("h{}", i)}});
        }
        auto info = session->getHashInfo();
        XX_TEST_EXPECT_EQ(info.count, size_t{10});
        XX_TEST_EXPECT_FALSE(info.tailHex.empty());
    }

    return TestResult{g_lockless_passed, g_lockless_failed};
}

TestResult testLockless() {
    TestResult result;

    auto r1 = testSessionStoreLockless();
    result += r1;

    auto r2 = testSessionDoubleBuffer();
    result += r2;

    auto r3 = testContextStatsAtomic();
    result += r3;

    auto r4 = testSessionMutexFields();
    result += r4;

    return result;
}

} // namespace test
} // namespace agentxx
