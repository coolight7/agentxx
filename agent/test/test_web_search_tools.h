#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/tools/web_search.h"
#include "neograph/neograph.h"
#include "test_framework.h"
#include <asio/awaitable.hpp>
#include <iostream>
#include <string>

namespace agentxx {
namespace test {

inline static int g_ws_passed = 0;
inline static int g_ws_failed = 0;

inline asio::awaitable<void> test_web_search_get_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::WebSearchTool{"https://example.com/search?q={}",
                                            false, agentContext};
  auto def = tool.get_definition();
  if (def.name == "web_search") {
    g_ws_passed++;
    TEST_PASS << "WebSearchTool::get_definition() name correct" << std::endl;
  } else {
    g_ws_failed++;
    TEST_FAIL << "WebSearchTool::get_definition() name incorrect" << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_web_search_empty_query(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::WebSearchTool{"https://example.com/search?q={}",
                                            false, agentContext};
  auto args = neograph::json{{"query", ""}};
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos) {
    g_ws_passed++;
    TEST_PASS << "WebSearchTool returns error for empty query" << std::endl;
  } else {
    std::cout
        << "[FAIL] WebSearchTool should return error for empty query, got: "
        << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_web_search_definition_has_required_query(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::WebSearchTool{"https://example.com/search?q={}",
                                            false, agentContext};
  auto def = tool.get_definition();
  auto &params = def.parameters;
  if (params.is_object() && params.contains("required") &&
      params["required"].is_array()) {
    auto required = params["required"];
    bool hasQuery = false;
    for (const auto &item : required) {
      if (item.is_string() && item.get<std::string>() == "query") {
        hasQuery = true;
        break;
      }
    }
    if (hasQuery) {
      std::cout
          << "[PASS] WebSearchTool definition has 'query' as required param"
          << std::endl;
    } else {
      g_ws_failed++;
      TEST_FAIL << "WebSearchTool definition missing 'query' in required"
                << std::endl;
    }
  } else {
    g_ws_failed++;
    TEST_FAIL << "WebSearchTool definition has no required params" << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_web_fetch_url_get_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::WebFetchUrlTool{agentContext};
  auto def = tool.get_definition();
  if (def.name == "web_fetch_url") {
    g_ws_passed++;
    TEST_PASS << "WebFetchUrlTool::get_definition() name correct" << std::endl;
  } else {
    g_ws_failed++;
    TEST_FAIL << "WebFetchUrlTool::get_definition() name incorrect"
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_web_fetch_url_empty_url(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::WebFetchUrlTool{agentContext};
  auto args = neograph::json{{"url", ""}};
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos) {
    g_ws_passed++;
    TEST_PASS << "WebFetchUrlTool returns error for empty url" << std::endl;
  } else {
    std::cout
        << "[FAIL] WebFetchUrlTool should return error for empty url, got: "
        << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_web_fetch_url_default_timeout(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::WebFetchUrlTool{agentContext};
  auto def = tool.get_definition();
  auto &params = def.parameters;
  if (params.is_object() && params.contains("properties") &&
      params["properties"].is_object()) {
    auto props = params["properties"];
    if (props.contains("timeout") && props["timeout"].is_object()) {
      g_ws_passed++;
      TEST_PASS << "WebFetchUrlTool definition has timeout parameter"
                << std::endl;
    } else {
      g_ws_failed++;
      TEST_FAIL << "WebFetchUrlTool definition missing timeout parameter"
                << std::endl;
    }
  }
  co_return;
}

inline asio::awaitable<void> test_web_fetch_url_markdown_get_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::WebFetchUrlMarkdownTool{agentContext};
  auto def = tool.get_definition();
  if (def.name == "web_fetch_url_markdown") {
    g_ws_passed++;
    TEST_PASS << "WebFetchUrlMarkdownTool::get_definition() name correct"
              << std::endl;
  } else {
    std::cout
        << "[FAIL] WebFetchUrlMarkdownTool::get_definition() name incorrect"
        << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_web_fetch_url_markdown_empty_url(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::WebFetchUrlMarkdownTool{agentContext};
  auto args = neograph::json{{"url", ""}};
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos) {
    g_ws_passed++;
    TEST_PASS << "WebFetchUrlMarkdownTool returns error for empty url"
              << std::endl;
  } else {
    g_ws_failed++;
    TEST_FAIL << "WebFetchUrlMarkdownTool should return error for empty "
                 "url, got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_web_fetch_url_markdown_description(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::WebFetchUrlMarkdownTool{agentContext};
  auto def = tool.get_definition();
  if (def.description.find("markdown") != std::string::npos ||
      def.description.find("Markdown") != std::string::npos) {
    g_ws_passed++;
    TEST_PASS << "WebFetchUrlMarkdownTool description mentions markdown"
              << std::endl;
  } else {
    g_ws_failed++;
    TEST_FAIL << "WebFetchUrlMarkdownTool description should mention "
                 "markdown"
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_web_search_convert_html2markdown(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::WebSearchTool{"https://example.com/search?q={}",
                                            true, agentContext};
  auto def = tool.get_definition();
  if (def.name == "web_search") {
    g_ws_passed++;
    TEST_PASS << "WebSearchTool with convertHtml2markdown=true created "
                 "successfully"
              << std::endl;
  } else {
    g_ws_failed++;
    TEST_FAIL << "WebSearchTool with convertHtml2markdown=true failed"
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<TestResult> run_web_search_tools_tests(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {

  auto run = [agentContext](auto testFn) -> asio::awaitable<void> {
    try {
      co_await testFn(agentContext);
    } catch (const std::exception &e) {
      g_ws_failed++;
      TEST_FAIL << "Exception in test: " << e.what() << std::endl;
    }
  };

  co_await run(test_web_search_get_definition);
  co_await run(test_web_search_empty_query);
  co_await run(test_web_search_definition_has_required_query);
  co_await run(test_web_search_convert_html2markdown);
  co_await run(test_web_fetch_url_get_definition);
  co_await run(test_web_fetch_url_empty_url);
  co_await run(test_web_fetch_url_default_timeout);
  co_await run(test_web_fetch_url_markdown_get_definition);
  co_await run(test_web_fetch_url_markdown_empty_url);
  co_await run(test_web_fetch_url_markdown_description);
  co_return TestResult{g_ws_passed, g_ws_failed};
}

} // namespace test
} // namespace agentxx