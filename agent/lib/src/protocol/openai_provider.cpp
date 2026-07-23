#include "agentxx/protocol/openai_provider.h"

namespace agentxx {
namespace server {

std::unique_ptr<OpenAIProvider> OpenAIProvider::create(const agentxx::agent::ModelConfig& config) {
    return std::unique_ptr<OpenAIProvider>(new OpenAIProvider(config));
}

std::shared_ptr<neograph::Provider>
    OpenAIProvider::create_shared(const agentxx::agent::ModelConfig& config) {
    return std::shared_ptr<neograph::Provider>(new OpenAIProvider(config));
}

std::string OpenAIProvider::get_name() const {
    return "openai";
}

asio::awaitable<neograph::ChatCompletion>
    OpenAIProvider::invoke(const neograph::CompletionParams& params,
                           neograph::StreamCallback          on_chunk) {
    if (!on_chunk) {
        co_return co_await invoke_format_data(params, nullptr);
    }
    co_return co_await invoke_format_data(params,
                                          [on_chunk](const neograph::ChatStreamChunk& chunk) {
                                              switch (chunk.type) {
                                              case neograph::ChatStreamChunk::TYPE_CONTENT:
                                                  on_chunk(chunk.data);
                                                  break;
                                              case neograph::ChatStreamChunk::TYPE_THINKING:
                                                  break;
                                              }
                                          });
}

asio::awaitable<neograph::ChatCompletion>
    OpenAIProvider::invoke_format_data(const neograph::CompletionParams&  params,
                                       neograph::FormatDataStreamCallback on_chunk) {
    if (on_chunk) {
        auto body              = buildBody(params);
        body["stream"]         = true;
        body["stream_options"] = {
            {"include_usage", true}
        };
        auto result = co_await doStream(params, body, on_chunk);
        co_return result;
    }
    co_return co_await completeAsync(params);
}

OpenAIProvider::OpenAIProvider(agentxx::agent::ModelConfig config) :
    config_(std::move(config)) {
    if (config_.baseUrl.empty()) {
        config_.baseUrl = kDefaultBaseUrl;
    }
}

neograph::json OpenAIProvider::buildBody(const neograph::CompletionParams& params) const {
    neograph::json body;
    body["model"]    = params.model.empty() ? config_.modelName : params.model;
    body["messages"] = neograph::messages_to_json(params.messages);

    if (!config_.sendThinking) {
        neograph::json src     = body["messages"];
        neograph::json cleaned = neograph::json::array();
        for (auto val : src) {
            neograph::json obj = neograph::json::object();
            for (auto [k, v] : val.items()) {
                if (k != "reasoning_content") {
                    obj[k] = v;
                }
            }
            cleaned.push_back(obj);
        }
        body["messages"] = cleaned;
    }

    if (!params.tools.empty()) {
        body["tools"]       = neograph::tools_to_json(params.tools);
        body["tool_choice"] = "auto";
    }

    if (params.temperature >= 0.0f) {
        body["temperature"] = params.temperature;
    }

    if (config_.extra_config.is_object()) {
        for (const auto& [key, val] : config_.extra_config.items()) {
            if (!body.contains(key)) {
                body[key] = val;
            }
        }
    }

    if (!params.extra_fields.empty()) {
        for (const auto& [key, val] : params.extra_fields.items()) {
            body[key] = val;
        }
    }

    return body;
}

OpenAIProvider::ParsedEndpoint OpenAIProvider::parseEndpoint(const std::string& base_url) {
    ParsedEndpoint ep;
    ep.scheme = "https";
    ep.port   = 443;

    auto schemeEnd = base_url.find("://");
    if (schemeEnd == std::string::npos) {
        ep.host = base_url;
        return ep;
    }
    ep.scheme             = base_url.substr(0, schemeEnd);
    auto        rest      = base_url.substr(schemeEnd + 3);
    auto        pathStart = rest.find('/');
    std::string hostPort  = (pathStart == std::string::npos) ? rest : rest.substr(0, pathStart);
    ep.prefix             = (pathStart == std::string::npos) ? "" : rest.substr(pathStart);

    ep.port = (ep.scheme == "https") ? 443 : 80;

    auto colon = hostPort.rfind(':');
    if (colon != std::string::npos) {
        ep.host = hostPort.substr(0, colon);
        int p   = 0;
        auto [ptr, ec]
            = std::from_chars(hostPort.data() + colon + 1, hostPort.data() + hostPort.size(), p);
        if (ec == std::errc{} && p > 0 && p <= 65535) {
            ep.port = static_cast<uint16_t>(p);
        }
    } else {
        ep.host = hostPort;
    }
    return ep;
}

asio::awaitable<neograph::ChatCompletion>
    OpenAIProvider::completeAsync(const neograph::CompletionParams& params) {
    using namespace agentxx::util;

    auto bodyJson = buildBody(params);
    auto bodyStr  = bodyJson.dump();

    HeaderMap headers;
    headers.set("Authorization", "Bearer " + config_.apiKey);

    auto resp = co_await HttpClient::postAsync(
        config_.baseUrl + "/chat/completions",
        bodyStr,
        "application/json",
        headers,
        HttpClient::RequestConfig{.connectTimeout
                                  = std::chrono::seconds{config_.connectTimeoutSeconds},
                                  .readTimeout = std::chrono::seconds{config_.readTimeoutSeconds}});

    if (!resp.has_value()) {
        throw std::runtime_error("HTTP request failed: " + resp.error());
    }

    auto& r = resp.value();

    if (r.status == 429) {
        auto raw        = r.findHeader("retry-after");
        int  retryAfter = -1;
        if (!raw.empty()) {
            int seconds    = 0;
            auto [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), seconds);
            if (ec == std::errc{} && seconds >= 0) {
                retryAfter = seconds;
            }
        }
        throw neograph::RateLimitError("API error (HTTP 429): " + r.body, retryAfter);
    }

    if (r.status != 200) {
        throw std::runtime_error("API error (HTTP " + std::to_string(r.status) + "): " + r.body);
    }

    auto respJson = neograph::json::parse(r.body);
    auto choice   = respJson.at("choices").at(0);

    neograph::ChatCompletion completion;
    completion.message = neograph::parse_response_message(choice);

    if (completion.message.reasoning_content.empty()) {
        extractThinkTags(completion.message.content, completion.message.reasoning_content);
    }

    if (completion.message.reasoning_content.empty()) {
        if (choice.contains("reasoning_content") && !choice["reasoning_content"].is_null()) {
            completion.message.reasoning_content = choice["reasoning_content"].get<std::string>();
        } else if (choice.contains("thinking") && !choice["thinking"].is_null()) {
            completion.message.reasoning_content = choice["thinking"].get<std::string>();
        }
    }

    if (respJson.contains("usage")) {
        auto u                             = respJson["usage"];
        completion.usage.prompt_tokens     = u.value("prompt_tokens", 0);
        completion.usage.completion_tokens = u.value("completion_tokens", 0);
        completion.usage.total_tokens      = u.value("total_tokens", 0);
    }

    co_return completion;
}

asio::awaitable<neograph::ChatCompletion>
    OpenAIProvider::doStream(const neograph::CompletionParams&  params,
                             const neograph::json&              body,
                             neograph::FormatDataStreamCallback on_chunk) {
    namespace http = boost::beast::http;
    using asio::ip::tcp;

    auto bodyStr  = body.dump();
    auto executor = co_await asio::this_coro::executor;
    auto ep       = parseEndpoint(config_.baseUrl);
    auto target   = ep.prefix + "/chat/completions";
    bool isHttps  = ep.scheme == "https";

    auto connectTimeout = std::chrono::seconds{config_.connectTimeoutSeconds};
    auto readTimeout    = std::chrono::seconds{config_.readTimeoutSeconds};
    auto sendTimeout    = agentxx::util::HttpClient::calcSendTimeout(bodyStr.size());
    auto deadline       = std::chrono::steady_clock::now() + connectTimeout;
    auto rem            = [&] {
        auto d = deadline - std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::max(d, std::chrono::steady_clock::duration::zero()));
    };

    http::request<http::string_body> req{http::verb::post, target, 11};
    req.set(http::field::host, ep.host);
    req.set(http::field::user_agent, "agentxx/1.0");
    req.set(http::field::content_type, "application/json");
    req.set(http::field::authorization, "Bearer " + config_.apiKey);
    req.set(http::field::accept, "text/event-stream");
    req.set(http::field::connection, "keep-alive");
    req.body() = bodyStr;
    req.prepare_payload();

    tcp::resolver resolver(executor);
    auto          endpoints
        = co_await resolver.async_resolve(ep.host,
                                          std::to_string(ep.port),
                                          asio::cancel_after(rem(), asio::use_awaitable));

    neograph::ChatCompletion completion;
    completion.message.role = "assistant";
    std::string                       fullContent;
    std::string                       fullThinking;
    std::map<int, neograph::ToolCall> tcMap;
    std::string                       lineBuffer;

    tcp::socket socket(executor);
    co_await asio::async_connect(socket, endpoints, asio::cancel_after(rem(), asio::use_awaitable));
    boost::system::error_code tcpEc;
    socket.set_option(asio::ip::tcp::no_delay(true), tcpEc);

    if (isHttps) {
        auto&                                              sslCtx = sslContext();
        boost::beast::ssl_stream<boost::beast::tcp_stream> stream(
            boost::beast::tcp_stream(std::move(socket)),
            sslCtx);
        ::SSL_set_tlsext_host_name(stream.native_handle(), const_cast<char*>(ep.host.c_str()));
        co_await stream.async_handshake(asio::ssl::stream_base::client,
                                        asio::cancel_after(rem(), asio::use_awaitable));

        co_await http::async_write(stream,
                                   req,
                                   asio::cancel_after(sendTimeout, asio::use_awaitable));

        co_await readSseStream(stream,
                               deadline,
                               readTimeout,
                               completion,
                               fullContent,
                               fullThinking,
                               tcMap,
                               lineBuffer,
                               on_chunk);

        boost::system::error_code shutEc;
        co_await stream.async_shutdown(asio::redirect_error(asio::use_awaitable, shutEc));
    } else {
        boost::beast::tcp_stream stream(std::move(socket));

        co_await http::async_write(stream,
                                   req,
                                   asio::cancel_after(sendTimeout, asio::use_awaitable));

        co_await readSseStream(stream,
                               deadline,
                               readTimeout,
                               completion,
                               fullContent,
                               fullThinking,
                               tcMap,
                               lineBuffer,
                               on_chunk);
    }

    if (fullThinking.empty()) {
        extractThinkTags(fullContent, fullThinking);
    }
    completion.message.content           = fullContent;
    completion.message.reasoning_content = fullThinking;
    for (auto& [idx, tc] : tcMap) {
        completion.message.tool_calls.push_back(std::move(tc));
    }

    co_return completion;
}

void OpenAIProvider::extractThinkTags(std::string& content, std::string& thinking) {
    std::string cleaned;
    size_t      pos = 0;
    while (pos < content.size()) {
        auto start = content.find("<think>", pos);
        if (start == std::string::npos) {
            cleaned += content.substr(pos);
            break;
        }
        cleaned  += content.substr(pos, start - pos);
        auto end  = content.find("</think>", start + 7);
        if (end == std::string::npos) {
            thinking += content.substr(start + 7);
            content.erase(start);
            return;
        }
        thinking += content.substr(start + 7, end - start - 7);
        pos       = end + 8;
    }
    content = cleaned;
}

asio::ssl::context& OpenAIProvider::sslContext() {
    return agentxx::util::HttpClient::sharedSslCtx(false);
}

} // namespace server
} // namespace agentxx
