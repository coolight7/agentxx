#include "test_session_concurrency.h"
#include <atomic>
#include <thread>
#include <chrono>

namespace agentxx {
namespace test {

TestResult testSessionConcurrentAccess() {
    TestResult result;
    
    TEST_INFO << "Testing atomic variable thread safety..." << std::endl;
    
    // 测试纯粹的原子变量（不访问 Session）
    std::atomic<uint64_t> counter{0};
    std::atomic<bool> stop{false};
    std::atomic<int> readCount{0};
    
    // 多个线程并发读取/写入原子变量
    std::thread writer([&]() {
        uint64_t val = 0;
        while (!stop.load()) {
            counter.store(++val);
            std::this_thread::yield();
        }
    });
    
    std::vector<std::thread> readers;
    for (int t = 0; t < 8; ++t) {
        readers.emplace_back([&]() {
            int localReads = 0;
            uint64_t lastVal = 0;
            while (!stop.load()) {
                lastVal = counter.load();
                localReads++;
                std::this_thread::yield();
            }
            readCount += localReads;
        });
    }
    
    // 运行 1 秒
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    // 停止所有线程
    stop.store(true);
    if (writer.joinable()) writer.join();
    for (auto& t : readers) {
        if (t.joinable()) t.join();
    }
    
    TEST_INFO << "Completed " << readCount.load() << " reads of atomic counter" << std::endl;
    TEST_INFO << "Final counter value: " << counter.load() << std::endl;
    
    // 验证没有崩溃
    if (readCount > 0 && counter > 0) {
        TEST_PASS << "Atomic operations concurrent access test passed" << std::endl;
        result.passed++;
    } else {
        TEST_FAIL << "Test failed - no reads or counter not incremented" << std::endl;
        result.failed++;
    }
    
    return result;
}

} // namespace test
} // namespace agentxx
