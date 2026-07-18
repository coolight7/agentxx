#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <neograph/json.h>
#include <asio/awaitable.hpp>

#include "agentxx/util/http_server.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"

namespace agentxx {
namespace server {

using json = neograph::json;

// ---------------------------------------------------------------------------
// JSON-RPC helpers
// ---------------------------------------------------------------------------

inline json jsonRpcError(int code, std::string_view message,
                         std::optional<json> data = {}) {
  json err;
  err["code"] = code;
  err["message"] = std::string(message);
  if (data.has_value())
    err["data"] = std::move(*data);
  return err;
}

inline json jsonRpcResponse(json id, json result) {
  json resp;
  resp["jsonrpc"] = "2.0";
  resp["id"] = std::move(id);
  resp["result"] = std::move(result);
  return resp;
}

inline json jsonRpcErrorResponse(json id, json error) {
  json resp;
  resp["jsonrpc"] = "2.0";
  resp["id"] = std::move(id);
  resp["error"] = std::move(error);
  return resp;
}

// Standard JSON-RPC error codes
inline constexpr int kJsonRpcParseError = -32700;
inline constexpr int kJsonRpcInvalidRequest = -32600;
inline constexpr int kJsonRpcMethodNotFound = -32601;
inline constexpr int kJsonRpcInvalidParams = -32602;
inline constexpr int kJsonRpcInternalError = -32603;

// MCP-specific error codes
inline constexpr int kMcpToolNotFound = -32000;
inline constexpr int kMcpToolExecutionError = -32001;
inline constexpr int kMcpResourceNotFound = -32002;
inline constexpr int kMcpPromptNotFound = -32003;

// ---------------------------------------------------------------------------
// Data types
// ---------------------------------------------------------------------------

struct McpToolDefinition {
  std::string name;
  std::string description;
  std::string title;          // 2025-11-25: display name
  json inputSchema = json::object();
  json outputSchema = json::object();  // 2025-11-25: structured output schema
  json annotations = json::object();   // 2025-11-25: tool behavior metadata
  json execution = json::object();     // 2025-11-25: execution config
};

struct McpResourceDefinition {
  std::string uri;
  std::string name;
  std::string description;
  std::string mimeType;
};

struct McpResourceContent {
  std::string uri;
  std::string mimeType;
  std::string text;
};

struct McpPromptArgument {
  std::string name;
  std::string description;
  bool required = false;
};

struct McpPromptDefinition {
  std::string name;
  std::string description;
  std::vector<McpPromptArgument> arguments;
};

struct McpPromptMessage {
  std::string role;  // "user" | "assistant"
  json content;
};

struct McpPromptResult {
  std::string description;
  std::vector<McpPromptMessage> messages;
};

// Supported MCP protocol versions (newest first for negotiation)
inline constexpr std::string_view kMcpProtocol2024_11_05 = "2024-11-05";
inline constexpr std::string_view kMcpProtocol2025_03_26 = "2025-03-26";
inline constexpr std::string_view kMcpProtocol2025_11_25 = "2025-11-25";
inline constexpr std::string_view kMcpSupportedProtocols[] = {
    kMcpProtocol2025_11_25,
    kMcpProtocol2025_03_26,
    kMcpProtocol2024_11_05,
};

// ---------------------------------------------------------------------------
// McpServer
// ---------------------------------------------------------------------------

class McpServer {
public:
  using ToolHandler = std::function<json(const json &arguments)>;
  using ResourceReader =
      std::function<std::optional<McpResourceContent>(const std::string &uri)>;
  using PromptHandler =
      std::function<std::optional<McpPromptResult>(
          const std::string &name, const json &arguments)>;

  struct Config {
    util::HttpServer::Config httpConfig;
    std::string mcpEndpoint = "/mcp";
    std::string sseEndpoint = "/mcp/sse";
    std::string serverName = "agentxx-mcp";
    std::string serverVersion = "0.1.0";
    std::chrono::seconds toolTimeout{60};
    size_t maxMessageSize = 4 * 1024 * 1024; // 4 MB
  };

  explicit McpServer() : config_(), httpServer_(std::make_unique<util::HttpServer>(util::HttpServer::Config{})) {
    setupRoutes();
  }

  explicit McpServer(Config config) : config_(std::move(config)) {
    httpServer_ = std::make_unique<util::HttpServer>(config_.httpConfig);
    setupRoutes();
  }

  McpServer(const McpServer &) = delete;
  McpServer &operator=(const McpServer &) = delete;

  ~McpServer() { stop(); }

  // -----------------------------------------------------------------------
  // Lifecycle
  // -----------------------------------------------------------------------

  void start() { httpServer_->start(); }

  void stop() {
    stopSSE();
    httpServer_->stop();
  }

  /// Run the server over stdin/stdout using newline-delimited JSON.
  /// Blocks until stdin is closed (EOF / Ctrl-D).
  void runStdio() {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.empty())
        continue;

      json requestJson;
      try {
        requestJson = json::parse(line);
      } catch (const json::parse_error &e) {
        json errorResp = jsonRpcErrorResponse(
            json{nullptr},
            jsonRpcError(kJsonRpcParseError,
                         std::string("Parse error: ") + e.what()));
        std::cout << errorResp.dump() << "\n" << std::flush;
        continue;
      }

      json response = processJsonRpc(requestJson);
      if (!response.is_null()) {
        std::cout << response.dump() << "\n" << std::flush;
      }
    }
  }

  uint16_t port() const { return httpServer_->port(); }
  size_t activeConnections() const { return httpServer_->activeConnections(); }
  bool isStopped() const { return httpServer_->isStopped(); }

  // -----------------------------------------------------------------------
  // Tool registration
  // -----------------------------------------------------------------------

  void addTool(McpToolDefinition def, ToolHandler handler) {
    const auto name = def.name;
    toolsByName_[name] = ToolEntry{std::move(def), std::move(handler)};
    toolsListChanged_ = true;
  }

  void removeTool(const std::string &name) {
    toolsByName_.erase(name);
    toolsListChanged_ = true;
  }

  std::vector<McpToolDefinition> listTools() const {
    std::vector<McpToolDefinition> result;
    result.reserve(toolsByName_.size());
    for (const auto &[key, entry] : toolsByName_)
      result.push_back(entry.def);
    return result;
  }

  void addResource(McpResourceDefinition def, ResourceReader reader) {
    const auto uri = def.uri;
    resourcesByUri_[uri] = ResourceEntry{std::move(def), std::move(reader)};
    resourcesListChanged_ = true;
  }

  void removeResource(const std::string &uri) {
    resourcesByUri_.erase(uri);
    resourcesListChanged_ = true;
  }

  std::vector<McpResourceDefinition> listResources() const {
    std::vector<McpResourceDefinition> result;
    result.reserve(resourcesByUri_.size());
    for (const auto &[key, entry] : resourcesByUri_)
      result.push_back(entry.def);
    return result;
  }

  // -----------------------------------------------------------------------
  // Prompt registration
  // -----------------------------------------------------------------------

  void addPrompt(McpPromptDefinition def, PromptHandler handler) {
    const auto name = def.name;
    promptsByName_[name] = PromptEntry{std::move(def), std::move(handler)};
    promptsListChanged_ = true;
  }

  void removePrompt(const std::string &name) {
    promptsByName_.erase(name);
    promptsListChanged_ = true;
  }

  std::vector<McpPromptDefinition> listPrompts() const {
    std::vector<McpPromptDefinition> result;
    result.reserve(promptsByName_.size());
    for (const auto &[key, entry] : promptsByName_)
      result.push_back(entry.def);
    return result;
  }

  // -----------------------------------------------------------------------
  // Capabilities
  // -----------------------------------------------------------------------

  struct Capabilities {
    bool tools = true;
    bool resources = false;
    bool prompts = false;
    bool logging = false;
  };

  void setCapabilities(Capabilities caps) { capabilities_ = caps; }

  const Capabilities &capabilities() const { return capabilities_; }

private:
  struct ToolEntry {
    McpToolDefinition def;
    ToolHandler handler;
  };

  struct ResourceEntry {
    McpResourceDefinition def;
    ResourceReader reader;
  };

  struct PromptEntry {
    McpPromptDefinition def;
    PromptHandler handler;
  };

  struct SSEClient {
    util::HttpServer::Request *reqPtr;
    util::HttpServer::Response *respPtr;
    std::shared_ptr<std::mutex> writeMutex;
    bool closed = false;
  };

  // -----------------------------------------------------------------------
  // Route setup
  // -----------------------------------------------------------------------

  void setupRoutes() {
    using Handler = util::HttpServer::Handler;

    auto mcpHandler = std::make_shared<Handler>(
        Handler([this](util::HttpServer::Request &req,
                       util::HttpServer::Response &resp,
                       const std::string &) -> asio::awaitable<void> {
          co_await handleMcpRequest(req, resp);
        }));
    httpServer_->router().add(config_.mcpEndpoint, 2, mcpHandler);

    auto sseHandler = std::make_shared<Handler>(
        Handler([this](util::HttpServer::Request &req,
                       util::HttpServer::Response &resp,
                       const std::string &) -> asio::awaitable<void> {
          co_await handleSseRequest(req, resp);
        }));
    httpServer_->router().add(config_.sseEndpoint, 0, sseHandler);
  }

  // -----------------------------------------------------------------------
  // JSON-RPC request processing (transport-agnostic)
  // -----------------------------------------------------------------------

  /// Process a parsed JSON-RPC request and return the response envelope.
  /// Returns a null json value for notifications (no response expected).
  /// Never throws – all handler exceptions are caught internally.
  json processJsonRpc(const json &requestJson) {
    // Be lenient with jsonrpc field: accept "2.0", 2.0 (number), or missing
    if (!requestJson.is_object()) {
      return jsonRpcErrorResponse(
          json{nullptr},
          jsonRpcError(kJsonRpcInvalidRequest, "Request must be a JSON object"));
    }
    auto jrpc = requestJson.value("jsonrpc", json{});
    bool validJsonRpc = false;
    if (jrpc.is_string() && jrpc.get<std::string>() == "2.0")
      validJsonRpc = true;
    else if (jrpc.is_number() && jrpc.get<double>() == 2.0)
      validJsonRpc = true;
    // If jsonrpc field is missing, be lenient and treat as 2.0
    if (requestJson.contains("jsonrpc") && !validJsonRpc) {
      return jsonRpcErrorResponse(
          json{nullptr},
          jsonRpcError(kJsonRpcInvalidRequest,
                       "Unsupported JSON-RPC version"));
    }

    std::string method = requestJson.value("method", "");
    bool hasId =
        requestJson.contains("id") && !requestJson["id"].is_null();
    json id = hasId ? requestJson["id"] : json{};
    json params = requestJson.contains("params")
                      ? requestJson["params"]
                      : json::object();

    // Extract _meta for passthrough (2025-03-26+)
    json meta;
    if (params.contains("_meta") && params["_meta"].is_object())
      meta = params["_meta"];

    // Notifications
    if (method == "notifications/initialized") {
      handleInitialized(params);
      return hasId ? jsonRpcResponse(id, json::object()) : json{};
    }
    if (method == "notifications/cancelled") {
      return hasId ? jsonRpcResponse(id, json::object()) : json{};
    }
    if (method == "notifications/message") {
      return json{}; // Server-to-client notification, not handled here
    }
    if (method.empty()) {
      return jsonRpcErrorResponse(
          json{nullptr},
          jsonRpcError(kJsonRpcInvalidRequest, "Missing method"));
    }

    json response;
    try {
      if (method == "initialize") {
        response = handleInitialize(id, params);
      } else if (method == "ping") {
        response = handlePing(id);
      } else if (method == "tools/list") {
        response = handleToolsList(id, params);
      } else if (method == "tools/call") {
        response = handleToolsCall(id, params);
      } else if (method == "resources/list") {
        response = handleResourcesList(id, params);
      } else if (method == "resources/read") {
        response = handleResourcesRead(id, params);
      } else if (method == "resources/subscribe") {
        response = handleResourcesSubscribe(id, params);
      } else if (method == "resources/unsubscribe") {
        response = handleResourcesUnsubscribe(id, params);
      } else if (method == "resources/templates/list") {
        response = handleResourceTemplatesList(id, params);
      } else if (method == "prompts/list") {
        response = handlePromptsList(id, params);
      } else if (method == "prompts/get") {
        response = handlePromptsGet(id, params);
      } else if (method == "logging/setLevel") {
        response = handleLoggingSetLevel(id, params);
      } else if (method == "completion/complete") {
        response = handleComplete(id, params);
      } else {
        response = jsonRpcErrorResponse(
            id, jsonRpcError(kJsonRpcMethodNotFound,
                             std::string("Method not found: ") + method));
      }
      // Attach _meta passthrough if present in request (2025-03-26+)
      if (!response.is_null() && !meta.is_null() && response.contains("result")) {
        if (!response["result"].contains("_meta"))
          response["result"]["_meta"] = meta;
      }
    } catch (const std::exception &e) {
      XX_LOGE("[mcp] Handler error [{}]: {}", method, e.what());
      response = jsonRpcErrorResponse(
          id, jsonRpcError(kJsonRpcInternalError,
                           std::string("Internal error: ") + e.what()));
    }

    if (!hasId) {
      return json{};
    }
    return response;
  }

  // -----------------------------------------------------------------------
  // Main MCP request handler (HTTP)
  // -----------------------------------------------------------------------

  asio::awaitable<void>
  handleMcpRequest(util::HttpServer::Request &req,
                   util::HttpServer::Response &resp) {
    namespace http = boost::beast::http;

    json requestJson;
    try {
      requestJson = json::parse(req.body());
    } catch (const json::parse_error &e) {
      auto errorResp = jsonRpcErrorResponse(
          json{nullptr},
          jsonRpcError(kJsonRpcParseError,
                       std::string("Parse error: ") + e.what()));
      writeJsonResponse(resp, http::status::bad_request, errorResp);
      co_return;
    }

    json response = processJsonRpc(requestJson);
    if (response.is_null()) {
      resp.result(http::status::accepted);
      co_return;
    }

    writeJsonResponse(resp, http::status::ok, response);
  }

  // -----------------------------------------------------------------------
  // Method handlers
  // -----------------------------------------------------------------------

  json handleInitialize(const json &id, const json &params) {
    std::string clientVersion =
        params.value("protocolVersion", std::string{kMcpProtocol2024_11_05});

    // Negotiate protocol version (per 2025-11-25 spec):
    // Pick the newest mutually supported version.
    // Supported list is ordered newest-first.
    std::string negotiatedVersion{kMcpProtocol2024_11_05};
    bool foundExact = false;
    for (const auto &sv : kMcpSupportedProtocols) {
      if (sv == clientVersion) {
        negotiatedVersion = sv;
        foundExact = true;
        break;
      }
    }
    // If no exact match, pick our newest below client's version
    if (!foundExact && !clientVersion.empty()) {
      auto clientYear = clientVersion.substr(0, 4);
      for (const auto &sv : kMcpSupportedProtocols) {
        auto svYear = sv.substr(0, 4);
        if (svYear <= clientYear) {
          negotiatedVersion = sv;
          break;
        }
      }
    }

    json capabilities;
    capabilities["experimental"] = json::object();
    if (capabilities_.tools)
      capabilities["tools"] = json::object();
    if (capabilities_.resources)
      capabilities["resources"] = json::object();
    if (capabilities_.prompts)
      capabilities["prompts"] = json::object();
    if (capabilities_.logging)
      capabilities["logging"] = json::object();
    // 2025-11-25: declare tasks support
    capabilities["tasks"] = json::object();

    json serverInfo;
    serverInfo["name"] = config_.serverName;
    serverInfo["version"] = config_.serverVersion;

    json result;
    result["protocolVersion"] = negotiatedVersion;
    result["capabilities"] = std::move(capabilities);
    result["serverInfo"] = std::move(serverInfo);
    // 2025-11-25: optional instructions field
    result["instructions"] = "MCP server powered by agentxx";

    return jsonRpcResponse(id, std::move(result));
  }

  json handlePing(const json &id) {
    return jsonRpcResponse(id, json::object());
  }

  json handleToolsList(const json &id, const json &) {
    auto tools = listTools();
    json result;
    json toolsJson = json::array();
    for (const auto &t : tools) {
      json tj;
      tj["name"] = t.name;
      tj["description"] = t.description;
      tj["inputSchema"] = t.inputSchema;
      if (!t.title.empty())
        tj["title"] = t.title;
      if (!t.outputSchema.is_null() && t.outputSchema.is_object() &&
          !t.outputSchema.empty())
        tj["outputSchema"] = t.outputSchema;
      if (!t.annotations.is_null() && t.annotations.is_object() &&
          !t.annotations.empty())
        tj["annotations"] = t.annotations;
      if (!t.execution.is_null() && t.execution.is_object() &&
          !t.execution.empty())
        tj["execution"] = t.execution;
      toolsJson.push_back(std::move(tj));
    }
    result["tools"] = std::move(toolsJson);
    return jsonRpcResponse(id, std::move(result));
  }

  json handleToolsCall(const json &id, const json &params) {
    std::string name = params.value("name", "");
    json arguments = params.contains("arguments") ? params["arguments"] : json::object();

    if (name.empty()) {
      return jsonRpcErrorResponse(
          id, jsonRpcError(kJsonRpcInvalidParams, "Missing tool name"));
    }

    ToolHandler handler;
    {
      std::shared_lock lock(toolsMutex_);
      auto it = toolsByName_.find(name);
      if (it == toolsByName_.end()) {
        return jsonRpcErrorResponse(
            id, jsonRpcError(kMcpToolNotFound,
                             std::string("Tool not found: ") + name));
      }
      handler = it->second.handler;
    }

    json result;
    try {
      json content = handler(arguments);
      result["content"] = json::array();
      result["content"].push_back(std::move(content));
      result["isError"] = false;
      // The handler can optionally set structuredContent on the result
      // by including it in the returned json (checked at call site)
    } catch (const std::exception &e) {
      XX_LOGE("[mcp] Tool execution error [{}]: {}", name, e.what());
      json content;
      content["type"] = "text";
      content["text"] = std::string("Error: ") + e.what();
      result["content"] = json::array();
      result["content"].push_back(std::move(content));
      result["isError"] = true;
    }

    return jsonRpcResponse(id, std::move(result));
  }

  json handleResourcesList(const json &id, const json &) {
    auto resources = listResources();
    json result;
    json resourcesJson = json::array();
    for (const auto &r : resources) {
      json rj;
      rj["uri"] = r.uri;
      rj["name"] = r.name;
      rj["description"] = r.description;
      rj["mimeType"] = r.mimeType;
      resourcesJson.push_back(std::move(rj));
    }
    result["resources"] = std::move(resourcesJson);
    return jsonRpcResponse(id, std::move(result));
  }

  json handleResourcesRead(const json &id, const json &params) {
    std::string uri = params.value("uri", "");
    if (uri.empty()) {
      return jsonRpcErrorResponse(
          id, jsonRpcError(kJsonRpcInvalidParams, "Missing resource URI"));
    }

    ResourceReader reader;
    {
      std::shared_lock lock(resourcesMutex_);
      auto it = resourcesByUri_.find(uri);
      if (it == resourcesByUri_.end()) {
        return jsonRpcErrorResponse(
            id, jsonRpcError(kMcpResourceNotFound,
                             std::string("Resource not found: ") + uri));
      }
      reader = it->second.reader;
    }

    auto content = reader(uri);
    if (!content.has_value()) {
      return jsonRpcErrorResponse(
          id, jsonRpcError(kMcpResourceNotFound,
                           std::string("Failed to read resource: ") + uri));
    }

    json result;
    json contents = json::array();
    json cj;
    cj["uri"] = content->uri;
    cj["mimeType"] = content->mimeType;
    cj["text"] = content->text;
    contents.push_back(std::move(cj));
    result["contents"] = std::move(contents);
    return jsonRpcResponse(id, std::move(result));
  }

  json handleResourcesSubscribe(const json &id, const json &params) {
    std::string uri = params.value("uri", "");
    if (uri.empty()) {
      return jsonRpcErrorResponse(
          id, jsonRpcError(kJsonRpcInvalidParams, "Missing resource URI"));
    }
    {
      std::unique_lock lock(subscribedResourcesMutex_);
      subscribedResources_.insert(uri);
    }
    return jsonRpcResponse(id, json::object());
  }

  json handleResourcesUnsubscribe(const json &id, const json &params) {
    std::string uri = params.value("uri", "");
    {
      std::unique_lock lock(subscribedResourcesMutex_);
      subscribedResources_.erase(uri);
    }
    return jsonRpcResponse(id, json::object());
  }

  json handlePromptsList(const json &id, const json &) {
    auto prompts = listPrompts();
    json result;
    json promptsJson = json::array();
    for (const auto &p : prompts) {
      json pj;
      pj["name"] = p.name;
      pj["description"] = p.description;
      json args = json::array();
      for (const auto &a : p.arguments) {
        json aj;
        aj["name"] = a.name;
        aj["description"] = a.description;
        aj["required"] = a.required;
        args.push_back(std::move(aj));
      }
      pj["arguments"] = std::move(args);
      promptsJson.push_back(std::move(pj));
    }
    result["prompts"] = std::move(promptsJson);
    return jsonRpcResponse(id, std::move(result));
  }

  json handlePromptsGet(const json &id, const json &params) {
    std::string name = params.value("name", "");
    json arguments = params.contains("arguments") ? params["arguments"] : json::object();

    if (name.empty()) {
      return jsonRpcErrorResponse(
          id, jsonRpcError(kJsonRpcInvalidParams, "Missing prompt name"));
    }

    PromptHandler handler;
    {
      std::shared_lock lock(promptsMutex_);
      auto it = promptsByName_.find(name);
      if (it == promptsByName_.end()) {
        return jsonRpcErrorResponse(
            id, jsonRpcError(kMcpPromptNotFound,
                             std::string("Prompt not found: ") + name));
      }
      handler = it->second.handler;
    }

    auto result = handler(name, arguments);
    if (!result.has_value()) {
      return jsonRpcErrorResponse(
          id, jsonRpcError(kMcpPromptNotFound,
                           std::string("Failed to get prompt: ") + name));
    }

    json rj;
    rj["description"] = result->description;
    json messages = json::array();
    for (const auto &m : result->messages) {
      json mj;
      mj["role"] = m.role;
      mj["content"] = m.content;
      messages.push_back(std::move(mj));
    }
    rj["messages"] = std::move(messages);
    return jsonRpcResponse(id, std::move(rj));
  }

  json handleLoggingSetLevel(const json &id, const json &params) {
    // Logging is a no-op in baseline but accepts the request
    return jsonRpcResponse(id, json::object());
  }

  json handleResourceTemplatesList(const json &id, const json &) {
    // 2025-03-26+: return empty list of resource templates for now
    json result;
    result["resourceTemplates"] = json::array();
    return jsonRpcResponse(id, std::move(result));
  }

  json handleComplete(const json &id, const json &) {
    // 2025-03-26+: return empty completion
    json result;
    result["completion"] = json::object();
    result["completion"]["values"] = json::array();
    result["completion"]["hasMore"] = false;
    result["completion"]["total"] = 0;
    return jsonRpcResponse(id, std::move(result));
  }

  void handleInitialized(const json &) {
    XX_LOGI("[mcp] Client initialized");
  }

  // -----------------------------------------------------------------------
  // SSE
  // -----------------------------------------------------------------------

  asio::awaitable<void>
  handleSseRequest(util::HttpServer::Request &req,
                   util::HttpServer::Response &resp) {
    resp.version(req.version());
    resp.result(boost::beast::http::status::ok);
    resp.set(boost::beast::http::field::content_type, "text/event-stream");
    resp.set(boost::beast::http::field::cache_control, "no-cache");
    resp.set(boost::beast::http::field::connection, "keep-alive");
    resp.set("X-Accel-Buffering", "no");

    std::string sseData = "event: endpoint\ndata: " + config_.mcpEndpoint + "\n\n";
    resp.body() = sseData;
    resp.prepare_payload();
    co_return;
  }

  // -----------------------------------------------------------------------
  // SSE notification broadcast
  // -----------------------------------------------------------------------

  void broadcastSSE(const std::string &event, const std::string &data) {
    // SSE broadcast is limited in the current HttpServer model.
    // Log the event for now.
    XX_LOGI("[mcp] SSE event: {} {}", event, data);
  }

  void stopSSE() {
    std::unique_lock lock(sseClientsMutex_);
    for (auto &client : sseClients_) {
      client->closed = true;
    }
    sseClients_.clear();
  }

  // -----------------------------------------------------------------------
  // Notification events
  // -----------------------------------------------------------------------

  void notifyToolsChanged() {
    // In a full implementation, we'd push SSE events to connected clients
    // Here we just set the flag
  }

  void notifyResourcesChanged() {
    // Same as above
  }

  void notifyPromptsChanged() {
    // Same as above
  }

  // -----------------------------------------------------------------------
  // Response helper
  // -----------------------------------------------------------------------

  void writeJsonResponse(util::HttpServer::Response &resp,
                         boost::beast::http::status status, const json &body) {
    resp.result(status);
    resp.set(boost::beast::http::field::content_type, "application/json");
    resp.body() = body.dump();
    resp.prepare_payload();
  }

  // -----------------------------------------------------------------------
  // Members
  // -----------------------------------------------------------------------

  Config config_;
  std::unique_ptr<util::HttpServer> httpServer_;
  Capabilities capabilities_;

  mutable std::shared_mutex toolsMutex_;
  std::unordered_map<std::string, ToolEntry> toolsByName_;
  std::atomic<bool> toolsListChanged_ = false;

  mutable std::shared_mutex resourcesMutex_;
  std::unordered_map<std::string, ResourceEntry> resourcesByUri_;
  std::atomic<bool> resourcesListChanged_ = false;

  mutable std::shared_mutex promptsMutex_;
  std::unordered_map<std::string, PromptEntry> promptsByName_;
  std::atomic<bool> promptsListChanged_ = false;

  std::mutex subscribedResourcesMutex_;
  std::unordered_set<std::string> subscribedResources_;

  std::mutex sseClientsMutex_;
  std::vector<std::shared_ptr<SSEClient> > sseClients_;
};

} // namespace server
} // namespace agentxx
