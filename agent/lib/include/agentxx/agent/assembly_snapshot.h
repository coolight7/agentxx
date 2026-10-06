#pragma once

#include "agentxx/agent/config.h"
#include "utilxx_base/json.h"
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace agent {

class AgentContext;

/// 启动装配快照 (计划 ARC-6 / TOOL-12 / CFG-3 / PLG-10)
///
/// 目的: 让"这次启动到底装配成了什么"变成可检查、可打印的对象, 而不是散落在
/// 十几条日志里。回答的问题包括:
/// - 配置来自哪里 (配置文件 / 环境变量 / 默认值), 哪些键被忽略或即将被修正
/// - 当前生效的模型、中间件顺序、工具清单 (来源 / 开关 / 被过滤原因)
/// - 插件装载结果 (是否启用、被依赖阻塞、注册了什么、装载耗时)
/// - 执行图名称与节点数、持久化是否开启、会话根目录
///
/// 两个来源分离, 便于在"没有可用模型"时也能打印配置侧快照:
/// - [buildConfigSnapshot]: 只依赖 AgentConfig, 不构造 agent
/// - [buildRuntimeSnapshot]: 需要 init() 之后的 AgentContext

/// 配置侧装配快照 (不依赖 agent 实例)
/// - `agentxx_cli --dump-config` 与启动日志共用同一份实现
/// - API key 只输出"是否已设置", 不输出内容
utilxx_base::Json buildConfigSnapshot(const AgentConfig& config);

/// 运行侧装配快照 (init() 之后)
/// - 模型注册表 / 中间件顺序 / 工具清单 (含来源与过滤原因) / 插件状态 /
///   执行图 / 持久化 / 已加载组件 (skill/memory/mcp) 与加载失败项
utilxx_base::Json buildRuntimeSnapshot(const AgentContext& ctx);

/// 合并配置侧与运行侧快照 (运行侧键覆盖配置侧同名键, 配置侧独有的保留)
utilxx_base::Json
    mergeAssemblySnapshot(const utilxx_base::Json& configSnapshot, const utilxx_base::Json& runtimeSnapshot);

/// 渲染为人工可读的文本行
/// - 启动日志与 `--dump-config` 使用同一份渲染实现, 两者不会漂移
std::vector<std::string> renderAssemblySnapshot(const utilxx_base::Json& snapshot);

/// 启动期记录装配快照
/// - Summary 级: 一行计数摘要 (模型/中间件/工具/插件/图/持久化)
/// - Debug 级: 完整 JSON, 排查时按需打开
void logAssemblySnapshot(const utilxx_base::Json& snapshot, std::string_view stage);

/// 推断工具来源: `middleware:<名>` / `plugin:<插件名>` / `mcp:<命名空间>` / `builtin`
/// - 中间件工具在收集时已写入 [AgentContext::toolSourceHints], 优先取该值
std::string resolveToolSource(const AgentContext& ctx, std::string_view toolName);

} // namespace agent
} // namespace agentxx
