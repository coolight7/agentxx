/// 配置校验测试 (计划 CFG-1)
///
/// 覆盖:
/// - 致命项: 无可用模型、`model.use` 指向不存在的模型、`work_dir` 非绝对路径、
///   插件 `config` 非绝对路径
/// - 警告项: 会被夹取的取值、空白名单 + 开启过滤、白黑名单同路径、Deny 模式 + 白名单、
///   插件路径重复/不存在、资源路径不存在、内存模式 (无 data_dir)
/// - 路径可写性: 持久化开启且会话根不可用 → 致命
/// - 结构化输出与来源标记
#include "agentxx-test/core/test_config_validation.h"

#include "agentxx/agent/config.h"
#include "agentxx/agent/config_validation.h"
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <string>

namespace {
int g_cv_passed = 0;
int g_cv_failed = 0;
} // namespace

#define XX_TEST_PASSED g_cv_passed
#define XX_TEST_FAILED g_cv_failed

namespace fs = std::filesystem;

namespace agentxx {
namespace test {

namespace {

using agentxx::agent::ConfigIssueLevel;
using agentxx::agent::ConfigValidationReport;

/// 临时目录 (测试结束由调用方删除)
std::string makeTempDir(std::string_view tag) {
    auto dir = fs::temp_directory_path()
               / fmt::format(
                   "agentxx_cfg_validate_{}_{}",
                   tag,
                   std::chrono::steady_clock::now().time_since_epoch().count()
               );
    fs::create_directories(dir);
    return dir.string();
}

/// 报告中是否出现指定键路径
bool hasKey(const ConfigValidationReport& report, std::string_view key) {
    for (const auto& issue : report.issues) {
        if (issue.keyPath == key) {
            return true;
        }
    }
    return false;
}

/// 取指定键路径的第一条问题文本
std::string messageOf(const ConfigValidationReport& report, std::string_view key) {
    for (const auto& issue : report.issues) {
        if (issue.keyPath == key) {
            return issue.message;
        }
    }
    return {};
}

/// 一份"可正常启动"的基础配置 (模型 + 绝对路径都在)
agentxx::agent::AgentConfig makeValidConfig(const std::string& root) {
    agentxx::agent::AgentConfig cfg;
    cfg.model.baseUrl   = "http://127.0.0.1:1/v1";
    cfg.model.apiKey    = "EMPTY";
    cfg.model.modelName = "test-model";
    cfg.dataDir         = root;
    cfg.workDir         = root;
    cfg.enableSessionStore = true;
    cfg.availableModels["test-model"] = cfg.model;
    cfg.currentModelName              = "test-model";
    return cfg;
}

} // namespace

TestResult testConfigValidation() {
    const int passedBefore = g_cv_passed;
    const int failedBefore = g_cv_failed;

    const auto root = makeTempDir("root");

    // ---- 正常配置: 无致命问题 (可能仍有内存/资源类警告) ----
    {
        auto cfg    = makeValidConfig(root);
        auto report = agentxx::agent::validateAgentConfigWithPaths(cfg);
        XX_TEST_EXPECT_FALSE(report.hasFatal());
    }

    // ---- 无可用模型 → 致命 ----
    {
        agentxx::agent::AgentConfig cfg;
        cfg.dataDir = root;
        auto report = agentxx::agent::validateAgentConfig(cfg);
        XX_TEST_EXPECT_TRUE(report.hasFatal());
        XX_TEST_EXPECT_TRUE(hasKey(report, "model.list"));
        XX_TEST_EXPECT_GE(report.count(ConfigIssueLevel::Fatal), size_t{1});
    }

    // ---- model.use 指向不存在模型 → 致命 ----
    {
        auto cfg              = makeValidConfig(root);
        cfg.currentModelName  = "not-there";
        auto report           = agentxx::agent::validateAgentConfig(cfg);
        XX_TEST_EXPECT_TRUE(report.hasFatal());
        XX_TEST_EXPECT_TRUE(hasKey(report, "model.use"));
        XX_TEST_EXPECT_TRUE(messageOf(report, "model.use").find("not-there") != std::string::npos);
    }

    // ---- work_dir 相对路径 → 致命; 错误信息带键路径 ----
    {
        auto cfg    = makeValidConfig(root);
        cfg.workDir = "relative/dir";
        auto report = agentxx::agent::validateAgentConfig(cfg);
        XX_TEST_EXPECT_TRUE(report.hasFatal());
        XX_TEST_EXPECT_TRUE(hasKey(report, "work_dir"));
        const auto rendered = report.render();
        XX_TEST_EXPECT_FALSE(rendered.empty());
        if (!rendered.empty()) {
            XX_TEST_EXPECT_TRUE(rendered.front().starts_with("[fatal] work_dir"));
        }
        // AgentConfig::validate() 与同一套规则同源
        XX_TEST_EXPECT_FALSE(cfg.validate().has_value());
    }

    // ---- 插件 config 相对路径 → 致命; 插件路径重复/缺失 → 警告 ----
    {
        auto cfg = makeValidConfig(root);
        agentxx::agent::PluginConfig pc;
        pc.path       = (fs::path{root} / "plugin.dll").string();
        pc.configPath = "relative.yaml";
        cfg.plugins.push_back(pc);

        auto report = agentxx::agent::validateAgentConfig(cfg);
        XX_TEST_EXPECT_TRUE(report.hasFatal());

        cfg.plugins.clear();
        agentxx::agent::PluginConfig missing;
        missing.path = (fs::path{root} / "not_exists_plugin.dll").string();
        cfg.plugins.push_back(missing);
        cfg.plugins.push_back(missing); // 同路径声明两次
        auto pathReport = agentxx::agent::validateConfigPaths(cfg);
        XX_TEST_EXPECT_FALSE(pathReport.hasFatal());
        XX_TEST_EXPECT_TRUE(hasKey(pathReport, "plugin.list[0]"));
        XX_TEST_EXPECT_TRUE(messageOf(pathReport, "plugin.list[0]").find("does not exist") != std::string::npos);

        auto dupReport = agentxx::agent::validateAgentConfig(cfg);
        XX_TEST_EXPECT_TRUE(hasKey(dupReport, "plugin.list[1]"));
        XX_TEST_EXPECT_TRUE(
            messageOf(dupReport, "plugin.list[1]").find("duplicates") != std::string::npos
        );
    }

    // ---- 权限组合: Deny + 白名单、Pass 模式、白黑名单同路径 ----
    {
        auto cfg                      = makeValidConfig(root);
        cfg.permissionMode            = agentxx::agent::PermissionMode::Deny;
        cfg.permissionAllowPaths      = {root};
        auto report                   = agentxx::agent::validateAgentConfig(cfg);
        XX_TEST_EXPECT_FALSE(report.hasFatal());
        XX_TEST_EXPECT_TRUE(hasKey(report, "permission.mode"));
        XX_TEST_EXPECT_TRUE(messageOf(report, "permission.mode").find("whitelist still wins") != std::string::npos);
    }
    {
        auto cfg           = makeValidConfig(root);
        cfg.permissionMode = agentxx::agent::PermissionMode::Pass;
        auto report        = agentxx::agent::validateAgentConfig(cfg);
        XX_TEST_EXPECT_TRUE(messageOf(report, "permission.mode").find("`pass` allows every path") != std::string::npos);
    }
    {
        auto cfg                 = makeValidConfig(root);
        cfg.permissionAllowPaths = {root};
        cfg.permissionDenyPaths  = {root};
        auto report              = agentxx::agent::validateAgentConfig(cfg);
        XX_TEST_EXPECT_TRUE(hasKey(report, "permission.whitelist[0]"));
        XX_TEST_EXPECT_TRUE(
            messageOf(report, "permission.whitelist[0]").find("blacklist wins") != std::string::npos
        );
    }

    // ---- 取值会被夹取 / 组合无意义 ----
    {
        auto cfg                           = makeValidConfig(root);
        cfg.toolParallelMaxConcurrency     = 99;
        cfg.enableToolFiltering            = true;
        cfg.toolWhitelist.clear();
        cfg.enableSessionStore             = false;
        cfg.sessionStoreDirectory          = root; // 配了根但关掉持久化
        auto report                        = agentxx::agent::validateAgentConfig(cfg);
        XX_TEST_EXPECT_TRUE(hasKey(report, "tool.parallel_max"));
        XX_TEST_EXPECT_TRUE(hasKey(report, "tool.whitelist"));
        XX_TEST_EXPECT_TRUE(hasKey(report, "session_store.enable"));
        XX_TEST_EXPECT_FALSE(report.hasFatal());
    }

    // ---- 内存模式 (无 data_dir) → 警告而非致命 ----
    {
        auto cfg           = makeValidConfig(root);
        cfg.dataDir        = "";
        cfg.enableSessionStore = false;
        auto report        = agentxx::agent::validateConfigPaths(cfg);
        XX_TEST_EXPECT_FALSE(report.hasFatal());
        XX_TEST_EXPECT_TRUE(hasKey(report, "data_dir"));
        XX_TEST_EXPECT_TRUE(messageOf(report, "data_dir").find("memory") != std::string::npos);
    }

    // ---- 持久化开启但会话根不可用 (被同名文件占用) → 致命 ----
    {
        const auto blockedRoot = (fs::path{root} / "blocked_root").string();
        {
            std::ofstream ofs{utilxx_base::utf8ToPath(blockedRoot)};
            ofs << "not a directory";
        }
        auto cfg                      = makeValidConfig(root);
        cfg.sessionStoreDirectory     = blockedRoot;
        auto report                   = agentxx::agent::validateConfigPaths(cfg);
        XX_TEST_EXPECT_TRUE(report.hasFatal());
        XX_TEST_EXPECT_TRUE(hasKey(report, "session_store.directory"));
        XX_TEST_EXPECT_TRUE(
            messageOf(report, "session_store.directory").find("not a directory") != std::string::npos
        );
    }

    // ---- 资源路径不存在 → 警告 (启动时记为加载失败) ----
    {
        auto cfg = makeValidConfig(root);
        cfg.skillDirPaths.push_back((fs::path{root} / "no_such_skill_dir").string());
        cfg.memoryFilePaths.push_back((fs::path{root} / "no_such_memory.md").string());
        cfg.ragDocsPaths.push_back((fs::path{root} / "no_such_docs").string());
        auto report = agentxx::agent::validateConfigPaths(cfg);
        XX_TEST_EXPECT_FALSE(report.hasFatal());
        XX_TEST_EXPECT_TRUE(hasKey(report, "skill.paths[0]"));
        XX_TEST_EXPECT_TRUE(hasKey(report, "memory.paths[0]"));
        XX_TEST_EXPECT_TRUE(hasKey(report, "rag.paths[0]"));
    }

    // ---- MCP: 空命名空间 / 非 http 端点 ----
    {
        auto cfg = makeValidConfig(root);
        cfg.mcpServerUrls[""] = agentxx::agent::McpServerConfig{.url = "http://127.0.0.1:1/sse"};
        cfg.mcpServerUrls["bad"] = agentxx::agent::McpServerConfig{.url = "ftp://example.com"};
        auto structure = agentxx::agent::validateAgentConfig(cfg);
        XX_TEST_EXPECT_TRUE(hasKey(structure, "mcp.list"));
        auto paths = agentxx::agent::validateConfigPaths(cfg);
        XX_TEST_EXPECT_TRUE(hasKey(paths, "mcp.list[bad].url"));
    }

    // ---- 结构化输出与来源标记 ----
    {
        auto cfg    = makeValidConfig(root);
        cfg.workDir = "relative/dir";
        auto report = agentxx::agent::validateAgentConfig(cfg);
        report.setSource("agentxx-config.yaml");
        const auto json = report.toJson();
        XX_TEST_EXPECT_TRUE(json.is_array());
        XX_TEST_EXPECT_GE(json.size(), size_t{1});
        if (json.size() > 0) {
            XX_TEST_EXPECT_EQ(json[0].value("level", std::string{}), std::string{"fatal"});
            XX_TEST_EXPECT_EQ(json[0].value("key", std::string{}), std::string{"work_dir"});
            XX_TEST_EXPECT_EQ(
                json[0].value("source", std::string{}),
                std::string{"agentxx-config.yaml"}
            );
        }
        // render 带来源
        const auto rendered = report.render();
        XX_TEST_EXPECT_FALSE(rendered.empty());
        if (!rendered.empty()) {
            XX_TEST_EXPECT_TRUE(rendered[0].find("agentxx-config.yaml") != std::string::npos);
        }
    }

    std::error_code ec;
    fs::remove_all(utilxx_base::utf8ToPath(root), ec);

    return TestResult{g_cv_passed - passedBefore, g_cv_failed - failedBefore};
}

} // namespace test
} // namespace agentxx
