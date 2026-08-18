#include "agentxx/tools/tool_skill_search.h"

#include "fmt/format.h"
#include <sstream>
#include <string>
#include <vector>

namespace agentxx {
namespace tools {

ToolSkillSearchSubAgentTask::ToolSkillSearchSubAgentTask(
    const std::vector<DelayToolInfo>&           in_delayToolInfos,
    const std::vector<std::string>&             in_skillDirPaths,
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    ::agentxx::tools::SubAgentTaskBase(
        "tool_skill_search",
        "Search available tool or skill for loading. "
        "(already set system prompt)",
        ""
    ),
    delayToolInfos(in_delayToolInfos),
    skillDirPaths(in_skillDirPaths),
    agentContext(in_agentContext) {
    createSystemPrompt();
}

void ToolSkillSearchSubAgentTask::createSystemPrompt() {
    std::ostringstream toolsList;
    if (delayToolInfos.empty()) {
        toolsList << "(none)";
    } else {
        for (const auto& item : delayToolInfos) {
            toolsList << fmt::format("- **{}**: {}\n", item.name, item.description);
        }
    }

    std::ostringstream skillsDirs;
    if (skillDirPaths.empty()) {
        skillsDirs << "(none)";
    } else {
        for (const auto& dir : skillDirPaths) {
            skillsDirs << fmt::format("- {}\n", dir);
        }
    }

    systemPrompt = fmt::format(defSystemPromptTemplate, toolsList.str(), skillsDirs.str());
}

} // namespace tools
} // namespace agentxx
