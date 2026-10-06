#include "agentxx-test/core/test_input_delivery.h"
#include "agentxx-test/core/test_agent.h" // 本地 LLM 模拟器 startDaSimServer/g_da_sim_*
#include "agentxx/agent/base_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/channel_io_transport.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/agent/session_store.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_id_passed = 0;
int g_id_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_id_passed
#define XX_TEST_FAILED g_id_failed

namespace agentxx {
namespace test {

using namespace utilxx_base;
namespace fs = std::filesystem;

namespace {

/// 客户端收到的消息记录 (由测试协程从传输层读入)
struct ClientRecorder {
    struct Ack {
        uint64_t    requestId = 0;
        std::string status;
        std::string reason;
        std::string detail;
        std::string itemId;
        std::string delivery;
    };

    std::mutex                                          mu;
    std::vector<Ack>                                    acks;
    std::vector<agentxx::agent::WireError>              errors;
    std::vector<agentxx::agent::WireMessageQueueUpdate> queues;
    std::vector<agentxx::agent::WireDelta>              deltas;
    std::vector<agentxx::agent::WireTurnResult>         turns;
    std::atomic<int>                                    turnResults{0};

    void handle(agentxx::agent::WireMessage msg) {
        std::visit(
            [this](auto&& m) {
                using T = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<T, agentxx::agent::WireInputAck>) {
                    std::lock_guard<std::mutex> lock(mu);
                    acks.push_back(Ack{
                        m.requestId,
                        m.status,
                        m.reason,
                        m.detail,
                        m.itemId,
                        m.delivery
                    });
                } else if constexpr (std::is_same_v<T, agentxx::agent::WireError>) {
                    std::lock_guard<std::mutex> lock(mu);
                    errors.push_back(m);
                } else if constexpr (std::is_same_v<T, agentxx::agent::WireMessageQueueUpdate>) {
                    std::lock_guard<std::mutex> lock(mu);
                    queues.push_back(m);
                } else if constexpr (std::is_same_v<T, agentxx::agent::WireDelta>) {
                    std::lock_guard<std::mutex> lock(mu);
                    deltas.push_back(m);
                } else if constexpr (std::is_same_v<T, agentxx::agent::WireTurnResult>) {
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        turns.push_back(m);
                    }
                    turnResults.fetch_add(1);
                }
            },
            std::move(msg)
        );
    }

    std::optional<Ack> ack(size_t index) {
        std::lock_guard<std::mutex> lock(mu);
        return index < acks.size() ? std::optional<Ack>{acks[index]} : std::nullopt;
    }

    size_t ackCount() {
        std::lock_guard<std::mutex> lock(mu);
        return acks.size();
    }

    size_t errorCount() {
        std::lock_guard<std::mutex> lock(mu);
        return errors.size();
    }

    std::string lastQueueState() {
        std::lock_guard<std::mutex> lock(mu);
        return queues.empty() ? std::string{} : queues.back().state;
    }

    bool lastQueueRecovered() {
        std::lock_guard<std::mutex> lock(mu);
        if (queues.empty()) {
            return false;
        }
        for (const auto& item : queues.back().items) {
            if (item.recovered) {
                return true;
            }
        }
        return false;
    }

    /// 最近一次轮次结果的错误文本 (诊断用; 无结果时返回 "<no turn>")
    std::string lastTurnError() {
        std::lock_guard<std::mutex> lock(mu);
        return turns.empty() ? std::string{"<no turn>"} : turns.back().errorMessage;
    }

    /// 是否收到过包含指定文本的展示消息增量 (InsertMessage/TurnStart)
    bool sawViewMessageWithText(std::string_view text) {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto& d : deltas) {
            if (d.message && d.message->text.find(text) != std::string::npos) {
                return true;
            }
            if (d.text.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

/// 等待条件成立 (轮转 io 线程: 每 15ms 轮询一次, 不用阻塞 sleep)
asio::awaitable<bool>
    waitFor(std::function<bool()> cond, int timeoutMs = 8000, int pollMs = 15) {
    auto ex       = co_await asio::this_coro::executor;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMs};
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            co_return true;
        }
        asio::steady_timer t(ex);
        t.expires_after(std::chrono::milliseconds{pollMs});
        utilxx_base::AsioErrorCode ec;
        co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }
    co_return cond();
}

/// 非阻塞等待 (仅推进时间, 用于"某事件不应发生"的用例)
asio::awaitable<void> spin(std::chrono::milliseconds total) {
    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer t(ex);
    t.expires_after(total);
    utilxx_base::AsioErrorCode ec;
    co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
}

/// 测试夹具: 本地 LLM 模拟器 + BaseAgent + 会话端点 + 客户端传输
struct Fixture {
    DaSimServer                                              sim;
    std::shared_ptr<agentxx::agent::AgentConfig>             cfg;
    std::shared_ptr<agentxx::agent::BaseAgent>               agent;
    std::shared_ptr<agentxx::agent::SessionServerAgentIO>    endpoint;
    std::shared_ptr<ClientRecorder>                          recorder;
    std::shared_ptr<agentxx::agent::ChannelAgentIOTransport> clientT;
    std::string                                              sessionId;

    ~Fixture() {
        if (clientT) {
            clientT->close();
        }
        if (endpoint) {
            endpoint->stop();
        }
        sim.stop();
    }

    /// 发送用户输入 (投递模式/请求序号可选)
    void send(std::string text, std::string delivery = {}, uint64_t requestId = 0) {
        agentxx::agent::WireUserInput input;
        input.sessionId = sessionId;
        input.text      = std::move(text);
        input.delivery  = std::move(delivery);
        input.requestId = requestId;
        clientT->send(agentxx::agent::WireMessage{std::move(input)});
    }
};

/// 创建夹具 (可选持久化数据目录)
asio::awaitable<std::shared_ptr<Fixture>> makeFixture(
    std::string sessionId,
    std::string dataDir  = {},
    std::string response = "ok response"
) {
    auto fx              = std::make_shared<Fixture>();
    fx->sim              = startDaSimServer();
    g_da_sim_response_content = std::move(response);
    g_da_sim_tool_calls       = utilxx_base::Json::array();
    g_da_sim_fail_count       = 0;
    g_da_sim_delay_ms         = 0;
    g_da_sim_tool_calls_remaining = -1;

    const auto baseUrl       = "http://127.0.0.1:" + std::to_string(fx->sim.port);
    fx->cfg                  = std::make_shared<agentxx::agent::AgentConfig>();
    fx->cfg->model.baseUrl   = baseUrl;
    fx->cfg->model.apiKey    = "EMPTY";
    fx->cfg->model.modelName = "default-model";
    fx->cfg->llmMaxRetry     = 1;
    if (!dataDir.empty()) {
        fx->cfg->dataDir            = dataDir;
        // 会话持久化默认关闭 (库使用方按需开启): 收件箱用例要求落盘
        fx->cfg->enableSessionStore = true;
    }

    fx->agent = std::make_shared<agentxx::agent::BaseAgent>(fx->cfg);
    co_await fx->agent->init();

    auto ex = co_await asio::this_coro::executor;

    agentxx::agent::SessionServerAgentIO::Config scCfg;
    scCfg.sessionId = sessionId;
    fx->sessionId   = sessionId;
    fx->endpoint    = std::make_shared<agentxx::agent::SessionServerAgentIO>(ex, fx->agent, scCfg);

    auto [clientOwn, serverT] = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);
    fx->endpoint
        ->setTransport(std::shared_ptr<agentxx::agent::AgentIOTransportBase>(std::move(serverT)));

    fx->recorder = std::make_shared<ClientRecorder>();
    fx->clientT  = std::move(clientOwn);

    // 传输对象与记录器均按值捕获 (分离协程可能比测试函数活得久)
    asio::co_spawn(
        ex,
        [clientT = fx->clientT, recorder = fx->recorder]() -> asio::awaitable<void> {
            while (clientT->alive()) {
                auto msg = co_await clientT->recv();
                if (!msg) {
                    break;
                }
                recorder->handle(std::move(*msg));
            }
        },
        asio::detached
    );
    asio::co_spawn(
        ex,
        [ep = fx->endpoint]() -> asio::awaitable<void> {
            co_await ep->runTransportLoop();
        },
        asio::detached
    );
    asio::co_spawn(
        ex,
        [ep = fx->endpoint]() -> asio::awaitable<void> {
            co_await ep->run();
        },
        asio::detached
    );
    co_return fx;
}

/// 创建唯一临时目录
std::string makeTempRoot() {
    auto dir = fs::temp_directory_path()
               / fmt::format(
                   "agentxx_id_test_{}",
                   std::chrono::steady_clock::now().time_since_epoch().count()
               );
    fs::create_directories(dir);
    return dir.string();
}

void removeTempRoot(const std::string& root) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        std::error_code ec;
        fs::remove_all(utilxx_base::utf8ToPath(root), ec);
        if (!ec) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
}

/// 会话当前 LLM 上下文里是否包含指定文本
bool contextHasText(
    const std::shared_ptr<agentxx::agent::BaseAgent>& agent,
    std::string_view                                  sessionId,
    std::string_view                                  text
) {
    auto session = agent->agentContext->sessions->get(sessionId);
    if (!session) {
        return false;
    }
    for (const auto& m : session->messages()) {
        if (m.content.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 1. 投递结果显式化: started / queued / rejected (LOOP-3)
// ---------------------------------------------------------------------------

asio::awaitable<void> test_input_ack_statuses() {
    auto fx = co_await makeFixture("ack-test-session", {}, "ack test response");

    // ① 空内容 → rejected + empty_content
    fx->send("", std::string{agentxx::agent::InputDelivery::NextTurn}, 1);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->ackCount() >= 1;
    }));
    if (auto ack = fx->recorder->ack(0)) {
        XX_TEST_EXPECT_EQ(ack->requestId, uint64_t{1});
        XX_TEST_EXPECT_EQ(ack->status, std::string{agentxx::agent::InputStatus::Rejected});
        XX_TEST_EXPECT_EQ(
            ack->reason,
            std::string{agentxx::agent::InputRejectReason::EmptyContent}
        );
    }

    // ② 会话不匹配 → rejected + session_mismatch (另有 WireError 结构化错误)
    {
        agentxx::agent::WireUserInput input;
        input.sessionId = "other-session";
        input.text      = "hello";
        input.requestId = 2;
        fx->clientT->send(agentxx::agent::WireMessage{std::move(input)});
    }
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->ackCount() >= 2;
    }));
    if (auto ack = fx->recorder->ack(1)) {
        XX_TEST_EXPECT_EQ(ack->status, std::string{agentxx::agent::InputStatus::Rejected});
        XX_TEST_EXPECT_EQ(
            ack->reason,
            std::string{agentxx::agent::InputRejectReason::SessionMismatch}
        );
    }
    XX_TEST_EXPECT_TRUE(fx->recorder->errorCount() >= 1);

    // ③ 未知投递模式 → rejected + bad_delivery
    fx->send("hello", "teleport", 3);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->ackCount() >= 3;
    }));
    if (auto ack = fx->recorder->ack(2)) {
        XX_TEST_EXPECT_EQ(
            ack->reason,
            std::string{agentxx::agent::InputRejectReason::BadDelivery}
        );
    }

    // ④ 空闲时正常输入 → started
    fx->send("turn one", std::string{agentxx::agent::InputDelivery::NextTurn}, 4);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->ackCount() >= 4;
    }));
    if (auto ack = fx->recorder->ack(3)) {
        XX_TEST_EXPECT_EQ(ack->status, std::string{agentxx::agent::InputStatus::Started});
        XX_TEST_EXPECT_EQ(ack->delivery, std::string{agentxx::agent::InputDelivery::NextTurn});
        XX_TEST_EXPECT_TRUE(!ack->itemId.empty());
    }

    // ⑤ 轮次进行中再发一条 → queued (等待当前轮次结束)
    co_await spin(std::chrono::milliseconds{30});
    fx->send("turn two", std::string{agentxx::agent::InputDelivery::NextTurn}, 5);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->ackCount() >= 5;
    }));
    if (auto ack = fx->recorder->ack(4)) {
        XX_TEST_EXPECT_EQ(ack->status, std::string{agentxx::agent::InputStatus::Queued});
    }

    // ⑥ 旧客户端 (requestId == 0): 不回执, 行为保持旧语义
    const size_t before = fx->recorder->ackCount();
    fx->send("legacy input", std::string{agentxx::agent::InputDelivery::NextTurn}, 0);
    co_await spin(std::chrono::milliseconds{120});
    XX_TEST_EXPECT_EQ(fx->recorder->ackCount(), before);
}

// ---------------------------------------------------------------------------
// 2. 队列状态机 (LOOP-4): idle → running → paused → 消化积压 → idle
// ---------------------------------------------------------------------------

asio::awaitable<void> test_queue_state_machine() {
    auto fx = co_await makeFixture("state-test-session", {}, "state response");

    XX_TEST_EXPECT_EQ(
        std::string{agentxx::agent::sessionQueueStateText(fx->endpoint->queueState())},
        std::string{"idle"}
    );

    // 长轮次 (延迟 400ms) 期间排队一条, 再取消 → 队列暂停
    g_da_sim_delay_ms = 400;
    fx->send("slow turn", std::string{agentxx::agent::InputDelivery::NextTurn}, 1);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->ackCount() >= 1;
    }));
    if (auto ack = fx->recorder->ack(0)) {
        XX_TEST_EXPECT_EQ(ack->status, std::string{agentxx::agent::InputStatus::Started});
    }

    fx->send("queued turn", std::string{agentxx::agent::InputDelivery::NextTurn}, 2);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->ackCount() >= 2;
    }));
    if (auto ack = fx->recorder->ack(1)) {
        XX_TEST_EXPECT_EQ(ack->status, std::string{agentxx::agent::InputStatus::Queued});
    }

    fx->clientT->send(agentxx::agent::WireMessage{
        agentxx::agent::WireCancel{fx->sessionId}
    });
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->endpoint->queueState() == agentxx::agent::SessionQueueState::Paused;
    }));
    XX_TEST_EXPECT_TRUE(fx->endpoint->isQueuePausedForTest());
    XX_TEST_EXPECT_TRUE(fx->endpoint->queueSizeForTest() >= 1);
    // 队列状态经 WireMessageQueueUpdate 同步给客户端 (投递后需等接收方处理)
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->lastQueueState() == "paused";
    }));

    // 用户发新输入 → 解除暂停并消化积压队列 (本条排在积压之后)
    g_da_sim_delay_ms        = 0;
    const int turnsBefore    = fx->recorder->turnResults.load();
    fx->send("resume turn", std::string{agentxx::agent::InputDelivery::NextTurn}, 3);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->ackCount() >= 3;
    }));
    if (auto ack = fx->recorder->ack(2)) {
        XX_TEST_EXPECT_EQ(ack->status, std::string{agentxx::agent::InputStatus::Queued});
    }

    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->turnResults.load() >= turnsBefore + 2
               && fx->endpoint->queueSizeForTest() == 0;
    }));
    // 消化完积压队列后回到 idle (失败时把实际状态与最近轮次错误一起打印, 便于定位)
    const auto stateText
        = std::string{agentxx::agent::sessionQueueStateText(fx->endpoint->queueState())};
    if (stateText != "idle") {
        XX_TEST_EXPECT_EQ(
            stateText + " | last_turn_error=" + fx->recorder->lastTurnError(),
            std::string{"idle"}
        );
    } else {
        XX_TEST_EXPECT_TRUE(true);
    }
}

// ---------------------------------------------------------------------------
// 3. next-step 注入: 在下一个 modelcall 边界进入权威上下文 (LOOP-2)
// ---------------------------------------------------------------------------

asio::awaitable<void> test_next_step_injection() {
    auto fx = co_await makeFixture("nextstep-test-session", {}, "next step response");

    // 先触发一次工具调用, 使同一轮次里有第二个 modelcall 边界
    g_da_sim_tool_calls = utilxx_base::Json::array({
        utilxx_base::Json{
            {"id",        "call_1"                   },
            {"name",      "no_such_tool_next_step"   },
            {"arguments", "{}"                       },
        }
    });
    g_da_sim_tool_calls_remaining = 1;
    g_da_sim_delay_ms             = 250;

    fx->send("first turn", std::string{agentxx::agent::InputDelivery::NextTurn}, 1);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->ackCount() >= 1;
    }));

    // 轮次进行中发送 next-step: 应在下一个 modelcall 边界注入
    fx->send("steered note", std::string{agentxx::agent::InputDelivery::NextStep}, 2);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->ackCount() >= 2;
    }));
    if (auto ack = fx->recorder->ack(1)) {
        XX_TEST_EXPECT_EQ(ack->status, std::string{agentxx::agent::InputStatus::Steered});
        XX_TEST_EXPECT_EQ(ack->delivery, std::string{agentxx::agent::InputDelivery::NextStep});
    }

    // 注入内容进入权威上下文 (后续请求可见) 与展示历史 (UI 可见)
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return contextHasText(fx->agent, fx->sessionId, "steered note")
               && fx->recorder->sawViewMessageWithText("steered note");
    }));
}

// ---------------------------------------------------------------------------
// 4. inject: 只进入下一次请求, 不改写权威上下文 (LOOP-2)
// ---------------------------------------------------------------------------

asio::awaitable<void> test_inject_only_in_request() {
    auto fx = co_await makeFixture("inject-test-session", {}, "inject response");

    // 空闲时 inject: 不唤醒会话 (不产生轮次), 保持待注入
    fx->send("injected note", std::string{agentxx::agent::InputDelivery::Inject}, 1);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->ackCount() >= 1;
    }));
    if (auto ack = fx->recorder->ack(0)) {
        XX_TEST_EXPECT_EQ(ack->status, std::string{agentxx::agent::InputStatus::Queued});
        XX_TEST_EXPECT_EQ(ack->delivery, std::string{agentxx::agent::InputDelivery::Inject});
    }
    co_await spin(std::chrono::milliseconds{120});
    XX_TEST_EXPECT_EQ(fx->recorder->turnResults.load(), 0);

    // 下一条普通输入触发轮次: 注入内容随本次请求发出, 但不进入权威上下文
    fx->send("real turn", std::string{agentxx::agent::InputDelivery::NextTurn}, 2);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->turnResults.load() >= 1;
    }));

    bool inRequest = false;
    if (g_da_sim_request_count.load() >= 1) {
        inRequest = g_da_sim_last_request.dump().find("injected note") != std::string::npos;
    }
    XX_TEST_EXPECT_TRUE(inRequest);
    XX_TEST_EXPECT_TRUE(!contextHasText(fx->agent, fx->sessionId, "injected note"));
    XX_TEST_EXPECT_TRUE(!fx->recorder->sawViewMessageWithText("injected note"));
}

// ---------------------------------------------------------------------------
// 5. collect: 静默窗口内合并同一客户端的连续输入 (LOOP-11)
// ---------------------------------------------------------------------------

asio::awaitable<void> test_collect_merging() {
    auto fx = co_await makeFixture("collect-test-session", {}, "collect response");

    fx->send("part one", std::string{agentxx::agent::InputDelivery::Collect}, 1);
    fx->send("part two", std::string{agentxx::agent::InputDelivery::Collect}, 2);
    fx->send("part three", std::string{agentxx::agent::InputDelivery::Collect}, 3);

    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->turnResults.load() >= 1;
    }));
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->ackCount() >= 3;
    }));

    // 三条输入合并为一轮
    co_await spin(std::chrono::milliseconds{300});
    XX_TEST_EXPECT_EQ(fx->recorder->turnResults.load(), 1);
    if (auto ack = fx->recorder->ack(2)) {
        XX_TEST_EXPECT_EQ(ack->delivery, std::string{agentxx::agent::InputDelivery::Collect});
        XX_TEST_EXPECT_TRUE(
            ack->status == agentxx::agent::InputStatus::Started
            || ack->status == agentxx::agent::InputStatus::Queued
        );
    }
    const auto dumped = g_da_sim_last_request.dump();
    XX_TEST_EXPECT_TRUE(dumped.find("part one") != std::string::npos);
    XX_TEST_EXPECT_TRUE(dumped.find("part two") != std::string::npos);
    XX_TEST_EXPECT_TRUE(dumped.find("part three") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 6. 收件箱存储层: 写入/查询/状态流转 (LOOP-1 的存储基础)
// ---------------------------------------------------------------------------

asio::awaitable<void> test_inbox_store_api() {
    const auto root = makeTempRoot();
    {
        agentxx::agent::SessionStore store{root};
        const std::string           sessionId = "store-api-session";

        agentxx::agent::SessionStore::SessionInputRecord rec;
        rec.id          = "i-1";
        rec.payload     = R"({"text":"hello","delivery":"inject"})";
        rec.delivery    = std::string{agentxx::agent::InputDelivery::Inject};
        rec.status      = std::string{agentxx::agent::SessionStore::SessionInputStatus::Admitted};
        rec.admittedSeq = 7;
        rec.createdMs   = 1000;
        store.addSessionInput(sessionId, rec);

        XX_TEST_EXPECT_EQ(store.listSessionInputs(sessionId).size(), size_t{1});
        auto admitted = store.listSessionInputs(
            sessionId,
            agentxx::agent::SessionStore::SessionInputStatus::Admitted
        );
        XX_TEST_EXPECT_EQ(admitted.size(), size_t{1});
        if (!admitted.empty()) {
            XX_TEST_EXPECT_EQ(admitted[0].id, std::string{"i-1"});
            XX_TEST_EXPECT_EQ(admitted[0].admittedSeq, int64_t{7});
            XX_TEST_EXPECT_TRUE(admitted[0].payload.find("hello") != std::string::npos);
        }

        // 标记投递: 不再是"待确认"
        store.markSessionInputPromoted(sessionId, "i-1", 42);
        XX_TEST_EXPECT_EQ(
            store
                .listSessionInputs(sessionId, agentxx::agent::SessionStore::SessionInputStatus::Admitted)
                .size(),
            size_t{0}
        );
        auto promoted = store.listSessionInputs(
            sessionId,
            agentxx::agent::SessionStore::SessionInputStatus::Promoted
        );
        XX_TEST_EXPECT_EQ(promoted.size(), size_t{1});
        if (!promoted.empty()) {
            XX_TEST_EXPECT_EQ(promoted[0].promotedSeq, int64_t{42});
        }

        // 丢弃 (删除/清空队列)
        store.setSessionInputStatus(
            sessionId,
            "i-1",
            agentxx::agent::SessionStore::SessionInputStatus::Dropped
        );
        XX_TEST_EXPECT_EQ(
            store.listSessionInputs(sessionId, agentxx::agent::SessionStore::SessionInputStatus::Dropped)
                .size(),
            size_t{1}
        );
        // 重复写入同 id 覆盖 (幂等)
        store.addSessionInput(sessionId, rec);
        XX_TEST_EXPECT_EQ(store.listSessionInputs(sessionId).size(), size_t{1});
    }
    removeTempRoot(root);
    co_return;
}

// ---------------------------------------------------------------------------
// 7. 持久化收件箱: 重启后未投递输入按"待确认"恢复 (LOOP-1)
// ---------------------------------------------------------------------------

asio::awaitable<void> test_durable_inbox_recovery() {
    const auto        root      = makeTempRoot();
    const std::string sessionId = "inbox-test-session";

    {
        auto fx = co_await makeFixture(sessionId, root, "inbox response");
        // inject 在空闲时不产生轮次: 条目停留在"已受理未投递"状态
        fx->send("pending note", std::string{agentxx::agent::InputDelivery::Inject}, 1);
        XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
            return fx->recorder->ackCount() >= 1;
        }));
        co_await spin(std::chrono::milliseconds{100});

        // 经 agent 自身的会话库核对 (避免第二个 SessionStore 实例重复取写租约)
        auto store = fx->agent->agentContext->sessions->sessionStore;
        XX_TEST_EXPECT_TRUE(store != nullptr);
        if (auto ack = fx->recorder->ack(0)) {
            XX_TEST_EXPECT_EQ(ack->status, std::string{agentxx::agent::InputStatus::Queued});
        }
        if (store) {
            auto rows = store->listSessionInputs(
                sessionId,
                agentxx::agent::SessionStore::SessionInputStatus::Admitted
            );
            XX_TEST_EXPECT_EQ(rows.size(), size_t{1});
            if (!rows.empty()) {
                XX_TEST_EXPECT_TRUE(rows[0].payload.find("pending note") != std::string::npos);
                XX_TEST_EXPECT_EQ(
                    rows[0].delivery,
                    std::string{agentxx::agent::InputDelivery::Inject}
                );
            }
        }
    }
    // 夹具析构: 端点停止 + 模拟器停止 (等价"进程重启", 会话库保留)

    {
        auto fx = co_await makeFixture(sessionId, root, "inbox response");
        // 新端点从收件箱恢复: 置暂停不自动执行
        XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
            return fx->endpoint->queueState() == agentxx::agent::SessionQueueState::Paused;
        }));
        XX_TEST_EXPECT_TRUE(fx->endpoint->queueSizeForTest() >= 1);
        XX_TEST_EXPECT_TRUE(fx->recorder->lastQueueRecovered());
        XX_TEST_EXPECT_EQ(fx->recorder->turnResults.load(), 0);

        // 用户确认 (发送新输入) 后驱动器消化队列
        fx->send("confirm and go", std::string{agentxx::agent::InputDelivery::NextTurn}, 1);
        XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
            return fx->recorder->turnResults.load() >= 1;
        }));
    }

    removeTempRoot(root);
}

// ---------------------------------------------------------------------------
// 7. 收件箱终态: 删除/清空的排队条目不作为"待确认"恢复
// ---------------------------------------------------------------------------

asio::awaitable<void> test_inbox_dropped_item_not_restored() {
    const auto        root      = makeTempRoot();
    const std::string sessionId = "inbox-drop-session";

    {
        auto fx = co_await makeFixture(sessionId, root, "drop response");
        g_da_sim_delay_ms = 300;
        fx->send("running", std::string{agentxx::agent::InputDelivery::NextTurn}, 1);
        XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
            return fx->recorder->ackCount() >= 1;
        }));
        fx->send("queued a", std::string{agentxx::agent::InputDelivery::NextTurn}, 2);
        XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
            return fx->recorder->ackCount() >= 2;
        }));
        if (auto ack = fx->recorder->ack(1); ack && !ack->itemId.empty()) {
            fx->clientT->send(agentxx::agent::WireMessage{
                agentxx::agent::WireRemoveQueueItem{fx->sessionId, ack->itemId}
            });
        }
        co_await spin(std::chrono::milliseconds{80});
        // 清空队列 (剩余条目一并作废)
        fx->clientT->send(agentxx::agent::WireMessage{
            agentxx::agent::WireClearMessageQueue{fx->sessionId}
        });
        co_await spin(std::chrono::milliseconds{150});

        auto store = fx->agent->agentContext->sessions->sessionStore;
        XX_TEST_EXPECT_TRUE(store != nullptr);
        if (store) {
            auto rows = store->listSessionInputs(
                sessionId,
                agentxx::agent::SessionStore::SessionInputStatus::Admitted
            );
            XX_TEST_EXPECT_EQ(rows.size(), size_t{0});
        }
    }

    {
        auto fx = co_await makeFixture(sessionId, root, "drop response");
        co_await spin(std::chrono::milliseconds{300});
        XX_TEST_EXPECT_EQ(fx->endpoint->queueSizeForTest(), size_t{0});
        XX_TEST_EXPECT_TRUE(fx->endpoint->queueState() == agentxx::agent::SessionQueueState::Idle);
    }

    removeTempRoot(root);
}

/// 连接阶段校验与错误码 (计划 PRO-5)
///
/// - 未握手的传输不能发业务消息: 端点回 `WireError(InvalidState)` (而不是按正常
///   流程处理或静默丢弃)
/// - 收到 hello 后端点把该传输推进到 ready, 之后业务消息照常受理
/// - 引用不存在的队列条目回 `WireError(MessageNotFound)`
asio::awaitable<void> test_connection_stage_guard() {
    /// 测试用传输: 阶段可控, 记录端点的阶段更新与发出的消息
    class StageTransport : public agentxx::agent::AgentIOTransportBase {
    public:

        void send(agentxx::agent::WireMessage msg) override {
            sent.push_back(std::move(msg));
        }

        asio::awaitable<std::optional<agentxx::agent::WireMessage>> recv() override {
            co_return std::nullopt;
        }

        void close() override {}

        bool alive() const noexcept override {
            return true;
        }

        agentxx::agent::WireConnectionStage stage() const noexcept override {
            return stage_;
        }

        void setStage(agentxx::agent::WireConnectionStage s, std::string_view /*reason*/) override {
            stage_ = s;
            ++setStageCalls;
        }

        /// 是否收到过指定错误码的 WireError
        bool hasError(int code) const {
            for (const auto& msg : sent) {
                if (const auto* err = std::get_if<agentxx::agent::WireError>(&msg)) {
                    if (err->code == code) {
                        return true;
                    }
                }
            }
            return false;
        }

        std::vector<agentxx::agent::WireMessage> sent;
        int                                      setStageCalls = 0;
        agentxx::agent::WireConnectionStage      stage_
            = agentxx::agent::WireConnectionStage::Unhandshaken;
    };

    auto fx = co_await makeFixture("stage_session", {}, "stage response");

    auto t = std::make_shared<StageTransport>();
    XX_TEST_EXPECT_TRUE(
        t->stage() == agentxx::agent::WireConnectionStage::Unhandshaken
    );

    // 1) 未握手时发业务消息: 只回 InvalidState, 不进入业务处理
    fx->endpoint->onPeerMessage(
        agentxx::agent::WireMessage{
            agentxx::agent::WireUserInput{.sessionId = "stage_session", .text = "too early"}
        },
        t
    );
    XX_TEST_EXPECT_TRUE(t->hasError(agentxx::agent::WireErrorCode::InvalidState));
    XX_TEST_EXPECT_EQ(fx->endpoint->queueSizeForTest(), size_t{0});

    // 2) hello 后端点把该传输推进到 ready (阶段由端点更新)
    fx->endpoint->onPeerMessage(
        agentxx::agent::WireMessage{
            agentxx::agent::WireHello{.sessionId = "stage_session", .token = "t"}
        },
        t
    );
    XX_TEST_EXPECT_TRUE(t->stage() == agentxx::agent::WireConnectionStage::Ready);
    XX_TEST_EXPECT_GE(t->setStageCalls, 1);

    // 3) 握手后同一传输的业务消息被受理 (带 requestId: 应收到受理回执)
    t->sent.clear();
    fx->endpoint->onPeerMessage(
        agentxx::agent::WireMessage{
            agentxx::agent::WireUserInput{
                .sessionId = "stage_session",
                .text      = "accepted now",
                .requestId = 7,
            }
        },
        t
    );
    XX_TEST_EXPECT_FALSE(t->hasError(agentxx::agent::WireErrorCode::InvalidState));
    bool sawAck = false;
    for (const auto& msg : t->sent) {
        if (const auto* ack = std::get_if<agentxx::agent::WireInputAck>(&msg)) {
            if (ack->requestId == 7) {
                sawAck = true;
            }
        }
    }
    XX_TEST_EXPECT_TRUE(sawAck);
    co_await spin(std::chrono::milliseconds{200});

    // 4) 删除不存在的队列条目: 明确回 MessageNotFound (不再静默成功)
    fx->clientT->send(
        agentxx::agent::WireMessage{
            agentxx::agent::WireRemoveQueueItem{
                .sessionId = "stage_session",
                .itemId    = "no-such-item",
            }
        }
    );
    for (int i = 0; i < 200; ++i) {
        co_await spin(std::chrono::milliseconds{5});
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(fx->recorder->mu);
            for (const auto& err : fx->recorder->errors) {
                if (err.code == agentxx::agent::WireErrorCode::MessageNotFound) {
                    found = true;
                }
            }
        }
        if (found) {
            break;
        }
    }
    bool foundNotFound = false;
    {
        std::lock_guard<std::mutex> lock(fx->recorder->mu);
        for (const auto& err : fx->recorder->errors) {
            if (err.code == agentxx::agent::WireErrorCode::MessageNotFound) {
                foundNotFound = true;
            }
        }
    }
    XX_TEST_EXPECT_TRUE(foundNotFound);

    // 5) 阶段文本往返 (未知文本返回 nullopt, 由调用方兜底)
    for (auto s : {agentxx::agent::WireConnectionStage::Unhandshaken,
                   agentxx::agent::WireConnectionStage::Unbound,
                   agentxx::agent::WireConnectionStage::Ready,
                   agentxx::agent::WireConnectionStage::Reconnecting,
                   agentxx::agent::WireConnectionStage::Draining}) {
        auto parsed = agentxx::agent::wireConnectionStageFromText(
            agentxx::agent::wireConnectionStageText(s)
        );
        XX_TEST_EXPECT_HAS_VALUE(parsed);
        if (parsed) {
            XX_TEST_EXPECT_TRUE(*parsed == s);
        }
    }
    XX_TEST_EXPECT_FALSE(agentxx::agent::wireConnectionStageFromText("nonsense").has_value());
}

} // namespace

asio::awaitable<TestResult> run_input_delivery_tests() {
    co_await test_input_ack_statuses();
    co_await test_queue_state_machine();
    co_await test_next_step_injection();
    co_await test_inject_only_in_request();
    co_await test_collect_merging();
    co_await test_inbox_store_api();
    co_await test_durable_inbox_recovery();
    co_await test_inbox_dropped_item_not_restored();
    co_await test_connection_stage_guard();

    // 还原全局模拟器开关 (避免影响后续模块)
    g_da_sim_delay_ms             = 0;
    g_da_sim_tool_calls_remaining = -1;
    co_return TestResult{g_id_passed, g_id_failed};
}

} // namespace test
} // namespace agentxx
