/// agentxx_execute_command 插件 —— 运行环境探测与工具提示词生成
///
/// 原 libagentxx 的 `AgentPrompt` 承担的环境探测 (PowerShell 探测 + 提示词刷新)
/// 随工具实现一起迁移到本插件, 由插件在 start 事务内完成:
/// 1. [detectExecEnv]: 探测 python / node 是否可用及版本 (Windows 侧另探测
///    PowerShell 可执行文件名与版本);
/// 2. [bashToolPrompt] / [windowsToolPrompt]: 用探测结果生成工具提示词
///    (depict + 参数描述), 由插件经 prompt 接口表作为本实例贡献注入宿主,
///    卸载/禁用时由宿主自动撤销。
///
/// 为什么在 start 事务内探测:
/// - 工具定义 (description / parameters) 在注册时固化 (见 PluginHost 的
///   `register_tool`), 描述文本必须在注册前就绪 —— 注册后无法再刷新;
/// - 探测结果同时决定 Windows 侧执行器 (PowerShell / cmd.exe) 的提示词分支;
/// - 探测为阻塞式子进程调用 (`--version`), 每次 start 只执行一次, 结果存
///   实例上下文 (ExecPluginCtx::env)。插件内不使用进程级可变静态量, 满足
///   同一动态库多实例并存的约束 (多实例三铁律)。
///
/// 提示词文本与工具 schema 由同一份 [ExecPromptText] 提供 (插件入口传给
/// `ctx.schema(...)`), 保证注入宿主的提示词与实际注册的 schema 一致。
#pragma once

#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include "utilxx_base/system.h"
#include "fmt/format.h"
#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#if XX_IS_WIN_D
#include <cstdio>
#else
#include <climits>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace agentxx_execmd_plugin {

/// 单个解释器 (python / node) 的探测结果
struct InterpreterInfo {
    /// 是否找到可用解释器
    bool available = false;
    /// 探测成功的可执行文件名, 如 "python3" / "node.exe" (未找到时为空)
    std::string exeName;
    /// 版本号, 如 "3.11.9" / "20.11.0" (未找到时为空)
    std::string version;
};

/// 命令执行插件的运行环境探测结果 (start 事务内探测一次, 存实例上下文)
struct ExecEnvInfo {
    /// 系统名 (如 "Ubuntu 22.04.5 LTS" / "Windows 10.0 (build 19045)")
    std::string systemName;
    /// 是否运行在 WSL (Windows 侧工具经 interop 调用时提示词需说明路径转换)
    bool isWSL = false;
    /// python 解释器探测结果
    InterpreterInfo python;
    /// node 解释器探测结果
    InterpreterInfo node;
    /// PowerShell 探测结果 (仅 Windows 侧探测; 其他平台恒为不可用)
    utilxx_base::PowerShellInfo powershell;
};

/// 工具提示词 (depict + 参数描述), 字段与宿主 AgentPrompt::ToolPrompt 对应
/// - depict 作为工具 definition 的整体描述, args 为各参数 description
struct ExecPromptText {
    std::string                                     depict;
    std::map<std::string, std::string, std::less<>> args;
};

/// 各解释器候选探测的超时 (毫秒): `--version` 正常在百毫秒级返回,
/// 目标可执行文件异常挂起时由此看门狗强制回收
/// - 单个候选超时后继续探测下一个候选, 最坏情况为"候选数 × 超时"
/// - 相关: Windows 侧的 [_popen] 无看门狗 (版本查询立即结束, 与 PowerShell
///   探测一致), 超时值仅 POSIX 路径生效
inline constexpr int kInterpreterProbeTimeoutMs = 3000;

// ---------------------------------------------------------------------------
// 解释器探测
// ---------------------------------------------------------------------------

/// 版本号样式校验: 只含数字与点, 且至少一个点与一个数字 (如 "3.11.9" / "20.11.0")
/// - 用于排除"同名无关程序"与 Windows 应用商店的 python.exe 占位程序输出
inline bool isValidVersionToken(std::string_view token) {
    if (token.empty()) {
        return false;
    }
    bool hasDot   = false;
    bool hasDigit = false;
    for (char c : token) {
        if (c == '.') {
            hasDot = true;
            continue;
        }
        if (c < '0' || c > '9') {
            return false;
        }
        hasDigit = true;
    }
    return hasDot && hasDigit;
}

/// 从 `--version` 输出中提取版本号
/// - 直接取首个形如版本号的空白分隔词: `Python 3.11.9` → `3.11.9`,
///   `v20.11.0` → `20.11.0` (去掉 `v` 前缀)
/// - 逐行尝试: 版本行之前可能有警告/提示行 (例如 python 的站点配置警告)
/// - 输出中找不到合法版本号时返回空串 (调用方按"未找到"处理)
inline std::string parseVersionText(std::string_view output) {
    size_t pos = 0;
    while (pos <= output.size()) {
        size_t end = output.find_first_of("\r\n", pos);
        if (end == std::string_view::npos) {
            end = output.size();
        }
        const auto line = utilxx_base::removeBetweenSpace(output.substr(pos, end - pos));
        if (false == line.empty()) {
            // 取行内最后一个空白分隔词: "Python 3.11.9" → "3.11.9"
            const auto sp    = line.find_last_of(" \t");
            auto       token = (sp == std::string::npos) ? line : line.substr(sp + 1);
            if (false == token.empty() && (token[0] == 'v' || token[0] == 'V')) {
                token.erase(0, 1);
            }
            if (isValidVersionToken(token)) {
                return token;
            }
        }
        if (end >= output.size()) {
            break;
        }
        pos = end + 1;
    }
    return {};
}

#if XX_IS_WIN_D

/// 运行候选可执行文件并读取其合并输出 (stdout + stderr)
/// - `exeName` 只来自本文件的固定候选列表 (无外部输入), 经 cmd.exe 执行
/// - 探测命令立即结束 (版本查询), 不做超时看门狗; 未找到该可执行文件时
///   cmd.exe 返回 9009, 输出为空或无关提示文本, 由调用方校验版本号
inline std::string runProbeCommand(const char* exeName, int timeoutMs) {
    (void)timeoutMs;
    std::string cmd = std::string{exeName} + " --version 2>&1";
    FILE*       fp  = _popen(cmd.c_str(), "r");
    if (nullptr == fp) {
        return {};
    }
    std::string out;
    char        buf[512];
    while (fgets(buf, sizeof(buf), fp) != nullptr) {
        out.append(buf);
    }
    _pclose(fp);
    return out;
}

#else

/// 运行候选可执行文件并读取其合并输出 (stdout + stderr)
/// - 直接 execvp 候选可执行文件 (不经 shell), 版本请求作为单个 argv 元素传入,
///   从根源避免引号/变量展开问题
/// - python 2 的 `--version` 输出走 stderr, 故 stderr 与 stdout 合并到同一管道
/// - fork + pipe + poll 看门狗: 目标异常挂起时 SIGKILL 强制回收, 子进程一律
///   waitpid 回收不留僵尸
/// - `exeName` 只来自本文件的固定候选列表; 仍校验字符集 (字母数字与 `._-`)
inline std::string runProbeCommand(const char* exeName, int timeoutMs) {
    for (char c : std::string_view{exeName}) {
        const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                           || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (false == valid) {
            return {};
        }
    }

    int pipefd[2] = {-1, -1};
    if (::pipe(pipefd) != 0) {
        return {};
    }
    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return {};
    }
    if (pid == 0) {
        // 子进程: stdout/stderr 接到管道, stdin 重定向到 /dev/null
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::close(devnull);
        }
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        // 新会话: 子进程组被误杀时不波及 agent 自身
        ::setsid();
        const char* argv[] = {exeName, "--version", nullptr};
        ::execvp(exeName, const_cast<char* const*>(argv));
        ::_exit(127);
    }
    ::close(pipefd[1]);

    std::string out;
    char        buf[512];
    bool        childExited = false;
    const auto  deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (false == childExited) {
        const auto remainMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now()
        )
                                  .count();
        if (remainMs <= 0) {
            ::kill(pid, SIGKILL);
            XX_LOGW("execute_command probe {} timeout, killed pid={}", exeName, (long)pid);
            break;
        }

        struct pollfd pfd {
            .fd = pipefd[0], .events = POLLIN
        };

        const int pollRet
            = ::poll(&pfd, 1, static_cast<int>(std::min<long long>(remainMs, INT_MAX)));
        if (pollRet > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            const ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
            if (n > 0) {
                out.append(buf, static_cast<size_t>(n));
                continue;
            }
        }
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid) {
            childExited = true;
        }
    }
    ::close(pipefd[0]);
    if (false == childExited) {
        int status = 0;
        ::waitpid(pid, &status, 0);
    }
    return out;
}

#endif

/// 依次探测候选可执行文件, 返回第一个能输出合法版本号者
/// - 未找到任何候选时返回 `available = false` (exeName/version 为空)
inline InterpreterInfo
    detectInterpreter(const std::vector<const char*>& candidates, int timeoutMs) {
    InterpreterInfo info;
    for (const char* exeName : candidates) {
        const auto output  = runProbeCommand(exeName, timeoutMs);
        auto       version = parseVersionText(output);
        XX_LOGD(
            "execute_command probe {}: output='{}' version='{}'",
            exeName,
            utilxx_base::removeBetweenSpace(output),
            version
        );
        if (false == version.empty()) {
            info.available = true;
            info.exeName   = exeName;
            info.version   = std::move(version);
            return info;
        }
    }
    return info;
}

/// 探测运行环境 (python / node, Windows 侧含 PowerShell 与系统名)
/// - 阻塞式: 每个候选 spawn 一次子进程 (`--version`), 由 [runProbeCommand] 超时看门狗兜底
/// - PowerShell 探测结果与执行期选择执行器共用 (agentxx_util 按进程缓存同一结果);
///   Linux/macOS 不需要 PowerShell, 直接跳过探测, 避免 WSL 下无谓的 interop 调用
inline ExecEnvInfo detectExecEnv() {
    ExecEnvInfo env;
    env.systemName = utilxx_base::getSystemName();
    env.isWSL      = utilxx_base::isRunningInWSL();

    const auto start = std::chrono::steady_clock::now();
#if XX_IS_WIN_D
    // Windows 侧: 优先 python.exe / python3.exe, 再回退 Python 启动器 py.exe
    env.python
        = detectInterpreter({"python.exe", "python3.exe", "py.exe"}, kInterpreterProbeTimeoutMs);
    env.node       = detectInterpreter({"node.exe"}, kInterpreterProbeTimeoutMs);
    env.powershell = utilxx_base::detectPowerShell();
#else
    // POSIX 侧: python 命令名在不同发行版可能是 python3 / python
    env.python = detectInterpreter({"python3", "python"}, kInterpreterProbeTimeoutMs);
    env.node   = detectInterpreter({"node", "nodejs"}, kInterpreterProbeTimeoutMs);
#endif
    const auto costMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start
    )
                            .count();

    XX_LOGI(
        "execute_command env detected in {}ms: system='{}' wsl={} python='{}' node='{}' "
        "powershell='{}'",
        costMs,
        env.systemName,
        env.isWSL,
        env.python.available ? fmt::format("{} {}", env.python.exeName, env.python.version)
                             : std::string{"not found"},
        env.node.available ? fmt::format("{} {}", env.node.exeName, env.node.version)
                           : std::string{"not found"},
        env.powershell.available
            ? fmt::format("{} {}", env.powershell.exeName, env.powershell.version)
            : std::string{"not found"}
    );
    return env;
}

// ---------------------------------------------------------------------------
// 提示词文本
// ---------------------------------------------------------------------------

/// `all_output` 参数描述 (bash / windows 工具共用)
inline constexpr std::string_view kAllOutputArgDesc =
    R"(Default `true`.
`true`: Always return stdout and stderr output.
`false`: Only return output when the command fails.)";

/// `timeout` 参数描述 (bash / windows 工具共用)
inline constexpr std::string_view kTimeoutArgDesc
    = "Default `60` seconds. Execution timeout in seconds. Set `0` for no limit.";

/// bash 工具 depict (POSIX 侧工具描述)
inline constexpr std::string_view kBashToolDepict
    = "Execute a shell/bash command and return its output.";

/// Windows 命令工具 `command` 参数的兜底描述 (提示词/探测结果不可用时使用;
/// 正常路径由 [windowsToolPrompt] 生成的描述覆盖)
inline constexpr std::string_view kWindowsCommandArgDescFallback
    = "The Windows command to execute.";

/// Windows 命令参数描述的公共前缀 (区分 WSL / 原生 Windows)
inline std::string winCommandPrefix(const ExecEnvInfo& env) {
    if (false == env.isWSL) {
        return "The Windows command to execute.";
    }
    return R"(Command to execute in the Windows terminal (via WSL interop).
Current system is WSL. This tool runs the command on the Windows side.
If the user provides a Windows path (e.g. `C:\...` or `D:\...`), convert it to a WSL path (`/mnt/c/...` or `/mnt/d/...`) for file operations, but use the original Windows path when passing to Windows executables.)";
}

/// 解释器可用性说明段: 追加到 `command` 参数描述
/// - 目的: 让模型直接知道本机有哪些解释器可用及其版本, 不必反复试探命令是否存在;
///   未探测到时明确写"未找到", 引导模型改用其他方式 (如 bash 内建命令)
inline std::string interpreterEnvSection(const ExecEnvInfo& env) {
    std::string out = "\n\n## Interpreters detected at startup\n";
    if (env.python.available) {
        out += fmt::format("- python: `{}` (Python {})\n", env.python.exeName, env.python.version);
    } else {
        out += "- python: NOT found in PATH — do not run `python` / `pip` commands\n";
    }
    if (env.node.available) {
        out += fmt::format("- node: `{}` (Node.js {})\n", env.node.exeName, env.node.version);
    } else {
        out += "- node: NOT found in PATH — do not run `node` / `npm` / `npx` commands\n";
    }
    out += "Use the executable names above when a command needs an interpreter.";
    return out;
}

/// Windows 命令工具 depict: 标明实际执行器 (PowerShell 及版本号, 或回退 cmd.exe)
inline std::string winCommandToolDepict(const ExecEnvInfo& env) {
    if (env.powershell.available) {
        return fmt::format(
            "Execute a Windows command via {} (PowerShell {}) and return its output.",
            env.powershell.exeName,
            env.powershell.version
        );
    }
    return "Execute a Windows command via cmd.exe and return its output.";
}

/// `command` 参数描述: boost.process v2 直传 argv 路径 (命令作为单个 `-Command` 参数)
/// - PowerShell 可用: 语法要点按 PowerShell 给出
/// - 不可用: 回退 cmd.exe, 语法要点按 cmd 给出
inline std::string winCommandProcessArgDesc(const ExecEnvInfo& env) {
    if (env.powershell.available) {
        const auto& ps  = env.powershell;
        std::string out = fmt::format(
            R"({}

The command is executed by {} (PowerShell {}) as ONE `-Command` argument — do NOT prepend `{}`, `-Command`, `powershell.exe`, or `cmd.exe /c` yourself.
Write plain PowerShell code. Syntax essentials (follow them to avoid quoting/`$` errors):
- `$name` is a variable reference: `$x = 1; Write-Output $x`.
- Double-quoted strings expand `$variables`; single-quoted strings are fully literal: `Write-Output 'a$b'` prints `a$b`.
- To embed a double quote inside a double-quoted string use backtick: "say `"hi`"", or prefer single quotes: 'say "hi"'.
- Quote paths that contain spaces or backslashes: `'C:\Program Files\app.exe'`.
- Separate statements with `;` or newlines (`&&` only works on PowerShell 7+).

## Examples:
- `Get-Process`: List processes
- `Get-ChildItem 'C:\Users'`: List a directory
- `$v = $PSVersionTable.PSVersion.ToString(); Write-Output $v`: Print PowerShell version
- `explorer.exe C:\Users`: Open File Explorer at path
- `Taskmgr.exe`: Open Task Manager
- `notepad.exe C:\file.txt`: Open file in Notepad)",
            winCommandPrefix(env),
            ps.exeName,
            ps.version,
            ps.exeName
        );
        out += interpreterEnvSection(env);
        return out;
    }

    std::string out = fmt::format(
        R"({}

The command is passed directly to `cmd.exe` — do NOT prepend `cmd.exe /c` yourself.
cmd.exe syntax essentials:
- `$` has no special meaning in cmd; environment variables use `%VAR%` (e.g. `echo %PATH%`).
- Special characters `& | < > ^` are cmd operators: quote them or escape with `^` when literal.
- Chain commands with `&` (always), `&&` (on success), `||` (on failure).

## Examples:
- `dir C:\Users`: List a directory
- `echo %USERPROFILE%`: Print user profile path
- `explorer.exe C:\Users`: Open File Explorer at path
- `Taskmgr.exe`: Open Task Manager
- `Control.exe`: Open Control Panel
- `regedit.exe`: Open Registry Editor
- `calc.exe`: Open Calculator
- `notepad.exe C:\file.txt`: Open file in Notepad)",
        winCommandPrefix(env)
    );
    out += interpreterEnvSection(env);
    return out;
}

/// `command` 参数描述: 未启用 boost.process 的 popen 回退路径 (命令经外层 shell 解析)
/// - WSL: 外层为 Linux shell, PowerShell 代码需单引号保护 / cmd 需双层引号
/// - 原生 Windows: 外层为 cmd.exe, PowerShell 代码需双引号包裹
inline std::string winCommandPopenArgDesc(const ExecEnvInfo& env) {
    if (env.powershell.available) {
        const auto& ps = env.powershell;
        std::string out;
        if (env.isWSL) {
            out = fmt::format(
                R"({}

Windows commands are executed via PowerShell ({} {}) through the Linux/WSL shell.
Format: {} -NoProfile -Command '<powershell code>'
- The outer command runs in the Linux/WSL shell: wrap the whole PowerShell code in single quotes to protect `$`, quotes and spaces.
- If the PowerShell code itself contains a single quote, escape it as `'\''` (close quote, escaped quote, reopen).
- Inside the PowerShell code use double quotes for strings.

## Examples:
- `{} -NoProfile -Command 'Get-Process'`: List processes
- `{} -NoProfile -Command 'Get-ChildItem C:\Users'`: List a directory
- `explorer.exe C:\Users`: Open File Explorer at path
- `Taskmgr.exe`: Open Task Manager)",
                winCommandPrefix(env),
                ps.exeName,
                ps.version,
                ps.exeName,
                ps.exeName,
                ps.exeName
            );
        } else {
            out = fmt::format(
                R"({}

Windows commands are executed via PowerShell ({} {}) through cmd.exe.
Format: {} -NoProfile -Command "<powershell code>"
- The command line is parsed by cmd.exe: wrap the PowerShell code in double quotes.
- Prefer single quotes for strings inside the PowerShell code to avoid cmd double-quote conflicts.

## Examples:
- `{} -NoProfile -Command "Get-Process"`: List processes
- `{} -NoProfile -Command "Get-ChildItem 'C:\Users'"`: List a directory
- `explorer.exe C:\Users`: Open File Explorer at path
- `Taskmgr.exe`: Open Task Manager)",
                winCommandPrefix(env),
                ps.exeName,
                ps.version,
                ps.exeName,
                ps.exeName,
                ps.exeName
            );
        }
        out += interpreterEnvSection(env);
        return out;
    }

    std::string out;
    if (env.isWSL) {
        out = R"(Command to execute in the Windows terminal (via WSL interop).
Current system is WSL. This tool runs the command on the Windows side via cmd.exe.
Windows commands must be invoked through `cmd.exe`. Format: `cmd.exe /c "win_command"`
- The outer command runs in the Linux/WSL shell, but `win_command` executes inside the Windows terminal.
- Wrap `win_command` in double quotes; inside it use single quotes or `^` for cmd special characters.
If the user provides a Windows path (e.g. `C:\...` or `D:\...`), convert it to a WSL path (`/mnt/c/...` or `/mnt/d/...`) for file operations, but use the original Windows path when passing to Windows executables.

## Examples:
- `cmd.exe /c "echo hello"`: Run a command in Windows CMD
- `cmd.exe /c "dir C:\Users"`: List a directory via CMD
- `explorer.exe C:\Users`: Open File Explorer at path
- `Taskmgr.exe`: Open Task Manager
- `Control.exe`: Open Control Panel
- `regedit.exe`: Open Registry Editor
- `calc.exe`: Open Calculator
- `notepad.exe C:\file.txt`: Open file in Notepad)";
    } else {
        out = R"(The Windows command to execute (executed via cmd.exe).

## Examples:
- `echo hello`: Print text
- `dir C:\Users`: List a directory
- `explorer.exe C:\Users`: Open File Explorer at path
- `Taskmgr.exe`: Open Task Manager
- `Control.exe`: Open Control Panel
- `regedit.exe`: Open Registry Editor
- `calc.exe`: Open Calculator
- `notepad.exe C:\file.txt`: Open file in Notepad)";
    }
    out += interpreterEnvSection(env);
    return out;
}

/// bash 工具 (POSIX) 提示词: depict + `command`/`all_output`/`timeout` 描述
/// - `command` 描述含系统名、WSL 标记与探测到的解释器列表
inline ExecPromptText bashToolPrompt(const ExecEnvInfo& env) {
    ExecPromptText prompt;
    prompt.depict          = std::string{kBashToolDepict};
    prompt.args["command"] = fmt::format(
        R"(The shell command to execute.
Current system: {}{}. Use standard shell/bash syntax.
The command string is passed as-is to `bash -c` (no extra escaping layer):
- `$` starts variable expansion — wrap literal `$` in single quotes (`echo 'a$b'`) or escape it (`echo \$HOME`).
- Prefer single quotes for text with spaces/special characters; use double quotes when `$` expansion is intended.
- Chain commands with `&&` / `||` / `;`; redirect with `>` / `2>&1`.{})",
        env.systemName,
        env.isWSL ? " (WSL)" : "",
        interpreterEnvSection(env)
    );
    prompt.args["all_output"] = std::string{kAllOutputArgDesc};
    prompt.args["timeout"]    = std::string{kTimeoutArgDesc};
    return prompt;
}

/// Windows 命令工具提示词: depict + `command`/`all_output`/`timeout` 描述
/// - `command` 描述的语法指引按实际执行路径选择:
///   [viaProcessSpawn] true (boost.process v2 直传 argv) 用 [winCommandProcessArgDesc],
///   false (未启用 boost.process 的 popen 回退) 用 [winCommandPopenArgDesc]
/// - 两个分支都附带探测到的解释器列表 (Windows 侧 python/node)
inline ExecPromptText windowsToolPrompt(const ExecEnvInfo& env, bool viaProcessSpawn) {
    ExecPromptText prompt;
    prompt.depict = winCommandToolDepict(env);
    prompt.args["command"]
        = viaProcessSpawn ? winCommandProcessArgDesc(env) : winCommandPopenArgDesc(env);
    prompt.args["all_output"] = std::string{kAllOutputArgDesc};
    prompt.args["timeout"]    = std::string{kTimeoutArgDesc};
    return prompt;
}

} // namespace agentxx_execmd_plugin
