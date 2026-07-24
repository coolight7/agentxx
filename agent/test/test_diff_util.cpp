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

namespace agentxx {
namespace test {

TestResult testDiffUtil() {
    test_computeLineDiff_identical();
    test_computeLineDiff_modify();
    test_computeLineDiff_insert();
    test_computeLineDiff_delete();
    test_computeLineDiff_lineNumbers();
    test_makeUnifiedDiff();
    return TestResult{g_diff_passed, g_diff_failed};
}

} // namespace test
} // namespace agentxx
