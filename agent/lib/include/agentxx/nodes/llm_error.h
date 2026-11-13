#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace agentxx {
namespace nodes {

/// LLM 请求失败的分类
///
/// **重试策略只有一条: 任何失败都会重试**, 直到 `llm.max_retry` 用完 (取消除外)。
/// 分类**只决定重试前等多久** (见 [llmRetryDelaySeconds]), 不参与"值不值得重试"的判断
/// (原因见下面的警告)。展示文本与分类分开: 文本给人看, 分类给退避策略用。
///
/// 改动这里之前先读两条警告 (都是实测踩过的坑):
/// - **不要加"这类错误不值得重试"的判断**。上游回的状态码说明不了请求本身有没有问题
///   (代理、网关、半个响应都会回 401/400/402), 停止重试只会让本来重试一次就过去的
///   失败直接结束本轮。想少重试就调小 `llm.max_retry`。
/// - **不要从错误文本/网络响应里判断"上下文超限", 也不要在超限时压缩后重试**。
///   按 400/413 与 "context length" 一类字样推断经常误判普通失败, 误判的代价是白压一次
///   历史 (信息真的丢了)。上下文长度只由自动压缩处理 (超过上限 75% 时触发, 见
///   `agentxx::middleware::SummarizationMiddlewareHandle`), 真到超限的情况基本不会出现。
///
/// 分类依据是错误文本 (provider 错误消息 / boost 诊断信息 / HTTP 状态码文本);
/// 各家 provider 的错误结构不同, 因此按"状态码 + 常见关键词"识别, 未匹配到任何特征时
/// 归为 [Unknown] (退避基数与超时相同)。
enum class LlmErrorKind {
    /// 未匹配到已知特征 (退避基数 2 秒)
    Unknown,
    /// 鉴权失败 (401/403, invalid api key 等; 退避基数 2 秒)
    Auth,
    /// 额度/计费问题 (402, insufficient_quota, billing, 余额不足 等; 退避基数 2 秒)
    Quota,
    /// 请求本身非法 (400, invalid_request_error, 参数不支持 等; 退避基数 2 秒)
    InvalidRequest,
    /// 限流 (429, rate limit, 请求频率 等; 退避基数 5 秒, 优先采用响应里的 retry-after)
    RateLimit,
    /// 超时 (timeout, SSE read timeout, 操作超时 等; 退避基数 2 秒)
    Timeout,
    /// 网络/传输层故障 (连接被拒绝/重置/关闭、域名解析失败、TLS 握手与证书问题、
    /// 代理/隧道、HTTP/2 流控、平台 socket 错误码、响应流被截断、读写 I/O 出错 等;
    /// 退避基数 1 秒 —— 这类失败多为瞬时, 尽快重来)
    ///
    /// 判定排在**所有**状态码类之前: 这类文本常夹带地址与端口数字
    /// (如 `https://host:10403 connect failed`)、我们自己的诊断后缀 (`recv=403684B`)、
    /// 上游回的半个响应, 先判网络才能给出最短的退避。
    Network,
    /// 服务端错误 (5xx, overloaded, 服务不可用 等; 退避基数 3 秒)
    Server,
};

/// 错误文本分类 (**只用于决定重试的退避时长**)
/// - 顺序敏感: **网络** → 鉴权 → 额度 → 请求非法 → 限流 → 超时 → 服务端
///   - 网络最前: 见 [LlmErrorKind::Network](网络失败文本里的状态码/鉴权字样只当噪声)
/// - 状态码 (`401` / `403` / `429` ...) 只在**独立成词**时才算: 前后不能是 ASCII
///   字母/数字, 否则 `recv=403684B` 一类字节数会被当成鉴权失败 (退避随之变长)
/// - 我们自己拼在末尾的流式诊断后缀 (` | stream: ...`) 本身是网络层失败的硬指标,
///   但后缀里的字节数与上游原文不参与其余规则的判定
LlmErrorKind classifyLlmError(std::string_view errInfo) noexcept;

/// 分类的可读文本 (日志与 UI 提示共用, 避免各处写不同的说法)
std::string_view llmErrorKindText(LlmErrorKind kind) noexcept;

/// 计算下次重试等待时长 (秒) —— **这是分类唯一的用途**
/// - 指数退避: 基数随类别不同 (网络 1 / 超时与未知 2 / 鉴权、额度、请求非法 2 /
///   服务端 3 / 限流 5 秒), 按 `attempt` 指数增长并夹到上限
/// - 类别只决定等多久, 不决定是否重试: 鉴权/额度/请求非法同样会重试, 次数上限只由
///   `llm.max_retry` 控制 (不在这里判)
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
