#include "agentxx-test/core/test_event_hub.h"

#include "agentxx/agent/context.h"
#include "agentxx/agent/session_input_sink.h"
#include "agentxx/event/hub.h"
#include "agentxx/tools/event.h"
#include "agentxx/util/tool_result_text.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "utilxx_base/asio_error.h"
#include <chrono>
#include <iostream>
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

asio::awaitable<std::shared_ptr<ToolFixture>> makeToolFixture(bool withHub = true) {
    auto fx     = std::make_shared<ToolFixture>();
    auto ex     = co_await asio::this_coro::executor;
    fx->ctx     = std::make_shared<agentxx::agent::AgentContext>();
    fx->ctx->agentConfig = std::make_shared<agentxx::agent::AgentConfig>();
    fx->session = fx->ctx->sessions->getOrCreate(fx->sessionId);
    fx->sink    = std::make_shared<ToolFakeSink>();
    fx->session->inputSink = fx->sink;
    if (withHub) {
        fx->hub            = std::make_shared<agentxx::events::EventHub>(ex, fx->ctx);
        fx->ctx->eventHub  = fx->hub;
    }
    fx->tool = std::make_unique<agentxx::tools::EventTool>(fx->ctx);
    co_return fx;
}

asio::awaitable<void> toolSleepMs(int64_t ms) {
    asio::steady_timer timer(co_await asio::this_coro::executor, std::chrono::milliseconds{ms});
    AsioErrorCode      ec;
    co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
}

/// 执行工具并把异常转成带前缀的文本 (断言只看文本, 保持用例线性)
///
/// 这里同时守住一条跨模块的约定: 成功返回的文本不得命中工具失败前缀
/// (`agentxx/util/tool_result_text.h`)。事件文本选 `[Event] ...` 前缀就是为了满足它,
/// 改文案时这个断言会第一时间报出来。
asio::awaitable<std::string>
    execTool(agentxx::tools::EventTool& tool, Json args) {
    std::string text;
    try {
        text = co_await tool.execute_async(args);
    } catch (const std::exception& e) {
        // 抛异常才是执行层记失败的路子, 这里转换成文本只是为了让用例保持线性
        co_return fmt::format("[threw] {}", e.what());
    }
    XX_TEST_EXPECT_FALSE(agentxx::util::toolResultTextIsFailure(text));
    co_return text;
}

bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

} // namespace

/// 1. 工具定义: 名称 / opt 枚举 / 必填项
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
    XX_TEST_EXPECT_TRUE(optEnum.size() == 7);
    const auto& modeEnum = params["properties"]["mode"]["enum"];
    XX_TEST_EXPECT_TRUE(modeEnum.size() == 3);
    co_return;
}

/// 2. watch: 挂起文本 + 值到达后进入会话
asio::awaitable<void> test_tool_watch_and_emit() {
    auto fx = co_await makeToolFixture();
    fx->sink->busy = true;

    auto armed = co_await execTool(
        *fx->tool,
        Json{
            {"opt",       "watch"      },
            {"event",     "build.done" },
            {"mode",      "first"      },
            {"sessionId", fx->sessionId},
            {"label",     "waiting for the build"},
        }
    );
    XX_TEST_EXPECT_TRUE(armed.starts_with("[Event wait armed]"));
    XX_TEST_EXPECT_TRUE(contains(armed, "wait=w-1"));
    XX_TEST_EXPECT_TRUE(contains(armed, "event=build.done"));
    XX_TEST_EXPECT_TRUE(contains(armed, "mode=first"));
    XX_TEST_EXPECT_TRUE(contains(armed, "delivery=next-step"));
    XX_TEST_EXPECT_TRUE(fx->hub->waitCount() == 1);

    auto emitResult = co_await execTool(
        *fx->tool,
        Json{
            {"opt",    "emit"                     },
            {"event",  "build.done"               },
            {"value",  Json{{"ok", true}}         },
            {"sessionId", fx->sessionId           },
        }
    );
    XX_TEST_EXPECT_TRUE(emitResult.starts_with("[Event emit]"));
    XX_TEST_EXPECT_TRUE(contains(emitResult, "published=1"));
    // 等待已被这个值满足 (first 模式), 因此不再计入"挂起中"的等待数
    XX_TEST_EXPECT_TRUE(contains(emitResult, "waits=0"));

    // 值已投递 (轮次在跑 → 待注入输入)
    auto inputs = fx->session->takePendingInputs(agentxx::agent::InputDelivery::NextStep);
    XX_TEST_EXPECT_TRUE(inputs.size() == 1);
    XX_TEST_EXPECT_TRUE(contains(inputs.front().text, "[Event] build.done (first)"));
    XX_TEST_EXPECT_TRUE(contains(inputs.front().text, R"({"ok":true})"));
    XX_TEST_EXPECT_TRUE(contains(inputs.front().text, "label: waiting for the build"));
    XX_TEST_EXPECT_TRUE(inputs.front().source == "event:build.done");
    co_return;
}

/// 3. watch 已满足时直接返回值 (不产生挂起的等待)
asio::awaitable<void> test_tool_watch_replay() {
    auto fx = co_await makeToolFixture();
    fx->sink->busy = true;
    std::string err;
    fx->hub->publishValue("already.there", Json{{"n", 3}}, &err);

    auto text = co_await execTool(
        *fx->tool,
        Json{
            {"opt",       "watch"        },
            {"event",     "already.there"},
            {"sessionId", fx->sessionId  },
        }
    );
    XX_TEST_EXPECT_TRUE(text.starts_with("[Event] already.there (first)"));
    XX_TEST_EXPECT_TRUE(contains(text, "status: value"));
    XX_TEST_EXPECT_TRUE(contains(text, R"({"n":3})"));
    // 回放后该等待已满足 (不再处于挂起状态), 没有新增挂起
    XX_TEST_EXPECT_TRUE(fx->hub->listWaitsJson(fx->sessionId)["count"].get<int>() == 1);
    XX_TEST_EXPECT_TRUE(fx->hub->waitCount() == 1);
    co_return;
}

/// 4. await: 拿到值 / 超时 / 超时上限被裁剪
asio::awaitable<void> test_tool_await() {
    auto fx = co_await makeToolFixture();
    auto ex = co_await asio::this_coro::executor;

    asio::co_spawn(
        ex,
        [hub = fx->hub]() -> asio::awaitable<void> {
            co_await toolSleepMs(60);
            std::string ignored;
            hub->publishValue("await.event", Json("payload"), &ignored);
            co_return;
        },
        asio::detached
    );
    auto got = co_await execTool(
        *fx->tool,
        Json{
            {"opt",        "await"      },
            {"event",      "await.event"},
            {"sessionId",  fx->sessionId},
            {"timeout_ms", 2000         },
        }
    );
    XX_TEST_EXPECT_TRUE(got.starts_with("[Event] await.event (first)"));
    XX_TEST_EXPECT_TRUE(contains(got, "status: value"));
    XX_TEST_EXPECT_TRUE(contains(got, "value: payload"));

    auto timedOut = co_await execTool(
        *fx->tool,
        Json{
            {"opt",        "await"       },
            {"event",      "never.event" },
            {"sessionId",  fx->sessionId },
            {"timeout_ms", 80            },
        }
    );
    XX_TEST_EXPECT_TRUE(timedOut.starts_with("[Event await timeout]"));
    XX_TEST_EXPECT_TRUE(contains(timedOut, "event=never.event"));
    XX_TEST_EXPECT_TRUE(fx->hub->waitCount() == 0);

    // 超过硬上限时被裁剪到 5 分钟 (只看不报错: 立即可取消)
    auto cancelTokenArgs = Json{
        {"opt",        "cancel"       },
        {"event",      "never.event"  },
        {"sessionId",  fx->sessionId  },
        {"all",        true           },
    };
    auto Canceled = co_await execTool(*fx->tool, cancelTokenArgs);
    XX_TEST_EXPECT_TRUE(Canceled.starts_with("[Event cancel]"));
    co_return;
}

/// 5. status / list / cancel 的输出
asio::awaitable<void> test_tool_status_list_cancel() {
    auto fx = co_await makeToolFixture();
    fx->sink->busy = true;

    auto armed = co_await execTool(
        *fx->tool,
        Json{
            {"opt",       "watch"    },
            {"event",     "status.me"},
            {"mode",      "each"     },
            {"sessionId", fx->sessionId},
        }
    );
    XX_TEST_EXPECT_TRUE(armed.starts_with("[Event wait armed]"));
    const auto waitId = armed.substr(armed.find("wait=") + 5, armed.find(" event=") - (armed.find("wait=") + 5));

    auto waitStatus = co_await execTool(
        *fx->tool,
        Json{{"opt", "status"}, {"wait", waitId}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(waitStatus.starts_with("[Event wait] wait="));
    XX_TEST_EXPECT_TRUE(contains(waitStatus, "event=status.me"));
    XX_TEST_EXPECT_TRUE(contains(waitStatus, "state=armed"));

    auto eventStatus = co_await execTool(
        *fx->tool,
        Json{{"opt", "status"}, {"event", "status.me"}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(eventStatus.starts_with("[Event record] name=status.me"));
    XX_TEST_EXPECT_TRUE(contains(eventStatus, "waits=1"));

    auto listAll = co_await execTool(
        *fx->tool,
        Json{{"opt", "list"}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(contains(listAll, "[Event list] events="));
    XX_TEST_EXPECT_TRUE(contains(listAll, "status.me"));
    XX_TEST_EXPECT_TRUE(contains(listAll, "[Event list] waits=1"));
    XX_TEST_EXPECT_TRUE(contains(listAll, waitId));

    auto listEventsOnly = co_await execTool(
        *fx->tool,
        Json{{"opt", "list"}, {"scope", "events"}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(contains(listEventsOnly, "[Event list] events="));
    // 事件行自带 waits=N 字段, 因此只能断言"没有等待段标题"
    XX_TEST_EXPECT_FALSE(contains(listEventsOnly, "[Event list] waits="));

    auto cancelOne = co_await execTool(
        *fx->tool,
        Json{{"opt", "cancel"}, {"wait", waitId}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(cancelOne.starts_with("[Event cancel] wait="));
    XX_TEST_EXPECT_TRUE(contains(cancelOne, "Canceled"));
    XX_TEST_EXPECT_TRUE(fx->hub->queryWait(waitId)->state == agentxx::events::WaitState::Canceled);

    // 已取消的等待再取消: 参数错误 (不是静默成功)
    auto cancelAgain = co_await execTool(
        *fx->tool,
        Json{{"opt", "cancel"}, {"wait", waitId}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(cancelAgain.starts_with("[threw] "));
    co_return;
}

/// 6. end / remove 与 emit 的 single 形态
asio::awaitable<void> test_tool_emit_end() {
    auto fx = co_await makeToolFixture();
    std::string err;
    auto        single = co_await execTool(
        *fx->tool,
        Json{
            {"opt",       "emit"       },
            {"event",     "one.value"  },
            {"single",    true         },
            {"value",     Json{{"a", 1}}},
            {"sessionId", fx->sessionId},
        }
    );
    XX_TEST_EXPECT_TRUE(single.starts_with("[Event emit] event=one.value"));
    XX_TEST_EXPECT_TRUE(fx->hub->queryEvent("one.value")->kind == agentxx::events::EventKind::Single);
    XX_TEST_EXPECT_TRUE(
        fx->hub->queryEvent("one.value")->state == agentxx::events::EventState::Ended
    );

    auto endText = co_await execTool(
        *fx->tool,
        Json{{"opt", "end"}, {"event", "one.value"}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(endText.starts_with("[Event end] event=one.value"));

    auto removeText = co_await execTool(
        *fx->tool,
        Json{{"opt", "end"}, {"event", "one.value"}, {"remove", true}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(contains(removeText, "removed=true"));
    XX_TEST_EXPECT_FALSE(fx->hub->queryEvent("one.value").has_value());

    // 结束不存在的事件: 参数错误
    auto missing = co_await execTool(
        *fx->tool,
        Json{{"opt", "end"}, {"event", "nope"}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(missing.starts_with("[threw] "));
    co_return;
}

/// 7. 失败形态: 全部以 `[threw] ` 开头 (由 ToolcallWrapNode 转成 [Error: ...])
asio::awaitable<void> test_tool_errors() {
    auto fx = co_await makeToolFixture();

    auto noOpt = co_await execTool(*fx->tool, Json{{"sessionId", fx->sessionId}});
    XX_TEST_EXPECT_TRUE(noOpt.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(noOpt, "opt"));

    auto noEvent = co_await execTool(
        *fx->tool,
        Json{{"opt", "watch"}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(noEvent.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(noEvent, "event"));

    auto reserved = co_await execTool(
        *fx->tool,
        Json{{"opt", "watch"}, {"event", "agent.turn.start"}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(reserved.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(reserved, "reserved"));

    auto badMode = co_await execTool(
        *fx->tool,
        Json{
            {"opt",       "watch"     },
            {"event",     "ok.event"  },
            {"mode",      "sometimes"},
            {"sessionId", fx->sessionId},
        }
    );
    XX_TEST_EXPECT_TRUE(badMode.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(badMode, "mode must be"));

    auto badOpt = co_await execTool(
        *fx->tool,
        Json{{"opt", "frobnicate"}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(badOpt.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(badOpt, "opt"));

    auto noSession = co_await execTool(*fx->tool, Json{{"opt", "list"}});
    XX_TEST_EXPECT_TRUE(noSession.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(noSession, "sessionId"));

    auto statusMissingArg = co_await execTool(
        *fx->tool,
        Json{{"opt", "status"}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(statusMissingArg.starts_with("[threw] "));

    auto unknownWait = co_await execTool(
        *fx->tool,
        Json{{"opt", "status"}, {"wait", "w-999"}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(unknownWait.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(unknownWait, "not found"));
    co_return;
}

/// 8. 中枢未启用时 (event.enable=false) 明确报错
asio::awaitable<void> test_tool_without_hub() {
    auto fx = co_await makeToolFixture(/*withHub=*/false);
    auto text = co_await execTool(
        *fx->tool,
        Json{{"opt", "watch"}, {"event", "x.y"}, {"sessionId", fx->sessionId}}
    );
    XX_TEST_EXPECT_TRUE(text.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(text, "disabled by config"));
    co_return;
}

/// 9. 定时源: watch 时用 after_ms 顺手建一个
asio::awaitable<void> test_tool_watch_timer() {
    auto fx = co_await makeToolFixture();
    fx->sink->busy = true;

    auto armed = co_await execTool(
        *fx->tool,
        Json{
            {"opt",       "watch"    },
            {"event",     "timer.soon"},
            {"mode",      "first"    },
            {"after_ms",  40         },
            {"sessionId", fx->sessionId},
        }
    );
    XX_TEST_EXPECT_TRUE(armed.starts_with("[Event wait armed]"));
    XX_TEST_EXPECT_TRUE(contains(armed, "event=timer.soon"));

    // 定时到点后值进入会话
    bool delivered = false;
    for (int i = 0; i < 200 && !delivered; ++i) {
        co_await toolSleepMs(10);
        delivered = fx->session->pendingInputCount("event:") > 0;
    }
    XX_TEST_EXPECT_TRUE(delivered);
    auto inputs = fx->session->takePendingInputs(agentxx::agent::InputDelivery::NextStep);
    XX_TEST_EXPECT_TRUE(inputs.size() == 1);
    XX_TEST_EXPECT_TRUE(contains(inputs.front().text, "[Event] timer.soon"));

    // 非法延迟: 参数错误
    auto badTimer = co_await execTool(
        *fx->tool,
        Json{
            {"opt",       "watch"     },
            {"event",     "timer.bad" },
            {"after_ms",  -5          },
            {"sessionId", fx->sessionId},
        }
    );
    XX_TEST_EXPECT_TRUE(badTimer.starts_with("[Event wait armed]"));
    co_return;
}

asio::awaitable<TestResult> run_event_tools_tests() {
    TestResult result;

    co_await test_tool_definition();
    co_await test_tool_watch_and_emit();
    co_await test_tool_watch_replay();
    co_await test_tool_await();
    co_await test_tool_status_list_cancel();
    co_await test_tool_emit_end();
    co_await test_tool_errors();
    co_await test_tool_without_hub();
    co_await test_tool_watch_timer();

    result.passed = g_et_passed;
    result.failed = g_et_failed;
    co_return result;
}

} // namespace test
} // namespace agentxx
