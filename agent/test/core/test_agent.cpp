#include "test_agent.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/middlewares/permission.h"
#include "agentxx/plugin/plugin_manager.h"
#include "asio/as_tuple.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_da_passed = 0;
int g_da_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_da_passed
#define XX_TEST_FAILED g_da_failed

namespace agentxx {
namespace test {

/// 测试用 IO: 记录 sendToPeer 产出的事件与 getInput 调用，供验证使用
/// - 由 BaseAgent 直接驱动 (无 transport/无真实对端), 覆写 sendToPeer 拦截记录事件
class TestAgentIO : public agentxx::agent::AgentIOBase {
public:

    std::vector<agentxx::agent::WireDelta> deltas;
    std::atomic<int>                       deltaCount{0};
    std::atomic<int>                       syncCount{0};
    bool                                   failGetInput = false;

    void sendToPeer(agentxx::agent::WireMessage msg) override {
        std::visit(
            [this](auto&& m) {
                using T = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<T, agentxx::agent::WireDelta>) {
                    deltas.push_back(std::move(m));
                    deltaCount++;
                } else if constexpr (std::is_same_v<T, agentxx::agent::WireSyncPayload>) {
                    syncCount++;
                }
            },
            std::move(msg)
        );
    }

    // 被动接收回调: 测试中不会被调用 (无对端消息), 空实现满足纯虚契约
    void onDelta(const agentxx::agent::WireDelta& /*delta*/) override {}

    void onSync(const agentxx::agent::WireSyncPayload& /*payload*/) override {}

    asio::awaitable<std::optional<std::string>> getInput() override {
        if (failGetInput) {
            co_return std::nullopt;
        }
        co_return std::string{"test_input"};
    }

    asio::awaitable<neograph::json> handleInterrupt(
        std::string_view /*sessionId*/,
        std::string_view /*interruptNode*/,
        std::string_view /*interruptValue*/,
        std::string_view /*interruptArgJson*/
    ) override {
        co_return neograph::json::array();
    }
};

/// 测试用权限应答 IO: 记录权限询问次数, 默认应答允许 (模拟客户端权限询问)
class PermissionTestIO : public agentxx::agent::AgentIOBase {
public:

    std::atomic<int> permissionCalls{0};

    void sendToPeer(agentxx::agent::WireMessage /*msg*/) override {}

    void onDelta(const agentxx::agent::WireDelta& /*delta*/) override {}

    void onSync(const agentxx::agent::WireSyncPayload& /*payload*/) override {}

    asio::awaitable<std::optional<std::string>> getInput() override {
        co_return std::nullopt;
    }

    asio::awaitable<neograph::json> handleInterrupt(
        std::string_view /*sessionId*/,
        std::string_view interruptNode,
        std::string_view /*interruptValue*/,
        std::string_view /*interruptArgJson*/
    ) override {
        if (interruptNode == "permission") {
            permissionCalls++;
            co_return neograph::json::array({"true"});
        }
        co_return neograph::json::array();
    }
};

/// 模拟 tools 节点 start 阶段抛异常的中间件
/// (验证 Toolcall 拦截普通异常后, agent 基于错误消息继续运行)
class ThrowToolcallStartMiddleware
    : public agentxx::middleware::MiddlewareWrapHandle<agentxx::middleware::BaseMiddlewareState> {
public:

    using Base
        = agentxx::middleware::MiddlewareWrapHandle<agentxx::middleware::BaseMiddlewareState>;

    ThrowToolcallStartMiddleware(std::weak_ptr<agentxx::agent::AgentContext> agentContext) :
        Base(
            "ThrowToolcallStart",
            agentContext,
            nullptr, // onAgentcallStart
            nullptr, // onAgentcallEnd
            nullptr, // onModelcallStart
            nullptr, // onModelcallRun
            nullptr, // onModelcallEnd
            // onToolcallStart: 每次 tools 节点 start 阶段抛出异常
            [](neograph::graph::NodeInput&) -> asio::awaitable<void> {
                throw std::runtime_error("simulated toolcall start failure");
            },
            nullptr // onToolcallEnd
        ) {}
};

/// 最小 Tool 实现: 仅提供名称 (权限路由测试用)
class TestTool : public neograph::Tool {
public:

    explicit TestTool(std::string name) :
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

// ===========================================================================
// Enhanced LLM Simulator Implementation
// ===========================================================================
std::string    g_da_sim_response_content  = "Hello! I am a simulated LLM response for testing.";
int            g_da_sim_prompt_tokens     = 100;
int            g_da_sim_completion_tokens = 50;
neograph::json g_da_sim_tool_calls        = neograph::json::array();
std::string    g_da_sim_reasoning_content = "";
int            g_da_sim_delay_ms          = 0;
neograph::json g_da_sim_last_request      = neograph::json::object();
/// 按到达顺序记录所有 /chat/completions 请求 (供测试断言多次请求)
std::vector<neograph::json> g_da_sim_requests;
/// 累计请求计数 (每次 /chat/completions 请求递增, 含失败请求), 供测试验证调用次数
int g_da_sim_request_count = 0;
/// 剩余失败次数: >0 时接下来的请求直接返回 HTTP 500 并递减, 用于模拟 LLM API 持续失败
int g_da_sim_fail_count = 0;
/// 前 N 次请求返回 tool_calls (之后返回纯文本); -1 = 不限制 (旧行为)
/// - 供嵌套委派等"先工具后文本"的多请求序列测试; 由测试显式设置并在
///   结束时恢复 -1 (响应 handler 消费递减, 不自动重置)
int g_da_sim_tool_calls_remaining = -1;

/// 默认模拟器配置
static DaSimConfig g_defaultSimConfig;

DaSimServer::DaSimServer(DaSimServer&& o) noexcept :
    svr(std::move(o.svr)),
    thr(std::move(o.thr)),
    port(o.port) {
    o.port = 0;
}

DaSimServer& DaSimServer::operator=(DaSimServer&& o) noexcept {
    if (this != &o) {
        stop();
        svr    = std::move(o.svr);
        thr    = std::move(o.thr);
        port   = o.port;
        o.port = 0;
    }
    return *this;
}

DaSimServer::~DaSimServer() {
    stop();
}

void DaSimServer::stop() {
    if (svr) {
        svr->stop();
    }
    if (thr.joinable()) {
        thr.join();
    }
    svr.reset();
    port = 0;
}

DaSimServer startDaSimServer() {
    DaSimServer sim;

    agentxx::util::HttpServer::Config cfg;
    cfg.address          = "127.0.0.1";
    cfg.port             = 0;
    cfg.ioThreads        = 1;
    cfg.accessLogEnabled = false;
    cfg.maxConnections   = 128;
    cfg.maxRequestBody   = 1024 * 1024;

    sim.svr      = std::make_unique<agentxx::util::HttpServer>(cfg);
    auto* rawSvr = sim.svr.get();

    rawSvr->router().add(
        "/chat/completions",
        2,
        std::make_shared<agentxx::util::HttpServer::Handler>(
            [](agentxx::util::HttpServer::Request&  req,
               agentxx::util::HttpServer::Response& resp,
               std::string_view) -> asio::awaitable<void> {
                namespace http = boost::beast::http;

                g_da_sim_request_count++;
                if (g_da_sim_fail_count > 0) {
                    // 模拟 LLM API 持续失败: 直接返回 HTTP 500
                    g_da_sim_fail_count--;
                    resp.result(http::status::internal_server_error);
                    resp.set(http::field::content_type, "application/json");
                    resp.body() = R"({"error":{"message":"simulated failure"}})";
                    resp.prepare_payload();
                    co_return;
                }

                if (g_da_sim_delay_ms > 0) {
                    // 模拟慢速 LLM: 延迟后再响应, 供取消测试中断在途请求
                    asio::steady_timer delayTimer(
                        co_await asio::this_coro::executor,
                        std::chrono::milliseconds(g_da_sim_delay_ms)
                    );
                    auto [ec] = co_await delayTimer.async_wait(asio::as_tuple(asio::use_awaitable));
                    if (ec) {
                        co_return;
                    }
                }

                auto j      = neograph::json::parse(req.body());
                bool stream = j.value("stream", false);
                // tool_calls 次数控制: g_da_sim_tool_calls_remaining >= 0 时,
                // 前 N 次请求返回 tool_calls, 之后返回纯文本 (供嵌套委派等
                // 需要"先工具后文本"的多请求序列测试); 默认 -1 保持旧行为
                bool hasToolCalls = !g_da_sim_tool_calls.empty();
                if (g_da_sim_tool_calls_remaining >= 0) {
                    if (g_da_sim_tool_calls_remaining <= 0) {
                        hasToolCalls = false;
                    } else {
                        --g_da_sim_tool_calls_remaining;
                    }
                }
                // 记录请求 (供测试断言模型名/消息前缀; 按到达顺序追加)
                g_da_sim_last_request = j;
                g_da_sim_requests.push_back(j);

                // provider 层现在把"完全无输出"的成功响应当作生成失败 (抛异常交由
                // modelcall 重试链路处理), 模拟器遵循同一契约: 未设置内容时使用占位
                // 文本, 不再产生真正的空响应 (历史用例将 content 设为 "" 仅表示最小
                // 回复, 并非在测试空响应语义)
                const std::string respContent = g_da_sim_response_content.empty()
                                                    ? std::string{"done"}
                                                    : g_da_sim_response_content;

                if (stream) {
                    std::string sseBody;
                    auto        append
                        = [&](const neograph::json& delta, const std::string& finishReason) {
                              auto ev       = neograph::json::object();
                              ev["id"]      = "chatcmpl-test-sim";
                              ev["object"]  = "chat.completion.chunk";
                              ev["created"] = 1234567890;
                              ev["model"]   = "test-sim";

                              auto choice     = neograph::json::object();
                              choice["index"] = 0;
                              choice["delta"] = delta;
                              if (finishReason.empty()) {
                                  choice["finish_reason"] = nullptr;
                              } else {
                                  choice["finish_reason"] = finishReason;
                              }
                              ev["choices"] = neograph::json::array({choice});

                              sseBody += "data: " + ev.dump() + "\n\n";
                          };

                    {
                        auto d    = neograph::json::object();
                        d["role"] = "assistant";
                        if (hasToolCalls) {
                            d["content"] = nullptr;
                        } else {
                            d["content"] = "";
                        }
                        append(d, "");
                    }

                    // 模拟 thinking 模型: 先推送 reasoning_content 增量 (TYPE_THINKING)
                    if (!g_da_sim_reasoning_content.empty()) {
                        auto d                 = neograph::json::object();
                        d["reasoning_content"] = g_da_sim_reasoning_content;
                        append(d, "");
                    }

                    if (hasToolCalls) {
                        auto d          = neograph::json::object();
                        d["tool_calls"] = g_da_sim_tool_calls;
                        append(d, "");
                        append(neograph::json::object(), "tool_calls");
                    } else {
                        const auto& content = respContent;
                        std::string acc;
                        for (size_t i = 0; i < content.size(); ++i) {
                            acc += content[i];
                            if (content[i] == ' ' || acc.size() >= 10 || i == content.size() - 1) {
                                auto d       = neograph::json::object();
                                d["content"] = acc;
                                append(d, "");
                                acc.clear();
                            }
                        }
                        if (!acc.empty()) {
                            auto d       = neograph::json::object();
                            d["content"] = acc;
                            append(d, "");
                        }
                        append(neograph::json::object(), "stop");
                    }

                    sseBody += "data: [DONE]\n\n";

                    resp.result(http::status::ok);
                    resp.set(http::field::content_type, "text/event-stream");
                    resp.set(http::field::cache_control, "no-cache");
                    resp.body() = std::move(sseBody);
                    resp.prepare_payload();
                } else {
                    auto msg    = neograph::json::object();
                    msg["role"] = "assistant";
                    if (!g_da_sim_reasoning_content.empty()) {
                        msg["reasoning_content"] = g_da_sim_reasoning_content;
                    }
                    if (hasToolCalls) {
                        msg["content"]    = nullptr;
                        msg["tool_calls"] = g_da_sim_tool_calls;
                    } else {
                        msg["content"] = respContent;
                    }

                    auto choice             = neograph::json::object();
                    choice["index"]         = 0;
                    choice["message"]       = msg;
                    choice["finish_reason"] = hasToolCalls ? "tool_calls" : "stop";

                    auto usage                 = neograph::json::object();
                    usage["prompt_tokens"]     = g_da_sim_prompt_tokens;
                    usage["completion_tokens"] = g_da_sim_completion_tokens;
                    usage["total_tokens"] = g_da_sim_prompt_tokens + g_da_sim_completion_tokens;

                    auto respBody       = neograph::json::object();
                    respBody["id"]      = "chatcmpl-test-sim";
                    respBody["object"]  = "chat.completion";
                    respBody["created"] = 1234567890;
                    respBody["model"]   = "test-sim";
                    respBody["choices"] = neograph::json::array({choice});
                    respBody["usage"]   = usage;

                    resp.result(http::status::ok);
                    resp.set(http::field::content_type, "application/json");
                    resp.body() = respBody.dump();
                    resp.prepare_payload();
                }

                g_da_sim_tool_calls        = neograph::json::array();
                g_da_sim_reasoning_content = "";
                co_return;
            }
        )
    );

    sim.thr = std::thread([rawSvr]() {
        rawSvr->start();
    });

    for (int i = 0; i < 100; ++i) {
        sim.port = rawSvr->port();
        if (sim.port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return sim;
}

asio::awaitable<void> test_agent_init() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg             = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl   = baseUrl;
    cfg->model.apiKey    = "EMPTY";
    cfg->model.modelName = "test-sim";

    agentxx::agent::CodeAgent agent(cfg);
    co_await agent.init();

    XX_TEST_EXPECT_TRUE(agent.engine != nullptr);
    XX_TEST_EXPECT_TRUE(agent.agentContext != nullptr);

    // collectAppendComponentInfo: init 后插件管理器已装配 (无配置插件时列表为空),
    // 收集结果应包含已加载插件 (数量 + 名称列表), 与 MCP/Skill/Memory 同级。
    // protected 成员经测试子类暴露访问。
    class AppendCollectAgent : public agentxx::agent::CodeAgent {
    public:

        using CodeAgent::CodeAgent;

        void collect(std::vector<agentxx::agent::AppendComponentNotification>& out) {
            collectAppendComponentInfo(out);
        }
    };

    AppendCollectAgent collectAgent(cfg);
    co_await collectAgent.init();
    XX_TEST_EXPECT_TRUE(collectAgent.agentContext->pluginManager != nullptr);
    std::vector<agentxx::agent::AppendComponentNotification> notifications;
    collectAgent.collect(notifications);
    size_t pluginCount = 0;
    for (const auto& n : notifications) {
        if (n.type == agentxx::agent::AppendComponentNotification::Type::Plugin) {
            ++pluginCount;
        }
    }
    XX_TEST_EXPECT_EQ(pluginCount, collectAgent.agentContext->pluginManager->list().size());

    co_return;
}

/// 权限模式规则集成测试:
/// CodeAgent 按 yaml 配置的 permission_mode 与白/黑名单注册文件系统读写规则,
/// 验证各模式下路径命中行为 (cwd 内允许/询问/拒绝/白名单放行/黑名单拒绝)。
/// 会话总线挂 PermissionTestIO 模拟客户端询问应答 (计数 + 允许)。
asio::awaitable<void> test_agent_permission_mode_rules() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfgBase             = std::make_shared<agentxx::agent::AgentConfig>();
    cfgBase->model.baseUrl   = baseUrl;
    cfgBase->model.apiKey    = "EMPTY";
    cfgBase->model.modelName = "test-sim";

    // 路径集合 (与中间件归一化口径一致: 相对/绝对均基于 cwd 解析)
    // 注: 路由器通配符仅支持整段 "*", 白/黑名单目录按最长前缀回退匹配其子路径
    const std::string cwd         = std::filesystem::current_path().generic_string();
    const std::string insidePath  = cwd + "/inside.txt";
    const std::string trustedPath = cwd + "/trusted/trusted.txt";
    const std::string secretPath  = cwd + "/secret/secret.txt";
    const std::string outsidePath = "/data/outside.txt";

    // 工具 + 会话总线 + 权限应答 IO (每次构造新 CodeAgent 前重建, 保证计数独立)
    TestTool         tool("agentxx_filesystem_write");
    constexpr size_t kWriteIndex
        = agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE;

    // 检查辅助: 走权限中间件判定指定路径的写权限
    auto check = [&](std::shared_ptr<agentxx::middleware::PermissionMiddlewareHandle> perm,
                     std::string_view                                                 path,
                     const std::string& sessionId) -> asio::awaitable<bool> {
        auto args = neograph::json{
            {"path",      std::string{path}},
            {"sessionId", sessionId        }
        };
        co_return co_await perm->defOnFilesystemHandle(tool, args, kWriteIndex);
    };

    // ---- 模式 ask: 工作目录内允许, 其他询问 ----
    {
        auto cfg                  = std::make_shared<agentxx::agent::AgentConfig>(*cfgBase);
        cfg->permissionMode       = agent::PermissionMode::Ask;
        cfg->permissionAllowPaths = {cwd + "/trusted"};
        cfg->permissionDenyPaths  = {cwd + "/secret"};

        agentxx::agent::CodeAgent agent(cfg);
        co_await agent.init();
        auto perm = agent.agentContext->permissionMiddleware;
        XX_TEST_EXPECT_TRUE(perm != nullptr);

        auto session = agent.agentContext->getSession("perm_ask");
        auto bus = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);
        auto io  = std::make_shared<PermissionTestIO>();
        io->registerOnBus(bus);
        session->bus = bus;

        // 工作目录内: 直接允许, 不询问
        bool ok = co_await check(perm, insidePath, "perm_ask");
        XX_TEST_EXPECT_TRUE(ok);
        XX_TEST_EXPECT_EQ(io->permissionCalls.load(), 0);
#if XX_IS_WIN_D
        // Windows 文件系统大小写不敏感: 请求路径与注册路径大小写不同
        // (模型可能传任意大小写, 如 d:/... 或 D:\...) 也应放行, 不询问
        std::string mixedCasePath = insidePath;
        for (auto& c : mixedCasePath) {
            if (c >= 'a' && c <= 'z') {
                c = static_cast<char>(c - 'a' + 'A');
            } else if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        if (mixedCasePath != insidePath) {
            ok = co_await check(perm, mixedCasePath, "perm_ask");
            XX_TEST_EXPECT_TRUE(ok);
            XX_TEST_EXPECT_EQ(io->permissionCalls.load(), 0);
        }
#endif
        // 白名单 (工作目录内 trusted 子目录): 允许, 不询问
        ok = co_await check(perm, trustedPath, "perm_ask");
        XX_TEST_EXPECT_TRUE(ok);
        XX_TEST_EXPECT_EQ(io->permissionCalls.load(), 0);
        // 黑名单 (工作目录内 secret 子目录): 拒绝, 不询问 (黑名单优先于模式 ALLOW)
        ok = co_await check(perm, secretPath, "perm_ask");
        XX_TEST_EXPECT_FALSE(ok);
        XX_TEST_EXPECT_EQ(io->permissionCalls.load(), 0);
        // 工作目录外: 询问 (prompter 允许 → 放行)
        ok = co_await check(perm, outsidePath, "perm_ask");
        XX_TEST_EXPECT_TRUE(ok);
        XX_TEST_EXPECT_EQ(io->permissionCalls.load(), 1);
    }

    // ---- 模式 all_ask: 所有路径均询问 ----
    {
        auto cfg            = std::make_shared<agentxx::agent::AgentConfig>(*cfgBase);
        cfg->permissionMode = agent::PermissionMode::AllAsk;

        agentxx::agent::CodeAgent agent(cfg);
        co_await agent.init();
        auto perm = agent.agentContext->permissionMiddleware;

        auto session = agent.agentContext->getSession("perm_allask");
        auto bus = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);
        auto io  = std::make_shared<PermissionTestIO>();
        io->registerOnBus(bus);
        session->bus = bus;

        // 工作目录内/外均询问
        bool ok = co_await check(perm, insidePath, "perm_allask");
        XX_TEST_EXPECT_TRUE(ok);
        ok = co_await check(perm, outsidePath, "perm_allask");
        XX_TEST_EXPECT_TRUE(ok);
        XX_TEST_EXPECT_EQ(io->permissionCalls.load(), 2);
    }

    // ---- 模式 pass: 全部放行, 不询问 ----
    {
        auto cfg            = std::make_shared<agentxx::agent::AgentConfig>(*cfgBase);
        cfg->permissionMode = agent::PermissionMode::Pass;

        agentxx::agent::CodeAgent agent(cfg);
        co_await agent.init();
        auto perm = agent.agentContext->permissionMiddleware;

        auto session = agent.agentContext->getSession("perm_pass");
        auto bus = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);
        auto io  = std::make_shared<PermissionTestIO>();
        io->registerOnBus(bus);
        session->bus = bus;

        bool ok = co_await check(perm, outsidePath, "perm_pass");
        XX_TEST_EXPECT_TRUE(ok);
        ok = co_await check(perm, secretPath, "perm_pass");
        XX_TEST_EXPECT_TRUE(ok);
        XX_TEST_EXPECT_EQ(io->permissionCalls.load(), 0);
    }

    // ---- 模式 deny: 全部拒绝, 不询问; 白名单路径仍放行 ----
    {
        auto cfg                  = std::make_shared<agentxx::agent::AgentConfig>(*cfgBase);
        cfg->permissionMode       = agent::PermissionMode::Deny;
        cfg->permissionAllowPaths = {cwd + "/trusted"};

        agentxx::agent::CodeAgent agent(cfg);
        co_await agent.init();
        auto perm = agent.agentContext->permissionMiddleware;

        auto session = agent.agentContext->getSession("perm_deny");
        auto bus = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);
        auto io  = std::make_shared<PermissionTestIO>();
        io->registerOnBus(bus);
        session->bus = bus;

        // 工作目录内/外均拒绝, 不询问
        bool ok = co_await check(perm, insidePath, "perm_deny");
        XX_TEST_EXPECT_FALSE(ok);
        ok = co_await check(perm, outsidePath, "perm_deny");
        XX_TEST_EXPECT_FALSE(ok);
        XX_TEST_EXPECT_EQ(io->permissionCalls.load(), 0);
        // 白名单路径: deny 模式下仍放行 (白名单优先于模式默认规则)
        ok = co_await check(perm, trustedPath, "perm_deny");
        XX_TEST_EXPECT_TRUE(ok);
        XX_TEST_EXPECT_EQ(io->permissionCalls.load(), 0);
    }

    co_return;
}

asio::awaitable<void> test_agent_single_input() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                 = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl       = baseUrl;
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "test-sim";
    cfg->prompt.systemPrompt = "You are a helpful assistant.";

    g_da_sim_response_content = "This is the test response content.";
    g_da_sim_tool_calls       = neograph::json::array();

    agentxx::agent::CodeAgent agent(cfg);
    co_await agent.init();

    auto result = co_await agent.runSingleInputAsync("test_thread", "Hello");

    XX_TEST_EXPECT_FALSE(result.empty());
    XX_TEST_EXPECT_TRUE(result.find("test response") != std::string::npos);

    co_return;
}

asio::awaitable<void> test_agent_conversation_turn() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                 = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl       = baseUrl;
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "test-sim";
    cfg->prompt.systemPrompt = "You are a helpful assistant.";

    g_da_sim_response_content = "Hello from the simulated LLM!";
    g_da_sim_tool_calls       = neograph::json::array();

    agentxx::agent::CodeAgent agent(cfg);
    co_await agent.init();

    auto result = co_await agent.runTurnAsync("conv_test", "What is the weather?", nullptr);

    XX_TEST_EXPECT_FALSE(result.hasError);
    XX_TEST_EXPECT_FALSE(result.interrupted);

    co_return;
}

asio::awaitable<void> test_agent_tool_calls() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                 = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl       = baseUrl;
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "test-sim";
    cfg->prompt.systemPrompt = "You are a helpful assistant.";

    g_da_sim_response_content = "";
    g_da_sim_tool_calls       = neograph::json::array({
        neograph::json{
                       {"index", 0},
                       {"id", "call_test_1"},
                       {"type", "function"},
                       {"function",
                   neograph::json{
                       {"name", "agentxx_filesystem_list"},
                       {"arguments", "{}"},
             }},
                       },
    });

    agentxx::agent::CodeAgent agent(cfg);
    co_await agent.init();

    auto result = co_await agent.runTurnAsync("tool_test", "List files", nullptr);

    XX_TEST_EXPECT_FALSE(result.hasError);

    co_return;
}

asio::awaitable<void> test_agent_multi_turn() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                 = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl       = baseUrl;
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "test-sim";
    cfg->prompt.systemPrompt = "You are a helpful assistant.";

    g_da_sim_response_content = "Response for turn ";
    g_da_sim_tool_calls       = neograph::json::array();

    agentxx::agent::CodeAgent agent(cfg);
    co_await agent.init();

    for (int turn = 0; turn < 3; ++turn) {
        auto input  = "Turn " + std::to_string(turn) + " input";
        auto result = co_await agent.runTurnAsync("multi_turn_test", input, nullptr);

        XX_TEST_EXPECT_FALSE(result.hasError);
        XX_TEST_EXPECT_FALSE(result.interrupted);
    }

    co_return;
}

asio::awaitable<void> test_agent_large_history() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                 = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl       = baseUrl;
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "test-sim";
    cfg->prompt.systemPrompt = "You are a helpful assistant.";

    g_da_sim_response_content = "Final response after long history.";
    g_da_sim_tool_calls       = neograph::json::array();

    agentxx::agent::CodeAgent agent(cfg);
    co_await agent.init();

    auto result = co_await agent.runTurnAsync("history_test", "Final question", nullptr);

    XX_TEST_EXPECT_FALSE(result.hasError);

    co_return;
}

asio::awaitable<void> test_agent_nonstream() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg             = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl   = baseUrl;
    cfg->model.apiKey    = "EMPTY";
    cfg->model.modelName = "test-sim";

    g_da_sim_response_content = "Non-stream test response.";
    g_da_sim_tool_calls       = neograph::json::array();

    agentxx::agent::CodeAgent agent(cfg);
    co_await agent.init();

    std::vector<neograph::ChatMessage> msgs;
    msgs.push_back(neograph::ChatMessage{
        .role    = "system",
        .content = "You are helpful.",
    });
    msgs.push_back(neograph::ChatMessage{
        .role    = "user",
        .content = "Hello",
    });

    auto result = co_await agent.runOverMsgsTurnAsync("nonstream_test", msgs);

    XX_TEST_EXPECT_FALSE(result.empty());
    XX_TEST_EXPECT_TRUE(result.find("Non-stream") != std::string::npos);

    co_return;
}

/// dataDir 未配置时: 会话持久化自动禁用 (仅存内存), 不落盘;
/// - dataDir 为空 + root 为空 → 不创建 SessionStore
/// - dataDir 非空 → 创建到 {dataDir}/sqlite/sessions/
/// - dataDir 为空但显式指定 sessionStoreDirectory → 仍持久化 (显式路径)
asio::awaitable<void> test_agent_persistence_datadir_gate() {
    auto makeCfg = [](std::string dataDir, std::string root) {
        auto cfg                   = std::make_shared<agentxx::agent::AgentConfig>();
        cfg->model.baseUrl         = "http://127.0.0.1:1";
        cfg->model.apiKey          = "EMPTY";
        cfg->model.modelName       = "test-sim";
        cfg->enableSessionStore    = true;
        cfg->dataDir               = std::move(dataDir);
        cfg->sessionStoreDirectory = std::move(root);
        return cfg;
    };

    // 1) dataDir 为空 + root 为空 → 不创建持久化 (内存模式)
    {
        agentxx::agent::CodeAgent agent(makeCfg("", ""));
        XX_TEST_EXPECT_TRUE(agent.agentContext->sessions->sessionStore == nullptr);
    }
    // 2) dataDir 非空 → 创建持久化到 {dataDir}/sqlite/sessions/
    {
        agentxx::agent::CodeAgent agent(makeCfg("/tmp/agentxx-test-data", ""));
        XX_TEST_EXPECT_TRUE(agent.agentContext->sessions->sessionStore != nullptr);
    }
    // 3) dataDir 为空但显式指定 sessionStoreDirectory → 仍持久化 (显式路径)
    {
        agentxx::agent::CodeAgent agent(makeCfg("", "/tmp/agentxx-test-sessions"));
        XX_TEST_EXPECT_TRUE(agent.agentContext->sessions->sessionStore != nullptr);
    }

    co_return;
}

asio::awaitable<void> test_agent_io_session_bus() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                  = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl        = baseUrl;
    cfg->model.apiKey         = "EMPTY";
    cfg->model.modelName      = "test-sim";
    cfg->prompt.systemPrompt  = "You are a helpful assistant.";
    g_da_sim_response_content = "Hello from IO session bus test!";
    g_da_sim_tool_calls       = neograph::json::array();

    agentxx::agent::CodeAgent agent(cfg);
    co_await agent.init();

    auto io = std::make_shared<TestAgentIO>();
    // 首次调用, 应创建 session bus 并注册 IO
    auto result = co_await agent.runTurnAsync("io_session_test", "Hello", io);

    XX_TEST_EXPECT_FALSE(result.hasError);
    // session bus 应已创建
    auto session = agent.agentContext->sessions->get("io_session_test");
    XX_TEST_EXPECT_TRUE(session != nullptr);
    XX_TEST_EXPECT_TRUE(session->bus != nullptr);
    // IO 应已注册到 session
    XX_TEST_EXPECT_TRUE(session->io != nullptr);

    // turn 开始时应经 sendToPeer 产出 TurnStart delta
    XX_TEST_EXPECT_TRUE(io->deltaCount > 0);

    co_return;
}

/// 轮次统计系统提示由 agent 线程插入:
/// - 带 io 运行一轮后, viewMessages 末尾应包含 System 消息 (轮次统计, 含
///   模型名 / tps / 时长)
/// - io 应收到 MessageTip WireDelta, 其 msgId 与 viewMessages 中消息 id 一致
asio::awaitable<void> test_agent_turn_system_message() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                  = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl        = baseUrl;
    cfg->model.apiKey         = "EMPTY";
    cfg->model.modelName      = "test-sim";
    cfg->prompt.systemPrompt  = "You are a helpful assistant.";
    g_da_sim_response_content = "System message test!";
    g_da_sim_tool_calls       = neograph::json::array();

    agentxx::agent::CodeAgent agent(cfg);
    co_await agent.init();

    auto io     = std::make_shared<TestAgentIO>();
    auto result = co_await agent.runTurnAsync("sysmsg_test", "Hello", io);
    XX_TEST_EXPECT_FALSE(result.hasError);

    auto session = agent.agentContext->sessions->get("sysmsg_test");
    XX_TEST_EXPECT_TRUE(session != nullptr);

    // viewMessages 应包含 Tip 轮次统计消息 (模型名 · t/s · 时长 · 时间)
    bool        foundStat = false;
    std::string statMsgId;
    for (const auto& vm : session->viewMessages) {
        if (vm.role == agentxx::agent::ViewMessage::Role::Tip) {
            foundStat = true;
            statMsgId = vm.id;
            XX_TEST_EXPECT_TRUE(vm.text.find("test-sim") != std::string::npos);
            XX_TEST_EXPECT_TRUE(vm.text.find("t/s") != std::string::npos);
            XX_TEST_EXPECT_TRUE(vm.text.find("·") != std::string::npos);
            XX_TEST_EXPECT_TRUE(vm.durationMs > 0);
        }
    }
    XX_TEST_EXPECT_TRUE(foundStat);

    // io 应收到 InsertMessage WireDelta, 携带完整 ViewMessage, msgId 与历史消息一致
    bool foundDelta = false;
    for (const auto& d : io->deltas) {
        if (d.type == agentxx::agent::WireDelta::Type::InsertMessage && d.message) {
            foundDelta = true;
            XX_TEST_EXPECT_EQ(d.message->id, statMsgId);
            XX_TEST_EXPECT_TRUE(!d.message->text.empty());
            XX_TEST_EXPECT_EQ(d.message->role, agentxx::agent::ViewMessage::Role::Tip);
        }
    }
    XX_TEST_EXPECT_TRUE(foundDelta);

    co_return;
}

asio::awaitable<void> test_agent_io_null() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                  = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl        = baseUrl;
    cfg->model.apiKey         = "EMPTY";
    cfg->model.modelName      = "test-sim";
    g_da_sim_response_content = "Null IO test.";
    g_da_sim_tool_calls       = neograph::json::array();

    agentxx::agent::CodeAgent agent(cfg);
    co_await agent.init();

    // 传入 nullptr IO, 验证不崩溃
    auto result = co_await agent.runTurnAsync("null_io_test", "test", nullptr);

    XX_TEST_EXPECT_FALSE(result.hasError);

    co_return;
}

asio::awaitable<void> test_agent_session_activity_streaming() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                  = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl        = baseUrl;
    cfg->model.apiKey         = "EMPTY";
    cfg->model.modelName      = "test-sim";
    g_da_sim_response_content = "Activity check response.";
    g_da_sim_tool_calls       = neograph::json::array();

    agentxx::agent::CodeAgent agent(cfg);
    co_await agent.init();

    auto io     = std::make_shared<TestAgentIO>();
    auto result = co_await agent.runTurnAsync("activity_stream_test", "Check", io);

    XX_TEST_EXPECT_FALSE(result.hasError);
    auto session = agent.agentContext->sessions->get("activity_stream_test");
    XX_TEST_EXPECT_TRUE(session != nullptr);
    // 流结束后 activity 应为 Idle
    XX_TEST_EXPECT_TRUE(session->activity == agentxx::agent::SessionActivity::Idle);

    co_return;
}

asio::awaitable<void> test_agent_session_activity_toolcall() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                  = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl        = baseUrl;
    cfg->model.apiKey         = "EMPTY";
    cfg->model.modelName      = "test-sim";
    g_da_sim_response_content = "";
    g_da_sim_tool_calls       = neograph::json::array({
        neograph::json{
                       {"index", 0},
                       {"id", "call_act_1"},
                       {"type", "function"},
                       {"function",
                   neograph::json{
                       {"name", "agentxx_filesystem_list"},
                       {"arguments", "{}"},
             }},
                       },
    });

    agentxx::agent::CodeAgent agent(cfg);
    co_await agent.init();

    auto io     = std::make_shared<TestAgentIO>();
    auto result = co_await agent.runTurnAsync("activity_tool_test", "List", io);

    XX_TEST_EXPECT_FALSE(result.hasError);
    auto session = agent.agentContext->sessions->get("activity_tool_test");
    // tool 执行结束后 activity 恢复 Idle
    XX_TEST_EXPECT_TRUE(session->activity == agentxx::agent::SessionActivity::Idle);

    co_return;
}

asio::awaitable<void> test_agent_multi_session_io() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                  = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl        = baseUrl;
    cfg->model.apiKey         = "EMPTY";
    cfg->model.modelName      = "test-sim";
    g_da_sim_response_content = "Multi-session response.";
    g_da_sim_tool_calls       = neograph::json::array();

    agentxx::agent::CodeAgent agent(cfg);
    co_await agent.init();

    auto ioA = std::make_shared<TestAgentIO>();
    auto ioB = std::make_shared<TestAgentIO>();

    auto resA = co_await agent.runTurnAsync("session_a", "Hello A", ioA);
    XX_TEST_EXPECT_FALSE(resA.hasError);

    auto resB = co_await agent.runTurnAsync("session_b", "Hello B", ioB);
    XX_TEST_EXPECT_FALSE(resB.hasError);

    // 两个 session 应独立, 都有自己的 bus
    auto sA = agent.agentContext->sessions->get("session_a");
    auto sB = agent.agentContext->sessions->get("session_b");
    XX_TEST_EXPECT_TRUE(sA->bus != nullptr);
    XX_TEST_EXPECT_TRUE(sB->bus != nullptr);
    XX_TEST_EXPECT_TRUE(sA->bus != sB->bus); // 不同 session 不同 bus
    XX_TEST_EXPECT_TRUE(sA->io != nullptr);
    XX_TEST_EXPECT_TRUE(sB->io != nullptr);
    // 各自 activity 独立
    XX_TEST_EXPECT_TRUE(sA->activity == agentxx::agent::SessionActivity::Idle);
    XX_TEST_EXPECT_TRUE(sB->activity == agentxx::agent::SessionActivity::Idle);

    co_return;
}

asio::awaitable<void> test_agent_reuse_session_bus() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                  = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl        = baseUrl;
    cfg->model.apiKey         = "EMPTY";
    cfg->model.modelName      = "test-sim";
    g_da_sim_response_content = "Reuse session bus test.";
    g_da_sim_tool_calls       = neograph::json::array();

    agentxx::agent::CodeAgent agent(cfg);
    co_await agent.init();

    auto io = std::make_shared<TestAgentIO>();

    // 多轮: 同一 session, bus 应只创建一次
    auto r1 = co_await agent.runTurnAsync("reuse_test", "Turn 1", io);
    XX_TEST_EXPECT_FALSE(r1.hasError);

    auto session = agent.agentContext->sessions->get("reuse_test");
    auto busPtr  = session->bus.get();

    auto r2 = co_await agent.runTurnAsync("reuse_test", "Turn 2", io);
    XX_TEST_EXPECT_FALSE(r2.hasError);

    // 同一 session 应复用同一个 bus (指针不变)
    XX_TEST_EXPECT_TRUE(session->bus.get() == busPtr);

    co_return;
}

/// LLM API 持续失败时, ModelCall 重试耗尽后应重抛异常停止本轮执行,
/// 而不是: 吞掉异常 -> 节点假装成功返回空输出 -> [has_tool_calls] 误路由 ->
/// 重复执行最后一次 toolcall 并插入重复结果 -> llm<->tools 无限循环 (消息无限堆积)
asio::awaitable<void> test_agent_llm_retry_exhaust() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                 = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl       = baseUrl;
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "test-sim";
    cfg->prompt.systemPrompt = "You are a helpful assistant.";
    // 重试 1 次即停止, 缩短测试等待时间 (退避 3 秒)
    cfg->llmMaxRetry = 1;

    g_da_sim_request_count = 0;
    g_da_sim_fail_count    = 0;

    agentxx::agent::CodeAgent agent(cfg);
    co_await agent.init();

    // ---- 第一轮: llm 返回 tool_calls, tools 执行一次 ----
    g_da_sim_response_content = "";
    g_da_sim_tool_calls       = neograph::json::array({
        neograph::json{
                       {"index", 0},
                       {"id", "call_retry_1"},
                       {"type", "function"},
                       {"function",
                   neograph::json{
                       {"name", "agentxx_filesystem_list"},
                       {"arguments", "{}"},
             }},
                       },
    });

    auto r1 = co_await agent.runTurnAsync("retry_test", "List files", nullptr);
    XX_TEST_EXPECT_FALSE(r1.hasError);
    // 2 次请求: tool_calls 请求 + tools 执行后回 llm 的收尾请求
    XX_TEST_EXPECT_EQ(g_da_sim_request_count, 2);

    // ---- 第二轮: llm 持续失败 (重试耗尽后应结束本轮) ----
    g_da_sim_response_content = "fallback text"; // bug 场景下第 4 次请求会成功返回此文本
    g_da_sim_tool_calls = neograph::json::array();
    // 接下来 2 次请求返回 500: 第 1 次失败 + 1 次重试失败
    g_da_sim_fail_count = 2;

    auto r2 = co_await agent.runTurnAsync("retry_test", "Continue", nullptr);

    // 重试耗尽后停止会话执行 (重抛 -> base_agent 报告错误): 共 4 次请求
    // (第一轮 2 次 + 本轮 2 次失败), 不再继续请求
    XX_TEST_EXPECT_EQ(g_da_sim_request_count, 4);
    // 重试耗尽 -> 会话以错误结束 (停止执行), 而不是无限循环
    XX_TEST_EXPECT_TRUE(r2.hasError);

    // 末尾消息为 assistant 且包含失败提示
    auto session = agent.agentContext->sessions->get("retry_test");
    XX_TEST_EXPECT_TRUE(session != nullptr);
    auto msgs = session->llmMessages;
    XX_TEST_EXPECT_TRUE(msgs.is_array());
    const auto& last = msgs.back();
    XX_TEST_EXPECT_TRUE(last["role"] == "assistant");
    XX_TEST_EXPECT_TRUE(last["content"].dump().find("[Exception aborted]") != std::string::npos);

    // toolcall 只执行了一次: 整个上下文中的 tool 结果消息数 == 1
    int toolResultCount = 0;
    for (const auto& m : msgs) {
        if (m["role"] == "tool") {
            toolResultCount++;
        }
    }
    XX_TEST_EXPECT_EQ(toolResultCount, 1);

    co_return;
}

/// Toolcall 节点拦截普通异常: 中间件 onToolcallStart 抛异常时,
/// 插入 [Start/Exception aborted] 错误消息并继续调度 (agent 基于错误继续运行),
/// 而不是终止会话; 取消/中断 仍重抛由 base_agent 控制流处理
asio::awaitable<void> test_agent_toolcall_intercept_exception() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                 = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl       = baseUrl;
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "test-sim";
    cfg->prompt.systemPrompt = "You are a helpful assistant.";

    g_da_sim_request_count    = 0;
    g_da_sim_fail_count       = 0;
    g_da_sim_response_content = "Final answer after tool error.";
    g_da_sim_tool_calls       = neograph::json::array({
        neograph::json{
                       {"index", 0},
                       {"id", "call_intercept_1"},
                       {"type", "function"},
                       {"function",
                   neograph::json{
                       {"name", "agentxx_filesystem_list"},
                       {"arguments", "{}"},
             }},
                       },
    });

    agentxx::agent::CodeAgent agent(cfg);
    co_await agent.init();

    // init 后插入抛异常中间件 (不带工具, 无需在 init 前注册)
    agent.agentContext->middlewareHandleContext->handles.push_back(
        std::make_shared<ThrowToolcallStartMiddleware>(agent.agentContext)
    );

    auto result = co_await agent.runTurnAsync("intercept_test", "Run tool", nullptr);

    // toolcall 拦截异常 -> agent 继续运行 -> 会话正常结束 (非错误/非中断)
    XX_TEST_EXPECT_FALSE(result.hasError);
    XX_TEST_EXPECT_FALSE(result.interrupted);
    // 2 次请求: tool_calls 请求 + 拦截后回 llm 的收尾请求
    XX_TEST_EXPECT_EQ(g_da_sim_request_count, 2);

    // 上下文包含 [Start/Exception aborted] 错误消息 + 末尾为最终回答
    auto session = agent.agentContext->sessions->get("intercept_test");
    XX_TEST_EXPECT_TRUE(session != nullptr);
    auto msgs = session->llmMessages;
    XX_TEST_EXPECT_TRUE(msgs.is_array());
    bool hasErrorMsg = false;
    for (const auto& m : msgs) {
        // 匹配 "[Start/Exception aborted" 前缀: 实际插入格式为
        // "[Start/Exception aborted: {exceptionStr}]" (带错误详情)
        if (m["role"] == "tool"
            && m["content"].dump().find("[Start/Exception aborted") != std::string::npos) {
            hasErrorMsg = true;
        }
    }
    XX_TEST_EXPECT_TRUE(hasErrorMsg);
    const auto& last = msgs.back();
    XX_TEST_EXPECT_TRUE(last["role"] == "assistant");
    XX_TEST_EXPECT_TRUE(last["content"].dump().find("Final answer") != std::string::npos);

    co_return;
}

asio::awaitable<TestResult> run_agent_tests() {
    // 注: 计数器重构后 g_da_* 为本编译单元匿名命名空间私有变量,
    // 其他模块 (test_agent_host/session_persistence/remote_agent 等) 的断言
    // 不再可能经头文件宏泄漏计入此处, 无需在此清零防御

    try {
        co_await test_agent_init();
        co_await test_agent_permission_mode_rules();
        co_await test_agent_single_input();
        co_await test_agent_conversation_turn();
        co_await test_agent_tool_calls();
        co_await test_agent_multi_turn();
        co_await test_agent_large_history();
        co_await test_agent_nonstream();
        co_await test_agent_persistence_datadir_gate();
        co_await test_agent_io_session_bus();
        co_await test_agent_turn_system_message();
        co_await test_agent_io_null();
        co_await test_agent_session_activity_streaming();
        co_await test_agent_session_activity_toolcall();
        co_await test_agent_multi_session_io();
        co_await test_agent_reuse_session_bus();
        co_await test_agent_llm_retry_exhaust();
        co_await test_agent_toolcall_intercept_exception();
    } catch (const std::exception& e) {
        TEST_FAIL << "agent suite exception: " << e.what() << std::endl;
        g_da_failed++;
    }

    co_return TestResult{g_da_passed, g_da_failed};
}

} // namespace test
} // namespace agentxx
