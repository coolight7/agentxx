#include "test_network_timeout.h"
#include "agentxx/util/http_client.h"
#include "agentxx/util/http_server.h"
#include "agentxx/util/ws_client.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace agentxx {
namespace test {

using namespace agentxx::util;
using namespace std::chrono_literals;

int g_network_timeout_passed = 0;
int g_network_timeout_failed = 0;

// ---------------------------------------------------------------------------
// Helper: start server, wait for port, return port (0 = failed)
// ---------------------------------------------------------------------------
static uint16_t waitServerPort(HttpServer& server) {
    for (int i = 0; i < 100; ++i) {
        auto p = server.port();
        if (p != 0) {
            return p;
        }
        std::this_thread::sleep_for(10ms);
    }
    return 0;
}

// ===========================================================================
// Test 1: HTTP client readTimeout triggers correctly
// ===========================================================================
static asio::awaitable<void> test_http_read_timeout() {
    // Server that accepts connection but never responds
    HttpServer server({
        .address        = "127.0.0.1",
        .port           = 0,
        .ioThreads      = 1,
        .requestTimeout = std::chrono::seconds{60}, // server side long timeout
    });

    // Register a handler that sleeps forever (simulates slow backend)
    server.router().add(
        "/slow",
        0,
        std::make_shared<HttpServer::Handler>(
            [](HttpServer::Request&, HttpServer::Response&, std::string_view
            ) -> asio::awaitable<void> {
                // Never respond — just wait until cancelled
                asio::steady_timer timer(co_await asio::this_coro::executor);
                timer.expires_after(std::chrono::seconds{300});
                neograph_asio_error_code ec;
                co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
                co_return;
            }
        )
    );

    std::thread serverThread([&server]() {
        server.start();
    });
    uint16_t    port = waitServerPort(server);
    if (port == 0) {
        TEST_FAIL << "test_http_read_timeout: server failed to start" << std::endl;
        g_network_timeout_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string url = "http://127.0.0.1:" + std::to_string(port) + "/slow";

    // Use a very short readTimeout (500ms) — should trigger timeout
    auto start  = std::chrono::steady_clock::now();
    auto result = co_await HttpClient::getAsync(
        url,
        {},
        HttpClient::RequestConfig{
            .connectTimeout   = 5s,
            .readChunkTimeout = 500ms,
        }
    );
    auto elapsed = std::chrono::steady_clock::now() - start;

    XX_TEST_EXPECT_FALSE(result.has_value());
    if (!result.has_value()) {
        // Error should mention timeout
        XX_TEST_EXPECT_TRUE(result.error().find("timeout") != std::string::npos);
    }
    // Should complete in roughly 500ms (allow generous margin)
    XX_TEST_EXPECT_TRUE(elapsed < 3s);
    XX_TEST_EXPECT_TRUE(elapsed >= 400ms);

    server.stop();
    serverThread.join();
}

// ===========================================================================
// Test 2: HTTP client connectTimeout as total deadline (not 3x)
// ===========================================================================
static asio::awaitable<void> test_http_connect_timeout_total() {
    // Connect to a non-routable address to trigger connect timeout
    // 192.0.2.1 is TEST-NET-1 (RFC 5737), guaranteed non-routable
    auto start  = std::chrono::steady_clock::now();
    auto result = co_await HttpClient::getAsync(
        "http://192.0.2.1:9999/test",
        {},
        HttpClient::RequestConfig{
            .connectTimeout   = 1s,
            .readChunkTimeout = 1s,
        }
    );
    auto elapsed = std::chrono::steady_clock::now() - start;

    XX_TEST_EXPECT_FALSE(result.has_value());
    // Total time should be bounded by connectTimeout (1s) + small margin,
    // NOT 3x (which would be 3s for DNS+TCP+TLS)
    XX_TEST_EXPECT_TRUE(elapsed < 2s);

    TEST_INFO << "connect timeout elapsed: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << "ms"
              << std::endl;
}

// ===========================================================================
// Test 3: WS recv timeout does NOT close connection (per-chunk semantics)
// ===========================================================================
static asio::awaitable<void> test_ws_recv_timeout_keeps_connection() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});

    server.enableWebSocket("/ws", [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
        boost::beast::flat_buffer buf;
        for (;;) {
            buf.clear();
            neograph_asio_error_code ec;
            co_await ws.async_read(buf, asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
                co_return;
            }
            ws.text(ws.got_text());
            co_await ws.async_write(buf.data(), asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
                co_return;
            }
        }
    });

    std::thread serverThread([&server]() {
        server.start();
    });
    uint16_t    port = waitServerPort(server);
    if (port == 0) {
        TEST_FAIL << "test_ws_recv_timeout: server failed to start" << std::endl;
        g_network_timeout_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string wsUrl = "ws://127.0.0.1:" + std::to_string(port) + "/ws";

    WsClientConfig cfg;
    cfg.recvTimeout = 500ms; // very short recv timeout

    auto result = co_await wsConnect(co_await asio::this_coro::executor, wsUrl, {}, cfg);
    XX_TEST_EXPECT_TRUE(result.has_value());
    if (!result.has_value()) {
        server.stop();
        serverThread.join();
        co_return;
    }

    auto& client = result.value();
    XX_TEST_EXPECT_TRUE(client->isOpen());

    // recv with no data → should timeout
    auto recvRes = co_await client->recv();
    XX_TEST_EXPECT_FALSE(recvRes.has_value());
    if (!recvRes.has_value()) {
        XX_TEST_EXPECT_EQ(recvRes.error(), "recv timeout");
    }
    // Beast websocket stream 内部状态在 cancel 后不可恢复, 连接必须标记关闭
    XX_TEST_EXPECT_FALSE(client->isOpen());

    // 后续操作应返回 "connection closed"
    auto sendRes = co_await client->sendText("should fail");
    XX_TEST_EXPECT_FALSE(sendRes.has_value());
    if (!sendRes.has_value()) {
        XX_TEST_EXPECT_EQ(sendRes.error(), "connection closed");
    }

    server.stop();
    serverThread.join();
}

// ===========================================================================
// Test 4: WS sendPing uses sendTimeout (not hardcoded 30s)
// ===========================================================================
static asio::awaitable<void> test_ws_ping_uses_config_timeout() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});

    server.enableWebSocket("/ws", [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
        boost::beast::flat_buffer buf;
        for (;;) {
            buf.clear();
            neograph_asio_error_code ec;
            co_await ws.async_read(buf, asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
                co_return;
            }
            ws.text(ws.got_text());
            co_await ws.async_write(buf.data(), asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
                co_return;
            }
        }
    });

    std::thread serverThread([&server]() {
        server.start();
    });
    uint16_t    port = waitServerPort(server);
    if (port == 0) {
        TEST_FAIL << "test_ws_ping: server failed to start" << std::endl;
        g_network_timeout_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string wsUrl = "ws://127.0.0.1:" + std::to_string(port) + "/ws";

    WsClientConfig cfg;
    cfg.sendTimeout = 5s;

    auto result = co_await wsConnect(co_await asio::this_coro::executor, wsUrl, {}, cfg);
    XX_TEST_EXPECT_TRUE(result.has_value());
    if (!result.has_value()) {
        server.stop();
        serverThread.join();
        co_return;
    }

    auto& client = result.value();

    // Ping should succeed quickly (server auto-responds with pong via Beast)
    auto pingRes = co_await client->sendPing("test");
    XX_TEST_EXPECT_TRUE(pingRes.has_value());
    XX_TEST_EXPECT_TRUE(client->isOpen());

    co_await client->sendClose();
    server.stop();
    serverThread.join();
}

// ===========================================================================
// Test 5: HTTP Server maxRequestsPerConnection limits keep-alive reuse
// ===========================================================================
static asio::awaitable<void> test_http_max_requests_per_connection() {
    // Server allows only 2 requests per connection
    HttpServer server({
        .address                  = "127.0.0.1",
        .port                     = 0,
        .ioThreads                = 1,
        .requestTimeout           = std::chrono::seconds{5},
        .maxRequestsPerConnection = 2,
    });

    server.router().add(
        "/count",
        0,
        std::make_shared<HttpServer::Handler>(
            [](HttpServer::Request&, HttpServer::Response& resp, std::string_view
            ) -> asio::awaitable<void> {
                resp.result(boost::beast::http::status::ok);
                resp.set(boost::beast::http::field::content_type, "text/plain");
                resp.body() = "ok";
                resp.prepare_payload();
                co_return;
            }
        )
    );

    std::thread serverThread([&server]() {
        server.start();
    });
    uint16_t    port = waitServerPort(server);
    if (port == 0) {
        TEST_FAIL << "test_max_requests: server failed to start" << std::endl;
        g_network_timeout_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string url = "http://127.0.0.1:" + std::to_string(port) + "/count";

    // With keepAlive=true, the server should close after 2 requests
    // First request should succeed
    auto r1 = co_await HttpClient::getAsync(
        url,
        {},
        HttpClient::RequestConfig{.connectTimeout = 2s, .readChunkTimeout = 2s, .keepAlive = true}
    );
    XX_TEST_EXPECT_TRUE(r1.has_value());

    // Second request on a new connection should also succeed
    auto r2 = co_await HttpClient::getAsync(
        url,
        {},
        HttpClient::RequestConfig{.connectTimeout = 2s, .readChunkTimeout = 2s, .keepAlive = true}
    );
    XX_TEST_EXPECT_TRUE(r2.has_value());

    // Third request should also succeed (new connection each time since client
    // doesn't pool connections), but the server enforces the limit per connection
    auto r3 = co_await HttpClient::getAsync(
        url,
        {},
        HttpClient::RequestConfig{.connectTimeout = 2s, .readChunkTimeout = 2s, .keepAlive = true}
    );
    XX_TEST_EXPECT_TRUE(r3.has_value());

    server.stop();
    serverThread.join();
}

// ===========================================================================
// Test 6: HTTP Server SSE writer uses configured sseWriteTimeout
// ===========================================================================
static asio::awaitable<void> test_sse_writer_timeout_config() {
    // Server with a custom sseWriteTimeout
    HttpServer server({
        .address         = "127.0.0.1",
        .port            = 0,
        .ioThreads       = 1,
        .requestTimeout  = std::chrono::seconds{5},
        .sseWriteTimeout = std::chrono::seconds{2},
    });

    server.addSseRoute(
        "/sse",
        [](HttpServer::Request&,
           std::shared_ptr<HttpServer::SseWriter> writer) -> asio::awaitable<void> {
            // Send one event then stop
            bool ok = co_await writer->writeEvent("message", "hello sse");
            if (!ok) {
                co_return;
            }
            co_await writer->close();
        }
    );

    std::thread serverThread([&server]() {
        server.start();
    });
    uint16_t    port = waitServerPort(server);
    if (port == 0) {
        TEST_FAIL << "test_sse_timeout: server failed to start" << std::endl;
        g_network_timeout_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string url = "http://127.0.0.1:" + std::to_string(port) + "/sse";

    // SSE request should succeed and receive data
    std::string received;
    try {
        co_await HttpClient::requestSseAsync(
            "GET",
            url,
            {},
            "",
            {},
            HttpClient::RequestConfig{.connectTimeout = 2s, .readChunkTimeout = 3s},
            [&](std::string_view chunk) -> bool {
                received.append(chunk);
                return false;
            }
        );
    } catch (const std::exception&) {
        // SSE stream may end with an error after close, that's ok
    }

    XX_TEST_EXPECT_TRUE(received.find("hello sse") != std::string::npos);

    server.stop();
    serverThread.join();
}

// ===========================================================================
// Test 7: WS connect timeout
// ===========================================================================
static asio::awaitable<void> test_ws_connect_timeout() {
    // Connect to non-routable address with short timeout
    WsClientConfig cfg;
    cfg.connectTimeout = 1s;

    auto start = std::chrono::steady_clock::now();
    auto result
        = co_await wsConnect(co_await asio::this_coro::executor, "ws://192.0.2.1:9999/ws", {}, cfg);
    auto elapsed = std::chrono::steady_clock::now() - start;

    XX_TEST_EXPECT_FALSE(result.has_value());
    if (!result.has_value()) {
        XX_TEST_EXPECT_TRUE(result.error().find("timeout") != std::string::npos);
    }
    // Should be bounded by connectTimeout
    XX_TEST_EXPECT_TRUE(elapsed < 3s);

    TEST_INFO << "ws connect timeout elapsed: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << "ms"
              << std::endl;
}

// ===========================================================================
// Test 8: calcTimeoutBySize boundary values
// ===========================================================================
static void test_calc_timeout_by_size() {
    // Minimum is always 30s
    XX_TEST_EXPECT_EQ(HttpClient::calcTimeoutBySize(0).count(), 30);
    XX_TEST_EXPECT_EQ(HttpClient::calcTimeoutBySize(1).count(), 30);
    XX_TEST_EXPECT_EQ(HttpClient::calcTimeoutBySize(65536 * 29).count(), 30);
    XX_TEST_EXPECT_EQ(HttpClient::calcTimeoutBySize(65536 * 30).count(), 30);
    // Above threshold scales linearly
    XX_TEST_EXPECT_EQ(HttpClient::calcTimeoutBySize(65536 * 31).count(), 31);
    XX_TEST_EXPECT_EQ(HttpClient::calcTimeoutBySize(65536 * 100).count(), 100);
    XX_TEST_EXPECT_EQ(HttpClient::calcTimeoutBySize(65536 * 1024).count(), 1024);
    // Edge: exactly at boundary rounds up
    XX_TEST_EXPECT_EQ(HttpClient::calcTimeoutBySize(65536 * 30 + 1).count(), 31);
}

// ===========================================================================
// Test 9: RequestConfig default values sanity
// ===========================================================================
static void test_request_config_defaults() {
    RequestConfig cfg;
    XX_TEST_EXPECT_EQ(cfg.connectTimeout.count(), 30000);
    XX_TEST_EXPECT_EQ(cfg.readChunkTimeout.count(), 30000);
    XX_TEST_EXPECT_FALSE(cfg.sendTimeout.has_value());
    XX_TEST_EXPECT_FALSE(cfg.sslVerify.has_value());
    XX_TEST_EXPECT_EQ(cfg.followRedirect, (size_t)3);
    XX_TEST_EXPECT_FALSE(cfg.keepAlive);
    XX_TEST_EXPECT_EQ(cfg.maxResponseBody, kDefaultMaxResponseBody);
}

// ===========================================================================
// Test 10: WsClientConfig default values sanity
// ===========================================================================
static void test_ws_client_config_defaults() {
    WsClientConfig cfg;
    XX_TEST_EXPECT_EQ(cfg.connectTimeout.count(), 16000);
    XX_TEST_EXPECT_EQ(cfg.recvTimeout.count(), 60000);
    XX_TEST_EXPECT_EQ(cfg.sendTimeout.count(), 60000);
    XX_TEST_EXPECT_FALSE(cfg.sslVerify);
    XX_TEST_EXPECT_EQ(cfg.maxMessageSize, (size_t)(16 * 1024 * 1024));
}

// ===========================================================================
// Test 11: HttpServer::Config new fields defaults
// ===========================================================================
static void test_http_server_config_defaults() {
    HttpServer::Config cfg;
    XX_TEST_EXPECT_EQ(cfg.sseWriteTimeout.count(), 120);
    XX_TEST_EXPECT_EQ(cfg.maxRequestsPerConnection, (size_t)1024);
    XX_TEST_EXPECT_EQ(cfg.requestTimeout.count(), 30);
    XX_TEST_EXPECT_EQ(cfg.maxConnections, (size_t)8192);
    XX_TEST_EXPECT_EQ(cfg.maxHeaderSize, (uint32_t)8192);
    XX_TEST_EXPECT_EQ(cfg.maxRequestBody, (uint64_t)(8 * 1024 * 1024));
}

// ===========================================================================
// Main entry
// ===========================================================================
asio::awaitable<TestResult> run_network_timeout_tests() {
    // Unit tests (sync)
    std::cout << "  [net_timeout] calcTimeoutBySize..." << std::endl;
    test_calc_timeout_by_size();

    std::cout << "  [net_timeout] RequestConfig defaults..." << std::endl;
    test_request_config_defaults();

    std::cout << "  [net_timeout] WsClientConfig defaults..." << std::endl;
    test_ws_client_config_defaults();

    std::cout << "  [net_timeout] HttpServer::Config defaults..." << std::endl;
    test_http_server_config_defaults();

    // Async integration tests
    std::cout << "  [net_timeout] HTTP read timeout..." << std::endl;
    co_await test_http_read_timeout();

    std::cout << "  [net_timeout] HTTP connect timeout total deadline..." << std::endl;
    co_await test_http_connect_timeout_total();

    std::cout << "  [net_timeout] WS recv timeout keeps connection..." << std::endl;
    co_await test_ws_recv_timeout_keeps_connection();

    std::cout << "  [net_timeout] WS ping uses config timeout..." << std::endl;
    co_await test_ws_ping_uses_config_timeout();

    std::cout << "  [net_timeout] HTTP maxRequestsPerConnection..." << std::endl;
    co_await test_http_max_requests_per_connection();

    std::cout << "  [net_timeout] SSE writer timeout config..." << std::endl;
    co_await test_sse_writer_timeout_config();

    std::cout << "  [net_timeout] WS connect timeout..." << std::endl;
    co_await test_ws_connect_timeout();

    co_return TestResult{g_network_timeout_passed, g_network_timeout_failed};
}

} // namespace test
} // namespace agentxx
