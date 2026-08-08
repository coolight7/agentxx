#include "agentxx/tools/execute_command.h"

#include "agentxx/agent/context.h"
#include "agentxx/util/async_offload.h"
#include "agentxx/util/string_util.h"
#include "agentxx/util/util.h"
#include "asio/as_tuple.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/dispatch.hpp"
#include "asio/experimental/awaitable_operators.hpp"
#include "asio/io_context.hpp"
#include "asio/read.hpp"
#include "asio/readable_pipe.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "neograph/graph/cancel.h"
#include <array>
#include <atomic>
#include <csignal>
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

#if defined(BOOST_PROCESS_V2_PROCESS_HPP)

/// RAII: 析构时置位 done, 通知取消 watcher 退出循环, 避免其继续引用已析构的 process
struct ProcCancelGuard {
    std::shared_ptr<std::atomic<bool>> done;
    ~ProcCancelGuard() {
        if (done) {
            done->store(true, std::memory_order_release);
        }
    }
};

/// 启动会话取消 watcher: 轮询 CancelToken, 会话取消时终止子进程, 避免孤儿进程继续运行
/// - Linux: 子进程经 setsid 启动 (pgid == pid), `kill(-pid)` 可整组清理 bash 派生的子孙进程
/// - Windows: proc.terminate()
/// - 注意: watcher 只终止、不调用 async_wait —— boost.process v2 的 async_wait op 内部
///   引用共享的 exit_status_ 成员, 与主协程并发 wait 会产生数据竞争; 子进程回收由主协程
///   负责。若主协程自身被 asio 取消 (异常路径) 未能 wait, 残留僵尸进程在 agent 进程
///   退出时由系统回收 (比孤儿进程继续运行危害小)
/// - 返回 RAII guard: execute_async 栈展开 (含异常路径) 时置位 done, watcher 退出
static ProcCancelGuard startProcCancelWatcher(
    asio::any_io_executor                                    ctx,
    boost::process::process&                                 proc,
    const std::shared_ptr<neograph::graph::CancelToken>&     cancelToken
) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    if (cancelToken && false == cancelToken->is_cancelled()) {
        asio::co_spawn(
            ctx,
            [&proc, cancelToken, done]() -> asio::awaitable<void> {
                asio::steady_timer timer(co_await asio::this_coro::executor);
                while (false == done->load(std::memory_order_acquire)
                       && false == cancelToken->is_cancelled()) {
                    timer.expires_after(std::chrono::milliseconds(20));
                    auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                    if (ec) {
                        co_return;
                    }
                }
                if (false == done->load(std::memory_order_acquire)) {
                    // 会话取消: 终止子进程
#if XX_IS_WIN_D
                    neograph_asio_error_code ec;
                    proc.terminate(ec);
#else
                    // setsid 启动, pgid == pid; kill 负 pid 整组清理 (含 bash 派生的子孙进程)
                    ::kill(-static_cast<pid_t>(proc.id()), SIGKILL);
#endif
                }
            },
            asio::detached
        );
    }
    return ProcCancelGuard{std::move(done)};
}

/// 终止子进程及其进程组 (Linux: 整组 SIGKILL; Windows: terminate), 用于超时场景
static void killProcGroup(boost::process::process& proc) {
#if XX_IS_WIN_D
    neograph_asio_error_code ec;
    proc.terminate(ec);
#else
    // setsid 启动, pgid == pid; kill 负 pid 整组清理 bash 派生的子孙进程
    ::kill(-static_cast<pid_t>(proc.id()), SIGKILL);
#endif
}

#endif // BOOST_PROCESS_V2_PROCESS_HPP

/// 构造超时错误结果 JSON
/// - 子进程输出可能含引号/反斜杠/换行或非 UTF-8 字节, 必须经 neograph::json 转义,
///   不能直接用 fmt 拼接进手写 JSON (会产生非法 JSON / JSON 注入)
static std::string makeTimeoutResult(int timeout, std::string strout, std::string strerr) {
    if (false == (strout.empty() || agentxx::util::autoConvertToUtf8(strout))) {
        strout = "[StdOut conversion to utf8 failed, discard]";
    }
    if (false == (strerr.empty() || agentxx::util::autoConvertToUtf8(strerr))) {
        strerr = "[StdErr conversion to utf8 failed, discard]";
    }
    return fmt::format(
        R"_(
## Error
Command timed out after {} seconds. If this command is expected to take longer and is not waiting for interactive input, retry with a larger timeout value.
## StdOut
{}
## StdErr
{}
)_",
        timeout,
        strout,
        strerr
    );
}

ExecuteLinuxCommandTool::ExecuteLinuxCommandTool(
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("agentxx_execute_linux_command", in_agentContext, true, false) {}

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
    // [all_output] 为false时，仅执行失败才返回 stdout和stderr
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
            // stdin 重定向到 null 设备 (Windows: NUL / POSIX: /dev/null),
            // 避免子进程 (如交互式命令) 抢读 agent 进程的终端输入
            boost::process::process_stdio{.in = nullptr, .out = outpip, .err = errpip},
        };

        // 会话取消监听: 取消时终止子进程 (含子孙进程), 避免孤儿进程继续运行
        auto agentCtxPtr = agentContext.lock();
        auto cancelToken = agentxx::tools::getSessionCancelToken(agentCtxPtr, arguments);
        auto cancelGuard = startProcCancelWatcher(ctx, proc, cancelToken);

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
            bool isTimeout = false;
            co_await agentxx::util::asyncWithTimeout<void>(
                [&]() -> asio::awaitable<void> {
                    using namespace asio::experimental::awaitable_operators;
                    co_await (
                        std::move(readStdOutFuture) && std::move(readStdErrFuture)
                        && proc.async_wait(asio::use_awaitable)
                    );
                },
                std::chrono::seconds{timeout},
                [&]() {
                    isTimeout = true;
                }
            );
            if (isTimeout) {
                // 超时: 整组终止 (Linux 连子孙进程一起清理), 避免 bash 派生的进程继续运行
                killProcGroup(proc);
                // 回收子进程避免僵尸
                neograph_asio_error_code ec;
                co_await proc.async_wait(asio::redirect_error(asio::use_awaitable, ec));
                co_return makeTimeoutResult(timeout, strout, strerr);
            }
        } else {
            // 无超时: stdout/stderr/退出 并发等待, 避免串行等待时
            // 子进程写满 stderr 管道而 stdout 未关闭导致的双方互等死锁
            using namespace asio::experimental::awaitable_operators;
            co_await (
                std::move(readStdOutFuture) && std::move(readStdErrFuture)
                && proc.async_wait(asio::use_awaitable)
            );
        }

        const auto         exitCode = proc.exit_code();
        std::ostringstream result;
        result << "## ExitCode: " << exitCode << "\n";
        if (all_output || 0 != exitCode) {
            // failed
            if (strout.empty() || agentxx::util::autoConvertToUtf8(strout)) {
                result << "## StdOut:\n" << strout << "\n";
            } else {
                result << "## StdOut conversion to utf8 failed, discard\n";
            }
            if (strerr.empty() || agentxx::util::autoConvertToUtf8(strerr)) {
                result << "## StdErr:\n" << strerr << "\n";
            } else {
                result << "## StdErr conversion to utf8 failed, discard\n";
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
    XXToolBase("agentxx_execute_windows_command", in_agentContext, true, false) {}

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
    // [all_output] 为false时，仅执行失败才返回 stdout和stderr
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
            // stdin 重定向到 null 设备, 避免子进程抢读 agent 进程的终端输入
            boost::process::process_stdio{.in = nullptr, .out = outpip, .err = errpip}
        };

        // 会话取消监听: 取消时终止子进程, 避免孤儿进程继续运行
        auto agentCtxPtr = agentContext.lock();
        auto cancelToken = agentxx::tools::getSessionCancelToken(agentCtxPtr, arguments);
        auto cancelGuard = startProcCancelWatcher(ctx, proc, cancelToken);

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
            bool isTimeout = false;
            co_await agentxx::util::asyncWithTimeout<void>(
                [&]() -> asio::awaitable<void> {
                    using namespace asio::experimental::awaitable_operators;
                    co_await (
                        std::move(readStdOutFuture) && std::move(readStdErrFuture)
                        && proc.async_wait(asio::use_awaitable)
                    );
                },
                std::chrono::seconds{timeout},
                [&]() {
                    isTimeout = true;
                }
            );
            if (isTimeout) {
                killProcGroup(proc);
                // 回收子进程避免僵尸
                neograph_asio_error_code ec;
                co_await proc.async_wait(asio::redirect_error(asio::use_awaitable, ec));
                co_return makeTimeoutResult(timeout, strout, strerr);
            }
        } else {
            // 无超时: stdout/stderr/退出 并发等待, 避免串行等待时
            // 子进程写满 stderr 管道而 stdout 未关闭导致的双方互等死锁
            using namespace asio::experimental::awaitable_operators;
            co_await (
                std::move(readStdOutFuture) && std::move(readStdErrFuture)
                && proc.async_wait(asio::use_awaitable)
            );
        }

        const auto         exitCode = proc.exit_code();
        std::ostringstream result;
        result << "## ExitCode: " << exitCode << "\n";
        if (all_output || 0 != exitCode) {
            // failed
            if (strout.empty() || agentxx::util::autoConvertToUtf8(strout)) {
                result << "## StdOut:\n" << strout << "\n";
            } else {
                result << "## StdOut conversion to utf8 failed, discard\n";
            }
            if (strerr.empty() || agentxx::util::autoConvertToUtf8(strerr)) {
                result << "## StdErr:\n" << strerr << "\n";
            } else {
                result << "## StdErr conversion to utf8 failed, discard\n";
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
    XXToolBase("agentxx_execute_python_command", in_agentContext, true, false) {}

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
    XXToolBase("agentxx_execute_javascript_command", in_agentContext, true, false) {}

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
