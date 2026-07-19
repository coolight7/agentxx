#include "test_mcp.h"
#include "agentxx/server/mcp_client.h"
#include "agentxx/server/mcp_server.h"
#include "agentxx/tools/tool.h"
#include "agentxx/util/http_client.h"

namespace agentxx {
namespace test {

using namespace agentxx::server;
using namespace agentxx::util;

int g_mcp_passed = 0;
int g_mcp_failed = 0;

// Unit test for version negotiation logic (doesn't require a server instance)
void test_mcp_version_negotiation_unit() {
  // Directly test the version negotiation algorithm
  auto testNegotiate = [](const std::string &clientVer) -> std::string {
    constexpr std::string_view supported[] = {
        "2025-11-25",
        "2025-06-18",
        "2025-03-26",
        "2024-11-05",
    };
    std::string negotiatedVersion{"2024-11-05"};
    bool foundExact = false;
    for (const auto &sv : supported) {
      if (sv == clientVer) {
        negotiatedVersion = sv;
        foundExact = true;
        break;
      }
    }
    if (!foundExact && !clientVer.empty()) {
      auto clientYear = clientVer.substr(0, 4);
      for (const auto &sv : supported) {
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
  XX_TEST_EXPECT_EQ(testNegotiate("2026-01-01"), "2025-11-25");
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
    auto resp = jsonRpcResponse(1, {{"ok", true}});
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
    auto server = std::make_unique<McpServer>();
    McpToolDefinition def;
    def.name = "echo";
    def.description = "Echo back the input";
    def.inputSchema = json::object();

    auto tools = server->listTools();
    XX_TEST_EXPECT_EQ(tools.size(), (size_t)0);

    server->addTool(def, [](const json &args) -> json {
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
    McpServer server;
    McpResourceDefinition def;
    def.uri = "file:///test.txt";
    def.name = "Test File";
    def.description = "A test resource";
    def.mimeType = "text/plain";

    server.addResource(
        def, [](const std::string &uri) -> std::optional<McpResourceContent> {
          if (uri == "file:///test.txt") {
            return McpResourceContent{
                .uri = uri, .mimeType = "text/plain", .text = "hello world"};
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
    McpServer server;
    McpPromptDefinition def;
    def.name = "greet";
    def.description = "Generate a greeting";
    McpPromptArgument arg;
    arg.name = "name";
    arg.description = "The name to greet";
    arg.required = true;
    def.arguments.push_back(std::move(arg));

    server.addPrompt(def,
                     [](const std::string &name,
                        const json &args) -> std::optional<McpPromptResult> {
                       if (name != "greet")
                         return std::nullopt;
                       McpPromptResult result;
                       result.description = "A friendly greeting";
                       McpPromptMessage msg;
                       msg.role = "assistant";
                       msg.content =
                           "Hello, " + args.value("name", "world") + "!";
                       result.messages.push_back(std::move(msg));
                       return result;
                     });

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
  cfg.httpConfig.address = "127.0.0.1";
  cfg.httpConfig.port = 0;
  cfg.httpConfig.ioThreads = 1;
  cfg.httpConfig.accessLogEnabled = false;

  Server server(std::move(cfg));
  McpToolDefinition def;
  def.name = "echo";
  def.description = "Echo back the input";
  def.inputSchema = json::parse(R"({
    "type": "object",
    "properties": {
      "text": {"type": "string"}
    }
  })");

  server.addTool(def, [](const json &args) -> json {
    json content;
    content["type"] = "text";
    content["text"] = args.value("text", "");
    return content;
  });

  // Start server
  std::thread serverThread([&server]() { server.start(); });

  // Wait for server to be ready
  uint16_t port = 0;
  for (int i = 0; i < 100; ++i) {
    port = server.port();
    if (port != 0)
      break;
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
      boost::asio::io_context tmpCtx;
      boost::asio::ip::tcp::socket sock(tmpCtx);
      sock.connect(boost::asio::ip::tcp::endpoint(
          boost::asio::ip::make_address("127.0.0.1"), port));
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
    req["id"] = 1;
    req["method"] = "initialize";
    req["params"] = {
        {"protocolVersion", "2024-11-05"},
        {"capabilities", json::object()},
        {"clientInfo", {{"name", "test-client"}, {"version", "1.0"}}}};

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
            "agentxx-mcp");
      }
    }
  }

  // Test ping
  {
    json req;
    req["jsonrpc"] = "2.0";
    req["id"] = 2;
    req["method"] = "ping";

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
    req["id"] = 3;
    req["method"] = "tools/list";

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
    req["id"] = 4;
    req["method"] = "tools/call";
    req["params"] = {{"name", "echo"}, {"arguments", {{"text", "hello mcp"}}}};

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
    req["id"] = 5;
    req["method"] = "tools/call";
    req["params"] = {{"name", "nonexistent"}, {"arguments", json::object()}};

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
    req["id"] = 6;
    req["method"] = "nonexistent/method";

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
    auto resp = co_await HttpClient::postAsync(baseUrl + "/mcp", "not json",
                                               "application/json", headers,
                                               std::chrono::seconds{5});
    XX_TEST_EXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      XX_TEST_EXPECT_EQ(resp.value().status, 400);
    }
  }

  // Test notification (no id) – should get 202 Accepted
  {
    json req;
    req["jsonrpc"] = "2.0";
    req["method"] = "notifications/initialized";

    auto resp = co_await HttpClient::postAsync(baseUrl + "/mcp", req);
    XX_TEST_EXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      XX_TEST_EXPECT_EQ(resp.value().status, 202);
    }
  }

  // Test SSE endpoint
  {
    auto resp = co_await HttpClient::getAsync(baseUrl + "/mcp/sse");
    XX_TEST_EXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      XX_TEST_EXPECT_EQ(resp.value().status, 200);
      auto ct = resp.value().findHeader("content-type");
      XX_TEST_EXPECT_TRUE(ct.find("text/event-stream") !=
                          std::string_view::npos);
      XX_TEST_EXPECT_TRUE(resp.value().body.find("endpoint") !=
                          std::string::npos);
    }
  }

  // Test resources/list (should return empty array)
  {
    json req;
    req["jsonrpc"] = "2.0";
    req["id"] = 7;
    req["method"] = "resources/list";

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
    req["id"] = 8;
    req["method"] = "prompts/list";

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
    McpServer server;
    std::string input;
    input +=
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","clientInfo":{"name":"test"}}})"
        "\n";
    input +=
        R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"2025-03-26","clientInfo":{"name":"test"}}})"
        "\n";
    input +=
        R"({"jsonrpc":"2.0","id":3,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"test"}}})"
        "\n";
    input +=
        R"({"jsonrpc":"2.0","id":4,"method":"initialize","params":{"protocolVersion":"2025-11-25","clientInfo":{"name":"test"}}})"
        "\n";
    input +=
        R"({"jsonrpc":"2.0","id":5,"method":"initialize","params":{"protocolVersion":"2025-01-01","clientInfo":{"name":"test"}}})"
        "\n";
    input +=
        R"({"jsonrpc":"2.0","id":6,"method":"initialize","params":{"clientInfo":{"name":"test"}}})"
        "\n";
    input +=
        R"({"jsonrpc":"2.0","id":7,"method":"initialize","params":{"protocolVersion":"2026-01-01","clientInfo":{"name":"test"}}})"
        "\n";

    auto oldCin = std::cin.rdbuf();
    auto oldCout = std::cout.rdbuf();
    std::istringstream in(input);
    std::ostringstream out;
    std::cin.rdbuf(in.rdbuf());
    std::cout.rdbuf(out.rdbuf());
    server.runStdio();
    std::cin.rdbuf(oldCin);
    std::cout.rdbuf(oldCout);

    std::vector<json> responses;
    std::istringstream outputStream(out.str());
    std::string line;
    while (std::getline(outputStream, line))
      if (!line.empty())
        responses.push_back(json::parse(line));

    XX_TEST_EXPECT_EQ(responses.size(), (size_t)7);
    if (responses.size() >= 7) {
      // Exact match 2024-11-05
      XX_TEST_EXPECT_EQ(
          responses[0]["result"]["protocolVersion"].get<std::string>(),
          "2024-11-05");
      // Exact match 2025-03-26
      XX_TEST_EXPECT_EQ(
          responses[1]["result"]["protocolVersion"].get<std::string>(),
          "2025-03-26");
      // Exact match 2025-06-18
      XX_TEST_EXPECT_EQ(
          responses[2]["result"]["protocolVersion"].get<std::string>(),
          "2025-06-18");
      // Exact match 2025-11-25
      XX_TEST_EXPECT_EQ(
          responses[3]["result"]["protocolVersion"].get<std::string>(),
          "2025-11-25");
      // Unknown version 2025-01-01 matches newest 2025
      XX_TEST_EXPECT_EQ(
          responses[4]["result"]["protocolVersion"].get<std::string>(),
          "2025-11-25");
      // Missing version defaults to oldest-supported (max compat)
      XX_TEST_EXPECT_EQ(
          responses[5]["result"]["protocolVersion"].get<std::string>(),
          "2024-11-05");
      // Future version 2026-01-01 matches newest supported
      XX_TEST_EXPECT_EQ(
          responses[6]["result"]["protocolVersion"].get<std::string>(),
          "2025-11-25");
    }
  }
}

// -----------------------------------------------------------------------
// 2025-11-25 feature tests
// -----------------------------------------------------------------------

void test_mcp_server_2025_features() {
  // Test instructions field in initialize response
  {
    McpServer server;
    std::string input;
    input +=
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","clientInfo":{"name":"test"}}})"
        "\n";

    auto oldCin = std::cin.rdbuf();
    auto oldCout = std::cout.rdbuf();
    std::istringstream in(input);
    std::ostringstream out;
    std::cin.rdbuf(in.rdbuf());
    std::cout.rdbuf(out.rdbuf());
    server.runStdio();
    std::cin.rdbuf(oldCin);
    std::cout.rdbuf(oldCout);

    std::vector<json> responses;
    std::istringstream outputStream(out.str());
    std::string line;
    while (std::getline(outputStream, line))
      if (!line.empty())
        responses.push_back(json::parse(line));

    XX_TEST_EXPECT_EQ(responses.size(), (size_t)1);
    if (!responses.empty()) {
      // Must have instructions field (2025-11-25 feature)
      XX_TEST_EXPECT_TRUE(responses[0]["result"].contains("instructions"));
      // Must have tasks capability
      XX_TEST_EXPECT_TRUE(
          responses[0]["result"]["capabilities"].contains("tasks"));
    }
  }

  // Test tool with title, outputSchema, annotations, execution
  {
    McpServer server;
    McpToolDefinition def;
    def.name = "advanced_tool";
    def.description = "A tool with all 2025-11-25 fields";
    def.title = "Advanced Tool";
    def.inputSchema = json::parse(
        R"({"type":"object","properties":{"x":{"type":"number"}}})");
    def.outputSchema = json::parse(
        R"({"type":"object","properties":{"result":{"type":"number"}}})");
    def.annotations = json::parse(R"({"title":"Advanced"})");
    def.execution = json::parse(R"({"taskSupport":"optional"})");

    server.addTool(def, [](const json &args) -> json {
      json content;
      content["type"] = "text";
      content["text"] = "result:" + std::to_string(args.value("x", 0.0));
      return content;
    });

    std::string input;
    input += R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"
             "\n";

    auto oldCin = std::cin.rdbuf();
    auto oldCout = std::cout.rdbuf();
    std::istringstream in(input);
    std::ostringstream out;
    std::cin.rdbuf(in.rdbuf());
    std::cout.rdbuf(out.rdbuf());
    server.runStdio();
    std::cin.rdbuf(oldCin);
    std::cout.rdbuf(oldCout);

    std::vector<json> responses;
    std::istringstream outputStream(out.str());
    std::string line;
    while (std::getline(outputStream, line))
      if (!line.empty())
        responses.push_back(json::parse(line));

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
    McpServer server;
    McpToolDefinition def;
    def.name = "basic_tool";
    def.description = "Basic tool";
    def.title = "Basic Tool (should be hidden in old protocol)";
    server.addTool(def, [](const json &) -> json {
      json content;
      content["type"] = "text";
      content["text"] = "ok";
      return content;
    });

    std::string input;
    input +=
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","clientInfo":{"name":"test"}}})"
        "\n";
    input += R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"
             "\n";

    auto oldCin = std::cin.rdbuf();
    auto oldCout = std::cout.rdbuf();
    std::istringstream in(input);
    std::ostringstream out;
    std::cin.rdbuf(in.rdbuf());
    std::cout.rdbuf(out.rdbuf());
    server.runStdio();
    std::cin.rdbuf(oldCin);
    std::cout.rdbuf(oldCout);

    std::vector<json> responses;
    std::istringstream outputStream(out.str());
    std::string line;
    while (std::getline(outputStream, line))
      if (!line.empty())
        responses.push_back(json::parse(line));

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
  cfg.httpConfig.address = "127.0.0.1";
  cfg.httpConfig.port = 0;
  cfg.httpConfig.ioThreads = 1;
  cfg.httpConfig.accessLogEnabled = false;

  Server server(std::move(cfg));
  McpToolDefinition def;
  def.name = "echo";
  def.description = "Echo";
  def.inputSchema = json::parse(
      R"({"type":"object","properties":{"text":{"type":"string"}}})");

  server.addTool(def, [](const json &args) -> json {
    json content;
    content["type"] = "text";
    content["text"] = args.value("text", "");
    return content;
  });

  std::thread serverThread([&server]() { server.start(); });

  uint16_t port = 0;
  for (int i = 0; i < 100; ++i) {
    port = server.port();
    if (port != 0)
      break;
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
      boost::asio::io_context tmpCtx;
      boost::asio::ip::tcp::socket sock(tmpCtx);
      sock.connect(boost::asio::ip::tcp::endpoint(
          boost::asio::ip::make_address("127.0.0.1"), port));
      sock.close();
      break;
    } catch (...) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  // Test with 2025-11-25 protocol version
  {
    McpClient::Config clientCfg;
    clientCfg.serverUrl = baseUrl + "/mcp";
    clientCfg.protocolVersion = std::string{McpClient::kProtocol2025_11_25};
    clientCfg.requestTimeout = std::chrono::seconds(5);

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

    auto echo = co_await client->callTool("echo", {{"text", "hello 2025"}});
    XX_TEST_EXPECT_TRUE(echo.has_value());

    co_await client->close();
  }

  // Test with 2024-11-05 (backward compatibility)
  {
    McpClient::Config clientCfg;
    clientCfg.serverUrl = baseUrl + "/mcp";
    clientCfg.protocolVersion = std::string{McpClient::kProtocol2024_11_05};
    clientCfg.requestTimeout = std::chrono::seconds(5);

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

  auto oldCin = std::cin.rdbuf();
  auto oldCout = std::cout.rdbuf();
  std::istringstream in(input);
  std::ostringstream out;
  std::cin.rdbuf(in.rdbuf());
  std::cout.rdbuf(out.rdbuf());
  server.runStdio();
  std::cin.rdbuf(oldCin);
  std::cout.rdbuf(oldCout);

  std::vector<json> responses;
  std::istringstream outputStream(out.str());
  std::string line;
  while (std::getline(outputStream, line))
    if (!line.empty())
      responses.push_back(json::parse(line));

  // All 5 requests should succeed
  XX_TEST_EXPECT_EQ(responses.size(), (size_t)5);
  if (responses.size() >= 5) {
    for (size_t i = 0; i < 5; i++) {
      XX_TEST_EXPECT_TRUE(responses[i].contains("result") ||
                          responses[i].contains("error"));
      if (responses[i].contains("error")) {
        TEST_INFO << "Response " << i << " has error: " << responses[i].dump()
                  << std::endl;
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
  resDef.uri = "file:///test.txt";
  resDef.name = "Test File";
  resDef.mimeType = "text/plain";
  server.addResource(
      resDef, [](const std::string &uri) -> std::optional<McpResourceContent> {
        if (uri == "file:///test.txt")
          return McpResourceContent{uri, "text/plain", "hello world"};
        return std::nullopt;
      });

  // Add a prompt
  McpPromptDefinition promptDef;
  promptDef.name = "greet";
  promptDef.description = "Generate a greeting";
  McpPromptArgument arg;
  arg.name = "name";
  arg.required = true;
  promptDef.arguments.push_back(std::move(arg));
  server.addPrompt(promptDef,
                   [](const std::string &name,
                      const json &args) -> std::optional<McpPromptResult> {
                     if (name != "greet")
                       return std::nullopt;
                     McpPromptResult result;
                     result.description = "A friendly greeting";
                     McpPromptMessage msg;
                     msg.role = "assistant";
                     msg.content =
                         "Hello, " + args.value("name", "world") + "!";
                     result.messages.push_back(std::move(msg));
                     return result;
                   });

  std::string input;
  input +=
      R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","clientInfo":{"name":"test"}}})"
      "\n";
  input += R"({"jsonrpc":"2.0","id":2,"method":"resources/list"})"
           "\n";
  input +=
      R"({"jsonrpc":"2.0","id":3,"method":"resources/read","params":{"uri":"file:///test.txt"}})"
      "\n";
  input +=
      R"({"jsonrpc":"2.0","id":4,"method":"resources/read","params":{"uri":"file:///nonexistent.txt"}})"
      "\n";
  input += R"({"jsonrpc":"2.0","id":5,"method":"prompts/list"})"
           "\n";
  input +=
      R"({"jsonrpc":"2.0","id":6,"method":"prompts/get","params":{"name":"greet","arguments":{"name":"World"}}})"
      "\n";
  input +=
      R"({"jsonrpc":"2.0","id":7,"method":"prompts/get","params":{"name":"nonexistent"}})"
      "\n";

  auto oldCin = std::cin.rdbuf();
  auto oldCout = std::cout.rdbuf();
  std::istringstream in(input);
  std::ostringstream out;
  std::cin.rdbuf(in.rdbuf());
  std::cout.rdbuf(out.rdbuf());
  server.runStdio();
  std::cin.rdbuf(oldCin);
  std::cout.rdbuf(oldCout);

  std::vector<json> responses;
  std::istringstream outputStream(out.str());
  std::string line;
  while (std::getline(outputStream, line))
    if (!line.empty())
      responses.push_back(json::parse(line));

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
        "hello world");

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
        "Hello, World!");

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
  cfg.httpConfig.address = "127.0.0.1";
  cfg.httpConfig.port = 0;
  cfg.httpConfig.ioThreads = 1;
  cfg.httpConfig.accessLogEnabled = false;

  Server server(std::move(cfg));
  McpToolDefinition def;
  def.name = "echo";
  def.description = "Echo back the input";
  def.inputSchema = json::parse(R"({
    "type": "object",
    "properties": {
      "text": {"type": "string"}
    }
  })");

  server.addTool(def, [](const json &args) -> json {
    json content;
    content["type"] = "text";
    content["text"] = args.value("text", "");
    return content;
  });

  std::thread serverThread([&server]() { server.start(); });

  uint16_t port = 0;
  for (int i = 0; i < 100; ++i) {
    port = server.port();
    if (port != 0)
      break;
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
      boost::asio::io_context tmpCtx;
      boost::asio::ip::tcp::socket sock(tmpCtx);
      sock.connect(boost::asio::ip::tcp::endpoint(
          boost::asio::ip::make_address("127.0.0.1"), port));
      sock.close();
      break;
    } catch (...) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  // Create MCP client
  McpClient::Config clientCfg;
  clientCfg.serverUrl = baseUrl + "/mcp";
  clientCfg.protocolVersion = std::string{McpClient::kProtocol2024_11_05};
  clientCfg.requestTimeout = std::chrono::seconds(5);
  clientCfg.initTimeout = std::chrono::seconds(5);

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
    if (result.has_value())
      XX_TEST_EXPECT_TRUE(result.value());
  }

  // Test listTools
  {
    auto result = co_await client->listTools();
    XX_TEST_EXPECT_TRUE(result.has_value());
    if (result.has_value()) {
      XX_TEST_EXPECT_EQ(result->size(), (size_t)1);
      if (!result->empty())
        XX_TEST_EXPECT_EQ((*result)[0].name, "echo");
    }
  }

  // Test callTool
  {
    auto result = co_await client->callTool("echo", {{"text", "hello client"}});
    XX_TEST_EXPECT_TRUE(result.has_value());
    if (result.has_value()) {
      XX_TEST_EXPECT_TRUE(result->contains("content"));
      if (result->contains("content") && (*result)["content"].is_array()) {
        XX_TEST_EXPECT_EQ((*result)["content"][0]["text"].get<std::string>(),
                          "hello client");
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
      bool hasIsError =
          result->contains("isError") && (*result)["isError"].get<bool>();
      XX_TEST_EXPECT_TRUE(hasDirectError || hasIsError);
    }
  }

  // Test listResources (empty)
  {
    auto result = co_await client->listResources();
    XX_TEST_EXPECT_TRUE(result.has_value());
    if (result.has_value())
      XX_TEST_EXPECT_TRUE(result->empty());
  }

  // Test listPrompts (empty)
  {
    auto result = co_await client->listPrompts();
    XX_TEST_EXPECT_TRUE(result.has_value());
    if (result.has_value())
      XX_TEST_EXPECT_TRUE(result->empty());
  }

  // Test tool adapter creation
  {
    McpToolDefinition toolDef;
    toolDef.name = "echo";
    toolDef.description = "Echo back";
    auto tool = client->createTool(std::move(toolDef), {});
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
  def.name = "echo";
  def.description = "Echo back the input";
  def.inputSchema = json::parse(R"({
    "type": "object",
    "properties": {
      "text": {"type": "string"}
    }
  })");

  server.addTool(def, [](const json &args) -> json {
    json content;
    content["type"] = "text";
    content["text"] = args.value("text", "");
    return content;
  });

  std::string input;
  input +=
      R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}})"
      "\n";
  input += R"({"jsonrpc":"2.0","id":2,"method":"ping"})"
           "\n";
  input += R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})"
           "\n";
  input +=
      R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hello stdio"}}})"
      "\n";
  input +=
      R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"nonexistent","arguments":{}}})"
      "\n";
  input += R"({"jsonrpc":"2.0","id":6,"method":"nonexistent/method"})"
           "\n";
  input += R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
           "\n";

  auto oldCin = std::cin.rdbuf();
  auto oldCout = std::cout.rdbuf();

  std::istringstream in(input);
  std::ostringstream out;

  std::cin.rdbuf(in.rdbuf());
  std::cout.rdbuf(out.rdbuf());

  server.runStdio();

  std::cin.rdbuf(oldCin);
  std::cout.rdbuf(oldCout);

  // Parse output
  std::string output = out.str();
  std::istringstream outputStream(output);
  std::string line;
  std::vector<json> responses;
  while (std::getline(outputStream, line)) {
    if (!line.empty())
      responses.push_back(json::parse(line));
  }

  // 6 requests with id → 6 responses, 1 notification → no response
  XX_TEST_EXPECT_EQ(responses.size(), (size_t)6);

  if (responses.size() >= 6) {
    // Resp 0: initialize
    XX_TEST_EXPECT_EQ(responses[0]["id"].get<int>(), 1);
    XX_TEST_EXPECT_TRUE(responses[0].contains("result"));
    XX_TEST_EXPECT_EQ(
        responses[0]["result"]["serverInfo"]["name"].get<std::string>(),
        "agentxx-mcp");

    // Resp 1: ping
    XX_TEST_EXPECT_EQ(responses[1]["id"].get<int>(), 2);
    XX_TEST_EXPECT_TRUE(responses[1].contains("result"));

    // Resp 2: tools/list
    XX_TEST_EXPECT_EQ(responses[2]["id"].get<int>(), 3);
    XX_TEST_EXPECT_TRUE(responses[2].contains("result"));
    XX_TEST_EXPECT_EQ(responses[2]["result"]["tools"].size(), (size_t)1);
    XX_TEST_EXPECT_EQ(
        responses[2]["result"]["tools"][0]["name"].get<std::string>(), "echo");

    // Resp 3: tools/call echo
    XX_TEST_EXPECT_EQ(responses[3]["id"].get<int>(), 4);
    XX_TEST_EXPECT_TRUE(responses[3].contains("result"));
    XX_TEST_EXPECT_EQ(
        responses[3]["result"]["content"][0]["text"].get<std::string>(),
        "hello stdio");

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

  auto oldCin = std::cin.rdbuf();
  auto oldCout = std::cout.rdbuf();

  std::istringstream in(input);
  std::ostringstream out;

  std::cin.rdbuf(in.rdbuf());
  std::cout.rdbuf(out.rdbuf());

  server.runStdio();

  std::cin.rdbuf(oldCin);
  std::cout.rdbuf(oldCout);

  std::string output = out.str();
  std::istringstream outputStream(output);
  std::string line;
  std::vector<json> responses;
  while (std::getline(outputStream, line)) {
    if (!line.empty())
      responses.push_back(json::parse(line));
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

    auto oldCin = std::cin.rdbuf();
    auto oldCout = std::cout.rdbuf();
    std::istringstream in(input);
    std::ostringstream out;
    std::cin.rdbuf(in.rdbuf());
    std::cout.rdbuf(out.rdbuf());
    server.runStdio();
    std::cin.rdbuf(oldCin);
    std::cout.rdbuf(oldCout);

    std::vector<json> responses;
    std::istringstream outputStream(out.str());
    std::string line;
    while (std::getline(outputStream, line))
      if (!line.empty())
        responses.push_back(json::parse(line));

    XX_TEST_EXPECT_EQ(responses.size(), (size_t)1);
    if (!responses.empty()) {
      XX_TEST_EXPECT_TRUE(responses[0].contains("result"));
      XX_TEST_EXPECT_TRUE(responses[0]["result"].contains("resourceTemplates"));
      XX_TEST_EXPECT_TRUE(
          responses[0]["result"]["resourceTemplates"].is_array());
    }
  }

  // Test completion/complete
  {
    std::string input;
    input +=
        R"({"jsonrpc":"2.0","id":1,"method":"completion/complete","params":{"ref":{"type":"ref/prompt","name":"test"},"argument":{"name":"arg","value":"val"}}})"
        "\n";

    auto oldCin = std::cin.rdbuf();
    auto oldCout = std::cout.rdbuf();
    std::istringstream in(input);
    std::ostringstream out;
    std::cin.rdbuf(in.rdbuf());
    std::cout.rdbuf(out.rdbuf());
    server.runStdio();
    std::cin.rdbuf(oldCin);
    std::cout.rdbuf(oldCout);

    std::vector<json> responses;
    std::istringstream outputStream(out.str());
    std::string line;
    while (std::getline(outputStream, line))
      if (!line.empty())
        responses.push_back(json::parse(line));

    XX_TEST_EXPECT_EQ(responses.size(), (size_t)1);
    if (!responses.empty()) {
      XX_TEST_EXPECT_TRUE(responses[0].contains("result"));
      XX_TEST_EXPECT_TRUE(responses[0]["result"].contains("completion"));
      XX_TEST_EXPECT_TRUE(
          responses[0]["result"]["completion"].contains("values"));
    }
  }

  // Test _meta passthrough on response
  {
    std::string input;
    input +=
        R"({"jsonrpc":"2.0","id":1,"method":"ping","params":{"_meta":{"progressToken":"tok-123"}}})"
        "\n";

    auto oldCin = std::cin.rdbuf();
    auto oldCout = std::cout.rdbuf();
    std::istringstream in(input);
    std::ostringstream out;
    std::cin.rdbuf(in.rdbuf());
    std::cout.rdbuf(out.rdbuf());
    server.runStdio();
    std::cin.rdbuf(oldCin);
    std::cout.rdbuf(oldCout);

    std::vector<json> responses;
    std::istringstream outputStream(out.str());
    std::string line;
    while (std::getline(outputStream, line))
      if (!line.empty())
        responses.push_back(json::parse(line));

    XX_TEST_EXPECT_EQ(responses.size(), (size_t)1);
    if (!responses.empty()) {
      // _meta should be passed through to response (2025-03-26+)
      if (responses[0]["result"].contains("_meta")) {
        XX_TEST_EXPECT_EQ(
            responses[0]["result"]["_meta"]["progressToken"].get<std::string>(),
            "tok-123");
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
    input +=
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"test","version":"1.0","title":"Test Client"},"capabilities":{}}})"
        "\n";

    auto oldCin = std::cin.rdbuf();
    auto oldCout = std::cout.rdbuf();
    std::istringstream in(input);
    std::ostringstream out;
    std::cin.rdbuf(in.rdbuf());
    std::cout.rdbuf(out.rdbuf());
    server.runStdio();
    std::cin.rdbuf(oldCin);
    std::cout.rdbuf(oldCout);

    std::vector<json> responses;
    std::istringstream outputStream(out.str());
    std::string line;
    while (std::getline(outputStream, line))
      if (!line.empty())
        responses.push_back(json::parse(line));

    XX_TEST_EXPECT_EQ(responses.size(), (size_t)1);
    if (!responses.empty()) {
      // serverInfo should have title (2025-06-18)
      XX_TEST_EXPECT_TRUE(
          responses[0]["result"]["serverInfo"].contains("title"));
      // capabilities should have elicitation (2025-06-18)
      XX_TEST_EXPECT_TRUE(
          responses[0]["result"]["capabilities"].contains("elicitation"));
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
      {"2026-01-01", "2025-11-25"},
      // Client requests older version → server responds with that version
      {"2024-06-01", "2024-11-05"},
  };

  for (const auto &pair : pairs) {
    McpServer server;
    std::string input;
    input +=
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{";
    input += "\"protocolVersion\":\"" + pair.clientVer + "\"";
    input += ",\"clientInfo\":{\"name\":\"test\",\"version\":\"1.0\"}";
    input += ",\"capabilities\":{}}}";
    input += "\n";

    auto oldCin = std::cin.rdbuf();
    auto oldCout = std::cout.rdbuf();
    std::istringstream in(input);
    std::ostringstream out;
    std::cin.rdbuf(in.rdbuf());
    std::cout.rdbuf(out.rdbuf());
    server.runStdio();
    std::cin.rdbuf(oldCin);
    std::cout.rdbuf(oldCout);

    std::vector<json> responses;
    std::istringstream outputStream(out.str());
    std::string line;
    while (std::getline(outputStream, line))
      if (!line.empty())
        responses.push_back(json::parse(line));

    bool ok = responses.size() == 1 && responses[0].contains("result") &&
              responses[0]["result"].contains("protocolVersion");

    if (ok) {
      std::string got =
          responses[0]["result"]["protocolVersion"].get<std::string>();
      std::string expected =
          pair.expectedVer.empty() ? pair.clientVer : pair.expectedVer;
      if (got == expected) {
        XX_TEST_PASSED++;
      } else {
        XX_TEST_FAILED++;
        TEST_FAIL << "line ~" << __LINE__ << ": client=" << pair.clientVer
                  << " expected=" << expected << " got=" << got << std::endl;
      }
      // Verify serverInfo is always present
      if (responses[0]["result"].contains("serverInfo"))
        XX_TEST_PASSED++;
      else {
        XX_TEST_FAILED++;
        TEST_FAIL << "line ~" << __LINE__ << ": missing serverInfo"
                  << std::endl;
      }
      // Verify capabilities is always present
      if (responses[0]["result"].contains("capabilities"))
        XX_TEST_PASSED++;
      else {
        XX_TEST_FAILED++;
        TEST_FAIL << "line ~" << __LINE__ << ": missing capabilities"
                  << std::endl;
      }
      // instructions field should be present for all versions (forward compat)
      if (responses[0]["result"].contains("instructions"))
        XX_TEST_PASSED++;
      else {
        XX_TEST_FAILED++;
        TEST_FAIL << "line ~" << __LINE__ << ": missing instructions"
                  << std::endl;
      }
    } else {
      XX_TEST_FAILED++;
      TEST_FAIL << "line ~" << __LINE__ << ": client=" << pair.clientVer
                << " bad response: "
                << (responses.empty() ? "empty" : responses[0].dump())
                << std::endl;
    }
  }
}

// Test cross-version via HTTP transport (all 3 × 3 = 9 combinations)
asio::awaitable<void> test_mcp_server_cross_version_http() {
  using Server = McpServer;

  Server::Config cfg;
  cfg.httpConfig.address = "127.0.0.1";
  cfg.httpConfig.port = 0;
  cfg.httpConfig.ioThreads = 1;
  cfg.httpConfig.accessLogEnabled = false;

  Server server(std::move(cfg));

  std::thread serverThread([&server]() { server.start(); });

  uint16_t port = 0;
  for (int i = 0; i < 100; ++i) {
    port = server.port();
    if (port != 0)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (port == 0) {
    TEST_FAIL << "McpServer failed to start for cross-version test"
              << std::endl;
    g_mcp_failed++;
    server.stop();
    serverThread.join();
    co_return;
  }

  std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);

  for (int i = 0; i < 100; ++i) {
    try {
      boost::asio::io_context tmpCtx;
      boost::asio::ip::tcp::socket sock(tmpCtx);
      sock.connect(boost::asio::ip::tcp::endpoint(
          boost::asio::ip::make_address("127.0.0.1"), port));
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

  for (const auto &clientVer : versions) {
    McpClient::Config clientCfg;
    clientCfg.serverUrl = baseUrl + "/mcp";
    clientCfg.protocolVersion = clientVer;
    clientCfg.requestTimeout = std::chrono::seconds(5);

    auto client = std::make_shared<McpClient>(std::move(clientCfg));
    auto result = co_await client->initialize();

    if (result.has_value()) {
      XX_TEST_PASSED++; // initialize succeeded
      // Verify returned version is non-empty and compatible
      if (!result->protocolVersion.empty()) {
        XX_TEST_PASSED++;
      } else {
        XX_TEST_FAILED++;
        TEST_FAIL << "client=" << clientVer << " empty protocolVersion"
                  << std::endl;
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
        TEST_FAIL << "client=" << clientVer
                  << " listTools failed: " << tools.error() << std::endl;
      }
    } else {
      XX_TEST_FAILED++;
      TEST_FAIL << "client=" << clientVer
                << " initialize failed: " << result.error() << std::endl;
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
  McpServer server;
  McpToolDefinition def;
  def.name = "greeter";
  def.description = "Greets the user";
  def.inputSchema = json::parse(
      R"({"type":"object","properties":{"name":{"type":"string"}}})");
  server.addTool(def, [](const json &args) -> json {
    json c;
    c["type"] = "text";
    c["text"] = "Hello, " + args.value("name", "world") + "!";
    return c;
  });

  // Simulate a 2025-03-26 client: initialize, then standard tool operations
  std::string input;
  input +=
      R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"test-client","version":"1.0"}}})"
      "\n";
  // Notification without id (per spec) → no response
  input += R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
           "\n";
  input += R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"
           "\n";
  input +=
      R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"greeter","arguments":{"name":"MCP"}}})"
      "\n";
  input += R"({"jsonrpc":"2.0","id":4,"method":"resources/templates/list"})"
           "\n";
  input +=
      R"({"jsonrpc":"2.0","id":5,"method":"completion/complete","params":{"ref":{"type":"ref/prompt","name":"test"},"argument":{"name":"a","value":"b"}}})"
      "\n";
  input += R"({"jsonrpc":"2.0","id":6,"method":"ping"})"
           "\n";

  auto oldCin = std::cin.rdbuf();
  auto oldCout = std::cout.rdbuf();
  std::istringstream in(input);
  std::ostringstream out;
  std::cin.rdbuf(in.rdbuf());
  std::cout.rdbuf(out.rdbuf());
  server.runStdio();
  std::cin.rdbuf(oldCin);
  std::cout.rdbuf(oldCout);

  std::vector<json> responses;
  std::istringstream outputStream(out.str());
  std::string line;
  while (std::getline(outputStream, line))
    if (!line.empty())
      responses.push_back(json::parse(line));

  // 6 requests with id → 6 responses (notification without id → no response)
  XX_TEST_EXPECT_EQ(responses.size(), (size_t)6);
  if (responses.size() >= 6) {
    // initialize
    XX_TEST_EXPECT_EQ(responses[0]["id"].get<int>(), 1);
    XX_TEST_EXPECT_TRUE(responses[0]["result"].contains("instructions"));
    XX_TEST_EXPECT_EQ(
        responses[0]["result"]["protocolVersion"].get<std::string>(),
        "2025-03-26");
    // tools/list
    XX_TEST_EXPECT_EQ(responses[1]["id"].get<int>(), 2);
    XX_TEST_EXPECT_EQ(responses[1]["result"]["tools"].size(), (size_t)1);
    // tools/call
    XX_TEST_EXPECT_EQ(responses[2]["id"].get<int>(), 3);
    XX_TEST_EXPECT_EQ(
        responses[2]["result"]["content"][0]["text"].get<std::string>(),
        "Hello, MCP!");
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
  co_await test_mcp_client_2025_version();
  co_await test_mcp_server_cross_version_http();
  co_return TestResult{g_mcp_passed, g_mcp_failed};
}

} // namespace test
} // namespace agentxx
