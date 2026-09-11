/// libagentxx_execute_javascript.so —— JS 执行工具插件的 C++ 壳
/// 仿 example_js / agentxx_execute_command 结构：
/// - 本体为 C++ 插件，entry 指向 libagentxx_execute_javascript.so
/// - start 阶段经 interpreter.js 能力把同目录 plugin.js 交给 QuickJS 引擎执行
/// - plugin.js 内注册 agentxx_execute_javascript 工具（仿照 agentxx_execute_command）
///
/// Reset-v1 生命周期 (见 plugin.md 第 5.2 节):
/// - create: 只构造上下文 (查询接口表 + 解析自身路径), 不注册、不启动线程;
/// - start:  校验 "interpreter.js" 能力后加载脚本, 脚本注册的工具属于本实例;
/// - stop:   通知引擎卸载脚本 (撤销脚本注册), 引擎不可用时只记录;
/// - destroy: 只释放本地状态, 不再调用宿主接口。
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"

#include "fmt/format.h"
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace {

struct ShellCtx {
    const AgentxxPluginHost*     host = nullptr;
    agentxx::plugin::AgentIfaces iface{};
    std::string                  name;
    std::string                  dir;
    std::string                  scriptPath;
    /// 脚本是否已由引擎加载 (load 成功后置位, stop 撤销; 仅在所属 IO 线程访问)
    bool scriptLoaded = false;
    /// start/stop 的 op 占位句柄 (宿主只当不透明非空 token; 每实例独立)
    int opToken = 0;
};

void shellLog(const ShellCtx* ctx, int level, const std::string& msg) {
    if (ctx && ctx->host && ctx->iface.log && ctx->iface.log->log) {
        auto sv = agentxx::plugin::PluginStringView::from(msg.data(), msg.size());
        ctx->iface.log->log(ctx->host, level, &sv);
    }
}

std::string dirOf(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? "." : path.substr(0, pos);
}

bool fileExists(const std::string& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

/// 拼装 JSON 转义后的字符串字段 (经宿主 json 接口表, 避免路径/名称转义错误)
std::string jsonEscapedString(const ShellCtx& ctx, const std::string& value) {
    if (!ctx.iface.json || !ctx.iface.json->json_escape) {
        return fmt::format("\"{}\"", value);
    }
    AgentxxPluginString esc{nullptr, 0};
    auto                sv = agentxx::plugin::PluginStringView::from(value.data(), value.size());
    if (ctx.iface.json->json_escape(ctx.host, &sv, &esc) != 0 || !esc.data) {
        return fmt::format("\"{}\"", value);
    }
    std::string out(esc.data, static_cast<size_t>(esc.size));
    agentxx::plugin::PluginString::free(ctx.host, &esc);
    return out;
}

/// "interpreter.js" 能力调用参数: {"name": <插件名>, "path": <脚本路径>}
std::string scriptArgsJson(const ShellCtx& ctx) {
    return fmt::format(
        "{{\"name\":{},\"path\":{}}}",
        jsonEscapedString(ctx, ctx.name),
        jsonEscapedString(ctx, ctx.scriptPath)
    );
}

/// 脚本加载完成 (异步 start 的收尾): 宿主 io 线程派发, 期间 caller lease
/// 保证本实例上下文存活。
void AGENTXX_PLUGIN_CALL onScriptLoadDone(
    void* ud, int32_t status, const AgentxxPluginStringView* payload
) {
    auto* state = static_cast<std::pair<ShellCtx*, AgentxxPluginOperatorNotify>*>(ud);
    if (!state) {
        return;
    }
    ShellCtx*                   ctx    = state->first;
    AgentxxPluginOperatorNotify notify = state->second;
    delete state;

    std::string text;
    if (payload && payload->data) {
        text.assign(payload->data, static_cast<size_t>(payload->size));
    }
    if (status == AGENTXX_PLUGIN_OPERATOR_OK) {
        if (ctx) {
            ctx->scriptLoaded = true;
            if (!text.empty()) {
                shellLog(ctx, 2, fmt::format("agentxx_execute_javascript: loaded tools {}", text));
            }
        }
        notify.done(notify.host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
        return;
    }
    if (ctx) {
        ctx->scriptLoaded = false;
    }
    std::string msg = text.empty() ? std::string("script load failed") : text;
    if (ctx) {
        shellLog(ctx, 4, fmt::format("agentxx_execute_javascript: interpreter load failed: {}", msg));
    }
    auto errSv = agentxx::plugin::PluginStringView::from(msg.data(), msg.size());
    notify.done(notify.host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &errSv);
}

/// 脚本卸载完成 (stop 事务的收尾, fire-and-forget: 结果只记录)
void AGENTXX_PLUGIN_CALL onScriptUnloadDone(
    void* ud, int32_t status, const AgentxxPluginStringView* payload
) {
    auto* ctx = static_cast<ShellCtx*>(ud);
    if (!ctx || status == AGENTXX_PLUGIN_OPERATOR_OK) {
        return;
    }
    std::string text;
    if (payload && payload->data) {
        text.assign(payload->data, static_cast<size_t>(payload->size));
    }
    shellLog(
        ctx,
        3,
        fmt::format(
            "agentxx_execute_javascript: interpreter unload failed: {}",
            text.empty() ? "?" : text
        )
    );
}

/// 通知引擎卸载本脚本 (best-effort: 引擎不可用时宿主已完成撤销)
void dispatchScriptUnload(ShellCtx& ctx) {
    if (!ctx.scriptLoaded) {
        return;
    }
    ctx.scriptLoaded = false;
    if (!ctx.iface.capabilities || !ctx.iface.capabilities->invoke_capability_async) {
        return;
    }
    auto        capSv    = agentxx::plugin::PluginStringView::fromCstr("interpreter.js");
    auto        unloadSv = agentxx::plugin::PluginStringView::fromCstr("unload");
    std::string args     = fmt::format("{{\"name\":{}}}", jsonEscapedString(ctx, ctx.name));
    auto        argsSv   = agentxx::plugin::PluginStringView::from(args.data(), args.size());
    AgentxxPluginString err{nullptr, 0};
    auto*               h = ctx.iface.capabilities->invoke_capability_async(
        ctx.host,
        &capSv,
        &unloadSv,
        &argsSv,
        &onScriptUnloadDone,
        &ctx,
        &err
    );
    if (err.data) {
        if (!h) {
            shellLog(
                &ctx,
                3,
                fmt::format("agentxx_execute_javascript: unload dispatch failed: {}", err.data)
            );
        }
        agentxx::plugin::PluginString::free(ctx.host, &err);
    }
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
                agentxx::plugin::PluginStringView::fromCstr("agentxx_execute_javascript"),
                agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
                agentxx::plugin::PluginStringView::fromCstr(
                    "Execute JavaScript code (JS equivalent of bash command) via QuickJS"
                ),
            };
            return &info;
        }
    );
}

/// create: 只构造上下文 (查询接口表 + 解析自身名称/脚本路径), 不做运行时注册,
/// 不启动线程, 不调用能力 (Reset-v1 第 5.2 节)。
extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_agent_create(const AgentxxPluginHost* host, void** plugin_ctx) {
    ShellCtx* raw = nullptr;
    return agentxx::plugin::guardCall(
        [&raw](const char* msg) noexcept {
            shellLog(raw, 4, msg ? msg : "");
        },
        -1,
        [&]() -> int {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            auto ctx   = std::make_unique<ShellCtx>();
            ctx->host  = host;
            ctx->iface = agentxx::plugin::AgentIfaces::query(host);
            raw        = ctx.get();
            if (!ctx->iface.capabilities || !ctx->iface.plugins || !ctx->iface.json
                || !ctx->iface.log) {
                return -1;
            }
            const auto& s_if = ctx->iface;
            auto        logE = [&](const std::string& msg) {
                auto sv = agentxx::plugin::PluginStringView::from(msg.data(), msg.size());
                s_if.log->log(host, 4, &sv);
            };

            AgentxxPluginString info{nullptr, 0};
            s_if.plugins->get_own_info(host, &info);
            if (!info.data) {
                logE("agentxx_execute_javascript: get_own_info failed");
                return -1;
            }
            auto field = [&](const char* key) -> std::string {
                AgentxxPluginString v{nullptr, 0};
                auto                infoSv = agentxx::plugin::PluginStringView::toSv(&info);
                auto                keySv  = agentxx::plugin::PluginStringView::fromCstr(key);
                s_if.json->json_get_string(host, &infoSv, &keySv, &v);
                if (!v.data) {
                    return {};
                }
                std::string s(v.data, static_cast<size_t>(v.size));
                agentxx::plugin::PluginString::free(host, &v);
                return s;
            };
            std::string libPath = field("path");
            ctx->name           = field("name");
            agentxx::plugin::PluginString::free(host, &info);
            if (ctx->name.empty() || libPath.empty()) {
                logE("agentxx_execute_javascript: own info invalid");
                return -1;
            }
            ctx->dir = dirOf(libPath);

            ctx->scriptPath = fmt::format("{}/plugin.js", ctx->dir);
            if (!fileExists(ctx->scriptPath)) {
                auto pos = ctx->dir.find_last_of("/\\");
                if (pos != std::string::npos) {
                    std::string parent = ctx->dir.substr(0, pos);
                    if (fileExists(fmt::format("{}/plugin.js", parent))) {
                        ctx->dir        = parent;
                        ctx->scriptPath = fmt::format("{}/plugin.js", parent);
                    }
                }
            }
            // 开发期兜底：若按 get_own_info 推导的 plugin.js 不存在，尝试源码目录
            // (host 传入的 path 在内置模式下可能为虚路径，此分支仅为本地调试兜底)
            if (!fileExists(ctx->scriptPath)) {
                std::string fallback = "agent/plugins/agentxx_execute_javascript/plugin.js";
                if (fileExists(fallback)) {
                    ctx->scriptPath = fallback;
                }
            }
            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

/// start 事务: 依赖能力校验 + 脚本加载。
/// - 加载经 "interpreter.js" 能力异步执行, 脚本注册的工具属于本实例;
///   因此 start 必须等加载结束才 done (返回宿主托管句柄, 完成回调上报结果);
/// - 能力不可用或脚本缺失 → 返回 NULL + error (拒绝); 加载失败 → done FAILED;
///   两条路径都由宿主回滚本次加载, 不留注册残留。
static void* jsShellAgentStart(
    ShellCtx& ctx, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString* error
) {
    auto setErr = [&](const std::string& msg) -> void* {
        if (error) {
            agentxx::plugin::PluginString::set(ctx.host, error, msg);
        }
        return nullptr;
    };
    if (!notify || !notify->done) {
        return setErr("agentxx_execute_javascript start: notify required");
    }
    if (!ctx.iface.capabilities || !ctx.iface.capabilities->has_capability
        || !ctx.iface.capabilities->invoke_capability_async) {
        return setErr("agentxx_execute_javascript start: host lacks capabilities interface");
    }
    auto capSv = agentxx::plugin::PluginStringView::fromCstr("interpreter.js");
    if (!ctx.iface.capabilities->has_capability(ctx.host, &capSv)) {
        return setErr(
            "agentxx_execute_javascript start: interpreter.js capability not available "
            "(need agentxx_javascript_engine)"
        );
    }
    if (!fileExists(ctx.scriptPath)) {
        return setErr(
            "agentxx_execute_javascript start: plugin.js not found next to the plugin library"
        );
    }
    if (ctx.scriptLoaded) {
        // 重复 start (幂等): 脚本已由本实例加载
        notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
        return nullptr;
    }

    std::string args   = scriptArgsJson(ctx);
    auto        loadSv = agentxx::plugin::PluginStringView::fromCstr("load");
    auto        argsSv = agentxx::plugin::PluginStringView::from(args.data(), args.size());
    AgentxxPluginString err{nullptr, 0};
    auto* state = new std::pair<ShellCtx*, AgentxxPluginOperatorNotify>(&ctx, *notify);
    auto* h     = ctx.iface.capabilities->invoke_capability_async(
        ctx.host,
        &capSv,
        &loadSv,
        &argsSv,
        &onScriptLoadDone,
        state,
        &err
    );
    if (!h) {
        delete state;
        std::string errStr = err.data ? std::string(err.data, static_cast<size_t>(err.size))
                                      : "load script async dispatch failed";
        if (err.data) {
            agentxx::plugin::PluginString::free(ctx.host, &err);
        }
        return setErr(errStr);
    }
    if (err.data) {
        agentxx::plugin::PluginString::free(ctx.host, &err);
    }
    shellLog(
        &ctx,
        2,
        fmt::format("agentxx_execute_javascript: loading script {}", ctx.scriptPath)
    );
    return &ctx.opToken; ///< 已接受: 完成通知在加载回调中发出
}

/// stop 事务: 通知引擎卸载脚本 (撤销脚本注册的工具/订阅/定时器)。
/// - 引擎已停用/卸载时卸载调用失败: 只记录, 脚本上下文由引擎的停止路径释放;
/// - 卸载 op 自身持有本实例 caller lease, 宿主会等它完成后才 destroy;
/// - 本函数同步完成 stop 本身 (卸载为 fire-and-forget, 无阻塞等待)。
static void* jsShellAgentStop(
    ShellCtx& ctx, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString*
) {
    if (!notify || !notify->done) {
        return nullptr;
    }
    dispatchScriptUnload(ctx);
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT(ShellCtx, jsShellAgentStart, jsShellAgentStop)

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<ShellCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(
        [ctx](const char* msg) noexcept {
            shellLog(ctx, 4, msg ? msg : "");
        },
        [&] {
            if (!ctx) {
                return;
            }
            // destroy 只处理已经停止的对象: 不再调用宿主接口、不创建异步工作
            // (脚本注销与上下文释放由 stop 与引擎的停止路径完成)
            delete ctx;
        }
    );
}
