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

    queryCpuUsage(result);
    queryMemoryInfo(result);
    co_await queryGpuInfo(result);

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
asio::awaitable<CpuGpuUsage> CpuGpuMonitor::query() { return impl_->query(); }

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