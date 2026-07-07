#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/tools/execute_command.h"
#include "asio/dispatch.hpp"
#include <iostream>
#include <string>

namespace agentxx {
namespace test {

inline asio::awaitable<void> test_linux_command_get_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto def = tool.get_definition();
  if (def.name == "execute_linux_command") {
    std::cout << "[PASS] ExecuteLinuxCommandTool::get_definition() name correct"
              << std::endl;
  } else {
    std::cout
        << "[FAIL] ExecuteLinuxCommandTool::get_definition() name incorrect"
        << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_linux_command_empty_command(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto args = neograph::json{{"command", ""}};
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos) {
    std::cout
        << "[PASS] ExecuteLinuxCommandTool returns error for empty command"
        << std::endl;
  } else {
    std::cout << "[FAIL] ExecuteLinuxCommandTool should return error for empty "
                 "command, got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_linux_command_echo(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto args = neograph::json{{"command", "echo hello_test"}};
  auto result = co_await tool.execute_async(args);
  if (result.find("hello_test") != std::string::npos) {
    std::cout << "[PASS] ExecuteLinuxCommandTool executes echo command"
              << std::endl;
  } else {
    std::cout << "[FAIL] ExecuteLinuxCommandTool echo failed, got: " << result
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_linux_command_ls(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto args = neograph::json{{"command", "ls /tmp"}};
  auto result = co_await tool.execute_async(args);
  if (false == result.empty()) {
    std::cout << "[PASS] ExecuteLinuxCommandTool executes ls command"
              << std::endl;
  } else {
    std::cout << "[FAIL] ExecuteLinuxCommandTool ls returned empty result"
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_linux_command_pwd(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto args = neograph::json{{"command", "pwd"}};
  auto result = co_await tool.execute_async(args);
  if (result.find("/") != std::string::npos) {
    std::cout << "[PASS] ExecuteLinuxCommandTool executes pwd command"
              << std::endl;
  } else {
    std::cout << "[FAIL] ExecuteLinuxCommandTool pwd failed, got: " << result
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_linux_command_whoami(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto args = neograph::json{{"command", "whoami"}};
  auto result = co_await tool.execute_async(args);
  if (false == result.empty()) {
    std::cout << "[PASS] ExecuteLinuxCommandTool executes whoami command"
              << std::endl;
  } else {
    std::cout << "[FAIL] ExecuteLinuxCommandTool whoami returned empty result"
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_windows_command_get_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteWindowsCommandTool{agentContext};
  auto def = tool.get_definition();
  if (def.name == "execute_windows_command") {
    std::cout
        << "[PASS] ExecuteWindowsCommandTool::get_definition() name correct"
        << std::endl;
  } else {
    std::cout
        << "[FAIL] ExecuteWindowsCommandTool::get_definition() name incorrect"
        << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_windows_command_empty_command(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteWindowsCommandTool{agentContext};
  auto args = neograph::json{{"command", ""}};
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos) {
    std::cout
        << "[PASS] ExecuteWindowsCommandTool returns error for empty command"
        << std::endl;
  } else {
    std::cout
        << "[FAIL] ExecuteWindowsCommandTool should return error for empty "
           "command, got: "
        << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_python_command_get_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecutePythonTool{agentContext};
  auto def = tool.get_definition();
  if (def.name == "execute_python_command") {
    std::cout << "[PASS] ExecutePythonTool::get_definition() name correct"
              << std::endl;
  } else {
    std::cout << "[FAIL] ExecutePythonTool::get_definition() name incorrect"
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_python_command_empty_command(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecutePythonTool{agentContext};
  auto args = neograph::json{{"command", ""}};
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos) {
    std::cout << "[PASS] ExecutePythonTool returns error for empty command"
              << std::endl;
  } else {
    std::cout
        << "[FAIL] ExecutePythonTool should return error for empty command, "
           "got: "
        << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_javascript_command_get_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteJavaScriptTool{agentContext};
  auto def = tool.get_definition();
  if (def.name == "execute_javascript_command") {
    std::cout << "[PASS] ExecuteJavaScriptTool::get_definition() name correct"
              << std::endl;
  } else {
    std::cout << "[FAIL] ExecuteJavaScriptTool::get_definition() name incorrect"
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_javascript_command_empty_command(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteJavaScriptTool{agentContext};
  auto args = neograph::json{{"command", ""}};
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos) {
    std::cout << "[PASS] ExecuteJavaScriptTool returns error for empty command"
              << std::endl;
  } else {
    std::cout << "[FAIL] ExecuteJavaScriptTool should return error for empty "
                 "command, got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> run_command_tools_tests(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  std::cout << "======= Test: Command Tools =======" << std::endl;

  auto run = [agentContext](auto testFn) -> asio::awaitable<void> {
    try {
      co_await testFn(agentContext);
    } catch (const std::exception &e) {
      std::cout << "[FAIL] Exception in test: " << e.what() << std::endl;
    }
  };

  co_await run(test_linux_command_get_definition);
  co_await run(test_linux_command_empty_command);
  co_await run(test_linux_command_echo);
  co_await run(test_linux_command_ls);
  co_await run(test_linux_command_pwd);
  co_await run(test_linux_command_whoami);
  co_await run(test_windows_command_get_definition);
  co_await run(test_windows_command_empty_command);
  co_await run(test_python_command_get_definition);
  co_await run(test_python_command_empty_command);
  co_await run(test_javascript_command_get_definition);
  co_await run(test_javascript_command_empty_command);
  std::cout << "======= Test Done =======" << std::endl;
}

} // namespace test
} // namespace agentxx