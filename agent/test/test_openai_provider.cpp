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

/// Codex (Responses API) 配置
agentxx::agent::ModelConfig makeCodexCfg(const std::string& baseUrl) {
    auto mc      = makeOaiCfg("sk-codex", baseUrl);
    mc.name      = "codex-test";
    mc.type      = "openai-responses";
    mc.modelName = "gpt-5-codex";
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
    // OpenAI Responses API (/responses)
    ResponsesNormal,
    ResponsesToolCall,
    ResponsesStreaming,
    ResponsesStreamingToolCall,
    // 原始响应: 使用 rawStatus + rawBody (测试畸形/非标准响应容错)
    Raw,
};

class MockOpenAIServer {
public:

    std::unique_ptr<HttpServer> server;
    std::thread                 thread;
    MockMode                    mode = MockMode::Normal;
    std::string                 lastRequestBody;
    std::string                 lastAuthHeader;
    std::string                 lastCustomHeader;

    // SSE chunks to emit in streaming mode
    std::vector<std::string> sseChunks;

    // Optional override: when non-null, used for the next non-streaming response
    std::optional<neograph::json> customResponse;

    // MockMode::Raw 使用的原始状态码与 body
    int         rawStatus = 200;
    std::string rawBody;
    std::string rawContentType = "application/json";

    static std::string sseData(std::string_view json) {
        return "data: " + std::string(json) + "\n\n";
    }

    static std::string sseEvent(std::string_view event, std::string_view json) {
        return "event: " + std::string(event) + "\ndata: " + std::string(json) + "\n\n";
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

    // ------------------------------------------------------------------
    // OpenAI Responses API (/responses) response builders
    // ------------------------------------------------------------------

    neograph::json
        makeResponsesResponse(std::string_view content, int prompt = 10, int completion = 5) const {
        auto textPart           = neograph::json::object();
        textPart["type"]        = "output_text";
        textPart["text"]        = std::string(content);
        textPart["annotations"] = neograph::json::array();
        auto contentArr         = neograph::json::array();
        contentArr.push_back(textPart);

        auto msgItem       = neograph::json::object();
        msgItem["type"]    = "message";
        msgItem["role"]    = "assistant";
        msgItem["content"] = contentArr;

        auto output = neograph::json::array();
        output.push_back(msgItem);

        neograph::json resp;
        resp["id"]     = "resp_mock";
        resp["object"] = "response";
        resp["status"] = "completed";
        resp["output"] = output;
        resp["usage"]  = {
            {"input_tokens",  prompt             },
            {"output_tokens", completion         },
            {"total_tokens",  prompt + completion}
        };
        return resp;
    }

    neograph::json makeResponsesToolCallResponse() const {
        auto fcItem         = neograph::json::object();
        fcItem["type"]      = "function_call";
        fcItem["id"]        = "fc_1";
        fcItem["call_id"]   = "call_abc123";
        fcItem["name"]      = "get_weather";
        fcItem["arguments"] = R"({"location":"Tokyo"})";
        fcItem["status"]    = "completed";

        auto output = neograph::json::array();
        output.push_back(fcItem);

        neograph::json resp;
        resp["id"]     = "resp_tool";
        resp["object"] = "response";
        resp["status"] = "completed";
        resp["output"] = output;
        resp["usage"]  = {
            {"input_tokens",  8 },
            {"output_tokens", 4 },
            {"total_tokens",  12}
        };
        return resp;
    }

    /// 默认的 Responses 流式 SSE 事件序列
    void setDefaultResponsesSseChunks() {
        sseChunks = {
            sseEvent(
                "response.created",
                R"({"type":"response.created","response":{"id":"resp_mock"}})"
            ),
            sseEvent(
                "response.output_text.delta",
                R"({"type":"response.output_text.delta","item_id":"msg_1","output_index":0,"delta":"Hello"})"
            ),
            sseEvent(
                "response.output_text.delta",
                R"({"type":"response.output_text.delta","item_id":"msg_1","output_index":0,"delta":" world"})"
            ),
            sseEvent(
                "response.reasoning_text.delta",
                R"({"type":"response.reasoning_text.delta","item_id":"rs_1","output_index":0,"delta":"deep thought"})"
            ),
            sseEvent(
                "response.completed",
                R"({"type":"response.completed","response":{"id":"resp_mock","status":"completed"},"usage":{"input_tokens":5,"output_tokens":3,"total_tokens":8}})"
            ),
        };
    }

    /// 默认的 Responses 流式 tool_call SSE 事件序列
    void setDefaultResponsesToolCallSseChunks() {
        sseChunks = {
            sseEvent(
                "response.output_item.added",
                R"({"type":"response.output_item.added","output_index":0,"item":{"type":"function_call","id":"fc_1","call_id":"call_abc123","name":"get_weather","arguments":"","status":"in_progress"}})"
            ),
            sseEvent(
                "response.function_call_arguments.delta",
                R"({"type":"response.function_call_arguments.delta","item_id":"fc_1","output_index":0,"delta":"{\"location\":"})"
            ),
            sseEvent(
                "response.function_call_arguments.delta",
                R"({"type":"response.function_call_arguments.delta","item_id":"fc_1","output_index":0,"delta":"\"Tokyo\"}"})"
            ),
            sseEvent(
                "response.function_call_arguments.done",
                R"({"type":"response.function_call_arguments.done","item_id":"fc_1","output_index":0,"arguments":"{\"location\":\"Tokyo\"}"})"
            ),
            sseEvent(
                "response.completed",
                R"({"type":"response.completed","response":{"id":"resp_tool","status":"completed"},"usage":{"input_tokens":8,"output_tokens":4,"total_tokens":12}})"
            ),
        };
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
    auto makeHandler = [mock = mock.get()]() {
        return std::make_shared<HttpServer::Handler>(
            [mock](HttpServer::Request& req, HttpServer::Response& resp, std::string_view)
                -> asio::awaitable<void> {
                mock->lastRequestBody  = req.body();
                mock->lastAuthHeader   = std::string(req["authorization"]);
                mock->lastCustomHeader = std::string(req["x-custom-test"]);

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

                    case MockMode::ResponsesNormal:
                        resp.result(boost::beast::http::status::ok);
                        resp.set(boost::beast::http::field::content_type, "application/json");
                        if (mock->customResponse.has_value()) {
                            resp.body() = mock->customResponse->dump();
                            mock->customResponse.reset();
                        } else {
                            resp.body()
                                = mock->makeResponsesResponse("Hello from responses!").dump();
                        }
                        resp.prepare_payload();
                        break;

                    case MockMode::ResponsesToolCall:
                        resp.result(boost::beast::http::status::ok);
                        resp.set(boost::beast::http::field::content_type, "application/json");
                        resp.body() = mock->makeResponsesToolCallResponse().dump();
                        resp.prepare_payload();
                        break;

                    case MockMode::Raw:
                        resp.result(static_cast<boost::beast::http::status>(mock->rawStatus));
                        resp.set(boost::beast::http::field::content_type, mock->rawContentType);
                        resp.body() = mock->rawBody;
                        resp.prepare_payload();
                        break;

                    case MockMode::Streaming:
                    case MockMode::StreamingToolCall:
                    case MockMode::ResponsesStreaming:
                    case MockMode::ResponsesStreamingToolCall: {
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
        );
    };

    // Chat Completions API 路由
    mock->server->router().add("/chat/completions", 2, makeHandler());
    // Responses API 路由 (Codex) — 默认路径 /responses, 兼容网关形态 /v1/responses
    mock->server->router().add("/v1/responses", 2, makeHandler());
    mock->server->router().add("/responses", 2, makeHandler());
    // 自定义 api_path 测试路由
    mock->server->router().add("/v1/chat/completions", 2, makeHandler());

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

/// Vercel AI Gateway / 部分网关把推理内容放在 message.reasoning 字段
void test_parse_response_message_with_reasoning_field() {
    auto choice = neograph::json::parse(
        R"({
        "index": 0,
        "message": {
          "role": "assistant",
          "content": "The meaning of life...",
          "reasoning": "Let me think about this carefully..."
        },
        "finish_reason": "stop"
      })"
    );

    auto msg = neograph::parse_response_message(choice);
    XX_TEST_EXPECT_EQ(msg.role, "assistant");
    XX_TEST_EXPECT_EQ(msg.content, "The meaning of life...");
    XX_TEST_EXPECT_EQ(msg.reasoning_content, "Let me think about this carefully...");
}

/// message.reasoning 为 null 时不应抛异常, 保持空
void test_parse_response_message_null_reasoning_field() {
    auto choice = neograph::json::parse(
        R"({
        "index": 0,
        "message": {
          "role": "assistant",
          "content": "Answer",
          "reasoning": null
        },
        "finish_reason": "stop"
      })"
    );

    auto msg = neograph::parse_response_message(choice);
    XX_TEST_EXPECT_EQ(msg.content, "Answer");
    XX_TEST_EXPECT_EQ(msg.reasoning_content, "");
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
// Unit tests for multimodal messages (image / audio / video)
// ---------------------------------------------------------------------------

void test_parse_data_url() {
    // 标准 base64 data URL
    {
        auto r = neograph::parse_data_url("data:image/png;base64,aGVsbG8=");
        XX_TEST_EXPECT_HAS_VALUE(r);
        if (r) {
            XX_TEST_EXPECT_EQ(r->first, "image/png");
            XX_TEST_EXPECT_EQ(r->second, "aGVsbG8=");
        }
    }
    // 带参数 (name=...) 的 data URL
    {
        auto r = neograph::parse_data_url("data:audio/wav;name=a.wav;base64,UklGRg==");
        XX_TEST_EXPECT_HAS_VALUE(r);
        if (r) {
            XX_TEST_EXPECT_EQ(r->first, "audio/wav");
            XX_TEST_EXPECT_EQ(r->second, "UklGRg==");
        }
    }
    // 无 media type (RFC 2397 允许 data:;base64,xxx)
    {
        auto r = neograph::parse_data_url("data:;base64,AAAA");
        XX_TEST_EXPECT_HAS_VALUE(r);
        if (r) {
            XX_TEST_EXPECT_EQ(r->first, "");
            XX_TEST_EXPECT_EQ(r->second, "AAAA");
        }
    }
    // 非 data URL → nullopt
    XX_TEST_EXPECT_NULLOPT(neograph::parse_data_url("https://example.com/a.png"));
    XX_TEST_EXPECT_NULLOPT(neograph::parse_data_url("/local/path/a.png"));
    // 无逗号分隔 → nullopt
    XX_TEST_EXPECT_NULLOPT(neograph::parse_data_url("data:image/png;base64"));
    // 非 base64 编码 (percent-encoded) → nullopt
    XX_TEST_EXPECT_NULLOPT(neograph::parse_data_url("data:text/plain,hello"));
}

void test_media_format_from_mime() {
    XX_TEST_EXPECT_EQ(neograph::media_format_from_mime("audio/wav"), "wav");
    XX_TEST_EXPECT_EQ(neograph::media_format_from_mime("audio/x-wav"), "wav");
    XX_TEST_EXPECT_EQ(neograph::media_format_from_mime("audio/wave"), "wav");
    XX_TEST_EXPECT_EQ(neograph::media_format_from_mime("audio/mpeg"), "mp3");
    XX_TEST_EXPECT_EQ(neograph::media_format_from_mime("audio/mp3"), "mp3");
    XX_TEST_EXPECT_EQ(neograph::media_format_from_mime("audio/mpga"), "mp3");
    XX_TEST_EXPECT_EQ(neograph::media_format_from_mime("audio/ogg"), "ogg");
    XX_TEST_EXPECT_EQ(neograph::media_format_from_mime("audio/aac"), "aac");
    XX_TEST_EXPECT_EQ(neograph::media_format_from_mime("audio/flac"), "flac");
    XX_TEST_EXPECT_EQ(neograph::media_format_from_mime("audio/webm"), "webm");
    XX_TEST_EXPECT_EQ(neograph::media_format_from_mime("video/mp4"), "mp4");
    XX_TEST_EXPECT_EQ(neograph::media_format_from_mime("video/mpeg"), "mpeg");
    XX_TEST_EXPECT_EQ(neograph::media_format_from_mime("video/quicktime"), "mov");
    XX_TEST_EXPECT_EQ(neograph::media_format_from_mime("video/x-msvideo"), "avi");
    XX_TEST_EXPECT_EQ(neograph::media_format_from_mime("video/x-matroska"), "mkv");
    XX_TEST_EXPECT_EQ(neograph::media_format_from_mime("video/mp2t"), "mpegts");
    XX_TEST_EXPECT_EQ(neograph::media_format_from_mime("video/vnd.apple.mpegurl"), "m3u8");
    // 带 codecs 参数
    XX_TEST_EXPECT_EQ(neograph::media_format_from_mime("audio/mpeg;codecs=mp3"), "mp3");
    XX_TEST_EXPECT_EQ(neograph::media_format_from_mime("video/mp4;codecs=avc1.42E01E"), "mp4");
    // 未知类型: 返回子类型小写
    XX_TEST_EXPECT_EQ(neograph::media_format_from_mime("audio/amr"), "amr");
    XX_TEST_EXPECT_EQ(neograph::media_format_from_mime("APPLICATION/PDF"), "pdf");
}

void test_messages_to_json_multimodal() {
    neograph::ChatMessage msg;
    msg.role       = "user";
    msg.content    = "看图说话";
    msg.image_urls = {"https://example.com/a.png"};
    msg.audio_urls = {"data:audio/wav;base64,UklGRg==", "https://example.com/a.wav"};
    msg.video_urls = {"data:video/mp4;base64,AAAA", "https://example.com/a.mp4"};

    auto arr = neograph::messages_to_json({msg});
    XX_TEST_EXPECT_EQ(arr.size(), (size_t)1);
    XX_TEST_EXPECT_TRUE(arr[0]["content"].is_array());

    const auto& parts = arr[0]["content"];
    // text + image + audio x2 + video x2
    XX_TEST_EXPECT_EQ(parts.size(), (size_t)6);

    // text
    XX_TEST_EXPECT_EQ(parts[0]["type"].get<std::string>(), "text");
    XX_TEST_EXPECT_EQ(parts[0]["text"].get<std::string>(), "看图说话");
    // image
    XX_TEST_EXPECT_EQ(parts[1]["type"].get<std::string>(), "image_url");
    XX_TEST_EXPECT_EQ(parts[1]["image_url"]["url"].get<std::string>(), "https://example.com/a.png");
    // audio (data URL → input_audio data+format)
    XX_TEST_EXPECT_EQ(parts[2]["type"].get<std::string>(), "input_audio");
    XX_TEST_EXPECT_EQ(parts[2]["input_audio"]["data"].get<std::string>(), "UklGRg==");
    XX_TEST_EXPECT_EQ(parts[2]["input_audio"]["format"].get<std::string>(), "wav");
    // audio (HTTP URL → url 透传)
    XX_TEST_EXPECT_EQ(parts[3]["type"].get<std::string>(), "input_audio");
    XX_TEST_EXPECT_EQ(
        parts[3]["input_audio"]["url"].get<std::string>(),
        "https://example.com/a.wav"
    );
    // video x2 (data URL + HTTP URL)
    XX_TEST_EXPECT_EQ(parts[4]["type"].get<std::string>(), "video_url");
    XX_TEST_EXPECT_EQ(
        parts[4]["video_url"]["url"].get<std::string>(),
        "data:video/mp4;base64,AAAA"
    );
    XX_TEST_EXPECT_EQ(parts[5]["type"].get<std::string>(), "video_url");
    XX_TEST_EXPECT_EQ(parts[5]["video_url"]["url"].get<std::string>(), "https://example.com/a.mp4");
}

void test_messages_to_json_multimodal_empty_content() {
    // content 为空但只有附件时, 不产生 text 块
    neograph::ChatMessage msg;
    msg.role          = "user";
    msg.audio_urls    = {"data:audio/mpeg;base64,SUQzBAAAAA=="};
    auto        arr   = neograph::messages_to_json({msg});
    const auto& parts = arr[0]["content"];
    XX_TEST_EXPECT_EQ(parts.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(parts[0]["type"].get<std::string>(), "input_audio");
    XX_TEST_EXPECT_EQ(parts[0]["input_audio"]["format"].get<std::string>(), "mp3");
}

void test_messages_to_json_multimodal_roundtrip() {
    // to_json/from_json (wire 协议) 应保留 audio_urls / video_urls
    neograph::ChatMessage msg;
    msg.role       = "user";
    msg.content    = "hi";
    msg.image_urls = {"https://example.com/i.png"};
    msg.audio_urls = {"data:audio/wav;base64,UklGRg=="};
    msg.video_urls = {"https://example.com/v.mp4"};

    neograph::json j;
    neograph::to_json(j, msg);
    neograph::ChatMessage back;
    neograph::from_json(j, back);
    XX_TEST_EXPECT_EQ(back.role, "user");
    XX_TEST_EXPECT_EQ(back.content, "hi");
    XX_TEST_EXPECT_EQ(back.image_urls.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(back.audio_urls.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(back.video_urls.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(back.audio_urls[0], "data:audio/wav;base64,UklGRg==");
    XX_TEST_EXPECT_EQ(back.video_urls[0], "https://example.com/v.mp4");
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
    std::string content = "A<think>B</think>C<think>D";
    std::string thinking;
    server::OpenAIProvider::extractThinkTags(content, thinking);
    XX_TEST_EXPECT_EQ(thinking, "BD");
    XX_TEST_EXPECT_EQ(content, "AC");
}

/// 直接调用实现函数验证单个未闭合标签场景
void test_extract_think_tags_unclosed_direct() {
    std::string content = "Start<think>Unclosed thinking";
    std::string thinking;
    server::OpenAIProvider::extractThinkTags(content, thinking);
    XX_TEST_EXPECT_EQ(thinking, "Unclosed thinking");
    XX_TEST_EXPECT_EQ(content, "Start");
}

/// llama.cpp 本地模型在 content 末尾输出
/// `<tool_call> <function=musicxx_GetMediaInfo> </function> </tool_call>` 的兼容场景
void test_extract_tool_calls_xml_in_content() {
    std::string content
        = "让我查一下这首歌的信息。<tool_call> <function=musicxx_GetMediaInfo> </function> "
          "</tool_call>";
    std::vector<neograph::ToolCall> calls;
    server::OpenAIProvider::extractToolCalls(content, calls);
    XX_TEST_EXPECT_EQ(calls.size(), (size_t)1);
    if (calls.size() == 1) {
        XX_TEST_EXPECT_EQ(calls[0].name, "musicxx_GetMediaInfo");
        // 缺失参数时按空对象处理, 保证下游 json::parse 可用
        XX_TEST_EXPECT_EQ(calls[0].arguments, "{}");
    }
    // 提取后从 content 中移除
    XX_TEST_EXPECT_TRUE(content.find("tool_call") == std::string::npos);
    XX_TEST_EXPECT_TRUE(content.find("musicxx_GetMediaInfo") == std::string::npos);
    XX_TEST_EXPECT_EQ(content, "让我查一下这首歌的信息。");
}

/// XML 风格 + JSON 参数: <tool_call><function=name>{...}</function></tool_call>
void test_extract_tool_calls_xml_with_args() {
    std::string content
        = "<tool_call><function=get_weather>{\"location\":\"Tokyo\",\"unit\":\"c\"}</function></"
          "tool_call>";
    std::vector<neograph::ToolCall> calls;
    server::OpenAIProvider::extractToolCalls(content, calls);
    XX_TEST_EXPECT_EQ(calls.size(), (size_t)1);
    if (calls.size() == 1) {
        XX_TEST_EXPECT_EQ(calls[0].name, "get_weather");
        auto args = neograph::json::parse(calls[0].arguments);
        XX_TEST_EXPECT_EQ(args["location"].get<std::string>(), "Tokyo");
        XX_TEST_EXPECT_EQ(args["unit"].get<std::string>(), "c");
    }
    XX_TEST_EXPECT_TRUE(content.empty());
}

/// 未用 <tool_call> 包裹的裸 <function=...> 标签
void test_extract_tool_calls_xml_bare_function() {
    std::string content = "请稍等<function=musicxx_GetMediaInfo>{\"id\":42}</function>";
    std::vector<neograph::ToolCall> calls;
    server::OpenAIProvider::extractToolCalls(content, calls);
    XX_TEST_EXPECT_EQ(calls.size(), (size_t)1);
    if (calls.size() == 1) {
        XX_TEST_EXPECT_EQ(calls[0].name, "musicxx_GetMediaInfo");
        auto args = neograph::json::parse(calls[0].arguments);
        XX_TEST_EXPECT_EQ(args["id"].get<int>(), 42);
    }
    XX_TEST_EXPECT_EQ(content, "请稍等");
}

/// 未闭合的 <tool_call> (截断输出): 仍应提取到 content 末尾
void test_extract_tool_calls_xml_unclosed() {
    std::string content = "思考...<tool_call> <function=musicxx_GetMediaInfo>";
    std::vector<neograph::ToolCall> calls;
    server::OpenAIProvider::extractToolCalls(content, calls);
    XX_TEST_EXPECT_EQ(calls.size(), (size_t)1);
    if (calls.size() == 1) {
        XX_TEST_EXPECT_EQ(calls[0].name, "musicxx_GetMediaInfo");
        XX_TEST_EXPECT_EQ(calls[0].arguments, "{}");
    }
    XX_TEST_EXPECT_EQ(content, "思考...");
}

/// 标签不区分大小写
void test_extract_tool_calls_xml_case_insensitive() {
    std::string content = "<TOOL_CALL><FUNCTION=musicxx_GetMediaInfo></FUNCTION></TOOL_CALL>";
    std::vector<neograph::ToolCall> calls;
    server::OpenAIProvider::extractToolCalls(content, calls);
    XX_TEST_EXPECT_EQ(calls.size(), (size_t)1);
    if (calls.size() == 1) {
        XX_TEST_EXPECT_EQ(calls[0].name, "musicxx_GetMediaInfo");
    }
    XX_TEST_EXPECT_TRUE(content.empty());
}

/// <tool_call> 内直接是 JSON (未使用 <function=...> 包裹)
void test_extract_tool_calls_xml_with_json_block() {
    std::string content
        = "<tool_call>{\"name\":\"get_weather\",\"arguments\":{\"city\":\"Beijing\"}}</tool_call>";
    std::vector<neograph::ToolCall> calls;
    server::OpenAIProvider::extractToolCalls(content, calls);
    XX_TEST_EXPECT_EQ(calls.size(), (size_t)1);
    if (calls.size() == 1) {
        XX_TEST_EXPECT_EQ(calls[0].name, "get_weather");
        auto args = neograph::json::parse(calls[0].arguments);
        XX_TEST_EXPECT_EQ(args["city"].get<std::string>(), "Beijing");
    }
    XX_TEST_EXPECT_TRUE(content.empty());
}

/// 无 tool call 时文本保持原样
void test_extract_tool_calls_xml_no_call_keeps_text() {
    std::string content = "普通回复, 没有调用任何工具。<tool_call> </tool_call>";
    std::vector<neograph::ToolCall> calls;
    server::OpenAIProvider::extractToolCalls(content, calls);
    XX_TEST_EXPECT_TRUE(calls.empty());
    XX_TEST_EXPECT_EQ(content, "普通回复, 没有调用任何工具。<tool_call> </tool_call>");
}

/// thinking 内容中的 XML 风格 tool call (reasoning_content 同样适用)
void test_extract_tool_calls_xml_in_thinking() {
    std::string content
        = "先获取歌曲信息。<tool_call> <function=musicxx_GetMediaInfo> {\"song\":\"test\"} </function> </tool_call>";
    std::vector<neograph::ToolCall> calls;
    server::OpenAIProvider::extractToolCalls(content, calls);
    XX_TEST_EXPECT_EQ(calls.size(), (size_t)1);
    if (calls.size() == 1) {
        XX_TEST_EXPECT_EQ(calls[0].name, "musicxx_GetMediaInfo");
        auto args = neograph::json::parse(calls[0].arguments);
        XX_TEST_EXPECT_EQ(args["song"].get<std::string>(), "test");
    }
    XX_TEST_EXPECT_EQ(content, "先获取歌曲信息。");
}

/// 多个 <tool_call> 块全部提取
void test_extract_tool_calls_xml_multiple() {
    std::string content
        = "<tool_call><function=musicxx_GetMediaInfo></function></tool_call> 然后 <tool_call><function=musicxx_GetLyric></function></tool_call>";
    std::vector<neograph::ToolCall> calls;
    server::OpenAIProvider::extractToolCalls(content, calls);
    XX_TEST_EXPECT_EQ(calls.size(), (size_t)2);
    if (calls.size() == 2) {
        XX_TEST_EXPECT_EQ(calls[0].name, "musicxx_GetMediaInfo");
        XX_TEST_EXPECT_EQ(calls[1].name, "musicxx_GetLyric");
    }
    XX_TEST_EXPECT_EQ(content, "然后");
}

/// 提取后 block 之后的文本保留 (含 提取失败 时 block 原样保留)
void test_extract_tool_calls_xml_trailing_text() {
    {
        std::string content
            = "<tool_call><function=musicxx_GetMediaInfo></function></tool_call>以上完成";
        std::vector<neograph::ToolCall> calls;
        server::OpenAIProvider::extractToolCalls(content, calls);
        XX_TEST_EXPECT_EQ(calls.size(), (size_t)1);
        if (calls.size() == 1) {
            XX_TEST_EXPECT_EQ(calls[0].name, "musicxx_GetMediaInfo");
        }
        XX_TEST_EXPECT_EQ(content, "以上完成");
    }
    {
        // 提取失败: 保留 block 原文及前后文本
        std::string content = "前文<tool_call> 无有效内容 </tool_call>后文";
        std::vector<neograph::ToolCall> calls;
        server::OpenAIProvider::extractToolCalls(content, calls);
        XX_TEST_EXPECT_TRUE(calls.empty());
        XX_TEST_EXPECT_EQ(content, "前文<tool_call> 无有效内容 </tool_call>后文");
    }
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
            // 兼容性增强: 非流式路径缺失 id 时同样回填 "call_N" (与流式路径一致)
            XX_TEST_EXPECT_EQ(result.message.tool_calls[0].id, "call_0");
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

/// Vercel AI Gateway / 部分网关使用 delta.reasoning 流式输出推理内容
asio::awaitable<void> test_streaming_reasoning_field_compat(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Streaming;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "vercel-gateway-model";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Reason via delta.reasoning"}
    };

    mock.sseChunks = {
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{"role":"assistant","content":""}}]})"
        ),
        MockOpenAIServer::sseData(R"({"choices":[{"index":0,"delta":{"reasoning":"Think hard"}}]})"
        ),
        MockOpenAIServer::sseData(
            R"({"choices":[{"index":0,"delta":{"reasoning":" about the answer"}}]})"
        ),
        MockOpenAIServer::sseData(R"({"choices":[{"index":0,"delta":{"content":"It is 7"}}]})"),
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
        XX_TEST_EXPECT_EQ(result.message.content, "It is 7");
        XX_TEST_EXPECT_EQ(result.message.reasoning_content, "Think hard about the answer");
        XX_TEST_EXPECT_EQ(accumulated, "It is 7");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "streaming reasoning field compat test failed: " << e.what() << std::endl;
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
// Tests — OpenAI provider compatibility enhancements
// ---------------------------------------------------------------------------

void test_create_provider_codex() {
    agentxx::agent::ModelConfig mc;
    mc.name    = "codex-test";
    mc.apiKey  = "sk-codex";
    mc.baseUrl = "http://localhost:8080";
    mc.type    = "openai-responses";
    auto p     = agentxx::agent::ModelProviderRegistry::createProvider(mc);
    XX_TEST_EXPECT_TRUE(p != nullptr);
    XX_TEST_EXPECT_EQ(p->get_name(), "openai-responses");
}

void test_create_provider_openai_responses_flag() {
    agentxx::agent::ModelConfig mc;
    mc.name    = "resp-test";
    mc.apiKey  = "sk-test";
    mc.baseUrl = "http://localhost:8080";
    mc.type    = "openai-responses";
    auto p     = agentxx::agent::ModelProviderRegistry::createProvider(mc);
    XX_TEST_EXPECT_TRUE(p != nullptr);
    XX_TEST_EXPECT_EQ(p->get_name(), "openai-responses");
}

void test_responses_sse_parsing_edge_cases() {
    using server::OpenAIProvider;

    // [DONE] 兼容标记
    {
        std::string                       buf = "data: [DONE]\n\n";
        neograph::ChatCompletion          completion;
        std::string                       content, thinking;
        std::map<int, neograph::ToolCall> tcMap;
        bool                              done = OpenAIProvider::processResponsesSseBuffer(
            buf,
            completion,
            content,
            thinking,
            tcMap,
            nullptr
        );
        XX_TEST_EXPECT_TRUE(done);
    }

    // 末尾无换行, finalFlush 补解析
    {
        std::string buf = R"(data: {"type":"response.output_text.delta","delta":"End"})";
        neograph::ChatCompletion          completion;
        std::string                       content, thinking;
        std::map<int, neograph::ToolCall> tcMap;
        OpenAIProvider::processResponsesSseBuffer(
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

    // error 事件写入 errOut
    {
        std::string              buf = "data: {\"type\":\"error\",\"message\":\"boom\"}\n\n";
        neograph::ChatCompletion completion;
        std::string              content, thinking;
        std::map<int, neograph::ToolCall> tcMap;
        std::string                       err;
        OpenAIProvider::processResponsesSseBuffer(
            buf,
            completion,
            content,
            thinking,
            tcMap,
            nullptr,
            /*finalFlush=*/false,
            &err
        );
        XX_TEST_EXPECT_EQ(err, "boom");
    }
}

/// 畸形 SSE chunk 容错: 非标准字段类型不应抛异常中断流
void test_openai_sse_malformed_types_tolerated() {
    using server::OpenAIProvider;

    // 数字 tool_call id / 字符串 index: 应转换为字符串 id 并按 index 归组
    {
        std::string buf
            = "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":\"0\",\"id\":12345,"
              "\"function\":{\"name\":\"get_weather\",\"arguments\":\"{\\\"a\\\":1}\"}}]}}]}\n\n";
        neograph::ChatCompletion          completion;
        std::string                       content, thinking;
        std::map<int, neograph::ToolCall> tcMap;
        OpenAIProvider::processSseBuffer(buf, completion, content, thinking, tcMap, nullptr);
        XX_TEST_EXPECT_EQ(tcMap.size(), (size_t)1);
        if (!tcMap.empty()) {
            XX_TEST_EXPECT_EQ(tcMap[0].id, "12345");
            XX_TEST_EXPECT_EQ(tcMap[0].name, "get_weather");
        }
    }

    // 非字符串 finish_reason / 非字符串 content / 非数组 tool_calls / 非对象 choices[0]:
    // 全部跳过且不抛异常
    {
        std::string buf = "data: {\"choices\":[{\"finish_reason\":3,\"delta\":{\"content\":42,"
                          "\"tool_calls\":{\"bad\":\"shape\"}}}]}\n"
                          "data: {\"choices\":[\"not-an-object\"]}\n"
                          "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n";
        neograph::ChatCompletion          completion;
        std::string                       content, thinking;
        std::map<int, neograph::ToolCall> tcMap;
        bool                              done = false;
        try {
            done = OpenAIProvider::processSseBuffer(
                buf,
                completion,
                content,
                thinking,
                tcMap,
                nullptr
            );
        } catch (...) {
            XX_TEST_FAILED++;
            TEST_FAIL << "malformed sse types should not throw" << std::endl;
        }
        XX_TEST_EXPECT_FALSE(done);
        XX_TEST_EXPECT_EQ(content, "ok");
    }

    // usage 为字符串数字时也应解析
    {
        std::string              buf = "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}],"
                                       "\"usage\":{\"prompt_tokens\":\"7\",\"completion_tokens\":\"3\","
                                       "\"total_tokens\":\"10\"}}\n";
        neograph::ChatCompletion completion;
        std::string              content, thinking;
        std::map<int, neograph::ToolCall> tcMap;
        OpenAIProvider::processSseBuffer(buf, completion, content, thinking, tcMap, nullptr);
        XX_TEST_EXPECT_EQ(completion.usage.prompt_tokens, 7);
        XX_TEST_EXPECT_EQ(completion.usage.completion_tokens, 3);
        XX_TEST_EXPECT_EQ(completion.usage.total_tokens, 10);
    }

    // usage 为非对象 (如字符串) 时跳过不抛异常
    {
        std::string              buf = "data: {\"choices\":[{\"delta\":{\"content\":\"y\"}}],"
                                       "\"usage\":\"weird\"}\n";
        neograph::ChatCompletion completion;
        std::string              content, thinking;
        std::map<int, neograph::ToolCall> tcMap;
        try {
            OpenAIProvider::processSseBuffer(buf, completion, content, thinking, tcMap, nullptr);
            XX_TEST_EXPECT_EQ(content, "y");
        } catch (...) {
            XX_TEST_FAILED++;
            TEST_FAIL << "non-object usage should not throw" << std::endl;
        }
    }
}

/// Responses API: response.incomplete (max_output_tokens 截断) 也视为正常结束标记
void test_responses_sse_incomplete_done() {
    using server::OpenAIProvider;

    std::string                       buf = "event: response.output_text.delta\n"
                                            "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Partial\"}\n\n"
                                            "event: response.incomplete\n"
                                            "data: {\"type\":\"response.incomplete\",\"response\":{\"id\":\"resp_x\","
                                            "\"status\":\"incomplete\"}}\n\n";
    neograph::ChatCompletion          completion;
    std::string                       content, thinking;
    std::map<int, neograph::ToolCall> tcMap;
    bool                              done = OpenAIProvider::processResponsesSseBuffer(
        buf,
        completion,
        content,
        thinking,
        tcMap,
        nullptr
    );
    XX_TEST_EXPECT_TRUE(done);
    XX_TEST_EXPECT_EQ(content, "Partial");
}

asio::awaitable<void> test_max_tokens_sent(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Normal;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model      = "gpt-4o-mini";
    params.max_tokens = 1024;
    params.messages   = {
        neograph::ChatMessage{.role = "user", .content = "hi"}
    };

    try {
        co_await provider->invoke(params, nullptr);
        auto sent = neograph::json::parse(mock.lastRequestBody);
        XX_TEST_EXPECT_TRUE(sent.contains("max_tokens"));
        XX_TEST_EXPECT_EQ(sent["max_tokens"].get<int>(), 1024);
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "max_tokens test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_max_completion_tokens_sent(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Normal;

    auto mc       = makeOaiCfg("sk-test", baseUrl);
    auto provider = server::OpenAIProvider::create(mc);

    neograph::CompletionParams params;
    params.model      = "gpt-5";
    params.max_tokens = 512;
    params.messages   = {
        neograph::ChatMessage{.role = "user", .content = "hi"}
    };

    try {
        co_await provider->invoke(params, nullptr);
        auto sent = neograph::json::parse(mock.lastRequestBody);
        XX_TEST_EXPECT_TRUE(sent.contains("max_completion_tokens"));
        XX_TEST_EXPECT_EQ(sent["max_completion_tokens"].get<int>(), 512);
        XX_TEST_EXPECT_FALSE(sent.contains("max_tokens"));
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "max_completion_tokens test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_stop_reason_mapping(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Normal;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-4o";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "hi"}
    };

    // finish_reason = tool_calls → stop_reason = tool_use
    mock.customResponse = mock.makeToolCallResponse();
    try {
        auto result = co_await provider->invoke(params, nullptr);
        XX_TEST_EXPECT_EQ(result.stop_reason, "tool_use");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "stop_reason(tool_calls) failed: " << e.what() << std::endl;
    }

    // finish_reason = stop → stop_reason = end_turn
    mock.customResponse = mock.makeCompletionResponse("ok");
    try {
        auto result = co_await provider->invoke(params, nullptr);
        XX_TEST_EXPECT_EQ(result.stop_reason, "end_turn");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "stop_reason(stop) failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_custom_api_path(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Normal;

    auto mc       = makeOaiCfg("sk-test", baseUrl);
    mc.apiPath    = "/v1/chat/completions";
    auto provider = server::OpenAIProvider::create(mc);

    neograph::CompletionParams params;
    params.model    = "gpt-4o-mini";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "path test"}
    };

    try {
        co_await provider->invoke(params, nullptr);
        // 请求到达 /v1/chat/completions 路由则 lastRequestBody 会被填充
        auto sent = neograph::json::parse(mock.lastRequestBody);
        XX_TEST_EXPECT_EQ(sent["model"].get<std::string>(), "gpt-4o-mini");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "custom api_path test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_send_temperature_disabled(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Normal;

    auto mc       = makeOaiCfg("sk-test", baseUrl);
    auto provider = server::OpenAIProvider::create(mc);

    neograph::CompletionParams params;
    params.model       = "deepseek-reasoner";
    params.temperature = static_cast<float>(0.9);
    params.messages    = {
        neograph::ChatMessage{.role = "user", .content = "hi"}
    };

    try {
        co_await provider->invoke(params, nullptr);
        auto sent = neograph::json::parse(mock.lastRequestBody);
        XX_TEST_EXPECT_FALSE(sent.contains("temperature"));
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "send_temperature disabled test failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_extra_headers_sent(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Normal;

    auto mc                          = makeOaiCfg("sk-test", baseUrl);
    mc.extraHeaders["x-custom-test"] = "custom-value";
    auto provider                    = server::OpenAIProvider::create(mc);

    neograph::CompletionParams params;
    params.model    = "gpt-4o-mini";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "header test"}
    };

    try {
        co_await provider->invoke(params, nullptr);
        XX_TEST_EXPECT_EQ(mock.lastCustomHeader, "custom-value");
        XX_TEST_EXPECT_TRUE(mock.lastAuthHeader.find("sk-test") != std::string::npos);
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "extra headers test failed: " << e.what() << std::endl;
    }
}

/// Chat Completions API: 用户消息携带图片/音频/视频附件时, 请求体应组装为多模态 content parts
asio::awaitable<void> test_multimodal_body_chat_completions(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Normal;

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-4o";
    params.messages = {
        {.role       = "user",
         .content    = "描述这个视频",
         .image_urls = {"https://example.com/cat.png"},
         .audio_urls = {"data:audio/wav;base64,UklGRg=="},
         .video_urls = {"https://example.com/cat.mp4"}},
    };

    try {
        co_await provider->invoke(params, nullptr);
        auto sent = neograph::json::parse(mock.lastRequestBody);
        XX_TEST_EXPECT_TRUE(sent["messages"].is_array());
        const auto& content = sent["messages"][0]["content"];
        XX_TEST_EXPECT_TRUE(content.is_array());
        XX_TEST_EXPECT_EQ(content.size(), (size_t)4);

        XX_TEST_EXPECT_EQ(content[0]["type"].get<std::string>(), "text");
        XX_TEST_EXPECT_EQ(content[0]["text"].get<std::string>(), "描述这个视频");
        XX_TEST_EXPECT_EQ(content[1]["type"].get<std::string>(), "image_url");
        XX_TEST_EXPECT_EQ(
            content[1]["image_url"]["url"].get<std::string>(),
            "https://example.com/cat.png"
        );
        XX_TEST_EXPECT_EQ(content[2]["type"].get<std::string>(), "input_audio");
        XX_TEST_EXPECT_EQ(content[2]["input_audio"]["data"].get<std::string>(), "UklGRg==");
        XX_TEST_EXPECT_EQ(content[2]["input_audio"]["format"].get<std::string>(), "wav");
        XX_TEST_EXPECT_EQ(content[3]["type"].get<std::string>(), "video_url");
        XX_TEST_EXPECT_EQ(
            content[3]["video_url"]["url"].get<std::string>(),
            "https://example.com/cat.mp4"
        );
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "multimodal chat.completions body test failed: " << e.what() << std::endl;
    }
}

/// Responses API (Codex): 用户消息携带图片/音频/视频附件时,
/// 请求体应组装为 input_image / input_audio / input_video
asio::awaitable<void> test_multimodal_body_responses(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::ResponsesNormal;

    auto provider = server::OpenAIProvider::create(makeCodexCfg(baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-5-codex";
    params.messages = {
        {.role       = "user",
         .content    = "视频里有什么?",
         .image_urls = {"https://example.com/scene.png"},
         .audio_urls = {"data:audio/mpeg;base64,SUQzBAAAAA=="},
         .video_urls = {"https://example.com/scene.mp4", "data:video/mp4;base64,AAAA"}},
    };

    try {
        co_await provider->invoke(params, nullptr);
        auto sent = neograph::json::parse(mock.lastRequestBody);
        XX_TEST_EXPECT_TRUE(sent["input"].is_array());

        const auto& item = sent["input"][0];
        XX_TEST_EXPECT_EQ(item["role"].get<std::string>(), "user");
        const auto& content = item["content"];
        XX_TEST_EXPECT_TRUE(content.is_array());
        // text + 1 image + 1 audio + 2 video
        XX_TEST_EXPECT_EQ(content.size(), (size_t)5);

        XX_TEST_EXPECT_EQ(content[0]["type"].get<std::string>(), "input_text");
        XX_TEST_EXPECT_EQ(content[0]["text"].get<std::string>(), "视频里有什么?");
        XX_TEST_EXPECT_EQ(content[1]["type"].get<std::string>(), "input_image");
        XX_TEST_EXPECT_EQ(
            content[1]["image_url"].get<std::string>(),
            "https://example.com/scene.png"
        );
        XX_TEST_EXPECT_EQ(content[2]["type"].get<std::string>(), "input_audio");
        XX_TEST_EXPECT_EQ(content[2]["input_audio"]["data"].get<std::string>(), "SUQzBAAAAA==");
        XX_TEST_EXPECT_EQ(content[2]["input_audio"]["format"].get<std::string>(), "mp3");
        // HTTP URL 视频: 扁平对象 {type, video_url}, 无 format
        XX_TEST_EXPECT_EQ(content[3]["type"].get<std::string>(), "input_video");
        XX_TEST_EXPECT_EQ(
            content[3]["video_url"].get<std::string>(),
            "https://example.com/scene.mp4"
        );
        XX_TEST_EXPECT_FALSE(content[3].contains("format"));
        // data URL 视频: video_url + format 推导
        XX_TEST_EXPECT_EQ(content[4]["type"].get<std::string>(), "input_video");
        XX_TEST_EXPECT_EQ(content[4]["video_url"].get<std::string>(), "data:video/mp4;base64,AAAA");
        XX_TEST_EXPECT_EQ(content[4]["format"].get<std::string>(), "mp4");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "multimodal responses body test failed: " << e.what() << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Tests — OpenAI Responses API (Codex)
// ---------------------------------------------------------------------------

asio::awaitable<void> test_responses_non_streaming(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::ResponsesNormal;

    auto provider = server::OpenAIProvider::create(makeCodexCfg(baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-5-codex";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Hi"}
    };

    try {
        auto result = co_await provider->invoke(params, nullptr);
        XX_TEST_EXPECT_EQ(result.message.role, "assistant");
        XX_TEST_EXPECT_TRUE(
            result.message.content.find("Hello from responses!") != std::string::npos
        );
        XX_TEST_EXPECT_TRUE(result.usage.total_tokens > 0);
        XX_TEST_EXPECT_EQ(result.stop_reason, "end_turn");

        auto sent = neograph::json::parse(mock.lastRequestBody);
        XX_TEST_EXPECT_EQ(sent["model"].get<std::string>(), "gpt-5-codex");
        // codex 默认: store=false; reasoning 不再硬编码, 由 extra_config/extra_fields 控制
        XX_TEST_EXPECT_TRUE(sent.contains("store"));
        XX_TEST_EXPECT_EQ(sent["store"].get<bool>(), false);
        XX_TEST_EXPECT_FALSE(sent.contains("reasoning"));
        // sendThinking 关闭时不请求 reasoning 摘要
        XX_TEST_EXPECT_FALSE(sent.contains("include"));
        // 无 system 消息时不应发送 instructions
        XX_TEST_EXPECT_FALSE(sent.contains("instructions"));
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "responses non-streaming failed: " << e.what() << std::endl;
    }
}

/// Responses API: reasoning 参数可通过 extra_config (config 级) 与
/// params.extra_fields (per-call 级) 配置, 不再硬编码 effort=high
asio::awaitable<void> test_responses_reasoning_configurable(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::ResponsesNormal;

    // 1) config 级: extra_config.reasoning
    {
        auto mc         = makeCodexCfg(baseUrl);
        mc.extra_config = neograph::json::parse(R"({"reasoning":{"effort":"medium"}})");
        auto                       provider = server::OpenAIProvider::create(mc);
        neograph::CompletionParams params;
        params.model    = "gpt-5-codex";
        params.messages = {
            neograph::ChatMessage{.role = "user", .content = "hi"}
        };
        try {
            co_await provider->invoke(params, nullptr);
            auto sent = neograph::json::parse(mock.lastRequestBody);
            XX_TEST_EXPECT_TRUE(sent.contains("reasoning"));
            XX_TEST_EXPECT_EQ(sent["reasoning"]["effort"].get<std::string>(), "medium");
        } catch (const std::exception& e) {
            XX_TEST_FAILED++;
            TEST_FAIL << "responses reasoning via extra_config failed: " << e.what() << std::endl;
        }
    }

    // 2) per-call 级: params.extra_fields.reasoning 覆盖 config 级
    {
        auto mc         = makeCodexCfg(baseUrl);
        mc.extra_config = neograph::json::parse(R"({"reasoning":{"effort":"medium"}})");
        auto                       provider = server::OpenAIProvider::create(mc);
        neograph::CompletionParams params;
        params.model    = "gpt-5-codex";
        params.messages = {
            neograph::ChatMessage{.role = "user", .content = "hi"}
        };
        params.extra_fields = neograph::json::parse(R"({"reasoning":{"effort":"low"}})");
        try {
            co_await provider->invoke(params, nullptr);
            auto sent = neograph::json::parse(mock.lastRequestBody);
            XX_TEST_EXPECT_TRUE(sent.contains("reasoning"));
            XX_TEST_EXPECT_EQ(sent["reasoning"]["effort"].get<std::string>(), "low");
        } catch (const std::exception& e) {
            XX_TEST_FAILED++;
            TEST_FAIL << "responses reasoning per-call override failed: " << e.what() << std::endl;
        }
    }

    // 3) OpenAI 模型支持的 reasoning.summary (摘要粒度)
    {
        auto                       mc       = makeCodexCfg(baseUrl);
        auto                       provider = server::OpenAIProvider::create(mc);
        neograph::CompletionParams params;
        params.model    = "gpt-5-codex";
        params.messages = {
            neograph::ChatMessage{.role = "user", .content = "hi"}
        };
        params.extra_fields
            = neograph::json::parse(R"({"reasoning":{"effort":"high","summary":"concise"}})");
        try {
            co_await provider->invoke(params, nullptr);
            auto sent = neograph::json::parse(mock.lastRequestBody);
            XX_TEST_EXPECT_TRUE(sent.contains("reasoning"));
            XX_TEST_EXPECT_EQ(sent["reasoning"]["effort"].get<std::string>(), "high");
            XX_TEST_EXPECT_EQ(sent["reasoning"]["summary"].get<std::string>(), "concise");
        } catch (const std::exception& e) {
            XX_TEST_FAILED++;
            TEST_FAIL << "responses reasoning summary failed: " << e.what() << std::endl;
        }
    }
}

/// Responses API: sendThinking=true 时
///   - 请求 include=["reasoning.summary"] 获取思考摘要
///   - assistant 消息携带历史 reasoning_content 时回传 reasoning item (summary 形式)
asio::awaitable<void> test_responses_send_thinking(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::ResponsesNormal;

    auto mc         = makeCodexCfg(baseUrl);
    mc.sendThinking = true;
    auto provider   = server::OpenAIProvider::create(mc);

    neograph::CompletionParams params;
    params.model    = "gpt-5-codex";
    params.messages = {
        {.role = "user", .content = "前一个问题"},
        {.role = "assistant", .content = "上一个回答", .reasoning_content = "上一轮思考过程"},
        {.role = "user", .content = "追问"},
    };

    try {
        co_await provider->invoke(params, nullptr);
        auto sent = neograph::json::parse(mock.lastRequestBody);

        // 请求 reasoning 摘要 (官方 include 值: reasoning.summary_text)
        XX_TEST_EXPECT_TRUE(sent.contains("include"));
        XX_TEST_EXPECT_TRUE(sent["include"].is_array());
        XX_TEST_EXPECT_EQ(sent["include"].size(), (size_t)1);
        XX_TEST_EXPECT_EQ(sent["include"][0].get<std::string>(), "reasoning.summary_text");

        // assistant 消息回传 reasoning item (summary 形式)
        const auto& input = sent["input"];
        XX_TEST_EXPECT_TRUE(input.is_array());
        bool foundReasoningItem = false;
        for (const auto& item : input) {
            if (item.is_object() && item.value("type", std::string{}) == "reasoning") {
                foundReasoningItem  = true;
                const auto& summary = item["summary"];
                XX_TEST_EXPECT_TRUE(summary.is_array());
                XX_TEST_EXPECT_EQ(summary.size(), (size_t)1);
                XX_TEST_EXPECT_EQ(summary[0]["type"].get<std::string>(), "summary_text");
                XX_TEST_EXPECT_EQ(summary[0]["text"].get<std::string>(), "上一轮思考过程");
            }
        }
        XX_TEST_EXPECT_TRUE(foundReasoningItem);
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "responses send thinking failed: " << e.what() << std::endl;
    }
}

/// Responses API: sendThinking=false 时不应请求 reasoning 摘要、也不回传历史 reasoning
asio::awaitable<void> test_responses_no_send_thinking(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::ResponsesNormal;

    auto provider = server::OpenAIProvider::create(makeCodexCfg(baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-5-codex";
    params.messages = {
        {.role = "assistant", .content = "上一个回答", .reasoning_content = "上一轮思考过程"},
        {.role = "user", .content = "追问"},
    };

    try {
        co_await provider->invoke(params, nullptr);
        auto sent = neograph::json::parse(mock.lastRequestBody);
        XX_TEST_EXPECT_FALSE(sent.contains("include"));
        const auto& input = sent["input"];
        XX_TEST_EXPECT_TRUE(input.is_array());
        for (const auto& item : input) {
            if (item.is_object()) {
                XX_TEST_EXPECT_FALSE(item.value("type", std::string{}) == "reasoning");
            }
        }
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "responses no-send-thinking failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void>
    test_responses_non_streaming_tool_call(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::ResponsesToolCall;

    auto provider = server::OpenAIProvider::create(makeCodexCfg(baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-5-codex";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Weather?"}
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
        XX_TEST_EXPECT_FALSE(result.message.tool_calls.empty());
        if (!result.message.tool_calls.empty()) {
            XX_TEST_EXPECT_EQ(result.message.tool_calls[0].name, "get_weather");
            XX_TEST_EXPECT_EQ(result.message.tool_calls[0].id, "call_abc123");
            XX_TEST_EXPECT_TRUE(
                result.message.tool_calls[0].arguments.find("Tokyo") != std::string::npos
            );
        }
        XX_TEST_EXPECT_EQ(result.stop_reason, "tool_use");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "responses non-streaming tool call failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_responses_streaming(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::ResponsesStreaming;
    mock.setDefaultResponsesSseChunks();

    auto provider = server::OpenAIProvider::create(makeCodexCfg(baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-5-codex";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Stream"}
    };

    std::string              accumulated;
    neograph::StreamCallback onChunk = [&](const std::string& chunk) {
        accumulated += chunk;
    };

    try {
        auto result = co_await provider->invoke(params, onChunk);
        XX_TEST_EXPECT_EQ(result.message.content, "Hello world");
        XX_TEST_EXPECT_EQ(result.message.reasoning_content, "deep thought");
        XX_TEST_EXPECT_EQ(accumulated, "Hello world");
        XX_TEST_EXPECT_EQ(result.usage.total_tokens, 8);
        XX_TEST_EXPECT_EQ(result.stop_reason, "end_turn");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "responses streaming failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_responses_streaming_tool_call(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::ResponsesStreamingToolCall;
    mock.setDefaultResponsesToolCallSseChunks();

    auto provider = server::OpenAIProvider::create(makeCodexCfg(baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-5-codex";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "Weather?"}
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

    std::string accumulated;
    try {
        auto result = co_await provider->invoke(
            params,
            neograph::StreamCallback{[&](const std::string& chunk) {
                accumulated += chunk;
            }}
        );
        XX_TEST_EXPECT_FALSE(result.message.tool_calls.empty());
        if (!result.message.tool_calls.empty()) {
            XX_TEST_EXPECT_EQ(result.message.tool_calls[0].name, "get_weather");
            XX_TEST_EXPECT_EQ(result.message.tool_calls[0].id, "call_abc123");
            XX_TEST_EXPECT_TRUE(
                result.message.tool_calls[0].arguments.find("Tokyo") != std::string::npos
            );
        }
        XX_TEST_EXPECT_EQ(result.stop_reason, "tool_use");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "responses streaming tool call failed: " << e.what() << std::endl;
    }
}

asio::awaitable<void> test_responses_rate_limit(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::RateLimit;

    auto provider = server::OpenAIProvider::create(makeCodexCfg(baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-5-codex";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "hi"}
    };

    bool caught     = false;
    int  retryAfter = -2;
    try {
        co_await provider->invoke(params, nullptr);
    } catch (const neograph::RateLimitError& e) {
        caught     = true;
        retryAfter = e.retry_after_seconds();
    } catch (const std::exception& e) {
        TEST_INFO << "responses rate limit caught generic error: " << e.what() << std::endl;
    }
    XX_TEST_EXPECT_TRUE(caught);
    XX_TEST_EXPECT_EQ(retryAfter, 5);
}

asio::awaitable<void> test_responses_server_error(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::ServerError;

    auto provider = server::OpenAIProvider::create(makeCodexCfg(baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-5-codex";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "err"}
    };

    bool caught = false;
    try {
        co_await provider->invoke(params, nullptr);
    } catch (const std::runtime_error& e) {
        caught = true;
        // extractApiError 应提取 error.message
        XX_TEST_EXPECT_TRUE(
            std::string(e.what()).find("Internal server error") != std::string::npos
        );
    } catch (...) {
    }
    XX_TEST_EXPECT_TRUE(caught);
}

// ---------------------------------------------------------------------------
// Integration tests — 畸形/非标准响应容错
// ---------------------------------------------------------------------------

/// 200 但 body 非 JSON (如网关返回 HTML 错误页): 应抛可读错误而不是 json 异常
asio::awaitable<void> test_invalid_json_response(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Raw;
    mock.rawStatus      = 200;
    mock.rawBody        = "<html><body>502 Bad Gateway</body></html>";
    mock.rawContentType = "text/html";

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-4o-mini";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "hi"}
    };

    bool        caught = false;
    std::string errMsg;
    try {
        co_await provider->invoke(params, nullptr);
    } catch (const std::exception& e) {
        caught = true;
        errMsg = e.what();
    }
    XX_TEST_EXPECT_TRUE(caught);
    XX_TEST_EXPECT_TRUE(errMsg.find("invalid JSON") != std::string::npos);
}

/// 200 合法 JSON 但缺失 choices: 应抛可读错误
asio::awaitable<void> test_missing_choices_response(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Raw;
    mock.rawStatus      = 200;
    mock.rawBody        = R"({"id":"x","object":"chat.completion","data":[]})";
    mock.rawContentType = "application/json";

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-4o-mini";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "hi"}
    };

    bool        caught = false;
    std::string errMsg;
    try {
        co_await provider->invoke(params, nullptr);
    } catch (const std::exception& e) {
        caught = true;
        errMsg = e.what();
    }
    XX_TEST_EXPECT_TRUE(caught);
    XX_TEST_EXPECT_TRUE(errMsg.find("choices") != std::string::npos);
}

/// 部分网关返回 201/202 等其它 2xx 状态码也应视为成功
asio::awaitable<void> test_non_200_2xx_accepted(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Raw;
    mock.rawStatus      = 201;
    mock.rawBody        = MockOpenAIServer{}.makeCompletionResponse("Created ok").dump();
    mock.rawContentType = "application/json";

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-4o-mini";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "hi"}
    };

    try {
        auto result = co_await provider->invoke(params, nullptr);
        XX_TEST_EXPECT_EQ(result.message.content, "Created ok");
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "2xx (201) response should succeed: " << e.what() << std::endl;
    }
}

/// 错误 body 使用顶层 {"message": ...} (无 error 包裹) 时也应提取
asio::awaitable<void> test_error_top_level_message(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::Raw;
    mock.rawStatus      = 500;
    mock.rawBody        = R"({"message":"upstream model overloaded"})";
    mock.rawContentType = "application/json";

    auto provider = server::OpenAIProvider::create(makeOaiCfg("sk-test", baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-4o-mini";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "hi"}
    };

    bool        caught = false;
    std::string errMsg;
    try {
        co_await provider->invoke(params, nullptr);
    } catch (const std::exception& e) {
        caught = true;
        errMsg = e.what();
    }
    XX_TEST_EXPECT_TRUE(caught);
    XX_TEST_EXPECT_TRUE(errMsg.find("upstream model overloaded") != std::string::npos);
}

/// Responses API: 200 + status="failed" 应报错而不是静默返回空结果
asio::awaitable<void> test_responses_status_failed(MockOpenAIServer& mock, uint16_t port) {
    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    mock.mode           = MockMode::ResponsesNormal;
    mock.customResponse = neograph::json::parse(
        R"({"id":"resp_failed","object":"response","status":"failed",
            "error":{"message":"content policy violation","code":"content_filter"}})"
    );

    auto provider = server::OpenAIProvider::create(makeCodexCfg(baseUrl));

    neograph::CompletionParams params;
    params.model    = "gpt-5-codex";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = "hi"}
    };

    bool        caught = false;
    std::string errMsg;
    try {
        co_await provider->invoke(params, nullptr);
    } catch (const std::exception& e) {
        caught = true;
        errMsg = e.what();
    }
    XX_TEST_EXPECT_TRUE(caught);
    XX_TEST_EXPECT_TRUE(errMsg.find("content policy violation") != std::string::npos);
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
        std::string              buf = "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\n";
        neograph::ChatCompletion completion;
        std::string              content, thinking;
        std::map<int, neograph::ToolCall> tcMap;
        bool                              done
            = OpenAIProvider::processSseBuffer(buf, completion, content, thinking, tcMap, nullptr);
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
        bool                              done
            = OpenAIProvider::processSseBuffer(buf, completion, content, thinking, tcMap, nullptr);
        XX_TEST_EXPECT_TRUE(done);
    }

    // [DONE] 无结尾换行, finalFlush 时也应识别
    {
        std::string                       buf = "data: [DONE]";
        neograph::ChatCompletion          completion;
        std::string                       content, thinking;
        std::map<int, neograph::ToolCall> tcMap;
        bool                              done = OpenAIProvider::processSseBuffer(
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

    // [DONE] 行尾被网关附加空白时也应识别
    {
        std::string                       buf = "data: [DONE] \t\n\n";
        neograph::ChatCompletion          completion;
        std::string                       content, thinking;
        std::map<int, neograph::ToolCall> tcMap;
        bool                              done
            = OpenAIProvider::processSseBuffer(buf, completion, content, thinking, tcMap, nullptr);
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
    test_openai_sse_malformed_types_tolerated();
    test_responses_sse_incomplete_done();

    // ModelProviderRegistry::createProvider tests
    test_create_provider_openai();
    test_create_provider_anthropic();
    test_create_provider_default_type();
    test_create_provider_codex();
    test_create_provider_openai_responses_flag();

    // Responses API SSE 解析单测
    test_responses_sse_parsing_edge_cases();

    // Unit tests for reasoning/thinking parsing (no server needed)
    test_parse_response_message_with_reasoning();
    test_parse_response_message_with_reasoning_field();
    test_parse_response_message_null_reasoning_field();
    test_parse_response_message_with_thinking_field();
    test_parse_response_message_without_reasoning();
    test_parse_response_message_null_reasoning();
    test_parse_response_message_reasoning_preferred_over_thinking();
    test_messages_to_json_with_reasoning();
    test_messages_to_json_without_reasoning();
    test_messages_to_json_reasoning_roundtrip();

    // Unit tests for multimodal messages (image / audio / video)
    test_parse_data_url();
    test_media_format_from_mime();
    test_messages_to_json_multimodal();
    test_messages_to_json_multimodal_empty_content();
    test_messages_to_json_multimodal_roundtrip();

    // Unit tests for <think> tag extraction (no server needed)
    test_extract_think_tags_basic();
    test_extract_think_tags_no_tags();
    test_extract_think_tags_unclosed();
    test_extract_think_tags_multiple_blocks();
    test_extract_think_tags_empty_block();
    test_extract_think_tags_only_think();
    test_extract_think_tags_closed_then_unclosed();
    test_extract_think_tags_unclosed_direct();

    // Unit tests for XML 风格 <tool_call> 提取 (no server needed)
    test_extract_tool_calls_xml_in_content();
    test_extract_tool_calls_xml_with_args();
    test_extract_tool_calls_xml_bare_function();
    test_extract_tool_calls_xml_unclosed();
    test_extract_tool_calls_xml_case_insensitive();
    test_extract_tool_calls_xml_with_json_block();
    test_extract_tool_calls_xml_no_call_keeps_text();
    test_extract_tool_calls_xml_in_thinking();
    test_extract_tool_calls_xml_multiple();
    test_extract_tool_calls_xml_trailing_text();

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

    // OpenAI 兼容性增强测试
    co_await test_max_tokens_sent(*mock, port);
    co_await test_max_completion_tokens_sent(*mock, port);
    co_await test_stop_reason_mapping(*mock, port);
    co_await test_custom_api_path(*mock, port);
    co_await test_send_temperature_disabled(*mock, port);
    co_await test_extra_headers_sent(*mock, port);

    // 多模态消息体 (图片/音频/视频附件)
    co_await test_multimodal_body_chat_completions(*mock, port);
    co_await test_multimodal_body_responses(*mock, port);

    // Responses API (Codex) 测试
    co_await test_responses_non_streaming(*mock, port);
    co_await test_responses_reasoning_configurable(*mock, port);
    co_await test_responses_send_thinking(*mock, port);
    co_await test_responses_no_send_thinking(*mock, port);
    co_await test_responses_non_streaming_tool_call(*mock, port);
    co_await test_responses_streaming(*mock, port);
    co_await test_responses_streaming_tool_call(*mock, port);
    co_await test_responses_rate_limit(*mock, port);
    co_await test_responses_server_error(*mock, port);

    // 畸形/非标准响应容错测试
    co_await test_invalid_json_response(*mock, port);
    co_await test_missing_choices_response(*mock, port);
    co_await test_non_200_2xx_accepted(*mock, port);
    co_await test_error_top_level_message(*mock, port);
    co_await test_responses_status_failed(*mock, port);

    // Reasoning/thinking content tests
    co_await test_non_streaming_reasoning_content(*mock, port);
    co_await test_non_streaming_thinking_field(*mock, port);
    co_await test_non_streaming_reasoning_at_choice_level(*mock, port);
    co_await test_non_streaming_thinking_at_choice_level(*mock, port);
    co_await test_streaming_reasoning_content(*mock, port);
    co_await test_streaming_thinking_field_compat(*mock, port);
    co_await test_streaming_reasoning_field_compat(*mock, port);
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
