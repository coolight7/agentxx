#include "test_string_util.h"

using namespace agentxx::util;

int g_su_passed = 0;
int g_su_failed = 0;

void test_compareExtend() {
    XX_TEST_EXPECT_EQ(agentxx::util::compareExtend("", ""), 0);
    XX_TEST_EXPECT_EQ(agentxx::util::compareExtend(" ", " "), 0);
    XX_TEST_EXPECT_EQ(agentxx::util::compareExtend("123", "123"), 0);
    XX_TEST_EXPECT_EQ(agentxx::util::compareExtend(" 123\t", " 123\t"), 0);
    XX_TEST_EXPECT_EQ(agentxx::util::compareExtend(" #=k123abc\t\r\n", " #=k123abc\t\r\n"), 0);

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
        XX_TEST_EXPECT_TRUE(agentxx::util::compareExtend("file9999999999", "file1") > 0);
        XX_TEST_EXPECT_TRUE(agentxx::util::compareExtend("file1", "file9999999999") < 0);
        // int64 范围内大数精确比较: 9999999999 - 9999999998 = 1
        XX_TEST_EXPECT_EQ(agentxx::util::compareExtend("x9999999999", "x9999999998"), 1);
        XX_TEST_EXPECT_EQ(agentxx::util::compareExtend("x9999999998", "x9999999999"), -1);
        // 超 int64 范围 (饱和处理): 不崩溃, 大数仍 > 小数
        XX_TEST_EXPECT_TRUE(agentxx::util::compareExtend("file99999999999999999999", "file1") > 0);
        XX_TEST_EXPECT_TRUE(agentxx::util::compareExtend("file1", "file99999999999999999999") < 0);
        // 回归: 前缀恰为 922337203685477580 (kInt64Max/10) 时, 旧实现 leftSum*10+d
        // 在 d>=8 时有符号溢出 (UB); 修复后:
        // - d<=7 走精确路径 (9223372036854775807 是最大可精确表示值)
        // - d>=8 走饱和路径 (映射到 INT64_MAX), 符号仍正确
        XX_TEST_EXPECT_EQ(
            agentxx::util::compareExtend("x9223372036854775807", "x9223372036854775806"), 1
        );
        XX_TEST_EXPECT_EQ(
            agentxx::util::compareExtend("x9223372036854775806", "x9223372036854775807"), -1
        );
        // d=8 溢出路径: 饱和后与小数比较符号正确
        XX_TEST_EXPECT_EQ(
            agentxx::util::compareExtend("x9223372036854775808", "x9223372036854775806"), 1
        );
        XX_TEST_EXPECT_EQ(
            agentxx::util::compareExtend("x9223372036854775806", "x9223372036854775808"), -1
        );
        XX_TEST_EXPECT_TRUE(agentxx::util::compareExtend("file9223372036854775808", "file1") > 0);
    }
}

void test_toStandardPath() {
    XX_TEST_EXPECT_EQ(agentxx::util::toStandardPath("//////"), "/");
    XX_TEST_EXPECT_EQ(agentxx::util::toStandardPath("\\\\\\"), "\\");
    XX_TEST_EXPECT_EQ(agentxx::util::toStandardPath("\\\\\\/\\/\\////\\/"), "\\");
    XX_TEST_EXPECT_EQ(agentxx::util::toStandardPath("a/b\\d"), "a/b\\d");
    XX_TEST_EXPECT_EQ(agentxx::util::toStandardPath("a///b\\d"), "a/b\\d");
    XX_TEST_EXPECT_EQ(agentxx::util::toStandardPath("a/b\\\\\\d"), "a/b\\d");
    XX_TEST_EXPECT_EQ(agentxx::util::toStandardPath("a/////b\\\\d"), "a/b\\d");
    XX_TEST_EXPECT_EQ(agentxx::util::toStandardPath("///a/b\\d"), "/a/b\\d");
    XX_TEST_EXPECT_EQ(agentxx::util::toStandardPath("//a///b\\\\\\\\d/////"), "/a/b\\d/");
    XX_TEST_EXPECT_EQ(agentxx::util::toStandardPath("\\\\\\a///b\\\\\\\\d\\\\\\"), "\\a/b\\d\\");
    XX_TEST_EXPECT_EQ(agentxx::util::toStandardPath("/\\\\\\a//b\\/\\\\///d\\\\\\/"), "\\a/b\\d\\");
}

void test_toUnixStandardPath() {
    XX_TEST_EXPECT_EQ(agentxx::util::toUnixStandardPath("\\\\\\\\\\"), "/");
    XX_TEST_EXPECT_EQ(agentxx::util::toUnixStandardPath("\\\\\\\\\\//////\\/\\/\\/"), "/");
    XX_TEST_EXPECT_EQ(agentxx::util::toUnixStandardPath("a/b/d"), "a/b/d");
    XX_TEST_EXPECT_EQ(agentxx::util::toUnixStandardPath("a/b\\d"), "a/b/d");
    XX_TEST_EXPECT_EQ(agentxx::util::toUnixStandardPath("\\a\\b/d\\"), "/a/b/d/");
    XX_TEST_EXPECT_EQ(
        agentxx::util::toUnixStandardPath("\\\\\\/\\/\\a/\\\\b\\\\\\/\\/d\\//\\/\\\\\\"),
        "/a/b/d/"
    );
}

void test_DirFilePath() {
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName(""), "");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("."), ".");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("..."), "...");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("...///\\\\"), "...");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("/"), "/");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("/////"), "/////");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("\\"), "\\");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("\\\\\\\\\\"), "\\\\\\\\\\");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("///\\\\\\\\//\\\\"), "///\\\\\\\\//\\\\");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName(".", true), ".");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("./.", true), ".");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("abc/..", true), "..");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("abc..123", true), "abc.");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("abc.123.tar.gz"), "abc.123.tar.gz");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("123"), "123");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("123/"), "123");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("123\\"), "123");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("./123"), "123");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName(".\\123"), "123");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("./123.456/", true), "123.456");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("\\//455//\\\\123/\\\\//\\/\\\\\\\\"), "123");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName(".///\\\\//\\\\/\\\\123"), "123");
    XX_TEST_EXPECT_EQ(
        agentxx::util::getFileName("///\\\\//\\\\/\\\\\\\\//"),
        "///\\\\//\\\\/\\\\\\\\//"
    );
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName(".///\\\\//\\\\/\\\\\\\\//"), ".");

    XX_TEST_EXPECT_NULLOPT(agentxx::util::getFileNameEXT(""));
    XX_TEST_EXPECT_NULLOPT(agentxx::util::getFileNameEXT("."));
    XX_TEST_EXPECT_NULLOPT(agentxx::util::getFileNameEXT("..."));
    XX_TEST_EXPECT_EQ(agentxx::util::getFileNameEXT("abc.name").value(), "name");
    XX_TEST_EXPECT_NULLOPT(agentxx::util::getFileNameEXT("abc.name/"));
    XX_TEST_EXPECT_NULLOPT(agentxx::util::getFileNameEXT("abc.name\\"));
    XX_TEST_EXPECT_NULLOPT(agentxx::util::getFileNameEXT("./../..."));
    XX_TEST_EXPECT_EQ(agentxx::util::getFileNameEXT("./../...name").value(), "name");
    XX_TEST_EXPECT_NULLOPT(agentxx::util::getFileNameEXT("./../name..."));

    XX_TEST_EXPECT_EQ(agentxx::util::replaceOrAppendExt("hello", "wav"), "hello.wav");
    XX_TEST_EXPECT_EQ(agentxx::util::replaceOrAppendExt("hello.mp3", "wav"), "hello.wav");
    XX_TEST_EXPECT_EQ(agentxx::util::replaceOrAppendExt("hello.f", "wav"), "hello.wav");
    XX_TEST_EXPECT_EQ(agentxx::util::replaceOrAppendExt("hello.flac", "wav"), "hello.wav");
    XX_TEST_EXPECT_EQ(agentxx::util::replaceOrAppendExt("hello.", "wav"), "hello.wav");
    XX_TEST_EXPECT_EQ(agentxx::util::replaceOrAppendExt(".hello", "wav"), ".hello.wav");
    XX_TEST_EXPECT_EQ(agentxx::util::replaceOrAppendExt(".hello.", "wav"), ".hello.wav");

    XX_TEST_EXPECT_NULLOPT(agentxx::util::getParentDirPath(""));
    XX_TEST_EXPECT_NULLOPT(agentxx::util::getParentDirPath("."));
    XX_TEST_EXPECT_NULLOPT(agentxx::util::getParentDirPath("..."));
    XX_TEST_EXPECT_NULLOPT(agentxx::util::getParentDirPath("...xx./"));
    XX_TEST_EXPECT_EQ(agentxx::util::getParentDirPath("/...xx.").value(), "/");
    XX_TEST_EXPECT_EQ(agentxx::util::getParentDirPath("/...xx./").value(), "/");
    XX_TEST_EXPECT_EQ(agentxx::util::getParentDirPath("/...xx./xxx").value(), "/...xx./");
    XX_TEST_EXPECT_EQ(agentxx::util::getParentDirPath("./xxx").value(), "./");
    XX_TEST_EXPECT_EQ(agentxx::util::getParentDirPath("../xxx").value(), "../");
}

void test_removeSpace() {
    XX_TEST_EXPECT_EQ(agentxx::util::removeAllSpace(""), "");
    XX_TEST_EXPECT_EQ(agentxx::util::removeAllSpace("  \t \t     "), "");
    XX_TEST_EXPECT_EQ(agentxx::util::removeAllSpace("   1 2   3 "), "123");
    XX_TEST_EXPECT_EQ(agentxx::util::removeAllSpace("\t   1\t  \t2   3 \t"), "123");

    XX_TEST_EXPECT_NULLOPT(agentxx::util::removeAllSpaceMayNull(""));
    XX_TEST_EXPECT_NULLOPT(agentxx::util::removeAllSpaceMayNull("     "));
    XX_TEST_EXPECT_NULLOPT(agentxx::util::removeAllSpaceMayNull("\t  \t  \t   \t"));
    XX_TEST_EXPECT_EQ(agentxx::util::removeAllSpaceMayNull("   1 2   3 ").value(), "123");
    XX_TEST_EXPECT_EQ(agentxx::util::removeAllSpaceMayNull("\t   1\t  \t2   3 \t").value(), "123");

    XX_TEST_EXPECT_EQ(agentxx::util::removeBetweenSpace(""), "");
    XX_TEST_EXPECT_EQ(agentxx::util::removeBetweenSpace("  "), "");
    XX_TEST_EXPECT_EQ(agentxx::util::removeBetweenSpace("\t\t\t"), "");
    XX_TEST_EXPECT_EQ(agentxx::util::removeBetweenSpace("\t   \t      \t"), "");
    XX_TEST_EXPECT_EQ(agentxx::util::removeBetweenSpace("   1 2   3 "), "1 2   3");
    XX_TEST_EXPECT_EQ(agentxx::util::removeBetweenSpace("\t   1\t  \t2   3 \t"), "1\t  \t2   3");
    XX_TEST_EXPECT_EQ(
        agentxx::util::removeBetweenSpace(" \n \r  1 2   3 \n\r", false),
        "\n \r  1 2   3 \n\r"
    );
    XX_TEST_EXPECT_EQ(
        agentxx::util::removeBetweenSpace("\n \r  1 2   3\n\r", false),
        "\n \r  1 2   3\n\r"
    );
    XX_TEST_EXPECT_EQ(
        agentxx::util::removeBetweenSpace("\n \r  1 2   3\n\r  ", false),
        "\n \r  1 2   3\n\r"
    );
    XX_TEST_EXPECT_EQ(agentxx::util::removeBetweenSpace(" \n \r  1 2   3 \n\r"), "1 2   3");
    XX_TEST_EXPECT_EQ(agentxx::util::removeBetweenSpace("\n \r  1 2   3\n\r  "), "1 2   3");
    XX_TEST_EXPECT_EQ(
        agentxx::util::removeBetweenSpace("\n \r  1 2   3\n\r  ", true, false, true),
        "\n \r  1 2   3"
    );
    XX_TEST_EXPECT_EQ(
        agentxx::util::removeBetweenSpace("\n \r  1 2   3\n\r  ", true, true, false),
        "1 2   3\n\r  "
    );

    XX_TEST_EXPECT_NULLOPT(agentxx::util::removeBetweenSpaceMayNull(""));
    XX_TEST_EXPECT_NULLOPT(agentxx::util::removeBetweenSpaceMayNull("  "));
    XX_TEST_EXPECT_NULLOPT(agentxx::util::removeBetweenSpaceMayNull("\t\t\t"));
    XX_TEST_EXPECT_NULLOPT(agentxx::util::removeBetweenSpaceMayNull("\t   \t      \t"));
    XX_TEST_EXPECT_EQ(agentxx::util::removeBetweenSpaceMayNull("   1 2   3 ").value(), "1 2   3");
    XX_TEST_EXPECT_EQ(
        agentxx::util::removeBetweenSpaceMayNull("\t   1\t  \t2   3 \t").value(),
        "1\t  \t2   3"
    );
}

void test_isIgnoreCaseEqual() {
    XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseEqual("", ""));
    XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseEqual(" ", " "));
    XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseEqual("123abcABC", "123abcABC"));
    XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseEqual("123abcABC", "123ABCabc"));
    XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseEqual("abc", "AbC"));
    XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseEqual("abc\n", "AbC\n"));

    XX_TEST_EXPECT_FALSE(agentxx::util::isIgnoreCaseEqual("", "     "));
    XX_TEST_EXPECT_FALSE(agentxx::util::isIgnoreCaseEqual("abc\n\r", "ABC"));
}

void test_isIgnoreCaseContains() {
    XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContains("", ""));
    XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContains(" ", " "));
    XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContains("   ", ""));
    XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContains("123abcABC +++ ", "123abcABC"));
    XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContains("abcAbC", "AbC"));
    XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContains("AbCabc", "AbC"));
    XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContains("abc\n1fdfaf56as", "AbC\n"));
    XX_TEST_EXPECT_TRUE(
        agentxx::util::isIgnoreCaseContains("  你 好 你 好AbC\n1fdfaf56as", "你 好AbC\n")
    );

    XX_TEST_EXPECT_FALSE(agentxx::util::isIgnoreCaseContains("123abcABC", "123abcABC +++ "));
    XX_TEST_EXPECT_FALSE(agentxx::util::isIgnoreCaseContains("", "     "));
    XX_TEST_EXPECT_FALSE(agentxx::util::isIgnoreCaseContains("你 好abc\n\r", "不 好ABC"));

    XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny("", ""));
    XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny(" ", " "));
    XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny("", "     "));
    XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny("123abcABC", "123abcABC"));
    XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny(" dddabc", "AbC"));
    XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny("AbC", " dddabc"));
    XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny("ABCddd ", "AbC"));
    XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny("AbC", "ABCddd "));
    XX_TEST_EXPECT_TRUE(
        agentxx::util::isIgnoreCaseContainsAny("  你 好 你 好aBc\n1fdfaf56as", "你 好AbC\n")
    );
    XX_TEST_EXPECT_TRUE(
        agentxx::util::isIgnoreCaseContainsAny("你 好AbC\n", "  你 好 你 好aBc\n1fdfaf56as")
    );

    XX_TEST_EXPECT_FALSE(agentxx::util::isIgnoreCaseContainsAny("你  好abc", "不 好ABC"));
    XX_TEST_EXPECT_FALSE(agentxx::util::isIgnoreCaseContainsAny("你 好abc\n\r", "不 好ABC"));

    XX_TEST_EXPECT_TRUE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny(" ", " "));
    XX_TEST_EXPECT_TRUE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("123abcABC", "123abcABC")
    );
    XX_TEST_EXPECT_TRUE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny(" dddabc", "AbC"));
    XX_TEST_EXPECT_TRUE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("AbC", " dddabc"));
    XX_TEST_EXPECT_TRUE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("ABCddd ", "AbC"));
    XX_TEST_EXPECT_TRUE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("AbC", "ABCddd "));
    XX_TEST_EXPECT_TRUE(
        agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("AbC\n1fdfaf56as", "AbC\n")
    );

    XX_TEST_EXPECT_FALSE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("", ""));
    XX_TEST_EXPECT_FALSE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("   ", ""));
    XX_TEST_EXPECT_FALSE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("", "     "));
    XX_TEST_EXPECT_FALSE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("你  好abc", "不 好ABC")
    );
    XX_TEST_EXPECT_FALSE(
        agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("你 好abc\n\r", "不 好ABC")
    );
}

void test_toArgument() {
    XX_TEST_EXPECT_EQ(agentxx::util::toArgument(""), "\"\"");
    XX_TEST_EXPECT_EQ(agentxx::util::toArgument("\"\""), "\"\\\"\\\"\"");
    XX_TEST_EXPECT_EQ(
        agentxx::util::toArgument("{\"enable_thinking\": false}"),
        "\"{\\\"enable_thinking\\\": false}\""
    );
    XX_TEST_EXPECT_EQ(agentxx::util::toArgument("\"hh\", --"), "\"\\\"hh\\\", --\"");
    XX_TEST_EXPECT_EQ(agentxx::util::toArgument("\"\"\", --"), "\"\\\"\\\"\\\", --\"");
    XX_TEST_EXPECT_EQ(agentxx::util::toArgument("\\\"\"\\\", --"), "\"\\\"\\\"\\\", --\"");
    XX_TEST_EXPECT_EQ(agentxx::util::toArgument("\"\\\"\", --"), "\"\\\"\\\"\\\", --\"");
    XX_TEST_EXPECT_EQ(agentxx::util::toArgument("\"wow\""), "\"\\\"wow\\\"\"");
    XX_TEST_EXPECT_EQ(agentxx::util::toArgument("\\\"wow\\\""), "\"\\\"wow\\\"\"");

    // 回归: mark 前连续反斜杠的奇偶性决定是否转义
    // - 奇数个反斜杠 (\\"): mark 已被转义 (字面引号), 不再转义
    XX_TEST_EXPECT_EQ(agentxx::util::toArgument("a\\\"b"), "\"a\\\"b\"");
    // - 偶数个反斜杠 (\\\\"): mark 是新界定符, 必须转义 (旧实现漏转义导致引号提前闭合)
    XX_TEST_EXPECT_EQ(agentxx::util::toArgument("a\\\\\"b"), "\"a\\\\\\\"b\"");
    XX_TEST_EXPECT_EQ(agentxx::util::toArgument("\\\\\""), "\"\\\\\\\"\"");
    // - 奇数个反斜杠在串尾 + 后续 mark: 不转义
    XX_TEST_EXPECT_EQ(agentxx::util::toArgument("a\\\"\"b"), "\"a\\\"\\\"b\"");
}

void test_base64() {
    // 往返
    XX_TEST_EXPECT_EQ(agentxx::util::base64Encode(""), "");
    XX_TEST_EXPECT_EQ(agentxx::util::base64Encode("f"), "Zg==");
    XX_TEST_EXPECT_EQ(agentxx::util::base64Encode("fo"), "Zm8=");
    XX_TEST_EXPECT_EQ(agentxx::util::base64Encode("foo"), "Zm9v");
    XX_TEST_EXPECT_EQ(agentxx::util::base64Encode("foobar"), "Zm9vYmFy");

    auto roundTrip = [](const std::string& data) {
        auto encoded = agentxx::util::base64Encode(data);
        auto decoded = agentxx::util::base64Decode(encoded);
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
    auto emptyDec = agentxx::util::base64Decode("");
    XX_TEST_EXPECT_TRUE(emptyDec.has_value());
    if (emptyDec.has_value()) {
        XX_TEST_EXPECT_EQ(emptyDec.value(), "");
    }

    // 合法 base64
    auto validDec = agentxx::util::base64Decode("Zm9vYmFy");
    XX_TEST_EXPECT_TRUE(validDec.has_value());
    if (validDec.has_value()) {
        XX_TEST_EXPECT_EQ(validDec.value(), "foobar");
    }

    // 未补齐 padding 的合法 base64 (长度 mod4 ∈ {2,3}) 也必须正确解码
    // (修复前 decoded_size 分配不足会导致堆越界写)
    {
        auto d2 = agentxx::util::base64Decode("Zg"); // "f"
        XX_TEST_EXPECT_TRUE(d2.has_value());
        if (d2.has_value()) {
            XX_TEST_EXPECT_EQ(d2.value(), "f");
        }
        auto d3 = agentxx::util::base64Decode("Zm8"); // "fo"
        XX_TEST_EXPECT_TRUE(d3.has_value());
        if (d3.has_value()) {
            XX_TEST_EXPECT_EQ(d3.value(), "fo");
        }
        auto d6 = agentxx::util::base64Decode("Zm9vYg"); // "foob"
        XX_TEST_EXPECT_TRUE(d6.has_value());
        if (d6.has_value()) {
            XX_TEST_EXPECT_EQ(d6.value(), "foob");
        }
        auto d7 = agentxx::util::base64Decode("Zm9vYmE"); // "fooba"
        XX_TEST_EXPECT_TRUE(d7.has_value());
        if (d7.has_value()) {
            XX_TEST_EXPECT_EQ(d7.value(), "fooba");
        }
    }

    // 大尺寸未补齐 padding 输入往返 (修复前会堆溢出): 编码后去掉 '=' 再解码
    {
        std::string large(1000, 'x');
        auto        encoded = agentxx::util::base64Encode(large);
        while (!encoded.empty() && encoded.back() == '=') {
            encoded.pop_back();
        }
        auto decoded = agentxx::util::base64Decode(encoded);
        XX_TEST_EXPECT_TRUE(decoded.has_value());
        if (decoded.has_value()) {
            XX_TEST_EXPECT_EQ(decoded.value(), large);
        }
    }

    // 非法输入必须返回 nullopt (修复: 不能与空结果混淆)
    XX_TEST_EXPECT_FALSE(agentxx::util::base64Decode("!!!not base64!!!").has_value());
    XX_TEST_EXPECT_FALSE(agentxx::util::base64Decode("====").has_value()); // 仅 padding
    XX_TEST_EXPECT_FALSE(agentxx::util::base64Decode("Zm9vY").has_value()); // 数据长度 mod4==1 非法
    XX_TEST_EXPECT_FALSE(agentxx::util::base64Decode("Zm=v").has_value()); // padding 位置非法
    XX_TEST_EXPECT_FALSE(agentxx::util::base64Decode("Zm9v YmFy").has_value()); // 含空格非法

    // 回归: 短输入含 '=' 时不应触发 size_t 下溢 (旧实现 i < str.size()-2)
    XX_TEST_EXPECT_FALSE(agentxx::util::base64Decode("=").has_value());
    XX_TEST_EXPECT_FALSE(agentxx::util::base64Decode("==").has_value());
    XX_TEST_EXPECT_FALSE(agentxx::util::base64Decode("===").has_value());
    XX_TEST_EXPECT_FALSE(agentxx::util::base64Decode("Z=").has_value()); // 数据长度 mod4==1
}

void test_convertCharset() {
    // 相同编码: 成功且无需转换 (nullopt)
    {
        auto [ok, res] = agentxx::util::convertCharset("hello", "UTF-8", "UTF-8");
        XX_TEST_EXPECT_TRUE(ok);
        XX_TEST_EXPECT_FALSE(res.has_value());
    }
    // 空输入: 失败
    {
        auto [ok, res] = agentxx::util::convertCharset("", "GB18030", "UTF-8");
        XX_TEST_EXPECT_FALSE(ok);
        XX_TEST_EXPECT_FALSE(res.has_value());
    }
    // 非法编码名: 失败 (修复: 不能谎报成功)
    {
        auto [ok, res] = agentxx::util::convertCharset("hello", "NOT_A_REAL_ENCODING_XYZ", "UTF-8");
        XX_TEST_EXPECT_FALSE(ok);
        XX_TEST_EXPECT_FALSE(res.has_value());
    }
    // GBK/GB18030 -> UTF-8: "中文" 的 GBK 字节为 D6 D0 CE C4
    {
        std::string gbkBytes = std::string("\xD6\xD0\xCE\xC4", 4);
        auto [ok, res]       = agentxx::util::convertCharset(gbkBytes, "GB18030", "UTF-8");
        XX_TEST_EXPECT_TRUE(ok);
        XX_TEST_EXPECT_TRUE(res.has_value());
        if (res.has_value()) {
            XX_TEST_EXPECT_EQ(res.value(), "中文");
        }
    }
    // autoConvertToUtf8 (in-place): GBK -> UTF-8
    {
        std::string gbkBytes = std::string("\xD6\xD0\xCE\xC4", 4);
        bool        ok       = agentxx::util::autoConvertToUtf8(gbkBytes);
        XX_TEST_EXPECT_TRUE(ok);
        XX_TEST_EXPECT_EQ(gbkBytes, "中文");
    }
}

void test_utf8Check() {
    // 合法 UTF-8
    XX_TEST_EXPECT_EQ(agentxx::util::utf8GetLengthCheckAvail("hello"), 5u);
    XX_TEST_EXPECT_EQ(agentxx::util::utf8GetLengthCheckAvail("中文"), 2u);
    XX_TEST_EXPECT_EQ(agentxx::util::utf8GetLengthCheckAvail("a中b文c"), 5u);
    XX_TEST_EXPECT_TRUE(agentxx::util::utf8IsAvail("中文abc"));

    // 非法/截断 UTF-8 返回 0
    XX_TEST_EXPECT_EQ(agentxx::util::utf8GetLengthCheckAvail(std::string("\xC0\x80", 2)), 0u);
    XX_TEST_EXPECT_EQ(agentxx::util::utf8GetLengthCheckAvail(std::string("\xFF\xFE", 2)), 0u);
    XX_TEST_EXPECT_EQ(
        agentxx::util::utf8GetLengthCheckAvail(std::string("\xE4\xB8", 2)),
        0u
    ); // 截断的 3 字节序列
    XX_TEST_EXPECT_FALSE(agentxx::util::utf8IsAvail(std::string("\xFF\xFE", 2)));
    XX_TEST_EXPECT_TRUE(agentxx::util::utf8IsAvail(""));

    // 含内嵌 '\0': 在 '\0' 处停止计数 (修复 #8: 直接传 string_view 而非 str.data())
    {
        std::string withNull("ab\0cd", 5);
        // 在 '\0' 处 break, 计到 "ab" 共 2 个字符
        XX_TEST_EXPECT_EQ(agentxx::util::utf8GetLengthCheckAvail(withNull), 2u);
    }

    // 更多边界: 5/6 字节头非法; 非最短编码 (0xF0 0x80..) 非法;
    // 替换字符 EF BF BD 合法 (部分转换需要用 U+FFFD 替代非法编码, utf8Repair 的产物必须被视为合法)
    XX_TEST_EXPECT_EQ(
        agentxx::util::utf8GetLengthCheckAvail(std::string("\xF8\x88\x80\x80\x80", 5)),
        0u
    );
    XX_TEST_EXPECT_EQ(
        agentxx::util::utf8GetLengthCheckAvail(std::string("\xFC\x84\x80\x80\x80\x80", 6)),
        0u
    );
    XX_TEST_EXPECT_EQ(
        agentxx::util::utf8GetLengthCheckAvail(std::string("\xF0\x80\x80\x80", 4)),
        0u
    );
    XX_TEST_EXPECT_EQ(agentxx::util::utf8GetLengthCheckAvail(std::string("\xE0\x80\x80", 3)), 0u);
    XX_TEST_EXPECT_EQ(agentxx::util::utf8GetLengthCheckAvail(std::string("\xEF\xBF\xBD", 3)), 1u);
    // 合法 4 字节 (emoji)
    XX_TEST_EXPECT_EQ(
        agentxx::util::utf8GetLengthCheckAvail(std::string("\xF0\x9F\x98\x80", 4)),
        1u
    );
    XX_TEST_EXPECT_TRUE(agentxx::util::utf8IsAvail(std::string("\xF0\x9F\x98\x80", 4)));
    // 截断的 4 字节序列
    XX_TEST_EXPECT_EQ(agentxx::util::utf8GetLengthCheckAvail(std::string("\xF0\x9F\x98", 3)), 0u);
    // 连续字符计数
    XX_TEST_EXPECT_EQ(
        agentxx::util::utf8GetLengthCheckAvail(std::string("\xE4\xB8\xAD\xE6\x96\x87", 6)),
        2u
    );
}

void test_utf8Repair() {
    const std::string replacement("\xEF\xBF\xBD"); // U+FFFD 的 UTF-8 编码

    // 合法输入: 不做修改, 返回 false
    {
        std::string s = "hello中文";
        XX_TEST_EXPECT_FALSE(agentxx::util::utf8Repair(s));
        XX_TEST_EXPECT_EQ(s, "hello中文");

        std::string empty;
        XX_TEST_EXPECT_FALSE(agentxx::util::utf8Repair(empty));
        XX_TEST_EXPECT_TRUE(empty.empty());

        std::string emoji("\xF0\x9F\x98\x80", 4); // 合法 4 字节
        XX_TEST_EXPECT_FALSE(agentxx::util::utf8Repair(emoji));
        XX_TEST_EXPECT_EQ(emoji, std::string("\xF0\x9F\x98\x80", 4));
    }

    // 孤立延续字节: 替换为单个 U+FFFD
    {
        std::string s("\x80", 1);
        XX_TEST_EXPECT_TRUE(agentxx::util::utf8Repair(s));
        XX_TEST_EXPECT_EQ(s, replacement);

        std::string s2("\x80\x81", 2);
        XX_TEST_EXPECT_TRUE(agentxx::util::utf8Repair(s2));
        XX_TEST_EXPECT_EQ(s2, replacement + replacement);
    }

    // 末尾截断的多字节序列: 整体替换为一个 U+FFFD
    {
        std::string s("\xE4\xB8", 2); // 截断的 3 字节序列
        XX_TEST_EXPECT_TRUE(agentxx::util::utf8Repair(s));
        XX_TEST_EXPECT_EQ(s, replacement);

        std::string s4("\xF0\x9F\x98", 3); // 截断的 4 字节序列
        XX_TEST_EXPECT_TRUE(agentxx::util::utf8Repair(s4));
        XX_TEST_EXPECT_EQ(s4, replacement);
    }

    // 过短编码头 0xC0/0xC1: 按 maximal subpart 逐字节替换
    {
        std::string s("\xC0\x80", 2);
        XX_TEST_EXPECT_TRUE(agentxx::util::utf8Repair(s));
        XX_TEST_EXPECT_EQ(s, replacement + replacement);
    }

    // 非最短编码序列: 前导字节替换, 后续孤立字节各自替换
    {
        std::string s("\xE0\x80\x80", 3);
        XX_TEST_EXPECT_TRUE(agentxx::util::utf8Repair(s));
        XX_TEST_EXPECT_EQ(s, replacement + replacement + replacement);

        std::string s4("\xF0\x80\x80\x80", 4);
        XX_TEST_EXPECT_TRUE(agentxx::util::utf8Repair(s4));
        XX_TEST_EXPECT_EQ(s4, replacement + replacement + replacement + replacement);
    }

    // 5/6 字节头及无效头: 逐字节替换
    {
        std::string s("\xF8\x88\x80\x80\x80", 5);
        XX_TEST_EXPECT_TRUE(agentxx::util::utf8Repair(s));
        XX_TEST_EXPECT_EQ(s, replacement + replacement + replacement + replacement + replacement);

        std::string s2("\xFF\xFE", 2);
        XX_TEST_EXPECT_TRUE(agentxx::util::utf8Repair(s2));
        XX_TEST_EXPECT_EQ(s2, replacement + replacement);
    }

    // 混合: 合法部分保留, 非法被替换且不吞掉后续合法字符
    {
        std::string s  = "a";
        s             += "\xFF";
        s             += " b中";
        XX_TEST_EXPECT_TRUE(agentxx::util::utf8Repair(s));
        XX_TEST_EXPECT_EQ(s, "a" + replacement + " b中");
    }

    // 截断序列后紧跟合法字符: 合法字符不被吞掉
    {
        std::string s("\xE4\xB8", 2);
        s += "A中";
        XX_TEST_EXPECT_TRUE(agentxx::util::utf8Repair(s));
        XX_TEST_EXPECT_EQ(s, replacement + "A中");
    }

    // 多字节序列中途出现非法字节: 序列前缀替换, 后续合法序列保留
    {
        std::string s("\xE4\xB8\xAD", 3); // 中
        s += "\xFF";
        s += "\xE6\x96\x87"; // 文
        XX_TEST_EXPECT_TRUE(agentxx::util::utf8Repair(s));
        XX_TEST_EXPECT_EQ(
            s,
            std::string("\xE4\xB8\xAD", 3) + replacement + std::string("\xE6\x96\x87", 3)
        );
    }

    // 幂等: 修复后结果为合法 UTF-8, 再次调用返回 false 且不再修改
    {
        std::string s("\xE4\xB8\xFF", 3);
        XX_TEST_EXPECT_TRUE(agentxx::util::utf8Repair(s));
        XX_TEST_EXPECT_TRUE(agentxx::util::utf8IsAvail(s));
        const std::string repaired = s;
        XX_TEST_EXPECT_FALSE(agentxx::util::utf8Repair(s));
        XX_TEST_EXPECT_EQ(s, repaired);
    }
}

void test_compareExtend_pinyin() {
    // 中文拼音比较依赖全局 s_pinyinCallback; 设置后测试并恢复
    auto oldCallback                = agentxx::util::s_pinyinCallback;
    agentxx::util::s_pinyinCallback = [](std::string_view str) -> std::string {
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
    XX_TEST_EXPECT_TRUE(agentxx::util::compareExtend("你", "我") < 0);
    XX_TEST_EXPECT_TRUE(agentxx::util::compareExtend("我", "你") > 0);
    // 首字相同, 次字按拼音: 八(ba) < 二(er)
    XX_TEST_EXPECT_TRUE(agentxx::util::compareExtend("七八九", "七二一") < 0);
    XX_TEST_EXPECT_TRUE(agentxx::util::compareExtend("七二一", "七八九") > 0);
    // 中文与数字: 数字优先
    XX_TEST_EXPECT_TRUE(agentxx::util::compareExtend("1", "你") < 0);
    XX_TEST_EXPECT_TRUE(agentxx::util::compareExtend("你", "1") > 0);
    // 中文与英文拼音首字母
    XX_TEST_EXPECT_TRUE(agentxx::util::compareExtend("你", "a") > 0); // n > a
    XX_TEST_EXPECT_TRUE(agentxx::util::compareExtend("你", "z") < 0); // n < z
    // 混排场景 (原注释掉的用例)
    shiftCompareExtend(" #= 你 77", " #= 你 234", 77 - 234);

    agentxx::util::s_pinyinCallback = oldCallback;
}

void test_toUpperLower() {
    XX_TEST_EXPECT_EQ(agentxx::util::toUpper(""), "");
    XX_TEST_EXPECT_EQ(agentxx::util::toUpper("aBc123!@#"), "ABC123!@#");
    XX_TEST_EXPECT_EQ(agentxx::util::toUpper("中文abc"), "中文ABC");
    XX_TEST_EXPECT_EQ(agentxx::util::toLower(""), "");
    XX_TEST_EXPECT_EQ(agentxx::util::toLower("AbC123!@#"), "abc123!@#");
    XX_TEST_EXPECT_EQ(agentxx::util::toLower("中文ABC"), "中文abc");

    // in-place 版本
    std::string s1 = "aBc123";
    agentxx::util::toUpperSelf(s1);
    XX_TEST_EXPECT_EQ(s1, "ABC123");
    std::string s2 = "AbC123";
    agentxx::util::toLowerSelf(s2);
    XX_TEST_EXPECT_EQ(s2, "abc123");
}

void test_charOps() {
    XX_TEST_EXPECT_EQ(agentxx::util::charToLower('A'), 'a');
    XX_TEST_EXPECT_EQ(agentxx::util::charToLower('z'), 'z');
    XX_TEST_EXPECT_EQ(agentxx::util::charToLower('1'), '1');
    XX_TEST_EXPECT_EQ(agentxx::util::charToUpper('a'), 'A');
    XX_TEST_EXPECT_EQ(agentxx::util::charToUpper('Z'), 'Z');
    XX_TEST_EXPECT_EQ(agentxx::util::charToUpper('1'), '1');

    XX_TEST_EXPECT_TRUE(agentxx::util::charIsSpace(' '));
    XX_TEST_EXPECT_TRUE(agentxx::util::charIsSpace('\t'));
    XX_TEST_EXPECT_TRUE(agentxx::util::charIsSpace('\n'));
    XX_TEST_EXPECT_TRUE(agentxx::util::charIsSpace('\r'));
    XX_TEST_EXPECT_TRUE(agentxx::util::charIsSpace('\v'));
    XX_TEST_EXPECT_TRUE(agentxx::util::charIsSpace('\f'));
    XX_TEST_EXPECT_FALSE(agentxx::util::charIsSpace('a'));
    XX_TEST_EXPECT_FALSE(agentxx::util::charIsSpace('0'));

    XX_TEST_EXPECT_TRUE(agentxx::util::isCode_num('0'));
    XX_TEST_EXPECT_TRUE(agentxx::util::isCode_num('9'));
    XX_TEST_EXPECT_FALSE(agentxx::util::isCode_num('a'));
    XX_TEST_EXPECT_TRUE(agentxx::util::isCode_AZ('A'));
    XX_TEST_EXPECT_TRUE(agentxx::util::isCode_AZ('Z'));
    XX_TEST_EXPECT_FALSE(agentxx::util::isCode_AZ('a'));
    XX_TEST_EXPECT_TRUE(agentxx::util::isCode_az('a'));
    XX_TEST_EXPECT_TRUE(agentxx::util::isCode_az('z'));
    XX_TEST_EXPECT_FALSE(agentxx::util::isCode_az('A'));
    XX_TEST_EXPECT_TRUE(agentxx::util::isCode_AZaz('a'));
    XX_TEST_EXPECT_TRUE(agentxx::util::isCode_AZaz('Z'));
    XX_TEST_EXPECT_FALSE(agentxx::util::isCode_AZaz('1'));

    XX_TEST_EXPECT_EQ(agentxx::util::toCode_tryAZ('a').value(), 'A');
    XX_TEST_EXPECT_EQ(agentxx::util::toCode_tryAZ('A').value(), 'A');
    XX_TEST_EXPECT_NULLOPT(agentxx::util::toCode_tryAZ('1'));
    XX_TEST_EXPECT_EQ(agentxx::util::toCode_tryaz('A').value(), 'a');
    XX_TEST_EXPECT_EQ(agentxx::util::toCode_tryaz('a').value(), 'a');
    XX_TEST_EXPECT_NULLOPT(agentxx::util::toCode_tryaz('1'));
    XX_TEST_EXPECT_EQ(agentxx::util::toCode_mayAZ('b'), 'B');
    XX_TEST_EXPECT_EQ(agentxx::util::toCode_mayAZ('1'), '1');
    XX_TEST_EXPECT_EQ(agentxx::util::toCode_mayaz('B'), 'b');
    XX_TEST_EXPECT_EQ(agentxx::util::toCode_mayaz('1'), '1');
    XX_TEST_EXPECT_EQ(agentxx::util::toCode_AZ('a'), 'A');
    XX_TEST_EXPECT_EQ(agentxx::util::toCode_AZ('A'), 'A');
    XX_TEST_EXPECT_EQ(agentxx::util::toCode_az('A'), 'a');
    XX_TEST_EXPECT_EQ(agentxx::util::toCode_az('a'), 'a');
}

void test_utf8GetLength() {
    XX_TEST_EXPECT_EQ(agentxx::util::utf8GetLength(""), 0u);
    XX_TEST_EXPECT_EQ(agentxx::util::utf8GetLength("abc"), 3u);
    XX_TEST_EXPECT_EQ(agentxx::util::utf8GetLength("中文abc"), 5u);
    // emoji 4 字节
    XX_TEST_EXPECT_EQ(agentxx::util::utf8GetLength(std::string("\xF0\x9F\x98\x80", 4)), 1u);
    // 混合
    XX_TEST_EXPECT_EQ(
        agentxx::util::utf8GetLength(std::string("a\xE4\xB8\xAD\xF0\x9F\x98\x80", 6)),
        3u
    );
}

void test_findIndexByUtf8Length() {
    // "中文abc": 中(3) 文(3) a b c -> 字节偏移 0,3,6,7,8,9
    XX_TEST_EXPECT_EQ(agentxx::util::findIndexByUtf8Length("中文abc", 1), 3u);
    XX_TEST_EXPECT_EQ(agentxx::util::findIndexByUtf8Length("中文abc", 2), 6u);
    XX_TEST_EXPECT_EQ(agentxx::util::findIndexByUtf8Length("中文abc", 3), 7u);
    XX_TEST_EXPECT_EQ(agentxx::util::findIndexByUtf8Length("中文abc", 5), 9u);
    // 超出长度: 返回 0
    XX_TEST_EXPECT_EQ(agentxx::util::findIndexByUtf8Length("中文abc", 6), 0u);
    // 空输入
    XX_TEST_EXPECT_EQ(agentxx::util::findIndexByUtf8Length("", 1), 0u);
    // 从 start 偏移开始
    XX_TEST_EXPECT_EQ(agentxx::util::findIndexByUtf8Length("中文abc", 1, 3), 6u);
}

void test_findIndexAndLastLineIndexByUtf8Length() {
    // "a\nb\nc": a(0) \n(1) b(2) \n(3) c(4)
    auto r1 = agentxx::util::findIndexAndLastLineIndexByUtf8Length("a\nb\nc", 3);
    XX_TEST_EXPECT_EQ(std::get<0>(r1), 3u); // 第3字符 'b' 后
    XX_TEST_EXPECT_EQ(std::get<1>(r1), 1u); // 遇到 1 个换行
    XX_TEST_EXPECT_EQ(std::get<2>(r1), 1u); // 最后换行在 index 1

    // 超出长度: 返回 {0,0,0}
    auto r2 = agentxx::util::findIndexAndLastLineIndexByUtf8Length("a\nb\nc", 10);
    XX_TEST_EXPECT_EQ(std::get<0>(r2), 0u);
    XX_TEST_EXPECT_EQ(std::get<1>(r2), 0u);
    XX_TEST_EXPECT_EQ(std::get<2>(r2), 0u);

    // 空输入
    auto r3 = agentxx::util::findIndexAndLastLineIndexByUtf8Length("", 1);
    XX_TEST_EXPECT_EQ(std::get<0>(r3), 0u);

    // 精确取到换行符本身
    auto r4 = agentxx::util::findIndexAndLastLineIndexByUtf8Length("a\nb", 2);
    XX_TEST_EXPECT_EQ(std::get<0>(r4), 2u);
    XX_TEST_EXPECT_EQ(std::get<1>(r4), 1u);
    XX_TEST_EXPECT_EQ(std::get<2>(r4), 1u);
}

void test_countLines() {
    // 空输入
    XX_TEST_EXPECT_EQ(agentxx::util::countLines(""), 0u);
    // 单个换行符: 前面空行, 末尾换行后无内容 -> 1 行
    XX_TEST_EXPECT_EQ(agentxx::util::countLines("\n"), 1u);
    // 多行且末尾无换行: 最后一个不完整行也算一行
    XX_TEST_EXPECT_EQ(agentxx::util::countLines("a\nb\nc"), 3u);
    // 末尾有换行: '\n' 数量即行数
    XX_TEST_EXPECT_EQ(agentxx::util::countLines("a\nb\nc\n"), 3u);
    // 连续换行: 空行也计数
    XX_TEST_EXPECT_EQ(agentxx::util::countLines("\n\n"), 2u);
    XX_TEST_EXPECT_EQ(agentxx::util::countLines("a\n\nb"), 3u);
    // 无换行: 单行
    XX_TEST_EXPECT_EQ(agentxx::util::countLines("hello"), 1u);
    // 仅末尾换行前有多行内容
    XX_TEST_EXPECT_EQ(agentxx::util::countLines("line1\nline2\nline3\nline4"), 4u);
}

void test_strSplit() {
    auto r1 = agentxx::util::strSplit("a,b,c", ',');
    XX_TEST_EXPECT_EQ(r1.size(), (size_t)3);
    XX_TEST_EXPECT_EQ(r1[0], std::string_view("a"));
    XX_TEST_EXPECT_EQ(r1[1], std::string_view("b"));
    XX_TEST_EXPECT_EQ(r1[2], std::string_view("c"));

    // 空串: split 空输入产生空结果 (无元素)
    auto r2 = agentxx::util::strSplit("", ',');
    XX_TEST_EXPECT_EQ(r2.size(), (size_t)0);

    // 单元素无分隔符
    auto r2b = agentxx::util::strSplit("a", ',');
    XX_TEST_EXPECT_EQ(r2b.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(r2b[0], std::string_view("a"));

    // 连续分隔符 -> 空元素
    auto r3 = agentxx::util::strSplit("a,,b", ',');
    XX_TEST_EXPECT_EQ(r3.size(), (size_t)3);
    XX_TEST_EXPECT_EQ(r3[1], std::string_view(""));

    // 首尾分隔符
    auto r4 = agentxx::util::strSplit(",a,", ',');
    XX_TEST_EXPECT_EQ(r4.size(), (size_t)3);
    XX_TEST_EXPECT_EQ(r4[0], std::string_view(""));
    XX_TEST_EXPECT_EQ(r4[2], std::string_view(""));

    // 无分隔符
    auto r5 = agentxx::util::strSplit("abc", ',');
    XX_TEST_EXPECT_EQ(r5.size(), (size_t)1);
    XX_TEST_EXPECT_EQ(r5[0], std::string_view("abc"));

    // 拷贝版本
    auto r6 = agentxx::util::strSplitCopid("a;b;c", ';');
    XX_TEST_EXPECT_EQ(r6.size(), (size_t)3);
    XX_TEST_EXPECT_EQ(r6[1], std::string("b"));
}

void test_stringVectorJoin() {
    XX_TEST_EXPECT_EQ(agentxx::util::stringVectorJoin(std::vector<std::string>{}), "");
    XX_TEST_EXPECT_EQ(
        agentxx::util::stringVectorJoin(std::vector<std::string>{"a", "b", "c"}),
        "a, b, c"
    );
    XX_TEST_EXPECT_EQ(agentxx::util::stringVectorJoin(std::vector<std::string>{"a"}, "-"), "a");
    // 非字符串元素
    XX_TEST_EXPECT_EQ(agentxx::util::stringVectorJoin(std::vector<int>{1, 2, 3}, "-"), "1-2-3");
}

void test_toStringNotNull() {
    XX_TEST_EXPECT_EQ(agentxx::util::toStringNotNull(nullptr), std::string_view(""));
    XX_TEST_EXPECT_EQ(agentxx::util::toStringNotNull("abc"), std::string_view("abc"));
    XX_TEST_EXPECT_EQ(agentxx::util::toStringNotNull(""), std::string_view(""));
}

void test_parseNumberFromString() {
    int    iv = 0;
    double dv = 0.0;
    auto   r1 = agentxx::util::parseNumberFromString("123", iv);
    XX_TEST_EXPECT_EQ(r1.ec, std::errc{});
    XX_TEST_EXPECT_EQ(iv, 123);

    // 前导空格: from_chars 不接受 -> 失败
    auto r2 = agentxx::util::parseNumberFromString("  123", iv);
    XX_TEST_EXPECT_TRUE(r2.ec != std::errc{});

    // 部分解析: "12a" 解析出 12, ptr 停在 'a'
    auto r3 = agentxx::util::parseNumberFromString("12a", iv);
    XX_TEST_EXPECT_EQ(r3.ec, std::errc{});
    XX_TEST_EXPECT_EQ(iv, 12);
    XX_TEST_EXPECT_EQ(std::string_view{r3.ptr}, std::string_view("12a").data() + 2);

    // 非法输入
    auto r4 = agentxx::util::parseNumberFromString("abc", iv);
    XX_TEST_EXPECT_TRUE(r4.ec != std::errc{});

    // double 解析
    auto r5 = agentxx::util::parseNumberFromString("3.14", dv);
    XX_TEST_EXPECT_EQ(r5.ec, std::errc{});
    XX_TEST_EXPECT_TRUE(dv > 3.13 && dv < 3.15);

    // 空串
    auto r6 = agentxx::util::parseNumberFromString("", iv);
    XX_TEST_EXPECT_TRUE(r6.ec != std::errc{});

    // 负数和溢出
    int  neg = 0;
    auto r7  = agentxx::util::parseNumberFromString("-42", neg);
    XX_TEST_EXPECT_EQ(r7.ec, std::errc{});
    XX_TEST_EXPECT_EQ(neg, -42);
    int  ov = 0;
    auto r8 = agentxx::util::parseNumberFromString("99999999999999999999", ov);
    XX_TEST_EXPECT_TRUE(r8.ec == std::errc::result_out_of_range);
}

void test_formatSize() {
    XX_TEST_EXPECT_EQ(agentxx::util::formatSize(0), std::string("0"));
    XX_TEST_EXPECT_EQ(agentxx::util::formatSize(1), std::string("1"));
    XX_TEST_EXPECT_EQ(agentxx::util::formatSize(999), std::string("999"));
    XX_TEST_EXPECT_EQ(agentxx::util::formatSize(1023), std::string("1023"));
    // 整数单位: 无小数 (修复: 原 "1.0K" 显示不合理)
    XX_TEST_EXPECT_EQ(agentxx::util::formatSize(1024), std::string("1K"));
    XX_TEST_EXPECT_EQ(agentxx::util::formatSize(1536), std::string("1.5K"));
    XX_TEST_EXPECT_EQ(agentxx::util::formatSize(102400), std::string("100K"));
    XX_TEST_EXPECT_EQ(agentxx::util::formatSize(1024ull * 1024), std::string("1M"));
    XX_TEST_EXPECT_EQ(agentxx::util::formatSize(1024ull * 1024 * 1024), std::string("1G"));
    // 非整数中间值保留一位小数
    XX_TEST_EXPECT_EQ(agentxx::util::formatSize(1024 + 512), std::string("1.5K"));
    // 十进制基数
    XX_TEST_EXPECT_EQ(agentxx::util::formatSize(1000, 1000), std::string("1K"));
    XX_TEST_EXPECT_EQ(agentxx::util::formatSize(999, 1000), std::string("999"));
    // 大数值跨到 T
    XX_TEST_EXPECT_EQ(agentxx::util::formatSize(1024ull * 1024 * 1024 * 1024), std::string("1T"));
}

void test_collapsePaths() {
    XX_TEST_EXPECT_EQ(agentxx::util::collapseSlashes(""), "");
    XX_TEST_EXPECT_EQ(agentxx::util::collapseSlashes("a//b///c"), "a/b/c");
    XX_TEST_EXPECT_EQ(agentxx::util::collapseSlashes("///a"), "/a");
    XX_TEST_EXPECT_EQ(agentxx::util::collapseSlashes("a///"), "a/");

    XX_TEST_EXPECT_EQ(agentxx::util::collapseBackslashes(""), "");
    XX_TEST_EXPECT_EQ(agentxx::util::collapseBackslashes("a\\\\b\\\\\\c"), "a\\b\\c");

    XX_TEST_EXPECT_EQ(agentxx::util::collapseMixedSlashes(""), "");
    // 连续 2+ 混合分隔符合并为 '\\', 单个保留
    XX_TEST_EXPECT_EQ(agentxx::util::collapseMixedSlashes("a//b\\\\c"), "a\\b\\c");
    XX_TEST_EXPECT_EQ(agentxx::util::collapseMixedSlashes("a///b"), "a\\b");
    XX_TEST_EXPECT_EQ(agentxx::util::collapseMixedSlashes("a/b\\c"), "a/b\\c");
    XX_TEST_EXPECT_EQ(agentxx::util::collapseMixedSlashes("a/\\b"), "a\\b");
    XX_TEST_EXPECT_EQ(agentxx::util::collapseMixedSlashes("///a"), "\\a");

    XX_TEST_EXPECT_EQ(agentxx::util::toWindowsStandardPath(""), "");
    XX_TEST_EXPECT_EQ(agentxx::util::toWindowsStandardPath("a/b\\c"), "a\\b\\c");
    XX_TEST_EXPECT_EQ(agentxx::util::toWindowsStandardPath("a//b"), "a\\b");

    XX_TEST_EXPECT_EQ(agentxx::util::toUnixStandardDirPath(""), "");
    XX_TEST_EXPECT_EQ(agentxx::util::toUnixStandardDirPath("a/b"), "a/b/");
    XX_TEST_EXPECT_EQ(agentxx::util::toUnixStandardDirPath("a/b/"), "a/b/");
    XX_TEST_EXPECT_EQ(agentxx::util::toUnixStandardDirPath("a"), "a/");
}

void test_toCurrentSystemStandardPath() {
#if XX_IS_WIN_D
    XX_TEST_EXPECT_EQ(agentxx::util::toCurrentSystemStandardPath("a/b\\c"), "a\\b\\c");
    XX_TEST_EXPECT_EQ(agentxx::util::toCurrentSystemStandardPath("a//b"), "a\\b");
#else
    // WSL/Linux: 盘符路径转为 /mnt/<drive>/
    XX_TEST_EXPECT_EQ(agentxx::util::toCurrentSystemStandardPath("C:/Users/x"), "/mnt/c/Users/x");
    XX_TEST_EXPECT_EQ(agentxx::util::toCurrentSystemStandardPath("D:\\work\\a"), "/mnt/d/work/a");
    XX_TEST_EXPECT_EQ(agentxx::util::toCurrentSystemStandardPath("a/b\\c"), "a/b/c");
    // 小写盘符
    XX_TEST_EXPECT_EQ(agentxx::util::toCurrentSystemStandardPath("e:/x"), "/mnt/e/x");
#endif
}

void test_ignoreCaseContainers() {
    // IgnoreCaseSet
    agentxx::util::IgnoreCaseSet set;
    set.insert("Hello");
    XX_TEST_EXPECT_TRUE(set.contains("hello"));
    XX_TEST_EXPECT_TRUE(set.contains("HELLO"));
    XX_TEST_EXPECT_TRUE(set.contains("HeLLo"));
    XX_TEST_EXPECT_FALSE(set.contains("world"));

    // IgnoreCaseMap
    agentxx::util::IgnoreCaseMap<int> map;
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
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("a/b/.hidden", true), ".hidden");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("a/b/.hidden"), ".hidden");
    // 常规去扩展名
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("a/b/c.txt", true), "c");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("a/b/c.txt"), "c.txt");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("a/b/c.tar.gz", true), "c.tar");
    // 多级目录 + 尾分隔符
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("/a/b/c.txt/", true), "c.txt");
    // 仅扩展名
    XX_TEST_EXPECT_EQ(agentxx::util::getFileNameEXT("file.txt").value(), "txt");
    // useRigthDot=false: 使用最左侧点
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("a.b.c", true, false), "a");
    XX_TEST_EXPECT_EQ(agentxx::util::getFileName("a.b.c", true, true), "a.b");
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
    test_collapsePaths();
    test_toCurrentSystemStandardPath();
    test_ignoreCaseContainers();
    test_getFileNameMore();

    return TestResult{g_su_passed, g_su_failed};
}

} // namespace test
} // namespace agentxx
