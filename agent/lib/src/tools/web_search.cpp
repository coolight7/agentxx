#include "agentxx/tools/web_search.h"

#include "agentxx/protocol/openai_provider.h"
#include "agentxx/util/http_client.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "fmt/format.h"
#include <chrono>
#include <optional>
#include <string>

namespace agentxx {
namespace tools {

WebSearchTool::WebSearchTool(
    std::string_view                            in_searchApiUrl,
    bool                                        in_convertHtml2markdown,
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("agentxx_web_search", in_agentContext, true, true),
    searchApiUrl(in_searchApiUrl),
    convertHtml2markdown(in_convertHtml2markdown) {}

neograph::ChatTool WebSearchTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

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
            }, {"required", neograph::json::array({"query"})},
                       },
    };
}

asio::awaitable<std::string> WebSearchTool::execute_async(const neograph::json& arguments) {
    std::string query = arguments.value("query", std::string{});
    if (query.empty()) {
        co_return R"({"error":"Arg `query` is empty"})";
    }
    auto search_url
        = fmt::format(fmt::runtime(searchApiUrl), agentxx::util::HttpClient::urlEncode(query));

    std::optional<std::string> out_resp_err;
    if (convertHtml2markdown) {
        auto resp = co_await agentxx::util::HttpClient::getAsync(
            search_url,
            {},
            agentxx::util::HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{15}}
        );
        out_resp_err = resp.error_or("unknown");
        if (resp.has_value()) {
            auto& respVal = resp.value();
            if (agentxx::util::HttpClient::respIsSucc(respVal)) {
                auto& data = respVal.body;
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
        auto resp    = co_await agentxx::util::HttpClient::fetchMarkdown(search_url);
        out_resp_err = resp.error_or("unknown");
        if (resp.has_value()) {
            auto& data = resp.value();
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

WebFetchUrlTool::WebFetchUrlTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext) :
    XXToolBase("agentxx_web_fetch_url", in_agentContext, true, true) {}

neograph::ChatTool WebFetchUrlTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

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
            }, {"required", neograph::json::array({"url"})},
                       },
    };
}

asio::awaitable<std::string> WebFetchUrlTool::execute_async(const neograph::json& arguments) {
    auto url = arguments.value("url", std::string{});
    if (url.empty()) {
        co_return R"({"error":"Arg `url` is empty"})";
    }
    int timeout = int(arguments.value<double>("timeout", 30.0));

    auto resp = co_await agentxx::util::HttpClient::getAsync(
        url,
        {},
        agentxx::util::HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds(timeout)}
    );
    if (resp.has_value()) {
        if (false == agentxx::util::HttpClient::respIsSucc(resp.value())) {
            co_return fmt::format(
                R"({{"error":"web_fetch_url failed, status {}, error: {}"}})",
                resp.value().status,
                resp.error_or("[unknown]")
            );
        }

        auto& data = resp.value().body;
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

WebFetchUrlMarkdownTool::WebFetchUrlMarkdownTool(
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("agentxx_web_fetch_url_markdown", in_agentContext, true, true) {}

neograph::ChatTool WebFetchUrlMarkdownTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

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
            }, {"required", neograph::json::array({"url"})},
                       },
    };
}

asio::awaitable<std::string> WebFetchUrlMarkdownTool::execute_async(const neograph::json& arguments
) {
    std::string url = arguments.value("url", std::string{});
    if (url.empty()) {
        co_return R"({"error":"Arg `url` is empty"})";
    }

    auto resp = co_await agentxx::util::HttpClient::fetchMarkdown(url);
    if (resp.has_value()) {
        auto& data = resp.value();
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

ModelWebSearchTool::ModelWebSearchTool(
    const agentxx::agent::ModelConfig&          modelCfg,
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("agentxx_web_search", in_agentContext, true, true) {
    provider = agentxx::server::OpenAIProvider::create(modelCfg);
}

neograph::ChatTool ModelWebSearchTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

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
            }, {"required", neograph::json::array({"query"})},
                       },
    };
}

asio::awaitable<std::string> ModelWebSearchTool::execute_async(const neograph::json& arguments) {
    std::string query = arguments.value("query", std::string{});
    if (query.empty()) {
        co_return R"({"error":"Arg `query` is empty"})";
    }

    // 构造消息，请求模型进行网络搜索
    neograph::CompletionParams params;
    params.model       = ""; // 使用 provider 默认模型
    params.temperature = 0.0f;
    params.messages    = {
        neograph::ChatMessage{
                              .role    = "system",
                              .content = "You are a web search assistant. Search the internet "
                          "for the user's query and provide comprehensive, "
                          "accurate results with sources. Respond in the same "
                          "language as the query.", },
        neograph::ChatMessage{
                              .role    = "user",
                              .content = query,
                              },
    };

    auto  completion = co_await provider->invoke(params, nullptr);
    auto& content    = completion.message.content;
    if (content.empty()) {
        co_return R"({"error": "Model web search returned empty result."})";
    }
    const size_t maxLength = 8000;
    if (content.size() > maxLength) {
        content.resize(maxLength);
        content += "\n\n[Too long, truncated]";
    }
    co_return content;
}

} // namespace tools
} // namespace agentxx
