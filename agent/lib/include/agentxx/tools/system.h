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

/// 获取系统核心信息：CPU占用、内存使用、多显卡信息
class GetSystemCoreInfoTool : public XXToolBase {
public:

    GetSystemCoreInfoTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

}; // namespace tools
}; // namespace agentxx
