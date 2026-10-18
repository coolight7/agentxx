#include "agentxx/agent/prompt_sources.h"

#include "agentxx/util/exception.h"
#include "fmt/format.h"
#include "utilxx/async_offload.h"
#include "utilxx_base/container_util.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"

#include <algorithm>
#include <chrono>

namespace agentxx {
namespace agent {

namespace {

/// 单调毫秒 (只用于耗时统计)
int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

/// 原因短名 (日志/快照用)
const char* reasonKey(PromptRefreshReason reason) noexcept {
    switch (reason) {
        case PromptRefreshReason::Initial:
            return "initial";
        case PromptRefreshReason::ResourcesChanged:
            return "resources_changed";
        case PromptRefreshReason::AfterCompaction:
            return "after_compaction";
        case PromptRefreshReason::Explicit:
            return "explicit";
    }
    return "initial";
}

const char* slotKey(PromptSectionSlot slot) noexcept {
    return slot == PromptSectionSlot::Dynamic ? "dynamic" : "stable";
}

/// 空的"无内容"文本 (返回引用用)
const std::string& emptyText() {
    static const std::string kEmpty;
    return kEmpty;
}

} // namespace

// ---------------------------------------------------------------------------
// SourceState
// ---------------------------------------------------------------------------

const PromptSourceRegistry::TextSlot*
    PromptSourceRegistry::SourceState::findSlot(std::string_view sessionId) const {
    if (!src.perSession) {
        return &shared;
    }
    auto it = sessionSlots.find(sessionId);
    if (it == sessionSlots.end()) {
        return nullptr;
    }
    return &it->second;
}

PromptSourceRegistry::TextSlot&
    PromptSourceRegistry::SourceState::slotFor(std::string_view sessionId) {
    if (!src.perSession) {
        return shared;
    }
    std::string key{sessionId};
    return sessionSlots[std::move(key)];
}

const std::string& PromptSourceRegistry::SourceState::resolvedText(std::string_view sessionId
) const {
    const auto* slot = findSlot(sessionId);
    if (nullptr == slot) {
        return emptyText();
    }
    return slot->text;
}

// ---------------------------------------------------------------------------
// 登记 / 撤销 / 标记
// ---------------------------------------------------------------------------

void PromptSourceRegistry::registerSource(PromptSource src) {
    if (src.key.empty()) {
        XX_LOGW("[prompt] registerSource ignored: empty key (owner={})", src.owner);
        return;
    }
    if (!src.build) {
        XX_LOGW("[prompt] registerSource ignored: `{}` has no build function", src.key);
        return;
    }

    SourceState state;
    state.src = std::move(src);

    auto it = sources_.find(state.src.key);
    if (it != sources_.end()) {
        if (it->second.src.owner == state.src.owner) {
            // 同 owner 重新登记 (插件启用后重新声明): 沿用已取到的文本,
            // 避免无意义的前缀变化
            state.shared       = std::move(it->second.shared);
            state.sessionSlots = std::move(it->second.sessionSlots);
            state.lastReason   = it->second.lastReason;
            state.lastMs       = it->second.lastMs;
            state.chars        = it->second.chars;
            state.refreshes    = it->second.refreshes;
            state.changed      = it->second.changed;
            state.failures     = it->second.failures;
            state.lastError    = std::move(it->second.lastError);
            XX_LOGW(
                "[prompt] source `{}` re-registered by `{}`, keeping cached text",
                state.src.key,
                state.src.owner
            );
        } else {
            XX_LOGW(
                "[prompt] source `{}` owner changed `{}` -> `{}`, cached text dropped",
                state.src.key,
                it->second.src.owner,
                state.src.owner
            );
        }
        it->second = std::move(state);
    } else {
        XX_LOGD(
            "[prompt] source registered: key=`{}` owner=`{}` order={} slot={} perSession={}",
            state.src.key,
            state.src.owner,
            state.src.order,
            slotKey(state.src.slot),
            state.src.perSession
        );
        sources_.emplace(state.src.key, std::move(state));
    }

    // 新来源/新实现等于一次资源变更: 让所有会话的下一次请求重取
    markAllSessions(reasonBit(PromptRefreshReason::ResourcesChanged));
    sharedDirty_ = true;
}

bool PromptSourceRegistry::removeSource(std::string_view key, std::string_view owner) {
    auto it = sources_.find(key);
    if (it == sources_.end()) {
        return false;
    }
    if (!owner.empty() && it->second.src.owner != owner) {
        XX_LOGW(
            "[prompt] removeSource `{}` rejected: owned by `{}`, requested by `{}`",
            key,
            it->second.src.owner,
            owner
        );
        return false;
    }
    XX_LOGD("[prompt] source removed: key=`{}` owner=`{}`", key, it->second.src.owner);
    sources_.erase(it);
    // 段落被摘除同样是一次资源变更 (system 前缀会变短一次)
    markAllSessions(reasonBit(PromptRefreshReason::ResourcesChanged));
    sharedDirty_ = true;
    return true;
}

void PromptSourceRegistry::markDirty(std::string_view sessionId, PromptRefreshReason reason) {
    auto& state           = sessions_[std::string{sessionId}];
    state.pendingReasons |= reasonBit(reason);
}

void PromptSourceRegistry::markAllDirty(PromptRefreshReason reason) {
    markAllSessions(reasonBit(reason));
    sharedDirty_ = true;
}

void PromptSourceRegistry::markAllSessions(uint32_t bits) {
    for (auto& [sid, state] : sessions_) {
        (void)sid;
        state.pendingReasons |= bits;
    }
}

bool PromptSourceRegistry::isDirty(std::string_view sessionId) const {
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return true; // 还没刷新过
    }
    return !it->second.initialized || it->second.pendingReasons != 0 || sharedDirty_;
}

// ---------------------------------------------------------------------------
// 刷新
// ---------------------------------------------------------------------------

PromptRefreshReason PromptSourceRegistry::dominantReason(uint32_t bits, bool first) noexcept {
    if (first) {
        return PromptRefreshReason::Initial;
    }
    if (bits & reasonBit(PromptRefreshReason::ResourcesChanged)) {
        return PromptRefreshReason::ResourcesChanged;
    }
    if (bits & reasonBit(PromptRefreshReason::AfterCompaction)) {
        return PromptRefreshReason::AfterCompaction;
    }
    if (bits & reasonBit(PromptRefreshReason::Explicit)) {
        return PromptRefreshReason::Explicit;
    }
    return PromptRefreshReason::Initial;
}

bool PromptSourceRegistry::needsBuild(
    const SourceState&  state,
    const SessionState& session,
    bool                force,
    std::string_view    sessionId
) const {
    const auto* slot = state.findSlot(sessionId);
    if (nullptr == slot || !slot->built) {
        return true; // 该会话还没取过这个来源
    }
    if (force) {
        return true;
    }
    // 资源变更 / 显式请求: 全部来源重取
    if (session.pendingReasons
        & (reasonBit(PromptRefreshReason::ResourcesChanged)
           | reasonBit(PromptRefreshReason::Explicit))) {
        return true;
    }
    // 压缩完成: 只重取声明了 refreshOnCompaction 的来源
    if (session.pendingReasons & reasonBit(PromptRefreshReason::AfterCompaction)) {
        return state.src.refreshOnCompaction;
    }
    // 共享来源由 markAllDirty 置位 (只在第一个来刷新的会话里重取一次)
    if (!state.src.perSession && sharedDirty_) {
        return true;
    }
    return false;
}

std::vector<PromptSourceRegistry::SourceState*> PromptSourceRegistry::orderedSources() {
    std::vector<SourceState*> ordered;
    ordered.reserve(sources_.size());
    for (auto& [key, state] : sources_) {
        (void)key;
        ordered.push_back(&state);
    }
    std::sort(ordered.begin(), ordered.end(), [](const SourceState* a, const SourceState* b) {
        if (a->src.order != b->src.order) {
            return a->src.order < b->src.order;
        }
        return a->src.key < b->src.key;
    });
    return ordered;
}

asio::awaitable<void> PromptSourceRegistry::ensureFresh(
    std::string_view            sessionId,
    const PromptRefreshContext& inCtx,
    bool                        force
) {
    const std::string sid{sessionId};
    auto&             session = sessions_[sid];

    // 重入: 只保留 dirty 标记 (外层正在刷新, 结果对内层同样有效)
    if (session.busy) {
        co_return;
    }
    // 快路径: 一次布尔判断, 不产生任何 I/O
    if (!force && session.initialized && session.pendingReasons == 0 && !sharedDirty_) {
        lastStats_ = PromptRefreshStats{};
        co_return;
    }

    /// 刷新期间的 busy 复位 (异常/取消逃逸时也要复位)
    struct BusyGuard {
        SessionState& state;

        ~BusyGuard() {
            state.busy = false;
        }
    };

    session.busy = true;
    BusyGuard busyGuard{session};

    const auto startMs = steadyNowMs();
    const bool first   = !session.initialized;

    uint32_t bits = session.pendingReasons;
    if (first || force) {
        bits |= reasonBit(PromptRefreshReason::Initial);
    }
    const auto reason = dominantReason(bits, first);

    auto  ordered   = orderedSources();
    bool  cancelled = false;
    auto& stats     = lastStats_;
    stats           = PromptRefreshStats{};
    for (auto* state : ordered) {
        if (nullptr == state) {
            continue;
        }
        if (inCtx.cancelToken && inCtx.cancelToken->is_cancelled()) {
            cancelled = true;
            break;
        }
        if (!needsBuild(*state, session, force, sid)) {
            continue;
        }

        // build 拿一份上下文副本 (超时时协程被销毁, 不引用调用方栈上的对象)
        auto ctx       = std::make_shared<PromptRefreshContext>(inCtx);
        ctx->sessionId = sid;
        ctx->reason    = reason;
        ctx->first     = first;

        /// 取文本的结果: ok=false 表示异常/超时 (不是"没有意见")
        struct BuildResult {
            bool                       ok = false;
            std::optional<std::string> text;
            std::string                error;
        };

        const auto timeout
            = state->src.timeout.count() > 0 ? state->src.timeout : std::chrono::milliseconds{3000};
        const auto begin = steadyNowMs();

        auto outcome = co_await utilxx::asyncWithTimeout<BuildResult>(
            [cfg = state->src.build, ctx]() -> asio::awaitable<BuildResult> {
                co_return co_await agentxx::util::catchErrorAsync<BuildResult>(
                    [&]() -> asio::awaitable<BuildResult> {
                        auto text = co_await cfg(*ctx);
                        co_return BuildResult{.ok = true, .text = std::move(text)};
                    },
                    [](std::string errmsg) -> asio::awaitable<BuildResult> {
                        co_return BuildResult{
                            .ok    = false,
                            .text  = std::nullopt,
                            .error = std::move(errmsg)
                        };
                    },
                    ctx->cancelToken
                );
            },
            timeout,
            []() -> BuildResult {
                return BuildResult{
                    .ok    = false,
                    .text  = std::nullopt,
                    .error = "timeout",
                };
            }
        );

        const int64_t elapsed = steadyNowMs() - begin;
        ++state->refreshes;
        ++stats.refreshes;
        state->lastMs     = elapsed;
        state->lastReason = reason;

        if (!outcome.ok) {
            ++state->failures;
            ++stats.failures;
            state->lastError = outcome.error;
            XX_LOGW(
                "[prompt] source `{}` build failed after {}ms (reason={}, session={}): {}; "
                "keeping previous text",
                state->src.key,
                elapsed,
                reasonKey(reason),
                sid,
                outcome.error
            );
            continue;
        }
        // 没有意见 (nullopt): 保留上次文本, 不算失败也不算变化
        if (!outcome.text.has_value()) {
            continue;
        }

        auto&      slot     = state->slotFor(sid);
        const bool wasBuilt = slot.built;
        const bool differs  = wasBuilt && slot.text != *outcome.text;
        const auto oldChars = slot.text.empty() ? size_t{0} : utilxx_base::utf8GetLength(slot.text);
        slot.text           = std::move(*outcome.text);
        slot.built          = true;
        state->chars        = utilxx_base::utf8GetLength(slot.text);
        if (differs) {
            ++state->changed;
            ++stats.changedSections;
            XX_LOGI(
                "[prompt] section `{}` changed ({} -> {} chars, reason={}, session={}); "
                "system prefix will be rebuilt once",
                state->src.key,
                oldChars,
                state->chars,
                reasonKey(reason),
                sid
            );
        }
    }

    if (cancelled) {
        // 取消: 保留 dirty 标记与旧文本, 下次请求再试
        XX_LOGD(
            "[prompt] refresh cancelled for session {} (reason={}), keeping old text",
            sid,
            reasonKey(reason)
        );
        stats.lastMs = steadyNowMs() - startMs;
        co_return;
    }

    session.initialized    = true;
    session.pendingReasons = 0;
    sharedDirty_           = false;

    stats.lastMs = steadyNowMs() - startMs;
    for (auto* state : ordered) {
        if (nullptr != state) {
            const auto& text = state->resolvedText(sid);
            if (!text.empty()) {
                stats.sectionChars += utilxx_base::utf8GetLength(text);
            }
        }
    }
    co_return;
}

// ---------------------------------------------------------------------------
// 视图 / 查询 / 回收 / 快照
// ---------------------------------------------------------------------------

std::vector<PromptSectionView> PromptSourceRegistry::stableSections(std::string_view sessionId
) const {
    std::vector<const SourceState*> ordered;
    ordered.reserve(sources_.size());
    for (const auto& [key, state] : sources_) {
        (void)key;
        if (state.src.slot == PromptSectionSlot::Stable) {
            ordered.push_back(&state);
        }
    }
    std::sort(ordered.begin(), ordered.end(), [](const SourceState* a, const SourceState* b) {
        if (a->src.order != b->src.order) {
            return a->src.order < b->src.order;
        }
        return a->src.key < b->src.key;
    });

    std::vector<PromptSectionView> views;
    views.reserve(ordered.size());
    for (const auto* state : ordered) {
        const auto& text = state->resolvedText(sessionId);
        if (text.empty()) {
            continue; // 空段落不占位
        }
        views.push_back(PromptSectionView{
            .key   = state->src.key,
            .text  = text,
            .order = state->src.order,
            .owner = state->src.owner,
            .scope = PromptSectionScope::Session,
            .slot  = state->src.slot,
        });
    }
    return views;
}

std::vector<PromptSectionView> PromptSourceRegistry::dynamicSections(std::string_view sessionId
) const {
    std::vector<PromptSectionView> views;
    for (const auto& [key, state] : sources_) {
        (void)key;
        if (state.src.slot != PromptSectionSlot::Dynamic) {
            continue;
        }
        const auto& text = state.resolvedText(sessionId);
        if (text.empty()) {
            continue;
        }
        views.push_back(PromptSectionView{
            .key   = state.src.key,
            .text  = text,
            .order = state.src.order,
            .owner = state.src.owner,
            .scope = PromptSectionScope::Session,
            .slot  = state.src.slot,
        });
    }
    std::sort(
        views.begin(),
        views.end(),
        [](const PromptSectionView& a, const PromptSectionView& b) {
            if (a.order != b.order) {
                return a.order < b.order;
            }
            return a.key < b.key;
        }
    );
    return views;
}

std::string
    PromptSourceRegistry::sectionText(std::string_view key, std::string_view sessionId) const {
    auto it = sources_.find(key);
    if (it == sources_.end()) {
        return {};
    }
    return it->second.resolvedText(sessionId);
}

size_t PromptSourceRegistry::sourceCount() const {
    return sources_.size();
}

size_t PromptSourceRegistry::sessionCount() const {
    return sessions_.size();
}

void PromptSourceRegistry::dropSession(std::string_view sessionId) {
    for (auto& [key, state] : sources_) {
        (void)key;
        if (state.src.perSession) {
            utilxx_base::eraseHeterogeneous(state.sessionSlots, sessionId);
        }
    }
    utilxx_base::eraseHeterogeneous(sessions_, sessionId);
}

PromptRefreshStats PromptSourceRegistry::takeRefreshStats() {
    auto out   = lastStats_;
    lastStats_ = PromptRefreshStats{};
    return out;
}

utilxx_base::Json PromptSourceRegistry::snapshot() const {
    utilxx_base::Json root = utilxx_base::Json::object();
    root["count"]          = sources_.size();
    root["sessions"]       = sessions_.size();
    root["shared_dirty"]   = sharedDirty_;

    size_t totalChars = 0;
    auto   sources    = utilxx_base::Json::array();
    for (const auto& [key, state] : sources_) {
        (void)key;
        utilxx_base::Json item        = utilxx_base::Json::object();
        item["key"]                   = state.src.key;
        item["owner"]                 = state.src.owner;
        item["order"]                 = state.src.order;
        item["slot"]                  = slotKey(state.src.slot);
        item["per_session"]           = state.src.perSession;
        item["refresh_on_compaction"] = state.src.refreshOnCompaction;
        item["last_reason"]           = reasonKey(state.lastReason);
        item["last_ms"]               = state.lastMs;
        item["chars"]                 = state.chars;
        item["refreshes"]             = state.refreshes;
        item["changed"]               = state.changed;
        item["failures"]              = state.failures;
        // 已取到文本的会话数 (共享来源: 0 或 1; 按会话来源: 已取过的会话数)
        size_t builtSessions = 0;
        if (state.src.perSession) {
            for (const auto& [sid, slot] : state.sessionSlots) {
                (void)sid;
                if (slot.built) {
                    ++builtSessions;
                }
            }
        } else {
            builtSessions = state.shared.built ? 1 : 0;
        }
        item["built_sessions"] = builtSessions;
        if (!state.lastError.empty()) {
            item["last_error"] = state.lastError;
        }
        totalChars += state.chars;
        sources.push_back(std::move(item));
    }
    root["sources"]     = std::move(sources);
    root["total_chars"] = totalChars;
    return root;
}

} // namespace agent
} // namespace agentxx
