#include "agentxx/nodes/toolcall.h"

#include "agentxx/middlewares/permission.h"
#include "agentxx/plugin/tool_registry.h"
#include "agentxx/tools/tool.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "fmt/format.h"
#include <algorithm>
#include <cassert>
#include <charconv>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>

namespace agentxx {
namespace nodes {
namespace {

/// 解析字符串为数值: 容忍首尾空白与一个前导 '+', 要求整个字符串被完整解析
/// (std::from_chars 不接受空白与前导 '+', 且允许部分解析, 这里做严格校验)
template<typename T>
bool parseFullNumber(std::string_view s, T& out) {
    size_t b = 0, e = s.size();
    while (b < e && agentxx::util::charIsSpace(s[b])) {
        ++b;
    }
    while (e > b && agentxx::util::charIsSpace(s[e - 1])) {
        --e;
    }
    s = s.substr(b, e - b);
    if (s.empty()) {
        return false;
    }
    if (s.front() == '+') {
        s.remove_prefix(1);
    }
    if (s.empty()) {
        return false;
    }
    // 浮点 from_chars (C++23+) 支持 0x 十六进制浮点写法, 这里拒绝, 避免 "0x10" 被
    // 意外解析为 16.0 (LLM 可能输出十六进制文本, 不应视为十进制数值)
    if constexpr (std::is_floating_point_v<T>) {
        if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            return false;
        }
    }
    T    v;
    auto result = std::from_chars(s.data(), s.data() + s.size(), v);
    if (result.ec == std::errc{} && result.ptr == s.data() + s.size()) {
        out = v;
        return true;
    }
    return false;
}

/// 数值 json -> 十进制字符串 (整数用 to_string, 浮点用最短可往返表示)
std::string numberToJsonString(const neograph::json& v) {
    if (v.is_number_unsigned()) {
        return std::to_string(v.get<unsigned long long>());
    }
    if (v.is_number_integer()) {
        return std::to_string(v.get<long long>());
    }
    return fmt::format("{}", v.get<double>());
}

/// 尝试把 double 无损转换为 long long:
/// 仅当值为整数值 (如 3.0) 且在 int64 表示范围内时才成功; NaN/Inf/3.5 等返回 false
bool tryDoubleToInt64(double v, long long& out) {
    const double t = std::trunc(v);
    if (t != v) {
        // 非整数值; NaN 也在此被拒绝 (NaN != NaN 恒为真)
        return false;
    }
    // 边界用可被 double 精确表示的 ±2^63 比较, 避免越界 static_cast 的未定义行为
    // (-2^63 为闭区间下界, +2^63 为开区间上界)
    constexpr double kInt64Min   = -9223372036854775808.0;
    constexpr double kInt64MaxP1 = 9223372036854775808.0;
    if (!(v >= kInt64Min && v < kInt64MaxP1)) {
        return false;
    }
    out = static_cast<long long>(v);
    return true;
}

/// schema 声明的类型集合
struct SchemaTypes {
    bool isArray   = false;
    bool isString  = false;
    bool isNumber  = false; // 声明了 "number"
    bool isInteger = false; // 声明了 "integer"
    bool isBool    = false;

    /// 是否声明了数值类型 (number 或 integer)
    bool isNumeric() const {
        return isNumber || isInteger;
    }
};

SchemaTypes getSchemaTypes(const neograph::json& schema) {
    SchemaTypes t;
    if (!schema.is_object()) {
        return t;
    }
    auto check = [&t](std::string_view one) {
        if ("array" == one) {
            t.isArray = true;
        } else if ("string" == one) {
            t.isString = true;
        } else if ("number" == one) {
            t.isNumber = true;
        } else if ("integer" == one) {
            t.isInteger = true;
        } else if ("boolean" == one) {
            t.isBool = true;
        }
    };
    auto type = schema.value("type", std::string{});
    if (!type.empty()) {
        check(type);
    } else if (schema.contains("type") && schema["type"].is_array()) {
        // 联合类型, 如 {"type": ["array", "string"]}
        for (const auto& item : schema["type"]) {
            if (item.is_string()) {
                check(item.get<std::string>());
            }
        }
    }
    return t;
}

/// 数组元素是否允许为字符串 (items 未声明或声明为 string 时视为字符串数组)
bool isStringArrayItems(const neograph::json& schema) {
    auto items = schema["items"];
    if (items.is_object()) {
        auto itemType = items.value("type", std::string{});
        if (!itemType.empty() && "string" != itemType) {
            return false;
        }
    }
    return true;
}

} // namespace

/// 根据 tool 的参数 JSON Schema 自动修正参数类型兼容性, 尽量让 arg 类型匹配参数需求:
/// - string -> 字符串数组: 参数声明为数组 (字符串数组) 而传入单个字符串时, 包装为 `[str]`
/// - string -> number/integer: 参数声明为数值而传入字符串时, 若字符串可完整解析为数值则转换
///   (integer 仅接受整数写法; number 支持小数/指数; 前导 '+', 首尾空白会被容忍)
/// - number/integer -> string: 参数声明为字符串而传入数值时, 转为十进制字符串
/// - number(double) <-> integer: 参数声明的数值类型与传入数值类型不同时互相转换
///   (integer -> number 转为 double; number -> integer 仅当浮点值恰为整数值且在
///   int64 表示范围内才无损转换, 如 3.0 转 3, 3.5 保持原样)
/// - bool -> string / string("true"/"false") -> boolean: 布尔与字符串互相转换
/// - [单字符串数组] -> string: 参数声明为字符串而传入单元素字符串数组时, 解包为字符串
/// - 仅当目标类型不包含 arg 当前类型时转换; 无法解析或类型不明确时保持原样
/// `return` 是否发生了参数转换
bool ToolcallWrapNode::autoFixArgsType(const neograph::ChatTool& def, neograph::json& args) {
    if (!args.is_object()) {
        return false;
    }
    const auto& params = def.parameters;
    if (!params.is_object()) {
        return false;
    }
    const auto& props = params["properties"];
    if (!props.is_object()) {
        return false;
    }

    bool changed = false;
    for (const auto& [name, schema] : props.items()) {
        // 跳过未传值的参数
        if (!args.contains(name)) {
            continue;
        }
        const auto& arg   = args[name];
        const auto  types = getSchemaTypes(schema);
        // 记录到日志的转换信息
        std::string fixInfo;

        if (arg.is_string()) {
            auto str = arg.get<std::string>();
            // 1) string -> 字符串数组
            if (types.isArray && isStringArrayItems(schema)) {
                auto arr = neograph::json::array();
                arr.push_back(arg);
                args[name] = std::move(arr);
                fixInfo    = "string -> [string]";
                changed    = true;
            } else if (types.isNumeric() && !types.isString) {
                // 2) string -> number/integer (目标不含 string 时)
                if (types.isNumber) {
                    // 声明了 number: 按浮点解析 (兼容小数/指数)
                    double v = 0;
                    if (parseFullNumber(str, v)) {
                        args[name] = v;
                        fixInfo    = "string -> number";
                        changed    = true;
                    }
                } else {
                    // 仅声明 integer: 只接受纯整数写法 ("3.5" 不转换)
                    long long v = 0;
                    if (parseFullNumber(str, v)) {
                        args[name] = v;
                        fixInfo    = "string -> integer";
                        changed    = true;
                    }
                }
            } else if (types.isBool && !types.isString) {
                // 3) string -> boolean (目标不含 string 时)
                if ("true" == str) {
                    args[name] = true;
                    fixInfo    = "string -> boolean";
                    changed    = true;
                } else if ("false" == str) {
                    args[name] = false;
                    fixInfo    = "string -> boolean";
                    changed    = true;
                }
            }
        } else if (arg.is_number()) {
            // number/integer -> string (目标不含数值时)
            if (types.isString && !types.isNumeric()) {
                args[name] = numberToJsonString(arg);
                fixInfo    = "number -> string";
                changed    = true;
            } else if (arg.is_number_float()) {
                // number(double) -> integer: 目标声明 integer 时, 仅当浮点值恰为
                // 整数值且在 int64 表示范围内才无损转换 (如 LLM 传 3.0 而工具要
                // integer); 3.5/越界值等保持原样
                if (types.isInteger && !types.isString) {
                    long long v = 0;
                    if (tryDoubleToInt64(arg.get<double>(), v)) {
                        args[name] = v;
                        fixInfo    = "number -> integer";
                        changed    = true;
                    }
                }
            } else {
                // integer -> number(double): 工具声明 number 即期望 double 类型
                // (联合类型含 string 或已声明 integer 时 arg 已合法, 不转换)
                if (types.isNumber && !types.isInteger && !types.isString) {
                    args[name] = arg.get<double>();
                    fixInfo    = "integer -> number";
                    changed    = true;
                }
            }
        } else if (arg.is_bool()) {
            // bool -> string (目标不含 boolean 时)
            if (types.isString && !types.isBool) {
                args[name] = arg.get<bool>() ? std::string{"true"} : std::string{"false"};
                fixInfo    = "bool -> string";
                changed    = true;
            }
        } else if (arg.is_array()) {
            // [单字符串数组] -> string (目标不含 array 时)
            if (types.isString && !types.isArray && arg.size() == 1 && arg[0].is_string()) {
                args[name] = arg[0].get<std::string>();
                fixInfo    = "[string] -> string";
                changed    = true;
            }
        }

        if (!fixInfo.empty()) {
            XX_LOGD("Toolcall auto-fix arg type: `{}` {} for tool {}", name, fixInfo, def.name);
        }
    }
    return changed;
}

ToolcallWrapNode::ToolcallWrapNode(
    std::string_view                            in_name,
    const neograph::graph::NodeContext&         in_ctx,
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    WrapHandleBaseNode<neograph::graph::ToolDispatchNode>(in_name, in_agentContext, in_ctx) {
    // 拦截所有普通异常: 工具调用/中间件回调异常由 onHandle*Error 插入错误消息,
    // 图继续调度, 让 agent 基于错误消息继续运行 (工具调用失败是常态, 不应终止会话);
    // 取消/中断/取消信号 仍重抛, 由 base_agent 按控制流处理
    interceptOrdinaryError_ = true;
}

void ToolcallWrapNode::onHandleStartError(
    bool                                                errorRethrow,
    bool                                                isCurrentError,
    std::string_view                                    exceptionStr,
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    neograph::graph::NodeInput&                         in,
    neograph::graph::NodeOutput&                        result
) noexcept {
    // START 出错，不运行 execTool，直接替换插入消息，保证消息顺序正确
    if (false == errorRethrow && isCurrentError) {
        // 回填 tool_call_id/tool_name，确保 ToolEnd 能正确关联
        auto  messages = in.state.get_messages();
        auto* assistant_msg
            = agentxx::middleware::BaseMiddlewareHandleInterface::getLastAssistantToolcallMessage(
                messages
            );
        if (assistant_msg && !assistant_msg->tool_calls.empty()) {
            auto appendToolResult = neograph::json::array();
            for (const auto& tool : assistant_msg->tool_calls) {
                auto msg = neograph::ChatMessage{
                    .role         = "tool",
                    .content      = fmt::format("[Start/Exception aborted: {}]", exceptionStr),
                    .tool_call_id = tool.id,
                    .tool_name    = tool.name,
                    .flags        = neograph::MessageFlag::AutoInserted,
                };
                auto msgJson = neograph::json{};
                neograph::to_json(msgJson, msg);
                appendToolResult.push_back(std::move(msgJson));
            }
            result.writes.push_back(neograph::graph::ChannelWrite{
                "messages",
                std::move(appendToolResult),
            });
        }
    }
}

void ToolcallWrapNode::onHandleBaseRunError(
    bool                         errorRethrow,
    bool                         isCurrentError,
    std::string_view             exceptionStr,
    neograph::graph::NodeInput&  in,
    neograph::graph::NodeOutput& result
) noexcept {
    // 插入消息，保证消息顺序正确
    if (false == errorRethrow && isCurrentError) {
        // 回填 tool_call_id/tool_name，确保 ToolEnd 能正确关联
        auto  messages = in.state.get_messages();
        auto* assistant_msg
            = agentxx::middleware::BaseMiddlewareHandleInterface::getLastAssistantToolcallMessage(
                messages
            );
        if (assistant_msg && !assistant_msg->tool_calls.empty()) {
            auto appendToolResult = neograph::json::array();
            for (const auto& tool : assistant_msg->tool_calls) {
                auto msg = neograph::ChatMessage{
                    .role         = "tool",
                    .content      = fmt::format("[BaseRun/Exception aborted: {}]", exceptionStr),
                    .tool_call_id = tool.id,
                    .tool_name    = tool.name,
                    .flags        = neograph::MessageFlag::AutoInserted,
                };
                auto msgJson = neograph::json{};
                neograph::to_json(msgJson, msg);
                appendToolResult.push_back(std::move(msgJson));
            }
            result.writes.push_back(neograph::graph::ChannelWrite{
                "messages",
                std::move(appendToolResult),
            });
        }
    }
}

asio::awaitable<void> ToolcallWrapNode::onHandleStart(
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    neograph::graph::NodeInput&                         in
) {
    co_await item.onToolcallStartFunc(in);
}

asio::awaitable<void> ToolcallWrapNode::onHandleEnd(
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    const neograph::graph::NodeInput&                   in,
    neograph::graph::NodeOutput&                        result
) {
    co_await item.onToolcallEndFunc(in, result);
}

asio::awaitable<std::string> ToolcallWrapNode::execTool(
    neograph::Tool*                                      tool,
    neograph::json&                                      args,
    const std::shared_ptr<neograph::graph::CancelToken>& cancelToken
) const {
    auto agentCtxPtr = agentContext.lock();
    {
        // 参数类型自动修正: 根据 tool 参数 JSON Schema 尽量让 arg 类型匹配参数需求
        // (string<->number/bool, string->字符串数组, [单字符串数组]->string 等)
        try {
            autoFixArgsType(tool->get_definition(), args);
        } catch (const std::exception& e) {
            XX_LOGW("Toolcall auto-fix arg type failed: {}", e.what());
        }
    }
    {
        // 权限检查 (permissionMiddleware 默认 nullptr, 未配置权限中间件时跳过)
        if (agentCtxPtr && agentCtxPtr->permissionMiddleware) {
            auto it = agentCtxPtr->permissionMiddleware->handles.find(tool->get_name());
            if (it != agentCtxPtr->permissionMiddleware->handles.end()) {
                auto allow = co_await it->second(*tool, args);
                if (false == allow) {
                    co_return "[Permission denied]";
                }
            }
        }
    }

    // 权限检查可能 co_await 挂起过, 执行 tool 前检查取消埋点
    if (cancelToken) {
        cancelToken->throw_if_cancelled("before tool execution");
    }

    size_t maxRetry = 0;
    {
        auto str    = tool->extra["maxRetry"];
        auto result = agentxx::util::parseNumberFromString(str, maxRetry);
        if (result.ec != std::errc{}) {
            maxRetry = 0;
        }
    }

    std::string result;
    size_t      retry = 0;
    do {
        bool               isCancel = false;
        std::string        errInfo;
        std::exception_ptr errorPtr;

        try {
            result = co_await tool->real_execute_async(args);
            break;
        } catch (const neograph::graph::CancelledException&) {
            isCancel = true;
            errorPtr = std::current_exception();
        } catch (const neograph::graph::NodeInterrupt&) {
            isCancel = true;
            errorPtr = std::current_exception();
        } catch (const boost::exception& e) {
            // boost::exception 在 std::exception 之前捕获, 保留完整诊断信息
            errInfo  = agentxx::util::autoTryConvertToUtf8(boost::diagnostic_information(e));
            errorPtr = std::current_exception();
        } catch (const std::exception& e) {
            errInfo  = agentxx::util::autoTryConvertToUtf8(e.what());
            errorPtr = std::current_exception();
        } catch (...) {
            errorPtr = std::current_exception();
        }

        // 触发异常
        if (retry >= maxRetry || isCancel) {
            std::rethrow_exception(errorPtr);
        }
        XX_LOGD("ToolCallNode {} retry: {}/{} | {}", tool->get_name(), retry, maxRetry, errInfo);
        retry++;
    } while (true);

    const size_t limitLength = agentCtxPtr->agentConfig->toolcallSummaryLimitOutputLength;
    if ("true" == tool->extra["autoSummaryOutput"] && result.size() >= limitLength) {
        // 字节数量超过，按 utf8 长度判断
        auto [targetIndex, lineCount, lastLineIndex]
            = agentxx::util::findIndexAndLastLineIndexByUtf8Length(result, limitLength);
        if (targetIndex > 0) {
            const auto session_id = args.value("sessionId", std::string{});
            assert(false == session_id.empty());
            // 超过限制长度，截断并存储原文
            auto storeId
                = agentCtxPtr->middlewareHandleContext->addShareStoreItemValue(session_id, result);
            // 总行数, 写入压缩结果便于后续用 `agentxx_share_store` 按行分页取值
            const auto totalLineCount = agentxx::util::countLines(result);
            // - 如果超过总摘要 1/3，按行摘要，留出行数以便后续用
            // `agentxx_share_store` 分页按行取值 否则取总摘要
            if (lastLineIndex >= targetIndex / 3) {
                co_return fmt::format(
                    R"([Content offloaded. Use the `agentxx_share_store` tool to fetch the full content by ID {}. Show {} lines, total {} lines, truncated {} lines]
{}
...)",
                    storeId,
                    lineCount,
                    totalLineCount,
                    totalLineCount - lineCount,
                    std::string_view{result}.substr(0, lastLineIndex)
                );
            } else {
                // 无法按行截断时取全部行数 (换行数) 作为截取行数
                co_return fmt::format(
                    R"([Content offloaded. Use the `agentxx_share_store` tool to fetch the full content by ID {}. Show {} chars, total {} lines, truncated {} lines]
{}
...)",
                    storeId,
                    limitLength,
                    totalLineCount,
                    lineCount,
                    std::string_view{result}.substr(0, targetIndex)
                );
            }
        }
    }
    co_return result;
}

asio::awaitable<void> ToolcallWrapNode::baseRun(
    std::vector<std::shared_ptr<agentxx::middleware::BaseMiddlewareHandleInterface>>& handles,
    neograph::graph::NodeInput&                                                       in,
    neograph::graph::NodeOutput&                                                      out
) {
    auto agentCtxPtr = agentContext.lock();

    auto toolcallsCache = std::map<std::string, std::string>{};
    {
        auto toolcallsCacheJson
            = agentCtxPtr->middlewareHandleContext->getGraphDataItemValue<neograph::json>(
                in.ctx.thread_id,
                agentxx::middleware::MiddlewareContext::graphDataKey_interruptToolcallCache
            );
        agentCtxPtr->middlewareHandleContext->removeGraphDataItem(
            in.ctx.thread_id,
            agentxx::middleware::MiddlewareContext::graphDataKey_interruptToolcallCache
        );
        if (toolcallsCacheJson.is_array()) {
            for (const auto& item : toolcallsCacheJson) {
                neograph::ChatMessage msg;
                neograph::from_json(item, msg);
                if (false == msg.tool_call_id.empty()
                    && false == neograph::hasFlag(msg.flags, neograph::MessageFlag::Interrupt)) {
                    toolcallsCache[msg.tool_call_id] = std::move(msg.content);
                }
            }
        }
    }

    auto messages = in.state.get_messages();
    if (messages.empty()) {
        out = neograph::graph::NodeOutput{};
        co_return;
    }

    // Find the last assistant message with tool_calls
    const neograph::ChatMessage* assistant_msg = nullptr;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == "assistant" && !it->tool_calls.empty()) {
            assistant_msg = &(*it);
            break;
        }
    }
    if (!assistant_msg) {
        out = neograph::graph::NodeOutput{};
        co_return;
    }

// debug 检查警告: 若该 assistant 声明的 tool_calls 在其之后已全部有 tool 结果消息,
// 说明这些工具已执行过, 本节点被重复调度
// - 正常流程不会发生 (工具执行后 llm 重新调用, 末尾变为新的 assistant 消息)
// - 出现时说明存在异常路径 (如异常被吞掉后 [has_tool_calls] 误路由), 记录
//   警告日志供排查; 不跳过执行, 避免掩盖真正的问题
#if XX_IS_DEBUG_D
    {
        const int64_t assistantIndex = static_cast<int64_t>(assistant_msg - &messages.front());
        std::set<std::string> replied;
        for (size_t i = static_cast<size_t>(assistantIndex) + 1; i < messages.size(); ++i) {
            if (messages[i].role == "tool" && false == messages[i].tool_call_id.empty()) {
                replied.insert(messages[i].tool_call_id);
            }
        }
        const bool allReplied = std::all_of(
            assistant_msg->tool_calls.begin(),
            assistant_msg->tool_calls.end(),
            [&](const neograph::ToolCall& tc) {
                return false == tc.id.empty() && replied.count(tc.id) > 0;
            }
        );
        if (allReplied) {
            XX_LOGW(
                "Toolcall re-execute check: last assistant tool_calls already fully replied ({} tools); abnormal scheduling suspected",
                assistant_msg->tool_calls.size()
            );
        }
    }
#endif

    bool isInterrupt   = false;
    bool isCancel      = false;
    auto interruptArgs = std::map<std::string, neograph::json>{};
    auto results       = neograph::json::array();
    // 已执行完成的 tool_call_id (取消时用于区分已完成/未完成, 未完成的补 [User canceled])
    std::set<std::string> completedToolcallIds{};
    std::exception_ptr    cancelErrorPtr;

    auto onExecTool = [&](const neograph::ToolCall& tc) -> asio::awaitable<neograph::ChatMessage> {
        neograph::ChatMessage tool_msg;
        tool_msg.role         = "tool";
        tool_msg.tool_call_id = tc.id;
        tool_msg.tool_name    = tc.name;
        {
            // 尝试缓存
            auto cacheit = toolcallsCache.find(tc.id);
            if (cacheit != toolcallsCache.end()) {
                tool_msg.content = cacheit->second;
                co_return tool_msg;
            }
        }

        auto it = std::find_if(tools_.begin(), tools_.end(), [&](neograph::Tool* t) {
            return t->get_name() == tc.name;
        });
        if (it == tools_.end()) {
            // 静态列表未命中: 查动态插件工具注册表 (热插拔工具)
            // - 返回 shared_ptr 保持插件代码段存活 (与插件的 inflight 计数配合,
            //   卸载流程等计数归零后才 dlclose)
            auto pluginTool = (agentCtxPtr && agentCtxPtr->toolRegistry)
                                  ? agentCtxPtr->toolRegistry->find(tc.name)
                                  : nullptr;
            if (!pluginTool) {
                tool_msg.content = fmt::format(R"([Error] Tool not found: {})", tc.name);
            } else {
                std::exception_ptr errorPtr;
                co_await agentxx::util::catchErrorAsync<bool>(
                    [&]() -> asio::awaitable<bool> {
                        try {
                            auto args = neograph::json::parse(tc.arguments);
                            if (args.is_object()) {
                                // append arg `session_id`
                                args["sessionId"] = in.ctx.thread_id;
                                // - 注入 toolCallId 供 tool 使用 (如 agentxx_subagent
                                // 的中断 resultId)
                                args["tool_call_id"] = tc.id;
                            }
                            tool_msg.content
                                = co_await execTool(pluginTool.get(), args, in.ctx.cancel_token);
                            // 取消埋点: tool 执行完成后检查, 避免取消后继续收集/执行后续 tool
                            if (in.ctx.cancel_token) {
                                in.ctx.cancel_token->throw_if_cancelled("after tool execution");
                            }
                        } catch (const neograph::graph::CancelledException&) {
                            // TODO: 保存已有的 toolcall 结果由 baseRun 的取消捕获处保存后再重新抛出
                            errorPtr = std::current_exception();
                        } catch (const neograph::graph::NodeInterrupt&) {
                            // tool触发中断
                            // - 不应在这里提取中断参数，协程并发等 co_await
                            // 执行完成时可能参数数组已经不是单一值
                            isInterrupt       = true;
                            tool_msg.flags   |= neograph::MessageFlag::Interrupt;
                            tool_msg.content  = "[Interrupt]";
                        }
                        co_return true;
                    },
                    [&](std::string errinfo) -> asio::awaitable<bool> {
                        tool_msg.content = fmt::format("[Exception aborted: {}]", errinfo);
                        co_return true;
                    },
                    nullptr,
                    // 传入取消令牌: tool 被取消信号中断产生的 operation_aborted
                    // 转换为 CancelledException, 避免取消被当作普通 tool 错误吞掉
                    in.ctx.cancel_token
                );
                if (errorPtr) {
                    std::rethrow_exception(errorPtr);
                }
            }
        } else {
            std::exception_ptr errorPtr;
            co_await agentxx::util::catchErrorAsync<bool>(
                [&]() -> asio::awaitable<bool> {
                    try {
                        auto args = neograph::json::parse(tc.arguments);
                        if (args.is_object()) {
                            // append arg `session_id`
                            args["sessionId"] = in.ctx.thread_id;
                            // - 注入 tool_call_id 供 tool 使用 (如 agentxx_subagent 的中断
                            // resultId)
                            args["tool_call_id"] = tc.id;
                        }
                        tool_msg.content = co_await execTool(*it, args, in.ctx.cancel_token);
                        // 取消埋点: tool 执行完成后检查, 避免取消后继续收集/执行后续 tool
                        if (in.ctx.cancel_token) {
                            in.ctx.cancel_token->throw_if_cancelled("after tool execution");
                        }
                    } catch (const neograph::graph::CancelledException&) {
                        // TODO: 保存已有的 toolcall 结果由 baseRun 的取消捕获处保存后再重新抛出
                        errorPtr = std::current_exception();
                    } catch (const neograph::graph::NodeInterrupt&) {
                        // tool触发中断
                        // - 不应在这里提取中断参数，协程并发等 co_await
                        // 执行完成时可能参数数组已经不是单一值
                        isInterrupt       = true;
                        tool_msg.flags   |= neograph::MessageFlag::Interrupt;
                        tool_msg.content  = "[Interrupt]";
                    }
                    co_return true;
                },
                [&](std::string errinfo) -> asio::awaitable<bool> {
                    tool_msg.content = fmt::format("[Exception aborted: {}]", errinfo);
                    co_return true;
                },
                nullptr,
                // 传入取消令牌: tool 被取消信号中断产生的 operation_aborted
                // 转换为 CancelledException, 避免取消被当作普通 tool 错误吞掉
                in.ctx.cancel_token
            );
            if (errorPtr) {
                std::rethrow_exception(errorPtr);
            }
        }
        co_return tool_msg;
    };

    /// 执行 toolcall
    std::vector<asio::awaitable<neograph::ChatMessage>> toolcallResults{};
    for (const auto& tc : assistant_msg->tool_calls) {
        toolcallResults.emplace_back(onExecTool(tc));
    }
    for (auto& item : toolcallResults) {
        // TODO: 真正并行
        try {
            auto           msg = co_await std::move(item);
            neograph::json msg_json;
            neograph::to_json(msg_json, msg);
            results.push_back(msg_json);
            completedToolcallIds.insert(msg.tool_call_id);
        } catch (const neograph::graph::CancelledException&) {
            // - 取消: 停止执行后续 tool, 由下方补齐未完成 tool 的取消提示消息
            // - 中断 (NodeInterrupt) 已在 onExecTool 内部捕获处理, 不会抛到这里
            isCancel       = true;
            cancelErrorPtr = std::current_exception();
            break;
        }
    }

    if (isCancel) {
        // - 取消后重新从图开始节点执行 (与中断不同, 中断会 resume 到本节点恢复,
        //   取消不会), 因此需要将本轮的 toolcall 结果直接写入 state,
        //   wrap_handle 会在 rethrow 前保存到 [graphDataKey_tempMessages],
        //   避免已完成的 tool 结果因 state 回滚而丢失
        // - 未完成的 tool 插入 [User canceled] 提示, 保证每条 assistant tool_call
        //   都有对应的 tool 结果消息, 上下文角色顺序和内容完整
        for (const auto& tc : assistant_msg->tool_calls) {
            if (completedToolcallIds.count(tc.id)) {
                continue;
            }
            neograph::ChatMessage tool_msg;
            tool_msg.role         = "tool";
            tool_msg.tool_call_id = tc.id;
            tool_msg.tool_name    = tc.name;
            tool_msg.content      = "[User canceled]";
            tool_msg.flags        = neograph::MessageFlag::AutoInserted;
            neograph::json msg_json;
            neograph::to_json(msg_json, tool_msg);
            results.push_back(std::move(msg_json));
        }
        in.state.write("messages", results);
        // 往外抛 cancel 异常，由 WrapNode 处理上下文临时保存
        std::rethrow_exception(cancelErrorPtr);
    }

    if (isInterrupt) {
        // 暂存 toolcall list 结果到 graphData
        agentCtxPtr->middlewareHandleContext->setGraphDataItemValue<neograph::json>(
            in.ctx.thread_id,
            agentxx::middleware::MiddlewareContext::graphDataKey_interruptToolcallCache,
            results
        );
        // 保存当前 messages，供 handler 恢复
        auto messages = in.state.get("messages");
        // 重新抛出异常
        agentCtxPtr->middlewareHandleContext->throwNodeInterruptBase(in.ctx.thread_id, messages);
    }

    out.writes.push_back(neograph::graph::ChannelWrite{"messages", std::move(results)});
    co_return;
}

void ToolcallWrapNode::defStdoutLogOnToolcallStart(
    neograph::graph::NodeInput& in,
    size_t                      limitOutput
) {
    auto messages = in.state.get_messages();
    auto assistant_msg
        = agentxx::middleware::BaseMiddlewareHandleInterface::getLastAssistantToolcallMessage(
            messages
        );

    std::ostringstream out{};
    if (assistant_msg) {
        size_t index = 0;
        for (auto& item : assistant_msg->tool_calls) {
            ++index;
            out << "┣━ Argument: " << index << ". " << item.name << "/" << item.id << std::endl;
            out << "             - "
                << ((0 == limitOutput) ? item.arguments
                                       : std::string_view{item.arguments}.substr(0, limitOutput))
                << std::endl;
        }
    } else {
        out << "┣━ Empty Argument List\n";
    }

    XX_LOGD(
        R"(
┏━━━━━━ Toolcall ━━━━━━┓
{}
)",
        out.str()
    );
}

void ToolcallWrapNode::defStdoutLogOnToolcallEnd(
    const neograph::graph::NodeInput& in,
    neograph::graph::NodeOutput&      result,
    size_t                            limitOutput
) {
    std::ostringstream out{};
    if (false == result.writes.empty()) {
        size_t index = 0;
        for (auto& item : result.writes) {
            ++index;
            auto str = item.value.dump();
            out << "┣━ Result  : " << index << ". " << item.channel << ": "
                << ((0 == limitOutput) ? str : std::string_view{str}.substr(0, limitOutput))
                << std::endl;
        }
    } else {
        out << "┣━ Empty Result List\n";
    }

    XX_LOGD(
        R"(
{}
┗━━━━━━ Toolcall ━━━━━━┛
)",
        out.str()
    );
}

} // namespace nodes
} // namespace agentxx
