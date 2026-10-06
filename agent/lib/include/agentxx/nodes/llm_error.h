#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace agentxx {
namespace nodes {

/// LLM 请求失败的分类
///
/// 分类只依赖错误文本 (provider 错误消息 / boost 诊断信息 / HTTP 状态码文本),
/// 决定三件事: 能否自动重试、退避多久、是否属于"上下文超限"。
/// 展示文本与分类分开: 文本给人看, 分类给重试策略用。
///
/// 各家 provider 的错误结构不同, 因此按"状态码 + 常见关键词"双口径识别;
/// 未匹配到任何特征时归为 [Unknown] (按可重试处理, 与历史行为一致)。
enum class LlmErrorKind {
    /// 未匹配到已知特征
    Unknown,
    /// 鉴权失败 (401/403, invalid api key 等): 重试无意义
    Auth,
    /// 额度/计费问题 (402, insufficient_quota, billing, 余额不足 等): 重试无意义
    Quota,
    /// 请求本身非法 (400, invalid_request_error, 参数不支持 等): 重试无意义
    InvalidRequest,
    /// 上下文超限 (context_length_exceeded / maximum context length / 413 等):
    /// 压缩一次后重试 (见 RetryPolicy 的 LLM-3 处理)
    ContextOverflow,
    /// 限流 (429, rate limit, 请求频率 等): 可重试, 优先采用响应里的 retry-after
    RateLimit,
    /// 超时/连接中断 (timeout, connection reset, EOF 等): 可重试
    Timeout,
    /// 服务端错误 (5xx, overloaded, 服务不可用 等): 可重试
    Server,
};

/// 错误文本分类
/// - 顺序敏感: 先判定上下文超限 (400 也可能带 context_length_exceeded), 再分鉴权/
///   额度/请求非法/限流/超时/服务端
LlmErrorKind classifyLlmError(std::string_view errInfo) noexcept;

/// 该类别是否允许自动重试 (否定类: 鉴权 / 额度 / 请求非法)
bool isLlmErrorRetryable(LlmErrorKind kind) noexcept;

/// 该类别是否表示"上下文超限" (触发一次压缩后重试)
bool isLlmContextOverflow(LlmErrorKind kind) noexcept;

/// 分类的可读文本 (日志与 UI 提示共用, 避免各处写不同的说法)
std::string_view llmErrorKindText(LlmErrorKind kind) noexcept;

/// 计算下次重试等待时长 (秒)
/// - 有界指数退避: 基数随类别不同 (限流更大), 按 `attempt` 指数增长并夹到上限
/// - 抖动: 按错误文本的哈希加 0~2 秒抖动, 避免多会话同时重试形成尖峰
/// - 错误文本里带 `retry-after: N` / `retry_after": N` 时采用该值 (夹到上限)
///
/// - `args`:
///     - [kind]    错误分类
///     - [attempt] 第几次重试 (从 1 开始)
///     - [errInfo] 错误文本 (用于读取 retry-after 与抖动种子)
int llmRetryDelaySeconds(LlmErrorKind kind, size_t attempt, std::string_view errInfo) noexcept;

/// 退避上限 (秒): 单个重试的最长等待
inline constexpr int kLlmRetryMaxDelaySeconds = 60;

} // namespace nodes
} // namespace agentxx
