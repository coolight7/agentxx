#pragma once

#include "agentxx/util/http_client.h"
#include "agentxx/util/log.h"
#include "asio/awaitable.hpp"
#include "asio/cancel_after.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include <charconv>
#include <chrono>
#include <map>
#include <memory>
#include <neograph/api.h>
#include <neograph/provider.h>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace server {

/// Anthropic Messages API provider.
/// Supports non-streaming, streaming, tool_use, and extended thinking.
class AnthropicProvider : public neograph::Provider {
public:
  struct Config {
    std::string api_key;
    std::string base_url = "https://api.anthropic.com";
    std::string default_model = "claude-sonnet-4-20250514";
    std::string anthropic_version = "2023-06-01";
    int timeout_seconds = 60;
    int max_tokens = 8096; // Anthropic requires max_tokens

    /// Extra JSON fields merged into every request body.
    /// Use this to set thinking, top_k, top_p, stop_sequences, etc.
    neograph::json extra_body = neograph::json::object();
  };

  static std::unique_ptr<AnthropicProvider> create(const Config &config) {
    return std::unique_ptr<AnthropicProvider>(new AnthropicProvider(config));
  }

  static std::shared_ptr<neograph::Provider>
  create_shared(const Config &config) {
    return std::shared_ptr<neograph::Provider>(new AnthropicProvider(config));
  }

  ~AnthropicProvider() override = default;

  std::string get_name() const override { return "anthropic"; }

  asio::awaitable<neograph::ChatCompletion>
  invoke(const neograph::CompletionParams &params,
         neograph::StreamCallback on_chunk = nullptr) override {
    if (!on_chunk) {
      co_return co_await invoke_format_data(params, nullptr);
    }
    co_return co_await invoke_format_data(
        params, [on_chunk](const neograph::ChatStreamChunk &chunk) {
          switch (chunk.type) {
          case neograph::ChatStreamChunk::TYPE_CONTENT:
            on_chunk(chunk.data);
            break;
          case neograph::ChatStreamChunk::TYPE_THINKING:
            break;
          }
        });
  }

  asio::awaitable<neograph::ChatCompletion> invoke_format_data(
      const neograph::CompletionParams &params,
      neograph::FormatDataStreamCallback on_chunk = nullptr) override {
    if (on_chunk) {
      auto body = buildBody(params);
      body["stream"] = true;
      auto result = co_await doStream(params, body, on_chunk);
      co_return result;
    }
    co_return co_await completeAsync(params);
  }

  // --- Public static helpers (exposed for unit testing) ---

  /// Convert neograph messages to Anthropic format.
  /// Returns {system_string, messages_json_array}.
  static std::pair<std::string, neograph::json>
  convertMessages(const std::vector<neograph::ChatMessage> &messages) {
    std::string system;
    neograph::json arr = neograph::json::array();

    for (const auto &msg : messages) {
      if (msg.role == "system") {
        // Anthropic uses top-level system field
        if (!system.empty())
          system += "\n";
        system += msg.content;
        continue;
      }

      if (msg.role == "tool") {
        // Convert tool result to Anthropic format
        neograph::json j;
        j["role"] = "user";
        neograph::json content_arr = neograph::json::array();
        neograph::json tool_result;
        tool_result["type"] = "tool_result";
        tool_result["tool_use_id"] = msg.tool_call_id;
        tool_result["content"] = msg.content;
        content_arr.push_back(std::move(tool_result));
        j["content"] = std::move(content_arr);
        arr.push_back(std::move(j));
      } else if (msg.role == "assistant" && !msg.tool_calls.empty()) {
        // Assistant message with tool calls -> content blocks
        neograph::json j;
        j["role"] = "assistant";
        neograph::json content_arr = neograph::json::array();
        if (!msg.content.empty()) {
          content_arr.push_back({{"type", "text"}, {"text", msg.content}});
        }
        for (const auto &tc : msg.tool_calls) {
          neograph::json tool_use;
          tool_use["type"] = "tool_use";
          tool_use["id"] = tc.id;
          tool_use["name"] = tc.name;
          // Parse arguments string to JSON object
          try {
            tool_use["input"] = neograph::json::parse(tc.arguments);
          } catch (...) {
            tool_use["input"] = neograph::json::object();
          }
          content_arr.push_back(std::move(tool_use));
        }
        j["content"] = std::move(content_arr);
        arr.push_back(std::move(j));
      } else {
        // Regular user/assistant message
        neograph::json j;
        j["role"] = msg.role;
        j["content"] = msg.content;
        arr.push_back(std::move(j));
      }
    }

    return {system, std::move(arr)};
  }

  /// Convert neograph tools to Anthropic format.
  static neograph::json
  convertTools(const std::vector<neograph::ChatTool> &tools) {
    neograph::json arr = neograph::json::array();
    for (const auto &tool : tools) {
      neograph::json t;
      t["name"] = tool.name;
      t["description"] = tool.description;
      t["input_schema"] = tool.parameters;
      arr.push_back(std::move(t));
    }
    return arr;
  }

  /// Parse a non-streaming Anthropic response.
  static neograph::ChatCompletion parseResponse(const neograph::json &resp) {
    neograph::ChatCompletion completion;
    completion.message.role = "assistant";

    if (resp.contains("content") && resp["content"].is_array()) {
      for (const auto &block : resp["content"]) {
        auto type = block.value("type", std::string{});
        if (type == "text") {
          completion.message.content += block.value("text", std::string{});
        } else if (type == "tool_use") {
          neograph::ToolCall tc;
          tc.id = block.value("id", std::string{});
          tc.name = block.value("name", std::string{});
          if (block.contains("input")) {
            tc.arguments = block["input"].dump();
          }
          completion.message.tool_calls.push_back(std::move(tc));
        } else if (type == "thinking") {
          completion.message.reasoning_content +=
              block.value("thinking", std::string{});
        }
      }
    }

    if (resp.contains("usage")) {
      auto u = resp["usage"];
      completion.usage.prompt_tokens = u.value("input_tokens", 0);
      completion.usage.completion_tokens = u.value("output_tokens", 0);
      completion.usage.total_tokens =
          completion.usage.prompt_tokens + completion.usage.completion_tokens;
    }

    return completion;
  }

  /// Process Anthropic SSE buffer.
  static void processSseBuffer(std::string &buf,
                               neograph::ChatCompletion &completion,
                               std::string &fullContent,
                               std::string &fullThinking,
                               std::map<int, neograph::ToolCall> &tcMap,
                               std::map<int, std::string> &blockTypes,
                               neograph::FormatDataStreamCallback on_chunk) {
    size_t pos;
    while ((pos = buf.find("\n\n")) != std::string::npos) {
      std::string block = buf.substr(0, pos);
      buf.erase(0, pos + 2);

      std::string currentEvent;
      std::string payload;

      size_t lineStart = 0;
      while (lineStart < block.size()) {
        auto lineEnd = block.find('\n', lineStart);
        std::string line = (lineEnd == std::string::npos)
                               ? block.substr(lineStart)
                               : block.substr(lineStart, lineEnd - lineStart);
        lineStart = (lineEnd == std::string::npos) ? block.size() : lineEnd + 1;

        if (!line.empty() && line.back() == '\r')
          line.pop_back();

        if (line.rfind("event: ", 0) == 0) {
          currentEvent = line.substr(7);
        } else if (line.rfind("data: ", 0) == 0) {
          payload = line.substr(6);
        }
      }

      if (payload.empty())
        continue;

      try {
        auto j = neograph::json::parse(payload);

        if (currentEvent == "message_start") {
          if (j.contains("message") && j["message"].contains("usage")) {
            auto u = j["message"]["usage"];
            completion.usage.prompt_tokens = u.value("input_tokens", 0);
          }
        } else if (currentEvent == "content_block_start") {
          int idx = j.value("index", 0);
          if (j.contains("content_block")) {
            auto type = j["content_block"].value("type", std::string{});
            blockTypes[idx] = type;
            if (type == "tool_use") {
              tcMap[idx].id = j["content_block"].value("id", std::string{});
              tcMap[idx].name = j["content_block"].value("name", std::string{});
            }
          }
        } else if (currentEvent == "content_block_delta") {
          int idx = j.value("index", 0);
          if (j.contains("delta")) {
            auto deltaType = j["delta"].value("type", std::string{});
            if (deltaType == "text_delta") {
              auto text = j["delta"].value("text", std::string{});
              fullContent += text;
              if (on_chunk) {
                on_chunk(neograph::ChatStreamChunk{
                    neograph::ChatStreamChunk::TYPE_CONTENT, text});
              }
            } else if (deltaType == "thinking_delta") {
              auto thinking = j["delta"].value("thinking", std::string{});
              fullThinking += thinking;
              if (on_chunk) {
                on_chunk(neograph::ChatStreamChunk{
                    neograph::ChatStreamChunk::TYPE_THINKING, thinking});
              }
            } else if (deltaType == "input_json_delta") {
              auto partialJson =
                  j["delta"].value("partial_json", std::string{});
              tcMap[idx].arguments += partialJson;
            }
          }
        } else if (currentEvent == "message_delta") {
          if (j.contains("usage")) {
            auto u = j["usage"];
            completion.usage.completion_tokens = u.value("output_tokens", 0);
            completion.usage.total_tokens = completion.usage.prompt_tokens +
                                            completion.usage.completion_tokens;
          }
        }
      } catch (...) {
      }
    }
  }

private:
  explicit AnthropicProvider(Config config) : config_(std::move(config)) {}

  neograph::json buildBody(const neograph::CompletionParams &params) const {
    neograph::json body;
    body["model"] = params.model.empty() ? config_.default_model : params.model;
    body["max_tokens"] = config_.max_tokens;

    auto [system, messages] = convertMessages(params.messages);
    body["messages"] = std::move(messages);
    if (!system.empty()) {
      body["system"] = system;
    }

    if (!params.tools.empty()) {
      body["tools"] = convertTools(params.tools);
    }

    if (params.temperature >= 0.0f) {
      body["temperature"] = params.temperature;
    }

    if (params.max_tokens > 0) {
      body["max_tokens"] = params.max_tokens;
    }

    // Merge config-level extra body (lower priority)
    if (!config_.extra_body.empty()) {
      for (const auto &[key, val] : config_.extra_body.items()) {
        if (!body.contains(key)) {
          body[key] = val;
        }
      }
    }

    // Apply per-call extra fields (highest priority)
    if (!params.extra_fields.empty()) {
      for (const auto &[key, val] : params.extra_fields.items()) {
        body[key] = val;
      }
    }

    return body;
  }

  struct ParsedEndpoint {
    std::string scheme;
    std::string host;
    uint16_t port;
    std::string prefix;
  };

  static ParsedEndpoint parseEndpoint(const std::string &base_url) {
    ParsedEndpoint ep;
    ep.scheme = "https";
    ep.port = 443;

    auto schemeEnd = base_url.find("://");
    if (schemeEnd == std::string::npos) {
      ep.host = base_url;
      return ep;
    }
    ep.scheme = base_url.substr(0, schemeEnd);
    auto rest = base_url.substr(schemeEnd + 3);
    auto pathStart = rest.find('/');
    std::string hostPort =
        (pathStart == std::string::npos) ? rest : rest.substr(0, pathStart);
    ep.prefix = (pathStart == std::string::npos) ? "" : rest.substr(pathStart);

    ep.port = (ep.scheme == "https") ? 443 : 80;

    auto colon = hostPort.rfind(':');
    if (colon != std::string::npos) {
      ep.host = hostPort.substr(0, colon);
      int p = 0;
      auto [ptr, ec] = std::from_chars(hostPort.data() + colon + 1,
                                       hostPort.data() + hostPort.size(), p);
      if (ec == std::errc{} && p > 0 && p <= 65535) {
        ep.port = static_cast<uint16_t>(p);
      }
    } else {
      ep.host = hostPort;
    }
    return ep;
  }

  asio::awaitable<neograph::ChatCompletion>
  completeAsync(const neograph::CompletionParams &params) {
    using namespace agentxx::util;

    auto bodyJson = buildBody(params);
    auto bodyStr = bodyJson.dump();

    HeaderMap headers;
    headers.set("x-api-key", config_.api_key);
    headers.set("anthropic-version", config_.anthropic_version);

    auto resp = co_await HttpClient::postAsync(
        config_.base_url + "/v1/messages", bodyStr, "application/json", headers,
        std::chrono::seconds{config_.timeout_seconds});

    if (!resp.has_value()) {
      throw std::runtime_error("HTTP request failed: " + resp.error());
    }

    auto &r = resp.value();

    if (r.status == 429) {
      auto raw = r.findHeader("retry-after");
      int retryAfter = -1;
      if (!raw.empty()) {
        int seconds = 0;
        auto [ptr, ec] =
            std::from_chars(raw.data(), raw.data() + raw.size(), seconds);
        if (ec == std::errc{} && seconds >= 0) {
          retryAfter = seconds;
        }
      }
      throw neograph::RateLimitError("API error (HTTP 429): " + r.body,
                                     retryAfter);
    }

    if (r.status != 200) {
      throw std::runtime_error("API error (HTTP " + std::to_string(r.status) +
                               "): " + r.body);
    }

    auto respJson = neograph::json::parse(r.body);
    co_return parseResponse(respJson);
  }

  asio::awaitable<neograph::ChatCompletion>
  doStream(const neograph::CompletionParams &params, const neograph::json &body,
           neograph::FormatDataStreamCallback on_chunk) {
    namespace http = boost::beast::http;
    using asio::ip::tcp;

    auto bodyStr = body.dump();
    auto executor = co_await asio::this_coro::executor;
    auto ep = parseEndpoint(config_.base_url);
    auto target = ep.prefix + "/v1/messages";
    bool isHttps = ep.scheme == "https";

    auto timeout = std::chrono::seconds{config_.timeout_seconds};
    auto deadline = std::chrono::steady_clock::now() + timeout;
    auto rem = [&] {
      auto d = deadline - std::chrono::steady_clock::now();
      return std::chrono::duration_cast<std::chrono::milliseconds>(
          std::max(d, std::chrono::steady_clock::duration::zero()));
    };

    // Build request
    http::request<http::string_body> req{http::verb::post, target, 11};
    req.set(http::field::host, ep.host);
    req.set(http::field::user_agent, "agentxx/1.0");
    req.set(http::field::content_type, "application/json");
    req.set("x-api-key", config_.api_key);
    req.set("anthropic-version", config_.anthropic_version);
    req.set(http::field::accept, "text/event-stream");
    req.set(http::field::connection, "keep-alive");
    req.body() = bodyStr;
    req.prepare_payload();

    // Resolve
    tcp::resolver resolver(executor);
    auto endpoints = co_await resolver.async_resolve(
        ep.host, std::to_string(ep.port),
        asio::cancel_after(rem(), asio::use_awaitable));

    // Accumulators
    neograph::ChatCompletion completion;
    completion.message.role = "assistant";
    std::string fullContent;
    std::string fullThinking;
    std::map<int, neograph::ToolCall> tcMap;
    std::map<int, std::string> blockTypes;
    std::string lineBuffer;

    // Connect, send, stream SSE
    tcp::socket socket(executor);
    co_await asio::async_connect(
        socket, endpoints, asio::cancel_after(rem(), asio::use_awaitable));
    boost::system::error_code tcpEc;
    socket.set_option(asio::ip::tcp::no_delay(true), tcpEc);

    if (isHttps) {
      auto &sslCtx = sslContext();
      boost::beast::ssl_stream<boost::beast::tcp_stream> stream(
          boost::beast::tcp_stream(std::move(socket)), sslCtx);
      ::SSL_set_tlsext_host_name(stream.native_handle(),
                                 const_cast<char *>(ep.host.c_str()));
      co_await stream.async_handshake(
          asio::ssl::stream_base::client,
          asio::cancel_after(rem(), asio::use_awaitable));

      co_await http::async_write(
          stream, req, asio::cancel_after(rem(), asio::use_awaitable));

      co_await readSseStream(stream, deadline, completion, fullContent,
                             fullThinking, tcMap, blockTypes, lineBuffer,
                             on_chunk);

      boost::system::error_code shutEc;
      co_await stream.async_shutdown(
          asio::redirect_error(asio::use_awaitable, shutEc));
    } else {
      boost::beast::tcp_stream stream(std::move(socket));

      co_await http::async_write(
          stream, req, asio::cancel_after(rem(), asio::use_awaitable));

      co_await readSseStream(stream, deadline, completion, fullContent,
                             fullThinking, tcMap, blockTypes, lineBuffer,
                             on_chunk);
    }

    completion.message.content = fullContent;
    completion.message.reasoning_content = fullThinking;
    for (auto &[idx, tc] : tcMap) {
      completion.message.tool_calls.push_back(std::move(tc));
    }

    co_return completion;
  }

  template <typename Stream>
  asio::awaitable<void>
  readSseStream(Stream &stream, std::chrono::steady_clock::time_point deadline,
                neograph::ChatCompletion &completion, std::string &fullContent,
                std::string &fullThinking,
                std::map<int, neograph::ToolCall> &tcMap,
                std::map<int, std::string> &blockTypes, std::string &lineBuffer,
                neograph::FormatDataStreamCallback on_chunk) {
    namespace http = boost::beast::http;

    auto rem = [&] {
      auto d = deadline - std::chrono::steady_clock::now();
      return std::chrono::duration_cast<std::chrono::milliseconds>(
          std::max(d, std::chrono::steady_clock::duration::zero()));
    };

    boost::beast::flat_buffer buf;
    http::response_parser<http::string_body> parser;
    parser.body_limit(std::numeric_limits<uint64_t>::max());
    parser.eager(true);

    co_await http::async_read_header(stream, buf, parser,
                                     asio::cancel_after(rem(), asio::use_awaitable));

    if (parser.get().result_int() == 429) {
      co_await http::async_read(stream, buf, parser,
                                asio::cancel_after(rem(), asio::use_awaitable));
      auto resp = parser.release();
      auto raw = resp[http::field::retry_after];
      int retryAfter = -1;
      if (!raw.empty()) {
        int seconds = 0;
        auto [ptr, ec] =
            std::from_chars(raw.data(), raw.data() + raw.size(), seconds);
        if (ec == std::errc{} && seconds >= 0) {
          retryAfter = seconds;
        }
      }
      throw neograph::RateLimitError("API error (HTTP 429): " + resp.body(),
                                     retryAfter);
    }

    if (parser.get().result_int() != 200) {
      co_await http::async_read(stream, buf, parser,
                                asio::cancel_after(rem(), asio::use_awaitable));
      auto resp = parser.release();
      throw std::runtime_error("API error (HTTP " +
                               std::to_string(resp.result_int()) +
                               "): " + resp.body());
    }

    size_t processed = 0;
    boost::system::error_code ec;
    while (!parser.is_done()) {
      co_await http::async_read_some(stream, buf, parser,
                                     asio::redirect_error(asio::use_awaitable, ec));
      if (ec) {
        break;
      }
      auto &body = parser.get().body();
      if (body.size() > processed) {
        lineBuffer += body.substr(processed);
        processed = body.size();
        processSseBuffer(lineBuffer, completion, fullContent, fullThinking,
                         tcMap, blockTypes, on_chunk);
      }
    }
    if (!lineBuffer.empty()) {
      processSseBuffer(lineBuffer, completion, fullContent, fullThinking, tcMap,
                       blockTypes, on_chunk);
    }
  }

  static asio::ssl::context &sslContext() {
    return agentxx::util::HttpClient::sharedSslCtx(false);
  }

  Config config_;
};

} // namespace server
} // namespace agentxx
