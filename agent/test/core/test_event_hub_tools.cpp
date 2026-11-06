#include "agentxx-test/core/test_event_hub.h"
#include "agentxx-test/core/test_event_naming.h"

#include "agentxx/agent/context.h"
#include "agentxx/agent/session_input_sink.h"
#include "agentxx/event/hub.h"
#include "agentxx/tools/async_emit.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "utilxx_base/asio_error.h"
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_et_passed = 0;
int g_et_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_et_passed
#define XX_TEST_FAILED g_et_failed

namespace agentxx {
namespace test {

using namespace utilxx_base;
using Json = utilxx_base::Json;

namespace {

/// 假驱动端 (同 event_hub 模块的做法; 两模块各一份避免共享状态)
class ToolFakeSink : public agentxx::agent::SessionInputSink {
public:

    bool                     busy = false;
    std::vector<std::string> texts;

    bool turnActive() const override {
        return busy;
    }

    bool submitTurnInput(std::string_view text, std::string_view, bool) override {
        texts.emplace_back(text);
        return true;
    }
};

struct ToolFixture {
    std::shared_ptr<agentxx::agent::AgentContext> ctx;
    std::shared_ptr<agentxx::agent::Session>      session;
    std::shared_ptr<ToolFakeSink>                 sink;
    std::shared_ptr<agentxx::events::EventHub>    hub;
    std::unique_ptr<agentxx::tools::AsyncEmitTool> tool;
    std::string                                    sessionId = "s1";
};

asio::awaitable<std::shared_ptr<ToolFixture>> makeToolFixture() {
    auto fx     = std::make_shared<ToolFixture>();
    auto ex     = co_await asio::this_coro::executor;
    fx->ctx     = std::make_shared<agentxx::agent::AgentContext>();
    fx->ctx->agentConfig = std::make_shared<agentxx::agent::AgentConfig>();
    fx->session = fx->ctx->sessions->getOrCreate(fx->sessionId);
    fx->sink    = std::make_shared<ToolFakeSink>();
    fx->session->inputSink = fx->sink;
    fx->hub                = std::make_shared<agentxx::events::EventHub>(ex, fx->ctx);
    fx->ctx->eventHub      = fx->hub;
    fx->tool               = std::make_unique<agentxx::tools::AsyncEmitTool>(fx->ctx);
    co_return fx;
}

/// 执行工具并把异常转成带前缀的文本 (断言只看文本, 保持用例线性)
///
/// 同时守住约定: 事件工具的**成功**结果状态位一定是 `Ok` ——
/// 文本不再参与成败判定, 文案怎么改都不会被误认为失败。
asio::awaitable<std::string> execTool(agentxx::tools::AsyncEmitTool& tool, Json args) {
    try {
        auto result = co_await tool.execute_async(args);
        XX_TEST_EXPECT_TRUE(result.status == agentxx::util::ToolStatus::Ok);
        co_return std::move(result.text);
    } catch (const std::exception& e) {
        // 抛异常才是执行层记失败的路子, 这里转换成文本只是为了让用例保持线性
        co_return fmt::format("[threw] {}", e.what());
    }
}

bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

/// 登记一个只留在中枢里的等待 (delivery=none: 不往会话投递)
std::string watchInHub(const std::shared_ptr<agentxx::events::EventHub>& hub,
                       std::string_view                                eventName,
                       std::string_view                                mode = "first") {
    agentxx::events::EventWaitSpec spec;
    spec.event         = std::string{eventName};
    spec.mode          = std::string{mode};
    spec.delivery      = "none";
    spec.originSession = "s1";
    spec.owner         = "test";
    spec.replay        = false;
    auto wait          = hub->watch(std::move(spec));
    return wait.has_value() ? wait->waitId : std::string{};
}

} // namespace

/// 1. 工具定义: 名称 / 参数集 (end 取代 opt, 创建期参数只在创建时用)
asio::awaitable<void> test_tool_definition() {
    auto fx  = co_await makeToolFixture();
    auto def = fx->tool->get_definition();
    XX_TEST_EXPECT_TRUE(def.name == "agentxx_async_emit");
    XX_TEST_EXPECT_FALSE(def.description.empty());
    XX_TEST_EXPECT_TRUE(def.parameters.is_object());
    const auto& params = def.parameters;
    XX_TEST_EXPECT_TRUE(params["required"].is_array());
    XX_TEST_EXPECT_EQ(params["required"].size(), size_t{1});
    XX_TEST_EXPECT_EQ(params["required"][0].get<std::string>(), std::string{"event"});
    // opt / single / create / remove 都不再存在
    for (const auto* gone : {"opt", "single", "create", "remove", "mode", "watch"}) {
        XX_TEST_EXPECT_FALSE(params["properties"].contains(gone));
    }
    for (const auto* kept : {"event", "value", "end", "summary", "allow_publish", "label"}) {
        XX_TEST_EXPECT_TRUE(params["properties"].contains(kept));
    }
    XX_TEST_EXPECT_TRUE(params["properties"]["end"]["type"].get<std::string>() == "boolean");
    co_return;
}

/// 2. 发布: 第一次发布顺手建流, 之后的发布只累加计数
asio::awaitable<void> test_publish_creates_and_counts() {
    auto fx = co_await makeToolFixture();
    auto first = co_await execTool(
        *fx->tool,
        Json{
            {"event",     evName("build.progress")},
            {"value",     Json{{"step", 1}}   },
            {"label",     "build progress"    },
            {"sessionId", fx->sessionId       },
        }
    );
    XX_TEST_EXPECT_FALSE(first.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(first, "Created a new stream"));
    XX_TEST_EXPECT_TRUE(contains(first, "publishing allowed to: owner"));
    XX_TEST_EXPECT_TRUE(contains(first, "1 value(s) published so far"));
    XX_TEST_EXPECT_TRUE(contains(first, "nobody was waiting"));
    // 结果不复述参数: 事件名与值都不出现
    XX_TEST_EXPECT_FALSE(contains(first, evName("build.progress")));
    XX_TEST_EXPECT_FALSE(contains(first, "step"));

    auto record = fx->hub->queryEvent(evName("build.progress"));
    XX_TEST_EXPECT_TRUE(record.has_value());
    XX_TEST_EXPECT_TRUE(record->kind == agentxx::events::EventKind::Stream);
    XX_TEST_EXPECT_TRUE(record->state == agentxx::events::EventState::Open);
    XX_TEST_EXPECT_EQ(record->published, uint64_t{1});
    XX_TEST_EXPECT_TRUE(record->description == "build progress");
    XX_TEST_EXPECT_TRUE(record->owner == "tool");

    auto second = co_await execTool(
        *fx->tool,
        Json{
            {"event",     evName("build.progress")},
            {"value",     Json{{"step", 2}}   },
            {"sessionId", fx->sessionId       },
        }
    );
    XX_TEST_EXPECT_TRUE(contains(second, "2 value(s) published so far"));
    XX_TEST_EXPECT_FALSE(contains(second, "Created a new stream"));
    co_return;
}

/// 3. 投递统计: 说清这次值给了哪个等待, 还剩几个等待
asio::awaitable<void> test_publish_reports_deliveries() {
    auto fx = co_await makeToolFixture();
    auto create = co_await execTool(
        *fx->tool,
        Json{{"event", evName("chat.message")}, {"value", "hello"}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(contains(create, "Created a new stream"));

    const auto waitId = watchInHub(fx->hub, evName("chat.message"));
    XX_TEST_EXPECT_FALSE(waitId.empty());

    auto delivered = co_await execTool(
        *fx->tool,
        Json{{"event", evName("chat.message")}, {"value", "again"}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(contains(delivered, "delivered to 1 waiter(s)"));
    XX_TEST_EXPECT_TRUE(contains(delivered, waitId));
    XX_TEST_EXPECT_TRUE(contains(delivered, "none"));
    XX_TEST_EXPECT_TRUE(contains(delivered, "2 value(s) published so far"));
    // first 模式拿到就结束: 没有剩余等待
    XX_TEST_EXPECT_FALSE(contains(delivered, "still attached"));
    co_return;
}

/// 4. 收尾: end 单独用 / 与 value 一起用 / 重复收尾是空操作
asio::awaitable<void> test_end_stream() {
    auto fx = co_await makeToolFixture();
    co_await execTool(
        *fx->tool,
        Json{{"event", evName("job.done")}, {"value", 1}, {"sessionId", fx->sessionId}}
    );
    const auto waitId = watchInHub(fx->hub, evName("job.done"), "each");
    XX_TEST_EXPECT_FALSE(waitId.empty());

    auto ended = co_await execTool(
        *fx->tool,
        Json{
            {"event",     evName("job.done")},
            {"end",       true               },
            {"summary",   Json{{"ok", true}} },
            {"sessionId", fx->sessionId      },
        }
    );
    XX_TEST_EXPECT_FALSE(ended.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(ended, "stream closed after 1 value(s)"));
    XX_TEST_EXPECT_TRUE(contains(ended, "1 waiter(s) finished without a value"));
    XX_TEST_EXPECT_TRUE(
        fx->hub->queryEvent(evName("job.done"))->state == agentxx::events::EventState::Ended
    );

    auto again = co_await execTool(
        *fx->tool,
        Json{{"event", evName("job.done")}, {"end", true}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(contains(again, "already closed"));

    // value + end: 先发布再收尾
    auto both = co_await execTool(
        *fx->tool,
        Json{
            {"event",     evName("job.done")},
            {"value",     2                  },
            {"end",       true               },
            {"sessionId", fx->sessionId      },
        }
    );
    XX_TEST_EXPECT_TRUE(contains(both, "2 value(s) published so far"));
    XX_TEST_EXPECT_TRUE(contains(both, "stream closed after 2 value(s)"));
    XX_TEST_EXPECT_TRUE(
        fx->hub->queryEvent(evName("job.done"))->state == agentxx::events::EventState::Ended
    );
    co_return;
}

/// 5. 发布权限: 创建时不放开的发布者会被拒绝, 放开的可以跨族发布
asio::awaitable<void> test_publish_permissions() {
    auto fx = co_await makeToolFixture();
    // 模型建的事件默认只有自己能发
    co_await execTool(
        *fx->tool,
        Json{{"event", evName("mine")}, {"value", 1}, {"sessionId", fx->sessionId}}
    );
    std::string pluginErr;
    XX_TEST_EXPECT_FALSE(fx->hub->publishValue(evName("mine"), Json{2}, "plugin:demo", &pluginErr));
    XX_TEST_EXPECT_TRUE(contains(pluginErr, "only allows publishing by owner"));
    // 模型自己可以继续发
    std::string modelErr;
    XX_TEST_EXPECT_TRUE(fx->hub->publishValue(evName("mine"), Json{3}, "tool", &modelErr));

    // 创建时放开 plugin: 插件可以发, 返回值里也写清楚
    auto opened = co_await execTool(
        *fx->tool,
        Json{
            {"event",         evName("shared")       },
            {"value",         1                      },
            {"allow_publish", Json::array({"plugin"})},
            {"sessionId",     fx->sessionId          },
        }
    );
    XX_TEST_EXPECT_TRUE(contains(opened, "publishing allowed to: owner+plugin"));
    XX_TEST_EXPECT_TRUE(
        fx->hub->queryEvent(evName("shared"))->publishMask
        == agentxx::events::PublishMask::Plugin
    );
    XX_TEST_EXPECT_TRUE(fx->hub->publishValue(evName("shared"), Json{2}, "plugin:demo", &pluginErr));
    // 框架自己的事件段 (`agentxx.agent.event.*`) 内置源始终可以发布
    XX_TEST_EXPECT_TRUE(fx->hub->publishValue(evName("shared"), Json{3}, "core:turn", &pluginErr));

    // 别家的事件: 没放开 model 时工具发布被拒
    agentxx::events::EventDefineSpec pluginOwned;
    pluginOwned.name  = "plugin.demo.task";
    pluginOwned.owner = "plugin:demo";
    std::string defineErr;
    XX_TEST_EXPECT_TRUE(fx->hub->define(pluginOwned, &defineErr));
    // 插件族的事件不由框架段管: 内置源同样要看权限
    XX_TEST_EXPECT_FALSE(fx->hub->publishValue("plugin.demo.task", Json{9}, "core:turn", &pluginErr));
    auto denied = co_await execTool(
        *fx->tool,
        Json{{"event", "plugin.demo.task"}, {"value", 1}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(denied.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(denied, "only allows publishing by owner"));

    // 插件放开 model 之后, 同一个调用就成功
    agentxx::events::EventDefineSpec openSpec;
    openSpec.name        = "plugin.demo.task";
    openSpec.owner       = "plugin:demo";
    openSpec.publishMask = agentxx::events::PublishMask::Model;
    XX_TEST_EXPECT_TRUE(fx->hub->define(openSpec, &defineErr));
    auto accepted = co_await execTool(
        *fx->tool,
        Json{{"event", "plugin.demo.task"}, {"value", 2}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_FALSE(accepted.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(accepted, "1 value(s) published so far"));
    // 收尾别人的流也要权限 (这里已放开, 所以可以)
    auto closeOthers = co_await execTool(
        *fx->tool,
        Json{{"event", "plugin.demo.task"}, {"end", true}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_FALSE(closeOthers.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(closeOthers, "stream closed"));
    co_return;
}

/// 6. 失败形态: 全部以 `[threw] ` 开头 (由 ToolcallWrapNode 转成 [Error: ...])
asio::awaitable<void> test_tool_errors() {
    auto fx = co_await makeToolFixture();

    auto noEvent = co_await execTool(*fx->tool, Json{{"sessionId", fx->sessionId}});
    XX_TEST_EXPECT_TRUE(noEvent.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(noEvent, "event"));

    auto badName = co_await execTool(
        *fx->tool,
        Json{{"event", "has space"}, {"value", 1}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(badName.starts_with("[threw] "));

    // 既不发值也不收尾: 没有可做的事
    auto empty = co_await execTool(
        *fx->tool,
        Json{{"event", evName("nothing")}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(empty.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(empty, "end"));

    // summary 只在 end 时有意义
    auto straySummary = co_await execTool(
        *fx->tool,
        Json{
            {"event",     evName("stray")     },
            {"value",     1                    },
            {"summary",   Json{{"s", 1}}       },
            {"sessionId", fx->sessionId        },
        }
    );
    XX_TEST_EXPECT_TRUE(straySummary.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(straySummary, "end"));

    // 收尾不存在的事件: 参数错误
    auto missing = co_await execTool(
        *fx->tool,
        Json{{"event", evName("nope")}, {"end", true}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(missing.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(missing, "nothing to end"));

    // 内建保留子段不能被模型创建
    auto reserved = co_await execTool(
        *fx->tool,
        Json{{"event", evName("turn.start")}, {"value", 1}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(reserved.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(reserved, "may not create"));

    // 插件族的事件只能由插件建
    auto pluginCreate = co_await execTool(
        *fx->tool,
        Json{{"event", "plugin.demo.x"}, {"value", 1}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(pluginCreate.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(pluginCreate, "may not create"));

    // allow_publish 取值非法
    auto badAllow = co_await execTool(
        *fx->tool,
        Json{
            {"event",         evName("x")     },
            {"value",         1                },
            {"allow_publish", Json::array({"nobody"})},
            {"sessionId",     fx->sessionId   },
        }
    );
    XX_TEST_EXPECT_TRUE(badAllow.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(badAllow, "allow_publish"));

    // 会话 id 由执行层注入, 缺失时报内部错误
    auto noSession = co_await execTool(*fx->tool, Json{{"event", evName("x")}, {"value", 1}});
    XX_TEST_EXPECT_TRUE(noSession.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(noSession, "sessionId"));
    co_return;
}

asio::awaitable<TestResult> run_event_tools_tests() {
    TestResult result;

    co_await test_tool_definition();
    co_await test_publish_creates_and_counts();
    co_await test_publish_reports_deliveries();
    co_await test_end_stream();
    co_await test_publish_permissions();
    co_await test_tool_errors();

    // 中枢常驻, 这里顺带守住"建出来就能用"
    {
        auto fx = co_await makeToolFixture();
        XX_TEST_EXPECT_TRUE(fx->hub != nullptr);
        XX_TEST_EXPECT_TRUE(fx->hub->eventCount() == 0);
    }

    result.passed = g_et_passed;
    result.failed = g_et_failed;
    co_return result;
}

} // namespace test
} // namespace agentxx
