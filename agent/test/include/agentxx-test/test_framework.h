#pragma once

#include "agentxx/util/tool_result.h"
#include "utilxx_base/log.h"
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if XX_IS_WIN_D
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif XX_IS_MACOS_D || XX_IS_IOS_D
#include <mach-o/dyld.h>
#endif

namespace agentxx {
namespace test {

/// 工具结果 -> 模型读到的文本 (与执行层同一实现)
///
/// 测试直调插件/工具实现体时拿到的是带状态的结果; 断言"模型看到什么"时用它
/// 转成文本, 断言成败时直接看 `status` (不要解析文本)
///
/// - `args`:
///     - [result] 工具结果
/// - `return` 执行层写进上下文的文本
inline std::string displayText(const agentxx::util::ToolResult& result) {
    return agentxx::util::toolResultTextForModel(result);
}

/// 定位当前可执行文件所在目录 (跨平台)
/// - 用于从任意 cwd 运行测试时, 仍能定位 exe 同目录的 plugins/ 构建产物
/// - Linux/Android: 解析 `/proc/self/exe` 符号链接
/// - macOS/iOS: `_NSGetExecutablePath` (Mach-O 无稳定 procfs 路径)
/// - Windows: `GetModuleFileNameW` (宽字符, 兼容非 ASCII 路径)
///
/// - `return` 成功返回可执行文件所在目录 (已消解 `..`/符号链接, 失败时退回原始父目录);
///   平台查询失败返回 `std::nullopt`
inline std::optional<std::filesystem::path> executableDir() {
    namespace fs = std::filesystem;
#if XX_IS_WIN_D
    wchar_t buf[MAX_PATH];
    DWORD   n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return fs::path(buf).parent_path();
    }
    return std::nullopt;
#elif XX_IS_MACOS_D || XX_IS_IOS_D
    uint32_t          size = 1024;
    std::vector<char> buf(size);
    if (::_NSGetExecutablePath(buf.data(), &size) != 0) {
        // 缓冲区不足: size 已被更新为所需大小, 重试一次
        buf.resize(size);
        if (::_NSGetExecutablePath(buf.data(), &size) != 0) {
            return std::nullopt;
        }
    }
    std::error_code ec;
    fs::path        p{buf.data()};
    if (auto canon = fs::weakly_canonical(p, ec); !ec) {
        return canon.parent_path();
    }
    return p.parent_path();
#else
    std::error_code ec;
    if (auto p = fs::read_symlink("/proc/self/exe", ec); !ec) {
        return p.parent_path();
    }
    return std::nullopt;
#endif
}

/// agent 源码根目录 (含 `lib`/`client`/`plugins`/`test`), 供需要读源码的模块自检
/// (config_keys 扫 `client/src/config_loader.cpp` 的键名, boundaries 查分层规则)
///
/// 解析按顺序取第一个通过目录特征校验的候选:
/// 1. `AGENTXX_TEST_SOURCE_ROOT` —— 构建期写入的绝对路径 (最可靠);
/// 2. `__FILE__` 推导 —— 没有该宏时的回退, 要求编译器收到的是绝对路径; 构建脚本
///    打开的 ccache 归一化 (`CCACHE_BASEDIR`) 会把源文件参数改写成相对路径,
///    此时 `__FILE__` 相对**当前工作目录**, 从构建产物目录运行推不出源码树;
/// 3. 当前工作目录向上回溯 —— 手工在源码树内运行时的备用手段。
///
/// - `return` 找不到返回空路径, 调用方按"跳过"或"环境未就绪"处理, 不误报失败
inline std::filesystem::path sourceRootDir() {
    namespace fs       = std::filesystem;
    auto looksLikeRoot = [](const fs::path& p) {
        std::error_code ec;
        return !p.empty() && fs::is_directory(p / "lib" / "include", ec)
               && fs::is_directory(p / "client", ec) && fs::is_directory(p / "test", ec)
               && fs::is_directory(p / "plugins", ec);
    };
    std::vector<fs::path> candidates;
#ifdef AGENTXX_TEST_SOURCE_ROOT
    candidates.emplace_back(AGENTXX_TEST_SOURCE_ROOT);
#endif
    candidates.emplace_back(std::filesystem::path{__FILE__}.parent_path());
    {
        std::error_code ec;
        candidates.push_back(fs::current_path(ec));
    }
    for (const auto& candidate : candidates) {
        std::error_code ec;
        auto            dir = fs::weakly_canonical(candidate, ec);
        if (ec) {
            dir = candidate;
        }
        for (auto p = dir; !p.empty() && p != p.root_path(); p = p.parent_path()) {
            if (looksLikeRoot(p)) {
                return p;
            }
        }
    }
    return {};
}

/// 源码树内的文件路径; 源码根目录解析失败时返回空路径
inline std::filesystem::path sourceFilePath(std::string_view relative) {
    const auto root = sourceRootDir();
    if (root.empty()) {
        return {};
    }
    return root / std::filesystem::path{std::string{relative}};
}

/// 清除模型 API 凭据类环境变量
///
/// 目的: 本机装有真实 API key 时, 任何"顺带发一次请求"的代码路径都会打线上服务
/// (按量计费)。测试只在本地模拟器/假 provider 上跑, 因此启动时先清掉常见凭据,
/// 使"意外联网"直接失败而不是悄悄花钱。只清凭据与端点覆盖项, 不动 PATH/HOME 等
/// 运行必需变量。
///
/// - `return` 实际清除的变量名列表 (供打印)
inline std::vector<std::string> clearCredentialEnv() {
    static constexpr const char* kNames[] = {
        "OPENAI_API_KEY",
        "OPENAI_BASE_URL",
        "OPENAI_API_BASE",
        "ANTHROPIC_API_KEY",
        "ANTHROPIC_BASE_URL",
        "DEEPSEEK_API_KEY",
        "GEMINI_API_KEY",
        "GOOGLE_API_KEY",
        "MOONSHOT_API_KEY",
        "DASHSCOPE_API_KEY",
        "AZURE_OPENAI_API_KEY",
        "AZURE_OPENAI_ENDPOINT",
        "AGENTXX_API_KEY",
    };
    std::vector<std::string> cleared;
#if XX_IS_WIN_D
    for (const char* name : kNames) {
        if (::GetEnvironmentVariableA(name, nullptr, 0) > 0) {
            ::SetEnvironmentVariableA(name, nullptr);
            cleared.emplace_back(name);
        }
    }
#else
    for (const char* name : kNames) {
        if (::getenv(name) != nullptr) {
            ::unsetenv(name);
            cleared.emplace_back(name);
        }
    }
#endif
    return cleared;
}

/// 敏感值脱敏: 保留首尾少量字符, 中间以 `*` 代替 (长度不足时整体掩掉)
/// - 用于测试日志/断言失败信息中打印可能含凭据的值
inline std::string redactSecret(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    if (value.size() <= 6) {
        return std::string(value.size(), '*');
    }
    return std::string{value.substr(0, 3)} + std::string(value.size() - 5, '*')
           + std::string{value.substr(value.size() - 2)};
}

struct TestResult {
    int passed = 0;
    int failed = 0;

    TestResult() = default;

    TestResult(int p, int f) :
        passed(p),
        failed(f) {}

    TestResult& operator+=(const TestResult& other) {
        passed += other.passed;
        failed += other.failed;
        return *this;
    }

    bool ok() const {
        return failed == 0;
    }
};

inline bool g_failFast = false;

/// 测试输出的日志级别 (由 `--log-level` 设置, 默认 `Info`)
/// - 直接复用库的 `utilxx_base::LogLevel`, 因此测试侧输出与库侧日志
///   (`XX_LOGW` / `XX_LOGE`) 用同一套级别判断, 只有一份含义
/// - 只影响 `[PASS]` / `[INFO]` / `[SKIP]` / `[Warn]` / `[Error]` 输不输出;
///   `[FAIL]` 与断言失败信息**始终输出** (测试失败不能被过滤掉)
inline utilxx_base::LogLevel g_logLevel = utilxx_base::LogLevel::Info;

/// 当前级别下是否要输出 [level] 的输出
/// - `args`:
///     - [level] 该条输出自身的级别 (`Info` / `Warn` / `Error`)
/// - `return` 级别不低于 `g_logLevel` 时为 true
inline bool logEnabled(utilxx_base::LogLevel level) noexcept {
    return static_cast<int>(level) >= static_cast<int>(g_logLevel);
}

/// 丢弃一切写入的流: 被过滤的输出写入它 (保持 `TEST_INFO << ... << std::endl;` 用法不变)
class NullStreamBuffer : public std::streambuf {
protected:

    int overflow(int c) override {
        return c;
    }
};

inline NullStreamBuffer g_nullStreamBuffer{};
inline std::ostream     g_nullStream{&g_nullStreamBuffer};

/// 本测试进程的可执行文件路径 (由 main 填入绝对路径)
/// - 供需要启动子进程的用例使用 (如 MCP stdio 传输把测试程序自身当 server 子进程)
/// - 为空时相关用例只跳过 (不当作失败)
inline std::string g_testExecutablePath;

/// `[PASS]` 成功提示: 按 `Info` 级别过滤
inline std::ostream& passStream() {
    if (false == logEnabled(utilxx_base::LogLevel::Info)) {
        return g_nullStream;
    }
    return std::cout << "[PASS] ";
}

/// `[FAIL]` 断言失败: 始终输出 (不受 `--log-level` 影响)
inline std::ostream& failStream() {
    return std::cout << "[FAIL] ";
}

/// `[INFO]` 过程信息: 按 `Info` 级别过滤
inline std::ostream& infoStream() {
    if (false == logEnabled(utilxx_base::LogLevel::Info)) {
        return g_nullStream;
    }
    return std::cout << "[INFO] ";
}

/// `[SKIP]` 跳过提示: 按 `Info` 级别过滤
inline std::ostream& skipStream() {
    if (false == logEnabled(utilxx_base::LogLevel::Info)) {
        return g_nullStream;
    }
    return std::cout << "[SKIP] ";
}

/// `[Warn]` 测试侧警告: 按 `Warn` 级别过滤 (与库日志前缀统一)
inline std::ostream& warnStream() {
    if (false == logEnabled(utilxx_base::LogLevel::Warn)) {
        return g_nullStream;
    }
    return std::cerr << "[Warn] ";
}

// 可流式输出检测: 某些类型 (std::errc/error_code 等) 无 operator<<,
// 失败输出时以占位符代替, 避免 XX_TEST_EXPECT_EQ 因无法打印而编译失败
template<typename T>
concept Streamable = requires(std::ostream& os, const T& t) { os << t; };

template<typename T>
void printTestValue(std::ostream& os, const T& v) {
    if constexpr (Streamable<T>) {
        os << v;
    } else {
        os << "<unprintable>";
    }
}

} // namespace test
} // namespace agentxx

#define TEST_PASS agentxx::test::passStream()
#define TEST_FAIL agentxx::test::failStream()
#define TEST_INFO agentxx::test::infoStream()
#define TEST_SKIP agentxx::test::skipStream()
#define TEST_WARN agentxx::test::warnStream()

// 统一断言宏 — 断言计数宏覆盖 (XX_TEST_PASSED / XX_TEST_FAILED 指向本模块计数器)
// 应在模块 cpp 文件中定义: 匿名命名空间计数器 + #define 覆盖, 测试函数末尾
// return TestResult{g_xxx_passed, g_xxx_failed}; 头文件仅保留函数声明,
// 不做宏定义/不 extern 导出计数器 (避免跨模块宏泄漏导致计数错乱)
// 例 (cpp 内):
//   namespace { int g_regex_passed = 0; int g_regex_failed = 0; } // namespace
//   #define XX_TEST_PASSED g_regex_passed
//   #define XX_TEST_FAILED g_regex_failed

#define XX_TEST_EXPECT_TRUE(expr)                                           \
    do {                                                                    \
        if (expr) {                                                         \
            XX_TEST_PASSED++;                                               \
        } else {                                                            \
            XX_TEST_FAILED++;                                               \
            TEST_FAIL << "expected true at line " << __LINE__ << std::endl; \
        }                                                                   \
    } while (0)

#define XX_TEST_EXPECT_FALSE(expr)                                           \
    do {                                                                     \
        if (!(expr)) {                                                       \
            XX_TEST_PASSED++;                                                \
        } else {                                                             \
            XX_TEST_FAILED++;                                                \
            TEST_FAIL << "expected false at line " << __LINE__ << std::endl; \
        }                                                                    \
    } while (0)

#define XX_TEST_EXPECT_EQ(expr, expected)                        \
    do {                                                         \
        auto _result   = (expr);                                 \
        auto _expected = (expected);                             \
        if (_result == _expected) {                              \
            XX_TEST_PASSED++;                                    \
        } else {                                                 \
            XX_TEST_FAILED++;                                    \
            TEST_FAIL << "line " << __LINE__ << ": expected ";   \
            agentxx::test::printTestValue(std::cout, _expected); \
            std::cout << ", got ";                               \
            agentxx::test::printTestValue(std::cout, _result);   \
            std::cout << std::endl;                              \
        }                                                        \
    } while (0)

#define XX_TEST_EXPECT_GE(expr, expected)                         \
    do {                                                          \
        auto _result   = (expr);                                  \
        auto _expected = (expected);                              \
        if (_result >= _expected) {                               \
            XX_TEST_PASSED++;                                     \
        } else {                                                  \
            XX_TEST_FAILED++;                                     \
            TEST_FAIL << "line " << __LINE__ << ": expected >= "; \
            agentxx::test::printTestValue(std::cout, _expected);  \
            std::cout << ", got ";                                \
            agentxx::test::printTestValue(std::cout, _result);    \
            std::cout << std::endl;                               \
        }                                                         \
    } while (0)

#define XX_TEST_EXPECT_NULLOPT(expr)                                               \
    do {                                                                           \
        if (!(expr).has_value()) {                                                 \
            XX_TEST_PASSED++;                                                      \
        } else {                                                                   \
            XX_TEST_FAILED++;                                                      \
            TEST_FAIL << "line " << __LINE__ << ": expected nullopt" << std::endl; \
        }                                                                          \
    } while (0)

#define XX_TEST_EXPECT_HAS_VALUE(expr)                                               \
    do {                                                                             \
        if ((expr).has_value()) {                                                    \
            XX_TEST_PASSED++;                                                        \
        } else {                                                                     \
            XX_TEST_FAILED++;                                                        \
            TEST_FAIL << "line " << __LINE__ << ": expected has_value" << std::endl; \
        }                                                                            \
    } while (0)
