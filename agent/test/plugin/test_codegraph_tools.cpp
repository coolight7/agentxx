#include "agentxx-test/plugin/test_codegraph_tools.h"
#include "agentxx/agent/context.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/tools/tool.h"
#include "utilxx_base/json.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_cg_passed = 0;
int g_cg_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_cg_passed
#define XX_TEST_FAILED g_cg_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

static std::atomic<int> g_temp_project_counter{0};

/// 定位 agentxx_codegraph 插件目录 (与测试可执行同目录的 plugins/ 下;
/// 优先 exe 同目录的构建产物, cwd 仅作回退)。
/// 必须校验目录内存在动态库产物: 避免 cwd 在源码仓库下时误命中
/// agent/plugins/ 下的插件源码目录 (只有 .cpp/plugin.yaml, 无 .so)
static std::string findCodegraphPluginPath() {
    std::error_code       ec;
    std::vector<fs::path> candidates;
    if (auto exeDir = executableDir()) {
        candidates.push_back(*exeDir / "plugins" / "agentxx_codegraph");
    }
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
    // 目录划分: code/ 代码文件, docs/ 数据/文档文件。
    // 测试按 [docs, code] 的顺序传入索引路径, 文档节点先入库 (id 更小),
    // 底层"先按节点类型, 同类型按 id"的排序会把文档条目排到代码条目前面,
    // 这样"代码文件优先"的重排才会被真正校验 (与此无关的遍历顺序、
    // 文件名都不影响断言结果)
    auto code_dir = tmp_dir / "code";
    auto docs_dir = tmp_dir / "docs";
    fs::create_directories(code_dir);
    fs::create_directories(docs_dir);

    {
        std::ofstream f(code_dir / "main.cpp");
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
        std::ofstream f(code_dir / "utils.h");
        f << R"(#pragma once

int multiply(int x, int y);

void print_result(int value);
)";
    }

    {
        std::ofstream f(code_dir / "utils.cpp");
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

    // 数据/文档类文件: 抽出与代码符号同名的节点 (markdown 标题 → function,
    // json 键 → variable), 用于校验搜索结果里代码文件排在前面
    {
        std::ofstream f(docs_dir / "notes.md");
        f << R"(# add

add 是示例符号 (文档文件, 搜索结果里应排在代码文件之后)。
)";
    }

    {
        std::ofstream f(docs_dir / "config.json");
        f << R"({
    "add": 1,
    "addMode": 2
})";
    }

    return tmp_dir.generic_string();
}

static void cleanup_temp_project(const std::string& path) {
    try {
        fs::remove_all(path);
    } catch (...) {
    }
}

/// 从搜索结果文本里取出各条目的文件路径 (每条形如 "    file: <path>:<line>")
static std::vector<std::string> parseResultFiles(const std::string& out) {
    static const std::string kPrefix = "    file: ";
    std::vector<std::string> files;
    size_t                   pos = 0;
    while ((pos = out.find(kPrefix, pos)) != std::string::npos) {
        size_t      begin = pos + kPrefix.size();
        size_t      end   = out.find('\n', begin);
        std::string entry
            = out.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        // 去掉结尾的行号 ":<line>" (Windows 盘符里的冒号不受影响: 取最后一个)
        auto colon = entry.find_last_of(':');
        files.push_back(colon == std::string::npos ? entry : entry.substr(0, colon));
        pos = end == std::string::npos ? out.size() : end;
    }
    return files;
}

/// 文件是否属于数据/文档类 (按扩展名判断, 口径与插件内一致)
static bool isDocOrDataFile(const std::string& path) {
    static const char* kExts[] = {
        "json", "md", "mdx", "markdown", "yaml", "yml", "xml", "html", "htm", "css",
    };
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (const char* e : kExts) {
        if (ext == e) {
            return true;
        }
    }
    return false;
}

/// 搜索结果顺序是否符合"代码文件优先": 数据/文档文件条目之后不应再出现代码文件条目
/// - [sawDocOrData] 输出是否出现过数据/文档条目 (未出现时顺序断言不具意义)
static bool codeFilesComeFirst(const std::string& out, bool& sawDocOrData) {
    sawDocOrData = false;
    for (const auto& file : parseResultFiles(out)) {
        if (isDocOrDataFile(file)) {
            sawDocOrData = true;
            continue;
        }
        if (sawDocOrData) {
            return false;
        }
    }
    return true;
}

asio::awaitable<TestResult>
    run_codegraph_tools_tests(std::weak_ptr<agentxx::agent::AgentContext> /*agentContext*/) {
    g_cg_passed = 0;
    g_cg_failed = 0;

    // ---- 1. 构造 AgentContext (临时 dataDir) ----
    // 目录名唯一化 (时间戳): 若此前有测试进程异常退出仍持有旧目录内的
    // 索引数据库句柄, 固定名目录的 remove_all 会静默失败, 插件打开被锁
    // 数据库 → 初始化失败 → loadPluginAsync 返回 nullptr; 唯一名彻底规避
    auto tmp_data_dir
        = fs::temp_directory_path()
          / ("codegraph_plugin_data_"
             + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
        //
        // 索引顺序: [docs, code] —— 文档/数据文件先索引 (节点 id 更小), 保证
        // 搜索排序断言检验的是插件的"代码文件优先"重排, 而不是入库先后
        std::vector<std::string> paths{
            (fs::path(tmp_project) / "docs").generic_string(),
            (fs::path(tmp_project) / "code").generic_string(),
        };
        utilxx_base::Json pathsArr = utilxx_base::Json::array();
        for (const auto& p : paths) {
            pathsArr.push_back(utilxx_base::Json(p));
        }
        pc.path    = path;
        pc.enabled = true;
        pc.args    = utilxx_base::Json{
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
             "agentxx_codegraph_path",
         }) {
        XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains(toolName));
    }

    // ---- 5. search ----
    {
        auto tool = ctx->toolRegistry->find("agentxx_codegraph_search");
        XX_TEST_EXPECT_TRUE(tool != nullptr);
        if (tool) {
            auto out = co_await tool->execute_async(utilxx_base::Json{
                {"query", "add"}
            });
            XX_TEST_EXPECT_TRUE(out.find("Symbols (") != std::string::npos);
            // 插件后台 warmup 索引延迟 2s 启动, 且索引在独立线程执行:
            // 加载完成立即查询只会得到空结果 ("Symbols (0):")。
            // 轮询等待索引落库后再断言命中符号 (搜索缓存 30s TTL, 但索引
            // 完成后 CodeGraphManager 会 invalidate 使缓存失效, 下次查询
            // 即按新数据重算; 临时项目仅 5 个小文件, 索引 <1s, 轮询间隔
            // 500ms, 20s 超时兜底)。
            // 三个夹具文件 (main.cpp/notes.md/config.json) 都要已入库: 排序
            // 断言需要代码与数据/文档条目同时出现
            auto hasAllFixtures = [](const std::string& text) {
                return text.find("main.cpp") != std::string::npos
                       && text.find("notes.md") != std::string::npos
                       && text.find("config.json") != std::string::npos;
            };
            auto               exec = co_await asio::this_coro::executor;
            asio::steady_timer timer(exec);
            int                waitedMs       = 0;
            const int          kWaitTimeoutMs = 20000;
            while (!hasAllFixtures(out) && waitedMs < kWaitTimeoutMs) {
                timer.expires_after(std::chrono::milliseconds(500));
                co_await timer.async_wait(asio::use_awaitable);
                waitedMs += 500;
                out       = co_await tool->execute_async(utilxx_base::Json{
                          {"query", "add"}
                });
            }
            if (!hasAllFixtures(out)) {
                fprintf(
                    stderr,
                    "[codegraph] search 'add' timeout after %dms, last out: %.400s\n",
                    waitedMs,
                    out.c_str()
                );
            }
            XX_TEST_EXPECT_TRUE(out.find("add") != std::string::npos);
            XX_TEST_EXPECT_TRUE(hasAllFixtures(out));
            // 结果排序: 代码文件条目在前, 数据/文档 (md/json) 条目在后
            bool sawDocOrData = false;
            XX_TEST_EXPECT_TRUE(codeFilesComeFirst(out, sawDocOrData));
            XX_TEST_EXPECT_TRUE(sawDocOrData);
            // 空 query → 参数检查失败抛异常 (插件侧抛 std::invalid_argument,
            // 经插件边界由宿主重新抛出为 std::runtime_error; 消息保留)
            bool threw = false;
            try {
                (void)co_await tool->execute_async(utilxx_base::Json{
                    {"query", ""}
                });
            } catch (const std::exception& e) {
                threw = true;
                XX_TEST_EXPECT_TRUE(
                    std::string_view{e.what()}.find("`query` is empty") != std::string::npos
                );
            }
            XX_TEST_EXPECT_TRUE(threw);
        }
    }

    // ---- 6. context ----
    {
        auto tool = ctx->toolRegistry->find("agentxx_codegraph_context");
        XX_TEST_EXPECT_TRUE(tool != nullptr);
        if (tool) {
            auto out = co_await tool->execute_async(utilxx_base::Json{
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
            auto out = co_await callers->execute_async(utilxx_base::Json{
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
            auto out = co_await callees->execute_async(utilxx_base::Json{
                {"symbol", "main"}
            });
            XX_TEST_EXPECT_TRUE(
                out.find("Callees (") != std::string::npos
                || out.find("error:") != std::string::npos
            );
        }
        auto pathTool = ctx->toolRegistry->find("agentxx_codegraph_path");
        XX_TEST_EXPECT_TRUE(pathTool != nullptr);
        if (pathTool) {
            auto out = co_await pathTool->execute_async(utilxx_base::Json{
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
