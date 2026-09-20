#pragma once

#include "fmt/format.h"
#include "utilxx_base/env.h"
#include "utilxx_base/json.h"
#include "utilxx_base/system.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace agentxx {
namespace bench {

struct BenchResult {
    std::string name;
    size_t      iterations;
    double      total_ns;
    double      mean_ns;
    double      min_ns;
    double      max_ns;
    double      stddev_ns;
    double      median_ns;
};

// ---------------------------------------------------------------------------
// 内存采样数据结构 (采样实现见 bench_mem_probe.h, 逻辑内存统计见 bench_mem_logical.h)
// ---------------------------------------------------------------------------

/// 进程内存细项 (Linux 从 /proc 读取; Windows 取可获取的部分)
struct ProcMemDetail {
    double   rssMB     = 0.0; ///< 常驻物理内存 (VmRSS / WorkingSet)
    double   pssMB     = 0.0; ///< 按共享比例分摊后的常驻 (仅 Linux; 0 = 未取到)
    double   privateMB = 0.0; ///< 私有内存 (Linux: RssAnon + RssShmem; Windows: PrivateUsage)
    double   privateDirtyMB = 0.0; ///< 私有脏页 (最接近"真实占用"; 仅 Linux)
    double   anonMB         = 0.0; ///< 匿名映射常驻 (仅 Linux)
    double   fileMB         = 0.0; ///< 文件映射常驻 (仅 Linux)
    double   shmemMB        = 0.0; ///< 共享内存常驻 (仅 Linux)
    double   swapMB         = 0.0; ///< 换出量 (仅 Linux)
    double   peakRssMB      = 0.0; ///< 峰值常驻 (VmHWM / PeakWorkingSetSize)
    double   peakVmMB       = 0.0; ///< 峰值虚拟内存 (VmPeak)
    double   vmsizeMB       = 0.0; ///< 当前虚拟内存
    uint64_t threads        = 0;   ///< 线程数
    uint64_t fds            = 0;   ///< 打开的文件描述符 / 句柄数

    double heapInUseMB = 0.0; ///< glibc 堆已分配字节 (mallinfo2.uordblks)
    double heapFreeMB  = 0.0; ///< 空闲但保留在堆内的字节 (fordblks)
    double heapArenaMB = 0.0; ///< 堆向系统申请的总量 (arena)
    double heapMmapMB  = 0.0; ///< mmap 方式分配的大块 (hblkhd)
    bool   heapValid   = false;

    bool valid = false;
};

/// 映射归属类别 (smaps 模块分解用)
enum class MemRegionKind {
    Exe,        ///< 基准测试可执行文件自身 (静态链接的库代码也在其中)
    ProjectLib, ///< 本项目动态库 (libagentxx / libcxx_utilxx / libcxx_pluginxx)
    PluginLib,  ///< 插件动态库 (逐个插件单独一行)
    SystemLib,  ///< 系统库 (libc/libstdc++/libgcc/...)
    DataFile,   ///< 其他文件映射 (sqlite 库文件 / 字体 / 数据文件)
    Heap,       ///< [heap]
    Stack,      ///< [stack] / 线程栈
    Vdso,       ///< [vdso] / [vvar] / [vsyscall]
    Shm,        ///< /dev/shm、memfd 等
    Anon,       ///< 无路径匿名映射
    Other,      ///< 其他
};

/// 模块分解中的一行 (同名同类别段已合并)
struct ModuleMemRow {
    std::string   name; ///< 显示名 (插件名 / 库文件名 / "[heap]" 等)
    std::string   path; ///< 映射路径 (匿名段为空)
    MemRegionKind kind           = MemRegionKind::Other;
    double        sizeMB         = 0.0;
    double        rssMB          = 0.0;
    double        pssMB          = 0.0;
    double        privateDirtyMB = 0.0;
    double        privateCleanMB = 0.0;
    double        sharedMB       = 0.0;
    double        anonMB         = 0.0;
    size_t        regions        = 0;
};

struct ModuleMemBreakdown {
    std::vector<ModuleMemRow> rows; ///< 按 rss 降序
    double                    totalRssMB = 0.0;
    double                    totalPssMB = 0.0;
    bool                      valid      = false;
    std::string               note;
};

/// 逻辑内存行 (数据结构自身字节数; 见 bench_mem_logical.h)
struct LogicalMemRow {
    std::string name;
    size_t      bytes = 0;
    size_t      count = 0;
    std::string note;
};

/// 分阶段采样点
struct MemPhaseSample {
    std::string   label;
    ProcMemDetail mem;
    double        deltaRssMB       = 0.0;
    double        deltaPssMB       = 0.0;
    double        deltaHeapInUseMB = 0.0;
    double        deltaAnonMB      = 0.0;
    double        deltaPrivateMB   = 0.0;
    double        elapsedMs        = 0.0;
    std::string   note;
};

/// CPU 采样窗口结果 (含用户态/内核态时间细分)
struct CpuDelta {
    double busyPct = -1.0; ///< 窗口内平均 CPU 利用率 (占单核百分比)
    double userMs  = 0.0;
    double sysMs   = 0.0;
    double wallMs  = 0.0;
};

/// 资源占用基准测试结果 (内存、CPU、容器大小、Token 等)
struct ResourceResult {
    std::string mode;                 ///< 模式: cli | tui | split_cli | split_tui | ...
    std::string side;                 ///< 侧: self | server | client
    std::string point;                ///< 采样时机: startup | ctx100k | ctx200k
    double      rssMB         = 0.0;  ///< 常驻物理内存 (MB)
    double      privateMB     = 0.0;  ///< 私有内存 (MB)
    double      cpuIdlePct    = -1.0; ///< 空闲期 CPU 占用百分比 (负数表示未测)
    double      cpuBusyPct    = -1.0; ///< 繁忙/生成期 CPU 占用百分比 (负数表示未测)
    size_t      viewCount     = 0;    ///< view 消息条数
    size_t      viewBytes     = 0;    ///< view 消息近似字节数
    size_t      llmCount      = 0;    ///< llm 消息条数
    size_t      llmBytes      = 0;    ///< llm 消息近似字节数
    size_t      tokens        = 0;    ///< 估算 token 数量
    size_t      pluginsAgent  = 0;    ///< agent 侧成功加载插件数
    size_t      pluginsClient = 0;    ///< client 侧成功加载插件数
    std::string note;                 ///< 备注 (headless/允差/降级说明等)

    // ---- 扩展指标 (资源基准测试 v2) ----
    ProcMemDetail             mem; ///< 完整内存细项 (含 PSS/堆/峰值/线程/fd)
    CpuDelta                  cpu; ///< CPU 细分 (用户态/内核态)
    double                    trimReclaimableMB = -1.0; ///< malloc_trim 可回收 (MB; -1 = 未测)
    double                    heapFragmentPct   = -1.0; ///< 堆碎片率 (空闲保留 / 堆总量)
    uint64_t                  frames            = 0;    ///< 渲染帧数 (TUI 场景)
    double                    frameAvgMs        = -1.0; ///< 平均帧耗时 (TUI 场景)
    double                    renderBytes       = 0.0;  ///< 渲染输出字节 (TUI 场景)
    std::vector<ModuleMemRow> modules;                  ///< 模块级内存分解 (可选)
    std::vector<LogicalMemRow> logical;                 ///< 逻辑内存 (可选)
    std::vector<MemPhaseSample> phases; ///< 分阶段采样 (可选; 挂在每个 mode+side 的首个点)
};

inline const char* memRegionKindName(MemRegionKind kind) {
    switch (kind) {
        case MemRegionKind::Exe:
            return "exe";
        case MemRegionKind::ProjectLib:
            return "project-lib";
        case MemRegionKind::PluginLib:
            return "plugin-lib";
        case MemRegionKind::SystemLib:
            return "system-lib";
        case MemRegionKind::DataFile:
            return "data-file";
        case MemRegionKind::Heap:
            return "heap";
        case MemRegionKind::Stack:
            return "stack";
        case MemRegionKind::Vdso:
            return "vdso";
        case MemRegionKind::Shm:
            return "shm";
        case MemRegionKind::Anon:
            return "anon";
        default:
            return "other";
    }
}

inline std::string fmtNs(double ns) {
    if (ns < 1000.0) {
        return fmt::format("{:.1f} ns", ns);
    } else if (ns < 1'000'000.0) {
        return fmt::format("{:.2f} us", ns / 1000.0);
    } else if (ns < 1'000'000'000.0) {
        return fmt::format("{:.2f} ms", ns / 1'000'000.0);
    } else {
        return fmt::format("{:.3f} s", ns / 1'000'000'000.0);
    }
}

inline void printResult(const BenchResult& r) {
    std::cout << "  [" << r.name << "]\n"
              << "    iterations : " << r.iterations << "\n"
              << "    total      : " << fmtNs(r.total_ns) << "\n"
              << "    mean       : " << fmtNs(r.mean_ns) << "\n"
              << "    median     : " << fmtNs(r.median_ns) << "\n"
              << "    min        : " << fmtNs(r.min_ns) << "\n"
              << "    max        : " << fmtNs(r.max_ns) << "\n"
              << "    stddev     : " << fmtNs(r.stddev_ns) << "\n";
}

/// 打印单条资源结果 (一行摘要 + 内存细项两行)
/// - os 默认 stdout; 真实 TUI 场景期间 fd 1 归 TUI 使用, 可传入延迟输出流
inline void printResourceResult(const ResourceResult& r, std::ostream& os = std::cout) {
    os << fmt::format(
        "  [{:<9}][{:<6}][{:<7}] rss={:>6.2f}MB priv={:>6.2f}MB cpuIdle={:>5.1f}% cpuBusy={:>5.1f}% "
        "view={}/{}KB llm={}/{}KB tok={:<6} plugins={}/{} note={}\n",
        r.mode,
        r.side,
        r.point,
        r.rssMB,
        r.privateMB,
        r.cpuIdlePct >= 0 ? r.cpuIdlePct : 0.0,
        r.cpuBusyPct >= 0 ? r.cpuBusyPct : 0.0,
        r.viewCount,
        r.viewBytes / 1024,
        r.llmCount,
        r.llmBytes / 1024,
        r.tokens,
        r.pluginsAgent,
        r.pluginsClient,
        r.note
    );
    if (r.mem.valid) {
        // 堆细项 (mallinfo2) 只能查询自身进程: 子进程采样显示 n/a, 避免误读为 0
        std::string heapText = r.mem.heapValid
                                   ? fmt::format(
                                         "堆在用={:.2f} 堆空闲={:.2f}(碎片{:.1f}%)",
                                         r.mem.heapInUseMB,
                                         r.mem.heapFreeMB,
                                         r.heapFragmentPct >= 0 ? r.heapFragmentPct : 0.0
                                     )
                                   : std::string{"堆=n/a(子进程)"};
        os << fmt::format(
            "              pss={:.2f} 私脏={:.2f} 匿名={:.2f} 文件={:.2f} 峰值={:.2f} {} "
            "虚拟={:.1f} 线程={} fd={} 可回收={}\n",
            r.mem.pssMB,
            r.mem.privateDirtyMB,
            r.mem.anonMB,
            r.mem.fileMB,
            r.mem.peakRssMB,
            heapText,
            r.mem.vmsizeMB,
            r.mem.threads,
            r.mem.fds,
            r.trimReclaimableMB >= 0 ? fmt::format("{:.2f}MB", r.trimReclaimableMB) : "n/a"
        );
    }
    if (r.cpu.userMs > 0.0 || r.cpu.sysMs > 0.0) {
        os << fmt::format(
            "              cpu: user={:.0f}ms sys={:.0f}ms wall={:.0f}ms\n",
            r.cpu.userMs,
            r.cpu.sysMs,
            r.cpu.wallMs
        );
    }
}

/// 运行环境信息 (写入报告头部, 便于跨机器/跨版本对比)
struct HostInfo {
    std::string system;      ///< 系统名 (utilxx_base::getSystemName)
    std::string exePath;     ///< 被测可执行文件路径
    std::string version;     ///< 版本号
    std::string buildConfig; ///< 构建配置 (Debug/Release 等)
    uint32_t    cpuCores   = 0;
    double      memTotalMB = 0.0;
};

namespace detail {

inline double jsonNumber(const utilxx_base::Json& j, std::string_view key, double def = 0.0) {
    if (!j.is_object() || !j.contains(key)) {
        return def;
    }
    const auto& v = j[key];
    if (v.is_number_integer()) {
        return static_cast<double>(v.get<long long>());
    }
    if (v.is_number()) {
        return v.get<double>();
    }
    return def;
}

inline std::string
    jsonText(const utilxx_base::Json& j, std::string_view key, std::string def = {}) {
    if (!j.is_object() || !j.contains(key)) {
        return def;
    }
    const auto& v = j[key];
    return v.is_string() ? std::string{v.get<std::string_view>()} : def;
}

inline bool jsonFlag(const utilxx_base::Json& j, std::string_view key, bool def = false) {
    if (!j.is_object() || !j.contains(key)) {
        return def;
    }
    const auto& v = j[key];
    return v.is_bool() ? v.get<bool>() : def;
}

/// 数值列定义 (统一用于 markdown 表与基线对比)
template<typename Fn>
struct MetricColumn {
    const char* name;
    Fn          get;
};

} // namespace detail

/// 是否运行在"聚合运行的子进程场景"模式 (由父进程设置 AGENTXX_BENCH_CHILD=1):
/// 此时抑制对比摘要与报告落盘的提示输出, 避免与父进程输出交错
inline bool benchChildMode() {
    return utilxx_base::ApplicationEnv::instance().has("AGENTXX_BENCH_CHILD");
}

/// 类别名 -> 枚举 (解析报告 JSON 的 `kind` 字段时使用)
inline MemRegionKind memRegionKindFromName(std::string_view name) {
    if (name == "exe") {
        return MemRegionKind::Exe;
    }
    if (name == "project-lib") {
        return MemRegionKind::ProjectLib;
    }
    if (name == "plugin-lib") {
        return MemRegionKind::PluginLib;
    }
    if (name == "system-lib") {
        return MemRegionKind::SystemLib;
    }
    if (name == "data-file") {
        return MemRegionKind::DataFile;
    }
    if (name == "heap") {
        return MemRegionKind::Heap;
    }
    if (name == "stack") {
        return MemRegionKind::Stack;
    }
    if (name == "vdso") {
        return MemRegionKind::Vdso;
    }
    if (name == "shm") {
        return MemRegionKind::Shm;
    }
    if (name == "anon") {
        return MemRegionKind::Anon;
    }
    return MemRegionKind::Other;
}

/// 从报告 JSON 文本解析资源结果 (JSON 由 BenchReporter 生成)
/// - 用于: 基线对比 (--baseline) 与聚合运行时合并各场景子进程的结果
/// - `return` 解析出的采样点数量 (0 表示无 resource 段或格式不匹配)
inline size_t
    parseResourceResultsFromJson(std::string_view jsonText, std::vector<ResourceResult>& out) {
    try {
        auto j = utilxx_base::Json::parse(jsonText);
        if (!j.is_object() || !j.contains("resource") || !j["resource"].is_array()) {
            return 0;
        }
        const auto& arr    = j["resource"];
        size_t      before = out.size();
        for (size_t i = 0; i < arr.size(); ++i) {
            const auto&    item = arr[i];
            ResourceResult r;
            r.mode  = detail::jsonText(item, "mode");
            r.side  = detail::jsonText(item, "side");
            r.point = detail::jsonText(item, "point");
            if (r.mode.empty() || r.point.empty()) {
                continue;
            }
            r.note          = detail::jsonText(item, "note");
            r.rssMB         = detail::jsonNumber(item, "rssMB", 0.0);
            r.privateMB     = detail::jsonNumber(item, "privateMB");
            r.cpuIdlePct    = detail::jsonNumber(item, "cpuIdlePct", -1.0);
            r.cpuBusyPct    = detail::jsonNumber(item, "cpuBusyPct", -1.0);
            r.viewCount     = static_cast<size_t>(detail::jsonNumber(item, "viewCount"));
            r.viewBytes     = static_cast<size_t>(detail::jsonNumber(item, "viewBytes"));
            r.llmCount      = static_cast<size_t>(detail::jsonNumber(item, "llmCount"));
            r.llmBytes      = static_cast<size_t>(detail::jsonNumber(item, "llmBytes"));
            r.tokens        = static_cast<size_t>(detail::jsonNumber(item, "tokens"));
            r.pluginsAgent  = static_cast<size_t>(detail::jsonNumber(item, "pluginsAgent"));
            r.pluginsClient = static_cast<size_t>(detail::jsonNumber(item, "pluginsClient"));
            r.frames        = static_cast<uint64_t>(detail::jsonNumber(item, "frames"));
            r.frameAvgMs    = detail::jsonNumber(item, "frameAvgMs", -1.0);
            r.renderBytes   = detail::jsonNumber(item, "renderBytes");
            r.mem.rssMB     = r.rssMB;
            r.mem.privateMB = r.privateMB;
            if (item.contains("mem") && item["mem"].is_object()) {
                const auto& m        = item["mem"];
                r.mem.rssMB          = detail::jsonNumber(m, "rssMB", r.rssMB);
                r.mem.pssMB          = detail::jsonNumber(m, "pssMB");
                r.mem.privateMB      = detail::jsonNumber(m, "privateMB", r.privateMB);
                r.mem.privateDirtyMB = detail::jsonNumber(m, "privateDirtyMB");
                r.mem.anonMB         = detail::jsonNumber(m, "anonMB");
                r.mem.fileMB         = detail::jsonNumber(m, "fileMB");
                r.mem.shmemMB        = detail::jsonNumber(m, "shmemMB");
                r.mem.swapMB         = detail::jsonNumber(m, "swapMB");
                r.mem.peakRssMB      = detail::jsonNumber(m, "peakRssMB");
                r.mem.peakVmMB       = detail::jsonNumber(m, "peakVmMB");
                r.mem.vmsizeMB       = detail::jsonNumber(m, "vmsizeMB");
                r.mem.threads        = static_cast<uint64_t>(detail::jsonNumber(m, "threads"));
                r.mem.fds            = static_cast<uint64_t>(detail::jsonNumber(m, "fds"));
                r.mem.heapInUseMB    = detail::jsonNumber(m, "heapInUseMB");
                r.mem.heapFreeMB     = detail::jsonNumber(m, "heapFreeMB");
                r.mem.heapArenaMB    = detail::jsonNumber(m, "heapArenaMB");
                r.mem.heapMmapMB     = detail::jsonNumber(m, "heapMmapMB");
                r.mem.heapValid      = detail::jsonFlag(m, "heapValid");
                r.mem.valid          = true;
            }
            r.trimReclaimableMB = detail::jsonNumber(item, "trimReclaimableMB", -1.0);
            r.heapFragmentPct   = detail::jsonNumber(item, "heapFragmentPct", -1.0);
            if (item.contains("cpu") && item["cpu"].is_object()) {
                const auto& c = item["cpu"];
                r.cpu.busyPct = detail::jsonNumber(c, "busyPct", r.cpuBusyPct);
                r.cpu.userMs  = detail::jsonNumber(c, "userMs");
                r.cpu.sysMs   = detail::jsonNumber(c, "sysMs");
                r.cpu.wallMs  = detail::jsonNumber(c, "wallMs");
            }
            if (item.contains("modules") && item["modules"].is_array()) {
                const auto& mods = item["modules"];
                for (size_t k = 0; k < mods.size(); ++k) {
                    const auto&  m = mods[k];
                    ModuleMemRow row;
                    row.name           = detail::jsonText(m, "name");
                    row.path           = detail::jsonText(m, "path");
                    row.kind           = memRegionKindFromName(detail::jsonText(m, "kind"));
                    row.sizeMB         = detail::jsonNumber(m, "sizeMB");
                    row.rssMB          = detail::jsonNumber(m, "rssMB");
                    row.pssMB          = detail::jsonNumber(m, "pssMB");
                    row.privateDirtyMB = detail::jsonNumber(m, "privateDirtyMB");
                    row.sharedMB       = detail::jsonNumber(m, "sharedMB");
                    row.anonMB         = detail::jsonNumber(m, "anonMB");
                    row.regions        = static_cast<size_t>(detail::jsonNumber(m, "regions"));
                    r.modules.push_back(std::move(row));
                }
            }
            if (item.contains("logical") && item["logical"].is_array()) {
                const auto& logs = item["logical"];
                for (size_t k = 0; k < logs.size(); ++k) {
                    const auto&   l = logs[k];
                    LogicalMemRow row;
                    row.name  = detail::jsonText(l, "name");
                    row.bytes = static_cast<size_t>(detail::jsonNumber(l, "bytes"));
                    row.count = static_cast<size_t>(detail::jsonNumber(l, "count"));
                    row.note  = detail::jsonText(l, "note");
                    r.logical.push_back(std::move(row));
                }
            }
            if (item.contains("phases") && item["phases"].is_array()) {
                const auto& phs = item["phases"];
                for (size_t k = 0; k < phs.size(); ++k) {
                    const auto&    p = phs[k];
                    MemPhaseSample s;
                    s.label              = detail::jsonText(p, "label");
                    s.note               = detail::jsonText(p, "note");
                    s.mem.rssMB          = detail::jsonNumber(p, "rssMB");
                    s.mem.pssMB          = detail::jsonNumber(p, "pssMB");
                    s.mem.privateDirtyMB = detail::jsonNumber(p, "privateDirtyMB");
                    s.mem.heapInUseMB    = detail::jsonNumber(p, "heapInUseMB");
                    s.mem.anonMB         = detail::jsonNumber(p, "anonMB");
                    s.deltaRssMB         = detail::jsonNumber(p, "deltaRssMB");
                    s.deltaPssMB         = detail::jsonNumber(p, "deltaPssMB");
                    s.deltaHeapInUseMB   = detail::jsonNumber(p, "deltaHeapInUseMB");
                    s.elapsedMs          = detail::jsonNumber(p, "elapsedMs");
                    r.phases.push_back(std::move(s));
                }
            }
            out.push_back(std::move(r));
        }
        return out.size() - before;
    } catch (const std::exception& e) {
        std::cerr << "[BenchReporter] parse resource results failed: " << e.what() << std::endl;
        return 0;
    }
}

/// 资源结果的身份键 (跨运行对比用): mode|side|point
inline std::string
    resourceKey(std::string_view mode, std::string_view side, std::string_view point) {
    return fmt::format("{}|{}|{}", mode, side, point);
}

inline std::string resourceKey(const ResourceResult& r) {
    return resourceKey(r.mode, r.side, r.point);
}

/// 资源结果的关键指标 (markdown 表 / 对比表共用的列)
inline std::vector<std::pair<std::string, double>> resourceMetrics(const ResourceResult& r) {
    return {
        {"rssMB",           r.rssMB                           },
        {"pssMB",           r.mem.pssMB                       },
        {"privDirtyMB",     r.mem.privateDirtyMB              },
        {"privateMB",       r.privateMB                       },
        {"anonMB",          r.mem.anonMB                      },
        {"peakRssMB",       r.mem.peakRssMB                   },
        {"heapInUseMB",     r.mem.heapInUseMB                 },
        {"heapFreeMB",      r.mem.heapFreeMB                  },
        {"vmsizeMB",        r.mem.vmsizeMB                    },
        {"threads",         static_cast<double>(r.mem.threads)},
        {"fds",             static_cast<double>(r.mem.fds)    },
        {"cpuIdlePct",      r.cpuIdlePct                      },
        {"cpuBusyPct",      r.cpuBusyPct                      },
        {"userMs",          r.cpu.userMs                      },
        {"sysMs",           r.cpu.sysMs                       },
        {"frames",          static_cast<double>(r.frames)     },
        {"frameAvgMs",      r.frameAvgMs                      },
        {"renderBytes",     r.renderBytes                     },
        {"viewCount",       static_cast<double>(r.viewCount)  },
        {"llmCount",        static_cast<double>(r.llmCount)   },
        {"tokens",          static_cast<double>(r.tokens)     },
        {"trimReclaimMB",   r.trimReclaimableMB               },
        {"heapFragmentPct", r.heapFragmentPct                 },
    };
}

class BenchReporter {
public:

    static BenchReporter& instance() {
        static BenchReporter reporter;
        return reporter;
    }

    void setOutputDir(const std::string& dir) {
        outputDir_ = dir;
    }

    /// 子进程场景模式 (只写文件, 不打印摘要/落盘提示)

    const std::string& getOutputDir() const {
        return outputDir_;
    }

    void setHostInfo(HostInfo info) {
        host_ = std::move(info);
    }

    const HostInfo& hostInfo() const {
        return host_;
    }

    void addResult(const BenchResult& r) {
        results_.push_back(r);
    }

    void addResource(const ResourceResult& r) {
        resourceResults_.push_back(r);
    }

    const std::vector<ResourceResult>& getResourceResults() const {
        return resourceResults_;
    }

    /// 把阶段采样表挂到该 mode+side 的第一个结果上 (报告中按 mode+side 展示一次)
    void attachPhases(
        const std::string&                 mode,
        const std::string&                 side,
        const std::vector<MemPhaseSample>& phases
    ) {
        if (phases.empty()) {
            return;
        }
        for (auto& r : resourceResults_) {
            if (r.mode == mode && r.side == side) {
                r.phases = phases;
                return;
            }
        }
    }

    // -----------------------------------------------------------------------
    // 基线对比 (--baseline <bench_*.json>)
    // -----------------------------------------------------------------------

    /// 载入基线报告 (优化前的 bench_*.json), 供本次运行对比
    bool loadBaselineFile(const std::string& path) {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs.is_open()) {
            std::cerr << "[BenchReporter] baseline not found: " << path << std::endl;
            return false;
        }
        std::ostringstream oss;
        oss << ifs.rdbuf();
        std::vector<ResourceResult> parsed;
        if (parseResourceResultsFromJson(oss.str(), parsed) == 0) {
            std::cerr << "[BenchReporter] baseline has no `resource` entries: " << path
                      << std::endl;
            return false;
        }
        baseline_.clear();
        for (auto& r : parsed) {
            baseline_[resourceKey(r)] = std::move(r);
        }
        baselinePath_ = path;
        std::cout << "[BenchReporter] baseline loaded: " << path << " (" << baseline_.size()
                  << " 个采样点)" << std::endl;
        return !baseline_.empty();
    }

    /// 合并另一个报告文件中的资源结果 (聚合运行时合并各场景子进程的结果)
    /// - `return` 合并进来的采样点数量
    size_t mergeResourceResultsFromFile(const std::string& path) {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs.is_open()) {
            std::cerr << "[BenchReporter] merge: file not found: " << path << std::endl;
            return 0;
        }
        std::ostringstream oss;
        oss << ifs.rdbuf();
        std::vector<ResourceResult> parsed;
        size_t                      n = parseResourceResultsFromJson(oss.str(), parsed);
        for (auto& r : parsed) {
            resourceResults_.push_back(std::move(r));
        }
        return n;
    }

    bool hasBaseline() const {
        return !baseline_.empty();
    }

    const std::string& baselinePath() const {
        return baselinePath_;
    }

    // -----------------------------------------------------------------------
    // 控制台对比摘要
    // -----------------------------------------------------------------------

    void printComparisonSummary() const {
        if (resourceResults_.empty() || benchChildMode()) {
            return;
        }
        std::cout << "\n========================================================================"
                     "========\n";
        std::cout << "  资源对比总览 (RSS / PSS / 私有脏页 / 峰值 / 堆在用, 单位 MB)\n";
        std::cout << "========================================================================"
                     "========\n";
        std::cout << fmt::format(
            "  {:<20} {:<8} {:<9} {:>8} {:>8} {:>8} {:>8} {:>8} {:>7} {:>7}\n",
            "模式",
            "侧",
            "采样点",
            "RSS",
            "PSS",
            "私脏",
            "峰值RSS",
            "堆在用",
            "线程",
            "fd"
        );
        for (const auto& r : resourceResults_) {
            std::cout << fmt::format(
                "  {:<20} {:<8} {:<9} {:>8.2f} {:>8.2f} {:>8.2f} {:>8.2f} {:>8.2f} {:>7} {:>7}\n",
                r.mode.size() > 20 ? r.mode.substr(0, 20) : r.mode,
                r.side.size() > 8 ? r.side.substr(0, 8) : r.side,
                r.point.size() > 9 ? r.point.substr(0, 9) : r.point,
                r.rssMB,
                r.mem.pssMB,
                r.mem.privateDirtyMB,
                r.mem.peakRssMB,
                r.mem.heapInUseMB,
                r.mem.threads,
                r.mem.fds
            );
        }

        // 渲染性能 (仅含 TUI 渲染的场景)
        bool hasRender = false;
        for (const auto& r : resourceResults_) {
            if (r.frames > 0 || r.frameAvgMs > 0.0 || r.renderBytes > 0.0) {
                hasRender = true;
                break;
            }
        }
        if (hasRender) {
            std::cout << "\n  ---- 渲染性能 (TUI 场景) ----\n";
            std::cout << fmt::format(
                "  {:<20} {:<8} {:<9} {:>10} {:>12} {:>12} {}\n",
                "模式",
                "侧",
                "采样点",
                "帧数",
                "平均帧(ms)",
                "渲染输出(MB)",
                "备注"
            );
            for (const auto& r : resourceResults_) {
                if (r.frames == 0 && r.frameAvgMs <= 0.0 && r.renderBytes <= 0.0) {
                    continue;
                }
                std::cout << fmt::format(
                    "  {:<20} {:<8} {:<9} {:>10} {:>12.3f} {:>12.2f} {}\n",
                    r.mode.size() > 20 ? r.mode.substr(0, 20) : r.mode,
                    r.side.size() > 8 ? r.side.substr(0, 8) : r.side,
                    r.point.size() > 9 ? r.point.substr(0, 9) : r.point,
                    r.frames,
                    r.frameAvgMs,
                    r.renderBytes / (1024.0 * 1024.0),
                    r.note
                );
            }
        }

        // 逻辑内存 Top (每个 mode+side 只打印一次, 取前 8 项)
        std::set<std::string> printedModes;
        bool                  anyLogical = false;
        for (const auto& r : resourceResults_) {
            if (r.logical.empty()) {
                continue;
            }
            auto key = r.mode + "|" + r.side;
            if (printedModes.count(key) != 0) {
                continue;
            }
            printedModes.insert(key);
            anyLogical                      = true;
            std::vector<LogicalMemRow> rows = r.logical;
            std::sort(rows.begin(), rows.end(), [](const LogicalMemRow& a, const LogicalMemRow& b) {
                return a.bytes > b.bytes;
            });
            std::cout << fmt::format(
                "\n  ---- 逻辑内存 Top ({} / {}, 采样点 {}; 数据结构自身字节) ----\n",
                r.mode,
                r.side,
                r.point
            );
            size_t shown = 0;
            for (const auto& row : rows) {
                if (row.bytes == 0 && row.count == 0) {
                    continue;
                }
                std::cout << fmt::format(
                    "    {:<38} {:>10} 条={:<8} {}\n",
                    row.name.size() > 38 ? row.name.substr(0, 38) : row.name,
                    row.bytes,
                    row.count,
                    row.note
                );
                if (++shown >= 8) {
                    break;
                }
            }
        }
        (void)anyLogical;

        if (baseline_.empty()) {
            std::cout << "\n  (未指定 --baseline, 无历史对比; 可用上次的 bench_*.json 作为基线)\n";
            return;
        }

        std::cout << "\n  ---- 与基线对比 (基线: " << baselinePath_ << ") ----\n";
        std::cout << fmt::format(
            "  {:<20} {:<8} {:<9} {:>10} {:>10} {:>10} {:>10} {:>10}\n",
            "模式",
            "侧",
            "采样点",
            "RSS(基线)",
            "RSS(本次)",
            "ΔRSS",
            "ΔPSS",
            "Δ堆在用"
        );
        for (const auto& r : resourceResults_) {
            auto it = baseline_.find(resourceKey(r));
            if (it == baseline_.end()) {
                continue;
            }
            const auto& b = it->second;
            std::cout << fmt::format(
                "  {:<20} {:<8} {:<9} {:>10.2f} {:>10.2f} {:>+10.2f} {:>+10.2f} {:>+10.2f}\n",
                r.mode.size() > 20 ? r.mode.substr(0, 20) : r.mode,
                r.side.size() > 8 ? r.side.substr(0, 8) : r.side,
                r.point.size() > 9 ? r.point.substr(0, 9) : r.point,
                b.rssMB,
                r.rssMB,
                r.rssMB - b.rssMB,
                r.mem.pssMB - b.mem.pssMB,
                r.mem.heapInUseMB - b.mem.heapInUseMB
            );
        }

        // 模块级差异 (只看变化明显的, 便于定位是哪个模块增长)
        std::cout << "\n  ---- 模块级差异 (|ΔRSS| > 0.5MB, 前 12 项) ----\n";
        std::vector<std::tuple<double, std::string, std::string, double, double>> modDiffs;
        for (const auto& r : resourceResults_) {
            auto it = baseline_.find(resourceKey(r));
            if (it == baseline_.end()) {
                continue;
            }
            std::map<std::string, double> baseMap;
            for (const auto& m : it->second.modules) {
                baseMap[m.name] += m.rssMB;
            }
            std::map<std::string, double> curMap;
            for (const auto& m : r.modules) {
                curMap[m.name] += m.rssMB;
            }
            std::set<std::string> names;
            for (const auto& kv : baseMap) {
                names.insert(kv.first);
            }
            for (const auto& kv : curMap) {
                names.insert(kv.first);
            }
            for (const auto& n : names) {
                double b = baseMap.count(n) ? baseMap[n] : 0.0;
                double c = curMap.count(n) ? curMap[n] : 0.0;
                if (std::abs(c - b) > 0.5) {
                    modDiffs.emplace_back(
                        std::abs(c - b),
                        fmt::format("{}|{}|{}", r.mode, r.side, r.point),
                        n,
                        b,
                        c
                    );
                }
            }
        }
        std::sort(modDiffs.begin(), modDiffs.end(), [](const auto& a, const auto& b) {
            return std::get<0>(a) > std::get<0>(b);
        });
        if (modDiffs.empty()) {
            std::cout << "    (无显著模块级差异)\n";
        }
        for (size_t i = 0; i < modDiffs.size() && i < 12; ++i) {
            const auto& [abs, key, name, b, c] = modDiffs[i];
            std::cout << fmt::format(
                "    {:<28} {:<30} 基线={:>8.2f} 本次={:>8.2f} Δ={:>+8.2f}\n",
                key,
                name.size() > 30 ? name.substr(0, 30) : name,
                b,
                c,
                c - b
            );
        }
    }

    // -----------------------------------------------------------------------
    // 落盘 (JSON + Markdown)
    // -----------------------------------------------------------------------

    void flushToFile() const {
        if (outputDir_.empty()) {
            std::cerr << "[BenchReporter] output dir not set, skip writing file" << std::endl;
            return;
        }

        namespace fs = std::filesystem;
        fs::path dir(outputDir_);
        if (!fs::exists(dir)) {
            std::error_code ec;
            fs::create_directories(dir, ec);
            if (ec) {
                std::cerr << "[BenchReporter] failed to create dir: " << dir
                          << ", error: " << ec.message() << std::endl;
                return;
            }
        }

        auto now = std::chrono::system_clock::now();
#if XX_IS_WIN_D || defined(_LIBCPP_VERSION)
        // Windows/MinGW 与 libc++ 的 zoned_time 缺失，回退为本地 tm
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm     tm{};
#if XX_IS_WIN_D
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
        std::string timestamp(buf);
#else
        std::chrono::zoned_time local_time{std::chrono::current_zone(), now};
        std::string             timestamp = std::format("{:%Y%m%d_%H%M%S}", local_time);
#endif

        fs::path jsonPath = dir / fmt::format("bench_{}.json", timestamp);
        fs::path mdPath   = dir / fmt::format("bench_{}.md", timestamp);

        {
            std::ofstream ofs(jsonPath, std::ios::out | std::ios::trunc);
            if (!ofs.is_open()) {
                std::cerr << "[BenchReporter] failed to open file: " << jsonPath << std::endl;
                return;
            }
            ofs << buildJson(timestamp);
            ofs.close();
        }
        {
            std::ofstream ofs(mdPath, std::ios::out | std::ios::trunc);
            if (ofs.is_open()) {
                ofs << buildMarkdown(timestamp);
                ofs.close();
                if (!benchChildMode()) {
                    std::cout << "\n[BenchReporter] 可读报告: " << mdPath << std::endl;
                }
            }
        }

        if (!benchChildMode()) {
            std::cout << "\n[BenchReporter] results written to: " << jsonPath << std::endl;
        }
    }

private:

    BenchReporter() = default;

    std::string buildJson(const std::string& timestamp) const {
        std::ostringstream ofs;
        ofs << "{\n";
        ofs << fmt::format("  \"timestamp\": \"{}\",\n", timestamp);
        ofs << fmt::format("  \"version\": \"{}\",\n", host_.version);
        ofs << fmt::format("  \"build\": \"{}\",\n", host_.buildConfig);
        ofs << "  \"host\": {\n";
        ofs << fmt::format("    \"system\": \"{}\",\n", escapeJson(host_.system));
        ofs << fmt::format("    \"exePath\": \"{}\",\n", escapeJson(host_.exePath));
        ofs << fmt::format("    \"cpuCores\": {},\n", host_.cpuCores);
        ofs << fmt::format("    \"memTotalMB\": {:.2f}\n", host_.memTotalMB);
        ofs << "  },\n";
        ofs << "  \"results\": [\n";
        for (size_t i = 0; i < results_.size(); ++i) {
            const auto& r = results_[i];
            ofs << "    {\n";
            ofs << fmt::format("      \"name\": \"{}\",\n", escapeJson(r.name));
            ofs << fmt::format("      \"iterations\": {},\n", r.iterations);
            ofs << fmt::format("      \"total_ns\": {:.2f},\n", r.total_ns);
            ofs << fmt::format("      \"mean_ns\": {:.2f},\n", r.mean_ns);
            ofs << fmt::format("      \"median_ns\": {:.2f},\n", r.median_ns);
            ofs << fmt::format("      \"min_ns\": {:.2f},\n", r.min_ns);
            ofs << fmt::format("      \"max_ns\": {:.2f},\n", r.max_ns);
            ofs << fmt::format("      \"stddev_ns\": {:.2f},\n", r.stddev_ns);
            ofs << fmt::format("      \"total_human\": \"{}\",\n", fmtNs(r.total_ns));
            ofs << fmt::format("      \"mean_human\": \"{}\",\n", fmtNs(r.mean_ns));
            ofs << fmt::format("      \"median_human\": \"{}\",\n", fmtNs(r.median_ns));
            ofs << fmt::format("      \"min_human\": \"{}\",\n", fmtNs(r.min_ns));
            ofs << fmt::format("      \"max_human\": \"{}\",\n", fmtNs(r.max_ns));
            ofs << fmt::format("      \"stddev_human\": \"{}\"\n", fmtNs(r.stddev_ns));
            ofs << "    }";
            if (i + 1 < results_.size()) {
                ofs << ",";
            }
            ofs << "\n";
        }
        ofs << "  ]";

        if (!resourceResults_.empty()) {
            ofs << ",\n  \"resource\": [\n";
            for (size_t i = 0; i < resourceResults_.size(); ++i) {
                const auto& r = resourceResults_[i];
                ofs << "    {\n";
                ofs << fmt::format("      \"mode\": \"{}\",\n", escapeJson(r.mode));
                ofs << fmt::format("      \"side\": \"{}\",\n", escapeJson(r.side));
                ofs << fmt::format("      \"point\": \"{}\",\n", escapeJson(r.point));
                ofs << fmt::format("      \"rssMB\": {:.2f},\n", r.rssMB);
                ofs << fmt::format("      \"privateMB\": {:.2f},\n", r.privateMB);
                ofs << fmt::format("      \"cpuIdlePct\": {:.2f},\n", r.cpuIdlePct);
                ofs << fmt::format("      \"cpuBusyPct\": {:.2f},\n", r.cpuBusyPct);
                ofs << fmt::format("      \"viewCount\": {},\n", r.viewCount);
                ofs << fmt::format("      \"viewBytes\": {},\n", r.viewBytes);
                ofs << fmt::format("      \"llmCount\": {},\n", r.llmCount);
                ofs << fmt::format("      \"llmBytes\": {},\n", r.llmBytes);
                ofs << fmt::format("      \"tokens\": {},\n", r.tokens);
                ofs << fmt::format("      \"pluginsAgent\": {},\n", r.pluginsAgent);
                ofs << fmt::format("      \"pluginsClient\": {},\n", r.pluginsClient);
                ofs << fmt::format("      \"trimReclaimableMB\": {:.2f},\n", r.trimReclaimableMB);
                ofs << fmt::format("      \"heapFragmentPct\": {:.2f},\n", r.heapFragmentPct);
                ofs << fmt::format("      \"frames\": {},\n", r.frames);
                ofs << fmt::format("      \"frameAvgMs\": {:.3f},\n", r.frameAvgMs);
                ofs << fmt::format("      \"renderBytes\": {:.0f},\n", r.renderBytes);

                // 内存细项
                ofs << "      \"mem\": {\n";
                ofs << fmt::format("        \"rssMB\": {:.2f},\n", r.mem.rssMB);
                ofs << fmt::format("        \"pssMB\": {:.2f},\n", r.mem.pssMB);
                ofs << fmt::format("        \"privateMB\": {:.2f},\n", r.mem.privateMB);
                ofs << fmt::format("        \"privateDirtyMB\": {:.2f},\n", r.mem.privateDirtyMB);
                ofs << fmt::format("        \"anonMB\": {:.2f},\n", r.mem.anonMB);
                ofs << fmt::format("        \"fileMB\": {:.2f},\n", r.mem.fileMB);
                ofs << fmt::format("        \"shmemMB\": {:.2f},\n", r.mem.shmemMB);
                ofs << fmt::format("        \"swapMB\": {:.2f},\n", r.mem.swapMB);
                ofs << fmt::format("        \"peakRssMB\": {:.2f},\n", r.mem.peakRssMB);
                ofs << fmt::format("        \"peakVmMB\": {:.2f},\n", r.mem.peakVmMB);
                ofs << fmt::format("        \"vmsizeMB\": {:.2f},\n", r.mem.vmsizeMB);
                ofs << fmt::format("        \"threads\": {},\n", r.mem.threads);
                ofs << fmt::format("        \"fds\": {},\n", r.mem.fds);
                ofs << fmt::format("        \"heapInUseMB\": {:.2f},\n", r.mem.heapInUseMB);
                ofs << fmt::format("        \"heapFreeMB\": {:.2f},\n", r.mem.heapFreeMB);
                ofs << fmt::format("        \"heapArenaMB\": {:.2f},\n", r.mem.heapArenaMB);
                ofs << fmt::format("        \"heapMmapMB\": {:.2f},\n", r.mem.heapMmapMB);
                ofs << fmt::format(
                    "        \"heapValid\": {}\n",
                    r.mem.heapValid ? "true" : "false"
                );
                ofs << "      },\n";

                // CPU 细分
                ofs << "      \"cpu\": {\n";
                ofs << fmt::format("        \"busyPct\": {:.2f},\n", r.cpu.busyPct);
                ofs << fmt::format("        \"userMs\": {:.1f},\n", r.cpu.userMs);
                ofs << fmt::format("        \"sysMs\": {:.1f},\n", r.cpu.sysMs);
                ofs << fmt::format("        \"wallMs\": {:.1f}\n", r.cpu.wallMs);
                ofs << "      },\n";

                // 模块分解
                ofs << "      \"modules\": [";
                for (size_t k = 0; k < r.modules.size(); ++k) {
                    const auto& m = r.modules[k];
                    if (k > 0) {
                        ofs << ",";
                    }
                    ofs << "\n        {";
                    ofs << fmt::format("\"name\": \"{}\", ", escapeJson(m.name));
                    ofs << fmt::format("\"path\": \"{}\", ", escapeJson(m.path));
                    ofs << fmt::format("\"kind\": \"{}\", ", memRegionKindName(m.kind));
                    ofs << fmt::format("\"sizeMB\": {:.3f}, ", m.sizeMB);
                    ofs << fmt::format("\"rssMB\": {:.3f}, ", m.rssMB);
                    ofs << fmt::format("\"pssMB\": {:.3f}, ", m.pssMB);
                    ofs << fmt::format("\"privateDirtyMB\": {:.3f}, ", m.privateDirtyMB);
                    ofs << fmt::format("\"sharedMB\": {:.3f}, ", m.sharedMB);
                    ofs << fmt::format("\"anonMB\": {:.3f}, ", m.anonMB);
                    ofs << fmt::format("\"regions\": {}}}", m.regions);
                }
                ofs << "\n      ],\n";

                // 逻辑内存
                ofs << "      \"logical\": [";
                for (size_t k = 0; k < r.logical.size(); ++k) {
                    const auto& l = r.logical[k];
                    if (k > 0) {
                        ofs << ",";
                    }
                    ofs << "\n        {";
                    ofs << fmt::format("\"name\": \"{}\", ", escapeJson(l.name));
                    ofs << fmt::format("\"bytes\": {}, ", l.bytes);
                    ofs << fmt::format("\"count\": {}, ", l.count);
                    ofs << fmt::format("\"note\": \"{}\"}}", escapeJson(l.note));
                }
                ofs << "\n      ],\n";

                // 分阶段采样
                ofs << "      \"phases\": [";
                for (size_t k = 0; k < r.phases.size(); ++k) {
                    const auto& p = r.phases[k];
                    if (k > 0) {
                        ofs << ",";
                    }
                    ofs << "\n        {";
                    ofs << fmt::format("\"label\": \"{}\", ", escapeJson(p.label));
                    ofs << fmt::format("\"note\": \"{}\", ", escapeJson(p.note));
                    ofs << fmt::format("\"rssMB\": {:.2f}, ", p.mem.rssMB);
                    ofs << fmt::format("\"pssMB\": {:.2f}, ", p.mem.pssMB);
                    ofs << fmt::format("\"privateDirtyMB\": {:.2f}, ", p.mem.privateDirtyMB);
                    ofs << fmt::format("\"heapInUseMB\": {:.2f}, ", p.mem.heapInUseMB);
                    ofs << fmt::format("\"anonMB\": {:.2f}, ", p.mem.anonMB);
                    ofs << fmt::format("\"deltaRssMB\": {:.2f}, ", p.deltaRssMB);
                    ofs << fmt::format("\"deltaPssMB\": {:.2f}, ", p.deltaPssMB);
                    ofs << fmt::format("\"deltaHeapInUseMB\": {:.2f}, ", p.deltaHeapInUseMB);
                    ofs << fmt::format("\"elapsedMs\": {:.1f}}}", p.elapsedMs);
                }
                ofs << "\n      ],\n";

                ofs << fmt::format("      \"note\": \"{}\"\n", escapeJson(r.note));
                ofs << "    }";
                if (i + 1 < resourceResults_.size()) {
                    ofs << ",";
                }
                ofs << "\n";
            }
            ofs << "  ]\n";
        } else {
            ofs << "\n";
        }
        ofs << "}\n";
        return ofs.str();
    }

    std::string buildMarkdown(const std::string& timestamp) const {
        std::ostringstream md;
        md << "# agentxx 资源基准报告\n\n";
        md << fmt::format("- 时间: `{}`\n", timestamp);
        md << fmt::format("- 版本: `{}`  构建: `{}`\n", host_.version, host_.buildConfig);
        md << fmt::format(
            "- 系统: `{}`  CPU 核数: {}  物理内存: {:.1f} GB\n",
            host_.system,
            host_.cpuCores,
            host_.memTotalMB / 1024.0
        );
        md << fmt::format("- 被测程序: `{}`\n", host_.exePath);
        if (!baselinePath_.empty()) {
            md << fmt::format("- 基线对比: `{}`\n", baselinePath_);
        }
        md << "\n> 指标口径: RSS=常驻物理内存; PSS=按共享比例分摊后的常驻; 私脏=私有脏页\n"
              "> (最接近\"真实独占\"); 堆在用=glibc uordblks; 堆空闲=已 free 但保留在堆内(碎片);\n"
              "> 可回收=malloc_trim(0) 后 RSS 下降量 (归还系统); 匿名=匿名映射常驻。\n";

        // ---- 总览 ----
        md << "\n## 1. 总览 (各模式 × 采样点)\n\n";
        md << "| 模式 | 侧 | 采样点 | RSS | PSS | 私脏 | 匿名 | 峰值RSS | 堆在用 | 堆空闲 | "
              "线程 | fd | CPU空闲% | CPU繁忙% | tokens | view/llm | 备注 |\n";
        md << "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
        for (const auto& r : resourceResults_) {
            md << fmt::format(
                "| {} | {} | {} | {:.2f} | {:.2f} | {:.2f} | {:.2f} | {:.2f} | {} | {} | "
                "{} | {} | {:.1f} | {:.1f} | {} | {}/{} | {} |\n",
                r.mode,
                r.side,
                r.point,
                r.rssMB,
                r.mem.pssMB,
                r.mem.privateDirtyMB,
                r.mem.anonMB,
                r.mem.peakRssMB,
                r.mem.heapValid ? fmt::format("{:.2f}", r.mem.heapInUseMB) : std::string{"n/a"},
                r.mem.heapValid ? fmt::format("{:.2f}", r.mem.heapFreeMB) : std::string{"n/a"},
                r.mem.threads,
                r.mem.fds,
                r.cpuIdlePct,
                r.cpuBusyPct,
                r.tokens,
                r.viewCount,
                r.llmCount,
                escapeMd(r.note)
            );
        }

        // ---- 分阶段 ----
        md << "\n## 2. 分阶段内存归属 (定位\"哪一步吃掉多少内存\")\n\n";
        for (const auto& r : resourceResults_) {
            if (r.phases.empty()) {
                continue;
            }
            md << fmt::format("\n### {} / {} (分阶段)\n\n", r.mode, r.side);
            md << "| 阶段 | RSS | 增量Δ | PSS | 堆在用 | 匿名 | 耗时(ms) | 说明 |\n";
            md << "|---|---|---|---|---|---|---|---|\n";
            for (const auto& p : r.phases) {
                md << fmt::format(
                    "| {} | {:.2f} | {:+.2f} | {:.2f} | {:.2f} | {:.2f} | {:.0f} | {} |\n",
                    p.label,
                    p.mem.rssMB,
                    p.deltaRssMB,
                    p.mem.pssMB,
                    p.mem.heapInUseMB,
                    p.mem.anonMB,
                    p.elapsedMs,
                    escapeMd(p.note)
                );
            }
        }

        // ---- 模块分解 ----
        md << "\n## 3. 模块级内存分解 (smaps)\n\n";
        for (const auto& r : resourceResults_) {
            if (r.modules.empty()) {
                continue;
            }
            md << fmt::format("\n### {} / {} / {} (模块分解)\n\n", r.mode, r.side, r.point);
            md << "| 类别 | 模块 | RSS | PSS | 私脏 | 映射大小 | 段数 |\n";
            md << "|---|---|---|---|---|---|---|\n";
            double shownRss = 0.0;
            for (const auto& m : r.modules) {
                shownRss += m.rssMB;
                md << fmt::format(
                    "| {} | `{}` | {:.2f} | {:.2f} | {:.2f} | {:.2f} | {} |\n",
                    memRegionKindName(m.kind),
                    m.name.empty() ? m.path : m.name,
                    m.rssMB,
                    m.pssMB,
                    m.privateDirtyMB,
                    m.sizeMB,
                    m.regions
                );
            }
            md << fmt::format("| 合计 | | {:.2f} | | | | |\n", shownRss);
        }

        // ---- 逻辑内存 ----
        md << "\n## 4. 逻辑内存 (各模块数据结构自身占用)\n\n";
        for (const auto& r : resourceResults_) {
            if (r.logical.empty()) {
                continue;
            }
            md << fmt::format("\n### {} / {} / {}\n\n", r.mode, r.side, r.point);
            md << "| 模块 | 字节 | 条目 | 说明 |\n|---|---|---|---|\n";
            for (const auto& l : r.logical) {
                if (l.bytes == 0 && l.count == 0) {
                    continue;
                }
                md << fmt::format(
                    "| `{}` | {} | {} | {} |\n",
                    l.name,
                    formatBytes(l.bytes),
                    l.count,
                    escapeMd(l.note)
                );
            }
        }

        // ---- 与基线对比 ----
        if (!baseline_.empty()) {
            md << "\n## 5. 与基线对比\n\n";
            md << fmt::format("基线: `{}`\n\n", baselinePath_);
            md << "| 模式 | 侧 | 采样点 | RSS基线 | RSS本次 | ΔRSS | ΔPSS | Δ堆在用 | Δ峰值 |\n";
            md << "|---|---|---|---|---|---|---|---|---|\n";
            for (const auto& r : resourceResults_) {
                auto it = baseline_.find(resourceKey(r));
                if (it == baseline_.end()) {
                    continue;
                }
                const auto& b = it->second;
                md << fmt::format(
                    "| {} | {} | {} | {:.2f} | {:.2f} | {:+.2f} | {:+.2f} | {:+.2f} | {:+.2f} |\n",
                    r.mode,
                    r.side,
                    r.point,
                    b.rssMB,
                    r.rssMB,
                    r.rssMB - b.rssMB,
                    r.mem.pssMB - b.mem.pssMB,
                    r.mem.heapInUseMB - b.mem.heapInUseMB,
                    r.mem.peakRssMB - b.mem.peakRssMB
                );
            }

            md << "\n### 模块级差异 (|ΔRSS| > 0.3MB)\n\n";
            md << "| 采样点 | 模块 | 基线RSS | 本次RSS | ΔRSS |\n|---|---|---|---|---|\n";

            struct ModDiff {
                double      absDelta;
                std::string key;
                std::string name;
                double      base = 0.0;
                double      cur  = 0.0;
            };

            std::vector<ModDiff> diffs;
            for (const auto& r : resourceResults_) {
                auto it = baseline_.find(resourceKey(r));
                if (it == baseline_.end()) {
                    continue;
                }
                std::map<std::string, double> baseMap;
                for (const auto& m : it->second.modules) {
                    baseMap[m.name] += m.rssMB;
                }
                std::map<std::string, double> curMap;
                for (const auto& m : r.modules) {
                    curMap[m.name] += m.rssMB;
                }
                std::set<std::string> names;
                for (const auto& kv : baseMap) {
                    names.insert(kv.first);
                }
                for (const auto& kv : curMap) {
                    names.insert(kv.first);
                }
                for (const auto& n : names) {
                    double b = baseMap.count(n) ? baseMap[n] : 0.0;
                    double c = curMap.count(n) ? curMap[n] : 0.0;
                    if (std::abs(c - b) > 0.3) {
                        diffs.push_back(ModDiff{std::abs(c - b), resourceKey(r), n, b, c});
                    }
                }
            }
            std::sort(diffs.begin(), diffs.end(), [](const ModDiff& a, const ModDiff& b) {
                return a.absDelta > b.absDelta;
            });
            for (const auto& d : diffs) {
                md << fmt::format(
                    "| {} | `{}` | {:.2f} | {:.2f} | {:+.2f} |\n",
                    d.key,
                    d.name,
                    d.base,
                    d.cur,
                    d.cur - d.base
                );
            }
        }
        return md.str();
    }

    static std::string escapeJson(std::string_view s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        out += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
                    } else {
                        out.push_back(c);
                    }
            }
        }
        return out;
    }

    static std::string escapeMd(std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == '|' || c == '\n' || c == '\r') {
                out.push_back(' ');
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    static std::string formatBytes(size_t bytes) {
        if (bytes >= 1024ull * 1024ull) {
            return fmt::format("{:.2f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        }
        if (bytes >= 1024ull) {
            return fmt::format("{:.1f} KB", static_cast<double>(bytes) / 1024.0);
        }
        return fmt::format("{} B", bytes);
    }

    std::string                           outputDir_;
    HostInfo                              host_;
    std::vector<BenchResult>              results_;
    std::vector<ResourceResult>           resourceResults_;
    std::map<std::string, ResourceResult> baseline_;
    std::string                           baselinePath_;
};

template<typename Fn>
BenchResult runBench(const std::string& name, size_t iterations, Fn&& fn) {
    std::vector<double> durations;
    durations.reserve(iterations);

    for (size_t i = 0; i < iterations; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        fn();
        auto   end = std::chrono::high_resolution_clock::now();
        double ns  = std::chrono::duration<double, std::nano>(end - start).count();
        durations.push_back(ns);
    }

    double total_ns = 0;
    double min_ns   = durations[0];
    double max_ns   = durations[0];
    for (auto d : durations) {
        total_ns += d;
        if (d < min_ns) {
            min_ns = d;
        }
        if (d > max_ns) {
            max_ns = d;
        }
    }
    double mean_ns = total_ns / static_cast<double>(iterations);

    double variance = 0;
    for (auto d : durations) {
        double diff  = d - mean_ns;
        variance    += diff * diff;
    }
    double stddev_ns = std::sqrt(variance / static_cast<double>(iterations));

    std::sort(durations.begin(), durations.end());
    double median_ns = durations[iterations / 2];

    BenchResult r;
    r.name       = name;
    r.iterations = iterations;
    r.total_ns   = total_ns;
    r.mean_ns    = mean_ns;
    r.min_ns     = min_ns;
    r.max_ns     = max_ns;
    r.stddev_ns  = stddev_ns;
    r.median_ns  = median_ns;
    BenchReporter::instance().addResult(r);
    return r;
}

template<typename SetupFn, typename Fn>
BenchResult
    runBenchWithSetup(const std::string& name, size_t iterations, SetupFn&& setup, Fn&& fn) {
    std::vector<double> durations;
    durations.reserve(iterations);

    for (size_t i = 0; i < iterations; ++i) {
        setup();
        auto start = std::chrono::high_resolution_clock::now();
        fn();
        auto   end = std::chrono::high_resolution_clock::now();
        double ns  = std::chrono::duration<double, std::nano>(end - start).count();
        durations.push_back(ns);
    }

    double total_ns = 0;
    double min_ns   = durations[0];
    double max_ns   = durations[0];
    for (auto d : durations) {
        total_ns += d;
        if (d < min_ns) {
            min_ns = d;
        }
        if (d > max_ns) {
            max_ns = d;
        }
    }
    double mean_ns = total_ns / static_cast<double>(iterations);

    double variance = 0;
    for (auto d : durations) {
        double diff  = d - mean_ns;
        variance    += diff * diff;
    }
    double stddev_ns = std::sqrt(variance / static_cast<double>(iterations));

    std::sort(durations.begin(), durations.end());
    double median_ns = durations[iterations / 2];

    BenchResult r;
    r.name       = name;
    r.iterations = iterations;
    r.total_ns   = total_ns;
    r.mean_ns    = mean_ns;
    r.min_ns     = min_ns;
    r.max_ns     = max_ns;
    r.stddev_ns  = stddev_ns;
    r.median_ns  = median_ns;
    BenchReporter::instance().addResult(r);
    return r;
}

} // namespace bench
} // namespace agentxx