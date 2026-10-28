/// test_plugin_example_runtime —— 示例插件 `example_runtime` 全流程
///
/// 与 `plugin_agent_runtime` 的分工:
/// - 那边用手工构造的伪实例直接打 C ABI 表, 验证**主程序侧语义**(预算、冲突、
///   回收、策略);
/// - 这里走**真实插件加载**(真实动态库 + 真实 create/start/mount), 验证插件作者按
///   SDK 写出来的一条完整路径能跑通, 也就是"写聊天平台机器人插件"时要照抄的骨架。
///
/// 覆盖:
/// 1. `start` 里挂自己的根 → 容器里出现该根, 提示词与工具白名单生效;
/// 2. 根上开通道 → 通道就绪;
/// 3. 插件在 agent 上注册的三个工具都在 (`say` / `ping` / `status`);
/// 4. `say` 投一条输入 → 跑一轮 → 上下文里出现这一轮的消息;
/// 5. `ping` 连发三条 → COALESCE 触发器合并成**一轮** (只发起一次模型请求);
/// 6. 卸载插件 → 通道关闭、根被回收、运行时清单清零。
#include "agentxx-test/plugin/test_plugin_example_runtime.h"

#include "agentxx-test/core/test_agent.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/tool_registry.h"
#include "asio/co_spawn.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "asio/use_future.hpp"
#include "fmt/format.h"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
int g_per_passed = 0;
int g_per_failed = 0;
} // namespace

#define XX_TEST_PASSED g_per_passed
#define XX_TEST_FAILED g_per_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

namespace {

using namespace std::chrono_literals;

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

/// 等待条件成立 (在容器 io 线程上轮转等待)
asio::awaitable<bool> waitUntil(
    asio::any_io_executor     ex,
    std::chrono::milliseconds timeout,
    std::function<bool()>     cond
) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            co_return true;
        }
        asio::steady_timer timer(ex);
        timer.expires_after(10ms);
        utilxx_base::AsioErrorCode ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }
    co_return cond();
}

/// 固定等待一段时间 (验证"没有第二次投递"这类否定结论时用)
asio::awaitable<void> delay(asio::any_io_executor ex, std::chrono::milliseconds ms) {
    asio::steady_timer timer(ex);
    timer.expires_after(ms);
    utilxx_base::AsioErrorCode ec;
    co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
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

/// 通道记录里的指定字段 (找不到通道返回空)
std::shared_ptr<plugin::RuntimeChannel>
    channelOf(const std::shared_ptr<plugin::PluginManager>& mgr, std::string_view owner) {
    for (const auto& channel : mgr->runtimeChannels_) {
        if (channel && channel->owner == owner) {
            return channel;
        }
    }
    return nullptr;
}

/// 按归属者取插件挂的根 (容器侧)
std::shared_ptr<agent::BaseAgent>
    rootOf(const std::shared_ptr<agent::AgentHost>& host, std::string_view owner) {
    for (const auto& root : host->roots()) {
        if (root.owner == owner && !root.primary) {
            return root.agent;
        }
    }
    return nullptr;
}

/// 会话上下文里的消息条数 (读不到返回 0)
size_t contextMessageCount(
    const std::shared_ptr<plugin::PluginManager>& mgr,
    int64_t                                       channelId
) {
    const auto text = mgr->channelContextMessages(nullptr, channelId);
    if (text.empty()) {
        return 0;
    }
    try {
        auto j = utilxx_base::Json::parse(text);
        return j.is_array() ? j.size() : 0;
    } catch (const std::exception&) {
        return 0;
    }
}

// ===========================================================================
// 用例主体 (全部在容器 io 线程上跑)
// ===========================================================================

asio::awaitable<void> runBody(
    const std::shared_ptr<asio::io_context>& io,
    std::string_view                         baseUrl,
    std::string                              pluginDir
) {
    const auto ex = io->get_executor();
    const std::string prompt = "你是 example_runtime 示例插件里的助手。";

    // ---- 主程序级上下文: 会话配置的来源 (模型/语言/工作目录) ----
    auto primaryCtx         = std::make_shared<agent::AgentContext>();
    primaryCtx->agentConfig = std::make_shared<agent::AgentConfig>();
    {
        auto& cfg                = *primaryCtx->agentConfig;
        cfg.model.baseUrl        = std::string{baseUrl};
        cfg.model.apiKey         = "EMPTY";
        cfg.model.modelName      = "sim-model";
        cfg.availableModels["test-sim"] = cfg.model;
        cfg.currentModelName            = "test-sim";
        cfg.prompt.systemPrompt         = "You are the built-in assistant.";
        cfg.permissionMode              = agent::PermissionMode::Pass;
        cfg.pluginMountPolicy           = agent::PluginMountPolicy::None;
        cfg.enableSessionStore          = false;
        cfg.enableSubagent              = false;
    }
    primaryCtx->bus = std::make_shared<events::EventBus>(ex);
    // 工具注册表与中间件上下文: 插件挂载时往里注册工具 (与真实启动路径一致)
    primaryCtx->middlewareHandleContext = std::make_shared<middleware::MiddlewareContext>();
    primaryCtx->toolRegistry            = std::make_shared<plugin::ToolRegistry>();

    // ---- 插件管理器: 在容器 io 线程上创建 (记录的就是这条线程) ----
    auto mgr = std::make_shared<plugin::PluginManager>(primaryCtx);
    mgr->setIoExecutor(ex);
    primaryCtx->pluginManager      = mgr;
    primaryCtx->pluginManagerOwned = false;

    // ---- 容器: 插件挂的根与内置根共用这条 io 线程 ----
    agent::AgentHost::Config hostCfg;
    hostCfg.ioCtx    = io;
    hostCfg.maxRoots = 4;
    hostCfg.agentFactory = [mgr](std::shared_ptr<agent::AgentConfig> cfg)
        -> std::shared_ptr<agent::BaseAgent> {
        auto agent = std::make_shared<agent::CodeAgent>(std::move(cfg));
        if (auto ctx = agent->getContext()) {
            ctx->pluginManager      = mgr;
            ctx->pluginManagerOwned = false;
        }
        return agent;
    };
    auto host         = agent::AgentHost::create(hostCfg);
    primaryCtx->host  = host;

    // ---- 主会话先建立挂载: 之后加载的插件挂到它身上 (工具才注册得进来) ----
    primaryCtx->pluginMount = co_await mgr->mountAgentAsync(primaryCtx);

    // ---- 加载示例插件 (真实动态库; create + start 各一次) ----
    agent::PluginConfig pluginCfg;
    pluginCfg.path = pluginDir;
    pluginCfg.args = utilxx_base::Json{
        {"prompt",            prompt                            },
        {"sessionId",         "ex_rt_sess"                      },
        {"triggerDebounceMs", 120                               },
        {"toolWhitelist",     std::vector<std::string>{"agentxx_share_store"}},
    };
    auto inst = co_await mgr->loadPluginAsync(pluginDir, &pluginCfg);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }

    // ===================== 1. start 里挂了根, 并开了通道 =====================
    // 根与通道都是异步 init: 等到通道就绪再断言
    XX_TEST_EXPECT_TRUE(co_await waitUntil(ex, 60s, [&]() {
        auto channel = channelOf(mgr, "example_runtime");
        return channel != nullptr && channel->ready;
    }));
    {
        auto channel = channelOf(mgr, "example_runtime");
        XX_TEST_EXPECT_TRUE(channel != nullptr);
        XX_TEST_EXPECT_EQ(host->rootCount(), size_t{1});
        auto root = rootOf(host, "example_runtime");
        XX_TEST_EXPECT_TRUE(root != nullptr);
        if (root) {
            auto ctx = root->getContext();
            XX_TEST_EXPECT_TRUE(ctx != nullptr);
            if (ctx && ctx->agentConfig) {
                // 根的提示词来自插件给的 config_json, 内置会话的那份一个字没动
                XX_TEST_EXPECT_EQ(ctx->agentConfig->prompt.systemPrompt, prompt);
                XX_TEST_EXPECT_TRUE(
                    primaryCtx->agentConfig->prompt.systemPrompt != prompt
                );
                // 工具白名单只让 share_store 留在该根里
                XX_TEST_EXPECT_TRUE(ctx->agentConfig->enableToolFiltering);
                const auto hasTool = [&ctx](std::string_view name) {
                    return std::find(ctx->toolNames.begin(), ctx->toolNames.end(), name)
                           != ctx->toolNames.end();
                };
                XX_TEST_EXPECT_TRUE(hasTool("agentxx_share_store"));
                XX_TEST_EXPECT_FALSE(hasTool("agentxx_filesystem_read"));
            }
        }
        if (channel) {
            XX_TEST_EXPECT_EQ(channel->sessionId, std::string{"ex_rt_sess"});
            XX_TEST_EXPECT_FALSE(channel->closed);
        }
    }

    // ===================== 2. 插件在 agent 上注册的工具都在 =====================
    XX_TEST_EXPECT_TRUE(primaryCtx->toolRegistry->contains("example_runtime_say"));
    XX_TEST_EXPECT_TRUE(primaryCtx->toolRegistry->contains("example_runtime_ping"));
    XX_TEST_EXPECT_TRUE(primaryCtx->toolRegistry->contains("example_runtime_status"));

    // 状态工具的初始形态: 根与通道都在, 还没跑过轮次
    {
        const auto status = co_await callTool(primaryCtx, "example_runtime_status");
        XX_TEST_EXPECT_FALSE(status.empty());
        const auto j = utilxx_base::Json::parse(status);
        XX_TEST_EXPECT_FALSE(j.value("rootId", std::string{}).empty());
        XX_TEST_EXPECT_TRUE(j.value("channelId", int64_t{0}) > 0);
        XX_TEST_EXPECT_TRUE(j.value("channelReady", false));
        XX_TEST_EXPECT_EQ(j.value("turnCount", -1), 0);
        XX_TEST_EXPECT_TRUE(j.contains("runtime"));
        XX_TEST_EXPECT_EQ(j["runtime"]["roots"].size(), size_t{1});
    }

    // ===================== 3. say 投一条输入 → 跑一轮 =====================
    auto channel = channelOf(mgr, "example_runtime");
    XX_TEST_EXPECT_TRUE(channel != nullptr);
    if (!channel) {
        co_return;
    }
    const int64_t channelId = channel->channelId;
    {
        g_da_sim_response_content     = "runtime channel answer";
        g_da_sim_tool_calls           = utilxx_base::Json::array();
        g_da_sim_tool_calls_remaining = 0;
        g_da_sim_delay_ms             = 0;

        const int requestsBefore = g_da_sim_request_count.load();
        const auto said = co_await callTool(
            primaryCtx,
            "example_runtime_say",
            R"({"text":"你好, 这是从主会话投进去的一条消息"})"
        );
        XX_TEST_EXPECT_FALSE(said.empty());
        const auto saidJson = utilxx_base::Json::parse(said);
        XX_TEST_EXPECT_TRUE(saidJson.value("accepted", false));
        XX_TEST_EXPECT_EQ(saidJson.value("channelId", int64_t{0}), channelId);

        // 一轮跑完: 模型请求 +1, 通道回到空闲, 上下文多出这一轮的消息
        XX_TEST_EXPECT_TRUE(co_await waitUntil(ex, 60s, [&]() {
            auto live = channelOf(mgr, "example_runtime");
            return g_da_sim_request_count.load() > requestsBefore && live != nullptr
                   && !live->turnBusy && contextMessageCount(mgr, channelId) >= 2;
        }));

        // 插件收到了最终回答 (status 工具读的是通道事件里记下来的那份)
        XX_TEST_EXPECT_TRUE(co_await waitUntil(ex, 10s, [&]() {
            auto live = channelOf(mgr, "example_runtime");
            return live != nullptr;
        }));
        const auto status = co_await callTool(primaryCtx, "example_runtime_status");
        const auto j      = utilxx_base::Json::parse(status);
        XX_TEST_EXPECT_EQ(j.value("turnCount", -1), 1);
        XX_TEST_EXPECT_TRUE(
            j.value("lastAnswer", std::string{}).find("runtime channel answer")
            != std::string::npos
        );

        // 参数检查: 空正文被工具拒绝 (工具层抛 invalid_argument, 由节点转成失败文本)
        std::string bad;
        try {
            bad = co_await callTool(primaryCtx, "example_runtime_say", R"({"text":""})");
        } catch (const std::exception& e) {
            bad = e.what();
        }
        XX_TEST_EXPECT_TRUE(bad.find("text 不能为空") != std::string::npos);
    }

    // ===================== 4. ping 连发三条 → 合并成一轮 =====================
    {
        const int requestsBefore = g_da_sim_request_count.load();
        for (int i = 0; i < 3; ++i) {
            const auto pong = co_await callTool(
                primaryCtx,
                "example_runtime_ping",
                fmt::format(R"({{"text":"batch-{}"}})", i)
            );
            XX_TEST_EXPECT_TRUE(utilxx_base::Json::parse(pong).value("rc", -1) == 0);
        }
        // 合并窗口内只投一条: 模型请求只多一次
        XX_TEST_EXPECT_TRUE(co_await waitUntil(ex, 60s, [&]() {
            auto live = channelOf(mgr, "example_runtime");
            return g_da_sim_request_count.load() > requestsBefore && live != nullptr
                   && !live->turnBusy;
        }));
        // 再等两个窗口, 确认没有被补投的第二次
        co_await delay(ex, 400ms);
        XX_TEST_EXPECT_EQ(g_da_sim_request_count.load(), requestsBefore + 1);

        const auto status = co_await callTool(primaryCtx, "example_runtime_status");
        const auto j      = utilxx_base::Json::parse(status);
        XX_TEST_EXPECT_EQ(j.value("turnCount", -1), 2);
        // 三条合并后的正文应该在上下文里: 逐条检查关键字
        const auto messages = mgr->channelContextMessages(nullptr, channelId);
        XX_TEST_EXPECT_TRUE(messages.find("batch-0") != std::string::npos);
        XX_TEST_EXPECT_TRUE(messages.find("batch-1") != std::string::npos);
        XX_TEST_EXPECT_TRUE(messages.find("batch-2") != std::string::npos);
    }

    // ===================== 5. 卸载插件 → 通道关闭、根回收 =====================
    {
        XX_TEST_EXPECT_TRUE(co_await mgr->unloadAsync("example_runtime", 10s));
        // 通道记录被回收 (最后一条事件 CLOSED 已交付给插件)
        XX_TEST_EXPECT_TRUE(channelOf(mgr, "example_runtime") == nullptr);
        // 根从容器里消失
        XX_TEST_EXPECT_TRUE(co_await waitUntil(ex, 20s, [&]() {
            return host->rootCount() == size_t{0};
        }));
        XX_TEST_EXPECT_TRUE(rootOf(host, "example_runtime") == nullptr);
        // 挂在主会话上的工具也随之撤销
        XX_TEST_EXPECT_FALSE(primaryCtx->toolRegistry->contains("example_runtime_say"));
        XX_TEST_EXPECT_TRUE(mgr->list().empty());
    }

    // 收尾: 拆主会话挂载并关管理器 (容器里已无插件根)
    co_await mgr->unmountAgentAsync(primaryCtx->agentId);
    XX_TEST_EXPECT_TRUE(co_await mgr->shutdownAsync(10s));
    primaryCtx->pluginMount.reset();
}

} // namespace

// ===========================================================================
// 入口
// ===========================================================================

asio::awaitable<TestResult> run_plugin_example_runtime_tests() {
    g_per_passed = 0;
    g_per_failed = 0;

    const auto pluginDir = findPluginDir("example_runtime");
    XX_TEST_EXPECT_TRUE(!pluginDir.empty());
    if (pluginDir.empty()) {
        co_return TestResult{g_per_passed, g_per_failed};
    }

    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);
    g_da_sim_response_content     = "runtime channel answer";
    g_da_sim_tool_calls           = utilxx_base::Json::array();
    g_da_sim_tool_calls_remaining = 0;
    g_da_sim_delay_ms             = 0;

    // 容器自己的 io 线程: 根 init、通道驱动、事件回调都在这一条上
    auto io   = std::make_shared<asio::io_context>();
    auto work = asio::make_work_guard(*io);
    auto fut  = asio::co_spawn(*io, runBody(io, baseUrl, pluginDir), asio::use_future);
    std::thread ioThread([io]() {
        io->run();
    });

    try {
        fut.get();
    } catch (const std::exception& e) {
        TEST_FAIL << "plugin_example_runtime body threw: " << e.what() << std::endl;
        g_per_failed++;
    }

    work.reset();
    io->stop();
    if (ioThread.joinable()) {
        ioThread.join();
    }

    std::cout << "[INFO] example_runtime done: passed=" << g_per_passed
              << " failed=" << g_per_failed << std::endl;
    co_return TestResult{g_per_passed, g_per_failed};
}

} // namespace test
} // namespace agentxx
