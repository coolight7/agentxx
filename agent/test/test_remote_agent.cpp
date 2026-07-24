#include "test_remote_agent.h"
#include "agentxx/agent/agent_io.h"
#include "agentxx/agent/remote/remote_client_io.h"
#include "agentxx/agent/remote/wire_protocol.h"
#include "agentxx/agent/remote/ws_transport.h"
#include "agentxx/util/http_server.h"
#include "agentxx/util/ws_client.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace agentxx {
namespace test {

using namespace agentxx::util;
namespace remote = agentxx::agent::remote;

int g_remote_passed = 0;
int g_remote_failed = 0;

// ---------------------------------------------------------------------------
// 测试用 IO: 记录收到的 delta
// ---------------------------------------------------------------------------

class TestIO : public agentxx::agent::AgentIOBase {
public:

    std::vector<agentxx::agent::Delta> deltas;
    int                                syncCount = 0;

    void onDelta(const agentxx::agent::Delta& delta) override {
        deltas.push_back(delta);
    }

    void onSync(const agentxx::agent::SyncPayload&) override {
        syncCount++;
    }

    asio::awaitable<std::optional<std::string>> getInput() override {
        co_return std::nullopt;
    }

    asio::awaitable<neograph::json> handleInterrupt(
        const std::string&,
        const std::string&,
        const std::string&,
        const std::string&
    ) override {
        co_return neograph::json::array({"true"});
    }
};

// ---------------------------------------------------------------------------
// WS 收发辅助 (服务端 handler 内使用)
// ---------------------------------------------------------------------------

static asio::awaitable<bool> wsSendJson(HttpServer::WsStream& ws, const neograph::json& j) {
    auto                    s = j.dump();
    ws.text(true);
    boost::system::error_code ec;
    co_await ws.async_write(asio::buffer(s), asio::redirect_error(asio::use_awaitable, ec));
    co_return !ec;
}

static asio::awaitable<std::optional<neograph::json>> wsRecvJson(HttpServer::WsStream& ws) {
    boost::beast::flat_buffer buf;
    boost::system::error_code ec;
    co_await ws.async_read(buf, asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
        co_return std::nullopt;
    }
    try {
        co_return neograph::json::parse(boost::beast::buffers_to_string(buf.data()));
    } catch (const std::exception&) {
        co_return std::nullopt;
    }
}

/// 启动 server 并等待端口就绪; 返回端口 (0 表示失败)
static uint16_t startServerAndWait(HttpServer& server, std::thread& th) {
    th = std::thread([&server]() {
        server.start();
    });
    for (int i = 0; i < 100; ++i) {
        auto port = server.port();
        if (port != 0) {
            return port;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 1. 协议序列化往返
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_protocol_roundtrip() {
    using agentxx::agent::Delta;
    using agentxx::agent::HistoryMessage;
    using agentxx::agent::SyncPayload;

    // Delta 各类型往返
    {
        Delta d;
        d.type     = Delta::Type::TextToken;
        d.seq      = 5;
        d.text     = "abc";
        d.msgId    = "m1";
        auto j     = remote::makeDeltaMsg(d);
        auto back  = remote::deltaMsgFromJson(j);
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            XX_TEST_EXPECT_TRUE(back->type == Delta::Type::TextToken);
            XX_TEST_EXPECT_EQ(back->seq, uint64_t{5});
            XX_TEST_EXPECT_EQ(back->text, std::string("abc"));
            XX_TEST_EXPECT_EQ(back->msgId, std::string("m1"));
        }
    }
    {
        Delta d;
        d.type       = Delta::Type::ToolEnd;
        d.seq        = 9;
        d.toolName   = "bash";
        d.toolCallId = "tc1";
        d.result     = "output";
        d.hasError   = true;
        auto back    = remote::deltaMsgFromJson(remote::makeDeltaMsg(d));
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            XX_TEST_EXPECT_TRUE(back->type == Delta::Type::ToolEnd);
            XX_TEST_EXPECT_EQ(back->toolName, std::string("bash"));
            XX_TEST_EXPECT_EQ(back->toolCallId, std::string("tc1"));
            XX_TEST_EXPECT_TRUE(back->hasError);
        }
    }
    // SyncPayload 往返
    {
        SyncPayload p;
        p.fromIndex = 2;
        p.tailHash  = "hash123";
        p.messages.push_back(HistoryMessage{
            "id1",
            neograph::json{{"role", "user"}, {"content", "hi"}},
        });
        auto back = remote::syncMsgFromJson(remote::makeSyncMsg(p));
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            XX_TEST_EXPECT_EQ(back->fromIndex, uint64_t{2});
            XX_TEST_EXPECT_EQ(back->tailHash, std::string("hash123"));
            XX_TEST_EXPECT_EQ(back->messages.size(), size_t{1});
            if (!back->messages.empty()) {
                XX_TEST_EXPECT_EQ(back->messages[0].id, std::string("id1"));
            }
        }
    }
    // 消息信封 type 字段
    {
        auto hello = remote::makeHello("sess", "tok", 3, "th");
        XX_TEST_EXPECT_EQ(remote::msgType(hello), std::string(remote::MsgType::Hello));
        XX_TEST_EXPECT_EQ(hello.value("token", std::string{}), std::string("tok"));
        XX_TEST_EXPECT_EQ(hello.value("last_seq", uint64_t{0}), uint64_t{3});

        auto ir = remote::makeInterruptRequest(7, "sess", "node", "val", "{}");
        XX_TEST_EXPECT_EQ(remote::msgType(ir), std::string(remote::MsgType::InterruptRequest));
        XX_TEST_EXPECT_EQ(ir.value("id", int64_t{0}), int64_t{7});
    }
    co_return;
}

// ---------------------------------------------------------------------------
// 2. 传输层回环 (ClientWsTransport <-> ServerWsTransport)
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_transport_loopback() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});
    server.enableWebSocket(
        "/echo",
        [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
            for (;;) {
                auto j = co_await wsRecvJson(ws);
                if (!j) {
                    co_return;
                }
                co_await wsSendJson(ws, *j);
            }
        }
    );

    std::thread th;
    uint16_t    port = startServerAndWait(server, th);
    if (port == 0) {
        g_remote_failed++;
        server.stop();
        th.join();
        co_return;
    }

    auto exec = co_await asio::this_coro::executor;
    auto client
        = co_await wsConnect(exec, "ws://127.0.0.1:" + std::to_string(port) + "/echo");
    XX_TEST_EXPECT_TRUE(client.has_value());
    if (client) {
        remote::ClientWsTransport transport(std::move(client.value()));
        auto sendRes = co_await transport.send(R"({"type":"ping","t":42})");
        XX_TEST_EXPECT_TRUE(sendRes.has_value());
        auto recvRes = co_await transport.recv();
        XX_TEST_EXPECT_TRUE(recvRes.has_value());
        if (recvRes) {
            auto j = neograph::json::parse(recvRes->payload);
            XX_TEST_EXPECT_EQ(remote::msgType(j), std::string("ping"));
            XX_TEST_EXPECT_EQ(j.value("t", int64_t{0}), int64_t{42});
        }
        transport.close();
    }

    server.stop();
    th.join();
}

// ---------------------------------------------------------------------------
// 3. 客户端握手 + 一轮对话 ( against 协议级 fake server )
// ---------------------------------------------------------------------------

/// fake server 的 WS handler: hello 鉴权 -> user_input 回 delta+turn_result
static asio::awaitable<void> fakeAgentHandler(HttpServer::WsStream& ws) {
    auto hello = co_await wsRecvJson(ws);
    if (!hello) {
        co_return;
    }
    bool ok = (hello->value("token", std::string{}) == "test-token");
    co_await wsSendJson(
        ws,
        remote::makeHelloAck(ok, hello->value("thread", std::string{}), "", {})
    );
    if (!ok) {
        co_return;
    }
    for (;;) {
        auto j = co_await wsRecvJson(ws);
        if (!j) {
            co_return;
        }
        auto t = remote::msgType(*j);
        if (t == remote::MsgType::UserInput) {
            agentxx::agent::Delta d;
            d.type = agentxx::agent::Delta::Type::TextToken;
            d.seq  = 1;
            d.text = "hello from server";
            co_await wsSendJson(ws, remote::makeDeltaMsg(d));
            co_await wsSendJson(
                ws,
                remote::makeTurnResult(j->value("thread", std::string{}), false, "", false)
            );
        } else if (t == remote::MsgType::Ping) {
            co_await wsSendJson(ws, remote::makePong(j->value("t", int64_t{0})));
        }
    }
}

static asio::awaitable<void> test_remote_client_handshake() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});
    server.enableWebSocket("/agent", fakeAgentHandler);

    std::thread th;
    uint16_t    port = startServerAndWait(server, th);
    if (port == 0) {
        g_remote_failed++;
        server.stop();
        th.join();
        co_return;
    }

    auto        exec = co_await asio::this_coro::executor;
    std::string url  = "ws://127.0.0.1:" + std::to_string(port) + "/agent";
    WsClientConfig wsCfg;
    wsCfg.recvTimeout = std::chrono::seconds{10};

    // ----- 正确 token: 握手成功 + 一轮对话 -----
    {
        auto client = co_await wsConnect(exec, url, {}, wsCfg);
        XX_TEST_EXPECT_TRUE(client.has_value());
        if (client) {
            auto transport = std::make_unique<remote::ClientWsTransport>(std::move(client.value()));
            auto io        = std::make_shared<TestIO>();
            auto remoteIo  = std::make_shared<remote::RemoteClientAgentIO>(
                exec,
                std::move(transport),
                io,
                remote::RemoteClientAgentIO::Config{}
            );
            bool ok = co_await remoteIo->start("session", "test-token");
            XX_TEST_EXPECT_TRUE(ok);

            remoteIo->sendUserInput("session", "hi", true, "");
            bool turnOk = true;
            try {
                auto r = co_await remoteIo->awaitTurnResult();
                XX_TEST_EXPECT_FALSE(r.hasError);
            } catch (const std::exception&) {
                turnOk = false;
            }
            XX_TEST_EXPECT_TRUE(turnOk);
            // delta 在 turn_result 之前送达, 此时应已收到
            XX_TEST_EXPECT_TRUE(!io->deltas.empty());
            if (!io->deltas.empty()) {
                XX_TEST_EXPECT_EQ(io->deltas[0].text, std::string("hello from server"));
            }
            co_await remoteIo->shutdown();
        }
    }

    // ----- 错误 token: 握手失败 -----
    {
        auto client = co_await wsConnect(exec, url, {}, wsCfg);
        XX_TEST_EXPECT_TRUE(client.has_value());
        if (client) {
            auto transport = std::make_unique<remote::ClientWsTransport>(std::move(client.value()));
            auto io        = std::make_shared<TestIO>();
            auto remoteIo  = std::make_shared<remote::RemoteClientAgentIO>(
                exec,
                std::move(transport),
                io,
                remote::RemoteClientAgentIO::Config{}
            );
            bool ok = co_await remoteIo->start("session", "wrong-token");
            XX_TEST_EXPECT_FALSE(ok);
        }
    }

    server.stop();
    th.join();
}

// ---------------------------------------------------------------------------

asio::awaitable<TestResult> run_remote_agent_tests() {
    std::cout << "  [remote] protocol roundtrip..." << std::endl;
    co_await test_remote_protocol_roundtrip();

    std::cout << "  [remote] transport loopback..." << std::endl;
    co_await test_remote_transport_loopback();

    std::cout << "  [remote] client handshake..." << std::endl;
    co_await test_remote_client_handshake();

    co_return TestResult{g_remote_passed, g_remote_failed};
}

} // namespace test
} // namespace agentxx
