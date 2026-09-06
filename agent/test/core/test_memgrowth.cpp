// 内存增长实测: 用 mock LLM 跑多轮会话, 逐轮采样进程内存与各容器大小,
// 验证 agentxx_cli 消息轮次增加时内存增长的来源与是否泄漏。
//
// 用法: agentxx_test memgrowth (经环境变量覆盖: MEM_TURNS / MEM_RESP_KB /
// MEM_HUGE_LIMIT / MEM_SCENARIOS, 后者为逗号分隔的 0/1/2 子集选择)
//
// 输出每轮: RSS/Private 内存、viewMessages / llmMessages / shareStore 的大小,
// 量化各模块内存占用。

#include "test_memgrowth.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/util/env.h"
#include "agentxx/util/log.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/experimental/channel.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "test_agent.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <psapi.h>
#include <windows.h>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace agentxx {
namespace test {

namespace {

// ---------------------------------------------------------------------------
// 进程内存采样
// ---------------------------------------------------------------------------

struct MemSample {
    double rssMB     = 0.0; // 常驻物理内存
    double privateMB = 0.0; // 私有内存 (Windows PrivateUsage / Linux RSS 近似)
};

MemSample sampleProcessMemory() {
    MemSample out;
#ifdef _WIN32
    HANDLE                     h = ::GetCurrentProcess();
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (h
        && ::GetProcessMemoryInfo(
            h,
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
            sizeof(pmc)
        )) {
        out.rssMB     = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        out.privateMB = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
    }
#else
    struct rusage ru {};

    if (::getrusage(RUSAGE_SELF, &ru) == 0) {
        out.rssMB = static_cast<double>(ru.ru_maxrss) / 1024.0; // KB -> MB
    }
    out.privateMB = out.rssMB;
#endif
    return out;
}

// ---------------------------------------------------------------------------
// 各容器字节估算 (dump 近似)
// ---------------------------------------------------------------------------

size_t estimateHistoryBytes(const std::vector<agentxx::agent::ViewMessage>& h) {
    size_t total = 0;
    for (const auto& m : h) {
        total += m.toJson().dump().size();
        total += m.id.size();
    }
    return total;
}

size_t estimateJsonBytes(const neograph::json& j) {
    return j.dump().size();
}

// ---------------------------------------------------------------------------
// 单次场景运行: 多轮会话 + 采样
// ---------------------------------------------------------------------------

struct ContainerSnapshot {
    size_t fullHistoryCount = 0;
    size_t fullHistoryBytes = 0;
    size_t llmMsgCount      = 0;
    size_t llmMsgBytes      = 0;
    size_t shareStoreItems  = 0;
};

ContainerSnapshot snapshotContainers(
    std::shared_ptr<agentxx::agent::AgentContext> ctx,
    std::string_view                              sessionId
) {
    ContainerSnapshot out;
    auto              session = ctx->sessions->get(sessionId);
    if (session) {
        out.fullHistoryCount = session->viewMessages.size();
        out.fullHistoryBytes = estimateHistoryBytes(session->viewMessages);
        out.llmMsgCount      = session->llmMessages.size();
        out.llmMsgBytes      = estimateJsonBytes(session->llmMessages);
    }
    auto it = ctx->middlewareHandleContext->shareStore.find(std::string{sessionId});
    if (it != ctx->middlewareHandleContext->shareStore.end()) {
        out.shareStoreItems = it->second.store.size();
    }
    return out;
}

asio::awaitable<int> runScenario(
    size_t      turns,
    size_t      responseKB,
    size_t      warmupSkip,
    bool        hugeTokenLimit, // true = 关闭压缩 (模拟不触发 summarization)
    bool        nonStream,      // true = 使用 runOverMsgsTurnAsync (无 token 流式事件)
    bool        runAgentCtx,    // true = 后台线程运行 agent io_context (模拟真实 CLI)
    std::string label,
    int&        okTurnsOut
) {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                 = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl       = baseUrl;
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "test-sim";
    cfg->prompt.systemPrompt = "You are a helpful assistant.";
    if (hugeTokenLimit) {
        // 极大上下文上限: summarization 永远不会触发, 观察纯线性增长
        cfg->model.modelContenxtMaxToken = 1024ull * 1024ull * 1024ull;
    }

    // 每条 assistant 回复约 responseKB KB
    const size_t k = responseKB * 1024;
    std::string  big;
    big.reserve(k);
    {
        std::string base = "The quick brown fox jumps over the lazy dog. ";
        while (big.size() < k) {
            big += base;
        }
        big.resize(k);
    }
    g_da_sim_response_content  = big;
    g_da_sim_tool_calls        = neograph::json::array();
    g_da_sim_prompt_tokens     = static_cast<int>(k / 4);
    g_da_sim_completion_tokens = static_cast<int>(k / 4);

    auto agent = std::make_shared<agentxx::agent::CodeAgent>(cfg);
    co_await agent->init();

    // 模拟真实 CLI: 后台线程运行 agent io_context
    // 注意: 必须持 work_guard, 否则 io_context 空闲时 run() 立即返回、线程退出,
    // 后续 co_spawn 到该 io_context 的轮次协程永远不会执行, channel 等待永久挂起
    std::thread                                                                 agentThread;
    std::shared_ptr<asio::executor_work_guard<asio::io_context::executor_type>> agentWorkGuard;
    if (runAgentCtx) {
        agentWorkGuard
            = std::make_shared<asio::executor_work_guard<asio::io_context::executor_type>>(
                agent->ioCtx->get_executor()
            );
        agentThread = std::thread([agent]() {
            agent->ioCtx->run();
        });
    }

    const std::string sessionId = "memgrowth_" + label;

#ifdef _MSC_VER
    // CRT 调试堆快照: lSizes 为"当前存活"分配字节, 区分真泄漏与堆碎片化
    // 注意: 进程存活分配字节较多时 _CrtMemCheckpoint 遍历整个堆极慢,
    // 可设置环境变量 MEM_NO_CRT=1 跳过该指标以加速长时间跑测
    _CrtMemState crtA, crtB;
    bool         useCrt = !agentxx::util::ApplicationEnv::instance().has("MEM_NO_CRT");
    if (useCrt) {
        _CrtMemCheckpoint(&crtA);
    }
#endif

    std::printf(
        "\n===== [%s] turns=%zu resp=%zuKB hugeLimit=%d nonStream=%d =====\n",
        label.c_str(),
        turns,
        responseKB,
        (int)hugeTokenLimit,
        (int)nonStream
    );
    std::fflush(stdout);
    std::printf(
        "%-6s %10s %10s | %10s %10s | %8s %8s | %8s\n",
        "turn",
        "RSS_MB",
        "Priv_MB",
        "histCnt",
        "histMB",
        "llmCnt",
        "llmMB",
        "shareN"
    );
    std::fflush(stdout);

    size_t lastLive = 0;
    (void)lastLive;
    size_t okTurns = 0;
    for (size_t turn = 0; turn < turns; ++turn) {
        std::fprintf(
            stderr,
            "[memgrow] turn %zu begin (stream=%d ctx=%d)\n",
            turn,
            (int)!nonStream,
            (int)runAgentCtx
        );
        auto input = "User message number " + std::to_string(turn) + " (memory growth probe)";
        bool ok    = true;
        if (runAgentCtx) {
            // 在 agent io_context 上执行 turn (与真实 CLI 一致), 经 channel 等待完成
            using ResCh = asio::experimental::channel<void(neograph_asio_error_code, bool)>;
            auto ch     = std::make_shared<ResCh>(agent->ioCtx->get_executor(), 1);
            asio::co_spawn(
                *agent->ioCtx,
                [agent, sessionId, input, turn, ch, nonStream]() -> asio::awaitable<void> {
                    bool success = false;
                    if (nonStream) {
                        std::vector<neograph::ChatMessage> msgs;
                        msgs.push_back(neograph::ChatMessage{
                            .role    = "user",
                            .content = input,
                        });
                        auto text = co_await agent->runOverMsgsTurnAsync(sessionId, msgs);
                        success   = !text.empty();
                    } else {
                        auto result = co_await agent->runTurnAsync(sessionId, input, nullptr);
                        success     = !result.hasError;
                    }
                    co_await ch
                        ->async_send(neograph_asio_error_code{}, success, asio::use_awaitable);
                    co_return;
                },
                asio::detached
            );
            auto [ec, ok2] = co_await ch->async_receive(asio::as_tuple(asio::use_awaitable));
            ok             = ok2;
        } else if (nonStream) {
            std::vector<neograph::ChatMessage> msgs;
            msgs.push_back(neograph::ChatMessage{
                .role    = "user",
                .content = input,
            });
            auto text = co_await agent->runOverMsgsTurnAsync(sessionId, msgs);
            // runOverMsgsTurnAsync 走 runInternalAsync(headless 无会话落盘):
            // 会话容器为空属预期, 仅以非空回复判定本轮成功
            if (text.empty()) {
                ok = false;
            }
        } else {
            auto result = co_await agent->runTurnAsync(
                sessionId,
                input,
                nullptr // headless: 不产生 delta 事件, 排除 client 侧干扰
            );
            ok = !result.hasError;
        }
        if (!ok) {
            std::printf("turn %zu FAILED\n", turn);
            std::fflush(stdout);
            break;
        }
        ++okTurns;
        std::fprintf(stderr, "[memgrow] turn %zu done ok=%d\n", turn, (int)ok);

        if (turn < warmupSkip || (turn + 1) % 5 != 0) {
            continue;
        }
        auto mem  = sampleProcessMemory();
        auto snap = snapshotContainers(agent->agentContext, sessionId);
        std::printf(
            "%-6zu %10.1f %10.1f | %10zu %10.2f | %8zu %8.2f | %8zu",
            turn + 1,
            mem.rssMB,
            mem.privateMB,
            snap.fullHistoryCount,
            snap.fullHistoryBytes / (1024.0 * 1024.0),
            snap.llmMsgCount,
            snap.llmMsgBytes / (1024.0 * 1024.0),
            snap.shareStoreItems
        );
#ifdef _MSC_VER
        if (useCrt) {
            _CrtMemState diff;
            _CrtMemCheckpoint(&crtB);
            if (_CrtMemDifference(&diff, &crtA, &crtB)) {
                // 存活分配字节数增量 (lSizes 为 5 个桶数组)
                size_t live = 0;
                for (int b = 0; b < _MAX_BLOCKS; ++b) {
                    live += (size_t)diff.lSizes[b];
                }
                std::printf(" | %10zu", live);
            } else {
                std::printf(" | %10s", "-");
            }
        } else {
            std::printf(" | %10s", "-");
        }
#endif
        std::printf("\n");
        std::fflush(stdout);
    }

    // 最终快照
    auto mem  = sampleProcessMemory();
    auto snap = snapshotContainers(agent->agentContext, sessionId);
    std::printf(
        "FINAL turn=%zu RSS=%.1fMB Priv=%.1fMB histCnt=%zu histMB=%.2f llmCnt=%zu llmMB=%.2f "
        "shareN=%zu",
        turns,
        mem.rssMB,
        mem.privateMB,
        snap.fullHistoryCount,
        snap.fullHistoryBytes / (1024.0 * 1024.0),
        snap.llmMsgCount,
        snap.llmMsgBytes / (1024.0 * 1024.0),
        snap.shareStoreItems
    );
#ifdef _MSC_VER
    if (useCrt) {
        _CrtMemCheckpoint(&crtB);
        _CrtMemState diff;
        if (_CrtMemDifference(&diff, &crtA, &crtB)) {
            size_t live = 0;
            for (int b = 0; b < _MAX_BLOCKS; ++b) {
                live += (size_t)diff.lSizes[b];
            }
            std::printf(" liveHeapBytes=%zu", live);
        }
    }
#endif
    std::printf("\n");

    // 清理 agent 资源: 无论是否起后台线程, CodeAgent 持有的插件实例/
    // GraphEngine 线程等都须显式释放, 否则跨场景累积导致内存/句柄泄漏
    // (shutdownAll 为同步卸载, 要求调用时无执行中回调 —— 此处各 turn 已
    // 完成, 与单测直调插件后 unloadAsync 语义一致; 要求 io 线程环境,
    // 故在测试协程所在 io_context 上同步调用)
    if (agent->agentContext && agent->agentContext->pluginManager) {
        agent->agentContext->pluginManager->shutdownAll();
    }
    // 停止 agent io_context 后台线程
    if (runAgentCtx) {
        if (agentWorkGuard) {
            agentWorkGuard->reset();
        }
        agent->ioCtx->stop();
        if (agentThread.joinable()) {
            agentThread.join();
        }
    } else {
        // 未起后台线程时 engine 的 poll 线程仍可能持有 ioCtx 工作:
        // 显式 stop + poll_one 排空, 避免残留回调在 agent 析构后触发
        if (agent->ioCtx) {
            agent->ioCtx->stop();
        }
    }

    sim.stop();
    okTurnsOut = static_cast<int>(okTurns);
    co_return static_cast<int>(okTurns);
}

} // namespace

// ---------------------------------------------------------------------------
// 入口: 解析参数并运行场景
// ---------------------------------------------------------------------------

asio::awaitable<TestResult> run_memgrowth_tests() {
    TestResult r;

    // 默认轻量参数: 全量测试须在数分钟内完成。
    // 原默认 60turns×16KB×3场景 在 Windows debug 下单轮可达数秒、全量超 5 分钟,
    // 导致全量测试超时挂死。历史重压场景仍可经环境变量恢复:
    // MEM_TURNS / MEM_RESP_KB / MEM_HUGE_LIMIT / MEM_SCENARIOS(逗号分隔 0/1/2)。
    size_t turns      = 5;
    size_t responseKB = 2;
    size_t warmupSkip = 0;
    bool   hugeLimit  = false;

    // 兼容原占位循环 (无实际效果, 保留以维持原代码结构; 改用安全封装避免 C4996)
    for (int i = 1; !agentxx::util::ApplicationEnv::instance().has("MEM_TURNS") && i < 0; ++i) {
        (void)i;
    }
    // 支持通过环境变量覆盖: MEM_TURNS / MEM_RESP_KB / MEM_HUGE_LIMIT (经全局单例统一封装, Windows
    // 安全)
    if (auto vOpt = agentxx::util::ApplicationEnv::instance().get("MEM_TURNS")) {
        turns = static_cast<size_t>(std::strtoull(vOpt->c_str(), nullptr, 10));
    }
    if (auto vOpt = agentxx::util::ApplicationEnv::instance().get("MEM_RESP_KB")) {
        responseKB = static_cast<size_t>(std::strtoull(vOpt->c_str(), nullptr, 10));
    }
    if (auto vOpt = agentxx::util::ApplicationEnv::instance().get("MEM_HUGE_LIMIT")) {
        hugeLimit = (std::strtoull(vOpt->c_str(), nullptr, 10) != 0);
    }
    // MEM_SCENARIOS: 逗号分隔的子集选择 (0=stream16k, 1=stream16k_ctx, 2=nostream16k),
    // 未设置时默认全跑 (轻量参数下三场景仍很快)
    std::set<int> scenarios{0, 1, 2};
    if (auto vOpt = agentxx::util::ApplicationEnv::instance().get("MEM_SCENARIOS")) {
        scenarios.clear();
        std::string cur;
        for (char c : *vOpt) {
            if (c == ',') {
                if (!cur.empty() && cur[0] >= '0' && cur[0] <= '2') {
                    scenarios.insert(cur[0] - '0');
                }
                cur.clear();
            } else if (c >= '0' && c <= '2') {
                cur.push_back(c);
            }
        }
        if (!cur.empty() && cur[0] >= '0' && cur[0] <= '2') {
            scenarios.insert(cur[0] - '0');
        }
        if (scenarios.empty()) {
            scenarios = {0, 1, 2};
        }
    }

    std::printf(
        "======= Memory Growth Probe (turns=%zu, resp=%zuKB, hugeLimit=%d) =======\n",
        turns,
        responseKB,
        (int)hugeLimit
    );

    // 场景 1: 默认流式 16KB 回复 (agent io_context 未运行 → 复现泄漏)
    int totalOk = 0, totalWant = 0;
    auto runOne
        = [&](size_t t, size_t kb, size_t warm, bool huge, bool ns, bool ctx, const char* label)
        -> asio::awaitable<void> {
        int ok = 0;
        co_await runScenario(t, kb, warm, huge, ns, ctx, label, ok);
        totalOk   += ok;
        totalWant += static_cast<int>(t);
    };
    if (scenarios.count(0)) {
        co_await runOne(turns, responseKB, warmupSkip, hugeLimit, false, false, "stream16k");
    }
    // 场景 2: 流式 16KB + 后台运行 agent io_context (模拟真实 CLI)
    if (scenarios.count(1)) {
        co_await runOne(turns, responseKB, warmupSkip, hugeLimit, false, true, "stream16k_ctx");
    }
    // 场景 3: 非流式 16KB (隔离流式 token 事件路径)
    if (scenarios.count(2)) {
        co_await runOne(turns, responseKB, warmupSkip, hugeLimit, true, false, "nostream16k");
    }

    // 每场景 turns 轮全部成功才算通过: 失败轮数计入 failed, 成功轮数计入 passed
    // (原实现恒返回 passed=1, 轮次失败仅打印 FAILED 文本, 失败不可见)
    r.passed = totalOk;
    r.failed = totalWant - totalOk;
    if (totalWant == 0) {
        r.passed = 1;
    }
    co_return r;
}

} // namespace test
} // namespace agentxx
