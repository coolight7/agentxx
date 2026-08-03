#include "test_http.h"
#include "agentxx/util/http_client.h"
#include "agentxx/util/http_server.h"
#include <asio/awaitable.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <atomic>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <fmt/format.h>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace agentxx {
namespace test {

using namespace agentxx::util;

int g_http_passed = 0;
int g_http_failed = 0;

template<typename T>
void expect_has_value_impl(T&& expr, const char* file, int line) {
    auto&& tmp = std::forward<T>(expr);
    if (tmp.has_value()) {
        ++XX_TEST_PASSED;
    } else {
        ++XX_TEST_FAILED;
        if constexpr (requires { tmp.error(); }) {
            TEST_FAIL << "expected has_value at " << file << ":" << line << " | " << tmp.error()
                      << std::endl;
        } else {
            TEST_FAIL << "expected has_value at " << file << ":" << line << std::endl;
        }
    }
}

void test_http_client_unit() {
    {
        XX_TEST_EXPECT_EQ(HttpClient::calcTimeoutBySize(0).count(), 30);
        XX_TEST_EXPECT_EQ(HttpClient::calcTimeoutBySize(1).count(), 30);
        XX_TEST_EXPECT_EQ(HttpClient::calcTimeoutBySize(65536).count(), 30);
        XX_TEST_EXPECT_EQ(HttpClient::calcTimeoutBySize(65537).count(), 30);
        XX_TEST_EXPECT_EQ(HttpClient::calcTimeoutBySize(65536 * 30).count(), 30);
        XX_TEST_EXPECT_EQ(HttpClient::calcTimeoutBySize(65536 * 31).count(), 31);
        XX_TEST_EXPECT_EQ(HttpClient::calcTimeoutBySize(65536 * 100).count(), 100);
        XX_TEST_EXPECT_EQ(HttpClient::calcTimeoutBySize(65536 * 1024).count(), 1024);
    }

    {
        HttpResponse resp;
        resp.status = 200;
        XX_TEST_EXPECT_TRUE(resp.isSuccess());
        resp.status = 201;
        XX_TEST_EXPECT_TRUE(resp.isSuccess());
        resp.status = 299;
        XX_TEST_EXPECT_TRUE(resp.isSuccess());
        resp.status = 301;
        XX_TEST_EXPECT_FALSE(resp.isSuccess());
        resp.status = 404;
        XX_TEST_EXPECT_FALSE(resp.isSuccess());
        resp.status = 500;
        XX_TEST_EXPECT_FALSE(resp.isSuccess());
        resp.status = 0;
        XX_TEST_EXPECT_FALSE(resp.isSuccess());
    }

    {
        HttpResponse resp;

        XX_TEST_EXPECT_EQ(resp.contentType(), "");
        resp.headers.set("content-type", "application/json");
        XX_TEST_EXPECT_EQ(resp.contentType(), "application/json");
        resp.headers.set("content-type", "application/json; charset=utf-8");
        XX_TEST_EXPECT_EQ(resp.contentType(), "application/json");
        resp.headers.set("content-type", "text/html; charset=utf-8");
        XX_TEST_EXPECT_EQ(resp.contentType(), "text/html");
        resp.headers.set("content-type", "  Text/Plain  ");
        XX_TEST_EXPECT_EQ(resp.contentType(), "text/plain");
        resp.headers.set("content-type", "application/ld+json");
        XX_TEST_EXPECT_EQ(resp.contentType(), "application/ld+json");
        resp.headers.set("content-type", "application/xml");
        XX_TEST_EXPECT_EQ(resp.contentType(), "application/xml");
        resp.headers.set("content-type", "application/octet-stream");
        XX_TEST_EXPECT_EQ(resp.contentType(), "application/octet-stream");
    }

    {
        XX_TEST_EXPECT_TRUE(HttpResponse::isJsonContentType("application/json"));
        XX_TEST_EXPECT_TRUE(HttpResponse::isJsonContentType("application/ld+json"));
        XX_TEST_EXPECT_TRUE(HttpResponse::isJsonContentType("application/vnd.api+json"));
        XX_TEST_EXPECT_FALSE(HttpResponse::isJsonContentType("text/plain"));
        XX_TEST_EXPECT_FALSE(HttpResponse::isJsonContentType("text/html"));
        XX_TEST_EXPECT_FALSE(HttpResponse::isJsonContentType("application/xml"));
        XX_TEST_EXPECT_FALSE(HttpResponse::isJsonContentType(""));
        XX_TEST_EXPECT_FALSE(HttpResponse::isJsonContentType("application/octet-stream"));
    }

    {
        XX_TEST_EXPECT_TRUE(HttpResponse::isTextContentType(""));
        XX_TEST_EXPECT_TRUE(HttpResponse::isTextContentType("text/plain"));
        XX_TEST_EXPECT_TRUE(HttpResponse::isTextContentType("text/html"));
        XX_TEST_EXPECT_TRUE(HttpResponse::isTextContentType("text/css"));
        XX_TEST_EXPECT_TRUE(HttpResponse::isTextContentType("text/javascript"));
        XX_TEST_EXPECT_TRUE(HttpResponse::isTextContentType("application/json"));
        XX_TEST_EXPECT_TRUE(HttpResponse::isTextContentType("application/ld+json"));
        XX_TEST_EXPECT_TRUE(HttpResponse::isTextContentType("application/xml"));
        XX_TEST_EXPECT_TRUE(HttpResponse::isTextContentType("application/rss+xml"));
        XX_TEST_EXPECT_TRUE(HttpResponse::isTextContentType("application/x-www-form-urlencoded"));
        XX_TEST_EXPECT_FALSE(HttpResponse::isTextContentType("application/octet-stream"));
        XX_TEST_EXPECT_FALSE(HttpResponse::isTextContentType("image/png"));
        XX_TEST_EXPECT_FALSE(HttpResponse::isTextContentType("audio/mpeg"));
    }

    {
        HttpResponse resp;

        resp.body = R"({"key": "value"})";
        resp.headers.set("content-type", "application/json");
        auto jsonResult = resp.bodyJson();
        XX_TEST_EXPECT_HAS_VALUE(jsonResult);
        if (jsonResult.has_value()) {
            XX_TEST_EXPECT_EQ(jsonResult.value()["key"].get<std::string>(), "value");
        }

        resp.body = R"({"key": "value"})";
        resp.headers.set("content-type", "application/ld+json");
        auto jsonResult2 = resp.bodyJson();
        XX_TEST_EXPECT_HAS_VALUE(jsonResult2);
        if (jsonResult2.has_value()) {
            XX_TEST_EXPECT_EQ(jsonResult2.value()["key"].get<std::string>(), "value");
        }

        resp.body = R"({"key": "value"})";
        resp.headers.set("content-type", "text/html");
        auto jsonResult3 = resp.bodyJson();
        XX_TEST_EXPECT_NULLOPT(jsonResult3);

        resp.body = "";
        resp.headers.set("content-type", "application/json");
        auto jsonResult4 = resp.bodyJson();
        XX_TEST_EXPECT_NULLOPT(jsonResult4);

        resp.body = "not valid json";
        resp.headers.set("content-type", "application/json");
        auto jsonResult5 = resp.bodyJson();
        XX_TEST_EXPECT_NULLOPT(jsonResult5);
    }

    {
        HttpResponse resp;

        resp.body = "hello world";
        resp.headers.set("content-type", "text/plain");
        auto textResult = resp.bodyText();
        XX_TEST_EXPECT_HAS_VALUE(textResult);
        if (textResult.has_value()) {
            XX_TEST_EXPECT_EQ(textResult.value(), "hello world");
        }

        resp.body = "<html></html>";
        resp.headers.set("content-type", "text/html; charset=utf-8");
        auto textResult2 = resp.bodyText();
        XX_TEST_EXPECT_HAS_VALUE(textResult2);
        if (textResult2.has_value()) {
            XX_TEST_EXPECT_EQ(textResult2.value(), "<html></html>");
        }

        resp.body = "text content";
        resp.headers.set("content-type", "");
        auto textResult3 = resp.bodyText();
        XX_TEST_EXPECT_HAS_VALUE(textResult3);

        resp.body = "binary data";
        resp.headers.set("content-type", "application/octet-stream");
        auto textResult4 = resp.bodyText();
        XX_TEST_EXPECT_NULLOPT(textResult4);

        resp.body = "image data";
        resp.headers.set("content-type", "image/png");
        auto textResult5 = resp.bodyText();
        XX_TEST_EXPECT_NULLOPT(textResult5);
    }

    {
        HttpResponse resp;
        resp.headers.set("X-Custom", "value123");
        resp.headers.set("Content-Type", "application/json");

        XX_TEST_EXPECT_EQ(resp.findHeader("X-Custom"), "value123");
        XX_TEST_EXPECT_EQ(resp.findHeader("Content-Type"), "application/json");
        XX_TEST_EXPECT_EQ(resp.findHeader("x-custom"), "value123");
        XX_TEST_EXPECT_EQ(resp.findHeader("Non-Existent"), "");
    }

    {
        XX_TEST_EXPECT_FALSE(HttpClient::isValidUrl(""));
        XX_TEST_EXPECT_FALSE(HttpClient::isValidUrl("ftp://example.com"));
        XX_TEST_EXPECT_FALSE(HttpClient::isValidUrl("ws://example.com"));
        XX_TEST_EXPECT_TRUE(HttpClient::isValidUrl("http://example.com"));
        XX_TEST_EXPECT_TRUE(HttpClient::isValidUrl("https://example.com"));
        XX_TEST_EXPECT_TRUE(HttpClient::isValidUrl("http://example.com/path"));
        XX_TEST_EXPECT_TRUE(HttpClient::isValidUrl("https://example.com:8080/path"));
        XX_TEST_EXPECT_TRUE(HttpClient::isValidUrl("example.com"));
        XX_TEST_EXPECT_TRUE(HttpClient::isValidUrl("example.com/path"));
        XX_TEST_EXPECT_FALSE(HttpClient::isValidUrl("http://"));
        XX_TEST_EXPECT_FALSE(HttpClient::isValidUrl("https://"));
    }

    {
        auto [base, path] = HttpClient::splitUrl("http://example.com/path");
        XX_TEST_EXPECT_EQ(base, "http://example.com");
        XX_TEST_EXPECT_EQ(path, "/path");

        auto [base2, path2] = HttpClient::splitUrl("https://example.com:8080/a/b");
        XX_TEST_EXPECT_EQ(base2, "https://example.com:8080");
        XX_TEST_EXPECT_EQ(path2, "/a/b");

        auto [base3, path3] = HttpClient::splitUrl("http://example.com");
        XX_TEST_EXPECT_EQ(base3, "http://example.com");
        XX_TEST_EXPECT_EQ(path3, "/");

        auto [base4, path4] = HttpClient::splitUrl("example.com/path");
        XX_TEST_EXPECT_EQ(base4, "example.com/path");
        XX_TEST_EXPECT_EQ(path4, "/");

        auto [base5, path5] = HttpClient::splitUrl("http://example.com/");
        XX_TEST_EXPECT_EQ(base5, "http://example.com");
        XX_TEST_EXPECT_EQ(path5, "/");
    }

    {
        auto p1 = HttpClient::parseUrl("https://example.com/a/b");
        XX_TEST_EXPECT_HAS_VALUE(p1);
        XX_TEST_EXPECT_EQ(p1->scheme, "https");
        XX_TEST_EXPECT_EQ(p1->host, "example.com");
        XX_TEST_EXPECT_EQ(p1->port, 443);
        XX_TEST_EXPECT_EQ(p1->path, "/a/b");

        auto p2 = HttpClient::parseUrl("http://example.com:8080/x");
        XX_TEST_EXPECT_HAS_VALUE(p2);
        XX_TEST_EXPECT_EQ(p2->scheme, "http");
        XX_TEST_EXPECT_EQ(p2->host, "example.com");
        XX_TEST_EXPECT_EQ(p2->port, 8080);
        XX_TEST_EXPECT_EQ(p2->path, "/x");

        auto p3 = HttpClient::parseUrl("http://example.com");
        XX_TEST_EXPECT_HAS_VALUE(p3);
        XX_TEST_EXPECT_EQ(p3->port, 80);
        XX_TEST_EXPECT_EQ(p3->path, "/");

        // IPv6 字面量: host 应去掉方括号以便 DNS 解析
        auto p4 = HttpClient::parseUrl("http://[::1]:8080/v6");
        XX_TEST_EXPECT_HAS_VALUE(p4);
        XX_TEST_EXPECT_EQ(p4->host, "::1");
        XX_TEST_EXPECT_EQ(p4->port, 8080);
        XX_TEST_EXPECT_EQ(p4->path, "/v6");

        auto p5 = HttpClient::parseUrl("https://[2001:db8::1]/");
        XX_TEST_EXPECT_HAS_VALUE(p5);
        XX_TEST_EXPECT_EQ(p5->host, "2001:db8::1");
        XX_TEST_EXPECT_EQ(p5->port, 443);
        XX_TEST_EXPECT_EQ(p5->path, "/");

        XX_TEST_EXPECT_FALSE(HttpClient::parseUrl("ftp://example.com").has_value());
        XX_TEST_EXPECT_FALSE(HttpClient::parseUrl("example.com/path").has_value());
        XX_TEST_EXPECT_FALSE(HttpClient::parseUrl("http://").has_value());
        XX_TEST_EXPECT_FALSE(HttpClient::parseUrl("http://[::1").has_value());
        XX_TEST_EXPECT_FALSE(HttpClient::parseUrl("http://host:99999/").has_value());
        XX_TEST_EXPECT_FALSE(HttpClient::parseUrl("http://:8080/").has_value());
    }

    {
        XX_TEST_EXPECT_EQ(HttpClient::urlEncode("hello"), "hello");
        XX_TEST_EXPECT_EQ(HttpClient::urlEncode("hello world"), "hello+world");
        XX_TEST_EXPECT_EQ(HttpClient::urlEncode("abc123"), "abc123");
        XX_TEST_EXPECT_EQ(HttpClient::urlEncode("a&b=c"), "a%26b%3dc");
        XX_TEST_EXPECT_EQ(HttpClient::urlEncode(""), "");
        XX_TEST_EXPECT_EQ(HttpClient::urlEncode("中文"), "%e4%b8%ad%e6%96%87");
        // Additional edge cases
        XX_TEST_EXPECT_EQ(HttpClient::urlEncode("!@#$%^&*()"), "%21%40%23%24%25%5e%26%2a%28%29");
        XX_TEST_EXPECT_EQ(HttpClient::urlEncode("-_.~"), "-_.~");
        XX_TEST_EXPECT_EQ(HttpClient::urlEncode("/path/to/file"), "%2fpath%2fto%2ffile");
        XX_TEST_EXPECT_EQ(HttpClient::urlEncode("\n\t"), "%0a%09");
    }

    {
        HttpResponse resp;
        resp.status = 200;
        XX_TEST_EXPECT_TRUE(HttpClient::respIsSucc(resp));
        resp.status = 404;
        XX_TEST_EXPECT_FALSE(HttpClient::respIsSucc(resp));
        resp.status = 500;
        XX_TEST_EXPECT_FALSE(HttpClient::respIsSucc(resp));
    }

    {
        bool original = HttpClient::getSslVerify();
        XX_TEST_EXPECT_FALSE(original); // default should be true (verify enabled)
        HttpClient::setSslVerify(false);
        XX_TEST_EXPECT_FALSE(HttpClient::getSslVerify());
        HttpClient::setSslVerify(true);
        XX_TEST_EXPECT_TRUE(HttpClient::getSslVerify());
        // Restore original
        HttpClient::setSslVerify(original);
    }

    {
        // Protocol-relative URL: //host/path -> scheme://host/path
        XX_TEST_EXPECT_EQ(
            HttpClient::resolveRedirectUrl("https://example.com/a", "//other.com/b"),
            "https://other.com/b"
        );
        XX_TEST_EXPECT_EQ(
            HttpClient::resolveRedirectUrl("http://example.com/a", "//other.com:8080/c"),
            "http://other.com:8080/c"
        );
    }
}

asio::awaitable<void> test_http_client() {
    co_return;
}

void test_http_server_unit() {
    {
        XX_TEST_EXPECT_EQ(httpMethodIndex(boost::beast::http::verb::get), 0);
        XX_TEST_EXPECT_EQ(httpMethodIndex(boost::beast::http::verb::head), 1);
        XX_TEST_EXPECT_EQ(httpMethodIndex(boost::beast::http::verb::post), 2);
        XX_TEST_EXPECT_EQ(httpMethodIndex(boost::beast::http::verb::put), 3);
        XX_TEST_EXPECT_EQ(httpMethodIndex(boost::beast::http::verb::delete_), 4);
        XX_TEST_EXPECT_EQ(httpMethodIndex(boost::beast::http::verb::connect), 5);
        XX_TEST_EXPECT_EQ(httpMethodIndex(boost::beast::http::verb::options), 6);
        XX_TEST_EXPECT_EQ(httpMethodIndex(boost::beast::http::verb::trace), 7);
        XX_TEST_EXPECT_EQ(httpMethodIndex(boost::beast::http::verb::patch), 8);
        XX_TEST_EXPECT_EQ(httpMethodIndex(static_cast<boost::beast::http::verb>(999)), -1);
    }

    {
        XX_TEST_EXPECT_EQ(requestPath("/"), "/");
        XX_TEST_EXPECT_EQ(requestPath("/path"), "/path");
        XX_TEST_EXPECT_EQ(requestPath("/path?q=1"), "/path");
        XX_TEST_EXPECT_EQ(requestPath("/a/b/c?x=y&z=w"), "/a/b/c");
        XX_TEST_EXPECT_EQ(requestPath(""), "");
        XX_TEST_EXPECT_EQ(requestPath("?alone"), "");
        // absolute-form (代理风格请求目标)
        XX_TEST_EXPECT_EQ(requestPath("http://example.com/path"), "/path");
        XX_TEST_EXPECT_EQ(requestPath("http://example.com/path?q=1"), "/path");
        XX_TEST_EXPECT_EQ(requestPath("https://example.com:8080/a/b"), "/a/b");
        XX_TEST_EXPECT_EQ(requestPath("http://example.com"), "/");
        XX_TEST_EXPECT_EQ(requestPath("http://example.com?root-query"), "/");
        // 容忍非法携带的 fragment
        XX_TEST_EXPECT_EQ(requestPath("/path#frag"), "/path");
        XX_TEST_EXPECT_EQ(requestPath("/path?q=1#frag"), "/path");
    }

    {
        XX_TEST_EXPECT_EQ(httpMethodName(0), "GET");
        XX_TEST_EXPECT_EQ(httpMethodName(1), "HEAD");
        XX_TEST_EXPECT_EQ(httpMethodName(2), "POST");
        XX_TEST_EXPECT_EQ(httpMethodName(3), "PUT");
        XX_TEST_EXPECT_EQ(httpMethodName(4), "DELETE");
        XX_TEST_EXPECT_EQ(httpMethodName(5), "CONNECT");
        XX_TEST_EXPECT_EQ(httpMethodName(6), "OPTIONS");
        XX_TEST_EXPECT_EQ(httpMethodName(7), "TRACE");
        XX_TEST_EXPECT_EQ(httpMethodName(8), "PATCH");
        XX_TEST_EXPECT_EQ(httpMethodName(-1), "UNKNOWN");
        XX_TEST_EXPECT_EQ(httpMethodName(9), "UNKNOWN");
    }

    {
        XX_TEST_EXPECT_TRUE(HttpClient::isRedirectStatus(301));
        XX_TEST_EXPECT_TRUE(HttpClient::isRedirectStatus(302));
        XX_TEST_EXPECT_TRUE(HttpClient::isRedirectStatus(303));
        XX_TEST_EXPECT_TRUE(HttpClient::isRedirectStatus(307));
        XX_TEST_EXPECT_TRUE(HttpClient::isRedirectStatus(308));
        XX_TEST_EXPECT_FALSE(HttpClient::isRedirectStatus(200));
        XX_TEST_EXPECT_FALSE(HttpClient::isRedirectStatus(404));
        XX_TEST_EXPECT_FALSE(HttpClient::isRedirectStatus(500));
        XX_TEST_EXPECT_FALSE(HttpClient::isRedirectStatus(0));
    }

    {
        XX_TEST_EXPECT_TRUE(HttpClient::redirectChangesToGet(301));
        XX_TEST_EXPECT_TRUE(HttpClient::redirectChangesToGet(302));
        XX_TEST_EXPECT_TRUE(HttpClient::redirectChangesToGet(303));
        XX_TEST_EXPECT_FALSE(HttpClient::redirectChangesToGet(307));
        XX_TEST_EXPECT_FALSE(HttpClient::redirectChangesToGet(308));
        XX_TEST_EXPECT_FALSE(HttpClient::redirectChangesToGet(200));
    }

    {
        // absolute URL
        XX_TEST_EXPECT_EQ(
            HttpClient::resolveRedirectUrl("http://example.com/a", "http://other.com/b"),
            "http://other.com/b"
        );
        // absolute path
        XX_TEST_EXPECT_EQ(
            HttpClient::resolveRedirectUrl("http://example.com/a", "/b/c"),
            "http://example.com/b/c"
        );
        // relative path
        XX_TEST_EXPECT_EQ(
            HttpClient::resolveRedirectUrl("http://example.com/a/b", "c"),
            "http://example.com/a/c"
        );
        // relative path with no parent
        XX_TEST_EXPECT_EQ(
            HttpClient::resolveRedirectUrl("http://example.com/", "c"),
            "http://example.com/c"
        );
        // root path
        XX_TEST_EXPECT_EQ(
            HttpClient::resolveRedirectUrl("http://example.com/a/b/c", "/"),
            "http://example.com/"
        );
        // fragment 应被去除 (RFC 7231: Location 中的 fragment 不参与资源定位)
        XX_TEST_EXPECT_EQ(
            HttpClient::resolveRedirectUrl("http://example.com/a", "/b#section-2"),
            "http://example.com/b"
        );
        XX_TEST_EXPECT_EQ(
            HttpClient::resolveRedirectUrl("http://example.com/a", "http://other.com/c#frag"),
            "http://other.com/c"
        );
        XX_TEST_EXPECT_EQ(
            HttpClient::resolveRedirectUrl("http://example.com/a", "#only-fragment"),
            "http://example.com/a"
        );
    }
}

asio::awaitable<void> test_http_client_beast_server() {
    using Server = HttpServer;
    using namespace boost::beast::http;

    // Build a handler helper: returns Handler for a simple string response
    auto strResp = [](const char* ct, std::string body, status st = status::ok
                   ) -> std::shared_ptr<Server::Handler> {
        return std::make_shared<Server::Handler>(
            [ct, body = std::move(body), st](
                Server::Request&,
                Server::Response& resp,
                std::string_view
            ) -> asio::awaitable<void> {
                resp.result(st);
                resp.set(field::content_type, ct);
                resp.body() = std::move(body);
                resp.prepare_payload();
                co_return;
            }
        );
    };

    Server server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});

    // GET /hello
    server.router().add("/hello", 0, strResp("text/plain", "hello world"));

    // GET /json
    server.router().add("/json", 0, strResp("application/json", R"({"key":"value"})"));

    // GET /empty
    server.router().add("/empty", 0, strResp("text/plain", ""));

    // GET /status/201
    server.router().add("/status/201", 0, strResp("text/plain", "created", status::created));

    // GET /status/500
    server.router().add(
        "/status/500",
        0,
        strResp("text/plain", "server error", status::internal_server_error)
    );

    // GET /headers – echo back X-Echo value
    server.router().add(
        "/headers",
        0,
        std::make_shared<Server::Handler>(
            [](Server::Request& req, Server::Response& resp, std::string_view
            ) -> asio::awaitable<void> {
                resp.result(status::ok);
                resp.set(field::content_type, "text/plain");
                auto val    = req[field::x_forwarded_for];
                resp.body() = val.empty() ? "(none)" : std::string(val);
                resp.prepare_payload();
                co_return;
            }
        )
    );

    // GET /search?q=xxx – use query params
    server.router().add(
        "/search",
        0,
        std::make_shared<Server::Handler>(
            [](Server::Request& req, Server::Response& resp, std::string_view
            ) -> asio::awaitable<void> {
                resp.result(status::ok);
                resp.set(field::content_type, "text/plain");
                auto target = req.target();
                resp.body() = std::string(target);
                resp.prepare_payload();
                co_return;
            }
        )
    );

    // POST /echo
    server.router().add(
        "/echo",
        2,
        std::make_shared<Server::Handler>(
            [](Server::Request& req, Server::Response& resp, std::string_view
            ) -> asio::awaitable<void> {
                resp.result(status::ok);
                resp.set(field::content_type, "text/plain");
                resp.body() = req.body();
                resp.prepare_payload();
                co_return;
            }
        )
    );

    // PUT /echo – prefix with "put:"
    server.router().add(
        "/echo",
        3,
        std::make_shared<Server::Handler>(
            [](Server::Request& req, Server::Response& resp, std::string_view
            ) -> asio::awaitable<void> {
                resp.result(status::ok);
                resp.set(field::content_type, "text/plain");
                resp.body() = "put:" + req.body();
                resp.prepare_payload();
                co_return;
            }
        )
    );

    // DELETE /data
    server.router().add("/data", 4, strResp("text/plain", "deleted"));

    // Redirect: GET /redirect-me -> 302 Location: /hello
    server.router().add(
        "/redirect-me",
        0,
        std::make_shared<Server::Handler>(
            [](Server::Request&, Server::Response& resp, std::string_view
            ) -> asio::awaitable<void> {
                resp.result(status::found);
                resp.set(field::location, "/hello");
                resp.prepare_payload();
                co_return;
            }
        )
    );

    // Redirect loop: GET /redirect-loop -> 302 Location: /redirect-loop
    server.router().add(
        "/redirect-loop",
        0,
        std::make_shared<Server::Handler>(
            [](Server::Request&, Server::Response& resp, std::string_view
            ) -> asio::awaitable<void> {
                resp.result(status::found);
                resp.set(field::location, "/redirect-loop");
                resp.prepare_payload();
                co_return;
            }
        )
    );

    // Wildcard: GET /wildcard/*
    server.router().add(
        "/wildcard/*",
        0,
        std::make_shared<Server::Handler>(
            [](Server::Request&, Server::Response& resp, std::string_view matched_path
            ) -> asio::awaitable<void> {
                resp.result(status::ok);
                resp.set(field::content_type, "text/plain");
                resp.body() = fmt::format("matched:{}", matched_path);
                resp.prepare_payload();
                co_return;
            }
        )
    );

    // PATCH /echo – prefix with "patch:"
    server.router().add(
        "/echo",
        8,
        std::make_shared<Server::Handler>(
            [](Server::Request& req, Server::Response& resp, std::string_view
            ) -> asio::awaitable<void> {
                resp.result(status::ok);
                resp.set(field::content_type, "text/plain");
                resp.body() = "patch:" + req.body();
                resp.prepare_payload();
                co_return;
            }
        )
    );

    // DELETE /echo – echo body
    server.router().add(
        "/echo",
        4,
        std::make_shared<Server::Handler>(
            [](Server::Request& req, Server::Response& resp, std::string_view
            ) -> asio::awaitable<void> {
                resp.result(status::ok);
                resp.set(field::content_type, "text/plain");
                resp.body() = "delete:" + req.body();
                resp.prepare_payload();
                co_return;
            }
        )
    );

    // HEAD /hello – should return headers only, no body
    server.router().add(
        "/hello",
        1,
        std::make_shared<Server::Handler>(
            [](Server::Request&, Server::Response& resp, std::string_view
            ) -> asio::awaitable<void> {
                resp.result(status::ok);
                resp.set(field::content_type, "text/plain");
                resp.body() = "hello world";
                resp.prepare_payload();
                co_return;
            }
        )
    );

    // GET /big-body – returns a configurable-size body for limit testing
    server.router().add(
        "/big-body",
        0,
        std::make_shared<Server::Handler>(
            [](Server::Request& req, Server::Response& resp, std::string_view
            ) -> asio::awaitable<void> {
                resp.result(status::ok);
                resp.set(field::content_type, "text/plain");
                // Default 1000 bytes, or use ?size=NNNN
                std::string target = std::string(req.target());
                size_t      size   = 1000;
                auto        pos    = target.find("size=");
                if (pos != std::string::npos) {
                    size = std::stoul(target.substr(pos + 5));
                }
                resp.body() = std::string(size, 'x');
                resp.prepare_payload();
                co_return;
            }
        )
    );

    // GET /redirect-proto-rel – not used (protocol-relative needs cross-host)

    // Start server
    std::thread serverThread([&server]() {
        server.start();
    });

    // Wait for the server to be ready
    uint16_t port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (port == 0) {
        TEST_FAIL << "Server failed to start" << std::endl;
        g_http_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    // Verify server is reachable
    for (int i = 0; i < 100; ++i) {
        try {
            asio::io_context      tmpCtx;
            asio::ip::tcp::socket sock(tmpCtx);
            sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
            sock.close();
            break;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    std::cout << "Beast Server URL: " << baseUrl << std::endl;

    // -----------------------------------------------------------------------
    // Tests
    // -----------------------------------------------------------------------

    // {
    //   HttpClient::setSslVerify(false);
    //   auto resp = co_await
    //   HttpClient::getAsync("https://blog.music.bool.run/");
    //   XX_TEST_EXPECT_HAS_VALUE(resp);
    //   if (resp.has_value()) {
    //     XX_TEST_EXPECT_EQ(resp.value().status, 200);
    //     XX_TEST_EXPECT_TRUE(resp.value().isSuccess());
    //   }
    // }

    {
        auto resp = co_await HttpClient::getAsync(baseUrl + "/hello");
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            XX_TEST_EXPECT_EQ(resp.value().body, "hello world");
            XX_TEST_EXPECT_TRUE(resp.value().isSuccess());
        }
    }

    {
        auto resp = co_await HttpClient::getAsync(baseUrl + "/json");
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            auto j = resp.value().bodyJson();
            XX_TEST_EXPECT_HAS_VALUE(j);
            if (j.has_value()) {
                XX_TEST_EXPECT_EQ(j.value()["key"].get<std::string>(), "value");
            }
        }
    }

    {
        // HEAD: 服务端只发头部不发 body, 客户端 parser.skip 不等待 body,
        // 双方都符合 RFC 7231 §4.3.2, 不会挂起
        auto resp = co_await HttpClient::headAsync(baseUrl + "/hello");
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            XX_TEST_EXPECT_EQ(resp.value().body, "");
            // Content-Length 保留 GET 响应的 body 长度
            XX_TEST_EXPECT_EQ(resp.value().findHeader("content-length"), "11");
        }
    }

    {
        // HEAD 未注册专用 handler 时回退到 GET handler (RFC 7231)
        auto resp = co_await HttpClient::headAsync(baseUrl + "/json");
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            XX_TEST_EXPECT_EQ(resp.value().body, "");
        }
    }

    {
        // 空 body 的 POST 也必须携带 Content-Length: 0, 服务端可正常处理
        auto resp = co_await HttpClient::postAsync(baseUrl + "/echo", "", "text/plain");
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            XX_TEST_EXPECT_EQ(resp.value().body, "");
        }
    }

    {
        auto resp = co_await HttpClient::getAsync(baseUrl + "/empty");
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            XX_TEST_EXPECT_EQ(resp.value().body, "");
        }
    }

    {
        auto resp = co_await HttpClient::getAsync(baseUrl + "/status/201");
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 201);
            XX_TEST_EXPECT_TRUE(resp.value().isSuccess());
        }
    }

    {
        auto resp = co_await HttpClient::getAsync(baseUrl + "/status/500");
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 500);
            XX_TEST_EXPECT_FALSE(resp.value().isSuccess());
        }
    }

    {
        auto resp = co_await HttpClient::postAsync(baseUrl + "/echo", "body data", "text/plain");
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            XX_TEST_EXPECT_EQ(resp.value().body, "body data");
        }
    }

    {
        neograph::json j = {
            {"msg", "hi"}
        };
        auto resp = co_await HttpClient::postAsync(baseUrl + "/echo", j);
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            XX_TEST_EXPECT_EQ(resp.value().body, R"({"msg":"hi"})");
        }
    }

    {
        auto resp = co_await HttpClient::putAsync(baseUrl + "/echo", "update");
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().body, "put:update");
        }
    }

    {
        // /data only has a DELETE handler; GET should return 405
        auto resp = co_await HttpClient::getAsync(baseUrl + "/data");
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 405);
            // 405 必须携带 Allow 头, 且只列出该资源实际注册的方法
            auto allow = resp.value().findHeader("allow");
            XX_TEST_EXPECT_EQ(allow, "DELETE");
        }
    }

    {
        auto resp = co_await HttpClient::getAsync(baseUrl + "/wildcard/foo/bar");
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            // matched_path should contain the wildcard segment
            XX_TEST_EXPECT_TRUE(resp.value().body.find("wildcard") != std::string::npos);
        }
    }

    {
        auto resp = co_await HttpClient::getAsync(baseUrl + "/nonexistent");
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 404);
            XX_TEST_EXPECT_FALSE(resp.value().isSuccess());
        }
    }

    {
        // POST to a GET-only route
        auto resp = co_await HttpClient::postAsync(baseUrl + "/hello", "body", "text/plain");
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 405);
            XX_TEST_EXPECT_FALSE(resp.value().isSuccess());
            // /hello 注册了 GET 与 HEAD
            XX_TEST_EXPECT_EQ(resp.value().findHeader("allow"), "GET, HEAD");
        }
    }

    {
        auto resp = co_await HttpClient::getAsync(baseUrl + "/search?q=hello");
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            // The handler echoes the full target
            XX_TEST_EXPECT_TRUE(resp.value().body.find("q=hello") != std::string::npos);
        }
    }

    {
        auto resp = co_await HttpClient::getAsync(
            "http://192.0.2.1:9999/nonexistent",
            {},
            HttpClient::RequestConfig{.connectTimeout = std::chrono::milliseconds{50}}
        );
        XX_TEST_EXPECT_FALSE(resp.has_value());
    }

    { XX_TEST_EXPECT_FALSE(server.isStopped()); }

    {
        auto resp = co_await HttpClient::getAsync(
            baseUrl + "/redirect-me",
            {},
            HttpClient::RequestConfig{
                .readChunkTimeout = std::chrono::seconds{10},
                .followRedirect   = 0
            }
        );
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 302);
            auto loc = resp.value().findHeader("location");
            XX_TEST_EXPECT_EQ(loc, "/hello");
        }
    }

    {
        auto resp = co_await HttpClient::getAsync(
            baseUrl + "/redirect-me",
            {},
            HttpClient::RequestConfig{
                .readChunkTimeout = std::chrono::seconds{10},
                .followRedirect   = 1
            }
        );
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            XX_TEST_EXPECT_EQ(resp.value().body, "hello world");
        }
    }

    {
        auto resp = co_await HttpClient::getAsync(
            baseUrl + "/redirect-loop",
            {},
            HttpClient::RequestConfig{
                .readChunkTimeout = std::chrono::seconds{10},
                .followRedirect   = 3
            }
        );
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            // Should stop at the last redirect (302) since it exceeds max
            XX_TEST_EXPECT_EQ(resp.value().status, 302);
        }
    }

    // -----------------------------------------------------------------------
    // New tests for production readiness improvements
    // -----------------------------------------------------------------------

    {
        auto resp = co_await HttpClient::deleteAsync(baseUrl + "/echo");
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            XX_TEST_EXPECT_EQ(resp.value().body, "delete:");
        }
    }

    {
        auto resp = co_await HttpClient::patchAsync(baseUrl + "/echo", "patchdata");
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            XX_TEST_EXPECT_EQ(resp.value().body, "patch:patchdata");
        }
    }

    {
        auto resp = co_await HttpClient::getAsync(baseUrl + "/hello");
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            auto date = resp.value().findHeader("date");
            XX_TEST_EXPECT_FALSE(date.empty());
            // Date should contain "GMT" per RFC 7231
            XX_TEST_EXPECT_TRUE(date.find("GMT") != std::string_view::npos);
        }
    }

    {
        auto resp = co_await HttpClient::getAsync(baseUrl + "/hello");
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            auto srv = resp.value().findHeader("server");
            XX_TEST_EXPECT_FALSE(srv.empty());
        }
    }

    {
        // Request a 100-byte body with a 50-byte limit → should fail
        auto resp = co_await HttpClient::getAsync(
            baseUrl + "/big-body?size=100",
            {},
            HttpClient::RequestConfig{
                .readChunkTimeout = std::chrono::seconds{5},
                .followRedirect   = 3,
                .maxResponseBody  = 50
            }
        );
        // Body limit exceeded should result in an error (no value)
        XX_TEST_EXPECT_FALSE(resp.has_value());
    }

    {
        // Request a 100-byte body with a 200-byte limit → should succeed
        auto resp = co_await HttpClient::getAsync(
            baseUrl + "/big-body?size=100",
            {},
            HttpClient::RequestConfig{
                .readChunkTimeout = std::chrono::seconds{5},
                .followRedirect   = 3,
                .maxResponseBody  = 200
            }
        );
        XX_TEST_EXPECT_HAS_VALUE(resp);
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            XX_TEST_EXPECT_EQ(resp.value().body.size(), (size_t)100);
        }
    }

    {
        // Wait for pending connection cleanup (server-side coroutine teardown)
        size_t conn = 1;
        for (size_t i = 0; i < 50; ++i) {
            conn = server.activeConnections();
            if (conn == 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        XX_TEST_EXPECT_EQ(conn, (size_t)0);
    }

    {
        // fetchMarkdown should return error (not UB) for 404
        auto result = co_await HttpClient::fetchMarkdown(
            baseUrl + "/nonexistent",
            HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{5}}
        );
        XX_TEST_EXPECT_FALSE(result.has_value());
    }

    {
        // Register an HTML route for markdown conversion
        // Using /hello which returns "hello world" (text/plain)
        // fetchMarkdown checks success status, not content-type
        auto result = co_await HttpClient::fetchMarkdown(
            baseUrl + "/hello",
            HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{5}}
        );
        XX_TEST_EXPECT_HAS_VALUE(result);
    }

    {
        // requestSseAsync: 非 2xx 响应 (404) 应抛异常并报出状态码
        bool threw = false;
        try {
            co_await HttpClient::requestSseAsync(
                "GET",
                baseUrl + "/nonexistent",
                "",
                "",
                {},
                HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{5}},
                [](std::string_view) {}
            );
        } catch (const std::exception& e) {
            threw = true;
            XX_TEST_EXPECT_TRUE(std::string(e.what()).find("404") != std::string::npos);
        }
        XX_TEST_EXPECT_TRUE(threw);
    }

    server.stop();
    serverThread.join();
}

/// Expect: 100-continue 兼容: 服务端必须先回 100 Continue, 客户端才发送 body
asio::awaitable<void> test_http_server_expect_100_continue() {
    using Server = HttpServer;
    Server server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});
    server.router().add(
        "/echo",
        2,
        std::make_shared<Server::Handler>(
            [](Server::Request& req, Server::Response& resp, std::string_view
            ) -> asio::awaitable<void> {
                resp.result(boost::beast::http::status::ok);
                resp.set(boost::beast::http::field::content_type, "text/plain");
                resp.body() = req.body();
                resp.prepare_payload();
                co_return;
            }
        )
    );

    std::thread serverThread([&server]() {
        server.start();
    });
    uint16_t port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        XX_TEST_FAILED++;
        server.stop();
        serverThread.join();
        co_return;
    }

    auto executor = co_await asio::this_coro::executor;
    try {
        asio::ip::tcp::socket sock(executor);
        co_await sock.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port),
            asio::use_awaitable
        );

        // 只发头部, 携带 Expect: 100-continue, body 等收到 100 后再发
        std::string head = fmt::format(
            "POST /echo HTTP/1.1\r\n"
            "Host: 127.0.0.1:{}\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 11\r\n"
            "Expect: 100-continue\r\n"
            "Connection: close\r\n"
            "\r\n",
            port
        );
        co_await asio::async_write(sock, asio::buffer(head), asio::use_awaitable);

        // 读取到 100 响应 (空行结尾) 为止
        std::string interim;
        char        buf[2048];
        while (interim.find("\r\n\r\n") == std::string::npos) {
            size_t n = co_await sock.async_read_some(asio::buffer(buf), asio::use_awaitable);
            if (n == 0) {
                break;
            }
            interim.append(buf, n);
        }
        XX_TEST_EXPECT_TRUE(interim.find("100") != std::string::npos);

        // 收到 100 后再发送 body
        co_await asio::async_write(sock, asio::buffer("hello-100ca"), asio::use_awaitable);

        // 读取最终响应 (Connection: close, 读到 EOF)
        std::string finalResp = interim;
        neograph_asio_error_code ec;
        for (;;) {
            size_t n = co_await sock.async_read_some(
                asio::buffer(buf),
                asio::redirect_error(asio::use_awaitable, ec)
            );
            if (n > 0) {
                finalResp.append(buf, n);
            }
            if (ec || n == 0) {
                break;
            }
        }
        XX_TEST_EXPECT_TRUE(finalResp.find("200") != std::string::npos);
        XX_TEST_EXPECT_TRUE(finalResp.find("hello-100ca") != std::string::npos);
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "expect 100-continue test failed: " << e.what() << std::endl;
    }

    server.stop();
    serverThread.join();
}

/// absolute-form 请求目标 (代理风格): "GET http://host/path HTTP/1.1" 应正确路由
asio::awaitable<void> test_http_server_absolute_form_target() {
    using Server = HttpServer;
    Server server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});
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
    uint16_t port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        XX_TEST_FAILED++;
        server.stop();
        serverThread.join();
        co_return;
    }

    auto executor = co_await asio::this_coro::executor;
    try {
        asio::ip::tcp::socket sock(executor);
        co_await sock.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port),
            asio::use_awaitable
        );

        std::string req = fmt::format(
            "GET http://127.0.0.1:{}/hello?q=1 HTTP/1.1\r\n"
            "Host: 127.0.0.1:{}\r\n"
            "Connection: close\r\n"
            "\r\n",
            port,
            port
        );
        co_await asio::async_write(sock, asio::buffer(req), asio::use_awaitable);

        std::string resp;
        char        buf[2048];
        neograph_asio_error_code ec;
        for (;;) {
            size_t n = co_await sock.async_read_some(
                asio::buffer(buf),
                asio::redirect_error(asio::use_awaitable, ec)
            );
            if (n > 0) {
                resp.append(buf, n);
            }
            if (ec || n == 0) {
                break;
            }
        }
        XX_TEST_EXPECT_TRUE(resp.find("200") != std::string::npos);
        XX_TEST_EXPECT_TRUE(resp.find("hello world") != std::string::npos);
    } catch (const std::exception& e) {
        XX_TEST_FAILED++;
        TEST_FAIL << "absolute-form target test failed: " << e.what() << std::endl;
    }

    server.stop();
    serverThread.join();
}

/// 原始 socket SSE 测试服务器: 用于模拟 HttpClient::requestSseAsync 的各种异常场景
/// - Complete:    正常发完事件并发送 chunked 终止块
/// - AbruptClose: 发送部分事件后直接关闭连接 (不发 chunked 终止块, 模拟对端中断)
/// - Stall:       发送部分事件后保持静默 (模拟卡死, 用于触发 readChunkTimeout)
class SseTestServer {
public:

    enum class Mode {
        Complete,
        AbruptClose,
        Stall,
    };

    std::thread              thread;
    uint16_t                 boundPort = 0;
    Mode                     mode      = Mode::Complete;
    std::vector<std::string> events;  // 原始 SSE 事件块 (含结尾 "\n\n")
    std::atomic<bool>        stopped{false};

private:

    asio::io_context                         ioCtx;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor;
    asio::ip::tcp::endpoint                  ep;

public:

    void start() {
        acceptor = std::make_unique<asio::ip::tcp::acceptor>(
            ioCtx,
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)
        );
        ep        = acceptor->local_endpoint();
        boundPort = ep.port();

        thread = std::thread([this]() {
            while (!stopped.load()) {
                neograph_asio_error_code ec;
                asio::ip::tcp::socket    sock(ioCtx);
                acceptor->accept(sock, ec);
                if (ec) {
                    break;
                }
                if (stopped.load()) {
                    break;
                }
                handleConn(sock);
            }
        });
    }

    void stop() {
        stopped.store(true);
        if (acceptor) {
            neograph_asio_error_code ec;
            asio::ip::tcp::socket    dummy(ioCtx);
            dummy.connect(ep, ec);
            acceptor->close(ec);
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

private:

    static std::string chunkFrame(const std::string& payload) {
        return fmt::format("{:x}\r\n{}\r\n", payload.size(), payload);
    }

    void handleConn(asio::ip::tcp::socket& sock) {
        namespace http = boost::beast::http;
        neograph_asio_error_code ec;

        boost::beast::flat_buffer        buf;
        http::request<http::string_body> req;
        http::read(sock, buf, req, ec);
        if (ec) {
            return;
        }

        std::string header = "HTTP/1.1 200 OK\r\n"
                             "Content-Type: text/event-stream\r\n"
                             "Transfer-Encoding: chunked\r\n"
                             "\r\n";
        asio::write(sock, asio::buffer(header), ec);
        if (ec) {
            return;
        }

        for (const auto& ev : events) {
            auto framed = chunkFrame(ev);
            asio::write(sock, asio::buffer(framed), ec);
            if (ec) {
                return;
            }
        }

        switch (mode) {
            case Mode::Complete: {
                // chunked 终止块, 正常结束响应
                asio::write(sock, asio::buffer(std::string{"0\r\n\r\n"}), ec);
                break;
            }
            case Mode::AbruptClose:
                // 不发终止块直接关闭 (FIN), 客户端应收到 eof/partial_message 错误
                break;
            case Mode::Stall:
                for (int i = 0; i < 300 && !stopped.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                break;
        }
    }
};

/// requestSseAsync: 正常完成 / 中途连接中断 / 无数据超时 三种场景
asio::awaitable<void> test_http_client_sse_interruption() {
    // 1) 正常完成: 所有事件都应通过 onChunk 送达, 不抛异常
    {
        SseTestServer srv;
        srv.mode   = SseTestServer::Mode::Complete;
        srv.events = {"data: one\n\n", "data: two\n\n"};
        srv.start();

        std::string url      = "http://127.0.0.1:" + std::to_string(srv.boundPort) + "/sse";
        std::string received;
        bool        threw = false;
        try {
            co_await HttpClient::requestSseAsync(
                "POST",
                url,
                "{}",
                "application/json",
                {},
                HttpClient::RequestConfig{
                    .connectTimeout   = std::chrono::seconds{5},
                    .readChunkTimeout = std::chrono::seconds{5},
                },
                [&](std::string_view chunk) {
                    received += chunk;
                }
            );
        } catch (const std::exception& e) {
            threw = true;
            TEST_FAIL << "sse complete case threw: " << e.what() << std::endl;
        }
        XX_TEST_EXPECT_FALSE(threw);
        XX_TEST_EXPECT_EQ(received, "data: one\n\ndata: two\n\n");

        srv.stop();
    }

    // 2) 中途连接突然中断: 已发送的部分事件应先送达 onChunk, 随后抛出传输错误
    {
        SseTestServer srv;
        srv.mode   = SseTestServer::Mode::AbruptClose;
        srv.events = {"data: partial\n\n"};
        srv.start();

        std::string url      = "http://127.0.0.1:" + std::to_string(srv.boundPort) + "/sse";
        std::string received;
        bool        threw = false;
        try {
            co_await HttpClient::requestSseAsync(
                "POST",
                url,
                "{}",
                "application/json",
                {},
                HttpClient::RequestConfig{
                    .connectTimeout   = std::chrono::seconds{5},
                    .readChunkTimeout = std::chrono::seconds{5},
                },
                [&](std::string_view chunk) {
                    received += chunk;
                }
            );
        } catch (const std::exception&) {
            threw = true;
        }
        XX_TEST_EXPECT_TRUE(threw);
        XX_TEST_EXPECT_TRUE(received.find("data: partial") != std::string::npos);

        srv.stop();
    }

    // 3) 服务端发送部分事件后卡死: readChunkTimeout 内无新数据应超时抛错,
    //    且超时前已送达的数据不丢失
    {
        SseTestServer srv;
        srv.mode   = SseTestServer::Mode::Stall;
        srv.events = {"data: first\n\n"};
        srv.start();

        std::string url      = "http://127.0.0.1:" + std::to_string(srv.boundPort) + "/sse";
        std::string received;
        bool        threw = false;
        auto        start = std::chrono::steady_clock::now();
        try {
            co_await HttpClient::requestSseAsync(
                "POST",
                url,
                "{}",
                "application/json",
                {},
                HttpClient::RequestConfig{
                    .connectTimeout   = std::chrono::seconds{5},
                    .readChunkTimeout = std::chrono::milliseconds{800},
                },
                [&](std::string_view chunk) {
                    received += chunk;
                }
            );
        } catch (const std::exception&) {
            threw = true;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start
        )
                           .count();
        XX_TEST_EXPECT_TRUE(threw);
        XX_TEST_EXPECT_TRUE(received.find("data: first") != std::string::npos);
        XX_TEST_EXPECT_TRUE(elapsed >= 700);
        XX_TEST_EXPECT_TRUE(elapsed < 5000);

        srv.stop();
    }
}

asio::awaitable<TestResult> run_http_client_tests() {
    test_http_client_unit();
    test_http_server_unit();
    co_await test_http_client();
    co_await test_http_client_beast_server();
    co_await test_http_server_expect_100_continue();
    co_await test_http_server_absolute_form_target();
    co_await test_http_client_sse_interruption();
    co_return TestResult{g_http_passed, g_http_failed};
}

} // namespace test
} // namespace agentxx
