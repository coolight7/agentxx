#include "agentxx-test/core/test_async_tools.h"
#include "agentxx-test/core/test_agent.h"
#include "agentxx-test/test_coro_util.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/async_work.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/session_input_sink.h"
#include "agentxx/plugin/tool_registry.h"
#include "agentxx/tools/async_tools.h"
#include "agentxx/tools/tool.h"
#include "agentxx/util/tool_result.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见)
int g_at_passed = 0;
int g_at_failed = 0;
} // namespace

#define XX_TEST_PASSED g_at_passed
#define XX_TEST_FAILED g_at_failed

namespace agentxx {
namespace test {

using namespace utilxx_base;
using Json = utilxx_base::Json;

namespace {

void pumpIoUntil(const std::shared_ptr<asio::io_context>& io, const std::atomic<bool>& done) {
    while (!done.load()) {
        asio::steady_timer t(*io);
        t.expires_after(std::chrono::milliseconds(5));
        t.async_wait([](const utilxx_base::AsioErrorCode&) {});
        io->run();
    }
    io->run();
}

template<typename Fn>
bool runOnIo(const std::shared_ptr<asio::io_context>& io, Fn fn) {
    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};
    asio::co_spawn(
        *io,
        keepAliveRun([fn, &done, &ok]() mutable -> asio::awaitable<void> {
            ok   = co_await fn();
            done = true;
        }),
        asio::detached
    );
    pumpIoUntil(io, done);
    return ok.load();
}

asio::awaitable<void> sleepMs(int64_t ms) {
    asio::steady_timer timer(co_await asio::this_coro::executor, std::chrono::milliseconds{ms});
    AsioErrorCode      ec;
    co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
}

/// 轮询等待条件成立 (最多 timeoutMs)
asio::awaitable<bool> waitUntil(std::function<bool()> cond, int64_t timeoutMs = 8000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMs};
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            co_return true;
        }
        co_await sleepMs(10);
    }
    co_return cond();
}

bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

/// 假驱动端: 记录投递到会话的输入 (notify 的投递处)
class AtFakeSink : public agentxx::agent::SessionInputSink {
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

/// 测试工具: 睡一段再返回固定文本 (可返回失败), 并记下收到的参数
class AtSleepTool final : public agentxx::tools::XXToolBase {
public:

    explicit AtSleepTool(std::weak_ptr<agentxx::agent::AgentContext> in_ctx) :
        XXToolBase("at_test_sleep", std::move(in_ctx), false, false, 0, false, false) {}

    neograph::ChatTool get_definition() const override {
        // 参数 schema 如实声明类型: 后台任务执行前会按它修正模型给的宽松参数
        // (见 [test_async_tool_args_normalize])
        return {
            get_name(),
            "Sleep, then answer with a fixed text.",
            neograph::json::parse(
                R"({"type":"object","properties":{"sleepMs":{"type":"integer"},"marker":{"type":"string"},"fail":{"type":"boolean"}}})"
            )
        };
    }

    asio::awaitable<agentxx::util::ToolResult>
        execute_async(const utilxx_base::Json& arguments) override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            lastArgs = arguments;
            ++calls;
        }
        const auto sleepMsArg
            = arguments.is_object() ? arguments.value("sleepMs", int64_t{0}) : int64_t{0};
        if (sleepMsArg > 0) {
            co_await sleepMs(sleepMsArg);
        }
        // 挂起之后再读一次参数: 后台协程可能在本工具的发起方 (async_work 的
        // runToolWork) 返回后才恢复执行, 参数引用必须仍然有效 ——
        // 见 [test_tool_closure_lifetime]
        {
            std::lock_guard<std::mutex> lock(mutex);
            markerAfterSleep = arguments.is_object()
                                   ? arguments.value("marker", std::string{})
                                   : std::string{};
            finished = true;
        }
        if (arguments.is_object() && arguments.value("fail", false)) {
            co_return agentxx::util::ToolResult::fail("tool said no");
        }
        co_return agentxx::util::ToolResult::ok("tool says hi");
    }

    int callCount() {
        std::lock_guard<std::mutex> lock(mutex);
        return calls;
    }

    bool finishedOk() {
        std::lock_guard<std::mutex> lock(mutex);
        return finished;
    }

    std::string markerAfterSleepText() {
        std::lock_guard<std::mutex> lock(mutex);
        return markerAfterSleep;
    }

    utilxx_base::Json lastArgs = utilxx_base::Json::object();
    int               calls    = 0;
    std::mutex        mutex;

private:

    std::string markerAfterSleep;
    bool        finished = false;
};

/// 夹具: 模拟 LLM + AgentHost + 一个根 CodeAgent + 一个真实会话 (含驱动端)
struct AtFixture {
    DaSimServer                                  sim;
    std::shared_ptr<asio::io_context>            io;
    std::shared_ptr<agentxx::agent::AgentHost>   host;
    std::shared_ptr<agentxx::agent::CodeAgent>   agent;
    std::shared_ptr<agentxx::agent::AgentConfig> cfg;
    std::shared_ptr<AtFakeSink>                  sink;
    std::shared_ptr<AtSleepTool>                 sleepTool;
    std::string                                  sessionId = "at-session";

    ~AtFixture() {
        sim.stop();
    }

    std::shared_ptr<agentxx::agent::AgentContext> ctx() const {
        return agent ? agent->getContext() : nullptr;
    }
};

bool buildFixture(const std::shared_ptr<AtFixture>& fx) {
    fx->sim = startDaSimServer();
    g_da_sim_response_content     = "async subagent result";
    g_da_sim_tool_calls           = Json::array();
    g_da_sim_tool_calls_remaining = -1;
    g_da_sim_delay_ms             = 0;
    g_da_sim_fail_count           = 0;
    g_da_sim_request_count        = 0;

    fx->io                   = std::make_shared<asio::io_context>();
    fx->cfg                  = std::make_shared<agentxx::agent::AgentConfig>();
    fx->cfg->model.baseUrl   = "http://127.0.0.1:" + std::to_string(fx->sim.port);
    fx->cfg->model.apiKey    = "EMPTY";
    fx->cfg->model.modelName = "test-sim";
    fx->cfg->prompt.systemPrompt = "You are a helpful assistant.";
    fx->cfg->permissionMode      = agentxx::agent::PermissionMode::Pass;
    fx->cfg->llmMaxRetry         = 1;
    fx->cfg->enableAsyncWork     = true;

    auto              io = fx->io;
    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};
    asio::co_spawn(
        *io,
        keepAliveRun([fx, &done, &ok]() -> asio::awaitable<void> {
            agentxx::agent::AgentHost::Config hostCfg;
            hostCfg.ioCtx = fx->io;
            fx->host      = agentxx::agent::AgentHost::create(hostCfg);
            fx->agent     = std::make_shared<agentxx::agent::CodeAgent>(fx->cfg);
            co_await fx->agent->init();
            fx->host->attachRoot(fx->agent);

            auto ctx = fx->agent->getContext();
            // 真实会话 + 假驱动端: notify 的投递落到 sink 里
            auto session       = ctx->sessions->getOrCreate(fx->sessionId);
            fx->sink           = std::make_shared<AtFakeSink>();
            session->inputSink = fx->sink;

            // 注册一个可后台执行的工具 (工具注册表里的工具才支持 kind=tool)
            fx->sleepTool = std::make_shared<AtSleepTool>(ctx);
            ok            = ctx->toolRegistry != nullptr
                 && ctx->toolRegistry->registerTool(fx->sleepTool->get_name(), fx->sleepTool);
            done = true;
        }),
        asio::detached
    );
    pumpIoUntil(io, done);
    return ok.load();
}

/// 执行工具并把异常转成带前缀的文本 (成功/失败都返回正文)
asio::awaitable<std::string>
    execTool(agentxx::tools::XXToolBase& tool, Json args) {
    try {
        auto result = co_await tool.execute_async(args);
        co_return std::move(result.text);
    } catch (const std::exception& e) {
        co_return fmt::format("[threw] {}", e.what());
    }
}

/// 从返回文本里取 `id=async-N`
std::string parseAsyncId(std::string_view text) {
    const auto pos = text.find("id=");
    if (pos == std::string_view::npos) {
        return {};
    }
    auto rest = text.substr(pos + 3);
    const auto end = rest.find_first_of(" \n");
    return std::string{rest.substr(0, end)};
}

} // namespace

/// 1. 开始一个工具任务: 默认 notify, 结果自动投回会话
asio::awaitable<void> test_start_tool_notify_default() {
    auto fx = std::make_shared<AtFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx));

    agentxx::tools::AsyncStartTool tool{fx->ctx()};
    std::string                     text;
    std::string                     asyncId;
    std::string                     ctxSessionId;
    std::string                     toolGotSessionId;

    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        text = co_await execTool(
            tool,
            Json{
                {"tool",      "at_test_sleep"      },
                {"args",      Json{{"sleepMs", 20}}},
                {"sessionId", fx->sessionId        },
            }
        );
        asyncId = parseAsyncId(text);
        // 等任务跑完 (结果由任务自己发事件)
        co_return co_await waitUntil([&] {
            const auto entry = fx->host->asyncWorkJson(asyncId);
            return entry.value("state", std::string{}) == "done";
        });
    }));

    XX_TEST_EXPECT_TRUE(text.starts_with("[Async started] id="));
    XX_TEST_EXPECT_TRUE(contains(text, "tool=at_test_sleep"));
    XX_TEST_EXPECT_TRUE(contains(text, "inserted into this conversation"));
    XX_TEST_EXPECT_FALSE(asyncId.empty());

    // 台账: done + 结果正文
    const auto entry = fx->host->asyncWorkJson(asyncId);
    XX_TEST_EXPECT_EQ(entry.value("kind", std::string{}), std::string{"tool"});
    XX_TEST_EXPECT_EQ(entry.value("state", std::string{}), std::string{"done"});
    XX_TEST_EXPECT_EQ(entry.value("result", std::string{}), std::string{"tool says hi"});
    // 执行层补了 sessionId (下游工具会读)
    {
        std::lock_guard<std::mutex> lock(fx->sleepTool->mutex);
        toolGotSessionId = fx->sleepTool->lastArgs.value("sessionId", std::string{});
    }
    XX_TEST_EXPECT_EQ(toolGotSessionId, fx->sessionId);

    // notify: 结果作为一条输入落到了会话
    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        co_return co_await waitUntil([&] { return !fx->sink->texts.empty(); });
    }));
    bool delivered = false;
    for (const auto& item : fx->sink->texts) {
        if (contains(item, "tool says hi")) {
            delivered = true;
        }
    }
    XX_TEST_EXPECT_TRUE(delivered);
    co_return;
}

/// 1b. 后台工具任务也按工具 schema 修正参数 (与前台 ToolcallWrapNode 同一套规则)
asio::awaitable<void> test_async_tool_args_normalize() {
    auto fx = std::make_shared<AtFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx));

    agentxx::tools::AsyncStartTool tool{fx->ctx()};
    std::string                     text;
    std::string                     asyncId;

    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        text = co_await execTool(
            tool,
            Json{
                {"tool", "at_test_sleep"},
                // 模型常见的宽松写法: 整数写字符串、字符串写数字、布尔写 "no"
                {"args",
                 Json{
                     {"sleepMs", "5"},
                     {"marker", 123},
                     {"fail", "no"},
                 }},
                {"sessionId", fx->sessionId},
            }
        );
        asyncId = parseAsyncId(text);
        co_return co_await waitUntil([&] {
            const auto entry = fx->host->asyncWorkJson(asyncId);
            return entry.value("state", std::string{}) == "done";
        });
    }));

    XX_TEST_EXPECT_FALSE(asyncId.empty());
    // 工具收到的参数已按 schema 声明修正 (不修正的话工具会把 sleepMs 当 0,
    // marker 读不到字符串, fail 也读不到布尔)
    std::lock_guard<std::mutex> lock(fx->sleepTool->mutex);
    XX_TEST_EXPECT_TRUE(fx->sleepTool->lastArgs["sleepMs"].is_number_integer());
    XX_TEST_EXPECT_EQ(fx->sleepTool->lastArgs["sleepMs"].get<long long>(), 5LL);
    XX_TEST_EXPECT_TRUE(fx->sleepTool->lastArgs["marker"].is_string());
    XX_TEST_EXPECT_EQ(
        fx->sleepTool->lastArgs["marker"].get<std::string>(),
        std::string{"123"}
    );
    XX_TEST_EXPECT_TRUE(fx->sleepTool->lastArgs["fail"].is_boolean());
    XX_TEST_EXPECT_EQ(fx->sleepTool->lastArgs["fail"].get<bool>(), false);
    co_return;
}

/// 2. notify=false: 只落台账, 不打扰会话
asio::awaitable<void> test_start_notify_off() {
    auto fx = std::make_shared<AtFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx));

    agentxx::tools::AsyncStartTool tool{fx->ctx()};
    std::string                     text;
    std::string                     asyncId;

    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        text = co_await execTool(
            tool,
            Json{
                {"tool",      "at_test_sleep"        },
                {"args",      Json{{"sleepMs", 10}}  },
                {"notify",    false                  },
                {"sessionId", fx->sessionId          },
            }
        );
        asyncId = parseAsyncId(text);
        co_return co_await waitUntil([&] {
            return fx->host->asyncWorkJson(asyncId).value("state", std::string{}) == "done";
        });
    }));
    XX_TEST_EXPECT_TRUE(text.starts_with("[Async started] id="));
    XX_TEST_EXPECT_TRUE(contains(text, "No automatic notification"));
    // 给中枢一点时间: 若误登记了等待, 会在这里投递
    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        co_await sleepMs(60);
        co_return true;
    }));
    XX_TEST_EXPECT_TRUE(fx->sink->texts.empty());
    XX_TEST_EXPECT_EQ(
        fx->host->asyncWorkJson(asyncId).value("result", std::string{}),
        std::string{"tool says hi"}
    );
    co_return;
}

/// 3. 子代理目标: 参数与直接调用一致, 结果同样回到会话
asio::awaitable<void> test_start_subagent_target() {
    auto fx = std::make_shared<AtFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx));

    agentxx::tools::AsyncStartTool tool{fx->ctx()};
    std::string                     text;
    std::string                     asyncId;

    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        text = co_await execTool(
            tool,
            Json{
                {"tool",      "agentxx_subagent"                              },
                {"args",
                 Json{{"subagent", "subagent_task"}, {"message", "do it"}}},
                {"label",     "my worker"                                  },
                {"sessionId", fx->sessionId                                },
            }
        );
        asyncId = parseAsyncId(text);
        co_return co_await waitUntil([&] {
            const auto entry = fx->host->asyncWorkJson(asyncId);
            const auto state = entry.value("state", std::string{});
            return state == "done" || state == "failed";
        }, 30000);
    }));

    XX_TEST_EXPECT_TRUE(text.starts_with("[Async started] id="));
    XX_TEST_EXPECT_TRUE(contains(text, "subagent=subagent_task"));
    const auto entry = fx->host->asyncWorkJson(asyncId);
    XX_TEST_EXPECT_EQ(entry.value("kind", std::string{}), std::string{"agent"});
    XX_TEST_EXPECT_EQ(entry.value("name", std::string{}), std::string{"my worker"});
    XX_TEST_EXPECT_EQ(entry.value("state", std::string{}), std::string{"done"});
    XX_TEST_EXPECT_TRUE(
        contains(entry.value("result", std::string{}), "async subagent result")
    );
    co_return;
}

/// 4. 参数与目标校验
asio::awaitable<void> test_start_errors() {
    auto fx = std::make_shared<AtFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx));

    agentxx::tools::AsyncStartTool tool{fx->ctx()};
    std::vector<std::string>        texts;

    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        texts.push_back(co_await execTool(tool, Json{{"sessionId", fx->sessionId}}));
        texts.push_back(co_await execTool(
            tool,
            Json{{"tool", "at_test_sleep"}, {"args", "nope"}, {"sessionId", fx->sessionId}}
        ));
        // 不允许把后台任务工具本身套进去
        texts.push_back(co_await execTool(
            tool,
            Json{{"tool", "agentxx_async_start"}, {"sessionId", fx->sessionId}}
        ));
        // 工具不在注册表里 (含 lib 内置工具)
        texts.push_back(co_await execTool(
            tool,
            Json{{"tool", "agentxx_share_store"}, {"sessionId", fx->sessionId}}
        ));
        // 子代理缺 subagent / 缺 message
        texts.push_back(co_await execTool(
            tool,
            Json{{"tool", "agentxx_subagent"}, {"args", Json{{"message", "x"}}}, {"sessionId", fx->sessionId}}
        ));
        texts.push_back(co_await execTool(
            tool,
            Json{{"tool", "agentxx_subagent"}, {"args", Json{{"subagent", "subagent_task"}}}, {"sessionId", fx->sessionId}}
        ));
        co_return true;
    }));

    XX_TEST_EXPECT_EQ(texts.size(), size_t{6});
    XX_TEST_EXPECT_TRUE(texts[0].starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(texts[0], "tool"));
    XX_TEST_EXPECT_TRUE(texts[1].starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(texts[1], "args"));
    XX_TEST_EXPECT_TRUE(texts[2].starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(texts[2], "background-task tool"));
    XX_TEST_EXPECT_TRUE(texts[3].starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(texts[3], "not in the tool registry"));
    XX_TEST_EXPECT_TRUE(texts[4].starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(texts[4], "subagent"));
    XX_TEST_EXPECT_TRUE(texts[5].starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(texts[5], "message"));
    co_return;
}

/// 5. 查询: group 六档 / id 详情 / 归属隔离
asio::awaitable<void> test_query() {
    auto fx = std::make_shared<AtFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx));

    agentxx::tools::AsyncStartTool  start{fx->ctx()};
    agentxx::tools::AsyncQueryTool  query{fx->ctx()};
    std::string                     fastId;
    std::string                     slowId;
    std::string                     otherId;
    std::string                     listAll;
    std::string                     listRunning;
    std::string                     listDone;
    std::string                     detail;
    std::string                     notFound = "[threw] ";
    std::string                     foreign;

    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        auto fast = co_await execTool(
            start,
            Json{
                {"tool",        "at_test_sleep"    },
                {"args",        Json{{"sleepMs", 5}}},
                {"notify",      false              },
                {"sessionId",   fx->sessionId      },
            }
        );
        fastId = parseAsyncId(fast);
        auto slow = co_await execTool(
            start,
            Json{
                {"tool",        "at_test_sleep"      },
                {"args",        Json{{"sleepMs", 900}}},
                {"notify",      false                },
                {"sessionId",   fx->sessionId        },
            }
        );
        slowId = parseAsyncId(slow);
        // 别的会话启动的任务 (直接走容器, 验证归属隔离)
        agentxx::agent::AsyncWorkSpec spec;
        spec.kind             = agentxx::agent::AsyncWorkKind::Tool;
        spec.toolName         = "at_test_sleep";
        spec.toolArgs         = Json{{"sleepMs", 5}};
        spec.creatorSessionId = "other-session";
        spec.baseConfig       = fx->cfg;
        otherId               = fx->host->submitAsyncWork(std::move(spec), fx->ctx()).asyncId;

        // 等 fast 结束 (slow 仍在跑)
        co_return co_await waitUntil([&] {
            return fx->host->asyncWorkJson(fastId).value("state", std::string{}) == "done";
        });
    }));

    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        listAll = co_await execTool(query, Json{{"sessionId", fx->sessionId}});
        listRunning
            = co_await execTool(query, Json{{"group", "running"}, {"sessionId", fx->sessionId}});
        listDone = co_await execTool(query, Json{{"group", "done"}, {"sessionId", fx->sessionId}});
        detail   = co_await execTool(
            query,
            Json{{"id", slowId}, {"sessionId", fx->sessionId}}
        );
        try {
            (void)co_await query.execute_async(
                Json{{"group", "bogus"}, {"sessionId", fx->sessionId}}
            );
        } catch (const std::exception& e) {
            notFound = fmt::format("[threw] {}", e.what());
        }
        foreign = co_await execTool(query, Json{{"id", otherId}, {"sessionId", fx->sessionId}});
        co_return true;
    }));

    XX_TEST_EXPECT_TRUE(listAll.starts_with("[Async tasks] group=all"));
    XX_TEST_EXPECT_TRUE(contains(listAll, fastId));
    XX_TEST_EXPECT_TRUE(contains(listAll, slowId));
    // 别的会话的任务不出现在本会话清单里
    XX_TEST_EXPECT_FALSE(contains(listAll, otherId));
    XX_TEST_EXPECT_TRUE(listRunning.starts_with("[Async tasks] group=running"));
    XX_TEST_EXPECT_TRUE(contains(listRunning, slowId));
    XX_TEST_EXPECT_FALSE(contains(listRunning, fastId));
    XX_TEST_EXPECT_TRUE(listDone.starts_with("[Async tasks] group=done"));
    XX_TEST_EXPECT_TRUE(contains(listDone, fastId));
    XX_TEST_EXPECT_FALSE(contains(listDone, slowId));
    XX_TEST_EXPECT_TRUE(detail.starts_with("[Async task] id=" + slowId));
    XX_TEST_EXPECT_TRUE(contains(detail, "state=running"));
    XX_TEST_EXPECT_TRUE(contains(notFound, "group"));
    XX_TEST_EXPECT_TRUE(foreign.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(foreign, "not started by this session"));

    // 等 slow 结束: none 分组为空
    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        co_return co_await waitUntil([&] {
            return fx->host->asyncWorkJson(slowId).value("state", std::string{}) == "done";
        });
    }));
    std::string listNone;
    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        listNone
            = co_await execTool(query, Json{{"group", "canceled"}, {"sessionId", fx->sessionId}});
        co_return true;
    }));
    XX_TEST_EXPECT_TRUE(contains(listNone, "matched=0"));
    co_return;
}

/// 6. 阻塞等待: 已完成立即返回 / 运行中等到结果 / 超时给说明
asio::awaitable<void> test_await() {
    auto fx = std::make_shared<AtFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx));

    agentxx::tools::AsyncStartTool start{fx->ctx()};
    agentxx::tools::AsyncAwaitTool awaitTool{fx->ctx()};
    std::string                     finishedId;
    std::string                     runningId;
    std::string                     doneText;
    std::string                     runningText;
    std::string                     timeoutText;

    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        auto a = co_await execTool(
            start,
            Json{{"tool", "at_test_sleep"}, {"args", Json{{"sleepMs", 5}}}, {"notify", false}, {"sessionId", fx->sessionId}}
        );
        finishedId = parseAsyncId(a);
        auto b = co_await execTool(
            start,
            Json{{"tool", "at_test_sleep"}, {"args", Json{{"sleepMs", 200}}}, {"notify", false}, {"sessionId", fx->sessionId}}
        );
        runningId = parseAsyncId(b);
        co_return co_await waitUntil([&] {
            return fx->host->asyncWorkJson(finishedId).value("state", std::string{}) == "done";
        });
    }));

    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        // 已结束: 直接给台账结果, 不再挂等待
        doneText = co_await execTool(
            awaitTool,
            Json{{"id", finishedId}, {"sessionId", fx->sessionId}}
        );
        // 运行中: 等到结果 (200ms)
        runningText = co_await execTool(
            awaitTool,
            Json{{"id", runningId}, {"timeout_ms", 5000}, {"sessionId", fx->sessionId}}
        );
        co_return true;
    }));

    XX_TEST_EXPECT_TRUE(doneText.starts_with("[Async task] id=" + finishedId));
    XX_TEST_EXPECT_TRUE(contains(doneText, "state=done"));
    XX_TEST_EXPECT_TRUE(contains(doneText, "tool says hi"));
    XX_TEST_EXPECT_TRUE(contains(runningText, "[Event] agentxx.agent.async.done"));
    XX_TEST_EXPECT_TRUE(contains(runningText, "tool says hi"));

    // 超时: 启动一个慢任务, 只等 100ms
    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        auto c = co_await execTool(
            start,
            Json{{"tool", "at_test_sleep"}, {"args", Json{{"sleepMs", 1500}}}, {"notify", false}, {"sessionId", fx->sessionId}}
        );
        const auto slowId = parseAsyncId(c);
        timeoutText       = co_await execTool(
            awaitTool,
            Json{{"id", slowId}, {"timeout_ms", 100}, {"sessionId", fx->sessionId}}
        );
        fx->host->cancelAsyncWork(slowId);
        co_return true;
    }));
    XX_TEST_EXPECT_TRUE(contains(timeoutText, "[Event await timeout]"));
    XX_TEST_EXPECT_TRUE(contains(timeoutText, "waited=0s"));
    co_return;
}

/// 7. 取消: id / group / 终态 / 归属
asio::awaitable<void> test_cancel() {
    auto fx = std::make_shared<AtFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx));

    agentxx::tools::AsyncStartTool  start{fx->ctx()};
    agentxx::tools::AsyncCancelTool cancel{fx->ctx()};
    std::string                     idText;
    std::string                     groupText;
    std::string                     finished;
    std::string                     terminalGroup;
    std::string                     otherId;
    std::string                     foreign;

    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        auto a = co_await execTool(
            start,
            Json{{"tool", "at_test_sleep"}, {"args", Json{{"sleepMs", 3000}}}, {"notify", false}, {"sessionId", fx->sessionId}}
        );
        const auto slowId = parseAsyncId(a);
        idText            = co_await execTool(
            cancel,
            Json{{"id", slowId}, {"sessionId", fx->sessionId}}
        );
        auto ok = co_await waitUntil([&] {
            return fx->host->asyncWorkJson(slowId).value("state", std::string{}) == "canceled";
        });
        if (!ok) {
            co_return false;
        }

        // 批量: 两条慢任务, group=running 全取消
        for (int i = 0; i < 2; ++i) {
            (void)co_await execTool(
                start,
                Json{{"tool", "at_test_sleep"}, {"args", Json{{"sleepMs", 3000}}}, {"notify", false}, {"sessionId", fx->sessionId}}
            );
        }
        co_await waitUntil([&] { return fx->host->runningAsyncWork() >= 2; });
        groupText = co_await execTool(
            cancel,
            Json{{"group", "running"}, {"sessionId", fx->sessionId}}
        );
        co_return co_await waitUntil([&] { return fx->host->runningAsyncWork() == 0; });
    }));

    XX_TEST_EXPECT_TRUE(idText.starts_with("[Async cancel] id="));
    XX_TEST_EXPECT_TRUE(contains(idText, "canceled"));
    XX_TEST_EXPECT_TRUE(groupText.starts_with("[Async cancel] canceled 2/2 running"));

    // 已结束的不能再取消; 终态分组按参数错误拒
    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        auto a = co_await execTool(
            start,
            Json{{"tool", "at_test_sleep"}, {"args", Json{{"sleepMs", 5}}}, {"notify", false}, {"sessionId", fx->sessionId}}
        );
        const auto fastId = parseAsyncId(a);
        co_await waitUntil([&] {
            return fx->host->asyncWorkJson(fastId).value("state", std::string{}) == "done";
        });
        finished = co_await execTool(cancel, Json{{"id", fastId}, {"sessionId", fx->sessionId}});
        terminalGroup = co_await execTool(
            cancel,
            Json{{"group", "done"}, {"sessionId", fx->sessionId}}
        );
        // 别的会话的任务不能取消
        agentxx::agent::AsyncWorkSpec spec;
        spec.kind             = agentxx::agent::AsyncWorkKind::Tool;
        spec.toolName         = "at_test_sleep";
        spec.toolArgs         = Json{{"sleepMs", 2000}};
        spec.creatorSessionId = "other-session";
        spec.baseConfig       = fx->cfg;
        otherId               = fx->host->submitAsyncWork(std::move(spec), fx->ctx()).asyncId;
        foreign = co_await execTool(cancel, Json{{"id", otherId}, {"sessionId", fx->sessionId}});
        fx->host->cancelAsyncWork(otherId);
        co_return true;
    }));

    XX_TEST_EXPECT_TRUE(finished.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(finished, "already finished"));
    XX_TEST_EXPECT_TRUE(terminalGroup.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(terminalGroup, "group"));
    XX_TEST_EXPECT_TRUE(foreign.starts_with("[threw] "));
    XX_TEST_EXPECT_TRUE(contains(foreign, "not started by this session"));
    co_return;
}

/// 8. 取消后台任务后工具仍在跑: 它捕获的参数必须仍然有效
/// - 工具执行体是 runToolWork 里 co_spawn 出去的后台协程, 取消时发起方先返回
///   (帧销毁), 工具继续跑到自己的超时
/// - 若 co_spawn 拿的是"内联调用的临时闭包"产生的 awaitable, 后台协程恢复后
///   访问闭包成员 (args/tool/state) 就是 use-after-free (实测 ASan 报 heap-use-after-free)
/// - 断言: 挂起之后再读参数仍能拿到原值 (marker 用长字符串, 让 Json 内部有堆分配,
///   悬垂读取才有实际后果)
asio::awaitable<void> test_tool_closure_lifetime() {
    auto fx = std::make_shared<AtFixture>();
    XX_TEST_EXPECT_TRUE(buildFixture(fx));

    agentxx::tools::AsyncStartTool start{fx->ctx()};
    const std::string             marker = "marker-" + std::string(512, 'x');
    std::string                   asyncId;

    XX_TEST_EXPECT_TRUE(runOnIo(fx->io, [&]() -> asio::awaitable<bool> {
        auto text = co_await execTool(
            start,
            Json{
                {"tool",      "at_test_sleep"                           },
                {"args",      Json{{"sleepMs", 400}, {"marker", marker}}},
                {"notify",    false                                    },
                {"sessionId", fx->sessionId                             },
            }
        );
        asyncId = parseAsyncId(text);
        if (asyncId.empty()) {
            co_return false;
        }
        // 等工具进入执行体 (此时它停在睡眠里) 再取消: 发起方 runToolWork 随即返回,
        // 工具协程稍后才会恢复
        if (!co_await waitUntil([&] { return fx->sleepTool->callCount() >= 1; })) {
            co_return false;
        }
        fx->host->cancelAsyncWork(asyncId);
        co_return co_await waitUntil([&] { return fx->sleepTool->finishedOk(); });
    }));

    XX_TEST_EXPECT_TRUE(fx->sleepTool->finishedOk());
    XX_TEST_EXPECT_EQ(fx->sleepTool->markerAfterSleepText(), marker);
    co_return;
}

asio::awaitable<TestResult> run_async_tools_tests() {
    TestResult result;

    co_await test_start_tool_notify_default();
    co_await test_async_tool_args_normalize();
    co_await test_start_notify_off();
    co_await test_start_subagent_target();
    co_await test_start_errors();
    co_await test_query();
    co_await test_await();
    co_await test_cancel();
    co_await test_tool_closure_lifetime();

    result.passed = g_at_passed;
    result.failed = g_at_failed;
    co_return result;
}

} // namespace test
} // namespace agentxx
