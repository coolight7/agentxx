#include "test_openai_provider.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/protocol/openai_provider.h"
#include "agentxx/util/http_client.h"
#include "agentxx/util/http_server.h"
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <thread>

namespace agentxx {
namespace test {

using namespace agentxx::util;
namespace server = agentxx::server;

namespace {
agentxx::agent::ModelConfig makeOaiCfg(
    const std::string& apiKey,
    const std::string& baseUrl,
    int                connectTO    = 10,
    int                readTO       = 10,
    bool               sendThinking = false
) {
    agentxx::agent::ModelConfig mc;
    mc.name                    = "test";
    mc.apiKey                  = apiKey;
    mc.baseUrl                 = baseUrl;
    mc.connectTimeoutSeconds   = connectTO;
    mc.readChunkTimeoutSeconds = readTO;
    mc.sendThinking            = sendThinking;
    return mc;
}
} // namespace

int g_openai_passed = 0;
int g_openai_failed = 0;

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

void test_factory_and_name() {
    {
        agentxx::agent::ModelConfig mc;
        mc.name   = "test";
        mc.apiKey = "sk-test-key";
        auto p    = server::OpenAIProvider::create(mc);
        XX_TEST_EXPECT_TRUE(p != nullptr);
        XX_TEST_EXPECT_EQ(p->get_name(), "openai");
    }

    {
        agentxx::agent::ModelConfig mc = makeOaiCfg("sk-test", "http://localhost:8080");
        mc.modelName                   = "my-model";
        auto p                         = server::OpenAIProvider::create(mc);
        XX_TEST_EXPECT_TRUE(p != nullptr);
        XX_TEST_EXPECT_EQ(p->get_name(), "openai");
    }

    {
        agentxx::agent::ModelConfig mc;
        mc.name   = "test";
        mc.apiKey = "sk-shared";
        auto p    = server::OpenAIProvider::create_shared(mc);
        XX_TEST_EXPECT_TRUE(p != nullptr);
        XX_TEST_EXPECT_EQ(p->get_name(), "openai");
    }
}

void test_config_defaults() {
    agentxx::agent::ModelConfig mc;
    mc.name         = "test";
    mc.apiKey       = "sk-defaults-test";
    mc.extra_config = neograph::json::parse(R"({"top_p":0.9,"frequency_penalty":0.2,"seed":42})");
    auto p          = server::OpenAIProvider::create(mc);
    XX_TEST_EXPECT_TRUE(p != nullptr);
}

void test_extra_body_with_custom_params() {
    auto extra                 = neograph::json::object();
    extra["top_p"]             = 0.95;
    extra["frequency_penalty"] = 0.5;
    extra["presence_penalty"]  = 0.3;
    extra["seed"]              = 42;
    extra["response_format"]   = {
        {"type", "json_object"}
    };
    extra["stop"] = neograph::json::parse(R"(["\n\n","STOP"])");

    agentxx::agent::ModelConfig mc;
    mc.name         = "test";
    mc.apiKey       = "sk-extra";
    mc.extra_config = std::move(extra);
    auto p          = server::OpenAIProvider::create(mc);
    XX_TEST_EXPECT_TRUE(p != nullptr);
    XX_TEST_EXPECT_EQ(p->get_name(), "openai");
}

// ---------------------------------------------------------------------------
// Unit tests — ModelProviderRegistry::createProvider
// ---------------------------------------------------------------------------

void test_create_provider_openai() {
    agentxx::agent::ModelConfig mc;
    mc.name    = "test";
    mc.apiKey  = "sk-test";
    mc.baseUrl = "http://localhost:8080";
    mc.type    = "openai";
    auto p     = agentxx::agent::ModelProviderRegistry::createProvider(mc);
    XX_TEST_EXPECT_TRUE(p != nullptr);
    XX_TEST_EXPECT_EQ(p->get_name(), "openai");
}

void test_create_provider_anthropic() {
    agentxx::agent::ModelConfig mc;
    mc.name    = "test";
    mc.apiKey  = "sk-ant-test";
    mc.baseUrl = "http://localhost:8080";
    mc.type    = "anthropic";
    auto p     = agentxx::agent::ModelProviderRegistry::createProvider(mc);
    XX_TEST_EXPECT_TRUE(p != nullptr);
    XX_TEST_EXPECT_EQ(p->get_name(), "anthropic");
}

void test_create_provider_default_type() {
    agentxx::agent::ModelConfig mc;
    mc.name    = "test";
    mc.apiKey  = "sk-test";
    mc.baseUrl = "http://localhost:8080";
    // type defaults to "openai"
    auto p = agentxx::agent::ModelProviderRegistry::createProvider(mc);
    XX_TEST_EXPECT_TRUE(p != nullptr);
    XX_TEST_EXPECT_EQ(p->get_name(), "openai");
}

// ---------------------------------------------------------------------------
// Mock server — single /chat/completions handler with mode switching
// ---------------------------------------------------------------------------

/// Controls what the mock returns on the next request.
enum class MockMode {
    Normal,
    ToolCall,
    RateLimit,
    ServerError,
    Streaming,
    StreamingToolCall,
};

class MockOpenAIServer {
public:

    std::unique_ptr<HttpServer> server;
    std::thread                 thread;
    MockMode                    mode = MockMode::Normal;
    std::string                 lastRequestBody;

    // SSE chunks to emit in streaming mode
    std::vector<std::string> sseChunks;

    // Optional override: when non-null, used for the next non-streaming response
    std::optional<neograph::json> customResponse;

    static std::string sseData(std::string_view json) {
        return "data: " + std::string(json) + "\n\n";
    }

    static std::string sseDone() {
        return "data: [DONE]\n\n";
    }

    neograph::json
        makeCompletionResponse(std::string_view content, int prompt = 10, int completion = 5)
            const {
        neograph::json resp;
        resp["id"]                            = "chatcmpl-mock";
        resp["object"]                        = "chat.completion";
        resp["created"]                       = 1700000000;
        resp["model"]                         = "mock-model";
        resp["choices"]                       = neograph::json::array({neograph::json::object()});
        resp["choices"][0]["index"]           = 0;
        resp["choices"][0]["message"]["role"] = "assistant";
        resp["choices"][0]["message"]["content"] = std::string(content);
        resp["choices"][0]["finish_reason"]      = "stop";
        resp["usage"]["prompt_tokens"]           = prompt;
        resp["usage"]["completion_tokens"]       = completion;
        resp["usage"]["total_tokens"]            = prompt + completion;
        return resp;
    }

    neograph::json makeCompletionResponse(
        std::string_view content,
        std::string_view reasoning,
        int              prompt     = 10,
        int              completion = 5
    ) const {
        auto resp = makeCompletionResponse(content, prompt, completion);
        resp["choices"][0]["message"]["reasoning_content"] = std::string(reasoning);
        return resp;
    }

    neograph::json makeToolCallResponse() const {
        auto tcFunc         = neograph::json::object();
        tcFunc["name"]      = "get_weather";
        tcFunc["arguments"] = R"({"location":"Tokyo"})";
        auto tc             = neograph::json::object();
        tc["id"]            = "call_abc123";
        tc["type"]          = "function";
        tc["function"]      = tcFunc;
        auto tcArr          = neograph::json::array();
        tcArr.push_back(tc);
        auto msg                = neograph::json::object();
        msg["role"]             = "assistant";
        msg["content"]          = neograph::json(nullptr);
        msg["tool_calls"]       = tcArr;
        auto choice             = neograph::json::object();
        choice["index"]         = 0;
        choice["finish_reason"] = "tool_calls";
        choice["message"]       = msg;
        auto choices            = neograph::json::array();
        choices.push_back(choice);
        neograph::json resp;
        resp["id"]      = "chatcmpl-tool";
        resp["object"]  = "chat.completion";
        resp["created"] = 1700000001;
        resp["model"]   = "mock-model";
        resp["choices"] = choices;
        return resp;
    }
};

std::unique_ptr<MockOpenAIServer> startMockServer(uint16_t& outPort) {
    auto mock = std::make_unique<MockOpenAIServer>();

    mock->sseChunks.clear();
    mock->sseChunks.push_back(MockOpenAIServer::sseData(
        R"({"choices":[{"index":0,"delta":{"role":"assistant","content":""}}]})"
    ));
    mock->sseChunks.push_back(
        MockOpenAIServer::sseData(R"({"choices":[{"index":0,"delta":{"content":"Hello"}}]})")
    );
    mock->sseChunks.push_back(
        MockOpenAIServer::sseData(R"({"choices":[{"index":0,"delta":{"content":" "}}]})")
    );
    mock->sseChunks.push_back(
        MockOpenAIServer::sseData(R"({"choices":[{"index":0,"delta":{"content":"world"}}]})")
    );
    mock->sseChunks.push_back(MockOpenAIServer::sseData(
        R"({"choices":[{"index":0,"delta":{}}],"usage":{"prompt_tokens":5,"completion_tokens":3,"total_tokens":8}})"
    ));
    mock->sseChunks.push_back(MockOpenAIServer::sseDone());

    mock->server = std::make_unique<HttpServer>(
        HttpServer::Config{.address = "127.0.0.1", .port = 0, .ioThreads = 1}
    );

    // Single handler for /chat/completions that dispatches by mode
    mock->server->router().add(
        "/chat/completions",
        2,
        std::make_shared<HttpServer::Handler>(
            [mock = mock.get(
             )](HttpServer::Request& req, HttpServer::Response& resp, std::string_view
            ) -> asio::awaitable<void> {
                mock->lastRequestBody = req.body();

                switch (mock->mode) {
                    case MockMode::RateLimit:
                        resp.result(boost::beast::http::status::too_many_requests);
                        resp.set(boost::beast::http::field::content_type, "application/json");
                        resp.set(boost::beast::http::field::retry_after, "5");
                        resp.body() = R"({"error":{"message":"Rate limit exceeded"}})";
                        resp.prepare_payload();
                        break;

                    case MockMode::ServerError:
                        resp.result(boost::beast::http::status::internal_server_error);
                        resp.set(boost::beast::http::field::content_type, "application/json");
                        resp.body() = R"({"error":{"message":"Internal server error"}})";
                        resp.prepare_payload();
                        break;

                    case MockMode::ToolCall:
                        resp.result(boost::beast::http::status::ok);
                        resp.set(boost::beast::http::field::content_type, "application/json");
                        resp.body() = mock->makeToolCallResponse().dump();
                        resp.prepare_payload();
                        break;

                    case MockMode::Streaming:
                    case MockMode::StreamingToolCall: {
                        resp.result(boost::beast::http::status::ok);
                        resp.set(boost::beast::http::field::content_type, "text/event-stream");
                        resp.set(boost::beast::http::field::cache_control, "no-cache");
                        resp.keep_alive(false);
                        std::string sseBody;
                        for (const auto& chunk : mock->sseChunks) {
                            sseBody += chunk;
                        }
                        resp.body() = sseBody;
                        resp.prepare_payload();
                        break;
                    }

                    case MockMode::Normal:
                    default:
                        resp.result(boost::beast::http::status::ok);
                        resp.set(boost::beast::http::field::content_type, "application/json");
                        if (mock->customResponse.has_value()) {
                            resp.body() = mock->customResponse->dump();
                            mock->customResponse.reset();
                        } else {
                            resp.body() = mock->makeCompletionResponse("Hello from mock!").dump();
                        }
                        resp.prepare_payload();
                        break;
                }
                co_return;
            }
        )
    );

    // Start server
    mock->thread = std::thread([s = mock->server.get()]() {
        s->start();
    });

    for (int i = 0; i < 100; ++i) {
        outPort = mock->server->port();
        if (outPort != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (outPort == 0) {
        mock->server->stop();
        if (mock->thread.joinable()) {
            mock->thread.join();
        }
        return nullptr;
    }

    for (int i = 0; i < 100; ++i) {
        try {
            asio::io_context      tmpCtx;
            asio::ip::tcp::socket sock(tmpCtx);
            sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), outPort));
            sock.close();
            break;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    return mock;
}

// ---------------------------------------------------------------------------
// Unit tests for reasoning / thinking content parsing
// ---------------------------------------------------------------------------

void test_parse_response_message_with_reasoning() {
    // Simulate an OpenAI non-streaming response choice with reasoning_content
    auto choice = neograph::json::parse(
        R"({
        "index": 0,
        "message": {
          "role": "assistant",
          "content": "Final answer",
          "reasoning_content": "Step-by-step reasoning..."
        },
        "finish_reason": "stop"
      })"
    );

    auto msg = neograph::parse_response_message(choice);
    XX_TEST_EXPECT_EQ(msg.role, "assistant");
    XX_TEST_EXPECT_EQ(msg.content, "Final answer");
    XX_TEST_EXPECT_EQ(msg.reasoning_content, "Step-by-step reasoning...");
}

void test_parse_response_message_with_thinking_field() {
    // Some providers (e.g. Anthropic-style) use "thinking" instead
    auto choice = neograph::json::parse(
        R"({
        "index": 0,
        "message": {
          "role": "assistant",
          "content": "I think therefore I am",
          "thinking": " cogito ergo sum"
        },
        "finish_reason": "stop"
      })"
    );

    auto msg = neograph::parse_response_message(choice);
    XX_TEST_EXPECT_EQ(msg.role, "assistant");
    XX_TEST_EXPECT_EQ(msg.content, "I think therefore I am");
    XX_TEST_EXPECT_EQ(msg.reasoning_content, " cogito ergo sum");
}

void test_parse_response_message_without_reasoning() {
    auto choice = neograph::json::parse(
        R"({
        "index": 0,
        "message": {
          "role": "assistant",
          "content": "Just answer"
        },
        "finish_reason": "stop"
      })"
    );

    auto msg = neograph::parse_response_message(choice);
    XX_TEST_EXPECT_EQ(msg.reasoning_content, "");
    XX_TEST_EXPECT_EQ(msg.content, "Just answer");
}

void test_parse_response_message_null_reasoning() {
    // Some providers return "reasoning_content": null
    auto choice = neograph::json::parse(
        R"({
        "index": 0,
        "message": {
          "role": "assistant",
          "content": "Answer",
          "reasoning_content": null
        },
        "finish_reason": "stop"
      })"
    );

    auto msg = neograph::parse_response_message(choice);
    XX_TEST_EXPECT_EQ(msg.reasoning_content, "");
    XX_TEST_EXPECT_EQ(msg.content, "Answer");
}

void test_parse_response_message_reasoning_preferred_over_thinking() {
    // When both are present, reasoning_content should win
    auto choice = neograph::json::parse(
        R"({
        "index": 0,
        "message": {
          "role": "assistant",
          "content": "Both",
          "reasoning_content": "primary reasoning",
          "thinking": "secondary thinking"
        },
        "finish_reason": "stop"
      })"
    );

    auto msg = neograph::parse_response_message(choice);
    XX_TEST_EXPECT_EQ(msg.reasoning_content, "primary reasoning");
}

void test_messages_to_json_with_reasoning() {
    neograph::ChatMessage msg;
    msg.role              = "assistant";
    msg.content           = "Visible answer";
    msg.reasoning_content = "Hidden reasoning";

    auto arr = neograph::messages_to_json({msg});
    XX_TEST_EXPECT_EQ(arr.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(arr[0]["role"], "assistant");
    XX_TEST_EXPECT_EQ(arr[0]["content"], "Visible answer");
    XX_TEST_EXPECT_TRUE(arr[0].contains("reasoning_content"));
    XX_TEST_EXPECT_EQ(arr[0]["reasoning_content"], "Hidden reasoning");
}

void test_messages_to_json_without_reasoning() {
    neograph::ChatMessage msg;
    msg.role    = "assistant";
    msg.content = "Just answer";

    auto arr = neograph::messages_to_json({msg});
    XX_TEST_EXPECT_FALSE(arr[0].contains("reasoning_content"));
}

void test_messages_to_json_reasoning_roundtrip() {
    // Serialize → parse back should preserve reasoning
    neograph::ChatMessage original;
    original.role              = "assistant";
    original.content           = "Roundtrip test";
    original.reasoning_content = "Deep thinking here";

    auto arr     = neograph::messages_to_json({original});
    auto jsonStr = arr[0].dump();

    // Now simulate what a provider would receive and how it would be parsed
    auto choice = neograph::json::parse(
        R"({"index":0,"message":)" + jsonStr + R"(,"finish_reason":"stop"})"
    );
    auto parsed = neograph::parse_response_message(choice);
    XX_TEST_EXPECT_EQ(parsed.content, original.content);
    XX_TEST_EXPECT_EQ(parsed.reasoning_content, original.reasoning_content);
}

// ---------------------------------------------------------------------------
// Unit tests for <think> tag extraction
// ---------------------------------------------------------------------------

void test_extract_think_tags_basic() {
    std::string content = "<think>Let me reason</think>The answer is 42.";
    std::string thinking;
    // Use the same logic as in OpenAIProvider
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
            break;
        }
        thinking += content.substr(start + 7, end - start - 7);
        pos       = end + 8;
    }
    XX_TEST_EXPECT_EQ(cleaned, "The answer is 42.");
    XX_TEST_EXPECT_EQ(thinking, "Let me reason");
}

void test_extract_think_tags_no_tags() {
    std::string content = "Plain content without think tags.";
    std::string thinking;
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
            break;
        }
        thinking += content.substr(start + 7, end - start - 7);
        pos       = end + 8;
    }
    XX_TEST_EXPECT_EQ(cleaned, "Plain content without think tags.");
    XX_TEST_EXPECT_TRUE(thinking.empty());
}

void test_extract_think_tags_unclosed() {
    std::string content = "Start<think>Unclosed thinking";
    std::string thinking;
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
            break;
        }
        thinking += content.substr(start + 7, end - start - 7);
        pos       = end + 8;
    }
    XX_TEST_EXPECT_EQ(cleaned, "Start");
    XX_TEST_EXPECT_EQ(thinking, "Unclosed thinking");
}

void test_extract_think_tags_multiple_blocks() {
    std::string content = "<think>First</think>Middle<think>Second</think>End";
    std::string thinking;
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
            break;
        }
        thinking += content.substr(start + 7, end - start - 7);
        pos       = end + 8;
    }
    XX_TEST_EXPECT_EQ(cleaned, "MiddleEnd");
    XX_TEST_EXPECT_EQ(thinking, "FirstSecond");
}

void test_extract_think_tags_empty_block() {
    std::string content = "<think></think>Just answer.";
    std::string thinking;
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
            break;
        }
        thinking += content.substr(start + 7, end - start - 7);
        pos       = end + 8;
    }
    XX_TEST_EXPECT_EQ(cleaned, "Just answer.");
    XX_TEST_EXPECT_TRUE(thinking.empty());
}

void test_extract_think_tags_only_think() {
    std::string content = "<think>Only thinking, no visible content</think>";
    std::string thinking;
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
            break;
        }
        thinking += content.substr(start + 7, end - start - 7);
        pos       = end + 8;
    }
    XX_TEST_EXPECT_TRUE(cleaned.empty());
    XX_TEST_EXPECT_EQ(thinking, "Only thinking, no visible content");
}

/// 闭合 think 标签之后又出现未闭合标签: 已提取的闭合标签不应残留在 content 中
void test_extract_think_tags_closed_then_unclosed() {
    std::string content  = "A<think>B</think>C<think>D";
    std::string thinking;
    server::OpenAIProvider::extractThinkTags(content, thinking);
    XX_TEST_EXPECT_EQ(thinking, "BD");
    XX_TEST_EXPECT_EQ(content, "AC");
}

/// 直接调用实现函数验证单个未闭合标签场景
void test_extract_think_tags_unclosed_direct() {
    std::string content  = "Start<think>Unclosed thinking";
    std::string thinking;
    server::OpenAIProvider::extractThinkTags(content, thinking);
    XX_TEST_EXPECT_EQ(thinking, "Unclosed thinking");
    XX_TEST_EXPECT_EQ(content, "Start");
}

// ---------------------------------------------------------------------------
// Integration tests
// ---------------------------------------------------------------------------

asio::awaitable<void> test_non_streaming_completion(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Normal;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-4o-mini";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Say hello"}
    };

    try {
        auto result = co_await provider->invoke(params, nullptr);
        XX_TEST_EXPECT_EQ(result.message.role, "assistant");
        XX_TEST_EXPECT_TRUE(result.message.content.find("Hello") != std::string::npos);
        XX_TEST_EXPECT_TRUE(result.usage.total_tokens > 0);

        // Verify the request body contains expected fields
        auto sent = neograph::json::parse(mock.lastRequestBody);
        XX_TEST_EXPECT_EQ(sent["model"].get<std::string>(), "gpt-4o-mini");
        XX_TEST_EXPECT_TRUE(sent.contains("messages"));
        XX_TEST_EXPECT_EQ(sent["messages"][0]["role"].get<std::string>(), "user");
        XX_TEST_EXPECT_EQ(sent["messages"][0]["content"].get<std::string>(), "Say hello");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "non-streaming completion failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_non_streaming_tool_call(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::ToolCall;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-4o";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "What is the weather?"}
    };
    params.tools = {
        neograph::ChatTool{
                           .name        = "get_weather",
                           .description = "Get weather for a location",
                           .parameters  = neograph::json::parse(
                R"({"type":"object","properties":{"location":{"type":"string"}}})"
            )
        }
    };

    try {
        auto result = co_await provider->invoke(params, nullptr);
        XX_TEST_EXPECT_EQ(result.message.role, "assistant");
        XX_TEST_EXPECT_FALSE(result.message.tool_calls.empty());
        if (!result.message.tool_calls.empty()) {
            XX_TEST_EXPECT_EQ(result.message.tool_calls[0].name, "get_weather");
            XX_TEST_EXPECT_TRUE(
                result.message.tool_calls[0].arguments.find("Tokyo") != std::string::npos
            );
        }
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "tool call completion failed: " << e.what() << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Test: LLM API 响应 toolcall 时没有 toolcall_id
// 某些 LLM 提供商返回的 tool_calls 中可能缺少 id 字段，
// 需要验证解析行为：非流式路径 id 为空字符串；流式路径有 fallback 生成 "call_N"。
// ---------------------------------------------------------------------------

asio::awaitable<void>
    test_non_streaming_tool_call_missing_id(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Normal;

    // 构造一个没有 "id" 字段的 tool_call 响应
    auto tcFunc         = neograph::json::object();
    tcFunc["name"]      = "get_weather";
    tcFunc["arguments"] = R"({"location":"Beijing"})";
    auto tc             = neograph::json::object();
    // 注意: 不设置 tc["id"]
    tc["type"]     = "function";
    tc["function"] = tcFunc;
    auto tcArr     = neograph::json::array();
    tcArr.push_back(tc);
    auto msg                = neograph::json::object();
    msg["role"]             = "assistant";
    msg["content"]          = neograph::json(nullptr);
    msg["tool_calls"]       = tcArr;
    auto choice             = neograph::json::object();
    choice["index"]         = 0;
    choice["finish_reason"] = "tool_calls";
    choice["message"]       = msg;
    auto choices            = neograph::json::array();
    choices.push_back(choice);
    neograph::json resp;
    resp["id"]      = "chatcmpl-no-id";
    resp["object"]  = "chat.completion";
    resp["created"] = 1700000002;
    resp["model"]   = "mock-model";
    resp["choices"] = choices;

    mock.customResponse = resp;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-4o";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "What is the weather?"}
    };
    params.tools = {
        neograph::ChatTool{
                           .name        = "get_weather",
                           .description = "Get weather for a location",
                           .parameters  = neograph::json::parse(
                R"({"type":"object","properties":{"location":{"type":"string"}}})"
            )
        }
    };

    try {
        auto result = co_await provider->invoke(params, nullptr);
        XX_TEST_EXPECT_EQ(result.message.role, "assistant");
        XX_TEST_EXPECT_FALSE(result.message.tool_calls.empty());
        if (!result.message.tool_calls.empty()) {
            XX_TEST_EXPECT_EQ(result.message.tool_calls[0].name, "get_weather");
            XX_TEST_EXPECT_TRUE(
                result.message.tool_calls[0].arguments.find("Beijing") != std::string::npos
            );
            // 非流式路径: parse_response_message 使用 tc.value("id", "")
            // 缺少 id 时应为空字符串
            XX_TEST_EXPECT_TRUE(result.message.tool_calls[0].id.empty());
        }
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "non-streaming tool call missing id failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_streaming_tool_call_missing_id(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::StreamingToolCall;

    // 构造流式 tool_call SSE chunks，不包含 id 字段
    mock.sseChunks.clear();
    // 第一个 chunk: role + tool_call 开始 (无 id)
    mock.sseChunks.push_back(MockOpenAIServer::sseData(
        R"({"choices":[{"index":0,"delta":{"role":"assistant","content":null,"tool_calls":[{"index":0,"function":{"name":"get_weather","arguments":""}}]}}]})"
    ));
    // 第二个 chunk: arguments 增量 (无 id)
    mock.sseChunks.push_back(MockOpenAIServer::sseData(
        R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"location\":"}}]}}]})"
    ));
    // 第三个 chunk: arguments 增量 (无 id)
    mock.sseChunks.push_back(MockOpenAIServer::sseData(
        R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"Shanghai\"}"}}]}}]})"
    ));
    // usage + done
    mock.sseChunks.push_back(MockOpenAIServer::sseData(
        R"({"choices":[{"index":0,"delta":{}}],"usage":{"prompt_tokens":8,"completion_tokens":4,"total_tokens":12}})"
    ));
    mock.sseChunks.push_back(MockOpenAIServer::sseDone());

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-4o";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "What is the weather?"}
    };
    params.tools = {
        neograph::ChatTool{
                           .name        = "get_weather",
                           .description = "Get weather for a location",
                           .parameters  = neograph::json::parse(
                R"({"type":"object","properties":{"location":{"type":"string"}}})"
            )
        }
    };

    try {
        std::string              accumulated;
        neograph::StreamCallback onChunk = [&](const std::string& chunk) {
            accumulated += chunk;
        };
        auto result = co_await provider->invoke(params, onChunk);
        XX_TEST_EXPECT_EQ(result.message.role, "assistant");
        XX_TEST_EXPECT_FALSE(result.message.tool_calls.empty());
        if (!result.message.tool_calls.empty()) {
            XX_TEST_EXPECT_EQ(result.message.tool_calls[0].name, "get_weather");
            XX_TEST_EXPECT_TRUE(
                result.message.tool_calls[0].arguments.find("Shanghai") != std::string::npos
            );
            // 流式路径: doStream 结束后有 fallback:
            // if (tc.id.empty()) tc.id = "call_" + std::to_string(idx);
            // 缺少 id 时应生成 "call_0"
            XX_TEST_EXPECT_EQ(result.message.tool_calls[0].id, "call_0");
        }
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "streaming tool call missing id failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_rate_limit_error(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::RateLimit;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-4o-mini";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "hello"}
    };

    bool caught     = false;
    int  retryAfter = -2;
    try {
        co_await provider->invoke(params, nullptr);
    } catch (const neograph::RateLimitError& e) {
        caught     = true;
        retryAfter = e.retry_after_seconds();
    } catch (const std::exception& e) {
        TEST_INFO << "rate limit test caught generic error: " << e.what() << std::endl;
    }

    if (caught) {
        XX_TEST_PASSED++;
        XX_TEST_EXPECT_EQ(retryAfter, 5);
    } else {
        XX_TEST_FAILED++;
        TEST_FAIL << "expected RateLimitError but was not thrown" << std::endl;
    }
}

asio::awaitable<void> test_server_error(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::ServerError;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-4o-mini";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "trigger error"}
    };

    bool caught = false;
    try {
        co_await provider->invoke(params, nullptr);
    } catch (const std::runtime_error& e) {
        caught = true;
        XX_TEST_EXPECT_TRUE(std::string(e.what()).find("500") != std::string::npos);
    } catch (...) {
    }

    if (caught) {
        XX_TEST_PASSED++;
    } else {
        XX_TEST_FAILED++;
        TEST_FAIL << "expected runtime_error for 500 but was not thrown" << std::endl;
    }
}

asio::awaitable<void> test_extra_body_passthrough(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Normal;

    auto extra     = neograph::json::object();
    extra["top_p"] = 0.9;
    extra["seed"]  = 12345;

    agentxx::agent::ModelConfig mc = makeOaiCfg("sk-test", baseUrl);
    mc.extra_config                = std::move(extra);
    auto provider                  = server::OpenAIProvider::create(mc);

    neograph::CompletionParams params;
    params.model    = "gpt-4o-mini";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "test extra"}
    };

    try {
        auto result = co_await provider->invoke(params, nullptr);
        XX_TEST_EXPECT_EQ(result.message.role, "assistant");

        // Verify request body includes extra_body fields
        auto sent = neograph::json::parse(mock.lastRequestBody);
        XX_TEST_EXPECT_TRUE(sent.contains("top_p"));
        XX_TEST_EXPECT_EQ(sent["top_p"].get<double>(), 0.9);
        XX_TEST_EXPECT_TRUE(sent.contains("seed"));
        XX_TEST_EXPECT_EQ(sent["seed"].get<int>(), 12345);
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "extra_body test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_per_call_extra_fields(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Normal;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-4o-mini";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "test per-call"}
    };
    params.extra_fields["max_completion_tokens"] = 500;
    params.extra_fields["temperature"]           = 0.2;

    try {
        co_await provider->invoke(params, nullptr);
        auto sent = neograph::json::parse(mock.lastRequestBody);
        // per-call extra_fields should override the provider defaults
        XX_TEST_EXPECT_TRUE(sent.contains("temperature"));
        XX_TEST_EXPECT_EQ(sent["temperature"].get<double>(), 0.2);
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "per-call extra_fields test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_streaming_completion(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Streaming;
    mock.sseChunks.clear();
    mock.sseChunks.push_back(MockOpenAIServer::sseData(
        R"({"choices":[{"index":0,"delta":{"role":"assistant","content":""}}]})"
    ));
    mock.sseChunks.push_back(
        MockOpenAIServer::sseData(R"({"choices":[{"index":0,"delta":{"content":"Hello"}}]})")
    );
    mock.sseChunks.push_back(
        MockOpenAIServer::sseData(R"({"choices":[{"index":0,"delta":{"content":" "}}]})")
    );
    mock.sseChunks.push_back(
        MockOpenAIServer::sseData(R"({"choices":[{"index":0,"delta":{"content":"world"}}]})")
    );
    mock.sseChunks.push_back(MockOpenAIServer::sseData(
        R"({"choices":[{"index":0,"delta":{}}],"usage":{"prompt_tokens":5,"completion_tokens":3,"total_tokens":8}})"
    ));
    mock.sseChunks.push_back(MockOpenAIServer::sseDone());

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-4o-mini";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Stream hello"}
    };

    std::string              accumulated;
    neograph::StreamCallback onChunk = [&](const std::string& chunk) {
        accumulated += chunk;
    };

    try {
        auto result = co_await provider->invoke(params, onChunk);
        XX_TEST_EXPECT_EQ(result.message.role, "assistant");
        XX_TEST_EXPECT_TRUE(
            result.message.content.find("Hello") != std::string::npos
            || result.message.content.find("world") != std::string::npos
        );

        // onChunk should have been called incrementally
        XX_TEST_EXPECT_FALSE(accumulated.empty());
        // Verify usage was captured from the final stream chunk
        XX_TEST_EXPECT_TRUE(result.usage.total_tokens > 0);
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "streaming completion failed: " << e.what() << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Integration tests — reasoning / thinking content
// ---------------------------------------------------------------------------

asio::awaitable<void> test_non_streaming_reasoning_content(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Normal;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "deepseek-reasoner";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Think step by step"}
    };

    mock.customResponse = mock.makeCompletionResponse("42", "Let me calculate... 6*7 = 42");

    try {
        auto result = co_await provider->invoke(params, nullptr);
        XX_TEST_EXPECT_EQ(result.message.role, "assistant");
        XX_TEST_EXPECT_EQ(result.message.content, "42");
        XX_TEST_EXPECT_TRUE(!result.message.reasoning_content.empty());
        XX_TEST_EXPECT_TRUE(
            result.message.reasoning_content.find("calculate") != std::string::npos
        );
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "non-streaming reasoning test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_non_streaming_thinking_field(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Normal;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "compat-model";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Think quietly"}
    };

    // Some providers put "thinking" at the message level
    auto resp                                 = mock.makeCompletionResponse("Answer");
    resp["choices"][0]["message"]["thinking"] = "quiet reasoning trace";
    mock.customResponse                       = resp;

    try {
        auto result = co_await provider->invoke(params, nullptr);
        XX_TEST_EXPECT_EQ(result.message.content, "Answer");
        XX_TEST_EXPECT_TRUE(!result.message.reasoning_content.empty());
        XX_TEST_EXPECT_TRUE(result.message.reasoning_content.find("quiet") != std::string::npos);
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "non-streaming thinking field test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void>
    test_non_streaming_reasoning_at_choice_level(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Normal;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "nonstandard-model";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Test choice-level"}
    };

    // Some providers put reasoning at the choice level, not inside message
    auto resp                               = mock.makeCompletionResponse("Answer text");
    resp["choices"][0]["reasoning_content"] = "choice-level reasoning";
    mock.customResponse                     = resp;

    try {
        auto result = co_await provider->invoke(params, nullptr);
        XX_TEST_EXPECT_EQ(result.message.content, "Answer text");
        XX_TEST_EXPECT_TRUE(!result.message.reasoning_content.empty());
        XX_TEST_EXPECT_TRUE(
            result.message.reasoning_content.find("choice-level") != std::string::npos
        );
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "choice-level reasoning test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void>
    test_non_streaming_thinking_at_choice_level(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Normal;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "nonstandard-model";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Test choice-level"}
    };

    auto resp                      = mock.makeCompletionResponse("Answer text");
    resp["choices"][0]["thinking"] = "choice-level thinking";
    mock.customResponse            = resp;

    try {
        auto result = co_await provider->invoke(params, nullptr);
        XX_TEST_EXPECT_EQ(result.message.content, "Answer text");
        XX_TEST_EXPECT_TRUE(!result.message.reasoning_content.empty());
        XX_TEST_EXPECT_TRUE(
            result.message.reasoning_content.find("choice-level") != std::string::npos
        );
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "choice-level thinking test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_streaming_reasoning_content(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Streaming;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "deepseek-r1";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Reason about life"}
    };

    mock.sseChunks = {
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{"role":"assistant","content":""}}]})"
        ),
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{"reasoning_content":"Let me think"}}]})"
        ),
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{"reasoning_content":" step by step"}}]})"
        ),
        MockOpenAIServer::sseData(R"({"choices":[{"index":0,"delta":{"content":"The answer"}}]})"),
        MockOpenAIServer::sseData(R"({"choices":[{"index":0,"delta":{"content":" is 42"}}]})"),
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{}}],"usage":{"prompt_tokens":8,"completion_tokens":6,"total_tokens":14}})"
        ),
        MockOpenAIServer::sseDone()
    };

    std::string              accumulated;
    neograph::StreamCallback onChunk = [&](const std::string& chunk) {
        accumulated += chunk;
    };

    try {
        auto result = co_await provider->invoke(params, onChunk);
        XX_TEST_EXPECT_EQ(result.message.content, "The answer is 42");
        XX_TEST_EXPECT_EQ(result.message.reasoning_content, "Let me think step by step");
        // StreamCallback only receives content tokens; thinking is in reasoning_content
        XX_TEST_EXPECT_EQ(accumulated, "The answer is 42");
        XX_TEST_EXPECT_TRUE(result.usage.total_tokens > 0);
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "streaming reasoning test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_streaming_thinking_field_compat(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Streaming;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "compat-model";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Use thinking field"}
    };

    // Some providers use "thinking" in delta instead of "reasoning_content"
    mock.sseChunks = {
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{"role":"assistant","content":""}}]})"
        ),
        MockOpenAIServer::sseData(R"({"choices":[{"index":0,"delta":{"thinking":"deep thought"}}]})"
        ),
        MockOpenAIServer::sseData(R"({"choices":[{"index":0,"delta":{"thinking":" process..."}}]})"
        ),
        MockOpenAIServer::sseData(R"({"choices":[{"index":0,"delta":{"content":"Final answer"}}]})"
        ),
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{}}],"usage":{"prompt_tokens":5,"completion_tokens":4,"total_tokens":9}})"
        ),
        MockOpenAIServer::sseDone()
    };

    std::string              accumulated;
    neograph::StreamCallback onChunk = [&](const std::string& chunk) {
        accumulated += chunk;
    };

    try {
        auto result = co_await provider->invoke(params, onChunk);
        XX_TEST_EXPECT_EQ(result.message.content, "Final answer");
        XX_TEST_EXPECT_EQ(result.message.reasoning_content, "deep thought process...");
        // StreamCallback only receives content tokens; thinking is in reasoning_content
        XX_TEST_EXPECT_EQ(accumulated, "Final answer");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "streaming thinking compat test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void>
    test_streaming_reasoning_with_null_skips(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Streaming;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "deepseek-r1";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Null reasoning"}
    };

    // When "reasoning_content" is explicitly null in a chunk, skip it
    mock.sseChunks = {
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{"role":"assistant","content":""}}]})"
        ),
        MockOpenAIServer::sseData(R"({"choices":[{"index":0,"delta":{"reasoning_content":null}}]})"
        ),
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{"reasoning_content":"real thinking"}}]})"
        ),
        MockOpenAIServer::sseData(R"({"choices":[{"index":0,"delta":{"content":"Done"}}]})"),
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{}}],"usage":{"prompt_tokens":3,"completion_tokens":2,"total_tokens":5}})"
        ),
        MockOpenAIServer::sseDone()
    };

    std::string              accumulated;
    neograph::StreamCallback onChunk = [&](const std::string& chunk) {
        accumulated += chunk;
    };

    try {
        auto result = co_await provider->invoke(params, onChunk);
        XX_TEST_EXPECT_EQ(result.message.content, "Done");
        XX_TEST_EXPECT_EQ(result.message.reasoning_content, "real thinking");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "streaming null reasoning test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void>
    test_streaming_reasoning_preferred_over_thinking(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Streaming;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "compat-model";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Both reasoning and thinking"}
    };

    // When both fields appear in delta, reasoning_content takes priority
    mock.sseChunks = {
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{"role":"assistant","content":""}}]})"
        ),
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{"reasoning_content":"primary","thinking":"secondary"}}]})"
        ),
        MockOpenAIServer::sseData(R"({"choices":[{"index":0,"delta":{"content":"Done"}}]})"),
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{}}],"usage":{"prompt_tokens":3,"completion_tokens":2,"total_tokens":5}})"
        ),
        MockOpenAIServer::sseDone()
    };

    std::string              accumulated;
    neograph::StreamCallback onChunk = [&](const std::string& chunk) {
        accumulated += chunk;
    };

    try {
        auto result = co_await provider->invoke(params, onChunk);
        XX_TEST_EXPECT_EQ(result.message.content, "Done");
        XX_TEST_EXPECT_EQ(result.message.reasoning_content, "primary");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "streaming reasoning preferred test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void>
    test_streaming_reasoning_only_no_content(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Streaming;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "thinker-model";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Only reasoning, no content"}
    };

    // Some models emit reasoning without any content at all
    mock.sseChunks = {
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{"role":"assistant","content":""}}]})"
        ),
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{"reasoning_content":"Just thinking"}}]})"
        ),
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{"reasoning_content":" out loud"}}]})"
        ),
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{}}],"usage":{"prompt_tokens":2,"completion_tokens":3,"total_tokens":5}})"
        ),
        MockOpenAIServer::sseDone()
    };

    std::string              accumulated;
    neograph::StreamCallback onChunk = [&](const std::string& chunk) {
        accumulated += chunk;
    };

    try {
        auto result = co_await provider->invoke(params, onChunk);
        XX_TEST_EXPECT_EQ(result.message.content, "");
        XX_TEST_EXPECT_EQ(result.message.reasoning_content, "Just thinking out loud");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "streaming reasoning-only test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void>
    test_streaming_malformed_chunk_skipped(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Streaming;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "robust-model";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Test malformed chunks"}
    };

    // Malformed chunks between valid ones should be skipped gracefully
    mock.sseChunks = {
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{"role":"assistant","content":""}}]})"
        ),
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{"reasoning_content":"Thinking"}}]})"
        ),
        "data: not-json\n\n",
        MockOpenAIServer::sseData(R"({"choices":[{"index":0,"delta":{"content":"Result"}}]})"),
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{}}],"usage":{"prompt_tokens":2,"completion_tokens":2,"total_tokens":4}})"
        ),
        MockOpenAIServer::sseDone()
    };

    std::string              accumulated;
    neograph::StreamCallback onChunk = [&](const std::string& chunk) {
        accumulated += chunk;
    };

    try {
        auto result = co_await provider->invoke(params, onChunk);
        XX_TEST_EXPECT_EQ(result.message.content, "Result");
        XX_TEST_EXPECT_EQ(result.message.reasoning_content, "Thinking");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "streaming malformed chunk test failed: " << e.what() << std::endl;
    }
}

/// 流未收到 [DONE] 就结束 (HTTP 层完整但 SSE 协议层被截断) 必须报错,
/// 不能把截断的响应静默当作正常结果返回
asio::awaitable<void> test_streaming_missing_done_throws(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Streaming;
    mock.sseChunks      = {
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{"role":"assistant","content":""}}]})"
        ),
        MockOpenAIServer::sseData(R"({"choices":[{"index":0,"delta":{"content":"Partial"}}]})"),
        // 故意不包含 sseDone(): 模拟长输出被中间代理截断
    };

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-4o-mini";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Truncation test"}
    };

    std::string              accumulated;
    neograph::StreamCallback onChunk = [&](const std::string& chunk) {
        accumulated += chunk;
    };

    bool        threw = false;
    std::string errMsg;
    try {
        co_await provider->invoke(params, onChunk);
    } catch (const std::exception& e) {
        threw  = true;
        errMsg = e.what();
    }
    XX_TEST_EXPECT_TRUE(threw);
    XX_TEST_EXPECT_TRUE(errMsg.find("truncated") != std::string::npos);
    // 截断前已送达的增量数据仍应已回调给调用方 (用于 UI 展示)
    XX_TEST_EXPECT_EQ(accumulated, "Partial");
}

// ---------------------------------------------------------------------------
// Integration tests — <think> tag in content field
// ---------------------------------------------------------------------------

asio::awaitable<void>
    test_non_streaming_think_tags_in_content(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Normal;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "deepseek-r1-local";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Reason with think tags"}
    };

    auto resp = mock.makeCompletionResponse("<think>I need to calculate</think>The result is 42.");
    mock.customResponse = resp;

    try {
        auto result = co_await provider->invoke(params, nullptr);
        XX_TEST_EXPECT_EQ(result.message.content, "The result is 42.");
        XX_TEST_EXPECT_TRUE(
            result.message.reasoning_content.find("calculate") != std::string::npos
        );
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "non-streaming think tags test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void>
    test_non_streaming_think_tags_prefer_reasoning_field(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Normal;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "hybrid-model";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Hybrid with both field and tag"}
    };

    // When reasoning_content is already provided, don't parse <think> tags
    auto resp = mock.makeCompletionResponse(
        "<think>tag thinking</think>Visible answer",
        "field reasoning"
    );
    mock.customResponse = resp;

    try {
        auto result = co_await provider->invoke(params, nullptr);
        // reasoning_content from the field should take priority
        XX_TEST_EXPECT_EQ(result.message.reasoning_content, "field reasoning");
        // content should preserve <think> tags since we didn't re-parse
        XX_TEST_EXPECT_TRUE(result.message.content.find("<think>") != std::string::npos);
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "non-streaming think tags priority test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void>
    test_streaming_think_tags_split_across_chunks(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Streaming;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "deepseek-r1-local";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Stream think tags split across chunks"}
    };

    // <think> tag split across chunks
    mock.sseChunks = {
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{"role":"assistant","content":""}}]})"
        ),
        MockOpenAIServer::sseData(R"({"choices":[{"index":0,"delta":{"content":"<think>Think"}}]})"
        ),
        MockOpenAIServer::sseData(R"({"choices":[{"index":0,"delta":{"content":"ing step"}}]})"),
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{"content":" by step</think>"}}]})"
        ),
        MockOpenAIServer::sseData(R"({"choices":[{"index":0,"delta":{"content":"Result."}}]})"),
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{}}],"usage":{"prompt_tokens":5,"completion_tokens":4,"total_tokens":9}})"
        ),
        MockOpenAIServer::sseDone()
    };

    std::string              accumulated;
    neograph::StreamCallback onChunk = [&](const std::string& chunk) {
        accumulated += chunk;
    };

    try {
        auto result = co_await provider->invoke(params, onChunk);
        // <think> tags should be extracted from the assembled content
        XX_TEST_EXPECT_EQ(result.message.content, "Result.");
        XX_TEST_EXPECT_EQ(result.message.reasoning_content, "Thinking step by step");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "streaming think tags split test failed: " << e.what() << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Integration tests — sendThinking config flag
// ---------------------------------------------------------------------------

asio::awaitable<void>
    test_sendthinking_false_strips_reasoning(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Normal;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl, 10, 10, false));

    neograph::CompletionParams params;
    params.model    = "gpt-4o-mini";
    params.messages = {
        neograph::ChatMessage{
                              .role              = "assistant",
                              .content           = "Previous answer",
                              .reasoning_content = "Hidden reasoning trace"
        },
        neograph::ChatMessage{.role = "user", .content = "Follow up"}
    };

    try {
        co_await provider->invoke(params, nullptr);
        auto sent = neograph::json::parse(mock.lastRequestBody);
        XX_TEST_EXPECT_FALSE(sent["messages"][0].contains("reasoning_content"));
        XX_TEST_EXPECT_EQ(sent["messages"][0]["content"].get<std::string>(), "Previous answer");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "sendThinking=false test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void>
    test_sendthinking_true_preserves_reasoning(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Normal;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl, 10, 10, true));

    neograph::CompletionParams params;
    params.model    = "gpt-4o-mini";
    params.messages = {
        neograph::ChatMessage{
                              .role              = "assistant",
                              .content           = "Previous answer",
                              .reasoning_content = "Reasoning to carry forward"
        },
        neograph::ChatMessage{.role = "user", .content = "Follow up"}
    };

    try {
        co_await provider->invoke(params, nullptr);
        auto sent = neograph::json::parse(mock.lastRequestBody);
        XX_TEST_EXPECT_TRUE(sent["messages"][0].contains("reasoning_content"));
        XX_TEST_EXPECT_EQ(
            sent["messages"][0]["reasoning_content"].get<std::string>(),
            "Reasoning to carry forward"
        );
        XX_TEST_EXPECT_EQ(sent["messages"][0]["content"].get<std::string>(), "Previous answer");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "sendThinking=true test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void>
    test_sendthinking_no_reasoning_has_no_effect(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Normal;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl, 10, 10, true));

    neograph::CompletionParams params;
    params.model    = "gpt-4o-mini";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Hello"}
    };

    try {
        co_await provider->invoke(params, nullptr);
        auto sent = neograph::json::parse(mock.lastRequestBody);
        // No reasoning_content at all — sendThinking flag should not inject it
        XX_TEST_EXPECT_FALSE(sent["messages"][0].contains("reasoning_content"));
        XX_TEST_EXPECT_EQ(sent["messages"][0]["content"].get<std::string>(), "Hello");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "sendThinking no-reasoning test failed: " << e.what() << std::endl;
    }
}

// ---------------------------------------------------------------------------
// True streaming verification — server sends chunks with delays
// ---------------------------------------------------------------------------

static std::string chunkFrame(const std::string& data) {
    char hexBuf[16];
    snprintf(hexBuf, sizeof(hexBuf), "%zx\r\n", data.size());
    return std::string(hexBuf) + data + "\r\n";
}

class DelayedStreamServer {
public:

    std::thread               thread;
    uint16_t                  boundPort = 0;
    std::vector<std::string>  chunks;
    std::chrono::milliseconds delay{80};
    std::atomic<bool>         stopped{false};

private:

    asio::io_context                         ioCtx;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor;
    asio::ip::tcp::endpoint                  ep;

public:

    void start() {
        acceptor = std::make_unique<asio::ip::tcp::acceptor>(
            ioCtx,
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)
        );
        ep        = acceptor->local_endpoint();
        boundPort = ep.port();

        thread = std::thread([this]() {
            while (!stopped.load()) {
                neograph_asio_error_code ec;
                asio::ip::tcp::socket    sock(ioCtx);
                acceptor->accept(sock, ec);
                if (ec) {
                    break;
                }
                if (stopped.load()) {
                    break;
                }
                handleConn(sock);
            }
        });
    }

    void stop() {
        stopped.store(true);
        if (acceptor) {
            neograph_asio_error_code ec;
            asio::ip::tcp::socket    dummy(ioCtx);
            dummy.connect(ep, ec);
            acceptor->close(ec);
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

private:

    void handleConn(asio::ip::tcp::socket& sock) {
        namespace http = boost::beast::http;
        neograph_asio_error_code ec;

        boost::beast::flat_buffer        buf;
        http::request<http::string_body> req;
        http::read(sock, buf, req, ec);
        if (ec) {
            return;
        }

        std::string header = "HTTP/1.1 200 OK\r\n"
                             "Content-Type: text/event-stream\r\n"
                             "Cache-Control: no-cache\r\n"
                             "Transfer-Encoding: chunked\r\n"
                             "\r\n";
        asio::write(sock, asio::buffer(header), ec);
        if (ec) {
            return;
        }

        for (const auto& chunk : chunks) {
            auto framed = chunkFrame(chunk);
            asio::write(sock, asio::buffer(framed), ec);
            if (ec) {
                return;
            }
            std::this_thread::sleep_for(delay);
        }

        std::string finalChunk = "0\r\n\r\n";
        asio::write(sock, asio::buffer(finalChunk), ec);
        sock.shutdown(asio::ip::tcp::socket::shutdown_send, ec);
    }
};

void test_true_streaming_incremental(uint16_t) {
    auto srv    = std::make_unique<DelayedStreamServer>();
    srv->chunks = {
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"\"}}]}\n\n",
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"A\"}}]}\n\n",
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"B\"}}]}\n\n",
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"C\"}}]}\n\n",
        "data: {\"choices\":[{\"index\":0,\"delta\":{}}],\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":3,\"total_tokens\":6}}\n\n",
        "data: [DONE]\n\n",
    };
    srv->delay = std::chrono::milliseconds(80);
    srv->start();

    std::string baseUrl  = "http://127.0.0.1:" + std::to_string(srv->boundPort);
    auto        provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-4o-mini";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Stream test"}
    };

    std::vector<std::chrono::steady_clock::time_point> callbackTimes;
    auto                                               startTime = std::chrono::steady_clock::now();

    neograph::StreamCallback onChunk = [&](const std::string&) {
        callbackTimes.push_back(std::chrono::steady_clock::now());
    };

    asio::io_context ctx;
    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
            auto result = co_await provider->invoke(params, onChunk);
            XX_TEST_EXPECT_EQ(result.message.content, "ABC");
        },
        asio::detached
    );
    ctx.run();

    XX_TEST_EXPECT_TRUE(callbackTimes.size() >= 3);

    if (callbackTimes.size() >= 3) {
        auto firstOffset = std::chrono::duration_cast<std::chrono::milliseconds>(
                               callbackTimes.front() - startTime
        )
                               .count();
        auto lastOffset = std::chrono::duration_cast<std::chrono::milliseconds>(
                              callbackTimes.back() - startTime
        )
                              .count();
        auto spread = lastOffset - firstOffset;

        XX_TEST_EXPECT_TRUE(spread >= 100);
        if (spread < 100) {
            TEST_FAIL << "streaming not incremental: spread=" << spread
                      << "ms, callbacks=" << callbackTimes.size() << std::endl;
        }
    }

    srv->stop();
}

// ---------------------------------------------------------------------------
// Timeout tests — connect, send, read
// ---------------------------------------------------------------------------

class StallServer {
public:

    enum class Mode {
        NeverReadBody,
        PartialThenStall,
        AbortAfterChunks,  // 发完 partialChunks 后直接断开 (不发 chunked 终止块)
    };

    std::thread              thread;
    uint16_t                 boundPort = 0;
    Mode                     mode      = Mode::NeverReadBody;
    std::vector<std::string> partialChunks;
    std::atomic<bool>        stopped{false};

private:

    asio::io_context                         ioCtx;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor;
    asio::ip::tcp::endpoint                  ep;

public:

    void start() {
        acceptor = std::make_unique<asio::ip::tcp::acceptor>(
            ioCtx,
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)
        );
        ep        = acceptor->local_endpoint();
        boundPort = ep.port();

        thread = std::thread([this]() {
            while (!stopped.load()) {
                neograph_asio_error_code ec;
                asio::ip::tcp::socket    sock(ioCtx);
                acceptor->accept(sock, ec);
                if (ec) {
                    break;
                }
                if (stopped.load()) {
                    break;
                }
                handleConn(sock);
            }
        });
    }

    void stop() {
        stopped.store(true);
        if (acceptor) {
            neograph_asio_error_code ec;
            asio::ip::tcp::socket    dummy(ioCtx);
            dummy.connect(ep, ec);
            acceptor->close(ec);
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

private:

    void handleConn(asio::ip::tcp::socket& sock) {
        if (mode == Mode::NeverReadBody) {
            for (int i = 0; i < 1200 && !stopped.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return;
        }

        namespace http = boost::beast::http;
        neograph_asio_error_code ec;

        boost::beast::flat_buffer        buf;
        http::request<http::string_body> req;
        http::read(sock, buf, req, ec);
        if (ec) {
            return;
        }

        std::string header = "HTTP/1.1 200 OK\r\n"
                             "Content-Type: text/event-stream\r\n"
                             "Transfer-Encoding: chunked\r\n"
                             "\r\n";
        asio::write(sock, asio::buffer(header), ec);
        if (ec) {
            return;
        }

        for (const auto& chunk : partialChunks) {
            auto framed = chunkFrame(chunk);
            asio::write(sock, asio::buffer(framed), ec);
            if (ec) {
                return;
            }
        }

        if (mode == Mode::AbortAfterChunks) {
            // 不发 chunked 终止块直接关闭 (FIN), 模拟长输出末尾连接被对端中断
            return;
        }

        for (int i = 0; i < 1200 && !stopped.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
};

void test_connect_timeout() {
    auto provider
        = server::OpenAIProvider::create(makeOaiCfg("sk-test", "http://192.0.2.1:12345", 2, 2));

    neograph::CompletionParams params;
    params.model    = "gpt-4o-mini";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "hello"}
    };

    auto             start  = std::chrono::steady_clock::now();
    bool             caught = false;
    asio::io_context ctx;
    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
            try {
                co_await provider->invoke(params, nullptr);
            } catch (const std::exception&) {
                caught = true;
            }
        },
        asio::detached
    );
    ctx.run();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start
    )
                       .count();

    XX_TEST_EXPECT_TRUE(caught);
    XX_TEST_EXPECT_TRUE(elapsed < 5000);
}

void test_read_timeout_streaming() {
    auto srv           = std::make_unique<StallServer>();
    srv->mode          = StallServer::Mode::PartialThenStall;
    srv->partialChunks = {
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"\"}}]}\n\n",
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hi\"}}]}\n\n",
    };
    srv->start();

    std::string baseUrl  = "http://127.0.0.1:" + std::to_string(srv->boundPort);
    auto        provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl, 5, 2));

    neograph::CompletionParams params;
    params.model    = "gpt-4o-mini";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Stream test"}
    };

    std::string              accumulated;
    neograph::StreamCallback onChunk = [&](const std::string& chunk) {
        accumulated += chunk;
    };

    auto             start = std::chrono::steady_clock::now();
    asio::io_context ctx;
    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
            try {
                auto result = co_await provider->invoke(params, onChunk);
                XX_TEST_EXPECT_EQ(result.message.content, "Hi");
            } catch (const std::exception&) {
            }
        },
        asio::detached
    );
    ctx.run();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start
    )
                       .count();

    XX_TEST_EXPECT_EQ(accumulated, "Hi");
    XX_TEST_EXPECT_TRUE(elapsed >= 1500);
    XX_TEST_EXPECT_TRUE(elapsed < 8000);

    srv->stop();
}

/// 已收到 [DONE] 后连接层报错 (如 stream_truncated/连接被中断) 不应使请求失败,
/// 应忽略传输错误并返回已完整接收的结果
void test_streaming_abort_after_done_ignored() {
    auto srv           = std::make_unique<StallServer>();
    srv->mode          = StallServer::Mode::AbortAfterChunks;
    srv->partialChunks = {
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"\"}}]}\n\n",
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Full\"}}]}\n\n",
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\" text\"}}]}\n\n",
        "data: {\"choices\":[{\"index\":0,\"delta\":{}}],\"usage\":{\"prompt_tokens\":3,"
        "\"completion_tokens\":2,\"total_tokens\":5}}\n\n",
        "data: [DONE]\n\n",
        // 无 chunked 终止块, 服务端直接断开 → 客户端传输层报错, 但 [DONE] 已收到
    };
    srv->start();

    std::string baseUrl  = "http://127.0.0.1:" + std::to_string(srv->boundPort);
    auto        provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl, 5, 5));

    neograph::CompletionParams params;
    params.model    = "gpt-4o-mini";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Stream abort test"}
    };

    std::string              accumulated;
    neograph::StreamCallback onChunk = [&](const std::string& chunk) {
        accumulated += chunk;
    };

    bool             threw = false;
    std::string      content;
    int              totalTokens = 0;
    asio::io_context ctx;
    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
            try {
                auto result = co_await provider->invoke(params, onChunk);
                content     = result.message.content;
                totalTokens = result.usage.total_tokens;
            } catch (const std::exception& e) {
                threw = true;
                TEST_FAIL << "abort-after-done should be ignored, but threw: " << e.what()
                          << std::endl;
            }
        },
        asio::detached
    );
    ctx.run();

    XX_TEST_EXPECT_FALSE(threw);
    XX_TEST_EXPECT_EQ(content, "Full text");
    XX_TEST_EXPECT_EQ(accumulated, "Full text");
    XX_TEST_EXPECT_EQ(totalTokens, 5);

    srv->stop();
}

void test_send_timeout_calculation() {
    XX_TEST_EXPECT_EQ(agentxx::util::HttpClient::calcTimeoutBySize(0).count(), 30);
    XX_TEST_EXPECT_EQ(agentxx::util::HttpClient::calcTimeoutBySize(1024).count(), 30);
    XX_TEST_EXPECT_EQ(agentxx::util::HttpClient::calcTimeoutBySize(65536 * 30).count(), 30);
    XX_TEST_EXPECT_EQ(agentxx::util::HttpClient::calcTimeoutBySize(65536 * 31).count(), 31);
    XX_TEST_EXPECT_EQ(agentxx::util::HttpClient::calcTimeoutBySize(65536 * 500).count(), 500);
}

// ---------------------------------------------------------------------------
// Test runner
// ---------------------------------------------------------------------------

/// 直接单测 SSE 解析: 冒号后无空格 / 末尾无 "\n" 的行
void test_openai_sse_parsing_edge_cases() {
    using server::OpenAIProvider;

    // B2: "data:" 冒号后无单个空格也必须解析
    {
        std::string              buf = "data:{\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\n";
        neograph::ChatCompletion completion;
        std::string              content, thinking;
        std::map<int, neograph::ToolCall> tcMap;
        OpenAIProvider::processSseBuffer(buf, completion, content, thinking, tcMap, nullptr);
        XX_TEST_EXPECT_EQ(content, "Hi");
    }

    // B3: 连接关闭时末尾未以 "\n" 结尾的行, finalFlush=true 时应补解析
    {
        std::string buf
            = "data: {\"choices\":[{\"delta\":{\"content\":\"End\"}}]}"; // 无 trailing \n
        neograph::ChatCompletion          completion;
        std::string                       content, thinking;
        std::map<int, neograph::ToolCall> tcMap;
        OpenAIProvider::processSseBuffer(
            buf,
            completion,
            content,
            thinking,
            tcMap,
            nullptr,
            /*finalFlush=*/false
        );
        XX_TEST_EXPECT_EQ(content, "");
        OpenAIProvider::processSseBuffer(
            buf,
            completion,
            content,
            thinking,
            tcMap,
            nullptr,
            /*finalFlush=*/true
        );
        XX_TEST_EXPECT_EQ(content, "End");
    }
}

/// processSseBuffer 返回值: 仅当处理到 "data: [DONE]" 时为 true (流截断检测依据)
void test_openai_sse_done_flag() {
    using server::OpenAIProvider;

    // 普通数据行 → false
    {
        std::string buf = "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\n";
        neograph::ChatCompletion          completion;
        std::string                       content, thinking;
        std::map<int, neograph::ToolCall> tcMap;
        bool done = OpenAIProvider::processSseBuffer(
            buf,
            completion,
            content,
            thinking,
            tcMap,
            nullptr
        );
        XX_TEST_EXPECT_FALSE(done);
        XX_TEST_EXPECT_EQ(content, "Hi");
    }

    // 含 [DONE] → true
    {
        std::string buf
            = "data: {\"choices\":[{\"delta\":{\"content\":\"Ok\"}}]}\n\ndata: [DONE]\n\n";
        neograph::ChatCompletion          completion;
        std::string                       content, thinking;
        std::map<int, neograph::ToolCall> tcMap;
        bool done = OpenAIProvider::processSseBuffer(
            buf,
            completion,
            content,
            thinking,
            tcMap,
            nullptr
        );
        XX_TEST_EXPECT_TRUE(done);
    }

    // [DONE] 无结尾换行, finalFlush 时也应识别
    {
        std::string                       buf = "data: [DONE]";
        neograph::ChatCompletion          completion;
        std::string                       content, thinking;
        std::map<int, neograph::ToolCall> tcMap;
        bool done = OpenAIProvider::processSseBuffer(
            buf,
            completion,
            content,
            thinking,
            tcMap,
            nullptr,
            /*finalFlush=*/true
        );
        XX_TEST_EXPECT_TRUE(done);
    }
}

asio::awaitable<TestResult> run_openai_provider_tests() {
    g_openai_passed = 0;
    g_openai_failed = 0;

    // Unit tests (no server needed)
    test_factory_and_name();
    test_config_defaults();
    test_extra_body_with_custom_params();
    test_openai_sse_parsing_edge_cases();
    test_openai_sse_done_flag();

    // ModelProviderRegistry::createProvider tests
    test_create_provider_openai();
    test_create_provider_anthropic();
    test_create_provider_default_type();

    // Unit tests for reasoning/thinking parsing (no server needed)
    test_parse_response_message_with_reasoning();
    test_parse_response_message_with_thinking_field();
    test_parse_response_message_without_reasoning();
    test_parse_response_message_null_reasoning();
    test_parse_response_message_reasoning_preferred_over_thinking();
    test_messages_to_json_with_reasoning();
    test_messages_to_json_without_reasoning();
    test_messages_to_json_reasoning_roundtrip();

    // Unit tests for <think> tag extraction (no server needed)
    test_extract_think_tags_basic();
    test_extract_think_tags_no_tags();
    test_extract_think_tags_unclosed();
    test_extract_think_tags_multiple_blocks();
    test_extract_think_tags_empty_block();
    test_extract_think_tags_only_think();
    test_extract_think_tags_closed_then_unclosed();
    test_extract_think_tags_unclosed_direct();

    // Integration tests with mock server
    uint16_t port = 0;
    auto     mock = startMockServer(port);
    if (!mock || port == 0) {
        TEST_FAIL << "Failed to start mock server" << std::endl;
        g_openai_failed++;
        co_return TestResult{g_openai_passed, g_openai_failed};
    }

    std::cout << "Mock OpenAI server on port " << port << std::endl;

    co_await test_non_streaming_completion(*mock, port);
    co_await test_non_streaming_tool_call(*mock, port);
    co_await test_non_streaming_tool_call_missing_id(*mock, port);
    co_await test_streaming_tool_call_missing_id(*mock, port);
    co_await test_rate_limit_error(*mock, port);
    co_await test_server_error(*mock, port);
    co_await test_extra_body_passthrough(*mock, port);
    co_await test_per_call_extra_fields(*mock, port);
    co_await test_streaming_completion(*mock, port);

    // Reasoning/thinking content tests
    co_await test_non_streaming_reasoning_content(*mock, port);
    co_await test_non_streaming_thinking_field(*mock, port);
    co_await test_non_streaming_reasoning_at_choice_level(*mock, port);
    co_await test_non_streaming_thinking_at_choice_level(*mock, port);
    co_await test_streaming_reasoning_content(*mock, port);
    co_await test_streaming_thinking_field_compat(*mock, port);
    co_await test_streaming_reasoning_with_null_skips(*mock, port);
    co_await test_streaming_reasoning_preferred_over_thinking(*mock, port);
    co_await test_streaming_reasoning_only_no_content(*mock, port);
    co_await test_streaming_malformed_chunk_skipped(*mock, port);
    co_await test_streaming_missing_done_throws(*mock, port);

    // <think> tag tests
    co_await test_non_streaming_think_tags_in_content(*mock, port);
    co_await test_non_streaming_think_tags_prefer_reasoning_field(*mock, port);
    co_await test_streaming_think_tags_split_across_chunks(*mock, port);

    // sendThinking config tests
    co_await test_sendthinking_false_strips_reasoning(*mock, port);
    co_await test_sendthinking_true_preserves_reasoning(*mock, port);
    co_await test_sendthinking_no_reasoning_has_no_effect(*mock, port);

    // True streaming incremental verification
    test_true_streaming_incremental(port);

    // Timeout tests
    test_send_timeout_calculation();
    test_connect_timeout();
    test_read_timeout_streaming();
    test_streaming_abort_after_done_ignored();

    mock->server->stop();
    mock->thread.join();

    co_return TestResult{g_openai_passed, g_openai_failed};
}

} // namespace test
} // namespace agentxx
