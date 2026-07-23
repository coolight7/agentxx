#pragma once

#include "agentxx/tools/tool.h"
#include <memory>
#include <string>

#if AGENTXX_ENABLE_CODEGRAPH

namespace agentxx {
namespace expand {
class CodeGraphManager;
} // namespace expand

namespace tools {

class CodeGraphSearchTool : public XXToolBase {
protected:

    std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:

    CodeGraphSearchTool(std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
                        std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

class CodeGraphContextTool : public XXToolBase {
protected:

    std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:

    CodeGraphContextTool(std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
                         std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

class CodeGraphCallersTool : public XXToolBase {
protected:

    std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:

    CodeGraphCallersTool(std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
                         std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

class CodeGraphCalleesTool : public XXToolBase {
protected:

    std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:

    CodeGraphCalleesTool(std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
                         std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

class CodeGraphImpactTool : public XXToolBase {
protected:

    std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:

    CodeGraphImpactTool(std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
                        std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

class CodeGraphStatusTool : public XXToolBase {
protected:

    std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:

    CodeGraphStatusTool(std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
                        std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

class CodeGraphIndexTool : public XXToolBase {
protected:

    std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:

    CodeGraphIndexTool(std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
                       std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

class CodeGraphPathTool : public XXToolBase {
protected:

    std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:

    CodeGraphPathTool(std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
                      std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

} // namespace tools
} // namespace agentxx

#endif
