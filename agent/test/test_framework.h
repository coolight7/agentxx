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
