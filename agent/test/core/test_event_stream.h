#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "test_framework.h"

namespace agentxx {
namespace test {

struct TestEvent {
    std::string msg;
    int         value;
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
