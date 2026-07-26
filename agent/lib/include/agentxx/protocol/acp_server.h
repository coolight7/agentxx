#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include <neograph/json.h>

#include "agentxx/agent/deepagent.h"
#include "agentxx/util/http_server.h"

namespace agentxx {
namespace server {

using json = neograph::json;

// ---------------------------------------------------------------------------
// Internal ACP protocol handler (shared by HTTP and stdio transports)
//
// Implements JSON-RPC 2.0 for the Agent Client Protocol:
//   - initialize        – handshake, returns capabilities + agent info
//   - session/new       – create a conversation session
//   - session/prompt    – submit a prompt (async, result via notification)
//   - session/cancel    – cancel an in-flight prompt
// ---------------------------------------------------------------------------

class AcpProtocolHandler {
public:

    struct Config {
        std::string serverName         = "agentxx-acp";
        std::string serverVersion      = "0.1.0";
        int         maxInflightPrompts = 32;
    };

    using NotificationSink = std::function<void(const json&)>;

    AcpProtocolHandler(
        std::shared_ptr<agentxx::agent::DeepAgent> agent,
        json                                       agentInfo,
        Config                                     config
    );

    AcpProtocolHandler(const AcpProtocolHandler&)            = delete;
    AcpProtocolHandler& operator=(const AcpProtocolHandler&) = delete;

    ~AcpProtocolHandler();

    bool initialized() const;

    const json& agentInfo() const;

    /// Set the notification sink used to deliver async responses and
    /// streaming updates. Must be set before processing messages.
    void setNotificationSink(NotificationSink sink);

    /// Stop all in-flight prompts, cancel pending requests.
    /// Check whether stop has been requested.
    bool stopRequested() const;

    void stop();

    /// Process one JSON-RPC envelope. Returns the response envelope for
    /// synchronous methods, or null json for notifications / async dispatch.
    /// Async responses are delivered via the notification sink.
    json handleMessage(const json& env);

    /// Emit an outbound request (agent→client) and wait for the response.
    /// The notification sink must be set before calling this.
    json callClient(
        std::string_view          method,
        json                      params,
        std::chrono::milliseconds timeout = std::chrono::seconds{30}
    );

    // -- Session queries (for tests / introspection) -----------------------

    bool        hasSession(std::string_view sessionId) const;
    std::string sessionCwd(std::string_view sessionId) const;
    bool        isInFlight(std::string_view sessionId) const;
    int         inflightCount() const;

    /// Drain in-flight workers (used by transports before shutdown).
    void drainWorkers();

    // -----------------------------------------------------------------------
    // JSON-RPC helpers
    // -----------------------------------------------------------------------

    static json        jsonRpcResult(const json& id, json result);
    static json        jsonRpcError(const json& id, int code, std::string_view msg);
    static json        makeParseError(std::string_view detail);
    static json        makeInvalidRequest();
    static std::string extractUserText(const json& prompt);
    static std::string generateSessionId();

    // -----------------------------------------------------------------------
    // Method handlers
    // -----------------------------------------------------------------------

    json handleInitialize(const json& params, const json& id);
    json handleSessionNew(const json& params, const json& id);
    void handleSessionPrompt(const json& env, const json& params, const json& id);
    void workerRunPrompt(
        std::string_view                   sessionId,
        const json&                        promptBlocks,
        const json&                        id,
        std::shared_ptr<std::atomic<bool>> cancelFlag
    );
    void workerCleanup(std::string_view sessionId);
    void handleSessionCancel(const json& params);

    // -----------------------------------------------------------------------
    // Notification / streaming helpers
    // -----------------------------------------------------------------------

    void emit(const json& env);
    void emitNotification(std::string_view method, const json& params);
    void emitAgentMessageChunk(std::string_view sessionId, std::string_view text);

private:

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    Config                                     config_;
    std::shared_ptr<agentxx::agent::DeepAgent> deepAgent_;
    json                                       agentInfo_;
    std::atomic<bool>                          initialized_{false};
    std::atomic<bool>                          stopFlag_{false};
    NotificationSink                           sink_;

    // -- Sessions --
    mutable std::mutex                                        sessionsMu_;
    std::map<std::string, std::string, std::less<>>           sessions_; // sessionId → cwd
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> cancelFlags_;

    // -- In-flight prompt tracking --
    mutable std::mutex                 inflightMu_;
    std::set<std::string, std::less<>> inflightSessions_;
    std::atomic<int>                   inflightCount_{0};

    std::mutex              workersMu_;
    std::condition_variable workersCv_;

    // -- Outbound request tracking (agent→client) --
    mutable std::mutex                                               pendingMu_;
    std::map<int64_t, std::shared_ptr<std::promise<neograph::json>>> pending_;
    std::atomic<int64_t>                                             nextOutboundId_{1};
};

// ===========================================================================
// HTTP ACP Server
//
// Wraps AcpProtocolHandler behind an HTTP transport.
//   POST /acp    – main JSON-RPC endpoint
//   GET /acp/sse – SSE endpoint for streaming notifications
// ===========================================================================

class HttpAcpServer {
public:

    struct Config {
        util::HttpServer::Config httpConfig;
        std::string              acpEndpoint   = "/acp";
        std::string              sseEndpoint   = "/acp/sse";
        std::string              serverName    = "agentxx-acp";
        std::string              serverVersion = "0.1.0";
        std::chrono::seconds     asyncTimeout{120};
    };

    HttpAcpServer(
        std::shared_ptr<agentxx::agent::DeepAgent> agent,
        neograph::json                             agentInfo,
        Config                                     config
    );

    HttpAcpServer(const HttpAcpServer&)            = delete;
    HttpAcpServer& operator=(const HttpAcpServer&) = delete;

    ~HttpAcpServer();

    void start();
    void stop();

    uint16_t port() const;
    bool     isStopped() const;

    AcpProtocolHandler& handler();

private:

    // -----------------------------------------------------------------------
    // Wire up the handler's notification sink → SSE + pending resolver
    // -----------------------------------------------------------------------

    void setupHandlerSink();

    // -----------------------------------------------------------------------
    // Route setup
    // -----------------------------------------------------------------------

    void setupRoutes();

    // -----------------------------------------------------------------------
    // ACP request handler (HTTP JSON-RPC)
    // -----------------------------------------------------------------------

    asio::awaitable<void>
        handleAcpRequest(util::HttpServer::Request& req, util::HttpServer::Response& resp);

    // -----------------------------------------------------------------------
    // SSE endpoint
    // -----------------------------------------------------------------------

    asio::awaitable<void>
        handleSseRequest(util::HttpServer::Request& req, util::HttpServer::Response& resp);

    void broadcastSSE(std::string_view /*data*/);
    void stopSSE();

    // -----------------------------------------------------------------------
    // HTTP response helpers
    // -----------------------------------------------------------------------

    void writeJsonResponse(
        util::HttpServer::Response& resp,
        boost::beast::http::status  status,
        const neograph::json&       body
    );

    neograph::json jsonRpcError(const neograph::json& id, int code, std::string_view message) const;

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    Config                                     config_;
    std::shared_ptr<agentxx::agent::DeepAgent> deepAgent_;
    AcpProtocolHandler                         handler_;
    std::unique_ptr<util::HttpServer>          httpServer_;

    // Pending async response tracking (for HTTP transport)
    std::mutex                                                       pendingMutex_;
    std::map<int64_t, std::shared_ptr<std::promise<neograph::json>>> pendingResponses_;
};

// ===========================================================================
// Stdio ACP Server
//
// Reads newline-delimited JSON-RPC from an input stream and writes
// responses to an output stream (default: std::cin / std::cout).
// ===========================================================================

class StdioAcpServer {
public:

    StdioAcpServer(std::shared_ptr<agentxx::agent::DeepAgent> agent, neograph::json agentInfo);

    StdioAcpServer(const StdioAcpServer&)            = delete;
    StdioAcpServer& operator=(const StdioAcpServer&) = delete;

    ~StdioAcpServer();

    void run();
    void run(std::istream& in, std::ostream& out);

    void stop();

    bool isRunning() const;

    AcpProtocolHandler& handler();

private:

    std::shared_ptr<agentxx::agent::DeepAgent> deepAgent_;
    AcpProtocolHandler                         handler_;
    std::atomic<bool>                          running_{false};
};

} // namespace server
} // namespace agentxx
