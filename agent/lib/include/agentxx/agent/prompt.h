#pragma once

#include "utilxx_base/json.h"

#include <map>
#include <string>
#include <string_view>

namespace agentxx {
namespace agent {

class ToolPrompt {
public:

    /// 工具描述 (非 const: 训练过程中允许修改)
    std::string depict;
    /// 工具参数描述 (非 const: 训练过程中允许修改)
    std::map<std::string, std::string, std::less<>> args;

    const std::string& getArg(std::string_view name) const;
};

/// 会话级提示词取值 (供 [AgentPrompt::renderVars] 替换占位符)
struct PromptSessionVars {
    /// 会话工作目录 (会话工作目录覆写 > agent 配置 / 进程 cwd)
    /// - 不含 worktree 绑定: 进出 worktree 由工具在会话内切换, 系统提示词不随之变化
    /// - 取不到时为空串, 替换时按 [AgentPrompt::renderVars] 的约定处理
    std::string workDir;
    /// 会话临时目录 ({系统临时目录}/agentxx/{会话 ID}/)
    std::string tempDir;
    /// 会话 ID (原文; 临时目录路径里的目录段已按文件系统规则清洗)
    std::string sessionId;
};

/// 提示词注册表
/// - 聚合系统提示词与各工具提示词, 便于定制、自更新与训练序列化
/// - 提示词文本全部定义在 [prompt.cpp]: 本头文件只声明成员与取值方式,
///   改文本不会牵连所有包含本头文件的编译单元
class AgentPrompt {
public:

    AgentPrompt();

    /// 系统提示词主体 (文本见 prompt.cpp)
    /// - 其中可写会话级占位符 (`${work_dir}` / `${temp_dir}` / `${session_id}`),
    ///   拼装系统消息时按会话替换 (见 [renderVars] 与 AgentContext::buildSystemPrompt)
    /// - `${work_dir}` 取会话工作目录 (不含 worktree 绑定), `${temp_dir}` 取会话临时目录
    std::string systemPrompt;

    /// 附加系统提示词表 (通用扩展点)
    /// - 键为插件或功能标识 (如 "planning"、"skill"、"codegraph")，值为主 prompt 之外的补充段
    /// - 插件通过 prompt iface 的 `appendSystemPrompts` 对象以键值形式注入/更新，卸载时按备份恢复
    /// - 最终 system 消息由 `systemPrompt` + 按键字典序拼接的附加段 + `appendSystemMessage`
    /// (skill/memory 动态) 组成
    /// - 为空时不占位，避免无对应工具时误导模型
    /// - 取值同样可写会话级占位符, 与 `systemPrompt` 一起替换
    std::map<std::string, std::string, std::less<>> appendSystemPrompts;

    /// git worktree 模式的系统提示词段
    /// - CodeAgent 初始化时 (配置启用 worktree 时) 追加到 `appendSystemPrompts["git-worktree"]`
    std::string kGitWorktreePrompt;

    /// 工具提示词表: key 为工具名, 值为该工具的 depict/args 提示词覆写
    std::map<std::string, ToolPrompt, std::less<>> toolPrompt;

    /// 会话级占位符 token (写在提示词文本中, 由 [renderVars] 替换)
    /// - 用 `${...}` 形式与本项目配置变量展开风格保持一致
    inline static constexpr std::string_view kVarWorkDir   = "${work_dir}";
    inline static constexpr std::string_view kVarTempDir   = "${temp_dir}";
    inline static constexpr std::string_view kVarSessionId = "${session_id}";

    /// 替换提示词文本中的会话级占位符
    /// - 只做固定 token 的纯文本替换, 不按 fmt 模板解析: 自定义提示词里出现
    ///   未配对的 `{` `}` (如 JSON 片段/路径) 时不会抛异常
    /// - 取值取不到 (空串) 时替换为 `unknown`, 不留 token: 让模型看到信息缺失,
    ///   也不会把空目录名写进提示词
    ///
    /// - `args`:
    ///     - [text] 待替换的提示词文本 (可含多个 token)
    ///     - [vars] 会话级取值
    ///
    /// - `return` 替换后的文本 (无 token 时内容不变)
    static std::string renderVars(std::string_view text, const PromptSessionVars& vars);

    // ----- 训练序列化辅助 -----
    // 将整个 AgentPrompt (含 toolPrompt) 序列化为 JSON, 供训练保存/加载。

    utilxx_base::Json toJson() const;

    /// 用 JSON 整体覆写当前提示词 (JSON 中缺失的字段保持不变)
    void fromJson(const utilxx_base::Json& j);

    /// 按补丁合并: 仅覆写 JSON 中出现的字段, 未出现的字段保持原样
    /// - toolPrompt 中已有工具: 仅覆写 JSON 中出现的 depict/args 子字段
    /// - toolPrompt 中尚无的工具: 插入新条目
    void mergeFromJson(const utilxx_base::Json& j);

    /// 计算整个提示词的哈希, 用于训练种群去重
    size_t promptHash() const;
};

} // namespace agent
} // namespace agentxx
