/*
 * example_resources —— 会话资源示例插件 (plugin_api v1 COM 风格接口表)
 *
 * 演示插件向宿主贡献 Skill / Memory / MCP 组件的两种通道:
 *
 * 1) 声明式: plugin.yaml 的 skill/memory/mcp 段 —— 宿主在本插件 entry 成功后
 *    自动应用 (加载失败则声明资源不生效); 相对路径按插件目录解析。
 *    本插件声明:
 *      - skill: skills/            (含 hello_skill/SKILL.md)
 *      - memory: assets/NOTES.md   (内容注入系统提示词)
 *      - mcp: example_time         (示例 URL, 连接失败仅记日志可观察)
 *
 * 2) 运行时: entry 内经 agentxx.agent.resources 接口表 register_skill_dir /
 *    register_memory_file / register_mcp_server 实时注册。本插件注册:
 *      - skill: skills_runtime/    (含 extra_skill/SKILL.md)
 *    MCP 运行时注册格式 (默认注释, 避免真实网络请求):
 *      spec_json = {"namespace":"...","url":"https://...","timeout":60}
 *
 * 所有权语义: 经本插件注册/声明的资源在卸载时由宿主自动摘除,
 * 禁用时摘除、启用时恢复 (与工具行为一致), 插件无需手动清理;
 * unload 回调中的显式反注册仅为 SDK 惯例示范。
 */
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_iface_helper.h"

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
            iface.log->log(host, 4, agentxx_plugin_sv(msg.data(), msg.size()));
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
    std::string out;
    AgentxxPluginString val{nullptr, 0};
    auto infoSv = agentxx_plugin_string_to_sv(&info);
    auto keySv  = agentxx_plugin_sv_cstr(key);
    iface.json->json_get_string(host, &infoSv, &keySv, &val);
    if (val.data) {
        out.assign(val.data, static_cast<size_t>(val.size));
        agentxx_plugin_string_free(host, &val);
    }
    agentxx_plugin_string_free(host, &info);
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
                AGENTXX_PLUGIN_API_VERSION, 0,
                agentxx_plugin_sv_cstr("example_resources"),
                agentxx_plugin_sv_cstr("1.0.0"),
                agentxx_plugin_sv_cstr(
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
            ctx->iface       = agentxx::plugin::AgentIfaces::query(host);
            raw              = ctx.get();
            const auto& g_if = ctx->iface;

            auto base = dirOf(ownInfoString(host, ctx->iface, "path"));

            // ---- 运行时注册: 追加 skill 目录 (声明式段见 plugin.yaml) ----
            // - 与 yaml 主配置或其他插件冲突时返回非 0 (yaml 优先, 此处仅告警不失败)
            if (ctx->iface.resources && ctx->iface.resources->register_skill_dir && ctx->iface.log
                && ctx->iface.log->log) {
                std::string runtimeSkillDir = fmt::format("{}/skills_runtime", base);
                if (ctx->iface.resources->register_skill_dir(
                        host,
                        agentxx_plugin_sv(runtimeSkillDir.data(), runtimeSkillDir.size())
                    )
                    != 0) {
                    ctx->iface.log->log(
                        host,
                        3,
                        agentxx_plugin_sv_cstr(
                            "[example_resources] register runtime skill dir failed"
                        )
                    );
                } else {
                    ctx->iface.log->log(
                        host,
                        2,
                        agentxx_plugin_sv_cstr(
                            "[example_resources] runtime skill dir registered: skills_runtime/"
                        )
                    );
                }
            }

            // ---- 运行时注册 MCP server 示例 (注释状态; 声明式段已示范配置格式) ----
            // std::string spec = std::string("{\"namespace\":\"example_calc\",\"url\":\"")
            //     + "https://mcp.example.com/calc\",\"timeout\":30}";
            // g_if.resources->register_mcp_server(host, agentxx_plugin_sv(spec.data(),
            // spec.size()));

            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    // C ABI 边界异常守卫: 销毁回调异常不得外泄
    auto* ctx = static_cast<ResCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(
        [ctx](const char* m) noexcept {
            if (ctx) {
                ctx->logErr(m);
            }
        },
        [&] {
            if (!ctx || !ctx->host || !ctx->iface.plugins || !ctx->iface.resources
                || !ctx->iface.json) {
                delete ctx;
                return;
            }
            const AgentxxPluginHost* host  = ctx->host;
            const auto&              iface = ctx->iface;
            // 宿主 detachAll 已自动摘除本插件的全部资源 (skill/memory/mcp),
            // 此处显式反注册仅为 SDK 惯例示范 (幂等, 失败无副作用)
            AgentxxPluginString info{nullptr, 0};
            iface.plugins->get_own_info(host, &info);
            if (info.data) {
                AgentxxPluginString p{nullptr, 0};
                auto infoSv = agentxx_plugin_string_to_sv(&info);
                auto pathSv = agentxx_plugin_sv_cstr("path");
                iface.json->json_get_string(host, &infoSv, &pathSv, &p);
                if (p.data) {
                    std::string libPath(p.data, static_cast<size_t>(p.size));
                    agentxx_plugin_string_free(host, &p);
                    auto        pos  = libPath.find_last_of("/\\");
                    std::string base = pos == std::string::npos ? "." : libPath.substr(0, pos);
                    std::string d    = fmt::format("{}/skills_runtime", base);
                    iface.resources->unregister_skill_dir(
                        host,
                        agentxx_plugin_sv(d.data(), d.size())
                    );
                }
                agentxx_plugin_string_free(host, &info);
            }
            delete ctx;
        }
    );
}
