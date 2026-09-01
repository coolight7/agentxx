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
    ///   {dataDir}/sqlite/sessions/{sessionId}/ (会话数据),
    ///   {dataDir}/sqlite/codegraph/... (CodeGraph 索引)
    std::string dataDir;

    /// 会话工作目录 (yaml `work_dir`, 支持 `${VAR}` 展开)
    /// - 为空 (默认): agent 使用进程当前工作目录 (旧行为)
    /// - 非空: 相对路径按程序工作目录解析为绝对路径后传入 AgentConfig::workDir,
    ///   作为 permission Ask 默认放行范围、filesystem 工具与权限校验的相对路径
    ///   解析基准、命令执行子进程初始目录、插件 projectRoot (codegraph 默认索引根)
    std::string workDir;

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
    /// 插件配置 (yaml `plugins` 列表项: path / enabled / sides / args / config)
    /// - path: 插件动态库路径 或 插件目录 (含 plugin.yaml 时按清单分派)
    ///   特殊前缀 `builtin://<name>` 表示内置编译插件 (无需外部文件)
    /// - enabled: 默认 true; sides: 运行侧 (auto/agent/client, 默认 auto);
    ///   args: 自定义参数 (预留, 存留供查询);
    ///   config: 插件配置文件所在目录或文件路径 (可指向文件/目录)
    std::vector<agent::PluginConfig> plugins;
    /// subagent 总开关 (yaml `subagent.enable`, 默认 true)
    bool enableSubagent = true;
    /// git worktree 模式开关 (yaml `worktree.enable`, 默认 false)
    /// - 开启后注册 agentxx_git_worktree 工具 + 注入行为提示词, 模型在代码
    ///   修改任务开始时创建独立 worktree 并绑定会话 (详见 tools/git_worktree.h)
    bool worktreeEnable = false;
};

/// 程序内置环境变量: 程序启动后的工作目录
/// - yaml 配置中可经 `${AGENTXX_WORK_DIR}` 引用 (如 `data_dir: ${AGENTXX_WORK_DIR}/data`)
/// - 值为程序启动 (main 入口) 时的工作目录; 查找顺序最优先 (先于 .env/系统环境变量)
/// - 未注入时 (测试/嵌入场景) resolveEnvVars 惰性回退 current_path()
inline constexpr std::string_view kBuiltinWorkDirEnv = "AGENTXX_WORK_DIR";

/// 程序内置环境变量: agentxx_cli 可执行程序所在目录
/// - yaml 配置中可经 `${AGENTXX_EXEC_DIR}` 引用 (如模型/插件路径相对 exe 目录)
/// - 值为程序启动时解析的可执行文件所在目录 (正斜杠格式); 仅 main 入口注入
/// - 未注入时 (测试/嵌入场景) resolveEnvVars 保留 ${AGENTXX_EXEC_DIR} 原样
///   (可执行目录无法惰性推导, 须由宿主在启动时注入)
inline constexpr std::string_view kBuiltinExecDirEnv = "AGENTXX_EXEC_DIR";

/// 注入程序内置环境变量 (main 启动时尽早调用; 供 yaml `${VAR}` 展开使用)
/// - 底层转发至 agentxx::util::ApplicationEnv 单例预设存储 (优先级高于系统环境变量, Windows 侧经
/// _dupenv_s 安全读取)
/// - 内置变量在 resolveEnvVars 中优先解析 (先于 override/env/.env)
/// - 传入空值表示清除该变量 (回退惰性解析)
void setBuiltinEnvVar(std::string_view name, std::string value);

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
