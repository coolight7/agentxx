#pragma once

#include "agentxx/middlewares/middleware.h"
#include "asio/io_context.hpp"
#include <map>
#include <memory>
#include <neograph/neograph.h>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace middleware {

class _SummarizationMiddlewareState : public BaseMiddlewareState {
public:

    _SummarizationMiddlewareState() {}
};

/// 上下文压缩
/// - `system prompt`、最近的消息 不压缩
/// - 超过 75% 上限时自动压缩:
///   - 触发压缩时先发送一条 viewMessage 提示 "正在压缩上下文"
///   - 确定性压缩 (toolcall 去重/探索折叠 + 噪音清理)
///   - LLM 同上下文总结压缩 (保持同一上下文, 不传入任何工具,
///     由 subagent 对当前上下文原样总结压缩)
///   - 压缩完成时更新 viewMessage 为 "压缩上下文
///   {旧上下文token量}->{新上下文token量}/{最大上下文限制} · {耗时}"
/// - 压缩结果覆盖回: [system] | [user 压缩指令] | [assistant 摘要] | 最近消息
/// - 压缩失败 >= 2 次 (同一轮内) 或 token >= 95% 上限: 硬截断兜底, 保证请求能发出
class SummarizationMiddlewareHandle : public BaseMiddlewareHandle<_SummarizationMiddlewareState> {
protected:

    /// 最近消息保留的 token 预算比例默认值 (占模型上下文上限)
    static constexpr double recentTokenBudgetRatioDefault = 0.20;

public:

    /// 模型支持的最大 token 默认值 (256k)
    /// - 当模型配置未指定 [agentxx::agent::ModelConfig::modelContenxtMaxToken] 时使用
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
    /// 最近消息保留的 token 预算比例 (占模型上下文上限, LLM 压缩时使用)
    const double recentTokenBudgetRatio;
    /// LLM 压缩摘要的最大输出 token 数 (经 {max_words} 注入压缩指令)
    const size_t summaryMaxTokens;

public:

    /// 压缩 tool 时处理函数
    std::map<std::string, SummarizationToolHandle> summarizationToolHandles{};

    SummarizationMiddlewareHandle(
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext,
        size_t in_defaultModelSupportMaxToken = defaultModelSupportMaxToken,
        double in_asciiCharsPerToken          = 4.0,
        double in_unicodeCharsPerToken        = 1.5,
        double in_tokensPerImage              = 400.0,
        double in_extraTokensPerMessage       = 3.0,
        double in_recentTokenBudgetRatio      = recentTokenBudgetRatioDefault,
        size_t in_summaryMaxTokens            = 2048
    );

    size_t countTokensForUtf8Str(std::string_view in_str) const;

    size_t countTokens(
        const std::vector<std::string>&           systemMsgs,
        const std::vector<neograph::ChatMessage>& messages,
        bool                                      countThinking = false
    ) const;

    std::string messagesToText(
        const std::vector<neograph::ChatMessage>& msgs,
        bool                                      includeSystem = false
    ) const;

    /// 确定性噪音清理 (保留 thinking, 不做 offload):
    /// - 删除完全空的消息 (无 content/tool_calls/reasoning/附件)
    /// - 相邻完全相同的消息只保留最后一条
    /// - 连续出现的 AutoInserted 提示噪音 ([Please continue] 等) 只保留最后一条
    /// - 对全部消息执行 (语义上保留最新, 对 recent 段安全)
    void cleanNoiseMessages(std::vector<neograph::ChatMessage>& messages);

    /// 工具调用压缩: 去重截断 (现有) + 探索型调用序列折叠
    /// - 连续 >= 3 次的同工具单工具调用段 (中间无 user/system 打断, 且工具注册了
    ///   truncateResponse 而无 truncateRequest 的"读类"工具), 只保留最后一组
    ///   (assistant + tool 结果), 其余整组删除: 探索过程无价值, 结论在最后一组
    void doSummarizeToolcall(std::vector<neograph::ChatMessage>& messages);

    /// 探索型调用序列折叠 (见 doSummarizeToolcall)
    void foldExploratoryToolcalls(std::vector<neograph::ChatMessage>& messages);

    /// 按 token 预算 + 轮次对齐切分消息, 返回最近消息段的起始索引 (oldEnd)
    /// - [systemCount, oldEnd) 为可压缩段; [oldEnd, size) 为 recent (预算内, 至少 1 条)
    /// - 对齐规则:
    ///   1. recent 开头为 tool 消息 → 回退到发起这组 toolcall 的 assistant (整组纳入 recent)
    ///   2. 压缩段末尾为 assistant(tool_calls) 且其 tool 结果在 recent 内 → 整组划入 recent,
    ///      避免压缩段以悬挂 tool_calls 结尾
    size_t splitRecentByTokenBudget(
        const std::vector<neograph::ChatMessage>& messages,
        size_t                                    systemCount,
        size_t                                    tokenBudget
    ) const;

    /// LLM 同上下文压缩: 通过 subagent 完成 (同上下文模式)
    /// - 请求参数: subagent="subagent_task", messages=压缩段原消息(含 system)
    ///   + 末尾追加 user 压缩指令 (结构化透传, 无文本转录),
    ///   sessionId=父线程 (与父会话相同 threadid + 相同模型 → 命中 KV cache),
    ///   tools 不传入 (子代理无任何工具, 仅对当前上下文原样做压缩, 不经过
    ///   share_store 外置), enable_summarization=false (禁止二次压缩)
    /// - subagent 内部完成"阅读上下文 → 输出摘要"的完整 agent 循环,
    ///   最终纯文本输出即为摘要
    /// - 通过 NodeInterrupt 中断父轮次派生 subagent, resume 后返回结果;
    ///   无 subagentManager / 消息为空 / 压缩失败时返回空串 (调用方保留原消息)
    asio::awaitable<std::string> doSummarizeWithLLM(
        std::string_view                          sessionId,
        const std::vector<neograph::ChatMessage>& messages
    );

    /// 硬截断兜底: 保留 system + 截断说明 + 最近消息 (30% 预算)
    std::vector<neograph::ChatMessage> hardTruncate(
        const std::vector<neograph::ChatMessage>& messages,
        size_t                                    systemCount,
        size_t                                    maxToken
    ) const;

    /// 手动压缩指定会话的上下文 (供客户端 Summy Context 按钮直接触发)
    asio::awaitable<bool> compactSessionContext(std::string_view sessionId);

    asio::awaitable<void> onModelcallRunFunc(neograph::graph::NodeInput& in) override;

    ~SummarizationMiddlewareHandle() override;

    /// 在 EventBus 上注册 Token 计算等服务
    void registerOnBus(const std::shared_ptr<agentxx::event::EventBus>& bus);

    /// 从 EventBus 注销
    void unregisterFromBus();

private:

    std::weak_ptr<agentxx::event::EventBus> registeredBus_;
    size_t                                  compactSubId_ = 0;
};

} // namespace middleware
} // namespace agentxx
