#include "test_math_tools.h"
#include "agentxx/agent/context.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/tool_registry.h"
#include "math_impl.h"
#include <asio/awaitable.hpp>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <neograph/types.h>
#include <string>
#include <vector>

#if XX_IS_WIN_D
#include <windows.h>
#endif

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_math_passed = 0;
int g_math_failed = 0;
} // namespace

namespace agentxx {
namespace tools {

/// 测试适配: 插件工具薄包装
struct MathCalculateTool {
    explicit MathCalculateTool(std::weak_ptr<agentxx::agent::AgentContext>) {}

    neograph::ChatTool get_definition() const {
        return {
            "agentxx_math_calculate",
            "Evaluate a mathematical expression and return the computed result.",
            {}
        };
    }

    asio::awaitable<std::string> execute_async(const neograph::json& args) const {
        co_return agentxx_math_plugin::mathCalculateExecute(args);
    }
};

} // namespace tools
} // namespace agentxx

namespace agentxx {
namespace test {

static void checkResult(
    const std::string& actual,
    const std::string& expected,
    const std::string& testName
) {
    if (actual == expected) {
        g_math_passed++;
        TEST_PASS << testName << " -> " << actual << std::endl;
    } else {
        g_math_failed++;
        std::cout << "[FAIL] " << testName << " expected: '" << expected << "', got: '" << actual
                  << "'" << std::endl;
    }
}

static void checkContains(
    const std::string& actual,
    const std::string& substr,
    const std::string& testName
) {
    if (actual.find(substr) != std::string::npos) {
        g_math_passed++;
        TEST_PASS << testName << " contains '" << substr << "' -> " << actual << std::endl;
    } else {
        g_math_failed++;
        std::cout << "[FAIL] " << testName << " expected to contain '" << substr << "', got: '"
                  << actual << "'" << std::endl;
    }
}

// 1. 基本四则与优先级
static asio::awaitable<void> test_basic_arithmetic(std::weak_ptr<agentxx::agent::AgentContext> ctx
) {
    auto tool = agentxx::tools::MathCalculateTool{ctx};

    auto r1 = co_await tool.execute_async({
        {"expression", "2 + 3 * 4"}
    });
    checkResult(r1, "14", "2 + 3 * 4");

    auto r2 = co_await tool.execute_async({
        {"expression", "(2 + 3) * 4"}
    });
    checkResult(r2, "20", "(2 + 3) * 4");

    auto r3 = co_await tool.execute_async({
        {"expression", "10 - 4 - 2"}
    });
    checkResult(r3, "4", "10 - 4 - 2");

    auto r4 = co_await tool.execute_async({
        {"expression", "15 / 4"}
    });
    checkResult(r4, "3.75", "15 / 4");

    auto r5 = co_await tool.execute_async({
        {"expression", "15 // 4"}
    });
    checkResult(r5, "3", "15 // 4 (floor div)");

    auto r6 = co_await tool.execute_async({
        {"expression", "15 % 4"}
    });
    checkResult(r6, "3", "15 % 4 (modulo)");

    auto r7 = co_await tool.execute_async({
        {"expression", "-5 + 8"}
    });
    checkResult(r7, "3", "-5 + 8");

    auto r8 = co_await tool.execute_async({
        {"expression", "-(-5)"}
    });
    checkResult(r8, "5", "-(-5)");

    co_return;
}

// 2. 乘方与阶乘
static asio::awaitable<void>
    test_power_and_factorial(std::weak_ptr<agentxx::agent::AgentContext> ctx) {
    auto tool = agentxx::tools::MathCalculateTool{ctx};

    auto r1 = co_await tool.execute_async({
        {"expression", "2^10"}
    });
    checkResult(r1, "1024", "2^10");

    auto r2 = co_await tool.execute_async({
        {"expression", "2**10"}
    });
    checkResult(r2, "1024", "2**10");

    // 右结合 2^3^2 = 2^(3^2) = 2^9 = 512
    auto r3 = co_await tool.execute_async({
        {"expression", "2^3^2"}
    });
    checkResult(r3, "512", "2^3^2 right-associative");

    auto r4 = co_await tool.execute_async({
        {"expression", "5!"}
    });
    checkResult(r4, "120", "5!");

    auto r5 = co_await tool.execute_async({
        {"expression", "0!"}
    });
    checkResult(r5, "1", "0!");

    auto r6 = co_await tool.execute_async({
        {"expression", "(3 + 2)!"}
    });
    checkResult(r6, "120", "(3 + 2)!");

    auto r7 = co_await tool.execute_async({
        {"expression", "2^3!"}
    });
    checkResult(r7, "64", "2^3! (2^6)");

    co_return;
}

// 3. 隐式乘法
static asio::awaitable<void>
    test_implicit_multiplication(std::weak_ptr<agentxx::agent::AgentContext> ctx) {
    auto tool = agentxx::tools::MathCalculateTool{ctx};

    auto r1 = co_await tool.execute_async({
        {"expression", "2(3 + 4)"}
    });
    checkResult(r1, "14", "2(3 + 4)");

    auto r2 = co_await tool.execute_async({
        {"expression", "(1 + 2)(3 + 4)"}
    });
    checkResult(r2, "21", "(1 + 2)(3 + 4)");

    auto r3 = co_await tool.execute_async({
        {"expression", "3sqrt(16)"}
    });
    checkResult(r3, "12", "3sqrt(16)");

    auto r4 = co_await tool.execute_async({
        {"expression", "2[3 + 4]"}
    });
    checkResult(r4, "14", "2[3 + 4]");

    co_return;
}

// 4. 常量与基础函数
static asio::awaitable<void>
    test_constants_and_functions(std::weak_ptr<agentxx::agent::AgentContext> ctx) {
    auto tool = agentxx::tools::MathCalculateTool{ctx};

    auto r1 = co_await tool.execute_async({
        {"expression", "sqrt(144)"}
    });
    checkResult(r1, "12", "sqrt(144)");

    auto r2 = co_await tool.execute_async({
        {"expression", "cbrt(27)"}
    });
    checkResult(r2, "3", "cbrt(27)");

    auto r3 = co_await tool.execute_async({
        {"expression", "abs(-42)"}
    });
    checkResult(r3, "42", "abs(-42)");

    auto r4 = co_await tool.execute_async({
        {"expression", "floor(3.9)"}
    });
    checkResult(r4, "3", "floor(3.9)");

    auto r5 = co_await tool.execute_async({
        {"expression", "ceil(3.1)"}
    });
    checkResult(r5, "4", "ceil(3.1)");

    auto r6 = co_await tool.execute_async({
        {"expression", "round(3.567, 2)"}
    });
    checkResult(r6, "3.57", "round(3.567, 2)");

    auto r7 = co_await tool.execute_async({
        {"expression", "log10(1000)"}
    });
    checkResult(r7, "3", "log10(1000)");

    auto r8 = co_await tool.execute_async({
        {"expression", "log2(256)"}
    });
    checkResult(r8, "8", "log2(256)");

    auto r9 = co_await tool.execute_async({
        {"expression", "exp(0)"}
    });
    checkResult(r9, "1", "exp(0)");

    auto r10 = co_await tool.execute_async({
        {"expression", "ln(e)"}
    });
    checkResult(r10, "1", "ln(e)");

    auto r11 = co_await tool.execute_async({
        {"expression", "min(5, 2, 8, 1, 9)"}
    });
    checkResult(r11, "1", "min(...)");

    auto r12 = co_await tool.execute_async({
        {"expression", "max(5, 2, 8, 1, 9)"}
    });
    checkResult(r12, "9", "max(...)");

    auto r13 = co_await tool.execute_async({
        {"expression", "sum(1, 2, 3, 4, 5)"}
    });
    checkResult(r13, "15", "sum(...)");

    auto r14 = co_await tool.execute_async({
        {"expression", "avg(10, 20, 30)"}
    });
    checkResult(r14, "20", "avg(...)");

    auto r15 = co_await tool.execute_async({
        {"expression", "gcd(48, 18)"}
    });
    checkResult(r15, "6", "gcd(48, 18)");

    auto r16 = co_await tool.execute_async({
        {"expression", "lcm(12, 18)"}
    });
    checkResult(r16, "36", "lcm(12, 18)");

    auto r17 = co_await tool.execute_async({
        {"expression", "comb(5, 2)"}
    });
    checkResult(r17, "10", "comb(5, 2)");

    auto r18 = co_await tool.execute_async({
        {"expression", "perm(5, 2)"}
    });
    checkResult(r18, "20", "perm(5, 2)");

    auto r19 = co_await tool.execute_async({
        {"expression", "clamp(15, 0, 10)"}
    });
    checkResult(r19, "10", "clamp(15, 0, 10)");

    auto r20 = co_await tool.execute_async({
        {"expression", "hypot(3, 4)"}
    });
    checkResult(r20, "5", "hypot(3, 4)");

    co_return;
}

// 5. 三角函数与角度模式
static asio::awaitable<void> test_trigonometric(std::weak_ptr<agentxx::agent::AgentContext> ctx) {
    auto tool = agentxx::tools::MathCalculateTool{ctx};

    // 默认弧度
    auto r1 = co_await tool.execute_async({
        {"expression", "sin(pi / 2)"}
    });
    checkResult(r1, "1", "sin(pi / 2)");

    auto r2 = co_await tool.execute_async({
        {"expression", "cos(0)"}
    });
    checkResult(r2, "1", "cos(0)");

    // 显式角度函数 sind, cosd
    auto r3 = co_await tool.execute_async({
        {"expression", "sind(90)"}
    });
    checkResult(r3, "1", "sind(90)");

    auto r4 = co_await tool.execute_async({
        {"expression", "cosd(180)"}
    });
    checkResult(r4, "-1", "cosd(180)");

    auto r5 = co_await tool.execute_async({
        {"expression", "tand(45)"}
    });
    checkResult(r5, "1", "tand(45)");

    // angle_unit = "deg"
    auto r6 = co_await tool.execute_async({
        {"expression", "sin(30)"},
        {"angle_unit", "deg"    },
        {"precision",  2        }
    });
    checkResult(r6, "0.50", "sin(30) with angle_unit=deg");

    auto r7 = co_await tool.execute_async({
        {"expression", "asin(0.5)"},
        {"angle_unit", "deg"      }
    });
    checkResult(r7, "30", "asin(0.5) with angle_unit=deg");

    // rad / deg 转换函数
    auto r8 = co_await tool.execute_async({
        {"expression", "deg(pi)"}
    });
    checkResult(r8, "180", "deg(pi)");

    auto r9 = co_await tool.execute_async({
        {"expression", "rad(180) / pi"}
    });
    checkResult(r9, "1", "rad(180) / pi");

    co_return;
}

// 6. 位运算与进制字面量
static asio::awaitable<void> test_bitwise_and_bases(std::weak_ptr<agentxx::agent::AgentContext> ctx
) {
    auto tool = agentxx::tools::MathCalculateTool{ctx};

    auto r1 = co_await tool.execute_async({
        {"expression", "0xFF"}
    });
    checkResult(r1, "255", "0xFF");

    auto r2 = co_await tool.execute_async({
        {"expression", "0b101010"}
    });
    checkResult(r2, "42", "0b101010");

    auto r3 = co_await tool.execute_async({
        {"expression", "0o77"}
    });
    checkResult(r3, "63", "0o77");

    auto r4 = co_await tool.execute_async({
        {"expression", "12 & 10"}
    });
    checkResult(r4, "8", "12 & 10");

    auto r5 = co_await tool.execute_async({
        {"expression", "12 | 10"}
    });
    checkResult(r5, "14", "12 | 10");

    auto r6 = co_await tool.execute_async({
        {"expression", "xor(12, 10)"}
    });
    checkResult(r6, "6", "xor(12, 10)");

    auto r7 = co_await tool.execute_async({
        {"expression", "1 << 8"}
    });
    checkResult(r7, "256", "1 << 8");

    auto r8 = co_await tool.execute_async({
        {"expression", "1024 >> 3"}
    });
    checkResult(r8, "128", "1024 >> 3");

    auto r9 = co_await tool.execute_async({
        {"expression", "~0"}
    });
    checkResult(r9, "-1", "~0");

    co_return;
}

// 7. 比较、逻辑与三元运算
static asio::awaitable<void> test_logic_and_ternary(std::weak_ptr<agentxx::agent::AgentContext> ctx
) {
    auto tool = agentxx::tools::MathCalculateTool{ctx};

    auto r1 = co_await tool.execute_async({
        {"expression", "5 > 3"}
    });
    checkResult(r1, "1", "5 > 3");

    auto r2 = co_await tool.execute_async({
        {"expression", "5 == 5"}
    });
    checkResult(r2, "1", "5 == 5");

    auto r3 = co_await tool.execute_async({
        {"expression", "5 != 3"}
    });
    checkResult(r3, "1", "5 != 3");

    auto r4 = co_await tool.execute_async({
        {"expression", "1 && 0"}
    });
    checkResult(r4, "0", "1 && 0");

    auto r5 = co_await tool.execute_async({
        {"expression", "1 || 0"}
    });
    checkResult(r5, "1", "1 || 0");

    auto r6 = co_await tool.execute_async({
        {"expression", "!0"}
    });
    checkResult(r6, "1", "!0");

    auto r7 = co_await tool.execute_async({
        {"expression", "10 > 5 ? 100 : 200"}
    });
    checkResult(r7, "100", "10 > 5 ? 100 : 200");

    auto r8 = co_await tool.execute_async({
        {"expression", "3 > 5 ? 100 : 200"}
    });
    checkResult(r8, "200", "3 > 5 ? 100 : 200");

    co_return;
}

// 8. 精度格式化与特殊值
static asio::awaitable<void>
    test_precision_and_special_values(std::weak_ptr<agentxx::agent::AgentContext> ctx) {
    auto tool = agentxx::tools::MathCalculateTool{ctx};

    auto r1 = co_await tool.execute_async({
        {"expression", "1 / 3"},
        {"precision",  4      }
    });
    checkResult(r1, "0.3333", "1 / 3 with precision=4");

    auto r2 = co_await tool.execute_async({
        {"expression", "pi"},
        {"precision",  6   }
    });
    checkResult(r2, "3.141593", "pi with precision=6");

    auto r3 = co_await tool.execute_async({
        {"expression", "inf"}
    });
    checkResult(r3, "Infinity", "inf");

    auto r4 = co_await tool.execute_async({
        {"expression", "-inf"}
    });
    checkResult(r4, "-Infinity", "-inf");

    auto r5 = co_await tool.execute_async({
        {"expression", "nan"}
    });
    checkResult(r5, "NaN", "nan");

    co_return;
}

// 9. 错误处理
static asio::awaitable<void> test_error_handling(std::weak_ptr<agentxx::agent::AgentContext> ctx) {
    auto tool = agentxx::tools::MathCalculateTool{ctx};

    // 表达式为空
    auto r1 = co_await tool.execute_async({
        {"expression", ""}
    });
    checkContains(r1, "empty", "empty expression");

    // 除以零
    auto r2 = co_await tool.execute_async({
        {"expression", "10 / 0"}
    });
    checkContains(r2, "Division by zero", "division by zero");

    // 模零
    auto r3 = co_await tool.execute_async({
        {"expression", "10 % 0"}
    });
    checkContains(r3, "Modulo by zero", "modulo by zero");

    // 未闭合括号
    auto r4 = co_await tool.execute_async({
        {"expression", "(2 + 3 * 4"}
    });
    checkContains(r4, "Unclosed", "unclosed parenthesis");

    // 未知函数
    auto r5 = co_await tool.execute_async({
        {"expression", "unknown_fn(123)"}
    });
    checkContains(r5, "Unknown function", "unknown function");

    // 负数开方 (定义域错误)
    auto r6 = co_await tool.execute_async({
        {"expression", "sqrt(-4)"}
    });
    checkContains(r6, "Domain error", "sqrt of negative");

    // 负数阶乘
    auto r7 = co_await tool.execute_async({
        {"expression", "(-3)!"}
    });
    checkContains(r7, "Factorial", "negative factorial");

    // 非法字符
    auto r8 = co_await tool.execute_async({
        {"expression", "2 @ 3"}
    });
    checkContains(r8, "Unexpected character", "unexpected character");

    co_return;
}

// 10. 插件动态加载与端到端测试
static std::string findMathPluginPath() {
    namespace fs = std::filesystem;
    std::error_code       ec;
    std::vector<fs::path> candidates;
#if XX_IS_WIN_D
    wchar_t buf[MAX_PATH];
    if (::GetModuleFileNameW(nullptr, buf, MAX_PATH) > 0) {
        candidates.push_back(fs::path(buf).parent_path() / "plugins" / "agentxx_math");
    }
#else
    if (auto p = fs::read_symlink("/proc/self/exe", ec); !ec) {
        candidates.push_back(p.parent_path() / "plugins" / "agentxx_math");
    }
#endif
    candidates.push_back(fs::current_path(ec) / "plugins" / "agentxx_math");
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
    return "plugins/agentxx_math";
}

static asio::awaitable<void>
    test_plugin_dynamic_load(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto ctx = agentContext.lock();
    if (!ctx) {
        co_return;
    }
    if (!ctx->toolRegistry) {
        ctx->toolRegistry = std::make_shared<agentxx::plugin::ToolRegistry>();
    }
    if (!ctx->pluginManager) {
        ctx->pluginManager = std::make_shared<agentxx::plugin::PluginManager>(ctx);
        ctx->pluginManager->setIoExecutor(co_await asio::this_coro::executor);
    }

    auto path = findMathPluginPath();
    auto inst = co_await ctx->pluginManager->loadPluginAsync(path);
    if (inst) {
        g_math_passed++;
        TEST_PASS << "PluginManager loaded agentxx_math successfully" << std::endl;

        const char* toolName = "agentxx_math_calculate";
        if (ctx->toolRegistry->contains(toolName)) {
            g_math_passed++;
            TEST_PASS << "ToolRegistry contains agentxx_math_calculate" << std::endl;

            auto tool = ctx->toolRegistry->find(toolName);
            if (tool) {
                auto out = co_await tool->execute_async(neograph::json{
                    {"expression", "sin(pi / 6)"},
                    {"precision",  2            }
                });
                checkResult(out, "0.50", "dynamic plugin execute sin(pi/6)");
            }
        } else {
            g_math_failed++;
            std::cout << "[FAIL] ToolRegistry does not contain agentxx_math_calculate" << std::endl;
        }

        co_await ctx->pluginManager->unloadAsync("agentxx_math");
        if (!ctx->toolRegistry->contains(toolName)) {
            g_math_passed++;
            TEST_PASS << "PluginManager unloaded agentxx_math successfully" << std::endl;
        } else {
            g_math_failed++;
            std::cout << "[FAIL] ToolRegistry still contains tool after unload" << std::endl;
        }
    } else {
        TEST_INFO << "agentxx_math plugin dynamic library not found at: " << path
                  << " (skipping dynamic load test)" << std::endl;
    }
}

asio::awaitable<TestResult>
    run_math_tools_tests(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    g_math_passed = 0;
    g_math_failed = 0;

    std::cout << "--- math_tools ---" << std::endl;

    co_await test_basic_arithmetic(agentContext);
    co_await test_power_and_factorial(agentContext);
    co_await test_implicit_multiplication(agentContext);
    co_await test_constants_and_functions(agentContext);
    co_await test_trigonometric(agentContext);
    co_await test_bitwise_and_bases(agentContext);
    co_await test_logic_and_ternary(agentContext);
    co_await test_precision_and_special_values(agentContext);
    co_await test_error_handling(agentContext);
    co_await test_plugin_dynamic_load(agentContext);

    std::cout << "--- math_tools done: passed=" << g_math_passed << " failed=" << g_math_failed
              << " ---" << std::endl;

    co_return TestResult(g_math_passed, g_math_failed);
}

} // namespace test
} // namespace agentxx
