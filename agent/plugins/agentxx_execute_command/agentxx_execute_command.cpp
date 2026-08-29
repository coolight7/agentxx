// agentxx_execute_command —— 命令执行工具插件
#include "agentxx_execmd_plugin.h"
#include "execute_command_impl.h"
#include <string>

using namespace agentxx_execmd_plugin;

namespace {

constexpr auto kNameBash    = "agentxx_execute_bash_command";
constexpr auto kNameWindows = "agentxx_execute_windows_command";

constexpr auto kDepictBash = "Execute a shell/bash command and return its output.";
constexpr auto kDepictWinPlaceholder =
    R"(Execute a Windows command and return its output.
The command is executed in the Windows terminal. Do NOT prepend any wrapper (`cmd.exe /c`, `powershell.exe -Command`, ...) — write the plain command; the executor is selected automatically.)";

const char* kAllOutputDesc =
    R"(Default `true`.
`true`: Always return stdout and stderr output.
`false`: Only return output when the command fails.)";
const char* kTimeoutDesc
    = "Default `60` seconds. Execution timeout in seconds. Set `0` for no limit.";
const char* kBashCommandDesc =
    R"(The shell command to execute.
The command string is passed as-is to `bash -c` (no extra escaping layer):
- `$` starts variable expansion — wrap literal `$` in single quotes (`echo 'a$b'`) or escape it (`echo \$HOME`).
- Prefer single quotes for text with spaces/special characters; use double quotes when `$` expansion is intended.
- Chain commands with `&&` / `||` / `;`; redirect with `>` / `2>&1`.)";

std::string argDesc(const agentxx::kit::ToolPromptText& p, const char* key, const std::string& fallback) {
    auto it = p.args.find(key);
    if (it != p.args.end() && !it->second.empty()) {
        return it->second;
    }
    return fallback;
}

std::string schemaBash(PluginCtx* ctx) {
    auto p = ctx->toolPrompt(kNameBash);
    return neograph::json{
        {"type", "object"},
        {"properties", {
            {"command", {
                {"type", "string"},
                {"description", argDesc(p, "command", kBashCommandDesc)}
            }},
            {"timeout", {
                {"type", "integer"},
                {"default", 60},
                {"description", argDesc(p, "timeout", kTimeoutDesc)}
            }},
            {"all_output", {
                {"type", "boolean"},
                {"default", true},
                {"description", argDesc(p, "all_output", kAllOutputDesc)}
            }}
        }},
        {"required", neograph::json::array({"command"})}
    }.dump();
}

std::string schemaWindows(PluginCtx* ctx) {
    auto p = ctx->toolPrompt(kNameWindows);
    return neograph::json{
        {"type", "object"},
        {"properties", {
            {"command", {
                {"type", "string"},
                {"description", argDesc(p, "command", "The Windows command to execute.")}
            }},
            {"timeout", {
                {"type", "integer"},
                {"default", 60},
                {"description", argDesc(p, "timeout", kTimeoutDesc)}
            }},
            {"all_output", {
                {"type", "boolean"},
                {"default", true},
                {"description", argDesc(p, "all_output", kAllOutputDesc)}
            }}
        }},
        {"required", neograph::json::array({"command"})}
    }.dump();
}

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    return agentxx::plugin_guard::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
        static const AgentxxPluginInfo info{
            AGENTXX_PLUGIN_API_VERSION,
            AGENTXX_SV("agentxx_execute_command"),
            AGENTXX_SV("1.0.0"),
            AGENTXX_SV("Execute system commands (bash/windows terminal) with timeout/cancellation"),
        };
        return &info;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT int agentxx_plugin_create(const AgentxxHost* host, void** plugin_ctx) {
    PluginCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
        [&raw](const char* msg) noexcept { ctxGuardLogger(raw)(msg); },
        -1,
        [&]() -> int {
        if (!host || !host->vtable || !plugin_ctx) {
            return -1;
        }
        auto ctx  = std::make_unique<PluginCtx>();
        ctx->init(host);
        raw = ctx.get();

        if (!ctx->iface.tools || !ctx->iface.tools->register_tool) {
            return -1;
        }

#if defined(_WIN32)
        auto pWin = ctx->toolPrompt(kNameWindows);
        std::string depictWin = pWin.depict.empty() ? kDepictWinPlaceholder : pWin.depict;

#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
        // 异步管线改为 blocking_tool + 临时 io_context 同步驱动（无私有 Reactor 线程）
        agentxx::kit::blocking_tool(
            *ctx,
            kNameWindows,
            depictWin,
            schemaWindows(ctx.get()),
            [](PluginCtx& c, std::string_view args_json, std::string_view tid, std::string_view workDir, volatile int* cancel_flag) -> std::string {
                auto arguments = args_json.empty() ? neograph::json::object() : neograph::json::parse(args_json);
                auto isCancelled = [&c, tid, cancel_flag]() -> bool {
                    if (cancel_flag && *cancel_flag != 0) return true;
                    return c.sessionCancelled(agentxx_plugin_sv(tid.data(), tid.size()));
                };
                StoreFn storeFn = nullptr;
                if (!tid.empty() && c.iface.session && c.iface.session->add_share_store) {
                    std::string tidCopy(tid);
                    storeFn = [&c, tidCopy](std::string_view content) -> long long {
                        return c.addShareStore(
                            agentxx_plugin_sv(tidCopy.data(), tidCopy.size()),
                            content
                        );
                    };
                }
                asio::io_context io;
                std::string result;
                std::exception_ptr ep;
                asio::co_spawn(
                    io,
                    [&]() -> asio::awaitable<void> {
                        try {
                            result = co_await windowsExecuteAsync(arguments, std::string(workDir), isCancelled, storeFn);
                        } catch (...) {
                            ep = std::current_exception();
                        }
                    },
                    asio::detached
                );
                io.run();
                if (ep) std::rethrow_exception(ep);
                return result;
            },
            0,
            AGENTXX_TOOL_FLAG_NONE
        );
#else
        agentxx::kit::blocking_tool(
            *ctx,
            kNameWindows,
            depictWin,
            schemaWindows(ctx.get()),
            [](PluginCtx& c, std::string_view args_json, std::string_view tid, std::string_view workDir, volatile int* cancel_flag) -> std::string {
                auto arguments = args_json.empty() ? neograph::json::object() : neograph::json::parse(args_json);
                auto isCancelled = [&c, tid, cancel_flag]() -> bool {
                    if (cancel_flag && *cancel_flag != 0) return true;
                    return c.sessionCancelled(agentxx_plugin_sv(tid.data(), tid.size()));
                };
                StoreFn storeFn = nullptr;
                if (!tid.empty() && c.iface.session && c.iface.session->add_share_store) {
                    std::string tidCopy(tid);
                    storeFn = [&c, tidCopy](std::string_view content) -> long long {
                        return c.addShareStore(
                            agentxx_plugin_sv(tidCopy.data(), tidCopy.size()),
                            content
                        );
                    };
                }
                return windowsExecute(arguments, std::string(workDir), isCancelled, storeFn);
            },
            0,
            AGENTXX_TOOL_FLAG_NONE
        );
#endif

#else // Linux / POSIX
        auto pBash = ctx->toolPrompt(kNameBash);
        std::string depictBash = pBash.depict.empty() ? kDepictBash : pBash.depict;

#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
        agentxx::kit::blocking_tool(
            *ctx,
            kNameBash,
            depictBash,
            schemaBash(ctx.get()),
            [](PluginCtx& c, std::string_view args_json, std::string_view tid, std::string_view workDir, volatile int* cancel_flag) -> std::string {
                auto arguments = args_json.empty() ? neograph::json::object() : neograph::json::parse(args_json);
                auto isCancelled = [&c, tid, cancel_flag]() -> bool {
                    if (cancel_flag && *cancel_flag != 0) return true;
                    return c.sessionCancelled(agentxx_plugin_sv(tid.data(), tid.size()));
                };
                StoreFn storeFn = nullptr;
                if (!tid.empty() && c.iface.session && c.iface.session->add_share_store) {
                    std::string tidCopy(tid);
                    storeFn = [&c, tidCopy](std::string_view content) -> long long {
                        return c.addShareStore(
                            agentxx_plugin_sv(tidCopy.data(), tidCopy.size()),
                            content
                        );
                    };
                }
                asio::io_context io;
                std::string result;
                std::exception_ptr ep;
                asio::co_spawn(
                    io,
                    [&]() -> asio::awaitable<void> {
                        try {
                            result = co_await bashExecuteAsync(arguments, std::string(workDir), isCancelled, storeFn);
                        } catch (...) {
                            ep = std::current_exception();
                        }
                    },
                    asio::detached
                );
                io.run();
                if (ep) std::rethrow_exception(ep);
                return result;
            },
            0,
            AGENTXX_TOOL_FLAG_NONE
        );
#else
        agentxx::kit::blocking_tool(
            *ctx,
            kNameBash,
            depictBash,
            schemaBash(ctx.get()),
            [](PluginCtx& c, std::string_view args_json, std::string_view tid, std::string_view workDir, volatile int* cancel_flag) -> std::string {
                auto arguments = args_json.empty() ? neograph::json::object() : neograph::json::parse(args_json);
                auto isCancelled = [&c, tid, cancel_flag]() -> bool {
                    if (cancel_flag && *cancel_flag != 0) return true;
                    return c.sessionCancelled(agentxx_plugin_sv(tid.data(), tid.size()));
                };
                StoreFn storeFn = nullptr;
                if (!tid.empty() && c.iface.session && c.iface.session->add_share_store) {
                    std::string tidCopy(tid);
                    storeFn = [&c, tidCopy](std::string_view content) -> long long {
                        return c.addShareStore(
                            agentxx_plugin_sv(tidCopy.data(), tidCopy.size()),
                            content
                        );
                    };
                }
                return bashExecute(arguments, std::string(workDir), isCancelled, storeFn);
            },
            0,
            AGENTXX_TOOL_FLAG_NONE
        );
#endif

#endif

        *plugin_ctx = ctx.release();
        return 0;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    agentxx::plugin_guard::guardCallVoid(ctxGuardLogger(ctx), [&] {
        delete ctx;
    });
}
