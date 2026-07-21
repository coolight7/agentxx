#pragma once

#include "agentxx/util/http_client.h"
#include "agentxx/util/log.h"
#include "asio/awaitable.hpp"
#include "asio/cancel_after.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include <atomic>
#include <boost/asio/read.hpp>
#include <charconv>
#include <chrono>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <neograph/api.h>
#include <neograph/provider.h>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace server {

class OpenAIProvider : public neograph::Provider {
public:
  struct Config {
    std::string api_key;
    std::string base_url = "https://api.openai.com";
    std::string default_model = "gpt-4o-mini";
    int timeout_seconds = 60;

    /// Extra JSON fields merged into every request body.
    /// Use this to set top_k, top_p, frequency_penalty, presence_penalty,
    /// stop, seed, response_format, reasoning_effort, or any other
    /// provider-specific parameters.
    neograph::json extra_body = neograph::json::object();
  };

  static std::unique_ptr<OpenAIProvider> create(const Config &config) {
    return std::unique_ptr<OpenAIProvider>(new OpenAIProvider(config));
  }

  static std::shared_ptr<neograph::Provider>
  create_shared(const Config &config) {
    return std::shared_ptr<neograph::Provider>(new OpenAIProvider(config));
  }

  ~OpenAIProvider() override = default;

  std::string get_name() const override { return "openai"; }

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
      body["stream_options"] = {{"include_usage", true}};
      auto result = co_await doStream(params, body, on_chunk);
      co_return result;
    }
    co_return co_await completeAsync(params);
  }

private:
  explicit OpenAIProvider(Config config) : config_(std::move(config)) {}

  neograph::json buildBody(const neograph::CompletionParams &params) const {
    neograph::json body;
    body["model"] = params.model.empty() ? config_.default_model : params.model;
    body["messages"] = neograph::messages_to_json(params.messages);

    if (!params.tools.empty()) {
      body["tools"] = neograph::tools_to_json(params.tools);
      body["tool_choice"] = "auto";
    }

    if (params.temperature >= 0.0f) {
      body["temperature"] = params.temperature;
    }

    if (params.max_tokens > 0) {
      body["max_completion_tokens"] = params.max_tokens;
    }

    // Merge config-level extra body (lower priority)
    if (!config_.extra_body.empty()) {
      for (const auto &[key, val] : config_.extra_body.items()) {
        // Skip keys already set explicitly above
        if (!body.contains(key)) {
          body[key] = val;
        }
      }
    }

    // Apply per-call extra fields (highest priority — overrides everything)
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
    headers.set("Authorization", "Bearer " + config_.api_key);

    auto resp = co_await HttpClient::postAsync(
        config_.base_url + "/chat/completions", bodyStr, "application/json",
        headers, std::chrono::seconds{config_.timeout_seconds});

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
    auto choice = respJson.at("choices").at(0);

    neograph::ChatCompletion completion;
    completion.message = neograph::parse_response_message(choice);

    // Fallback: some providers embed thinking in <think> tags inside content
    if (completion.message.reasoning_content.empty()) {
      extractThinkTags(completion.message.content,
                       completion.message.reasoning_content);
    }

    // Fallback: some providers put reasoning at choice level
    if (completion.message.reasoning_content.empty()) {
      if (choice.contains("reasoning_content") &&
          !choice["reasoning_content"].is_null()) {
        completion.message.reasoning_content =
            choice["reasoning_content"].get<std::string>();
      } else if (choice.contains("thinking") && !choice["thinking"].is_null()) {
        completion.message.reasoning_content =
            choice["thinking"].get<std::string>();
      }
    }

    if (respJson.contains("usage")) {
      auto u = respJson["usage"];
      completion.usage.prompt_tokens = u.value("prompt_tokens", 0);
      completion.usage.completion_tokens = u.value("completion_tokens", 0);
      completion.usage.total_tokens = u.value("total_tokens", 0);
    }

    co_return completion;
  }

  asio::awaitable<neograph::ChatCompletion>
  doStream(const neograph::CompletionParams &params, const neograph::json &body,
           neograph::FormatDataStreamCallback on_chunk) {
    namespace http = boost::beast::http;
    using asio::ip::tcp;

    auto bodyStr = body.dump();
    auto executor = co_await asio::this_coro::executor;
    auto ep = parseEndpoint(config_.base_url);
    auto target = ep.prefix + "/chat/completions";
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
    req.set(http::field::authorization, "Bearer " + config_.api_key);
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
                             fullThinking, tcMap, lineBuffer, on_chunk);

      boost::system::error_code shutEc;
      co_await stream.async_shutdown(
          asio::redirect_error(asio::use_awaitable, shutEc));
    } else {
      boost::beast::tcp_stream stream(std::move(socket));

      co_await http::async_write(
          stream, req, asio::cancel_after(rem(), asio::use_awaitable));

      co_await readSseStream(stream, deadline, completion, fullContent,
                             fullThinking, tcMap, lineBuffer, on_chunk);
    }

    // If the provider used <think> tags inside content, extract them
    if (fullThinking.empty()) {
      extractThinkTags(fullContent, fullThinking);
    }
    completion.message.content = fullContent;
    completion.message.reasoning_content = fullThinking;
    for (auto &[idx, tc] : tcMap) {
      completion.message.tool_calls.push_back(std::move(tc));
    }

    co_return completion;
  }

  template <typename Stream>
  asio::awaitable<void> readSseStream(
      Stream &stream, std::chrono::steady_clock::time_point deadline,
      neograph::ChatCompletion &completion, std::string &fullContent,
      std::string &fullThinking, std::map<int, neograph::ToolCall> &tcMap,
      std::string &lineBuffer, neograph::FormatDataStreamCallback on_chunk) {
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
                         tcMap, on_chunk);
      }
    }
    if (!lineBuffer.empty()) {
      processSseBuffer(lineBuffer, completion, fullContent, fullThinking, tcMap,
                       on_chunk);
    }
  }

  static void processSseBuffer(std::string &buf,
                               neograph::ChatCompletion &completion,
                               std::string &fullContent,
                               std::string &fullThinking,
                               std::map<int, neograph::ToolCall> &tcMap,
                               neograph::FormatDataStreamCallback on_chunk) {
    size_t pos;
    while ((pos = buf.find('\n')) != std::string::npos) {
      std::string line = buf.substr(0, pos);
      buf.erase(0, pos + 1);

      // Strip \r
      if (!line.empty() && line.back() == '\r')
        line.pop_back();

      if (line.rfind("data: ", 0) != 0)
        continue;
      std::string payload = line.substr(6);

      if (payload == "[DONE]")
        continue;

      try {
        auto j = neograph::json::parse(payload);

        // Capture usage from final chunk
        if (j.contains("usage") && !j["usage"].is_null()) {
          auto u = j["usage"];
          completion.usage.prompt_tokens = u.value("prompt_tokens", 0);
          completion.usage.completion_tokens = u.value("completion_tokens", 0);
          completion.usage.total_tokens =
              u.value("total_tokens", completion.usage.prompt_tokens +
                                          completion.usage.completion_tokens);
        }

        if (!j.contains("choices") || !j["choices"].is_array() ||
            j["choices"].empty()) {
          continue;
        }
        auto delta = j["choices"][0]["delta"];

        // Content token
        if (delta.contains("content") && !delta["content"].is_null()) {
          std::string token = delta["content"].get<std::string>();
          if (!token.empty()) {
            fullContent += token;
            if (on_chunk)
              on_chunk(neograph::ChatStreamChunk{
                  neograph::ChatStreamChunk::TYPE_CONTENT, token});
          }
        }

        // Reasoning / thinking content (e.g. DeepSeek reasoner, QwQ, etc.)
        if (delta.contains("reasoning_content") &&
            delta["reasoning_content"].is_string()) {
          auto token = delta["reasoning_content"].get<std::string>();
          if (!token.empty()) {
            fullThinking += token;
            if (on_chunk)
              on_chunk(neograph::ChatStreamChunk{
                  neograph::ChatStreamChunk::TYPE_THINKING, token});
          }
        } else if (delta.contains("thinking") &&
                   delta["thinking"].is_string()) {
          auto token = delta["thinking"].get<std::string>();
          if (!token.empty()) {
            fullThinking += token;
            if (on_chunk)
              on_chunk(neograph::ChatStreamChunk{
                  neograph::ChatStreamChunk::TYPE_THINKING, token});
          }
        }

        // Tool calls (streamed incrementally)
        if (delta.contains("tool_calls")) {
          for (const auto &tc : delta["tool_calls"]) {
            int idx = tc.value("index", 0);
            if (tc.contains("id")) {
              tcMap[idx].id = tc["id"].get<std::string>();
            }
            if (tc.contains("function")) {
              if (tc["function"].contains("name"))
                tcMap[idx].name += tc["function"]["name"].get<std::string>();
              if (tc["function"].contains("arguments"))
                tcMap[idx].arguments +=
                    tc["function"]["arguments"].get<std::string>();
            }
          }
        }
      } catch (...) {
        // Skip malformed chunks
      }
    }
  }

  static void extractThinkTags(std::string &content, std::string &thinking) {
    std::string cleaned;
    size_t pos = 0;
    while (pos < content.size()) {
      auto start = content.find("<think>", pos);
      if (start == std::string::npos) {
        cleaned += content.substr(pos);
        break;
      }
      cleaned += content.substr(pos, start - pos);
      auto end = content.find("</think>", start + 7);
      if (end == std::string::npos) {
        thinking += content.substr(start + 7);
        content.erase(start);
        return;
      }
      thinking += content.substr(start + 7, end - start - 7);
      pos = end + 8;
    }
    content = cleaned;
  }

  static asio::ssl::context &sslContext() {
    return agentxx::util::HttpClient::sharedSslCtx(false);
  }

  Config config_;
};

} // namespace server
} // namespace agentxx
