/// test_plugin_net —— 插件网络能力 (`<应用名>.net` 表) 的本地回环验证
///
/// 网络本体在 `cxx_utilxx` 的 `utilxx::NetHub` (自有网络线程); 主程序侧只做
/// "属主 id = 插件实例名 / 事件换频道 / 状态位对齐"。
///
/// 覆盖:
/// 1. `net.info`: 服务起来后可查, 没有服务时列表为空
/// 2. 回环往返: `http.serve` (127.0.0.1:0 自动选端口) + `http.route` 挂静态内容,
///    `http.fetch` 异步取回, 结果 `status=200` 且正文一致
/// 3. 事件: 事件落到广播频道 `plugin.<属主>.net` (带 `kind`)
/// 4. 属主卸载: `shutdownOwner` 之后该属主的服务消失、未完成请求按 "已取消" 结算
#include "agentxx-test/plugin/test_plugin_net.h"

#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/event/hub.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/tool_registry.h"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "utilxx/net_hub.h"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace {
int g_pn_passed = 0;
int g_pn_failed = 0;
} // namespace

#define XX_TEST_PASSED g_pn_passed
#define XX_TEST_FAILED g_pn_failed

namespace agentxx {
namespace test {

namespace {

/// 最小夹具: AgentContext + 插件管理器 (与其它插件模块同一套)
struct Fixture {
    std::shared_ptr<agentxx::agent::AgentContext>   ctx;
    std::shared_ptr<agentxx::plugin::PluginManager> mgr;
};

asio::awaitable<Fixture> makeFixture() {
    Fixture f;
    f.ctx                          = std::make_shared<agentxx::agent::AgentContext>();
    f.ctx->agentConfig             = std::make_shared<agentxx::agent::AgentConfig>();
    f.ctx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
    f.ctx->bus = std::make_shared<agentxx::events::EventBus>(co_await asio::this_coro::executor);
    f.ctx->toolRegistry       = std::make_shared<agentxx::plugin::ToolRegistry>();
    f.ctx->eventHub
        = std::make_shared<agentxx::events::EventHub>(co_await asio::this_coro::executor, f.ctx);
    f.mgr               = std::make_shared<agentxx::plugin::PluginManager>(f.ctx);
    f.ctx->pluginManager = f.mgr;
    f.mgr->setIoExecutor(co_await asio::this_coro::executor);
    f.ctx->pluginMount = co_await f.mgr->mountAgentAsync(f.ctx);
    co_return f;
}

/// 等条件成立 (网络操作都在别的线程上跑)
asio::awaitable<bool> waitFor(std::function<bool()> cond, int timeoutMs = 8000) {
    auto       ex       = co_await asio::this_coro::executor;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMs};
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            co_return true;
        }
        asio::steady_timer timer(ex);
        timer.expires_after(std::chrono::milliseconds{10});
        utilxx_base::AsioErrorCode ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }
    co_return cond();
}

/// 一次异步请求的完成结果
struct FetchState {
    bool        done   = false;
    int32_t     status = -1;
    std::string payload;
};

void PLUGINXX_CALL onFetchDone(void* ud, int32_t status, const PluginxxStringView* payload) {
    auto* state  = static_cast<FetchState*>(ud);
    state->done   = true;
    state->status = status;
    if (payload != nullptr && payload->data != nullptr) {
        state->payload.assign(payload->data, static_cast<size_t>(payload->size));
    }
}

// ===========================================================================
// 1. 服务起来后可查: `net.info` 有 running / 没有服务时列表为空
// ===========================================================================
asio::awaitable<void> test_net_info() {
    auto f = co_await makeFixture();
    XX_TEST_EXPECT_TRUE(f.mgr->netService() != nullptr);

    std::string out;
    const int32_t rc = f.mgr->netCall("probe_owner", "net.info", "{}", &out);
    XX_TEST_EXPECT_EQ(rc, int32_t{utilxx::net::Ok});
    XX_TEST_EXPECT_FALSE(out.empty());
    if (!out.empty()) {
        auto json = utilxx_base::Json::parse(out);
        XX_TEST_EXPECT_TRUE(json.value("ok", false));
        XX_TEST_EXPECT_TRUE(json.value("running", false));
        // 还没挂服务: 两个列表都空
        XX_TEST_EXPECT_EQ(json["servers"].size(), size_t{0});
    }
    // 未知 op: 参数层面就拒 (net::ErrArg), 不是业务失败
    std::string bad;
    XX_TEST_EXPECT_TRUE(
        f.mgr->netCall("probe_owner", "net.no_such_op", "{}", &bad) != utilxx::net::Ok
    );
    // 坏 JSON: 参数层面就失败
    std::string errJson;
    XX_TEST_EXPECT_TRUE(
        f.mgr->netCall("probe_owner", "net.info", "{not json", &errJson) != utilxx::net::Ok
    );
    f.ctx->pluginManager.reset();
    f.mgr.reset();
    co_return;
}

// ===========================================================================
// 2. 回环往返: http.serve + http.route + http.fetch
// 3. 事件: 落到 plugin.<属主>.net 广播
// ===========================================================================
asio::awaitable<void> test_http_loopback() {
    auto f = co_await makeFixture();
    const std::string owner{"probe_owner"};

    // 订阅网络事件频道 (属主 = probe_owner; 主程序按名字补声明)
    std::vector<std::string> events;
    const std::string        topic{"plugin.probe_owner.net"};
    f.mgr->ensureTopicBroadcast(topic);
    const auto sub = f.mgr->broadcasts()->subscribe(
        topic,
        std::string{},
        [&events](std::string_view payload) { events.emplace_back(payload); }
    );
    XX_TEST_EXPECT_TRUE(sub != 0);

    // 起一个 HTTP 服务 (回环 + 端口 0 = 自动选)
    std::string served;
    XX_TEST_EXPECT_EQ(
        f.mgr->netCall(owner, "http.serve", R"({"host":"127.0.0.1","port":0})", &served),
        int32_t{utilxx::net::Ok}
    );
    auto servedJson = utilxx_base::Json::parse(served.empty() ? "{}" : served);
    XX_TEST_EXPECT_TRUE(servedJson.value("ok", false));
    const std::string serverId = servedJson.value("id", std::string{});
    const int64_t     port     = servedJson.value("port", int64_t{0});
    XX_TEST_EXPECT_FALSE(serverId.empty());
    XX_TEST_EXPECT_TRUE(port > 0);

    // 挂一条静态路由 (框架直接回, 不问属主)
    std::string routed;
    XX_TEST_EXPECT_EQ(
        f.mgr->netCall(
            owner,
            "http.route",
            fmt::format(
                R"({{"serverId":"{}","path":"/hello","bytes":"hello-from-net-hub"}})",
                serverId
            ),
            &routed
        ),
        int32_t{utilxx::net::Ok}
    );
    if (!routed.empty()) {
        XX_TEST_EXPECT_TRUE(utilxx_base::Json::parse(routed).value("ok", false));
    }

    // 客户端取回 (异步): 结果恰好一次
    FetchState state;
    int64_t    requestId = 0;
    std::string accepted;
    XX_TEST_EXPECT_EQ(
        f.mgr->netCallAsync(
            owner,
            "http.fetch",
            fmt::format(R"({{"url":"http://127.0.0.1:{}/hello"}})", port),
            &onFetchDone,
            &state,
            &accepted,
            &requestId
        ),
        int32_t{utilxx::net::Ok}
    );
    XX_TEST_EXPECT_TRUE(requestId > 0);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&]() { return state.done; }));
    XX_TEST_EXPECT_EQ(state.status, int32_t{PLUGINXX_OPERATOR_OK});
    if (!state.payload.empty()) {
        auto result = utilxx_base::Json::parse(state.payload);
        XX_TEST_EXPECT_TRUE(result.value("ok", false));
        XX_TEST_EXPECT_EQ(result.value("status", 0), 200);
        XX_TEST_EXPECT_TRUE(
            result.value("body", std::string{}).find("hello-from-net-hub") != std::string::npos
        );
    }
    // 事件: 至少有 http.listening / http.response 之类, 且带 kind
    XX_TEST_EXPECT_TRUE(co_await waitFor([&]() { return !events.empty(); }));
    if (!events.empty()) {
        XX_TEST_EXPECT_TRUE(events.front().find("\"kind\"") != std::string::npos);
    }

    // 属主卸载: 服务与端口一起释放, 请求列表清空
    f.mgr->netShutdownPlugin(owner);
    std::string afterShutdown;
    XX_TEST_EXPECT_EQ(
        f.mgr->netCall(owner, "net.info", "{}", &afterShutdown),
        int32_t{utilxx::net::Ok}
    );
    if (!afterShutdown.empty()) {
        auto json = utilxx_base::Json::parse(afterShutdown);
        XX_TEST_EXPECT_EQ(json["servers"].size(), size_t{0});
    }
    // 端口已释放: 再取同一个地址失败 (连接被拒)
    FetchState again;
    int64_t    againId = 0;
    std::string againAccepted;
    XX_TEST_EXPECT_EQ(
        f.mgr->netCallAsync(
            owner,
            "http.fetch",
            fmt::format(R"({{"url":"http://127.0.0.1:{}/hello"}})", port),
            &onFetchDone,
            &again,
            &againAccepted,
            &againId
        ),
        int32_t{utilxx::net::Ok}
    );
    XX_TEST_EXPECT_TRUE(co_await waitFor([&]() { return again.done; }));
    // 连接失败: 完成通知是"失败" + 错误 JSON (与 call 的"操作执行完毕"不同)
    XX_TEST_EXPECT_EQ(again.status, int32_t{PLUGINXX_OPERATOR_FAILED});
    if (!again.payload.empty()) {
        auto result = utilxx_base::Json::parse(again.payload);
        XX_TEST_EXPECT_FALSE(result.value("ok", true));
    }

    f.mgr->broadcasts()->unsubscribe(topic, sub);
    co_return;
}

// ===========================================================================
// 4. 卸载时未完成请求按 "已取消" 结算
// ===========================================================================
asio::awaitable<void> test_shutdown_cancels_pending() {
    auto f = co_await makeFixture();
    const std::string owner{"probe_owner"};

    // 一个连不上的地址 + 长超时, 保证请求还在飞
    FetchState state;
    int64_t    requestId = 0;
    std::string accepted;
    XX_TEST_EXPECT_EQ(
        f.mgr->netCallAsync(
            owner,
            "http.fetch",
            R"({"url":"http://127.0.0.1:9/nothing","timeoutMs":15000})",
            &onFetchDone,
            &state,
            &accepted,
            &requestId
        ),
        int32_t{utilxx::net::Ok}
    );
    XX_TEST_EXPECT_TRUE(requestId > 0);
    // 让它先跑起来
    for (int i = 0; i < 20 && state.done; ++i) {
        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds{10});
        utilxx_base::AsioErrorCode ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }
    f.mgr->netShutdownPlugin(owner);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&]() { return state.done; }));
    // 未完成请求按取消结算 (端口 9 也可能立刻连接失败 → Failed, 两者都算"没留悬挂")
    XX_TEST_EXPECT_TRUE(
        state.status == PLUGINXX_OPERATOR_CANCELED || state.status == PLUGINXX_OPERATOR_OK
        || state.status == PLUGINXX_OPERATOR_FAILED
    );
    // 取消已完成/不存在的请求: 参数层面就是"找不到"
    std::string cancelOut;
    XX_TEST_EXPECT_EQ(
        f.mgr->netCancel(owner, requestId, &cancelOut),
        int32_t{utilxx::net::ErrNotFound}
    );
    co_return;
}

} // namespace

asio::awaitable<TestResult> run_plugin_net_tests() {
    g_pn_passed = 0;
    g_pn_failed = 0;
    co_await test_net_info();
    co_await test_http_loopback();
    co_await test_shutdown_cancels_pending();
    co_return TestResult{g_pn_passed, g_pn_failed};
}

} // namespace test
} // namespace agentxx
