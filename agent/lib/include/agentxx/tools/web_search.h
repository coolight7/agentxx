#pragma once

#include "agentxx/tools/tool.h"
#include "agentxx/util/http_client.h"
#include "agentxx/util/log.h"
#include "fmt/format.h"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <neograph/llm/rate_limited_provider.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/neograph.h>
#include <sstream>
#include <string>
#include <vector>

namespace agentxx {
namespace tools {

class WebSearchTool : public XXToolBase {
protected:
  const std::string searchApiUrl;
  const bool convertHtml2markdown;

public:
  WebSearchTool(std::string_view in_searchApiUrl, bool in_convertHtml2markdown,
                std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("web_search", in_agentContext, true, true),
        searchApiUrl(in_searchApiUrl),
        convertHtml2markdown(in_convertHtml2markdown) {}

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
                    "query",
                    {
                        {"type", "string"},
                        {"description", prompt.getArg("query")},
                    },
                }},
            },
            {"required", neograph::json::array({"query"})},
        },
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    std::string query = arguments.value("query", std::string{});
    if (query.empty()) {
      co_return R"({"error":"Arg `query` is empty"})";
    }
    auto search_url = fmt::format(fmt::runtime(searchApiUrl),
                                  agentxx::util::HttpClient::urlEncode(query));

    std::optional<std::string> out_resp_err;
    if (convertHtml2markdown) {
      auto resp = co_await agentxx::util::HttpClient::getAsync(
          search_url, std::chrono::seconds{15});
      out_resp_err = resp.error_or("unknown");
      if (resp.has_value()) {
        auto &respVal = resp.value();
        if (agentxx::util::HttpClient::respIsSucc(respVal)) {
          auto &data = respVal.body;
          if (data.empty()) {
            co_return R"({"error": "Empty search result."})";
          }
          const size_t maxLength = 8000;
          if (data.size() > maxLength) {
            data.resize(maxLength);
            data += "\n\n[Too long, truncated]";
          }
          co_return data;
        }
      }
    } else {
      auto resp = co_await agentxx::util::HttpClient::fetchMarkdown(search_url);
      out_resp_err = resp.error_or("unknown");
      if (resp.has_value()) {
        auto &data = resp.value();
        if (data.empty()) {
          co_return R"({"error": "Empty search result."})";
        }
        const size_t maxLength = 8000;
        if (data.size() > maxLength) {
          data.resize(maxLength);
          data += "\n\n[Too long, truncated]";
        }
        co_return data;
      }
    }
    throw std::runtime_error(out_resp_err.value_or("[unknown]"));
  }
};

class WebFetchUrlTool : public XXToolBase {
public:
  WebFetchUrlTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("web_fetch_url", in_agentContext, true, true) {}

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
                     "url",
                     {
                         {"type", "string"},
                         {"description", prompt.getArg("url")},
                     },
                 },
                 {
                     "timeout",
                     {
                         {"type", "number"},
                         {"description", prompt.getArg("timeout")},
                     },
                 }},
            },
            {"required", neograph::json::array({"url"})},
        },
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    auto url = arguments.value("url", std::string{});
    if (url.empty()) {
      co_return R"({"error":"Arg `url` is empty"})";
    }
    int timeout = int(arguments.value<double>("timeout", 60.0));

    auto resp = co_await agentxx::util::HttpClient::getAsync(
        url, std::chrono::seconds(timeout));
    if (resp.has_value()) {
      if (false == agentxx::util::HttpClient::respIsSucc(resp.value())) {
        co_return fmt::format(
            R"({{"error":"web_fetch_url failed, status {}, error: {}"}})",
            resp.value().status, resp.error_or("[unknown]"));
      }

      auto &data = resp.value().body;
      if (data.empty()) {
        co_return R"({"error": "Http GET request Success, but got empty body."})";
      }
      if (agentxx::util::autoConvertToUtf8(data)) {
        co_return data;
      }
      co_return data;
    }
    throw std::runtime_error(resp.error_or("[unknown]"));
  }
};

class WebFetchUrlMarkdownTool : public XXToolBase {
public:
  WebFetchUrlMarkdownTool(
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("web_fetch_url_markdown", in_agentContext, true, true) {}

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
                    "url",
                    {
                        {"type", "string"},
                        {"description", prompt.getArg("url")},
                    },
                }},
            },
            {"required", neograph::json::array({"url"})},
        },
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    std::string url = arguments.value("url", std::string{});
    if (url.empty()) {
      co_return R"({"error":"Arg `url` is empty"})";
    }

    auto resp = co_await agentxx::util::HttpClient::fetchMarkdown(url);
    if (resp.has_value()) {
      auto &data = resp.value();
      if (data.empty()) {
        co_return R"({"error": "Request Success, but got empty result."})";
      }
      const size_t maxLength = 8000;
      if (data.size() > maxLength) {
        data.resize(maxLength);
        data += "\n\n[Too long, truncated]";
      }
      co_return data;
    }
    throw std::runtime_error(resp.error_or("[unknown]"));
  }
};
} // namespace tools
} // namespace agentxx