/// agentxx_execute_command —— 命令执行工具插件
#include "agentxx_execmd_plugin.h"
#include "execute_command_impl.h"
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
        blocking_tool(
            ctx,
            kNameWindows,
            kDepictWinPlaceholder,
            winSchema,
            [](ExecPluginCtx&    c,
               std::string_view  args_json,
               std::string_view  tid,
               std::string_view  workDir,
               volatile int32_t* cancel_flag) -> std::string {
                ArgReader   args(args_json);
                std::string tidStr(tid);
                if (cancel_flag && *cancel_flag != 0) {
                    c.cancelRegistry.cancel(tidStr);
                }
                StoreFn storeFn = nullptr;
                if (!tid.empty() && c.iface.session && c.iface.session->add_share_store) {
                    storeFn = [&c, tidStr](std::string_view content) -> long long {
                        return c.addShareStore(tidStr, content);
                    };
                }
                asio::io_context   io;
                std::string        result;
                std::exception_ptr ep;
                asio::co_spawn(
                    io,
                    [&]() -> asio::awaitable<void> {
                        try {
                            result = co_await windowsExecuteAsync(
                                args.raw(),
                                std::string(workDir),
                                [&c, tidStr, cancel_flag]() -> bool {
                                    if (cancel_flag && *cancel_flag != 0)
                                        return true;
                                    return c.cancelRegistry.isCancelled(tidStr);
                                },
                                storeFn,
                                &c.cancelRegistry,
                                tidStr
                            );
                        } catch (...) {
                            ep = std::current_exception();
                        }
                    },
                    asio::detached
                );
                io.run();
                if (ep)
                    std::rethrow_exception(ep);
                return result;
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
               volatile int32_t* cancel_flag) -> std::string {
                ArgReader   args(args_json);
                std::string tidStr(tid);
                if (cancel_flag && *cancel_flag != 0) {
                    c.cancelRegistry.cancel(tidStr);
                }
                auto isCancelled = [&c, tidStr, cancel_flag]() -> bool {
                    if (cancel_flag && *cancel_flag != 0)
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
        blocking_tool(
            ctx,
            kNameBash,
            kDepictBash,
            bashSchema,
            [](ExecPluginCtx&    c,
               std::string_view  args_json,
               std::string_view  tid,
               std::string_view  workDir,
               volatile int32_t* cancel_flag) -> std::string {
                ArgReader   args(args_json);
                std::string tidStr(tid);
                if (cancel_flag && *cancel_flag != 0) {
                    c.cancelRegistry.cancel(tidStr);
                }
                StoreFn storeFn = nullptr;
                if (!tid.empty() && c.iface.session && c.iface.session->add_share_store) {
                    storeFn = [&c, tidStr](std::string_view content) -> long long {
                        return c.addShareStore(tidStr, content);
                    };
                }
                asio::io_context   io;
                std::string        result;
                std::exception_ptr ep;
                asio::co_spawn(
                    io,
                    [&]() -> asio::awaitable<void> {
                        try {
                            result = co_await bashExecuteAsync(
                                args.raw(),
                                std::string(workDir),
                                [&c, tidStr, cancel_flag]() -> bool {
                                    if (cancel_flag && *cancel_flag != 0)
                                        return true;
                                    return c.cancelRegistry.isCancelled(tidStr);
                                },
                                storeFn,
                                &c.cancelRegistry,
                                tidStr
                            );
                        } catch (...) {
                            ep = std::current_exception();
                        }
                    },
                    asio::detached
                );
                io.run();
                if (ep)
                    std::rethrow_exception(ep);
                return result;
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
               volatile int32_t* cancel_flag) -> std::string {
                ArgReader   args(args_json);
                std::string tidStr(tid);
                if (cancel_flag && *cancel_flag != 0) {
                    c.cancelRegistry.cancel(tidStr);
                }
                auto isCancelled = [&c, tidStr, cancel_flag]() -> bool {
                    if (cancel_flag && *cancel_flag != 0)
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

AGENTXX_PLUGIN_AGENT_EXPORT(
    ExecPluginCtx,
    "agentxx_execute_command",
    "1.0.0",
    "Execute system commands (bash/windows terminal) with timeout/cancellation",
    setupExecPlugin
);

struct ExecClientCtx : public ClientPluginBase {};

AGENTXX_PLUGIN_CLIENT_EXPORT(
    ExecClientCtx,
    "agentxx_execute_command",
    "1.0.0",
    "Command execution specialized UI template renderer",
    [](ExecClientCtx& ctx) -> int32_t {
        ctx.registerTemplate(kNameBash, "Bash", "command");
        ctx.registerTemplate(kNameWindows, "Cmd", "command");
        return 0;
    }
);
