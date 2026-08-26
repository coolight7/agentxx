#include "test_command_tools.h"
#include <neograph/types.h>
#include "agentxx/agent/context.h"
// 原 lib 内置工具已迁移至 agentxx_execute_command 插件 (同名同行为); 测试
// 直测插件同一实现 (execute_command_impl.h), 保证插件行为与测试覆盖一致
#include "execute_command_impl.h"
#include "agentxx/util/util.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/dispatch.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_cmd_passed = 0;
int g_cmd_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_cmd_passed
#define XX_TEST_FAILED g_cmd_failed
namespace agentxx {
namespace tools {

/// 与插件入口同源: 优先宿主 toolPrompt (AgentPrompt 含环境探测后的动态
/// 描述, 经 refreshEnvDetectedPrompts 刷新), 未配置回退内置默认描述
inline neograph::ChatTool execmdDefinitionOf(
    const std::weak_ptr<agentxx::agent::AgentContext>& ctx,
    const char*                                        name,
    const char*                                        fallbackDepict
) {
    std::string depict = fallbackDepict;
    if (auto p = ctx.lock()) {
        if (p->agentConfig) {
            const auto& tp = p->agentConfig->prompt.toolPrompt;
            if (auto it = tp.find(name); it != tp.end() && !it->second.depict.empty()) {
                depict = it->second.depict;
            }
        }
    }
    neograph::json params = neograph::json{
        {"type", "object"},
        {"properties",
         {
             {"command",
              {
                  {"type", "string"},
                  {"description", depict},
              }},
             {"all_output",
              {{"type", "boolean"},
               {"description",
                "Default `true`. `false`: only return stdout and stderr when the command fails."}}},
             {"timeout", {{"type", "integer"}, {"description", "Default `60` seconds."}}},
         }},
        {"required", neograph::json::array({"command"})},
    };
    return {name, depict, std::move(params)};
}

/// 会话工作目录解析 (原工具经 AgentContext; 与插件 get_work_dir 同源):
/// ctx 未装配时回退进程 cwd, 相对路径用例语义不变
inline std::string testResolvedWorkDir(const std::weak_ptr<agentxx::agent::AgentContext>& ctx) {
    if (auto p = ctx.lock()) {
        if (p->agentConfig) {
            auto dir = p->agentConfig->resolvedWorkDir();
            if (!dir.empty()) {
                return dir;
            }
        }
    }
    return std::filesystem::current_path().generic_string();
}

/// 测试适配: 原工具类的同名薄包装 (execute_async 直调插件实现)
struct ExecuteBashCommandTool {
    std::weak_ptr<agentxx::agent::AgentContext> ctx;
    explicit ExecuteBashCommandTool(std::weak_ptr<agentxx::agent::AgentContext> c)
        : ctx(std::move(c)) {}

    neograph::ChatTool get_definition() const {
        return execmdDefinitionOf(
            ctx,
            "agentxx_execute_bash_command",
            "Execute a shell/bash command and return its output."
        );
    }

    asio::awaitable<std::string> execute_async(const neograph::json& args) const {
#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
        // poll 寄生驱动协程版执行体 (与插件注册路径同一实现)
        co_return co_await agentxx_execmd_plugin::bashExecuteAsync(
            args,
            testResolvedWorkDir(ctx),
            /*isCancelled=*/nullptr
        );
#else
        // popen 回退 (阻塞实现, 测试协程内直调等价于原同步语义)
        co_return agentxx_execmd_plugin::bashExecute(
            args,
            testResolvedWorkDir(ctx),
            /*isCancelled=*/nullptr
        );
#endif
    }
};

struct ExecuteWindowsCommandTool {
    std::weak_ptr<agentxx::agent::AgentContext> ctx;
    explicit ExecuteWindowsCommandTool(std::weak_ptr<agentxx::agent::AgentContext> c)
        : ctx(std::move(c)) {}

    neograph::ChatTool get_definition() const {
        return execmdDefinitionOf(
            ctx,
            "agentxx_execute_windows_command",
            "Execute a Windows command via cmd.exe."
        );
    }

    asio::awaitable<std::string> execute_async(const neograph::json& args) const {
#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
        co_return co_await agentxx_execmd_plugin::windowsExecuteAsync(
            args,
            testResolvedWorkDir(ctx),
            /*isCancelled=*/nullptr
        );
#else
        co_return agentxx_execmd_plugin::windowsExecute(
            args,
            testResolvedWorkDir(ctx),
            /*isCancelled=*/nullptr
        );
#endif
    }
};

} // namespace tools
} // namespace agentxx

namespace agentxx {
namespace test {

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

// ---- 子进程工作目录 (AgentConfig::workDir) ----

/// 构造绑定指定会话工作目录的独立 AgentContext (work_dir 相关测试专用)
static std::shared_ptr<agentxx::agent::AgentContext>
    makeWorkDirContext(const std::string& dir) {
    auto ctx                  = std::make_shared<agentxx::agent::AgentContext>();
    ctx->agentConfig          = std::make_shared<agentxx::agent::AgentConfig>();
    ctx->agentConfig->workDir = dir;
    return ctx;
}

/// 子进程初始工作目录应为会话工作目录 (AgentConfig::workDir):
/// 在 workDir 下创建 marker 文件后执行无路径参数的列目录命令,
/// 输出包含 marker 名即证明子进程 cwd 确为 workDir
/// (避免直接比较路径字符串 —— Windows 盘符大小写/正反斜杠格式差异易误判)
/// - popen 回退编译 (AGENTXX_ENABLE_BOOST_PROCESS 关闭) 无法指定子进程目录,
///   此时跳过断言仅记录信息
asio::awaitable<void>
    test_command_subprocess_workdir(std::weak_ptr<agentxx::agent::AgentContext>) {
#if AGENTXX_ENABLE_BOOST_PROCESS
    namespace fs = std::filesystem;
    auto wd       = (fs::temp_directory_path() / "agentxx_test_cmd_wd").generic_string();
    auto markerNm = std::string{"wd_probe_marker_8f3a.txt"};
    std::error_code ec;
    fs::remove_all(wd, ec);
    fs::create_directories(wd, ec);
    if (ec) {
        g_cmd_failed++;
        TEST_FAIL << "create workdir failed: " << ec.message() << std::endl;
        co_return;
    }
    {
        std::ofstream f((fs::path(wd) / markerNm));
        f << "probe\n";
        f.close();
        if (!f) {
            g_cmd_failed++;
            TEST_FAIL << "write marker failed" << std::endl;
            co_return;
        }
    }

    auto ctx = makeWorkDirContext(wd);
#if XX_IS_WIN_D
    // 无 PowerShell 时回退 cmd.exe 不识别 PS 语法: 与既有 PS 执行测试一致跳过
    auto psInfo = agentxx::util::detectPowerShell();
    if (false == psInfo.available) {
        TEST_INFO << "skip subprocess workdir assert: PowerShell not available" << std::endl;
        fs::remove_all(wd, ec);
        co_return;
    }
    auto tool = agentxx::tools::ExecuteWindowsCommandTool{ctx};
    auto args = neograph::json{
        {"command", "Get-ChildItem -Name"}
    };
#else
    auto tool = agentxx::tools::ExecuteBashCommandTool{ctx};
    auto args = neograph::json{
        {"command", "ls"}
    };
#endif
    auto result = co_await tool.execute_async(args);
    if (result.find(markerNm) != std::string::npos) {
        g_cmd_passed++;
        TEST_PASS << "command subprocess starts in AgentConfig::workDir (" << wd << ")"
                  << std::endl;
    } else {
        g_cmd_failed++;
        TEST_FAIL << "command subprocess cwd should be workDir=" << wd << ", got: " << result
                  << std::endl;
    }
    fs::remove_all(wd, ec);
#else
    TEST_INFO << "skip subprocess workdir test: AGENTXX_ENABLE_BOOST_PROCESS off "
                 "(popen fallback cannot set child cwd)"
              << std::endl;
#endif
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

#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
/// poll 寄生驱动并发性: 两条 sleep 命令并行执行总耗时 ≈ 单条 (而非串行 2 倍)
/// - 寄生 loop 上两个子进程协程交错等待就绪事件, 验证不再"每命令独占线程"
asio::awaitable<void>
    test_command_concurrent_commands(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::ExecuteBashCommandTool{agentContext};
    auto args = neograph::json{
        {"command", "sleep 1 && echo concurrent_done"},
        {"timeout", 10                               },
    };
    auto              ex   = co_await asio::this_coro::executor;
    std::atomic<int>  done{0};
    std::string       r1, r2;
    auto              start = std::chrono::steady_clock::now();
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> {
            r1 = co_await tool.execute_async(args);
            done.fetch_add(1, std::memory_order_release);
        },
        asio::detached
    );
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> {
            r2 = co_await tool.execute_async(args);
            done.fetch_add(1, std::memory_order_release);
        },
        asio::detached
    );
    while (done.load(std::memory_order_acquire) < 2) {
        asio::steady_timer t(ex);
        t.expires_after(std::chrono::milliseconds(20));
        co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start
    )
                       .count();
    XX_TEST_EXPECT_TRUE(r1.find("concurrent_done") != std::string::npos);
    XX_TEST_EXPECT_TRUE(r2.find("concurrent_done") != std::string::npos);
    // 并行 ≈1s; 串行 (每命令独占驱动) ≥2s。留 800ms 调度余量防 CI 抖动误判
    XX_TEST_EXPECT_TRUE(elapsed < 1800);
    TEST_INFO << "two concurrent sleep-1 commands finished in " << elapsed << "ms" << std::endl;
    co_return;
}

/// 会话取消及时性: isCancelled 恒真 → watcher 20ms 轮询到后整组 kill 子进程,
/// 工具应秒级返回而非等满 timeout (寄生驱动下取消传播路径验证)
/// - 环境门控: 依赖 killpg 整组终止生效; WSL 下与 timeout_triggers 同源受限
///   (基线即失败, 见 test_linux_timeout_kills_descendants 注释), 门控跳过
asio::awaitable<void>
    test_command_cancel_promptly(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
#if XX_IS_LINUX_D
    if (agentxx::util::isRunningInWSL()) {
        TEST_INFO << "skip cancel-promptly assert on WSL (process-group kill "
                     "limited, same as timeout_triggers baseline)"
                  << std::endl;
        co_return;
    }
#endif
    auto args = neograph::json{
        {"command", "sleep 30"},
        {"timeout", 60        },
    };
    auto start   = std::chrono::steady_clock::now();
    auto result  = co_await agentxx_execmd_plugin::bashExecuteAsync(
        args,
        agentxx::tools::testResolvedWorkDir(agentContext),
        /*isCancelled=*/[]() { return true; }
    );
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start
    )
                       .count();
    // watcher 20ms 轮询 + kill 后主工作收尾, 应远小于 sleep 的 30s
    XX_TEST_EXPECT_TRUE(elapsed < 10000);
    XX_TEST_EXPECT_TRUE(result.find("ExitCode") != std::string::npos);
    TEST_INFO << "cancelled sleep-30 command returned in " << elapsed << "ms" << std::endl;
    co_return;
}
#endif // BOOST_PROCESS_V2_PROCESS_HPP

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
#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
    // poll 寄生驱动新增语义验证 (并发不串行阻塞 + 取消及时传播)
    co_await run(test_command_concurrent_commands);
    co_await run(test_command_cancel_promptly);
#endif
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

    co_await run(test_command_subprocess_workdir);
    co_await run(test_detect_powershell);
    co_await run(test_windows_definition_ps_info);
    co_await run(test_windows_execute_ps);

    co_return TestResult{g_cmd_passed, g_cmd_failed};
}

} // namespace test
} // namespace agentxx
