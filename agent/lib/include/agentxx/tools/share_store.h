#pragma once

#include "agentxx/tools/tool.h"

namespace agentxx {
namespace tools {

/// - 寄存信息，节省模型上下文、为 llm、node、skill、tool
/// 之间方便传递数据
/// TODO: 支持重启恢复
class SessionShareStoreTool : public XXToolBase {
public:

    SessionShareStoreTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    std::optional<agentxx::middleware::SummarizationToolHandle>
        createSummarizationToolHandle() const override;

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};
} // namespace tools
} // namespace agentxx
