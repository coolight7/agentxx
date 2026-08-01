#include "test_concurrency.h"

#include "agentxx/agent/config.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/protocol/mcp_server.h"
#include "agentxx/util/async_mutex.h"
#include "agentxx/util/async_offload.h"
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

    void onLog(const agentxx::util::LogEntry&) override {
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

    std::atomic<bool>        start{false};
    std::vector<std::thread> threads;
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

    // dispatch 仅入队, 需 pump 消费后 onLog 才执行
    for (int i = 0; i < kSinks; ++i) {
        sinks[i]->pump();
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
// ModelProviderRegistry: 单线程功能验证 (已改为单线程设计, 仅 agent io 线程访问)
// ---------------------------------------------------------------------------

void testModelRegistryConcurrency() {
    using namespace agentxx::agent;

    // (1) getProvider 缓存: 多次调用返回同一实例
    {
        ModelProviderRegistry reg;
        reg.registerModel("m1", ModelConfig{});

        auto p1 = reg.getProvider("m1");
        auto p2 = reg.getProvider("m1");

        XX_TEST_EXPECT_TRUE(p1 != nullptr);
        XX_TEST_EXPECT_TRUE(p1 == p2);
    }

    // (2) 注册/读取/解析功能正确性
    {
        ModelProviderRegistry reg;
        reg.registerModel("base", ModelConfig{});

        for (int i = 0; i < 100; ++i) {
            reg.registerModel("dyn_" + std::to_string(i), ModelConfig{});
        }

        auto name = reg.resolveModelName("base");
        auto cfg  = reg.getModelConfig(name);
        auto list = reg.listModelNames();
        auto has  = reg.hasModel("base");
        (void)cfg;

        XX_TEST_EXPECT_TRUE(reg.size() >= 1);
        XX_TEST_EXPECT_TRUE(has);
        XX_TEST_EXPECT_TRUE(list.size() == 101);
        XX_TEST_EXPECT_TRUE(name == "base");
    }
}

// ---------------------------------------------------------------------------
// McpServer: 注册表并发 add/list (修复注册无锁数据竞争)
// ---------------------------------------------------------------------------

void testMcpServerRegistrationConcurrency() {
    using namespace agentxx::server;

    McpServer server;

    constexpr int            kThreads = 8;
    constexpr int            kIter    = 100;
    std::atomic<bool>        start{false};
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

// 回归: Guard 移动赋值须先归还已持有的令牌, 否则令牌泄漏导致该锁永久死锁。
// (修复前 operator=(Guard&&) 直接覆盖 ch_, 未 try_send 归还旧令牌)
void testAsyncMutexGuardMoveAssign() {
    using namespace agentxx::util;

    asio::io_context ioc;
    auto             ex = asio::any_io_executor(ioc.get_executor());
    AsyncMutex       mtx1(ex);
    AsyncMutex       mtx2(ex);

    bool mainDone     = false;
    bool relockedMtx1 = false;

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto guardA = co_await mtx1.lock(); // 持有 mtx1 令牌
            {
                auto guardB = co_await mtx2.lock(); // 持有 mtx2 令牌
                guardA      = std::move(guardB); // 移动赋值: 修复后须先归还 mtx1 令牌
            }
            mainDone = true;
            // guardA (现持 mtx2) 于协程结束析构, 归还 mtx2
        },
        asio::detached
    );

    // 等主协程完成移动赋值后尝试再锁 mtx1: 修复后令牌已归还 → 立即成功; 泄漏则挂起
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            asio::steady_timer t(co_await asio::this_coro::executor);
            t.expires_after(std::chrono::milliseconds(10));
            co_await t.async_wait(asio::use_awaitable);
            if (!mainDone) {
                co_return;
            }
            auto g       = co_await mtx1.lock();
            relockedMtx1 = true;
        },
        asio::detached
    );

    // 看门狗: 超时停止 io_context, 防止令牌泄漏时 lock 永久挂起拖死测试
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            asio::steady_timer t(co_await asio::this_coro::executor);
            t.expires_after(std::chrono::milliseconds(300));
            co_await t.async_wait(asio::use_awaitable);
            ioc.stop();
        },
        asio::detached
    );

    ioc.run();

    XX_TEST_EXPECT_TRUE(mainDone);
    XX_TEST_EXPECT_TRUE(relockedMtx1); // 修复前为 false (mtx1 令牌泄漏, 再锁挂起)
}

// ---------------------------------------------------------------------------
// AsyncOffload: 阻塞任务卸载到线程池
// ---------------------------------------------------------------------------

void testAsyncOffload() {
    using namespace agentxx::util;

    // 1. offloadAsync: 卸载到线程池执行并返回结果
    {
        asio::io_context  ioc;
        asio::thread_pool pool(2);
        std::atomic<bool> ranOnWorker{false};

        asio::co_spawn(
            ioc,
            [&]() -> asio::awaitable<void> {
                auto v = co_await agentxx::util::offloadAsync<int>(
                    pool,
                    [&]() -> asio::awaitable<int> {
                        ranOnWorker.store(true, std::memory_order_release);
                        co_return 42;
                    }
                );
                XX_TEST_EXPECT_EQ(v, 42);
            },
            asio::detached
        );
        ioc.run();
        pool.join();
        XX_TEST_EXPECT_TRUE(ranOnWorker.load());
    }

    // 2. offloadCancellableAsync: 正常完成返回结果 (未取消)
    {
        asio::io_context  ioc;
        asio::thread_pool pool(1);
        asio::co_spawn(
            ioc,
            [&]() -> asio::awaitable<void> {
                auto v = co_await agentxx::util::offloadCancellableAsync<int>(
                    pool,
                    [](std::atomic<bool>&) -> asio::awaitable<int> {
                        co_return 7;
                    }
                );
                XX_TEST_EXPECT_EQ(v, 7);
            },
            asio::detached
        );
        ioc.run();
        pool.join();
    }

    // 3. offloadCancellableAsync 外部 cancelFlag 版本: 工作线程轮询检测取消并提前退出
    {
        asio::io_context  ioc;
        asio::thread_pool pool(1);
        auto              flag = std::make_shared<std::atomic<bool>>(false);
        std::atomic<bool> workerSawCancel{false};

        asio::co_spawn(
            ioc,
            [&]() -> asio::awaitable<void> {
                bool threw = false;
                try {
                    co_await agentxx::util::offloadCancellableAsync<int>(
                        pool,
                        flag,
                        [&](std::atomic<bool>& cancel_flag) -> asio::awaitable<int> {
                            // 工作线程: 轮询外部取消标志
                            while (!cancel_flag.load(std::memory_order_acquire)) {
                                std::this_thread::yield();
                            }
                            workerSawCancel.store(true, std::memory_order_release);
                            throw neograph::graph::CancelledException("external cancel");
                        }
                    );
                } catch (const neograph::graph::CancelledException&) {
                    threw = true;
                }
                XX_TEST_EXPECT_TRUE(threw);
            },
            asio::detached
        );

        // 稍后从外部设置取消标志
        std::thread setter([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            flag->store(true, std::memory_order_release);
        });
        ioc.run();
        setter.join();
        pool.join();

        XX_TEST_EXPECT_TRUE(workerSawCancel.load());
    }
}

} // namespace

TestResult testConcurrency() {
    g_conc_passed = 0;
    g_conc_failed = 0;

    testLogDispatcherConcurrency();
    testModelRegistryConcurrency();
    testMcpServerRegistrationConcurrency();
    testAsyncMutex();
    testAsyncMutexGuardMoveAssign();
    testAsyncOffload();

    return TestResult{g_conc_passed, g_conc_failed};
}

} // namespace test
} // namespace agentxx
