// 不可见文本判断测试 (见 agentxx/util/invisible_text.h)
//
// 覆盖场景:
// - 空串、纯空白、零宽字符、格式控制符判为"视觉为空"
// - 含任意可见字符 (英文/中文/数字/标点/emoji) 时为假
// - 零宽字符**后面跟可见字符**时为假 (不能因为开头是零宽就吞掉真实内容)
// - 非法 UTF-8 字节按"有内容"处理 (宁可多显示一条, 也不要静默丢掉内容)
// - 单码点判断 (isInvisibleCodePoint) 的边界取值
//
// 这些字符来自真实会话: 模型偶尔把 U+200B 当正文输出并独占一条消息,
// 若当成正常内容提交, 消息列表里就是一条看不见内容的空消息。
#include "agentxx-test/core/test_invisible_text.h"
#include "agentxx/util/invisible_text.h"

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_it_passed = 0;
int g_it_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_it_passed
#define XX_TEST_FAILED g_it_failed

namespace agentxx {
namespace test {

TestResult testInvisibleText() {
    using agentxx::util::isInvisibleCodePoint;
    using agentxx::util::isVisuallyBlank;

    // 1) 空串与纯空白
    XX_TEST_EXPECT_TRUE(isVisuallyBlank(""));
    XX_TEST_EXPECT_TRUE(isVisuallyBlank(" "));
    XX_TEST_EXPECT_TRUE(isVisuallyBlank(" \t\r\n"));
    XX_TEST_EXPECT_TRUE(isVisuallyBlank("\xE3\x80\x80"));       // U+3000 全角空格
    XX_TEST_EXPECT_TRUE(isVisuallyBlank(" \xE3\x80\x80 \t"));  // 混合空白

    // 2) 零宽字符与格式控制符 (模型输出的占位符)
    XX_TEST_EXPECT_TRUE(isVisuallyBlank("\xE2\x80\x8B"));                // U+200B 零宽空格
    XX_TEST_EXPECT_TRUE(isVisuallyBlank("\xE2\x80\x8B\xE2\x80\x8C "));   // U+200B + U+200C + 空格
    XX_TEST_EXPECT_TRUE(isVisuallyBlank("\xEF\xBB\xBF"));                // U+FEFF BOM
    XX_TEST_EXPECT_TRUE(isVisuallyBlank("\xC2\xA0"));                    // U+00A0 不换行空格
    XX_TEST_EXPECT_TRUE(isVisuallyBlank("\xC2\xAD"));                    // U+00AD 软连字符
    XX_TEST_EXPECT_TRUE(isVisuallyBlank("\xE2\x81\xA0"));                // U+2060 单词连接符
    XX_TEST_EXPECT_TRUE(isVisuallyBlank("\xE2\x80\xA8"));                // U+2028 行分隔符
    XX_TEST_EXPECT_TRUE(isVisuallyBlank("\xE2\xA0\x80"));                // U+2800 盲文空白

    // 3) 含可见字符时为假
    XX_TEST_EXPECT_FALSE(isVisuallyBlank("a"));
    XX_TEST_EXPECT_FALSE(isVisuallyBlank(" a "));
    XX_TEST_EXPECT_FALSE(isVisuallyBlank("0"));
    XX_TEST_EXPECT_FALSE(isVisuallyBlank("."));
    XX_TEST_EXPECT_FALSE(isVisuallyBlank("\xE4\xB8\xAD\xE6\x96\x87"));  // 中文
    XX_TEST_EXPECT_FALSE(isVisuallyBlank("\xF0\x9F\x98\x80"));          // U+1F600 emoji

    // 4) 零宽字符后面跟可见字符: 有内容
    XX_TEST_EXPECT_FALSE(isVisuallyBlank("\xE2\x80\x8B" "a"));
    XX_TEST_EXPECT_FALSE(isVisuallyBlank(" \xE2\x80\x8B" "\xE4\xB8\xAD "));

    // 5) 非法 UTF-8 字节按有内容处理
    XX_TEST_EXPECT_FALSE(isVisuallyBlank("\xFF"));
    XX_TEST_EXPECT_FALSE(isVisuallyBlank("\xE2\x80"));  // 被截断的三字节序列

    // 6) 单码点判断边界
    XX_TEST_EXPECT_TRUE(isInvisibleCodePoint(0x00));
    XX_TEST_EXPECT_TRUE(isInvisibleCodePoint(0x09));   // TAB
    XX_TEST_EXPECT_TRUE(isInvisibleCodePoint(0x20));   // 空格
    XX_TEST_EXPECT_TRUE(isInvisibleCodePoint(0x7F));   // DEL
    XX_TEST_EXPECT_TRUE(isInvisibleCodePoint(0x85));   // C1 控制符
    XX_TEST_EXPECT_TRUE(isInvisibleCodePoint(0x200B)); // 零宽空格
    XX_TEST_EXPECT_TRUE(isInvisibleCodePoint(0x3000)); // 全角空格
    XX_TEST_EXPECT_TRUE(isInvisibleCodePoint(0xFEFF)); // BOM
    XX_TEST_EXPECT_FALSE(isInvisibleCodePoint(0x21));  // '!'
    XX_TEST_EXPECT_FALSE(isInvisibleCodePoint(0x41));  // 'A'
    XX_TEST_EXPECT_FALSE(isInvisibleCodePoint(0x4E2D)); // 中
    XX_TEST_EXPECT_FALSE(isInvisibleCodePoint(0x1F600)); // emoji

    return TestResult{g_it_passed, g_it_failed};
}

} // namespace test
} // namespace agentxx
