#pragma once

#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config.h"
#include <memory>
#include <string>

namespace agentxx {
namespace client {

void runLocalCliUnified(std::shared_ptr<agent::CodeAgent> agent);

void runLocalTuiUnified(
    std::shared_ptr<agent::CodeAgent>   agent,
    std::shared_ptr<agent::AgentConfig> config
);

void runRemoteCli(std::string_view url, std::string_view token, std::string_view model);

void runRemoteTui(
    std::shared_ptr<agent::AgentConfig> config,
    std::string_view                    url,
    std::string_view                    token,
    std::string_view                    model
);

} // namespace client
} // namespace agentxx
