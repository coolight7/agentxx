/// test_plugin_host_mount —— 一份插件主程序 + 多个 agent 挂载
///
/// 这一层是"加载一次是进程的事, 注册一次是 agent 的事"的正面验证:
/// 插件实例只 `create`/`start` 一次, 每个 agent 各有一份挂载, 挂在某个 agent 上的
/// 工具 / 提示词贡献 / 钩子 / 事件订阅互不影响, 拆挂载只影响被拆的那一个。
///
/// 覆盖 (编号对应设计文档 `04-implementation.md` §3.1):
/// 1 一主程序两 agent / 2 工具隔离 / 3 提示词只影响本 agent / 4 钩子只在本 agent 派发 /
/// 5 卸载一个 agent / 6 主程序关闭 / 7 挂载失败只回滚该 agent / 8 事件订阅按挂载分开 /
/// 9 无引用的注册被拒 / 10 重复挂载幂等 / 11 工具名冲突 / 12 第三个 agent /
/// 13 版本门槛 / 14 引用校验 (空 / 未知 id / 过期序号) / 15 22 张表与挂载表查询
#include "agentxx-test/plugin/test_plugin_host_mount.h"

#include "agentxx/agent/context.h"
#include "agentxx/agent/assembly_snapshot.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/feature/points.h"
#include "agentxx/feature/registry.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/api/plugin_iface_meta.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/tool_registry.h"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "neograph/graph/node.h"
#include "neograph/graph/registry.h"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
int g_phm_passed = 0;
int g_phm_failed = 0;
} // namespace

#define XX_TEST_PASSED g_phm_passed
#define XX_TEST_FAILED g_phm_failed

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

/// 测试用管理器: 把受保护的 `hostVtable` 拿出来, 直接经插件侧接口表验证拒绝路径
struct TestHost : public plugin::PluginManager {
    using plugin::PluginManager::PluginManager;
    using plugin::PluginManager::hostVtable;
};

/// 建一个 agent 上下文 (共用同一份插件主程序)
std::shared_ptr<agent::AgentContext>
    makeAgent(asio::any_io_executor ex, std::shared_ptr<TestHost> host, std::string agentId) {
    auto ctx                          = std::make_shared<agent::AgentContext>();
    ctx->agentId                      = std::move(agentId);
    ctx->agentConfig                  = std::make_shared<agent::AgentConfig>();
    ctx->middlewareHandleContext      = std::make_shared<middleware::MiddlewareContext>();
    ctx->bus                          = std::make_shared<events::EventBus>(ex);
    ctx->toolRegistry                 = std::make_shared<plugin::ToolRegistry>();
    ctx->graphRegistry                = std::make_shared<neograph::graph::GraphRegistry>();
    ctx->features                     = std::make_shared<feature::Registry>();
    feature::ContextPointOptions pointOptions;
    feature::registerContextPoints(*ctx->features, pointOptions);
    ctx->pluginManager      = host;
    ctx->pluginManagerOwned = false; ///< 主程序由本模块持有, agent 退出时不能关掉它
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

/// 某个 agent 上注册表里的工具名出现次数 (验证重复登记)
size_t toolCount(const std::shared_ptr<agent::AgentContext>& ctx, std::string_view name) {
    if (!ctx->toolRegistry) {
        return 0;
    }
    size_t hits = 0;
    for (const auto& item : ctx->toolRegistry->names()) {
        if (item == name) {
            ++hits;
        }
    }
    return hits;
}

/// 用 agentId + 挂载序号造一个引用 (给 mountOf / 接口表校验用)
PluginxxScopeRef scopeRefOf(std::string_view agentId, uint64_t seq) {
    PluginxxScopeRef ref{};
    ref.struct_size = sizeof(PluginxxScopeRef);
    ref.id          = agentxx::plugin::PluginStringView::from(agentId.data(), agentId.size());
    ref.seq         = seq;
    return ref;
}

/// 某个挂载该实例上还活着的订阅数 (按作用域标识筛)
size_t subscriptionsOf(const plugin::PluginInstance& inst, std::string_view scopeId) {
    size_t hits = 0;
    for (const auto& sub : inst.subscriptions) {
        if (sub && sub->scopeId == scopeId) {
            ++hits;
        }
    }
    return hits;
}

/// 直接经插件侧接口表登记一个工具时的回调 (只为验证 vtable 入口的引用校验)
void* PLUGINXX_CALL probeToolStart(
    void*,
    const PluginxxStringView*,
    const PluginxxStringView*,
    const PluginxxStringView*,
    const PluginxxOperatorNotify* notify,
    PluginxxString*
) {
    if (notify && notify->done) {
        const auto payload = agentxx::plugin::PluginStringView::fromCstr(R"({"ok":true})");
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &payload);
    }
    return nullptr;
}

/// 取插件侧看到的某张接口表 (hostView + query_interface)
const void* queryTable(const PluginxxHost* host, const char* iid) {
    if (!host || !host->vtable || !host->vtable->query_interface) {
        return nullptr;
    }
    auto view = agentxx::plugin::PluginStringView::fromCstr(iid);
    return host->vtable->query_interface(host, &view);
}

/// core 层钩子处理器的调用计数 (验证"钩子只在本 agent 派发")
struct HookTrace {
    int calls = 0;
};

void* PLUGINXX_CALL traceHookStart(
    void*                         user_data,
    int32_t,
    const PluginxxStringView*,
    const PluginxxOperatorNotify*,
    PluginxxString*
) {
    if (auto* trace = static_cast<HookTrace*>(user_data)) {
        ++trace->calls;
    }
    return nullptr;
}

/// 一次派发用的最小图输入
neograph::graph::NodeInput makeNodeInput(neograph::graph::GraphState& state) {
    static neograph::graph::RunContext runCtx;
    runCtx.thread_id = "host_mount_session";
    return neograph::graph::NodeInput{state, runCtx, nullptr};
}

} // namespace

asio::awaitable<TestResult> run_plugin_host_mount_tests() {
    g_phm_passed = 0;
    g_phm_failed = 0;

    const auto exampleDir = findPluginDir("example_plugin");
    XX_TEST_EXPECT_TRUE(!exampleDir.empty());
    if (exampleDir.empty()) {
        co_return TestResult{g_phm_passed, g_phm_failed};
    }

    auto ex = co_await asio::this_coro::executor;

    // 先建 A, 再用它当主程序的主 agent (插件在 `start` 里的进程级订阅取它的总线)
    auto agentA = makeAgent(ex, nullptr, "agent_a");
    auto host   = std::make_shared<TestHost>(agentA);
    host->setIoExecutor(ex);
    agentA->pluginManager = host;

    auto agentB = makeAgent(ex, host, "agent_b");
    agentA->pluginMount = co_await host->mountAgentAsync(agentA);
    agentB->pluginMount = co_await host->mountAgentAsync(agentB);
    XX_TEST_EXPECT_TRUE(agentA->pluginMount != nullptr);
    XX_TEST_EXPECT_TRUE(agentB->pluginMount != nullptr);
    if (!agentA->pluginMount || !agentB->pluginMount) {
        co_return TestResult{g_phm_passed, g_phm_failed};
    }
    const uint64_t seqA = agentA->pluginMount->mountSeq();
    const uint64_t seqB = agentB->pluginMount->mountSeq();
    XX_TEST_EXPECT_TRUE(seqA != seqB); ///< 每份挂载有自己的序号

    // ================= 1. 一主程序两 agent: 实例只有一份, 两处都挂上了 =================
    auto inst = co_await host->loadPluginAsync(exampleDir);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return TestResult{g_phm_passed, g_phm_failed};
    }
    XX_TEST_EXPECT_EQ(host->list().size(), size_t{1});
    XX_TEST_EXPECT_TRUE(inst->anyMounted());
    XX_TEST_EXPECT_EQ(inst->mounts.size(), size_t{2}); ///< 两个 agent 各一条挂载记录
    XX_TEST_EXPECT_EQ(agentA->pluginMount->mountedCount(), size_t{1});
    XX_TEST_EXPECT_EQ(agentB->pluginMount->mountedCount(), size_t{1});
    XX_TEST_EXPECT_TRUE(agentA->toolRegistry->contains("example_echo"));
    XX_TEST_EXPECT_TRUE(agentB->toolRegistry->contains("example_echo"));

    // 加载快照: 进程角度 (`plugins`) 与本 agent 角度 (`plugins_agent`) 各报告自己的数字
    {
        const auto snapshot = agent::buildRuntimeSnapshot(*agentA);
        XX_TEST_EXPECT_TRUE(snapshot["plugins"].is_array());
        XX_TEST_EXPECT_EQ(snapshot["plugins"].size(), size_t{1});
        XX_TEST_EXPECT_EQ(snapshot["plugins"][0].value("mount_count", 0), 2);
        XX_TEST_EXPECT_EQ(snapshot["plugins"][0]["mount_agents"].size(), size_t{2});

        XX_TEST_EXPECT_TRUE(snapshot["plugins_agent"].is_object());
        XX_TEST_EXPECT_EQ(
            snapshot["plugins_agent"].value("agent_id", std::string{}),
            std::string{"agent_a"}
        );
        XX_TEST_EXPECT_EQ(snapshot["plugins_agent"].value("mount_seq", 0), 1);
        XX_TEST_EXPECT_EQ(
            snapshot["plugins_agent"].value("mount_policy", std::string{}),
            std::string{"all"}
        );
        XX_TEST_EXPECT_EQ(snapshot["plugins_agent"].value("count", 0), 1);
        const auto& mountPlugin = snapshot["plugins_agent"]["plugins"][0];
        XX_TEST_EXPECT_EQ(mountPlugin.value("name", std::string{}), std::string{"example_plugin"});
        XX_TEST_EXPECT_GE(mountPlugin.value("tool_count", 0), 1);
        XX_TEST_EXPECT_GE(mountPlugin.value("registration_total", 0), 1);
    }

    // ================= 2. 工具状态隔离: 各调各的, 结果不串 =================
    {
        const auto outA = co_await callTool(agentA, "example_echo", R"({"from":"host-A"})");
        const auto outB = co_await callTool(agentB, "example_echo", R"({"from":"host-B"})");
        XX_TEST_EXPECT_TRUE(outA.find("host-A") != std::string::npos);
        XX_TEST_EXPECT_TRUE(outB.find("host-B") != std::string::npos);
        XX_TEST_EXPECT_TRUE(outA.find("host-B") == std::string::npos);
        XX_TEST_EXPECT_TRUE(outB.find("host-A") == std::string::npos);
    }

    // ================= 3. 提示词只影响本 agent =================
    {
        const auto promptA = agentA->pluginMount->getPromptJson();
        const auto promptB = agentB->pluginMount->getPromptJson();
        XX_TEST_EXPECT_TRUE(promptA.find("example_echo") != std::string::npos);
        XX_TEST_EXPECT_TRUE(promptB.find("example_echo") != std::string::npos);

        // 拆掉 B 上的这一个实例: B 的提示词贡献随之撤销, A 不受影响
        co_await host->unmountOneAsync(agentB->pluginMount, inst);
        const auto promptAAfter = agentA->pluginMount->getPromptJson();
        const auto promptBAfter = agentB->pluginMount->getPromptJson();
        XX_TEST_EXPECT_TRUE(promptAAfter.find("example_echo") != std::string::npos);
        XX_TEST_EXPECT_TRUE(promptBAfter.find("example_echo") == std::string::npos);
        XX_TEST_EXPECT_FALSE(agentB->toolRegistry->contains("example_echo"));

        // 重新挂到 B: 插件在自己的 mount 回调里重新声明
        XX_TEST_EXPECT_TRUE(co_await host->mountOneAsync(agentB->pluginMount, inst));
        XX_TEST_EXPECT_TRUE(agentB->toolRegistry->contains("example_echo"));
        XX_TEST_EXPECT_TRUE(agentB->pluginMount->getPromptJson().find("example_echo")
                            != std::string::npos);
    }

    // ================= 4. 钩子只在本 agent 派发 =================
    {
        HookTrace trace;
        AgentxxPluginHookSpec spec{};
        spec.struct_size = sizeof(AgentxxPluginHookSpec);
        spec.point       = AGENTXX_PLUGIN_HOOK_AGENT_START;
        spec.hook_start  = &traceHookStart;
        spec.user_data   = &trace;
        const int64_t handle
            = agentA->pluginMount->addCoreHookHandler(AGENTXX_PLUGIN_HOOK_AGENT_START, 0, "host_mount_test", spec);
        XX_TEST_EXPECT_TRUE(handle > 0);

        neograph::graph::GraphState stateA;
        auto                        inputA = makeNodeInput(stateA);
        co_await agentA->pluginMount->dispatchHook(AGENTXX_PLUGIN_HOOK_AGENT_START, inputA);
        XX_TEST_EXPECT_EQ(trace.calls, 1);

        // 同一个点在 B 上派发: B 的注册表里没有这个处理器, 计数不变
        neograph::graph::GraphState stateB;
        auto                        inputB = makeNodeInput(stateB);
        co_await agentB->pluginMount->dispatchHook(AGENTXX_PLUGIN_HOOK_AGENT_START, inputB);
        XX_TEST_EXPECT_EQ(trace.calls, 1);

        XX_TEST_EXPECT_TRUE(agentA->pluginMount->removeCoreHookHandler(handle));
        co_await agentA->pluginMount->dispatchHook(AGENTXX_PLUGIN_HOOK_AGENT_START, inputA);
        XX_TEST_EXPECT_EQ(trace.calls, 1);
    }

    // ================= 8. 事件订阅按挂载分开 =================
    {
        // example_plugin 在 mount 里订阅两个主题: 每个挂载各一份 (作用域标识区分)
        const size_t subA = subscriptionsOf(*inst, "agent_a");
        const size_t subB = subscriptionsOf(*inst, "agent_b");
        XX_TEST_EXPECT_EQ(subA, size_t{2});
        XX_TEST_EXPECT_EQ(subB, size_t{2});
        XX_TEST_EXPECT_TRUE(subA + subB <= inst->subscriptions.size());

        // 拆掉 A 的一个实例: 只有 A 那份订阅被撤销
        co_await host->unmountOneAsync(agentA->pluginMount, inst);
        XX_TEST_EXPECT_EQ(subscriptionsOf(*inst, "agent_a"), size_t{0});
        XX_TEST_EXPECT_EQ(subscriptionsOf(*inst, "agent_b"), size_t{2});
        XX_TEST_EXPECT_TRUE(co_await host->mountOneAsync(agentA->pluginMount, inst));
        XX_TEST_EXPECT_EQ(subscriptionsOf(*inst, "agent_a"), size_t{2});
    }

    // ================= 10. 重复挂载幂等 =================
    {
        auto sameMount = co_await host->mountAgentAsync(agentA);
        XX_TEST_EXPECT_TRUE(sameMount == agentA->pluginMount);
        XX_TEST_EXPECT_EQ(sameMount->mountSeq(), seqA); ///< 序号没有重新分配
        XX_TEST_EXPECT_EQ(sameMount->mountedCount(), size_t{1});
        XX_TEST_EXPECT_EQ(toolCount(agentA, "example_echo"), size_t{1});
        XX_TEST_EXPECT_EQ(host->mounts().size(), size_t{2});
    }

    // ================= 11. 工具名冲突只影响本 agent =================
    {
        AgentxxPluginToolSpec spec{};
        spec.struct_size     = sizeof(AgentxxPluginToolSpec);
        spec.name            = agentxx::plugin::PluginStringView::fromCstr("example_echo");
        spec.description     = agentxx::plugin::PluginStringView::fromCstr("duplicate name probe");
        spec.parameters_json = agentxx::plugin::PluginStringView::fromCstr("{}");
        spec.execute_start   = &probeToolStart;
        // 同名工具已在本挂载的注册表里 → 拒绝, 不产生第二个
        XX_TEST_EXPECT_TRUE(agentB->pluginMount->registerTool(inst.get(), &spec) != 0);
        XX_TEST_EXPECT_EQ(toolCount(agentB, "example_echo"), size_t{1});
        XX_TEST_EXPECT_EQ(toolCount(agentA, "example_echo"), size_t{1});
    }

    // ================= 12. 第三个 agent =================
    auto agentC = makeAgent(ex, host, "agent_c");
    {
        agentC->pluginMount = co_await host->mountAgentAsync(agentC);
        XX_TEST_EXPECT_TRUE(agentC->pluginMount != nullptr);
        if (agentC->pluginMount) {
            XX_TEST_EXPECT_EQ(agentC->pluginMount->mountedCount(), size_t{1});
            XX_TEST_EXPECT_TRUE(agentC->toolRegistry->contains("example_echo"));
            XX_TEST_EXPECT_EQ(host->mounts().size(), size_t{3});
            const auto outC = co_await callTool(agentC, "example_echo", R"({"from":"host-C"})");
            XX_TEST_EXPECT_TRUE(outC.find("host-C") != std::string::npos);
            XX_TEST_EXPECT_EQ(toolCount(agentA, "example_echo"), size_t{1});
        }
    }

    // ================= 7. 挂载失败只回滚该 agent =================
#ifdef AGENTXX_TEST_START_FAIL_PLUGIN_PATH
    {
        auto bad = co_await host->loadPluginAsync(AGENTXX_TEST_START_FAIL_PLUGIN_PATH);
        XX_TEST_EXPECT_TRUE(bad != nullptr); ///< 插件本身加载成功 (create/start 没问题)
        if (bad) {
            // mount 回调主动失败: 该实例在所有 agent 上都没有生效注册
            XX_TEST_EXPECT_FALSE(bad->anyMounted());
            for (const auto& ctx : {agentA, agentB, agentC}) {
                XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("dso_rollback_tool"));
            }
            // 其他插件不受影响
            XX_TEST_EXPECT_TRUE(agentA->toolRegistry->contains("example_echo"));
            XX_TEST_EXPECT_TRUE(agentB->toolRegistry->contains("example_echo"));
            XX_TEST_EXPECT_TRUE(co_await host->unloadAsync("test_start_fail_plugin"));
            XX_TEST_EXPECT_TRUE(host->find("test_start_fail_plugin") == nullptr);
        }
    }
#endif

    // ================= 9 / 14. 引用校验: 空 / 未知 id / 过期序号 =================
    {
        const PluginxxHost* hostView = inst->hostView();
        XX_TEST_EXPECT_TRUE(hostView != nullptr);
        const auto* tools = static_cast<const AgentxxPluginToolsIface*>(
            queryTable(hostView, AGENTXX_PLUGIN_IFACE_AGENT_TOOLS)
        );
        XX_TEST_EXPECT_TRUE(tools != nullptr);
        if (tools != nullptr && tools->register_tool != nullptr) {
            AgentxxPluginToolSpec spec{};
            spec.struct_size     = sizeof(AgentxxPluginToolSpec);
            spec.name            = agentxx::plugin::PluginStringView::fromCstr("host_mount_probe");
            spec.description     = agentxx::plugin::PluginStringView::fromCstr("ref probe");
            spec.parameters_json = agentxx::plugin::PluginStringView::fromCstr("{}");
            spec.execute_start   = &probeToolStart;

            // 9. 不带引用 = 等同于在 `start` 里注册: 拒绝且无副作用
            XX_TEST_EXPECT_EQ(
                tools->register_tool(hostView, nullptr, &spec),
                int32_t{AGENTXX_PLUGIN_ERR_NO_AGENT}
            );
            XX_TEST_EXPECT_FALSE(agentA->toolRegistry->contains("host_mount_probe"));

            // 14. 未知 agentId
            auto unknown = scopeRefOf("agent_zzz", seqA);
            XX_TEST_EXPECT_TRUE(host->mountOf(&unknown) == nullptr);
            XX_TEST_EXPECT_EQ(
                tools->register_tool(hostView, &unknown, &spec),
                int32_t{AGENTXX_PLUGIN_ERR_NO_AGENT}
            );
            XX_TEST_EXPECT_FALSE(agentA->toolRegistry->contains("host_mount_probe"));

            // 14. 过期序号: 拆掉再挂上, 上一轮的序号不再匹配
            auto        refA     = scopeRefOf("agent_a", seqA);
            const auto* mountNow = host->mountOf(&refA);
            XX_TEST_EXPECT_TRUE(mountNow == agentA->pluginMount.get());
            co_await host->unmountAgentAsync("agent_a");
            agentA->pluginMount = co_await host->mountAgentAsync(agentA);
            XX_TEST_EXPECT_TRUE(agentA->pluginMount != nullptr);
            if (agentA->pluginMount) {
                const uint64_t seqA2 = agentA->pluginMount->mountSeq();
                XX_TEST_EXPECT_TRUE(seqA2 > seqA);
                auto stale = scopeRefOf("agent_a", seqA);
                auto fresh = scopeRefOf("agent_a", seqA2);
                XX_TEST_EXPECT_TRUE(host->mountOf(&stale) == nullptr);
                XX_TEST_EXPECT_TRUE(host->mountOf(&fresh) == agentA->pluginMount.get());
                // 过期引用登记被拒, 新引用登记成功 (证明拒绝不是"永远失败")
                XX_TEST_EXPECT_EQ(
                    tools->register_tool(hostView, &stale, &spec),
                    int32_t{AGENTXX_PLUGIN_ERR_NO_AGENT}
                );
                XX_TEST_EXPECT_EQ(tools->register_tool(hostView, &fresh, &spec), int32_t{0});
                XX_TEST_EXPECT_TRUE(agentA->toolRegistry->contains("host_mount_probe"));
                // 收尾: 撤销这个探针工具
                auto probeName = agentxx::plugin::PluginStringView::fromCstr("host_mount_probe");
                XX_TEST_EXPECT_EQ(
                    tools->unregister_tool(hostView, &fresh, &probeName),
                    int32_t{0}
                );
                XX_TEST_EXPECT_FALSE(agentA->toolRegistry->contains("host_mount_probe"));
            }
        }
    }

    // ================= 13. 版本门槛 =================
    {
        XX_TEST_EXPECT_TRUE(PLUGINXX_API_VERSION >= 2); ///< 全局契约: 旧插件在加载期被拒
        XX_TEST_EXPECT_TRUE(PLUGINXX_MIN_API_VERSION >= 2);

        AgentxxPluginToolsIface probe{};
        probe.version     = 1;
        probe.struct_size = sizeof(AgentxxPluginToolsIface);
        XX_TEST_EXPECT_TRUE(
            pluginxx::validateInterface<AgentxxPluginToolsIface>(&probe) == nullptr
        );
        probe.version = AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION;
        XX_TEST_EXPECT_TRUE(
            pluginxx::validateInterface<AgentxxPluginToolsIface>(&probe) != nullptr
        );
        // 基线字节数不足同样被拒
        probe.struct_size = AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_BASELINE_SIZE - 1;
        XX_TEST_EXPECT_TRUE(
            pluginxx::validateInterface<AgentxxPluginToolsIface>(&probe) == nullptr
        );
    }

    // ================= 15. 22 张表可查 + 挂载表内容 =================
    {
        XX_TEST_EXPECT_EQ(plugin::PluginManager::kInterfaceTableCount, size_t{22});
        const PluginxxHost* hostView = inst->hostView();
        const char*         iids[]   = {
            PLUGINXX_IFACE_LOG,
            PLUGINXX_IFACE_JSON,
            PLUGINXX_IFACE_CONFIG,
            PLUGINXX_IFACE_PLUGINS,
            PLUGINXX_IFACE_EVENTS,
            PLUGINXX_IFACE_SCHEDULER,
            PLUGINXX_IFACE_COROUTINE_RUNTIME,
            PLUGINXX_IFACE_TASKS,
            PLUGINXX_IFACE_CANCEL,
            PLUGINXX_IFACE_CAPABILITIES,
            AGENTXX_PLUGIN_IFACE_AGENT_TOOLS,
            AGENTXX_PLUGIN_IFACE_AGENT_PERMISSION,
            AGENTXX_PLUGIN_IFACE_AGENT_HOOKS,
            AGENTXX_PLUGIN_IFACE_AGENT_SESSION,
            AGENTXX_PLUGIN_IFACE_AGENT_CONTEXT,
            AGENTXX_PLUGIN_IFACE_AGENT_MODEL,
            AGENTXX_PLUGIN_IFACE_AGENT_PROMPT,
            AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES,
            AGENTXX_PLUGIN_IFACE_AGENT_GRAPH,
            AGENTXX_PLUGIN_IFACE_AGENT_FEATURE,
            AGENTXX_PLUGIN_IFACE_AGENT_MOUNT,
            AGENTXX_PLUGIN_IFACE_AGENT_RUNTIME,
        };
        XX_TEST_EXPECT_EQ(sizeof(iids) / sizeof(iids[0]), plugin::PluginManager::kInterfaceTableCount);
        for (const char* iid : iids) {
            XX_TEST_EXPECT_TRUE(queryTable(hostView, iid) != nullptr);
        }

        // 挂载表: 三份挂载都能查到, 各自报自己的 agentId / 已挂插件
        utilxx_base::Json mounts = utilxx_base::Json::parse(host->listMountsJson());
        XX_TEST_EXPECT_TRUE(mounts.is_array());
        XX_TEST_EXPECT_EQ(mounts.size(), size_t{3});
        for (const auto& item : mounts) {
            XX_TEST_EXPECT_TRUE(item.contains("agentId"));
            XX_TEST_EXPECT_TRUE(item.contains("mountSeq"));
            const auto names = item.value("plugins", utilxx_base::Json::array());
            XX_TEST_EXPECT_TRUE(names.is_array());
            XX_TEST_EXPECT_FALSE(names.empty());
        }
        utilxx_base::Json info = utilxx_base::Json::parse(host->mountInfoJson("agent_b"));
        XX_TEST_EXPECT_TRUE(info.is_object());
        XX_TEST_EXPECT_TRUE(info.value("agentId", std::string{}) == "agent_b");
        XX_TEST_EXPECT_TRUE(host->mountInfoJson("agent_zzz").empty());
    }

    // ================= 5. 卸载一个 agent: 只影响它自己 =================
    {
        co_await host->unmountAgentAsync("agent_b");
        XX_TEST_EXPECT_FALSE(agentB->toolRegistry->contains("example_echo"));
        XX_TEST_EXPECT_TRUE(agentA->toolRegistry->contains("example_echo"));
        XX_TEST_EXPECT_TRUE(agentC->toolRegistry->contains("example_echo"));
        XX_TEST_EXPECT_TRUE(host->find("example_plugin") != nullptr); ///< 主程序没有关掉实例
        XX_TEST_EXPECT_FALSE(inst->lifecycleStopped);
        XX_TEST_EXPECT_EQ(host->mounts().size(), size_t{2});
        XX_TEST_EXPECT_TRUE(host->findMountById("agent_b") == nullptr);
    }

    // ================= 6. 主程序关闭: stop/destroy 一次, 挂载表清空 =================
    {
        co_await host->unmountAgentAsync("agent_a");
        co_await host->unmountAgentAsync("agent_c");
        XX_TEST_EXPECT_TRUE(host->mounts().empty());
        XX_TEST_EXPECT_FALSE(agentA->toolRegistry->contains("example_echo"));
        XX_TEST_EXPECT_FALSE(agentC->toolRegistry->contains("example_echo"));

        XX_TEST_EXPECT_TRUE(co_await host->shutdownAsync(std::chrono::seconds{15}));
        XX_TEST_EXPECT_TRUE(host->list().empty());
        XX_TEST_EXPECT_TRUE(inst->lifecycleStopped);
    }

    // 上下文里的挂载句柄要随 agent 一起放掉 (真实路径是 AgentContext::shutdownPluginsAsync)
    agentA->pluginMount.reset();
    agentB->pluginMount.reset();
    agentC->pluginMount.reset();

    std::cout << "[INFO] host mount done: passed=" << g_phm_passed << " failed=" << g_phm_failed
              << std::endl;
    co_return TestResult{g_phm_passed, g_phm_failed};
}

} // namespace test
} // namespace agentxx
