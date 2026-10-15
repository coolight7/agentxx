#pragma once

#include <algorithm>
#include <cstddef>

namespace agentxx {
namespace client {

/// 客户端本地展示消息条数上限 (TUI 消息列表; 从 TUI 抽出的可单测模型)
///
/// 背景: 服务端 viewMessages 与会话长度同增, 客户端只保留最近的一段用于渲染 ——
/// 超过上限、且**不在视口内**的头部消息在末尾追加 (push) 时被清理 (消息数据与
/// 渲染缓存一并释放), 上滚到已加载窗口顶部时再由历史分页拉回 (见 [HistoryWindow])。
///
/// 只限制条数, 不按字节限制: 长消息的渲染内存由 LazyScrollable 的缓存预算约束,
/// 单条消息本体的体积不参与判定。
inline constexpr size_t kLocalMessageLimit = 100;

/// 本地头部可清理条数 (纯函数, 便于在模型层穷举边界)
///
/// 清理条件 (必须全部满足, 否则返回 0):
/// - 条数超过上限 ([kLocalMessageLimit]);
/// - 滚动位置在底部 (`atBottom`: 用户正跟随最新内容; 上滚查看历史时不动列表);
/// - 只清理视口上方的消息: `firstVisibleMessageIndex` = 视口顶行所在消息下标,
///   该下标及其之后的都可能在屏幕上, 一律不动。
///
/// - `args`:
///     - [messageCount] 本地当前消息条数
///     - [firstVisibleMessageIndex] 视口顶行所在消息下标 (大于消息数时按消息数算)
///     - [atBottom] 消息列表当前是否吸附底部
///
/// - `return` 可安全清理的头部条数 (0 = 不清理)
inline size_t planLocalMessageTrim(
    const size_t messageCount,
    const size_t firstVisibleMessageIndex,
    const bool   atBottom
) noexcept {
    if (!atBottom || messageCount <= kLocalMessageLimit) {
        return 0;
    }
    // 超出上限的部分与"视口上方可清理的部分"取小: 只清理多余的那几条,
    // 不多删 (视口上方留着的消息就是用户上滚时的可读历史)
    const size_t excess = messageCount - kLocalMessageLimit;
    const size_t safe   = std::min(firstVisibleMessageIndex, messageCount);
    return std::min(excess, safe);
}

} // namespace client
} // namespace agentxx
