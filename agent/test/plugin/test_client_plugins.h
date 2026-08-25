#pragma once

#include "agentxx/agent/context.h"
#include <asio/awaitable.hpp>
#include <string>

#include "test_framework.h"

namespace agentxx {
namespace test {

asio::awaitable<TestResult> run_client_plugin_tests();

} // namespace test
} // namespace agentxx
