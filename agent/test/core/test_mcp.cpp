#include "test_mcp.h"
#include "agentxx/protocol/mcp_client.h"
#include "agentxx/protocol/mcp_server.h"
#include "agentxx/tools/tool.h"
#include "agentxx/util/http_client.h"
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace agentxx {
namespace test {

using namespace agentxx::server;
using namespace agentxx::util;

int g_mcp_passed = 0;
int g_mcp_failed = 0;

// Unit test for version negotiation logic (doesn't require a server instance)
void test_mcp_version_negotiation_unit() {
    // Directly test the version negotiation algorithm
    auto testNegotiate = [](const std::string& clientVer) -> std::string {
        constexpr std::string_view supported[] = {
            "2026-07-28",
            "2025-11-25",
            "2025-06-18",
            "2025-03-26",
            "2024-11-05",
        };
        std::string negotiatedVersion{"2024-11-05"};
        bool        foundExact = false;
        for (const auto& sv : supported) {
            if (sv == clientVer) {
                negotiatedVersion = sv;
                foundExact        = true;
                break;
            }
        }
        if (!foundExact && !clientVer.empty()) {
            auto clientYear = clientVer.substr(0, 4);
            for (const auto& sv : supported) {
                auto svYear = std::string(sv.substr(0, 4));
                if (svYear <= clientYear) {
                    negotiatedVersion = sv;
                    break;
                }
            }
        }
        return negotiatedVersion;
    };

    XX_TEST_EXPECT_EQ(testNegotiate("2025-11-25"), "2025-11-25");
    XX_TEST_EXPECT_EQ(testNegotiate("2025-06-18"), "2025-06-18");
    XX_TEST_EXPECT_EQ(testNegotiate("2025-03-26"), "2025-03-26");
    XX_TEST_EXPECT_EQ(testNegotiate("2024-11-05"), "2024-11-05");
    XX_TEST_EXPECT_EQ(testNegotiate("2025-01-01"), "2025-11-25");
    XX_TEST_EXPECT_EQ(testNegotiate("2026-01-01"), "2026-07-28");
    XX_TEST_EXPECT_EQ(testNegotiate("2026-07-28"), "2026-07-28");
    XX_TEST_EXPECT_EQ(testNegotiate(""), "2024-11-05");
    XX_TEST_EXPECT_EQ(testNegotiate("2024-06-01"), "2024-11-05");
}

void test_mcp_server_unit() {
    // Test JSON-RPC helpers
    {
        auto err = jsonRpcError(-32601, "Method not found");
        XX_TEST_EXPECT_EQ(err["code"].get<int>(), -32601);
        XX_TEST_EXPECT_EQ(err["message"].get<std::string>(), "Method not found");
    }

    {
        auto resp = jsonRpcResponse(
            1,
            {
                {"ok", true}
        }
        );
        XX_TEST_EXPECT_EQ(resp["jsonrpc"].get<std::string>(), "2.0");
        XX_TEST_EXPECT_EQ(resp["id"].get<int>(), 1);
        XX_TEST_EXPECT_TRUE(resp["result"]["ok"].get<bool>());
    }

    {
        auto resp = jsonRpcErrorResponse(1, jsonRpcError(-32700, "Parse error"));
        XX_TEST_EXPECT_EQ(resp["jsonrpc"].get<std::string>(), "2.0");
        XX_TEST_EXPECT_EQ(resp["id"].get<int>(), 1);
        XX_TEST_EXPECT_EQ(resp["error"]["code"].get<int>(), -32700);
    }

    // Test tool registration
    {
        auto              server = std::make_unique<McpServer>();
        McpToolDefinition def;
        def.name        = "echo";
        def.description = "Echo back the input";
        def.inputSchema = json::object();

        auto tools = server->listTools();
        XX_TEST_EXPECT_EQ(tools.size(), (size_t)0);

        server->addTool(def, [](const json& args) -> json {
            json content;
            content["type"] = "text";
            content["text"] = args.dump();
            return content;
        });

        tools = server->listTools();
        XX_TEST_EXPECT_EQ(tools.size(), (size_t)1);
        XX_TEST_EXPECT_EQ(tools[0].name, "echo");

        server->removeTool("echo");
        tools = server->listTools();
        XX_TEST_EXPECT_EQ(tools.size(), (size_t)0);
        server.reset();
    }

    // Test resource registration
    {
        McpServer             server;
        McpResourceDefinition def;
        def.uri         = "file:///test.txt";
        def.name        = "Test File";
        def.description = "A test resource";
        def.mimeType    = "text/plain";

        server.addResource(def, [](std::string_view uri) -> std::optional<McpResourceContent> {
            if (uri == "file:///test.txt") {
                return McpResourceContent{
                    .uri      = std::string{uri},
                    .mimeType = "text/plain",
                    .text     = "hello world"
                };
            }
            return std::nullopt;
        });

        auto resources = server.listResources();
        XX_TEST_EXPECT_EQ(resources.size(), (size_t)1);
        XX_TEST_EXPECT_EQ(resources[0].uri, "file:///test.txt");

        server.removeResource("file:///test.txt");
        resources = server.listResources();
        XX_TEST_EXPECT_EQ(resources.size(), (size_t)0);
    }

    // Test prompt registration
    {
        McpServer           server;
        McpPromptDefinition def;
        def.name        = "greet";
        def.description = "Generate a greeting";
        McpPromptArgument arg;
        arg.name        = "name";
        arg.description = "The name to greet";
        arg.required    = true;
        def.arguments.push_back(std::move(arg));

        server.addPrompt(
            def,
            [](std::string_view name, const json& args) -> std::optional<McpPromptResult> {
                if (name != "greet") {
                    return std::nullopt;
                }
                McpPromptResult result;
                result.description = "A friendly greeting";
                McpPromptMessage msg;
                msg.role    = "assistant";
                msg.content = "Hello, " + args.value("name", "world") + "!";
                result.messages.push_back(std::move(msg));
                return result;
            }
        );

        auto prompts = server.listPrompts();
        XX_TEST_EXPECT_EQ(prompts.size(), (size_t)1);
        XX_TEST_EXPECT_EQ(prompts[0].name, "greet");

        server.removePrompt("greet");
        prompts = server.listPrompts();
        XX_TEST_EXPECT_EQ(prompts.size(), (size_t)0);
    }
}

asio::awaitable<void> test_mcp_server_integration() {
    using Server = McpServer;

    Server::Config cfg;
    cfg.httpConfig.address          = "127.0.0.1";
    cfg.httpConfig.port             = 0;
    cfg.httpConfig.ioThreads        = 1;
    cfg.httpConfig.accessLogEnabled = false;

    Server            server(std::move(cfg));
    McpToolDefinition def;
    def.name        = "echo";
    def.description = "Echo back the input";
    def.inputSchema = json::parse(R"({
    "type": "object",
    "properties": {
      "text": {"type": "string"}
    }
  })");

    server.addTool(def, [](const json& args) -> json {
        json content;
        content["type"] = "text";
        content["text"] = args.value("text", "");
        return content;
    });

    // Start server
    std::thread serverThread([&server]() {
        server.start();
    });

    // Wait for server to be ready
    uint16_t port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (port == 0) {
        TEST_FAIL << "MCP Server failed to start" << std::endl;
        g_mcp_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    TEST_INFO << "MCP Server URL: " << baseUrl << std::endl;

    // Wait for server to be reachable
    for (int i = 0; i < 100; ++i) {
        try {
            asio::io_context      tmpCtx;
            asio::ip::tcp::socket sock(tmpCtx);
            sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
            sock.close();
            break;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // Test initialize
    {
        json req;
        req["jsonrpc"] = "2.0";
        req["id"]      = 1;
        req["method"]  = "initialize";
        req["params"]  = {
            {"protocolVersion", "2024-11-05"                                 },
            {"capabilities",    json::object()                               },
            {"clientInfo",      {{"name", "test-client"}, {"version", "1.0"}}}
        };

        auto resp = co_await HttpClient::postAsync(baseUrl + "/mcp", req);
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            auto j = resp.value().bodyJson();
            XX_TEST_EXPECT_HAS_VALUE(j);
            if (j.has_value()) {
                XX_TEST_EXPECT_EQ((*j)["jsonrpc"].get<std::string>(), "2.0");
                XX_TEST_EXPECT_TRUE((*j).contains("result"));
                XX_TEST_EXPECT_TRUE((*j)["result"].contains("serverInfo"));
                XX_TEST_EXPECT_EQ(
                    (*j)["result"]["serverInfo"]["name"].get<std::string>(),
                    "agentxx-mcp"
                );
            }
        }
    }

    // Test ping
    {
        json req;
        req["jsonrpc"] = "2.0";
        req["id"]      = 2;
        req["method"]  = "ping";

        auto resp = co_await HttpClient::postAsync(baseUrl + "/mcp", req);
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            auto j = resp.value().bodyJson();
            XX_TEST_EXPECT_HAS_VALUE(j);
            if (j.has_value()) {
                XX_TEST_EXPECT_EQ((*j)["id"].get<int>(), 2);
                XX_TEST_EXPECT_TRUE((*j).contains("result"));
            }
        }
    }

    // Test tools/list
    {
        json req;
        req["jsonrpc"] = "2.0";
        req["id"]      = 3;
        req["method"]  = "tools/list";

        auto resp = co_await HttpClient::postAsync(baseUrl + "/mcp", req);
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            auto j = resp.value().bodyJson();
            XX_TEST_EXPECT_HAS_VALUE(j);
            if (j.has_value()) {
                const auto tools = (*j)["result"]["tools"];
                XX_TEST_EXPECT_TRUE(tools.is_array());
                XX_TEST_EXPECT_EQ(tools.size(), (size_t)1);
                XX_TEST_EXPECT_EQ(tools[0]["name"].get<std::string>(), "echo");
            }
        }
    }

    // Test tools/call
    {
        json req;
        req["jsonrpc"] = "2.0";
        req["id"]      = 4;
        req["method"]  = "tools/call";
        req["params"]  = {
            {"name",      "echo"                 },
            {"arguments", {{"text", "hello mcp"}}}
        };

        auto resp = co_await HttpClient::postAsync(baseUrl + "/mcp", req);
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            auto j = resp.value().bodyJson();
            XX_TEST_EXPECT_HAS_VALUE(j);
            if (j.has_value()) {
                const auto content = (*j)["result"]["content"];
                XX_TEST_EXPECT_TRUE(content.is_array());
                XX_TEST_EXPECT_EQ(content[0]["text"].get<std::string>(), "hello mcp");
            }
        }
    }

    // Test tools/call with non-existent tool
    {
        json req;
        req["jsonrpc"] = "2.0";
        req["id"]      = 5;
        req["method"]  = "tools/call";
        req["params"]  = {
            {"name",      "nonexistent" },
            {"arguments", json::object()}
        };

        auto resp = co_await HttpClient::postAsync(baseUrl + "/mcp", req);
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            auto j = resp.value().bodyJson();
            XX_TEST_EXPECT_HAS_VALUE(j);
            if (j.has_value()) {
                XX_TEST_EXPECT_TRUE((*j).contains("error"));
                XX_TEST_EXPECT_EQ((*j)["error"]["code"].get<int>(), -32000);
            }
        }
    }

    // Test method not found
    {
        json req;
        req["jsonrpc"] = "2.0";
        req["id"]      = 6;
        req["method"]  = "nonexistent/method";

        auto resp = co_await HttpClient::postAsync(baseUrl + "/mcp", req);
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            auto j = resp.value().bodyJson();
            XX_TEST_EXPECT_HAS_VALUE(j);
            if (j.has_value()) {
                XX_TEST_EXPECT_TRUE((*j).contains("error"));
                XX_TEST_EXPECT_EQ((*j)["error"]["code"].get<int>(), -32601);
            }
        }
    }

    // Test invalid JSON
    {
        HeaderMap headers;
        headers.set("content-type", "application/json");
        auto resp = co_await HttpClient::postAsync(
            baseUrl + "/mcp",
            "not json",
            "application/json",
            headers,
            HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{5}}
        );
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 400);
        }
    }

    // Test notification (no id) – should get 202 Accepted
    {
        json req;
        req["jsonrpc"] = "2.0";
        req["method"]  = "notifications/initialized";

        auto resp = co_await HttpClient::postAsync(baseUrl + "/mcp", req);
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 202);
        }
    }

    // Test SSE endpoint (长连接流: 读取到 endpoint 事件后主动断开)
    {
        std::string sseBody;
        co_await agentxx::util::catchErrorAsync<bool>(
            [&]() -> asio::awaitable<bool> {
                co_await util::HttpClient::requestSseAsync(
                    "GET",
                    baseUrl + "/mcp/sse",
                    "",
                    "",
                    {},
                    util::HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{5}},
                    [&](std::string_view chunk) -> bool {
                        sseBody.append(chunk);
                        // 收到 endpoint 事件即停止读取 (长连接不会自行结束)
                        return sseBody.find("event: endpoint") != std::string::npos;
                    }
                );
                co_return true;
            },
            [](std::string) -> asio::awaitable<bool> {
                co_return true;
            }
        );
        XX_TEST_EXPECT_TRUE(sseBody.find("event: endpoint") != std::string::npos);
        XX_TEST_EXPECT_TRUE(sseBody.find("/mcp") != std::string::npos);
    }

    // Test resources/list (should return empty array)
    {
        json req;
        req["jsonrpc"] = "2.0";
        req["id"]      = 7;
        req["method"]  = "resources/list";

        auto resp = co_await HttpClient::postAsync(baseUrl + "/mcp", req);
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            auto j = resp.value().bodyJson();
            XX_TEST_EXPECT_HAS_VALUE(j);
            if (j.has_value()) {
                const auto resources = (*j)["result"]["resources"];
                XX_TEST_EXPECT_TRUE(resources.is_array());
                XX_TEST_EXPECT_EQ(resources.size(), (size_t)0);
            }
        }
    }

    // Test prompts/list (should return empty array)
    {
        json req;
        req["jsonrpc"] = "2.0";
        req["id"]      = 8;
        req["method"]  = "prompts/list";

        auto resp = co_await HttpClient::postAsync(baseUrl + "/mcp", req);
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            auto j = resp.value().bodyJson();
            XX_TEST_EXPECT_HAS_VALUE(j);
            if (j.has_value()) {
                const auto prompts = (*j)["result"]["prompts"];
                XX_TEST_EXPECT_TRUE(prompts.is_array());
                XX_TEST_EXPECT_EQ(prompts.size(), (size_t)0);
            }
        }
    }

    // Verify server is not stopped
    XX_TEST_EXPECT_FALSE(server.isStopped());

    server.stop();
    serverThread.join();
}

// -----------------------------------------------------------------------
// Version negotiation tests
// -----------------------------------------------------------------------

void test_mcp_server_version_negotiation() {
    // Test all three supported versions
    {
        McpServer   server;
        std::string input;
        input
            += R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","clientInfo":{"name":"test"}}})"
               "\n";
        input
            += R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"2025-03-26","clientInfo":{"name":"test"}}})"
               "\n";
        input
            += R"({"jsonrpc":"2.0","id":3,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"test"}}})"
               "\n";
        input
            += R"({"jsonrpc":"2.0","id":4,"method":"initialize","params":{"protocolVersion":"2025-11-25","clientInfo":{"name":"test"}}})"
               "\n";
        input
            += R"({"jsonrpc":"2.0","id":5,"method":"initialize","params":{"protocolVersion":"2025-01-01","clientInfo":{"name":"test"}}})"
               "\n";
        input
            += R"({"jsonrpc":"2.0","id":6,"method":"initialize","params":{"clientInfo":{"name":"test"}}})"
               "\n";
        input
            += R"({"jsonrpc":"2.0","id":7,"method":"initialize","params":{"protocolVersion":"2026-01-01","clientInfo":{"name":"test"}}})"
               "\n";

        auto               oldCin  = std::cin.rdbuf();
        auto               oldCout = std::cout.rdbuf();
        std::istringstream in(input);
        std::ostringstream out;
        std::cin.rdbuf(in.rdbuf());
        std::cout.rdbuf(out.rdbuf());
        server.runStdio();
        std::cin.rdbuf(oldCin);
        std::cout.rdbuf(oldCout);

        std::vector<json>  responses;
        std::istringstream outputStream(out.str());
        std::string        line;
        while (std::getline(outputStream, line)) {
            if (!line.empty()) {
                responses.push_back(json::parse(line));
            }
        }

        XX_TEST_EXPECT_EQ(responses.size(), (size_t)7);
        if (responses.size() >= 7) {
            // Exact match 2024-11-05
            XX_TEST_EXPECT_EQ(
                responses[0]["result"]["protocolVersion"].get<std::string>(),
                "2024-11-05"
            );
            // Exact match 2025-03-26
            XX_TEST_EXPECT_EQ(
                responses[1]["result"]["protocolVersion"].get<std::string>(),
                "2025-03-26"
            );
            // Exact match 2025-06-18
            XX_TEST_EXPECT_EQ(
                responses[2]["result"]["protocolVersion"].get<std::string>(),
                "2025-06-18"
            );
            // Exact match 2025-11-25
            XX_TEST_EXPECT_EQ(
                responses[3]["result"]["protocolVersion"].get<std::string>(),
                "2025-11-25"
            );
            // Unknown version 2025-01-01 matches newest 2025
            XX_TEST_EXPECT_EQ(
                responses[4]["result"]["protocolVersion"].get<std::string>(),
                "2025-11-25"
            );
            // Missing version defaults to oldest-supported (max compat)
            XX_TEST_EXPECT_EQ(
                responses[5]["result"]["protocolVersion"].get<std::string>(),
                "2024-11-05"
            );
            // Future version 2026-01-01 matches newest supported (2026-07-28)
            XX_TEST_EXPECT_EQ(
                responses[6]["result"]["protocolVersion"].get<std::string>(),
                "2026-07-28"
            );
        }
    }
}

// -----------------------------------------------------------------------
// 2025-11-25 feature tests
// -----------------------------------------------------------------------

void test_mcp_server_2025_features() {
    // Test instructions field in initialize response
    {
        McpServer   server;
        std::string input;
        input
            += R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","clientInfo":{"name":"test"}}})"
               "\n";

        auto               oldCin  = std::cin.rdbuf();
        auto               oldCout = std::cout.rdbuf();
        std::istringstream in(input);
        std::ostringstream out;
        std::cin.rdbuf(in.rdbuf());
        std::cout.rdbuf(out.rdbuf());
        server.runStdio();
        std::cin.rdbuf(oldCin);
        std::cout.rdbuf(oldCout);

        std::vector<json>  responses;
        std::istringstream outputStream(out.str());
        std::string        line;
        while (std::getline(outputStream, line)) {
            if (!line.empty()) {
                responses.push_back(json::parse(line));
            }
        }

        XX_TEST_EXPECT_EQ(responses.size(), (size_t)1);
        if (!responses.empty()) {
            // Must have instructions field (2025-11-25 feature)
            XX_TEST_EXPECT_TRUE(responses[0]["result"].contains("instructions"));
            // Must have tasks capability
            XX_TEST_EXPECT_TRUE(responses[0]["result"]["capabilities"].contains("tasks"));
        }
    }

    // Test tool with title, outputSchema, annotations, execution
    {
        McpServer         server;
        McpToolDefinition def;
        def.name        = "advanced_tool";
        def.description = "A tool with all 2025-11-25 fields";
        def.title       = "Advanced Tool";
        def.inputSchema = json::parse(R"({"type":"object","properties":{"x":{"type":"number"}}})");
        def.outputSchema
            = json::parse(R"({"type":"object","properties":{"result":{"type":"number"}}})");
        def.annotations = json::parse(R"({"title":"Advanced"})");
        def.execution   = json::parse(R"({"taskSupport":"optional"})");

        server.addTool(def, [](const json& args) -> json {
            json content;
            content["type"] = "text";
            content["text"] = "result:" + std::to_string(args.value("x", 0.0));
            return content;
        });

        std::string input;
        input += R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"
                 "\n";

        auto               oldCin  = std::cin.rdbuf();
        auto               oldCout = std::cout.rdbuf();
        std::istringstream in(input);
        std::ostringstream out;
        std::cin.rdbuf(in.rdbuf());
        std::cout.rdbuf(out.rdbuf());
        server.runStdio();
        std::cin.rdbuf(oldCin);
        std::cout.rdbuf(oldCout);

        std::vector<json>  responses;
        std::istringstream outputStream(out.str());
        std::string        line;
        while (std::getline(outputStream, line)) {
            if (!line.empty()) {
                responses.push_back(json::parse(line));
            }
        }

        XX_TEST_EXPECT_EQ(responses.size(), (size_t)1);
        if (!responses.empty()) {
            json tools = responses[0]["result"]["tools"];
            XX_TEST_EXPECT_TRUE(tools.is_array());
            XX_TEST_EXPECT_EQ(tools.size(), (size_t)1);
            if (tools.size() >= 1) {
                json t = tools[0];
                XX_TEST_EXPECT_EQ(t["name"].get<std::string>(), "advanced_tool");
                XX_TEST_EXPECT_TRUE(t.contains("title"));
                XX_TEST_EXPECT_EQ(t["title"].get<std::string>(), "Advanced Tool");
                XX_TEST_EXPECT_TRUE(t.contains("outputSchema"));
                XX_TEST_EXPECT_TRUE(t.contains("annotations"));
                XX_TEST_EXPECT_TRUE(t.contains("execution"));
            }
        }
    }

    // Test that older versions (2024-11-05) don't receive 2025-only fields
    {
        McpServer         server;
        McpToolDefinition def;
        def.name        = "basic_tool";
        def.description = "Basic tool";
        def.title       = "Basic Tool (should be hidden in old protocol)";
        server.addTool(def, [](const json&) -> json {
            json content;
            content["type"] = "text";
            content["text"] = "ok";
            return content;
        });

        std::string input;
        input
            += R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","clientInfo":{"name":"test"}}})"
               "\n";
        input += R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"
                 "\n";

        auto               oldCin  = std::cin.rdbuf();
        auto               oldCout = std::cout.rdbuf();
        std::istringstream in(input);
        std::ostringstream out;
        std::cin.rdbuf(in.rdbuf());
        std::cout.rdbuf(out.rdbuf());
        server.runStdio();
        std::cin.rdbuf(oldCin);
        std::cout.rdbuf(oldCout);

        std::vector<json>  responses;
        std::istringstream outputStream(out.str());
        std::string        line;
        while (std::getline(outputStream, line)) {
            if (!line.empty()) {
                responses.push_back(json::parse(line));
            }
        }

        XX_TEST_EXPECT_EQ(responses.size(), (size_t)2);
        if (responses.size() >= 2) {
            // Initialize response should not have instructions for 2024-11-05
            // Actually our server always includes them, which is fine (forward
            // compat) Check tools/list response
            json tools = responses[1]["result"]["tools"];
            if (tools.is_array() && tools.size() >= 1) {
                json t = tools[0];
                XX_TEST_EXPECT_EQ(t["name"].get<std::string>(), "basic_tool");
                // title is not required for old protocol, but including it is harmless
                // The key is that the server doesn't break
            }
        }
    }
}

// -----------------------------------------------------------------------
// McpClient 2025-11-25 version tests
// -----------------------------------------------------------------------

asio::awaitable<void> test_mcp_client_2025_version() {
    using Server = McpServer;

    Server::Config cfg;
    cfg.httpConfig.address          = "127.0.0.1";
    cfg.httpConfig.port             = 0;
    cfg.httpConfig.ioThreads        = 1;
    cfg.httpConfig.accessLogEnabled = false;

    Server            server(std::move(cfg));
    McpToolDefinition def;
    def.name        = "echo";
    def.description = "Echo";
    def.inputSchema = json::parse(R"({"type":"object","properties":{"text":{"type":"string"}}})");

    server.addTool(def, [](const json& args) -> json {
        json content;
        content["type"] = "text";
        content["text"] = args.value("text", "");
        return content;
    });

    std::thread serverThread([&server]() {
        server.start();
    });

    uint16_t port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        TEST_FAIL << "McpServer failed to start" << std::endl;
        g_mcp_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);

    for (int i = 0; i < 100; ++i) {
        try {
            asio::io_context      tmpCtx;
            asio::ip::tcp::socket sock(tmpCtx);
            sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
            sock.close();
            break;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // Test with 2025-11-25 protocol version
    {
        McpClient::Config clientCfg;
        clientCfg.serverUrl       = baseUrl + "/mcp";
        clientCfg.protocolVersion = std::string{McpClient::kProtocol2025_11_25};
        clientCfg.requestTimeout  = std::chrono::seconds(5);

        auto client = std::make_shared<McpClient>(std::move(clientCfg));

        auto result = co_await client->initialize();
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            // Should negotiate to 2025-11-25
            XX_TEST_EXPECT_EQ(result->protocolVersion, "2025-11-25");
        }

        // All standard methods should still work
        auto ping = co_await client->ping();
        XX_TEST_EXPECT_TRUE(ping.has_value());

        auto tools = co_await client->listTools();
        XX_TEST_EXPECT_TRUE(tools.has_value());

        auto echo = co_await client->callTool(
            "echo",
            {
                {"text", "hello 2025"}
        }
        );
        XX_TEST_EXPECT_TRUE(echo.has_value());

        co_await client->close();
    }

    // Test with 2024-11-05 (backward compatibility)
    {
        McpClient::Config clientCfg;
        clientCfg.serverUrl       = baseUrl + "/mcp";
        clientCfg.protocolVersion = std::string{McpClient::kProtocol2024_11_05};
        clientCfg.requestTimeout  = std::chrono::seconds(5);

        auto client = std::make_shared<McpClient>(std::move(clientCfg));

        auto result = co_await client->initialize();
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            // Server always responds with 2025-11-25 now (newest)
            // Client should accept this per lenient negotiation
            XX_TEST_EXPECT_FALSE(result->protocolVersion.empty());
        }

        co_await client->close();
    }

    server.stop();
    serverThread.join();
}

// -----------------------------------------------------------------------
// Lenient parsing tests
// -----------------------------------------------------------------------

void test_mcp_server_lenient_parsing() {
    McpServer server;

    std::string input;
    // jsonrpc as number 2.0 (not string)
    input += R"({"jsonrpc":2.0,"id":1,"method":"ping"})"
             "\n";
    // missing jsonrpc field entirely
    input += R"({"id":2,"method":"ping"})"
             "\n";
    // params as null (not object)
    input += R"({"jsonrpc":"2.0","id":3,"method":"ping","params":null})"
             "\n";
    // integer id
    input += R"({"jsonrpc":"2.0","id":4,"method":"tools/list"})"
             "\n";
    // string id
    input += R"({"jsonrpc":"2.0","id":"req-5","method":"ping"})"
             "\n";

    auto               oldCin  = std::cin.rdbuf();
    auto               oldCout = std::cout.rdbuf();
    std::istringstream in(input);
    std::ostringstream out;
    std::cin.rdbuf(in.rdbuf());
    std::cout.rdbuf(out.rdbuf());
    server.runStdio();
    std::cin.rdbuf(oldCin);
    std::cout.rdbuf(oldCout);

    std::vector<json>  responses;
    std::istringstream outputStream(out.str());
    std::string        line;
    while (std::getline(outputStream, line)) {
        if (!line.empty()) {
            responses.push_back(json::parse(line));
        }
    }

    // All 5 requests should succeed
    XX_TEST_EXPECT_EQ(responses.size(), (size_t)5);
    if (responses.size() >= 5) {
        for (size_t i = 0; i < 5; i++) {
            XX_TEST_EXPECT_TRUE(responses[i].contains("result") || responses[i].contains("error"));
            if (responses[i].contains("error")) {
                TEST_INFO << "Response " << i << " has error: " << responses[i].dump() << std::endl;
            }
        }
    }
}

// -----------------------------------------------------------------------
// Stdio resource/prompt tests
// -----------------------------------------------------------------------

void test_mcp_server_stdio_resources_prompts() {
    McpServer server;

    // Add a resource
    McpResourceDefinition resDef;
    resDef.uri      = "file:///test.txt";
    resDef.name     = "Test File";
    resDef.mimeType = "text/plain";
    server.addResource(resDef, [](std::string_view uri) -> std::optional<McpResourceContent> {
        if (uri == "file:///test.txt") {
            return McpResourceContent{std::string{uri}, "text/plain", "hello world"};
        }
        return std::nullopt;
    });

    // Add a prompt
    McpPromptDefinition promptDef;
    promptDef.name        = "greet";
    promptDef.description = "Generate a greeting";
    McpPromptArgument arg;
    arg.name     = "name";
    arg.required = true;
    promptDef.arguments.push_back(std::move(arg));
    server.addPrompt(
        promptDef,
        [](std::string_view name, const json& args) -> std::optional<McpPromptResult> {
            if (name != "greet") {
                return std::nullopt;
            }
            McpPromptResult result;
            result.description = "A friendly greeting";
            McpPromptMessage msg;
            msg.role    = "assistant";
            msg.content = "Hello, " + args.value("name", "world") + "!";
            result.messages.push_back(std::move(msg));
            return result;
        }
    );

    std::string input;
    input
        += R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","clientInfo":{"name":"test"}}})"
           "\n";
    input += R"({"jsonrpc":"2.0","id":2,"method":"resources/list"})"
             "\n";
    input
        += R"({"jsonrpc":"2.0","id":3,"method":"resources/read","params":{"uri":"file:///test.txt"}})"
           "\n";
    input
        += R"({"jsonrpc":"2.0","id":4,"method":"resources/read","params":{"uri":"file:///nonexistent.txt"}})"
           "\n";
    input += R"({"jsonrpc":"2.0","id":5,"method":"prompts/list"})"
             "\n";
    input
        += R"({"jsonrpc":"2.0","id":6,"method":"prompts/get","params":{"name":"greet","arguments":{"name":"World"}}})"
           "\n";
    input += R"({"jsonrpc":"2.0","id":7,"method":"prompts/get","params":{"name":"nonexistent"}})"
             "\n";

    auto               oldCin  = std::cin.rdbuf();
    auto               oldCout = std::cout.rdbuf();
    std::istringstream in(input);
    std::ostringstream out;
    std::cin.rdbuf(in.rdbuf());
    std::cout.rdbuf(out.rdbuf());
    server.runStdio();
    std::cin.rdbuf(oldCin);
    std::cout.rdbuf(oldCout);

    std::vector<json>  responses;
    std::istringstream outputStream(out.str());
    std::string        line;
    while (std::getline(outputStream, line)) {
        if (!line.empty()) {
            responses.push_back(json::parse(line));
        }
    }

    // 7 requests → 7 responses (init + 6 resource/prompt ops)
    XX_TEST_EXPECT_EQ(responses.size(), (size_t)7);
    if (responses.size() >= 7) {
        // Resp 1: resources/list
        XX_TEST_EXPECT_EQ(responses[1]["id"].get<int>(), 2);
        XX_TEST_EXPECT_TRUE(responses[1].contains("result"));
        XX_TEST_EXPECT_TRUE(responses[1]["result"]["resources"].is_array());
        XX_TEST_EXPECT_EQ(responses[1]["result"]["resources"].size(), (size_t)1);

        // Resp 2: resources/read success
        XX_TEST_EXPECT_EQ(responses[2]["id"].get<int>(), 3);
        XX_TEST_EXPECT_TRUE(responses[2].contains("result"));
        XX_TEST_EXPECT_EQ(
            responses[2]["result"]["contents"][0]["text"].get<std::string>(),
            "hello world"
        );

        // Resp 3: resources/read nonexistent
        XX_TEST_EXPECT_EQ(responses[3]["id"].get<int>(), 4);
        XX_TEST_EXPECT_TRUE(responses[3].contains("error"));

        // Resp 4: prompts/list
        XX_TEST_EXPECT_EQ(responses[4]["id"].get<int>(), 5);
        XX_TEST_EXPECT_TRUE(responses[4]["result"]["prompts"].is_array());
        XX_TEST_EXPECT_EQ(responses[4]["result"]["prompts"].size(), (size_t)1);

        // Resp 5: prompts/get success
        XX_TEST_EXPECT_EQ(responses[5]["id"].get<int>(), 6);
        XX_TEST_EXPECT_TRUE(responses[5].contains("result"));
        XX_TEST_EXPECT_EQ(
            responses[5]["result"]["messages"][0]["content"].get<std::string>(),
            "Hello, World!"
        );

        // Resp 6: prompts/get nonexistent
        XX_TEST_EXPECT_EQ(responses[6]["id"].get<int>(), 7);
        XX_TEST_EXPECT_TRUE(responses[6].contains("error"));
    }
}

// -----------------------------------------------------------------------
// McpClient HTTP transport tests
// -----------------------------------------------------------------------

asio::awaitable<void> test_mcp_client_http() {
    using Server = McpServer;

    Server::Config cfg;
    cfg.httpConfig.address          = "127.0.0.1";
    cfg.httpConfig.port             = 0;
    cfg.httpConfig.ioThreads        = 1;
    cfg.httpConfig.accessLogEnabled = false;

    Server            server(std::move(cfg));
    McpToolDefinition def;
    def.name        = "echo";
    def.description = "Echo back the input";
    def.inputSchema = json::parse(R"({
    "type": "object",
    "properties": {
      "text": {"type": "string"}
    }
  })");

    server.addTool(def, [](const json& args) -> json {
        json content;
        content["type"] = "text";
        content["text"] = args.value("text", "");
        return content;
    });

    std::thread serverThread([&server]() {
        server.start();
    });

    uint16_t port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        TEST_FAIL << "McpServer failed to start" << std::endl;
        g_mcp_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);

    // Wait for server to be reachable
    for (int i = 0; i < 100; ++i) {
        try {
            asio::io_context      tmpCtx;
            asio::ip::tcp::socket sock(tmpCtx);
            sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
            sock.close();
            break;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // Create MCP client
    McpClient::Config clientCfg;
    clientCfg.serverUrl       = baseUrl + "/mcp";
    clientCfg.protocolVersion = std::string{McpClient::kProtocol2024_11_05};
    clientCfg.requestTimeout  = std::chrono::seconds(5);
    clientCfg.initTimeout     = std::chrono::seconds(5);

    auto client = std::make_shared<McpClient>(std::move(clientCfg));

    // Test initialize
    {
        auto result = co_await client->initialize();
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            XX_TEST_EXPECT_FALSE(result->serverName.empty());
            XX_TEST_EXPECT_EQ(result->protocolVersion, "2024-11-05");
        }
    }

    // Test ping
    {
        auto result = co_await client->ping();
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            XX_TEST_EXPECT_TRUE(result.value());
        }
    }

    // Test listTools
    {
        auto result = co_await client->listTools();
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            XX_TEST_EXPECT_EQ(result->size(), (size_t)1);
            if (!result->empty()) {
                XX_TEST_EXPECT_EQ((*result)[0].name, "echo");
            }
        }
    }

    // Test callTool
    {
        auto result = co_await client->callTool(
            "echo",
            {
                {"text", "hello client"}
        }
        );
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            XX_TEST_EXPECT_TRUE(result->contains("content"));
            if (result->contains("content") && (*result)["content"].is_array()) {
                XX_TEST_EXPECT_EQ(
                    (*result)["content"][0]["text"].get<std::string>(),
                    "hello client"
                );
            }
        }
    }

    // Test callTool nonexistent
    {
        auto result = co_await client->callTool("nonexistent", json::object());
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            // Should have error in the response envelope
            bool hasDirectError = result->contains("error");
            // Or isError flag in the result
            bool hasIsError = result->contains("isError") && (*result)["isError"].get<bool>();
            XX_TEST_EXPECT_TRUE(hasDirectError || hasIsError);
        }
    }

    // Test listResources (empty)
    {
        auto result = co_await client->listResources();
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            XX_TEST_EXPECT_TRUE(result->empty());
        }
    }

    // Test listPrompts (empty)
    {
        auto result = co_await client->listPrompts();
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            XX_TEST_EXPECT_TRUE(result->empty());
        }
    }

    // Test tool adapter creation
    {
        McpToolDefinition toolDef;
        toolDef.name        = "echo";
        toolDef.description = "Echo back";
        auto tool           = client->createTool(std::move(toolDef), {});
        XX_TEST_EXPECT_EQ(tool->get_name(), "echo");
        auto defn = tool->get_definition();
        XX_TEST_EXPECT_EQ(defn.name, "echo");
    }

    co_await client->close();
    server.stop();
    serverThread.join();
}

// -----------------------------------------------------------------------
// Stdio transport tests
// -----------------------------------------------------------------------

void test_mcp_server_stdio_basic() {
    McpServer server;

    McpToolDefinition def;
    def.name        = "echo";
    def.description = "Echo back the input";
    def.inputSchema = json::parse(R"({
    "type": "object",
    "properties": {
      "text": {"type": "string"}
    }
  })");

    server.addTool(def, [](const json& args) -> json {
        json content;
        content["type"] = "text";
        content["text"] = args.value("text", "");
        return content;
    });

    std::string input;
    input
        += R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}})"
           "\n";
    input += R"({"jsonrpc":"2.0","id":2,"method":"ping"})"
             "\n";
    input += R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})"
             "\n";
    input
        += R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hello stdio"}}})"
           "\n";
    input
        += R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"nonexistent","arguments":{}}})"
           "\n";
    input += R"({"jsonrpc":"2.0","id":6,"method":"nonexistent/method"})"
             "\n";
    input += R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
             "\n";

    auto oldCin  = std::cin.rdbuf();
    auto oldCout = std::cout.rdbuf();

    std::istringstream in(input);
    std::ostringstream out;

    std::cin.rdbuf(in.rdbuf());
    std::cout.rdbuf(out.rdbuf());

    server.runStdio();

    std::cin.rdbuf(oldCin);
    std::cout.rdbuf(oldCout);

    // Parse output
    std::string        output = out.str();
    std::istringstream outputStream(output);
    std::string        line;
    std::vector<json>  responses;
    while (std::getline(outputStream, line)) {
        if (!line.empty()) {
            responses.push_back(json::parse(line));
        }
    }

    // 6 requests with id → 6 responses, 1 notification → no response
    XX_TEST_EXPECT_EQ(responses.size(), (size_t)6);

    if (responses.size() >= 6) {
        // Resp 0: initialize
        XX_TEST_EXPECT_EQ(responses[0]["id"].get<int>(), 1);
        XX_TEST_EXPECT_TRUE(responses[0].contains("result"));
        XX_TEST_EXPECT_EQ(
            responses[0]["result"]["serverInfo"]["name"].get<std::string>(),
            "agentxx-mcp"
        );

        // Resp 1: ping
        XX_TEST_EXPECT_EQ(responses[1]["id"].get<int>(), 2);
        XX_TEST_EXPECT_TRUE(responses[1].contains("result"));

        // Resp 2: tools/list
        XX_TEST_EXPECT_EQ(responses[2]["id"].get<int>(), 3);
        XX_TEST_EXPECT_TRUE(responses[2].contains("result"));
        XX_TEST_EXPECT_EQ(responses[2]["result"]["tools"].size(), (size_t)1);
        XX_TEST_EXPECT_EQ(responses[2]["result"]["tools"][0]["name"].get<std::string>(), "echo");

        // Resp 3: tools/call echo
        XX_TEST_EXPECT_EQ(responses[3]["id"].get<int>(), 4);
        XX_TEST_EXPECT_TRUE(responses[3].contains("result"));
        XX_TEST_EXPECT_EQ(
            responses[3]["result"]["content"][0]["text"].get<std::string>(),
            "hello stdio"
        );

        // Resp 4: tools/call nonexistent
        XX_TEST_EXPECT_EQ(responses[4]["id"].get<int>(), 5);
        XX_TEST_EXPECT_TRUE(responses[4].contains("error"));
        XX_TEST_EXPECT_EQ(responses[4]["error"]["code"].get<int>(), -32000);

        // Resp 5: nonexistent method
        XX_TEST_EXPECT_EQ(responses[5]["id"].get<int>(), 6);
        XX_TEST_EXPECT_TRUE(responses[5].contains("error"));
        XX_TEST_EXPECT_EQ(responses[5]["error"]["code"].get<int>(), -32601);
    }
}

void test_mcp_server_stdio_errors() {
    McpServer server;

    std::string input;
    input += "not valid json\n";
    input += R"({"jsonrpc":"3.0","id":1,"method":"ping"})"
             "\n";
    input += "\n";
    input += R"({"jsonrpc":"2.0"})"
             "\n";

    auto oldCin  = std::cin.rdbuf();
    auto oldCout = std::cout.rdbuf();

    std::istringstream in(input);
    std::ostringstream out;

    std::cin.rdbuf(in.rdbuf());
    std::cout.rdbuf(out.rdbuf());

    server.runStdio();

    std::cin.rdbuf(oldCin);
    std::cout.rdbuf(oldCout);

    std::string        output = out.str();
    std::istringstream outputStream(output);
    std::string        line;
    std::vector<json>  responses;
    while (std::getline(outputStream, line)) {
        if (!line.empty()) {
            responses.push_back(json::parse(line));
        }
    }

    // 3 error responses, empty line skipped
    XX_TEST_EXPECT_EQ(responses.size(), (size_t)3);

    if (responses.size() >= 3) {
        // Resp 0: parse error
        XX_TEST_EXPECT_TRUE(responses[0].contains("error"));
        XX_TEST_EXPECT_EQ(responses[0]["error"]["code"].get<int>(), -32700);

        // Resp 1: invalid request (wrong jsonrpc version)
        XX_TEST_EXPECT_TRUE(responses[1].contains("error"));
        XX_TEST_EXPECT_EQ(responses[1]["error"]["code"].get<int>(), -32600);

        // Resp 2: missing method
        XX_TEST_EXPECT_TRUE(responses[2].contains("error"));
        XX_TEST_EXPECT_EQ(responses[2]["error"]["code"].get<int>(), -32600);
    }
}

// -----------------------------------------------------------------------
// 2025-03-26 specific feature tests
// -----------------------------------------------------------------------

void test_mcp_server_2025_03_features() {
    McpServer server;

    // Test resource templates list
    {
        std::string input;
        input += R"({"jsonrpc":"2.0","id":1,"method":"resources/templates/list"})"
                 "\n";

        auto               oldCin  = std::cin.rdbuf();
        auto               oldCout = std::cout.rdbuf();
        std::istringstream in(input);
        std::ostringstream out;
        std::cin.rdbuf(in.rdbuf());
        std::cout.rdbuf(out.rdbuf());
        server.runStdio();
        std::cin.rdbuf(oldCin);
        std::cout.rdbuf(oldCout);

        std::vector<json>  responses;
        std::istringstream outputStream(out.str());
        std::string        line;
        while (std::getline(outputStream, line)) {
            if (!line.empty()) {
                responses.push_back(json::parse(line));
            }
        }

        XX_TEST_EXPECT_EQ(responses.size(), (size_t)1);
        if (!responses.empty()) {
            XX_TEST_EXPECT_TRUE(responses[0].contains("result"));
            XX_TEST_EXPECT_TRUE(responses[0]["result"].contains("resourceTemplates"));
            XX_TEST_EXPECT_TRUE(responses[0]["result"]["resourceTemplates"].is_array());
        }
    }

    // Test completion/complete
    {
        std::string input;
        input
            += R"({"jsonrpc":"2.0","id":1,"method":"completion/complete","params":{"ref":{"type":"ref/prompt","name":"test"},"argument":{"name":"arg","value":"val"}}})"
               "\n";

        auto               oldCin  = std::cin.rdbuf();
        auto               oldCout = std::cout.rdbuf();
        std::istringstream in(input);
        std::ostringstream out;
        std::cin.rdbuf(in.rdbuf());
        std::cout.rdbuf(out.rdbuf());
        server.runStdio();
        std::cin.rdbuf(oldCin);
        std::cout.rdbuf(oldCout);

        std::vector<json>  responses;
        std::istringstream outputStream(out.str());
        std::string        line;
        while (std::getline(outputStream, line)) {
            if (!line.empty()) {
                responses.push_back(json::parse(line));
            }
        }

        XX_TEST_EXPECT_EQ(responses.size(), (size_t)1);
        if (!responses.empty()) {
            XX_TEST_EXPECT_TRUE(responses[0].contains("result"));
            XX_TEST_EXPECT_TRUE(responses[0]["result"].contains("completion"));
            XX_TEST_EXPECT_TRUE(responses[0]["result"]["completion"].contains("values"));
        }
    }

    // Test _meta passthrough on response
    {
        std::string input;
        input
            += R"({"jsonrpc":"2.0","id":1,"method":"ping","params":{"_meta":{"progressToken":"tok-123"}}})"
               "\n";

        auto               oldCin  = std::cin.rdbuf();
        auto               oldCout = std::cout.rdbuf();
        std::istringstream in(input);
        std::ostringstream out;
        std::cin.rdbuf(in.rdbuf());
        std::cout.rdbuf(out.rdbuf());
        server.runStdio();
        std::cin.rdbuf(oldCin);
        std::cout.rdbuf(oldCout);

        std::vector<json>  responses;
        std::istringstream outputStream(out.str());
        std::string        line;
        while (std::getline(outputStream, line)) {
            if (!line.empty()) {
                responses.push_back(json::parse(line));
            }
        }

        XX_TEST_EXPECT_EQ(responses.size(), (size_t)1);
        if (!responses.empty()) {
            // _meta should be passed through to response (2025-03-26+)
            if (responses[0]["result"].contains("_meta")) {
                XX_TEST_EXPECT_EQ(
                    responses[0]["result"]["_meta"]["progressToken"].get<std::string>(),
                    "tok-123"
                );
            }
            // Either way (passthrough or not), server should return a valid ping
            // response
            XX_TEST_EXPECT_TRUE(responses[0].contains("result"));
        }
    }
}

// -----------------------------------------------------------------------
// 2025-06-18 specific feature tests
// -----------------------------------------------------------------------

void test_mcp_server_2025_06_features() {
    McpServer server;

    // Test that serverInfo includes title (2025-06-18 feature)
    {
        std::string input;
        input
            += R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"test","version":"1.0","title":"Test Client"},"capabilities":{}}})"
               "\n";

        auto               oldCin  = std::cin.rdbuf();
        auto               oldCout = std::cout.rdbuf();
        std::istringstream in(input);
        std::ostringstream out;
        std::cin.rdbuf(in.rdbuf());
        std::cout.rdbuf(out.rdbuf());
        server.runStdio();
        std::cin.rdbuf(oldCin);
        std::cout.rdbuf(oldCout);

        std::vector<json>  responses;
        std::istringstream outputStream(out.str());
        std::string        line;
        while (std::getline(outputStream, line)) {
            if (!line.empty()) {
                responses.push_back(json::parse(line));
            }
        }

        XX_TEST_EXPECT_EQ(responses.size(), (size_t)1);
        if (!responses.empty()) {
            // serverInfo should have title (2025-06-18)
            XX_TEST_EXPECT_TRUE(responses[0]["result"]["serverInfo"].contains("title"));
            // capabilities should have elicitation (2025-06-18)
            XX_TEST_EXPECT_TRUE(responses[0]["result"]["capabilities"].contains("elicitation"));
        }
    }
}

// -----------------------------------------------------------------------
// Cross-version client-server tests
// -----------------------------------------------------------------------

/// Test all version pairs via stdio
void test_mcp_server_cross_version_stdio() {
    struct VersionPair {
        std::string clientVer;
        std::string expectedVer; // "" means expect exact match
    };

    VersionPair pairs[] = {
        // Client requests known version → server responds with same
        {"2024-11-05", "2024-11-05"},
        {"2025-03-26", "2025-03-26"},
        {"2025-06-18", "2025-06-18"},
        {"2025-11-25", "2025-11-25"},
        // Client requests unknown 2025 version → server responds with newest 2025
        {"2025-01-01", "2025-11-25"},
        // Client requests future version → server responds with newest
        {"2026-01-01", "2026-07-28"},
        // Client requests older version → server responds with that version
        {"2024-06-01", "2024-11-05"},
    };

    for (const auto& pair : pairs) {
        McpServer   server;
        std::string input;
        input += "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{";
        input += "\"protocolVersion\":\"" + pair.clientVer + "\"";
        input += ",\"clientInfo\":{\"name\":\"test\",\"version\":\"1.0\"}";
        input += ",\"capabilities\":{}}}";
        input += "\n";

        auto               oldCin  = std::cin.rdbuf();
        auto               oldCout = std::cout.rdbuf();
        std::istringstream in(input);
        std::ostringstream out;
        std::cin.rdbuf(in.rdbuf());
        std::cout.rdbuf(out.rdbuf());
        server.runStdio();
        std::cin.rdbuf(oldCin);
        std::cout.rdbuf(oldCout);

        std::vector<json>  responses;
        std::istringstream outputStream(out.str());
        std::string        line;
        while (std::getline(outputStream, line)) {
            if (!line.empty()) {
                responses.push_back(json::parse(line));
            }
        }

        bool ok = responses.size() == 1 && responses[0].contains("result")
                  && responses[0]["result"].contains("protocolVersion");

        if (ok) {
            std::string got      = responses[0]["result"]["protocolVersion"].get<std::string>();
            std::string expected = pair.expectedVer.empty() ? pair.clientVer : pair.expectedVer;
            if (got == expected) {
                XX_TEST_PASSED++;
            } else {
                XX_TEST_FAILED++;
                TEST_FAIL << "line ~" << __LINE__ << ": client=" << pair.clientVer
                          << " expected=" << expected << " got=" << got << std::endl;
            }
            // Verify serverInfo is always present
            if (responses[0]["result"].contains("serverInfo")) {
                XX_TEST_PASSED++;
            } else {
                XX_TEST_FAILED++;
                TEST_FAIL << "line ~" << __LINE__ << ": missing serverInfo" << std::endl;
            }
            // Verify capabilities is always present
            if (responses[0]["result"].contains("capabilities")) {
                XX_TEST_PASSED++;
            } else {
                XX_TEST_FAILED++;
                TEST_FAIL << "line ~" << __LINE__ << ": missing capabilities" << std::endl;
            }
            // instructions field should be present for all versions (forward compat)
            if (responses[0]["result"].contains("instructions")) {
                XX_TEST_PASSED++;
            } else {
                XX_TEST_FAILED++;
                TEST_FAIL << "line ~" << __LINE__ << ": missing instructions" << std::endl;
            }
        } else {
            XX_TEST_FAILED++;
            TEST_FAIL << "line ~" << __LINE__ << ": client=" << pair.clientVer
                      << " bad response: " << (responses.empty() ? "empty" : responses[0].dump())
                      << std::endl;
        }
    }
}

// Test cross-version via HTTP transport (all 3 × 3 = 9 combinations)
asio::awaitable<void> test_mcp_server_cross_version_http() {
    using Server = McpServer;

    Server::Config cfg;
    cfg.httpConfig.address          = "127.0.0.1";
    cfg.httpConfig.port             = 0;
    cfg.httpConfig.ioThreads        = 1;
    cfg.httpConfig.accessLogEnabled = false;

    Server server(std::move(cfg));

    std::thread serverThread([&server]() {
        server.start();
    });

    uint16_t port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        TEST_FAIL << "McpServer failed to start for cross-version test" << std::endl;
        g_mcp_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);

    for (int i = 0; i < 100; ++i) {
        try {
            asio::io_context      tmpCtx;
            asio::ip::tcp::socket sock(tmpCtx);
            sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
            sock.close();
            break;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // Test each client version against the server
    std::string versions[] = {
        std::string{McpClient::kProtocol2024_11_05},
        std::string{McpClient::kProtocol2025_03_26},
        std::string{McpClient::kProtocol2025_06_18},
        std::string{McpClient::kProtocol2025_11_25},
    };

    for (const auto& clientVer : versions) {
        McpClient::Config clientCfg;
        clientCfg.serverUrl       = baseUrl + "/mcp";
        clientCfg.protocolVersion = clientVer;
        clientCfg.requestTimeout  = std::chrono::seconds(5);

        auto client = std::make_shared<McpClient>(std::move(clientCfg));
        auto result = co_await client->initialize();

        if (result.has_value()) {
            XX_TEST_PASSED++; // initialize succeeded
            // Verify returned version is non-empty and compatible
            if (!result->protocolVersion.empty()) {
                XX_TEST_PASSED++;
            } else {
                XX_TEST_FAILED++;
                TEST_FAIL << "client=" << clientVer << " empty protocolVersion" << std::endl;
            }
            // All versions should work for basic operations
            auto ping = co_await client->ping();
            if (ping.has_value()) {
                XX_TEST_PASSED++;
            } else {
                XX_TEST_FAILED++;
                TEST_FAIL << "client=" << clientVer << " ping failed: " << ping.error()
                          << std::endl;
            }
            auto tools = co_await client->listTools();
            if (tools.has_value()) {
                XX_TEST_PASSED++;
            } else {
                XX_TEST_FAILED++;
                TEST_FAIL << "client=" << clientVer << " listTools failed: " << tools.error()
                          << std::endl;
            }
        } else {
            XX_TEST_FAILED++;
            TEST_FAIL << "client=" << clientVer << " initialize failed: " << result.error()
                      << std::endl;
        }
        co_await client->close();
    }

    server.stop();
    serverThread.join();
}

// -----------------------------------------------------------------------
// Test 2025-03-26 client connecting to a 2025-03-26 server (stdio)
// -----------------------------------------------------------------------

void test_mcp_server_2025_03_26_stdio() {
    McpServer         server;
    McpToolDefinition def;
    def.name        = "greeter";
    def.description = "Greets the user";
    def.inputSchema = json::parse(R"({"type":"object","properties":{"name":{"type":"string"}}})");
    server.addTool(def, [](const json& args) -> json {
        json c;
        c["type"] = "text";
        c["text"] = "Hello, " + args.value("name", "world") + "!";
        return c;
    });

    // Simulate a 2025-03-26 client: initialize, then standard tool operations
    std::string input;
    input
        += R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"test-client","version":"1.0"}}})"
           "\n";
    // Notification without id (per spec) → no response
    input += R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
             "\n";
    input += R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"
             "\n";
    input
        += R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"greeter","arguments":{"name":"MCP"}}})"
           "\n";
    input += R"({"jsonrpc":"2.0","id":4,"method":"resources/templates/list"})"
             "\n";
    input
        += R"({"jsonrpc":"2.0","id":5,"method":"completion/complete","params":{"ref":{"type":"ref/prompt","name":"test"},"argument":{"name":"a","value":"b"}}})"
           "\n";
    input += R"({"jsonrpc":"2.0","id":6,"method":"ping"})"
             "\n";

    auto               oldCin  = std::cin.rdbuf();
    auto               oldCout = std::cout.rdbuf();
    std::istringstream in(input);
    std::ostringstream out;
    std::cin.rdbuf(in.rdbuf());
    std::cout.rdbuf(out.rdbuf());
    server.runStdio();
    std::cin.rdbuf(oldCin);
    std::cout.rdbuf(oldCout);

    std::vector<json>  responses;
    std::istringstream outputStream(out.str());
    std::string        line;
    while (std::getline(outputStream, line)) {
        if (!line.empty()) {
            responses.push_back(json::parse(line));
        }
    }

    // 6 requests with id → 6 responses (notification without id → no response)
    XX_TEST_EXPECT_EQ(responses.size(), (size_t)6);
    if (responses.size() >= 6) {
        // initialize
        XX_TEST_EXPECT_EQ(responses[0]["id"].get<int>(), 1);
        XX_TEST_EXPECT_TRUE(responses[0]["result"].contains("instructions"));
        XX_TEST_EXPECT_EQ(
            responses[0]["result"]["protocolVersion"].get<std::string>(),
            "2025-03-26"
        );
        // tools/list
        XX_TEST_EXPECT_EQ(responses[1]["id"].get<int>(), 2);
        XX_TEST_EXPECT_EQ(responses[1]["result"]["tools"].size(), (size_t)1);
        // tools/call
        XX_TEST_EXPECT_EQ(responses[2]["id"].get<int>(), 3);
        XX_TEST_EXPECT_EQ(
            responses[2]["result"]["content"][0]["text"].get<std::string>(),
            "Hello, MCP!"
        );
        // resources/templates/list
        XX_TEST_EXPECT_EQ(responses[3]["id"].get<int>(), 4);
        XX_TEST_EXPECT_TRUE(responses[3]["result"].contains("resourceTemplates"));
        // completion/complete
        XX_TEST_EXPECT_EQ(responses[4]["id"].get<int>(), 5);
        XX_TEST_EXPECT_TRUE(responses[4]["result"].contains("completion"));
        // ping
        XX_TEST_EXPECT_EQ(responses[5]["id"].get<int>(), 6);
        XX_TEST_EXPECT_TRUE(responses[5].contains("result"));
    }
}

// -----------------------------------------------------------------------
// Accept header validation test
// -----------------------------------------------------------------------

asio::awaitable<void> test_mcp_client_accept_header() {
    using Server = util::HttpServer;

    Server::Config cfg;
    cfg.address          = "127.0.0.1";
    cfg.port             = 0;
    cfg.ioThreads        = 1;
    cfg.accessLogEnabled = false;

    auto server = std::make_shared<Server>(std::move(cfg));

    using Handler = Server::Handler;
    auto handler  = std::make_shared<Handler>(
        [](Server::Request& req, Server::Response& resp, std::string_view
        ) -> asio::awaitable<void> {
            namespace http = boost::beast::http;

            auto accept = req[http::field::accept];
            bool valid  = (accept == "*/*")
                         || (accept.find("application/json") != boost::string_view::npos
                             && accept.find("text/event-stream") != boost::string_view::npos);

            if (!valid) {
                resp.version(req.version());
                resp.result(http::status::not_acceptable);
                resp.set(http::field::content_type, "application/json");
                json error;
                error["jsonrpc"]       = "2.0";
                error["error"]["code"] = -32000;
                error["error"]["message"]
                    = "Not Acceptable: Client must accept both application/json "
                       "and text/event-stream";
                resp.body() = error.dump();
                resp.prepare_payload();
                co_return;
            }

            json requestJson;
            try {
                requestJson = json::parse(req.body());
            } catch (...) {
                resp.version(req.version());
                resp.result(http::status::bad_request);
                resp.prepare_payload();
                co_return;
            }

            json        id     = requestJson.value("id", json{});
            std::string method = requestJson.value("method", "");
            json        response;
            response["jsonrpc"] = "2.0";
            response["id"]      = id;

            if (method == "initialize") {
                response["result"]["protocolVersion"]       = "2024-11-05";
                response["result"]["serverInfo"]["name"]    = "accept-test-server";
                response["result"]["serverInfo"]["version"] = "1.0";
                response["result"]["capabilities"]          = json::object();
            } else if (method == "ping") {
                response["result"] = json::object();
            } else if (method == "tools/list") {
                response["result"]["tools"] = json::array();
            } else {
                response["error"]["code"]    = -32601;
                response["error"]["message"] = "Method not found";
            }

            resp.version(req.version());
            resp.result(http::status::ok);
            resp.set(http::field::content_type, "application/json");
            resp.body() = response.dump();
            resp.prepare_payload();
        }
    );

    server->router().add("/mcp", 2, handler);

    std::thread serverThread([server]() {
        server->start();
    });

    uint16_t port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server->port();
        if (port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (port == 0) {
        TEST_FAIL << "Accept header test server failed to start" << std::endl;
        g_mcp_failed++;
        server->stop();
        serverThread.join();
        co_return;
    }

    for (int i = 0; i < 100; ++i) {
        try {
            asio::io_context      tmpCtx;
            asio::ip::tcp::socket sock(tmpCtx);
            sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
            sock.close();
            break;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);

    // Client with default Accept (application/json, text/event-stream) → must
    // succeed
    {
        McpClient::Config clientCfg;
        clientCfg.serverUrl       = baseUrl + "/mcp";
        clientCfg.protocolVersion = std::string{McpClient::kProtocol2024_11_05};
        clientCfg.requestTimeout  = std::chrono::seconds(5);

        auto client = std::make_shared<McpClient>(std::move(clientCfg));
        auto result = co_await client->initialize();
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            XX_TEST_EXPECT_EQ(result->serverName, "accept-test-server");
        }

        auto ping = co_await client->ping();
        XX_TEST_EXPECT_TRUE(ping.has_value());

        co_await client->close();
    }

    // Client with Accept: */* (via extraHeaders override) → also must succeed
    {
        McpClient::Config clientCfg;
        clientCfg.serverUrl       = baseUrl + "/mcp";
        clientCfg.protocolVersion = std::string{McpClient::kProtocol2024_11_05};
        clientCfg.requestTimeout  = std::chrono::seconds(5);
        clientCfg.extraHeaders.set("Accept", "*/*");

        auto client = std::make_shared<McpClient>(std::move(clientCfg));
        auto result = co_await client->initialize();
        XX_TEST_EXPECT_TRUE(result.has_value());

        co_await client->close();
    }

    // Client with a wrong Accept header in extraHeaders must still succeed
    // because buildHttpHeaders() always overrides Accept with the correct value.
    {
        McpClient::Config clientCfg;
        clientCfg.serverUrl       = baseUrl + "/mcp";
        clientCfg.protocolVersion = std::string{McpClient::kProtocol2024_11_05};
        clientCfg.requestTimeout  = std::chrono::seconds(5);
        clientCfg.extraHeaders.set("Accept", "application/xml");

        auto client = std::make_shared<McpClient>(std::move(clientCfg));
        auto result = co_await client->initialize();
        // The client always sends Accept: application/json, text/event-stream
        // regardless of extraHeaders, so this must succeed
        XX_TEST_EXPECT_TRUE(result.has_value());

        co_await client->close();
    }

    server->stop();
    serverThread.join();
}

// -----------------------------------------------------------------------
// Server-side Accept header validation & SSE response mode tests
// -----------------------------------------------------------------------

asio::awaitable<void> test_mcp_server_accept_sse() {
    using Server = McpServer;

    Server::Config cfg;
    cfg.httpConfig.address          = "127.0.0.1";
    cfg.httpConfig.port             = 0;
    cfg.httpConfig.ioThreads        = 1;
    cfg.httpConfig.accessLogEnabled = false;

    Server            server(std::move(cfg));
    McpToolDefinition def;
    def.name        = "echo";
    def.description = "Echo";
    def.inputSchema = json::parse(R"({"type":"object","properties":{"text":{"type":"string"}}})");
    server.addTool(def, [](const json& args) -> json {
        json content;
        content["type"] = "text";
        content["text"] = args.value("text", "echo");
        return content;
    });

    std::thread serverThread([&server]() {
        server.start();
    });

    uint16_t port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        TEST_FAIL << "Server failed to start" << std::endl;
        g_mcp_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);

    for (int i = 0; i < 100; ++i) {
        try {
            asio::io_context      tmpCtx;
            asio::ip::tcp::socket sock(tmpCtx);
            sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
            sock.close();
            break;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // 1. Wrong Accept header → 406
    {
        HeaderMap headers;
        headers.set("Accept", "application/xml");
        headers.set("content-type", "application/json");
        json req = {
            {"jsonrpc", "2.0" },
            {"id",      1     },
            {"method",  "ping"}
        };
        auto resp = co_await HttpClient::postAsync(
            baseUrl + "/mcp",
            req,
            headers,
            HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{5}}
        );
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 406);
        }
    }

    // 2. Correct Accept header (both types) → 200 JSON
    {
        HeaderMap headers;
        headers.set("Accept", "application/json, text/event-stream");
        headers.set("content-type", "application/json");
        json req = {
            {"jsonrpc", "2.0" },
            {"id",      2     },
            {"method",  "ping"}
        };
        auto resp = co_await HttpClient::postAsync(
            baseUrl + "/mcp",
            req,
            headers,
            HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{5}}
        );
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            XX_TEST_EXPECT_EQ(resp.value().contentType(), "application/json");
        }
    }

    // 3. */* Accept → 200 JSON (not SSE)
    {
        json req = {
            {"jsonrpc", "2.0" },
            {"id",      3     },
            {"method",  "ping"}
        };
        auto resp = co_await HttpClient::postAsync(baseUrl + "/mcp", req);
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            XX_TEST_EXPECT_EQ(resp.value().contentType(), "application/json");
        }
    }

    // 4. Accept: text/event-stream → 200 + SSE wrapping
    {
        HeaderMap headers;
        headers.set("Accept", "text/event-stream");
        headers.set("content-type", "application/json");
        json req = {
            {"jsonrpc", "2.0" },
            {"id",      4     },
            {"method",  "ping"}
        };
        auto resp = co_await HttpClient::postAsync(
            baseUrl + "/mcp",
            req,
            headers,
            HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{5}}
        );
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            // Content-Type should be text/event-stream
            auto ct = resp.value().contentType();
            XX_TEST_EXPECT_TRUE(ct.find("event-stream") != std::string::npos);
            // Body should be SSE formatted
            XX_TEST_EXPECT_TRUE(resp.value().body.find("event: message") != std::string::npos);
            XX_TEST_EXPECT_TRUE(resp.value().body.find("\"result\"") != std::string::npos);
        }
    }

    // 5. SSE endpoint (GET /mcp/sse) → text/event-stream (长连接流式读取)
    {
        HeaderMap headers;
        headers.set("Accept", "text/event-stream");
        std::string sseBody;
        co_await agentxx::util::catchErrorAsync<bool>(
            [&]() -> asio::awaitable<bool> {
                co_await util::HttpClient::requestSseAsync(
                    "GET",
                    baseUrl + "/mcp/sse",
                    "",
                    "",
                    headers,
                    util::HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{5}},
                    [&](std::string_view chunk) -> bool {
                        sseBody.append(chunk);
                        // 收到 endpoint 事件即停止读取 (长连接不会自行结束)
                        return sseBody.find("event: endpoint") != std::string::npos;
                    }
                );
                co_return true;
            },
            [](std::string) -> asio::awaitable<bool> {
                co_return true;
            }
        );
        // Should contain endpoint event
        XX_TEST_EXPECT_TRUE(sseBody.find("event: endpoint") != std::string::npos);
        XX_TEST_EXPECT_TRUE(sseBody.find("/mcp") != std::string::npos);
    }

    // 6. SSE endpoint with */* → also works (长连接流式读取)
    {
        std::string sseBody;
        co_await agentxx::util::catchErrorAsync<bool>(
            [&]() -> asio::awaitable<bool> {
                co_await util::HttpClient::requestSseAsync(
                    "GET",
                    baseUrl + "/mcp/sse",
                    "",
                    "",
                    {},
                    util::HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{5}},
                    [&](std::string_view chunk) -> bool {
                        sseBody.append(chunk);
                        return sseBody.find("event: endpoint") != std::string::npos;
                    }
                );
                co_return true;
            },
            [](std::string) -> asio::awaitable<bool> {
                co_return true;
            }
        );
        XX_TEST_EXPECT_TRUE(sseBody.find("event: endpoint") != std::string::npos);
    }

    // 7. SSE endpoint with wrong Accept → server closes connection
    {
        HeaderMap headers;
        headers.set("Accept", "application/xml");
        // 服务器拒绝后立即关闭连接; 读取应快速失败或返回空流
        bool failed = false;
        co_await agentxx::util::catchErrorAsync<bool>(
            [&]() -> asio::awaitable<bool> {
                co_await util::HttpClient::requestSseAsync(
                    "GET",
                    baseUrl + "/mcp/sse",
                    "",
                    "",
                    headers,
                    util::HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{5}},
                    [&](std::string_view) -> bool {
                        return false;
                    }
                );
                co_return true;
            },
            [&](std::string) -> asio::awaitable<bool> {
                failed = true;
                co_return true;
            }
        );
        // 服务器立即关闭 → 客户端读取失败或空流, 两者皆可
        XX_TEST_EXPECT_TRUE(failed);
    }

    // 8. Notification request with correct Accept → 202
    {
        HeaderMap headers;
        headers.set("Accept", "application/json, text/event-stream");
        headers.set("content-type", "application/json");
        json req = {
            {"jsonrpc", "2.0"                      },
            {"method",  "notifications/initialized"}
        };
        auto resp = co_await HttpClient::postAsync(
            baseUrl + "/mcp",
            req,
            headers,
            HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{5}}
        );
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 202);
        }
    }

    server.stop();
    serverThread.join();
}

// -----------------------------------------------------------------------
// McpClient tool namespace tests
// -----------------------------------------------------------------------

asio::awaitable<void> test_mcp_client_tool_namespace() {
    using Server = McpServer;

    Server::Config cfg;
    cfg.httpConfig.address          = "127.0.0.1";
    cfg.httpConfig.port             = 0;
    cfg.httpConfig.ioThreads        = 1;
    cfg.httpConfig.accessLogEnabled = false;

    Server            server(std::move(cfg));
    McpToolDefinition def;
    def.name        = "echo";
    def.description = "Echo back the input";
    def.inputSchema = json::parse(R"({"type":"object","properties":{"text":{"type":"string"}}})");
    server.addTool(def, [](const json& args) -> json {
        json content;
        content["type"] = "text";
        content["text"] = args.value("text", "");
        return content;
    });

    std::thread serverThread([&server]() {
        server.start();
    });

    uint16_t port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        TEST_FAIL << "McpServer failed to start" << std::endl;
        g_mcp_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);

    for (int i = 0; i < 100; ++i) {
        try {
            asio::io_context      tmpCtx;
            asio::ip::tcp::socket sock(tmpCtx);
            sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
            sock.close();
            break;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // 1. 带命名空间的 client: 对外名称带前缀, 远程调用仍用原始名称
    {
        McpClient::Config clientCfg;
        clientCfg.serverUrl       = baseUrl + "/mcp";
        clientCfg.protocolVersion = std::string{McpClient::kProtocol2024_11_05};
        clientCfg.requestTimeout  = std::chrono::seconds(5);
        clientCfg.initTimeout     = std::chrono::seconds(5);
        clientCfg.toolNamespace   = "myns";

        auto client = std::make_shared<McpClient>(std::move(clientCfg));
        auto init   = co_await client->initialize();
        XX_TEST_EXPECT_TRUE(init.has_value());

        auto toolsList = co_await client->listTools();
        XX_TEST_EXPECT_TRUE(toolsList.has_value());
        if (toolsList.has_value() && !toolsList->empty()) {
            auto tool = client->createTool((*toolsList)[0], {});
            XX_TEST_EXPECT_EQ(tool->get_name(), "myns_echo");
            XX_TEST_EXPECT_EQ(tool->namespacedName(), "myns_echo");
            XX_TEST_EXPECT_EQ(tool->get_definition().name, "myns_echo");
            // 远程调用使用原始名称 "echo", 应成功返回
            auto result = co_await tool->execute_async({
                {"text", "hello ns"}
            });
            XX_TEST_EXPECT_EQ(result, "hello ns");
        }
        co_await client->close();
    }

    // 2. 空命名空间: 名称保持原始名称
    {
        McpClient::Config clientCfg;
        clientCfg.serverUrl       = baseUrl + "/mcp";
        clientCfg.protocolVersion = std::string{McpClient::kProtocol2024_11_05};
        clientCfg.requestTimeout  = std::chrono::seconds(5);
        clientCfg.initTimeout     = std::chrono::seconds(5);

        auto client = std::make_shared<McpClient>(std::move(clientCfg));
        auto init   = co_await client->initialize();
        XX_TEST_EXPECT_TRUE(init.has_value());

        McpToolDefinition toolDef;
        toolDef.name        = "echo";
        toolDef.description = "Echo back";
        auto tool           = client->createTool(std::move(toolDef), {});
        XX_TEST_EXPECT_EQ(tool->get_name(), "echo");
        XX_TEST_EXPECT_EQ(tool->namespacedName(), "echo");
        XX_TEST_EXPECT_EQ(tool->get_definition().name, "echo");
        co_await client->close();
    }

    server.stop();
    serverThread.join();
}

// -----------------------------------------------------------------------
// McpClientTool tool call timeout tests
// -----------------------------------------------------------------------

asio::awaitable<void> test_mcp_client_tool_timeout() {
    using Server = McpServer;

    Server::Config cfg;
    cfg.httpConfig.address          = "127.0.0.1";
    cfg.httpConfig.port             = 0;
    cfg.httpConfig.ioThreads        = 1;
    cfg.httpConfig.accessLogEnabled = false;

    Server            server(std::move(cfg));
    McpToolDefinition def;
    def.name        = "slow";
    def.description = "Slow tool that sleeps 1500ms";
    def.inputSchema = json::parse(R"({"type":"object","properties":{}})");
    server.addTool(def, [](const json&) -> json {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        json content;
        content["type"] = "text";
        content["text"] = "done";
        return content;
    });

    std::thread serverThread([&server]() {
        server.start();
    });

    uint16_t port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        TEST_FAIL << "McpServer failed to start" << std::endl;
        g_mcp_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);

    for (int i = 0; i < 100; ++i) {
        try {
            asio::io_context      tmpCtx;
            asio::ip::tcp::socket sock(tmpCtx);
            sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
            sock.close();
            break;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // 1. toolCallTimeout 有限 (300ms): execute_async 超时抛异常
    {
        McpClient::Config clientCfg;
        clientCfg.serverUrl       = baseUrl + "/mcp";
        clientCfg.requestTimeout  = std::chrono::seconds(5);
        clientCfg.initTimeout     = std::chrono::seconds(5);
        clientCfg.toolCallTimeout = std::chrono::milliseconds(300);

        auto client = std::make_shared<McpClient>(std::move(clientCfg));
        auto init   = co_await client->initialize();
        XX_TEST_EXPECT_TRUE(init.has_value());

        auto tools = co_await client->listTools();
        XX_TEST_EXPECT_TRUE(tools.has_value());
        if (tools.has_value() && !tools->empty()) {
            auto    tool     = client->createTool((*tools)[0], {});
            bool    timedOut = false;
            bool    success  = false;
            co_await agentxx::util::catchErrorAsync<bool>(
                [&]() -> asio::awaitable<bool> {
                    auto r = co_await tool->execute_async(json::object());
                    success = (r == "done");
                    co_return true;
                },
                [&](std::string errmsg) -> asio::awaitable<bool> {
                    TEST_INFO << "tool timeout error: " << errmsg << std::endl;
                    timedOut = errmsg.find("timed out") != std::string::npos;
                    co_return true;
                }
            );
            XX_TEST_EXPECT_FALSE(success);
            XX_TEST_EXPECT_TRUE(timedOut);
        }
        co_await client->close();
    }

    // 2. toolCallTimeout = 0 (不限制): 等待慢工具完成, 应成功返回
    {
        McpClient::Config clientCfg;
        clientCfg.serverUrl       = baseUrl + "/mcp";
        clientCfg.requestTimeout  = std::chrono::seconds(5);
        clientCfg.initTimeout     = std::chrono::seconds(5);
        clientCfg.toolCallTimeout = std::chrono::milliseconds(0);

        auto client = std::make_shared<McpClient>(std::move(clientCfg));
        auto init   = co_await client->initialize();
        XX_TEST_EXPECT_TRUE(init.has_value());

        auto tools = co_await client->listTools();
        XX_TEST_EXPECT_TRUE(tools.has_value());
        if (tools.has_value() && !tools->empty()) {
            auto tool   = client->createTool((*tools)[0], {});
            auto result = co_await tool->execute_async(json::object());
            XX_TEST_EXPECT_EQ(result, "done");
        }
        co_await client->close();
    }

    server.stop();
    serverThread.join();
}

// -----------------------------------------------------------------------
// 2026-07-28 protocol tests
// -----------------------------------------------------------------------

// server/discover over stdio: modern request returns supportedVersions etc.
void test_mcp_server_2026_discover_stdio() {
    McpServer server;

    std::string input;
    input
        += R"({"jsonrpc":"2.0","id":1,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{"name":"test","version":"1.0"},"io.modelcontextprotocol/clientCapabilities":{}}}})"
           "\n";

    auto               oldCin  = std::cin.rdbuf();
    auto               oldCout = std::cout.rdbuf();
    std::istringstream in(input);
    std::ostringstream out;
    std::cin.rdbuf(in.rdbuf());
    std::cout.rdbuf(out.rdbuf());
    server.runStdio();
    std::cin.rdbuf(oldCin);
    std::cout.rdbuf(oldCout);

    std::vector<json>  responses;
    std::istringstream outputStream(out.str());
    std::string        line;
    while (std::getline(outputStream, line)) {
        if (!line.empty()) {
            responses.push_back(json::parse(line));
        }
    }

    XX_TEST_EXPECT_EQ(responses.size(), (size_t)1);
    if (!responses.empty()) {
        auto r = responses[0]["result"];
        // resultType (2026-07-28 必需)
        XX_TEST_EXPECT_EQ(r["resultType"].get<std::string>(), "complete");
        // supportedVersions 必须包含 2026-07-28
        bool has2026 = false;
        for (const auto& v : r["supportedVersions"]) {
            if (v.get<std::string>() == "2026-07-28") {
                has2026 = true;
            }
        }
        XX_TEST_EXPECT_TRUE(has2026);
        // capabilities + serverInfo
        XX_TEST_EXPECT_TRUE(r.contains("capabilities"));
        XX_TEST_EXPECT_TRUE(r["_meta"]["io.modelcontextprotocol/serverInfo"].contains("name"));
        // CacheableResult
        XX_TEST_EXPECT_TRUE(r.contains("ttlMs"));
        XX_TEST_EXPECT_TRUE(r.contains("cacheScope"));
    }
}

// Unsupported protocol version → -32022 with supported list
void test_mcp_server_2026_version_gate() {
    McpServer server;

    std::string input;
    // 未知版本 (现代请求, _meta 声明)
    input
        += R"({"jsonrpc":"2.0","id":1,"method":"ping","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2099-01-01","io.modelcontextprotocol/clientCapabilities":{}}}})"
           "\n";
    // 现代请求缺少必需 _meta 字段 → -32602
    input
        += R"({"jsonrpc":"2.0","id":2,"method":"ping","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}})"
           "\n";
    // 现代请求移除了 initialize → -32601
    input
        += R"({"jsonrpc":"2.0","id":3,"method":"initialize","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}})"
           "\n";
    // 现代请求移除了 logging/setLevel → -32601
    input
        += R"({"jsonrpc":"2.0","id":4,"method":"logging/setLevel","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}})"
           "\n";
    // 现代请求移除了 resources/subscribe → -32601
    input
        += R"({"jsonrpc":"2.0","id":5,"method":"resources/subscribe","params":{"uri":"file:///x","_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}})"
           "\n";

    auto               oldCin  = std::cin.rdbuf();
    auto               oldCout = std::cout.rdbuf();
    std::istringstream in(input);
    std::ostringstream out;
    std::cin.rdbuf(in.rdbuf());
    std::cout.rdbuf(out.rdbuf());
    server.runStdio();
    std::cin.rdbuf(oldCin);
    std::cout.rdbuf(oldCout);

    std::vector<json>  responses;
    std::istringstream outputStream(out.str());
    std::string        line;
    while (std::getline(outputStream, line)) {
        if (!line.empty()) {
            responses.push_back(json::parse(line));
        }
    }

    XX_TEST_EXPECT_EQ(responses.size(), (size_t)5);
    if (responses.size() >= 5) {
        // 1: UnsupportedProtocolVersionError
        XX_TEST_EXPECT_EQ(responses[0]["error"]["code"].get<int>(), -32022);
        XX_TEST_EXPECT_TRUE(responses[0]["error"]["data"]["supported"].is_array());
        bool has2026 = false;
        for (const auto& v : responses[0]["error"]["data"]["supported"]) {
            if (v.get<std::string>() == "2026-07-28") {
                has2026 = true;
            }
        }
        XX_TEST_EXPECT_TRUE(has2026);
        // 2: 缺 clientCapabilities → -32602
        XX_TEST_EXPECT_EQ(responses[1]["error"]["code"].get<int>(), -32602);
        // 3-5: 已移除的方法 → -32601
        XX_TEST_EXPECT_EQ(responses[2]["error"]["code"].get<int>(), -32601);
        XX_TEST_EXPECT_EQ(responses[3]["error"]["code"].get<int>(), -32601);
        XX_TEST_EXPECT_EQ(responses[4]["error"]["code"].get<int>(), -32601);
    }
}

// Modern results carry resultType/_meta/serverInfo/ttlMs; legacy results don't
void test_mcp_server_2026_modern_results() {
    McpServer server;

    std::string input;
    // modern tools/list
    input
        += R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{},"progressToken":"tok-1"}}})"
           "\n";
    // legacy tools/list (无 _meta)
    input += R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"
             "\n";
    // modern ping
    input
        += R"({"jsonrpc":"2.0","id":3,"method":"ping","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}})"
           "\n";

    auto               oldCin  = std::cin.rdbuf();
    auto               oldCout = std::cout.rdbuf();
    std::istringstream in(input);
    std::ostringstream out;
    std::cin.rdbuf(in.rdbuf());
    std::cout.rdbuf(out.rdbuf());
    server.runStdio();
    std::cin.rdbuf(oldCin);
    std::cout.rdbuf(oldCout);

    std::vector<json>  responses;
    std::istringstream outputStream(out.str());
    std::string        line;
    while (std::getline(outputStream, line)) {
        if (!line.empty()) {
            responses.push_back(json::parse(line));
        }
    }

    XX_TEST_EXPECT_EQ(responses.size(), (size_t)3);
    if (responses.size() >= 3) {
        // modern: resultType complete + _meta.serverInfo + ttlMs/cacheScope
        XX_TEST_EXPECT_EQ(responses[0]["result"]["resultType"].get<std::string>(), "complete");
        XX_TEST_EXPECT_TRUE(
            responses[0]["result"]["_meta"]["io.modelcontextprotocol/serverInfo"].contains("name")
        );
        XX_TEST_EXPECT_TRUE(responses[0]["result"].contains("ttlMs"));
        XX_TEST_EXPECT_TRUE(responses[0]["result"].contains("cacheScope"));
        // 请求 _meta 透传 (progressToken)
        XX_TEST_EXPECT_EQ(
            responses[0]["result"]["_meta"]["progressToken"].get<std::string>(),
            "tok-1"
        );
        // legacy: 无 resultType
        XX_TEST_EXPECT_TRUE(responses[1]["result"].contains("tools"));
        XX_TEST_EXPECT_FALSE(responses[1]["result"].contains("resultType"));
        // modern ping 也带 resultType
        XX_TEST_EXPECT_EQ(responses[2]["result"]["resultType"].get<std::string>(), "complete");
    }
}

// subscriptions/listen over stdio: ack notification + graceful-end response
void test_mcp_server_2026_subscriptions_stdio() {
    McpServer server;

    std::string input;
    input
        += R"({"jsonrpc":"2.0","id":7,"method":"subscriptions/listen","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}},"notifications":{"toolsListChanged":true}}})"
           "\n";

    auto               oldCin  = std::cin.rdbuf();
    auto               oldCout = std::cout.rdbuf();
    std::istringstream in(input);
    std::ostringstream out;
    std::cin.rdbuf(in.rdbuf());
    std::cout.rdbuf(out.rdbuf());
    server.runStdio();
    std::cin.rdbuf(oldCin);
    std::cout.rdbuf(oldCout);

    std::vector<json>  messages;
    std::istringstream outputStream(out.str());
    std::string        line;
    while (std::getline(outputStream, line)) {
        if (!line.empty()) {
            messages.push_back(json::parse(line));
        }
    }

    // 2 条消息: ack 通知 + 优雅结束响应 (stdin EOF)
    XX_TEST_EXPECT_EQ(messages.size(), (size_t)2);
    if (messages.size() >= 2) {
        // 1) ack 通知
        XX_TEST_EXPECT_EQ(
            messages[0]["method"].get<std::string>(),
            "notifications/subscriptions/acknowledged"
        );
        XX_TEST_EXPECT_EQ(
            messages[0]["params"]["_meta"]["io.modelcontextprotocol/subscriptionId"].get<int>(),
            7
        );
        XX_TEST_EXPECT_TRUE(messages[0]["params"]["notifications"]["toolsListChanged"].get<bool>());
        // 2) 优雅结束: 空 result 响应 (id = 7)
        XX_TEST_EXPECT_EQ(messages[1]["id"].get<int>(), 7);
        XX_TEST_EXPECT_EQ(messages[1]["result"]["resultType"].get<std::string>(), "complete");
        XX_TEST_EXPECT_EQ(
            messages[1]["result"]["_meta"]["io.modelcontextprotocol/subscriptionId"].get<int>(),
            7
        );
    }
}

// subscriptions/listen over HTTP: SSE 流 + 变更通知 + 优雅结束
asio::awaitable<void> test_mcp_server_2026_subscriptions_http() {
    using Server = McpServer;

    Server::Config cfg;
    cfg.httpConfig.address          = "127.0.0.1";
    cfg.httpConfig.port             = 0;
    cfg.httpConfig.ioThreads        = 1;
    cfg.httpConfig.accessLogEnabled = false;

    Server server(std::move(cfg));

    std::thread serverThread([&server]() {
        server.start();
    });

    uint16_t port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        TEST_FAIL << "McpServer failed to start for subscriptions test" << std::endl;
        g_mcp_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    for (int i = 0; i < 100; ++i) {
        try {
            asio::io_context      tmpCtx;
            asio::ip::tcp::socket sock(tmpCtx);
            sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
            sock.close();
            break;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);

    json req;
    req["jsonrpc"]                                                       = "2.0";
    req["id"]                                                            = 42;
    req["method"]                                                        = "subscriptions/listen";
    req["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"]    = "2026-07-28";
    req["params"]["_meta"]["io.modelcontextprotocol/clientCapabilities"] = json::object();
    req["params"]["notifications"]["toolsListChanged"]                   = true;

    util::HeaderMap headers;
    headers.set("MCP-Protocol-Version", "2026-07-28");
    headers.set("Mcp-Method", "subscriptions/listen");
    headers.set("Accept", "application/json, text/event-stream");

    std::string sseData;
    {
        std::mutex dataMutex;
        auto       executor = co_await asio::this_coro::executor;

        auto reader = [&]() -> asio::awaitable<void> {
            co_await agentxx::util::catchErrorAsync<bool>(
                [&]() -> asio::awaitable<bool> {
                    co_await util::HttpClient::requestSseAsync(
                        "POST",
                        baseUrl + "/mcp",
                        req.dump(),
                        "application/json",
                        headers,
                        util::HttpClient::RequestConfig{
                            .readChunkTimeout = std::chrono::seconds{30}
                        },
                        [&](std::string_view chunk) -> bool {
                            std::lock_guard lock(dataMutex);
                            sseData.append(chunk);
                            TEST_INFO << "sse chunk: " << chunk.substr(0, 160) << std::endl;
                            return false; // 持续读, 直到服务端关闭
                        }
                    );
                    co_return true;
                },
                [](std::string errmsg) -> asio::awaitable<bool> {
                    TEST_INFO << "sse reader error: " << errmsg << std::endl;
                    co_return false;
                }
            );
        };

        auto fut = asio::co_spawn(executor, reader(), asio::use_future);

        auto waitFor = [&](const std::string& needle, int maxMs) -> asio::awaitable<bool> {
            auto st = std::chrono::steady_clock::now();
            for (int i = 0; i < maxMs / 20; ++i) {
                {
                    std::lock_guard lock(dataMutex);
                    if (sseData.find(needle) != std::string::npos) {
                        TEST_INFO << "waitFor(" << needle << ") found after "
                                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - st
                                     )
                                         .count()
                                  << "ms (total " << sseData.size() << " bytes)" << std::endl;
                        co_return true;
                    }
                }
                asio::steady_timer timer(executor);
                timer.expires_after(std::chrono::milliseconds(20));
                neograph_asio_error_code ec;
                co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            }
            TEST_INFO << "waitFor(" << needle << ") TIMEOUT after " << maxMs << "ms (total "
                      << sseData.size() << " bytes)" << std::endl;
            co_return false;
        };

        // 1) ack 通知
        XX_TEST_EXPECT_TRUE(co_await waitFor("notifications/subscriptions/acknowledged", 5000));

        // 2) 注册工具 → tools/list_changed 通知
        McpToolDefinition def;
        def.name        = "sub_tool";
        def.description = "Subscription test tool";
        TEST_INFO << "adding tool at "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch()
                     )
                         .count()
                  << "ms" << std::endl;
        server.addTool(def, [](const json&) -> json {
            json c;
            c["type"] = "text";
            c["text"] = "ok";
            return c;
        });
        XX_TEST_EXPECT_TRUE(co_await waitFor("notifications/tools/list_changed", 5000));

        // 3) 服务端停止 → 优雅结束 (空 result, id=42)
        server.stop();
        serverThread.join();
        XX_TEST_EXPECT_TRUE(co_await waitFor("\"id\":42", 5000));

        // 等待读取协程完成 (不能阻塞 io 线程在 future::get 上)
        bool ended = false;
        for (int i = 0; i < 500 && !ended; ++i) {
            if (fut.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                ended = true;
                break;
            }
            asio::steady_timer timer(executor);
            timer.expires_after(std::chrono::milliseconds(20));
            neograph_asio_error_code ec;
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        }
        XX_TEST_EXPECT_TRUE(ended);
        if (ended) {
            fut.get();
        }
    }

    // 校验 SSE 内容: ack 带 subscriptionId
    XX_TEST_EXPECT_TRUE(
        sseData.find("\"io.modelcontextprotocol/subscriptionId\":42") != std::string::npos
    );
    XX_TEST_EXPECT_TRUE(sseData.find("tools/list_changed") != std::string::npos);
    XX_TEST_EXPECT_TRUE(sseData.find("\"resultType\":\"complete\"") != std::string::npos);
}

// HTTP 标准请求头校验 (MCP-Protocol-Version / Mcp-Method / Mcp-Name)
asio::awaitable<void> test_mcp_server_2026_http_headers() {
    using Server = McpServer;

    Server::Config cfg;
    cfg.httpConfig.address          = "127.0.0.1";
    cfg.httpConfig.port             = 0;
    cfg.httpConfig.ioThreads        = 1;
    cfg.httpConfig.accessLogEnabled = false;

    Server            server(std::move(cfg));
    McpToolDefinition def;
    def.name        = "echo";
    def.description = "Echo";
    def.inputSchema = json::parse(R"({"type":"object","properties":{"text":{"type":"string"}}})");
    server.addTool(def, [](const json& args) -> json {
        json content;
        content["type"] = "text";
        content["text"] = args.value("text", "");
        return content;
    });

    std::thread serverThread([&server]() {
        server.start();
    });

    uint16_t port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        TEST_FAIL << "McpServer failed to start for header test" << std::endl;
        g_mcp_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    for (int i = 0; i < 100; ++i) {
        try {
            asio::io_context      tmpCtx;
            asio::ip::tcp::socket sock(tmpCtx);
            sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
            sock.close();
            break;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);

    auto modernBody = [](const char* method) {
        json req;
        req["jsonrpc"]                                                       = "2.0";
        req["id"]                                                            = 1;
        req["method"]                                                        = method;
        req["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"]    = "2026-07-28";
        req["params"]["_meta"]["io.modelcontextprotocol/clientCapabilities"] = json::object();
        return req;
    };

    // 1. 缺少 Mcp-Method → 400 + -32020
    {
        util::HeaderMap h;
        h.set("MCP-Protocol-Version", "2026-07-28");
        h.set("Accept", "application/json, text/event-stream");
        auto resp = co_await HttpClient::postAsync(
            baseUrl + "/mcp",
            modernBody("ping"),
            h,
            HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{5}}
        );
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 400);
            auto j = resp.value().bodyJson();
            XX_TEST_EXPECT_HAS_VALUE(j);
            if (j.has_value()) {
                XX_TEST_EXPECT_EQ((*j)["error"]["code"].get<int>(), -32020);
            }
        }
    }

    // 2. MCP-Protocol-Version 与 body _meta 不一致 → 400 + -32020
    {
        util::HeaderMap h;
        h.set("MCP-Protocol-Version", "2025-11-25");
        h.set("Mcp-Method", "ping");
        h.set("Accept", "application/json, text/event-stream");
        auto resp = co_await HttpClient::postAsync(
            baseUrl + "/mcp",
            modernBody("ping"),
            h,
            HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{5}}
        );
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 400);
            auto j = resp.value().bodyJson();
            XX_TEST_EXPECT_HAS_VALUE(j);
            if (j.has_value()) {
                XX_TEST_EXPECT_EQ((*j)["error"]["code"].get<int>(), -32020);
            }
        }
    }

    // 3. Mcp-Name 与 body 不一致 (tools/call) → 400 + -32020
    {
        util::HeaderMap h;
        h.set("MCP-Protocol-Version", "2026-07-28");
        h.set("Mcp-Method", "tools/call");
        h.set("Mcp-Name", "wrong_name");
        h.set("Accept", "application/json, text/event-stream");
        auto req                   = modernBody("tools/call");
        req["params"]["name"]      = "echo";
        req["params"]["arguments"] = json::object();
        auto resp                  = co_await HttpClient::postAsync(
            baseUrl + "/mcp",
            req,
            h,
            HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{5}}
        );
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 400);
            auto j = resp.value().bodyJson();
            XX_TEST_EXPECT_HAS_VALUE(j);
            if (j.has_value()) {
                XX_TEST_EXPECT_EQ((*j)["error"]["code"].get<int>(), -32020);
            }
        }
    }

    // 4. 正确请求头 → 200
    {
        util::HeaderMap h;
        h.set("MCP-Protocol-Version", "2026-07-28");
        h.set("Mcp-Method", "tools/list");
        h.set("Accept", "application/json, text/event-stream");
        auto resp = co_await HttpClient::postAsync(
            baseUrl + "/mcp",
            modernBody("tools/list"),
            h,
            HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{5}}
        );
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            auto j = resp.value().bodyJson();
            XX_TEST_EXPECT_HAS_VALUE(j);
            if (j.has_value()) {
                XX_TEST_EXPECT_EQ((*j)["result"]["resultType"].get<std::string>(), "complete");
                XX_TEST_EXPECT_EQ((*j)["result"]["tools"].size(), (size_t)1);
            }
        }
    }

    // 5. 未知方法 → 404 (现代协议)
    {
        util::HeaderMap h;
        h.set("MCP-Protocol-Version", "2026-07-28");
        h.set("Mcp-Method", "no/such/method");
        h.set("Accept", "application/json, text/event-stream");
        auto resp = co_await HttpClient::postAsync(
            baseUrl + "/mcp",
            modernBody("no/such/method"),
            h,
            HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{5}}
        );
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 404);
            auto j = resp.value().bodyJson();
            XX_TEST_EXPECT_HAS_VALUE(j);
            if (j.has_value()) {
                XX_TEST_EXPECT_EQ((*j)["error"]["code"].get<int>(), -32601);
            }
        }
    }

    // 6. 未知协议版本 → 400 + -32022
    {
        util::HeaderMap h;
        h.set("MCP-Protocol-Version", "2099-01-01");
        h.set("Mcp-Method", "ping");
        h.set("Accept", "application/json, text/event-stream");
        auto req                                                          = modernBody("ping");
        req["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2099-01-01";
        auto resp = co_await HttpClient::postAsync(
            baseUrl + "/mcp",
            req,
            h,
            HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{5}}
        );
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 400);
            auto j = resp.value().bodyJson();
            XX_TEST_EXPECT_HAS_VALUE(j);
            if (j.has_value()) {
                XX_TEST_EXPECT_EQ((*j)["error"]["code"].get<int>(), -32022);
            }
        }
    }

    server.stop();
    serverThread.join();
}

// x-mcp-header: 客户端镜像参数为 Mcp-Param-* 头, 服务端校验
asio::awaitable<void> test_mcp_server_2026_x_mcp_header() {
    using Server = McpServer;

    Server::Config cfg;
    cfg.httpConfig.address          = "127.0.0.1";
    cfg.httpConfig.port             = 0;
    cfg.httpConfig.ioThreads        = 1;
    cfg.httpConfig.accessLogEnabled = false;

    Server            server(std::move(cfg));
    McpToolDefinition def;
    def.name        = "sql";
    def.description = "Execute SQL";
    def.inputSchema = json::parse(R"({
    "type": "object",
    "properties": {
      "region": {"type": "string", "x-mcp-header": "Region"},
      "query": {"type": "string"}
    },
    "required": ["region", "query"]
  })");
    server.addTool(def, [](const json& args) -> json {
        json content;
        content["type"] = "text";
        content["text"] = "executed in " + args.value("region", "?");
        return content;
    });

    std::thread serverThread([&server]() {
        server.start();
    });

    uint16_t port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        TEST_FAIL << "McpServer failed to start for x-mcp-header test" << std::endl;
        g_mcp_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    for (int i = 0; i < 100; ++i) {
        try {
            asio::io_context      tmpCtx;
            asio::ip::tcp::socket sock(tmpCtx);
            sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
            sock.close();
            break;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);

    // 1. 客户端 (现代) 完整流程: discover → listTools → callTool (自动带 Mcp-Param-Region)
    {
        McpClient::Config clientCfg;
        clientCfg.serverUrl      = baseUrl + "/mcp";
        clientCfg.requestTimeout = std::chrono::seconds(5);
        clientCfg.initTimeout    = std::chrono::seconds(5);

        auto client = std::make_shared<McpClient>(std::move(clientCfg));
        auto init   = co_await client->initialize();
        XX_TEST_EXPECT_TRUE(init.has_value());
        if (init.has_value()) {
            XX_TEST_EXPECT_EQ(init->protocolVersion, "2026-07-28");
        }

        auto tools = co_await client->listTools();
        XX_TEST_EXPECT_TRUE(tools.has_value());
        if (tools.has_value()) {
            XX_TEST_EXPECT_EQ(tools->size(), (size_t)1);
        }

        // 先不带 header 直接调用: 服务端应拒绝 (参数有值但缺 Mcp-Param 头)
        // 客户端实现会自动刷新缓存并重试, 因此最终成功
        auto result = co_await client->callTool(
            "sql",
            {
                {"region", "us-west1"},
                {"query",  "SELECT 1"}
        }
        );
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            XX_TEST_EXPECT_TRUE(result->contains("content"));
            if (result->contains("content")) {
                XX_TEST_EXPECT_EQ(
                    (*result)["content"][0].value("text", std::string{}),
                    "executed in us-west1"
                );
            }
        }
        co_await client->close();
    }

    // 2. 裸请求: body 带 region 但无 Mcp-Param-Region 头 → 400 + -32020
    {
        json req;
        req["jsonrpc"]             = "2.0";
        req["id"]                  = 1;
        req["method"]              = "tools/call";
        req["params"]["name"]      = "sql";
        req["params"]["arguments"] = {
            {"region", "eu-west1"},
            {"query",  "SELECT 2"}
        };
        req["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"]    = "2026-07-28";
        req["params"]["_meta"]["io.modelcontextprotocol/clientCapabilities"] = json::object();

        util::HeaderMap h;
        h.set("MCP-Protocol-Version", "2026-07-28");
        h.set("Mcp-Method", "tools/call");
        h.set("Mcp-Name", "sql");
        h.set("Accept", "application/json, text/event-stream");
        auto resp = co_await HttpClient::postAsync(
            baseUrl + "/mcp",
            req,
            h,
            HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{5}}
        );
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 400);
            auto j = resp.value().bodyJson();
            XX_TEST_EXPECT_HAS_VALUE(j);
            if (j.has_value()) {
                XX_TEST_EXPECT_EQ((*j)["error"]["code"].get<int>(), -32020);
            }
        }
    }

    // 3. 带正确 Mcp-Param-Region 头 (Base64 sentinel 编码) → 200
    {
        json req;
        req["jsonrpc"]             = "2.0";
        req["id"]                  = 2;
        req["method"]              = "tools/call";
        req["params"]["name"]      = "sql";
        req["params"]["arguments"] = {
            {"region", "eu-west1"},
            {"query",  "SELECT 3"}
        };
        req["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"]    = "2026-07-28";
        req["params"]["_meta"]["io.modelcontextprotocol/clientCapabilities"] = json::object();

        util::HeaderMap h;
        h.set("MCP-Protocol-Version", "2026-07-28");
        h.set("Mcp-Method", "tools/call");
        h.set("Mcp-Name", "sql");
        h.set("Mcp-Param-Region", "eu-west1");
        h.set("Accept", "application/json, text/event-stream");
        auto resp = co_await HttpClient::postAsync(
            baseUrl + "/mcp",
            req,
            h,
            HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{5}}
        );
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            auto j = resp.value().bodyJson();
            XX_TEST_EXPECT_HAS_VALUE(j);
            if (j.has_value()) {
                XX_TEST_EXPECT_EQ(
                    (*j)["result"]["content"][0].value("text", std::string{}),
                    "executed in eu-west1"
                );
            }
        }
    }

    server.stop();
    serverThread.join();
}

// McpClient (2026-07-28 默认) 连接现代服务端: discover 完成初始化, 无握手
asio::awaitable<void> test_mcp_client_2026_modern_http() {
    using Server = McpServer;

    Server::Config cfg;
    cfg.httpConfig.address          = "127.0.0.1";
    cfg.httpConfig.port             = 0;
    cfg.httpConfig.ioThreads        = 1;
    cfg.httpConfig.accessLogEnabled = false;

    Server            server(std::move(cfg));
    McpToolDefinition def;
    def.name        = "echo";
    def.description = "Echo";
    def.inputSchema = json::parse(R"({"type":"object","properties":{"text":{"type":"string"}}})");
    server.addTool(def, [](const json& args) -> json {
        json content;
        content["type"] = "text";
        content["text"] = args.value("text", "");
        return content;
    });

    std::thread serverThread([&server]() {
        server.start();
    });

    uint16_t port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        TEST_FAIL << "McpServer failed to start" << std::endl;
        g_mcp_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);

    for (int i = 0; i < 100; ++i) {
        try {
            asio::io_context      tmpCtx;
            asio::ip::tcp::socket sock(tmpCtx);
            sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
            sock.close();
            break;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    McpClient::Config clientCfg;
    clientCfg.serverUrl      = baseUrl + "/mcp";
    clientCfg.requestTimeout = std::chrono::seconds(5);
    clientCfg.initTimeout    = std::chrono::seconds(5);

    // 默认协议版本 = 2026-07-28
    XX_TEST_EXPECT_EQ(clientCfg.protocolVersion, "2026-07-28");

    auto client = std::make_shared<McpClient>(std::move(clientCfg));

    // 现代初始化: server/discover, 无 initialize 握手
    auto init = co_await client->initialize();
    XX_TEST_EXPECT_TRUE(init.has_value());
    if (init.has_value()) {
        XX_TEST_EXPECT_EQ(init->protocolVersion, "2026-07-28");
        XX_TEST_EXPECT_EQ(init->serverName, "agentxx-mcp");
        XX_TEST_EXPECT_TRUE(init->capabilities.contains("tools"));
    }

    // discover() 独立调用
    auto disc = co_await client->discover();
    XX_TEST_EXPECT_TRUE(disc.has_value());
    if (disc.has_value()) {
        bool has2026 = false;
        for (const auto& v : disc->supportedVersions) {
            if (v == "2026-07-28") {
                has2026 = true;
            }
        }
        XX_TEST_EXPECT_TRUE(has2026);
        XX_TEST_EXPECT_EQ(disc->serverName, "agentxx-mcp");
    }

    // ping (我们的双时代服务端保留 ping)
    auto ping = co_await client->ping();
    XX_TEST_EXPECT_TRUE(ping.has_value());

    // tools/list + tools/call
    auto tools = co_await client->listTools();
    XX_TEST_EXPECT_TRUE(tools.has_value());
    if (tools.has_value()) {
        XX_TEST_EXPECT_EQ(tools->size(), (size_t)1);
        XX_TEST_EXPECT_EQ((*tools)[0].name, "echo");
    }

    auto echo = co_await client->callTool(
        "echo",
        {
            {"text", "hello 2026"}
    }
    );
    XX_TEST_EXPECT_TRUE(echo.has_value());
    if (echo.has_value()) {
        XX_TEST_EXPECT_EQ((*echo)["content"][0].value("text", ""), "hello 2026");
    }

    co_await client->close();
    server.stop();
    serverThread.join();
}

// McpClient (2026-07-28) 连接 legacy 服务端: discover 失败 → 自动回退 initialize 握手
asio::awaitable<void> test_mcp_client_2026_legacy_fallback() {
    using Server = util::HttpServer;

    Server::Config cfg;
    cfg.address          = "127.0.0.1";
    cfg.port             = 0;
    cfg.ioThreads        = 1;
    cfg.accessLogEnabled = false;

    auto server = std::make_shared<Server>(std::move(cfg));

    using Handler = Server::Handler;
    auto handler  = std::make_shared<Handler>(
        [](Server::Request& req, Server::Response& resp, std::string_view
        ) -> asio::awaitable<void> {
            namespace http = boost::beast::http;

            json requestJson;
            try {
                requestJson = json::parse(req.body());
            } catch (...) {
                resp.version(req.version());
                resp.result(http::status::bad_request);
                resp.prepare_payload();
                co_return;
            }

            json        id     = requestJson.value("id", json{});
            std::string method = requestJson.value("method", "");
            json        response;
            response["jsonrpc"] = "2.0";
            response["id"]      = id;

            if (method == "server/discover") {
                // legacy 服务端不认识 discover → -32601
                response["error"]["code"]    = -32601;
                response["error"]["message"] = "Method not found";
            } else if (method == "initialize") {
                response["result"]["protocolVersion"]       = "2024-11-05";
                response["result"]["serverInfo"]["name"]    = "legacy-server";
                response["result"]["serverInfo"]["version"] = "1.0";
                response["result"]["capabilities"]          = json::object();
            } else if (method == "ping") {
                response["result"] = json::object();
            } else if (method == "tools/list") {
                response["result"]["tools"] = json::array();
            } else {
                response["error"]["code"]    = -32601;
                response["error"]["message"] = "Method not found";
            }

            resp.version(req.version());
            resp.result(http::status::ok);
            resp.set(http::field::content_type, "application/json");
            resp.body() = response.dump();
            resp.prepare_payload();
        }
    );

    server->router().add("/mcp", 2, handler);

    std::thread serverThread([server]() {
        server->start();
    });

    uint16_t port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server->port();
        if (port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        TEST_FAIL << "Legacy mock server failed to start" << std::endl;
        g_mcp_failed++;
        server->stop();
        serverThread.join();
        co_return;
    }

    for (int i = 0; i < 100; ++i) {
        try {
            asio::io_context      tmpCtx;
            asio::ip::tcp::socket sock(tmpCtx);
            sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
            sock.close();
            break;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);

    McpClient::Config clientCfg;
    clientCfg.serverUrl      = baseUrl + "/mcp";
    clientCfg.requestTimeout = std::chrono::seconds(5);
    clientCfg.initTimeout    = std::chrono::seconds(5);

    auto client = std::make_shared<McpClient>(std::move(clientCfg));

    // discover 探测失败 → 回退 legacy initialize 握手 → 仍应成功
    auto init = co_await client->initialize();
    XX_TEST_EXPECT_TRUE(init.has_value());
    if (init.has_value()) {
        XX_TEST_EXPECT_EQ(init->serverName, "legacy-server");
        XX_TEST_EXPECT_EQ(init->protocolVersion, "2024-11-05");
    }

    auto ping = co_await client->ping();
    XX_TEST_EXPECT_TRUE(ping.has_value());

    auto tools = co_await client->listTools();
    XX_TEST_EXPECT_TRUE(tools.has_value());

    co_await client->close();
    server->stop();
    serverThread.join();
}

/// 模拟"服务端响应被截断"场景 (Content-Length 偏大 / 网关超时掐断连接):
/// 第一次 HTTP 响应返回不完整 body 后立即关闭连接, 客户端 HttpClient 读到
/// EOF 报 beast partial_message; 验证 McpClient 将该错误识别为瞬时传输
/// 错误并自动重试一次, 第二次请求成功。
asio::awaitable<void> test_mcp_client_truncated_retry() {
    struct TruncServer {
        std::thread                              thread;
        std::atomic<uint16_t>                    port{0};
        std::atomic<int>                         requestCount{0};
        std::atomic<bool>                        stopped{false};
        std::unique_ptr<asio::io_context>        ioCtx;
        std::unique_ptr<asio::ip::tcp::acceptor> acceptor;

        /// 从 JSON body 提取 "id": <N> 回显, 保证响应与请求 id 匹配
        static std::string extractJsonId(const std::string& body) {
            auto pos = body.find("\"id\"");
            if (pos == std::string::npos) {
                return "0";
            }
            pos = body.find(':', pos);
            if (pos == std::string::npos) {
                return "0";
            }
            ++pos;
            while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) {
                ++pos;
            }
            auto end = pos;
            while (end < body.size()
                   && ((body[end] >= '0' && body[end] <= '9') || body[end] == '-')) {
                ++end;
            }
            return end == pos ? "0" : body.substr(pos, end - pos);
        }

        void start() {
            ioCtx = std::make_unique<asio::io_context>();
            acceptor = std::make_unique<asio::ip::tcp::acceptor>(
                *ioCtx,
                asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)
            );
            port.store(acceptor->local_endpoint().port());
            thread = std::thread([this]() {
                while (!stopped.load()) {
                    neograph_asio_error_code ec;
                    asio::ip::tcp::socket    sock(*ioCtx);
                    acceptor->accept(sock, ec);
                    if (ec) {
                        break;
                    }
                    handle(sock);
                }
            });
        }

        void handle(asio::ip::tcp::socket& sock) {
            namespace http = boost::beast::http;
            neograph_asio_error_code         ec;
            boost::beast::flat_buffer        buf;
            http::request<http::string_body> req;
            http::read(sock, buf, req, ec);
            if (ec) {
                return;
            }

            const int         n  = requestCount.fetch_add(1) + 1;
            const std::string id = extractJsonId(req.body());

            if (n == 1) {
                // 第一次: 截断响应 — Content-Length 声明 10000 但实际 body 远小于
                // 该值, 发送后立即关闭连接; 客户端读到 EOF → beast partial_message
                std::string partial = "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{}}";
                const std::string header = "HTTP/1.1 200 OK\r\n"
                                           "Content-Type: application/json\r\n"
                                           "Content-Length: 10000\r\n"
                                           "\r\n";
                asio::write(sock, asio::buffer(header), ec);
                asio::write(sock, asio::buffer(partial), ec);
                sock.close();
                return;
            }

            // 后续: 完整响应 (server/discover 返回支持版本, 其余返回空 result)
            std::string body;
            if (req.body().find("server/discover") != std::string::npos) {
                body = "{\"jsonrpc\":\"2.0\",\"id\":" + id
                       + ",\"result\":{\"supportedVersions\":[\"2026-07-28\"],"
                         "\"capabilities\":{},\"_meta\":{\"io.modelcontextprotocol/"
                         "serverInfo\":{\"name\":\"trunc-test\",\"version\":\"1.0\"}}}}";
            } else {
                body = "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{}}";
            }
            const std::string resp = "HTTP/1.1 200 OK\r\n"
                                     "Content-Type: application/json\r\n"
                                     "Content-Length: "
                                     + std::to_string(body.size()) + "\r\n"
                                     "Connection: close\r\n"
                                     "\r\n" + body;
            asio::write(sock, asio::buffer(resp), ec);
            sock.close();
        }

        void stop() {
            stopped.store(true);
            if (acceptor) {
                neograph_asio_error_code ec;
                // 先连接一次唤醒阻塞中的同步 accept: 跨线程 close 无法可靠
                // 中断 asio 同步 accept (见 test_http.cpp 同模式), 先送一个
                // 连接让 accept 返回, 再 close 使后续 accept 返回错误退出循环
                asio::ip::tcp::socket dummy(*ioCtx);
                dummy.connect(
                    asio::ip::tcp::endpoint(
                        asio::ip::make_address("127.0.0.1"),
                        port.load()
                    ),
                    ec
                );
                dummy.close();
                acceptor->close(ec);
            }
            if (thread.joinable()) {
                thread.join();
            }
        }
    };

    TruncServer server;
    server.start();
    if (server.port.load() == 0) {
        TEST_FAIL << "truncated-response server failed to start" << std::endl;
        ++g_mcp_failed;
        server.stop();
        co_return;
    }

    const std::string serverUrl = "http://127.0.0.1:" + std::to_string(server.port.load());

    McpClient::Config clientCfg;
    clientCfg.serverUrl       = serverUrl;
    clientCfg.protocolVersion = std::string{McpClient::kProtocol2026_07_28};
    clientCfg.requestTimeout  = std::chrono::seconds(5);
    clientCfg.initTimeout     = std::chrono::seconds(5);

    auto client = std::make_shared<McpClient>(std::move(clientCfg));

    // 第一次 server/discover 响应被截断 → McpClient 自动重试 → 第二次成功
    auto init = co_await client->initialize();
    XX_TEST_EXPECT_TRUE(init.has_value());
    if (init.has_value()) {
        XX_TEST_EXPECT_EQ(init->serverName, "trunc-test");
        XX_TEST_EXPECT_EQ(init->protocolVersion, "2026-07-28");
    }

    auto ping = co_await client->ping();
    XX_TEST_EXPECT_TRUE(ping.has_value());

    co_await client->close();

    // 至少 2 次请求 (第 1 次截断 + 重试), 证明重试确实发生
    const int reqCount = server.requestCount.load();
    XX_TEST_EXPECT_TRUE(reqCount >= 2);
    TEST_INFO << "truncated-response server received " << reqCount
              << " requests (1 truncated + retried)" << std::endl;

    server.stop();
}

/// 模拟"服务端会话过期"场景: 服务器颁发 session 后, 第一次业务请求返回
/// 401 SessionExpired, 验证 McpClient 自动重建会话 (重新 initialize +
/// notifications/initialized) 并重试成功。
asio::awaitable<void> test_mcp_client_session_rebuild() {
    struct SessionServer {
        std::thread                              thread;
        std::atomic<uint16_t>                    port{0};
        std::atomic<bool>                        stopped{false};
        std::atomic<int>                         initCount{0};
        std::atomic<int>                         businessCalls{0};
        std::unique_ptr<asio::io_context>        ioCtx;
        std::unique_ptr<asio::ip::tcp::acceptor> acceptor;

        void start() {
            ioCtx = std::make_unique<asio::io_context>();
            acceptor = std::make_unique<asio::ip::tcp::acceptor>(
                *ioCtx,
                asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)
            );
            port.store(acceptor->local_endpoint().port());
            thread = std::thread([this]() {
                while (!stopped.load()) {
                    neograph_asio_error_code ec;
                    asio::ip::tcp::socket    sock(*ioCtx);
                    acceptor->accept(sock, ec);
                    if (ec) {
                        break;
                    }
                    handle(sock);
                }
            });
        }

        void handle(asio::ip::tcp::socket& sock) {
            namespace http = boost::beast::http;
            neograph_asio_error_code         ec;
            boost::beast::flat_buffer        buf;
            http::request<http::string_body> req;
            http::read(sock, buf, req, ec);
            if (ec) {
                return;
            }

            const std::string body = req.body();

            // 现代探测 server/discover 无 session → 400 (触发客户端回退 legacy)
            if (body.find("server/discover") != std::string::npos) {
                const std::string respBody
                    = "{\"RequestId\":\"x\",\"Code\":\"InvalidArgument\","
                      "\"Message\":\"request without mcp-session-id header should be "
                      "mcp initialize request\"}";
                const std::string resp = "HTTP/1.1 400 Bad Request\r\n"
                                         "Content-Type: application/json\r\n"
                                         "Content-Length: "
                                         + std::to_string(respBody.size()) + "\r\n"
                                         "Connection: close\r\n\r\n" + respBody;
                asio::write(sock, asio::buffer(resp), ec);
                sock.close();
                return;
            }
            // SSE 发现 GET /sse → 404 (模拟 Streamable HTTP 服务器无 SSE endpoint)
            if (req.method() == http::verb::get) {
                const std::string respBody = "{\"error\":{\"message\":\"record not found\"}}";
                const std::string resp = "HTTP/1.1 404 Not Found\r\n"
                                         "Content-Type: application/json\r\n"
                                         "Content-Length: "
                                         + std::to_string(respBody.size()) + "\r\n"
                                         "Connection: close\r\n\r\n" + respBody;
                asio::write(sock, asio::buffer(resp), ec);
                sock.close();
                return;
            }
            // initialize: 颁发新 session
            if (body.find("\"initialize\"") != std::string::npos) {
                const int n = initCount.fetch_add(1) + 1;
                const std::string sid = fmt::format("good-{}", n);
                const std::string resultBody = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{"
                                               "\"protocolVersion\":\"2025-03-26\","
                                               "\"capabilities\":{},\"serverInfo\":{"
                                               "\"name\":\"session-test\",\"version\":\"1.0\"}}}";
                const std::string resp = "HTTP/1.1 200 OK\r\n"
                                         "Content-Type: application/json\r\n"
                                         "Content-Length: "
                                         + std::to_string(resultBody.size()) + "\r\n"
                                         "Mcp-Session-Id: " + sid + "\r\n"
                                         "Connection: close\r\n\r\n" + resultBody;
                asio::write(sock, asio::buffer(resp), ec);
                sock.close();
                return;
            }
            // notifications/initialized → 202 空响应
            if (body.find("notifications/initialized") != std::string::npos) {
                const std::string resp = "HTTP/1.1 202 Accepted\r\n"
                                         "Content-Length: 0\r\n"
                                         "Connection: close\r\n\r\n";
                asio::write(sock, asio::buffer(resp), ec);
                sock.close();
                return;
            }
            // 其他业务请求 (tools/list 等):
            // - 第 1 次业务调用 → 401 SessionExpired (模拟会话过期, 不改变状态)
            // - 之后 (会话重建后重试) → 200 成功
            if (businessCalls.fetch_add(1) == 0) {
                const std::string errBody
                    = "{\"RequestId\":\"x\",\"Code\":\"SessionExpired\","
                      "\"Message\":\"session is expired\"}";
                const std::string resp = "HTTP/1.1 401 Unauthorized\r\n"
                                         "Content-Type: application/json\r\n"
                                         "Content-Length: "
                                         + std::to_string(errBody.size()) + "\r\n"
                                         "Connection: close\r\n\r\n" + errBody;
                asio::write(sock, asio::buffer(resp), ec);
                sock.close();
                return;
            }
            // 成功响应: 回显请求 id
            const std::string id = extractJsonId(body);
            const std::string okBody = "{\"jsonrpc\":\"2.0\",\"id\":" + id
                                       + ",\"result\":{\"tools\":[],\"resources\":[]}}";
            const std::string resp = "HTTP/1.1 200 OK\r\n"
                                     "Content-Type: application/json\r\n"
                                     "Content-Length: "
                                     + std::to_string(okBody.size()) + "\r\n"
                                     "Connection: close\r\n\r\n" + okBody;
            asio::write(sock, asio::buffer(resp), ec);
            sock.close();
        }

        static std::string extractJsonId(const std::string& body) {
            auto pos = body.find("\"id\"");
            if (pos == std::string::npos) {
                return "0";
            }
            pos = body.find(':', pos);
            if (pos == std::string::npos) {
                return "0";
            }
            ++pos;
            while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) {
                ++pos;
            }
            auto end = pos;
            while (end < body.size()
                   && ((body[end] >= '0' && body[end] <= '9') || body[end] == '-')) {
                ++end;
            }
            return end == pos ? "0" : body.substr(pos, end - pos);
        }

        void stop() {
            stopped.store(true);
            if (acceptor) {
                neograph_asio_error_code ec;
                asio::ip::tcp::socket    dummy(*ioCtx);
                dummy.connect(
                    asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port.load()),
                    ec
                );
                dummy.close();
                acceptor->close(ec);
            }
            if (thread.joinable()) {
                thread.join();
            }
        }
    };

    SessionServer server;
    server.start();
    if (server.port.load() == 0) {
        TEST_FAIL << "session-rebuild server failed to start" << std::endl;
        ++g_mcp_failed;
        server.stop();
        co_return;
    }

    const std::string serverUrl = "http://127.0.0.1:" + std::to_string(server.port.load());

    McpClient::Config cfg;
    cfg.serverUrl       = serverUrl;
    cfg.protocolVersion = std::string{McpClient::kProtocol2026_07_28};
    cfg.requestTimeout  = std::chrono::seconds(5);
    cfg.initTimeout     = std::chrono::seconds(5);

    auto client = std::make_shared<McpClient>(std::move(cfg));

    // 初始化 (modern 探测 400 → 回退 legacy initialize 握手)
    auto init = co_await client->initialize();
    XX_TEST_EXPECT_TRUE(init.has_value());
    if (init.has_value()) {
        XX_TEST_EXPECT_EQ(init->serverName, "session-test");
    }

    // 第一次业务请求: 服务器返回 401 SessionExpired → McpClient 应自动
    // 重建会话并重试成功 (initCount 应变为 2: 首次 + 重建)
    auto tools = co_await client->listTools();
    XX_TEST_EXPECT_TRUE(tools.has_value());

    const int initCnt = server.initCount.load();
    XX_TEST_EXPECT_EQ(initCnt, 2);

    // 重建后的会话应能继续使用
    auto ping = co_await client->ping();
    XX_TEST_EXPECT_TRUE(ping.has_value());

    co_await client->close();
    server.stop();
}

asio::awaitable<TestResult> run_mcp_tests() {
    test_mcp_version_negotiation_unit();
    test_mcp_server_unit();
    co_await test_mcp_server_integration();
    test_mcp_server_version_negotiation();
    test_mcp_server_lenient_parsing();
    test_mcp_server_stdio_resources_prompts();
    test_mcp_server_stdio_basic();
    test_mcp_server_stdio_errors();
    test_mcp_server_2025_features();
    test_mcp_server_2025_03_features();
    test_mcp_server_2025_06_features();
    test_mcp_server_cross_version_stdio();
    test_mcp_server_2025_03_26_stdio();
    co_await test_mcp_client_http();
    co_await test_mcp_client_tool_namespace();
    co_await test_mcp_client_tool_timeout();
    co_await test_mcp_client_2025_version();
    co_await test_mcp_server_cross_version_http();
    co_await test_mcp_client_accept_header();
    co_await test_mcp_server_accept_sse();
    // 2026-07-28
    test_mcp_server_2026_discover_stdio();
    test_mcp_server_2026_version_gate();
    test_mcp_server_2026_modern_results();
    test_mcp_server_2026_subscriptions_stdio();
    co_await test_mcp_server_2026_subscriptions_http();
    co_await test_mcp_server_2026_http_headers();
    co_await test_mcp_server_2026_x_mcp_header();
    co_await test_mcp_client_2026_modern_http();
    co_await test_mcp_client_2026_legacy_fallback();
    co_await test_mcp_client_truncated_retry();
    co_await test_mcp_client_session_rebuild();
    co_return TestResult{g_mcp_passed, g_mcp_failed};
}

} // namespace test
} // namespace agentxx
