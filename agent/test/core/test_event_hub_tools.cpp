#include "agentxx-test/core/test_event_hub.h"
#include "agentxx-test/core/test_event_naming.h"

#include "agentxx/agent/context.h"
#include "agentxx/agent/session_input_sink.h"
#include "agentxx/event/hub.h"
#include "agentxx/tools/event.h"
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
    std::unique_ptr<agentxx::tools::EventTool>    tool;
    std::string                                   sessionId = "s1";
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
    fx->tool               = std::make_unique<agentxx::tools::EventTool>(fx->ctx);
    co_return fx;
}

/// 执行工具并把异常转成带前缀的文本 (断言只看文本, 保持用例线性)
///
/// 同时守住约定: 事件工具的**成功**结果状态位一定是 `Ok` ——
/// 文本不再参与成败判定, 文案怎么改都不会被误认为失败。
asio::awaitable<std::string> execTool(agentxx::tools::EventTool& tool, Json args) {
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

} // namespace

/// 1. 工具定义: 名称 / opt 只剩 emit 与 end / 必填项
asio::awaitable<void> test_tool_definition() {
    auto fx  = co_await makeToolFixture();
    auto def = fx->tool->get_definition();
    XX_TEST_EXPECT_TRUE(def.name == "agentxx_event");
    XX_TEST_EXPECT_FALSE(def.description.empty());
    XX_TEST_EXPECT_TRUE(def.parameters.is_object());
    const auto& params = def.parameters;
    XX_TEST_EXPECT_TRUE(params["required"].is_array());
    XX_TEST_EXPECT_TRUE(params["required"][0].get<std::string>() == "opt");
    const auto& optEnum = params["properties"]["opt"]["enum"];
    XX_TEST_EXPECT_TRUE(optEnum.is_array());
    XX_TEST_EXPECT_EQ(optEnum.size(), size_t{2});
    XX_TEST_EXPECT_EQ(optEnum[0].get<std::string>(), std::string{"emit"});
    XX_TEST_EXPECT_EQ(optEnum[1].get<std::string>(), std::string{"end"});
    // 读侧参数已拆走 (watch / await / status / list / works / cancel)
    for (const auto* gone : {"mode", "delivery", "wait", "after_ms", "every_ms", "scope"}) {
        XX_TEST_EXPECT_FALSE(params["properties"].contains(gone));
    }
    // 写侧参数都在
    for (const auto* kept : {"event", "value", "summary", "single", "create", "label", "remove"}) {
        XX_TEST_EXPECT_TRUE(params["properties"].contains(kept));
    }
    co_return;
}

/// 2. emit: 建事件 + 发值; single 形态第一次发值就结束
asio::awaitable<void> test_tool_emit_end() {
    auto fx = co_await makeToolFixture();
    auto emitted = co_await execTool(
        *fx->tool,
        Json{
            {"opt",       "emit"},
            {"event",     evName("build.progress")},
            {"value",     Json{{"step", 1}}},
            {"sessionId", fx->sessionId},
        }
    );
    XX_TEST_EXPECT_TRUE(emitted.starts_with("[Event emit] event=" + evName("build.progress")));
    XX_TEST_EXPECT_TRUE(contains(emitted, "state=open"));
    XX_TEST_EXPECT_TRUE(contains(emitted, "published=1"));
    auto record = fx->hub->queryEvent(evName("build.progress"));
    XX_TEST_EXPECT_TRUE(record.has_value());
    XX_TEST_EXPECT_TRUE(record->kind == agentxx::events::EventKind::Stream);
    XX_TEST_EXPECT_EQ(record->published, uint64_t{1});

    // 同一事件再发一次 (只加计数, 不重复建)
    auto emittedAgain = co_await execTool(
        *fx->tool,
        Json{
            {"opt",       "emit"},
            {"event",     evName("build.progress")},
            {"value",     Json{{"step", 2}}},
            {"sessionId", fx->sessionId},
        }
    );
    XX_TEST_EXPECT_TRUE(contains(emittedAgain, "published=2"));

    // end: 结束流
    auto ended = co_await execTool(
        *fx->tool,
        Json{
            {"opt",       "end"},
            {"event",     evName("build.progress")},
            {"summary",   Json{{"done", true}}},
            {"sessionId", fx->sessionId},
        }
    );
    XX_TEST_EXPECT_TRUE(ended.starts_with("[Event end] event=" + evName("build.progress")));
    XX_TEST_EXPECT_TRUE(contains(ended, "published=2"));
    XX_TEST_EXPECT_TRUE(
        fx->hub->queryEvent(evName("build.progress"))->state
        == agentxx::events::EventState::Ended
    );

    // single: 第一次发值即结束, 不需要再 end
    auto single = co_await execTool(
        *fx->tool,
        Json{
            {"opt",       "emit"},
            {"event",     evName("one.value")},
            {"single",    true},
            {"value",     Json{{"a", 1}}},
            {"label",     "one shot"},
            {"sessionId", fx->sessionId},
        }
    );
    XX_TEST_EXPECT_TRUE(single.starts_with("[Event emit] event=" + evName("one.value")));
    XX_TEST_EXPECT_TRUE(
        fx->hub->queryEvent(evName("one.value"))->kind == agentxx::events::EventKind::Single
    );
    XX_TEST_EXPECT_TRUE(
        fx->hub->queryEvent(evName("one.value"))->state == agentxx::events::EventState::Ended
    );

    // end + remove: 记录直接消失
    auto removeText = co_await execTool(
        *fx->tool,
        Json{
            {"opt",       "end" },
            {"event",     evName("one.value")},
            {"remove",    true  },
            {"sessionId", fx->sessionId},
        }
    );
    XX_TEST_EXPECT_TRUE(contains(removeText, "removed=true"));
    XX_TEST_EXPECT_FALSE(fx->hub->queryEvent(evName("one.value")).has_value());
    co_return;
}

/// 3. 失败形态: 全部以 `[threw] ` 开头 (由 ToolcallWrapNode 转成 [Error: ...])
asio::awaitable<void> test_tool_errors() {
    auto fx = co_await makeToolFixture();

    auto noOpt = co_await execTool(*fx->tool, Json{{"sessionId", fx->sessionId}});
    XX_TEST_EXPECT_TRUE(noOpt.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(noOpt, "opt"));

    auto badOpt = co_await execTool(
        *fx->tool,
        Json{{"opt", "frobnicate"}, {"event", evName("x")}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(badOpt.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(badOpt, "emit"));

    // 读侧 opt 已经不存在了: 明确按"只能是 emit/end"报错
    auto oldWatch = co_await execTool(
        *fx->tool,
        Json{{"opt", "watch"}, {"event", evName("x")}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(oldWatch.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(oldWatch, "emit"));

    auto noEvent = co_await execTool(*fx->tool, Json{{"opt", "emit"}, {"sessionId", fx->sessionId}});
    XX_TEST_EXPECT_TRUE(noEvent.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(noEvent, "event"));

    // 写侧不能占框架保留子段 (内建事件源会用同名事件)
    auto reserved = co_await execTool(
        *fx->tool,
        Json{
            {"opt",       "emit"      },
            {"event",     evName("turn.start")},
            {"value",     1           },
            {"sessionId", fx->sessionId},
        }
    );
    XX_TEST_EXPECT_TRUE(reserved.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(reserved, "reserved"));

    // 写侧也不能写插件族 (`plugin.<插件名>.*` 归插件自己)
    auto pluginWrite = co_await execTool(
        *fx->tool,
        Json{
            {"opt",       "emit"},
            {"event",     "plugin.demo.x"},
            {"value",     1     },
            {"sessionId", fx->sessionId},
        }
    );
    XX_TEST_EXPECT_TRUE(pluginWrite.starts_with("[threw] "));

    // 结束不存在的事件: 参数错误
    auto missing = co_await execTool(
        *fx->tool,
        Json{{"opt", "end"}, {"event", evName("nope")}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(missing.starts_with("[threw] "));

    // 会话 id 由执行层注入, 缺失时报内部错误
    auto noSession = co_await execTool(*fx->tool, Json{{"opt", "emit"}, {"event", evName("x")}});
    XX_TEST_EXPECT_TRUE(noSession.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(noSession, "sessionId"));
    co_return;
}

asio::awaitable<TestResult> run_event_tools_tests() {
    TestResult result;

    co_await test_tool_definition();
    co_await test_tool_emit_end();
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
