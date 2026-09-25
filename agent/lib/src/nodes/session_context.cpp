#include "agentxx/nodes/session_context.h"

#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/neograph_json_bridge.h"
#include <neograph/graph/node.h>

namespace agentxx {
namespace nodes {

namespace {

/// 空的会话上下文 (会话不存在时的只读兜底; 避免为读一次就创建会话)
const std::vector<neograph::ChatMessage>& emptyMessages() {
    static const std::vector<neograph::ChatMessage> kEmpty{};
    return kEmpty;
}

/// 组装上下文影子信息 (载荷与上下文大小无关: 条数/版本/角色分布/末尾消息摘要)
neograph::json buildMessagesMeta(const std::shared_ptr<agentxx::agent::Session>& session) {
    neograph::json meta = neograph::json::object();
    if (!session) {
        return meta;
    }
    const auto& msgs = session->messages();
    meta["count"]    = msgs.size();
    meta["version"]  = session->messagesVersion();

    neograph::json roleCounts = neograph::json::object();
    for (const auto& m : msgs) {
        roleCounts[m.role] = roleCounts.value(m.role, int64_t{0}) + 1;
    }
    meta["role_counts"] = std::move(roleCounts);

    if (!msgs.empty()) {
        const auto& last        = msgs.back();
        meta["last_role"]       = last.role;
        meta["last_tool_calls"] = last.tool_calls.size();
        neograph::json ids      = neograph::json::array();
        for (const auto& tc : last.tool_calls) {
            ids.push_back(tc.id);
        }
        meta["last_tool_call_ids"] = std::move(ids);
    }
    return meta;
}

} // namespace

const std::vector<neograph::ChatMessage>&
    sessionMessages(const std::shared_ptr<agentxx::agent::AgentContext>& ctx, std::string_view sessionId) {
    if (!ctx || !ctx->sessions || sessionId.empty()) {
        return emptyMessages();
    }
    auto session = ctx->sessions->get(sessionId);
    if (!session) {
        return emptyMessages();
    }
    return session->messages();
}

void updateMessagesMeta(
    const std::shared_ptr<agentxx::agent::AgentContext>& ctx,
    neograph::graph::GraphState&                         state,
    std::string_view                                     sessionId
) {
    if (!ctx || !ctx->sessions) {
        return;
    }
    if (!state.has_channel(agentxx::middleware::MiddlewareContext::channel_messagesMeta)) {
        return;
    }
    state.overwrite(
        agentxx::middleware::MiddlewareContext::channel_messagesMeta,
        buildMessagesMeta(ctx->sessions->get(sessionId))
    );
}

size_t appendSessionMessages(
    const std::shared_ptr<agentxx::agent::AgentContext>& ctx,
    const neograph::graph::NodeInput&                    in,
    std::vector<neograph::ChatMessage>                   msgs,
    std::string_view                                     nodeName
) {
    if (msgs.empty()) {
        return 0;
    }
    if (!ctx || !ctx->sessions) {
        return 0;
    }
    auto session = ctx->sessions->getOrCreate(in.ctx.thread_id);
    if (!session) {
        return 0;
    }

    // 事件载荷 (本次追加的批; 与旧引擎对 messages 通道写入发出的事件同形),
    // 需在 move 进会话前构造
    neograph::json batch = neograph::json::array();
    for (const auto& m : msgs) {
        neograph::json one;
        neograph::to_json(one, m);
        batch.push_back(std::move(one));
    }

    session->appendMessages(std::move(msgs));
    updateMessagesMeta(ctx, in.state, in.ctx.thread_id);

    if (in.stream_cb != nullptr) {
        (*in.stream_cb)(neograph::graph::GraphEvent{
            neograph::graph::GraphEvent::Type::CHANNEL_WRITE,
            nodeName.empty() ? std::string{"session"} : std::string{nodeName},
            neograph::json{
                {"channel", "messages"},
                {"value",   std::move(batch)},
            },
        });
    }
    return session->messagesCount();
}

} // namespace nodes
} // namespace agentxx
