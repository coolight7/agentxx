#pragma once

#include "agentxx/agent/agent_io.h"
#include "asio/awaitable.hpp"
#include <optional>
#include <string>

class AgentStdIO : public agentxx::agent::AgentIOBase {
private:
  bool isThinking_ = false;

public:
  AgentStdIO() = default;

  void onToken(const std::string &token, const std::string &kind) override;

  void onDisplay(const std::string &level, const std::string &content) override;

  asio::awaitable<std::optional<std::string>> getInput() override;

  asio::awaitable<bool> promptPermission(const std::string &toolName,
                                         const std::string &category,
                                         const std::string &target) override;

  void onInterrupt(const std::string &node, const std::string &value,
                   const std::string &handleName) override;

  void resetTokenState();
};
