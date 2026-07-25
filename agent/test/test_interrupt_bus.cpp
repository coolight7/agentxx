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

/// 确定性 Mock IO: handleInterrupt 不依赖 stdin, 返回可控结果, 供总线往返测试
class MockIO : public agentxx::agent::AgentIOBase {
public:

    std::string interruptTag    = "answered";
    bool        permissionAllow = true;
    int         interruptCalls  = 0;

    void onDelta(const agentxx::agent::Delta&) override {}

    void onSync(const agentxx::agent::SyncPayload&) override {}

    asio::awaitable<std::optional<std::string>> getInput() override {
        co_return std::nullopt;
    }

    asio::awaitable<neograph::json> handleInterrupt(
        const std::string& /*threadId*/,
        const std::string& interruptNode,
        const std::string& /*interruptValue*/,
        const std::string& /*interruptArgJson*/
    ) override {
        ++interruptCalls;
        if (interruptNode == "permission") {
            co_return neograph::json::array({permissionAllow ? "true" : "false"});
        }
        co_return neograph::json::array({interruptTag});
    }
};

/// 中断总线往返: MockIO 注册后, request 应确定性拿到结果 (不依赖 stdin/不超时)
asio::awaitable<void> test_interrupt_bus_request_response() {
    auto sessionBus
        = std::make_shared<agentxx::middleware::EventBus>(co_await asio::this_coro::executor);

    auto io  = std::make_shared<MockIO>();
    io->interruptTag = "answered";
    io->registerOnBus(sessionBus);

    auto resp = co_await sessionBus
                    ->request<agentxx::events::ReqInterrupt, agentxx::events::RespInterrupt>(
                        agentxx::events::Topic::Interrupt,
                        agentxx::events::ReqInterrupt{
                            .agentName         = "test",
                            .threadId          = "t1",
                            .interruptNode     = "tool_x",
                            .handleName        = "default",
                            .interruptArgsJson = "{}",
                            .resultId          = "call_1",
                        },
                        std::chrono::seconds(5)
                    );

    XX_TEST_EXPECT_TRUE(resp.has_value());
    if (resp.has_value()) {
        XX_TEST_EXPECT_TRUE(resp->handled);
        XX_TEST_EXPECT_EQ(resp->resultJson, "[\"answered\"]");
    }
    XX_TEST_EXPECT_EQ(io->interruptCalls, 1);

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

/// 权限总线往返: MockIO 注册后, request 应确定性拿到 Allow/Deny 决策
asio::awaitable<void> test_permission_bus_request_response() {
    auto sessionBus
        = std::make_shared<agentxx::middleware::EventBus>(co_await asio::this_coro::executor);

    auto io  = std::make_shared<MockIO>();
    io->permissionAllow = true;
    io->registerOnBus(sessionBus);

    auto reqAllow = agentxx::events::ReqPermission{
        .agentName     = "test",
        .threadId      = "t1",
        .toolName      = "filesystem_write",
        .category      = "filesystem_write",
        .target        = "/etc/passwd",
        .argumentsJson = R"({"path":"/etc/passwd"})",
    };

    auto resp = co_await sessionBus
                    ->request<agentxx::events::ReqPermission, agentxx::events::RespPermission>(
                        agentxx::events::Topic::Permission,
                        reqAllow,
                        std::chrono::seconds(5)
                    );
    XX_TEST_EXPECT_TRUE(resp.has_value());
    if (resp.has_value()) {
        XX_TEST_EXPECT_TRUE(resp->decision == agentxx::events::RespPermission::Decision::Allow);
    }

    // 切换为拒绝
    io->permissionAllow = false;
    auto respDeny       = co_await sessionBus
                    ->request<agentxx::events::ReqPermission, agentxx::events::RespPermission>(
                        agentxx::events::Topic::Permission,
                        reqAllow,
                        std::chrono::seconds(5)
                    );
    XX_TEST_EXPECT_TRUE(respDeny.has_value());
    if (respDeny.has_value()) {
        XX_TEST_EXPECT_TRUE(respDeny->decision == agentxx::events::RespPermission::Decision::Deny);
    }

    // 无 server 的总线应超时
    auto deadBus
        = std::make_shared<agentxx::middleware::EventBus>(co_await asio::this_coro::executor);
    auto resp2 = co_await deadBus
                     ->request<agentxx::events::ReqPermission, agentxx::events::RespPermission>(
                         agentxx::events::Topic::Permission,
                         reqAllow,
                         std::chrono::milliseconds(200)
                     );
    XX_TEST_EXPECT_TRUE(!resp2.has_value());

    co_return;
}

/// #4: 同一 IO 重复 registerOnBus 不应累积 handler (泄漏) 且最新 handler 生效
asio::awaitable<void> test_registerOnBus_no_accumulation() {
    auto sessionBus
        = std::make_shared<agentxx::middleware::EventBus>(co_await asio::this_coro::executor);

    auto& interruptRR
        = sessionBus->getRR<agentxx::events::ReqInterrupt, agentxx::events::RespInterrupt>(
            agentxx::events::Topic::Interrupt
        );
    auto& permRR
        = sessionBus->getRR<agentxx::events::ReqPermission, agentxx::events::RespPermission>(
            agentxx::events::Topic::Permission
        );

    auto io  = std::make_shared<MockIO>();
    io->interruptTag = "v1";

    // 模拟每个会话轮次都调用 registerOnBus (同一 IO 对象)
    io->registerOnBus(sessionBus);
    XX_TEST_EXPECT_EQ(interruptRR.serverCount(), 1u);
    XX_TEST_EXPECT_EQ(permRR.serverCount(), 1u);

    io->registerOnBus(sessionBus);
    io->registerOnBus(sessionBus);
    // 修复 #4: 重注册先移除旧 handler, server 数量保持为 1 (不累积/不泄漏)
    XX_TEST_EXPECT_EQ(interruptRR.serverCount(), 1u);
    XX_TEST_EXPECT_EQ(permRR.serverCount(), 1u);

    // 重注册后 handler 仍可用, 且反映 IO 当前状态 (最新)
    io->interruptTag = "v2";
    auto resp        = co_await sessionBus
                    ->request<agentxx::events::ReqInterrupt, agentxx::events::RespInterrupt>(
                        agentxx::events::Topic::Interrupt,
                        agentxx::events::ReqInterrupt{
                            .agentName         = "t",
                            .threadId          = "t",
                            .interruptNode     = "n",
                            .handleName        = "default",
                            .interruptArgsJson = "{}",
                            .resultId          = "r",
                        },
                        std::chrono::seconds(5)
                    );
    XX_TEST_EXPECT_TRUE(resp.has_value());
    if (resp.has_value()) {
        XX_TEST_EXPECT_EQ(resp->resultJson, "[\"v2\"]");
    }

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

    XX_TEST_EXPECT_TRUE(resp.has_value());
    if (resp.has_value()) {
        XX_TEST_EXPECT_TRUE(resp->handled);
        XX_TEST_EXPECT_TRUE(resp->resultJson == "\"custom_ok_myHandle\"");
    }

    co_return;
}

asio::awaitable<TestResult> run_interrupt_bus_tests() {
    g_ib_passed = 0;
    g_ib_failed = 0;
    try {
        co_await test_interrupt_bus_request_response();
        co_await test_permission_bus_request_response();
        co_await test_registerOnBus_no_accumulation();
        co_await test_interrupt_bus_custom_handler();
    } catch (const std::exception& e) {
        TEST_FAIL << "interrupt_bus suite exception: " << e.what() << std::endl;
        g_ib_failed++;
    }
    co_return TestResult{g_ib_passed, g_ib_failed};
}

} // namespace test
} // namespace agentxx
