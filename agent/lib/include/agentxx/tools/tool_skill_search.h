#pragma once

#include "agentxx/tools/sub_agent.h"
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace tools {

/// TODO: 尝试 subagent 提交结果时使用 toolcall，以便规范其传输结果的格式
/// 当主模型在已加载的 tool和skill
/// 中未找到可以解决用户需求的方法时，可以尝试调用`tool_skill_search`查找可能可用的
/// tool 或 skill
/// - 需要实现区分 tool 是可延迟加载的，并区分已加载、未加载，未加载的 tool
/// 只需要在主模型的上下文中保留 toolname/skillname 即可
/// - 搜索实现:
///   - 当 tool 和 skill < 30 个时，不延迟加载
///   - 子模型搜索可用的 tool和skill 时，可以预先加载可能需要的 skill
/// 内容，然后对比后决定具体应当加载的 tool、skill 文件
class ToolSkillSearchSubAgentTask : public ::agentxx::tools::SubAgentTaskBase {
public:

    inline static constexpr auto defSystemPromptTemplate = std::string_view{R"(
You are an assistant that, based on user requirements, tries to find the appropriate tools and skills to load. 
You can use filesystem tools to search for and read SKILL.md files, analyze the user's needs to determine usable tools and skills, 
and then output a JSON object in the format: `{{"tool": ["tool_name_1", "tool_name_2"], "skill": ["/absolute/path/to/skill"]}}`.
You can output multiple tools and skills at the same time. 
If no suitable tool is found, output an empty array; similarly, if no suitable skill is found, output an empty array. 
If neither tools nor skills are suitable, output `{{"tool": [], "skill": []}}`.

## Delay-Loadable Tools (available but not yet fully loaded):
{}

## Skill Search Directories:
{}

## Workflow:
1. Analyze the user's requirements to understand what capabilities are needed
2. Use `filesystem_glob` or `filesystem_listfile` to search for SKILL.md files in the skill directories
3. Use `filesystem_read_text_file` to read potentially relevant SKILL.md files (use line_limit=1000)
4. Compare skill content against the user's needs and decide which skills to load
5. Determine which delay-loadable tools would also help
6. Output the final JSON with selected tools and skills

Remember: Output ONLY valid JSON, nothing else before or after.

)"};
    inline static constexpr auto graphDataKey_loadedTools
        = std::string_view{"toolSkillSearch_loadedTools"};
    inline static constexpr auto graphDataKey_loadedSkills
        = std::string_view{"toolSkillSearch_loadedSkills"};

    struct DelayToolInfo {
        std::string name;
        std::string description;
    };

    std::vector<DelayToolInfo>                  delayToolInfos;
    std::vector<std::string>                    skillDirPaths;
    std::weak_ptr<agentxx::agent::AgentContext> agentContext;

    ToolSkillSearchSubAgentTask(
        const neograph::graph::NodeContext&         in_context,
        const std::vector<DelayToolInfo>&           in_delayToolInfos,
        const std::vector<std::string>&             in_skillDirPaths,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
    );

    asio::awaitable<void> onSubagentEnd(std::string& result) override;

    void createSystemPrompt();

    void createSubgraph(const neograph::graph::NodeContext& context);

    inline static neograph::json defCreateSubGraphDefine();
};
} // namespace tools
} // namespace agentxx
