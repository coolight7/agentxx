/// 配置校验实现 (计划 CFG-1)
///
/// 目标: 把"配置哪里写错了"变成结构化结果 (键路径 + 严重级别 + 来源), 而不是散落各处
/// 的告警日志, 也不是启动到一半才发现模型/目录不可用。
///
/// 边界 (按计划人工核定的限定范围):
/// - 只做校验与提示, 不重建配置对象树, 不改 base/overlay 的合并语义;
/// - 需要"修正"的旧写法不在这里改写, 只给出警告 (读取侧的内存态适配在 loader);
/// - 不写文件、不改用户的配置文件。
#include "agentxx/agent/config_validation.h"

#include "fmt/format.h"
#include "utilxx_base/log.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace agentxx {
namespace agent {

namespace {

void addIssue(
    ConfigValidationReport& report,
    ConfigIssueLevel        level,
    std::string             keyPath,
    std::string             message
) {
    report.issues.push_back(ConfigIssue{
        .level   = level,
        .keyPath = std::move(keyPath),
        .message = std::move(message),
    });
}

/// 把字符串列表按键路径编号输出 (如 `permission.whitelist[0]`)
std::string indexed(std::string_view key, size_t index) {
    return fmt::format("{}[{}]", key, index);
}

/// 目录是否可用 (存在且是目录; 不存在时尝试创建一级目录)
/// - 不递归创建多级: 配置错误应当由用户修正, 不在校验阶段悄悄建出一串目录
bool ensureDirUsable(const std::string& path, std::string& error) {
    std::error_code ec;
    const auto      fp = utilxx_base::utf8ToPath(path);
    if (fs::exists(fp, ec)) {
        if (!fs::is_directory(fp, ec)) {
            error = "path exists but is not a directory";
            return false;
        }
        // 目录存在不代表可写 (只读挂载/权限): 写入探针文件再删除
        const auto probe = fp / ".agentxx_validate_probe";
        {
            std::ofstream ofs{probe, std::ios::binary | std::ios::trunc};
            if (!ofs) {
                error = "directory is not writable";
                return false;
            }
            ofs << "agentxx";
            ofs.flush();
            if (!ofs.good()) {
                error = "directory is not writable (probe write failed)";
                return false;
            }
        }
        fs::remove(probe, ec);
        return true;
    }
    // 不存在: 尝试创建 (只建一层, 父目录不存在时报错并保留原因)
    fs::create_directory(fp, ec);
    if (ec) {
        error = fmt::format("cannot create directory: {}", ec.message());
        return false;
    }
    return true;
}

} // namespace

bool ConfigValidationReport::hasFatal() const noexcept {
    return std::any_of(issues.begin(), issues.end(), [](const ConfigIssue& issue) {
        return issue.level == ConfigIssueLevel::Fatal;
    });
}

size_t ConfigValidationReport::count(ConfigIssueLevel level) const noexcept {
    return static_cast<size_t>(std::count_if(
        issues.begin(),
        issues.end(),
        [level](const ConfigIssue& issue) { return issue.level == level; }
    ));
}

std::vector<std::string> ConfigValidationReport::render() const {
    std::vector<std::string> lines;
    lines.reserve(issues.size());
    for (const auto& issue : issues) {
        lines.push_back(fmt::format(
            "[{}] {}: {}{}{}",
            issue.level == ConfigIssueLevel::Fatal ? "fatal" : "warning",
            issue.keyPath.empty() ? "<config>" : issue.keyPath,
            issue.message,
            issue.source.empty() ? "" : " (",
            issue.source.empty() ? "" : issue.source + ")"
        ));
    }
    return lines;
}

utilxx_base::Json ConfigValidationReport::toJson() const {
    utilxx_base::Json out = utilxx_base::Json::array();
    for (const auto& issue : issues) {
        utilxx_base::Json item = utilxx_base::Json::object();
        item["level"]   = issue.level == ConfigIssueLevel::Fatal ? "fatal" : "warning";
        item["key"]     = issue.keyPath;
        item["message"] = issue.message;
        if (!issue.source.empty()) {
            item["source"] = issue.source;
        }
        out.push_back(std::move(item));
    }
    return out;
}

void ConfigValidationReport::setSource(std::string_view source) {
    for (auto& issue : issues) {
        if (issue.source.empty()) {
            issue.source = std::string{source};
        }
    }
}

ConfigValidationReport validateAgentConfig(const AgentConfig& config) {
    ConfigValidationReport report;

    // ---- 模型 ----
    const bool hasUsableModel = config.model.isValid() || !config.availableModels.empty();
    if (!hasUsableModel) {
        addIssue(
            report,
            ConfigIssueLevel::Fatal,
            "model.list",
            "no usable model configured: set `model.base_url`/`model.api_key` or add at least "
            "one entry to `model.list`"
        );
    }
    if (!config.availableModels.empty() && !config.currentModelName.empty()
        && !config.availableModels.contains(config.currentModelName)) {
        addIssue(
            report,
            ConfigIssueLevel::Fatal,
            "model.use",
            fmt::format(
                "selected model `{}` is not in `model.list` (available: {})",
                config.currentModelName,
                config.availableModels.size()
            )
        );
    }
    for (const auto& [name, mc] : config.availableModels) {
        if (!mc.isValid()) {
            addIssue(
                report,
                ConfigIssueLevel::Warning,
                fmt::format("model.list[{}]", name),
                "model has neither `base_url` nor `api_key`: requests will fail at runtime"
            );
        }
    }
    if (config.subagentModel.has_value() && !config.subagentModel->isValid()) {
        addIssue(
            report,
            ConfigIssueLevel::Warning,
            "model.subagent",
            "subagent model has neither `base_url` nor `api_key`: it will fail at runtime"
        );
    }
    if (config.websearchModel.has_value() && !config.websearchModel->isValid()) {
        addIssue(
            report,
            ConfigIssueLevel::Warning,
            "model.websearch",
            "websearch model has neither `base_url` nor `api_key`: it will fail at runtime"
        );
    }

    // ---- 路径形态 (绝对路径要求; 相对路径的展开归属装配侧) ----
    if (!config.workDir.empty() && !fs::path(config.workDir).is_absolute()) {
        addIssue(
            report,
            ConfigIssueLevel::Fatal,
            "work_dir",
            fmt::format(
                "must be an absolute path (got `{}`); resolve relative paths against the process "
                "cwd at assembly time",
                config.workDir
            )
        );
    }
    if (!config.dataDir.empty() && !fs::path(config.dataDir).is_absolute()) {
        addIssue(
            report,
            ConfigIssueLevel::Warning,
            "data_dir",
            fmt::format("is not an absolute path (`{}`): it will be resolved against the process "
                        "cwd, which changes when the program is started elsewhere",
                        config.dataDir)
        );
    }
    if (!config.sessionStoreDirectory.empty()
        && !fs::path(config.sessionStoreDirectory).is_absolute()) {
        addIssue(
            report,
            ConfigIssueLevel::Warning,
            "session_store.directory",
            fmt::format("is not an absolute path (`{}`)", config.sessionStoreDirectory)
        );
    }
    for (size_t i = 0; i < config.skillDirPaths.size(); ++i) {
        if (config.skillDirPaths[i].empty()) {
            addIssue(report, ConfigIssueLevel::Warning, indexed("skill.paths", i), "empty path entry, ignored");
        }
    }
    for (size_t i = 0; i < config.memoryFilePaths.size(); ++i) {
        if (config.memoryFilePaths[i].empty()) {
            addIssue(
                report,
                ConfigIssueLevel::Warning,
                indexed("memory.paths", i),
                "empty path entry, ignored"
            );
        }
    }

    // ---- 权限组合 ----
    // 判定顺序 (见 docs/zh-cn/design/security.md): 工作区隔离 > 配置拒绝 > 模式默认 > 规则
    // 说明: `ask` 模式默认放行规则跟随工作目录 (未配置 work_dir 时回退进程 cwd) 属既定行为,
    // 不在此告警 (启动日志已打印实际模式); 这里只报"配置之间互相矛盾/无效"的组合。
    if (config.permissionMode == PermissionMode::Deny && !config.permissionAllowPaths.empty()) {
        addIssue(
            report,
            ConfigIssueLevel::Warning,
            "permission.mode",
            fmt::format(
                "mode `deny` with {} whitelist path(s): the whitelist still wins over the mode "
                "default, so those paths stay accessible",
                config.permissionAllowPaths.size()
            )
        );
    }
    if (config.permissionMode == PermissionMode::Pass) {
        addIssue(
            report,
            ConfigIssueLevel::Warning,
            "permission.mode",
            "mode `pass` allows every path without asking: use it only in trusted environments"
        );
    }
    for (size_t i = 0; i < config.permissionAllowPaths.size(); ++i) {
        if (config.permissionAllowPaths[i].empty()) {
            addIssue(
                report,
                ConfigIssueLevel::Warning,
                indexed("permission.whitelist", i),
                "empty path entry, ignored"
            );
        }
    }
    for (size_t i = 0; i < config.permissionDenyPaths.size(); ++i) {
        if (config.permissionDenyPaths[i].empty()) {
            addIssue(
                report,
                ConfigIssueLevel::Warning,
                indexed("permission.blacklist", i),
                "empty path entry, ignored"
            );
        }
    }
    {
        // 同一路径同时出现在白名单与黑名单: 黑名单优先 (行为明确, 但通常是配置写重了)
        for (size_t i = 0; i < config.permissionAllowPaths.size(); ++i) {
            const auto& allowPath = config.permissionAllowPaths[i];
            for (size_t j = 0; j < config.permissionDenyPaths.size(); ++j) {
                if (allowPath == config.permissionDenyPaths[j]) {
                    addIssue(
                        report,
                        ConfigIssueLevel::Warning,
                        indexed("permission.whitelist", i),
                        fmt::format(
                            "also listed in `permission.blacklist[{}]`: the blacklist wins, the "
                            "whitelist entry has no effect",
                            j
                        )
                    );
                }
            }
        }
    }

    // ---- 取值会被夹取/忽略的字段 ----
    if (config.toolParallelMaxConcurrency < 1 || config.toolParallelMaxConcurrency > 32) {
        addIssue(
            report,
            ConfigIssueLevel::Warning,
            "tool.parallel_max",
            fmt::format(
                "{} is out of range [1, 32]: it will be clamped at startup",
                config.toolParallelMaxConcurrency
            )
        );
    }
    if (config.enableToolFiltering && config.toolWhitelist.empty()) {
        addIssue(
            report,
            ConfigIssueLevel::Warning,
            "tool.whitelist",
            "tool filtering is enabled but the whitelist is empty: no tool will be assembled"
        );
    }
    if (config.toolcallRepeatCheckThreshold == 0) {
        addIssue(
            report,
            ConfigIssueLevel::Warning,
            "tool.repeat_check_threshold",
            "0 disables the repeated-call check for every tool"
        );
    }
    if (config.llmMaxRetry > 10) {
        addIssue(
            report,
            ConfigIssueLevel::Warning,
            "llm.max_retry",
            fmt::format("{} is very high: a failing request can take a long time to give up", config.llmMaxRetry)
        );
    }

    // ---- 插件声明 ----
    for (size_t i = 0; i < config.plugins.size(); ++i) {
        const auto& plugin = config.plugins[i];
        const auto  key    = indexed("plugin.list", i);
        if (plugin.path.empty()) {
            addIssue(report, ConfigIssueLevel::Warning, key, "empty `path`, entry is ignored");
        }
        if (!plugin.configPath.empty() && !fs::path(plugin.configPath).is_absolute()
            && !plugin.configPath.starts_with("builtin://")) {
            addIssue(
                report,
                ConfigIssueLevel::Fatal,
                fmt::format("{}.config", key),
                fmt::format(
                    "must be an absolute path (got `{}`); resolve it at assembly time",
                    plugin.configPath
                )
            );
        }
    }
    // 同一文件/目录被声明多次: 第二次装载会因重名工具被拒绝 (日志里容易被忽略)
    for (size_t i = 0; i < config.plugins.size(); ++i) {
        if (config.plugins[i].path.empty()) {
            continue;
        }
        for (size_t j = i + 1; j < config.plugins.size(); ++j) {
            if (config.plugins[j].path == config.plugins[i].path) {
                addIssue(
                    report,
                    ConfigIssueLevel::Warning,
                    indexed("plugin.list", j),
                    fmt::format(
                        "duplicates `plugin.list[{}]` (same path `{}`): the second load fails on "
                        "duplicate tool names",
                        i,
                        config.plugins[j].path
                    )
                );
            }
        }
    }

    // ---- MCP ----
    for (const auto& [ns, mcp] : config.mcpServerUrls) {
        if (ns.empty()) {
            addIssue(
                report,
                ConfigIssueLevel::Warning,
                "mcp.list",
                "empty namespace: tools of that server cannot be namespaced"
            );
        }
        if (mcp.url.empty()) {
            addIssue(
                report,
                ConfigIssueLevel::Warning,
                fmt::format("mcp.list[{}]", ns),
                "empty `url`: the server is skipped at startup"
            );
        }
    }

    // ---- 持久化组合 ----
    if (config.enableSessionStore && config.dataDir.empty()
        && config.sessionStoreDirectory.empty()) {
        addIssue(
            report,
            ConfigIssueLevel::Warning,
            "session_store",
            "persistence is enabled but neither `data_dir` nor a session root is set: sessions "
            "stay in memory and are lost on exit"
        );
    }
    if (!config.enableSessionStore && !config.sessionStoreDirectory.empty()) {
        addIssue(
            report,
            ConfigIssueLevel::Warning,
            "session_store.enable",
            "a session root is configured but persistence is disabled: the root is unused"
        );
    }

    return report;
}

ConfigValidationReport validateConfigPaths(const AgentConfig& config) {
    ConfigValidationReport report;

    // ---- 会话根目录 / 数据目录 (仅在要求持久化时视为致命) ----
    const bool persistenceRequested = config.enableSessionStore
                                      && (!config.dataDir.empty()
                                          || !config.sessionStoreDirectory.empty());
    if (!config.dataDir.empty()) {
        std::string error;
        if (!ensureDirUsable(config.dataDir, error)) {
            addIssue(
                report,
                persistenceRequested ? ConfigIssueLevel::Fatal : ConfigIssueLevel::Warning,
                "data_dir",
                fmt::format("`{}` is not usable: {}", config.dataDir, error)
            );
        }
    } else {
        addIssue(
            report,
            ConfigIssueLevel::Warning,
            "data_dir",
            "not set: settings/sessions/codegraph data stay in memory and are lost on exit"
        );
    }
    if (!config.sessionStoreDirectory.empty()) {
        std::string error;
        if (!ensureDirUsable(config.sessionStoreDirectory, error)) {
            addIssue(
                report,
                ConfigIssueLevel::Fatal,
                "session_store.directory",
                fmt::format("`{}` is not usable: {}", config.sessionStoreDirectory, error)
            );
        }
    }

    // ---- 资源路径: 不存在只警告, 启动时记为加载失败 ----
    auto checkList = [&](const std::vector<std::string>& paths, std::string_view key, bool wantDir) {
        for (size_t i = 0; i < paths.size(); ++i) {
            if (paths[i].empty()) {
                continue;
            }
            std::error_code ec;
            const auto      fp       = utilxx_base::utf8ToPath(paths[i]);
            const bool      exists   = fs::exists(fp, ec);
            const bool      typeOk   = exists && (wantDir ? fs::is_directory(fp, ec) : true);
            if (!exists) {
                addIssue(
                    report,
                    ConfigIssueLevel::Warning,
                    indexed(key, i),
                    fmt::format(
                        "`{}` does not exist: it is recorded as a failed component at startup",
                        paths[i]
                    )
                );
            } else if (!typeOk) {
                addIssue(
                    report,
                    ConfigIssueLevel::Warning,
                    indexed(key, i),
                    fmt::format("`{}` is not a directory", paths[i])
                );
            } else if (ec) {
                addIssue(
                    report,
                    ConfigIssueLevel::Warning,
                    indexed(key, i),
                    fmt::format("`{}` is not accessible: {}", paths[i], ec.message())
                );
            }
        }
    };
    checkList(config.skillDirPaths, "skill.paths", true);
    checkList(config.memoryFilePaths, "memory.paths", false);
    checkList(config.ragDocsPaths, "rag.paths", true);

    // ---- 插件引用的文件/目录 (缺失只警告: 启动时跳过该插件) ----
    for (size_t i = 0; i < config.plugins.size(); ++i) {
        const auto& plugin = config.plugins[i];
        if (plugin.path.empty() || plugin.path.starts_with("builtin://")) {
            continue;
        }
        std::error_code ec;
        const auto      fp     = utilxx_base::utf8ToPath(plugin.path);
        const bool      exists = fs::exists(fp, ec);
        if (!exists) {
            addIssue(
                report,
                ConfigIssueLevel::Warning,
                indexed("plugin.list", i),
                fmt::format("`{}` does not exist: the plugin is skipped at startup", plugin.path)
            );
        }
    }

    // ---- MCP 端点形态 (不连接网络, 只校验协议前缀) ----
    for (const auto& [ns, mcp] : config.mcpServerUrls) {
        if (mcp.url.empty()) {
            continue;
        }
        if (!mcp.url.starts_with("http://") && !mcp.url.starts_with("https://")
            && !mcp.url.starts_with("ws://") && !mcp.url.starts_with("wss://")) {
            addIssue(
                report,
                ConfigIssueLevel::Warning,
                fmt::format("mcp.list[{}].url", ns),
                fmt::format("`{}` does not look like an http(s)/ws(s) endpoint", mcp.url)
            );
        }
    }

    return report;
}

ConfigValidationReport validateAgentConfigWithPaths(const AgentConfig& config) {
    auto report = validateAgentConfig(config);
    auto paths  = validateConfigPaths(config);
    report.issues.insert(
        report.issues.end(),
        std::make_move_iterator(paths.issues.begin()),
        std::make_move_iterator(paths.issues.end())
    );
    return report;
}

} // namespace agent
} // namespace agentxx
