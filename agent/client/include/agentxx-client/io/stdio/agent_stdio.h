#pragma once

#include "agentxx/agent/agent_io.h"
#include "asio/awaitable.hpp"
#include "neograph/json.h"
#include <optional>
#include <string>

class AgentStdIO : public agentxx::agent::AgentIOBase {
private:

    bool isThinking_ = false;

public:

    AgentStdIO() = default;

    void onDelta(const agentxx::agent::Delta& delta) override;

    void onSync(const agentxx::agent::SyncPayload& payload) override;

    asio::awaitable<std::optional<std::string>> getInput() override;

    asio::awaitable<neograph::json> handleInterrupt(
        const std::string& threadId,
        const std::string& interruptNode,
        const std::string& interruptValue,
        const std::string& interruptArgJson
    ) override;
};
