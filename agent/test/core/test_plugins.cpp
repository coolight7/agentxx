#include "test_plugins.h"

#include "agentxx/agent/context.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/util/async_offload.h"
#include "agentxx/util/log.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace agentxx {
namespace test {

int g_plugin_passed = 0;
int g_plugin_failed = 0;

/// 定位示例插件库 (与测试可执行同目录, 见 test/CMakeLists.txt)
static std::string findExamplePluginPath() {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto            cwd = fs::current_path(ec) / "plugins" / "example_plugin";
    if (!ec) {
        return cwd.string();
    }
    // 异常兜底: 返回相对目录路径, loadPluginAsync 解析失败时日志暴露原因
    return "plugins/example_plugin";
}

static asio::awaitable<void> sleepMs(int ms) {
    auto timer
        = asio::steady_timer(co_await asio::this_coro::executor, std::chrono::milliseconds{ms});
    co_await timer.async_wait(asio::use_awaitable);
}

asio::awaitable<TestResult> run_plugin_tests() {
    g_plugin_passed = 0;
    g_plugin_failed = 0;

    // ---- 1. 构造最小 AgentContext (io 线程环境, 与库内无锁模型一致) ----
    auto ctx                     = std::make_shared<agentxx::agent::AgentContext>();
    ctx->agentConfig             = std::make_shared<agentxx::agent::AgentConfig>();
    ctx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
    ctx->bus = std::make_shared<agentxx::middleware::EventBus>(co_await asio::this_coro::executor);
    ctx->toolRegistry  = std::make_shared<agentxx::plugin::ToolRegistry>();
    ctx->pluginManager = std::make_shared<agentxx::plugin::PluginManager>(ctx);
    // 装配 io executor (与 BaseAgent::init 一致): 跨线程 (JS 线程/线程池) 的
    // vtable 调用经真实 post 到 io 线程执行, 而非测试默认的"伪 io 线程"直连
    ctx->pluginManager->setIoExecutor(co_await asio::this_coro::executor);

    // ---- 2. 加载示例插件 (目录 + plugin.yaml 清单分派) ----
    auto path = findExamplePluginPath();
    XX_TEST_EXPECT_TRUE(path.find("example_plugin") != std::string::npos);
    auto inst = co_await ctx->pluginManager->loadPluginAsync(path);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        XX_TEST_EXPECT_TRUE(false);
        co_return TestResult{g_plugin_passed, g_plugin_failed};
    }
    XX_TEST_EXPECT_EQ(inst->name, "example_plugin");
    XX_TEST_EXPECT_EQ(inst->version, "1.0.0");
    XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("example_echo"));
    XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("example_caller"));
    XX_TEST_EXPECT_TRUE(ctx->pluginManager->capabilities()->has("example.demo"));
    XX_TEST_EXPECT_TRUE(ctx->pluginManager->hasCapability("example.demo") == 1);

    // ---- 3. 工具执行 (同步 C 回调经线程池卸载) ----
    {
        auto tool = ctx->toolRegistry->find("example_echo");
        XX_TEST_EXPECT_TRUE(tool != nullptr);
        if (tool) {
            auto out = co_await tool->execute_async(neograph::json{
                {"thread_id", "t1"   },
                {"hello",     "world"},
            });
            auto j   = neograph::json::parse(out);
            XX_TEST_EXPECT_EQ(j["echo"]["hello"].get<std::string>(), "world");
            XX_TEST_EXPECT_EQ(j["thread_id"].get<std::string>(), "t1");
        }
    }

    // ---- 4. 插件互调 (call_tool) ----
    {
        auto tool = ctx->toolRegistry->find("example_caller");
        XX_TEST_EXPECT_TRUE(tool != nullptr);
        if (tool) {
            auto out = co_await tool->execute_async(neograph::json{
                {"thread_id", "t1"},
                {"x",         42  },
            });
            auto j   = neograph::json::parse(out);
            XX_TEST_EXPECT_EQ(j["via_call_tool"]["echo"]["x"].get<int>(), 42);
        }
    }

    // ---- 5. 钩子注册: 中间件句柄入栈 + 钩子点记录 ----
    {
        const auto& handles = ctx->middlewareHandleContext->handles;
        bool        found   = std::any_of(handles.begin(), handles.end(), [](const auto& h) {
            return h->name == "example_plugin_middleware";
        });
        XX_TEST_EXPECT_TRUE(found);
        XX_TEST_EXPECT_EQ(inst->hookRegistrations.size(), size_t{1});
        XX_TEST_EXPECT_TRUE(inst->middleware != nullptr);
    }

    // ---- 6. 事件订阅/发布回环 (plugin.demo.topic) ----
    {
        // 测试侧订阅同一 topic (JSON 字符串载荷)
        bool received = false;
        auto subId    = ctx->bus->get<std::string>("plugin.demo.topic")
                         .subscribe([&](const std::string& data) -> asio::awaitable<void> {
                             if (data.find("\"k\"") != std::string::npos) {
                                 received = true;
                             }
                             co_return;
                         });
        int rc = ctx->pluginManager->publish("demo.topic", R"({"k":"v"})");
        XX_TEST_EXPECT_EQ(rc, 0);
        co_await sleepMs(150);
        XX_TEST_EXPECT_TRUE(received);
        ctx->bus->get<std::string>("plugin.demo.topic").unsubscribe(subId);
    }

    // ---- 7. 禁用 → 工具摘除/钩子停用; 启用 → 恢复 ----
    {
        ctx->pluginManager->disable("example_plugin");
        XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("example_echo"));
        XX_TEST_EXPECT_FALSE(ctx->pluginManager->registry()->contains("example_caller"));
        // 无轮次执行时 disable 立即摘除中间件 (hooks 停用; enable 时按记录重建)
        XX_TEST_EXPECT_TRUE(inst->middleware == nullptr);
        XX_TEST_EXPECT_FALSE(inst->enabled);

        ctx->pluginManager->enable("example_plugin");
        XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("example_echo"));
        XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("example_caller"));
        XX_TEST_EXPECT_TRUE(inst->middleware != nullptr);
        XX_TEST_EXPECT_FALSE(inst->middleware->disabled);
        XX_TEST_EXPECT_TRUE(inst->enabled);
    }

    // ---- 8. 卸载: 注册残留全部清理 + 插件移除 + 能力移除 ----
    {
        auto ok = co_await ctx->pluginManager->unloadAsync("example_plugin");
        XX_TEST_EXPECT_TRUE(ok);
        XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("example_echo"));
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("example_plugin") == nullptr);
        XX_TEST_EXPECT_FALSE(ctx->pluginManager->capabilities()->has("example.demo"));
        // 中间件应从 handles 摘除 (pendingCleanup 于下轮 flush; 无轮次时直接摘除)
        const auto& handles = ctx->middlewareHandleContext->handles;
        bool        found   = std::any_of(handles.begin(), handles.end(), [](const auto& h) {
            return h->name == "example_plugin_middleware";
        });
        XX_TEST_EXPECT_FALSE(found);
        // 工具对象应已释放 (inflight 归零后 dlclose)
        XX_TEST_EXPECT_EQ(inst->tools.size(), size_t{0});
    }

    // ---- 9. 重复卸载拒绝 ----
    {
        auto ok = co_await ctx->pluginManager->unloadAsync("example_plugin");
        XX_TEST_EXPECT_FALSE(ok);
    }

    // ---- 10. 名称冲突: 静态工具名集合 ----
    {
        // 最小非抽象工具 (仅测试注册表冲突检测用)
        struct DummyTool : agentxx::tools::XXToolBase {
            using XXToolBase::XXToolBase;

            neograph::ChatTool get_definition() const override {
                return neograph::ChatTool{};
            }

            asio::awaitable<std::string> execute_async(const neograph::json&) override {
                co_return std::string{};
            }
        };

        ctx->toolRegistry->setStaticToolNames({"builtin_tool_a"});
        // 插件工具注册冲突在目录分派 (loadPluginAsync) 路径覆盖, 此处验证注册表拒绝同名静态工具
        auto dummy = std::make_shared<DummyTool>("builtin_tool_a", ctx);
        XX_TEST_EXPECT_FALSE(ctx->toolRegistry->registerTool("builtin_tool_a", dummy));
        XX_TEST_EXPECT_TRUE(ctx->toolRegistry->registerTool("plugin_ok_tool", dummy));
        ctx->toolRegistry->unregisterTool("plugin_ok_tool");
    }

    // ---- 11. 插件列表 ----
    {
        // 重新加载用于 list 验证
        auto inst2 = co_await ctx->pluginManager->loadPluginAsync(path);
        XX_TEST_EXPECT_TRUE(inst2 != nullptr);
        if (inst2) {
            auto list  = ctx->pluginManager->list();
            bool found = std::any_of(list.begin(), list.end(), [](const auto& item) {
                return item.name == "example_plugin" && item.enabled;
            });
            XX_TEST_EXPECT_TRUE(found);
            co_await ctx->pluginManager->unloadAsync("example_plugin");
        }
    }

    // JS 引擎库与脚本插件目录 (第 12~26 段共用; 函数级声明)
    // findExamplePluginPath() 已含 plugins/ 前缀 (…/plugins/example_plugin),
    // parent_path() 即 plugins/ 目录, 直接拼接插件子目录名即可
    namespace fs       = std::filesystem;
    auto        jsPath = findExamplePluginPath();
    std::string jsLib  = (fs::path(jsPath).parent_path() / "agentxx_javascript_engine").string();
    std::string jsDir  = (fs::path(jsPath).parent_path() / "example_js").string();

    // ---- 12. JS 引擎插件加载 (二期) ----
    {
        auto engineInst = co_await ctx->pluginManager->loadPluginAsync(jsLib);
        XX_TEST_EXPECT_TRUE(engineInst != nullptr);
        if (!engineInst) {
            XX_TEST_EXPECT_TRUE(false);
            co_return TestResult{g_plugin_passed, g_plugin_failed};
        }
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->hasCapability("interpreter.js") == 1);

        // ---- 13. JS 插件 (plugin.yaml 目录分派) ----
        // 需宿主 example_echo 供 js_call_host 互调
        auto hostInst = co_await ctx->pluginManager->loadPluginAsync(path);
        XX_TEST_EXPECT_TRUE(hostInst != nullptr);
        auto jsInst = co_await ctx->pluginManager->loadPluginAsync(jsDir);
        XX_TEST_EXPECT_TRUE(jsInst != nullptr);
        if (!jsInst) {
            XX_TEST_EXPECT_TRUE(false);
            co_return TestResult{g_plugin_passed, g_plugin_failed};
        }
        XX_TEST_EXPECT_EQ(jsInst->name, "example_js");
        // 统一模型: 所有插件都是 C++ 插件, 无 type 概念; 脚本能力由壳经
        // 能力调用委派给引擎 (宿主不参与)。内置合并编译模式下插件编译进
        // libagentxx, 无动态库句柄 (dlHandle 为空)
#ifdef AGENTXX_ENABLE_PLUGIN_BUILTIN
        XX_TEST_EXPECT_TRUE(jsInst->dlHandle == nullptr);
#else
        XX_TEST_EXPECT_TRUE(jsInst->dlHandle != nullptr);
#endif
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("example_js") == jsInst);
        // 依赖声明 (manifest depends: [agentxx_javascript_engine])
        XX_TEST_EXPECT_EQ(jsInst->depends.size(), size_t{1});
        XX_TEST_EXPECT_EQ(jsInst->depends[0], "agentxx_javascript_engine");
        // 脚本内注册的工具挂到本插件实例 (引擎经壳 host 注册)
        XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("js_hello"));
        XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("js_async_wait"));
        XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("js_call_js"));
        XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("js_call_host"));

        // ---- 14. JS 工具执行 (同步 + async Promise) ----
        {
            auto tool = ctx->toolRegistry->find("js_hello");
            XX_TEST_EXPECT_TRUE(tool != nullptr);
            if (tool) {
                auto out = co_await tool->execute_async(neograph::json{
                    {"thread_id", "t1"     },
                    {"name",      "agentxx"},
                });
                auto j   = neograph::json::parse(out);
                XX_TEST_EXPECT_EQ(j["greeting"].get<std::string>(), "Hello, agentxx!");
                XX_TEST_EXPECT_EQ(j["from"].get<std::string>(), "js plugin");
            }
            auto asyncTool = ctx->toolRegistry->find("js_async_wait");
            XX_TEST_EXPECT_TRUE(asyncTool != nullptr);
            if (asyncTool) {
                auto out = co_await asyncTool->execute_async(neograph::json{
                    {"thread_id", "t2"}
                });
                auto j   = neograph::json::parse(out);
                XX_TEST_EXPECT_EQ(j["waited"].get<bool>(), true);
                XX_TEST_EXPECT_EQ(j["thread"].get<std::string>(), "t2");
            }
        }

        // ---- 15. JS 内互调 (本引擎工具内联执行) ----
        {
            auto tool = ctx->toolRegistry->find("js_call_js");
            XX_TEST_EXPECT_TRUE(tool != nullptr);
            if (tool) {
                auto out = co_await tool->execute_async(neograph::json{
                    {"name", "inner-x"}
                });
                auto j   = neograph::json::parse(out);
                XX_TEST_EXPECT_EQ(j["inner"]["greeting"].get<std::string>(), "Hello, inner-x!");
            }
        }

        // ---- 16. JS → 宿主插件互调 (C 桥) ----
        {
            auto tool = ctx->toolRegistry->find("js_call_host");
            XX_TEST_EXPECT_TRUE(tool != nullptr);
            if (tool) {
                auto out = co_await tool->execute_async(neograph::json{
                    {"hello", "host"}
                });
                auto j   = neograph::json::parse(out);
                XX_TEST_EXPECT_EQ(j["host"]["echo"]["hello"].get<std::string>(), "host");
            }
        }

        // ---- 17. 卸载 JS 插件: 工具摘除 + 引擎侧清理 ----
        {
            auto ok = co_await ctx->pluginManager->unloadAsync("example_js");
            XX_TEST_EXPECT_TRUE(ok);
            XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("js_hello"));
            XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("example_js") == nullptr);
            // 宿主 example_echo 不受影响
            XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("example_echo"));
        }

        // ---- 18. 卸载宿主示例插件 ----
        {
            co_await ctx->pluginManager->unloadAsync("example_plugin");
            XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("example_echo"));
        }

        // ---- 19. 卸载 JS 引擎插件: 能力消失 (脚本插件已在 17 卸载) ----
        {
            auto ok = co_await ctx->pluginManager->unloadAsync("agentxx_javascript_engine");
            XX_TEST_EXPECT_TRUE(ok);
            XX_TEST_EXPECT_FALSE(ctx->pluginManager->capabilities()->has("interpreter.js"));
        }

        // ---- 20. 依赖图级联卸载: 引擎卸载连带 depends 它的脚本插件 ----
        {
            // 重新加载引擎 + 脚本插件
            auto engine2 = co_await ctx->pluginManager->loadPluginAsync(jsLib);
            XX_TEST_EXPECT_TRUE(engine2 != nullptr);
            auto js2 = co_await ctx->pluginManager->loadPluginAsync(jsDir);
            XX_TEST_EXPECT_TRUE(js2 != nullptr);
            if (!js2) {
                XX_TEST_EXPECT_TRUE(false);
                co_return TestResult{g_plugin_passed, g_plugin_failed};
            }
            XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("js_hello"));
            // 卸载引擎 → 级联卸载 example_js (依赖图: example_js depends 引擎)
            auto ok = co_await ctx->pluginManager->unloadAsync("agentxx_javascript_engine");
            XX_TEST_EXPECT_TRUE(ok);
            XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("example_js") == nullptr);
            XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("js_hello"));
        }

        // ---- 21. 依赖检查: 必选依赖缺失 → 加载失败 ----
        {
            // 引擎未加载, example_js depends agentxx_javascript_engine → 加载失败
            auto js3 = co_await ctx->pluginManager->loadPluginAsync(jsDir);
            XX_TEST_EXPECT_TRUE(js3 == nullptr);
            XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("example_js") == nullptr);
        }

        // ---- 22. 插件互查 API (vtable list_plugins/get_plugin) ----
        {
            // 重新加载引擎 (供脚本插件验证互查)
            auto engine3 = co_await ctx->pluginManager->loadPluginAsync(jsLib);
            XX_TEST_EXPECT_TRUE(engine3 != nullptr);
            auto js4 = co_await ctx->pluginManager->loadPluginAsync(jsDir);
            XX_TEST_EXPECT_TRUE(js4 != nullptr);
            if (js4) {
                // 脚本内互查已由 plugin.js 顶层执行 (日志); 此处验证宿主侧 JSON
                auto engineJson = ctx->pluginManager->getPluginJson("agentxx_javascript_engine");
                XX_TEST_EXPECT_FALSE(engineJson.empty());
                if (!engineJson.empty()) {
                    auto j = neograph::json::parse(engineJson);
                    XX_TEST_EXPECT_EQ(j["name"].get<std::string>(), "agentxx_javascript_engine");
                    bool hasInterp = false;
                    for (const auto& c : j["capabilities"]) {
                        if (c.get<std::string>() == "interpreter.js") {
                            hasInterp = true;
                        }
                    }
                    XX_TEST_EXPECT_TRUE(hasInterp);
                }
                auto jsJson = ctx->pluginManager->getPluginJson("example_js");
                XX_TEST_EXPECT_FALSE(jsJson.empty());
                if (!jsJson.empty()) {
                    auto j = neograph::json::parse(jsJson);
                    XX_TEST_EXPECT_EQ(
                        j["depends"][0].get<std::string>(),
                        "agentxx_javascript_engine"
                    );
                    bool hasHello = false;
                    for (const auto& t : j["tools"]) {
                        if (t.get<std::string>() == "js_hello") {
                            hasHello = true;
                        }
                    }
                    XX_TEST_EXPECT_TRUE(hasHello);
                }
                auto allJson = ctx->pluginManager->listPluginsJson();
                XX_TEST_EXPECT_FALSE(allJson.empty());
                if (!allJson.empty()) {
                    auto arr     = neograph::json::parse(allJson);
                    bool foundJs = false;
                    for (const auto& item : arr) {
                        if (item["name"].get<std::string>() == "example_js"
                            && item["depends"][0].get<std::string>()
                                   == "agentxx_javascript_engine") {
                            foundJs = true;
                        }
                    }
                    XX_TEST_EXPECT_TRUE(foundJs);
                }
                // 未安装插件 → 空
                XX_TEST_EXPECT_TRUE(ctx->pluginManager->getPluginJson("not_installed").empty());
            }
            co_await ctx->pluginManager->unloadAsync("example_js");
            co_await ctx->pluginManager->unloadAsync("agentxx_javascript_engine");
        }
    }

    // ---- 23. H1 回归: 工具超时后立即卸载不得执行已卸载代码段 ----
    // - 超时 (asyncWithTimeout) 取消等待协程, 但线程池中的 C 回调仍会跑完
    // - 修复: inflight 计数在线程池入口递增 + shared_ptr 保活 → unloadAsync
    //   必须等回调真正返回后才 dlclose (卸载耗时 ≈ 回调剩余时间)
    {
        auto inst23 = co_await ctx->pluginManager->loadPluginAsync(path);
        XX_TEST_EXPECT_TRUE(inst23 != nullptr);
        if (inst23) {
            // 注册带超时的慢工具: 超时 100ms, C 回调 sleep 600ms
            static AgentxxToolSpec slowSpec;
            slowSpec.name            = AGENTXX_SV("slow_timeout_tool");
            slowSpec.description     = AGENTXX_SV("slow tool for unload race test");
            slowSpec.parameters_json = AGENTXX_SV("{}");
            slowSpec.execute         = +[](void*,
                                   AgentxxPluginStringView,
                                   AgentxxPluginStringView,
                                   AgentxxPluginStringView,
                                   char**) -> char* {
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
                char* p = static_cast<char*>(::malloc(3));
                p[0]    = '{';
                p[1]    = '}';
                p[2]    = '\0';
                return p;
            };
            slowSpec.default_timeout_ms = 100;
            XX_TEST_EXPECT_EQ(ctx->pluginManager->registerTool(inst23.get(), &slowSpec), 0);

            auto tool = ctx->toolRegistry->find("slow_timeout_tool");
            XX_TEST_EXPECT_TRUE(tool != nullptr);
            if (tool) {
                std::atomic<bool> execDone{false};
                auto              ex = co_await asio::this_coro::executor;
                asio::co_spawn(
                    ex,
                    [&execDone, tool]() -> asio::awaitable<void> {
                        auto out = co_await tool->execute_async(neograph::json{});
                        (void)out; // 超时返回 "[Plugin tool timeout]"
                        execDone.store(true);
                        co_return;
                    },
                    asio::detached
                );
                // 等待回调已开始 (inflight>0 说明线程池已进入 C 回调):
                // - Windows 线程池调度延迟不定, 直接 sleep 后卸载可能遇到
                //   inflight==0 (回调未开始), 卸载立即完成, 时序断言失真
                // - 注意不能以 execDone 作为轮询出口: 它由超时返回 (100ms)
                //   置位, 早于回调真正开始; 仅 inflight>0 能确认回调在跑
                for (int i = 0; i < 200 && inst23->inflight.load(std::memory_order_acquire) == 0;
                     ++i) {
                    co_await sleepMs(10);
                }
                // 等待超时返回 (100ms) + 回调仍在线程池执行 (600ms 未完成)
                co_await sleepMs(250);
                // 立即卸载: 必须等 inflight 归零 (回调完成) 才 dlclose
                auto t0        = std::chrono::steady_clock::now();
                auto ok        = co_await ctx->pluginManager->unloadAsync("example_plugin");
                auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t0
                )
                                     .count();
                XX_TEST_EXPECT_TRUE(ok);
                // 卸载应等待回调完成 (600-250 ≈ 350ms; 放宽到 >= 250ms),
                // 且回调必须完整执行完 (execDone)
                // - execDone 由 detached 协程在 execute_async 返回后置位, 其
                //   恢复依赖 io 线程调度 (Windows 上可能与卸载完成时刻竞态),
                //   断言前限时等待其恢复, 消除调度时序抖动
                for (int i = 0; i < 100 && !execDone.load(); ++i) {
                    co_await sleepMs(10);
                }
                XX_TEST_EXPECT_TRUE(elapsedMs >= 250);
                XX_TEST_EXPECT_TRUE(execDone.load());
            }
        }
    }

    // ---- 24. H4 回归: disable 跨轮次 (中间件已物理摘除) 后 enable 钩子恢复 ----
    {
        auto inst24 = co_await ctx->pluginManager->loadPluginAsync(path);
        XX_TEST_EXPECT_TRUE(inst24 != nullptr);
        if (inst24) {
            XX_TEST_EXPECT_TRUE(inst24->middleware != nullptr);
            ctx->pluginManager->onTurnBegin(); // 轮次执行中
            ctx->pluginManager->disable("example_plugin");
            ctx->pluginManager->flushPendingCleanup(); // 轮末摘除中间件
            ctx->pluginManager->onTurnEnd();
            XX_TEST_EXPECT_TRUE(inst24->middleware == nullptr); // 已物理摘除
            XX_TEST_EXPECT_TRUE(inst24->hookRegistrations.size() == size_t{1});
            // 启用: 钩子按注册记录重建中间件
            ctx->pluginManager->enable("example_plugin");
            XX_TEST_EXPECT_TRUE(inst24->middleware != nullptr);
            XX_TEST_EXPECT_FALSE(inst24->middleware->disabled);
            const auto& handles = ctx->middlewareHandleContext->handles;
            bool        found   = std::any_of(handles.begin(), handles.end(), [](const auto& h) {
                return h->name == "example_plugin_middleware" && !h->disabled;
            });
            XX_TEST_EXPECT_TRUE(found);
            co_await ctx->pluginManager->unloadAsync("example_plugin");
        }
    }

    // ---- 25. M8 回归: 用户手动禁用的插件不被 enable 级联恢复 ----
    {
        auto engine25 = co_await ctx->pluginManager->loadPluginAsync(jsLib);
        XX_TEST_EXPECT_TRUE(engine25 != nullptr);
        auto js25 = co_await ctx->pluginManager->loadPluginAsync(jsDir);
        XX_TEST_EXPECT_TRUE(js25 != nullptr);
        if (engine25 && js25) {
            ctx->pluginManager->disable("example_js"); // 用户手动禁用
            XX_TEST_EXPECT_FALSE(ctx->pluginManager->find("example_js")->enabled);
            // 启用引擎 → 不应级联恢复被用户手动禁用的脚本插件
            ctx->pluginManager->enable("agentxx_javascript_engine");
            XX_TEST_EXPECT_FALSE(ctx->pluginManager->find("example_js")->enabled);
            // 用户显式启用 → 恢复
            ctx->pluginManager->enable("example_js");
            XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("example_js")->enabled);
            co_await ctx->pluginManager->unloadAsync("example_js");
            co_await ctx->pluginManager->unloadAsync("agentxx_javascript_engine");
        }
    }

    // ---- 26. H2 回归: shutdownAll (含 JS 引擎) 先子后父, unload 回调后 dlclose ----
    // - 引擎插件 unload 回调 join JS 线程; 脚本插件先卸载 (通知引擎释放
    //   JSContext) → 引擎 join 时 JS 线程空闲 → 无挂死/无执行已卸载代码段
    {
        auto engine26 = co_await ctx->pluginManager->loadPluginAsync(jsLib);
        XX_TEST_EXPECT_TRUE(engine26 != nullptr);
        auto js26 = co_await ctx->pluginManager->loadPluginAsync(jsDir);
        XX_TEST_EXPECT_TRUE(js26 != nullptr);
        ctx->pluginManager->shutdownAll();
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("example_js") == nullptr);
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("agentxx_javascript_engine") == nullptr);
    }

    // ---- 27. loadConfiguredPlugins 拓扑排序: 配置顺序无关 (脚本插件在前也能加载) ----
    // - 库路径项按文件名推导插件名 (libagentxx_javascript_engine.so → agentxx_javascript_engine),
    //   参与依赖排序: example_js (depends agentxx_javascript_engine) 排在引擎之后
    {
        std::vector<agentxx::agent::PluginConfig> cfgs;
        // 故意把脚本插件目录写在引擎库之前 (配置顺序与依赖顺序相反)
        agentxx::agent::PluginConfig jsCfg;
        jsCfg.path    = jsDir;
        jsCfg.enabled = true;
        cfgs.push_back(jsCfg);
        agentxx::agent::PluginConfig engCfg;
        engCfg.path    = jsLib;
        engCfg.enabled = true;
        cfgs.push_back(engCfg);
        co_await ctx->pluginManager->loadConfiguredPlugins(cfgs);
        // 两者都应加载成功 (拓扑排序保证引擎先加载)
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("agentxx_javascript_engine") != nullptr);
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("example_js") != nullptr);
        XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("js_hello"));
        // 清理
        co_await ctx->pluginManager->unloadAsync("agentxx_javascript_engine");
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("example_js") == nullptr); // 级联卸载
        // 禁用项不加载
        agentxx::agent::PluginConfig disCfg;
        disCfg.path    = jsDir;
        disCfg.enabled = false;
        co_await ctx->pluginManager->loadConfiguredPlugins({disCfg});
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("example_js") == nullptr);
    }

    // ---- 28. 提示词读写 (get_prompt/set_prompt): 插件写入默认条目 + 卸载回滚 ----
    // - 插件加载时经 set_prompt 写入宿主 toolPrompt (仅当无该条目);
    //   卸载时自动回滚 (恢复加载前状态, 条目删除)
    // - 用户 yaml 覆盖 (加载前已存在条目) 优先, 插件不覆盖
    {
        auto& prompt = ctx->agentConfig->prompt;
        // 初始: 无 example_echo 条目
        XX_TEST_EXPECT_FALSE(prompt.toolPrompt.contains("example_echo"));

        // 加载插件 → 写入默认条目
        auto inst28 = co_await ctx->pluginManager->loadPluginAsync(path);
        XX_TEST_EXPECT_TRUE(inst28 != nullptr);
        if (inst28) {
            XX_TEST_EXPECT_TRUE(prompt.toolPrompt.contains("example_echo"));
            if (prompt.toolPrompt.contains("example_echo")) {
                XX_TEST_EXPECT_EQ(
                    prompt.toolPrompt.at("example_echo").depict,
                    "Echo the input arguments back as JSON (example plugin tool)."
                );
            }
            // get_tool_prompt 可读取该条目
            auto tpJson = ctx->pluginManager->getToolPromptJson("example_echo");
            XX_TEST_EXPECT_FALSE(tpJson.empty());
            if (!tpJson.empty()) {
                auto j = neograph::json::parse(tpJson);
                XX_TEST_EXPECT_TRUE(j["depict"].is_string());
            }
            // get_prompt 完整提示词含该条目
            auto fullJson = ctx->pluginManager->getPromptJson();
            XX_TEST_EXPECT_FALSE(fullJson.empty());
            if (!fullJson.empty()) {
                auto j = neograph::json::parse(fullJson);
                XX_TEST_EXPECT_TRUE(
                    j.contains("toolPrompt") && j["toolPrompt"].is_object()
                    && j["toolPrompt"].contains("example_echo")
                );
            }

            // system 提示词写入 + 回滚 (set_prompt 直接调用路径)
            auto originalSystem = prompt.systemPrompt;
            XX_TEST_EXPECT_EQ(
                ctx->pluginManager
                    ->setPromptJson(inst28.get(), R"({"systemPrompt":"modified by plugin"})"),
                0
            );
            XX_TEST_EXPECT_EQ(prompt.systemPrompt, "modified by plugin");

            // 卸载 → toolPrompt 条目删除 + system 提示词恢复
            auto ok = co_await ctx->pluginManager->unloadAsync("example_plugin");
            XX_TEST_EXPECT_TRUE(ok);
            XX_TEST_EXPECT_FALSE(prompt.toolPrompt.contains("example_echo"));
            XX_TEST_EXPECT_EQ(prompt.systemPrompt, originalSystem);
        }

        // 用户覆盖优先: 加载前已存在条目 (模拟 yaml 覆盖), 插件不覆盖
        prompt.toolPrompt["example_echo"].depict = "user override";
        auto inst28b = co_await ctx->pluginManager->loadPluginAsync(path);
        XX_TEST_EXPECT_TRUE(inst28b != nullptr);
        if (inst28b) {
            XX_TEST_EXPECT_TRUE(prompt.toolPrompt.contains("example_echo"));
            XX_TEST_EXPECT_EQ(prompt.toolPrompt.at("example_echo").depict, "user override");
            co_await ctx->pluginManager->unloadAsync("example_plugin");
            // 回滚后保留用户覆盖值 (插件未写入, 无回滚动作)
            XX_TEST_EXPECT_TRUE(prompt.toolPrompt.contains("example_echo"));
            XX_TEST_EXPECT_EQ(prompt.toolPrompt.at("example_echo").depict, "user override");
            prompt.toolPrompt.erase("example_echo"); // 清理
        }
    }

    // ---- 29. A7 + C2: sides 过滤 + args 随配置直接传递 ----
    // - sides=client 的配置项不在 agent 侧加载 (原实现漏过滤, agent 侧会
    //   误加载纯 client 插件)
    // - args 经 loadConfiguredPlugins 直接传入实例 (原实现事后按路径推导名
    //   回查, manifest name 与目录名不一致时静默丢失)
    {
        std::vector<agentxx::agent::PluginConfig> cfgs;
        agentxx::agent::PluginConfig              pc;
        pc.path    = path;
        pc.enabled = true;
        pc.args    = neograph::json{
               {"custom_key", "custom_value"}
        };

        // sides=client: agent 侧跳过
        pc.sides = agentxx::agent::PluginSide::Client;
        cfgs.push_back(pc);
        co_await ctx->pluginManager->loadConfiguredPlugins(cfgs);
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("example_plugin") == nullptr);

        // sides=auto: 正常加载, args 随配置传入实例
        pc.sides = agentxx::agent::PluginSide::Auto;
        cfgs.clear();
        cfgs.push_back(pc);
        co_await ctx->pluginManager->loadConfiguredPlugins(cfgs);
        auto inst29 = ctx->pluginManager->find("example_plugin");
        XX_TEST_EXPECT_TRUE(inst29 != nullptr);
        if (inst29) {
            XX_TEST_EXPECT_EQ(inst29->args.value("custom_key", std::string{}), "custom_value");
            auto json = ctx->pluginManager->getPluginArgsJson(inst29.get());
            XX_TEST_EXPECT_FALSE(json.empty());
            if (!json.empty()) {
                auto j = neograph::json::parse(json);
                XX_TEST_EXPECT_EQ(j["custom_key"].get<std::string>(), "custom_value");
            }
            co_await ctx->pluginManager->unloadAsync("example_plugin");
        }
    }

    // ---- 30. B5 回归: 禁用插件 vtable publish 被拒绝 ----
    {
        auto inst30 = co_await ctx->pluginManager->loadPluginAsync(path);
        XX_TEST_EXPECT_TRUE(inst30 != nullptr);
        if (inst30) {
            // 启用状态: publish 正常
            XX_TEST_EXPECT_EQ(
                inst30->host.vtable
                    ->publish(&inst30->host, AGENTXX_SV("demo.topic"), AGENTXX_SV(R"({"k":"v"})")),
                0
            );
            ctx->pluginManager->disable("example_plugin");
            // 禁用状态: vtable publish 拒绝 (返回非 0)
            int rc = inst30->host.vtable->publish(
                &inst30->host,
                AGENTXX_SV("demo.topic"),
                AGENTXX_SV(R"({"k":"v"})")
            );
            XX_TEST_EXPECT_TRUE(rc != 0);
            co_await ctx->pluginManager->unloadAsync("example_plugin");
        }
    }

    co_return TestResult{g_plugin_passed, g_plugin_failed};
}

} // namespace test
} // namespace agentxx
