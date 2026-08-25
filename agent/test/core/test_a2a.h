#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>

#include "test_framework.h"

namespace agentxx {
namespace test {

asio::awaitable<TestResult> run_a2a_tests();

} // namespace test
} // namespace agentxx
