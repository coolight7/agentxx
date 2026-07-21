#pragma once

#include "agentxx/util/log.h"
#include "asio/awaitable.hpp"
#include "asio/this_coro.hpp"
#include "fmt/format.h"
#include "neograph/api.h"
#include <iostream>
#include <memory>
#include <optional>
#include <string>

namespace agentxx {
namespace agent {

class AgentIOBase {
public:
  virtual ~AgentIOBase() = default;

  virtual void onToken(const std::string &token, const std::string &kind) = 0;

  virtual void onDisplay(const std::string &level,
                         const std::string &content) = 0;

  virtual asio::awaitable<std::optional<std::string>> getInput() = 0;

  virtual asio::awaitable<bool> promptPermission(const std::string &toolName,
                                                 const std::string &category,
                                                 const std::string &target) = 0;

  virtual void onInterrupt(const std::string &node, const std::string &value,
                           const std::string &handleName) = 0;
};

} // namespace agent
} // namespace agentxx
