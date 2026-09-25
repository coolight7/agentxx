#pragma once

#include "agentxx/agent/context.h"
#include <neograph/graph/node.h>
#include <neograph/graph/state.h>
#include <neograph/types.h>
#include <string_view>
#include <vector>

namespace agentxx {
namespace nodes {

/// 会话上下文访问辅助 (节点/中间件共用)
///
/// 背景: LLM 上下文以会话为唯一权威 (见 Session::messages 系列接口), 图状态通道
/// 不再持有上下文; 节点与中间件统一经本文件的入口读写, 不要自行拼装通道写事件。

/// 取指定会话的 LLM 上下文 (typed 只读借用; 仅 io 线程)
/// - 会话不存在时返回静态空列表 (不隐式创建会话, 避免只为读一次就构造 Session)
/// - 借用期间不得跨 co_await, 且不得同时追加/替换上下文 (引用会失效)
const std::vector<neograph::ChatMessage>&
    sessionMessages(const std::shared_ptr<agentxx::agent::AgentContext>& ctx, std::string_view sessionId);

/// 刷新图状态里的上下文影子通道 (条数/版本/角色分布/末尾消息摘要)
/// - 上下文变更点 (追加/替换/压缩) 调用, 供插件图节点观察会话上下文规模
/// - 图定义未声明该通道时为 no-op (插件可能重写了图定义)
void updateMessagesMeta(
    const std::shared_ptr<agentxx::agent::AgentContext>& ctx,
    neograph::graph::GraphState&                         state,
    std::string_view                                     sessionId
);

/// 追加消息到会话上下文, 并发出与旧 `messages` 通道写入同形的图事件
/// - 事件 (`{"channel":"messages","value":[...]}` 的 CHANNEL_WRITE) 由 EventBridge
///   转成 view 消息增量 (UI 展示) 并请求节流持久化; 事件在追加完成后发出,
///   保证持久化看到的上下文已包含本批消息
/// - 图状态不再写入上下文, 因此该事件是 UI/持久化侧的唯一触发源
/// - 无 stream_cb (headless 直接运行) 时只做追加
///
/// - `args`:
///     - [ctx] 当前 agent 上下文 (会话表来源)
///     - [in] 节点输入 (取 thread_id 与 stream_cb)
///     - [msgs] 本次追加的消息 (为空时不发事件也不变更版本)
///     - [nodeName] 事件上报的节点名; 为空时记为 "session"
///
/// - `return` 追加后的上下文条数
size_t appendSessionMessages(
    const std::shared_ptr<agentxx::agent::AgentContext>& ctx,
    const neograph::graph::NodeInput&                    in,
    std::vector<neograph::ChatMessage>                   msgs,
    std::string_view                                     nodeName = {}
);

} // namespace nodes
} // namespace agentxx
