/// libexample_js_execute_command.so —— JS 执行工具插件的 C++ 壳
/// 仿 example_js / agentxx_execute_command 结构：
/// - 本体为 C++ 插件，entry 指向 libexample_js_execute_command.so
/// - start 阶段经 interpreter.js 能力把同目录 plugin.js 交给 QuickJS 引擎执行
/// - plugin.js 内注册 example_js_execute_command 工具（仿照 agentxx_execute_command）
///
/// 实例生命周期: create 只构造, start 加载脚本, stop 停止脚本执行
/// - create: 只构造上下文 (查询接口表 + 解析自身路径), 不注册、不启动线程;
/// - start:  校验 "interpreter.js" 能力后加载脚本, 脚本注册的工具属于本实例;
/// - stop:   通知引擎卸载脚本 (撤销脚本注册), 引擎不可用时只记录;
/// - destroy: 只释放本地状态, 不再调用主程序接口。
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/agent_plugin_kit.h"

#include "fmt/format.h"
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace {

/// JS 引擎声明的能力全名 (引擎以短名 `interpreter.js` 声明, 主程序补全成
/// `plugin.<插件 id>.<名字>`; 调用方必须写全名)
constexpr const char* kInterpreterCapability
    = "plugin.agentxx_javascript_engine.interpreter.js";

struct ShellCtx {
    const PluginxxHost*          host = nullptr;
    agentxx::plugin::AgentIfaces iface{};
    std::string                  name;
    std::string                  dir;
    std::string                  scriptPath;
    /// 脚本是否已由引擎加载 (load 成功后置位, stop 撤销; 仅在所属 IO 线程访问)
    bool scriptLoaded = false;
    /// start/stop 的 op 占位句柄 (主程序只当不透明非空 token; 每实例独立)
    int opToken = 0;

    /// 构造入口经 [agentxx::plugin::detail::createCtxEntry] 调用:
    /// 记主程序句柄并一次查齐本插件用到的接口表
    void init(const PluginxxHost* h) {
        host  = h;
        iface = agentxx::plugin::AgentIfaces::query(h);
    }
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

/// 拼装 JSON 转义后的字符串字段 (经主程序 json 接口表, 避免路径/名称转义错误)
/// - 转义逻辑统一在 SDK (见 agentxx::plugin::jsonEscape), 此处仅固定本插件的调用形态
std::string jsonEscapedString(const ShellCtx& ctx, const std::string& value) {
    return agentxx::plugin::jsonEscape(ctx.host, ctx.iface.json, value);
}

/// "interpreter.js" 能力调用参数: {"op": "load", "name": <插件名>, "path": <脚本路径>}
std::string scriptArgsJson(const ShellCtx& ctx) {
    return fmt::format(
        "{{\"op\":\"load\",\"name\":{},\"path\":{}}}",
        jsonEscapedString(ctx, ctx.name),
        jsonEscapedString(ctx, ctx.scriptPath)
    );
}

/// 脚本加载完成 (异步 start 的收尾): 主程序 io 线程派发, 期间 caller lease
/// 保证本实例上下文存活。
void PLUGINXX_CALL onScriptLoadDone(void* ud, int32_t status, const PluginxxStringView* payload) {
    auto* state = static_cast<std::pair<ShellCtx*, PluginxxOperatorNotify>*>(ud);
    if (!state) {
        return;
    }
    ShellCtx*              ctx    = state->first;
    PluginxxOperatorNotify notify = state->second;
    delete state;

    std::string text;
    if (payload && payload->data) {
        text.assign(payload->data, static_cast<size_t>(payload->size));
    }
    if (status == PLUGINXX_OPERATOR_OK) {
        if (ctx) {
            ctx->scriptLoaded = true;
            if (!text.empty()) {
                shellLog(ctx, 2, fmt::format("example_js_execute_command: loaded tools {}", text));
            }
        }
        notify.done(notify.host_ud, PLUGINXX_OPERATOR_OK, nullptr);
        return;
    }
    if (ctx) {
        ctx->scriptLoaded = false;
    }
    std::string msg = text.empty() ? std::string("script load failed") : text;
    if (ctx) {
        shellLog(
            ctx,
            4,
            fmt::format("example_js_execute_command: interpreter load failed: {}", msg)
        );
    }
    auto errSv = agentxx::plugin::PluginStringView::from(msg.data(), msg.size());
    notify.done(notify.host_ud, PLUGINXX_OPERATOR_FAILED, &errSv);
}

/// 脚本卸载完成 (stop 事务的收尾, fire-and-forget: 结果只记录)
void PLUGINXX_CALL onScriptUnloadDone(void* ud, int32_t status, const PluginxxStringView* payload) {
    // ud 是 {脚本上下文, stop 的完成通知}: 卸载结果只记录, stop 本身一律按成功收尾
    // (脚本上下文由引擎释放; 主程序侧注册随实例卸载一起撤销)
    auto* state = static_cast<std::pair<ShellCtx*, PluginxxOperatorNotify>*>(ud);
    if (!state) {
        return;
    }
    ShellCtx*              ctx    = state->first;
    PluginxxOperatorNotify notify = state->second;
    delete state;
    if (ctx && status != PLUGINXX_OPERATOR_OK) {
        std::string text;
        if (payload && payload->data) {
            text.assign(payload->data, static_cast<size_t>(payload->size));
        }
        shellLog(
            ctx,
            3,
            fmt::format(
                "example_js_execute_command: interpreter unload failed: {}",
                text.empty() ? "?" : text
            )
        );
    }
    if (notify.done) {
        notify.done(notify.host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
}

/// 通知引擎卸载本脚本 (best-effort: 引擎不可用时主程序已完成撤销)
bool dispatchScriptUnload(ShellCtx& ctx, const PluginxxOperatorNotify* stopNotify) {
    if (!ctx.scriptLoaded) {
        return false;
    }
    ctx.scriptLoaded = false;
    if (!ctx.iface.capabilities || !ctx.iface.capabilities->invoke_capability) {
        return false;
    }
    auto capSv = agentxx::plugin::PluginStringView::fromCstr(kInterpreterCapability);
    std::string args
        = fmt::format("{{\"op\":\"unload\",\"name\":{}}}", jsonEscapedString(ctx, ctx.name));
    auto           argsSv = agentxx::plugin::PluginStringView::from(args.data(), args.size());
    PluginxxString err{nullptr, 0};
    auto*          state = new std::pair<ShellCtx*, PluginxxOperatorNotify>(
        &ctx,
        stopNotify != nullptr && stopNotify->done ? *stopNotify : PluginxxOperatorNotify{}
    );
    auto* h = ctx.iface.capabilities->invoke_capability(
        ctx.host,
        nullptr,
        &capSv,
        &argsSv,
        0,
        -1,
        &onScriptUnloadDone,
        state,
        &err
    );
    if (err.data) {
        shellLog(
            &ctx,
            4,
            fmt::format(
                "example_js_execute_command: unload dispatch failed: {}",
                std::string{err.data, static_cast<size_t>(err.size)}
            )
        );
        agentxx::plugin::PluginString::free(ctx.host, &err);
    }
    if (!h) {
        delete state;
        return false;
    }
    return true;
}

/// 提交脚本加载 (异步): 校验脚本文件后把 load 交给 "interpreter.js" 能力
/// - 失败直接 `done(FAILED, 原因)`: 主程序靠完成包判定本次 create 成败, 不能只写 error
void beginScriptLoad(ShellCtx& ctx, const PluginxxOperatorNotify& notify, PluginxxString* error) {
    auto fail = [&](const std::string& msg) {
        if (error) {
            agentxx::plugin::PluginString::set(ctx.host, error, msg);
        }
        auto sv = agentxx::plugin::PluginStringView::from(msg.data(), msg.size());
        notify.done(notify.host_ud, PLUGINXX_OPERATOR_FAILED, &sv);
    };
    if (!ctx.iface.capabilities || !ctx.iface.capabilities->invoke_capability) {
        fail("example_js_execute_command create: host lacks capabilities interface");
        return;
    }
    // 不单查 capability_info: 解释器能力没注册时下面的 invoke_capability 就不受理,
    // 与“先查再调”等价, 且不必同步等待一次表调用
    if (ctx.scriptPath.empty() || !fileExists(ctx.scriptPath)) {
        fail(
            "example_js_execute_command create: plugin.js not found next to the plugin "
            "library"
        );
        return;
    }
    auto           capSv = agentxx::plugin::PluginStringView::fromCstr(kInterpreterCapability);
    std::string    args   = scriptArgsJson(ctx);
    auto           argsSv = agentxx::plugin::PluginStringView::from(args.data(), args.size());
    PluginxxString err{nullptr, 0};
    auto*          state = new std::pair<ShellCtx*, PluginxxOperatorNotify>(&ctx, notify);
    auto*          h     = ctx.iface.capabilities->invoke_capability(
        ctx.host,
        nullptr,
        &capSv,
        &argsSv,
        0,
        -1,
        &onScriptLoadDone,
        state,
        &err
    );
    if (!h) {
        delete state;
        std::string msg = err.data ? std::string(err.data, static_cast<size_t>(err.size))
                                   : std::string("load script async dispatch failed");
        if (err.data) {
            agentxx::plugin::PluginString::free(ctx.host, &err);
        }
        fail(msg);
        return;
    }
    if (err.data) {
        agentxx::plugin::PluginString::free(ctx.host, &err);
    }
    shellLog(&ctx, 2, fmt::format("example_js_execute_command: loading script {}", ctx.scriptPath));
    // 已受理: 完成通知在加载回调中发出
}

/// 从 `get_own_info` 的 JSON 里取插件名与库路径, 定位同目录 plugin.js
/// - `return` 空串 = 成功; 非空 = 失败原因
std::string resolveOwnInfo(ShellCtx& ctx, const std::string& infoJson) {
    const auto& iface = ctx.iface;
    if (!iface.json || !iface.json->json_get_string) {
        return "host lacks json interface";
    }
    auto infoSv = agentxx::plugin::PluginStringView::from(infoJson.data(), infoJson.size());
    auto field  = [&](const char* key) -> std::string {
        PluginxxString v{nullptr, 0};
        auto           keySv = agentxx::plugin::PluginStringView::fromCstr(key);
        iface.json->json_get_string(ctx.host, &infoSv, &keySv, &v);
        if (!v.data) {
            return {};
        }
        std::string s(v.data, static_cast<size_t>(v.size));
        agentxx::plugin::PluginString::free(ctx.host, &v);
        return s;
    };
    std::string libPath = field("path");
    ctx.name            = field("name");
    if (ctx.name.empty() || libPath.empty()) {
        return "own info has no name/path";
    }
    ctx.dir = dirOf(libPath);
    {
        std::error_code ec;
        if (std::filesystem::is_directory(libPath, ec)) {
            ctx.dir = libPath;
        }
    }
    ctx.scriptPath = fmt::format("{}/plugin.js", ctx.dir);
    if (!fileExists(ctx.scriptPath)) {
        auto pos = ctx.dir.find_last_of("/\\");
        if (pos != std::string::npos) {
            std::string parent = ctx.dir.substr(0, pos);
            if (fileExists(fmt::format("{}/plugin.js", parent))) {
                ctx.dir        = parent;
                ctx.scriptPath = fmt::format("{}/plugin.js", parent);
            }
        }
    }
    // 开发期回退: 若按 get_own_info 推导的 plugin.js 不存在, 尝试源码目录
    // (host 传入的 path 在内置模式下可能为虚路径, 此分支仅为本地调试保留)
    if (!fileExists(ctx.scriptPath)) {
        std::string fallback = "agent/plugins/example_js_execute_command/plugin.js";
        if (fileExists(fallback)) {
            ctx.scriptPath = fallback;
        }
    }
    return {};
}

/// 自身清单读取完成 (create 的第一步收尾): 解析名称/路径 → 提交脚本加载
void PLUGINXX_CALL
    onOwnInfoDone(void* ud, int32_t status, const PluginxxStringView* payload) {
    auto* state = static_cast<std::pair<ShellCtx*, PluginxxOperatorNotify>*>(ud);
    if (!state) {
        return;
    }
    ShellCtx*              ctx    = state->first;
    PluginxxOperatorNotify notify = state->second;
    delete state;
    if (!ctx) {
        notify.done(notify.host_ud, PLUGINXX_OPERATOR_FAILED, nullptr);
        return;
    }
    std::string text;
    if (payload && payload->data) {
        text.assign(payload->data, static_cast<size_t>(payload->size));
    }
    std::string reason;
    if (status != PLUGINXX_OPERATOR_OK) {
        reason = text.empty() ? std::string("get_own_info rejected") : text;
    } else if (text.empty()) {
        reason = "get_own_info returned empty";
    } else {
        reason = resolveOwnInfo(*ctx, text);
    }
    if (!reason.empty()) {
        shellLog(
            ctx,
            4,
            fmt::format("example_js_execute_command: own info unavailable: {}", reason)
        );
        auto sv = agentxx::plugin::PluginStringView::from(reason.data(), reason.size());
        notify.done(notify.host_ud, PLUGINXX_OPERATOR_FAILED, &sv);
        return;
    }
    beginScriptLoad(*ctx, notify, nullptr);
}

} // namespace

extern "C" PLUGINXX_EXPORT int32_t agentxx_agent_plugin_get_info(PluginxxInfo* out) {
    return agentxx::plugin::detail::fillAgentPluginInfo(
        out,
        "example_js_execute_command",
        "1.0.0",
        "Execute JavaScript code (JS equivalent of bash command) via QuickJS"
    );
}

/// create: 首次构造上下文 (查接口表), 然后读自身清单定位同目录 plugin.js 并把它
/// 交给引擎执行; 可重入: 再次 create 只重跑脚本加载。
///
/// **create 跑在插件执行线程上**, 期间不能同步等接口表 (会被拒), 所以自身清单读取
/// 与脚本加载两步都是“提交 + 完成回调”。
static void jsShellAgentStart(ShellCtx&, const PluginxxOperatorNotify*, PluginxxString*);

extern "C" PLUGINXX_EXPORT void agentxx_agent_plugin_create(
    const PluginxxHost*           host,
    void**                        plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    agentxx::plugin::detail::createCtxEntry<ShellCtx>(
        host,
        plugin_ctx,
        notify,
        err,
        "example_js_execute_command",
        [](ShellCtx& ctx) -> std::string {
            const auto& iface = ctx.iface;
            if (!iface.capabilities || !iface.plugins || !iface.json || !iface.log) {
                return "example_js_execute_command: host lacks capabilities/plugins/json/log";
            }
            return {};
        },
        [](ShellCtx& c, const PluginxxOperatorNotify* n, PluginxxString* e) {
            jsShellAgentStart(c, n, e);
        }
    );
}


/// 注册事务 (create 的实际内容): 取自身清单(首次) → 加载脚本
/// - 任何失败都 `done(FAILED, 原因)`: 只写 error 不通知会让主程序一直等。
static void
    jsShellAgentStart(ShellCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString* error) {
    auto fail = [&](const std::string& msg) {
        if (error) {
            agentxx::plugin::PluginString::set(ctx.host, error, msg);
        }
        if (notify && notify->done) {
            auto sv = agentxx::plugin::PluginStringView::from(msg.data(), msg.size());
            notify->done(notify->host_ud, PLUGINXX_OPERATOR_FAILED, &sv);
        }
    };
    if (!notify || !notify->done) {
        fail("example_js_execute_command create: notify required");
        return;
    }
    if (ctx.scriptLoaded) {
        // 重复 create (幂等): 脚本已由本实例加载
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
        return;
    }
    if (!ctx.scriptPath.empty()) {
        beginScriptLoad(ctx, *notify, error);
        return;
    }

    // 首次: 自身清单必须异步读 —— create 跑在插件执行线程上, 同步等表调用会被拒
    auto*          state = new std::pair<ShellCtx*, PluginxxOperatorNotify>(&ctx, *notify);
    PluginxxString err{nullptr, 0};
    void*          h = ctx.iface.plugins->get_own_info(ctx.host, &onOwnInfoDone, state, &err);
    if (!h) {
        delete state;
        std::string msg = err.data ? std::string(err.data, static_cast<size_t>(err.size))
                                   : std::string("get_own_info was not accepted");
        if (err.data) {
            agentxx::plugin::PluginString::free(ctx.host, &err);
        }
        fail(msg);
        return;
    }
    if (err.data) {
        agentxx::plugin::PluginString::free(ctx.host, &err);
    }
    // 已受理: 后续直到脚本加载完成才发完成通知
}

/// stop 事务: 通知引擎卸载脚本 (撤销脚本注册的工具/订阅/定时器)。
/// - 引擎已停用/卸载时卸载调用失败: 只记录, 脚本上下文由引擎的停止路径释放;
/// - 卸载 op 自身持有本实例 caller lease, 主程序会等它完成后才 destroy;
/// - 本函数同步完成 stop 本身 (卸载为 fire-and-forget, 无阻塞等待)。
static void    jsShellAgentStop(ShellCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString*) {
    if (!notify || !notify->done) {
        return;
    }
    if (dispatchScriptUnload(ctx, notify)) {
        return; ///< 已受理: 完成通知在卸载回调里发出 (脚本注册先撤净)
    }
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
}

AGENTXX_AGENT_PLUGIN_LIFECYCLE_EXPORT(
    ShellCtx,
    jsShellAgentStop, nullptr, nullptr)

extern "C" PLUGINXX_EXPORT void agentxx_agent_plugin_destroy(
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    agentxx::plugin::detail::destroyCtxEntry<ShellCtx>(
        plugin_ctx,
        notify,
        err,
        [](ShellCtx* ctx) { delete ctx; }
    );
}
