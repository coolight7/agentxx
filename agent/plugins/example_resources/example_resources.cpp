/*
 * example_resources —— 会话资源示例插件 (plugin_api v8)
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
 * 2) 运行时: entry 内经 vtable register_skill_dir / register_memory_file /
 *    register_mcp_server 实时注册。本插件注册:
 *      - skill: skills_runtime/    (含 extra_skill/SKILL.md)
 *    MCP 运行时注册格式 (默认注释, 避免真实网络请求):
 *      spec_json = {"namespace":"...","url":"https://...","timeout":60}
 *
 * 所有权语义: 经本插件注册/声明的资源在卸载时由宿主自动摘除,
 * 禁用时摘除、启用时恢复 (与工具行为一致), 插件无需手动清理;
 * unload 回调中的显式反注册仅为 SDK 惯例示范。
 */
#include "agentxx/plugin/plugin_api.h"

#include <string>

static const AgentxxHost* g_host = nullptr;

/// 从 get_own_info JSON 中提取字段值 (host->alloc, 用完 free)
static std::string ownInfoString(const AgentxxHost* host, const char* key) {
    auto& vt = *host->vtable;
    char* info = vt.get_own_info(host);
    if (!info) {
        return {};
    }
    std::string out;
    char* val = vt.json_get_string(
        host,
        agentxx_plugin_sv_cstr(info),
        agentxx_plugin_sv_cstr(key)
    );
    if (val) {
        out = val;
        vt.free(val);
    }
    vt.free(info);
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

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        AGENTXX_SV("example_resources"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV("Example plugin contributing skill/memory/mcp resources "
                   "(declarative manifest + runtime vtable API)"),
    };
    return &info;
}

extern "C" AGENTXX_PLUGIN_EXPORT int agentxx_plugin_entry(const AgentxxHost* host, void** plugin_ctx) {
    (void)plugin_ctx;
    g_host       = host;
    auto& vt     = *host->vtable;
    auto  base   = dirOf(ownInfoString(host, "path"));

    // ---- 运行时注册: 追加 skill 目录 (声明式段见 plugin.yaml) ----
    // - 与 yaml 主配置或其他插件冲突时返回非 0 (yaml 优先, 此处仅告警不失败)
    std::string runtimeSkillDir = base + "/skills_runtime";
    if (vt.register_skill_dir(host, agentxx_plugin_sv(runtimeSkillDir.data(), runtimeSkillDir.size()))
        != 0) {
        vt.log(host,
               3,
               AGENTXX_SV("[example_resources] register runtime skill dir failed"));
    } else {
        vt.log(host,
               2,
               AGENTXX_SV("[example_resources] runtime skill dir registered: skills_runtime/"));
    }

    // ---- 运行时注册 MCP server 示例 (注释状态; 声明式段已示范配置格式) ----
    // std::string spec = std::string("{\"namespace\":\"example_calc\",\"url\":\"")
    //     + "https://mcp.example.com/calc\",\"timeout\":30}";
    // vt.register_mcp_server(host, agentxx_plugin_sv(spec.data(), spec.size()));

    return 0;
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_unload(void* plugin_ctx) {
    (void)plugin_ctx;
    // 宿主 detachAll 已自动摘除本插件的全部资源 (skill/memory/mcp),
    // 此处显式反注册仅为 SDK 惯例示范 (幂等, 失败无副作用)
    if (!g_host) {
        return;
    }
    auto& vt = *g_host->vtable;
    char* info = vt.get_own_info(g_host);
    if (info) {
        char* p = vt.json_get_string(
            g_host,
            agentxx_plugin_sv_cstr(info),
            agentxx_plugin_sv_cstr("path")
        );
        if (p) {
            std::string libPath = p;
            vt.free(p);
            auto        pos = libPath.find_last_of("/\\");
            std::string base = pos == std::string::npos ? "." : libPath.substr(0, pos);
            std::string d    = base + "/skills_runtime";
            vt.unregister_skill_dir(g_host, agentxx_plugin_sv(d.data(), d.size()));
        }
        vt.free(info);
    }
    g_host = nullptr;
}
