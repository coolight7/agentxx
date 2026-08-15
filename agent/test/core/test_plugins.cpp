#include "test_plugins.h"

#include "agentxx/agent/context.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/util/async_offload.h"
#include "agentxx/util/log.h"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

namespace agentxx {
namespace test {

int g_plugin_passed = 0;
int g_plugin_failed = 0;

/// 定位示例插件库 (与测试可执行同目录, 见 test/CMakeLists.txt)
static std::string findExamplePluginPath() {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    if (!ec) {
        for (const auto& cand : {
                 cwd / "libagentxx_plugin_example.so",
                 cwd / "libagentxx_plugin_example.dylib",
                 cwd / "libagentxx_plugin_example.dll",
             }) {
            if (fs::exists(cand, ec)) {
                return cand.string();
            }
        }
    }
    return "libagentxx_plugin_example.so"; // 让加载失败暴露日志
}

static asio::awaitable<void> sleepMs(int ms) {
    auto timer = asio::steady_timer(co_await asio::this_coro::executor, std::chrono::milliseconds{ms});
    co_await timer.async_wait(asio::use_awaitable);
}

asio::awaitable<TestResult> run_plugin_tests() {
    g_plugin_passed = 0;
    g_plugin_failed = 0;

    // ---- 1. 构造最小 AgentContext (io 线程环境, 与库内无锁模型一致) ----
    auto ctx                     = std::make_shared<agentxx::agent::AgentContext>();
    ctx->agentConfig             = std::make_shared<agentxx::agent::AgentConfig>();
    ctx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
    ctx->bus                     = std::make_shared<agentxx::middleware::EventBus>(co_await asio::this_coro::executor);
    ctx->toolRegistry            = std::make_shared<agentxx::plugin::ToolRegistry>();
    ctx->pluginManager           = std::make_shared<agentxx::plugin::PluginManager>(ctx);

    // ---- 2. 加载示例插件 ----
    auto path = findExamplePluginPath();
    XX_TEST_EXPECT_TRUE(path.find("libagentxx_plugin_example") != std::string::npos);
    auto inst = co_await ctx->pluginManager->loadNativeAsync(path);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        XX_TEST_EXPECT_TRUE(false);
        co_return TestResult{g_plugin_passed, g_plugin_failed};
    }
    XX_TEST_EXPECT_EQ(inst->name, "agentxx_plugin_example");
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
                {"thread_id", "t1"},
                {"hello", "world"},
            });
            auto j = neograph::json::parse(out);
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
                {"x", 42},
            });
            auto j = neograph::json::parse(out);
            XX_TEST_EXPECT_EQ(j["via_call_tool"]["echo"]["x"].get<int>(), 42);
        }
    }

    // ---- 5. 钩子注册: 中间件句柄入栈 + 钩子点记录 ----
    {
        const auto& handles = ctx->middlewareHandleContext->handles;
        bool found = std::any_of(handles.begin(), handles.end(), [](const auto& h) {
            return h->name == "agentxx_plugin_example_middleware";
        });
        XX_TEST_EXPECT_TRUE(found);
        XX_TEST_EXPECT_EQ(inst->hookPoints.size(), size_t{1});
        XX_TEST_EXPECT_TRUE(inst->middleware != nullptr);
    }

    // ---- 6. 事件订阅/发布回环 (plugin.demo.topic) ----
    {
        // 测试侧订阅同一 topic (JSON 字符串载荷)
        bool received = false;
        auto subId    = ctx->bus->get<std::string>("plugin.demo.topic").subscribe(
            [&](const std::string& data) -> asio::awaitable<void> {
                if (data.find("\"k\"") != std::string::npos) {
                    received = true;
                }
                co_return;
            }
        );
        int rc = ctx->pluginManager->publish("demo.topic", R"({"k":"v"})");
        XX_TEST_EXPECT_EQ(rc, 0);
        co_await sleepMs(150);
        XX_TEST_EXPECT_TRUE(received);
        ctx->bus->get<std::string>("plugin.demo.topic").unsubscribe(subId);
    }

    // ---- 7. 禁用 → 工具摘除/钩子停用; 启用 → 恢复 ----
    {
        ctx->pluginManager->disable("agentxx_plugin_example");
        XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("example_echo"));
        XX_TEST_EXPECT_FALSE(ctx->pluginManager->registry()->contains("example_caller"));
        XX_TEST_EXPECT_TRUE(inst->middleware->disabled);
        XX_TEST_EXPECT_FALSE(inst->enabled);

        ctx->pluginManager->enable("agentxx_plugin_example");
        XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("example_echo"));
        XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("example_caller"));
        XX_TEST_EXPECT_FALSE(inst->middleware->disabled);
        XX_TEST_EXPECT_TRUE(inst->enabled);
    }

    // ---- 8. 卸载: 注册残留全部清理 + 插件移除 + 能力移除 ----
    {
        auto ok = co_await ctx->pluginManager->unloadAsync("agentxx_plugin_example");
        XX_TEST_EXPECT_TRUE(ok);
        XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("example_echo"));
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("agentxx_plugin_example") == nullptr);
        XX_TEST_EXPECT_FALSE(ctx->pluginManager->capabilities()->has("example.demo"));
        // 中间件应从 handles 摘除 (pendingCleanup 于下轮 flush; 无轮次时直接摘除)
        const auto& handles = ctx->middlewareHandleContext->handles;
        bool found = std::any_of(handles.begin(), handles.end(), [](const auto& h) {
            return h->name == "agentxx_plugin_example_middleware";
        });
        XX_TEST_EXPECT_FALSE(found);
        // 工具对象应已释放 (inflight 归零后 dlclose)
        XX_TEST_EXPECT_EQ(inst->tools.size(), size_t{0});
    }

    // ---- 9. 重复卸载拒绝 ----
    {
        auto ok = co_await ctx->pluginManager->unloadAsync("agentxx_plugin_example");
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
        // 插件工具注册冲突在 loadNativeAsync 路径覆盖, 此处验证注册表拒绝同名静态工具
        auto dummy = std::make_shared<DummyTool>("builtin_tool_a", ctx);
        XX_TEST_EXPECT_FALSE(ctx->toolRegistry->registerTool("builtin_tool_a", dummy));
        XX_TEST_EXPECT_TRUE(ctx->toolRegistry->registerTool("plugin_ok_tool", dummy));
        ctx->toolRegistry->unregisterTool("plugin_ok_tool");
    }

    // ---- 11. 插件列表 ----
    {
        // 重新加载用于 list 验证
        auto inst2 = co_await ctx->pluginManager->loadNativeAsync(path);
        XX_TEST_EXPECT_TRUE(inst2 != nullptr);
        if (inst2) {
            auto list = ctx->pluginManager->list();
            bool found = std::any_of(list.begin(), list.end(), [](const auto& item) {
                return item.name == "agentxx_plugin_example" && item.enabled;
            });
            XX_TEST_EXPECT_TRUE(found);
            co_await ctx->pluginManager->unloadAsync("agentxx_plugin_example");
        }
    }

    // ---- 12. JS 引擎插件加载 (二期) ----
    {
        auto jsPath = findExamplePluginPath();
        // 引擎插件库与示例插件同目录: exec/libagentxx_plugin_js.so
        namespace fs = std::filesystem;
        std::string jsLib = (fs::path(jsPath).parent_path() / "libagentxx_plugin_js.so").string();
        auto engineInst = co_await ctx->pluginManager->loadNativeAsync(jsLib);
        XX_TEST_EXPECT_TRUE(engineInst != nullptr);
        if (!engineInst) {
            XX_TEST_EXPECT_TRUE(false);
            co_return TestResult{g_plugin_passed, g_plugin_failed};
        }
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->hasCapability("interpreter.js") == 1);

        // ---- 13. JS 插件 (plugin.yaml 目录分派) ----
        // 需宿主 example_echo 供 js_call_host 互调
        auto hostInst = co_await ctx->pluginManager->loadNativeAsync(path);
        XX_TEST_EXPECT_TRUE(hostInst != nullptr);
        auto jsDir = (fs::path(jsPath).parent_path() / "plugins" / "js_example").string();
        auto jsInst = co_await ctx->pluginManager->loadPluginAsync(jsDir);
        XX_TEST_EXPECT_TRUE(jsInst != nullptr);
        if (!jsInst) {
            XX_TEST_EXPECT_TRUE(false);
            co_return TestResult{g_plugin_passed, g_plugin_failed};
        }
        XX_TEST_EXPECT_EQ(jsInst->name, "js_example");
        // 统一模型: 所有插件都是 C++ 插件 (有 dlHandle), 无 type 概念;
        // 脚本能力由壳经能力调用委派给引擎 (宿主不参与)
        XX_TEST_EXPECT_TRUE(jsInst->dlHandle != nullptr);
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("js_example") == jsInst);
        // 依赖声明 (manifest depends: [agentxx_plugin_js])
        XX_TEST_EXPECT_EQ(jsInst->depends.size(), size_t{1});
        XX_TEST_EXPECT_EQ(jsInst->depends[0], "agentxx_plugin_js");
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
                    {"thread_id", "t1"},
                    {"name", "agentxx"},
                });
                auto j = neograph::json::parse(out);
                XX_TEST_EXPECT_EQ(j["greeting"].get<std::string>(), "Hello, agentxx!");
                XX_TEST_EXPECT_EQ(j["from"].get<std::string>(), "js plugin");
            }
            auto asyncTool = ctx->toolRegistry->find("js_async_wait");
            XX_TEST_EXPECT_TRUE(asyncTool != nullptr);
            if (asyncTool) {
                auto out = co_await asyncTool->execute_async(neograph::json{{"thread_id", "t2"}});
                auto j = neograph::json::parse(out);
                XX_TEST_EXPECT_EQ(j["waited"].get<bool>(), true);
                XX_TEST_EXPECT_EQ(j["thread"].get<std::string>(), "t2");
            }
        }

        // ---- 15. JS 内互调 (本引擎工具内联执行) ----
        {
            auto tool = ctx->toolRegistry->find("js_call_js");
            XX_TEST_EXPECT_TRUE(tool != nullptr);
            if (tool) {
                auto out = co_await tool->execute_async(neograph::json{{"name", "inner-x"}});
                auto j = neograph::json::parse(out);
                XX_TEST_EXPECT_EQ(j["inner"]["greeting"].get<std::string>(), "Hello, inner-x!");
            }
        }

        // ---- 16. JS → 宿主插件互调 (C 桥) ----
        {
            auto tool = ctx->toolRegistry->find("js_call_host");
            XX_TEST_EXPECT_TRUE(tool != nullptr);
            if (tool) {
                auto out = co_await tool->execute_async(neograph::json{{"hello", "host"}});
                auto j = neograph::json::parse(out);
                XX_TEST_EXPECT_EQ(j["host"]["echo"]["hello"].get<std::string>(), "host");
            }
        }

        // ---- 17. 卸载 JS 插件: 工具摘除 + 引擎侧清理 ----
        {
            auto ok = co_await ctx->pluginManager->unloadAsync("js_example");
            XX_TEST_EXPECT_TRUE(ok);
            XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("js_hello"));
            XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("js_example") == nullptr);
            // 宿主 example_echo 不受影响
            XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("example_echo"));
        }

        // ---- 18. 卸载宿主示例插件 ----
        {
            co_await ctx->pluginManager->unloadAsync("agentxx_plugin_example");
            XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("example_echo"));
        }

        // ---- 19. 卸载 JS 引擎插件: 能力消失 (脚本插件已在 17 卸载) ----
        {
            auto ok = co_await ctx->pluginManager->unloadAsync("agentxx_plugin_js");
            XX_TEST_EXPECT_TRUE(ok);
            XX_TEST_EXPECT_FALSE(ctx->pluginManager->capabilities()->has("interpreter.js"));
        }

        // ---- 20. 依赖图级联卸载: 引擎卸载连带 depends 它的脚本插件 ----
        {
            // 重新加载引擎 + 脚本插件
            auto engine2 = co_await ctx->pluginManager->loadNativeAsync(jsLib);
            XX_TEST_EXPECT_TRUE(engine2 != nullptr);
            auto js2 = co_await ctx->pluginManager->loadPluginAsync(jsDir);
            XX_TEST_EXPECT_TRUE(js2 != nullptr);
            if (!js2) {
                XX_TEST_EXPECT_TRUE(false);
                co_return TestResult{g_plugin_passed, g_plugin_failed};
            }
            XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("js_hello"));
            // 卸载引擎 → 级联卸载 js_example (依赖图: js_example depends 引擎)
            auto ok = co_await ctx->pluginManager->unloadAsync("agentxx_plugin_js");
            XX_TEST_EXPECT_TRUE(ok);
            XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("js_example") == nullptr);
            XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("js_hello"));
        }

        // ---- 21. 依赖检查: 必选依赖缺失 → 加载失败 ----
        {
            // 引擎未加载, js_example depends agentxx_plugin_js → 加载失败
            auto js3 = co_await ctx->pluginManager->loadPluginAsync(jsDir);
            XX_TEST_EXPECT_TRUE(js3 == nullptr);
            XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("js_example") == nullptr);
        }

        // ---- 22. 插件互查 API (vtable list_plugins/get_plugin) ----
        {
            // 重新加载引擎 (供脚本插件验证互查)
            auto engine3 = co_await ctx->pluginManager->loadNativeAsync(jsLib);
            XX_TEST_EXPECT_TRUE(engine3 != nullptr);
            auto js4 = co_await ctx->pluginManager->loadPluginAsync(jsDir);
            XX_TEST_EXPECT_TRUE(js4 != nullptr);
            if (js4) {
                // 脚本内互查已由 plugin.js 顶层执行 (日志); 此处验证宿主侧 JSON
                auto engineJson = ctx->pluginManager->getPluginJson("agentxx_plugin_js");
                XX_TEST_EXPECT_FALSE(engineJson.empty());
                if (!engineJson.empty()) {
                    auto j = neograph::json::parse(engineJson);
                    XX_TEST_EXPECT_EQ(j["name"].get<std::string>(), "agentxx_plugin_js");
                    bool hasInterp = false;
                    for (const auto& c : j["capabilities"]) {
                        if (c.get<std::string>() == "interpreter.js") {
                            hasInterp = true;
                        }
                    }
                    XX_TEST_EXPECT_TRUE(hasInterp);
                }
                auto jsJson = ctx->pluginManager->getPluginJson("js_example");
                XX_TEST_EXPECT_FALSE(jsJson.empty());
                if (!jsJson.empty()) {
                    auto j = neograph::json::parse(jsJson);
                    XX_TEST_EXPECT_EQ(j["depends"][0].get<std::string>(), "agentxx_plugin_js");
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
                    auto arr = neograph::json::parse(allJson);
                    bool foundJs = false;
                    for (const auto& item : arr) {
                        if (item["name"].get<std::string>() == "js_example"
                            && item["depends"][0].get<std::string>() == "agentxx_plugin_js") {
                            foundJs = true;
                        }
                    }
                    XX_TEST_EXPECT_TRUE(foundJs);
                }
                // 未安装插件 → 空
                XX_TEST_EXPECT_TRUE(ctx->pluginManager->getPluginJson("not_installed").empty());
            }
            co_await ctx->pluginManager->unloadAsync("js_example");
            co_await ctx->pluginManager->unloadAsync("agentxx_plugin_js");
        }
    }

    co_return TestResult{g_plugin_passed, g_plugin_failed};
}

} // namespace test
} // namespace agentxx
