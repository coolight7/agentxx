/// example_resources —— 会话资源示例插件 (plugin_api v1 COM 风格接口表)
///
/// 演示插件向宿主贡献 Skill / Memory / MCP 组件的两种通道:
///
/// 1) 声明式: plugin.yaml 的 skill/memory/mcp 段 —— 宿主在本插件 entry 成功后
///    自动应用 (加载失败则声明资源不生效); 相对路径按插件目录解析。
///    本插件声明:
///      - skill: skills/            (含 hello_skill/SKILL.md)
///      - memory: assets/NOTES.md   (内容注入系统提示词)
///      - mcp: example_time         (示例 URL, 连接失败仅记日志可观察)
///
/// 2) 运行时: entry 内经 agentxx.agent.resources 接口表 register_skill_dir /
///    register_memory_file / register_mcp_server 实时注册。本插件注册:
///      - skill: skills_runtime/    (含 extra_skill/SKILL.md)
///    MCP 运行时注册格式 (默认注释, 避免真实网络请求):
///      spec_json = {"namespace":"...","url":"https://...","timeout":60}
///
/// 所有权语义: 经本插件注册/声明的资源在卸载时由宿主自动摘除,
/// 禁用时摘除、启用时恢复 (与工具行为一致), 插件无需手动清理;
/// unload 回调中的显式反注册仅为 SDK 惯例示范。
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"

#include "fmt/format.h"
#include <memory>
#include <string>

// 多实例约定 (2026-08 API v1): 零可变全局; 实例状态 (host/iface) 存于
// ResCtx, create 经 *plugin_ctx 交付宿主 / destroy 释放
struct ResCtx {
    const AgentxxPluginHost*     host = nullptr;
    agentxx::plugin::AgentIfaces iface{};

    auto logger() const noexcept {
        return [this](const char* msg) noexcept {
            logErr(msg ? msg : "");
        };
    }

    void logErr(const std::string& msg) const {
        if (host && iface.log && iface.log->log) {
            auto sv = agentxx::plugin::PluginStringView::from(msg.data(), msg.size());
            iface.log->log(host, 4, &sv);
        }
    }
};

/// 从 get_own_info JSON 中提取字段值 (host->alloc, 用完 free)
static std::string ownInfoString(
    const AgentxxPluginHost*            host,
    const agentxx::plugin::AgentIfaces& iface,
    const char*                         key
) {
    if (!iface.plugins || !iface.plugins->get_own_info || !iface.json
        || !iface.json->json_get_string) {
        return {};
    }
    AgentxxPluginString info{nullptr, 0};
    iface.plugins->get_own_info(host, &info);
    if (!info.data) {
        return {};
    }
    std::string         out;
    AgentxxPluginString val{nullptr, 0};
    auto                infoSv = agentxx::plugin::PluginStringView::toSv(&info);
    auto                keySv  = agentxx::plugin::PluginStringView::fromCstr(key);
    iface.json->json_get_string(host, &infoSv, &keySv, &val);
    if (val.data) {
        out.assign(val.data, static_cast<size_t>(val.size));
        agentxx::plugin::PluginString::free(host, &val);
    }
    agentxx::plugin::PluginString::free(host, &info);
    return out;
}

/// 库文件路径 → 所在目录 (get_own_info 的 path 为库路径, 资源按"库路径所在
/// 目录"推导, 与 example_js 壳的 dirOf 约定一致)
static std::string dirOf(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return ".";
    }
    return path.substr(0, pos);
}

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    // C ABI 边界异常守卫: 异常返回 NULL; 本边界为纯静态元数据 → 空操作日志
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
            static const AgentxxPluginInfo info{
                AGENTXX_PLUGIN_API_VERSION,
                0,
                agentxx::plugin::PluginStringView::fromCstr("example_resources"),
                agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
                agentxx::plugin::PluginStringView::fromCstr(
                    "Example plugin contributing skill/memory/mcp resources "
                    "(declarative manifest + runtime agentxx.agent.resources interface)"
                ),
            };
            return &info;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_agent_create(const AgentxxPluginHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: 异常返回 -1 (创建失败); 日志闭包捕获局部裸指针
    auto    ctx = std::make_unique<ResCtx>();
    ResCtx* raw = nullptr;
    return agentxx::plugin::guardCall(
        [&raw](const char* m) noexcept {
            if (raw) {
                raw->logErr(m);
            }
        },
        -1,
        [&]() -> int {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            ctx->host = host;
            // COM 风格接口表查询 (存入本实例上下文; 原函数级 static 缓存多实例不安全)
            ctx->iface = agentxx::plugin::AgentIfaces::query(host);
            raw        = ctx.get();

            // create 只构造上下文与查询接口: 资源注册属于 start 事务
            return 0;
        }
    );
}

/// ---------------- 实例生命周期 (create 构造 / start 注册 / stop 撤销) ----------------
///
/// 入口语义 (见 docs/zh-cn/design/plugins.md 第 15 节):
/// - `create`: 只分配上下文、查询接口, 不提交运行时注册。
/// - `start`: 注册事务 (运行时 skill 目录); 失败返回 NULL + error, 宿主按拒绝
///   处理并回滚本次已生效的注册与声明式资源。
/// - `stop`: 本插件没有自管线程/定时器, 只给出完成信号; 资源由宿主在 stop 后
///   统一摘除 (skills/memory/mcp 的 owner 记录), 这里不重复反注册。
/// - `destroy`: 只释放本地内存, 不创建异步工作、不调用宿主注册接口。

static void* resAgentStart(
    ResCtx&                            ctx,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               error
) {
    if (!notify) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "example_resources start: notify required"
            );
        }
        return nullptr;
    }
    const AgentxxPluginHost* host = ctx.host;

    // ---- 运行时注册: 追加 skill 目录 (声明式段见 plugin.yaml) ----
    // - 与 yaml 主配置或其他插件冲突时返回非 0 (yaml 优先, 此处仅告警不失败);
    //   start 事务本身仍成功, 已生效的注册由宿主在 stop 后统一撤销。
    if (ctx.iface.resources && ctx.iface.resources->register_skill_dir && ctx.iface.log
        && ctx.iface.log->log) {
        auto        base            = dirOf(ownInfoString(host, ctx.iface, "path"));
        std::string runtimeSkillDir = fmt::format("{}/skills_runtime", base);
        auto        skillDirSv      = agentxx::plugin::PluginStringView::from(
            runtimeSkillDir.data(),
            runtimeSkillDir.size()
        );
        if (ctx.iface.resources->register_skill_dir(host, &skillDirSv) != 0) {
            auto warnSv = agentxx::plugin::PluginStringView::fromCstr(
                "[example_resources] register runtime skill dir failed"
            );
            ctx.iface.log->log(host, 3, &warnSv);
        } else {
            auto infoSv = agentxx::plugin::PluginStringView::fromCstr(
                "[example_resources] runtime skill dir registered: skills_runtime/"
            );
            ctx.iface.log->log(host, 2, &infoSv);
        }
    }

    // ---- 运行时注册 MCP server 示例 (注释状态; 声明式段已示范配置格式) ----
    // std::string spec = std::string("{\"namespace\":\"example_calc\",\"url\":\"")
    //     + "https://mcp.example.com/calc\",\"timeout\":30}";
    // ctx.iface.resources->register_mcp_server(host,
    // agentxx::plugin::PluginStringView::from(spec.data(), spec.size()));

    if (ctx.iface.log && ctx.iface.log->log) {
        auto infoSv
            = agentxx::plugin::PluginStringView::fromCstr("[example_resources] plugin started");
        ctx.iface.log->log(host, 2, &infoSv);
    }
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

static void*
    resAgentStop(ResCtx&, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString*) {
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT(ResCtx, resAgentStart, resAgentStop)

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    // C ABI 边界异常守卫: 销毁回调异常不得外泄。
    // 宿主已在 stop 后摘除本插件的全部资源 (skill/memory/mcp), destroy 只释放内存。
    auto* ctx = static_cast<ResCtx*>(plugin_ctx);
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
