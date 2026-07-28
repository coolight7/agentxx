#pragma once

#include "agentxx/agent/base_agent.h"

namespace agentxx {

namespace middleware {
class SummarizationMiddlewareHandle;
} // namespace middleware

namespace tools {
class SubAgentManagerTool;
} // namespace tools

namespace agent {

/// 代码 Agent: 继承 BaseAgent, 提供完整的编程辅助能力
/// - 文件系统读写/编辑/搜索
/// - 命令执行 (Linux/Windows)
/// - 网络搜索/抓取
/// - RAG 知识库搜索
/// - 子代理管理
/// - MCP 外部工具
/// - 权限控制 / 技能发现 / 上下文压缩 / 任务规划
class CodeAgent : public BaseAgent {
public:

    CodeAgent(std::shared_ptr<agentxx::agent::AgentConfig> in_config);

    ~CodeAgent() override;

protected:

    asio::awaitable<void> setupMiddleware() override;

    asio::awaitable<std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>>
        createTools() override;

private:

    /// subagent 管理工具 (在 setupMiddleware 中创建, createTools 中完成配置)
    std::unique_ptr<agentxx::tools::SubAgentManagerTool> subagentManagerTool_;
    /// summarization 中间件 (在 setupMiddleware 中创建, createTools 后关联压缩句柄)
    std::shared_ptr<agentxx::middleware::SummarizationMiddlewareHandle> summarizationMiddleware_;
};

} // namespace agent
} // namespace agentxx
