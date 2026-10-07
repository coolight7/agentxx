#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace agentxx {
namespace client {

/// 展示历史的分页窗口 (计划 UI-1: 从 TUI 抽出的可单测模型, 无 FTXUI/无 IO 依赖)
///
/// 背景: 长会话恢复时服务端只同步末尾窗口, 用户向上滚动时按绝对下标分页拉取
/// 更早历史。窗口的"上下边界 + 未完成的请求 + 序号连续性"判断原先写在 TUI 端点里,
/// 与渲染/线程代码缠在一起, 无法脱离终端测试。这里把状态机收敛成一个可拷贝的
/// 值类型 (随渲染快照一起复制), 端点只负责发请求与渲染。
///
/// 语义:
/// - 已加载窗口是**连续区间** `[windowStart, windowStart + loadedCount)`;
///   页响应必须紧贴当前窗口首条 (`page.startIndex + count == windowStart`) 才被接受,
///   否则视为窗口已被整体替换后的过期响应, 丢弃 (返回 NonContiguous);
/// - `windowStart == 0` 表示"上方没有更早历史" (全量同步或已拉到会话开头);
/// - 断线重连走增量补拉时, 服务端下发的尾部序号用于检测空洞: 序号不连续
///   (期望 lastSeq+1) 说明中间有消息没收到, 调用方应改走全量/尾窗同步。
class HistoryWindow {
public:

    /// 页响应处理结果
    enum class PageOutcome {
        Accepted,       ///< 页已可用 (调用方前插后调用 [notePrepended])
        Empty,          ///< 空页 = 上方没有更早历史, 窗口起点归零
        StaleSession,   ///< 迟到响应 (会话已切换), 丢弃
        NotLoading,     ///< 当前没有未完成的请求 (重复/意外响应), 丢弃
        NonContiguous,  ///< 页不紧贴窗口首条 (窗口已被整体替换), 丢弃
    };

    /// 尾部序号观察结果 (断线/增量补拉场景)
    enum class SeqOutcome {
        First,  ///< 首个被观察到的序号 (开始时基线, 不作连续性判断)
        Ok,     ///< 与服务端下发顺序连续
        Gap,    ///< 中间缺号 (需要全量/尾窗同步补齐)
        Repeat, ///< 与上一序号相同或更小 (重复投递, 忽略)
    };

    /// 窗口整体替换 (全量/尾窗同步)
    /// - `windowStart` = 本批消息的起始绝对下标 (0 = 全量, 上方无更早历史)
    /// - `totalCount` = 服务端会话总消息数 (0 = 未知, 保持上次值)
    /// - `loadedCount` = 本批实际装载条数 (界面过滤空消息后可能少于服务端条数)
    void reset(uint64_t windowStart, uint64_t totalCount, uint64_t loadedCount);

    /// 切换会话: 窗口作废, 未完成的请求随之失效 (迟到响应返回 [PageOutcome::StaleSession])
    void resetForSession(std::string sessionId);
    const std::string& sessionId() const noexcept {
        return sessionId_;
    }

    /// 开始一次"拉取更早一页"的请求; 不满足条件 (加载中 / 上方无更早历史) 返回 false
    /// - 成功后 [loading] 为 true, 直到 [applyPage] 或 [abortLoading] 结束
    bool beginOlderPageRequest();
    bool loading() const noexcept {
        return loading_;
    }
    void abortLoading() noexcept {
        loading_ = false;
    }

    /// 处理一页更早历史的响应 (不改变窗口, 判定交给调用方: 接受时随后调用
    /// [notePrepended] 把页内条数计入窗口)
    PageOutcome applyPage(std::string_view pageSessionId, uint64_t startIndex, uint64_t count, uint64_t totalCount);

    /// 前插完成: 窗口起点左移到 `startIndex`, 已加载条数增加 (接受页后调用)
    void notePrepended(uint64_t startIndex, uint64_t prependedCount);

    /// 增量补拉 (断线恢复) 追加尾部消息: 只更新总数/条数, 窗口起点不变
    void noteTailAppended(uint64_t appendedCount, uint64_t totalCount);

    /// 观察服务端下发的尾部序号 (增量补拉/实时 delta 的序号)
    SeqOutcome observeTailSeq(uint64_t seq);
    uint64_t    lastSeq() const noexcept {
        return lastSeq_;
    }
    bool hasSeq() const noexcept {
        return hasSeq_;
    }
    bool hasGap() const noexcept {
        return gap_;
    }

    /// 窗口起点 (0 = 上方无更早历史)
    uint64_t windowStart() const noexcept {
        return windowStart_;
    }
    /// 服务端会话总消息数 (0 = 未知)
    uint64_t totalCount() const noexcept {
        return totalCount_;
    }
    /// 已加载区间内的条数 (界面过滤空消息后的实际条数)
    uint64_t loadedCount() const noexcept {
        return loadedCount_;
    }
    /// 上方是否还有更早历史
    bool hasMore() const noexcept {
        return windowStart_ > 0;
    }

private:

    /// 会话标识 (用于丢弃切换会话后迟到的页响应)
    std::string sessionId_;
    /// 已加载窗口首条消息的绝对下标
    uint64_t windowStart_ = 0;
    /// 服务端会话总消息数
    uint64_t totalCount_ = 0;
    /// 已加载条数
    uint64_t loadedCount_ = 0;
    /// 是否有未完成的分页请求
    bool loading_ = false;
    /// 最近观察到的尾部序号
    uint64_t lastSeq_ = 0;
    bool     hasSeq_  = false;
    /// 最近一次序号观察是否发现空洞 (下一次连续观察会清除)
    bool gap_ = false;
};

} // namespace client
} // namespace agentxx
