/// agentxx_execute_command —— 命令执行工具插件
///
/// 两个工具的实现体都是 asio 协程 (`bashExecuteAsync` / `windowsExecuteAsync`):
/// 子进程管道绑定到协程 executor, 等待的是插件本地 reactor 上的管道就绪事件,
/// 因此注册为**声明式受控轮询**工具 (`polled_tool`, 见 plugin_kit.h)。
/// 并发多条命令共享同一条 polled 驱动序列与同一个本地 reactor, 不再每条命令
/// 占死一个宿主工作线程直到超时 (会话取消经 CancelRegistry 事件驱动 kill 进程组)。
/// 只有关闭 `AGENTXX_ENABLE_BOOST_PROCESS` 的 popen 回退实现仍是阻塞函数,
/// 继续走 `blocking_tool` (offload 工作线程)。
///
/// 工具提示词由本插件负责 (原 libagentxx 的 AgentPrompt 环境探测已迁移至此):
/// start 事务内先探测运行环境 (python/node 与 Windows 侧 PowerShell), 再把
/// 生成的提示词作为本实例贡献注入宿主 (见 [publishToolPrompt])。
#include "agentxx_execmd_plugin.h"
#include "asio/awaitable.hpp"
#include "execute_command_env.h"
#include "execute_command_impl.h"
#include "utilxx_base/json.h"
#include <string>

using namespace agentxx_execmd_plugin;
using namespace agentxx::plugin;

namespace {

constexpr std::string_view kNameBash    = "agentxx_execute_bash_command";
constexpr std::string_view kNameWindows = "agentxx_execute_windows_command";

/// 取参数描述 (提示词里没有该参数时回退给定文本)
/// - 提示词来自本插件自身生成, 正常都命中; 回退保证宿主提示词接口缺失时
///   schema 描述仍完整
std::string_view
    argDescOr(const ExecPromptText& prompt, std::string_view name, std::string_view fallback) {
    const auto it = prompt.args.find(name);
    if (it != prompt.args.end() && !it->second.empty()) {
        return it->second;
    }
    return fallback;
}

} // namespace

struct ExecPluginCtx : public PluginBase {
    /// 运行环境探测结果 (start 事务内探测一次, 工具提示词与 schema 由此生成)
    ExecEnvInfo env;
};

/// 把工具提示词 (depict + 参数描述) 作为本实例的贡献注入宿主提示词表
/// - 宿主按所有者记录贡献 (见 PluginManager::setPromptJson), 插件卸载/禁用时
///   自动撤销并恢复基础值; 同一工具由多个插件实例贡献时互不覆盖
/// - 宿主未提供 prompt 接口表 (精简宿主) 时跳过: 描述文本仍随工具注册直接生效
static void
    publishToolPrompt(ExecPluginCtx& ctx, std::string_view toolName, const ExecPromptText& prompt) {
    if (!ctx.host || !ctx.iface.prompt || !ctx.iface.prompt->set_prompt) {
        pluginLog(
            &ctx,
            2,
            "agentxx_execute_command: host has no prompt iface, tool prompt skipped"
        );
        return;
    }
    utilxx_base::Json args = utilxx_base::Json::object();
    for (const auto& [name, desc] : prompt.args) {
        args[name] = desc;
    }
    utilxx_base::Json tool = utilxx_base::Json::object();
    tool["depict"]         = prompt.depict;
    tool["args"]           = std::move(args);

    utilxx_base::Json tools      = utilxx_base::Json::object();
    tools[std::string{toolName}] = std::move(tool);

    utilxx_base::Json patch = utilxx_base::Json::object();
    patch["toolPrompt"]     = std::move(tools);

    const std::string js   = patch.dump();
    const auto        jsSv = PluginStringView::from(js.data(), js.size());
    if (ctx.iface.prompt->set_prompt(ctx.host, &jsSv) != 0) {
        pluginLog(&ctx, 3, fmt::format("agentxx_execute_command: set_prompt({}) failed", toolName));
        return;
    }
    pluginLog(
        &ctx,
        2,
        fmt::format("agentxx_execute_command: tool prompt of {} injected", toolName)
    );
}

static int32_t setupExecPlugin(ExecPluginCtx& ctx) {
    // 环境探测 (python/node 可用性与版本, Windows 侧含 PowerShell 版本):
    // - 阻塞式子进程探测, 每次 start 只执行一次, 结果存实例上下文
    // - 必须在工具注册前完成: 工具 definition 的 description/parameters 在注册时
    //   由宿主固化, 之后无法再刷新 (提示词注入了也只能改宿主提示词表)
    ctx.env = detectExecEnv();

#if XX_IS_WIN_D
    // Windows 侧命令的语法指引随实际执行路径变化: boost.process v2 直传 argv
    // (命令作为单个 -Command 参数) 与 popen 回退 (命令经外层 shell 解析) 的
    // 引号/转义要求不同, 故按编译期路径选择对应描述
    const bool viaProcessSpawn =
#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
        true;
#else
        false;
#endif
    const auto winPrompt = windowsToolPrompt(ctx.env, viaProcessSpawn);
    // 先注入提示词再注册: polled_tool/blocking_tool 注册时经 ctx.toolPrompt(name)
    // 读取宿主提示词的 depict 作为工具描述, 注册后无法再改
    publishToolPrompt(ctx, kNameWindows, winPrompt);

    auto winSchema
        = ctx.schema(kNameWindows)
              .string(
                  "command",
                  argDescOr(winPrompt, "command", kWindowsCommandArgDescFallback),
                  /*required=*/true
              )
              .integer("timeout", argDescOr(winPrompt, "timeout", kTimeoutArgDesc), false, 60)
              .boolean(
                  "all_output",
                  argDescOr(winPrompt, "all_output", kAllOutputArgDesc),
                  false,
                  true
              )
              .build();

#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
    polled_tool(
        ctx,
        kNameWindows,
        winPrompt.depict,
        winSchema,
        [](ExecPluginCtx&                  c,
           std::string_view                args_json,
           std::string_view                tid,
           std::string_view                workDir,
           const PluginxxCancelToken* cancel_token) -> asio::awaitable<std::string> {
            ArgReader   args(args_json);
            std::string tidStr(tid);
            if (pluginxx_cancel_is_requested(cancel_token)) {
                c.cancelRegistry.cancel(tidStr);
            }
            auto isCancelled = [&c, tidStr, cancel_token]() -> bool {
                if (pluginxx_cancel_is_requested(cancel_token)) {
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
        winPrompt.depict,
        winSchema,
        [](ExecPluginCtx&                  c,
           std::string_view                args_json,
           std::string_view                tid,
           std::string_view                workDir,
           const PluginxxCancelToken* cancel_token) -> std::string {
            ArgReader   args(args_json);
            std::string tidStr(tid);
            if (pluginxx_cancel_is_requested(cancel_token)) {
                c.cancelRegistry.cancel(tidStr);
            }
            auto isCancelled = [&c, tidStr, cancel_token]() -> bool {
                if (pluginxx_cancel_is_requested(cancel_token)) {
                    return true;
                }
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
    const auto bashPrompt = bashToolPrompt(ctx.env);
    // 先注入提示词再注册: polled_tool/blocking_tool 注册时经 ctx.toolPrompt(name)
    // 读取宿主提示词的 depict 作为工具描述, 注册后无法再改
    publishToolPrompt(ctx, kNameBash, bashPrompt);

    auto bashSchema
        = ctx.schema(kNameBash)
              .string(
                  "command",
                  argDescOr(bashPrompt, "command", kBashToolDepict),
                  /*required=*/true
              )
              .integer("timeout", argDescOr(bashPrompt, "timeout", kTimeoutArgDesc), false, 60)
              .boolean(
                  "all_output",
                  argDescOr(bashPrompt, "all_output", kAllOutputArgDesc),
                  false,
                  true
              )
              .build();

#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
    polled_tool(
        ctx,
        kNameBash,
        bashPrompt.depict,
        bashSchema,
        [](ExecPluginCtx&                  c,
           std::string_view                args_json,
           std::string_view                tid,
           std::string_view                workDir,
           const PluginxxCancelToken* cancel_token) -> asio::awaitable<std::string> {
            ArgReader   args(args_json);
            std::string tidStr(tid);
            if (pluginxx_cancel_is_requested(cancel_token)) {
                c.cancelRegistry.cancel(tidStr);
            }
            auto isCancelled = [&c, tidStr, cancel_token]() -> bool {
                if (pluginxx_cancel_is_requested(cancel_token)) {
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
        bashPrompt.depict,
        bashSchema,
        [](ExecPluginCtx&                  c,
           std::string_view                args_json,
           std::string_view                tid,
           std::string_view                workDir,
           const PluginxxCancelToken* cancel_token) -> std::string {
            ArgReader   args(args_json);
            std::string tidStr(tid);
            if (pluginxx_cancel_is_requested(cancel_token)) {
                c.cancelRegistry.cancel(tidStr);
            }
            auto isCancelled = [&c, tidStr, cancel_token]() -> bool {
                if (pluginxx_cancel_is_requested(cancel_token)) {
                    return true;
                }
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
    ExecPluginCtx&                     ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error
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
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

static void*
    execStop(ExecPluginCtx&, const PluginxxOperatorNotify* notify, PluginxxString*) {
    // 无自管线程/定时器; 注册记录由宿主在 stop 后统一撤销。
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
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
    ctx.registerTemplate(kNameWindows, "Bash", "command");
    return 0;
}

static void* execClientStart(
    ExecClientCtx&                     ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error
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
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

static void*
    execClientStop(ExecClientCtx&, const PluginxxOperatorNotify* notify, PluginxxString*) {
    // UI 注册记录由宿主在 stop 后统一撤销。
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
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
