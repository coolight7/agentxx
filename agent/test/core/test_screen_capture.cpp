#include "test_screen_capture.h"
#include "agentxx/agent/context.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/plugin_manager.h"
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

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

    // dataDir: 插件 entry 经 get_config 读取, 创建 {dataDir}/captures 作为
    // 截图 PNG 落盘目录 (save_images=true 时 image_path 指向此目录)。
    // 唯一化目录名避免多进程/多次运行互相干扰。
    const auto tmpDataDir = std::filesystem::temp_directory_path()
                            / ("agentxx_test_screen_capture_"
                               + std::to_string(
                                   std::chrono::steady_clock::now().time_since_epoch().count()
                               ));
    std::error_code fsEc;
    std::filesystem::remove_all(tmpDataDir, fsEc);
    std::filesystem::create_directories(tmpDataDir, fsEc);
    ctx->agentConfig->dataDir = tmpDataDir.string();

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
    XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("agentxx_get_screen_frames"));
    // 独立插件不注册 ui_control 工具 (已从 computer_use 拆分)
    XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("agentxx_ui_control_keyboard_mouse"));

    // ---- 3. get_screen_count ----
    int screenCount = 0;
    {
        auto tool = ctx->toolRegistry->find("agentxx_screen_capture");
        XX_TEST_EXPECT_TRUE(tool != nullptr);
        if (tool) {
            auto out = co_await tool->execute_async(neograph::json{
                {"command", "get_screen_count"}
            });
            auto j   = neograph::json::parse(out);
            XX_TEST_EXPECT_EQ(j["ok"].get<bool>(), true);
            screenCount = j["count"].get<int>();
            XX_TEST_EXPECT_TRUE(screenCount > 0);
        }
    }

    // ---- 4. capture_all (元信息, 不落盘图片) ----
    {
        auto tool = ctx->toolRegistry->find("agentxx_screen_capture");
        if (tool) {
            auto out = co_await tool->execute_async(neograph::json{
                {"command",     "capture_all" },
                {"save_images", false         },
            });
            auto j   = neograph::json::parse(out);
            XX_TEST_EXPECT_EQ(j["ok"].get<bool>(), true);
            XX_TEST_EXPECT_TRUE(j["frames"].size() > 0);
            const auto& f0 = j["frames"][0];
            XX_TEST_EXPECT_TRUE(f0["width"].get<int>() > 0);
            XX_TEST_EXPECT_TRUE(f0["height"].get<int>() > 0);
            // save_images=false: 不落盘, 无 image_path; 像素也永不进消息
            XX_TEST_EXPECT_FALSE(f0.contains("image_path"));
            XX_TEST_EXPECT_FALSE(f0.contains("pixels_base64"));
        }
    }

    // ---- 5. 默认不传 command (默认 capture_all) + save_images 落盘 PNG ----
    {
        auto tool = ctx->toolRegistry->find("agentxx_screen_capture");
        if (tool) {
            auto out = co_await tool->execute_async(neograph::json{
                {"save_images", true}
            });
            auto j   = neograph::json::parse(out);
            XX_TEST_EXPECT_EQ(j["ok"].get<bool>(), true);
            XX_TEST_EXPECT_EQ(j["frames"].size(), static_cast<size_t>(screenCount));
            for (size_t i = 0; i < j["frames"].size(); ++i) {
                const auto& f = j["frames"][i];
                XX_TEST_EXPECT_TRUE(f["width"].get<int>() > 0);
                XX_TEST_EXPECT_TRUE(f["height"].get<int>() > 0);
                // 新契约: 捕获像素经 WIC 编码 PNG 落盘到 {dataDir}/captures/,
                // 结果仅含元信息 + 文件路径 (pixel_bytes 为原始像素字节数),
                // 像素数据永不进入会话消息
                XX_TEST_EXPECT_TRUE(f["pixel_bytes"].get<int64_t>() > 0);
                XX_TEST_EXPECT_FALSE(f.contains("pixels_base64"));
                XX_TEST_EXPECT_TRUE(f.contains("image_path"));
                if (f.contains("image_path")) {
                    auto imgPath = f["image_path"].get<std::string>();
                    XX_TEST_EXPECT_TRUE(std::filesystem::exists(imgPath));
                    XX_TEST_EXPECT_TRUE(std::filesystem::file_size(imgPath) > 0);
                }
            }
        }
    }

    // ---- 6. capture_screen (指定屏幕下标 0) ----
    {
        auto tool = ctx->toolRegistry->find("agentxx_screen_capture");
        if (tool) {
            auto out = co_await tool->execute_async(neograph::json{
                {"command",      "capture_screen"},
                {"screen_index", 0               },
                {"save_images",  false          },
            });
            auto j   = neograph::json::parse(out);
            XX_TEST_EXPECT_EQ(j["ok"].get<bool>(), true);
            XX_TEST_EXPECT_EQ(j["frames"].size(), 1u);
            const auto& f0 = j["frames"][0];
            XX_TEST_EXPECT_EQ(f0["screen_index"].get<int>(), 0);
            XX_TEST_EXPECT_TRUE(f0["width"].get<int>() > 0);
            XX_TEST_EXPECT_TRUE(f0["height"].get<int>() > 0);
        }
    }

    // ---- 7. capture_screen (越界屏幕下标) ----
    {
        auto tool = ctx->toolRegistry->find("agentxx_screen_capture");
        if (tool) {
            auto out = co_await tool->execute_async(neograph::json{
                {"command",      "capture_screen"},
                {"screen_index", 99999           }
            });
            auto j   = neograph::json::parse(out);
            XX_TEST_EXPECT_EQ(j["ok"].get<bool>(), false);
        }
    }

    // ---- 8. agentxx_computer_use 依赖验证: 缺依赖时加载失败 ----
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
        XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("agentxx_get_screen_frames"));
        XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("agentxx_ui_control_keyboard_mouse"));
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("agentxx_screen_capture") == nullptr);
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("agentxx_computer_use") == nullptr);
    }

    // 清理临时 dataDir (截图 PNG 已在上方断言过存在性)
    std::filesystem::remove_all(tmpDataDir, fsEc);
#else
    TEST_SKIP << "agentxx_screen_capture (screen_capture) 仅支持 Windows 平台" << std::endl;
#endif

    co_return TestResult{g_sc_passed, g_sc_failed};
}

} // namespace test
} // namespace agentxx
