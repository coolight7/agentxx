// agentxx_execute_command —— 命令执行工具插件
// - 从 libagentxx src/tools/execute_command 拆分独立 (同名同行为):
//   - agentxx_execute_bash_command    (原 ExecuteBashCommandTool)
//   - agentxx_execute_windows_command (原 ExecuteWindowsCommandTool)
// - 子进程初始工作目录经宿主 get_work_dir (会话工作目录); 取消 watcher 经
//   agentxx.agent.cancel 接口轮询会话取消令牌 (与原内置工具行为一致)
// - 统一异步操作模型 (poll 寄生驱动): BOOST_PROCESS 可用时经
//   register_polled_tool 注册 —— 工作协程在实例 PollLoop 上 spawn, 由宿主
//   io 线程非阻塞步进, 与内置工具同线程交错执行; 并发命令共享寄生 loop,
//   不再每命令占死一个阻塞池线程至超时。bp 关闭时回退 sync 垫片 (popen)
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

#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
/// poll 寄生驱动工作协程: 解析参数 JSON → 调用协程执行体 → PolledOutcome
/// - 协程运行在实例 PollLoop (宿主 io 线程序列化步进); 异常由适配器兜底转
///   OP_FAILED, 此处无需自行捕获
/// - 取消双通道: job.cancelFlag (宿主 op 驱动器置位) + 会话取消令牌轮询;
///   is_cancelled 为 io 线程约束接口, 协程已在 io 线程 → 宿主内联直执行
template<auto ExecAsyncFn>
asio::awaitable<agentxx::plugin::PolledOutcome> execmdWork(agentxx::plugin::PolledJob& job) {
    auto* ctx = static_cast<PluginCtx*>(job.userData);
    auto  sv  = job.argsView();
    std::string argsStr(sv.data ? sv.data : "{}", sv.size);
    auto arguments = argsStr.empty() ? neograph::json::object() : neograph::json::parse(argsStr);
    // 会话取消查询回调: 捕获 thread_id 拷贝与 job 引用 (job 存活至终结上报)
    auto tid = std::string{job.tidView().data ? job.tidView().data : "", job.tidView().size};
    auto isCancelled = [ctx, tid, &job]() -> bool {
        if (job.cancelFlag != 0) {
            return true;
        }
        return isSessionCancelled(ctx, agentxx_plugin_sv(tid.data(), tid.size()));
    };
    auto workDir = readWorkDir(ctx, agentxx_plugin_sv(tid.data(), tid.size()));
    auto result  = co_await ExecAsyncFn(arguments, workDir, isCancelled);
    co_return agentxx::plugin::PolledOutcome::ok(std::move(result));
}
#endif

/// C ABI execute 包装 (popen 回退路径专用): 解析参数 JSON → 调用实现 →
/// 结果 strdup (异常不外泄)
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
    auto* ctx = static_cast<PluginCtx*>(user_data);
    (void)tool_call_id;
    const AgentxxHost* host = ctx ? ctx->host : nullptr;
    try {
        std::string argsStr(args_json.data ? args_json.data : "", args_json.size);
        auto arguments = argsStr.empty() ? neograph::json::object() : neograph::json::parse(argsStr);
        // 会话取消查询回调: 捕获 thread_id 视图内容 (回调生命周期内有效);
        // cancel_flag 置位时同样视为取消 (宿主超时/放弃路径触发)
        std::string tid{thread_id.data ? thread_id.data : "", thread_id.size};
        auto isCancelled = [ctx, tid, cancel_flag]() -> bool {
            if (cancel_flag && *cancel_flag != 0) {
                return true;
            }
            return isSessionCancelled(ctx, agentxx_plugin_sv(tid.data(), tid.size()));
        };
        auto workDir = readWorkDir(ctx, agentxx_plugin_sv(tid.data(), tid.size()));
        auto result  = ExecFn(arguments, workDir, isCancelled);
        return pluginStrdup(host, result.c_str());
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = pluginStrdup(host, ex.what());
        }
        return nullptr;
    } catch (...) {
        if (error_out) {
            *error_out = pluginStrdup(host, "unknown exception");
        }
        return nullptr;
    }
}

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    // C ABI 边界异常守卫: 异常返回 NULL (宿主按"未导出"处理);
    // 本边界为纯静态元数据, 无实例上下文可捕获 → 空操作日志闭包
    return agentxx::plugin_guard::guardCall(
        [](const char*) noexcept {},
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
    agentxx_plugin_create(const AgentxxHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: create 内含 JSON schema 构建等可抛操作, 异常返回 -1;
    // 守卫日志闭包捕获局部裸指针 (ctx 装配前置空 → 异常路径静默丢弃)
    PluginCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
        [&raw](const char* msg) noexcept { pluginLog(raw, 4, msg ? msg : ""); },
        -1,
        [&]() -> int {
        if (!host || !host->vtable || !plugin_ctx) {
            return -1;
        }
        auto ctx   = std::make_unique<PluginCtx>();
        ctx->host  = host;
        ctx->iface = agentxx::plugin::AgentIfaces::query(host);
        raw        = ctx.get();
        auto&      g_storage = ctx->storage; ///< 沿用原局部别名 (现挂到实例)

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
            ToolPromptText p      = readToolPrompt(ctx->host, ctx->iface, kNameWindows);
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

#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
            // poll 寄生驱动注册 (统一异步操作模型): 工作协程在实例 PollLoop 上
            // spawn, 宿主 io 线程经 pollOnce 非阻塞步进, 与内置工具同线程交错;
            // 并发命令共享寄生 loop 等就绪事件 (不再占死阻塞池线程至超时)
            auto shim = std::make_unique<agentxx::plugin::PolledToolShim>();
            if (agentxx::plugin::register_polled_tool(
                    host,
                    agentxx_plugin_sv(kNameWindows, std::strlen(kNameWindows)),
                    agentxx_plugin_sv(g_storage[0].data(), g_storage[0].size()),
                    agentxx_plugin_sv(g_storage[1].data(), g_storage[1].size()),
                    ctx->pollLoop,
                    &execmdWork<agentxx_execmd_plugin::windowsExecuteAsync>,
                    ctx.get(),
                    shim.get(),
                    /*timeoutMs=*/0,
                    AGENTXX_TOOL_FLAG_AUTO_SUMMARY
                ) != 0) {
                pluginLog(ctx.get(), 3,
                          fmt::format("agentxx_execute_command: register tool {} failed", kNameWindows));
            } else {
                ctx->polled_shims.push_back(std::move(shim));
            }
#else
            // popen 回退: 阻塞实现经 sync 垫片注册 (offload 池线程执行)
            auto shim = std::make_unique<AgentxxSyncToolShim>();

            AgentxxSyncToolSpec spec{};
            spec.name        = agentxx_plugin_sv(kNameWindows, std::strlen(kNameWindows));
            spec.description = agentxx_plugin_sv(g_storage[0].data(), g_storage[0].size());
            spec.parameters_json
                = agentxx_plugin_sv(g_storage[1].data(), g_storage[1].size());
            spec.user_data = ctx.get();
            spec.flags     = AGENTXX_TOOL_FLAG_AUTO_SUMMARY;
            spec.execute   = &wrapExecute<agentxx_execmd_plugin::windowsExecute>;
            if (agentxx_register_sync_tool(host, &spec, shim.get()) != 0) {
                pluginLog(ctx.get(), 3,
                          fmt::format("agentxx_execute_command: register tool {} failed", kNameWindows));
            } else {
                ctx->sync_tool_shims.push_back(std::move(shim));
            }
#endif
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
            ToolPromptText p      = readToolPrompt(ctx->host, ctx->iface, kNameBash);
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

#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
            // poll 寄生驱动注册 (语义同上方 windows 工具)
            auto shim = std::make_unique<agentxx::plugin::PolledToolShim>();
            if (agentxx::plugin::register_polled_tool(
                    host,
                    agentxx_plugin_sv(kNameBash, std::strlen(kNameBash)),
                    agentxx_plugin_sv(g_storage[2].data(), g_storage[2].size()),
                    agentxx_plugin_sv(g_storage[3].data(), g_storage[3].size()),
                    ctx->pollLoop,
                    &execmdWork<agentxx_execmd_plugin::bashExecuteAsync>,
                    ctx.get(),
                    shim.get(),
                    /*timeoutMs=*/0,
                    AGENTXX_TOOL_FLAG_AUTO_SUMMARY
                ) != 0) {
                pluginLog(ctx.get(), 3,
                          fmt::format("agentxx_execute_command: register tool {} failed", kNameBash));
            } else {
                ctx->polled_shims.push_back(std::move(shim));
            }
#else
            // popen 回退: 阻塞实现经 sync 垫片注册 (offload 池线程执行)
            auto shim = std::make_unique<AgentxxSyncToolShim>();

            AgentxxSyncToolSpec spec{};
            spec.name        = agentxx_plugin_sv(kNameBash, std::strlen(kNameBash));
            spec.description = agentxx_plugin_sv(g_storage[2].data(), g_storage[2].size());
            spec.parameters_json
                = agentxx_plugin_sv(g_storage[3].data(), g_storage[3].size());
            spec.user_data = ctx.get();
            spec.flags     = AGENTXX_TOOL_FLAG_AUTO_SUMMARY;
            spec.execute   = &wrapExecute<agentxx_execmd_plugin::bashExecute>;
            if (agentxx_register_sync_tool(host, &spec, shim.get()) != 0) {
                pluginLog(ctx.get(), 3,
                          fmt::format("agentxx_execute_command: register tool {} failed", kNameBash));
            } else {
                ctx->sync_tool_shims.push_back(std::move(shim));
            }
#endif
        }

        *plugin_ctx = ctx.release(); ///< 所有权移交宿主 (destroy 时取回归还)
        return 0;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_destroy(void* plugin_ctx) {
    // C ABI 边界异常守卫: 销毁回调异常不得外泄
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    agentxx::plugin_guard::guardCallVoid(
        [ctx](const char* msg) noexcept { pluginLog(ctx, 4, msg ? msg : ""); },
        [&] { delete ctx; }); // 垫片适配器为 ctx 内嵌存储, 随 ctx 一并释放
}
