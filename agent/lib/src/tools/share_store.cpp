#include "agentxx/tools/share_store.h"

#include "agentxx/agent/context.h"
#include "fmt/format.h"
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

namespace agentxx {
namespace tools {

ThreadShareStoreTool::ThreadShareStoreTool(
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("agentxx_share_store", in_agentContext, false, false) {}

std::optional<agentxx::middleware::SummarizationToolHandle>
    ThreadShareStoreTool::createSummarizationToolHandle() const {
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

neograph::ChatTool ThreadShareStoreTool::get_definition() const {
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
                            {"type", "number"},
                            {"description", prompt.getArg("line_offset")},
                        },
                    },
                    {
                        "line_limit",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("line_limit")},
                        },
                    },
                    {
                        "id",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("id")},
                        },
                    },
                },
            }, {"required", neograph::json::array({"opt"})},
                       },
    };
}

asio::awaitable<std::string> ThreadShareStoreTool::execute_async(const neograph::json& arguments) {
    auto thread_id = arguments.value("thread_id", std::string{});
    if (thread_id.empty()) {
        co_return R"({"error":"Toolcall inner error, need `thread_id`"})";
    }
    size_t text_id          = arguments.value<size_t>("id", 0);
    auto   text_line_offset = arguments.value<int64_t>("line_offset", -1);
    auto   text_line_limit  = arguments.value<int64_t>("line_limit", -1);
    auto   text             = arguments.value("text", std::string{});
    auto   text_opt         = arguments.value("opt", std::string{});
    if (text_opt.empty()) {
        co_return R"({"error":"Arg `opt` is empty"})";
    }

    if (text_line_offset >= 0 || text_line_limit > 0) {
        const auto offset = (text_line_offset >= 0) ? static_cast<size_t>(text_line_offset) : 0;
        const auto limit  = (text_line_limit > 0) ? static_cast<size_t>(text_line_limit)
                                                  : std::numeric_limits<size_t>::max();
        auto       stream = std::istringstream{text};
        std::stringstream result{};
        size_t            lineNum = 0;
        size_t            endLine = offset;
        if (offset < std::numeric_limits<size_t>::max() - limit) {
            // 防止相加溢出回绕
            endLine = offset + limit;
        } else {
            endLine = std::numeric_limits<size_t>::max();
        }

        for (std::string buf; lineNum < endLine; lineNum++) {
            if (!std::getline(stream, buf)) {
                // EOF/错误: getline 会先清空 buf, 此处 buf 必为空, 无需再追加
                break;
            }

            if (lineNum >= offset) {
                result << buf << "\n";
            }
        }

        if (lineNum <= offset) {
            // offset 超出文件行数
            throw std::runtime_error{fmt::format(
                R"(Arg `line_offset`({} lines) is out of range of file lines({} lines).)",
                offset,
                lineNum
            )};
        }

        text = result.str();
    }

    auto agentContextPtr = agentContext.lock();
    if (text_opt == std::string_view{"insert"}) {
        auto reId
            = agentContextPtr->middlewareHandleContext->addShareStoreItemValue(thread_id, text);
        co_return neograph::json{
            {"id", reId},
        }
            .dump();
    } else if (text_opt == std::string_view{"get"}) {
        if (text_id <= 0) {
            co_return R"({"error":"Arg `id` is empty"})";
        }
        auto result
            = agentContextPtr->middlewareHandleContext->getShareStoreItemValue(thread_id, text_id);
        co_return result.value_or(R"({"error":"Not found"})");
    } else if (text_opt == std::string_view{"set"}) {
        if (text_id <= 0) {
            co_return R"({"error":"Arg `id` is empty"})";
        }
        agentContextPtr->middlewareHandleContext->setShareStoreItemValue(thread_id, text_id, text);
        co_return "success";
    } else if (text_opt == std::string_view{"delete"}) {
        if (text_id <= 0) {
            co_return R"({"error":"Arg `id` is empty"})";
        }
        agentContextPtr->middlewareHandleContext->removeShareStoreItemValue(thread_id, text_id);
        co_return "success";
    } else {
        co_return R"({"error":"Arg `opt` is invalid"})";
    }
}

} // namespace tools
} // namespace agentxx
