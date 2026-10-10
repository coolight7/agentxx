/// agentxx 核心功能点的 id、请求/结果结构与实现装配
///
/// 这一份头是"核心点的稳定清单": 点 id、展示名、说明全部集中在这里, 文档表格与
/// 测试引用同一份常量 (避免命名与文档漂移)。插件不写进这份表 —— 插件的点是运行期
/// 数据, 跟着插件生命周期来去 (见 `agentxx/feature/registry.h`)。
///
/// 第一批点:
/// - `agentxx.context.countTokens`: 文本 Token 估算 (可调; 带按身份的值缓存);
/// - `agentxx.context.summarize`:   上下文压缩 (本轮不可调);
/// - `agentxx.tool.summarizeOutput`: 工具输出摘要 (第二批, 见设计文档 §8.3)。
#pragma once

#include "agentxx/feature/feature.h"
#include "agentxx/util/neograph_json_bridge.h"
#include "utilxx_base/hash.h"
#include "utilxx_base/string_util.h"
#include <memory>
#include <neograph/types.h>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace feature {

class Registry;

namespace points {

/// 文本 Token 估算 (provide)
/// - 请求: `{text}` 或 `{messages}` 二选一, 可带 `model` / `kind` / `countThinking`
/// - 结果: `{tokens}`
/// - 身份: `model|kind|fnv1a64(text)`
inline constexpr std::string_view kContextCountTokens = "agentxx.context.countTokens";

/// 上下文压缩 (provide)
/// - 请求: `{sessionId}` 或 `{messages}`, 可带 `model` / `targetTokens` /
///   `maxSummaryTokens` / `language`
/// - 结果: `{summary, truncated}`; 实现只产出摘要文本, 不写回会话
/// - 身份: `sessionId|messagesVersion|model`
inline constexpr std::string_view kContextSummarize = "agentxx.context.summarize";

/// 工具输出摘要 (provide; 第二批落地)
inline constexpr std::string_view kToolSummarizeOutput = "agentxx.tool.summarizeOutput";

/// 核心实现归属 (清单里的 `by` / `owner` 取值)
inline constexpr std::string_view kOwnerCountTokens = "core:context:tokenEstimate";
inline constexpr std::string_view kOwnerSummarize   = "core:context:summarize";

} // namespace points

// ==================== 文本 Token 估算 ====================

/// Token 估算的请求 (文本与消息二选一; 两个都给时以 messages 为准)
struct CountTokensRequest {
    std::string                        text;
    std::vector<neograph::ChatMessage> messages;
    std::string                        model;
    /// `text` / `messages`; 缺省按给了哪个字段判断
    std::string kind;
    /// 是否把 thinking (reasoning_content) 计入
    bool countThinking = false;
};

/// Token 估算的结果
struct CountTokensValue {
    int64_t tokens = 0;
};

template<>
struct Codec<CountTokensRequest> {
    static std::string toJson(const CountTokensRequest& req);
    static std::optional<CountTokensRequest> fromJson(std::string_view text);
};

template<>
struct Codec<CountTokensValue> {
    static std::string toJson(const CountTokensValue& value) {
        return fmt::format(R"({{"tokens":{}}})", value.tokens);
    }

    static std::optional<CountTokensValue> fromJson(std::string_view text);
};

/// 文本 Token 估算规则 (一份实现、两个入口)
///
/// - 同步快路径: TPS 计算 (`service.token.count`) 直连这里的 [estimateText],
///   插件实现不参与这条路径 (见设计文档 §8.1);
/// - 功能点: `agentxx.context.countTokens` 的核心实现调用同一份规则, 因此
///   "中间件旧函数" 与 "功能点取值" 结果必然一致。
struct TokenEstimator {
    /// 每个 token 大约为多少个 ascii 字符
    double asciiCharsPerToken = 4.0;
    /// 每个 token 大约为多少个 unicode (除去 ascii) 字符
    double unicodeCharsPerToken = 1.1;
    /// 每个图片 / 音频 / 视频附件的 token 估算
    double tokensPerImage = 400.0;
    /// 每条消息的额外 token 估算
    double extraTokensPerMessage = 3.0;

    /// UTF-8 文本的 token 估算 (ascii 与 unicode 分别折算后相加)
    size_t estimateText(std::string_view text) const;

    /// 一组消息的 token 估算 (含角色 / 正文 / 工具调用 / 多媒体附件)
    size_t
        estimateMessages(const std::vector<neograph::ChatMessage>& messages, bool countThinking)
            const;

    /// 身份串 (见 `points::kContextCountTokens` 说明)
    std::string identityOf(const CountTokensRequest& req) const;

    /// 规范请求的 JSON 形态
    static std::string requestToJson(const CountTokensRequest& req);
};

// ==================== 上下文压缩 ====================

/// 上下文压缩的请求
///
/// - 消息由编排方 (压缩中间件) 清理过: 噪音清理 / thinking 清理 / 工具折叠 /
///   多模态降级都在编排侧做完, 实现只负责产出摘要文本;
/// - `sessionId` 与 `messages` 二选一 (两个都给时以 `messages` 为准)。
struct SummarizeRequest {
    std::string                        sessionId;
    std::vector<neograph::ChatMessage> messages;
    std::string                        model;
    /// 压缩目标 (可选, 0 = 不指定)
    int64_t targetTokens = 0;
    /// 摘要长度上限 (可选, 0 = 用实现内置值)
    int64_t maxSummaryTokens = 0;
    std::string language;
    /// 手动触发 (agent 空闲时调用):
    /// - true: 核心实现直接派生压缩子代理并等待完成, 不抛中断 (与轮次无关)
    /// - false (默认): 轮次内自动触发, 核心实现走中断路径派生 (需要 AgentRunner 中断循环)
    bool manual = false;
};

/// 上下文压缩的结果 (只有摘要文本; 写回会话由编排方做)
struct SummarizeValue {
    std::string summary;
    bool        truncated = false;
};

template<>
struct Codec<SummarizeRequest> {
    static std::string toJson(const SummarizeRequest& req);
    static std::optional<SummarizeRequest> fromJson(std::string_view text);
};

template<>
struct Codec<SummarizeValue> {
    static std::string toJson(const SummarizeValue& value);
    static std::optional<SummarizeValue> fromJson(std::string_view text);
};

/// 摘要身份串: `sessionId|messagesVersion|model`
std::string summarizeIdentity(const SummarizeRequest& req);

// ==================== 核心点的实现装配 ====================

/// 上下文相关核心点的装配参数
struct ContextPointOptions {
    /// token 估算规则 (与压缩中间件的构造参数一致)
    TokenEstimator estimator;
    /// 压缩核心实现 (由压缩中间件提供: 内部派生"用完即弃"的压缩子代理, 只返回
    /// 摘要文本; 返回 nullopt = 没有核心实现 / 这次算不出来)
    /// - 实现里**不许**写回会话 / 不发提示消息 / 不写 share store: 这些是编排方的事
    std::function<asio::awaitable<std::optional<SummarizeValue>>(
        const SummarizeRequest&,
        const ImplContext&
    )>
        summarizeImpl;
};

/// 上下文相关核心点的引用集合 (由压缩中间件持有)
struct ContextPoints {
    ProvidePoint<CountTokensRequest, CountTokensValue>* countTokens = nullptr;
    ProvidePoint<SummarizeRequest, SummarizeValue>*     summarize  = nullptr;
};

/// 声明上下文两个核心点并登记核心实现 (由压缩中间件在装配期调用)
/// - 同一 AgentContext 重复调用 = 无害重声明 (点复用, 核心实现按归属覆盖)
ContextPoints registerContextPoints(Registry& registry, const ContextPointOptions& options);

} // namespace feature
} // namespace agentxx
