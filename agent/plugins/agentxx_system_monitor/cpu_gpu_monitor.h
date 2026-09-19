#pragma once

// fix asio check [__cpp_lib_is_invocable]
#include <type_traits>
// ---
#include "asio/awaitable.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace asio = ::boost::asio;

namespace agentxx_system_monitor_plugin {

struct MemoryInfo {
    uint64_t totalPhysicalMB = 0;
    uint64_t usedPhysicalMB  = 0;
    double   usagePercent    = 0.0;
};

struct GpuInfo {
    std::string name;
    uint64_t    dedicatedVramMB     = 0;
    uint64_t    dedicatedVramUsedMB = 0;
    uint64_t    sharedVramMB        = 0;
    uint64_t    sharedVramUsedMB    = 0;
    double      usagePercent        = 0.0;
};

struct CpuGpuUsage {
    /// CPU 逻辑核心数 (含超线程, 与利用率同为一个整机的口径; 取不到时为 0,
    /// 展示端遇到 0 应忽略该字段)
    uint32_t             cpuCoreCount    = 0;
    double               cpuUsagePercent = 0.0;
    MemoryInfo           memory;
    std::vector<GpuInfo> gpus;
};

class CpuGpuMonitor {
public:

    CpuGpuMonitor();
    ~CpuGpuMonitor();

    CpuGpuMonitor(const CpuGpuMonitor&)            = delete;
    CpuGpuMonitor& operator=(const CpuGpuMonitor&) = delete;

    asio::awaitable<CpuGpuUsage> query();

private:

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace agentxx_system_monitor_plugin