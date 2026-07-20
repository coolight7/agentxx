#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "test_framework.h"
#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_es_passed
#define XX_TEST_FAILED g_es_failed

namespace agentxx {
namespace test {

extern int g_es_passed;
extern int g_es_failed;

struct TestEvent {
  std::string msg;
  int value;
};

struct TestReq {
  std::string question;
};
struct TestResp {
  std::string answer;
};

asio::awaitable<TestResult> run_event_stream_tests();

} // namespace test
} // namespace agentxx
