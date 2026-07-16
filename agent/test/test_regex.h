#include "agentxx/util/regex.h"
#include "test_framework.h"

#include <cassert>
#include <memory>
#include <string>
#include <vector>

using namespace agentxx::util;

inline static int g_regex_passed = 0;
inline static int g_regex_failed = 0;

#define REGEX_EXPECT_EQ(expr, expected)                                        \
  do {                                                                         \
    auto _result = (expr);                                                     \
    auto _expected = (expected);                                               \
    if (_result == _expected) {                                                \
      g_regex_passed++;                                                        \
    } else {                                                                   \
      g_regex_failed++;                                                        \
      TEST_FAIL << "line " << __LINE__ << ": expected " << (_expected)         \
                << ", got " << (_result) << std::endl;                         \
    }                                                                          \
  } while (0)

#define REGEX_EXPECT_TRUE(expr)                                                \
  do {                                                                         \
    if (expr) {                                                                \
      g_regex_passed++;                                                        \
    } else {                                                                   \
      g_regex_failed++;                                                        \
      TEST_FAIL << "line " << __LINE__ << ": expected true" << std::endl;      \
    }                                                                          \
  } while (0)

#define REGEX_EXPECT_FALSE(expr)                                               \
  do {                                                                         \
    if (!(expr)) {                                                             \
      g_regex_passed++;                                                        \
    } else {                                                                   \
      g_regex_failed++;                                                        \
      TEST_FAIL << "line " << __LINE__ << ": expected false" << std::endl;     \
    }                                                                          \
  } while (0)

void test_regex_create() {

  auto re = XXRegex::createRegex("hello");
  REGEX_EXPECT_TRUE(re != nullptr);

  auto re_invalid = XXRegex::createRegex("[invalid");
  REGEX_EXPECT_TRUE(re_invalid != nullptr);

  auto re_multi =
      XXRegex::createRegex(std::vector<std::string>{"hello", "world"});
  REGEX_EXPECT_TRUE(re_multi != nullptr);

  auto re_empty = XXRegex::createRegex(std::vector<std::string>{});
  REGEX_EXPECT_TRUE(re_empty != nullptr);
}

void test_regex_match_basic() {

  auto re = XXRegex::createRegex("hello");
  std::vector<XXRegexMatchResult> results;

  REGEX_EXPECT_TRUE(re->match("hello world", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)1);
  REGEX_EXPECT_EQ(results[0].start, (size_t)0);
  REGEX_EXPECT_EQ(results[0].end, (size_t)5);

  REGEX_EXPECT_FALSE(re->match("world", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_match_multi() {

  auto re = XXRegex::createRegex("ab");
  std::vector<XXRegexMatchResult> results;

  REGEX_EXPECT_TRUE(re->match("abxxabxxab", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)3);
  REGEX_EXPECT_EQ(results[0].start, (size_t)0);
  REGEX_EXPECT_EQ(results[0].end, (size_t)2);
  REGEX_EXPECT_EQ(results[1].start, (size_t)4);
  REGEX_EXPECT_EQ(results[1].end, (size_t)6);
  REGEX_EXPECT_EQ(results[2].start, (size_t)8);
  REGEX_EXPECT_EQ(results[2].end, (size_t)10);
}

void test_regex_match_overlap_merge() {

  // 多模式场景：不同模式产生重叠匹配，应合并 [1,3) [2,4) → [1,4)
  auto re = XXRegex::createRegex(std::vector<std::string>{"ab", "bc"});
  std::vector<XXRegexMatchResult> results;

  REGEX_EXPECT_TRUE(re->match("abc", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)1);
  REGEX_EXPECT_EQ(results[0].start, (size_t)0);
  REGEX_EXPECT_EQ(results[0].end, (size_t)3);
}

void test_regex_match_adjacent_no_merge() {

  // 相邻区间 [0,2) [2,4) 不应合并
  auto re = XXRegex::createRegex("ab");
  std::vector<XXRegexMatchResult> results;

  REGEX_EXPECT_TRUE(re->match("abab", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)2);
  REGEX_EXPECT_EQ(results[0].start, (size_t)0);
  REGEX_EXPECT_EQ(results[0].end, (size_t)2);
  REGEX_EXPECT_EQ(results[1].start, (size_t)2);
  REGEX_EXPECT_EQ(results[1].end, (size_t)4);
}

void test_regex_match_empty_input() {

  auto re = XXRegex::createRegex("hello");
  std::vector<XXRegexMatchResult> results;

  REGEX_EXPECT_FALSE(re->match("", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_match_dot_star() {

  auto re = XXRegex::createRegex("a.*b");
  std::vector<XXRegexMatchResult> results;

  REGEX_EXPECT_TRUE(re->match("a123b", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)1);
  REGEX_EXPECT_EQ(results[0].start, (size_t)0);
  REGEX_EXPECT_EQ(results[0].end, (size_t)5);

  REGEX_EXPECT_TRUE(re->match("ab", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)1);
  REGEX_EXPECT_EQ(results[0].start, (size_t)0);
  REGEX_EXPECT_EQ(results[0].end, (size_t)2);
}

void test_regex_match_multi_pattern() {

  auto re = XXRegex::createRegex(std::vector<std::string>{"foo", "bar"});
  std::vector<XXRegexMatchResult> results;

  REGEX_EXPECT_TRUE(re->match("hello foo world", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)1);
  REGEX_EXPECT_EQ(results[0].start, (size_t)6);
  REGEX_EXPECT_EQ(results[0].end, (size_t)9);

  REGEX_EXPECT_TRUE(re->match("foo xx bar", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)2);
  REGEX_EXPECT_EQ(results[0].start, (size_t)0);
  REGEX_EXPECT_EQ(results[0].end, (size_t)3);
  REGEX_EXPECT_EQ(results[1].start, (size_t)7);
  REGEX_EXPECT_EQ(results[1].end, (size_t)10);
}

void test_regex_remove_basic() {

  auto re = XXRegex::createRegex("abc");
  std::vector<XXRegexMatchResult> results;

  auto result = re->remove("123abc456", results);
  REGEX_EXPECT_EQ(result, std::string("123456"));
  REGEX_EXPECT_EQ(results.size(), (size_t)1);
  REGEX_EXPECT_EQ(results[0].start, (size_t)3);
  REGEX_EXPECT_EQ(results[0].end, (size_t)6);
}

void test_regex_remove_no_match() {

  auto re = XXRegex::createRegex("xyz");
  std::vector<XXRegexMatchResult> results;

  auto result = re->remove("hello world", results);
  REGEX_EXPECT_EQ(result, std::string("hello world"));
  REGEX_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_remove_multi() {

  auto re = XXRegex::createRegex("XX");
  std::vector<XXRegexMatchResult> results;

  auto result = re->remove("aXXbbXXc", results);
  REGEX_EXPECT_EQ(result, std::string("abbc"));
  REGEX_EXPECT_EQ(results.size(), (size_t)2);
}

void test_regex_remove_all() {

  auto re = XXRegex::createRegex(".+");
  std::vector<XXRegexMatchResult> results;

  auto result = re->remove("hello", results);
  REGEX_EXPECT_EQ(result, std::string(""));
  REGEX_EXPECT_TRUE(results.size() > 0);
}

void test_regex_replace_basic() {

  auto re = XXRegex::createRegex("cat");
  std::vector<XXRegexMatchResult> results;

  auto result = re->replace("the cat sat", "dog", results);
  REGEX_EXPECT_EQ(result, std::string("the dog sat"));
  REGEX_EXPECT_EQ(results.size(), (size_t)1);
  REGEX_EXPECT_EQ(results[0].start, (size_t)4);
  REGEX_EXPECT_EQ(results[0].end, (size_t)7);
}

void test_regex_replace_no_match() {

  auto re = XXRegex::createRegex("xyz");
  std::vector<XXRegexMatchResult> results;

  auto result = re->replace("hello world", "test", results);
  REGEX_EXPECT_EQ(result, std::string("hello world"));
  REGEX_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_replace_multi() {

  auto re = XXRegex::createRegex("XX");
  std::vector<XXRegexMatchResult> results;

  auto result = re->replace("aXXbbXXc", "yy", results);
  REGEX_EXPECT_EQ(result, std::string("ayybbyyc"));
  REGEX_EXPECT_EQ(results.size(), (size_t)2);
}

void test_regex_replace_with_empty() {

  auto re = XXRegex::createRegex("XX");
  std::vector<XXRegexMatchResult> results;

  auto result = re->replace("aXXbbXXc", "", results);
  REGEX_EXPECT_EQ(result, std::string("abbc"));
  REGEX_EXPECT_EQ(results.size(), (size_t)2);
}

void test_regex_digit_match() {

  auto re = XXRegex::createRegex("\\d+");
  std::vector<XXRegexMatchResult> results;

  REGEX_EXPECT_TRUE(re->match("abc123def456", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)2);
  REGEX_EXPECT_EQ(results[0].start, (size_t)3);
  REGEX_EXPECT_EQ(results[0].end, (size_t)6);
  REGEX_EXPECT_EQ(results[1].start, (size_t)9);
  REGEX_EXPECT_EQ(results[1].end, (size_t)12);
}

void test_regex_alternation() {

  auto re = XXRegex::createRegex("cat|dog");
  std::vector<XXRegexMatchResult> results;

  REGEX_EXPECT_TRUE(re->match("I have a cat", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)1);

  REGEX_EXPECT_TRUE(re->match("I have a dog", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)1);

  REGEX_EXPECT_FALSE(re->match("I have a bird", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_chinese() {

  auto re = XXRegex::createRegex("你好");
  std::vector<XXRegexMatchResult> results;

  REGEX_EXPECT_TRUE(re->match("hello 你好 world", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)1);
}

void test_regex_case_sensitive() {

  auto re = XXRegex::createRegex("hello");
  std::vector<XXRegexMatchResult> results;

  REGEX_EXPECT_TRUE(re->match("hello world", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)1);

  REGEX_EXPECT_FALSE(re->match("Hello World", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_only_contains() {

  auto re = XXRegex::createRegex("hello", XXRegex::defHSFlags_onlyContains);
  std::vector<XXRegexMatchResult> results;

  bool matched = re->match("hello world", results);
  REGEX_EXPECT_TRUE(matched);
}

void test_regex_invalid_pattern() {

  auto re = XXRegex::createRegex("[unclosed");
  std::vector<XXRegexMatchResult> results;

  REGEX_EXPECT_FALSE(re->match("test", results));
}

void test_regex_exact_match() {

  auto re = XXRegex::createRegex("^hello$");
  std::vector<XXRegexMatchResult> results;

  REGEX_EXPECT_TRUE(re->match("hello", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)1);
  REGEX_EXPECT_EQ(results[0].start, (size_t)0);
  REGEX_EXPECT_EQ(results[0].end, (size_t)5);

  REGEX_EXPECT_FALSE(re->match("hello world", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_start_anchor() {

  auto re = XXRegex::createRegex("^hello");
  std::vector<XXRegexMatchResult> results;

  REGEX_EXPECT_TRUE(re->match("hello world", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)1);

  REGEX_EXPECT_FALSE(re->match("world hello", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_end_anchor() {

  auto re = XXRegex::createRegex("world$");
  std::vector<XXRegexMatchResult> results;

  REGEX_EXPECT_TRUE(re->match("hello world", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)1);

  REGEX_EXPECT_FALSE(re->match("world hello", results));
  REGEX_EXPECT_EQ(results.size(), (size_t)0);
}

namespace agentxx {
namespace test {

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