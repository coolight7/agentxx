#pragma once

// 资源基准测试的内存探测工具:
// - 进程内存细项 (RSS / PSS / 私有 / 匿名 / 文件映射 / 峰值 / 线程 / 文件句柄)
// - glibc 堆统计 (在用 / 空闲 / 向系统申请量 / 碎片率) 与 malloc_trim 可回收量
// - /proc/<pid>/smaps 模块级分解 (可执行文件 / 工具库 / 各插件 / 系统库 / 堆 / 栈 / 匿名)
// - 分阶段采样追踪 (每个阶段相对上一阶段的增量, 用于定位内存归属)
//
// 说明:
// - Linux 上读取 /proc 获取全部细项; 其他平台回退到能获取的部分 (Windows: 工作集/私有/峰值/句柄数)
// - 堆统计 (mallinfo2) 只能查询**自身进程**, 采样子进程时 heapValid=false
// - smaps 读取开销 ~ 数毫秒 (与映射数量相关), 仅在阶段性采样点调用

#include "bench_util.h"

#include "utilxx_base/log.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if XX_IS_WIN_D
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <psapi.h>
#include <tlhelp32.h>
#include <windows.h>
#else
#include <dirent.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace agentxx {
namespace bench {

// ---------------------------------------------------------------------------
// 0. 基础工具 (进程 id / 文件读取)
// ---------------------------------------------------------------------------

inline uint32_t currentProcessId() {
#if XX_IS_WIN_D
    return static_cast<uint32_t>(::GetCurrentProcessId());
#else
    return static_cast<uint32_t>(getpid());
#endif
}

/// 归一化进程 id: 0 表示自身进程
inline uint32_t normalizePid(uint32_t pid) {
    return (pid == 0) ? currentProcessId() : pid;
}

inline bool isSelfPid(uint32_t pid) {
    return normalizePid(pid) == currentProcessId();
}

/// 读取整个文件 (失败返回空串; 仅用于 /proc 小文件与统计文件)
inline std::string readWholeFile(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        return {};
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

/// 当前可执行文件绝对路径 (取不到时返回空串)
inline std::string currentExecutablePath() {
#if XX_IS_WIN_D
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return {};
        }
        if (len < buf.size()) {
            buf.resize(len);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    std::string out(buf.begin(), buf.end());
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
#else
    char    buf[4096] = {0};
    ssize_t n         = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return {};
    }
    return std::string(buf, static_cast<size_t>(n));
#endif
}

/// 指定进程的可执行文件路径 (自身读取 /proc/self/exe; 子进程读 /proc/<pid>/exe)
inline std::string processExecutablePath(uint32_t pid = 0) {
    uint32_t actualPid = normalizePid(pid);
    if (isSelfPid(actualPid)) {
        return currentExecutablePath();
    }
#if XX_IS_WIN_D
    std::string out;
    HANDLE      h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, actualPid);
    if (h) {
        char buf[MAX_PATH * 2] = {0};
        DWORD len              = MAX_PATH;
        if (::QueryFullProcessImageNameA(h, 0, buf, &len)) {
            out.assign(buf, len);
            std::replace(out.begin(), out.end(), '\\', '/');
        }
        ::CloseHandle(h);
    }
    return out;
#else
    char    buf[4096] = {0};
    ssize_t n         = ::readlink(
        ("/proc/" + std::to_string(actualPid) + "/exe").c_str(),
        buf,
        sizeof(buf) - 1
    );
    if (n <= 0) {
        return {};
    }
    std::string out(buf, static_cast<size_t>(n));
    auto        deleted = out.find(" (deleted)");
    if (deleted != std::string::npos) {
        out.resize(deleted);
    }
    return out;
#endif
}

// ---------------------------------------------------------------------------
// 1. 进程内存细项
// ---------------------------------------------------------------------------


#if XX_IS_WIN_D

inline ProcMemDetail sampleProcMemDetail(uint32_t pid = 0) {
    ProcMemDetail out;
    uint32_t      actualPid = normalizePid(pid);

    HANDLE h = isSelfPid(actualPid)
                   ? ::GetCurrentProcess()
                   : ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, actualPid);
    if (!h) {
        return out;
    }
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (::GetProcessMemoryInfo(h, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        out.rssMB     = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        out.peakRssMB = static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
        out.privateMB = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
        out.peakVmMB  = static_cast<double>(pmc.PagefileUsage) / (1024.0 * 1024.0);
        out.vmsizeMB  = static_cast<double>(pmc.PagefileUsage) / (1024.0 * 1024.0);
        out.valid     = true;
    }
    DWORD handleCount = 0;
    if (::GetProcessHandleCount(h, &handleCount)) {
        out.fds = handleCount;
    }
    if (!isSelfPid(actualPid)) {
        ::CloseHandle(h);
    }

    // 线程数: 遍历系统线程快照统计归属该进程的线程
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        if (::Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID == actualPid) {
                    ++out.threads;
                }
            } while (::Thread32Next(snap, &te));
        }
        ::CloseHandle(snap);
    }
    return out;
}

#else // ------------------------------ Linux / Android ------------------------------

/// 解析 /proc/<pid>/status (键值对, 值形如 "1234 kB")
inline ProcMemDetail sampleProcMemDetail(uint32_t pid = 0) {
    ProcMemDetail out;
    uint32_t      actualPid = normalizePid(pid);

    std::string status = readWholeFile("/proc/" + std::to_string(actualPid) + "/status");
    if (status.empty()) {
        return out;
    }

    auto parseKb = [](std::string_view value) -> double {
        // 输入形如 "  12345 kB"; 取数字部分
        double      num = 0.0;
        std::string tmp(value);
        std::istringstream iss(tmp);
        iss >> num;
        return num / 1024.0; // kB -> MB
    };

    std::istringstream iss(status);
    std::string        line;
    while (std::getline(iss, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            out.rssMB = parseKb(line.substr(6));
        } else if (line.rfind("VmHWM:", 0) == 0) {
            out.peakRssMB = parseKb(line.substr(6));
        } else if (line.rfind("VmSize:", 0) == 0) {
            out.vmsizeMB = parseKb(line.substr(7));
        } else if (line.rfind("VmPeak:", 0) == 0) {
            out.peakVmMB = parseKb(line.substr(7));
        } else if (line.rfind("VmSwap:", 0) == 0) {
            out.swapMB = parseKb(line.substr(7));
        } else if (line.rfind("RssAnon:", 0) == 0) {
            out.privateMB += parseKb(line.substr(8));
            out.anonMB     = out.privateMB;
        } else if (line.rfind("RssFile:", 0) == 0) {
            out.fileMB = parseKb(line.substr(8));
        } else if (line.rfind("RssShmem:", 0) == 0) {
            out.shmemMB  = parseKb(line.substr(9));
            out.privateMB += out.shmemMB;
        } else if (line.rfind("Threads:", 0) == 0) {
            std::istringstream vs(line.substr(8));
            vs >> out.threads;
        }
    }
    out.valid = true;
    if (out.privateMB <= 0.0) {
        out.privateMB = out.rssMB;
    }

    // PSS / 私有脏页: smaps_rollup 一次读取即可 (Linux 4.14+), 缺失时回退逐段累加
    std::string rollup = readWholeFile("/proc/" + std::to_string(actualPid) + "/smaps_rollup");
    if (!rollup.empty()) {
        std::istringstream rs(rollup);
        while (std::getline(rs, line)) {
            if (line.rfind("Pss:", 0) == 0) {
                out.pssMB = parseKb(line.substr(4));
            } else if (line.rfind("Private_Dirty:", 0) == 0) {
                out.privateDirtyMB = parseKb(line.substr(14));
            } else if (line.rfind("Anonymous:", 0) == 0) {
                out.anonMB = parseKb(line.substr(10));
            }
        }
    }

    // 文件描述符数量
    DIR* dir = ::opendir(("/proc/" + std::to_string(actualPid) + "/fd").c_str());
    if (dir) {
        uint64_t count = 0;
        while (::readdir(dir) != nullptr) {
            ++count;
        }
        ::closedir(dir);
        out.fds = (count >= 2) ? (count - 2) : count; // 去掉 "." 与 ".."
    }

    // glibc 堆统计: 仅对自身进程有意义 (mallinfo2 查询的是调用进程)
    if (isSelfPid(actualPid)) {
#if defined(__GLIBC__)
        struct mallinfo2 mi = ::mallinfo2();
        out.heapInUseMB     = static_cast<double>(mi.uordblks) / (1024.0 * 1024.0);
        out.heapFreeMB      = static_cast<double>(mi.fordblks) / (1024.0 * 1024.0);
        out.heapArenaMB     = static_cast<double>(mi.arena) / (1024.0 * 1024.0);
        out.heapMmapMB      = static_cast<double>(mi.hblkhd) / (1024.0 * 1024.0);
        out.heapValid       = true;
#endif
    }
    return out;
}

#endif

/// 多次采样取 RSS 中位数 (抑制抖动); 每轮间隔 intervalMs
inline ProcMemDetail
    sampleProcMemDetailMedian(uint32_t pid = 0, size_t times = 3, int intervalMs = 30) {
    std::vector<ProcMemDetail> samples;
    samples.reserve(times);
    for (size_t i = 0; i < times; ++i) {
        auto s = sampleProcMemDetail(pid);
        if (s.valid) {
            samples.push_back(s);
        }
        if (i + 1 < times) {
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
        }
    }
    if (samples.empty()) {
        return sampleProcMemDetail(pid);
    }
    std::sort(samples.begin(), samples.end(), [](const ProcMemDetail& a, const ProcMemDetail& b) {
        return a.rssMB < b.rssMB;
    });
    return samples[samples.size() / 2];
}

/// 堆碎片率 = 空闲保留字节 / 堆总量; -1 表示不可用 (非自身进程/非 glibc)
inline double heapFragmentPercent(const ProcMemDetail& m) {
    if (!m.heapValid) {
        return -1.0;
    }
    double total = m.heapInUseMB + m.heapFreeMB;
    if (total <= 0.0) {
        return 0.0;
    }
    return m.heapFreeMB * 100.0 / total;
}

/// malloc_trim(0) 后 RSS 下降量 (可回收内存 MB; 仅 Linux glibc 自身进程可用)
/// - 表示"已 free 但仍保留在堆/arena 中, 未归还系统"的字节
/// - 注意: 会改变进程内存状态 (归还后无法通过再次 trim 观察), 仅在采样点调用
inline double trimReclaimableMB() {
#if XX_IS_WIN_D || !defined(__GLIBC__)
    return -1.0;
#else
    auto before = sampleProcMemDetailMedian(0, 3, 10).rssMB;
    ::malloc_trim(0);
    auto  after  = sampleProcMemDetailMedian(0, 3, 10).rssMB;
    double delta = before - after;
    return (delta > 0.0) ? delta : 0.0;
#endif
}

// ---------------------------------------------------------------------------
// 2. smaps 模块级内存分解
// ---------------------------------------------------------------------------

/// 汇总各类别 (exe/project-lib/plugin-lib/system-lib/heap/stack/anon/...) 的占用
struct MemKindSummary {
    double rssMB          = 0.0;
    double pssMB          = 0.0;
    double privateDirtyMB = 0.0;
    double sizeMB         = 0.0;
    size_t regions        = 0;
};

inline std::map<MemRegionKind, MemKindSummary> summarizeByKind(const ModuleMemBreakdown& bd) {
    std::map<MemRegionKind, MemKindSummary> out;
    for (const auto& row : bd.rows) {
        auto& s = out[row.kind];
        s.rssMB += row.rssMB;
        s.pssMB += row.pssMB;
        s.privateDirtyMB += row.privateDirtyMB;
        s.sizeMB += row.sizeMB;
        s.regions += row.regions;
    }
    return out;
}

#if XX_IS_WIN_D

/// Windows: 仅列出已加载模块的映像大小 (PSAPI 不提供按模块的常驻内存)
inline ModuleMemBreakdown sampleModuleBreakdown(uint32_t pid = 0, size_t topN = 0) {
    (void)pid;
    (void)topN;
    ModuleMemBreakdown bd;
    bd.note = "Windows 平台不提供按模块的常驻内存分解 (仅 RSS/私有/峰值可用)";
    return bd;
}

#else

/// 依据映射路径判断归属类别
inline void classifyRegion(
    std::string_view path,
    const std::string& exePath,
    MemRegionKind&     kind,
    std::string&       displayName
) {
    displayName = std::string(path);
    if (path.empty()) {
        kind        = MemRegionKind::Anon;
        displayName = "[anon]";
        return;
    }
    if (path == "[heap]") {
        kind        = MemRegionKind::Heap;
        displayName = "[heap]";
        return;
    }
    if (path.rfind("[stack", 0) == 0) {
        kind        = MemRegionKind::Stack;
        displayName = "[stack]";
        return;
    }
    if (path.rfind("[vdso", 0) == 0 || path.rfind("[vvar", 0) == 0 || path.rfind("[vsyscall", 0) == 0) {
        kind        = MemRegionKind::Vdso;
        displayName = std::string(path);
        return;
    }
    if (path.rfind("/dev/shm/", 0) == 0 || path.rfind("/memfd:", 0) == 0
        || path.rfind("memfd:", 0) == 0 || path.find("/SYSV", path.size() > 8 ? path.size() - 8 : 0) != std::string::npos) {
        kind        = MemRegionKind::Shm;
        displayName = std::string(path);
        return;
    }

    // 文件映射: 取末尾文件名作为显示名
    auto slash = path.rfind('/');
    auto leaf  = (slash == std::string_view::npos) ? path : path.substr(slash + 1);
    // 去掉 "(deleted)" 后缀
    auto del = leaf.find(" (deleted)");
    if (del != std::string_view::npos) {
        leaf = leaf.substr(0, del);
    }
    displayName = std::string(leaf);

    if (!exePath.empty() && path == exePath) {
        kind = MemRegionKind::Exe;
        return;
    }
    // 插件: .../plugins/<插件目录>/xxx.so (目录名即插件名)
    auto pluginsPos = path.find("/plugins/");
    if (pluginsPos != std::string_view::npos) {
        auto after  = path.substr(pluginsPos + 9);
        auto nextSl = after.find('/');
        if (nextSl != std::string_view::npos) {
            displayName = std::string(after.substr(0, nextSl));
        } else {
            displayName = std::string(after);
        }
        kind = MemRegionKind::PluginLib;
        return;
    }
    if (leaf.rfind("libagentxx", 0) == 0 || leaf.rfind("libcxx_utilxx", 0) == 0
        || leaf.rfind("libcxx_pluginxx", 0) == 0) {
        kind = MemRegionKind::ProjectLib;
        return;
    }
    if (leaf.rfind("libc.so", 0) == 0 || leaf.rfind("libm.so", 0) == 0
        || leaf.rfind("libstdc++", 0) == 0 || leaf.rfind("libgcc_s", 0) == 0
        || leaf.rfind("ld-linux", 0) == 0 || leaf.rfind("libpthread", 0) == 0
        || leaf.rfind("libdl", 0) == 0 || leaf.rfind("librt", 0) == 0
        || leaf.rfind("libc++.so", 0) == 0 || leaf.rfind("libunwind", 0) == 0
        || leaf.rfind("libwinpthread", 0) == 0) {
        kind = MemRegionKind::SystemLib;
        return;
    }
    if (path.rfind("/usr/", 0) == 0 || path.rfind("/lib/", 0) == 0 || path.rfind("/lib64/", 0) == 0
        || path.rfind("/system/", 0) == 0 || path.rfind("/apex/", 0) == 0
        || path.rfind("/opt/", 0) == 0) {
        kind = (leaf.find(".so") != std::string_view::npos) ? MemRegionKind::SystemLib
                                                            : MemRegionKind::DataFile;
        return;
    }
    if (leaf.find(".so") != std::string_view::npos) {
        kind = MemRegionKind::SystemLib;
        return;
    }
    kind = MemRegionKind::DataFile;
}

inline ModuleMemBreakdown sampleModuleBreakdown(uint32_t pid = 0, size_t topN = 0) {
    ModuleMemBreakdown bd;
    uint32_t           actualPid = normalizePid(pid);
    // 子进程同样解析 exe 路径: 否则其可执行文件段会被误判为"数据文件"
    std::string        exePath   = processExecutablePath(actualPid);

    std::ifstream ifs("/proc/" + std::to_string(actualPid) + "/smaps");
    if (!ifs.is_open()) {
        bd.note = "无法读取 smaps (权限或进程已退出)";
        return bd;
    }

    std::map<std::string, ModuleMemRow> agg; // key = kind|name
    ModuleMemRow*                       cur = nullptr;

    auto flushRow = [&](ModuleMemRow* row) {
        if (!row) {
            return;
        }
        std::string key = std::string(memRegionKindName(row->kind)) + "|" + row->name;
        auto        it  = agg.find(key);
        if (it == agg.end()) {
            ModuleMemRow copy = *row;
            agg.emplace(key, copy);
        } else {
            auto& t = it->second;
            t.sizeMB += row->sizeMB;
            t.rssMB += row->rssMB;
            t.pssMB += row->pssMB;
            t.privateDirtyMB += row->privateDirtyMB;
            t.privateCleanMB += row->privateCleanMB;
            t.sharedMB += row->sharedMB;
            t.anonMB += row->anonMB;
            t.regions += row->regions;
        }
    };

    std::string        line;
    ModuleMemRow       pendingRow;
    while (std::getline(ifs, line)) {
        if (line.empty()) {
            continue;
        }
        // 段头形如 "7f12...-7f13... rw-p 00000000 00:00 0   /path";
        // 属性行形如 "Size:  132 kB" (首个冒号出现在首个空格之前)
        const auto colonPos = line.find(':');
        const auto spacePos = line.find(' ');
        const bool isPropertyLine
            = (colonPos != std::string::npos)
              && (spacePos == std::string::npos || colonPos < spacePos);

        if (!isPropertyLine) {
            // 段头: "start-end perms offset dev inode  path"
            flushRow(cur);
            cur = &pendingRow;
            pendingRow = ModuleMemRow{};

            // 取前 5 个空白分隔字段, 其余为路径
            size_t pos = 0;
            int    field = 0;
            while (pos < line.size() && field < 5) {
                while (pos < line.size() && line[pos] == ' ') {
                    ++pos;
                }
                while (pos < line.size() && line[pos] != ' ') {
                    ++pos;
                }
                ++field;
            }
            while (pos < line.size() && line[pos] == ' ') {
                ++pos;
            }
            std::string path = (pos < line.size()) ? line.substr(pos) : std::string{};
            if (path.rfind("[anon:") == 0 || path == "[anon]") {
                path.clear();
            }
            std::string   displayName;
            MemRegionKind kind = MemRegionKind::Other;
            classifyRegion(path, exePath, kind, displayName);
            pendingRow.path      = path;
            pendingRow.name      = displayName;
            pendingRow.kind      = kind;
            pendingRow.regions   = 1;
        } else if (cur != nullptr) {
            // 段属性行: "Key:  value kB"
            std::string_view   key = std::string_view(line).substr(0, colonPos);
            std::istringstream vs(line.substr(colonPos + 1));
            double             kb = 0.0;
            vs >> kb;
            double mb = kb / 1024.0;
            if (key == "Size") {
                cur->sizeMB = mb;
            } else if (key == "Rss") {
                cur->rssMB = mb;
            } else if (key == "Pss") {
                cur->pssMB = mb;
            } else if (key == "Private_Dirty") {
                cur->privateDirtyMB = mb;
            } else if (key == "Private_Clean") {
                cur->privateCleanMB = mb;
            } else if (key == "Shared_Clean") {
                cur->sharedMB += mb;
            } else if (key == "Shared_Dirty") {
                cur->sharedMB += mb;
            } else if (key == "Anonymous") {
                cur->anonMB = mb;
            }
        }
    }
    flushRow(cur);

    for (auto& kv : agg) {
        auto& row = kv.second;
        bd.totalRssMB += row.rssMB;
        bd.totalPssMB += row.pssMB;
        bd.rows.push_back(row);
    }
    std::sort(bd.rows.begin(), bd.rows.end(), [](const ModuleMemRow& a, const ModuleMemRow& b) {
        return a.rssMB > b.rssMB;
    });
    if (topN > 0 && bd.rows.size() > topN) {
        ModuleMemRow other;
        other.name = fmt::format("[其余 {} 项汇总]", bd.rows.size() - topN);
        other.kind = MemRegionKind::Other;
        for (size_t i = topN; i < bd.rows.size(); ++i) {
            other.rssMB += bd.rows[i].rssMB;
            other.pssMB += bd.rows[i].pssMB;
            other.privateDirtyMB += bd.rows[i].privateDirtyMB;
            other.sizeMB += bd.rows[i].sizeMB;
            other.regions += bd.rows[i].regions;
        }
        bd.rows.resize(topN);
        bd.rows.push_back(other);
    }
    bd.valid = true;
    return bd;
}

#endif

/// 打印模块分解表 (取前 topN 行)
inline void printModuleBreakdown(
    const ModuleMemBreakdown& bd,
    const std::string&        title,
    size_t                    topN = 15,
    std::ostream&             os   = std::cout
) {
    if (!bd.valid) {
        os << fmt::format("  [{}] 模块分解不可用: {}\n", title, bd.note);
        return;
    }
    os << fmt::format(
        "  [{}] 模块内存分解 (总 RSS {:.2f} MB / PSS {:.2f} MB)\n",
        title,
        bd.totalRssMB,
        bd.totalPssMB
    );
    os << fmt::format(
        "    {:<8} {:<34} {:>9} {:>9} {:>9} {:>9} {:>6}\n",
        "类别",
        "模块",
        "RSS(MB)",
        "PSS(MB)",
        "私脏(MB)",
        "大小(MB)",
        "段数"
    );
    size_t shown = 0;
    for (const auto& row : bd.rows) {
        if (topN > 0 && shown >= topN && row.name.rfind("[其余", 0) != 0) {
            continue;
        }
        std::string name = row.name;
        if (name.size() > 34) {
            name = name.substr(0, 31) + "...";
        }
        os << fmt::format(
            "    {:<8} {:<34} {:>9.2f} {:>9.2f} {:>9.2f} {:>9.2f} {:>6}\n",
            memRegionKindName(row.kind),
            name,
            row.rssMB,
            row.pssMB,
            row.privateDirtyMB,
            row.sizeMB,
            row.regions
        );
        ++shown;
    }
}

/// 打印按类别汇总的模块分解 (简洁版, 用于报告开头)
inline void printKindSummary(
    const ModuleMemBreakdown& bd,
    const std::string&        title,
    std::ostream&             os = std::cout
) {
    if (!bd.valid) {
        return;
    }
    auto summary = summarizeByKind(bd);
    os << fmt::format("  [{}] 按类别汇总:\n", title);
    for (const auto& kv : summary) {
        os << fmt::format(
            "    {:<12} RSS={:>8.2f}MB  PSS={:>8.2f}MB  私脏={:>8.2f}MB  段={}\n",
            memRegionKindName(kv.first),
            kv.second.rssMB,
            kv.second.pssMB,
            kv.second.privateDirtyMB,
            kv.second.regions
        );
    }
}

// ---------------------------------------------------------------------------
// 3. 分阶段采样追踪
// ---------------------------------------------------------------------------

/// 阶段内存追踪器: 每个 mark() 采样一次并计算与上一次的增量
/// - 用于定位"哪一步吃掉了多少内存" (启动各阶段 / 注入上下文 / 每轮对话)
/// - 采样默认取中位数, 减少抖动; 需要账目细项对比时可启用 includeBreakdown
class MemPhaseTracker {
public:

    MemPhaseTracker(uint32_t pid = 0, std::string side = "self") :
        pid_(normalizePid(pid)),
        side_(std::move(side)) {}

    uint32_t pid() const {
        return pid_;
    }

    const std::string& side() const {
        return side_;
    }

    /// 记录一个阶段点 (label 建议用英文短标识, 便于报告对比)
    MemPhaseSample& mark(std::string label, std::string note = {}) {
        auto  now  = std::chrono::steady_clock::now();
        auto  mem  = sampleProcMemDetailMedian(pid_, 3, 20);
        double elapsedMs
            = lastTime_.time_since_epoch().count() == 0
                  ? 0.0
                  : std::chrono::duration<double, std::milli>(now - lastTime_).count();

        MemPhaseSample s;
        s.label   = std::move(label);
        s.note    = std::move(note);
        s.mem     = mem;
        s.elapsedMs = elapsedMs;
        if (!samples_.empty()) {
            const auto& prev    = samples_.back().mem;
            s.deltaRssMB        = mem.rssMB - prev.rssMB;
            s.deltaPssMB        = mem.pssMB - prev.pssMB;
            s.deltaHeapInUseMB  = mem.heapInUseMB - prev.heapInUseMB;
            s.deltaAnonMB       = mem.anonMB - prev.anonMB;
            s.deltaPrivateMB    = mem.privateMB - prev.privateMB;
        }
        lastTime_ = now;
        samples_.push_back(s);
        return samples_.back();
    }

    const std::vector<MemPhaseSample>& samples() const {
        return samples_;
    }

    /// 打印阶段表 (含增量)
    void printTable(const std::string& title, std::ostream& os = std::cout) const {
        if (samples_.empty()) {
            return;
        }
        os << fmt::format("  [{}] 分阶段内存 (RSS {:.2f} MB 起)\n", title, samples_.front().mem.rssMB);
        os << fmt::format(
            "    {:<24} {:>10} {:>10} {:>10} {:>10} {:>10} {:>8}\n",
            "阶段",
            "RSS(MB)",
            "增量(MB)",
            "PSS(MB)",
            "堆在用(MB)",
            "匿名(MB)",
            "耗时(ms)"
        );
        for (const auto& s : samples_) {
            os << fmt::format(
                "    {:<24} {:>10.2f} {:>+10.2f} {:>10.2f} {:>10.2f} {:>10.2f} {:>8.0f} {}\n",
                s.label.size() > 24 ? s.label.substr(0, 24) : s.label,
                s.mem.rssMB,
                s.deltaRssMB,
                s.mem.pssMB,
                s.mem.heapInUseMB,
                s.mem.anonMB,
                s.elapsedMs,
                s.note
            );
        }
    }

private:

    uint32_t                                    pid_;
    std::string                                 side_;
    std::vector<MemPhaseSample>                 samples_;
    std::chrono::steady_clock::time_point       lastTime_{};
};

// ---------------------------------------------------------------------------
// 4. CPU 细项 (用户态/内核态时间 + 平均利用率)
// ---------------------------------------------------------------------------

#if !XX_IS_WIN_D

/// 读取 /proc/<pid>/stat 的 utime/stime (毫秒) 与线程数
inline bool procCpuTimeMs(uint32_t pid, double& userMs, double& sysMs, uint64_t* threads = nullptr) {
    std::string content = readWholeFile("/proc/" + std::to_string(normalizePid(pid)) + "/stat");
    if (content.empty()) {
        return false;
    }
    auto lastParen = content.rfind(')');
    if (lastParen == std::string::npos || lastParen + 1 >= content.size()) {
        return false;
    }
    std::istringstream iss(content.substr(lastParen + 1));
    std::string        state;
    // 字段: state(3) ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime
    //       cutime cstime priority nice num_threads(20) ...
    uint64_t ppid, pgrp, session, tty, tpgid, flags, minflt, cminflt, majflt, cmajflt;
    uint64_t utime = 0, stime = 0, cutime = 0, cstime = 0, priority = 0, nice = 0, numThreads = 0;
    if (!(iss >> state >> ppid >> pgrp >> session >> tty >> tpgid >> flags >> minflt >> cminflt
          >> majflt >> cmajflt >> utime >> stime >> cutime >> cstime >> priority >> nice
          >> numThreads)) {
        return false;
    }
    long clkTck = ::sysconf(_SC_CLK_TCK);
    if (clkTck <= 0) {
        clkTck = 100;
    }
    userMs = static_cast<double>(utime) * 1000.0 / static_cast<double>(clkTck);
    sysMs  = static_cast<double>(stime) * 1000.0 / static_cast<double>(clkTck);
    if (threads != nullptr) {
        *threads = numThreads;
    }
    return true;
}

#endif

/// 采样窗口内的 CPU 细项 (需先调用 [cpuBegin] 获取窗口)
inline CpuDelta cpuEndDetail(uint32_t pid, uint64_t startUserTicks, uint64_t startSysTicks,
                             std::chrono::steady_clock::time_point startTime) {
    CpuDelta d;
    auto     now  = std::chrono::steady_clock::now();
    d.wallMs      = std::chrono::duration<double, std::milli>(now - startTime).count();
#if XX_IS_WIN_D
    HANDLE h = isSelfPid(pid) ? ::GetCurrentProcess()
                              : ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        return d;
    }
    FILETIME ftCreation{}, ftExit{}, ftKernel{}, ftUser{};
    if (::GetProcessTimes(h, &ftCreation, &ftExit, &ftKernel, &ftUser)) {
        uint64_t k = (static_cast<uint64_t>(ftKernel.dwHighDateTime) << 32) | ftKernel.dwLowDateTime;
        uint64_t u = (static_cast<uint64_t>(ftUser.dwHighDateTime) << 32) | ftUser.dwLowDateTime;
        uint64_t kTicks = k / 10000; // 100ns -> ms
        uint64_t uTicks = u / 10000;
        if (kTicks >= startSysTicks && uTicks >= startUserTicks) {
            d.sysMs  = static_cast<double>(kTicks - startSysTicks);
            d.userMs = static_cast<double>(uTicks - startUserTicks);
        }
    }
    if (!isSelfPid(pid)) {
        ::CloseHandle(h);
    }
#else
    double userMs = 0.0, sysMs = 0.0;
    if (procCpuTimeMs(pid, userMs, sysMs)) {
        double startUserMs = static_cast<double>(startUserTicks);
        double startSysMs  = static_cast<double>(startSysTicks);
        if (userMs >= startUserMs && sysMs >= startSysMs) {
            d.userMs = userMs - startUserMs;
            d.sysMs  = sysMs - startSysMs;
        }
    }
#endif
    if (d.wallMs > 0.5) {
        d.busyPct = (d.userMs + d.sysMs) * 100.0 / d.wallMs;
    }
    return d;
}

/// CPU 采样窗口 (与 bench_resource_util.h 的 cpuBegin/cpuEnd 语义一致, 额外携带细分时间)
struct CpuWindowDetail {
    uint32_t                              pid {0};
    uint64_t                              userTicks {0};
    uint64_t                              sysTicks {0};
    std::chrono::steady_clock::time_point startTime {};
    bool                                  valid {false};
    /// 最近一次结束采样得到的细分结果 (经非 const 版 cpuEnd 记录, 便于直接挂到报告里)
    CpuDelta                              result {};
};

inline CpuWindowDetail cpuBeginDetail(uint32_t pid = 0) {
    CpuWindowDetail win;
    win.pid       = normalizePid(pid);
    win.startTime = std::chrono::steady_clock::now();
#if XX_IS_WIN_D
    HANDLE h = isSelfPid(win.pid) ? ::GetCurrentProcess()
                                  : ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, win.pid);
    if (h) {
        FILETIME ftCreation{}, ftExit{}, ftKernel{}, ftUser{};
        if (::GetProcessTimes(h, &ftCreation, &ftExit, &ftKernel, &ftUser)) {
            uint64_t k = (static_cast<uint64_t>(ftKernel.dwHighDateTime) << 32) | ftKernel.dwLowDateTime;
            uint64_t u = (static_cast<uint64_t>(ftUser.dwHighDateTime) << 32) | ftUser.dwLowDateTime;
            win.sysTicks  = k / 10000;
            win.userTicks = u / 10000;
            win.valid     = true;
        }
        if (!isSelfPid(win.pid)) {
            ::CloseHandle(h);
        }
    }
#else
    double userMs = 0.0, sysMs = 0.0;
    if (procCpuTimeMs(win.pid, userMs, sysMs)) {
        win.userTicks = static_cast<uint64_t>(userMs);
        win.sysTicks  = static_cast<uint64_t>(sysMs);
        win.valid     = true;
    }
#endif
    return win;
}

inline CpuDelta cpuEndDetailByWindow(const CpuWindowDetail& win) {
    if (!win.valid) {
        return CpuDelta{};
    }
    return cpuEndDetail(win.pid, win.userTicks, win.sysTicks, win.startTime);
}

/// 结束窗口并返回平均利用率百分比, 同时把细分结果记录在窗口对象内
/// (调用方随后可直接读取 win.result 挂到报告)
inline double cpuEnd(CpuWindowDetail& win) {
    win.result = cpuEndDetailByWindow(win);
    return win.result.busyPct;
}

} // namespace bench
} // namespace agentxx
