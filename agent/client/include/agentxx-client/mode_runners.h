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

void runRemoteCli(std::string_view url, std::string_view token, std::string_view model);

void runRemoteTui(
    std::shared_ptr<agent::AgentConfig> config,
    std::string_view                    url,
    std::string_view                    token,
    std::string_view                    model
);

} // namespace client
} // namespace agentxx
