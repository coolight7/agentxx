#include "agentxx/agent/config.h"

namespace agentxx {
namespace agent {

bool ModelConfig::isValid() const {
  return !baseUrl.empty() || apiKey != "EMPTY";
}

const ModelConfig &AgentConfig::getSubagentModel() const {
  return subagentModel.has_value() ? subagentModel.value() : model;
}

} // namespace agent
} // namespace agentxx
