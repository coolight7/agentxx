#pragma once

#include "agentxx/agent/base_agent.h"

namespace agentxx {

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
/// - 插件 (codegraph 代码分析 / computer_use 桌面控制等) 统一经 yaml
///   `plugins` 段 path 指定加载
class CodeAgent : public BaseAgent {
public:

    CodeAgent(std::shared_ptr<agentxx::agent::AgentConfig> in_config);

    ~CodeAgent() override;

protected:

    asio::awaitable<void> initMiddleware() override;

    asio::awaitable<std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>> initTools() override;

private:

    /// subagent 管理工具 (在 initMiddleware 中创建, initTools 中完成配置)
    std::unique_ptr<agentxx::tools::SubAgentManagerTool> subagentManagerTool_;
    // (summarization 中间件由 AgentContext::summarizationMiddleware 持有,
    //  CodeAgent 不再单独保存)
};

} // namespace agent
} // namespace agentxx
