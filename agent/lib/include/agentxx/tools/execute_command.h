#pragma once

#include "agentxx/tools/tool.h"

namespace agentxx {
namespace tools {

class ExecuteBashCommandTool : public XXToolBase {
public:

    ExecuteBashCommandTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

/// Windows 命令执行 tool
/// - 默认经 PowerShell 执行 (自动探测 pwsh.exe / powershell.exe 并注入版本号到提示词);
///   未找到 PowerShell 时自动回退到 cmd.exe (两种 shell 使用不同的参数提示词)
/// - 命令以独立 argv 元素传递, 绕过 shell 二次解析, 避免引号/$ 转义问题
/// - 支持 WSL (经 interop 调用 Windows 侧 exe)
class ExecuteWindowsCommandTool : public XXToolBase {
public:

    ExecuteWindowsCommandTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

} // namespace tools
} // namespace agentxx
