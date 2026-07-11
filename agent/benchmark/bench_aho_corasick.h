#pragma once

#include "agentxx/util/aho_corasick.h"
#include "bench_util.h"
#include <string>
#include <vector>

namespace agentxx {
namespace bench {

inline void benchAhoCorasick() {
  std::cout << "\n=== AhoCorasick Benchmarks ===" << std::endl;

  {
    std::vector<std::string> patterns = {"hello", "world", "foo", "bar", "baz"};
    std::string text;
    for (int i = 0; i < 10000; ++i) {
      text += "hello world foo bar baz this is a test string for benchmarking ";
    }

    auto r =
        runBench("AhoCorasick::search [5 patterns, 500KB text]", 100, [&]() {
          agentxx::util::AhoCorasick<char> ac(patterns);
          auto result = ac.search(text);
          (void)result;
        });
    printResult(r);
  }

  {
    std::vector<std::string> patterns = {"hello", "world", "foo", "bar", "baz"};
    agentxx::util::AhoCorasick<char> ac(patterns);
    std::string text;
    for (int i = 0; i < 10000; ++i) {
      text += "hello world foo bar baz this is a test string for benchmarking ";
    }

    auto r = runBench("AhoCorasick::search [pre-built, 5 patterns, 500KB text]",
                      200, [&]() {
                        auto result = ac.search(text);
                        (void)result;
                      });
    printResult(r);
  }

  {
    std::vector<std::string> patterns;
    for (int i = 0; i < 100; ++i) {
      patterns.push_back("pattern_" + std::to_string(i));
    }
    std::string text;
    for (int i = 0; i < 5000; ++i) {
      text += "pattern_42 pattern_0 some text pattern_99 more text ";
    }

    auto r =
        runBench("AhoCorasick::search [100 patterns, 250KB text]", 50, [&]() {
          agentxx::util::AhoCorasick<char> ac(patterns);
          auto result = ac.search(text);
          (void)result;
        });
    printResult(r);
  }

  {
    std::vector<std::string> patterns = {"hello", "world"};
    agentxx::util::AhoCorasick<char> ac(patterns);
    std::string text;
    for (int i = 0; i < 10000; ++i) {
      text += "hello world this is a test ";
    }

    auto r =
        runBench("AhoCorasick::removeAll [2 patterns, 250KB text]", 100, [&]() {
          auto result = ac.removeAll(text);
          (void)result;
        });
    printResult(r);
  }

  {
    std::vector<std::string> patterns = {"<script>", "</script>", "<style>",
                                         "</style>"};
    agentxx::util::AhoCorasick<char> ac(patterns);
    std::string text;
    for (int i = 0; i < 5000; ++i) {
      text += "<script>alert('xss')</script><style>body{}</style>normal text ";
    }

    auto r = runBench("AhoCorasick::removeAll [HTML tag patterns, 250KB text]",
                      100, [&]() {
                        auto result = ac.removeAll(text);
                        (void)result;
                      });
    printResult(r);
  }

  {
    std::vector<std::string> patterns = {"hello"};
    agentxx::util::AhoCorasick<char> ac(patterns);
    std::string text;
    for (int i = 0; i < 100000; ++i) {
      text += "no match here just regular text ";
    }

    auto r = runBench("AhoCorasick::search [no match, 3MB text]", 20, [&]() {
      auto result = ac.search(text);
      (void)result;
    });
    printResult(r);
  }

  {
    std::vector<std::string> patterns;
    for (int i = 0; i < 1000; ++i) {
      patterns.push_back("pat" + std::to_string(i));
    }

    auto r = runBench("AhoCorasick::build [1000 patterns]", 10, [&]() {
      agentxx::util::AhoCorasick<char> ac(patterns);
      (void)ac;
    });
    printResult(r);
  }
}

} // namespace bench
} // namespace agentxx