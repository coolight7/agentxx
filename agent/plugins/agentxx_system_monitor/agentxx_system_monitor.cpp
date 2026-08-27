// agentxx_system_monitor —— 系统资源监控插件 (CPU/内存/GPU)
#include "agentxx/plugin/client_plugin_api.h"
#include "agentxx/plugin/plugin_guard.h"
#include "agentxx/plugin/plugin_kit.h"
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

static agentxx_system_monitor_plugin::CpuGpuUsage querySync() {
    asio::io_context                           io;
    agentxx_system_monitor_plugin::CpuGpuUsage usage;
    asio::co_spawn(
        io,
        [&usage]() -> asio::awaitable<void> {
            agentxx_system_monitor_plugin::CpuGpuMonitor monitor;
            usage = co_await monitor.query();
        },
        asio::detached
    );
    io.run();
    return usage;
}

static constexpr int kUsageIntervalSec = 5;

struct PluginCtx : public agentxx::kit::PluginBase {
    std::atomic<bool> usageEnabled{true};
    agentxx_system_monitor_plugin::PluginLogSink log_sink;
};

static auto ctxGuardLogger(PluginCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx) {
            ctx->log.error(msg ? msg : "");
        }
    };
}

static std::string
    usageToJson(const agentxx_system_monitor_plugin::CpuGpuUsage& u) {
    neograph::json j;
    j["cpu"] = u.cpuUsagePercent;
    j["mem_total_mb"] = u.memory.totalPhysicalMB;
    j["mem_used_mb"] = u.memory.usedPhysicalMB;
    j["mem_percent"] = u.memory.usagePercent;
    neograph::json gpus = neograph::json::array();
    for (const auto& g : u.gpus) {
        gpus.push_back({
            {"name", g.name},
            {"dedicated_vram_mb", g.dedicatedVramMB},
            {"dedicated_vram_used_mb", g.dedicatedVramUsedMB},
            {"shared_vram_mb", g.sharedVramMB},
            {"shared_vram_used_mb", g.sharedVramUsedMB},
            {"usage_percent", g.usagePercent}
        });
    }
    j["gpus"] = std::move(gpus);
    return j.dump();
}

static std::string formatUsageText(const agentxx_system_monitor_plugin::CpuGpuUsage& usage) {
    std::stringstream ss;
    ss << fmt::format("CPU Usage: {:.1f}%\n", usage.cpuUsagePercent);
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
    return ss.str();
}

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

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_create(const AgentxxHost* host, void** plugin_ctx) {
    PluginCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
        [&raw](const char* msg) noexcept { ctxGuardLogger(raw)(msg); },
        -1,
        [&]() -> int {
        if (!host || !host->vtable || !plugin_ctx) {
            return -1;
        }
        auto ctx  = std::make_unique<PluginCtx>();
        ctx->init(host);
        raw = ctx.get();

        ctx->log_sink = [raw = ctx.get()](int level, const std::string& msg) {
            if (raw) {
                raw->log.log(level, msg);
            }
        };
        agentxx_system_monitor_plugin::g_log_sink.store(&ctx->log_sink, std::memory_order_release);

        if (!ctx->iface.tools || !ctx->iface.tools->register_tool) {
            return -1;
        }

        // 1. 工具: agentxx_get_system_core_info (blocking_tool)
        agentxx::kit::blocking_tool(
            *ctx,
            "agentxx_get_system_core_info",
            "Get system resource usage: CPU utilization, memory usage, GPU utilization, and GPU memory usage.",
            R"({"type":"object","properties":{}})",
            [](std::string_view) -> std::string {
                auto usage = querySync();
                return formatUsageText(usage);
            }
        );

        // 2. 能力: agentxx.system_usage (方法 query)
        agentxx::kit::capability(
            *ctx,
            "agentxx.system_usage",
            [](PluginCtx&, const AgentxxHost*, std::string_view, std::string_view) -> std::string {
                auto usage = querySync();
                return usageToJson(usage);
            }
        );

        // 3. 跨端控制事件与宿主约定事件订阅
        if (ctx->iface.events && ctx->iface.events->subscribe) {
            ctx->iface.events->subscribe(
                host,
                AGENTXX_SV("client.agentxx_system_monitor.usage_enabled"),
                [](AgentxxPluginStringView event_json, void* ud) {
                    auto* c = static_cast<PluginCtx*>(ud);
                    if (!c) return;
                    try {
                        std::string s(event_json.data ? event_json.data : "{}", event_json.size);
                        auto j = neograph::json::parse(s);
                        c->usageEnabled.store(j.value("enabled", true), std::memory_order_release);
                    } catch (...) {}
                },
                ctx.get()
            );

            // 客户端接入/重连时立即发布一次状态快照 (修复初始状态滞留为空)
            ctx->iface.events->subscribe(
                host,
                AGENTXX_SV("agentxx_host.client_attached"),
                [](AgentxxPluginStringView, void* ud) {
                    auto* c = static_cast<PluginCtx*>(ud);
                    if (!c || !c->usageEnabled.load(std::memory_order_relaxed)) return;
                    if (!c->iface.scheduler || !c->iface.scheduler->offload) return;
                    c->iface.scheduler->offload(
                        c->host,
                        nullptr,
                        [](void*, volatile int*, char**) -> void* {
                            return new agentxx_system_monitor_plugin::CpuGpuUsage(querySync());
                        },
                        [](void* ud, void* res, char* err) {
                            auto* c = static_cast<PluginCtx*>(ud);
                            auto* u = static_cast<agentxx_system_monitor_plugin::CpuGpuUsage*>(res);
                            if (c && u && c->iface.events && c->iface.events->publish) {
                                std::string json = usageToJson(*u);
                                c->iface.events->publish(
                                    c->host,
                                    AGENTXX_SV("agentxx_system_monitor.usage"),
                                    agentxx_plugin_sv(json.data(), json.size())
                                );
                            }
                            delete u;
                            if (err && c && c->host && c->host->vtable && c->host->vtable->free) {
                                c->host->vtable->free(err);
                            }
                        },
                        c
                    );
                },
                ctx.get()
            );
        }

        // 4. 周期采集后台任务 (spawn + sleep + offload)
        agentxx::kit::spawn(*ctx, [](PluginCtx& c, agentxx::kit::OpCtl ctl) -> agentxx::kit::Task<void> {
            while (!ctl.cancelled()) {
                if (c.usageEnabled.load(std::memory_order_relaxed)) {
                    auto usage = co_await agentxx::kit::offload(c, [](volatile int*) {
                        return querySync();
                    });
                    if (ctl.cancelled()) {
                        break;
                    }
                    std::string json = usageToJson(usage);
                    if (c.iface.events && c.iface.events->publish) {
                        c.iface.events->publish(
                            c.host,
                            AGENTXX_SV("agentxx_system_monitor.usage"),
                            agentxx_plugin_sv(json.data(), json.size())
                        );
                    }
                }
                co_await agentxx::kit::sleep(c, kUsageIntervalSec * 1000);
            }
        });

        *plugin_ctx = ctx.release();
        return 0;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    agentxx::plugin_guard::guardCallVoid(ctxGuardLogger(ctx), [&] {
        if (ctx) {
            const auto* exp = &ctx->log_sink;
            agentxx_system_monitor_plugin::g_log_sink.compare_exchange_strong(exp, nullptr);
            delete ctx;
        }
    });
}

/* ==================== client 侧入口 ==================== */

struct ClientPluginCtx {
    const AgentxxClientHost*      host         = nullptr;
    agentxx::plugin::ClientIfaces iface        {};
    const AgentxxClientUiIface*   ui           = nullptr;
    AgentxxInfoSection*           info_section = nullptr;
    bool                          enabled      = true;
};

static auto clientGuardLogger(ClientPluginCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx && ctx->host && ctx->iface.log && ctx->iface.log->log) {
            ctx->iface.log->log(ctx->host, 4, agentxx_plugin_sv(msg, msg ? strlen(msg) : 0));
        }
    };
}

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxClientPluginInfo* agentxx_client_get_info(void) {
    static const AgentxxClientPluginInfo info{
        AGENTXX_CLIENT_PLUGIN_API_VERSION,
        AGENTXX_SV("agentxx_system_monitor"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV("System resource monitor: renders CPU/RAM/GPU usage in sidebar"),
    };
    return &info;
}

static void onClientPluginData(AgentxxPluginStringView payload_json, void* ud) {
    auto* ctx = static_cast<ClientPluginCtx*>(ud);
    if (!ctx || !ctx->ui || !ctx->enabled) {
        return;
    }
    std::string raw(payload_json.data ? payload_json.data : "{}", payload_json.size);
    try {
        auto j = neograph::json::parse(raw);
        if (j.value("plugin", "") != "agentxx_system_monitor" || j.value("event", "") != "usage") {
            return;
        }
        auto dataVal = j.contains("data") ? j["data"] : neograph::json::object();
        neograph::json u;
        if (dataVal.is_string()) {
            u = neograph::json::parse(dataVal.get<std::string>());
        } else if (dataVal.is_object()) {
            u = dataVal;
        } else {
            return;
        }

        double cpu = u.value("cpu", 0.0);
        double memPct = u.value("mem_percent", 0.0);
        uint64_t memUsed = u.value<uint64_t>("mem_used_mb", 0);
        uint64_t memTotal = u.value<uint64_t>("mem_total_mb", 0);

        std::string cpuLine = fmt::format("CPU: {:.1f}%", cpu);
        std::string memLine = fmt::format("RAM: {:.1f}% ({}/{} MB)", memPct, memUsed, memTotal);

        neograph::json items = neograph::json::array();
        items.push_back({{"kind", "text"}, {"role", "normal"}, {"text", cpuLine}});
        items.push_back({{"kind", "text"}, {"role", "normal"}, {"text", memLine}});

        if (u.contains("gpus") && u["gpus"].is_array()) {
            for (const auto& g : u["gpus"]) {
                std::string gName = g.value("name", "GPU");
                double gUsage = g.value("usage_percent", 0.0);
                uint64_t vramUsed = g.value<uint64_t>("dedicated_vram_used_mb", 0);
                uint64_t vramTotal = g.value<uint64_t>("dedicated_vram_mb", 0);
                std::string gLine = fmt::format("{}: {:.1f}% ({}/{} MB)", gName, gUsage, vramUsed, vramTotal);
                items.push_back({{"kind", "text"}, {"role", "normal"}, {"text", gLine}});
            }
        }

        if (!ctx->info_section && ctx->ui->register_info_section) {
            ctx->info_section = ctx->ui->register_info_section(
                ctx->host,
                AGENTXX_SV("agentxx_system_monitor.usage"),
                AGENTXX_SV(R"({"title":"System Usage"})")
            );
        }
        if (ctx->info_section && ctx->ui->update_info_section) {
            neograph::json sectionJson;
            sectionJson["items"] = std::move(items);
            std::string out = sectionJson.dump();
            ctx->ui->update_info_section(ctx->host, ctx->info_section, agentxx_plugin_sv(out.data(), out.size()));
        }
    } catch (...) {}
}

static char* cmdSysinfoExecute(void* ud, AgentxxPluginStringView, char** errorOut) {
    (void)errorOut;
    auto* ctx = static_cast<ClientPluginCtx*>(ud);
    if (!ctx || !ctx->host || !ctx->host->vtable || !ctx->host->vtable->strdup) return nullptr;
    ctx->enabled = !ctx->enabled;

    if (ctx->iface.wire && ctx->iface.wire->send_plugin_data) {
        std::string j = fmt::format(R"({{"enabled":{}}})", ctx->enabled ? "true" : "false");
        ctx->iface.wire->send_plugin_data(ctx->host, AGENTXX_SV("usage_enabled"), agentxx_plugin_sv(j.data(), j.size()));
    }

    std::string toast = fmt::format("System monitor display: {}", ctx->enabled ? "ON" : "OFF");
    std::string action = fmt::format(R"({{"action":"toast","text":"{}","level":0}})", toast);
    return ctx->host->vtable->strdup(action.c_str());
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_client_create(const AgentxxClientHost* host, void** plugin_ctx) {
    ClientPluginCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
        [&raw](const char* msg) noexcept { clientGuardLogger(raw)(msg); },
        -1,
        [&]() -> int {
        if (!host || !host->vtable || !plugin_ctx) return -1;
        auto ctx = std::make_unique<ClientPluginCtx>();
        ctx->host = host;
        ctx->iface = agentxx::plugin::ClientIfaces::query(host);
        raw = ctx.get();

        ctx->ui = AGENTXX_QUERY_IFACE(host, AgentxxClientUiIface, AGENTXX_IFACE_CLIENT_UI);
        if (ctx->ui) {
            if (ctx->ui->register_info_section) {
                ctx->info_section = ctx->ui->register_info_section(
                    host,
                    AGENTXX_SV("agentxx_system_monitor.usage"),
                    AGENTXX_SV(R"({"title":"System Usage"})")
                );
            }
            if (ctx->ui->register_command) {
                ctx->ui->register_command(
                    host,
                    AGENTXX_SV("sysinfo"),
                    AGENTXX_SV("Toggle system monitor sidebar display"),
                    cmdSysinfoExecute,
                    ctx.get()
                );
            }
        }

        if (ctx->iface.events && ctx->iface.events->subscribe) {
            ctx->iface.events->subscribe(host, AGENTXX_CLIENT_EVT_PLUGIN_DATA, onClientPluginData, ctx.get());
        }

        *plugin_ctx = ctx.release();
        return 0;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_client_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<ClientPluginCtx*>(plugin_ctx);
    agentxx::plugin_guard::guardCallVoid(clientGuardLogger(ctx), [&] {
        if (!ctx) return;
        if (ctx->ui && ctx->ui->unregister_info_section && ctx->info_section) {
            ctx->ui->unregister_info_section(ctx->host, ctx->info_section);
            ctx->info_section = nullptr;
        }
        delete ctx;
    });
}
