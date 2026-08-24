// agentxx_system —— 系统信息工具插件
// - 从 libagentxx src/tools/system 拆分独立 (同名同行为):
//   - agentxx_get_current_datetime (原 GetCurrentDateTimeTool)
// - 业务逻辑在 system_impl.h (纯函数, 测试直测同一实现)
#include "agentxx_system_plugin.h"
#include "system_impl.h"
#include <cstring>
#include <string>
#include <vector>

using namespace agentxx_system_plugin;

namespace {

constexpr auto kNameDatetime = "agentxx_get_current_datetime";
constexpr auto kDepictDatetime = "Get the current date, time, and Unix timestamp.";

/// 注册无参工具 (schema/描述存储于插件侧静态区; spec 字符串字段以 string_view
/// 传入, 宿主注册时拷贝)
void registerTool(
    const char*        name,
    const char*        defaultDepict,
    const std::string& schema,
    char* (*execute)(
        void*                   user_data,
        AgentxxPluginStringView args_json,
        AgentxxPluginStringView thread_id,
        AgentxxPluginStringView tool_call_id,
        char**                  error_out
    ),
    int flags = 0
) {
    static std::vector<std::string> g_storage;
    std::string                     depict = readToolDepict(name);
    if (depict.empty()) {
        depict = defaultDepict;
    }
    g_storage.push_back(std::move(depict));
    g_storage.push_back(schema);

    AgentxxToolSpec spec{};
    spec.name        = agentxx_plugin_sv(name, std::strlen(name));
    spec.description = agentxx_plugin_sv(
        g_storage[g_storage.size() - 2].data(),
        g_storage[g_storage.size() - 2].size()
    );
    spec.parameters_json = agentxx_plugin_sv(g_storage.back().data(), g_storage.back().size());
    spec.user_data       = nullptr;
    spec.flags           = flags;
    spec.execute         = execute;
    if (!g_if.tools || !g_if.tools->register_tool
        || g_if.tools->register_tool(g_host, &spec) != 0) {
        XX_LOGW("agentxx_system: register tool {} failed", name);
    }
}

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        AGENTXX_SV("agentxx_system"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV("System info tools: current date/time with Unix timestamp"),
    };
    return &info;
}

extern "C" AGENTXX_PLUGIN_EXPORT int agentxx_plugin_entry(const AgentxxHost* host, void** plugin_ctx) {
    if (!host || !host->vtable || !plugin_ctx) {
        return -1;
    }
    g_host = host;
    g_if   = agentxx::plugin::AgentIfaces::query(host);
    *plugin_ctx = nullptr;

    // ---- agentxx_get_current_datetime ----
    // 无参数工具也声明空对象 schema: parameters 为 null 会被部分严格网关
    // (如 SCNet) 拒绝, 返回 400 "Format Error"
    registerTool(
        kNameDatetime,
        kDepictDatetime,
        R"({"type":"object","properties":{}})",
        [](void*                   user_data,
           AgentxxPluginStringView args_json,
           AgentxxPluginStringView thread_id,
           AgentxxPluginStringView tool_call_id,
           char**                  error_out) -> char* {
            (void)user_data;
            (void)args_json;
            (void)thread_id;
            (void)tool_call_id;
            try {
                auto result = agentxx::system_plugin::currentDatetimeExecute();
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
    );

    return 0;
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_unload(void* plugin_ctx) {
    (void)plugin_ctx;
    g_host = nullptr;
    g_if   = {};
}
