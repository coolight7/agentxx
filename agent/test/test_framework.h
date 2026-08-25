#pragma once

#include <cstdint>
#include <iostream>
#include <string>

namespace agentxx {
namespace test {

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

inline std::ostream& passStream() {
    return std::cout << "[PASS] ";
}

inline std::ostream& failStream() {
    return std::cout << "[FAIL] ";
}

inline std::ostream& infoStream() {
    return std::cout << "[INFO] ";
}

inline std::ostream& skipStream() {
    return std::cout << "[SKIP] ";
}

inline std::ostream& warnStream() {
    return std::cerr << "[WARN] ";
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
