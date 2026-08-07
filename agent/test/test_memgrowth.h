#pragma once

#include "test_framework.h"
#include <asio/awaitable.hpp>
#include <neograph/api.h>

namespace agentxx {
namespace test {

asio::awaitable<TestResult> run_memgrowth_tests();

} // namespace test
} // namespace agentxx
