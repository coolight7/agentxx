// agentxx_system_monitor —— 系统资源监控插件 (CPU/内存/GPU)
// - 从 libagentxx src/expand/get_cpu_gpu_use 拆分独立:
//   原内置工具 agentxx_get_system_core_info 迁入本插件注册 (同名, 行为一致)
// - 另注册能力 "agentxx.system_usage" (方法 query): 返回使用率 JSON (schema
//   由本插件定义), 供测试与工具执行使用; 常规展示不再走请求-响应链路
//   (WireGetSystemUsage/WireSystemUsage 已随插件化移除, lib wire 层不含
//   系统资源 DTO)
// - 周期采集: 宿主定时器 (v7 add_timer) 每 500ms tick (io 线程快速返回),
//   到 kUsageIntervalSec 后经 host->offload 把阻塞采样 (CpuGpuMonitor::query,
//   ~100ms) 卸载到宿主阻塞池, done 回 io 线程 publish
//   "agentxx_system_monitor.usage" (server 原样转发 WirePluginData) ——
//   定时/采集/发布完全位于插件内且不自建线程 (线程数量可控, 卸载安全由
//   宿主统一保证), TUI 不再参与
// - client 侧入口 (agentxx_client_entry): 订阅宿主转发的 usage 插件事件,
//   以侧边栏 Info 栏段落渲染明细 (CPU/RAM/GPU); 显示开关命令 /sysinfo
//   经跨端事件 (usage_enabled) 上行同步到 agent 侧, 关闭期间跳过采集
// - 插件不链接 libagentxx: 日志经 vtable log, JSON 组装用 fmt + json_escape
// - RAM/VRAM 等容量展示复用 agentxx_util 的 formatSize (字节入参, 自动选单位)
#include "agentxx/plugin/client_plugin_api.h"
#include "agentxx/plugin/plugin_iface_helper.h"
#include "agentxx/util/string_util.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "cpu_gpu_monitor.h"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include "system_monitor_plugin.h"
#include <atomic>
#include <chrono>
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
    asio::io_context             io;
    agentxx::expand::CpuGpuUsage usage;
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
// - 经 publish "agentxx_system_monitor.usage" 事件原样透传 (lib wire 层不含
//   系统资源 DTO); client 侧入口按本 schema 渲染 Info 段落
// =====================================================================

/// 转义字符串为 JSON 字面量 (经宿主 vtable json_escape)
static std::string jsonEscape(const std::string& s) {
    if (!g_host || !g_if.json || !g_if.json->json_escape || s.empty()) {
        return "\"\"";
    }
    char* esc = g_if.json->json_escape(g_host, agentxx_plugin_sv(s.data(), s.size()));
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
    if (!g_host || !g_if.config || !g_if.config->get_tool_prompt) {
        return {};
    }
    char* json = g_if.config->get_tool_prompt(
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
        pluginLog(3, fmt::format("agentxx_system_monitor: register tool {} failed", name));
    }
}

/// 把插件默认提示词写入宿主 toolPrompt (仅当宿主无该条目时; io 线程)
/// - 用户 yaml 覆盖早于插件加载 → get_prompt 已含覆盖 → 跳过 (尊重用户配置)
/// - 宿主未提供 get_prompt/set_prompt (旧宿主) → 跳过, registerTool 回退插件默认
static void ensureToolPromptInHost() {
    if (!g_host || !g_if.prompt || !g_if.prompt->get_prompt || !g_if.prompt->set_prompt) {
        return;
    }
    char* json = g_if.prompt->get_prompt(g_host);
    if (!json) {
        return;
    }
    std::string s{json};
    g_host->vtable->free(json);
    SimpleJson j(s);
    if (!j.ok()) {
        return;
    }
    // 宿主已有条目 (用户 yaml 覆盖 / 之前已写入): 尊重, 不覆盖
    if (!j.doc().at_pointer("/toolPrompt/agentxx_get_system_core_info").error()) {
        return;
    }
    // 工具无参数, 描述文本不含引号/反斜杠 → 直接拼 JSON 安全
    // (与 registerTool 的默认描述一致, 从 lib AgentPrompt 剥离迁移)
    std::string payload
        = R"({"toolPrompt":{"agentxx_get_system_core_info":{"depict":")"
          + std::
              string{"Get system resource usage: CPU utilization, memory usage, GPU utilization, and "
                     "GPU memory usage."}
          + R"(","args":{}}}})";
    if (g_if.prompt->set_prompt(g_host, agentxx_plugin_sv(payload.data(), payload.size()))
        != 0) {
        pluginLog(3, "agentxx_system_monitor: set_prompt failed");
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
        auto              usage = querySync();
        std::stringstream ss;
        ss << fmt::format("CPU Usage: {:.1f}%\n", usage.cpuUsagePercent);
        // formatSize 入参为字节: memory.*MB 为 MB 值, 需先乘 1024*1024 转字节
        // (直接传 MB 会得到错误的 "K" 级单位, 如 8192MB 显示 "8K")
        const auto mbToBytes = [](uint64_t mb) {
            return mb * 1024 * 1024;
        };
        ss << fmt::format(
            "Memory: {:.1f}% (Used: {} / Total: {})\n",
            usage.memory.usagePercent,
            agentxx::util::formatSize(mbToBytes(usage.memory.usedPhysicalMB)),
            agentxx::util::formatSize(mbToBytes(usage.memory.totalPhysicalMB))
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
// - 数据源: 周期采集线程与工具 agentxx_get_system_core_info 均基于
//   CpuGpuMonitor::query 采样; 本能力供测试/其他插件按需查询
//   (回调在调用方线程执行, 同步阻塞 ~100ms 采样, 可接受)
// - 返回 JSON schema 由本插件定义 (usageToJson)
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

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        AGENTXX_SV("agentxx_system_monitor"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV("System resource monitor: CPU/memory/GPU usage tool and "
                   "agentxx.system_usage capability"),
    };
    return &info;
}

// =====================================================================
// 周期采集 (agent 侧; 定时/采集/发布完全位于插件内)
// - 每 kUsageIntervalSec 秒执行一次 querySync() (阻塞 ~100ms 采样) 并
//   publish "agentxx_system_monitor.usage"; server 的 subscribePluginEvents
//   原样转发为 WirePluginData, client 侧插件订阅后渲染 Info 段落
// - publish 线程安全: 宿主内部 co_spawn 到总线 executor 异步投递
//   (不等待 io 线程, 采集线程可安全调用; 与 codegraph 工作线程 publish 同模式)
// - 显示开关由 client 侧 /sysinfo 命令经跨端事件 (usage_enabled) 同步,
//   关闭期间跳过采集 (仍保持周期唤醒, 便于随时重新开启)
// - 退出: unload 回调置 stop 并 join (tick 500ms 轮询, join 最坏 ~600ms)
// =====================================================================

/// 周期采集间隔 (秒)
static constexpr int kUsageIntervalSec = 5;
/// 轮询 tick (毫秒): 短周期便于退出时快速 join
static constexpr int kUsageTickMs = 500;

struct PluginCtx {
    /// 采集/发布开关 (client /sysinfo 经 usage_enabled 事件同步)
    std::atomic<bool> usageEnabled{true};
    /// 宿主周期定时器句柄 (v7 add_timer; unload 时 cancel_timer)
    void* timer = nullptr;
    /// 采集进行中标记 (io 线程独占: 定时器回调/offload done 均回 io 线程;
    /// 防采集耗时 > tick 间隔导致重入)
    bool collecting = false;
    /// tick 计数 (io 线程独占; 每 kUsageIntervalSec 秒采一次)
    int tick = 0;
};

/// 周期采集 work: 宿主阻塞池线程执行 (阻塞 ~100ms 采样)
static void* usageCollectWork(void* ud, char** error_out) {
    auto* ctx = static_cast<PluginCtx*>(ud);
    if (!ctx || !g_host) {
        return nullptr;
    }
    try {
        auto usage = querySync();
        auto json  = usageToJson(usage);
        // 结果经宿主分配 (跨 CRT 堆边界)
        char* p = static_cast<char*>(g_host->vtable->alloc(json.size() + 1));
        if (p) {
            std::memcpy(p, json.c_str(), json.size() + 1);
        }
        return p;
    } catch (const std::exception& e) {
        if (error_out) {
            *error_out = g_host->vtable->strdup(e.what());
        }
        return nullptr;
    } catch (...) {
        if (error_out) {
            *error_out = g_host->vtable->strdup("unknown error");
        }
        return nullptr;
    }
}

/// 周期采集 done: io 线程执行 (快速返回; publish 异步投递)
static void usageCollectDone(void* ud, void* result, char* error) {
    auto* ctx = static_cast<PluginCtx*>(ud);
    if (ctx) {
        ctx->collecting = false;
    }
    if (error) {
        pluginLog(3, fmt::format("agentxx_system_monitor: usage collect failed: {}", error));
        if (g_host && g_host->vtable) {
            g_host->vtable->free(error);
        }
        return;
    }
    if (result) {
        if (g_host && g_if.events && g_if.events->publish) {
            const char* s = static_cast<const char*>(result);
            g_if.events->publish(
                g_host,
                AGENTXX_SV("agentxx_system_monitor.usage"),
                agentxx_plugin_sv(s, std::strlen(s))
            );
        }
        g_host->vtable->free(result);
    }
}

/// 周期采集 tick (宿主定时器回调, io 线程; 必须快速返回):
/// - 计数到 kUsageIntervalSec 后经 host->offload 把阻塞采样卸载到宿主阻塞池
///   (work 在阻塞池, done 回 io 线程 publish) —— 不再自建采集线程
static void onUsageTick(void* ud) {
    auto* ctx = static_cast<PluginCtx*>(ud);
    if (!ctx || !g_host || !g_if.scheduler || !g_if.scheduler->offload) {
        return;
    }
    if (ctx->collecting) {
        return; // 上次采集未完成 (阻塞池忙), 跳过本 tick
    }
    if (++ctx->tick < kUsageIntervalSec * 1000 / kUsageTickMs) {
        return;
    }
    ctx->tick = 0;
    if (!ctx->usageEnabled.load(std::memory_order_relaxed)) {
        return; // 显示关闭: 跳过采集
    }
    ctx->collecting = true;
    g_if.scheduler->offload(g_host, usageCollectWork, usageCollectDone, ctx);
}

/// 跨端事件: client /sysinfo 开关同步 (WirePluginDataUp 上行后 server 发布到
/// plugin.client.agentxx_system_monitor.usage_enabled; 订阅回调在 agent io 线程,
/// 仅写原子标志, 快速返回)
static void on_usage_enabled(AgentxxPluginStringView event_json, void* ud) {
    auto* ctx = static_cast<PluginCtx*>(ud);
    if (!ctx) {
        return;
    }
    std::string json{event_json.data ? event_json.data : "", event_json.size};
    SimpleJson  j(json);
    bool        enabled = true;
    if (j.ok() && jsonGetBool(j.doc().at_pointer("/enabled"), enabled)) {
        ctx->usageEnabled.store(enabled, std::memory_order_relaxed);
    }
}

/// 宿主约定事件 client_attached 响应: 把 tick 计数置为"下一个 tick 即采集",
/// 晚接入/重连的客户端 ≤500ms 内即可收到第一份数据, 无需等满 5s 周期
/// (开关关闭时不影响 —— tick 到期后仍按 usageEnabled 跳过采集)
static void on_client_attached(AgentxxPluginStringView event_json, void* ud) {
    (void)event_json;
    auto* ctx = static_cast<PluginCtx*>(ud);
    if (ctx) {
        ctx->tick = kUsageIntervalSec * 1000 / kUsageTickMs - 1;
    }
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_entry(const AgentxxHost* host, void** plugin_ctx) {
    g_host = host;
    // COM 风格接口表查询 (entry 一次性查询缓存; 进程级静态数据, 长期有效)
    static const agentxx::plugin::AgentIfaces s_if = agentxx::plugin::AgentIfaces::query(host);
    g_if = s_if;

    // 默认提示词写入宿主 (从 lib AgentPrompt 剥离迁移; 用户 yaml 覆盖优先)
    ensureToolPromptInHost();

    // 1. 工具 agentxx_get_system_core_info (与原内置工具同名同行为)
    static const std::string kSchema
        = R"({"type":"object","properties":{},"additionalProperties":false})";
    registerTool(
        "agentxx_get_system_core_info",
        // 默认描述 (从 lib AgentPrompt 剥离迁移, 2026-08)
        "Get system resource usage: CPU utilization, memory usage, GPU utilization, and GPU "
        "memory usage.",
        kSchema,
        getSystemCoreInfoExecute
    );

    // 2. 能力 agentxx.system_usage (方法 query)
    if (!g_if.capabilities || !g_if.capabilities->register_capability_ex
        || g_if.capabilities->register_capability_ex(
            host,
            AGENTXX_SV("agentxx.system_usage"),
            systemUsageInvoke,
            nullptr
        )
            != 0)
    {
        pluginLog(3, "agentxx_system_monitor: register capability agentxx.system_usage failed");
    }

    // 3. 周期采集: 订阅 client /sysinfo 开关同步事件 + 宿主定时器
    //    (v7 add_timer): 每 kUsageTickMs 触发一次 tick (io 线程快速返回),
    //    到 kUsageIntervalSec 后经 host->offload 把阻塞采样卸载到宿主阻塞池
    //    (work 阻塞池 / done io 线程 publish) —— 不占 io 线程、不自建线程,
    //    卸载安全由宿主统一保证 (定时器取消 + inflight 保活)
    auto ctx = std::make_unique<PluginCtx>();
    if (!g_if.events || !g_if.events->subscribe
        || !g_if.events->subscribe(
            host,
            AGENTXX_SV("client.agentxx_system_monitor.usage_enabled"),
            on_usage_enabled,
            ctx.get()
        )) {
        pluginLog(3, "agentxx_system_monitor: subscribe usage_enabled failed");
    }
    // 订阅宿主约定事件 client_attached: 客户端接入/重连后立即采集一次
    // (晚接入客户端 ≤500ms 收到首份数据, 无需等满 5s 周期)
    if (!g_if.events->subscribe(
            host,
            AGENTXX_SV("agentxx_host.client_attached"),
            on_client_attached,
            ctx.get()
        )) {
        pluginLog(3, "agentxx_system_monitor: subscribe client_attached failed");
    }
    if (g_if.scheduler && g_if.scheduler->add_timer) {
        ctx->timer = g_if.scheduler->add_timer(host, kUsageTickMs, onUsageTick, ctx.get());
        if (!ctx->timer) {
            pluginLog(3, "agentxx_system_monitor: add_timer failed (collector disabled)");
        }
    } else {
        pluginLog(3, "agentxx_system_monitor: host has no add_timer (collector disabled)");
    }

    *plugin_ctx = ctx.release();
    pluginLog(2, "agentxx_system_monitor loaded (1 tool, 1 capability, periodic collector)");
    return 0;
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_unload(void* plugin_ctx) {
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    if (ctx) {
        // 取消宿主定时器 (在途 tick/offload 由宿主 inflight 计数等待完成,
        // 此处无任何在途引用后安全释放 ctx)
        if (g_host && g_if.scheduler && g_if.scheduler->cancel_timer && ctx->timer) {
            g_if.scheduler->cancel_timer(g_host, ctx->timer);
            ctx->timer = nullptr;
        }
        delete ctx;
    }
    if (g_host && g_if.tools && g_if.tools->unregister_tool) {
        g_if.tools->unregister_tool(g_host, AGENTXX_SV("agentxx_get_system_core_info"));
    }
    if (g_host && g_if.capabilities && g_if.capabilities->unregister_capability) {
        g_if.capabilities->unregister_capability(g_host, AGENTXX_SV("agentxx.system_usage"));
    }
    pluginLog(2, "agentxx_system_monitor unloaded");
}

/* =====================================================================
 * client 侧入口 (agentxx_client_entry) —— 系统资源占用渲染
 *
 * 数据链路 (定时/采集/发布完全位于 agent 侧插件内, 见文件头部说明):
 * - agent 侧插件周期线程每 5s 采集并 publish "agentxx_system_monitor.usage"
 * - server 原样转发为 WirePluginData, 宿主 (TUI) 经事件接收器分发到
 *   client 插件系统 (AGENTXX_CLIENT_EVT_PLUGIN_DATA, 见 TUI onPeerMessage)
 * - 本入口订阅该事件, 过滤 usage 事件后解析 JSON (schema 由本插件定义),
 *   以侧边栏 Info 栏段落渲染明细 (CPU/RAM/GPU 汇总)
 * - 显示开关: 命令 /sysinfo 切换 (插件内部状态; Info 段落渲染与命令
 *   执行均在 client io 线程, 无跨线程竞争); 开关状态经跨端事件
 *   (usage_enabled) 上行同步, agent 侧据此跳过采集
 * - CLI 宿主 (无 INFO_SECTION 能力) 下 register_info_section 返回 NULL,
 *   插件降级 (仅保持数据接收, 不渲染)
 * ===================================================================== */

static const AgentxxClientHost* g_client_host = nullptr;
/// client 侧接口表缓存 (entry 时 ClientIfaces::query 一次查询)
static const agentxx::plugin::ClientIfaces* g_client_if = nullptr;
/// "agentxx.client.ui" 展示接口表 (Info 段落/命令; 不支持子能力成员为 NULL, 调用前判空)
static const AgentxxClientUiIface* g_client_ui = nullptr;
static AgentxxInfoSection*      g_section     = nullptr;
/// 系统资源显示开关 (命令 /sysinfo 切换; 默认开启, 与旧 TUI 设置一致)
static std::atomic<bool> g_usage_enabled{true};
/// 最近一次收到的 usage JSON (原始字符串; 用于开关重新开启时立即刷新)
static std::string g_last_usage_json;

/// 字符串 → JSON 字符串字面量 (经宿主 agentxx.client.json 接口表; 结果含引号;
/// 供 fmt::format 组装 JSON 时嵌入字段值, 避免手工拼接)
static std::string clientJsonEscape(const std::string& s) {
    if (!g_client_host || !g_client_if || !g_client_if->json || s.empty()) {
        return "\"\"";
    }
    char* esc
        = g_client_if->json->json_escape(g_client_host, agentxx_plugin_sv(s.data(), s.size()));
    if (!esc) {
        return "\"\"";
    }
    std::string out{esc};
    g_client_host->vtable->free(esc);
    return out;
}

/// 从 usage JSON 读取双精度字段 (schema 由本插件定义:
/// {"cpu","mem_total_mb","mem_used_mb","mem_percent","gpus":[...]};
/// cpu/mem_percent 恒为 {:.2f} 浮点文本)
static double jsonGetDouble(SimpleJson& j, const char* pointer) {
    auto v = j.doc().at_pointer(pointer);
    if (v.error()) {
        return 0.0;
    }
    double d = 0.0;
    if (!v.value().get_double().get(d)) {
        return d;
    }
    return 0.0;
}

/// 从 usage JSON 读取整数字段 (mem_* 单位为 MB 的整数)
static int64_t jsonGetInt64(SimpleJson& j, const char* pointer) {
    auto v = j.doc().at_pointer(pointer);
    if (v.error()) {
        return 0;
    }
    uint64_t u = 0;
    if (!v.value().get_uint64().get(u)) {
        return static_cast<int64_t>(u);
    }
    int64_t i = 0;
    if (!v.value().get_int64().get(i)) {
        return i;
    }
    return 0;
}

/// usage JSON 解析汇总 (Info 段落数据源)
struct UsageStat {
    double  cpu        = 0.0; ///< CPU 占用 %
    double  memPct     = 0.0; ///< 内存占用 %
    int64_t memUsedMb  = 0;   ///< 已用内存 MB
    int64_t memTotalMb = 0;   ///< 总内存 MB
    size_t  gpuCount   = 0;   ///< 检测到的 GPU 数量
    double  gpuPeakPct = 0.0; ///< GPU 最高占用 %
};

static UsageStat parseUsage(const std::string& raw) {
    UsageStat  st;
    SimpleJson j(raw);
    if (!j.ok()) {
        return st;
    }
    st.cpu        = jsonGetDouble(j, "/cpu");
    st.memPct     = jsonGetDouble(j, "/mem_percent");
    st.memUsedMb  = jsonGetInt64(j, "/mem_used_mb");
    st.memTotalMb = jsonGetInt64(j, "/mem_total_mb");
    // gpus 汇总: 数量 + 峰值占用 (逐元素单次遍历, 避免嵌套二次访问)
    auto gpus = j.doc().at_pointer("/gpus");
    if (!gpus.error()) {
        simdjson::ondemand::array arr;
        if (!gpus.value().get_array().get(arr)) {
            for (auto elem : arr) {
                if (elem.error()) {
                    continue;
                }
                ++st.gpuCount;
                double u   = 0.0;
                auto   obj = elem.get_object();
                if (!obj.error()) {
                    auto usageField = obj["usage_percent"];
                    if (!usageField.error()) {
                        (void)usageField.get_double().get(u);
                    }
                }
                if (u > st.gpuPeakPct) {
                    st.gpuPeakPct = u;
                }
            }
        }
    }
    return st;
}

/// 组装 Info 栏段落 items JSON (明细: CPU/RAM/GPU; kind: text, role 指定
/// 样式: title=高亮 / normal=普通(默认) / hint=减淡; 列表项由宿主按
/// Append 段样式 "|  xxx" 展示, 插件不拼接前缀)
/// - 各条目用 fmt::format 构造, 最后 fmt::join 组装 (避免手工字符串拼接)
static std::string buildUsageInfoItemsJson(const UsageStat& st) {
    std::vector<std::string> items;
    auto textItem = [&](const std::string& text, const std::string& role = "normal") {
        items.push_back(fmt::format(
            R"({{"kind":"text","role":{},"text":{}}})",
            clientJsonEscape(role),
            clientJsonEscape(text)
        ));
    };

    textItem(fmt::format("|- CPU {:.0f}%", st.cpu));
    // RAM: 45% (8G/18G) —— agentxx::util::formatSize 按字节入参自动选单位
    // (K/M/G/T); mem_* 为 MB 值需先乘 1024*1024 转字节, showFloat=false
    // 取整展示 (Info 栏空间有限, 整数单位足够; 与状态栏上下文显示一致)
    std::string ram = fmt::format("|- RAM {:.0f}%", st.memPct);
    if (st.memTotalMb > 0) {
        const auto mbToBytes = [](int64_t mb) {
            return static_cast<uint64_t>(mb) * 1024 * 1024;
        };
        ram = fmt::format(
            "{} ({}/{})",
            ram,
            agentxx::util::formatSize(mbToBytes(st.memUsedMb), 1024, false),
            agentxx::util::formatSize(mbToBytes(st.memTotalMb), 1024, false)
        );
    }
    textItem(ram);
    if (st.gpuCount == 0) {
        // 无gpu，不显示
    } else if (st.gpuCount == 1) {
        textItem(fmt::format("|- GPU {:.0f}%", st.gpuPeakPct));
    } else {
        textItem(fmt::format("|- GPU {}x · {:.0f}%", st.gpuCount, st.gpuPeakPct));
    }
    return fmt::format(R"({{"items":[{}]}})", fmt::join(items, ","));
}

/// 用最新数据刷新 Info 段落 (client io 线程调用; 开关关闭时显示占位提示)
static void refreshUsageDisplay() {
    if (!g_client_host) {
        return;
    }
    // Info 段落: 明细 (开关关闭时显示占位)
    if (g_section && !g_last_usage_json.empty() && g_client_ui
        && g_client_ui->update_info_section) {
        std::string json;
        if (g_usage_enabled.load(std::memory_order_relaxed)) {
            json = buildUsageInfoItemsJson(parseUsage(g_last_usage_json));
        } else {
            json = R"({"items":[{"kind":"text","role":"hint","text":"System info: OFF"}]})";
        }
        g_client_ui->update_info_section(
            g_client_host,
            g_section,
            agentxx_plugin_sv(json.data(), json.size())
        );
    }
}

/// PLUGIN_DATA 事件: 过滤宿主转发的系统资源占用事件 (agentxx_system_monitor.usage)
static void on_client_plugin_data(AgentxxPluginStringView payload_json, void* ud) {
    (void)ud;
    if (!g_client_host || !g_section) {
        return;
    }
    // payload: {"plugin","event","data"}
    char* plugin   = g_client_if->json
                         ? g_client_if->json->json_get_string(
                       g_client_host, payload_json, AGENTXX_SV("plugin"))
                         : nullptr;
    char* event    = g_client_if->json
                        ? g_client_if->json->json_get_string(
                      g_client_host, payload_json, AGENTXX_SV("event"))
                        : nullptr;
    char* data     = g_client_if->json
                       ? g_client_if->json->json_get_string(
                     g_client_host, payload_json, AGENTXX_SV("data"))
                       : nullptr;
    const bool mine = plugin && event && std::strcmp(plugin, "agentxx_system_monitor") == 0
                      && std::strcmp(event, "usage") == 0 && data;
    if (mine) {
        // 缓存原始数据; refreshUsageDisplay 内部按开关状态决定渲染
        // (开启: Info 明细; 关闭: Info 段落显示占位)
        g_last_usage_json = data;
        refreshUsageDisplay();
    }
    if (plugin) {
        g_client_host->vtable->free(plugin);
    }
    if (event) {
        g_client_host->vtable->free(event);
    }
    if (data) {
        g_client_host->vtable->free(data);
    }
}

/// 命令 /sysinfo: 切换系统资源显示开关 (返回 toast 动作;
/// 开关状态经跨端事件 usage_enabled 上行同步, agent 侧据此跳过采集)
static char* sysinfo_cmd_execute(void* ud, AgentxxPluginStringView args_json, char** error_out) {
    (void)ud;
    (void)args_json;
    (void)error_out;
    if (!g_client_host) {
        return nullptr;
    }
    const bool next = !g_usage_enabled.load(std::memory_order_relaxed);
    g_usage_enabled.store(next, std::memory_order_relaxed);
    // 立即按新开关状态刷新 (重新开启时用缓存的最新数据; 关闭时 Info 段落
    // 显示占位); 数据在关闭期间仍持续接收缓存
    refreshUsageDisplay();
    // 上行同步: agent 侧插件订阅 client.agentxx_system_monitor.usage_enabled,
    // 关闭期间跳过周期采集 (省采样开销/网络流量)
    {
        std::string payload = next ? R"({"enabled":true})" : R"({"enabled":false})";
        if (g_client_if->wire && g_client_if->wire->send_plugin_data)
            g_client_if->wire->send_plugin_data(
            g_client_host,
              AGENTXX_SV("usage_enabled"),
              agentxx_plugin_sv(payload.data(), payload.size())
        );
    }
    std::string       text = next ? "System resource info: ON" : "System resource info: OFF";
    // 对端可用性检查: get_client_state("agentPlugins") 为服务端已加载的
    // agent 侧插件结构化列表 [{name,version,interfaces},...] (宿主约定事件
    // server_plugins / HelloAck.plugins; 空数组 = 服务端未提供, 不据此断言
    // 缺失)。agent 侧插件缺失时上行开关同步会被静默丢弃 (采集照旧) ——
    // 明确提示, 避免"操作成功"假象
    {
        char* stateJson    = g_client_if->session && g_client_if->session->get_client_state
                                          ? g_client_if->session->get_client_state(g_client_host)
                                          : nullptr;
        bool  agentMissing = false;
        if (stateJson) {
            SimpleJson st{std::string(stateJson)};
            if (st.ok()) {
                simdjson::ondemand::array arr;
                if (!st.doc().at_pointer("/agentPlugins").get(arr)) {
                    size_t n     = 0;
                    bool   found = false;
                    for (auto v : arr) {
                        ++n;
                        // 元素为对象: 取 name 字段比对
                        simdjson::ondemand::object obj;
                        if (v.get_object().get(obj) != simdjson::SUCCESS) {
                            continue;
                        }
                        std::string_view sv;
                        if (obj["name"].get_string().get(sv) == simdjson::SUCCESS
                            && sv == "agentxx_system_monitor") {
                            found = true;
                        }
                    }
                    agentMissing = (n > 0 && !found);
                }
            }
            g_client_host->vtable->free(stateJson);
        }
        if (agentMissing) {
            text += " (warn: plugin missing on server side; toggle is local only)";
        }
    }
    const std::string out
        = fmt::format(R"({{"action":"toast","text":{},"level":0}})", clientJsonEscape(text));
    return g_client_host->vtable->strdup(out.c_str());
}

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxClientPluginInfo* agentxx_client_get_info(void) {
    static const AgentxxClientPluginInfo info{
        AGENTXX_CLIENT_PLUGIN_API_VERSION,
        AGENTXX_SV("agentxx_system_monitor"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV("System resource usage: Info section (CPU/RAM/GPU), /sysinfo toggle"),
    };
    return &info;
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_client_entry(const AgentxxClientHost* host, void** plugin_ctx) {
    g_client_host = host;
    (void)plugin_ctx;

    // COM 风格接口表查询 (entry 一次性查询缓存; 进程级静态数据)
    static const agentxx::plugin::ClientIfaces s_if = agentxx::plugin::ClientIfaces::query(host);
    g_client_if = &s_if;
    g_client_ui = s_if.ui;

    // 1. 侧边栏 Info 栏段落 (资源占用明细: CPU/RAM/GPU; 内容由
    //    refreshUsageDisplay 更新)
    g_section = g_client_ui && g_client_ui->register_info_section
                  ? g_client_ui->register_info_section(
                      host,
                      AGENTXX_SV("agentxx_system_monitor.usage"),
                      AGENTXX_SV(R"({"title":"System"})")
                  )
                  : nullptr;
    // 宿主不支持 Info 段落时成员为 NULL, 插件降级 (不视为失败)

    // 2. 事件订阅: 宿主转发的系统资源事件 (WirePluginData agentxx_system_monitor.usage)
    if (!s_if.events || !s_if.events->subscribe
        || !s_if.events->subscribe(
            host,
            AGENTXX_CLIENT_EVT_PLUGIN_DATA,
            on_client_plugin_data,
            nullptr
        )) {
        return -1;
    }

    // 3. 命令 /sysinfo: 切换显示 (无命令输入面的宿主成员为 NULL → 加载失败;
    //    命令是本插件核心交互, 与原 register 失败行为一致)
    if (!g_client_ui || !g_client_ui->register_command
        || g_client_ui->register_command(
               host,
               AGENTXX_SV("sysinfo"),
               AGENTXX_SV("Toggle system resource info display (CPU/RAM/GPU Info section)"),
               sysinfo_cmd_execute,
               nullptr
           )
               != 0) {
        return -1;
    }

    if (s_if.log && s_if.log->log) {
        s_if.log->log(host, 2, AGENTXX_SV("agentxx_system_monitor client loaded"));
    }
    return 0;
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_client_unload(void* plugin_ctx) {
    (void)plugin_ctx;
    if (!g_client_host || !g_client_if) {
        return;
    }
    if (g_section && g_client_ui->unregister_info_section) {
        g_client_ui->unregister_info_section(g_client_host, g_section);
        g_section = nullptr;
    }
    if (g_client_ui->unregister_command) {
        g_client_ui->unregister_command(g_client_host, AGENTXX_SV("sysinfo"));
    }
    g_last_usage_json.clear();
    if (g_client_if->log && g_client_if->log->log) {
        g_client_if->log->log(
            g_client_host,
            2,
            AGENTXX_SV("agentxx_system_monitor client unloaded")
        );
    }
    g_client_host = nullptr;
    g_client_if   = nullptr;
    g_client_ui   = nullptr;
}
