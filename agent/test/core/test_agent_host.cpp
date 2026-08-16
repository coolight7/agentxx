#include "test_agent.h"
#include "test_agent_host.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/middlewares/events.h"
#include "agentxx/protocol/a2a_client.h"
#include "agentxx/protocol/a2a_server.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include <atomic>
#include <expected>
#include <memory>
#include <string>
#include <thread>

namespace agentxx {
namespace test {

int g_host_passed = 0;
int g_host_failed = 0;

namespace {

std::shared_ptr<agentxx::agent::AgentConfig> makeSimConfig(std::string_view baseUrl) {
    auto cfg                 = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl       = std::string{baseUrl};
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "test-sim";
    cfg->prompt.systemPrompt = "You are a helpful assistant.";
    // 测试内不触发权限询问
    cfg->permissionMode      = agentxx::agent::PermissionMode::Pass;
    return cfg;
}

/// 驱动 io 直到 done 置位:
/// - 子代理运行期间存在外部异步等待 (mock LLM HTTP 在独立线程),
///   此时 io 事件队列可能为空, io_context::run() 会提前返回;
///   以 5ms 定时器泵送保活, 保证在飞行协程全部完成前测试帧不被销毁
void pumpIoUntil(std::shared_ptr<asio::io_context> io, const std::atomic<bool>& done) {
    while (!done.load()) {
        asio::steady_timer t(*io);
        t.expires_after(std::chrono::milliseconds(5));
        t.async_wait([](const neograph_asio_error_code&) {});
        io->run();
    }
    // 排空完成信号之后遗留的发布 (progress/done 事件等)
    io->run();
}

} // namespace

/// 验证: AgentRegistry 基本操作 (insert/get/remove/childrenOf)
asio::awaitable<void> test_host_registry() {
    agentxx::agent::AgentRegistry reg;

    auto makeNode = [](std::string id, std::string parent) {
        auto n             = std::make_shared<agentxx::agent::AgentNode>();
        n->agentId         = id;
        n->parentAgentId   = parent;
        return n;
    };

    XX_TEST_EXPECT_EQ(reg.size(), 0u);
    reg.insert(makeNode("root", ""));
    reg.insert(makeNode("a", "root"));
    reg.insert(makeNode("b", "root"));
    reg.insert(makeNode("a1", "a"));
    XX_TEST_EXPECT_EQ(reg.size(), 4u);
    XX_TEST_EXPECT_TRUE(reg.contains("a"));
    XX_TEST_EXPECT_FALSE(reg.contains("nope"));
    XX_TEST_EXPECT_TRUE(reg.get("b") != nullptr);

    auto kids = reg.childrenOf("root");
    XX_TEST_EXPECT_EQ(kids.size(), 2u);
    auto grandKids = reg.childrenOf("a");
    XX_TEST_EXPECT_EQ(grandKids.size(), 1u);
    XX_TEST_EXPECT_EQ(grandKids[0]->agentId, std::string{"a1"});

    reg.remove("b");
    XX_TEST_EXPECT_EQ(reg.size(), 3u);
    reg.clear();
    XX_TEST_EXPECT_EQ(reg.size(), 0u);

    co_return;
}

/// 验证: 宿主派生子代理端到端 (根 agent 总线 service.subagent → 宿主 → 独立 agent)
/// - 子代理使用独立 AgentContext (配置为轻量: 不建 MCP 连接)
/// - 输出 = 模拟 LLM 响应; 子代理节点运行后回收 (registry 仅剩 root)
/// - agent.progress / agent.done 事件发布
asio::awaitable<void> test_host_spawn_e2e() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);
    auto cfg     = makeSimConfig(baseUrl);

    g_da_sim_response_content = "Host spawn result";
    g_da_sim_tool_calls       = neograph::json::array();
    g_da_sim_delay_ms         = 0;

    auto io = std::make_shared<asio::io_context>();
    agentxx::agent::AgentHost::Config hostCfg;
    hostCfg.ioCtx = io;
    auto host     = agentxx::agent::AgentHost::create(hostCfg);
    std::shared_ptr<agentxx::agent::CodeAgent> agent;
    std::expected<agentxx::events::RespSubagentResult, std::string> resp;
    std::atomic<int> progressCount{0};
    std::atomic<int> doneCount{0};
    std::atomic<bool> finished{false};

    host->hostBus()
        ->get<agentxx::events::EventHostProgress>(agentxx::events::HostTopic::AgentProgress)
        .subscribe([&](const agentxx::events::EventHostProgress& e) -> asio::awaitable<void> {
            if (e.kind == "token") {
                progressCount++;
            }
            co_return;
        });
    host->hostBus()
        ->get<agentxx::events::EventHostDone>(agentxx::events::HostTopic::AgentDone)
        .subscribe([&](const agentxx::events::EventHostDone&) -> asio::awaitable<void> {
            doneCount++;
            co_return;
        });

    asio::co_spawn(
        *io,
        [&]() -> asio::awaitable<void> {
            agent = std::make_shared<agentxx::agent::CodeAgent>(cfg);
            co_await agent->init();
            host->attachRoot(agent);
            // 父会话: 供子代理继承 bus (HIL 冒泡路径)
            auto parentSession = agent->getContext()->getSession("parent-session");
            parentSession->bus = std::make_shared<agentxx::middleware::EventBus>(
                co_await asio::this_coro::executor
            );

            resp = co_await agent->getContext()->bus->request<
                agentxx::events::ReqSubagentStart,
                agentxx::events::RespSubagentResult>(
                agentxx::events::Topic::Subagent,
                agentxx::events::ReqSubagentStart{
                    .parentAgentName = "root",
                    .parentThreadId  = "parent-session",
                    .subagentName    = "subagent_task",
                    .systemPrompt    = "You are a worker.",
                    .message         = "do the thing",
                    .resultId        = "call_1",
                    .cancelToken     = nullptr,
                },
                std::chrono::seconds(30)
            );
            finished = true;
        },
        asio::detached
    );
    pumpIoUntil(io, finished);

    XX_TEST_EXPECT_TRUE(resp.has_value());
    if (resp.has_value()) {
        XX_TEST_EXPECT_FALSE(resp->hasError);
        XX_TEST_EXPECT_EQ(resp->content, std::string{"Host spawn result"});
        XX_TEST_EXPECT_TRUE(!resp->agentId.empty());
    }
    // 子代理节点已回收 (仅剩 root 节点)
    XX_TEST_EXPECT_EQ(host->registry().size(), 1u);
    XX_TEST_EXPECT_TRUE(host->registry().contains("root"));
    XX_TEST_EXPECT_EQ(host->runningSubagents(), 0u);
    // 进度与结束事件
    XX_TEST_EXPECT_TRUE(progressCount.load() > 0);
    XX_TEST_EXPECT_EQ(doneCount.load(), 1);

    co_return;
}

/// 验证: 嵌套深度预算 (maxDepth=0 拒绝一切派生)
asio::awaitable<void> test_host_spawn_depth_limit() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);
    auto cfg     = makeSimConfig(baseUrl);

    g_da_sim_response_content = "should not run";
    g_da_sim_tool_calls       = neograph::json::array();

    auto io = std::make_shared<asio::io_context>();
    agentxx::agent::AgentHost::Config hostCfg;
    hostCfg.ioCtx    = io;
    hostCfg.maxDepth = 0;
    auto host        = agentxx::agent::AgentHost::create(hostCfg);
    std::expected<agentxx::events::RespHostSpawn, std::string> resp;
    std::atomic<bool> finished{false};

    asio::co_spawn(
        *io,
        [&]() -> asio::awaitable<void> {
            auto agent = std::make_shared<agentxx::agent::CodeAgent>(cfg);
            co_await agent->init();
            host->attachRoot(agent);
            resp = co_await host->hostBus()->request<
                agentxx::events::ReqHostSpawn,
                agentxx::events::RespHostSpawn>(
                agentxx::events::HostTopic::AgentSpawn,
                agentxx::events::ReqHostSpawn{
                    .parentAgentId = "root",
                    .name          = "subagent_task",
                    .systemPrompt  = "",
                    .message       = "hi",
                    .modelName     = "",
                    .cancelToken   = nullptr,
                },
                std::chrono::seconds(5)
            );
            finished = true;
        },
        asio::detached
    );
    pumpIoUntil(io, finished);

    XX_TEST_EXPECT_TRUE(resp.has_value());
    if (resp.has_value()) {
        XX_TEST_EXPECT_TRUE(resp->hasError);
        XX_TEST_EXPECT_TRUE(
            resp->errorMessage.find("depth") != std::string::npos
            || resp->errorMessage.find("maxDepth") != std::string::npos
        );
    }
    // 未运行任何子代理
    XX_TEST_EXPECT_EQ(host->runningSubagents(), 0u);
    XX_TEST_EXPECT_EQ(host->registry().size(), 1u);

    co_return;
}

/// 验证: 并发预算 (maxConcurrentSubagents=1, 第二个并发派生被拒绝)
asio::awaitable<void> test_host_spawn_concurrent_limit() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);
    auto cfg     = makeSimConfig(baseUrl);

    g_da_sim_response_content = "concurrent result";
    g_da_sim_tool_calls       = neograph::json::array();
    g_da_sim_delay_ms         = 300; // 拉长首个子代理运行窗口, 让第二个请求并发到达

    auto io = std::make_shared<asio::io_context>();
    agentxx::agent::AgentHost::Config hostCfg;
    hostCfg.ioCtx                  = io;
    hostCfg.maxConcurrentSubagents = 1;
    auto host                      = agentxx::agent::AgentHost::create(hostCfg);
    std::atomic<int> okCount{0};
    std::atomic<int> errCount{0};
    std::atomic<int> finishedCount{0};
    std::atomic<bool> allDone{false};

    asio::co_spawn(
        *io,
        [&]() -> asio::awaitable<void> {
            auto agent = std::make_shared<agentxx::agent::CodeAgent>(cfg);
            co_await agent->init();
            host->attachRoot(agent);

            auto spawnOne = [&](std::string tag) -> asio::awaitable<void> {
                auto r = co_await host->hostBus()->request<
                    agentxx::events::ReqHostSpawn,
                    agentxx::events::RespHostSpawn>(
                    agentxx::events::HostTopic::AgentSpawn,
                    agentxx::events::ReqHostSpawn{
                        .parentAgentId = "root",
                        .name          = "subagent_task",
                        .systemPrompt  = "",
                        .message       = tag,
                        .modelName     = "",
                        .cancelToken   = nullptr,
                    },
                    std::chrono::seconds(10)
                );
                if (r.has_value() && !r->hasError) {
                    okCount++;
                } else {
                    errCount++;
                }
                finishedCount++;
            };

            // 并发两个派生: 单 io 线程协作式调度, 首个在 LLM 请求处挂起,
            // 第二个进入时并发预算已满 → 拒绝
            asio::co_spawn(*io, spawnOne("a"), asio::detached);
            co_await spawnOne("b");
            // 等待 "a" 完成 (其 LLM 请求有 300ms 延迟)
            while (finishedCount.load() < 2) {
                asio::steady_timer t(co_await asio::this_coro::executor);
                t.expires_after(std::chrono::milliseconds(5));
                co_await t.async_wait(asio::use_awaitable);
            }
            allDone = true;
        },
        asio::detached
    );
    pumpIoUntil(io, allDone);

    XX_TEST_EXPECT_EQ(okCount.load(), 1);
    XX_TEST_EXPECT_EQ(errCount.load(), 1);
    XX_TEST_EXPECT_EQ(host->runningSubagents(), 0u);

    g_da_sim_delay_ms = 0;
    co_return;
}

/// 验证: 批量派生 (spawnBatch, wait_for_all, 结果顺序一致)
asio::awaitable<void> test_host_spawn_batch() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);
    auto cfg     = makeSimConfig(baseUrl);

    g_da_sim_response_content = "batch result";
    g_da_sim_tool_calls       = neograph::json::array();
    g_da_sim_delay_ms         = 0;

    auto io = std::make_shared<asio::io_context>();
    agentxx::agent::AgentHost::Config hostCfg;
    hostCfg.ioCtx = io;
    auto host     = agentxx::agent::AgentHost::create(hostCfg);
    std::expected<agentxx::events::RespSubagentBatch, std::string> resp;
    std::atomic<bool> finished{false};

    asio::co_spawn(
        *io,
        [&]() -> asio::awaitable<void> {
            auto agent = std::make_shared<agentxx::agent::CodeAgent>(cfg);
            co_await agent->init();
            host->attachRoot(agent);

            resp = co_await agent->getContext()->bus->request<
                agentxx::events::ReqSubagentBatch,
                agentxx::events::RespSubagentBatch>(
                agentxx::events::Topic::SubagentBatch,
                agentxx::events::ReqSubagentBatch{
                    .parentAgentName = "root",
                    .parentThreadId  = "",
                    .cancelToken     = nullptr,
                    .tasks           = {
                        agentxx::events::SubagentBatchItem{
                            .subagentName = "subagent_task",
                            .systemPrompt = "",
                            .message      = "task 1",
                            .resultId     = "r1",
                        },
                        agentxx::events::SubagentBatchItem{
                            .subagentName = "subagent_task",
                            .systemPrompt = "",
                            .message      = "task 2",
                            .resultId     = "r2",
                        },
                    },
                },
                std::chrono::seconds(30)
            );
            finished = true;
        },
        asio::detached
    );
    pumpIoUntil(io, finished);

    XX_TEST_EXPECT_TRUE(resp.has_value());
    if (resp.has_value()) {
        XX_TEST_EXPECT_EQ(resp->results.size(), 2u);
        if (resp->results.size() == 2) {
            XX_TEST_EXPECT_EQ(resp->results[0].resultId, std::string{"r1"});
            XX_TEST_EXPECT_EQ(resp->results[0].content, std::string{"batch result"});
            XX_TEST_EXPECT_FALSE(resp->results[0].hasError);
            XX_TEST_EXPECT_EQ(resp->results[1].resultId, std::string{"r2"});
            XX_TEST_EXPECT_EQ(resp->results[1].content, std::string{"batch result"});
        }
    }
    XX_TEST_EXPECT_EQ(host->runningSubagents(), 0u);
    XX_TEST_EXPECT_EQ(host->registry().size(), 1u);

    co_return;
}

/// 验证: 跨 agent 消息 (mailbox 路由) 与未注册目标
asio::awaitable<void> test_host_mailbox_message() {
    auto io = std::make_shared<asio::io_context>();
    agentxx::agent::AgentHost::Config hostCfg;
    hostCfg.ioCtx = io;
    auto host     = agentxx::agent::AgentHost::create(hostCfg);

    host->setMailbox(
        "worker",
        [](const agentxx::events::ReqHostMessage& req)
            -> asio::awaitable<agentxx::events::RespHostMessage> {
            co_return agentxx::events::RespHostMessage{
                .content = "echo: " + req.message,
            };
        }
    );

    std::expected<agentxx::events::RespHostMessage, std::string> okResp;
    std::expected<agentxx::events::RespHostMessage, std::string> missResp;
    std::atomic<bool> finished{false};

    asio::co_spawn(
        *io,
        [&]() -> asio::awaitable<void> {
            okResp = co_await host->hostBus()->request<
                agentxx::events::ReqHostMessage,
                agentxx::events::RespHostMessage>(
                agentxx::events::HostTopic::AgentMessage,
                agentxx::events::ReqHostMessage{
                    .fromAgentId = "root",
                    .toAgentId   = "worker",
                    .message     = "hello",
                    .cancelToken = nullptr,
                },
                std::chrono::seconds(5)
            );
            missResp = co_await host->hostBus()->request<
                agentxx::events::ReqHostMessage,
                agentxx::events::RespHostMessage>(
                agentxx::events::HostTopic::AgentMessage,
                agentxx::events::ReqHostMessage{
                    .fromAgentId = "root",
                    .toAgentId   = "nobody",
                    .message     = "hi",
                    .cancelToken = nullptr,
                },
                std::chrono::seconds(5)
            );
            finished = true;
        },
        asio::detached
    );
    pumpIoUntil(io, finished);

    XX_TEST_EXPECT_TRUE(okResp.has_value());
    if (okResp.has_value()) {
        XX_TEST_EXPECT_FALSE(okResp->hasError);
        XX_TEST_EXPECT_EQ(okResp->content, std::string{"echo: hello"});
    }
    XX_TEST_EXPECT_TRUE(missResp.has_value());
    if (missResp.has_value()) {
        XX_TEST_EXPECT_TRUE(missResp->hasError);
    }

    co_return;
}

/// 验证: 远程 agent (A2A 桥接) — sendMessage 目标不在本地 mailbox 时
/// 经 A2A 协议 (SendMessage + GetTask 轮询) 转发, 本地与远程 agent 消息面同构
asio::awaitable<void> test_host_remote_a2a() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);
    auto cfg     = makeSimConfig(baseUrl);

    g_da_sim_response_content = "Remote agent result";
    g_da_sim_tool_calls       = neograph::json::array();
    g_da_sim_delay_ms         = 0;

    auto io = std::make_shared<asio::io_context>();
    std::expected<agentxx::events::RespHostMessage, std::string> resp;
    std::atomic<bool> finished{false};
    std::thread serverThread;
    std::shared_ptr<agentxx::server::A2aServer> a2aServer;

    asio::co_spawn(
        *io,
        [&]() -> asio::awaitable<void> {
            // 远程 agent: 独立 CodeAgent + 进程内 A2A 服务
            auto remoteAgent = std::make_shared<agentxx::agent::CodeAgent>(cfg);
            co_await remoteAgent->init();

            agentxx::server::A2aServer::Config scfg;
            scfg.httpConfig.address = "127.0.0.1";
            scfg.httpConfig.port    = 0;
            scfg.serverName         = "remote-worker";
            a2aServer = std::make_shared<agentxx::server::A2aServer>(remoteAgent, std::move(scfg));
            serverThread            = std::thread([s = a2aServer]() { s->start(); });
            // 协作式等待端口就绪 (不阻塞 io 事件循环)
            while (a2aServer->port() == 0 && !a2aServer->isStopped()) {
                asio::steady_timer t(co_await asio::this_coro::executor);
                t.expires_after(std::chrono::milliseconds(5));
                co_await t.async_wait(asio::use_awaitable);
            }

            agentxx::server::A2aClient::Config cc;
            cc.baseUrl = "http://127.0.0.1:" + std::to_string(a2aServer->port());
            auto client = std::make_shared<agentxx::server::A2aClient>(std::move(cc));

            // 宿主 + 根 agent + 远程注册
            agentxx::agent::AgentHost::Config hostCfg;
            hostCfg.ioCtx = io;
            auto host     = agentxx::agent::AgentHost::create(hostCfg);
            auto rootAgent = std::make_shared<agentxx::agent::CodeAgent>(cfg);
            co_await rootAgent->init();
            host->attachRoot(rootAgent);
            host->registerRemoteAgent("remote-worker", client);

            resp = co_await host->hostBus()->request<
                agentxx::events::ReqHostMessage,
                agentxx::events::RespHostMessage>(
                agentxx::events::HostTopic::AgentMessage,
                agentxx::events::ReqHostMessage{
                    .fromAgentId = "root",
                    .toAgentId   = "remote-worker",
                    .message     = "hi remote",
                    .cancelToken = nullptr,
                },
                std::chrono::seconds(60)
            );
            finished = true;
        },
        asio::detached
    );
    pumpIoUntil(io, finished);
    // 停止 A2A 服务; 服务器线程分离不 join:
    // util::HttpServer 在已执行过 agent 任务的场景存在 worker 线程回收边界问题
    // (ioCtx.stop 后仍有 handler 未退出), 不影响 A2A 转发功能验证
    if (a2aServer) {
        asio::co_spawn(
            *io,
            [s = a2aServer]() -> asio::awaitable<void> {
                co_await s->stop();
            },
            asio::detached
        );
        io->run();
    }
    if (serverThread.joinable()) {
        serverThread.detach();
    }

    XX_TEST_EXPECT_TRUE(resp.has_value());
    if (resp.has_value()) {
        XX_TEST_EXPECT_FALSE(resp->hasError);
        XX_TEST_EXPECT_EQ(resp->content, std::string{"Remote agent result"});
    }

    co_return;
}

asio::awaitable<TestResult> run_agent_host_tests() {
    g_host_passed = 0;
    g_host_failed = 0;
    try {
        co_await test_host_registry();
        co_await test_host_spawn_e2e();
        co_await test_host_spawn_depth_limit();
        co_await test_host_spawn_concurrent_limit();
        co_await test_host_spawn_batch();
        co_await test_host_mailbox_message();
        co_await test_host_remote_a2a();
    } catch (const std::exception& e) {
        TEST_FAIL << "agent_host suite exception: " << e.what() << std::endl;
        g_host_failed++;
    }
    co_return TestResult{g_host_passed, g_host_failed};
}

} // namespace test
} // namespace agentxx
