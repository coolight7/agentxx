#include "test_regex.h"

namespace agentxx {
namespace test {

using namespace agentxx::util;

int g_regex_passed = 0;
int g_regex_failed = 0;

void test_regex_create() {
    auto re = XXRegex::createRegex("hello");
    XX_TEST_EXPECT_TRUE(re != nullptr);

    auto re_invalid = XXRegex::createRegex("[invalid");
    XX_TEST_EXPECT_TRUE(re_invalid != nullptr);

    auto re_multi = XXRegex::createRegex(std::vector<std::string>{"hello", "world"});
    XX_TEST_EXPECT_TRUE(re_multi != nullptr);

    auto re_empty = XXRegex::createRegex(std::vector<std::string>{});
    XX_TEST_EXPECT_TRUE(re_empty != nullptr);
}

void test_regex_match_basic() {
    auto                            re = XXRegex::createRegex("hello");
    std::vector<XXRegexMatchResult> results;

    XX_TEST_EXPECT_TRUE(re->match("hello world", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(results[0].start, (size_t)0);
    XX_TEST_EXPECT_EQ(results[0].end, (size_t)5);

    XX_TEST_EXPECT_FALSE(re->match("world", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_match_multi() {
    auto                            re = XXRegex::createRegex("ab");
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
    // 多模式场景：不同模式产生重叠匹配，应合并 [1,3) [2,4) -> [1,4)
    auto                            re = XXRegex::createRegex(std::vector<std::string>{"ab", "bc"});
    std::vector<XXRegexMatchResult> results;

    XX_TEST_EXPECT_TRUE(re->match("abc", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(results[0].start, (size_t)0);
    XX_TEST_EXPECT_EQ(results[0].end, (size_t)3);
}

void test_regex_match_adjacent_no_merge() {
    // 相邻区间 [0,2) [2,4) 不应合并
    auto                            re = XXRegex::createRegex("ab");
    std::vector<XXRegexMatchResult> results;

    XX_TEST_EXPECT_TRUE(re->match("abab", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)2);
    XX_TEST_EXPECT_EQ(results[0].start, (size_t)0);
    XX_TEST_EXPECT_EQ(results[0].end, (size_t)2);
    XX_TEST_EXPECT_EQ(results[1].start, (size_t)2);
    XX_TEST_EXPECT_EQ(results[1].end, (size_t)4);
}

void test_regex_match_empty_input() {
    auto                            re = XXRegex::createRegex("hello");
    std::vector<XXRegexMatchResult> results;

    XX_TEST_EXPECT_FALSE(re->match("", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_match_dot_star() {
    auto                            re = XXRegex::createRegex("a.*b");
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
    auto                            re = XXRegex::createRegex("abc");
    std::vector<XXRegexMatchResult> results;

    auto result = re->remove("123abc456", results);
    XX_TEST_EXPECT_EQ(result, std::string("123456"));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(results[0].start, (size_t)3);
    XX_TEST_EXPECT_EQ(results[0].end, (size_t)6);
}

void test_regex_remove_no_match() {
    auto                            re = XXRegex::createRegex("xyz");
    std::vector<XXRegexMatchResult> results;

    auto result = re->remove("hello world", results);
    XX_TEST_EXPECT_EQ(result, std::string("hello world"));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_remove_multi() {
    auto                            re = XXRegex::createRegex("XX");
    std::vector<XXRegexMatchResult> results;

    auto result = re->remove("aXXbbXXc", results);
    XX_TEST_EXPECT_EQ(result, std::string("abbc"));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)2);
}

void test_regex_remove_all() {
    auto                            re = XXRegex::createRegex(".+");
    std::vector<XXRegexMatchResult> results;

    auto result = re->remove("hello", results);
    XX_TEST_EXPECT_EQ(result, std::string(""));
    XX_TEST_EXPECT_TRUE(results.size() > 0);
}

void test_regex_replace_basic() {
    auto                            re = XXRegex::createRegex("cat");
    std::vector<XXRegexMatchResult> results;

    auto result = re->replace("the cat sat", "dog", results);
    XX_TEST_EXPECT_EQ(result, std::string("the dog sat"));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(results[0].start, (size_t)4);
    XX_TEST_EXPECT_EQ(results[0].end, (size_t)7);
}

void test_regex_replace_no_match() {
    auto                            re = XXRegex::createRegex("xyz");
    std::vector<XXRegexMatchResult> results;

    auto result = re->replace("hello world", "test", results);
    XX_TEST_EXPECT_EQ(result, std::string("hello world"));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_replace_multi() {
    auto                            re = XXRegex::createRegex("XX");
    std::vector<XXRegexMatchResult> results;

    auto result = re->replace("aXXbbXXc", "yy", results);
    XX_TEST_EXPECT_EQ(result, std::string("ayybbyyc"));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)2);
}

void test_regex_replace_with_empty() {
    auto                            re = XXRegex::createRegex("XX");
    std::vector<XXRegexMatchResult> results;

    auto result = re->replace("aXXbbXXc", "", results);
    XX_TEST_EXPECT_EQ(result, std::string("abbc"));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)2);
}

void test_regex_digit_match() {
    auto                            re = XXRegex::createRegex("\\d+");
    std::vector<XXRegexMatchResult> results;

    XX_TEST_EXPECT_TRUE(re->match("abc123def456", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)2);
    XX_TEST_EXPECT_EQ(results[0].start, (size_t)3);
    XX_TEST_EXPECT_EQ(results[0].end, (size_t)6);
    XX_TEST_EXPECT_EQ(results[1].start, (size_t)9);
    XX_TEST_EXPECT_EQ(results[1].end, (size_t)12);
}

void test_regex_alternation() {
    auto                            re = XXRegex::createRegex("cat|dog");
    std::vector<XXRegexMatchResult> results;

    XX_TEST_EXPECT_TRUE(re->match("I have a cat", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)1);

    XX_TEST_EXPECT_TRUE(re->match("I have a dog", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)1);

    XX_TEST_EXPECT_FALSE(re->match("I have a bird", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_chinese() {
    auto                            re = XXRegex::createRegex("你好");
    std::vector<XXRegexMatchResult> results;

    XX_TEST_EXPECT_TRUE(re->match("hello 你好 world", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)1);
}

void test_regex_case_sensitive() {
    auto                            re = XXRegex::createRegex("hello");
    std::vector<XXRegexMatchResult> results;

    XX_TEST_EXPECT_TRUE(re->match("hello world", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)1);

    XX_TEST_EXPECT_FALSE(re->match("Hello World", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_case_insensitive() {
    // caseInsensitive=true: 大小写不敏感匹配 (Hyperscan: HS_FLAG_CASELESS,
    // std::regex fallback: icase), 不再依赖外部折叠模式
    auto re = XXRegex::createRegex("hello", XXRegex::defHSFlags_normal, true);
    std::vector<XXRegexMatchResult> results;

    XX_TEST_EXPECT_TRUE(re->match("Hello World", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(results[0].start, (size_t)0);
    XX_TEST_EXPECT_EQ(results[0].end, (size_t)5);

    XX_TEST_EXPECT_TRUE(re->match("heLLo", results));
    XX_TEST_EXPECT_TRUE(re->match("HELLO", results));

    // 多模式 + 大小写不敏感
    auto re_multi = XXRegex::createRegex(
        std::vector<std::string>{"foo", "bar"},
        XXRegex::defHSFlags_normal,
        true
    );
    XX_TEST_EXPECT_TRUE(re_multi->match("FOO", results));
    XX_TEST_EXPECT_TRUE(re_multi->match("BaR", results));
    XX_TEST_EXPECT_FALSE(re_multi->match("baz", results));

    // 大小写不敏感不应影响其他语法 (如字符类内的转义)
    auto re_class = XXRegex::createRegex("[a-z]+", XXRegex::defHSFlags_normal, true);
    XX_TEST_EXPECT_TRUE(re_class->match("ABC", results));
    XX_TEST_EXPECT_TRUE(re_class->match("abc", results));
}

void test_regex_only_contains() {
    auto re = XXRegex::createRegex("hello", XXRegex::defHSFlags_onlyContains);
    std::vector<XXRegexMatchResult> results;

    bool matched = re->match("hello world", results);
    XX_TEST_EXPECT_TRUE(matched);
}

void test_regex_invalid_pattern() {
    auto                            re = XXRegex::createRegex("[unclosed");
    std::vector<XXRegexMatchResult> results;

    XX_TEST_EXPECT_FALSE(re->match("test", results));
}

void test_regex_exact_match() {
    auto                            re = XXRegex::createRegex("^hello$");
    std::vector<XXRegexMatchResult> results;

    XX_TEST_EXPECT_TRUE(re->match("hello", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(results[0].start, (size_t)0);
    XX_TEST_EXPECT_EQ(results[0].end, (size_t)5);

    XX_TEST_EXPECT_FALSE(re->match("hello world", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_start_anchor() {
    auto                            re = XXRegex::createRegex("^hello");
    std::vector<XXRegexMatchResult> results;

    XX_TEST_EXPECT_TRUE(re->match("hello world", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)1);

    XX_TEST_EXPECT_FALSE(re->match("world hello", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_end_anchor() {
    auto                            re = XXRegex::createRegex("world$");
    std::vector<XXRegexMatchResult> results;

    XX_TEST_EXPECT_TRUE(re->match("hello world", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)1);

    XX_TEST_EXPECT_FALSE(re->match("world hello", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)0);
}

void test_regex_word_no_newline() {
    // \w 不应匹配换行符 \n / \r
    auto                            re = XXRegex::createRegex("\\w");
    std::vector<XXRegexMatchResult> results;

    // 单个换行符不应被 \w 匹配
    XX_TEST_EXPECT_FALSE(re->match("\n", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)0);
    XX_TEST_EXPECT_FALSE(re->match("\r", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)0);

    // \w 匹配 "a\nb" 中的 a 和 b, 不包含/跨越换行符
    XX_TEST_EXPECT_TRUE(re->match("a\nb", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)2);
    XX_TEST_EXPECT_EQ(results[0].start, (size_t)0);
    XX_TEST_EXPECT_EQ(results[0].end, (size_t)1);
    XX_TEST_EXPECT_EQ(results[1].start, (size_t)2);
    XX_TEST_EXPECT_EQ(results[1].end, (size_t)3);

    // \w 仍可匹配普通单词字符
    XX_TEST_EXPECT_TRUE(re->match("A", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)1);
    XX_TEST_EXPECT_TRUE(re->match("_", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)1);
}

void test_regex_only_contains_behavior() {
    auto re = XXRegex::createRegex("hello", XXRegex::defHSFlags_onlyContains);
    std::vector<XXRegexMatchResult> results;

    // 存在即匹配
    XX_TEST_EXPECT_TRUE(re->match("say hello world", results));
    XX_TEST_EXPECT_TRUE(results.size() > 0);
    // 不存在返回 false
    XX_TEST_EXPECT_FALSE(re->match("goodbye world", results));
    // 空输入
    XX_TEST_EXPECT_FALSE(re->match("", results));

    // 多模式 + onlyContains
    auto re2 = XXRegex::createRegex(
        std::vector<std::string>{"foo", "bar"},
        XXRegex::defHSFlags_onlyContains
    );
    XX_TEST_EXPECT_TRUE(re2->match("a foo here", results));
    XX_TEST_EXPECT_TRUE(re2->match("a bar here", results));
    XX_TEST_EXPECT_FALSE(re2->match("nothing", results));
}

void test_regex_dot_not_match_newline() {
    // "." 默认不匹配换行符
    auto                            re = XXRegex::createRegex("a.c");
    std::vector<XXRegexMatchResult> results;

    XX_TEST_EXPECT_TRUE(re->match("abc", results));
    // "." 不匹配 \n (hyperscan 默认), 但匹配 \r
    XX_TEST_EXPECT_FALSE(re->match("a\nc", results));
    // XX_TEST_EXPECT_TRUE(re->match("a\rc", results));
}

void test_regex_empty_pattern() {
    // 空模式: 创建与调用不崩溃
    auto                            re = XXRegex::createRegex("");
    std::vector<XXRegexMatchResult> results;
    (void)re->match("anything", results);
    (void)re->remove("anything", results);
    (void)re->replace("anything", "x", results);

    // 空模式列表
    auto re2 = XXRegex::createRegex(std::vector<std::string>{});
    (void)re2->match("anything", results);
}

void test_regex_escape_special() {
    // 转义特殊字符按字面匹配
    auto                            re = XXRegex::createRegex("a\\.b");
    std::vector<XXRegexMatchResult> results;

    XX_TEST_EXPECT_TRUE(re->match("a.b", results));
    XX_TEST_EXPECT_FALSE(re->match("axb", results));

    auto re2 = XXRegex::createRegex("\\d+\\.\\d+");
    XX_TEST_EXPECT_TRUE(re2->match("3.14", results));
    XX_TEST_EXPECT_FALSE(re2->match("abc", results));
}

void test_regex_remove_edge() {
    auto                            re = XXRegex::createRegex("ab");
    std::vector<XXRegexMatchResult> results;

    // 开头匹配
    XX_TEST_EXPECT_EQ(re->remove("abxx", results), std::string("xx"));
    // 结尾匹配
    XX_TEST_EXPECT_EQ(re->remove("xxab", results), std::string("xx"));
    // 全部匹配
    XX_TEST_EXPECT_EQ(re->remove("abab", results), std::string(""));
    // 匹配之间保留原文
    XX_TEST_EXPECT_EQ(re->remove("xabyabz", results), std::string("xyz"));
}

void test_regex_replace_edge() {
    auto                            re = XXRegex::createRegex("ab");
    std::vector<XXRegexMatchResult> results;

    XX_TEST_EXPECT_EQ(re->replace("abxx", "X", results), std::string("Xxx"));
    XX_TEST_EXPECT_EQ(re->replace("xxab", "X", results), std::string("xxX"));
    XX_TEST_EXPECT_EQ(re->replace("abab", "X", results), std::string("XX"));
    // 替换为多字符
    XX_TEST_EXPECT_EQ(re->replace("ab", "long-repl", results), std::string("long-repl"));
    // 替换串为空 (等效 remove)
    XX_TEST_EXPECT_EQ(re->replace("xab", "", results), std::string("x"));
}

void test_regex_long_input() {
    auto                            re = XXRegex::createRegex("target");
    std::vector<XXRegexMatchResult> results;

    std::string longStr(10000, 'x');
    longStr += "target";
    XX_TEST_EXPECT_TRUE(re->match(longStr, results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(results[0].start, (size_t)10000);
    XX_TEST_EXPECT_EQ(results[0].end, (size_t)10006);
}

void test_regex_unicode_mixed() {
    auto                            re = XXRegex::createRegex("世界");
    std::vector<XXRegexMatchResult> results;

    XX_TEST_EXPECT_TRUE(re->match("hello 世界 hello", results));
    // 中文 + 英文混合模式
    auto re2 = XXRegex::createRegex("测试\\d+");
    XX_TEST_EXPECT_TRUE(re2->match("abc测试123xyz", results));
    XX_TEST_EXPECT_EQ(results.size(), (size_t)1);
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
    test_regex_case_insensitive();
    test_regex_only_contains();
    test_regex_invalid_pattern();
    test_regex_exact_match();
    test_regex_start_anchor();
    test_regex_end_anchor();
    test_regex_word_no_newline();
    test_regex_only_contains_behavior();
    test_regex_dot_not_match_newline();
    test_regex_empty_pattern();
    test_regex_escape_special();
    test_regex_remove_edge();
    test_regex_replace_edge();
    test_regex_long_input();
    test_regex_unicode_mixed();

    return TestResult{g_regex_passed, g_regex_failed};
}

} // namespace test
} // namespace agentxx
