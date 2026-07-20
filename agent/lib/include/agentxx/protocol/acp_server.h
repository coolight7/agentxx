#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <neograph/json.h>

#include "agentxx/agent/deepagent.h"
#include "agentxx/util/http_server.h"
#include "agentxx/util/log.h"

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
    std::string serverName = "agentxx-acp";
    std::string serverVersion = "0.1.0";
    int maxInflightPrompts = 32;
  };

  using NotificationSink = std::function<void(const json &)>;

  AcpProtocolHandler(std::shared_ptr<agentxx::agent::DeepAgent> agent,
                     json agentInfo, Config config)
      : config_(std::move(config)), deepAgent_(std::move(agent)),
        agentInfo_(std::move(agentInfo)) {}

  AcpProtocolHandler(const AcpProtocolHandler &) = delete;
  AcpProtocolHandler &operator=(const AcpProtocolHandler &) = delete;

  ~AcpProtocolHandler() { stop(); }

  bool initialized() const {
    return initialized_.load(std::memory_order_acquire);
  }

  const json &agentInfo() const { return agentInfo_; }

  /// Set the notification sink used to deliver async responses and
  /// streaming updates. Must be set before processing messages.
  void setNotificationSink(NotificationSink sink) { sink_ = std::move(sink); }

  /// Stop all in-flight prompts, cancel pending requests.
  /// Check whether stop has been requested.
  bool stopRequested() const {
    return stopFlag_.load(std::memory_order_acquire);
  }

  void stop() {
    stopFlag_.store(true, std::memory_order_release);
    {
      std::lock_guard lk(sessionsMu_);
      for (auto &[sid, flag] : cancelFlags_) {
        if (flag)
          flag->store(true, std::memory_order_release);
      }
    }
    workersCv_.notify_all();
  }

  /// Process one JSON-RPC envelope. Returns the response envelope for
  /// synchronous methods, or null json for notifications / async dispatch.
  /// Async responses are delivered via the notification sink.
  json handleMessage(const json &env) {
    bool hasMethod = env.contains("method") && !env["method"].is_null();

    // Response to one of our outbound requests — fulfil the promise
    if (!hasMethod) {
      if (env.contains("id")) {
        auto idV = env["id"];
        int64_t id = idV.is_number_integer() ? idV.get<int64_t>() : -1;
        std::shared_ptr<std::promise<json>> p;
        {
          std::lock_guard lk(pendingMu_);
          auto it = pending_.find(id);
          if (it != pending_.end()) {
            p = it->second;
            pending_.erase(it);
          }
        }
        if (p)
          p->set_value(env);
      }
      return {};
    }

    auto method = env.value("method", std::string());
    auto params = env.contains("params") ? env["params"] : json::object();
    auto id = env.contains("id") ? env["id"] : json();
    bool isNotification = !env.contains("id");

    try {
      if (method == "initialize")
        return handleInitialize(params, id);
      if (method == "session/new")
        return handleSessionNew(params, id);
      if (method == "session/prompt") {
        handleSessionPrompt(env, params, id);
        return {}; // async – response via sink
      }
      if (method == "session/cancel") {
        handleSessionCancel(params);
        return {};
      }
    } catch (const std::exception &e) {
      XX_LOGE("[acp] error handling '{}': {}", method, e.what());
      if (!isNotification)
        return jsonRpcError(id, -32602,
                            "Invalid params: " + std::string(e.what()));
      return {};
    }

    if (isNotification)
      return {};
    return jsonRpcError(id, -32601, "Method not found: " + method);
  }

  /// Emit an outbound request (agent→client) and wait for the response.
  /// The notification sink must be set before calling this.
  json callClient(const std::string &method, json params,
                  std::chrono::milliseconds timeout = std::chrono::seconds{
                      30}) {
    if (!sink_) {
      throw std::runtime_error(
          "AcpProtocolHandler::callClient: no notification sink set");
    }

    auto id = nextOutboundId_.fetch_add(1, std::memory_order_relaxed);
    json env;
    env["jsonrpc"] = "2.0";
    env["id"] = id;
    env["method"] = method;
    env["params"] = std::move(params);

    auto promise = std::make_shared<std::promise<json>>();
    auto fut = promise->get_future();
    {
      std::lock_guard lk(pendingMu_);
      pending_[id] = promise;
    }

    sink_(env);

    auto status = fut.wait_for(timeout);
    if (status != std::future_status::ready) {
      std::lock_guard lk(pendingMu_);
      pending_.erase(id);
      throw std::runtime_error("AcpProtocolHandler::callClient: timeout for '" +
                               method + "'");
    }
    auto resp = fut.get();
    if (resp.contains("error")) {
      throw std::runtime_error("AcpProtocolHandler::callClient: error: " +
                               resp["error"].dump());
    }
    return resp.contains("result") ? resp["result"] : json::object();
  }

  // -- Session queries (for tests / introspection) -----------------------

  bool hasSession(const std::string &sessionId) const {
    std::lock_guard lk(sessionsMu_);
    return sessions_.find(sessionId) != sessions_.end();
  }

  std::string sessionCwd(const std::string &sessionId) const {
    std::lock_guard lk(sessionsMu_);
    auto it = sessions_.find(sessionId);
    return it != sessions_.end() ? it->second : std::string{};
  }

  bool isInFlight(const std::string &sessionId) const {
    std::lock_guard lk(inflightMu_);
    return inflightSessions_.count(sessionId) > 0;
  }

  int inflightCount() const {
    return inflightCount_.load(std::memory_order_acquire);
  }

  /// Drain in-flight workers (used by transports before shutdown).
  void drainWorkers() {
    std::unique_lock lk(workersMu_);
    workersCv_.wait(lk, [this] {
      return inflightCount_.load(std::memory_order_acquire) == 0;
    });
  }

  // -----------------------------------------------------------------------
  // JSON-RPC helpers
  // -----------------------------------------------------------------------

  static json jsonRpcResult(const json &id, json result) {
    json j;
    j["jsonrpc"] = "2.0";
    j["id"] = id;
    j["result"] = std::move(result);
    return j;
  }

  static json jsonRpcError(const json &id, int code, const std::string &msg) {
    json j;
    j["jsonrpc"] = "2.0";
    j["id"] = id;
    j["error"] = {{"code", code}, {"message", msg}};
    return j;
  }

  static json makeParseError(const std::string &detail) {
    return jsonRpcError(json{}, -32700, "Parse error: " + detail);
  }

  static json makeInvalidRequest() {
    return jsonRpcError(json{}, -32600, "Invalid Request");
  }

  static std::string extractUserText(const json &prompt) {
    if (!prompt.is_array())
      return {};
    std::string text;
    for (const auto &block : prompt) {
      if (block.value("type", "") == "text" && block.contains("text")) {
        if (!text.empty())
          text += ' ';
        text += block["text"].get<std::string>();
      }
    }
    return text;
  }

  static std::string generateSessionId() {
    static std::atomic<uint64_t> counter{0};
    static uint64_t seed =
        std::chrono::steady_clock::now().time_since_epoch().count();
    auto c = counter.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream oss;
    oss << "sess-" << std::hex << (seed & 0xFFFFFFFF) << "-" << std::setw(12)
        << std::setfill('0') << c;
    return oss.str();
  }

  // -----------------------------------------------------------------------
  // Method handlers
  // -----------------------------------------------------------------------

  json handleInitialize(const json &params, const json &id) {
    initialized_.store(true, std::memory_order_release);

    auto caps = json{
        {"loadSession", false},
        {"promptCapabilities",
         {{"image", false}, {"audio", false}, {"embeddedContext", false}}},
        {"mcpCapabilities", {{"http", false}, {"sse", false}}},
        {"sessionCapabilities",
         {{"close", false}, {"list", false}, {"resume", false}}},
    };

    auto result = json{
        {"protocolVersion", params.value("protocolVersion", 1)},
        {"agentCapabilities", std::move(caps)},
        {"authMethods", json::array()},
        {"agentInfo", agentInfo_},
    };
    return jsonRpcResult(id, std::move(result));
  }

  json handleSessionNew(const json &params, const json &id) {
    std::string cwd = params.value("cwd", std::string{});
    auto sessionId = generateSessionId();

    {
      std::lock_guard lk(sessionsMu_);
      sessions_[sessionId] = cwd;
      // Pre-create cancel flag for this session
      cancelFlags_[sessionId] = std::make_shared<std::atomic<bool>>(false);
    }

    XX_LOGI("[acp] session/new: {} (cwd={})", sessionId, cwd);

    auto result = json{{"sessionId", sessionId},
                       {"configOptions", json::object()},
                       {"modes", json::object()}};
    return jsonRpcResult(id, std::move(result));
  }

  void handleSessionPrompt(const json &env, const json &params,
                           const json &id) {
    std::string sessionId = params.value("sessionId", std::string{});

    // Backpressure check
    if (inflightCount_.load(std::memory_order_acquire) >=
        config_.maxInflightPrompts) {
      auto err =
          jsonRpcError(id, -32000,
                       "ACP server overloaded: " +
                           std::to_string(config_.maxInflightPrompts) +
                           " concurrent prompts in flight; retry shortly");
      emit(err);
      return;
    }

    // Ensure session exists (auto-create if missing)
    std::shared_ptr<std::atomic<bool>> cancelFlag;
    {
      std::lock_guard lk(sessionsMu_);
      if (sessions_.find(sessionId) == sessions_.end()) {
        sessions_[sessionId] = {};
      }
      auto it = cancelFlags_.find(sessionId);
      if (it == cancelFlags_.end()) {
        it = cancelFlags_
                 .emplace(sessionId, std::make_shared<std::atomic<bool>>(false))
                 .first;
      }
      cancelFlag = it->second;
    }

    // Single-flight per session
    {
      std::lock_guard lk(inflightMu_);
      if (!inflightSessions_.insert(sessionId).second) {
        auto err = jsonRpcError(id, -32000,
                                "session_id " + sessionId +
                                    " already has a prompt in flight; "
                                    "ACP requires single-flight per session");
        emit(err);
        return;
      }
    }

    inflightCount_.fetch_add(1, std::memory_order_acq_rel);

    auto promptBlocks =
        params.contains("prompt") ? params["prompt"] : json::array();

    // Spawn worker thread
    std::thread worker([this, sessionId, promptBlocks, id, cancelFlag]() {
      workerRunPrompt(sessionId, promptBlocks, id, cancelFlag);
    });
    worker.detach();
  }

  void workerRunPrompt(const std::string &sessionId, const json &promptBlocks,
                       const json &id,
                       std::shared_ptr<std::atomic<bool>> cancelFlag) {
    try {
      auto userText = extractUserText(promptBlocks);

      auto &engine = deepAgent_->engine;
      if (!engine) {
        emitAgentMessageChunk(sessionId, "(graph error: engine is null)");
        emit(jsonRpcResult(id, {{"stopReason", "end_turn"}}));
        workerCleanup(sessionId);
        return;
      }

      // Build initial state
      json state;
      state["prompt"] = userText;
      state["_acp_session_id"] = sessionId;

      neograph::graph::RunConfig cfg;
      cfg.thread_id = sessionId;
      cfg.input = std::move(state);
      cfg.stream_mode = neograph::graph::StreamMode::ALL;
      cfg.cancel_token = std::make_shared<neograph::graph::CancelToken>();

      // Run the engine synchronously on this worker thread
      auto result = engine->run(cfg);

      // Check cancellation
      if (cancelFlag->exchange(false, std::memory_order_acq_rel)) {
        emit(jsonRpcResult(id, {{"stopReason", "cancelled"}}));
        workerCleanup(sessionId);
        return;
      }

      // Extract agent text
      std::string agentText;

      auto channels = result.channel_raw("messages");
      if (channels.is_array() && !channels.empty()) {
        auto last = channels[channels.size() - 1];
        if (last.contains("content"))
          agentText = last["content"].get<std::string>();
      }
      // Fallback: try response channel
      if (agentText.empty()) {
        auto resp = result.channel_raw("response");
        if (resp.is_string())
          agentText = resp.get<std::string>();
      }

      if (!agentText.empty()) {
        json chunk = json::object();
        chunk["session_id"] = sessionId;
        chunk["update"]["session_update"] = "agent_message_chunk";
        chunk["update"]["content"] =
            json{{"type", "text"}, {"text", agentText}};
        chunk["update"]["raw"] = chunk["update"];
        emitNotification("session/update", chunk);
      }

      emit(jsonRpcResult(id, {{"stopReason", "end_turn"}}));
    } catch (const std::exception &e) {
      XX_LOGE("[acp] worker error: {}", e.what());
      emitAgentMessageChunk(sessionId,
                            "(graph error: " + std::string(e.what()) + ")");
      emit(jsonRpcResult(id, {{"stopReason", "end_turn"}}));
    }

    workerCleanup(sessionId);
  }

  void workerCleanup(const std::string &sessionId) {
    {
      std::lock_guard lk(inflightMu_);
      inflightSessions_.erase(sessionId);
    }
    auto prev = inflightCount_.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) {
      std::lock_guard lk(workersMu_);
      workersCv_.notify_all();
    }
  }

  void handleSessionCancel(const json &params) {
    std::string sessionId = params.value("sessionId", std::string{});
    if (sessionId.empty())
      return;

    std::lock_guard lk(sessionsMu_);
    auto it = cancelFlags_.find(sessionId);
    if (it != cancelFlags_.end() && it->second) {
      it->second->store(true, std::memory_order_release);
      XX_LOGI("[acp] session/cancel: {}", sessionId);
    } else {
      cancelFlags_[sessionId] = std::make_shared<std::atomic<bool>>(true);
    }
  }

  // -----------------------------------------------------------------------
  // Notification / streaming helpers
  // -----------------------------------------------------------------------

  void emit(const json &env) {
    if (sink_)
      sink_(env);
  }

  void emitNotification(const std::string &method, const json &params) {
    json env;
    env["jsonrpc"] = "2.0";
    env["method"] = method;
    env["params"] = params;
    emit(env);
  }

  void emitAgentMessageChunk(const std::string &sessionId,
                             const std::string &text) {
    json chunk;
    chunk["session_id"] = sessionId;
    chunk["update"]["session_update"] = "agent_message_chunk";
    chunk["update"]["content"] = json{{"type", "text"}, {"text", text}};
    chunk["update"]["raw"] = chunk["update"];
    emitNotification("session/update", chunk);
  }

private:
  // -----------------------------------------------------------------------
  // Members
  // -----------------------------------------------------------------------

  Config config_;
  std::shared_ptr<agentxx::agent::DeepAgent> deepAgent_;
  json agentInfo_;
  std::atomic<bool> initialized_{false};
  std::atomic<bool> stopFlag_{false};
  NotificationSink sink_;

  // -- Sessions --
  mutable std::mutex sessionsMu_;
  std::map<std::string, std::string> sessions_; // sessionId → cwd
  std::map<std::string, std::shared_ptr<std::atomic<bool>>> cancelFlags_;

  // -- In-flight prompt tracking --
  mutable std::mutex inflightMu_;
  std::set<std::string> inflightSessions_;
  std::atomic<int> inflightCount_{0};

  std::mutex workersMu_;
  std::condition_variable workersCv_;

  // -- Outbound request tracking (agent→client) --
  mutable std::mutex pendingMu_;
  std::map<int64_t, std::shared_ptr<std::promise<json>>> pending_;
  std::atomic<int64_t> nextOutboundId_{1};
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
    std::string acpEndpoint = "/acp";
    std::string sseEndpoint = "/acp/sse";
    std::string serverName = "agentxx-acp";
    std::string serverVersion = "0.1.0";
    std::chrono::seconds asyncTimeout{120};
  };

  HttpAcpServer(std::shared_ptr<agentxx::agent::DeepAgent> agent,
                json agentInfo, Config config)
      : config_(std::move(config)), deepAgent_(std::move(agent)),
        handler_(deepAgent_, std::move(agentInfo),
                 {.serverName = config_.serverName,
                  .serverVersion = config_.serverVersion}) {
    setupHandlerSink();
    httpServer_ = std::make_unique<util::HttpServer>(config_.httpConfig);
    setupRoutes();
  }

  HttpAcpServer(const HttpAcpServer &) = delete;
  HttpAcpServer &operator=(const HttpAcpServer &) = delete;

  ~HttpAcpServer() { stop(); }

  void start() { httpServer_->start(); }

  void stop() {
    handler_.stop();
    stopSSE();
    httpServer_->stop();
  }

  uint16_t port() const { return httpServer_->port(); }
  bool isStopped() const { return httpServer_->isStopped(); }

  AcpProtocolHandler &handler() { return handler_; }

private:
  // -----------------------------------------------------------------------
  // Wire up the handler's notification sink → SSE + pending resolver
  // -----------------------------------------------------------------------

  void setupHandlerSink() {
    handler_.setNotificationSink([this](const json &envelope) {
      // Fulfill pending outbound promises
      if (!envelope.contains("method") && envelope.contains("id") &&
          !envelope["id"].is_null()) {
        json id = envelope["id"];
        int64_t idVal = id.is_number_integer() ? id.get<int64_t>() : -1;

        std::unique_lock lock(pendingMutex_);
        auto it = pendingResponses_.find(idVal);
        if (it != pendingResponses_.end()) {
          it->second->set_value(envelope);
          pendingResponses_.erase(it);
        }
      }

      // Broadcast to SSE clients (if any)
      std::string sseData;
      bool isResponse = !envelope.contains("method") &&
                        envelope.contains("id") && !envelope["id"].is_null();
      if (isResponse) {
        sseData = "data: " + envelope.dump() + "\n\n";
      } else {
        std::string method = envelope.value("method", "");
        std::string eventType = method;
        if (!eventType.empty()) {
          for (auto &c : eventType)
            if (c == '/')
              c = '_';
          sseData =
              "event: " + eventType + "\ndata: " + envelope.dump() + "\n\n";
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

    auto acpHandler = std::make_shared<Handler>(Handler(
        [this](util::HttpServer::Request &req, util::HttpServer::Response &resp,
               const std::string &) -> asio::awaitable<void> {
          co_await handleAcpRequest(req, resp);
        }));
    httpServer_->router().add(config_.acpEndpoint, 2, acpHandler);

    auto sseHandler = std::make_shared<Handler>(Handler(
        [this](util::HttpServer::Request &req, util::HttpServer::Response &resp,
               const std::string &) -> asio::awaitable<void> {
          co_await handleSseRequest(req, resp);
        }));
    httpServer_->router().add(config_.sseEndpoint, 0, sseHandler);
  }

  // -----------------------------------------------------------------------
  // ACP request handler (HTTP JSON-RPC)
  // -----------------------------------------------------------------------

  asio::awaitable<void> handleAcpRequest(util::HttpServer::Request &req,
                                         util::HttpServer::Response &resp) {
    namespace http = boost::beast::http;

    json requestJson;
    try {
      requestJson = json::parse(req.body());
    } catch (const json::parse_error &e) {
      writeJsonResponse(resp, http::status::bad_request,
                        AcpProtocolHandler::makeParseError(e.what()));
      co_return;
    }

    if (!requestJson.is_object() || requestJson.value("jsonrpc", "") != "2.0") {
      writeJsonResponse(resp, http::status::bad_request,
                        AcpProtocolHandler::makeInvalidRequest());
      co_return;
    }

    json id = requestJson.contains("id") ? requestJson["id"] : json{};

    json response;
    try {
      response = handler_.handleMessage(requestJson);
    } catch (const std::exception &e) {
      XX_LOGE("[acp] handleMessage error: {}", e.what());
      writeJsonResponse(
          resp, http::status::internal_server_error,
          jsonRpcError(id, -32603, "Internal error: " + std::string(e.what())));
      co_return;
    }

    // Synchronous response
    if (!response.is_null()) {
      writeJsonResponse(resp, http::status::ok, response);
      co_return;
    }

    // Notification (no response expected)
    if (id.is_null()) {
      writeJsonResponse(resp, http::status::accepted,
                        json{{"jsonrpc", "2.0"},
                             {"id", json{nullptr}},
                             {"result", json::object()}});
      co_return;
    }

    // Async (session/prompt): wait for response via notification sink
    int64_t idVal = id.is_number_integer() ? id.get<int64_t>() : -1;
    if (idVal < 0 && id.is_string()) {
      std::string idStr = id.get<std::string>();
      idVal = static_cast<int64_t>(std::hash<std::string>{}(idStr));
    }

    auto promise = std::make_shared<std::promise<json>>();
    auto future = promise->get_future().share();

    {
      std::unique_lock lock(pendingMutex_);
      auto [it, inserted] = pendingResponses_.try_emplace(idVal, promise);
      if (!inserted) {
        writeJsonResponse(resp, http::status::internal_server_error,
                          jsonRpcError(id, -32603, "Duplicate request ID"));
        co_return;
      }
    }

    // Wait for async response with timeout
    auto status = future.wait_for(config_.asyncTimeout);
    if (status == std::future_status::timeout) {
      std::unique_lock lock(pendingMutex_);
      pendingResponses_.erase(idVal);
      writeJsonResponse(resp, http::status::gateway_timeout,
                        jsonRpcError(id, -32000, "Async request timed out"));
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
  // SSE endpoint
  // -----------------------------------------------------------------------

  asio::awaitable<void> handleSseRequest(util::HttpServer::Request &req,
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

  void broadcastSSE(const std::string & /*data*/) {
    // SSE broadcast through the HttpServer model is limited.
    // Logged for observability when access logging is enabled.
    if (config_.httpConfig.accessLogEnabled) {
      XX_LOGI("[acp] SSE notification");
    }
  }

  void stopSSE() {}

  // -----------------------------------------------------------------------
  // HTTP response helpers
  // -----------------------------------------------------------------------

  void writeJsonResponse(util::HttpServer::Response &resp,
                         boost::beast::http::status status, const json &body) {
    resp.result(status);
    resp.set(boost::beast::http::field::content_type, "application/json");
    resp.body() = body.dump();
    resp.prepare_payload();
  }

  json jsonRpcError(const json &id, int code,
                    const std::string &message) const {
    json err;
    err["jsonrpc"] = "2.0";
    err["id"] = id;
    err["error"] = {{"code", code}, {"message", message}};
    return err;
  }

  // -----------------------------------------------------------------------
  // Members
  // -----------------------------------------------------------------------

  Config config_;
  std::shared_ptr<agentxx::agent::DeepAgent> deepAgent_;
  AcpProtocolHandler handler_;
  std::unique_ptr<util::HttpServer> httpServer_;

  // Pending async response tracking (for HTTP transport)
  std::mutex pendingMutex_;
  std::map<int64_t, std::shared_ptr<std::promise<json>>> pendingResponses_;
};

// ===========================================================================
// Stdio ACP Server
//
// Reads newline-delimited JSON-RPC from an input stream and writes
// responses to an output stream (default: std::cin / std::cout).
// ===========================================================================

class StdioAcpServer {
public:
  StdioAcpServer(std::shared_ptr<agentxx::agent::DeepAgent> agent,
                 json agentInfo)
      : deepAgent_(std::move(agent)),
        handler_(
            deepAgent_, std::move(agentInfo),
            {.serverName = "agentxx-acp-stdio", .serverVersion = "0.1.0"}) {}

  StdioAcpServer(const StdioAcpServer &) = delete;
  StdioAcpServer &operator=(const StdioAcpServer &) = delete;

  ~StdioAcpServer() { stop(); }

  void run() { run(std::cin, std::cout); }

  void run(std::istream &in, std::ostream &out) {
    running_.store(true, std::memory_order_release);

    struct RunningGuard {
      std::atomic<bool> *flag;
      ~RunningGuard() {
        if (flag)
          flag->store(false, std::memory_order_release);
      }
    };
    RunningGuard guard{&running_};

    auto outMu = std::make_shared<std::mutex>();
    auto outPtr = &out;

    // Install notification sink: write JSON-RPC envelopes as NDJSON lines
    handler_.setNotificationSink([outPtr, outMu](const json &env) {
      auto s = env.dump();
      std::lock_guard lk(*outMu);
      (*outPtr) << s << '\n';
      outPtr->flush();
    });

    std::string line;
    while (!handler_.stopRequested() && std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (line.empty())
        continue;

      json env;
      try {
        env = json::parse(line);
      } catch (const std::exception &) {
        std::lock_guard lk(*outMu);
        (*outPtr) << AcpProtocolHandler::makeParseError("invalid JSON").dump()
                  << '\n';
        outPtr->flush();
        continue;
      }

      auto resp = handler_.handleMessage(env);
      if (!resp.is_null()) {
        std::lock_guard lk(*outMu);
        (*outPtr) << resp.dump() << '\n';
        outPtr->flush();
      }
    }

    // Drain in-flight workers before tearing down the sink
    handler_.drainWorkers();

    // Clear the notification sink
    handler_.setNotificationSink(nullptr);
  }

  void stop() { handler_.stop(); }

  bool isRunning() const { return running_.load(std::memory_order_acquire); }

  AcpProtocolHandler &handler() { return handler_; }

private:
  std::shared_ptr<agentxx::agent::DeepAgent> deepAgent_;
  AcpProtocolHandler handler_;
  std::atomic<bool> running_{false};
};

} // namespace server
} // namespace agentxx
