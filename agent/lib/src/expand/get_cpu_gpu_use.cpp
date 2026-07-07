#include "agentxx/expand/get_cpu_gpu_use.h"
#include "agentxx/util/log.h"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include <map>
#include <string>
#include <vector>

#if XX_IS_WIN_D
#include <dxgi1_6.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <windows.h>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "pdh.lib")

namespace agentxx {
namespace expand {

struct LUIDLess {
  bool operator()(const LUID &a, const LUID &b) const {
    if (a.HighPart != b.HighPart) {
      return a.HighPart < b.HighPart;
    }
    return a.LowPart < b.LowPart;
  }
};

class CpuGpuMonitor::Impl {
public:
  Impl() {}

  ~Impl() { cleanupPdhQuery(); }

  asio::awaitable<CpuGpuUsage> query() {
    CpuGpuUsage result;

    queryMemoryInfo(result);
    co_await queryGpuInfo(result);

    if (prevTotalTime_ == 0 || prevIdleTime_ == 0) {
      // 间隔一段时间做差值计算，如果没有初始化需要先初始化一次
      queryCpuUsage(result);
      asio::steady_timer timer(co_await asio::this_coro::executor,
                               std::chrono::milliseconds(100));
      co_await timer.async_wait(asio::use_awaitable);
    }
    queryCpuUsage(result);

    co_return result;
  }

private:
  void queryCpuUsage(CpuGpuUsage &result) {
    FILETIME idleTime, kernelTime, userTime;
    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
      XX_LOGE("CpuGpuMonitor: GetSystemTimes failed, error={}", GetLastError());
      return;
    }

    ULARGE_INTEGER idle, kernel, user;
    idle.LowPart = idleTime.dwLowDateTime;
    idle.HighPart = idleTime.dwHighDateTime;
    kernel.LowPart = kernelTime.dwLowDateTime;
    kernel.HighPart = kernelTime.dwHighDateTime;
    user.LowPart = userTime.dwLowDateTime;
    user.HighPart = userTime.dwHighDateTime;

    ULONGLONG totalTime = kernel.QuadPart + user.QuadPart;

    if (prevTotalTime_ > 0 && totalTime > prevTotalTime_) {
      ULONGLONG idleDelta = idle.QuadPart - prevIdleTime_;
      ULONGLONG totalDelta = totalTime - prevTotalTime_;

      if (totalDelta > 0) {
        result.cpuUsagePercent = (1.0 - static_cast<double>(idleDelta) /
                                            static_cast<double>(totalDelta)) *
                                 100.0;
        if (result.cpuUsagePercent < 0.0) {
          result.cpuUsagePercent = 0.0;
        }
        if (result.cpuUsagePercent > 100.0) {
          result.cpuUsagePercent = 100.0;
        }
      }
    }

    prevIdleTime_ = idle.QuadPart;
    prevTotalTime_ = totalTime;
  }

  void queryMemoryInfo(CpuGpuUsage &result) {
    MEMORYSTATUSEX memStatus = {};
    memStatus.dwLength = sizeof(memStatus);

    if (!GlobalMemoryStatusEx(&memStatus)) {
      XX_LOGE("CpuGpuMonitor: GlobalMemoryStatusEx failed, error={}",
              GetLastError());
      return;
    }

    result.memory.totalPhysicalMB = memStatus.ullTotalPhys / (1024 * 1024);
    result.memory.usedPhysicalMB =
        (memStatus.ullTotalPhys - memStatus.ullAvailPhys) / (1024 * 1024);

    if (memStatus.ullTotalPhys > 0) {
      result.memory.usagePercent =
          static_cast<double>(memStatus.ullTotalPhys - memStatus.ullAvailPhys) /
          static_cast<double>(memStatus.ullTotalPhys) * 100.0;
    }
  }

  asio::awaitable<void> queryGpuInfo(CpuGpuUsage &result) {
    IDXGIFactory6 *factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory6),
                                    reinterpret_cast<void **>(&factory));
    if (FAILED(hr) || !factory) {
      XX_LOGE("CpuGpuMonitor: CreateDXGIFactory1 failed, hr=0x{:08X}",
              static_cast<unsigned>(hr));
      co_return;
    }

    std::map<LUID, double, LUIDLess> gpuUsageMap;
    std::map<LUID, uint64_t, LUIDLess> gpuDedicatedUsedMap;
    std::map<LUID, uint64_t, LUIDLess> gpuSharedUsedMap;

    if (initPdhQuery()) {
      co_await collectPdhGpuData(gpuUsageMap, gpuDedicatedUsedMap,
                                 gpuSharedUsedMap);
    }

    UINT adapterIndex = 0;
    IDXGIAdapter1 *adapter = nullptr;

    while (factory->EnumAdapters1(adapterIndex, &adapter) !=
           DXGI_ERROR_NOT_FOUND) {
      GpuInfo info;

      DXGI_ADAPTER_DESC1 desc = {};
      hr = adapter->GetDesc1(&desc);
      if (SUCCEEDED(hr)) {
        int len = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr,
                                      0, nullptr, nullptr);
        if (len > 0) {
          std::vector<char> buf(len);
          WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, buf.data(), len,
                              nullptr, nullptr);
          info.name = buf.data();
        }
      }

      if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
        adapter->Release();
        adapterIndex++;
        continue;
      }

      info.dedicatedVramMB = desc.DedicatedVideoMemory / (1024 * 1024);
      info.sharedVramMB = desc.SharedSystemMemory / (1024 * 1024);

      const LUID &luid = desc.AdapterLuid;

      auto itDedicated = gpuDedicatedUsedMap.find(luid);
      if (itDedicated != gpuDedicatedUsedMap.end()) {
        info.dedicatedVramUsedMB = itDedicated->second / (1024 * 1024);
      }

      auto itShared = gpuSharedUsedMap.find(luid);
      if (itShared != gpuSharedUsedMap.end()) {
        info.sharedVramUsedMB = itShared->second / (1024 * 1024);
      }

      auto itUsage = gpuUsageMap.find(luid);
      if (itUsage != gpuUsageMap.end()) {
        info.usagePercent = itUsage->second;
      }

      result.gpus.push_back(std::move(info));

      adapter->Release();
      adapterIndex++;
    }

    factory->Release();
  }

  asio::awaitable<void>
  collectPdhGpuData(std::map<LUID, double, LUIDLess> &gpuUsageMap,
                    std::map<LUID, uint64_t, LUIDLess> &dedicatedUsedMap,
                    std::map<LUID, uint64_t, LUIDLess> &sharedUsedMap) {
    HCOUNTER hEngineCounter = nullptr;
    PDH_STATUS status =
        PdhAddCounterW(pdhQuery_, L"\\GPU Engine(*)\\Utilization Percentage", 0,
                       &hEngineCounter);

    HCOUNTER hProcMemDedicated = nullptr;
    PDH_STATUS memDedicatedStatus =
        PdhAddCounterW(pdhQuery_, L"\\GPU Process Memory(*)\\Dedicated Usage",
                       0, &hProcMemDedicated);

    HCOUNTER hProcMemShared = nullptr;
    PDH_STATUS memSharedStatus =
        PdhAddCounterW(pdhQuery_, L"\\GPU Process Memory(*)\\Shared Usage", 0,
                       &hProcMemShared);

    if (status != ERROR_SUCCESS && memDedicatedStatus != ERROR_SUCCESS &&
        memSharedStatus != ERROR_SUCCESS) {
      if (hEngineCounter) {
        PdhRemoveCounter(hEngineCounter);
      }
      if (hProcMemDedicated) {
        PdhRemoveCounter(hProcMemDedicated);
      }
      if (hProcMemShared) {
        PdhRemoveCounter(hProcMemShared);
      }
      co_return;
    }

    PdhCollectQueryData(pdhQuery_);

    asio::steady_timer timer(co_await asio::this_coro::executor,
                             std::chrono::milliseconds(100));
    co_await timer.async_wait(asio::use_awaitable);

    PdhCollectQueryData(pdhQuery_);

    if (hEngineCounter && status == ERROR_SUCCESS) {
      collectEngineUtilization(hEngineCounter, gpuUsageMap);
      PdhRemoveCounter(hEngineCounter);
    }

    if (hProcMemDedicated && memDedicatedStatus == ERROR_SUCCESS) {
      collectProcessMemory(hProcMemDedicated, dedicatedUsedMap);
      PdhRemoveCounter(hProcMemDedicated);
    }

    if (hProcMemShared && memSharedStatus == ERROR_SUCCESS) {
      collectProcessMemory(hProcMemShared, sharedUsedMap);
      PdhRemoveCounter(hProcMemShared);
    }
  }

  void collectEngineUtilization(HCOUNTER hCounter,
                                std::map<LUID, double, LUIDLess> &outMap) {
    DWORD bufSize = 0;
    DWORD itemCount = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(
        hCounter, PDH_FMT_DOUBLE, &bufSize, &itemCount, nullptr);
    if (status != PDH_MORE_DATA || bufSize == 0) {
      return;
    }

    std::vector<BYTE> buf(bufSize);
    auto *pItems = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buf.data());
    status = PdhGetFormattedCounterArrayW(hCounter, PDH_FMT_DOUBLE, &bufSize,
                                          &itemCount, pItems);
    if (status != ERROR_SUCCESS) {
      return;
    }

    for (DWORD i = 0; i < itemCount; ++i) {
      const wchar_t *instanceName = pItems[i].szName;
      if (!instanceName || !*instanceName) {
        continue;
      }

      LUID luid = {};
      if (parseEngineLuid(instanceName, luid)) {
        double val = pItems[i].FmtValue.doubleValue;
        auto it = outMap.find(luid);
        if (it == outMap.end() || val > it->second) {
          outMap[luid] = val;
        }
      }
    }
  }

  void collectProcessMemory(HCOUNTER hCounter,
                            std::map<LUID, uint64_t, LUIDLess> &outMap) {
    DWORD bufSize = 0;
    DWORD itemCount = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(
        hCounter, PDH_FMT_LARGE, &bufSize, &itemCount, nullptr);
    if (status != PDH_MORE_DATA || bufSize == 0) {
      return;
    }

    std::vector<BYTE> buf(bufSize);
    auto *pItems = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buf.data());
    status = PdhGetFormattedCounterArrayW(hCounter, PDH_FMT_LARGE, &bufSize,
                                          &itemCount, pItems);
    if (status != ERROR_SUCCESS) {
      return;
    }

    for (DWORD i = 0; i < itemCount; ++i) {
      const wchar_t *instanceName = pItems[i].szName;
      if (!instanceName || !*instanceName) {
        continue;
      }

      LUID luid = {};
      if (parseProcessMemoryLuid(instanceName, luid)) {
        outMap[luid] += pItems[i].FmtValue.largeValue;
      }
    }
  }

  bool parseEngineLuid(const wchar_t *instanceName, LUID &outLuid) {
    std::wstring name(instanceName);

    auto luidPos = name.find(L"_luid_");
    if (luidPos == std::wstring::npos) {
      return false;
    }

    std::wstring luidStr = name.substr(luidPos + 6);

    auto physPos = luidStr.find(L"_phys_");
    if (physPos == std::wstring::npos) {
      return false;
    }

    luidStr = luidStr.substr(0, physPos);

    auto underscorePos = luidStr.find(L'_');
    if (underscorePos == std::wstring::npos) {
      return false;
    }

    std::wstring highStr = luidStr.substr(0, underscorePos);
    std::wstring lowStr = luidStr.substr(underscorePos + 1);

    outLuid.HighPart = static_cast<LONG>(wcstoul(highStr.c_str(), nullptr, 16));
    outLuid.LowPart = static_cast<LONG>(wcstoul(lowStr.c_str(), nullptr, 16));
    return true;
  }

  bool parseProcessMemoryLuid(const wchar_t *instanceName, LUID &outLuid) {
    std::wstring name(instanceName);

    auto luidPos = name.find(L"_luid_");
    if (luidPos == std::wstring::npos) {
      return false;
    }

    std::wstring luidStr = name.substr(luidPos + 6);

    auto underscorePos = luidStr.find(L'_');
    if (underscorePos == std::wstring::npos) {
      return false;
    }

    std::wstring highStr = luidStr.substr(0, underscorePos);
    std::wstring lowStr = luidStr.substr(underscorePos + 1);

    outLuid.HighPart = static_cast<LONG>(wcstoul(highStr.c_str(), nullptr, 16));
    outLuid.LowPart = static_cast<LONG>(wcstoul(lowStr.c_str(), nullptr, 16));
    return true;
  }

  bool initPdhQuery() {
    if (pdhQuery_) {
      return true;
    }
    PDH_STATUS status = PdhOpenQueryW(nullptr, 0, &pdhQuery_);
    if (status != ERROR_SUCCESS) {
      XX_LOGW("CpuGpuMonitor: PdhOpenQuery failed, status={}", status);
      pdhQuery_ = nullptr;
      return false;
    }
    return true;
  }

  void cleanupPdhQuery() {
    if (pdhQuery_) {
      PdhCloseQuery(pdhQuery_);
      pdhQuery_ = nullptr;
    }
  }

  ULONGLONG prevIdleTime_ = 0;
  ULONGLONG prevTotalTime_ = 0;
  HQUERY pdhQuery_ = nullptr;
};

CpuGpuMonitor::CpuGpuMonitor() : impl_(std::make_unique<Impl>()) {}
CpuGpuMonitor::~CpuGpuMonitor() = default;
asio::awaitable<CpuGpuUsage> CpuGpuMonitor::query() {
  co_return co_await impl_->query();
}

} // namespace expand
} // namespace agentxx

#elif XX_IS_LINUX_D

#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace agentxx {
namespace expand {

class CpuGpuMonitor::Impl {
public:
  Impl() {}

  ~Impl() {}

  struct CpuTimes {
    uint64_t total = 0;
    uint64_t idle = 0;
  };

  asio::awaitable<CpuGpuUsage> query() {
    CpuGpuUsage result;

    CpuTimes oldSample = _sample;
    if (_sample.total == 0 || _sample.idle == 0) {
      oldSample = readCpuStat();
      asio::steady_timer timer(co_await asio::this_coro::executor,
                               std::chrono::milliseconds(100));
      co_await timer.async_wait(asio::use_awaitable);
    }

    _sample = readCpuStat();

    if (oldSample.total > _sample.total) {
      uint64_t totalDelta = oldSample.total - _sample.total;
      uint64_t idleDelta = oldSample.idle - _sample.idle;

      if (totalDelta > 0) {
        result.cpuUsagePercent = (1.0 - static_cast<double>(idleDelta) /
                                            static_cast<double>(totalDelta)) *
                                 100.0;
        if (result.cpuUsagePercent < 0.0) {
          result.cpuUsagePercent = 0.0;
        }
        if (result.cpuUsagePercent > 100.0) {
          result.cpuUsagePercent = 100.0;
        }
      }
    }

    queryMemoryInfo(result);
    queryGpuInfo(result);

    co_return result;
  }

protected:
  CpuTimes _sample;

  static CpuTimes readCpuStat() {
    CpuTimes times;
    std::ifstream statFile("/proc/stat");
    if (!statFile.is_open()) {
      return times;
    }

    std::string line;
    if (!std::getline(statFile, line)) {
      return times;
    }

    std::istringstream iss(line);
    std::string cpuLabel;
    uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0;
    uint64_t irq = 0, softirq = 0, steal = 0;
    iss >> cpuLabel >> user >> nice >> system >> idle >> iowait >> irq >>
        softirq >> steal;

    times.total = user + nice + system + idle + iowait + irq + softirq + steal;
    times.idle = idle + iowait;
    return times;
  }

  void queryMemoryInfo(CpuGpuUsage &result) {
    std::ifstream memFile("/proc/meminfo");
    if (!memFile.is_open()) {
      return;
    }

    std::string line;
    uint64_t memTotalKB = 0;
    uint64_t memAvailableKB = 0;

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
      uint64_t usedKB = memTotalKB - memAvailableKB;
      result.memory.usedPhysicalMB = usedKB / 1024;
      result.memory.usagePercent =
          static_cast<double>(usedKB) / static_cast<double>(memTotalKB) * 100.0;
    }
  }

  void queryGpuInfo(CpuGpuUsage &result) { queryGpuInfoSysfs(result); }

  void queryGpuInfoSysfs(CpuGpuUsage &result) {
    for (int cardIdx = 0;; ++cardIdx) {
      std::string devicePath =
          "/sys/class/drm/card" + std::to_string(cardIdx) + "/device";

      std::ifstream vendorFile(devicePath + "/vendor");
      if (!vendorFile.is_open()) {
        break;
      }

      std::string vendorLine;
      std::getline(vendorFile, vendorLine);
      vendorFile.close();

      uint32_t vendorId = parseHexSysfs(vendorLine);

      GpuInfo info;

      readSysfsString(devicePath + "/product_name", info.name);

      if (vendorId == 0x1002) {
        queryAmdGpuSysfs(info, devicePath);
      } else if (vendorId == 0x10de) {
        queryNvidiaProcfs(info);
      }

      if (info.name.empty()) {
        info.name = "GPU " + std::to_string(cardIdx);
      }

      result.gpus.push_back(std::move(info));
    }
  }

  void queryAmdGpuSysfs(GpuInfo &info, const std::string &devicePath) {
    readSysfsUint64(devicePath + "/mem_info_vram_total", info.dedicatedVramMB);
    info.dedicatedVramMB /= (1024 * 1024);

    readSysfsUint64(devicePath + "/mem_info_vram_used",
                    info.dedicatedVramUsedMB);
    info.dedicatedVramUsedMB /= (1024 * 1024);

    uint64_t gpuBusy = 0;
    readSysfsUint64(devicePath + "/gpu_busy_percent", gpuBusy);
    info.usagePercent = static_cast<double>(gpuBusy);

    if (info.usagePercent == 0.0 && info.dedicatedVramMB > 0) {
      info.usagePercent = static_cast<double>(info.dedicatedVramUsedMB) /
                          static_cast<double>(info.dedicatedVramMB) * 100.0;
    }
  }

  void queryNvidiaProcfs(GpuInfo &info) {
    std::ifstream gpusDir("/proc/driver/nvidia/gpus");
    if (!gpusDir.is_open()) {
      return;
    }
    gpusDir.close();

    DIR *dir = opendir("/proc/driver/nvidia/gpus");
    if (!dir) {
      return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
      if (entry->d_name[0] == '.') {
        continue;
      }

      std::string infoPath = std::string("/proc/driver/nvidia/gpus/") +
                             entry->d_name + "/information";
      std::ifstream infoFile(infoPath);
      if (!infoFile.is_open()) {
        continue;
      }

      std::string line;
      while (std::getline(infoFile, line)) {
        if (line.rfind("Model:", 0) == 0) {
          size_t pos = line.find(':');
          if (pos != std::string::npos) {
            info.name = trim(line.substr(pos + 1));
          }
        } else if (line.rfind("Video Memory:", 0) == 0) {
          size_t pos = line.find(':');
          if (pos != std::string::npos) {
            std::string memStr = trim(line.substr(pos + 1));
            uint64_t totalMiB = 0;
            std::sscanf(memStr.c_str(), "%lu MiB", &totalMiB);
            info.dedicatedVramMB = totalMiB;
          }
        }
      }
      infoFile.close();
      break;
    }
    closedir(dir);
  }

  static uint32_t parseHexSysfs(const std::string &line) {
    std::string hex = line;
    if (hex.rfind("0x", 0) == 0) {
      hex = hex.substr(2);
    }
    while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r')) {
      hex.pop_back();
    }
    uint32_t val = 0;
    std::sscanf(hex.c_str(), "%x", &val);
    return val;
  }

  static void readSysfsString(const std::string &path, std::string &out) {
    std::ifstream file(path);
    if (!file.is_open()) {
      return;
    }
    std::string line;
    if (std::getline(file, line)) {
      out = trim(line);
    }
  }

  static void readSysfsUint64(const std::string &path, uint64_t &out) {
    std::ifstream file(path);
    if (!file.is_open()) {
      return;
    }
    file >> out;
  }

  static std::string trim(const std::string &s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' ||
                                s[start] == '\n' || s[start] == '\r')) {
      ++start;
    }
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
                           s[end - 1] == '\n' || s[end - 1] == '\r')) {
      --end;
    }
    return s.substr(start, end - start);
  }
};

CpuGpuMonitor::CpuGpuMonitor() : impl_(std::make_unique<Impl>()) {}
CpuGpuMonitor::~CpuGpuMonitor() = default;
asio::awaitable<CpuGpuUsage> CpuGpuMonitor::query() {
  co_return co_await impl_->query();
}

} // namespace expand
} // namespace agentxx

#else

namespace agentxx {
namespace expand {

class CpuGpuMonitor::Impl {
public:
  Impl() {}
  asio::awaitable<CpuGpuUsage> query() { co_return CpuGpuUsage{}; }
};

CpuGpuMonitor::CpuGpuMonitor() : impl_(std::make_unique<Impl>()) {}
CpuGpuMonitor::~CpuGpuMonitor() = default;
asio::awaitable<CpuGpuUsage> CpuGpuMonitor::query() { return impl_->query(); }

} // namespace expand
} // namespace agentxx

#endif