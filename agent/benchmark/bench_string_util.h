#pragma once

#include "utilxx_base/string_util.h"
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
            auto len = utilxx_base::utf8GetLength(text);
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
            auto len = utilxx_base::utf8GetLengthCheckAvail(text);
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
            auto result = utilxx_base::toUpper(text);
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
            auto result = utilxx_base::toLower(text);
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
            auto result = utilxx_base::strSplit(text, ',');
            (void)result;
        });
        printResult(r);
    }

    {
        std::string text;
        for (int i = 0; i < 100000; ++i) {
            text += "field1,field2,field3,field4,field5\n";
        }

        auto r = runBench("strSplitCopied [2MB CSV text]", 50, [&]() {
            auto result = utilxx_base::strSplitCopied(text, ',');
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
            auto result = utilxx_base::removeBetweenSpace(text);
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
            auto result = utilxx_base::removeAllSpace(text);
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
            auto result = utilxx_base::toStandardPath(text);
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
            auto result = utilxx_base::toUnixStandardPath(text);
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
            auto result = utilxx_base::stringVectorJoin(data, ", ");
            (void)result;
        });
        printResult(r);
    }

    {
        std::string data = "Hello, World! This is a test string for base64 encoding.";
        auto        r    = runBench("base64Encode [60 bytes]", 100000, [&]() {
            auto result = utilxx_base::base64Encode(data);
            (void)result;
        });
        printResult(r);
    }

    {
        std::string data    = "Hello, World! This is a test string for base64 encoding.";
        auto        encoded = utilxx_base::base64Encode(data);
        auto        r       = runBench("base64Decode [80 bytes]", 100000, [&]() {
            auto result = utilxx_base::base64Decode(encoded);
            (void)result;
        });
        printResult(r);
    }

    {
        std::string largeData(1024 * 100, 'X');
        auto        r = runBench("base64Encode [100KB]", 1000, [&]() {
            auto result = utilxx_base::base64Encode(largeData);
            (void)result;
        });
        printResult(r);
    }

    {
        std::string largeData(1024 * 100, 'X');
        auto        encoded = utilxx_base::base64Encode(largeData);
        auto        r       = runBench("base64Decode [100KB]", 1000, [&]() {
            auto result = utilxx_base::base64Decode(encoded);
            (void)result;
        });
        printResult(r);
    }

    {
        auto r = runBench("compareExtend [short strings]", 1000000, [&]() {
            auto result = utilxx_base::compareExtend("03.9,999 xxx", "01. xxx");
            (void)result;
        });
        printResult(r);
    }

    {
        auto r = runBench("compareExtend [numeric comparison]", 1000000, [&]() {
            auto result = utilxx_base::compareExtend("file_100.txt", "file_99.txt");
            (void)result;
        });
        printResult(r);
    }

    {
        auto r = runBench("isIgnoreCaseEqual [10-char strings]", 1000000, [&]() {
            auto result = utilxx_base::isIgnoreCaseEqual("HelloWorld", "helloworld");
            (void)result;
        });
        printResult(r);
    }

    {
        auto r = runBench("isIgnoreCaseContains [100-char haystack]", 1000000, [&]() {
            auto result = utilxx_base::isIgnoreCaseContains(
                "The quick brown fox jumps over the lazy dog The quick brown fox "
                "jumps over the lazy dog",
                "LAZY DOG"
            );
            (void)result;
        });
        printResult(r);
    }

    {
        auto r = runBench("toArgument [short string]", 500000, [&]() {
            auto result = utilxx_base::toArgument("{\"enable_thinking\": false}");
            (void)result;
        });
        printResult(r);
    }

    {
        std::string text;
        for (int i = 0; i < 10000; ++i) {
            text += "Hello World 你好世界 12345 ";
        }

        auto r = runBench("findIndexByUtf8Length [250KB text, target=5000]", 100, [&]() {
            auto result = utilxx_base::findIndexByUtf8Length(text, 5000);
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
            "findIndexAndLastLineIndexByUtf8Length [250KB text, target=5000]",
            100,
            [&]() {
                auto result = utilxx_base::findIndexAndLastLineIndexByUtf8Length(text, 5000);
                (void)result;
            }
        );
        printResult(r);
    }

    {
        auto r = runBench("utf8IsAvail [valid UTF-8 100 chars]", 100000, [&]() {
            auto result = utilxx_base::utf8IsAvail(
                "Hello World 你好世界 12345 Hello World 你好世界 12345 Hello World "
                "你好世界 12345 Hello World 你好世界 12345"
            );
            (void)result;
        });
        printResult(r);
    }

    {
        auto r = runBench("getFileName [path extraction]", 1000000, [&]() {
            auto result = utilxx_base::getFileName("/home/user/docs/test_file.txt");
            (void)result;
        });
        printResult(r);
    }

    {
        auto r = runBench("getFileNameEXT [extension extraction]", 1000000, [&]() {
            auto result = utilxx_base::getFileNameEXT("/home/user/docs/test_file.txt");
            (void)result;
        });
        printResult(r);
    }

    {
        auto r = runBench("getParentDirPath [parent dir extraction]", 1000000, [&]() {
            auto result = utilxx_base::getParentDirPath("/home/user/docs/test_file.txt");
            (void)result;
        });
        printResult(r);
    }
}

} // namespace bench
} // namespace agentxx