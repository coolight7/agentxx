#include "agentxx/tools/execute_command.h"

#include "agentxx/util/string_util.h"
#include "agentxx/util/util.h"
#include "asio/dispatch.hpp"
#include "asio/experimental/awaitable_operators.hpp"
#include "asio/io_context.hpp"
#include "asio/read.hpp"
#include "asio/readable_pipe.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include <array>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>

#if AGENTXX_ENABLE_BOOST_PROCESS
#include "boost/process.hpp"
#endif

namespace agentxx {
namespace tools {

/// 构造超时错误结果 JSON
/// - 子进程输出可能含引号/反斜杠/换行或非 UTF-8 字节, 必须经 neograph::json 转义,
///   不能直接用 fmt 拼接进手写 JSON (会产生非法 JSON / JSON 注入)
static std::string makeTimeoutResult(int timeout, std::string strout, std::string strerr) {
    if (!(strout.empty() || agentxx::util::autoConvertToUtf8(strout))) {
        strout = "[stdout conversion to utf8 failed]";
    }
    if (!(strerr.empty() || agentxx::util::autoConvertToUtf8(strerr))) {
        strerr = "[stderr conversion to utf8 failed]";
    }
    return neograph::json{
        {"error", fmt::format("Command timed out after {} seconds", timeout)},
        {"stdout", strout},
        {"stderr", strerr},
    }
        .dump();
}

ExecuteLinuxCommandTool::ExecuteLinuxCommandTool(
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("execute_linux_command", in_agentContext, true, false) {}

neograph::ChatTool ExecuteLinuxCommandTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
                        "command",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("command")},
                        },
                    },
                    {
                        "all_output",
                        {
                            {"type", "boolean"},
                            {"description", prompt.getArg("all_output")},
                        },
                    },
                    {
                        "timeout",
                        {
                            {"type", "integer"},
                            {"description", prompt.getArg("timeout")},
                        },
                    },
                },
            }, {"required", neograph::json::array({"command"})},
                       },
    };
}

asio::awaitable<std::string> ExecuteLinuxCommandTool::execute_async(const neograph::json& arguments
) {
    auto command = arguments.value("command", std::string{});
    if (command.empty()) {
        co_return R"({"error":"Arg `command` is empty"})";
    }
    auto all_output = arguments.value("all_output", true);
    auto timeout    = arguments.value("timeout", 60);

#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
    {
        auto                ctx = co_await asio::this_coro::executor;
        asio::readable_pipe outpip{ctx}, errpip{ctx};
        // 创建管道，用于接收子进程的输出
        std::unordered_map<boost::process::environment::key, boost::process::environment::value>
            procEnv;
        // TODO: 缓存环境变量
        for (const auto& kv : boost::process::environment::current()) {
            if (kv.key().string() != "SECRET") {
                procEnv[kv.key()] = kv.value();
            }
        }

#if XX_IS_WIN_D
        auto procExe  = boost::process::environment::find_executable("bash");
        auto procArgs = std::vector<std::string>{"-c", command};
#else
        // setsid 使子进程成为新会话/进程组 leader (pgid == pid),
        // 超时时可经 killpg 整组清理 bash 派生的子孙进程, 避免孤儿进程持有管道
        auto procExe  = boost::process::environment::find_executable("setsid");
        auto procArgs = std::vector<std::string>{"bash", "-c", command};
#endif
        auto proc = boost::process::process{
            ctx,
            procExe,
            procArgs,
            boost::process::process_environment(procEnv),
            boost::process::process_stdio{.out = outpip, .err = errpip},
        };

        std::string              strout, strerr;
        neograph_asio_error_code errCodeStdOut, errCodeStdErr;
        auto                     readStdOutFuture = asio::async_read(
            outpip,
            asio::dynamic_buffer(strout),
            asio::transfer_all(),
            asio::redirect_error(asio::use_awaitable, errCodeStdOut)
        );
        auto readStdErrFuture = asio::async_read(
            errpip,
            asio::dynamic_buffer(strerr),
            asio::transfer_all(),
            asio::redirect_error(asio::use_awaitable, errCodeStdErr)
        );
        // assert(!ec || (ec == asio::error::eof));
        if (timeout > 0) {
            using namespace asio::experimental::awaitable_operators;
            asio::steady_timer timer(ctx, std::chrono::seconds(timeout));
            auto               res = co_await (
                (std::move(readStdOutFuture) && std::move(readStdErrFuture)
                 && proc.async_wait(asio::use_awaitable))
                || timer.async_wait(asio::use_awaitable)
            );
            if (res.index() == 1) {
                boost::system::error_code ec;
                proc.terminate(ec);
                co_return makeTimeoutResult(timeout, strout, strerr);
            }
        } else {
            co_await std::move(readStdOutFuture);
            co_await std::move(readStdErrFuture);
            co_await proc.async_wait();
        }

        const auto         exitCode = proc.exit_code();
        std::ostringstream result;
        result << "## ExitCode: " << exitCode << "\n";
        if (all_output || 0 != exitCode) {
            // failed
            if (strout.empty() || agentxx::util::autoConvertToUtf8(strout)) {
                result << "## StdOut:\n" << strout << "\n";
            } else {
                result << "## StdOut conversion to utf8 failed, truncated\n";
            }
            if (strerr.empty() || agentxx::util::autoConvertToUtf8(strerr)) {
                result << "## StdErr:\n" << strerr << "\n";
            } else {
                result << "## StdErr conversion to utf8 failed, truncated\n";
            }
        }
        co_return result.str();
    }
#else
#if XX_IS_WIN_D
    auto pipe = std::unique_ptr<FILE, decltype(&_pclose)>{_popen(command.c_str(), "r"), _pclose};
#else
    auto pipe = std::unique_ptr<FILE, decltype(&pclose)>{popen(command.c_str(), "r"), pclose};
#endif
    if (!pipe) {
        auto ec = std::error_code{errno, std::system_category()};
        co_return neograph::json{
            {"error", fmt::format("Exec command failed. Error: {}", ec.message())},
        }
            .dump();
    }

    // TODO: 压缩
    // TODO: 太长则暂存到 share_store
    // TODO: 超长时存入文件，不在内存逗留
    std::array<char, 1024> buffer{};
    std::ostringstream     result;
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result << buffer.data();
    }
    co_return result.str();
#endif
}

ExecuteWindowsCommandTool::ExecuteWindowsCommandTool(
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("execute_windows_command", in_agentContext, true, false) {}

neograph::ChatTool ExecuteWindowsCommandTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
                        "command",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("command_process")},
                        },
                    },
#else
                        "command",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("command_popen")},
                        },
                    },
#endif
                    {
                        "all_output",
                        {
                            {"type", "boolean"},
                            {"description", prompt.getArg("all_output")},
                        },
                    },
                    {
                        "timeout",
                        {
                            {"type", "integer"},
                            {"description", prompt.getArg("timeout")},
                        },
                    },
                },
            }, {"required", neograph::json::array({"command"})},
                       },
    };
}

asio::awaitable<std::string>
    ExecuteWindowsCommandTool::execute_async(const neograph::json& arguments) {
    auto command = arguments.value("command", std::string{});
    if (command.empty()) {
        co_return R"({"error":"Arg `command` is empty"})";
    }
    auto all_output = arguments.value("all_output", true);
    auto timeout    = arguments.value("timeout", 60);
// TODO: UNC 路径不受支持。默认值设为 Windows 目录
#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
    {
        auto                ctx = co_await asio::this_coro::executor;
        asio::readable_pipe outpip{ctx}, errpip{ctx};
        // 2. 创建管道，用于接收子进程的输出
        std::unordered_map<boost::process::environment::key, boost::process::environment::value>
            procEnv;
        for (const auto& kv : boost::process::environment::current()) {
            if (kv.key().string() != "SECRET") {
                procEnv[kv.key()] = kv.value();
            }
        }

        auto proc = boost::process::process{
            ctx,
            boost::process::environment::find_executable("cmd.exe"),
            {"/c",          command      },
            boost::process::process_environment(procEnv),
            boost::process::process_stdio{.out = outpip, .err = errpip}
        };

        std::string              strout, strerr;
        neograph_asio_error_code errCodeStdOut, errCodeStdErr;
        auto                     readStdOutFuture = asio::async_read(
            outpip,
            asio::dynamic_buffer(strout),
            asio::transfer_all(),
            asio::redirect_error(asio::use_awaitable, errCodeStdOut)
        );
        auto readStdErrFuture = asio::async_read(
            errpip,
            asio::dynamic_buffer(strerr),
            asio::transfer_all(),
            asio::redirect_error(asio::use_awaitable, errCodeStdErr)
        );
        // assert(!ec || (ec == asio::error::eof));
        if (timeout > 0) {
            using namespace asio::experimental::awaitable_operators;
            asio::steady_timer timer(ctx, std::chrono::seconds(timeout));
            auto               res = co_await (
                (proc.async_wait(asio::use_awaitable) && std::move(readStdOutFuture)
                 && std::move(readStdErrFuture))
                || timer.async_wait(asio::use_awaitable)
            );
            if (res.index() == 1) {
                boost::system::error_code ec;
                proc.terminate(ec);
                // 回收子进程避免僵尸
                co_await proc.async_wait(asio::redirect_error(asio::use_awaitable, ec));
                co_return makeTimeoutResult(timeout, strout, strerr);
            }
        } else {
            co_await proc.async_wait(asio::use_awaitable);
            co_await std::move(readStdOutFuture);
            co_await std::move(readStdErrFuture);
        }

        const auto         exitCode = proc.exit_code();
        std::ostringstream result;
        result << "## ExitCode: " << exitCode << "\n";
        if (all_output || 0 != exitCode) {
            // failed
            if (strout.empty() || agentxx::util::autoConvertToUtf8(strout)) {
                result << "## StdOut:\n" << strout << "\n";
            } else {
                result << "## StdOut conversion to utf8 failed, truncated\n";
            }
            if (strerr.empty() || agentxx::util::autoConvertToUtf8(strerr)) {
                result << "## StdErr:\n" << strerr << "\n";
            } else {
                result << "## StdErr conversion to utf8 failed, truncated\n";
            }
        }
        co_return result.str();
    }
#else
#if XX_IS_WIN_D
    auto pipe = std::unique_ptr<FILE, decltype(&_pclose)>{_popen(command.c_str(), "r"), _pclose};
#else
    auto pipe = std::unique_ptr<FILE, decltype(&pclose)>{popen(command.c_str(), "r"), pclose};
#endif
    if (!pipe) {
        auto ec = std::error_code{errno, std::system_category()};
        co_return neograph::json{
            {"error", fmt::format("Exec command failed. Error: {}", ec.message())},
        }
            .dump();
    }

    std::array<char, 1024> buffer{};
    std::ostringstream     out{};
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        out << buffer.data();
    }
    std::string result = out.str();
    agentxx::util::autoConvertToUtf8(result);
    co_return result;
#endif
}

ExecutePythonTool::ExecutePythonTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext) :
    XXToolBase("execute_python_command", in_agentContext, true, false) {}

neograph::ChatTool ExecutePythonTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
                        "command",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("command")},
                        },
                    },
                    {
                        "timeout",
                        {
                            {"type", "integer"},
                            {"description", prompt.getArg("timeout")},
                        },
                    },
                },
            }, {"required", neograph::json::array({"command"})},
                       },
    };
}

asio::awaitable<std::string> ExecutePythonTool::execute_async(const neograph::json& arguments) {
    auto command = arguments.value("command", std::string{});
    if (command.empty()) {
        co_return R"({"error":"Arg `command` is empty"})";
    }
    // 尚未实现: 必须返回明确错误, 不能返回空串让 LLM 误以为执行成功
    co_return R"({"error":"execute_python_command is not implemented"})";
}

ExecuteJavaScriptTool::ExecuteJavaScriptTool(
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("execute_javascript_command", in_agentContext, true, false) {}

neograph::ChatTool ExecuteJavaScriptTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
                        "command",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("command")},
                        },
                    },
                    {
                        "timeout",
                        {
                            {"type", "integer"},
                            {"description", prompt.getArg("timeout")},
                        },
                    },
                },
            }, {"required", neograph::json::array({"command"})},
                       },
    };
}

asio::awaitable<std::string> ExecuteJavaScriptTool::execute_async(const neograph::json& arguments) {
    auto command = arguments.value("command", std::string{});
    if (command.empty()) {
        co_return R"({"error":"Arg `command` is empty"})";
    }
    // 尚未实现: 必须返回明确错误, 不能返回空串让 LLM 误以为执行成功
    co_return R"({"error":"execute_javascript_command is not implemented"})";
}

} // namespace tools
} // namespace agentxx
