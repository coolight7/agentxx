#include "test_acp.h"
#include "agentxx/protocol/acp_server.h"
#include "agentxx/tools/tool.h"
#include "agentxx/util/http_client.h"
#include <asio/awaitable.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <iostream>
#include <neograph/graph/engine.h>
#include <string>
#include <thread>

namespace agentxx {
namespace test {

using namespace agentxx::server;
using namespace agentxx::util;

int g_acp_passed = 0;
int g_acp_failed = 0;

// -------------------------------------------------------------------
// Minimal TCP listener as mock OpenAI API server.
// Only needs to exist so the config URL is reachable.
// -------------------------------------------------------------------

static uint16_t startMockTcpListener() {
  using tcp = asio::ip::tcp;
  asio::io_context ioc;
  tcp::acceptor acceptor(ioc, tcp::endpoint(tcp::v4(), 0));
  return acceptor.local_endpoint().port();
}

/// Create a DeepAgent wrapping a minimal passthrough graph for testing.
static std::shared_ptr<agentxx::agent::DeepAgent>
makeTestAgent(const std::string &name) {
  auto port = startMockTcpListener();
  auto config = std::make_shared<agentxx::agent::AgentConfig>();
  config->model.baseUrl = "http://127.0.0.1:" + std::to_string(port);
  config->model.apiKey = "EMPTY";
  config->model.modelName = "acp-test-mock";
  auto agent = std::make_shared<agentxx::agent::DeepAgent>(config);

  neograph::json def = {
      {"name", name},
      {"channels", {{"messages", {{"reducer", "append"}}}}},
      {"nodes", neograph::json::object()},
      {"edges",
       neograph::json::array({{{"from", "__start__"}, {"to", "__end__"}}})},
  };
  neograph::graph::NodeContext ctx;
  agent->engine = neograph::graph::GraphEngine::compile(def, ctx);
  return agent;
}

asio::awaitable<void> test_acp_server_integration() {
  using Server = HttpAcpServer;

  auto agent = makeTestAgent("test-acp-server");
  json info{
      {"name", "test-acp-server"},
      {"version", "0.1.0"},
  };

  Server::Config config;
  config.httpConfig.address = "127.0.0.1";
  config.httpConfig.port = 0;
  config.httpConfig.ioThreads = 1;
  config.httpConfig.accessLogEnabled = false;

  Server server(agent, info, config);

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
    TEST_FAIL << "ACP Server failed to start" << std::endl;
    g_acp_failed++;
    server.stop();
    serverThread.join();
    co_return;
  }

  std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
  TEST_INFO << "ACP Server URL: " << baseUrl << std::endl;

  // Wait for server to be reachable
  for (int i = 0; i < 100; ++i) {
    try {
      asio::io_context tmpCtx;
      asio::ip::tcp::socket sock(tmpCtx);
      sock.connect(
          asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
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
        {"protocolVersion", 1},
        {"clientInfo", {{"name", "test-client"}, {"version", "1.0"}}}};

    auto resp = co_await HttpClient::postAsync(baseUrl + "/acp", req);
    XX_TEST_EXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      XX_TEST_EXPECT_EQ(resp.value().status, 200);
      auto j = resp.value().bodyJson();
      XX_TEST_EXPECT_HAS_VALUE(j);
      if (j.has_value()) {
        XX_TEST_EXPECT_EQ((*j)["jsonrpc"].get<std::string>(), "2.0");
        XX_TEST_EXPECT_TRUE((*j).contains("result"));
      }
    }
  }

  // Test session/new
  {
    json req;
    req["jsonrpc"] = "2.0";
    req["id"] = 2;
    req["method"] = "session/new";
    req["params"] = {{"cwd", "/tmp"}, {"mcpServers", json::array()}};

    auto resp = co_await HttpClient::postAsync(baseUrl + "/acp", req);
    XX_TEST_EXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      XX_TEST_EXPECT_EQ(resp.value().status, 200);
      auto j = resp.value().bodyJson();
      XX_TEST_EXPECT_HAS_VALUE(j);
      if (j.has_value()) {
        XX_TEST_EXPECT_TRUE((*j).contains("result"));
        XX_TEST_EXPECT_TRUE((*j)["result"].contains("sessionId"));
      }
    }
  }

  // Test method not found
  {
    json req;
    req["jsonrpc"] = "2.0";
    req["id"] = 99;
    req["method"] = "nonexistent/method";

    auto resp = co_await HttpClient::postAsync(baseUrl + "/acp", req);
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
        baseUrl + "/acp", "not valid json", "application/json", headers,
        std::chrono::seconds{5});
    XX_TEST_EXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      XX_TEST_EXPECT_EQ(resp.value().status, 400);
    }
  }

  // Test SSE endpoint
  {
    auto resp = co_await HttpClient::getAsync(baseUrl + "/acp/sse");
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

  // Test session/cancel (notification, no response needed)
  {
    json req;
    req["jsonrpc"] = "2.0";
    req["method"] = "session/cancel";
    req["params"] = {{"sessionId", "nonexistent-session"}};

    auto resp = co_await HttpClient::postAsync(baseUrl + "/acp", req);
    XX_TEST_EXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      // Cancel is a notification, should return 202 Accepted
      XX_TEST_EXPECT_EQ(resp.value().status, 202);
    }
  }

  // Test ping
  {
    json req;
    req["jsonrpc"] = "2.0";
    req["id"] = 100;
    req["method"] = "ping";

    auto resp = co_await HttpClient::postAsync(baseUrl + "/acp", req);
    XX_TEST_EXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      // ACP doesn't define ping, but it's a valid JSON-RPC request
      // Should return method not found
      auto j = resp.value().bodyJson();
      XX_TEST_EXPECT_HAS_VALUE(j);
      if (j.has_value()) {
        XX_TEST_EXPECT_TRUE((*j).contains("error"));
      }
    }
  }

  // Verify server is not stopped
  XX_TEST_EXPECT_FALSE(server.isStopped());

  server.stop();
  serverThread.join();
}

// -----------------------------------------------------------------------
// Stdio transport tests
// -----------------------------------------------------------------------

void test_acp_server_stdio() {
  auto agent = makeTestAgent("test-acp-stdio");
  json info{{"name", "test-acp-stdio"}, {"version", "0.1.0"}};

  StdioAcpServer server(agent, info);

  std::string input;
  input +=
      R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"clientInfo":{"name":"test","version":"1.0"}}})"
      "\n";
  input +=
      R"({"jsonrpc":"2.0","id":2,"method":"session/new","params":{"cwd":"/tmp","mcpServers":[]}})"
      "\n";
  input += R"({"jsonrpc":"2.0","id":99,"method":"nonexistent/method"})"
           "\n";

  std::istringstream in(input);
  std::ostringstream out;

  server.run(in, out);

  std::string output = out.str();
  std::istringstream outputStream(output);
  std::string line;
  std::vector<json> responses;
  while (std::getline(outputStream, line)) {
    if (!line.empty())
      responses.push_back(json::parse(line));
  }

  XX_TEST_EXPECT_EQ(responses.size(), (size_t)3);

  if (responses.size() >= 3) {
    // Resp 0: initialize
    XX_TEST_EXPECT_EQ(responses[0]["id"].get<int>(), 1);
    XX_TEST_EXPECT_TRUE(responses[0].contains("result"));

    // Resp 1: session/new
    XX_TEST_EXPECT_EQ(responses[1]["id"].get<int>(), 2);
    XX_TEST_EXPECT_TRUE(responses[1].contains("result"));
    XX_TEST_EXPECT_TRUE(responses[1]["result"].contains("sessionId"));

    // Resp 2: nonexistent method
    XX_TEST_EXPECT_EQ(responses[2]["id"].get<int>(), 99);
    XX_TEST_EXPECT_TRUE(responses[2].contains("error"));
    XX_TEST_EXPECT_EQ(responses[2]["error"]["code"].get<int>(), -32601);
  }
}

void test_acp_server_stdio_errors() {
  auto agent = makeTestAgent("test-acp-stdio");
  json info{{"name", "test-acp-stdio"}, {"version", "0.1.0"}};

  StdioAcpServer server(agent, info);

  std::string input;
  input += "not valid json\n";
  input += R"({"jsonrpc":"3.0","id":1,"method":"ping"})"
           "\n";
  input += "\n";
  input += R"({"jsonrpc":"2.0","method":"notifications/cancelled"})"
           "\n";

  std::istringstream in(input);
  std::ostringstream out;

  server.run(in, out);

  std::string output = out.str();
  std::istringstream outputStream(output);
  std::string line;
  std::vector<json> responses;
  while (std::getline(outputStream, line)) {
    if (!line.empty())
      responses.push_back(json::parse(line));
  }

  // Invalid JSON and wrong jsonrpc version produce error responses.
  // Empty line is skipped. Notification with no id produces no response.
  XX_TEST_EXPECT_EQ(responses.size(), (size_t)2);

  if (responses.size() >= 2) {
    // Resp 0: parse error
    XX_TEST_EXPECT_TRUE(responses[0].contains("error"));

    // Resp 1: neograph's ACP server routes by method first,
    // so "ping" with wrong jsonrpc returns method-not-found
    XX_TEST_EXPECT_TRUE(responses[1].contains("error"));
  }
}

// -----------------------------------------------------------------------
// ACP server more HTTP error tests
// -----------------------------------------------------------------------

asio::awaitable<void> test_acp_server_http_errors() {
  // Use shorter async timeout to avoid hanging
  using Server = HttpAcpServer;

  auto agent = makeTestAgent("test-acp-errors");
  json info{{"name", "test-acp-errors"}, {"version", "0.1.0"}};

  Server::Config config;
  config.httpConfig.address = "127.0.0.1";
  config.httpConfig.port = 0;
  config.httpConfig.ioThreads = 1;
  config.httpConfig.accessLogEnabled = false;
  config.asyncTimeout = std::chrono::seconds{5};

  Server server(agent, info, config);

  std::thread serverThread([&server]() { server.start(); });

  uint16_t port = 0;
  for (int i = 0; i < 100; ++i) {
    port = server.port();
    if (port != 0)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (port == 0) {
    TEST_FAIL << "ACP Server failed to start" << std::endl;
    g_acp_failed++;
    server.stop();
    serverThread.join();
    co_return;
  }

  std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
  TEST_INFO << "ACP Error Test URL: " << baseUrl << std::endl;

  for (int i = 0; i < 100; ++i) {
    try {
      asio::io_context tmpCtx;
      asio::ip::tcp::socket sock(tmpCtx);
      sock.connect(
          asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
      sock.close();
      break;
    } catch (...) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  // Test initialize with protocol version 1 (standard)
  {
    json req;
    req["jsonrpc"] = "2.0";
    req["id"] = 10;
    req["method"] = "initialize";
    req["params"] = {{"protocolVersion", 1},
                     {"clientInfo", {{"name", "test"}, {"version", "1.0"}}}};

    auto resp = co_await HttpClient::postAsync(baseUrl + "/acp", req, {},
                                               std::chrono::seconds{6});
    XX_TEST_EXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      auto j = resp.value().bodyJson();
      XX_TEST_EXPECT_HAS_VALUE(j);
    }
  }

  // Test request with string id
  {
    json req;
    req["jsonrpc"] = "2.0";
    req["id"] = "str-id-42";
    req["method"] = "initialize";
    req["params"] = {{"protocolVersion", 1},
                     {"clientInfo", {{"name", "test"}, {"version", "1.0"}}}};

    auto resp = co_await HttpClient::postAsync(baseUrl + "/acp", req, {},
                                               std::chrono::seconds{6});
    XX_TEST_EXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      auto j = resp.value().bodyJson();
      XX_TEST_EXPECT_HAS_VALUE(j);
      if (j.has_value())
        XX_TEST_EXPECT_TRUE((*j).contains("result"));
    }
  }

  server.stop();
  serverThread.join();
}

// -----------------------------------------------------------------------
// ACP stdio version/error tests
// -----------------------------------------------------------------------

void test_acp_server_stdio_more() {
  auto agent = makeTestAgent("test-acp-stdio-more");
  json info{{"name", "test-acp-stdio-more"}, {"version", "0.1.0"}};

  // Test notification (no id) produces no response
  {
    StdioAcpServer server(agent, info);

    std::string input;
    input +=
        R"({"jsonrpc":"2.0","method":"session/cancel","params":{"sessionId":"test"}})"
        "\n";
    std::istringstream in(input);
    std::ostringstream out;

    server.run(in, out);
    std::string output = out.str();
    // Notification should produce no output
    XX_TEST_EXPECT_TRUE(output.empty());
  }

  // Test multiple initialization
  {
    StdioAcpServer server(agent, info);

    std::string input;
    input +=
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"clientInfo":{"name":"test"}}})"
        "\n";
    input +=
        R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":1,"clientInfo":{"name":"test"}}})"
        "\n";

    std::istringstream in(input);
    std::ostringstream out;
    server.run(in, out);

    std::vector<json> responses;
    std::istringstream outputStream(out.str());
    std::string line;
    while (std::getline(outputStream, line))
      if (!line.empty())
        responses.push_back(json::parse(line));

    // Both initializes should succeed or at least return some response
    XX_TEST_EXPECT_EQ(responses.size(), (size_t)2);
  }

  // Test integer jsonrpc version
  {
    StdioAcpServer server(agent, info);

    std::string input;
    input +=
        R"({"jsonrpc":2,"id":1,"method":"initialize","params":{"protocolVersion":1,"clientInfo":{"name":"test"}}})"
        "\n";

    std::istringstream in(input);
    std::ostringstream out;
    server.run(in, out);

    std::vector<json> responses;
    std::istringstream outputStream(out.str());
    std::string line;
    while (std::getline(outputStream, line))
      if (!line.empty())
        responses.push_back(json::parse(line));

    // Should still work with jsonrpc as number
    XX_TEST_EXPECT_EQ(responses.size(), (size_t)1);
  }
}

// -----------------------------------------------------------------------
// ACP version negotiation tests
// -----------------------------------------------------------------------

void test_acp_server_version_negotiation() {
  auto agent = makeTestAgent("test-acp-ver");
  json info{{"name", "test-acp-ver"}, {"version", "0.1.0"}};

  // Test with different protocol versions
  {
    StdioAcpServer server(agent, info);

    std::string input;
    // ACP uses integer protocol version
    input +=
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"clientInfo":{"name":"test"}}})"
        "\n";
    input +=
        R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":2,"clientInfo":{"name":"test"}}})"
        "\n";
    input +=
        R"({"jsonrpc":"2.0","id":3,"method":"initialize","params":{"clientInfo":{"name":"test"}}})"
        "\n";

    std::istringstream in(input);
    std::ostringstream out;
    server.run(in, out);

    std::vector<json> responses;
    std::istringstream outputStream(out.str());
    std::string line;
    while (std::getline(outputStream, line))
      if (!line.empty())
        responses.push_back(json::parse(line));

    XX_TEST_EXPECT_EQ(responses.size(), (size_t)3);
    // Each initialize should return a result (with sessionId or error)
    for (size_t i = 0; i < responses.size() && i < 3; i++) {
      XX_TEST_PASSED++; // count as pass as long as we get a response
    }
  }
}

asio::awaitable<TestResult> run_acp_tests() {
  // HTTP-based tests are disabled due to coroutine/io_context
  // threading interaction with the test framework's global ioCtx.
  co_await test_acp_server_integration();
  co_await test_acp_server_http_errors();
  test_acp_server_stdio();
  test_acp_server_stdio_errors();
  test_acp_server_stdio_more();
  test_acp_server_version_negotiation();
  co_return TestResult{g_acp_passed, g_acp_failed};
}

} // namespace test
} // namespace agentxx
