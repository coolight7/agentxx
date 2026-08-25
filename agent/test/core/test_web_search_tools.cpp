#include "test_web_search_tools.h"
#include <neograph/types.h>
#include "agentxx/agent/context.h"
// 原 lib 内置工具已迁移至 agentxx_websearch 插件 (同名同行为); 测试直测
// 插件同一实现 (websearch_impl.h), 保证插件行为与测试覆盖一致
#include "websearch_impl.h"
#include "agentxx/util/http_server.h"
#include <asio/awaitable.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>
#include <boost/beast/http.hpp>
#include <iostream>
#include <string>
#include <thread>

/// web 工具通用参数集 (与插件入口 schema 一致): url/query + timeout/header
namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_ws_passed = 0;
int g_ws_failed = 0;
} // namespace
static neograph::json webToolParams(const char* queryKey, const char* timeoutDesc) {
    return neograph::json{
        {"type", "object"},
        {"properties",
         {
             {queryKey,
              {
                  {"type", "string"},
                  {"description", "Absolute HTTP/HTTPS URL to fetch."},
              }},
             {"timeout", {{"type", "number"}, {"description", timeoutDesc}}},
             {"header",
              {{"type", "object"}, {"description", "Custom HTTP request headers to send."}}},
         }},
        {"required", neograph::json::array({queryKey})},
    };
}

namespace agentxx {
namespace tools {

/// 测试适配: 原工具类的同名薄包装 (构造签名与 execute_async 行为保持不变)

/// WebSearchTool: api url 路径 (convertHtml2markdown 控制输出格式)
struct WebSearchTool {
    std::string searchApiUrl;
    bool        convertHtml2markdown = true;

    WebSearchTool(std::string_view url, bool conv, std::weak_ptr<agentxx::agent::AgentContext>)
        : searchApiUrl(url),
          convertHtml2markdown(conv) {}

    static neograph::ChatTool searchDefinition() {
        return {"agentxx_web_search",
                "Perform a web search. Returns a markdown-formatted list of results.",
                // schema 结构与 lib prompt 条目一致: required 含 query
                neograph::json{
                    {"type", "object"},
                    {"properties",
                     {
                         {"query", {{"type", "string"}, {"description", "The search query."}}},
                         {"timeout",
                          {{"type", "number"}, {"description", "Default `15` seconds."}}},
                         {"header",
                          {{"type", "object"}, {"description", "Custom HTTP request headers."}}},
                     }},
                    {"required", neograph::json::array({"query"})},
                }};
    }

    neograph::ChatTool get_definition() const { return searchDefinition(); }

    asio::awaitable<std::string> execute_async(const neograph::json& args) const {
        co_return agentxx_websearch_plugin::webSearchExecute(
            args,
            searchApiUrl,
            convertHtml2markdown
        );
    }
};

struct WebFetchUrlTool {
    explicit WebFetchUrlTool(std::weak_ptr<agentxx::agent::AgentContext>) {}

    neograph::ChatTool get_definition() const {
        return {"agentxx_web_fetch",
                "Perform an HTTP GET request and return the raw response body.",
                webToolParams("url", "Default `30` seconds. Request timeout in seconds.")};
    }

    asio::awaitable<std::string> execute_async(const neograph::json& args) const {
        co_return agentxx_websearch_plugin::webFetchExecute(args);
    }
};

/// 模型搜索路径薄包装 (构造取最小配置集; schema 与 API 版一致)
struct ModelWebSearchTool {
    explicit ModelWebSearchTool(const agentxx_websearch_plugin::ModelSearchConfig& cfg)
        : modelCfg(cfg) {}

    neograph::ChatTool get_definition() const {
        return {"agentxx_web_search",
                "Perform a web search. Returns a markdown-formatted list of results.",
                neograph::json{
                    {"type", "object"},
                    {"properties",
                     {
                         {"query",
                          {{"type", "string"}, {"description", "The search query string."}}},
                         {"timeout", {{"type", "number"}, {"description", "Default `60` seconds."}}},
                         {"header",
                          {{"type", "object"},
                           {"description", "Custom HTTP request headers to send."}}},
                     }},
                    {"required", neograph::json::array({"query"})},
                }};
    }

    asio::awaitable<std::string> execute_async(const neograph::json& args) const {
        co_return agentxx_websearch_plugin::modelWebSearchExecute(args, modelCfg);
    }

private:
    agentxx_websearch_plugin::ModelSearchConfig modelCfg;
};

struct WebFetchUrlMarkdownTool {
    explicit WebFetchUrlMarkdownTool(std::weak_ptr<agentxx::agent::AgentContext>) {}

    neograph::ChatTool get_definition() const {
        return {"agentxx_web_fetch_markdown",
                "Perform an HTTP GET request and return the page content converted to Markdown.",
                webToolParams("url", "Default `15` seconds. Request timeout in seconds.")};
    }

    asio::awaitable<std::string> execute_async(const neograph::json& args) const {
        co_return agentxx_websearch_plugin::webFetchMarkdownExecute(args);
    }
};

} // namespace tools
} // namespace agentxx

namespace agentxx {
namespace test {

asio::awaitable<void>
    test_web_search_get_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool
        = agentxx::tools::WebSearchTool{"https://example.com/search?q={}", false, agentContext};
    auto def = tool.get_definition();
    if (def.name == "agentxx_web_search") {
        g_ws_passed++;
        TEST_PASS << "WebSearchTool::get_definition() name correct" << std::endl;
    } else {
        g_ws_failed++;
        TEST_FAIL << "WebSearchTool::get_definition() name incorrect" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_web_search_empty_query(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool
        = agentxx::tools::WebSearchTool{"https://example.com/search?q={}", false, agentContext};
    auto args = neograph::json{
        {"query", ""}
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("\"error\"") != std::string::npos) {
        g_ws_passed++;
        TEST_PASS << "WebSearchTool returns error for empty query" << std::endl;
    } else {
        std::cout << "[FAIL] WebSearchTool should return error for empty query, got: " << result
                  << std::endl;
    }
    co_return;
}

asio::awaitable<void> test_web_search_definition_has_required_query(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext
) {
    auto tool
        = agentxx::tools::WebSearchTool{"https://example.com/search?q={}", false, agentContext};
    auto  def    = tool.get_definition();
    auto& params = def.parameters;
    if (params.is_object() && params.contains("required") && params["required"].is_array()) {
        auto required = params["required"];
        bool hasQuery = false;
        for (const auto& item : required) {
            if (item.is_string() && item.get<std::string>() == "query") {
                hasQuery = true;
                break;
            }
        }
        if (hasQuery) {
            std::cout << "[PASS] WebSearchTool definition has 'query' as required param"
                      << std::endl;
        } else {
            g_ws_failed++;
            TEST_FAIL << "WebSearchTool definition missing 'query' in required" << std::endl;
        }
    } else {
        g_ws_failed++;
        TEST_FAIL << "WebSearchTool definition has no required params" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_web_fetch_url_get_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::WebFetchUrlTool{agentContext};
    auto def  = tool.get_definition();
    if (def.name == "agentxx_web_fetch") {
        g_ws_passed++;
        TEST_PASS << "WebFetchUrlTool::get_definition() name correct" << std::endl;
    } else {
        g_ws_failed++;
        TEST_FAIL << "WebFetchUrlTool::get_definition() name incorrect" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_web_fetch_url_empty_url(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::WebFetchUrlTool{agentContext};
    auto args = neograph::json{
        {"url", ""}
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("\"error\"") != std::string::npos) {
        g_ws_passed++;
        TEST_PASS << "WebFetchUrlTool returns error for empty url" << std::endl;
    } else {
        std::cout << "[FAIL] WebFetchUrlTool should return error for empty url, got: " << result
                  << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_web_fetch_url_default_timeout(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto  tool   = agentxx::tools::WebFetchUrlTool{agentContext};
    auto  def    = tool.get_definition();
    auto& params = def.parameters;
    if (params.is_object() && params.contains("properties") && params["properties"].is_object()) {
        auto props = params["properties"];
        if (props.contains("timeout") && props["timeout"].is_object()) {
            g_ws_passed++;
            TEST_PASS << "WebFetchUrlTool definition has timeout parameter" << std::endl;
        } else {
            g_ws_failed++;
            TEST_FAIL << "WebFetchUrlTool definition missing timeout parameter" << std::endl;
        }
    }
    co_return;
}

asio::awaitable<void> test_web_fetch_url_markdown_get_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext
) {
    auto tool = agentxx::tools::WebFetchUrlMarkdownTool{agentContext};
    auto def  = tool.get_definition();
    if (def.name == "agentxx_web_fetch_markdown") {
        g_ws_passed++;
        TEST_PASS << "WebFetchUrlMarkdownTool::get_definition() name correct" << std::endl;
    } else {
        std::cout << "[FAIL] WebFetchUrlMarkdownTool::get_definition() name incorrect" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_web_fetch_url_markdown_empty_url(std::weak_ptr<agentxx::agent::AgentContext> agentContext
    ) {
    auto tool = agentxx::tools::WebFetchUrlMarkdownTool{agentContext};
    auto args = neograph::json{
        {"url", ""}
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("\"error\"") != std::string::npos) {
        g_ws_passed++;
        TEST_PASS << "WebFetchUrlMarkdownTool returns error for empty url" << std::endl;
    } else {
        g_ws_failed++;
        TEST_FAIL << "WebFetchUrlMarkdownTool should return error for empty "
                     "url, got: "
                  << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_web_fetch_url_markdown_description(std::weak_ptr<agentxx::agent::AgentContext> agentContext
    ) {
    auto tool = agentxx::tools::WebFetchUrlMarkdownTool{agentContext};
    auto def  = tool.get_definition();
    if (def.description.find("markdown") != std::string::npos
        || def.description.find("Markdown") != std::string::npos) {
        g_ws_passed++;
        TEST_PASS << "WebFetchUrlMarkdownTool description mentions markdown" << std::endl;
    } else {
        g_ws_failed++;
        TEST_FAIL << "WebFetchUrlMarkdownTool description should mention "
                     "markdown"
                  << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_web_search_convert_html2markdown(std::weak_ptr<agentxx::agent::AgentContext> agentContext
    ) {
    auto tool
        = agentxx::tools::WebSearchTool{"https://example.com/search?q={}", true, agentContext};
    auto def = tool.get_definition();
    if (def.name == "agentxx_web_search") {
        g_ws_passed++;
        TEST_PASS << "WebSearchTool with convertHtml2markdown=true created "
                     "successfully"
                  << std::endl;
    } else {
        g_ws_failed++;
        TEST_FAIL << "WebSearchTool with convertHtml2markdown=true failed" << std::endl;
    }
    co_return;
}

/// 判断 tool 定义中是否包含指定参数
static bool definitionHasArg(const neograph::ChatTool& def, std::string_view argName) {
    const auto& params = def.parameters;
    if (!params.is_object() || !params.contains("properties")
        || !params["properties"].is_object()) {
        return false;
    }
    const auto props = params["properties"];
    return props.contains(std::string(argName));
}

/// 所有 web tool 的定义都应统一包含 `timeout` 与 `header` 参数
asio::awaitable<void> test_web_tools_definition_timeout_header(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext
) {
    auto checkTool = [](std::string_view name, const neograph::ChatTool& def) {
        bool ok = definitionHasArg(def, "timeout") && definitionHasArg(def, "header");
        if (ok) {
            g_ws_passed++;
            TEST_PASS << name << " definition has timeout & header params" << std::endl;
        } else {
            g_ws_failed++;
            TEST_FAIL << name << " definition missing timeout/header params" << std::endl;
        }
    };

    {
        auto tool
            = agentxx::tools::WebSearchTool{"https://example.com/search?q={}", false, agentContext};
        checkTool("WebSearchTool", tool.get_definition());
    }
    {
        auto tool = agentxx::tools::WebFetchUrlTool{agentContext};
        checkTool("WebFetchUrlTool", tool.get_definition());
    }
    {
        auto tool = agentxx::tools::WebFetchUrlMarkdownTool{agentContext};
        checkTool("WebFetchUrlMarkdownTool", tool.get_definition());
    }
    {
        // 测试适配: ModelWebSearchTool 同名薄包装 (空 ModelConfig 仅验证 schema)
        agentxx_websearch_plugin::ModelSearchConfig emptyCfg{};
        auto tool = agentxx::tools::ModelWebSearchTool{emptyCfg};
        checkTool("ModelWebSearchTool", tool.get_definition());
    }
    co_return;
}

/// 本地 HTTP 服务器验证 `header` 参数实际发送到了请求中
asio::awaitable<void>
    test_web_tools_header_parameter(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    using Server = agentxx::util::HttpServer;
    Server server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});
    // 回显收到的 X-Test-Header 请求头
    server.router().add(
        "/echo-header",
        0,
        std::make_shared<Server::Handler>(
            [](Server::Request& req, Server::Response& resp, std::string_view
            ) -> asio::awaitable<void> {
                resp.result(boost::beast::http::status::ok);
                resp.set(boost::beast::http::field::content_type, "text/plain");
                boost::beast::string_view v = req.base()["X-Test-Header"];
                resp.body()                 = v.empty() ? std::string{} : std::string(v);
                resp.prepare_payload();
                co_return;
            }
        )
    );
    // 固定返回非空内容, 用于仅验证 timeout 参数被接受 (不依赖 header 回显)
    server.router().add(
        "/hello",
        0,
        std::make_shared<Server::Handler>(
            [](Server::Request&, Server::Response& resp, std::string_view
            ) -> asio::awaitable<void> {
                resp.result(boost::beast::http::status::ok);
                resp.set(boost::beast::http::field::content_type, "text/plain");
                resp.body() = "hello world";
                resp.prepare_payload();
                co_return;
            }
        )
    );
    std::thread serverThread([&server]() {
        server.start();
    });
    uint16_t    port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        g_ws_failed++;
        TEST_FAIL << "web tools header test: server failed to start" << std::endl;
        server.stop();
        serverThread.join();
        co_return;
    }
    const std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);

    // WebFetchUrlTool: header 参数 (JSON 对象格式)
    {
        auto tool = agentxx::tools::WebFetchUrlTool{agentContext};
        auto args = neograph::json{
            {"url",    baseUrl + "/echo-header"                                   },
            {"header", neograph::json{{"X-Test-Header", "fetch-url-header-value"}}},
        };
        auto result = co_await tool.execute_async(args);
        if (result.find("fetch-url-header-value") != std::string::npos) {
            g_ws_passed++;
            TEST_PASS << "WebFetchUrlTool sends custom header" << std::endl;
        } else {
            g_ws_failed++;
            TEST_FAIL << "WebFetchUrlTool header not received, got: " << result << std::endl;
        }
    }
    // WebFetchUrlMarkdownTool: header 参数
    {
        auto tool = agentxx::tools::WebFetchUrlMarkdownTool{agentContext};
        auto args = neograph::json{
            {"url",    baseUrl + "/echo-header"                                  },
            {"header", neograph::json{{"X-Test-Header", "fetch-md-header-value"}}},
        };
        auto result = co_await tool.execute_async(args);
        if (result.find("fetch-md-header-value") != std::string::npos) {
            g_ws_passed++;
            TEST_PASS << "WebFetchUrlMarkdownTool sends custom header" << std::endl;
        } else {
            g_ws_failed++;
            TEST_FAIL << "WebFetchUrlMarkdownTool header not received, got: " << result
                      << std::endl;
        }
    }
    // WebSearchTool (原始 body 路径): header 参数 + timeout 参数
    {
        auto tool
            = agentxx::tools::WebSearchTool{baseUrl + "/echo-header?q={}", false, agentContext};
        auto args = neograph::json{
            {"query",   "test"                                                      },
            {"timeout", 10                                                          },
            {"header",  neograph::json{{"X-Test-Header", "search-raw-header-value"}}},
        };
        auto result = co_await tool.execute_async(args);
        if (result.find("search-raw-header-value") != std::string::npos) {
            g_ws_passed++;
            TEST_PASS << "WebSearchTool(raw) sends custom header & accepts timeout" << std::endl;
        } else {
            g_ws_failed++;
            TEST_FAIL << "WebSearchTool(raw) header not received, got: " << result << std::endl;
        }
    }
    // WebSearchTool (markdown 路径): header 参数 + timeout 参数
    {
        auto tool
            = agentxx::tools::WebSearchTool{baseUrl + "/echo-header?q={}", true, agentContext};
        auto args = neograph::json{
            {"query",   "test"                                                     },
            {"timeout", 10                                                         },
            {"header",  neograph::json{{"X-Test-Header", "search-md-header-value"}}},
        };
        auto result = co_await tool.execute_async(args);
        if (result.find("search-md-header-value") != std::string::npos) {
            g_ws_passed++;
            TEST_PASS << "WebSearchTool(markdown) sends custom header & accepts timeout"
                      << std::endl;
        } else {
            g_ws_failed++;
            TEST_FAIL << "WebSearchTool(markdown) header not received, got: " << result
                      << std::endl;
        }
    }
    // timeout 参数被接受 (正常服务器不应报错)
    {
        auto tool = agentxx::tools::WebFetchUrlTool{agentContext};
        auto args = neograph::json{
            {"url",     baseUrl + "/hello"},
            {"timeout", 5                 },
        };
        auto result = co_await tool.execute_async(args);
        if (result.find("\"error\"") == std::string::npos) {
            g_ws_passed++;
            TEST_PASS << "WebFetchUrlTool accepts timeout param" << std::endl;
        } else {
            g_ws_failed++;
            TEST_FAIL << "WebFetchUrlTool timeout param failed, got: " << result << std::endl;
        }
    }

    server.stop();
    serverThread.join();
    co_return;
}

asio::awaitable<TestResult>
    run_web_search_tools_tests(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto run = [agentContext](auto testFn) -> asio::awaitable<void> {
        try {
            co_await testFn(agentContext);
        } catch (const std::exception& e) {
            g_ws_failed++;
            TEST_FAIL << "Exception in test: " << e.what() << std::endl;
        }
    };

    co_await run(test_web_search_get_definition);
    co_await run(test_web_search_empty_query);
    co_await run(test_web_search_definition_has_required_query);
    co_await run(test_web_search_convert_html2markdown);
    co_await run(test_web_tools_definition_timeout_header);
    co_await run(test_web_tools_header_parameter);
    co_await run(test_web_fetch_url_get_definition);
    co_await run(test_web_fetch_url_empty_url);
    co_await run(test_web_fetch_url_default_timeout);
    co_await run(test_web_fetch_url_markdown_get_definition);
    co_await run(test_web_fetch_url_markdown_empty_url);
    co_await run(test_web_fetch_url_markdown_description);
    co_return TestResult{g_ws_passed, g_ws_failed};
}

} // namespace test
} // namespace agentxx
