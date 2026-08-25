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
/// - 统一异步操作模型: 快同步工具经内联垫片注册 (宿主 io 线程直接执行,
///   零线程切换); 执行函数签名与旧同步模型一致
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

    AgentxxInlineToolSpec spec{};
    spec.name        = agentxx_plugin_sv(name, std::strlen(name));
    spec.description = agentxx_plugin_sv(
        g_storage[g_storage.size() - 2].data(),
        g_storage[g_storage.size() - 2].size()
    );
    spec.parameters_json = agentxx_plugin_sv(g_storage.back().data(), g_storage.back().size());
    spec.user_data       = nullptr;
    spec.flags           = flags;
    spec.execute         = execute;
    if (agentxx_register_inline_tool(g_host, &spec) != 0) {
        XX_LOGW("agentxx_system: register tool {} failed", name);
    }
}

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    // C ABI 边界异常守卫: 异常返回 NULL (宿主按"未导出"处理)
    return agentxx::plugin_guard::guardCall(
        pluginCatchLog,
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
        static const AgentxxPluginInfo info{
            AGENTXX_PLUGIN_API_VERSION,
            AGENTXX_SV("agentxx_system"),
            AGENTXX_SV("1.0.0"),
            AGENTXX_SV("System info tools: current date/time with Unix timestamp"),
        };
        return &info;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT int agentxx_plugin_entry(const AgentxxHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: entry 内含接口查询/注册等可抛操作, 异常返回 -1
    return agentxx::plugin_guard::guardCall(
        pluginCatchLog,
        -1,
        [&]() -> int {
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
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_unload(void* plugin_ctx) {
    // C ABI 边界异常守卫: 卸载回调异常不得外泄
    agentxx::plugin_guard::guardCallVoid(pluginCatchLog, [&] {
        (void)plugin_ctx;
        g_host = nullptr;
        g_if   = {};
    });
}
