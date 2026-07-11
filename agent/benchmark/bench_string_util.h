#pragma once

#include "agentxx/util/string_util.h"
#include "bench_util.h"
#include <string>
#include <vector>

namespace agentxx {
namespace bench {

inline void benchStringUtil() {
  std::cout << "\n=== string_util Benchmarks ===" << std::endl;

  {
    std::string text;
    for (int i = 0; i < 10000; ++i) {
      text += "Hello World 你好世界 12345 ";
    }

    auto r = runBench("utf8GetLength [250KB mixed text]", 100, [&]() {
      auto len = agentxx::util::utf8GetLength(text);
      (void)len;
    });
    printResult(r);
  }

  {
    std::string text;
    for (int i = 0; i < 10000; ++i) {
      text += "Hello World 你好世界 12345 ";
    }

    auto r = runBench("utf8GetLengthCheckAvail [250KB mixed text]", 100, [&]() {
      auto len = agentxx::util::utf8GetLengthCheckAvail(text);
      (void)len;
    });
    printResult(r);
  }

  {
    std::string text;
    for (int i = 0; i < 100000; ++i) {
      text += "Hello World 12345 ";
    }

    auto r = runBench("toUpper [2MB text]", 50, [&]() {
      auto result = agentxx::util::toUpper(text);
      (void)result;
    });
    printResult(r);
  }

  {
    std::string text;
    for (int i = 0; i < 100000; ++i) {
      text += "Hello World 12345 ";
    }

    auto r = runBench("toLower [2MB text]", 50, [&]() {
      auto result = agentxx::util::toLower(text);
      (void)result;
    });
    printResult(r);
  }

  {
    std::string text;
    for (int i = 0; i < 100000; ++i) {
      text += "field1,field2,field3,field4,field5\n";
    }

    auto r = runBench("strSplit [2MB CSV text]", 50, [&]() {
      auto result = agentxx::util::strSplit(text, ',');
      (void)result;
    });
    printResult(r);
  }

  {
    std::string text;
    for (int i = 0; i < 100000; ++i) {
      text += "field1,field2,field3,field4,field5\n";
    }

    auto r = runBench("strSplitCopid [2MB CSV text]", 50, [&]() {
      auto result = agentxx::util::strSplitCopid(text, ',');
      (void)result;
    });
    printResult(r);
  }

  {
    std::string text;
    for (int i = 0; i < 100000; ++i) {
      text += "  Hello World  \t\n";
    }

    auto r = runBench("removeBetweenSpace [2MB text]", 50, [&]() {
      auto result = agentxx::util::removeBetweenSpace(text);
      (void)result;
    });
    printResult(r);
  }

  {
    std::string text;
    for (int i = 0; i < 100000; ++i) {
      text += "  Hello World  \t\n";
    }

    auto r = runBench("removeAllSpace [2MB text]", 50, [&]() {
      auto result = agentxx::util::removeAllSpace(text);
      (void)result;
    });
    printResult(r);
  }

  {
    std::string text;
    for (int i = 0; i < 50000; ++i) {
      text += "///a///b\\\\c\\\\d///e\\\\";
    }

    auto r = runBench("toStandardPath [1MB path text]", 100, [&]() {
      auto result = agentxx::util::toStandardPath(text);
      (void)result;
    });
    printResult(r);
  }

  {
    std::string text;
    for (int i = 0; i < 50000; ++i) {
      text += "///a///b\\\\c\\\\d///e\\\\";
    }

    auto r = runBench("toUnixStandardPath [1MB path text]", 100, [&]() {
      auto result = agentxx::util::toUnixStandardPath(text);
      (void)result;
    });
    printResult(r);
  }

  {
    std::vector<std::string> data;
    data.reserve(100000);
    for (int i = 0; i < 100000; ++i) {
      data.push_back("item_" + std::to_string(i));
    }

    auto r = runBench("stringVectorJoin [100K items]", 50, [&]() {
      auto result = agentxx::util::stringVectorJoin(data, ", ");
      (void)result;
    });
    printResult(r);
  }

  {
    std::string data =
        "Hello, World! This is a test string for base64 encoding.";
    auto r = runBench("base64Encode [60 bytes]", 100000, [&]() {
      auto result = agentxx::util::base64Encode(data.data(), data.size());
      (void)result;
    });
    printResult(r);
  }

  {
    std::string data =
        "Hello, World! This is a test string for base64 encoding.";
    auto encoded = agentxx::util::base64Encode(data.data(), data.size());
    auto r = runBench("base64Decode [80 bytes]", 100000, [&]() {
      auto result = agentxx::util::base64Decode(encoded);
      (void)result;
    });
    printResult(r);
  }

  {
    std::string largeData(1024 * 100, 'X');
    auto r = runBench("base64Encode [100KB]", 1000, [&]() {
      auto result =
          agentxx::util::base64Encode(largeData.data(), largeData.size());
      (void)result;
    });
    printResult(r);
  }

  {
    std::string largeData(1024 * 100, 'X');
    auto encoded =
        agentxx::util::base64Encode(largeData.data(), largeData.size());
    auto r = runBench("base64Decode [100KB]", 1000, [&]() {
      auto result = agentxx::util::base64Decode(encoded);
      (void)result;
    });
    printResult(r);
  }

  {
    auto r = runBench("compareExtend [short strings]", 1000000, [&]() {
      auto result = agentxx::util::compareExtend("03.9,999 xxx", "01. xxx");
      (void)result;
    });
    printResult(r);
  }

  {
    auto r = runBench("compareExtend [numeric comparison]", 1000000, [&]() {
      auto result = agentxx::util::compareExtend("file_100.txt", "file_99.txt");
      (void)result;
    });
    printResult(r);
  }

  {
    auto r = runBench("isIgnoreCaseEqual [10-char strings]", 1000000, [&]() {
      auto result =
          agentxx::util::isIgnoreCaseEqual("HelloWorld", "helloworld");
      (void)result;
    });
    printResult(r);
  }

  {
    auto r =
        runBench("isIgnoreCaseContains [100-char haystack]", 1000000, [&]() {
          auto result = agentxx::util::isIgnoreCaseContains(
              "The quick brown fox jumps over the lazy dog The quick brown fox "
              "jumps over the lazy dog",
              "LAZY DOG");
          (void)result;
        });
    printResult(r);
  }

  {
    auto r = runBench("toArgument [short string]", 500000, [&]() {
      auto result = agentxx::util::toArgument("{\"enable_thinking\": false}");
      (void)result;
    });
    printResult(r);
  }

  {
    std::string text;
    for (int i = 0; i < 10000; ++i) {
      text += "Hello World 你好世界 12345 ";
    }

    auto r =
        runBench("findIndexByUtf8Length [250KB text, target=5000]", 100, [&]() {
          auto result = agentxx::util::findIndexByUtf8Length(text, 5000);
          (void)result;
        });
    printResult(r);
  }

  {
    std::string text;
    for (int i = 0; i < 10000; ++i) {
      text += "Hello World 你好世界 12345\n";
    }

    auto r = runBench(
        "findIndexAndLastLineIndexByUtf8Length [250KB text, target=5000]", 100,
        [&]() {
          auto result =
              agentxx::util::findIndexAndLastLineIndexByUtf8Length(text, 5000);
          (void)result;
        });
    printResult(r);
  }

  {
    auto r = runBench("utf8IsAvail [valid UTF-8 100 chars]", 100000, [&]() {
      auto result = agentxx::util::utf8IsAvail(
          "Hello World 你好世界 12345 Hello World 你好世界 12345 Hello World "
          "你好世界 12345 Hello World 你好世界 12345");
      (void)result;
    });
    printResult(r);
  }

  {
    auto r = runBench("getFileName [path extraction]", 1000000, [&]() {
      auto result = agentxx::util::getFileName("/home/user/docs/test_file.txt");
      (void)result;
    });
    printResult(r);
  }

  {
    auto r = runBench("getFileNameEXT [extension extraction]", 1000000, [&]() {
      auto result =
          agentxx::util::getFileNameEXT("/home/user/docs/test_file.txt");
      (void)result;
    });
    printResult(r);
  }

  {
    auto r =
        runBench("getParentDirPath [parent dir extraction]", 1000000, [&]() {
          auto result =
              agentxx::util::getParentDirPath("/home/user/docs/test_file.txt");
          (void)result;
        });
    printResult(r);
  }
}

} // namespace bench
} // namespace agentxx