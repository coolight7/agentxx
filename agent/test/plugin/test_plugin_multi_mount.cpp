/// test_plugin_multi_mount —— 多挂载端到端 (真实插件 `example_multi_mount`)
///
/// 场景:
/// 1. 一份主程序 (PluginManager) + 两个 agent 各建一份挂载, 插件只加载一次
///    (`create`/`start` 各一次), 两个 agent 各有自己的工具与计数;
/// 2. 计数按挂载隔离: A 连调 3 次后 A=4, B 仍为 1;
/// 3. `plugin.mount[].args` 按 agent 覆盖: 两个 agent 拿到的 `config_json` 不同;
/// 4. 拆掉 A 的挂载: A 的工具消失, B 正常, 主程序里的实例仍在 (没有 stop);
/// 5. 重新挂载 A: 计数从 1 开始 (状态随挂载重建), 挂载序号变大;
/// 6. 旧引用的 `seq` 不再匹配 → 主程序拒绝 (`mountOf` 返回空)。
#include "agentxx-test/plugin/test_plugin_multi_mount.h"

#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/tool_registry.h"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
int g_pmm_passed = 0;
int g_pmm_failed = 0;
} // namespace

#define XX_TEST_PASSED g_pmm_passed
#define XX_TEST_FAILED g_pmm_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

namespace {

/// 定位插件库目录 (与其他插件测试同一策略: exe 同目录优先, cwd 回退)
std::string findPluginDir(std::string_view pluginName) {
    std::error_code       ec;
    std::vector<fs::path> candidates;
    if (auto exeDir = executableDir()) {
        candidates.push_back(*exeDir / "plugins" / pluginName);
    }
    candidates.push_back(fs::current_path(ec) / "plugins" / pluginName);
    auto hasLibFile = [](const fs::path& dir) {
        std::error_code                     ec2;
        std::filesystem::directory_iterator it(dir, ec2);
        std::filesystem::directory_iterator end;
        for (; it != end; it.increment(ec2)) {
            auto ext = it->path().extension().string();
            if (ext == ".so" || ext == ".dll" || ext == ".dylib") {
                return true;
            }
        }
        return false;
    };
    for (const auto& c : candidates) {
        if (fs::is_directory(c, ec) && hasLibFile(c)) {
            return c.string();
        }
    }
    return {};
}

/// 建一个 agent 上下文 (共用同一份插件主程序)
///
/// - `pluginManagerOwned = false`: 主程序由本模块持有, agent 退出时不能把它关掉;
/// - `mountArgs` 走 `pluginMountArgs`, 即 yaml `plugin.mount[].args` 的挂载级覆盖。
std::shared_ptr<agent::AgentContext> makeAgent(
    asio::any_io_executor                      ex,
    std::shared_ptr<plugin::PluginManager>     host,
    std::string                                agentId,
    utilxx_base::Json                          mountArgs
) {
    auto ctx                          = std::make_shared<agent::AgentContext>();
    ctx->agentId                      = std::move(agentId);
    ctx->agentConfig                  = std::make_shared<agent::AgentConfig>();
    ctx->agentConfig->pluginMountArgs = std::move(mountArgs);
    ctx->middlewareHandleContext      = std::make_shared<middleware::MiddlewareContext>();
    ctx->bus                          = std::make_shared<events::EventBus>(ex);
    ctx->toolRegistry                 = std::make_shared<plugin::ToolRegistry>();
    ctx->pluginManager                = host;
    ctx->pluginManagerOwned           = false;
    return ctx;
}

/// 调用一个工具并返回结果文本 (工具不存在返回空串)
asio::awaitable<std::string> callTool(
    const std::shared_ptr<agent::AgentContext>& ctx,
    std::string_view                            name,
    std::string_view                            argsJson = "{}"
) {
    auto tool = ctx->toolRegistry ? ctx->toolRegistry->find(name) : nullptr;
    if (!tool) {
        co_return std::string{};
    }
    utilxx_base::Json args = utilxx_base::Json::object();
    if (!argsJson.empty()) {
        args = utilxx_base::Json::parse(std::string{argsJson});
    }
    co_return co_await tool->execute_async(args);
}

/// 解析工具结果里的一个整数字段 (解析失败返回 -1)
int64_t intField(const std::string& json, std::string_view key) {
    try {
        auto parsed = utilxx_base::Json::parse(json);
        if (!parsed.is_object() || !parsed.contains(key) || !parsed[key].is_number_integer()) {
            return -1;
        }
        return parsed[key].get<int64_t>();
    } catch (const std::exception&) {
        return -1;
    }
}

/// 解析工具结果里的字符串字段 (没有返回空串)
std::string stringField(const std::string& json, std::string_view key) {
    try {
        auto parsed = utilxx_base::Json::parse(json);
        if (!parsed.is_object() || !parsed.contains(key) || !parsed[key].is_string()) {
            return {};
        }
        return parsed[key].get<std::string>();
    } catch (const std::exception&) {
        return {};
    }
}

/// 用 agentId + 挂载序号造一个引用 (给 mountOf 校验用)
PluginxxScopeRef scopeRefOf(std::string_view agentId, uint64_t seq) {
    PluginxxScopeRef ref{};
    ref.struct_size = sizeof(PluginxxScopeRef);
    ref.id          = agentxx::plugin::PluginStringView::from(agentId.data(), agentId.size());
    ref.seq         = seq;
    return ref;
}

} // namespace

asio::awaitable<TestResult> run_plugin_multi_mount_tests() {
    g_pmm_passed = 0;
    g_pmm_failed = 0;

    const auto pluginDir = findPluginDir("example_multi_mount");
    XX_TEST_EXPECT_TRUE(!pluginDir.empty());
    if (pluginDir.empty()) {
        co_return TestResult{g_pmm_passed, g_pmm_failed};
    }

    auto ex = co_await asio::this_coro::executor;

    // 一份主程序, 两个 agent; 两个 agent 的挂载级参数不同 (验证 plugin.mount[].args)
    // - 主程序创建时把 A 作为主 agent (插件在 `start` 里的进程级订阅取它的总线)
    auto ctxA = makeAgent(
        ex,
        nullptr,
        "agent_a",
        utilxx_base::Json{{"example_multi_mount", utilxx_base::Json{{"mode", "mode_a"}}}}
    );
    auto host = std::make_shared<plugin::PluginManager>(ctxA);
    host->setIoExecutor(ex);
    ctxA->pluginManager = host;
    auto ctxB           = makeAgent(
        ex,
        host,
        "agent_b",
        utilxx_base::Json{{"example_multi_mount", utilxx_base::Json{{"mode", "mode_b"}}}}
    );
    ctxA->pluginMount = co_await host->mountAgentAsync(ctxA);
    ctxB->pluginMount = co_await host->mountAgentAsync(ctxB);
    XX_TEST_EXPECT_TRUE(ctxA->pluginMount != nullptr);
    XX_TEST_EXPECT_TRUE(ctxB->pluginMount != nullptr);
    if (!ctxA->pluginMount || !ctxB->pluginMount) {
        co_return TestResult{g_pmm_passed, g_pmm_failed};
    }
    const uint64_t seqA = ctxA->pluginMount->mountSeq();
    const uint64_t seqB = ctxB->pluginMount->mountSeq();
    XX_TEST_EXPECT_TRUE(seqA != seqB);

    // ---- 1. 插件只加载一次, 两处各自挂载 ----
    auto inst = co_await host->loadPluginAsync(pluginDir);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return TestResult{g_pmm_passed, g_pmm_failed};
    }
    XX_TEST_EXPECT_EQ(host->list().size(), size_t{1}); ///< 实例表只有一条 (进程一份)
    XX_TEST_EXPECT_TRUE(inst->anyMounted());
    XX_TEST_EXPECT_TRUE(ctxA->toolRegistry->contains("multi_mount_incr"));
    XX_TEST_EXPECT_TRUE(ctxB->toolRegistry->contains("multi_mount_incr"));
    XX_TEST_EXPECT_EQ(ctxA->pluginMount->mountedCount(), size_t{1});
    XX_TEST_EXPECT_EQ(ctxB->pluginMount->mountedCount(), size_t{1});

    // ---- 2. 计数按挂载隔离: A 连 3 次 -> 4; B 仍为 1 ----
    {
        const std::string a1 = co_await callTool(ctxA, "multi_mount_incr");
        const std::string a2 = co_await callTool(ctxA, "multi_mount_incr");
        const std::string a3 = co_await callTool(ctxA, "multi_mount_incr");
        const std::string a4 = co_await callTool(ctxA, "multi_mount_incr");
        XX_TEST_EXPECT_EQ(intField(a1, "count"), int64_t{2});
        XX_TEST_EXPECT_EQ(intField(a2, "count"), int64_t{3});
        XX_TEST_EXPECT_EQ(intField(a3, "count"), int64_t{4});
        XX_TEST_EXPECT_EQ(intField(a4, "count"), int64_t{5});
        // 结果里带的 agentId / 序号必须是本挂载的 (回调拿到的是自己的引用)
        XX_TEST_EXPECT_TRUE(stringField(a4, "agentId") == "agent_a");
        XX_TEST_EXPECT_EQ(intField(a4, "seq"), static_cast<int64_t>(seqA));

        const std::string bState = co_await callTool(ctxB, "multi_mount_state");
        XX_TEST_EXPECT_EQ(intField(bState, "count"), int64_t{1}); ///< B 未被调用, 仍是初值
        XX_TEST_EXPECT_TRUE(stringField(bState, "agentId") == "agent_b");
        XX_TEST_EXPECT_EQ(intField(bState, "seq"), static_cast<int64_t>(seqB));
    }

    // ---- 3. plugin.mount[].args 按 agent 覆盖 ----
    {
        const std::string aState = co_await callTool(ctxA, "multi_mount_state");
        const std::string bState = co_await callTool(ctxB, "multi_mount_state");
        XX_TEST_EXPECT_TRUE(aState.find("mode_a") != std::string::npos);
        XX_TEST_EXPECT_TRUE(bState.find("mode_b") != std::string::npos);
        XX_TEST_EXPECT_TRUE(aState.find("mode_b") == std::string::npos);
        XX_TEST_EXPECT_TRUE(bState.find("mode_a") == std::string::npos);
    }

    // ---- 4. 拆掉 A 的挂载: A 工具消失, B 正常, 主程序实例仍在 ----
    {
        co_await host->unmountAgentAsync("agent_a");
        XX_TEST_EXPECT_TRUE(ctxA->pluginMount != nullptr); ///< 上下文里的挂载对象还在 (只拆了实体)
        XX_TEST_EXPECT_FALSE(ctxA->toolRegistry->contains("multi_mount_incr"));
        XX_TEST_EXPECT_TRUE(ctxB->toolRegistry->contains("multi_mount_incr"));
        XX_TEST_EXPECT_TRUE(host->find("example_multi_mount") != nullptr); ///< 进程级实例没被卸载
        XX_TEST_EXPECT_FALSE(inst->lifecycleStopped);
        const std::string bState = co_await callTool(ctxB, "multi_mount_state");
        XX_TEST_EXPECT_EQ(intField(bState, "count"), int64_t{1});
        XX_TEST_EXPECT_TRUE(stringField(bState, "agentId") == "agent_b");
    }

    // ---- 5. 重新挂载 A: 计数从 1 开始 (状态随挂载重建), 挂载序号变大 ----
    ctxA->pluginMount = co_await host->mountAgentAsync(ctxA);
    XX_TEST_EXPECT_TRUE(ctxA->pluginMount != nullptr);
    if (ctxA->pluginMount) {
        const uint64_t newSeqA = ctxA->pluginMount->mountSeq();
        XX_TEST_EXPECT_TRUE(newSeqA > seqA); ///< 序号单调递增
        XX_TEST_EXPECT_TRUE(ctxA->toolRegistry->contains("multi_mount_incr"));
        const std::string state = co_await callTool(ctxA, "multi_mount_state");
        XX_TEST_EXPECT_EQ(intField(state, "count"), int64_t{1});
        XX_TEST_EXPECT_EQ(intField(state, "seq"), static_cast<int64_t>(newSeqA));

        // ---- 6. 旧引用 (旧挂载序号) 被拒绝, 新引用可用 ----
        auto oldRef = scopeRefOf("agent_a", seqA);
        auto newRef = scopeRefOf("agent_a", newSeqA);
        XX_TEST_EXPECT_TRUE(host->mountOf(&oldRef) == nullptr);
        XX_TEST_EXPECT_TRUE(host->mountOf(&newRef) == ctxA->pluginMount.get());
        auto unknownRef = scopeRefOf("agent_zzz", newSeqA);
        XX_TEST_EXPECT_TRUE(host->mountOf(&unknownRef) == nullptr);
        XX_TEST_EXPECT_TRUE(host->mountOf(nullptr) == nullptr);
    }

    // B 的挂载全程不受影响
    XX_TEST_EXPECT_TRUE(ctxB->toolRegistry->contains("multi_mount_incr"));

    // ---- 7. 拆全部挂载 + 关闭主程序: 实例 stop/destroy 一次, 挂载表清空 ----
    {
        co_await host->unmountAgentAsync("agent_a");
        co_await host->unmountAgentAsync("agent_b");
        XX_TEST_EXPECT_TRUE(host->mounts().empty());
        XX_TEST_EXPECT_FALSE(ctxA->toolRegistry->contains("multi_mount_incr"));
        XX_TEST_EXPECT_FALSE(ctxB->toolRegistry->contains("multi_mount_incr"));
        bool closed = co_await host->shutdownAsync(std::chrono::seconds{10});
        XX_TEST_EXPECT_TRUE(closed);
        XX_TEST_EXPECT_TRUE(host->list().empty());
        XX_TEST_EXPECT_TRUE(inst->lifecycleStopped);
    }

    // 上下文里的挂载句柄要随 agent 一起放掉 (真实路径是 AgentContext::shutdownPluginsAsync)
    ctxA->pluginMount.reset();
    ctxB->pluginMount.reset();

    std::cout << "[INFO] multi mount done: passed=" << g_pmm_passed << " failed=" << g_pmm_failed
              << std::endl;
    co_return TestResult{g_pmm_passed, g_pmm_failed};
}

} // namespace test
} // namespace agentxx
