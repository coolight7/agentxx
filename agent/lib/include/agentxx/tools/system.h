#pragma once

#include "agentxx/tools/tool.h"

namespace agentxx {
namespace tools {

/// 获取系统日期和时间
class GetCurrentDateTimeTool : public XXToolBase {
public:

    GetCurrentDateTimeTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

}; // namespace tools
}; // namespace agentxx
