#include "test_concurrency.h"

#include "agentxx/agent/config.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/protocol/mcp_server.h"
#include "agentxx/util/async_mutex.h"
#include "agentxx/util/log.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace agentxx {
namespace test {

int g_conc_passed = 0;
int g_conc_failed = 0;

namespace {

// ---------------------------------------------------------------------------
// LogDispatcher: 无锁 copy-on-write 热路径 (多线程并发 dispatch + 注册/注销)
// ---------------------------------------------------------------------------

struct CountingSink : public agentxx::util::LogSink {
    std::atomic<size_t> count{0};

    void onLog(agentxx::util::LogLevel, const std::string&) override {
        count.fetch_add(1, std::memory_order_relaxed);
    }
};

void testLogDispatcherConcurrency() {
    using namespace agentxx::util;
    auto& disp = LogDispatcher::instance();

    constexpr int kSinks     = 3;
    constexpr int kThreads   = 8;
    constexpr int kPerThread = 500;

    std::vector<std::shared_ptr<CountingSink>> sinks;
    for (int i = 0; i < kSinks; ++i) {
        auto s = std::make_shared<CountingSink>();
        sinks.push_back(s);
        disp.addSink(s);
    }

    std::vector<size_t> baseline(kSinks);
    for (int i = 0; i < kSinks; ++i) {
        baseline[i] = sinks[i]->count.load();
    }

    std::atomic<bool>          start{false};
    std::vector<std::thread>   threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < kPerThread; ++i) {
                disp.dispatch(LogLevel::Info, "concurrent-msg");
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& th : threads) {
        th.join();
    }

    const size_t expected = static_cast<size_t>(kThreads) * kPerThread;
    for (int i = 0; i < kSinks; ++i) {
        // 每个 sink 收到全部日志 (无丢失), 且并发 dispatch 无崩溃
        XX_TEST_EXPECT_EQ(sinks[i]->count.load() - baseline[i], expected);
    }

    // 注销后不再收到
    disp.removeSink(sinks[0]);
    auto before = sinks[0]->count.load();
    disp.dispatch(LogLevel::Info, "after-remove");
    XX_TEST_EXPECT_EQ(sinks[0]->count.load(), before);

    for (int i = 1; i < kSinks; ++i) {
        disp.removeSink(sinks[i]);
    }
}

// ---------------------------------------------------------------------------
// ModelProviderRegistry: getProvider 双重检查锁 (并发取同一缓存实例) + 读写并发
// ---------------------------------------------------------------------------

void testModelRegistryConcurrency() {
    using namespace agentxx::agent;

    // (1) 并发 getProvider: 所有线程应拿到同一个缓存 Provider (双重检查锁正确)
    {
        ModelProviderRegistry reg;
        reg.registerModel("m1", ModelConfig{});

        constexpr int                              kThreads = 8;
        constexpr int                              kIter    = 200;
        std::atomic<bool>                          start{false};
        std::vector<std::thread>                   threads;
        std::vector<std::shared_ptr<neograph::Provider>> results(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t]() {
                while (!start.load(std::memory_order_acquire)) {
                }
                std::shared_ptr<neograph::Provider> p;
                for (int i = 0; i < kIter; ++i) {
                    p = reg.getProvider("m1");
                }
                results[t] = p;
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

    // (2) 读写并发: 多线程读 (resolve/getConfig/list/has) + 单线程写 (register), 不崩溃
    {
        ModelProviderRegistry reg;
        reg.registerModel("base", ModelConfig{});

        std::atomic<bool> stop{false};
        std::thread       writer([&]() {
            int i = 0;
            while (!stop.load(std::memory_order_acquire)) {
                reg.registerModel("dyn_" + std::to_string(i++), ModelConfig{});
            }
        });

        constexpr int            kReaders = 6;
        std::vector<std::thread> readers;
        for (int r = 0; r < kReaders; ++r) {
            readers.emplace_back([&]() {
                for (int i = 0; i < 2000; ++i) {
                    auto name = reg.resolveModelName("base");
                    auto cfg  = reg.getModelConfig(name);
                    auto list = reg.listModelNames();
                    auto has  = reg.hasModel("base");
                    (void)cfg;
                    (void)list;
                    (void)has;
                }
            });
        }
        for (auto& th : readers) {
            th.join();
        }
        stop.store(true, std::memory_order_release);
        writer.join();

        XX_TEST_EXPECT_TRUE(reg.size() >= 1);
        XX_TEST_EXPECT_TRUE(reg.hasModel("base"));
    }
}

// ---------------------------------------------------------------------------
// McpServer: 注册表并发 add/list (修复注册无锁数据竞争)
// ---------------------------------------------------------------------------

void testMcpServerRegistrationConcurrency() {
    using namespace agentxx::server;

    McpServer server;

    constexpr int          kThreads = 8;
    constexpr int          kIter    = 100;
    std::atomic<bool>      start{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < kIter; ++i) {
                McpToolDefinition def;
                def.name = "tool_" + std::to_string(t) + "_" + std::to_string(i);
                server.addTool(def, [](const json&) {
                    return json{};
                });
                auto list = server.listTools();
                (void)list;
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& th : threads) {
        th.join();
    }

    // 所有唯一 tool 均成功注册 (无丢失/无崩溃)
    XX_TEST_EXPECT_EQ(server.listTools().size(), static_cast<size_t>(kThreads * kIter));

    // resource / prompt 同样并发验证
    std::atomic<bool>        start2{false};
    std::vector<std::thread> threads2;
    for (int t = 0; t < kThreads; ++t) {
        threads2.emplace_back([&, t]() {
            while (!start2.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < kIter; ++i) {
                McpResourceDefinition res;
                res.uri = "res://" + std::to_string(t) + "/" + std::to_string(i);
                server.addResource(res, nullptr);
                McpPromptDefinition pr;
                pr.name = "prompt_" + std::to_string(t) + "_" + std::to_string(i);
                server.addPrompt(pr, nullptr);
                (void)server.listResources();
                (void)server.listPrompts();
            }
        });
    }
    start2.store(true, std::memory_order_release);
    for (auto& th : threads2) {
        th.join();
    }
    XX_TEST_EXPECT_EQ(server.listResources().size(), static_cast<size_t>(kThreads * kIter));
    XX_TEST_EXPECT_EQ(server.listPrompts().size(), static_cast<size_t>(kThreads * kIter));
}

// ---------------------------------------------------------------------------
// AsyncMutex: 协程互斥, 持锁跨越 co_await 仍保持互斥且不死锁
// ---------------------------------------------------------------------------

void testAsyncMutex() {
    using namespace agentxx::util;

    asio::io_context ioc;
    AsyncMutex       mtx(asio::any_io_executor(ioc.get_executor()));

    constexpr int kCoros = 20;
    constexpr int kIter  = 100;

    // 以下计数均为普通 int: 全部协程在单线程 ioc 上交错执行, 靠 AsyncMutex 保护
    int sharedCounter      = 0;
    int inCritical         = 0;
    int criticalViolations = 0;
    int done               = 0;

    for (int c = 0; c < kCoros; ++c) {
        asio::co_spawn(
            ioc,
            [&]() -> asio::awaitable<void> {
                for (int i = 0; i < kIter; ++i) {
                    auto guard = co_await mtx.lock();
                    // 临界区: 锁正确时 inCritical 恒为 1
                    if (++inCritical != 1) {
                        criticalViolations++;
                    }
                    sharedCounter++;
                    // 持锁跨越 co_await (若用 std::mutex 此处会死锁)
                    asio::steady_timer timer(co_await asio::this_coro::executor);
                    timer.expires_after(std::chrono::microseconds(1));
                    co_await timer.async_wait(asio::use_awaitable);
                    sharedCounter++;
                    inCritical--;
                    // guard 于本次迭代结束时析构释放锁
                }
                done++;
            },
            asio::detached
        );
    }

    ioc.run();

    XX_TEST_EXPECT_EQ(done, kCoros);
    XX_TEST_EXPECT_EQ(criticalViolations, 0);
    XX_TEST_EXPECT_EQ(sharedCounter, kCoros * kIter * 2);
}

} // namespace

TestResult testConcurrency() {
    int passed = 0;
    int failed = 0;

    testLogDispatcherConcurrency();
    testModelRegistryConcurrency();
    testMcpServerRegistrationConcurrency();
    testAsyncMutex();

    passed = g_conc_passed;
    failed = g_conc_failed;
    return TestResult{passed, failed};
}

} // namespace test
} // namespace agentxx
