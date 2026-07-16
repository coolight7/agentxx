#include "agentxx/util/string_util.h"
#include "test_framework.h"

#include <cassert>

using namespace agentxx::util;

inline static int g_su_passed = 0;
inline static int g_su_failed = 0;

#define EXPECT_EQ(expr, expected)                                              \
  do {                                                                         \
    auto _result = (expr);                                                     \
    auto _expected = (expected);                                               \
    if (_result == _expected) {                                                \
      g_su_passed++;                                                           \
    } else {                                                                   \
      g_su_failed++;                                                           \
      TEST_FAIL << "line " << __LINE__ << ": expected " << (_expected)         \
                << ", got " << (_result) << std::endl;                         \
    }                                                                          \
  } while (0)

#define EXPECT_TRUE(expr)                                                      \
  do {                                                                         \
    if (expr) {                                                                \
      g_su_passed++;                                                           \
    } else {                                                                   \
      g_su_failed++;                                                           \
      TEST_FAIL << "line " << __LINE__ << ": expected true" << std::endl;      \
    }                                                                          \
  } while (0)

#define EXPECT_FALSE(expr)                                                     \
  do {                                                                         \
    if (!(expr)) {                                                             \
      g_su_passed++;                                                           \
    } else {                                                                   \
      g_su_failed++;                                                           \
      TEST_FAIL << "line " << __LINE__ << ": expected false" << std::endl;     \
    }                                                                          \
  } while (0)

#define EXPECT_NULLOPT(expr)                                                   \
  do {                                                                         \
    if (!(expr).has_value()) {                                                 \
      g_su_passed++;                                                           \
    } else {                                                                   \
      g_su_failed++;                                                           \
      TEST_FAIL << "line " << __LINE__ << ": expected nullopt" << std::endl;   \
    }                                                                          \
  } while (0)

#define EXPECT_HAS_VALUE(expr)                                                 \
  do {                                                                         \
    if ((expr).has_value()) {                                                  \
      g_su_passed++;                                                           \
    } else {                                                                   \
      g_su_failed++;                                                           \
      TEST_FAIL << "line " << __LINE__ << ": expected has_value" << std::endl; \
    }                                                                          \
  } while (0)

#define shiftCompareExtend(left, right, sub)                                   \
  EXPECT_EQ(agentxx::util::compareExtend(left, right), sub);                   \
  EXPECT_EQ(agentxx::util::compareExtend(right, left), -(sub));

void test_compareExtend() {

  EXPECT_EQ(agentxx::util::compareExtend("", ""), 0);
  EXPECT_EQ(agentxx::util::compareExtend(" ", " "), 0);
  EXPECT_EQ(agentxx::util::compareExtend("123", "123"), 0);
  EXPECT_EQ(agentxx::util::compareExtend(" 123\t", " 123\t"), 0);
  EXPECT_EQ(
      agentxx::util::compareExtend(" #=k123abc\t\r\n", " #=k123abc\t\r\n"), 0);

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

  EXPECT_EQ(agentxx::util::toStandardPath("//////"), "/");
  EXPECT_EQ(agentxx::util::toStandardPath("\\\\\\"), "\\");
  EXPECT_EQ(agentxx::util::toStandardPath("\\\\\\/\\/\\////\\/"), "\\");
  EXPECT_EQ(agentxx::util::toStandardPath("a/b\\d"), "a/b\\d");
  EXPECT_EQ(agentxx::util::toStandardPath("a///b\\d"), "a/b\\d");
  EXPECT_EQ(agentxx::util::toStandardPath("a/b\\\\\\d"), "a/b\\d");
  EXPECT_EQ(agentxx::util::toStandardPath("a/////b\\\\d"), "a/b\\d");
  EXPECT_EQ(agentxx::util::toStandardPath("///a/b\\d"), "/a/b\\d");
  EXPECT_EQ(agentxx::util::toStandardPath("//a///b\\\\\\\\d/////"), "/a/b\\d/");
  EXPECT_EQ(agentxx::util::toStandardPath("\\\\\\a///b\\\\\\\\d\\\\\\"),
            "\\a/b\\d\\");
  EXPECT_EQ(agentxx::util::toStandardPath("/\\\\\\a//b\\/\\\\///d\\\\\\/"),
            "\\a/b\\d\\");
}

void test_toUnixStandardPath() {

  EXPECT_EQ(agentxx::util::toUnixStandardPath("\\\\\\\\\\"), "/");
  EXPECT_EQ(agentxx::util::toUnixStandardPath("\\\\\\\\\\//////\\/\\/\\/"),
            "/");
  EXPECT_EQ(agentxx::util::toUnixStandardPath("a/b/d"), "a/b/d");
  EXPECT_EQ(agentxx::util::toUnixStandardPath("a/b\\d"), "a/b/d");
  EXPECT_EQ(agentxx::util::toUnixStandardPath("\\a\\b/d\\"), "/a/b/d/");
  EXPECT_EQ(agentxx::util::toUnixStandardPath(
                "\\\\\\/\\/\\a/\\\\b\\\\\\/\\/d\\//\\/\\\\\\"),
            "/a/b/d/");
}

void test_DirFilePath() {

  EXPECT_EQ(agentxx::util::getFileName(""), "");
  EXPECT_EQ(agentxx::util::getFileName("."), ".");
  EXPECT_EQ(agentxx::util::getFileName("..."), "...");
  EXPECT_EQ(agentxx::util::getFileName("...///\\\\"), "...");
  EXPECT_EQ(agentxx::util::getFileName("/"), "/");
  EXPECT_EQ(agentxx::util::getFileName("/////"), "/////");
  EXPECT_EQ(agentxx::util::getFileName("\\"), "\\");
  EXPECT_EQ(agentxx::util::getFileName("\\\\\\\\\\"), "\\\\\\\\\\");
  EXPECT_EQ(agentxx::util::getFileName("///\\\\\\\\//\\\\"),
            "///\\\\\\\\//\\\\");
  EXPECT_EQ(agentxx::util::getFileName(".", true), ".");
  EXPECT_EQ(agentxx::util::getFileName("./.", true), ".");
  EXPECT_EQ(agentxx::util::getFileName("abc/..", true), "..");
  EXPECT_EQ(agentxx::util::getFileName("abc..123", true), "abc.");
  EXPECT_EQ(agentxx::util::getFileName("abc.123.tar.gz"), "abc.123.tar.gz");
  EXPECT_EQ(agentxx::util::getFileName("123"), "123");
  EXPECT_EQ(agentxx::util::getFileName("123/"), "123");
  EXPECT_EQ(agentxx::util::getFileName("123\\"), "123");
  EXPECT_EQ(agentxx::util::getFileName("./123"), "123");
  EXPECT_EQ(agentxx::util::getFileName(".\\123"), "123");
  EXPECT_EQ(agentxx::util::getFileName("./123.456/", true), "123.456");
  EXPECT_EQ(agentxx::util::getFileName("\\//455//\\\\123/\\\\//\\/\\\\\\\\"),
            "123");
  EXPECT_EQ(agentxx::util::getFileName(".///\\\\//\\\\/\\\\123"), "123");
  EXPECT_EQ(agentxx::util::getFileName("///\\\\//\\\\/\\\\\\\\//"),
            "///\\\\//\\\\/\\\\\\\\//");
  EXPECT_EQ(agentxx::util::getFileName(".///\\\\//\\\\/\\\\\\\\//"), ".");

  EXPECT_NULLOPT(agentxx::util::getFileNameEXT(""));
  EXPECT_NULLOPT(agentxx::util::getFileNameEXT("."));
  EXPECT_NULLOPT(agentxx::util::getFileNameEXT("..."));
  EXPECT_EQ(agentxx::util::getFileNameEXT("abc.name").value(), "name");
  EXPECT_NULLOPT(agentxx::util::getFileNameEXT("abc.name/"));
  EXPECT_NULLOPT(agentxx::util::getFileNameEXT("abc.name\\"));
  EXPECT_NULLOPT(agentxx::util::getFileNameEXT("./../..."));
  EXPECT_EQ(agentxx::util::getFileNameEXT("./../...name").value(), "name");
  EXPECT_NULLOPT(agentxx::util::getFileNameEXT("./../name..."));

  EXPECT_EQ(agentxx::util::replaceOrAppendExt("hello", "wav"), "hello.wav");
  EXPECT_EQ(agentxx::util::replaceOrAppendExt("hello.mp3", "wav"), "hello.wav");
  EXPECT_EQ(agentxx::util::replaceOrAppendExt("hello.f", "wav"), "hello.wav");
  EXPECT_EQ(agentxx::util::replaceOrAppendExt("hello.flac", "wav"),
            "hello.wav");
  EXPECT_EQ(agentxx::util::replaceOrAppendExt("hello.", "wav"), "hello.wav");
  EXPECT_EQ(agentxx::util::replaceOrAppendExt(".hello", "wav"), ".hello.wav");
  EXPECT_EQ(agentxx::util::replaceOrAppendExt(".hello.", "wav"), ".hello.wav");

  EXPECT_NULLOPT(agentxx::util::getParentDirPath(""));
  EXPECT_NULLOPT(agentxx::util::getParentDirPath("."));
  EXPECT_NULLOPT(agentxx::util::getParentDirPath("..."));
  EXPECT_NULLOPT(agentxx::util::getParentDirPath("...xx./"));
  EXPECT_EQ(agentxx::util::getParentDirPath("/...xx.").value(), "/");
  EXPECT_EQ(agentxx::util::getParentDirPath("/...xx./").value(), "/");
  EXPECT_EQ(agentxx::util::getParentDirPath("/...xx./xxx").value(), "/...xx./");
  EXPECT_EQ(agentxx::util::getParentDirPath("./xxx").value(), "./");
  EXPECT_EQ(agentxx::util::getParentDirPath("../xxx").value(), "../");
}

void test_removeSpace() {

  EXPECT_EQ(agentxx::util::removeAllSpace(""), "");
  EXPECT_EQ(agentxx::util::removeAllSpace("  \t \t     "), "");
  EXPECT_EQ(agentxx::util::removeAllSpace("   1 2   3 "), "123");
  EXPECT_EQ(agentxx::util::removeAllSpace("\t   1\t  \t2   3 \t"), "123");

  EXPECT_NULLOPT(agentxx::util::removeAllSpaceMayNull(""));
  EXPECT_NULLOPT(agentxx::util::removeAllSpaceMayNull("     "));
  EXPECT_NULLOPT(agentxx::util::removeAllSpaceMayNull("\t  \t  \t   \t"));
  EXPECT_EQ(agentxx::util::removeAllSpaceMayNull("   1 2   3 ").value(), "123");
  EXPECT_EQ(
      agentxx::util::removeAllSpaceMayNull("\t   1\t  \t2   3 \t").value(),
      "123");

  EXPECT_EQ(agentxx::util::removeBetweenSpace(""), "");
  EXPECT_EQ(agentxx::util::removeBetweenSpace("  "), "");
  EXPECT_EQ(agentxx::util::removeBetweenSpace("\t\t\t"), "");
  EXPECT_EQ(agentxx::util::removeBetweenSpace("\t   \t      \t"), "");
  EXPECT_EQ(agentxx::util::removeBetweenSpace("   1 2   3 "), "1 2   3");
  EXPECT_EQ(agentxx::util::removeBetweenSpace("\t   1\t  \t2   3 \t"),
            "1\t  \t2   3");
  EXPECT_EQ(agentxx::util::removeBetweenSpace(" \n \r  1 2   3 \n\r", false),
            "\n \r  1 2   3 \n\r");
  EXPECT_EQ(agentxx::util::removeBetweenSpace("\n \r  1 2   3\n\r", false),
            "\n \r  1 2   3\n\r");
  EXPECT_EQ(agentxx::util::removeBetweenSpace("\n \r  1 2   3\n\r  ", false),
            "\n \r  1 2   3\n\r");
  EXPECT_EQ(agentxx::util::removeBetweenSpace(" \n \r  1 2   3 \n\r"),
            "1 2   3");
  EXPECT_EQ(agentxx::util::removeBetweenSpace("\n \r  1 2   3\n\r  "),
            "1 2   3");
  EXPECT_EQ(agentxx::util::removeBetweenSpace("\n \r  1 2   3\n\r  ", true,
                                              false, true),
            "\n \r  1 2   3");
  EXPECT_EQ(agentxx::util::removeBetweenSpace("\n \r  1 2   3\n\r  ", true,
                                              true, false),
            "1 2   3\n\r  ");

  EXPECT_NULLOPT(agentxx::util::removeBetweenSpaceMayNull(""));
  EXPECT_NULLOPT(agentxx::util::removeBetweenSpaceMayNull("  "));
  EXPECT_NULLOPT(agentxx::util::removeBetweenSpaceMayNull("\t\t\t"));
  EXPECT_NULLOPT(agentxx::util::removeBetweenSpaceMayNull("\t   \t      \t"));
  EXPECT_EQ(agentxx::util::removeBetweenSpaceMayNull("   1 2   3 ").value(),
            "1 2   3");
  EXPECT_EQ(
      agentxx::util::removeBetweenSpaceMayNull("\t   1\t  \t2   3 \t").value(),
      "1\t  \t2   3");
}

void test_isIgnoreCaseEqual() {

  EXPECT_TRUE(agentxx::util::isIgnoreCaseEqual("", ""));
  EXPECT_TRUE(agentxx::util::isIgnoreCaseEqual(" ", " "));
  EXPECT_TRUE(agentxx::util::isIgnoreCaseEqual("123abcABC", "123abcABC"));
  EXPECT_TRUE(agentxx::util::isIgnoreCaseEqual("123abcABC", "123ABCabc"));
  EXPECT_TRUE(agentxx::util::isIgnoreCaseEqual("abc", "AbC"));
  EXPECT_TRUE(agentxx::util::isIgnoreCaseEqual("abc\n", "AbC\n"));

  EXPECT_FALSE(agentxx::util::isIgnoreCaseEqual("", "     "));
  EXPECT_FALSE(agentxx::util::isIgnoreCaseEqual("abc\n\r", "ABC"));
}

void test_isIgnoreCaseContains() {

  EXPECT_TRUE(agentxx::util::isIgnoreCaseContains("", ""));
  EXPECT_TRUE(agentxx::util::isIgnoreCaseContains(" ", " "));
  EXPECT_TRUE(agentxx::util::isIgnoreCaseContains("   ", ""));
  EXPECT_TRUE(
      agentxx::util::isIgnoreCaseContains("123abcABC +++ ", "123abcABC"));
  EXPECT_TRUE(agentxx::util::isIgnoreCaseContains("abcAbC", "AbC"));
  EXPECT_TRUE(agentxx::util::isIgnoreCaseContains("AbCabc", "AbC"));
  EXPECT_TRUE(agentxx::util::isIgnoreCaseContains("abc\n1fdfaf56as", "AbC\n"));
  EXPECT_TRUE(agentxx::util::isIgnoreCaseContains(
      "  你 好 你 好AbC\n1fdfaf56as", "你 好AbC\n"));

  EXPECT_FALSE(
      agentxx::util::isIgnoreCaseContains("123abcABC", "123abcABC +++ "));
  EXPECT_FALSE(agentxx::util::isIgnoreCaseContains("", "     "));
  EXPECT_FALSE(agentxx::util::isIgnoreCaseContains("你 好abc\n\r", "不 好ABC"));

  EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny("", ""));
  EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny(" ", " "));
  EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny("", "     "));
  EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny("123abcABC", "123abcABC"));
  EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny(" dddabc", "AbC"));
  EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny("AbC", " dddabc"));
  EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny("ABCddd ", "AbC"));
  EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny("AbC", "ABCddd "));
  EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny(
      "  你 好 你 好aBc\n1fdfaf56as", "你 好AbC\n"));
  EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny(
      "你 好AbC\n", "  你 好 你 好aBc\n1fdfaf56as"));

  EXPECT_FALSE(agentxx::util::isIgnoreCaseContainsAny("你  好abc", "不 好ABC"));
  EXPECT_FALSE(
      agentxx::util::isIgnoreCaseContainsAny("你 好abc\n\r", "不 好ABC"));

  EXPECT_TRUE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny(" ", " "));
  EXPECT_TRUE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("123abcABC",
                                                                "123abcABC"));
  EXPECT_TRUE(
      agentxx::util::isNotEmptyAndIgnoreCaseContainsAny(" dddabc", "AbC"));
  EXPECT_TRUE(
      agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("AbC", " dddabc"));
  EXPECT_TRUE(
      agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("ABCddd ", "AbC"));
  EXPECT_TRUE(
      agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("AbC", "ABCddd "));
  EXPECT_TRUE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny(
      "AbC\n1fdfaf56as", "AbC\n"));

  EXPECT_FALSE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("", ""));
  EXPECT_FALSE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("   ", ""));
  EXPECT_FALSE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("", "     "));
  EXPECT_FALSE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("你  好abc",
                                                                 "不 好ABC"));
  EXPECT_FALSE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("你 好abc\n\r",
                                                                 "不 好ABC"));
}

void test_toArgument() {

  EXPECT_EQ(agentxx::util::toArgument(""), "\"\"");
  EXPECT_EQ(agentxx::util::toArgument("\"\""), "\"\\\"\\\"\"");
  EXPECT_EQ(agentxx::util::toArgument("{\"enable_thinking\": false}"),
            "\"{\\\"enable_thinking\\\": false}\"");
  EXPECT_EQ(agentxx::util::toArgument("\"hh\", --"), "\"\\\"hh\\\", --\"");
  EXPECT_EQ(agentxx::util::toArgument("\"\"\", --"), "\"\\\"\\\"\\\", --\"");
  EXPECT_EQ(agentxx::util::toArgument("\\\"\"\\\", --"),
            "\"\\\"\\\"\\\", --\"");
  EXPECT_EQ(agentxx::util::toArgument("\"\\\"\", --"), "\"\\\"\\\"\\\", --\"");
  EXPECT_EQ(agentxx::util::toArgument("\"wow\""), "\"\\\"wow\\\"\"");
  EXPECT_EQ(agentxx::util::toArgument("\\\"wow\\\""), "\"\\\"wow\\\"\"");
}

namespace agentxx {
namespace test {

TestResult testStringUtil() {
  test_compareExtend();
  test_toStandardPath();
  test_toUnixStandardPath();
  test_DirFilePath();
  test_removeSpace();
  test_isIgnoreCaseEqual();
  test_isIgnoreCaseContains();
  test_toArgument();

  return TestResult{g_su_passed, g_su_failed};
}

} // namespace test
} // namespace agentxx