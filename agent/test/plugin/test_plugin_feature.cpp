/// test_plugin_feature —— 功能埋点的插件面 (实现行 = 能力行)
///
/// 背景: 功能埋点框架侧的语义在 `feature_points` 覆盖, 管理器侧的基础在 `plugin_point`
/// 覆盖; 本模块验证**插件登记实现行 → 点取值时用到这一行**这条路上的事实一致。
///
/// 覆盖:
/// 1. 主程序点上的插件实现行排在核心实现之前并生效, 回答里的 `{"value": ...}`
///    被解开成业务值;
/// 2. 状态可逆: 禁用摘掉实现行后回落核心实现, 启用由 `start` 事务重新登记,
///    卸载后回到基线;
/// 3. 插件自己的功能埋点: 第一次登记即声明, `emitPoint` 拿得到同步行的值;
/// 4. 异步实现行 (`Task<T>`): 协程行真的等过 (耗时断言) 且给出值;
/// 5. 没有实现行 / 点未声明: `emitPoint` 按"没人实现 = 没意见"返回空;
///    核心实现不进能力调用链 (只由点自己调用)。
#include "agentxx-test/plugin/test_plugin_feature.h"

#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/feature/points.h"
#include "agentxx/feature/registry.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/api/agentxx_point_ids.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/tool_registry.h"
#include "asio/this_coro.hpp"
#include "utilxx_base/json.h"
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_pf_passed = 0;
int g_pf_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_pf_passed
#define XX_TEST_FAILED g_pf_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

namespace {

/// 定位插件库目录 (与 test_plugins.cpp / test_plugin_cleanup.cpp 同一策略)
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
///
/// 点注册表与上下文核心点在这里先加载 (与 BaseAgent::init 同一顺序:
/// 核心点先声明, 插件加载时才有地方登记实现), 插件层入口也按同一接线装上。
struct Fixture {
    std::shared_ptr<agentxx::agent::AgentContext>   ctx;
    std::shared_ptr<agentxx::plugin::PluginManager> mgr;
    /// 上下文核心点 (`agentxx.agent.context.countTokens` / `summarize`)
    agentxx::feature::ContextPoints points;
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
    f.points = agentxx::feature::registerContextPoints(*f.ctx->features, pointOptions);

    // 插件层入口: 与 BaseAgent 的 `feature_registry` 步骤同一接线
    std::weak_ptr<agentxx::agent::AgentContext> weakCtx = f.ctx;
    f.ctx->features->setPluginEmitter(
        [weakCtx](std::string_view pointId,
                  std::string_view payloadJson) -> asio::awaitable<std::optional<std::string>> {
            auto ctx = weakCtx.lock();
            if (nullptr == ctx || nullptr == ctx->pluginManager) {
                co_return std::nullopt;
            }
            const std::string agentId = ctx->agentId.empty() ? std::string{"root"} : ctx->agentId;
            co_return co_await ctx->pluginManager->emitPoint(agentId, pointId, payloadJson);
        }
    );

    f.mgr                = std::make_shared<agentxx::plugin::PluginManager>(f.ctx);
    f.ctx->pluginManager = f.mgr;
    // 加载 io executor (与 BaseAgent::init 一致): 跨线程 vtable 调用经真实 post
    f.mgr->setIoExecutor(co_await asio::this_coro::executor);
    // 建立挂载: 之后加载的插件挂到本 agent (运行期新加载的自动挂上)
    f.ctx->pluginMount = co_await f.mgr->mountAgentAsync(f.ctx);
    co_return f;
}

/// 等插件的主程序点实现行生效 (启用/加载事务投递到 io 线程, 轮询到能拿到值为止)
asio::awaitable<bool> waitRowActive(Fixture& f, int timeoutMs = 5000) {
    auto       ex       = co_await asio::this_coro::executor;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMs};
    while (std::chrono::steady_clock::now() < deadline) {
        auto answer = co_await f.mgr->emitPoint(
            "root",
            agentxx::plugin::points::ContextCountTokens,
            R"({"args":{"text":"probe"}})"
        );
        if (answer.has_value()) {
            co_return true;
        }
        asio::steady_timer timer(ex);
        timer.expires_after(std::chrono::milliseconds{10});
        utilxx_base::AsioErrorCode ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }
    co_return false;
}

/// 一次 token 估算的结果 (只留断言要用的字段)
struct TokenProbe {
    bool        ok    = false;
    int64_t     tokens = -1;
    std::string by;
    bool        fromCache = false;
    std::string error;
};

/// 取一次 `agentxx.agent.context.countTokens` (走真实点调用路径: 插件层 → 核心层)
asio::awaitable<TokenProbe> askTokens(Fixture& f, std::string_view text) {
    TokenProbe out;
    auto*      point = f.ctx->features->find(agentxx::feature::points::kContextCountTokens);
    if (point == nullptr) {
        co_return out;
    }
    auto* typed = dynamic_cast<agentxx::feature::ProvidePoint<
        agentxx::feature::CountTokensRequest,
        agentxx::feature::CountTokensValue>*>(point);
    if (typed == nullptr) {
        co_return out;
    }
    agentxx::feature::CountTokensRequest req;
    req.text = std::string{text}; ///< 身份由点自己按文本算出: 每处给不同文本
    auto resolved = co_await typed->ask(req);
    out.ok        = resolved.ok();
    out.by        = resolved.by;
    out.fromCache = resolved.fromCache;
    out.error     = agentxx::feature::callErrorKey(resolved.error);
    if (resolved.value.has_value()) {
        out.tokens = resolved.value->tokens;
    }
    co_return out;
}

/// 发一次功能埋点并解析结果 (空 = 没人给值)
///
/// `emitPoint` 是**能力调用链本身**, 拿到的就是实现行回的原文; 实现行习惯用
/// `{"value": ...}` 包一层 (`pointAnswer`), 这里按同一规则解开, 断言才看业务字段。
asio::awaitable<std::optional<utilxx_base::Json>>
    emitPointJson(Fixture& f, std::string_view pointId, std::string_view payloadJson) {
    auto answer = co_await f.mgr->emitPoint("root", pointId, payloadJson);
    if (!answer.has_value()) {
        co_return std::nullopt;
    }
    auto json = utilxx_base::catchError<std::optional<utilxx_base::Json>>(
        [&answer]() -> std::optional<utilxx_base::Json> {
            return utilxx_base::Json::parse(*answer);
        },
        [](std::string) -> std::optional<utilxx_base::Json> {
            return std::nullopt;
        }
    );
    if (!json.has_value() || !json->is_object()) {
        co_return json;
    }
    if (auto it = json->find("value"); it != json->end()) {
        co_return *it;
    }
    co_return json;
}

// ===========================================================================
// 1. 主程序点上的插件实现行: 生效 / 禁用回落 / 启用恢复 / 卸载回基线
// ===========================================================================
asio::awaitable<void> test_impl_row_lifecycle() {
    auto f = co_await makeFixture();

    // 加载前: 核心实现给值
    auto before = co_await askTokens(f, "probe text before plugin load");
    XX_TEST_EXPECT_TRUE(before.ok);
    XX_TEST_EXPECT_EQ(before.by, std::string{agentxx::feature::points::kOwnerCountTokens});
    XX_TEST_EXPECT_TRUE(before.tokens > 0);

    auto inst = co_await f.mgr->loadPluginAsync(findPluginDir("example_feature"));
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }

    // 加载后: 插件的实现行排在核心实现之前, 固定值 (便于断言), 回答里的
    // `{"value": {"tokens": 424242}}` 被解开成业务值
    auto withPlugin = co_await askTokens(f, "probe text with plugin loaded");
    XX_TEST_EXPECT_TRUE(withPlugin.ok);
    XX_TEST_EXPECT_EQ(withPlugin.by, std::string{"plugin"});
    XX_TEST_EXPECT_EQ(withPlugin.tokens, int64_t{424242});
    XX_TEST_EXPECT_FALSE(withPlugin.fromCache);

    // 插件登记的 3 行实现 (自己的两个点 + 主程序点) 都已生效: 上面已经看到插件给值
    // (注册本数看 `list_capabilities`, 不在统一注册清单里计)

    // 禁用: 实现行被摘除 → 回落核心实现
    f.mgr->disable("example_feature");
    auto afterDisable = co_await askTokens(f, "probe text after disable");
    XX_TEST_EXPECT_TRUE(afterDisable.ok);
    XX_TEST_EXPECT_EQ(afterDisable.by, std::string{agentxx::feature::points::kOwnerCountTokens});
    XX_TEST_EXPECT_TRUE(afterDisable.tokens > 0);

    // 启用: 生命周期重新驱动一次注册事务 (投递到 io 线程) → 又由插件给值
    f.mgr->enable("example_feature");
    XX_TEST_EXPECT_TRUE(co_await waitRowActive(f));
    auto afterEnable = co_await askTokens(f, "probe text after enable");
    XX_TEST_EXPECT_TRUE(afterEnable.ok);
    XX_TEST_EXPECT_EQ(afterEnable.by, std::string{"plugin"});
    XX_TEST_EXPECT_EQ(afterEnable.tokens, int64_t{424242});

    // 卸载: 回到核心实现
    XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("example_feature", std::chrono::seconds{5}));
    auto afterUnload = co_await askTokens(f, "probe text after unload");
    XX_TEST_EXPECT_TRUE(afterUnload.ok);
    XX_TEST_EXPECT_EQ(afterUnload.by, std::string{agentxx::feature::points::kOwnerCountTokens});
    co_return;
}

// ===========================================================================
// 2. 插件自己的功能埋点: 声明与实现, 同步行 / 异步行
// ===========================================================================
asio::awaitable<void> test_plugin_own_points() {
    auto f = co_await makeFixture();

    auto inst = co_await f.mgr->loadPluginAsync(findPluginDir("example_feature"));
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }

    // 同步实现行: 载荷里的 args 被实现读到, 结果就是它返回的业务 JSON
    {
        auto json = co_await emitPointJson(
            f,
            "plugin.example_feature.beat",
            R"({"point":"plugin.example_feature.beat","args":{"path":"beats.wav","minBpm":90}})"
        );
        XX_TEST_EXPECT_TRUE(json.has_value());
        if (json.has_value()) {
            XX_TEST_EXPECT_TRUE(json->is_object());
            XX_TEST_EXPECT_EQ(json->value<std::string>("path", ""), std::string{"beats.wav"});
            XX_TEST_EXPECT_EQ(json->value<int>("bpm", 0), 128);
            XX_TEST_EXPECT_TRUE(json->value<bool>("confident", false));
        }
    }

    // 异步实现行: 协程里等了 60ms 才回答 (结果里带回入参, 耗时也真的等过)
    {
        const auto begin = std::chrono::steady_clock::now();
        auto       json
            = co_await emitPointJson(f, "plugin.example_feature.slow", R"({"args":{"input":7}})");
        const auto cost = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - begin
        )
                              .count();
        XX_TEST_EXPECT_TRUE(json.has_value());
        if (json.has_value()) {
            XX_TEST_EXPECT_EQ(json->value<int64_t>("input", 0), int64_t{7});
            XX_TEST_EXPECT_GE(json->value<int64_t>("counter", 0), int64_t{1});
        }
        XX_TEST_EXPECT_GE(cost, int64_t{50});
    }

    // 没声明的点: 受理即失败 → 按"没人给值"返回
    {
        auto json = co_await emitPointJson(f, "plugin.example_feature.nope", "{}");
        XX_TEST_EXPECT_FALSE(json.has_value());
    }

    XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("example_feature", std::chrono::seconds{5}));
    co_return;
}

// ===========================================================================
// 3. 没有插件实现行时: 核心实现不进能力调用链, 点自己照常给值
// ===========================================================================
asio::awaitable<void> test_no_impl_rows() {
    auto f = co_await makeFixture();

    // 能力调用链上没有实现行 → "没人给值" (这里用的是插件面的功能埋点 id)
    auto emitted = co_await emitPointJson(
        f,
        agentxx::plugin::points::ContextCountTokens,
        R"({"args":{"text":"hello"}})"
    );
    XX_TEST_EXPECT_FALSE(emitted.has_value());

    // 但点本身的核心实现照常给值 (核心实现是库内的 C++ 调用, 不进能力调用链)
    auto asked = co_await askTokens(f, "hello");
    XX_TEST_EXPECT_TRUE(asked.ok);
    XX_TEST_EXPECT_EQ(asked.by, std::string{agentxx::feature::points::kOwnerCountTokens});
    XX_TEST_EXPECT_TRUE(asked.tokens > 0);
    co_return;
}

} // namespace

// ===========================================================================
// 模块入口
// ===========================================================================
asio::awaitable<TestResult> run_plugin_feature_tests() {
    g_pf_passed = 0;
    g_pf_failed = 0;
    TestResult result;

    co_await test_impl_row_lifecycle();
    co_await test_plugin_own_points();
    co_await test_no_impl_rows();

    result.passed = g_pf_passed;
    result.failed = g_pf_failed;
    co_return result;
}

} // namespace test
} // namespace agentxx
