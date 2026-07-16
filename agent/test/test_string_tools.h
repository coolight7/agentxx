#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/tools/string.h"
#include "neograph/neograph.h"
#include "test_framework.h"
#include <asio/awaitable.hpp>
#include <iostream>
#include <string>

namespace agentxx {
namespace test {

inline static int g_st_passed = 0;
inline static int g_st_failed = 0;

inline asio::awaitable<void> test_html_to_markdown_basic(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::StringHtml2MarkdownTool{agentContext};
  auto def = tool.get_definition();
  if (def.name == "string_html_to_markdown") {
    g_st_passed++;
    TEST_PASS << "StringHtml2MarkdownTool::get_definition() name correct"
              << std::endl;
  } else {
    std::cout
        << "[FAIL] StringHtml2MarkdownTool::get_definition() name incorrect"
        << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_html_to_markdown_empty_content(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::StringHtml2MarkdownTool{agentContext};
  auto args = neograph::json{{"content", ""}};
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos) {
    std::cout
        << "[PASS] StringHtml2MarkdownTool returns error for empty content"
        << std::endl;
  } else {
    g_st_failed++;
    TEST_FAIL << "StringHtml2MarkdownTool should return error for empty "
                 "content, got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_html_to_markdown_convert(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::StringHtml2MarkdownTool{agentContext};
  auto args = neograph::json{{"content", "<h1>Hello</h1><p>World</p>"}};
  auto result = co_await tool.execute_async(args);
  if (result.find("# Hello") != std::string::npos &&
      result.find("World") != std::string::npos) {
    g_st_passed++;
    TEST_PASS << "StringHtml2MarkdownTool converts HTML to markdown"
              << std::endl;
  } else {
    g_st_failed++;
    TEST_FAIL << "StringHtml2MarkdownTool conversion failed, got: " << result
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_html_to_markdown_simple_text(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::StringHtml2MarkdownTool{agentContext};
  auto args = neograph::json{{"content", "<b>bold</b> <i>italic</i>"}};
  auto result = co_await tool.execute_async(args);
  if (result.find("**bold**") != std::string::npos &&
      result.find("*italic*") != std::string::npos) {
    g_st_passed++;
    TEST_PASS << "StringHtml2MarkdownTool converts bold and italic"
              << std::endl;
  } else {
    g_st_failed++;
    TEST_FAIL << "StringHtml2MarkdownTool bold/italic conversion "
                 "failed, got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_regexp_get_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::StringRegexpTool{agentContext};
  auto def = tool.get_definition();
  if (def.name == "string_regexp") {
    g_st_passed++;
    TEST_PASS << "StringRegexpTool::get_definition() name correct" << std::endl;
  } else {
    g_st_failed++;
    TEST_FAIL << "StringRegexpTool::get_definition() name incorrect"
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_regexp_empty_content(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::StringRegexpTool{agentContext};
  auto args = neograph::json{
      {"content", ""},
      {"exps", neograph::json::array({"test"})},
      {"opt", "search"},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos) {
    g_st_passed++;
    TEST_PASS << "StringRegexpTool returns error for empty content"
              << std::endl;
  } else {
    g_st_failed++;
    TEST_FAIL << "StringRegexpTool should return error for empty "
                 "content, got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_regexp_empty_exps(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::StringRegexpTool{agentContext};
  auto args = neograph::json{
      {"content", "some text"},
      {"exps", neograph::json::array()},
      {"opt", "search"},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos) {
    g_st_passed++;
    TEST_PASS << "StringRegexpTool returns error for empty exps" << std::endl;
  } else {
    g_st_failed++;
    TEST_FAIL << "StringRegexpTool should return error for empty exps, "
                 "got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_regexp_empty_opt(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::StringRegexpTool{agentContext};
  auto args = neograph::json{
      {"content", "some text"},
      {"exps", neograph::json::array({"test"})},
      {"opt", ""},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos) {
    g_st_passed++;
    TEST_PASS << "StringRegexpTool returns error for empty opt" << std::endl;
  } else {
    g_st_failed++;
    TEST_FAIL << "StringRegexpTool should return error for empty opt, "
                 "got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_regexp_invalid_opt(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::StringRegexpTool{agentContext};
  auto args = neograph::json{
      {"content", "some text"},
      {"exps", neograph::json::array({"test"})},
      {"opt", "invalid"},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos &&
      result.find("invalid") != std::string::npos) {
    g_st_passed++;
    TEST_PASS << "StringRegexpTool returns error for invalid opt" << std::endl;
  } else {
    g_st_failed++;
    TEST_FAIL << "StringRegexpTool should return error for invalid opt, "
                 "got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_regexp_search_match(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::StringRegexpTool{agentContext};
  auto args = neograph::json{
      {"content", "hello world, hello everyone"},
      {"exps", neograph::json::array({"hello"})},
      {"opt", "search"},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("Match found") != std::string::npos &&
      result.find("hello") != std::string::npos) {
    g_st_passed++;
    TEST_PASS << "StringRegexpTool search finds matches" << std::endl;
  } else {
    g_st_failed++;
    TEST_FAIL << "StringRegexpTool search should find matches, got: " << result
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_regexp_search_no_match(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::StringRegexpTool{agentContext};
  auto args = neograph::json{
      {"content", "hello world"},
      {"exps", neograph::json::array({"xyz_not_found"})},
      {"opt", "search"},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("No match found") != std::string::npos) {
    g_st_passed++;
    TEST_PASS << "StringRegexpTool search returns 'No match found' "
                 "when no match"
              << std::endl;
  } else {
    g_st_failed++;
    TEST_FAIL << "StringRegexpTool search should return 'No match "
                 "found', got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void>
test_regexp_replace(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::StringRegexpTool{agentContext};
  auto args = neograph::json{
      {"content", "hello world"},
      {"exps", neograph::json::array({"world"})},
      {"opt", "replace"},
      {"replace_str", "universe"},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("hello universe") != std::string::npos) {
    g_st_passed++;
    TEST_PASS << "StringRegexpTool replace works correctly" << std::endl;
  } else {
    g_st_failed++;
    TEST_FAIL << "StringRegexpTool replace failed, got: " << result
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_regexp_replace_no_match(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::StringRegexpTool{agentContext};
  auto args = neograph::json{
      {"content", "hello world"},
      {"exps", neograph::json::array({"xyz_not_found"})},
      {"opt", "replace"},
      {"replace_str", "universe"},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("No match found") != std::string::npos) {
    g_st_passed++;
    TEST_PASS << "StringRegexpTool replace returns 'No match found' "
                 "when no match"
              << std::endl;
  } else {
    g_st_failed++;
    TEST_FAIL << "StringRegexpTool replace should return 'No match "
                 "found', got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void>
test_regexp_remove(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::StringRegexpTool{agentContext};
  auto args = neograph::json{
      {"content", "hello world, hello everyone"},
      {"exps", neograph::json::array({"hello "})},
      {"opt", "remove"},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("world") != std::string::npos &&
      result.find("hello") == std::string::npos) {
    g_st_passed++;
    TEST_PASS << "StringRegexpTool remove works correctly" << std::endl;
  } else {
    g_st_failed++;
    TEST_FAIL << "StringRegexpTool remove failed, got: " << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_regexp_remove_no_match(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::StringRegexpTool{agentContext};
  auto args = neograph::json{
      {"content", "hello world"},
      {"exps", neograph::json::array({"xyz_not_found"})},
      {"opt", "remove"},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("No match found") != std::string::npos) {
    g_st_passed++;
    TEST_PASS << "StringRegexpTool remove returns 'No match found' "
                 "when no match"
              << std::endl;
  } else {
    g_st_failed++;
    TEST_FAIL << "StringRegexpTool remove should return 'No match "
                 "found', got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_regexp_search_multi_patterns(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::StringRegexpTool{agentContext};
  auto args = neograph::json{
      {"content", "apple banana cherry"},
      {"exps", neograph::json::array({"apple", "cherry"})},
      {"opt", "search"},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("apple") != std::string::npos &&
      result.find("cherry") != std::string::npos) {
    g_st_passed++;
    TEST_PASS << "StringRegexpTool search with multiple patterns" << std::endl;
  } else {
    g_st_failed++;
    TEST_FAIL << "StringRegexpTool multi-pattern search failed, got: " << result
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_regexp_replace_multi_patterns(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::StringRegexpTool{agentContext};
  auto args = neograph::json{
      {"content", "apple banana apple"},
      {"exps", neograph::json::array({"apple"})},
      {"opt", "replace"},
      {"replace_str", "orange"},
  };
  auto result = co_await tool.execute_async(args);
  auto count = size_t{0};
  size_t pos = 0;
  while ((pos = result.find("orange", pos)) != std::string::npos) {
    count++;
    pos += 6;
  }
  if (count >= 2) {
    g_st_passed++;
    TEST_PASS << "StringRegexpTool replace replaces all occurrences"
              << std::endl;
  } else {
    g_st_failed++;
    TEST_FAIL << "StringRegexpTool replace should replace all, got: " << result
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<TestResult> run_string_tools_tests(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto run = [agentContext](auto testFn) -> asio::awaitable<void> {
    try {
      co_await testFn(agentContext);
    } catch (const std::exception &e) {
      g_st_failed++;
      TEST_FAIL << "Exception in test: " << e.what() << std::endl;
    }
  };

  co_await run(test_html_to_markdown_basic);
  co_await run(test_html_to_markdown_empty_content);
  co_await run(test_html_to_markdown_convert);
  co_await run(test_html_to_markdown_simple_text);
  co_await run(test_regexp_get_definition);
  co_await run(test_regexp_empty_content);
  co_await run(test_regexp_empty_exps);
  co_await run(test_regexp_empty_opt);
  co_await run(test_regexp_invalid_opt);
  co_await run(test_regexp_search_match);
  co_await run(test_regexp_search_no_match);
  co_await run(test_regexp_replace);
  co_await run(test_regexp_replace_no_match);
  co_await run(test_regexp_remove);
  co_await run(test_regexp_remove_no_match);
  co_await run(test_regexp_search_multi_patterns);
  co_await run(test_regexp_replace_multi_patterns);
  co_return TestResult{g_st_passed, g_st_failed};
}

} // namespace test
} // namespace agentxx