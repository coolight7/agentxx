#pragma once

#include "agentxx/agent/config.h"
#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "test_framework.h"

namespace agentxx {
namespace test {

asio::awaitable<TestResult> run_openai_provider_tests();

} // namespace test
} // namespace agentxx
