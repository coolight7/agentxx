#include "test_remote_agent.h"
#include "agentxx/agent/agent_io.h"
#include "agentxx/agent/remote/channel_transport.h"
#include "agentxx/agent/remote/remote_client_io.h"
#include "agentxx/agent/remote/remote_server_io.h"
#include "agentxx/agent/remote/session_controller.h"
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
// 测试用连接下沉: 记录 SessionController 推送的消息
// ---------------------------------------------------------------------------

class MockSink : public remote::IConnectionSink {
public:

    std::vector<neograph::json> messages;
    std::mutex                  mu;
    bool                        isAlive = true;

    void pushMessage(neograph::json msg) override {
        std::lock_guard<std::mutex> lock(mu);
        messages.push_back(std::move(msg));
    }

    bool alive() const noexcept override {
        return isAlive;
    }

    std::vector<neograph::json> snapshot() {
        std::lock_guard<std::mutex> lock(mu);
        return messages;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu);
        messages.clear();
    }
};

/// 测试用可定时返回若干输入、随后 nullopt 的 IO
class ScriptedIO : public agentxx::agent::AgentIOBase {
public:

    std::vector<std::string> inputs;
    std::atomic<size_t>      idx{0};
    std::vector<agentxx::agent::Delta> deltas;
    std::atomic<int>         syncCount{0};
    std::mutex               dmu;

    void onDelta(const agentxx::agent::Delta& d) override {
        std::lock_guard<std::mutex> lock(dmu);
        deltas.push_back(d);
    }
    void onSync(const agentxx::agent::SyncPayload&) override {
        syncCount.fetch_add(1);
    }

    asio::awaitable<std::optional<std::string>> getInput() override {
        size_t i = idx.fetch_add(1);
        if (i < inputs.size()) {
            co_return inputs[i];
        }
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
// 4. SessionController: delta 环形缓冲 + 增量重放
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_session_controller_replay() {
    auto ex = co_await asio::this_coro::executor;

    remote::SessionController::Config cfg;
    cfg.threadId       = "session";
    cfg.deltaBufferCap = 100;
    auto sc = std::make_shared<remote::SessionController>(
        ex,
        std::weak_ptr<agentxx::agent::DeepAgent>{},
        cfg
    );

    // 喂入 seq 1..5 的 delta (此时无活动连接, onDelta 仅记录缓冲)
    for (uint64_t s = 1; s <= 5; ++s) {
        agentxx::agent::Delta d;
        d.type = agentxx::agent::Delta::Type::TextToken;
        d.seq  = s;
        d.text = "t" + std::to_string(s);
        sc->onDelta(d);
    }

    auto mock = std::make_shared<MockSink>();
    // lastSeq=3 -> 应增量重放 seq 4,5
    sc->attach(mock, 3, "");

    auto msgs = mock->snapshot();
    XX_TEST_EXPECT_EQ(msgs.size(), size_t{2});
    if (msgs.size() == 2) {
        XX_TEST_EXPECT_EQ(msgs[0].value("seq", uint64_t{0}), uint64_t{4});
        XX_TEST_EXPECT_EQ(msgs[1].value("seq", uint64_t{0}), uint64_t{5});
        XX_TEST_EXPECT_EQ(msgs[0].value("type", std::string{}), std::string(remote::MsgType::DeltaMsg));
    }
    co_return;
}

// ---------------------------------------------------------------------------
// 5. SessionController: 缓冲过旧 -> 回退全量 sync
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_session_controller_replay_fallback() {
    auto ex = co_await asio::this_coro::executor;

    remote::SessionController::Config cfg;
    cfg.threadId       = "session";
    cfg.deltaBufferCap = 3; // 仅保留最近 3 条
    auto sc = std::make_shared<remote::SessionController>(
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
    // 缓冲保留 seq 8,9,10; lastSeq=2 -> 2+1=3 < 8 -> 缓冲不覆盖 -> 全量 sync
    auto mock = std::make_shared<MockSink>();
    sc->attach(mock, 2, "");

    auto msgs = mock->snapshot();
    XX_TEST_EXPECT_EQ(msgs.size(), size_t{1});
    if (!msgs.empty()) {
        XX_TEST_EXPECT_EQ(msgs[0].value("type", std::string{}), std::string(remote::MsgType::SyncMsg));
    }
    co_return;
}

// ---------------------------------------------------------------------------
// 6. SessionController: 请求级超时 (handleInterrupt 无响应 -> 超时返回)
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_session_controller_interrupt_timeout() {
    auto ex = co_await asio::this_coro::executor;

    remote::SessionController::Config cfg;
    cfg.threadId         = "session";
    cfg.interruptTimeout = std::chrono::milliseconds{300};
    auto sc = std::make_shared<remote::SessionController>(
        ex,
        std::weak_ptr<agentxx::agent::DeepAgent>{},
        cfg
    );

    auto start = std::chrono::steady_clock::now();
    auto result = co_await sc->handleInterrupt("session", "node", "val", "{}");
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

    remote::SessionController::Config cfg;
    cfg.threadId         = "session";
    cfg.gracePeriod      = std::chrono::milliseconds{200};
    cfg.interruptTimeout = std::chrono::seconds{10}; // 远大于 grace
    auto sc = std::make_shared<remote::SessionController>(
        ex,
        std::weak_ptr<agentxx::agent::DeepAgent>{},
        cfg
    );

    auto mock = std::make_shared<MockSink>();
    sc->attach(mock, 0, "");
    sc->setTurnActiveForTest(true);

    // 发起一个 interrupt (注册 pending 并等待响应)
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

    // 等待 interrupt 注册 pending
    co_await testSleep(ex, std::chrono::milliseconds{50});
    XX_TEST_EXPECT_FALSE(*done); // 尚未超时

    // 断线 -> 启动 grace (200ms); 期满无重连 -> 失败 pending -> interrupt 提前返回
    sc->detach(mock.get());
    co_await testSleep(ex, std::chrono::milliseconds{500});

    XX_TEST_EXPECT_TRUE(*done); // grace 期满失败 pending, interrupt 提前返回 (远早于 10s)
    co_return;
}

// ---------------------------------------------------------------------------
// 8. 客户端断线自动重连 (runSession) + 重连携带 lastSeq
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_client_reconnect() {
    std::atomic<int>  connCount{0};
    std::atomic<bool> sawResumeLastSeq{false};

    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});
    server.enableWebSocket(
        "/agent",
        [&](HttpServer::WsStream& ws) -> asio::awaitable<void> {
            int myConn = ++connCount;
            auto hello = co_await wsRecvJson(ws);
            if (!hello) {
                co_return;
            }
            // 第二次及以后的连接应携带 last_seq>0 (客户端断线重连恢复)
            if (myConn >= 2 && hello->value("last_seq", uint64_t{0}) > 0) {
                sawResumeLastSeq = true;
            }
            co_await wsSendJson(
                ws,
                remote::makeHelloAck(true, hello->value("thread", std::string{}), "", {})
            );
            if (myConn == 1) {
                // 首个连接: 发一个 delta(seq=1)+turn_result 后立即断开, 触发客户端重连
                auto j = co_await wsRecvJson(ws); // user_input
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
                co_return; // 关闭连接
            }
            // 后续连接: 正常回应轮次
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

    auto ex = co_await asio::this_coro::executor;

    auto io      = std::make_shared<ScriptedIO>();
    io->inputs   = {"msg1", "msg2", "msg3", "msg4"};

    remote::RemoteClientAgentIO::Config cfg;
    cfg.reconnectBackoff = std::chrono::milliseconds{100};
    WsClientConfig wsCfg;
    wsCfg.recvTimeout = std::chrono::seconds{5};

    auto remote = std::make_shared<remote::RemoteClientAgentIO>(
        ex,
        io,
        "ws://127.0.0.1:" + std::to_string(port) + "/agent",
        "test-token",
        cfg,
        wsCfg
    );

    // 后台运行 runSession (输入耗尽后自行退出)
    asio::co_spawn(
        ex,
        [remote]() -> asio::awaitable<void> {
            co_await remote->runSession("session", "");
            co_return;
        },
        asio::detached
    );

    // 等待重连发生 (connCount>=2) 或超时
    bool reconnected = false;
    for (int i = 0; i < 100; ++i) {
        if (connCount.load() >= 2) {
            reconnected = true;
            break;
        }
        co_await testSleep(ex, std::chrono::milliseconds{50});
    }
    XX_TEST_EXPECT_TRUE(reconnected);
    // 等待客户端处理完, 检查重连携带 lastSeq
    co_await testSleep(ex, std::chrono::milliseconds{300});
    XX_TEST_EXPECT_TRUE(sawResumeLastSeq.load());

    co_await remote->shutdown();
    server.stop();
    th.join();
}

// ---------------------------------------------------------------------------
// 9. ChannelTransport 回环 (进程内传输)
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_channel_transport_loopback() {
    auto ex   = co_await asio::this_coro::executor;
    auto pair = remote::ChannelTransport::makePair(ex, ex);
    auto&     clientT = *pair.first;
    auto&     serverT = *pair.second;

    auto s1 = co_await clientT.send(R"({"a":1})");
    XX_TEST_EXPECT_TRUE(s1.has_value());
    auto r1 = co_await serverT.recv();
    XX_TEST_EXPECT_TRUE(r1.has_value());
    if (r1) {
        XX_TEST_EXPECT_EQ(r1->payload, std::string(R"({"a":1})"));
    }

    co_await serverT.send(R"({"b":2})");
    auto r2 = co_await clientT.recv();
    XX_TEST_EXPECT_TRUE(r2.has_value());
    if (r2) {
        XX_TEST_EXPECT_EQ(r2->payload, std::string(R"({"b":2})"));
    }

    // 关闭一端 -> 对端 recv 感知断开
    clientT.close();
    auto r3 = co_await serverT.recv();
    XX_TEST_EXPECT_FALSE(r3.has_value());
    co_return;
}

// ---------------------------------------------------------------------------
// 10. 服务端 token delta 合并降帧
// ---------------------------------------------------------------------------

/// 记录发送内容; recv 阻塞于 gate (供 readLoop 挂起), close/stop 时返回 Close
class MockServerTransport : public remote::MessageTransport {
public:

    using GateChan = asio::experimental::concurrent_channel<void(boost::system::error_code, bool)>;

    explicit MockServerTransport(asio::any_io_executor ex) :
        gate_(std::make_shared<GateChan>(ex, 1)) {}

    std::vector<std::string> sent;
    std::mutex               sentMu;

    asio::awaitable<std::expected<void, std::string>> send(std::string_view text) override {
        std::lock_guard<std::mutex> lock(sentMu);
        sent.emplace_back(text);
        co_return std::expected<void, std::string>{};
    }

    asio::awaitable<std::expected<util::WsMessage, std::string>> recv() override {
        try {
            co_await gate_->async_receive(asio::use_awaitable);
            util::WsMessage m;
            m.type = util::WsMessage::Type::Close;
            co_return m;
        } catch (const boost::system::system_error& e) {
            co_return std::unexpected<std::string>(e.what());
        }
    }

    void close() override {
        gate_->close();
    }

    bool isOpen() const noexcept override {
        return true;
    }

    std::vector<std::string> snapshotSent() {
        std::lock_guard<std::mutex> lock(sentMu);
        return sent;
    }

private:

    std::shared_ptr<GateChan> gate_;
};

static asio::awaitable<void> test_server_token_coalescing() {
    auto ex = co_await asio::this_coro::executor;

    auto mock = new MockServerTransport(ex);
    std::unique_ptr<remote::MessageTransport> transport(mock);

    remote::RemoteServerAgentIO::Config cfg;
    auto io = std::make_shared<remote::RemoteServerAgentIO>(ex, std::move(transport), cfg);

    // 启动读/写协程 (readLoop 挂起于 mock recv; writeLoop 处理写队列)
    asio::co_spawn(
        ex,
        [io]() -> asio::awaitable<void> {
            co_await io->run();
            co_return;
        },
        asio::detached
    );

    // 入队: 3 个 text token + 1 个 sync(非 delta) + 2 个 text token
    for (uint64_t s = 1; s <= 3; ++s) {
        agentxx::agent::Delta d;
        d.type = agentxx::agent::Delta::Type::TextToken;
        d.seq  = s;
        d.text = "t" + std::to_string(s);
        io->pushMessage(remote::makeDeltaMsg(d));
    }
    agentxx::agent::SyncPayload sp;
    sp.tailHash = "h";
    io->pushMessage(remote::makeSyncMsg(sp));
    for (uint64_t s = 4; s <= 5; ++s) {
        agentxx::agent::Delta d;
        d.type = agentxx::agent::Delta::Type::TextToken;
        d.seq  = s;
        d.text = "t" + std::to_string(s);
        io->pushMessage(remote::makeDeltaMsg(d));
    }

    // 等待 writeLoop 处理
    co_await testSleep(ex, std::chrono::milliseconds{150});

    auto sent = mock->snapshotSent();
    // 期望 3 帧: 合并(t1t2t3) | sync | 合并(t4t5)
    XX_TEST_EXPECT_EQ(sent.size(), size_t{3});
    if (sent.size() == 3) {
        auto j0 = neograph::json::parse(sent[0]);
        XX_TEST_EXPECT_EQ(j0.value("kind", std::string{}), std::string("text_token"));
        XX_TEST_EXPECT_EQ(j0.value("text", std::string{}), std::string("t1t2t3"));
        XX_TEST_EXPECT_EQ(j0.value("seq", uint64_t{0}), uint64_t{3});

        auto j1 = neograph::json::parse(sent[1]);
        XX_TEST_EXPECT_EQ(j1.value("type", std::string{}), std::string(remote::MsgType::SyncMsg));

        auto j2 = neograph::json::parse(sent[2]);
        XX_TEST_EXPECT_EQ(j2.value("text", std::string{}), std::string("t4t5"));
        XX_TEST_EXPECT_EQ(j2.value("seq", uint64_t{0}), uint64_t{5});
    }

    // 清理: 关闭 mock -> readLoop 退出 -> run() 结束
    mock->close();
    co_await testSleep(ex, std::chrono::milliseconds{100});
    co_return;
}

// ---------------------------------------------------------------------------
// 11. 客户端接收上下文统计 (context_stats)
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_client_context_stats() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});
    server.enableWebSocket(
        "/agent",
        [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
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

    auto ex = co_await asio::this_coro::executor;
    auto client = co_await wsConnect(ex, "ws://127.0.0.1:" + std::to_string(port) + "/agent");
    XX_TEST_EXPECT_TRUE(client.has_value());
    if (client) {
        auto transport = std::make_unique<remote::ClientWsTransport>(std::move(client.value()));
        auto io        = std::make_shared<TestIO>();
        auto remoteIo  = std::make_shared<remote::RemoteClientAgentIO>(
            ex,
            std::move(transport),
            io,
            remote::RemoteClientAgentIO::Config{}
        );

        std::atomic<uint64_t> gotCtx{0};
        std::atomic<uint64_t> gotMax{0};
        remoteIo->setContextStatsCallback([&](uint64_t c, uint64_t m) {
            gotCtx.store(c);
            gotMax.store(m);
        });

        bool ok = co_await remoteIo->start("session", "test-token");
        XX_TEST_EXPECT_TRUE(ok);
        co_await testSleep(ex, std::chrono::milliseconds{200});
        XX_TEST_EXPECT_EQ(gotCtx.load(), uint64_t{1234});
        XX_TEST_EXPECT_EQ(gotMax.load(), uint64_t{5678});
        co_await remoteIo->shutdown();
    }

    server.stop();
    th.join();
}

// ---------------------------------------------------------------------------
// 12. 进程内集成: RemoteClientAgentIO over ChannelTransport vs 协议级 fake server
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_channel_client_integration() {
    auto ex   = co_await asio::this_coro::executor;
    auto pair = remote::ChannelTransport::makePair(ex, ex);
    auto clientT = std::move(pair.first);
    auto serverT = std::move(pair.second);

    // fake server (经 channel 说协议)
    asio::co_spawn(
        ex,
        [st = std::move(serverT)]() mutable -> asio::awaitable<void> {
            auto hello = co_await st->recv();
            if (!hello) {
                co_return;
            }
            auto hj = neograph::json::parse(hello->payload);
            co_await st->send(
                remote::makeHelloAck(true, hj.value("thread", std::string{}), "", {}).dump()
            );
            for (;;) {
                auto m = co_await st->recv();
                if (!m) {
                    co_return;
                }
                auto j = neograph::json::parse(m->payload);
                if (remote::msgType(j) == remote::MsgType::UserInput) {
                    agentxx::agent::Delta d;
                    d.type = agentxx::agent::Delta::Type::TextToken;
                    d.seq  = 1;
                    d.text = "chan-reply";
                    co_await st->send(remote::makeDeltaMsg(d).dump());
                    co_await st->send(
                        remote::makeTurnResult(j.value("thread", std::string{}), false, "", false)
                            .dump()
                    );
                }
            }
        },
        asio::detached
    );

    auto io       = std::make_shared<TestIO>();
    auto remoteIo = std::make_shared<remote::RemoteClientAgentIO>(
        ex,
        std::move(clientT),
        io,
        remote::RemoteClientAgentIO::Config{}
    );

    bool ok = co_await remoteIo->start("session", "");
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
    XX_TEST_EXPECT_TRUE(!io->deltas.empty());
    if (!io->deltas.empty()) {
        XX_TEST_EXPECT_EQ(io->deltas[0].text, std::string("chan-reply"));
    }

    co_await remoteIo->shutdown();
    co_return;
}

// ---------------------------------------------------------------------------
// 13. echo: 多轮输入往返
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_echo() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});
    server.enableWebSocket(
        "/agent",
        [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
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
                    auto text = j->value("text", std::string{});
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

    auto ex     = co_await asio::this_coro::executor;
    auto client = co_await wsConnect(ex, "ws://127.0.0.1:" + std::to_string(port) + "/agent");
    XX_TEST_EXPECT_TRUE(client.has_value());
    if (client) {
        auto transport = std::make_unique<remote::ClientWsTransport>(std::move(client.value()));
        auto io        = std::make_shared<TestIO>();
        auto remoteIo  = std::make_shared<remote::RemoteClientAgentIO>(
            ex,
            std::move(transport),
            io,
            remote::RemoteClientAgentIO::Config{}
        );
        bool ok = co_await remoteIo->start("session", "test-token");
        XX_TEST_EXPECT_TRUE(ok);

        for (int i = 0; i < 3; ++i) {
            remoteIo->sendUserInput("session", "m" + std::to_string(i), i == 0, "");
            auto r = co_await remoteIo->awaitTurnResult();
            XX_TEST_EXPECT_FALSE(r.hasError);
        }
        co_await testSleep(ex, std::chrono::milliseconds{100});
        XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{3});
        if (io->deltas.size() == 3) {
            XX_TEST_EXPECT_EQ(io->deltas[0].text, std::string("echo:m0"));
            XX_TEST_EXPECT_EQ(io->deltas[2].text, std::string("echo:m2"));
        }
        co_await remoteIo->shutdown();
    }
    server.stop();
    th.join();
}

// ---------------------------------------------------------------------------
// 14. 并发写: 多线程同时 pushMessage, 单写协程串行发送, 验证无丢失
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_concurrent_writes() {
    auto ex = co_await asio::this_coro::executor;

    auto mock = new MockServerTransport(ex);
    std::unique_ptr<remote::MessageTransport> transport(mock);
    remote::RemoteServerAgentIO::Config cfg;
    auto io = std::make_shared<remote::RemoteServerAgentIO>(ex, std::move(transport), cfg);

    asio::co_spawn(
        ex,
        [io]() -> asio::awaitable<void> {
            co_await io->run();
            co_return;
        },
        asio::detached
    );

    const int numThreads = 4;
    const int perThread  = 100;
    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([io, t, perThread]() {
            for (int i = 0; i < perThread; ++i) {
                // pong 消息 (非 token delta, 不会被合并); t*10000+i 唯一
                io->pushMessage(remote::makePong(static_cast<int64_t>(t) * 10000 + i));
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    // 等待单写协程排空
    co_await testSleep(ex, std::chrono::milliseconds{400});

    auto sent = mock->snapshotSent();
    XX_TEST_EXPECT_EQ(sent.size(), size_t{numThreads * perThread});

    // 校验唯一性 (无重复/无丢失)
    std::set<int64_t> seen;
    for (const auto& s : sent) {
        try {
            auto j = neograph::json::parse(s);
            seen.insert(j.value("t", int64_t{-1}));
        } catch (...) {
        }
    }
    XX_TEST_EXPECT_EQ(seen.size(), size_t{numThreads * perThread});

    mock->close();
    co_await testSleep(ex, std::chrono::milliseconds{100});
    co_return;
}

// ---------------------------------------------------------------------------
// 15. 多次断线重连: server 前 N 次连接立即断开, 客户端自动多次重连
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_multi_reconnect() {
    std::atomic<int> connCount{0};
    const int        dropTimes = 3;

    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});
    server.enableWebSocket(
        "/agent",
        [&](HttpServer::WsStream& ws) -> asio::awaitable<void> {
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
                co_return; // 立即断开 -> 触发客户端重连
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

    auto ex = co_await asio::this_coro::executor;
    auto io = std::make_shared<ScriptedIO>();
    io->inputs = {"a", "b", "c", "d", "e", "f"};

    remote::RemoteClientAgentIO::Config cfg;
    cfg.reconnectBackoff = std::chrono::milliseconds{50};
    WsClientConfig wsCfg;
    wsCfg.recvTimeout = std::chrono::seconds{5};

    auto remoteIo = std::make_shared<remote::RemoteClientAgentIO>(
        ex,
        io,
        "ws://127.0.0.1:" + std::to_string(port) + "/agent",
        "test-token",
        cfg,
        wsCfg
    );
    asio::co_spawn(
        ex,
        [remoteIo]() -> asio::awaitable<void> {
            co_await remoteIo->runSession("session", "");
            co_return;
        },
        asio::detached
    );

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

    co_await remoteIo->shutdown();
    server.stop();
    th.join();
}

// ---------------------------------------------------------------------------
// 16. 取消: 轮次进行中客户端发 cancel, server 收到并回 interrupted 结果
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_cancel() {
    std::atomic<bool> gotCancel{false};

    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});
    server.enableWebSocket(
        "/agent",
        [&](HttpServer::WsStream& ws) -> asio::awaitable<void> {
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

    auto ex     = co_await asio::this_coro::executor;
    auto client = co_await wsConnect(ex, "ws://127.0.0.1:" + std::to_string(port) + "/agent");
    XX_TEST_EXPECT_TRUE(client.has_value());
    if (client) {
        auto transport = std::make_unique<remote::ClientWsTransport>(std::move(client.value()));
        auto io        = std::make_shared<TestIO>();
        auto remoteIo  = std::make_shared<remote::RemoteClientAgentIO>(
            ex,
            std::move(transport),
            io,
            remote::RemoteClientAgentIO::Config{}
        );
        bool ok = co_await remoteIo->start("session", "test-token");
        XX_TEST_EXPECT_TRUE(ok);

        remoteIo->sendUserInput("session", "hi", true, "");
        remoteIo->cancel("session");
        auto r = co_await remoteIo->awaitTurnResult();
        XX_TEST_EXPECT_TRUE(r.interrupted);
        XX_TEST_EXPECT_TRUE(gotCancel.load());
        co_await remoteIo->shutdown();
    }
    server.stop();
    th.join();
}

// ---------------------------------------------------------------------------
// 17. 重连 sync: 重连时 server 下发全量 sync, 客户端 onSync 被调用
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_reconnect_sync() {
    std::atomic<int> connCount{0};

    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});
    server.enableWebSocket(
        "/agent",
        [&](HttpServer::WsStream& ws) -> asio::awaitable<void> {
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
                auto j = co_await wsRecvJson(ws); // user_input
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
                co_return; // 断开 -> 触发重连
            }
            // 重连: 下发全量 sync
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

    auto ex = co_await asio::this_coro::executor;
    auto io = std::make_shared<ScriptedIO>();
    io->inputs = {"m1", "m2", "m3"};

    remote::RemoteClientAgentIO::Config cfg;
    cfg.reconnectBackoff = std::chrono::milliseconds{50};
    WsClientConfig wsCfg;
    wsCfg.recvTimeout = std::chrono::seconds{5};

    auto remoteIo = std::make_shared<remote::RemoteClientAgentIO>(
        ex,
        io,
        "ws://127.0.0.1:" + std::to_string(port) + "/agent",
        "test-token",
        cfg,
        wsCfg
    );
    asio::co_spawn(
        ex,
        [remoteIo]() -> asio::awaitable<void> {
            co_await remoteIo->runSession("session", "");
            co_return;
        },
        asio::detached
    );

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

    co_await remoteIo->shutdown();
    server.stop();
    th.join();
}

// ---------------------------------------------------------------------------
// 18. 鉴权超时: server 永不回应 hello_ack, 客户端 connect 超时失败
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_auth_timeout() {
    HttpServer server({.address = "127.0.0.1", .port = 0, .ioThreads = 1});
    server.enableWebSocket(
        "/agent",
        [](HttpServer::WsStream& ws) -> asio::awaitable<void> {
            auto hello = co_await wsRecvJson(ws);
            if (hello) {
                // 收到 hello 但永不回应 -> 客户端鉴权超时
                asio::steady_timer timer(
                    co_await asio::this_coro::executor,
                    std::chrono::seconds{5}
                );
                boost::system::error_code ec;
                co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
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

    auto ex     = co_await asio::this_coro::executor;
    auto client = co_await wsConnect(ex, "ws://127.0.0.1:" + std::to_string(port) + "/agent");
    XX_TEST_EXPECT_TRUE(client.has_value());
    if (client) {
        auto transport = std::make_unique<remote::ClientWsTransport>(std::move(client.value()));
        auto io        = std::make_shared<TestIO>();
        remote::RemoteClientAgentIO::Config cfg;
        cfg.authTimeout = std::chrono::milliseconds{300};
        auto remoteIo   = std::make_shared<remote::RemoteClientAgentIO>(
            ex,
            std::move(transport),
            io,
            cfg
        );
        auto start = std::chrono::steady_clock::now();
        bool ok    = co_await remoteIo->start("session", "test-token");
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start
        );
        XX_TEST_EXPECT_FALSE(ok);
        XX_TEST_EXPECT_TRUE(elapsed.count() >= 250 && elapsed.count() < 3000);
        co_await remoteIo->shutdown();
    }
    server.stop();
    th.join();
}

// ---------------------------------------------------------------------------
// 19. 鉴权失败(服务端): 错误 token -> RemoteServerAgentIO 回 hello_ack(ok=false)
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_remote_auth_rejected() {
    auto ex   = co_await asio::this_coro::executor;
    auto pair = remote::ChannelTransport::makePair(ex, ex);
    auto serverT = std::move(pair.first);
    auto clientT = std::move(pair.second);

    remote::RemoteServerAgentIO::Config cfg;
    cfg.token = "secret";
    auto io   = std::make_shared<remote::RemoteServerAgentIO>(ex, std::move(serverT), cfg);
    io->setAuthHandler(
        [ex](
            const std::string& threadId,
            uint64_t,
            const std::string&,
            std::string& outTailHash
        ) -> std::shared_ptr<remote::SessionController> {
            outTailHash = "";
            remote::SessionController::Config scCfg;
            scCfg.threadId = threadId;
            return std::make_shared<remote::SessionController>(
                ex,
                std::weak_ptr<agentxx::agent::DeepAgent>{},
                scCfg
            );
        }
    );

    asio::co_spawn(
        ex,
        [io]() -> asio::awaitable<void> {
            co_await io->run();
            co_return;
        },
        asio::detached
    );

    // 错误 token -> 应收到 hello_ack(ok=false)
    co_await clientT->send(remote::makeHello("session", "wrong-token").dump());
    auto resp = co_await clientT->recv();
    XX_TEST_EXPECT_TRUE(resp.has_value());
    if (resp) {
        auto j = neograph::json::parse(resp->payload);
        XX_TEST_EXPECT_EQ(remote::msgType(j), std::string(remote::MsgType::HelloAck));
        XX_TEST_EXPECT_FALSE(j.value("ok", true));
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

    std::cout << "  [remote] server token coalescing..." << std::endl;
    co_await test_server_token_coalescing();

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
