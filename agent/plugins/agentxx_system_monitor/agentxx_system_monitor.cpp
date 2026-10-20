/// agentxx_system_monitor 插件 —— 系统资源监控工具与展示插件
#include "cpu_gpu_monitor.h"
#include "system_monitor_plugin.h"

#include "agentxx/plugin/api/client_plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"

#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "fmt/format.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using namespace agentxx_system_monitor_plugin;
using namespace agentxx::plugin;

namespace {

constexpr int kUsageIntervalSec = 5;

/// 显存取哪一份数据
enum class VramSource {
    None,      ///< 驱动没报任何显存信息 (如 Linux 上的 Intel/virtio)
    Dedicated, ///< 独立显存 (独显)
    Shared,    ///< 共享显存 (核显/统一内存 GPU, 如 Apple Silicon)
};

const char* vramSourceName(VramSource source) {
    switch (source) {
        case VramSource::Dedicated:
            return "dedicated";
        case VramSource::Shared:
            return "shared";
        default:
            return "none";
    }
}

/// 可用量 = 总量 - 已用; 已用可能因驱动取整/统计口径略大于总量, 此时按 0 处理
uint64_t availableOf(uint64_t totalMB, uint64_t usedMB) {
    return usedMB < totalMB ? totalMB - usedMB : 0;
}

/// 一张卡对外报的显存三元组 (总量/已用/可用)
struct VramView {
    VramSource source      = VramSource::None;
    uint64_t   totalMB     = 0;
    uint64_t   usedMB      = 0;
    uint64_t   availableMB = 0;
};

/// 选一张卡的显存: 有独立显存就用独立显存; 统一内存 GPU 与核显没有独立显存,
/// 退回共享显存; 两者都没有时 source = None (总量/可用量为 0)
/// - 工具文本与能力载荷都经这里取值, 两处显示不会出现口径差异
VramView vramOf(const GpuInfo& gpu) {
    VramView view;
    if (gpu.dedicatedVramMB > 0) {
        view.source  = VramSource::Dedicated;
        view.totalMB = gpu.dedicatedVramMB;
        view.usedMB  = gpu.dedicatedVramUsedMB;
    } else if (gpu.sharedVramMB > 0) {
        view.source  = VramSource::Shared;
        view.totalMB = gpu.sharedVramMB;
        view.usedMB  = gpu.sharedVramUsedMB;
    }
    view.availableMB = availableOf(view.totalMB, view.usedMB);
    return view;
}

/// 可用内存 MB: 优先用平台直接报的可用量; 平台未提供 (0) 时按"总量 - 已用"兜底
uint64_t availableMemoryMB(const MemoryInfo& memory) {
    if (memory.availablePhysicalMB > 0 || memory.usedPhysicalMB == 0) {
        return memory.availablePhysicalMB;
    }
    return availableOf(memory.totalPhysicalMB, memory.usedPhysicalMB);
}

/// 系统负担快照 (工具文本、能力载荷、周期事件共用同一份数据)
///
/// JSON 字段 (占用为百分比 0~100, 容量为 MB):
/// - `cpu` / `cpu_cores`: 整机 CPU 占用与其逻辑核心数
/// - `mem_total_mb` / `mem_used_mb` / `mem_available_mb` / `mem_percent`: 物理内存
/// - `gpu_count` / `gpu_peak_percent`: GPU 数量与各卡占用的最大值
/// - `vram_total_mb` / `vram_used_mb` / `vram_available_mb`: 各卡显存合计 (取值规则同单卡)
/// - `gpus[]`: 逐卡明细
///   - `name` / `usage_percent`: 名称与 GPU 占用
///   - `vram_source`: `dedicated` / `shared` / `none`, 说明下面的 vram_* 取自哪份数据
///   - `vram_total_mb` / `vram_used_mb` / `vram_available_mb`: 本卡显存 (可用量与总量)
///   - `dedicated_vram_mb` / `dedicated_vram_used_mb` / `dedicated_vram_available_mb`
///   - `shared_vram_mb` / `shared_vram_used_mb` / `shared_vram_available_mb`
/// - 驱动不上报已用显存时按 0 处理, 此时可用量等于总量
std::string usageToJson(const CpuGpuUsage& u) {
    utilxx_base::Json j;
    j["cpu"]              = u.cpuUsagePercent;
    j["cpu_cores"]        = u.cpuCoreCount;
    j["mem_total_mb"]     = u.memory.totalPhysicalMB;
    j["mem_used_mb"]      = u.memory.usedPhysicalMB;
    j["mem_available_mb"] = availableMemoryMB(u.memory);
    j["mem_percent"]      = u.memory.usagePercent;

    utilxx_base::Json gpus         = utilxx_base::Json::array();
    uint64_t          vramTotalSum = 0;
    uint64_t          vramUsedSum  = 0;
    uint64_t          vramAvailSum = 0;
    double            gpuPeak      = 0.0;
    for (const auto& g : u.gpus) {
        const VramView vram = vramOf(g);
        gpus.push_back({
            {"name",                        g.name                                        },
            {"usage_percent",               g.usagePercent                                },
            {"vram_source",                 vramSourceName(vram.source)                   },
            {"vram_total_mb",               vram.totalMB                                  },
            {"vram_used_mb",                vram.usedMB                                   },
            {"vram_available_mb",           vram.availableMB                              },
            {"dedicated_vram_mb",           g.dedicatedVramMB                             },
            {"dedicated_vram_used_mb",      g.dedicatedVramUsedMB                         },
            {"dedicated_vram_available_mb",
             availableOf(g.dedicatedVramMB, g.dedicatedVramUsedMB)                       },
            {"shared_vram_mb",              g.sharedVramMB                                },
            {"shared_vram_used_mb",         g.sharedVramUsedMB                            },
            {"shared_vram_available_mb",    availableOf(g.sharedVramMB, g.sharedVramUsedMB)},
        });
        vramTotalSum += vram.totalMB;
        vramUsedSum += vram.usedMB;
        vramAvailSum += vram.availableMB;
        if (g.usagePercent > gpuPeak) {
            gpuPeak = g.usagePercent;
        }
    }
    j["gpus"]              = std::move(gpus);
    j["gpu_count"]         = u.gpus.size();
    j["gpu_peak_percent"]  = gpuPeak;
    j["vram_total_mb"]     = vramTotalSum;
    j["vram_used_mb"]      = vramUsedSum;
    j["vram_available_mb"] = vramAvailSum;
    return j.dump();
}

std::string formatUsageText(const CpuGpuUsage& usage) {
    std::stringstream ss;
    std::string       cpuLine = fmt::format("CPU Usage: {:.1f}%", usage.cpuUsagePercent);
    if (usage.cpuCoreCount > 0) {
        cpuLine += fmt::format(" ({} cores)", usage.cpuCoreCount);
    }
    ss << cpuLine << "\n";
    const auto mbToBytes = [](uint64_t mb) {
        return mb * 1024 * 1024;
    };
    ss << fmt::format(
        "Memory: {:.1f}% · Used: {} / Total: {} · Available: {}\n",
        usage.memory.usagePercent,
        utilxx_base::formatSize(mbToBytes(usage.memory.usedPhysicalMB)),
        utilxx_base::formatSize(mbToBytes(usage.memory.totalPhysicalMB)),
        utilxx_base::formatSize(mbToBytes(availableMemoryMB(usage.memory)))
    );
    for (size_t i = 0; i < usage.gpus.size(); ++i) {
        const auto&    gpu   = usage.gpus[i];
        const VramView vram  = vramOf(gpu);
        std::string    title = fmt::format("GPU {}", i);
        if (!gpu.name.empty()) {
            title += fmt::format(" [{}]", gpu.name);
        }
        ss << fmt::format("{}: GPU Usage: {:.1f}%", title, gpu.usagePercent);
        if (vram.source != VramSource::None) {
            /// 统一内存 GPU (如 Apple Silicon) 没有独立显存, 标出来避免被读成"独显很小"
            ss << fmt::format(
                ", {}: {}MB Used / {}MB Total / {}MB Available",
                vram.source == VramSource::Shared ? "VRAM (shared)" : "VRAM",
                vram.usedMB,
                vram.totalMB,
                vram.availableMB
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

    /// 同实例查询串行化。
    ///
    /// 后台采样任务（offload 到主程序阻塞池）、`client_attached` 的一次性采样、
    /// 工具执行与能力调用可能同时发起查询，而 `CpuGpuMonitor` 的 CPU 采样基线
    /// 与 GPU 枚举缓存是实例内可变状态：并发访问会读写同一份数据（TSan 定向
    /// 回归可复现）。
    /// - 锁是**实例成员**，不跨实例共享（多实例契约）；
    /// - 查询只使用调用线程自己创建的 io_context 与文件 IO，不回调主程序 IO 线程，
    ///   因此"阻塞池线程持锁 + 另一线程等待"不会与主程序 IO 线程形成锁反转；
    /// - 阻塞池本就被查询同步占用（`querySync` 内 `io.run()`），串行化不改变
    ///   现有线程占用模型，只把并发查询变成排队。
    std::mutex queryMutex;

    CpuGpuUsage querySync() {
        std::lock_guard<std::mutex> lock(queryMutex);
        asio::io_context            io;
        CpuGpuUsage                 usage;
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

/// 注册事务 (start 的实际内容); 失败由主程序按拒绝处理并回滚。
static int32_t sysMonSetup(SysMonCtx& ctx) {
    // 1. 工具
    auto schema = ctx.schema("agentxx_get_system_core_info").build();
    blocking_tool(
        ctx,
        "agentxx_get_system_core_info",
        "Get system resource usage: CPU utilization and core count, memory usage, GPU utilization, and GPU memory usage.",
        schema,
        [](SysMonCtx& c, std::string_view) -> std::string {
            auto usage = c.querySync();
            return formatUsageText(usage);
        }
    );

    // 2. 能力: agentxx.system_usage —— 读取当前系统负担 (cpu/gpu 占用、内存与显存占用)
    //    - 其他插件经 `agentxx.agent.capabilities` 的 invoke (kit: invoke_cap /
    //      invoke_capability_blocking) 调用; 主程序/FFI 侧经
    //      PluginManager::invokeCapabilityAsync 调用 (须携带调用方插件实例租约)
    //    - `method` 不参与分支 (约定 "query"), 参数忽略: 每次都读一份当前快照
    //    - 载荷字段见 [usageToJson] (内存与显存都给出可用量与总量)
    //    - 查询要读系统/GPU 计数器 (约 100ms 以上), 交给主程序阻塞池执行,
    //      异步返回: 调用方可以随时调用, 不会占住主程序 IO 线程
    capability(
        ctx,
        "agentxx.system_usage",
        [](SysMonCtx& c, const PluginxxHost*, std::string_view, std::string_view)
            -> Task<std::string> {
            auto usage = co_await offload(c, [&c](const PluginxxCancelToken*) {
                return c.querySync();
            });
            co_return usageToJson(usage);
        }
    );

    // 3. 事件订阅
    if (ctx.iface.events && ctx.iface.events->subscribe) {
        auto t1 = PluginStringView::fromCstr("client.agentxx_system_monitor.usage_enabled");
        ctx.iface.events->subscribe(
            ctx.host,
            &t1,
            [](const PluginxxStringView* event_json, void* ud) {
                auto* c = static_cast<SysMonCtx*>(ud);
                if (!c) {
                    return;
                }
                try {
                    std::string s(
                        event_json && event_json->data ? event_json->data : "{}",
                        event_json ? static_cast<size_t>(event_json->size) : 0
                    );
                    auto j = utilxx_base::Json::parse(s);
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
            [](const PluginxxStringView*, void* ud) {
                auto* c = static_cast<SysMonCtx*>(ud);
                if (!c || !c->usageEnabled.load(std::memory_order_relaxed)) {
                    return;
                }
                if (!c->iface.scheduler || !c->iface.scheduler->offload) {
                    return;
                }
                c->iface.scheduler->offload(
                    c->host,
                    [](void* ud, const PluginxxCancelToken*, PluginxxString*) -> void* {
                        auto* c = static_cast<SysMonCtx*>(ud);
                        return new CpuGpuUsage(c->querySync());
                    },
                    [](void* ud, int32_t status, void* res, const PluginxxStringView*) {
                        auto* c = static_cast<SysMonCtx*>(ud);
                        if (status == PLUGINXX_OPERATOR_OK && res && c && c->host && c->iface.events
                            && c->iface.events->publish) {
                            auto*       u    = static_cast<CpuGpuUsage*>(res);
                            std::string json = usageToJson(*u);
                            auto        topicSv
                                = PluginStringView::fromCstr("agentxx_system_monitor.usage");
                            auto jsonSv = PluginStringView::from(json.data(), json.size());
                            c->iface.events->publish(c->host, &topicSv, &jsonSv);
                            delete u;
                        } else {
                            delete static_cast<CpuGpuUsage*>(res);
                        }
                    },
                    c,
                    nullptr
                );
            },
            &ctx
        );
    }

    // 4. 后台采样任务
    ctx.spawn([](SysMonCtx& c, OpCtl ctl) -> Task<void> {
        while (!ctl.cancelled()) {
            if (c.usageEnabled.load(std::memory_order_relaxed)) {
                auto usage = co_await offload(c, [&](const PluginxxCancelToken*) {
                    return c.querySync();
                });
                if (ctl.cancelled()) {
                    break;
                }

                std::string json    = usageToJson(usage);
                auto        topicSv = PluginStringView::fromCstr("agentxx_system_monitor.usage");
                auto        jsonSv  = PluginStringView::from(json.data(), json.size());
                if (c.iface.events && c.iface.events->publish) {
                    c.iface.events->publish(c.host, &topicSv, &jsonSv);
                }
            }
            co_await sleep(c, kUsageIntervalSec * 1000);
        }
    });

    return 0;
}

static void*
    sysMonStart(SysMonCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString* error) {
    if (!notify) {
        if (error) {
            PluginString::set(ctx.host, error, "agentxx_system_monitor start: notify required");
        }
        return nullptr;
    }
    if (sysMonSetup(ctx) != 0) {
        if (error) {
            PluginString::set(ctx.host, error, "agentxx_system_monitor start: registration failed");
        }
        return nullptr;
    }
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

static void* sysMonStop(SysMonCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString*) {
    ctx.stopSpawns();
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_EXPORT(
    SysMonCtx,
    "agentxx_system_monitor",
    "1.2.0",
    "System resource monitor: CPU/memory/GPU usage tool; capability agentxx.system_usage "
    "returns the current system load (usage + memory/vram total and available)",
    sysMonStart,
    sysMonStop
);

struct SysMonClientCtx : public ClientPluginBase {
    AgentxxInfoSection* section = nullptr;
    std::atomic<bool>   usage_enabled{true};
    std::string         last_usage_json;
};

struct UsageStat {
    double  cpu        = 0.0;
    int64_t cpuCores   = 0;
    double  memPct     = 0.0;
    int64_t memUsedMb  = 0;
    int64_t memTotalMb = 0;
    size_t  gpuCount   = 0;
    double  gpuPeakPct = 0.0;
};

static UsageStat parseUsage(const std::string& raw) {
    UsageStat st;
    try {
        auto j        = utilxx_base::Json::parse(raw);
        st.cpu        = j.value("cpu", 0.0);
        st.cpuCores   = j.value<int64_t>("cpu_cores", 0);
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
    // 组件项经描述层与扩展 kit 组装 (同一份描述, 终端与将来的 GUI 客户端共用;
    // 每行是一个 Text, 语义色用 tone)
    std::vector<pluginxx::ui::Item> items;
    auto                            pushText = [&](const std::string& text, std::string tone = {}) {
        utilxx_base::Json params;
        params["text"] = text;
        if (!tone.empty()) {
            params["tone"] = std::move(tone);
        }
        items.push_back(agentxx::ui::kit::text(params));
    };
    pushText(fmt::format("|- CPU {:.0f}% · {}x", st.cpu, st.cpuCores));
    std::string ram = fmt::format("|- RAM {:.0f}%", st.memPct);
    if (st.memTotalMb > 0) {
        const auto mbToBytes = [](int64_t mb) {
            return static_cast<uint64_t>(mb) * 1024 * 1024;
        };
        ram = fmt::format(
            "{} · {}/{}",
            ram,
            utilxx_base::formatSize(mbToBytes(st.memUsedMb), 1024, false),
            utilxx_base::formatSize(mbToBytes(st.memTotalMb), 1024, false)
        );
    }
    pushText(ram);
    if (st.gpuCount == 1) {
        pushText(fmt::format("|- GPU {:.0f}%", st.gpuPeakPct));
    } else if (st.gpuCount > 1) {
        pushText(fmt::format("|- GPU {:.0f}% · {}x", st.gpuPeakPct, st.gpuCount));
    }
    utilxx_base::Json out;
    out["items"] = pluginxx::ui::dumpBlocks(items);
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
        utilxx_base::Json off;
        off["items"] = utilxx_base::Json::array();
        json         = off.dump();
    }
    auto jsonSv = PluginStringView::from(json.data(), json.size());
    ctx.iface.ui->update_info_section(ctx.host, ctx.section, &jsonSv);
}

/// client 侧注册事务 (start 的实际内容); 无 UI 能力时静默降级 (返回 0)。
static int32_t sysMonClientSetup(SysMonClientCtx& ctx) {
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
            [](const PluginxxStringView* payload_json, void* ud) {
                auto* ctx = static_cast<SysMonClientCtx*>(ud);
                if (!ctx || PluginStringView::empty(payload_json)) {
                    return;
                }
                try {
                    auto j = utilxx_base::Json::parse(
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
            [](void* ud, const PluginxxStringView*, PluginxxString* actionOut, PluginxxString*)
                -> int32_t {
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
                    auto        paySv   = PluginStringView::from(payload.data(), payload.size());
                    ctx->iface.wire->send_plugin_data(ctx->host, &evtSv, &paySv);
                }
                std::string text = next ? "System resource info: ON" : "System resource info: OFF";
                std::string stateStr = ctx->clientState();
                if (!stateStr.empty() && stateStr != "{}") {
                    try {
                        auto st = utilxx_base::Json::parse(stateStr);
                        if (st.contains("agentPlugins") && st["agentPlugins"].is_array()) {
                            bool found = false;
                            for (const auto& v : st["agentPlugins"]) {
                                if (v.is_object()
                                    && v.value("name", std::string{}) == "agentxx_system_monitor") {
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
                utilxx_base::Json out;
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

static void* sysMonClientStart(
    SysMonClientCtx&              ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error
) {
    if (!notify) {
        if (error) {
            PluginString::set(
                ctx.host,
                error,
                "agentxx_system_monitor client start: notify required"
            );
        }
        return nullptr;
    }
    if (sysMonClientSetup(ctx) != 0) {
        if (error) {
            PluginString::set(
                ctx.host,
                error,
                "agentxx_system_monitor client start: registration failed"
            );
        }
        return nullptr;
    }
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

static void*
    sysMonClientStop(SysMonClientCtx&, const PluginxxOperatorNotify* notify, PluginxxString*) {
    // UI 注册与订阅由主程序在 stop 后统一撤销。
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_CLIENT_EXPORT(
    SysMonClientCtx,
    "agentxx_system_monitor",
    "1.2.0",
    "System resource usage: Info section (CPU/RAM/GPU), /sysinfo toggle",
    sysMonClientStart,
    sysMonClientStop
);
