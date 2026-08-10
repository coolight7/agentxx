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
#include <filesystem>
#include <fmt/format.h>
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
        std::string_view /*threadId*/,
        std::string_view interruptNode,
        std::string_view /*interruptValue*/,
        std::string_view /*interruptArgJson*/
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

    auto io          = std::make_shared<MockIO>();
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

    auto io             = std::make_shared<MockIO>();
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

    auto io          = std::make_shared<MockIO>();
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
                .resultJson = fmt::format("\"custom_ok_{}\"", req.handleName),
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

/// 权限路由: 相对路径应基于当前工作目录转换为绝对路径后再匹配规则,
/// 使注册的绝对路径规则 (如 {cwd}/*) 也能命中相对路径访问
asio::awaitable<void> test_permission_relative_path() {
    /// 最小 Tool 实现: 仅提供名称
    class MockTool : public neograph::Tool {
    public:

        explicit MockTool(std::string name) :
            name_(std::move(name)) {}

        neograph::ChatTool get_definition() const override {
            return neograph::ChatTool{
                .name        = name_,
                .description = "",
                .parameters  = neograph::json::object(),
            };
        }

        std::string get_name() const override { return name_; }

        std::string execute(const neograph::json&) override { return ""; }

    private:

        std::string name_;
    };

    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    auto permission
        = std::make_shared<agentxx::middleware::PermissionMiddlewareHandle>(agentContext);

    // 注册绝对路径规则 (与 code_agent.cpp 的默认注册方式一致)
    auto cwd = std::filesystem::current_path().generic_string();
    permission->setFilesystemPermission(
        fmt::format("{}/*", cwd),
        agentxx::middleware::PermissionOperator::ALLOW,
        agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
    );
    // 最长前缀优先: cwd 下的 secret 子目录 DENY, 覆盖外层 ALLOW
    permission->setFilesystemPermission(
        fmt::format("{}/secret/*", cwd),
        agentxx::middleware::PermissionOperator::DENY,
        agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
    );
    permission->setFilesystemPermission(
        "/*",
        agentxx::middleware::PermissionOperator::INTERRUPT,
        agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
    );

    MockTool item("agentxx_filesystem_write_file");

    auto check = [&](std::string_view rel, std::string_view abs) -> asio::awaitable<void> {
        // 相对路径访问
        auto relArgs = neograph::json{{"path", std::string{rel}}};
        auto relOk   = co_await permission->defOnFilesystemHandle(
            item,
            relArgs,
            agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
        );
        // 对应绝对路径访问
        auto absArgs = neograph::json{{"path", std::string{abs}}};
        auto absOk   = co_await permission->defOnFilesystemHandle(
            item,
            absArgs,
            agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
        );
        XX_TEST_EXPECT_EQ(relOk, absOk);
    };

    // 1. cwd 下的相对路径 -> 命中 {cwd}/* ALLOW (绝对/相对一致)
    co_await check("src/main.cpp", fmt::format("{}/src/main.cpp", cwd));
    co_await check("./a.txt", fmt::format("{}/a.txt", cwd));

    // 2. cwd 下 secret 目录 -> 命中 {cwd}/secret/* DENY (最长前缀优先于外层 ALLOW)
    co_await check("secret/x.log", fmt::format("{}/secret/x.log", cwd));

    // 3. 带 .. 的相对路径 -> 词法规范化后命中 /* INTERRUPT (无 prompter 时拒绝)
    {
        std::error_code ec;
        auto            parent = std::filesystem::path{cwd}.parent_path().generic_string();
        co_await check("../outside.txt", fmt::format("{}/outside.txt", parent));
    }

    // 4. 空路径与 cwd 路径均不命中 {cwd}/* 规则, 回退到 /* INTERRUPT
    //    (无 prompter 时均拒绝, 行为一致)
    auto emptyArgs = neograph::json{{"path", ""}};
    auto emptyOk   = co_await permission->defOnFilesystemHandle(
        item,
        emptyArgs,
        agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
    );
    auto cwdArgs = neograph::json{{"path", cwd}};
    auto cwdOk   = co_await permission->defOnFilesystemHandle(
        item,
        cwdArgs,
        agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
    );
    XX_TEST_EXPECT_EQ(emptyOk, cwdOk);

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
        co_await test_permission_relative_path();
    } catch (const std::exception& e) {
        TEST_FAIL << "interrupt_bus suite exception: " << e.what() << std::endl;
        g_ib_failed++;
    }
    co_return TestResult{g_ib_passed, g_ib_failed};
}

} // namespace test
} // namespace agentxx
