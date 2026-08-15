#include "test_plugins.h"

#include "agentxx/agent/context.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/util/async_offload.h"
#include "agentxx/util/log.h"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

namespace agentxx {
namespace test {

int g_plugin_passed = 0;
int g_plugin_failed = 0;

/// 定位示例插件库 (与测试可执行同目录, 见 test/CMakeLists.txt)
static std::string findExamplePluginPath() {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    if (!ec) {
        for (const auto& cand : {
                 cwd / "libagentxx_plugin_example.so",
                 cwd / "libagentxx_plugin_example.dylib",
                 cwd / "libagentxx_plugin_example.dll",
             }) {
            if (fs::exists(cand, ec)) {
                return cand.string();
            }
        }
    }
    return "libagentxx_plugin_example.so"; // 让加载失败暴露日志
}

static asio::awaitable<void> sleepMs(int ms) {
    auto timer = asio::steady_timer(co_await asio::this_coro::executor, std::chrono::milliseconds{ms});
    co_await timer.async_wait(asio::use_awaitable);
}

asio::awaitable<TestResult> run_plugin_tests() {
    g_plugin_passed = 0;
    g_plugin_failed = 0;

    // ---- 1. 构造最小 AgentContext (io 线程环境, 与库内无锁模型一致) ----
    auto ctx                     = std::make_shared<agentxx::agent::AgentContext>();
    ctx->agentConfig             = std::make_shared<agentxx::agent::AgentConfig>();
    ctx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
    ctx->bus                     = std::make_shared<agentxx::middleware::EventBus>(co_await asio::this_coro::executor);
    ctx->toolRegistry            = std::make_shared<agentxx::plugin::ToolRegistry>();
    ctx->pluginManager           = std::make_shared<agentxx::plugin::PluginManager>(ctx);

    // ---- 2. 加载示例插件 ----
    auto path = findExamplePluginPath();
    XX_TEST_EXPECT_TRUE(path.find("libagentxx_plugin_example") != std::string::npos);
    auto inst = co_await ctx->pluginManager->loadNativeAsync(path);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        XX_TEST_EXPECT_TRUE(false);
        co_return TestResult{g_plugin_passed, g_plugin_failed};
    }
    XX_TEST_EXPECT_EQ(inst->name, "agentxx_plugin_example");
    XX_TEST_EXPECT_EQ(inst->version, "1.0.0");
    XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("example_echo"));
    XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("example_caller"));
    XX_TEST_EXPECT_TRUE(ctx->pluginManager->capabilities()->has("example.demo"));
    XX_TEST_EXPECT_TRUE(ctx->pluginManager->hasCapability("example.demo") == 1);

    // ---- 3. 工具执行 (同步 C 回调经线程池卸载) ----
    {
        auto tool = ctx->toolRegistry->find("example_echo");
        XX_TEST_EXPECT_TRUE(tool != nullptr);
        if (tool) {
            auto out = co_await tool->execute_async(neograph::json{
                {"thread_id", "t1"},
                {"hello", "world"},
            });
            auto j = neograph::json::parse(out);
            XX_TEST_EXPECT_EQ(j["echo"]["hello"].get<std::string>(), "world");
            XX_TEST_EXPECT_EQ(j["thread_id"].get<std::string>(), "t1");
        }
    }

    // ---- 4. 插件互调 (call_tool) ----
    {
        auto tool = ctx->toolRegistry->find("example_caller");
        XX_TEST_EXPECT_TRUE(tool != nullptr);
        if (tool) {
            auto out = co_await tool->execute_async(neograph::json{
                {"thread_id", "t1"},
                {"x", 42},
            });
            auto j = neograph::json::parse(out);
            XX_TEST_EXPECT_EQ(j["via_call_tool"]["echo"]["x"].get<int>(), 42);
        }
    }

    // ---- 5. 钩子注册: 中间件句柄入栈 + 钩子点记录 ----
    {
        const auto& handles = ctx->middlewareHandleContext->handles;
        bool found = std::any_of(handles.begin(), handles.end(), [](const auto& h) {
            return h->name == "agentxx_plugin_example_middleware";
        });
        XX_TEST_EXPECT_TRUE(found);
        XX_TEST_EXPECT_EQ(inst->hookPoints.size(), size_t{1});
        XX_TEST_EXPECT_TRUE(inst->middleware != nullptr);
    }

    // ---- 6. 事件订阅/发布回环 (plugin.demo.topic) ----
    {
        // 测试侧订阅同一 topic (JSON 字符串载荷)
        bool received = false;
        auto subId    = ctx->bus->get<std::string>("plugin.demo.topic").subscribe(
            [&](const std::string& data) -> asio::awaitable<void> {
                if (data.find("\"k\"") != std::string::npos) {
                    received = true;
                }
                co_return;
            }
        );
        int rc = ctx->pluginManager->publish("demo.topic", R"({"k":"v"})");
        XX_TEST_EXPECT_EQ(rc, 0);
        co_await sleepMs(150);
        XX_TEST_EXPECT_TRUE(received);
        ctx->bus->get<std::string>("plugin.demo.topic").unsubscribe(subId);
    }

    // ---- 7. 禁用 → 工具摘除/钩子停用; 启用 → 恢复 ----
    {
        ctx->pluginManager->disable("agentxx_plugin_example");
        XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("example_echo"));
        XX_TEST_EXPECT_FALSE(ctx->pluginManager->registry()->contains("example_caller"));
        XX_TEST_EXPECT_TRUE(inst->middleware->disabled);
        XX_TEST_EXPECT_FALSE(inst->enabled);

        ctx->pluginManager->enable("agentxx_plugin_example");
        XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("example_echo"));
        XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("example_caller"));
        XX_TEST_EXPECT_FALSE(inst->middleware->disabled);
        XX_TEST_EXPECT_TRUE(inst->enabled);
    }

    // ---- 8. 卸载: 注册残留全部清理 + 插件移除 + 能力移除 ----
    {
        auto ok = co_await ctx->pluginManager->unloadAsync("agentxx_plugin_example");
        XX_TEST_EXPECT_TRUE(ok);
        XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("example_echo"));
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("agentxx_plugin_example") == nullptr);
        XX_TEST_EXPECT_FALSE(ctx->pluginManager->capabilities()->has("example.demo"));
        // 中间件应从 handles 摘除 (pendingCleanup 于下轮 flush; 无轮次时直接摘除)
        const auto& handles = ctx->middlewareHandleContext->handles;
        bool found = std::any_of(handles.begin(), handles.end(), [](const auto& h) {
            return h->name == "agentxx_plugin_example_middleware";
        });
        XX_TEST_EXPECT_FALSE(found);
        // 工具对象应已释放 (inflight 归零后 dlclose)
        XX_TEST_EXPECT_EQ(inst->tools.size(), size_t{0});
    }

    // ---- 9. 重复卸载拒绝 ----
    {
        auto ok = co_await ctx->pluginManager->unloadAsync("agentxx_plugin_example");
        XX_TEST_EXPECT_FALSE(ok);
    }

    // ---- 10. 名称冲突: 静态工具名集合 ----
    {
        // 最小非抽象工具 (仅测试注册表冲突检测用)
        struct DummyTool : agentxx::tools::XXToolBase {
            using XXToolBase::XXToolBase;
            neograph::ChatTool get_definition() const override {
                return neograph::ChatTool{};
            }
            asio::awaitable<std::string> execute_async(const neograph::json&) override {
                co_return std::string{};
            }
        };
        ctx->toolRegistry->setStaticToolNames({"builtin_tool_a"});
        // 插件工具注册冲突在 loadNativeAsync 路径覆盖, 此处验证注册表拒绝同名静态工具
        auto dummy = std::make_shared<DummyTool>("builtin_tool_a", ctx);
        XX_TEST_EXPECT_FALSE(ctx->toolRegistry->registerTool("builtin_tool_a", dummy));
        XX_TEST_EXPECT_TRUE(ctx->toolRegistry->registerTool("plugin_ok_tool", dummy));
        ctx->toolRegistry->unregisterTool("plugin_ok_tool");
    }

    // ---- 11. 插件列表 ----
    {
        // 重新加载用于 list 验证
        auto inst2 = co_await ctx->pluginManager->loadNativeAsync(path);
        XX_TEST_EXPECT_TRUE(inst2 != nullptr);
        if (inst2) {
            auto list = ctx->pluginManager->list();
            bool found = std::any_of(list.begin(), list.end(), [](const auto& item) {
                return item.name == "agentxx_plugin_example" && item.enabled;
            });
            XX_TEST_EXPECT_TRUE(found);
            co_await ctx->pluginManager->unloadAsync("agentxx_plugin_example");
        }
    }

    co_return TestResult{g_plugin_passed, g_plugin_failed};
}

} // namespace test
} // namespace agentxx
