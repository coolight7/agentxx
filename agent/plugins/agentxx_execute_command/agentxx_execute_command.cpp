// agentxx_execute_command —— 命令执行工具插件
#include "agentxx_execmd_plugin.h"
#include "execute_command_impl.h"
#include <string>

using namespace agentxx_execmd_plugin;

namespace {

constexpr std::string_view kNameBash    = "agentxx_execute_bash_command";
constexpr std::string_view kNameWindows = "agentxx_execute_windows_command";

constexpr std::string_view kDepictBash = "Execute a shell/bash command and return its output.";
constexpr std::string_view kDepictWinPlaceholder =
    R"(Execute a Windows command and return its output.
The command is executed in the Windows terminal. Do NOT prepend any wrapper (`cmd.exe /c`, `powershell.exe -Command`, ...) — write the plain command; the executor is selected automatically.)";

std::string_view kAllOutputDesc =
    R"(Default `true`.
`true`: Always return stdout and stderr output.
`false`: Only return output when the command fails.)";
std::string_view kTimeoutDesc
    = "Default `60` seconds. Execution timeout in seconds. Set `0` for no limit.";
std::string_view kBashCommandDesc =
    R"(The shell command to execute.
The command string is passed as-is to `bash -c` (no extra escaping layer):
- `$` starts variable expansion — wrap literal `$` in single quotes (`echo 'a$b'`) or escape it (`echo \$HOME`).
- Prefer single quotes for text with spaces/special characters; use double quotes when `$` expansion is intended.
- Chain commands with `&&` / `||` / `;`; redirect with `>` / `2>&1`.)";

std::string schemaBash(PluginCtx* ctx) {
    auto p = ctx->toolPrompt(kNameBash);
    return neograph::json{
        {"type", "object"},
        {
         "properties", {{
                 "command",
                 {
                     {"type", "string"},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(p, "command", kBashCommandDesc),
                     },
                 },
             },
             {
                 "timeout",
                 {
                     {"type", "integer"},
                     {"default", 60},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(p, "timeout", kTimeoutDesc),
                     },
                 },
             },
             {
                 "all_output",
                 {
                     {"type", "boolean"},
                     {"default", true},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(p, "all_output", kAllOutputDesc),
                     },
                 },
             }},
         },
        {"required", neograph::json::array({"command"})}
    }.dump();
}

std::string schemaWindows(PluginCtx* ctx) {
    auto p = ctx->toolPrompt(kNameWindows);
    return neograph::json{
        {"type", "object"},
        {
         "properties", {{
                 "command",
                 {
                     {"type", "string"},
                     {
                         "description",
                         agentxx::plugin::
                             toolPromptArgDesc(p, "command", "The Windows command to execute."),
                     },
                 },
             },
             {
                 "timeout",
                 {
                     {"type", "integer"},
                     {"default", 60},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(p, "timeout", kTimeoutDesc),
                     },
                 },
             },
             {
                 "all_output",
                 {
                     {"type", "boolean"},
                     {"default", true},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(p, "all_output", kAllOutputDesc),
                     },
                 },
             }},
         },
        {"required", neograph::json::array({"command"})}
    }.dump();
}

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
            static const AgentxxPluginInfo info{
                AGENTXX_PLUGIN_API_VERSION,
                0,
                agentxx::plugin::PluginStringView::fromCstr("agentxx_execute_command"),
                agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
                agentxx::plugin::PluginStringView::fromCstr(
                    "Execute system commands (bash/windows terminal) with timeout/cancellation"
                ),
            };
            return &info;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_agent_create(const AgentxxPluginHost* host, void** plugin_ctx) {
    PluginCtx* raw = nullptr;
    return agentxx::plugin::guardCall(
        [&raw](const char* msg) noexcept {
            ctxGuardLogger(raw)(msg);
        },
        -1,
        [&]() -> int {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            auto ctx = std::make_unique<PluginCtx>();
            ctx->init(host);
            raw = ctx.get();

            if (!ctx->iface.tools || !ctx->iface.tools->register_tool) {
                return -1;
            }

#if XX_IS_WIN_D
            auto        pWin = ctx->toolPrompt(kNameWindows);
            std::string depictWin
                = pWin.depict.empty() ? std::string{kDepictWinPlaceholder} : pWin.depict;

#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
            // 异步管线改为 blocking_tool + 临时 io_context 同步驱动（无私有 Reactor 线程）
            agentxx::plugin::blocking_tool(
                *ctx,
                kNameWindows,
                depictWin,
                schemaWindows(ctx.get()),
                [](PluginCtx&       c,
                   std::string_view args_json,
                   std::string_view tid,
                   std::string_view workDir,
                   volatile int*    cancel_flag) -> std::string {
                    auto arguments   = args_json.empty() ? neograph::json::object()
                                                         : neograph::json::parse(args_json);
                    auto isCancelled = [&c, tid, cancel_flag]() -> bool {
                        if (cancel_flag && *cancel_flag != 0)
                            return true;
                        return c.sessionCancelled(
                            agentxx::plugin::PluginStringView::from(tid.data(), tid.size())
                        );
                    };
                    StoreFn storeFn = nullptr;
                    if (!tid.empty() && c.iface.session && c.iface.session->add_share_store) {
                        std::string tidCopy(tid);
                        storeFn = [&c, tidCopy](std::string_view content) -> long long {
                            return c.addShareStore(
                                agentxx::plugin::PluginStringView::from(
                                    tidCopy.data(),
                                    tidCopy.size()
                                ),
                                content
                            );
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
                                    arguments,
                                    std::string(workDir),
                                    isCancelled,
                                    storeFn
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
                },
                0,
                AGENTXX_PLUGIN_TOOL_FLAG_NONE
            );
#else
            agentxx::plugin::blocking_tool(
                *ctx,
                kNameWindows,
                depictWin,
                schemaWindows(ctx.get()),
                [](PluginCtx&       c,
                   std::string_view args_json,
                   std::string_view tid,
                   std::string_view workDir,
                   volatile int*    cancel_flag) -> std::string {
                    auto arguments   = args_json.empty() ? neograph::json::object()
                                                         : neograph::json::parse(args_json);
                    auto isCancelled = [&c, tid, cancel_flag]() -> bool {
                        if (cancel_flag && *cancel_flag != 0)
                            return true;
                        return c.sessionCancelled(
                            agentxx::plugin::PluginStringView::from(tid.data(), tid.size())
                        );
                    };
                    StoreFn storeFn = nullptr;
                    if (!tid.empty() && c.iface.session && c.iface.session->add_share_store) {
                        std::string tidCopy(tid);
                        storeFn = [&c, tidCopy](std::string_view content) -> long long {
                            return c.addShareStore(
                                agentxx::plugin::PluginStringView::from(
                                    tidCopy.data(),
                                    tidCopy.size()
                                ),
                                content
                            );
                        };
                    }
                    return windowsExecute(arguments, std::string(workDir), isCancelled, storeFn);
                },
                0,
                AGENTXX_PLUGIN_TOOL_FLAG_NONE
            );
#endif

#else // Linux / POSIX
            auto pBash      = ctx->toolPrompt(kNameBash);
            auto depictBash = pBash.depict.empty() ? std::string{kDepictBash} : pBash.depict;

#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
            agentxx::plugin::blocking_tool(
                *ctx,
                kNameBash,
                depictBash,
                schemaBash(ctx.get()),
                [](PluginCtx&       c,
                   std::string_view args_json,
                   std::string_view tid,
                   std::string_view workDir,
                   volatile int*    cancel_flag) -> std::string {
                    auto arguments   = args_json.empty() ? neograph::json::object()
                                                         : neograph::json::parse(args_json);
                    auto isCancelled = [&c, tid, cancel_flag]() -> bool {
                        if (cancel_flag && *cancel_flag != 0)
                            return true;
                        return c.sessionCancelled(
                            agentxx::plugin::PluginStringView::from(tid.data(), tid.size())
                        );
                    };
                    StoreFn storeFn = nullptr;
                    if (!tid.empty() && c.iface.session && c.iface.session->add_share_store) {
                        std::string tidCopy(tid);
                        storeFn = [&c, tidCopy](std::string_view content) -> long long {
                            return c.addShareStore(
                                agentxx::plugin::PluginStringView::from(
                                    tidCopy.data(),
                                    tidCopy.size()
                                ),
                                content
                            );
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
                                    arguments,
                                    std::string(workDir),
                                    isCancelled,
                                    storeFn
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
                },
                0,
                AGENTXX_PLUGIN_TOOL_FLAG_NONE
            );
#else
            agentxx::plugin::blocking_tool(
                *ctx,
                kNameBash,
                depictBash,
                schemaBash(ctx.get()),
                [](PluginCtx&       c,
                   std::string_view args_json,
                   std::string_view tid,
                   std::string_view workDir,
                   volatile int*    cancel_flag) -> std::string {
                    auto arguments   = args_json.empty() ? neograph::json::object()
                                                         : neograph::json::parse(args_json);
                    auto isCancelled = [&c, tid, cancel_flag]() -> bool {
                        if (cancel_flag && *cancel_flag != 0)
                            return true;
                        return c.sessionCancelled(
                            agentxx::plugin::PluginStringView::from(tid.data(), tid.size())
                        );
                    };
                    StoreFn storeFn = nullptr;
                    if (!tid.empty() && c.iface.session && c.iface.session->add_share_store) {
                        std::string tidCopy(tid);
                        storeFn = [&c, tidCopy](std::string_view content) -> long long {
                            return c.addShareStore(
                                agentxx::plugin::PluginStringView::from(
                                    tidCopy.data(),
                                    tidCopy.size()
                                ),
                                content
                            );
                        };
                    }
                    return bashExecute(arguments, std::string(workDir), isCancelled, storeFn);
                },
                0,
                AGENTXX_PLUGIN_TOOL_FLAG_NONE
            );
#endif

#endif

            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(ctxGuardLogger(ctx), [&] {
        delete ctx;
    });
}

/* ==================== Client 侧入口 ==================== */

namespace {

struct ClientCtx {
    const AgentxxPluginHost*      host = nullptr;
    agentxx::plugin::ClientIfaces iface{};

    void logErr(const char* m) const noexcept {
        agentxx::plugin::logTo(host, iface.log, 4, "agentxx_execute_command", m ? m : "");
    }
};

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxClientPluginInfo* agentxx_plugin_client_get_info(void
) {
    static const AgentxxClientPluginInfo info{
        AGENTXX_CLIENT_PLUGIN_API_VERSION,
        0,
        agentxx::plugin::PluginStringView::fromCstr("agentxx_execute_command"),
        agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
        agentxx::plugin::PluginStringView::fromCstr(
            "Command execution tools specialized UI renderer"
        ),
    };
    return &info;
}

extern "C" AGENTXX_PLUGIN_EXPORT int32_t AGENTXX_PLUGIN_CALL
    agentxx_plugin_client_create(const AgentxxPluginHost* host, void** plugin_ctx) {
    ClientCtx* raw = nullptr;
    return agentxx::plugin::guardCall(
        [&raw](const char* m) noexcept {
            if (raw) {
                raw->logErr(m);
            }
        },
        -1,
        [&]() -> int32_t {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            auto ctx   = std::make_unique<ClientCtx>();
            ctx->host  = host;
            ctx->iface = agentxx::plugin::ClientIfaces::query(host);
            raw        = ctx.get();

            if (!ctx->iface.ui) {
                *plugin_ctx = ctx.release();
                return 0;
            }

            // 1. Bash (预设模版)
            agentxx::plugin::registerToolTemplate(
                host,
                ctx->iface.ui,
                kNameBash,
                "Bash",
                "command"
            );

            // 2. Windows (预设模版)
            agentxx::plugin::registerToolTemplate(
                host,
                ctx->iface.ui,
                kNameWindows,
                "Bash",
                "command"
            );

            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void AGENTXX_PLUGIN_CALL
    agentxx_plugin_client_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<ClientCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(
        [ctx](const char* m) noexcept {
            if (ctx) {
                ctx->logErr(m);
            }
        },
        [&] {
            delete ctx;
        }
    );
}
