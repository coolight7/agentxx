#pragma once

#include "agentxx/agent/config.h"
#include "utilxx_base/json.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace agent {

/// 配置问题严重级别 (计划 CFG-1)
/// - Warning: 可以继续启动, 但行为与预期可能不同 (内存降级、被忽略的键、会被夹取的取值)
/// - Fatal:   必须终止启动 (没有可用模型、路径不可用、必需的绝对路径写成了相对路径)
enum class ConfigIssueLevel : uint8_t {
    Warning = 0,
    Fatal,
};

/// 一条配置问题
///
/// 结构化而非一句文本: 键路径用于定位到具体配置项, 来源说明问题出自哪份文件
/// (base/overlay/env/默认值), 便于"按提示改一行配置"而不用通读启动日志。
struct ConfigIssue {
    ConfigIssueLevel level = ConfigIssueLevel::Warning;
    /// 配置键路径, 如 `model.use` / `permission.mode` / `plugin.list[2].path`
    std::string keyPath;
    /// 面向用户的问题说明 (原因 + 如何修正)
    std::string message;
    /// 来源描述 (配置文件链 / 环境变量 / 默认值); 由装配侧填入, 允许为空
    std::string source;
};

/// 配置校验结果
struct ConfigValidationReport {
    std::vector<ConfigIssue> issues;

    bool hasFatal() const noexcept;
    size_t count(ConfigIssueLevel level) const noexcept;
    /// 一行一条: `[warning] key.path: message (source)`
    std::vector<std::string> render() const;
    /// 结构化输出 (供装配快照 / `--dump-config` 使用)
    utilxx_base::Json toJson() const;
    /// 给已有及后续问题统一打来源标签
    void setSource(std::string_view source);
};

/// 配置结构/语义校验 (不访问文件系统)
/// - 模型必填与当前模型可解析、路径字段形态 (绝对路径)、权限组合、取值范围与夹取提示、
///   重复/空字段
ConfigValidationReport validateAgentConfig(const AgentConfig& config);

/// 路径存在性与可写性检查 (访问文件系统, 只做 stat/建目录探测)
/// - skill/memory/rag 路径不存在 → Warning (不阻断, 启动时记为加载失败)
/// - 插件配置引用的文件/目录不存在 → Warning (启动时跳过该插件)
/// - 持久化开启时 dataDir / session root 不可用 → Fatal
/// - 未配置 dataDir (内存模式) → Warning (明确"重启后不保留")
ConfigValidationReport validateConfigPaths(const AgentConfig& config);

/// 结构校验 + 路径校验 (启动装配侧统一调用)
ConfigValidationReport validateAgentConfigWithPaths(const AgentConfig& config);

} // namespace agent
} // namespace agentxx
