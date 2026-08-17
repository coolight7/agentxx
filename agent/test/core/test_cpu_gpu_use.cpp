#include "test_cpu_gpu_use.h"
#include "agentxx/agent/context.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/tool_registry.h"
#include "asio/use_awaitable.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace agentxx {
namespace test {

int g_cpu_passed = 0;
int g_cpu_failed = 0;

#if XX_IS_WIN_D || XX_IS_LINUX_D

namespace {

/// 定位 agentxx_system_monitor 插件目录
static std::string findSystemMonitorPluginPath() {
    std::error_code                    ec;
    std::vector<std::filesystem::path> candidates;
    candidates.push_back(std::filesystem::current_path(ec) / "plugins" / "agentxx_system_monitor");
#if XX_IS_WIN_D
    wchar_t buf[4096];
    DWORD   n = ::GetModuleFileNameW(nullptr, buf, 4096);
    if (n > 0 && n < 4096) {
        int len = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            buf,
            static_cast<int>(n),
            nullptr,
            0,
            nullptr,
            nullptr
        );
        if (len > 0) {
            std::string exe(static_cast<size_t>(len), '\0');
            ::WideCharToMultiByte(
                CP_UTF8,
                0,
                buf,
                static_cast<int>(n),
                exe.data(),
                len,
                nullptr,
                nullptr
            );
            candidates.push_back(
                std::filesystem::path(exe).parent_path() / "plugins" / "agentxx_system_monitor"
            );
        }
    }
#endif
    for (const auto& c : candidates) {
        if (std::filesystem::is_directory(c, ec)) {
            return c.string();
        }
    }
    return "plugins/agentxx_system_monitor"; // 让加载失败暴露日志
}

} // namespace

#endif // XX_IS_WIN_D || XX_IS_LINUX_D

asio::awaitable<TestResult>
    run_cpu_gpu_use_tests(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    g_cpu_passed = 0;
    g_cpu_failed = 0;

#if XX_IS_WIN_D || XX_IS_LINUX_D
    // ---- 1. 构造 AgentContext ----
    auto ctx                     = std::make_shared<agentxx::agent::AgentContext>();
    ctx->agentConfig             = std::make_shared<agentxx::agent::AgentConfig>();
    ctx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
    ctx->toolRegistry            = std::make_shared<agentxx::plugin::ToolRegistry>();
    ctx->pluginManager           = std::make_shared<agentxx::plugin::PluginManager>(ctx);
    ctx->pluginManager->setIoExecutor(co_await asio::this_coro::executor);

    // ---- 2. 加载 agentxx_system_monitor 插件 ----
    auto path = findSystemMonitorPluginPath();
    XX_TEST_EXPECT_TRUE(path.find("agentxx_system_monitor") != std::string::npos);
    auto inst = co_await ctx->pluginManager->loadPluginAsync(path);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return TestResult{g_cpu_passed, g_cpu_failed};
    }
    XX_TEST_EXPECT_EQ(inst->name, "agentxx_system_monitor");
    XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("agentxx_get_system_core_info"));

    // ---- 3. 工具 agentxx_get_system_core_info (原内置工具迁移) ----
    {
        auto tool = ctx->toolRegistry->find("agentxx_get_system_core_info");
        XX_TEST_EXPECT_TRUE(tool != nullptr);
        if (tool) {
            auto out = co_await tool->execute_async(neograph::json::object());
            TEST_INFO << "get_system_core_info output:\n" << out << std::endl;
            XX_TEST_EXPECT_TRUE(out.find("CPU Usage:") != std::string::npos);
            XX_TEST_EXPECT_TRUE(out.find("Memory:") != std::string::npos);
        }
    }

    // ---- 4. 能力 agentxx.system_usage (agent 侧周期采集 publish 的数据源) ----
    {
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->hasCapability("agentxx.system_usage"));
        char* err  = nullptr;
        char* json = ctx->pluginManager
                         ->invokeCapability(nullptr, "agentxx.system_usage", "query", "{}", &err);
        XX_TEST_EXPECT_TRUE(json != nullptr);
        if (json) {
            auto j = neograph::json::parse(json);
            std::free(json);
            if (j.is_object()) {
                double cpu = j.value("cpu", -1.0);
                XX_TEST_EXPECT_TRUE(cpu >= 0.0 && cpu <= 100.0);
                uint64_t total = j.value("mem_total_mb", uint64_t{0});
                XX_TEST_EXPECT_TRUE(total > 0);
                XX_TEST_EXPECT_TRUE(j.contains("gpus"));
            } else {
                g_cpu_failed++;
                TEST_FAIL << "system_usage capability returned non-object json" << std::endl;
            }
        } else {
            if (err) {
                TEST_FAIL << "system_usage capability failed: " << err << std::endl;
                std::free(err);
            } else {
                g_cpu_failed++;
                TEST_FAIL << "system_usage capability failed (no error)" << std::endl;
            }
        }
    }

    // ---- 5. 卸载插件 (工具/能力摘除) ----
    co_await ctx->pluginManager->unloadAsync("agentxx_system_monitor");
    XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("agentxx_get_system_core_info"));
    XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("agentxx_system_monitor") == nullptr);
#else
    TEST_SKIP << "agentxx_system_monitor (system monitor) 仅支持 Windows/Linux 平台" << std::endl;
#endif

    (void)agentContext;
    co_return TestResult{g_cpu_passed, g_cpu_failed};
}

} // namespace test
} // namespace agentxx
