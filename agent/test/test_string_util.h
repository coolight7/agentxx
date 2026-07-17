#include "agentxx/util/string_util.h"
#include "test_framework.h"

#include <cassert>

using namespace agentxx::util;

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_su_passed
#define XX_TEST_FAILED g_su_failed

inline static int g_su_passed = 0;
inline static int g_su_failed = 0;

#define shiftCompareExtend(left, right, sub)                                   \
  XX_TEST_EXPECT_EQ(agentxx::util::compareExtend(left, right), sub);           \
  XX_TEST_EXPECT_EQ(agentxx::util::compareExtend(right, left), -(sub));

void test_compareExtend() {

  XX_TEST_EXPECT_EQ(agentxx::util::compareExtend("", ""), 0);
  XX_TEST_EXPECT_EQ(agentxx::util::compareExtend(" ", " "), 0);
  XX_TEST_EXPECT_EQ(agentxx::util::compareExtend("123", "123"), 0);
  XX_TEST_EXPECT_EQ(agentxx::util::compareExtend(" 123\t", " 123\t"), 0);
  XX_TEST_EXPECT_EQ(
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

  XX_TEST_EXPECT_EQ(agentxx::util::toStandardPath("//////"), "/");
  XX_TEST_EXPECT_EQ(agentxx::util::toStandardPath("\\\\\\"), "\\");
  XX_TEST_EXPECT_EQ(agentxx::util::toStandardPath("\\\\\\/\\/\\////\\/"), "\\");
  XX_TEST_EXPECT_EQ(agentxx::util::toStandardPath("a/b\\d"), "a/b\\d");
  XX_TEST_EXPECT_EQ(agentxx::util::toStandardPath("a///b\\d"), "a/b\\d");
  XX_TEST_EXPECT_EQ(agentxx::util::toStandardPath("a/b\\\\\\d"), "a/b\\d");
  XX_TEST_EXPECT_EQ(agentxx::util::toStandardPath("a/////b\\\\d"), "a/b\\d");
  XX_TEST_EXPECT_EQ(agentxx::util::toStandardPath("///a/b\\d"), "/a/b\\d");
  XX_TEST_EXPECT_EQ(agentxx::util::toStandardPath("//a///b\\\\\\\\d/////"),
                    "/a/b\\d/");
  XX_TEST_EXPECT_EQ(agentxx::util::toStandardPath("\\\\\\a///b\\\\\\\\d\\\\\\"),
                    "\\a/b\\d\\");
  XX_TEST_EXPECT_EQ(
      agentxx::util::toStandardPath("/\\\\\\a//b\\/\\\\///d\\\\\\/"),
      "\\a/b\\d\\");
}

void test_toUnixStandardPath() {

  XX_TEST_EXPECT_EQ(agentxx::util::toUnixStandardPath("\\\\\\\\\\"), "/");
  XX_TEST_EXPECT_EQ(
      agentxx::util::toUnixStandardPath("\\\\\\\\\\//////\\/\\/\\/"), "/");
  XX_TEST_EXPECT_EQ(agentxx::util::toUnixStandardPath("a/b/d"), "a/b/d");
  XX_TEST_EXPECT_EQ(agentxx::util::toUnixStandardPath("a/b\\d"), "a/b/d");
  XX_TEST_EXPECT_EQ(agentxx::util::toUnixStandardPath("\\a\\b/d\\"), "/a/b/d/");
  XX_TEST_EXPECT_EQ(agentxx::util::toUnixStandardPath(
                        "\\\\\\/\\/\\a/\\\\b\\\\\\/\\/d\\//\\/\\\\\\"),
                    "/a/b/d/");
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
  XX_TEST_EXPECT_EQ(agentxx::util::getFileName("///\\\\\\\\//\\\\"),
                    "///\\\\\\\\//\\\\");
  XX_TEST_EXPECT_EQ(agentxx::util::getFileName(".", true), ".");
  XX_TEST_EXPECT_EQ(agentxx::util::getFileName("./.", true), ".");
  XX_TEST_EXPECT_EQ(agentxx::util::getFileName("abc/..", true), "..");
  XX_TEST_EXPECT_EQ(agentxx::util::getFileName("abc..123", true), "abc.");
  XX_TEST_EXPECT_EQ(agentxx::util::getFileName("abc.123.tar.gz"),
                    "abc.123.tar.gz");
  XX_TEST_EXPECT_EQ(agentxx::util::getFileName("123"), "123");
  XX_TEST_EXPECT_EQ(agentxx::util::getFileName("123/"), "123");
  XX_TEST_EXPECT_EQ(agentxx::util::getFileName("123\\"), "123");
  XX_TEST_EXPECT_EQ(agentxx::util::getFileName("./123"), "123");
  XX_TEST_EXPECT_EQ(agentxx::util::getFileName(".\\123"), "123");
  XX_TEST_EXPECT_EQ(agentxx::util::getFileName("./123.456/", true), "123.456");
  XX_TEST_EXPECT_EQ(
      agentxx::util::getFileName("\\//455//\\\\123/\\\\//\\/\\\\\\\\"), "123");
  XX_TEST_EXPECT_EQ(agentxx::util::getFileName(".///\\\\//\\\\/\\\\123"),
                    "123");
  XX_TEST_EXPECT_EQ(agentxx::util::getFileName("///\\\\//\\\\/\\\\\\\\//"),
                    "///\\\\//\\\\/\\\\\\\\//");
  XX_TEST_EXPECT_EQ(agentxx::util::getFileName(".///\\\\//\\\\/\\\\\\\\//"),
                    ".");

  XX_TEST_EXPECT_NULLOPT(agentxx::util::getFileNameEXT(""));
  XX_TEST_EXPECT_NULLOPT(agentxx::util::getFileNameEXT("."));
  XX_TEST_EXPECT_NULLOPT(agentxx::util::getFileNameEXT("..."));
  XX_TEST_EXPECT_EQ(agentxx::util::getFileNameEXT("abc.name").value(), "name");
  XX_TEST_EXPECT_NULLOPT(agentxx::util::getFileNameEXT("abc.name/"));
  XX_TEST_EXPECT_NULLOPT(agentxx::util::getFileNameEXT("abc.name\\"));
  XX_TEST_EXPECT_NULLOPT(agentxx::util::getFileNameEXT("./../..."));
  XX_TEST_EXPECT_EQ(agentxx::util::getFileNameEXT("./../...name").value(),
                    "name");
  XX_TEST_EXPECT_NULLOPT(agentxx::util::getFileNameEXT("./../name..."));

  XX_TEST_EXPECT_EQ(agentxx::util::replaceOrAppendExt("hello", "wav"),
                    "hello.wav");
  XX_TEST_EXPECT_EQ(agentxx::util::replaceOrAppendExt("hello.mp3", "wav"),
                    "hello.wav");
  XX_TEST_EXPECT_EQ(agentxx::util::replaceOrAppendExt("hello.f", "wav"),
                    "hello.wav");
  XX_TEST_EXPECT_EQ(agentxx::util::replaceOrAppendExt("hello.flac", "wav"),
                    "hello.wav");
  XX_TEST_EXPECT_EQ(agentxx::util::replaceOrAppendExt("hello.", "wav"),
                    "hello.wav");
  XX_TEST_EXPECT_EQ(agentxx::util::replaceOrAppendExt(".hello", "wav"),
                    ".hello.wav");
  XX_TEST_EXPECT_EQ(agentxx::util::replaceOrAppendExt(".hello.", "wav"),
                    ".hello.wav");

  XX_TEST_EXPECT_NULLOPT(agentxx::util::getParentDirPath(""));
  XX_TEST_EXPECT_NULLOPT(agentxx::util::getParentDirPath("."));
  XX_TEST_EXPECT_NULLOPT(agentxx::util::getParentDirPath("..."));
  XX_TEST_EXPECT_NULLOPT(agentxx::util::getParentDirPath("...xx./"));
  XX_TEST_EXPECT_EQ(agentxx::util::getParentDirPath("/...xx.").value(), "/");
  XX_TEST_EXPECT_EQ(agentxx::util::getParentDirPath("/...xx./").value(), "/");
  XX_TEST_EXPECT_EQ(agentxx::util::getParentDirPath("/...xx./xxx").value(),
                    "/...xx./");
  XX_TEST_EXPECT_EQ(agentxx::util::getParentDirPath("./xxx").value(), "./");
  XX_TEST_EXPECT_EQ(agentxx::util::getParentDirPath("../xxx").value(), "../");
}

void test_removeSpace() {

  XX_TEST_EXPECT_EQ(agentxx::util::removeAllSpace(""), "");
  XX_TEST_EXPECT_EQ(agentxx::util::removeAllSpace("  \t \t     "), "");
  XX_TEST_EXPECT_EQ(agentxx::util::removeAllSpace("   1 2   3 "), "123");
  XX_TEST_EXPECT_EQ(agentxx::util::removeAllSpace("\t   1\t  \t2   3 \t"),
                    "123");

  XX_TEST_EXPECT_NULLOPT(agentxx::util::removeAllSpaceMayNull(""));
  XX_TEST_EXPECT_NULLOPT(agentxx::util::removeAllSpaceMayNull("     "));
  XX_TEST_EXPECT_NULLOPT(
      agentxx::util::removeAllSpaceMayNull("\t  \t  \t   \t"));
  XX_TEST_EXPECT_EQ(agentxx::util::removeAllSpaceMayNull("   1 2   3 ").value(),
                    "123");
  XX_TEST_EXPECT_EQ(
      agentxx::util::removeAllSpaceMayNull("\t   1\t  \t2   3 \t").value(),
      "123");

  XX_TEST_EXPECT_EQ(agentxx::util::removeBetweenSpace(""), "");
  XX_TEST_EXPECT_EQ(agentxx::util::removeBetweenSpace("  "), "");
  XX_TEST_EXPECT_EQ(agentxx::util::removeBetweenSpace("\t\t\t"), "");
  XX_TEST_EXPECT_EQ(agentxx::util::removeBetweenSpace("\t   \t      \t"), "");
  XX_TEST_EXPECT_EQ(agentxx::util::removeBetweenSpace("   1 2   3 "),
                    "1 2   3");
  XX_TEST_EXPECT_EQ(agentxx::util::removeBetweenSpace("\t   1\t  \t2   3 \t"),
                    "1\t  \t2   3");
  XX_TEST_EXPECT_EQ(
      agentxx::util::removeBetweenSpace(" \n \r  1 2   3 \n\r", false),
      "\n \r  1 2   3 \n\r");
  XX_TEST_EXPECT_EQ(
      agentxx::util::removeBetweenSpace("\n \r  1 2   3\n\r", false),
      "\n \r  1 2   3\n\r");
  XX_TEST_EXPECT_EQ(
      agentxx::util::removeBetweenSpace("\n \r  1 2   3\n\r  ", false),
      "\n \r  1 2   3\n\r");
  XX_TEST_EXPECT_EQ(agentxx::util::removeBetweenSpace(" \n \r  1 2   3 \n\r"),
                    "1 2   3");
  XX_TEST_EXPECT_EQ(agentxx::util::removeBetweenSpace("\n \r  1 2   3\n\r  "),
                    "1 2   3");
  XX_TEST_EXPECT_EQ(agentxx::util::removeBetweenSpace("\n \r  1 2   3\n\r  ",
                                                      true, false, true),
                    "\n \r  1 2   3");
  XX_TEST_EXPECT_EQ(agentxx::util::removeBetweenSpace("\n \r  1 2   3\n\r  ",
                                                      true, true, false),
                    "1 2   3\n\r  ");

  XX_TEST_EXPECT_NULLOPT(agentxx::util::removeBetweenSpaceMayNull(""));
  XX_TEST_EXPECT_NULLOPT(agentxx::util::removeBetweenSpaceMayNull("  "));
  XX_TEST_EXPECT_NULLOPT(agentxx::util::removeBetweenSpaceMayNull("\t\t\t"));
  XX_TEST_EXPECT_NULLOPT(
      agentxx::util::removeBetweenSpaceMayNull("\t   \t      \t"));
  XX_TEST_EXPECT_EQ(
      agentxx::util::removeBetweenSpaceMayNull("   1 2   3 ").value(),
      "1 2   3");
  XX_TEST_EXPECT_EQ(
      agentxx::util::removeBetweenSpaceMayNull("\t   1\t  \t2   3 \t").value(),
      "1\t  \t2   3");
}

void test_isIgnoreCaseEqual() {

  XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseEqual("", ""));
  XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseEqual(" ", " "));
  XX_TEST_EXPECT_TRUE(
      agentxx::util::isIgnoreCaseEqual("123abcABC", "123abcABC"));
  XX_TEST_EXPECT_TRUE(
      agentxx::util::isIgnoreCaseEqual("123abcABC", "123ABCabc"));
  XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseEqual("abc", "AbC"));
  XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseEqual("abc\n", "AbC\n"));

  XX_TEST_EXPECT_FALSE(agentxx::util::isIgnoreCaseEqual("", "     "));
  XX_TEST_EXPECT_FALSE(agentxx::util::isIgnoreCaseEqual("abc\n\r", "ABC"));
}

void test_isIgnoreCaseContains() {

  XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContains("", ""));
  XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContains(" ", " "));
  XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContains("   ", ""));
  XX_TEST_EXPECT_TRUE(
      agentxx::util::isIgnoreCaseContains("123abcABC +++ ", "123abcABC"));
  XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContains("abcAbC", "AbC"));
  XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContains("AbCabc", "AbC"));
  XX_TEST_EXPECT_TRUE(
      agentxx::util::isIgnoreCaseContains("abc\n1fdfaf56as", "AbC\n"));
  XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContains(
      "  你 好 你 好AbC\n1fdfaf56as", "你 好AbC\n"));

  XX_TEST_EXPECT_FALSE(
      agentxx::util::isIgnoreCaseContains("123abcABC", "123abcABC +++ "));
  XX_TEST_EXPECT_FALSE(agentxx::util::isIgnoreCaseContains("", "     "));
  XX_TEST_EXPECT_FALSE(
      agentxx::util::isIgnoreCaseContains("你 好abc\n\r", "不 好ABC"));

  XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny("", ""));
  XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny(" ", " "));
  XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny("", "     "));
  XX_TEST_EXPECT_TRUE(
      agentxx::util::isIgnoreCaseContainsAny("123abcABC", "123abcABC"));
  XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny(" dddabc", "AbC"));
  XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny("AbC", " dddabc"));
  XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny("ABCddd ", "AbC"));
  XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny("AbC", "ABCddd "));
  XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny(
      "  你 好 你 好aBc\n1fdfaf56as", "你 好AbC\n"));
  XX_TEST_EXPECT_TRUE(agentxx::util::isIgnoreCaseContainsAny(
      "你 好AbC\n", "  你 好 你 好aBc\n1fdfaf56as"));

  XX_TEST_EXPECT_FALSE(
      agentxx::util::isIgnoreCaseContainsAny("你  好abc", "不 好ABC"));
  XX_TEST_EXPECT_FALSE(
      agentxx::util::isIgnoreCaseContainsAny("你 好abc\n\r", "不 好ABC"));

  XX_TEST_EXPECT_TRUE(
      agentxx::util::isNotEmptyAndIgnoreCaseContainsAny(" ", " "));
  XX_TEST_EXPECT_TRUE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny(
      "123abcABC", "123abcABC"));
  XX_TEST_EXPECT_TRUE(
      agentxx::util::isNotEmptyAndIgnoreCaseContainsAny(" dddabc", "AbC"));
  XX_TEST_EXPECT_TRUE(
      agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("AbC", " dddabc"));
  XX_TEST_EXPECT_TRUE(
      agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("ABCddd ", "AbC"));
  XX_TEST_EXPECT_TRUE(
      agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("AbC", "ABCddd "));
  XX_TEST_EXPECT_TRUE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny(
      "AbC\n1fdfaf56as", "AbC\n"));

  XX_TEST_EXPECT_FALSE(
      agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("", ""));
  XX_TEST_EXPECT_FALSE(
      agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("   ", ""));
  XX_TEST_EXPECT_FALSE(
      agentxx::util::isNotEmptyAndIgnoreCaseContainsAny("", "     "));
  XX_TEST_EXPECT_FALSE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny(
      "你  好abc", "不 好ABC"));
  XX_TEST_EXPECT_FALSE(agentxx::util::isNotEmptyAndIgnoreCaseContainsAny(
      "你 好abc\n\r", "不 好ABC"));
}

void test_toArgument() {

  XX_TEST_EXPECT_EQ(agentxx::util::toArgument(""), "\"\"");
  XX_TEST_EXPECT_EQ(agentxx::util::toArgument("\"\""), "\"\\\"\\\"\"");
  XX_TEST_EXPECT_EQ(agentxx::util::toArgument("{\"enable_thinking\": false}"),
                    "\"{\\\"enable_thinking\\\": false}\"");
  XX_TEST_EXPECT_EQ(agentxx::util::toArgument("\"hh\", --"),
                    "\"\\\"hh\\\", --\"");
  XX_TEST_EXPECT_EQ(agentxx::util::toArgument("\"\"\", --"),
                    "\"\\\"\\\"\\\", --\"");
  XX_TEST_EXPECT_EQ(agentxx::util::toArgument("\\\"\"\\\", --"),
                    "\"\\\"\\\"\\\", --\"");
  XX_TEST_EXPECT_EQ(agentxx::util::toArgument("\"\\\"\", --"),
                    "\"\\\"\\\"\\\", --\"");
  XX_TEST_EXPECT_EQ(agentxx::util::toArgument("\"wow\""), "\"\\\"wow\\\"\"");
  XX_TEST_EXPECT_EQ(agentxx::util::toArgument("\\\"wow\\\""),
                    "\"\\\"wow\\\"\"");
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