#pragma once

#include "agentxx/expand/codegraph_manager.h"
#include "agentxx/tools/tool.h"
#include "fmt/format.h"
#include "neograph/neograph.h"
#include <memory>
#include <sstream>
#include <string>

#if AGENTXX_ENABLE_CODEGRAPH

namespace agentxx {
namespace tools {

class CodeGraphSearchTool : public XXToolBase {
protected:
  std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:
  CodeGraphSearchTool(
      std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("codegraph_search", in_agentContext, true, true),
        codegraph(std::move(in_codegraph)) {}

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
            {"type", "object"},
            {
                "properties",
                {
                    {
                        "query",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("query")},
                        },
                    },
                    {
                        "limit",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("limit")},
                        },
                    },
                },
            },
            {"required", neograph::json::array({"query"})},
        },
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    std::string query = arguments.value("query", std::string{});
    if (query.empty()) {
      co_return R"({"error":"Arg `query` is empty"})";
    }
    int limit = static_cast<int>(arguments.value("limit", 20.0));

    auto result = codegraph->searchSymbols(query, limit);
    if (!result.success) {
      co_return fmt::format(R"({{"error":"{}"}})", result.error);
    }

    auto json = neograph::json::array();
    for (const auto &node : result.nodes) {
      json.push_back({
          {"kind", codegraph::node_kind_str(node.kind)},
          {"name", node.name},
          {"qualified_name", node.qualified_name},
          {"file", node.file_path},
          {"line", node.line},
          {"signature", node.signature},
      });
    }
    co_return json.dump();
  }
};

class CodeGraphContextTool : public XXToolBase {
protected:
  std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:
  CodeGraphContextTool(
      std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("codegraph_context", in_agentContext, true, true),
        codegraph(std::move(in_codegraph)) {}

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
            {"type", "object"},
            {
                "properties",
                {
                    {
                        "symbol",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("symbol")},
                        },
                    },
                    {
                        "limit",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("limit")},
                        },
                    },
                    {
                        "max_depth",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("max_depth")},
                        },
                    },
                },
            },
            {"required", neograph::json::array({"symbol"})},
        },
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    std::string symbol = arguments.value("symbol", std::string{});
    if (symbol.empty()) {
      co_return R"({"error":"Arg `symbol` is empty"})";
    }
    int limit = static_cast<int>(arguments.value("limit", 10.0));
    int max_depth = static_cast<int>(arguments.value("max_depth", 3.0));

    auto result = codegraph->getSymbolContext(symbol, limit, max_depth);
    if (!result.success) {
      co_return fmt::format(R"({{"error":"{}"}})", result.error);
    }

    co_return result.context.dump();
  }
};

class CodeGraphCallersTool : public XXToolBase {
protected:
  std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:
  CodeGraphCallersTool(
      std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("codegraph_callers", in_agentContext, true, true),
        codegraph(std::move(in_codegraph)) {}

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
            {"type", "object"},
            {
                "properties",
                {
                    {
                        "symbol",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("symbol")},
                        },
                    },
                    {
                        "max_depth",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("max_depth")},
                        },
                    },
                },
            },
            {"required", neograph::json::array({"symbol"})},
        },
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    std::string symbol = arguments.value("symbol", std::string{});
    if (symbol.empty()) {
      co_return R"({"error":"Arg `symbol` is empty"})";
    }
    int max_depth = static_cast<int>(arguments.value("max_depth", 3.0));

    auto result = codegraph->getCallers(symbol, max_depth);
    if (!result.success) {
      co_return fmt::format(R"({{"error":"{}"}})", result.error);
    }
    co_return result.impact.dump();
  }
};

class CodeGraphCalleesTool : public XXToolBase {
protected:
  std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:
  CodeGraphCalleesTool(
      std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("codegraph_callees", in_agentContext, true, true),
        codegraph(std::move(in_codegraph)) {}

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
            {"type", "object"},
            {
                "properties",
                {
                    {
                        "symbol",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("symbol")},
                        },
                    },
                    {
                        "max_depth",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("max_depth")},
                        },
                    },
                },
            },
            {"required", neograph::json::array({"symbol"})},
        },
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    std::string symbol = arguments.value("symbol", std::string{});
    if (symbol.empty()) {
      co_return R"({"error":"Arg `symbol` is empty"})";
    }
    int max_depth = static_cast<int>(arguments.value("max_depth", 3.0));

    auto result = codegraph->getCallees(symbol, max_depth);
    if (!result.success) {
      co_return fmt::format(R"({{"error":"{}"}})", result.error);
    }
    co_return result.impact.dump();
  }
};

class CodeGraphImpactTool : public XXToolBase {
protected:
  std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:
  CodeGraphImpactTool(
      std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("codegraph_impact", in_agentContext, true, true),
        codegraph(std::move(in_codegraph)) {}

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
            {"type", "object"},
            {
                "properties",
                {
                    {
                        "symbol",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("symbol")},
                        },
                    },
                    {
                        "max_depth",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("max_depth")},
                        },
                    },
                },
            },
            {"required", neograph::json::array({"symbol"})},
        },
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    std::string symbol = arguments.value("symbol", std::string{});
    if (symbol.empty()) {
      co_return R"({"error":"Arg `symbol` is empty"})";
    }
    int max_depth = static_cast<int>(arguments.value("max_depth", 5.0));

    auto result = codegraph->getImpact(symbol, max_depth);
    if (!result.success) {
      co_return fmt::format(R"({{"error":"{}"}})", result.error);
    }
    co_return result.impact.dump();
  }
};

class CodeGraphStatusTool : public XXToolBase {
protected:
  std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:
  CodeGraphStatusTool(
      std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("codegraph_status", in_agentContext, false, true),
        codegraph(std::move(in_codegraph)) {}

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
            {"type", "object"},
            {"properties", neograph::json::object()},
        },
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    auto result = codegraph->getStatus();
    if (!result.success) {
      co_return fmt::format(R"({{"error":"{}"}})", result.error);
    }

    co_return fmt::format(
        R"({{"total_nodes":{},"total_edges":{},"total_files":{},"circular_deps":{}}})",
        result.total_nodes, result.total_edges, result.total_files,
        result.circular_deps);
  }
};

class CodeGraphIndexTool : public XXToolBase {
protected:
  std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:
  CodeGraphIndexTool(
      std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("codegraph_index", in_agentContext, false, false),
        codegraph(std::move(in_codegraph)) {}

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
            {"type", "object"},
            {
                "properties",
                {
                    {
                        "path",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("path")},
                        },
                    },
                    {
                        "incremental",
                        {
                            {"type", "boolean"},
                            {"description", prompt.getArg("incremental")},
                        },
                    },
                },
            },
            {"required", neograph::json::array({"path"})},
        },
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    std::string path = arguments.value("path", std::string{});
    if (path.empty()) {
      co_return R"({"error":"Arg `path` is empty"})";
    }
    bool incremental = arguments.value("incremental", true);

    bool ok = codegraph->indexDirectory(path, incremental);
    auto status = codegraph->getStatus();

    if (ok) {
      co_return fmt::format(
          R"({{"success":true,"total_nodes":{},"total_edges":{},"total_files":{}}})",
          status.total_nodes, status.total_edges, status.total_files);
    } else {
      co_return R"({"error":"Indexing failed"})";
    }
  }
};

class CodeGraphPathTool : public XXToolBase {
protected:
  std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:
  CodeGraphPathTool(
      std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("codegraph_path", in_agentContext, true, true),
        codegraph(std::move(in_codegraph)) {}

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
            {"type", "object"},
            {
                "properties",
                {
                    {
                        "from",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("from")},
                        },
                    },
                    {
                        "to",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("to")},
                        },
                    },
                    {
                        "max_depth",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("max_depth")},
                        },
                    },
                },
            },
            {"required", neograph::json::array({"from", "to"})},
        },
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    std::string from = arguments.value("from", std::string{});
    std::string to = arguments.value("to", std::string{});
    if (from.empty() || to.empty()) {
      co_return R"({"error":"Args `from` and `to` are required"})";
    }
    int max_depth = static_cast<int>(arguments.value("max_depth", 10.0));

    auto result = codegraph->findPath(from, to, max_depth);
    if (!result.success) {
      co_return fmt::format(R"({{"error":"{}"}})", result.error);
    }

    auto json = neograph::json::array();
    for (const auto &node : result.path) {
      json.push_back({
          {"kind", codegraph::node_kind_str(node.kind)},
          {"name", node.name},
          {"file", node.file_path},
          {"line", node.line},
      });
    }
    co_return fmt::format(R"({{"path":{},"depth":{}}})", json.dump(),
                          result.path.size());
  }
};

} // namespace tools
} // namespace agentxx

#endif