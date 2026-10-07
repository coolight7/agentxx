#include "agentxx-client/io/tui/model/history_window.h"

namespace agentxx {
namespace client {

void HistoryWindow::reset(
    const uint64_t windowStart,
    const uint64_t totalCount,
    const uint64_t loadedCount
) {
    windowStart_ = windowStart;
    loadedCount_ = loadedCount;
    if (totalCount != 0) {
        totalCount_ = totalCount;
    }
    loading_ = false;
    // 窗口整体替换后旧的序号基线不再有效 (新窗口的尾部序号由后续增量观察给出)
    hasSeq_ = false;
    gap_    = false;
}

void HistoryWindow::resetForSession(std::string sessionId) {
    sessionId_   = std::move(sessionId);
    windowStart_ = 0;
    totalCount_  = 0;
    loadedCount_ = 0;
    loading_     = false;
    lastSeq_     = 0;
    hasSeq_      = false;
    gap_         = false;
}

bool HistoryWindow::beginOlderPageRequest() {
    if (loading_ || !hasMore()) {
        return false;
    }
    loading_ = true;
    return true;
}

HistoryWindow::PageOutcome HistoryWindow::applyPage(
    const std::string_view pageSessionId,
    const uint64_t         startIndex,
    const uint64_t         count,
    const uint64_t         totalCount
) {
    // 请求生命周期结束: 无论本页是否可用, 加载标志都要复位
    loading_ = false;

    if (!pageSessionId.empty() && !sessionId_.empty() && pageSessionId != sessionId_) {
        return PageOutcome::StaleSession;
    }
    if (count == 0) {
        // 空页 = 上方没有更早历史 (窗口起点归零, 终止后续触发)
        windowStart_ = 0;
        if (totalCount != 0) {
            totalCount_ = totalCount;
        }
        return PageOutcome::Empty;
    }
    if (totalCount > totalCount_) {
        totalCount_ = totalCount;
    }
    // 连续性: 页尾必须紧贴当前窗口首条。窗口为空 (首次填充) 时不做判断,
    // 由调用方决定是否接受 (TUI 在首屏填充与向上分页都走这里)。
    const uint64_t pageEnd = startIndex + count;
    if (loadedCount_ != 0 || windowStart_ != 0) {
        if (pageEnd != windowStart_) {
            return PageOutcome::NonContiguous;
        }
    }
    return PageOutcome::Accepted;
}

void HistoryWindow::notePrepended(const uint64_t startIndex, const uint64_t prependedCount) {
    // 前插后窗口起点左移到本页起始下标; 条数按实际入列表的条数累加
    // (界面过滤掉空 content 消息时 prependedCount 会小于页内条数, 但绝对下标
    //  仍以服务端 startIndex 为准 —— 两者下标基准不同, 不能混算)
    if (prependedCount == 0 && startIndex == windowStart_) {
        return;
    }
    windowStart_ = startIndex;
    loadedCount_ += prependedCount;
}

void HistoryWindow::noteTailAppended(const uint64_t appendedCount, const uint64_t totalCount) {
    loadedCount_ += appendedCount;
    if (totalCount != 0) {
        totalCount_ = totalCount;
    }
}

HistoryWindow::SeqOutcome HistoryWindow::observeTailSeq(const uint64_t seq) {
    if (!hasSeq_) {
        hasSeq_  = true;
        lastSeq_ = seq;
        gap_     = false;
        return SeqOutcome::First;
    }
    if (seq <= lastSeq_) {
        // 重复投递 (同一消息的多次补拉) 或乱序的旧消息: 不算空洞
        return SeqOutcome::Repeat;
    }
    const bool continuous = (seq == lastSeq_ + 1);
    lastSeq_              = seq;
    gap_                  = !continuous;
    return continuous ? SeqOutcome::Ok : SeqOutcome::Gap;
}

} // namespace client
} // namespace agentxx
