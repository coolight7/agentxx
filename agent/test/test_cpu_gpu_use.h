#pragma once

#include "agentxx/expand/get_cpu_gpu_use.h"
#include <iostream>

#include "asio/awaitable.hpp"

#if XX_IS_WIN_D
#include <windows.h>
#endif
#include <assert.h>

namespace agentxx {
namespace test {

inline asio::awaitable<void> test_cpu_gpu_use() {
#if XX_IS_WIN_D || XX_IS_LINUX_D
  std::cout << "======= CpuGpuMonitor Test =======" << std::endl;

  agentxx::expand::CpuGpuMonitor monitor{};

  co_await monitor.query();
  agentxx::expand::CpuGpuUsage usage = co_await monitor.query();

  std::cout << "[INFO] CPU 使用率: " << usage.cpuUsagePercent << "%"
            << std::endl;
  if (usage.cpuUsagePercent >= 0.0 && usage.cpuUsagePercent <= 100.0) {
    std::cout << "[PASS] cpuUsagePercent 范围正常" << std::endl;
  } else {
    std::cerr << "[FAIL] cpuUsagePercent 超出范围: " << usage.cpuUsagePercent
              << std::endl;
    assert(false);
    co_return;
  }

  std::cout << "[INFO] 内存总量: " << usage.memory.totalPhysicalMB << " MB"
            << std::endl;
  std::cout << "[INFO] 已用内存: " << usage.memory.usedPhysicalMB << " MB"
            << std::endl;
  std::cout << "[INFO] 内存使用率: " << usage.memory.usagePercent << "%"
            << std::endl;

  if (usage.memory.totalPhysicalMB > 0) {
    std::cout << "[PASS] totalPhysicalMB > 0" << std::endl;
  } else {
    std::cerr << "[FAIL] totalPhysicalMB 为 0" << std::endl;
    assert(false);
    co_return;
  }

  if (usage.memory.usedPhysicalMB > 0 &&
      usage.memory.usedPhysicalMB <= usage.memory.totalPhysicalMB) {
    std::cout << "[PASS] usedPhysicalMB 范围正常" << std::endl;
  } else {
    std::cerr << "[FAIL] usedPhysicalMB 异常: " << usage.memory.usedPhysicalMB
              << " / " << usage.memory.totalPhysicalMB << std::endl;
    assert(false);
    co_return;
  }

  if (usage.memory.usagePercent >= 0.0 && usage.memory.usagePercent <= 100.0) {
    std::cout << "[PASS] memory.usagePercent 范围正常" << std::endl;
  } else {
    std::cerr << "[FAIL] memory.usagePercent 超出范围: "
              << usage.memory.usagePercent << std::endl;
    assert(false);
    co_return;
  }

  if (usage.gpus.empty()) {
    std::cerr << "[WARNING] 未检测到任何 GPU" << std::endl;
    co_return;
  }

  std::cout << "[INFO] 检测到 " << usage.gpus.size() << " 个 GPU" << std::endl;
  if (usage.gpus.size() >= 1) {
    std::cout << "[PASS] gpu 数量 >= 1" << std::endl;
  }

  for (size_t i = 0; i < usage.gpus.size(); ++i) {
    const auto &gpu = usage.gpus[i];
    std::cout << "  GPU[" << i << "]: name=" << gpu.name << std::endl;
    std::cout << "    专用显存: " << gpu.dedicatedVramUsedMB << " / "
              << gpu.dedicatedVramMB << " MB" << std::endl;
    std::cout << "    共享显存: " << gpu.sharedVramUsedMB << " / "
              << gpu.sharedVramMB << " MB" << std::endl;
    std::cout << "    GPU 使用率: " << gpu.usagePercent << "%" << std::endl;

    if (gpu.name.empty()) {
      std::cerr << "[FAIL] GPU[" << i << "] 名称为空" << std::endl;
      assert(false);
      co_return;
    }
    std::cout << "  [PASS] GPU[" << i << "] 名称有效" << std::endl;

    if (gpu.dedicatedVramMB > 0) {
      std::cout << "  [PASS] GPU[" << i << "] dedicatedVramMB > 0" << std::endl;
    } else {
      std::cerr << "[FAIL] GPU[" << i << "] dedicatedVramMB 为 0" << std::endl;
      assert(false);
      co_return;
    }

    if (gpu.dedicatedVramUsedMB <= gpu.dedicatedVramMB) {
      std::cout << "  [PASS] GPU[" << i << "] dedicatedVramUsedMB 范围正常"
                << std::endl;
    } else {
      std::cerr << "[FAIL] GPU[" << i
                << "] 专用显存已用 > 总量: " << gpu.dedicatedVramUsedMB << " > "
                << gpu.dedicatedVramMB << std::endl;
      assert(false);
      co_return;
    }

    if (gpu.usagePercent >= 0.0 && gpu.usagePercent <= 100.0) {
      std::cout << "  [PASS] GPU[" << i << "] usagePercent 范围正常"
                << std::endl;
    } else {
      std::cerr << "[FAIL] GPU[" << i
                << "] usagePercent 超出范围: " << gpu.usagePercent << std::endl;
      assert(false);
      co_return;
    }
  }

  std::cout << "======= CpuGpuMonitor Test All Passed =======" << std::endl;
#else
  std::cout << "======= CpuGpuMonitor Test =======" << std::endl;
  std::cout << "[SKIP] CpuGpuMonitor 仅支持 Windows/Linux 平台" << std::endl;
  co_return;
#endif
}

} // namespace test
} // namespace agentxx