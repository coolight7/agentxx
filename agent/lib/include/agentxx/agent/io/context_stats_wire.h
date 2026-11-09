#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/agent/io/agent_io_transport.h"
#include <algorithm>
#include <memory>
#include <string_view>

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
/// 会话标题 (ContextStats::title) 于所有推送点一并下发 —— 界面据此显示当前会话标题,
/// 标题在会话加载/自动命名/改名后才变, 取不到时为空串 (界面保留原显示)。
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
    out.sessionTitle     = stats.title;
    // 累计用量按非负值下发 (统计本身只增, 负数只可能来自脏数据)
    out.promptTokens = static_cast<uint64_t>(std::max<int64_t>(0, stats.usage.promptTokens));
    out.completionTokens
        = static_cast<uint64_t>(std::max<int64_t>(0, stats.usage.completionTokens));
    out.totalTokens = static_cast<uint64_t>(std::max<int64_t>(0, stats.usage.totalTokens));
    out.cachedPromptTokens
        = static_cast<uint64_t>(std::max<int64_t>(0, stats.usage.cachedPromptTokens));
    return out;
}

/// 会话上下文统计 → 线消息 (服务端所有推送点的统一入口)
///
/// 与 [toWireContextStats] 的区别只有一个: 这里额外填「后台任务」段
/// (`WireContextStats::asyncWorks`)。该段需要 agent 容器 (台账在 `AgentHost` 上)
/// 与会话 id, 只有本函数拿得到, 因此**服务端的每次统计推送都要走这里** ——
/// 少走一处就会让界面上的后台任务段在那种推送之后变成空快照 (界面按整份覆盖)。
///
/// - 无容器 (单机库调用 / 未挂载) 或无会话 id 时该段保持空数组 (= 没有任务)
/// - 条目挑选顺序: 运行 → 排队 → 完成 (完成按结束时间倒序), 上限 10 条
///   (界面只关心正在发生什么, 完成态只是附带信息)
///
/// - `args`:
///     - [ctx]       会话所属的 agent 上下文 (可为空)
///     - [sessionId] 会话 id (空 = 不查台账)
///     - [stats]     会话持有的统计 (Session::contextStats; 必须非空)
///     - [tps]       当前 ModelCall 的平均生成速度 (token/s); 0 表示本次不报速度
///
/// - `return` 可直接发往对端的线消息
WireContextStats makeWireContextStats(
    const std::shared_ptr<AgentContext>& ctx,
    std::string_view                     sessionId,
    const ContextStats&                  stats,
    double                               tps = 0.0
);

} // namespace io
} // namespace agent
} // namespace agentxx
