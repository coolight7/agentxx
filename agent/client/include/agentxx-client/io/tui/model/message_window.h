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

/// 视口上方额外保留的消息条数 (用户往上滑一点时还有内容可看)
inline constexpr size_t kLocalMessageViewportMargin = 3;

/// 本地头部可清理条数 (纯函数, 便于在模型层穷举边界)
///
/// 保留窗口 = max(末尾 [kLocalMessageLimit] 条, 视口顶行及其后的全部消息 +
/// 视口上方 [kLocalMessageViewportMargin] 条), 比它更旧的头部消息全部清理:
/// - 未上滚 (跟随最新内容): 视口就在末尾, 按末尾 [kLocalMessageLimit] 条保留;
/// - 上滚查看历史 (视口在倒数第 N 条): 保留末尾 N + [kLocalMessageViewportMargin] 条,
///   视口之前的更早消息照样清理 —— 它们留在服务端展示历史里, 上滚到窗口顶部时
///   按页拉回 (见 [HistoryWindow])。
///
/// 视口内的消息一律不动: 返回值最多删到"视口上方保留的那几条"之前。
///
/// - `args`:
///     - [messageCount] 本地当前消息条数
///     - [firstVisibleMessageIndex] 视口顶行所在消息下标 (大于消息数时按消息数算;
///       锚点落在流式/占位子项上时同样按消息数算, 即消息全都在视口上方)
///
/// - `return` 可清理的头部条数 (0 = 不清理)
inline size_t planLocalMessageTrim(
    const size_t messageCount,
    const size_t firstVisibleMessageIndex
) noexcept {
    const size_t visible = std::min(firstVisibleMessageIndex, messageCount);
    // 视口顶行及其后的消息都在屏幕上 (或即将被看到), 必须留着
    const size_t fromViewport = messageCount - visible;
    // 视口上方再留几条: visible 不足 margin 时最多留到第 0 条
    const size_t keepForViewport = fromViewport + std::min(visible, kLocalMessageViewportMargin);
    const size_t keep            = std::max(kLocalMessageLimit, keepForViewport);
    if (messageCount <= keep) {
        return 0;
    }
    const size_t trim = messageCount - keep;
    // 兜底: 视口内的消息一条都不删 (keepForViewport 已保证, 防止上面规则改动后越界)
    const size_t safe
        = (visible > kLocalMessageViewportMargin) ? (visible - kLocalMessageViewportMargin) : 0;
    return std::min(trim, safe);
}

} // namespace client
} // namespace agentxx
