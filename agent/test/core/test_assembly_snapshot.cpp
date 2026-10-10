/// 启动装配快照测试 (计划 ARC-6 / TOOL-12 / CFG-3 / PLG-10)
///
/// 覆盖:
/// - 配置侧快照: 模型 (API key 不外泄) / 路径 / 权限 / 特性 / 插件声明 / 提示词段落
/// - 运行侧快照: 模型注册表 / 中间件顺序 / 工具清单 (来源 + 被白名单过滤的原因) /
///   插件条目 / 执行图 / 持久化 / 组件加载信息
/// - 渲染文本: 与启动日志同源, 关键小节都能被人工读到
#include "agentxx-test/core/test_assembly_snapshot.h"

#include "agentxx/agent/assembly_snapshot.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/plugin/plugin_manager.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/use_awaitable.hpp>
#include <fmt/format.h>
#include <string>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见)
int g_as_passed = 0;
int g_as_failed = 0;
} // namespace

#define XX_TEST_PASSED g_as_passed
#define XX_TEST_FAILED g_as_failed

namespace agentxx {
namespace test {

using namespace utilxx_base;

namespace {

/// 文本中是否包含子串
bool has(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

/// 把渲染行拼成一段文本 (便于整体断言)
std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        out += line;
        out += '\n';
    }
    return out;
}

/// 构造一个"配置项尽量齐全"的 AgentConfig (不依赖真实路径存在)
std::shared_ptr<agentxx::agent::AgentConfig> makeRichConfig() {
    auto cfg = std::make_shared<agentxx::agent::AgentConfig>();

    cfg->agentName = "Agentxx";
    cfg->dataDir   = "/tmp/agentxx-data";
    cfg->workDir   = "/tmp/agentxx-work";
    cfg->sessionStoreDirectory = "/tmp/agentxx-data/sqlite/sessions";
    cfg->language  = "zh-cn";
    cfg->languageExplicit = true;

    cfg->model.type       = "openai";
    cfg->model.name       = "primary";
    cfg->model.modelName  = "gpt-test";
    cfg->model.baseUrl    = "https://api.example.com/v1";
    cfg->model.apiKey     = "sk-super-secret-value";
    cfg->model.modelContextMaxToken = 128000;
    cfg->model.extraHeaders["X-Tenant"] = "tenant-secret-value";
    cfg->model.extraConfig["temperature"] = 0.2;

    auto sub = cfg->model;
    sub.name = "secondary";
    sub.modelName = "gpt-test-mini";
    cfg->availableModels["primary"]   = cfg->model;
    cfg->availableModels["secondary"] = sub;
    cfg->currentModelName = "primary";

    cfg->permissionMode = agentxx::agent::PermissionMode::AllAsk;
    cfg->permissionAllowPaths.push_back("/tmp/agentxx-work/allowed");
    cfg->permissionDenyPaths.push_back("/tmp/agentxx-work/secret");

    cfg->enableSessionStore  = true;
    cfg->enableSummarization = true;
    cfg->enableSubagent      = true;
    cfg->enableWorktree      = true;
    cfg->enableToolFiltering = true;
    cfg->toolWhitelist       = {"agentxx_share_store"};
    cfg->toolParallelMaxConcurrency = 6;

    cfg->skillDirPaths.push_back("/tmp/agentxx-skills");
    cfg->memoryFilePaths.push_back("/tmp/agentxx-work/AGENTS.md");
    cfg->ragDocsPaths.push_back("/tmp/agentxx-docs");
    cfg->mcpServerUrls["mymcp"] = agentxx::agent::McpServerConfig{
        .url = "http://127.0.0.1:9000/sse",
    };

    agentxx::agent::PluginConfig pc;
    pc.path    = "builtin://agentxx_filesystem";
    pc.enabled = true;
    pc.sides   = agentxx::agent::PluginSide::Agent;
    pc.args    = utilxx_base::Json::object();
    pc.args["loadPaths"] = "/tmp/agentxx-work";
    cfg->plugins.push_back(std::move(pc));

    return cfg;
}

/// 装配一个最小可用的 CodeAgent 并完成 init (不发起任何网络请求)
asio::awaitable<std::shared_ptr<agentxx::agent::CodeAgent>>
    makeInitializedAgent(std::shared_ptr<agentxx::agent::AgentConfig> cfg) {
    // 无 dataDir / 无持久化: init 不落盘 (内存模式)
    auto agent = std::make_shared<agentxx::agent::CodeAgent>(cfg);
    co_await agent->init();
    co_return agent;
}

} // namespace

TestResult testAssemblySnapshotConfig() {
    const int passedBefore = g_as_passed;
    const int failedBefore = g_as_failed;

    auto cfg = makeRichConfig();

    const auto snapshot = agentxx::agent::buildConfigSnapshot(*cfg);
    const auto text     = snapshot.dump();

    // ---- 模型: 关键字段在, 凭据不在 ----
    XX_TEST_EXPECT_EQ(snapshot["model"]["default"].value("model", std::string{}), std::string{"gpt-test"});
    XX_TEST_EXPECT_EQ(
        snapshot["model"]["default"].value("base_url", std::string{}),
        std::string{"https://api.example.com/v1"}
    );
    XX_TEST_EXPECT_TRUE(snapshot["model"]["default"].value("api_key_set", false));
    XX_TEST_EXPECT_TRUE(!has(text, "sk-super-secret-value"));
    XX_TEST_EXPECT_TRUE(!has(text, "tenant-secret-value"));
    // 头字段只记录名称, extra 配置只记录键
    XX_TEST_EXPECT_EQ(snapshot["model"]["default"]["extra_headers"].size(), size_t{1});
    XX_TEST_EXPECT_EQ(
        snapshot["model"]["default"]["extra_headers"][0].get<std::string>(),
        std::string{"X-Tenant"}
    );
    XX_TEST_EXPECT_EQ(snapshot["model"]["default"]["extra_config_keys"].size(), size_t{1});
    // 可用模型列表逐项输出
    XX_TEST_EXPECT_EQ(snapshot["model"]["available"].size(), size_t{2});
    XX_TEST_EXPECT_TRUE(snapshot["model"]["available"].contains("secondary"));

    // ---- 路径与权限 ----
    XX_TEST_EXPECT_EQ(snapshot["paths"].value("data_dir", std::string{}), std::string{"/tmp/agentxx-data"});
    XX_TEST_EXPECT_EQ(snapshot["paths"].value("work_dir", std::string{}), std::string{"/tmp/agentxx-work"});
    XX_TEST_EXPECT_EQ(
        snapshot["paths"].value("session_root", std::string{}),
        std::string{"/tmp/agentxx-data/sqlite/sessions"}
    );
    XX_TEST_EXPECT_EQ(snapshot["permission"].value("mode", std::string{}), std::string{"all_ask"});
    XX_TEST_EXPECT_EQ(snapshot["permission"]["allow_paths"].size(), size_t{1});
    XX_TEST_EXPECT_EQ(snapshot["permission"]["deny_paths"].size(), size_t{1});

    // ---- 特性与上限 ----
    XX_TEST_EXPECT_TRUE(snapshot["features"].value("session_store", false));
    XX_TEST_EXPECT_TRUE(snapshot["features"].value("tool_filtering", false));
    XX_TEST_EXPECT_EQ(snapshot["features"]["tool_whitelist"].size(), size_t{1});
    XX_TEST_EXPECT_EQ(snapshot["limits"].value("tool_parallel_max", 0), 6);

    // ---- 语言与来源标记 ----
    XX_TEST_EXPECT_EQ(snapshot.value("language", std::string{}), std::string{"zh-cn"});
    XX_TEST_EXPECT_TRUE(snapshot.value("language_explicit", false));

    // ---- 资源与插件声明 ----
    XX_TEST_EXPECT_EQ(snapshot["resources"]["skill_dirs"].size(), size_t{1});
    XX_TEST_EXPECT_EQ(snapshot["resources"]["memory_files"].size(), size_t{1});
    XX_TEST_EXPECT_TRUE(snapshot["resources"]["mcp_servers"].contains("mymcp"));
    XX_TEST_EXPECT_EQ(snapshot["plugin_declarations"].size(), size_t{1});
    XX_TEST_EXPECT_EQ(
        snapshot["plugin_declarations"][0].value("path", std::string{}),
        std::string{"builtin://agentxx_filesystem"}
    );
    XX_TEST_EXPECT_EQ(
        snapshot["plugin_declarations"][0].value("sides", std::string{}),
        std::string{"agent"}
    );
    // 插件参数只输出键名 (参数可能含凭据)
    XX_TEST_EXPECT_EQ(snapshot["plugin_declarations"][0]["args_keys"].size(), size_t{1});

    // ---- 渲染文本 ----
    const auto rendered = joinLines(agentxx::agent::renderAssemblySnapshot(snapshot));
    XX_TEST_EXPECT_TRUE(has(rendered, "model.default: gpt-test"));
    XX_TEST_EXPECT_TRUE(has(rendered, "model.available: 2"));
    XX_TEST_EXPECT_TRUE(has(rendered, "paths.data_dir: /tmp/agentxx-data"));
    XX_TEST_EXPECT_TRUE(has(rendered, "permission.mode: all_ask"));
    XX_TEST_EXPECT_TRUE(has(rendered, "language: zh-cn (from config)"));
    XX_TEST_EXPECT_TRUE(!has(rendered, "sk-super-secret-value"));

    // ---- 合并 (配置 + 运行侧) ----
    auto merged = agentxx::agent::mergeAssemblySnapshot(
        snapshot,
        utilxx_base::Json::object({{"models", utilxx_base::Json::object({{"count", 1}})}})
    );
    XX_TEST_EXPECT_EQ(merged["models"].value("count", 0), 1);
    XX_TEST_EXPECT_TRUE(merged.contains("paths"));

    return TestResult{g_as_passed - passedBefore, g_as_failed - failedBefore};
}

asio::awaitable<TestResult> run_assembly_snapshot_tests() {
    const int passedBefore = g_as_passed;
    const int failedBefore = g_as_failed;

    // ---- 运行侧快照: 未过滤白名单的常规装配 ----
    {
        auto cfg = std::make_shared<agentxx::agent::AgentConfig>();
        cfg->model.type      = "openai";
        cfg->model.name      = "test-model";
        cfg->model.modelName = "test-model";
        cfg->model.baseUrl   = "http://127.0.0.1:1/v1";
        cfg->model.apiKey    = "EMPTY";
        cfg->mcpServerUrls["dummy"] = agentxx::agent::McpServerConfig{
            .url = "http://127.0.0.1:1/sse",
        };

        auto agent = co_await makeInitializedAgent(cfg);
        const auto snapshot = agentxx::agent::buildRuntimeSnapshot(*agent->agentContext);

        // 模型注册表
        XX_TEST_EXPECT_EQ(snapshot["models"].value("count", 0), 1);
        XX_TEST_EXPECT_EQ(
            snapshot["models"].value("default", std::string{}),
            std::string{"test-model"}
        );

        // 中间件顺序: 非空, 且日志中间件固定在末尾 (其注释要求作为最后一层)
        XX_TEST_EXPECT_GE(snapshot["middlewares"].size(), size_t{2});
        XX_TEST_EXPECT_EQ(
            snapshot["middlewares"][snapshot["middlewares"].size() - 1].value("name", std::string{}),
            std::string{"LogPrint"}
        );
        bool hasPermissionMiddleware = false;
        for (const auto& item : snapshot["middlewares"]) {
            if (item.value("name", std::string{}) == "PermissionMiddlewareHandle") {
                hasPermissionMiddleware = true;
            }
        }
        XX_TEST_EXPECT_TRUE(hasPermissionMiddleware);

        // 工具清单: 装配数 = 启用数 (未开启白名单过滤), 分享存储工具来自 builtin
        XX_TEST_EXPECT_EQ(
            snapshot["tools"].value("assembled_count", 0),
            snapshot["tools"].value("enabled_count", 0)
        );
        XX_TEST_EXPECT_GE(snapshot["tools"].value("enabled_count", 0), 1);
        bool foundShareStore = false;
        bool foundMiddlewareTool = false;
        for (const auto& item : snapshot["tools"]["assembled"]) {
            const auto name   = item.value("name", std::string{});
            const auto source = item.value("source", std::string{});
            XX_TEST_EXPECT_FALSE(item.value("filtered", false));
            if (name == "agentxx_share_store") {
                foundShareStore = true;
                XX_TEST_EXPECT_EQ(source, std::string{"builtin"});
            }
            if (source.starts_with("middleware:")) {
                foundMiddlewareTool = true;
            }
        }
        XX_TEST_EXPECT_TRUE(foundShareStore);
        XX_TEST_EXPECT_TRUE(foundMiddlewareTool);

        // 来源推断: MCP 命名空间前缀 / 未知名回退内置
        XX_TEST_EXPECT_EQ(
            agentxx::agent::resolveToolSource(*agent->agentContext, "dummy_list_files"),
            std::string{"mcp:dummy"}
        );
        XX_TEST_EXPECT_EQ(
            agentxx::agent::resolveToolSource(*agent->agentContext, "dummy"),
            std::string{"builtin"}
        );

        // 执行图与持久化
        XX_TEST_EXPECT_EQ(
            snapshot["graph"].value("name", std::string{}),
            std::string{"agentxx.default"}
        );
        XX_TEST_EXPECT_GE(snapshot["graph"].value("nodes", 0), 4);
        XX_TEST_EXPECT_FALSE(snapshot["persistence"].value("enabled", true));

        // 钩子清单: 7 个固定点都在, 没有插件登记处理器时为 0 (派发器也不在链上)
        XX_TEST_EXPECT_TRUE(snapshot.contains("hooks"));
        XX_TEST_EXPECT_EQ(snapshot["hooks"].value("count", 0), 7);
        XX_TEST_EXPECT_EQ(snapshot["hooks"].value("handlers", 0), 0);
        XX_TEST_EXPECT_TRUE(snapshot["hooks"]["points"].is_array());
        XX_TEST_EXPECT_EQ(snapshot["hooks"]["points"].size(), size_t{7});
        bool hookPointsNamed = false;
        for (const auto& point : snapshot["hooks"]["points"]) {
            XX_TEST_EXPECT_TRUE(point.value("count", -1) == 0);
            if (point.value("name", std::string{}) == "AGENT_START") {
                hookPointsNamed = true;
            }
        }
        XX_TEST_EXPECT_TRUE(hookPointsNamed);

        // 渲染 + 日志摘要都不应抛异常
        const auto rendered = joinLines(agentxx::agent::renderAssemblySnapshot(snapshot));
        XX_TEST_EXPECT_TRUE(has(rendered, "models: default=test-model"));
        XX_TEST_EXPECT_TRUE(has(rendered, "graph: agentxx.default"));
        XX_TEST_EXPECT_TRUE(has(rendered, "persistence: OFF"));
        // 钩子处理器清单也渲染出来 (没有处理器时只有一行标题)
        XX_TEST_EXPECT_TRUE(has(rendered, "hookHandlers[0] devMode=no:"));
        agentxx::agent::logAssemblySnapshot(snapshot, "test");
    }

    // ---- 运行侧快照: 开启工具白名单过滤, 被过滤的工具仍留记录 ----
    {
        auto cfg = std::make_shared<agentxx::agent::AgentConfig>();
        cfg->model.type      = "openai";
        cfg->model.name      = "test-model";
        cfg->model.modelName = "test-model";
        cfg->model.baseUrl   = "http://127.0.0.1:1/v1";
        cfg->model.apiKey    = "EMPTY";
        cfg->enableToolFiltering = true;
        cfg->toolWhitelist       = {"agentxx_share_store"};
        cfg->enableSubagent      = true;

        auto agent = co_await makeInitializedAgent(cfg);
        const auto snapshot = agentxx::agent::buildRuntimeSnapshot(*agent->agentContext);

        XX_TEST_EXPECT_EQ(snapshot["tools"].value("enabled_count", 0), 1);
        // 被过滤的记录先写入 (过滤发生在装配记录之前), 启用项要在整体列表中找到
        bool foundEnabledShareStore = false;
        for (const auto& item : snapshot["tools"]["assembled"]) {
            if (!item.value("filtered", false) && item.value("name", std::string{}) == "agentxx_share_store") {
                foundEnabledShareStore = true;
            }
        }
        XX_TEST_EXPECT_TRUE(foundEnabledShareStore);

        bool        sawFiltered = false;
        std::string filteredReason;
        for (const auto& item : snapshot["tools"]["assembled"]) {
            if (item.value("filtered", false)) {
                sawFiltered    = true;
                filteredReason = item.value("filter_reason", std::string{});
                XX_TEST_EXPECT_FALSE(item.value("name", std::string{}).empty());
            }
        }
        XX_TEST_EXPECT_TRUE(sawFiltered);
        XX_TEST_EXPECT_TRUE(has(filteredReason, "toolWhitelist"));
        // 装配数 = 启用数 + 被过滤数
        XX_TEST_EXPECT_EQ(
            snapshot["tools"].value("assembled_count", 0),
            snapshot["tools"].value("enabled_count", 0) + 1
        );

        const auto rendered = joinLines(agentxx::agent::renderAssemblySnapshot(snapshot));
        XX_TEST_EXPECT_TRUE(has(rendered, "FILTERED"));
    }

    co_return TestResult{g_as_passed - passedBefore, g_as_failed - failedBefore};
}

} // namespace test
} // namespace agentxx
