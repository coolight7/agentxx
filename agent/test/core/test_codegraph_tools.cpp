#include "test_codegraph_tools.h"
#include "agentxx/agent/context.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/tools/tool.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/use_awaitable.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace agentxx {
namespace test {

int g_cg_passed = 0;
int g_cg_failed = 0;

namespace fs = std::filesystem;

static std::atomic<int> g_temp_project_counter{0};

/// 定位 agentxx_codegraph 插件目录 (与测试可执行同目录的 plugins/ 下;
/// 兼容从其他 cwd 运行: 回退可执行文件目录)
static std::string findCodegraphPluginPath() {
    std::error_code ec;
    std::vector<fs::path> candidates;
    candidates.push_back(fs::current_path(ec) / "plugins" / "agentxx_codegraph");
#if !XX_IS_WIN_D
    if (auto p = fs::read_symlink("/proc/self/exe", ec); !ec) {
        candidates.push_back(p.parent_path() / "plugins" / "agentxx_codegraph");
    }
#endif
    for (const auto& c : candidates) {
        if (fs::is_directory(c, ec)) {
            return c.string();
        }
    }
    return "plugins/agentxx_codegraph"; // 让加载失败暴露日志
}

static std::string create_temp_project() {
    int  idx     = g_temp_project_counter.fetch_add(1);
    auto tmp_dir = fs::temp_directory_path() / ("codegraph_plugin_test_" + std::to_string(idx));
    if (fs::exists(tmp_dir)) {
        fs::remove_all(tmp_dir);
    }
    fs::create_directories(tmp_dir);

    {
        std::ofstream f(tmp_dir / "main.cpp");
        f << R"(#include "utils.h"

int add(int a, int b) {
    return a + b;
}

int main() {
    int result = add(1, 2);
    int doubled = multiply(result, 2);
    return doubled;
}
)";
    }

    {
        std::ofstream f(tmp_dir / "utils.h");
        f << R"(#pragma once

int multiply(int x, int y);

void print_result(int value);
)";
    }

    {
        std::ofstream f(tmp_dir / "utils.cpp");
        f << R"(#include "utils.h"
#include <iostream>

int multiply(int x, int y) {
    int result = 0;
    for (int i = 0; i < y; i++) {
        result = add_impl(result, x);
    }
    return result;
}

static int add_impl(int a, int b) {
    return a + b;
}

void print_result(int value) {
    std::cout << "Result: " << value << std::endl;
}
)";
    }

    return tmp_dir.generic_string();
}

static void cleanup_temp_project(const std::string& path) {
    try {
        fs::remove_all(path);
    } catch (...) {
    }
}

asio::awaitable<TestResult>
    run_codegraph_tools_tests(std::weak_ptr<agentxx::agent::AgentContext> /*agentContext*/) {
    g_cg_passed = 0;
    g_cg_failed = 0;

    // ---- 1. 构造 AgentContext (临时 dataDir) ----
    auto tmp_data_dir = fs::temp_directory_path() / "codegraph_plugin_data";
    std::error_code ec;
    fs::remove_all(tmp_data_dir, ec);
    fs::create_directories(tmp_data_dir, ec);

    auto ctx                     = std::make_shared<agentxx::agent::AgentContext>();
    ctx->agentConfig             = std::make_shared<agentxx::agent::AgentConfig>();
    ctx->agentConfig->dataDir    = tmp_data_dir.string();
    // ---- 2. 加载 agentxx_codegraph 插件 ----
    auto path = findCodegraphPluginPath();
    XX_TEST_EXPECT_TRUE(path.find("agentxx_codegraph") != std::string::npos);
    // 插件参数经 plugins 配置传递 (宿主不解析 args 字段语义, 整体传给插件;
    // 按配置 path 匹配插件, 与 yaml 配置行为一致)
    {
        agentxx::agent::PluginConfig pc;
        pc.path    = path;
        pc.enabled = true;
        pc.args    = neograph::json{{"load_cwd", true}, {"use_gitignore", true}};
        ctx->agentConfig->plugins.push_back(std::move(pc));
    }
    ctx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
    ctx->toolRegistry            = std::make_shared<agentxx::plugin::ToolRegistry>();
    ctx->pluginManager           = std::make_shared<agentxx::plugin::PluginManager>(ctx);
    ctx->pluginManager->setIoExecutor(co_await asio::this_coro::executor);

    auto inst = co_await ctx->pluginManager->loadPluginAsync(path);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return TestResult{g_cg_passed, g_cg_failed};
    }
    XX_TEST_EXPECT_EQ(inst->name, "agentxx_codegraph");
    // 8 个工具全部注册
    for (const char* toolName : {
             "agentxx_codegraph_search",
             "agentxx_codegraph_context",
             "agentxx_codegraph_callers",
             "agentxx_codegraph_callees",
             "agentxx_codegraph_impact",
             "agentxx_codegraph_status",
             "agentxx_codegraph_index",
             "agentxx_codegraph_path",
         }) {
        XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains(toolName));
    }

    // ---- 3. 索引临时项目 ----
    auto tmp_project = create_temp_project();
    {
        auto tool = ctx->toolRegistry->find("agentxx_codegraph_index");
        XX_TEST_EXPECT_TRUE(tool != nullptr);
        if (tool) {
            auto out = co_await tool->execute_async(neograph::json{
                {"path", tmp_project},
                {"incremental", false},
            });
            XX_TEST_EXPECT_TRUE(out.find("success: true") != std::string::npos);
        }
    }

    // ---- 4. status ----
    {
        auto tool = ctx->toolRegistry->find("agentxx_codegraph_status");
        XX_TEST_EXPECT_TRUE(tool != nullptr);
        if (tool) {
            auto out = co_await tool->execute_async(neograph::json::object());
            XX_TEST_EXPECT_TRUE(out.find("total_nodes:") != std::string::npos);
            XX_TEST_EXPECT_TRUE(out.find("total_files:") != std::string::npos);
        }
    }

    // ---- 5. search ----
    {
        auto tool = ctx->toolRegistry->find("agentxx_codegraph_search");
        XX_TEST_EXPECT_TRUE(tool != nullptr);
        if (tool) {
            auto out = co_await tool->execute_async(neograph::json{{"query", "add"}});
            XX_TEST_EXPECT_TRUE(out.find("Symbols (") != std::string::npos);
            XX_TEST_EXPECT_TRUE(out.find("add") != std::string::npos);
            // 空 query → error
            auto err = co_await tool->execute_async(neograph::json{{"query", ""}});
            XX_TEST_EXPECT_TRUE(err.find("error:") != std::string::npos);
        }
    }

    // ---- 6. context ----
    {
        auto tool = ctx->toolRegistry->find("agentxx_codegraph_context");
        XX_TEST_EXPECT_TRUE(tool != nullptr);
        if (tool) {
            auto out = co_await tool->execute_async(neograph::json{{"symbol", "add"}});
            XX_TEST_EXPECT_TRUE(
                out.find("symbol:") != std::string::npos || out.find("error:") != std::string::npos
            );
        }
    }

    // ---- 7. callers / callees / impact / path ----
    {
        auto callers = ctx->toolRegistry->find("agentxx_codegraph_callers");
        XX_TEST_EXPECT_TRUE(callers != nullptr);
        if (callers) {
            auto out = co_await callers->execute_async(neograph::json{{"symbol", "add"}});
            XX_TEST_EXPECT_TRUE(
                out.find("Callers (") != std::string::npos || out.find("error:") != std::string::npos
            );
        }
        auto callees = ctx->toolRegistry->find("agentxx_codegraph_callees");
        XX_TEST_EXPECT_TRUE(callees != nullptr);
        if (callees) {
            auto out = co_await callees->execute_async(neograph::json{{"symbol", "main"}});
            XX_TEST_EXPECT_TRUE(
                out.find("Callees (") != std::string::npos || out.find("error:") != std::string::npos
            );
        }
        auto impact = ctx->toolRegistry->find("agentxx_codegraph_impact");
        XX_TEST_EXPECT_TRUE(impact != nullptr);
        if (impact) {
            auto out = co_await impact->execute_async(neograph::json{{"symbol", "add"}});
            XX_TEST_EXPECT_TRUE(
                out.find("Impact (") != std::string::npos || out.find("error:") != std::string::npos
            );
        }
        auto pathTool = ctx->toolRegistry->find("agentxx_codegraph_path");
        XX_TEST_EXPECT_TRUE(pathTool != nullptr);
        if (pathTool) {
            auto out = co_await pathTool->execute_async(neograph::json{
                {"from", "main"},
                {"to",   "multiply"},
            });
            XX_TEST_EXPECT_TRUE(
                out.find("Path (") != std::string::npos || out.find("error:") != std::string::npos
            );
        }
    }

    // ---- 8. 卸载: 工具全部摘除 ----
    {
        auto ok = co_await ctx->pluginManager->unloadAsync("agentxx_codegraph");
        XX_TEST_EXPECT_TRUE(ok);
        XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("agentxx_codegraph_search"));
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("agentxx_codegraph") == nullptr);
    }

    cleanup_temp_project(tmp_project);
    fs::remove_all(tmp_data_dir, ec);

    co_return TestResult{g_cg_passed, g_cg_failed};
}

} // namespace test
} // namespace agentxx
