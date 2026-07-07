#pragma once

#include "agentxx/tools/tool.h"
#include "agentxx/util/log.h"
#include "agentxx/util/regex.h"
#include "agentxx/util/string_util.h"
#include "fmt/format.h"
#include <cstdlib>
#include <filesystem>
#include <html2md/html2md.h>
#include <iostream>
#include <limits>
#include <memory>
#include <neograph/llm/rate_limited_provider.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/neograph.h>
#include <sstream>
#include <string>
#include <vector>

namespace agentxx {
namespace tools {

class StringHtml2MarkdownTool : public XXToolBase {
public:
  StringHtml2MarkdownTool(
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("string_html_to_markdown", in_agentContext, true, true) {}

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
            {"type", "object"},
            {
                "properties",
                {{
                    "content",
                    {
                        {"type", "string"},
                        {"description", prompt.getArg("content")},
                    },
                }},
            },
            {"required", neograph::json::array({"content"})},
        },
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    auto content = arguments.value("content", std::string{});
    if (content.empty()) {
      co_return R"({"error":"Arg `content` is empty"})";
    }

    auto options = html2md::Options{
        .splitLines = false,
    };
    auto convert = html2md::Converter{content, &options};
    co_return convert.convert();
  }
};

class StringRegexpTool : public XXToolBase {
public:
  StringRegexpTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("string_regexp", in_agentContext, true) {}

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
            {"type", "object"},
            {
                "properties",
                {
                    {
                        "content",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("content")},
                        },
                    },
                    {
                        "exps",
                        {
                            {"type", "array"},
                            {"items", {{"type", "string"}}},
                            {"description", prompt.getArg("exps")},
                        },
                    },
                    {
                        "opt",
                        {
                            {"type", "string"},
                            {"enum", neograph::json::array(
                                         {"search", "replace", "remove"})},
                            {"description", prompt.getArg("opt")},
                        },
                    },
                    {
                        "replace_str",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("replace_str")},
                        },
                    },
                },
            },
            {
                "required",
                neograph::json::array({"content", "exps", "opt"}),
            },
        },
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    auto content = arguments.value("content", std::string{});
    if (content.empty()) {
      co_return R"({"error":"Arg `content` is empty"})";
    }
    auto match_exps = arguments.value("exps", std::vector<std::string>{});
    if (match_exps.empty()) {
      co_return R"({"error":"Arg `exps` is empty"})";
    }
    auto match_opt = arguments.value("opt", std::string{});
    if (match_opt.empty()) {
      co_return R"({"error":"Arg `opt` is empty"})";
    }

    auto regex = agentxx::util::XXRegex::createRegex(match_exps);
    if (match_opt == std::string_view{"search"}) {
      auto results = std::vector<agentxx::util::XXRegexMatchResult>{};
      if (regex->match(content, results)) {
        auto relist = neograph::json::array();
        for (size_t i = 0; i < results.size(); ++i) {
          const auto &item = results[i];
          relist.push_back(content.substr(item.start, item.end - item.start));
        }
        co_return neograph::json{
            {"tip", fmt::format("Match found {} items.", results.size())},
            {"result", relist},
        }
            .dump();
      }
    } else if (match_opt == std::string_view{"replace"}) {
      auto results = std::vector<agentxx::util::XXRegexMatchResult>{};
      auto restr = regex->replace(
          content, arguments.value("replace_str", std::string{}), results);
      if (false == results.empty()) {
        co_return restr;
      }
    } else if (match_opt == std::string_view{"remove"}) {
      auto results = std::vector<agentxx::util::XXRegexMatchResult>{};
      auto restr = regex->remove(content, results);
      if (false == results.empty()) {
        co_return restr;
      }
    } else {
      co_return R"({"error":"Arg `opt` is invalid"})";
    }
    co_return R"({"error":"No match found"})";
  }
};
} // namespace tools
} // namespace agentxx