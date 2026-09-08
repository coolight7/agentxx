#include "agentxx/tools/tool.h"
#include "agentxx/util/neograph_json_bridge.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"

#include <string>
#include <utility>

namespace agentxx {
namespace tools {

std::shared_ptr<neograph::graph::CancelToken> getSessionCancelToken(
    const std::shared_ptr<agentxx::agent::AgentContext>& agentCtx,
    const agentxx::util::Json&                           args
) {
    if (nullptr == agentCtx || nullptr == agentCtx->sessions) {
        return nullptr;
    }
    auto sessionId = args.value("sessionId", std::string{});
    if (sessionId.empty()) {
        return nullptr;
    }
    auto sess = agentCtx->sessions->get(sessionId);
    if (nullptr == sess) {
        return nullptr;
    }
    return sess->getCancelToken();
}

XXToolBase::XXToolBase(
    std::string_view                            in_name,
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext,
    bool                                        in_autoSummaryOutput,
    bool                                        in_canDelayLoad,
    size_t                                      in_maxRetry,
    bool                                        in_repeatCallCheck
) :
    name(in_name),
    agentContext(in_agentContext),
    autoSummaryOutput(in_autoSummaryOutput),
    canDelayLoad(in_canDelayLoad),
    maxRetry(in_maxRetry),
    repeatCallCheck(in_repeatCallCheck) {
    extra["autoSummaryOutput"] = autoSummaryOutput ? "true" : "false";
    extra["canDelayLoad"]      = canDelayLoad ? "true" : "false";
    extra["maxRetry"]          = std::to_string(maxRetry);
    extra["repeatCallCheck"]   = repeatCallCheck ? "true" : "false";
}

std::string XXToolBase::get_name() const {
    return name;
}

// 子类未覆写 Json 主接口时默认抛错 (基类无逻辑, 不应被直接调用)
asio::awaitable<std::string> XXToolBase::execute_async(const agentxx::util::Json&) {
    throw std::runtime_error("XXToolBase::execute_async(Json) not implemented");
    co_return "";
}

asio::awaitable<std::string> XXToolBase::execute_async(const neograph::json& arguments) {
    auto args = agentxx::util::fromNeographJson(arguments);
    co_return co_await execute_async(args);
}

std::string XXToolBase::execute(const neograph::json& arguments) {
    // 同步桥接: 独立 io_context 驱动 Json 主接口 (与原 AsyncTool::execute 语义一致)
    auto               args = agentxx::util::fromNeographJson(arguments);
    std::string        out;
    std::exception_ptr eptr;
    asio::io_context   io;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                out = co_await execute_async(args);
            } catch (...) {
                eptr = std::current_exception();
            }
        },
        asio::detached
    );
    io.run();
    if (eptr) {
        std::rethrow_exception(eptr);
    }
    return out;
}

std::optional<agentxx::middleware::SummarizationToolHandle>
    XXToolBase::createSummarizationToolHandle() const {
    return std::nullopt;
    // return agentxx::middleware::SummarizationToolHandle{
    //     .generateDeduplicationKey =
    //         [](const agentxx::util::Json &args) -> std::optional<std::string> {
    //           return "tool_name:unique_key";
    //         },
    //     .truncateRequest =
    //         [](neograph::ToolCall &toolcall) {
    //           toolcall.arguments =
    //               R"({"tip":"[Outdated Message Truncated]"})";
    //         },
    //     .truncateResponse =
    //         [](neograph::ChatMessage &msg) {
    //           msg.content = "[Outdated Content truncated]";
    //         },
    // };
}

XXToolWrap::XXToolWrap(
    std::unique_ptr<neograph::Tool>&&                           in_inner,
    std::weak_ptr<agentxx::agent::AgentContext>                 in_agentContext,
    bool                                                        in_autoSummaryOutput,
    bool                                                        in_canDelayLoad,
    size_t                                                      in_maxRetry,
    std::optional<agentxx::middleware::SummarizationToolHandle> in_summarizationHandle,
    bool                                                        in_repeatCallCheck
) :
    XXToolBase(
        in_inner->get_name(),
        in_agentContext,
        in_autoSummaryOutput,
        in_canDelayLoad,
        in_maxRetry,
        in_repeatCallCheck
    ),
    inner(std::move(in_inner)),
    summarizationHandle(in_summarizationHandle) {}

std::string XXToolWrap::get_name() const {
    return inner->get_name();
}

neograph::ChatTool XXToolWrap::get_definition() const {
    return inner->get_definition();
}

asio::awaitable<std::string> XXToolWrap::execute_async(const agentxx::util::Json& arguments) {
    // 被包装的是原始 neograph::Tool: 经桥接把 Json 转回 neograph::json 后调用
    auto neoArgs = agentxx::util::toNeographJson(arguments);
    co_return co_await inner->execute_async(neoArgs);
}

} // namespace tools
} // namespace agentxx
