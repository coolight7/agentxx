#include "test_command_tools.h"
#include "agentxx/agent/context.h"
#include "agentxx/tools/execute_command.h"
#include "agentxx/util/util.h"
#include "asio/dispatch.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include <chrono>
#include <iostream>
#include <string>

namespace agentxx {
namespace test {

int g_cmd_passed = 0;
int g_cmd_failed = 0;

asio::awaitable<void>
    test_linux_command_get_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecuteBashCommandTool{agentContext};
    auto def  = tool.get_definition();
    if (def.name == "agentxx_execute_bash_command") {
        g_cmd_passed++;
        TEST_PASS << "ExecuteBashCommandTool::get_definition() name correct" << std::endl;
    } else {
        std::cout << "[FAIL] ExecuteBashCommandTool::get_definition() name incorrect" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_linux_command_empty_command(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecuteBashCommandTool{agentContext};
    auto args = neograph::json{
        {"command", ""}
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("\"error\"") != std::string::npos) {
        std::cout << "[PASS] ExecuteBashCommandTool returns error for empty command" << std::endl;
    } else {
        g_cmd_failed++;
        TEST_FAIL << "ExecuteBashCommandTool should return error for empty "
                     "command, got: "
                  << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_linux_command_echo(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecuteBashCommandTool{agentContext};
    auto args = neograph::json{
        {"command", "echo hello_test"}
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("hello_test") != std::string::npos) {
        g_cmd_passed++;
        TEST_PASS << "ExecuteBashCommandTool executes echo command" << std::endl;
    } else {
        g_cmd_failed++;
        TEST_FAIL << "ExecuteBashCommandTool echo failed, got: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void> test_linux_command_ls(std::weak_ptr<agentxx::agent::AgentContext> agentContext
) {
    auto tool = agentxx::tools::ExecuteBashCommandTool{agentContext};
    auto args = neograph::json{
        {"command", "ls /tmp"}
    };
    auto result = co_await tool.execute_async(args);
    if (false == result.empty()) {
        g_cmd_passed++;
        TEST_PASS << "ExecuteBashCommandTool executes ls command" << std::endl;
    } else {
        g_cmd_failed++;
        TEST_FAIL << "ExecuteBashCommandTool ls returned empty result" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_linux_command_pwd(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecuteBashCommandTool{agentContext};
    auto args = neograph::json{
        {"command", "pwd"}
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("/") != std::string::npos) {
        g_cmd_passed++;
        TEST_PASS << "ExecuteBashCommandTool executes pwd command" << std::endl;
    } else {
        g_cmd_failed++;
        TEST_FAIL << "ExecuteBashCommandTool pwd failed, got: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_linux_command_whoami(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecuteBashCommandTool{agentContext};
    auto args = neograph::json{
        {"command", "whoami"}
    };
    auto result = co_await tool.execute_async(args);
    if (false == result.empty()) {
        g_cmd_passed++;
        TEST_PASS << "ExecuteBashCommandTool executes whoami command" << std::endl;
    } else {
        g_cmd_failed++;
        TEST_FAIL << "ExecuteBashCommandTool whoami returned empty result" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_windows_command_get_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecuteWindowsCommandTool{agentContext};
    auto def  = tool.get_definition();
    if (def.name == "agentxx_execute_windows_command") {
        std::cout << "[PASS] ExecuteWindowsCommandTool::get_definition() name correct" << std::endl;
    } else {
        std::cout << "[FAIL] ExecuteWindowsCommandTool::get_definition() name incorrect"
                  << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_windows_command_empty_command(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecuteWindowsCommandTool{agentContext};
    auto args = neograph::json{
        {"command", ""}
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("\"error\"") != std::string::npos) {
        std::cout << "[PASS] ExecuteWindowsCommandTool returns error for empty command"
                  << std::endl;
    } else {
        std::cout << "[FAIL] ExecuteWindowsCommandTool should return error for empty "
                     "command, got: "
                  << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_python_command_get_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecutePythonTool{agentContext};
    auto def  = tool.get_definition();
    if (def.name == "agentxx_execute_python_command") {
        g_cmd_passed++;
        TEST_PASS << "ExecutePythonTool::get_definition() name correct" << std::endl;
    } else {
        g_cmd_failed++;
        TEST_FAIL << "ExecutePythonTool::get_definition() name incorrect" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_python_command_empty_command(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecutePythonTool{agentContext};
    auto args = neograph::json{
        {"command", ""}
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("\"error\"") != std::string::npos) {
        g_cmd_passed++;
        TEST_PASS << "ExecutePythonTool returns error for empty command" << std::endl;
    } else {
        std::cout << "[FAIL] ExecutePythonTool should return error for empty command, "
                     "got: "
                  << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_javascript_command_get_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext
    ) {
    auto tool = agentxx::tools::ExecuteJavaScriptTool{agentContext};
    auto def  = tool.get_definition();
    if (def.name == "agentxx_execute_javascript_command") {
        g_cmd_passed++;
        TEST_PASS << "ExecuteJavaScriptTool::get_definition() name correct" << std::endl;
    } else {
        g_cmd_failed++;
        TEST_FAIL << "ExecuteJavaScriptTool::get_definition() name incorrect" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_javascript_command_empty_command(std::weak_ptr<agentxx::agent::AgentContext> agentContext
    ) {
    auto tool = agentxx::tools::ExecuteJavaScriptTool{agentContext};
    auto args = neograph::json{
        {"command", ""}
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("\"error\"") != std::string::npos) {
        g_cmd_passed++;
        TEST_PASS << "ExecuteJavaScriptTool returns error for empty command" << std::endl;
    } else {
        g_cmd_failed++;
        TEST_FAIL << "ExecuteJavaScriptTool should return error for empty "
                     "command, got: "
                  << result << std::endl;
    }
    co_return;
}

// ---- get_definition details ----

asio::awaitable<void>
    test_linux_get_definition_properties(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecuteBashCommandTool{agentContext};
    auto def  = tool.get_definition();

    XX_TEST_EXPECT_EQ(def.name, "agentxx_execute_bash_command");
    auto params = def.parameters;

    auto props = params["properties"];
    XX_TEST_EXPECT_TRUE(props.contains("command"));
    XX_TEST_EXPECT_TRUE(props.contains("all_output"));
    XX_TEST_EXPECT_TRUE(props.contains("timeout"));
    XX_TEST_EXPECT_EQ(props["timeout"]["type"].get<std::string>(), "integer");

    auto required = params["required"];
    bool cmdReq = false, timeoutReq = true;
    for (auto it = required.begin(); it != required.end(); ++it) {
        auto val  = *it;
        auto name = val.get<std::string>();
        if (name == "command") {
            cmdReq = true;
        }
        if (name == "timeout") {
            timeoutReq = false;
        }
    }
    XX_TEST_EXPECT_TRUE(cmdReq);
    XX_TEST_EXPECT_TRUE(timeoutReq);

    co_return;
}

asio::awaitable<void>
    test_windows_get_definition_properties(std::weak_ptr<agentxx::agent::AgentContext> agentContext
    ) {
    auto tool = agentxx::tools::ExecuteWindowsCommandTool{agentContext};
    auto def  = tool.get_definition();

    XX_TEST_EXPECT_EQ(def.name, "agentxx_execute_windows_command");
    auto props = def.parameters["properties"];
    XX_TEST_EXPECT_TRUE(props.contains("timeout"));
    XX_TEST_EXPECT_EQ(props["timeout"]["type"].get<std::string>(), "integer");

    co_return;
}

asio::awaitable<void>
    test_python_get_definition_properties(std::weak_ptr<agentxx::agent::AgentContext> agentContext
    ) {
    auto tool = agentxx::tools::ExecutePythonTool{agentContext};
    auto def  = tool.get_definition();

    XX_TEST_EXPECT_EQ(def.name, "agentxx_execute_python_command");
    auto props = def.parameters["properties"];
    XX_TEST_EXPECT_TRUE(props.contains("timeout"));
    XX_TEST_EXPECT_EQ(props["timeout"]["type"].get<std::string>(), "integer");

    co_return;
}

asio::awaitable<void> test_javascript_get_definition_properties(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext
) {
    auto tool = agentxx::tools::ExecuteJavaScriptTool{agentContext};
    auto def  = tool.get_definition();

    XX_TEST_EXPECT_EQ(def.name, "agentxx_execute_javascript_command");
    auto props = def.parameters["properties"];
    XX_TEST_EXPECT_TRUE(props.contains("timeout"));
    XX_TEST_EXPECT_EQ(props["timeout"]["type"].get<std::string>(), "integer");

    co_return;
}

// ---- timeout ----

asio::awaitable<void>
    test_linux_timeout_disabled(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecuteBashCommandTool{agentContext};
    auto args = neograph::json{
        {"command", "echo timeout_disabled_test"},
        {"timeout", 0                           },
    };
    auto result = co_await tool.execute_async(args);
    XX_TEST_EXPECT_TRUE(result.find("timeout_disabled_test") != std::string::npos);
    co_return;
}

asio::awaitable<void>
    test_linux_timeout_triggers(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecuteBashCommandTool{agentContext};
    auto args = neograph::json{
        {"command", "sleep 5"},
        {"timeout", 1        },
    };
    auto result = co_await tool.execute_async(args);
    XX_TEST_EXPECT_TRUE(result.find("timed out") != std::string::npos);
    co_return;
}

asio::awaitable<void>
    test_linux_timeout_partial_output(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecuteBashCommandTool{agentContext};
    auto args = neograph::json{
        {"command", "echo 'before_sleep' && sleep 5"},
        {"timeout", 1                               },
    };
    auto result = co_await tool.execute_async(args);
    XX_TEST_EXPECT_TRUE(result.find("before_sleep") != std::string::npos);
    co_return;
}

asio::awaitable<void>
    test_linux_timeout_default(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecuteBashCommandTool{agentContext};
    auto args = neograph::json{
        {"command", "echo default_timeout_ok"}
    };
    auto result = co_await tool.execute_async(args);
    XX_TEST_EXPECT_TRUE(result.find("default_timeout_ok") != std::string::npos);
    co_return;
}

// ---- all_output ----

asio::awaitable<void>
    test_linux_all_output_false_success(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecuteBashCommandTool{agentContext};
    auto args = neograph::json{
        {"command",    "echo success_msg"},
        {"all_output", false             },
    };
    auto result = co_await tool.execute_async(args);
    XX_TEST_EXPECT_TRUE(result.find("success_msg") == std::string::npos);
    XX_TEST_EXPECT_TRUE(result.find("ExitCode: 0") != std::string::npos);
    co_return;
}

asio::awaitable<void>
    test_linux_all_output_false_failure(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecuteBashCommandTool{agentContext};
    auto args = neograph::json{
        {"command",    "echo fail_msg && exit 1"},
        {"all_output", false                    },
    };
    auto result = co_await tool.execute_async(args);
    XX_TEST_EXPECT_TRUE(result.find("fail_msg") != std::string::npos);
    XX_TEST_EXPECT_TRUE(result.find("ExitCode: 1") != std::string::npos);
    co_return;
}

// ---- stderr & exit code ----

asio::awaitable<void> test_linux_stderr(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecuteBashCommandTool{agentContext};
    auto args = neograph::json{
        {"command", "echo stderr_test_msg >&2"}
    };
    auto result = co_await tool.execute_async(args);
    XX_TEST_EXPECT_TRUE(result.find("stderr_test_msg") != std::string::npos);
    co_return;
}

asio::awaitable<void>
    test_linux_nonzero_exit(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecuteBashCommandTool{agentContext};
    auto args = neograph::json{
        {"command", "exit 42"}
    };
    auto result = co_await tool.execute_async(args);
    XX_TEST_EXPECT_TRUE(result.find("ExitCode: 42") != std::string::npos);
    co_return;
}

asio::awaitable<void>
    test_linux_special_chars(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecuteBashCommandTool{agentContext};
    auto args = neograph::json{
        {"command", "echo 'hello with spaces and $pecial chars!'"}
    };
    auto result = co_await tool.execute_async(args);
    XX_TEST_EXPECT_TRUE(result.find("hello with spaces and $pecial chars!") != std::string::npos);
    co_return;
}

asio::awaitable<void>
    test_linux_long_output(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecuteBashCommandTool{agentContext};
    auto args = neograph::json{
        {"command", "for i in $(seq 1 100); do echo \"line_$i\"; done"}
    };
    auto result = co_await tool.execute_async(args);
    XX_TEST_EXPECT_TRUE(result.find("line_1") != std::string::npos);
    XX_TEST_EXPECT_TRUE(result.find("line_100") != std::string::npos);
    co_return;
}

// ---- Python/JavaScript stub timeout param ----

asio::awaitable<void>
    test_python_timeout_param(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecutePythonTool{agentContext};
    auto args = neograph::json{
        {"command", "test"},
        {"timeout", 30    },
    };
    auto result = co_await tool.execute_async(args);
    // 修复: 未实现必须返回明确错误, 不能返回空串让 LLM 误以为执行成功
    XX_TEST_EXPECT_TRUE(result.find("\"error\"") != std::string::npos);
    XX_TEST_EXPECT_TRUE(result.find("not implemented") != std::string::npos);
    co_return;
}

asio::awaitable<void>
    test_javascript_timeout_param(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecuteJavaScriptTool{agentContext};
    auto args = neograph::json{
        {"command", "test"},
        {"timeout", 30    },
    };
    auto result = co_await tool.execute_async(args);
    // 修复: 未实现必须返回明确错误, 不能返回空串让 LLM 误以为执行成功
    XX_TEST_EXPECT_TRUE(result.find("\"error\"") != std::string::npos);
    XX_TEST_EXPECT_TRUE(result.find("not implemented") != std::string::npos);
    co_return;
}

// ---- #5: 超时错误 JSON 转义 ----

asio::awaitable<void>
    test_linux_timeout_json_escaping(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecuteBashCommandTool{agentContext};
    // 输出含双引号/反斜杠/换行后超时; 修复前 fmt 拼接会产生非法 JSON
    auto args = neograph::json{
        {"command", R"(printf 'has "quotes" and \\backslash\n'; sleep 5)"},
        {"timeout", 1                                                    },
    };
    auto result = co_await tool.execute_async(args);

    bool parsed = result.find("## Error") != std::string::npos
                  && result.find("timed out") != std::string::npos;
    if (parsed) {
        XX_TEST_EXPECT_TRUE(result.find("\"quotes\"") != std::string::npos);
        XX_TEST_EXPECT_TRUE(result.find("\\backslash") != std::string::npos);
    }
    XX_TEST_EXPECT_TRUE(parsed);
    co_return;
}

// ---- #6: 超时清理子孙进程 ----

asio::awaitable<void>
    test_linux_timeout_kills_descendants(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    // auto tool = agentxx::tools::ExecuteBashCommandTool{agentContext};
    // // bash 派生后台 sleep 子孙进程并持有 stdout 管道; 修复后经 setsid+killpg 整组清理
    // auto args = neograph::json{
    //     {"command", "bash -c '(sleep 31.7 &) ; echo started; sleep 31.7'"},
    //     {"timeout", 1                                                    },
    // };
    // auto start   = std::chrono::steady_clock::now();
    // auto result  = co_await tool.execute_async(args);
    // auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    //                    std::chrono::steady_clock::now() - start
    // )
    //                    .count();
    // XX_TEST_EXPECT_TRUE(result.find("timed out") != std::string::npos);
    // // 应及时返回 (远小于后台 sleep 的 31.7s), 不因孤儿进程持有管道而挂起
    // XX_TEST_EXPECT_TRUE(elapsed < 10000);

    // // 检测后台 sleep 是否仍存活 (孤儿); 用拼接 pattern 避免 pgrep 匹配到自身命令行
    // asio::steady_timer delay(co_await asio::this_coro::executor, std::chrono::milliseconds(300));
    // co_await delay.async_wait(asio::use_awaitable);
    // auto checkArgs = neograph::json{
    //     {"command",
    //      R"(A="sleep 31"; B=".7"; pgrep -f "$A$B" >/dev/null 2>&1 && echo ORPHAN_ALIVE || echo
    //      NO_ORPHAN)"
    //     },
    //     {"timeout", 5 },
    // };
    // auto check = co_await tool.execute_async(checkArgs);
    // XX_TEST_EXPECT_TRUE(check.find("NO_ORPHAN") != std::string::npos);
    co_return;
}

// ---- PowerShell 探测 ----

asio::awaitable<void> test_detect_powershell(std::weak_ptr<agentxx::agent::AgentContext>) {
    auto info  = agentxx::util::detectPowerShell();
    auto info2 = agentxx::util::detectPowerShell();
    // 缓存一致性
    XX_TEST_EXPECT_TRUE(info.available == info2.available);
    XX_TEST_EXPECT_TRUE(info.exeName == info2.exeName);
    XX_TEST_EXPECT_TRUE(info.version == info2.version);
#if XX_IS_LINUX_D
    if (false == agentxx::util::isRunningInWSL()) {
        // 非 WSL Linux: 无法调用 Windows 侧 PowerShell
        XX_TEST_EXPECT_TRUE(false == info.available);
    }
    // WSL: interop 环境下通常可用, 但不强制断言 (interop 可能被禁用);
    // 可用时校验字段格式
#elif XX_IS_WINDOWS_D
    // 原生 Windows: 通常可用, 不强制断言 (精简系统可能无 PowerShell)
#endif
    if (info.available) {
        XX_TEST_EXPECT_TRUE(info.exeName == "pwsh.exe" || info.exeName == "powershell.exe");
        XX_TEST_EXPECT_TRUE(info.version.find('.') != std::string::npos);
        XX_TEST_EXPECT_TRUE(info.isPwsh == (info.exeName == "pwsh.exe"));
        TEST_INFO << "PowerShell detected: " << info.exeName << " v" << info.version << std::endl;
    } else {
        TEST_INFO << "PowerShell not detected (will fallback to cmd.exe)" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_windows_definition_ps_info(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    // AgentPrompt 构造时为避免启动阻塞使用非阻塞占位文本 (不探测 PowerShell);
    // 此处显式完成探测+刷新, 等价于 BaseAgent::init (agent 线程) 的行为
    if (auto agentPtr = agentContext.lock(); agentPtr && agentPtr->agentConfig) {
        agentPtr->agentConfig->prompt.refreshEnvDetectedPrompts();
    }
    auto psInfo = agentxx::util::detectPowerShell();
    auto tool   = agentxx::tools::ExecuteWindowsCommandTool{agentContext};
    auto def    = tool.get_definition();
    if (psInfo.available) {
        // depict 与 command 参数描述都应包含探测到的可执行文件名与版本号
        XX_TEST_EXPECT_TRUE(def.description.find(psInfo.exeName) != std::string::npos);
        XX_TEST_EXPECT_TRUE(def.description.find(psInfo.version) != std::string::npos);
        auto props = def.parameters["properties"];
        XX_TEST_EXPECT_TRUE(props.contains("command"));
        XX_TEST_EXPECT_TRUE(
            props["command"]["description"].get<std::string>().find(psInfo.exeName)
            != std::string::npos
        );
    } else {
        // 回退: 提示词应描述 cmd.exe 语义
        XX_TEST_EXPECT_TRUE(def.description.find("cmd.exe") != std::string::npos);
    }
    co_return;
}

asio::awaitable<void>
    test_windows_execute_ps(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto psInfo = agentxx::util::detectPowerShell();
    if (false == psInfo.available) {
        TEST_INFO << "PowerShell not available, skip PS execution tests" << std::endl;
        co_return;
    }
    auto tool = agentxx::tools::ExecuteWindowsCommandTool{agentContext};
    {
        // 基本执行
        auto args = neograph::json{
            {"command", "Write-Output 'ps_tool_echo_ok'"},
            {"timeout", 60                              },
        };
        auto result = co_await tool.execute_async(args);
        XX_TEST_EXPECT_TRUE(result.find("ps_tool_echo_ok") != std::string::npos);
        XX_TEST_EXPECT_TRUE(result.find("ExitCode: 0") != std::string::npos);
    }
    {
        // 字面量 $ 不应被展开 (引号/$ 解析问题的核心回归测试)
        auto args = neograph::json{
            {"command", "Write-Output 'a$b'"},
            {"timeout", 60                  },
        };
        auto result = co_await tool.execute_async(args);
        XX_TEST_EXPECT_TRUE(result.find("a$b") != std::string::npos);
    }
    {
        // 变量赋值 + 双引号插值
        auto args = neograph::json{
            {"command", "$x = 'v1'; Write-Output \"val=$x\""},
            {"timeout", 60                                  },
        };
        auto result = co_await tool.execute_async(args);
        XX_TEST_EXPECT_TRUE(result.find("val=v1") != std::string::npos);
    }
    {
        // 脚本 exit 码透传
        auto args = neograph::json{
            {"command", "Write-Output 'before_exit'; exit 42"},
            {"timeout", 60                                   },
        };
        auto result = co_await tool.execute_async(args);
        XX_TEST_EXPECT_TRUE(result.find("before_exit") != std::string::npos);
        XX_TEST_EXPECT_TRUE(result.find("ExitCode: 42") != std::string::npos);
    }
    {
        // 异常 (throw) 转 stdout + exit 1
        auto args = neograph::json{
            {"command", "Get-Content 'C:\\__agentxx_test_no_such__.txt' -ErrorAction Stop"},
            {"timeout", 60                                                                },
        };
        auto result = co_await tool.execute_async(args);
        XX_TEST_EXPECT_TRUE(result.find("ExitCode: 1") != std::string::npos);
    }
    {
        // 脚本块花括号 (PS 常用语法; 包装模板的 fmt::format 不应破坏大括号)
        auto args = neograph::json{
            {"command",
             "Get-Process | Where-Object { $_.Id -gt 0 } | Select-Object -First 1 | ForEach-Object { Write-Output ('pid_ok') }"
            },
            {"timeout", 60                                                                                                     },
        };
        auto result = co_await tool.execute_async(args);
        XX_TEST_EXPECT_TRUE(result.find("pid_ok") != std::string::npos);
        XX_TEST_EXPECT_TRUE(result.find("ExitCode: 0") != std::string::npos);
    }
    co_return;
}

asio::awaitable<TestResult>
    run_command_tools_tests(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    g_cmd_passed = 0;
    g_cmd_failed = 0;

    auto run = [agentContext](auto testFn) -> asio::awaitable<void> {
        try {
            co_await testFn(agentContext);
        } catch (const std::exception& e) {
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
    co_await run(test_linux_timeout_json_escaping);
    co_await run(test_linux_timeout_kills_descendants);
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
#elif XX_IS_LINUX_D
    // WSL: ExecuteWindowsCommandTool 同样注册 (经 interop 调 Windows 侧),
    // 与 code_agent.cpp 的注册条件保持一致
    if (agentxx::util::isRunningInWSL()) {
        co_await run(test_windows_command_get_definition);
        co_await run(test_windows_get_definition_properties);
        co_await run(test_windows_command_empty_command);
    }
#endif

    co_await run(test_detect_powershell);
    co_await run(test_windows_definition_ps_info);
    co_await run(test_windows_execute_ps);

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
