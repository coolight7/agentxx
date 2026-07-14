#pragma once

#include "fmt/format.h"
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace agent {

class AgentConfigStatic {
public:
  AgentConfigStatic(int _);

  inline static constexpr std::string_view agentxxDataDirPath = ".agentxx";

  inline static std::string getResultPath(std::string_view parent) noexcept {
    return fmt::format("{}/results/{}", agentxxDataDirPath, parent);
  }

  inline static std::optional<std::string> getCurrentWorkPath() noexcept {
    try {
      auto cwd = std::filesystem::current_path();
      return cwd.string();
    } catch (...) {
      return std::nullopt;
    }
  }
};

} // namespace agent
} // namespace agentxx