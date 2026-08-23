#include "agentxx/tools/share_store.h"

#include "agentxx/agent/context.h"
#include "fmt/format.h"
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

namespace agentxx {
namespace tools {

SessionShareStoreTool::SessionShareStoreTool(
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("agentxx_share_store", in_agentContext, false, false, 0, true) {}

std::optional<agentxx::middleware::SummarizationToolHandle>
    SessionShareStoreTool::createSummarizationToolHandle() const {
    return agentxx::middleware::SummarizationToolHandle{
        .generateDeduplicationKey = [](const neograph::json& args) -> std::optional<std::string> {
            if (args.is_object() && args["id"].is_string()) {
                auto line_offset = args.value<int64_t>("line_offset", -1);
                auto line_limit  = args.value<int64_t>("line_limit", -1);
                return fmt::format(
                    "share_store:{}:lo:{}:ll:{}",
                    args["id"].get<std::string>(),
                    line_offset,
                    line_limit
                );
            }
            return std::nullopt;
        },
        .truncateRequest =
            [](neograph::ToolCall& toolcall) {
                auto args          = neograph::json::parse(toolcall.arguments);
                args["text"]       = "[Outdated Message Truncated]";
                toolcall.arguments = args.dump();
            },
        .truncateResponse =
            [](neograph::ChatMessage& msg) {
                msg.content = "[Outdated Content truncated]";
                msg.flags
                    |= neograph::MessageFlag::ShareStoreTruncated | neograph::MessageFlag::Outdated;
            },
    };
}

neograph::ChatTool SessionShareStoreTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
                        "opt",
                        {
                            {"type", "string"},
                            {"enum",
                             neograph::json::array({
                                 "get",
                                 "insert",
                                 "set",
                                 "delete",
                             })},
                            {"description", prompt.getArg("opt")},
                        },
                    },
                    {
                        "text",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("text")},
                        },
                    },
                    {
                        "line_offset",
                        {
                            {"type", "integer"},
                            {"description", prompt.getArg("line_offset")},
                        },
                    },
                    {
                        "line_limit",
                        {
                            {"type", "integer"},
                            {"description", prompt.getArg("line_limit")},
                        },
                    },
                    {
                        "id",
                        {
                            {"type", "integer"},
                            {"description", prompt.getArg("id")},
                        },
                    },
                },
            }, {"required", neograph::json::array({"opt"})},
                       },
    };
}

asio::awaitable<std::string> SessionShareStoreTool::execute_async(const neograph::json& arguments) {
    auto session_id = arguments.value("sessionId", std::string{});
    if (session_id.empty()) {
        co_return R"({"error":"Toolcall inner error, need `sessionId`"})";
    }
    size_t text_id          = arguments.value<size_t>("id", 0);
    auto   text_line_offset = arguments.value<int64_t>("line_offset", -1);
    auto   text_line_limit  = arguments.value<int64_t>("line_limit", -1);
    auto   text             = arguments.value("text", std::string{});
    auto   text_opt         = arguments.value("opt", std::string{});
    if (text_opt.empty()) {
        co_return R"({"error":"Arg `opt` is empty"})";
    }

    // 分页切片: 对 `get` 取回的存储内容 / `set`·`insert` 的入参文本统一应用
    // line_offset/line_limit (修正: 原实现对 get 无效, 因为切片作用在空入参 text 上,
    // 而 get 分支直接返回了存储的完整内容)
    auto sliceByLine = [&](std::string input) -> std::string {
        if (text_line_offset < 0 && text_line_limit <= 0) {
            return input;
        }
        const auto offset = (text_line_offset >= 0) ? static_cast<size_t>(text_line_offset) : 0;
        const auto limit  = (text_line_limit > 0) ? static_cast<size_t>(text_line_limit)
                                                  : std::numeric_limits<size_t>::max();
        std::stringstream result{};
        size_t            lineNum = 0;
        size_t            endLine = offset;
        if (offset < std::numeric_limits<size_t>::max() - limit) {
            // 防止相加溢出回绕
            endLine = offset + limit;
        } else {
            endLine = std::numeric_limits<size_t>::max();
        }

        auto stream = std::istringstream{std::move(input)};
        for (std::string buf; lineNum < endLine; lineNum++) {
            if (!std::getline(stream, buf)) {
                // EOF/错误: getline 会先清空 buf, 此处 buf 必为空, 无需再追加
                break;
            }

            if (lineNum >= offset) {
                result << buf << "\n";
            }
        }

        if (lineNum == 0 && offset == 0) {
            // 空文本: 第 0 行视为空行, 返回空串而非报错
            return "";
        }
        if (lineNum <= offset) {
            // offset 超出文本行数
            throw std::runtime_error{fmt::format(
                R"(Arg `line_offset`({} lines) is out of range of text lines({} lines).)",
                offset,
                lineNum
            )};
        }

        return result.str();
    };

    auto agentContextPtr = agentContext.lock();
    if (!agentContextPtr || !agentContextPtr->middlewareHandleContext) {
        co_return R"({"error":"AgentContext not available"})";
    }
    // share store 桥接: 配置了 sharedShareStoreContext (同上下文子代理) 时,
    // 读写父会话的 store, 保证 id 空间一致 (如压缩子代理写入的长内容,
    // 父会话按摘要中的 id 可直接读取)
    auto mctx = agentContextPtr->agentConfig ? agentContextPtr->agentConfig->sharedShareStoreContext
                                             : nullptr;
    if (nullptr == mctx) {
        mctx = agentContextPtr->middlewareHandleContext;
    }
    if (text_opt == std::string_view{"insert"}) {
        auto reId = mctx->addShareStoreItemValue(session_id, sliceByLine(std::move(text)));
        co_return neograph::json{
            {"id", reId},
        }
            .dump();
    } else if (text_opt == std::string_view{"get"}) {
        if (text_id <= 0) {
            co_return R"({"error":"Arg `id` is empty"})";
        }
        auto result = mctx->getShareStoreItemValue(session_id, text_id);
        if (false == result.has_value()) {
            co_return R"({"error":"Not found"})";
        }
        // 分页: 对存储的完整内容按行切片返回
        co_return sliceByLine(std::move(result.value()));
    } else if (text_opt == std::string_view{"set"}) {
        if (text_id <= 0) {
            co_return R"({"error":"Arg `id` is empty"})";
        }
        mctx->setShareStoreItemValue(session_id, text_id, sliceByLine(std::move(text)));
        co_return "success";
    } else if (text_opt == std::string_view{"delete"}) {
        if (text_id <= 0) {
            co_return R"({"error":"Arg `id` is empty"})";
        }
        mctx->removeShareStoreItemValue(session_id, text_id);
        co_return "success";
    } else {
        co_return R"({"error":"Arg `opt` is invalid"})";
    }
}

} // namespace tools
} // namespace agentxx
