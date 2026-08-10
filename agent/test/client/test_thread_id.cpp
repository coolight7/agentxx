#include "test_thread_id.h"

#include "agentxx-client/mode_runners.h"
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h> // GetCurrentProcessId
#else
#include <unistd.h> // getpid
#endif

namespace agentxx {
namespace test {

int g_thread_id_passed = 0;
int g_thread_id_failed = 0;

// ---------------------------------------------------------------------------
// generateUniqueThreadId 唯一性验证
// ---------------------------------------------------------------------------

namespace {

/// 按 '-' 拆分为段: [prefix, ts, pid, rnd, seq]
std::vector<std::string> splitId(const std::string& id) {
    std::vector<std::string> parts;
    std::string              cur;
    for (char c : id) {
        if (c == '-') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    return parts;
}

} // namespace

void test_format() {
    // 格式: "sess-<hex ts>-<pid 十进制>-<8位hex rnd>-<4位hex seq>"
    const std::string              id    = client::generateUniqueThreadId();
    const std::vector<std::string> parts = splitId(id);

    XX_TEST_EXPECT_EQ(parts.size(), (size_t)5);
    if (parts.size() == 5) {
        XX_TEST_EXPECT_EQ(parts[0], "sess");
        for (size_t i = 1; i < 5; ++i) {
            XX_TEST_EXPECT_FALSE(parts[i].empty());
            // 时间戳/随机数/序号为 hex, pid 为十进制; 十进制数字是 hex 字符子集,
            // 统一按 hex 字符集校验即可
            XX_TEST_EXPECT_TRUE(
                parts[i].find_first_not_of("0123456789abcdef") == std::string::npos
            );
        }
        // 随机数段固定 8 位, 序号段固定 4 位
        XX_TEST_EXPECT_EQ(parts[3].size(), (size_t)8);
        XX_TEST_EXPECT_EQ(parts[4].size(), (size_t)4);
    }
}

void test_uniqueness_many_calls() {
    // 同进程连续大量调用, 不允许重复
    std::set<std::string> seen;
    for (int i = 0; i < 10000; ++i) {
        const auto id = client::generateUniqueThreadId();
        XX_TEST_EXPECT_TRUE(seen.insert(id).second);
    }
}

void test_uniqueness_concurrent() {
    // 多线程并发调用, 不允许重复 (自增序号 + 时间戳保证线程安全)
    constexpr int            kThreads   = 8;
    constexpr int            kPerThread = 2000;
    std::vector<std::string> results;
    results.reserve(kThreads * kPerThread);
    std::mutex m;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) {
                const auto                  id = client::generateUniqueThreadId();
                std::lock_guard<std::mutex> lock(m);
                results.push_back(id);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    std::set<std::string> seen(results.begin(), results.end());
    XX_TEST_EXPECT_EQ(seen.size(), results.size());
}

void test_uniqueness_across_processes() {
    // 跨进程唯一性: id 中嵌入的 PID 段与当前进程 PID 一致 ⇒ 不同进程的 id 必不同
    // (不同进程 PID 不同; 单测内无法真跨进程, 校验 PID 段即可)
#ifdef _WIN32
    const long pid = static_cast<long>(::GetCurrentProcessId());
#else
    const long pid = static_cast<long>(::getpid());
#endif
    const std::string              id    = client::generateUniqueThreadId();
    const std::vector<std::string> parts = splitId(id);

    XX_TEST_EXPECT_EQ(parts.size(), (size_t)5);
    if (parts.size() == 5) {
        XX_TEST_EXPECT_EQ(parts[2], std::to_string(pid));
    }
}

TestResult testThreadId() {
    g_thread_id_passed = 0;
    g_thread_id_failed = 0;

    test_format();
    test_uniqueness_many_calls();
    test_uniqueness_concurrent();
    test_uniqueness_across_processes();

    return TestResult{g_thread_id_passed, g_thread_id_failed};
}

} // namespace test
} // namespace agentxx
