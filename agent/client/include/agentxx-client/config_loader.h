#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/util/log.h"
#include "neograph/json.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace agentxx {
namespace client {

/// 权限模式名称 (供日志/启动提示/测试展示; 越界返回 "ask")
inline constexpr std::string_view permissionModeName(agent::PermissionMode mode) noexcept {
    switch (mode) {
        case agent::PermissionMode::Ask:
            return "ask";
        case agent::PermissionMode::AllAsk:
            return "all_ask";
        case agent::PermissionMode::Pass:
            return "pass";
        case agent::PermissionMode::Deny:
            return "deny";
    }
    return "ask";
}

inline constexpr std::string_view permissionModeDepict(agent::PermissionMode mode) noexcept {
    switch (mode) {
        case agent::PermissionMode::Ask:
            return "工作目录内允许/其他询问";
        case agent::PermissionMode::AllAsk:
            return "全部询问";
        case agent::PermissionMode::Pass:
            return "全部允许";
        case agent::PermissionMode::Deny:
            return "全部拒绝";
    }
    return "ask";
}

/// CodeGraph 代码分析配置 (yaml `codegraph` 块)
/// - 默认禁用 (enable=false); 索引加载路径/忽略路径可配置
struct CodeGraphConfig {
    /// 是否启用 CodeGraph 代码分析 (yaml `codegraph.enable`, 默认 false)
    bool enable = false;
    /// 加载(索引)路径列表 (yaml `codegraph.paths`, 相对路径按工作目录解析为绝对路径)
    /// - 非空时按此列表索引; 为空时按 loadCwd 决定是否默认索引当前工作目录
    std::vector<std::string> paths;
    /// 忽略路径列表 (yaml `codegraph.ignore_paths`, 相对路径按工作目录解析,
    /// 支持 * 通配符; 命中即跳过)
    std::vector<std::string> ignorePaths;
    /// 未配置 paths 时是否默认加载当前工作目录 (yaml `codegraph.load_cwd`, 默认 true)
    bool loadCwd = true;
    /// 是否默认启用 .gitignore 规则与 .gitmodules 子模块目录忽略
    /// (yaml `codegraph.use_gitignore`, 默认 true)
    bool useGitignore = true;
};

struct YamlAppConfig {
    std::map<std::string, agent::ModelConfig> models;
    /// MCP 服务器配置 (yaml `mcp` 列表项, key 为命名空间)
    /// - timeout 字段按秒配置, 0 = 不限制, 未配置默认 120 秒
    std::map<std::string, agent::McpServerConfig> mcpServers;
    std::vector<std::string>                      skillDirPaths;
    std::vector<std::string>                      memoryFilePaths;
    std::string                                   useModelDefault;
    std::string                                   useModelSubagent;
    std::string                                   useModelWebSearch;
    std::string                                   useModelAcp;
    std::string                                   useModelTrain;
    std::string                                   useModelTrainScorer;
    std::string                                   useModelTrainOptimizer;
    /// 统一数据根目录 (yaml `data_dir`, 支持 `~`/环境变量展开)
    /// - 为空表示不持久化: 设置/会话/codegraph 数据仅存内存 (BaseAgent 输出警告)
    /// - 特殊关键字 `default` (仅 tui/cli 模式): 使用当前系统数据目录
    ///   (Linux/macOS: ~/.agentxx/, Windows: %APPDATA%/agentxx/)
    /// - 非空时数据子路径: {dataDir}/sqlite/global.db (全局设置),
    ///   {dataDir}/sqlite/sessions/{threadId}/ (会话数据),
    ///   {dataDir}/sqlite/codegraph/... (CodeGraph 索引)
    std::string dataDir;
    /// CodeGraph 代码分析配置 (yaml `codegraph` 块, 默认禁用)
    /// - 索引数据库: {dataDir}/sqlite/codegraph/<折叠路径>/index.db
    ///   (深层路径折叠 + 单段截断控制长度, 子目录可前缀复用最近父级索引)
    CodeGraphConfig codeGraph;

    /// 权限询问处理模式 (yaml `permission.mode`: ask/all_ask/pass/deny, 默认 ask)
    /// - ask:     当前工作目录内允许读写, 其他路径询问用户
    /// - all_ask: 所有路径读写均询问用户
    /// - pass:    全部放行, 不询问
    /// - deny:    全部拒绝, 不询问
    /// 服务端 CodeAgent 按模式注册文件系统读写规则; 客户端仅对仍到达的
    /// 权限 INTERRUPT 作兜底 (pass 放行 / deny 拒绝 / ask、all_ask 询问)
    agent::PermissionMode permissionMode = agent::PermissionMode::Ask;
    /// 权限白名单: 始终放行的路径列表 (yaml `permission.whitelist`)
    /// - 最长前缀匹配, 支持 * 通配符; 相对路径按程序工作目录解析
    /// - 优先级高于模式默认规则 (如 deny 模式下白名单路径仍可访问)
    std::vector<std::string> permissionAllowPaths;
    /// 权限黑名单: 始终拒绝的路径列表 (yaml `permission.blacklist`)
    /// - 与白名单同路径时黑名单优先 (后注册覆盖)
    std::vector<std::string> permissionDenyPaths;
    /// 插件配置 (yaml `plugins` 列表项: path / enabled / args)
    /// - path: 插件动态库路径 或 插件目录 (含 plugin.yaml 时按清单分派)
    /// - enabled: 默认 true; args: 自定义参数 (预留, 存留供查询)
    std::vector<agent::PluginConfig> plugins;
};

std::map<std::string, std::string> loadDotEnv(std::string_view path);
std::map<std::string, std::string> loadDotEnv(const std::vector<std::string>& paths);
std::map<std::string, std::string> loadOverrideEnv(std::string_view path);

std::string resolveEnvVars(
    std::string_view                          input,
    const std::map<std::string, std::string>& dotEnvVars,
    const std::map<std::string, std::string>& overrideEnvVars
);

YamlAppConfig loadYamlConfig(
    std::string_view                          path,
    const std::map<std::string, std::string>& dotEnvVars,
    const std::map<std::string, std::string>& overrideEnvVars
);

agent::ModelConfig resolveModelConfig(
    const std::map<std::string, agent::ModelConfig>& models,
    std::string_view                                 modelName
);

void applyModelToConfig(
    std::shared_ptr<agent::AgentConfig>              agentConfig,
    const std::map<std::string, agent::ModelConfig>& models,
    std::string_view                                 modelName
);

void applySubagentModelToConfig(
    std::shared_ptr<agent::AgentConfig>              agentConfig,
    const std::map<std::string, agent::ModelConfig>& models,
    std::string_view                                 modelName
);

void applyWebSearchModelToConfig(
    std::shared_ptr<agent::AgentConfig>              agentConfig,
    const std::map<std::string, agent::ModelConfig>& models,
    std::string_view                                 modelName
);

void applyAvailableModelsToConfig(
    std::shared_ptr<agent::AgentConfig>              agentConfig,
    const std::map<std::string, agent::ModelConfig>& models,
    std::string_view                                 currentModelName
);

} // namespace client
} // namespace agentxx
