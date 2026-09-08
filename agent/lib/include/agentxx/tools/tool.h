#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/middlewares/middleware.h"
#include "asio/io_context.hpp"
#include "fmt/base.h"
#include "fmt/format.h"
#include "neograph/graph/cancel.h"
#include <functional>
#include <neograph/llm/rate_limited_provider.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/neograph.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace asio = ::boost::asio;

namespace agentxx {
namespace tools {

/// 从 tool 参数中取 sessionId 并获取对应会话的取消令牌
/// - sessionId 由 ToolCallNode 在调用工具前注入 arguments
/// - 会话不存在 (如非 toolcall 路径调用) 或令牌为空时返回 nullptr (无取消支持)
std::shared_ptr<neograph::graph::CancelToken> getSessionCancelToken(
    const std::shared_ptr<agentxx::agent::AgentContext>& agentCtx,
    const agentxx::util::Json&                           args
);

/// - 封装原始的 [neograph::Tool] 类型，添加额外功能
/// - 部分函数 (如 MCP) 返回的 tool 类型是原始的 [neograph::Tool]，可以用
/// [XXToolWrap] 进行封装扩展功能
///
/// 图兼容说明:
/// - 本类同时继承 `neograph::Tool` (图引擎 `ToolDispatchNode` 仅识别该类型):
///   `execute_async(const neograph::json&)` 为桥接 override, 内部经
///   `neograph_json_bridge` 转为 `Json` 后调用主接口 `execute_async(const Json&)`
/// - 业务层 (ToolcallWrapNode/测试/插件适配) 一律调用 `Json` 主接口,
///   不再触碰 `neograph::json`
class XXToolBase : public neograph::Tool {
protected:

    const std::string                           name;
    std::weak_ptr<agentxx::agent::AgentContext> agentContext;

public:

    /// - 自动压缩 tool 输出，当长度超过限制值
    /// [agentxx::agent::AgentConfig::toolcallSummaryLimitOutputLength] 时，且该
    /// tool 启用 [autoSummaryOutput] 则进行压缩
    const bool autoSummaryOutput;
    /// - 延迟加载
    /// - `true`: 该 tool 在初始时仅记录名称等简短信息在 system prompt，由
    /// `tool_skill_search` 检索查找合适的 tool 后才加载全量信息并支持LLM调用
    const bool canDelayLoad;
    /// - 最大重试次数
    /// - 如果 [maxRetry] > 0，当 tool 执行抛出异常时，进行重试
    /// - 最多执行 1 + maxRetry(retry) 次
    const size_t maxRetry;
    /// - 连续相同调用重复检查开关 (默认 false 关闭)
    /// - `true`: 当同一 llm <-> tool 交替链内连续多次 (阈值
    ///   [agentxx::agent::AgentConfig::toolcallRepeatCheckThreshold], 默认 5 次)
    ///   相同 tool + 相同参数调用时, ToolcallNode 会经 permission 总线发起询问
    ///   警告用户, 用户确认后才继续执行, 拒绝则中止本次调用
    /// - 功能实现见 [agentxx::nodes::ToolcallWrapNode::execTool]
    const bool repeatCallCheck;

    XXToolBase(
        std::string_view                            in_name,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext,
        bool                                        in_autoSummaryOutput = false,
        bool                                        in_canDelayLoad      = true,
        size_t                                      in_maxRetry          = 0,
        bool                                        in_repeatCallCheck   = false
    );

    std::string get_name() const override;

    neograph::ChatTool get_definition() const override {
        // 子类必须提供工具定义；基类无定义时返回空定义（不应被直接实例化调用）
        return neograph::ChatTool{};
    }

    /// 业务主接口 (Json): 子类覆写此函数实现工具逻辑
    /// - 注意: 这不是 `neograph::Tool` 的虚覆盖 (签名不同), 而是本类新增虚函数;
    ///   子类用 `override` 关键字覆盖本函数 (如 GitWorktreeTool 等)
    virtual asio::awaitable<std::string> execute_async(const agentxx::util::Json& arguments);

    /// 图兼容桥接: `neograph::Tool::execute_async` 的 override,
    /// 经 `fromNeographJson` 转发到 Json 主接口
    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;

    /// 同步桥接: `neograph::Tool::execute` 纯虚函数的实现,
    /// 经事件循环同步驱动 Json 主接口 (与原 AsyncTool 语义一致)
    std::string execute(const neograph::json& arguments) override;

    /// real_execute_async 默认走 Tool 基类实现 (桥接到 execute_async 图桥接);
    /// 保留虚分发以便 need 时定制
    using neograph::Tool::real_execute_async;

    virtual std::optional<agentxx::middleware::SummarizationToolHandle>
        createSummarizationToolHandle() const;
};

/// - 封装原始的 [neograph::Tool] 类型，添加额外功能
/// - 部分函数 (如 MCP) 返回的 tool 类型是原始的 [neograph::Tool]，可以用
/// [XXToolWrap] 进行封装扩展功能
class XXToolWrap : public XXToolBase {
protected:

    std::unique_ptr<neograph::Tool>                             inner;
    std::optional<agentxx::middleware::SummarizationToolHandle> summarizationHandle;

public:

    XXToolWrap(
        std::unique_ptr<neograph::Tool>&&                           in_inner,
        std::weak_ptr<agentxx::agent::AgentContext>                 in_agentContext,
        bool                                                        in_autoSummaryOutput = false,
        bool                                                        in_canDelayLoad      = false,
        size_t                                                      in_maxRetry          = 0,
        std::optional<agentxx::middleware::SummarizationToolHandle> in_summarizationHandle
        = std::nullopt,
        bool in_repeatCallCheck = false
    );

    std::string get_name() const override;

    neograph::ChatTool get_definition() const override;

    /// 业务主接口: 转发到被包装的 inner tool (Json 经桥接转换)
    asio::awaitable<std::string> execute_async(const agentxx::util::Json& arguments) override;
};

} // namespace tools
} // namespace agentxx
