#include "test_http.h"

namespace agentxx {
namespace test {

using namespace agentxx::util;

int g_http_passed = 0;
int g_http_failed = 0;

template <typename T>
void expect_has_value_impl(T &&expr, const char *file, int line) {
  auto &&tmp = std::forward<T>(expr);
  if (tmp.has_value()) {
    ++XX_TEST_PASSED;
  } else {
    ++XX_TEST_FAILED;
    if constexpr (requires { tmp.error(); }) {
      TEST_FAIL << "expected has_value at " << file << ":" << line << " | "
                << tmp.error() << std::endl;
    } else {
      TEST_FAIL << "expected has_value at " << file << ":" << line << std::endl;
    }
  }
}

void test_http_client_unit() {

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
    XX_TEST_EXPECT_TRUE(
        HttpResponse::isJsonContentType("application/vnd.api+json"));
    XX_TEST_EXPECT_FALSE(HttpResponse::isJsonContentType("text/plain"));
    XX_TEST_EXPECT_FALSE(HttpResponse::isJsonContentType("text/html"));
    XX_TEST_EXPECT_FALSE(HttpResponse::isJsonContentType("application/xml"));
    XX_TEST_EXPECT_FALSE(HttpResponse::isJsonContentType(""));
    XX_TEST_EXPECT_FALSE(
        HttpResponse::isJsonContentType("application/octet-stream"));
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
    XX_TEST_EXPECT_TRUE(
        HttpResponse::isTextContentType("application/x-www-form-urlencoded"));
    XX_TEST_EXPECT_FALSE(
        HttpResponse::isTextContentType("application/octet-stream"));
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
    XX_TEST_EXPECT_TRUE(
        HttpClient::isValidUrl("https://example.com:8080/path"));
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
    XX_TEST_EXPECT_EQ(HttpClient::urlEncode("hello"), "hello");
    XX_TEST_EXPECT_EQ(HttpClient::urlEncode("hello world"), "hello+world");
    XX_TEST_EXPECT_EQ(HttpClient::urlEncode("abc123"), "abc123");
    XX_TEST_EXPECT_EQ(HttpClient::urlEncode("a&b=c"), "a%26b%3dc");
    XX_TEST_EXPECT_EQ(HttpClient::urlEncode(""), "");
    XX_TEST_EXPECT_EQ(HttpClient::urlEncode("中文"), "%e4%b8%ad%e6%96%87");
    // Additional edge cases
    XX_TEST_EXPECT_EQ(HttpClient::urlEncode("!@#$%^&*()"),
                      "%21%40%23%24%25%5e%26%2a%28%29");
    XX_TEST_EXPECT_EQ(HttpClient::urlEncode("-_.~"), "-_.~");
    XX_TEST_EXPECT_EQ(HttpClient::urlEncode("/path/to/file"),
                      "%2fpath%2fto%2ffile");
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
    XX_TEST_EXPECT_TRUE(original); // default should be true (verify enabled)
    HttpClient::setSslVerify(false);
    XX_TEST_EXPECT_FALSE(HttpClient::getSslVerify());
    HttpClient::setSslVerify(true);
    XX_TEST_EXPECT_TRUE(HttpClient::getSslVerify());
    // Restore original
    HttpClient::setSslVerify(original);
  }

  {
    // Protocol-relative URL: //host/path -> scheme://host/path
    XX_TEST_EXPECT_EQ(HttpClient::resolveRedirectUrl("https://example.com/a",
                                                     "//other.com/b"),
                      "https://other.com/b");
    XX_TEST_EXPECT_EQ(HttpClient::resolveRedirectUrl("http://example.com/a",
                                                     "//other.com:8080/c"),
                      "http://other.com:8080/c");
  }
}

asio::awaitable<void> test_http_client() { co_return; }

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
    XX_TEST_EXPECT_EQ(
        httpMethodIndex(static_cast<boost::beast::http::verb>(999)), -1);
  }

  {
    XX_TEST_EXPECT_EQ(requestPath("/"), "/");
    XX_TEST_EXPECT_EQ(requestPath("/path"), "/path");
    XX_TEST_EXPECT_EQ(requestPath("/path?q=1"), "/path");
    XX_TEST_EXPECT_EQ(requestPath("/a/b/c?x=y&z=w"), "/a/b/c");
    XX_TEST_EXPECT_EQ(requestPath(""), "");
    XX_TEST_EXPECT_EQ(requestPath("?alone"), "");
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
    XX_TEST_EXPECT_EQ(HttpClient::resolveRedirectUrl("http://example.com/a",
                                                     "http://other.com/b"),
                      "http://other.com/b");
    // absolute path
    XX_TEST_EXPECT_EQ(
        HttpClient::resolveRedirectUrl("http://example.com/a", "/b/c"),
        "http://example.com/b/c");
    // relative path
    XX_TEST_EXPECT_EQ(
        HttpClient::resolveRedirectUrl("http://example.com/a/b", "c"),
        "http://example.com/a/c");
    // relative path with no parent
    XX_TEST_EXPECT_EQ(
        HttpClient::resolveRedirectUrl("http://example.com/", "c"),
        "http://example.com/c");
    // root path
    XX_TEST_EXPECT_EQ(
        HttpClient::resolveRedirectUrl("http://example.com/a/b/c", "/"),
        "http://example.com/");
  }
}

asio::awaitable<void> test_http_client_beast_server() {

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
    TEST_FAIL << "Server failed to start" << std::endl;
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

  {
    HttpClient::setSslVerify(false);
    auto resp = co_await HttpClient::getAsync("https://blog.music.bool.run/");
    XX_TEST_EXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      XX_TEST_EXPECT_EQ(resp.value().status, 200);
      XX_TEST_EXPECT_TRUE(resp.value().isSuccess());
    }
  }

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
      if (j.has_value())
        XX_TEST_EXPECT_EQ(j.value()["key"].get<std::string>(), "value");
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
    auto resp = co_await HttpClient::postAsync(baseUrl + "/echo", "body data",
                                               "text/plain");
    XX_TEST_EXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      XX_TEST_EXPECT_EQ(resp.value().status, 200);
      XX_TEST_EXPECT_EQ(resp.value().body, "body data");
    }
  }

  {
    neograph::json j = {{"msg", "hi"}};
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
    }
  }

  {
    auto resp = co_await HttpClient::getAsync(baseUrl + "/wildcard/foo/bar");
    XX_TEST_EXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      XX_TEST_EXPECT_EQ(resp.value().status, 200);
      // matched_path should contain the wildcard segment
      XX_TEST_EXPECT_TRUE(resp.value().body.find("wildcard") !=
                          std::string::npos);
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
    auto resp = co_await HttpClient::postAsync(baseUrl + "/hello", "body",
                                               "text/plain");
    XX_TEST_EXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      XX_TEST_EXPECT_EQ(resp.value().status, 405);
      XX_TEST_EXPECT_FALSE(resp.value().isSuccess());
    }
  }

  {
    auto resp = co_await HttpClient::getAsync(baseUrl + "/search?q=hello");
    XX_TEST_EXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      XX_TEST_EXPECT_EQ(resp.value().status, 200);
      // The handler echoes the full target
      XX_TEST_EXPECT_TRUE(resp.value().body.find("q=hello") !=
                          std::string::npos);
    }
  }

  {
    auto resp = co_await HttpClient::getAsync(
        "http://192.0.2.1:9999/nonexistent", {}, std::chrono::milliseconds{50});
    XX_TEST_EXPECT_FALSE(resp.has_value());
  }

  { XX_TEST_EXPECT_FALSE(server.isStopped()); }

  {
    auto resp = co_await HttpClient::getAsync(baseUrl + "/redirect-me", {},
                                              std::chrono::seconds{10}, 0);
    XX_TEST_EXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      XX_TEST_EXPECT_EQ(resp.value().status, 302);
      auto loc = resp.value().findHeader("location");
      XX_TEST_EXPECT_EQ(loc, "/hello");
    }
  }

  {
    auto resp = co_await HttpClient::getAsync(baseUrl + "/redirect-me", {},
                                              std::chrono::seconds{10}, 1);
    XX_TEST_EXPECT_HAS_VALUE(resp);
    if (resp.has_value()) {
      XX_TEST_EXPECT_EQ(resp.value().status, 200);
      XX_TEST_EXPECT_EQ(resp.value().body, "hello world");
    }
  }

  {
    auto resp = co_await HttpClient::getAsync(baseUrl + "/redirect-loop", {},
                                              std::chrono::seconds{10}, 3);
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
        baseUrl + "/big-body?size=100", {}, std::chrono::seconds{5}, 3, 50);
    // Body limit exceeded should result in an error (no value)
    XX_TEST_EXPECT_FALSE(resp.has_value());
  }

  {
    // Request a 100-byte body with a 200-byte limit → should succeed
    auto resp = co_await HttpClient::getAsync(
        baseUrl + "/big-body?size=100", {}, std::chrono::seconds{5}, 3, 200);
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
      if (conn == 0)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    XX_TEST_EXPECT_EQ(conn, (size_t)0);
  }

  {
    // fetchMarkdown should return error (not UB) for 404
    auto result = co_await HttpClient::fetchMarkdown(baseUrl + "/nonexistent",
                                                     std::chrono::seconds{5});
    XX_TEST_EXPECT_FALSE(result.has_value());
  }

  {
    // Register an HTML route for markdown conversion
    // Using /hello which returns "hello world" (text/plain)
    // fetchMarkdown checks success status, not content-type
    auto result = co_await HttpClient::fetchMarkdown(baseUrl + "/hello",
                                                     std::chrono::seconds{5});
    XX_TEST_EXPECT_HAS_VALUE(result);
  }

  server.stop();
  serverThread.join();
}

asio::awaitable<TestResult> run_http_client_tests() {
  test_http_client_unit();
  test_http_server_unit();
  co_await test_http_client();
  co_await test_http_client_beast_server();
  co_return TestResult{g_http_passed, g_http_failed};
}

} // namespace test
} // namespace agentxx
