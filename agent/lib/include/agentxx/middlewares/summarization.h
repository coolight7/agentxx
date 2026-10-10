#pragma once

#include "agentxx/feature/points.h"
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
/// - `system prompt` 不压缩
/// - 超过 75% 上限时自动压缩:
///   - 触发压缩时先发送一条 viewMessage 提示 "正在压缩上下文"
///   - 确定性压缩 (toolcall 去重/探索折叠 + 噪音清理)
///   - LLM 同上下文总结压缩 (保持同一上下文, 不传入任何工具):
///     压缩请求 = 当前完整上下文 (system + 全部消息) + 末尾压缩指令, 由 subagent
///     决定哪些信息必须写进摘要 —— 末尾的消息也交给它, 让它在摘要里写清进行中的
///     动作、最近一次工具调用的要点与下一步
///   - 压缩完成时更新 viewMessage 为 "压缩上下文
///   {旧上下文token量}->{新上下文token量}/{最大上下文限制} · {耗时}"
/// - 压缩结果覆盖回: [system] | [user 压缩指令] | [assistant 摘要]
///   (不再追加最近消息: 末尾要点已由摘要覆盖, 否则刚压完的上下文立刻被最近消息
///   重新填满, 压缩等于没做)
/// - 压缩请求中的 thinking 只保留最新一条, 其余清空: 旧轮 thinking 对续写没有价值
///   (结论已写在 content / tool 结果里), 加密 thinking 也无法按字符估算 token
/// - 摘要有内置长度上限 [summaryMaxTokensDefault]: 超过时截断后写回, 避免模型写出
///   超长摘要把刚压下去的上下文重新撑大
/// - 压缩完成即把压缩结果写回会话上下文并落盘: 进程在压缩后到轮末之间退出
///   (崩溃/被杀) 时不丢压缩结果, 重启后不会因上下文重新超限而反复压缩
/// - 自动压缩经 NodeInterrupt 派生压缩子代理, resume 后本中间件从头重新执行:
///   压缩提示消息按挂起 id 复用 (更新而非追加), 不产生重复提示
/// - 压缩失败 >= 2 次 (同一轮内) 或 token >= 95% 上限: 硬截断兜底, 保证请求能发出
class SummarizationMiddlewareHandle : public BaseMiddlewareHandle<_SummarizationMiddlewareState> {
protected:

    /// 摘要 (写回的 assistant 压缩结果) 的最大 token 数, 内置值不暴露配置
    /// - 上限而非目标: 正常摘要远小于它, 这里只拦住模型写出的超长摘要
    /// - 同时作为压缩指令里 {max_words} 的来源 ([summaryMaxTokens] / 4)
    static constexpr size_t summaryMaxTokensDefault = 64 * 1024;

public:

    /// 模型支持的最大 token 默认值 (256k)
    /// - 当模型配置未指定 [agentxx::agent::ModelConfig::modelContextMaxToken] 时使用
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
    /// 摘要 (assistant 压缩结果) 的最大 token 数
    /// - 经 {max_words} (= [summaryMaxTokens] / 4) 注入压缩指令
    /// - 写回前超长时按它截断, 见 [fitSummaryMaxTokens]
    const size_t summaryMaxTokens;

    /// token 估算规则 (功能点 `agentxx.context.countTokens` 的核心实现与
    /// 同步快路径共用同一份实例: 规则只写一次, 两个入口结果必然一致)
    std::shared_ptr<agentxx::feature::TokenEstimator> tokenEstimator;

    /// 本中间件声明的上下文功能点 (token 估算 / 压缩)
    /// - 在构造时声明 (早于插件装载), 因此插件可以为这两个点登记实现
    /// - 未取到 AgentContext 时为空 (单测直接构造中间件的场景)
    agentxx::feature::ContextPoints featurePoints;

public:

    /// 压缩 tool 时处理函数
    std::map<std::string, SummarizationToolHandle> summarizationToolHandles{};

    /// 本中间件声明的上下文功能点 (token 估算 / 压缩)
    /// - 在构造时声明 (早于插件装载), 因此插件可以为这两个点登记实现
    /// - 未取到 AgentContext 时为空 (单测直接构造中间件的场景)
    const agentxx::feature::ContextPoints& points() const noexcept {
        return featurePoints;
    }

    /// token 估算规则实例 (功能点核心实现与同步快路径共用同一份)
    const std::shared_ptr<agentxx::feature::TokenEstimator>& estimator() const noexcept {
        return tokenEstimator;
    }

    SummarizationMiddlewareHandle(
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext,
        size_t in_defaultModelSupportMaxToken = defaultModelSupportMaxToken,
        double in_asciiCharsPerToken          = 4.0,
        double in_unicodeCharsPerToken        = 1.1,
        double in_tokensPerImage              = 400.0,
        double in_extraTokensPerMessage       = 3.0,
        size_t in_summaryMaxTokens            = summaryMaxTokensDefault
    );

    size_t countTokensForUtf8Str(std::string_view in_str) const;

    size_t countTokens(
        const std::vector<std::string>&           systemMsgs,
        const std::vector<neograph::ChatMessage>& messages,
        bool                                      countThinking = false
    ) const;

    /// 经功能点 `agentxx.context.countTokens` 取 token 数 (预算计算用)
    /// - 插件 / FFI 宿主为该点登记的实现会在这里生效; 同步快路径
    ///   ([countTokensForUtf8Str] / [countTokens], 供 TPS 与内部裁剪使用)
    ///   仍只走核心实现, 插件实现不参与 (见 plan §8.1)
    /// - 拿不到值时回退同步快路径 (功能点不可用不影响压缩流程)
    asio::awaitable<size_t> countTokensViaPoint(
        std::string_view                          sessionId,
        const std::vector<neograph::ChatMessage>& messages,
        bool                                      countThinking
    );

    std::string messagesToText(
        const std::vector<neograph::ChatMessage>& msgs,
        bool                                      includeSystem = false
    ) const;

    /// 确定性噪音清理 (不做 offload):
    /// - 删除完全空的消息 (无 content/tool_calls/reasoning/附件)
    /// - 相邻完全相同的消息只保留最后一条
    /// - 连续出现的 AutoInserted 提示噪音 ([Please continue] 等) 只保留最后一条
    /// - 对全部消息执行 (语义上保留最新)
    void cleanNoiseMessages(std::vector<neograph::ChatMessage>& messages);

    /// thinking 清理: 只保留最新一条非空 thinking, 其余全部清空
    /// - 旧轮 thinking 对续写没有价值 (结论已写在 content / tool 结果里)
    /// - 部分模型的 thinking 是加密内容, 按字符估算 token 不准, 不留在上下文里
    /// - 保留最新一条: 压缩子代理据此看到父会话最近一次的推理状态
    /// - 用于压缩请求; 压缩结果只含摘要, 写回结果无需再清理
    void cleanThinkingMessages(std::vector<neograph::ChatMessage>& messages);

    /// 摘要超过 [summaryMaxTokens] 时按估算 token 截断后返回 (保留开头 + 截断说明)
    /// - 压缩指令里的长度要求不保证被模型遵守, 这里保证写回的摘要不会把
    ///   刚压下去的上下文重新撑大
    /// - 未超限时原样返回
    std::string fitSummaryMaxTokens(std::string summary) const;

    /// 工具调用压缩: 去重截断 (现有) + 探索型调用序列折叠    /// - 连续 >= 3 次的同工具单工具调用段 (中间无 user/system 打断, 且工具注册了
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

    /// LLM 同上下文压缩的核心实现 (功能点 `agentxx.context.summarize` 的 core 层)
    ///
    /// 只产出摘要文本: 不替换会话消息、不发提示消息、不写 share store ——
    /// 写回由编排方 (本中间件) 做。内部派生"用完即弃"的压缩子代理属于实现细节。
    ///
    /// - 请求参数: subagent="subagent_task", messages=当前完整上下文(含 system)
    ///   + 末尾追加 user 压缩指令 (结构化透传, 无文本转录),
    ///   sessionId=父线程 (与父会话相同 threadid + 相同模型 → 命中 KV cache),
    ///   tools=[] (子代理无任何工具, 仅对当前上下文原样做压缩, 不经过
    ///   share_store 外置), enable_summarization=false (禁止二次压缩)
    /// - subagent 内部完成"阅读上下文 → 输出摘要"的完整 agent 循环,
    ///   最终纯文本输出即为摘要
    /// - direct=false (默认): 自动触发压缩, 在 agent 轮次内调用 —— 经
    ///   NodeInterrupt 中断父轮次派生 subagent (SubagentExecute RR →
    ///   requestInterrupt), resume 后返回结果; 必须在 AgentRunner
    ///   中断循环内执行, 否则 NodeInterrupt 无人处理会逃逸
    /// - direct=true: 手动触发压缩, agent 空闲时调用 (无 AgentRunner 中断
    ///   循环) —— 直接经宿主 AgentHost::spawnBatch 派生压缩子代理并等待
    ///   完成, 不抛 NodeInterrupt; 需要 AgentContext::host 有效, 超时
    ///   或失败返回空字符串 (调用方保留原消息)
    /// - 无 subagentManager / 消息为空 / 压缩失败时返回空串 (调用方保留原消息)
    asio::awaitable<std::string> doSummarizeWithLLM(
        std::string_view                          sessionId,
        const std::vector<neograph::ChatMessage>& messages,
        bool                                      direct = false
    );

    /// 经功能点 `agentxx.context.summarize` 取摘要文本 (编排方入口)
    /// - 插件 / FFI 宿主为该点登记的实现会在这里生效; 拿不到值时返回空串
    ///   (调用方走硬截断兜底)
    /// - 本轮该点声明为不可对外开放调用 (`callable = false`), 只由本中间件取用
    asio::awaitable<std::string> summarizeViaPoint(
        std::string_view                          sessionId,
        const std::vector<neograph::ChatMessage>& messages,
        bool                                      direct = false
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
    void registerOnBus(const std::shared_ptr<agentxx::events::EventBus>& bus);

    /// 从 EventBus 注销
    void unregisterFromBus();

private:

    std::weak_ptr<agentxx::events::EventBus> registeredBus_;
    size_t                                   compactSubId_ = 0;
};

} // namespace middleware
} // namespace agentxx
