#pragma once

#include "agentxx/expand/text_selection_monitor.h"
#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

namespace agentxx {
namespace test {

std::shared_ptr<agentxx::expand::TextSelectionMonitor>
test_text_selection_monitor();

}
} // namespace agentxx
