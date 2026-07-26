#pragma once

#include "agentxx/agent/agent_io.h"
#include "asio/awaitable.hpp"
#include "neograph/json.h"
#include <iostream>
#include <optional>
#include <string>

class StderrLogSink : public agentxx::util::LogSink {
public:

    void onLog(agentxx::util::LogLevel, const std::string& message) override {
        std::cerr << message << std::endl;
    }
};

class AgentStdIO : public agentxx::agent::AgentIOBase {
private:

    std::shared_ptr<StderrLogSink> logSink_;
    bool                           isThinking_ = false;

public:

    void onDelta(const agentxx::agent::Delta& delta) override;

    void onSync(const agentxx::agent::SyncPayload& payload) override;

    asio::awaitable<std::optional<std::string>> getInput() override;

    asio::awaitable<neograph::json> handleInterrupt(
        const std::string& threadId,
        const std::string& interruptNode,
        const std::string& interruptValue,
        const std::string& interruptArgJson
    ) override;

    AgentStdIO();
    ~AgentStdIO() override;
};
