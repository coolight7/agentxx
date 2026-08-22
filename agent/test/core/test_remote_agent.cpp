// 注意: test_agent.h 会重定义 XX_TEST_PASSED/FAILED 为 g_da_* 计数器,
// 必须先于 test_remote_agent.h 引入, 使后者 (g_remote_*) 的宏定义最后生效
#include "test_agent.h" // 本地 LLM 模拟器 startDaSimServer/g_da_sim_*
#include "test_remote_agent.h"
#include "agentxx/agent/base_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/io/agent_io.h"
#include "agentxx/agent/io/agent_server.h"
#include "agentxx/agent/io/channel_io_transport.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/agent/io/ws_io_transport.h"
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
namespace io = agentxx::agent::io;
using agentxx::agent::SessionServerAgentIO;

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
    std::atomic<double>                lastTps{0.0};
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
        lastTps.store(s.tps);
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
    neograph_asio_error_code ec;
    co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
}

// ---------------------------------------------------------------------------
// WS 收发辅助 (服务端 handler 内使用)
// ---------------------------------------------------------------------------

static asio::awaitable<bool> wsSendJson(HttpServer::WsStream& ws, const neograph::json& j) {
    auto s = j.dump();
    ws.text(true);
    neograph_asio_error_code ec;
    co_await ws.async_write(asio::buffer(s), asio::redirect_error(asio::use_awaitable, ec));
    co_return !ec;
}

static asio::awaitable<std::optional<neograph::json>> wsRecvJson(HttpServer::WsStream& ws) {
    boost::beast::flat_buffer buf;
    neograph_asio_error_code  ec;
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
    using agentxx::agent::SyncPayload;
    using agentxx::agent::ViewMessage;
    using agentxx::agent::WireMessage;
    using agentxx::agent::WsAgentIOTransport;

    {
        Delta d;
        d.type    = Delta::Type::TextToken;
        d.seq     = 5;
        d.text    = "abc";
        d.msgId   = "m1";
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
        auto back = WsAgentIOTransport::deserialize(WsAgentIOTransport::serialize(WireMessage{d}));
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
        // MessageUITip: 通用提示消息 delta 序列化往返
        Delta d;
        d.type    = Delta::Type::MessageUITip;
        d.seq     = 11;
        d.tipType = Delta::TipType::Warning;
        d.text    = "LLM API 请求失败，6 秒后自动重试 (2/5)，错误: connection reset";
        auto back = WsAgentIOTransport::deserialize(WsAgentIOTransport::serialize(WireMessage{d}));
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            auto* bd = std::get_if<Delta>(&*back);
            XX_TEST_EXPECT_TRUE(bd != nullptr);
            if (bd) {
                XX_TEST_EXPECT_TRUE(bd->type == Delta::Type::MessageUITip);
                XX_TEST_EXPECT_TRUE(bd->tipType == Delta::TipType::Warning);
                XX_TEST_EXPECT_EQ(
                    bd->text,
                    std::string("LLM API 请求失败，6 秒后自动重试 (2/5)，错误: connection reset")
                );
            }
        }
    }
    {
        // MessageUITip: Error 级别往返
        Delta d;
        d.type    = Delta::Type::MessageUITip;
        d.tipType = Delta::TipType::Error;
        d.text    = "something went wrong";
        auto back = WsAgentIOTransport::deserialize(WsAgentIOTransport::serialize(WireMessage{d}));
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            auto* bd = std::get_if<Delta>(&*back);
            XX_TEST_EXPECT_TRUE(bd != nullptr);
            if (bd) {
                XX_TEST_EXPECT_TRUE(bd->type == Delta::Type::MessageUITip);
                XX_TEST_EXPECT_TRUE(bd->tipType == Delta::TipType::Error);
                XX_TEST_EXPECT_EQ(bd->text, std::string("something went wrong"));
            }
        }
    }
    {
        // ThinkToken: 加密 thinking 载体 Delta 序列化往返
        Delta d;
        d.type  = Delta::Type::ThinkToken;
        d.seq   = 15;
        d.text  = "";
        d.think = ViewMessage::ThinkData{
            .reasoningTokens = 250,
            .isEncrypted     = true,
        };
        auto back = WsAgentIOTransport::deserialize(WsAgentIOTransport::serialize(WireMessage{d}));
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            auto* bd = std::get_if<Delta>(&*back);
            XX_TEST_EXPECT_TRUE(bd != nullptr);
            if (bd) {
                XX_TEST_EXPECT_TRUE(bd->type == Delta::Type::ThinkToken);
                XX_TEST_EXPECT_EQ(bd->seq, uint64_t{15});
                XX_TEST_EXPECT_TRUE(bd->text.empty());
                XX_TEST_EXPECT_TRUE(bd->think.has_value());
                if (bd->think) {
                    XX_TEST_EXPECT_TRUE(bd->think->isEncrypted);
                    XX_TEST_EXPECT_EQ(bd->think->reasoningTokens, 250);
                }
            }
        }
    }
    {
        SyncPayload p;
        p.fromIndex = 2;
        p.tailHash  = "hash123";
        auto vm     = ViewMessage::makeText(ViewMessage::Role::User, "hi");
        vm.id       = "id1";
        p.messages.push_back(std::move(vm));
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
                    XX_TEST_EXPECT_TRUE(sp->messages[0].role == ViewMessage::Role::User);
                    XX_TEST_EXPECT_EQ(sp->messages[0].text, std::string("hi"));
                }
            }
        }
    }
    {
        agentxx::agent::WireHello hello{"sess", "tok", 3, "th"};
        auto                      json = WsAgentIOTransport::serialize(WireMessage{hello});
        auto                      back = WsAgentIOTransport::deserialize(json);
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            auto* h = std::get_if<agentxx::agent::WireHello>(&*back);
            XX_TEST_EXPECT_TRUE(h != nullptr);
            if (h) {
                XX_TEST_EXPECT_EQ(h->sessionId, std::string("sess"));
                XX_TEST_EXPECT_EQ(h->token, std::string("tok"));
                XX_TEST_EXPECT_EQ(h->lastSeq, uint64_t{3});
            }
        }
    }
    {
        agentxx::agent::WireInterruptRequest ir{7, "sess", "node", "val", "{}"};
        auto                                 json = WsAgentIOTransport::serialize(WireMessage{ir});
        auto                                 back = WsAgentIOTransport::deserialize(json);
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            auto* r = std::get_if<agentxx::agent::WireInterruptRequest>(&*back);
            XX_TEST_EXPECT_TRUE(r != nullptr);
            if (r) {
                XX_TEST_EXPECT_EQ(r->id, int64_t{7});
            }
        }
    }
    {
        agentxx::agent::WireInterruptExpired expired{7, "sess"};
        auto json = WsAgentIOTransport::serialize(WireMessage{expired});
        auto back = WsAgentIOTransport::deserialize(json);
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            auto* e = std::get_if<agentxx::agent::WireInterruptExpired>(&*back);
            XX_TEST_EXPECT_TRUE(e != nullptr);
            if (e) {
                XX_TEST_EXPECT_EQ(e->id, int64_t{7});
                XX_TEST_EXPECT_EQ(e->sessionId, std::string("sess"));
            }
        }
    }
    {
        // 客户端记住权限选择 (WireSetPermission) 序列化往返
        agentxx::agent::WireSetPermission perm{"sess", "/data/projects", true, 1};
        auto                              json = WsAgentIOTransport::serialize(WireMessage{perm});
        auto                              back = WsAgentIOTransport::deserialize(json);
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            auto* p = std::get_if<agentxx::agent::WireSetPermission>(&*back);
            XX_TEST_EXPECT_TRUE(p != nullptr);
            if (p) {
                XX_TEST_EXPECT_EQ(p->sessionId, std::string("sess"));
                XX_TEST_EXPECT_EQ(p->path, std::string("/data/projects"));
                XX_TEST_EXPECT_TRUE(p->allow);
                XX_TEST_EXPECT_EQ(p->index, size_t{1});
            }
        }
        // 拒绝规则往返
        agentxx::agent::WireSetPermission deny{"sess", "/etc/secret", false, 0};
        auto denyJson = WsAgentIOTransport::serialize(WireMessage{deny});
        auto denyBack = WsAgentIOTransport::deserialize(denyJson);
        XX_TEST_EXPECT_TRUE(denyBack.has_value());
        if (denyBack) {
            auto* p = std::get_if<agentxx::agent::WireSetPermission>(&*denyBack);
            XX_TEST_EXPECT_TRUE(p != nullptr);
            if (p) {
                XX_TEST_EXPECT_FALSE(p->allow);
                XX_TEST_EXPECT_EQ(p->index, size_t{0});
            }
        }
    }
    {
        // 用户输入附带模型选择 (TUI 切模型随下一条消息携带, BaseAgent 新一轮
        // 会话自动切换): 序列化往返须完整保留 model 字段
        agentxx::agent::WireUserInput ui{"sess", "hello", "model-b"};
        auto                          json = WsAgentIOTransport::serialize(WireMessage{ui});
        auto                          back = WsAgentIOTransport::deserialize(json);
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            auto* r = std::get_if<agentxx::agent::WireUserInput>(&*back);
            XX_TEST_EXPECT_TRUE(r != nullptr);
            if (r) {
                XX_TEST_EXPECT_EQ(r->sessionId, std::string("sess"));
                XX_TEST_EXPECT_EQ(r->text, std::string("hello"));
                XX_TEST_EXPECT_EQ(r->model, std::string("model-b"));
            }
        }

        // 未携带模型: model 为空且 JSON 不输出 "model" 字段 (保持旧格式,
        // 旧客户端向后兼容)
        agentxx::agent::WireUserInput plain{"sess", "hi"};
        auto                          plainJson = WsAgentIOTransport::serialize(WireMessage{plain});
        XX_TEST_EXPECT_TRUE(plainJson.find("\"model\"") == std::string::npos);
        auto plainBack = WsAgentIOTransport::deserialize(plainJson);
        XX_TEST_EXPECT_TRUE(plainBack.has_value());
        if (plainBack) {
            auto* r = std::get_if<agentxx::agent::WireUserInput>(&*plainBack);
            XX_TEST_EXPECT_TRUE(r != nullptr);
            if (r) {
                XX_TEST_EXPECT_TRUE(r->model.empty());
            }
        }
    }
    {
        agentxx::agent::WireListSessions ls{};
        auto back = WsAgentIOTransport::deserialize(WsAgentIOTransport::serialize(WireMessage{ls}));
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            XX_TEST_EXPECT_TRUE(std::get_if<agentxx::agent::WireListSessions>(&*back) != nullptr);
        }
    }
    {
        agentxx::agent::WireSessionList sl;
        sl.sessions.push_back(agentxx::agent::SessionInfo{"t1", "title-1", 1712345678000});
        sl.sessions.push_back(agentxx::agent::SessionInfo{"t2", "", 0});
        auto back = WsAgentIOTransport::deserialize(WsAgentIOTransport::serialize(WireMessage{sl}));
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            auto* r = std::get_if<agentxx::agent::WireSessionList>(&*back);
            XX_TEST_EXPECT_TRUE(r != nullptr);
            if (r) {
                XX_TEST_EXPECT_EQ(r->sessions.size(), size_t{2});
                if (r->sessions.size() == 2) {
                    XX_TEST_EXPECT_EQ(r->sessions[0].sessionId, std::string("t1"));
                    XX_TEST_EXPECT_EQ(r->sessions[0].title, std::string("title-1"));
                    XX_TEST_EXPECT_EQ(r->sessions[0].lastActiveMs, int64_t{1712345678000});
                    XX_TEST_EXPECT_EQ(r->sessions[1].sessionId, std::string("t2"));
                    XX_TEST_EXPECT_TRUE(r->sessions[1].title.empty());
                    XX_TEST_EXPECT_EQ(r->sessions[1].lastActiveMs, int64_t{0});
                }
            }
        }
    }
    {
        agentxx::agent::WireSwitchSession sw{"target-session"};
        auto back = WsAgentIOTransport::deserialize(WsAgentIOTransport::serialize(WireMessage{sw}));
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            auto* r = std::get_if<agentxx::agent::WireSwitchSession>(&*back);
            XX_TEST_EXPECT_TRUE(r != nullptr);
            if (r) {
                XX_TEST_EXPECT_EQ(r->sessionId, std::string("target-session"));
            }
        }
    }
    {
        // 插件事件转发: WirePluginData 序列化往返 (载荷为插件定义 schema 的
        // JSON 字符串, 宿主只透传; 系统资源占用链路 (agentxx_system_monitor
        // 周期采集) 与 codegraph 索引状态均走本通道)
        agentxx::agent::WirePluginData pd;
        pd.plugin = "agentxx_system_monitor";
        pd.event  = "usage";
        pd.data
            = R"({"cpu":42.5,"mem_total_mb":16384,"mem_used_mb":8192,"mem_percent":50.0,"gpus":[{"name":"NVIDIA RTX 4090","dedicated_vram_mb":24576,"dedicated_vram_used_mb":12000,"shared_vram_mb":8192,"shared_vram_used_mb":100,"usage_percent":33.3}]})";
        auto back = WsAgentIOTransport::deserialize(WsAgentIOTransport::serialize(WireMessage{pd}));
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            auto* r = std::get_if<agentxx::agent::WirePluginData>(&*back);
            XX_TEST_EXPECT_TRUE(r != nullptr);
            if (r) {
                XX_TEST_EXPECT_EQ(r->plugin, pd.plugin);
                XX_TEST_EXPECT_EQ(r->event, pd.event);
                // JSON 字符串原样往返
                XX_TEST_EXPECT_EQ(r->data, pd.data);
            }
        }
    }
    {
        // client 插件事件上行: WirePluginDataUp 序列化往返 (如 /sysinfo 开关
        // 同步 agentxx_system_monitor.usage_enabled)
        agentxx::agent::WirePluginDataUp pu;
        pu.plugin = "agentxx_system_monitor";
        pu.event  = "usage_enabled";
        pu.data   = R"({"enabled":true})";
        auto back = WsAgentIOTransport::deserialize(WsAgentIOTransport::serialize(WireMessage{pu}));
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            auto* r = std::get_if<agentxx::agent::WirePluginDataUp>(&*back);
            XX_TEST_EXPECT_TRUE(r != nullptr);
            if (r) {
                XX_TEST_EXPECT_EQ(r->plugin, pu.plugin);
                XX_TEST_EXPECT_EQ(r->event, pu.event);
                XX_TEST_EXPECT_EQ(r->data, pu.data);
            }
        }
    }
    {
        // 消息队列同步: WireMessageQueueUpdate 序列化往返
        agentxx::agent::WireMessageQueueUpdate qu;
        qu.sessionId = "session-q";
        agentxx::agent::MessageQueueItem it1;
        it1.id          = "q-1";
        it1.text        = "task 1";
        it1.model       = "model-a";
        it1.createdAtMs = 123456789;
        qu.items.push_back(it1);
        auto back = WsAgentIOTransport::deserialize(WsAgentIOTransport::serialize(WireMessage{qu}));
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            auto* r = std::get_if<agentxx::agent::WireMessageQueueUpdate>(&*back);
            XX_TEST_EXPECT_TRUE(r != nullptr);
            if (r) {
                XX_TEST_EXPECT_EQ(r->sessionId, qu.sessionId);
                XX_TEST_EXPECT_EQ(r->items.size(), size_t{1});
                if (!r->items.empty()) {
                    XX_TEST_EXPECT_EQ(r->items[0].id, it1.id);
                    XX_TEST_EXPECT_EQ(r->items[0].text, it1.text);
                    XX_TEST_EXPECT_EQ(r->items[0].model, it1.model);
                    XX_TEST_EXPECT_EQ(r->items[0].createdAtMs, it1.createdAtMs);
                }
            }
        }
    }
    {
        // 清空消息队列: WireClearMessageQueue
        agentxx::agent::WireClearMessageQueue cq;
        cq.sessionId = "session-q";
        auto back = WsAgentIOTransport::deserialize(WsAgentIOTransport::serialize(WireMessage{cq}));
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            auto* r = std::get_if<agentxx::agent::WireClearMessageQueue>(&*back);
            XX_TEST_EXPECT_TRUE(r != nullptr);
            if (r) {
                XX_TEST_EXPECT_EQ(r->sessionId, cq.sessionId);
            }
        }
    }
    {
        // 删除消息队列条目: WireRemoveQueueItem
        agentxx::agent::WireRemoveQueueItem rq;
        rq.sessionId = "session-q";
        rq.itemId    = "q-2";
        auto back = WsAgentIOTransport::deserialize(WsAgentIOTransport::serialize(WireMessage{rq}));
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            auto* r = std::get_if<agentxx::agent::WireRemoveQueueItem>(&*back);
            XX_TEST_EXPECT_TRUE(r != nullptr);
            if (r) {
                XX_TEST_EXPECT_EQ(r->sessionId, rq.sessionId);
                XX_TEST_EXPECT_EQ(r->itemId, rq.itemId);
            }
        }
    }
    {
        // 插队执行: WireInterruptAndRunNext
        agentxx::agent::WireInterruptAndRunNext in;
        in.sessionId = "session-q";
        auto back = WsAgentIOTransport::deserialize(WsAgentIOTransport::serialize(WireMessage{in}));
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back) {
            auto* r = std::get_if<agentxx::agent::WireInterruptAndRunNext>(&*back);
            XX_TEST_EXPECT_TRUE(r != nullptr);
            if (r) {
                XX_TEST_EXPECT_EQ(r->sessionId, in.sessionId);
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
            io::makeHelloAck(true, hello->value("sessionId", std::string{}), "", {})
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

    auto transport
        = std::make_shared<agentxx::agent::WsAgentIOTransport>(ex, url, "test-token", cfg, wsCfg);

    agentxx::agent::WireHello hello{"session", "test-token", 0, ""};
    bool                      ok = co_await transport->connect(hello);
    XX_TEST_EXPECT_TRUE(ok);

    if (ok) {
        agentxx::agent::WireUserInput input{"session", "ping"};
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
    co_await wsSendJson(ws, io::makeHelloAck(ok, hello->value("sessionId", std::string{}), "", {}));
    if (!ok) {
        co_return;
    }
    for (;;) {
        auto j = co_await wsRecvJson(ws);
        if (!j) {
            co_return;
        }
        auto t = io::msgType(*j);
        if (t == io::MsgType::UserInput) {
            agentxx::agent::Delta d;
            d.type = agentxx::agent::Delta::Type::TextToken;
            d.seq  = 1;
            d.text = "hello from server";
            co_await wsSendJson(ws, io::makeDeltaMsg(d));
            co_await wsSendJson(
                ws,
                io::makeTurnResult(j->value("sessionId", std::string{}), false, "", false)
            );
        } else if (t == io::MsgType::Ping) {
            co_await wsSendJson(ws, io::makePong(j->value("t", int64_t{0})));
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
            ex,
            url,
            "test-token",
            cfg,
            wsCfg
        );
        auto io = std::make_shared<TestIO>();
        io->setTransport(transport);

        agentxx::agent::WireHello hello{"session", "test-token", 0, ""};
        bool                      ok = co_await transport->connect(hello);
        XX_TEST_EXPECT_TRUE(ok);

        if (ok) {
            asio::co_spawn(ex, io->runTransportLoop(), asio::detached);

            io->sendToPeer(agentxx::agent::WireUserInput{"session", "hi"});
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
            ex,
            url,
            "wrong-token",
            cfg,
            wsCfg
        );
        agentxx::agent::WireHello hello{"session", "wrong-token", 0, ""};
        bool                      ok = co_await transport->connect(hello);
        XX_TEST_EXPECT_FALSE(ok);
        transport->close();
    }

    server.stop();
    th.join();
}

// ---------------------------------------------------------------------------
// 4. SessionServerAgentIO: delta 环形缓冲 + 增量重放 (经 ChannelAgentIOTransport)
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_session_controller_replay() {
    auto ex = co_await asio::this_coro::executor;

    auto tp      = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);
    auto clientT = std::move(tp.first);
    auto serverT = std::move(tp.second);

    SessionServerAgentIO::Config cfg;
    cfg.sessionId      = "session";
    cfg.deltaBufferCap = 100;
    auto sc            = std::make_shared<SessionServerAgentIO>(
        ex,
        std::weak_ptr<agentxx::agent::BaseAgent>{},
        cfg
    );

    // 强制 transport: 先装配再产出事件 (sendToPeer 同时写缓冲并实时转发 client)
    sc->setTransport(std::shared_ptr<agentxx::agent::AgentIOTransportBase>(std::move(serverT)));

    for (uint64_t s = 1; s <= 5; ++s) {
        agentxx::agent::Delta d;
        d.type = agentxx::agent::Delta::Type::TextToken;
        d.seq  = s;
        d.text = "t" + std::to_string(s);
        sc->sendToPeer(d);
    }

    // lastSeq=3 -> 应增量重放 seq 4,5
    agentxx::agent::WireHello hello{"session", "", 3, ""};
    sc->handleHello(hello);

    // 从 client 端读取: 先收到 5 条实时 delta, 随后 HelloAck, 最后增量重放 delta(4), delta(5)。
    // (客户端握手循环会丢弃 HelloAck 之前的消息, 故服务端必须先发 HelloAck 再重放)
    std::vector<agentxx::agent::WireMessage> received;
    for (int i = 0; i < 8; ++i) {
        auto msg = co_await clientT->recv();
        if (!msg) {
            break;
        }
        received.push_back(std::move(*msg));
    }

    XX_TEST_EXPECT_EQ(received.size(), size_t{8});
    if (received.size() >= 8) {
        // 前 5 条为实时转发的 delta
        for (uint64_t s = 1; s <= 5; ++s) {
            auto* d = std::get_if<agentxx::agent::Delta>(&received[s - 1]);
            XX_TEST_EXPECT_TRUE(d != nullptr);
            if (d) {
                XX_TEST_EXPECT_EQ(d->seq, s);
            }
        }
        auto* ack = std::get_if<agentxx::agent::WireHelloAck>(&received[5]);
        XX_TEST_EXPECT_TRUE(ack != nullptr);
        if (ack) {
            XX_TEST_EXPECT_TRUE(ack->ok);
        }
        // 重放的 delta 不应重复写入缓冲 (seq 守卫), 此处收到重放 seq 4,5
        auto* d0 = std::get_if<agentxx::agent::Delta>(&received[6]);
        auto* d1 = std::get_if<agentxx::agent::Delta>(&received[7]);
        XX_TEST_EXPECT_TRUE(d0 != nullptr);
        XX_TEST_EXPECT_TRUE(d1 != nullptr);
        if (d0 && d1) {
            XX_TEST_EXPECT_EQ(d0->seq, uint64_t{4});
            XX_TEST_EXPECT_EQ(d1->seq, uint64_t{5});
        }
    }
    // 显式关闭: 使挂起的 recv 完成, 避免 detached/挂起协程持有 transport 泄漏
    clientT->close();
    sc->stop();
    co_return;
}

// ---------------------------------------------------------------------------
// 5. SessionServerAgentIO: 缓冲过旧 -> 回退全量 sync
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_session_controller_replay_fallback() {
    auto ex = co_await asio::this_coro::executor;

    auto tp      = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);
    auto clientT = std::move(tp.first);
    auto serverT = std::move(tp.second);

    SessionServerAgentIO::Config cfg;
    cfg.sessionId      = "session";
    cfg.deltaBufferCap = 3;
    auto sc            = std::make_shared<SessionServerAgentIO>(
        ex,
        std::weak_ptr<agentxx::agent::BaseAgent>{},
        cfg
    );

    // 强制 transport: 先装配再产出事件
    sc->setTransport(std::shared_ptr<agentxx::agent::AgentIOTransportBase>(std::move(serverT)));

    for (uint64_t s = 1; s <= 10; ++s) {
        agentxx::agent::Delta d;
        d.type = agentxx::agent::Delta::Type::TextToken;
        d.seq  = s;
        sc->sendToPeer(d);
    }

    // lastSeq=2 -> 缓冲保留 8,9,10; 2+1=3 < 8 -> 全量 sync
    agentxx::agent::WireHello hello{"session", "", 2, ""};
    sc->handleHello(hello);

    // 先排空 10 条实时转发的 delta
    for (int i = 0; i < 10; ++i) {
        auto liveMsg = co_await clientT->recv();
        XX_TEST_EXPECT_TRUE(liveMsg.has_value());
    }

    // HelloAck 须最先到达, 随后全量 SyncPayload
    auto ackMsg = co_await clientT->recv();
    XX_TEST_EXPECT_TRUE(ackMsg.has_value());
    if (ackMsg) {
        XX_TEST_EXPECT_TRUE(std::get_if<agentxx::agent::WireHelloAck>(&*ackMsg) != nullptr);
    }
    auto msg = co_await clientT->recv();
    XX_TEST_EXPECT_TRUE(msg.has_value());
    if (msg) {
        auto* sp = std::get_if<agentxx::agent::SyncPayload>(&*msg);
        XX_TEST_EXPECT_TRUE(sp != nullptr);
    }
    // 显式关闭: 使挂起的 recv 完成, 避免挂起协程持有 transport 泄漏
    clientT->close();
    sc->stop();
    co_return;
}

// ---------------------------------------------------------------------------
// 6. SessionServerAgentIO: 请求级超时 (handleInterrupt 无响应 -> 超时返回,
//    并通知客户端中断已过期)
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_session_controller_interrupt_timeout() {
    auto ex = co_await asio::this_coro::executor;

    auto gracePair = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);
    auto clientT   = std::move(gracePair.first);
    auto serverT   = std::move(gracePair.second);

    SessionServerAgentIO::Config cfg;
    cfg.sessionId        = "session";
    cfg.interruptTimeout = std::chrono::milliseconds{300};
    auto sc              = std::make_shared<SessionServerAgentIO>(
        ex,
        std::weak_ptr<agentxx::agent::BaseAgent>{},
        cfg
    );
    sc->setTransport(std::shared_ptr<agentxx::agent::AgentIOTransportBase>(std::move(serverT)));

    auto start   = std::chrono::steady_clock::now();
    auto result  = co_await sc->handleInterrupt("session", "node", "val", "{}");
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start
    );

    XX_TEST_EXPECT_TRUE(result.is_array());
    XX_TEST_EXPECT_TRUE(elapsed.count() >= 250 && elapsed.count() < 3000);

    // 超时后 server 应通知客户端该中断已过期 (客户端据此标记消息并结束等待);
    // channel 中先收到 WireInterruptRequest, 超时后再收到 WireInterruptExpired
    auto reqMsg = co_await clientT->recv();
    XX_TEST_EXPECT_TRUE(reqMsg.has_value());
    if (reqMsg) {
        auto* req = std::get_if<agentxx::agent::WireInterruptRequest>(&*reqMsg);
        XX_TEST_EXPECT_TRUE(req != nullptr);
        if (req) {
            XX_TEST_EXPECT_EQ(req->id, int64_t{1});
        }
    }
    auto msg = co_await clientT->recv();
    XX_TEST_EXPECT_TRUE(msg.has_value());
    if (msg) {
        auto* expired = std::get_if<agentxx::agent::WireInterruptExpired>(&*msg);
        XX_TEST_EXPECT_TRUE(expired != nullptr);
        if (expired) {
            XX_TEST_EXPECT_EQ(expired->id, int64_t{1});
            XX_TEST_EXPECT_EQ(expired->sessionId, std::string("session"));
        }
    }
    co_return;
}

// ---------------------------------------------------------------------------
// 7. SessionServerAgentIO: grace period 断线 -> 宽限期满失败挂起请求
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_session_controller_grace() {
    auto ex = co_await asio::this_coro::executor;

    auto gracePair = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);
    auto clientT   = std::move(gracePair.first);
    auto serverT   = std::move(gracePair.second);

    SessionServerAgentIO::Config cfg;
    cfg.sessionId        = "session";
    cfg.gracePeriod      = std::chrono::milliseconds{200};
    cfg.interruptTimeout = std::chrono::seconds{10};
    auto sc              = std::make_shared<SessionServerAgentIO>(
        ex,
        std::weak_ptr<agentxx::agent::BaseAgent>{},
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
    // 显式关闭: 使挂起的 recv 完成, 避免挂起协程持有 transport 泄漏
    clientT->close();
    sc->stop();
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
        if (myConn >= 2 && hello->value("lastSeq", uint64_t{0}) > 0) {
            sawResumeLastSeq = true;
        }
        co_await wsSendJson(
            ws,
            io::makeHelloAck(true, hello->value("sessionId", std::string{}), "", {})
        );
        if (myConn == 1) {
            auto j = co_await wsRecvJson(ws);
            if (j) {
                agentxx::agent::Delta d;
                d.type = agentxx::agent::Delta::Type::TextToken;
                d.seq  = 1;
                d.text = "before-drop";
                co_await wsSendJson(ws, io::makeDeltaMsg(d));
                co_await wsSendJson(
                    ws,
                    io::makeTurnResult(j->value("sessionId", std::string{}), false, "", false)
                );
            }
            co_return;
        }
        for (;;) {
            auto j = co_await wsRecvJson(ws);
            if (!j) {
                co_return;
            }
            if (io::msgType(*j) == io::MsgType::UserInput) {
                co_await wsSendJson(
                    ws,
                    io::makeTurnResult(j->value("sessionId", std::string{}), false, "", false)
                );
            } else if (io::msgType(*j) == io::MsgType::Ping) {
                co_await wsSendJson(ws, io::makePong(j->value("t", int64_t{0})));
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
    cfg.reconnectBackoff  = std::chrono::milliseconds{100};
    cfg.heartbeatInterval = std::chrono::seconds{60};
    util::WsClientConfig wsCfg;
    wsCfg.recvTimeout = std::chrono::seconds{5};

    auto transport
        = std::make_shared<agentxx::agent::WsAgentIOTransport>(ex, url, "test-token", cfg, wsCfg);
    auto io = std::make_shared<TestIO>();
    io->setTransport(transport);

    agentxx::agent::WireHello hello{"session", "test-token", 0, ""};
    bool                      ok = co_await transport->connect(hello);
    XX_TEST_EXPECT_TRUE(ok);

    asio::co_spawn(ex, io->runTransportLoop(), asio::detached);

    io->sendToPeer(agentxx::agent::WireUserInput{"session", "msg1"});

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
    auto  ex      = co_await asio::this_coro::executor;
    auto  pair    = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);
    auto& clientT = *pair.first;
    auto& serverT = *pair.second;

    agentxx::agent::WireUserInput msg1{"session", "hello"};
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
    serverT.close();
    co_return;
}

// ---------------------------------------------------------------------------
// 9b. runTransportLoop 绑定局部 transport (连接替换安全):
// 服务端同一 sessionId 新连接替换旧连接 (AgentServer::serveTransport 中
// setTransport + close 旧 transport) 时, 旧接收循环应随旧 transport 关闭而退出,
// 而不是跟随成员 transport_ 切换到新 transport 上继续 recv (消息被两个循环
// 瓜分 / 旧协程泄漏 / onDisconnect 重复触发)
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_run_transport_loop_replace_transport() {
    auto ex = co_await asio::this_coro::executor;

    // 第一次连接的传输对 (controller 端 tA / 对端 tAPeer)
    auto [tA, tAPeer] = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);
    // 替换后的新传输对 (controller 端 tB / 对端 tBPeer)
    auto [tB, tBPeer] = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);

    auto io = std::make_shared<TestIO>();
    io->setTransport(std::shared_ptr<agentxx::agent::AgentIOTransportBase>(std::move(tA)));

    // 旧接收循环 (对应旧连接的 serveTransport 协程), 完成时置 oldLoopExited
    std::atomic<bool> oldLoopExited{false};
    asio::co_spawn(ex, io->runTransportLoop(), [&oldLoopExited](std::exception_ptr) {
        oldLoopExited.store(true, std::memory_order_release);
    });

    // 旧连接上收一条 Delta (确认循环工作)
    agentxx::agent::Delta da;
    da.type = agentxx::agent::Delta::Type::TextToken;
    da.text = "a";
    tAPeer->send(agentxx::agent::WireMessage{da});
    co_await testSleep(ex, std::chrono::milliseconds{50});
    XX_TEST_EXPECT_EQ(io->deltaCount(), size_t{1});
    XX_TEST_EXPECT_TRUE(io->deltaText(0) == "a");
    XX_TEST_EXPECT_FALSE(oldLoopExited.load()); // 循环仍在运行

    // 模拟连接替换: 换用新 transport, 关闭旧 transport。
    // 注意 tB 是 unique_ptr, move 进 shared_ptr 后原指针为空, 后续 close 须
    // 使用保存的 shared_ptr 副本 (tBShared)
    auto tBShared = std::shared_ptr<agentxx::agent::AgentIOTransportBase>(std::move(tB));
    io->setTransport(tBShared);
    tAPeer->close(); // 旧 transport 关闭 → 旧接收循环应退出

    co_await testSleep(ex, std::chrono::milliseconds{100});
    // 修复验证点: 旧循环随旧 transport 关闭而退出
    // (修复前绑定成员 transport_: 旧循环会切到新 transport 继续 recv, 此处为 false)
    XX_TEST_EXPECT_TRUE(oldLoopExited.load(std::memory_order_acquire));
    // 旧对端已不再使用, 立即释放 (避免与协程帧析构的时序竞争导致泄漏)
    tAPeer.reset();

    // 新接收循环 (模拟新连接的 serveTransport 发起): 绑定当前成员 transport_ (tB)
    std::atomic<bool> newLoopExited{false};
    asio::co_spawn(ex, io->runTransportLoop(), [&newLoopExited](std::exception_ptr) {
        newLoopExited.store(true, std::memory_order_release);
    });

    // 新连接上收发消息正常 (无两个循环瓜分)
    agentxx::agent::Delta db;
    db.type = agentxx::agent::Delta::Type::TextToken;
    db.text = "b";
    tBPeer->send(agentxx::agent::WireMessage{db});
    co_await testSleep(ex, std::chrono::milliseconds{50});
    XX_TEST_EXPECT_EQ(io->deltaCount(), size_t{2});
    XX_TEST_EXPECT_TRUE(io->deltaText(1) == "b");

    // 关闭新 transport, 等待新循环退出: 本测试持有的 io (shared_ptr) 是
    // runTransportLoop 协程的 this, 若协程未退出就返回, io/transport 析构后
    // 挂起的 recv 会访问已析构对象 (use-after-free)
    tBShared->close();
    tBPeer->close();
    for (int i = 0; i < 20 && !newLoopExited.load(std::memory_order_acquire); ++i) {
        co_await testSleep(ex, std::chrono::milliseconds{20});
    }
    XX_TEST_EXPECT_TRUE(newLoopExited.load(std::memory_order_acquire));
    // 等 co_spawn 状态机完全析构接收循环 (flag 在 handler 中置位, 状态机/帧
    // 析构紧随其后; 立即返回会让帧析构与对端/io 析构产生时序竞争, 偶发泄漏)
    co_await testSleep(ex, std::chrono::milliseconds{20});
    // 显式释放对端传输: 确保 tBPeer 在测试帧析构前销毁
    tBPeer.reset();
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
            io::makeHelloAck(true, hello->value("sessionId", std::string{}), "", {})
        );
        co_await wsSendJson(ws, io::makeContextStats(1234, 5678, 12.5));
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

    auto transport
        = std::make_shared<agentxx::agent::WsAgentIOTransport>(ex, url, "test-token", cfg, wsCfg);
    auto io = std::make_shared<TestIO>();
    io->setTransport(transport);

    agentxx::agent::WireHello hello{"session", "test-token", 0, ""};
    bool                      ok = co_await transport->connect(hello);
    XX_TEST_EXPECT_TRUE(ok);

    if (ok) {
        asio::co_spawn(ex, io->runTransportLoop(), asio::detached);
        co_await testSleep(ex, std::chrono::milliseconds{300});
        XX_TEST_EXPECT_EQ(io->lastCtxTokens.load(), uint64_t{1234});
        XX_TEST_EXPECT_EQ(io->lastMaxTokens.load(), uint64_t{5678});
        // tps 字段 round-trip (makeContextStats 第三个参数)
        XX_TEST_EXPECT_TRUE(io->lastTps.load() > 12.0 && io->lastTps.load() < 13.0);
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

    auto tp      = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);
    auto clientT = std::move(tp.first);
    auto serverT = std::move(tp.second);

    // fake server (经 channel 说协议)
    asio::co_spawn(
        ex,
        [st = std::move(serverT)]() mutable -> asio::awaitable<void> {
            auto hello = co_await st->recv();
            if (!hello) {
                co_return;
            }
            auto* h = std::get_if<agentxx::agent::WireHello>(&*hello);
            if (!h) {
                co_return;
            }
            st->send(agentxx::agent::WireMessage{
                agentxx::agent::WireHelloAck{true, h->sessionId, "", {}}
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
                        agentxx::agent::WireTurnResult{ui->sessionId, false, "", false}
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

    io->sendToPeer(agentxx::agent::WireUserInput{"session", "hi"});
    co_await testSleep(ex, std::chrono::milliseconds{300});

    XX_TEST_EXPECT_TRUE(io->deltaCount() > 0);
    if (io->deltaCount() > 0) {
        XX_TEST_EXPECT_EQ(io->deltaText(0), std::string("chan-reply"));
    }
    XX_TEST_EXPECT_TRUE(io->turnResultCount.load() > 0);
    // 显式关闭 transport: 打破与无限循环 recv 的 detached 协程之间的循环引用,
    // 使其 recv 返回 nullopt 退出, 避免持有 transport 泄漏
    io->transport()->close();
    co_await testSleep(ex, std::chrono::milliseconds{100});
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
            io::makeHelloAck(true, hello->value("sessionId", std::string{}), "", {})
        );
        uint64_t seq = 0;
        for (;;) {
            auto j = co_await wsRecvJson(ws);
            if (!j) {
                co_return;
            }
            if (io::msgType(*j) == io::MsgType::UserInput) {
                auto                  text = j->value("text", std::string{});
                agentxx::agent::Delta d;
                d.type = agentxx::agent::Delta::Type::TextToken;
                d.seq  = ++seq;
                d.text = "echo:" + text;
                co_await wsSendJson(ws, io::makeDeltaMsg(d));
                co_await wsSendJson(
                    ws,
                    io::makeTurnResult(j->value("sessionId", std::string{}), false, "", false)
                );
            } else if (io::msgType(*j) == io::MsgType::Ping) {
                co_await wsSendJson(ws, io::makePong(j->value("t", int64_t{0})));
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

    auto transport
        = std::make_shared<agentxx::agent::WsAgentIOTransport>(ex, url, "test-token", cfg, wsCfg);
    auto io = std::make_shared<TestIO>();
    io->setTransport(transport);

    agentxx::agent::WireHello hello{"session", "test-token", 0, ""};
    bool                      ok = co_await transport->connect(hello);
    XX_TEST_EXPECT_TRUE(ok);

    if (ok) {
        asio::co_spawn(ex, io->runTransportLoop(), asio::detached);

        for (int i = 0; i < 3; ++i) {
            io->sendToPeer(agentxx::agent::WireUserInput{"session", "m" + std::to_string(i)});
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

    auto tp      = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex, 8192);
    auto clientT = std::move(tp.first);
    auto serverT = std::move(tp.second);

    const int                numThreads = 4;
    const int                perThread  = 100;
    std::vector<std::thread> threads;
    auto                     clientPtr = clientT.get();
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([clientPtr, t, perThread]() {
            for (int i = 0; i < perThread; ++i) {
                agentxx::agent::WireUserInput msg{
                    "session",
                    std::to_string(t) + "-" + std::to_string(i)
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
        if (!msg) {
            break;
        }
        auto* ui = std::get_if<agentxx::agent::WireUserInput>(&*msg);
        if (ui) {
            seen.insert(ui->text);
        }
    }
    XX_TEST_EXPECT_EQ(seen.size(), size_t{numThreads * perThread});
    // 显式关闭: 使挂起的 recv 完成, 避免挂起协程持有 transport 泄漏
    clientT->close();
    serverT->close();
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
            io::makeHelloAck(true, hello->value("sessionId", std::string{}), "", {})
        );
        if (myConn <= dropTimes) {
            co_return;
        }
        for (;;) {
            auto j = co_await wsRecvJson(ws);
            if (!j) {
                co_return;
            }
            if (io::msgType(*j) == io::MsgType::UserInput) {
                co_await wsSendJson(
                    ws,
                    io::makeTurnResult(j->value("sessionId", std::string{}), false, "", false)
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

    auto transport
        = std::make_shared<agentxx::agent::WsAgentIOTransport>(ex, url, "test-token", cfg, wsCfg);
    auto io = std::make_shared<TestIO>();
    io->setTransport(transport);

    agentxx::agent::WireHello hello{"session", "test-token", 0, ""};
    bool                      ok = co_await transport->connect(hello);
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
            io::makeHelloAck(true, hello->value("sessionId", std::string{}), "", {})
        );
        for (;;) {
            auto j = co_await wsRecvJson(ws);
            if (!j) {
                co_return;
            }
            auto t = io::msgType(*j);
            if (t == io::MsgType::UserInput) {
                // 不立即回应, 等待 cancel
            } else if (t == io::MsgType::Cancel) {
                gotCancel.store(true);
                co_await wsSendJson(
                    ws,
                    io::makeTurnResult(j->value("sessionId", std::string{}), false, "", true)
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

    auto transport
        = std::make_shared<agentxx::agent::WsAgentIOTransport>(ex, url, "test-token", cfg, wsCfg);
    auto io = std::make_shared<TestIO>();
    io->setTransport(transport);

    agentxx::agent::WireHello hello{"session", "test-token", 0, ""};
    bool                      ok = co_await transport->connect(hello);
    XX_TEST_EXPECT_TRUE(ok);

    if (ok) {
        asio::co_spawn(ex, io->runTransportLoop(), asio::detached);

        io->sendToPeer(agentxx::agent::WireUserInput{"session", "hi"});
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
            io::makeHelloAck(true, hello->value("sessionId", std::string{}), "", {})
        );
        if (myConn == 1) {
            auto j = co_await wsRecvJson(ws);
            if (j) {
                agentxx::agent::Delta d;
                d.type = agentxx::agent::Delta::Type::TextToken;
                d.seq  = 1;
                d.text = "x";
                co_await wsSendJson(ws, io::makeDeltaMsg(d));
                co_await wsSendJson(
                    ws,
                    io::makeTurnResult(j->value("sessionId", std::string{}), false, "", false)
                );
            }
            co_return;
        }
        agentxx::agent::SyncPayload sp;
        sp.tailHash = "reconnect-sync";
        co_await wsSendJson(ws, io::makeSyncMsg(sp, 1));
        for (;;) {
            auto j = co_await wsRecvJson(ws);
            if (!j) {
                co_return;
            }
            if (io::msgType(*j) == io::MsgType::UserInput) {
                co_await wsSendJson(
                    ws,
                    io::makeTurnResult(j->value("sessionId", std::string{}), false, "", false)
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

    auto transport
        = std::make_shared<agentxx::agent::WsAgentIOTransport>(ex, url, "test-token", cfg, wsCfg);
    auto io = std::make_shared<TestIO>();
    io->setTransport(transport);

    agentxx::agent::WireHello hello{"session", "test-token", 0, ""};
    bool                      ok = co_await transport->connect(hello);
    XX_TEST_EXPECT_TRUE(ok);

    asio::co_spawn(ex, io->runTransportLoop(), asio::detached);

    io->sendToPeer(agentxx::agent::WireUserInput{"session", "m1"});

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
            neograph_asio_error_code ec;
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

    auto transport
        = std::make_shared<agentxx::agent::WsAgentIOTransport>(ex, url, "test-token", cfg, wsCfg);

    auto                      start = std::chrono::steady_clock::now();
    agentxx::agent::WireHello hello{"session", "test-token", 0, ""};
    bool                      ok      = co_await transport->connect(hello);
    auto                      elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
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

    auto tp      = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);
    auto clientT = std::move(tp.first);
    auto serverT = std::move(tp.second);

    // 模拟 AgentServer 鉴权逻辑
    asio::co_spawn(
        ex,
        [st = std::move(serverT)]() mutable -> asio::awaitable<void> {
            auto msg = co_await st->recv();
            if (!msg) {
                co_return;
            }
            auto* hello = std::get_if<agentxx::agent::WireHello>(&*msg);
            if (!hello) {
                co_return;
            }
            bool authOk = (hello->token == "secret");
            st->send(agentxx::agent::WireMessage{
                agentxx::agent::WireHelloAck{authOk, hello->sessionId, "", {}}
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
    // 显式关闭: 使 detached 协程中挂起的 recv 完成退出, 避免持有 transport 泄漏
    clientT->close();
    co_return;
}

// ---------------------------------------------------------------------------
// 19. SessionServerAgentIO: switchSession 切换会话
//     (回推全量 Sync + 模型信息 + 上下文统计; 运行态拒绝切换; 同会话校准)
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_session_controller_switch_session() {
    auto ex = co_await asio::this_coro::executor;

    auto tp      = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);
    auto clientT = std::move(tp.first);
    auto serverT = std::move(tp.second);

    // 最小 BaseAgent (无需 init: switchSession 仅依赖 agentContext/SessionStore;
    // getCurrentModelName 在 modelRegistry 为空时回退 agentConfig->model)
    auto cfg             = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl   = "http://127.0.0.1:1";
    cfg->model.modelName = "test-model";
    auto agent           = std::make_shared<agentxx::agent::BaseAgent>(cfg);

    // 预置目标会话历史 (io 线程未绑定, assertIoThread 为 no-op)
    auto target = agent->agentContext->getSession("target-session");
    target->appendViewMessage(
        agentxx::agent::ViewMessage::makeText(agentxx::agent::ViewMessage::Role::User, "hello")
    );

    SessionServerAgentIO::Config scCfg;
    scCfg.sessionId = "session-a";
    auto sc         = std::make_shared<SessionServerAgentIO>(ex, agent, scCfg);
    sc->setTransport(std::shared_ptr<agentxx::agent::AgentIOTransportBase>(std::move(serverT)));

    // ---- 正常切换: 回推全量 Sync + 模型信息 + 上下文统计 ----
    sc->switchSession("target-session");
    XX_TEST_EXPECT_EQ(std::string{sc->sessionId()}, std::string("target-session"));

    auto syncMsg = co_await clientT->recv();
    XX_TEST_EXPECT_TRUE(syncMsg.has_value());
    if (syncMsg) {
        auto* sp = std::get_if<agentxx::agent::SyncPayload>(&*syncMsg);
        XX_TEST_EXPECT_TRUE(sp != nullptr);
        if (sp) {
            XX_TEST_EXPECT_EQ(sp->messages.size(), size_t{1});
            if (!sp->messages.empty()) {
                XX_TEST_EXPECT_TRUE(
                    sp->messages[0].role == agentxx::agent::ViewMessage::Role::User
                );
                XX_TEST_EXPECT_EQ(sp->messages[0].text, std::string("hello"));
            }
        }
    }
    auto modelMsg = co_await clientT->recv();
    XX_TEST_EXPECT_TRUE(modelMsg.has_value());
    if (modelMsg) {
        auto* mi = std::get_if<agentxx::agent::WireModelInfo>(&*modelMsg);
        XX_TEST_EXPECT_TRUE(mi != nullptr);
        if (mi) {
            XX_TEST_EXPECT_EQ(mi->currentModel, std::string("test-model"));
        }
    }
    auto statsMsg = co_await clientT->recv();
    XX_TEST_EXPECT_TRUE(statsMsg.has_value());
    if (statsMsg) {
        XX_TEST_EXPECT_TRUE(std::get_if<agentxx::agent::WireContextStats>(&*statsMsg) != nullptr);
    }

    // ---- 运行态拒绝切换 (客户端已前置拦截, 服务端兜底; 不产生任何消息) ----
    sc->setTurnActiveForTest(true);
    sc->switchSession("another-session");
    XX_TEST_EXPECT_EQ(std::string{sc->sessionId()}, std::string("target-session"));
    sc->setTurnActiveForTest(false);

    // ---- 切换到同一会话: sessionId 不变, 仅回推校准 Sync + 统计 ----
    sc->switchSession("target-session");
    auto reSyncMsg = co_await clientT->recv();
    XX_TEST_EXPECT_TRUE(reSyncMsg.has_value());
    if (reSyncMsg) {
        XX_TEST_EXPECT_TRUE(std::get_if<agentxx::agent::SyncPayload>(&*reSyncMsg) != nullptr);
    }
    auto reStatsMsg = co_await clientT->recv();
    XX_TEST_EXPECT_TRUE(reStatsMsg.has_value());
    if (reStatsMsg) {
        XX_TEST_EXPECT_TRUE(std::get_if<agentxx::agent::WireContextStats>(&*reStatsMsg) != nullptr);
    }

    // ---- 空 sessionId 非法: 直接忽略 ----
    sc->switchSession("");
    XX_TEST_EXPECT_EQ(std::string{sc->sessionId()}, std::string("target-session"));

    // 显式关闭: 使挂起的 recv 完成, 避免挂起协程持有 transport 泄漏
    clientT->close();
    sc->stop();
    co_return;
}

// ---------------------------------------------------------------------------
// 20. TUI 切模型随下一条用户消息携带 (WireUserInput.model):
//     TUI 不再直接通知 agent-io 切换 (WireSelectModel), 而是把选择的模型随
//     下一次发送的用户消息携带; SessionServerAgentIO 记录待应用模型并传给
//     runTurnAsync, BaseAgent 执行新一轮会话 (runTurnAsync 开头 selectModel)
//     时自动切换, 后续轮次沿用该模型 (会话级持久)
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_model_switch_with_next_input() {
    auto ex = co_await asio::this_coro::executor;

    // 本地 LLM 模拟器: 记录最近一次 /chat/completions 请求体 (含 model 字段),
    // 可用于断言实际 LLM API 调用使用的模型
    auto                          sim     = startDaSimServer();
    const auto                    baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);
    g_da_sim_response_content              = "hello from model switch test";
    g_da_sim_tool_calls                    = neograph::json::array();

    auto cfg             = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl   = baseUrl;
    cfg->model.apiKey    = "EMPTY";
    cfg->model.modelName = "default-model";
    auto agent           = std::make_shared<agentxx::agent::BaseAgent>(cfg);
    co_await agent->init();

    // 注册第二个可用模型: 注册表 key="model-b", 发送给 API 的字段 "model-b-api"
    agentxx::agent::ModelConfig second;
    second.name      = "model-b";
    second.modelName = "model-b-api";
    second.baseUrl   = baseUrl;
    second.apiKey    = "EMPTY";
    agent->agentContext->modelRegistry->registerModel("model-b", second);

    // 通道直连: client 端模拟 TUI 发送 (WireUserInput), server 端为会话控制器
    auto tp      = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);
    auto clientT = std::move(tp.first);
    auto serverT = std::move(tp.second);

    agentxx::agent::SessionServerAgentIO::Config scCfg;
    scCfg.sessionId = "model-switch-session";
    auto sc         = std::make_shared<agentxx::agent::SessionServerAgentIO>(ex, agent, scCfg);
    sc->setTransport(std::shared_ptr<agentxx::agent::AgentIOTransportBase>(std::move(serverT)));

    // transport 接收循环 (onPeerMessage 消费客户端消息) + 会话驱动循环 (run)
    asio::co_spawn(
        ex,
        [sc]() -> asio::awaitable<void> {
            co_await sc->runTransportLoop();
        },
        asio::detached
    );
    asio::co_spawn(
        ex,
        [sc]() -> asio::awaitable<void> {
            co_await sc->run();
        },
        asio::detached
    );

    // ---- 首条消息携带模型 "model-b": 该轮会话开始时自动切换 ----
    const int req0 = g_da_sim_request_count;
    clientT->send(agentxx::agent::WireMessage{agentxx::agent::WireUserInput{
        "model-switch-session",
        "switch to model-b",
        "model-b",
    }});

    // 等待会话模型切换生效 (selectModel 在 runTurnAsync 开头同步执行)
    bool switched = false;
    for (int i = 0; i < 200; ++i) {
        if (agent->getCurrentModelName("model-switch-session") == "model-b") {
            switched = true;
            break;
        }
        co_await testSleep(ex, std::chrono::milliseconds{50});
    }
    XX_TEST_EXPECT_TRUE(switched);

    // 等待 LLM API 请求到达: 实际调用应使用切换后的模型 (model-b-api)
    bool sawRequest = false;
    for (int i = 0; i < 200; ++i) {
        if (g_da_sim_request_count > req0) {
            sawRequest = true;
            break;
        }
        co_await testSleep(ex, std::chrono::milliseconds{50});
    }
    XX_TEST_EXPECT_TRUE(sawRequest);
    if (sawRequest) {
        XX_TEST_EXPECT_EQ(
            g_da_sim_last_request.value("model", std::string{}),
            std::string("model-b-api")
        );
    }

    // ---- 第二条消息不携带模型: 沿用会话当前模型 (model-b), 不切回默认 ----
    const int req1 = g_da_sim_request_count;
    clientT->send(agentxx::agent::WireMessage{agentxx::agent::WireUserInput{
        "model-switch-session",
        "keep model-b",
        "",
    }});
    bool secondRequest = false;
    for (int i = 0; i < 200; ++i) {
        if (g_da_sim_request_count > req1) {
            secondRequest = true;
            break;
        }
        co_await testSleep(ex, std::chrono::milliseconds{50});
    }
    XX_TEST_EXPECT_TRUE(secondRequest);
    if (secondRequest) {
        XX_TEST_EXPECT_EQ(
            agent->getCurrentModelName("model-switch-session"),
            std::string("model-b")
        );
        XX_TEST_EXPECT_EQ(
            g_da_sim_last_request.value("model", std::string{}),
            std::string("model-b-api")
        );
    }

    // ---- 未注册的模型名: selectModel 拒绝 (会话模型保持不变) ----
    const int req2 = g_da_sim_request_count;
    clientT->send(agentxx::agent::WireMessage{agentxx::agent::WireUserInput{
        "model-switch-session",
        "invalid model name",
        "no-such-model",
    }});
    bool thirdRequest = false;
    for (int i = 0; i < 200; ++i) {
        if (g_da_sim_request_count > req2) {
            thirdRequest = true;
            break;
        }
        co_await testSleep(ex, std::chrono::milliseconds{50});
    }
    XX_TEST_EXPECT_TRUE(thirdRequest);
    if (thirdRequest) {
        // 拒绝无效模型: 会话模型仍为 model-b, LLM 仍用 model-b-api
        XX_TEST_EXPECT_EQ(
            agent->getCurrentModelName("model-switch-session"),
            std::string("model-b")
        );
        XX_TEST_EXPECT_EQ(
            g_da_sim_last_request.value("model", std::string{}),
            std::string("model-b-api")
        );
    }

    // 显式关闭: 使挂起的 recv 完成, 避免挂起协程持有 transport 泄漏
    clientT->close();
    sc->stop();
    sim.stop();
    co_return;
}

// ---------------------------------------------------------------------------
// 22. viewMessages 历史分页: 尾窗 hello 同步 + WireGetViewMessages 分页拉取
//     (initialSyncTailCount>0 时首次接入仅同步末尾窗口; 客户端按绝对下标
//      向上分页拉取更早历史; beforeIndex==0 兜底从末尾取; count==0 用默认页大小;
//      viewMessages append-only 保证绝对下标恒定)
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_view_messages_pagination() {
    auto ex = co_await asio::this_coro::executor;

    auto tp      = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);
    auto clientT = std::move(tp.first);
    auto serverT = std::move(tp.second);

    // 最小 BaseAgent + 预置 250 条历史消息 (msg_000001..msg_000250 / m1..m250)
    auto cfg             = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl   = "http://127.0.0.1:1";
    cfg->model.modelName = "test-model";
    auto agent           = std::make_shared<agentxx::agent::BaseAgent>(cfg);

    const size_t kTotal = 250;
    auto         sess   = agent->agentContext->getSession("pag-session");
    for (size_t i = 1; i <= kTotal; ++i) {
        sess->appendViewMessage(agentxx::agent::ViewMessage::makeText(
            agentxx::agent::ViewMessage::Role::User,
            "m" + std::to_string(i)
        ));
    }

    SessionServerAgentIO::Config scCfg;
    scCfg.sessionId            = "pag-session";
    scCfg.initialSyncTailCount = 100;
    auto sc                    = std::make_shared<SessionServerAgentIO>(ex, agent, scCfg);
    sc->setTransport(std::shared_ptr<agentxx::agent::AgentIOTransportBase>(std::move(serverT)));

    // ---- 首次接入 (lastSeq=0): 尾窗 sync [150, 250) ----
    agentxx::agent::WireHello hello{"pag-session", "", 0, ""};
    sc->handleHello(hello);

    auto ackMsg = co_await clientT->recv();
    XX_TEST_EXPECT_TRUE(ackMsg.has_value());
    if (ackMsg) {
        auto* ack = std::get_if<agentxx::agent::WireHelloAck>(&*ackMsg);
        XX_TEST_EXPECT_TRUE(ack != nullptr);
        if (ack) {
            XX_TEST_EXPECT_TRUE(ack->ok);
        }
    }
    auto syncMsg = co_await clientT->recv();
    XX_TEST_EXPECT_TRUE(syncMsg.has_value());
    uint64_t windowStart = 0;
    if (syncMsg) {
        auto* sp = std::get_if<agentxx::agent::SyncPayload>(&*syncMsg);
        XX_TEST_EXPECT_TRUE(sp != nullptr);
        if (sp) {
            XX_TEST_EXPECT_EQ(sp->messages.size(), size_t{100});
            XX_TEST_EXPECT_EQ(sp->fromIndex, uint64_t{150});
            XX_TEST_EXPECT_EQ(sp->totalMessages, static_cast<uint64_t>(kTotal));
            windowStart = sp->fromIndex;
            if (sp->messages.size() == 100) {
                // 尾窗内容: msg_000151..msg_000250
                XX_TEST_EXPECT_EQ(sp->messages.front().id, std::string("msg_000151"));
                XX_TEST_EXPECT_EQ(sp->messages.back().id, std::string("msg_000250"));
            }
        }
    }
    // handleHello 尾部: ContextStats (无 pending interrupt)
    auto statsMsg = co_await clientT->recv();
    XX_TEST_EXPECT_TRUE(statsMsg.has_value());
    if (statsMsg) {
        XX_TEST_EXPECT_TRUE(std::get_if<agentxx::agent::WireContextStats>(&*statsMsg) != nullptr);
    }

    // ---- 分页请求: 窗口上方 [50, 150) ----
    sc->onPeerMessage(agentxx::agent::WireMessage{
        agentxx::agent::WireGetViewMessages{"pag-session", windowStart, 100}
    });
    auto page1 = co_await clientT->recv();
    XX_TEST_EXPECT_TRUE(page1.has_value());
    if (page1) {
        auto* pg = std::get_if<agentxx::agent::WireViewMessagesPage>(&*page1);
        XX_TEST_EXPECT_TRUE(pg != nullptr);
        if (pg) {
            XX_TEST_EXPECT_EQ(pg->sessionId, std::string("pag-session"));
            XX_TEST_EXPECT_EQ(pg->startIndex, uint64_t{50});
            XX_TEST_EXPECT_EQ(pg->totalCount, static_cast<uint64_t>(kTotal));
            XX_TEST_EXPECT_EQ(pg->messages.size(), size_t{100});
            if (pg->messages.size() == 100) {
                // 绝对下标连续: [50,150) ↔ msg_000051..msg_000150
                XX_TEST_EXPECT_EQ(pg->messages.front().id, std::string("msg_000051"));
                XX_TEST_EXPECT_EQ(pg->messages.back().id, std::string("msg_000150"));
                XX_TEST_EXPECT_EQ(pg->messages.front().text, std::string("m51"));
            }
        }
    }

    // ---- 分页请求: 剩余头部不足一页时截断 [0, 50) ----
    sc->onPeerMessage(agentxx::agent::WireMessage{
        agentxx::agent::WireGetViewMessages{"pag-session", 50, 100}
    });
    auto page2 = co_await clientT->recv();
    XX_TEST_EXPECT_TRUE(page2.has_value());
    if (page2) {
        auto* pg = std::get_if<agentxx::agent::WireViewMessagesPage>(&*page2);
        XX_TEST_EXPECT_TRUE(pg != nullptr);
        if (pg) {
            XX_TEST_EXPECT_EQ(pg->startIndex, uint64_t{0});
            XX_TEST_EXPECT_EQ(pg->totalCount, static_cast<uint64_t>(kTotal));
            XX_TEST_EXPECT_EQ(pg->messages.size(), size_t{50});
            if (pg->messages.size() == 50) {
                XX_TEST_EXPECT_EQ(pg->messages.front().id, std::string("msg_000001"));
                XX_TEST_EXPECT_EQ(pg->messages.back().id, std::string("msg_000050"));
            }
        }
    }

    // ---- beforeIndex == 0 兜底语义: 从末尾向前取 count 条 ----
    sc->onPeerMessage(agentxx::agent::WireMessage{
        agentxx::agent::WireGetViewMessages{"pag-session", 0, 3}
    });
    auto page3 = co_await clientT->recv();
    XX_TEST_EXPECT_TRUE(page3.has_value());
    if (page3) {
        auto* pg = std::get_if<agentxx::agent::WireViewMessagesPage>(&*page3);
        XX_TEST_EXPECT_TRUE(pg != nullptr);
        if (pg) {
            XX_TEST_EXPECT_EQ(pg->startIndex, static_cast<uint64_t>(kTotal - 3));
            XX_TEST_EXPECT_EQ(pg->messages.size(), size_t{3});
            if (pg->messages.size() == 3) {
                XX_TEST_EXPECT_EQ(pg->messages.back().text, std::string("m250"));
            }
        }
    }

    // ---- count == 0: 使用服务端默认页大小 (100) ----
    sc->onPeerMessage(agentxx::agent::WireMessage{
        agentxx::agent::WireGetViewMessages{"pag-session", 200, 0}
    });
    auto page4 = co_await clientT->recv();
    XX_TEST_EXPECT_TRUE(page4.has_value());
    if (page4) {
        auto* pg = std::get_if<agentxx::agent::WireViewMessagesPage>(&*page4);
        XX_TEST_EXPECT_TRUE(pg != nullptr);
        if (pg) {
            XX_TEST_EXPECT_EQ(pg->startIndex, uint64_t{100});
            XX_TEST_EXPECT_EQ(pg->messages.size(), size_t{100});
        }
    }

    // ---- 越界 beforeIndex 收敛到总数; 不存在的会话回空页 ----
    sc->onPeerMessage(agentxx::agent::WireMessage{
        agentxx::agent::WireGetViewMessages{"pag-session", 99999, 10}
    });
    auto page5 = co_await clientT->recv();
    XX_TEST_EXPECT_TRUE(page5.has_value());
    if (page5) {
        auto* pg = std::get_if<agentxx::agent::WireViewMessagesPage>(&*page5);
        XX_TEST_EXPECT_TRUE(pg != nullptr);
        if (pg) {
            XX_TEST_EXPECT_EQ(pg->startIndex, static_cast<uint64_t>(kTotal - 10));
            XX_TEST_EXPECT_EQ(pg->messages.size(), size_t{10});
        }
    }
    sc->onPeerMessage(agentxx::agent::WireMessage{
        agentxx::agent::WireGetViewMessages{"no-such-session", 10, 5}
    });
    auto page6 = co_await clientT->recv();
    XX_TEST_EXPECT_TRUE(page6.has_value());
    if (page6) {
        auto* pg = std::get_if<agentxx::agent::WireViewMessagesPage>(&*page6);
        XX_TEST_EXPECT_TRUE(pg != nullptr);
        if (pg) {
            XX_TEST_EXPECT_TRUE(pg->messages.empty());
            XX_TEST_EXPECT_EQ(pg->totalCount, uint64_t{0});
        }
    }

    clientT->close();
    sc->stop();
    co_return;
}

// ---------------------------------------------------------------------------
// 23. 历史分页 wire 序列化 roundtrip (WS JSON):
//     GetViewMessages / ViewMessagesPage / Sync.totalMessages 字段保真
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_wire_pagination_roundtrip() {
    using agentxx::agent::WsAgentIOTransport;

    // ---- WireGetViewMessages roundtrip ----
    {
        auto jsonText = WsAgentIOTransport::serialize(
            agentxx::agent::WireGetViewMessages{"sess-a", 150, 100}
        );
        auto msg = WsAgentIOTransport::deserialize(jsonText);
        XX_TEST_EXPECT_TRUE(msg.has_value());
        if (msg) {
            auto* req = std::get_if<agentxx::agent::WireGetViewMessages>(&*msg);
            XX_TEST_EXPECT_TRUE(req != nullptr);
            if (req) {
                XX_TEST_EXPECT_EQ(req->sessionId, std::string("sess-a"));
                XX_TEST_EXPECT_EQ(req->beforeIndex, uint64_t{150});
                XX_TEST_EXPECT_EQ(req->count, uint32_t{100});
            }
        }
    }

    // ---- WireViewMessagesPage roundtrip (含 role 专属子结构保真) ----
    {
        agentxx::agent::WireViewMessagesPage page;
        page.sessionId  = "sess-b";
        page.startIndex = 42;
        page.totalCount = 250;

        auto user     = agentxx::agent::ViewMessage::makeText(
            agentxx::agent::ViewMessage::Role::User, "hello"
        );
        user.id       = "msg_000043";
        page.messages.push_back(user);

        auto tool               = agentxx::agent::ViewMessage::makeText(
            agentxx::agent::ViewMessage::Role::Tool, "{\"path\":\"a.txt\"}"
        );
        tool.id                 = "msg_000044";
        tool.tool               = agentxx::agent::ViewMessage::ToolData{};
        tool.tool->toolName     = "read";
        tool.tool->toolCallId   = "tc-1";
        tool.tool->toolResult   = "content";
        tool.tool->toolFinished = true;
        page.messages.push_back(tool);

        auto tip          = agentxx::agent::ViewMessage::makeText(
            agentxx::agent::ViewMessage::Role::Tip, "warn"
        );
        tip.id            = "msg_000045";
        tip.tip->tipLevel = agentxx::agent::ViewMessage::TipLevel::Warning;
        page.messages.push_back(tip);

        auto jsonText = WsAgentIOTransport::serialize(agentxx::agent::WireMessage{page});
        auto msg      = WsAgentIOTransport::deserialize(jsonText);
        XX_TEST_EXPECT_TRUE(msg.has_value());
        if (msg) {
            auto* back = std::get_if<agentxx::agent::WireViewMessagesPage>(&*msg);
            XX_TEST_EXPECT_TRUE(back != nullptr);
            if (back) {
                XX_TEST_EXPECT_EQ(back->sessionId, std::string("sess-b"));
                XX_TEST_EXPECT_EQ(back->startIndex, uint64_t{42});
                XX_TEST_EXPECT_EQ(back->totalCount, uint64_t{250});
                XX_TEST_EXPECT_EQ(back->messages.size(), size_t{3});
                if (back->messages.size() == 3) {
                    XX_TEST_EXPECT_EQ(back->messages[0].id, std::string("msg_000043"));
                    XX_TEST_EXPECT_TRUE(back->messages[1].tool.has_value());
                    if (back->messages[1].tool) {
                        XX_TEST_EXPECT_EQ(back->messages[1].tool->toolName, std::string("read"));
                        XX_TEST_EXPECT_TRUE(back->messages[1].tool->toolFinished);
                    }
                    XX_TEST_EXPECT_TRUE(back->messages[2].tip.has_value());
                    if (back->messages[2].tip) {
                        XX_TEST_EXPECT_TRUE(
                            back->messages[2].tip->tipLevel
                            == agentxx::agent::ViewMessage::TipLevel::Warning
                        );
                    }
                }
            }
        }
    }

    // ---- SyncPayload totalMessages / fromIndex roundtrip ----
    {
        agentxx::agent::SyncPayload sync;
        sync.fromIndex     = 150;
        sync.totalMessages = 250;
        sync.messages.push_back(agentxx::agent::ViewMessage::makeText(
            agentxx::agent::ViewMessage::Role::User, "tail"
        ));
        auto jsonText = WsAgentIOTransport::serialize(agentxx::agent::WireMessage{sync});
        auto msg      = WsAgentIOTransport::deserialize(jsonText);
        XX_TEST_EXPECT_TRUE(msg.has_value());
        if (msg) {
            auto* back = std::get_if<agentxx::agent::SyncPayload>(&*msg);
            XX_TEST_EXPECT_TRUE(back != nullptr);
            if (back) {
                XX_TEST_EXPECT_EQ(back->fromIndex, uint64_t{150});
                XX_TEST_EXPECT_EQ(back->totalMessages, uint64_t{250});
                XX_TEST_EXPECT_EQ(back->messages.size(), size_t{1});
            }
        }
    }
    co_return;
}

// ---------------------------------------------------------------------------
// 22. 服务端消息队列与调度: 排队、成功自动连续执行、取消暂停、插队立即执行
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_session_controller_message_queue() {
    auto ex = co_await asio::this_coro::executor;

    auto       sim     = startDaSimServer();
    const auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);
    g_da_sim_response_content = "echo response from queue test";
    g_da_sim_tool_calls       = neograph::json::array();

    auto cfg             = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl   = baseUrl;
    cfg->model.apiKey    = "EMPTY";
    cfg->model.modelName = "default-model";

    auto agent = std::make_shared<agentxx::agent::BaseAgent>(cfg);
    co_await agent->init();

    agentxx::agent::SessionServerAgentIO::Config scCfg;
    scCfg.sessionId = "queue-test-session";

    auto sc = std::make_shared<agentxx::agent::SessionServerAgentIO>(ex, agent, scCfg);

    auto [clientT, serverT] = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);
    sc->setTransport(std::shared_ptr<agentxx::agent::AgentIOTransportBase>(std::move(serverT)));

    std::vector<agentxx::agent::WireMessageQueueUpdate> queueUpdates;
    std::vector<agentxx::agent::WireTurnResult>         turnResults;
    std::mutex                                          mu;

    auto clientLoop = [&]() -> asio::awaitable<void> {
        while (clientT->alive()) {
            auto msg = co_await clientT->recv();
            if (!msg) {
                break;
            }
            std::visit(
                [&](auto&& m) {
                    using T = std::decay_t<decltype(m)>;
                    if constexpr (std::is_same_v<T, agentxx::agent::WireMessageQueueUpdate>) {
                        std::lock_guard<std::mutex> lk(mu);
                        queueUpdates.push_back(m);
                    } else if constexpr (std::is_same_v<T, agentxx::agent::WireTurnResult>) {
                        std::lock_guard<std::mutex> lk(mu);
                        turnResults.push_back(m);
                    }
                },
                *msg
            );
        }
    };

    asio::co_spawn(ex, clientLoop(), asio::detached);
    asio::co_spawn(
        ex,
        [sc]() -> asio::awaitable<void> {
            co_await sc->runTransportLoop();
        },
        asio::detached
    );
    asio::co_spawn(
        ex,
        [sc]() -> asio::awaitable<void> {
            co_await sc->run();
        },
        asio::detached
    );

    // 发送两轮输入 (第一轮立即执行，第二轮进入队列)
    clientT->send(agentxx::agent::WireMessage{agentxx::agent::WireUserInput{
        "queue-test-session", "turn 1", ""
    }});
    clientT->send(agentxx::agent::WireMessage{agentxx::agent::WireUserInput{
        "queue-test-session", "turn 2", ""
    }});

    // 等待两轮均执行完毕 (因为第一轮成功，自动执行第二轮)
    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (turnResults.size() >= 2) {
                break;
            }
        }
        co_await testSleep(ex, std::chrono::milliseconds{50});
    }

    {
        std::lock_guard<std::mutex> lk(mu);
        XX_TEST_EXPECT_EQ(turnResults.size(), size_t{2});
        if (turnResults.size() >= 2) {
            XX_TEST_EXPECT_TRUE(!turnResults[0].hasError && !turnResults[0].interrupted);
            XX_TEST_EXPECT_TRUE(!turnResults[1].hasError && !turnResults[1].interrupted);
        }
    }

    // 取消后暂停自动调度：发送 turn 3 和 turn 4，发送取消
    clientT->send(agentxx::agent::WireMessage{agentxx::agent::WireUserInput{
        "queue-test-session", "turn 3", ""
    }});
    clientT->send(agentxx::agent::WireMessage{agentxx::agent::WireUserInput{
        "queue-test-session", "turn 4", ""
    }});
    clientT->send(agentxx::agent::WireMessage{agentxx::agent::WireCancel{
        "queue-test-session"
    }});

    // 等待 turn 3 结束
    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (turnResults.size() >= 3) {
                break;
            }
        }
        co_await testSleep(ex, std::chrono::milliseconds{50});
    }

    co_await testSleep(ex, std::chrono::milliseconds{200});

    // 验证 turn 3 被中断，且 turn 4 并没有自动执行 (turnResults.size() 仍为 3)
    {
        std::lock_guard<std::mutex> lk(mu);
        XX_TEST_EXPECT_EQ(turnResults.size(), size_t{3});
        if (turnResults.size() >= 3) {
            XX_TEST_EXPECT_TRUE(turnResults[2].interrupted || turnResults[2].hasError);
        }
    }

    // 点击 insert (WireInterruptAndRunNext) 唤醒执行 turn 4
    clientT->send(agentxx::agent::WireMessage{agentxx::agent::WireInterruptAndRunNext{
        "queue-test-session"
    }});

    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (turnResults.size() >= 4) {
                break;
            }
        }
        co_await testSleep(ex, std::chrono::milliseconds{50});
    }

    {
        std::lock_guard<std::mutex> lk(mu);
        XX_TEST_EXPECT_EQ(turnResults.size(), size_t{4});
        if (turnResults.size() >= 4) {
            XX_TEST_EXPECT_TRUE(!turnResults[3].hasError && !turnResults[3].interrupted);
        }
    }

    clientT->close();
    sc->stop();
    sim.stop();
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

    std::cout << "  [remote] run transport loop replace transport..." << std::endl;
    co_await test_run_transport_loop_replace_transport();

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

    std::cout << "  [remote] session controller switch session..." << std::endl;
    co_await test_session_controller_switch_session();

    std::cout << "  [remote] model switch with next input..." << std::endl;
    co_await test_model_switch_with_next_input();

    std::cout << "  [remote] session controller message queue..." << std::endl;
    co_await test_session_controller_message_queue();

    std::cout << "  [remote] view messages pagination..." << std::endl;
    co_await test_view_messages_pagination();

    std::cout << "  [remote] wire pagination roundtrip..." << std::endl;
    co_await test_wire_pagination_roundtrip();

    co_return TestResult{g_remote_passed, g_remote_failed};
}

} // namespace test
} // namespace agentxx
