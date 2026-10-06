#include "agentxx/nodes/session_context.h"

#include "agentxx/agent/session_store.h"
#include "agentxx/agent/io/agent_io.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/neograph_json_bridge.h"
#include "fmt/format.h"
#include "utilxx_base/log.h"
#include <chrono>
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

    // 最后一条 assistant 消息的 tool_calls 条数: 循环路由条件
    // `xx_has_tool_calls` 的取值来源 (语义与 neograph 内置 has_tool_calls 一致:
    // 从末尾回溯第一条 assistant 消息)
    size_t lastAssistantToolCalls = 0;
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
        if (it->role == "assistant") {
            lastAssistantToolCalls = it->tool_calls.size();
            break;
        }
    }
    meta["last_assistant_tool_calls"] = lastAssistantToolCalls;

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
    // 统计只扫描角色字符串 (不拷贝正文), 产物是固定几个字段的对象;
    // 因此图状态载荷与上下文大小无关, 每次上下文变更的代价也只是一次轻量遍历
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

namespace {

/// 标记持久化收件箱条目已投递 (promoted); 未启用持久化/无 id 时为 no-op
void markSessionInputPromoted(
    const std::shared_ptr<agentxx::agent::AgentContext>& ctx,
    std::string_view                                     sessionId,
    std::string_view                                     inputId,
    uint64_t                                             seq
) {
    if (!ctx || !ctx->sessions || !ctx->sessions->sessionStore || inputId.empty()) {
        return;
    }
    ctx->sessions->sessionStore->markSessionInputPromoted(sessionId, inputId, seq);
}

/// 构造一条来源化的 user 消息 (来源写入 extra, 供诊断/审计读取)
neograph::ChatMessage makeInjectedUserMessage(const agentxx::agent::SessionPendingInput& input) {
    utilxx_base::Json j{
        {"role",    "user"          },
        {"content", input.text      },
    };
    neograph::ChatMessage msg;
    neograph::from_json(agentxx::util::toNeographJson(j), msg);
    neograph::json extra = neograph::json::object();
    extra["input_source"]   = input.source.empty() ? std::string{"user"} : input.source;
    extra["input_delivery"] = input.delivery;
    if (!input.id.empty()) {
        extra["input_id"] = input.id;
    }
    msg.extra = std::move(extra);
    return msg;
}

/// 追加一条展示消息 (user 输入) 并通知 UI
/// - EventBridge 只展开 assistant/tool 角色, user 消息需调用方自行落到展示历史,
///   否则 UI 看不到注入的输入 (只出现在 LLM 上下文里)
void appendUserViewMessage(
    const std::shared_ptr<agentxx::agent::Session>& session,
    const agentxx::agent::SessionPendingInput&      input
) {
    if (!session) {
        return;
    }
    const auto nowMs = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        )
            .count()
    );
    auto vm = agentxx::agent::ViewMessage::makeText(
        agentxx::agent::ViewMessage::Role::User,
        input.text,
        input.createdAtMs > 0 ? input.createdAtMs : nowMs
    );
    vm.id = session->appendViewMessage(vm);
    if (!session->io) {
        return;
    }
    auto delta = agentxx::agent::WireDelta{
        .message = std::make_shared<agentxx::agent::ViewMessage>(std::move(vm)),
        .type    = agentxx::agent::WireDelta::Type::InsertMessage,
    };
    delta.seq = session->nextDeltaSeq();
    session->io->sendToPeer(std::move(delta));
}

} // namespace

PendingInjections drainPendingSessionInputs(
    const std::shared_ptr<agentxx::agent::AgentContext>& ctx,
    const neograph::graph::NodeInput&                    in,
    std::string_view                                     sessionId,
    std::string_view                                     nodeName
) {
    PendingInjections out;
    if (!ctx || !ctx->sessions || sessionId.empty()) {
        return out;
    }
    auto session = ctx->sessions->get(sessionId);
    if (!session) {
        return out;
    }
    if (session->pendingInputCount() == 0) {
        return out;
    }

    // next-step: 写入权威上下文 (安全边界 = 请求装配前, 不需打断流式输出)
    auto steps = session->takePendingInputs(agentxx::agent::InputDelivery::NextStep);
    if (!steps.empty()) {
        std::vector<neograph::ChatMessage> msgs;
        msgs.reserve(steps.size());
        for (const auto& input : steps) {
            msgs.push_back(makeInjectedUserMessage(input));
        }
        appendSessionMessages(ctx, in, std::move(msgs), nodeName);
        for (const auto& input : steps) {
            appendUserViewMessage(session, input);
            markSessionInputPromoted(ctx, sessionId, input.id, session->deltaSeq);
        }
        out.promotedSteps = steps.size();
        XX_LOGI(
            "[session_context] promoted {} next-step input(s) at modelcall boundary (session={})",
            steps.size(),
            sessionId
        );
    }

    // inject: 仅本次请求可见 (不改写权威上下文)
    auto injects = session->takePendingInputs(agentxx::agent::InputDelivery::Inject);
    for (auto& input : injects) {
        auto msg = makeInjectedUserMessage(input);
        if (!input.source.empty() && input.source != "user") {
            msg.content = fmt::format("[{}] {}", input.source, msg.content);
        }
        out.requestScoped.push_back(std::move(msg));
        markSessionInputPromoted(ctx, sessionId, input.id, session->deltaSeq);
    }
    if (!injects.empty()) {
        XX_LOGI(
            "[session_context] attached {} injected input(s) to this request (session={})",
            injects.size(),
            sessionId
        );
    }
    return out;
}

} // namespace nodes
} // namespace agentxx
