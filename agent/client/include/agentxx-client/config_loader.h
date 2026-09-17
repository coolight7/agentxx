#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/agent/model_registry.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
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
    ///   修改任务开始时创建独立 worktree 并绑定会话 (详见
    ///   [git_worktree.h](/agent/lib/include/agentxx/tools/git_worktree.h))
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
/// - 底层转发至 utilxx_base::ApplicationEnv 单例预设存储 (优先级高于系统环境变量, Windows 侧经
/// _dupenv_s 安全读取)
/// - 内置变量在 resolveEnvVars 中优先解析 (先于 override/env/.env)
/// - 传入空值表示清除该变量 (回退惰性解析)
void setBuiltinEnvVar(std::string_view name, std::string value);

/// 配置文件名 (overlay 层: 工作目录或 `--config` 指定的文件;
/// base 层: data_dir 目录下的同名文件)
inline constexpr std::string_view kDefaultConfigFileName = "agentxx-config.yaml";
/// 环境变量文件名 (base 层: data_dir 目录下的该文件)
inline constexpr std::string_view kDefaultEnvFileName = ".env";

std::map<std::string, std::string> loadDotEnv(std::string_view path);
std::map<std::string, std::string> loadDotEnv(const std::vector<std::string>& paths);
std::map<std::string, std::string> loadOverrideEnv(std::string_view path);

std::string resolveEnvVars(
    std::string_view                          input,
    const std::map<std::string, std::string>& dotEnvVars,
    const std::map<std::string, std::string>& overrideEnvVars
);

/// 加载单个配置文件为应用配置 (单层; 分层合并见 loadYamlConfigLayered)
/// - 同样按新段结构归一化 (`model.list` / `plugin.list` / `*/list` + `overwrite`),
///   解析层只读归一化后的形状
YamlAppConfig loadYamlConfig(
    std::string_view                          path,
    const std::map<std::string, std::string>& dotEnvVars,
    const std::map<std::string, std::string>& overrideEnvVars
);

/// 解析 yaml `data_dir` 配置值为绝对路径 (含 `~` 展开与相对路径按程序工作目录绝对化)
/// - 空值返回空串 (表示不持久化数据)
/// - `default` 关键字返回系统数据目录 (Linux/macOS: ~/.agentxx/,
///   Windows: %APPDATA%/agentxx/)
std::string resolveDataDirValue(std::string_view raw);

/// 读取 yaml 配置中 `data_dir` 字段的原始值 (经 `${VAR}` 展开, 不做路径归一化)
/// - 仅用于分层加载时定位 base 配置所在目录 (此时 base 的 .env 尚未加载)
/// - 文件不存在/根节点非映射/无 `data_dir` 字段时返回空串; 解析失败抛出异常
std::string readYamlDataDirValue(
    std::string_view                          path,
    const std::map<std::string, std::string>& dotEnvVars,
    const std::map<std::string, std::string>& overrideEnvVars
);

/// 合并加载 base + overlay 两个配置文件 (base 为底, overlay 覆盖, 路径为空表示该层不存在)
///
/// 配置结构 (列表型配置段统一为 `list:` + 可选 `overwrite:` 两键):
/// ```yaml
/// model:
///     overwrite:                 # 与 base 的合并策略 (可省略, 默认 merge)
///         mode: merge            # merge(默认, 继承并叠加 base) | replace(整段只用本层)
///         remove:                # 可选: 从合并结果中剔除的条目 (按身份匹配)
///             - old-model
///     list:                      # 本层条目 (可省略 = 空)
///         - name: my-model
///           type: openai
///     use:                       # 各用途使用的模型名 (原顶层 use_model 段)
///         default: my-model
/// plugin:
///     overwrite: {mode: merge, remove: [agentxx_codegraph]}
///     list:
///         - path: builtin://agentxx_filesystem
/// skill:
///     overwrite: {mode: replace}  # 只用本层技能列表
///     list: [./skills]
/// permission:
///     mode: ask
///     whitelist:
///         overwrite: {mode: merge, remove: [/home/other]}
///         list: [/workspace]
/// ```
/// - 适用段: `model` / `plugin` / `mcp` / `skill` / `memory` /
///   `permission.whitelist` / `permission.blacklist`
/// - 段值必须是映射 (`list:` 存条目, `overwrite:` 存策略); 旧写法直接给列表
///   (`skill: [a, b]`) 或给空字符串 (`skill: ""`) 已不再支持, 会记警告并忽略该段
/// - `remove` 匹配身份: `model` 按 `name`, `plugin` 按 `path`(`name` 与
///   `builtin://<name>` 写法同样可匹配), `mcp` 按 `namespace`,
///   `skill`/`memory`/权限名单按字符串本身 (按原始文本比较, 不展开 `${VAR}`)
/// - `remove` 在两种模式下都生效 (从最终结果中剔除); 未匹配到任何条目时记警告
/// - 策略只由 overlay 层生效: base 自身作为底层没有继承对象, 其 `overwrite` 忽略
/// - 其他配置段 (标量/映射) 仍按 `data_dir`/`work_dir` 覆盖、
///   `subagent`/`worktree` 等逐键递归合并, 列表型字段整体覆盖
///
/// 合并规则 (逐键判断, 只有 overlay 中出现的键才会覆盖 base):
/// - 标量 (`data_dir` / `work_dir` / `permission.mode` 等): overlay 覆盖
/// - 映射 (`permission` 的其余键, 模型 `extra_headers` / `extra_api_config`,
///   插件 `args` 等): 逐键递归合并, 同键 overlay 覆盖; 映射内列表整体覆盖
/// - 列表段: 见上方 `list` / `overwrite` 结构 (`mode: merge` 时按键归并或追加去重)
/// - 显式空值 (`key:` 无内容 = null) 视为该层未配置, 不参与覆盖
/// - 列表归并保持 base 项在前, 结果顺序稳定 (便于日志与测试比对)
YamlAppConfig loadYamlConfigLayered(
    std::string_view                          basePath,
    std::string_view                          overlayPath,
    const std::map<std::string, std::string>& dotEnvVars,
    const std::map<std::string, std::string>& overrideEnvVars
);

/// 分层配置加载参数 (base 为底, overlay 覆盖)
struct LayeredConfigOptions {
    /// 上层 (overlay) 配置路径; 默认 `agentxx-config.yaml`, 可经 `--config` 指定
    std::string overlayConfigPath;
    /// 上层 .env 文件路径列表 (后者覆盖前者; 通常为
    /// [程序工作目录/.env, 上层配置所在目录/.env])
    std::vector<std::string> overlayEnvPaths;
    /// `--env` 指定的覆盖式环境变量文件 (最高优先级; 空表示未指定)
    std::string overrideEnvPath;
};

/// 分层配置加载结果
struct LayeredConfigLoad {
    /// 合并后的配置 (base 为底 + overlay 覆盖)
    YamlAppConfig cfg;
    /// 上层配置文件路径 (原样回传)
    std::string overlayConfigPath;
    /// base 配置文件路径 ({baseDir}/agentxx-config.yaml)
    std::string baseConfigPath;
    /// base .env 文件路径 ({baseDir}/.env)
    std::string baseEnvPath;
    /// 定位 base 用的数据目录: overlay 的 `data_dir` (经 `~`/相对路径归一化);
    /// overlay 未配置 `data_dir` 或 overlay 不存在时取系统数据目录
    std::string baseDir;
    /// 各层配置文件是否存在并加载成功
    bool overlayLoaded = false;
    bool baseLoaded    = false;
    /// 合并后的 .env 变量 (overlay 优先: base 同名变量被舍弃)
    std::map<std::string, std::string> dotEnvVars;
    /// `--env` 覆盖式文件变量 (最高优先级)
    std::map<std::string, std::string> overrideEnvVars;
    /// base .env 变量总数 / 其中因 overlay 同名而被舍弃的数量 (日志与测试用)
    size_t baseEnvTotal   = 0;
    size_t baseEnvDropped = 0;
};

/// 分层加载应用配置 (data_dir 目录下的 base 配置为底, overlay 配置覆盖)
/// - overlay: `opts.overlayConfigPath` (程序工作目录或 `--config` 指定)
/// - base: overlay 的 `data_dir` 目录下的 `agentxx-config.yaml` 与 `.env`;
///   overlay 未配置 `data_dir` (或 overlay 配置不存在) 时使用系统数据目录,
///   即程序工作目录无配置时直接加载数据目录下的配置
/// - 只加载一层 base: base 配置内的 `data_dir` 不再向下查找
/// - base 与 overlay 指向同一文件 (如 `--config <data_dir>/agentxx-config.yaml`) 时
///   只加载一次, 不重复合并
/// - .env 变量: 内置变量 > `--env` > overlay .env > base .env (同名舍弃 base 值)
///   > 系统环境变量; 合并后的变量同时用于展开两层 yaml 中的 `${VAR}`
/// - base 层解析失败 (文件损坏等) 仅记错误日志并忽略 base, 不影响 overlay;
///   overlay 层解析失败抛出异常, 由调用方决定是否退出
LayeredConfigLoad loadLayeredConfig(const LayeredConfigOptions& opts);

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
