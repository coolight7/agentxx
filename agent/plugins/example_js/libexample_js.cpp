/// libexample_js.so —— example_js 插件的 C++ 壳 (统一插件模型示例)
///
/// 双入口: 同一个动态库既做 agent 侧插件, 也做 client 侧插件
/// - 两侧壳逻辑完全一样 (能力校验 → 把同目录 plugin.js 交给 `interpreter.js`
///   引擎执行 → 停用时卸载脚本), 差别只在 create 阶段查询哪一侧的接口表;
/// - 脚本自己用 `agentxx.side` 判断跑在哪一侧, 各注册自己那侧的东西。
///
/// 实例生命周期: create 只构造, start 加载脚本, stop 停止脚本执行
/// - create: 只构造上下文 (查询接口表 + 解析自身路径), 不注册、不启动线程;
/// - start:  校验 "interpreter.js" 能力可用后, 把同目录 plugin.js 交给引擎执行,
///           脚本内注册的工具/订阅/展示项全部属于本实例的注册事务;
/// - stop:   通知引擎卸载脚本 (撤销脚本注册), 引擎不可用时只记录;
/// - destroy: 只释放本地状态, 不再调用主程序接口。
#include "agentxx/plugin/api/client_plugin_api.h"
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"

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

/// create 的公共主体: 查命侧接口表 → 解析自身名称与路径 → 定位 plugin.js
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
    auto logE = [&](const std::string& msg) {
        auto sv = agentxx::plugin::PluginStringView::from(msg.data(), msg.size());
        ctx->log->log(host, 4, &sv);
    };

    // agent 侧 `plugins.get_own_info` 是异步操作契约: 用阻塞形态取回清单 JSON
    // (client 侧的 `self.get_own_info` 仍是同步入口, 这里保持原样)
    std::string infoJson;
    if (clientSide) {
        PluginxxString info{nullptr, 0};
        if (ctx->client.self->get_own_info(host, &info) != 0 || !info.data) {
            logE("example_js: get_own_info failed");
            return -1;
        }
        infoJson.assign(info.data, static_cast<size_t>(info.size));
        agentxx::plugin::PluginString::free(host, &info);
    } else {
        infoJson = agentxx::plugin::detail::tableOpText(
            host,
            ctx->agent.cancel,
            ctx->agent.scheduler,
            [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return ctx->agent.plugins->get_own_info(host, cb, ud, err);
            }
        );
        if (infoJson.empty()) {
            logE("example_js: get_own_info failed");
            return -1;
        }
    }
    auto infoSv = agentxx::plugin::PluginStringView::from(infoJson.data(), infoJson.size());
    auto field  = [&](const char* key) -> std::string {
        PluginxxString v{nullptr, 0};
        auto           keySv = agentxx::plugin::PluginStringView::fromCstr(key);
        ctx->json->json_get_string(host, &infoSv, &keySv, &v);
        if (!v.data) {
            return {};
        }
        std::string s(v.data, static_cast<size_t>(v.size));
        agentxx::plugin::PluginString::free(host, &v);
        return s;
    };
    std::string libPath = field("path");
    ctx->name           = field("name");
    if (ctx->name.empty() || libPath.empty()) {
        logE("example_js: own info invalid");
        return -1;
    }
    ctx->dir = dirOf(libPath);
    // path 可能是插件目录 (client 侧记录的加载路径就是目录), 也可能是库文件本身
    {
        std::error_code ec;
        if (std::filesystem::is_directory(libPath, ec)) {
            ctx->dir = libPath;
        }
    }

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
    *plugin_ctx = ctx.release();
    return 0;
}

/// start 事务: 依赖能力校验 + 脚本加载 (两侧共用)。
/// - 加载经 "interpreter.js" 能力异步执行, 脚本注册的工具/展示项属于本实例;
///   因此 start 必须等加载结束才 done (返回主程序托管句柄, 完成回调上报结果);
/// - 能力不可用或脚本缺失 → 返回 NULL + error (拒绝); 加载失败 → done FAILED;
///   两条路径都由主程序回滚本次加载, 不留注册残留。
void* shellStart(ShellCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString* error) {
    auto setErr = [&](const std::string& msg) -> void* {
        if (error) {
            agentxx::plugin::PluginString::set(ctx.host, error, msg);
        }
        return nullptr;
    };
    if (!notify || !notify->done) {
        return setErr("example_js start: notify required");
    }
    if (!ctx.capabilities || !ctx.capabilities->capability_info
        || !ctx.capabilities->invoke_capability) {
        return setErr("example_js start: host lacks capabilities interface");
    }
    auto capSv = agentxx::plugin::PluginStringView::fromCstr(kInterpreterCapability);
    // 表成员是异步契约: start 事务在主程序 IO 线程上, 主程序内联完成
    // 两侧的取消/调度表在各自的聚合里 (壳按 side 选用)
    const bool hasCap
        = pluginxx::tableOpBlockingRaw(
              ctx.host,
              ctx.clientSide ? ctx.client.cancel : ctx.agent.cancel,
              ctx.clientSide ? ctx.client.scheduler : ctx.agent.scheduler,
              [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                  return ctx.capabilities->capability_info(ctx.host, &capSv, cb, ud, err);
              }
          )
              .ok();
    if (!hasCap) {
        return setErr("example_js start: interpreter.js capability not available");
    }
    if (!fileExists(ctx.scriptPath)) {
        return setErr("example_js start: plugin.js not found next to the plugin library");
    }
    if (ctx.scriptLoaded) {
        // 重复 start (幂等): 脚本已由本实例加载
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
        return nullptr;
    }

    std::string    args   = scriptArgsJson(ctx);
    auto           argsSv = agentxx::plugin::PluginStringView::from(args.data(), args.size());
    PluginxxString err{nullptr, 0};
    auto*          state = new std::pair<ShellCtx*, PluginxxOperatorNotify>(&ctx, *notify);
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
    shellLog(&ctx, 2, fmt::format("example_js: loading script {}", ctx.scriptPath));
    return &ctx.opToken; ///< 已接受: 完成通知在加载回调中发出
}

/// stop 事务: 通知引擎卸载脚本 (撤销脚本注册的工具/订阅/展示项)。
/// - 引擎已停用/卸载时卸载调用失败: 只记录, 脚本上下文由引擎的停止路径释放;
/// - 卸载 op 自身持有本实例 caller lease, 主程序会等它完成后才 destroy;
/// - 本函数同步完成 stop 本身 (卸载为 fire-and-forget, 无阻塞等待)。
void* shellStop(ShellCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString*) {
    if (!notify || !notify->done) {
        return nullptr;
    }
    if (dispatchScriptUnload(ctx, notify)) {
        return &ctx.opToken; ///< 已受理: 完成通知在卸载回调里发出 (脚本注册先撤净)
    }
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

} // namespace

AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT(ShellCtx, shellStart, shellStop, nullptr, nullptr)
AGENTXX_PLUGIN_CLIENT_LIFECYCLE_EXPORT(ShellCtx, shellStart, shellStop)

extern "C" PLUGINXX_EXPORT const PluginxxInfo* agentxx_plugin_agent_get_info(void) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const PluginxxInfo* {
            static const PluginxxInfo info{
                PLUGINXX_API_VERSION,
                0,
                agentxx::plugin::PluginStringView::fromCstr("example_js"),
                agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
                agentxx::plugin::PluginStringView::fromCstr(
                    "Example JS plugin (C++ shell + JS via interpreter.js capability)"
                ),
            };
            return &info;
        }
    );
}

extern "C" PLUGINXX_EXPORT const AgentxxClientPluginInfo* agentxx_plugin_client_get_info(void) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        static_cast<const AgentxxClientPluginInfo*>(nullptr),
        [&]() -> const AgentxxClientPluginInfo* {
            static const AgentxxClientPluginInfo info{
                AGENTXX_CLIENT_PLUGIN_API_VERSION,
                0,
                agentxx::plugin::PluginStringView::fromCstr("example_js"),
                agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
                agentxx::plugin::PluginStringView::fromCstr(
                    "Example JS plugin, client side (C++ shell + JS via interpreter.js)"
                ),
            };
            return &info;
        }
    );
}

/// agent 侧 create: 只构造上下文 (查询接口表 + 解析自身名称/路径), 不做运行时注册,
/// 不启动线程, 不调用能力。
extern "C" PLUGINXX_EXPORT int
    agentxx_plugin_agent_create(const PluginxxHost* host, void** plugin_ctx) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        -1,
        [&]() -> int {
            return createShell(host, /*clientSide=*/false, plugin_ctx);
        }
    );
}

/// client 侧 create: 同上, 只是查 client 侧接口表
extern "C" PLUGINXX_EXPORT int
    agentxx_plugin_client_create(const PluginxxHost* host, void** plugin_ctx) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        -1,
        [&]() -> int {
            return createShell(host, /*clientSide=*/true, plugin_ctx);
        }
    );
}

extern "C" PLUGINXX_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<ShellCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(
        [ctx](const char* msg) noexcept {
            shellLog(ctx, 4, msg ? msg : "");
        },
        [&] {
            if (!ctx) {
                return;
            }
            // destroy 只处理已经停止的对象: 不再调用主程序接口、不创建异步工作
            // (脚本注销与上下文释放由 stop 与引擎的停止路径完成)
            delete ctx;
        }
    );
}

extern "C" PLUGINXX_EXPORT void agentxx_plugin_client_destroy(void* plugin_ctx) {
    agentxx_plugin_agent_destroy(plugin_ctx);
}
