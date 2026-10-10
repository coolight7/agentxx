/// test_plugin_cleanup —— 插件注册可逆、独占 slot 与教学式错误
/// (计划 PLG-1 / PLG-4 / PLG-7)
///
/// 背景:
/// - PLG-1: 一个插件实例贡献的东西分散在工具注册表/权限中间件/中间件链/图注册表/
///   事件总线/能力表/提示词/资源应用器里, "禁用/卸载后清干净了没有"需要一份可比较
///   的定义。这里用 `PluginManager::registrationInventory` 做基线断言。
/// - PLG-4: 执行图定义是"独占能力"的典型: 两个插件交替覆盖后, 任何一方卸载都
///   无法判断该恢复成哪一份定义。这里验证占用/拒绝/释放后回到原定义 (内置)。
/// - PLG-7: 装载失败时宿主给出"照什么改"的建议 (路径/清单/导出符号)。
///
/// 覆盖:
/// 1. 注册清单基线: 装载 → 各计数与注册效果; 禁用 → 全部摘除且实例记录保留;
///    启用 → start 事务重新声明; 卸载 → 实例消失、注册与提示词贡献无残留
/// 2. 提示词贡献多 owner: 后写者生效, 卸载只删自己的贡献 (不会把旧值写回),
///    外部直写会 rebase 基础值
/// 3. 执行图定义独占 slot: 先到者占用, 后到者被拒绝, 占用者卸载恢复原定义
///    (含真实插件 example_graph_node 在 start 里设置图的路径)
/// 4. 装载失败建议: 空路径 / 路径不存在 / 无清单 / 清单非法 / entry 库缺失 /
///    库缺入口符号 / 内置名不存在
#include "agentxx-test/plugin/test_plugin_cleanup.h"

#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/tool_registry.h"
#include "neograph/graph/registry.h"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"
#include "utilxx_base/string_util.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_pc_passed = 0;
int g_pc_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_pc_passed
#define XX_TEST_FAILED g_pc_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

namespace {

/// 定位插件库目录 (与 test_plugins.cpp 同一策略: exe 同目录优先, cwd 回退)
std::string findPluginDir(const char* pluginName) {
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
    return std::string{"plugins/"} + pluginName;
}

std::string makeTempDir(std::string_view tag) {
    auto dir = fs::temp_directory_path()
               / fmt::format(
                   "agentxx_pc_test_{}_{}",
                   tag,
                   std::chrono::steady_clock::now().time_since_epoch().count()
               );
    fs::create_directories(dir);
    return dir.string();
}

void removeTempDir(const std::string& dir) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        std::error_code ec;
        fs::remove_all(utilxx_base::utf8ToPath(dir), ec);
        if (!ec) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
}

void writeTextFile(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(utilxx_base::utf8ToPath(path.string()), std::ios::binary);
    ofs << text;
}

/// 最小宿主夹具: AgentContext + 中间件上下文 + 总线 + 工具注册表 + 插件管理器
struct Fixture {
    std::shared_ptr<agentxx::agent::AgentContext>    ctx;
    std::shared_ptr<agentxx::plugin::PluginManager>  mgr;
};

asio::awaitable<Fixture> makeFixture() {
    Fixture f;
    f.ctx                     = std::make_shared<agentxx::agent::AgentContext>();
    f.ctx->agentConfig        = std::make_shared<agentxx::agent::AgentConfig>();
    f.ctx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
    f.ctx->bus                = std::make_shared<agentxx::events::EventBus>(
        co_await asio::this_coro::executor
    );
    f.ctx->toolRegistry = std::make_shared<agentxx::plugin::ToolRegistry>();
    // 图注册表: 插件经 graph 接口表注册自定义节点类型 (BaseAgent::init 会注入)
    f.ctx->graphRegistry = std::make_shared<neograph::graph::GraphRegistry>();
    f.mgr               = std::make_shared<agentxx::plugin::PluginManager>(f.ctx);
    f.ctx->pluginManager = f.mgr;
    // 装配 io executor (与 BaseAgent::init 一致): 跨线程 vtable 调用经真实 post
    f.mgr->setIoExecutor(co_await asio::this_coro::executor);
    co_return f;
}

/// 轮询等待条件成立 (启用事务是投递到 io 线程的协程)
asio::awaitable<bool> waitFor(std::function<bool()> cond, int timeoutMs = 5000) {
    auto       ex       = co_await asio::this_coro::executor;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMs};
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            co_return true;
        }
        asio::steady_timer timer(ex);
        timer.expires_after(std::chrono::milliseconds{10});
        utilxx_base::AsioErrorCode ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }
    co_return cond();
}

/// 提示词 append 段当前取值 (空串 = 不存在)
std::string appendSectionText(const agentxx::agent::AgentPrompt& prompt, const std::string& key) {
    auto it = prompt.appendSystemPrompts.find(key);
    return it == prompt.appendSystemPrompts.end() ? std::string{} : it->second;
}

} // namespace

// ===========================================================================
// 1. 注册清单基线: 装载 / 禁用 / 启用 / 卸载
// ===========================================================================
namespace {

asio::awaitable<void> test_registration_inventory_baseline() {
    auto      f           = co_await makeFixture();
    const auto pluginPath = findPluginDir("example_plugin");

    // 装载前的基线: 没有工具、没有提示词贡献
    XX_TEST_EXPECT_FALSE(f.ctx->toolRegistry->contains("example_echo"));
    XX_TEST_EXPECT_EQ(
        appendSectionText(f.ctx->agentConfig->prompt, "toolPrompt"),
        std::string{}
    );
    XX_TEST_EXPECT_EQ(f.mgr->graphDefinitionOwner(), std::string{});

    auto inst = co_await f.mgr->loadPluginAsync(pluginPath);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }

    // 装载后: 统一注册清单的每一项都对得上 example_plugin 实际注册的内容
    const auto inv = f.mgr->registrationInventory(*inst);
    TEST_INFO << "[plugin_cleanup] example_plugin inventory: tools=" << inv.tools
              << " hooks=" << inv.hooks << " events=" << inv.eventSubscriptions
              << " caps=" << inv.capabilities << " promptKeys=" << inv.promptKeys
              << " total=" << inv.total() << std::endl;
    XX_TEST_EXPECT_GE(inv.tools, size_t{5}); // echo / caller / sleep / bridge / polled_timer
    XX_TEST_EXPECT_EQ(inv.hooks, size_t{1});
    XX_TEST_EXPECT_GE(inv.eventSubscriptions, size_t{2});
    XX_TEST_EXPECT_EQ(inv.capabilities, size_t{1});
    XX_TEST_EXPECT_EQ(inv.permissionTools, size_t{0});
    XX_TEST_EXPECT_EQ(inv.graphNodeTypes, size_t{0});
    XX_TEST_EXPECT_EQ(inv.featurePoints, size_t{0}); ///< example_plugin 不登记功能点
    XX_TEST_EXPECT_EQ(inv.featureImpls, size_t{0});
    XX_TEST_EXPECT_EQ(inv.skillDirs, size_t{0});
    XX_TEST_EXPECT_EQ(inv.memoryFiles, size_t{0});
    XX_TEST_EXPECT_EQ(inv.mcpNamespaces, size_t{0});
    XX_TEST_EXPECT_FALSE(inv.ownsGraphDefinition);
    XX_TEST_EXPECT_GE(inv.promptKeys, size_t{1}); // toolPrompt.example_echo
    XX_TEST_EXPECT_GE(inv.total(), size_t{10});

    // 注册清单与 list() 的诊断字段同源
    {
        auto views = f.mgr->list();
        XX_TEST_EXPECT_EQ(views.size(), size_t{1});
        if (views.size() == 1) {
            XX_TEST_EXPECT_EQ(views[0].registrationTotal, inv.total());
            XX_TEST_EXPECT_EQ(views[0].promptKeyCount, inv.promptKeys);
            XX_TEST_EXPECT_EQ(views[0].ownsGraphDefinition, inv.ownsGraphDefinition);
            XX_TEST_EXPECT_EQ(views[0].featurePointCount, inv.featurePoints);
            XX_TEST_EXPECT_EQ(views[0].featureImplCount, inv.featureImpls);
        }
    }

    // 装载后的实际效果: 工具有、中间件在链上、提示词贡献生效
    XX_TEST_EXPECT_TRUE(f.ctx->toolRegistry->contains("example_echo"));
    const size_t handlesBefore = f.ctx->middlewareHandleContext->handles.size();
    XX_TEST_EXPECT_GE(handlesBefore, size_t{1});

    // ---- 禁用: 宿主侧生效注册全部摘除, 实例内记录保留 (启用时 start 重新声明) ----
    f.mgr->disable("example_plugin");
    XX_TEST_EXPECT_FALSE(inst->enabled);
    XX_TEST_EXPECT_FALSE(f.ctx->toolRegistry->contains("example_echo"));
    XX_TEST_EXPECT_TRUE(inst->toolNames.size() >= 5); ///< 记录保留 (供 enable 重新声明)
    {
        const auto disabled = f.mgr->registrationInventory(*inst);
        XX_TEST_EXPECT_EQ(disabled.tools, size_t{0});
        XX_TEST_EXPECT_EQ(disabled.hooks, size_t{0});
        XX_TEST_EXPECT_EQ(disabled.eventSubscriptions, size_t{0});
        XX_TEST_EXPECT_EQ(disabled.capabilities, size_t{0});
        XX_TEST_EXPECT_EQ(disabled.promptKeys, size_t{0});
        XX_TEST_EXPECT_EQ(disabled.featurePoints, size_t{0});
        XX_TEST_EXPECT_EQ(disabled.featureImpls, size_t{0});
        XX_TEST_EXPECT_EQ(disabled.total(), size_t{0}); ///< 回到基线
    }

    // ---- 启用: start 事务重新声明 (投递到 io 线程协程, 轮询等待) ----
    f.mgr->enable("example_plugin");
    const bool enabledAgain = co_await waitFor([&]() {
        return f.ctx->toolRegistry->contains("example_echo");
    });
    if (!enabledAgain) {
        TEST_INFO << "[plugin_cleanup] enable 后未恢复注册: enabled=" << inst->enabled
                  << " started=" << inst->lifecycleStarted << " stopped=" << inst->lifecycleStopped
                  << " toolRecords=" << inst->toolNames.size() << std::endl;
    }
    XX_TEST_EXPECT_TRUE(enabledAgain);
    {
        // 重新启用后按"插件 start 到底声明了什么"计数: 工具/hook/能力由 start 重新声明
        // 因而与装载时一致; 事件订阅与提示词贡献是**条件性**的 (插件只补缺失项,
        // 宿主也可能自己增减订阅), 因此只断言"至少恢复", 不断言与装载时完全相等。
        const auto enabled = f.mgr->registrationInventory(*inst);
        XX_TEST_EXPECT_GE(enabled.tools, inv.tools);
        XX_TEST_EXPECT_EQ(enabled.hooks, inv.hooks);
        XX_TEST_EXPECT_GE(enabled.eventSubscriptions, size_t{2});
        XX_TEST_EXPECT_GE(enabled.capabilities, inv.capabilities);
        XX_TEST_EXPECT_GE(enabled.total(), size_t{9});
    }

    // ---- 卸载: 实例消失, 注册与提示词贡献无残留 ----
    XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("example_plugin", std::chrono::seconds{5}));
    XX_TEST_EXPECT_TRUE(f.mgr->find("example_plugin") == nullptr);
    XX_TEST_EXPECT_TRUE(f.mgr->list().empty());
    XX_TEST_EXPECT_FALSE(f.ctx->toolRegistry->contains("example_echo"));
    XX_TEST_EXPECT_FALSE(f.ctx->toolRegistry->contains("example_caller"));
    // 提示词里的插件贡献也回到基础值 (toolPrompt 段不再有 example_echo 条目)
    {
        const auto& toolPrompt = f.ctx->agentConfig->prompt.toolPrompt;
        XX_TEST_EXPECT_TRUE(toolPrompt.find("example_echo") == toolPrompt.end());
    }
    co_return;
}

// ===========================================================================
// 2. 提示词贡献多 owner: 后写者生效, 卸载只删自己的贡献
// ===========================================================================
asio::awaitable<void> test_prompt_contribution_recompute() {
    auto f = co_await makeFixture();

    auto instA = co_await f.mgr->loadPluginAsync(findPluginDir("example_plugin"));
    auto instB = co_await f.mgr->loadPluginAsync(findPluginDir("agentxx_math"));
    XX_TEST_EXPECT_TRUE(instA != nullptr);
    XX_TEST_EXPECT_TRUE(instB != nullptr);
    if (!instA || !instB) {
        co_return;
    }

    const std::string probeKey = "plugin_cleanup_probe";
    XX_TEST_EXPECT_EQ(appendSectionText(f.ctx->agentConfig->prompt, probeKey), std::string{});

    // A 先贡献
    XX_TEST_EXPECT_EQ(
        f.mgr->setPromptJson(
            instA.get(),
            agentxx::plugin::PluginStringView::fromCstr(
                R"({"appendSystemPrompts":{"plugin_cleanup_probe":"from-A"}})"
            )
        ),
        0
    );
    XX_TEST_EXPECT_EQ(appendSectionText(f.ctx->agentConfig->prompt, probeKey), std::string{"from-A"});

    // B 后贡献: 后写者生效 (贡献按 sequence 顺序应用)
    XX_TEST_EXPECT_EQ(
        f.mgr->setPromptJson(
            instB.get(),
            agentxx::plugin::PluginStringView::fromCstr(
                R"({"appendSystemPrompts":{"plugin_cleanup_probe":"from-B"}})"
            )
        ),
        0
    );
    XX_TEST_EXPECT_EQ(appendSectionText(f.ctx->agentConfig->prompt, probeKey), std::string{"from-B"});
    {
        const auto invA = f.mgr->registrationInventory(*instA);
        const auto invB = f.mgr->registrationInventory(*instB);
        XX_TEST_EXPECT_GE(invA.promptKeys, size_t{1});
        XX_TEST_EXPECT_GE(invB.promptKeys, size_t{1});
    }

    // 卸载 A: 只删除 A 的贡献, B 的取值仍然生效 (不回退成 A 的旧值/base)
    XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("example_plugin", std::chrono::seconds{5}));
    XX_TEST_EXPECT_EQ(appendSectionText(f.ctx->agentConfig->prompt, probeKey), std::string{"from-B"});

    // 外部直写 (用户/宿主代码): 直写立即生效 (模型看到的是直写值),
    // 贡献会在下一次"合成"时被识别为新基础值并在其上重新应用
    f.ctx->agentConfig->prompt.setAppendSection(probeKey, "external", 0, "user");
    XX_TEST_EXPECT_EQ(appendSectionText(f.ctx->agentConfig->prompt, probeKey), std::string{"external"});

    // B 再贡献一次: 触发合成 —— 外部值成为新基础值, B 的贡献在其上生效
    XX_TEST_EXPECT_EQ(
        f.mgr->setPromptJson(
            instB.get(),
            agentxx::plugin::PluginStringView::fromCstr(
                R"({"appendSystemPrompts":{"plugin_cleanup_probe":"from-B2"}})"
            )
        ),
        0
    );
    XX_TEST_EXPECT_EQ(appendSectionText(f.ctx->agentConfig->prompt, probeKey), std::string{"from-B2"});

    // 卸载 B: 外部直写的值 (已成为基础值) 保留, 不会被清成"不存在"
    XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("agentxx_math", std::chrono::seconds{5}));
    XX_TEST_EXPECT_EQ(appendSectionText(f.ctx->agentConfig->prompt, probeKey), std::string{"external"});

    // 清理: 移除探针键, 避免影响同进程的其它用例
    f.ctx->agentConfig->prompt.removeAppendSection(probeKey);
    XX_TEST_EXPECT_EQ(appendSectionText(f.ctx->agentConfig->prompt, probeKey), std::string{});
    co_return;
}

// ===========================================================================
// 3. 执行图定义独占 slot
// ===========================================================================
asio::awaitable<void> test_graph_definition_slot() {
    auto f = co_await makeFixture();

    const std::string baseJson = f.mgr->getGraphJson();
    XX_TEST_EXPECT_EQ(f.mgr->graphDefinitionOwner(), std::string{});

    auto instA = co_await f.mgr->loadPluginAsync(findPluginDir("example_plugin"));
    auto instB = co_await f.mgr->loadPluginAsync(findPluginDir("agentxx_math"));
    XX_TEST_EXPECT_TRUE(instA != nullptr);
    XX_TEST_EXPECT_TRUE(instB != nullptr);
    if (!instA || !instB) {
        co_return;
    }

    // A 占用: 记录基础定义并成为占用者
    XX_TEST_EXPECT_EQ(
        f.mgr->setGraphJson(
            instA.get(),
            agentxx::plugin::PluginStringView::fromCstr(
                R"({"name":"plugin-graph-a","nodes":{},"edges":[]})"
            )
        ),
        0
    );
    XX_TEST_EXPECT_EQ(f.mgr->graphDefinitionOwner(), std::string{"example_plugin"});
    XX_TEST_EXPECT_TRUE(f.mgr->getGraphJson().find("plugin-graph-a") != std::string::npos);
    {
        const auto invA = f.mgr->registrationInventory(*instA);
        XX_TEST_EXPECT_TRUE(invA.ownsGraphDefinition);
        const auto invB = f.mgr->registrationInventory(*instB);
        XX_TEST_EXPECT_FALSE(invB.ownsGraphDefinition);
    }

    // B 尝试占用: 被拒绝, 且不覆盖 A 的定义 (独占语义)
    XX_TEST_EXPECT_EQ(
        f.mgr->setGraphJson(
            instB.get(),
            agentxx::plugin::PluginStringView::fromCstr(
                R"({"name":"plugin-graph-b","nodes":{},"edges":[]})"
            )
        ),
        -1
    );
    XX_TEST_EXPECT_EQ(f.mgr->graphDefinitionOwner(), std::string{"example_plugin"});
    XX_TEST_EXPECT_TRUE(f.mgr->getGraphJson().find("plugin-graph-b") == std::string::npos);

    // 占用者自己可以继续修改
    XX_TEST_EXPECT_EQ(
        f.mgr->setGraphJson(
            instA.get(),
            agentxx::plugin::PluginStringView::fromCstr(
                R"({"name":"plugin-graph-a2","nodes":{},"edges":[]})"
            )
        ),
        0
    );
    XX_TEST_EXPECT_TRUE(f.mgr->getGraphJson().find("plugin-graph-a2") != std::string::npos);

    // 占用者卸载: 恢复到占用前的定义 (内置/宿主定义), slot 释放
    XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("example_plugin", std::chrono::seconds{5}));
    XX_TEST_EXPECT_EQ(f.mgr->graphDefinitionOwner(), std::string{});
    XX_TEST_EXPECT_EQ(f.mgr->getGraphJson(), baseJson);

    // slot 释放后 B 可以占用
    XX_TEST_EXPECT_EQ(
        f.mgr->setGraphJson(
            instB.get(),
            agentxx::plugin::PluginStringView::fromCstr(
                R"({"name":"plugin-graph-b","nodes":{},"edges":[]})"
            )
        ),
        0
    );
    XX_TEST_EXPECT_EQ(f.mgr->graphDefinitionOwner(), std::string{"agentxx_math"});

    // 禁用也会释放 (与卸载同一路径), 恢复基础定义
    f.mgr->disable("agentxx_math");
    XX_TEST_EXPECT_EQ(f.mgr->graphDefinitionOwner(), std::string{});
    XX_TEST_EXPECT_EQ(f.mgr->getGraphJson(), baseJson);
    co_return;
}

/// 真实插件路径: example_graph_node 在自己的 start 里设置执行图
asio::awaitable<void> test_graph_slot_with_real_plugin() {
    auto f = co_await makeFixture();

    const std::string baseJson = f.mgr->getGraphJson();
    auto inst = co_await f.mgr->loadPluginAsync(findPluginDir("example_graph_node"));
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return;
    }
    // 插件 start 里设置了图: 占用者应当是它, 图定义已变更
    XX_TEST_EXPECT_EQ(f.mgr->graphDefinitionOwner(), std::string{"example_graph_node"});
    XX_TEST_EXPECT_TRUE(f.mgr->getGraphJson() != baseJson);

    XX_TEST_EXPECT_TRUE(co_await f.mgr->unloadAsync("example_graph_node", std::chrono::seconds{5}));
    XX_TEST_EXPECT_EQ(f.mgr->graphDefinitionOwner(), std::string{});
    XX_TEST_EXPECT_EQ(f.mgr->getGraphJson(), baseJson);
    co_return;
}

// ===========================================================================
// 4. 装载失败建议 (PLG-7)
// ===========================================================================
asio::awaitable<void> test_load_failure_advice() {
    auto f = co_await makeFixture();
    auto& mgr = *f.mgr;
    // 空路径
    {
        const auto msg = mgr.diagnosePluginPath("");
        XX_TEST_EXPECT_TRUE(msg.find("path 为空") != std::string::npos);
    }
    // 路径不存在
    {
        const auto msg = mgr.diagnosePluginPath("/definitely/not/here/agentxx_plugin");
        XX_TEST_EXPECT_TRUE(msg.find("路径不存在") != std::string::npos);
    }
    // 内置名不存在
    {
        const auto msg = mgr.diagnosePluginPath("builtin://__no_such_builtin__");
        XX_TEST_EXPECT_TRUE(msg.find("内置") != std::string::npos);
    }

    const auto root = makeTempDir("advice");

    // 目录里没有 plugin.yaml
    {
        const auto dir = fs::path{root} / "no_manifest";
        fs::create_directories(dir);
        const auto msg = mgr.diagnosePluginPath(dir.string());
        XX_TEST_EXPECT_TRUE(msg.find("没有 plugin.yaml") != std::string::npos);
    }
    // plugin.yaml 语法非法
    {
        const auto dir = fs::path{root} / "bad_yaml";
        fs::create_directories(dir);
        writeTextFile(dir / "plugin.yaml", "name: [unclosed\nentry: x\n  bad: : :\n");
        const auto msg = mgr.diagnosePluginPath(dir.string());
        XX_TEST_EXPECT_TRUE(msg.find("解析失败") != std::string::npos);
    }
    // 清单 entry 指向的库文件不存在
    {
        const auto dir = fs::path{root} / "missing_entry";
        fs::create_directories(dir);
        writeTextFile(
            dir / "plugin.yaml",
            "name: missing_entry\nversion: 1.0.0\nentry: libmissing_entry.so\ndepends:\n"
        );
        const auto msg = mgr.diagnosePluginPath(dir.string());
        XX_TEST_EXPECT_TRUE(msg.find("不存在") != std::string::npos);
    }
    // 库文件存在但缺宿主入口符号 (用主库本体当"缺少入口符号的库")
    {
        std::string candidate;
        if (auto exeDir = executableDir()) {
            // 用例运行在 {build}/exec (与主库同目录); 直接从 exe 同目录与其上级寻找
            // 非插件动态库 (主库本体没有插件入口符号)
            const char* libNames[] = {
#if XX_IS_WIN_D
                "libagentxxd.dll",
#else
                "libagentxxd.so",
#endif
            };
            for (const char* name : libNames) {
                for (const auto& dir :
                     {*exeDir, exeDir->parent_path(), exeDir->parent_path().parent_path() / "exec"}) {
                    const auto probe = (dir / name).string();
                    if (fs::exists(utilxx_base::utf8ToPath(probe))) {
                        candidate = probe;
                        break;
                    }
                }
                if (!candidate.empty()) {
                    break;
                }
            }
        }
        if (!candidate.empty() && fs::exists(utilxx_base::utf8ToPath(candidate))) {
            const auto msg = mgr.diagnosePluginPath(candidate);
            XX_TEST_EXPECT_TRUE(msg.find("缺少宿主入口符号") != std::string::npos);
        } else {
            TEST_INFO << "[plugin_cleanup] 跳过缺入口符号用例: 未找到可用的非插件动态库"
                      << std::endl;
        }
    }

    removeTempDir(root);
}

} // namespace

asio::awaitable<TestResult> run_plugin_cleanup_tests() {
    const int passedBefore = g_pc_passed;
    const int failedBefore = g_pc_failed;

    try {
        co_await test_registration_inventory_baseline();
        co_await test_prompt_contribution_recompute();
        co_await test_graph_definition_slot();
        co_await test_graph_slot_with_real_plugin();
        co_await test_load_failure_advice();
    } catch (const std::exception& e) {
        TEST_FAIL << "plugin_cleanup suite exception: " << e.what() << std::endl;
        g_pc_failed++;
    }

    co_return TestResult{g_pc_passed - passedBefore, g_pc_failed - failedBefore};
}

} // namespace test
} // namespace agentxx
