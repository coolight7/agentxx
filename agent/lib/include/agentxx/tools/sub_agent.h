#pragma once

#include "agentxx/tools/tool.h"
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace agentxx {
namespace tools {

/// subagent 注册项基类: 仅承载名称/描述/系统提示等静态元数据
/// - 实际执行由 AgentHost 派生独立 agent 完成 (中断委派, 不再使用图内
///   嵌套 subgraph; 旧的 getSubgraph/onSubagentEnd 已移除)
class SubAgentTaskBase {
public:

    const std::string name;
    const std::string depict;
    std::string       systemPrompt;

    SubAgentTaskBase(
        std::string_view in_subAgentName,
        std::string_view in_subAgentDepict,
        std::string_view in_systemPrompt
    );

    virtual ~SubAgentTaskBase();
};

/// 默认 subagent 任务 (普通委派: 隔离上下文独立运行)
class SubAgentNormalTask : public SubAgentTaskBase {
public:

    SubAgentNormalTask(
        std::string_view in_subAgentName,
        std::string_view in_subAgentDepict
    );
};

class SubAgentManagerTool : public XXToolBase {
public:

    std::map<std::string, std::shared_ptr<SubAgentTaskBase>, std::less<>> subAgentList{};

    SubAgentManagerTool(
        std::string_view                            in_nodeName,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
    );

    std::string get_name() const override;

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

}; // namespace tools
}; // namespace agentxx
