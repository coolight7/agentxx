#pragma once

#include <string>

namespace agentxx {

namespace util {

[[nodiscard]] std::string getSystemName();

[[nodiscard]] bool isRunningInWSL();

}; // namespace util
}; // namespace agentxx