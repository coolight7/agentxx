#include "test_diff_util.h"

using namespace agentxx::util;

int g_diff_passed = 0;
int g_diff_failed = 0;

namespace {

int countType(const std::vector<DiffLine>& diff, DiffLineType t) {
    int n = 0;
    for (const auto& l : diff) {
        if (l.type == t) {
            ++n;
        }
    }
    return n;
}

} // namespace

void test_computeLineDiff_identical() {
    auto diff = computeLineDiff("a\nb\nc", "a\nb\nc");
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Context), 3);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Add), 0);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Delete), 0);
}

void test_computeLineDiff_modify() {
    auto diff = computeLineDiff("line1\nline2\nline3", "line1\nLINE2\nline3");
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Context), 2);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Delete), 1);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Add), 1);
}

void test_computeLineDiff_insert() {
    auto diff = computeLineDiff("a\nb", "a\nx\ny\nb");
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Context), 2);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Add), 2);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Delete), 0);
}

void test_computeLineDiff_delete() {
    auto diff = computeLineDiff("a\nx\ny\nb", "a\nb");
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Context), 2);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Delete), 2);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Add), 0);
}

void test_computeLineDiff_lineNumbers() {
    auto diff = computeLineDiff("a\nb\nc", "a\nX\nc");
    for (const auto& l : diff) {
        if (l.type == DiffLineType::Delete) {
            XX_TEST_EXPECT_EQ(l.text, std::string("b"));
            XX_TEST_EXPECT_EQ(l.oldLineNo, 2);
            XX_TEST_EXPECT_EQ(l.newLineNo, 0);
        } else if (l.type == DiffLineType::Add) {
            XX_TEST_EXPECT_EQ(l.text, std::string("X"));
            XX_TEST_EXPECT_EQ(l.oldLineNo, 0);
            XX_TEST_EXPECT_EQ(l.newLineNo, 2);
        }
    }
}

void test_makeUnifiedDiff() {
    auto out = makeUnifiedDiff("a\nb", "a\nx\nb", "f.txt");
    XX_TEST_EXPECT_TRUE(out.find("--- a/f.txt") != std::string::npos);
    XX_TEST_EXPECT_TRUE(out.find("+++ b/f.txt") != std::string::npos);
    XX_TEST_EXPECT_TRUE(out.find("@@ -1,2 +1,3 @@") != std::string::npos);
    XX_TEST_EXPECT_TRUE(out.find("\n+x") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 边界: 空输入 / 全删 / 全增 / 单行 / CRLF / 空行
// ---------------------------------------------------------------------------

void test_computeLineDiff_empty() {
    // 空 vs 空
    auto d1 = computeLineDiff("", "");
    XX_TEST_EXPECT_EQ(d1.size(), (size_t)0);

    // 空 vs 非空: 全部新增
    auto d2 = computeLineDiff("", "a\nb");
    XX_TEST_EXPECT_EQ(countType(d2, DiffLineType::Context), 0);
    XX_TEST_EXPECT_EQ(countType(d2, DiffLineType::Add), 2);
    XX_TEST_EXPECT_EQ(countType(d2, DiffLineType::Delete), 0);
    // Add 行无旧行号, 新行号递增
    for (const auto& l : d2) {
        XX_TEST_EXPECT_EQ(l.oldLineNo, 0);
    }
    XX_TEST_EXPECT_EQ(d2[0].newLineNo, 1);
    XX_TEST_EXPECT_EQ(d2[1].newLineNo, 2);

    // 非空 vs 空: 全部删除
    auto d3 = computeLineDiff("a\nb", "");
    XX_TEST_EXPECT_EQ(countType(d3, DiffLineType::Context), 0);
    XX_TEST_EXPECT_EQ(countType(d3, DiffLineType::Add), 0);
    XX_TEST_EXPECT_EQ(countType(d3, DiffLineType::Delete), 2);
    XX_TEST_EXPECT_EQ(d3[0].oldLineNo, 1);
    XX_TEST_EXPECT_EQ(d3[1].oldLineNo, 2);
}

void test_computeLineDiff_single_line_no_newline() {
    // 单行无换行
    auto d1 = computeLineDiff("a", "a");
    XX_TEST_EXPECT_EQ(countType(d1, DiffLineType::Context), 1);
    XX_TEST_EXPECT_EQ(d1[0].text, std::string("a"));

    auto d2 = computeLineDiff("a", "b");
    XX_TEST_EXPECT_EQ(countType(d2, DiffLineType::Delete), 1);
    XX_TEST_EXPECT_EQ(countType(d2, DiffLineType::Add), 1);
}

void test_computeLineDiff_crlf() {
    // CRLF 行尾: 行内容保留 \r, 修改最后一行
    auto diff = computeLineDiff("a\r\nb\r\nc", "a\r\nb\r\nX");
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Context), 2);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Delete), 1);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Add), 1);
    for (const auto& l : diff) {
        if (l.type == DiffLineType::Context) {
            // 上下文行保留 \r
            XX_TEST_EXPECT_TRUE(l.text == "a\r" || l.text == "b\r");
        }
    }
}

void test_computeLineDiff_trailing_newline() {
    // 末尾换行符被忽略: "a\nb" 与 "a\nb\n" 视为相同
    auto diff = computeLineDiff("a\nb", "a\nb\n");
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Context), 2);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Add), 0);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Delete), 0);
}

void test_computeLineDiff_empty_lines() {
    // 中间空行
    auto diff = computeLineDiff("a\n\nb", "a\n\nc");
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Context), 2); // a 与空行
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Delete), 1);  // b
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Add), 1);     // c
}

void test_computeLineDiff_completely_different() {
    auto diff = computeLineDiff("1\n2\n3", "x\ny\nz");
    // 全部不同: 3 删 3 增 (无公共行)
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Context), 0);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Delete), 3);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Add), 3);
}

void test_computeLineDiff_line_order() {
    // 顺序敏感: "a\nb" 与 "b\na" 中 LCS=1, 仅保留 1 个公共行
    // (先匹配到哪个公共行取决于回溯顺序, 因此只断言数量)
    auto diff = computeLineDiff("a\nb", "b\na");
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Context), 1);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Delete), 1);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Add), 1);
}

void test_makeUnifiedDiff_full() {
    // 完整输出匹配 (含上下文行与变更行)
    auto out = makeUnifiedDiff("a\nb", "a\nx\nb", "f.txt");
    XX_TEST_EXPECT_EQ(
        out,
        std::string("--- a/f.txt\n+++ b/f.txt\n@@ -1,2 +1,3 @@\n a\n+x\n b\n")
    );
}

void test_computeLineDiff_large_input_degrades() {
    // 回归: 超大输入触发规模保护降级 (避免 (n+1)*(m+1) DP 表 OOM/极慢)。
    // 5000x5000 > 16M 阈值 -> 降级为"全删除 + 全新增", 结果仍正确表示完整变更。
    std::string oldText;
    std::string newText;
    const int   N = 5000;
    for (int i = 0; i < N; ++i) {
        oldText += "old_line_" + std::to_string(i) + "\n";
        newText += "new_line_" + std::to_string(i) + "\n";
    }
    auto diff = computeLineDiff(oldText, newText);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Delete), N);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Add), N);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Context), 0);
}

void test_computeLineDiff_moderate_input_normal() {
    // 中等规模 (远低于阈值) 仍走正常 LCS, 能识别公共行
    std::string oldText;
    std::string newText;
    for (int i = 0; i < 100; ++i) {
        oldText += "common_" + std::to_string(i) + "\n";
        newText += "common_" + std::to_string(i) + "\n";
    }
    auto diff = computeLineDiff(oldText, newText);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Context), 100);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Delete), 0);
    XX_TEST_EXPECT_EQ(countType(diff, DiffLineType::Add), 0);
}

namespace agentxx {
namespace test {

TestResult testDiffUtil() {
    test_computeLineDiff_identical();
    test_computeLineDiff_modify();
    test_computeLineDiff_insert();
    test_computeLineDiff_delete();
    test_computeLineDiff_lineNumbers();
    test_makeUnifiedDiff();
    test_computeLineDiff_empty();
    test_computeLineDiff_single_line_no_newline();
    test_computeLineDiff_crlf();
    test_computeLineDiff_trailing_newline();
    test_computeLineDiff_empty_lines();
    test_computeLineDiff_completely_different();
    test_computeLineDiff_line_order();
    test_makeUnifiedDiff_full();
    test_computeLineDiff_large_input_degrades();
    test_computeLineDiff_moderate_input_normal();
    return TestResult{g_diff_passed, g_diff_failed};
}

} // namespace test
} // namespace agentxx
