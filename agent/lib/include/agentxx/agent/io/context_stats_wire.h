#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/agent/io/agent_io_transport.h"
#include <algorithm>

namespace agentxx {
namespace agent {
namespace io {

/// 会话上下文统计 → 线消息 `context_stats` (唯一组装点)
///
/// 服务端把会话的上下文占用与累计用量下发给客户端时都经这里组装, 避免多处各写
/// 一份字段拷贝 (新增统计字段时只改这一处)。各推送点的差别只有 `tps`:
///
/// - `handleNodeEnd` / `sendContextStats` 只报占用与用量, `tps` 取 0
/// - 流式期间的定时推送 (EventBridge::pushTpsIfDue) 额外带上当前窗口的生成速度
///
/// - `args`:
///     - [stats] 会话持有的统计 (Session::contextStats; 必须非空)
///     - [tps]   当前 ModelCall 的平均生成速度 (token/s); 0 表示本次不报速度
///
/// - `return` 可直接发往对端的线消息 (累计用量字段一并填充)
inline WireContextStats toWireContextStats(const ContextStats& stats, double tps = 0.0) {
    WireContextStats out;
    out.contextTokens    = static_cast<uint64_t>(stats.contextTokens);
    out.maxContextTokens = static_cast<uint64_t>(stats.maxContextTokens);
    out.tps              = tps;
    // 累计用量按非负值下发 (统计本身只增, 负数只可能来自脏数据)
    out.promptTokens       = static_cast<uint64_t>(std::max<int64_t>(0, stats.usage.promptTokens));
    out.completionTokens
        = static_cast<uint64_t>(std::max<int64_t>(0, stats.usage.completionTokens));
    out.totalTokens = static_cast<uint64_t>(std::max<int64_t>(0, stats.usage.totalTokens));
    out.cachedPromptTokens
        = static_cast<uint64_t>(std::max<int64_t>(0, stats.usage.cachedPromptTokens));
    return out;
}

} // namespace io
} // namespace agent
} // namespace agentxx
