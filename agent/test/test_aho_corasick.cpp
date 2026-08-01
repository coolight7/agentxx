#include "test_aho_corasick.h"

#include "agentxx/util/aho_corasick.h"

namespace agentxx {
namespace test {

using namespace agentxx::util;

int g_ac_passed = 0;
int g_ac_failed = 0;

namespace {

/// 便捷: 提取匹配区间 (start, end) 列表
std::vector<std::pair<size_t, size_t>> ranges(const AhoCorasick<>& ac, std::string_view text) {
    auto                                   matches = ac.search(text);
    std::vector<std::pair<size_t, size_t>> result;
    result.reserve(matches.size());
    for (const auto& m : matches) {
        result.emplace_back(m.start, m.end);
    }
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// 基本匹配
// ---------------------------------------------------------------------------

void test_ac_basic_single() {
    AhoCorasick<> ac(std::vector<std::string>{"abc"});
    XX_TEST_EXPECT_EQ(ranges(ac, "xxabcxx").size(), (size_t)1);
    auto m = ac.search("xxabcxx");
    XX_TEST_EXPECT_EQ(m[0].start, (size_t)2);
    XX_TEST_EXPECT_EQ(m[0].end, (size_t)5);
    XX_TEST_EXPECT_EQ(ac.getPattern(m[0].patternId), std::string("abc"));

    // 多次出现
    XX_TEST_EXPECT_EQ(ranges(ac, "abcabcabc").size(), (size_t)3);
    // 无匹配
    XX_TEST_EXPECT_EQ(ranges(ac, "xyz").size(), (size_t)0);
}

void test_ac_basic_multi() {
    AhoCorasick<> ac(std::vector<std::string>{"ab", "cd"});
    auto          m = ac.search("xxabxxcdxx");
    XX_TEST_EXPECT_EQ(m.size(), (size_t)2);
    XX_TEST_EXPECT_EQ(m[0].start, (size_t)2);
    XX_TEST_EXPECT_EQ(m[0].end, (size_t)4);
    XX_TEST_EXPECT_EQ(m[1].start, (size_t)6);
    XX_TEST_EXPECT_EQ(m[1].end, (size_t)8);
}

void test_ac_adjacent_not_merged() {
    // 相邻区间 [0,2) [2,4) 均保留
    AhoCorasick<> ac(std::vector<std::string>{"ab", "cd"});
    XX_TEST_EXPECT_EQ(ranges(ac, "abcd").size(), (size_t)2);
}

// ---------------------------------------------------------------------------
// 重叠匹配: 长优先、去重叠
// ---------------------------------------------------------------------------

void test_ac_overlap_longest() {
    // 经典教材例子: patterns {he, she, his, hers}, text "ushers"
    // 原始匹配: she[1,4) he[2,4) hers[2,6) -> 去重叠后保留 she[1,4)
    AhoCorasick<> ac(std::vector<std::string>{"he", "she", "his", "hers"});
    auto          m = ac.search("ushers");
    XX_TEST_EXPECT_EQ(m.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(m[0].start, (size_t)1);
    XX_TEST_EXPECT_EQ(m[0].end, (size_t)4);
    XX_TEST_EXPECT_EQ(ac.getPattern(m[0].patternId), std::string("she"));
}

void test_ac_overlap_ab_bc() {
    // "abc" 中 "ab"[0,2) 与 "bc"[1,3) 重叠 -> 保留先出现的 [0,2)
    AhoCorasick<> ac(std::vector<std::string>{"ab", "bc"});
    auto          m = ac.search("abc");
    XX_TEST_EXPECT_EQ(m.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(m[0].start, (size_t)0);
    XX_TEST_EXPECT_EQ(m[0].end, (size_t)2);
}

void test_ac_same_start_longer_priority() {
    // 同一起点: 长模式优先 (排序 end 降序)
    AhoCorasick<> ac(std::vector<std::string>{"a", "ab", "abc"});
    auto          m = ac.search("abcd");
    XX_TEST_EXPECT_EQ(m.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(m[0].start, (size_t)0);
    XX_TEST_EXPECT_EQ(m[0].end, (size_t)3);
}

// ---------------------------------------------------------------------------
// 大小写
// ---------------------------------------------------------------------------

void test_ac_ignore_case() {
    AhoCorasick<> ac(std::vector<std::string>{"abc"});
    // 默认忽略大小写
    XX_TEST_EXPECT_EQ(ranges(ac, "ABCabcAbC").size(), (size_t)3);
    XX_TEST_EXPECT_EQ(ranges(ac, "aBc").size(), (size_t)1);
}

void test_ac_case_sensitive() {
    AhoCorasick<> ac(std::vector<std::string>{"abc"}, false);
    XX_TEST_EXPECT_EQ(ranges(ac, "abcABC").size(), (size_t)1);
    auto m = ac.search("abcABC");
    XX_TEST_EXPECT_EQ(m[0].start, (size_t)0);
    XX_TEST_EXPECT_EQ(m[0].end, (size_t)3);
}

// ---------------------------------------------------------------------------
// contains / removeAll
// ---------------------------------------------------------------------------

void test_ac_contains() {
    AhoCorasick<> ac(std::vector<std::string>{"敏感词1", "敏感词2"});
    XX_TEST_EXPECT_TRUE(ac.contains("这段文本包含敏感词1"));
    XX_TEST_EXPECT_FALSE(ac.contains("这段文本没有敏感内容"));
    XX_TEST_EXPECT_FALSE(ac.contains(""));
}

void test_ac_remove_all() {
    AhoCorasick<> ac(std::vector<std::string>{"a", "b"});
    XX_TEST_EXPECT_EQ(ac.removeAll("xxayybzz"), std::string("xxyyzz"));
    // 无匹配: 原样返回
    XX_TEST_EXPECT_EQ(ac.removeAll("hello world"), std::string("hello world"));
    // 空输入
    XX_TEST_EXPECT_EQ(ac.removeAll(""), std::string(""));
    // 全部匹配
    XX_TEST_EXPECT_EQ(ac.removeAll("ababab"), std::string(""));
}

void test_ac_remove_all_with_callback() {
    AhoCorasick<> ac(std::vector<std::string>{"a"});
    size_t        callbackCount = 0;
    auto          result        = ac.removeAll("xaxa", [&](const auto& matches) {
        callbackCount = matches.size();
    });
    XX_TEST_EXPECT_EQ(callbackCount, (size_t)2);
    XX_TEST_EXPECT_EQ(result, std::string("xx"));
}

// ---------------------------------------------------------------------------
// 边界: 空文本 / 空模式列表 / 中文
// ---------------------------------------------------------------------------

void test_ac_empty() {
    // 空文本
    AhoCorasick<> ac1(std::vector<std::string>{"abc"});
    XX_TEST_EXPECT_EQ(ranges(ac1, "").size(), (size_t)0);
    XX_TEST_EXPECT_FALSE(ac1.contains(""));

    // 空模式列表: 不崩溃且不匹配
    AhoCorasick<> ac2(std::vector<std::string>{});
    XX_TEST_EXPECT_EQ(ranges(ac2, "hello").size(), (size_t)0);
    XX_TEST_EXPECT_FALSE(ac2.contains("hello"));
    XX_TEST_EXPECT_EQ(ac2.removeAll("hello"), std::string("hello"));
}

void test_ac_chinese() {
    // 多字节 UTF-8: 区间按字节偏移
    AhoCorasick<> ac(std::vector<std::string>{"你好", "世界"});
    auto          m = ac.search("你好世界你好");
    XX_TEST_EXPECT_EQ(m.size(), (size_t)3);
    XX_TEST_EXPECT_EQ(m[0].start, (size_t)0);
    XX_TEST_EXPECT_EQ(m[0].end, (size_t)6); // "你好" 6 字节
    XX_TEST_EXPECT_EQ(m[1].start, (size_t)6);
    XX_TEST_EXPECT_EQ(m[1].end, (size_t)12);
    XX_TEST_EXPECT_EQ(m[2].start, (size_t)12);
    XX_TEST_EXPECT_EQ(m[2].end, (size_t)18);
}

// ---------------------------------------------------------------------------
// 前缀/后缀关系 (fail 指针路径)
// ---------------------------------------------------------------------------

void test_ac_prefix_suffix() {
    // "ab" 是 "abc" 的前缀; "bc" 是 "abc" 的后缀
    // text "abc": 匹配 ab[0,2) abc[0,3) bc[1,3) -> 去重叠保留 abc[0,3)
    AhoCorasick<> ac(std::vector<std::string>{"ab", "abc", "bc"});
    auto          m = ac.search("abc");
    XX_TEST_EXPECT_EQ(m.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(m[0].start, (size_t)0);
    XX_TEST_EXPECT_EQ(m[0].end, (size_t)3);
    XX_TEST_EXPECT_EQ(ac.getPattern(m[0].patternId), std::string("abc"));
}

void test_ac_fail_chain() {
    // 失败指针链: "aaaa" 中连续 'a' 触发多次 "aa" 匹配
    AhoCorasick<> ac(std::vector<std::string>{"aa"});
    auto          m = ac.search("aaaa");
    XX_TEST_EXPECT_EQ(m.size(), (size_t)2);
    XX_TEST_EXPECT_EQ(m[0].start, (size_t)0);
    XX_TEST_EXPECT_EQ(m[0].end, (size_t)2);
    XX_TEST_EXPECT_EQ(m[1].start, (size_t)2);
    XX_TEST_EXPECT_EQ(m[1].end, (size_t)4);
}

// ---------------------------------------------------------------------------
// 模式中包含通配字符/特殊字符 (按字面匹配)
// ---------------------------------------------------------------------------

void test_ac_literal_special_chars() {
    // 非正则: '.' '*' 等按字面匹配
    AhoCorasick<> ac(std::vector<std::string>{"a.b", "x*y"});
    auto          m = ac.search("xxa.bzzx*y");
    XX_TEST_EXPECT_EQ(m.size(), (size_t)2);
    XX_TEST_EXPECT_EQ(m[0].start, (size_t)2);
    XX_TEST_EXPECT_EQ(m[0].end, (size_t)5);
    XX_TEST_EXPECT_EQ(m[1].start, (size_t)7);
    XX_TEST_EXPECT_EQ(m[1].end, (size_t)10);
}

TestResult testAhoCorasick() {
    g_ac_passed = 0;
    g_ac_failed = 0;

    test_ac_basic_single();
    test_ac_basic_multi();
    test_ac_adjacent_not_merged();
    test_ac_overlap_longest();
    test_ac_overlap_ab_bc();
    test_ac_same_start_longer_priority();
    test_ac_ignore_case();
    test_ac_case_sensitive();
    test_ac_contains();
    test_ac_remove_all();
    test_ac_remove_all_with_callback();
    test_ac_empty();
    test_ac_chinese();
    test_ac_prefix_suffix();
    test_ac_fail_chain();
    test_ac_literal_special_chars();

    return TestResult{g_ac_passed, g_ac_failed};
}

} // namespace test
} // namespace agentxx
