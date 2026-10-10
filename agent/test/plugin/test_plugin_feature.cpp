/// test_plugin_feature —— 功能点的插件面 (接口表 `agentxx.agent.feature` + SDK kit)
///
/// 背景: 功能点子系统的 C++ 侧行为已在 `feature_points` 模块覆盖; 本模块验证
/// **插件经 C ABI 走一遍**时的事实一致 —— 插件声明的点、登记的实现落到同一份
/// 注册表, 插件实现按层序排在核心实现之前, 按名调用的结果 JSON 与失败原因完整。
///
/// 覆盖:
/// 1. 声明与登记: 插件点在注册表里 (归属 / 可调 / 实现层), 实例视图与注册清单
///    计数, 清单 JSON 含插件点 (插件 `list_points` 与装配快照共用一份事实)
/// 2. 覆盖核心点: 插件实现排在 core 之前并生效; 摘除后自动回落核心实现
///    (含按归属失效值缓存)
/// 3. 按名调用 (`call_point_async`): 同步实现 / 协程实现 / 实现超时按没意见继续 /
///    无实现 / 显式置空 / 强类型点拒绝受理 / 点不存在 / 并发重入 busy
/// 4. 零副作用契约: `call` 不写值缓存、不记置空标记 (`ask` 才记)
/// 5. 生命周期: 禁用摘净、启用按 start 重新声明、卸载后注册表回到基线
#include "agentxx-test/plugin/test_plugin_feature.h"

#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/feature/points.h"
#include "agentxx/feature/registry.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/tool_registry.h"
#include "neograph/graph/registry.h"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"
#include <chrono>
#include <filesystem>
#include <functional>
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

/// 最小宿主夹具: AgentContext + 中间件上下文 + 总线 + 工具注册表 + 功能点注册表
///
/// 功能点注册表与上下文核心点在这里先装配 (与 BaseAgent::init 同一顺序:
/// 核心点先声明, 插件装载时才有地方登记实现)。
struct Fixture {
    std::shared_ptr<agentxx::agent::AgentContext>   ctx;
    std::shared_ptr<agentxx::plugin::PluginManager> mgr;
    /// 上下文核心点 (`agentxx.context.countTokens` / `summarize`)
    agentxx::feature::ContextPoints points;
};

asio::awaitable<Fixture> makeFixture() {
    Fixture f;
    f.ctx                          = std::make_shared<agentxx::agent::AgentContext>();
    f.ctx->agentConfig              = std::make_shared<agentxx::agent::AgentConfig>();
    f.ctx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
    f.ctx->bus = std::make_shared<agentxx::events::EventBus>(co_await asio::this_coro::executor);
    f.ctx->toolRegistry  = std::make_shared<agentxx::plugin::ToolRegistry>();
    f.ctx->graphRegistry = std::make_shared<neograph::graph::GraphRegistry>();

    f.ctx->features = std::make_shared<agentxx::feature::Registry>();
    agentxx::feature::ContextPointOptions pointOptions;
    f.points = agentxx::feature::registerContextPoints(*f.ctx->features, pointOptions);

    f.mgr                = std::make_shared<agentxx::plugin::PluginManager>(f.ctx);
    f.ctx->pluginManager = f.mgr;
    // 装配 io executor (与 BaseAgent::init 一致): 跨线程 vtable 调用经真实 post
    f.mgr->setIoExecutor(co_await asio::this_coro::executor);
    co_return f;
}

/// 轮询等待条件成立 (插件侧调用与生命周期事务都投递到 io 线程)
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

/// 一次按名调用的完成探针 (完成回调在宿主 IO 线程发布)
struct CallProbe {
    bool        done    = false;
    int32_t     status  = 0;
    std::string payload;
};

void PLUGINXX_CALL onFeatureCall(void* ud, int32_t status, const PluginxxStringView* payload) {
    auto* probe   = static_cast<CallProbe*>(ud);
    probe->done   = true;
    probe->status = status;
    probe->payload.clear();
    if (payload != nullptr && payload->data != nullptr && payload->size > 0) {
        probe->payload.assign(payload->data, static_cast<size_t>(payload->size));
    }
}

/// 按名调用一次功能点的结果 (受理 + 回包)
struct CallOutcome {
    bool        accepted = false; ///< 受理成功 (拿到操作句柄)
    std::string acceptError;      ///< 受理失败原因 (accepted = false 时有值)
    bool        finished = false; ///< 是否等到完成回调
    int32_t     status   = 0;     ///< 终态 (PLUGINXX_OPERATOR_*)
    std::string payload;          ///< 结果 JSON 文本
    bool        ok       = false; ///< 结果 JSON 的 ok
    std::string error;            ///< 结果 JSON 的 error (失败时的错误码字符串)
};

/// 经宿主入口调用一个功能点并等结果
asio::awaitable<CallOutcome>
    callPoint(Fixture& f, agentxx::plugin::PluginInstance* caller, std::string_view pointId, std::string_view argsJson) {
    CallOutcome out;
    CallProbe   probe;
    auto        idSv   = agentxx::plugin::PluginStringView::from(pointId.data(), pointId.size());
    auto        argsSv = agentxx::plugin::PluginStringView::from(argsJson.data(), argsJson.size());
    PluginxxString err{nullptr, 0};
    auto* handle = f.mgr->callFeatureAsync(caller, idSv, argsSv, &onFeatureCall, &probe, &err);
    if (handle == nullptr) {
        out.accepted = false;
        if (err.data != nullptr && err.size > 0) {
            out.acceptError.assign(err.data, static_cast<size_t>(err.size));
        }
        if (err.data != nullptr) {
            agentxx::plugin::PluginString::free(caller->hostView(), &err);
        }
        co_return out;
    }
    out.accepted = true;
    out.finished = co_await waitFor([&probe] {
        return probe.done;
    });
    out.status  = probe.status;
    out.payload = probe.payload;
    if (!out.payload.empty()) {
        auto json = utilxx_base::catchError<std::optional<utilxx_base::Json>>(
            [&]() -> std::optional<utilxx_base::Json> {
                return utilxx_base::Json::parse(out.payload);
            },
            [](std::string) -> std::optional<utilxx_base::Json> {
                return std::nullopt;
            }
        );
        if (json.has_value() && json->is_object()) {
            out.ok    = json->value<bool>("ok", false);
            out.error = json->value<std::string>("error", "");
        }
    }
    co_return out;
}

/// 声明一个宿主 JSON 点 (可调; 不缓存), 返回点引用
agentxx::feature::JsonProvidePoint&
    declareHostPoint(Fixture& f, std::string_view id) {
    agentxx::feature::PointOptions opts;
    opts.title    = std::string{id};
    opts.depict   = "测试用的宿主点";
    opts.callable = true;
    opts.cache    = agentxx::feature::CacheMode::None;
    return f.ctx->features->provideJson(id, std::move(opts), "core");
}

/// 登记一个同步的插件层实现 (返回给定的回答文本)
int32_t addSyncPluginImpl(
    agentxx::feature::PointBase&        point,
    std::string_view                    owner,
    std::string_view                    answer
) {
    agentxx::feature::ImplSpec spec;
    spec.layer  = agentxx::feature::ImplLayer::Plugin;
    spec.owner  = std::string{owner};
    spec.load   = "dynamic";
    spec.fn     = [answer = std::string{answer}](std::string) -> asio::awaitable<std::optional<std::string>> {
        co_return std::optional<std::string>{answer};
    };
    return point.addImpl(std::move(spec));
}

} // namespace

// ===========================================================================
// 1. 声明与登记: 插件点在注册表里, 计数与清单一致
// ===========================================================================
namespace {

asio::awaitable<void> test_declaration_and_inventory() {
    auto f = co_await makeFixture();

    const auto pluginPath = findPluginDir("example_feature");
    // 装载前: 注册表里只有宿主核心点
    XX_TEST_EXPECT_EQ(f.ctx->features->pointsCount(), size_t{2});
    XX_TEST_EXPECT_TRUE(f.ctx->features->find("plugin.example_feature.beat") == nullptr);

    auto inst = co_await f.mgr->loadPluginAsync(pluginPath);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }

    // 插件声明的点: 归属 = 本实例, 可被外部调用, 实现只有一个 (它自己)
    auto* beat = f.ctx->features->find("plugin.example_feature.beat");
    XX_TEST_EXPECT_TRUE(beat != nullptr);
    if (beat != nullptr) {
        XX_TEST_EXPECT_EQ(beat->origin(), std::string{"plugin:example_feature"});
        XX_TEST_EXPECT_TRUE(beat->callable());
        XX_TEST_EXPECT_EQ(beat->implCount(), size_t{1});
        XX_TEST_EXPECT_EQ(beat->effectiveBy(), std::string{"plugin:example_feature"});
    }
    XX_TEST_EXPECT_TRUE(f.ctx->features->find("plugin.example_feature.slow") != nullptr);
    XX_TEST_EXPECT_TRUE(f.ctx->features->find("plugin.example_feature.tooslow") != nullptr);
    XX_TEST_EXPECT_EQ(f.ctx->features->pointsCount(), size_t{5});

    // 宿主点上的插件实现: 插件层排在 core 之前, 因此它就是当前生效者
    auto* tokens = f.ctx->features->find(agentxx::feature::points::kContextCountTokens);
    XX_TEST_EXPECT_TRUE(tokens != nullptr);
    if (tokens != nullptr) {
        XX_TEST_EXPECT_EQ(tokens->implCount(), size_t{1});
        XX_TEST_EXPECT_TRUE(tokens->hasImplOf("plugin:example_feature"));
        XX_TEST_EXPECT_EQ(tokens->effectiveBy(), std::string{"plugin:example_feature"});
        // 清单里两个层都在, 且插件层在前 (清单顺序 = 实际询问顺序)
        const auto json = tokens->listJson();
        const auto impls = json["impls"];
        XX_TEST_EXPECT_EQ(impls.size(), size_t{2});
        if (impls.size() == 2) {
            XX_TEST_EXPECT_EQ(impls[0].value<std::string>("layer", ""), std::string{"plugin"});
            XX_TEST_EXPECT_EQ(impls[0].value<std::string>("owner", ""), std::string{"plugin:example_feature"});
            XX_TEST_EXPECT_EQ(impls[1].value<std::string>("layer", ""), std::string{"core"});
        }
    }

    // 实例视图与注册清单: 3 个点 + 4 个实现 (3 个自己的点 + 宿主点 1 个)
    {
        const auto inv = f.mgr->registrationInventory(*inst);
        XX_TEST_EXPECT_EQ(inv.featurePoints, size_t{3});
        XX_TEST_EXPECT_EQ(inv.featureImpls, size_t{4});
        XX_TEST_EXPECT_GE(inv.total(), size_t{7}); ///< 功能点计入统一注册清单

        auto views = f.mgr->list();
        XX_TEST_EXPECT_EQ(views.size(), size_t{1});
        if (views.size() == 1) {
            XX_TEST_EXPECT_EQ(views[0].featurePointCount, size_t{3});
            XX_TEST_EXPECT_EQ(views[0].featureImplCount, size_t{4});
            XX_TEST_EXPECT_EQ(views[0].registrationTotal, inv.total());
        }
    }

    // 清单 JSON (插件 `list_points` 与装配快照共用): 含插件点与归属
    {
        const auto listJson = f.mgr->listFeaturePoints();
        XX_TEST_EXPECT_TRUE(listJson.find("plugin.example_feature.beat") != std::string::npos);
        XX_TEST_EXPECT_TRUE(listJson.find("plugin.example_feature.tooslow") != std::string::npos);
        XX_TEST_EXPECT_TRUE(listJson.find("plugin:example_feature") != std::string::npos);
        XX_TEST_EXPECT_TRUE(listJson.find("agentxx.context.countTokens") != std::string::npos);
    }

    XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("example_feature", std::chrono::seconds{5}));
    co_return;
}

// ===========================================================================
// 2. 插件实现覆盖核心点; 摘除后回落
// ===========================================================================
asio::awaitable<void> test_plugin_impl_overrides_core_ask() {
    auto f = co_await makeFixture();

    auto* tokens = f.ctx->features->find(agentxx::feature::points::kContextCountTokens);
    XX_TEST_EXPECT_TRUE(tokens != nullptr);
    if (tokens == nullptr) {
        co_return;
    }
    auto* typed = dynamic_cast<
        agentxx::feature::ProvidePoint<agentxx::feature::CountTokensRequest, agentxx::feature::CountTokensValue>*>(
        tokens
    );
    XX_TEST_EXPECT_TRUE(typed != nullptr);
    if (typed == nullptr) {
        co_return;
    }

    // 装载前: 核心实现给值 (核心归属)
    {
        agentxx::feature::CountTokensRequest req;
        req.text    = "probe text before plugin load";
        auto before = co_await typed->ask(req);
        XX_TEST_EXPECT_TRUE(before.ok());
        XX_TEST_EXPECT_EQ(before.by, std::string{agentxx::feature::points::kOwnerCountTokens});
        XX_TEST_EXPECT_TRUE(before.value.has_value());
    }

    auto inst = co_await f.mgr->loadPluginAsync(findPluginDir("example_feature"));
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }

    // 装载后: 插件实现先被问到, 因此核心点由插件给值 (固定值, 便于断言)
    agentxx::feature::CountTokensRequest probe;
    probe.text = "probe text with plugin loaded";
    {
        auto withPlugin = co_await typed->ask(probe);
        XX_TEST_EXPECT_TRUE(withPlugin.ok());
        XX_TEST_EXPECT_TRUE(withPlugin.value.has_value());
        if (withPlugin.value.has_value()) {
            XX_TEST_EXPECT_EQ(withPlugin.value->tokens, int64_t{424242});
        }
        XX_TEST_EXPECT_EQ(withPlugin.by, std::string{"plugin:example_feature"});
        XX_TEST_EXPECT_FALSE(withPlugin.fromCache);
    }

    // 禁用: 实现被摘除, 它产出的值缓存条目与置空标记一并清掉 → 同身份回到核心实现
    f.mgr->disable("example_feature");
    XX_TEST_EXPECT_FALSE(tokens->hasImplOf("plugin:example_feature"));
    XX_TEST_EXPECT_EQ(tokens->effectiveBy(), std::string{agentxx::feature::points::kOwnerCountTokens});
    {
        auto afterDisable = co_await typed->ask(probe); // 同一身份: 验证缓存也失效了
        XX_TEST_EXPECT_TRUE(afterDisable.ok());
        XX_TEST_EXPECT_EQ(afterDisable.by, std::string{agentxx::feature::points::kOwnerCountTokens});
        XX_TEST_EXPECT_TRUE(afterDisable.value.has_value());
    }
    co_return;
}

} // namespace

// ===========================================================================
// 3. 按名调用 (call_point_async) 的各条结果路径
// ===========================================================================
namespace {

asio::awaitable<void> test_call_point_paths() {
    auto f = co_await makeFixture();

    // 两个插件: example_feature 声明点与实现, agentxx_math 作为"另一个调用方"
    // (调用方自己的实现不会被问到 —— 同归属的调用会走到 no_impl, 见本用例最后一条)
    auto provider = co_await f.mgr->loadPluginAsync(findPluginDir("example_feature"));
    auto caller   = co_await f.mgr->loadPluginAsync(findPluginDir("agentxx_math"));
    XX_TEST_EXPECT_TRUE(provider != nullptr);
    XX_TEST_EXPECT_TRUE(caller != nullptr);
    if (!provider || !caller) {
        co_return;
    }

    // (1) 同步实现: 插件自己的点, 参数与结果都过一遍
    {
        auto out = co_await callPoint(
            f,
            caller.get(),
            "plugin.example_feature.beat",
            R"({"path":"beats.wav","minBpm":90})"
        );
        XX_TEST_EXPECT_TRUE(out.accepted);
        XX_TEST_EXPECT_TRUE(out.finished);
        XX_TEST_EXPECT_EQ(out.status, PLUGINXX_OPERATOR_OK);
        XX_TEST_EXPECT_TRUE(out.ok);
        auto json = utilxx_base::Json::parse(out.payload);
        XX_TEST_EXPECT_EQ(json.value<std::string>("id", ""), std::string{"plugin.example_feature.beat"});
        XX_TEST_EXPECT_EQ(json.value<std::string>("by", ""), std::string{"plugin:example_feature"});
        XX_TEST_EXPECT_FALSE(json.value<bool>("fromCache", true));
        const auto value = json["value"];
        XX_TEST_EXPECT_EQ(value.value<std::string>("path", ""), std::string{"beats.wav"});
        XX_TEST_EXPECT_EQ(value.value<int>("bpm", 0), 128);
        XX_TEST_EXPECT_TRUE(value.value<bool>("confident", false));
    }

    // (2) 协程实现: 插件实现里等待 60ms 后给值 (异步实现经操作协议驱动)
    {
        auto out = co_await callPoint(f, caller.get(), "plugin.example_feature.slow", R"({"input":7})");
        XX_TEST_EXPECT_TRUE(out.accepted);
        XX_TEST_EXPECT_TRUE(out.finished);
        XX_TEST_EXPECT_TRUE(out.ok);
        auto json = utilxx_base::Json::parse(out.payload);
        XX_TEST_EXPECT_EQ(json["value"].value<int64_t>("input", 0), int64_t{7});
        XX_TEST_EXPECT_GE(json["value"].value<int64_t>("counter", 0), int64_t{1});
        // 真的等过 (实现里的 60ms 等待): 结果里带回耗时
        XX_TEST_EXPECT_GE(json.value<int64_t>("ms", 0), int64_t{50});
        XX_TEST_EXPECT_EQ(json.value<std::string>("by", ""), std::string{"plugin:example_feature"});
    }

    // (3) 实现超时: 点声明等实现最多 50ms, 实现却等 400ms
    //     到点取消当次实现并按"没意见"继续 → 链上没人给值 = no_impl (不会一直等下去)
    {
        const auto begin = std::chrono::steady_clock::now();
        auto       out   = co_await callPoint(f, caller.get(), "plugin.example_feature.tooslow", "{}");
        const auto cost  = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - begin
                          )
                              .count();
        XX_TEST_EXPECT_TRUE(out.accepted);
        XX_TEST_EXPECT_TRUE(out.finished);
        XX_TEST_EXPECT_FALSE(out.ok);
        XX_TEST_EXPECT_EQ(out.error, std::string{"no_impl"});
        XX_TEST_EXPECT_TRUE(cost < 400); ///< 到点就返回 (没有等实现跑完)
    }

    // (4) 同归属调用: 调用方自己的实现不会被问到 (无用往返), 因此没人给值
    {
        auto out = co_await callPoint(f, provider.get(), "plugin.example_feature.beat", R"({"path":"x"})");
        XX_TEST_EXPECT_TRUE(out.accepted);
        XX_TEST_EXPECT_TRUE(out.finished);
        XX_TEST_EXPECT_FALSE(out.ok);
        XX_TEST_EXPECT_EQ(out.error, std::string{"no_impl"});
    }

    // (5) 强类型核心点: 不接受按名调用 (受理即被拒, 不会进链)
    {
        auto out = co_await callPoint(f, caller.get(), "agentxx.context.countTokens", R"({"text":"x"})");
        XX_TEST_EXPECT_FALSE(out.accepted);
        XX_TEST_EXPECT_TRUE(out.acceptError.find("不接受按名调用") != std::string::npos);
    }

    // (6) 点不存在: 受理即被拒
    {
        auto out = co_await callPoint(f, caller.get(), "plugin.example_feature.nope", "{}");
        XX_TEST_EXPECT_FALSE(out.accepted);
        XX_TEST_EXPECT_TRUE(out.acceptError.find("not declared") != std::string::npos);
    }

    // (6b) 参数不是合法 JSON: 受理成功, 结果是 bad_args (实现不会被问到)
    {
        auto out = co_await callPoint(f, caller.get(), "plugin.example_feature.beat", "not-json");
        XX_TEST_EXPECT_TRUE(out.accepted);
        XX_TEST_EXPECT_TRUE(out.finished);
        XX_TEST_EXPECT_FALSE(out.ok);
        XX_TEST_EXPECT_EQ(out.error, std::string{"bad_args"});
    }

    // (7) 点存在但没人给值: 受理成功, 结果是 no_impl
    {
        declareHostPoint(f, "host.test.no_impl");
        auto out = co_await callPoint(f, caller.get(), "host.test.no_impl", "{}");
        XX_TEST_EXPECT_TRUE(out.accepted);
        XX_TEST_EXPECT_TRUE(out.finished);
        XX_TEST_EXPECT_FALSE(out.ok);
        XX_TEST_EXPECT_EQ(out.error, std::string{"no_impl"});
    }

    // (8) 实现显式置空: 结果是 disabled, 并带上实现给的原因
    {
        auto& point = declareHostPoint(f, "host.test.disable");
        XX_TEST_EXPECT_EQ(
            addSyncPluginImpl(point, "plugin:test_disable", R"({"disable":true,"reason":"测试置空"})"),
            0
        );
        auto out = co_await callPoint(f, caller.get(), "host.test.disable", "{}");
        XX_TEST_EXPECT_TRUE(out.accepted);
        XX_TEST_EXPECT_FALSE(out.ok);
        XX_TEST_EXPECT_EQ(out.error, std::string{"disabled"});
        XX_TEST_EXPECT_TRUE(out.payload.find("测试置空") != std::string::npos);
        // `call` 的零副作用契约: 不记置空标记 (应用自己 `ask` 才记)
        XX_TEST_EXPECT_EQ(point.disabledBy(), std::string{});
        auto asked = co_await point.ask("{}");
        XX_TEST_EXPECT_FALSE(asked.ok());
        XX_TEST_EXPECT_EQ(point.disabledBy(), std::string{"plugin:test_disable"});
    }

    XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("agentxx_math", std::chrono::seconds{5}));
    XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("example_feature", std::chrono::seconds{5}));
    co_return;
}

/// 并发重入保护: 同一 (点, 调用方) 上一次没结束 → busy
asio::awaitable<void> test_call_busy_protection() {
    auto f = co_await makeFixture();

    auto inst = co_await f.mgr->loadPluginAsync(findPluginDir("example_feature"));
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }

    // 一个"等外部放开才给值"的插件层实现: 第一次调用会一直挂在闸门上
    auto& point = declareHostPoint(f, "host.test.gate");
    auto  ex    = co_await asio::this_coro::executor;
    struct Gate {
        std::shared_ptr<asio::steady_timer> timer;
        bool                                entered = false;
    };
    auto gate = std::make_shared<Gate>();
    gate->timer = std::make_shared<asio::steady_timer>(
        ex,
        (std::chrono::steady_clock::time_point::max)()
    );

    agentxx::feature::ImplSpec spec;
    spec.layer = agentxx::feature::ImplLayer::Plugin;
    spec.owner = "plugin:test_gate";
    spec.load  = "dynamic";
    spec.fn    = [gate](std::string) -> asio::awaitable<std::optional<std::string>> {
        gate->entered = true;
        utilxx_base::AsioErrorCode ec;
        co_await gate->timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
        co_return std::optional<std::string>{std::string{R"({"value":{"gate":"open"}})"}};
    };
    XX_TEST_EXPECT_EQ(point.addImpl(std::move(spec)), 0);

    // 第一次: 受理成功, 挂在闸门上
    CallProbe      first;
    CallProbe      second;
    PluginxxString err1{nullptr, 0};
    PluginxxString err2{nullptr, 0};
    auto           pointSv = agentxx::plugin::PluginStringView::from("host.test.gate");
    auto           argsSv  = agentxx::plugin::PluginStringView::from("{}");
    auto*          h1      = f.mgr->callFeatureAsync(inst.get(), pointSv, argsSv, &onFeatureCall, &first, &err1);
    XX_TEST_EXPECT_TRUE(h1 != nullptr);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&gate] {
        return gate->entered;
    }));

    // 同一调用方的第二次调用: 被拒为 busy (不排队, 也不并发跑第二份实现)
    auto* h2 = f.mgr->callFeatureAsync(inst.get(), pointSv, argsSv, &onFeatureCall, &second, &err2);
    XX_TEST_EXPECT_TRUE(h2 != nullptr);
    XX_TEST_EXPECT_TRUE(co_await waitFor([&second] {
        return second.done;
    }));
    XX_TEST_EXPECT_FALSE(second.done ? second.payload.find(R"("ok":true)") != std::string::npos : true);
    XX_TEST_EXPECT_TRUE(second.payload.find("busy") != std::string::npos);
    XX_TEST_EXPECT_FALSE(first.done); ///< 第一次仍在闸门上

    // 放开闸门: 第一次正常完成
    gate->timer->cancel();
    XX_TEST_EXPECT_TRUE(co_await waitFor([&first] {
        return first.done;
    }));
    XX_TEST_EXPECT_EQ(first.status, PLUGINXX_OPERATOR_OK);
    XX_TEST_EXPECT_TRUE(first.payload.find(R"("gate":"open")") != std::string::npos);
    co_return;
}

// ===========================================================================
// 4. 生命周期: 禁用 / 启用 / 卸载
// ===========================================================================
asio::awaitable<void> test_lifecycle_detach_and_restore() {
    auto f = co_await makeFixture();

    auto inst = co_await f.mgr->loadPluginAsync(findPluginDir("example_feature"));
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }
    auto* tokens = f.ctx->features->find(agentxx::feature::points::kContextCountTokens);
    XX_TEST_EXPECT_TRUE(tokens != nullptr);
    if (tokens == nullptr) {
        co_return;
    }
    XX_TEST_EXPECT_TRUE(f.ctx->features->find("plugin.example_feature.beat") != nullptr);

    // ---- 禁用: 生效注册全部摘除 (点消失、实现摘掉) ----
    f.mgr->disable("example_feature");
    XX_TEST_EXPECT_TRUE(f.ctx->features->find("plugin.example_feature.beat") == nullptr);
    XX_TEST_EXPECT_TRUE(f.ctx->features->find("plugin.example_feature.tooslow") == nullptr);
    XX_TEST_EXPECT_FALSE(tokens->hasImplOf("plugin:example_feature"));
    {
        const auto inv = f.mgr->registrationInventory(*inst);
        XX_TEST_EXPECT_EQ(inv.featurePoints, size_t{0});
        XX_TEST_EXPECT_EQ(inv.featureImpls, size_t{0});
        auto views = f.mgr->list();
        if (views.size() == 1) {
            XX_TEST_EXPECT_EQ(views[0].featurePointCount, size_t{0});
            XX_TEST_EXPECT_EQ(views[0].featureImplCount, size_t{0});
        }
    }

    // ---- 启用: start 事务重新声明 (投递到 io 线程, 轮询等待) ----
    f.mgr->enable("example_feature");
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return f.ctx->features->find("plugin.example_feature.beat") != nullptr;
    }));
    XX_TEST_EXPECT_TRUE(tokens->hasImplOf("plugin:example_feature"));
    {
        const auto inv = f.mgr->registrationInventory(*inst);
        XX_TEST_EXPECT_EQ(inv.featurePoints, size_t{3});
        XX_TEST_EXPECT_EQ(inv.featureImpls, size_t{4});
    }

    // ---- 卸载: 实例消失, 注册表回到基线 (只剩宿主核心点) ----
    XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("example_feature", std::chrono::seconds{5}));
    XX_TEST_EXPECT_TRUE(f.mgr->find("example_feature") == nullptr);
    XX_TEST_EXPECT_TRUE(f.ctx->features->find("plugin.example_feature.beat") == nullptr);
    XX_TEST_EXPECT_TRUE(f.ctx->features->find("plugin.example_feature.slow") == nullptr);
    XX_TEST_EXPECT_TRUE(f.ctx->features->find("plugin.example_feature.tooslow") == nullptr);
    XX_TEST_EXPECT_FALSE(tokens->hasImplOf("plugin:example_feature"));
    // `implCount` 数的是插件层实现; 插件实现摘掉后为 0, 生效者回到核心实现
    XX_TEST_EXPECT_EQ(tokens->implCount(), size_t{0});
    XX_TEST_EXPECT_EQ(tokens->effectiveBy(), std::string{agentxx::feature::points::kOwnerCountTokens});
    XX_TEST_EXPECT_EQ(f.ctx->features->pointsCount(), size_t{2});
    co_return;
}

} // namespace

asio::awaitable<TestResult> run_plugin_feature_tests() {
    const int passedBefore = g_pf_passed;
    const int failedBefore = g_pf_failed;

    try {
        co_await test_declaration_and_inventory();
        co_await test_plugin_impl_overrides_core_ask();
        co_await test_call_point_paths();
        co_await test_call_busy_protection();
        co_await test_lifecycle_detach_and_restore();
    } catch (const std::exception& e) {
        TEST_FAIL << "plugin_feature suite exception: " << e.what() << std::endl;
        g_pf_failed++;
    }

    co_return TestResult{g_pf_passed - passedBefore, g_pf_failed - failedBefore};
}

} // namespace test
} // namespace agentxx
