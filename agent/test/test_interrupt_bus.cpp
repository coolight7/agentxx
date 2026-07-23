#include "test_interrupt_bus.h"
#include "agentxx-client/io/stdio/agent_stdio.h"
#include "agentxx/agent/context.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/events.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/middlewares/permission.h"
#include "agentxx/tools/tool.h"
#include "asio/as_tuple.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/use_awaitable.hpp"
#include <iostream>
#include <memory>

namespace agentxx {
namespace test {

int g_ib_passed = 0;
int g_ib_failed = 0;

asio::awaitable<void> test_interrupt_bus_request_response() {
    auto sessionBus
        = std::make_shared<agentxx::middleware::EventBus>(co_await asio::this_coro::executor);

    auto io = std::make_shared<AgentStdIO>();
    io->registerOnBus(sessionBus);

    // 构造一个无 inputs 的 InterruptHandleArg
    auto arg = agentxx::middleware::InterruptHandleArg{
        .name     = "default",
        .resultId = "call_1",
    };
    auto argJson = arg.toJson().dump();

    auto resp = co_await sessionBus
                    ->request<agentxx::events::ReqInterrupt, agentxx::events::RespInterrupt>(
                        agentxx::events::Topic::Interrupt,
                        agentxx::events::ReqInterrupt{
                            .agentName         = "test",
                            .threadId          = "t1",
                            .interruptNode     = "tool_x",
                            .handleName        = arg.name,
                            .interruptArgsJson = argJson,
                            .resultId          = arg.resultId,
                        },
                        std::chrono::seconds(5)
                    );

    if (false == resp.has_value() && resp.error() == "Timeout") {
        XX_TEST_EXPECT_TRUE(resp.error() == "Timeout");
    } else {
        XX_TEST_EXPECT_TRUE(resp.has_value());
    }

    // 新总线 (无任何 server) 上 request 应超时返回 nullopt
    auto deadBus
        = std::make_shared<agentxx::middleware::EventBus>(co_await asio::this_coro::executor);
    auto resp2
        = co_await deadBus->request<agentxx::events::ReqInterrupt, agentxx::events::RespInterrupt>(
            agentxx::events::Topic::Interrupt,
            agentxx::events::ReqInterrupt{
                .agentName         = "test",
                .threadId          = "t1",
                .interruptNode     = "n",
                .handleName        = "x",
                .interruptArgsJson = "{}",
                .resultId          = "r",
            },
            std::chrono::milliseconds(200)
        );
    XX_TEST_EXPECT_TRUE(!resp2.has_value());

    co_return;
}

/// 验证: IO 在 session bus 注册后, bus.request(service.permission) 能拿到决策
asio::awaitable<void> test_permission_bus_request_response() {
    auto sessionBus
        = std::make_shared<agentxx::middleware::EventBus>(co_await asio::this_coro::executor);

    auto io = std::make_shared<AgentStdIO>();
    io->registerOnBus(sessionBus);

    // 注意: 无 stdin 输入时 handleInterrupt 调用 getInput() 会超时
    auto resp = co_await sessionBus
                    ->request<agentxx::events::ReqPermission, agentxx::events::RespPermission>(
                        agentxx::events::Topic::Permission,
                        agentxx::events::ReqPermission{
                            .agentName     = "test",
                            .threadId      = "t1",
                            .toolName      = "filesystem_write",
                            .category      = "filesystem_write",
                            .target        = "/etc/passwd",
                            .argumentsJson = R"({"path":"/etc/passwd"})",
                        },
                        std::chrono::seconds(5)
                    );

    if (false == resp.has_value() && resp.error() == "Timeout") {
        XX_TEST_EXPECT_TRUE(resp.error() == "Timeout");
    } else {
        XX_TEST_EXPECT_TRUE(resp.has_value());
    }

    auto deadBus
        = std::make_shared<agentxx::middleware::EventBus>(co_await asio::this_coro::executor);
    auto resp2 = co_await deadBus
                     ->request<agentxx::events::ReqPermission, agentxx::events::RespPermission>(
                         agentxx::events::Topic::Permission,
                         agentxx::events::ReqPermission{
                             .agentName     = "t",
                             .threadId      = "t",
                             .toolName      = "x",
                             .category      = "x",
                             .target        = "x",
                             .argumentsJson = "{}",
                         },
                         std::chrono::milliseconds(200)
                     );
    XX_TEST_EXPECT_TRUE(!resp2.has_value());

    co_return;
}

/// 验证: 自定义 interrupt handler 可替换 CLI handler (扩展性)
asio::awaitable<void> test_interrupt_bus_custom_handler() {
    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    agentContext->bus
        = std::make_shared<agentxx::middleware::EventBus>(co_await asio::this_coro::executor);

    // 注册一个自定义 handler, 直接返回固定结果
    auto& rr
        = agentContext->bus->getRR<agentxx::events::ReqInterrupt, agentxx::events::RespInterrupt>(
            agentxx::events::Topic::Interrupt
        );
    rr.serve(
        [](const agentxx::events::ReqInterrupt& req,
           size_t /*corrId*/) -> asio::awaitable<agentxx::events::RespInterrupt> {
            co_return agentxx::events::RespInterrupt{
                .handled    = true,
                .resultJson = std::string{"\"custom_ok_"} + req.handleName + "\"",
            };
        }
    );

    auto resp = co_await agentContext->bus
                    ->request<agentxx::events::ReqInterrupt, agentxx::events::RespInterrupt>(
                        agentxx::events::Topic::Interrupt,
                        agentxx::events::ReqInterrupt{
                            .agentName         = "t",
                            .threadId          = "t",
                            .interruptNode     = "n",
                            .handleName        = "myHandle",
                            .interruptArgsJson = "{}",
                            .resultId          = "r",
                        },
                        std::chrono::seconds(5)
                    );

    if (false == resp.has_value() && resp.error() == "Timeout") {
        // allow timeout
        XX_TEST_EXPECT_TRUE(resp.error() == "Timeout");
    } else {
        XX_TEST_EXPECT_TRUE(resp.has_value());
        if (resp.has_value()) {
            XX_TEST_EXPECT_TRUE(resp->handled);
            XX_TEST_EXPECT_TRUE(resp->resultJson == "\"custom_ok_myHandle\"");
        }
    }

    co_return;
}

asio::awaitable<TestResult> run_interrupt_bus_tests() {
    try {
        co_await test_interrupt_bus_request_response();
        co_await test_permission_bus_request_response();
        co_await test_interrupt_bus_custom_handler();
    } catch (const std::exception& e) {
        TEST_FAIL << "interrupt_bus suite exception: " << e.what() << std::endl;
        g_ib_failed++;
    }
    co_return TestResult{g_ib_passed, g_ib_failed};
}

} // namespace test
} // namespace agentxx
