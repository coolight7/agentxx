#include "test_anthropic_provider.h"
#include "agentxx/protocol/anthropic_provider.h"
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
agentxx::agent::ModelConfig makeAntCfg(
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

int g_anthropic_passed = 0;
int g_anthropic_failed = 0;

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

void test_anthropic_factory_and_name() {
    {
        agentxx::agent::ModelConfig mc;
        mc.name   = "test";
        mc.apiKey = "sk-ant-test";
        auto p    = server::AnthropicProvider::create(mc);
        XX_TEST_EXPECT_TRUE(p != nullptr);
        XX_TEST_EXPECT_EQ(p->get_name(), "anthropic");
    }
    {
        agentxx::agent::ModelConfig mc;
        mc.name   = "test";
        mc.apiKey = "sk-ant-shared";
        auto p    = server::AnthropicProvider::create_shared(mc);
        XX_TEST_EXPECT_TRUE(p != nullptr);
        XX_TEST_EXPECT_EQ(p->get_name(), "anthropic");
    }
}

void test_anthropic_config_defaults() {
    agentxx::agent::ModelConfig mc;
    mc.name   = "test";
    mc.apiKey = "sk-ant-defaults";
    auto p    = server::AnthropicProvider::create(mc);
    XX_TEST_EXPECT_TRUE(p != nullptr);
    XX_TEST_EXPECT_EQ(p->get_name(), "anthropic");
}

void test_convert_messages_basic() {
    std::vector<neograph::ChatMessage> msgs = {
        {.role = "user",      .content = "Hello"   },
        {.role = "assistant", .content = "Hi there"},
    };
    auto [system, arr] = server::AnthropicProvider::convertMessages(msgs);
    XX_TEST_EXPECT_TRUE(system.empty());
    XX_TEST_EXPECT_EQ(arr.size(), (size_t)2);
    XX_TEST_EXPECT_EQ(arr[0]["role"].get<std::string>(), "user");
    XX_TEST_EXPECT_EQ(arr[0]["content"].get<std::string>(), "Hello");
    XX_TEST_EXPECT_EQ(arr[1]["role"].get<std::string>(), "assistant");
    XX_TEST_EXPECT_EQ(arr[1]["content"].get<std::string>(), "Hi there");
}

void test_convert_messages_system_extraction() {
    std::vector<neograph::ChatMessage> msgs = {
        {.role = "system", .content = "You are helpful."},
        {.role = "user",   .content = "Hello"           },
    };
    auto [system, arr] = server::AnthropicProvider::convertMessages(msgs);
    XX_TEST_EXPECT_EQ(system, "You are helpful.");
    XX_TEST_EXPECT_EQ(arr.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(arr[0]["role"].get<std::string>(), "user");
}

void test_convert_messages_multiple_system() {
    std::vector<neograph::ChatMessage> msgs = {
        {.role = "system", .content = "Rule 1."},
        {.role = "system", .content = "Rule 2."},
        {.role = "user",   .content = "Hi"     },
    };
    auto [system, arr] = server::AnthropicProvider::convertMessages(msgs);
    XX_TEST_EXPECT_EQ(system, "Rule 1.\nRule 2.");
    XX_TEST_EXPECT_EQ(arr.size(), (size_t)1);
}

void test_convert_messages_tool_result() {
    std::vector<neograph::ChatMessage> msgs = {
        {.role = "user", .content = "What is the weather?"},
        {.role    = "assistant",
         .content = "",
         .tool_calls
         = {{.id = "call_1", .name = "get_weather", .arguments = R"({"location":"Tokyo"})"}}},
        {.role = "tool", .content = "Sunny, 25C", .tool_call_id = "call_1"},
    };
    auto [system, arr] = server::AnthropicProvider::convertMessages(msgs);
    XX_TEST_EXPECT_EQ(arr.size(), (size_t)3);

    // Check tool_use block in assistant message
    const auto& assistantMsg = arr[1];
    XX_TEST_EXPECT_EQ(assistantMsg["role"].get<std::string>(), "assistant");
    XX_TEST_EXPECT_TRUE(assistantMsg["content"].is_array());
    const auto& blocks = assistantMsg["content"];
    XX_TEST_EXPECT_EQ(blocks.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(blocks[0]["type"].get<std::string>(), "tool_use");
    XX_TEST_EXPECT_EQ(blocks[0]["id"].get<std::string>(), "call_1");
    XX_TEST_EXPECT_EQ(blocks[0]["name"].get<std::string>(), "get_weather");
    XX_TEST_EXPECT_EQ(blocks[0]["input"]["location"].get<std::string>(), "Tokyo");

    // Check tool_result block
    const auto& toolMsg = arr[2];
    XX_TEST_EXPECT_EQ(toolMsg["role"].get<std::string>(), "user");
    XX_TEST_EXPECT_TRUE(toolMsg["content"].is_array());
    const auto& toolBlocks = toolMsg["content"];
    XX_TEST_EXPECT_EQ(toolBlocks[0]["type"].get<std::string>(), "tool_result");
    XX_TEST_EXPECT_EQ(toolBlocks[0]["tool_use_id"].get<std::string>(), "call_1");
    XX_TEST_EXPECT_EQ(toolBlocks[0]["content"].get<std::string>(), "Sunny, 25C");
}

void test_convert_messages_assistant_with_text_and_tool() {
    std::vector<neograph::ChatMessage> msgs = {
        {.role       = "assistant",
         .content    = "Let me check.",
         .tool_calls = {{.id = "call_2", .name = "search", .arguments = R"({"q":"test"})"}}},
    };
    auto [system, arr] = server::AnthropicProvider::convertMessages(msgs);
    XX_TEST_EXPECT_EQ(arr.size(), (size_t)1);
    const auto& blocks = arr[0]["content"];
    XX_TEST_EXPECT_EQ(blocks.size(), (size_t)2);
    XX_TEST_EXPECT_EQ(blocks[0]["type"].get<std::string>(), "text");
    XX_TEST_EXPECT_EQ(blocks[0]["text"].get<std::string>(), "Let me check.");
    XX_TEST_EXPECT_EQ(blocks[1]["type"].get<std::string>(), "tool_use");
}

void test_convert_messages_thinking_enabled() {
    std::vector<neograph::ChatMessage> msgs = {
        {.role = "assistant", .content = "Final answer", .reasoning_content = "Step by step..."},
    };
    auto [system, arr] = server::AnthropicProvider::convertMessages(msgs, true);
    XX_TEST_EXPECT_TRUE(system.empty());
    XX_TEST_EXPECT_EQ(arr.size(), (size_t)1);

    // Content should be an array with thinking + text blocks
    XX_TEST_EXPECT_TRUE(arr[0]["content"].is_array());
    XX_TEST_EXPECT_EQ(arr[0]["content"].size(), (size_t)2);
    XX_TEST_EXPECT_EQ(arr[0]["content"][0]["type"].get<std::string>(), "thinking");
    XX_TEST_EXPECT_EQ(arr[0]["content"][0]["thinking"].get<std::string>(), "Step by step...");
    XX_TEST_EXPECT_EQ(arr[0]["content"][1]["type"].get<std::string>(), "text");
    XX_TEST_EXPECT_EQ(arr[0]["content"][1]["text"].get<std::string>(), "Final answer");
}

void test_convert_messages_thinking_disabled() {
    std::vector<neograph::ChatMessage> msgs = {
        {.role = "assistant", .content = "Final answer", .reasoning_content = "Step by step..."},
    };
    auto [system, arr] = server::AnthropicProvider::convertMessages(msgs, false);
    XX_TEST_EXPECT_TRUE(system.empty());
    XX_TEST_EXPECT_EQ(arr.size(), (size_t)1);

    // Content should be a plain string, not an array
    XX_TEST_EXPECT_TRUE(arr[0]["content"].is_string());
    XX_TEST_EXPECT_EQ(arr[0]["content"].get<std::string>(), "Final answer");
}

void test_convert_messages_thinking_default_disabled() {
    std::vector<neograph::ChatMessage> msgs = {
        {.role = "assistant", .content = "No thinking sent", .reasoning_content = "Hidden reasoning"
        },
    };
    auto [system, arr] = server::AnthropicProvider::convertMessages(msgs); // default false
    XX_TEST_EXPECT_TRUE(arr[0]["content"].is_string());
    XX_TEST_EXPECT_EQ(arr[0]["content"].get<std::string>(), "No thinking sent");
}

void test_convert_messages_thinking_with_tool_calls() {
    neograph::ChatMessage msg;
    msg.role              = "assistant";
    msg.content           = "Let me check";
    msg.reasoning_content = "I need to search";
    msg.tool_calls        = {
        {.id = "call_1", .name = "search", .arguments = R"({"q":"test"})"}
    };
    std::vector<neograph::ChatMessage> msgs = {msg};
    auto [system, arr] = server::AnthropicProvider::convertMessages(msgs, true);
    XX_TEST_EXPECT_EQ(arr.size(), (size_t)1);
    const auto& blocks = arr[0]["content"];
    // order: thinking, text, tool_use
    XX_TEST_EXPECT_EQ(blocks.size(), (size_t)3);
    XX_TEST_EXPECT_EQ(blocks[0]["type"].get<std::string>(), "thinking");
    XX_TEST_EXPECT_EQ(blocks[0]["thinking"].get<std::string>(), "I need to search");
    XX_TEST_EXPECT_EQ(blocks[1]["type"].get<std::string>(), "text");
    XX_TEST_EXPECT_EQ(blocks[2]["type"].get<std::string>(), "tool_use");
}

void test_convert_messages_thinking_disabled_with_tool_calls() {
    neograph::ChatMessage msg;
    msg.role              = "assistant";
    msg.content           = "Let me check";
    msg.reasoning_content = "I need to search";
    msg.tool_calls        = {
        {.id = "call_1", .name = "search", .arguments = R"({"q":"test"})"}
    };
    std::vector<neograph::ChatMessage> msgs = {msg};
    auto [system, arr] = server::AnthropicProvider::convertMessages(msgs, false);
    XX_TEST_EXPECT_EQ(arr.size(), (size_t)1);
    const auto& blocks = arr[0]["content"];
    // Only text and tool_use, no thinking
    XX_TEST_EXPECT_EQ(blocks.size(), (size_t)2);
    XX_TEST_EXPECT_EQ(blocks[0]["type"].get<std::string>(), "text");
    XX_TEST_EXPECT_EQ(blocks[1]["type"].get<std::string>(), "tool_use");
}

void test_convert_messages_thinking_only_reasoning_no_content() {
    // Edge case: reasoning_content present but content is empty
    std::vector<neograph::ChatMessage> msgs = {
        {.role = "assistant", .content = "", .reasoning_content = "Just thinking"},
    };
    auto [system, arr] = server::AnthropicProvider::convertMessages(msgs, true);
    XX_TEST_EXPECT_TRUE(arr[0]["content"].is_array());
    XX_TEST_EXPECT_EQ(arr[0]["content"].size(), (size_t)1);
    XX_TEST_EXPECT_EQ(arr[0]["content"][0]["type"].get<std::string>(), "thinking");
    XX_TEST_EXPECT_EQ(arr[0]["content"][0]["thinking"].get<std::string>(), "Just thinking");
}

void test_convert_messages_thinking_user_no_effect() {
    // User messages with reasoning_content should not be affected
    std::vector<neograph::ChatMessage> msgs = {
        {.role = "user", .content = "Hello", .reasoning_content = "User thinking"},
    };
    auto [system, arr] = server::AnthropicProvider::convertMessages(msgs, true);
    XX_TEST_EXPECT_TRUE(arr[0]["content"].is_string());
    XX_TEST_EXPECT_EQ(arr[0]["content"].get<std::string>(), "Hello");
}

void test_convert_tools() {
    std::vector<neograph::ChatTool> tools = {
        {.name        = "get_weather",
         .description = "Get weather",
         .parameters
         = neograph::json::parse(R"({"type":"object","properties":{"location":{"type":"string"}}})")
        },
    };
    auto arr = server::AnthropicProvider::convertTools(tools);
    XX_TEST_EXPECT_EQ(arr.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(arr[0]["name"].get<std::string>(), "get_weather");
    XX_TEST_EXPECT_EQ(arr[0]["description"].get<std::string>(), "Get weather");
    XX_TEST_EXPECT_TRUE(arr[0].contains("input_schema"));
    XX_TEST_EXPECT_EQ(arr[0]["input_schema"]["type"].get<std::string>(), "object");
    XX_TEST_EXPECT_FALSE(arr[0].contains("parameters"));
}

void test_parse_response_text() {
    auto resp       = neograph::json::parse(R"({
    "id": "msg_001",
    "type": "message",
    "role": "assistant",
    "content": [{"type": "text", "text": "Hello from Claude!"}],
    "stop_reason": "end_turn",
    "usage": {"input_tokens": 10, "output_tokens": 5}
  })");
    auto completion = server::AnthropicProvider::parseResponse(resp);
    XX_TEST_EXPECT_EQ(completion.message.role, "assistant");
    XX_TEST_EXPECT_EQ(completion.message.content, "Hello from Claude!");
    XX_TEST_EXPECT_TRUE(completion.message.tool_calls.empty());
    XX_TEST_EXPECT_TRUE(completion.message.reasoning_content.empty());
    XX_TEST_EXPECT_EQ(completion.usage.prompt_tokens, 10);
    XX_TEST_EXPECT_EQ(completion.usage.completion_tokens, 5);
    XX_TEST_EXPECT_EQ(completion.usage.total_tokens, 15);
}

void test_parse_response_tool_use() {
    auto resp       = neograph::json::parse(R"({
    "id": "msg_002",
    "type": "message",
    "role": "assistant",
    "content": [
      {"type": "text", "text": "Let me check the weather."},
      {"type": "tool_use", "id": "toolu_01", "name": "get_weather", "input": {"location": "Tokyo"}}
    ],
    "stop_reason": "tool_use",
    "usage": {"input_tokens": 20, "output_tokens": 15}
  })");
    auto completion = server::AnthropicProvider::parseResponse(resp);
    XX_TEST_EXPECT_EQ(completion.message.content, "Let me check the weather.");
    XX_TEST_EXPECT_EQ(completion.message.tool_calls.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(completion.message.tool_calls[0].id, "toolu_01");
    XX_TEST_EXPECT_EQ(completion.message.tool_calls[0].name, "get_weather");
    XX_TEST_EXPECT_TRUE(
        completion.message.tool_calls[0].arguments.find("Tokyo") != std::string::npos
    );
}

void test_parse_response_thinking() {
    auto resp       = neograph::json::parse(R"({
    "id": "msg_003",
    "type": "message",
    "role": "assistant",
    "content": [
      {"type": "thinking", "thinking": "Let me reason step by step..."},
      {"type": "text", "text": "The answer is 42."}
    ],
    "stop_reason": "end_turn",
    "usage": {"input_tokens": 5, "output_tokens": 20}
  })");
    auto completion = server::AnthropicProvider::parseResponse(resp);
    XX_TEST_EXPECT_EQ(completion.message.content, "The answer is 42.");
    XX_TEST_EXPECT_EQ(completion.message.reasoning_content, "Let me reason step by step...");
}

void test_parse_response_mixed() {
    auto resp       = neograph::json::parse(R"({
    "id": "msg_004",
    "type": "message",
    "role": "assistant",
    "content": [
      {"type": "thinking", "thinking": "Hmm..."},
      {"type": "text", "text": "Part 1. "},
      {"type": "text", "text": "Part 2."},
      {"type": "tool_use", "id": "toolu_02", "name": "calc", "input": {"expr": "6*7"}}
    ],
    "stop_reason": "tool_use",
    "usage": {"input_tokens": 8, "output_tokens": 30}
  })");
    auto completion = server::AnthropicProvider::parseResponse(resp);
    XX_TEST_EXPECT_EQ(completion.message.content, "Part 1. Part 2.");
    XX_TEST_EXPECT_EQ(completion.message.reasoning_content, "Hmm...");
    XX_TEST_EXPECT_EQ(completion.message.tool_calls.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(completion.message.tool_calls[0].name, "calc");
}

void test_parse_response_usage() {
    auto resp       = neograph::json::parse(R"({
    "id": "msg_005",
    "type": "message",
    "role": "assistant",
    "content": [{"type": "text", "text": "Hi"}],
    "usage": {"input_tokens": 100, "output_tokens": 50}
  })");
    auto completion = server::AnthropicProvider::parseResponse(resp);
    XX_TEST_EXPECT_EQ(completion.usage.prompt_tokens, 100);
    XX_TEST_EXPECT_EQ(completion.usage.completion_tokens, 50);
    XX_TEST_EXPECT_EQ(completion.usage.total_tokens, 150);
}

// ---------------------------------------------------------------------------
// Mock server
// ---------------------------------------------------------------------------

enum class AnthropicMockMode {
    Normal,
    ToolCall,
    Thinking,
    RateLimit,
    ServerError,
    Streaming,
    StreamingThinking,
    StreamingToolCall,
};

class MockAnthropicServer {
public:

    std::unique_ptr<HttpServer> server;
    std::thread                 thread;
    AnthropicMockMode           mode = AnthropicMockMode::Normal;
    std::string                 lastRequestBody;
    std::string                 lastRequestHeaders;

    std::vector<std::string>      sseChunks;
    std::optional<neograph::json> customResponse;

    static std::string sseEvent(std::string_view event, std::string_view data) {
        return "event: " + std::string(event) + "\ndata: " + std::string(data) + "\n\n";
    }

    neograph::json
        makeTextResponse(std::string_view content, int inputTok = 10, int outputTok = 5) const {
        return neograph::json::parse(
            R"({
      "id": "msg_mock",
      "type": "message",
      "role": "assistant",
      "content": [{"type": "text", "text": ")"
            + std::string(content) + R"("}],
      "stop_reason": "end_turn",
      "usage": {"input_tokens": )"
            + std::to_string(inputTok) + R"(, "output_tokens": )" + std::to_string(outputTok)
            + R"(}
    })"
        );
    }

    neograph::json makeToolCallResponse() const {
        return neograph::json::parse(R"({
      "id": "msg_tool",
      "type": "message",
      "role": "assistant",
      "content": [
        {"type": "text", "text": "Let me check."},
        {"type": "tool_use", "id": "toolu_mock", "name": "get_weather", "input": {"location": "Tokyo"}}
      ],
      "stop_reason": "tool_use",
      "usage": {"input_tokens": 15, "output_tokens": 20}
    })");
    }

    neograph::json makeThinkingResponse() const {
        return neograph::json::parse(R"({
      "id": "msg_think",
      "type": "message",
      "role": "assistant",
      "content": [
        {"type": "thinking", "thinking": "Step by step reasoning..."},
        {"type": "text", "text": "The answer is 42."}
      ],
      "stop_reason": "end_turn",
      "usage": {"input_tokens": 8, "output_tokens": 25}
    })");
    }
};

std::unique_ptr<MockAnthropicServer> startAnthropicMockServer(uint16_t& outPort) {
    auto mock = std::make_unique<MockAnthropicServer>();

    // Default streaming chunks
    mock->sseChunks = {
        MockAnthropicServer::sseEvent(
            "message_start",
            R"({"type":"message_start","message":{"id":"msg_stream","type":"message","role":"assistant","usage":{"input_tokens":5}}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_start",
            R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hello"}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":" world"}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_stop",
            R"({"type":"content_block_stop","index":0})"
        ),
        MockAnthropicServer::sseEvent(
            "message_delta",
            R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":3}})"
        ),
        MockAnthropicServer::sseEvent("message_stop", R"({"type":"message_stop"})"),
    };

    mock->server = std::make_unique<HttpServer>(
        HttpServer::Config{.address = "127.0.0.1", .port = 0, .ioThreads = 1}
    );

    auto handle = [mock = mock.get(
                   )](HttpServer::Request& req, HttpServer::Response& resp, std::string_view
                  ) -> asio::awaitable<void> {
        mock->lastRequestBody = req.body();
        // Capture headers for verification
        std::string headers;
        for (const auto& field : req) {
            headers += std::string(field.name_string()) + ": " + std::string(field.value()) + "\n";
        }
        mock->lastRequestHeaders = headers;

        switch (mock->mode) {
            case AnthropicMockMode::RateLimit:
                resp.result(boost::beast::http::status::too_many_requests);
                resp.set(boost::beast::http::field::content_type, "application/json");
                resp.set(boost::beast::http::field::retry_after, "7");
                resp.body(
                ) = R"({"type":"error","error":{"type":"rate_limit_error","message":"Rate limit exceeded"}})";
                resp.prepare_payload();
                break;

            case AnthropicMockMode::ServerError:
                resp.result(boost::beast::http::status::internal_server_error);
                resp.set(boost::beast::http::field::content_type, "application/json");
                resp.body()
                    = R"({"type":"error","error":{"type":"api_error","message":"Internal error"}})";
                resp.prepare_payload();
                break;

            case AnthropicMockMode::ToolCall:
                resp.result(boost::beast::http::status::ok);
                resp.set(boost::beast::http::field::content_type, "application/json");
                resp.body() = mock->makeToolCallResponse().dump();
                resp.prepare_payload();
                break;

            case AnthropicMockMode::Thinking:
                resp.result(boost::beast::http::status::ok);
                resp.set(boost::beast::http::field::content_type, "application/json");
                resp.body() = mock->makeThinkingResponse().dump();
                resp.prepare_payload();
                break;

            case AnthropicMockMode::Streaming:
            case AnthropicMockMode::StreamingThinking:
            case AnthropicMockMode::StreamingToolCall: {
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

            case AnthropicMockMode::Normal:
            default:
                resp.result(boost::beast::http::status::ok);
                resp.set(boost::beast::http::field::content_type, "application/json");
                if (mock->customResponse.has_value()) {
                    resp.body() = mock->customResponse->dump();
                    mock->customResponse.reset();
                } else {
                    resp.body() = mock->makeTextResponse("Hello from Claude!").dump();
                }
                resp.prepare_payload();
                break;
        }
        co_return;
    };

    mock->server->router().add("/v1/messages", 2, std::make_shared<HttpServer::Handler>(handle));

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
// Integration tests
// ---------------------------------------------------------------------------

asio::awaitable<void> test_non_streaming_completion(MockAnthropicServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = AnthropicMockMode::Normal;

    auto provider = server::AnthropicProvider::create(makeAntCfg("sk-ant-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "claude-sonnet-4-20250514";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Say hello"}
    };

    try {
        auto result = co_await provider->invoke(params, nullptr);
        XX_TEST_EXPECT_EQ(result.message.role, "assistant");
        XX_TEST_EXPECT_TRUE(result.message.content.find("Hello") != std::string::npos);
        XX_TEST_EXPECT_TRUE(result.usage.total_tokens > 0);

        auto sent = neograph::json::parse(mock.lastRequestBody);
        XX_TEST_EXPECT_EQ(sent["model"].get<std::string>(), "claude-sonnet-4-20250514");
        XX_TEST_EXPECT_TRUE(sent.contains("messages"));
        XX_TEST_EXPECT_TRUE(sent.contains("max_tokens"));
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "non-streaming completion failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_non_streaming_tool_call(MockAnthropicServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = AnthropicMockMode::ToolCall;

    auto provider = server::AnthropicProvider::create(makeAntCfg("sk-ant-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "claude-sonnet-4-20250514";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Weather?"}
    };
    params.tools = {
        neograph::ChatTool{
                           .name        = "get_weather",
                           .description = "Get weather",
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
        TEST_FAIL << "tool call failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_non_streaming_thinking(MockAnthropicServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = AnthropicMockMode::Thinking;

    auto provider = server::AnthropicProvider::create(makeAntCfg("sk-ant-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "claude-sonnet-4-20250514";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Think step by step"}
    };

    try {
        auto result = co_await provider->invoke(params, nullptr);
        XX_TEST_EXPECT_EQ(result.message.content, "The answer is 42.");
        XX_TEST_EXPECT_TRUE(!result.message.reasoning_content.empty());
        XX_TEST_EXPECT_TRUE(
            result.message.reasoning_content.find("reasoning") != std::string::npos
        );
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "thinking test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_rate_limit_error(MockAnthropicServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = AnthropicMockMode::RateLimit;

    auto provider = server::AnthropicProvider::create(makeAntCfg("sk-ant-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "claude-sonnet-4-20250514";
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
    } catch (...) {
    }

    if (caught) {
        XX_TEST_PASSED++;
        XX_TEST_EXPECT_EQ(retryAfter, 7);
    } else {
        XX_TEST_FAILED++;
        TEST_FAIL << "expected RateLimitError" << std::endl;
    }
}

asio::awaitable<void> test_server_error(MockAnthropicServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = AnthropicMockMode::ServerError;

    auto provider = server::AnthropicProvider::create(makeAntCfg("sk-ant-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "claude-sonnet-4-20250514";
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
        TEST_FAIL << "expected runtime_error for 500" << std::endl;
    }
}

asio::awaitable<void> test_request_headers(MockAnthropicServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = AnthropicMockMode::Normal;

    auto provider = server::AnthropicProvider::create(makeAntCfg("sk-ant-header-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "claude-sonnet-4-20250514";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "test headers"}
    };

    try {
        co_await provider->invoke(params, nullptr);
        XX_TEST_EXPECT_TRUE(
            mock.lastRequestHeaders.find("x-api-key: sk-ant-header-test") != std::string::npos
        );
        XX_TEST_EXPECT_TRUE(
            mock.lastRequestHeaders.find("anthropic-version: 2023-06-01") != std::string::npos
        );
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "request headers test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_request_body_format(MockAnthropicServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = AnthropicMockMode::Normal;

    auto provider = server::AnthropicProvider::create(makeAntCfg("sk-ant-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "claude-sonnet-4-20250514";
    params.messages = {
        neograph::ChatMessage{.role = "system", .content = "Be helpful."},
        neograph::ChatMessage{.role = "user",   .content = "Hi"         },
    };
    params.tools = {
        neograph::ChatTool{
                           .name        = "search",
                           .description = "Search the web",
                           .parameters  = neograph::json::parse(R"({"type":"object","properties":{}})")
        }
    };

    try {
        co_await provider->invoke(params, nullptr);
        auto sent = neograph::json::parse(mock.lastRequestBody);

        // System should be top-level
        XX_TEST_EXPECT_TRUE(sent.contains("system"));
        XX_TEST_EXPECT_EQ(sent["system"].get<std::string>(), "Be helpful.");

        // Messages should not contain system
        XX_TEST_EXPECT_EQ(sent["messages"].size(), (size_t)1);
        XX_TEST_EXPECT_EQ(sent["messages"][0]["role"].get<std::string>(), "user");

        // Tools should use input_schema
        XX_TEST_EXPECT_TRUE(sent.contains("tools"));
        XX_TEST_EXPECT_TRUE(sent["tools"][0].contains("input_schema"));
        XX_TEST_EXPECT_FALSE(sent["tools"][0].contains("parameters"));

        // max_tokens required
        XX_TEST_EXPECT_TRUE(sent.contains("max_tokens"));
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "request body format test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_sendthinking_in_request_body(MockAnthropicServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = AnthropicMockMode::Normal;

    auto provider
        = server::AnthropicProvider::create(makeAntCfg("sk-ant-test", baseUrl, 10, 10, true));

    neograph::CompletionParams params;
    params.model    = "claude-sonnet-4-20250514";
    params.messages = {
        neograph::ChatMessage{
                              .role              = "assistant",
                              .content           = "Final answer",
                              .reasoning_content = "Deep thinking..."
        },
        neograph::ChatMessage{.role = "user", .content = "Continue"}
    };

    try {
        co_await provider->invoke(params, nullptr);
        auto sent = neograph::json::parse(mock.lastRequestBody);

        // First message should have thinking content block
        XX_TEST_EXPECT_TRUE(sent["messages"][0]["content"].is_array());
        XX_TEST_EXPECT_EQ(sent["messages"][0]["content"].size(), (size_t)2);
        XX_TEST_EXPECT_EQ(sent["messages"][0]["content"][0]["type"].get<std::string>(), "thinking");
        XX_TEST_EXPECT_EQ(
            sent["messages"][0]["content"][0]["thinking"].get<std::string>(),
            "Deep thinking..."
        );
        XX_TEST_EXPECT_EQ(sent["messages"][0]["content"][1]["type"].get<std::string>(), "text");
        XX_TEST_EXPECT_EQ(
            sent["messages"][0]["content"][1]["text"].get<std::string>(),
            "Final answer"
        );
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "sendThinking integration test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_streaming_completion(MockAnthropicServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = AnthropicMockMode::Streaming;

    // Reset to default streaming chunks
    mock.sseChunks = {
        MockAnthropicServer::sseEvent(
            "message_start",
            R"({"type":"message_start","message":{"id":"msg_s","type":"message","role":"assistant","usage":{"input_tokens":5}}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_start",
            R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hello"}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":" world"}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_stop",
            R"({"type":"content_block_stop","index":0})"
        ),
        MockAnthropicServer::sseEvent(
            "message_delta",
            R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":3}})"
        ),
        MockAnthropicServer::sseEvent("message_stop", R"({"type":"message_stop"})"),
    };

    auto provider = server::AnthropicProvider::create(makeAntCfg("sk-ant-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "claude-sonnet-4-20250514";
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
        XX_TEST_EXPECT_EQ(result.message.content, "Hello world");
        XX_TEST_EXPECT_EQ(accumulated, "Hello world");
        XX_TEST_EXPECT_TRUE(result.usage.total_tokens > 0);
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "streaming completion failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_streaming_thinking(MockAnthropicServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = AnthropicMockMode::StreamingThinking;

    mock.sseChunks = {
        MockAnthropicServer::sseEvent(
            "message_start",
            R"({"type":"message_start","message":{"id":"msg_t","type":"message","role":"assistant","usage":{"input_tokens":5}}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_start",
            R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":""}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"Let me think"}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":" carefully..."}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_stop",
            R"({"type":"content_block_stop","index":0})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_start",
            R"({"type":"content_block_start","index":1,"content_block":{"type":"text","text":""}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_delta",
            R"({"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"The answer is 42."}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_stop",
            R"({"type":"content_block_stop","index":1})"
        ),
        MockAnthropicServer::sseEvent(
            "message_delta",
            R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":10}})"
        ),
        MockAnthropicServer::sseEvent("message_stop", R"({"type":"message_stop"})"),
    };

    auto provider = server::AnthropicProvider::create(makeAntCfg("sk-ant-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "claude-sonnet-4-20250514";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Think about it"}
    };

    std::string              accumulated;
    neograph::StreamCallback onChunk = [&](const std::string& chunk) {
        accumulated += chunk;
    };

    try {
        auto result = co_await provider->invoke(params, onChunk);
        XX_TEST_EXPECT_EQ(result.message.content, "The answer is 42.");
        XX_TEST_EXPECT_EQ(result.message.reasoning_content, "Let me think carefully...");
        // StreamCallback only receives content tokens; thinking is in
        // reasoning_content
        XX_TEST_EXPECT_EQ(accumulated, "The answer is 42.");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "streaming thinking failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_streaming_tool_call(MockAnthropicServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = AnthropicMockMode::StreamingToolCall;

    mock.sseChunks = {
        MockAnthropicServer::sseEvent(
            "message_start",
            R"({"type":"message_start","message":{"id":"msg_tc","type":"message","role":"assistant","usage":{"input_tokens":10}}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_start",
            R"({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"toolu_stream","name":"get_weather"}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\"location\":"}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"\"Tokyo\"}"}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_stop",
            R"({"type":"content_block_stop","index":0})"
        ),
        MockAnthropicServer::sseEvent(
            "message_delta",
            R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":8}})"
        ),
        MockAnthropicServer::sseEvent("message_stop", R"({"type":"message_stop"})"),
    };

    auto provider = server::AnthropicProvider::create(makeAntCfg("sk-ant-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "claude-sonnet-4-20250514";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Weather in Tokyo?"}
    };
    params.tools = {
        neograph::ChatTool{
                           .name        = "get_weather",
                           .description = "Get weather",
                           .parameters  = neograph::json::parse(
                R"({"type":"object","properties":{"location":{"type":"string"}}})"
            )
        }
    };

    neograph::StreamCallback noop = [](const std::string&) {};

    try {
        auto result = co_await provider->invoke(params, noop);
        XX_TEST_EXPECT_EQ(result.message.tool_calls.size(), (size_t)1);
        if (!result.message.tool_calls.empty()) {
            XX_TEST_EXPECT_EQ(result.message.tool_calls[0].id, "toolu_stream");
            XX_TEST_EXPECT_EQ(result.message.tool_calls[0].name, "get_weather");
            XX_TEST_EXPECT_TRUE(
                result.message.tool_calls[0].arguments.find("Tokyo") != std::string::npos
            );
        }
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "streaming tool call failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void>
    test_streaming_mixed_thinking_and_content(MockAnthropicServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = AnthropicMockMode::StreamingThinking;

    mock.sseChunks = {
        MockAnthropicServer::sseEvent(
            "message_start",
            R"({"type":"message_start","message":{"id":"msg_mix","type":"message","role":"assistant","usage":{"input_tokens":5}}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_start",
            R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":""}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"Hmm..."}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_stop",
            R"({"type":"content_block_stop","index":0})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_start",
            R"({"type":"content_block_start","index":1,"content_block":{"type":"text","text":""}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_delta",
            R"({"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"Result."}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_stop",
            R"({"type":"content_block_stop","index":1})"
        ),
        MockAnthropicServer::sseEvent(
            "message_delta",
            R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":6}})"
        ),
        MockAnthropicServer::sseEvent("message_stop", R"({"type":"message_stop"})"),
    };

    auto provider = server::AnthropicProvider::create(makeAntCfg("sk-ant-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "claude-sonnet-4-20250514";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Mixed test"}
    };

    std::string              contentAccum;
    std::string              thinkingAccum;
    neograph::StreamCallback onChunk = [&](const std::string& chunk) {
        contentAccum += chunk;
    };

    try {
        auto result = co_await provider->invoke(params, onChunk);
        XX_TEST_EXPECT_EQ(result.message.content, "Result.");
        XX_TEST_EXPECT_EQ(result.message.reasoning_content, "Hmm...");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "streaming mixed test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_streaming_usage(MockAnthropicServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = AnthropicMockMode::Streaming;

    mock.sseChunks = {
        MockAnthropicServer::sseEvent(
            "message_start",
            R"({"type":"message_start","message":{"id":"msg_u","type":"message","role":"assistant","usage":{"input_tokens":42}}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_start",
            R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hi"}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_stop",
            R"({"type":"content_block_stop","index":0})"
        ),
        MockAnthropicServer::sseEvent(
            "message_delta",
            R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":7}})"
        ),
        MockAnthropicServer::sseEvent("message_stop", R"({"type":"message_stop"})"),
    };

    auto provider = server::AnthropicProvider::create(makeAntCfg("sk-ant-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "claude-sonnet-4-20250514";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Usage test"}
    };

    neograph::StreamCallback noop = [](const std::string&) {};

    try {
        auto result = co_await provider->invoke(params, noop);
        XX_TEST_EXPECT_EQ(result.usage.prompt_tokens, 42);
        XX_TEST_EXPECT_EQ(result.usage.completion_tokens, 7);
        XX_TEST_EXPECT_EQ(result.usage.total_tokens, 49);
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "streaming usage test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void>
    test_streaming_malformed_event_skipped(MockAnthropicServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = AnthropicMockMode::Streaming;

    mock.sseChunks = {
        MockAnthropicServer::sseEvent(
            "message_start",
            R"({"type":"message_start","message":{"id":"msg_m","type":"message","role":"assistant","usage":{"input_tokens":3}}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_start",
            R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"
        ),
        "event: content_block_delta\ndata: not-valid-json\n\n",
        MockAnthropicServer::sseEvent(
            "content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"OK"}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_stop",
            R"({"type":"content_block_stop","index":0})"
        ),
        MockAnthropicServer::sseEvent(
            "message_delta",
            R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":1}})"
        ),
        MockAnthropicServer::sseEvent("message_stop", R"({"type":"message_stop"})"),
    };

    auto provider = server::AnthropicProvider::create(makeAntCfg("sk-ant-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "claude-sonnet-4-20250514";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Malformed test"}
    };

    neograph::StreamCallback noop = [](const std::string&) {};

    try {
        auto result = co_await provider->invoke(params, noop);
        XX_TEST_EXPECT_EQ(result.message.content, "OK");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "malformed event test failed: " << e.what() << std::endl;
    }
}

/// 流未收到 message_stop 就结束 (HTTP 层完整但 SSE 协议层被截断) 必须报错,
/// 不能把截断的响应静默当作正常结果返回
asio::awaitable<void>
    test_streaming_missing_message_stop_throws(MockAnthropicServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = AnthropicMockMode::Streaming;

    mock.sseChunks = {
        MockAnthropicServer::sseEvent(
            "message_start",
            R"({"type":"message_start","message":{"id":"msg_t2","type":"message","role":"assistant","usage":{"input_tokens":3}}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_start",
            R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Partial"}})"
        ),
        // 故意不包含 message_stop: 模拟长输出被中间代理截断
    };

    auto provider = server::AnthropicProvider::create(makeAntCfg("sk-ant-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "claude-sonnet-4-20250514";
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

asio::awaitable<void> test_thinking_callback_separation(MockAnthropicServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = AnthropicMockMode::StreamingThinking;

    mock.sseChunks = {
        MockAnthropicServer::sseEvent(
            "message_start",
            R"({"type":"message_start","message":{"id":"msg_sep","type":"message","role":"assistant","usage":{"input_tokens":5}}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_start",
            R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":""}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"Deep thought"}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_stop",
            R"({"type":"content_block_stop","index":0})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_start",
            R"({"type":"content_block_start","index":1,"content_block":{"type":"text","text":""}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_delta",
            R"({"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"Answer"}})"
        ),
        MockAnthropicServer::sseEvent(
            "content_block_stop",
            R"({"type":"content_block_stop","index":1})"
        ),
        MockAnthropicServer::sseEvent(
            "message_delta",
            R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":5}})"
        ),
        MockAnthropicServer::sseEvent("message_stop", R"({"type":"message_stop"})"),
    };

    auto provider = server::AnthropicProvider::create(makeAntCfg("sk-ant-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "claude-sonnet-4-20250514";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Separation test"}
    };

    // Set thinking callback to separate thinking from content
    std::string thinkingAccum;
    std::string contentAccum;

    try {
        auto result = co_await provider->invoke_format_data(
            params,
            [&](const neograph::ChatStreamChunk& chunk) {
                switch (chunk.type) {
                    case neograph::ChatStreamChunk::TYPE_CONTENT: {
                        contentAccum += chunk.data;
                    } break;
                    case neograph::ChatStreamChunk::TYPE_THINKING: {
                        thinkingAccum += chunk.data;
                    } break;
                }
            }
        );
        XX_TEST_EXPECT_EQ(result.message.content, "Answer");
        XX_TEST_EXPECT_EQ(result.message.reasoning_content, "Deep thought");
        // With thinking_callback set, on_chunk should only get content
        XX_TEST_EXPECT_EQ(contentAccum, "Answer");
        XX_TEST_EXPECT_EQ(thinkingAccum, "Deep thought");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "thinking callback separation failed: " << e.what() << std::endl;
    }
}

// ---------------------------------------------------------------------------
// True streaming verification — server sends chunks with delays
// ---------------------------------------------------------------------------

static std::string antChunkFrame(const std::string& data) {
    char hexBuf[16];
    snprintf(hexBuf, sizeof(hexBuf), "%zx\r\n", data.size());
    return std::string(hexBuf) + data + "\r\n";
}

class AnthropicDelayedStreamServer {
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
            auto framed = antChunkFrame(chunk);
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

void test_anthropic_true_streaming_incremental() {
    auto srv    = std::make_unique<AnthropicDelayedStreamServer>();
    srv->chunks = {
        "event: message_start\ndata: "
        "{\"type\":\"message_start\",\"message\":{\"id\":\"msg_ts\",\"type\":"
        "\"message\",\"role\":\"assistant\",\"usage\":{\"input_tokens\":5}}}\n\n",
        "event: content_block_start\ndata: "
        "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{"
        "\"type\":\"text\",\"text\":\"\"}}\n\n",
        "event: content_block_delta\ndata: "
        "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
        "\"text_delta\",\"text\":\"X\"}}\n\n",
        "event: content_block_delta\ndata: "
        "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
        "\"text_delta\",\"text\":\"Y\"}}\n\n",
        "event: content_block_delta\ndata: "
        "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
        "\"text_delta\",\"text\":\"Z\"}}\n\n",
        "event: content_block_stop\ndata: "
        "{\"type\":\"content_block_stop\",\"index\":0}\n\n",
        "event: message_delta\ndata: "
        "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
        "\"usage\":{\"output_tokens\":3}}\n\n",
        "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n",
    };
    srv->delay = std::chrono::milliseconds(80);
    srv->start();

    std::string baseUrl  = "http://127.0.0.1:" + std::to_string(srv->boundPort);
    auto        provider = server::AnthropicProvider::create(makeAntCfg("sk-ant-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "claude-sonnet-4-20250514";
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
            XX_TEST_EXPECT_EQ(result.message.content, "XYZ");
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

class AnthropicStallServer {
public:

    enum class Mode {
        NeverReadBody,
        PartialThenStall,
        AbortAfterChunks, // 发完 partialChunks 后直接断开 (不发 chunked 终止块)
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
            auto framed = antChunkFrame(chunk);
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

void test_anthropic_connect_timeout() {
    auto provider = server::AnthropicProvider::create(
        makeAntCfg("sk-ant-test", "http://192.0.2.1:12345", 2, 2)
    );

    neograph::CompletionParams params;
    params.model    = "claude-sonnet-4-20250514";
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

void test_anthropic_read_timeout_streaming() {
    auto srv           = std::make_unique<AnthropicStallServer>();
    srv->mode          = AnthropicStallServer::Mode::PartialThenStall;
    srv->partialChunks = {
        "event: message_start\ndata: "
        "{\"type\":\"message_start\",\"message\":{\"id\":\"msg_t\",\"type\":"
        "\"message\",\"role\":\"assistant\",\"usage\":{\"input_tokens\":5}}}\n\n",
        "event: content_block_start\ndata: "
        "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{"
        "\"type\":\"text\",\"text\":\"\"}}\n\n",
        "event: content_block_delta\ndata: "
        "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
        "\"text_delta\",\"text\":\"Par\"}}\n\n",
    };
    srv->start();

    std::string baseUrl = "http://127.0.0.1:" + std::to_string(srv->boundPort);
    auto provider = server::AnthropicProvider::create(makeAntCfg("sk-ant-test", baseUrl, 5, 2));

    neograph::CompletionParams params;
    params.model    = "claude-sonnet-4-20250514";
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
                XX_TEST_EXPECT_EQ(result.message.content, "Par");
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

    XX_TEST_EXPECT_EQ(accumulated, "Par");
    XX_TEST_EXPECT_TRUE(elapsed >= 1500);
    XX_TEST_EXPECT_TRUE(elapsed < 8000);

    srv->stop();
}

/// 已收到 message_stop 后连接层报错 (如 stream_truncated/连接被中断) 不应使请求失败,
/// 应忽略传输错误并返回已完整接收的结果
void test_anthropic_streaming_abort_after_message_stop_ignored() {
    auto srv           = std::make_unique<AnthropicStallServer>();
    srv->mode          = AnthropicStallServer::Mode::AbortAfterChunks;
    srv->partialChunks = {
        "event: message_start\ndata: "
        "{\"type\":\"message_start\",\"message\":{\"id\":\"msg_a\",\"type\":"
        "\"message\",\"role\":\"assistant\",\"usage\":{\"input_tokens\":4}}}\n\n",
        "event: content_block_start\ndata: "
        "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{"
        "\"type\":\"text\",\"text\":\"\"}}\n\n",
        "event: content_block_delta\ndata: "
        "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
        "\"text_delta\",\"text\":\"Full\"}}\n\n",
        "event: content_block_delta\ndata: "
        "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
        "\"text_delta\",\"text\":\" text\"}}\n\n",
        "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n",
        "event: message_delta\ndata: "
        "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
        "\"usage\":{\"output_tokens\":2}}\n\n",
        "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n",
        // 无 chunked 终止块, 服务端直接断开 → 客户端传输层报错, 但 message_stop 已收到
    };
    srv->start();

    std::string baseUrl = "http://127.0.0.1:" + std::to_string(srv->boundPort);
    auto provider = server::AnthropicProvider::create(makeAntCfg("sk-ant-test", baseUrl, 5, 5));

    neograph::CompletionParams params;
    params.model    = "claude-sonnet-4-20250514";
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
                TEST_FAIL << "abort-after-message_stop should be ignored, but threw: " << e.what()
                          << std::endl;
            }
        },
        asio::detached
    );
    ctx.run();

    XX_TEST_EXPECT_FALSE(threw);
    XX_TEST_EXPECT_EQ(content, "Full text");
    XX_TEST_EXPECT_EQ(accumulated, "Full text");
    XX_TEST_EXPECT_EQ(totalTokens, 6);

    srv->stop();
}

void test_anthropic_send_timeout_calculation() {
    XX_TEST_EXPECT_EQ(agentxx::util::HttpClient::calcTimeoutBySize(0).count(), 30);
    XX_TEST_EXPECT_EQ(agentxx::util::HttpClient::calcTimeoutBySize(1024).count(), 30);
    XX_TEST_EXPECT_EQ(agentxx::util::HttpClient::calcTimeoutBySize(65536 * 30).count(), 30);
    XX_TEST_EXPECT_EQ(agentxx::util::HttpClient::calcTimeoutBySize(65536 * 31).count(), 31);
    XX_TEST_EXPECT_EQ(agentxx::util::HttpClient::calcTimeoutBySize(65536 * 500).count(), 500);
}

// ---------------------------------------------------------------------------
// Test runner
// ---------------------------------------------------------------------------

/// 直接单测 SSE 解析: 多 data: 行拼接 / 冒号后无空格 / 末尾无 "\n\n" 的事件
void test_anthropic_sse_parsing_edge_cases() {
    using server::AnthropicProvider;

    // B2: "data:" / "event:" 冒号后无单个空格也必须解析
    {
        std::string buf
            = "event:content_block_delta\n"
              "data:{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_"
              "delta\",\"text\":\"Hi\"}}\n\n";
        neograph::ChatCompletion          completion;
        std::string                       content, thinking;
        std::map<int, neograph::ToolCall> tcMap;
        std::map<int, std::string>        blockTypes;
        std::map<int, std::string>        thinkingTexts;
        std::map<int, std::string>        blockSignatures;
        AnthropicProvider::processSseBuffer(
            buf,
            completion,
            content,
            thinking,
            tcMap,
            blockTypes,
            thinkingTexts,
            blockSignatures,
            nullptr
        );
        XX_TEST_EXPECT_EQ(content, "Hi");
    }

    // B1: 同一事件的多个 data: 行应按 SSE 规范以 "\n" 拼接后再解析
    {
        std::string                       buf = "event: content_block_delta\n"
                                                "data: {\"type\":\"content_block_delta\",\"index\":0,\n"
                                                "data: \"delta\":{\"type\":\"text_delta\",\"text\":\"AB\"}}\n\n";
        neograph::ChatCompletion          completion;
        std::string                       content, thinking;
        std::map<int, neograph::ToolCall> tcMap;
        std::map<int, std::string>        blockTypes;
        std::map<int, std::string>        thinkingTexts;
        std::map<int, std::string>        blockSignatures;
        AnthropicProvider::processSseBuffer(
            buf,
            completion,
            content,
            thinking,
            tcMap,
            blockTypes,
            thinkingTexts,
            blockSignatures,
            nullptr
        );
        // 修复前仅保留最后一个 data: 行 -> 非法 JSON -> content 为空
        XX_TEST_EXPECT_EQ(content, "AB");
    }

    // B3: 连接关闭时末尾未以 "\n\n" 结尾的事件, finalFlush=true 时应补解析
    {
        std::string buf
            = "event: content_block_delta\n"
              "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_"
              "delta\",\"text\":\"End\"}}"; // 无 trailing "\n\n"
        neograph::ChatCompletion          completion;
        std::string                       content, thinking;
        std::map<int, neograph::ToolCall> tcMap;
        std::map<int, std::string>        blockTypes;
        std::map<int, std::string>        thinkingTexts;
        std::map<int, std::string>        blockSignatures;
        // finalFlush=false: 事件不完整, 暂不解析
        AnthropicProvider::processSseBuffer(
            buf,
            completion,
            content,
            thinking,
            tcMap,
            blockTypes,
            thinkingTexts,
            blockSignatures,
            nullptr,
            /*finalFlush=*/false
        );
        XX_TEST_EXPECT_EQ(content, "");
        // finalFlush=true: 解析末尾块
        AnthropicProvider::processSseBuffer(
            buf,
            completion,
            content,
            thinking,
            tcMap,
            blockTypes,
            thinkingTexts,
            blockSignatures,
            nullptr,
            /*finalFlush=*/true
        );
        XX_TEST_EXPECT_EQ(content, "End");
    }
}

/// tool 消息映射为 user 后可能出现连续同 role, 必须合并以满足 Anthropic 的交替 role 要求
void test_convert_messages_merges_consecutive_roles() {
    std::vector<neograph::ChatMessage> msgs = {
        {.role = "user", .content = "Q?"},
        {.role    = "assistant",
         .content = "",
         .tool_calls
         = {{.id = "c1", .name = "t1", .arguments = "{}"},
            {.id = "c2", .name = "t2", .arguments = "{}"}}},
        {.role = "tool", .content = "R1", .tool_call_id = "c1"},
        {.role = "tool", .content = "R2", .tool_call_id = "c2"},
        {.role = "user", .content = "Follow-up"},
    };
    auto [system, arr] = server::AnthropicProvider::convertMessages(msgs);
    // user, assistant, user (合并: 2 个 tool_result + follow-up 文本)
    XX_TEST_EXPECT_EQ(arr.size(), (size_t)3);
    XX_TEST_EXPECT_EQ(arr[2]["role"].get<std::string>(), "user");
    XX_TEST_EXPECT_TRUE(arr[2]["content"].is_array());
    XX_TEST_EXPECT_EQ(arr[2]["content"].size(), (size_t)3);
    XX_TEST_EXPECT_EQ(arr[2]["content"][0]["type"].get<std::string>(), "tool_result");
    XX_TEST_EXPECT_EQ(arr[2]["content"][0]["tool_use_id"].get<std::string>(), "c1");
    XX_TEST_EXPECT_EQ(arr[2]["content"][1]["type"].get<std::string>(), "tool_result");
    XX_TEST_EXPECT_EQ(arr[2]["content"][1]["tool_use_id"].get<std::string>(), "c2");
    XX_TEST_EXPECT_EQ(arr[2]["content"][2]["type"].get<std::string>(), "text");
    XX_TEST_EXPECT_EQ(arr[2]["content"][2]["text"].get<std::string>(), "Follow-up");
}

/// sendThinking 时应优先原样回传从响应中捕获的带 signature 的 thinking 块
void test_convert_messages_thinking_signature_roundtrip() {
    neograph::ChatMessage msg;
    msg.role                                                 = "assistant";
    msg.content                                              = "Answer";
    msg.reasoning_content                                    = "Thinking text";
    msg.extra[server::AnthropicProvider::kThinkingBlocksKey] = neograph::json::parse(R"([
        {"type":"thinking","thinking":"Thinking text","signature":"sig123"},
        {"type":"redacted_thinking","data":"redacted-data"}
    ])");
    std::vector<neograph::ChatMessage> msgs                  = {msg};
    auto [system, arr] = server::AnthropicProvider::convertMessages(msgs, true);
    XX_TEST_EXPECT_TRUE(arr[0]["content"].is_array());
    const auto& blocks = arr[0]["content"];
    XX_TEST_EXPECT_EQ(blocks.size(), (size_t)3);
    XX_TEST_EXPECT_EQ(blocks[0]["type"].get<std::string>(), "thinking");
    XX_TEST_EXPECT_EQ(blocks[0]["signature"].get<std::string>(), "sig123");
    XX_TEST_EXPECT_EQ(blocks[1]["type"].get<std::string>(), "redacted_thinking");
    XX_TEST_EXPECT_EQ(blocks[1]["data"].get<std::string>(), "redacted-data");
    XX_TEST_EXPECT_EQ(blocks[2]["type"].get<std::string>(), "text");
    XX_TEST_EXPECT_EQ(blocks[2]["text"].get<std::string>(), "Answer");
}

/// 多模态消息: 图片/音频/视频附件应转换为 image/audio/video 内容块
/// - data URL → base64 source (自动推导 media_type)
/// - HTTP URL → url source
void test_convert_messages_multimodal() {
    std::vector<neograph::ChatMessage> msgs = {
        {.role        = "user",
         .content     = "看看这个视频",
         .image_urls  = {"https://example.com/a.png"},
         .audio_urls  = {"data:audio/wav;base64,UklGRg=="},
         .video_urls  = {"data:video/mp4;base64,AAAA", "https://example.com/b.mp4"}},
    };
    auto [system, arr] = server::AnthropicProvider::convertMessages(msgs);
    XX_TEST_EXPECT_TRUE(system.empty());
    XX_TEST_EXPECT_EQ(arr.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(arr[0]["role"].get<std::string>(), "user");
    XX_TEST_EXPECT_TRUE(arr[0]["content"].is_array());

    const auto& blocks = arr[0]["content"];
    // text + image + audio + video x2
    XX_TEST_EXPECT_EQ(blocks.size(), (size_t)5);

    // text
    XX_TEST_EXPECT_EQ(blocks[0]["type"].get<std::string>(), "text");
    XX_TEST_EXPECT_EQ(blocks[0]["text"].get<std::string>(), "看看这个视频");
    // image (HTTP URL → url source)
    XX_TEST_EXPECT_EQ(blocks[1]["type"].get<std::string>(), "image");
    XX_TEST_EXPECT_EQ(blocks[1]["source"]["type"].get<std::string>(), "url");
    XX_TEST_EXPECT_EQ(blocks[1]["source"]["url"].get<std::string>(), "https://example.com/a.png");
    // audio (data URL → base64 source)
    XX_TEST_EXPECT_EQ(blocks[2]["type"].get<std::string>(), "audio");
    XX_TEST_EXPECT_EQ(blocks[2]["source"]["type"].get<std::string>(), "base64");
    XX_TEST_EXPECT_EQ(blocks[2]["source"]["media_type"].get<std::string>(), "audio/wav");
    XX_TEST_EXPECT_EQ(blocks[2]["source"]["data"].get<std::string>(), "UklGRg==");
    // video (data URL → base64 source)
    XX_TEST_EXPECT_EQ(blocks[3]["type"].get<std::string>(), "video");
    XX_TEST_EXPECT_EQ(blocks[3]["source"]["type"].get<std::string>(), "base64");
    XX_TEST_EXPECT_EQ(blocks[3]["source"]["media_type"].get<std::string>(), "video/mp4");
    XX_TEST_EXPECT_EQ(blocks[3]["source"]["data"].get<std::string>(), "AAAA");
    // video (HTTP URL → url source)
    XX_TEST_EXPECT_EQ(blocks[4]["type"].get<std::string>(), "video");
    XX_TEST_EXPECT_EQ(blocks[4]["source"]["type"].get<std::string>(), "url");
    XX_TEST_EXPECT_EQ(blocks[4]["source"]["url"].get<std::string>(), "https://example.com/b.mp4");
}

/// 多模态消息: 无文本时不应产生 text 块
void test_convert_messages_multimodal_no_text() {
    std::vector<neograph::ChatMessage> msgs = {
        {.role       = "user",
         .audio_urls = {"data:audio/mpeg;base64,SUQzBAAAAA=="}},
    };
    auto [system, arr] = server::AnthropicProvider::convertMessages(msgs);
    const auto& blocks = arr[0]["content"];
    XX_TEST_EXPECT_EQ(blocks.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(blocks[0]["type"].get<std::string>(), "audio");
    XX_TEST_EXPECT_EQ(blocks[0]["source"]["media_type"].get<std::string>(), "audio/mpeg");
}

/// 多模态消息合并: 相邻同 role 多模态消息的 content 块应正确拼接
void test_convert_messages_multimodal_merge() {
    std::vector<neograph::ChatMessage> msgs = {
        {.role       = "user",
         .content    = "图1",
         .image_urls = {"https://example.com/1.png"}},
        {.role       = "user",
         .content    = "图2",
         .image_urls = {"https://example.com/2.png"}},
    };
    auto [system, arr] = server::AnthropicProvider::convertMessages(msgs);
    // 相邻同 role 合并为一条
    XX_TEST_EXPECT_EQ(arr.size(), (size_t)1);
    const auto& blocks = arr[0]["content"];
    XX_TEST_EXPECT_TRUE(blocks.is_array());
    XX_TEST_EXPECT_EQ(blocks.size(), (size_t)4);
    XX_TEST_EXPECT_EQ(blocks[0]["type"].get<std::string>(), "text");
    XX_TEST_EXPECT_EQ(blocks[0]["text"].get<std::string>(), "图1");
    XX_TEST_EXPECT_EQ(blocks[1]["type"].get<std::string>(), "image");
    XX_TEST_EXPECT_EQ(blocks[1]["source"]["url"].get<std::string>(), "https://example.com/1.png");
    XX_TEST_EXPECT_EQ(blocks[2]["type"].get<std::string>(), "text");
    XX_TEST_EXPECT_EQ(blocks[2]["text"].get<std::string>(), "图2");
    XX_TEST_EXPECT_EQ(blocks[3]["type"].get<std::string>(), "image");
    XX_TEST_EXPECT_EQ(blocks[3]["source"]["url"].get<std::string>(), "https://example.com/2.png");
}

/// 非流式响应解析: 带 signature 的 thinking 块与 redacted_thinking 块应存入 extra 以便回传
void test_parse_response_thinking_signature() {
    auto resp       = neograph::json::parse(R"({
        "content": [
            {"type":"thinking","thinking":"Hmm...","signature":"sig-abc"},
            {"type":"redacted_thinking","data":"EQo=="},
            {"type":"text","text":"Ok"}
        ],
        "usage": {"input_tokens": 1, "output_tokens": 1}
    })");
    auto completion = server::AnthropicProvider::parseResponse(resp);
    XX_TEST_EXPECT_EQ(completion.message.reasoning_content, "Hmm...");
    XX_TEST_EXPECT_EQ(completion.message.content, "Ok");
    XX_TEST_EXPECT_TRUE(
        completion.message.extra.contains(server::AnthropicProvider::kThinkingBlocksKey)
    );
    const auto& blocks = completion.message.extra[server::AnthropicProvider::kThinkingBlocksKey];
    XX_TEST_EXPECT_EQ(blocks.size(), (size_t)2);
    XX_TEST_EXPECT_EQ(blocks[0]["type"].get<std::string>(), "thinking");
    XX_TEST_EXPECT_EQ(blocks[0]["signature"].get<std::string>(), "sig-abc");
    XX_TEST_EXPECT_EQ(blocks[1]["type"].get<std::string>(), "redacted_thinking");
    XX_TEST_EXPECT_EQ(blocks[1]["data"].get<std::string>(), "EQo==");
}

/// 流式 SSE: signature_delta 累积, content_block_stop 时组装带 signature 的 thinking 块
void test_anthropic_sse_signature_capture() {
    using server::AnthropicProvider;
    std::string buf
        = "event: content_block_start\n"
          "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\","
          "\"thinking\":\"\"}}\n\n"
          "event: content_block_delta\n"
          "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\","
          "\"thinking\":\"Let me\"}}\n\n"
          "event: content_block_delta\n"
          "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\","
          "\"thinking\":\" think\"}}\n\n"
          "event: content_block_delta\n"
          "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\","
          "\"signature\":\"sig-xyz\"}}\n\n"
          "event: content_block_stop\n"
          "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n";
    neograph::ChatCompletion          completion;
    std::string                       content, thinking;
    std::map<int, neograph::ToolCall> tcMap;
    std::map<int, std::string>        blockTypes;
    std::map<int, std::string>        thinkingTexts;
    std::map<int, std::string>        blockSignatures;
    AnthropicProvider::processSseBuffer(
        buf,
        completion,
        content,
        thinking,
        tcMap,
        blockTypes,
        thinkingTexts,
        blockSignatures,
        nullptr
    );
    XX_TEST_EXPECT_EQ(thinking, "Let me think");
    XX_TEST_EXPECT_TRUE(completion.message.extra.contains(AnthropicProvider::kThinkingBlocksKey));
    const auto& blocks = completion.message.extra[AnthropicProvider::kThinkingBlocksKey];
    XX_TEST_EXPECT_EQ(blocks.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(blocks[0]["type"].get<std::string>(), "thinking");
    XX_TEST_EXPECT_EQ(blocks[0]["thinking"].get<std::string>(), "Let me think");
    XX_TEST_EXPECT_EQ(blocks[0]["signature"].get<std::string>(), "sig-xyz");
}

/// 事件分隔符为 "\r\n\r\n" (部分代理使用 CRLF 行结尾) 时也必须正确切分
void test_anthropic_sse_crlf_separator() {
    using server::AnthropicProvider;
    std::string buf
        = "event: content_block_delta\r\n"
          "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\","
          "\"text\":\"CRLF\"}}\r\n\r\n"
          "event: content_block_delta\r\n"
          "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\","
          "\"text\":\" ok\"}}\r\n\r\n";
    neograph::ChatCompletion          completion;
    std::string                       content, thinking;
    std::map<int, neograph::ToolCall> tcMap;
    std::map<int, std::string>        blockTypes;
    std::map<int, std::string>        thinkingTexts;
    std::map<int, std::string>        blockSignatures;
    AnthropicProvider::processSseBuffer(
        buf,
        completion,
        content,
        thinking,
        tcMap,
        blockTypes,
        thinkingTexts,
        blockSignatures,
        nullptr
    );
    XX_TEST_EXPECT_EQ(content, "CRLF ok");
    XX_TEST_EXPECT_TRUE(buf.empty());
}

/// processSseBuffer 返回值: 仅当处理到 "message_stop" 事件时为 true (流截断检测依据)
void test_anthropic_sse_message_stop_flag() {
    using server::AnthropicProvider;

    auto run = [](const std::string& input, bool finalFlush = false) {
        std::string                       buf = input;
        neograph::ChatCompletion          completion;
        std::string                       content, thinking;
        std::map<int, neograph::ToolCall> tcMap;
        std::map<int, std::string>        blockTypes, thinkingTexts, blockSignatures;
        return AnthropicProvider::processSseBuffer(
            buf,
            completion,
            content,
            thinking,
            tcMap,
            blockTypes,
            thinkingTexts,
            blockSignatures,
            nullptr,
            finalFlush
        );
    };

    // 普通事件 → false
    XX_TEST_EXPECT_FALSE(run(
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\","
        "\"text\":\"Hi\"}}\n\n"
    ));

    // 含 message_stop → true
    XX_TEST_EXPECT_TRUE(
        run("event: message_delta\n"
            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
            "\"usage\":{\"output_tokens\":1}}\n\n"
            "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n")
    );

    // message_stop 无结尾分隔符, finalFlush 时也应识别
    XX_TEST_EXPECT_TRUE(run("event: message_stop\ndata: {\"type\":\"message_stop\"}", true));
}

asio::awaitable<TestResult> run_anthropic_provider_tests() {
    g_anthropic_passed = 0;
    g_anthropic_failed = 0;

    // Unit tests
    test_anthropic_factory_and_name();
    test_anthropic_config_defaults();
    test_convert_messages_basic();
    test_convert_messages_system_extraction();
    test_convert_messages_multiple_system();
    test_convert_messages_tool_result();
    test_convert_messages_assistant_with_text_and_tool();
    test_convert_messages_thinking_enabled();
    test_convert_messages_thinking_disabled();
    test_convert_messages_thinking_default_disabled();
    test_convert_messages_thinking_with_tool_calls();
    test_convert_messages_thinking_disabled_with_tool_calls();
    test_convert_messages_thinking_only_reasoning_no_content();
    test_convert_messages_thinking_user_no_effect();
    test_convert_tools();
    test_parse_response_text();
    test_parse_response_tool_use();
    test_parse_response_thinking();
    test_parse_response_mixed();
    test_parse_response_usage();
    test_anthropic_sse_parsing_edge_cases();
    test_convert_messages_merges_consecutive_roles();
    test_convert_messages_thinking_signature_roundtrip();
    test_convert_messages_multimodal();
    test_convert_messages_multimodal_no_text();
    test_convert_messages_multimodal_merge();
    test_parse_response_thinking_signature();
    test_anthropic_sse_signature_capture();
    test_anthropic_sse_crlf_separator();
    test_anthropic_sse_message_stop_flag();

    // Integration tests
    uint16_t port = 0;
    auto     mock = startAnthropicMockServer(port);
    if (!mock || port == 0) {
        TEST_FAIL << "Failed to start Anthropic mock server" << std::endl;
        g_anthropic_failed++;
        co_return TestResult{g_anthropic_passed, g_anthropic_failed};
    }

    std::cout << "Mock Anthropic server on port " << port << std::endl;

    co_await test_non_streaming_completion(*mock, port);
    co_await test_non_streaming_tool_call(*mock, port);
    co_await test_non_streaming_thinking(*mock, port);
    co_await test_rate_limit_error(*mock, port);
    co_await test_server_error(*mock, port);
    co_await test_request_headers(*mock, port);
    co_await test_request_body_format(*mock, port);
    co_await test_streaming_completion(*mock, port);
    co_await test_streaming_thinking(*mock, port);
    co_await test_streaming_tool_call(*mock, port);
    co_await test_streaming_mixed_thinking_and_content(*mock, port);
    co_await test_streaming_usage(*mock, port);
    co_await test_streaming_malformed_event_skipped(*mock, port);
    co_await test_streaming_missing_message_stop_throws(*mock, port);
    co_await test_sendthinking_in_request_body(*mock, port);
    co_await test_thinking_callback_separation(*mock, port);

    // True streaming incremental verification
    test_anthropic_true_streaming_incremental();

    // Timeout tests
    test_anthropic_send_timeout_calculation();
    test_anthropic_connect_timeout();
    test_anthropic_read_timeout_streaming();
    test_anthropic_streaming_abort_after_message_stop_ignored();

    mock->server->stop();
    mock->thread.join();

    co_return TestResult{g_anthropic_passed, g_anthropic_failed};
}

} // namespace test
} // namespace agentxx
