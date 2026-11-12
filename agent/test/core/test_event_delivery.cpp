#include "agentxx-test/core/test_event_hub.h"
#include "agentxx-test/core/test_agent.h" // 本地 LLM 模拟器 startDaSimServer/g_da_sim_*
#include "agentxx-test/test_coro_util.h"
#include "agentxx-test/core/test_event_naming.h"

#include "agentxx/agent/base_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/channel_io_transport.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/event/hub.h"
#include "agentxx/util/observability.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "utilxx_base/asio_error.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_ed_passed = 0;
int g_ed_failed = 0;
} // namespace

#define XX_TEST_PASSED g_ed_passed
#define XX_TEST_FAILED g_ed_failed

namespace agentxx {
namespace test {

using namespace utilxx_base;
using Json = utilxx_base::Json;

namespace {

/// 客户端侧只记录"轮次结束"与"队列状态", 用于等待时序
struct DeliveryRecorder {
    std::mutex                                       mu;
    std::vector<agentxx::agent::WireTurnResult>      turns;
    std::vector<agentxx::agent::WireMessageQueueUpdate> queues;

    void handle(agentxx::agent::WireMessage msg) {
        std::visit(
            [this](auto&& m) {
                using T = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<T, agentxx::agent::WireTurnResult>) {
                    std::lock_guard<std::mutex> lock(mu);
                    turns.push_back(m);
                } else if constexpr (std::is_same_v<T, agentxx::agent::WireMessageQueueUpdate>) {
                    std::lock_guard<std::mutex> lock(mu);
                    queues.push_back(m);
                }
            },
            std::move(msg)
        );
    }

    int turnCount() {
        std::lock_guard<std::mutex> lock(mu);
        return static_cast<int>(turns.size());
    }

    std::string lastQueueState() {
        std::lock_guard<std::mutex> lock(mu);
        return queues.empty() ? std::string{} : queues.back().state;
    }
};

struct DeliveryFixture {
    DaSimServer                                              sim;
    std::shared_ptr<agentxx::agent::AgentConfig>             cfg;
    std::shared_ptr<agentxx::agent::BaseAgent>               agent;
    std::shared_ptr<agentxx::agent::SessionServerAgentIO>    endpoint;
    std::shared_ptr<DeliveryRecorder>                        recorder;
    std::shared_ptr<agentxx::agent::ChannelAgentIOTransport> clientT;
    std::string                                              sessionId;

    ~DeliveryFixture() {
        if (clientT) {
            clientT->close();
        }
        if (endpoint) {
            endpoint->stop();
        }
        sim.stop();
    }

    agentxx::events::EventHub* hub() const {
        return agent && agent->agentContext ? agent->agentContext->eventHub.get() : nullptr;
    }
};

asio::awaitable<void> sleepFor(int64_t ms) {
    asio::steady_timer timer(co_await asio::this_coro::executor, std::chrono::milliseconds{ms});
    AsioErrorCode      ec;
    co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
}

asio::awaitable<bool> waitFor(std::function<bool()> cond, int64_t timeoutMs = 8000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMs};
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            co_return true;
        }
        co_await sleepFor(15);
    }
    co_return cond();
}

bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

/// 会话上下文里是否出现了指定文本 (跳过 system: 提示词里就有 `[Event]` 的说明)
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
        if (m.role == "system") {
            continue;
        }
        if (contains(m.content, text)) {
            return true;
        }
    }
    return false;
}

/// 会话展示历史里是否出现了指定文本
bool viewHasText(
    const std::shared_ptr<agentxx::agent::BaseAgent>& agent,
    std::string_view                                  sessionId,
    std::string_view                                  text
) {
    auto session = agent->agentContext->sessions->get(sessionId);
    if (!session) {
        return false;
    }
    for (const auto& m : session->viewWindow()) {
        if (contains(m.text, text)) {
            return true;
        }
    }
    return false;
}

/// 展示历史里那条事件输入消息的来源字段 (找不到时返回空)
/// - 事件输入在展示历史里是 Role::Event (LLM 上下文里仍是 user 消息)
std::string viewSourceOfEventMessage(
    const std::shared_ptr<agentxx::agent::BaseAgent>& agent,
    std::string_view                                  sessionId,
    std::string_view                                  text
) {
    auto session = agent->agentContext->sessions->get(sessionId);
    if (!session) {
        return {};
    }
    for (const auto& m : session->viewWindow()) {
        if (m.role == agentxx::agent::ViewMessage::Role::Event && contains(m.text, text)) {
            return m.source;
        }
    }
    return {};
}

/// 展示历史里首条事件输入消息 (无则返回 nullptr)
const agentxx::agent::ViewMessage* firstEventViewMessage(
    const std::shared_ptr<agentxx::agent::BaseAgent>& agent,
    std::string_view                                  sessionId
) {
    auto session = agent->agentContext->sessions->get(sessionId);
    if (!session) {
        return nullptr;
    }
    for (const auto& m : session->viewWindow()) {
        if (m.role == agentxx::agent::ViewMessage::Role::Event) {
            return &m;
        }
    }
    return nullptr;
}

asio::awaitable<std::shared_ptr<DeliveryFixture>> makeDeliveryFixture(
    std::string sessionId,
    bool        startEndpoint = true,
    int         llmMaxRetry   = 1
) {
    auto fx                       = std::make_shared<DeliveryFixture>();
    fx->sim                       = startDaSimServer();
    g_da_sim_response_content     = "ok response";
    g_da_sim_tool_calls           = Json::array();
    g_da_sim_fail_count           = 0;
    g_da_sim_delay_ms             = 0;
    g_da_sim_tool_calls_remaining = -1;

    const auto baseUrl       = "http://127.0.0.1:" + std::to_string(fx->sim.port);
    fx->cfg                  = std::make_shared<agentxx::agent::AgentConfig>();
    fx->cfg->model.baseUrl   = baseUrl;
    fx->cfg->model.apiKey    = "EMPTY";
    fx->cfg->model.modelName = "default-model";
    fx->cfg->llmMaxRetry     = llmMaxRetry;

    fx->agent = std::make_shared<agentxx::agent::BaseAgent>(fx->cfg);
    co_await fx->agent->init();

    // 先建出会话: 事件投递要求会话已存在 (不自动新建会话)
    auto session = fx->agent->agentContext->getSession(sessionId);
    XX_TEST_EXPECT_TRUE(session != nullptr);

    auto ex = co_await asio::this_coro::executor;

    agentxx::agent::SessionServerAgentIO::Config scCfg;
    scCfg.sessionId = sessionId;
    fx->sessionId   = sessionId;
    fx->endpoint    = std::make_shared<agentxx::agent::SessionServerAgentIO>(ex, fx->agent, scCfg);

    auto [clientOwn, serverT] = agentxx::agent::ChannelAgentIOTransport::makePair(ex, ex);
    fx->endpoint->setTransport(
        std::shared_ptr<agentxx::agent::AgentIOTransportBase>(std::move(serverT))
    );

    fx->recorder = std::make_shared<DeliveryRecorder>();
    fx->clientT  = std::move(clientOwn);

    asio::co_spawn(
        ex,
        keepAliveRun([clientT = fx->clientT, recorder = fx->recorder]() -> asio::awaitable<void> {
            while (clientT->alive()) {
                auto msg = co_await clientT->recv();
                if (!msg) {
                    break;
                }
                recorder->handle(std::move(*msg));
            }
        }),
        asio::detached
    );
    asio::co_spawn(
        ex,
        keepAliveRun([ep = fx->endpoint]() -> asio::awaitable<void> {
            co_await ep->runTransportLoop();
        }),
        asio::detached
    );
    if (startEndpoint) {
        asio::co_spawn(
            ex,
            keepAliveRun([ep = fx->endpoint]() -> asio::awaitable<void> {
                co_await ep->run();
            }),
            asio::detached
        );
    }
    // 等端点把 sink 登记到会话上 (run() 预热会话后才登记)
    if (startEndpoint) {
        co_await waitFor([&] {
            auto session = fx->agent->agentContext->sessions->get(sessionId);
            return session && !session->inputSink.expired();
        });
    }
    co_return fx;
}

} // namespace

/// 1. 空闲 + next-step: 事件直接把会话唤醒成新一轮, 值作为 user 消息进入上下文与展示历史
asio::awaitable<void> test_delivery_idle_starts_turn() {
    auto fx = co_await makeDeliveryFixture("ev-idle");
    auto hub = fx->hub();
    XX_TEST_EXPECT_TRUE(hub != nullptr);

    auto watched = hub->watch(agentxx::events::EventWaitSpec{
        .event         = evName("chat.message"),
        .mode          = "first",
        .delivery      = "next-step",
        .originSession = fx->sessionId,
    });
    XX_TEST_EXPECT_TRUE(watched.has_value());
    XX_TEST_EXPECT_TRUE(watched->state == agentxx::events::WaitState::Armed);

    std::string err;
    XX_TEST_EXPECT_TRUE(hub->publishValue(evName("chat.message"), Json{{"text", "hi there"}}, &err));
    // 会话被唤醒并跑完一轮
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->turnCount() >= 1;
    }));
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return contextHasText(fx->agent, fx->sessionId, "[Event] " + evName("chat.message"));
    }));
    XX_TEST_EXPECT_TRUE(contextHasText(fx->agent, fx->sessionId, "hi there"));
    // 展示历史也有一条 (用户看得见事件把什么塞进了会话)
    XX_TEST_EXPECT_TRUE(viewHasText(fx->agent, fx->sessionId, "[Event] " + evName("chat.message")));
    // 展示消息带结构化来源标记 (界面据此折叠成 `+ [事件] <名字>` 头部)
    XX_TEST_EXPECT_EQ(
        viewSourceOfEventMessage(fx->agent, fx->sessionId, "[Event] " + evName("chat.message")),
        std::string{agentxx::agent::kInputSourceEventPrefix} + evName("chat.message")
    );
    // 展示角色是 Event 且默认折叠 (事件正文很长, 展开会占满屏幕)
    if (const auto* evMsg = firstEventViewMessage(fx->agent, fx->sessionId)) {
        XX_TEST_EXPECT_TRUE(evMsg->collapsed);
        XX_TEST_EXPECT_EQ(
            std::string{agentxx::agent::eventNameOfInputSource(evMsg->source)},
            evName("chat.message")
        );
    } else {
        XX_TEST_EXPECT_TRUE(false);
    }
    // 端点队列里的来源标记
    {
        auto session = fx->agent->agentContext->sessions->get(fx->sessionId);
        XX_TEST_EXPECT_TRUE(session->pendingInputCount() == 0);
    }
    // 关键指标: 投递计数随中枢投递增长
    if (fx->agent->agentContext->metrics) {
        XX_TEST_EXPECT_TRUE(fx->agent->agentContext->metrics->eventDelivered() >= 1);
    }
    co_return;
}

/// 2. 轮次在跑 + next-step: 值写入待注入输入, 不立刻开新轮
asio::awaitable<void> test_delivery_busy_steps() {
    auto fx = co_await makeDeliveryFixture("ev-busy");
    auto hub = fx->hub();
    fx->endpoint->setTurnActiveForTest(true);

    auto watched = hub->watch(agentxx::events::EventWaitSpec{
        .event         = evName("build.done"),
        .mode          = "first",
        .delivery      = "next-step",
        .originSession = fx->sessionId,
    });
    XX_TEST_EXPECT_TRUE(watched.has_value());
    std::string err;
    XX_TEST_EXPECT_TRUE(hub->publishValue(evName("build.done"), Json{{"ok", true}}, &err));

    auto session = fx->agent->agentContext->sessions->get(fx->sessionId);
    XX_TEST_EXPECT_TRUE(session->pendingInputCount("event:") == 1);
    XX_TEST_EXPECT_TRUE(fx->endpoint->queueSizeForTest() == 0);
    {
        auto inputs = session->takePendingInputs(agentxx::agent::InputDelivery::NextStep);
        XX_TEST_EXPECT_TRUE(inputs.size() == 1);
        XX_TEST_EXPECT_TRUE(
            contains(inputs.front().text, "[Event] " + evName("build.done") + "\n[Info]\n")
        );
        XX_TEST_EXPECT_TRUE(inputs.front().source == "event:" + evName("build.done"));
    }
    fx->endpoint->setTurnActiveForTest(false);
    co_return;
}

/// 3. inject 在轮次进行中只进请求, 不改写上下文
asio::awaitable<void> test_delivery_busy_inject() {
    auto fx = co_await makeDeliveryFixture("ev-inject");
    auto hub = fx->hub();
    fx->endpoint->setTurnActiveForTest(true);

    auto watched = hub->watch(agentxx::events::EventWaitSpec{
        .event         = evName("notice.item"),
        .mode          = "first",
        .delivery      = "inject",
        .originSession = fx->sessionId,
    });
    XX_TEST_EXPECT_TRUE(watched.has_value());
    std::string err;
    XX_TEST_EXPECT_TRUE(hub->publishValue(evName("notice.item"), Json("hello"), &err));

    auto session = fx->agent->agentContext->sessions->get(fx->sessionId);
    auto injects = session->takePendingInputs(agentxx::agent::InputDelivery::Inject);
    XX_TEST_EXPECT_TRUE(injects.size() == 1);
    XX_TEST_EXPECT_TRUE(contains(injects.front().text, "[Event] " + evName("notice.item")));
    fx->endpoint->setTurnActiveForTest(false);
    co_return;
}

/// 4. 无驱动端 (端点未启动): 写待注入输入, 记 deferred, 不开轮
asio::awaitable<void> test_delivery_no_driver() {
    auto fx = co_await makeDeliveryFixture("ev-nodriver", /*startEndpoint=*/false);
    auto hub = fx->hub();

    auto watched = hub->watch(agentxx::events::EventWaitSpec{
        .event         = evName("orphan.event"),
        .mode          = "first",
        .delivery      = "next-step",
        .originSession = fx->sessionId,
    });
    XX_TEST_EXPECT_TRUE(watched.has_value());
    std::string err;
    XX_TEST_EXPECT_TRUE(hub->publishValue(evName("orphan.event"), Json("x"), &err));
    XX_TEST_EXPECT_TRUE(hub->stats().deferred >= 1);

    auto session = fx->agent->agentContext->sessions->get(fx->sessionId);
    XX_TEST_EXPECT_TRUE(session->pendingInputCount("event:") == 1);
    (void)session->takePendingInputs(agentxx::agent::InputDelivery::NextStep);
    co_return;
}

/// 5. 轮末兜底: 落在最后一次 modelcall 之后的 next-step 输入提升为下一轮
asio::awaitable<void> test_delivery_rollover() {
    auto fx      = co_await makeDeliveryFixture("ev-rollover");
    auto session = fx->agent->agentContext->sessions->get(fx->sessionId);

    // 待注入输入只在 modelcall 开头被取用: 要造出"轮末残留", 必须在本次请求
    // 已经发出之后再塞进去, 否则它会在本轮开头就被取走, 测不到兜底逻辑。
    // 让模型响应慢一点 (400 ms), 等请求到达模型侧后再入队 —— 此时本轮不会再
    // 有 modelcall, 输入只能靠轮末提升才能被执行。
    g_da_sim_delay_ms      = 400;
    const int reqBeforeTurn = g_da_sim_request_count.load();

    agentxx::agent::WireUserInput input;
    input.sessionId = fx->sessionId;
    input.text      = "do the work";
    fx->clientT->send(agentxx::agent::WireMessage{std::move(input)});

    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return g_da_sim_request_count.load() > reqBeforeTurn;
    }));

    agentxx::agent::SessionPendingInput leftover;
    leftover.text        = "[Event] late.arrival\n[Info]\nmode: first\nstatus: value\n[Content]\n1";
    leftover.source      = "event:late.arrival";
    leftover.delivery    = std::string{agentxx::agent::InputDelivery::NextStep};
    leftover.createdAtMs = 1;
    session->enqueuePendingInput(leftover);
    XX_TEST_EXPECT_TRUE(session->pendingInputCount() == 1);

    // 之后的轮次恢复默认延迟, 避免拖慢断言
    g_da_sim_delay_ms = 0;

    // 两轮: 第一轮把残留提升, 第二轮把它作为 user 消息写入上下文
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->turnCount() >= 2;
    }));
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return contextHasText(fx->agent, fx->sessionId, "[Event] late.arrival");
    }));
    XX_TEST_EXPECT_TRUE(session->pendingInputCount() == 0);
    co_return;
}

/// 6. 端点停止后解绑 sink: 后续投递不再入队, 计 deferred
asio::awaitable<void> test_delivery_after_stop() {
    auto fx = co_await makeDeliveryFixture("ev-stop");
    auto hub = fx->hub();
    fx->endpoint->stop();
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        auto session = fx->agent->agentContext->sessions->get(fx->sessionId);
        return session && session->inputSink.expired();
    }));

    const auto deferredBefore = hub->stats().deferred;
    auto       watched        = hub->watch(agentxx::events::EventWaitSpec{
        .event         = evName("after.stop"),
        .mode          = "first",
        .delivery      = "next-step",
        .originSession = fx->sessionId,
    });
    XX_TEST_EXPECT_TRUE(watched.has_value());
    std::string err;
    XX_TEST_EXPECT_TRUE(hub->publishValue(evName("after.stop"), Json("x"), &err));
    XX_TEST_EXPECT_TRUE(hub->stats().deferred == deferredBefore + 1);
    co_return;
}

/// 7. 关停: hub 关闭后投递一律丢弃, 等待被取消
asio::awaitable<void> test_delivery_close_all() {
    auto fx = co_await makeDeliveryFixture("ev-close");
    auto hub = fx->hub();
    auto watched = hub->watch(agentxx::events::EventWaitSpec{
        .event         = evName("closing.event"),
        .mode          = "first",
        .delivery      = "next-step",
        .originSession = fx->sessionId,
    });
    XX_TEST_EXPECT_TRUE(watched.has_value());

    hub->closeAll("test-shutdown");
    XX_TEST_EXPECT_TRUE(hub->closed());
    XX_TEST_EXPECT_TRUE(
        hub->queryWait(watched->waitId)->state == agentxx::events::WaitState::Canceled
    );
    std::string err;
    XX_TEST_EXPECT_FALSE(hub->publishValue(evName("closing.event"), Json("x"), &err));
    co_return;
}

/// 8. 暂停态 (上一轮失败留下的) + 空闲: 后台任务结果的投递仍要开新一轮
/// - 背景: 会话因上一轮失败/取消停在暂停态时, 驱动循环不再自动继续队列; 事件输入
///   若也按"不解除暂停"处理, 结果只能等用户下次发消息才被顺带取走
/// - 这类结果必须进上下文 (值是不是取消终态都一样, 见 hasError/canceled 字段)
asio::awaitable<void> test_delivery_async_result_resumes_paused_queue() {
    // llmMaxRetry=0: 让第一轮一次就失败 (否则要等 3 秒退避)
    auto fx  = co_await makeDeliveryFixture("ev-paused", /*startEndpoint=*/true, /*llmMaxRetry=*/0);
    auto hub = fx->hub();
    XX_TEST_EXPECT_TRUE(hub != nullptr);

    // 一轮失败: 驱动端轮末把队列置为暂停
    g_da_sim_fail_count = 100;
    {
        agentxx::agent::WireUserInput input;
        input.sessionId = fx->sessionId;
        input.text      = "make it fail";
        fx->clientT->send(agentxx::agent::WireMessage{std::move(input)});
    }
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->turnCount() >= 1;
    }));
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->endpoint->isQueuePausedForTest();
    }));
    g_da_sim_fail_count = 0;
    const int turnsBefore = fx->recorder->turnCount();

    // 后台任务结果 (取消终态) 投递: 空闲 -> 解除暂停并开新一轮
    const std::string asyncDone{agentxx::events::naming::kAgentAsyncDoneEvent};
    XX_TEST_EXPECT_TRUE(
        hub->watch(agentxx::events::EventWaitSpec{
                       .event         = asyncDone,
                       .mode          = "first",
                       .delivery      = "next-step",
                       .originSession = fx->sessionId,
                       .match         = Json{{"asyncId", "async-9"}},
                   })
            .has_value()
    );
    std::string err;
    Json        value        = Json::object();
    value["asyncId"]         = "async-9";
    value["kind"]            = "tool";
    value["name"]            = "agentxx_execute_command";
    value["hasError"]        = false;
    value["canceled"]        = true;
    value["content"]         = "Async work canceled";
    XX_TEST_EXPECT_TRUE(hub->publishValue(asyncDone, std::move(value), &err));

    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->turnCount() > turnsBefore;
    }));
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return contextHasText(fx->agent, fx->sessionId, "[Event] " + asyncDone);
    }));
    co_return;
}

/// 9. 轮末兜底 + 暂停态: 后台任务结果的残留 next-step 输入照样被执行
/// - 轮末兜底发生在"按结果置暂停"附近 (提交那一刻队列可能还没暂停, 也可能已经
///   暂停), 所以"结果必须进上下文"的意向要记在条目上, 由轮末收尾统一判断
/// - 这里用"取消本轮"制造暂停态: WireCancel 会当场把队列置为暂停, 随后本轮以
///   失败收尾 —— 残留输入若只按"不解除暂停"处理, 就要等用户下次发消息才被执行
asio::awaitable<void> test_delivery_rollover_async_result_resumes() {
    auto fx      = co_await makeDeliveryFixture("ev-rollover-async", true, /*llmMaxRetry=*/0);
    auto session = fx->agent->agentContext->sessions->get(fx->sessionId);

    // 第一轮: 响应慢一点, 留出"请求已发出"的窗口塞残留输入, 再取消本轮
    g_da_sim_delay_ms       = 600;
    const int reqBeforeTurn = g_da_sim_request_count.load();
    {
        agentxx::agent::WireUserInput input;
        input.sessionId = fx->sessionId;
        input.text      = "long running turn";
        fx->clientT->send(agentxx::agent::WireMessage{std::move(input)});
    }
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return g_da_sim_request_count.load() > reqBeforeTurn;
    }));

    // 请求已发出后再塞残留输入: 本轮不会再有 modelcall, 只能靠轮末兜底提升
    const std::string asyncDone{agentxx::events::naming::kAgentAsyncDoneEvent};
    agentxx::agent::SessionPendingInput leftover;
    leftover.text = "[Event] " + asyncDone
                    + "\n[Info]\nmode: first\nwait: w-1\nstatus: value\n[Content]\n"
                      "{\"asyncId\":\"async-1\",\"kind\":\"tool\",\"canceled\":true}";
    leftover.source      = std::string{agentxx::agent::kInputSourceEventPrefix} + asyncDone;
    leftover.delivery    = std::string{agentxx::agent::InputDelivery::NextStep};
    leftover.createdAtMs = 1;
    session->enqueuePendingInput(leftover);
    XX_TEST_EXPECT_TRUE(session->pendingInputCount() == 1);

    // 取消本轮: 队列当场置暂停, 轮末收尾也按"失败/中断"走
    fx->clientT->send(agentxx::agent::WireMessage{agentxx::agent::WireCancel{fx->sessionId}});
    g_da_sim_delay_ms = 0;

    // 残留输入被提升并执行 (第二轮): 上下文里出现那条事件文本
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return fx->recorder->turnCount() >= 2;
    }));
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return contextHasText(fx->agent, fx->sessionId, "[Event] " + asyncDone);
    }));
    XX_TEST_EXPECT_TRUE(session->pendingInputCount() == 0);
    co_return;
}

asio::awaitable<TestResult> run_event_delivery_tests() {
    TestResult result;

    co_await test_delivery_idle_starts_turn();
    co_await test_delivery_busy_steps();
    co_await test_delivery_busy_inject();
    co_await test_delivery_no_driver();
    co_await test_delivery_rollover();
    co_await test_delivery_after_stop();
    co_await test_delivery_close_all();
    co_await test_delivery_async_result_resumes_paused_queue();
    co_await test_delivery_rollover_async_result_resumes();

    result.passed = g_ed_passed;
    result.failed = g_ed_failed;
    co_return result;
}

} // namespace test
} // namespace agentxx
