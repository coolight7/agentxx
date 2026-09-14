#include "test_interrupt_bus.h"
#include "agentxx-client/io/stdio/agent_stdio.h"
#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/event/events.h"
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
    /// 权限询问是否勾选"记住本次选择" (结果 options.remember = true)
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
        // 结果恒为对象形态 {"values":[...], "options":{...}} (客户端契约:
        // agentxx::middleware::makeInterruptResult)
        if (interruptNode == "permission") {
            auto options = agentxx::util::Json::object();
            if (permissionRemember) {
                options["remember"] = true;
            }
            co_return agentxx::middleware::makeInterruptResult(
                agentxx::util::Json::array({permissionAllow ? "true" : "false"}),
                options
            );
        }
        co_return agentxx::middleware::makeInterruptResult(
            agentxx::util::Json::array({interruptTag}),
            agentxx::util::Json::object()
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
        XX_TEST_EXPECT_EQ(resp->resultJson, "[\"answered\"]");
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
        XX_TEST_EXPECT_EQ(resp->resultJson, "[\"v2\"]");
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

    // 用户选择"记住 (允许)": 注册 ALLOW 规则 (等价于客户端发送 WireSetPermission)
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

/// 中断结果组装 (客户端 → agent 的 JSON 形态): 恒为对象 {values, options}
void test_make_interrupt_result_forms() {
    using agentxx::middleware::makeInterruptResult;
    using agentxx::util::Json;

    // 无勾选项: 仍为对象形态, options 为空对象
    auto emptyOptsForm = makeInterruptResult(Json::array({"true"}), Json::object());
    XX_TEST_EXPECT_TRUE(emptyOptsForm.is_object());
    XX_TEST_EXPECT_TRUE(emptyOptsForm.contains("values"));
    XX_TEST_EXPECT_TRUE(emptyOptsForm.contains("options"));
    if (emptyOptsForm.contains("values") && emptyOptsForm["values"].is_array()) {
        XX_TEST_EXPECT_EQ(emptyOptsForm["values"].size(), size_t{1});
        XX_TEST_EXPECT_EQ(emptyOptsForm["values"][0].get<std::string>(), std::string("true"));
    }
    if (emptyOptsForm.contains("options") && emptyOptsForm["options"].is_object()) {
        XX_TEST_EXPECT_TRUE(emptyOptsForm["options"].empty());
    }

    // 有勾选项: 对象形态携带 options
    auto objForm = makeInterruptResult(Json::array({"false"}), Json{{"remember", true}});
    XX_TEST_EXPECT_TRUE(objForm.is_object());
    XX_TEST_EXPECT_TRUE(objForm.contains("values"));
    XX_TEST_EXPECT_TRUE(objForm.contains("options"));
    if (objForm.contains("values") && objForm["values"].is_array()) {
        XX_TEST_EXPECT_EQ(objForm["values"][0].get<std::string>(), std::string("false"));
    }
    if (objForm.contains("options") && objForm["options"].is_object()) {
        XX_TEST_EXPECT_TRUE(objForm["options"].value("remember", false));
    }

    // 非数组 values / 非对象 options 归一化为合法形态 (契约兜底)
    auto normalized = makeInterruptResult(Json::object(), Json::array());
    XX_TEST_EXPECT_TRUE(normalized.is_object());
    XX_TEST_EXPECT_TRUE(normalized["values"].is_array());
    XX_TEST_EXPECT_TRUE(normalized["options"].is_object());
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

    // 描述解析: 头行分段含权限标记/工具名/分类, 项含勾选项与允许/拒绝按钮,
    // 结果映射声明 options=remember (客户端只回传该选项的值)
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
        // 描述项: 目标描述(硬折行) / 空行 / 勾选项 / 空行 / 一键按钮
        XX_TEST_EXPECT_EQ(ui.items.size(), size_t{5});
        size_t toggles = 0;
        size_t inputs  = 0;
        for (const auto& item : ui.items) {
            if (item.kind == "toggle") {
                ++toggles;
                XX_TEST_EXPECT_EQ(item.id, std::string("remember"));
            } else if (item.kind == "input") {
                ++inputs;
                XX_TEST_EXPECT_EQ(item.view, std::string("buttons"));
                XX_TEST_EXPECT_EQ(item.buttons.size(), size_t{2});
            }
        }
        XX_TEST_EXPECT_EQ(toggles, size_t{1});
        XX_TEST_EXPECT_EQ(inputs, size_t{1});
        XX_TEST_EXPECT_EQ(ui.options.size(), size_t{1});
        if (!ui.options.empty()) {
            XX_TEST_EXPECT_EQ(ui.options[0], std::string("remember"));
        }
        // 输入项字段仍保留 (旧客户端按 inputs 渲染的兼容路径)
        XX_TEST_EXPECT_EQ(argOpt->inputs.size(), size_t{1});
    }
    co_return;
}

/// 中断参数必带 UI 描述 (描述必填): 生产者未声明时 toJson 下发通用默认模板
void test_interrupt_arg_ui_always_present() {
    using agentxx::middleware::InterruptHandleArg;
    using agentxx::middleware::InterruptUi;

    // 未声明描述: toJson 仍下发默认描述 (进度头行 + 输入控件 + 确认行)
    InterruptHandleArg arg;
    arg.name                      = "default";
    InterruptHandleArg::InterruptHandleInputItem item;
    item.label                    = "label";
    item.depict                   = "/tmp/x";
    item.type                     = "bool";
    item.defaultValue             = "no";
    arg.inputs                    = {item};
    const auto j                  = arg.toJson();
    XX_TEST_EXPECT_TRUE(j.contains("ui"));
    const auto ui = InterruptUi::fromJson(j.contains("ui") ? j["ui"] : agentxx::util::Json{});
    XX_TEST_EXPECT_FALSE(ui.empty());
    XX_TEST_EXPECT_EQ(ui.items.size(), size_t{3}); // text + input + submit
    XX_TEST_EXPECT_EQ(ui.values.size(), size_t{1});
    if (!ui.values.empty()) {
        XX_TEST_EXPECT_EQ(ui.values[0], std::string("value"));
    }

    // 显式声明描述: 原样下发 (权限卡片等自定义形态)
    arg.ui = InterruptUi::permissionUi("read_file", "filesystem_read", "/tmp/y");
    const auto jPerm     = arg.toJson();
    const auto uiPerm    = InterruptUi::fromJson(jPerm["ui"]);
    XX_TEST_EXPECT_EQ(uiPerm.header.segments.size(), size_t{3});
    XX_TEST_EXPECT_EQ(uiPerm.options.size(), size_t{1});
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
/// 客户端只回传 {values, options.remember}; 规则注册由 agent 侧 permission
/// 处理器完成 (客户端不再发 WireSetPermission, 也不参与权限语义)
asio::awaitable<void> test_permission_remember_via_result_options() {
    auto sessionBus
        = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);

    auto io                = std::make_shared<MockIO>();
    io->permissionAllow    = true;
    io->permissionRemember = true; // 结果 = {"values":["true"], "options":{"remember":true}}
    io->registerOnBus(sessionBus);

    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    auto session      = agentContext->getSession("remember_options");
    session->bus      = sessionBus;
    auto permission
        = std::make_shared<agentxx::middleware::PermissionMiddlewareHandle>(agentContext);
    // 订阅权限规则设置事件 (记住选择经总线注册规则到本中间件)
    permission->registerOnBus(sessionBus);
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

/// HIL 中断结果对象形态: 结果 {"values":[...], "options":{...}} 只取值数组
/// 写回 resume 值 (options 为界面声明项, 由对应服务端消费)
asio::awaitable<void> test_hil_interrupt_result_object_values_only() {
    auto sessionBus
        = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);

    auto io = std::make_shared<MockIO>();
    io->registerOnBus(sessionBus);

    // 直接经总线请求 HIL 中断 (MockIO 返回纯数组形态), 断言 resultJson 为数组
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
        XX_TEST_EXPECT_EQ(resp->resultJson, "[\"answered\"]");
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
        co_await test_hil_interrupt_result_object_values_only();
        co_await test_malformed_result_rejected();
        test_make_interrupt_result_forms();
        test_interrupt_arg_ui_always_present();
    } catch (const std::exception& e) {
        TEST_FAIL << "interrupt_bus suite exception: " << e.what() << std::endl;
        g_ib_failed++;
    }
    co_return TestResult{g_ib_passed, g_ib_failed};
}

} // namespace test
} // namespace agentxx
