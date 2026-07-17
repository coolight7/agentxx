#include "agentxx/util/regex.h"
#include "test_framework.h"

#include <cassert>
#include <memory>
#include <string>
#include <vector>

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_regex_passed
#define XX_TEST_FAILED g_regex_failed

namespace agentxx {
namespace test {

using namespace agentxx::util;

inline static int g_regex_passed = 0;
inline static int g_regex_failed = 0;

void test_regex_create() {

  auto re = XXRegex::createRegex("hello");
  XX_TEST_EXPECT_TRUE(re != nullptr);

  auto re_invalid = XXRegex::createRegex("[invalid");
  XX_TEST_EXPECT_TRUE(re_invalid != nullptr);

  auto re_multi =
      XXRegex::createRegex(std::vector<std::string>{"hello", "world"});
  XX_TEST_EXPECT_TRUE(re_multi != nullptr);

  auto re_empty = XXRegex::createRegex(std::vector<std::string>{});
  XX_TEST_EXPECT_TRUE(re_empty != nullptr);
}

void test_regex_match_basic() {

  auto re = XXRegex::createRegex("hello");
  std::vector<XXRegexMatchResult> results;

  XX_TEST_EXPECT_TRUE(re->match("hello world", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)1);
  XX_TEST_EXPECT_EQ(results[0].start, (size_t)0);
  XX_TEST_EXPECT_EQ(results[0].end, (size_t)5);

  XX_TEST_EXPECT_FALSE(re->match("world", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_match_multi() {

  auto re = XXRegex::createRegex("ab");
  std::vector<XXRegexMatchResult> results;

  XX_TEST_EXPECT_TRUE(re->match("abxxabxxab", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)3);
  XX_TEST_EXPECT_EQ(results[0].start, (size_t)0);
  XX_TEST_EXPECT_EQ(results[0].end, (size_t)2);
  XX_TEST_EXPECT_EQ(results[1].start, (size_t)4);
  XX_TEST_EXPECT_EQ(results[1].end, (size_t)6);
  XX_TEST_EXPECT_EQ(results[2].start, (size_t)8);
  XX_TEST_EXPECT_EQ(results[2].end, (size_t)10);
}

void test_regex_match_overlap_merge() {

  // 多模式场景：不同模式产生重叠匹配，应合并 [1,3) [2,4) → [1,4)
  auto re = XXRegex::createRegex(std::vector<std::string>{"ab", "bc"});
  std::vector<XXRegexMatchResult> results;

  XX_TEST_EXPECT_TRUE(re->match("abc", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)1);
  XX_TEST_EXPECT_EQ(results[0].start, (size_t)0);
  XX_TEST_EXPECT_EQ(results[0].end, (size_t)3);
}

void test_regex_match_adjacent_no_merge() {

  // 相邻区间 [0,2) [2,4) 不应合并
  auto re = XXRegex::createRegex("ab");
  std::vector<XXRegexMatchResult> results;

  XX_TEST_EXPECT_TRUE(re->match("abab", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)2);
  XX_TEST_EXPECT_EQ(results[0].start, (size_t)0);
  XX_TEST_EXPECT_EQ(results[0].end, (size_t)2);
  XX_TEST_EXPECT_EQ(results[1].start, (size_t)2);
  XX_TEST_EXPECT_EQ(results[1].end, (size_t)4);
}

void test_regex_match_empty_input() {

  auto re = XXRegex::createRegex("hello");
  std::vector<XXRegexMatchResult> results;

  XX_TEST_EXPECT_FALSE(re->match("", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_match_dot_star() {

  auto re = XXRegex::createRegex("a.*b");
  std::vector<XXRegexMatchResult> results;

  XX_TEST_EXPECT_TRUE(re->match("a123b", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)1);
  XX_TEST_EXPECT_EQ(results[0].start, (size_t)0);
  XX_TEST_EXPECT_EQ(results[0].end, (size_t)5);

  XX_TEST_EXPECT_TRUE(re->match("ab", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)1);
  XX_TEST_EXPECT_EQ(results[0].start, (size_t)0);
  XX_TEST_EXPECT_EQ(results[0].end, (size_t)2);
}

void test_regex_match_multi_pattern() {

  auto re = XXRegex::createRegex(std::vector<std::string>{"foo", "bar"});
  std::vector<XXRegexMatchResult> results;

  XX_TEST_EXPECT_TRUE(re->match("hello foo world", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)1);
  XX_TEST_EXPECT_EQ(results[0].start, (size_t)6);
  XX_TEST_EXPECT_EQ(results[0].end, (size_t)9);

  XX_TEST_EXPECT_TRUE(re->match("foo xx bar", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)2);
  XX_TEST_EXPECT_EQ(results[0].start, (size_t)0);
  XX_TEST_EXPECT_EQ(results[0].end, (size_t)3);
  XX_TEST_EXPECT_EQ(results[1].start, (size_t)7);
  XX_TEST_EXPECT_EQ(results[1].end, (size_t)10);
}

void test_regex_remove_basic() {

  auto re = XXRegex::createRegex("abc");
  std::vector<XXRegexMatchResult> results;

  auto result = re->remove("123abc456", results);
  XX_TEST_EXPECT_EQ(result, std::string("123456"));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)1);
  XX_TEST_EXPECT_EQ(results[0].start, (size_t)3);
  XX_TEST_EXPECT_EQ(results[0].end, (size_t)6);
}

void test_regex_remove_no_match() {

  auto re = XXRegex::createRegex("xyz");
  std::vector<XXRegexMatchResult> results;

  auto result = re->remove("hello world", results);
  XX_TEST_EXPECT_EQ(result, std::string("hello world"));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_remove_multi() {

  auto re = XXRegex::createRegex("XX");
  std::vector<XXRegexMatchResult> results;

  auto result = re->remove("aXXbbXXc", results);
  XX_TEST_EXPECT_EQ(result, std::string("abbc"));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)2);
}

void test_regex_remove_all() {

  auto re = XXRegex::createRegex(".+");
  std::vector<XXRegexMatchResult> results;

  auto result = re->remove("hello", results);
  XX_TEST_EXPECT_EQ(result, std::string(""));
  XX_TEST_EXPECT_TRUE(results.size() > 0);
}

void test_regex_replace_basic() {

  auto re = XXRegex::createRegex("cat");
  std::vector<XXRegexMatchResult> results;

  auto result = re->replace("the cat sat", "dog", results);
  XX_TEST_EXPECT_EQ(result, std::string("the dog sat"));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)1);
  XX_TEST_EXPECT_EQ(results[0].start, (size_t)4);
  XX_TEST_EXPECT_EQ(results[0].end, (size_t)7);
}

void test_regex_replace_no_match() {

  auto re = XXRegex::createRegex("xyz");
  std::vector<XXRegexMatchResult> results;

  auto result = re->replace("hello world", "test", results);
  XX_TEST_EXPECT_EQ(result, std::string("hello world"));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_replace_multi() {

  auto re = XXRegex::createRegex("XX");
  std::vector<XXRegexMatchResult> results;

  auto result = re->replace("aXXbbXXc", "yy", results);
  XX_TEST_EXPECT_EQ(result, std::string("ayybbyyc"));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)2);
}

void test_regex_replace_with_empty() {

  auto re = XXRegex::createRegex("XX");
  std::vector<XXRegexMatchResult> results;

  auto result = re->replace("aXXbbXXc", "", results);
  XX_TEST_EXPECT_EQ(result, std::string("abbc"));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)2);
}

void test_regex_digit_match() {

  auto re = XXRegex::createRegex("\\d+");
  std::vector<XXRegexMatchResult> results;

  XX_TEST_EXPECT_TRUE(re->match("abc123def456", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)2);
  XX_TEST_EXPECT_EQ(results[0].start, (size_t)3);
  XX_TEST_EXPECT_EQ(results[0].end, (size_t)6);
  XX_TEST_EXPECT_EQ(results[1].start, (size_t)9);
  XX_TEST_EXPECT_EQ(results[1].end, (size_t)12);
}

void test_regex_alternation() {

  auto re = XXRegex::createRegex("cat|dog");
  std::vector<XXRegexMatchResult> results;

  XX_TEST_EXPECT_TRUE(re->match("I have a cat", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)1);

  XX_TEST_EXPECT_TRUE(re->match("I have a dog", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)1);

  XX_TEST_EXPECT_FALSE(re->match("I have a bird", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_chinese() {

  auto re = XXRegex::createRegex("你好");
  std::vector<XXRegexMatchResult> results;

  XX_TEST_EXPECT_TRUE(re->match("hello 你好 world", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)1);
}

void test_regex_case_sensitive() {

  auto re = XXRegex::createRegex("hello");
  std::vector<XXRegexMatchResult> results;

  XX_TEST_EXPECT_TRUE(re->match("hello world", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)1);

  XX_TEST_EXPECT_FALSE(re->match("Hello World", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_only_contains() {

  auto re = XXRegex::createRegex("hello", XXRegex::defHSFlags_onlyContains);
  std::vector<XXRegexMatchResult> results;

  bool matched = re->match("hello world", results);
  XX_TEST_EXPECT_TRUE(matched);
}

void test_regex_invalid_pattern() {

  auto re = XXRegex::createRegex("[unclosed");
  std::vector<XXRegexMatchResult> results;

  XX_TEST_EXPECT_FALSE(re->match("test", results));
}

void test_regex_exact_match() {

  auto re = XXRegex::createRegex("^hello$");
  std::vector<XXRegexMatchResult> results;

  XX_TEST_EXPECT_TRUE(re->match("hello", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)1);
  XX_TEST_EXPECT_EQ(results[0].start, (size_t)0);
  XX_TEST_EXPECT_EQ(results[0].end, (size_t)5);

  XX_TEST_EXPECT_FALSE(re->match("hello world", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_start_anchor() {

  auto re = XXRegex::createRegex("^hello");
  std::vector<XXRegexMatchResult> results;

  XX_TEST_EXPECT_TRUE(re->match("hello world", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)1);

  XX_TEST_EXPECT_FALSE(re->match("world hello", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_end_anchor() {

  auto re = XXRegex::createRegex("world$");
  std::vector<XXRegexMatchResult> results;

  XX_TEST_EXPECT_TRUE(re->match("hello world", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)1);

  XX_TEST_EXPECT_FALSE(re->match("world hello", results));
  XX_TEST_EXPECT_EQ(results.size(), (size_t)0);
}

TestResult testRegex() {
  test_regex_create();
  test_regex_match_basic();
  test_regex_match_multi();
  test_regex_match_overlap_merge();
  test_regex_match_adjacent_no_merge();
  test_regex_match_empty_input();
  test_regex_match_dot_star();
  test_regex_match_multi_pattern();
  test_regex_remove_basic();
  test_regex_remove_no_match();
  test_regex_remove_multi();
  test_regex_remove_all();
  test_regex_replace_basic();
  test_regex_replace_no_match();
  test_regex_replace_multi();
  test_regex_replace_with_empty();
  test_regex_digit_match();
  test_regex_alternation();
  test_regex_chinese();
  test_regex_case_sensitive();
  test_regex_only_contains();
  test_regex_invalid_pattern();
  test_regex_exact_match();
  test_regex_start_anchor();
  test_regex_end_anchor();

  return TestResult{g_regex_passed, g_regex_failed};
}

} // namespace test
} // namespace agentxx