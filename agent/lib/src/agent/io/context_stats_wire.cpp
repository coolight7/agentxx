#include "agentxx/agent/io/context_stats_wire.h"

#include "agentxx/agent/agent_host.h"
#include "utilxx_base/json.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>

namespace agentxx {
namespace agent {
namespace io {

namespace {

/// 信息侧边栏「后台任务」段最多下发的条数
/// - 侧边栏只有几行可见高度, 发多了反而看不到标的 (完整台账由 `agentxx_async_query`
///   与插件接口给出)
constexpr size_t kAsyncWorksInStats = 10;

/// 挑选之前先从台账取的条数
/// - 台账按 `asyncId` 字典序 (async-10 排在 async-2 之前) 且含已结束的 linger 条目,
///   直接按上限取会把正在运行的挤掉; 先多取一批再按状态挑
constexpr size_t kAsyncWorksFetch = 100;

/// 状态分组权重 (界面按 运行 → 排队 → 完成 顺序分组显示)
int asyncWorkStateRank(std::string_view state) {
    if (state == "running") {
        return 0;
    }
    if (state == "queued") {
        return 1;
    }
    return 2; // done / failed / canceled
}

/// 台账条目 → 界面条目
///
/// 台账 (`AsyncWorkManager::listJson`) 是给模型/插件看的完整记录, 界面只需要少量
/// 字段: 状态、名字、已跑时长与排队位。已跑时长由时间戳当场算出 (运行中 = 现在 -
/// 开始, 已结束 = 结束 - 开始), 台账里不存"时长"避免每次写回。
///
/// - `args`:
///     - [works] `AgentHost::asyncWorksJson` 返回的数组
///     - [limit] 最多取几条 (界面限制 10 条, 避免侧边栏被挤满)
///
/// - `return` 可直接放进 `WireContextStats::asyncWorks` 的条目 (运行/排队在前)
std::vector<WireAsyncWorkItem>
    asyncWorkItemsOf(const utilxx_base::Json& works, size_t limit) {
    std::vector<WireAsyncWorkItem> out;
    if (!works.is_array() || limit == 0) {
        return out;
    }
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch()
    )
                           .count();

    /// 一条待排序的界面条目: 分组权重 + 组内顺序
    struct Row {
        WireAsyncWorkItem item;
        int               rank      = 2;
        int64_t           sortKeyMs = 0;
    };
    std::vector<Row> rows;
    rows.reserve(works.size());
    for (const auto& item : works) {
        WireAsyncWorkItem work;
        work.id    = item.value("asyncId", std::string{});
        work.kind  = item.value("kind", std::string{});
        work.name  = item.value("name", std::string{});
        work.state = item.value("state", std::string{});
        if (work.id.empty()) {
            continue;
        }
        const auto startedAtMs  = item.value("startedAtMs", int64_t{0});
        const auto finishedAtMs = item.value("finishedAtMs", int64_t{0});
        if (startedAtMs > 0) {
            const auto endMs = finishedAtMs > startedAtMs ? finishedAtMs : nowMs;
            work.elapsedMs   = std::max<int64_t>(0, endMs - startedAtMs);
        }
        work.queueAhead = static_cast<int32_t>(item.value("queueAhead", int64_t{0}));

        Row row;
        row.rank = asyncWorkStateRank(work.state);
        if (row.rank == 0) {
            row.sortKeyMs = startedAtMs; // 运行中: 先起跑的在前
        } else if (row.rank == 2) {
            row.sortKeyMs = -finishedAtMs; // 完成: 最近结束的在前
        }
        row.item = std::move(work);
        rows.push_back(std::move(row));
    }
    // 稳定排序: 同一权重与时间戳的条目保持台账原顺序
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.rank != b.rank) {
            return a.rank < b.rank;
        }
        return a.sortKeyMs < b.sortKeyMs;
    });
    const size_t take = std::min(rows.size(), limit);
    out.reserve(take);
    for (size_t i = 0; i < take; ++i) {
        out.push_back(std::move(rows[i].item));
    }
    return out;
}

} // namespace

WireContextStats makeWireContextStats(
    const std::shared_ptr<AgentContext>& ctx,
    std::string_view                     sessionId,
    const ContextStats&                  stats,
    double                               tps
) {
    WireContextStats out = toWireContextStats(stats, tps);
    if (!ctx || sessionId.empty()) {
        return out;
    }
    auto host = ctx->host.lock();
    if (!host) {
        return out;
    }
    out.asyncWorks = asyncWorkItemsOf(
        host->asyncWorksJson({}, sessionId, /*includeFinished=*/true, kAsyncWorksFetch),
        kAsyncWorksInStats
    );
    return out;
}

} // namespace io
} // namespace agent
} // namespace agentxx
