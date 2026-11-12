#pragma once

#include "agentxx/util/tool_result.h"

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace agentxx {
namespace agent {
class AgentContext;
}

namespace util {

/// 卸载提示的首行前缀
/// - 提示文本由 [offloadShareStoreIfOversized] 生成, 界面侧靠这个前缀把提示行
///   当成"非内容"跳过 (例如事件折叠头只预览真实内容)
/// - 提示行本身固定作为第一行输出 (后面接开头片段)
inline constexpr std::string_view kOffloadHintPrefix = "[Content offloaded.";

/// 单条文本卸载到会话 share store 的结果 (见 [offloadShareStoreIfOversized])
struct ShareStoreOffload {
    /// 是否发生了卸载 (false = 内容未达阈值, [text] 即输入原文)
    bool offloaded = false;
    /// 原文在 share store 中占用的 id (未卸载时为 0)
    size_t storeId = 0;
    /// 定稿文本: 未卸载时与输入原文相同; 卸载时为 "取回提示 + 开头片段"
    std::string text;
};

/// 单条文本过长时截断, 并把原文存入会话 share store
///
/// 同一套判定与提示格式的三处用途 (共用本实现, 避免规则不一致):
/// - toolcall 输出压缩: 见 [agentxx::nodes::ToolcallWrapNode::finalizeToolCall]
/// - agent 队列输入进入轮次时: 见 [agentxx::agent::BaseAgent::runTurnAsync]
/// - next-step / inject 注入进入上下文/请求时: 见
///   [agentxx::nodes::drainPendingSessionInputs]
///
/// 判定按 **utf8 字符数**: 内容达到 [limitLength] 个字符时把原文存入 share store,
/// 返回的文本只保留开头片段与一行提示 —— 提示里给出 id、总行数与可见行范围,
/// 模型据此用 `agentxx_share_store` 工具按行取回剩余内容。
///
/// 截断分两种 (与 share store 按行分页的方式一致):
/// - 按行截断后还剩得下内容 (最后一个换行符落在片段的前 1/3 之后): 截到最后一条
///   完整行, 提示给出 `show [1, N], hide [N+1, 总行数]`
/// - 内容集中在少数行 (例如只有一行但特别长, 按行截断几乎不剩内容): 按字符截断,
///   提示改为让模型直接用 id 取全文 (再自行按行分页)
///
/// - `args`:
///     - [agentContext] 会话所在的 agent 上下文 (为空或没有中间件上下文时不做卸载)
///     - [sessionId]    会话 id (为空时不做卸载: share store 按会话隔离)
///     - [text]         待检查的文本
///     - [limitLength]  阈值 (utf8 字符数; 0 = 不检查, 直接返回原文)
/// - `return` 见 [ShareStoreOffload]; 无法卸载 (无上下文/无会话 id/存储分配失败) 时
///   [ShareStoreOffload::offloaded] 为 false 且原样返回文本
ShareStoreOffload offloadShareStoreIfOversized(
    const std::shared_ptr<agentxx::agent::AgentContext>& agentContext,
    std::string_view                                     sessionId,
    std::string_view                                     text,
    size_t                                               limitLength
);

/// 工具输出过长时转存会话 share store (前台工具调用与后台工具任务共用的唯一实现)
///
/// 是否压缩由**工具自己的声明**决定, 与它在哪条路径上执行无关:
/// - 工具声明了自动压缩 (`neograph::Tool::extra["autoSummaryOutput"] == "true"`,
///   来源 [agentxx::tools::XXToolBase::autoSummaryOutput]) 才动手;
/// - 阈值 = [agentxx::agent::AgentConfig::toolcallSummaryLimitOutputLength]
///   (utf8 字符数, 0 = 不压缩);
/// - 压缩规则与提示格式见 [offloadShareStoreIfOversized]。
///
/// 调用点:
/// - 前台工具调用: [agentxx::nodes::ToolcallWrapNode::finalizeToolCall]
/// - 后台工具任务 (`agentxx_async_run` 的 `kind=tool`): 后台任务执行的工具调用
///   (`runToolWork`), 这样同一段输出无论前后台都只被裁剪一次; 事件投递侧对
///   `kind=tool` 的结果不再整段卸载 (见 [agentxx::events::EventHub])
///
/// - `args`:
///     - [agentContext] 会话所在的 agent 上下文 (为空或没有配置时不做压缩)
///     - [toolExtra]    被调用工具的 `extra` 开关表 (`neograph::Tool::extra`)
///     - [sessionId]    会话 id (空 = 不压缩: share store 按会话隔离)
///     - [result]       工具结果, 就地把 `text` 换成压缩后的文本 (状态位不变)
/// - `return` 是否发生了压缩
bool offloadToolResultIfOversized(
    const std::shared_ptr<agentxx::agent::AgentContext>& agentContext,
    const std::map<std::string, std::string>&            toolExtra,
    std::string_view                                     sessionId,
    ToolResult&                                          result
);

} // namespace util
} // namespace agentxx
