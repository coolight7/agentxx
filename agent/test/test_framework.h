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
  TestResult(int p, int f) : passed(p), failed(f) {}

  TestResult &operator+=(const TestResult &other) {
    passed += other.passed;
    failed += other.failed;
    return *this;
  }

  bool ok() const { return failed == 0; }
};

inline bool g_failFast = false;

inline std::ostream &passStream() { return std::cout << "[PASS] "; }
inline std::ostream &failStream() { return std::cout << "[FAIL] "; }
inline std::ostream &infoStream() { return std::cout << "[INFO] "; }
inline std::ostream &skipStream() { return std::cout << "[SKIP] "; }
inline std::ostream &warnStream() { return std::cerr << "[WARN] "; }

} // namespace test
} // namespace agentxx

#define TEST_PASS agentxx::test::passStream()
#define TEST_FAIL agentxx::test::failStream()
#define TEST_INFO agentxx::test::infoStream()
#define TEST_SKIP agentxx::test::skipStream()
#define TEST_WARN agentxx::test::warnStream()

// 统一断言宏 — 每个文件需在使用前定义 XX_TEST_PASSED / XX_TEST_FAILED 指向本地计数器
// 例: #define XX_TEST_PASSED g_regex_passed / #define XX_TEST_FAILED g_regex_failed

#define XX_TEST_EXPECT_TRUE(expr)                                              \
  do {                                                                         \
    if (expr) {                                                                \
      XX_TEST_PASSED++;                                                        \
    } else {                                                                   \
      XX_TEST_FAILED++;                                                        \
      TEST_FAIL << "expected true at line " << __LINE__ << std::endl;          \
    }                                                                          \
  } while (0)

#define XX_TEST_EXPECT_FALSE(expr)                                             \
  do {                                                                         \
    if (!(expr)) {                                                             \
      XX_TEST_PASSED++;                                                        \
    } else {                                                                   \
      XX_TEST_FAILED++;                                                        \
      TEST_FAIL << "expected false at line " << __LINE__ << std::endl;         \
    }                                                                          \
  } while (0)

#define XX_TEST_EXPECT_EQ(expr, expected)                                      \
  do {                                                                         \
    auto _result = (expr);                                                     \
    auto _expected = (expected);                                               \
    if (_result == _expected) {                                                \
      XX_TEST_PASSED++;                                                        \
    } else {                                                                   \
      XX_TEST_FAILED++;                                                        \
      TEST_FAIL << "line " << __LINE__ << ": expected " << (_expected)         \
                << ", got " << (_result) << std::endl;                         \
    }                                                                          \
  } while (0)

#define XX_TEST_EXPECT_NULLOPT(expr)                                           \
  do {                                                                         \
    if (!(expr).has_value()) {                                                 \
      XX_TEST_PASSED++;                                                        \
    } else {                                                                   \
      XX_TEST_FAILED++;                                                        \
      TEST_FAIL << "line " << __LINE__ << ": expected nullopt" << std::endl;   \
    }                                                                          \
  } while (0)

#define XX_TEST_EXPECT_HAS_VALUE(expr)                                         \
  do {                                                                         \
    if ((expr).has_value()) {                                                  \
      XX_TEST_PASSED++;                                                        \
    } else {                                                                   \
      XX_TEST_FAILED++;                                                        \
      TEST_FAIL << "line " << __LINE__ << ": expected has_value" << std::endl; \
    }                                                                          \
  } while (0)
