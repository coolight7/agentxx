#include "test_interrupt_bus.h"
#include "agentxx-client/io/stdio/agent_stdio.h"
#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/event/events.h"
#include "agentxx/middlewares/interrupt_presets.h"
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

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_ib_passed = 0;
int g_ib_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_ib_passed
#define XX_TEST_FAILED g_ib_failed

namespace agentxx {
namespace test {

/// 确定性 Mock IO: handleInterrupt 不依赖 stdin, 返回可控结果, 供总线往返测试
class MockIO : public agentxx::agent::AgentIOBase {
public:

    std::string interruptTag    = "answered";
    bool        permissionAllow = true;
    /// 权限询问是否勾选"记住本次选择" (结果 values.remember = true)
    bool        permissionRemember = false;
    int         interruptCalls     = 0;
    /// 最近一次中断请求参数 (InterruptHandleArg JSON; 供断言 UI 描述下发)
    std::string lastInterruptArgJson;
    /// true = 故意回传契约外的非对象结果 (验证服务端按未应答/拒绝处理)
    bool        malformedResult = false;

    void onDelta(const agentxx::agent::WireDelta&) override {}

    void onSync(const agentxx::agent::WireSyncPayload&) override {}

    asio::awaitable<std::optional<std::string>> getInput() override {
        co_return std::nullopt;
    }

    asio::awaitable<agentxx::util::Json> handleInterrupt(
        std::string_view /*sessionId*/,
        std::string_view interruptNode,
        std::string_view /*interruptValue*/,
        std::string_view interruptArgJson
    ) override {
        ++interruptCalls;
        lastInterruptArgJson = std::string{interruptArgJson};
        if (malformedResult) {
            co_return agentxx::util::Json::array({interruptTag}); // 契约外形态
        }
        // 结果恒为对象形态 {"values": {控件 id: 值}} (客户端契约:
        // agentxx::middleware::makeInterruptResult)
        if (interruptNode == "permission") {
            // 权限卡片控件: decision (允许/拒绝) + remember (勾选项)
            co_return agentxx::middleware::makeInterruptResult(agentxx::util::Json{
                {"decision", permissionAllow ? "true" : "false"},
                {"remember", permissionRemember},
            });
        }
        // 通用确认卡片控件: allow
        co_return agentxx::middleware::makeInterruptResult(
            agentxx::util::Json{{"allow", interruptTag}}
        );
    }
};

/// 中断总线往返: MockIO 注册后, request 应确定性拿到结果 (不依赖 stdin/不超时)
asio::awaitable<void> test_interrupt_bus_request_response() {
    auto sessionBus
        = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);

    auto io          = std::make_shared<MockIO>();
    io->interruptTag = "answered";
    io->registerOnBus(sessionBus);

    auto resp = co_await sessionBus
                    ->request<agentxx::events::ReqInterrupt, agentxx::events::RespInterrupt>(
                        agentxx::events::Topic::Interrupt,
                        agentxx::events::ReqInterrupt{
                            .agentName         = "test",
                            .sessionId         = "t1",
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
        XX_TEST_EXPECT_EQ(resp->resultJson, "{\"allow\":\"answered\"}");
    }
    XX_TEST_EXPECT_EQ(io->interruptCalls, 1);

    // 新总线 (无任何 server) 上 request 应超时返回 nullopt
    auto deadBus = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);
    auto resp2
        = co_await deadBus->request<agentxx::events::ReqInterrupt, agentxx::events::RespInterrupt>(
            agentxx::events::Topic::Interrupt,
            agentxx::events::ReqInterrupt{
                .agentName         = "test",
                .sessionId         = "t1",
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
        = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);

    auto io             = std::make_shared<MockIO>();
    io->permissionAllow = true;
    io->registerOnBus(sessionBus);

    auto reqAllow = agentxx::events::ReqPermission{
        .agentName     = "test",
        .sessionId     = "t1",
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
    auto deadBus = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);
    auto resp2   = co_await deadBus
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
        = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);

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
                            .sessionId         = "t",
                            .interruptNode     = "n",
                            .handleName        = "default",
                            .interruptArgsJson = "{}",
                            .resultId          = "r",
                        },
                        std::chrono::seconds(5)
                    );
    XX_TEST_EXPECT_TRUE(resp.has_value());
    if (resp.has_value()) {
        XX_TEST_EXPECT_EQ(resp->resultJson, "{\"allow\":\"v2\"}");
    }

    co_return;
}

/// 验证: 自定义 interrupt handler 可替换 CLI handler (扩展性)
asio::awaitable<void> test_interrupt_bus_custom_handler() {
    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    agentContext->bus
        = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);

    // 注册一个自定义 handler, 直接返回固定结果
    auto& rr
        = agentContext->bus->getRR<agentxx::events::ReqInterrupt, agentxx::events::RespInterrupt>(
            agentxx::events::Topic::Interrupt
        );
    rr.registerServer(
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
                            .sessionId         = "t",
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

/// 最小 Tool 实现: 仅提供名称 (权限路由测试用)
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

    std::string get_name() const override {
        return name_;
    }

    std::string execute(const neograph::json&) override {
        return "";
    }

private:

    std::string name_;
};

/// 权限路由: 相对路径应基于当前工作目录转换为绝对路径后再匹配规则,
/// 使注册的绝对路径规则 (如 {cwd}/*) 也能命中相对路径访问
asio::awaitable<void> test_permission_relative_path() {
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

    MockTool item("agentxx_filesystem_write");

    auto check = [&](std::string_view rel, std::string_view abs) -> asio::awaitable<void> {
        // 相对路径访问
        auto relArgs = agentxx::util::Json{
            {"path", std::string{rel}}
        };
        auto relOk = co_await permission->defOnFilesystemHandle(
            item,
            relArgs,
            agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
        );
        // 对应绝对路径访问
        auto absArgs = agentxx::util::Json{
            {"path", std::string{abs}}
        };
        auto absOk = co_await permission->defOnFilesystemHandle(
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
    auto emptyArgs = agentxx::util::Json{
        {"path", ""}
    };
    auto emptyOk = co_await permission->defOnFilesystemHandle(
        item,
        emptyArgs,
        agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
    );
    auto cwdArgs = agentxx::util::Json{
        {"path", cwd}
    };
    auto cwdOk = co_await permission->defOnFilesystemHandle(
        item,
        cwdArgs,
        agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
    );
    XX_TEST_EXPECT_EQ(emptyOk, cwdOk);

    co_return;
}

/// 记住权限选择: 用户选择允许/拒绝并"记住"后, 注册的路径规则 (ALLOW/DENY) 使
/// 后续访问该路径或其子目录直接按规则处理, 不再经总线询问 (prompter 不被调用)
asio::awaitable<void> test_permission_remember_rule() {
    auto sessionBus
        = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);

    // prompter (模拟客户端权限询问应答): 记录被询问次数
    auto io             = std::make_shared<MockIO>();
    io->permissionAllow = true;
    io->registerOnBus(sessionBus);

    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    auto session      = agentContext->getSession("remember_test");
    session->bus      = sessionBus;
    auto permission
        = std::make_shared<agentxx::middleware::PermissionMiddlewareHandle>(agentContext);

    // 与 code_agent.cpp 默认注册一致: cwd 内写 ALLOW, 其余 /* INTERRUPT (询问)
    auto cwd = std::filesystem::current_path().generic_string();
    permission->setFilesystemPermission(
        fmt::format("{}/*", cwd),
        agentxx::middleware::PermissionOperator::ALLOW,
        agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
    );
    permission->setFilesystemPermission(
        "/*",
        agentxx::middleware::PermissionOperator::INTERRUPT,
        agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
    );

    MockTool          item("agentxx_filesystem_write");
    const std::string outsidePath = "/data/projects/remember/out.txt";
    const std::string subPath     = "/data/projects/remember/sub/deep.txt";
    const std::string secretPath  = "/data/projects/remember/secret/key.txt";

    auto write = [&](std::string_view path) -> asio::awaitable<bool> {
        // 必须携带 sessionId: requestPermission 经 sessions->get(sessionId) 取会话总线
        auto args = agentxx::util::Json{
            {"path",      std::string{path}},
            {"sessionId", "remember_test"  }
        };
        co_return co_await permission->defOnFilesystemHandle(
            item,
            args,
            agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
        );
    };

    // 初始: 未注册规则 → INTERRUPT → 经总线询问 (prompter 应答允许)
    bool ok = co_await write(outsidePath);
    XX_TEST_EXPECT_TRUE(ok);
    XX_TEST_EXPECT_EQ(io->interruptCalls, 1);

    // 用户选择"记住 (允许)": 注册 ALLOW 规则
    // (等价于权限中间件按应答 RespPermission.remember 自行注册)
    permission->setFilesystemPermission(
        "/data/projects/remember",
        agentxx::middleware::PermissionOperator::ALLOW,
        agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
    );

    // 记住后: 该路径及其子目录直接放行, 不再询问
    ok = co_await write(outsidePath);
    XX_TEST_EXPECT_TRUE(ok);
    ok = co_await write(subPath);
    XX_TEST_EXPECT_TRUE(ok);
    XX_TEST_EXPECT_EQ(io->interruptCalls, 1); // 未再询问

    // 最长前缀优先: 子目录记住"拒绝" (DENY) 覆盖外层 ALLOW, 且不再询问
    permission->setFilesystemPermission(
        "/data/projects/remember/secret",
        agentxx::middleware::PermissionOperator::DENY,
        agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
    );
    ok = co_await write(secretPath);
    XX_TEST_EXPECT_FALSE(ok);
    XX_TEST_EXPECT_EQ(io->interruptCalls, 1); // 仍未再询问

    co_return;
}

/// 记住权限选择 (生产总线拓扑) + 目录规则覆盖子目录与文件:
/// - 生产环境中权限中间件注册在 agent 全局总线 (agentContext->bus) 上, 而 IO 端点
///   的 interrupt/permission 服务注册在会话总线 (session->bus) 上 —— 两者是不同对象
/// - 规则表归中间件所有, 因此"记住本次选择"经 RespPermission.remember 回传给中间件
///   自行注册 (若改由端点经会话总线发布规则事件, 中间件收不到, 表现为勾选记住后
///   下次访问仍反复询问)
/// - 目录规则按最长前缀匹配覆盖其下全部子目录与文件 (读/写各自一套规则)
asio::awaitable<void> test_permission_remember_across_bus_and_dir_subtree() {
    // 生产拓扑: 两个独立总线 (agent 全局总线 / 会话总线)
    auto agentBus = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);
    auto sessionBus
        = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);

    // 客户端权限应答 (勾选"记住本次选择", 允许)
    auto io                = std::make_shared<MockIO>();
    io->permissionAllow    = true;
    io->permissionRemember = true;
    io->registerOnBus(sessionBus);

    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    agentContext->bus = agentBus;
    auto session      = agentContext->getSession("remember_cross_bus");
    session->bus      = sessionBus;
    auto permission
        = std::make_shared<agentxx::middleware::PermissionMiddlewareHandle>(agentContext);
    permission->registerOnBus(agentBus);
    // 无任何已注册规则 → 一律询问
    permission->noRuleOperator = agentxx::middleware::PermissionOperator::INTERRUPT;

    // 真实临时目录树: <tmp>/agentxx_perm_remember/{allowed/{file.txt,sub/deep.txt},
    // wdir/{file.txt,sub/deep.txt}, other/, denied/inner.txt}
    const auto      tmpRoot = std::filesystem::temp_directory_path() / "agentxx_perm_remember";
    std::error_code ec;
    std::filesystem::remove_all(tmpRoot, ec);
    std::filesystem::create_directories(tmpRoot / "allowed" / "sub", ec);
    std::filesystem::create_directories(tmpRoot / "wdir" / "sub", ec);
    std::filesystem::create_directories(tmpRoot / "other", ec);
    std::filesystem::create_directories(tmpRoot / "denied", ec);
    const std::string allowedDir  = (tmpRoot / "allowed").generic_string();
    const std::string allowedSub  = (tmpRoot / "allowed" / "sub").generic_string();
    const std::string allowedDeep = (tmpRoot / "allowed" / "sub" / "deep.txt").generic_string();
    const std::string allowedFile = (tmpRoot / "allowed" / "file.txt").generic_string();
    const std::string wdirDir     = (tmpRoot / "wdir").generic_string();
    const std::string wdirSub     = (tmpRoot / "wdir" / "sub").generic_string();
    const std::string wdirFile    = (tmpRoot / "wdir" / "file.txt").generic_string();
    const std::string otherPath   = (tmpRoot / "other" / "x.txt").generic_string();
    const std::string otherPath2  = (tmpRoot / "other" / "y.txt").generic_string();
    const std::string deniedDir   = (tmpRoot / "denied").generic_string();
    const std::string deniedInner = (tmpRoot / "denied" / "inner.txt").generic_string();

    MockTool readItem("agentxx_filesystem_read");
    MockTool writeItem("agentxx_filesystem_write");

    auto check = [&](const MockTool&  item,
                     std::string_view path,
                     size_t           index) -> asio::awaitable<bool> {
        auto args = agentxx::util::Json{
            {"path",      std::string{path}   },
            {"sessionId", "remember_cross_bus"}
        };
        co_return co_await permission->defOnFilesystemHandle(item, args, index);
    };
    auto read = [&](std::string_view path) -> asio::awaitable<bool> {
        co_return co_await check(
            readItem,
            path,
            agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionREAD
        );
    };
    auto write = [&](std::string_view path) -> asio::awaitable<bool> {
        co_return co_await check(
            writeItem,
            path,
            agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
        );
    };

    // 1. 读目录: 首次询问 → 勾选记住 (允许) → 目录规则立即生效
    bool ok = co_await read(allowedDir);
    XX_TEST_EXPECT_TRUE(ok);
    XX_TEST_EXPECT_EQ(io->interruptCalls, 1);

    // 2. 记住目录后: 目录自身/其下文件/子目录/子目录内文件均直接放行, 不再询问
    ok = co_await read(allowedFile);
    XX_TEST_EXPECT_TRUE(ok);
    XX_TEST_EXPECT_EQ(io->interruptCalls, 1); // 未再询问
    ok = co_await read(allowedSub);
    XX_TEST_EXPECT_TRUE(ok);
    XX_TEST_EXPECT_EQ(io->interruptCalls, 1);
    ok = co_await read(allowedDeep);
    XX_TEST_EXPECT_TRUE(ok);
    XX_TEST_EXPECT_EQ(io->interruptCalls, 1);

    // 3. 读规则不外溢: 兄弟目录仍需询问 (询问后同样记住其子树)
    ok = co_await read(otherPath);
    XX_TEST_EXPECT_TRUE(ok);
    XX_TEST_EXPECT_EQ(io->interruptCalls, 2);

    // 4. 写规则独立于读规则: 写目录询问一次并记住后, 写其下文件/子目录均放行
    ok = co_await write(wdirDir);
    XX_TEST_EXPECT_TRUE(ok);
    XX_TEST_EXPECT_EQ(io->interruptCalls, 3);
    ok = co_await write(wdirFile);
    XX_TEST_EXPECT_TRUE(ok);
    XX_TEST_EXPECT_EQ(io->interruptCalls, 3);
    ok = co_await write(wdirSub);
    XX_TEST_EXPECT_TRUE(ok);
    XX_TEST_EXPECT_EQ(io->interruptCalls, 3);

    // 5. 记住"拒绝": 该目录及其子目录后续直接拒绝, 不再询问
    io->permissionAllow = false;
    ok                  = co_await read(deniedDir);
    XX_TEST_EXPECT_FALSE(ok);
    XX_TEST_EXPECT_EQ(io->interruptCalls, 4);
    ok = co_await read(deniedInner);
    XX_TEST_EXPECT_FALSE(ok);
    XX_TEST_EXPECT_EQ(io->interruptCalls, 4); // 拒绝规则覆盖子树且未再询问

    // 6. 未勾选记住时不注册规则: 每次访问都继续询问
    io->permissionAllow    = true;
    io->permissionRemember = false;
    ok                     = co_await read(otherPath2);
    XX_TEST_EXPECT_TRUE(ok);
    XX_TEST_EXPECT_EQ(io->interruptCalls, 5);
    ok = co_await read(otherPath2);
    XX_TEST_EXPECT_TRUE(ok);
    XX_TEST_EXPECT_EQ(io->interruptCalls, 6);

    std::filesystem::remove_all(tmpRoot, ec);
    co_return;
}

/// worktree 会话隔离边界: worktree 子树 (allowPath) 内读写放行, 主检出子树
/// (denyWritePath) 内写操作拒绝 (读不受限)
/// - 真实 worktree 位于主检出的 `.agentxx/agent/worktrees/{name}` 下 (见
///   agentxx::util::worktree::worktreesRoot), 即 allowPath 本身就在 denyWritePath
///   子树内; 因此 allowPath 必须先于 denyWritePath 判定, 否则会话对自身工作区的
///   写操作也会被"主检出写拒绝"命中 (表现为绑定 worktree 后无法写任何文件)
asio::awaitable<void> test_permission_worktree_isolation_subtree() {
    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    auto permission
        = std::make_shared<agentxx::middleware::PermissionMiddlewareHandle>(agentContext);
    // 未命中任何规则 → 询问 (无 prompter 时拒绝, 用于断言"未放行")
    permission->noRuleOperator = agentxx::middleware::PermissionOperator::INTERRUPT;

    // 真实目录布局: <tmp>/agentxx_wt_iso/repo/{src, .agentxx/agent/worktrees/wt-1/src}
    const auto      tmpRoot = std::filesystem::temp_directory_path() / "agentxx_wt_iso";
    std::error_code ec;
    std::filesystem::remove_all(tmpRoot, ec);
    const std::string repoDir  = (tmpRoot / "repo").generic_string();
    const std::string worktree = (tmpRoot / "repo" / ".agentxx" / "agent" / "worktrees" / "wt-1")
                                     .generic_string();
    std::filesystem::create_directories(worktree + "/src", ec);
    std::filesystem::create_directories(repoDir + "/src", ec);
    const std::string wtFile    = worktree + "/src/wt.cpp";
    const std::string mainFile  = repoDir + "/src/main.cpp";

    // Ask 模式默认规则: 工作目录 (主检出根) 内读写放行
    permission->setFilesystemPermission(
        repoDir,
        agentxx::middleware::PermissionOperator::ALLOW,
        agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionREAD
    );
    permission->setFilesystemPermission(
        repoDir,
        agentxx::middleware::PermissionOperator::ALLOW,
        agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
    );
    // 会话绑定 worktree: worktree 子树放行, 主检出子树写拒绝
    permission->setSessionIsolation(
        "wt_session",
        agentxx::middleware::SessionFsIsolation{
            .allowPath     = worktree,
            .denyWritePath = repoDir,
        }
    );

    MockTool readItem("agentxx_filesystem_read");
    MockTool writeItem("agentxx_filesystem_write");
    auto     check = [&](const MockTool&  item,
                     std::string_view path,
                     size_t           index) -> asio::awaitable<bool> {
        auto args = agentxx::util::Json{
            {"path",      std::string{path} },
            {"sessionId", "wt_session"       }
        };
        co_return co_await permission->defOnFilesystemHandle(item, args, index);
    };

    using Mw = agentxx::middleware::PermissionMiddlewareHandle;
    // 1. worktree 子树内写: 放行 (未被主检出写拒绝命中)
    XX_TEST_EXPECT_TRUE(
        co_await check(writeItem, wtFile, Mw::FilesystemPermissionWRITE)
    );
    XX_TEST_EXPECT_TRUE(
        co_await check(writeItem, worktree, Mw::FilesystemPermissionWRITE)
    );
    // 2. worktree 子树内读: 放行
    XX_TEST_EXPECT_TRUE(
        co_await check(readItem, wtFile, Mw::FilesystemPermissionREAD)
    );
    // 3. 主检出子树写 (worktree 之外): 拒绝 (读不受限)
    XX_TEST_EXPECT_FALSE(
        co_await check(writeItem, mainFile, Mw::FilesystemPermissionWRITE)
    );
    XX_TEST_EXPECT_FALSE(
        co_await check(writeItem, repoDir, Mw::FilesystemPermissionWRITE)
    );
    // 4. 主检出子树读: 不受隔离影响 (按已注册规则放行)
    XX_TEST_EXPECT_TRUE(
        co_await check(readItem, mainFile, Mw::FilesystemPermissionREAD)
    );

    // 5. 清除隔离后: 主检出写恢复按规则放行
    permission->clearSessionIsolation("wt_session");
    XX_TEST_EXPECT_TRUE(
        co_await check(writeItem, mainFile, Mw::FilesystemPermissionWRITE)
    );

    std::filesystem::remove_all(tmpRoot, ec);
    co_return;
}

/// 中断结果组装 (客户端 → agent 的 JSON 形态): 恒为对象 {"values": {控件 id: 值}}
void test_make_interrupt_result_forms() {
    using agentxx::middleware::makeInterruptResult;
    using agentxx::util::Json;

    // 正常提交: 控件 id → 值
    auto form = makeInterruptResult(Json{{"decision", "true"}, {"remember", true}});
    XX_TEST_EXPECT_TRUE(form.is_object());
    XX_TEST_EXPECT_TRUE(form.contains("values"));
    if (form.contains("values") && form["values"].is_object()) {
        XX_TEST_EXPECT_EQ(form["values"]["decision"].get<std::string>(), std::string("true"));
        XX_TEST_EXPECT_TRUE(form["values"].value("remember", false));
    }

    // 非对象 values 归一化为空对象 (取消/未提交语义)
    auto normalized = makeInterruptResult(Json::array({"x"}));
    XX_TEST_EXPECT_TRUE(normalized.is_object());
    XX_TEST_EXPECT_TRUE(normalized["values"].is_object());
    XX_TEST_EXPECT_TRUE(normalized["values"].empty());

    // 结果取值 helper: 整体结果对象与纯 values 对象两种口径均可
    const auto boolTrue = Json{{"decision", "true"}};
    XX_TEST_EXPECT_TRUE(agentxx::middleware::interruptValueBool(form, "decision", false));
    XX_TEST_EXPECT_TRUE(agentxx::middleware::interruptValueBool(boolTrue, "decision", false));
    XX_TEST_EXPECT_FALSE(agentxx::middleware::interruptValueBool(form, "missing", false));
    XX_TEST_EXPECT_EQ(
        agentxx::middleware::interruptValueString(form, "decision", ""),
        std::string("true")
    );
    XX_TEST_EXPECT_TRUE(agentxx::middleware::interruptValueBool(Json{{"n", 3}}, "n", false));
    XX_TEST_EXPECT_EQ(agentxx::middleware::interruptValueInt(Json{{"n", "42"}}, "n", 0), int64_t{42});
}

/// 权限询问的 UI 描述下发: 中断参数须携带声明式描述 (`ui` 字段),
/// 客户端据此通用渲染 (分段头/勾选项/一键按钮), 不再识别 permission 语义
asio::awaitable<void> test_permission_prompt_carries_ui_descriptor() {
    auto sessionBus
        = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);

    auto io             = std::make_shared<MockIO>();
    io->permissionAllow = true;
    io->registerOnBus(sessionBus);

    auto req = agentxx::events::ReqPermission{
        .agentName     = "test",
        .sessionId     = "t1",
        .toolName      = "agentxx_filesystem_write",
        .category      = "filesystem_write",
        .target        = "/data/projects/ui/out.txt",
        .argumentsJson = R"({"path":"/data/projects/ui/out.txt"})",
    };
    auto resp = co_await sessionBus
                    ->request<agentxx::events::ReqPermission, agentxx::events::RespPermission>(
                        agentxx::events::Topic::Permission,
                        req,
                        std::chrono::seconds(5)
                    );
    XX_TEST_EXPECT_TRUE(resp.has_value());

    // 描述解析 (预设模板生成的权限卡片): 头行分段含权限标记/工具名/分类,
    // blocks 含目标描述文本/勾选控件 (remember)/按钮控件 (decision)
    const auto argOpt = agentxx::middleware::InterruptHandleArg::fromJson(
        agentxx::util::Json::parse(io->lastInterruptArgJson)
    );
    XX_TEST_EXPECT_TRUE(argOpt.has_value());
    if (argOpt.has_value()) {
        const auto& ui = argOpt->ui;
        XX_TEST_EXPECT_FALSE(ui.empty());
        XX_TEST_EXPECT_EQ(ui.header.segments.size(), size_t{3});
        if (ui.header.segments.size() == 3) {
            XX_TEST_EXPECT_EQ(ui.header.segments[0].text, std::string("! [Permission] "));
            XX_TEST_EXPECT_EQ(ui.header.segments[1].text, std::string("agentxx_filesystem_write"));
            XX_TEST_EXPECT_EQ(ui.header.segments[2].text, std::string(" filesystem_write"));
        }
        // 块序列: 目标描述 (hint, 硬折行) / 空行 / 勾选控件 / 空行 / 按钮控件
        size_t hintTexts = 0;
        size_t checkbox  = 0;
        size_t buttons   = 0;
        for (const auto& block : ui.blocks) {
            if (block.kind == "text" && block.color == "hint") {
                ++hintTexts;
                XX_TEST_EXPECT_TRUE(block.wrap);
            } else if (block.kind == "control" && block.control == "checkbox") {
                ++checkbox;
                XX_TEST_EXPECT_EQ(block.id, std::string("remember"));
                XX_TEST_EXPECT_EQ(block.labelKey, std::string("interrupt.remember"));
            } else if (block.kind == "control" && block.control == "buttons") {
                ++buttons;
                XX_TEST_EXPECT_EQ(block.id, std::string("decision"));
                XX_TEST_EXPECT_TRUE(block.commitOnPick);
                XX_TEST_EXPECT_EQ(block.options.size(), size_t{2});
                if (block.options.size() == 2) {
                    XX_TEST_EXPECT_EQ(block.options[0].value.get<std::string>(), std::string("true"));
                    XX_TEST_EXPECT_EQ(block.options[1].value.get<std::string>(), std::string("false"));
                }
            }
        }
        XX_TEST_EXPECT_EQ(hintTexts, size_t{1});
        XX_TEST_EXPECT_EQ(checkbox, size_t{1});
        XX_TEST_EXPECT_EQ(buttons, size_t{1});
        // 文件目标: 勾选项不附生效范围提示 (仅记住该文件本身)
        for (const auto& block : ui.blocks) {
            if (block.kind == "control" && block.control == "checkbox") {
                XX_TEST_EXPECT_TRUE(block.help.empty());
                XX_TEST_EXPECT_TRUE(block.helpKey.empty());
            }
        }
    }

    // 目录目标 (规范化路径带尾斜杠): 勾选项附生效范围提示 —— 记住的目录规则同时
    // 覆盖其子目录与文件 (与中间件最长前缀匹配语义一致)
    auto reqDir     = req;
    reqDir.target   = "/data/projects/ui/";
    auto respDir    = co_await sessionBus
                       ->request<agentxx::events::ReqPermission, agentxx::events::RespPermission>(
                           agentxx::events::Topic::Permission,
                           reqDir,
                           std::chrono::seconds(5)
                       );
    XX_TEST_EXPECT_TRUE(respDir.has_value());
    const auto argDirOpt = agentxx::middleware::InterruptHandleArg::fromJson(
        agentxx::util::Json::parse(io->lastInterruptArgJson)
    );
    XX_TEST_EXPECT_TRUE(argDirOpt.has_value());
    if (argDirOpt.has_value()) {
        bool checkedDirHelp = false;
        for (const auto& block : argDirOpt->ui.blocks) {
            if (block.kind == "control" && block.control == "checkbox") {
                XX_TEST_EXPECT_EQ(block.id, std::string("remember"));
                XX_TEST_EXPECT_EQ(block.helpKey, std::string("interrupt.rememberDir"));
                XX_TEST_EXPECT_FALSE(block.help.empty());
                checkedDirHelp = true;
            }
        }
        XX_TEST_EXPECT_TRUE(checkedDirHelp);
    }
    co_return;
}

/// 中断参数序列化: ui 原样下发 (HIL 中断必填, 由生产者用预设模板/组装器构造)
void test_interrupt_arg_ui_roundtrip() {
    using agentxx::middleware::InterruptHandleArg;
    using agentxx::middleware::InterruptUi;

    // 未声明描述: 序列化为空 ui (该中断不进入客户端渲染路径 —— 如 subagent 委派;
    // 客户端收到空 ui 时按契约错误渲染诊断行, 不静默回退默认表单)
    InterruptHandleArg arg;
    arg.name = "default";
    const auto j = arg.toJson();
    XX_TEST_EXPECT_TRUE(j.contains("ui"));
    XX_TEST_EXPECT_TRUE(InterruptUi::fromJson(j["ui"]).empty());

    // 预设模板 (inputForm): 类型化输入展开为"标签 + 说明 + 控件 + 提交行",
    // 控件形态与结果 id 由预设决定 (客户端不感知类型)
    agentxx::middleware::preset::InputSpec mode;
    mode.label        = "Mode";
    mode.depict       = "pick one";
    mode.type         = "enum";
    mode.defaultValue = "fast";
    mode.enumValues   = {"fast", "slow"};
    agentxx::middleware::preset::InputSpec retries;
    retries.label        = "Retries";
    retries.type         = "int";
    retries.defaultValue = "1";
    arg.ui               = agentxx::middleware::preset::inputForm({mode, retries});

    const auto jForm = arg.toJson();
    const auto uiForm = InterruptUi::fromJson(jForm["ui"]);
    XX_TEST_EXPECT_FALSE(uiForm.empty());
    size_t controls = 0;
    for (const auto& block : uiForm.blocks) {
        if (block.kind != "control") {
            continue;
        }
        ++controls;
        XX_TEST_EXPECT_TRUE(!block.control.empty());
    }
    XX_TEST_EXPECT_EQ(controls, size_t{2});
    // 控件 id: 多输入项为 value1/value2 (结果 values 的键)
    XX_TEST_EXPECT_EQ(uiForm.blocks[2].id, std::string("value1"));
    XX_TEST_EXPECT_EQ(uiForm.blocks[2].control, std::string("select"));
    // 末块为提交行
    XX_TEST_EXPECT_EQ(uiForm.blocks.back().kind, std::string("submit"));

    // 权限卡片预设: 序列化往返一致 (含头行分段)
    arg.ui               = agentxx::middleware::preset::permissionCard(
        "read_file",
        "filesystem_read",
        "/tmp/y"
    );
    const auto jPerm = arg.toJson();
    const auto uiPerm = InterruptUi::fromJson(jPerm["ui"]);
    XX_TEST_EXPECT_EQ(uiPerm.header.segments.size(), size_t{3});
    const auto roundtrip = InterruptUi::fromJson(uiPerm.toJson());
    XX_TEST_EXPECT_EQ(roundtrip.blocks.size(), uiPerm.blocks.size());
    XX_TEST_EXPECT_EQ(roundtrip.toJson().dump(), uiPerm.toJson().dump());
}

/// 契约外的中断结果 (非对象形态) 不被接受: HIL 按未应答处理, 权限询问按拒绝处理
asio::awaitable<void> test_malformed_result_rejected() {
    auto sessionBus
        = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);

    // HIL: 结果非对象 → handled = false (AgentRunner 据此不 resume)
    auto io                = std::make_shared<MockIO>();
    io->interruptTag       = "answered";
    io->malformedResult    = true;
    io->registerOnBus(sessionBus);
    auto resp = co_await sessionBus
                    ->request<agentxx::events::ReqInterrupt, agentxx::events::RespInterrupt>(
                        agentxx::events::Topic::Interrupt,
                        agentxx::events::ReqInterrupt{
                            .agentName         = "test",
                            .sessionId         = "t1",
                            .interruptNode     = "tool_x",
                            .handleName        = "default",
                            .interruptArgsJson = "{}",
                            .resultId          = "call_1",
                        },
                        std::chrono::seconds(5)
                    );
    XX_TEST_EXPECT_TRUE(resp.has_value());
    if (resp.has_value()) {
        XX_TEST_EXPECT_FALSE(resp->handled);
    }

    // 权限: 结果非对象 → 拒绝
    auto respPerm
        = co_await sessionBus->request<agentxx::events::ReqPermission, agentxx::events::RespPermission>(
            agentxx::events::Topic::Permission,
            agentxx::events::ReqPermission{
                .agentName     = "test",
                .sessionId     = "t1",
                .toolName      = "agentxx_filesystem_write",
                .category      = "filesystem_write",
                .target        = "/tmp/z",
                .argumentsJson = R"({"path":"/tmp/z"})",
            },
            std::chrono::seconds(5)
        );
    XX_TEST_EXPECT_TRUE(respPerm.has_value());
    if (respPerm.has_value()) {
        XX_TEST_EXPECT_TRUE(respPerm->decision == agentxx::events::RespPermission::Decision::Deny);
    }
    co_return;
}

/// 权限询问 + "记住本次选择" (结果对象形态):
/// 客户端只回传 {"values": {...}} (含 remember); 规则注册由 agent 侧 permission
/// 处理器完成 (客户端不参与权限语义)
asio::awaitable<void> test_permission_remember_via_result_options() {
    auto sessionBus
        = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);
    // agent 全局总线 (权限中间件注册在此; 与 IO 端点所在的会话总线相互独立)
    auto agentBus
        = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);

    auto io                = std::make_shared<MockIO>();
    io->permissionAllow    = true;
    io->permissionRemember = true; // 结果 = {"values":{"decision":"true","remember":true}}
    io->registerOnBus(sessionBus);

    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    agentContext->bus = agentBus;
    auto session      = agentContext->getSession("remember_options");
    session->bus      = sessionBus;
    auto permission
        = std::make_shared<agentxx::middleware::PermissionMiddlewareHandle>(agentContext);
    // 规则表归权限中间件所有, 记住选择经应答 (RespPermission.remember) 由本中间件注册
    permission->registerOnBus(agentBus);
    // 无任何已注册规则 → 一律询问 (走 HIL 权限询问路径)
    permission->noRuleOperator = agentxx::middleware::PermissionOperator::INTERRUPT;

    MockTool          item("agentxx_filesystem_write");
    const std::string targetPath = "/data/projects/remember_opts/out.txt";

    auto write = [&](std::string_view path) -> asio::awaitable<bool> {
        auto args = agentxx::util::Json{
            {"path",      std::string{path}       },
            {"sessionId", "remember_options"      }
        };
        co_return co_await permission->defOnFilesystemHandle(
            item,
            args,
            agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
        );
    };

    // 首次: 询问 (MockIO 应答 允许 + 记住)
    bool ok = co_await write(targetPath);
    XX_TEST_EXPECT_TRUE(ok);
    XX_TEST_EXPECT_EQ(io->interruptCalls, 1);

    // 记住生效: 同一目标路径直接放行, 不再询问
    ok = co_await write(targetPath);
    XX_TEST_EXPECT_TRUE(ok);
    XX_TEST_EXPECT_EQ(io->interruptCalls, 1); // 未再询问

    // 规则仅覆盖记住的目标: 其他路径仍会询问 (注册按目标路径前缀匹配)
    const std::string otherPath = "/data/projects/remember_opts/other.txt";
    ok                          = co_await write(otherPath);
    XX_TEST_EXPECT_TRUE(ok);
    XX_TEST_EXPECT_EQ(io->interruptCalls, 2);

    // 目录型目标: 记住后其子路径一并放行
    const std::string dirPath   = "/data/projects/remember_opts/sub";
    const std::string childPath = "/data/projects/remember_opts/sub/deep.txt";
    ok                          = co_await write(dirPath);
    XX_TEST_EXPECT_TRUE(ok);
    XX_TEST_EXPECT_EQ(io->interruptCalls, 3);
    ok = co_await write(childPath);
    XX_TEST_EXPECT_TRUE(ok);
    XX_TEST_EXPECT_EQ(io->interruptCalls, 3); // 子路径未再询问

    co_return;
}

/// HIL 中断结果对象形态: 结果 {"values": {控件 id: 值}} 只取 values 对象写回
/// resume 值 (消费端按控件 id 取值)
asio::awaitable<void> test_hil_interrupt_result_object_values_only() {
    auto sessionBus
        = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);

    auto io = std::make_shared<MockIO>();
    io->registerOnBus(sessionBus);

    // 直接经总线请求 HIL 中断, 断言 resultJson 为 values 对象 (控件 id → 值)
    auto resp = co_await sessionBus
                    ->request<agentxx::events::ReqInterrupt, agentxx::events::RespInterrupt>(
                        agentxx::events::Topic::Interrupt,
                        agentxx::events::ReqInterrupt{
                            .agentName         = "test",
                            .sessionId         = "t1",
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
        XX_TEST_EXPECT_EQ(resp->resultJson, "{\"allow\":\"answered\"}");
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
        co_await test_permission_relative_path();
        co_await test_permission_remember_rule();
        co_await test_permission_prompt_carries_ui_descriptor();
        co_await test_permission_remember_via_result_options();
        co_await test_permission_remember_across_bus_and_dir_subtree();
        co_await test_permission_worktree_isolation_subtree();
        co_await test_hil_interrupt_result_object_values_only();
        co_await test_malformed_result_rejected();
        test_make_interrupt_result_forms();
        test_interrupt_arg_ui_roundtrip();
    } catch (const std::exception& e) {
        TEST_FAIL << "interrupt_bus suite exception: " << e.what() << std::endl;
        g_ib_failed++;
    }
    co_return TestResult{g_ib_passed, g_ib_failed};
}

} // namespace test
} // namespace agentxx
