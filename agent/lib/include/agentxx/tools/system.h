#pragma once

#include "agentxx/expand/get_cpu_gpu_use.h"
#include "agentxx/tools/tool.h"
#include "fmt/format.h"
#include <format>
#include <iostream>
#include <map>
#include <neograph/api.h>
#include <neograph/json.h>
#include <sstream>
#include <string>
#include <vector>

namespace agentxx {
namespace tools {

/// 获取系统日期和时间
class GetCurrentDateTimeTool : public XXToolBase {
public:
  GetCurrentDateTimeTool(
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("get_current_datetime", in_agentContext, false, true) {}

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{},
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    auto now = std::chrono::system_clock::now();
    std::chrono::zoned_time local_time{std::chrono::current_zone(), now};

    co_return fmt::format(
        R"(Timestamp: {} millisecond
Local Time (24Hour): {}
UTC Time (24Hour): {})",
        now.time_since_epoch().count() / 1000 / 1000,
        std::format("{:%Y-%m-%d %H:%M:%S}", local_time),
        std::format("{:%Y-%m-%d %H:%M:%S}", now));
  }
};

/// 获取系统核心信息：CPU占用、内存使用、多显卡信息
class GetSystemCoreInfoTool : public XXToolBase {
public:
  GetSystemCoreInfoTool(
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("get_system_core_info", in_agentContext, false, true) {}

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{},
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    expand::CpuGpuMonitor monitor;
    auto usage = co_await monitor.query();

    std::stringstream ss;
    ss << fmt::format("CPU Usage: {:.1f}%\n", usage.cpuUsagePercent);
    ss << fmt::format("Memory: {:.1f}% (Used: {}MB / Total: {}MB)\n",
                      usage.memory.usagePercent, usage.memory.usedPhysicalMB,
                      usage.memory.totalPhysicalMB);

    for (size_t i = 0; i < usage.gpus.size(); ++i) {
      const auto &gpu = usage.gpus[i];
      if (!gpu.name.empty()) {
        ss << fmt::format("GPU {} [{}]: GPU Usage: {:.1f}%, "
                          "VRAM: {}MB Used / {}MB Total",
                          i, gpu.name, gpu.usagePercent,
                          gpu.dedicatedVramUsedMB, gpu.dedicatedVramMB);
      } else {
        ss << fmt::format("GPU {}: GPU Usage: {:.1f}%, "
                          "VRAM: {}MB Used / {}MB Total",
                          i, gpu.usagePercent, gpu.dedicatedVramUsedMB,
                          gpu.dedicatedVramMB);
      }
      if (gpu.sharedVramMB > 0) {
        ss << fmt::format(" (Shared: {}MB Used / {}MB Total)",
                          gpu.sharedVramUsedMB, gpu.sharedVramMB);
      }
      ss << "\n";
    }

    if (usage.gpus.empty()) {
      ss << "GPU: No GPU detected\n";
    }

    co_return ss.str();
  }
};

}; // namespace tools
}; // namespace agentxx