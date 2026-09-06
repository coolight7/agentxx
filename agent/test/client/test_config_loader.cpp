#include "test_config_loader.h"

#include "agentxx-client/config_loader.h"
#include "agentxx/util/env.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <string>
#include <system_error>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_config_loader_passed = 0;
int g_config_loader_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_config_loader_passed
#define XX_TEST_FAILED g_config_loader_failed

namespace agentxx {
namespace test {

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

/// 加载 yaml 并携带 .env 变量 (dotEnvVars)
static agentxx::client::YamlAppConfig loadYamlWithDotEnv(
    std::string_view                          content,
    const std::map<std::string, std::string>& dotEnvVars
) {
    auto path = fs::temp_directory_path()
                / fmt::format(
                    "agentxx_config_loader_test_{}.yaml",
                    std::chrono::steady_clock::now().time_since_epoch().count()
                );
    {
        std::ofstream ofs(path);
        ofs << content;
    }
    auto            cfg = agentxx::client::loadYamlConfig(path.string(), dotEnvVars, {});
    std::error_code ec;
    fs::remove(path, ec);
    return cfg;
}

// ---------------------------------------------------------------------------
// 系统环境变量读写辅助 (测试查找顺序用; 结束时恢复原值)
// ---------------------------------------------------------------------------

#if XX_IS_WIN_D
static void setSystemEnvVar(const std::string& key, const std::string& value) {
    _putenv_s(key.c_str(), value.c_str());
}

static void clearSystemEnvVar(const std::string& key) {
    _putenv_s(key.c_str(), "");
}
#else
static void setSystemEnvVar(const std::string& key, const std::string& value) {
    setenv(key.c_str(), value.c_str(), 1);
}

static void clearSystemEnvVar(const std::string& key) {
    unsetenv(key.c_str());
}
#endif

/// RAII: 设置系统环境变量, 析构时恢复原值/删除
class SystemEnvGuard {
public:

    SystemEnvGuard(const std::string& key, const std::string& value) :
        key_(key) {
        auto hadOpt = agentxx::util::ApplicationEnv::instance().getSystem(key_);
        existed_    = hadOpt.has_value();
        saved_      = hadOpt ? *hadOpt : std::string{};
        setSystemEnvVar(key_, value);
    }

    ~SystemEnvGuard() {
        if (existed_) {
            setSystemEnvVar(key_, saved_);
        } else {
            clearSystemEnvVar(key_);
        }
    }

    SystemEnvGuard(const SystemEnvGuard&)            = delete;
    SystemEnvGuard& operator=(const SystemEnvGuard&) = delete;

private:

    std::string key_;
    bool        existed_ = false;
    std::string saved_;
};

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
// 思考摘要请求配置 (yaml `models[].request_reasoning_summary`, 默认 true):
// 部分网关 (如 opencode-muse-spark) 不支持 reasoning.summary_text include 变体,
// 需设 false 避免 API 400
// ---------------------------------------------------------------------------

void test_model_request_reasoning_summary() {
    // 未配置: 默认 true (sendThinking 开启时请求思考摘要)
    auto cfg = loadYaml("models:\n  - name: m1\n    type: \"openai\"\n");
    auto it  = cfg.models.find("m1");
    XX_TEST_EXPECT_TRUE(it != cfg.models.end());
    if (it != cfg.models.end()) {
        XX_TEST_EXPECT_TRUE(it->second.requestReasoningSummary);
    }

    // send_thinking: true + 显式 request_reasoning_summary: false (opencode-muse-spark 场景)
    cfg = loadYaml(R"(models:
  - name: m2
    type: "openai-responses"
    send_thinking: true
    request_reasoning_summary: false
)");
    it  = cfg.models.find("m2");
    XX_TEST_EXPECT_TRUE(it != cfg.models.end());
    if (it != cfg.models.end()) {
        XX_TEST_EXPECT_TRUE(it->second.sendThinking);
        XX_TEST_EXPECT_FALSE(it->second.requestReasoningSummary);
    }

    // 显式 request_reasoning_summary: true
    cfg = loadYaml(R"(models:
  - name: m3
    type: "openai-responses"
    send_thinking: true
    request_reasoning_summary: true
)");
    it  = cfg.models.find("m3");
    XX_TEST_EXPECT_TRUE(it != cfg.models.end());
    if (it != cfg.models.end()) {
        XX_TEST_EXPECT_TRUE(it->second.requestReasoningSummary);
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
    // name 简写已恢复为内置插件快捷方式: 仅配置 name (无 path) 时自动补为
    // builtin://<name> (与 path: builtin://agentxx_codegraph 等价)
    auto cfg = loadYaml(R"(plugins:
  - name: agentxx_codegraph
    enabled: true
    args:
      load_cwd: true
      use_gitignore: false
)");
    XX_TEST_EXPECT_EQ(cfg.plugins.size(), size_t{1});
    if (cfg.plugins.size() == 1) {
        XX_TEST_EXPECT_EQ(cfg.plugins[0].path, std::string("builtin://agentxx_codegraph"));
        XX_TEST_EXPECT_TRUE(cfg.plugins[0].enabled);
        XX_TEST_EXPECT_TRUE(cfg.plugins[0].args.is_object());
    }
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
    // builtin:// 显式前缀等价于 name 简写
    cfg = loadYaml(R"(plugins:
  - path: builtin://agentxx_codegraph
    enabled: true
)");
    XX_TEST_EXPECT_EQ(cfg.plugins.size(), size_t{1});
    if (cfg.plugins.size() == 1) {
        XX_TEST_EXPECT_EQ(cfg.plugins[0].path, std::string("builtin://agentxx_codegraph"));
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

void test_plugins_config_path_parse() {
    // config: 可指向文件或目录, 支持 ${VAR} 展开, 宿主原样保存 (归一化由装配侧完成)
    auto cfg = loadYaml(R"(plugins:
  - path: "/opt/plugins/my_plugin.so"
    config: "/etc/my_plugin/config.yaml"
  - path: builtin://agentxx_filesystem
    config: "./relative/config_dir"
  - path: "/opt/plugins/no_config.so"
)");
    XX_TEST_EXPECT_EQ(cfg.plugins.size(), size_t{3});
    if (cfg.plugins.size() == 3) {
        XX_TEST_EXPECT_EQ(cfg.plugins[0].configPath, std::string("/etc/my_plugin/config.yaml"));
        XX_TEST_EXPECT_EQ(cfg.plugins[1].configPath, std::string("./relative/config_dir"));
        XX_TEST_EXPECT_TRUE(cfg.plugins[2].configPath.empty());
    }
    // config 支持 ${VAR} 展开
    auto path = fs::temp_directory_path()
                / fmt::format(
                    "agentxx_config_loader_test_{}.yaml",
                    std::chrono::steady_clock::now().time_since_epoch().count()
                );
    {
        std::ofstream ofs(path);
        ofs << R"(plugins:
  - path: "/opt/plugins/my_plugin.so"
    config: "${AGENTXX_TEST_PLUGIN_CONFIG}/conf"
)";
    }
    auto cfg2 = agentxx::client::loadYamlConfig(
        path.string(),
        {
            {"AGENTXX_TEST_PLUGIN_CONFIG", "/data/plugin_cfg"}
    },
        {}
    );
    std::error_code ec;
    fs::remove(path, ec);
    XX_TEST_EXPECT_EQ(cfg2.plugins.size(), size_t{1});
    if (cfg2.plugins.size() == 1) {
        XX_TEST_EXPECT_EQ(cfg2.plugins[0].configPath, std::string("/data/plugin_cfg/conf"));
    }
    // name 简写 + config 组合
    cfg = loadYaml(R"(plugins:
  - name: agentxx_planning
    config: "/tmp/planning_config"
)");
    XX_TEST_EXPECT_EQ(cfg.plugins.size(), size_t{1});
    if (cfg.plugins.size() == 1) {
        XX_TEST_EXPECT_EQ(cfg.plugins[0].path, std::string("builtin://agentxx_planning"));
        XX_TEST_EXPECT_EQ(cfg.plugins[0].configPath, std::string("/tmp/planning_config"));
    }
}

// ---------------------------------------------------------------------------
// 程序内置环境变量 (yaml ${VAR} 展开: AGENTXX_WORK_DIR = 程序启动后的工作目录)
// ---------------------------------------------------------------------------

void test_builtin_work_dir_default() {
    // 未注入时惰性回退当前工作目录 (正斜杠)
    agentxx::client::setBuiltinEnvVar(agentxx::client::kBuiltinWorkDirEnv, "");
    auto cfg      = loadYaml("data_dir: ${AGENTXX_WORK_DIR}/agentxx-data\n");
    auto expected = (std::filesystem::current_path() / "agentxx-data").generic_string();
    XX_TEST_EXPECT_EQ(cfg.dataDir, expected);
}

void test_builtin_work_dir_inject() {
    // main 启动时注入的值生效
    agentxx::client::setBuiltinEnvVar(agentxx::client::kBuiltinWorkDirEnv, "C:/custom/work");
    auto cfg = loadYaml("data_dir: ${AGENTXX_WORK_DIR}/agentxx-data\n");
    XX_TEST_EXPECT_EQ(cfg.dataDir, std::string("C:/custom/work/agentxx-data"));
}

void test_builtin_work_dir_priority() {
    // 内置变量优先于 .env (dotEnvVars) / 系统环境变量: 同名 .env 值不生效
    agentxx::client::setBuiltinEnvVar(agentxx::client::kBuiltinWorkDirEnv, "C:/builtin/work");
    auto path = fs::temp_directory_path()
                / fmt::format(
                    "agentxx_config_loader_test_{}.yaml",
                    std::chrono::steady_clock::now().time_since_epoch().count()
                );
    {
        std::ofstream ofs(path);
        ofs << "data_dir: ${AGENTXX_WORK_DIR}/data\n";
    }
    auto cfg = agentxx::client::loadYamlConfig(
        path.string(),
        {
            {"AGENTXX_WORK_DIR", "C:/from/dotenv"}
    },
        {}
    );
    XX_TEST_EXPECT_EQ(cfg.dataDir, std::string("C:/builtin/work/data"));

    // 清除注入 (空值) 后回退惰性解析: 内置变量仍优先于 .env (值 = 当前工作目录)
    agentxx::client::setBuiltinEnvVar(agentxx::client::kBuiltinWorkDirEnv, "");
    cfg = agentxx::client::loadYamlConfig(
        path.string(),
        {
            {"AGENTXX_WORK_DIR", "C:/from/dotenv"}
    },
        {}
    );
    XX_TEST_EXPECT_EQ(cfg.dataDir, (std::filesystem::current_path() / "data").generic_string());

    std::error_code ec;
    fs::remove(path, ec);
}

void test_builtin_exec_dir_inject() {
    // main 注入的可执行目录生效 (与 AGENTXX_WORK_DIR 独立)
    agentxx::client::setBuiltinEnvVar(agentxx::client::kBuiltinExecDirEnv, "C:/tools/agentxx/bin");
    auto cfg = loadYaml("data_dir: ${AGENTXX_EXEC_DIR}/data\n");
    XX_TEST_EXPECT_EQ(cfg.dataDir, std::string("C:/tools/agentxx/bin/data"));
    // 清理注入, 避免影响后续测试
    agentxx::client::setBuiltinEnvVar(agentxx::client::kBuiltinExecDirEnv, "");
}

void test_builtin_exec_dir_uninjected_kept() {
    // 未注入且无系统/.env 变量: 保留 ${AGENTXX_EXEC_DIR} 原样 (可执行目录无法惰性推导)
    agentxx::client::setBuiltinEnvVar(agentxx::client::kBuiltinExecDirEnv, "");
    clearSystemEnvVar("AGENTXX_EXEC_DIR");
    auto        cfg    = loadYaml("data_dir: ${AGENTXX_EXEC_DIR}/data\n");
    auto        curOpt = agentxx::util::ApplicationEnv::instance().getSystem("AGENTXX_EXEC_DIR");
    const char* cur    = curOpt ? curOpt->c_str() : nullptr;
    if (cur == nullptr) {
        // 变量被真正删除: 保留 ${VAR} 原样
        XX_TEST_EXPECT_TRUE(cfg.dataDir.find("${AGENTXX_EXEC_DIR}") != std::string::npos);
    } else if (*cur == '\0') {
        // 平台将变量置为空串: 展开结果为空串 (空串视为未定义)
        XX_TEST_EXPECT_EQ(cfg.dataDir, std::string("/data"));
    } else {
        // 变量意外存在 (测试环境脏): 展开行为与真实环境一致
        XX_TEST_EXPECT_EQ(cfg.dataDir, std::string(cur) + "/data");
    }
}

// ---------------------------------------------------------------------------
// 环境变量查找顺序: 程序内置变量 > --env 覆盖文件 > .env 文件 > 系统环境变量 > 保留 ${VAR} 原样
// ---------------------------------------------------------------------------

void test_env_order_dotenv_over_system() {
    // .env 变量优先于系统环境变量 (同 key 时取 .env 值)
    SystemEnvGuard guard{"AGENTXX_TEST_ENV_ORDER", "from-system"};
    auto           cfg = loadYamlWithDotEnv(
        "data_dir: ${AGENTXX_TEST_ENV_ORDER}/data\n",
        {
            {"AGENTXX_TEST_ENV_ORDER", "from-dotenv"}
    }
    );
    XX_TEST_EXPECT_EQ(cfg.dataDir, std::string("from-dotenv/data"));
}

void test_env_order_system_fallback() {
    // .env 未定义、系统环境变量有值: 取系统值
    SystemEnvGuard guard{"AGENTXX_TEST_ENV_ORDER", "from-system"};
    auto           cfg = loadYaml("data_dir: ${AGENTXX_TEST_ENV_ORDER}/data\n");
    XX_TEST_EXPECT_EQ(cfg.dataDir, std::string("from-system/data"));
}

void test_env_order_override_highest() {
    // --env 覆盖式文件 (overrideEnvVars) 优先于 .env 与系统环境变量
    SystemEnvGuard guard{"AGENTXX_TEST_ENV_ORDER", "from-system"};
    auto           path = fs::temp_directory_path()
                / fmt::format(
                    "agentxx_config_loader_test_{}.yaml",
                    std::chrono::steady_clock::now().time_since_epoch().count()
                );
    {
        std::ofstream ofs(path);
        ofs << "data_dir: ${AGENTXX_TEST_ENV_ORDER}/data\n";
    }
    auto cfg = agentxx::client::loadYamlConfig(
        path.string(),
        {
            {"AGENTXX_TEST_ENV_ORDER", "from-dotenv"}
    },
        {{"AGENTXX_TEST_ENV_ORDER", "from-override"}}
    );
    std::error_code ec;
    fs::remove(path, ec);
    XX_TEST_EXPECT_EQ(cfg.dataDir, std::string("from-override/data"));
}

void test_env_order_unresolved_kept() {
    // 内置/.env/系统均未定义: 保留 ${VAR} 原样
    // 注: 部分平台 (Windows _putenv_s) 清除变量时可能置为空串而非删除,
    // 空串同样视为"未定义"(展开为空串); 两种情况分别断言
    const char* key = "AGENTXX_TEST_ENV_MISSING_9F3K2Q";
    clearSystemEnvVar(key);
    auto        cfg    = loadYaml("data_dir: ${AGENTXX_TEST_ENV_MISSING_9F3K2Q}/data\n");
    auto        curOpt = agentxx::util::ApplicationEnv::instance().getSystem(key);
    const char* cur    = curOpt ? curOpt->c_str() : nullptr;
    if (cur == nullptr) {
        // 变量被真正删除: 保留 ${VAR} 原样
        XX_TEST_EXPECT_TRUE(
            cfg.dataDir.find("${AGENTXX_TEST_ENV_MISSING_9F3K2Q}") != std::string::npos
        );
    } else if (*cur == '\0') {
        // 平台将变量置为空串: 展开结果为空串 (空串视为未定义)
        XX_TEST_EXPECT_EQ(cfg.dataDir, std::string("/data"));
    } else {
        // 变量意外存在 (测试环境脏): 展开行为与真实环境一致
        XX_TEST_EXPECT_EQ(cfg.dataDir, std::string(cur) + "/data");
    }
}

void test_dotenv_file_over_system() {
    // loadDotEnv 文件读取: .env 文件值直接生效, 不被系统环境变量覆盖
    SystemEnvGuard guard{"AGENTXX_TEST_ENV_FILE", "from-system"};
    auto           path = fs::temp_directory_path()
                / fmt::format(
                    "agentxx_config_loader_test_{}.env",
                    std::chrono::steady_clock::now().time_since_epoch().count()
                );
    {
        std::ofstream ofs(path);
        ofs << "AGENTXX_TEST_ENV_FILE=from-file\n";
    }
    auto            vars = agentxx::client::loadDotEnv(path.string());
    std::error_code ec;
    fs::remove(path, ec);
    auto it = vars.find("AGENTXX_TEST_ENV_FILE");
    XX_TEST_EXPECT_TRUE(it != vars.end());
    if (it != vars.end()) {
        XX_TEST_EXPECT_EQ(it->second, std::string("from-file"));
    }
}

void test_subagent_enable_default_true();
void test_subagent_enable_false();
void test_subagent_enable_true_variants();
void test_subagent_enable_invalid_fallback();
void test_subagent_enable_env_expand();
void test_plugins_config_path_parse();

void test_subagent_enable_default_true() {
    auto cfg = loadYaml("data_dir: default\n");
    XX_TEST_EXPECT_TRUE(cfg.enableSubagent);
}

void test_subagent_enable_false() {
    auto cfg = loadYaml("subagent:\n  enable: false\n");
    XX_TEST_EXPECT_FALSE(cfg.enableSubagent);
    cfg = loadYaml("subagent:\n  enable: '0'\n");
    XX_TEST_EXPECT_FALSE(cfg.enableSubagent);
    cfg = loadYaml("subagent:\n  enable: 'off'\n");
    XX_TEST_EXPECT_FALSE(cfg.enableSubagent);
}

void test_subagent_enable_true_variants() {
    auto cfg = loadYaml("subagent:\n  enable: true\n");
    XX_TEST_EXPECT_TRUE(cfg.enableSubagent);
    cfg = loadYaml("subagent:\n  enable: '1'\n");
    XX_TEST_EXPECT_TRUE(cfg.enableSubagent);
    cfg = loadYaml("subagent:\n  enable: 'yes'\n");
    XX_TEST_EXPECT_TRUE(cfg.enableSubagent);
    cfg = loadYaml("subagent:\n  enable: 'on'\n");
    XX_TEST_EXPECT_TRUE(cfg.enableSubagent);
}

void test_subagent_enable_invalid_fallback() {
    auto cfg = loadYaml("subagent:\n  enable: 'maybe'\n");
    XX_TEST_EXPECT_TRUE(cfg.enableSubagent);
}

void test_subagent_enable_env_expand() {
    auto path = fs::temp_directory_path()
                / fmt::format(
                    "agentxx_subagent_test_{}.yaml",
                    std::chrono::steady_clock::now().time_since_epoch().count()
                );
    {
        std::ofstream ofs(path);
        ofs << "subagent:\n  enable: ${AGENTXX_TEST_SUBAGENT_ENABLE}\n";
    }
    auto cfg = agentxx::client::loadYamlConfig(
        path.string(),
        {
            {"AGENTXX_TEST_SUBAGENT_ENABLE", "false"}
    },
        {}
    );
    XX_TEST_EXPECT_FALSE(cfg.enableSubagent);
    std::error_code ec;
    fs::remove(path, ec);
}

void test_language_config() {
    // 1. 默认未指定: "en"
    {
        auto cfg = loadYaml("data_dir: default\n");
        XX_TEST_EXPECT_EQ(cfg.language, std::string("en"));
    }
    // 2. 显式指定 "zh-cn"
    {
        auto cfg = loadYaml("language: zh-cn\n");
        XX_TEST_EXPECT_EQ(cfg.language, std::string("zh-cn"));
    }
    // 3. 显式指定 "en"
    {
        auto cfg = loadYaml("language: en\n");
        XX_TEST_EXPECT_EQ(cfg.language, std::string("en"));
    }
    // 4. 不支持 auto: 指定 "auto" 回退为 "en"
    {
        auto cfg = loadYaml("language: auto\n");
        XX_TEST_EXPECT_EQ(cfg.language, std::string("en"));
    }
    {
        auto cfg = loadYaml("language: AUTO\n");
        XX_TEST_EXPECT_EQ(cfg.language, std::string("en"));
    }
    // 5. 空字符串回退为 "en"
    {
        auto cfg = loadYaml("language: \"\"\n");
        XX_TEST_EXPECT_EQ(cfg.language, std::string("en"));
    }
    // 6. 支持环境变量展开
    {
        auto cfg = loadYamlWithDotEnv("language: ${MY_LANG}\n", {{"MY_LANG", "zh-cn"}});
        XX_TEST_EXPECT_EQ(cfg.language, std::string("zh-cn"));
    }
}

TestResult testConfigLoader() {
    g_config_loader_passed = 0;
    g_config_loader_failed = 0;

    test_language_config();
    test_permission_mode_default_ask();
    test_permission_mode_ask();
    test_permission_mode_all_ask();
    test_permission_mode_pass();
    test_permission_mode_deny();
    test_permission_mode_case_insensitive();
    test_permission_mode_invalid_fallback();
    test_permission_mode_env_expand();
    test_builtin_work_dir_default();
    test_builtin_work_dir_inject();
    test_builtin_work_dir_priority();
    test_builtin_exec_dir_inject();
    test_builtin_exec_dir_uninjected_kept();
    test_env_order_dotenv_over_system();
    test_env_order_system_fallback();
    test_env_order_override_highest();
    test_env_order_unresolved_kept();
    test_dotenv_file_over_system();
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
    test_model_request_reasoning_summary();
    test_plugins_empty_by_default();
    test_plugin_name_form_removed();
    test_plugin_args_paths_parse();
    test_plugin_missing_path_skipped();
    test_plugin_args_env_expand();
    test_plugins_config_path_parse();
    test_subagent_enable_default_true();
    test_subagent_enable_false();
    test_subagent_enable_true_variants();
    test_subagent_enable_invalid_fallback();
    test_subagent_enable_env_expand();

    return TestResult{g_config_loader_passed, g_config_loader_failed};
}

} // namespace test
} // namespace agentxx
