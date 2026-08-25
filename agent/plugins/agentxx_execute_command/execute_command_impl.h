// agentxx_execute_command 插件 —— 工具实现 (纯函数, 不含 C ABI 胶水)
// - 从 libagentxx src/tools/execute_command 拆分: 同名工具同行为
//     agentxx_execute_bash_command / agentxx_execute_windows_command
// - 头文件-only: 插件入口与测试共同包含, 保证插件行为与测试覆盖一致
// - 与原实现的差异点:
//   - 会话工作目录/取消令牌经参数注入 (workDir / isCancelled 回调),
//     由插件入口从宿主接口表取值 (agentxx.agent.config.get_work_dir /
//     agentxx.agent.cancel.is_cancelled), 便于测试直测纯逻辑
//   - 协程经局部 io_context 同步驱动 (插件 execute 为同步 C 回调,
//     运行在宿主线程池, 阻塞调用方线程安全, 与其他内置工具插件同模式)
#pragma once

#include "agentxx/util/async_offload.h"
#include "agentxx/util/log.h"
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
#include <array>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <neograph/json.h>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#if !defined(XX_IS_WIN_D)
#define XX_IS_WIN_D 0
#endif
#if !defined(XX_IS_LINUX_D)
#define XX_IS_LINUX_D 0
#endif

#if defined(AGENTXX_ENABLE_BOOST_PROCESS) && AGENTXX_ENABLE_BOOST_PROCESS
#include "boost/process.hpp"
#endif

namespace agentxx_execmd_plugin {

/// 取消查询回调 (返回 true 表示会话已取消); 测试可传 nullptr 等价无取消支持
using IsCancelledFn = std::function<bool()>;

namespace detail {

#if defined(BOOST_PROCESS_V2_PROCESS_HPP)

/// 子进程初始工作目录: workDir (会话工作目录) 优先; 为空时回退进程 cwd。
/// 始终返回非空串 (兜底 "." 等价于继承当前目录), 使 process_start_dir 可无条件传入
inline std::string subprocessWorkDir(const std::string& workDir) {
    if (!workDir.empty()) {
        return workDir;
    }
    std::error_code ec;
    auto            cwd = std::filesystem::current_path(ec);
    if (!ec && !cwd.empty()) {
        return cwd.generic_string();
    }
    // 兜底: chdir(".") / current_directory="." 等价于继承 agent 进程当前目录
    return ".";
}

/// 会话取消监听协程体 (与主工作经 awaitable_operators 并行运行):
/// - 轮询 isCancelled 回调 (宿主会话取消令牌), 取消时终止子进程
/// - 【关键生命周期语义】动作完成后不立即结束, 而是挂起直至被并行组取消:
///   || 组合下"任一先完成即整体完成并取消其余", 若本协程在 kill 后立刻返回,
///   会在主工作组装结果前把它整体取消 (丢失输出); 挂起让主工作自然收尾,
///   由主工作完成驱动整体终结 —— 同时保证 io.run() 不会被无限轮询的 watcher
///   吊住 (历史 bug: detached watcher + RAII guard 互相死等)
/// - Linux: 子进程经 setsid 启动 (pgid == pid), `kill(-pid)` 可整组清理
/// - 注意: watcher 只终止、不调用 async_wait —— boost.process v2 的
///   async_wait op 内部引用共享的 exit_status_ 成员, 与主协程并发 wait
///   会产生数据竞争; 子进程回收由主协程负责
inline asio::awaitable<void>
    procCancelWatchLoop(boost::process::process& proc, const IsCancelledFn& isCancelled) {
    asio::steady_timer timer(co_await asio::this_coro::executor);
    if (!isCancelled) {
        // 无取消源: 挂起直至被并行组取消 (主工作完成)
        timer.expires_after(std::chrono::hours(24));
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        co_return;
    }
    while (false == isCancelled()) {
        timer.expires_after(std::chrono::milliseconds(20));
        auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        if (ec) {
            co_return; // 被并行组取消: 主工作已完成
        }
    }
    // 会话取消: 终止子进程后挂起, 让主工作自然收尾
#if XX_IS_WIN_D
    neograph_asio_error_code ec;
    proc.terminate(ec);
#else
    // setsid 启动, pgid == pid; kill 负 pid 整组清理 (含 bash 派生的子孙进程)
    ::kill(-static_cast<pid_t>(proc.id()), SIGKILL);
#endif
    timer.expires_after(std::chrono::hours(24));
    co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
}

/// 终止子进程及其进程组 (Linux: 整组 SIGKILL; Windows: terminate), 用于超时场景
inline void killProcGroup(boost::process::process& proc) {
#if XX_IS_WIN_D
    neograph_asio_error_code ec;
    proc.terminate(ec);
#else
    // setsid 启动, pgid == pid; kill 负 pid 整组清理 (含 bash 派生的子孙进程)
    ::kill(-static_cast<pid_t>(proc.id()), SIGKILL);
#endif
}

/// 构造超时错误结果文本 (多行纯文本输出, 非 JSON)
inline std::string makeTimeoutResult(int timeout, std::string strout, std::string strerr) {
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

/// 构造 Windows tool 的子进程启动参数: 可执行文件 + argv 列表
/// - PowerShell 可用 (默认): {exeName, -NoProfile, -NonInteractive, -Command, <包装后的脚本>}
///   脚本包装为 try/catch, 强制 UTF-8 输出并透传脚本退出码;
///   整段脚本作为单个 argv 元素直传, 不经 cmd/sh 解析, 根治引号/$ 转义问题
/// - PowerShell 不可用 (回退): {cmd.exe, /c, command} (cmd 语法, 由提示词引导 LLM)
struct WinProcLaunch {
    std::string              exeName;
    std::vector<std::string> args;
};

inline WinProcLaunch buildWinProcLaunch(std::string_view command) {
    const auto psInfo = agentxx::util::detectPowerShell();
    if (psInfo.available) {
        // -Command 后的多个 argv 元素会被 PowerShell 用空格拼接成一条命令串,
        // 因此必须作为单个元素传入 (脚本内可含换行, PowerShell 按语句解析)
        // - try/catch 把脚本错误 (异常) 转为 stdout 文本 + 退出码 1, 避免错误只进 stderr
        //   且丢失详情; `exit $LASTEXITCODE` 透传脚本/外部程序的退出码
        // - UTF8 输出编码仅对 Windows PowerShell 5.1 必要 (7+ 默认 UTF-8), 统一设置无害
        auto wrapped = fmt::format(
            "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8; "
            "try {{ {} }} catch {{ Write-Output $_.Exception.Message; exit 1 }}; "
            "exit $LASTEXITCODE",
            command
        );
        return WinProcLaunch{
            psInfo.exeName,
            {"-NoProfile", "-NonInteractive", "-Command", std::move(wrapped)},
        };
    }
    XX_LOGD("ExecuteWindowsCommandTool: PowerShell not found, fallback to cmd.exe");
    return WinProcLaunch{
        "cmd.exe",
        {"/c", std::string{command}}
    };
}
#endif // BOOST_PROCESS_V2_PROCESS_HPP

} // namespace detail

// =====================================================================
// 执行体
// - 返回结果文本; 失败抛出异常 (由 C ABI 边界捕获转 error_out)
// =====================================================================

/// agentxx_execute_bash_command 执行体 (原 ExecuteBashCommandTool::execute_async)
inline std::string bashExecute(
    const neograph::json& arguments,
    const std::string&    workDir,
    const IsCancelledFn&  isCancelled = nullptr
) {
    auto command = arguments.value("command", std::string{});
    if (command.empty()) {
        return R"({"error":"Arg `command` is empty"})";
    }
    // [all_output] 为false时，仅执行失败才返回 stdout和stderr
    auto all_output = arguments.value("all_output", true);
    auto timeout    = arguments.value("timeout", 60);

#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
    {
        asio::io_context    io;
        asio::readable_pipe outpip{io}, errpip{io};
        // 创建管道，用于接收子进程的输出
        std::unordered_map<boost::process::environment::key, boost::process::environment::value>
            procEnv;
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
            io,
            procExe,
            procArgs,
            boost::process::process_environment(procEnv),
 // 子进程初始工作目录: 会话工作目录 (未配置时显式取进程 cwd, 行为等价继承)
            boost::process::process_start_dir{detail::subprocessWorkDir(workDir)},
 // stdin 重定向到 null 设备 (Windows: NUL / POSIX: /dev/null),
  // 避免子进程 (如交互式命令) 抢读 agent 进程的终端输入
            boost::process::process_stdio{.in = nullptr, .out = outpip, .err = errpip},
        };

        // 会话取消监听: 与主工作并行运行 (见下方 co_spawn 内说明)

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

        std::string        resultStr;
        std::exception_ptr ep{};
        asio::co_spawn(
            io,
            [&]() -> asio::awaitable<void> {
                std::atomic<bool> timedOut{false};
                // 主工作: stdout/stderr/退出 三方并发等待后组装结果
                // (并发等待避免子进程写满 stderr 管道与读 stdout 互等死锁)
                auto mainWork = [&]() -> asio::awaitable<void> {
                    using namespace asio::experimental::awaitable_operators;
                    co_await (
                        std::move(readStdOutFuture) && std::move(readStdErrFuture)
                        && proc.async_wait(asio::use_awaitable)
                    );
                    const auto         exitCode = proc.exit_code();
                    std::ostringstream result;
                    if (timedOut.load(std::memory_order_acquire)) {
                        result << detail::makeTimeoutResult(timeout, strout, strerr);
                    } else {
                        result << "[ExitCode]" << exitCode << "\n";
                        if (all_output || 0 != exitCode) {
                            // failed
                            if (strout.empty() || agentxx::util::autoConvertToUtf8(strout)) {
                                result << "[StdOut]\n" << strout << "\n";
                            } else {
                                result << "[StdOut conversion to utf8 failed, discard]\n";
                            }
                            if (strerr.empty() || agentxx::util::autoConvertToUtf8(strerr)) {
                                result << "[StdErr]\n" << strerr << "\n";
                            } else {
                                result << "[StdErr conversion to utf8 failed, discard]\n";
                            }
                        }
                    }
                    resultStr = result.str();
                };
                // 超时守护: 到点整组终止 (Linux 连子孙进程一起清理) 并置标记;
                // 之后挂起直至被并行组取消 —— 由主工作基于 timedOut 组装超时结果。
                // 【禁止】对已被并行组取消的 async_wait 再补二次等待: bp::v2 的
                // 退出状态仅投递一次 (SIGCHLD 已被首个 wait 消费), 二次 wait 会
                // 永久挂起并把 io.run()/整个工具调用吊死 (本次卡死 bug 根因)
                auto timeoutGuard = [&]() -> asio::awaitable<void> {
                    if (timeout > 0) {
                        asio::steady_timer t(co_await asio::this_coro::executor);
                        t.expires_after(std::chrono::seconds{timeout});
                        auto [ec] = co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
                        if (!ec) {
                            timedOut.store(true, std::memory_order_release);
                            detail::killProcGroup(proc);
                        }
                    }
                    asio::steady_timer idle(co_await asio::this_coro::executor);
                    idle.expires_after(std::chrono::hours(24));
                    co_await idle.async_wait(asio::as_tuple(asio::use_awaitable));
                };
                try {
                    using namespace asio::experimental::awaitable_operators;
                    // 三路并行: 主工作 / 超时守护 / 会话取消监听。
                    // 后两者动作完成后挂起而非返回 —— || 组合下"任一先完成即
                    // 整体完成并取消其余", 若守护/监听抢先返回会把主工作在结果
                    // 组装前整体取消; 只有主工作完成能终结整体, 完成即取消其余,
                    // io.run() 随之自然返回
                    co_await (
                        mainWork() || timeoutGuard()
                        || detail::procCancelWatchLoop(proc, isCancelled)
                    );
                } catch (...) {
                    ep = std::current_exception();
                }
            },
            asio::detached
        );
        io.run();
        if (ep) {
            std::rethrow_exception(ep);
        }
        return resultStr;
    }
#else
    // popen 回退路径: 无法指定子进程工作目录, 继承 agent 进程 cwd
    (void)workDir;
    (void)isCancelled;
#if XX_IS_WIN_D
    auto pipe = std::unique_ptr<FILE, decltype(&_pclose)>{_popen(command.c_str(), "r"), _pclose};
#else
    auto pipe = std::unique_ptr<FILE, decltype(&pclose)>{popen(command.c_str(), "r"), pclose};
#endif
    if (!pipe) {
        auto ec = std::error_code{errno, std::system_category()};
        return neograph::json{
            {"error", fmt::format("Exec command failed. Error: {}", ec.message())},
        }
            .dump();
    }

    std::array<char, 1024> buffer{};
    std::ostringstream     result;
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result << buffer.data();
    }
    return result.str();
#endif
}

/// agentxx_execute_windows_command 执行体 (原 ExecuteWindowsCommandTool::execute_async)
inline std::string windowsExecute(
    const neograph::json& arguments,
    const std::string&    workDir,
    const IsCancelledFn&  isCancelled = nullptr
) {
    auto command = arguments.value("command", std::string{});
    if (command.empty()) {
        return R"({"error":"Arg `command` is empty"})";
    }
    // [all_output] 为false时，仅执行失败才返回 stdout和stderr
    auto all_output = arguments.value("all_output", true);
    auto timeout    = arguments.value("timeout", 60);

#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
    {
        asio::io_context    io;
        asio::readable_pipe outpip{io}, errpip{io};
        std::unordered_map<boost::process::environment::key, boost::process::environment::value>
            procEnv;
        for (const auto& kv : boost::process::environment::current()) {
            if (kv.key().string() != "SECRET") {
                procEnv[kv.key()] = kv.value();
            }
        }

        auto launch  = detail::buildWinProcLaunch(command);
        auto procExe = boost::process::environment::find_executable(launch.exeName);
        if (procExe.empty() && launch.exeName != "cmd.exe") {
            // 探测到 PowerShell 但 PATH 中找不到 (极端环境): 回退 cmd.exe
            XX_LOGW(
                "ExecuteWindowsCommandTool: {} not found in PATH, fallback to cmd.exe",
                launch.exeName
            );
            launch = detail::WinProcLaunch{
                "cmd.exe",
                {"/c", command}
            };
            procExe = boost::process::environment::find_executable(launch.exeName);
        }
        auto proc = boost::process::process{
            io,
            procExe,
            launch.args,
            boost::process::process_environment(procEnv),
 // 子进程初始工作目录: 会话工作目录 (未配置时显式取进程 cwd, 行为等价继承)
            boost::process::process_start_dir{detail::subprocessWorkDir(workDir)},
 // stdin 重定向到 null 设备, 避免子进程抢读 agent 进程的终端输入
            boost::process::process_stdio{.in = nullptr, .out = outpip, .err = errpip}
        };

        // 会话取消监听: 与主工作并行运行 (见 bashExecute 同款修复说明)
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

        std::string        resultStr;
        std::exception_ptr ep{};
        asio::co_spawn(
            io,
            [&]() -> asio::awaitable<void> {
                std::atomic<bool> timedOut{false};
                // 主工作: stdout/stderr/退出 三方并发等待后组装结果
                auto mainWork = [&]() -> asio::awaitable<void> {
                    using namespace asio::experimental::awaitable_operators;
                    co_await (
                        std::move(readStdOutFuture) && std::move(readStdErrFuture)
                        && proc.async_wait(asio::use_awaitable)
                    );
                    const auto         exitCode = proc.exit_code();
                    std::ostringstream result;
                    if (timedOut.load(std::memory_order_acquire)) {
                        result << detail::makeTimeoutResult(timeout, strout, strerr);
                    } else {
                        result << "[ExitCode] " << exitCode << "\n";
                        if (all_output || 0 != exitCode) {
                            // failed
                            if (strout.empty() || agentxx::util::autoConvertToUtf8(strout)) {
                                result << "[StdOut]\n" << strout << "\n";
                            } else {
                                result << "[StdOut conversion to utf8 failed, discard]\n";
                            }
                            if (strerr.empty() || agentxx::util::autoConvertToUtf8(strerr)) {
                                result << "[StdErr]\n" << strerr << "\n";
                            } else {
                                result << "[StdErr conversion to utf8 failed, discard]\n";
                            }
                        }
                    }
                    resultStr = result.str();
                };
                // 超时守护: 到点整组终止并置标记; 之后挂起直至被并行组取消。
                // 【禁止】对已被取消的 async_wait 再补二次等待 (bp::v2 退出状态
                // 仅投递一次, 二次 wait 永久挂起) —— 同 bashExecute 修复说明
                auto timeoutGuard = [&]() -> asio::awaitable<void> {
                    if (timeout > 0) {
                        asio::steady_timer t(co_await asio::this_coro::executor);
                        t.expires_after(std::chrono::seconds{timeout});
                        auto [ec] = co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
                        if (!ec) {
                            timedOut.store(true, std::memory_order_release);
                            detail::killProcGroup(proc);
                        }
                    }
                    asio::steady_timer idle(co_await asio::this_coro::executor);
                    idle.expires_after(std::chrono::hours(24));
                    co_await idle.async_wait(asio::as_tuple(asio::use_awaitable));
                };
                try {
                    using namespace asio::experimental::awaitable_operators;
                    // 三路并行: 主工作 / 超时守护 / 会话取消监听 (语义同 bashExecute)
                    co_await (
                        mainWork() || timeoutGuard()
                        || detail::procCancelWatchLoop(proc, isCancelled)
                    );
                } catch (...) {
                    ep = std::current_exception();
                }
            },
            asio::detached
        );
        io.run();
        if (ep) {
            std::rethrow_exception(ep);
        }
        return resultStr;
    }
#else
    // popen 回退路径: 无法指定子进程工作目录, 继承 agent 进程 cwd
    (void)workDir;
    (void)isCancelled;
#if XX_IS_WIN_D
    auto pipe = std::unique_ptr<FILE, decltype(&_pclose)>{_popen(command.c_str(), "r"), _pclose};
#else
    auto pipe = std::unique_ptr<FILE, decltype(&pclose)>{popen(command.c_str(), "r"), pclose};
#endif
    if (!pipe) {
        auto ec = std::error_code{errno, std::system_category()};
        return neograph::json{
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
    return result;
#endif
}

} // namespace agentxx_execmd_plugin
