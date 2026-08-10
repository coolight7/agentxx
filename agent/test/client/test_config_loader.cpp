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
    auto cfg = agentxx::client::loadYamlConfig(path.string(), {}, {});
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
        {{"AGENTXX_TEST_PERM_MODE", "pass"}},
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

    return TestResult{g_config_loader_passed, g_config_loader_failed};
}

} // namespace test
} // namespace agentxx
