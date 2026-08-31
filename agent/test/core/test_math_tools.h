#pragma once

#include "agentxx/agent/context.h"
#include "test_framework.h"
#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

namespace agentxx {
namespace test {

asio::awaitable<TestResult>
    run_math_tools_tests(std::weak_ptr<agentxx::agent::AgentContext> agentContext);

} // namespace test
} // namespace agentxx
