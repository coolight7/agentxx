#pragma once

#include "agentxx/util/regex.h"
#include "bench_util.h"
#include <memory>
#include <string>
#include <vector>

namespace agentxx {
namespace bench {

inline void benchRegex() {
  std::cout << "\n=== XXRegex Benchmarks ===" << std::endl;

  {
    auto r =
        runBench("XXRegex::createRegex [single simple pattern]", 1000, [&]() {
          auto re = agentxx::util::XXRegex::createRegex("hello");
          (void)re;
        });
    printResult(r);
  }

  {
    auto r =
        runBench("XXRegex::createRegex [single complex pattern]", 1000, [&]() {
          auto re = agentxx::util::XXRegex::createRegex("\\d+\\.\\d+");
          (void)re;
        });
    printResult(r);
  }

  {
    std::vector<std::string> patterns = {"hello", "world", "foo",    "bar",
                                         "baz",   "\\d+",  "[a-z]+", "test",
                                         "abc",   "xyz"};
    auto r = runBench("XXRegex::createRegex [10 patterns]", 500, [&]() {
      auto re = agentxx::util::XXRegex::createRegex(patterns);
      (void)re;
    });
    printResult(r);
  }

  {
    auto re = agentxx::util::XXRegex::createRegex("hello");
    std::string text;
    for (int i = 0; i < 10000; ++i) {
      text += "hello world this is a test ";
    }

    auto r =
        runBench("XXRegex::match [single pattern, 250KB text]", 100, [&]() {
          std::vector<agentxx::util::XXRegexMatchResult> results;
          re->match(text, results);
          (void)results;
        });
    printResult(r);
  }

  {
    auto re = agentxx::util::XXRegex::createRegex("\\d+");
    std::string text;
    for (int i = 0; i < 10000; ++i) {
      text += "abc123def456ghi789jkl";
    }

    auto r = runBench("XXRegex::match [\\d+ pattern, 200KB text]", 100, [&]() {
      std::vector<agentxx::util::XXRegexMatchResult> results;
      re->match(text, results);
      (void)results;
    });
    printResult(r);
  }

  {
    std::vector<std::string> patterns = {"hello", "world", "\\d+", "[a-z]+"};
    auto re = agentxx::util::XXRegex::createRegex(patterns);
    std::string text;
    for (int i = 0; i < 10000; ++i) {
      text += "hello world 123 abc ";
    }

    auto r = runBench("XXRegex::match [4 patterns, 200KB text]", 100, [&]() {
      std::vector<agentxx::util::XXRegexMatchResult> results;
      re->match(text, results);
      (void)results;
    });
    printResult(r);
  }

  {
    auto re = agentxx::util::XXRegex::createRegex("hello");
    std::string text;
    for (int i = 0; i < 10000; ++i) {
      text += "hello world this is a test ";
    }

    auto r =
        runBench("XXRegex::remove [single pattern, 250KB text]", 100, [&]() {
          std::vector<agentxx::util::XXRegexMatchResult> results;
          auto result = re->remove(text, results);
          (void)result;
        });
    printResult(r);
  }

  {
    auto re = agentxx::util::XXRegex::createRegex("\\d+");
    std::string text;
    for (int i = 0; i < 10000; ++i) {
      text += "abc123def456ghi789jkl";
    }

    auto r = runBench("XXRegex::remove [\\d+ pattern, 200KB text]", 100, [&]() {
      std::vector<agentxx::util::XXRegexMatchResult> results;
      auto result = re->remove(text, results);
      (void)result;
    });
    printResult(r);
  }

  {
    auto re = agentxx::util::XXRegex::createRegex("hello");
    std::string text;
    for (int i = 0; i < 10000; ++i) {
      text += "hello world this is a test ";
    }

    auto r =
        runBench("XXRegex::replace [single pattern, 250KB text]", 100, [&]() {
          std::vector<agentxx::util::XXRegexMatchResult> results;
          auto result = re->replace(text, "HI", results);
          (void)result;
        });
    printResult(r);
  }

  {
    auto re = agentxx::util::XXRegex::createRegex("\\d+");
    std::string text;
    for (int i = 0; i < 10000; ++i) {
      text += "abc123def456ghi789jkl";
    }

    auto r =
        runBench("XXRegex::replace [\\d+ pattern, 200KB text]", 100, [&]() {
          std::vector<agentxx::util::XXRegexMatchResult> results;
          auto result = re->replace(text, "NUM", results);
          (void)result;
        });
    printResult(r);
  }

  {
    auto re = agentxx::util::XXRegex::createRegex("hello");
    std::string text;
    for (int i = 0; i < 100000; ++i) {
      text += "no match here just regular text ";
    }

    auto r = runBench("XXRegex::match [no match, 3MB text]", 20, [&]() {
      std::vector<agentxx::util::XXRegexMatchResult> results;
      re->match(text, results);
      (void)results;
    });
    printResult(r);
  }

  {
    auto re = agentxx::util::XXRegex::createRegex(
        "hello", agentxx::util::XXRegex::defHSFlags_onlyContains);
    std::string text;
    for (int i = 0; i < 10000; ++i) {
      text += "hello world this is a test ";
    }

    auto r = runBench("XXRegex::match [onlyContains, match at start, 250KB]",
                      500, [&]() {
                        std::vector<agentxx::util::XXRegexMatchResult> results;
                        re->match(text, results);
                        (void)results;
                      });
    printResult(r);
  }

  {
    auto re = agentxx::util::XXRegex::createRegex(
        "hello", agentxx::util::XXRegex::defHSFlags_onlyContains);
    std::string text;
    for (int i = 0; i < 100000; ++i) {
      text += "no match here just regular text ";
    }

    auto r =
        runBench("XXRegex::match [onlyContains, no match, 3MB]", 100, [&]() {
          std::vector<agentxx::util::XXRegexMatchResult> results;
          re->match(text, results);
          (void)results;
        });
    printResult(r);
  }

  {
    auto re = agentxx::util::XXRegex::createRegex(
        "hello", agentxx::util::XXRegex::defHSFlags_onlyContains);
    std::string text;
    for (int i = 0; i < 99999; ++i) {
      text += "no match here just regular text ";
    }
    text += "hello at the very end";

    auto r = runBench("XXRegex::match [onlyContains, match at end, 3MB]", 100,
                      [&]() {
                        std::vector<agentxx::util::XXRegexMatchResult> results;
                        re->match(text, results);
                        (void)results;
                      });
    printResult(r);
  }

  {
    auto reNormal = agentxx::util::XXRegex::createRegex("hello");
    auto reOnlyContains = agentxx::util::XXRegex::createRegex(
        "hello", agentxx::util::XXRegex::defHSFlags_onlyContains);
    std::string text;
    for (int i = 0; i < 10000; ++i) {
      text += "hello world this is a test ";
    }

    auto r1 = runBench(
        "XXRegex::match [normal vs onlyContains - normal, 250KB]", 200, [&]() {
          std::vector<agentxx::util::XXRegexMatchResult> results;
          reNormal->match(text, results);
          (void)results;
        });
    printResult(r1);

    auto r2 = runBench(
        "XXRegex::match [normal vs onlyContains - onlyContains, 250KB]", 2000,
        [&]() {
          std::vector<agentxx::util::XXRegexMatchResult> results;
          reOnlyContains->match(text, results);
          (void)results;
        });
    printResult(r2);
  }
}

} // namespace bench
} // namespace agentxx