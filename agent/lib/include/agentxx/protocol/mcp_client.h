#pragma once

#include "agentxx/protocol/mcp_server.h"
#include "agentxx/tools/tool.h"
#include "agentxx/util/http_client.h"
#include "asio/awaitable.hpp"
#include <atomic>
#include <chrono>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <neograph/api.h>
#include <neograph/json.h>
#include <neograph/types.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#if AGENTXX_ENABLE_BOOST_PROCESS
#include "asio/readable_pipe.hpp"
#include "asio/writable_pipe.hpp"
#include "boost/process.hpp"
#else
#include <thread>
#if XX_IS_LINUX_D || XX_IS_MACOS_D
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#elif XX_IS_WIN_D
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#endif

namespace asio = ::boost::asio;

namespace agentxx {
namespace server {

// ---------------------------------------------------------------------------
// McpClient — async MCP client with HTTP + stdio transports
// ---------------------------------------------------------------------------

class McpClientTool;

class McpClient : public std::enable_shared_from_this<McpClient> {
public:
  inline static constexpr std::string_view kProtocol2024_11_05 = "2024-11-05";
  inline static constexpr std::string_view kProtocol2025_03_26 = "2025-03-26";
  inline static constexpr std::string_view kProtocol2025_06_18 = "2025-06-18";
  inline static constexpr std::string_view kProtocol2025_11_25 = "2025-11-25";
  inline static constexpr std::string_view kSupportedProtocols[] = {
      kProtocol2025_11_25,
      kProtocol2025_06_18,
      kProtocol2025_03_26,
      kProtocol2024_11_05,
  };

  struct Config {
    std::string serverUrl;
    std::vector<std::string> serverCommand;
    std::string clientName = "agentxx-mcp-client";
    std::string clientVersion = "0.1.0";
    std::string protocolVersion{kProtocol2024_11_05};
    /// MCP tool 命名空间
    /// - 非空时作为该 client 所有 tool 对外名称的前缀 (格式: "namespace_toolName")
    /// - 远程调用时仍使用 tool 的原始名称
    std::string toolNamespace;
    std::chrono::milliseconds requestTimeout{60000};
    std::chrono::milliseconds initTimeout{10000};
    util::HeaderMap extraHeaders;

    bool isHttp() const { return !serverUrl.empty(); }
    bool isStdio() const { return !serverCommand.empty(); }
    bool isValid() const { return isHttp() || isStdio(); }
  };

  struct InitializeResult {
    std::string protocolVersion;
    json capabilities;
    std::string serverName;
    std::string serverVersion;
  };

  explicit McpClient(Config config);
  ~McpClient();

  McpClient(const McpClient &) = delete;
  McpClient &operator=(const McpClient &) = delete;

  // -----------------------------------------------------------------------
  // Lifecycle
  // -----------------------------------------------------------------------

  asio::awaitable<std::expected<InitializeResult, std::string>> initialize();
  asio::awaitable<void> close();

  bool isInitialized() const;
  bool isClosed() const;
  const InitializeResult &serverInfo() const;

  // -----------------------------------------------------------------------
  // MCP methods
  // -----------------------------------------------------------------------

  asio::awaitable<std::expected<bool, std::string>> ping();

  asio::awaitable<std::expected<std::vector<McpToolDefinition>, std::string>>
  listTools();

  asio::awaitable<std::expected<json, std::string>>
  callTool(const std::string &name, const json &arguments = json::object());

  asio::awaitable<
      std::expected<std::vector<McpResourceDefinition>, std::string>>
  listResources();

  asio::awaitable<std::expected<McpResourceContent, std::string>>
  readResource(const std::string &uri);

  asio::awaitable<std::expected<std::vector<McpPromptDefinition>, std::string>>
  listPrompts();

  asio::awaitable<std::expected<McpPromptResult, std::string>>
  getPrompt(const std::string &name, const json &arguments = json::object());

  // -----------------------------------------------------------------------
  // Tool adapter factory
  // -----------------------------------------------------------------------

  std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>
  createTools(std::weak_ptr<agentxx::agent::AgentContext> ctx);

  /// Create a single tool adapter from a tool definition
  std::unique_ptr<McpClientTool>
  createTool(McpToolDefinition def,
             std::weak_ptr<agentxx::agent::AgentContext> ctx);

  // -----------------------------------------------------------------------
  // Internal: JSON-RPC request/response
  // -----------------------------------------------------------------------

private:
  friend class McpClientTool;

  struct PendingRequest {
    std::promise<json> promise;
  };

  static json makeRequest(int64_t id, const std::string &method,
                          const json &params);

  static std::optional<std::string>
  getErrorFromResponse(const json &response);

  static std::string
  negotiateProtocolVersion(const std::string &requested,
                           const json &serverResult);

  asio::awaitable<std::expected<json, std::string>>
  sendRequest(const std::string &method, const json &params);

  // -----------------------------------------------------------------------
  // SSE endpoint discovery & event parsing
  // -----------------------------------------------------------------------

  /// Build the SSE endpoint URL from the server URL (append /sse)
  static std::string buildSseUrl(std::string_view serverUrl);

  struct SseEvent {
    std::string event;
    std::string data;
  };

  /// Parse SSE text into a list of (event-type, data) pairs.
  static std::vector<SseEvent> parseSseEvents(const std::string &body);

  /// Extract a value from a URL query string by key.
  static std::string getQueryParam(std::string_view query,
                                   std::string_view key);

  /// Discover the message endpoint by connecting to the SSE URL.
  asio::awaitable<void> discoverSseEndpoint();

  /// Build common headers for MCP HTTP requests.
  util::HeaderMap buildHttpHeaders() const;

  asio::awaitable<std::expected<json, std::string>>
  sendHttpRequest(int64_t id, const std::string &method, const json &params);

  asio::awaitable<std::expected<json, std::string>>
  sendStdioRequest(int64_t id, const std::string &method, const json &params);

  asio::awaitable<void> sendRawNotification(const std::string &method,
                                            const json &params);

  // -----------------------------------------------------------------------
  // Stdio subprocess management
  // -----------------------------------------------------------------------

  bool startStdioSubprocess(asio::any_io_executor executor);

  void closeInternal();

#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
  asio::awaitable<void> stdioReaderLoop();
#else
  void stdioReaderLoop();
#endif

  void deliverResponse(const json &response);

  Config config_;
  std::atomic<bool> initialized_{false};
  std::atomic<bool> closed_{false};
  InitializeResult serverInfo_;
  std::atomic<int64_t> nextId_{1};

  // HTTP SSE transport state
  std::atomic<bool> sseDiscovered_{false};
  std::string httpMessageUrl_;
  std::string mcpSessionId_;

  // Stdio transport state
#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
  std::optional<boost::process::process> stdioProcess_;
  std::optional<asio::writable_pipe> stdioStdinPipe_;
  std::optional<asio::readable_pipe> stdioStdoutPipe_;
#else
#if XX_IS_LINUX_D || XX_IS_MACOS_D
  int stdioStdinFd_ = -1;
  int stdioStdoutFd_ = -1;
  int stdioChildPid_ = -1;
#elif XX_IS_WIN_D
  HANDLE stdioStdinHandle_ = nullptr;
  HANDLE stdioStdoutHandle_ = nullptr;
  HANDLE stdioChildProcess_ = nullptr;
#endif
  std::thread stdioReaderThread_;
#endif
  std::atomic<bool> stdioRunning_{false};
  std::mutex stdioWriteMutex_;
  std::mutex pendingMutex_;
  std::unordered_map<int64_t, std::shared_ptr<PendingRequest>> pending_;
  boost::system::error_code ignoreEc_;
};

// ---------------------------------------------------------------------------
// McpClientTool — wraps a remote MCP tool as an XXToolBase
// ---------------------------------------------------------------------------

class McpClientTool : public agentxx::tools::XXToolBase {
public:
  McpClientTool(std::shared_ptr<McpClient> client, McpToolDefinition def,
                std::weak_ptr<agentxx::agent::AgentContext> ctx,
                std::string toolNamespace = {});

  neograph::ChatTool get_definition() const override;

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override;

  std::string get_name() const override;

  /// 命名空间前缀后的对外 tool 名称 (namespace 非空时为 "namespace_toolName")
  std::string namespacedName() const;

private:
  std::shared_ptr<McpClient> client_;
  McpToolDefinition def_;
  std::string toolNamespace_;
};

} // namespace server
} // namespace agentxx
