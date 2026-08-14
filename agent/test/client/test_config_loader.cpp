#include "test_config_loader.h"

#include "agentxx-client/config_loader.h"
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <string>
#include <system_error>

namespace agentxx {
namespace test {

int g_config_loader_passed = 0;
int g_config_loader_failed = 0;

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// 辅助: 写入临时 yaml 并加载
// ---------------------------------------------------------------------------

static agentxx::client::YamlAppConfig loadYaml(std::string_view content) {
    auto path = fs::temp_directory_path()
                / fmt::format(
                    "agentxx_config_loader_test_{}.yaml",
                    std::chrono::steady_clock::now().time_since_epoch().count()
                );
    {
        std::ofstream ofs(path);
        ofs << content;
    }
    auto            cfg = agentxx::client::loadYamlConfig(path.string(), {}, {});
    std::error_code ec;
    fs::remove(path, ec);
    return cfg;
}

// ---------------------------------------------------------------------------
// permission.mode 解析 (yaml `permission` 块)
// ---------------------------------------------------------------------------

void test_permission_mode_default_ask() {
    // 未配置 permission 块: 默认 Ask (工作目录内允许, 其他询问)
    auto cfg = loadYaml("data_dir: default\n");
    XX_TEST_EXPECT_TRUE(cfg.permissionMode == agent::PermissionMode::Ask);
}

void test_permission_mode_ask() {
    auto cfg = loadYaml("permission:\n  mode: ask\n");
    XX_TEST_EXPECT_TRUE(cfg.permissionMode == agent::PermissionMode::Ask);
}

void test_permission_mode_all_ask() {
    auto cfg = loadYaml("permission:\n  mode: all_ask\n");
    XX_TEST_EXPECT_TRUE(cfg.permissionMode == agent::PermissionMode::AllAsk);
}

void test_permission_mode_pass() {
    auto cfg = loadYaml("permission:\n  mode: pass\n");
    XX_TEST_EXPECT_TRUE(cfg.permissionMode == agent::PermissionMode::Pass);
}

void test_permission_mode_deny() {
    auto cfg = loadYaml("permission:\n  mode: deny\n");
    XX_TEST_EXPECT_TRUE(cfg.permissionMode == agent::PermissionMode::Deny);
}

void test_permission_mode_case_insensitive() {
    // 忽略大小写: PASS / Ask / ALL_ASK / Deny 均合法
    auto cfg = loadYaml("permission:\n  mode: PASS\n");
    XX_TEST_EXPECT_TRUE(cfg.permissionMode == agent::PermissionMode::Pass);
    cfg = loadYaml("permission:\n  mode: Ask\n");
    XX_TEST_EXPECT_TRUE(cfg.permissionMode == agent::PermissionMode::Ask);
    cfg = loadYaml("permission:\n  mode: ALL_ASK\n");
    XX_TEST_EXPECT_TRUE(cfg.permissionMode == agent::PermissionMode::AllAsk);
    cfg = loadYaml("permission:\n  mode: Deny\n");
    XX_TEST_EXPECT_TRUE(cfg.permissionMode == agent::PermissionMode::Deny);
}

void test_permission_mode_invalid_fallback() {
    // 非法值: 警告并回退默认 ask, 不崩溃
    auto cfg = loadYaml("permission:\n  mode: always\n");
    XX_TEST_EXPECT_TRUE(cfg.permissionMode == agent::PermissionMode::Ask);
}

void test_permission_mode_env_expand() {
    // 支持 ${VAR} 展开 (pass 由环境变量注入)
    auto path = fs::temp_directory_path()
                / fmt::format(
                    "agentxx_config_loader_test_{}.yaml",
                    std::chrono::steady_clock::now().time_since_epoch().count()
                );
    {
        std::ofstream ofs(path);
        ofs << "permission:\n  mode: ${AGENTXX_TEST_PERM_MODE}\n";
    }
    auto cfg = agentxx::client::loadYamlConfig(
        path.string(),
        {
            {"AGENTXX_TEST_PERM_MODE", "pass"}
    },
        {}
    );
    XX_TEST_EXPECT_TRUE(cfg.permissionMode == agent::PermissionMode::Pass);
    std::error_code ec;
    fs::remove(path, ec);
}

// ---------------------------------------------------------------------------
// permission.whitelist / permission.blacklist 解析
// ---------------------------------------------------------------------------

void test_permission_lists_parse() {
    auto cfg = loadYaml(R"(permission:
  whitelist:
    - "C:/work/trusted"
    - "/home/user/shared/**"
  blacklist:
    - "C:/secret"
    - "/home/user/.ssh"
)");
    // 白名单
    XX_TEST_EXPECT_EQ(cfg.permissionAllowPaths.size(), size_t{2});
    if (cfg.permissionAllowPaths.size() == 2) {
        XX_TEST_EXPECT_EQ(cfg.permissionAllowPaths[0], std::string("C:/work/trusted"));
        XX_TEST_EXPECT_EQ(cfg.permissionAllowPaths[1], std::string("/home/user/shared/**"));
    }
    // 黑名单
    XX_TEST_EXPECT_EQ(cfg.permissionDenyPaths.size(), size_t{2});
    if (cfg.permissionDenyPaths.size() == 2) {
        XX_TEST_EXPECT_EQ(cfg.permissionDenyPaths[0], std::string("C:/secret"));
        XX_TEST_EXPECT_EQ(cfg.permissionDenyPaths[1], std::string("/home/user/.ssh"));
    }
}

void test_permission_lists_absent() {
    // permission 块仅有 mode: 名单为空
    auto cfg = loadYaml("permission:\n  mode: deny\n");
    XX_TEST_EXPECT_TRUE(cfg.permissionAllowPaths.empty());
    XX_TEST_EXPECT_TRUE(cfg.permissionDenyPaths.empty());
}

void test_permission_lists_env_expand() {
    // 名单项支持 ${VAR} 展开; 无环境变量时保留占位符原样
    auto cfg = loadYaml(R"(permission:
  whitelist:
    - "${AGENTXX_TEST_ALLOW_DIR}/trusted"
  blacklist:
    - "${AGENTXX_TEST_DENY_DIR}/secret"
)");
    XX_TEST_EXPECT_EQ(cfg.permissionAllowPaths.size(), size_t{1});
    if (cfg.permissionAllowPaths.size() == 1) {
        XX_TEST_EXPECT_TRUE(
            cfg.permissionAllowPaths[0].find("${AGENTXX_TEST_ALLOW_DIR}") != std::string::npos
        );
    }
}

void test_permission_legacy_flat_keys_ignored() {
    // 旧的扁平键 (permission_mode/permission_whitelist/permission_blacklist) 已废弃,
    // 仅 `permission` 块生效: 扁平键出现时不产生任何效果
    auto cfg = loadYaml(R"(permission_mode: deny
permission_whitelist:
  - "/tmp/legacy"
)");
    XX_TEST_EXPECT_TRUE(cfg.permissionMode == agent::PermissionMode::Ask);
    XX_TEST_EXPECT_TRUE(cfg.permissionAllowPaths.empty());
}

// ---------------------------------------------------------------------------
// mcp / skill / memory 解析 (yaml `mcp` / `skill` / `memory` 键)
// ---------------------------------------------------------------------------

void test_mcp_parse_basic() {
    // mcp 列表项: namespace + url + timeout (秒, 默认 120)
    auto cfg = loadYaml(R"(mcp:
  - namespace: fs
    url: "http://127.0.0.1:8000/mcp"
  - namespace: git
    url: "http://127.0.0.1:8001/mcp"
    timeout: 30
)");
    XX_TEST_EXPECT_EQ(cfg.mcpServers.size(), size_t{2});
    auto it = cfg.mcpServers.find("fs");
    XX_TEST_EXPECT_TRUE(it != cfg.mcpServers.end());
    if (it != cfg.mcpServers.end()) {
        XX_TEST_EXPECT_EQ(it->second.url, std::string("http://127.0.0.1:8000/mcp"));
        // 未配置 timeout: 默认 120 秒
        XX_TEST_EXPECT_EQ(it->second.toolTimeout.count(), 120 * 1000);
    }
    it = cfg.mcpServers.find("git");
    XX_TEST_EXPECT_TRUE(it != cfg.mcpServers.end());
    if (it != cfg.mcpServers.end()) {
        XX_TEST_EXPECT_EQ(it->second.url, std::string("http://127.0.0.1:8001/mcp"));
        XX_TEST_EXPECT_EQ(it->second.toolTimeout.count(), 30 * 1000);
    }
}

void test_mcp_timeout_zero_unlimited() {
    // timeout: 0 表示不限制 (toolTimeout 为 0)
    auto cfg = loadYaml(R"(mcp:
  - namespace: fs
    url: "http://127.0.0.1:8000/mcp"
    timeout: 0
)");
    auto it  = cfg.mcpServers.find("fs");
    XX_TEST_EXPECT_TRUE(it != cfg.mcpServers.end());
    if (it != cfg.mcpServers.end()) {
        XX_TEST_EXPECT_EQ(it->second.toolTimeout.count(), 0);
    }
}

void test_mcp_timeout_invalid_fallback() {
    // 非法 timeout 值: 容错回退默认 120 秒, 不崩溃
    auto cfg = loadYaml(R"(mcp:
  - namespace: fs
    url: "http://127.0.0.1:8000/mcp"
    timeout: abc
)");
    auto it  = cfg.mcpServers.find("fs");
    XX_TEST_EXPECT_TRUE(it != cfg.mcpServers.end());
    if (it != cfg.mcpServers.end()) {
        XX_TEST_EXPECT_EQ(it->second.toolTimeout.count(), 120 * 1000);
    }
}

void test_mcp_missing_fields_skipped() {
    // 缺 namespace 或 url 的条目跳过, 不影响其他条目
    auto cfg = loadYaml(R"(mcp:
  - namespace: ok
    url: "http://127.0.0.1:8000/mcp"
  - url: "http://127.0.0.1:8001/mcp"
  - namespace: no-url
)");
    XX_TEST_EXPECT_EQ(cfg.mcpServers.size(), size_t{1});
    XX_TEST_EXPECT_TRUE(cfg.mcpServers.contains("ok"));
}

void test_mcp_env_expand() {
    // namespace/url/timeout 均支持 ${VAR} 展开
    auto path = fs::temp_directory_path()
                / fmt::format(
                    "agentxx_config_loader_test_{}.yaml",
                    std::chrono::steady_clock::now().time_since_epoch().count()
                );
    {
        std::ofstream ofs(path);
        ofs << R"(mcp:
  - namespace: ${AGENTXX_TEST_MCP_NS}
    url: "${AGENTXX_TEST_MCP_URL}"
    timeout: ${AGENTXX_TEST_MCP_TIMEOUT}
)";
    }
    auto cfg = agentxx::client::loadYamlConfig(
        path.string(),
        {
            {"AGENTXX_TEST_MCP_NS",      "fs"                       },
            {"AGENTXX_TEST_MCP_URL",     "http://127.0.0.1:9000/mcp"},
            {"AGENTXX_TEST_MCP_TIMEOUT", "45"                       },
    },
        {}
    );
    std::error_code ec;
    fs::remove(path, ec);

    auto it = cfg.mcpServers.find("fs");
    XX_TEST_EXPECT_TRUE(it != cfg.mcpServers.end());
    if (it != cfg.mcpServers.end()) {
        XX_TEST_EXPECT_EQ(it->second.url, std::string("http://127.0.0.1:9000/mcp"));
        XX_TEST_EXPECT_EQ(it->second.toolTimeout.count(), 45 * 1000);
    }
}

void test_skill_parse() {
    auto cfg = loadYaml(R"(skill:
  - "C:/skills/skill_a"
  - "C:/skills/skill_b"
)");
    XX_TEST_EXPECT_EQ(cfg.skillDirPaths.size(), size_t{2});
    if (cfg.skillDirPaths.size() == 2) {
        XX_TEST_EXPECT_EQ(cfg.skillDirPaths[0], std::string("C:/skills/skill_a"));
        XX_TEST_EXPECT_EQ(cfg.skillDirPaths[1], std::string("C:/skills/skill_b"));
    }
}

void test_memory_parse() {
    auto cfg = loadYaml(R"(memory:
  - "C:/memory/memory_a.md"
  - "C:/memory/memory_b.md"
)");
    XX_TEST_EXPECT_EQ(cfg.memoryFilePaths.size(), size_t{2});
    if (cfg.memoryFilePaths.size() == 2) {
        XX_TEST_EXPECT_EQ(cfg.memoryFilePaths[0], std::string("C:/memory/memory_a.md"));
        XX_TEST_EXPECT_EQ(cfg.memoryFilePaths[1], std::string("C:/memory/memory_b.md"));
    }
}

// ---------------------------------------------------------------------------
// 模型连接池配置 (yaml `models[].max_concurrent_connections`, 默认 5)
// ---------------------------------------------------------------------------

void test_model_max_concurrent_connections() {
    // 未配置: 默认 5
    auto cfg = loadYaml(R"(models:
  - name: m1
    type: "openai"
    base_url: "http://127.0.0.1:8000/v1"
)");
    auto it = cfg.models.find("m1");
    XX_TEST_EXPECT_TRUE(it != cfg.models.end());
    if (it != cfg.models.end()) {
        XX_TEST_EXPECT_EQ(it->second.maxConcurrentConnections, size_t{5});
    }

    // 显式指定
    cfg = loadYaml(R"(models:
  - name: m2
    type: "openai"
    base_url: "http://127.0.0.1:8000/v1"
    max_concurrent_connections: 3
)");
    it = cfg.models.find("m2");
    XX_TEST_EXPECT_TRUE(it != cfg.models.end());
    if (it != cfg.models.end()) {
        XX_TEST_EXPECT_EQ(it->second.maxConcurrentConnections, size_t{3});
    }

    // 0 = 不限制
    cfg = loadYaml(R"(models:
  - name: m3
    type: "openai"
    max_concurrent_connections: 0
)");
    it = cfg.models.find("m3");
    XX_TEST_EXPECT_TRUE(it != cfg.models.end());
    if (it != cfg.models.end()) {
        XX_TEST_EXPECT_EQ(it->second.maxConcurrentConnections, size_t{0});
    }

    // 非法值: 容错回退默认 5, 不崩溃
    cfg = loadYaml(R"(models:
  - name: m4
    type: "openai"
    max_concurrent_connections: abc
)");
    it = cfg.models.find("m4");
    XX_TEST_EXPECT_TRUE(it != cfg.models.end());
    if (it != cfg.models.end()) {
        XX_TEST_EXPECT_EQ(it->second.maxConcurrentConnections, size_t{5});
    }
}

TestResult testConfigLoader() {
    g_config_loader_passed = 0;
    g_config_loader_failed = 0;

    test_permission_mode_default_ask();
    test_permission_mode_ask();
    test_permission_mode_all_ask();
    test_permission_mode_pass();
    test_permission_mode_deny();
    test_permission_mode_case_insensitive();
    test_permission_mode_invalid_fallback();
    test_permission_mode_env_expand();
    test_permission_lists_parse();
    test_permission_lists_absent();
    test_permission_lists_env_expand();
    test_permission_legacy_flat_keys_ignored();
    test_mcp_parse_basic();
    test_mcp_timeout_zero_unlimited();
    test_mcp_timeout_invalid_fallback();
    test_mcp_missing_fields_skipped();
    test_mcp_env_expand();
    test_skill_parse();
    test_memory_parse();
    test_model_max_concurrent_connections();

    return TestResult{g_config_loader_passed, g_config_loader_failed};
}

} // namespace test
} // namespace agentxx
