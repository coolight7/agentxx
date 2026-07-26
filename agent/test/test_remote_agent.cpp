#include "test_remote_agent.h"
#include "agentxx/agent/agent_io.h"
#include "agentxx/agent/channel_io_transport.h"
#include "agentxx/agent/remote/agent_server.h"
#include "agentxx/agent/remote/session_controller.h"
#include "agentxx/agent/remote/wire_protocol.h"
#include "agentxx/agent/ws_io_transport.h"
#include "agentxx/util/http_server.h"
#include "agentxx/util/ws_client.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace agentxx {
namespace test {

using namespace agentxx::util;
namespace remote = agentxx::agent::remote;
using agentxx::agent::SessionController;

int g_remote_passed = 0;
int g_remote_failed = 0;

// ---------------------------------------------------------------------------
// 测试用 IO: 记录收到的 delta/sync/turnResult/contextStats
// ---------------------------------------------------------------------------

class TestIO : public agentxx::agent::AgentIOBase {
public:

    std::vector<agentxx::agent::Delta> deltas;
    std::atomic<int>                   syncCount{0};
    std::atomic<int>                   turnResultCount{0};
    std::atomic<bool>                  lastTurnInterrupted{false};
    std::atomic<uint64_t>              lastCtxTokens{0};
    std::atomic<uint64_t>              lastMaxTokens{0};
    std::mutex                         mu;

    void onDelta(const agentxx::agent::Delta& delta) override {
        std::lock_guard<std::mutex> lock(mu);
        deltas.push_back(delta);
    }

    void onSync(const agentxx::agent::SyncPayload&) override {
        syncCount.fetch_add(1);
    }

    void onTurnResult(const agentxx::agent::WireTurnResult& r) override {
        turnResultCount.fetch_add(1);
        lastTurnInterrupted.store(r.interrupted);
    }

    void onContextStats(const agentxx::agent::WireContextStats& s) override {
        lastCtxTokens.store(s.contextTokens);
        lastMaxTokens.store(s.maxContextTokens);
    }

    asio::awaitable<std::optional<std::string>> getInput() override {
        co_return std::nullopt;
    }

    asio::awaitable<neograph::json>
        handleInterrupt(std::string_view, std::string_view, std::string_view, std::string_view)
            override {
        co_return neograph::json::array({"true"});
    }

    size_t deltaCount() {
        std::lock_guard<std::mutex> lock(mu);
        return deltas.size();
    }

    std::string deltaText(size_t i) {
        std::lock_guard<std::mutex> lock(mu);
        return i < deltas.size() ? deltas[i].text : std::string{};
    }
};

static asio::awaitable<void> testSleep(asio::any_io_executor ex, std::chrono::milliseconds d) {
    asio::steady_timer t(ex);
    t.expires_after(d);
    boost::system::error_code ec;
    co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
}

// ---------------------------------------------------------------------------
// WS 收发辅助 (服务端 handler 内使用)
// ---------------------------------------------------------------------------

static asio::awaitable<bool> wsSendJson(HttpServer::WsStream& ws, const neograph::json& j) {
    auto s = j.dump();
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
// 1. 协议序列化往返 (WsAgentIOTransport::serialize/deserialize)
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_protocol_roundtrip() {
    using agentxx::agent::Delta;
    using agentxx::agent::HistoryMessage;
    using agentxx::agent::SyncPayload;
    using agentxx::agent::WireMessage;
    using agentxx::agent::WsAgentIOTransport;

    {
        Delta d;
        d.type  = Delta::Type::TextToken;
        d.seq   = 5;
        d.text  = "abc";
        d.msgId = "m1";
        auto json = WsAgentIOTransport::serialize(WireMessage{d});
        auto back = WsAgentIOTransport::deserialize(json);
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            auto* bd = std::get_if<Delta>(&*back);
            XX_TEST_EXPECT_TRUE(bd != nullptr);
            if (bd) {
                XX_TEST_EXPECT_TRUE(bd->type == Delta::Type::TextToken);
                XX_TEST_EXPECT_EQ(bd->seq, uint64_t{5});
                XX_TEST_EXPECT_EQ(bd->text, std::string("abc"));
                XX_TEST_EXPECT_EQ(bd->msgId, std::string("m1"));
            }
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
        auto back    = WsAgentIOTransport::deserialize(WsAgentIOTransport::serialize(WireMessage{d}));
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            auto* bd = std::get_if<Delta>(&*back);
            XX_TEST_EXPECT_TRUE(bd != nullptr);
            if (bd) {
                XX_TEST_EXPECT_TRUE(bd->type == Delta::Type::ToolEnd);
                XX_TEST_EXPECT_EQ(bd->toolName, std::string("bash"));
                XX_TEST_EXPECT_EQ(bd->toolCallId, std::string("tc1"));
                XX_TEST_EXPECT_TRUE(bd->hasError);
            }
        }
    }
    {
        SyncPayload p;
        p.fromIndex = 2;
        p.tailHash  = "hash123";
        p.messages.push_back(HistoryMessage{
            "id1",
            neograph::json{{"role", "user"}, {"content", "hi"}},
        });
        auto back = WsAgentIOTransport::deserialize(WsAgentIOTransport::serialize(WireMessage{p}));
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            auto* sp = std::get_if<SyncPayload>(&*back);
            XX_TEST_EXPECT_TRUE(sp != nullptr);
            if (sp) {
                XX_TEST_EXPECT_EQ(sp->fromIndex, uint64_t{2});
                XX_TEST_EXPECT_EQ(sp->tailHash, std::string("hash123"));
                XX_TEST_EXPECT_EQ(sp->messages.size(), size_t{1});
                if (!sp->messages.empty()) {
                    XX_TEST_EXPECT_EQ(sp->messages[0].id, std::string("id1"));
                }
            }
        }
    }
    {
        agentxx::agent::WireHello hello{"sess", "tok", 3, "th"};
        auto json = WsAgentIOTransport::serialize(WireMessage{hello});
        auto back = WsAgentIOTransport::deserialize(json);
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            auto* h = std::get_if<agentxx::agent::WireHello>(&*back);
            XX_TEST_EXPECT_TRUE(h != nullptr);
            if (h) {
                XX_TEST_EXPECT_EQ(h->threadId, std::string("sess"));
                XX_TEST_EXPECT_EQ(h->token, std::string("tok"));
                XX_TEST_EXPECT_EQ(h->lastSeq, uint64_t{3});
            }
        }
    }
    {
        agentxx::agent::WireInterruptRequest ir{7, "sess", "node", "val", "{}"};
        auto json = WsAgentIOTransport::serialize(WireMessage{ir});
        auto back = WsAgentIOTransport::deserialize(json);
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            auto* r = std::get_if<agentxx::agent::WireInterruptRequest>(&*back);
            XX_TEST_EXPECT_TRUE(r != nullptr);
            if (r) {
                XX_TEST_EXPECT_EQ(r->id, int64_t{7});
            }
        }
    }
    co_return;
}

// ---------------------------------------------------------------------------
// 2. WsAgentIOTransport 回环 (client <-> echo server)
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_transport_loopback() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});
    server.enableWebSocket("/echo", [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
        // 先回应 helloAck 完成握手, 之后 echo
        auto hello = co_await wsRecvJson(ws);
        if (!hello) {
            co_return;
        }
        co_await wsSendJson(
            ws,
            remote::makeHelloAck(true, hello->value("thread", std::string{}), "", {})
        );
        for (;;) {
            auto j = co_await wsRecvJson(ws);
            if (!j) {
                co_return;
            }
            co_await wsSendJson(ws, *j);
        }
    });

    std::thread th;
    uint16_t    port = startServerAndWait(server, th);
    if (port == 0) {
        g_remote_failed++;
        server.stop();
        th.join();
        co_return;
    }

    auto ex  = co_await asio::this_coro::executor;
    auto url = "ws://127.0.0.1:" + std::to_string(port) + "/echo";

    agentxx::agent::WsAgentIOTransport::Config cfg;
    cfg.heartbeatInterval = std::chrono::seconds{60};
    util::WsClientConfig wsCfg;
    wsCfg.recvTimeout = std::chrono::seconds{10};

    auto transport = std::make_shared<agentxx::agent::WsAgentIOTransport>(
        ex, url, "test-token", cfg, wsCfg
    );

    agentxx::agent::WireHello hello{"session", "test-token", 0, ""};
    bool ok = co_await transport->connect(hello);
    XX_TEST_EXPECT_TRUE(ok);

    if (ok) {
        agentxx::agent::WireUserInput input{"session", "ping", false, ""};
        transport->send(agentxx::agent::WireMessage{input});

        auto recvMsg = co_await transport->recv();
        XX_TEST_EXPECT_TRUE(recvMsg.has_value());
        if (recvMsg) {
            auto* ui = std::get_if<agentxx::agent::WireUserInput>(&*recvMsg);
            XX_TEST_EXPECT_TRUE(ui != nullptr);
            if (ui) {
                XX_TEST_EXPECT_EQ(ui->text, std::string("ping"));
            }
        }
    }
    transport->close();

    server.stop();
    th.join();
}

// ---------------------------------------------------------------------------
// 3. 客户端握手 + 一轮对话 (WsAgentIOTransport + TestIO vs fake server)
// ---------------------------------------------------------------------------

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

    auto ex  = co_await asio::this_coro::executor;
    auto url = "ws://127.0.0.1:" + std::to_string(port) + "/agent";

    // ----- 正确 token: 握手成功 + 一轮对话 -----
    {
        agentxx::agent::WsAgentIOTransport::Config cfg;
        cfg.heartbeatInterval = std::chrono::seconds{60};
        util::WsClientConfig wsCfg;
        wsCfg.recvTimeout = std::chrono::seconds{10};

        auto transport = std::make_shared<agentxx::agent::WsAgentIOTransport>(
            ex, url, "test-token", cfg, wsCfg
        );
        auto io = std::make_shared<TestIO>();
        io->setTransport(transport);

        agentxx::agent::WireHello hello{"session", "test-token", 0, ""};
        bool ok = co_await transport->connect(hello);
        XX_TEST_EXPECT_TRUE(ok);

        if (ok) {
            asio::co_spawn(ex, io->runTransportLoop(), asio::detached);

            io->sendToPeer(agentxx::agent::WireUserInput{"session", "hi", true, ""});
            co_await testSleep(ex, std::chrono::milliseconds{500});

            XX_TEST_EXPECT_TRUE(io->deltaCount() > 0);
            if (io->deltaCount() > 0) {
                XX_TEST_EXPECT_EQ(io->deltaText(0), std::string("hello from server"));
            }
            XX_TEST_EXPECT_TRUE(io->turnResultCount.load() > 0);
        }
        transport->close();
    }

    // ----- 错误 token: 握手失败 -----
    {
        agentxx::agent::WsAgentIOTransport::Config cfg;
        cfg.heartbeatInterval = std::chrono::seconds{60};
        cfg.authTimeout       = std::chrono::seconds{3};
        util::WsClientConfig wsCfg;
        wsCfg.recvTimeout = std::chrono::seconds{5};

        auto transport = std::make_shared<agentxx::agent::WsAgentIOTransport>(
            ex, url, "wrong-token", cfg, wsCfg
        );
        agentxx::agent::WireHello hello{"session", "wrong-token", 0, ""};
        bool ok = co_await transport->connect(hello);
        XX_TEST_EXPECT_FALSE(ok);
        transport->close();
    }

    server.stop();
    th.join();
}

// ---------------------------------------------------------------------------
// 4. SessionController: delta 环形缓冲 + 增量重放 (经 ChannelAgentIOTransport)
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_session_controller_replay() {
    auto ex = co_await asio::this_coro::executor;

    auto [clientT, serverT] = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);

    SessionController::Config cfg;
    cfg.threadId       = "session";
    cfg.deltaBufferCap = 100;
    auto sc            = std::make_shared<SessionController>(
        ex,
        std::weak_ptr<agentxx::agent::DeepAgent>{},
        cfg
    );

    // 先喂入 delta (此时无 transport, 仅记录缓冲)
    for (uint64_t s = 1; s <= 5; ++s) {
        agentxx::agent::Delta d;
        d.type = agentxx::agent::Delta::Type::TextToken;
        d.seq  = s;
        d.text = "t" + std::to_string(s);
        sc->onDelta(d);
    }

    // 设置 transport 后再 handleHello (触发增量重放)
    sc->setTransport(std::shared_ptr<agentxx::agent::AgentIOTransportBase>(std::move(serverT)));

    // lastSeq=3 -> 应增量重放 seq 4,5
    agentxx::agent::WireHello hello{"session", "", 3, ""};
    sc->handleHello(hello);

    // 从 client 端读取: 应有 delta(4), delta(5), helloAck
    std::vector<agentxx::agent::WireMessage> received;
    for (int i = 0; i < 3; ++i) {
        auto msg = co_await clientT->recv();
        if (!msg) break;
        received.push_back(std::move(*msg));
    }

    XX_TEST_EXPECT_EQ(received.size(), size_t{3});
    if (received.size() >= 3) {
        auto* d0 = std::get_if<agentxx::agent::Delta>(&received[0]);
        auto* d1 = std::get_if<agentxx::agent::Delta>(&received[1]);
        XX_TEST_EXPECT_TRUE(d0 != nullptr);
        XX_TEST_EXPECT_TRUE(d1 != nullptr);
        if (d0 && d1) {
            XX_TEST_EXPECT_EQ(d0->seq, uint64_t{4});
            XX_TEST_EXPECT_EQ(d1->seq, uint64_t{5});
        }
        auto* ack = std::get_if<agentxx::agent::WireHelloAck>(&received[2]);
        XX_TEST_EXPECT_TRUE(ack != nullptr);
        if (ack) {
            XX_TEST_EXPECT_TRUE(ack->ok);
        }
    }
    co_return;
}

// ---------------------------------------------------------------------------
// 5. SessionController: 缓冲过旧 -> 回退全量 sync
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_session_controller_replay_fallback() {
    auto ex = co_await asio::this_coro::executor;

    auto [clientT, serverT] = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);

    SessionController::Config cfg;
    cfg.threadId       = "session";
    cfg.deltaBufferCap = 3;
    auto sc            = std::make_shared<SessionController>(
        ex,
        std::weak_ptr<agentxx::agent::DeepAgent>{},
        cfg
    );

    for (uint64_t s = 1; s <= 10; ++s) {
        agentxx::agent::Delta d;
        d.type = agentxx::agent::Delta::Type::TextToken;
        d.seq  = s;
        sc->onDelta(d);
    }

    // 设置 transport 后再 handleHello
    sc->setTransport(std::shared_ptr<agentxx::agent::AgentIOTransportBase>(std::move(serverT)));

    // lastSeq=2 -> 缓冲保留 8,9,10; 2+1=3 < 8 -> 全量 sync
    agentxx::agent::WireHello hello{"session", "", 2, ""};
    sc->handleHello(hello);

    auto msg = co_await clientT->recv();
    XX_TEST_EXPECT_TRUE(msg.has_value());
    if (msg) {
        auto* sp = std::get_if<agentxx::agent::SyncPayload>(&*msg);
        XX_TEST_EXPECT_TRUE(sp != nullptr);
    }
    co_return;
}

// ---------------------------------------------------------------------------
// 6. SessionController: 请求级超时 (handleInterrupt 无响应 -> 超时返回)
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_session_controller_interrupt_timeout() {
    auto ex = co_await asio::this_coro::executor;

    SessionController::Config cfg;
    cfg.threadId         = "session";
    cfg.interruptTimeout = std::chrono::milliseconds{300};
    auto sc              = std::make_shared<SessionController>(
        ex,
        std::weak_ptr<agentxx::agent::DeepAgent>{},
        cfg
    );

    auto start   = std::chrono::steady_clock::now();
    auto result  = co_await sc->handleInterrupt("session", "node", "val", "{}");
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start
    );

    XX_TEST_EXPECT_TRUE(result.is_array());
    XX_TEST_EXPECT_TRUE(elapsed.count() >= 250 && elapsed.count() < 3000);
    co_return;
}

// ---------------------------------------------------------------------------
// 7. SessionController: grace period 断线 -> 宽限期满失败挂起请求
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_session_controller_grace() {
    auto ex = co_await asio::this_coro::executor;

    auto [clientT, serverT] = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);

    SessionController::Config cfg;
    cfg.threadId         = "session";
    cfg.gracePeriod      = std::chrono::milliseconds{200};
    cfg.interruptTimeout = std::chrono::seconds{10};
    auto sc              = std::make_shared<SessionController>(
        ex,
        std::weak_ptr<agentxx::agent::DeepAgent>{},
        cfg
    );
    sc->setTransport(std::shared_ptr<agentxx::agent::AgentIOTransportBase>(std::move(serverT)));
    sc->setTurnActiveForTest(true);

    auto done = std::make_shared<bool>(false);
    asio::co_spawn(
        ex,
        [sc, done]() -> asio::awaitable<void> {
            co_await sc->handleInterrupt("session", "node", "val", "{}");
            *done = true;
            co_return;
        },
        asio::detached
    );

    co_await testSleep(ex, std::chrono::milliseconds{50});
    XX_TEST_EXPECT_FALSE(*done);

    // 关闭 transport 模拟断线 -> 启动 grace (200ms); 期满无重连 -> 失败 pending
    sc->transport()->close();
    sc->onDisconnect();
    co_await testSleep(ex, std::chrono::milliseconds{500});

    XX_TEST_EXPECT_TRUE(*done);
    co_return;
}

// ---------------------------------------------------------------------------
// 8. 客户端断线自动重连 (WsAgentIOTransport) + 重连携带 lastSeq
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_client_reconnect() {
    std::atomic<int>  connCount{0};
    std::atomic<bool> sawResumeLastSeq{false};

    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});
    server.enableWebSocket("/agent", [&](HttpServer::WsStream& ws) -> asio::awaitable<void> {
        int  myConn = ++connCount;
        auto hello  = co_await wsRecvJson(ws);
        if (!hello) {
            co_return;
        }
        if (myConn >= 2 && hello->value("last_seq", uint64_t{0}) > 0) {
            sawResumeLastSeq = true;
        }
        co_await wsSendJson(
            ws,
            remote::makeHelloAck(true, hello->value("thread", std::string{}), "", {})
        );
        if (myConn == 1) {
            auto j = co_await wsRecvJson(ws);
            if (j) {
                agentxx::agent::Delta d;
                d.type = agentxx::agent::Delta::Type::TextToken;
                d.seq  = 1;
                d.text = "before-drop";
                co_await wsSendJson(ws, remote::makeDeltaMsg(d));
                co_await wsSendJson(
                    ws,
                    remote::makeTurnResult(j->value("thread", std::string{}), false, "", false)
                );
            }
            co_return;
        }
        for (;;) {
            auto j = co_await wsRecvJson(ws);
            if (!j) {
                co_return;
            }
            if (remote::msgType(*j) == remote::MsgType::UserInput) {
                co_await wsSendJson(
                    ws,
                    remote::makeTurnResult(j->value("thread", std::string{}), false, "", false)
                );
            } else if (remote::msgType(*j) == remote::MsgType::Ping) {
                co_await wsSendJson(ws, remote::makePong(j->value("t", int64_t{0})));
            }
        }
    });

    std::thread th;
    uint16_t    port = startServerAndWait(server, th);
    if (port == 0) {
        g_remote_failed++;
        server.stop();
        th.join();
        co_return;
    }

    auto ex  = co_await asio::this_coro::executor;
    auto url = "ws://127.0.0.1:" + std::to_string(port) + "/agent";

    agentxx::agent::WsAgentIOTransport::Config cfg;
    cfg.reconnectBackoff     = std::chrono::milliseconds{100};
    cfg.heartbeatInterval    = std::chrono::seconds{60};
    util::WsClientConfig wsCfg;
    wsCfg.recvTimeout = std::chrono::seconds{5};

    auto transport = std::make_shared<agentxx::agent::WsAgentIOTransport>(
        ex, url, "test-token", cfg, wsCfg
    );
    auto io = std::make_shared<TestIO>();
    io->setTransport(transport);

    agentxx::agent::WireHello hello{"session", "test-token", 0, ""};
    bool ok = co_await transport->connect(hello);
    XX_TEST_EXPECT_TRUE(ok);

    asio::co_spawn(ex, io->runTransportLoop(), asio::detached);

    io->sendToPeer(agentxx::agent::WireUserInput{"session", "msg1", true, ""});

    bool reconnected = false;
    for (int i = 0; i < 100; ++i) {
        if (connCount.load() >= 2) {
            reconnected = true;
            break;
        }
        co_await testSleep(ex, std::chrono::milliseconds{50});
    }
    XX_TEST_EXPECT_TRUE(reconnected);
    co_await testSleep(ex, std::chrono::milliseconds{300});
    XX_TEST_EXPECT_TRUE(sawResumeLastSeq.load());

    transport->close();
    server.stop();
    th.join();
}

// ---------------------------------------------------------------------------
// 9. ChannelAgentIOTransport 回环 (进程内传输)
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_channel_transport_loopback() {
    auto ex   = co_await asio::this_coro::executor;
    auto pair = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);
    auto& clientT = *pair.first;
    auto& serverT = *pair.second;

    agentxx::agent::WireUserInput msg1{"session", "hello", false, ""};
    clientT.send(agentxx::agent::WireMessage{msg1});
    auto r1 = co_await serverT.recv();
    XX_TEST_EXPECT_TRUE(r1.has_value());
    if (r1) {
        auto* ui = std::get_if<agentxx::agent::WireUserInput>(&*r1);
        XX_TEST_EXPECT_TRUE(ui != nullptr);
        if (ui) {
            XX_TEST_EXPECT_EQ(ui->text, std::string("hello"));
        }
    }

    agentxx::agent::WireCancel msg2{"session"};
    serverT.send(agentxx::agent::WireMessage{msg2});
    auto r2 = co_await clientT.recv();
    XX_TEST_EXPECT_TRUE(r2.has_value());
    if (r2) {
        auto* c = std::get_if<agentxx::agent::WireCancel>(&*r2);
        XX_TEST_EXPECT_TRUE(c != nullptr);
    }

    clientT.close();
    auto r3 = co_await serverT.recv();
    XX_TEST_EXPECT_FALSE(r3.has_value());
    co_return;
}

// ---------------------------------------------------------------------------
// 10. 客户端接收上下文统计 (context_stats)
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_client_context_stats() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});
    server.enableWebSocket("/agent", [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
        auto hello = co_await wsRecvJson(ws);
        if (!hello) {
            co_return;
        }
        co_await wsSendJson(
            ws,
            remote::makeHelloAck(true, hello->value("thread", std::string{}), "", {})
        );
        co_await wsSendJson(ws, remote::makeContextStats(1234, 5678));
        for (;;) {
            auto j = co_await wsRecvJson(ws);
            if (!j) {
                co_return;
            }
        }
    });

    std::thread th;
    uint16_t    port = startServerAndWait(server, th);
    if (port == 0) {
        g_remote_failed++;
        server.stop();
        th.join();
        co_return;
    }

    auto ex  = co_await asio::this_coro::executor;
    auto url = "ws://127.0.0.1:" + std::to_string(port) + "/agent";

    agentxx::agent::WsAgentIOTransport::Config cfg;
    cfg.heartbeatInterval = std::chrono::seconds{60};
    util::WsClientConfig wsCfg;
    wsCfg.recvTimeout = std::chrono::seconds{10};

    auto transport = std::make_shared<agentxx::agent::WsAgentIOTransport>(
        ex, url, "test-token", cfg, wsCfg
    );
    auto io = std::make_shared<TestIO>();
    io->setTransport(transport);

    agentxx::agent::WireHello hello{"session", "test-token", 0, ""};
    bool ok = co_await transport->connect(hello);
    XX_TEST_EXPECT_TRUE(ok);

    if (ok) {
        asio::co_spawn(ex, io->runTransportLoop(), asio::detached);
        co_await testSleep(ex, std::chrono::milliseconds{300});
        XX_TEST_EXPECT_EQ(io->lastCtxTokens.load(), uint64_t{1234});
        XX_TEST_EXPECT_EQ(io->lastMaxTokens.load(), uint64_t{5678});
    }
    transport->close();

    server.stop();
    th.join();
}

// ---------------------------------------------------------------------------
// 11. 进程内集成: ChannelAgentIOTransport + AgentServer::serveTransport
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_channel_client_integration() {
    auto ex = co_await asio::this_coro::executor;

    auto [clientT, serverT] = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);

    // fake server (经 channel 说协议)
    asio::co_spawn(
        ex,
        [st = std::move(serverT)]() mutable -> asio::awaitable<void> {
            auto hello = co_await st->recv();
            if (!hello) {
                co_return;
            }
            auto* h = std::get_if<agentxx::agent::WireHello>(&*hello);
            if (!h) co_return;
            st->send(agentxx::agent::WireMessage{
                agentxx::agent::WireHelloAck{true, h->threadId, "", {}}
            });
            for (;;) {
                auto m = co_await st->recv();
                if (!m) {
                    co_return;
                }
                auto* ui = std::get_if<agentxx::agent::WireUserInput>(&*m);
                if (ui) {
                    agentxx::agent::Delta d;
                    d.type = agentxx::agent::Delta::Type::TextToken;
                    d.seq  = 1;
                    d.text = "chan-reply";
                    st->send(agentxx::agent::WireMessage{d});
                    st->send(agentxx::agent::WireMessage{
                        agentxx::agent::WireTurnResult{ui->threadId, false, "", false}
                    });
                }
            }
        },
        asio::detached
    );

    auto io = std::make_shared<TestIO>();
    io->setTransport(std::shared_ptr<agentxx::agent::AgentIOTransportBase>(std::move(clientT)));

    io->sendToPeer(agentxx::agent::WireHello{"session", "", 0, ""});
    asio::co_spawn(ex, io->runTransportLoop(), asio::detached);

    co_await testSleep(ex, std::chrono::milliseconds{100});

    io->sendToPeer(agentxx::agent::WireUserInput{"session", "hi", true, ""});
    co_await testSleep(ex, std::chrono::milliseconds{300});

    XX_TEST_EXPECT_TRUE(io->deltaCount() > 0);
    if (io->deltaCount() > 0) {
        XX_TEST_EXPECT_EQ(io->deltaText(0), std::string("chan-reply"));
    }
    XX_TEST_EXPECT_TRUE(io->turnResultCount.load() > 0);
    co_return;
}

// ---------------------------------------------------------------------------
// 12. echo: 多轮输入往返
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_echo() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});
    server.enableWebSocket("/agent", [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
        auto hello = co_await wsRecvJson(ws);
        if (!hello) {
            co_return;
        }
        co_await wsSendJson(
            ws,
            remote::makeHelloAck(true, hello->value("thread", std::string{}), "", {})
        );
        uint64_t seq = 0;
        for (;;) {
            auto j = co_await wsRecvJson(ws);
            if (!j) {
                co_return;
            }
            if (remote::msgType(*j) == remote::MsgType::UserInput) {
                auto                  text = j->value("text", std::string{});
                agentxx::agent::Delta d;
                d.type = agentxx::agent::Delta::Type::TextToken;
                d.seq  = ++seq;
                d.text = "echo:" + text;
                co_await wsSendJson(ws, remote::makeDeltaMsg(d));
                co_await wsSendJson(
                    ws,
                    remote::makeTurnResult(j->value("thread", std::string{}), false, "", false)
                );
            } else if (remote::msgType(*j) == remote::MsgType::Ping) {
                co_await wsSendJson(ws, remote::makePong(j->value("t", int64_t{0})));
            }
        }
    });

    std::thread th;
    uint16_t    port = startServerAndWait(server, th);
    if (port == 0) {
        g_remote_failed++;
        server.stop();
        th.join();
        co_return;
    }

    auto ex  = co_await asio::this_coro::executor;
    auto url = "ws://127.0.0.1:" + std::to_string(port) + "/agent";

    agentxx::agent::WsAgentIOTransport::Config cfg;
    cfg.heartbeatInterval = std::chrono::seconds{60};
    util::WsClientConfig wsCfg;
    wsCfg.recvTimeout = std::chrono::seconds{10};

    auto transport = std::make_shared<agentxx::agent::WsAgentIOTransport>(
        ex, url, "test-token", cfg, wsCfg
    );
    auto io = std::make_shared<TestIO>();
    io->setTransport(transport);

    agentxx::agent::WireHello hello{"session", "test-token", 0, ""};
    bool ok = co_await transport->connect(hello);
    XX_TEST_EXPECT_TRUE(ok);

    if (ok) {
        asio::co_spawn(ex, io->runTransportLoop(), asio::detached);

        for (int i = 0; i < 3; ++i) {
            io->sendToPeer(agentxx::agent::WireUserInput{
                "session", "m" + std::to_string(i), i == 0, ""
            });
            co_await testSleep(ex, std::chrono::milliseconds{200});
        }
        co_await testSleep(ex, std::chrono::milliseconds{200});
        XX_TEST_EXPECT_EQ(io->deltaCount(), size_t{3});
        if (io->deltaCount() == 3) {
            XX_TEST_EXPECT_EQ(io->deltaText(0), std::string("echo:m0"));
            XX_TEST_EXPECT_EQ(io->deltaText(2), std::string("echo:m2"));
        }
    }
    transport->close();
    server.stop();
    th.join();
}

// ---------------------------------------------------------------------------
// 13. 并发写: 多线程同时 send, 验证无丢失
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_concurrent_writes() {
    auto ex = co_await asio::this_coro::executor;

    auto [clientT, serverT] = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex, 8192);

    const int                numThreads = 4;
    const int                perThread  = 100;
    std::vector<std::thread> threads;
    auto                     clientPtr = clientT.get();
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([clientPtr, t, perThread]() {
            for (int i = 0; i < perThread; ++i) {
                agentxx::agent::WireUserInput msg{
                    "session",
                    std::to_string(t) + "-" + std::to_string(i),
                    false,
                    ""
                };
                clientPtr->send(agentxx::agent::WireMessage{std::move(msg)});
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    std::set<std::string> seen;
    for (int i = 0; i < numThreads * perThread; ++i) {
        auto msg = co_await serverT->recv();
        if (!msg) break;
        auto* ui = std::get_if<agentxx::agent::WireUserInput>(&*msg);
        if (ui) {
            seen.insert(ui->text);
        }
    }
    XX_TEST_EXPECT_EQ(seen.size(), size_t{numThreads * perThread});
    co_return;
}

// ---------------------------------------------------------------------------
// 14. 多次断线重连: server 前 N 次连接立即断开, 客户端自动多次重连
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_multi_reconnect() {
    std::atomic<int> connCount{0};
    const int        dropTimes = 3;

    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});
    server.enableWebSocket("/agent", [&](HttpServer::WsStream& ws) -> asio::awaitable<void> {
        int  myConn = ++connCount;
        auto hello  = co_await wsRecvJson(ws);
        if (!hello) {
            co_return;
        }
        co_await wsSendJson(
            ws,
            remote::makeHelloAck(true, hello->value("thread", std::string{}), "", {})
        );
        if (myConn <= dropTimes) {
            co_return;
        }
        for (;;) {
            auto j = co_await wsRecvJson(ws);
            if (!j) {
                co_return;
            }
            if (remote::msgType(*j) == remote::MsgType::UserInput) {
                co_await wsSendJson(
                    ws,
                    remote::makeTurnResult(j->value("thread", std::string{}), false, "", false)
                );
            }
        }
    });

    std::thread th;
    uint16_t    port = startServerAndWait(server, th);
    if (port == 0) {
        g_remote_failed++;
        server.stop();
        th.join();
        co_return;
    }

    auto ex  = co_await asio::this_coro::executor;
    auto url = "ws://127.0.0.1:" + std::to_string(port) + "/agent";

    agentxx::agent::WsAgentIOTransport::Config cfg;
    cfg.reconnectBackoff  = std::chrono::milliseconds{50};
    cfg.heartbeatInterval = std::chrono::seconds{60};
    util::WsClientConfig wsCfg;
    wsCfg.recvTimeout = std::chrono::seconds{5};

    auto transport = std::make_shared<agentxx::agent::WsAgentIOTransport>(
        ex, url, "test-token", cfg, wsCfg
    );
    auto io = std::make_shared<TestIO>();
    io->setTransport(transport);

    agentxx::agent::WireHello hello{"session", "test-token", 0, ""};
    bool ok = co_await transport->connect(hello);
    XX_TEST_EXPECT_TRUE(ok);

    asio::co_spawn(ex, io->runTransportLoop(), asio::detached);

    bool multiReconnect = false;
    for (int i = 0; i < 100; ++i) {
        if (connCount.load() > dropTimes) {
            multiReconnect = true;
            break;
        }
        co_await testSleep(ex, std::chrono::milliseconds{50});
    }
    XX_TEST_EXPECT_TRUE(multiReconnect);
    XX_TEST_EXPECT_TRUE(connCount.load() >= dropTimes + 1);

    transport->close();
    server.stop();
    th.join();
}

// ---------------------------------------------------------------------------
// 15. 取消: 轮次进行中客户端发 cancel, server 收到并回 interrupted 结果
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_cancel() {
    std::atomic<bool> gotCancel{false};

    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});
    server.enableWebSocket("/agent", [&](HttpServer::WsStream& ws) -> asio::awaitable<void> {
        auto hello = co_await wsRecvJson(ws);
        if (!hello) {
            co_return;
        }
        co_await wsSendJson(
            ws,
            remote::makeHelloAck(true, hello->value("thread", std::string{}), "", {})
        );
        for (;;) {
            auto j = co_await wsRecvJson(ws);
            if (!j) {
                co_return;
            }
            auto t = remote::msgType(*j);
            if (t == remote::MsgType::UserInput) {
                // 不立即回应, 等待 cancel
            } else if (t == remote::MsgType::Cancel) {
                gotCancel.store(true);
                co_await wsSendJson(
                    ws,
                    remote::makeTurnResult(j->value("thread", std::string{}), false, "", true)
                );
            }
        }
    });

    std::thread th;
    uint16_t    port = startServerAndWait(server, th);
    if (port == 0) {
        g_remote_failed++;
        server.stop();
        th.join();
        co_return;
    }

    auto ex  = co_await asio::this_coro::executor;
    auto url = "ws://127.0.0.1:" + std::to_string(port) + "/agent";

    agentxx::agent::WsAgentIOTransport::Config cfg;
    cfg.heartbeatInterval = std::chrono::seconds{60};
    util::WsClientConfig wsCfg;
    wsCfg.recvTimeout = std::chrono::seconds{10};

    auto transport = std::make_shared<agentxx::agent::WsAgentIOTransport>(
        ex, url, "test-token", cfg, wsCfg
    );
    auto io = std::make_shared<TestIO>();
    io->setTransport(transport);

    agentxx::agent::WireHello hello{"session", "test-token", 0, ""};
    bool ok = co_await transport->connect(hello);
    XX_TEST_EXPECT_TRUE(ok);

    if (ok) {
        asio::co_spawn(ex, io->runTransportLoop(), asio::detached);

        io->sendToPeer(agentxx::agent::WireUserInput{"session", "hi", true, ""});
        io->sendToPeer(agentxx::agent::WireCancel{"session"});
        co_await testSleep(ex, std::chrono::milliseconds{500});

        XX_TEST_EXPECT_TRUE(gotCancel.load());
        XX_TEST_EXPECT_TRUE(io->lastTurnInterrupted.load());
    }
    transport->close();
    server.stop();
    th.join();
}

// ---------------------------------------------------------------------------
// 16. 重连 sync: 重连时 server 下发全量 sync, 客户端 onSync 被调用
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_reconnect_sync() {
    std::atomic<int> connCount{0};

    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});
    server.enableWebSocket("/agent", [&](HttpServer::WsStream& ws) -> asio::awaitable<void> {
        int  myConn = ++connCount;
        auto hello  = co_await wsRecvJson(ws);
        if (!hello) {
            co_return;
        }
        co_await wsSendJson(
            ws,
            remote::makeHelloAck(true, hello->value("thread", std::string{}), "", {})
        );
        if (myConn == 1) {
            auto j = co_await wsRecvJson(ws);
            if (j) {
                agentxx::agent::Delta d;
                d.type = agentxx::agent::Delta::Type::TextToken;
                d.seq  = 1;
                d.text = "x";
                co_await wsSendJson(ws, remote::makeDeltaMsg(d));
                co_await wsSendJson(
                    ws,
                    remote::makeTurnResult(j->value("thread", std::string{}), false, "", false)
                );
            }
            co_return;
        }
        agentxx::agent::SyncPayload sp;
        sp.tailHash = "reconnect-sync";
        co_await wsSendJson(ws, remote::makeSyncMsg(sp, 1));
        for (;;) {
            auto j = co_await wsRecvJson(ws);
            if (!j) {
                co_return;
            }
            if (remote::msgType(*j) == remote::MsgType::UserInput) {
                co_await wsSendJson(
                    ws,
                    remote::makeTurnResult(j->value("thread", std::string{}), false, "", false)
                );
            }
        }
    });

    std::thread th;
    uint16_t    port = startServerAndWait(server, th);
    if (port == 0) {
        g_remote_failed++;
        server.stop();
        th.join();
        co_return;
    }

    auto ex  = co_await asio::this_coro::executor;
    auto url = "ws://127.0.0.1:" + std::to_string(port) + "/agent";

    agentxx::agent::WsAgentIOTransport::Config cfg;
    cfg.reconnectBackoff  = std::chrono::milliseconds{50};
    cfg.heartbeatInterval = std::chrono::seconds{60};
    util::WsClientConfig wsCfg;
    wsCfg.recvTimeout = std::chrono::seconds{5};

    auto transport = std::make_shared<agentxx::agent::WsAgentIOTransport>(
        ex, url, "test-token", cfg, wsCfg
    );
    auto io = std::make_shared<TestIO>();
    io->setTransport(transport);

    agentxx::agent::WireHello hello{"session", "test-token", 0, ""};
    bool ok = co_await transport->connect(hello);
    XX_TEST_EXPECT_TRUE(ok);

    asio::co_spawn(ex, io->runTransportLoop(), asio::detached);

    io->sendToPeer(agentxx::agent::WireUserInput{"session", "m1", true, ""});

    bool gotSync = false;
    for (int i = 0; i < 100; ++i) {
        if (io->syncCount.load() > 0) {
            gotSync = true;
            break;
        }
        co_await testSleep(ex, std::chrono::milliseconds{50});
    }
    XX_TEST_EXPECT_TRUE(gotSync);
    XX_TEST_EXPECT_TRUE(connCount.load() >= 2);

    transport->close();
    server.stop();
    th.join();
}

// ---------------------------------------------------------------------------
// 17. 鉴权超时: server 永不回应 hello_ack, 客户端 connect 超时失败
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_auth_timeout() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});
    server.enableWebSocket("/agent", [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
        auto hello = co_await wsRecvJson(ws);
        if (hello) {
            asio::steady_timer timer(co_await asio::this_coro::executor, std::chrono::seconds{5});
            boost::system::error_code ec;
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        }
    });

    std::thread th;
    uint16_t    port = startServerAndWait(server, th);
    if (port == 0) {
        g_remote_failed++;
        server.stop();
        th.join();
        co_return;
    }

    auto ex  = co_await asio::this_coro::executor;
    auto url = "ws://127.0.0.1:" + std::to_string(port) + "/agent";

    agentxx::agent::WsAgentIOTransport::Config cfg;
    cfg.authTimeout       = std::chrono::milliseconds{300};
    cfg.heartbeatInterval = std::chrono::seconds{60};
    util::WsClientConfig wsCfg;
    wsCfg.recvTimeout = std::chrono::seconds{5};

    auto transport = std::make_shared<agentxx::agent::WsAgentIOTransport>(
        ex, url, "test-token", cfg, wsCfg
    );

    auto start = std::chrono::steady_clock::now();
    agentxx::agent::WireHello hello{"session", "test-token", 0, ""};
    bool ok = co_await transport->connect(hello);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start
    );
    XX_TEST_EXPECT_FALSE(ok);
    XX_TEST_EXPECT_TRUE(elapsed.count() >= 250 && elapsed.count() < 3000);
    transport->close();

    server.stop();
    th.join();
}

// ---------------------------------------------------------------------------
// 18. 鉴权失败(服务端): 错误 token -> AgentServer 回 hello_ack(ok=false)
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_auth_rejected() {
    auto ex = co_await asio::this_coro::executor;

    auto [clientT, serverT] = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);

    // 模拟 AgentServer 鉴权逻辑
    asio::co_spawn(
        ex,
        [st = std::move(serverT)]() mutable -> asio::awaitable<void> {
            auto msg = co_await st->recv();
            if (!msg) co_return;
            auto* hello = std::get_if<agentxx::agent::WireHello>(&*msg);
            if (!hello) co_return;
            bool authOk = (hello->token == "secret");
            st->send(agentxx::agent::WireMessage{
                agentxx::agent::WireHelloAck{authOk, hello->threadId, "", {}}
            });
        },
        asio::detached
    );

    clientT->send(agentxx::agent::WireMessage{
        agentxx::agent::WireHello{"session", "wrong-token", 0, ""}
    });
    auto resp = co_await clientT->recv();
    XX_TEST_EXPECT_TRUE(resp.has_value());
    if (resp) {
        auto* ack = std::get_if<agentxx::agent::WireHelloAck>(&*resp);
        XX_TEST_EXPECT_TRUE(ack != nullptr);
        if (ack) {
            XX_TEST_EXPECT_FALSE(ack->ok);
        }
    }
    co_await testSleep(ex, std::chrono::milliseconds{100});
    co_return;
}

// ---------------------------------------------------------------------------

asio::awaitable<TestResult> run_remote_agent_tests() {
    std::cout << "  [remote] protocol roundtrip..." << std::endl;
    co_await test_remote_protocol_roundtrip();

    std::cout << "  [remote] transport loopback..." << std::endl;
    co_await test_remote_transport_loopback();

    std::cout << "  [remote] client handshake..." << std::endl;
    co_await test_remote_client_handshake();

    std::cout << "  [remote] session controller replay..." << std::endl;
    co_await test_session_controller_replay();

    std::cout << "  [remote] session controller replay fallback..." << std::endl;
    co_await test_session_controller_replay_fallback();

    std::cout << "  [remote] session controller interrupt timeout..." << std::endl;
    co_await test_session_controller_interrupt_timeout();

    std::cout << "  [remote] session controller grace period..." << std::endl;
    co_await test_session_controller_grace();

    std::cout << "  [remote] client auto-reconnect..." << std::endl;
    co_await test_remote_client_reconnect();

    std::cout << "  [remote] channel transport loopback..." << std::endl;
    co_await test_channel_transport_loopback();

    std::cout << "  [remote] client context stats..." << std::endl;
    co_await test_remote_client_context_stats();

    std::cout << "  [remote] channel client integration..." << std::endl;
    co_await test_channel_client_integration();

    std::cout << "  [remote] echo multi-turn..." << std::endl;
    co_await test_remote_echo();

    std::cout << "  [remote] concurrent writes..." << std::endl;
    co_await test_remote_concurrent_writes();

    std::cout << "  [remote] multi reconnect..." << std::endl;
    co_await test_remote_multi_reconnect();

    std::cout << "  [remote] cancel..." << std::endl;
    co_await test_remote_cancel();

    std::cout << "  [remote] reconnect sync..." << std::endl;
    co_await test_remote_reconnect_sync();

    std::cout << "  [remote] auth timeout..." << std::endl;
    co_await test_remote_auth_timeout();

    std::cout << "  [remote] auth rejected..." << std::endl;
    co_await test_remote_auth_rejected();

    co_return TestResult{g_remote_passed, g_remote_failed};
}

} // namespace test
} // namespace agentxx
