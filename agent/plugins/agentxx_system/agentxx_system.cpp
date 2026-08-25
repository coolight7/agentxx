// agentxx_system —— 系统信息工具插件
// - 从 libagentxx src/tools/system 拆分独立 (同名同行为):
//   - agentxx_get_current_datetime (原 GetCurrentDateTimeTool)
// - 业务逻辑在 system_impl.h (纯函数, 测试直测同一实现)
// - 多实例约定 (2026-08 API v1): 零可变全局, 状态存于 PluginCtx
#include "agentxx_system_plugin.h"
#include "system_impl.h"
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace agentxx_system_plugin;

namespace {

constexpr auto kNameDatetime = "agentxx_get_current_datetime";
constexpr auto kDepictDatetime = "Get the current date, time, and Unix timestamp.";

/// 注册无参工具 (schema/描述存储于本实例 ctx->storage; spec 字符串字段以
/// string_view 传入, 宿主注册时拷贝)
/// - 统一异步操作模型: 快同步工具经内联垫片注册 (宿主 io 线程直接执行,
///   零线程切换); 执行函数签名与旧同步模型一致
void registerTool(
    PluginCtx*         ctx,
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
    auto&       storage = ctx->storage;
    std::string depict  = readToolDepict(ctx->host, ctx->iface, name);
    if (depict.empty()) {
        depict = defaultDepict;
    }
    storage.push_back(std::move(depict));
    storage.push_back(schema);

    // 垫片适配器: 实例内嵌存储 (ctx 持有, 随实例销毁释放; 多实例契约)
    auto shim = std::make_unique<AgentxxInlineToolShim>();

    AgentxxInlineToolSpec spec{};
    spec.name        = agentxx_plugin_sv(name, std::strlen(name));
    spec.description = agentxx_plugin_sv(
        storage[storage.size() - 2].data(),
        storage[storage.size() - 2].size()
    );
    spec.parameters_json = agentxx_plugin_sv(storage.back().data(), storage.back().size());
    spec.user_data       = ctx; ///< 回调经 user_data 恢复本实例上下文
    spec.flags           = flags;
    spec.execute         = execute;
    // 注意: 本文件先包含了 system_impl.h (其引入 util/log.h 重定义 XX_LOG*
    // 宏为库版签名), 故此处直接调用 pluginLog, 不经 XX_LOGW 宏
    if (agentxx_register_inline_tool(ctx->host, &spec, shim.get()) != 0) {
        pluginLog(ctx, 3, fmt::format("agentxx_system: register tool {} failed", name));
        return;
    }
    ctx->inline_tool_shims.push_back(std::move(shim));
}

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    // C ABI 边界异常守卫: 异常返回 NULL (宿主按"未导出"处理);
    // 本边界为纯静态元数据, 无实例上下文可捕获 → 空操作日志闭包
    return agentxx::plugin_guard::guardCall(
        [](const char*) noexcept {},
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

extern "C" AGENTXX_PLUGIN_EXPORT int agentxx_plugin_create(const AgentxxHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: create 内含接口查询/注册等可抛操作, 异常返回 -1;
    // 守卫日志闭包捕获局部裸指针 (ctx 装配前置空 → 异常路径静默丢弃)
    PluginCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
        [&raw](const char* msg) noexcept { pluginLog(raw, 4, msg ? msg : ""); },
        -1,
        [&]() -> int {
        if (!host || !host->vtable || !plugin_ctx) {
            return -1;
        }
        auto ctx   = std::make_unique<PluginCtx>();
        ctx->host  = host;
        ctx->iface = agentxx::plugin::AgentIfaces::query(host);
        raw        = ctx.get();

        // ---- agentxx_get_current_datetime ----
        // 无参数工具也声明空对象 schema: parameters 为 null 会被部分严格网关
        // (如 SCNet) 拒绝, 返回 400 "Format Error"
        registerTool(
            ctx.get(),
            kNameDatetime,
            kDepictDatetime,
            R"({"type":"object","properties":{}})",
            [](void*                   user_data,
               AgentxxPluginStringView args_json,
               AgentxxPluginStringView thread_id,
               AgentxxPluginStringView tool_call_id,
               char**                  error_out) -> char* {
                auto* ctx = static_cast<PluginCtx*>(user_data);
                (void)args_json;
                (void)thread_id;
                (void)tool_call_id;
                try {
                    auto result = agentxx_system_plugin::currentDatetimeExecute();
                    return pluginStrdup(ctx ? ctx->host : nullptr, result.c_str());
                } catch (const std::exception& ex) {
                    if (error_out) {
                        *error_out = pluginStrdup(ctx ? ctx->host : nullptr, ex.what());
                    }
                    return nullptr;
                } catch (...) {
                    if (error_out) {
                        *error_out = pluginStrdup(ctx ? ctx->host : nullptr, "unknown exception");
                    }
                    return nullptr;
                }
            }
        );

        *plugin_ctx = ctx.release(); ///< 所有权移交宿主 (destroy 时取回归还)
        return 0;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_destroy(void* plugin_ctx) {
    // C ABI 边界异常守卫: 销毁回调异常不得外泄
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    agentxx::plugin_guard::guardCallVoid(
        [ctx](const char* msg) noexcept { pluginLog(ctx, 4, msg ? msg : ""); },
        [&] { delete ctx; }); // 垫片适配器为 ctx 内嵌存储, 随 ctx 一并释放
}
