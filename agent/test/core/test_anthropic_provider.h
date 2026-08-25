#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "test_framework.h"

namespace agentxx {
namespace test {

asio::awaitable<TestResult> run_anthropic_provider_tests();

} // namespace test
} // namespace agentxx
