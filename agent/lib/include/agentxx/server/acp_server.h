#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <neograph/acp/server.h>
#include <neograph/json.h>

#include "agentxx/util/http_server.h"
#include "agentxx/util/log.h"

namespace agentxx {
namespace server {

using json = neograph::json;

// ---------------------------------------------------------------------------
// HTTP ACP Server
//
// Wraps neograph::acp::ACPServer behind an HTTP transport.
//   POST /acp  – main JSON-RPC endpoint
//   GET /acp/sse – SSE endpoint for streaming notifications
// ---------------------------------------------------------------------------

class HttpAcpServer {
public:
  struct Config {
    util::HttpServer::Config httpConfig;
    std::string acpEndpoint = "/acp";
    std::string sseEndpoint = "/acp/sse";
    std::string serverName = "agentxx-acp";
    std::string serverVersion = "0.1.0";
    std::chrono::seconds asyncTimeout{120};
  };

  HttpAcpServer(std::shared_ptr<neograph::graph::GraphEngine> engine,
                neograph::json info,
                Config config)
      : config_(std::move(config)),
        neographServer_(std::move(engine), std::move(info)) {
    httpServer_ = std::make_unique<util::HttpServer>(config_.httpConfig);
    setupNotificationSink();
    setupRoutes();
  }

  HttpAcpServer(const HttpAcpServer &) = delete;
  HttpAcpServer &operator=(const HttpAcpServer &) = delete;

  ~HttpAcpServer() { stop(); }

  // -----------------------------------------------------------------------
  // Lifecycle
  // -----------------------------------------------------------------------

  void start() {
    httpServer_->start();
  }

  void stop() {
    neographServer_.stop();
    stopSSE();
    httpServer_->stop();
  }

  uint16_t port() const { return httpServer_->port(); }
  bool isStopped() const { return httpServer_->isStopped(); }

  neograph::acp::ACPServer &neographServer() { return neographServer_; }
  neograph::acp::AgentCapabilities &capabilities() {
    return neographServer_.capabilities();
  }

private:
  // -----------------------------------------------------------------------
  // Notification sink → SSE + pending response resolver
  // -----------------------------------------------------------------------



  void setupNotificationSink() {
    neographServer_.set_notification_sink(
        [this](const json &envelope) {
          bool isResponse = !envelope.contains("method") &&
                            envelope.contains("id") && !envelope["id"].is_null();

          if (isResponse) {
            json id = envelope["id"];
            int64_t idVal = id.is_number_integer() ? id.get<int64_t>() : -1;

            std::unique_lock lock(pendingMutex_);
            auto it = pendingResponses_.find(idVal);
              if (it != pendingResponses_.end()) {
                it->second->set_value(envelope);
                pendingResponses_.erase(it);
              }
          }

          // Broadcast to SSE clients
          std::string sseData;
          if (isResponse) {
            sseData = "data: " + envelope.dump() + "\n\n";
          } else {
            std::string method = envelope.value("method", "");
            std::string eventType = method;
            if (!eventType.empty()) {
              // Replace / with _ for SSE event name
              for (auto &c : eventType)
                if (c == '/') c = '_';
              sseData = "event: " + eventType + "\ndata: " +
                        envelope.dump() + "\n\n";
            } else {
              sseData = "data: " + envelope.dump() + "\n\n";
            }
          }

          broadcastSSE(sseData);
        });
  }

  // -----------------------------------------------------------------------
  // Route setup
  // -----------------------------------------------------------------------

  void setupRoutes() {
    using Handler = util::HttpServer::Handler;

    auto acpHandler = std::make_shared<Handler>(
        Handler([this](util::HttpServer::Request &req,
                       util::HttpServer::Response &resp,
                       const std::string &) -> asio::awaitable<void> {
          co_await handleAcpRequest(req, resp);
        }));
    httpServer_->router().add(config_.acpEndpoint, 2, acpHandler);

    auto sseHandler = std::make_shared<Handler>(
        Handler([this](util::HttpServer::Request &req,
                       util::HttpServer::Response &resp,
                       const std::string &) -> asio::awaitable<void> {
          co_await handleSseRequest(req, resp);
        }));
    httpServer_->router().add(config_.sseEndpoint, 0, sseHandler);
  }

  // -----------------------------------------------------------------------
  // ACP request handler
  // -----------------------------------------------------------------------

  asio::awaitable<void>
  handleAcpRequest(util::HttpServer::Request &req,
                   util::HttpServer::Response &resp) {
    namespace http = boost::beast::http;

    json requestJson;
    try {
      requestJson = json::parse(req.body());
    } catch (const json::parse_error &e) {
      writeJsonResponse(resp, http::status::bad_request,
                        makeParseErrorResponse(e.what()));
      co_return;
    }

    if (!requestJson.is_object() ||
        requestJson.value("jsonrpc", "") != "2.0") {
      writeJsonResponse(resp, http::status::bad_request,
                        makeInvalidRequestResponse());
      co_return;
    }

    json id = requestJson.contains("id") ? requestJson["id"] : json{};

    // Call handle_message (synchronous – returns immediately for dispatch,
    // even for session/prompt which runs on a worker thread)
    json response;
    try {
      response = neographServer_.handle_message(requestJson);
    } catch (const std::exception &e) {
      XX_LOGE("[acp] handle_message error: {}", e.what());
      writeJsonResponse(resp, http::status::internal_server_error,
                        makeInternalErrorResponse(id, e.what()));
      co_return;
    }

    // If the response is non-null, it's a synchronous result
    if (!response.is_null()) {
      writeJsonResponse(resp, http::status::ok, response);
      co_return;
    }

    // Async response: wait for the notification sink to deliver it
    if (id.is_null()) {
      // Notification with no response expected
      writeJsonResponse(resp, http::status::accepted,
                        json{{"jsonrpc", "2.0"}, {"id", json{nullptr}},
                             {"result", json::object()}});
      co_return;
    }

    int64_t idVal = id.is_number_integer() ? id.get<int64_t>() : -1;

    if (idVal < 0 && id.is_string()) {
      // Use hash of string id for map key
      std::string idStr = id.get<std::string>();
      idVal = static_cast<int64_t>(std::hash<std::string>{}(idStr));
    }

    auto promise = std::make_shared<std::promise<json>>();
    auto future = promise->get_future().share();

    {
      std::unique_lock lock(pendingMutex_);
      auto [pendingIt, inserted] = pendingResponses_.try_emplace(
          idVal, std::move(promise));
      if (!inserted) {
        writeJsonResponse(resp, http::status::internal_server_error,
                          makeInternalErrorResponse(id, "Duplicate request ID"));
        co_return;
      }
    }

    // Wait for the async response with timeout
    auto status = future.wait_for(config_.asyncTimeout);
    if (status == std::future_status::timeout) {
      std::unique_lock lock(pendingMutex_);
      pendingResponses_.erase(idVal);

      writeJsonResponse(resp, http::status::gateway_timeout,
                        jsonRpcErrorResponse(
                            id, -32000, "Async request timed out"));
      co_return;
    }

    json asyncResponse = future.get();

    {
      std::unique_lock lock(pendingMutex_);
      pendingResponses_.erase(idVal);
    }

    writeJsonResponse(resp, http::status::ok, asyncResponse);
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

    resp.body() = "event: endpoint\ndata: " + config_.acpEndpoint + "\n\n";
    resp.prepare_payload();
    co_return;
  }

  void broadcastSSE(const std::string &data) {
    // SSE broadcast through the HttpServer model is limited.
    // In production, this would be delivered via persistent connections.
    // Logged for observability.
    if (config_.httpConfig.accessLogEnabled) {
      XX_LOGI("[acp] SSE: {}", data);
    }
  }

  void stopSSE() {}
  // -----------------------------------------------------------------------
  // Response helpers
  // -----------------------------------------------------------------------

  void writeJsonResponse(util::HttpServer::Response &resp,
                         boost::beast::http::status status,
                         const json &body) {
    resp.result(status);
    resp.set(boost::beast::http::field::content_type, "application/json");
    resp.body() = body.dump();
    resp.prepare_payload();
  }

  json jsonRpcErrorResponse(const json &id, int code,
                            const std::string &message) {
    json err;
    err["jsonrpc"] = "2.0";
    err["id"] = id;
    err["error"] = {{"code", code}, {"message", message}};
    return err;
  }

  json makeParseErrorResponse(const std::string &detail) {
    return jsonRpcErrorResponse(json{nullptr}, -32700,
                                "Parse error: " + detail);
  }

  json makeInvalidRequestResponse() {
    return jsonRpcErrorResponse(json{nullptr}, -32600,
                                "Invalid Request");
  }

  json makeInternalErrorResponse(const json &id,
                                 const std::string &detail) {
    return jsonRpcErrorResponse(id, -32603,
                                "Internal error: " + detail);
  }

  // -----------------------------------------------------------------------
  // Members
  // -----------------------------------------------------------------------

  Config config_;
  std::unique_ptr<util::HttpServer> httpServer_;
  neograph::acp::ACPServer neographServer_;

  std::mutex pendingMutex_;
  std::map<int64_t, std::shared_ptr<std::promise<json>>> pendingResponses_;
};

// ---------------------------------------------------------------------------
// Stdio ACP Server
//
// Thin wrapper around neograph::acp::ACPServer that reads newline-delimited
// JSON-RPC from stdin and writes responses to stdout.
// ---------------------------------------------------------------------------

class StdioAcpServer {
public:
  StdioAcpServer(std::shared_ptr<neograph::graph::GraphEngine> engine,
                 neograph::json info)
      : neographServer_(std::move(engine), std::move(info)) {}

  StdioAcpServer(const StdioAcpServer &) = delete;
  StdioAcpServer &operator=(const StdioAcpServer &) = delete;

  ~StdioAcpServer() { stop(); }

  void run() { neographServer_.run(); }
  void run(std::istream &in, std::ostream &out) {
    neographServer_.run(in, out);
  }
  void stop() { neographServer_.stop(); }

  neograph::acp::ACPServer &neographServer() { return neographServer_; }
  neograph::acp::AgentCapabilities &capabilities() {
    return neographServer_.capabilities();
  }

private:
  neograph::acp::ACPServer neographServer_;
};

} // namespace server
} // namespace agentxx
