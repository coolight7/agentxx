#pragma once

#include "agentxx/util/exception.h"
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
        return agentxx::util::catchError<std::optional<std::string>>(
            []() -> std::optional<std::string> {
                auto cwd = std::filesystem::current_path();
                return cwd.string();
            },
            [](std::string) -> std::optional<std::string> { return std::nullopt; }
        );
    }
};

} // namespace agent
} // namespace agentxx