#include "test_config_loader.h"

#include "agentxx-client/config_loader.h"
#include "agentxx/agent/config_static.h"
#include "agentxx/util/env.h"
#include "agentxx/util/http_client.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <stdlib.h>
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

/// 设置/删除**系统**环境变量 (与 test_client_plugins 的辅助函数一致)
/// - 不能用 ApplicationEnv::instance().set(): 那是进程内预设变量通道
///   (内置变量通道, 优先级高于 .env/--env), 用它无法验证
///   ".env 优先于系统环境变量" 的查找顺序
/// - 值传空串表示删除该变量 (Windows _putenv_s 与 POSIX unsetenv 均如此约定)
static void setSystemEnvVarValue(const std::string& key, const std::string& value) {
#if XX_IS_WIN_D
    _putenv_s(key.c_str(), value.c_str());
#else
    if (value.empty()) {
        ::unsetenv(key.c_str());
    } else {
        ::setenv(key.c_str(), value.c_str(), 1);
    }
#endif
}

/// RAII: 设置系统环境变量, 析构时恢复原值/删除
class SystemEnvGuard {
public:

    SystemEnvGuard(const std::string& key, const std::string& value) :
        key_(key) {
        auto hadOpt = agentxx::util::ApplicationEnv::instance().getSystem(key_);
        existed_    = hadOpt.has_value();
        saved_      = hadOpt ? *hadOpt : std::string{};
        setSystemEnvVarValue(key_, value);
    }

    ~SystemEnvGuard() {
        setSystemEnvVarValue(key_, existed_ ? saved_ : std::string{});
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
    list:
      - "C:/work/trusted"
      - "/home/user/shared/**"
  blacklist:
    list:
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
    list:
      - "${AGENTXX_TEST_ALLOW_DIR}/trusted"
  blacklist:
    list:
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
  list:
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
  list:
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
  list:
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
  list:
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
  list:
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
  list:
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
  list:
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
    auto cfg = loadYaml(R"(plugin:
  list:
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
    auto cfg = loadYaml(R"(plugin:
  list:
    - enabled: true
    - 123
    - path: "/opt/plugins/ok.so"
)");
    XX_TEST_EXPECT_EQ(cfg.plugins.size(), size_t{1});
    if (cfg.plugins.size() == 1) {
        XX_TEST_EXPECT_EQ(cfg.plugins[0].path, std::string("/opt/plugins/ok.so"));
    }
    // 全注释的 plugins 段 (null): 解析为空列表
    cfg = loadYaml("plugin:\n  list:\n    # - path: \"/opt/plugins/x.so\"\n");
    XX_TEST_EXPECT_TRUE(cfg.plugins.empty());
    // 未配置 plugins 段
    cfg = loadYaml("data_dir: default\n");
    XX_TEST_EXPECT_TRUE(cfg.plugins.empty());
}

void test_plugins_env_expand() {
    // path 支持 ${VAR} 环境变量展开
    auto cfg = loadYaml("plugin:\n  list:\n    - path: \"${PLUGIN_DIR}/my_plugin.so\"\n");
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
    auto cfg = loadYaml(R"(model:
  list:
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
    cfg = loadYaml(R"(model:
  list:
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
    cfg = loadYaml(R"(model:
  list:
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
    cfg = loadYaml(R"(model:
  list:
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
    auto cfg = loadYaml("model:\n  list:\n    - name: m1\n      type: \"openai\"\n");
    auto it  = cfg.models.find("m1");
    XX_TEST_EXPECT_TRUE(it != cfg.models.end());
    if (it != cfg.models.end()) {
        XX_TEST_EXPECT_TRUE(it->second.requestReasoningSummary);
    }

    // send_thinking: true + 显式 request_reasoning_summary: false (opencode-muse-spark 场景)
    cfg = loadYaml(R"(model:
  list:
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
    cfg = loadYaml(R"(model:
  list:
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
// 多模态输入能力 (yaml `models[].image_input/audio_input/video_input`, 默认 false)
// ---------------------------------------------------------------------------

void test_model_multimodal_input() {
    // 未配置: 默认全 false, hasMultimodalInput() == false (向后兼容纯文本)
    auto cfg = loadYaml("model:\n  list:\n    - name: m1\n      type: \"openai\"\n");
    auto it  = cfg.models.find("m1");
    XX_TEST_EXPECT_TRUE(it != cfg.models.end());
    if (it != cfg.models.end()) {
        XX_TEST_EXPECT_FALSE(it->second.imageInput);
        XX_TEST_EXPECT_FALSE(it->second.audioInput);
        XX_TEST_EXPECT_FALSE(it->second.videoInput);
        XX_TEST_EXPECT_FALSE(it->second.hasMultimodalInput());
    }

    // 显式开启 image/audio
    cfg = loadYaml(R"(model:
  list:
    - name: m2
      type: "openai"
      image_input: true
      audio_input: true
      video_input: false
)");
    it  = cfg.models.find("m2");
    XX_TEST_EXPECT_TRUE(it != cfg.models.end());
    if (it != cfg.models.end()) {
        XX_TEST_EXPECT_TRUE(it->second.imageInput);
        XX_TEST_EXPECT_TRUE(it->second.audioInput);
        XX_TEST_EXPECT_FALSE(it->second.videoInput);
        XX_TEST_EXPECT_TRUE(it->second.hasMultimodalInput());
    }

    // 全开 video
    cfg = loadYaml(R"(model:
  list:
    - name: m3
      type: "openai"
      video_input: true
)");
    it  = cfg.models.find("m3");
    XX_TEST_EXPECT_TRUE(it != cfg.models.end());
    if (it != cfg.models.end()) {
        XX_TEST_EXPECT_FALSE(it->second.imageInput);
        XX_TEST_EXPECT_FALSE(it->second.audioInput);
        XX_TEST_EXPECT_TRUE(it->second.videoInput);
        XX_TEST_EXPECT_TRUE(it->second.hasMultimodalInput());
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
    auto cfg = loadYaml(R"(plugin:
  list:
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
    cfg = loadYaml(R"(plugin:
  list:
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
    cfg = loadYaml(R"(plugin:
  list:
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
    auto cfg = loadYaml(R"(plugin:
  list:
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

void test_yaml_to_json_big_integer() {
    // P3-4: 超过 32 位 int 范围的大整数精确解析为整数 (long long), 不丢失精度到 double
    auto cfg = loadYaml(R"(plugin:
  list:
    - path: "/opt/plugins/test_plugin"
      enabled: true
      args:
        big_num: 10000000000
)");
    XX_TEST_EXPECT_EQ(cfg.plugins.size(), size_t{1});
    if (cfg.plugins.size() == 1) {
        XX_TEST_EXPECT_TRUE(cfg.plugins[0].args.contains("big_num"));
        XX_TEST_EXPECT_TRUE(cfg.plugins[0].args["big_num"].is_number_integer());
        XX_TEST_EXPECT_EQ(cfg.plugins[0].args["big_num"].get<long long>(), 10000000000LL);
    }
}

void test_url_decode() {
    // P3-4: URL 解码功能验证 (支持 + 转空格, %XX 十六进制还原)
    auto decoded = agentxx::util::HttpClient::urlDecode("hello%20world%2B%2F%3D+test%21");
    XX_TEST_EXPECT_EQ(decoded, std::string("hello world+/= test!"));
}

void test_plugin_missing_path_skipped() {
    // path 缺失 (唯一必填字段): 跳过 (记警告)
    auto cfg = loadYaml("plugin:\n  list:\n    - enabled: true\n");
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
        ofs << R"(plugin:
  list:
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
    auto cfg = loadYaml(R"(plugin:
  list:
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
        ofs << R"(plugin:
  list:
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
    cfg = loadYaml(R"(plugin:
  list:
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
    agentxx::util::ApplicationEnv::instance().remove("AGENTXX_EXEC_DIR");
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
    agentxx::util::ApplicationEnv::instance().remove(key);
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

// ---------------------------------------------------------------------------
// 分层配置 (base: data_dir 目录下的配置; overlay: 工作目录/--config 指定的配置)
// ---------------------------------------------------------------------------

/// 临时目录 (构造时创建, 析构时递归清理; 供分层配置测试写入 base/overlay 文件)
class TempConfigDir {
public:

    TempConfigDir() {
        static int counter = 0;
        auto       name    = fmt::format(
            "agentxx_cfg_layer_{}_{}",
            std::chrono::steady_clock::now().time_since_epoch().count(),
            counter++
        );
        path_ = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::create_directories(path_, ec);
    }

    ~TempConfigDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    TempConfigDir(const TempConfigDir&)            = delete;
    TempConfigDir& operator=(const TempConfigDir&) = delete;

    const fs::path& path() const noexcept {
        return path_;
    }

    /// 写入文件 (relativePath 可含子目录), 返回其绝对路径 (正斜杠)
    std::string write(std::string_view relativePath, std::string_view content) const {
        auto            p = path_ / fs::path{std::string{relativePath}};
        std::error_code ec;
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path(), ec);
        }
        std::ofstream ofs(p);
        ofs << content;
        return p.generic_string();
    }

    /// 创建并返回子目录绝对路径 (正斜杠)
    std::string subDir(std::string_view name) const {
        auto            p = path_ / fs::path{std::string{name}};
        std::error_code ec;
        fs::create_directories(p, ec);
        return p.generic_string();
    }

private:

    fs::path path_;
};

void test_layered_scalar_override() {
    TempConfigDir dir;
    auto          base    = dir.write("base.yaml", R"(data_dir: /data/base
work_dir: /work/base
subagent:
  enable: false
worktree:
  enable: true
model:
  use:
    default: m1
    subagent: m1
permission:
  mode: deny
)");
    auto overlay = dir.write("overlay.yaml", R"(permission:
  mode: pass
model:
  use:
    subagent: m2
)");
    auto cfg = agentxx::client::loadYamlConfigLayered(base, overlay, {}, {});

    // overlay 未配置的标量: 保留 base 值
    XX_TEST_EXPECT_EQ(cfg.dataDir, std::string("/data/base"));
    XX_TEST_EXPECT_EQ(cfg.workDir, std::string("/work/base"));
    XX_TEST_EXPECT_FALSE(cfg.enableSubagent);
    XX_TEST_EXPECT_TRUE(cfg.worktreeEnable);
    // overlay 覆盖标量, 但映射内其他键保留 base 值 (model.use 逐键合并)
    XX_TEST_EXPECT_TRUE(cfg.permissionMode == agent::PermissionMode::Pass);
    XX_TEST_EXPECT_EQ(cfg.useModelDefault, std::string("m1"));
    XX_TEST_EXPECT_EQ(cfg.useModelSubagent, std::string("m2"));

    // 反向: base 未配置的键由 overlay 提供
    auto overlayOnly = dir.write("overlay_only.yaml", R"(model:
  use:
    default: m9
)");
    auto cfg2        = agentxx::client::loadYamlConfigLayered(base, overlayOnly, {}, {});
    XX_TEST_EXPECT_EQ(cfg2.useModelDefault, std::string("m9"));
    XX_TEST_EXPECT_EQ(cfg2.useModelSubagent, std::string("m1"));
    XX_TEST_EXPECT_TRUE(cfg2.permissionMode == agent::PermissionMode::Deny);
}

void test_layered_models_merge() {
    TempConfigDir dir;
    auto          base    = dir.write("base.yaml", R"(model:
  list:
    - name: shared
      type: openai
      base_url: https://base.example.com
      api_key: base-key
      extra_headers:
        x-base: "1"
    - name: base-only
      type: openai
      base_url: https://base2.example.com
)");
    auto overlay = dir.write("overlay.yaml", R"(model:
  list:
    - name: shared
      model_name: shared-v2
      extra_headers:
        x-overlay: "2"
    - name: overlay-only
      type: anthropic
)");
    auto cfg = agentxx::client::loadYamlConfigLayered(base, overlay, {}, {});

    XX_TEST_EXPECT_EQ(cfg.models.size(), size_t{3});
    auto shared = cfg.models.find("shared");
    XX_TEST_EXPECT_TRUE(shared != cfg.models.end());
    if (shared != cfg.models.end()) {
        // 同名字段级合并: overlay 未配置的字段保留 base 值
        XX_TEST_EXPECT_EQ(shared->second.baseUrl, std::string("https://base.example.com"));
        XX_TEST_EXPECT_EQ(shared->second.apiKey, std::string("base-key"));
        XX_TEST_EXPECT_EQ(shared->second.type, std::string("openai"));
        XX_TEST_EXPECT_EQ(shared->second.modelName, std::string("shared-v2"));
        // extra_headers 映射逐键合并
        XX_TEST_EXPECT_EQ(shared->second.extraHeaders.size(), size_t{2});
        XX_TEST_EXPECT_EQ(shared->second.extraHeaders["x-base"], std::string("1"));
        XX_TEST_EXPECT_EQ(shared->second.extraHeaders["x-overlay"], std::string("2"));
    }
    XX_TEST_EXPECT_TRUE(cfg.models.contains("base-only"));
    XX_TEST_EXPECT_TRUE(cfg.models.contains("overlay-only"));
}

void test_layered_mcp_and_plugins_merge() {
    TempConfigDir dir;
    auto          base    = dir.write("base.yaml", R"(mcp:
  list:
    - namespace: ns1
      url: "http://base/mcp"
      timeout: 30
    - namespace: ns2
      url: "http://base2/mcp"
plugin:
  list:
    - path: "/plugins/p1"
      enabled: true
      args:
        a: 1
        b: 2
    - path: "builtin://p2"
      enabled: true
      args:
        k: v
)");
    auto overlay = dir.write("overlay.yaml", R"(mcp:
  list:
    - namespace: ns1
      url: "http://overlay/mcp"
plugin:
  list:
    - path: "/plugins/p1"
      enabled: false
      args:
        b: 3
        c: 4
)");
    auto cfg = agentxx::client::loadYamlConfigLayered(base, overlay, {}, {});

    // mcp 按 namespace 归并: url 覆盖, 未配置的 timeout 保留 base 值
    XX_TEST_EXPECT_EQ(cfg.mcpServers.size(), size_t{2});
    auto ns1 = cfg.mcpServers.find("ns1");
    XX_TEST_EXPECT_TRUE(ns1 != cfg.mcpServers.end());
    if (ns1 != cfg.mcpServers.end()) {
        XX_TEST_EXPECT_EQ(ns1->second.url, std::string("http://overlay/mcp"));
        XX_TEST_EXPECT_EQ(ns1->second.toolTimeout.count(), 30 * 1000);
    }
    auto ns2 = cfg.mcpServers.find("ns2");
    XX_TEST_EXPECT_TRUE(ns2 != cfg.mcpServers.end());
    if (ns2 != cfg.mcpServers.end()) {
        XX_TEST_EXPECT_EQ(ns2->second.url, std::string("http://base2/mcp"));
    }

    // plugins 按 path 归并: enabled 覆盖 (可用于禁用 base 插件), args 逐键合并
    XX_TEST_EXPECT_EQ(cfg.plugins.size(), size_t{2});
    if (cfg.plugins.size() == 2) {
        XX_TEST_EXPECT_EQ(cfg.plugins[0].path, std::string("/plugins/p1")); // base 顺序保持
        XX_TEST_EXPECT_FALSE(cfg.plugins[0].enabled);
        XX_TEST_EXPECT_TRUE(cfg.plugins[0].args.is_object());
        if (cfg.plugins[0].args.is_object()) {
            XX_TEST_EXPECT_EQ(cfg.plugins[0].args["a"].get<int>(), 1);
            XX_TEST_EXPECT_EQ(cfg.plugins[0].args["b"].get<int>(), 3);
            XX_TEST_EXPECT_EQ(cfg.plugins[0].args["c"].get<int>(), 4);
        }
        XX_TEST_EXPECT_EQ(cfg.plugins[1].path, std::string("builtin://p2"));
        XX_TEST_EXPECT_TRUE(cfg.plugins[1].enabled);
    }
}

void test_layered_path_lists_append() {
    TempConfigDir dir;
    auto          base    = dir.write("base.yaml", R"(skill:
  list:
    - /skills/s1
    - /skills/s2
memory:
  list:
    - /mem/m1
permission:
  mode: ask
  whitelist:
    list:
      - /allow/a
  blacklist:
    list:
      - /deny/x
)");
    auto overlay = dir.write("overlay.yaml", R"(skill:
  list:
    - /skills/s2
    - /skills/s3
permission:
  whitelist:
    list:
      - /allow/b
)");
    auto cfg = agentxx::client::loadYamlConfigLayered(base, overlay, {}, {});

    // 路径列表追加合并 (base 在前, overlay 去重后追加)
    XX_TEST_EXPECT_EQ(cfg.skillDirPaths.size(), size_t{3});
    if (cfg.skillDirPaths.size() == 3) {
        XX_TEST_EXPECT_EQ(cfg.skillDirPaths[0], std::string("/skills/s1"));
        XX_TEST_EXPECT_EQ(cfg.skillDirPaths[1], std::string("/skills/s2"));
        XX_TEST_EXPECT_EQ(cfg.skillDirPaths[2], std::string("/skills/s3"));
    }
    XX_TEST_EXPECT_EQ(cfg.memoryFilePaths.size(), size_t{1});
    XX_TEST_EXPECT_EQ(cfg.permissionAllowPaths.size(), size_t{2});
    if (cfg.permissionAllowPaths.size() == 2) {
        XX_TEST_EXPECT_EQ(cfg.permissionAllowPaths[0], std::string("/allow/a"));
        XX_TEST_EXPECT_EQ(cfg.permissionAllowPaths[1], std::string("/allow/b"));
    }
    // 未在 overlay 出现的黑名单保留 base 值
    XX_TEST_EXPECT_EQ(cfg.permissionDenyPaths.size(), size_t{1});
}

void test_layered_plugin_args_list_replaced() {
    TempConfigDir dir;
    auto          base    = dir.write("base.yaml", R"(plugin:
  list:
    - path: "builtin://agentxx_codegraph"
      args:
        use_gitignore: false
        paths:
          - /proj/base_a
)");
    auto overlay = dir.write("overlay.yaml", R"(plugin:
  list:
    - path: "builtin://agentxx_codegraph"
      args:
        paths:
          - /proj/overlay_b
)");
    auto cfg = agentxx::client::loadYamlConfigLayered(base, overlay, {}, {});

    XX_TEST_EXPECT_EQ(cfg.plugins.size(), size_t{1});
    if (cfg.plugins.size() == 1) {
        auto& args = cfg.plugins[0].args;
        XX_TEST_EXPECT_TRUE(args.is_object());
        // 映射内的列表整体覆盖 (不追加)
        XX_TEST_EXPECT_TRUE(args.contains("paths"));
        if (args.contains("paths")) {
            XX_TEST_EXPECT_EQ(args["paths"].size(), size_t{1});
            if (args["paths"].size() == 1) {
                XX_TEST_EXPECT_EQ(args["paths"][0].get<std::string>(), std::string("/proj/overlay_b"));
            }
        }
        // 映射内其他键保留 base 值
        XX_TEST_EXPECT_TRUE(args.contains("use_gitignore"));
        if (args.contains("use_gitignore")) {
            XX_TEST_EXPECT_FALSE(args["use_gitignore"].get<bool>());
        }
    }
}

void test_layered_null_value_keeps_base() {
    // 显式空值 (null) 视为该层未配置: 不覆盖 base
    TempConfigDir dir;
    auto          base    = dir.write("base.yaml", R"(data_dir: /data/base
skill:
  list:
    - /skills/s1
)");
    auto overlay = dir.write("overlay.yaml", R"(data_dir:
skill:
  list:
)");
    auto cfg = agentxx::client::loadYamlConfigLayered(base, overlay, {}, {});
    XX_TEST_EXPECT_EQ(cfg.dataDir, std::string("/data/base"));
    XX_TEST_EXPECT_EQ(cfg.skillDirPaths.size(), size_t{1});

    // 旧写法 (段值直接给列表 / 给空字符串) 已不再支持: 记警告并忽略该段, base 项保留
    auto overlayLegacy = dir.write("overlay_legacy.yaml", R"(skill:
  - /skills/s9
)");
    auto cfgLegacy = agentxx::client::loadYamlConfigLayered(base, overlayLegacy, {}, {});
    XX_TEST_EXPECT_EQ(cfgLegacy.skillDirPaths.size(), size_t{1});
    if (cfgLegacy.skillDirPaths.size() == 1) {
        XX_TEST_EXPECT_EQ(cfgLegacy.skillDirPaths[0], std::string("/skills/s1"));
    }
    auto overlayScalar = dir.write("overlay_scalar.yaml", "skill: \"\"\n");
    auto cfgScalar     = agentxx::client::loadYamlConfigLayered(base, overlayScalar, {}, {});
    XX_TEST_EXPECT_EQ(cfgScalar.skillDirPaths.size(), size_t{1});

    // 标量仍按覆盖处理: `data_dir: ""` 清空 base 值
    auto overlayDataDir = dir.write("overlay_datadir.yaml", "data_dir: \"\"\n");
    auto cfgDataDir
        = agentxx::client::loadYamlConfigLayered(base, overlayDataDir, {}, {});
    XX_TEST_EXPECT_TRUE(cfgDataDir.dataDir.empty());

    // 清空列表段: `overwrite.mode: replace` + 无 list (= 空列表)
    auto overlayClear = dir.write("overlay_clear.yaml", R"(skill:
  overwrite:
    mode: replace
)");
    auto cfgClear = agentxx::client::loadYamlConfigLayered(base, overlayClear, {}, {});
    XX_TEST_EXPECT_TRUE(cfgClear.skillDirPaths.empty());
}

/// 构造分层目录: {tmp}/data (base 层: agentxx-config.yaml + .env),
/// {tmp}/proj (overlay 层: agentxx-config.yaml + .env)
void test_layered_config_with_base_env() {
    TempConfigDir dir;
    const auto    dataDir = dir.subDir("data");
    const auto    projDir = dir.subDir("proj");

    dir.write("data/.env", "BASE_ONLY=from-base\nBOTH=base-value\n");
    dir.write(
        "data/agentxx-config.yaml",
        R"(skill:
  list:
    - /skills/from-base
model:
  list:
    - name: base-model
      type: openai
      base_url: "https://base.example.com"
      api_key: ${BOTH}
)"
    );
    const auto overlayPath = dir.write(
        "proj/agentxx-config.yaml",
        fmt::format(
            R"(data_dir: "{}"
skill:
  list:
    - /skills/from-overlay
model:
  list:
    - name: overlay-model
      type: openai
      base_url: ${{BASE_ONLY}}
      api_key: ${{BOTH}}
)",
            dataDir
        )
    );
    dir.write("proj/.env", "BOTH=overlay-value\nOVERLAY_ONLY=from-overlay\n");

    agentxx::client::LayeredConfigOptions opts;
    opts.overlayConfigPath = overlayPath;
    opts.overlayEnvPaths.push_back((fs::path{projDir} / ".env").generic_string());

    auto loaded = agentxx::client::loadLayeredConfig(opts);

    XX_TEST_EXPECT_TRUE(loaded.overlayLoaded);
    XX_TEST_EXPECT_TRUE(loaded.baseLoaded);
    XX_TEST_EXPECT_EQ(loaded.baseDir, dataDir);
    XX_TEST_EXPECT_EQ(loaded.baseConfigPath, (fs::path{dataDir} / "agentxx-config.yaml").generic_string());

    // base .env: BOTH 被 overlay 同名变量舍弃, BASE_ONLY 并入
    XX_TEST_EXPECT_EQ(loaded.baseEnvTotal, size_t{2});
    XX_TEST_EXPECT_EQ(loaded.baseEnvDropped, size_t{1});
    XX_TEST_EXPECT_EQ(loaded.dotEnvVars.size(), size_t{3});
    auto both = loaded.dotEnvVars.find("BOTH");
    XX_TEST_EXPECT_TRUE(both != loaded.dotEnvVars.end());
    if (both != loaded.dotEnvVars.end()) {
        XX_TEST_EXPECT_EQ(both->second, std::string("overlay-value"));
    }
    XX_TEST_EXPECT_TRUE(loaded.dotEnvVars.contains("BASE_ONLY"));
    XX_TEST_EXPECT_TRUE(loaded.dotEnvVars.contains("OVERLAY_ONLY"));

    // 合并后的环境变量同时用于展开两层 yaml 的 ${VAR}
    XX_TEST_EXPECT_EQ(loaded.cfg.models.size(), size_t{2});
    auto baseModel = loaded.cfg.models.find("base-model");
    XX_TEST_EXPECT_TRUE(baseModel != loaded.cfg.models.end());
    if (baseModel != loaded.cfg.models.end()) {
        // base 配置引用 overlay .env 变量: 取 overlay 值
        XX_TEST_EXPECT_EQ(baseModel->second.apiKey, std::string("overlay-value"));
        XX_TEST_EXPECT_EQ(baseModel->second.baseUrl, std::string("https://base.example.com"));
    }
    auto overlayModel = loaded.cfg.models.find("overlay-model");
    XX_TEST_EXPECT_TRUE(overlayModel != loaded.cfg.models.end());
    if (overlayModel != loaded.cfg.models.end()) {
        // overlay 配置引用 base .env 变量: 取 base 值 (overlay 未定义该变量)
        XX_TEST_EXPECT_EQ(overlayModel->second.baseUrl, std::string("from-base"));
        XX_TEST_EXPECT_EQ(overlayModel->second.apiKey, std::string("overlay-value"));
    }
    // 路径列表追加合并: base 项在前, overlay 新增项在后
    XX_TEST_EXPECT_EQ(loaded.cfg.skillDirPaths.size(), size_t{2});
    if (loaded.cfg.skillDirPaths.size() == 2) {
        XX_TEST_EXPECT_EQ(loaded.cfg.skillDirPaths[0], std::string("/skills/from-base"));
        XX_TEST_EXPECT_EQ(loaded.cfg.skillDirPaths[1], std::string("/skills/from-overlay"));
    }
}

void test_layered_base_missing() {
    // base 目录下没有配置文件: 只用 overlay (base .env 若存在仍并入)
    TempConfigDir dir;
    const auto    dataDir = dir.subDir("data");
    dir.write("data/.env", "BASE_ONLY=from-base\n");

    const auto overlayPath = dir.write(
        "proj/agentxx-config.yaml",
        fmt::format("data_dir: \"{}\"\nskill:\n  list:\n    - /skills/s1\n", dataDir)
    );

    agentxx::client::LayeredConfigOptions opts;
    opts.overlayConfigPath = overlayPath;
    opts.overlayEnvPaths.push_back((dir.path() / "proj" / ".env").generic_string());

    auto loaded = agentxx::client::loadLayeredConfig(opts);
    XX_TEST_EXPECT_TRUE(loaded.overlayLoaded);
    XX_TEST_EXPECT_FALSE(loaded.baseLoaded);
    XX_TEST_EXPECT_EQ(loaded.baseDir, dataDir);
    XX_TEST_EXPECT_EQ(loaded.baseEnvTotal, size_t{1});
    XX_TEST_EXPECT_EQ(loaded.baseEnvDropped, size_t{0});
    XX_TEST_EXPECT_TRUE(loaded.dotEnvVars.contains("BASE_ONLY"));
    XX_TEST_EXPECT_EQ(loaded.cfg.skillDirPaths.size(), size_t{1});
}

void test_layered_overlay_equals_base() {
    // overlay 配置即 data_dir 目录下的配置 (文件名与 base 相同): 只加载一次, 不重复合并
    TempConfigDir dir;
    const auto    dirPath = dir.path().generic_string();
    const auto    overlayPath = dir.write(
        "agentxx-config.yaml",
        fmt::format("data_dir: \"{}\"\nskill:\n  list:\n    - /skills/s1\n", dirPath)
    );
    dir.write(".env", "ONLY=1\n");

    agentxx::client::LayeredConfigOptions opts;
    opts.overlayConfigPath = overlayPath;
    opts.overlayEnvPaths.push_back((dir.path() / ".env").generic_string());

    auto loaded = agentxx::client::loadLayeredConfig(opts);
    XX_TEST_EXPECT_TRUE(loaded.overlayLoaded);
    XX_TEST_EXPECT_FALSE(loaded.baseLoaded); // base 与 overlay 同一文件: 跳过 base 层
    // 列表项不因"同文件当两层"而重复
    XX_TEST_EXPECT_EQ(loaded.cfg.skillDirPaths.size(), size_t{1});
    XX_TEST_EXPECT_EQ(loaded.cfg.dataDir, dirPath);
    // base .env 与 overlay .env 为同一文件: 只加载一次, 不产生被舍弃计数
    XX_TEST_EXPECT_EQ(loaded.baseEnvTotal, size_t{0});
    XX_TEST_EXPECT_EQ(loaded.baseEnvDropped, size_t{0});
    XX_TEST_EXPECT_EQ(loaded.dotEnvVars.size(), size_t{1});
}

void test_layered_base_dir_defaults_to_system_dir() {
    // overlay 未配置 data_dir: base 目录取系统数据目录 (工作目录无配置时
    // 直接加载数据目录下的配置); 此处只断言目录定位, 不依赖真实数据目录内容
    TempConfigDir dir;
    const auto    overlayPath
        = dir.write("proj/agentxx-config.yaml", "skill:\n  list:\n    - /skills/s1\n");

    agentxx::client::LayeredConfigOptions opts;
    opts.overlayConfigPath = overlayPath;
    auto loaded            = agentxx::client::loadLayeredConfig(opts);
    XX_TEST_EXPECT_EQ(
        loaded.baseDir,
        agentxx::agent::AgentConfigStatic::systemDataDir()
    );
    XX_TEST_EXPECT_EQ(
        loaded.baseConfigPath,
        (fs::path{loaded.baseDir} / "agentxx-config.yaml").generic_string()
    );
    XX_TEST_EXPECT_EQ(loaded.baseEnvPath, (fs::path{loaded.baseDir} / ".env").generic_string());
    // overlay 自身的配置始终生效
    XX_TEST_EXPECT_EQ(loaded.cfg.skillDirPaths.size(), size_t{1});
}

void test_layered_overlay_missing() {
    // overlay 配置不存在 (工作目录无配置): 只用 base 层 (系统数据目录)
    TempConfigDir dir;
    const auto    missingOverlay = (dir.path() / "proj" / "agentxx-config.yaml").generic_string();

    agentxx::client::LayeredConfigOptions opts;
    opts.overlayConfigPath = missingOverlay;
    auto loaded            = agentxx::client::loadLayeredConfig(opts);
    XX_TEST_EXPECT_FALSE(loaded.overlayLoaded);
    XX_TEST_EXPECT_EQ(loaded.baseDir, agentxx::agent::AgentConfigStatic::systemDataDir());
}

void test_layered_dotenv_merge_helper() {
    // .env 分文件加载: overlay .env 与 base .env 同名变量取 overlay 值
    TempConfigDir dir;
    auto          basePath    = dir.write("data/.env", "A=base-a\nB=base-b\n");
    auto          overlayPath = dir.write("proj/.env", "B=overlay-b\nC=overlay-c\n");

    auto overloadEnv = agentxx::client::loadDotEnv(overlayPath);
    auto baseEnv     = agentxx::client::loadDotEnv(basePath);
    XX_TEST_EXPECT_EQ(baseEnv.size(), size_t{2});
    for (auto& kv : baseEnv) {
        overloadEnv.try_emplace(kv.first, kv.second);
    }
    XX_TEST_EXPECT_EQ(overloadEnv.size(), size_t{3});
    XX_TEST_EXPECT_EQ(overloadEnv["A"], std::string("base-a"));
    XX_TEST_EXPECT_EQ(overloadEnv["B"], std::string("overlay-b"));
    XX_TEST_EXPECT_EQ(overloadEnv["C"], std::string("overlay-c"));
}

void test_resolve_data_dir_value() {
    // 空值: 不持久化
    XX_TEST_EXPECT_TRUE(agentxx::client::resolveDataDirValue("").empty());
    // default 关键字: 系统数据目录
    XX_TEST_EXPECT_EQ(
        agentxx::client::resolveDataDirValue("default"),
        agentxx::agent::AgentConfigStatic::systemDataDir()
    );
    // 绝对路径: 词法规范化 + 正斜杠
    XX_TEST_EXPECT_EQ(
        agentxx::client::resolveDataDirValue("/data/agentxx/./sub/../sub2"),
        std::string("/data/agentxx/sub2")
    );
    // 相对路径: 按程序工作目录绝对化
    XX_TEST_EXPECT_EQ(
        agentxx::client::resolveDataDirValue("rel/data"),
        (fs::current_path() / "rel" / "data").generic_string()
    );
}

void test_read_yaml_data_dir_value() {
    TempConfigDir dir;
    // 存在 data_dir 字段: 返回展开后的原始值
    auto withDataDir = dir.write("a.yaml", "data_dir: ${AGENTXX_TEST_DD}/sub\n");
    XX_TEST_EXPECT_EQ(
        agentxx::client::readYamlDataDirValue(
            withDataDir,
            {{"AGENTXX_TEST_DD", "/dd"}},
            {}
        ),
        std::string("/dd/sub")
    );
    // 无该字段/空文件/文件不存在: 返回空串
    auto noDataDir = dir.write("b.yaml", "skill:\n  list:\n    - /s1\n");
    XX_TEST_EXPECT_TRUE(agentxx::client::readYamlDataDirValue(noDataDir, {}, {}).empty());
    auto empty = dir.write("c.yaml", "");
    XX_TEST_EXPECT_TRUE(agentxx::client::readYamlDataDirValue(empty, {}, {}).empty());
    XX_TEST_EXPECT_TRUE(
        agentxx::client::readYamlDataDirValue((dir.path() / "missing.yaml").generic_string(), {}, {})
            .empty()
    );
}

// ---------------------------------------------------------------------------
// 列表段 `overwrite` 策略 (mode: merge/replace + remove) 与段结构校验
// ---------------------------------------------------------------------------

void test_section_overwrite_replace() {
    // mode: replace = 整段不继承 base (只保留本层 list)
    TempConfigDir dir;
    auto          base    = dir.write("base.yaml", R"(skill:
  list:
    - /s/base1
    - /s/base2
plugin:
  list:
    - path: builtin://keep
)");
    auto overlay = dir.write("overlay.yaml", R"(skill:
  overwrite:
    mode: replace
  list:
    - /s/mine
plugin:
  overwrite:
    mode: replace
  list:
    - path: builtin://only-mine
)");
    auto cfg = agentxx::client::loadYamlConfigLayered(base, overlay, {}, {});
    XX_TEST_EXPECT_EQ(cfg.skillDirPaths.size(), size_t{1});
    if (cfg.skillDirPaths.size() == 1) {
        XX_TEST_EXPECT_EQ(cfg.skillDirPaths[0], std::string("/s/mine"));
    }
    XX_TEST_EXPECT_EQ(cfg.plugins.size(), size_t{1});
    if (cfg.plugins.size() == 1) {
        XX_TEST_EXPECT_EQ(cfg.plugins[0].path, std::string("builtin://only-mine"));
    }
}

void test_section_overwrite_remove_keyed() {
    // 按键列表 remove: plugin 按 path 匹配, 也可按 name / builtin://<name> 写法匹配
    TempConfigDir dir;
    auto          base    = dir.write("base.yaml", R"(plugin:
  list:
    - path: "/p1"
    - path: builtin://p2
    - name: p3
)");
    auto overlay = dir.write("overlay.yaml", R"(plugin:
  overwrite:
    remove:
      - "/p1"
      - p3
)");
    auto cfg = agentxx::client::loadYamlConfigLayered(base, overlay, {}, {});
    XX_TEST_EXPECT_EQ(cfg.plugins.size(), size_t{1});
    if (cfg.plugins.size() == 1) {
        XX_TEST_EXPECT_EQ(cfg.plugins[0].path, std::string("builtin://p2"));
    }

    // remove 未匹配到任何条目: 结果不变 (仅记警告)
    auto overlayNoMatch = dir.write("overlay_nomatch.yaml", R"(plugin:
  overwrite:
    remove:
      - does-not-exist
)");
    auto cfgNoMatch = agentxx::client::loadYamlConfigLayered(base, overlayNoMatch, {}, {});
    XX_TEST_EXPECT_EQ(cfgNoMatch.plugins.size(), size_t{3});
}

void test_section_overwrite_remove_path_list() {
    // 路径列表 remove: 按字符串本身匹配 (同时支持追加本层条目)
    TempConfigDir dir;
    auto          base    = dir.write("base.yaml", R"(skill:
  list:
    - /s/base1
    - /s/base2
memory:
  list:
    - /m/keep
    - /m/drop
)");
    auto overlay = dir.write("overlay.yaml", R"(skill:
  overwrite:
    remove:
      - /s/base1
  list:
    - /s/mine
memory:
  overwrite:
    remove:
      - /m/drop
)");
    auto cfg = agentxx::client::loadYamlConfigLayered(base, overlay, {}, {});
    XX_TEST_EXPECT_EQ(cfg.skillDirPaths.size(), size_t{2});
    if (cfg.skillDirPaths.size() == 2) {
        XX_TEST_EXPECT_EQ(cfg.skillDirPaths[0], std::string("/s/base2"));
        XX_TEST_EXPECT_EQ(cfg.skillDirPaths[1], std::string("/s/mine"));
    }
    XX_TEST_EXPECT_EQ(cfg.memoryFilePaths.size(), size_t{1});
    if (cfg.memoryFilePaths.size() == 1) {
        XX_TEST_EXPECT_EQ(cfg.memoryFilePaths[0], std::string("/m/keep"));
    }
}

void test_section_overwrite_remove_with_replace_mode() {
    // `remove` 在 replace 模式下同样生效 (从最终结果中剔除)
    TempConfigDir dir;
    auto          base    = dir.write("base.yaml", R"(skill:
  list:
    - /s/base1
)");
    auto overlay = dir.write("overlay.yaml", R"(skill:
  overwrite:
    mode: replace
    remove:
      - /s/mine2
  list:
    - /s/mine1
    - /s/mine2
)");
    auto cfg = agentxx::client::loadYamlConfigLayered(base, overlay, {}, {});
    XX_TEST_EXPECT_EQ(cfg.skillDirPaths.size(), size_t{1});
    if (cfg.skillDirPaths.size() == 1) {
        XX_TEST_EXPECT_EQ(cfg.skillDirPaths[0], std::string("/s/mine1"));
    }
}

void test_section_default_mode_merge() {
    // 未写 `overwrite` 时默认 merge: 按键列表归并 / 路径列表追加 (与旧行为一致)
    TempConfigDir dir;
    auto          base    = dir.write("base.yaml", R"(skill:
  list:
    - /s/base1
plugin:
  list:
    - path: "/p1"
      enabled: true
)");
    auto overlay = dir.write("overlay.yaml", R"(skill:
  list:
    - /s/mine
plugin:
  list:
    - path: "/p1"
      enabled: false
)");
    auto cfg = agentxx::client::loadYamlConfigLayered(base, overlay, {}, {});
    XX_TEST_EXPECT_EQ(cfg.skillDirPaths.size(), size_t{2});
    XX_TEST_EXPECT_EQ(cfg.plugins.size(), size_t{1});
    if (cfg.plugins.size() == 1) {
        XX_TEST_EXPECT_TRUE(!cfg.plugins[0].enabled); // overlay 覆盖同键字段
    }
}

void test_section_invalid_overwrite_values() {
    // 非法 `overwrite` (非映射 / 未知 mode / remove 非列表): 记警告并回退默认 (merge)
    TempConfigDir dir;
    auto          base = dir.write("base.yaml", R"(skill:
  list:
    - /s/base1
)");

    auto overlayScalar = dir.write("overlay_scalar.yaml", R"(skill:
  overwrite: replace
  list:
    - /s/mine
)");
    auto cfgScalar = agentxx::client::loadYamlConfigLayered(base, overlayScalar, {}, {});
    XX_TEST_EXPECT_EQ(cfgScalar.skillDirPaths.size(), size_t{2}); // 回退 merge: 追加

    auto overlayBadMode = dir.write("overlay_badmode.yaml", R"(skill:
  overwrite:
    mode: bogus
    remove: not-a-list
  list:
    - /s/mine2
)");
    auto cfgBadMode = agentxx::client::loadYamlConfigLayered(base, overlayBadMode, {}, {});
    XX_TEST_EXPECT_EQ(cfgBadMode.skillDirPaths.size(), size_t{2}); // mode 回退 merge, remove 忽略
}

void test_section_unknown_key_ignored() {
    // 段内未知键 (拼写错误等): 记警告并忽略, 不影响 `list` 生效
    TempConfigDir dir;
    auto          base = dir.write("base.yaml", R"(skill:
  list:
    - /s/base1
)");
    auto overlay = dir.write("overlay.yaml", R"(skill:
  miswritten: true
  list:
    - /s/mine
)");
    auto cfg = agentxx::client::loadYamlConfigLayered(base, overlay, {}, {});
    XX_TEST_EXPECT_EQ(cfg.skillDirPaths.size(), size_t{2});
}

void test_section_container_single_layer() {
    // 单层配置 (无 base) 也支持段结构, `overwrite` 无继承对象 (忽略)
    auto cfg = loadYaml(R"(skill:
  overwrite:
    mode: replace
  list:
    - /a
    - /b
plugin:
  list:
    - path: builtin://p1
)");
    XX_TEST_EXPECT_EQ(cfg.skillDirPaths.size(), size_t{2});
    XX_TEST_EXPECT_EQ(cfg.plugins.size(), size_t{1});
}

void test_section_model_list_and_use() {
    // model 段: `list` 按 name 归并 + `remove` 剔除; `use` 逐键合并
    TempConfigDir dir;
    auto          base    = dir.write("base.yaml", R"(model:
  list:
    - name: m1
      base_url: https://a.example.com
  use:
    default: m1
)");
    auto overlay = dir.write("overlay.yaml", R"(model:
  overwrite:
    remove:
      - m2
  list:
    - name: m2
    - name: m3
  use:
    subagent: m3
)");
    auto cfg = agentxx::client::loadYamlConfigLayered(base, overlay, {}, {});
    // m2 被 remove 剔除 (remove 对 overlay 自身条目同样生效)
    XX_TEST_EXPECT_EQ(cfg.models.size(), size_t{2});
    XX_TEST_EXPECT_TRUE(cfg.models.contains("m1"));
    XX_TEST_EXPECT_TRUE(cfg.models.contains("m3"));
    XX_TEST_EXPECT_FALSE(cfg.models.contains("m2"));
    auto m1 = cfg.models.find("m1");
    if (m1 != cfg.models.end()) {
        XX_TEST_EXPECT_EQ(m1->second.baseUrl, std::string("https://a.example.com"));
    }
    XX_TEST_EXPECT_EQ(cfg.useModelDefault, std::string("m1"));
    XX_TEST_EXPECT_EQ(cfg.useModelSubagent, std::string("m3"));
}

void test_section_permission_overwrite() {
    // permission 白/黑名单段: 支持 replace/remove, mode 仍为标量覆盖
    TempConfigDir dir;
    auto          base    = dir.write("base.yaml", R"(permission:
  mode: ask
  whitelist:
    list:
      - /allow/a
      - /allow/b
  blacklist:
    list:
      - /deny/x
)");
    auto overlay = dir.write("overlay.yaml", R"(permission:
  mode: pass
  whitelist:
    overwrite:
      mode: replace
      remove:
        - /allow/c
    list:
      - /allow/c
  blacklist:
    overwrite:
      remove:
        - /deny/x
)");
    auto cfg = agentxx::client::loadYamlConfigLayered(base, overlay, {}, {});
    XX_TEST_EXPECT_TRUE(cfg.permissionMode == agent::PermissionMode::Pass);
    // whitelist 整段替换 (/allow/c 又被 remove 剔除)
    XX_TEST_EXPECT_TRUE(cfg.permissionAllowPaths.empty());
    // blacklist 合并 + 剔除 base 项
    XX_TEST_EXPECT_TRUE(cfg.permissionDenyPaths.empty());
}

void test_permission_whitelist_without_blacklist() {
    // 只配置 whitelist (blacklist 段缺失) 时正常解析, 不因缺失键报错
    auto cfg = loadYaml(R"(permission:
  mode: ask
  whitelist:
    list:
      - "/tmp/"
)");
    XX_TEST_EXPECT_EQ(cfg.permissionAllowPaths.size(), size_t{1});
    XX_TEST_EXPECT_TRUE(cfg.permissionDenyPaths.empty());
}

void test_section_legacy_keys_ignored() {
    // 旧段名/旧位置 (models / plugins / use_model): 记迁移提示后忽略
    auto cfg = loadYaml(R"(models:
  - name: m1
    base_url: https://x.example.com
use_model:
  default: m1
plugins:
  - path: "/p1"
)");
    XX_TEST_EXPECT_TRUE(cfg.models.empty());
    XX_TEST_EXPECT_TRUE(cfg.useModelDefault.empty());
    XX_TEST_EXPECT_TRUE(cfg.plugins.empty());
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
    test_model_multimodal_input();
    test_plugins_empty_by_default();
    test_plugin_name_form_removed();
    test_plugin_args_paths_parse();
    test_yaml_to_json_big_integer();
    test_url_decode();
    test_plugin_missing_path_skipped();
    test_plugin_args_env_expand();
    test_plugins_config_path_parse();
    test_subagent_enable_default_true();
    test_subagent_enable_false();
    test_subagent_enable_true_variants();
    test_subagent_enable_invalid_fallback();
    test_subagent_enable_env_expand();
    test_layered_scalar_override();
    test_layered_models_merge();
    test_layered_mcp_and_plugins_merge();
    test_layered_path_lists_append();
    test_layered_plugin_args_list_replaced();
    test_layered_null_value_keeps_base();
    test_layered_config_with_base_env();
    test_layered_base_missing();
    test_layered_overlay_equals_base();
    test_layered_base_dir_defaults_to_system_dir();
    test_layered_overlay_missing();
    test_layered_dotenv_merge_helper();
    test_resolve_data_dir_value();
    test_read_yaml_data_dir_value();
    test_permission_whitelist_without_blacklist();
    test_section_overwrite_replace();
    test_section_overwrite_remove_keyed();
    test_section_overwrite_remove_path_list();
    test_section_overwrite_remove_with_replace_mode();
    test_section_default_mode_merge();
    test_section_invalid_overwrite_values();
    test_section_unknown_key_ignored();
    test_section_container_single_layer();
    test_section_model_list_and_use();
    test_section_permission_overwrite();
    test_section_legacy_keys_ignored();

    return TestResult{g_config_loader_passed, g_config_loader_failed};
}

} // namespace test
} // namespace agentxx
