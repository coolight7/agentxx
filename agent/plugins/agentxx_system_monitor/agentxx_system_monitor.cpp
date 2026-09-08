/// agentxx_system_monitor 插件 —— 系统资源监控工具与展示插件
#include "cpu_gpu_monitor.h"
#include "system_monitor_plugin.h"

#include "agentxx/plugin/api/client_plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/util/json.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "agentxx/util/util.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "fmt/format.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace agentxx_system_monitor_plugin;
using namespace agentxx::plugin;

namespace {

constexpr int kUsageIntervalSec = 5;

std::string usageToJson(const CpuGpuUsage& u) {
    agentxx::util::Json j;
    j["cpu"]                 = u.cpuUsagePercent;
    j["mem_total_mb"]        = u.memory.totalPhysicalMB;
    j["mem_used_mb"]         = u.memory.usedPhysicalMB;
    j["mem_percent"]         = u.memory.usagePercent;
    agentxx::util::Json gpus = agentxx::util::Json::array();
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

std::string formatUsageText(const CpuGpuUsage& usage) {
    std::stringstream ss;
    ss << fmt::format("CPU Usage: {:.1f}%\n", usage.cpuUsagePercent);
    const auto mbToBytes = [](uint64_t mb) {
        return mb * 1024 * 1024;
    };
    ss << fmt::format(
        "Memory: {:.1f}% · Used: {} / Total: {}\n",
        usage.memory.usagePercent,
        agentxx::util::formatSize(mbToBytes(usage.memory.usedPhysicalMB)),
        agentxx::util::formatSize(mbToBytes(usage.memory.totalPhysicalMB))
    );
    for (size_t i = 0; i < usage.gpus.size(); ++i) {
        const auto& gpu = usage.gpus[i];
        if (!gpu.name.empty()) {
            ss << fmt::format(
                "GPU {} [{}]: GPU Usage: {:.1f}%, VRAM: {}MB Used / {}MB Total",
                i,
                gpu.name,
                gpu.usagePercent,
                gpu.dedicatedVramUsedMB,
                gpu.dedicatedVramMB
            );
        } else {
            ss << fmt::format(
                "GPU {}: GPU Usage: {:.1f}%, VRAM: {}MB Used / {}MB Total",
                i,
                gpu.usagePercent,
                gpu.dedicatedVramUsedMB,
                gpu.dedicatedVramMB
            );
        }
        if (i + 1 < usage.gpus.size()) {
            ss << "\n";
        }
    }
    return ss.str();
}

} // namespace

struct SysMonCtx : public PluginBase {
    std::atomic<bool> usageEnabled{true};
    CpuGpuMonitor     monitor;

    CpuGpuUsage querySync() {
        asio::io_context io;
        CpuGpuUsage      usage;
        asio::co_spawn(
            io,
            [this, &usage]() -> asio::awaitable<void> {
                usage = co_await monitor.query();
            },
            asio::detached
        );
        io.run();
        return usage;
    }
};

AGENTXX_PLUGIN_AGENT_EXPORT(
    SysMonCtx,
    "agentxx_system_monitor",
    "1.0.0",
    "System resource monitor: CPU/memory/GPU usage tool",
    [](SysMonCtx& ctx) -> int32_t {
        // 1. 工具
        auto schema = ctx.schema("agentxx_get_system_core_info").build();
        blocking_tool(
            ctx,
            "agentxx_get_system_core_info",
            "Get system resource usage: CPU utilization, memory usage, GPU utilization, and GPU memory usage.",
            schema,
            [](SysMonCtx& c, std::string_view) -> std::string {
                auto usage = c.querySync();
                return formatUsageText(usage);
            }
        );

        // 2. 能力: agentxx.system_usage
        capability(
            ctx,
            "agentxx.system_usage",
            [](SysMonCtx& c, const AgentxxPluginHost*, std::string_view, std::string_view
            ) -> std::string {
                auto usage = c.querySync();
                return usageToJson(usage);
            }
        );

        // 3. 事件订阅
        if (ctx.iface.events && ctx.iface.events->subscribe) {
            auto t1 = PluginStringView::fromCstr("client.agentxx_system_monitor.usage_enabled");
            ctx.iface.events->subscribe(
                ctx.host,
                &t1,
                [](const AgentxxPluginStringView* event_json, void* ud) {
                    auto* c = static_cast<SysMonCtx*>(ud);
                    if (!c) {
                        return;
                    }
                    try {
                        std::string s(
                            event_json && event_json->data ? event_json->data : "{}",
                            event_json ? static_cast<size_t>(event_json->size) : 0
                        );
                        auto j = agentxx::util::Json::parse(s);
                        c->usageEnabled.store(j.value("enabled", true), std::memory_order_release);
                    } catch (...) {
                    }
                },
                &ctx
            );

            auto t2 = PluginStringView::fromCstr("agentxx_host.client_attached");
            ctx.iface.events->subscribe(
                ctx.host,
                &t2,
                [](const AgentxxPluginStringView*, void* ud) {
                    auto* c = static_cast<SysMonCtx*>(ud);
                    if (!c || !c->usageEnabled.load(std::memory_order_relaxed)) {
                        return;
                    }
                    if (!c->iface.scheduler || !c->iface.scheduler->offload) {
                        return;
                    }
                    c->iface.scheduler->offload(
                        c->host,
                        nullptr,
                        [](void* ud, volatile int32_t*, AgentxxPluginString*) -> void* {
                            auto* c = static_cast<SysMonCtx*>(ud);
                            return new CpuGpuUsage(c->querySync());
                        },
                        [](void* ud, void* res, const AgentxxPluginStringView*) {
                            auto* c = static_cast<SysMonCtx*>(ud);
                            if (res && c && c->host && c->iface.events
                                && c->iface.events->publish) {
                                auto*       u    = static_cast<CpuGpuUsage*>(res);
                                std::string json = usageToJson(*u);
                                auto        topicSv
                                    = PluginStringView::fromCstr("agentxx_system_monitor.usage");
                                auto jsonSv = PluginStringView::from(json.data(), json.size());
                                c->iface.events->publish(c->host, &topicSv, &jsonSv);
                                delete u;
                            }
                        },
                        c
                    );
                },
                &ctx
            );
        }

        // 4. 后台采样任务
        ctx.spawn([](SysMonCtx& c, OpCtl ctl) -> Task<void> {
            while (!ctl.cancelled()) {
                if (c.usageEnabled.load(std::memory_order_relaxed)) {
                    auto usage = co_await offload(c, [&](volatile int*) {
                        return c.querySync();
                    });
                    if (ctl.cancelled()) {
                        break;
                    }

                    std::string json = usageToJson(usage);
                    auto topicSv     = PluginStringView::fromCstr("agentxx_system_monitor.usage");
                    auto jsonSv      = PluginStringView::from(json.data(), json.size());
                    if (c.iface.events && c.iface.events->publish) {
                        c.iface.events->publish(c.host, &topicSv, &jsonSv);
                    }
                }
                co_await sleep(c, kUsageIntervalSec * 1000);
            }
        });

        return 0;
    }
);

struct SysMonClientCtx : public ClientPluginBase {
    AgentxxInfoSection* section = nullptr;
    std::atomic<bool>   usage_enabled{true};
    std::string         last_usage_json;
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
        auto j        = agentxx::util::Json::parse(raw);
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

static std::string buildUsageInfoItemsJson(const SysMonClientCtx&, const UsageStat& st) {
    agentxx::util::Json items = agentxx::util::Json::array();
    auto pushText             = [&](const std::string& text, const std::string& role = "normal") {
        agentxx::util::Json it;
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
            "{} · {}/{}",
            ram,
            agentxx::util::formatSize(mbToBytes(st.memUsedMb), 1024, false),
            agentxx::util::formatSize(mbToBytes(st.memTotalMb), 1024, false)
        );
    }
    pushText(ram, "normal");
    if (st.gpuCount == 1) {
        pushText(fmt::format("|- GPU {:.0f}%", st.gpuPeakPct), "normal");
    } else if (st.gpuCount > 1) {
        pushText(fmt::format("|- GPU {}x · {:.0f}%", st.gpuCount, st.gpuPeakPct), "normal");
    }
    agentxx::util::Json out;
    out["items"] = std::move(items);
    return out.dump();
}

static void refreshUsageDisplay(SysMonClientCtx& ctx) {
    if (!ctx.host) {
        return;
    }
    if (!ctx.section && ctx.iface.ui && ctx.iface.ui->register_info_section) {
        auto idSv    = PluginStringView::fromCstr("agentxx_system_monitor.usage");
        auto propsSv = PluginStringView::fromCstr(R"({"title":"System"})");
        ctx.section  = ctx.iface.ui->register_info_section(ctx.host, &idSv, &propsSv);
    }
    if (!ctx.section || !ctx.iface.ui || !ctx.iface.ui->update_info_section
        || ctx.last_usage_json.empty()) {
        return;
    }
    std::string json;
    if (ctx.usage_enabled.load(std::memory_order_relaxed)) {
        json = buildUsageInfoItemsJson(ctx, parseUsage(ctx.last_usage_json));
    } else {
        agentxx::util::Json off;
        off["items"] = agentxx::util::Json::array();
        json         = off.dump();
    }
    auto jsonSv = PluginStringView::from(json.data(), json.size());
    ctx.iface.ui->update_info_section(ctx.host, ctx.section, &jsonSv);
}

AGENTXX_PLUGIN_CLIENT_EXPORT(
    SysMonClientCtx,
    "agentxx_system_monitor",
    "1.0.0",
    "System resource usage: Info section (CPU/RAM/GPU), /sysinfo toggle",
    [](SysMonClientCtx& ctx) -> int32_t {
        if (!ctx.iface.ui) {
            return 0;
        }

        auto idSv    = PluginStringView::fromCstr("agentxx_system_monitor.usage");
        auto propsSv = PluginStringView::fromCstr(R"({"title":"System"})");
        ctx.section  = ctx.iface.ui->register_info_section(ctx.host, &idSv, &propsSv);

        if (ctx.iface.events && ctx.iface.events->subscribe) {
            ctx.iface.events->subscribe(
                ctx.host,
                AGENTXX_CLIENT_EVT_PLUGIN_DATA,
                [](const AgentxxPluginStringView* payload_json, void* ud) {
                    auto* ctx = static_cast<SysMonClientCtx*>(ud);
                    if (!ctx || PluginStringView::empty(payload_json)) {
                        return;
                    }
                    try {
                        auto j = agentxx::util::Json::parse(
                            std::string_view(payload_json->data, payload_json->size)
                        );
                        if (j.value("plugin", std::string{}) != "agentxx_system_monitor"
                            || j.value("event", std::string{}) != "usage") {
                            return;
                        }
                        if (j.contains("data") && j["data"].is_string()) {
                            ctx->last_usage_json = j["data"].get<std::string>();
                            refreshUsageDisplay(*ctx);
                        }
                    } catch (...) {
                    }
                },
                &ctx
            );
        }

        if (ctx.iface.ui->register_command) {
            auto nameSv = PluginStringView::fromCstr("sysinfo");
            auto descSv = PluginStringView::fromCstr(
                "Toggle system resource usage display in sidebar Info section"
            );
            ctx.iface.ui->register_command(
                ctx.host,
                &nameSv,
                &descSv,
                [](void* ud,
                   const AgentxxPluginStringView*,
                   AgentxxPluginString* actionOut,
                   AgentxxPluginString*) -> int32_t {
                    auto* ctx = static_cast<SysMonClientCtx*>(ud);
                    if (!ctx) {
                        return -1;
                    }
                    const bool next = !ctx->usage_enabled.load(std::memory_order_relaxed);
                    ctx->usage_enabled.store(next, std::memory_order_relaxed);
                    refreshUsageDisplay(*ctx);
                    if (ctx->iface.wire && ctx->iface.wire->send_plugin_data) {
                        std::string payload = next ? R"({"enabled":true})" : R"({"enabled":false})";
                        auto        evtSv   = PluginStringView::fromCstr("usage_enabled");
                        auto        paySv = PluginStringView::from(payload.data(), payload.size());
                        ctx->iface.wire->send_plugin_data(ctx->host, &evtSv, &paySv);
                    }
                    std::string text
                        = next ? "System resource info: ON" : "System resource info: OFF";
                    std::string stateStr = ctx->clientState();
                    if (!stateStr.empty() && stateStr != "{}") {
                        try {
                            auto st = agentxx::util::Json::parse(stateStr);
                            if (st.contains("agentPlugins") && st["agentPlugins"].is_array()) {
                                bool found = false;
                                for (const auto& v : st["agentPlugins"]) {
                                    if (v.is_object()
                                        && v.value("name", std::string{})
                                               == "agentxx_system_monitor") {
                                        found = true;
                                    }
                                }
                                if (!found) {
                                    text
                                        += " (warn: plugin missing on server side; toggle is local only)";
                                }
                            }
                        } catch (...) {
                        }
                    }
                    agentxx::util::Json out;
                    out["action"]      = "toast";
                    out["text"]        = text;
                    out["level"]       = 0;
                    std::string dumped = out.dump();
                    auto        paySv  = PluginStringView::from(dumped.data(), dumped.size());
                    if (actionOut) {
                        *actionOut = PluginString::from(ctx->host, &paySv);
                    }
                    return 0;
                },
                &ctx
            );
        }

        return 0;
    }
);
