#include "test_subagent_bus.h"
#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/event/events.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/use_awaitable.hpp"
#include <atomic>
#include <iostream>
#include <memory>
#include <string>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_sb_passed = 0;
int g_sb_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_sb_passed
#define XX_TEST_FAILED g_sb_failed
namespace agentxx {
namespace test {

/// 验证: bus.request<ReqSubagentBatch, RespSubagentBatch> 请求-响应闭环 (统一批量)
/// - 注册一个模拟 server, 验证请求参数传递与响应回填
asio::awaitable<void> test_subagent_bus_request_response() {
    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    agentContext->bus
        = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);

    // 注册模拟 server
    auto& rr = agentContext->bus->getRR<events::ReqSubagentBatch, events::RespSubagentBatch>(
        events::Topic::Subagent
    );
    rr.registerServer(
        [](const events::ReqSubagentBatch& req,
           size_t                          corrId) -> asio::awaitable<events::RespSubagentBatch> {
            XX_TEST_EXPECT_TRUE(corrId > 0);
            XX_TEST_EXPECT_EQ(req.tasks.size(), size_t{1});
            XX_TEST_EXPECT_EQ(req.tasks[0].subagentName, std::string{"research"});
            XX_TEST_EXPECT_EQ(req.tasks[0].message, std::string{"find foo"});
            XX_TEST_EXPECT_EQ(req.tasks[0].resultId, std::string{"call_1"});
            co_return events::RespSubagentBatch{
                .results = {
                    events::RespSubagentBatchItem{
                        .resultId = "call_1",
                        .content  = fmt::format("result_for_{}", req.tasks[0].subagentName),
                    },
                },
            };
        }
    );

    // [workaround] 聚合提取为具名变量, 绕过 g++ 16.1 ICE (gimplify.cc:841)
    events::ReqSubagentBatch req{
        .parentAgentName = "parent",
        .parentSessionId  = "t1",
        .cancelToken     = nullptr,
        .tasks           = {
            events::SubagentBatchItem{
                .subagentName = "research",
                .systemPrompt = "",
                .message      = "find foo",
                .resultId     = "call_1",
            },
        },
    };
    auto resp
        = co_await agentContext->bus->request<events::ReqSubagentBatch, events::RespSubagentBatch>(
            events::Topic::Subagent,
            req,
            std::chrono::seconds(5)
        );

    XX_TEST_EXPECT_TRUE(resp.has_value());
    if (resp.has_value()) {
        XX_TEST_EXPECT_EQ(resp->results.size(), size_t{1});
        XX_TEST_EXPECT_EQ(resp->results[0].content, std::string{"result_for_research"});
        XX_TEST_EXPECT_TRUE(!resp->results[0].hasError);
    }

    co_return;
}

/// 验证: SubagentProgress 事件发布与订阅
asio::awaitable<void> test_subagent_progress_events() {
    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    agentContext->bus
        = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);

    std::atomic<int> tokenCount{0};
    std::string      lastToken;
    std::string      lastSubagentId;

    agentContext->bus->get<events::EventSubagentProgress>(events::Topic::SubagentProgress)
        .subscribe([&](const events::EventSubagentProgress& e) -> asio::awaitable<void> {
            if (e.kind == "token") {
                tokenCount++;
                lastToken      = e.data;
                lastSubagentId = e.subagentId;
            }
            co_return;
        });

    // 发布几个 token 进度事件
    co_await agentContext->bus->publish<events::EventSubagentProgress>(
        events::Topic::SubagentProgress,
        events::EventSubagentProgress{
            .subagentId = "subagent_research",
            .agentName  = "research",
            .kind       = "token",
            .data       = "Hello",
        }
    );
    co_await agentContext->bus->publish<events::EventSubagentProgress>(
        events::Topic::SubagentProgress,
        events::EventSubagentProgress{
            .subagentId = "subagent_research",
            .agentName  = "research",
            .kind       = "token",
            .data       = " World",
        }
    );

    XX_TEST_EXPECT_EQ(tokenCount.load(), 2);
    XX_TEST_EXPECT_EQ(lastToken, std::string{" World"});
    XX_TEST_EXPECT_EQ(lastSubagentId, std::string{"subagent_research"});

    co_return;
}

/// 验证: subagent 总线超时返回 nullopt
asio::awaitable<void> test_subagent_bus_timeout() {
    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    agentContext->bus
        = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);

    // 注册一个永不响应的 server
    auto& rr = agentContext->bus->getRR<events::ReqSubagentBatch, events::RespSubagentBatch>(
        events::Topic::Subagent
    );
    rr.registerServer(
        [](const events::ReqSubagentBatch&, size_t) -> asio::awaitable<events::RespSubagentBatch> {
            auto timer
                = asio::steady_timer(co_await asio::this_coro::executor, std::chrono::seconds(1));
            co_await timer.async_wait(asio::use_awaitable);
            co_return events::RespSubagentBatch{
                .results = {events::RespSubagentBatchItem{.content = "too late"}},
            };
        }
    );

    // [workaround] 聚合提取为具名变量, 绕过 g++ 16.1 ICE (gimplify.cc:841)
    events::ReqSubagentBatch req{
        .parentAgentName = "p",
        .parentSessionId  = "t",
        .cancelToken     = nullptr,
        .tasks           = {
            events::SubagentBatchItem{
                .subagentName = "x",
                .systemPrompt = "",
                .message      = "m",
                .resultId     = "r",
            },
        },
    };
    auto resp
        = co_await agentContext->bus->request<events::ReqSubagentBatch, events::RespSubagentBatch>(
            events::Topic::Subagent,
            req,
            std::chrono::milliseconds(200)
        );

    XX_TEST_EXPECT_TRUE(!resp.has_value());

    co_return;
}

/// 验证: MiddlewareContext::cleanupSession 清理一次性会话 (subagent) 的
/// graphData / shareStore / 中间件 states, 防止按 thread 累积泄漏 (P0 修复)
asio::awaitable<void> test_middleware_cleanup_thread() {
    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    agentContext->middlewareHandleContext
        = std::make_shared<agentxx::middleware::MiddlewareContext>();

    const std::string tid = "subagent_research_parent_1";

    // 写入三类 per-thread 状态
    agentContext->middlewareHandleContext
        ->setGraphDataItemValue<std::string>(tid, "xx_key", "value");
    agentContext->middlewareHandleContext->addShareStoreItemValue(tid, "big content");
    auto handle = std::make_shared<
        agentxx::middleware::MiddlewareWrapHandle<agentxx::middleware::BaseMiddlewareState>>(
        "TestHandle",
        agentContext
    );
    handle->states[tid] = std::make_shared<agentxx::middleware::BaseMiddlewareState>();
    agentContext->middlewareHandleContext->handles.push_back(handle);

    XX_TEST_EXPECT_TRUE(agentContext->middlewareHandleContext->graphData.contains(tid));
    XX_TEST_EXPECT_TRUE(agentContext->middlewareHandleContext->shareStore.contains(tid));
    XX_TEST_EXPECT_TRUE(handle->states.contains(tid));

    // 清理后全部移除
    agentContext->middlewareHandleContext->cleanupSession(tid);
    XX_TEST_EXPECT_FALSE(agentContext->middlewareHandleContext->graphData.contains(tid));
    XX_TEST_EXPECT_FALSE(agentContext->middlewareHandleContext->shareStore.contains(tid));
    XX_TEST_EXPECT_FALSE(handle->states.contains(tid));

    co_return;
}

/// 验证: 子代理会话结束后的 Session 移除语义 (SessionStore::remove)
asio::awaitable<void> test_subagent_session_remove() {
    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();

    const std::string tid     = "subagent_research_parent_1";
    auto              session = agentContext->getSession(tid);
    XX_TEST_EXPECT_TRUE(agentContext->sessions->get(tid) == session);

    agentContext->sessions->remove(tid);
    XX_TEST_EXPECT_TRUE(agentContext->sessions->get(tid) == nullptr);

    co_return;
}

asio::awaitable<TestResult> run_subagent_bus_tests() {
    g_sb_passed = 0;
    g_sb_failed = 0;
    try {
        co_await test_subagent_bus_request_response();
        co_await test_subagent_progress_events();
        co_await test_subagent_bus_timeout();
        co_await test_middleware_cleanup_thread();
        co_await test_subagent_session_remove();
    } catch (const std::exception& e) {
        TEST_FAIL << "subagent_bus suite exception: " << e.what() << std::endl;
        g_sb_failed++;
    }
    co_return TestResult{g_sb_passed, g_sb_failed};
}

} // namespace test
} // namespace agentxx
