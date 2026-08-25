#pragma once

#include <asio/awaitable.hpp>

#include "test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

asio::awaitable<TestResult> run_cancel_tests();

} // namespace test
} // namespace agentxx

