/// 功能点子系统测试 (阶段 1: 点 / 实现 / 调用三层骨架)
///
/// 覆盖点:
/// - 顺序与默认优先级带 (插件 0 / FFI 宿主 1000 / core 层), 同优先级按登记顺序;
///   `priority` 越界被裁剪到上下限 (用清单读回生效值断言);
/// - 不设注册数量上限 (一个点多个实现、一个实现登记多个点);
/// - 异常隔离 (实现抛异常 / 返回不合法 JSON 都只等于"没意见");
/// - 显式置空 (`ask` 记标记、`call` 不记、实现摘除后自动还原);
/// - 值缓存 (ByIdentity 命中 / 换身份不命中 / refresh 跳过 / 字节上限 / None 不产生条目 /
///   `call` 前后条目与产出方不变 / 同一身份并发只跑一次实现 / 按来源失效);
/// - 调用保护 (载荷带 caller 与 viaCall / 调用方自己的实现不问 / 重入 → busy);
/// - 两处超时 (implTimeoutMs 到点取消实现并继续链; 调用方 timeoutMs 到点返回 failed);
/// - 清单字段与开发者模式门控 (`stat` 段只在开发者模式下出现)。
#include "agentxx-test/core/test_feature_points.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/agent/context.h"
#include "agentxx/feature/points.h"
#include "agentxx/feature/registry.h"
#include "agentxx/middlewares/summarization.h"
#include "asio/this_coro.hpp"
#include "utilxx/async_offload.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <string>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_fp_passed = 0;
int g_fp_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_fp_passed
#define XX_TEST_FAILED g_fp_failed

namespace agentxx {
namespace test {

/// 测试用的请求 / 值类型 (与真实核心点同一套写法: 强类型 + JSON 编解码)
struct PingRequest {
    std::string key;
};

struct PingValue {
    int value = 0;
};

} // namespace test
} // namespace agentxx

namespace agentxx {
namespace feature {

template<>
struct Codec<agentxx::test::PingRequest> {
    static std::string toJson(const agentxx::test::PingRequest& req) {
        return fmt::format(R"({{"key":"{}"}})", req.key);
    }

    static std::optional<agentxx::test::PingRequest> fromJson(std::string_view text) {
        return utilxx_base::catchError<std::optional<agentxx::test::PingRequest>>(
            [&]() -> std::optional<agentxx::test::PingRequest> {
                const auto json = utilxx_base::Json::parse(text);
                agentxx::test::PingRequest req;
                req.key = json.value<std::string>("key", "");
                return req;
            },
            [](std::string) -> std::optional<agentxx::test::PingRequest> {
                return std::nullopt;
            }
        );
    }
};

template<>
struct Codec<agentxx::test::PingValue> {
    /// 值就是"点的结果形状"本身 (这里刻意用一个裸数字, 与真实点的对象形状区分开;
    /// 实现侧的回答形状是 `{"value": <值>}`, 见 ImplSpec::Fn 说明)
    static std::string toJson(const agentxx::test::PingValue& value) {
        return fmt::format("{}", value.value);
    }

    static std::optional<agentxx::test::PingValue> fromJson(std::string_view text) {
        return utilxx_base::catchError<std::optional<agentxx::test::PingValue>>(
            [&]() -> std::optional<agentxx::test::PingValue> {
                const auto json = utilxx_base::Json::parse(text);
                if (!json.is_number()) {
                    return std::nullopt;
                }
                agentxx::test::PingValue value;
                value.value = json.get<int>();
                return value;
            },
            [](std::string) -> std::optional<agentxx::test::PingValue> {
                return std::nullopt;
            }
        );
    }
};

} // namespace feature
} // namespace agentxx

namespace agentxx {
namespace test {

using agentxx::feature::AskResult;
using agentxx::feature::CallError;
using agentxx::feature::CallOptions;
using agentxx::feature::CallResult;
using agentxx::feature::ImplLayer;
using agentxx::feature::ImplSpec;
using agentxx::feature::PointOptions;
using agentxx::feature::ProvidePoint;
using agentxx::feature::Registry;

namespace {

/// 等待若干毫秒 (被取消时立即返回; 用来模拟"实现要算一会儿")
asio::awaitable<void> sleepMs(int ms) {
    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer timer(ex, std::chrono::milliseconds{ms});
    boost::system::error_code ec;
    co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    co_return;
}

/// 造一个登记规格 (JSON 实现): 回答固定 JSON 文本
ImplSpec jsonImpl(
    std::string                             owner,
    int32_t                                 priority,
    std::function<std::string(std::string)> answerFn,
    ImplLayer                               layer = ImplLayer::Plugin
) {
    ImplSpec spec;
    spec.layer    = layer;
    spec.owner    = std::move(owner);
    spec.priority = priority;
    spec.fn       = [answerFn = std::move(answerFn)](std::string callJson)
        -> asio::awaitable<std::optional<std::string>> {
        co_return answerFn(std::move(callJson));
    };
    return spec;
}

/// 造一个"返回错误形态"的实现 (用来验证异常隔离)
ImplSpec
    plainImpl(std::string owner, int32_t priority, ImplSpec::Fn fn, ImplLayer layer = ImplLayer::Plugin) {
    ImplSpec spec;
    spec.layer    = layer;
    spec.owner    = std::move(owner);
    spec.priority = priority;
    spec.fn       = std::move(fn);
    return spec;
}

PointOptions pingOptions(std::string title, agentxx::feature::CacheMode cache, bool callable = false) {
    PointOptions opts;
    opts.title    = std::move(title);
    opts.cache    = cache;
    opts.callable = callable;
    return opts;
}

ProvidePoint<PingRequest, PingValue>&
    makePingPoint(Registry& registry, std::string id, PointOptions opts) {
    return registry.provide<PingRequest, PingValue>(
        id,
        std::move(opts),
        [](const PingRequest& req) {
            return req.key;
        }
    );
}

// ==================== 顺序与默认优先级带 ====================

asio::awaitable<void> test_order_and_priority_band() {
    Registry registry;
    auto&    point = makePingPoint(
        registry,
        "test.order",
        pingOptions("顺序测试", agentxx::feature::CacheMode::None)
    );
    // 核心兜底实现 (层 core)
    point.addCoreImpl(
        "core:test:fallback",
        0,
        [](const PingRequest&, const agentxx::feature::ImplContext&)
            -> asio::awaitable<std::optional<PingValue>> {
            co_return PingValue{1};
        }
    );

    // 插件层: priority 0 (登记顺序决定同优先级先后)
    registry.addImpl(
        "test.order",
        jsonImpl("plugin:a", 0, [](std::string) {
            return std::string{R"({"value":2})"};
        })
    );
    registry.addImpl(
        "test.order",
        jsonImpl("plugin:b", 0, [](std::string) {
            return std::string{R"({"value":3})"};
        })
    );

    auto r1 = co_await point.ask(PingRequest{"k1"});
    XX_TEST_EXPECT_TRUE(r1.ok());
    XX_TEST_EXPECT_EQ((r1.ok() ? r1.value->value : -1), 2);
    XX_TEST_EXPECT_EQ(r1.by, std::string{"plugin:a"});

    // FFI 宿主实现默认优先级 1000: 排在插件之后、core 之前
    registry.addImpl(
        "test.order",
        jsonImpl("host:h1", agentxx::feature::kHostDefaultPriority, [](std::string) {
            return std::string{R"({"value":4})"};
        })
    );
    auto r2 = co_await point.ask(PingRequest{"k2"});
    XX_TEST_EXPECT_EQ(r2.by, std::string{"plugin:a"}); // 插件仍在前

    // 插件 a 摘除 -> 插件 b 生效
    XX_TEST_EXPECT_EQ(registry.removeImpl("test.order", "plugin:a"), size_t{1});
    auto r3 = co_await point.ask(PingRequest{"k3"});
    XX_TEST_EXPECT_EQ(r3.by, std::string{"plugin:b"});

    // 显式优先级覆盖默认带: 插件 b 改 priority 1001 (排在宿主 1000 之后)
    registry.addImpl(
        "test.order",
        jsonImpl("plugin:b", 1001, [](std::string) {
            return std::string{R"({"value":30})"};
        })
    );
    auto r4 = co_await point.ask(PingRequest{"k4"});
    XX_TEST_EXPECT_EQ(r4.by, std::string{"host:h1"});
    XX_TEST_EXPECT_EQ((r4.ok() ? r4.value->value : -1), 4);

    // 越界裁剪: priority 5000 -> 裁剪到上限 1000 (清单里读回生效值)
    registry.addImpl(
        "test.order",
        jsonImpl("plugin:c", 999999, [](std::string) {
            return std::string{R"({"value":5})"};
        })
    );
    const auto list    = registry.listPointsJson();
    const auto& points = list["points"];
    bool        found  = false;
    for (size_t i = 0; i < points.size(); ++i) {
        if (points[i].value<std::string>("id", "") != std::string{"test.order"}) {
            continue;
        }
        for (size_t k = 0; k < points[i]["impls"].size(); ++k) {
            const auto& impl = points[i]["impls"][k];
            if (impl.value<std::string>("owner", "") == "plugin:c") {
                XX_TEST_EXPECT_EQ(impl.value<int>("priority", 0), 100000);
                found = true;
            }
        }
    }
    XX_TEST_EXPECT_TRUE(found);

    // 全部实现摘除 -> 回到 core 兜底值
    registry.removeImplsByOwner("plugin:b");
    registry.removeImplsByOwner("plugin:c");
    registry.removeImplsByOwner("host:h1");
    auto r5 = co_await point.ask(PingRequest{"k5"});
    XX_TEST_EXPECT_EQ((r5.ok() ? r5.value->value : -1), 1);
    XX_TEST_EXPECT_EQ(r5.by, std::string{"core:test:fallback"});
    co_return;
}

/// 一个点可以登记多个实现, 一个实现可以登记到多个点 (不设数量上限)
asio::awaitable<void> test_no_registration_limit() {
    Registry registry;
    auto&    point = makePingPoint(
        registry,
        "test.many",
        pingOptions("数量测试", agentxx::feature::CacheMode::None)
    );
    for (int i = 0; i < 64; ++i) {
        registry.addImpl(
            "test.many",
            jsonImpl(fmt::format("plugin:p{}", i), i, [](std::string) {
                return std::string{R"({"value":7})"};
            })
        );
    }
    XX_TEST_EXPECT_EQ(point.implCount(), size_t{64});

    // 一个 owner 为多个点提供实现
    auto& second = makePingPoint(
        registry,
        "test.many2",
        pingOptions("数量测试2", agentxx::feature::CacheMode::None)
    );
    XX_TEST_EXPECT_EQ(registry.addImpl("test.many2", jsonImpl("plugin:p0", 0, [](std::string) {
                          return std::string{R"({"value":9})"};
                      })),
                      0);
    XX_TEST_EXPECT_EQ(second.implCount(), size_t{1});
    XX_TEST_EXPECT_EQ(registry.removeImplsByOwner("plugin:p0"), size_t{2});

    // 点不存在时登记被拒绝
    XX_TEST_EXPECT_EQ(registry.addImpl("test.not.exists", jsonImpl("plugin:x", 0, [](std::string) {
                          return std::string{};
                      })),
                      -1);
    co_return;
}

// ==================== 异常隔离与没意见 ====================

asio::awaitable<void> test_exception_isolation() {
    Registry registry;
    auto&    point = makePingPoint(
        registry,
        "test.ex",
        pingOptions("异常隔离", agentxx::feature::CacheMode::None)
    );

    // 抛异常的实现
    registry.addImpl(
        "test.ex",
        plainImpl("plugin:thrower", 0, [](std::string) -> asio::awaitable<std::optional<std::string>> {
            throw std::runtime_error("boom");
        })
    );
    // 返回不合法 JSON 的实现
    registry.addImpl(
        "test.ex",
        jsonImpl("plugin:badjson", 10, [](std::string) {
            return std::string{"not a json"};
        })
    );
    // 返回"空对象"= 没意见
    registry.addImpl(
        "test.ex",
        jsonImpl("plugin:silent", 20, [](std::string) {
            return std::string{"{}"};
        })
    );
    // 正常实现
    registry.addImpl(
        "test.ex",
        jsonImpl("plugin:good", 30, [](std::string) {
            return std::string{R"({"value":42})"};
        })
    );

    auto r = co_await point.ask(PingRequest{"k"});
    XX_TEST_EXPECT_TRUE(r.ok());
    XX_TEST_EXPECT_EQ((r.ok() ? r.value->value : -1), 42);
    XX_TEST_EXPECT_EQ(r.by, std::string{"plugin:good"});

    // 链条全无 -> no_impl
    Registry registry2;
    auto&    empty = makePingPoint(
        registry2,
        "test.empty",
        pingOptions("空链", agentxx::feature::CacheMode::None)
    );
    auto r2 = co_await empty.ask(PingRequest{"k"});
    XX_TEST_EXPECT_EQ(static_cast<int>(r2.error), static_cast<int>(CallError::NoImpl));
    XX_TEST_EXPECT_TRUE(!r2.message.empty());
    co_return;
}

// ==================== 显式置空 ====================

asio::awaitable<void> test_disable_marker() {
    Registry registry;
    auto&    point = makePingPoint(
        registry,
        "test.disable",
        pingOptions("置空", agentxx::feature::CacheMode::None, true)
    );
    point.addCoreImpl(
        "core:test:fallback",
        0,
        [](const PingRequest&, const agentxx::feature::ImplContext&)
            -> asio::awaitable<std::optional<PingValue>> {
            co_return PingValue{1};
        }
    );
    registry.addImpl(
        "test.disable",
        jsonImpl("plugin:off", 0, [](std::string) {
            return std::string{R"({"disable":true,"reason":"维护中"})"};
        })
    );

    // ask: 记置空标记, 且不再问后面的实现 (拿不到 core 兜底值)
    auto r = co_await point.ask(PingRequest{"k"});
    XX_TEST_EXPECT_EQ(static_cast<int>(r.error), static_cast<int>(CallError::Disabled));
    XX_TEST_EXPECT_EQ(point.disabledBy(), std::string{"plugin:off"});
    XX_TEST_EXPECT_EQ(r.message, std::string{"维护中"});
    XX_TEST_EXPECT_FALSE(r.value.has_value());

    // call: 只影响本次, 不记标记 (先清掉标记再验证)
    point.clearDisabled();
    auto rc = co_await point.call(PingRequest{"k"}, CallOptions{.caller = "plugin:other"});
    XX_TEST_EXPECT_EQ(static_cast<int>(rc.error), static_cast<int>(CallError::Disabled));
    XX_TEST_EXPECT_EQ(point.disabledBy(), std::string{""});

    // 实现摘除后标记自动还原
    auto rd = co_await point.ask(PingRequest{"k"});
    XX_TEST_EXPECT_EQ(static_cast<int>(rd.error), static_cast<int>(CallError::Disabled));
    XX_TEST_EXPECT_EQ(point.disabledBy(), std::string{"plugin:off"});
    registry.removeImpl("test.disable", "plugin:off");
    XX_TEST_EXPECT_EQ(point.disabledBy(), std::string{""});
    auto r2 = co_await point.ask(PingRequest{"k"});
    XX_TEST_EXPECT_EQ((r2.ok() ? r2.value->value : -1), 1);
    co_return;
}

// ==================== 值缓存 ====================

asio::awaitable<void> test_value_cache() {
    Registry registry;
    PointOptions opts;
    opts.title    = "值缓存";
    opts.cache    = agentxx::feature::CacheMode::ByIdentity;
    opts.maxItems = 2;
    opts.maxBytes = 4096;
    opts.callable = true;
    auto& point   = makePingPoint(registry, "test.cache", std::move(opts));

    auto calls = std::make_shared<std::atomic<int>>(0);
    registry.addImpl(
        "test.cache",
        plainImpl("plugin:c", 0, [calls](std::string) -> asio::awaitable<std::optional<std::string>> {
            calls->fetch_add(1);
            co_return std::string{R"({"value":10})"};
        })
    );

    auto r1 = co_await point.ask(PingRequest{"A"});
    XX_TEST_EXPECT_FALSE(r1.fromCache);
    XX_TEST_EXPECT_EQ(calls->load(), 1);
    auto r2 = co_await point.ask(PingRequest{"A"});
    XX_TEST_EXPECT_TRUE(r2.fromCache);
    XX_TEST_EXPECT_EQ(calls->load(), 1);
    XX_TEST_EXPECT_EQ((r2.ok() ? r2.value->value : -1), 10);
    XX_TEST_EXPECT_EQ(r2.by, std::string{"plugin:c"});

    // 换身份不命中
    auto r3 = co_await point.ask(PingRequest{"B"});
    XX_TEST_EXPECT_FALSE(r3.fromCache);
    XX_TEST_EXPECT_EQ(calls->load(), 2);
    XX_TEST_EXPECT_EQ(point.cacheSize(), size_t{2});

    // 条数上限: 第 3 个身份写入后仍只有 2 条 (按最近最少使用淘汰)
    (void)co_await point.ask(PingRequest{"C"});
    XX_TEST_EXPECT_EQ(point.cacheSize(), size_t{2});

    // refresh 跳过缓存重算
    auto r4 = co_await point.ask(PingRequest{"A"}, agentxx::feature::AskOptions{.refresh = true});
    XX_TEST_EXPECT_FALSE(r4.fromCache);
    XX_TEST_EXPECT_EQ(calls->load(), 4);

    // call 前后值缓存条数与产出方不变 (核心用例)
    const size_t before    = point.cacheSize();
    auto         rc        = co_await point.call(PingRequest{"D"}, CallOptions{.caller = "plugin:x"});
    XX_TEST_EXPECT_TRUE(rc.ok());
    XX_TEST_EXPECT_EQ(point.cacheSize(), before);

    // 单条超字节上限不入缓存 (用通用 JSON 点: 值本身就是要写入缓存的那段文本)
    Registry     small;
    PointOptions smallOpts;
    smallOpts.title    = "小上限";
    smallOpts.cache    = agentxx::feature::CacheMode::ByIdentity;
    smallOpts.maxBytes = 32;
    auto& smallPoint   = small.provideJson(
        "test.cache.small",
        std::move(smallOpts),
        "host:test"
    );
    small.addImpl(
        "test.cache.small",
        jsonImpl("plugin:s", 0, [](std::string) {
            return std::string{R"({"value":")"} + std::string(200, 'x') + R"("})";
        })
    );
    auto rs = co_await smallPoint.ask("{}");
    XX_TEST_EXPECT_TRUE(rs.ok());
    XX_TEST_EXPECT_EQ(smallPoint.cacheSize(), size_t{0});

    // None 策略不产生任何条目
    Registry     none;
    auto&        nonePoint = makePingPoint(
        none,
        "test.cache.none",
        pingOptions("不缓存", agentxx::feature::CacheMode::None)
    );
    none.addImpl("test.cache.none", jsonImpl("plugin:n", 0, [](std::string) {
        return std::string{R"({"value":1})"};
    }));
    (void)co_await nonePoint.ask(PingRequest{"A"});
    (void)co_await nonePoint.ask(PingRequest{"A"});
    XX_TEST_EXPECT_EQ(nonePoint.cacheSize(), size_t{0});

    // 按来源失效: 产出方摘除后它的条目被删除
    auto r5 = co_await point.ask(PingRequest{"E"});
    XX_TEST_EXPECT_TRUE(r5.ok());
    XX_TEST_EXPECT_GE(point.cacheSize(), size_t{1});
    registry.removeImpl("test.cache", "plugin:c");
    XX_TEST_EXPECT_EQ(point.cacheSize(), size_t{0});
    co_return;
}

/// 同一身份并发只跑一次实现 (in-flight 去重)
asio::awaitable<void> test_inflight_dedup() {
    Registry registry;
    PointOptions opts;
    opts.title = "去重";
    opts.cache = agentxx::feature::CacheMode::ByIdentity;
    auto& point = makePingPoint(registry, "test.inflight", std::move(opts));

    auto calls = std::make_shared<std::atomic<int>>(0);
    registry.addImpl(
        "test.inflight",
        plainImpl("plugin:slow", 0, [calls](std::string) -> asio::awaitable<std::optional<std::string>> {
            calls->fetch_add(1);
            co_await sleepMs(60);
            co_return std::string{R"({"value":77})"};
        })
    );

    auto ex = co_await asio::this_coro::executor;
    auto group = co_await asio::experimental::make_parallel_group(
                     asio::co_spawn(
                         ex,
                         [&point]() -> asio::awaitable<AskResult<PingValue>> {
                             co_return co_await point.ask(PingRequest{"same"});
                         },
                         asio::deferred
                     ),
                     asio::co_spawn(
                         ex,
                         [&point]() -> asio::awaitable<AskResult<PingValue>> {
                             co_return co_await point.ask(PingRequest{"same"});
                         },
                         asio::deferred
                     )
                 )
                     .async_wait(asio::experimental::wait_for_all(), asio::deferred);

    const auto& ex1 = std::get<1>(group);
    const auto& r1  = std::get<2>(group);
    const auto& ex2 = std::get<3>(group);
    const auto& r2  = std::get<4>(group);
    XX_TEST_EXPECT_FALSE(ex1 != nullptr);
    XX_TEST_EXPECT_FALSE(ex2 != nullptr);
    XX_TEST_EXPECT_TRUE(r1.ok());
    XX_TEST_EXPECT_TRUE(r2.ok());
    XX_TEST_EXPECT_EQ((r1.ok() ? r1.value->value : -1), 77);
    XX_TEST_EXPECT_EQ((r2.ok() ? r2.value->value : -1), 77);
    // 两次并发只跑了一次实现
    XX_TEST_EXPECT_EQ(calls->load(), 1);
    co_return;
}

// ==================== 调用保护 ====================

asio::awaitable<void> test_call_protections() {
    Registry registry;
    auto&    point = makePingPoint(
        registry,
        "test.call",
        pingOptions("调用保护", agentxx::feature::CacheMode::None, true)
    );

    auto seenJson = std::make_shared<std::string>();
    registry.addImpl(
        "test.call",
        plainImpl("plugin:inner", 0, [seenJson](std::string callJson)
                      -> asio::awaitable<std::optional<std::string>> {
            *seenJson = callJson;
            co_return std::string{R"({"value":5})"};
        })
    );
    // 一个优先级更高的宿主实现 (排在 plugin:inner 之后, 因为默认带 1000)
    registry.addImpl(
        "test.call",
        jsonImpl("host:outer", agentxx::feature::kHostDefaultPriority, [](std::string) {
            return std::string{R"({"value":6})"};
        })
    );

    auto rc = co_await point.call(PingRequest{"K"}, CallOptions{.caller = "plugin:outer"});
    XX_TEST_EXPECT_TRUE(rc.ok());
    XX_TEST_EXPECT_EQ((rc.ok() ? rc.value->value : -1), 5);
    // 载荷带 caller 与 viaCall
    const auto callJson = utilxx_base::Json::parse(*seenJson);
    XX_TEST_EXPECT_EQ(callJson.value<std::string>("caller", ""), std::string{"plugin:outer"});
    XX_TEST_EXPECT_TRUE(callJson.value<bool>("viaCall", false));
    XX_TEST_EXPECT_EQ(callJson.value<std::string>("point", ""), std::string{"test.call"});
    XX_TEST_EXPECT_EQ(callJson["args"].value<std::string>("key", ""), std::string{"K"});

    // 调用方自己的实现不问回自己: 只有 plugin:outer 一个插件实现时跳过插件层 -> 拿宿主值
    Registry registry2;
    auto&    point2 = makePingPoint(
        registry2,
        "test.call.self",
        pingOptions("自问", agentxx::feature::CacheMode::None, true)
    );
    auto selfCalls = std::make_shared<std::atomic<int>>(0);
    registry2.addImpl(
        "test.call.self",
        plainImpl("plugin:outer", 0, [selfCalls](std::string)
                      -> asio::awaitable<std::optional<std::string>> {
            selfCalls->fetch_add(1);
            co_return std::string{R"({"value":1})"};
        })
    );
    registry2.addImpl(
        "test.call.self",
        jsonImpl("host:h", agentxx::feature::kHostDefaultPriority, [](std::string) {
            return std::string{R"({"value":8})"};
        })
    );
    auto rc2 = co_await point2.call(PingRequest{"K"}, CallOptions{.caller = "plugin:outer"});
    XX_TEST_EXPECT_EQ((rc2.ok() ? rc2.value->value : -1), 8);
    XX_TEST_EXPECT_EQ(selfCalls->load(), 0);

    // 重入保护: 同一 (点, 调用方) 上一次没结束又来一次 -> busy
    Registry registry3;
    auto&    point3 = makePingPoint(
        registry3,
        "test.call.busy",
        pingOptions("重入", agentxx::feature::CacheMode::None, true)
    );
    auto innerError = std::make_shared<int>(0);
    registry3.addImpl(
        "test.call.busy",
        plainImpl("plugin:inner", 0, [&point3, innerError](std::string)
                      -> asio::awaitable<std::optional<std::string>> {
            auto inner = co_await point3.call(
                PingRequest{"K"},
                CallOptions{.caller = "plugin:outer"}
            );
            *innerError = static_cast<int>(inner.error);
            co_return std::string{R"({"value":11})"};
        })
    );
    registry3.addImpl(
        "test.call.busy",
        jsonImpl("plugin:outer", agentxx::feature::kHostDefaultPriority, [](std::string) {
            return std::string{R"({"value":12})"};
        })
    );
    auto rc3 = co_await point3.call(PingRequest{"K"}, CallOptions{.caller = "plugin:outer"});
    XX_TEST_EXPECT_TRUE(rc3.ok());
    XX_TEST_EXPECT_EQ((rc3.ok() ? rc3.value->value : -1), 11);
    XX_TEST_EXPECT_EQ(*innerError, static_cast<int>(CallError::Busy));
    co_return;
}

// ==================== 超时 ====================

asio::awaitable<void> test_timeouts() {
    // ① implTimeoutMs: 实现挂住 -> 取消它并按"没意见"继续链, 最终拿到 core 值
    Registry registry;
    PointOptions opts;
    opts.title         = "等实现超时";
    opts.cache         = agentxx::feature::CacheMode::None;
    opts.implTimeoutMs = 60;
    auto& point        = makePingPoint(registry, "test.timeout.impl", std::move(opts));
    registry.addImpl(
        "test.timeout.impl",
        plainImpl("plugin:hang", 0, [](std::string) -> asio::awaitable<std::optional<std::string>> {
            co_await sleepMs(5000);
            co_return std::string{R"({"value":99})"};
        })
    );
    point.addCoreImpl(
        "core:test:fallback",
        0,
        [](const PingRequest&, const agentxx::feature::ImplContext&)
            -> asio::awaitable<std::optional<PingValue>> {
            co_return PingValue{1};
        }
    );
    const auto t0 = std::chrono::steady_clock::now();
    auto       r  = co_await point.ask(PingRequest{"k"});
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0
    )
                        .count();
    XX_TEST_EXPECT_TRUE(r.ok());
    XX_TEST_EXPECT_EQ((r.ok() ? r.value->value : -1), 1);
    XX_TEST_EXPECT_EQ(r.by, std::string{"core:test:fallback"});
    XX_TEST_EXPECT_TRUE(ms < 1000); // 没有等到 5 秒

    // ② 调用方 timeoutMs: 到点返回 failed, message 写清限时值
    Registry     registry2;
    PointOptions opts2;
    opts2.title    = "调用方超时";
    opts2.cache    = agentxx::feature::CacheMode::None;
    opts2.callable = true;
    auto& point2 = makePingPoint(registry2, "test.timeout.caller", std::move(opts2));
    registry2.addImpl(
        "test.timeout.caller",
        plainImpl("plugin:hang", 0, [](std::string) -> asio::awaitable<std::optional<std::string>> {
            co_await sleepMs(5000);
            co_return std::string{R"({"value":1})"};
        })
    );
    auto rc = co_await point2.call(
        PingRequest{"k"},
        CallOptions{.caller = "plugin:x", .timeout = std::chrono::milliseconds{50}}
    );
    XX_TEST_EXPECT_EQ(static_cast<int>(rc.error), static_cast<int>(CallError::Failed));
    XX_TEST_EXPECT_TRUE(rc.message.find("timeout after 50ms") != std::string::npos);

    // ③ 都不给 (默认 0 = 不限): 慢实现最终仍能给出值
    Registry registry3;
    auto&    point3 = makePingPoint(
        registry3,
        "test.timeout.none",
        pingOptions("不限时", agentxx::feature::CacheMode::None)
    );
    registry3.addImpl(
        "test.timeout.none",
        plainImpl("plugin:slow", 0, [](std::string) -> asio::awaitable<std::optional<std::string>> {
            co_await sleepMs(60);
            co_return std::string{R"({"value":31})"};
        })
    );
    auto r3 = co_await point3.ask(PingRequest{"k"});
    XX_TEST_EXPECT_TRUE(r3.ok());
    XX_TEST_EXPECT_EQ((r3.ok() ? r3.value->value : -1), 31);

    // ④ 实现自报 default_timeout_ms 与点声明取较小非 0 值
    Registry registry4;
    PointOptions opts4;
    opts4.title         = "自报超时";
    opts4.cache         = agentxx::feature::CacheMode::None;
    opts4.implTimeoutMs = 0;
    auto& point4        = makePingPoint(registry4, "test.timeout.self", std::move(opts4));
    auto  spec          = plainImpl("plugin:slow", 0, [](std::string) -> asio::awaitable<std::optional<std::string>> {
        co_await sleepMs(5000);
        co_return std::string{R"({"value":1})"};
    });
    spec.defaultTimeoutMs = 40;
    registry4.addImpl("test.timeout.self", std::move(spec));
    point4.addCoreImpl(
        "core:test:fallback",
        0,
        [](const PingRequest&, const agentxx::feature::ImplContext&)
            -> asio::awaitable<std::optional<PingValue>> {
            co_return PingValue{2};
        }
    );
    auto r4 = co_await point4.ask(PingRequest{"k"});
    XX_TEST_EXPECT_EQ((r4.ok() ? r4.value->value : -1), 2);
    co_return;
}

// ==================== 声明校验 / 清单 / 开发者模式 ====================

asio::awaitable<void> test_list_and_dev_mode() {
    Registry     registry;
    PointOptions opts;
    opts.title    = "清单测试";
    opts.depict   = "一句话说明";
    opts.callDoc  = "参数: key; 返回: {value}";
    opts.callable = false;
    auto& point   = makePingPoint(registry, "test.list", std::move(opts));
    registry.addImpl("test.list", jsonImpl("plugin:p", 0, [](std::string) {
        return std::string{R"({"value":1})"};
    }));
    point.addCoreImpl(
        "core:test:fallback",
        0,
        [](const PingRequest&, const agentxx::feature::ImplContext&)
            -> asio::awaitable<std::optional<PingValue>> {
            co_return PingValue{1};
        }
    );

    // 不可调 -> not_callable
    auto rc = co_await point.call(PingRequest{"k"}, CallOptions{.caller = "plugin:x"});
    XX_TEST_EXPECT_EQ(static_cast<int>(rc.error), static_cast<int>(CallError::NotCallable));

    // 清单字段齐全
    const auto        list = registry.listPointsJson();
    const std::string text = list.dump();
    XX_TEST_EXPECT_EQ(list.value<int>("count", 0), 1);
    XX_TEST_EXPECT_TRUE(text.find("test.list") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("\"type\":\"provide\"") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("参数: key; 返回: {value}") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("core:test:fallback") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("\"layer\":\"plugin\"") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("\"layer\":\"core\"") != std::string::npos);
    XX_TEST_EXPECT_FALSE(text.find("\"layer\":\"builtin\"") != std::string::npos);

    // 开发者模式关闭: 清单没有 stat 段
    agentxx::agent::AgentConfigStatic::devMode = false;
    XX_TEST_EXPECT_FALSE(registry.listPointsJson().dump().find("\"stat\"") != std::string::npos);

    // 开发者模式开启: 记 asks / calls / errors 与最近产出方
    agentxx::agent::AgentConfigStatic::devMode = true;
    (void)co_await point.ask(PingRequest{"k1"});
    (void)co_await point.ask(PingRequest{"k2"});
    (void)co_await point.call(PingRequest{"k3"}, CallOptions{.caller = "plugin:x"});

    const auto stats = registry.listPointsJson().dump();
    XX_TEST_EXPECT_TRUE(stats.find("\"stat\"") != std::string::npos);
    XX_TEST_EXPECT_TRUE(stats.find("\"asks\":2") != std::string::npos);
    XX_TEST_EXPECT_TRUE(stats.find("\"calls\":1") != std::string::npos);
    XX_TEST_EXPECT_TRUE(stats.find("lastCaller\":\"plugin:x") != std::string::npos);
    agentxx::agent::AgentConfigStatic::devMode = false;
    co_return;
}

/// 真实核心点: agentxx.context.countTokens (阶段 2 迁移前先验证点本身可用)
asio::awaitable<void> test_context_count_tokens_point() {
    Registry registry;
    agentxx::feature::ContextPointOptions options;
    auto points = agentxx::feature::registerContextPoints(registry, options);
    XX_TEST_EXPECT_TRUE(points.countTokens != nullptr);
    XX_TEST_EXPECT_TRUE(points.summarize != nullptr);

    agentxx::feature::CountTokensRequest req;
    req.text  = "hello world";
    req.model = "m1";
    auto r    = co_await points.countTokens->ask(req);
    XX_TEST_EXPECT_TRUE(r.ok());
    // "hello world" (11 个 ascii 字符) / 4 = 2
    XX_TEST_EXPECT_EQ((r.ok() ? r.value->tokens : -1), int64_t{2});

    // 值缓存生效 (同一身份第二次命中)
    auto r2 = co_await points.countTokens->ask(req);
    XX_TEST_EXPECT_TRUE(r2.ok());
    XX_TEST_EXPECT_TRUE(r2.fromCache);

    // 插件实现替换核心实现: 核心 ask 拿到插件给的值, by 是 plugin:<名>
    registry.addImpl(
        "agentxx.context.countTokens",
        jsonImpl("plugin:tok", 0, [](std::string) {
            return std::string{R"({"value":{"tokens":123456}})"};
        })
    );
    auto r3 = co_await points.countTokens->ask(req, agentxx::feature::AskOptions{.refresh = true});
    XX_TEST_EXPECT_EQ((r3.ok() ? r3.value->tokens : -1), int64_t{123456});
    XX_TEST_EXPECT_EQ(r3.by, std::string{"plugin:tok"});

    // 摘除后回到核心实现
    registry.removeImpl("agentxx.context.countTokens", "plugin:tok");
    auto r4 = co_await points.countTokens->ask(req, agentxx::feature::AskOptions{.refresh = true});
    XX_TEST_EXPECT_EQ((r4.ok() ? r4.value->tokens : -1), int64_t{2});

    // summarize 本轮不可调
    agentxx::feature::SummarizeRequest sreq;
    sreq.sessionId = "s1";
    auto rc        = co_await points.summarize->call(sreq, CallOptions{.caller = "plugin:x"});
    XX_TEST_EXPECT_EQ(static_cast<int>(rc.error), static_cast<int>(CallError::NotCallable));
    co_return;
}

/// 插件点: 声明 / 实现 / 撤销 / 命名空间校验
asio::awaitable<void> test_plugin_points() {
    Registry registry;
    agentxx::feature::PluginPointDecl decl;
    decl.id        = "plugin.my_plugin.beat";
    decl.owner     = "plugin:my_plugin";
    decl.title     = "节拍检测";
    decl.depict    = "算出这段音频的 BPM 与节拍时刻";
    decl.argsDoc   = "path";
    decl.resultDoc = "{bpm, beats}";
    XX_TEST_EXPECT_EQ(registry.definePluginPoint(decl), 0);

    // 命名空间校验: 别人的点不能声明
    agentxx::feature::PluginPointDecl bad = decl;
    bad.id                                 = "plugin.other_plugin.beat";
    XX_TEST_EXPECT_EQ(registry.definePluginPoint(bad), -1);
    bad.id = "agentxx.context.countTokens";
    XX_TEST_EXPECT_EQ(registry.definePluginPoint(bad), -1);
    bad.id = "plugin.my_plugin.";
    XX_TEST_EXPECT_EQ(registry.definePluginPoint(bad), -1);

    // 声明后恒为可调; 没有实现时 no_impl
    auto* point = dynamic_cast<agentxx::feature::JsonProvidePoint*>(
        registry.find("plugin.my_plugin.beat")
    );
    XX_TEST_EXPECT_TRUE(point != nullptr);
    auto rc = co_await point->call(R"({"path":"a.wav"})", CallOptions{.caller = "plugin:other"});
    XX_TEST_EXPECT_EQ(static_cast<int>(rc.error), static_cast<int>(CallError::NoImpl));

    // 另一个插件为它登记实现
    registry.addImpl(
        "plugin.my_plugin.beat",
        jsonImpl("plugin:other", 0, [](std::string callJson) {
            return callJson.find("a.wav") != std::string::npos
                       ? std::string{R"({"value":{"bpm":128}})"}
                       : std::string{R"({})"};
        })
    );
    auto r = co_await point->call(R"({"path":"a.wav"})", CallOptions{.caller = "plugin:consumer"});
    XX_TEST_EXPECT_TRUE(r.ok());
    XX_TEST_EXPECT_EQ(r.ok() ? r.value->value<int>("bpm", 0) : -1, 128);
    // 参数给不出目标 (返回空对象 = 没意见) -> no_impl
    auto rEmpty = co_await point->call(R"({"path":"b.wav"})", CallOptions{.caller = "plugin:consumer"});
    XX_TEST_EXPECT_EQ(static_cast<int>(rEmpty.error), static_cast<int>(CallError::NoImpl));

    // 撤销点: 声明方走了, 点与实现一起消失
    XX_TEST_EXPECT_EQ(registry.removePointsOwnedBy("plugin:my_plugin"), size_t{1});
    XX_TEST_EXPECT_TRUE(registry.find("plugin.my_plugin.beat") == nullptr);
    co_return;
}

/// 同步快路径与功能点取值必须是同一份实现 (一份规则, 两个入口)
/// - 压缩中间件上的 `countTokens` / `countTokensForUtf8Str` 供 TPS 与内部裁剪同步调用;
///   功能点 `agentxx.context.countTokens` 供预算计算与插件调用
/// - 这条测试守住"估算规则只写一次": 两边结果必须相等
asio::awaitable<void> test_sync_path_matches_point() {
    auto ctx        = std::make_shared<agentxx::agent::AgentContext>();
    ctx->agentConfig = std::make_shared<agentxx::agent::AgentConfig>();
    ctx->features   = std::make_shared<Registry>();

    auto handle = std::make_shared<agentxx::middleware::SummarizationMiddlewareHandle>(ctx);
    XX_TEST_EXPECT_TRUE(handle->points().countTokens != nullptr);
    XX_TEST_EXPECT_TRUE(handle->points().summarize != nullptr);

    std::vector<neograph::ChatMessage> messages;
    messages.push_back(neograph::ChatMessage{.role = "user", .content = "hello world"});
    messages.push_back(neograph::ChatMessage{.role = "assistant", .content = "你好，世界"});

    const size_t syncCount = handle->countTokens({}, messages, false);
    const size_t pointa    = co_await handle->countTokensViaPoint("s1", messages, false);
    XX_TEST_EXPECT_EQ(syncCount, pointa);
    // 纯文本估算同样一致
    XX_TEST_EXPECT_EQ(
        handle->countTokensForUtf8Str("hello world"),
        static_cast<size_t>(handle->estimator()->estimateText("hello world"))
    );

    // 声明顺序: 注册表里两个核心点已存在, 且 token 估算可被外部调用
    auto* point = ctx->features->find(std::string{agentxx::feature::points::kContextCountTokens});
    XX_TEST_EXPECT_TRUE(point != nullptr);
    if (point != nullptr) {
        XX_TEST_EXPECT_TRUE(point->callable());
        XX_TEST_EXPECT_EQ(point->origin(), std::string{"core"});
    }
    auto* summarizePoint
        = ctx->features->find(std::string{agentxx::feature::points::kContextSummarize});
    XX_TEST_EXPECT_TRUE(summarizePoint != nullptr);
    if (summarizePoint != nullptr) {
        XX_TEST_EXPECT_FALSE(summarizePoint->callable());
    }
    co_return;
}

} // namespace

asio::awaitable<TestResult> run_feature_points_tests() {
    try {
        co_await test_order_and_priority_band();
        co_await test_no_registration_limit();
        co_await test_exception_isolation();
        co_await test_disable_marker();
        co_await test_value_cache();
        co_await test_inflight_dedup();
        co_await test_call_protections();
        co_await test_timeouts();
        co_await test_list_and_dev_mode();
        co_await test_context_count_tokens_point();
        co_await test_plugin_points();
        co_await test_sync_path_matches_point();
    } catch (const std::exception& e) {
        TEST_FAIL << "feature_points suite exception: " << e.what() << std::endl;
        g_fp_failed++;
    }
    co_return TestResult{g_fp_passed, g_fp_failed};
}

} // namespace test
} // namespace agentxx