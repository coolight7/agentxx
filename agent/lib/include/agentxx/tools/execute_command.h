#pragma once

#include "agentxx/tools/tool.h"

namespace agentxx {
namespace tools {

class ExecuteLinuxCommandTool : public XXToolBase {
public:
  ExecuteLinuxCommandTool(
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

  neograph::ChatTool get_definition() const override;

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override;
};

/// windows cmd
/// 支持 WSL
class ExecuteWindowsCommandTool : public XXToolBase {
public:
  ExecuteWindowsCommandTool(
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

  neograph::ChatTool get_definition() const override;

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override;
};

class ExecutePythonTool : public XXToolBase {
public:
  ExecutePythonTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

  neograph::ChatTool get_definition() const override;

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override;
};

class ExecuteJavaScriptTool : public XXToolBase {
public:
  ExecuteJavaScriptTool(
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

  neograph::ChatTool get_definition() const override;

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override;
};

} // namespace tools
} // namespace agentxx
