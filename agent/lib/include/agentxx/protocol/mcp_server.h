#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <asio/awaitable.hpp>
#include <neograph/json.h>

#include "agentxx/util/http_server.h"

namespace agentxx {
namespace server {

using json = neograph::json;

// ---------------------------------------------------------------------------
// JSON-RPC helpers
// ---------------------------------------------------------------------------

inline json jsonRpcError(int code, std::string_view message, std::optional<json> data = {}) {
    json err;
    err["code"]    = code;
    err["message"] = std::string(message);
    if (data.has_value()) {
        err["data"] = std::move(*data);
    }
    return err;
}

inline json jsonRpcResponse(json id, json result) {
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = std::move(id);
    resp["result"]  = std::move(result);
    return resp;
}

inline json jsonRpcErrorResponse(json id, json error) {
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = std::move(id);
    resp["error"]   = std::move(error);
    return resp;
}

// Standard JSON-RPC error codes
inline constexpr int kJsonRpcParseError     = -32700;
inline constexpr int kJsonRpcInvalidRequest = -32600;
inline constexpr int kJsonRpcMethodNotFound = -32601;
inline constexpr int kJsonRpcInvalidParams  = -32602;
inline constexpr int kJsonRpcInternalError  = -32603;

// MCP-specific error codes
inline constexpr int kMcpToolNotFound       = -32000;
inline constexpr int kMcpToolExecutionError = -32001;
inline constexpr int kMcpResourceNotFound   = -32002;
inline constexpr int kMcpPromptNotFound     = -32003;

// ---------------------------------------------------------------------------
// Data types
// ---------------------------------------------------------------------------

struct McpToolDefinition {
    std::string name;
    std::string description;
    std::string title; // 2025-11-25: display name
    json        inputSchema  = json::object();
    json        outputSchema = json::object(); // 2025-11-25: structured output schema
    json        annotations  = json::object(); // 2025-11-25: tool behavior metadata
    json        execution    = json::object(); // 2025-11-25: execution config
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
    bool        required = false;
};

struct McpPromptDefinition {
    std::string                    name;
    std::string                    description;
    std::vector<McpPromptArgument> arguments;
};

struct McpPromptMessage {
    std::string role; // "user" | "assistant"
    json        content;
};

struct McpPromptResult {
    std::string                   description;
    std::vector<McpPromptMessage> messages;
};

// Supported MCP protocol versions (newest first for negotiation)
inline constexpr std::string_view kMcpProtocol2024_11_05   = "2024-11-05";
inline constexpr std::string_view kMcpProtocol2025_03_26   = "2025-03-26";
inline constexpr std::string_view kMcpProtocol2025_06_18   = "2025-06-18";
inline constexpr std::string_view kMcpProtocol2025_11_25   = "2025-11-25";
inline constexpr std::string_view kMcpSupportedProtocols[] = {
    kMcpProtocol2025_11_25,
    kMcpProtocol2025_06_18,
    kMcpProtocol2025_03_26,
    kMcpProtocol2024_11_05,
};

// ---------------------------------------------------------------------------
// McpServer
// ---------------------------------------------------------------------------

class McpServer {
public:

    using ToolHandler    = std::function<json(const json& arguments)>;
    using ResourceReader = std::function<std::optional<McpResourceContent>(const std::string& uri)>;
    using PromptHandler  = std::function<
         std::optional<McpPromptResult>(const std::string& name, const json& arguments)>;

    struct Config {
        util::HttpServer::Config httpConfig;
        std::string              mcpEndpoint   = "/mcp";
        std::string              sseEndpoint   = "/mcp/sse";
        std::string              serverName    = "agentxx-mcp";
        std::string              serverVersion = "0.1.0";
        std::chrono::seconds     toolTimeout{60};
        size_t                   maxMessageSize = 4 * 1024 * 1024; // 4 MB
    };

    explicit McpServer();
    explicit McpServer(Config config);

    McpServer(const McpServer&)            = delete;
    McpServer& operator=(const McpServer&) = delete;

    ~McpServer();

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    void start();
    void stop();

    /// Run the server over stdin/stdout using newline-delimited JSON.
    /// Blocks until stdin is closed (EOF / Ctrl-D).
    void runStdio();

    uint16_t port() const;
    size_t   activeConnections() const;
    bool     isStopped() const;

    // -----------------------------------------------------------------------
    // Tool registration
    // -----------------------------------------------------------------------

    void                           addTool(McpToolDefinition def, ToolHandler handler);
    void                           removeTool(const std::string& name);
    std::vector<McpToolDefinition> listTools() const;

    void addResource(McpResourceDefinition def, ResourceReader reader);
    void removeResource(const std::string& uri);
    std::vector<McpResourceDefinition> listResources() const;

    // -----------------------------------------------------------------------
    // Prompt registration
    // -----------------------------------------------------------------------

    void                             addPrompt(McpPromptDefinition def, PromptHandler handler);
    void                             removePrompt(const std::string& name);
    std::vector<McpPromptDefinition> listPrompts() const;

    // -----------------------------------------------------------------------
    // Capabilities
    // -----------------------------------------------------------------------

    struct Capabilities {
        bool tools     = true;
        bool resources = false;
        bool prompts   = false;
        bool logging   = false;
    };

    void                setCapabilities(Capabilities caps);
    const Capabilities& capabilities() const;

private:

    struct ToolEntry {
        McpToolDefinition def;
        ToolHandler       handler;
    };

    struct ResourceEntry {
        McpResourceDefinition def;
        ResourceReader        reader;
    };

    struct PromptEntry {
        McpPromptDefinition def;
        PromptHandler       handler;
    };

    struct SSEClient {
        std::shared_ptr<util::HttpServer::SseWriter> writer;
        bool                                         closed = false;
    };

    // -----------------------------------------------------------------------
    // Accept header validation helpers
    // -----------------------------------------------------------------------

    static bool isAcceptValid(std::string_view accept);
    static bool prefersSse(std::string_view accept);

    // -----------------------------------------------------------------------
    // Route setup
    // -----------------------------------------------------------------------

    void setupRoutes();

    // -----------------------------------------------------------------------
    // JSON-RPC request processing (transport-agnostic)
    // -----------------------------------------------------------------------

    json processJsonRpc(const json& requestJson);

    // -----------------------------------------------------------------------
    // Main MCP request handler (HTTP)
    // -----------------------------------------------------------------------

    asio::awaitable<void>
        handleMcpRequest(util::HttpServer::Request& req, util::HttpServer::Response& resp);

    // -----------------------------------------------------------------------
    // Method handlers
    // -----------------------------------------------------------------------

    json handleInitialize(const json& id, const json& params);
    json handlePing(const json& id);
    json handleToolsList(const json& id, const json&);
    json handleToolsCall(const json& id, const json& params);
    json handleResourcesList(const json& id, const json&);
    json handleResourcesRead(const json& id, const json& params);
    json handleResourcesSubscribe(const json& id, const json& params);
    json handleResourcesUnsubscribe(const json& id, const json& params);
    json handlePromptsList(const json& id, const json&);
    json handlePromptsGet(const json& id, const json& params);
    json handleLoggingSetLevel(const json& id, const json& params);
    json handleResourceTemplatesList(const json& id, const json&);
    json handleComplete(const json& id, const json&);
    void handleInitialized(const json&);

    // -----------------------------------------------------------------------
    // SSE streaming handler
    // -----------------------------------------------------------------------

    asio::awaitable<void> handleSseStream(
        util::HttpServer::Request&                   req,
        std::shared_ptr<util::HttpServer::SseWriter> writer
    );

    // -----------------------------------------------------------------------
    // SSE notification broadcast
    // -----------------------------------------------------------------------

    void broadcastSSE(const std::string& event, const std::string& data);
    void stopSSE();

    // -----------------------------------------------------------------------
    // Notification events
    // -----------------------------------------------------------------------

    void notifyToolsChanged();
    void notifyResourcesChanged();
    void notifyPromptsChanged();

    // -----------------------------------------------------------------------
    // Response helper
    // -----------------------------------------------------------------------

    void writeJsonResponse(
        util::HttpServer::Response& resp,
        boost::beast::http::status  status,
        const json&                 body
    );

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    Config                            config_;
    std::unique_ptr<util::HttpServer> httpServer_;
    Capabilities                      capabilities_;

    mutable std::shared_mutex                  toolsMutex_;
    std::unordered_map<std::string, ToolEntry> toolsByName_;
    std::atomic<bool>                          toolsListChanged_ = false;

    mutable std::shared_mutex                      resourcesMutex_;
    std::unordered_map<std::string, ResourceEntry> resourcesByUri_;
    std::atomic<bool>                              resourcesListChanged_ = false;

    mutable std::shared_mutex                    promptsMutex_;
    std::unordered_map<std::string, PromptEntry> promptsByName_;
    std::atomic<bool>                            promptsListChanged_ = false;

    std::mutex                      subscribedResourcesMutex_;
    std::unordered_set<std::string> subscribedResources_;

    std::mutex                              sseClientsMutex_;
    std::vector<std::shared_ptr<SSEClient>> sseClients_;
};

} // namespace server
} // namespace agentxx
