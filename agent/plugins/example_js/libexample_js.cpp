/// libexample_js.so —— example_js 插件的 C++ 壳 (统一插件模型示例)
///
/// 双入口: 同一个动态库既做 agent 侧插件, 也做 client 侧插件
/// - 两侧壳逻辑完全一样 (能力校验 → 把同目录 plugin.js 交给 `interpreter.js`
///   引擎执行 → 停用时卸载脚本), 差别只在 create 阶段查询哪一侧的接口表;
/// - 脚本自己用 `agentxx.side` 判断跑在哪一侧, 各注册自己那侧的东西。
///
/// 实例生命周期: create 构造上下文并加载脚本, stop 卸载脚本, destroy 释放本地状态
/// - create: 首次构造上下文 (查接口表), 随后异步读自身清单定位同目录 plugin.js,
///           把它交给引擎执行 —— 脚本内注册的工具/订阅/展示项就是本实例的注册事务;
///           可重入: 同一实例再次 create (禁用后启用) 只重跑脚本加载;
/// - stop:   通知引擎卸载脚本 (撤销脚本注册), 引擎不可用时只记录;
/// - destroy: 只释放本地状态, 不再调用主程序接口。
///
/// **create 跑在插件执行线程上**, 期间不能同步等接口表 (会被拒), 所以自身清单读取
/// 与脚本加载两步都是“提交 + 完成回调”, 成败一律经完成通知交回主程序。
#include "agentxx/plugin/api/client_plugin_api.h"
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/agent_plugin_kit.h"
#include "agentxx/plugin/api/client_plugin_kit.h"

#include "fmt/format.h"
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace {

/// JS 引擎声明的能力全名
/// - 引擎以短名 `interpreter.js` 声明, 主程序补成 `plugin.<插件 id>.<名字>`;
///   调用方必须写全名
constexpr const char* kInterpreterCapability
    = "plugin.agentxx_javascript_engine.interpreter.js";

struct ShellCtx {
    const PluginxxHost* host = nullptr;
    /// 本实例服务哪一侧 (create 决定; 之后只读)
    bool clientSide = false;
    /// 两侧的接口表聚合 (只填对应那一侧, 另一侧全空)
    agentxx::plugin::AgentIfaces  agent{};
    agentxx::plugin::ClientIfaces client{};
    /// 两侧通用表 (壳自己用: 能力调用 / JSON 解析 / 日志)
    const PluginxxCapabilitiesIface* capabilities = nullptr;
    const PluginxxJsonIface*         json         = nullptr;
    const PluginxxLogIface*          log          = nullptr;
    std::string                      name;
    std::string                      dir;
    std::string                      scriptPath;
    /// 脚本是否已由引擎加载 (load 成功后置位, stop 撤销; 仅在所属 IO 线程访问)
    bool scriptLoaded = false;
    /// start/stop 的 op 占位句柄 (主程序只当不透明非空 token; 每实例独立)
    int opToken = 0;
};

void shellLog(const ShellCtx* ctx, int level, const std::string& msg) {
    if (ctx && ctx->host && ctx->log && ctx->log->log) {
        auto sv = agentxx::plugin::PluginStringView::from(msg.data(), msg.size());
        ctx->log->log(ctx->host, level, &sv);
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
    return agentxx::plugin::jsonEscape(ctx.host, ctx.json, value);
}

/// "interpreter.js" 能力调用参数: {"op": "load", "name": <插件名>, "path": <脚本路径>}
/// - 能力调用的 args 里带 `op` (能力链没有独立的 method 参数)
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
                shellLog(ctx, 2, fmt::format("example_js: loaded tools {}", text));
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
        shellLog(ctx, 4, fmt::format("example_js: interpreter load failed: {}", msg));
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
                "example_js: interpreter unload failed: {}",
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
    if (!ctx.capabilities || !ctx.capabilities->invoke_capability) {
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
    auto* h = ctx.capabilities->invoke_capability(
        ctx.host,
        nullptr,
        &capSv,
        &argsSv,
        0,
        &onScriptUnloadDone,
        state,
        &err
    );
    if (err.data) {
        if (!h) {
            shellLog(&ctx, 3, fmt::format("example_js: unload dispatch failed: {}", err.data));
        }
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
    if (!ctx.capabilities || !ctx.capabilities->invoke_capability) {
        fail("example_js create: host lacks capabilities interface");
        return;
    }
    // 不单查 capability_info: 解释器能力没注册时下面的 invoke_capability 就不受理,
    // 与“先查再调”等价, 且不必同步等待一次表调用
    if (ctx.scriptPath.empty() || !fileExists(ctx.scriptPath)) {
        fail("example_js create: plugin.js not found next to the plugin library");
        return;
    }
    auto capSv = agentxx::plugin::PluginStringView::fromCstr(kInterpreterCapability);
    std::string    args   = scriptArgsJson(ctx);
    auto           argsSv = agentxx::plugin::PluginStringView::from(args.data(), args.size());
    PluginxxString err{nullptr, 0};
    auto*          state = new std::pair<ShellCtx*, PluginxxOperatorNotify>(&ctx, notify);
    auto*          h     = ctx.capabilities->invoke_capability(
        ctx.host,
        nullptr,
        &capSv,
        &argsSv,
        0,
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
    shellLog(&ctx, 2, fmt::format("example_js: loading script {}", ctx.scriptPath));
    // 已受理: 完成通知在加载回调中发出
}

/// 从 `get_own_info` 的 JSON 里取插件名与库路径, 定位同目录 plugin.js
/// - `return` 空串 = 成功; 非空 = 失败原因
std::string resolveOwnInfo(ShellCtx& ctx, const std::string& infoJson) {
    if (!ctx.json || !ctx.json->json_get_string) {
        return "host lacks json interface";
    }
    auto infoSv = agentxx::plugin::PluginStringView::from(infoJson.data(), infoJson.size());
    auto field  = [&](const char* key) -> std::string {
        PluginxxString v{nullptr, 0};
        auto           keySv = agentxx::plugin::PluginStringView::fromCstr(key);
        ctx.json->json_get_string(ctx.host, &infoSv, &keySv, &v);
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
    // path 可能是插件目录 (client 侧记录的加载路径就是目录), 也可能是库文件本身
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
        shellLog(ctx, 4, fmt::format("example_js: own info unavailable: {}", reason));
        auto sv = agentxx::plugin::PluginStringView::from(reason.data(), reason.size());
        notify.done(notify.host_ud, PLUGINXX_OPERATOR_FAILED, &sv);
        return;
    }
    beginScriptLoad(*ctx, notify, nullptr);
}

/// create 的构造部分: 查命侧接口表 (不调表: 自身清单在 shellStart 里异步读)
///
/// - agent 侧: `plugins.get_own_info` + `AgentIfaces`
/// - client 侧: `self.get_own_info` + `ClientIfaces`
int createShell(const PluginxxHost* host, bool clientSide, void** plugin_ctx) {
    if (!host || !host->vtable || !plugin_ctx) {
        return -1;
    }
    auto ctx          = std::make_unique<ShellCtx>();
    ctx->host         = host;
    ctx->clientSide   = clientSide;
    ctx->capabilities = pluginxx::queryInterface<PluginxxCapabilitiesIface>(
        host,
        PLUGINXX_IFACE_CAPABILITIES
    );
    ctx->json  = pluginxx::queryInterface<PluginxxJsonIface>(host, PLUGINXX_IFACE_JSON);
    ctx->log   = pluginxx::queryInterface<PluginxxLogIface>(host, PLUGINXX_IFACE_LOG);
    if (clientSide) {
        ctx->client = agentxx::plugin::ClientIfaces::query(host);
        if (!ctx->client.self || !ctx->client.self->get_own_info) {
            return -1;
        }
    } else {
        ctx->agent = agentxx::plugin::AgentIfaces::query(host);
        if (!ctx->agent.plugins || !ctx->agent.plugins->get_own_info) {
            return -1;
        }
    }
    if (!ctx->capabilities || !ctx->json || !ctx->log) {
        return -1;
    }
    *plugin_ctx = ctx.release();
    return 0;
}

/// 注册事务 (create 的实际内容, 两侧共用): 校验能力 → 读自身清单 → 加载脚本
///
/// - 两个异步步骤串起来: 首次 create 先经接口表取自身清单 (`get_own_info`),
///   拿到库路径后定位同目录 plugin.js, 再交给 `interpreter.js` 能力执行;
/// - 脚本注册的工具/展示项属于本实例, 所以 create 必须等脚本加载完才 done;
/// - 任何失败都 `done(FAILED, 原因)` —— 只写 error 不通知会让主程序一直等;
///   失败路径由主程序回滚本次加载, 不留注册残留。
void shellStart(ShellCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString* error) {
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
        fail("example_js create: notify required");
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
    void*          h = nullptr;
    if (ctx.clientSide) {
        h = ctx.client.self->get_own_info(ctx.host, &onOwnInfoDone, state, &err);
    } else {
        h = ctx.agent.plugins->get_own_info(ctx.host, &onOwnInfoDone, state, &err);
    }
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

/// stop 事务: 通知引擎卸载脚本 (撤销脚本注册的工具/订阅/展示项)。
/// - 引擎已停用/卸载时卸载调用失败: 只记录, 脚本上下文由引擎的停止路径释放;
/// - 卸载 op 自身持有本实例 caller lease, 主程序会等它完成后才 destroy;
/// - 本函数同步完成 stop 本身 (卸载为 fire-and-forget, 无阻塞等待)。
void shellStop(ShellCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString*) {
    if (!notify || !notify->done) {
        return;
    }
    if (dispatchScriptUnload(ctx, notify)) {
        return; ///< 已受理: 完成通知在卸载回调里发出 (脚本注册先撤净)
    }
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
}

} // namespace

AGENTXX_AGENT_PLUGIN_LIFECYCLE_EXPORT(ShellCtx, shellStop, nullptr, nullptr)
AGENTXX_CLIENT_PLUGIN_LIFECYCLE_EXPORT(ShellCtx, shellStop)

extern "C" PLUGINXX_EXPORT int32_t agentxx_agent_plugin_get_info(PluginxxInfo* out) {
    return agentxx::plugin::detail::fillAgentPluginInfo(
        out,
        "example_js",
        "1.0.0",
        "Example JS plugin (C++ shell + JS via interpreter.js capability)"
    );
}

extern "C" PLUGINXX_EXPORT int32_t agentxx_client_plugin_get_info(PluginxxInfo* out) {
    return agentxx::plugin::detail::fillClientPluginInfo(
        out,
        "example_js",
        "1.0.0",
        "Example JS plugin, client side (C++ shell + JS via interpreter.js)"
    );
}

/// create 的公共体: 首次构造上下文 (可重入时跳过) → 注册事务由 shellStart 完成
static void shellCreate(
    const PluginxxHost*           host,
    void**                        plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err,
    bool                          clientSide
) {
    if (!plugin_ctx) {
        if (notify && notify->done) {
            notify->done(notify->host_ud, PLUGINXX_OPERATOR_FAILED, nullptr);
        }
        return;
    }
    if (*plugin_ctx == nullptr) {
        if (createShell(host, clientSide, plugin_ctx) != 0) {
            if (notify && notify->done) {
                notify->done(notify->host_ud, PLUGINXX_OPERATOR_FAILED, nullptr);
            }
            return;
        }
    }
    auto* ctx = static_cast<ShellCtx*>(*plugin_ctx);
    (void)shellStart(*ctx, notify, err);
}

extern "C" PLUGINXX_EXPORT void agentxx_agent_plugin_create(
    const PluginxxHost*           host,
    void**                        plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    shellCreate(host, plugin_ctx, notify, err, /*clientSide=*/false);
}

extern "C" PLUGINXX_EXPORT void agentxx_client_plugin_create(
    const PluginxxHost*           host,
    void**                        plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    shellCreate(host, plugin_ctx, notify, err, /*clientSide=*/true);
}

extern "C" PLUGINXX_EXPORT void agentxx_agent_plugin_destroy(
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    auto* ctx = static_cast<ShellCtx*>(plugin_ctx);
    // destroy 只处理已经停止的对象: 不再调用主程序接口、不创建异步工作
    // (脚本注销与上下文释放由 stop 与引擎的停止路径完成)
    agentxx::plugin::detail::destroyCtxEntry<ShellCtx>(
        plugin_ctx,
        notify,
        err,
        [](ShellCtx* c) { delete c; }
    );
    (void)ctx;
}

extern "C" PLUGINXX_EXPORT void agentxx_client_plugin_destroy(
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    agentxx_agent_plugin_destroy(plugin_ctx, notify, err);
}
