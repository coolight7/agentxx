#pragma once

#include "agentxx/tools/tool.h"
#include <map>
#include <memory>
#include <neograph/graph/engine.h>
#include <neograph/graph/types.h>
#include <string>
#include <string_view>

namespace agentxx {
namespace tools {

class SubAgentTaskBase {
protected:

    std::shared_ptr<neograph::graph::GraphEngine> subgraph = nullptr;

public:

    const std::string name;
    const std::string depict;
    std::string       systemPrompt;

    SubAgentTaskBase(std::string_view in_subAgentName,
                     std::string_view in_subAgentDepict,
                     std::string_view in_systemPrompt);

    virtual std::shared_ptr<neograph::graph::GraphEngine> getSubgraph() const;

    virtual asio::awaitable<void> onSubagentEnd(std::string& result);

    virtual ~SubAgentTaskBase();
};

class SubAgentNormalTask : public SubAgentTaskBase {
public:

    SubAgentNormalTask(std::string_view                    in_subAgentName,
                       std::string_view                    in_subAgentDepict,
                       const neograph::graph::NodeContext& in_context);

    void createSubgraph(const neograph::graph::NodeContext& context);

    inline static neograph::json defCreateSubGraphDefine();
};

class SubAgentManagerTool : public XXToolBase {
public:

    std::map<std::string, std::shared_ptr<SubAgentTaskBase>> subAgentList{};

    SubAgentManagerTool(std::string_view                            in_nodeName,
                        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    std::string get_name() const override;

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

}; // namespace tools
}; // namespace agentxx
