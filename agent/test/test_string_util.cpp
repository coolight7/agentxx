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

    return TestResult{g_su_passed, g_su_failed};
}

} // namespace test
} // namespace agentxx
