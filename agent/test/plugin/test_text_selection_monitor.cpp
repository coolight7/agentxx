#include "test_text_selection_monitor.h"
#include "agentxx/agent/context.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/tool_registry.h"
#include "asio/use_awaitable.hpp"
#include <filesystem>
#include <iostream>
#include <string>

namespace agentxx {
namespace test {

namespace {

int g_ts_passed = 0;
int g_ts_failed = 0;

#define XX_TEST_PASSED g_ts_passed
#define XX_TEST_FAILED g_ts_failed

} // namespace

asio::awaitable<TestResult>
    run_text_selection_monitor_tests(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    g_ts_passed = 0;
    g_ts_failed = 0;

#if XX_IS_WIN_D
    // ---- 1. 构造 AgentContext ----
    auto ctx                     = std::make_shared<agentxx::agent::AgentContext>();
    ctx->agentConfig             = std::make_shared<agentxx::agent::AgentConfig>();
    ctx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
    ctx->toolRegistry            = std::make_shared<agentxx::plugin::ToolRegistry>();
    ctx->pluginManager           = std::make_shared<agentxx::plugin::PluginManager>(ctx);
    ctx->pluginManager->setIoExecutor(co_await asio::this_coro::executor);

    // ---- 2. 加载 agentxx_text_selection_monitor 插件 ----
    auto path = findTextSelectionPluginPath();
    XX_TEST_EXPECT_TRUE(path.find("agentxx_text_selection_monitor") != std::string::npos);
    auto inst = co_await ctx->pluginManager->loadPluginAsync(path);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return TestResult{g_ts_passed, g_ts_failed};
    }
    XX_TEST_EXPECT_EQ(inst->name, "agentxx_text_selection_monitor");
    XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("agentxx_text_selection_monitor"));

    // ---- 3. 工具 status (未启动: running=false) ----
    {
        auto tool = ctx->toolRegistry->find("agentxx_text_selection_monitor");
        XX_TEST_EXPECT_TRUE(tool != nullptr);
        if (tool) {
            auto out = co_await tool->execute_async(agentxx::util::Json{
                {"command", "status"}
            });
            auto j   = agentxx::util::Json::parse(out);
            XX_TEST_EXPECT_EQ(j["ok"].get<bool>(), true);
            XX_TEST_EXPECT_EQ(j["running"].get<bool>(), false);
        }
    }

    // ---- 4. 工具 start + stop (启动系统级文本选择钩子后立即停止) ----
    {
        auto tool = ctx->toolRegistry->find("agentxx_text_selection_monitor");
        if (tool) {
            auto out = co_await tool->execute_async(agentxx::util::Json{
                {"command",     "start"},
                {"debounce_ms", 300    },
            });
            auto j   = agentxx::util::Json::parse(out);
            XX_TEST_EXPECT_EQ(j["ok"].get<bool>(), true);
            XX_TEST_EXPECT_EQ(j["running"].get<bool>(), true);

            auto out2 = co_await tool->execute_async(agentxx::util::Json{
                {"command", "stop"}
            });
            auto j2   = agentxx::util::Json::parse(out2);
            XX_TEST_EXPECT_EQ(j2["ok"].get<bool>(), true);
            XX_TEST_EXPECT_EQ(j2["running"].get<bool>(), false);
        }
    }

    // ---- 5. 卸载插件 ----
    co_await ctx->pluginManager->unloadAsync("agentxx_text_selection_monitor");
    XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("agentxx_text_selection_monitor"));
    XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("agentxx_text_selection_monitor") == nullptr);
#else
    TEST_SKIP << "agentxx_text_selection_monitor 仅支持 Windows 平台" << std::endl;
#endif

    (void)agentContext;
    co_return TestResult{g_ts_passed, g_ts_failed};
}

} // namespace test
} // namespace agentxx
