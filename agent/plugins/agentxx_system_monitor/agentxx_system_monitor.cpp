// agentxx_system_monitor —— 系统资源监控插件 (CPU/内存/GPU)
// - 从 libagentxx src/expand/get_cpu_gpu_use 拆分独立:
//   原内置工具 agentxx_get_system_core_info 迁入本插件注册 (同名, 行为一致)
// - 另注册能力 "agentxx.system_usage" (方法 query): 返回使用率 JSON (schema
//   由本插件定义), 供宿主 SessionServerAgentIO 经能力调用服务 TUI 的系统资源
//   侧边栏展示 (WireGetSystemUsage/WireSystemUsage 链路; 宿主只透传 JSON,
//   不解析语义)
// - 插件不链接 libagentxx: 日志经 vtable log, JSON 组装用 fmt + json_escape
#include "system_monitor_plugin.h"
#include "cpu_gpu_monitor.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "fmt/format.h"
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

using namespace agentxx_system_monitor_plugin;

// =====================================================================
// 同步查询封装
// - CpuGpuMonitor::query() 为 asio 协程 (内部含 100ms 采样定时器与文件读取),
//   依赖 io executor; 插件 execute/能力回调为同步 C 回调 (运行在宿主线程池),
//   用局部 io_context 驱动协程至完成 (阻塞调用方线程, 宿主已卸载到线程池)
// =====================================================================

static agentxx::expand::CpuGpuUsage querySync() {
    asio::io_context               io;
    agentxx::expand::CpuGpuUsage   usage;
    asio::co_spawn(
        io,
        [&usage]() -> asio::awaitable<void> {
            agentxx::expand::CpuGpuMonitor monitor;
            usage = co_await monitor.query();
        },
        asio::detached
    );
    io.run();
    return usage;
}

// =====================================================================
// JSON 组装 (能力返回的使用率 JSON schema, 由本插件定义:
// {"cpu","mem_total_mb","mem_used_mb","mem_percent","gpus":[...]})
// - 宿主经 WireSystemUsage 原样透传 (lib wire 层不含系统资源 DTO);
//   TUI 侧按本 schema 渲染; 未来 client 插件化渲染 UI 时消费同一 JSON
// =====================================================================

/// 转义字符串为 JSON 字面量 (经宿主 vtable json_escape)
static std::string jsonEscape(const std::string& s) {
    if (!g_host || !g_host->vtable || !g_host->vtable->json_escape || s.empty()) {
        return "\"\"";
    }
    char* esc = g_host->vtable->json_escape(g_host, agentxx_plugin_sv(s.data(), s.size()));
    if (!esc) {
        return "\"\"";
    }
    std::string out{esc};
    g_host->vtable->free(esc);
    return out;
}

static std::string usageToJson(const agentxx::expand::CpuGpuUsage& u) {
    std::string gpus = "[";
    for (size_t i = 0; i < u.gpus.size(); ++i) {
        const auto& g = u.gpus[i];
        if (i > 0) {
            gpus += ",";
        }
        gpus += fmt::format(
            R"({{"name":{},"dedicated_vram_mb":{},"dedicated_vram_used_mb":{},"shared_vram_mb":{},"shared_vram_used_mb":{},"usage_percent":{:.2f}}})",
            jsonEscape(g.name),
            g.dedicatedVramMB,
            g.dedicatedVramUsedMB,
            g.sharedVramMB,
            g.sharedVramUsedMB,
            g.usagePercent
        );
    }
    gpus += "]";
    return fmt::format(
        R"({{"cpu":{:.2f},"mem_total_mb":{},"mem_used_mb":{},"mem_percent":{:.2f},"gpus":{}}})",
        u.cpuUsagePercent,
        u.memory.totalPhysicalMB,
        u.memory.usedPhysicalMB,
        u.memory.usagePercent,
        gpus
    );
}

// =====================================================================
// 工具注册辅助 (与 agentxx_screen_capture 插件同模式)
// =====================================================================

/// 读取宿主 toolPrompt 的 depict; 未配置返回空
static std::string readToolDepict(const std::string& toolName) {
    if (!g_host || !g_host->vtable || !g_host->vtable->get_tool_prompt) {
        return {};
    }
    char* json = g_host->vtable->get_tool_prompt(
        g_host,
        agentxx_plugin_sv(toolName.data(), toolName.size())
    );
    if (!json) {
        return {};
    }
    std::string s{json};
    g_host->vtable->free(json);
    SimpleJson j(s);
    if (!j.ok()) {
        return {};
    }
    std::string depict;
    jsonGetString(j.doc().at_pointer("/depict"), depict);
    return depict;
}

/// 注册无参工具 (schema/描述存储于插件侧静态区; spec 字符串字段以 string_view
/// 传入, 宿主注册时拷贝); execute 由调用方提供 (静态 lambda, 无捕获)
static void registerTool(
    const char* name,
    const char* defaultDepict,
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
    std::string depict = readToolDepict(name);
    if (depict.empty()) {
        depict = defaultDepict;
    }
    g_storage.push_back(std::move(depict));
    g_storage.push_back(schema);

    AgentxxToolSpec spec{};
    spec.name = agentxx_plugin_sv(name, std::strlen(name));
    spec.description
        = agentxx_plugin_sv(g_storage[g_storage.size() - 2].data(), g_storage[g_storage.size() - 2].size());
    spec.parameters_json = agentxx_plugin_sv(g_storage.back().data(), g_storage.back().size());
    spec.user_data       = nullptr;
    spec.flags           = flags;
    spec.execute         = execute;
    if (g_host->vtable->register_tool(g_host, &spec) != 0) {
        pluginLog(3, fmt::format("agentxx_system_monitor: register tool {} failed", name));
    }
}

/// 工具 agentxx_get_system_core_info 的执行回调 (与旧内置工具输出格式一致)
static char* getSystemCoreInfoExecute(
    void*                   user_data,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView thread_id,
    AgentxxPluginStringView tool_call_id,
    char**                  error_out
) {
    (void)user_data;
    (void)args_json;
    (void)thread_id;
    (void)tool_call_id;
    try {
        auto usage = querySync();
        std::stringstream ss;
        ss << fmt::format("CPU Usage: {:.1f}%\n", usage.cpuUsagePercent);
        ss << fmt::format(
            "Memory: {:.1f}% (Used: {}MB / Total: {}MB)\n",
            usage.memory.usagePercent,
            usage.memory.usedPhysicalMB,
            usage.memory.totalPhysicalMB
        );
        for (size_t i = 0; i < usage.gpus.size(); ++i) {
            const auto& gpu = usage.gpus[i];
            if (!gpu.name.empty()) {
                ss << fmt::format(
                    "GPU {} [{}]: GPU Usage: {:.1f}%, "
                    "VRAM: {}MB Used / {}MB Total",
                    i,
                    gpu.name,
                    gpu.usagePercent,
                    gpu.dedicatedVramUsedMB,
                    gpu.dedicatedVramMB
                );
            } else {
                ss << fmt::format(
                    "GPU {}: GPU Usage: {:.1f}%, "
                    "VRAM: {}MB Used / {}MB Total",
                    i,
                    gpu.usagePercent,
                    gpu.dedicatedVramUsedMB,
                    gpu.dedicatedVramMB
                );
            }
            if (gpu.sharedVramMB > 0) {
                ss << fmt::format(
                    " (Shared: {}MB Used / {}MB Total)",
                    gpu.sharedVramUsedMB,
                    gpu.sharedVramMB
                );
            }
            ss << "\n";
        }
        if (usage.gpus.empty()) {
            ss << "GPU: No GPU detected\n";
        }
        return pluginStrdup(ss.str().c_str());
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

// =====================================================================
// 能力: agentxx.system_usage (方法 query)
// - 宿主 SessionServerAgentIO 处理 WireGetSystemUsage 时经
//   PluginManager::invokeCapability 调用 (回调在调用方线程执行, 宿主已
//   卸载到 blockingPool; 本回调同步阻塞 ~100ms 采样, 可接受)
// - 返回 JSON schema 与 lib wire_protocol.h 一致
// =====================================================================

static char* systemUsageInvoke(
    void*                   ctx,
    const AgentxxHost*      caller_host,
    AgentxxPluginStringView method,
    AgentxxPluginStringView args_json,
    char**                  error_out
) {
    (void)ctx;
    (void)caller_host;
    (void)method;
    (void)args_json;
    (void)error_out;
    try {
        auto usage = querySync();
        auto json  = usageToJson(usage);
        return pluginStrdup(json.c_str());
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

// =====================================================================
// 插件入口 (C ABI)
// =====================================================================

extern "C" const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        AGENTXX_SV("agentxx_system_monitor"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV("System resource monitor: CPU/memory/GPU usage tool and "
                   "agentxx.system_usage capability"),
    };
    return &info;
}

extern "C" int agentxx_plugin_entry(const AgentxxHost* host, void** /*plugin_ctx*/) {
    g_host = host;

    // 1. 工具 agentxx_get_system_core_info (与原内置工具同名同行为)
    static const std::string kSchema
        = R"({"type":"object","properties":{},"additionalProperties":false})";
    registerTool(
        "agentxx_get_system_core_info",
        "Get system resource usage: CPU usage, memory usage, and GPU usage (name, VRAM, "
        "utilization) on the host machine where the agent server runs.",
        kSchema,
        getSystemCoreInfoExecute
    );

    // 2. 能力 agentxx.system_usage (方法 query)
    if (host->vtable->register_capability_ex(
            host,
            AGENTXX_SV("agentxx.system_usage"),
            systemUsageInvoke,
            nullptr
        ) != 0) {
        pluginLog(3, "agentxx_system_monitor: register capability agentxx.system_usage failed");
    }

    pluginLog(2, "agentxx_system_monitor loaded (1 tool, 1 capability)");
    return 0;
}

extern "C" void agentxx_plugin_unload(void* /*plugin_ctx*/) {
    if (g_host && g_host->vtable) {
        g_host->vtable->unregister_tool(g_host, AGENTXX_SV("agentxx_get_system_core_info"));
        g_host->vtable->unregister_capability(g_host, AGENTXX_SV("agentxx.system_usage"));
    }
    pluginLog(2, "agentxx_system_monitor unloaded");
}
