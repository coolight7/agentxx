// 压缩请求的前缀对齐与模拟缓存命中测试
//
// 背景 (见 plan §3.7 / §4):
// - 上下文压缩派生的子代理与父会话"同上下文", 期望命中父会话已缓存的前缀
// - 此前它有两条不一致: ① 子代理按自己的配置重建 system (缺记忆文件/技能清单等
//   运行时段落) ② 子代理没有工具定义; 两者都让请求前缀在很早的位置分叉
// - 修复后: 保留调用方给的 system 消息 + 继承父会话上一次请求的工具定义,
//   并且在本次运行内不执行任何工具 (工具定义只用于对齐前缀)
//
// 本模块用模拟器的"模拟前缀缓存"度量命中比例 (主程序侧缓存用量解析会把
// cached_tokens 记入用量台账), 因此断言的是真实的请求前缀一致性。

#include "agentxx-test/core/test_prompt_cache_prefix.h"

#include "agentxx-test/core/test_agent.h" // 本地 LLM 模拟器 startDaSimServer/g_da_sim_*
#include "agentxx-test/test_coro_util.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/prompt_sources.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/event/events.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/middlewares/summarization.h"
#include "agentxx/util/observability.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_pcp_passed = 0;
int g_pcp_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_pcp_passed
#define XX_TEST_FAILED g_pcp_failed

namespace agentxx {
namespace test {

namespace {

namespace fs = std::filesystem;

std::string makeTempRoot() {
    auto dir = fs::temp_directory_path()
               / fmt::format(
                   "agentxx_pcp_test_{}",
                   std::chrono::steady_clock::now().time_since_epoch().count()
               );
    fs::create_directories(dir);
    return dir.string();
}

void removeTempRoot(const std::string& root) {
    std::error_code ec;
    fs::remove_all(utilxx_base::utf8ToPath(root), ec);
}

/// 请求体里第一条 system 消息的正文
std::string systemOf(const utilxx_base::Json& request) {
    if (!request.contains("messages") || !request["messages"].is_array()) {
        return {};
    }
    for (const auto& m : request["messages"]) {
        if (m.value("role", std::string{}) == "system") {
            return m.value("content", std::string{});
        }
    }
    return {};
}

/// 请求体里的工具定义 (JSON 文本; 没有 tools 时为空串)
std::string toolsOf(const utilxx_base::Json& request) {
    if (!request.contains("tools") || !request["tools"].is_array() || request["tools"].empty()) {
        return {};
    }
    return request["tools"].dump();
}

std::vector<utilxx_base::Json> messagesOf(const utilxx_base::Json& request) {
    std::vector<utilxx_base::Json> out;
    if (!request.contains("messages") || !request["messages"].is_array()) {
        return out;
    }
    out.reserve(request["messages"].size());
    for (const auto& m : request["messages"]) {
        out.push_back(m);
    }
    return out;
}

/// 取装配的压缩中间件 (未装配返回 nullptr)
agentxx::middleware::SummarizationMiddlewareHandle*
    findSummarizationMiddleware(agentxx::agent::CodeAgent& agent) {
    auto ctx = agent.agentContext;
    if (!ctx || !ctx->middlewareHandleContext) {
        return nullptr;
    }
    for (auto& handle : ctx->middlewareHandleContext->handles) {
        if (auto* sum
            = dynamic_cast<agentxx::middleware::SummarizationMiddlewareHandle*>(handle.get())) {
            return sum;
        }
    }
    return nullptr;
}

/// 往会话上下文里塞一段"长历史" (模拟已经聊了很多轮)
void seedLongHistory(agentxx::agent::CodeAgent& agent, std::string_view sessionId, size_t rounds) {
    std::vector<neograph::ChatMessage> msgs;
    msgs.reserve(rounds * 2);
    for (size_t i = 0; i < rounds; ++i) {
        // 每条 2000 字符 (约 500 token): 长历史让压缩请求的前缀占比足够高
        msgs.push_back(neograph::ChatMessage{
            .role    = "user",
            .content = fmt::format("HISTORY-USER-{}-{}", i, std::string(2000, 'u')),
        });
        msgs.push_back(neograph::ChatMessage{
            .role    = "assistant",
            .content = fmt::format("HISTORY-ASSISTANT-{}-{}", i, std::string(2000, 'a')),
        });
    }
    if (auto session = agent.agentContext->sessions->get(sessionId); session) {
        session->appendMessages(std::move(msgs), false);
    }
}

/// 驱动 io 直到 done 置位 (子代理/HTTP 都在别的线程上推进, 需要自己泵事件循环)
void pumpIoUntil(std::shared_ptr<asio::io_context> io, const std::atomic<bool>& done) {
    while (!done.load()) {
        asio::steady_timer t(*io);
        t.expires_after(std::chrono::milliseconds(5));
        t.async_wait([](const utilxx_base::AsioErrorCode&) {});
        io->run();
    }
    io->run();
}

/// 用例主体 (运行在自建的 io_context 上, 供 AgentHost 共享同一事件循环)
asio::awaitable<void> run_prefix_body(
    std::shared_ptr<asio::io_context> io,
    const std::string&                memoryFile,
    const std::string&                storeRoot,
    agentxx::test::DaSimServer&       sim,
    TestResult&                       result,
    int                               passedBefore,
    int                               failedBefore
) {
    auto cfg             = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl   = "http://127.0.0.1:" + std::to_string(sim.port);
    cfg->model.apiKey    = "EMPTY";
    cfg->model.modelName = "default-model";
    cfg->permissionMode  = agentxx::agent::PermissionMode::Pass;
    // 打开会话持久化: 用量台账 (usage 表) 才能按请求逐条核对
    cfg->enableSessionStore    = true;
    cfg->sessionStoreDirectory = storeRoot;
    cfg->prompt.systemPrompt   = "SYSTEM-PROMPT-BASE";
    cfg->memoryFilePaths.push_back(memoryFile);
    cfg->llmMaxRetry = 1;

    auto agent = std::make_shared<agentxx::agent::CodeAgent>(cfg);
    co_await agent->init();

    // 派生压缩子代理需要一个 agent 节点容器 (同上下文压缩走 AgentHost::spawnBatch)
    auto host = agentxx::agent::AgentHost::create(agentxx::agent::AgentHost::Config{
        .ioCtx = io,
    });
    host->attachRoot(agent);

    const std::string sessionId = "prompt-cache-prefix-session";

    // ---- 父会话第一条请求: 建立"上一次请求"的前缀基准 ----
    (void)co_await agent->runTurnAsync(sessionId, "first turn", nullptr);
    XX_TEST_EXPECT_EQ(g_da_sim_requests.size(), size_t{1});
    if (g_da_sim_requests.empty()) {
        result = TestResult{g_pcp_passed - passedBefore, g_pcp_failed - failedBefore};
        co_return;
    }
    const auto system1 = systemOf(g_da_sim_requests[0]);
    const auto tools1  = toolsOf(g_da_sim_requests[0]);
    XX_TEST_EXPECT_TRUE(!system1.empty());
    // 运行时段落 (记忆文件) 在稳定段末尾: 前缀对齐必须带上它
    XX_TEST_EXPECT_TRUE(system1.find("MEMORY-MARKER") != std::string::npos);
    XX_TEST_EXPECT_TRUE(!tools1.empty());

    // ---- 塞入长历史后跑第二圈: 父会话的上一次请求就是"大请求" ----
    seedLongHistory(*agent, sessionId, 12);
    (void)co_await agent->runTurnAsync(sessionId, "second turn", nullptr);
    XX_TEST_EXPECT_EQ(g_da_sim_requests.size(), size_t{2});
    if (g_da_sim_requests.size() < 2) {
        result = TestResult{g_pcp_passed - passedBefore, g_pcp_failed - failedBefore};
        co_return;
    }
    const auto& parentRequest = g_da_sim_requests[1];
    const auto  system2       = systemOf(parentRequest);
    const auto  tools2        = toolsOf(parentRequest);
    const auto  parentMsgs    = messagesOf(parentRequest);
    XX_TEST_EXPECT_EQ(system2, system1); // 内容没变 → system 逐字节相同 (稳定前缀)
    XX_TEST_EXPECT_EQ(tools2, tools1);
    XX_TEST_EXPECT_TRUE(parentMsgs.size() > 20); // 长历史确实进了请求
    TEST_INFO << "2nd turn request: cached/prompt = " << g_da_sim_last_cached_tokens << "/"
              << g_da_sim_last_prompt_tokens << " (ratio " << g_da_sim_last_cache_ratio << ")"
              << std::endl;

    // ---- 手动压缩 (同上下文子代理, 走 AgentHost): 检验请求前缀对齐 ----
    auto* summarization = findSummarizationMiddleware(*agent);
    XX_TEST_EXPECT_TRUE(summarization != nullptr);
    if (!summarization) {
        result = TestResult{g_pcp_passed - passedBefore, g_pcp_failed - failedBefore};
        co_return;
    }
    const int beforeCompact = static_cast<int>(g_da_sim_requests.size());
    // 注意: 这里的引用在下一次请求入队后就失效 (vector 可能扩容),
    // 需要跨压缩比较的值先拷出来
    const auto parentToolChoice = parentRequest.value("tool_choice", std::string{});
    const bool compacted        = co_await summarization->compactSessionContext(sessionId);
    XX_TEST_EXPECT_TRUE(compacted);
    XX_TEST_EXPECT_EQ(g_da_sim_requests.size(), static_cast<size_t>(beforeCompact) + 1);
    if (g_da_sim_requests.size() < static_cast<size_t>(beforeCompact + 1)) {
        result = TestResult{g_pcp_passed - passedBefore, g_pcp_failed - failedBefore};
        co_return;
    }
    const auto& compactRequest = g_da_sim_requests.back();
    const auto  compactMsgs    = messagesOf(compactRequest);

    // ① system 与父会话上一次请求逐字节相同 (保留调用方给的 system 消息)
    XX_TEST_EXPECT_EQ(systemOf(compactRequest), system2);
    // ② 工具定义与父会话上一次请求逐字节相同 (继承上一次请求的工具)
    XX_TEST_EXPECT_EQ(toolsOf(compactRequest), tools2);
    // ③ 消息是父会话前缀 + 末尾压缩指令
    XX_TEST_EXPECT_TRUE(compactMsgs.size() > parentMsgs.size());
    {
        const auto count = std::min(compactMsgs.size(), parentMsgs.size());
        bool       same  = true;
        for (size_t i = 0; i < count; ++i) {
            if (compactMsgs[i].dump() != parentMsgs[i].dump()) {
                same = false;
                break;
            }
        }
        XX_TEST_EXPECT_TRUE(same);
    }
    // ④ 模拟前缀缓存: 长历史下命中比例 >= 0.9
    TEST_INFO << "compact request: cached/prompt = " << g_da_sim_last_cached_tokens << "/"
              << g_da_sim_last_prompt_tokens << " (ratio " << g_da_sim_last_cache_ratio << ")"
              << std::endl;
    XX_TEST_EXPECT_TRUE(g_da_sim_last_cache_ratio >= 0.9);
    // ⑤ 不执行工具: 工具定义仍在请求里 (对齐前缀), 且不改写 tool_choice ——
    //    真实网关实测表明该字段与父请求不同会让前缀缓存不再命中,
    //    因此压缩请求与父请求的 tool_choice 必须完全一致
    XX_TEST_EXPECT_EQ(compactRequest.value("tool_choice", std::string{}), parentToolChoice);
    // ⑥ 压缩结果已写回 (会话上下文被替换成 system | user(压缩) | assistant(摘要))
    {
        auto session = agent->agentContext->sessions->get(sessionId);
        XX_TEST_EXPECT_TRUE(session != nullptr);
        if (session) {
            XX_TEST_EXPECT_TRUE(session->messagesCount() <= 3);
        }
        // 关键指标: 压缩次数已计入 (此前有定义但主程序从未调用)
        if (agent->agentContext->metrics) {
            XX_TEST_EXPECT_TRUE(agent->agentContext->metrics->compactions() >= 1);
        }
    }
    // ⑦ 压缩子代理的用量记到父会话台账 (子代理没有自己的 sessionStore):
    //    父会话此前 2 条请求 → 现在应有第 3 条, 且内容就是压缩请求本身
    {
        auto store = agent->agentContext->sessions->sessionStore;
        XX_TEST_EXPECT_TRUE(store != nullptr);
        if (store) {
            auto rows = store->recentUsage(sessionId, 10);
            XX_TEST_EXPECT_EQ(rows.size(), size_t{3});
            if (rows.size() >= 3) {
                const auto& newest = rows.front();
                XX_TEST_EXPECT_TRUE(newest.ok);
                XX_TEST_EXPECT_EQ(newest.promptTokens, g_da_sim_last_prompt_tokens);
                XX_TEST_EXPECT_EQ(newest.cachedPromptTokens, g_da_sim_last_cached_tokens);
                // 摘要正文 (completion) 也确实记下来了
                XX_TEST_EXPECT_TRUE(newest.completionTokens > 0);
            }
        }
    }
    // ⑧ 自动压缩经总线到达时 `spawnBatch(req)` 不带父上下文:
    //    用量仍须回退到根会话台账 (否则自动压缩的调用又记不下来)
    if (auto rootCtx = agent->agentContext; rootCtx && rootCtx->sessions->sessionStore) {
        auto store      = rootCtx->sessions->sessionStore;
        auto beforeRows = store->recentUsage(sessionId, 20).size();
        auto autoReq    = agentxx::events::ReqSubagentBatch{
               .parentSessionId = sessionId,
        };
        autoReq.tasks.push_back(agentxx::events::SubagentBatchItem{
            .subagentName        = "subagent_task",
            .message             = "auto path usage accounting",
            .sessionId           = sessionId,
            .tools               = utilxx_base::Json::array(),
            .enableSummarization = false,
        });
        auto resp = co_await host->spawnBatch(autoReq);
        XX_TEST_EXPECT_EQ(resp.results.size(), size_t{1});
        auto afterRows = store->recentUsage(sessionId, 20);
        XX_TEST_EXPECT_EQ(afterRows.size(), beforeRows + 1);
        if (!afterRows.empty()) {
            XX_TEST_EXPECT_TRUE(afterRows.front().promptTokens > 0);
        }
    }

    // ---- 压缩后父会话的下一条请求: system 前缀仍与压缩请求一致 ----
    const int beforeNext = static_cast<int>(g_da_sim_requests.size());
    (void)co_await agent->runTurnAsync(sessionId, "third turn", nullptr);
    XX_TEST_EXPECT_EQ(g_da_sim_requests.size(), static_cast<size_t>(beforeNext) + 1);
    if (g_da_sim_requests.size() >= static_cast<size_t>(beforeNext + 1)) {
        // 记忆文件内容没变 → 压缩后的刷新不改变段落, system 前缀保持
        XX_TEST_EXPECT_EQ(systemOf(g_da_sim_requests.back()), system2);
    }

    // ---- 不执行工具: 模型返回 tool_calls 时忽略, 不进入 tools 节点 ----
    // 若工具被执行, 图会再发一次请求 (llm -> tools -> llm), 请求数增量 > 1
    {
        g_da_sim_tool_calls = utilxx_base::Json::array({
            utilxx_base::Json{
                              {"id", "call_prefix_1"},
                              {"type", "function"},
                              {"function",
                 utilxx_base::Json{
                     {"name", "agentxx_filesystem_read"},
                     {"arguments", R"({"path":"a.txt"})"},
                 }},
                              }
        });
        const int before    = static_cast<int>(g_da_sim_requests.size());
        (void)co_await summarization->compactSessionContext(sessionId);
        XX_TEST_EXPECT_EQ(g_da_sim_requests.size(), static_cast<size_t>(before) + 1);
        g_da_sim_tool_calls = utilxx_base::Json::array();

        // 压缩结果里不会出现 tool 角色的消息 (工具没有被执行)
        auto session = agent->agentContext->sessions->get(sessionId);
        if (session) {
            bool hasToolMessage = false;
            for (const auto& m : session->messages()) {
                if (m.role == "tool") {
                    hasToolMessage = true;
                    break;
                }
            }
            XX_TEST_EXPECT_FALSE(hasToolMessage);
        }
    }

    host.reset();
    result = TestResult{g_pcp_passed - passedBefore, g_pcp_failed - failedBefore};
    co_return;
}

} // namespace

asio::awaitable<TestResult> run_prompt_cache_prefix_tests() {
    const int passedBefore = g_pcp_passed;
    const int failedBefore = g_pcp_failed;

    const auto root       = makeTempRoot();
    const auto memoryFile = (fs::path{root} / "memory.md").string();
    const auto storeRoot  = (fs::path{root} / "sessions").string();
    {
        std::ofstream ofs{utilxx_base::utf8ToPath(memoryFile)};
        ofs << "MEMORY-MARKER: prefix alignment must keep this section\n";
    }

    auto sim                  = startDaSimServer();
    g_da_sim_response_content = "prefix test response";
    g_da_sim_tool_calls       = utilxx_base::Json::array();
    g_da_sim_fail_count       = 0;
    g_da_sim_delay_ms         = 0;
    // 打开模拟前缀缓存: 之后的请求会与上一条请求逐字节比较前缀
    g_da_sim_prefix_cache_enabled = true;

    // 自建 io_context: AgentHost 与 agent 共享同一条事件循环
    auto              io = std::make_shared<asio::io_context>();
    std::atomic<bool> done{false};
    TestResult        result{};

    asio::co_spawn(
        *io,
        keepAliveRun([&, io]() -> asio::awaitable<void> {
            co_await run_prefix_body(
                io,
                memoryFile,
                storeRoot,
                sim,
                result,
                passedBefore,
                failedBefore
            );
            done = true;
        }),
        asio::detached
    );
    pumpIoUntil(io, done);

    sim.stop();
    g_da_sim_prefix_cache_enabled = false;
    removeTempRoot(root);
    co_return result;
}

} // namespace test
} // namespace agentxx
