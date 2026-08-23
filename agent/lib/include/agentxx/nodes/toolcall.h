#pragma once

#include "agentxx/nodes/wrap_handle.h"
#include <set>
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
    /// - [repeatCallTriggered] 当前调用是否触发连续重复阈值 (由 baseRun 经
    ///   findConsecutiveRepeatCallKeys 基于 messages 的 llm <-> tool 交替链检测);
    ///   为 true 且该 tool 启用了 [agentxx::tools::XXToolBase::repeatCallCheck]
    ///   时, 经 permission 总线发起询问警告用户, 用户确认后才继续执行
    /// - [repeatCallKey] 当前调用的重复标识 key (见 makeRepeatCallKey),
    ///   用于询问提示中向用户展示
    asio::awaitable<std::string> execTool(
        neograph::Tool*                                      tool,
        neograph::json&                                      args,
        const std::shared_ptr<neograph::graph::CancelToken>& cancelToken,
        bool                                                 repeatCallTriggered = false,
        std::string_view                                     repeatCallKey       = {}
    ) const;

    /// 计算重复调用标识 key: `{toolName}_{arg字符串长度}_{arg哈希值}`
    /// - 相同 tool + 相同参数 (arguments 原始 JSON 字符串) 得到相同 key;
    ///   长度参与拼接可降低哈希碰撞被误判为相同调用的概率
    static std::string makeRepeatCallKey(std::string_view toolName, std::string_view arguments);

    /// 检测 messages 中以 [assistantMsg] 结尾的连续 llm <-> tool 交替链内的循环调用,
    /// 返回达到 [threshold] 次连续相同调用 (key 口径见 makeRepeatCallKey) 的 key 集合
    /// (通常为空; 同轮并行调用可能多于一个)
    /// - 连续链: 从 assistantMsg 起向前仅允许出现 assistant(带 tool_calls) 与
    ///   tool 结果消息, 二者交替; 遇到 user/system 等其他角色消息即视为断开;
    ///   不带 tool_calls 的 assistant (最终文本回复) 同样视为断开
    /// - 性能 (提前终止, 回溯有界):
    ///   - 从 assistantMsg 向前最多回溯 threshold 条 assistant 消息: 若某 key 真的
    ///     连续出现达 threshold 次, 最近 threshold 条 assistant 每条必含该 key,
    ///     更早的消息不可能再补足缺口
    ///   - 某条 (非当前轮) assistant 的所有 tool_call 生成的 key 均不在已统计
    ///     集合中时, 说明该条开启了全新调用, 重复计数至多延续到这里, 终止回溯
    ///   - 任一 key 计数达到 threshold 时立即返回 (已确定存在循环调用)
    static std::set<std::string> findConsecutiveRepeatCallKeys(
        const std::vector<neograph::ChatMessage>& messages,
        size_t                                    assistantMsgIndex,
        size_t                                    threshold
    );

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
