#pragma once

#include "agentxx/tools/tool.h"

namespace agentxx {
namespace tools {

/// 文件读取 tool 的去重 key 生成（包含 path + offset/limit 等参数）
std::optional<std::string> _defFileReadGenerateKey(const neograph::json& args);

/// 文件写入/编辑 tool 的去重 key 生成（仅用 path，最新写入覆盖旧写入）
std::optional<std::string> _defFileWriteGenerateKey(const neograph::json& args);

void _defTruncateToolcallRequest(neograph::ToolCall& toolcall);

void _defTruncateToolcallResponse(neograph::ChatMessage& msg);

/// ls
class FileSystemListTool : public XXToolBase {
public:

    FileSystemListTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    std::optional<agentxx::middleware::SummarizationToolHandle>
        createSummarizationToolHandle() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

/// read
class FilesystemReadTextFileTool : public XXToolBase {
protected:
public:

    FilesystemReadTextFileTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    std::optional<agentxx::middleware::SummarizationToolHandle>
        createSummarizationToolHandle() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

/// read
class FilesystemReadBinaryFileTool : public XXToolBase {
public:

    FilesystemReadBinaryFileTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    std::optional<agentxx::middleware::SummarizationToolHandle>
        createSummarizationToolHandle() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

/// write
class FilesystemWriteFileTool : public XXToolBase {
public:

    FilesystemWriteFileTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    std::optional<agentxx::middleware::SummarizationToolHandle>
        createSummarizationToolHandle() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

/// edit file
class FilesystemEditTextFileTool : public XXToolBase {
public:

    FilesystemEditTextFileTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    std::optional<agentxx::middleware::SummarizationToolHandle>
        createSummarizationToolHandle() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

class FilesystemGlobTool : public XXToolBase {
public:

    FilesystemGlobTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

class FilesystemGrepTool : public XXToolBase {
public:

    FilesystemGrepTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> readFileContent(const std::string& filepath);

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

} // namespace tools
} // namespace agentxx
