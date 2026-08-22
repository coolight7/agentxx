#include "test_codegraph_tools.h"
#include "agentxx/agent/context.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/tools/tool.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace agentxx {
namespace test {

int g_cg_passed = 0;
int g_cg_failed = 0;

namespace fs = std::filesystem;

static std::atomic<int> g_temp_project_counter{0};

/// 定位 agentxx_codegraph 插件目录 (与测试可执行同目录的 plugins/ 下;
/// 优先 exe 同目录的构建产物, cwd 仅作回退)。
/// 必须校验目录内存在动态库产物: 避免 cwd 在源码仓库下时误命中
/// agent/plugins/ 下的插件源码目录 (只有 .cpp/plugin.yaml, 无 .so)
static std::string findCodegraphPluginPath() {
    std::error_code       ec;
    std::vector<fs::path> candidates;
#if !XX_IS_WIN_D
    if (auto p = fs::read_symlink("/proc/self/exe", ec); !ec) {
        candidates.push_back(p.parent_path() / "plugins" / "agentxx_codegraph");
    }
#endif
    candidates.push_back(fs::current_path(ec) / "plugins" / "agentxx_codegraph");
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
    auto            tmp_data_dir = fs::temp_directory_path() / "codegraph_plugin_data";
    std::error_code ec;
    fs::remove_all(tmp_data_dir, ec);
    fs::create_directories(tmp_data_dir, ec);

    auto ctx                  = std::make_shared<agentxx::agent::AgentContext>();
    ctx->agentConfig          = std::make_shared<agentxx::agent::AgentConfig>();
    ctx->agentConfig->dataDir = tmp_data_dir.string();
    // ---- 2. 加载 agentxx_codegraph 插件 ----
    auto path = findCodegraphPluginPath();
    XX_TEST_EXPECT_TRUE(path.find("agentxx_codegraph") != std::string::npos);
    // 插件参数经 plugins 配置传递 (宿主不解析 args 字段语义, 整体传给插件;
    // 按配置 path 匹配插件, 与 yaml 配置行为一致)。注意: 必须把 cfg 传入
    // loadPluginAsync(path, &cfg) —— 单参形式 cfg=nullptr 时 args 不会写入
    // PluginInstance, 插件读到的是默认配置 (load_cwd=true → 索引测试进程
    // cwd 的 exec 目录, 巨大的二进制/so, 20s 内索引进度有限)。
    //
    // 索引范围: 显式传入临时项目 (args "paths" = loadPaths) 而非 load_cwd
    // —— codegraph status/index 工具已移除 (2026-08), 插件加载后由后台
    // warmup 线程 (2s 延迟) 按 loadPaths 执行 updateIndex。
    auto                         tmp_project = create_temp_project();
    agentxx::agent::PluginConfig pc;
    {
        // 注意: 不能写成 json::array({json{path}}) 或 json{path} —— braced
        // list-init 会优先匹配 json 的 initializer_list 构造 (元素数组),
        // 产出嵌套数组 [[path]]; json{path} 同理得到 1 元素数组而非字符串。
        // 用圆括号构造 json(path) (普通构造函数, 走 string 构造) 再入数组
        neograph::json pathsArr = neograph::json::array();
        pathsArr.push_back(neograph::json(tmp_project));
        pc.path    = path;
        pc.enabled = true;
        pc.args    = neograph::json{
               {"paths",         std::move(pathsArr)},
               {"load_cwd",      false              },
               {"use_gitignore", true               }
        };
        ctx->agentConfig->plugins.push_back(pc);
    }
    ctx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
    ctx->toolRegistry            = std::make_shared<agentxx::plugin::ToolRegistry>();
    ctx->pluginManager           = std::make_shared<agentxx::plugin::PluginManager>(ctx);
    ctx->pluginManager->setIoExecutor(co_await asio::this_coro::executor);

    auto inst = co_await ctx->pluginManager->loadPluginAsync(path, &pc);
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
             "agentxx_codegraph_path",
         }) {
        XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains(toolName));
    }

    // ---- 5. search ----
    {
        auto tool = ctx->toolRegistry->find("agentxx_codegraph_search");
        XX_TEST_EXPECT_TRUE(tool != nullptr);
        if (tool) {
            auto out = co_await tool->execute_async(neograph::json{
                {"query", "add"}
            });
            XX_TEST_EXPECT_TRUE(out.find("Symbols (") != std::string::npos);
            // 插件后台 warmup 索引延迟 2s 启动, 且索引在独立线程执行:
            // 加载完成立即查询只会得到空结果 ("Symbols (0):")。
            // 轮询等待索引落库后再断言命中符号 (搜索缓存 30s TTL, 但索引
            // 完成后 CodeGraphManager 会 invalidate 使缓存失效, 下次查询
            // 即按新数据重算; 临时项目仅 3 个小文件, 索引 <1s, 轮询间隔
            // 500ms, 20s 超时兜底)。
            auto               exec = co_await asio::this_coro::executor;
            asio::steady_timer timer(exec);
            int                waitedMs       = 0;
            const int          kWaitTimeoutMs = 20000;
            while (out.find("add") == std::string::npos && waitedMs < kWaitTimeoutMs) {
                timer.expires_after(std::chrono::milliseconds(500));
                co_await timer.async_wait(asio::use_awaitable);
                waitedMs += 500;
                out       = co_await tool->execute_async(neograph::json{
                          {"query", "add"}
                });
            }
            if (out.find("add") == std::string::npos) {
                fprintf(
                    stderr,
                    "[codegraph] search 'add' timeout after %dms, last out: %.200s\n",
                    waitedMs,
                    out.c_str()
                );
            }
            XX_TEST_EXPECT_TRUE(out.find("add") != std::string::npos);
            // 空 query → error
            auto err = co_await tool->execute_async(neograph::json{
                {"query", ""}
            });
            XX_TEST_EXPECT_TRUE(err.find("error:") != std::string::npos);
        }
    }

    // ---- 6. context ----
    {
        auto tool = ctx->toolRegistry->find("agentxx_codegraph_context");
        XX_TEST_EXPECT_TRUE(tool != nullptr);
        if (tool) {
            auto out = co_await tool->execute_async(neograph::json{
                {"symbol", "add"}
            });
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
            auto out = co_await callers->execute_async(neograph::json{
                {"symbol", "add"}
            });
            XX_TEST_EXPECT_TRUE(
                out.find("Callers (") != std::string::npos
                || out.find("error:") != std::string::npos
            );
        }
        auto callees = ctx->toolRegistry->find("agentxx_codegraph_callees");
        XX_TEST_EXPECT_TRUE(callees != nullptr);
        if (callees) {
            auto out = co_await callees->execute_async(neograph::json{
                {"symbol", "main"}
            });
            XX_TEST_EXPECT_TRUE(
                out.find("Callees (") != std::string::npos
                || out.find("error:") != std::string::npos
            );
        }
        auto impact = ctx->toolRegistry->find("agentxx_codegraph_impact");
        XX_TEST_EXPECT_TRUE(impact != nullptr);
        if (impact) {
            auto out = co_await impact->execute_async(neograph::json{
                {"symbol", "add"}
            });
            XX_TEST_EXPECT_TRUE(
                out.find("Impact (") != std::string::npos || out.find("error:") != std::string::npos
            );
        }
        auto pathTool = ctx->toolRegistry->find("agentxx_codegraph_path");
        XX_TEST_EXPECT_TRUE(pathTool != nullptr);
        if (pathTool) {
            auto out = co_await pathTool->execute_async(neograph::json{
                {"from", "main"    },
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
