#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/agent/deepagent.h"
#include <memory>
#include <string>

namespace agentxx {
namespace client {

void runLocalCliUnified(std::shared_ptr<agent::DeepAgent> agent);

void runLocalTuiUnified(
    std::shared_ptr<agent::DeepAgent>   agent,
    std::shared_ptr<agent::AgentConfig> config
);

void runRemoteCli(const std::string& url, const std::string& token, const std::string& model);

void runRemoteTui(
    std::shared_ptr<agent::AgentConfig> config,
    const std::string&                  url,
    const std::string&                  token,
    const std::string&                  model
);

} // namespace client
} // namespace agentxx
