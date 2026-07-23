#pragma once

#include "agentxx/middlewares/middleware.h"
#include "asio/io_context.hpp"
#include <list>
#include <map>
#include <memory>
#include <neograph/neograph.h>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace tools {
class SubAgentManagerTool;
} // namespace tools

namespace middleware {

class _SummarizationContext {
public:

    std::list<std::vector<neograph::ChatMessage>> oldMessagesHistory{};
};

class _SummarizationMiddlewareState : public BaseMiddlewareState {
public:

    _SummarizationContext summarizationContext{};

    _SummarizationMiddlewareState() {}
};

/// 上下文压缩
/// - `system prompt`、最近的消息 不压缩
/// - 可压缩的长消息内容用 `share_store` 暂存，留下 id + depict
/// - 选择多条消息总结压缩合并为一条
class SummarizationMiddlewareHandle : public BaseMiddlewareHandle<_SummarizationMiddlewareState> {
protected:

    /// 保留至少最近 N 条消息不被压缩
    static constexpr size_t keepRecentMessageCount = 4;
    /// 单条消息内容超过此字节数时考虑暂存到 share_store
    static constexpr size_t longContentByteThreshold = 2000;

    agentxx::tools::SubAgentManagerTool* subagentManager;

public:

    /// 模型支持的最大 token 默认值 (256k)
    /// - 当模型配置未指定 [agentxx::agent::ModelConfig::modelSupportMaxToken] 时使用
    static constexpr size_t defaultModelSupportMaxToken = 256 * 1024;

protected:

    /// 模型支持的最大 token 默认值 (模型配置未指定时使用)
    const size_t modelSupportMaxTokenDefault;
    /// 每个 token 大约为 [asciiCharsPerToken] 个 ascii 字符
    const double asciiCharsPerToken;
    /// 每个 token 大约为 [unicodeCharsPerToken] 个 unicode(除去 ascii) 字符
    const double unicodeCharsPerToken;
    const double tokensPerImage;
    const double extraTokensPerMessage;

public:

    /// 压缩 tool 时处理函数
    std::map<std::string, SummarizationToolHandle> summarizationToolHandles{};

    SummarizationMiddlewareHandle(
        agentxx::tools::SubAgentManagerTool*        in_subagentManager,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext,
        size_t in_defaultModelSupportMaxToken = defaultModelSupportMaxToken,
        double in_asciiCharsPerToken          = 4.0,
        double in_unicodeCharsPerToken        = 1.1,
        double in_tokensPerImage              = 400.0,
        double in_extraTokensPerMessage       = 3.0
    );

    size_t countTokensForUtf8Str(std::string_view in_str);

    size_t countTokens(
        const std::vector<std::string>&           systemMsgs,
        const std::vector<neograph::ChatMessage>& messages
    );

    std::string
        messagesToText(const std::vector<neograph::ChatMessage>& msgs, bool includeSystem = false);

    asio::awaitable<std::string>
        doSummarizeWithLLM(const std::vector<neograph::ChatMessage>& messages);

    void offloadLongContentToTempStore(
        neograph::ChatMessage&                    msg,
        const std::shared_ptr<MiddlewareContext>& ctx,
        const std::string&                        thread_id
    );

    void doSummarizeToolcall(std::vector<neograph::ChatMessage>& messages);

    asio::awaitable<void> onModelcallRunFunc(neograph::graph::NodeInput& in) override;
};

} // namespace middleware
} // namespace agentxx