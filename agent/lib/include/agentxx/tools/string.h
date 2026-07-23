#pragma once

#include "agentxx/tools/tool.h"

namespace agentxx {
namespace tools {

class StringHtml2MarkdownTool : public XXToolBase {
public:

    StringHtml2MarkdownTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

class StringRegexpTool : public XXToolBase {
public:

    StringRegexpTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

} // namespace tools
} // namespace agentxx
