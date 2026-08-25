#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

asio::awaitable<TestResult> run_mcp_tests();

} // namespace test
} // namespace agentxx
