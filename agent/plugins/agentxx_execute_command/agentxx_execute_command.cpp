/// agentxx_execute_command —— 命令执行工具插件
///
/// 两个工具的实现体都是 asio 协程 (`bashExecuteAsync` / `windowsExecuteAsync`):
/// 子进程管道绑定到协程 executor, 等待的是插件本地 reactor 上的管道就绪事件,
/// 因此注册为**声明式受控轮询**工具 (`polled_tool`, 见 plugin_kit.h)。
/// 并发多条命令共享同一条 polled 驱动序列与同一个本地 reactor, 不再每条命令
/// 占死一个宿主工作线程直到超时 (会话取消经 CancelRegistry 事件驱动 kill 进程组)。
/// 只有关闭 `AGENTXX_ENABLE_BOOST_PROCESS` 的 popen 回退实现仍是阻塞函数,
/// 继续走 `blocking_tool` (offload 工作线程)。
#include "agentxx_execmd_plugin.h"
#include "execute_command_impl.h"
#include "asio/awaitable.hpp"
#include <string>

using namespace agentxx_execmd_plugin;
using namespace agentxx::plugin;

namespace {

constexpr std::string_view kNameBash    = "agentxx_execute_bash_command";
constexpr std::string_view kNameWindows = "agentxx_execute_windows_command";

constexpr std::string_view kDepictBash = "Execute a shell/bash command and return its output.";
constexpr std::string_view kDepictWinPlaceholder =
    R"(Execute a Windows command and return its output.
The command is executed in the Windows terminal. Do NOT prepend any wrapper (`cmd.exe /c`, `powershell.exe -Command`, ...) — write the plain command; the executor is selected automatically.)";

constexpr std::string_view kAllOutputDesc =
    R"(Default `true`.
`true`: Always return stdout and stderr output.
`false`: Only return output when the command fails.)";
constexpr std::string_view kTimeoutDesc
    = "Default `60` seconds. Execution timeout in seconds. Set `0` for no limit.";
constexpr std::string_view kBashCommandDesc =
    R"(The shell command to execute.
The command string is passed as-is to `bash -c` (no extra escaping layer):
- `$` starts variable expansion — wrap literal `$` in single quotes (`echo 'a$b'`) or escape it (`echo \$HOME`).
- Prefer single quotes for text with spaces/special characters; use double quotes when `$` expansion is intended.
- Chain commands with `&&` / `||` / `;`; redirect with `>` / `2>&1`.)";

} // namespace

struct ExecPluginCtx : public PluginBase {};

static int32_t setupExecPlugin(ExecPluginCtx& ctx) {
#if XX_IS_WIN_D
        auto winSchema
            = ctx.schema(kNameWindows)
                  .string("command", "The Windows command to execute.", /*required=*/true)
                  .integer("timeout", kTimeoutDesc, false, 60)
                  .boolean("all_output", kAllOutputDesc, false, true)
                  .build();

#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
        polled_tool(
            ctx,
            kNameWindows,
            kDepictWinPlaceholder,
            winSchema,
            [](ExecPluginCtx&    c,
               std::string_view  args_json,
               std::string_view  tid,
               std::string_view  workDir,
               const AgentxxPluginCancelToken* cancel_token) -> asio::awaitable<std::string> {
                ArgReader   args(args_json);
                std::string tidStr(tid);
                if (agentxx_plugin_cancel_is_requested(cancel_token)) {
                    c.cancelRegistry.cancel(tidStr);
                }
                auto isCancelled = [&c, tidStr, cancel_token]() -> bool {
                    if (agentxx_plugin_cancel_is_requested(cancel_token)) {
                        return true;
                    }
                    return c.cancelRegistry.isCancelled(tidStr);
                };
                StoreFn storeFn = nullptr;
                if (!tidStr.empty() && c.iface.session && c.iface.session->add_share_store) {
                    storeFn = [&c, tidStr](std::string_view content) -> long long {
                        return c.addShareStore(tidStr, content);
                    };
                }
                // 子进程管道/计时器绑定到本协程的 executor (= 插件本地 reactor),
                // 由受控轮询推进; workDir 用局部量保证整个 co_await 期间有效。
                std::string workDirStr(workDir);
                co_return co_await windowsExecuteAsync(
                    args.raw(),
                    workDirStr,
                    isCancelled,
                    storeFn,
                    &c.cancelRegistry,
                    tidStr
                );
            }
        );
#else
        blocking_tool(
            ctx,
            kNameWindows,
            kDepictWinPlaceholder,
            winSchema,
            [](ExecPluginCtx&    c,
               std::string_view  args_json,
               std::string_view  tid,
               std::string_view  workDir,
               const AgentxxPluginCancelToken* cancel_token) -> std::string {
                ArgReader   args(args_json);
                std::string tidStr(tid);
                if (agentxx_plugin_cancel_is_requested(cancel_token)) {
                    c.cancelRegistry.cancel(tidStr);
                }
                auto isCancelled = [&c, tidStr, cancel_token]() -> bool {
                    if (agentxx_plugin_cancel_is_requested(cancel_token))
                        return true;
                    return c.cancelRegistry.isCancelled(tidStr);
                };
                StoreFn storeFn = nullptr;
                if (!tid.empty() && c.iface.session && c.iface.session->add_share_store) {
                    storeFn = [&c, tidStr](std::string_view content) -> long long {
                        return c.addShareStore(tidStr, content);
                    };
                }
                return windowsExecute(
                    args.raw(),
                    std::string(workDir),
                    isCancelled,
                    storeFn,
                    &c.cancelRegistry,
                    tidStr
                );
            }
        );
#endif

#else // Linux / POSIX
        auto bashSchema = ctx.schema(kNameBash)
                              .string("command", kBashCommandDesc, /*required=*/true)
                              .integer("timeout", kTimeoutDesc, false, 60)
                              .boolean("all_output", kAllOutputDesc, false, true)
                              .build();

#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
        polled_tool(
            ctx,
            kNameBash,
            kDepictBash,
            bashSchema,
            [](ExecPluginCtx&    c,
               std::string_view  args_json,
               std::string_view  tid,
               std::string_view  workDir,
               const AgentxxPluginCancelToken* cancel_token) -> asio::awaitable<std::string> {
                ArgReader   args(args_json);
                std::string tidStr(tid);
                if (agentxx_plugin_cancel_is_requested(cancel_token)) {
                    c.cancelRegistry.cancel(tidStr);
                }
                auto isCancelled = [&c, tidStr, cancel_token]() -> bool {
                    if (agentxx_plugin_cancel_is_requested(cancel_token)) {
                        return true;
                    }
                    return c.cancelRegistry.isCancelled(tidStr);
                };
                StoreFn storeFn = nullptr;
                if (!tidStr.empty() && c.iface.session && c.iface.session->add_share_store) {
                    storeFn = [&c, tidStr](std::string_view content) -> long long {
                        return c.addShareStore(tidStr, content);
                    };
                }
                // 子进程管道/计时器绑定到本协程的 executor (= 插件本地 reactor),
                // 由受控轮询推进; workDir 用局部量保证整个 co_await 期间有效。
                std::string workDirStr(workDir);
                co_return co_await bashExecuteAsync(
                    args.raw(),
                    workDirStr,
                    isCancelled,
                    storeFn,
                    &c.cancelRegistry,
                    tidStr
                );
            }
        );
#else
        blocking_tool(
            ctx,
            kNameBash,
            kDepictBash,
            bashSchema,
            [](ExecPluginCtx&    c,
               std::string_view  args_json,
               std::string_view  tid,
               std::string_view  workDir,
               const AgentxxPluginCancelToken* cancel_token) -> std::string {
                ArgReader   args(args_json);
                std::string tidStr(tid);
                if (agentxx_plugin_cancel_is_requested(cancel_token)) {
                    c.cancelRegistry.cancel(tidStr);
                }
                auto isCancelled = [&c, tidStr, cancel_token]() -> bool {
                    if (agentxx_plugin_cancel_is_requested(cancel_token))
                        return true;
                    return c.cancelRegistry.isCancelled(tidStr);
                };
                StoreFn storeFn = nullptr;
                if (!tid.empty() && c.iface.session && c.iface.session->add_share_store) {
                    storeFn = [&c, tidStr](std::string_view content) -> long long {
                        return c.addShareStore(tidStr, content);
                    };
                }
                return bashExecute(
                    args.raw(),
                    std::string(workDir),
                    isCancelled,
                    storeFn,
                    &c.cancelRegistry,
                    tidStr
                );
            }
        );
#endif
#endif
        return 0;
    }

static void* execStart(
    ExecPluginCtx& ctx, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString* error
) {
    if (!notify) {
        if (error) {
            PluginString::set(ctx.host, error, "agentxx_execute_command start: notify required");
        }
        return nullptr;
    }
    if (setupExecPlugin(ctx) != 0) {
        if (error) {
            PluginString::set(
                ctx.host,
                error,
                "agentxx_execute_command start: registration failed"
            );
        }
        return nullptr;
    }
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

static void* execStop(
    ExecPluginCtx&, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString*
) {
    // 无自管线程/定时器; 注册记录由宿主在 stop 后统一撤销。
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_EXPORT(
    ExecPluginCtx,
    "agentxx_execute_command",
    "1.0.0",
    "Execute system commands (bash/windows terminal) with timeout/cancellation",
    execStart,
    execStop
);

struct ExecClientCtx : public ClientPluginBase {};

/// client 侧注册事务 (start 的实际内容)。
static int32_t setupExecClient(ExecClientCtx& ctx) {
        ctx.registerTemplate(kNameBash, "Bash", "command");
        ctx.registerTemplate(kNameWindows, "Cmd", "command");
        return 0;
}

static void* execClientStart(
    ExecClientCtx& ctx, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString* error
) {
    if (!notify) {
        if (error) {
            PluginString::set(
                ctx.host,
                error,
                "agentxx_execute_command client start: notify required"
            );
        }
        return nullptr;
    }
    if (setupExecClient(ctx) != 0) {
        if (error) {
            PluginString::set(
                ctx.host,
                error,
                "agentxx_execute_command client start: registration failed"
            );
        }
        return nullptr;
    }
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

static void* execClientStop(
    ExecClientCtx&, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString*
) {
    // UI 注册记录由宿主在 stop 后统一撤销。
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_CLIENT_EXPORT(
    ExecClientCtx,
    "agentxx_execute_command",
    "1.0.0",
    "Command execution specialized UI template renderer",
    execClientStart,
    execClientStop
);
