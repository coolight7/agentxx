#include "cpu_gpu_monitor.h"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "system_monitor_plugin.h"
#include "utilxx_base/log.h"
#include "utilxx_base/system.h"
#include <fmt/format.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if XX_IS_WIN_D
// 注意: windows.h 必须放在 pdh.h / pdhmsg.h 之前 (pdh.h 只包含 winapifamily.h,
// 依赖 windows.h 提供 BOOL/DWORD/HANDLE 等基础类型)
#include <windows.h>
// ---
#include <dxgi1_6.h>
#include <pdh.h>
#include <pdhmsg.h>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "pdh.lib")

#undef max
#undef min

namespace agentxx_system_monitor_plugin {

struct LUIDHash {
    size_t operator()(const LUID& luid) const {
        uint64_t combined = (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32)
                            | static_cast<uint32_t>(luid.LowPart);
        return std::hash<uint64_t>{}(combined);
    }
};

struct LUIDEqual {
    bool operator()(const LUID& a, const LUID& b) const {
        return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
    }
};

struct CachedGpuAdapter {
    std::string name;
    LUID        luid;
    uint64_t    dedicatedVramMB = 0;
    uint64_t    sharedVramMB    = 0;
};

/// DXGI 适配器枚举缓存与 PDH 计数器句柄。
///
/// 两者都是 **CpuGpuMonitor::Impl 的实例成员** (不再是函数级 static):
/// 枚举/PDH 采样都带 lazy 构建与可变的查询句柄, 同进程多实例并发查询会读写同一
/// 份 static (数据竞争); 且 PDH 句柄的创建/释放必须与实例生命周期配对,
/// 见多实例契约"禁止可变全局/函数级 static 保存实例状态"。
struct GpuAdapterCache {
    std::vector<CachedGpuAdapter> adapters;
    bool                          built = false;
};

struct SharedPdhContext {
    HQUERY   query             = nullptr;
    HCOUNTER hEngineCounter    = nullptr;
    HCOUNTER hProcMemDedicated = nullptr;
    HCOUNTER hProcMemShared    = nullptr;
    bool     initialized       = false;
    bool     initAttempted     = false;

    ~SharedPdhContext() {
        if (hEngineCounter) {
            PdhRemoveCounter(hEngineCounter);
        }
        if (hProcMemDedicated) {
            PdhRemoveCounter(hProcMemDedicated);
        }
        if (hProcMemShared) {
            PdhRemoveCounter(hProcMemShared);
        }
        if (query) {
            PdhCloseQuery(query);
        }
    }

    bool ensureInitialized() {
        if (initAttempted) {
            return initialized;
        }
        initAttempted = true;

        PDH_STATUS status = PdhOpenQueryW(nullptr, 0, &query);
        if (status != ERROR_SUCCESS) {
            XX_LOGW("CpuGpuMonitor: PdhOpenQuery failed, status={}", status);
            query = nullptr;
            return false;
        }

        PDH_STATUS engStatus
            = PdhAddCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &hEngineCounter);
        PDH_STATUS dedStatus = PdhAddCounterW(
            query,
            L"\\GPU Process Memory(*)\\Dedicated Usage",
            0,
            &hProcMemDedicated
        );
        PDH_STATUS shrStatus
            = PdhAddCounterW(query, L"\\GPU Process Memory(*)\\Shared Usage", 0, &hProcMemShared);

        initialized
            = (engStatus == ERROR_SUCCESS || dedStatus == ERROR_SUCCESS
               || shrStatus == ERROR_SUCCESS);
        return initialized;
    }
};

class CpuGpuMonitor::Impl {
public:

    Impl() {}

    ~Impl() = default;

    asio::awaitable<CpuGpuUsage> query() {
        CpuGpuUsage result;
        result.cpuCoreCount = cpuCoreCount_;

        queryMemoryInfo(result);

        bool needCpuInit = (prevTotalTime_ == 0 || prevIdleTime_ == 0);
        if (needCpuInit) {
            queryCpuUsage(result);
        }

        bool pdhAvailable = _pdh.ensureInitialized();
        if (pdhAvailable) {
            PdhCollectQueryData(_pdh.query);
        }

        if (needCpuInit || pdhAvailable) {
            asio::steady_timer timer(
                co_await asio::this_coro::executor,
                std::chrono::milliseconds(100)
            );
            co_await timer.async_wait(asio::use_awaitable);
        }

        queryCpuUsage(result);

        if (pdhAvailable) {
            co_await buildGpuCache();
            PdhCollectQueryData(_pdh.query);
            collectPdhGpuData(result);
        }

        co_return result;
    }

private:

    void queryCpuUsage(CpuGpuUsage& result) {
        FILETIME idleTime, kernelTime, userTime;
        if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
            XX_LOGE("CpuGpuMonitor: GetSystemTimes failed, error={}", GetLastError());
            return;
        }

        ULARGE_INTEGER idle, kernel, user;
        idle.LowPart    = idleTime.dwLowDateTime;
        idle.HighPart   = idleTime.dwHighDateTime;
        kernel.LowPart  = kernelTime.dwLowDateTime;
        kernel.HighPart = kernelTime.dwHighDateTime;
        user.LowPart    = userTime.dwLowDateTime;
        user.HighPart   = userTime.dwHighDateTime;

        ULONGLONG totalTime = kernel.QuadPart + user.QuadPart;

        if (prevTotalTime_ > 0 && totalTime > prevTotalTime_) {
            ULONGLONG idleDelta  = idle.QuadPart - prevIdleTime_;
            ULONGLONG totalDelta = totalTime - prevTotalTime_;

            if (totalDelta > 0) {
                result.cpuUsagePercent
                    = (1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta))
                      * 100.0;
                if (result.cpuUsagePercent < 0.0) {
                    result.cpuUsagePercent = 0.0;
                }
                if (result.cpuUsagePercent > 100.0) {
                    result.cpuUsagePercent = 100.0;
                }
            }
        }

        prevIdleTime_  = idle.QuadPart;
        prevTotalTime_ = totalTime;
    }

    void queryMemoryInfo(CpuGpuUsage& result) {
        MEMORYSTATUSEX memStatus = {};
        memStatus.dwLength       = sizeof(memStatus);

        if (!GlobalMemoryStatusEx(&memStatus)) {
            XX_LOGE("CpuGpuMonitor: GlobalMemoryStatusEx failed, error={}", GetLastError());
            return;
        }

        result.memory.totalPhysicalMB = memStatus.ullTotalPhys / (1024 * 1024);
        result.memory.usedPhysicalMB
            = (memStatus.ullTotalPhys - memStatus.ullAvailPhys) / (1024 * 1024);

        if (memStatus.ullTotalPhys > 0) {
            result.memory.usagePercent
                = static_cast<double>(memStatus.ullTotalPhys - memStatus.ullAvailPhys)
                  / static_cast<double>(memStatus.ullTotalPhys) * 100.0;
        }
    }

    asio::awaitable<void> buildGpuCache() {
        auto& cache = _adapterCache;
        if (cache.built) {
            co_return;
        }

        IDXGIFactory6* factory = nullptr;
        HRESULT        hr
            = CreateDXGIFactory1(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory));
        if (FAILED(hr) || !factory) {
            XX_LOGE(
                "CpuGpuMonitor: CreateDXGIFactory1 failed, hr=0x{:08X}",
                static_cast<unsigned>(hr)
            );
            cache.built = true;
            co_return;
        }

        UINT           adapterIndex = 0;
        IDXGIAdapter1* adapter      = nullptr;

        while (factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND) {
            DXGI_ADAPTER_DESC1 desc = {};
            hr                      = adapter->GetDesc1(&desc);
            if (SUCCEEDED(hr) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                CachedGpuAdapter info;
                info.luid            = desc.AdapterLuid;
                info.dedicatedVramMB = desc.DedicatedVideoMemory / (1024 * 1024);
                info.sharedVramMB    = desc.SharedSystemMemory / (1024 * 1024);

                int len = WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    desc.Description,
                    -1,
                    nullptr,
                    0,
                    nullptr,
                    nullptr
                );
                if (len > 0) {
                    info.name.resize(static_cast<size_t>(len) - 1);
                    WideCharToMultiByte(
                        CP_UTF8,
                        0,
                        desc.Description,
                        -1,
                        &info.name[0],
                        len,
                        nullptr,
                        nullptr
                    );
                }

                cache.adapters.push_back(std::move(info));
            }

            adapter->Release();
            adapterIndex++;
        }

        factory->Release();
        cache.built = true;
    }

    void collectPdhGpuData(CpuGpuUsage& result) {
        auto& cache = _adapterCache;
        if (!cache.built) {
            return;
        }

        auto& pdh = _pdh;

        std::unordered_map<LUID, double, LUIDHash, LUIDEqual>   gpuUsageMap;
        std::unordered_map<LUID, uint64_t, LUIDHash, LUIDEqual> gpuDedicatedUsedMap;
        std::unordered_map<LUID, uint64_t, LUIDHash, LUIDEqual> gpuSharedUsedMap;

        if (pdh.hEngineCounter) {
            collectEngineUtilization(pdh.hEngineCounter, gpuUsageMap);
        }
        if (pdh.hProcMemDedicated) {
            collectProcessMemory(pdh.hProcMemDedicated, gpuDedicatedUsedMap);
        }
        if (pdh.hProcMemShared) {
            collectProcessMemory(pdh.hProcMemShared, gpuSharedUsedMap);
        }

        for (const auto& cached : cache.adapters) {
            GpuInfo info;
            info.name            = cached.name;
            info.dedicatedVramMB = cached.dedicatedVramMB;
            info.sharedVramMB    = cached.sharedVramMB;

            auto itDedicated = gpuDedicatedUsedMap.find(cached.luid);
            if (itDedicated != gpuDedicatedUsedMap.end()) {
                info.dedicatedVramUsedMB = itDedicated->second / (1024 * 1024);
            }

            auto itShared = gpuSharedUsedMap.find(cached.luid);
            if (itShared != gpuSharedUsedMap.end()) {
                info.sharedVramUsedMB = itShared->second / (1024 * 1024);
            }

            auto itUsage = gpuUsageMap.find(cached.luid);
            if (itUsage != gpuUsageMap.end()) {
                info.usagePercent = itUsage->second;
            }

            result.gpus.push_back(std::move(info));
        }
    }

    void collectEngineUtilization(
        HCOUNTER                                               hCounter,
        std::unordered_map<LUID, double, LUIDHash, LUIDEqual>& outMap
    ) {
        DWORD      bufSize   = 0;
        DWORD      itemCount = 0;
        PDH_STATUS status
            = PdhGetFormattedCounterArrayW(hCounter, PDH_FMT_DOUBLE, &bufSize, &itemCount, nullptr);
        if (status != PDH_MORE_DATA || bufSize == 0) {
            return;
        }

        std::vector<BYTE> buf(bufSize);
        auto*             pItems = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        status
            = PdhGetFormattedCounterArrayW(hCounter, PDH_FMT_DOUBLE, &bufSize, &itemCount, pItems);
        if (status != ERROR_SUCCESS) {
            return;
        }

        for (DWORD i = 0; i < itemCount; ++i) {
            const wchar_t* instanceName = pItems[i].szName;
            if (!instanceName || !*instanceName) {
                continue;
            }

            LUID luid = {};
            if (parseEngineLuid(instanceName, luid)) {
                double val = pItems[i].FmtValue.doubleValue;
                auto   it  = outMap.find(luid);
                if (it == outMap.end() || val > it->second) {
                    outMap[luid] = val;
                }
            }
        }
    }

    void collectProcessMemory(
        HCOUNTER                                                 hCounter,
        std::unordered_map<LUID, uint64_t, LUIDHash, LUIDEqual>& outMap
    ) {
        DWORD      bufSize   = 0;
        DWORD      itemCount = 0;
        PDH_STATUS status
            = PdhGetFormattedCounterArrayW(hCounter, PDH_FMT_LARGE, &bufSize, &itemCount, nullptr);
        if (status != PDH_MORE_DATA || bufSize == 0) {
            return;
        }

        std::vector<BYTE> buf(bufSize);
        auto*             pItems = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        status
            = PdhGetFormattedCounterArrayW(hCounter, PDH_FMT_LARGE, &bufSize, &itemCount, pItems);
        if (status != ERROR_SUCCESS) {
            return;
        }

        for (DWORD i = 0; i < itemCount; ++i) {
            const wchar_t* instanceName = pItems[i].szName;
            if (!instanceName || !*instanceName) {
                continue;
            }

            LUID luid = {};
            if (parseProcessMemoryLuid(instanceName, luid)) {
                outMap[luid] += pItems[i].FmtValue.largeValue;
            }
        }
    }

    static bool parseEngineLuid(const wchar_t* instanceName, LUID& outLuid) {
        std::wstring_view name(instanceName);

        auto luidPos = name.find(L"_luid_");
        if (luidPos == std::wstring_view::npos) {
            return false;
        }

        std::wstring_view luidStr = name.substr(luidPos + 6);

        auto physPos = luidStr.find(L"_phys_");
        if (physPos == std::wstring_view::npos) {
            return false;
        }

        luidStr = luidStr.substr(0, physPos);

        auto underscorePos = luidStr.find(L'_');
        if (underscorePos == std::wstring_view::npos) {
            return false;
        }

        std::wstring highStr{luidStr.substr(0, underscorePos)};
        std::wstring lowStr{luidStr.substr(underscorePos + 1)};

        outLuid.HighPart = static_cast<LONG>(wcstoul(highStr.c_str(), nullptr, 16));
        outLuid.LowPart  = static_cast<LONG>(wcstoul(lowStr.c_str(), nullptr, 16));
        return true;
    }

    static bool parseProcessMemoryLuid(const wchar_t* instanceName, LUID& outLuid) {
        std::wstring_view name(instanceName);

        auto luidPos = name.find(L"_luid_");
        if (luidPos == std::wstring_view::npos) {
            return false;
        }

        std::wstring_view luidStr = name.substr(luidPos + 6);

        auto underscorePos = luidStr.find(L'_');
        if (underscorePos == std::wstring_view::npos) {
            return false;
        }

        std::wstring highStr{luidStr.substr(0, underscorePos)};
        std::wstring lowStr{luidStr.substr(underscorePos + 1)};

        outLuid.HighPart = static_cast<LONG>(wcstoul(highStr.c_str(), nullptr, 16));
        outLuid.LowPart  = static_cast<LONG>(wcstoul(lowStr.c_str(), nullptr, 16));
        return true;
    }

    /// 查询 CPU 逻辑核心数 (含超线程; 取不到返回 0)
    /// - 用 GetActiveProcessorCount(ALL_PROCESSOR_GROUPS): 逻辑核超过 64 时系统会划分
    ///   多个处理器组, GetSystemInfo 只报告当前组的数量, 这里需要整机总数
    static uint32_t queryCpuCoreCount() {
        DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (count == 0) {
            SYSTEM_INFO sysInfo = {};
            GetNativeSystemInfo(&sysInfo);
            count = sysInfo.dwNumberOfProcessors;
        }
        return static_cast<uint32_t>(count);
    }

    ULONGLONG prevIdleTime_  = 0;
    ULONGLONG prevTotalTime_ = 0;

    /// CPU 逻辑核心数 (构造时取一次; 展示口径与 GetSystemTimes 的整机利用率一致)
    uint32_t cpuCoreCount_ = queryCpuCoreCount();

    /// GPU 适配器枚举缓存 (每实例一份; 见 [GpuAdapterCache] 说明)
    GpuAdapterCache _adapterCache;
    /// PDH 查询句柄与计数器 (每实例一份, 随实例析构释放)
    SharedPdhContext _pdh;
};

CpuGpuMonitor::CpuGpuMonitor() :
    impl_(std::make_unique<Impl>()) {}

CpuGpuMonitor::~CpuGpuMonitor() = default;

asio::awaitable<CpuGpuUsage> CpuGpuMonitor::query() {
    co_return co_await impl_->query();
}

} // namespace agentxx_system_monitor_plugin

#elif XX_IS_LINUX_D || XX_IS_ANDROID_D

#include "asio/random_access_file.hpp"
#include "asio/read.hpp"
#include "asio/read_at.hpp"
#include "asio/read_until.hpp"
#include "asio/redirect_error.hpp"
#include "asio/registered_buffer.hpp"
#include "asio/stream_file.hpp"
#include "asio/use_awaitable.hpp"
#include "system_monitor_plugin.h"
#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace agentxx_system_monitor_plugin {

/// 原 libagentxx utilxx_base::removeBetweenSpace 的本地拷贝 (插件不链接 libagentxx):
/// 移除字符串首尾空白 (可选移除换行), 供解析 sysfs/proc 内容使用
[[nodiscard]] inline std::string removeBetweenSpace(
    std::string_view str,
    bool             removeLine = true,
    bool             subLeft    = true,
    bool             subRight   = true
) {
    if (str.empty()) {
        return std::string{str};
    }

    int left  = 0;
    int right = static_cast<int>(str.size()) - 1;

    auto isSpace = [removeLine](char c) {
        if (removeLine) {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
        }
        return c == ' ' || c == '\t';
    };

    if (subLeft) {
        while (left < right + 1 && isSpace(str[static_cast<size_t>(left)])) {
            ++left;
        }
    }
    if (subRight) {
        while (right >= left && isSpace(str[static_cast<size_t>(right)])) {
            --right;
        }
    }

    if (right < left) {
        return std::string{};
    }
    return std::string{str.substr(static_cast<size_t>(left), static_cast<size_t>(right - left + 1))
    };
}

struct LinuxGpuCacheEntry {
    std::string devicePath;
    uint32_t    vendorId = 0;
    std::string name;
    uint64_t    dedicatedVramMB = 0;
    bool        isAmd           = false;
    bool        isNvidia        = false;
};

/// Linux GPU 枚举结果缓存。
///
/// 属于 **CpuGpuMonitor::Impl 实例成员**: 枚举本身读写 sysfs 且带 lazy 构建,
/// 若放在函数级 static 中, 同进程多个插件实例 (各自线程/io_context) 并发查询
/// 会同时读写同一份缓存 (数据竞争, TSan 可复现); 同时违反多实例契约
/// 契约"禁止可变全局/函数级 static 保存实例状态"。
struct LinuxGpuCache {
    std::vector<LinuxGpuCacheEntry> entries;
    bool                            built = false;
};

class CpuGpuMonitor::Impl {
public:

    Impl() {}

    ~Impl() {}

    struct CpuTimes {
        uint64_t total = 0;
        uint64_t idle  = 0;
    };

    asio::awaitable<CpuGpuUsage> query() {
        CpuGpuUsage result;
        result.cpuCoreCount = cpuCoreCount_;

        CpuTimes oldSample = _sample;
        if (_sample.total == 0 || _sample.idle == 0) {
            oldSample = co_await readCpuStat();
            asio::steady_timer timer(
                co_await asio::this_coro::executor,
                std::chrono::milliseconds(100)
            );
            co_await timer.async_wait(asio::use_awaitable);
        }

        co_await queryMemoryInfo(result);
        co_await queryGpuInfo(result);

        {
            _sample = co_await readCpuStat();
            if (_sample.total > oldSample.total) {
                uint64_t totalDelta = _sample.total - oldSample.total;
                uint64_t idleDelta  = _sample.idle - oldSample.idle;

                if (totalDelta > 0) {
                    result.cpuUsagePercent
                        = (1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta))
                          * 100.0;
                    if (result.cpuUsagePercent < 0.0) {
                        result.cpuUsagePercent = 0.0;
                    }
                    if (result.cpuUsagePercent > 100.0) {
                        result.cpuUsagePercent = 100.0;
                    }
                }
            }
        }

        co_return result;
    }

protected:

    CpuTimes _sample;
    /// CPU 逻辑核心数 (构造时取一次; 展示口径与 /proc/stat 的整机利用率一致)
    uint32_t cpuCoreCount_ = queryCpuCoreCount();
    /// GPU 枚举缓存 (每实例一份; 见 [LinuxGpuCache] 的说明)
    LinuxGpuCache _gpuCache;

    /// 查询 CPU 逻辑核心数 (含超线程; 取不到返回 0)
    /// - 取**在线**核数: 离线核不参与 /proc/stat 的整机时间累计, 显示在线数量才与利用率口径一致
    static uint32_t queryCpuCoreCount() {
        long online = ::sysconf(_SC_NPROCESSORS_ONLN);
        if (online > 0) {
            return static_cast<uint32_t>(online);
        }
        unsigned int hw = std::thread::hardware_concurrency();
        return hw > 0 ? static_cast<uint32_t>(hw) : 0;
    }

    static asio::awaitable<std::string> readFileContent(std::string_view path) {
#if ASIO_HAS_FILE || BOOST_ASIO_HAS_FILE
        /// 文件异步 I/O 可用时异步读取, 避免同步读盘阻塞事件循环
        /// (可用性含运行时 io_uring 探测, 见 utilxx_base::isAsyncFileIoSupported)
        if (utilxx_base::isAsyncFileIoSupported()) {
            auto                      executor = co_await asio::this_coro::executor;
            asio::stream_file         stream{executor};
            boost::system::error_code errCode;
            stream.open(std::string{path}, asio::stream_file::read_only, errCode);
            if (!stream.is_open()) {
                co_return "";
            }
            std::string data;
            co_await asio::async_read(
                stream,
                asio::dynamic_buffer(data),
                asio::transfer_all(),
                asio::redirect_error(asio::use_awaitable, errCode)
            );
            stream.close();
            if (errCode && errCode != asio::error::eof) {
                co_return "";
            }
            co_return data;
        }
#endif
        /// 同步兜底读取 (文件异步 I/O 不可用)
        std::ifstream stream;
        stream.open(std::string{path});
        if (!stream) {
            co_return "";
        }

        auto result
            = std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        stream.close();
        co_return result;
    }

    static asio::awaitable<CpuTimes> readCpuStat() {
        CpuTimes    times;
        std::string content = co_await readFileContent("/proc/stat");
        if (content.empty()) {
            co_return times;
        }

        std::istringstream iss(content);
        std::string        cpuLabel;
        uint64_t           user = 0, nice = 0, system = 0, idle = 0, iowait = 0;
        uint64_t           irq = 0, softirq = 0, steal = 0;
        iss >> cpuLabel >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

        times.total = user + nice + system + idle + iowait + irq + softirq + steal;
        times.idle  = idle + iowait;
        co_return times;
    }

    asio::awaitable<void> queryMemoryInfo(CpuGpuUsage& result) {
        std::string content = co_await readFileContent("/proc/meminfo");
        if (content.empty()) {
            co_return;
        }

        std::istringstream memFile(content);
        std::string        line;
        uint64_t           memTotalKB     = 0;
        uint64_t           memAvailableKB = 0;

        while (std::getline(memFile, line)) {
            if (line.rfind("MemTotal:", 0) == 0) {
                std::sscanf(line.c_str(), "MemTotal: %lu kB", &memTotalKB);
            } else if (line.rfind("MemAvailable:", 0) == 0) {
                std::sscanf(line.c_str(), "MemAvailable: %lu kB", &memAvailableKB);
            }

            if (memTotalKB > 0 && memAvailableKB > 0) {
                break;
            }
        }

        if (memTotalKB > 0) {
            result.memory.totalPhysicalMB = memTotalKB / 1024;
            uint64_t usedKB               = memTotalKB - memAvailableKB;
            result.memory.usedPhysicalMB  = usedKB / 1024;
            result.memory.usagePercent
                = static_cast<double>(usedKB) / static_cast<double>(memTotalKB) * 100.0;
        }
    }

    asio::awaitable<void> queryGpuInfo(CpuGpuUsage& result) {
        if (!_gpuCache.built) {
            co_await buildGpuCache();
        }

        for (const auto& entry : _gpuCache.entries) {
            GpuInfo info;
            info.name            = entry.name;
            info.dedicatedVramMB = entry.dedicatedVramMB;

            if (entry.isAmd) {
                co_await queryAmdGpuUsage(info, entry.devicePath);
            }

            result.gpus.push_back(std::move(info));
        }
    }

    asio::awaitable<void> buildGpuCache() {
        auto& cache = _gpuCache;

        for (int cardIdx = 0;; ++cardIdx) {
            auto devicePath = fmt::format("/sys/class/drm/card{}/device", cardIdx);

            std::string vendorContent
                = co_await readFileContent(fmt::format("{}/vendor", devicePath));
            if (vendorContent.empty()) {
                break;
            }

            LinuxGpuCacheEntry entry;
            entry.devicePath = devicePath;
            entry.vendorId   = parseHexSysfs(vendorContent);

            co_await readSysfsString(fmt::format("{}/product_name", devicePath), entry.name);

            if (entry.vendorId == 0x1002) {
                entry.isAmd = true;
                co_await readSysfsUint64(
                    fmt::format("{}/mem_info_vram_total", devicePath),
                    entry.dedicatedVramMB
                );
                entry.dedicatedVramMB /= (1024 * 1024);
            } else if (entry.vendorId == 0x10de) {
                entry.isNvidia = true;
                co_await readNvidiaInfo(entry);
            }

            if (entry.name.empty()) {
                entry.name = fmt::format("GPU {}", cardIdx);
            }

            cache.entries.push_back(std::move(entry));
        }
        cache.built = true;
    }

    static asio::awaitable<void> queryAmdGpuUsage(GpuInfo& info, std::string_view devicePath) {
        co_await readSysfsUint64(
            fmt::format("{}/mem_info_vram_used", devicePath),
            info.dedicatedVramUsedMB
        );
        info.dedicatedVramUsedMB /= (1024 * 1024);

        uint64_t gpuBusy = 0;
        co_await readSysfsUint64(fmt::format("{}/gpu_busy_percent", devicePath), gpuBusy);
        info.usagePercent = static_cast<double>(gpuBusy);

        if (info.usagePercent == 0.0 && info.dedicatedVramMB > 0) {
            info.usagePercent = static_cast<double>(info.dedicatedVramUsedMB)
                                / static_cast<double>(info.dedicatedVramMB) * 100.0;
        }
    }

    static asio::awaitable<void> readNvidiaInfo(LinuxGpuCacheEntry& entry) {
        std::string gpusDirContent = co_await readFileContent("/proc/driver/nvidia/gpus");
        if (gpusDirContent.empty()) {
            co_return;
        }

        // 先同步收集目录项并立即关闭 DIR, 避免跨 co_await 持有 DIR* 导致协程取消时 fd 泄漏
        std::vector<std::string> gpuDirs;
        {
            DIR* dir = opendir("/proc/driver/nvidia/gpus");
            if (!dir) {
                co_return;
            }
            struct dirent* ent;
            while ((ent = readdir(dir)) != nullptr) {
                if (ent->d_name[0] == '.') {
                    continue;
                }
                gpuDirs.emplace_back(ent->d_name);
            }
            closedir(dir);
        }
        if (gpuDirs.empty()) {
            co_return;
        }

        auto parseInformation = [&](std::string_view infoContent) {
            std::istringstream infoFile(infoContent);
            std::string        line;
            while (std::getline(infoFile, line)) {
                if (line.rfind("Model:", 0) == 0) {
                    size_t pos = line.find(':');
                    if (pos != std::string::npos) {
                        entry.name = removeBetweenSpace(std::string_view{line}.substr(pos + 1));
                    }
                } else if (line.rfind("Video Memory:", 0) == 0) {
                    size_t pos = line.find(':');
                    if (pos != std::string::npos) {
                        auto memStr = removeBetweenSpace(std::string_view{line}.substr(pos + 1));
                        uint64_t totalMiB = 0;
                        std::sscanf(memStr.c_str(), "%lu MiB", &totalMiB);
                        entry.dedicatedVramMB = totalMiB;
                    }
                }
            }
        };

        // 多 GPU: 优先按 PCI 地址精确匹配本 card 对应的 GPU 目录
        // entry.devicePath 形如 /sys/class/drm/cardN/device, 符号链接目标的 basename 即 PCI 地址
        std::error_code ec;
        auto            symlinkTarget = std::filesystem::read_symlink(entry.devicePath, ec);
        std::string     pciAddr       = ec ? std::string{} : symlinkTarget.filename().string();
        if (!pciAddr.empty()) {
            std::string infoContent = co_await readFileContent(
                fmt::format("/proc/driver/nvidia/gpus/{}/information", pciAddr)
            );
            if (!infoContent.empty()) {
                parseInformation(infoContent);
                co_return;
            }
        }

        // 退化: 遍历各 GPU 目录取第一个有效项 (保持旧行为, 兼容无法解析 PCI 地址的情况)
        for (const auto& name : gpuDirs) {
            std::string infoContent = co_await readFileContent(
                fmt::format("/proc/driver/nvidia/gpus/{}/information", name)
            );
            if (infoContent.empty()) {
                continue;
            }
            parseInformation(infoContent);
            break;
        }
    }

    static uint32_t parseHexSysfs(std::string_view line) {
        if (line.size() >= 2 && line[0] == '0' && (line[1] == 'x' || line[1] == 'X')) {
            line = line.substr(2);
        }
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line = line.substr(0, line.size() - 1);
        }
        std::string hex(line);
        uint32_t    val = 0;
        std::sscanf(hex.c_str(), "%x", &val);
        return val;
    }

    static asio::awaitable<void> readSysfsString(std::string_view path, std::string& out) {
        std::string content = co_await readFileContent(path);
        if (!content.empty()) {
            out = removeBetweenSpace(content);
        }
    }

    static asio::awaitable<void> readSysfsUint64(std::string_view path, uint64_t& out) {
        std::string content = co_await readFileContent(path);
        if (!content.empty()) {
            std::istringstream iss(content);
            iss >> out;
        }
    }
};

CpuGpuMonitor::CpuGpuMonitor() :
    impl_(std::make_unique<Impl>()) {}

CpuGpuMonitor::~CpuGpuMonitor() = default;

asio::awaitable<CpuGpuUsage> CpuGpuMonitor::query() {
    co_return co_await impl_->query();
}

} // namespace agentxx_system_monitor_plugin

#elif XX_IS_MACOS_D

// macOS 数据来源 (均为内核接口/IOKit 属性读取, macOS 下 asio 没有文件异步 I/O
// 可用, 故与 Linux 分支的 stream_file 异步读不同, 这里直接同步读取):
// - CPU: mach host_statistics(HOST_CPU_LOAD_INFO) 的累计时间片, 取两次差值
// - 内存: host_statistics64(HOST_VM_INFO64) 页统计 + sysctl hw.memsize
// - GPU: IOKit 的 IOAccelerator 服务 (每张识别到的 GPU 一个), 读其
//   PerformanceStatistics 字典得到利用率与显存; Apple Silicon 为统一内存,
//   只有"占用的系统内存"字段, 没有独立显存字段
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/host_info.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/vm_statistics.h>
#include <sys/sysctl.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace agentxx_system_monitor_plugin {

/// IOKit 主端口常量
/// - macOS 12 SDK 起 `kIOMasterPortDefault` 改名为 `kIOMainPortDefault`, 按编译
///   所用 SDK 版本选择 (新 SDK 上继续用旧名会触发弃用告警)
[[nodiscard]] inline mach_port_t macIokitMainPort() {
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 120000
    return kIOMainPortDefault;
#else
    return kIOMasterPortDefault;
#endif
}

/// 整机 CPU 累计时间片与其中的空闲时间片
struct MacCpuTicks {
    uint64_t total = 0;
    uint64_t idle  = 0;
};

/// 单个 GPU 的枚举结果
struct MacGpuEntry {
    std::string name;
    /// IOAccelerator 服务句柄 (所有权来自 IOIteratorNext, 随缓存析构释放)
    io_service_t service = IO_OBJECT_NULL;
};

/// GPU 枚举结果缓存。
///
/// 属于 **CpuGpuMonitor::Impl 实例成员**: 缓存持 IOKit 服务句柄且 lazy 构建,
/// 若放在函数级 static 中, 同进程多个插件实例 (各自线程/io_context) 并发查询会
/// 同时读写同一份缓存 (数据竞争), 句柄释放时机也不再跟随实例 (违反多实例契约
/// "禁止可变全局/函数级 static 保存实例状态")。
struct MacGpuCache {
    std::vector<MacGpuEntry> entries;
    bool                     built = false;

    MacGpuCache()                              = default;
    MacGpuCache(const MacGpuCache&)            = delete;
    MacGpuCache& operator=(const MacGpuCache&) = delete;
    MacGpuCache(MacGpuCache&&)                 = delete;
    MacGpuCache& operator=(MacGpuCache&&)      = delete;

    ~MacGpuCache() {
        releaseAll();
    }

    /// 释放已保留的服务句柄并清空列表
    void releaseAll() {
        for (auto& entry : entries) {
            if (entry.service != IO_OBJECT_NULL) {
                IOObjectRelease(entry.service);
                entry.service = IO_OBJECT_NULL;
            }
        }
        entries.clear();
    }
};

class CpuGpuMonitor::Impl {
public:

    Impl() {
        /// mach_host_self() 每次调用都会增加发送权引用计数, 这里保留一份供
        /// 实例存续期复用 (析构时释放)
        host_     = mach_host_self();
        pageSize_ = queryPageSize(host_);
    }

    ~Impl() {
        if (host_ != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), host_);
            host_ = MACH_PORT_NULL;
        }
    }

    asio::awaitable<CpuGpuUsage> query() {
        CpuGpuUsage result;
        result.cpuCoreCount = cpuCoreCount_;

        /// 首轮没有基线: 先采一次, 等 100ms 后再采一次算出时间差
        /// (与 Windows/Linux 分支同一口径: 整机利用率)
        MacCpuTicks oldTicks = _cpuTicks;
        if (_cpuTicks.total == 0) {
            readCpuTicks(oldTicks);
            asio::steady_timer timer(
                co_await asio::this_coro::executor,
                std::chrono::milliseconds(100)
            );
            co_await timer.async_wait(asio::use_awaitable);
        }

        queryMemoryInfo(result);
        queryGpuInfo(result);

        MacCpuTicks newTicks;
        if (readCpuTicks(newTicks)) {
            if (newTicks.total > oldTicks.total) {
                uint64_t totalDelta = newTicks.total - oldTicks.total;
                uint64_t idleDelta
                    = newTicks.idle > oldTicks.idle ? newTicks.idle - oldTicks.idle : 0;
                if (totalDelta > 0) {
                    result.cpuUsagePercent
                        = (1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta))
                          * 100.0;
                    result.cpuUsagePercent = clampPercent(result.cpuUsagePercent);
                }
            }
            _cpuTicks = newTicks;
        }

        co_return result;
    }

protected:

    /// 系统 host 端口 (构造时保留一份引用, 析构释放)
    host_t host_ = MACH_PORT_NULL;
    /// CPU 累计时间片基线 (每轮刷新; total == 0 表示尚无基线)
    MacCpuTicks _cpuTicks;
    /// CPU 逻辑核心数 (构造时取一次; 与 host_statistics 的整机利用率同一口径)
    uint32_t cpuCoreCount_ = queryCpuCoreCount();
    /// 物理内存总量 MB (构造时取一次: sysctl hw.memsize)
    uint64_t totalPhysicalMemoryMB_ = queryTotalPhysicalMemoryMB();
    /// 内存页大小 字节 (构造时取一次: host_page_size)
    uint64_t pageSize_ = 0;
    /// GPU 枚举缓存 (每实例一份; 见 [MacGpuCache] 的说明)
    MacGpuCache _gpuCache;

    static double clampPercent(double value) {
        if (value < 0.0) {
            return 0.0;
        }
        if (value > 100.0) {
            return 100.0;
        }
        return value;
    }

    /// 统计值 (字节) 规整为非负整数; 负数/NaN 一律按 0 处理, 超出范围取上限
    static uint64_t toBytes(double value) {
        if (!(value > 0.0)) {
            return 0;
        }
        const double maxBytes = static_cast<double>(std::numeric_limits<uint64_t>::max());
        if (value > maxBytes) {
            return std::numeric_limits<uint64_t>::max();
        }
        return static_cast<uint64_t>(value);
    }

    static uint64_t bytesToMb(uint64_t bytes) {
        return bytes / (1024 * 1024);
    }

    /// 查询 CPU 逻辑核心数 (取不到返回 0)
    static uint32_t queryCpuCoreCount() {
        int    value = 0;
        size_t len   = sizeof(value);
        if (sysctlbyname("hw.logicalcpu", &value, &len, nullptr, 0) == 0 && value > 0) {
            return static_cast<uint32_t>(value);
        }

        value = 0;
        len   = sizeof(value);
        if (sysctlbyname("hw.ncpu", &value, &len, nullptr, 0) == 0 && value > 0) {
            return static_cast<uint32_t>(value);
        }

        unsigned int hw = std::thread::hardware_concurrency();
        return hw > 0 ? static_cast<uint32_t>(hw) : 0;
    }

    /// 查询物理内存总量 MB (取不到返回 0)
    static uint64_t queryTotalPhysicalMemoryMB() {
        uint64_t memSize = 0;
        size_t   len     = sizeof(memSize);
        if (sysctlbyname("hw.memsize", &memSize, &len, nullptr, 0) != 0 || memSize == 0) {
            XX_LOGW("CpuGpuMonitor: sysctl hw.memsize failed");
            return 0;
        }
        return memSize / (1024 * 1024);
    }

    /// 查询内存页大小 (字节, 取不到返回 0)
    static uint64_t queryPageSize(host_t host) {
        vm_size_t pageSize = 0;
        if (host == MACH_PORT_NULL || host_page_size(host, &pageSize) != KERN_SUCCESS
            || pageSize == 0) {
            XX_LOGW("CpuGpuMonitor: host_page_size failed");
            return 0;
        }
        return static_cast<uint64_t>(pageSize);
    }

    /// 读取整机 CPU 累计时间片 (host_statistics 的 HOST_CPU_LOAD_INFO)
    /// - cpu_ticks 是内核累计的调度时间片计数 (单位与秒不直接对应, 只用于求比值)
    /// - 该 flavor 由内核把固定大小结构拷入调用方缓冲区, 调用方无需释放额外内存
    bool readCpuTicks(MacCpuTicks& out) const {
        if (host_ == MACH_PORT_NULL) {
            return false;
        }

        host_cpu_load_info_data_t info;
        mach_msg_type_number_t    count = HOST_CPU_LOAD_INFO_COUNT;
        kern_return_t             kr    = host_statistics(
            host_,
            HOST_CPU_LOAD_INFO,
            reinterpret_cast<host_info_t>(&info),
            &count
        );
        if (kr != KERN_SUCCESS) {
            XX_LOGW("CpuGpuMonitor: host_statistics(HOST_CPU_LOAD_INFO) failed, kr={}", kr);
            return false;
        }

        out.total = static_cast<uint64_t>(info.cpu_ticks[CPU_STATE_USER])
                    + static_cast<uint64_t>(info.cpu_ticks[CPU_STATE_SYSTEM])
                    + static_cast<uint64_t>(info.cpu_ticks[CPU_STATE_IDLE])
                    + static_cast<uint64_t>(info.cpu_ticks[CPU_STATE_NICE]);
        out.idle  = static_cast<uint64_t>(info.cpu_ticks[CPU_STATE_IDLE]);
        return true;
    }

    void queryMemoryInfo(CpuGpuUsage& result) const {
        if (host_ == MACH_PORT_NULL || totalPhysicalMemoryMB_ == 0 || pageSize_ == 0) {
            return;
        }

        vm_statistics64_data_t vmStat;
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        kern_return_t          kr    = host_statistics64(
            host_,
            HOST_VM_INFO64,
            reinterpret_cast<host_info64_t>(&vmStat),
            &count
        );
        if (kr != KERN_SUCCESS) {
            XX_LOGW("CpuGpuMonitor: host_statistics64(HOST_VM_INFO64) failed, kr={}", kr);
            return;
        }

        const uint64_t totalBytes = totalPhysicalMemoryMB_ * 1024 * 1024;
        /// 可用内存 (近似 Linux /proc/meminfo 的 MemAvailable): 空闲页 + 非活跃页
        /// + 推测页 + 可清除页 —— 这些页在被新的分配申请到时都可由内核直接回收
        const uint64_t availableBytes
            = (static_cast<uint64_t>(vmStat.free_count)
               + static_cast<uint64_t>(vmStat.inactive_count)
               + static_cast<uint64_t>(vmStat.speculative_count)
               + static_cast<uint64_t>(vmStat.purgeable_count))
              * pageSize_;
        const uint64_t usedBytes = availableBytes < totalBytes ? totalBytes - availableBytes : 0;

        result.memory.totalPhysicalMB = totalPhysicalMemoryMB_;
        result.memory.usedPhysicalMB  = usedBytes / (1024 * 1024);
        result.memory.usagePercent
            = static_cast<double>(usedBytes) / static_cast<double>(totalBytes) * 100.0;
    }

    void queryGpuInfo(CpuGpuUsage& result) {
        /// 缓存为空时每轮重新枚举: 覆盖枚举时 GPU 尚未注册、外接显卡后接入的情况
        /// (IOKit 匹配查询开销很小; 有卡时只在首次枚举)
        if (!_gpuCache.built || _gpuCache.entries.empty()) {
            buildGpuCache();
        }

        for (const auto& entry : _gpuCache.entries) {
            GpuInfo info;
            info.name = entry.name;
            readGpuStats(entry.service, info);
            result.gpus.push_back(std::move(info));
        }
    }

    /// 枚举 IOAccelerator 服务 (每张被系统识别到的 GPU 一个), 记录名称与服务句柄
    void buildGpuCache() {
        auto& cache = _gpuCache;
        cache.releaseAll();
        cache.built = true;

        CFMutableDictionaryRef matchDict = IOServiceMatching("IOAccelerator");
        if (!matchDict) {
            XX_LOGW("CpuGpuMonitor: IOServiceMatching(IOAccelerator) failed");
            return;
        }

        /// 匹配字典由 IOServiceGetMatchingServices 消费 (成功/失败都由 IOKit 释放)
        io_iterator_t serviceIter = IO_OBJECT_NULL;
        kern_return_t kr = IOServiceGetMatchingServices(macIokitMainPort(), matchDict, &serviceIter);
        if (kr != KERN_SUCCESS || serviceIter == IO_OBJECT_NULL) {
            XX_LOGW("CpuGpuMonitor: IOServiceGetMatchingServices failed, kr={}", kr);
            return;
        }

        io_service_t service = IO_OBJECT_NULL;
        while ((service = IOIteratorNext(serviceIter)) != IO_OBJECT_NULL) {
            MacGpuEntry entry;
            entry.service = service; ///< 迭代器返回即持有引用, 直接保留给缓存
            entry.name    = readGpuName(service);
            if (entry.name.empty()) {
                entry.name = fmt::format("GPU {}", cache.entries.size());
            }
            cache.entries.push_back(std::move(entry));
        }
        IOObjectRelease(serviceIter);
    }

    /// 读取 GPU 名称
    /// - 先找服务自身与其各级父节点的 `model` 属性: Apple Silicon 上是芯片名
    ///   ("Apple M1"), 独显/核显上多挂在父级 IOPCIDevice (产品名)
    /// - `model` 可能是 CFString, 也可能是 NUL 结尾字符串的 CFData
    /// - 取不到时退化用注册表节点名 (如 "AMDRadeonX6000"), 便于区分是多卡中的哪张
    static std::string readGpuName(io_service_t service) {
        CFTypeRef model = IORegistryEntrySearchCFProperty(
            service,
            kIOServicePlane,
            CFSTR("model"),
            kCFAllocatorDefault,
            kIORegistryIterateRecursively | kIORegistryIterateParents
        );
        std::string name = cfTextToUtf8(model);
        if (model) {
            CFRelease(model);
        }
        if (!name.empty()) {
            return name;
        }

        io_name_t entryName = {0};
        if (IORegistryEntryGetName(service, entryName) == KERN_SUCCESS) {
            return std::string{entryName};
        }
        return {};
    }

    /// 读取单个 GPU 的性能统计 (IOAccelerator 的 `PerformanceStatistics` 字典)
    ///
    /// 字段口径 (不同厂商/架构给出的字段不同):
    /// - `Device Utilization %`: GPU 利用率 (0~100)
    /// - `vramTotalBytes` / `vramUsedBytes` / `vramFreeBytes`: **独立显存**;
    ///   总量字段缺失时按"已用 + 空闲"推算 (Intel/AMD 独显与划分出显存的核显)
    /// - `In use system memory` / `Alloc system memory`: GPU 占用的**系统内存**
    ///   (Apple Silicon 统一内存没有独立显存, 只有这一组字段; 独显通常也会给出)
    void readGpuStats(io_service_t service, GpuInfo& info) const {
        CFTypeRef statsRef = IORegistryEntryCreateCFProperty(
            service,
            CFSTR("PerformanceStatistics"),
            kCFAllocatorDefault,
            0
        );
        if (!statsRef || CFGetTypeID(statsRef) != CFDictionaryGetTypeID()) {
            if (statsRef) {
                CFRelease(statsRef);
            }
            /// 服务已失效 (GPU 复位/驱动重载等): 本轮该卡回退为空数据,
            /// 下轮缓存为空时会重新枚举
            return;
        }
        CFDictionaryRef stats = static_cast<CFDictionaryRef>(statsRef);

        double     utilization    = 0.0;
        const bool hasUtilization = cfDictNumber(stats, CFSTR("Device Utilization %"), utilization);

        double     vramTotal    = 0.0;
        double     vramUsed     = 0.0;
        double     vramFree     = 0.0;
        const bool hasVramTotal = cfDictNumber(stats, CFSTR("vramTotalBytes"), vramTotal);
        const bool hasVramUsed  = cfDictNumber(stats, CFSTR("vramUsedBytes"), vramUsed);
        const bool hasVramFree  = cfDictNumber(stats, CFSTR("vramFreeBytes"), vramFree);

        double systemMemoryUsed    = 0.0;
        bool   hasSystemMemoryUsed = cfDictNumber(stats, CFSTR("In use system memory"), systemMemoryUsed);
        if (!hasSystemMemoryUsed) {
            hasSystemMemoryUsed
                = cfDictNumber(stats, CFSTR("Alloc system memory"), systemMemoryUsed);
        }

        if (hasVramTotal || hasVramUsed || hasVramFree) {
            const uint64_t totalBytes = hasVramTotal
                                            ? toBytes(vramTotal)
                                            : toBytes(vramUsed) + toBytes(vramFree);
            info.dedicatedVramMB      = bytesToMb(totalBytes);
            info.dedicatedVramUsedMB  = hasVramUsed ? bytesToMb(toBytes(vramUsed)) : 0;
        }

        if (hasSystemMemoryUsed) {
            /// 共享/统一内存口径: 总量取物理内存, 已用量取 GPU 当前占用的系统内存
            uint64_t usedBytes  = toBytes(systemMemoryUsed);
            uint64_t limitBytes = totalPhysicalMemoryMB_ * 1024 * 1024;
            if (limitBytes > 0 && usedBytes > limitBytes) {
                usedBytes = limitBytes; ///< 防御: 统计异常时不超过物理内存
            }
            info.sharedVramMB     = totalPhysicalMemoryMB_;
            info.sharedVramUsedMB = bytesToMb(usedBytes);
        }

        if (hasUtilization) {
            info.usagePercent = clampPercent(utilization);
        } else if (info.dedicatedVramMB > 0 && info.dedicatedVramUsedMB > 0) {
            /// 无利用率字段时按独立显存占用比例估算 (与 Linux AMD 分支同一口径)
            info.usagePercent = clampPercent(
                static_cast<double>(info.dedicatedVramUsedMB)
                / static_cast<double>(info.dedicatedVramMB) * 100.0
            );
        }

        CFRelease(statsRef);
    }

    /// CFString → UTF-8 字符串 (失败返回空串)
    static std::string cfStringToUtf8(CFStringRef str) {
        if (!str) {
            return {};
        }
        const CFIndex len = CFStringGetLength(str);
        if (len <= 0) {
            return {};
        }
        const CFIndex maxSize = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
        std::string   out(static_cast<size_t>(maxSize), '\0');
        if (!CFStringGetCString(str, out.data(), maxSize, kCFStringEncodingUTF8)) {
            return {};
        }
        out.resize(std::strlen(out.c_str()));
        return out;
    }

    /// CFString / CFData (NUL 结尾字符串) → UTF-8 字符串; 其他类型返回空串
    static std::string cfTextToUtf8(CFTypeRef ref) {
        if (!ref) {
            return {};
        }

        const CFTypeID typeId = CFGetTypeID(ref);
        if (typeId == CFStringGetTypeID()) {
            return cfStringToUtf8(static_cast<CFStringRef>(ref));
        }
        if (typeId == CFDataGetTypeID()) {
            CFDataRef   data = static_cast<CFDataRef>(ref);
            const char* ptr  = reinterpret_cast<const char*>(CFDataGetBytePtr(data));
            const CFIndex len = CFDataGetLength(data);
            if (!ptr || len <= 0) {
                return {};
            }
            size_t size = static_cast<size_t>(len);
            while (size > 0 && ptr[size - 1] == '\0') {
                --size;
            }
            return std::string(ptr, size);
        }
        return {};
    }

    /// 读取 CFDictionary 中的数值字段 (CFNumber; 少数驱动给 CFString)
    /// - 字段缺失或取值不可用时返回 false (调用方据此区分"0"与"没有该字段")
    static bool cfDictNumber(CFDictionaryRef dict, CFStringRef key, double& out) {
        const void* value = nullptr;
        if (!CFDictionaryGetValueIfPresent(dict, key, &value) || !value) {
            return false;
        }

        const CFTypeID typeId = CFGetTypeID(value);
        if (typeId == CFNumberGetTypeID()) {
            return CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberDoubleType, &out)
                   != 0;
        }
        if (typeId == CFStringGetTypeID()) {
            const std::string text = cfStringToUtf8(static_cast<CFStringRef>(value));
            if (text.empty()) {
                return false;
            }
            char*  end = nullptr;
            double val = std::strtod(text.c_str(), &end);
            if (end == text.c_str()) {
                return false;
            }
            out = val;
            return true;
        }
        return false;
    }
};

CpuGpuMonitor::CpuGpuMonitor() :
    impl_(std::make_unique<Impl>()) {}

CpuGpuMonitor::~CpuGpuMonitor() = default;

asio::awaitable<CpuGpuUsage> CpuGpuMonitor::query() {
    co_return co_await impl_->query();
}

} // namespace agentxx_system_monitor_plugin

#else

#include <thread>

namespace agentxx_system_monitor_plugin {

class CpuGpuMonitor::Impl {
public:

    Impl() {}

    asio::awaitable<CpuGpuUsage> query() {
        CpuGpuUsage result;
        result.cpuCoreCount = cpuCoreCount_;
        co_return result;
    }

private:

    /// CPU 逻辑核心数 (构造时取一次; 平台未实现时退回标准库探测, 取不到为 0)
    uint32_t cpuCoreCount_ = queryCpuCoreCount();

    static uint32_t queryCpuCoreCount() {
        unsigned int hw = std::thread::hardware_concurrency();
        return hw > 0 ? static_cast<uint32_t>(hw) : 0;
    }
};

CpuGpuMonitor::CpuGpuMonitor() :
    impl_(std::make_unique<Impl>()) {}

CpuGpuMonitor::~CpuGpuMonitor() = default;

asio::awaitable<CpuGpuUsage> CpuGpuMonitor::query() {
    return impl_->query();
}

} // namespace agentxx_system_monitor_plugin

#endif