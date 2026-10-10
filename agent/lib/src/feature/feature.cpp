/// agentxx 功能点子系统的类型擦除部分实现
///
/// 本文件承载 [PointBase] 与 [JsonProvidePoint] 的实现: id / 选项 / 插件层实现链 /
/// 值缓存 / 置空标记 / in-flight 去重 / 统计 / 清单。类型化的取值逻辑在
/// `feature.h` 的 [ProvidePoint] 模板里 (核心实现不经 JSON, 热路径零拷贝)。
///
/// 线程模型: 功能点的状态只在宿主 io 线程上访问 (与事件总线/会话一致),
/// 因此这里没有任何锁; 插件侧入口经宿主投递 (见 plugin_manager_feature.cpp)。
#include "agentxx/feature/feature.h"

#include "agentxx/agent/config_static.h"
#include "utilxx/async_offload.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "fmt/format.h"
#include <algorithm>
#include <stdexcept>

namespace agentxx {
namespace feature {

// ==================== 对外字符串 ====================

const char* pointTypeKey(PointType type) noexcept {
    switch (type) {
        case PointType::Provide:
            return "provide";
        case PointType::Decide:
            return "decide";
        case PointType::Notify:
            return "notify";
    }
    return "provide";
}

const char* implLayerKey(ImplLayer layer) noexcept {
    return (layer == ImplLayer::Plugin) ? "plugin" : "core";
}

const char* cacheModeKey(CacheMode mode) noexcept {
    switch (mode) {
        case CacheMode::None:
            return "none";
        case CacheMode::Latest:
            return "latest";
        case CacheMode::ByIdentity:
            return "by_identity";
    }
    return "none";
}

const char* callErrorKey(CallError error) noexcept {
    switch (error) {
        case CallError::None:
            return "ok";
        case CallError::NotCallable:
            return "not_callable";
        case CallError::BadArgs:
            return "bad_args";
        case CallError::NoImpl:
            return "no_impl";
        case CallError::Disabled:
            return "disabled";
        case CallError::Busy:
            return "busy";
        case CallError::Failed:
            return "failed";
    }
    return "failed";
}

namespace {

/// 安全解析 JSON 文本 (失败返回 null; 非法参数在入口处已按 bad_args 拒绝)
utilxx_base::Json parseOrNull(std::string_view text) {
    if (text.empty()) {
        return utilxx_base::Json{};
    }
    return utilxx_base::catchError<utilxx_base::Json>(
        [&]() -> utilxx_base::Json {
            return utilxx_base::Json::parse(text);
        },
        [](std::string errmsg) -> utilxx_base::Json {
            XX_LOGW("功能点: 参数不是合法 JSON, 按空值处理: {}", errmsg);
            return utilxx_base::Json{};
        }
    );
}

} // namespace

ImplAnswer parseImplAnswer(std::string_view text) {
    ImplAnswer answer;
    if (text.empty()) {
        return answer; // 没意见
    }
    auto parsed = utilxx_base::catchError<std::optional<utilxx_base::Json>>(
        [&]() -> std::optional<utilxx_base::Json> {
            return utilxx_base::Json::parse(text);
        },
        [](std::string errmsg) -> std::optional<utilxx_base::Json> {
            // 非法 JSON 等价于"没意见": 调用方记一条警告并继续问下一个实现
            XX_LOGW("功能点: 实现返回的回答不是合法 JSON, 按没意见继续: {}", errmsg);
            return std::nullopt;
        }
    );
    if (!parsed.has_value() || !parsed->is_object()) {
        return answer;
    }
    const auto& json = *parsed;
    if (json.contains("verdict")) {
        answer.kind    = ImplAnswer::Kind::Verdict;
        answer.verdict = json.value<std::string>("verdict", "");
        answer.reason  = json.value<std::string>("reason", "");
        return answer;
    }
    if (json.value<bool>("disable", false)) {
        answer.kind   = ImplAnswer::Kind::Disable;
        answer.reason = json.value<std::string>("reason", "");
        return answer;
    }
    if (json.contains("value")) {
        answer.kind      = ImplAnswer::Kind::Value;
        answer.valueJson = json["value"].dump();
        return answer;
    }
    return answer; // 空对象 / 不认识的形状 = 没意见
}

// ==================== PointBase ====================

PointBase::PointBase(std::string id, PointType type, PointOptions opts) :
    id_(std::move(id)),
    type_(type),
    opts_(std::move(opts)) {
    if (opts_.title.empty()) {
        opts_.title = id_;
    }
    cache_.configure(opts_.cache, opts_.maxItems, opts_.maxBytes);
}

PointBase::~PointBase() = default;

const std::string& PointBase::id() const noexcept {
    return id_;
}

PointType PointBase::type() const noexcept {
    return type_;
}

const PointOptions& PointBase::options() const noexcept {
    return opts_;
}

const std::string& PointBase::origin() const noexcept {
    return origin_;
}

void PointBase::setOrigin(std::string origin) {
    origin_ = std::move(origin);
}

bool PointBase::callable() const noexcept {
    return opts_.callable;
}

int32_t PointBase::implTimeoutMs() const noexcept {
    return opts_.implTimeoutMs;
}

size_t PointBase::implCount() const noexcept {
    return impls_.size();
}

const std::string& PointBase::disabledBy() const noexcept {
    return disabledBy_;
}

void PointBase::clearDisabled() {
    disabledBy_.clear();
}

void PointBase::resetState() {
    clearDisabled();
    cache_.clear();
    activeCallers_.clear();
    inFlight_.clear();
}

size_t PointBase::cacheSize() const noexcept {
    return cache_.size();
}

size_t PointBase::cacheBytes() const noexcept {
    return cache_.bytes();
}

uint64_t PointBase::cacheHits() const noexcept {
    return cache_.hits();
}

size_t PointBase::invalidateCacheOf(std::string_view owner) {
    return cache_.invalidateBy(owner);
}

int32_t PointBase::addImpl(ImplSpec spec) {
    if (id_.empty() || !spec.fn) {
        XX_LOGW("功能点 `{}`: 实现登记被拒绝 (调用函数为空)", id_);
        return -1;
    }
    if (spec.owner.empty()) {
        spec.owner = spec.layer == ImplLayer::Core ? std::string{"core:unknown"}
                                                   : std::string{"plugin:unknown"};
    }
    // 优先级越界: 裁剪到上下限并记一条警告 (不拒绝登记)
    int32_t priority = spec.priority;
    if (priority < kPriorityMin || priority > kPriorityMax) {
        XX_LOGW(
            "功能点 `{}`: 实现 `{}` 的 priority {} 越界, 已裁剪到 [{}, {}]",
            id_,
            spec.owner,
            priority,
            kPriorityMin,
            kPriorityMax
        );
        priority = std::clamp(priority, kPriorityMin, kPriorityMax);
    }

    for (auto& item : impls_) {
        if (item.owner == spec.owner) {
            // 同一 (点, owner) 重复登记 = 覆盖 (记日志便于排查两个插件抢同一个点)
            XX_LOGI("功能点 `{}`: 实现 `{}` 重复登记, 已覆盖", id_, spec.owner);
            item = ImplEntry{
                spec.layer,
                std::move(spec.owner),
                priority,
                spec.defaultTimeoutMs,
                item.seq,
                std::move(spec.load),
                std::move(spec.note),
                std::move(spec.fn),
                std::move(spec.cancel),
            };
            return 0;
        }
    }
    ImplEntry entry{
        spec.layer,
        std::move(spec.owner),
        priority,
        spec.defaultTimeoutMs,
        ++seqCounter_,
        std::move(spec.load),
        std::move(spec.note),
        std::move(spec.fn),
        std::move(spec.cancel),
    };
    impls_.push_back(std::move(entry));
    return 0;
}

size_t PointBase::removeImpls(std::string_view owner) {
    const size_t before = impls_.size();
    std::erase_if(impls_, [owner](const ImplEntry& item) {
        return item.owner == owner;
    });
    const size_t removed = before - impls_.size();
    if (removed == 0) {
        return 0;
    }
    // 产出方被摘除: 它产出的缓存条目与留下的置空标记一起清掉 (自动还原)
    invalidateCacheOf(owner);
    if (disabledBy_ == owner) {
        clearDisabled();
    }
    return removed;
}

bool PointBase::hasImplOf(std::string_view owner) const {
    return std::any_of(impls_.begin(), impls_.end(), [owner](const ImplEntry& item) {
        return item.owner == owner;
    });
}

void PointBase::clearImpls() {
    impls_.clear();
    resetState();
}

std::string PointBase::effectiveBy() const {
    // 生效者 = 排序后第一个实现 (层 → 优先级 → 登记顺序)
    auto views = implViews();
    const ImplView* best = nullptr;
    for (const auto& item : views) {
        if (best == nullptr || item.layer < best->layer
            || (item.layer == best->layer && item.priority < best->priority)
            || (item.layer == best->layer && item.priority == best->priority
                && item.seq < best->seq)) {
            best = &item;
        }
    }
    return best != nullptr ? best->owner : std::string{};
}

std::vector<const PointBase::ImplEntry*> PointBase::orderedPluginImpls() const {
    std::vector<const ImplEntry*> out;
    out.reserve(impls_.size());
    for (const auto& item : impls_) {
        out.push_back(&item);
    }
    std::stable_sort(out.begin(), out.end(), [](const ImplEntry* a, const ImplEntry* b) {
        if (a->layer != b->layer) {
            return a->layer < b->layer;
        }
        if (a->priority != b->priority) {
            return a->priority < b->priority;
        }
        return a->seq < b->seq;
    });
    return out;
}

// ==================== 统计 (只在开发者模式下累加) ====================

void PointBase::noteAsk() noexcept {
    if (agentxx::agent::AgentConfigStatic::devMode) {
        ++stats_.asks;
    }
}

void PointBase::noteCall() noexcept {
    if (agentxx::agent::AgentConfigStatic::devMode) {
        ++stats_.calls;
    }
}

void PointBase::noteOutcome(
    bool             ok,
    std::string_view by,
    std::string_view caller,
    int64_t          ms
) noexcept {
    if (!agentxx::agent::AgentConfigStatic::devMode) {
        return;
    }
    if (!ok) {
        ++stats_.errors;
    }
    stats_.lastBy     = std::string{by};
    stats_.lastCaller = std::string{caller};
    stats_.lastMs     = ms;
}

// ==================== 实现链 ====================

std::string PointBase::buildCallJson(const ImplEntry& entry, const ChainEnv& env) const {
    utilxx_base::Json call = utilxx_base::Json::object();
    call["point"]     = id_;
    call["args"]      = parseOrNull(env.argsJson);
    call["request"]   = parseOrNull(env.requestJson);
    call["caller"]    = env.caller;
    call["viaCall"]   = env.viaCall;
    call["identity"]  = env.identity;
    call["sessionId"] = env.sessionId;
    if (agentxx::agent::AgentConfigStatic::devMode) {
        // 开发者模式才带实现归属 (便于实现侧日志自认; 正常模式下载荷保持最小)
        call["owner"] = entry.owner;
    }
    return call.dump();
}

asio::awaitable<std::optional<std::string>>
    PointBase::invokeImpl(const ImplEntry& entry, std::string callJson) {
    // 实现抛异常等价于"没意见": 记一条警告后继续问下一个实现
    // (实现既可能在调用时同步抛, 也可能在挂起点之后抛, 因此统一经 catchErrorAsync)
    const std::string owner = entry.owner;
    co_return co_await agentxx::util::catchErrorAsync<std::optional<std::string>>(
        [&entry, callJson = std::move(callJson)]() mutable
            -> asio::awaitable<std::optional<std::string>> {
            co_return co_await entry.fn(std::move(callJson));
        },
        [this, owner](std::string errmsg) -> asio::awaitable<std::optional<std::string>> {
            XX_LOGW("功能点 `{}`: 实现 `{}` 抛异常, 按没意见继续: {}", id_, owner, errmsg);
            co_return std::nullopt;
        }
    );
}

asio::awaitable<std::optional<ChainOutcome>>
    PointBase::runOneImpl(const ImplEntry& entry, const ChainEnv& env) {
    // 生效超时 = 点的 implTimeoutMs 与该实现自报值的非 0 较小者 (都为 0 = 不限)
    int32_t effectiveMs = 0;
    for (const int32_t candidate : {opts_.implTimeoutMs, entry.defaultTimeoutMs}) {
        if (candidate > 0 && (effectiveMs == 0 || candidate < effectiveMs)) {
            effectiveMs = candidate;
        }
    }
    const auto timeout = std::chrono::milliseconds{effectiveMs};
    auto       callJson = buildCallJson(entry, env);

    std::optional<std::string> answer;
    if (timeout.count() > 0) {
        // 到点取消当次实现, 并按"没意见"继续问链上的下一个实现
        const std::string pointId = id_;
        const std::string owner   = entry.owner;
        const ImplEntry&  entryRef = entry;
        answer                    = co_await utilxx::asyncWithTimeout<std::optional<std::string>>(
            [this, &entryRef, callJson]() mutable -> asio::awaitable<std::optional<std::string>> {
                co_return co_await invokeImpl(entryRef, std::move(callJson));
            },
            timeout,
            [&pointId, &owner, effectiveMs]() -> std::optional<std::string> {
                XX_LOGW(
                    "功能点 `{}`: 实现 `{}` 超过 {} ms 未完成, 已取消并按没意见继续",
                    pointId,
                    owner,
                    effectiveMs
                );
                return std::nullopt;
            }
        );
    } else {
        answer = co_await invokeImpl(entry, std::move(callJson));
    }
    if (!answer.has_value()) {
        co_return std::nullopt; // 没意见
    }

    auto parsed = parseImplAnswer(*answer);
    if (parsed.kind == ImplAnswer::Kind::NoOpinion) {
        co_return std::nullopt;
    }
    ChainOutcome out;
    out.by = entry.owner;
    if (parsed.kind == ImplAnswer::Kind::Disable) {
        out.disabled = true;
        out.error    = CallError::Disabled;
        out.message  = parsed.reason.empty()
                         ? fmt::format("功能点 `{}` 被 `{}` 置空", id_, entry.owner)
                         : parsed.reason;
        co_return out;
    }
    if (parsed.kind == ImplAnswer::Kind::Verdict) {
        // 本轮只有核心点使用裁决型; 插件层给出裁决回答按"没意见"处理并记日志
        XX_LOGW(
            "功能点 `{}`: 实现 `{}` 返回了裁决型回答 `{}`, 本点按没意见处理",
            id_,
            entry.owner,
            parsed.verdict
        );
        co_return std::nullopt;
    }
    out.hasValue  = true;
    out.valueJson = std::move(parsed.valueJson);
    out.error     = CallError::None;
    co_return out;
}

asio::awaitable<ChainOutcome> PointBase::runPluginLayer(const ChainEnv& env) {
    ChainOutcome out;
    for (const auto* entry : orderedPluginImpls()) {
        if (entry->layer != ImplLayer::Plugin || !entry->fn) {
            continue;
        }
        // 保护 ②: 调用方自己的实现不再问回自己 (无用往返; 重活尤其值钱)
        if (env.viaCall && !env.caller.empty() && entry->owner == env.caller) {
            continue;
        }
        auto result = co_await runOneImpl(*entry, env);
        if (!result.has_value()) {
            continue; // 没意见 (含异常、超时、无法解析的回答)
        }
        if (result->disabled || result->hasValue) {
            co_return *result;
        }
    }
    out.error   = CallError::NoImpl;
    out.message = fmt::format("功能点 `{}` 现在没有实现给出值", id_);
    co_return out;
}

// ==================== 取值 / 调用主流程 ====================

ResolvedValue PointBase::finalize(
    const ChainOutcome&   outcome,
    const ResolveRequest& req,
    int64_t               ms
) {
    ResolvedValue out;
    out.identity = req.identity;
    out.ms       = ms;

    if (outcome.hasValue) {
        out.error     = CallError::None;
        out.valueJson = outcome.valueJson;
        out.by        = outcome.by;
        // 写不写值缓存由"发起这次计算的一方"决定 (`ask` 写、`call` 不写)
        if (req.persist && cache_.mode() != CacheMode::None
            && (cache_.mode() == CacheMode::Latest || !req.identity.empty())) {
            cache_.store(req.identity, outcome.valueJson, outcome.by);
        }
        if (agentxx::agent::AgentConfigStatic::devMode) {
            XX_LOGI(
                "功能点调用: {} ← {}: {} · {} ms ok{}",
                id_,
                req.caller.empty() ? "app" : req.caller,
                out.by,
                ms,
                req.viaCall ? " (call)" : ""
            );
        }
        return out;
    }

    if (outcome.disabled) {
        out.error = CallError::Disabled;
        out.by    = outcome.by;
        out.message
            = outcome.message.empty() ? fmt::format("功能点 `{}` 被 `{}` 置空", id_, outcome.by)
                                      : outcome.message;
        // 置空标记只由 `ask` 记录 (`call` 看到置空只影响本次)
        if (req.rememberDisable && !outcome.by.empty()) {
            disabledBy_ = outcome.by;
        }
        if (!req.viaCall) {
            XX_LOGW("功能点 {}: {}", id_, out.message);
        }
        return out;
    }

    out.error = (outcome.error == CallError::None) ? CallError::NoImpl : outcome.error;
    out.message
        = outcome.message.empty() ? fmt::format("功能点 `{}` 现在没有实现给出值", id_)
                                  : outcome.message;
    return out;
}

void PointBase::publishInFlight(
    const std::string&               identity,
    const std::shared_ptr<InFlight>& slot,
    ChainOutcome                     outcome
) {
    if (!slot) {
        return;
    }
    if (!slot->done) {
        slot->done    = true;
        slot->outcome = std::move(outcome);
    }
    auto waiters = std::move(slot->waiters);
    for (auto& timer : waiters) {
        if (timer) {
            boost::system::error_code ec;
            timer->cancel();
        }
    }
    if (!identity.empty()) {
        inFlight_.erase(identity);
    }
}

asio::awaitable<ResolvedValue> PointBase::runResolve(ResolveRequest req) {
    const auto begin = std::chrono::steady_clock::now();
    const auto elapsedMs = [&begin]() -> int64_t {
        return static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin
            )
                .count()
        );
    };

    // 0) 负载上限: 跨边界只经一次拷贝, 超过上限直接拒绝并说明
    if (req.requestJson.size() > kFeatureMaxPayloadBytes) {
        ResolvedValue out;
        out.identity = req.identity;
        out.error    = CallError::Failed;
        out.message  = fmt::format(
            "功能点 `{}` 的请求过大 ({} 字节, 上限 {} 字节)",
            id_,
            req.requestJson.size(),
            kFeatureMaxPayloadBytes
        );
        XX_LOGW("{}", out.message);
        noteOutcome(false, "", req.caller, elapsedMs());
        co_return out;
    }

    // 1) 声明校验: 没有对外开放调用时直接返回 (本轮只有应用点会声明不可调)
    if (req.viaCall && !opts_.callable) {
        ResolvedValue out;
        out.identity = req.identity;
        out.error    = CallError::NotCallable;
        out.message  = opts_.callDoc.empty()
                         ? fmt::format("功能点 `{}` 没有对外开放调用", id_)
                         : fmt::format("功能点 `{}` 没有对外开放调用: {}", id_, opts_.callDoc);
        noteOutcome(false, "", req.caller, elapsedMs());
        co_return out;
    }

    // 2) 重入保护: 同一 (点, 调用方) 上一次没结束又来一次 -> busy (不排队)
    //    它把任何环路 (A→P→A→P…) 变成一次有边界的失败, 而不是无限递归或死等
    const bool guardBusy = req.viaCall;
    if (guardBusy && !activeCallers_.insert(req.caller).second) {
        ResolvedValue out;
        out.identity = req.identity;
        out.error    = CallError::Busy;
        out.message  = fmt::format(
            "功能点 `{}` 上一次调用 (调用方 `{}`) 还没结束",
            id_,
            req.caller.empty() ? "app" : req.caller
        );
        XX_LOGW("{}", out.message);
        noteOutcome(false, "", req.caller, elapsedMs());
        co_return out;
    }
    /// 作用域退出 (含协程被取消销毁) 时移除重入标记
    struct BusyGuard {
        std::set<std::string, std::less<>>* set = nullptr;
        std::string                         key;
        ~BusyGuard() {
            if (set != nullptr) {
                set->erase(key);
            }
        }
    } busyGuard{guardBusy ? &activeCallers_ : nullptr, req.caller};

    // 3) 值缓存 (refresh = true 时跳过; `call` 只读不写)
    if (!req.refresh) {
        if (const auto* hit = cache_.find(req.identity)) {
            ResolvedValue out;
            out.error     = CallError::None;
            out.valueJson = hit->valueJson;
            out.by        = hit->by;
            out.identity  = req.identity;
            out.fromCache = true;
            out.ms        = elapsedMs();
            noteOutcome(true, out.by, req.caller, out.ms);
            co_return out;
        }
    }

    // 4) 同一身份并发只跑一次实现 (in-flight 去重)
    std::shared_ptr<InFlight> slot;
    bool                      initiator = true;
    if (!req.identity.empty()) {
        auto it = inFlight_.find(req.identity);
        if (it == inFlight_.end()) {
            slot = std::make_shared<InFlight>();
            inFlight_.emplace(req.identity, slot);
        } else {
            slot      = it->second;
            initiator = false;
        }
    }

    if (!initiator) {
        // 搭顺风车: 等发起方完成 (调用方给了 timeoutMs 时到点返回 failed)
        auto waitDone = [&slot]() -> asio::awaitable<bool> {
            auto ex    = co_await asio::this_coro::executor;
            auto timer = std::make_shared<asio::steady_timer>(ex);
            slot->waiters.push_back(timer);
            timer->expires_at((std::chrono::steady_clock::time_point::max)());
            boost::system::error_code ec;
            co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
            co_return slot->done;
        };
        bool finished = false;
        if (req.timeout.count() > 0) {
            const auto tmo = req.timeout;
            finished       = co_await utilxx::asyncWithTimeout<bool>(
                waitDone,
                tmo,
                []() {
                    return false;
                }
            );
        } else {
            finished = co_await waitDone();
        }
        if (!finished) {
            ResolvedValue out;
            out.identity = req.identity;
            out.error    = CallError::Failed;
            out.ms       = elapsedMs();
            out.message  = fmt::format("timeout after {}ms", req.timeout.count());
            XX_LOGW("功能点 `{}`: {}", id_, out.message);
            noteOutcome(false, "", req.caller, out.ms);
            co_return out;
        }
        auto out = finalize(slot->outcome, req, elapsedMs());
        noteOutcome(out.ok(), out.by, req.caller, out.ms);
        co_return out;
    }

    // 5) 发起方: 跑实现链; 无论正常结束还是被取消, 都要发布结果唤醒等待者
    /// 作用域退出时若还没发布, 用"失败"兜底发布 (避免等待者永远挂着)
    struct PublishGuard {
        PointBase*                self = nullptr;
        std::string               identity;
        std::shared_ptr<InFlight> slot;
        ~PublishGuard() {
            if (slot == nullptr || slot->done) {
                return;
            }
            ChainOutcome fallback;
            fallback.error   = CallError::Failed;
            fallback.message = "发起方提前结束 (取消或异常)";
            self->publishInFlight(identity, slot, std::move(fallback));
        }
    } publishGuard{this, req.identity, slot};

    ChainEnv env;
    env.typedRequest = req.typedRequest;
    env.requestJson  = req.requestJson;
    env.argsJson     = req.argsJson;
    env.identity    = req.identity;
    env.caller      = req.caller;
    env.sessionId   = req.sessionId;
    env.viaCall     = req.viaCall;

    ChainOutcome outcome;
    if (req.timeout.count() > 0) {
        // 调用方超时: 到点取消当次实现并返回 failed (message 写清限时值)
        const auto tmo    = req.timeout;
        const std::string pointId = id_;
        outcome           = co_await utilxx::asyncWithTimeout<ChainOutcome>(
            [this, &env]() -> asio::awaitable<ChainOutcome> {
                co_return co_await runChain(env);
            },
            tmo,
            [&pointId, tmo]() -> ChainOutcome {
                ChainOutcome o;
                o.error   = CallError::Failed;
                o.message = fmt::format("timeout after {}ms", tmo.count());
                XX_LOGW("功能点 `{}`: 调用超时 ({} ms)", pointId, tmo.count());
                return o;
            }
        );
    } else {
        outcome = co_await runChain(env);
    }

    publishInFlight(req.identity, slot, outcome);
    auto out = finalize(outcome, req, elapsedMs());
    noteOutcome(out.ok(), out.by, req.caller, out.ms);
    co_return out;
}

// ==================== 清单 ====================

std::vector<PointBase::ImplView> PointBase::implViews() const {
    std::vector<ImplView> views;
    views.reserve(impls_.size());
    for (const auto& item : impls_) {
        views.push_back(ImplView{
            item.layer,
            item.owner,
            item.priority,
            item.defaultTimeoutMs,
            item.seq,
            item.load,
            item.note,
        });
    }
    return views;
}

utilxx_base::Json PointBase::listJson() const {
    utilxx_base::Json json = utilxx_base::Json::object();
    json["id"]        = id_;
    json["type"]      = pointTypeKey(type_);
    json["title"]     = opts_.title;
    json["depict"]    = opts_.depict;
    json["origin"]    = origin_;
    json["callable"]  = opts_.callable;
    json["callDoc"]   = opts_.callDoc;

    utilxx_base::Json cache = utilxx_base::Json::object();
    cache["mode"]     = cacheModeKey(opts_.cache);
    cache["maxItems"] = cache_.maxItems();
    cache["maxBytes"] = cache_.maxBytes();
    if (opts_.cache != CacheMode::None) {
        cache["items"]      = cache_.size();
        cache["bytes"]      = cache_.bytes();
        cache["hits"]       = cache_.hits();
        cache["misses"]     = cache_.misses();
    }
    json["cache"] = std::move(cache);

    json["effectiveBy"] = effectiveBy();
    json["disabledBy"]  = disabledBy_;

    utilxx_base::Json impls = utilxx_base::Json::array();
    {
        // 插件层与核心层一起列出, 按 (层, 优先级, 登记顺序) 统一排序 —— 清单里看到的
        // 顺序就是实际的询问顺序
        auto views = implViews();
        std::stable_sort(views.begin(), views.end(), [](const ImplView& a, const ImplView& b) {
            if (a.layer != b.layer) {
                return a.layer < b.layer;
            }
            if (a.priority != b.priority) {
                return a.priority < b.priority;
            }
            return a.seq < b.seq;
        });
        for (const auto& entry : views) {
            utilxx_base::Json item = utilxx_base::Json::object();
            item["layer"]     = implLayerKey(entry.layer);
            item["owner"]     = entry.owner;
            item["priority"]  = entry.priority;
            item["load"]      = entry.load;
            item["note"]      = entry.note;
            item["seq"]       = static_cast<int64_t>(entry.seq);
            item["timeoutMs"] = entry.defaultTimeoutMs;
            impls.push_back(std::move(item));
        }
    }
    json["impls"] = std::move(impls);

    // 统计只在开发者模式下收集; 关闭时清单只给"当前状态"
    if (agentxx::agent::AgentConfigStatic::devMode) {
        utilxx_base::Json stat = utilxx_base::Json::object();
        stat["asks"]       = static_cast<int64_t>(stats_.asks);
        stat["calls"]      = static_cast<int64_t>(stats_.calls);
        stat["errors"]     = static_cast<int64_t>(stats_.errors);
        stat["lastBy"]     = stats_.lastBy;
        stat["lastCaller"] = stats_.lastCaller;
        stat["lastMs"]     = stats_.lastMs;
        json["stat"]       = std::move(stat);
    }
    return json;
}

// ==================== JsonProvidePoint ====================

JsonProvidePoint::JsonProvidePoint(std::string id, PointOptions opts, std::string origin) :
    PointBase(std::move(id), PointType::Provide, std::move(opts)) {
    setOrigin(std::move(origin));
}

asio::awaitable<ChainOutcome> JsonProvidePoint::runChain(const ChainEnv& env) {
    // 插件点没有核心实现: 只问插件层, 没人给值就是 no_impl
    co_return co_await runPluginLayer(env);
}

namespace {

/// 把 JSON 文本解成算法结果 (失败返回 error 说明)
JsonResult makeJsonResult(const ResolvedValue& rv) {
    JsonResult out;
    out.error     = rv.error;
    out.by        = rv.by;
    out.identity  = rv.identity;
    out.fromCache = rv.fromCache;
    out.ms        = rv.ms;
    out.message   = rv.message;
    if (!rv.ok()) {
        return out;
    }
    auto value = utilxx_base::catchError<std::optional<utilxx_base::Json>>(
        [&]() -> std::optional<utilxx_base::Json> {
            return utilxx_base::Json::parse(rv.valueJson);
        },
        [](std::string errmsg) -> std::optional<utilxx_base::Json> {
            XX_LOGW("功能点: 实现返回的值不是合法 JSON: {}", errmsg);
            return std::nullopt;
        }
    );
    if (!value.has_value()) {
        out.error   = CallError::Failed;
        out.message = "实现返回了无法解析的值";
        return out;
    }
    out.value = std::move(*value);
    return out;
}

} // namespace

asio::awaitable<JsonResult> JsonProvidePoint::ask(std::string argsJson, AskOptions opts) {
    noteAsk();
    ResolveRequest rr;
    rr.requestJson     = "{}";
    rr.argsJson        = std::move(argsJson);
    rr.identity        = opts.identity;
    rr.sessionId       = opts.sessionId;
    rr.persist         = true;
    rr.rememberDisable = true;
    rr.refresh         = opts.refresh;
    auto rv            = co_await runResolve(std::move(rr));
    co_return makeJsonResult(rv);
}

asio::awaitable<JsonResult> JsonProvidePoint::call(std::string argsJson, CallOptions opts) {
    noteCall();
    ResolveRequest rr;
    rr.requestJson     = "{}";
    rr.argsJson        = std::move(argsJson);
    rr.identity        = opts.identity;
    rr.caller          = opts.caller;
    rr.sessionId       = opts.sessionId;
    rr.viaCall         = true;
    rr.persist         = false;
    rr.rememberDisable = false;
    rr.refresh         = opts.refresh;
    rr.timeout         = opts.timeout;
    auto rv            = co_await runResolve(std::move(rr));
    co_return makeJsonResult(rv);
}

} // namespace feature
} // namespace agentxx
