#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/tools/tool.h"
#include <memory>
#include <string>
#include <string_view>

namespace agentxx {
namespace tools {

class WebSearchTool : public XXToolBase {
protected:

    const std::string searchApiUrl;
    const bool        convertHtml2markdown;

public:

    WebSearchTool(
        std::string_view                            in_searchApiUrl,
        bool                                        in_convertHtml2markdown,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
    );

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

class WebFetchUrlTool : public XXToolBase {
public:

    WebFetchUrlTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

class WebFetchUrlMarkdownTool : public XXToolBase {
public:

    WebFetchUrlMarkdownTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

/// 使用模型进行网络搜索的 Tool
/// - 通过调用支持联网搜索的模型（如 OpenAI web_search 等）获取搜索结果
/// - 由 [AgentConfig::websearchModel] 配置驱动
/// - 执行时支持 `timeout` / `header` 参数覆盖模型请求的超时与自定义请求头
class ModelWebSearchTool : public XXToolBase {
protected:

    /// 基础模型配置 (每次执行时复制一份, 应用 timeout/header 参数覆盖后再创建 provider)
    agentxx::agent::ModelConfig modelCfg;

public:

    ModelWebSearchTool(
        const agentxx::agent::ModelConfig&          modelCfg,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
    );

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

} // namespace tools
} // namespace agentxx
