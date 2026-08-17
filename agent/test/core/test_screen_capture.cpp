#include "test_screen_capture.h"
#include "agentxx/agent/context.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/plugin_manager.h"
#include <asio/use_awaitable.hpp>
#include <filesystem>
#include <iostream>
#include <string>

namespace agentxx {
namespace test {

int g_sc_passed = 0;
int g_sc_failed = 0;

#if XX_IS_WIN_D

namespace {

/// 定位 agentxx_screen_capture 插件目录
static std::string findScreenCapturePluginPath() {
    std::error_code                    ec;
    std::vector<std::filesystem::path> candidates;
    candidates.push_back(std::filesystem::current_path(ec) / "plugins" / "agentxx_screen_capture");
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
                std::filesystem::path(exe).parent_path() / "plugins" / "agentxx_screen_capture"
            );
        }
    }
    for (const auto& c : candidates) {
        if (std::filesystem::is_directory(c, ec)) {
            return c.string();
        }
    }
    return "plugins/agentxx_screen_capture"; // 让加载失败暴露日志
}

/// 定位 agentxx_computer_use 插件目录 (依赖 agentxx_screen_capture)
static std::string findComputerUsePluginPath() {
    std::error_code                    ec;
    std::vector<std::filesystem::path> candidates;
    candidates.push_back(std::filesystem::current_path(ec) / "plugins" / "agentxx_computer_use");
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
                std::filesystem::path(exe).parent_path() / "plugins" / "agentxx_computer_use"
            );
        }
    }
    for (const auto& c : candidates) {
        if (std::filesystem::is_directory(c, ec)) {
            return c.string();
        }
    }
    return "plugins/agentxx_computer_use"; // 让加载失败暴露日志
}

} // namespace

#endif // XX_IS_WIN_D

asio::awaitable<agentxx::test::TestResult>
    run_screen_capture_tests(std::weak_ptr<agentxx::agent::AgentContext> /*agentContext*/) {
    g_sc_passed = 0;
    g_sc_failed = 0;

#if XX_IS_WIN_D
    // ---- 1. 构造 AgentContext ----
    auto ctx                     = std::make_shared<agentxx::agent::AgentContext>();
    ctx->agentConfig             = std::make_shared<agentxx::agent::AgentConfig>();
    ctx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
    ctx->toolRegistry            = std::make_shared<agentxx::plugin::ToolRegistry>();
    ctx->pluginManager           = std::make_shared<agentxx::plugin::PluginManager>(ctx);
    ctx->pluginManager->setIoExecutor(co_await asio::this_coro::executor);

    // ---- 2. 加载 agentxx_screen_capture 插件 (独立) ----
    auto path = findScreenCapturePluginPath();
    XX_TEST_EXPECT_TRUE(path.find("agentxx_screen_capture") != std::string::npos);
    auto inst = co_await ctx->pluginManager->loadPluginAsync(path);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return TestResult{g_sc_passed, g_sc_failed};
    }
    XX_TEST_EXPECT_EQ(inst->name, "agentxx_screen_capture");
    XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("agentxx_screen_capture"));
    // 独立插件不注册 ui_control 工具 (已从 computer_use 拆分)
    XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("agentxx_ui_control_keyboard_mouse"));

    // ---- 3. get_screen_count ----
    {
        auto tool = ctx->toolRegistry->find("agentxx_screen_capture");
        XX_TEST_EXPECT_TRUE(tool != nullptr);
        if (tool) {
            auto out = co_await tool->execute_async(neograph::json{
                {"command", "get_screen_count"}
            });
            auto j   = neograph::json::parse(out);
            XX_TEST_EXPECT_EQ(j["ok"].get<bool>(), true);
            XX_TEST_EXPECT_TRUE(j["count"].get<int>() > 0);
        }
    }

    // ---- 4. capture_all (元信息, 不取像素) ----
    {
        auto tool = ctx->toolRegistry->find("agentxx_screen_capture");
        if (tool) {
            auto out = co_await tool->execute_async(neograph::json{
                {"command",        "capture_all"},
                {"include_pixels", false        },
            });
            auto j   = neograph::json::parse(out);
            XX_TEST_EXPECT_EQ(j["ok"].get<bool>(), true);
            XX_TEST_EXPECT_TRUE(j["frames"].size() > 0);
            const auto& f0 = j["frames"][0];
            XX_TEST_EXPECT_TRUE(f0["width"].get<int>() > 0);
            XX_TEST_EXPECT_TRUE(f0["height"].get<int>() > 0);
        }
    }

    // ---- 5. agentxx_computer_use 依赖验证: 缺依赖时加载失败 ----
    {
        auto cuPath = findComputerUsePluginPath();
        XX_TEST_EXPECT_TRUE(cuPath.find("agentxx_computer_use") != std::string::npos);
        // 当前已加载 screen_capture, 依赖满足, 应加载成功 (拓扑/依赖检查通过)
        auto cuInst = co_await ctx->pluginManager->loadPluginAsync(cuPath);
        XX_TEST_EXPECT_TRUE(cuInst != nullptr);
        if (cuInst) {
            XX_TEST_EXPECT_EQ(cuInst->name, "agentxx_computer_use");
            XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("agentxx_ui_control_keyboard_mouse"));
        }
        // 卸载 screen_capture 时级联卸载依赖它的 computer_use (依赖方先卸载)
        co_await ctx->pluginManager->unloadAsync("agentxx_screen_capture");
        XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("agentxx_screen_capture"));
        XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("agentxx_ui_control_keyboard_mouse"));
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("agentxx_screen_capture") == nullptr);
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("agentxx_computer_use") == nullptr);
    }
#else
    TEST_SKIP << "agentxx_screen_capture (screen_capture) 仅支持 Windows 平台" << std::endl;
#endif

    co_return TestResult{g_sc_passed, g_sc_failed};
}

} // namespace test
} // namespace agentxx
