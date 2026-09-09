#include "test_plugin_runtime.h"

#include "agentxx/agent/context.h"
#include "agentxx/plugin/op_driver.h"
#include "asio/co_spawn.hpp"
#include "asio/io_context.hpp"
#include "asio/use_future.hpp"

#include <barrier>
#include <future>
#include <thread>

#define XX_TEST_PASSED result.passed
#define XX_TEST_FAILED result.failed

namespace agentxx::test {
namespace {
using namespace agentxx::plugin;
using namespace std::chrono_literals;

/// 每个用例自行推进 IO；worker 用 promise/barrier 控制，无定时轮询。
struct RuntimeFixture {
    asio::io_context io;
    std::shared_ptr<PluginManager> manager = std::make_shared<PluginManager>(
        std::weak_ptr<agentxx::agent::AgentContext>{});
    std::shared_ptr<PluginInstance> provider;
    std::shared_ptr<PluginInstance> caller;

    explicit RuntimeFixture(std::shared_ptr<agentxx::agent::AgentContext> ctx = {}) {
        if (ctx) {
            manager = std::make_shared<PluginManager>(ctx);
        }
        manager->setIoExecutor(io.get_executor());
        provider = instance("provider", 1);
        caller = instance("caller", 2);
    }
    std::shared_ptr<PluginInstance> instance(std::string name, uint64_t generation) {
        auto inst = std::make_shared<PluginInstance>(std::move(name));
        inst->self = inst;
        inst->manager = manager;
        inst->host.opaque = inst.get();
        inst->lifetime = std::make_shared<InstanceLifetime>(io.get_executor(), inst->name, generation);
        inst->lifetime->setState(PluginInstanceState::Ready);
        manager->plugins_.emplace(inst->name, inst);
        return inst;
    }
    std::shared_ptr<OpCore> operation() {
        return OpCore::create(manager->runtime(), provider, caller, "runtime regression");
    }
    void drain() { io.restart(); io.poll(); }
};

struct CallbackState {
    int calls = 0;
    int status = -1;
    bool protectedDuringCallback = false;
    bool onIo = false;
    std::string payload;
    RuntimeFixture* fixture = nullptr;

    static void AGENTXX_PLUGIN_CALL done(void* ud, int32_t status, const AgentxxPluginStringView* payload) {
        auto& state = *static_cast<CallbackState*>(ud);
        ++state.calls;
        state.status = status;
        state.payload = svToStr(payload);
        if (state.fixture) {
            state.onIo = state.fixture->manager->isIoThread();
            state.protectedDuringCallback = state.fixture->provider->lifetime->leaseCount() != 0
                && state.fixture->caller->lifetime->leaseCount() != 0;
        }
    }
};

AgentxxPluginToolSpec fakeTool(void* ud, bool reject) {
    AgentxxPluginToolSpec spec{};
    spec.name = strToSv("runtime_tool");
    spec.parameters_json = strToSv("{}");
    spec.user_data = ud;
    if (reject) {
        spec.execute_start = +[](void*, const AgentxxPluginStringView*, const AgentxxPluginStringView*, const AgentxxPluginStringView*, const AgentxxPluginOperatorNotify*, AgentxxPluginString* error) -> void* {
            hostMemorySetString(error, "rejected by fake provider");
            return nullptr;
        };
    } else {
        spec.execute_start = +[](void* ud, const AgentxxPluginStringView* args, const AgentxxPluginStringView*, const AgentxxPluginStringView*,
                                 const AgentxxPluginOperatorNotify* notify, AgentxxPluginString*) -> void* {
            *static_cast<bool*>(ud) = true;
            notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, args);
            return nullptr;
        };
    }
    return spec;
}
} // namespace

TestResult testPluginRuntime() {
    TestResult result;

    /// F01：拒绝不回调，双方登记和 lease 完整回滚；同步 done 仍为接受。
    {
        RuntimeFixture f;
        CallbackState cb{.fixture = &f};
        auto spec = fakeTool(nullptr, true);
        XX_TEST_EXPECT_EQ(f.manager->registerTool(f.provider.get(), &spec), 0);
        AgentxxPluginString error{};
        auto* rejected = f.manager->callToolAsync(f.caller.get(), "runtime_tool", "{}", "s",
                                                 CallbackState::done, &cb, &error);
        XX_TEST_EXPECT_TRUE(rejected == nullptr);
        XX_TEST_EXPECT_TRUE(error.data != nullptr);
        hostMemoryFree(error.data);
        f.drain();
        XX_TEST_EXPECT_EQ(cb.calls, 0);
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
        XX_TEST_EXPECT_TRUE(f.provider->outstandingOps.empty());
        XX_TEST_EXPECT_TRUE(f.caller->outstandingOps.empty());
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
        XX_TEST_EXPECT_EQ(f.caller->lifetime->leaseCount(), size_t{0});
        f.manager->unregisterTool(f.provider.get(), "runtime_tool");
        bool started = false;
        spec = fakeTool(&started, false);
        XX_TEST_EXPECT_EQ(f.manager->registerTool(f.provider.get(), &spec), 0);
        error = {};
        auto* accepted = f.manager->callToolAsync(f.caller.get(), "runtime_tool", R"({"hello":"world"})", "s",
                                                 CallbackState::done, &cb, &error);
        XX_TEST_EXPECT_TRUE(accepted != nullptr);
        XX_TEST_EXPECT_TRUE(started);
        XX_TEST_EXPECT_TRUE(error.data == nullptr);
        XX_TEST_EXPECT_EQ(cb.calls, 0);
        f.drain();
        XX_TEST_EXPECT_EQ(cb.calls, 1);
        XX_TEST_EXPECT_TRUE(cb.protectedDuringCallback && cb.onIo);
        XX_TEST_EXPECT_TRUE(cb.payload.find("world") != std::string::npos);
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
        /// callback 可选，不允许因未提供 callback 将已接受请求改为拒绝。
        accepted = f.manager->callToolAsync(f.caller.get(), "runtime_tool", "{}", "s", nullptr, nullptr, &error);
        XX_TEST_EXPECT_TRUE(accepted != nullptr);
        f.drain();
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
    }

    /// P0-A/F07/F14：任意线程复制 payload，提交后取消无效；caller Closing
    /// 仍保护本次回调直到返回，不依赖 provider 启用位延长代码生命周期。
    {
        RuntimeFixture f;
        CallbackState cb{.fixture = &f};
        auto op = f.operation();
        int cancels = 0;
        op->accept([&] { ++cancels; });
        op->setCallback(CallbackState::done, &cb);
        const auto notify = op->notify();
        const std::string expected(65536, 'x');
        std::thread worker([notify, expected] {
            auto text = expected;
            auto view = strToSv(text);
            notify.done(notify.host_ud, AGENTXX_PLUGIN_OPERATOR_OK, &view);
            text.assign(text.size(), 'z');
        });
        worker.join();
        XX_TEST_EXPECT_TRUE(op->submitted());
        XX_TEST_EXPECT_FALSE(op->completed());
        XX_TEST_EXPECT_EQ(cb.calls, 0);
        f.caller->lifetime->requestClose();
        f.provider->lifetime->requestClose();
        op->cancel();
        XX_TEST_EXPECT_EQ(cancels, 0);
        notify.done(notify.host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, nullptr);
        f.drain();
        XX_TEST_EXPECT_TRUE(op->completed());
        XX_TEST_EXPECT_EQ(cb.calls, 1);
        XX_TEST_EXPECT_EQ(cb.status, AGENTXX_PLUGIN_OPERATOR_OK);
        XX_TEST_EXPECT_EQ(cb.payload, expected);
        XX_TEST_EXPECT_TRUE(cb.protectedDuringCallback);
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
        XX_TEST_EXPECT_EQ(f.caller->lifetime->leaseCount(), size_t{0});
        cancelPluginOperation(op->handle());
        f.drain();
        XX_TEST_EXPECT_EQ(cancels, 0);
    }

    /// 取消中同步 done 可重入，终态清理不能遗漏；异常 callback 不阻止内部清理。
    {
        RuntimeFixture f;
        auto op = f.operation();
        auto notify = op->notify();
        int cancels = 0, cleanup = 0;
        op->accept([&] {
            ++cancels;
            notify.done(notify.host_ud, AGENTXX_PLUGIN_OPERATOR_CANCELLED, nullptr);
        });
        op->setCallback(+[](void*, int32_t, const AgentxxPluginStringView*) {
            throw std::runtime_error("fake callback failure");
        }, nullptr);
        op->setCompletionHandler([&](int32_t, std::string_view) { ++cleanup; });
        op->cancel();
        op->cancel();
        f.drain();
        XX_TEST_EXPECT_EQ(cancels, 1);
        XX_TEST_EXPECT_EQ(cleanup, 1);
        XX_TEST_EXPECT_TRUE(op->completed());
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
    }

    /// 关闭 deadline 与一次性 idle 事件；超时保留操作，后续释放可重试并广播。
    {
        RuntimeFixture f;
        auto op = f.operation();
        op->accept();
        f.provider->lifetime->requestClose();
        XX_TEST_EXPECT_FALSE(static_cast<bool>(InstanceLease::acquire(f.provider->lifetime)));
        auto expired = asio::co_spawn(f.io, f.provider->lifetime->waitIdleUntil(std::chrono::steady_clock::now()), asio::use_future);
        f.drain();
        XX_TEST_EXPECT_FALSE(expired.get());
        f.provider->lifetime->setState(PluginInstanceState::CloseFailed);
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{1});
        auto first = asio::co_spawn(f.io, f.provider->lifetime->waitIdleUntil(std::chrono::steady_clock::now() + 5s), asio::use_future);
        auto second = asio::co_spawn(f.io, f.provider->lifetime->waitIdleUntil(std::chrono::steady_clock::now() + 5s), asio::use_future);
        f.drain();
        XX_TEST_EXPECT_TRUE(first.wait_for(0s) != std::future_status::ready);
        auto notify = op->notify();
        notify.done(notify.host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
        f.drain();
        XX_TEST_EXPECT_TRUE(first.get());
        XX_TEST_EXPECT_TRUE(second.get());
        f.provider->lifetime->setState(PluginInstanceState::Closed);
        f.manager->plugins_.erase(f.provider->name);
    }

    /// F06：worker 发起互调，start/登记/完成完整地在 IO 线程执行。
    {
        RuntimeFixture f;
        CallbackState cb{.fixture = &f};
        bool started = false;
        auto spec = fakeTool(&started, false);
        XX_TEST_EXPECT_EQ(f.manager->registerTool(f.provider.get(), &spec), 0);
        auto keepIo = asio::make_work_guard(f.io);
        std::promise<bool> returned;
        std::thread worker([&] {
            AgentxxPluginString error{};
            auto* handle = f.manager->callToolAsync(f.caller.get(), "runtime_tool", "{}", "worker",
                                                   CallbackState::done, &cb, &error);
            returned.set_value(handle != nullptr && error.data == nullptr);
            hostMemoryFree(error.data);
            keepIo.reset();
        });
        f.io.run();
        worker.join();
        XX_TEST_EXPECT_TRUE(returned.get_future().get());
        XX_TEST_EXPECT_TRUE(started && cb.onIo);
        XX_TEST_EXPECT_EQ(cb.calls, 1);
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
    }

    /// 1000 个跨线程完成：每个包的独立输入、状态和登记最终全部回收。
    {
        RuntimeFixture f;
        std::vector<std::shared_ptr<OpCore>> ops;
        std::vector<CallbackState> callbacks(1000);
        for (auto& cb : callbacks) {
            auto op = f.operation();
            op->accept();
            op->setCallback(CallbackState::done, &cb);
            ops.push_back(std::move(op));
        }
        std::barrier ready(5);
        std::vector<std::thread> workers;
        for (size_t worker = 0; worker < 4; ++worker) {
            workers.emplace_back([&, worker] {
                ready.arrive_and_wait();
                for (size_t i = worker; i < ops.size(); i += 4) {
                    auto text = std::to_string(i) + std::string(128, 'p');
                    auto view = strToSv(text);
                    auto notify = ops[i]->notify();
                    notify.done(notify.host_ud, AGENTXX_PLUGIN_OPERATOR_OK, &view);
                }
            });
        }
        ready.arrive_and_wait();
        for (auto& worker : workers) { worker.join(); }
        f.drain();
        bool correct = true;
        for (size_t i = 0; i < callbacks.size(); ++i) {
            correct = correct && callbacks[i].calls == 1 && callbacks[i].status == AGENTXX_PLUGIN_OPERATOR_OK
                && callbacks[i].payload == std::to_string(i) + std::string(128, 'p');
        }
        XX_TEST_EXPECT_TRUE(correct);
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
        XX_TEST_EXPECT_TRUE(f.provider->outstandingOps.empty() && f.caller->outstandingOps.empty());
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
    }

    /// worker 完成与 IO 取消并发。cancel 只请求取消，worker 仍唯一地提交终态；
    /// IO 在 worker 继续运行时处理完成包，覆盖并发投递而非仅 join 后批量读取。
    {
        RuntimeFixture f;
        constexpr size_t count = 128;
        std::vector<std::shared_ptr<OpCore>> ops;
        std::vector<CallbackState> callbacks(count);
        std::vector<int> cancels(count, 0);
        for (size_t i = 0; i < count; ++i) {
            auto op = f.operation();
            op->accept([&, i] { ++cancels[i]; });
            op->setCallback(CallbackState::done, &callbacks[i]);
            ops.push_back(std::move(op));
        }
        auto keepIo = asio::make_work_guard(f.io);
        std::barrier ready(2);
        std::thread worker([&] {
            ready.arrive_and_wait();
            for (const auto& op : ops) {
                auto notify = op->notify();
                notify.done(notify.host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
            }
            keepIo.reset();
        });
        ready.arrive_and_wait();
        for (const auto& op : ops) { cancelPluginOperation(op->handle()); }
        f.io.run();
        worker.join();
        bool correct = true;
        for (size_t i = 0; i < count; ++i) {
            correct = correct && callbacks[i].calls == 1 && cancels[i] <= 1 && ops[i]->completed();
        }
        XX_TEST_EXPECT_TRUE(correct);
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
    }

    /// 等待者取消与 provider 完成分开：取消等待不能回收尚未完成的执行 lease。
    {
        RuntimeFixture f;
        auto op = f.operation();
        op->accept();
        asio::cancellation_signal cancel;
        auto waited = asio::co_spawn(f.io, op->wait(),
            asio::bind_cancellation_slot(cancel.slot(), asio::use_future));
        f.drain();
        cancel.emit(asio::cancellation_type::all);
        f.drain();
        bool aborted = false;
        try { waited.get(); } catch (const util::AsioSystemError&) { aborted = true; }
        XX_TEST_EXPECT_TRUE(aborted);
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{1});
        XX_TEST_EXPECT_FALSE(op->completed());
        auto notify = op->notify();
        notify.done(notify.host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
        f.drain();
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
    }

    /// F14/F05：后台 task 提交 done 后，尚未 commit 就卸载，不调用失效 cancel_ud。
    {
        RuntimeFixture f;
        AgentxxPluginOperatorNotify notify{};
        AgentxxPluginString error{};
        int cancels = 0;
        auto* handle = f.manager->registerTask(f.provider.get(),
            +[](void* ud, void*) { ++*static_cast<int*>(ud); }, &cancels, &notify, &error);
        XX_TEST_EXPECT_TRUE(handle != nullptr && error.data == nullptr && notify.done != nullptr);
        notify.done(notify.host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
        f.provider->lifetime->requestClose();
        f.manager->detachAll(f.provider.get());
        XX_TEST_EXPECT_EQ(cancels, 0);
        XX_TEST_EXPECT_FALSE(f.provider->outstandingOps.empty());
        f.drain();
        XX_TEST_EXPECT_EQ(cancels, 0);
        XX_TEST_EXPECT_TRUE(f.provider->outstandingOps.empty());
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
    }

    /// F08：sleep 接受即计入 lease；取消和正常完成都回收记录，并且只回调一次。
    {
        RuntimeFixture f;
        int calls = 0;
        auto callback = +[](void* ud) { ++*static_cast<int*>(ud); };
        bool allAccepted = true;
        for (int i = 0; i < 1000; ++i) {
            allAccepted = (f.manager->sleep(f.provider.get(), 0, callback, &calls) != nullptr) && allAccepted;
        }
        XX_TEST_EXPECT_TRUE(allAccepted);
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{1000});
        XX_TEST_EXPECT_EQ(calls, 0);
        f.drain();
        XX_TEST_EXPECT_EQ(calls, 1000);
        XX_TEST_EXPECT_TRUE(f.provider->sleepTimers.empty());
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
        auto* timer = f.manager->sleep(f.provider.get(), 60000, callback, &calls);
        f.manager->cancelSleep(f.provider.get(), timer);
        f.manager->cancelSleep(f.provider.get(), timer);
        f.provider->lifetime->requestClose();
        f.drain();
        XX_TEST_EXPECT_EQ(calls, 1001);
        XX_TEST_EXPECT_TRUE(f.provider->sleepTimers.empty());
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
    }

    /// F15：无裸 manager 的 post；manager 消失不丢失已接受 callback/lease。
    {
        RuntimeFixture f;
        int calls = 0;
        auto runtime = f.manager->runtime();
        f.manager->postCallback(f.provider.get(), +[](void* ud) { ++*static_cast<int*>(ud); }, &calls);
        XX_TEST_EXPECT_EQ(calls, 0);
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{1});
        /// 先移出实例，避免旧同步 shutdown 路径干扰这里独立验证的投递协议。
        f.manager->plugins_.clear();
        f.manager.reset();
        f.provider->lifetime->requestClose();
        f.drain();
        XX_TEST_EXPECT_EQ(calls, 1);
        XX_TEST_EXPECT_TRUE(runtime->operations.empty());
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
    }

    /// 缺少 blocking pool 也必须通过一次异步失败完成，不允许静默挂起。
    {
        RuntimeFixture f;
        struct State { int work = 0; int done = 0; std::string error; } state;
        f.manager->offload(f.provider.get(), nullptr,
            +[](void* ud, volatile int32_t*, AgentxxPluginString*) -> void* {
                ++static_cast<State*>(ud)->work;
                return nullptr;
            },
            +[](void* ud, void*, const AgentxxPluginStringView* error) {
                auto& state = *static_cast<State*>(ud);
                ++state.done;
                state.error = svToStr(error);
            }, &state);
        XX_TEST_EXPECT_EQ(state.done, 0);
        f.drain();
        XX_TEST_EXPECT_EQ(state.work, 0);
        XX_TEST_EXPECT_EQ(state.done, 1);
        XX_TEST_EXPECT_TRUE(state.error.find("no thread pool") != std::string::npos);
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
    }

    /// 真实 worker 由事件释放；Closing 期间不能提前 idle，done 在 IO 调用。
    {
        auto ctx = std::make_shared<agentxx::agent::AgentContext>();
        RuntimeFixture f(ctx);
        struct State {
            std::promise<void> started, release;
            bool done = false, protectedDuringCallback = false, onIo = false;
            RuntimeFixture* fixture;
        } state{.fixture = &f};
        f.manager->offload(f.provider.get(), nullptr,
            +[](void* ud, volatile int32_t*, AgentxxPluginString*) -> void* {
                auto& state = *static_cast<State*>(ud);
                state.started.set_value();
                state.release.get_future().wait();
                return ud;
            },
            +[](void* ud, void* value, const AgentxxPluginStringView*) {
                auto& state = *static_cast<State*>(ud);
                state.done = value == ud;
                state.onIo = state.fixture->manager->isIoThread();
                state.protectedDuringCallback = state.fixture->provider->lifetime->leaseCount() == 1;
            }, &state);
        state.started.get_future().wait();
        f.provider->lifetime->requestClose();
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{1});
        state.release.set_value();
        ctx->threadPool->join();
        f.drain();
        XX_TEST_EXPECT_TRUE(state.done && state.onIo && state.protectedDuringCallback);
        XX_TEST_EXPECT_TRUE(f.manager->runtime()->operations.empty());
        XX_TEST_EXPECT_EQ(f.provider->lifetime->leaseCount(), size_t{0});
    }
    return result;
}
} // namespace agentxx::test
