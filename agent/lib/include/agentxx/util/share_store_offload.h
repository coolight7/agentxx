#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace agentxx {
namespace agent {
class AgentContext;
}
namespace util {

/// 单条文本卸载到会话 share store 的结果 (见 [offloadShareStoreIfOversized])
struct ShareStoreOffload {
    /// 是否发生了卸载 (false = 内容未达阈值, [text] 即输入原文)
    bool   offloaded = false;
    /// 原文在 share store 中占用的 id (未卸载时为 0)
    size_t storeId = 0;
    /// 定稿文本: 未卸载时与输入原文相同; 卸载时为 "取回提示 + 开头片段"
    std::string text;
};

/// 单条文本过长时截断, 并把原文存入会话 share store
///
/// 同一套判定与提示格式的两处用途 (共用本实现, 避免两处规则漂移):
/// - toolcall 输出压缩: 见 [agentxx::nodes::ToolcallWrapNode::finalizeToolCall]
/// - agent 队列输入进入轮次时: 见 [agentxx::agent::BaseAgent::runTurnAsync]
///
/// 判定按 **utf8 字符数**: 内容达到 [limitLength] 个字符时把原文存入 share store,
/// 返回的文本只保留开头片段与一行提示 —— 提示里给出 id、总行数与可见行范围,
/// 模型据此用 `agentxx_share_store` 工具按行取回剩余内容。
///
/// 截断分两种 (与 share store 按行分页的能力对齐):
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

} // namespace util
} // namespace agentxx
