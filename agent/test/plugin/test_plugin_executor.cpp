///
/// 插件执行器 (插件独立线程) —— "主程序 → 插件"回调的线程归属
///
/// 背景: 接口表异步化解决的是"插件 → 主程序"方向; 本模块验证另一半 ——
/// 主程序调插件的回调不再落在 io 线程上, 而在管理器自己的插件执行器里串行执行。
///
/// 覆盖:
/// 1. 派发落到插件线程, 同实例严格 FIFO, `isPluginThread` 判定正确
/// 2. lease 在派发时取 (排队中也算在途); 实例进入关闭后新派发被丢弃并计数
/// 3. 回调耗时统计与超预算计数
/// 4. `OpCore` 的 start / 完成回调 / 取消三处都在插件线程执行
/// 5. 生命周期入口 (`awaitPluginLifecycle`, start/stop/mount/unmount 的公共底座)
/// 6. 协程驱动票在插件线程推进
/// 7. 广播订阅处理器在插件线程执行
/// 8. 插件执行器停止后派发记为 unschedulable, 不会悬挂
/// 9. 同步桥: 插件执行线程同步等 io 时, 完成回调在完成线程就地调用 (不自等)
#include "agentxx-test/plugin/test_plugin_executor.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/context.h"
#include "agentxx/plugin/api/agent_plugin_kit.h"
#include "agentxx/plugin/api/client_plugin_kit.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/tool_registry.h"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "pluginxx/runtime/driver.h"
#include "pluginxx/runtime/op_driver.h"
#include "pluginxx/runtime/plugin_thread.h"
#include "pluginxx/host/table_op.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
int g_pe_passed = 0;
int g_pe_failed = 0;
} // namespace

#define XX_TEST_PASSED g_pe_passed
#define XX_TEST_FAILED g_pe_failed

/// 真实插件加载路径用例的辅助 (全局匿名命名空间: 与其它测试 TU 无符号冲突)
namespace {

/// 定位插件目录 (与 `test_agent_host.cpp` / `test_plugins.cpp` 同一策略)
std::string findPluginDirForExecutor(std::string_view pluginName) {
    namespace fs = std::filesystem;
    const std::string     name{pluginName};
    std::error_code       ec;
    std::vector<fs::path> candidates;
    if (auto exeDir = agentxx::test::executableDir()) {
        candidates.push_back(*exeDir / "plugins" / name);
    }    candidates.push_back(fs::current_path(ec) / "plugins" / name);
    auto hasPluginFiles = [](const fs::path& dir) {
        std::error_code                     ec2;
        std::filesystem::directory_iterator it(dir, ec2);
        std::filesystem::directory_iterator end;
        for (; it != end; it.increment(ec2)) {
            const auto ext = it->path().extension().string();
            if (ext == ".dll" || ext == ".so" || ext == ".dylib") {
                return true;
            }
        }
        return fs::exists(dir / "plugin.yaml", ec2);
    };
    for (const auto& dir : candidates) {
        if (fs::is_directory(dir, ec) && hasPluginFiles(dir)) {
            return dir.string();
        }
    }
    return {};
}

// =====================================================================
// 9. 真实 AgentHost + 真实插件: start 事务在插件线程上跑通
// =====================================================================
/// `agentxx_javascript_engine` 的 start 事务里要登记一条能力行: 配了插件执行器时
/// 它跑在插件线程上, 因此不能用同步表助手 (完成回调会投回这条线程), 必须写异步形态。
/// 本用例走真实加载路径 (`AgentHost::loadPluginsAsync` → 插件线程 → start), 验证
/// 能力行真的登记成功。
asio::awaitable<void> test_real_host_js_engine_start() {
    const std::string engineDir = findPluginDirForExecutor("agentxx_javascript_engine");
    const std::string jsDir     = findPluginDirForExecutor("example_js");
    if (engineDir.empty() || jsDir.empty()) {
        TEST_INFO << "javascript engine / example_js plugin not found, skip real-host test"
                  << std::endl;
        co_return;
    }

    auto                               io = std::make_shared<asio::io_context>();
    agentxx::agent::AgentHost::Config  hostCfg;
    hostCfg.ioCtx = io;
    hostCfg.pluginList.push_back(agentxx::agent::PluginConfig{.path = engineDir});
    hostCfg.pluginList.push_back(agentxx::agent::PluginConfig{.path = jsDir});

    std::atomic<bool> loaded{false};
    bool              hasExecutor   = false;
    bool              hasCapability = false;
    bool              engineRunning = false;

    asio::co_spawn(
        *io,
        [&]() -> asio::awaitable<void> {
            auto work = asio::make_work_guard(*io);
            auto host = agentxx::agent::AgentHost::create(hostCfg);
            if (!host) {
                loaded.store(true);
                co_return;
            }
            co_await host->loadPluginsAsync();
            auto mgr    = host->pluginHost();
            hasExecutor = mgr && mgr->hasPluginExecutor();
            if (mgr) {
                hasCapability = mgr->hasCapability(
                                    "plugin.agentxx_javascript_engine.interpreter.js"
                                )
                                == 1;
                for (const auto& view : mgr->list()) {
                    if (view.name == "agentxx_javascript_engine" && view.enabled
                        && !view.capabilities.empty()) {
                        engineRunning = true;
                    }
                }
                co_await mgr->shutdownAsync();
            }
            host.reset();
            loaded.store(true);
            co_return;
        }(),
        asio::detached
    );

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{90};
    while (!loaded.load() && std::chrono::steady_clock::now() < deadline) {
        io->run_for(std::chrono::milliseconds{10});
        io->restart();
    }
    XX_TEST_EXPECT_TRUE(loaded.load());
    XX_TEST_EXPECT_TRUE(hasExecutor);
    XX_TEST_EXPECT_TRUE(hasCapability);
    XX_TEST_EXPECT_TRUE(engineRunning);
}

} // namespace // 全局匿名命名空间

namespace agentxx {
namespace test {

namespace {

using namespace std::chrono_literals;

/// 一个简易的一次性同步点 (阻塞等待只发生在测试线程上, 不在插件线程上)
class Latch {
public:

    void signal() {
        {
            std::lock_guard lock(mutex_);
            done_ = true;
        }
        cv_.notify_all();
    }

    bool wait(int timeoutMs) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds{timeoutMs}, [this] {
            return done_;
        });
    }

private:

    std::mutex              mutex_;
    std::condition_variable cv_;
    bool                    done_ = false;
};

/// 等条件成立 (只推进 io 线程上的定时器, 不阻塞插件线程)
asio::awaitable<bool> waitFor(std::function<bool()> cond, int timeoutMs = 5000) {
    auto       ex       = co_await asio::this_coro::executor;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMs};
    asio::steady_timer timer{ex};
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            co_return true;
        }
        timer.expires_after(1ms);
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
    }
    co_return cond();
}

/// 用例夹具: 一条插件执行线程 + 一个管理器 + 一个伪实例
///
/// 实例是手工构造的 (没有动态库): 只加载 lifetime / runtime, 与加载器给真实实例
/// 准备的那部分一致, 足以驱动派发、Operation 与生命周期入口。
struct Fixture {
    std::shared_ptr<pluginxx::PluginExecutorThread>  thread;
    std::shared_ptr<agentxx::agent::AgentContext>    ctx;
    std::shared_ptr<agentxx::plugin::PluginManager>  mgr;
    std::shared_ptr<agentxx::plugin::PluginInstance> inst;
    /// 插件线程自己的线程 id (插件回调里记录到它才算"跑在插件线程上")
    std::atomic<std::thread::id> pluginThreadId{};

    ~Fixture() {
        // 顺序: 管理器 (卸载/关闭) → 插件线程停 → io
        inst.reset();
        mgr.reset();
        if (thread) {
            thread->stop();
        }
        thread.reset();
        ctx.reset();
    }

    /// 启动夹具 (**必须是 io 线程上的协程**: 记录 io 线程 id 靠它)
    asio::awaitable<bool> start(std::string name = "provider") {
        auto ex = co_await asio::this_coro::executor;

        thread = std::make_shared<pluginxx::PluginExecutorThread>();
        if (!thread->start("agentxx-test-plugin")) {
            co_return false;
        }

        ctx               = std::make_shared<agentxx::agent::AgentContext>();
        ctx->toolRegistry = std::make_shared<agentxx::plugin::ToolRegistry>();
        mgr               = std::make_shared<agentxx::plugin::PluginManager>(
            std::static_pointer_cast<agentxx::agent::AgentContext>(ctx)
        );
        ctx->pluginManager = mgr;
        mgr->setIoExecutor(ex);
        mgr->setPluginExecutor(thread->executor());

        inst            = std::make_shared<agentxx::plugin::PluginInstance>(std::move(name));
        inst->self      = inst;
        inst->ownerSelf = inst;
        inst->manager   = mgr;
        inst->runtime   = mgr->runtime();
        const std::weak_ptr<pluginxx::PluginRuntime> runtime = mgr->runtime();
        inst->lifetime = std::make_shared<pluginxx::InstanceLifetime>(
            ex,
            inst->name,
            1,
            [runtime](std::function<void()> fn) {
                if (auto state = runtime.lock()) {
                    return pluginxx::enqueueRuntimeAction(state, std::move(fn), true);
                }
                return false;
            }
        );
        inst->lifetime->setState(pluginxx::PluginInstanceState::Ready);
        mgr->plugins_.emplace(inst->name, inst);

        // 记下插件线程的线程 id (后面每个用例都要拿它做对比)
        std::atomic<bool> probed{false};
        pluginxx::dispatchToPlugin(inst, "probe plugin thread", [&] {
            pluginThreadId.store(std::this_thread::get_id());
            probed.store(true);
        });
        if (!co_await waitFor([&] {
                return probed.load();
            })) {
            co_return false;
        }
        co_return true;
    }
};

// =====================================================================
// 1. 派发落到插件线程 + FIFO + isPluginThread
// =====================================================================
asio::awaitable<void> test_dispatch_thread_and_order() {
    Fixture f;
    if (!co_await f.start()) {
        XX_TEST_EXPECT_TRUE(false);
        co_return;
    }

    const auto                   ioThreadId = std::this_thread::get_id();
    std::mutex                   mutex;
    std::vector<int>             order;
    std::vector<std::thread::id> threads;
    std::atomic<bool>            finished{false};

    constexpr int kCount = 200;
    for (int i = 0; i < kCount; ++i) {
        const bool queued = pluginxx::dispatchToPlugin(f.inst, "test order", [&, i] {
            if (i == 0) {
                f.pluginThreadId.store(std::this_thread::get_id());
            }
            std::lock_guard lock(mutex);
            order.push_back(i);
            threads.push_back(std::this_thread::get_id());
            if (order.size() == kCount) {
                finished.store(true);
            }
        });
        XX_TEST_EXPECT_TRUE(queued);
    }
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return finished.load();
    }));
    XX_TEST_EXPECT_EQ(static_cast<int>(order.size()), kCount);
    for (int i = 0; i < static_cast<int>(order.size()); ++i) {
        XX_TEST_EXPECT_EQ(order[static_cast<size_t>(i)], i);
    }
    const auto pluginThread = f.pluginThreadId.load();
    XX_TEST_EXPECT_TRUE(pluginThread != std::thread::id{});
    XX_TEST_EXPECT_TRUE(pluginThread != ioThreadId);
    for (const auto& tid : threads) {
        XX_TEST_EXPECT_TRUE(tid == pluginThread);
    }

    // 统计: 派发次数 (夹具启动时已派发过一次探针)
    // - 计数在回调返回后才累加, 而 `finished` 是在回调内部置位的, 所以等计数到齐再断言
    const auto& stats = f.mgr->pluginCallbackStats();
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return stats.callbacks.load() >= static_cast<uint64_t>(kCount) + 1;
    }));

    // isPluginThread: io 线程上为 false, 插件线程上为 true
    XX_TEST_EXPECT_FALSE(f.mgr->isPluginThread());
    std::atomic<int> seen{-1};
    pluginxx::dispatchToPlugin(f.inst, "test is plugin thread", [&] {
        seen.store(f.mgr->isPluginThread() ? 1 : 0);
    });
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return seen.load() >= 0;
    }));
    XX_TEST_EXPECT_EQ(seen.load(), 1);
}

// =====================================================================
// 2. lease 在派发时取 + 关闭后丢弃
// =====================================================================
asio::awaitable<void> test_lease_and_drop_after_close() {
    Fixture f;
    if (!co_await f.start()) {
        XX_TEST_EXPECT_TRUE(false);
        co_return;
    }

    Latch             entered;
    Latch             release;
    std::atomic<bool> done{false};
    XX_TEST_EXPECT_TRUE(pluginxx::dispatchToPlugin(f.inst, "test blocker", [&] {
        entered.signal();
        release.wait(5000);
        done.store(true);
    }));
    // 排队中就算在途: 关闭要等它跑完
    XX_TEST_EXPECT_TRUE(entered.wait(5000));
    XX_TEST_EXPECT_EQ(f.inst->lifetime->leaseCount(), size_t{1});

    // 占住插件线程时, 后续派发仍按顺序排队 (lease 立刻 +1)
    std::atomic<bool> second{false};
    XX_TEST_EXPECT_TRUE(pluginxx::dispatchToPlugin(f.inst, "test second", [&] {
        second.store(true);
    }));
    XX_TEST_EXPECT_EQ(f.inst->lifetime->leaseCount(), size_t{2});
    XX_TEST_EXPECT_FALSE(second.load());

    // 关闭: 常规回调不再受理
    f.inst->lifetime->requestClose();
    const auto droppedBefore = f.mgr->pluginCallbackStats().dropped.load();
    XX_TEST_EXPECT_FALSE(pluginxx::dispatchToPlugin(f.inst, "test after close", [] {}));
    XX_TEST_EXPECT_EQ(f.mgr->pluginCallbackStats().dropped.load(), droppedBefore + 1);

    release.signal();
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return done.load() && second.load();
    }));
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return f.inst->lifetime->leaseCount() == 0;
    }));
}

// =====================================================================
// 3. 预算统计
// =====================================================================
asio::awaitable<void> test_callback_budget() {
    Fixture f;
    if (!co_await f.start()) {
        XX_TEST_EXPECT_TRUE(false);
        co_return;
    }
    f.mgr->setPluginCallbackBudget(1); // 1 ms
    std::atomic<bool> done{false};
    pluginxx::dispatchToPlugin(f.inst, "test slow", [&] {
        std::this_thread::sleep_for(8ms);
        done.store(true);
    });
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return done.load();
    }));
    const auto& stats = f.mgr->pluginCallbackStats();
    // 统计在回调返回之后才更新: 等它结算完再看计数
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return stats.callbacks.load() >= 1 && stats.overBudget.load() >= 1;
    }));
    XX_TEST_EXPECT_EQ(stats.overBudget.load(), uint64_t{1});
    XX_TEST_EXPECT_TRUE(stats.maxMicros.load() >= 1000);
    f.mgr->setPluginCallbackBudget(100);
}

// =====================================================================
// 4. Operation: start / 取消 / 完成回调 都在插件线程
// =====================================================================
asio::awaitable<void> test_operation_on_plugin_thread() {
    Fixture f;
    if (!co_await f.start()) {
        XX_TEST_EXPECT_TRUE(false);
        co_return;
    }
    const auto ioThreadId = std::this_thread::get_id();

    std::atomic<std::thread::id> startThread{};
    std::atomic<std::thread::id> cancelThread{};
    std::atomic<std::thread::id> doneCallbackThread{};
    std::atomic<int32_t>         callbackStatus{9999};
    std::atomic<bool>            callbackDone{false};

    {
        auto core
            = pluginxx::OpCore::create(f.mgr->runtime(), f.inst, nullptr, "executor probe");
        pluginxx::OpDrive drive;
        drive.start = [&](const PluginxxOperatorNotify*, PluginxxString*) -> void* {
            startThread.store(std::this_thread::get_id());
            return reinterpret_cast<void*>(0x1); // 受理但不同步完成
        };
        drive.cancel = [&](void*) {
            cancelThread.store(std::this_thread::get_id());
        };
        std::string error;
        XX_TEST_EXPECT_TRUE(co_await core->startAsync(std::move(drive), error));
        XX_TEST_EXPECT_TRUE(startThread.load() == f.pluginThreadId.load());
        XX_TEST_EXPECT_TRUE(startThread.load() != ioThreadId);

        core->cancel();
        XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
            return cancelThread.load() != std::thread::id{};
        }));
        XX_TEST_EXPECT_TRUE(cancelThread.load() == f.pluginThreadId.load());

        auto notify = core->notify();
        notify.done(notify.host_ud, PLUGINXX_OPERATOR_CANCELED, nullptr);
        XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
            return core->completed();
        }));
    }

    {
        struct Probe {
            std::atomic<std::thread::id>* thread = nullptr;
            std::atomic<int32_t>*         status = nullptr;
            std::atomic<bool>*            done   = nullptr;
        } probe{&doneCallbackThread, &callbackStatus, &callbackDone};

        auto core = pluginxx::OpCore::create(f.mgr->runtime(), f.inst, nullptr, "executor done");
        core->setCallback(
            [](void* ud, int32_t status, const PluginxxStringView*) {
                auto* self = static_cast<Probe*>(ud);
                self->thread->store(std::this_thread::get_id());
                self->status->store(status);
                self->done->store(true);
            },
            &probe
        );
        pluginxx::OpDrive drive;
        drive.start = [](const PluginxxOperatorNotify* notify, PluginxxString*) -> void* {
            auto copy = *notify;
            copy.done(copy.host_ud, PLUGINXX_OPERATOR_OK, nullptr);
            return reinterpret_cast<void*>(0x2);
        };
        std::string error;
        XX_TEST_EXPECT_TRUE(co_await core->startAsync(std::move(drive), error));
        XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
            return callbackDone.load();
        }));
        XX_TEST_EXPECT_TRUE(doneCallbackThread.load() == f.pluginThreadId.load());
        XX_TEST_EXPECT_EQ(callbackStatus.load(), PLUGINXX_OPERATOR_OK);
    }
}

// =====================================================================
// 5. 生命周期入口 (start/stop/mount/unmount 的公共底座)
// =====================================================================
asio::awaitable<void> test_lifecycle_on_plugin_thread() {
    Fixture f;
    if (!co_await f.start()) {
        XX_TEST_EXPECT_TRUE(false);
        co_return;
    }
    const auto                   ioThreadId = std::this_thread::get_id();
    std::atomic<std::thread::id> lifecycleThread{};

    std::string error;
    const bool  ok = co_await pluginxx::awaitPluginLifecycleVoid(
        f.mgr->runtime(),
        f.inst,
        [&](const PluginxxOperatorNotify* notify, PluginxxString*) {
            lifecycleThread.store(std::this_thread::get_id());
            auto copy = *notify;
            copy.done(copy.host_ud, PLUGINXX_OPERATOR_OK, nullptr);
        },
        "plugin start",
        error
    );
    XX_TEST_EXPECT_TRUE(ok);
    XX_TEST_EXPECT_TRUE(error.empty());
    XX_TEST_EXPECT_TRUE(lifecycleThread.load() == f.pluginThreadId.load());
    XX_TEST_EXPECT_TRUE(lifecycleThread.load() != ioThreadId);
}

// =====================================================================
// 6. 协程驱动票在插件线程推进
// =====================================================================
asio::awaitable<void> test_driver_ticket_on_plugin_thread() {
    Fixture f;
    if (!co_await f.start()) {
        XX_TEST_EXPECT_TRUE(false);
        co_return;
    }
    struct Probe {
        std::atomic<std::thread::id> thread{};
        std::atomic<int>             calls{0};
    } probe;

    auto driver = ::PluginxxDriver::create(
        f.mgr->runtime(),
        f.inst->lifetime,
        [](void* ud) {
            auto* self = static_cast<Probe*>(ud);
            self->thread.store(std::this_thread::get_id());
            self->calls.fetch_add(1);
        },
        &probe,
        "executor driver"
    );
    XX_TEST_EXPECT_TRUE(driver != nullptr);
    XX_TEST_EXPECT_TRUE(driver->schedule());
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return probe.calls.load() == 1;
    }));
    XX_TEST_EXPECT_TRUE(probe.thread.load() == f.pluginThreadId.load());
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return driver->finished();
    }));
}

// =====================================================================
// 7. 广播订阅处理器在插件线程
// =====================================================================
asio::awaitable<void> test_broadcast_handler_on_plugin_thread() {
    Fixture f;
    if (!co_await f.start("cli")) {
        XX_TEST_EXPECT_TRUE(false);
        co_return;
    }
    struct Probe {
        std::atomic<std::thread::id> thread{};
        std::atomic<int>             calls{0};
        std::string                  payload;
    } probe;

    const std::string topic = "plugin.cli.ping";
    f.mgr->ensureTopicBroadcast(topic);
    auto* sub = f.mgr->subscribeBroadcast(
        f.inst.get(),
        nullptr,
        topic,
        [](const PluginxxStringView* payloadJson, void* ud) {
            auto* self = static_cast<Probe*>(ud);
            self->thread.store(std::this_thread::get_id());
            if (payloadJson && payloadJson->data) {
                self->payload.assign(payloadJson->data, static_cast<size_t>(payloadJson->size));
            }
            self->calls.fetch_add(1);
        },
        &probe
    );
    XX_TEST_EXPECT_TRUE(sub != nullptr);

    std::string error;
    const int   rc = f.mgr->publishBroadcastFrom(f.inst.get(), topic, "\"hi\"", &error);
    if (rc != PLUGINXX_RESULT_OK) {
        TEST_INFO << "publish `" << topic << "` failed: " << error << std::endl;
    }
    XX_TEST_EXPECT_EQ(rc, PLUGINXX_RESULT_OK);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return probe.calls.load() == 1;
    }));
    XX_TEST_EXPECT_TRUE(probe.thread.load() == f.pluginThreadId.load());
    XX_TEST_EXPECT_TRUE(probe.payload == "\"hi\"");
}

// =====================================================================
// 8. 执行器停止后派发被丢弃, 不悬挂
// =====================================================================
asio::awaitable<void> test_executor_stopped() {
    Fixture f;
    if (!co_await f.start()) {
        XX_TEST_EXPECT_TRUE(false);
        co_return;
    }
    std::atomic<bool> once{false};
    XX_TEST_EXPECT_TRUE(pluginxx::dispatchToPlugin(f.inst, "before stop", [&] {
        once.store(true);
    }));
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return once.load();
    }));

    f.thread->stop();
    const auto before = f.mgr->pluginCallbackStats().unschedulable.load();
    XX_TEST_EXPECT_FALSE(pluginxx::dispatchToPlugin(f.inst, "after stop", [] {}));
    XX_TEST_EXPECT_EQ(f.mgr->pluginCallbackStats().unschedulable.load(), before + 1);
    // 拿不到执行器时不能留下在途记录
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return f.inst->lifetime->leaseCount() == 0;
    }));
}

// =====================================================================
// 9. 真实插件: JS 引擎 + 脚本工具在插件线程开启时跑通
// =====================================================================
/// `agentxx_javascript_engine` 的 start 事务里要登记一条能力行, 脚本又通过能力行
/// 在 JS 线程里注册工具。配了插件执行器时这条链是“io → 插件线程 → JS 线程”三跳,
/// 而脚本里的表调用会在 JS 线程上同步等完成 (完成回调投回插件线程)。
/// 本组用真实 DSO 验证: 能力行登记成功、脚本工具注册成功、工具能真实执行。
asio::awaitable<void> test_js_engine_on_plugin_thread() {
    const std::string engineDir = findPluginDirForExecutor("agentxx_javascript_engine");
    const std::string jsDir     = findPluginDirForExecutor("example_js");
    if (engineDir.empty() || jsDir.empty()) {
        TEST_INFO << "javascript engine / example_js plugin not found, skip js-on-thread test"
                  << std::endl;
        co_return;
    }
    auto ex = co_await asio::this_coro::executor;

    auto thread = std::make_shared<pluginxx::PluginExecutorThread>();
    if (!thread->start("agentxx-test-plugin")) {
        XX_TEST_EXPECT_TRUE(false);
        co_return;
    }
    auto ctx               = std::make_shared<agentxx::agent::AgentContext>();
    ctx->toolRegistry      = std::make_shared<agentxx::plugin::ToolRegistry>();
    auto mgr               = std::make_shared<agentxx::plugin::PluginManager>(ctx);
    ctx->pluginManager     = mgr;
    mgr->setIoExecutor(ex);
    mgr->setPluginExecutor(thread->executor());
    auto mount       = mgr->attachMountSync(ctx);
    ctx->pluginMount = mount;

    auto engine = co_await mgr->loadPluginAsync(engineDir);
    XX_TEST_EXPECT_TRUE(engine != nullptr);
    auto script = co_await mgr->loadPluginAsync(jsDir);
    XX_TEST_EXPECT_TRUE(script != nullptr);
    XX_TEST_EXPECT_TRUE(mgr->hasPluginExecutor());

    // 脚本在 JS 线程加载、异步注册工具, 等它出现
    XX_TEST_EXPECT_TRUE(co_await waitFor(
        [&] {
            return ctx->toolRegistry->contains("js_hello");
        },
        20000
    ));
    auto tool = ctx->toolRegistry->find("js_hello");
    XX_TEST_EXPECT_TRUE(tool != nullptr);
    if (tool) {
        auto result = co_await tool->execute_async(utilxx_base::Json{{"name", "plugin-thread"}});
        XX_TEST_EXPECT_TRUE(result.text.find("plugin-thread") != std::string::npos);
    }

    // 先卸脚本、再卸引擎 (依赖顺序), 全部走异步 stop
    XX_TEST_EXPECT_TRUE(co_await mgr->unloadAsync("example_js", std::chrono::seconds{30}));
    XX_TEST_EXPECT_TRUE(
        co_await mgr->unloadAsync("agentxx_javascript_engine", std::chrono::seconds{30})
    );
    co_await mgr->shutdownAsync();
    ctx->pluginMount.reset();
    tool.reset();
    script.reset();
    engine.reset();
    mgr.reset();
    thread->stop();
    co_return;
}


} // namespace

asio::awaitable<TestResult> run_plugin_executor_tests() {
    TestResult result;

    co_await test_dispatch_thread_and_order();
    co_await test_lease_and_drop_after_close();
    co_await test_callback_budget();
    co_await test_operation_on_plugin_thread();
    co_await test_lifecycle_on_plugin_thread();
    co_await test_driver_ticket_on_plugin_thread();
    co_await test_broadcast_handler_on_plugin_thread();
    co_await test_executor_stopped();
    co_await test_real_host_js_engine_start();
    co_await test_js_engine_on_plugin_thread();

    result.passed = g_pe_passed;
    result.failed = g_pe_failed;
    co_return result;
}

} // namespace test
} // namespace agentxx
