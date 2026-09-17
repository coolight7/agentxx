#include "agentxx-test/core/test_string_util.h"
#include "utilxx_base/string_util.h"

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_su_passed = 0;
int g_su_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_su_passed
#define XX_TEST_FAILED g_su_failed

// 组合断言: compareExtend 双向比较结果应互为相反数
#define shiftCompareExtend(left, right, sub)                           \
    XX_TEST_EXPECT_EQ(utilxx_base::compareExtend(left, right), sub); \
    XX_TEST_EXPECT_EQ(utilxx_base::compareExtend(right, left), -(sub));

// 原 agentxx::util 已拆分: 基础件在 utilxx_base, 重依赖工具在 utilxx
using namespace utilxx_base;

void test_compareExtend() {
    XX_TEST_EXPECT_EQ(utilxx_base::compareExtend("", ""), 0);
    XX_TEST_EXPECT_EQ(utilxx_base::compareExtend(" ", " "), 0);
    XX_TEST_EXPECT_EQ(utilxx_base::compareExtend("123", "123"), 0);
    XX_TEST_EXPECT_EQ(utilxx_base::compareExtend(" 123\t", " 123\t"), 0);
    XX_TEST_EXPECT_EQ(utilxx_base::compareExtend(" #=k123abc\t\r\n", " #=k123abc\t\r\n"), 0);

    shiftCompareExtend("", "   ", -1);
    shiftCompareExtend(" ", "    ", -3);
    shiftCompareExtend("1", "2", -1);
    shiftCompareExtend("1", "111", 1 - 111);
    shiftCompareExtend("2", "234", 2 - 234);
    shiftCompareExtend("77", "234", 77 - 234);
    shiftCompareExtend("03.9,999 xxx", "01. xxx", 3 - 1);
    shiftCompareExtend("03.9,999", "01.", 3 - 1);
    shiftCompareExtend("03.9,999 xxx", "03.9,88", 999 - 88);
    shiftCompareExtend("03.9,999 xxx", "01", 3 - 1);
    shiftCompareExtend("03.9,999 xxx", "03.77 xxx", 9 - 77);
    shiftCompareExtend("003.xxx", "08.xxx", 3 - 8);
    shiftCompareExtend("003.xxx", "80.xxx", 3 - 80);
    // shiftCompareExtend(" #= 你 77", " #= 你 234", 77 - 234);
    shiftCompareExtend(" #= 2 kkk", " #= 7", 2 - 7);
    // shiftCompareExtend("123 cool q", "123 cool 七八九", -2);

    // 回归: 大数字段不应整数溢出 (原 int 累加对 >10 位数字是 UB)。
    // 饱和截断时 INT_MIN/INT_MAX 不对称, 故用符号断言而非 shiftCompareExtend。
    {
        // 10 位大数 (> INT_MAX) vs 小数: 符号正确
        XX_TEST_EXPECT_TRUE(utilxx_base::compareExtend("file9999999999", "file1") > 0);
        XX_TEST_EXPECT_TRUE(utilxx_base::compareExtend("file1", "file9999999999") < 0);
        // int64 范围内大数精确比较: 9999999999 - 9999999998 = 1
        XX_TEST_EXPECT_EQ(utilxx_base::compareExtend("x9999999999", "x9999999998"), 1);
        XX_TEST_EXPECT_EQ(utilxx_base::compareExtend("x9999999998", "x9999999999"), -1);
        // 超 int64 范围 (饱和处理): 不崩溃, 大数仍 > 小数
        XX_TEST_EXPECT_TRUE(utilxx_base::compareExtend("file99999999999999999999", "file1") > 0);
        XX_TEST_EXPECT_TRUE(utilxx_base::compareExtend("file1", "file99999999999999999999") < 0);
        // 前缀恰为 922337203685477580 (kInt64Max/10) 时, 如果实现为 leftSum*10+d
        // 在 d>=8 时有符号溢出 (UB); 修复后:
        // - d<=7 走精确路径 (9223372036854775807 是最大可精确表示值)
        // - d>=8 走饱和路径 (映射到 INT64_MAX), 符号仍正确
        XX_TEST_EXPECT_EQ(
            utilxx_base::compareExtend("x9223372036854775807", "x9223372036854775806"),
            1
        );
        XX_TEST_EXPECT_EQ(
            utilxx_base::compareExtend("x9223372036854775806", "x9223372036854775807"),
            -1
        );
        // d=8 溢出路径: 饱和后与小数比较符号正确
        XX_TEST_EXPECT_EQ(
            utilxx_base::compareExtend("x9223372036854775808", "x9223372036854775806"),
            1
        );
        XX_TEST_EXPECT_EQ(
            utilxx_base::compareExtend("x9223372036854775806", "x9223372036854775808"),
            -1
        );
        XX_TEST_EXPECT_TRUE(utilxx_base::compareExtend("file9223372036854775808", "file1") > 0);
    }
}

void test_toStandardPath() {
    XX_TEST_EXPECT_EQ(utilxx_base::toStandardPath("//////"), "/");
    XX_TEST_EXPECT_EQ(utilxx_base::toStandardPath("\\\\\\"), "\\");
    XX_TEST_EXPECT_EQ(utilxx_base::toStandardPath("\\\\\\/\\/\\////\\/"), "\\");
    XX_TEST_EXPECT_EQ(utilxx_base::toStandardPath("a/b\\d"), "a/b\\d");
    XX_TEST_EXPECT_EQ(utilxx_base::toStandardPath("a///b\\d"), "a/b\\d");
    XX_TEST_EXPECT_EQ(utilxx_base::toStandardPath("a/b\\\\\\d"), "a/b\\d");
    XX_TEST_EXPECT_EQ(utilxx_base::toStandardPath("a/////b\\\\d"), "a/b\\d");
    XX_TEST_EXPECT_EQ(utilxx_base::toStandardPath("///a/b\\d"), "/a/b\\d");
    XX_TEST_EXPECT_EQ(utilxx_base::toStandardPath("//a///b\\\\\\\\d/////"), "/a/b\\d/");
    XX_TEST_EXPECT_EQ(utilxx_base::toStandardPath("\\\\\\a///b\\\\\\\\d\\\\\\"), "\\a/b\\d\\");
    XX_TEST_EXPECT_EQ(utilxx_base::toStandardPath("/\\\\\\a//b\\/\\\\///d\\\\\\/"), "\\a/b\\d\\");
}

void test_toUnixStandardPath() {
    XX_TEST_EXPECT_EQ(utilxx_base::toUnixStandardPath("\\\\\\\\\\"), "/");
    XX_TEST_EXPECT_EQ(utilxx_base::toUnixStandardPath("\\\\\\\\\\//////\\/\\/\\/"), "/");
    XX_TEST_EXPECT_EQ(utilxx_base::toUnixStandardPath("a/b/d"), "a/b/d");
    XX_TEST_EXPECT_EQ(utilxx_base::toUnixStandardPath("a/b\\d"), "a/b/d");
    XX_TEST_EXPECT_EQ(utilxx_base::toUnixStandardPath("\\a\\b/d\\"), "/a/b/d/");
    XX_TEST_EXPECT_EQ(
        utilxx_base::toUnixStandardPath("\\\\\\/\\/\\a/\\\\b\\\\\\/\\/d\\//\\/\\\\\\"),
        "/a/b/d/"
    );
}

void test_DirFilePath() {
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName(""), "");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("."), ".");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("..."), "...");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("...///\\\\"), "...");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("/"), "/");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("/////"), "/////");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("\\"), "\\");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("\\\\\\\\\\"), "\\\\\\\\\\");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("///\\\\\\\\//\\\\"), "///\\\\\\\\//\\\\");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName(".", true), ".");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("./.", true), ".");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("abc/..", true), "..");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("abc..123", true), "abc.");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("abc.123.tar.gz"), "abc.123.tar.gz");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("123"), "123");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("123/"), "123");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("123\\"), "123");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("./123"), "123");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName(".\\123"), "123");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("./123.456/", true), "123.456");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("\\//455//\\\\123/\\\\//\\/\\\\\\\\"), "123");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName(".///\\\\//\\\\/\\\\123"), "123");
    XX_TEST_EXPECT_EQ(
        utilxx_base::getFileName("///\\\\//\\\\/\\\\\\\\//"),
        "///\\\\//\\\\/\\\\\\\\//"
    );
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName(".///\\\\//\\\\/\\\\\\\\//"), ".");

    XX_TEST_EXPECT_NULLOPT(utilxx_base::getFileNameEXT(""));
    XX_TEST_EXPECT_NULLOPT(utilxx_base::getFileNameEXT("."));
    XX_TEST_EXPECT_NULLOPT(utilxx_base::getFileNameEXT("..."));
    XX_TEST_EXPECT_EQ(utilxx_base::getFileNameEXT("abc.name").value(), "name");
    XX_TEST_EXPECT_NULLOPT(utilxx_base::getFileNameEXT("abc.name/"));
    XX_TEST_EXPECT_NULLOPT(utilxx_base::getFileNameEXT("abc.name\\"));
    XX_TEST_EXPECT_NULLOPT(utilxx_base::getFileNameEXT("./../..."));
    XX_TEST_EXPECT_EQ(utilxx_base::getFileNameEXT("./../...name").value(), "name");
    XX_TEST_EXPECT_NULLOPT(utilxx_base::getFileNameEXT("./../name..."));

    XX_TEST_EXPECT_EQ(utilxx_base::replaceOrAppendExt("hello", "wav"), "hello.wav");
    XX_TEST_EXPECT_EQ(utilxx_base::replaceOrAppendExt("hello.mp3", "wav"), "hello.wav");
    XX_TEST_EXPECT_EQ(utilxx_base::replaceOrAppendExt("hello.f", "wav"), "hello.wav");
    XX_TEST_EXPECT_EQ(utilxx_base::replaceOrAppendExt("hello.flac", "wav"), "hello.wav");
    XX_TEST_EXPECT_EQ(utilxx_base::replaceOrAppendExt("hello.", "wav"), "hello.wav");
    XX_TEST_EXPECT_EQ(utilxx_base::replaceOrAppendExt(".hello", "wav"), ".hello.wav");
    XX_TEST_EXPECT_EQ(utilxx_base::replaceOrAppendExt(".hello.", "wav"), ".hello.wav");

    XX_TEST_EXPECT_NULLOPT(utilxx_base::getParentDirPath(""));
    XX_TEST_EXPECT_NULLOPT(utilxx_base::getParentDirPath("."));
    XX_TEST_EXPECT_NULLOPT(utilxx_base::getParentDirPath("..."));
    XX_TEST_EXPECT_NULLOPT(utilxx_base::getParentDirPath("...xx./"));
    XX_TEST_EXPECT_EQ(utilxx_base::getParentDirPath("/...xx.").value(), "/");
    XX_TEST_EXPECT_EQ(utilxx_base::getParentDirPath("/...xx./").value(), "/");
    XX_TEST_EXPECT_EQ(utilxx_base::getParentDirPath("/...xx./xxx").value(), "/...xx./");
    XX_TEST_EXPECT_EQ(utilxx_base::getParentDirPath("./xxx").value(), "./");
    XX_TEST_EXPECT_EQ(utilxx_base::getParentDirPath("../xxx").value(), "../");
    XX_TEST_EXPECT_EQ(
        utilxx_base::getParentDirPath("/absolute/dir/file.txt").value(),
        "/absolute/dir/"
    );
    XX_TEST_EXPECT_EQ(utilxx_base::getParentDirPath("relative.txt").value_or(""), "");

    // 隐藏文件 / 多后缀 (定点行为, 防止边界推导被改坏)
    // - 隐藏文件 (以 '.' 开头且无其它 '.'): 整体视为文件名, removeEXT 不生效
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName(".gitignore", true), ".gitignore");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("/a/b/.gitignore", true), ".gitignore");
    // - 多后缀: removeEXT 只去掉最后一个扩展名
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("a.tar.gz", true), "a.tar");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("archive.tar.gz", false), "archive.tar.gz");
    // - 单后缀
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("a.txt", true), "a");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("/a/b/a.txt", true), "a");
    // - 目录分隔符结尾同样按"最后一段"处理
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("a/b/", true), "b");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("a/b//", true), "b");
    // - UTF-8 文件名不受影响
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("dir/文件.txt", true), "文件");
}

void test_removeSpace() {
    XX_TEST_EXPECT_EQ(utilxx_base::removeAllSpace(""), "");
    XX_TEST_EXPECT_EQ(utilxx_base::removeAllSpace("  \t \t     "), "");
    XX_TEST_EXPECT_EQ(utilxx_base::removeAllSpace("   1 2   3 "), "123");
    XX_TEST_EXPECT_EQ(utilxx_base::removeAllSpace("\t   1\t  \t2   3 \t"), "123");

    XX_TEST_EXPECT_NULLOPT(utilxx_base::removeAllSpaceMayNull(""));
    XX_TEST_EXPECT_NULLOPT(utilxx_base::removeAllSpaceMayNull("     "));
    XX_TEST_EXPECT_NULLOPT(utilxx_base::removeAllSpaceMayNull("\t  \t  \t   \t"));
    XX_TEST_EXPECT_EQ(utilxx_base::removeAllSpaceMayNull("   1 2   3 ").value(), "123");
    XX_TEST_EXPECT_EQ(utilxx_base::removeAllSpaceMayNull("\t   1\t  \t2   3 \t").value(), "123");

    XX_TEST_EXPECT_EQ(utilxx_base::removeBetweenSpace(""), "");
    XX_TEST_EXPECT_EQ(utilxx_base::removeBetweenSpace("  "), "");
    XX_TEST_EXPECT_EQ(utilxx_base::removeBetweenSpace("\t\t\t"), "");
    XX_TEST_EXPECT_EQ(utilxx_base::removeBetweenSpace("\t   \t      \t"), "");
    XX_TEST_EXPECT_EQ(utilxx_base::removeBetweenSpace("   1 2   3 "), "1 2   3");
    XX_TEST_EXPECT_EQ(utilxx_base::removeBetweenSpace("\t   1\t  \t2   3 \t"), "1\t  \t2   3");
    XX_TEST_EXPECT_EQ(
        utilxx_base::removeBetweenSpace(" \n \r  1 2   3 \n\r", false),
        "\n \r  1 2   3 \n\r"
    );
    XX_TEST_EXPECT_EQ(
        utilxx_base::removeBetweenSpace("\n \r  1 2   3\n\r", false),
        "\n \r  1 2   3\n\r"
    );
    XX_TEST_EXPECT_EQ(
        utilxx_base::removeBetweenSpace("\n \r  1 2   3\n\r  ", false),
        "\n \r  1 2   3\n\r"
    );
    XX_TEST_EXPECT_EQ(utilxx_base::removeBetweenSpace(" \n \r  1 2   3 \n\r"), "1 2   3");
    XX_TEST_EXPECT_EQ(utilxx_base::removeBetweenSpace("\n \r  1 2   3\n\r  "), "1 2   3");
    XX_TEST_EXPECT_EQ(
        utilxx_base::removeBetweenSpace("\n \r  1 2   3\n\r  ", true, false, true),
        "\n \r  1 2   3"
    );
    XX_TEST_EXPECT_EQ(
        utilxx_base::removeBetweenSpace("\n \r  1 2   3\n\r  ", true, true, false),
        "1 2   3\n\r  "
    );

    XX_TEST_EXPECT_NULLOPT(utilxx_base::removeBetweenSpaceMayNull(""));
    XX_TEST_EXPECT_NULLOPT(utilxx_base::removeBetweenSpaceMayNull("  "));
    XX_TEST_EXPECT_NULLOPT(utilxx_base::removeBetweenSpaceMayNull("\t\t\t"));
    XX_TEST_EXPECT_NULLOPT(utilxx_base::removeBetweenSpaceMayNull("\t   \t      \t"));
    XX_TEST_EXPECT_EQ(utilxx_base::removeBetweenSpaceMayNull("   1 2   3 ").value(), "1 2   3");
    XX_TEST_EXPECT_EQ(
        utilxx_base::removeBetweenSpaceMayNull("\t   1\t  \t2   3 \t").value(),
        "1\t  \t2   3"
    );
}

void test_isIgnoreCaseEqual() {
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseEqual("", ""));
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseEqual(" ", " "));
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseEqual("123abcABC", "123abcABC"));
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseEqual("123abcABC", "123ABCabc"));
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseEqual("abc", "AbC"));
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseEqual("abc\n", "AbC\n"));

    XX_TEST_EXPECT_FALSE(utilxx_base::isIgnoreCaseEqual("", "     "));
    XX_TEST_EXPECT_FALSE(utilxx_base::isIgnoreCaseEqual("abc\n\r", "ABC"));
}

void test_isIgnoreCaseContains() {
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseContains("", ""));
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseContains(" ", " "));
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseContains("   ", ""));
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseContains("123abcABC +++ ", "123abcABC"));
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseContains("abcAbC", "AbC"));
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseContains("AbCabc", "AbC"));
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseContains("abc\n1fdfaf56as", "AbC\n"));
    XX_TEST_EXPECT_TRUE(
        utilxx_base::isIgnoreCaseContains("  你 好 你 好AbC\n1fdfaf56as", "你 好AbC\n")
    );

    XX_TEST_EXPECT_FALSE(utilxx_base::isIgnoreCaseContains("123abcABC", "123abcABC +++ "));
    XX_TEST_EXPECT_FALSE(utilxx_base::isIgnoreCaseContains("", "     "));
    XX_TEST_EXPECT_FALSE(utilxx_base::isIgnoreCaseContains("你 好abc\n\r", "不 好ABC"));

    // 逐字符实现的行为定点 (等价于 "两侧 toLower 后 find"):
    // - 空模式恒命中; 模式长于被查找串恒不命中
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseContains("abc", ""));
    XX_TEST_EXPECT_FALSE(utilxx_base::isIgnoreCaseContains("", "a"));
    XX_TEST_EXPECT_FALSE(utilxx_base::isIgnoreCaseContains("abc", "abcd"));
    // - 命中位置在末尾 / 需要跳过前缀
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseContains("xxxxABC", "abc"));
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseContains("xxAbCxx", "abc"));
    // - 大小写折叠仅作用于 ASCII 字母 (非 ASCII 字节按原样比较)
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseContains("ÄÖÜ äöü", "ÄÖÜ"));
    XX_TEST_EXPECT_FALSE(utilxx_base::isIgnoreCaseContains("ÄÖÜ", "äöü"));
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseContains("中文 AbC", "abc"));

    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseContainsAny("", ""));
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseContainsAny(" ", " "));
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseContainsAny("", "     "));
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseContainsAny("123abcABC", "123abcABC"));
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseContainsAny(" dddabc", "AbC"));
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseContainsAny("AbC", " dddabc"));
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseContainsAny("ABCddd ", "AbC"));
    XX_TEST_EXPECT_TRUE(utilxx_base::isIgnoreCaseContainsAny("AbC", "ABCddd "));
    XX_TEST_EXPECT_TRUE(
        utilxx_base::isIgnoreCaseContainsAny("  你 好 你 好aBc\n1fdfaf56as", "你 好AbC\n")
    );
    XX_TEST_EXPECT_TRUE(
        utilxx_base::isIgnoreCaseContainsAny("你 好AbC\n", "  你 好 你 好aBc\n1fdfaf56as")
    );

    XX_TEST_EXPECT_FALSE(utilxx_base::isIgnoreCaseContainsAny("你  好abc", "不 好ABC"));
    XX_TEST_EXPECT_FALSE(utilxx_base::isIgnoreCaseContainsAny("你 好abc\n\r", "不 好ABC"));

    XX_TEST_EXPECT_TRUE(utilxx_base::isNotEmptyAndIgnoreCaseContainsAny(" ", " "));
    XX_TEST_EXPECT_TRUE(utilxx_base::isNotEmptyAndIgnoreCaseContainsAny("123abcABC", "123abcABC")
    );
    XX_TEST_EXPECT_TRUE(utilxx_base::isNotEmptyAndIgnoreCaseContainsAny(" dddabc", "AbC"));
    XX_TEST_EXPECT_TRUE(utilxx_base::isNotEmptyAndIgnoreCaseContainsAny("AbC", " dddabc"));
    XX_TEST_EXPECT_TRUE(utilxx_base::isNotEmptyAndIgnoreCaseContainsAny("ABCddd ", "AbC"));
    XX_TEST_EXPECT_TRUE(utilxx_base::isNotEmptyAndIgnoreCaseContainsAny("AbC", "ABCddd "));
    XX_TEST_EXPECT_TRUE(
        utilxx_base::isNotEmptyAndIgnoreCaseContainsAny("AbC\n1fdfaf56as", "AbC\n")
    );

    XX_TEST_EXPECT_FALSE(utilxx_base::isNotEmptyAndIgnoreCaseContainsAny("", ""));
    XX_TEST_EXPECT_FALSE(utilxx_base::isNotEmptyAndIgnoreCaseContainsAny("   ", ""));
    XX_TEST_EXPECT_FALSE(utilxx_base::isNotEmptyAndIgnoreCaseContainsAny("", "     "));
    XX_TEST_EXPECT_FALSE(utilxx_base::isNotEmptyAndIgnoreCaseContainsAny("你  好abc", "不 好ABC")
    );
    XX_TEST_EXPECT_FALSE(
        utilxx_base::isNotEmptyAndIgnoreCaseContainsAny("你 好abc\n\r", "不 好ABC")
    );
}

void test_toArgument() {
    XX_TEST_EXPECT_EQ(utilxx_base::toArgument(""), "\"\"");
    XX_TEST_EXPECT_EQ(utilxx_base::toArgument("\"\""), "\"\\\"\\\"\"");
    XX_TEST_EXPECT_EQ(
        utilxx_base::toArgument("{\"enable_thinking\": false}"),
        "\"{\\\"enable_thinking\\\": false}\""
    );
    XX_TEST_EXPECT_EQ(utilxx_base::toArgument("\"hh\", --"), "\"\\\"hh\\\", --\"");
    XX_TEST_EXPECT_EQ(utilxx_base::toArgument("\"\"\", --"), "\"\\\"\\\"\\\", --\"");
    XX_TEST_EXPECT_EQ(utilxx_base::toArgument("\\\"\"\\\", --"), "\"\\\"\\\"\\\", --\"");
    XX_TEST_EXPECT_EQ(utilxx_base::toArgument("\"\\\"\", --"), "\"\\\"\\\"\\\", --\"");
    XX_TEST_EXPECT_EQ(utilxx_base::toArgument("\"wow\""), "\"\\\"wow\\\"\"");
    XX_TEST_EXPECT_EQ(utilxx_base::toArgument("\\\"wow\\\""), "\"\\\"wow\\\"\"");

    // 回归: mark 前连续反斜杠的奇偶性决定是否转义
    // - 奇数个反斜杠 (\\"): mark 已被转义 (字面引号), 不再转义
    XX_TEST_EXPECT_EQ(utilxx_base::toArgument("a\\\"b"), "\"a\\\"b\"");
    // - 偶数个反斜杠 (\\\\"): mark 是新界定符, 必须转义
    XX_TEST_EXPECT_EQ(utilxx_base::toArgument("a\\\\\"b"), "\"a\\\\\\\"b\"");
    XX_TEST_EXPECT_EQ(utilxx_base::toArgument("\\\\\""), "\"\\\\\\\"\"");
    // - 奇数个反斜杠在串尾 + 后续 mark: 不转义
    XX_TEST_EXPECT_EQ(utilxx_base::toArgument("a\\\"\"b"), "\"a\\\"\\\"b\"");
}

void test_base64() {
    // 往返
    XX_TEST_EXPECT_EQ(utilxx_base::base64Encode(""), "");
    XX_TEST_EXPECT_EQ(utilxx_base::base64Encode("f"), "Zg==");
    XX_TEST_EXPECT_EQ(utilxx_base::base64Encode("fo"), "Zm8=");
    XX_TEST_EXPECT_EQ(utilxx_base::base64Encode("foo"), "Zm9v");
    XX_TEST_EXPECT_EQ(utilxx_base::base64Encode("foobar"), "Zm9vYmFy");

    auto roundTrip = [](const std::string& data) {
        auto encoded = utilxx_base::base64Encode(data);
        auto decoded = utilxx_base::base64Decode(encoded);
        XX_TEST_EXPECT_TRUE(decoded.has_value());
        if (decoded.has_value()) {
            XX_TEST_EXPECT_EQ(decoded.value(), data);
        }
    };
    roundTrip("");
    roundTrip("hello world");
    roundTrip(std::string("\x00\x01\x02\xff\xfe", 5)); // 含二进制/非 UTF-8 字节
    roundTrip(std::string(1000, 'x'));

    // 空输入合法, 解码为空结果 (has_value 且为空)
    auto emptyDec = utilxx_base::base64Decode("");
    XX_TEST_EXPECT_TRUE(emptyDec.has_value());
    if (emptyDec.has_value()) {
        XX_TEST_EXPECT_EQ(emptyDec.value(), "");
    }

    // 合法 base64
    auto validDec = utilxx_base::base64Decode("Zm9vYmFy");
    XX_TEST_EXPECT_TRUE(validDec.has_value());
    if (validDec.has_value()) {
        XX_TEST_EXPECT_EQ(validDec.value(), "foobar");
    }

    // 未补齐 padding 的合法 base64 (长度 mod4 ∈ {2,3}) 也必须正确解码
    // (修复前 decoded_size 分配不足会导致堆越界写)
    {
        auto d2 = utilxx_base::base64Decode("Zg"); // "f"
        XX_TEST_EXPECT_TRUE(d2.has_value());
        if (d2.has_value()) {
            XX_TEST_EXPECT_EQ(d2.value(), "f");
        }
        auto d3 = utilxx_base::base64Decode("Zm8"); // "fo"
        XX_TEST_EXPECT_TRUE(d3.has_value());
        if (d3.has_value()) {
            XX_TEST_EXPECT_EQ(d3.value(), "fo");
        }
        auto d6 = utilxx_base::base64Decode("Zm9vYg"); // "foob"
        XX_TEST_EXPECT_TRUE(d6.has_value());
        if (d6.has_value()) {
            XX_TEST_EXPECT_EQ(d6.value(), "foob");
        }
        auto d7 = utilxx_base::base64Decode("Zm9vYmE"); // "fooba"
        XX_TEST_EXPECT_TRUE(d7.has_value());
        if (d7.has_value()) {
            XX_TEST_EXPECT_EQ(d7.value(), "fooba");
        }
    }

    // 大尺寸未补齐 padding 输入往返 (修复前会堆溢出): 编码后去掉 '=' 再解码
    {
        std::string large(1000, 'x');
        auto        encoded = utilxx_base::base64Encode(large);
        while (!encoded.empty() && encoded.back() == '=') {
            encoded.pop_back();
        }
        auto decoded = utilxx_base::base64Decode(encoded);
        XX_TEST_EXPECT_TRUE(decoded.has_value());
        if (decoded.has_value()) {
            XX_TEST_EXPECT_EQ(decoded.value(), large);
        }
    }

    // 非法输入必须返回 nullopt (修复: 不能与空结果混淆)
    XX_TEST_EXPECT_FALSE(utilxx_base::base64Decode("!!!not base64!!!").has_value());
    XX_TEST_EXPECT_FALSE(utilxx_base::base64Decode("====").has_value()); // 仅 padding
    XX_TEST_EXPECT_FALSE(utilxx_base::base64Decode("Zm9vY").has_value()); // 数据长度 mod4==1 非法
    XX_TEST_EXPECT_FALSE(utilxx_base::base64Decode("Zm=v").has_value()); // padding 位置非法
    XX_TEST_EXPECT_FALSE(utilxx_base::base64Decode("Zm9v YmFy").has_value()); // 含空格非法

    // 短输入含 '=' 时不应触发 size_t 下溢
    XX_TEST_EXPECT_FALSE(utilxx_base::base64Decode("=").has_value());
    XX_TEST_EXPECT_FALSE(utilxx_base::base64Decode("==").has_value());
    XX_TEST_EXPECT_FALSE(utilxx_base::base64Decode("===").has_value());
    XX_TEST_EXPECT_FALSE(utilxx_base::base64Decode("Z=").has_value()); // 数据长度 mod4==1
}

void test_convertCharset() {
    // 相同编码: 成功且无需转换 (nullopt)
    {
        auto [ok, res] = utilxx_base::convertCharset("hello", "UTF-8", "UTF-8");
        XX_TEST_EXPECT_TRUE(ok);
        XX_TEST_EXPECT_FALSE(res.has_value());
    }
    // 空输入: 失败
    {
        auto [ok, res] = utilxx_base::convertCharset("", "GB18030", "UTF-8");
        XX_TEST_EXPECT_FALSE(ok);
        XX_TEST_EXPECT_FALSE(res.has_value());
    }
    // 非法编码名: 失败 (修复: 不能谎报成功)
    {
        auto [ok, res] = utilxx_base::convertCharset("hello", "NOT_A_REAL_ENCODING_XYZ", "UTF-8");
        XX_TEST_EXPECT_FALSE(ok);
        XX_TEST_EXPECT_FALSE(res.has_value());
    }
    // GBK/GB18030 -> UTF-8: "中文" 的 GBK 字节为 D6 D0 CE C4
    {
        std::string gbkBytes = std::string("\xD6\xD0\xCE\xC4", 4);
        auto [ok, res]       = utilxx_base::convertCharset(gbkBytes, "GB18030", "UTF-8");
        XX_TEST_EXPECT_TRUE(ok);
        XX_TEST_EXPECT_TRUE(res.has_value());
        if (res.has_value()) {
            XX_TEST_EXPECT_EQ(res.value(), "中文");
        }
    }
    // autoConvertToUtf8 (in-place): GBK -> UTF-8
    {
        std::string gbkBytes = std::string("\xD6\xD0\xCE\xC4", 4);
        bool        ok       = utilxx_base::autoConvertToUtf8(gbkBytes);
        XX_TEST_EXPECT_TRUE(ok);
        XX_TEST_EXPECT_EQ(gbkBytes, "中文");
    }
}

void test_utf8Check() {
    // 合法 UTF-8
    XX_TEST_EXPECT_EQ(utilxx_base::utf8GetLengthCheckAvail("hello"), 5u);
    XX_TEST_EXPECT_EQ(utilxx_base::utf8GetLengthCheckAvail("中文"), 2u);
    XX_TEST_EXPECT_EQ(utilxx_base::utf8GetLengthCheckAvail("a中b文c"), 5u);
    XX_TEST_EXPECT_TRUE(utilxx_base::utf8IsAvail("中文abc"));

    // 非法/截断 UTF-8 返回 0
    XX_TEST_EXPECT_EQ(utilxx_base::utf8GetLengthCheckAvail(std::string("\xC0\x80", 2)), 0u);
    XX_TEST_EXPECT_EQ(utilxx_base::utf8GetLengthCheckAvail(std::string("\xFF\xFE", 2)), 0u);
    XX_TEST_EXPECT_EQ(
        utilxx_base::utf8GetLengthCheckAvail(std::string("\xE4\xB8", 2)),
        0u
    ); // 截断的 3 字节序列
    XX_TEST_EXPECT_FALSE(utilxx_base::utf8IsAvail(std::string("\xFF\xFE", 2)));
    XX_TEST_EXPECT_TRUE(utilxx_base::utf8IsAvail(""));

    // 含内嵌 '\0': 在 '\0' 处停止计数 (修复 #8: 直接传 string_view 而非 str.data())
    {
        std::string withNull("ab\0cd", 5);
        // 在 '\0' 处 break, 计到 "ab" 共 2 个字符
        XX_TEST_EXPECT_EQ(utilxx_base::utf8GetLengthCheckAvail(withNull), 2u);
    }

    // 更多边界: 5/6 字节头非法; 非最短编码 (0xF0 0x80..) 非法;
    // 替换字符 EF BF BD 合法 (部分转换需要用 U+FFFD 替代非法编码, utf8Repair 的产物必须被视为合法)
    XX_TEST_EXPECT_EQ(
        utilxx_base::utf8GetLengthCheckAvail(std::string("\xF8\x88\x80\x80\x80", 5)),
        0u
    );
    XX_TEST_EXPECT_EQ(
        utilxx_base::utf8GetLengthCheckAvail(std::string("\xFC\x84\x80\x80\x80\x80", 6)),
        0u
    );
    XX_TEST_EXPECT_EQ(
        utilxx_base::utf8GetLengthCheckAvail(std::string("\xF0\x80\x80\x80", 4)),
        0u
    );
    XX_TEST_EXPECT_EQ(utilxx_base::utf8GetLengthCheckAvail(std::string("\xE0\x80\x80", 3)), 0u);
    XX_TEST_EXPECT_EQ(utilxx_base::utf8GetLengthCheckAvail(std::string("\xEF\xBF\xBD", 3)), 1u);
    // 合法 4 字节 (emoji)
    XX_TEST_EXPECT_EQ(
        utilxx_base::utf8GetLengthCheckAvail(std::string("\xF0\x9F\x98\x80", 4)),
        1u
    );
    XX_TEST_EXPECT_TRUE(utilxx_base::utf8IsAvail(std::string("\xF0\x9F\x98\x80", 4)));
    // 截断的 4 字节序列
    XX_TEST_EXPECT_EQ(utilxx_base::utf8GetLengthCheckAvail(std::string("\xF0\x9F\x98", 3)), 0u);
    // 连续字符计数
    XX_TEST_EXPECT_EQ(
        utilxx_base::utf8GetLengthCheckAvail(std::string("\xE4\xB8\xAD\xE6\x96\x87", 6)),
        2u
    );
}

void test_utf8Repair() {
    const std::string replacement("\xEF\xBF\xBD"); // U+FFFD 的 UTF-8 编码

    // 合法输入: 不做修改, 返回 false
    {
        std::string s = "hello中文";
        XX_TEST_EXPECT_FALSE(utilxx_base::utf8Repair(s));
        XX_TEST_EXPECT_EQ(s, "hello中文");

        std::string empty;
        XX_TEST_EXPECT_FALSE(utilxx_base::utf8Repair(empty));
        XX_TEST_EXPECT_TRUE(empty.empty());

        std::string emoji("\xF0\x9F\x98\x80", 4); // 合法 4 字节
        XX_TEST_EXPECT_FALSE(utilxx_base::utf8Repair(emoji));
        XX_TEST_EXPECT_EQ(emoji, std::string("\xF0\x9F\x98\x80", 4));
    }

    // 孤立延续字节: 替换为单个 U+FFFD
    {
        std::string s("\x80", 1);
        XX_TEST_EXPECT_TRUE(utilxx_base::utf8Repair(s));
        XX_TEST_EXPECT_EQ(s, replacement);

        std::string s2("\x80\x81", 2);
        XX_TEST_EXPECT_TRUE(utilxx_base::utf8Repair(s2));
        XX_TEST_EXPECT_EQ(s2, replacement + replacement);
    }

    // 末尾截断的多字节序列: 整体替换为一个 U+FFFD
    {
        std::string s("\xE4\xB8", 2); // 截断的 3 字节序列
        XX_TEST_EXPECT_TRUE(utilxx_base::utf8Repair(s));
        XX_TEST_EXPECT_EQ(s, replacement);

        std::string s4("\xF0\x9F\x98", 3); // 截断的 4 字节序列
        XX_TEST_EXPECT_TRUE(utilxx_base::utf8Repair(s4));
        XX_TEST_EXPECT_EQ(s4, replacement);
    }

    // 过短编码头 0xC0/0xC1: 按 maximal subpart 逐字节替换
    {
        std::string s("\xC0\x80", 2);
        XX_TEST_EXPECT_TRUE(utilxx_base::utf8Repair(s));
        XX_TEST_EXPECT_EQ(s, replacement + replacement);
    }

    // 非最短编码序列: 前导字节替换, 后续孤立字节各自替换
    {
        std::string s("\xE0\x80\x80", 3);
        XX_TEST_EXPECT_TRUE(utilxx_base::utf8Repair(s));
        XX_TEST_EXPECT_EQ(s, replacement + replacement + replacement);

        std::string s4("\xF0\x80\x80\x80", 4);
        XX_TEST_EXPECT_TRUE(utilxx_base::utf8Repair(s4));
        XX_TEST_EXPECT_EQ(s4, replacement + replacement + replacement + replacement);
    }

    // 5/6 字节头及无效头: 逐字节替换
    {
        std::string s("\xF8\x88\x80\x80\x80", 5);
        XX_TEST_EXPECT_TRUE(utilxx_base::utf8Repair(s));
        XX_TEST_EXPECT_EQ(s, replacement + replacement + replacement + replacement + replacement);

        std::string s2("\xFF\xFE", 2);
        XX_TEST_EXPECT_TRUE(utilxx_base::utf8Repair(s2));
        XX_TEST_EXPECT_EQ(s2, replacement + replacement);
    }

    // 混合: 合法部分保留, 非法被替换且不吞掉后续合法字符
    {
        std::string s  = "a";
        s             += "\xFF";
        s             += " b中";
        XX_TEST_EXPECT_TRUE(utilxx_base::utf8Repair(s));
        XX_TEST_EXPECT_EQ(s, "a" + replacement + " b中");
    }

    // 截断序列后紧跟合法字符: 合法字符不被吞掉
    {
        std::string s("\xE4\xB8", 2);
        s += "A中";
        XX_TEST_EXPECT_TRUE(utilxx_base::utf8Repair(s));
        XX_TEST_EXPECT_EQ(s, replacement + "A中");
    }

    // 多字节序列中途出现非法字节: 序列前缀替换, 后续合法序列保留
    {
        std::string s("\xE4\xB8\xAD", 3); // 中
        s += "\xFF";
        s += "\xE6\x96\x87"; // 文
        XX_TEST_EXPECT_TRUE(utilxx_base::utf8Repair(s));
        XX_TEST_EXPECT_EQ(
            s,
            std::string("\xE4\xB8\xAD", 3) + replacement + std::string("\xE6\x96\x87", 3)
        );
    }

    // 幂等: 修复后结果为合法 UTF-8, 再次调用返回 false 且不再修改
    {
        std::string s("\xE4\xB8\xFF", 3);
        XX_TEST_EXPECT_TRUE(utilxx_base::utf8Repair(s));
        XX_TEST_EXPECT_TRUE(utilxx_base::utf8IsAvail(s));
        const std::string repaired = s;
        XX_TEST_EXPECT_FALSE(utilxx_base::utf8Repair(s));
        XX_TEST_EXPECT_EQ(s, repaired);
    }
}

void test_compareExtend_pinyin() {
    // 中文拼音比较依赖全局 s_pinyinCallback; 设置后测试并恢复
    auto oldCallback                = utilxx_base::s_pinyinCallback;
    utilxx_base::s_pinyinCallback = [](std::string_view str) -> std::string {
        if (str.starts_with("你")) {
            return "ni";
        }
        if (str.starts_with("我")) {
            return "wo";
        }
        if (str.starts_with("七")) {
            return "qi";
        }
        if (str.starts_with("八")) {
            return "ba";
        }
        if (str.starts_with("二")) {
            return "er";
        }
        if (str.starts_with("九")) {
            return "jiu";
        }
        return "";
    };

    // 中文按拼音比较: 你(ni) < 我(wo)
    XX_TEST_EXPECT_TRUE(utilxx_base::compareExtend("你", "我") < 0);
    XX_TEST_EXPECT_TRUE(utilxx_base::compareExtend("我", "你") > 0);
    // 首字相同, 次字按拼音: 八(ba) < 二(er)
    XX_TEST_EXPECT_TRUE(utilxx_base::compareExtend("七八九", "七二一") < 0);
    XX_TEST_EXPECT_TRUE(utilxx_base::compareExtend("七二一", "七八九") > 0);
    // 中文与数字: 数字优先
    XX_TEST_EXPECT_TRUE(utilxx_base::compareExtend("1", "你") < 0);
    XX_TEST_EXPECT_TRUE(utilxx_base::compareExtend("你", "1") > 0);
    // 中文与英文拼音首字母
    XX_TEST_EXPECT_TRUE(utilxx_base::compareExtend("你", "a") > 0); // n > a
    XX_TEST_EXPECT_TRUE(utilxx_base::compareExtend("你", "z") < 0); // n < z
    // 混排场景 (原注释掉的用例)
    shiftCompareExtend(" #= 你 77", " #= 你 234", 77 - 234);

    utilxx_base::s_pinyinCallback = oldCallback;
}

void test_toUpperLower() {
    XX_TEST_EXPECT_EQ(utilxx_base::toUpper(""), "");
    XX_TEST_EXPECT_EQ(utilxx_base::toUpper("aBc123!@#"), "ABC123!@#");
    XX_TEST_EXPECT_EQ(utilxx_base::toUpper("中文abc"), "中文ABC");
    XX_TEST_EXPECT_EQ(utilxx_base::toLower(""), "");
    XX_TEST_EXPECT_EQ(utilxx_base::toLower("AbC123!@#"), "abc123!@#");
    XX_TEST_EXPECT_EQ(utilxx_base::toLower("中文ABC"), "中文abc");

    // in-place 版本
    std::string s1 = "aBc123";
    utilxx_base::toUpperSelf(s1);
    XX_TEST_EXPECT_EQ(s1, "ABC123");
    std::string s2 = "AbC123";
    utilxx_base::toLowerSelf(s2);
    XX_TEST_EXPECT_EQ(s2, "abc123");
}

void test_charOps() {
    XX_TEST_EXPECT_EQ(utilxx_base::charToLower('A'), 'a');
    XX_TEST_EXPECT_EQ(utilxx_base::charToLower('z'), 'z');
    XX_TEST_EXPECT_EQ(utilxx_base::charToLower('1'), '1');
    XX_TEST_EXPECT_EQ(utilxx_base::charToUpper('a'), 'A');
    XX_TEST_EXPECT_EQ(utilxx_base::charToUpper('Z'), 'Z');
    XX_TEST_EXPECT_EQ(utilxx_base::charToUpper('1'), '1');

    XX_TEST_EXPECT_TRUE(utilxx_base::charIsSpace(' '));
    XX_TEST_EXPECT_TRUE(utilxx_base::charIsSpace('\t'));
    XX_TEST_EXPECT_TRUE(utilxx_base::charIsSpace('\n'));
    XX_TEST_EXPECT_TRUE(utilxx_base::charIsSpace('\r'));
    XX_TEST_EXPECT_TRUE(utilxx_base::charIsSpace('\v'));
    XX_TEST_EXPECT_TRUE(utilxx_base::charIsSpace('\f'));
    XX_TEST_EXPECT_FALSE(utilxx_base::charIsSpace('a'));
    XX_TEST_EXPECT_FALSE(utilxx_base::charIsSpace('0'));

    XX_TEST_EXPECT_TRUE(utilxx_base::isCode_num('0'));
    XX_TEST_EXPECT_TRUE(utilxx_base::isCode_num('9'));
    XX_TEST_EXPECT_FALSE(utilxx_base::isCode_num('a'));
    XX_TEST_EXPECT_TRUE(utilxx_base::isCode_AZ('A'));
    XX_TEST_EXPECT_TRUE(utilxx_base::isCode_AZ('Z'));
    XX_TEST_EXPECT_FALSE(utilxx_base::isCode_AZ('a'));
    XX_TEST_EXPECT_TRUE(utilxx_base::isCode_az('a'));
    XX_TEST_EXPECT_TRUE(utilxx_base::isCode_az('z'));
    XX_TEST_EXPECT_FALSE(utilxx_base::isCode_az('A'));
    XX_TEST_EXPECT_TRUE(utilxx_base::isCode_AZaz('a'));
    XX_TEST_EXPECT_TRUE(utilxx_base::isCode_AZaz('Z'));
    XX_TEST_EXPECT_FALSE(utilxx_base::isCode_AZaz('1'));

    XX_TEST_EXPECT_EQ(utilxx_base::toCode_tryAZ('a').value(), 'A');
    XX_TEST_EXPECT_EQ(utilxx_base::toCode_tryAZ('A').value(), 'A');
    XX_TEST_EXPECT_NULLOPT(utilxx_base::toCode_tryAZ('1'));
    XX_TEST_EXPECT_EQ(utilxx_base::toCode_tryaz('A').value(), 'a');
    XX_TEST_EXPECT_EQ(utilxx_base::toCode_tryaz('a').value(), 'a');
    XX_TEST_EXPECT_NULLOPT(utilxx_base::toCode_tryaz('1'));
    XX_TEST_EXPECT_EQ(utilxx_base::toCode_mayAZ('b'), 'B');
    XX_TEST_EXPECT_EQ(utilxx_base::toCode_mayAZ('1'), '1');
    XX_TEST_EXPECT_EQ(utilxx_base::toCode_mayaz('B'), 'b');
    XX_TEST_EXPECT_EQ(utilxx_base::toCode_mayaz('1'), '1');
    XX_TEST_EXPECT_EQ(utilxx_base::toCode_AZ('a'), 'A');
    XX_TEST_EXPECT_EQ(utilxx_base::toCode_AZ('A'), 'A');
    XX_TEST_EXPECT_EQ(utilxx_base::toCode_az('A'), 'a');
    XX_TEST_EXPECT_EQ(utilxx_base::toCode_az('a'), 'a');
}

void test_utf8GetLength() {
    XX_TEST_EXPECT_EQ(utilxx_base::utf8GetLength(""), 0u);
    XX_TEST_EXPECT_EQ(utilxx_base::utf8GetLength("abc"), 3u);
    XX_TEST_EXPECT_EQ(utilxx_base::utf8GetLength("中文abc"), 5u);
    // emoji 4 字节
    XX_TEST_EXPECT_EQ(utilxx_base::utf8GetLength(std::string("\xF0\x9F\x98\x80", 4)), 1u);
    // 混合
    XX_TEST_EXPECT_EQ(
        utilxx_base::utf8GetLength(std::string("a\xE4\xB8\xAD\xF0\x9F\x98\x80", 6)),
        3u
    );
}

void test_findIndexByUtf8Length() {
    // "中文abc": 中(3) 文(3) a b c -> 字节偏移 0,3,6,7,8,9
    XX_TEST_EXPECT_EQ(utilxx_base::findIndexByUtf8Length("中文abc", 1), 3u);
    XX_TEST_EXPECT_EQ(utilxx_base::findIndexByUtf8Length("中文abc", 2), 6u);
    XX_TEST_EXPECT_EQ(utilxx_base::findIndexByUtf8Length("中文abc", 3), 7u);
    XX_TEST_EXPECT_EQ(utilxx_base::findIndexByUtf8Length("中文abc", 5), 9u);
    // 超出长度: 返回 0
    XX_TEST_EXPECT_EQ(utilxx_base::findIndexByUtf8Length("中文abc", 6), 0u);
    // 空输入
    XX_TEST_EXPECT_EQ(utilxx_base::findIndexByUtf8Length("", 1), 0u);
    // 从 start 偏移开始
    XX_TEST_EXPECT_EQ(utilxx_base::findIndexByUtf8Length("中文abc", 1, 3), 6u);
}

void test_findIndexAndLastLineIndexByUtf8Length() {
    // "a\nb\nc": a(0) \n(1) b(2) \n(3) c(4)
    auto r1 = utilxx_base::findIndexAndLastLineIndexByUtf8Length("a\nb\nc", 3);
    XX_TEST_EXPECT_EQ(std::get<0>(r1), 3u); // 第3字符 'b' 后
    XX_TEST_EXPECT_EQ(std::get<1>(r1), 1u); // 遇到 1 个换行
    XX_TEST_EXPECT_EQ(std::get<2>(r1), 1u); // 最后换行在 index 1

    // 超出长度: 返回 {0,0,0}
    auto r2 = utilxx_base::findIndexAndLastLineIndexByUtf8Length("a\nb\nc", 10);
    XX_TEST_EXPECT_EQ(std::get<0>(r2), 0u);
    XX_TEST_EXPECT_EQ(std::get<1>(r2), 0u);
    XX_TEST_EXPECT_EQ(std::get<2>(r2), 0u);

    // 空输入
    auto r3 = utilxx_base::findIndexAndLastLineIndexByUtf8Length("", 1);
    XX_TEST_EXPECT_EQ(std::get<0>(r3), 0u);

    // 精确取到换行符本身
    auto r4 = utilxx_base::findIndexAndLastLineIndexByUtf8Length("a\nb", 2);
    XX_TEST_EXPECT_EQ(std::get<0>(r4), 2u);
    XX_TEST_EXPECT_EQ(std::get<1>(r4), 1u);
    XX_TEST_EXPECT_EQ(std::get<2>(r4), 1u);
}

void test_countLines() {
    // 空输入
    XX_TEST_EXPECT_EQ(utilxx_base::countLines(""), 0u);
    // 单个换行符: 前面空行, 末尾换行后无内容 -> 1 行
    XX_TEST_EXPECT_EQ(utilxx_base::countLines("\n"), 1u);
    // 多行且末尾无换行: 最后一个不完整行也算一行
    XX_TEST_EXPECT_EQ(utilxx_base::countLines("a\nb\nc"), 3u);
    // 末尾有换行: '\n' 数量即行数
    XX_TEST_EXPECT_EQ(utilxx_base::countLines("a\nb\nc\n"), 3u);
    // 连续换行: 空行也计数
    XX_TEST_EXPECT_EQ(utilxx_base::countLines("\n\n"), 2u);
    XX_TEST_EXPECT_EQ(utilxx_base::countLines("a\n\nb"), 3u);
    // 无换行: 单行
    XX_TEST_EXPECT_EQ(utilxx_base::countLines("hello"), 1u);
    // 仅末尾换行前有多行内容
    XX_TEST_EXPECT_EQ(utilxx_base::countLines("line1\nline2\nline3\nline4"), 4u);
}

void test_strSplit() {
    auto r1 = utilxx_base::strSplit("a,b,c", ',');
    XX_TEST_EXPECT_EQ(r1.size(), (size_t)3);
    XX_TEST_EXPECT_EQ(r1[0], std::string_view("a"));
    XX_TEST_EXPECT_EQ(r1[1], std::string_view("b"));
    XX_TEST_EXPECT_EQ(r1[2], std::string_view("c"));

    // 空串: split 空输入产生空结果 (无元素)
    auto r2 = utilxx_base::strSplit("", ',');
    XX_TEST_EXPECT_EQ(r2.size(), (size_t)0);

    // 单元素无分隔符
    auto r2b = utilxx_base::strSplit("a", ',');
    XX_TEST_EXPECT_EQ(r2b.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(r2b[0], std::string_view("a"));

    // 连续分隔符 -> 空元素
    auto r3 = utilxx_base::strSplit("a,,b", ',');
    XX_TEST_EXPECT_EQ(r3.size(), (size_t)3);
    XX_TEST_EXPECT_EQ(r3[1], std::string_view(""));

    // 首尾分隔符
    auto r4 = utilxx_base::strSplit(",a,", ',');
    XX_TEST_EXPECT_EQ(r4.size(), (size_t)3);
    XX_TEST_EXPECT_EQ(r4[0], std::string_view(""));
    XX_TEST_EXPECT_EQ(r4[2], std::string_view(""));

    // 无分隔符
    auto r5 = utilxx_base::strSplit("abc", ',');
    XX_TEST_EXPECT_EQ(r5.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(r5[0], std::string_view("abc"));

    // 拷贝版本
    auto r6_new = utilxx_base::strSplitCopied("x/y/z", '/');
    XX_TEST_EXPECT_EQ(r6_new.size(), (size_t)3);
    XX_TEST_EXPECT_EQ(r6_new[0], std::string("x"));
    XX_TEST_EXPECT_EQ(r6_new[2], std::string("z"));
}

void test_stringVectorJoin() {
    XX_TEST_EXPECT_EQ(utilxx_base::stringVectorJoin(std::vector<std::string>{}), "");
    XX_TEST_EXPECT_EQ(
        utilxx_base::stringVectorJoin(std::vector<std::string>{"a", "b", "c"}),
        "a, b, c"
    );
    XX_TEST_EXPECT_EQ(utilxx_base::stringVectorJoin(std::vector<std::string>{"a"}, "-"), "a");
    // 非字符串元素
    XX_TEST_EXPECT_EQ(utilxx_base::stringVectorJoin(std::vector<int>{1, 2, 3}, "-"), "1-2-3");

    // stringJoin: 泛型 input_range
    std::vector<std::string> vec{"x", "y", "z"};
    XX_TEST_EXPECT_EQ(utilxx_base::stringJoin(vec, "/"), "x/y/z");
    std::vector<int> nums{10, 20, 30};
    XX_TEST_EXPECT_EQ(utilxx_base::stringJoin(nums, ":"), "10:20:30");

    // normalizeCrlfToLf / normalizeLfToCrlf
    std::string text1 = "line1\r\nline2\r\nline3";
    utilxx_base::normalizeCrlfToLf(text1);
    XX_TEST_EXPECT_EQ(text1, "line1\nline2\nline3");

    std::string text2 = "line1\nline2\nline3";
    utilxx_base::normalizeLfToCrlf(text2);
    XX_TEST_EXPECT_EQ(text2, "line1\r\nline2\r\nline3");
}

void test_toStringNotNull() {
    XX_TEST_EXPECT_EQ(utilxx_base::toStringNotNull(nullptr), std::string_view(""));
    XX_TEST_EXPECT_EQ(utilxx_base::toStringNotNull("abc"), std::string_view("abc"));
    XX_TEST_EXPECT_EQ(utilxx_base::toStringNotNull(""), std::string_view(""));
}

void test_parseNumberFromString() {
    int    iv = 0;
    double dv = 0.0;
    auto   r1 = utilxx_base::parseNumberFromString("123", iv);
    XX_TEST_EXPECT_EQ(r1.ec, std::errc{});
    XX_TEST_EXPECT_EQ(iv, 123);

    // 前导空格: from_chars 不接受 -> 失败
    auto r2 = utilxx_base::parseNumberFromString("  123", iv);
    XX_TEST_EXPECT_TRUE(r2.ec != std::errc{});

    // 部分解析: "12a" 解析出 12, ptr 停在 'a'
    auto r3 = utilxx_base::parseNumberFromString("12a", iv);
    XX_TEST_EXPECT_EQ(r3.ec, std::errc{});
    XX_TEST_EXPECT_EQ(iv, 12);
    XX_TEST_EXPECT_EQ(std::string_view{r3.ptr}, std::string_view("12a").data() + 2);

    // 非法输入
    auto r4 = utilxx_base::parseNumberFromString("abc", iv);
    XX_TEST_EXPECT_TRUE(r4.ec != std::errc{});

    // double 解析
    auto r5 = utilxx_base::parseNumberFromString("3.14", dv);
    XX_TEST_EXPECT_EQ(r5.ec, std::errc{});
    XX_TEST_EXPECT_TRUE(dv > 3.13 && dv < 3.15);

    // 空串
    auto r6 = utilxx_base::parseNumberFromString("", iv);
    XX_TEST_EXPECT_TRUE(r6.ec != std::errc{});

    // 负数和溢出
    int  neg = 0;
    auto r7  = utilxx_base::parseNumberFromString("-42", neg);
    XX_TEST_EXPECT_EQ(r7.ec, std::errc{});
    XX_TEST_EXPECT_EQ(neg, -42);
    int  ov = 0;
    auto r8 = utilxx_base::parseNumberFromString("99999999999999999999", ov);
    XX_TEST_EXPECT_TRUE(r8.ec == std::errc::result_out_of_range);

    // 浮点边界 (M1-6 回归: libc++ 回退实现曾按 errno 判定失败, 导致下溢的
    // 极小值 (1e-320) 被误判为非法 —— 下溢返回的仍是可用数值, 只有上溢
    // (±inf) 与无法完整消费的输入才算失败)
    double tiny = 0.0;
    auto   r9   = utilxx_base::parseNumberFromString("1e-320", tiny);
    XX_TEST_EXPECT_EQ(r9.ec, std::errc{});
    XX_TEST_EXPECT_TRUE(tiny >= 0.0 && tiny < 1e-300);

    double big = 0.0;
    auto   r10 = utilxx_base::parseNumberFromString("1e400", big);
    XX_TEST_EXPECT_TRUE(r10.ec != std::errc{});

    double neg0 = 1.0;
    auto   r11  = utilxx_base::parseNumberFromString("-1e-320", neg0);
    XX_TEST_EXPECT_EQ(r11.ec, std::errc{});
    XX_TEST_EXPECT_TRUE(neg0 <= 0.0);

    double pi  = 0.0;
    auto   r12 = utilxx_base::parseNumberFromString("3.141592653589793", pi);
    XX_TEST_EXPECT_EQ(r12.ec, std::errc{});
    XX_TEST_EXPECT_EQ(pi, 3.141592653589793);
}

void test_formatSize() {
    XX_TEST_EXPECT_EQ(utilxx_base::formatSize(0), std::string("0"));
    XX_TEST_EXPECT_EQ(utilxx_base::formatSize(1), std::string("1"));
    XX_TEST_EXPECT_EQ(utilxx_base::formatSize(999), std::string("999"));
    XX_TEST_EXPECT_EQ(utilxx_base::formatSize(1023), std::string("1023"));
    // 整数单位: 无小数 (修复: 原 "1.0K" 显示不合理)
    XX_TEST_EXPECT_EQ(utilxx_base::formatSize(1024), std::string("1K"));
    XX_TEST_EXPECT_EQ(utilxx_base::formatSize(1536), std::string("1.5K"));
    XX_TEST_EXPECT_EQ(utilxx_base::formatSize(102400), std::string("100K"));
    XX_TEST_EXPECT_EQ(utilxx_base::formatSize(1024ull * 1024), std::string("1M"));
    XX_TEST_EXPECT_EQ(utilxx_base::formatSize(1024ull * 1024 * 1024), std::string("1G"));
    // 非整数中间值保留一位小数
    XX_TEST_EXPECT_EQ(utilxx_base::formatSize(1024 + 512), std::string("1.5K"));
    // 十进制基数
    XX_TEST_EXPECT_EQ(utilxx_base::formatSize(1000, 1000), std::string("1K"));
    XX_TEST_EXPECT_EQ(utilxx_base::formatSize(999, 1000), std::string("999"));
    // 大数值跨到 T
    XX_TEST_EXPECT_EQ(utilxx_base::formatSize(1024ull * 1024 * 1024 * 1024), std::string("1T"));
}

void test_formatDurationMilliseconds() {
    XX_TEST_EXPECT_EQ(utilxx_base::formatDurationMilliseconds(-1), std::string("0ms"));
    XX_TEST_EXPECT_EQ(utilxx_base::formatDurationMilliseconds(0), std::string("0ms"));
    XX_TEST_EXPECT_EQ(utilxx_base::formatDurationMilliseconds(15), std::string("15ms"));
    XX_TEST_EXPECT_EQ(utilxx_base::formatDurationMilliseconds(99), std::string("99ms"));
    XX_TEST_EXPECT_EQ(utilxx_base::formatDurationMilliseconds(100), std::string("0.1s"));
    XX_TEST_EXPECT_EQ(utilxx_base::formatDurationMilliseconds(500), std::string("0.5s"));
    XX_TEST_EXPECT_EQ(utilxx_base::formatDurationMilliseconds(1200), std::string("1.2s"));
    XX_TEST_EXPECT_EQ(utilxx_base::formatDurationMilliseconds(60000), std::string("1m0s"));
    XX_TEST_EXPECT_EQ(utilxx_base::formatDurationMilliseconds(65000), std::string("1m5s"));
    XX_TEST_EXPECT_EQ(utilxx_base::formatDurationMilliseconds(3661000), std::string("1h1m1s"));
}

void test_collapsePaths() {
    XX_TEST_EXPECT_EQ(utilxx_base::collapseSlashes(""), "");
    XX_TEST_EXPECT_EQ(utilxx_base::collapseSlashes("a//b///c"), "a/b/c");
    XX_TEST_EXPECT_EQ(utilxx_base::collapseSlashes("///a"), "/a");
    XX_TEST_EXPECT_EQ(utilxx_base::collapseSlashes("a///"), "a/");

    XX_TEST_EXPECT_EQ(utilxx_base::collapseBackslashes(""), "");
    XX_TEST_EXPECT_EQ(utilxx_base::collapseBackslashes("a\\\\b\\\\\\c"), "a\\b\\c");

    XX_TEST_EXPECT_EQ(utilxx_base::collapseMixedSlashes(""), "");
    // 连续 2+ 混合分隔符合并为 '\\', 单个保留
    XX_TEST_EXPECT_EQ(utilxx_base::collapseMixedSlashes("a//b\\\\c"), "a\\b\\c");
    XX_TEST_EXPECT_EQ(utilxx_base::collapseMixedSlashes("a///b"), "a\\b");
    XX_TEST_EXPECT_EQ(utilxx_base::collapseMixedSlashes("a/b\\c"), "a/b\\c");
    XX_TEST_EXPECT_EQ(utilxx_base::collapseMixedSlashes("a/\\b"), "a\\b");
    XX_TEST_EXPECT_EQ(utilxx_base::collapseMixedSlashes("///a"), "\\a");

    XX_TEST_EXPECT_EQ(utilxx_base::toWindowsStandardPath(""), "");
    XX_TEST_EXPECT_EQ(utilxx_base::toWindowsStandardPath("a/b\\c"), "a\\b\\c");
    XX_TEST_EXPECT_EQ(utilxx_base::toWindowsStandardPath("a//b"), "a\\b");

    XX_TEST_EXPECT_EQ(utilxx_base::toUnixStandardDirPath(""), "");
    XX_TEST_EXPECT_EQ(utilxx_base::toUnixStandardDirPath("a/b"), "a/b/");
    XX_TEST_EXPECT_EQ(utilxx_base::toUnixStandardDirPath("a/b/"), "a/b/");
    XX_TEST_EXPECT_EQ(utilxx_base::toUnixStandardDirPath("a"), "a/");
}

void test_toCurrentSystemStandardPath() {
#if XX_IS_WIN_D
    XX_TEST_EXPECT_EQ(utilxx_base::toCurrentSystemStandardPath("a/b\\c"), "a\\b\\c");
    XX_TEST_EXPECT_EQ(utilxx_base::toCurrentSystemStandardPath("a//b"), "a\\b");
#else
    // WSL/Linux: 盘符路径转为 /mnt/<drive>/
    XX_TEST_EXPECT_EQ(utilxx_base::toCurrentSystemStandardPath("C:/Users/x"), "/mnt/c/Users/x");
    XX_TEST_EXPECT_EQ(utilxx_base::toCurrentSystemStandardPath("D:\\work\\a"), "/mnt/d/work/a");
    XX_TEST_EXPECT_EQ(utilxx_base::toCurrentSystemStandardPath("a/b\\c"), "a/b/c");
    // 小写盘符
    XX_TEST_EXPECT_EQ(utilxx_base::toCurrentSystemStandardPath("e:/x"), "/mnt/e/x");
#endif
}

void test_ignoreCaseContainers() {
    // IgnoreCaseSet
    utilxx_base::IgnoreCaseSet set;
    set.insert("Hello");
    XX_TEST_EXPECT_TRUE(set.contains("hello"));
    XX_TEST_EXPECT_TRUE(set.contains("HELLO"));
    XX_TEST_EXPECT_TRUE(set.contains("HeLLo"));
    XX_TEST_EXPECT_FALSE(set.contains("world"));

    // IgnoreCaseMap
    utilxx_base::IgnoreCaseMap<int> map;
    map["Key"] = 42;
    XX_TEST_EXPECT_TRUE(map.contains("KEY"));
    XX_TEST_EXPECT_TRUE(map.contains("key"));
    XX_TEST_EXPECT_EQ(map.at("kEy"), 42);
    XX_TEST_EXPECT_FALSE(map.contains("other"));

    // 透明查找 (string_view)
    std::string_view sv = "KEY";
    XX_TEST_EXPECT_TRUE(map.contains(sv));
    XX_TEST_EXPECT_TRUE(set.contains(std::string_view("HeLLo")));
}

void test_getFileNameMore() {
    // 隐藏文件: 不剥离扩展名
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("a/b/.hidden", true), ".hidden");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("a/b/.hidden"), ".hidden");
    // 常规去扩展名
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("a/b/c.txt", true), "c");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("a/b/c.txt"), "c.txt");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("a/b/c.tar.gz", true), "c.tar");
    // 多级目录 + 尾分隔符
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("/a/b/c.txt/", true), "c.txt");
    // 仅扩展名
    XX_TEST_EXPECT_EQ(utilxx_base::getFileNameEXT("file.txt").value(), "txt");
    // useRigthDot=false: 使用最左侧点
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("a.b.c", true, false), "a");
    XX_TEST_EXPECT_EQ(utilxx_base::getFileName("a.b.c", true, true), "a.b");
}

void test_toCurrentSystemAbsolutePathBaseDir() {
    // 空输入: 原样返回
    XX_TEST_EXPECT_EQ(utilxx_base::toCurrentSystemAbsolutePath("", "/base"), "");

    // 绝对路径输入: 忽略 baseDir 原样规范化
    XX_TEST_EXPECT_EQ(
        utilxx_base::toCurrentSystemAbsolutePath("/abs/x/y", "/base"),
        std::string("/abs/x/y")
    );

    // 相对路径: 基于 baseDir 拼接 + 词法规范化
    XX_TEST_EXPECT_EQ(
        utilxx_base::toCurrentSystemAbsolutePath("a/b", "/base"),
        std::string("/base/a/b")
    );
    // `..` 收敛
    XX_TEST_EXPECT_EQ(
        utilxx_base::toCurrentSystemAbsolutePath("sub/../c.txt", "/base"),
        std::string("/base/c.txt")
    );
    // `./` 收敛
    XX_TEST_EXPECT_EQ(
        utilxx_base::toCurrentSystemAbsolutePath("./d", "/base"),
        std::string("/base/d")
    );

#if XX_IS_WIN_D
    // Windows: 分隔符统一 + generic_string 输出正斜杠
    XX_TEST_EXPECT_EQ(
        utilxx_base::toCurrentSystemAbsolutePath("a\\b", "D:/work"),
        std::string("D:/work/a/b")
    );
#endif

    // baseDir 为空: 与单参版本行为一致 (基于进程 cwd)
    {
        const std::string rel       = "ws_abs_probe_dir/file";
        auto              viaSingle = utilxx_base::toCurrentSystemAbsolutePath(rel);
        auto              viaTwoArg = utilxx_base::toCurrentSystemAbsolutePath(rel, "");
        XX_TEST_EXPECT_EQ(viaSingle, viaTwoArg);
        XX_TEST_EXPECT_TRUE(utilxx_base::isAbsolutePath(viaTwoArg));
    }
}

namespace agentxx {
namespace test {

TestResult testStringUtil() {
    g_su_passed = 0;
    g_su_failed = 0;

    test_compareExtend();
    test_toStandardPath();
    test_toUnixStandardPath();
    test_DirFilePath();
    test_removeSpace();
    test_isIgnoreCaseEqual();
    test_isIgnoreCaseContains();
    test_toArgument();
    test_base64();
    test_convertCharset();
    test_utf8Check();
    test_utf8Repair();
    test_compareExtend_pinyin();
    test_toUpperLower();
    test_charOps();
    test_utf8GetLength();
    test_findIndexByUtf8Length();
    test_findIndexAndLastLineIndexByUtf8Length();
    test_countLines();
    test_strSplit();
    test_stringVectorJoin();
    test_toStringNotNull();
    test_parseNumberFromString();
    test_formatSize();
    test_formatDurationMilliseconds();
    test_collapsePaths();
    test_toCurrentSystemStandardPath();
    test_ignoreCaseContainers();
    test_getFileNameMore();
    test_toCurrentSystemAbsolutePathBaseDir();

    return TestResult{g_su_passed, g_su_failed};
}

} // namespace test
} // namespace agentxx
