#include "test_command_tools.h"
#include "agentxx/agent/context.h"
#include "agentxx/tools/execute_command.h"
#include "asio/dispatch.hpp"
#include <iostream>
#include <string>

namespace agentxx {
namespace test {

int g_cmd_passed = 0;
int g_cmd_failed = 0;

asio::awaitable<void> test_linux_command_get_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto def = tool.get_definition();
  if (def.name == "execute_linux_command") {
    g_cmd_passed++;
    TEST_PASS << "ExecuteLinuxCommandTool::get_definition() name correct"
              << std::endl;
  } else {
    std::cout
        << "[FAIL] ExecuteLinuxCommandTool::get_definition() name incorrect"
        << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_linux_command_empty_command(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto args = neograph::json{{"command", ""}};
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos) {
    std::cout
        << "[PASS] ExecuteLinuxCommandTool returns error for empty command"
        << std::endl;
  } else {
    g_cmd_failed++;
    TEST_FAIL << "ExecuteLinuxCommandTool should return error for empty "
                 "command, got: "
              << result << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_linux_command_echo(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto args = neograph::json{{"command", "echo hello_test"}};
  auto result = co_await tool.execute_async(args);
  if (result.find("hello_test") != std::string::npos) {
    g_cmd_passed++;
    TEST_PASS << "ExecuteLinuxCommandTool executes echo command" << std::endl;
  } else {
    g_cmd_failed++;
    TEST_FAIL << "ExecuteLinuxCommandTool echo failed, got: " << result
              << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_linux_command_ls(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto args = neograph::json{{"command", "ls /tmp"}};
  auto result = co_await tool.execute_async(args);
  if (false == result.empty()) {
    g_cmd_passed++;
    TEST_PASS << "ExecuteLinuxCommandTool executes ls command" << std::endl;
  } else {
    g_cmd_failed++;
    TEST_FAIL << "ExecuteLinuxCommandTool ls returned empty result"
              << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_linux_command_pwd(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto args = neograph::json{{"command", "pwd"}};
  auto result = co_await tool.execute_async(args);
  if (result.find("/") != std::string::npos) {
    g_cmd_passed++;
    TEST_PASS << "ExecuteLinuxCommandTool executes pwd command" << std::endl;
  } else {
    g_cmd_failed++;
    TEST_FAIL << "ExecuteLinuxCommandTool pwd failed, got: " << result
              << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_linux_command_whoami(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto args = neograph::json{{"command", "whoami"}};
  auto result = co_await tool.execute_async(args);
  if (false == result.empty()) {
    g_cmd_passed++;
    TEST_PASS << "ExecuteLinuxCommandTool executes whoami command" << std::endl;
  } else {
    g_cmd_failed++;
    TEST_FAIL << "ExecuteLinuxCommandTool whoami returned empty result"
              << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_windows_command_get_definition(
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

asio::awaitable<void> test_windows_command_empty_command(
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

asio::awaitable<void> test_python_command_get_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecutePythonTool{agentContext};
  auto def = tool.get_definition();
  if (def.name == "execute_python_command") {
    g_cmd_passed++;
    TEST_PASS << "ExecutePythonTool::get_definition() name correct"
              << std::endl;
  } else {
    g_cmd_failed++;
    TEST_FAIL << "ExecutePythonTool::get_definition() name incorrect"
              << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_python_command_empty_command(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecutePythonTool{agentContext};
  auto args = neograph::json{{"command", ""}};
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos) {
    g_cmd_passed++;
    TEST_PASS << "ExecutePythonTool returns error for empty command"
              << std::endl;
  } else {
    std::cout
        << "[FAIL] ExecutePythonTool should return error for empty command, "
           "got: "
        << result << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_javascript_command_get_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteJavaScriptTool{agentContext};
  auto def = tool.get_definition();
  if (def.name == "execute_javascript_command") {
    g_cmd_passed++;
    TEST_PASS << "ExecuteJavaScriptTool::get_definition() name correct"
              << std::endl;
  } else {
    g_cmd_failed++;
    TEST_FAIL << "ExecuteJavaScriptTool::get_definition() name incorrect"
              << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_javascript_command_empty_command(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteJavaScriptTool{agentContext};
  auto args = neograph::json{{"command", ""}};
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos) {
    g_cmd_passed++;
    TEST_PASS << "ExecuteJavaScriptTool returns error for empty command"
              << std::endl;
  } else {
    g_cmd_failed++;
    TEST_FAIL << "ExecuteJavaScriptTool should return error for empty "
                 "command, got: "
              << result << std::endl;
  }
  co_return;
}

// ---- get_definition details ----

asio::awaitable<void> test_linux_get_definition_properties(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto def = tool.get_definition();

  XX_TEST_EXPECT_EQ(def.name, "execute_linux_command");
  auto params = def.parameters;

  auto props = params["properties"];
  XX_TEST_EXPECT_TRUE(props.contains("command"));
  XX_TEST_EXPECT_TRUE(props.contains("all_output"));
  XX_TEST_EXPECT_TRUE(props.contains("timeout"));
  XX_TEST_EXPECT_EQ(props["timeout"]["type"].get<std::string>(), "integer");

  auto required = params["required"];
  bool cmdReq = false, timeoutReq = true;
  for (auto it = required.begin(); it != required.end(); ++it) {
    auto val = *it;
    auto name = val.get<std::string>();
    if (name == "command")
      cmdReq = true;
    if (name == "timeout")
      timeoutReq = false;
  }
  XX_TEST_EXPECT_TRUE(cmdReq);
  XX_TEST_EXPECT_TRUE(timeoutReq);

  co_return;
}

asio::awaitable<void> test_windows_get_definition_properties(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteWindowsCommandTool{agentContext};
  auto def = tool.get_definition();

  XX_TEST_EXPECT_EQ(def.name, "execute_windows_command");
  auto props = def.parameters["properties"];
  XX_TEST_EXPECT_TRUE(props.contains("timeout"));
  XX_TEST_EXPECT_EQ(props["timeout"]["type"].get<std::string>(), "integer");

  co_return;
}

asio::awaitable<void> test_python_get_definition_properties(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecutePythonTool{agentContext};
  auto def = tool.get_definition();

  XX_TEST_EXPECT_EQ(def.name, "execute_python_command");
  auto props = def.parameters["properties"];
  XX_TEST_EXPECT_TRUE(props.contains("timeout"));
  XX_TEST_EXPECT_EQ(props["timeout"]["type"].get<std::string>(), "integer");

  co_return;
}

asio::awaitable<void> test_javascript_get_definition_properties(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteJavaScriptTool{agentContext};
  auto def = tool.get_definition();

  XX_TEST_EXPECT_EQ(def.name, "execute_javascript_command");
  auto props = def.parameters["properties"];
  XX_TEST_EXPECT_TRUE(props.contains("timeout"));
  XX_TEST_EXPECT_EQ(props["timeout"]["type"].get<std::string>(), "integer");

  co_return;
}

// ---- timeout ----

asio::awaitable<void> test_linux_timeout_disabled(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto args = neograph::json{
      {"command", "echo timeout_disabled_test"},
      {"timeout", 0},
  };
  auto result = co_await tool.execute_async(args);
  XX_TEST_EXPECT_TRUE(result.find("timeout_disabled_test") !=
                      std::string::npos);
  co_return;
}

asio::awaitable<void> test_linux_timeout_triggers(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto args = neograph::json{
      {"command", "sleep 5"},
      {"timeout", 1},
  };
  auto result = co_await tool.execute_async(args);
  XX_TEST_EXPECT_TRUE(result.find("timed out") != std::string::npos);
  co_return;
}

asio::awaitable<void> test_linux_timeout_partial_output(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto args = neograph::json{
      {"command", "echo 'before_sleep' && sleep 5"},
      {"timeout", 1},
  };
  auto result = co_await tool.execute_async(args);
  XX_TEST_EXPECT_TRUE(result.find("before_sleep") != std::string::npos);
  co_return;
}

asio::awaitable<void> test_linux_timeout_default(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto args = neograph::json{{"command", "echo default_timeout_ok"}};
  auto result = co_await tool.execute_async(args);
  XX_TEST_EXPECT_TRUE(result.find("default_timeout_ok") != std::string::npos);
  co_return;
}

// ---- all_output ----

asio::awaitable<void> test_linux_all_output_false_success(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto args = neograph::json{
      {"command", "echo success_msg"},
      {"all_output", false},
  };
  auto result = co_await tool.execute_async(args);
  XX_TEST_EXPECT_TRUE(result.find("success_msg") == std::string::npos);
  XX_TEST_EXPECT_TRUE(result.find("ExitCode: 0") != std::string::npos);
  co_return;
}

asio::awaitable<void> test_linux_all_output_false_failure(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto args = neograph::json{
      {"command", "echo fail_msg && exit 1"},
      {"all_output", false},
  };
  auto result = co_await tool.execute_async(args);
  XX_TEST_EXPECT_TRUE(result.find("fail_msg") != std::string::npos);
  XX_TEST_EXPECT_TRUE(result.find("ExitCode: 1") != std::string::npos);
  co_return;
}

// ---- stderr & exit code ----

asio::awaitable<void>
test_linux_stderr(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto args = neograph::json{{"command", "echo stderr_test_msg >&2"}};
  auto result = co_await tool.execute_async(args);
  XX_TEST_EXPECT_TRUE(result.find("stderr_test_msg") != std::string::npos);
  co_return;
}

asio::awaitable<void> test_linux_nonzero_exit(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto args = neograph::json{{"command", "exit 42"}};
  auto result = co_await tool.execute_async(args);
  XX_TEST_EXPECT_TRUE(result.find("ExitCode: 42") != std::string::npos);
  co_return;
}

asio::awaitable<void> test_linux_special_chars(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto args = neograph::json{
      {"command", "echo 'hello with spaces and $pecial chars!'"}};
  auto result = co_await tool.execute_async(args);
  XX_TEST_EXPECT_TRUE(result.find("hello with spaces and $pecial chars!") !=
                      std::string::npos);
  co_return;
}

asio::awaitable<void> test_linux_long_output(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteLinuxCommandTool{agentContext};
  auto args = neograph::json{
      {"command", "for i in $(seq 1 100); do echo \"line_$i\"; done"}};
  auto result = co_await tool.execute_async(args);
  XX_TEST_EXPECT_TRUE(result.find("line_1") != std::string::npos);
  XX_TEST_EXPECT_TRUE(result.find("line_100") != std::string::npos);
  co_return;
}

// ---- Python/JavaScript stub timeout param ----

asio::awaitable<void> test_python_timeout_param(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecutePythonTool{agentContext};
  auto args = neograph::json{
      {"command", "test"},
      {"timeout", 30},
  };
  auto result = co_await tool.execute_async(args);
  co_return;
}

asio::awaitable<void> test_javascript_timeout_param(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::ExecuteJavaScriptTool{agentContext};
  auto args = neograph::json{
      {"command", "test"},
      {"timeout", 30},
  };
  auto result = co_await tool.execute_async(args);
  co_return;
}

asio::awaitable<TestResult> run_command_tools_tests(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {

  auto run = [agentContext](auto testFn) -> asio::awaitable<void> {
    try {
      co_await testFn(agentContext);
    } catch (const std::exception &e) {
      g_cmd_failed++;
      TEST_FAIL << "Exception in test: " << e.what() << std::endl;
    }
  };

#if XX_IS_LINUX_D || XX_IS_MACOS_D
  co_await run(test_linux_command_get_definition);
  co_await run(test_linux_get_definition_properties);
  co_await run(test_linux_command_empty_command);
  co_await run(test_linux_command_echo);
  co_await run(test_linux_command_ls);
  co_await run(test_linux_command_pwd);
  co_await run(test_linux_command_whoami);
  co_await run(test_linux_timeout_disabled);
  co_await run(test_linux_timeout_triggers);
  co_await run(test_linux_timeout_partial_output);
  co_await run(test_linux_timeout_default);
  co_await run(test_linux_all_output_false_success);
  co_await run(test_linux_all_output_false_failure);
  co_await run(test_linux_stderr);
  co_await run(test_linux_nonzero_exit);
  co_await run(test_linux_special_chars);
  co_await run(test_linux_long_output);
#endif

#if XX_IS_WINDOWS_D
  co_await run(test_windows_command_get_definition);
  co_await run(test_windows_get_definition_properties);
  co_await run(test_windows_command_empty_command);
#endif

  co_await run(test_python_command_get_definition);
  co_await run(test_python_get_definition_properties);
  co_await run(test_python_command_empty_command);
  co_await run(test_python_timeout_param);
  co_await run(test_javascript_command_get_definition);
  co_await run(test_javascript_get_definition_properties);
  co_await run(test_javascript_command_empty_command);
  co_await run(test_javascript_timeout_param);
  co_return TestResult{g_cmd_passed, g_cmd_failed};
}

} // namespace test
} // namespace agentxx
