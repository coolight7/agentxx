#pragma once

#include "agentxx/util/http_client.h"
#include "agentxx/util/http_server.h"
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

using namespace agentxx::util;

inline static int g_http_passed = 0;
inline static int g_http_failed = 0;

#define HTEST(name) std::cout << "  [" << (name) << "]" << std::endl;

#define HEXPECT_EQ(expr, expected)                                             \
  do {                                                                         \
    auto _result = (expr);                                                     \
    auto _expected = (expected);                                               \
    if (_result == _expected) {                                                \
      g_http_passed++;                                                         \
    } else {                                                                   \
      g_http_failed++;                                                         \
      std::cerr << "    FAIL at line " << __LINE__ << ": expected "            \
                << (_expected) << ", got " << (_result) << std::endl;          \
    }                                                                          \
  } while (0)

#define HEXPECT_TRUE(expr)                                                     \
  do {                                                                         \
    if (expr) {                                                                \
      g_http_passed++;                                                         \
    } else {                                                                   \
      g_http_failed++;                                                         \
      std::cerr << "    FAIL at line " << __LINE__ << ": expected true"        \
                << std::endl;                                                  \
    }                                                                          \
  } while (0)

#define HEXPECT_FALSE(expr)                                                    \
  do {                                                                         \
    if (!(expr)) {                                                             \
      g_http_passed++;                                                         \
    } else {                                                                   \
      g_http_failed++;                                                         \
      std::cerr << "    FAIL at line " << __LINE__ << ": expected false"       \
                << std::endl;                                                  \
    }                                                                          \
  } while (0)

template <typename T>
void expect_has_value_impl(T &&expr, const char *file, int line) {
  auto &&tmp = std::forward<T>(expr); // 仅求值一次
  if (tmp.has_value()) {
    ++g_http_passed;
  } else {
    ++g_http_failed;
    // 检测是否存在 error() 成员
    if constexpr (requires { tmp.error(); }) {
      std::cerr << "    FAIL at " << file << ":" << line
                << ": expected has_value | " << tmp.error() << std::endl;
    } else {
      std::cerr << "    FAIL at " << file << ":" << line
                << ": expected has_value" << std::endl;
    }
  }
}

#define HEXPECT_HAS_VALUE(expr) expect_has_value_impl(expr, __FILE__, __LINE__)

#define HEXPECT_NULLOPT(expr)                                                  \
  do {                                                                         \
    if (!(expr).has_value()) {                                                 \
      g_http_passed++;                                                         \
    } else {                                                                   \
      g_http_failed++;                                                         \
      std::cerr << "    FAIL at line " << __LINE__ << ": expected nullopt"     \
                << std::endl;                                                  \
    }                                                                          \
  } while (0)

namespace agentxx {
namespace test {

inline void test_http_client_unit() {
  std::cout << "======= Test: HTTP Client Unit =======" << std::endl;

  HTEST("HttpResponse::isSuccess");
  {
    HttpResponse resp;
    resp.status = 200;
    HEXPECT_TRUE(resp.isSuccess());
    resp.status = 201;
    HEXPECT_TRUE(resp.isSuccess());
    resp.status = 299;
    HEXPECT_TRUE(resp.isSuccess());
    resp.status = 301;
    HEXPECT_FALSE(resp.isSuccess());
    resp.status = 404;
    HEXPECT_FALSE(resp.isSuccess());
    resp.status = 500;
    HEXPECT_FALSE(resp.isSuccess());
    resp.status = 0;
    HEXPECT_FALSE(resp.isSuccess());
  }

  HTEST("HttpResponse::contentType");
  {
    HttpResponse resp;

    HEXPECT_EQ(resp.contentType(), "");
    resp.headers.set("content-type", "application/json");
    HEXPECT_EQ(resp.contentType(), "application/json");
    resp.headers.set("content-type", "application/json; charset=utf-8");
    HEXPECT_EQ(resp.contentType(), "application/json");
    resp.headers.set("content-type", "text/html; charset=utf-8");
    HEXPECT_EQ(resp.contentType(), "text/html");
    resp.headers.set("content-type", "  Text/Plain  ");
    HEXPECT_EQ(resp.contentType(), "text/plain");
    resp.headers.set("content-type", "application/ld+json");
    HEXPECT_EQ(resp.contentType(), "application/ld+json");
    resp.headers.set("content-type", "application/xml");
    HEXPECT_EQ(resp.contentType(), "application/xml");
    resp.headers.set("content-type", "application/octet-stream");
    HEXPECT_EQ(resp.contentType(), "application/octet-stream");
  }

  HTEST("HttpResponse::isJsonContentType");
  {
    HEXPECT_TRUE(HttpResponse::isJsonContentType("application/json"));
    HEXPECT_TRUE(HttpResponse::isJsonContentType("application/ld+json"));
    HEXPECT_TRUE(HttpResponse::isJsonContentType("application/vnd.api+json"));
    HEXPECT_FALSE(HttpResponse::isJsonContentType("text/plain"));
    HEXPECT_FALSE(HttpResponse::isJsonContentType("text/html"));
    HEXPECT_FALSE(HttpResponse::isJsonContentType("application/xml"));
    HEXPECT_FALSE(HttpResponse::isJsonContentType(""));
    HEXPECT_FALSE(HttpResponse::isJsonContentType("application/octet-stream"));
  }

  HTEST("HttpResponse::isTextContentType");
  {
    HEXPECT_TRUE(HttpResponse::isTextContentType(""));
    HEXPECT_TRUE(HttpResponse::isTextContentType("text/plain"));
    HEXPECT_TRUE(HttpResponse::isTextContentType("text/html"));
    HEXPECT_TRUE(HttpResponse::isTextContentType("text/css"));
    HEXPECT_TRUE(HttpResponse::isTextContentType("text/javascript"));
    HEXPECT_TRUE(HttpResponse::isTextContentType("application/json"));
    HEXPECT_TRUE(HttpResponse::isTextContentType("application/ld+json"));
    HEXPECT_TRUE(HttpResponse::isTextContentType("application/xml"));
    HEXPECT_TRUE(HttpResponse::isTextContentType("application/rss+xml"));
    HEXPECT_TRUE(
        HttpResponse::isTextContentType("application/x-www-form-urlencoded"));
    HEXPECT_FALSE(HttpResponse::isTextContentType("application/octet-stream"));
    HEXPECT_FALSE(HttpResponse::isTextContentType("image/png"));
    HEXPECT_FALSE(HttpResponse::isTextContentType("audio/mpeg"));
  }

  HTEST("HttpResponse::bodyJson");
  {
    HttpResponse resp;

    resp.body = R"({"key": "value"})";
    resp.headers.set("content-type", "application/json");
    auto jsonResult = resp.bodyJson();
    HEXPECT_HAS_VALUE(jsonResult);
    if (jsonResult.has_value()) {
      HEXPECT_EQ(jsonResult.value()["key"].get<std::string>(), "value");
    }

    resp.body = R"({"key": "value"})";
    resp.headers.set("content-type", "application/ld+json");
    auto jsonResult2 = resp.bodyJson();
    HEXPECT_HAS_VALUE(jsonResult2);
    if (jsonResult2.has_value()) {
      HEXPECT_EQ(jsonResult2.value()["key"].get<std::string>(), "value");
    }

    resp.body = R"({"key": "value"})";
    resp.headers.set("content-type", "text/html");
    auto jsonResult3 = resp.bodyJson();
    HEXPECT_NULLOPT(jsonResult3);

    resp.body = "";
    resp.headers.set("content-type", "application/json");
    auto jsonResult4 = resp.bodyJson();
    HEXPECT_NULLOPT(jsonResult4);

    resp.body = "not valid json";
    resp.headers.set("content-type", "application/json");
    auto jsonResult5 = resp.bodyJson();
    HEXPECT_NULLOPT(jsonResult5);
  }

  HTEST("HttpResponse::bodyText");
  {
    HttpResponse resp;

    resp.body = "hello world";
    resp.headers.set("content-type", "text/plain");
    auto textResult = resp.bodyText();
    HEXPECT_HAS_VALUE(textResult);
    if (textResult.has_value()) {
      HEXPECT_EQ(textResult.value(), "hello world");
    }

    resp.body = "<html></html>";
    resp.headers.set("content-type", "text/html; charset=utf-8");
    auto textResult2 = resp.bodyText();
    HEXPECT_HAS_VALUE(textResult2);
    if (textResult2.has_value()) {
      HEXPECT_EQ(textResult2.value(), "<html></html>");
    }

    resp.body = "text content";
    resp.headers.set("content-type", "");
    auto textResult3 = resp.bodyText();
    HEXPECT_HAS_VALUE(textResult3);

    resp.body = "binary data";
    resp.headers.set("content-type", "application/octet-stream");
    auto textResult4 = resp.bodyText();
    HEXPECT_NULLOPT(textResult4);

    resp.body = "image data";
    resp.headers.set("content-type", "image/png");
    auto textResult5 = resp.bodyText();
    HEXPECT_NULLOPT(textResult5);
  }

  HTEST("HttpResponse::findHeader");
  {
    HttpResponse resp;
    resp.headers.set("X-Custom", "value123");
    resp.headers.set("Content-Type", "application/json");

    HEXPECT_EQ(resp.findHeader("X-Custom"), "value123");
    HEXPECT_EQ(resp.findHeader("Content-Type"), "application/json");
    HEXPECT_EQ(resp.findHeader("x-custom"), "value123");
    HEXPECT_EQ(resp.findHeader("Non-Existent"), "");
  }

  HTEST("HttpClient::isValidUrl");
  {
    HEXPECT_FALSE(HttpClient::isValidUrl(""));
    HEXPECT_FALSE(HttpClient::isValidUrl("ftp://example.com"));
    HEXPECT_FALSE(HttpClient::isValidUrl("ws://example.com"));
    HEXPECT_TRUE(HttpClient::isValidUrl("http://example.com"));
    HEXPECT_TRUE(HttpClient::isValidUrl("https://example.com"));
    HEXPECT_TRUE(HttpClient::isValidUrl("http://example.com/path"));
    HEXPECT_TRUE(HttpClient::isValidUrl("https://example.com:8080/path"));
    HEXPECT_TRUE(HttpClient::isValidUrl("example.com"));
    HEXPECT_TRUE(HttpClient::isValidUrl("example.com/path"));
    HEXPECT_FALSE(HttpClient::isValidUrl("http://"));
    HEXPECT_FALSE(HttpClient::isValidUrl("https://"));
  }

  HTEST("HttpClient::splitUrl");
  {
    auto [base, path] = HttpClient::splitUrl("http://example.com/path");
    HEXPECT_EQ(base, "http://example.com");
    HEXPECT_EQ(path, "/path");

    auto [base2, path2] = HttpClient::splitUrl("https://example.com:8080/a/b");
    HEXPECT_EQ(base2, "https://example.com:8080");
    HEXPECT_EQ(path2, "/a/b");

    auto [base3, path3] = HttpClient::splitUrl("http://example.com");
    HEXPECT_EQ(base3, "http://example.com");
    HEXPECT_EQ(path3, "/");

    auto [base4, path4] = HttpClient::splitUrl("example.com/path");
    HEXPECT_EQ(base4, "example.com/path");
    HEXPECT_EQ(path4, "/");

    auto [base5, path5] = HttpClient::splitUrl("http://example.com/");
    HEXPECT_EQ(base5, "http://example.com");
    HEXPECT_EQ(path5, "/");
  }

  HTEST("HttpClient::urlEncode");
  {
    HEXPECT_EQ(HttpClient::urlEncode("hello"), "hello");
    HEXPECT_EQ(HttpClient::urlEncode("hello world"), "hello+world");
    HEXPECT_EQ(HttpClient::urlEncode("abc123"), "abc123");
    HEXPECT_EQ(HttpClient::urlEncode("a&b=c"), "a%26b%3dc");
    HEXPECT_EQ(HttpClient::urlEncode(""), "");
    HEXPECT_EQ(HttpClient::urlEncode("中文"), "%e4%b8%ad%e6%96%87");
    // Additional edge cases
    HEXPECT_EQ(HttpClient::urlEncode("!@#$%^&*()"),
               "%21%40%23%24%25%5e%26%2a%28%29");
    HEXPECT_EQ(HttpClient::urlEncode("-_.~"), "-_.~");
    HEXPECT_EQ(HttpClient::urlEncode("/path/to/file"), "%2fpath%2fto%2ffile");
    HEXPECT_EQ(HttpClient::urlEncode("\n\t"), "%0a%09");
  }

  HTEST("HttpClient::respIsSucc");
  {
    HttpResponse resp;
    resp.status = 200;
    HEXPECT_TRUE(HttpClient::respIsSucc(resp));
    resp.status = 404;
    HEXPECT_FALSE(HttpClient::respIsSucc(resp));
    resp.status = 500;
    HEXPECT_FALSE(HttpClient::respIsSucc(resp));
  }

  HTEST("HttpClient::setSslVerify / getSslVerify");
  {
    bool original = HttpClient::getSslVerify();
    HEXPECT_TRUE(original); // default should be true (verify enabled)
    HttpClient::setSslVerify(false);
    HEXPECT_FALSE(HttpClient::getSslVerify());
    HttpClient::setSslVerify(true);
    HEXPECT_TRUE(HttpClient::getSslVerify());
    // Restore original
    HttpClient::setSslVerify(original);
  }

  HTEST("HttpClient::resolveRedirectUrl protocol-relative");
  {
    // Protocol-relative URL: //host/path -> scheme://host/path
    HEXPECT_EQ(HttpClient::resolveRedirectUrl("https://example.com/a",
                                              "//other.com/b"),
               "https://other.com/b");
    HEXPECT_EQ(HttpClient::resolveRedirectUrl("http://example.com/a",
                                              "//other.com:8080/c"),
               "http://other.com:8080/c");
  }

  std::cout << "======= HTTP Client Unit Test Done: " << g_http_passed
            << " passed, " << g_http_failed << " failed =======" << std::endl;
}

inline asio::awaitable<void> test_http_client() { co_return; }

// -----------------------------------------------------------------------
// Integration test: HttpClient + Beast-based HttpServer
// -----------------------------------------------------------------------

inline void test_http_server_unit() {
  std::cout << "======= Test: HTTP Server Unit =======" << std::endl;

  HTEST("httpMethodIndex");
  {
    HEXPECT_EQ(httpMethodIndex(boost::beast::http::verb::get), 0);
    HEXPECT_EQ(httpMethodIndex(boost::beast::http::verb::head), 1);
    HEXPECT_EQ(httpMethodIndex(boost::beast::http::verb::post), 2);
    HEXPECT_EQ(httpMethodIndex(boost::beast::http::verb::put), 3);
    HEXPECT_EQ(httpMethodIndex(boost::beast::http::verb::delete_), 4);
    HEXPECT_EQ(httpMethodIndex(boost::beast::http::verb::connect), 5);
    HEXPECT_EQ(httpMethodIndex(boost::beast::http::verb::options), 6);
    HEXPECT_EQ(httpMethodIndex(boost::beast::http::verb::trace), 7);
    HEXPECT_EQ(httpMethodIndex(boost::beast::http::verb::patch), 8);
    HEXPECT_EQ(httpMethodIndex(static_cast<boost::beast::http::verb>(999)), -1);
  }

  HTEST("requestPath");
  {
    HEXPECT_EQ(requestPath("/"), "/");
    HEXPECT_EQ(requestPath("/path"), "/path");
    HEXPECT_EQ(requestPath("/path?q=1"), "/path");
    HEXPECT_EQ(requestPath("/a/b/c?x=y&z=w"), "/a/b/c");
    HEXPECT_EQ(requestPath(""), "");
    HEXPECT_EQ(requestPath("?alone"), "");
  }

  HTEST("isRedirectStatus");
  {
    HEXPECT_TRUE(HttpClient::isRedirectStatus(301));
    HEXPECT_TRUE(HttpClient::isRedirectStatus(302));
    HEXPECT_TRUE(HttpClient::isRedirectStatus(303));
    HEXPECT_TRUE(HttpClient::isRedirectStatus(307));
    HEXPECT_TRUE(HttpClient::isRedirectStatus(308));
    HEXPECT_FALSE(HttpClient::isRedirectStatus(200));
    HEXPECT_FALSE(HttpClient::isRedirectStatus(404));
    HEXPECT_FALSE(HttpClient::isRedirectStatus(500));
    HEXPECT_FALSE(HttpClient::isRedirectStatus(0));
  }

  HTEST("redirectChangesToGet");
  {
    HEXPECT_TRUE(HttpClient::redirectChangesToGet(301));
    HEXPECT_TRUE(HttpClient::redirectChangesToGet(302));
    HEXPECT_TRUE(HttpClient::redirectChangesToGet(303));
    HEXPECT_FALSE(HttpClient::redirectChangesToGet(307));
    HEXPECT_FALSE(HttpClient::redirectChangesToGet(308));
    HEXPECT_FALSE(HttpClient::redirectChangesToGet(200));
  }

  HTEST("resolveRedirectUrl");
  {
    // absolute URL
    HEXPECT_EQ(HttpClient::resolveRedirectUrl("http://example.com/a",
                                              "http://other.com/b"),
               "http://other.com/b");
    // absolute path
    HEXPECT_EQ(HttpClient::resolveRedirectUrl("http://example.com/a", "/b/c"),
               "http://example.com/b/c");
    // relative path
    HEXPECT_EQ(HttpClient::resolveRedirectUrl("http://example.com/a/b", "c"),
               "http://example.com/a/c");
    // relative path with no parent
    HEXPECT_EQ(HttpClient::resolveRedirectUrl("http://example.com/", "c"),
               "http://example.com/c");
    // root path
    HEXPECT_EQ(HttpClient::resolveRedirectUrl("http://example.com/a/b/c", "/"),
               "http://example.com/");
  }

  std::cout << "======= HTTP Server Unit Test Done: " << g_http_passed
            << " passed, " << g_http_failed << " failed =======" << std::endl;
}

inline asio::awaitable<void> test_http_client_beast_server() {
  std::cout << "======= Test: HTTP Client + Beast Server Integration ======="
            << std::endl;

  using Server = HttpServer;
  using namespace boost::beast::http;

  // Build a handler helper: returns Handler for a simple string response
  auto strResp = [](const char *ct, std::string body,
                    status st =
                        status::ok) -> std::shared_ptr<Server::Handler> {
    return std::make_shared<Server::Handler>(
        [ct, body = std::move(body),
         st](Server::Request &, Server::Response &resp,
             const std::string &) -> asio::awaitable<void> {
          resp.result(st);
          resp.set(field::content_type, ct);
          resp.body() = std::move(body);
          resp.prepare_payload();
          co_return;
        });
  };

  Server server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});

  // GET /hello
  server.router().add("/hello", 0, strResp("text/plain", "hello world"));

  // GET /json
  server.router().add("/json", 0,
                      strResp("application/json", R"({"key":"value"})"));

  // GET /empty
  server.router().add("/empty", 0, strResp("text/plain", ""));

  // GET /status/201
  server.router().add("/status/201", 0,
                      strResp("text/plain", "created", status::created));

  // GET /status/500
  server.router().add(
      "/status/500", 0,
      strResp("text/plain", "server error", status::internal_server_error));

  // GET /headers – echo back X-Echo value
  server.router().add("/headers", 0,
                      std::make_shared<Server::Handler>(
                          [](Server::Request &req, Server::Response &resp,
                             const std::string &) -> asio::awaitable<void> {
                            resp.result(status::ok);
                            resp.set(field::content_type, "text/plain");
                            auto val = req[field::x_forwarded_for];
                            resp.body() =
                                val.empty() ? "(none)" : std::string(val);
                            resp.prepare_payload();
                            co_return;
                          }));

  // GET /search?q=xxx – use query params
  server.router().add("/search", 0,
                      std::make_shared<Server::Handler>(
                          [](Server::Request &req, Server::Response &resp,
                             const std::string &) -> asio::awaitable<void> {
                            resp.result(status::ok);
                            resp.set(field::content_type, "text/plain");
                            auto target = req.target();
                            resp.body() = std::string(target);
                            resp.prepare_payload();
                            co_return;
                          }));

  // POST /echo
  server.router().add("/echo", 2,
                      std::make_shared<Server::Handler>(
                          [](Server::Request &req, Server::Response &resp,
                             const std::string &) -> asio::awaitable<void> {
                            resp.result(status::ok);
                            resp.set(field::content_type, "text/plain");
                            resp.body() = req.body();
                            resp.prepare_payload();
                            co_return;
                          }));

  // PUT /echo – prefix with "put:"
  server.router().add("/echo", 3,
                      std::make_shared<Server::Handler>(
                          [](Server::Request &req, Server::Response &resp,
                             const std::string &) -> asio::awaitable<void> {
                            resp.result(status::ok);
                            resp.set(field::content_type, "text/plain");
                            resp.body() = "put:" + req.body();
                            resp.prepare_payload();
                            co_return;
                          }));

  // DELETE /data
  server.router().add("/data", 4, strResp("text/plain", "deleted"));

  // Redirect: GET /redirect-me -> 302 Location: /hello
  server.router().add("/redirect-me", 0,
                      std::make_shared<Server::Handler>(
                          [](Server::Request &, Server::Response &resp,
                             const std::string &) -> asio::awaitable<void> {
                            resp.result(status::found);
                            resp.set(field::location, "/hello");
                            resp.prepare_payload();
                            co_return;
                          }));

  // Redirect loop: GET /redirect-loop -> 302 Location: /redirect-loop
  server.router().add("/redirect-loop", 0,
                      std::make_shared<Server::Handler>(
                          [](Server::Request &, Server::Response &resp,
                             const std::string &) -> asio::awaitable<void> {
                            resp.result(status::found);
                            resp.set(field::location, "/redirect-loop");
                            resp.prepare_payload();
                            co_return;
                          }));

  // Wildcard: GET /wildcard/*
  server.router().add(
      "/wildcard/*", 0,
      std::make_shared<Server::Handler>(
          [](Server::Request &, Server::Response &resp,
             const std::string &matched_path) -> asio::awaitable<void> {
            resp.result(status::ok);
            resp.set(field::content_type, "text/plain");
            resp.body() = "matched:" + matched_path;
            resp.prepare_payload();
            co_return;
          }));

  // PATCH /echo – prefix with "patch:"
  server.router().add("/echo", 8,
                      std::make_shared<Server::Handler>(
                          [](Server::Request &req, Server::Response &resp,
                             const std::string &) -> asio::awaitable<void> {
                            resp.result(status::ok);
                            resp.set(field::content_type, "text/plain");
                            resp.body() = "patch:" + req.body();
                            resp.prepare_payload();
                            co_return;
                          }));

  // DELETE /echo – echo body
  server.router().add("/echo", 4,
                      std::make_shared<Server::Handler>(
                          [](Server::Request &req, Server::Response &resp,
                             const std::string &) -> asio::awaitable<void> {
                            resp.result(status::ok);
                            resp.set(field::content_type, "text/plain");
                            resp.body() = "delete:" + req.body();
                            resp.prepare_payload();
                            co_return;
                          }));

  // HEAD /hello – should return headers only, no body
  server.router().add("/hello", 1,
                      std::make_shared<Server::Handler>(
                          [](Server::Request &, Server::Response &resp,
                             const std::string &) -> asio::awaitable<void> {
                            resp.result(status::ok);
                            resp.set(field::content_type, "text/plain");
                            resp.body() = "hello world";
                            resp.prepare_payload();
                            co_return;
                          }));

  // GET /big-body – returns a configurable-size body for limit testing
  server.router().add("/big-body", 0,
                      std::make_shared<Server::Handler>(
                          [](Server::Request &req, Server::Response &resp,
                             const std::string &) -> asio::awaitable<void> {
                            resp.result(status::ok);
                            resp.set(field::content_type, "text/plain");
                            // Default 1000 bytes, or use ?size=NNNN
                            std::string target = std::string(req.target());
                            size_t size = 1000;
                            auto pos = target.find("size=");
                            if (pos != std::string::npos) {
                              size = std::stoul(target.substr(pos + 5));
                            }
                            resp.body() = std::string(size, 'x');
                            resp.prepare_payload();
                            co_return;
                          }));

  // GET /redirect-proto-rel – not used (protocol-relative needs cross-host)

  // Start server
  std::thread serverThread([&server]() { server.start(); });

  // Wait for the server to be ready
  uint16_t port = 0;
  for (int i = 0; i < 100; ++i) {
    port = server.port();
    if (port != 0)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (port == 0) {
    std::cerr << "    FAIL: Server failed to start" << std::endl;
    g_http_failed++;
    server.stop();
    serverThread.join();
    co_return;
  }

  // Verify server is reachable
  for (int i = 0; i < 100; ++i) {
    try {
      boost::asio::io_context tmpCtx;
      boost::asio::ip::tcp::socket sock(tmpCtx);
      sock.connect(boost::asio::ip::tcp::endpoint(
          boost::asio::ip::make_address("127.0.0.1"), port));
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

  HTEST("getAsync basic - bool.run");
  {
    auto resp = co_await HttpClient::getAsync("https://blog.music.bool.run/");
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      HEXPECT_EQ(resp.value().status, 200);
      HEXPECT_TRUE(resp.value().isSuccess());
    }
  }

  HTEST("getAsync basic – Beast server");
  {
    auto resp = co_await HttpClient::getAsync(baseUrl + "/hello");
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      HEXPECT_EQ(resp.value().status, 200);
      HEXPECT_EQ(resp.value().body, "hello world");
      HEXPECT_TRUE(resp.value().isSuccess());
    }
  }

  HTEST("getAsync JSON – Beast server");
  {
    auto resp = co_await HttpClient::getAsync(baseUrl + "/json");
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      HEXPECT_EQ(resp.value().status, 200);
      auto j = resp.value().bodyJson();
      HEXPECT_HAS_VALUE(j);
      if (j.has_value())
        HEXPECT_EQ(j.value()["key"].get<std::string>(), "value");
    }
  }

  HTEST("getAsync empty response – Beast server");
  {
    auto resp = co_await HttpClient::getAsync(baseUrl + "/empty");
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      HEXPECT_EQ(resp.value().status, 200);
      HEXPECT_EQ(resp.value().body, "");
    }
  }

  HTEST("getAsync 201 – Beast server");
  {
    auto resp = co_await HttpClient::getAsync(baseUrl + "/status/201");
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      HEXPECT_EQ(resp.value().status, 201);
      HEXPECT_TRUE(resp.value().isSuccess());
    }
  }

  HTEST("getAsync 500 – Beast server");
  {
    auto resp = co_await HttpClient::getAsync(baseUrl + "/status/500");
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      HEXPECT_EQ(resp.value().status, 500);
      HEXPECT_FALSE(resp.value().isSuccess());
    }
  }

  HTEST("postAsync – Beast server");
  {
    auto resp = co_await HttpClient::postAsync(baseUrl + "/echo", "body data",
                                               "text/plain");
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      HEXPECT_EQ(resp.value().status, 200);
      HEXPECT_EQ(resp.value().body, "body data");
    }
  }

  HTEST("postAsync JSON body – Beast server");
  {
    neograph::json j = {{"msg", "hi"}};
    auto resp = co_await HttpClient::postAsync(baseUrl + "/echo", j);
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      HEXPECT_EQ(resp.value().status, 200);
      HEXPECT_EQ(resp.value().body, R"({"msg":"hi"})");
    }
  }

  HTEST("putAsync – Beast server");
  {
    auto resp = co_await HttpClient::putAsync(baseUrl + "/echo", "update");
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      HEXPECT_EQ(resp.value().body, "put:update");
    }
  }

  HTEST("DELETE route – 405 for wrong method");
  {
    // /data only has a DELETE handler; GET should return 405
    auto resp = co_await HttpClient::getAsync(baseUrl + "/data");
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      HEXPECT_EQ(resp.value().status, 405);
    }
  }

  HTEST("Wildcard route – Beast server");
  {
    auto resp = co_await HttpClient::getAsync(baseUrl + "/wildcard/foo/bar");
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      HEXPECT_EQ(resp.value().status, 200);
      // matched_path should contain the wildcard segment
      HEXPECT_TRUE(resp.value().body.find("wildcard") != std::string::npos);
    }
  }

  HTEST("Not Found (404) – Beast server");
  {
    auto resp = co_await HttpClient::getAsync(baseUrl + "/nonexistent");
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      HEXPECT_EQ(resp.value().status, 404);
      HEXPECT_FALSE(resp.value().isSuccess());
    }
  }

  HTEST("Method Not Allowed (405) – Beast server");
  {
    // POST to a GET-only route
    auto resp = co_await HttpClient::postAsync(baseUrl + "/hello", "body",
                                               "text/plain");
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      HEXPECT_EQ(resp.value().status, 405);
      HEXPECT_FALSE(resp.value().isSuccess());
    }
  }

  HTEST("Query string stripped for routing – Beast server");
  {
    auto resp = co_await HttpClient::getAsync(baseUrl + "/search?q=hello");
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      HEXPECT_EQ(resp.value().status, 200);
      // The handler echoes the full target
      HEXPECT_TRUE(resp.value().body.find("q=hello") != std::string::npos);
    }
  }

  HTEST("getAsync timeout – Beast server");
  {
    auto resp = co_await HttpClient::getAsync(
        "http://192.0.2.1:9999/nonexistent", {}, std::chrono::milliseconds{50});
    HEXPECT_FALSE(resp.has_value());
  }

  HTEST("Server stop and restart sanity");
  { HEXPECT_FALSE(server.isStopped()); }

  HTEST("Redirect followRedirect=0 does not follow");
  {
    auto resp = co_await HttpClient::getAsync(baseUrl + "/redirect-me", {},
                                              std::chrono::seconds{10}, 0);
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      HEXPECT_EQ(resp.value().status, 302);
      auto loc = resp.value().findHeader("location");
      HEXPECT_EQ(loc, "/hello");
    }
  }

  HTEST("Redirect followRedirect=1 follows to final");
  {
    auto resp = co_await HttpClient::getAsync(baseUrl + "/redirect-me", {},
                                              std::chrono::seconds{10}, 1);
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      HEXPECT_EQ(resp.value().status, 200);
      HEXPECT_EQ(resp.value().body, "hello world");
    }
  }

  HTEST("Redirect loop stops at max redirects");
  {
    auto resp = co_await HttpClient::getAsync(baseUrl + "/redirect-loop", {},
                                              std::chrono::seconds{10}, 3);
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      // Should stop at the last redirect (302) since it exceeds max
      HEXPECT_EQ(resp.value().status, 302);
    }
  }

  // -----------------------------------------------------------------------
  // New tests for production readiness improvements
  // -----------------------------------------------------------------------

  HTEST("DELETE method with body – Beast server");
  {
    auto resp = co_await HttpClient::deleteAsync(baseUrl + "/echo");
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      HEXPECT_EQ(resp.value().status, 200);
      HEXPECT_EQ(resp.value().body, "delete:");
    }
  }

  HTEST("PATCH method – Beast server");
  {
    auto resp = co_await HttpClient::patchAsync(baseUrl + "/echo", "patchdata");
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      HEXPECT_EQ(resp.value().status, 200);
      HEXPECT_EQ(resp.value().body, "patch:patchdata");
    }
  }

  HTEST("Response Date header present – Beast server");
  {
    auto resp = co_await HttpClient::getAsync(baseUrl + "/hello");
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      auto date = resp.value().findHeader("date");
      HEXPECT_FALSE(date.empty());
      // Date should contain "GMT" per RFC 7231
      HEXPECT_TRUE(date.find("GMT") != std::string_view::npos);
    }
  }

  HTEST("Response Server header present – Beast server");
  {
    auto resp = co_await HttpClient::getAsync(baseUrl + "/hello");
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      auto srv = resp.value().findHeader("server");
      HEXPECT_FALSE(srv.empty());
    }
  }

  HTEST("Response body size limit (client-side)");
  {
    // Request a 100-byte body with a 50-byte limit → should fail
    auto resp = co_await HttpClient::getAsync(
        baseUrl + "/big-body?size=100", {}, std::chrono::seconds{5}, 3, 50);
    // Body limit exceeded should result in an error (no value)
    HEXPECT_FALSE(resp.has_value());
  }

  HTEST("Response body size limit not exceeded");
  {
    // Request a 100-byte body with a 200-byte limit → should succeed
    auto resp = co_await HttpClient::getAsync(
        baseUrl + "/big-body?size=100", {}, std::chrono::seconds{5}, 3, 200);
    HEXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      HEXPECT_EQ(resp.value().status, 200);
      HEXPECT_EQ(resp.value().body.size(), (size_t)100);
    }
  }

  HTEST("Server activeConnections tracking");
  {
    // Wait for pending connection cleanup (server-side coroutine teardown)
    size_t conn = 1;
    for (size_t i = 0; i < 50; ++i) {
      conn = server.activeConnections();
      if (conn == 0)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    HEXPECT_EQ(conn, (size_t)0);
  }

  HTEST("fetchMarkdown returns error on non-2xx");
  {
    // fetchMarkdown should return error (not UB) for 404
    auto result = co_await HttpClient::fetchMarkdown(baseUrl + "/nonexistent",
                                                     std::chrono::seconds{5});
    HEXPECT_FALSE(result.has_value());
  }

  HTEST("fetchMarkdown success on HTML");
  {
    // Register an HTML route for markdown conversion
    // Using /hello which returns "hello world" (text/plain)
    // fetchMarkdown checks success status, not content-type
    auto result = co_await HttpClient::fetchMarkdown(baseUrl + "/hello",
                                                     std::chrono::seconds{5});
    HEXPECT_HAS_VALUE(result);
  }

  server.stop();
  serverThread.join();

  std::cout << "======= HTTP Client + Beast Server Integration Test Done: "
            << g_http_passed << " passed, " << g_http_failed
            << " failed =======" << std::endl;
}

inline asio::awaitable<void> run_http_client_tests() {
  test_http_client_unit();
  test_http_server_unit();
  co_await test_http_client();
  co_await test_http_client_beast_server();
}

} // namespace test
} // namespace agentxx