// agentxx_execute_command —— 命令执行工具插件
// - 从 libagentxx src/tools/execute_command 拆分独立 (同名同行为):
//   - agentxx_execute_bash_command    (原 ExecuteBashCommandTool)
//   - agentxx_execute_windows_command (原 ExecuteWindowsCommandTool)
// - 子进程初始工作目录经宿主 get_work_dir (会话工作目录); 取消 watcher 经
//   agentxx.agent.cancel 接口轮询会话取消令牌 (与原内置工具行为一致)
#include "agentxx_execmd_plugin.h"
#include "execute_command_impl.h"
#include <cstring>
#include <string>
#include <vector>

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

std::string argDesc(const ToolPromptText& p, const char* key, const std::string& fallback) {
    auto it = p.args.find(key);
    if (it != p.args.end() && !it->second.empty()) {
        return it->second;
    }
    return fallback;
}

/// C ABI execute 包装: 解析参数 JSON → 调用实现 → 结果 strdup (异常不外泄)
/// - workDir/cancel 经宿主接口表在调用时取值 (会话级动态)
/// - 取消双通道: cancel_flag (宿主 op 驱动器置位) + 会话取消令牌轮询
template<auto ExecFn>
char* wrapExecute(
    void*                   user_data,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView thread_id,
    AgentxxPluginStringView tool_call_id,
    volatile int*           cancel_flag,
    char**                  error_out
) {
    (void)user_data;
    (void)tool_call_id;
    try {
        std::string argsStr(args_json.data ? args_json.data : "", args_json.size);
        auto arguments = argsStr.empty() ? neograph::json::object() : neograph::json::parse(argsStr);
        // 会话取消查询回调: 捕获 thread_id 视图内容 (回调生命周期内有效);
        // cancel_flag 置位时同样视为取消 (宿主超时/放弃路径触发)
        std::string tid{thread_id.data ? thread_id.data : "", thread_id.size};
        auto isCancelled = [tid, cancel_flag]() -> bool {
            if (cancel_flag && *cancel_flag != 0) {
                return true;
            }
            return isSessionCancelled(agentxx_plugin_sv(tid.data(), tid.size()));
        };
        auto workDir = readWorkDir(agentxx_plugin_sv(tid.data(), tid.size()));
        auto result  = ExecFn(arguments, workDir, isCancelled);
        return pluginStrdup(result.c_str());
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = pluginStrdup(ex.what());
        }
        return nullptr;
    } catch (...) {
        if (error_out) {
            *error_out = pluginStrdup("unknown exception");
        }
        return nullptr;
    }
}

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    // C ABI 边界异常守卫: 异常返回 NULL (宿主按"未导出"处理)
    return agentxx::plugin_guard::guardCall(
        pluginCatchLog,
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
        static const AgentxxPluginInfo info{
            AGENTXX_PLUGIN_API_VERSION,
            AGENTXX_SV("agentxx_execute_command"),
            AGENTXX_SV("1.0.0"),
            AGENTXX_SV("Command execution tools: bash (and Windows PowerShell/cmd via WSL interop)"),
        };
        return &info;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_entry(const AgentxxHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: entry 内含 JSON schema 构建等可抛操作, 异常返回 -1
    return agentxx::plugin_guard::guardCall(
        pluginCatchLog,
        -1,
        [&]() -> int {
        if (!host || !host->vtable || !plugin_ctx) {
            return -1;
        }
        g_host      = host;
        g_if        = agentxx::plugin::AgentIfaces::query(host);
        *plugin_ctx = nullptr;

        static std::vector<std::string> g_storage;

    #if XX_IS_WIN_D
        // 原生 Windows: 仅注册 windows command 工具
    #elif XX_IS_LINUX_D
        // Linux/WSL: 注册 bash 工具; WSL 下额外注册 windows command 工具
    #else
        // 其他平台 (macOS): bash 工具
    #endif

        // ---- agentxx_execute_windows_command ----
        // - 原生 Windows 或 WSL 环境注册 (与原 lib initTools 分支一致)
        const bool registerWindows =
    #if XX_IS_WIN_D
            true;
    #elif XX_IS_LINUX_D
            agentxx::util::isRunningInWSL();
    #else
            false;
    #endif

        if (registerWindows) {
            ToolPromptText p      = readToolPrompt(kNameWindows);
            std::string    depict = p.depict;
            if (depict.empty()) {
                depict = kDepictWinPlaceholder;
            }
            g_storage.push_back(std::move(depict));
            // command 参数描述区分直传/popen 路径与 WSL/原生环境 (与 lib prompt 一致,
            // 宿主 toolPrompt 正常提供完整文案, 此处为兜底简述)
            const char* commandKey = "command";
            std::string commandDesc
                = argDesc(p, commandKey, "The Windows command to execute.");
            if (argDesc(p, "command_process", "").empty()) {
                // 宿主条目缺失时兜底 key 也用 command (schema 统一)
                commandKey = "command";
            }
            // 经 json 对象构造后序列化 (commandKey 为运行期键, 直接作对象键使用)
            std::string schema = neograph::json{
                {"type", "object"},
                {"properties",
                 {
                     {commandKey,
                      {
                          {"type", "string"},
                          {"description", argDesc(p, commandKey, commandDesc)},
                      }},
                     {"all_output",
                      {
                          {"type", "boolean"},
                          {"description", argDesc(p, "all_output", kAllOutputDesc)},
                      }},
                     {"timeout",
                      {
                          {"type", "integer"},
                          {"description", argDesc(p, "timeout", kTimeoutDesc)},
                      }},
                 }},
                {"required", neograph::json::array({std::string{commandKey}})},
            }
                                  .dump();
            g_storage.push_back(std::move(schema));

            AgentxxSyncToolSpec spec{};
            spec.name        = agentxx_plugin_sv(kNameWindows, std::strlen(kNameWindows));
            spec.description = agentxx_plugin_sv(g_storage[0].data(), g_storage[0].size());
            spec.parameters_json
                = agentxx_plugin_sv(g_storage[1].data(), g_storage[1].size());
            spec.user_data = nullptr;
            spec.flags     = AGENTXX_TOOL_FLAG_AUTO_SUMMARY;
            spec.execute   = &wrapExecute<agentxx::execmd_plugin::windowsExecute>;
            if (agentxx_register_sync_tool(g_host, &spec) != 0) {
                XX_LOGW("agentxx_execute_command: register tool {} failed", kNameWindows);
            }
        }

        // ---- agentxx_execute_bash_command ----
        // - Linux/macOS 注册; 原生 Windows 不注册 (与原 lib initTools 分支一致:
        //   Windows 下仅当 bash 可用时才走该分支, 此处保持平台判断简化为不注册)
        const bool registerBash =
    #if XX_IS_WIN_D
            false;
    #else
            true;
    #endif

        if (registerBash) {
            ToolPromptText p      = readToolPrompt(kNameBash);
            std::string    depict = p.depict;
            if (depict.empty()) {
                depict = kDepictBash;
            }
            g_storage.push_back(std::move(depict));
            std::string schema = neograph::json{
                {"type", "object"},
                {"properties",
                 {
                     {"command",
                      {
                          {"type", "string"},
                          {"description", argDesc(p, "command", kBashCommandDesc)},
                      }},
                     {"all_output",
                      {
                          {"type", "boolean"},
                          {"description", argDesc(p, "all_output", kAllOutputDesc)},
                      }},
                     {"timeout",
                      {
                          {"type", "integer"},
                          {"description", argDesc(p, "timeout", kTimeoutDesc)},
                      }},
                 }},
                {"required", neograph::json::array({"command"})},
            }
                                  .dump();
            g_storage.push_back(std::move(schema));

            AgentxxSyncToolSpec spec{};
            spec.name        = agentxx_plugin_sv(kNameBash, std::strlen(kNameBash));
            spec.description = agentxx_plugin_sv(g_storage[2].data(), g_storage[2].size());
            spec.parameters_json
                = agentxx_plugin_sv(g_storage[3].data(), g_storage[3].size());
            spec.user_data = nullptr;
            spec.flags     = AGENTXX_TOOL_FLAG_AUTO_SUMMARY;
            spec.execute   = &wrapExecute<agentxx::execmd_plugin::bashExecute>;
            if (agentxx_register_sync_tool(g_host, &spec) != 0) {
                XX_LOGW("agentxx_execute_command: register tool {} failed", kNameBash);
            }
        }

        return 0;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_unload(void* plugin_ctx) {
    // C ABI 边界异常守卫: 卸载回调异常不得外泄
    agentxx::plugin_guard::guardCallVoid(pluginCatchLog, [&] {
        (void)plugin_ctx;
        g_host = nullptr;
        g_if   = {};
    });
}
