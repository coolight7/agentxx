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
// 插件配置解析 (yaml `plugins` 列表项: path / enabled / args)
// ---------------------------------------------------------------------------

void test_plugins_parse_basic() {
    // 列表项: path (必填) + enabled (默认 true) + args (任意 YAML → JSON)
    auto cfg = loadYaml(R"(plugins:
  - path: "/opt/plugins/my_plugin.so"
  - path: "/opt/plugins/example_js"
    enabled: false
    args:
      foo: bar
      n: 42
)");
    XX_TEST_EXPECT_EQ(cfg.plugins.size(), size_t{2});
    if (cfg.plugins.size() == 2) {
        // 默认 enabled = true
        XX_TEST_EXPECT_TRUE(cfg.plugins[0].enabled);
        XX_TEST_EXPECT_EQ(cfg.plugins[0].path, std::string("/opt/plugins/my_plugin.so"));
        // enabled: false 生效
        XX_TEST_EXPECT_FALSE(cfg.plugins[1].enabled);
        XX_TEST_EXPECT_EQ(cfg.plugins[1].path, std::string("/opt/plugins/example_js"));
        // args 解析为 JSON 对象
        auto& args = cfg.plugins[1].args;
        XX_TEST_EXPECT_TRUE(args.is_object());
        if (args.is_object()) {
            XX_TEST_EXPECT_EQ(args["foo"].get<std::string>(), std::string("bar"));
            XX_TEST_EXPECT_EQ(args["n"].get<int>(), 42);
        }
    }
}

void test_plugins_missing_path_skipped() {
    // 缺 path 或非 map 条目跳过; 空 plugins (全注释) 不报错
    auto cfg = loadYaml(R"(plugins:
  - enabled: true
  - 123
  - path: "/opt/plugins/ok.so"
)");
    XX_TEST_EXPECT_EQ(cfg.plugins.size(), size_t{1});
    if (cfg.plugins.size() == 1) {
        XX_TEST_EXPECT_EQ(cfg.plugins[0].path, std::string("/opt/plugins/ok.so"));
    }
    // 全注释的 plugins 段 (null): 解析为空列表
    cfg = loadYaml("plugins:\n  # - path: \"/opt/plugins/x.so\"\n");
    XX_TEST_EXPECT_TRUE(cfg.plugins.empty());
    // 未配置 plugins 段
    cfg = loadYaml("data_dir: default\n");
    XX_TEST_EXPECT_TRUE(cfg.plugins.empty());
}

void test_plugins_env_expand() {
    // path 支持 ${VAR} 环境变量展开
    auto cfg = loadYaml("plugins:\n  - path: \"${PLUGIN_DIR}/my_plugin.so\"\n");
    XX_TEST_EXPECT_EQ(cfg.plugins.size(), size_t{1});
    if (cfg.plugins.size() == 1) {
        XX_TEST_EXPECT_EQ(
            cfg.plugins[0].path,
            std::string("${PLUGIN_DIR}/my_plugin.so") // 无对应环境变量: 保留原样
        );
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
    auto it  = cfg.models.find("m1");
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
    it  = cfg.models.find("m2");
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
    it  = cfg.models.find("m3");
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
    it  = cfg.models.find("m4");
    XX_TEST_EXPECT_TRUE(it != cfg.models.end());
    if (it != cfg.models.end()) {
        XX_TEST_EXPECT_EQ(it->second.maxConcurrentConnections, size_t{5});
    }
}

// ---------------------------------------------------------------------------
// codegraph 参数迁移到插件配置 (yaml `plugins` 条目 args):
// 宿主只整体解析 args json, 不解析其字段语义 (字段由插件自行定义)
// ---------------------------------------------------------------------------

void test_plugins_empty_by_default() {
    // 未配置 plugins 段: 列表为空
    auto cfg = loadYaml("data_dir: default\n");
    XX_TEST_EXPECT_TRUE(cfg.plugins.empty());
}

void test_plugin_name_form_removed() {
    // name 形式已移除: 所有插件统一经 path 外置指定;
    // 仅配置 name (无 path) 的条目被跳过 (记警告)
    auto cfg = loadYaml(R"(plugins:
  - name: agentxx_codegraph
    enabled: true
    args:
      load_cwd: true
      use_gitignore: false
)");
    XX_TEST_EXPECT_TRUE(cfg.plugins.empty());
    // 同时提供 name + path: 正常加载 (name 字段被忽略, 仅 path 生效)
    cfg = loadYaml(R"(plugins:
  - name: agentxx_codegraph
    path: "/opt/plugins/agentxx_codegraph"
    enabled: true
)");
    XX_TEST_EXPECT_EQ(cfg.plugins.size(), size_t{1});
    if (cfg.plugins.size() == 1) {
        XX_TEST_EXPECT_EQ(cfg.plugins[0].path, std::string("/opt/plugins/agentxx_codegraph"));
        XX_TEST_EXPECT_TRUE(cfg.plugins[0].enabled);
    }
}

void test_plugin_args_paths_parse() {
    // args 内路径列表原样解析 (宿主不解析语义)
    auto cfg = loadYaml(R"(plugins:
  - path: "/opt/plugins/agentxx_codegraph"
    enabled: true
    args:
      paths:
        - "/path/to/project_a"
        - "relative/path/project_b"
      ignore_paths:
        - "/path/to/project_a/third_party"
        - "**/generated/**"
)");
    XX_TEST_EXPECT_EQ(cfg.plugins.size(), size_t{1});
    if (cfg.plugins.size() != 1) {
        return;
    }
    const auto& pc = cfg.plugins[0];
    XX_TEST_EXPECT_EQ(pc.path, std::string("/opt/plugins/agentxx_codegraph"));
    XX_TEST_EXPECT_TRUE(pc.args.is_object());
    if (pc.args.is_object() && pc.args.contains("paths")) {
        const auto& paths = pc.args["paths"];
        XX_TEST_EXPECT_EQ(paths.size(), size_t{2});
        if (paths.size() == 2) {
            XX_TEST_EXPECT_EQ(paths[0].get<std::string>(), std::string("/path/to/project_a"));
            XX_TEST_EXPECT_EQ(paths[1].get<std::string>(), std::string("relative/path/project_b"));
        }
    } else {
        XX_TEST_EXPECT_TRUE(false);
    }
    if (pc.args.is_object() && pc.args.contains("ignore_paths")) {
        const auto& ig = pc.args["ignore_paths"];
        XX_TEST_EXPECT_EQ(ig.size(), size_t{2});
        if (ig.size() == 2) {
            XX_TEST_EXPECT_EQ(
                ig[0].get<std::string>(),
                std::string("/path/to/project_a/third_party")
            );
            XX_TEST_EXPECT_EQ(ig[1].get<std::string>(), std::string("**/generated/**"));
        }
    } else {
        XX_TEST_EXPECT_TRUE(false);
    }
}

void test_plugin_missing_path_skipped() {
    // path 缺失 (唯一必填字段): 跳过 (记警告)
    auto cfg = loadYaml("plugins:\n  - enabled: true\n");
    XX_TEST_EXPECT_TRUE(cfg.plugins.empty());
}

void test_plugin_args_env_expand() {
    // 插件 path/args 值均支持 ${VAR} 展开
    auto path = fs::temp_directory_path()
                / fmt::format(
                    "agentxx_config_loader_test_{}.yaml",
                    std::chrono::steady_clock::now().time_since_epoch().count()
                );
    {
        std::ofstream ofs(path);
        ofs << R"(plugins:
  - path: ${AGENTXX_TEST_CG_PATH}/agentxx_codegraph
    enabled: ${AGENTXX_TEST_CG_ENABLE}
    args:
      paths:
        - "${AGENTXX_TEST_CG_PATH}"
      ignore_paths:
        - "${AGENTXX_TEST_CG_IGNORE}"
)";
    }
    auto cfg = agentxx::client::loadYamlConfig(
        path.string(),
        {
            {"AGENTXX_TEST_CG_ENABLE", "true"                     },
            {"AGENTXX_TEST_CG_PATH",   "/data/cg/proj"            },
            {"AGENTXX_TEST_CG_IGNORE", "/data/cg/proj/third_party"},
    },
        {}
    );
    std::error_code ec;
    fs::remove(path, ec);

    XX_TEST_EXPECT_EQ(cfg.plugins.size(), size_t{1});
    if (cfg.plugins.size() != 1) {
        return;
    }
    const auto& pc = cfg.plugins[0];
    XX_TEST_EXPECT_EQ(pc.path, std::string("/data/cg/proj/agentxx_codegraph"));
    XX_TEST_EXPECT_TRUE(pc.enabled);
    if (pc.args.is_object() && pc.args.contains("paths")) {
        const auto& paths = pc.args["paths"];
        XX_TEST_EXPECT_EQ(paths.size(), size_t{1});
        if (paths.size() == 1) {
            XX_TEST_EXPECT_EQ(paths[0].get<std::string>(), std::string("/data/cg/proj"));
        }
    } else {
        XX_TEST_EXPECT_TRUE(false);
    }
    if (pc.args.is_object() && pc.args.contains("ignore_paths")) {
        const auto& ig = pc.args["ignore_paths"];
        XX_TEST_EXPECT_EQ(ig.size(), size_t{1});
        if (ig.size() == 1) {
            XX_TEST_EXPECT_EQ(ig[0].get<std::string>(), std::string("/data/cg/proj/third_party"));
        }
    } else {
        XX_TEST_EXPECT_TRUE(false);
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
    test_plugins_parse_basic();
    test_plugins_missing_path_skipped();
    test_plugins_env_expand();
    test_model_max_concurrent_connections();
    test_plugins_empty_by_default();
    test_plugin_name_form_removed();
    test_plugin_args_paths_parse();
    test_plugin_missing_path_skipped();
    test_plugin_args_env_expand();

    return TestResult{g_config_loader_passed, g_config_loader_failed};
}

} // namespace test
} // namespace agentxx
