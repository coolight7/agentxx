#include "test_session_concurrency.h"

#include "agentxx/agent/context.h"
#include <atomic>
#include <chrono>
#include <fmt/format.h>
#include <thread>
#include <vector>

namespace agentxx {
namespace test {

int g_sc_passed = 0;
int g_sc_failed = 0;

/// 测试 1: writer 绑定 io 线程持续 appendHistory, 多 reader 并发 getFullHistoryCopy
/// 验证: 快照 size 单调递增、内容连续完整 (不会读到半写状态)
static void test_concurrent_history_snapshot() {
    using namespace agentxx::agent;

    auto session = std::make_shared<Session>();

    // 在 "io 线程" 上绑定并写入
    constexpr int kTotalMessages = 2000;
    std::atomic<bool> writerDone{false};
    std::atomic<bool> consistent{true};

    std::thread writer([&]() {
        session->bindIoThread();
        for (int i = 0; i < kTotalMessages; ++i) {
            neograph::json msg = {
                {"role", "user"},
                {"content", fmt::format("msg_{}", i)}
            };
            session->appendHistory(std::move(msg));
        }
        writerDone.store(true, std::memory_order_release);
    });

    // 多个 reader 线程并发读取快照 (模拟 UI 线程)
    constexpr int kReaders = 4;
    std::vector<std::thread> readers;
    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&]() {
            size_t lastSize = 0;
            while (!writerDone.load(std::memory_order_acquire)) {
                auto snap = session->getFullHistoryCopy();
                // size 必须单调递增
                if (snap.size() < lastSize) {
                    consistent.store(false, std::memory_order_relaxed);
                    return;
                }
                // 内容连续性: 每条消息的 content 应为 "msg_{index}"
                for (size_t i = 0; i < snap.size(); ++i) {
                    auto expected = fmt::format("msg_{}", i);
                    if (snap[i].data.value("content", "") != expected) {
                        consistent.store(false, std::memory_order_relaxed);
                        return;
                    }
                }
                lastSize = snap.size();
            }
        });
    }

    writer.join();
    for (auto& th : readers) {
        th.join();
    }

    XX_TEST_EXPECT_TRUE(consistent.load());
    XX_TEST_EXPECT_EQ(session->getFullHistoryCopy().size(), size_t{kTotalMessages});
}

/// 测试 2: 并发读取 getHashInfo 快照一致性
/// 验证: count 单调递增, tailHex 长度恒为 16 (或空)
static void test_concurrent_hash_snapshot() {
    using namespace agentxx::agent;

    auto session = std::make_shared<Session>();

    constexpr int kTotalMessages = 1000;
    std::atomic<bool> writerDone{false};
    std::atomic<bool> hashOk{true};

    std::thread writer([&]() {
        session->bindIoThread();
        for (int i = 0; i < kTotalMessages; ++i) {
            session->appendHistory(neograph::json{
                {"role", "assistant"},
                {"content", fmt::format("h{}", i)}
            });
        }
        writerDone.store(true, std::memory_order_release);
    });

    constexpr int kReaders = 3;
    std::vector<std::thread> readers;
    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&]() {
            size_t lastCount = 0;
            while (!writerDone.load(std::memory_order_acquire)) {
                auto info = session->getHashInfo();
                if (info.count < lastCount) {
                    hashOk.store(false, std::memory_order_relaxed);
                    return;
                }
                // tailHex: 有消息时应为 16 位十六进制, 无消息时为空
                if (info.count > 0 && info.tailHex.size() != 16) {
                    hashOk.store(false, std::memory_order_relaxed);
                    return;
                }
                if (info.count == 0 && !info.tailHex.empty()) {
                    hashOk.store(false, std::memory_order_relaxed);
                    return;
                }
                lastCount = info.count;
            }
        });
    }

    writer.join();
    for (auto& th : readers) {
        th.join();
    }

    XX_TEST_EXPECT_TRUE(hashOk.load());
    auto finalInfo = session->getHashInfo();
    XX_TEST_EXPECT_EQ(finalInfo.count, size_t{kTotalMessages});
    XX_TEST_EXPECT_EQ(finalInfo.tailHex.size(), 16u);
}

/// 测试 3: 并发读取 activity 原子状态 + deltaSeq
/// 验证: activity 始终为合法枚举值, deltaSeq 单调递增
static void test_concurrent_activity_and_deltaSeq() {
    using namespace agentxx::agent;

    auto session = std::make_shared<Session>();

    constexpr int kIter = 50000;
    std::atomic<bool> stateOk{true};
    std::atomic<bool> seqOk{true};

    // writer: 循环切换 activity 状态并递增 deltaSeq
    std::thread writer([&]() {
        for (int i = 0; i < kIter; ++i) {
            session->activity.store(Activity::Streaming, std::memory_order_relaxed);
            session->deltaSeq.fetch_add(1, std::memory_order_relaxed);
            session->activity.store(Activity::ExecutingTool, std::memory_order_relaxed);
            session->deltaSeq.fetch_add(1, std::memory_order_relaxed);
            session->activity.store(Activity::Idle, std::memory_order_relaxed);
        }
    });

    // readers: 并发读取 activity 和 deltaSeq
    constexpr int kReaders = 4;
    std::vector<std::thread> readers;
    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&]() {
            uint64_t lastSeq = 0;
            for (int i = 0; i < kIter; ++i) {
                auto a = session->activity.load(std::memory_order_relaxed);
                if (a != Activity::Idle && a != Activity::Streaming &&
                    a != Activity::ExecutingTool && a != Activity::WaitingInput) {
                    stateOk.store(false, std::memory_order_relaxed);
                    return;
                }
                auto seq = session->getDeltaSeq();
                if (seq < lastSeq) {
                    seqOk.store(false, std::memory_order_relaxed);
                    return;
                }
                lastSeq = seq;
            }
        });
    }

    writer.join();
    for (auto& th : readers) {
        th.join();
    }

    XX_TEST_EXPECT_TRUE(stateOk.load());
    XX_TEST_EXPECT_TRUE(seqOk.load());
    XX_TEST_EXPECT_EQ(session->getDeltaSeq(), uint64_t{kIter * 2});
}

/// 测试 4: SessionStore 单线程 getOrCreate/get/remove 基本正确性
/// (SessionStore 设计为仅 io 线程访问, 此处验证功能正确)
static void test_session_store_basic() {
    using namespace agentxx::agent;

    SessionStore store;

    auto s1 = store.getOrCreate("thread_a");
    auto s2 = store.getOrCreate("thread_b");
    auto s3 = store.get("thread_a");

    XX_TEST_EXPECT_TRUE(s1 != nullptr);
    XX_TEST_EXPECT_TRUE(s2 != nullptr);
    XX_TEST_EXPECT_TRUE(s1 == s3);   // 同一 thread_id 返回同一实例
    XX_TEST_EXPECT_TRUE(s1 != s2);   // 不同 thread_id 不同实例

    store.remove("thread_a");
    XX_TEST_EXPECT_TRUE(store.get("thread_a") == nullptr);
    XX_TEST_EXPECT_TRUE(store.get("thread_b") != nullptr);

    // 移除后重新创建应为新实例
    auto s4 = store.getOrCreate("thread_a");
    XX_TEST_EXPECT_TRUE(s4 != nullptr);
    XX_TEST_EXPECT_TRUE(s4 != s1);
}

TestResult testSessionConcurrentAccess() {
    g_sc_passed = 0;
    g_sc_failed = 0;

    test_concurrent_history_snapshot();
    test_concurrent_hash_snapshot();
    test_concurrent_activity_and_deltaSeq();
    test_session_store_basic();

    return TestResult{g_sc_passed, g_sc_failed};
}

} // namespace test
} // namespace agentxx
