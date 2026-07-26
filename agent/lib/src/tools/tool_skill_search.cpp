#include "agentxx/tools/tool_skill_search.h"

#include "agentxx/nodes/agentcall.h"
#include "agentxx/nodes/modelcall.h"
#include "agentxx/nodes/toolcall.h"
#include "agentxx/nodes/wrap_handle.h"
#include "fmt/format.h"
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace agentxx {
namespace tools {

ToolSkillSearchSubAgentTask::ToolSkillSearchSubAgentTask(
    const neograph::graph::NodeContext&         in_context,
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
    createSubgraph(in_context);
    createSystemPrompt();
}

asio::awaitable<void> ToolSkillSearchSubAgentTask::onSubagentEnd(std::string& result) {
    try {
        auto jsonResult = neograph::json::parse(result);
        if (!jsonResult.is_object()) {
            co_return;
        }

        auto agentCtxPtr = agentContext.lock();
        if (!agentCtxPtr) {
            co_return;
        }

        // if (jsonResult["tool"].is_array()) {
        //   auto &loadedTools =
        //       agentCtxPtr->getGraphDataItemValue<std::vector<std::string>>(
        //           "session", graphDataKey_loadedTools);
        //   for (const auto &item : jsonResult["tool"]) {
        //     if (item.is_string()) {
        //       loadedTools.push_back(item.get<std::string>());
        //     }
        //   }
        // }

        // if (jsonResult["skill"].is_array()) {
        //   auto &loadedSkills =
        //       agentCtxPtr->getGraphDataItemValue<std::vector<std::string>>(
        //           "session", graphDataKey_loadedSkills);
        //   for (const auto &item : jsonResult["skill"]) {
        //     if (item.is_string()) {
        //       auto skillPath = item.get<std::string>();
        //       loadedSkills.push_back(skillPath);

        //       auto skillMdPath = skillPath + "/SKILL.md";
        //       std::ifstream stream(skillMdPath);
        //       if (stream) {
        //         auto content =
        //         std::string{std::istreambuf_iterator<char>(stream),
        //                                    std::istreambuf_iterator<char>()};
        //         stream.close();
        //         if (!content.empty()) {
        //           auto &systemMsgList =
        //               agentCtxPtr
        //                   ->getGraphDataItemValue<std::vector<std::string>>(
        //                       "session",
        //                       agentxx::middleware::MiddlewareContext::
        //                           graphDataKey_systemMessage);
        //           systemMsgList.push_back(fmt::format(
        //               "\n## Loaded Skill: {}\n\n{}", skillPath, content));
        //         }
        //       }
        //     }
        //   }
        // }
    } catch (const std::exception& _) {
        // 转json失败则不处理
    }
    co_return;
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

void ToolSkillSearchSubAgentTask::createSubgraph(const neograph::graph::NodeContext& context) {
    if (nullptr == subgraph) {
        auto inner = neograph::graph::GraphEngine::compile(defCreateSubGraphDefine(), context);
        assert(nullptr != inner);
        subgraph = std::shared_ptr<neograph::graph::GraphEngine>(inner.release());
    }
}

neograph::json ToolSkillSearchSubAgentTask::defCreateSubGraphDefine() {
    return neograph::json{
        {"name", "xx_ToolSkillSearch"},
        {
         "channels", {
                {"messages", {{"reducer", "append"}}},
            }, },
        {
         "nodes", {
                {
                    "agent_start",
                    {{
                        "type",
                        agentxx::nodes::AgentStartCallWrapNode::defNodeType,
                    }},
                },
                {
                    "agent_end",
                    {{
                        "type",
                        agentxx::nodes::MiddlewareWrapAgentEndCallNode::defNodeType,
                    }},
                },
                {
                    "tools",
                    {{
                        "type",
                        agentxx::nodes::ToolcallWrapNode::defNodeType,
                    }},
                },
                {
                    "llm",
                    {{
                        "type",
                        agentxx::nodes::ModelCallWrapNode::defNodeType,
                    }},
                },
            }, },
        {
         "edges", neograph::json::array({
                {{"from", "__start__"}, {"to", "llm"}},
                {{"from", "agent_start"}, {"to", "llm"}},
                {
                    {"from", "llm"},
                    {"type", "conditional"},
                    {"condition", "has_tool_calls"},
                    {"routes", {{"true", "tools"}, {"false", "agent_end"}}},
                },
                {{"from", "tools"}, {"to", "llm"}},
                {{"from", "agent_end"}, {"to", "__end__"}},
            }),
         },
    };
}

} // namespace tools
} // namespace agentxx
