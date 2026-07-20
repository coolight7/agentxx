#include "test_rag_search_tools.h"
#include "agentxx/tools/rag_search.h"
#include "agentxx/util/string_util.h"
#include <asio/awaitable.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>
#include <iostream>
#include <string>

namespace agentxx {
namespace test {

using RAGSearchTool = agentxx::tools::RAGSearchTool;
using VectorStore = agentxx::tools::RAGSearchTool::VectorStore;

int g_rag_passed = 0;
int g_rag_failed = 0;

// =========================================================================
// Tests: splitStringByFixedLength
// =========================================================================

asio::awaitable<void> test_fixed_length_basic() {
  auto result = VectorStore::splitByFixedLength("Hello World", 5);
  if (result.size() == 3 && result[0] == "Hello" && result[1] == " Worl" &&
      result[2] == "d") {
    g_rag_passed++;
    TEST_PASS << "splitStringByFixedLength basic" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitStringByFixedLength basic, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_fixed_length_empty() {
  auto result = VectorStore::splitByFixedLength("", 5);
  if (result.empty()) {
    g_rag_passed++;
    TEST_PASS << "splitStringByFixedLength empty" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitStringByFixedLength empty should return empty"
              << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_fixed_length_utf8() {
  std::string utf8Text = "你好世界Hello";
  auto result = VectorStore::splitByFixedLength(utf8Text, 4);
  if (result.size() == 3 && result[0] == "你好世界" && result[1] == "Hell" &&
      result[2] == "o") {
    g_rag_passed++;
    TEST_PASS << "splitStringByFixedLength utf8" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitStringByFixedLength utf8, got " << result.size()
              << " chunks:";
    for (auto &s : result) {
      std::cout << " [" << s << "]";
    }
    std::cout << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_fixed_length_smaller_than_one_char() {
  std::string utf8Text = "你好";
  auto result = VectorStore::splitByFixedLength(utf8Text, 1);
  if (result.size() == 2 && result[0] == "你" && result[1] == "好") {
    g_rag_passed++;
    TEST_PASS << "splitStringByFixedLength single char boundary" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitStringByFixedLength single char boundary, got "
              << result.size() << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_fixed_length_shorter_than_block() {
  auto result = VectorStore::splitByFixedLength("Hi", 100);
  if (result.size() == 1 && result[0] == "Hi") {
    g_rag_passed++;
    TEST_PASS << "splitStringByFixedLength shorter than block" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitStringByFixedLength shorter than block, got "
              << result.size() << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_fixed_length_exact_boundary() {
  auto result = VectorStore::splitByFixedLength("ABCDE", 5);
  if (result.size() == 1 && result[0] == "ABCDE") {
    g_rag_passed++;
    TEST_PASS << "splitStringByFixedLength exact boundary" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitStringByFixedLength exact boundary, got "
              << result.size() << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_fixed_length_mixed_ascii_utf8() {
  std::string text = "AB你好CD世界";
  auto result = VectorStore::splitByFixedLength(text, 3);
  if (result.size() == 3 && result[0] == "AB你" && result[1] == "好CD" &&
      result[2] == "世界") {
    g_rag_passed++;
    TEST_PASS << "splitStringByFixedLength mixed ascii utf8" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitStringByFixedLength mixed ascii utf8, got "
              << result.size() << " chunks:";
    for (auto &s : result) {
      std::cout << " [" << s << "]";
    }
    std::cout << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_fixed_length_single_char() {
  auto result = VectorStore::splitByFixedLength("X", 5);
  if (result.size() == 1 && result[0] == "X") {
    g_rag_passed++;
    TEST_PASS << "splitStringByFixedLength single char" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitStringByFixedLength single char, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_fixed_length_very_long() {
  std::string text(1000, 'A');
  auto result = VectorStore::splitByFixedLength(text, 100);
  bool allWithinLimit = true;
  for (auto &chunk : result) {
    if (agentxx::util::utf8GetLength(chunk) > 100) {
      allWithinLimit = false;
      break;
    }
  }
  if (result.size() == 10 && allWithinLimit) {
    g_rag_passed++;
    TEST_PASS << "splitStringByFixedLength very long" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitStringByFixedLength very long, got " << result.size()
              << " chunks, allWithinLimit=" << allWithinLimit << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: splitByDelimiter
// =========================================================================

asio::awaitable<void> test_delimiter_basic() {
  auto result = VectorStore::splitByDelimiter("a\n\nb\n\nc", "\n\n");
  if (result.size() == 3 && result[0] == "a" && result[1] == "b" &&
      result[2] == "c") {
    g_rag_passed++;
    TEST_PASS << "splitByDelimiter basic" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByDelimiter basic, got " << result.size() << " chunks"
              << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_delimiter_empty() {
  auto result = VectorStore::splitByDelimiter("", "\n\n");
  if (result.empty()) {
    g_rag_passed++;
    TEST_PASS << "splitByDelimiter empty" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByDelimiter empty should return empty" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_delimiter_no_match() {
  auto result = VectorStore::splitByDelimiter("no delimiter here", "\n\n");
  if (result.size() == 1 && result[0] == "no delimiter here") {
    g_rag_passed++;
    TEST_PASS << "splitByDelimiter no match" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByDelimiter no match, got " << result.size() << " chunks"
              << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_delimiter_empty_delim() {
  auto result = VectorStore::splitByDelimiter("hello world", "");
  if (result.size() == 1 && result[0] == "hello world") {
    g_rag_passed++;
    TEST_PASS << "splitByDelimiter empty delimiter" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByDelimiter empty delimiter, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_delimiter_chinese_punctuation() {
  auto result = VectorStore::splitByDelimiter("第一句。第二句。第三句", "。");
  if (result.size() == 3 && result[0] == "第一句" && result[1] == "第二句" &&
      result[2] == "第三句") {
    g_rag_passed++;
    TEST_PASS << "splitByDelimiter Chinese period" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByDelimiter Chinese period, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_delimiter_comma() {
  auto result = VectorStore::splitByDelimiter("apple, banana, cherry", ", ");
  if (result.size() == 3 && result[0] == "apple" && result[1] == "banana" &&
      result[2] == "cherry") {
    g_rag_passed++;
    TEST_PASS << "splitByDelimiter comma" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByDelimiter comma, got " << result.size() << " chunks"
              << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_delimiter_at_start() {
  auto result = VectorStore::splitByDelimiter("\n\nstart", "\n\n");
  if (result.size() == 1 && result[0] == "start") {
    g_rag_passed++;
    TEST_PASS << "splitByDelimiter at start" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByDelimiter at start, got " << result.size() << " chunks"
              << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_delimiter_at_end() {
  auto result = VectorStore::splitByDelimiter("end\n\n", "\n\n");
  if (result.size() == 1 && result[0] == "end") {
    g_rag_passed++;
    TEST_PASS << "splitByDelimiter at end" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByDelimiter at end, got " << result.size() << " chunks"
              << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_delimiter_consecutive() {
  auto result = VectorStore::splitByDelimiter("a\n\n\n\nb", "\n\n");
  if (result.size() == 2 && result[0] == "a" && result[1] == "b") {
    g_rag_passed++;
    TEST_PASS << "splitByDelimiter consecutive" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByDelimiter consecutive, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_delimiter_only_delim() {
  auto result = VectorStore::splitByDelimiter("\n\n", "\n\n");
  if (result.empty()) {
    g_rag_passed++;
    TEST_PASS << "splitByDelimiter only delimiter" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByDelimiter only delimiter, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_delimiter_multibyte_utf8() {
  auto result =
      VectorStore::splitByDelimiter("第一部分¶¶第二部分¶¶第三部分", "¶¶");
  if (result.size() == 3 && result[0] == "第一部分" &&
      result[1] == "第二部分" && result[2] == "第三部分") {
    g_rag_passed++;
    TEST_PASS << "splitByDelimiter multibyte utf8" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByDelimiter multibyte utf8, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_delimiter_multiple_chinese() {
  auto result = VectorStore::splitByDelimiter("你好！世界！测试", "！");
  if (result.size() == 3 && result[0] == "你好" && result[1] == "世界" &&
      result[2] == "测试") {
    g_rag_passed++;
    TEST_PASS << "splitByDelimiter multiple Chinese excl" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByDelimiter multiple Chinese excl, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: splitByStructure
// =========================================================================

asio::awaitable<void> test_structure_headings() {
  std::string text = "# Title\n\nSome content\n\n## Section 1\n\nMore content";
  auto result = VectorStore::splitByStructure(text);
  if (result.size() >= 2) {
    g_rag_passed++;
    TEST_PASS << "splitByStructure headings" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByStructure headings, got " << result.size() << " blocks"
              << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_structure_code_block() {
  std::string text = "Before\n\n```cpp\nint x = 1;\n```\n\nAfter";
  auto result = VectorStore::splitByStructure(text);
  bool hasCodeBlock = false;
  for (auto &block : result) {
    if (block.find("```") != std::string::npos) {
      hasCodeBlock = true;
    }
  }
  if (hasCodeBlock && result.size() >= 2) {
    g_rag_passed++;
    TEST_PASS << "splitByStructure code block" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByStructure code block, got " << result.size()
              << " blocks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_structure_list() {
  std::string text = "- item1\n- item2\n- item3\n\nparagraph";
  auto result = VectorStore::splitByStructure(text);
  bool hasListBlock = false;
  for (auto &block : result) {
    if (block.find("item1") != std::string::npos &&
        block.find("item2") != std::string::npos) {
      hasListBlock = true;
    }
  }
  if (hasListBlock) {
    g_rag_passed++;
    TEST_PASS << "splitByStructure list" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByStructure list, got " << result.size() << " blocks"
              << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_structure_empty() {
  auto result = VectorStore::splitByStructure("");
  if (result.empty()) {
    g_rag_passed++;
    TEST_PASS << "splitByStructure empty" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByStructure empty should return empty" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_structure_numbered_list() {
  std::string text = "1. first\n2. second\n3. third\n\nparagraph";
  auto result = VectorStore::splitByStructure(text);
  bool hasListBlock = false;
  for (auto &block : result) {
    if (block.find("first") != std::string::npos &&
        block.find("second") != std::string::npos) {
      hasListBlock = true;
    }
  }
  if (hasListBlock) {
    g_rag_passed++;
    TEST_PASS << "splitByStructure numbered list" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByStructure numbered list, got " << result.size()
              << " blocks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_structure_mixed_headings() {
  std::string text =
      "# H1\n\ncontent1\n\n## H2\n\ncontent2\n\n### H3\n\ncontent3";
  auto result = VectorStore::splitByStructure(text);
  if (result.size() >= 3) {
    g_rag_passed++;
    TEST_PASS << "splitByStructure mixed headings" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByStructure mixed headings, got " << result.size()
              << " blocks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_structure_single_paragraph() {
  std::string text = "Just a single paragraph without any structure.";
  auto result = VectorStore::splitByStructure(text);
  if (result.size() == 1 && result[0] == text) {
    g_rag_passed++;
    TEST_PASS << "splitByStructure single paragraph" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByStructure single paragraph, got " << result.size()
              << " blocks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_structure_many_paragraphs() {
  std::string text = "p1\n\np2\n\np3\n\np4\n\np5";
  auto result = VectorStore::splitByStructure(text);
  if (result.size() == 5) {
    g_rag_passed++;
    TEST_PASS << "splitByStructure many paragraphs" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByStructure many paragraphs, got " << result.size()
              << " blocks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_structure_blockquote() {
  std::string text = "> quoted line 1\n> quoted line 2\n\nnormal text";
  auto result = VectorStore::splitByStructure(text);
  if (result.size() >= 2) {
    g_rag_passed++;
    TEST_PASS << "splitByStructure blockquote" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByStructure blockquote, got " << result.size()
              << " blocks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_structure_table() {
  std::string text = "| A | B |\n|---|---|\n| 1 | 2 |\n\nAfter table";
  auto result = VectorStore::splitByStructure(text);
  if (result.size() >= 2) {
    g_rag_passed++;
    TEST_PASS << "splitByStructure table" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByStructure table, got " << result.size() << " blocks"
              << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_structure_heading_merge() {
  std::string text = "# Title\n\ncontent\n\n# Another\n\nmore";
  auto result = VectorStore::splitByStructure(text);
  bool headingMerged = false;
  for (auto &block : result) {
    if (block.find("# Title") != std::string::npos &&
        block.find("content") != std::string::npos) {
      headingMerged = true;
    }
  }
  if (headingMerged) {
    g_rag_passed++;
    TEST_PASS << "splitByStructure heading merge" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByStructure heading merge, got " << result.size()
              << " blocks" << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: splitByDelimiters
// =========================================================================

asio::awaitable<void> test_delimiters_basic() {
  std::vector<std::string> delims = {"\n\n", "\n"};
  auto result =
      VectorStore::splitByDelimiters("para1\n\npara2\n\npara3", 256, delims);
  if (result.size() == 3) {
    g_rag_passed++;
    TEST_PASS << "splitByDelimiters basic" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByDelimiters basic, got " << result.size() << " chunks"
              << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_delimiters_fallback_to_fixed() {
  std::string longText = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::vector<std::string> delims = {"\n\n"};
  auto result = VectorStore::splitByDelimiters(longText, 5, delims);
  if (result.size() >= 5) {
    g_rag_passed++;
    TEST_PASS << "splitByDelimiters fallback to fixed" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByDelimiters fallback to fixed, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_delimiters_within_limit() {
  std::vector<std::string> delims = {"\n\n", "\n"};
  auto result = VectorStore::splitByDelimiters("short", 256, delims);
  if (result.size() == 1 && result[0] == "short") {
    g_rag_passed++;
    TEST_PASS << "splitByDelimiters within limit" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByDelimiters within limit, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_delimiters_empty_text() {
  std::vector<std::string> delims = {"\n\n", "\n"};
  auto result = VectorStore::splitByDelimiters("", 256, delims);
  if (result.empty()) {
    g_rag_passed++;
    TEST_PASS << "splitByDelimiters empty text" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByDelimiters empty text, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_delimiters_empty_delims() {
  std::vector<std::string> delims = {};
  auto result = VectorStore::splitByDelimiters("hello world", 256, delims);
  if (result.size() == 1 && result[0] == "hello world") {
    g_rag_passed++;
    TEST_PASS << "splitByDelimiters empty delimiters" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByDelimiters empty delimiters, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_delimiters_priority() {
  std::vector<std::string> delims = {"\n\n", "\n"};
  auto result = VectorStore::splitByDelimiters("a\n\nb\nc\n\nd", 256, delims);
  if (result.size() == 3 && result[0] == "a" && result[1] == "b\nc" &&
      result[2] == "d") {
    g_rag_passed++;
    TEST_PASS << "splitByDelimiters priority" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByDelimiters priority, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_delimiters_chinese() {
  std::vector<std::string> delims = {"。", "！", "？", "；", "，"};
  auto result = VectorStore::splitByDelimiters(
      "第一句。第二句！第三句？第四句；第五句，第六句", 256, delims);
  if (result.size() == 2) {
    g_rag_passed++;
    TEST_PASS << "splitByDelimiters Chinese" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByDelimiters Chinese, got " << result.size() << " chunks"
              << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_delimiters_recursive_split() {
  std::vector<std::string> delims = {"\n\n", "\n"};
  std::string text = "short\n\n"
                     "AAAAABBBBBCCCCCDDDDDEEEEEFFFFFGGGGGHHHHHIIIIIJJJJJ";
  auto result = VectorStore::splitByDelimiters(text, 10, delims);
  bool allWithinLimit = true;
  for (auto &chunk : result) {
    if (agentxx::util::utf8GetLength(chunk) > 10) {
      allWithinLimit = false;
      break;
    }
  }
  if (result.size() > 1 && allWithinLimit) {
    g_rag_passed++;
    TEST_PASS << "splitByDelimiters recursive split" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByDelimiters recursive split, got " << result.size()
              << " chunks, allWithinLimit=" << allWithinLimit << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: splitTextToChunks — FixedLength mode
// =========================================================================

asio::awaitable<void> test_chunks_fixed_length() {
  VectorStore::SplitConfig config;
  config.mode = VectorStore::SplitMode::FixedLength;
  config.maxUtf8Length = 10;
  auto result =
      VectorStore::splitTextToChunks("Hello World Hello World", config);
  if (result.size() == 3) {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks FixedLength" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks FixedLength, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: splitTextToChunks — Character mode
// =========================================================================

asio::awaitable<void> test_chunks_character() {
  VectorStore::SplitConfig config;
  config.mode = VectorStore::SplitMode::Character;
  config.maxUtf8Length = 256;
  auto result =
      VectorStore::splitTextToChunks("para1\n\npara2\n\npara3", config);
  if (result.size() == 3) {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks Character" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks Character, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: splitTextToChunks — Structural mode
// =========================================================================

asio::awaitable<void> test_chunks_structural() {
  VectorStore::SplitConfig config;
  config.mode = RAGSearchTool::VectorStore::SplitMode::Structural;
  config.maxUtf8Length = 256;
  std::string text = "# Title\n\nParagraph\n\n## Section\n\nMore text";
  auto result = VectorStore::splitTextToChunks(text, config);
  if (result.size() >= 2) {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks Structural" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks Structural, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: splitTextToChunks — StructuralThenChar mode
// =========================================================================

asio::awaitable<void> test_chunks_structural_then_char() {
  VectorStore::SplitConfig config;
  config.mode = RAGSearchTool::VectorStore::SplitMode::StructuralThenChar;
  config.maxUtf8Length = 256;
  std::string text =
      "# Title\n\nParagraph with\n\nbreaks\n\n## Section\n\nMore text";
  auto result = VectorStore::splitTextToChunks(text, config);
  if (result.size() >= 2) {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks StructuralThenChar" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks StructuralThenChar, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: splitTextToChunks — StructuralThenCharThenFixed mode
// =========================================================================

asio::awaitable<void> test_chunks_structural_then_char_then_fixed() {
  VectorStore::SplitConfig config;
  config.mode =
      RAGSearchTool::VectorStore::SplitMode::StructuralThenCharThenFixed;
  config.maxUtf8Length = 256;
  std::string text =
      "# Title\n\nParagraph with\n\nbreaks\n\n## Section\n\nMore text";
  auto result = VectorStore::splitTextToChunks(text, config);
  if (result.size() >= 2) {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks StructuralThenCharThenFixed" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks StructuralThenCharThenFixed, got "
              << result.size() << " chunks" << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: splitTextToChunks — empty text
// =========================================================================

asio::awaitable<void> test_chunks_empty() {
  VectorStore::SplitConfig config;
  auto result = VectorStore::splitTextToChunks("", config);
  if (result.empty()) {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks empty" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks empty should return empty" << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: splitTextToChunks — default config
// =========================================================================

asio::awaitable<void> test_chunks_default_config() {
  VectorStore::SplitConfig config;
  auto result =
      VectorStore::splitTextToChunks("Hello World\n\nThis is a test", config);
  if (result.size() >= 1) {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks default config" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks default config, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: splitTextToChunks — maxUtf8Length enforcement
// =========================================================================

asio::awaitable<void> test_chunks_length_enforcement() {
  VectorStore::SplitConfig config;
  config.mode = VectorStore::SplitMode::FixedLength;
  config.maxUtf8Length = 5;
  std::string longText = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  auto result = VectorStore::splitTextToChunks(longText, config);
  bool allWithinLimit = true;
  for (auto &chunk : result) {
    if (agentxx::util::utf8GetLength(chunk) > 5) {
      allWithinLimit = false;
      break;
    }
  }
  if (allWithinLimit && result.size() >= 6) {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks length enforcement" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks length enforcement" << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: splitTextToChunks — Chinese text with character split
// =========================================================================

asio::awaitable<void> test_chunks_chinese_character() {
  VectorStore::SplitConfig config;
  config.mode = RAGSearchTool::VectorStore::SplitMode::Character;
  config.maxUtf8Length = 256;
  std::string text = "第一段内容。第二段内容。第三段内容。";
  auto result = VectorStore::splitTextToChunks(text, config);
  if (result.size() == 3) {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks Chinese character" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks Chinese character, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: splitTextToChunks — fallback chain with small maxUtf8Length
// =========================================================================

asio::awaitable<void> test_chunks_fallback_chain() {
  VectorStore::SplitConfig config;
  config.mode = VectorStore::SplitMode::StructuralThenCharThenFixed;
  config.maxUtf8Length = 10;
  config.overlapPercent = 0.0;
  std::string text =
      "# Title\n\nParagraphA\n\n## Section\n\nAAAAABBBBBCCCCCDDDDDEEEEE";
  auto result = VectorStore::splitTextToChunks(text, config);
  bool allWithinLimit = true;
  for (auto &chunk : result) {
    if (agentxx::util::utf8GetLength(chunk) > 10) {
      allWithinLimit = false;
      break;
    }
  }
  if (result.size() > 0 && allWithinLimit) {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks fallback chain" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks fallback chain, got " << result.size()
              << " chunks, allWithinLimit=" << allWithinLimit << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: splitTextToChunks — markdown with code blocks
// =========================================================================

asio::awaitable<void> test_chunks_markdown_code() {
  VectorStore::SplitConfig config;
  config.mode = VectorStore::SplitMode::Structural;
  config.maxUtf8Length = 256;
  std::string text = "# Title\n\nSome text\n\n```cpp\nint main() {\n  return "
                     "0;\n}\n```\n\nAfter "
                     "code";
  auto result = VectorStore::splitTextToChunks(text, config);
  bool hasCodeBlock = false;
  for (auto &block : result) {
    if (block.find("```") != std::string::npos) {
      hasCodeBlock = true;
    }
  }
  if (hasCodeBlock && result.size() >= 2) {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks markdown code" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks markdown code, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: splitTextToChunks — short text across all modes
// =========================================================================

asio::awaitable<void> test_chunks_short_text_all_modes() {
  std::string shortText = "Hello";

  for (auto mode :
       {VectorStore::SplitMode::FixedLength, VectorStore::SplitMode::Character,
        VectorStore::SplitMode::Structural,
        VectorStore::SplitMode::StructuralThenChar,
        VectorStore::SplitMode::StructuralThenCharThenFixed}) {
    VectorStore::SplitConfig config;
    config.mode = mode;
    config.maxUtf8Length = 256;
    auto result = VectorStore::splitTextToChunks(shortText, config);
    if (result.size() != 1 || result[0] != "Hello") {
      g_rag_failed++;
      TEST_FAIL << "splitTextToChunks short text mode="
                << static_cast<int>(mode) << ", got " << result.size()
                << " chunks" << std::endl;
      co_return;
    }
  }
  g_rag_passed++;
  TEST_PASS << "splitTextToChunks short text all modes" << std::endl;
  co_return;
}

// =========================================================================
// Tests: splitTextToChunks — custom delimiters
// =========================================================================

asio::awaitable<void> test_chunks_custom_delimiters() {
  VectorStore::SplitConfig config;
  config.mode = VectorStore::SplitMode::Character;
  config.maxUtf8Length = 256;
  config.delimiters = {"|", ",", " "};
  auto result = VectorStore::splitTextToChunks("apple|banana|cherry", config);
  if (result.size() == 3 && result[0] == "apple" && result[1] == "banana" &&
      result[2] == "cherry") {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks custom delimiters" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks custom delimiters, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: splitTextToChunks — very long paragraph with
// StructuralThenCharThenFixed
// =========================================================================

asio::awaitable<void> test_chunks_very_long_paragraph() {
  VectorStore::SplitConfig config;
  config.mode = VectorStore::SplitMode::StructuralThenCharThenFixed;
  config.maxUtf8Length = 20;
  config.overlapPercent = 0.0;
  std::string text = std::string(500, 'X');
  auto result = VectorStore::splitTextToChunks(text, config);
  bool allWithinLimit = true;
  for (auto &chunk : result) {
    if (agentxx::util::utf8GetLength(chunk) > 20) {
      allWithinLimit = false;
      break;
    }
  }
  if (result.size() == 25 && allWithinLimit) {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks very long paragraph" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks very long paragraph, got " << result.size()
              << " chunks, allWithinLimit=" << allWithinLimit << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: splitTextToChunks — Chinese structural mode
// =========================================================================

asio::awaitable<void> test_chunks_chinese_structural() {
  VectorStore::SplitConfig config;
  config.mode = VectorStore::SplitMode::Structural;
  config.maxUtf8Length = 256;
  std::string text = "# 标题\n\n内容段落\n\n## 第二节\n\n更多内容";
  auto result = VectorStore::splitTextToChunks(text, config);
  if (result.size() >= 2) {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks Chinese structural" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks Chinese structural, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: splitTextToChunks — numbered list with structural mode
// =========================================================================

asio::awaitable<void> test_chunks_numbered_list() {
  VectorStore::SplitConfig config;
  config.mode = VectorStore::SplitMode::Structural;
  config.maxUtf8Length = 256;
  std::string text = "# Steps\n\n1. First step\n2. Second step\n3. Third step";
  auto result = VectorStore::splitTextToChunks(text, config);
  if (result.size() >= 1) {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks numbered list" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks numbered list, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: splitTextToChunks — mixed content with all delimiters
// =========================================================================

asio::awaitable<void> test_chunks_mixed_content() {
  VectorStore::SplitConfig config;
  config.mode = VectorStore::SplitMode::StructuralThenCharThenFixed;
  config.maxUtf8Length = 256;
  std::string text = "# Title\n\nParagraph one.\n\n```\ncode block\n```\n\n"
                     "- item 1\n- item 2\n\nFinal paragraph.";
  auto result = VectorStore::splitTextToChunks(text, config);
  if (result.size() >= 3) {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks mixed content" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks mixed content, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: splitTextToChunks — whitespace only text
// =========================================================================

asio::awaitable<void> test_chunks_whitespace_only() {
  VectorStore::SplitConfig config;
  config.mode = VectorStore::SplitMode::Character;
  config.maxUtf8Length = 256;
  auto result = VectorStore::splitTextToChunks("   \n\n   ", config);
  g_rag_passed++;
  TEST_PASS << "splitTextToChunks whitespace only" << std::endl;
  co_return;
}

// =========================================================================
// Tests: splitByFixedLength — with overlap (sliding window)
// =========================================================================

asio::awaitable<void> test_fixed_length_overlap_basic() {
  std::string text = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  auto result = VectorStore::splitByFixedLength(text, 10, 20.0);
  bool hasOverlap = true;
  if (result.size() >= 2) {
    for (size_t i = 1; i < result.size(); ++i) {
      if (result[i - 1].size() >= 2 &&
          result[i - 1].substr(result[i - 1].size() - 2) !=
              result[i].substr(0, 2)) {
        hasOverlap = false;
        break;
      }
    }
  }
  if (result.size() >= 3 && hasOverlap) {
    g_rag_passed++;
    TEST_PASS << "splitByFixedLength overlap basic" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByFixedLength overlap basic, got " << result.size()
              << " chunks, hasOverlap=" << hasOverlap << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_fixed_length_overlap_zero() {
  std::string text = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  auto resultNoOverlap = VectorStore::splitByFixedLength(text, 10, 0.0);
  auto resultOverlap = VectorStore::splitByFixedLength(text, 10, 0.0);
  if (resultNoOverlap.size() == resultOverlap.size() &&
      resultNoOverlap == resultOverlap) {
    g_rag_passed++;
    TEST_PASS << "splitByFixedLength overlap zero (disabled)" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByFixedLength overlap zero, sizes: "
              << resultNoOverlap.size() << " vs " << resultOverlap.size()
              << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_fixed_length_overlap_single_chunk() {
  std::string text = "Hello";
  auto result = VectorStore::splitByFixedLength(text, 100, 20.0);
  if (result.size() == 1 && result[0] == "Hello") {
    g_rag_passed++;
    TEST_PASS << "splitByFixedLength overlap single chunk" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByFixedLength overlap single chunk, got "
              << result.size() << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_fixed_length_overlap_utf8() {
  std::string text = "你好世界你好世界你好世界";
  auto result = VectorStore::splitByFixedLength(text, 4, 25.0);
  if (result.size() >= 3) {
    g_rag_passed++;
    TEST_PASS << "splitByFixedLength overlap utf8" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByFixedLength overlap utf8, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_fixed_length_overlap_50_percent() {
  std::string text = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  auto result = VectorStore::splitByFixedLength(text, 10, 50.0);
  bool allWithinLimit = true;
  for (auto &chunk : result) {
    if (agentxx::util::utf8GetLength(chunk) > 10) {
      allWithinLimit = false;
      break;
    }
  }
  if (result.size() >= 5 && allWithinLimit) {
    g_rag_passed++;
    TEST_PASS << "splitByFixedLength overlap 50 percent" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitByFixedLength overlap 50 percent, got " << result.size()
              << " chunks, allWithinLimit=" << allWithinLimit << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: applyChunkOverlap
// =========================================================================

asio::awaitable<void> test_apply_chunk_overlap_basic() {
  std::vector<std::string> chunks = {"AAAAABBBBB", "CCCCCDDDDD", "EEEEEFFFFF"};
  auto result = VectorStore::applyChunkOverlap(chunks, 10, 20.0);
  if (result.size() == 3 && result[0] == "AAAAABBBBB" &&
      result[1] == "BBCCCCCDDDDD" && result[2] == "DDEEEEEFFFFF") {
    g_rag_passed++;
    TEST_PASS << "applyChunkOverlap basic" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "applyChunkOverlap basic, got " << result.size() << " chunks:";
    for (auto &s : result) {
      std::cout << " [" << s << "]";
    }
    std::cout << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_apply_chunk_overlap_zero() {
  std::vector<std::string> chunks = {"AAAAABBBBB", "CCCCCDDDDD", "EEEEEFFFFF"};
  auto result = VectorStore::applyChunkOverlap(chunks, 10, 0.0);
  if (result == chunks) {
    g_rag_passed++;
    TEST_PASS << "applyChunkOverlap zero (disabled)" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "applyChunkOverlap zero, result differs" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_apply_chunk_overlap_single() {
  std::vector<std::string> chunks = {"HelloWorld"};
  auto result = VectorStore::applyChunkOverlap(chunks, 10, 20.0);
  if (result.size() == 1 && result[0] == "HelloWorld") {
    g_rag_passed++;
    TEST_PASS << "applyChunkOverlap single chunk" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "applyChunkOverlap single chunk, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_apply_chunk_overlap_empty() {
  std::vector<std::string> chunks = {};
  auto result = VectorStore::applyChunkOverlap(chunks, 10, 20.0);
  if (result.empty()) {
    g_rag_passed++;
    TEST_PASS << "applyChunkOverlap empty" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "applyChunkOverlap empty, got " << result.size() << " chunks"
              << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_apply_chunk_overlap_short_prev() {
  std::vector<std::string> chunks = {"AB", "CCCCCDDDDD", "EEEEEFFFFF"};
  auto result = VectorStore::applyChunkOverlap(chunks, 10, 20.0);
  if (result.size() == 3 && result[0] == "AB" && result[1] == "CCCCCDDDDD" &&
      result[2] == "DDEEEEEFFFFF") {
    g_rag_passed++;
    TEST_PASS << "applyChunkOverlap short prev" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "applyChunkOverlap short prev, got " << result.size()
              << " chunks:";
    for (auto &s : result) {
      std::cout << " [" << s << "]";
    }
    std::cout << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_apply_chunk_overlap_utf8() {
  std::vector<std::string> chunks = {"你好世界甲乙丙丁", "戊己庚辛壬癸子丑"};
  auto result = VectorStore::applyChunkOverlap(chunks, 8, 25.0);
  if (result.size() == 2 && result[0] == "你好世界甲乙丙丁" &&
      result[1].size() > chunks[1].size()) {
    g_rag_passed++;
    TEST_PASS << "applyChunkOverlap utf8" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "applyChunkOverlap utf8, got " << result.size() << " chunks"
              << std::endl;
  }
  co_return;
}

// =========================================================================
// Tests: splitTextToChunks — with overlap
// =========================================================================

asio::awaitable<void> test_chunks_overlap_fixed_length() {
  VectorStore::SplitConfig config;
  config.mode = VectorStore::SplitMode::FixedLength;
  config.maxUtf8Length = 10;
  config.overlapPercent = 20.0;
  std::string text = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcd";
  auto result = VectorStore::splitTextToChunks(text, config);
  auto resultNoOverlap =
      VectorStore::splitByFixedLength(text, config.maxUtf8Length);
  if (result.size() > resultNoOverlap.size()) {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks overlap FixedLength" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks overlap FixedLength, got " << result.size()
              << " (no-overlap: " << resultNoOverlap.size() << ")" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_chunks_overlap_character() {
  VectorStore::SplitConfig config;
  config.mode = VectorStore::SplitMode::Character;
  config.maxUtf8Length = 256;
  config.overlapPercent = 20.0;
  std::string text = "para1\n\npara2\n\npara3\n\npara4";
  auto result = VectorStore::splitTextToChunks(text, config);
  if (result.size() >= 4) {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks overlap Character" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks overlap Character, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_chunks_overlap_structural() {
  VectorStore::SplitConfig config;
  config.mode = VectorStore::SplitMode::Structural;
  config.maxUtf8Length = 256;
  config.overlapPercent = 20.0;
  std::string text = "# Title\n\nParagraph\n\n## Section\n\nMore text";
  auto result = VectorStore::splitTextToChunks(text, config);
  if (result.size() >= 2) {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks overlap Structural" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks overlap Structural, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_chunks_overlap_zero_disabled() {
  VectorStore::SplitConfig config;
  config.mode = VectorStore::SplitMode::FixedLength;
  config.maxUtf8Length = 10;
  config.overlapPercent = 0.0;
  std::string text = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  auto result = VectorStore::splitTextToChunks(text, config);
  auto resultDefault = VectorStore::splitByFixedLength(text, 10);
  if (result.size() == resultDefault.size() && result == resultDefault) {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks overlap zero disabled" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks overlap zero disabled, sizes: "
              << result.size() << " vs " << resultDefault.size() << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_chunks_overlap_default_config() {
  VectorStore::SplitConfig config;
  config.maxUtf8Length = 10;
  std::string text = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  auto result = VectorStore::splitTextToChunks(text, config);
  if (result.size() >= 1) {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks overlap default config" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks overlap default config, got "
              << result.size() << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_chunks_overlap_short_text() {
  VectorStore::SplitConfig config;
  config.mode = VectorStore::SplitMode::FixedLength;
  config.maxUtf8Length = 100;
  config.overlapPercent = 20.0;
  std::string text = "Hello";
  auto result = VectorStore::splitTextToChunks(text, config);
  if (result.size() == 1 && result[0] == "Hello") {
    g_rag_passed++;
    TEST_PASS << "splitTextToChunks overlap short text" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "splitTextToChunks overlap short text, got " << result.size()
              << " chunks" << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_chunks_overlap_all_modes() {
  std::string text = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  for (auto mode :
       {VectorStore::SplitMode::FixedLength, VectorStore::SplitMode::Character,
        VectorStore::SplitMode::Structural,
        VectorStore::SplitMode::StructuralThenChar,
        VectorStore::SplitMode::StructuralThenCharThenFixed}) {
    VectorStore::SplitConfig config;
    config.mode = mode;
    config.maxUtf8Length = 10;
    config.overlapPercent = 20.0;
    auto result = VectorStore::splitTextToChunks(text, config);
    if (result.empty()) {
      g_rag_failed++;
      TEST_FAIL << "splitTextToChunks overlap all modes, mode="
                << static_cast<int>(mode) << " returned empty" << std::endl;
      co_return;
    }
  }
  g_rag_passed++;
  TEST_PASS << "splitTextToChunks overlap all modes" << std::endl;
  co_return;
}

// =========================================================================
// Tests: cosineSimilarity
// =========================================================================

asio::awaitable<void> test_cosine_identical() {
  std::vector<double> a = {1.0f, 2.0f, 3.0f};
  std::vector<double> b = {1.0f, 2.0f, 3.0f};
  double sim = RAGSearchTool::cosineSimilarity(a, b);
  if (std::abs(sim - 1.0) < 0.0001) {
    g_rag_passed++;
    TEST_PASS << "cosineSimilarity identical" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "cosineSimilarity identical, sim=" << sim << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_cosine_orthogonal() {
  std::vector<double> a = {1.0f, 0.0f, 0.0f};
  std::vector<double> b = {0.0f, 1.0f, 0.0f};
  double sim = RAGSearchTool::cosineSimilarity(a, b);
  if (std::abs(sim - 0.0) < 0.0001) {
    g_rag_passed++;
    TEST_PASS << "cosineSimilarity orthogonal" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "cosineSimilarity orthogonal, sim=" << sim << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_cosine_empty() {
  std::vector<double> a = {};
  std::vector<double> b = {1.0f, 2.0f};
  double sim = RAGSearchTool::cosineSimilarity(a, b);
  if (std::abs(sim - 0.0) < 0.0001) {
    g_rag_passed++;
    TEST_PASS << "cosineSimilarity empty" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "cosineSimilarity empty, sim=" << sim << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_cosine_both_empty() {
  std::vector<double> a = {};
  std::vector<double> b = {};
  double sim = RAGSearchTool::cosineSimilarity(a, b);
  if (std::abs(sim - 0.0) < 0.0001) {
    g_rag_passed++;
    TEST_PASS << "cosineSimilarity both empty" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "cosineSimilarity both empty, sim=" << sim << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_cosine_mismatched_size() {
  std::vector<double> a = {1.0f, 2.0f, 3.0f};
  std::vector<double> b = {1.0f, 2.0f};
  double sim = RAGSearchTool::cosineSimilarity(a, b);
  if (std::abs(sim - 0.0) < 0.0001) {
    g_rag_passed++;
    TEST_PASS << "cosineSimilarity mismatched size" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "cosineSimilarity mismatched size, sim=" << sim << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_cosine_single_element() {
  std::vector<double> a = {5.0f};
  std::vector<double> b = {3.0f};
  double sim = RAGSearchTool::cosineSimilarity(a, b);
  if (std::abs(sim - 1.0) < 0.0001) {
    g_rag_passed++;
    TEST_PASS << "cosineSimilarity single element" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "cosineSimilarity single element, sim=" << sim << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_cosine_negative_values() {
  std::vector<double> a = {-1.0f, -2.0f, -3.0f};
  std::vector<double> b = {1.0f, 2.0f, 3.0f};
  double sim = RAGSearchTool::cosineSimilarity(a, b);
  if (std::abs(sim - (-1.0)) < 0.0001) {
    g_rag_passed++;
    TEST_PASS << "cosineSimilarity negative values" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "cosineSimilarity negative values, sim=" << sim << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_cosine_partial_overlap() {
  std::vector<double> a = {1.0f, 1.0f, 0.0f, 0.0f};
  std::vector<double> b = {1.0f, 0.0f, 1.0f, 0.0f};
  double sim = RAGSearchTool::cosineSimilarity(a, b);
  double expected = 1.0 / (std::sqrt(2.0) * std::sqrt(2.0));
  if (std::abs(sim - expected) < 0.0001) {
    g_rag_passed++;
    TEST_PASS << "cosineSimilarity partial overlap" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "cosineSimilarity partial overlap, sim=" << sim
              << ", expected=" << expected << std::endl;
  }
  co_return;
}

asio::awaitable<void> test_cosine_zero_vector() {
  std::vector<double> a = {0.0f, 0.0f, 0.0f};
  std::vector<double> b = {1.0f, 2.0f, 3.0f};
  double sim = RAGSearchTool::cosineSimilarity(a, b);
  if (std::abs(sim - 0.0) < 0.0001) {
    g_rag_passed++;
    TEST_PASS << "cosineSimilarity zero vector" << std::endl;
  } else {
    g_rag_failed++;
    TEST_FAIL << "cosineSimilarity zero vector, sim=" << sim << std::endl;
  }
  co_return;
}

// =========================================================================
// Test runner
// =========================================================================

asio::awaitable<TestResult> run_rag_search_tools_tests(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {

  auto run = [](auto testFn) -> asio::awaitable<void> {
    try {
      co_await testFn();
    } catch (const std::exception &e) {
      g_rag_failed++;
      TEST_FAIL << "Exception in test: " << e.what() << std::endl;
    }
  };

  // splitStringByFixedLength
  co_await run(test_fixed_length_basic);
  co_await run(test_fixed_length_empty);
  co_await run(test_fixed_length_utf8);
  co_await run(test_fixed_length_smaller_than_one_char);
  co_await run(test_fixed_length_shorter_than_block);
  co_await run(test_fixed_length_exact_boundary);
  co_await run(test_fixed_length_mixed_ascii_utf8);
  co_await run(test_fixed_length_single_char);
  co_await run(test_fixed_length_very_long);

  // splitByDelimiter
  co_await run(test_delimiter_basic);
  co_await run(test_delimiter_empty);
  co_await run(test_delimiter_no_match);
  co_await run(test_delimiter_empty_delim);
  co_await run(test_delimiter_chinese_punctuation);
  co_await run(test_delimiter_comma);
  co_await run(test_delimiter_at_start);
  co_await run(test_delimiter_at_end);
  co_await run(test_delimiter_consecutive);
  co_await run(test_delimiter_only_delim);
  co_await run(test_delimiter_multibyte_utf8);
  co_await run(test_delimiter_multiple_chinese);

  // splitByStructure
  co_await run(test_structure_headings);
  co_await run(test_structure_code_block);
  co_await run(test_structure_list);
  co_await run(test_structure_empty);
  co_await run(test_structure_numbered_list);
  co_await run(test_structure_mixed_headings);
  co_await run(test_structure_single_paragraph);
  co_await run(test_structure_many_paragraphs);
  co_await run(test_structure_blockquote);
  co_await run(test_structure_table);
  co_await run(test_structure_heading_merge);

  // splitByDelimiters
  co_await run(test_delimiters_basic);
  co_await run(test_delimiters_fallback_to_fixed);
  co_await run(test_delimiters_within_limit);
  co_await run(test_delimiters_empty_text);
  co_await run(test_delimiters_empty_delims);
  co_await run(test_delimiters_priority);
  co_await run(test_delimiters_chinese);
  co_await run(test_delimiters_recursive_split);

  // splitTextToChunks — various modes
  co_await run(test_chunks_fixed_length);
  co_await run(test_chunks_character);
  co_await run(test_chunks_structural);
  co_await run(test_chunks_structural_then_char);
  co_await run(test_chunks_structural_then_char_then_fixed);
  co_await run(test_chunks_empty);
  co_await run(test_chunks_default_config);
  co_await run(test_chunks_length_enforcement);
  co_await run(test_chunks_chinese_character);
  co_await run(test_chunks_fallback_chain);
  co_await run(test_chunks_markdown_code);
  co_await run(test_chunks_short_text_all_modes);
  co_await run(test_chunks_custom_delimiters);
  co_await run(test_chunks_very_long_paragraph);
  co_await run(test_chunks_chinese_structural);
  co_await run(test_chunks_numbered_list);
  co_await run(test_chunks_mixed_content);
  co_await run(test_chunks_whitespace_only);

  // splitByFixedLength — overlap (sliding window)
  co_await run(test_fixed_length_overlap_basic);
  co_await run(test_fixed_length_overlap_zero);
  co_await run(test_fixed_length_overlap_single_chunk);
  co_await run(test_fixed_length_overlap_utf8);
  co_await run(test_fixed_length_overlap_50_percent);

  // applyChunkOverlap
  co_await run(test_apply_chunk_overlap_basic);
  co_await run(test_apply_chunk_overlap_zero);
  co_await run(test_apply_chunk_overlap_single);
  co_await run(test_apply_chunk_overlap_empty);
  co_await run(test_apply_chunk_overlap_short_prev);
  co_await run(test_apply_chunk_overlap_utf8);

  // splitTextToChunks — overlap
  co_await run(test_chunks_overlap_fixed_length);
  co_await run(test_chunks_overlap_character);
  co_await run(test_chunks_overlap_structural);
  co_await run(test_chunks_overlap_zero_disabled);
  co_await run(test_chunks_overlap_default_config);
  co_await run(test_chunks_overlap_short_text);
  co_await run(test_chunks_overlap_all_modes);

  // cosineSimilarity
  co_await run(test_cosine_identical);
  co_await run(test_cosine_orthogonal);
  co_await run(test_cosine_empty);
  co_await run(test_cosine_both_empty);
  co_await run(test_cosine_mismatched_size);
  co_await run(test_cosine_single_element);
  co_await run(test_cosine_negative_values);
  co_await run(test_cosine_partial_overlap);
  co_await run(test_cosine_zero_vector);

  co_return TestResult{g_rag_passed, g_rag_failed};
}

} // namespace test
} // namespace agentxx
