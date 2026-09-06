/// agentxx_execute_command 插件 —— 工具实现 (纯函数, 不含 C ABI 胶水)
/// - 从 libagentxx src/tools/execute_command 拆分: 同名工具同行为
///     agentxx_execute_bash_command / agentxx_execute_windows_command
/// - 头文件-only: 插件入口与测试共同包含, 保证插件行为与测试覆盖一致
/// - 与原实现的差异点:
///   - 会话工作目录/取消令牌经参数注入 (workDir / isCancelled 回调),
///     由插件入口从宿主接口表取值 (agentxx.agent.config.get_session_work_dir /
///     agentxx.agent.cancel.is_cancelled), 便于测试直测纯逻辑
///   - 统一异步操作模型 (poll 寄生驱动): 协程版执行体 *ExecuteAsync 在插件
///     实例的 PollLoop (agentxx.plugin.PollLoop, 无线程寄生事件循环) 上运行,
///     经三件套嫁接到宿主 io 线程协作式交错执行 —— 与内置工具完全同线程,
///     并发多条命令共享一个寄生 loop 等就绪事件, 不再每命令占死一个阻塞池
///     线程至超时 (原局部 io_context + io.run() 同步驱动模式已移除)
///   - AGENTXX_ENABLE_BOOST_PROCESS 关闭时的 popen 回退为阻塞实现 (*Execute
///     同步函数), 由入口经 plugin_kit.h 的 SyncToolSpec/registerSyncTool 适配注册

/// ## 输出结果压缩
/// - 禁用 ToolcallNode 的自动压缩，改由自己实现压缩, 分别独立对 stdout、stderr 压缩
/// - 格式:
/// [ExitCode]0
/// [StdOut][Content offloaded. xxx]
/// xxx
/// [StdErr][Content offloaded. xxx]
/// xxx
///
/// - 这是为了方便 LLM 判断是否存在 [StdErr] 内容，由 ToolcallNode 压缩时如果 [StdOut] 过长，
/// 可能导致 [StdErr] 全部被裁剪隐藏，LLM 需要额外读取全量才能判断是否存在 [StdErr] 内容
#pragma once

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
#include "asio/this_coro.hpp"
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

#if defined(AGENTXX_ENABLE_BOOST_PROCESS) && AGENTXX_ENABLE_BOOST_PROCESS
#include "boost/process.hpp"
#endif

#if XX_IS_WIN_D && defined(BOOST_PROCESS_V2_PROCESS_HPP)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace agentxx_execmd_plugin {

/// 取消查询回调 (返回 true 表示会话已取消); 测试可传 nullptr 等价无取消支持
using IsCancelledFn = std::function<bool()>;

/// 写入 share_store 回调 (返回 ID，失败返回 -1); 插件侧注入，测试可传 nullptr
using StoreFn = std::function<long long(std::string_view)>;

namespace detail {

// ---------------------------------------------------------------------
// 手动裁剪 stdout / stderr (插件侧自治, 不依赖 ToolcallNode 的 share_store 卸载)
// - 每路独立按 UTF-8 字符数限制, 超限时截断并以 toolcall 同款格式提示
//   [Content offloaded. Use the `agentxx_share_store` tool ... Total X lines,
//    show [0, Y], hide [Y+1, X]]\n<cropped>...
//   与 ToolcallNode::execTool 的 offload 格式统一, 便于模型用
//   agentxx_share_store 按行分页取回 (当前插件侧暂不实际 offload 到
//   share_store，storeId==-1 时省略 ID，仅提示可用 share_store)
// - 保持 UTF-8 安全, 使用 findIndexAndLastLineIndexByUtf8Length (行边界优先)
// - 限制值选取 30k 字符/每路 (≈ 30KB 文本, 远大于 ToolcallNode 的 2k 全局限制
//   但足以避免 60k+ 超长输出撑爆 LLM 上下文); 总结果 ≤ 60k+ 开销, 适中
// ---------------------------------------------------------------------
constexpr size_t kMaxStdOutUtf8Length = 30000;
constexpr size_t kMaxStdErrUtf8Length = 30000;

/// 统一格式的截断实现 (仿照 ToolcallNode::execTool)
inline std::string truncateWithStoreFormat(
    const std::string& s,
    size_t             maxLen,
    long long          storeId = -1 // -1 表示未 offload，不带 ID
) {
    if (s.empty()) {
        return s;
    }
    // 快速路径: 未超限直接返回 (先按字节粗判，避免大串的 utf8GetLength 开销)
    // 与 ToolcallNode 保持一致: 先判字节 size >= limit 再按 utf8 长度
    if (s.size() < maxLen) {
        const size_t totalLen = agentxx::util::utf8GetLength(s);
        if (totalLen <= maxLen) {
            return s;
        }
    } else {
        // 大串仍需精确 utf8 长度判断
        const size_t totalLen = agentxx::util::utf8GetLength(s);
        if (totalLen <= maxLen) {
            return s;
        }
    }
    auto [targetIndex, lineCount, lastLineIndex]
        = agentxx::util::findIndexAndLastLineIndexByUtf8Length(s, maxLen);
    if (targetIndex == 0) {
        // 极端情况 (如非法 UTF-8 导致 0) 回退按字节截断，避免返回空
        const size_t cut        = std::min(s.size(), maxLen);
        const size_t totalLines = agentxx::util::countLines(s);
        std::string  truncated(s.data(), cut);
        std::string  header;
        if (storeId >= 0) {
            header = fmt::format(
                "[Content offloaded. Use the `agentxx_share_store` tool to fetch the content by ID {}. Total {} lines.]",
                storeId,
                totalLines
            );
        } else {
            header = fmt::format(
                "[Content offloaded. Use the `agentxx_share_store` tool to fetch the full content. Total {} lines.]",
                totalLines
            );
        }
        return fmt::format("{}\n{}...", header, truncated);
    }
    const size_t totalLineCount = agentxx::util::countLines(s);
    if (lastLineIndex >= targetIndex / 3) {
        // 行边界截断可行: 显示 [0, lineCount], 隐藏 [lineCount+1, total]
        std::string header;
        if (storeId >= 0) {
            header = fmt::format(
                "[Content offloaded. Use the `agentxx_share_store` tool to fetch the content by ID {}. Total {} lines, show [0, {}], hide [{}, {}].]",
                storeId,
                totalLineCount,
                lineCount,
                lineCount + 1,
                totalLineCount
            );
        } else {
            header = fmt::format(
                "[Content offloaded. Use the `agentxx_share_store` tool to fetch the content. Total {} lines, show [0, {}], hide [{}, {}].]",
                totalLineCount,
                lineCount,
                lineCount + 1,
                totalLineCount
            );
        }
        return fmt::format("{}\n{}...", header, std::string_view{s}.substr(0, lastLineIndex));
    } else {
        // 无法按行截断 (如单行超长): 按字符截断
        std::string header;
        if (storeId >= 0) {
            header = fmt::format(
                "[Content offloaded. Use the `agentxx_share_store` tool to fetch the full content by ID {}. Total {} lines.]",
                storeId,
                totalLineCount
            );
        } else {
            header = fmt::format(
                "[Content offloaded. Use the `agentxx_share_store` tool to fetch the full content. Total {} lines.]",
                totalLineCount
            );
        }
        return fmt::format("{}\n{}...", header, std::string_view{s}.substr(0, targetIndex));
    }
}

inline std::string truncateStdOut(const std::string& s, long long storeId = -1) {
    return truncateWithStoreFormat(s, kMaxStdOutUtf8Length, storeId);
}

inline std::string truncateStdErr(const std::string& s, long long storeId = -1) {
    return truncateWithStoreFormat(s, kMaxStdErrUtf8Length, storeId);
}

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

// 前置声明 (定义在下方 WinJobGuard/killProcGroup 之后; procCancelWatchLoop
// 与 timeoutGuard 需要调用它们)
inline void killProcGroup(boost::process::process& proc, void* winJob = nullptr);
inline void closePipesAfterKill(asio::readable_pipe& outpip, asio::readable_pipe& errpip);

/// 会话取消监听协程体 (与主工作经 awaitable_operators 并行运行):
/// - 轮询 isCancelled 回调 (宿主会话取消令牌 + op cancel_flag 双通道),
///   取消时终止子进程
/// - 【关键生命周期语义】动作完成后不立即结束, 而是挂起直至被并行组取消:
///   || 组合下"任一先完成即整体完成并取消其余", 若本协程在 kill 后立刻返回,
///   会在主工作组装结果前把它整体取消 (丢失输出); 挂起让主工作自然收尾,
///   由主工作完成驱动整体终结 —— 同时保证寄生 loop 不会被无限轮询的 watcher
///   吊住 (历史 bug: detached watcher + RAII guard 互相死等)
/// - Linux: 子进程经 setsid 启动 (pgid == pid), `kill(-pid)` 可整组清理
/// - Windows: 子进程挂入 Job Object, `TerminateJobObject` 整树清理
/// - 注意: watcher 只终止、不调用 async_wait —— boost.process v2 的
///   async_wait op 内部引用共享的 exit_status_ 成员, 与主协程并发 wait
///   会产生数据竞争; 子进程回收由主协程负责
inline asio::awaitable<void> procCancelWatchLoop(
    boost::process::process& proc,
    asio::readable_pipe&     outpip,
    asio::readable_pipe&     errpip,
    void*                    winJob,
    const IsCancelledFn&     isCancelled
) {
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
    // 会话取消: 终止整棵进程树 (Windows: Job Object; Linux: 进程组) 后挂起,
    // 让主工作自然收尾; 终止后主动关闭本端管道读句柄, 确保即使有漏网
    // 孙进程仍持有写端, 在途 async_read 也能立刻以错误完成, 避免主工作
    // 永久等待 EOF 而挂死 (见 runProcPipeline timeoutGuard 同款处理)
    detail::killProcGroup(proc, winJob);
    detail::closePipesAfterKill(outpip, errpip);
    timer.expires_after(std::chrono::hours(24));
    co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
}

#if XX_IS_WIN_D
/// Windows Job Object 句柄封装 (仅 Windows + bp::v2 编译路径存在):
/// - CreateJobObject 创建, 配置 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE:
///   最后一个 job 句柄关闭时自动终止其中所有进程 —— 即便调用方忘记显式
///   TerminateJobObject (异常路径/析构), 子进程树也不会泄漏成孤儿
/// - AssignProcessToJobObject 把直接子进程挂入; 其后代 (pwsh 再派生 ping
///   等) 自动继承同一 job (除非它们自身创建了不允许 breakaway 的嵌套 job)
/// - TerminateJobObject 整树终止: 修复原先只 TerminateProcess 直接子进程,
///   漏杀持管道写端的孙进程导致管道永不 EOF、工具永久挂起的缺陷
struct WinJobGuard {
    void* job = nullptr; // HANDLE

    WinJobGuard() {
        HANDLE h = ::CreateJobObjectW(nullptr, nullptr);
        if (!h) {
            return;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!::SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
            ::CloseHandle(h);
            return;
        }
        job = h;
    }

    ~WinJobGuard() {
        if (job) {
            // 兜底整树终止 (正常路径 runProcPipeline 已 TerminateJobObject,
            // 此处幂等; 主要覆盖异常/提前返回路径防孤儿)
            ::TerminateJobObject(static_cast<HANDLE>(job), 260);
            ::CloseHandle(static_cast<HANDLE>(job));
        }
    }

    WinJobGuard(const WinJobGuard&)            = delete;
    WinJobGuard& operator=(const WinJobGuard&) = delete;

    bool valid() const {
        return job != nullptr;
    }

    void* handle() const {
        return job;
    }
};
#endif

/// 终止子进程及其整棵进程树
/// - Linux: setsid 启动, pgid == pid, `kill(-pid)` 整组 SIGKILL
/// - Windows: 若已挂入 Job Object (winJob 非空) 则 TerminateJobObject 整树
///   终止 (pwsh 派生的孙进程一并清理); 否则回退 TerminateProcess 仅杀直接子进程
inline void killProcGroup(boost::process::process& proc, void* winJob) {
#if XX_IS_WIN_D
    if (winJob) {
        ::TerminateJobObject(static_cast<HANDLE>(winJob), 260);
        return;
    }
    neograph_asio_error_code ec;
    proc.terminate(ec);
#else
    // setsid 启动, pgid == pid; kill 负 pid 整组清理 (含 bash 派生的子孙进程)
    (void)winJob;
    ::kill(-static_cast<pid_t>(proc.id()), SIGKILL);
#endif
}

/// 终止子进程后主动关闭 stdout/stderr 管道本端读句柄:
/// - 目的: 即使进程树未杀净 (例如 Windows 下 Job assign 失败回退
///   TerminateProcess, 孙进程仍持有管道写端), 在途 async_read(transfer_all)
///   也会立即以错误结束, 使 runProcPipeline 主工作能返回, 不会永久挂起
/// - 仅在 kill 之后调用 (正常结束路径由子进程自然关闭写端, 无需关闭)
/// - Linux 整组 SIGKILL 后子进程写端必然关闭, 此调用幂等无害
inline void closePipesAfterKill(asio::readable_pipe& outpip, asio::readable_pipe& errpip) {
    neograph_asio_error_code ec;
    outpip.close(ec);
    errpip.close(ec);
}

/// 构造超时错误结果文本 (多行纯文本输出, 非 JSON)
inline std::string makeTimeoutResult(
    int            timeout,
    std::string    strout,
    std::string    strerr,
    const StoreFn& storeFn = nullptr
) {
    if (false == (strout.empty() || agentxx::util::autoConvertToUtf8(strout))) {
        strout = "[StdOut conversion to utf8 failed, discard]";
    } else if (!strout.empty()) {
        long long id = -1;
        // 仅超限时才 offload 到 share_store
        if (storeFn && agentxx::util::utf8GetLength(strout) > kMaxStdOutUtf8Length) {
            id = storeFn(strout);
        }
        strout = truncateStdOut(strout, id);
    }
    if (false == (strerr.empty() || agentxx::util::autoConvertToUtf8(strerr))) {
        strerr = "[StdErr conversion to utf8 failed, discard]";
    } else if (!strerr.empty()) {
        long long id = -1;
        if (storeFn && agentxx::util::utf8GetLength(strerr) > kMaxStdErrUtf8Length) {
            id = storeFn(strerr);
        }
        strerr = truncateStdErr(strerr, id);
    }
    return fmt::format(
        R"_(
[Error]
Command timed out after {} seconds. If this command is expected to take longer and is not waiting for interactive input, retry with a larger timeout value.
[StdOut]
{}
[StdErr]
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

/// 三路并行等待 stdout/stderr/退出并组装结果 (bash/windows 两执行体共用):
/// - mainWork / timeoutGuard / procCancelWatchLoop 三路并行, 只有主工作完成能
///   终结整体 (后两者动作后挂起, 见 procCancelWatchLoop 注释)
/// - 返回组装后的结果文本; 组合协程异常原样传播 (调用方 C ABI 边界兜底)
/// - winJob: Windows 下子进程挂入的 Job Object (HANDLE); 超时/取消时经
///   TerminateJobObject 整树终止。Linux 恒为 nullptr (进程组已覆盖)
inline asio::awaitable<std::string> runProcPipeline(
    boost::process::process& proc,
    asio::readable_pipe&     outpip,
    asio::readable_pipe&     errpip,
    int                      timeout,
    bool                     all_output,
    const IsCancelledFn&     isCancelled,
    const StoreFn&           storeFn = nullptr,
    void*                    winJob  = nullptr
) {
    std::string              strout, strerr;
    neograph_asio_error_code errCodeStdOut, errCodeStdErr;
    // awaitable 为 move-only: 创建后 move 进 && 组合 (不可拷贝)
    auto readStdOutFuture = asio::async_read(
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

    std::atomic<bool> timedOut{false};
    std::string       resultStr;
    // 主工作: stdout/stderr/退出 三方并发等待后组装结果
    // (并发等待避免子进程写满 stderr 管道与读 stdout 互等死锁)
    // 【组合约束】三路并行分支必须同为 awaitable<void> (|| 组合要求分支类型
    // 一致), 结果经捕获引用写入 resultStr —— 与原实现同款结构
    auto mainWork = [&]() -> asio::awaitable<void> {
        using namespace asio::experimental::awaitable_operators;
        co_await (
            std::move(readStdOutFuture) && std::move(readStdErrFuture)
            && proc.async_wait(asio::use_awaitable)
        );
        const auto         exitCode = proc.exit_code();
        std::ostringstream result;
        if (timedOut.load(std::memory_order_acquire)) {
            result << detail::makeTimeoutResult(timeout, strout, strerr, storeFn);
        } else {
            result << "[ExitCode]\n" << exitCode << "\n";
            if (all_output || 0 != exitCode) {
                // 手动裁剪: 每路 stdout/stderr 独立按 UTF-8 长度限制, 格式与
                // ToolcallNode::execTool 的 [Content offloaded...] 保持一致，通过 storeFn
                // 将完整内容 offload 到 share_store（超限时）并带 ID
                if (strout.empty() || agentxx::util::autoConvertToUtf8(strout)) {
                    if (!strout.empty()) {
                        long long id = -1;
                        if (storeFn
                            && agentxx::util::utf8GetLength(strout)
                                   > detail::kMaxStdOutUtf8Length) {
                            id = storeFn(strout);
                        }
                        strout = detail::truncateStdOut(strout, id);
                    }
                    // 预期紧凑格式: [StdOut][Content offloaded...] 截断时同行，无截断时换行
                    if (!strout.empty() && strout.rfind("[Content", 0) == 0) {
                        result << "[StdOut]" << strout << "\n";
                    } else {
                        result << "[StdOut]\n" << strout << "\n";
                    }
                } else {
                    result << "[StdOut conversion to utf8 failed, discard]\n";
                }
                if (strerr.empty() || agentxx::util::autoConvertToUtf8(strerr)) {
                    if (!strerr.empty()) {
                        long long id = -1;
                        if (storeFn
                            && agentxx::util::utf8GetLength(strerr)
                                   > detail::kMaxStdErrUtf8Length) {
                            id = storeFn(strerr);
                        }
                        strerr = detail::truncateStdErr(strerr, id);
                    }
                    if (!strerr.empty() && strerr.rfind("[Content", 0) == 0) {
                        result << "[StdErr]" << strerr << "\n";
                    } else {
                        result << "[StdErr]\n" << strerr << "\n";
                    }
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
    // 退出状态仅投递一次 (SIGCHLD 已被首个 wait 处理), 二次 wait 会
    // 永久挂起并把整个工具调用吊死 (历史卡死 bug 根因)
    auto timeoutGuard = [&]() -> asio::awaitable<void> {
        if (timeout > 0) {
            asio::steady_timer t(co_await asio::this_coro::executor);
            t.expires_after(std::chrono::seconds{timeout});
            auto [ec] = co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
            if (!ec) {
                timedOut.store(true, std::memory_order_release);
                detail::killProcGroup(proc, winJob);
                // 终止后主动关闭本端管道读句柄: 若进程树未杀净 (Windows 下
                // Job 不可用/assign 失败回退 TerminateProcess 时, 漏网孙进程
                // 仍持有写端), 在途 async_read 立即以错误完成, 主工作不会
                // 永久等待 EOF —— 保证工具调用到点必返回
                detail::closePipesAfterKill(outpip, errpip);
            }
        }
        asio::steady_timer idle(co_await asio::this_coro::executor);
        idle.expires_after(std::chrono::hours(24));
        co_await idle.async_wait(asio::as_tuple(asio::use_awaitable));
    };

    // 三路并行: 主工作 / 超时守护 / 会话取消监听。
    // 后两者动作完成后挂起而非返回 —— || 组合下"任一先完成即整体完成并取消
    // 其余", 若守护/监听抢先返回会把主工作在结果组装前整体取消; 只有主工作
    // 完成能终结整体
    {
        using namespace asio::experimental::awaitable_operators;
        co_await (
            mainWork() || timeoutGuard()
            || detail::procCancelWatchLoop(proc, outpip, errpip, winJob, isCancelled)
        );
    }
    co_return resultStr;
}

#endif // BOOST_PROCESS_V2_PROCESS_HPP

} // namespace detail

#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
// =====================================================================
// 执行体 —— 协程版 (poll 寄生驱动路径; BOOST_PROCESS 可用时唯一注册路径)
// - 在插件实例 PollLoop 的 io_context 上 spawn, 挂起点让出给宿主 io 线程;
//   返回结果文本, 失败抛出异常 (由 C ABI 边界/适配器捕获转 error_out)
// =====================================================================

/// agentxx_execute_bash_command 执行体 (原 ExecuteBashCommandTool::execute_async)
inline asio::awaitable<std::string> bashExecuteAsync(
    const neograph::json& arguments,
    const std::string&    workDir,
    const IsCancelledFn&  isCancelled = nullptr,
    const StoreFn&        storeFn     = nullptr
) {
    auto command = arguments.value("command", std::string{});
    if (command.empty()) {
        co_return R"({"error":"Arg `command` is empty"})";
    }
    // [all_output] 为false时，仅执行失败才返回 stdout和stderr
    auto all_output = arguments.value("all_output", true);
    auto timeout    = arguments.value("timeout", 60);

    // 寄生驱动: 管道/进程绑定到当前协程的 executor (插件实例 PollLoop),
    // 由宿主 io 线程经 pollOnce 非阻塞步进 (不再自建 io_context + run())
    auto                ex = co_await asio::this_coro::executor;
    asio::readable_pipe outpip{ex}, errpip{ex};
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
        ex,
        procExe,
        procArgs,
        boost::process::process_environment(procEnv),
 // 子进程初始工作目录: 会话工作目录 (未配置时显式取进程 cwd, 行为等价继承)
        boost::process::process_start_dir{detail::subprocessWorkDir(workDir)},
 // stdin 重定向到 null 设备 (Windows: NUL / POSIX: /dev/null),
  // 避免子进程 (如交互式命令) 抢读 agent 进程的终端输入
        boost::process::process_stdio{.in = nullptr, .out = outpip, .err = errpip},
    };

    // 会话取消监听: 与主工作并行运行 (见 detail::runProcPipeline 说明)
    co_return co_await detail::runProcPipeline(
        proc,
        outpip,
        errpip,
        timeout,
        all_output,
        isCancelled,
        storeFn
    );
}

/// agentxx_execute_windows_command 执行体 (原 ExecuteWindowsCommandTool::execute_async)
inline asio::awaitable<std::string> windowsExecuteAsync(
    const neograph::json& arguments,
    const std::string&    workDir,
    const IsCancelledFn&  isCancelled = nullptr,
    const StoreFn&        storeFn     = nullptr
) {
    auto command = arguments.value("command", std::string{});
    if (command.empty()) {
        co_return R"({"error":"Arg `command` is empty"})";
    }
    auto all_output = arguments.value("all_output", true);
    auto timeout    = arguments.value("timeout", 60);

    auto                ex = co_await asio::this_coro::executor;
    asio::readable_pipe outpip{ex}, errpip{ex};
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

#if XX_IS_WIN_D
    // Windows Job Object: 把子进程整棵树 (pwsh/cmd 及其派生的孙进程, 如
    // ping -t) 纳入同一 job。超时/取消时 TerminateJobObject 一次整树终止,
    // 修复只杀直接子进程导致孙进程持管道写端、工具永久挂起的缺陷。
    // KILL_ON_JOB_CLOSE 保证异常路径下 job 析构时进程树也被清理, 不泄漏孤儿。
    detail::WinJobGuard winJobGuard;
    void*               winJob = winJobGuard.valid() ? winJobGuard.handle() : nullptr;

    auto proc = boost::process::process{
        ex,
        procExe,
        launch.args,
        boost::process::process_environment(procEnv),
 // 子进程初始工作目录: 会话工作目录 (未配置时显式取进程 cwd, 行为等价继承)
        boost::process::process_start_dir{detail::subprocessWorkDir(workDir)},
 // stdin 重定向到 null 设备, 避免子进程抢读 agent 进程的终端输入
        boost::process::process_stdio{.in = nullptr, .out = outpip, .err = errpip}
    };

    // CreateProcess 返回后立即挂入 job。这里不做 CREATE_SUSPENDED + 挂起期
    // assign (bp v2 launcher 在返回前关闭主线程句柄, 无法恢复线程; 且其
    // on_success initializer 存在返回类型缺陷), 而是接受极小竞态窗口:
    // 即使孙进程抢跑逃逸出 job, 方案 B (kill 后关闭管道) 仍保证工具到点
    // 返回, 不会永久挂起。assign 失败 (如子进程已在他 job 且不允许
    // breakaway) 仅告警并回退单进程 terminate。
    if (winJob) {
        HANDLE hProc = proc.handle().native_handle();
        if (!hProc || hProc == INVALID_HANDLE_VALUE
            || !::AssignProcessToJobObject(static_cast<HANDLE>(winJob), hProc)) {
            XX_LOGW("ExecuteWindowsCommandTool: AssignProcessToJobObject failed, "
                    "fallback to single-process terminate");
            winJob = nullptr;
        }
    }
#else
    auto proc = boost::process::process{
        ex,
        procExe,
        launch.args,
        boost::process::process_environment(procEnv),
        boost::process::process_start_dir{detail::subprocessWorkDir(workDir)},
        boost::process::process_stdio{.in = nullptr, .out = outpip, .err = errpip}
    };
    void* winJob = nullptr;
#endif

    // 会话取消监听: 与主工作并行运行 (语义同 bashExecuteAsync)
    co_return co_await detail::runProcPipeline(
        proc,
        outpip,
        errpip,
        timeout,
        all_output,
        isCancelled,
        storeFn,
        winJob
    );
}

#else
// =====================================================================
// 执行体 —— popen 回退版 (仅 AGENTXX_ENABLE_BOOST_PROCESS 关闭时编译/注册)
// - 阻塞实现: 只允许经 plugin_kit.h 的 SyncToolSpec/registerSyncTool 适配注册,
// 禁止在宿主 io 线程/poll 寄生 loop 上直接调用
// - 无法指定子进程工作目录 (继承 agent 进程 cwd), 无会话取消支持
// =====================================================================

inline std::string bashExecute(
    const neograph::json& arguments,
    const std::string&    workDir,
    const IsCancelledFn&  isCancelled = nullptr,
    const StoreFn&        storeFn     = nullptr
) {
    (void)workDir;
    (void)isCancelled;
    auto command = arguments.value("command", std::string{});
    if (command.empty()) {
        return R"({"error":"Arg `command` is empty"})";
    }
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
    std::string out = result.str();
    agentxx::util::autoConvertToUtf8(out);
    // 手动裁剪: popen 回退同样按 stdout 限制截断 (不依赖 ToolcallNode)
    if (!out.empty()) {
        long long id = -1;
        if (storeFn && agentxx::util::utf8GetLength(out) > detail::kMaxStdOutUtf8Length) {
            id = storeFn(out);
        }
        out = detail::truncateStdOut(out, id);
    }
    return out;
}

inline std::string windowsExecute(
    const neograph::json& arguments,
    const std::string&    workDir,
    const IsCancelledFn&  isCancelled = nullptr,
    const StoreFn&        storeFn     = nullptr
) {
    // 无 bp::v2 时 Windows 命令同样走 popen 回退 (cmd.exe 语义由提示词引导)
    return bashExecute(arguments, workDir, isCancelled, storeFn);
}

#endif // BOOST_PROCESS_V2_PROCESS_HPP

} // namespace agentxx_execmd_plugin
