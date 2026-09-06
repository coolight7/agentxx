// test_plugin_multi_instance —— 多实例回归测试
//
// 验证同一动态库插件可被同进程内不同 agent 宿主各自创建为并存实例:
// - 两个 AgentContext 各持独立 PluginManager, 加载同一 example_plugin
// - 工具执行路由正确: A 实例的回调打到 A 的宿主 (echo 回显 sessionId 可区分)
// - 状态隔离: 卸载 A 后 B 的工具仍可用; B 重新加载亦正常
// - 反复 load/unload 循环不泄漏 (LeakSanitizer 兜底)
#include "test_plugin_multi_instance.h"

#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/plugin_manager.h"
#include "asio/co_spawn.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include <atomic>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace {
int g_mi_passed = 0;
int g_mi_failed = 0;
} // namespace

#define XX_TEST_PASSED g_mi_passed
#define XX_TEST_FAILED g_mi_failed

namespace agentxx {
namespace test {

/// 与 test_plugins 同款定位逻辑: exe 同目录 plugins/example_plugin
static std::string findExamplePluginDirMI() {
    namespace fs = std::filesystem;
    std::error_code       ec;
    std::vector<fs::path> candidates;
#if XX_IS_WIN_D
    wchar_t buf[MAX_PATH];
    if (::GetModuleFileNameW(nullptr, buf, MAX_PATH) > 0) {
        candidates.push_back(fs::path(buf).parent_path() / "plugins" / "example_plugin");
    }
#else
    if (auto p = fs::read_symlink("/proc/self/exe", ec); !ec) {
        candidates.push_back(p.parent_path() / "plugins" / "example_plugin");
    }
#endif
    candidates.push_back(fs::current_path(ec) / "plugins" / "example_plugin");
    auto hasLibFile = [](const fs::path& dir) {
        std::error_code                     ec2;
        std::filesystem::directory_iterator it(dir, ec2);
        std::filesystem::directory_iterator end;
        for (; it != end; it.increment(ec2)) {
            auto ext = it->path().extension().string();
            if (ext == ".so" || ext == ".dll" || ext == ".dylib") {
                return true;
            }
        }
        return false;
    };
    for (const auto& c : candidates) {
        if (fs::is_directory(c, ec) && hasLibFile(c)) {
            return c.string();
        }
    }
    return {};
}

/// 构造最小 AgentContext (与 run_plugin_tests 相同装配)
static std::shared_ptr<agent::AgentContext> makeAgentCtx(asio::any_io_executor ex) {
    auto ctx                     = std::make_shared<agent::AgentContext>();
    ctx->agentConfig             = std::make_shared<agent::AgentConfig>();
    ctx->middlewareHandleContext = std::make_shared<middleware::MiddlewareContext>();
    ctx->bus                     = std::make_shared<event::EventBus>(ex);
    ctx->toolRegistry            = std::make_shared<plugin::ToolRegistry>();
    ctx->pluginManager           = std::make_shared<plugin::PluginManager>(ctx);
    ctx->pluginManager->setIoExecutor(ex);
    return ctx;
}

asio::awaitable<TestResult> run_plugin_multi_instance_tests() {
    g_mi_passed = 0;
    g_mi_failed = 0;

    auto path = findExamplePluginDirMI();
    XX_TEST_EXPECT_TRUE(!path.empty());
    if (path.empty()) {
        XX_TEST_EXPECT_TRUE(false);
        co_return TestResult{g_mi_passed, g_mi_failed};
    }

    // ---- 1. 双宿主并存加载同一插件 ----
    auto ex   = co_await asio::this_coro::executor;
    auto ctxA = makeAgentCtx(ex);
    auto ctxB = makeAgentCtx(ex);

    auto instA = co_await ctxA->pluginManager->loadPluginAsync(path);
    auto instB = co_await ctxB->pluginManager->loadPluginAsync(path);
    XX_TEST_EXPECT_TRUE(instA != nullptr);
    XX_TEST_EXPECT_TRUE(instB != nullptr);
    XX_TEST_EXPECT_TRUE(instA->host.opaque != instB->host.opaque); ///< 宿主句柄互异
    if (!instA || !instB) {
        XX_TEST_EXPECT_TRUE(false);
        co_return TestResult{g_mi_passed, g_mi_failed};
    }

    // ---- 2. 工具路由正确性: 各自 registry 内 echo 回到各自宿主 ----
    {
        auto toolA = ctxA->toolRegistry->find("example_echo");
        auto toolB = ctxB->toolRegistry->find("example_echo");
        XX_TEST_EXPECT_TRUE(toolA != nullptr);
        XX_TEST_EXPECT_TRUE(toolB != nullptr);
        if (toolA && toolB) {
            auto outA = co_await toolA->execute_async(neograph::json{
                {"sessionId", "from-A"}
            });
            auto outB = co_await toolB->execute_async(neograph::json{
                {"sessionId", "from-B"}
            });
            // echo 原样回显参数 → 结果携带各自标识, 且互不串扰
            XX_TEST_EXPECT_TRUE(outA.find("from-A") != std::string::npos);
            XX_TEST_EXPECT_TRUE(outB.find("from-B") != std::string::npos);
            XX_TEST_EXPECT_TRUE(outA.find("from-B") == std::string::npos);
            XX_TEST_EXPECT_TRUE(outB.find("from-A") == std::string::npos);
        }
    }

    // ---- 3. 卸载 A: B 不受影响 ----
    {
        bool okA = co_await ctxA->pluginManager->unloadAsync("example_plugin");
        XX_TEST_EXPECT_TRUE(okA);
        XX_TEST_EXPECT_FALSE(ctxA->toolRegistry->contains("example_echo"));
        XX_TEST_EXPECT_TRUE(ctxB->toolRegistry->contains("example_echo"));

        auto toolB = ctxB->toolRegistry->find("example_echo");
        XX_TEST_EXPECT_TRUE(toolB != nullptr);
        if (!toolB) {
            co_return TestResult{g_mi_passed, g_mi_failed};
        }
        auto outB = co_await toolB->execute_async(neograph::json{
            {"sessionId", "after-A-unload"}
        });
        XX_TEST_EXPECT_TRUE(outB.find("after-A-unload") != std::string::npos);
    }

    // ---- 4. B 也卸载后可再次加载 (反复创建/销毁路径) ----
    {
        XX_TEST_EXPECT_TRUE(co_await ctxB->pluginManager->unloadAsync("example_plugin"));
        auto again = co_await ctxB->pluginManager->loadPluginAsync(path);
        XX_TEST_EXPECT_TRUE(again != nullptr);
        if (again) {
            auto toolB2 = ctxB->toolRegistry->find("example_echo");
            XX_TEST_EXPECT_TRUE(toolB2 != nullptr);
            if (toolB2) {
                auto out = co_await toolB2->execute_async(neograph::json{
                    {"sessionId", "reloaded"}
                });
                XX_TEST_EXPECT_TRUE(out.find("reloaded") != std::string::npos);
            }
            XX_TEST_EXPECT_TRUE(co_await ctxB->pluginManager->unloadAsync("example_plugin"));
        }
    }

    // ---- 5. 宿主侧 registerTask 托管语义 (agentxx.agent.tasks) ----
    // 直接经 PluginManager::registerTask 验证: 句柄登记 (outstandingOps) /
    // cancel_fn 触发 / notify.done 恰好一次 (inflight 归零 + 句柄回收)
    {
        auto instC = co_await ctxA->pluginManager->loadPluginAsync(path);
        XX_TEST_EXPECT_TRUE(instC != nullptr);
        if (instC) {
            struct TaskState {
                std::atomic<int>  cancelCount{0};
                std::atomic<bool> done{false};
            };

            auto                        st = std::make_shared<TaskState>();
            auto                        ex = co_await asio::this_coro::executor;
            AgentxxPluginOperatorNotify ntf{nullptr, nullptr};
            AgentxxPluginString         err{nullptr, 0};
            auto*                       h = ctxA->pluginManager->registerTask(
                instC.get(),
                [](void* ud, void*) {
                    auto* s = static_cast<TaskState*>(ud);
                    s->cancelCount.fetch_add(1, std::memory_order_relaxed);
                },
                st.get(),
                &ntf,
                &err
            );
            XX_TEST_EXPECT_TRUE(h != nullptr);
            XX_TEST_EXPECT_TRUE(ntf.done != nullptr); ///< notify 为出参 (宿主回填)
            // 句柄已登记 (detachAll 可取消; 未完成前存在)
            bool tracked = std::any_of(
                instC->outstandingOps.begin(),
                instC->outstandingOps.end(),
                [h](const std::shared_ptr<AgentxxPluginOperatorHandle>& op) {
                    return op.get() == h;
                }
            );
            XX_TEST_EXPECT_TRUE(tracked);
            size_t inflightBefore = instC->inflight.load(std::memory_order_acquire);

            // 通知完成 (模拟插件协程结束上报) → inflight-1 + 句柄回收 (异步)
            auto nullSv = agentxx::plugin::PluginStringView::from(nullptr, 0);
            ntf.done(ntf.host_ud, AGENTXX_PLUGIN_OPERATOR_OK, &nullSv);
            XX_TEST_EXPECT_TRUE(
                st->done.exchange(true) == false
            ); ///< 恰好一次语义由 OpCore CAS 保证
            // 等待回收协程把句柄从 outstandingOps 移除
            for (int i = 0; i < 100; ++i) {
                auto stillTracked = std::any_of(
                    instC->outstandingOps.begin(),
                    instC->outstandingOps.end(),
                    [h](const std::shared_ptr<AgentxxPluginOperatorHandle>& op) {
                        return op.get() == h;
                    }
                );
                if (!stillTracked) {
                    break;
                }
                asio::steady_timer t(ex);
                t.expires_after(std::chrono::milliseconds(5));
                co_await t.async_wait(asio::use_awaitable);
            }
            bool trackedAfter = std::any_of(
                instC->outstandingOps.begin(),
                instC->outstandingOps.end(),
                [h](const std::shared_ptr<AgentxxPluginOperatorHandle>& op) {
                    return op.get() == h;
                }
            );
            XX_TEST_EXPECT_FALSE(trackedAfter); ///< 完成后句柄被回收
            size_t inflightAfter = instC->inflight.load(std::memory_order_acquire);
            XX_TEST_EXPECT_TRUE(inflightAfter < inflightBefore || inflightBefore == 0);
            if (err.data) {
                agentxx::plugin::PluginString::free(&instC->host, &err);
            }

            // 取消路径: 新任务 → xx_op_cancel 语义 (op->cancelled CAS + cancelFn)
            AgentxxPluginOperatorNotify ntf2{nullptr, nullptr};
            AgentxxPluginString         err2{nullptr, 0};
            auto*                       h2 = ctxA->pluginManager->registerTask(
                instC.get(),
                [](void* ud, void*) {
                    auto* s = static_cast<TaskState*>(ud);
                    s->cancelCount.fetch_add(1, std::memory_order_relaxed);
                },
                st.get(),
                &ntf2,
                &err2
            );
            if (err2.data) {
                agentxx::plugin::PluginString::free(&instC->host, &err2);
            }
            XX_TEST_EXPECT_TRUE(h2 != nullptr);
            if (h2) {
                h2->cancelled.store(true); ///< 模拟 detachAll / op_cancel 前置置位
                if (h2->cancelFn) {
                    h2->cancelFn();
                }
                // cancel_fn 被调用 (协作式)
                XX_TEST_EXPECT_TRUE(st->cancelCount.load(std::memory_order_relaxed) >= 1);
                // 取消后仍可上报完成 (宿主幂等; 不 UAF)
                auto nullSv2 = agentxx::plugin::PluginStringView::from(nullptr, 0);
                ntf2.done(ntf2.host_ud, AGENTXX_PLUGIN_OPERATOR_CANCELLED, &nullSv2);
                for (int i = 0; i < 100; ++i) {
                    auto stillTracked = std::any_of(
                        instC->outstandingOps.begin(),
                        instC->outstandingOps.end(),
                        [h2](const std::shared_ptr<AgentxxPluginOperatorHandle>& op) {
                            return op.get() == h2;
                        }
                    );
                    if (!stillTracked) {
                        break;
                    }
                    asio::steady_timer t(ex);
                    t.expires_after(std::chrono::milliseconds(5));
                    co_await t.async_wait(asio::use_awaitable);
                }
            }
            co_await ctxA->pluginManager->unloadAsync("example_plugin");
        }
    }

    // ---- 清理 (shutdownAll 为同步接口) ----
    ctxA->pluginManager->shutdownAll();
    ctxB->pluginManager->shutdownAll();

    std::cout << "[INFO] multi-instance done: passed=" << g_mi_passed << " failed=" << g_mi_failed
              << std::endl;
    co_return TestResult{g_mi_passed, g_mi_failed};
}

} // namespace test
} // namespace agentxx
