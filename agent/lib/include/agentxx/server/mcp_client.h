#pragma once

#include "agentxx/server/mcp_server.h"
#include "agentxx/tools/tool.h"
#include "agentxx/util/http_client.h"
#include "agentxx/util/log.h"
#include "asio/awaitable.hpp"
#include "asio/cancel_after.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"

#include <atomic>
#include <chrono>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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

namespace agentxx {
namespace server {

// ---------------------------------------------------------------------------
// McpClient — async MCP client with HTTP + stdio transports
// ---------------------------------------------------------------------------

class McpClientTool;

class McpClient : public std::enable_shared_from_this<McpClient> {
public:
  static constexpr std::string_view kProtocol2024_11_05 = "2024-11-05";
  static constexpr std::string_view kProtocol2025_03_26 = "2025-03-26";
  static constexpr std::string_view kProtocol2025_11_25 = "2025-11-25";
  static constexpr std::string_view kSupportedProtocols[] = {
      kProtocol2025_11_25,
      kProtocol2025_03_26,
      kProtocol2024_11_05,
  };

  struct Config {
    std::string serverUrl;
    std::vector<std::string> serverCommand;
    std::string clientName = "agentxx-mcp-client";
    std::string clientVersion = "0.1.0";
    std::string protocolVersion{kProtocol2024_11_05};
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

  explicit McpClient(Config config) : config_(std::move(config)) {}

  ~McpClient() { closeInternal(); }

  McpClient(const McpClient &) = delete;
  McpClient &operator=(const McpClient &) = delete;

  // -----------------------------------------------------------------------
  // Lifecycle
  // -----------------------------------------------------------------------

  asio::awaitable<std::expected<InitializeResult, std::string>>
  initialize() {
    if (initialized_.load()) {
      co_return std::unexpected{std::string{"already initialized"}};
    }
    if (config_.isStdio()) {
      if (!startStdioSubprocess()) {
        co_return std::unexpected{std::string{"failed to start subprocess"}};
      }
    }

    json clientInfo;
    clientInfo["name"] = config_.clientName;
    clientInfo["version"] = config_.clientVersion;

    json params;
    params["protocolVersion"] = config_.protocolVersion;
    params["capabilities"] = json::object();
    params["clientInfo"] = std::move(clientInfo);

    auto resp = co_await sendRequest("initialize", std::move(params));
    if (!resp.has_value()) {
      co_return std::unexpected{std::move(resp.error())};
    }

    auto &result = resp.value();
    if (!result.contains("result")) {
      std::string errMsg = "initialize returned no result";
      if (result.contains("error")) {
        errMsg = result["error"].value("message", errMsg);
      }
      co_return std::unexpected{std::move(errMsg)};
    }

    json r = result["result"];
    InitializeResult info;
    info.protocolVersion =
        r.value("protocolVersion", std::string{kProtocol2024_11_05});
    if (r.contains("capabilities") && r["capabilities"].is_object())
      info.capabilities = r["capabilities"];
    if (r.contains("serverInfo") && r["serverInfo"].is_object()) {
      info.serverName = r["serverInfo"].value("name", std::string{});
      info.serverVersion = r["serverInfo"].value("version", std::string{});
    }

    // Negotiate protocol version — be lenient with the server
    auto negotiated =
        negotiateProtocolVersion(config_.protocolVersion, r);
    info.protocolVersion = std::move(negotiated);

    // Send initialized notification (fire-and-forget)
    if (!config_.isStdio()) {
      // For HTTP, we send the notification through the same mechanism
      // but since it's a notification (no id), we just fire it
      co_await sendRawNotification("notifications/initialized", json::object());
    }
    // For stdio, the notification is sent automatically in the subprocess

    serverInfo_ = info;
    initialized_.store(true);
    co_return std::move(info);
  }

  asio::awaitable<void> close() {
    closeInternal();
    co_return;
  }

  bool isInitialized() const { return initialized_.load(); }
  bool isClosed() const { return closed_.load(); }
  const InitializeResult &serverInfo() const { return serverInfo_; }

  // -----------------------------------------------------------------------
  // MCP methods
  // -----------------------------------------------------------------------

  asio::awaitable<std::expected<bool, std::string>> ping() {
    auto resp = co_await sendRequest("ping", json::object());
    if (!resp.has_value())
      co_return std::unexpected{std::move(resp.error())};
    co_return resp.value().contains("result");
  }

  asio::awaitable<std::expected<std::vector<McpToolDefinition>, std::string>>
  listTools() {
    auto resp = co_await sendRequest("tools/list", json::object());
    if (!resp.has_value())
      co_return std::unexpected{std::move(resp.error())};
    json result = resp.value();

    std::vector<McpToolDefinition> tools;
    if (!result.contains("result") || !result["result"].is_object())
      co_return tools;

    json r = result["result"];
    if (!r.contains("tools") || !r["tools"].is_array())
      co_return tools;

    for (const auto &t : r["tools"]) {
      McpToolDefinition def;
      def.name = t.value("name", std::string{});
      if (def.name.empty())
        continue;
      def.description = t.value("description", std::string{});
      def.title = t.value("title", std::string{});
      if (t.contains("inputSchema") && t["inputSchema"].is_object())
        def.inputSchema = t["inputSchema"];
      else if (t.contains("input_schema") && t["input_schema"].is_object())
        def.inputSchema = t["input_schema"]; // tolerate snake_case
      if (t.contains("outputSchema") && t["outputSchema"].is_object())
        def.outputSchema = t["outputSchema"];
      if (t.contains("annotations") && t["annotations"].is_object())
        def.annotations = t["annotations"];
      if (t.contains("execution") && t["execution"].is_object())
        def.execution = t["execution"];
      tools.push_back(std::move(def));
    }
    co_return tools;
  }

  asio::awaitable<std::expected<json, std::string>>
  callTool(const std::string &name, const json &arguments = json::object()) {
    json params;
    params["name"] = name;
    params["arguments"] = arguments;
    auto resp = co_await sendRequest("tools/call", std::move(params));
    if (!resp.has_value())
      co_return std::unexpected{std::move(resp.error())};
    json result = resp.value();

    // If the server returned an error envelope, still return it
    if (result.contains("error"))
      co_return result;

    if (!result.contains("result"))
      co_return json::object();

    json r = result["result"];
    if (!r.contains("content") && !r.contains("structuredContent"))
      co_return r;

    co_return r;
  }

  asio::awaitable<
      std::expected<std::vector<McpResourceDefinition>, std::string>>
  listResources() {
    auto resp = co_await sendRequest("resources/list", json::object());
    if (!resp.has_value())
      co_return std::unexpected{std::move(resp.error())};

    std::vector<McpResourceDefinition> resources;
    if (!resp.value().contains("result"))
      co_return resources;

    json r = resp.value()["result"];
    if (!r.contains("resources") || !r["resources"].is_array())
      co_return resources;

    for (const auto &res : r["resources"]) {
      McpResourceDefinition def;
      def.uri = res.value("uri", std::string{});
      if (def.uri.empty())
        continue;
      def.name = res.value("name", std::string{});
      def.description = res.value("description", std::string{});
      def.mimeType = res.value("mimeType", std::string{});
      resources.push_back(std::move(def));
    }
    co_return resources;
  }

  asio::awaitable<std::expected<McpResourceContent, std::string>>
  readResource(const std::string &uri) {
    json params;
    params["uri"] = uri;
    auto resp = co_await sendRequest("resources/read", std::move(params));
    if (!resp.has_value())
      co_return std::unexpected{std::move(resp.error())};

    json result = resp.value();
    if (!result.contains("result"))
      co_return std::unexpected{std::string{"no result in response"}};

    json r = result["result"];
    McpResourceContent content;
    content.uri = uri;
    if (r.contains("contents") && r["contents"].is_array() &&
        !r["contents"].empty()) {
      json c = r["contents"][0];
      content.uri = c.value("uri", uri);
      content.mimeType = c.value("mimeType", std::string{});
      if (c.contains("text"))
        content.text = c["text"].get<std::string>();
      else if (c.contains("blob"))
        content.text = c["blob"].get<std::string>();
    }
    co_return content;
  }

  asio::awaitable<std::expected<std::vector<McpPromptDefinition>, std::string>>
  listPrompts() {
    auto resp = co_await sendRequest("prompts/list", json::object());
    if (!resp.has_value())
      co_return std::unexpected{std::move(resp.error())};

    std::vector<McpPromptDefinition> prompts;
    if (!resp.value().contains("result"))
      co_return prompts;

    json r = resp.value()["result"];
    if (!r.contains("prompts") || !r["prompts"].is_array())
      co_return prompts;

    for (const auto &p : r["prompts"]) {
      McpPromptDefinition def;
      def.name = p.value("name", std::string{});
      if (def.name.empty())
        continue;
      def.description = p.value("description", std::string{});
      if (p.contains("arguments") && p["arguments"].is_array()) {
        for (const auto &a : p["arguments"]) {
          McpPromptArgument arg;
          arg.name = a.value("name", std::string{});
          arg.description = a.value("description", std::string{});
          arg.required = a.value("required", false);
          def.arguments.push_back(std::move(arg));
        }
      }
      prompts.push_back(std::move(def));
    }
    co_return prompts;
  }

  asio::awaitable<std::expected<McpPromptResult, std::string>>
  getPrompt(const std::string &name,
            const json &arguments = json::object()) {
    json params;
    params["name"] = name;
    params["arguments"] = arguments;
    auto resp = co_await sendRequest("prompts/get", std::move(params));
    if (!resp.has_value())
      co_return std::unexpected{std::move(resp.error())};

    json result = resp.value();
    if (!result.contains("result"))
      co_return std::unexpected{std::string{"no result in response"}};

    json r = result["result"];
    McpPromptResult pr;
    pr.description = r.value("description", std::string{});
    if (r.contains("messages") && r["messages"].is_array()) {
      for (const auto &m : r["messages"]) {
        McpPromptMessage msg;
        msg.role = m.value("role", std::string{"user"});
        if (m.contains("content"))
          msg.content = m["content"];
        pr.messages.push_back(std::move(msg));
      }
    }
    co_return pr;
  }

  // -----------------------------------------------------------------------
  // Tool adapter factory
  // -----------------------------------------------------------------------

  std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>
  createTools(std::weak_ptr<agentxx::agent::AgentContext> ctx) {
    std::vector<std::unique_ptr<agentxx::tools::XXToolBase>> tools;
    auto self = shared_from_this();
    // We can't call listTools() synchronously, so return empty and let
    // the user call listTools() then create McpClientTool individually.
    // For now we provide the factory function for direct tool creation.
    return tools;
  }

  /// Create a single tool adapter from a tool definition
  std::unique_ptr<McpClientTool>
  createTool(McpToolDefinition def,
             std::weak_ptr<agentxx::agent::AgentContext> ctx) {
    return std::make_unique<McpClientTool>(shared_from_this(), std::move(def),
                                           std::move(ctx));
  }

  // -----------------------------------------------------------------------
  // Internal: JSON-RPC request/response
  // -----------------------------------------------------------------------

private:
  friend class McpClientTool;

  struct PendingRequest {
    std::promise<json> promise;
  };

  static json makeRequest(int64_t id, const std::string &method,
                          const json &params) {
    json req;
    req["jsonrpc"] = "2.0";
    req["id"] = id;
    req["method"] = method;
    req["params"] = params;
    return req;
  }

  static std::optional<std::string>
  getErrorFromResponse(const json &response) {
    if (response.contains("error") && response["error"].is_object()) {
      json err = response["error"];
      int code = err.value("code", 0);
      std::string msg = err.value("message", "unknown error");
      if (err.contains("data") && !err["data"].is_null())
        msg += " (data: " + err["data"].dump() + ")";
      return "JSON-RPC error " + std::to_string(code) + ": " + msg;
    }
    return std::nullopt;
  }

  static std::string
  negotiateProtocolVersion(const std::string &requested,
                           const json &serverResult) {
    auto serverVersion =
        serverResult.value("protocolVersion", std::string{});
    if (serverVersion.empty())
      return std::string(kProtocol2024_11_05);

    // If the server returned exactly what we requested, use it
    if (serverVersion == requested)
      return serverVersion;

    // Check if server version is in our supported list
    for (const auto &sv : kSupportedProtocols) {
      if (sv == serverVersion)
        return serverVersion;
    }

    // Lenient: accept what the server gives if it looks like a version string
    if (serverVersion.find("2024") != std::string::npos ||
        serverVersion.find("2025") != std::string::npos)
      return serverVersion;

    // Fall back to our requested version
    return requested;
  }

  asio::awaitable<std::expected<json, std::string>>
  sendRequest(const std::string &method, const json &params) {
    if (closed_.load())
      co_return std::unexpected{std::string{"client is closed"}};

    int64_t id = nextId_.fetch_add(1);

    if (config_.isHttp()) {
      auto result = co_await sendHttpRequest(id, method, params);
      co_return std::move(result);
    } else if (config_.isStdio()) {
      auto result = co_await sendStdioRequest(id, method, params);
      co_return std::move(result);
    }
    co_return std::unexpected{std::string{"no transport configured"}};
  }

  asio::awaitable<std::expected<json, std::string>>
  sendHttpRequest(int64_t id, const std::string &method, const json &params) {
    auto req = makeRequest(id, method, params);

    // 2025-11-25: include MCP-Protocol-Version header for HTTP transport
    auto headers = config_.extraHeaders;
    if (config_.protocolVersion == kProtocol2025_11_25) {
      if (!headers.contains("MCP-Protocol-Version"))
        headers.set("MCP-Protocol-Version", std::string{kProtocol2025_11_25});
    }

    auto resp = co_await util::HttpClient::postAsync(
        config_.serverUrl, req, headers, config_.requestTimeout);

    if (!resp.has_value()) {
      co_return std::unexpected{std::move(resp.error())};
    }
    auto &httpResp = resp.value();
    if (httpResp.status / 100 != 2) {
      co_return std::unexpected{
          "HTTP " + std::to_string(httpResp.status) + ": " +
          httpResp.body.substr(0, 256)};
    }

    auto bodyJson = httpResp.bodyJson();
    if (!bodyJson.has_value()) {
      co_return std::unexpected{
          "invalid JSON response: " + httpResp.body.substr(0, 256)};
    }

    json j = bodyJson.value();
    auto err = getErrorFromResponse(j);
    if (err.has_value()) {
      // Return the error as a result with the error field
      // The caller can check for "error" in the returned json
      co_return j;
    }

    // Check the id matches
    if (j.contains("id") && !j["id"].is_null()) {
      auto respId = j["id"];
      bool idMatch = false;
      if (respId.is_number_integer())
        idMatch = respId.get<int64_t>() == id;
      else if (respId.is_string())
        idMatch = respId.get<std::string>() == std::to_string(id);

      if (!idMatch) {
        XX_LOGW("[McpClient] response id mismatch: sent={}, got={}", id,
                respId.dump());
        // Continue anyway — be lenient
      }
    }

    co_return j;
  }

  asio::awaitable<std::expected<json, std::string>>
  sendStdioRequest(int64_t id, const std::string &method, const json &params) {
    auto req = makeRequest(id, method, params);
    auto reqStr = req.dump() + "\n";

    auto promise = std::make_shared<PendingRequest>();
    auto future = promise->promise.get_future();

    {
      std::lock_guard lock(pendingMutex_);
      pending_[id] = std::move(promise);
    }

    {
      std::lock_guard lock(stdioWriteMutex_);
#if XX_IS_LINUX_D || XX_IS_MACOS_D
      auto data = reqStr;
      const char *buf = data.data();
      size_t remaining = data.size();
      while (remaining > 0) {
        ssize_t n = write(stdioStdinFd_, buf, remaining);
        if (n <= 0)
          break;
        buf += n;
        remaining -= static_cast<size_t>(n);
      }
#elif XX_IS_WIN_D
      DWORD written = 0;
      WriteFile(stdioStdinHandle_, reqStr.data(),
                static_cast<DWORD>(reqStr.size()), &written, nullptr);
#endif
    }

    // Co-await the future result
    auto executor = co_await asio::this_coro::executor;
    json response;

    while (true) {
      if (future.wait_for(std::chrono::milliseconds(5)) ==
          std::future_status::ready) {
        response = future.get();
        break;
      }
      // Yield to asio event loop
      asio::steady_timer timer(executor);
      timer.expires_after(std::chrono::milliseconds(5));
      co_await timer.async_wait(
          asio::redirect_error(asio::use_awaitable, ignoreEc_));
    }

    auto err = getErrorFromResponse(response);
    if (err.has_value()) {
      co_return response; // Return with error included
    }
    co_return response;
  }

  asio::awaitable<void>
  sendRawNotification(const std::string &method, const json &params) {
    json req;
    req["jsonrpc"] = "2.0";
    req["method"] = method;
    if (!params.is_null())
      req["params"] = params;

    if (config_.isHttp()) {
      [[maybe_unused]] auto resp = co_await util::HttpClient::postAsync(
          config_.serverUrl, req, config_.extraHeaders,
          config_.requestTimeout);
    } else if (config_.isStdio()) {
      auto reqStr = req.dump() + "\n";
      std::lock_guard lock(stdioWriteMutex_);
#if XX_IS_LINUX_D || XX_IS_MACOS_D
      write(stdioStdinFd_, reqStr.data(), reqStr.size());
#elif XX_IS_WIN_D
      DWORD written = 0;
      WriteFile(stdioStdinHandle_, reqStr.data(),
                static_cast<DWORD>(reqStr.size()), &written, nullptr);
#endif
    }
    co_return;
  }

  // -----------------------------------------------------------------------
  // Stdio subprocess management
  // -----------------------------------------------------------------------

  bool startStdioSubprocess() {
    if (config_.serverCommand.empty())
      return false;

#if XX_IS_LINUX_D || XX_IS_MACOS_D
    int stdinPipe[2] = {-1, -1};
    int stdoutPipe[2] = {-1, -1};

    if (::pipe(stdinPipe) != 0 || ::pipe(stdoutPipe) != 0) {
      return false;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
      ::close(stdinPipe[0]);
      ::close(stdinPipe[1]);
      ::close(stdoutPipe[0]);
      ::close(stdoutPipe[1]);
      return false;
    }

    if (pid == 0) {
      // Child process
      ::close(stdinPipe[1]);  // Close write end of stdin pipe
      ::close(stdoutPipe[0]); // Close read end of stdout pipe

      ::dup2(stdinPipe[0], STDIN_FILENO);
      ::dup2(stdoutPipe[1], STDOUT_FILENO);

      // Close all other fds
      int maxFd = static_cast<int>(::sysconf(_SC_OPEN_MAX));
      for (int i = 3; i < maxFd; i++) {
        if (i != stdinPipe[0] && i != stdoutPipe[1])
          ::close(i);
      }

      // Build argv
      std::vector<char *> argv;
      for (auto &arg : config_.serverCommand) {
        argv.push_back(const_cast<char *>(arg.data()));
      }
      argv.push_back(nullptr);

      ::execvp(argv[0], argv.data());
      ::_exit(127); // exec failed
    }

    // Parent process
    ::close(stdinPipe[0]);  // Close read end of stdin pipe
    ::close(stdoutPipe[1]); // Close write end of stdout pipe

    stdioStdinFd_ = stdinPipe[1];
    stdioStdoutFd_ = stdoutPipe[0];
    stdioChildPid_ = static_cast<int>(pid);

    // Set stdout pipe to non-blocking for the reader thread
    int flags = ::fcntl(stdioStdoutFd_, F_GETFL, 0);
    ::fcntl(stdioStdoutFd_, F_SETFL, flags | O_NONBLOCK);

    // Start reader thread
    stdioRunning_ = true;
    stdioReaderThread_ = std::thread([this]() { stdioReaderLoop(); });

    return true;

#elif XX_IS_WIN_D
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;

    HANDLE parentStdinRd = nullptr, childStdinWr = nullptr;
    HANDLE childStdoutRd = nullptr, parentStdoutWr = nullptr;

    if (!CreatePipe(&parentStdinRd, &childStdinWr, &sa, 0) ||
        !CreatePipe(&childStdoutRd, &parentStdoutWr, &sa, 0)) {
      return false;
    }

    SetHandleInformation(childStdinWr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(childStdoutRd, HANDLE_FLAG_INHERIT, 0);

    std::string cmdLine;
    for (size_t i = 0; i < config_.serverCommand.size(); i++) {
      if (i > 0)
        cmdLine += " ";
      cmdLine += config_.serverCommand[i];
    }

    PROCESS_INFORMATION pi;
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdError = parentStdoutWr;
    si.hStdOutput = parentStdoutWr;
    si.hStdInput = parentStdinRd;
    si.dwFlags |= STARTF_USESTDHANDLES;

    if (!CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, 0,
                        nullptr, nullptr, &si, &pi)) {
      CloseHandle(parentStdinRd);
      CloseHandle(childStdinWr);
      CloseHandle(childStdoutRd);
      CloseHandle(parentStdoutWr);
      return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(parentStdinRd);
    CloseHandle(parentStdoutWr);

    stdioStdinHandle_ = childStdinWr;
    stdioStdoutHandle_ = childStdoutRd;
    stdioChildProcess_ = pi.hProcess;

    stdioRunning_ = true;
    stdioReaderThread_ = std::thread([this]() { stdioReaderLoop(); });
    return true;
#else
    return false;
#endif
  }

  void closeInternal() {
    bool expected = false;
    if (!closed_.compare_exchange_strong(expected, true))
      return;

    initialized_.store(false);

#if XX_IS_LINUX_D || XX_IS_MACOS_D
    if (stdioChildPid_ > 0) {
      stdioRunning_ = false;
      ::kill(static_cast<pid_t>(stdioChildPid_), SIGTERM);

      if (stdioReaderThread_.joinable())
        stdioReaderThread_.join();

      int status = 0;
      ::waitpid(static_cast<pid_t>(stdioChildPid_), &status, WNOHANG);

      if (stdioStdinFd_ >= 0)
        ::close(stdioStdinFd_);
      if (stdioStdoutFd_ >= 0)
        ::close(stdioStdoutFd_);
      stdioStdinFd_ = -1;
      stdioStdoutFd_ = -1;
      stdioChildPid_ = -1;
    }
#elif XX_IS_WIN_D
    if (stdioChildProcess_ != nullptr) {
      stdioRunning_ = false;
      TerminateProcess(stdioChildProcess_, 0);
      if (stdioReaderThread_.joinable())
        stdioReaderThread_.join();
      CloseHandle(stdioChildProcess_);
      CloseHandle(stdioStdinHandle_);
      CloseHandle(stdioStdoutHandle_);
      stdioChildProcess_ = nullptr;
      stdioStdinHandle_ = nullptr;
      stdioStdoutHandle_ = nullptr;
    }
#endif

    // Fulfill any remaining pending requests with error
    std::lock_guard lock(pendingMutex_);
    for (auto &[id, req] : pending_) {
      json errorResp;
      errorResp["jsonrpc"] = "2.0";
      errorResp["id"] = id;
      errorResp["error"] =
          jsonRpcError(-32000, "Client closed before response");
      try {
        req->promise.set_value(std::move(errorResp));
      } catch (...) {
      }
    }
    pending_.clear();
  }

  void stdioReaderLoop() {
#if XX_IS_LINUX_D || XX_IS_MACOS_D
    std::string buffer;
    constexpr size_t kBufSize = 4096;

    while (stdioRunning_) {
      char buf[kBufSize];
      ssize_t n = ::read(stdioStdoutFd_, buf, kBufSize);
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          continue;
        }
        break; // EOF or error
      }
      if (n == 0)
        break; // EOF

      buffer.append(buf, static_cast<size_t>(n));

      // Process complete lines
      size_t pos;
      while ((pos = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);

        if (line.empty())
          continue;

        try {
          auto response = json::parse(line);
          deliverResponse(response);
        } catch (const json::parse_error &) {
          XX_LOGW("[McpClient] ignoring malformed stdout line: {}",
                  line.substr(0, 128));
        }
      }
    }

    // Process remaining buffer
    if (!buffer.empty()) {
      try {
        auto response = json::parse(buffer);
        deliverResponse(response);
      } catch (...) {
      }
    }
#elif XX_IS_WIN_D
    std::string buffer;
    char buf[4096];

    while (stdioRunning_) {
      DWORD read = 0;
      if (!ReadFile(stdioStdoutHandle_, buf, sizeof(buf) - 1, &read,
                    nullptr)) {
        break;
      }
      if (read == 0)
        break;

      buffer.append(buf, static_cast<size_t>(read));

      size_t pos;
      while ((pos = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);
        if (line.empty())
          continue;
        try {
          auto response = json::parse(line);
          deliverResponse(response);
        } catch (...) {
        }
      }
    }
#endif
    stdioRunning_ = false;
  }

  void deliverResponse(const json &response) {
    if (!response.contains("id"))
      return; // notification, ignore
    json respId = response["id"];
    if (respId.is_null())
      return;

    int64_t idVal = -1;
    if (respId.is_number_integer())
      idVal = respId.get<int64_t>();
    else if (respId.is_number_unsigned())
      idVal = static_cast<int64_t>(respId.get<uint64_t>());
    else if (respId.is_string()) {
      try {
        idVal = std::stoll(respId.get<std::string>());
      } catch (...) {
        return;
      }
    } else
      return;

    std::shared_ptr<PendingRequest> req;
    {
      std::lock_guard lock(pendingMutex_);
      auto it = pending_.find(idVal);
      if (it == pending_.end())
        return;
      req = std::move(it->second);
      pending_.erase(it);
    }

    try {
      req->promise.set_value(response);
    } catch (...) {
    }
  }

  Config config_;
  std::atomic<bool> initialized_{false};
  std::atomic<bool> closed_{false};
  InitializeResult serverInfo_;
  std::atomic<int64_t> nextId_{1};

  // Stdio transport state
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
                std::weak_ptr<agentxx::agent::AgentContext> ctx)
      : agentxx::tools::XXToolBase(def.name, std::move(ctx)),
        client_(std::move(client)), def_(std::move(def)) {}

  neograph::ChatTool get_definition() const override {
    neograph::ChatTool tool;
    tool.name = def_.name;
    tool.description = def_.description;
    tool.parameters = def_.inputSchema;
    return tool;
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    auto result = co_await client_->callTool(def_.name, arguments);
    if (!result.has_value()) {
      throw std::runtime_error("MCP tool call [" + def_.name +
                               "] failed: " + result.error());
    }
    json resp = result.value();

    // Extract text content from the MCP content array
    if (resp.contains("content") && resp["content"].is_array()) {
      std::string combined;
      for (const auto &c : resp["content"]) {
        std::string type = c.value("type", "");
        if (type == "text") {
          if (!combined.empty())
            combined += "\n";
          combined += c.value("text", "");
        } else if (type == "audio" || type == "image") {
          // Reference only — actual data is handled externally
          if (!combined.empty())
            combined += "\n";
          combined += "[content type: " + type + ", mimeType: " +
                      c.value("mimeType", "") + "]";
        } else if (type == "resource") {
          if (!combined.empty())
            combined += "\n";
          combined += "[embedded resource: " +
                      c.value("resource", json::object())
                          .value("uri", "unknown") +
                      "]";
        } else if (type == "resource_link") {
          if (!combined.empty())
            combined += "\n";
          combined += "[resource link: " + c.value("uri", "") + "]";
        }
      }
      if (!combined.empty())
        co_return combined;
    }

    // 2025-11-25: structured content as JSON
    if (resp.contains("structuredContent") &&
        !resp["structuredContent"].is_null()) {
      co_return "[structuredContent]: " + resp["structuredContent"].dump();
    }

    // If content array is missing or no text blocks, return the full result
    co_return resp.dump();
  }

  std::string get_name() const override { return def_.name; }

private:
  std::shared_ptr<McpClient> client_;
  McpToolDefinition def_;
};

} // namespace server
} // namespace agentxx
