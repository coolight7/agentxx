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
    std::atomic<bool>                            usageEnabled{true};
    agentxx_system_monitor_plugin::PluginLogSink log_sink;
};

static auto ctxGuardLogger(PluginCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx) {
            ctx->log.error(msg ? msg : "");
        }
    };
}

static std::string usageToJson(const agentxx_system_monitor_plugin::CpuGpuUsage& u) {
    neograph::json j;
    j["cpu"]            = u.cpuUsagePercent;
    j["mem_total_mb"]   = u.memory.totalPhysicalMB;
    j["mem_used_mb"]    = u.memory.usedPhysicalMB;
    j["mem_percent"]    = u.memory.usagePercent;
    neograph::json gpus = neograph::json::array();
    for (const auto& g : u.gpus) {
        gpus.push_back({
            {"name",                   g.name               },
            {"dedicated_vram_mb",      g.dedicatedVramMB    },
            {"dedicated_vram_used_mb", g.dedicatedVramUsedMB},
            {"shared_vram_mb",         g.sharedVramMB       },
            {"shared_vram_used_mb",    g.sharedVramUsedMB   },
            {"usage_percent",          g.usagePercent       }
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
        [&raw](const char* msg) noexcept {
            ctxGuardLogger(raw)(msg);
        },
        -1,
        [&]() -> int {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            auto ctx = std::make_unique<PluginCtx>();
            ctx->init(host);
            raw = ctx.get();

            ctx->log_sink = [raw = ctx.get()](int level, const std::string& msg) {
                if (raw) {
                    raw->log.log(level, msg);
                }
            };
            agentxx_system_monitor_plugin::g_log_sink.store(
                &ctx->log_sink,
                std::memory_order_release
            );

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
                [](PluginCtx&, const AgentxxHost*, std::string_view, std::string_view
                ) -> std::string {
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
                        if (!c) {
                            return;
                        }
                        try {
                            std::string s(
                                event_json.data ? event_json.data : "{}",
                                event_json.size
                            );
                            auto j = neograph::json::parse(s);
                            c->usageEnabled.store(
                                j.value("enabled", true),
                                std::memory_order_release
                            );
                        } catch (...) {
                        }
                    },
                    ctx.get()
                );

                // 客户端接入/重连时立即发布一次状态快照 (修复初始状态滞留为空)
                ctx->iface.events->subscribe(
                    host,
                    AGENTXX_SV("agentxx_host.client_attached"),
                    [](AgentxxPluginStringView, void* ud) {
                        auto* c = static_cast<PluginCtx*>(ud);
                        if (!c || !c->usageEnabled.load(std::memory_order_relaxed)) {
                            return;
                        }
                        if (!c->iface.scheduler || !c->iface.scheduler->offload) {
                            return;
                        }
                        c->iface.scheduler->offload(
                            c->host,
                            nullptr,
                            [](void*, volatile int*, char**) -> void* {
                                return new agentxx_system_monitor_plugin::CpuGpuUsage(querySync());
                            },
                            [](void* ud, void* res, char* err) {
                                auto* c = static_cast<PluginCtx*>(ud);
                                auto* u
                                    = static_cast<agentxx_system_monitor_plugin::CpuGpuUsage*>(res);
                                if (c && u && c->iface.events && c->iface.events->publish) {
                                    std::string json = usageToJson(*u);
                                    c->iface.events->publish(
                                        c->host,
                                        AGENTXX_SV("agentxx_system_monitor.usage"),
                                        agentxx_plugin_sv(json.data(), json.size())
                                    );
                                }
                                delete u;
                                if (err && c && c->host && c->host->vtable
                                    && c->host->vtable->free) {
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
            agentxx::kit::spawn(
                *ctx,
                [](PluginCtx& c, agentxx::kit::OpCtl ctl) -> agentxx::kit::Task<void> {
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
                }
            );

            *plugin_ctx = ctx.release();
            return 0;
        }
    );
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

/* ==================== client 侧入口 —— 系统资源占用渲染 (Append 段风格, 恢复 1e524e62)
 * ==================== */

struct ClientCtx {
    const AgentxxClientHost*      host = nullptr;
    agentxx::plugin::ClientIfaces iface{};
    const AgentxxClientUiIface*   ui      = nullptr;
    AgentxxInfoSection*           section = nullptr;
    std::atomic<bool>             usage_enabled{true};
    std::string                   last_usage_json;

    void logErr(const char* m) const noexcept {
        agentxx::plugin_guard::logTo(host, iface.log, 4, "agentxx_system_monitor", m ? m : "");
    }
};

struct UsageStat {
    double  cpu        = 0.0;
    double  memPct     = 0.0;
    int64_t memUsedMb  = 0;
    int64_t memTotalMb = 0;
    size_t  gpuCount   = 0;
    double  gpuPeakPct = 0.0;
};

static UsageStat parseUsage(const std::string& raw) {
    UsageStat st;
    try {
        auto j        = neograph::json::parse(raw);
        st.cpu        = j.value("cpu", 0.0);
        st.memPct     = j.value("mem_percent", 0.0);
        st.memUsedMb  = j.value<int64_t>("mem_used_mb", 0);
        st.memTotalMb = j.value<int64_t>("mem_total_mb", 0);
        if (j.contains("gpus") && j["gpus"].is_array()) {
            for (const auto& elem : j["gpus"]) {
                ++st.gpuCount;
                double u = 0.0;
                if (elem.is_object() && elem.contains("usage_percent")) {
                    u = elem.value("usage_percent", 0.0);
                }
                if (u > st.gpuPeakPct) {
                    st.gpuPeakPct = u;
                }
            }
        }
    } catch (...) {
    }
    return st;
}

static std::string buildUsageInfoItemsJson(const ClientCtx& ctx, const UsageStat& st) {
    (void)ctx;
    neograph::json items    = neograph::json::array();
    auto           pushText = [&](const std::string& text, const std::string& role = "normal") {
        neograph::json it;
        it["kind"] = "text";
        it["role"] = role;
        it["text"] = text;
        items.push_back(std::move(it));
    };
    pushText(fmt::format("|- CPU {:.0f}%", st.cpu), "normal");
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
    pushText(ram, "normal");
    if (st.gpuCount == 1) {
        pushText(fmt::format("|- GPU {:.0f}%", st.gpuPeakPct), "normal");
    } else if (st.gpuCount > 1) {
        pushText(fmt::format("|- GPU {}x \u00b7 {:.0f}%", st.gpuCount, st.gpuPeakPct), "normal");
    }
    neograph::json out;
    out["items"] = std::move(items);
    return out.dump();
}

static void refreshUsageDisplay(ClientCtx& ctx) {
    if (!ctx.host) {
        return;
    }
    if (!ctx.section && ctx.ui && ctx.ui->register_info_section) {
        ctx.section = ctx.ui->register_info_section(
            ctx.host,
            AGENTXX_SV("agentxx_system_monitor.usage"),
            AGENTXX_SV(R"({"title":"System"})")
        );
    }
    if (!ctx.section || !ctx.ui || !ctx.ui->update_info_section || ctx.last_usage_json.empty()) {
        return;
    }
    std::string json;
    if (ctx.usage_enabled.load(std::memory_order_relaxed)) {
        json = buildUsageInfoItemsJson(ctx, parseUsage(ctx.last_usage_json));
    } else {
        neograph::json off;
        off["items"] = neograph::json::array();
        neograph::json it;
        it["kind"] = "text";
        it["role"] = "hint";
        it["text"] = "System info: OFF";
        off["items"].push_back(std::move(it));
        json = off.dump();
    }
    ctx.ui->update_info_section(ctx.host, ctx.section, agentxx_plugin_sv(json.data(), json.size()));
}

static void onClientPluginData(AgentxxPluginStringView payload_json, void* ud) {
    auto* ctx = static_cast<ClientCtx*>(ud);
    if (!ctx || !ctx->host) {
        return;
    }
    std::string raw(payload_json.data ? payload_json.data : "{}", payload_json.size);
    try {
        auto j = neograph::json::parse(raw);
        if (j.value("plugin", std::string{}) != "agentxx_system_monitor"
            || j.value("event", std::string{}) != "usage") {
            return;
        }
        neograph::json u;
        if (j.contains("data")) {
            auto dv = j["data"];
            if (dv.is_string()) {
                try {
                    u = neograph::json::parse(dv.get<std::string>());
                } catch (...) {
                    return;
                }
            } else if (dv.is_object()) {
                u = dv;
            } else {
                return;
            }
        } else {
            return;
        }
        ctx->last_usage_json = u.dump();
        refreshUsageDisplay(*ctx);
    } catch (...) {
    }
}

static char* cmdSysinfoExecute(void* ud, AgentxxPluginStringView args_json, char** errorOut) {
    (void)args_json;
    (void)errorOut;
    auto* ctx = static_cast<ClientCtx*>(ud);
    if (!ctx || !ctx->host) {
        return nullptr;
    }
    return agentxx::plugin_guard::guardCall(
        [ctx](const char* m) noexcept {
            ctx->logErr(m);
        },
        nullptr,
        [&]() -> char* {
            const bool next = !ctx->usage_enabled.load(std::memory_order_relaxed);
            ctx->usage_enabled.store(next, std::memory_order_relaxed);
            refreshUsageDisplay(*ctx);
            if (ctx->iface.wire && ctx->iface.wire->send_plugin_data) {
                std::string payload = next ? R"({"enabled":true})" : R"({"enabled":false})";
                ctx->iface.wire->send_plugin_data(
                    ctx->host,
                    AGENTXX_SV("usage_enabled"),
                    agentxx_plugin_sv(payload.data(), payload.size())
                );
            }
            std::string text = next ? "System resource info: ON" : "System resource info: OFF";
            {
                char* stateJson    = ctx->iface.session && ctx->iface.session->get_client_state
                                         ? ctx->iface.session->get_client_state(ctx->host)
                                         : nullptr;
                bool  agentMissing = false;
                if (stateJson) {
                    try {
                        auto st = neograph::json::parse(std::string(stateJson));
                        if (st.contains("agentPlugins") && st["agentPlugins"].is_array()) {
                            size_t n     = 0;
                            bool   found = false;
                            for (const auto& v : st["agentPlugins"]) {
                                ++n;
                                if (v.is_object()
                                    && v.value("name", std::string{}) == "agentxx_system_monitor") {
                                    found = true;
                                }
                            }
                            agentMissing = (n > 0 && !found);
                        }
                    } catch (...) {
                    }
                    ctx->host->vtable->free(stateJson);
                }
                if (agentMissing) {
                    text += " (warn: plugin missing on server side; toggle is local only)";
                }
            }
            neograph::json esc;
            esc["text"] = text;
            // use simple json escape via neograph dump then extract? easier use clientJson logic
            // via neograph 但 toast 的 text 需要 json 转义，neograph 会自动处理，直接构造
            neograph::json out;
            out["action"]      = "toast";
            out["text"]        = text;
            out["level"]       = 0;
            std::string dumped = out.dump();
            return ctx->host->vtable->strdup(dumped.c_str());
        }
    );
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
    agentxx_client_create(const AgentxxClientHost* host, void** plugin_ctx) {
    ClientCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
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
            auto ctx   = std::make_unique<ClientCtx>();
            ctx->host  = host;
            ctx->iface = agentxx::plugin::ClientIfaces::query(host);
            ctx->ui    = AGENTXX_QUERY_IFACE(host, AgentxxClientUiIface, AGENTXX_IFACE_CLIENT_UI);
            raw        = ctx.get();

            if (ctx->ui && ctx->ui->register_info_section) {
                ctx->section = ctx->ui->register_info_section(
                    host,
                    AGENTXX_SV("agentxx_system_monitor.usage"),
                    AGENTXX_SV(R"({"title":"System"})")
                );
            }

            if (!ctx->iface.events || !ctx->iface.events->subscribe
                || !ctx->iface.events->subscribe(
                    host,
                    AGENTXX_CLIENT_EVT_PLUGIN_DATA,
                    onClientPluginData,
                    ctx.get()
                )) {
                return -1;
            }

            if (ctx->ui && ctx->ui->register_command) {
                if (ctx->ui->register_command(
                        host,
                        AGENTXX_SV("sysinfo"),
                        AGENTXX_SV("Toggle system resource info display (CPU/RAM/GPU Info section)"
                        ),
                        cmdSysinfoExecute,
                        ctx.get()
                    )
                    != 0) {
                    return -1;
                }
            }

            if (ctx->iface.log && ctx->iface.log->log) {
                ctx->iface.log->log(host, 2, AGENTXX_SV("agentxx_system_monitor client loaded"));
            }
            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_client_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<ClientCtx*>(plugin_ctx);
    agentxx::plugin_guard::guardCallVoid(
        [ctx](const char* m) noexcept {
            if (ctx) {
                ctx->logErr(m);
            }
        },
        [&] {
            if (!ctx || !ctx->host) {
                delete ctx;
                return;
            }
            if (ctx->section && ctx->ui && ctx->ui->unregister_info_section) {
                ctx->ui->unregister_info_section(ctx->host, ctx->section);
                ctx->section = nullptr;
            }
            if (ctx->ui && ctx->ui->unregister_command) {
                ctx->ui->unregister_command(ctx->host, AGENTXX_SV("sysinfo"));
            }
            ctx->last_usage_json.clear();
            if (ctx->iface.log && ctx->iface.log->log) {
                ctx->iface.log
                    ->log(ctx->host, 2, AGENTXX_SV("agentxx_system_monitor client unloaded"));
            }
            delete ctx;
        }
    );
}
