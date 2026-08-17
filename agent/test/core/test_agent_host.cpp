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

/// 验证: 同上下文模式派生子代理 (messages 结构化透传 + threadId 指定)
/// - messages 原样透传为子代理初始上下文 (含 system, 不做文本转录,
///   不插入子代理默认提示)
/// - 子代理运行在指定 thread, 强制使用父会话当前模型 (忽略子代理 config 默认)
/// - 对照: 默认模式使用独立 subagent 线程 + config 默认模型 (不受父会话
///   运行时选择影响), 消息回退为 systemPrompt + message 文本
/// - 三者共同保证"相同上下文前缀 + 相同 threadid + 相同模型"命中 KV cache
asio::awaitable<void> test_host_spawn_same_context() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);
    auto cfg     = makeSimConfig(baseUrl);

    // 多模型: config 默认与父会话运行时选择区分开, 验证模型强制路径
    auto makeModel = [&](std::string name) {
        agentxx::agent::ModelConfig mc;
        mc.baseUrl   = baseUrl;
        mc.apiKey    = "EMPTY";
        mc.modelName = name;
        return mc;
    };
    cfg->availableModels["config-default-model"] = makeModel("config-default-model");
    cfg->availableModels["parent-model"]         = makeModel("parent-model");
    cfg->currentModelName                        = "config-default-model";
    cfg->subagentModel                           = makeModel("sub-model");

    g_da_sim_response_content = "same-context result";
    g_da_sim_tool_calls       = neograph::json::array();
    g_da_sim_delay_ms         = 0;
    g_da_sim_last_request     = neograph::json::object();
    g_da_sim_requests.clear();

    auto io = std::make_shared<asio::io_context>();
    agentxx::agent::AgentHost::Config hostCfg;
    hostCfg.ioCtx = io;
    auto host     = agentxx::agent::AgentHost::create(hostCfg);
    std::expected<agentxx::events::RespSubagentResult, std::string> sameCtxResp;
    std::expected<agentxx::events::RespSubagentResult, std::string> normalResp;
    std::atomic<bool> finished{false};

    // 透传的结构化消息前缀 (模拟压缩场景: 父 system + 历史消息)
    const neograph::json prefix = neograph::json::array({
        {{"role", "system"}, {"content", "PARENT SYSTEM PROMPT"}},
        {{"role", "user"}, {"content", "user old msg"}},
        {{"role", "assistant"}, {"content", "assistant old reply"}},
    });

    asio::co_spawn(
        *io,
        [&]() -> asio::awaitable<void> {
            auto agent = std::make_shared<agentxx::agent::CodeAgent>(cfg);
            co_await agent->init();
            host->attachRoot(agent);
            // 父会话: 显式选择模型 (模拟用户运行时切换, 与 config 默认不同) + 供子代理继承 bus
            auto parentSession = agent->getContext()->getSession("parent-session");
            parentSession->setModelName("parent-model");
            parentSession->bus = std::make_shared<agentxx::middleware::EventBus>(
                co_await asio::this_coro::executor
            );

            // [workaround] 聚合提取为具名变量, 绕过 g++ 16.1 ICE (gimplify.cc:8406)
            agentxx::events::ReqSubagentStart sameCtxReq{
                .parentAgentName = "root",
                .parentThreadId  = "parent-session",
                .subagentName    = "subagent_task",
                .systemPrompt    = "",
                .message         = "",
                // 结构化消息透传 (可含 system, 原样透传)
                .messages = prefix,
                // 同上下文: 运行在父线程, 共享上下文前缀
                .threadId = "parent-session",
                .resultId = "call_same_ctx",
                .cancelToken = nullptr,
            };
            sameCtxResp = co_await agent->getContext()->bus->request<
                agentxx::events::ReqSubagentStart,
                agentxx::events::RespSubagentResult>(
                agentxx::events::Topic::Subagent,
                sameCtxReq,
                std::chrono::seconds(30)
            );

            // 对照: 默认模式 (无 messages/threadId) → 独立线程 + config 默认模型
            agentxx::events::ReqSubagentStart normalReq{
                .parentAgentName = "root",
                .parentThreadId  = "parent-session",
                .subagentName    = "subagent_task",
                .systemPrompt    = "You are a worker.",
                .message         = "plain task",
                .messages        = std::nullopt,
                .threadId        = "",
                .resultId        = "call_normal",
                .cancelToken     = nullptr,
            };
            normalResp = co_await agent->getContext()->bus->request<
                agentxx::events::ReqSubagentStart,
                agentxx::events::RespSubagentResult>(
                agentxx::events::Topic::Subagent,
                normalReq,
                std::chrono::seconds(30)
            );
            finished = true;
        },
        asio::detached
    );
    pumpIoUntil(io, finished);

    // ① 同上下文模式: 派生成功, 输出正常
    XX_TEST_EXPECT_TRUE(sameCtxResp.has_value());
    if (sameCtxResp.has_value()) {
        XX_TEST_EXPECT_FALSE(sameCtxResp->hasError);
        XX_TEST_EXPECT_EQ(sameCtxResp->content, std::string{"same-context result"});
    }

    // 两次请求按顺序记录: [0]=同上下文, [1]=默认对照
    XX_TEST_EXPECT_EQ(g_da_sim_requests.size(), size_t{2});
    const auto& sameCtxReqBody = g_da_sim_requests[0];
    const auto& normalReqBody  = g_da_sim_requests[1];

    // ② 模型强制: 同上下文请求使用父会话当前模型 (parent-model), 而非 config 默认
    XX_TEST_EXPECT_EQ(
        sameCtxReqBody.value("model", std::string{}),
        std::string{"parent-model"}
    );

    // ③ 消息前缀: system 由引擎统一替换为 (子 config 拷贝自父的) systemPrompt,
    //    与父会话请求前缀一致 (这正是 KV cache 前缀一致的关键);
    //    其余消息原样透传 (无默认提示插入, 无文本转录);
    //    透传段以 assistant 结尾时, 引擎按与父会话相同的 repairMessages 规则
    //    补充 user "[Please continue]" (两边一致, 前缀仍命中)
    {
        const auto& reqMsgs = sameCtxReqBody["messages"];
        XX_TEST_EXPECT_TRUE(reqMsgs.is_array());
        XX_TEST_EXPECT_EQ(reqMsgs.size(), prefix.size() + 1);
        // system: 统一为 config systemPrompt (+ 附加 system prompt, 与父会话同款)
        XX_TEST_EXPECT_EQ(reqMsgs[0].value("role", std::string{}), std::string{"system"});
        XX_TEST_EXPECT_TRUE(
            reqMsgs[0].value("content", std::string{}).find("You are a helpful assistant.")
            != std::string::npos
        );
        // 其余消息原样透传
        for (size_t i = 1; i < prefix.size(); ++i) {
            XX_TEST_EXPECT_EQ(
                reqMsgs[i].value("role", std::string{}),
                prefix[i].value("role", std::string{})
            );
            XX_TEST_EXPECT_EQ(
                reqMsgs[i].value("content", std::string{}),
                prefix[i].value("content", std::string{})
            );
        }
        // 末尾: repairMessages 补齐的 user 提示 (与父会话同规则)
        XX_TEST_EXPECT_EQ(
            reqMsgs.back().value("role", std::string{}),
            std::string{"user"}
        );
        XX_TEST_EXPECT_EQ(
            reqMsgs.back().value("content", std::string{}),
            std::string{"[Please continue]"}
        );
    }

    // ④ 对照: 默认模式独立运行 — config 默认模型 + systemPrompt/message 文本
    //    (system 同样被引擎替换为 config systemPrompt)
    XX_TEST_EXPECT_TRUE(normalResp.has_value());
    if (normalResp.has_value()) {
        XX_TEST_EXPECT_FALSE(normalResp->hasError);
    }
    XX_TEST_EXPECT_EQ(
        normalReqBody.value("model", std::string{}),
        std::string{"config-default-model"}
    );
    {
        const auto& reqMsgs = normalReqBody["messages"];
        XX_TEST_EXPECT_TRUE(reqMsgs.is_array());
        XX_TEST_EXPECT_EQ(reqMsgs.size(), size_t{2});
        XX_TEST_EXPECT_TRUE(
            reqMsgs[0].value("content", std::string{}).find("You are a helpful assistant.")
            != std::string::npos
        );
        XX_TEST_EXPECT_EQ(reqMsgs[1].value("content", std::string{}), std::string{"plain task"});
    }

    // ⑤ 回收: 子代理节点已清理 (含同上下文共享父线程场景, 不残留深度记录)
    XX_TEST_EXPECT_EQ(host->runningSubagents(), 0u);
    XX_TEST_EXPECT_EQ(host->registry().size(), 1u);

    co_return;
}

/// 验证: 子代理工具策略 (tools 参数)
/// - [] = 无工具; ["agentxx_share_store"] = 自定义白名单;
///   ["*"] = 全量继承父 agent 工具; 缺省 = 子代理默认全量
asio::awaitable<void> test_host_spawn_tool_policy() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);
    auto cfg     = makeSimConfig(baseUrl);

    g_da_sim_response_content = "tool policy result";
    g_da_sim_tool_calls       = neograph::json::array();
    g_da_sim_delay_ms         = 0;
    g_da_sim_requests.clear();

    auto io = std::make_shared<asio::io_context>();
    agentxx::agent::AgentHost::Config hostCfg;
    hostCfg.ioCtx = io;
    auto host     = agentxx::agent::AgentHost::create(hostCfg);
    std::atomic<bool> finished{false};

    asio::co_spawn(
        *io,
        [&]() -> asio::awaitable<void> {
            auto agent = std::make_shared<agentxx::agent::CodeAgent>(cfg);
            co_await agent->init();
            host->attachRoot(agent);

            // 顺序派生 4 次 (不同工具策略), 请求体按到达顺序记录
            auto spawnOne = [&](std::optional<neograph::json> tools, std::string tag)
                -> asio::awaitable<void> {
                // [workaround] 聚合提取为具名变量, 绕过 g++ 16.1 ICE
                agentxx::events::ReqSubagentStart req{
                    .parentAgentName = "root",
                    .parentThreadId  = "parent-session",
                    .subagentName    = "subagent_task",
                    .systemPrompt    = "You are a worker.",
                    .message         = "do " + tag,
                    .messages        = std::nullopt,
                    .threadId        = "",
                    .tools           = tools,
                    .enableSummarization = std::nullopt,
                    .resultId        = "call_" + tag,
                    .cancelToken     = nullptr,
                };
                auto resp = co_await agent->getContext()->bus->request<
                    agentxx::events::ReqSubagentStart,
                    agentxx::events::RespSubagentResult>(
                    agentxx::events::Topic::Subagent,
                    req,
                    std::chrono::seconds(30)
                );
                if (resp.has_value() && resp->hasError) {
                    XX_LOGE("spawn tool-policy `{}` failed: {}", tag, resp->errorMessage);
                }
            };

            // ① 无工具
            co_await spawnOne(neograph::json::array(), "none");
            // ② 自定义白名单
            co_await spawnOne(
                neograph::json::array({"agentxx_share_store"}),
                "custom"
            );
            // ③ 全量继承父工具
            co_await spawnOne(neograph::json::array({"*"}), "inherit");
            // ④ 缺省 (子代理默认全量)
            co_await spawnOne(std::nullopt, "default");
            finished = true;
        },
        asio::detached
    );
    pumpIoUntil(io, finished);

    // 4 次请求按顺序记录
    XX_TEST_EXPECT_EQ(g_da_sim_requests.size(), size_t{4});

    // 父工具名集合 (继承断言基准)
    std::vector<std::string> parentToolNames;
    if (auto rootNode = host->registry().get("root")) {
        if (rootNode->agent && rootNode->agent->getContext()) {
            parentToolNames = rootNode->agent->getContext()->toolNames;
        }
    }
    XX_TEST_EXPECT_FALSE(parentToolNames.empty());

    auto toolsOf = [](const neograph::json& req) -> std::vector<std::string> {
        std::vector<std::string> names;
        if (req.contains("tools") && req["tools"].is_array()) {
            for (const auto& t : req["tools"]) {
                if (t.is_object() && t["function"].is_object()
                    && t["function"]["name"].is_string()) {
                    names.push_back(t["function"]["name"].get<std::string>());
                }
            }
        }
        return names;
    };

    // ① 无工具: tools 为空 (缺失或空数组)
    {
        const auto& names = toolsOf(g_da_sim_requests[0]);
        XX_TEST_EXPECT_TRUE(names.empty());
    }
    // ② 自定义: 仅 agentxx_share_store
    {
        const auto& names = toolsOf(g_da_sim_requests[1]);
        XX_TEST_EXPECT_EQ(names.size(), size_t{1});
        XX_TEST_EXPECT_EQ(names[0], std::string{"agentxx_share_store"});
    }
    // ③ 继承: 工具集 == 父工具名集合
    {
        const auto& names = toolsOf(g_da_sim_requests[2]);
        XX_TEST_EXPECT_TRUE(names.size() >= parentToolNames.size());
        // 父工具全部保留
        for (const auto& name : parentToolNames) {
            XX_TEST_EXPECT_TRUE(
                std::find(names.begin(), names.end(), name) != names.end()
            );
        }
    }
    // ④ 缺省: 与继承一致 (无 MCP/插件差异时等于父工具集)
    {
        const auto& names = toolsOf(g_da_sim_requests[3]);
        XX_TEST_EXPECT_TRUE(names.size() >= parentToolNames.size());
        for (const auto& name : parentToolNames) {
            XX_TEST_EXPECT_TRUE(
                std::find(names.begin(), names.end(), name) != names.end()
            );
        }
    }

    // 回收
    XX_TEST_EXPECT_EQ(host->runningSubagents(), 0u);
    XX_TEST_EXPECT_EQ(host->registry().size(), 1u);

    co_return;
}

/// 验证: 子代理上下文压缩 (summarization) 开关
/// - enableSummarization=false → init 后无 summarization 中间件
/// - 缺省/true → 存在 (summarization 发起的压缩子代理必须显式 false)
asio::awaitable<void> test_subagent_summarization_switch() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    g_da_sim_response_content = "ok";
    g_da_sim_tool_calls       = neograph::json::array();

    // ① enableSummarization = false → 无 summarization 中间件
    {
        auto cfg                 = makeSimConfig(baseUrl);
        cfg->enableSummarization = false;
        auto agent = std::make_shared<agentxx::agent::CodeAgent>(cfg);
        co_await agent->init();
        XX_TEST_EXPECT_TRUE(agent->getContext()->summarizationMiddleware == nullptr);
    }
    // ② 缺省 (true) → 存在
    {
        auto cfg    = makeSimConfig(baseUrl);
        auto agent  = std::make_shared<agentxx::agent::CodeAgent>(cfg);
        co_await agent->init();
        XX_TEST_EXPECT_TRUE(agent->getContext()->summarizationMiddleware != nullptr);
    }
    // ③ 显式 true → 存在
    {
        auto cfg                 = makeSimConfig(baseUrl);
        cfg->enableSummarization = true;
        auto agent = std::make_shared<agentxx::agent::CodeAgent>(cfg);
        co_await agent->init();
        XX_TEST_EXPECT_TRUE(agent->getContext()->summarizationMiddleware != nullptr);
    }

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

            // [workaround] 提取聚合为具名变量, 绕过 g++ 16.1 对
            // request<Req,Resp>(topic, 复杂聚合字面量, timeout) 的 ICE
            // (gimplify.cc:8406 internal compiler error)
            agentxx::events::ReqSubagentBatch batchReq{
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
            };
            resp = co_await agent->getContext()->bus->request<
                agentxx::events::ReqSubagentBatch,
                agentxx::events::RespSubagentBatch>(
                agentxx::events::Topic::SubagentBatch,
                batchReq,
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
        co_await test_host_spawn_same_context();
        co_await test_host_spawn_tool_policy();
        co_await test_subagent_summarization_switch();
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
