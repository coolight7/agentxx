/// test_plugin_point —— 功能埋点 (point) 的主程序侧基础
///
/// 一个功能埋点 = 一个 id, 同时是
/// - 一条**能力** (`agentxx.capabilities`, `impls: many` + `merge: first`, 只开 impl 阶段);
/// - 一个**同名广播频道** (`agentxx.broadcasts`, 通知型不缓存 / 取值型缓存当前值)。
///
/// 覆盖:
/// 1. 声明表: id 头里的 10 个功能埋点在管理器构造后两边都已声明, 缓存策略与类型相符
/// 2. 没有实现行: `emitPointAsync` 按"没人实现 = 没意见"完成 (`OK` + `null`)
/// 3. 通知面: 订阅同名频道后能收到载荷; `publishPoint` 是它的"不等结果"入口
/// 4. 取值型频道保留当前值 (`readBroadcast` 拿得到最后一次载荷)
/// 5. 应用能力 (非功能埋点): 主程序构造时已声明, 形状同样是 many + first
#include "agentxx-test/plugin/test_plugin_point.h"

#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/api/agentxx_capability_ids.h"
#include "agentxx/plugin/api/agentxx_point_ids.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/tool_registry.h"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "utilxx_base/asio_error.h"
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见)
int g_pp_passed = 0;
int g_pp_failed = 0;
} // namespace

#define XX_TEST_PASSED g_pp_passed
#define XX_TEST_FAILED g_pp_failed

namespace agentxx {
namespace test {

namespace {

namespace points = agentxx::plugin::points;
namespace appcaps = agentxx::plugin::capabilities;

struct Fixture {
    std::shared_ptr<agentxx::agent::AgentContext>   ctx;
    std::shared_ptr<agentxx::plugin::PluginManager> mgr;
};

asio::awaitable<Fixture> makeFixture() {
    Fixture f;
    f.ctx                          = std::make_shared<agentxx::agent::AgentContext>();
    f.ctx->agentConfig             = std::make_shared<agentxx::agent::AgentConfig>();
    f.ctx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
    f.ctx->bus         = std::make_shared<agentxx::events::EventBus>(co_await asio::this_coro::executor);
    f.ctx->toolRegistry = std::make_shared<agentxx::plugin::ToolRegistry>();
    f.mgr               = std::make_shared<agentxx::plugin::PluginManager>(f.ctx);
    f.ctx->pluginManager = f.mgr;
    f.mgr->setIoExecutor(co_await asio::this_coro::executor);
    f.ctx->pluginMount = co_await f.mgr->mountAgentAsync(f.ctx);
    co_return f;
}

/// 等一次 emit 的完成通知 (回调是 C 函数指针, 状态放栈上; 调用方等 done)
struct EmitResult {
    bool        done   = false;
    int32_t     status = -1;
    std::string payload;
};

void PLUGINXX_CALL onEmitDone(void* ud, int32_t status, const PluginxxStringView* payload) {
    auto* r   = static_cast<EmitResult*>(ud);
    r->done   = true;
    r->status = status;
    if (payload != nullptr && payload->data != nullptr) {
        r->payload.assign(payload->data, static_cast<size_t>(payload->size));
    }
}

/// 轮询直到条件成立
asio::awaitable<bool> waitFor(std::function<bool()> cond, int timeoutMs = 3000) {
    auto       ex       = co_await asio::this_coro::executor;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMs};
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            co_return true;
        }
        asio::steady_timer timer(ex);
        timer.expires_after(std::chrono::milliseconds{5});
        utilxx_base::AsioErrorCode ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }
    co_return cond();
}

// ===========================================================================
// 1. 声明表: 两边都声明, 缓存策略与类型相符
// ===========================================================================
asio::awaitable<void> test_point_declarations() {
    auto f = co_await makeFixture();

    for (const auto id : points::kNotifyPoints) {
        const std::string key{id};
        XX_TEST_EXPECT_TRUE(f.mgr->capabilities()->declares(key));
        XX_TEST_EXPECT_TRUE(f.mgr->broadcasts()->declares(key));
        const auto* decl = f.mgr->broadcasts()->declOf(key);
        XX_TEST_EXPECT_TRUE(decl != nullptr);
        if (decl != nullptr) {
            // 通知型: 不缓存 (订阅方只关心"发生了"), 属主 = 主程序
            XX_TEST_EXPECT_TRUE(decl->cache == pluginxx::BroadcastCache_e::None);
            XX_TEST_EXPECT_TRUE(decl->owner.empty());
        }
    }
    for (const auto id : points::kValuePoints) {
        const std::string key{id};
        XX_TEST_EXPECT_TRUE(f.mgr->capabilities()->declares(key));
        XX_TEST_EXPECT_TRUE(f.mgr->broadcasts()->declares(key));
        const auto* decl = f.mgr->broadcasts()->declOf(key);
        XX_TEST_EXPECT_TRUE(decl != nullptr);
        if (decl != nullptr) {
            // 取值型: 保留当前值, 订阅时先推一次
            XX_TEST_EXPECT_TRUE(decl->cache == pluginxx::BroadcastCache_e::Latest);
            XX_TEST_EXPECT_TRUE(decl->pushOnSubscribe);
        }
    }
    // 能力侧是 `impls: many` + `merge: first` 的形状 (第一个给出非空结果的行生效)
    const auto info = f.mgr->capabilities()->infoJson(std::string{points::ContextSummarize});
    XX_TEST_EXPECT_FALSE(info.empty());
    XX_TEST_EXPECT_TRUE(info.find("many") != std::string::npos);
    co_return;
}

// ===========================================================================
// 2. 没有实现行 = 没意见: 完成回调 OK + "null"
// ===========================================================================
asio::awaitable<void> test_point_without_impl() {
    auto       f = co_await makeFixture();
    EmitResult r;
    auto*      handle = f.mgr->emitPointAsync(
        points::ContextCountTokens,
        /*agentId=*/{},
        R"({"kind":"estimate","model":"m"})",
        &onEmitDone,
        &r,
        nullptr
    );
    XX_TEST_EXPECT_TRUE(handle != nullptr);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&]() { return r.done; }));
    XX_TEST_EXPECT_EQ(r.status, int32_t{PLUGINXX_OPERATOR_OK});
    XX_TEST_EXPECT_EQ(r.payload, std::string{"null"});
    co_return;
}

// ===========================================================================
// 3. 通知面: 订阅同名频道 → 收到载荷; `publishPoint` 不等结果
// ===========================================================================
asio::awaitable<void> test_point_broadcast_face() {
    auto f = co_await makeFixture();
    std::vector<std::string> got;
    const std::string        topic{points::ToolEnd};
    const auto               token = f.mgr->broadcasts()->subscribe(
        topic,
        std::string{},
        [&got](std::string_view payload) { got.emplace_back(payload); }
    );
    XX_TEST_EXPECT_TRUE(token != 0);

    f.mgr->publishPoint(points::ToolEnd, {}, R"({"tool":"agentxx_filesystem_read","status":"ok"})");
    XX_TEST_EXPECT_TRUE(co_await waitFor([&]() { return !got.empty(); }));
    XX_TEST_EXPECT_EQ(got.size(), size_t{1});
    if (!got.empty()) {
        XX_TEST_EXPECT_TRUE(got.front().find("agentxx_filesystem_read") != std::string::npos);
    }
    f.mgr->broadcasts()->unsubscribe(topic, token);
    co_return;
}

// ===========================================================================
// 4. 取值型频道保留当前值
// ===========================================================================
asio::awaitable<void> test_point_value_cache() {
    auto              f = co_await makeFixture();
    const std::string topic{points::ContextSummarize};
    // 还没写过: 读不到值
    std::string value;
    XX_TEST_EXPECT_FALSE(f.mgr->broadcasts()->read(topic, &value));
    f.mgr->publishPoint(topic, {}, R"({"targetTokens":1000})");
    XX_TEST_EXPECT_TRUE(f.mgr->broadcasts()->read(topic, &value));
    XX_TEST_EXPECT_TRUE(value.find("1000") != std::string::npos);
    co_return;
}

// ===========================================================================
// 5. 应用能力 (非功能埋点): 主程序构造时已声明, 形状是 many + first + impl
// ===========================================================================
asio::awaitable<void> test_app_capability_declarations() {
    auto f = co_await makeFixture();
    for (const auto id : appcaps::kAppCapabilities) {
        const std::string key{id};
        XX_TEST_EXPECT_TRUE(f.mgr->capabilities()->declares(key));
        // 只声明、没人实现: `has` 看的是实现行, 因此这里必须为假
        XX_TEST_EXPECT_FALSE(f.mgr->capabilities()->has(key));
        const auto info = f.mgr->capabilities()->infoJson(key);
        XX_TEST_EXPECT_FALSE(info.empty());
        // 声明方是应用: 自省里 declaredBy 取值 "app" (插件声明时写的是插件名)
        XX_TEST_EXPECT_TRUE(info.find("many") != std::string::npos);
        XX_TEST_EXPECT_TRUE(info.find("\"declaredBy\":\"app\"") != std::string::npos);
    }
    co_return;
}

} // namespace

asio::awaitable<TestResult> run_plugin_point_tests() {
    g_pp_passed = 0;
    g_pp_failed = 0;
    co_await test_point_declarations();
    co_await test_point_without_impl();
    co_await test_point_broadcast_face();
    co_await test_point_value_cache();
    co_await test_app_capability_declarations();
    co_return TestResult{g_pp_passed, g_pp_failed};
}

} // namespace test
} // namespace agentxx
