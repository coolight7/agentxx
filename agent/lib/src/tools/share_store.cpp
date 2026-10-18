#include "agentxx/tools/share_store.h"
#include "agentxx/util/neograph_json_bridge.h"

#include "agentxx/agent/context.h"
#include "fmt/format.h"
#include "utilxx_base/json.h"
#include <charconv>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace agentxx {
namespace tools {

namespace {

/// 解析 `id` 参数 (声明为字符串: 模型传数字时主程序已按模式改成字符串;
/// 这里同时接受数字与纯数字字符串, 便于直接调用实现体的场合)
/// - 缺失/空值返回 0 (调用方按"未给 id"处理); 非数字字符串报参数错误
inline size_t parseShareStoreId(const utilxx_base::Json& arguments) {
    const auto& id = arguments["id"];
    if (id.is_string()) {
        const auto s     = id.get<std::string>();
        size_t     value = 0;
        auto [ptr, ec]   = std::from_chars(s.data(), s.data() + s.size(), value);
        if (ec != std::errc{} || ptr != s.data() + s.size()) {
            if (s.empty()) {
                return 0;
            }
            throw std::invalid_argument{fmt::format("Arg `id` is not a valid ID: {}", s)};
        }
        return value;
    }
    if (id.is_number()) {
        return static_cast<size_t>(id.get<double>());
    }
    return 0;
}

} // namespace

SessionShareStoreTool::SessionShareStoreTool(
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("agentxx_share_store", in_agentContext, false, false, 0, true) {}

std::optional<agentxx::middleware::SummarizationToolHandle>
    SessionShareStoreTool::createSummarizationToolHandle() const {
    return agentxx::middleware::SummarizationToolHandle{
        .generateDeduplicationKey
        = [](const utilxx_base::Json& args) -> std::optional<std::string> {
            if (args.is_object() && args["id"].is_string()) {
                auto offset = args.value<int64_t>("offset", -1);
                auto limit  = args.value<int64_t>("limit", -1);
                return fmt::format(
                    "share_store:{}:offset:{}:limit:{}",
                    args["id"].get<std::string>(),
                    offset,
                    limit
                );
            }
            return std::nullopt;
        },
        .truncateRequest =
            [](neograph::ToolCall& toolcall) {
                auto args          = utilxx_base::Json::parse(toolcall.arguments);
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

    utilxx_base::Json params = utilxx_base::Json{
        {"type", "object"},
        {
         "properties", {
                {
                    "opt",
                    {
                        {"type", "string"},
                        {"enum",
                         utilxx_base::Json::array({
                             "get",
                             "new",
                             "update",
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
                    "offset",
                    {
                        {"type", "integer"},
                        {"description", prompt.getArg("offset")},
                    },
                },
                {
                    "limit",
                    {
                        {"type", "integer"},
                        {"description", prompt.getArg("limit")},
                    },
                },
                {
                    "id",
                    {
                        {"type", "string"},
                        {"description", prompt.getArg("id")},
                    },
                },
            }, },
        {"required", utilxx_base::Json::array({"opt"})},
    };
    return {get_name(), prompt.depict, agentxx::util::toNeographJson(params)};
}

asio::awaitable<std::string> SessionShareStoreTool::execute_async(const utilxx_base::Json& arguments
) {
    // 参数检查失败一律抛异常 (由 ToolcallWrapNode 统一转成工具错误结果),
    // 不再返回编码后的错误 JSON: 错误与正常结果不会混在同一形态里
    auto session_id = arguments.value("sessionId", std::string{});
    if (session_id.empty()) {
        throw std::invalid_argument{"Toolcall inner error, need `sessionId`"};
    }
    // 已改名的分页参数直接报错: 静默忽略会让调用方以为取到了指定行区间
    if (arguments.contains("line_offset") || arguments.contains("line_limit")) {
        throw std::invalid_argument{
            "Args `line_offset`/`line_limit` are renamed to `offset`/`limit`"
        };
    }
    size_t text_id          = parseShareStoreId(arguments);
    auto   text_line_offset = arguments.value<int64_t>("offset", -1);
    auto   text_line_limit  = arguments.value<int64_t>("limit", -1);
    auto   text             = arguments.value("text", std::string{});
    auto   text_opt         = arguments.value("opt", std::string{});
    if (text_opt.empty()) {
        throw std::invalid_argument{"Arg `opt` is empty"};
    }

    // 分页切片: 只用于 `get` 取回的存储内容 —— 按行返回 [offset, offset+limit) 区间;
    // `new`/`update` 的入参文本原样存储, `offset`/`limit` 对它们无效 (与工具描述一致)
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
                R"(Arg `offset`({} lines) is out of range of text lines({} lines).)",
                offset,
                lineNum
            )};
        }

        return result.str();
    };

    auto agentContextPtr = agentContext.lock();
    if (!agentContextPtr || !agentContextPtr->middlewareHandleContext) {
        throw std::runtime_error{"AgentContext not available"};
    }
    // share store 桥接: 配置了 sharedShareStoreContext (同上下文子代理) 时,
    // 读写父会话的 store, 保证 id 空间一致 (如压缩子代理写入的长内容,
    // 父会话按摘要中的 id 可直接读取)
    auto mctx = agentContextPtr->agentConfig ? agentContextPtr->agentConfig->sharedShareStoreContext
                                             : nullptr;
    if (nullptr == mctx) {
        mctx = agentContextPtr->middlewareHandleContext;
    }
    if (text_opt == std::string_view{"new"}) {
        auto reId = mctx->addShareStoreItemValue(session_id, std::move(text));
        co_return utilxx_base::Json{
            {"id", reId},
        }
            .dump();
    } else if (text_opt == std::string_view{"get"}) {
        if (text_id <= 0) {
            throw std::invalid_argument{"Arg `id` is empty"};
        }
        auto result = mctx->getShareStoreItemValue(session_id, text_id);
        if (false == result.has_value()) {
            // id 不存在 (参数指向的内容缺失): 与参数检查同样按错误抛出
            throw std::invalid_argument{fmt::format("Share store item not found: id={}", text_id)};
        }
        // 分页: 对存储的完整内容按行切片返回
        co_return sliceByLine(std::move(result.value()));
    } else if (text_opt == std::string_view{"update"}) {
        if (text_id <= 0) {
            throw std::invalid_argument{"Arg `id` is empty"};
        }
        mctx->setShareStoreItemValue(session_id, text_id, std::move(text));
        co_return "success";
    } else {
        throw std::invalid_argument{fmt::format("Arg `opt` is invalid: {}", text_opt)};
    }
}

} // namespace tools
} // namespace agentxx
