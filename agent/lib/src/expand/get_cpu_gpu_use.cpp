#include "agentxx/expand/get_cpu_gpu_use.h"
#include "agentxx/util/log.h"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include <string>
#include <string_view>
#include <unordered_map>
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

struct LUIDHash {
  size_t operator()(const LUID &luid) const {
    uint64_t combined =
        (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32) |
        static_cast<uint32_t>(luid.LowPart);
    return std::hash<uint64_t>{}(combined);
  }
};

struct LUIDEqual {
  bool operator()(const LUID &a, const LUID &b) const {
    return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
  }
};

struct CachedGpuAdapter {
  std::string name;
  LUID luid;
  uint64_t dedicatedVramMB = 0;
  uint64_t sharedVramMB = 0;
};

struct GpuAdapterCache {
  std::vector<CachedGpuAdapter> adapters;
  bool built = false;
};

static GpuAdapterCache &getGpuAdapterCache() {
  static GpuAdapterCache cache;
  return cache;
}

struct SharedPdhContext {
  HQUERY query = nullptr;
  HCOUNTER hEngineCounter = nullptr;
  HCOUNTER hProcMemDedicated = nullptr;
  HCOUNTER hProcMemShared = nullptr;
  bool initialized = false;
  bool initAttempted = false;

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

    PDH_STATUS engStatus = PdhAddCounterW(
        query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &hEngineCounter);
    PDH_STATUS dedStatus =
        PdhAddCounterW(query, L"\\GPU Process Memory(*)\\Dedicated Usage", 0,
                       &hProcMemDedicated);
    PDH_STATUS shrStatus = PdhAddCounterW(
        query, L"\\GPU Process Memory(*)\\Shared Usage", 0, &hProcMemShared);

    initialized = (engStatus == ERROR_SUCCESS || dedStatus == ERROR_SUCCESS ||
                   shrStatus == ERROR_SUCCESS);
    return initialized;
  }
};

static SharedPdhContext &getSharedPdh() {
  static SharedPdhContext ctx;
  return ctx;
}

class CpuGpuMonitor::Impl {
public:
  Impl() {}

  ~Impl() = default;

  asio::awaitable<CpuGpuUsage> query() {
    CpuGpuUsage result;

    queryMemoryInfo(result);

    bool needCpuInit = (prevTotalTime_ == 0 || prevIdleTime_ == 0);
    if (needCpuInit) {
      queryCpuUsage(result);
    }

    bool pdhAvailable = getSharedPdh().ensureInitialized();
    if (pdhAvailable) {
      PdhCollectQueryData(getSharedPdh().query);
    }

    if (needCpuInit || pdhAvailable) {
      asio::steady_timer timer(co_await asio::this_coro::executor,
                               std::chrono::milliseconds(100));
      co_await timer.async_wait(asio::use_awaitable);
    }

    queryCpuUsage(result);

    if (pdhAvailable) {
      co_await buildGpuCache();
      PdhCollectQueryData(getSharedPdh().query);
      collectPdhGpuData(result);
    }

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

  static asio::awaitable<void> buildGpuCache() {
    auto &cache = getGpuAdapterCache();
    if (cache.built) {
      co_return;
    }

    IDXGIFactory6 *factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory6),
                                    reinterpret_cast<void **>(&factory));
    if (FAILED(hr) || !factory) {
      XX_LOGE("CpuGpuMonitor: CreateDXGIFactory1 failed, hr=0x{:08X}",
              static_cast<unsigned>(hr));
      cache.built = true;
      co_return;
    }

    UINT adapterIndex = 0;
    IDXGIAdapter1 *adapter = nullptr;

    while (factory->EnumAdapters1(adapterIndex, &adapter) !=
           DXGI_ERROR_NOT_FOUND) {
      DXGI_ADAPTER_DESC1 desc = {};
      hr = adapter->GetDesc1(&desc);
      if (SUCCEEDED(hr) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
        CachedGpuAdapter info;
        info.luid = desc.AdapterLuid;
        info.dedicatedVramMB = desc.DedicatedVideoMemory / (1024 * 1024);
        info.sharedVramMB = desc.SharedSystemMemory / (1024 * 1024);

        int len = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr,
                                      0, nullptr, nullptr);
        if (len > 0) {
          info.name.resize(static_cast<size_t>(len) - 1);
          WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, &info.name[0],
                              len, nullptr, nullptr);
        }

        cache.adapters.push_back(std::move(info));
      }

      adapter->Release();
      adapterIndex++;
    }

    factory->Release();
    cache.built = true;
  }

  void collectPdhGpuData(CpuGpuUsage &result) {
    auto &cache = getGpuAdapterCache();
    if (!cache.built) {
      return;
    }

    auto &pdh = getSharedPdh();

    std::unordered_map<LUID, double, LUIDHash, LUIDEqual> gpuUsageMap;
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

    for (const auto &cached : cache.adapters) {
      GpuInfo info;
      info.name = cached.name;
      info.dedicatedVramMB = cached.dedicatedVramMB;
      info.sharedVramMB = cached.sharedVramMB;

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
      HCOUNTER hCounter,
      std::unordered_map<LUID, double, LUIDHash, LUIDEqual> &outMap) {
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

  void collectProcessMemory(
      HCOUNTER hCounter,
      std::unordered_map<LUID, uint64_t, LUIDHash, LUIDEqual> &outMap) {
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

  static bool parseEngineLuid(const wchar_t *instanceName, LUID &outLuid) {
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
    outLuid.LowPart = static_cast<LONG>(wcstoul(lowStr.c_str(), nullptr, 16));
    return true;
  }

  static bool parseProcessMemoryLuid(const wchar_t *instanceName,
                                     LUID &outLuid) {
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
    outLuid.LowPart = static_cast<LONG>(wcstoul(lowStr.c_str(), nullptr, 16));
    return true;
  }

  ULONGLONG prevIdleTime_ = 0;
  ULONGLONG prevTotalTime_ = 0;
};

CpuGpuMonitor::CpuGpuMonitor() : impl_(std::make_unique<Impl>()) {}
CpuGpuMonitor::~CpuGpuMonitor() = default;
asio::awaitable<CpuGpuUsage> CpuGpuMonitor::query() {
  co_return co_await impl_->query();
}

} // namespace expand
} // namespace agentxx

#elif XX_IS_LINUX_D

#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "asio/random_access_file.hpp"
#include "asio/read.hpp"
#include "asio/read_at.hpp"
#include "asio/read_until.hpp"
#include "asio/redirect_error.hpp"
#include "asio/registered_buffer.hpp"
#include "asio/stream_file.hpp"
#include "asio/use_awaitable.hpp"
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
#include <neograph/neograph.h>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace expand {

struct LinuxGpuCacheEntry {
  std::string devicePath;
  uint32_t vendorId = 0;
  std::string name;
  uint64_t dedicatedVramMB = 0;
  bool isAmd = false;
  bool isNvidia = false;
};

struct LinuxGpuCache {
  std::vector<LinuxGpuCacheEntry> entries;
  bool built = false;
};

static LinuxGpuCache &getLinuxGpuCache() {
  static LinuxGpuCache cache;
  return cache;
}

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
      oldSample = co_await readCpuStat();
      asio::steady_timer timer(co_await asio::this_coro::executor,
                               std::chrono::milliseconds(100));
      co_await timer.async_wait(asio::use_awaitable);
    }

    co_await queryMemoryInfo(result);
    co_await queryGpuInfo(result);

    {
      _sample = co_await readCpuStat();
      if (_sample.total > oldSample.total) {
        uint64_t totalDelta = _sample.total - oldSample.total;
        uint64_t idleDelta = _sample.idle - oldSample.idle;

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
    }

    co_return result;
  }

protected:
  CpuTimes _sample;

  static asio::awaitable<std::string> readFileContent(const std::string &path) {
#if ASIO_HAS_FILE || BOOST_ASIO_HAS_FILE
    {
      auto executor = co_await asio::this_coro::executor;
      asio::stream_file stream{executor};
      neograph_asio_error_code errCode;
      stream.open(path, asio::stream_file::read_only, errCode);
      if (!stream.is_open()) {
        co_return "";
      }
      std::string data;
      co_await asio::async_read(
          stream, asio::dynamic_buffer(data), asio::transfer_all(),
          asio::redirect_error(asio::use_awaitable, errCode));
      stream.close();
      if (errCode && errCode != asio::error::eof) {
        co_return "";
      }
      co_return data;
    }
#else
    std::ifstream stream;
    stream.open(path);
    if (!stream) {
      co_return "";
    }

    auto result = std::string{std::istreambuf_iterator<char>(stream),
                              std::istreambuf_iterator<char>()};
    stream.close();
    co_return result;
#endif
  }

  static asio::awaitable<CpuTimes> readCpuStat() {
    CpuTimes times;
    std::string content = co_await readFileContent("/proc/stat");
    if (content.empty()) {
      co_return times;
    }

    std::istringstream iss(content);
    std::string cpuLabel;
    uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0;
    uint64_t irq = 0, softirq = 0, steal = 0;
    iss >> cpuLabel >> user >> nice >> system >> idle >> iowait >> irq >>
        softirq >> steal;

    times.total = user + nice + system + idle + iowait + irq + softirq + steal;
    times.idle = idle + iowait;
    co_return times;
  }

  asio::awaitable<void> queryMemoryInfo(CpuGpuUsage &result) {
    std::string content = co_await readFileContent("/proc/meminfo");
    if (content.empty()) {
      co_return;
    }

    std::istringstream memFile(content);
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

  asio::awaitable<void> queryGpuInfo(CpuGpuUsage &result) {
    auto &cache = getLinuxGpuCache();
    if (!cache.built) {
      co_await buildGpuCache();
    }

    for (const auto &entry : cache.entries) {
      GpuInfo info;
      info.name = entry.name;
      info.dedicatedVramMB = entry.dedicatedVramMB;

      if (entry.isAmd) {
        co_await queryAmdGpuUsage(info, entry.devicePath);
      }

      result.gpus.push_back(std::move(info));
    }
  }

  static asio::awaitable<void> buildGpuCache() {
    auto &cache = getLinuxGpuCache();

    for (int cardIdx = 0;; ++cardIdx) {
      auto devicePath = fmt::format("/sys/class/drm/card{}/device", cardIdx);

      std::string vendorContent =
          co_await readFileContent(devicePath + "/vendor");
      if (vendorContent.empty()) {
        break;
      }

      LinuxGpuCacheEntry entry;
      entry.devicePath = devicePath;
      entry.vendorId = parseHexSysfs(vendorContent);

      co_await readSysfsString(devicePath + "/product_name", entry.name);

      if (entry.vendorId == 0x1002) {
        entry.isAmd = true;
        co_await readSysfsUint64(devicePath + "/mem_info_vram_total",
                                 entry.dedicatedVramMB);
        entry.dedicatedVramMB /= (1024 * 1024);
      } else if (entry.vendorId == 0x10de) {
        entry.isNvidia = true;
        co_await readNvidiaInfo(entry);
      }

      if (entry.name.empty()) {
        entry.name = "GPU " + std::to_string(cardIdx);
      }

      cache.entries.push_back(std::move(entry));
    }
    cache.built = true;
  }

  static asio::awaitable<void> queryAmdGpuUsage(GpuInfo &info,
                                                const std::string &devicePath) {
    co_await readSysfsUint64(devicePath + "/mem_info_vram_used",
                             info.dedicatedVramUsedMB);
    info.dedicatedVramUsedMB /= (1024 * 1024);

    uint64_t gpuBusy = 0;
    co_await readSysfsUint64(devicePath + "/gpu_busy_percent", gpuBusy);
    info.usagePercent = static_cast<double>(gpuBusy);

    if (info.usagePercent == 0.0 && info.dedicatedVramMB > 0) {
      info.usagePercent = static_cast<double>(info.dedicatedVramUsedMB) /
                          static_cast<double>(info.dedicatedVramMB) * 100.0;
    }
  }

  static asio::awaitable<void> readNvidiaInfo(LinuxGpuCacheEntry &entry) {
    std::string gpusDirContent =
        co_await readFileContent("/proc/driver/nvidia/gpus");
    if (gpusDirContent.empty()) {
      co_return;
    }

    DIR *dir = opendir("/proc/driver/nvidia/gpus");
    if (!dir) {
      co_return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
      if (ent->d_name[0] == '.') {
        continue;
      }

      std::string infoPath = std::string("/proc/driver/nvidia/gpus/") +
                             ent->d_name + "/information";
      std::string infoContent = co_await readFileContent(infoPath);
      if (infoContent.empty()) {
        continue;
      }

      std::istringstream infoFile(infoContent);
      std::string line;
      while (std::getline(infoFile, line)) {
        if (line.rfind("Model:", 0) == 0) {
          size_t pos = line.find(':');
          if (pos != std::string::npos) {
            entry.name = agentxx::util::removeBetweenSpace(
                std::string_view{line}.substr(pos + 1));
          }
        } else if (line.rfind("Video Memory:", 0) == 0) {
          size_t pos = line.find(':');
          if (pos != std::string::npos) {
            auto memStr = agentxx::util::removeBetweenSpace(
                std::string_view{line}.substr(pos + 1));
            uint64_t totalMiB = 0;
            std::sscanf(memStr.c_str(), "%lu MiB", &totalMiB);
            entry.dedicatedVramMB = totalMiB;
          }
        }
      }
      break;
    }
    closedir(dir);
  }

  static uint32_t parseHexSysfs(std::string_view line) {
    if (line.size() >= 2 && line[0] == '0' &&
        (line[1] == 'x' || line[1] == 'X')) {
      line = line.substr(2);
    }
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
      line = line.substr(0, line.size() - 1);
    }
    std::string hex(line);
    uint32_t val = 0;
    std::sscanf(hex.c_str(), "%x", &val);
    return val;
  }

  static asio::awaitable<void> readSysfsString(const std::string &path,
                                               std::string &out) {
    std::string content = co_await readFileContent(path);
    if (!content.empty()) {
      out = agentxx::util::removeBetweenSpace(content);
    }
  }

  static asio::awaitable<void> readSysfsUint64(const std::string &path,
                                               uint64_t &out) {
    std::string content = co_await readFileContent(path);
    if (!content.empty()) {
      std::istringstream iss(content);
      iss >> out;
    }
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