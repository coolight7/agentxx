#include "agentxx/agent/agent_io.h"

namespace agentxx {
namespace agent {

void AgentIOBase::onToolStart(const std::string& toolName,
                              const std::string& toolCallId,
                              const std::string& arguments) {}

void AgentIOBase::onToolEnd(const std::string& toolName,
                            const std::string& toolCallId,
                            const std::string& result,
                            bool               hasError) {}

} // namespace agent
} // namespace agentxx
