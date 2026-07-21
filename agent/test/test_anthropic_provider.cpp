#include "test_anthropic_provider.h"
#include "agentxx/protocol/anthropic_provider.h"
#include "agentxx/util/http_client.h"
#include "agentxx/util/http_server.h"
#include <asio/awaitable.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <format>
#include <iostream>
#include <memory>
#include <thread>

namespace agentxx {
namespace test {

using namespace agentxx::util;
namespace server = agentxx::server;

int g_anthropic_passed = 0;
int g_anthropic_failed = 0;

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

void test_anthropic_factory_and_name() {
  {
    auto p = server::AnthropicProvider::create({.api_key = "sk-ant-test"});
    XX_TEST_EXPECT_TRUE(p != nullptr);
    XX_TEST_EXPECT_EQ(p->get_name(), "anthropic");
  }
  {
    auto p =
        server::AnthropicProvider::create_shared({.api_key = "sk-ant-shared"});
    XX_TEST_EXPECT_TRUE(p != nullptr);
    XX_TEST_EXPECT_EQ(p->get_name(), "anthropic");
  }
}

void test_anthropic_config_defaults() {
  auto p = server::AnthropicProvider::create({.api_key = "sk-ant-defaults"});
  XX_TEST_EXPECT_TRUE(p != nullptr);
  XX_TEST_EXPECT_EQ(p->get_name(), "anthropic");
}

void test_convert_messages_basic() {
  std::vector<neograph::ChatMessage> msgs = {
      {.role = "user", .content = "Hello"},
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
      {.role = "user", .content = "Hello"},
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
      {.role = "user", .content = "Hi"},
  };
  auto [system, arr] = server::AnthropicProvider::convertMessages(msgs);
  XX_TEST_EXPECT_EQ(system, "Rule 1.\nRule 2.");
  XX_TEST_EXPECT_EQ(arr.size(), (size_t)1);
}

void test_convert_messages_tool_result() {
  std::vector<neograph::ChatMessage> msgs = {
      {.role = "user", .content = "What is the weather?"},
      {.role = "assistant",
       .content = "",
       .tool_calls = {{.id = "call_1",
                       .name = "get_weather",
                       .arguments = R"({"location":"Tokyo"})"}}},
      {.role = "tool", .content = "Sunny, 25C", .tool_call_id = "call_1"},
  };
  auto [system, arr] = server::AnthropicProvider::convertMessages(msgs);
  XX_TEST_EXPECT_EQ(arr.size(), (size_t)3);

  // Check tool_use block in assistant message
  const auto &assistantMsg = arr[1];
  XX_TEST_EXPECT_EQ(assistantMsg["role"].get<std::string>(), "assistant");
  XX_TEST_EXPECT_TRUE(assistantMsg["content"].is_array());
  const auto &blocks = assistantMsg["content"];
  XX_TEST_EXPECT_EQ(blocks.size(), (size_t)1);
  XX_TEST_EXPECT_EQ(blocks[0]["type"].get<std::string>(), "tool_use");
  XX_TEST_EXPECT_EQ(blocks[0]["id"].get<std::string>(), "call_1");
  XX_TEST_EXPECT_EQ(blocks[0]["name"].get<std::string>(), "get_weather");
  XX_TEST_EXPECT_EQ(blocks[0]["input"]["location"].get<std::string>(), "Tokyo");

  // Check tool_result block
  const auto &toolMsg = arr[2];
  XX_TEST_EXPECT_EQ(toolMsg["role"].get<std::string>(), "user");
  XX_TEST_EXPECT_TRUE(toolMsg["content"].is_array());
  const auto &toolBlocks = toolMsg["content"];
  XX_TEST_EXPECT_EQ(toolBlocks[0]["type"].get<std::string>(), "tool_result");
  XX_TEST_EXPECT_EQ(toolBlocks[0]["tool_use_id"].get<std::string>(), "call_1");
  XX_TEST_EXPECT_EQ(toolBlocks[0]["content"].get<std::string>(), "Sunny, 25C");
}

void test_convert_messages_assistant_with_text_and_tool() {
  std::vector<neograph::ChatMessage> msgs = {
      {.role = "assistant",
       .content = "Let me check.",
       .tool_calls = {{.id = "call_2",
                       .name = "search",
                       .arguments = R"({"q":"test"})"}}},
  };
  auto [system, arr] = server::AnthropicProvider::convertMessages(msgs);
  XX_TEST_EXPECT_EQ(arr.size(), (size_t)1);
  const auto &blocks = arr[0]["content"];
  XX_TEST_EXPECT_EQ(blocks.size(), (size_t)2);
  XX_TEST_EXPECT_EQ(blocks[0]["type"].get<std::string>(), "text");
  XX_TEST_EXPECT_EQ(blocks[0]["text"].get<std::string>(), "Let me check.");
  XX_TEST_EXPECT_EQ(blocks[1]["type"].get<std::string>(), "tool_use");
}

void test_convert_tools() {
  std::vector<neograph::ChatTool> tools = {
      {.name = "get_weather",
       .description = "Get weather",
       .parameters = neograph::json::parse(
           R"({"type":"object","properties":{"location":{"type":"string"}}})")},
  };
  auto arr = server::AnthropicProvider::convertTools(tools);
  XX_TEST_EXPECT_EQ(arr.size(), (size_t)1);
  XX_TEST_EXPECT_EQ(arr[0]["name"].get<std::string>(), "get_weather");
  XX_TEST_EXPECT_EQ(arr[0]["description"].get<std::string>(), "Get weather");
  XX_TEST_EXPECT_TRUE(arr[0].contains("input_schema"));
  XX_TEST_EXPECT_EQ(arr[0]["input_schema"]["type"].get<std::string>(),
                    "object");
  XX_TEST_EXPECT_FALSE(arr[0].contains("parameters"));
}

void test_parse_response_text() {
  auto resp = neograph::json::parse(R"({
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
  auto resp = neograph::json::parse(R"({
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
  XX_TEST_EXPECT_TRUE(completion.message.tool_calls[0].arguments.find(
                          "Tokyo") != std::string::npos);
}

void test_parse_response_thinking() {
  auto resp = neograph::json::parse(R"({
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
  XX_TEST_EXPECT_EQ(completion.message.reasoning_content,
                    "Let me reason step by step...");
}

void test_parse_response_mixed() {
  auto resp = neograph::json::parse(R"({
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
  auto resp = neograph::json::parse(R"({
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
  std::thread thread;
  AnthropicMockMode mode = AnthropicMockMode::Normal;
  std::string lastRequestBody;
  std::string lastRequestHeaders;

  std::vector<std::string> sseChunks;
  std::optional<neograph::json> customResponse;

  static std::string sseEvent(std::string_view event, std::string_view data) {
    return "event: " + std::string(event) + "\ndata: " + std::string(data) +
           "\n\n";
  }

  neograph::json makeTextResponse(std::string_view content, int inputTok = 10,
                                  int outputTok = 5) const {
    return neograph::json::parse(R"({
      "id": "msg_mock",
      "type": "message",
      "role": "assistant",
      "content": [{"type": "text", "text": ")" +
                                 std::string(content) + R"("}],
      "stop_reason": "end_turn",
      "usage": {"input_tokens": )" +
                                 std::to_string(inputTok) +
                                 R"(, "output_tokens": )" +
                                 std::to_string(outputTok) + R"(}
    })");
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

std::unique_ptr<MockAnthropicServer>
startAnthropicMockServer(uint16_t &outPort) {
  auto mock = std::make_unique<MockAnthropicServer>();

  // Default streaming chunks
  mock->sseChunks = {
      MockAnthropicServer::sseEvent(
          "message_start",
          R"({"type":"message_start","message":{"id":"msg_stream","type":"message","role":"assistant","usage":{"input_tokens":5}}})"),
      MockAnthropicServer::sseEvent(
          "content_block_start",
          R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"),
      MockAnthropicServer::sseEvent(
          "content_block_delta",
          R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hello"}})"),
      MockAnthropicServer::sseEvent(
          "content_block_delta",
          R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":" world"}})"),
      MockAnthropicServer::sseEvent(
          "content_block_stop", R"({"type":"content_block_stop","index":0})"),
      MockAnthropicServer::sseEvent(
          "message_delta",
          R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":3}})"),
      MockAnthropicServer::sseEvent("message_stop",
                                    R"({"type":"message_stop"})"),
  };

  mock->server = std::make_unique<HttpServer>(
      HttpServer::Config{.address = "127.0.0.1", .port = 0, .ioThreads = 1});

  mock->server->router().add("/v1/messages", 2,
                             std::
                                 make_shared<HttpServer::Handler>(
                                     [mock =
                                          mock.get()](HttpServer::Request &req,
                                                      HttpServer::Response
                                                          &resp,
                                                      const std::string &) -> asio::
                                                                               awaitable<
                                                                                   void> {
                                                                                 mock->lastRequestBody =
                                                                                     req.body();
                                                                                 // Capture headers for verification
                                                                                 std::string
                                                                                     headers;
                                                                                 for (
                                                                                     const auto
                                                                                         &field :
                                                                                     req) {
                                                                                   headers +=
                                                                                       std::string(
                                                                                           field
                                                                                               .name_string()) +
                                                                                       ": " +
                                                                                       std::string(
                                                                                           field
                                                                                               .value()) +
                                                                                       "\n";
                                                                                 }
                                                                                 mock->lastRequestHeaders =
                                                                                     headers;

                                                                                 switch (
                                                                                     mock->mode) {
                                                                                 case AnthropicMockMode::
                                                                                     RateLimit:
                                                                                   resp.result(
                                                                                       boost::beast::
                                                                                           http::status::
                                                                                               too_many_requests);
                                                                                   resp.set(
                                                                                       boost::beast::
                                                                                           http::field::
                                                                                               content_type,
                                                                                       "application/json");
                                                                                   resp.set(
                                                                                       boost::beast::
                                                                                           http::field::
                                                                                               retry_after,
                                                                                       "7");
                                                                                   resp.body() =
                                                                                       R"({"type":"error","error":{"type":"rate_limit_error","message":"Rate limit exceeded"}})";
                                                                                   resp.prepare_payload();
                                                                                   break;

                                                                                 case AnthropicMockMode::
                                                                                     ServerError:
                                                                                   resp.result(
                                                                                       boost::beast::
                                                                                           http::status::
                                                                                               internal_server_error);
                                                                                   resp.set(
                                                                                       boost::beast::
                                                                                           http::field::
                                                                                               content_type,
                                                                                       "application/json");
                                                                                   resp.body() =
                                                                                       R"({"type":"error","error":{"type":"api_error","message":"Internal error"}})";
                                                                                   resp.prepare_payload();
                                                                                   break;

                                                                                 case AnthropicMockMode::
                                                                                     ToolCall:
                                                                                   resp.result(
                                                                                       boost::beast::
                                                                                           http::status::
                                                                                               ok);
                                                                                   resp.set(
                                                                                       boost::beast::
                                                                                           http::field::
                                                                                               content_type,
                                                                                       "application/json");
                                                                                   resp.body() =
                                                                                       mock->makeToolCallResponse()
                                                                                           .dump();
                                                                                   resp.prepare_payload();
                                                                                   break;

                                                                                 case AnthropicMockMode::
                                                                                     Thinking:
                                                                                   resp.result(
                                                                                       boost::beast::
                                                                                           http::status::
                                                                                               ok);
                                                                                   resp.set(
                                                                                       boost::beast::
                                                                                           http::field::
                                                                                               content_type,
                                                                                       "application/json");
                                                                                   resp.body() =
                                                                                       mock->makeThinkingResponse()
                                                                                           .dump();
                                                                                   resp.prepare_payload();
                                                                                   break;

                                                                                 case AnthropicMockMode::
                                                                                     Streaming:
                                                                                 case AnthropicMockMode::
                                                                                     StreamingThinking:
                                                                                 case AnthropicMockMode::
                                                                                     StreamingToolCall: {
                                                                                   resp.result(
                                                                                       boost::beast::
                                                                                           http::status::
                                                                                               ok);
                                                                                   resp.set(
                                                                                       boost::beast::
                                                                                           http::field::
                                                                                               content_type,
                                                                                       "text/event-stream");
                                                                                   resp.set(
                                                                                       boost::beast::
                                                                                           http::field::
                                                                                               cache_control,
                                                                                       "no-cache");
                                                                                   resp.keep_alive(
                                                                                       false);
                                                                                   std::string
                                                                                       sseBody;
                                                                                   for (
                                                                                       const auto
                                                                                           &chunk :
                                                                                       mock->sseChunks) {
                                                                                     sseBody +=
                                                                                         chunk;
                                                                                   }
                                                                                   resp.body() =
                                                                                       sseBody;
                                                                                   resp.prepare_payload();
                                                                                   break;
                                                                                 }

                                                                                 case AnthropicMockMode::
                                                                                     Normal:
                                                                                 default:
                                                                                   resp.result(
                                                                                       boost::beast::
                                                                                           http::status::
                                                                                               ok);
                                                                                   resp.set(
                                                                                       boost::beast::
                                                                                           http::field::
                                                                                               content_type,
                                                                                       "application/json");
                                                                                   if (mock->customResponse
                                                                                           .has_value()) {
                                                                                     resp.body() =
                                                                                         mock->customResponse
                                                                                             ->dump();
                                                                                     mock->customResponse
                                                                                         .reset();
                                                                                   } else {
                                                                                     resp.body() =
                                                                                         mock->makeTextResponse(
                                                                                                 "Hello from Claude!")
                                                                                             .dump();
                                                                                   }
                                                                                   resp.prepare_payload();
                                                                                   break;
                                                                                 }
                                                                                 co_return;
                                                                               }));

  mock->thread = std::thread([s = mock->server.get()]() { s->start(); });

  for (int i = 0; i < 100; ++i) {
    outPort = mock->server->port();
    if (outPort != 0)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (outPort == 0) {
    mock->server->stop();
    if (mock->thread.joinable())
      mock->thread.join();
    return nullptr;
  }

  for (int i = 0; i < 100; ++i) {
    try {
      asio::io_context tmpCtx;
      asio::ip::tcp::socket sock(tmpCtx);
      sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"),
                                           outPort));
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

asio::awaitable<void> test_non_streaming_completion(MockAnthropicServer &mock,
                                                    uint16_t port) {
  std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
  mock.mode = AnthropicMockMode::Normal;

  auto provider = server::AnthropicProvider::create(
      {.api_key = "sk-ant-test", .base_url = baseUrl, .timeout_seconds = 10});

  neograph::CompletionParams params;
  params.model = "claude-sonnet-4-20250514";
  params.messages = {
      neograph::ChatMessage{.role = "user", .content = "Say hello"}};

  try {
    auto result = co_await provider->invoke(params, nullptr);
    XX_TEST_EXPECT_EQ(result.message.role, "assistant");
    XX_TEST_EXPECT_TRUE(result.message.content.find("Hello") !=
                        std::string::npos);
    XX_TEST_EXPECT_TRUE(result.usage.total_tokens > 0);

    auto sent = neograph::json::parse(mock.lastRequestBody);
    XX_TEST_EXPECT_EQ(sent["model"].get<std::string>(),
                      "claude-sonnet-4-20250514");
    XX_TEST_EXPECT_TRUE(sent.contains("messages"));
    XX_TEST_EXPECT_TRUE(sent.contains("max_tokens"));
  } catch (const std::exception &e) {
    XX_TEST_FAILED++;
    TEST_FAIL << "non-streaming completion failed: " << e.what() << std::endl;
  }
}

asio::awaitable<void> test_non_streaming_tool_call(MockAnthropicServer &mock,
                                                   uint16_t port) {
  std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
  mock.mode = AnthropicMockMode::ToolCall;

  auto provider = server::AnthropicProvider::create(
      {.api_key = "sk-ant-test", .base_url = baseUrl, .timeout_seconds = 10});

  neograph::CompletionParams params;
  params.model = "claude-sonnet-4-20250514";
  params.messages = {
      neograph::ChatMessage{.role = "user", .content = "Weather?"}};
  params.tools = {neograph::ChatTool{
      .name = "get_weather",
      .description = "Get weather",
      .parameters = neograph::json::parse(
          R"({"type":"object","properties":{"location":{"type":"string"}}})")}};

  try {
    auto result = co_await provider->invoke(params, nullptr);
    XX_TEST_EXPECT_EQ(result.message.role, "assistant");
    XX_TEST_EXPECT_FALSE(result.message.tool_calls.empty());
    if (!result.message.tool_calls.empty()) {
      XX_TEST_EXPECT_EQ(result.message.tool_calls[0].name, "get_weather");
      XX_TEST_EXPECT_TRUE(result.message.tool_calls[0].arguments.find(
                              "Tokyo") != std::string::npos);
    }
  } catch (const std::exception &e) {
    XX_TEST_FAILED++;
    TEST_FAIL << "tool call failed: " << e.what() << std::endl;
  }
}

asio::awaitable<void> test_non_streaming_thinking(MockAnthropicServer &mock,
                                                  uint16_t port) {
  std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
  mock.mode = AnthropicMockMode::Thinking;

  auto provider = server::AnthropicProvider::create(
      {.api_key = "sk-ant-test", .base_url = baseUrl, .timeout_seconds = 10});

  neograph::CompletionParams params;
  params.model = "claude-sonnet-4-20250514";
  params.messages = {
      neograph::ChatMessage{.role = "user", .content = "Think step by step"}};

  try {
    auto result = co_await provider->invoke(params, nullptr);
    XX_TEST_EXPECT_EQ(result.message.content, "The answer is 42.");
    XX_TEST_EXPECT_TRUE(!result.message.reasoning_content.empty());
    XX_TEST_EXPECT_TRUE(result.message.reasoning_content.find("reasoning") !=
                        std::string::npos);
  } catch (const std::exception &e) {
    XX_TEST_FAILED++;
    TEST_FAIL << "thinking test failed: " << e.what() << std::endl;
  }
}

asio::awaitable<void> test_rate_limit_error(MockAnthropicServer &mock,
                                            uint16_t port) {
  std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
  mock.mode = AnthropicMockMode::RateLimit;

  auto provider = server::AnthropicProvider::create(
      {.api_key = "sk-ant-test", .base_url = baseUrl, .timeout_seconds = 10});

  neograph::CompletionParams params;
  params.model = "claude-sonnet-4-20250514";
  params.messages = {neograph::ChatMessage{.role = "user", .content = "hello"}};

  bool caught = false;
  int retryAfter = -2;
  try {
    co_await provider->invoke(params, nullptr);
  } catch (const neograph::RateLimitError &e) {
    caught = true;
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

asio::awaitable<void> test_server_error(MockAnthropicServer &mock,
                                        uint16_t port) {
  std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
  mock.mode = AnthropicMockMode::ServerError;

  auto provider = server::AnthropicProvider::create(
      {.api_key = "sk-ant-test", .base_url = baseUrl, .timeout_seconds = 10});

  neograph::CompletionParams params;
  params.model = "claude-sonnet-4-20250514";
  params.messages = {
      neograph::ChatMessage{.role = "user", .content = "trigger error"}};

  bool caught = false;
  try {
    co_await provider->invoke(params, nullptr);
  } catch (const std::runtime_error &e) {
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

asio::awaitable<void> test_request_headers(MockAnthropicServer &mock,
                                           uint16_t port) {
  std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
  mock.mode = AnthropicMockMode::Normal;

  auto provider =
      server::AnthropicProvider::create({.api_key = "sk-ant-header-test",
                                         .base_url = baseUrl,
                                         .anthropic_version = "2023-06-01",
                                         .timeout_seconds = 10});

  neograph::CompletionParams params;
  params.model = "claude-sonnet-4-20250514";
  params.messages = {
      neograph::ChatMessage{.role = "user", .content = "test headers"}};

  try {
    co_await provider->invoke(params, nullptr);
    XX_TEST_EXPECT_TRUE(
        mock.lastRequestHeaders.find("x-api-key: sk-ant-header-test") !=
        std::string::npos);
    XX_TEST_EXPECT_TRUE(
        mock.lastRequestHeaders.find("anthropic-version: 2023-06-01") !=
        std::string::npos);
  } catch (const std::exception &e) {
    XX_TEST_FAILED++;
    TEST_FAIL << "request headers test failed: " << e.what() << std::endl;
  }
}

asio::awaitable<void> test_request_body_format(MockAnthropicServer &mock,
                                               uint16_t port) {
  std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
  mock.mode = AnthropicMockMode::Normal;

  auto provider = server::AnthropicProvider::create(
      {.api_key = "sk-ant-test", .base_url = baseUrl, .timeout_seconds = 10});

  neograph::CompletionParams params;
  params.model = "claude-sonnet-4-20250514";
  params.messages = {
      neograph::ChatMessage{.role = "system", .content = "Be helpful."},
      neograph::ChatMessage{.role = "user", .content = "Hi"},
  };
  params.tools = {neograph::ChatTool{
      .name = "search",
      .description = "Search the web",
      .parameters =
          neograph::json::parse(R"({"type":"object","properties":{}})")}};

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
  } catch (const std::exception &e) {
    XX_TEST_FAILED++;
    TEST_FAIL << "request body format test failed: " << e.what() << std::endl;
  }
}

asio::awaitable<void> test_streaming_completion(MockAnthropicServer &mock,
                                                uint16_t port) {
  std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
  mock.mode = AnthropicMockMode::Streaming;

  // Reset to default streaming chunks
  mock.sseChunks = {
      MockAnthropicServer::sseEvent(
          "message_start",
          R"({"type":"message_start","message":{"id":"msg_s","type":"message","role":"assistant","usage":{"input_tokens":5}}})"),
      MockAnthropicServer::sseEvent(
          "content_block_start",
          R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"),
      MockAnthropicServer::sseEvent(
          "content_block_delta",
          R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hello"}})"),
      MockAnthropicServer::sseEvent(
          "content_block_delta",
          R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":" world"}})"),
      MockAnthropicServer::sseEvent(
          "content_block_stop", R"({"type":"content_block_stop","index":0})"),
      MockAnthropicServer::sseEvent(
          "message_delta",
          R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":3}})"),
      MockAnthropicServer::sseEvent("message_stop",
                                    R"({"type":"message_stop"})"),
  };

  auto provider = server::AnthropicProvider::create(
      {.api_key = "sk-ant-test", .base_url = baseUrl, .timeout_seconds = 10});

  neograph::CompletionParams params;
  params.model = "claude-sonnet-4-20250514";
  params.messages = {
      neograph::ChatMessage{.role = "user", .content = "Stream hello"}};

  std::string accumulated;
  neograph::StreamCallback onChunk = [&](const std::string &chunk) {
    accumulated += chunk;
  };

  try {
    auto result = co_await provider->invoke(params, onChunk);
    XX_TEST_EXPECT_EQ(result.message.role, "assistant");
    XX_TEST_EXPECT_EQ(result.message.content, "Hello world");
    XX_TEST_EXPECT_EQ(accumulated, "Hello world");
    XX_TEST_EXPECT_TRUE(result.usage.total_tokens > 0);
  } catch (const std::exception &e) {
    XX_TEST_FAILED++;
    TEST_FAIL << "streaming completion failed: " << e.what() << std::endl;
  }
}

asio::awaitable<void> test_streaming_thinking(MockAnthropicServer &mock,
                                              uint16_t port) {
  std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
  mock.mode = AnthropicMockMode::StreamingThinking;

  mock.sseChunks = {
      MockAnthropicServer::sseEvent(
          "message_start",
          R"({"type":"message_start","message":{"id":"msg_t","type":"message","role":"assistant","usage":{"input_tokens":5}}})"),
      MockAnthropicServer::sseEvent(
          "content_block_start",
          R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":""}})"),
      MockAnthropicServer::sseEvent(
          "content_block_delta",
          R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"Let me think"}})"),
      MockAnthropicServer::sseEvent(
          "content_block_delta",
          R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":" carefully..."}})"),
      MockAnthropicServer::sseEvent(
          "content_block_stop", R"({"type":"content_block_stop","index":0})"),
      MockAnthropicServer::sseEvent(
          "content_block_start",
          R"({"type":"content_block_start","index":1,"content_block":{"type":"text","text":""}})"),
      MockAnthropicServer::sseEvent(
          "content_block_delta",
          R"({"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"The answer is 42."}})"),
      MockAnthropicServer::sseEvent(
          "content_block_stop", R"({"type":"content_block_stop","index":1})"),
      MockAnthropicServer::sseEvent(
          "message_delta",
          R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":10}})"),
      MockAnthropicServer::sseEvent("message_stop",
                                    R"({"type":"message_stop"})"),
  };

  auto provider = server::AnthropicProvider::create(
      {.api_key = "sk-ant-test", .base_url = baseUrl, .timeout_seconds = 10});

  neograph::CompletionParams params;
  params.model = "claude-sonnet-4-20250514";
  params.messages = {
      neograph::ChatMessage{.role = "user", .content = "Think about it"}};

  std::string accumulated;
  neograph::StreamCallback onChunk = [&](const std::string &chunk) {
    accumulated += chunk;
  };

  try {
    auto result = co_await provider->invoke(params, onChunk);
    XX_TEST_EXPECT_EQ(result.message.content, "The answer is 42.");
    XX_TEST_EXPECT_EQ(result.message.reasoning_content,
                      "Let me think carefully...");
    // StreamCallback only receives content tokens; thinking is in reasoning_content
    XX_TEST_EXPECT_EQ(accumulated, "The answer is 42.");
  } catch (const std::exception &e) {
    XX_TEST_FAILED++;
    TEST_FAIL << "streaming thinking failed: " << e.what() << std::endl;
  }
}

asio::awaitable<void> test_streaming_tool_call(MockAnthropicServer &mock,
                                               uint16_t port) {
  std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
  mock.mode = AnthropicMockMode::StreamingToolCall;

  mock.sseChunks = {
      MockAnthropicServer::sseEvent(
          "message_start",
          R"({"type":"message_start","message":{"id":"msg_tc","type":"message","role":"assistant","usage":{"input_tokens":10}}})"),
      MockAnthropicServer::sseEvent(
          "content_block_start",
          R"({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"toolu_stream","name":"get_weather"}})"),
      MockAnthropicServer::sseEvent(
          "content_block_delta",
          R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\"location\":"}})"),
      MockAnthropicServer::sseEvent(
          "content_block_delta",
          R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"\"Tokyo\"}"}})"),
      MockAnthropicServer::sseEvent(
          "content_block_stop", R"({"type":"content_block_stop","index":0})"),
      MockAnthropicServer::sseEvent(
          "message_delta",
          R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":8}})"),
      MockAnthropicServer::sseEvent("message_stop",
                                    R"({"type":"message_stop"})"),
  };

  auto provider = server::AnthropicProvider::create(
      {.api_key = "sk-ant-test", .base_url = baseUrl, .timeout_seconds = 10});

  neograph::CompletionParams params;
  params.model = "claude-sonnet-4-20250514";
  params.messages = {
      neograph::ChatMessage{.role = "user", .content = "Weather in Tokyo?"}};
  params.tools = {neograph::ChatTool{
      .name = "get_weather",
      .description = "Get weather",
      .parameters = neograph::json::parse(
          R"({"type":"object","properties":{"location":{"type":"string"}}})")}};

  neograph::StreamCallback noop = [](const std::string &) {};

  try {
    auto result = co_await provider->invoke(params, noop);
    XX_TEST_EXPECT_EQ(result.message.tool_calls.size(), (size_t)1);
    if (!result.message.tool_calls.empty()) {
      XX_TEST_EXPECT_EQ(result.message.tool_calls[0].id, "toolu_stream");
      XX_TEST_EXPECT_EQ(result.message.tool_calls[0].name, "get_weather");
      XX_TEST_EXPECT_TRUE(result.message.tool_calls[0].arguments.find(
                              "Tokyo") != std::string::npos);
    }
  } catch (const std::exception &e) {
    XX_TEST_FAILED++;
    TEST_FAIL << "streaming tool call failed: " << e.what() << std::endl;
  }
}

asio::awaitable<void>
test_streaming_mixed_thinking_and_content(MockAnthropicServer &mock,
                                          uint16_t port) {
  std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
  mock.mode = AnthropicMockMode::StreamingThinking;

  mock.sseChunks = {
      MockAnthropicServer::sseEvent(
          "message_start",
          R"({"type":"message_start","message":{"id":"msg_mix","type":"message","role":"assistant","usage":{"input_tokens":5}}})"),
      MockAnthropicServer::sseEvent(
          "content_block_start",
          R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":""}})"),
      MockAnthropicServer::sseEvent(
          "content_block_delta",
          R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"Hmm..."}})"),
      MockAnthropicServer::sseEvent(
          "content_block_stop", R"({"type":"content_block_stop","index":0})"),
      MockAnthropicServer::sseEvent(
          "content_block_start",
          R"({"type":"content_block_start","index":1,"content_block":{"type":"text","text":""}})"),
      MockAnthropicServer::sseEvent(
          "content_block_delta",
          R"({"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"Result."}})"),
      MockAnthropicServer::sseEvent(
          "content_block_stop", R"({"type":"content_block_stop","index":1})"),
      MockAnthropicServer::sseEvent(
          "message_delta",
          R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":6}})"),
      MockAnthropicServer::sseEvent("message_stop",
                                    R"({"type":"message_stop"})"),
  };

  auto provider = server::AnthropicProvider::create(
      {.api_key = "sk-ant-test", .base_url = baseUrl, .timeout_seconds = 10});

  neograph::CompletionParams params;
  params.model = "claude-sonnet-4-20250514";
  params.messages = {
      neograph::ChatMessage{.role = "user", .content = "Mixed test"}};

  std::string contentAccum;
  std::string thinkingAccum;
  neograph::StreamCallback onChunk = [&](const std::string &chunk) {
    contentAccum += chunk;
  };

  try {
    auto result = co_await provider->invoke(params, onChunk);
    XX_TEST_EXPECT_EQ(result.message.content, "Result.");
    XX_TEST_EXPECT_EQ(result.message.reasoning_content, "Hmm...");
  } catch (const std::exception &e) {
    XX_TEST_FAILED++;
    TEST_FAIL << "streaming mixed test failed: " << e.what() << std::endl;
  }
}

asio::awaitable<void> test_streaming_usage(MockAnthropicServer &mock,
                                           uint16_t port) {
  std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
  mock.mode = AnthropicMockMode::Streaming;

  mock.sseChunks = {
      MockAnthropicServer::sseEvent(
          "message_start",
          R"({"type":"message_start","message":{"id":"msg_u","type":"message","role":"assistant","usage":{"input_tokens":42}}})"),
      MockAnthropicServer::sseEvent(
          "content_block_start",
          R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"),
      MockAnthropicServer::sseEvent(
          "content_block_delta",
          R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hi"}})"),
      MockAnthropicServer::sseEvent(
          "content_block_stop", R"({"type":"content_block_stop","index":0})"),
      MockAnthropicServer::sseEvent(
          "message_delta",
          R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":7}})"),
      MockAnthropicServer::sseEvent("message_stop",
                                    R"({"type":"message_stop"})"),
  };

  auto provider = server::AnthropicProvider::create(
      {.api_key = "sk-ant-test", .base_url = baseUrl, .timeout_seconds = 10});

  neograph::CompletionParams params;
  params.model = "claude-sonnet-4-20250514";
  params.messages = {
      neograph::ChatMessage{.role = "user", .content = "Usage test"}};

  neograph::StreamCallback noop = [](const std::string &) {};

  try {
    auto result = co_await provider->invoke(params, noop);
    XX_TEST_EXPECT_EQ(result.usage.prompt_tokens, 42);
    XX_TEST_EXPECT_EQ(result.usage.completion_tokens, 7);
    XX_TEST_EXPECT_EQ(result.usage.total_tokens, 49);
  } catch (const std::exception &e) {
    XX_TEST_FAILED++;
    TEST_FAIL << "streaming usage test failed: " << e.what() << std::endl;
  }
}

asio::awaitable<void>
test_streaming_malformed_event_skipped(MockAnthropicServer &mock,
                                       uint16_t port) {
  std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
  mock.mode = AnthropicMockMode::Streaming;

  mock.sseChunks = {
      MockAnthropicServer::sseEvent(
          "message_start",
          R"({"type":"message_start","message":{"id":"msg_m","type":"message","role":"assistant","usage":{"input_tokens":3}}})"),
      MockAnthropicServer::sseEvent(
          "content_block_start",
          R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"),
      "event: content_block_delta\ndata: not-valid-json\n\n",
      MockAnthropicServer::sseEvent(
          "content_block_delta",
          R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"OK"}})"),
      MockAnthropicServer::sseEvent(
          "content_block_stop", R"({"type":"content_block_stop","index":0})"),
      MockAnthropicServer::sseEvent(
          "message_delta",
          R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":1}})"),
      MockAnthropicServer::sseEvent("message_stop",
                                    R"({"type":"message_stop"})"),
  };

  auto provider = server::AnthropicProvider::create(
      {.api_key = "sk-ant-test", .base_url = baseUrl, .timeout_seconds = 10});

  neograph::CompletionParams params;
  params.model = "claude-sonnet-4-20250514";
  params.messages = {
      neograph::ChatMessage{.role = "user", .content = "Malformed test"}};

  neograph::StreamCallback noop = [](const std::string &) {};

  try {
    auto result = co_await provider->invoke(params, noop);
    XX_TEST_EXPECT_EQ(result.message.content, "OK");
  } catch (const std::exception &e) {
    XX_TEST_FAILED++;
    TEST_FAIL << "malformed event test failed: " << e.what() << std::endl;
  }
}

asio::awaitable<void>
test_thinking_callback_separation(MockAnthropicServer &mock, uint16_t port) {
  std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
  mock.mode = AnthropicMockMode::StreamingThinking;

  mock.sseChunks = {
      MockAnthropicServer::sseEvent(
          "message_start",
          R"({"type":"message_start","message":{"id":"msg_sep","type":"message","role":"assistant","usage":{"input_tokens":5}}})"),
      MockAnthropicServer::sseEvent(
          "content_block_start",
          R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":""}})"),
      MockAnthropicServer::sseEvent(
          "content_block_delta",
          R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"Deep thought"}})"),
      MockAnthropicServer::sseEvent(
          "content_block_stop", R"({"type":"content_block_stop","index":0})"),
      MockAnthropicServer::sseEvent(
          "content_block_start",
          R"({"type":"content_block_start","index":1,"content_block":{"type":"text","text":""}})"),
      MockAnthropicServer::sseEvent(
          "content_block_delta",
          R"({"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"Answer"}})"),
      MockAnthropicServer::sseEvent(
          "content_block_stop", R"({"type":"content_block_stop","index":1})"),
      MockAnthropicServer::sseEvent(
          "message_delta",
          R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":5}})"),
      MockAnthropicServer::sseEvent("message_stop",
                                    R"({"type":"message_stop"})"),
  };

  auto provider = server::AnthropicProvider::create(
      {.api_key = "sk-ant-test", .base_url = baseUrl, .timeout_seconds = 10});

  neograph::CompletionParams params;
  params.model = "claude-sonnet-4-20250514";
  params.messages = {
      neograph::ChatMessage{.role = "user", .content = "Separation test"}};

  // Set thinking callback to separate thinking from content
  std::string thinkingAccum;
  std::string contentAccum;

  try {
    auto result = co_await provider->invoke_format_data(
        params, [&](const neograph::ChatStreamChunk &chunk) {
          switch (chunk.type) {
          case neograph::ChatStreamChunk::TYPE_CONTENT: {
            contentAccum += chunk.data;
          } break;
          case neograph::ChatStreamChunk::TYPE_THINKING: {
            thinkingAccum += chunk.data;
          } break;
          }
        });
    XX_TEST_EXPECT_EQ(result.message.content, "Answer");
    XX_TEST_EXPECT_EQ(result.message.reasoning_content, "Deep thought");
    // With thinking_callback set, on_chunk should only get content
    XX_TEST_EXPECT_EQ(contentAccum, "Answer");
    XX_TEST_EXPECT_EQ(thinkingAccum, "Deep thought");
  } catch (const std::exception &e) {
    XX_TEST_FAILED++;
    TEST_FAIL << "thinking callback separation failed: " << e.what()
              << std::endl;
  }
}

// ---------------------------------------------------------------------------
// True streaming verification — server sends chunks with delays
// ---------------------------------------------------------------------------

class AnthropicDelayedStreamServer {
public:
  std::unique_ptr<asio::ip::tcp::acceptor> acceptor;
  asio::io_context ioCtx;
  std::thread thread;
  uint16_t boundPort = 0;
  std::vector<std::string> chunks;
  std::chrono::milliseconds delay{80};

  void start() {
    acceptor = std::make_unique<asio::ip::tcp::acceptor>(
        ioCtx, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    boundPort = acceptor->local_endpoint().port();

    asio::co_spawn(ioCtx, acceptLoop(), asio::detached);
    thread = std::thread([this]() { ioCtx.run(); });
  }

  void stop() {
    boost::system::error_code ec;
    if (acceptor)
      acceptor->close(ec);
    ioCtx.stop();
    if (thread.joinable())
      thread.join();
  }

private:
  asio::awaitable<void> acceptLoop() {
    while (acceptor->is_open()) {
      boost::system::error_code ec;
      auto socket = co_await acceptor->async_accept(
          asio::redirect_error(asio::use_awaitable, ec));
      if (ec)
        break;
      asio::co_spawn(ioCtx, handleConnection(std::move(socket)),
                     asio::detached);
    }
  }

  asio::awaitable<void> handleConnection(asio::ip::tcp::socket socket) {
    namespace http = boost::beast::http;
    boost::system::error_code ec;

    boost::beast::flat_buffer buf;
    http::request<http::string_body> req;
    co_await http::async_read(socket, buf, req,
                              asio::redirect_error(asio::use_awaitable, ec));
    if (ec)
      co_return;

    std::string header = "HTTP/1.1 200 OK\r\n"
                         "Content-Type: text/event-stream\r\n"
                         "Cache-Control: no-cache\r\n"
                         "Transfer-Encoding: chunked\r\n"
                         "\r\n";
    co_await asio::async_write(socket, asio::buffer(header),
                               asio::redirect_error(asio::use_awaitable, ec));
    if (ec)
      co_return;

    for (const auto &chunk : chunks) {
      std::string framed =
          std::format("{:x}\r\n{}\r\n", chunk.size(), chunk);
      co_await asio::async_write(socket, asio::buffer(framed),
                                 asio::redirect_error(asio::use_awaitable, ec));
      if (ec)
        co_return;

      asio::steady_timer timer(socket.get_executor(), delay);
      co_await timer.async_wait(
          asio::redirect_error(asio::use_awaitable, ec));
    }

    std::string finalChunk = "0\r\n\r\n";
    co_await asio::async_write(socket, asio::buffer(finalChunk),
                               asio::redirect_error(asio::use_awaitable, ec));

    socket.shutdown(asio::ip::tcp::socket::shutdown_send, ec);
  }
};

asio::awaitable<void> test_anthropic_true_streaming_incremental() {
  auto srv = std::make_unique<AnthropicDelayedStreamServer>();
  srv->chunks = {
      "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_ts\",\"type\":\"message\",\"role\":\"assistant\",\"usage\":{\"input_tokens\":5}}}\n\n",
      "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n",
      "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"X\"}}\n\n",
      "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Y\"}}\n\n",
      "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Z\"}}\n\n",
      "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n",
      "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":3}}\n\n",
      "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n",
  };
  srv->delay = std::chrono::milliseconds(80);
  srv->start();

  std::string baseUrl = "http://127.0.0.1:" + std::to_string(srv->boundPort);
  auto provider = server::AnthropicProvider::create(
      {.api_key = "sk-ant-test", .base_url = baseUrl, .timeout_seconds = 10});

  neograph::CompletionParams params;
  params.model = "claude-sonnet-4-20250514";
  params.messages = {
      neograph::ChatMessage{.role = "user", .content = "Stream test"}};

  std::vector<std::chrono::steady_clock::time_point> callbackTimes;
  auto startTime = std::chrono::steady_clock::now();

  neograph::StreamCallback onChunk = [&](const std::string &) {
    callbackTimes.push_back(std::chrono::steady_clock::now());
  };

  try {
    auto result = co_await provider->invoke(params, onChunk);
    XX_TEST_EXPECT_EQ(result.message.content, "XYZ");

    XX_TEST_EXPECT_TRUE(callbackTimes.size() >= 3);

    if (callbackTimes.size() >= 3) {
      auto firstOffset = std::chrono::duration_cast<std::chrono::milliseconds>(
                             callbackTimes.front() - startTime)
                             .count();
      auto lastOffset = std::chrono::duration_cast<std::chrono::milliseconds>(
                            callbackTimes.back() - startTime)
                            .count();
      auto spread = lastOffset - firstOffset;

      XX_TEST_EXPECT_TRUE(spread >= 100);
      if (spread < 100) {
        TEST_FAIL << "streaming not incremental: spread=" << spread
                  << "ms, callbacks=" << callbackTimes.size() << std::endl;
      }
    }
  } catch (const std::exception &e) {
    XX_TEST_FAILED++;
    TEST_FAIL << "true streaming test failed: " << e.what() << std::endl;
  }

  srv->stop();
}

// ---------------------------------------------------------------------------
// Test runner
// ---------------------------------------------------------------------------

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
  test_convert_tools();
  test_parse_response_text();
  test_parse_response_tool_use();
  test_parse_response_thinking();
  test_parse_response_mixed();
  test_parse_response_usage();

  // Integration tests
  uint16_t port = 0;
  auto mock = startAnthropicMockServer(port);
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
  co_await test_thinking_callback_separation(*mock, port);

  // True streaming incremental verification
  co_await test_anthropic_true_streaming_incremental();

  mock->server->stop();
  mock->thread.join();

  co_return TestResult{g_anthropic_passed, g_anthropic_failed};
}

} // namespace test
} // namespace agentxx
