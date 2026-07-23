#include "test_websocket.h"
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
#include <vector>

namespace agentxx {
namespace test {

using namespace agentxx::util;

int g_websocket_passed = 0;
int g_websocket_failed = 0;

static asio::awaitable<void> test_ws_echo() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});

    server.enableWebSocket(
        "/ws",
        [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
            boost::beast::flat_buffer buf;
            for (;;) {
                buf.clear();
                boost::system::error_code ec;
                co_await ws.async_read(buf, asio::redirect_error(asio::use_awaitable, ec));
                if (ec) {
                    co_return;
                }
                bool isText = ws.got_text();
                ws.text(isText);
                co_await ws.async_write(
                    buf.data(),
                    asio::redirect_error(asio::use_awaitable, ec)
                );
                if (ec) {
                    co_return;
                }
            }
        }
    );

    std::thread serverThread([&server]() {
        server.start();
    });

    uint16_t port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        TEST_FAIL << "WS echo server failed to start" << std::endl;
        g_websocket_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string wsUrl = "ws://127.0.0.1:" + std::to_string(port) + "/ws";

    {
        auto result = co_await wsConnect(
            co_await asio::this_coro::executor,
            wsUrl
        );
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            auto& client = result.value();
            XX_TEST_EXPECT_TRUE(client->isOpen());

            auto sendRes = co_await client->sendText("hello websocket");
            XX_TEST_EXPECT_TRUE(sendRes.has_value());

            auto recvRes = co_await client->recv();
            XX_TEST_EXPECT_TRUE(recvRes.has_value());
            if (recvRes.has_value()) {
                XX_TEST_EXPECT_EQ(recvRes.value().payload, "hello websocket");
                XX_TEST_EXPECT_TRUE(recvRes.value().type == WsMessage::Type::Text);
            }

            co_await client->sendClose(1000, "done");
        }
    }

    server.stop();
    serverThread.join();
}

static asio::awaitable<void> test_ws_multiple_messages() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});

    server.enableWebSocket(
        "/ws",
        [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
            boost::beast::flat_buffer buf;
            for (;;) {
                buf.clear();
                boost::system::error_code ec;
                co_await ws.async_read(buf, asio::redirect_error(asio::use_awaitable, ec));
                if (ec) co_return;
                ws.text(ws.got_text());
                co_await ws.async_write(buf.data(), asio::redirect_error(asio::use_awaitable, ec));
                if (ec) co_return;
            }
        }
    );

    std::thread serverThread([&server]() { server.start(); });
    uint16_t    port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        g_websocket_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string wsUrl = "ws://127.0.0.1:" + std::to_string(port) + "/ws";

    {
        auto result = co_await wsConnect(co_await asio::this_coro::executor, wsUrl);
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            auto& client = result.value();

            for (int i = 0; i < 50; ++i) {
                std::string msg = "message_" + std::to_string(i);
                auto        sr  = co_await client->sendText(msg);
                XX_TEST_EXPECT_TRUE(sr.has_value());
                auto rr = co_await client->recv();
                XX_TEST_EXPECT_TRUE(rr.has_value());
                if (rr.has_value()) {
                    XX_TEST_EXPECT_EQ(rr.value().payload, msg);
                }
            }

            co_await client->sendClose();
        }
    }

    server.stop();
    serverThread.join();
}

static asio::awaitable<void> test_ws_binary() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});

    server.enableWebSocket(
        "/ws",
        [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
            boost::beast::flat_buffer buf;
            for (;;) {
                buf.clear();
                boost::system::error_code ec;
                co_await ws.async_read(buf, asio::redirect_error(asio::use_awaitable, ec));
                if (ec) co_return;
                ws.binary(true);
                co_await ws.async_write(buf.data(), asio::redirect_error(asio::use_awaitable, ec));
                if (ec) co_return;
            }
        }
    );

    std::thread serverThread([&server]() { server.start(); });
    uint16_t    port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        g_websocket_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string wsUrl = "ws://127.0.0.1:" + std::to_string(port) + "/ws";

    {
        auto result = co_await wsConnect(co_await asio::this_coro::executor, wsUrl);
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            auto& client = result.value();

            std::string binaryData;
            for (int i = 0; i < 256; ++i) {
                binaryData.push_back(static_cast<char>(i));
            }

            auto sr = co_await client->sendBinary(binaryData);
            XX_TEST_EXPECT_TRUE(sr.has_value());

            auto rr = co_await client->recv();
            XX_TEST_EXPECT_TRUE(rr.has_value());
            if (rr.has_value()) {
                XX_TEST_EXPECT_TRUE(rr.value().type == WsMessage::Type::Binary);
                XX_TEST_EXPECT_EQ(rr.value().payload.size(), (size_t)256);
                XX_TEST_EXPECT_TRUE(rr.value().payload == binaryData);
            }

            co_await client->sendClose();
        }
    }

    server.stop();
    serverThread.join();
}

static asio::awaitable<void> test_ws_large_message() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});

    server.enableWebSocket(
        "/ws",
        [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
            ws.read_message_max(64 * 1024 * 1024);
            boost::beast::flat_buffer buf;
            for (;;) {
                buf.clear();
                boost::system::error_code ec;
                co_await ws.async_read(buf, asio::redirect_error(asio::use_awaitable, ec));
                if (ec) co_return;
                ws.text(ws.got_text());
                co_await ws.async_write(buf.data(), asio::redirect_error(asio::use_awaitable, ec));
                if (ec) co_return;
            }
        }
    );

    std::thread serverThread([&server]() { server.start(); });
    uint16_t    port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        g_websocket_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string wsUrl = "ws://127.0.0.1:" + std::to_string(port) + "/ws";

    {
        auto result = co_await wsConnect(co_await asio::this_coro::executor, wsUrl);
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            auto& client = result.value();

            std::string largeMsg(1024 * 1024, 'A');
            auto        sr = co_await client->sendText(largeMsg);
            XX_TEST_EXPECT_TRUE(sr.has_value());

            auto rr = co_await client->recv();
            XX_TEST_EXPECT_TRUE(rr.has_value());
            if (rr.has_value()) {
                XX_TEST_EXPECT_EQ(rr.value().payload.size(), (size_t)(1024 * 1024));
            }

            co_await client->sendClose();
        }
    }

    server.stop();
    serverThread.join();
}

static asio::awaitable<void> test_ws_ping_pong() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});

    server.enableWebSocket(
        "/ws",
        [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
            boost::beast::flat_buffer buf;
            for (;;) {
                buf.clear();
                boost::system::error_code ec;
                co_await ws.async_read(buf, asio::redirect_error(asio::use_awaitable, ec));
                if (ec) co_return;
                ws.text(ws.got_text());
                co_await ws.async_write(buf.data(), asio::redirect_error(asio::use_awaitable, ec));
                if (ec) co_return;
            }
        }
    );

    std::thread serverThread([&server]() { server.start(); });
    uint16_t    port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        g_websocket_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string wsUrl = "ws://127.0.0.1:" + std::to_string(port) + "/ws";

    {
        auto result = co_await wsConnect(co_await asio::this_coro::executor, wsUrl);
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            auto& client = result.value();

            auto pingRes = co_await client->sendPing("ping-data");
            XX_TEST_EXPECT_TRUE(pingRes.has_value());

            auto sr = co_await client->sendText("after-ping");
            XX_TEST_EXPECT_TRUE(sr.has_value());
            auto rr = co_await client->recv();
            XX_TEST_EXPECT_TRUE(rr.has_value());
            if (rr.has_value()) {
                XX_TEST_EXPECT_EQ(rr.value().payload, "after-ping");
            }

            co_await client->sendClose();
        }
    }

    server.stop();
    serverThread.join();
}

static asio::awaitable<void> test_ws_server_close() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});

    server.enableWebSocket(
        "/ws",
        [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
            boost::beast::flat_buffer buf;
            boost::system::error_code ec;
            co_await ws.async_read(buf, asio::redirect_error(asio::use_awaitable, ec));
            if (ec) co_return;
            co_await ws.async_close(
                boost::beast::websocket::close_code::normal,
                asio::redirect_error(asio::use_awaitable, ec)
            );
        }
    );

    std::thread serverThread([&server]() { server.start(); });
    uint16_t    port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        g_websocket_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string wsUrl = "ws://127.0.0.1:" + std::to_string(port) + "/ws";

    {
        auto result = co_await wsConnect(co_await asio::this_coro::executor, wsUrl);
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            auto& client = result.value();

            co_await client->sendText("trigger-close");

            auto rr = co_await client->recv();
            XX_TEST_EXPECT_TRUE(rr.has_value());
            if (rr.has_value()) {
                XX_TEST_EXPECT_TRUE(rr.value().type == WsMessage::Type::Close);
            }

            XX_TEST_EXPECT_FALSE(client->isOpen());
        }
    }

    server.stop();
    serverThread.join();
}

static asio::awaitable<void> test_ws_invalid_path() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});

    server.enableWebSocket(
        "/ws",
        [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
            boost::beast::flat_buffer buf;
            boost::system::error_code ec;
            co_await ws.async_read(buf, asio::redirect_error(asio::use_awaitable, ec));
        }
    );

    std::thread serverThread([&server]() { server.start(); });
    uint16_t    port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        g_websocket_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    {
        std::string badUrl = "ws://127.0.0.1:" + std::to_string(port) + "/nonexistent";
        auto        result = co_await wsConnect(
            co_await asio::this_coro::executor,
            badUrl,
            {},
            WsClientConfig{.connectTimeout = std::chrono::milliseconds{2000}}
        );
        XX_TEST_EXPECT_FALSE(result.has_value());
    }

    server.stop();
    serverThread.join();
}

static asio::awaitable<void> test_ws_connect_refused() {
    {
        auto result = co_await wsConnect(
            co_await asio::this_coro::executor,
            "ws://127.0.0.1:19999/ws",
            {},
            WsClientConfig{.connectTimeout = std::chrono::milliseconds{500}}
        );
        XX_TEST_EXPECT_FALSE(result.has_value());
    }
}

static asio::awaitable<void> test_ws_invalid_url() {
    {
        auto result = co_await wsConnect(
            co_await asio::this_coro::executor,
            "http://127.0.0.1:8080/ws"
        );
        XX_TEST_EXPECT_FALSE(result.has_value());
    }
    {
        auto result = co_await wsConnect(
            co_await asio::this_coro::executor,
            "ws://"
        );
        XX_TEST_EXPECT_FALSE(result.has_value());
    }
}

static asio::awaitable<void> test_ws_concurrent_clients() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 2});

    server.enableWebSocket(
        "/ws",
        [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
            boost::beast::flat_buffer buf;
            for (;;) {
                buf.clear();
                boost::system::error_code ec;
                co_await ws.async_read(buf, asio::redirect_error(asio::use_awaitable, ec));
                if (ec) co_return;
                ws.text(ws.got_text());
                co_await ws.async_write(buf.data(), asio::redirect_error(asio::use_awaitable, ec));
                if (ec) co_return;
            }
        }
    );

    std::thread serverThread([&server]() { server.start(); });
    uint16_t    port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (port == 0) {
        g_websocket_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string wsUrl = "ws://127.0.0.1:" + std::to_string(port) + "/ws";
    auto        executor = co_await asio::this_coro::executor;

    constexpr int kNumClients = 5;
    int           successCount = 0;

    for (int c = 0; c < kNumClients; ++c) {
        auto result = co_await wsConnect(executor, wsUrl, {},
            WsClientConfig{.connectTimeout = std::chrono::milliseconds{5000}});
        if (result.has_value()) {
            auto& client = result.value();
            std::string msg = "client_" + std::to_string(c);
            co_await client->sendText(msg);
            auto rr = co_await client->recv();
            if (rr.has_value() && rr.value().payload == msg) {
                successCount++;
            } else {
                TEST_FAIL << "ws concurrent client " << c << ": echo mismatch" << std::endl;
            }
            co_await client->sendClose();
        } else {
            TEST_FAIL << "ws concurrent client " << c << ": connect failed: " << result.error()
                      << std::endl;
        }
        asio::steady_timer delay(executor, std::chrono::milliseconds(100));
        boost::system::error_code dec;
        co_await delay.async_wait(asio::redirect_error(asio::use_awaitable, dec));
    }
    XX_TEST_EXPECT_EQ(successCount, kNumClients);

    server.stop();
    serverThread.join();
}

static asio::awaitable<void> test_ws_client_disconnect() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});

    bool serverDetectedClose = false;

    server.enableWebSocket(
        "/ws",
        [&serverDetectedClose](HttpServer::WsStream& ws) -> asio::awaitable<void> {
            boost::beast::flat_buffer buf;
            for (;;) {
                buf.clear();
                boost::system::error_code ec;
                co_await ws.async_read(buf, asio::redirect_error(asio::use_awaitable, ec));
                if (ec) {
                    if (ec == boost::beast::websocket::error::closed
                        || ec == asio::error::eof
                        || ec == boost::beast::error::timeout) {
                        serverDetectedClose = true;
                    }
                    co_return;
                }
            }
        }
    );

    std::thread serverThread([&server]() { server.start(); });
    uint16_t    port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        g_websocket_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string wsUrl = "ws://127.0.0.1:" + std::to_string(port) + "/ws";

    {
        auto result = co_await wsConnect(co_await asio::this_coro::executor, wsUrl);
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            auto& client = result.value();
            co_await client->sendText("before-disconnect");
            co_await client->sendClose(1000, "bye");
        }
    }

    asio::steady_timer timer(
        co_await asio::this_coro::executor,
        std::chrono::milliseconds(200)
    );
    boost::system::error_code tec;
    co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, tec));

    XX_TEST_EXPECT_TRUE(serverDetectedClose);

    server.stop();
    serverThread.join();
}

static asio::awaitable<void> test_ws_start_async_mode() {
    asio::io_context serverCtx;
    HttpServer       server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});

    server.enableWebSocket(
        "/ws",
        [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
            boost::beast::flat_buffer buf;
            for (;;) {
                buf.clear();
                boost::system::error_code ec;
                co_await ws.async_read(buf, asio::redirect_error(asio::use_awaitable, ec));
                if (ec) co_return;
                ws.text(ws.got_text());
                co_await ws.async_write(buf.data(), asio::redirect_error(asio::use_awaitable, ec));
                if (ec) co_return;
            }
        }
    );

    server.router().add(
        "/health",
        0,
        std::make_shared<HttpServer::Handler>(
            [](HttpServer::Request&,
               HttpServer::Response& resp,
               const std::string&) -> asio::awaitable<void> {
                resp.result(boost::beast::http::status::ok);
                resp.set(boost::beast::http::field::content_type, "text/plain");
                resp.body() = "ok";
                resp.prepare_payload();
                co_return;
            }
        )
    );

    server.startAsync(serverCtx.get_executor());

    std::thread serverThread([&serverCtx]() {
        serverCtx.run();
    });

    uint16_t port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        g_websocket_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    {
        auto resp = co_await HttpClient::getAsync(
            "http://127.0.0.1:" + std::to_string(port) + "/health"
        );
        XX_TEST_EXPECT_TRUE(resp.has_value());
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().body, "ok");
        }
    }

    {
        std::string wsUrl = "ws://127.0.0.1:" + std::to_string(port) + "/ws";
        auto        result = co_await wsConnect(co_await asio::this_coro::executor, wsUrl);
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            auto& client = result.value();
            co_await client->sendText("async-mode-test");
            auto rr = co_await client->recv();
            XX_TEST_EXPECT_TRUE(rr.has_value());
            if (rr.has_value()) {
                XX_TEST_EXPECT_EQ(rr.value().payload, "async-mode-test");
            }
            co_await client->sendClose();
        }
    }

    server.stop();
    serverThread.join();
}

static asio::awaitable<void> test_ws_recv_timeout() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});

    server.enableWebSocket(
        "/ws",
        [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
            asio::steady_timer timer(
                co_await asio::this_coro::executor,
                std::chrono::seconds{30}
            );
            boost::system::error_code ec;
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        }
    );

    std::thread serverThread([&server]() { server.start(); });
    uint16_t    port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        g_websocket_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string wsUrl = "ws://127.0.0.1:" + std::to_string(port) + "/ws";

    {
        auto result = co_await wsConnect(
            co_await asio::this_coro::executor,
            wsUrl,
            {},
            WsClientConfig{.recvTimeout = std::chrono::milliseconds{300}}
        );
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            auto& client = result.value();
            auto  rr     = co_await client->recv();
            XX_TEST_EXPECT_FALSE(rr.has_value());
            if (!rr.has_value()) {
                XX_TEST_EXPECT_TRUE(rr.error().find("timeout") != std::string::npos);
            }
            co_await client->sendClose();
        }
    }

    server.stop();
    serverThread.join();
}

static asio::awaitable<void> test_ws_empty_message() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});

    server.enableWebSocket(
        "/ws",
        [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
            boost::beast::flat_buffer buf;
            for (;;) {
                buf.clear();
                boost::system::error_code ec;
                co_await ws.async_read(buf, asio::redirect_error(asio::use_awaitable, ec));
                if (ec) co_return;
                ws.text(ws.got_text());
                co_await ws.async_write(buf.data(), asio::redirect_error(asio::use_awaitable, ec));
                if (ec) co_return;
            }
        }
    );

    std::thread serverThread([&server]() { server.start(); });
    uint16_t    port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        g_websocket_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string wsUrl = "ws://127.0.0.1:" + std::to_string(port) + "/ws";

    {
        auto result = co_await wsConnect(co_await asio::this_coro::executor, wsUrl);
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            auto& client = result.value();

            auto sr = co_await client->sendText("");
            XX_TEST_EXPECT_TRUE(sr.has_value());
            auto rr = co_await client->recv();
            XX_TEST_EXPECT_TRUE(rr.has_value());
            if (rr.has_value()) {
                XX_TEST_EXPECT_EQ(rr.value().payload, "");
            }

            co_await client->sendClose();
        }
    }

    server.stop();
    serverThread.join();
}

static asio::awaitable<void> test_ws_unicode() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});

    server.enableWebSocket(
        "/ws",
        [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
            boost::beast::flat_buffer buf;
            for (;;) {
                buf.clear();
                boost::system::error_code ec;
                co_await ws.async_read(buf, asio::redirect_error(asio::use_awaitable, ec));
                if (ec) co_return;
                ws.text(ws.got_text());
                co_await ws.async_write(buf.data(), asio::redirect_error(asio::use_awaitable, ec));
                if (ec) co_return;
            }
        }
    );

    std::thread serverThread([&server]() { server.start(); });
    uint16_t    port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        g_websocket_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string wsUrl = "ws://127.0.0.1:" + std::to_string(port) + "/ws";

    {
        auto result = co_await wsConnect(co_await asio::this_coro::executor, wsUrl);
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            auto& client = result.value();

            std::string unicodeMsg = "你好世界 🌍 héllo wörld";
            auto        sr         = co_await client->sendText(unicodeMsg);
            XX_TEST_EXPECT_TRUE(sr.has_value());
            auto rr = co_await client->recv();
            XX_TEST_EXPECT_TRUE(rr.has_value());
            if (rr.has_value()) {
                XX_TEST_EXPECT_EQ(rr.value().payload, unicodeMsg);
            }

            co_await client->sendClose();
        }
    }

    server.stop();
    serverThread.join();
}

static asio::awaitable<void> test_ws_http_and_ws_coexist() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});

    server.router().add(
        "/api/data",
        0,
        std::make_shared<HttpServer::Handler>(
            [](HttpServer::Request&,
               HttpServer::Response& resp,
               const std::string&) -> asio::awaitable<void> {
                resp.result(boost::beast::http::status::ok);
                resp.set(boost::beast::http::field::content_type, "application/json");
                resp.body() = R"({"status":"ok"})";
                resp.prepare_payload();
                co_return;
            }
        )
    );

    server.enableWebSocket(
        "/ws",
        [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
            boost::beast::flat_buffer buf;
            for (;;) {
                buf.clear();
                boost::system::error_code ec;
                co_await ws.async_read(buf, asio::redirect_error(asio::use_awaitable, ec));
                if (ec) co_return;
                ws.text(true);
                std::string reply = "ws:" + boost::beast::buffers_to_string(buf.data());
                co_await ws.async_write(
                    asio::buffer(reply),
                    asio::redirect_error(asio::use_awaitable, ec)
                );
                if (ec) co_return;
            }
        }
    );

    std::thread serverThread([&server]() { server.start(); });
    uint16_t    port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        g_websocket_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    std::string wsUrl   = "ws://127.0.0.1:" + std::to_string(port) + "/ws";

    {
        auto resp = co_await HttpClient::getAsync(baseUrl + "/api/data");
        XX_TEST_EXPECT_TRUE(resp.has_value());
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
            XX_TEST_EXPECT_EQ(resp.value().body, R"({"status":"ok"})");
        }
    }

    {
        auto result = co_await wsConnect(co_await asio::this_coro::executor, wsUrl);
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            auto& client = result.value();
            co_await client->sendText("test");
            auto rr = co_await client->recv();
            XX_TEST_EXPECT_TRUE(rr.has_value());
            if (rr.has_value()) {
                XX_TEST_EXPECT_EQ(rr.value().payload, "ws:test");
            }
            co_await client->sendClose();
        }
    }

    {
        auto resp = co_await HttpClient::getAsync(baseUrl + "/api/data");
        XX_TEST_EXPECT_TRUE(resp.has_value());
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp.value().status, 200);
        }
    }

    server.stop();
    serverThread.join();
}

static asio::awaitable<void> test_ws_send_after_close() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});

    server.enableWebSocket(
        "/ws",
        [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
            boost::beast::flat_buffer buf;
            boost::system::error_code ec;
            co_await ws.async_read(buf, asio::redirect_error(asio::use_awaitable, ec));
        }
    );

    std::thread serverThread([&server]() { server.start(); });
    uint16_t    port = 0;
    for (int i = 0; i < 100; ++i) {
        port = server.port();
        if (port != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (port == 0) {
        g_websocket_failed++;
        server.stop();
        serverThread.join();
        co_return;
    }

    std::string wsUrl = "ws://127.0.0.1:" + std::to_string(port) + "/ws";

    {
        auto result = co_await wsConnect(co_await asio::this_coro::executor, wsUrl);
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            auto& client = result.value();
            co_await client->sendClose(1000, "done");
            XX_TEST_EXPECT_FALSE(client->isOpen());

            auto sr = co_await client->sendText("after-close");
            XX_TEST_EXPECT_FALSE(sr.has_value());
        }
    }

    server.stop();
    serverThread.join();
}

asio::awaitable<TestResult> run_websocket_tests() {
    std::cout << "  [ws] echo..." << std::endl;
    co_await test_ws_echo();

    std::cout << "  [ws] multiple messages..." << std::endl;
    co_await test_ws_multiple_messages();

    std::cout << "  [ws] binary..." << std::endl;
    co_await test_ws_binary();

    std::cout << "  [ws] large message..." << std::endl;
    co_await test_ws_large_message();

    std::cout << "  [ws] ping/pong..." << std::endl;
    co_await test_ws_ping_pong();

    std::cout << "  [ws] server close..." << std::endl;
    co_await test_ws_server_close();

    std::cout << "  [ws] invalid path..." << std::endl;
    co_await test_ws_invalid_path();

    std::cout << "  [ws] connect refused..." << std::endl;
    co_await test_ws_connect_refused();

    std::cout << "  [ws] invalid url..." << std::endl;
    co_await test_ws_invalid_url();

    std::cout << "  [ws] concurrent clients..." << std::endl;
    co_await test_ws_concurrent_clients();

    std::cout << "  [ws] client disconnect..." << std::endl;
    co_await test_ws_client_disconnect();

    std::cout << "  [ws] startAsync mode..." << std::endl;
    co_await test_ws_start_async_mode();

    std::cout << "  [ws] recv timeout..." << std::endl;
    co_await test_ws_recv_timeout();

    std::cout << "  [ws] empty message..." << std::endl;
    co_await test_ws_empty_message();

    std::cout << "  [ws] unicode..." << std::endl;
    co_await test_ws_unicode();

    std::cout << "  [ws] http+ws coexist..." << std::endl;
    co_await test_ws_http_and_ws_coexist();

    std::cout << "  [ws] send after close..." << std::endl;
    co_await test_ws_send_after_close();

    co_return TestResult{g_websocket_passed, g_websocket_failed};
}

} // namespace test
} // namespace agentxx
