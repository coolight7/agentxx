#pragma once

#include "agentxx/plugin/client_plugin_manager.h"
#include "test_framework.h"
#include <memory>

namespace agentxx {
namespace test {

std::shared_ptr<agentxx::plugin::ClientUiRegistry> makeTestToolRegistry();

TestResult testTuiToolHeader();

} // namespace test
} // namespace agentxx