/// HTTP 错误类型 (agentxx::util::RateLimitError)
///
/// - 从 `neograph::RateLimitError` 迁移而来: util 层不再依赖图引擎头,
///   限流语义由 util 自有类型承载
/// - 与 neograph 版本二进制无关, 仅按消息 + retry-after 秒数传递
#pragma once

#include <stdexcept>
#include <string>

namespace agentxx {
namespace util {

/// 上游 API 返回 HTTP 429 (限流) 时抛出
///
/// - `retry_after_seconds()`: 上游 `Retry-After` 头秒数, 无可用值时为 -1
///   (调用方优先采用正值, 否则用自身默认退避)
class RateLimitError : public std::runtime_error {
public:

    explicit RateLimitError(const std::string& message, int retry_after_seconds = -1) :
        std::runtime_error(message),
        retry_after_seconds_(retry_after_seconds) {}

    int retry_after_seconds() const noexcept {
        return retry_after_seconds_;
    }

private:

    int retry_after_seconds_ = -1;
};

} // namespace util
} // namespace agentxx
