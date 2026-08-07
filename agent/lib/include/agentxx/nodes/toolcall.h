#pragma once

#include "agentxx/nodes/wrap_handle.h"
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace nodes {

class NEOGRAPH_API ToolcallWrapNode : public WrapHandleBaseNode<neograph::graph::ToolDispatchNode> {
protected:
public:

    inline static constexpr auto defNodeType = std::string_view{"xx_Toolcall"};

    ToolcallWrapNode(
        std::string_view                            in_name,
        const neograph::graph::NodeContext&         in_ctx,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
    );

    void onHandleStartError(
        bool                                                errorRethrow,
        bool                                                isCurrentError,
        std::string_view                                    exceptionStr,
        agentxx::middleware::BaseMiddlewareHandleInterface& item,
        neograph::graph::NodeInput&                         in,
        neograph::graph::NodeOutput&                        result
    ) noexcept override;

    void onHandleBaseRunError(
        bool                         errorRethrow,
        bool                         isCurrentError,
        std::string_view             exceptionStr,
        neograph::graph::NodeInput&  in,
        neograph::graph::NodeOutput& result
    ) noexcept override;

    asio::awaitable<void> onHandleStart(
        agentxx::middleware::BaseMiddlewareHandleInterface& item,
        neograph::graph::NodeInput&                         in
    ) override;

    asio::awaitable<void> onHandleEnd(
        agentxx::middleware::BaseMiddlewareHandleInterface& item,
        const neograph::graph::NodeInput&                   in,
        neograph::graph::NodeOutput&                        result
    ) override;

    /// 执行单个 tool
    /// - [cancelToken] 当前轮次取消令牌: 传递给 ContextualAsyncTool 以便 tool
    ///   轮询取消或传播到其传输层; 可为 nullptr (无取消支持)
    asio::awaitable<std::string> execTool(
        neograph::Tool*                                      tool,
        neograph::json&                                      args,
        const std::shared_ptr<neograph::graph::CancelToken>& cancelToken
    ) const;

    /// 根据 tool 的参数 JSON Schema 自动修正参数类型兼容性, 尽量让 arg 类型匹配参数需求:
    /// - string -> 字符串数组: 参数声明为数组 (字符串数组) 而传入单个字符串时, 包装为 `[str]`
    /// - string -> number/integer: 参数声明为数值而传入字符串时, 若字符串可完整解析为数值则转换
    ///   (integer 仅接受整数写法; number 支持小数/指数; 前导 '+', 首尾空白会被容忍)
    /// - number/integer -> string: 参数声明为字符串而传入数值时, 转为十进制字符串
    /// - bool -> string / string("true"/"false") -> boolean: 布尔与字符串互相转换
    /// - [单字符串数组] -> string: 参数声明为字符串而传入单元素字符串数组时, 解包为字符串
    /// - 仅当目标类型不包含 arg 当前类型时转换; 无法解析或类型不明确时保持原样
    /// @return 是否发生了参数转换
    static bool autoFixArgsType(const neograph::ChatTool& def, neograph::json& args);

    asio::awaitable<void> baseRun(
        std::vector<std::shared_ptr<agentxx::middleware::BaseMiddlewareHandleInterface>>& handles,
        neograph::graph::NodeInput&                                                       in,
        neograph::graph::NodeOutput&                                                      out
    ) override;

    static void defStdoutLogOnToolcallStart(neograph::graph::NodeInput& in, size_t limitOutput = 0);

    static void defStdoutLogOnToolcallEnd(
        const neograph::graph::NodeInput& in,
        neograph::graph::NodeOutput&      result,
        size_t                            limitOutput = 0
    );
};
} // namespace nodes
} // namespace agentxx
