///
/// 通知型功能埋点的派发 —— 7 处调用点各发一次广播 (`agentxx.agent.point.*`)
///
/// 插件参与方式只有一种: **订阅同名频道**。主程序侧只保留一个派发器
/// (中间件句柄 `plugin_points`, 建挂载时挂上), 它把每个调用点转成一次广播。
///
/// 覆盖:
/// 1. 点清单: 7 个点的 id / 名称 / 顺序 (`hooksJson`), 派发记录只在开发者模式下出现
/// 2. 派发: 每个点都发到自己的频道, 载荷是 `{agentId, sessionId, point}`
/// 3. 没有订阅方时派发不做事、不崩
/// 4. 派发器常驻: 建挂载即有、插件禁用/卸载/轮末清理都不摘, 拆挂载才摘;
///    重复挂载不重复加句柄; 链上那个句柄的回调就是派发入口
/// 5. 开发者模式: 派发次数与上次耗时被记录
/// 6. 多挂载: 清单按挂载分开 (`hooksJson` 变成数组, 每个挂载一份)
#include "agentxx-test/plugin/test_plugin_dispatch.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/feature/points.h"
#include "agentxx/feature/registry.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/api/agentxx_point_ids.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/tool_registry.h"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "neograph/graph/node.h"
#include "neograph/graph/registry.h"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见)
int g_pd_passed = 0;
int g_pd_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_pd_passed
#define XX_TEST_FAILED g_pd_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

namespace {

/// 定位插件库目录 (与其他插件测试同一策略: exe 同目录优先, cwd 回退)
std::string findPluginDir(const char* pluginName) {
    std::error_code       ec;
    std::vector<fs::path> candidates;
    if (auto exeDir = executableDir()) {
        candidates.push_back(*exeDir / "plugins" / pluginName);
    }
    candidates.push_back(fs::current_path(ec) / "plugins" / pluginName);
    auto hasLibFile = [](const fs::path& dir) {
        std::error_code                     ec2;
        std::filesystem::directory_iterator it(dir, ec2);
        std::filesystem::directory_iterator end;
        for (; it != end; it.increment(ec2)) {
            auto ext = it->path().extension().string();
            if (ext == ".so" || ext == ".dll" || ext == ".dylib") {
                return true;
            }
        }
        return false;
    };
    for (const auto& c : candidates) {
        if (fs::is_directory(c, ec) && hasLibFile(c)) {
            return c.string();
        }
    }
    return std::string{"plugins/"} + pluginName;
}

/// 最小主程序夹具: AgentContext + 中间件上下文 + 总线 + 工具注册表 + 功能埋点注册表
/// + 插件管理器 + 一份挂载
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
    f.ctx->toolRegistry  = std::make_shared<agentxx::plugin::ToolRegistry>();
    f.ctx->graphRegistry = std::make_shared<neograph::graph::GraphRegistry>();

    f.ctx->features = std::make_shared<agentxx::feature::Registry>();
    agentxx::feature::ContextPointOptions pointOptions;
    agentxx::feature::registerContextPoints(*f.ctx->features, pointOptions);

    f.mgr                = std::make_shared<agentxx::plugin::PluginManager>(f.ctx);
    f.ctx->pluginManager = f.mgr;
    f.mgr->setIoExecutor(co_await asio::this_coro::executor);
    f.ctx->pluginMount = co_await f.mgr->mountAgentAsync(f.ctx);
    co_return f;
}

/// 收尾: 拆全部挂载并放掉上下文上的挂载句柄 (agent 关掉时本来就这么做)
asio::awaitable<void> teardown(Fixture& f) {
    if (f.mgr != nullptr) {
        co_await f.mgr->unmountAllAgentsAsync();
    }
    if (f.ctx != nullptr) {
        f.ctx->pluginMount.reset();
    }
}

/// 轮询等待条件成立 (插件侧登记与生命周期事务都投递到 io 线程)
asio::awaitable<bool> waitFor(std::function<bool()> cond, int timeoutMs = 5000) {
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

/// 链上是否有一份派发器 (wantDisabled 指定期望的停用状态)
bool hasDispatchHandle(Fixture& f, bool wantDisabled = false) {
    if (!f.ctx->middlewareHandleContext) {
        return false;
    }
    const auto& handles = f.ctx->middlewareHandleContext->handles;
    return std::any_of(handles.begin(), handles.end(), [&](const auto& h) {
        return h->name == "plugin_points" && h->disabled == wantDisabled;
    });
}

/// 派发器在链上的副本数
size_t dispatchHandleCount(Fixture& f) {
    if (!f.ctx->middlewareHandleContext) {
        return 0;
    }
    const auto& handles = f.ctx->middlewareHandleContext->handles;
    return static_cast<size_t>(std::count_if(handles.begin(), handles.end(), [](const auto& h) {
        return h->name == "plugin_points";
    }));
}

/// 一次派发用的最小图输入 (与中间件派发器收到的形状一致)
neograph::graph::NodeInput makeNodeInput(
    neograph::graph::GraphState& state,
    neograph::graph::RunContext& runCtx,
    std::string_view             sessionId = "dispatch_test_session"
) {
    runCtx.thread_id = std::string{sessionId};
    return neograph::graph::NodeInput{state, runCtx, nullptr};
}

/// 一个频道收到的载荷
struct ChannelCapture {
    std::vector<std::string> payloads;
};

/// 订阅 + 自动退订 (回调捕获的是本对象, 出作用域必须退订)
struct Subscription {
    agentxx::plugin::PluginManager*   mgr   = nullptr;
    std::string                       id{};
    uint64_t                          token = 0;
    std::shared_ptr<ChannelCapture>   capture;

    Subscription() = default;
    Subscription(const Subscription&)            = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&& other) noexcept :
        mgr(other.mgr), id(std::move(other.id)), token(other.token),
        capture(std::move(other.capture)) {
        other.mgr   = nullptr;
        other.token = 0;
    }
    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) {
            // 先把手上这份退订, 再接过来
            if (mgr != nullptr && token != 0) {
                mgr->broadcasts()->unsubscribe(id, token);
            }
            mgr      = other.mgr;
            id       = std::move(other.id);
            token    = other.token;
            capture  = std::move(other.capture);
            other.mgr   = nullptr;
            other.token = 0;
        }
        return *this;
    }
    ~Subscription() {
        if (mgr != nullptr && token != 0) {
            mgr->broadcasts()->unsubscribe(id, token);
        }
    }

    /// 订阅一个频道 (失败时 token 保持 0)
    static Subscription
        open(agentxx::plugin::PluginManager& mgr, std::string_view id, std::string* error = nullptr) {
        Subscription sub;
        sub.mgr     = &mgr;
        sub.id      = std::string{id};
        sub.capture = std::make_shared<ChannelCapture>();
        auto capture = sub.capture;
        sub.token    = mgr.broadcasts()->subscribe(
            sub.id,
            std::string{},
            [capture](std::string_view data) { capture->payloads.emplace_back(data); },
            {},
            error
        );
        return sub;
    }

    const std::vector<std::string>& payloads() const {
        return capture->payloads;
    }
};

/// 7 个通知型点: 点名称与 id (顺序与 PluginMount::HookPoint 一致)
struct PointRow {
    agentxx::plugin::PluginMount::HookPoint point;
    std::string_view                       id;
    std::string_view                       name;
};

const std::array<PointRow, agentxx::plugin::PluginMount::kHookPointCount> kPointRows{{
    {agentxx::plugin::PluginMount::HookPoint::TurnStart, agentxx::plugin::points::TurnStart, "TURN_START"},
    {agentxx::plugin::PluginMount::HookPoint::TurnEnd, agentxx::plugin::points::TurnEnd, "TURN_END"},
    {agentxx::plugin::PluginMount::HookPoint::ModelStart, agentxx::plugin::points::ModelStart, "MODEL_START"},
    {agentxx::plugin::PluginMount::HookPoint::ModelRun, agentxx::plugin::points::ModelRun, "MODEL_RUN"},
    {agentxx::plugin::PluginMount::HookPoint::ModelEnd, agentxx::plugin::points::ModelEnd, "MODEL_END"},
    {agentxx::plugin::PluginMount::HookPoint::ToolStart, agentxx::plugin::points::ToolStart, "TOOL_START"},
    {agentxx::plugin::PluginMount::HookPoint::ToolEnd, agentxx::plugin::points::ToolEnd, "TOOL_END"},
}};

} // namespace

// ===========================================================================
// 1. 点清单: id / 名称 / 顺序, 派发记录只在开发者模式下出现
// ===========================================================================
namespace {

asio::awaitable<void> test_point_manifest() {
    const bool devModeBefore = agentxx::agent::AgentConfigStatic::devMode;
    agentxx::agent::AgentConfigStatic::devMode = false;

    auto f = co_await makeFixture();
    XX_TEST_EXPECT_TRUE(f.ctx->pluginMount != nullptr);
    if (!f.ctx->pluginMount) {
        agentxx::agent::AgentConfigStatic::devMode = devModeBefore;
        co_return;
    }

    utilxx_base::Json manifest;
    try {
        manifest = utilxx_base::Json::parse(f.ctx->pluginMount->hooksJson());
    } catch (const std::exception& e) {
        TEST_FAIL << "[plugin_dispatch] hooksJson 解析失败: " << e.what() << std::endl;
    }
    XX_TEST_EXPECT_TRUE(manifest.is_object());
    XX_TEST_EXPECT_EQ(manifest.value("count", 0), 7);
    XX_TEST_EXPECT_FALSE(manifest.value("devMode", true));
    XX_TEST_EXPECT_TRUE(manifest.contains("points"));
    if (manifest.contains("points") && manifest["points"].is_array()) {
        const auto& points = manifest["points"];
        XX_TEST_EXPECT_EQ(points.size(), kPointRows.size());
        for (size_t i = 0; i < points.size() && i < kPointRows.size(); ++i) {
            // 顺序 = HookPoint 枚举顺序; 名称与 id 与 agentxx_point_ids.h 一致
            XX_TEST_EXPECT_EQ(points[i].value("id", std::string{}), std::string{kPointRows[i].id});
            XX_TEST_EXPECT_EQ(
                points[i].value("name", std::string{}),
                std::string{kPointRows[i].name}
            );
            // 非开发者模式: 没有派发记录 (不是"全 0")
            XX_TEST_EXPECT_FALSE(points[i].contains("stat"));
        }
    }

    // 管理器入口: 单挂载时与挂载视图同源
    XX_TEST_EXPECT_EQ(
        utilxx_base::Json::parse(f.mgr->hooksJson()).value("count", 0),
        7
    );

    agentxx::agent::AgentConfigStatic::devMode = devModeBefore;
    co_await teardown(f);
}

} // namespace

// ===========================================================================
// 2. 派发: 每个点都发到自己的频道, 载荷是 {agentId, sessionId, point}
// ===========================================================================
namespace {

asio::awaitable<void> test_dispatch_payload_per_point() {
    auto f = co_await makeFixture();
    if (!f.ctx->pluginMount) {
        co_return;
    }
    const std::string agentId{f.ctx->pluginMount->agentId()};

    std::vector<Subscription> subs;
    subs.reserve(kPointRows.size());
    for (const auto& row : kPointRows) {
        std::string error;
        subs.push_back(Subscription::open(*f.mgr, row.id, &error));
        XX_TEST_EXPECT_TRUE(subs.back().token != 0);
        if (subs.back().token == 0) {
            XX_TEST_EXPECT_TRUE(error.empty() == false);
        }
    }

    neograph::graph::GraphState state;
    neograph::graph::RunContext runCtx;
    auto                        in = makeNodeInput(state, runCtx, "sess-dispatch");
    for (const auto& row : kPointRows) {
        co_await f.ctx->pluginMount->dispatchHook(row.point, in);
    }

    for (size_t i = 0; i < kPointRows.size(); ++i) {
        const auto& payloads = subs[i].payloads();
        // 每个点只发到自己的频道 (1 次, 不串台)
        XX_TEST_EXPECT_EQ(payloads.size(), size_t{1});
        if (payloads.empty()) {
            continue;
        }
        utilxx_base::Json payload;
        try {
            payload = utilxx_base::Json::parse(payloads[0]);
        } catch (const std::exception&) {
            XX_TEST_EXPECT_TRUE(false);
            continue;
        }
        XX_TEST_EXPECT_EQ(payload.value("point", std::string{}), std::string{kPointRows[i].name});
        XX_TEST_EXPECT_EQ(payload.value("agentId", std::string{}), agentId);
        XX_TEST_EXPECT_EQ(payload.value("sessionId", std::string{}), std::string{"sess-dispatch"});
        // 不带会话内容 (载荷不随上下文增长)
        XX_TEST_EXPECT_FALSE(payload.contains("messages"));
    }
    co_await teardown(f);
}

} // namespace

// ===========================================================================
// 3. 没有订阅方时派发不做事、不崩
// ===========================================================================
namespace {

asio::awaitable<void> test_dispatch_without_subscriber() {
    auto f = co_await makeFixture();
    if (!f.ctx->pluginMount) {
        co_return;
    }
    neograph::graph::GraphState state;
    neograph::graph::RunContext runCtx;
    auto                        in = makeNodeInput(state, runCtx);
    // 一个订阅都没有: 每个点都是"发出去没人听", 不抛异常
    for (const auto& row : kPointRows) {
        co_await f.ctx->pluginMount->dispatchHook(row.point, in);
    }
    XX_TEST_EXPECT_TRUE(true);

    // 空 agentId 的派发 (只发通知面) 也不崩
    f.mgr->publishPoint(agentxx::plugin::points::TurnStart, std::string{}, "{}");
    XX_TEST_EXPECT_TRUE(true);
    co_await teardown(f);
}

} // namespace

// ===========================================================================
// 4. 派发器常驻: 建挂载即有; 插件禁用/卸载/轮末清理都不摘; 拆挂载才摘
// ===========================================================================
namespace {

asio::awaitable<void> test_dispatcher_lifecycle() {
    auto f = co_await makeFixture();
    if (!f.ctx->pluginMount) {
        co_return;
    }
    // 建挂载就挂上派发器 (加载插件之前)
    XX_TEST_EXPECT_TRUE(hasDispatchHandle(f));
    XX_TEST_EXPECT_EQ(dispatchHandleCount(f), size_t{1});

    // 重复挂载幂等: 还是那一份
    const auto sameMount = co_await f.mgr->mountAgentAsync(f.ctx);
    XX_TEST_EXPECT_TRUE(sameMount == f.ctx->pluginMount);
    XX_TEST_EXPECT_EQ(dispatchHandleCount(f), size_t{1});

    // 插件禁用 / 卸载 / 轮末清理都不动它
    auto inst = co_await f.mgr->loadPluginAsync(findPluginDir("example_plugin"));
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (inst) {
        XX_TEST_EXPECT_TRUE(hasDispatchHandle(f));
        f.mgr->onTurnBegin();
        f.mgr->disable("example_plugin");
        XX_TEST_EXPECT_TRUE(hasDispatchHandle(f));
        f.mgr->flushPendingCleanup();
        f.mgr->onTurnEnd();
        XX_TEST_EXPECT_TRUE(hasDispatchHandle(f));
        f.mgr->enable("example_plugin");
        XX_TEST_EXPECT_TRUE(co_await waitFor([&]() { return inst->enabled; }));
        XX_TEST_EXPECT_TRUE(hasDispatchHandle(f));
        XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("example_plugin", std::chrono::seconds{5}));
        f.mgr->flushPendingCleanup();
        XX_TEST_EXPECT_TRUE(hasDispatchHandle(f));
        XX_TEST_EXPECT_EQ(dispatchHandleCount(f), size_t{1});
    }

    // 拆挂载: 派发器属于挂载, 一起摘掉; 重新挂载只再加一份
    co_await f.mgr->unmountAgentAsync(f.ctx->pluginMount->agentId());
    XX_TEST_EXPECT_FALSE(hasDispatchHandle(f));
    f.ctx->pluginMount = co_await f.mgr->mountAgentAsync(f.ctx);
    XX_TEST_EXPECT_TRUE(f.ctx->pluginMount != nullptr);
    XX_TEST_EXPECT_EQ(dispatchHandleCount(f), size_t{1});
    co_await teardown(f);
}

} // namespace

// ===========================================================================
// 5. 链上那个句柄的回调就是派发入口
// ===========================================================================
namespace {

asio::awaitable<void> test_dispatcher_entry_on_chain() {
    auto f = co_await makeFixture();
    if (!f.ctx->pluginMount) {
        co_return;
    }
    std::string error;
    auto        sub = Subscription::open(*f.mgr, agentxx::plugin::points::ToolStart, &error);
    XX_TEST_EXPECT_TRUE(sub.token != 0);

    // 从链上取出真正的句柄并调它的回调 (中间件链就是这样进入派发的)
    std::shared_ptr<agentxx::middleware::BaseMiddlewareHandleInterface> found;
    for (const auto& h : f.ctx->middlewareHandleContext->handles) {
        if (h->name == "plugin_points") {
            found = h;
            break;
        }
    }
    XX_TEST_EXPECT_TRUE(found != nullptr);
    if (!found) {
        co_return;
    }
    auto dispatch = std::dynamic_pointer_cast<agentxx::plugin::PluginHookDispatchHandle>(found);
    XX_TEST_EXPECT_TRUE(dispatch != nullptr);
    if (!dispatch) {
        co_return;
    }

    neograph::graph::GraphState state;
    neograph::graph::RunContext runCtx;
    auto                        in = makeNodeInput(state, runCtx, "sess-entry");
    co_await dispatch->onToolcallStartFunc(in);
    XX_TEST_EXPECT_EQ(sub.payloads().size(), size_t{1});
    if (!sub.payloads().empty()) {
        const auto payload = utilxx_base::Json::parse(sub.payloads()[0]);
        XX_TEST_EXPECT_EQ(payload.value("point", std::string{}), std::string{"TOOL_START"});
        XX_TEST_EXPECT_EQ(payload.value("sessionId", std::string{}), std::string{"sess-entry"});
    }

    // End 形状的回调 (第二参数是节点输出) 也要派发自己的点
    auto                             endSub = Subscription::open(*f.mgr, agentxx::plugin::points::ToolEnd);
    neograph::graph::NodeOutput      output;
    co_await dispatch->onToolcallEndFunc(in, output);
    XX_TEST_EXPECT_EQ(endSub.payloads().size(), size_t{1});
    if (!endSub.payloads().empty()) {
        const auto payload = utilxx_base::Json::parse(endSub.payloads()[0]);
        XX_TEST_EXPECT_EQ(payload.value("point", std::string{}), std::string{"TOOL_END"});
    }
    co_await teardown(f);
}

} // namespace

// ===========================================================================
// 6. 开发者模式: 派发次数与上次耗时被记录
// ===========================================================================
namespace {

asio::awaitable<void> test_dev_mode_stats() {
    const bool devModeBefore = agentxx::agent::AgentConfigStatic::devMode;
    agentxx::agent::AgentConfigStatic::devMode = true;

    auto f = co_await makeFixture();
    if (!f.ctx->pluginMount) {
        agentxx::agent::AgentConfigStatic::devMode = devModeBefore;
        co_return;
    }
    neograph::graph::GraphState state;
    neograph::graph::RunContext runCtx;
    auto                        in = makeNodeInput(state, runCtx);
    co_await f.ctx->pluginMount->dispatchHook(
        agentxx::plugin::PluginMount::HookPoint::ModelRun,
        in
    );
    co_await f.ctx->pluginMount->dispatchHook(
        agentxx::plugin::PluginMount::HookPoint::ModelRun,
        in
    );

    const auto manifest = utilxx_base::Json::parse(f.ctx->pluginMount->hooksJson());
    XX_TEST_EXPECT_TRUE(manifest.value("devMode", false));
    bool statFound = false;
    for (const auto& point : manifest["points"]) {
        if (point.value("name", std::string{}) != "MODEL_RUN") {
            continue;
        }
        statFound = true;
        XX_TEST_EXPECT_TRUE(point.contains("stat"));
        const auto& stat = point["stat"];
        XX_TEST_EXPECT_EQ(stat.value("dispatches", 0), 2);
        XX_TEST_EXPECT_GE(stat.value("lastMs", 0), 0);
        break;
    }
    XX_TEST_EXPECT_TRUE(statFound);

    // 其它点没有派发过: 记录是 0 (不是缺字段)
    for (const auto& point : manifest["points"]) {
        if (point.value("name", std::string{}) != "TURN_START") {
            continue;
        }
        XX_TEST_EXPECT_EQ(point["stat"].value("dispatches", -1), 0);
        break;
    }

    agentxx::agent::AgentConfigStatic::devMode = devModeBefore;
    co_await teardown(f);
}

} // namespace

// ===========================================================================
// 7. 多挂载: 清单按挂载分开
// ===========================================================================
namespace {

asio::awaitable<void> test_manifest_per_mount() {
    auto f = co_await makeFixture();
    if (!f.ctx->pluginMount) {
        co_return;
    }
    // 第二个 agent (同一个管理器)
    auto ctx2                          = std::make_shared<agentxx::agent::AgentContext>();
    ctx2->agentId                      = "agent_two";
    ctx2->agentConfig                  = std::make_shared<agentxx::agent::AgentConfig>();
    ctx2->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
    ctx2->bus           = std::make_shared<agentxx::events::EventBus>(co_await asio::this_coro::executor);
    ctx2->toolRegistry  = std::make_shared<agentxx::plugin::ToolRegistry>();
    ctx2->graphRegistry = std::make_shared<neograph::graph::GraphRegistry>();
    ctx2->features      = std::make_shared<agentxx::feature::Registry>();
    ctx2->pluginManager = f.mgr;
    ctx2->pluginManagerOwned = false;
    ctx2->pluginMount        = co_await f.mgr->mountAgentAsync(ctx2);
    XX_TEST_EXPECT_TRUE(ctx2->pluginMount != nullptr);
    if (!ctx2->pluginMount) {
        co_return;
    }
    // 每个 agent 的链上各有一份派发器
    XX_TEST_EXPECT_EQ(dispatchHandleCount(f), size_t{1});
    XX_TEST_EXPECT_TRUE(std::any_of(
        ctx2->middlewareHandleContext->handles.begin(),
        ctx2->middlewareHandleContext->handles.end(),
        [](const auto& h) { return h->name == "plugin_points"; }
    ));

    // 多挂载时管理器入口给数组 (每个挂载一份 7 个点)
    const auto manifest = utilxx_base::Json::parse(f.mgr->hooksJson());
    XX_TEST_EXPECT_TRUE(manifest.is_array());
    XX_TEST_EXPECT_EQ(manifest.size(), size_t{2});
    for (const auto& item : manifest) {
        XX_TEST_EXPECT_EQ(item.value("count", 0), 7);
    }

    // 派发只在被派发的那份挂载所在 agent 的总线上 (另一份收不到)
    std::string error;
    auto        subA = Subscription::open(*f.mgr, agentxx::plugin::points::TurnStart, &error);
    XX_TEST_EXPECT_TRUE(subA.token != 0);
    neograph::graph::GraphState state;
    neograph::graph::RunContext runCtx;
    auto                        in = makeNodeInput(state, runCtx, "sess-two");
    co_await ctx2->pluginMount->dispatchHook(
        agentxx::plugin::PluginMount::HookPoint::TurnStart,
        in
    );
    XX_TEST_EXPECT_EQ(subA.payloads().size(), size_t{1});
    if (!subA.payloads().empty()) {
        const auto payload = utilxx_base::Json::parse(subA.payloads()[0]);
        XX_TEST_EXPECT_EQ(
            payload.value("agentId", std::string{}),
            std::string{ctx2->pluginMount->agentId()}
        );
    }

    ctx2->pluginMount.reset();
    co_await teardown(f);
}

} // namespace

// ===========================================================================
// 模块入口
// ===========================================================================

asio::awaitable<TestResult> run_plugin_dispatch_tests() {
    TEST_INFO << "[plugin_dispatch] 通知型功能埋点派发 (agentxx.agent.point.*)" << std::endl;
    try {
        co_await test_point_manifest();
        co_await test_dispatch_payload_per_point();
        co_await test_dispatch_without_subscriber();
        co_await test_dispatcher_lifecycle();
        co_await test_dispatcher_entry_on_chain();
        co_await test_dev_mode_stats();
        co_await test_manifest_per_mount();
    } catch (const std::exception& e) {
        TEST_FAIL << "[plugin_dispatch] 用例异常: " << e.what() << std::endl;
        ++g_pd_failed;
    } catch (...) {
        TEST_FAIL << "[plugin_dispatch] 用例未知异常" << std::endl;
        ++g_pd_failed;
    }
    co_return TestResult{g_pd_passed, g_pd_failed};
}

} // namespace test
} // namespace agentxx
