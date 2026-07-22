#pragma once

#include "agentxx/tools/tool.h"

namespace agentxx {
namespace tools {

/// 跨 agent 查询工具
/// - 供任一 agent (含 subagent) 调用, 向另一指定 agent 发起查询
/// - 经总线 service.crossagent 派发, 由目标 agent 的持有者响应
/// - 实现 agent 间 actor 式通信
class CrossAgentQueryTool : public XXToolBase {
public:
  CrossAgentQueryTool(
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

  std::string get_name() const override;

  neograph::ChatTool get_definition() const override;

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override;
};

} // namespace tools
} // namespace agentxx
