#include "test_cpu_gpu_use.h"

namespace agentxx {
namespace test {

int g_cpu_passed = 0;
int g_cpu_failed = 0;

asio::awaitable<TestResult> run_cpu_gpu_use_tests() {
#if XX_IS_WIN_D || XX_IS_LINUX_D

  agentxx::expand::CpuGpuMonitor monitor{};

  co_await monitor.query();
  agentxx::expand::CpuGpuUsage usage = co_await monitor.query();

  TEST_INFO << "CPU 使用率: " << usage.cpuUsagePercent << "%" << std::endl;
  if (usage.cpuUsagePercent >= 0.0 && usage.cpuUsagePercent <= 100.0) {
    g_cpu_passed++;
    TEST_PASS << "cpuUsagePercent 范围正常" << std::endl;
  } else {
    g_cpu_failed++;
    TEST_FAIL << "cpuUsagePercent 超出范围: " << usage.cpuUsagePercent
              << std::endl;
    co_return TestResult{g_cpu_passed, g_cpu_failed};
  }

  TEST_INFO << "内存总量: " << usage.memory.totalPhysicalMB << " MB"
            << std::endl;
  TEST_INFO << "已用内存: " << usage.memory.usedPhysicalMB << " MB"
            << std::endl;
  TEST_INFO << "内存使用率: " << usage.memory.usagePercent << "%" << std::endl;

  if (usage.memory.totalPhysicalMB > 0) {
    g_cpu_passed++;
    TEST_PASS << "totalPhysicalMB > 0" << std::endl;
  } else {
    g_cpu_failed++;
    TEST_FAIL << "totalPhysicalMB 为 0" << std::endl;
    co_return TestResult{g_cpu_passed, g_cpu_failed};
  }

  if (usage.memory.usedPhysicalMB > 0 &&
      usage.memory.usedPhysicalMB <= usage.memory.totalPhysicalMB) {
    g_cpu_passed++;
    TEST_PASS << "usedPhysicalMB 范围正常" << std::endl;
  } else {
    g_cpu_failed++;
    TEST_FAIL << "usedPhysicalMB 异常: " << usage.memory.usedPhysicalMB << " / "
              << usage.memory.totalPhysicalMB << std::endl;
    co_return TestResult{g_cpu_passed, g_cpu_failed};
  }

  if (usage.memory.usagePercent >= 0.0 && usage.memory.usagePercent <= 100.0) {
    g_cpu_passed++;
    TEST_PASS << "memory.usagePercent 范围正常" << std::endl;
  } else {
    g_cpu_failed++;
    TEST_FAIL << "memory.usagePercent 超出范围: " << usage.memory.usagePercent
              << std::endl;
    co_return TestResult{g_cpu_passed, g_cpu_failed};
  }

  if (usage.gpus.empty()) {
    TEST_WARN << "未检测到任何 GPU" << std::endl;
    co_return TestResult{g_cpu_passed, g_cpu_failed};
  }

  TEST_INFO << "检测到 " << usage.gpus.size() << " 个 GPU" << std::endl;
  if (usage.gpus.size() >= 1) {
    g_cpu_passed++;
    TEST_PASS << "gpu 数量 >= 1" << std::endl;
  }

  for (size_t i = 0; i < usage.gpus.size(); ++i) {
    const auto &gpu = usage.gpus[i];
    TEST_INFO << "GPU[" << i << "]: name=" << gpu.name << std::endl;
    TEST_INFO << "  专用显存: " << gpu.dedicatedVramUsedMB << " / "
              << gpu.dedicatedVramMB << " MB" << std::endl;
    TEST_INFO << "  共享显存: " << gpu.sharedVramUsedMB << " / "
              << gpu.sharedVramMB << " MB" << std::endl;
    TEST_INFO << "  GPU 使用率: " << gpu.usagePercent << "%" << std::endl;

    if (gpu.name.empty()) {
      g_cpu_failed++;
      TEST_FAIL << "GPU[" << i << "] 名称为空" << std::endl;
      co_return TestResult{g_cpu_passed, g_cpu_failed};
    }
    g_cpu_passed++;
    TEST_PASS << "GPU[" << i << "] 名称有效" << std::endl;

    if (gpu.dedicatedVramMB > 0) {
      g_cpu_passed++;
      TEST_PASS << "GPU[" << i << "] dedicatedVramMB > 0" << std::endl;
    } else {
      g_cpu_failed++;
      TEST_FAIL << "GPU[" << i << "] dedicatedVramMB 为 0" << std::endl;
      co_return TestResult{g_cpu_passed, g_cpu_failed};
    }

    if (gpu.dedicatedVramUsedMB <= gpu.dedicatedVramMB) {
      g_cpu_passed++;
      TEST_PASS << "GPU[" << i << "] dedicatedVramUsedMB 范围正常" << std::endl;
    } else {
      g_cpu_failed++;
      TEST_FAIL << "GPU[" << i
                << "] 专用显存已用 > 总量: " << gpu.dedicatedVramUsedMB << " > "
                << gpu.dedicatedVramMB << std::endl;
      co_return TestResult{g_cpu_passed, g_cpu_failed};
    }

    if (gpu.usagePercent >= 0.0 && gpu.usagePercent <= 100.0) {
      g_cpu_passed++;
      TEST_PASS << "GPU[" << i << "] usagePercent 范围正常" << std::endl;
    } else {
      g_cpu_failed++;
      TEST_FAIL << "GPU[" << i
                << "] usagePercent 超出范围: " << gpu.usagePercent << std::endl;
      co_return TestResult{g_cpu_passed, g_cpu_failed};
    }
  }

  co_return TestResult{g_cpu_passed, g_cpu_failed};
#else
  TEST_SKIP << "CpuGpuMonitor 仅支持 Windows/Linux 平台" << std::endl;
  co_return TestResult{0, 0};
#endif
}

} // namespace test
} // namespace agentxx
