#include "agentxx/util/observability.h"

#include "fmt/format.h"
#include <chrono>
#include <mutex>

namespace agentxx {
namespace util {

namespace {

/// 单调毫秒时钟 (计时只用于"耗时", 不受系统时间调整影响)
int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

void noteMax(std::atomic<int64_t>& target, const int64_t value) {
    int64_t prev = target.load(std::memory_order_relaxed);
    while (value > prev && !target.compare_exchange_weak(prev, value)) {
    }
}

/// 平均值 (样本数为 0 时返回 0; 保留一位小数便于阅读)
double avgOf(const int64_t total, const uint64_t samples) {
    if (samples == 0) {
        return 0.0;
    }
    return static_cast<double>(total) / static_cast<double>(samples);
}

} // namespace

void KeyMetrics::noteTurnStart() {
    turns_.fetch_add(1, std::memory_order_relaxed);
    turnStartSteadyMs_.store(steadyNowMs(), std::memory_order_relaxed);
    firstTokenSeen_.store(false, std::memory_order_relaxed);
}

void KeyMetrics::noteFirstToken() {
    // 同一轮只记第一次 (流式 token 很密集, 这里必须是"一次判定 + 一次写入")
    if (firstTokenSeen_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const int64_t begin = turnStartSteadyMs_.load(std::memory_order_relaxed);
    if (begin <= 0) {
        return;
    }
    const int64_t elapsed = steadyNowMs() - begin;
    if (elapsed < 0) {
        return;
    }
    firstTokenSamples_.fetch_add(1, std::memory_order_relaxed);
    firstTokenTotalMs_.fetch_add(elapsed, std::memory_order_relaxed);
    noteMax(firstTokenMaxMs_, elapsed);
}

void KeyMetrics::noteTurnEnd(const TurnOutcome outcome, const int64_t durationMs) {
    switch (outcome) {
        case TurnOutcome::Completed:
            turnsCompleted_.fetch_add(1, std::memory_order_relaxed);
            break;
        case TurnOutcome::Failed:
            turnsFailed_.fetch_add(1, std::memory_order_relaxed);
            break;
        case TurnOutcome::Cancelled:
            turnsCancelled_.fetch_add(1, std::memory_order_relaxed);
            break;
        case TurnOutcome::Interrupted:
            turnsInterrupted_.fetch_add(1, std::memory_order_relaxed);
            break;
    }
    if (durationMs > 0) {
        turnTotalMs_.fetch_add(durationMs, std::memory_order_relaxed);
        noteMax(turnMaxMs_, durationMs);
    }
    turnStartSteadyMs_.store(0, std::memory_order_relaxed);
}

void KeyMetrics::noteModelCall(
    const int64_t promptTokens,
    const int64_t completionTokens,
    const int64_t cachedTokens,
    const int64_t cacheWriteTokens
) {
    modelCalls_.fetch_add(1, std::memory_order_relaxed);
    if (promptTokens > 0) {
        promptTokens_.fetch_add(promptTokens, std::memory_order_relaxed);
    }
    if (completionTokens > 0) {
        completionTokens_.fetch_add(completionTokens, std::memory_order_relaxed);
    }
    if (cachedTokens > 0) {
        cachedTokens_.fetch_add(cachedTokens, std::memory_order_relaxed);
    }
    if (cacheWriteTokens > 0) {
        cacheWriteTokens_.fetch_add(cacheWriteTokens, std::memory_order_relaxed);
    }
}

void KeyMetrics::noteModelError(const std::string_view kind) {
    modelErrors_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(errorMutex_);
    lastModelErrorKind_.assign(kind);
}

void KeyMetrics::noteToolCall(const ToolOutcome outcome) {
    switch (outcome) {
        case ToolOutcome::Ok:
            toolOk_.fetch_add(1, std::memory_order_relaxed);
            break;
        case ToolOutcome::Failed:
            toolFailed_.fetch_add(1, std::memory_order_relaxed);
            break;
        case ToolOutcome::Cancelled:
            toolCancelled_.fetch_add(1, std::memory_order_relaxed);
            break;
        case ToolOutcome::Interrupted:
            toolInterrupted_.fetch_add(1, std::memory_order_relaxed);
            break;
    }
}

void KeyMetrics::noteCompaction(const int64_t tokensBefore, const int64_t tokensAfter) {
    compactions_.fetch_add(1, std::memory_order_relaxed);
    if (tokensBefore > 0) {
        compactionTokensBefore_.fetch_add(tokensBefore, std::memory_order_relaxed);
    }
    if (tokensAfter > 0) {
        compactionTokensAfter_.fetch_add(tokensAfter, std::memory_order_relaxed);
    }
}

std::string KeyMetrics::lastModelErrorKind() const {
    std::lock_guard<std::mutex> lock(errorMutex_);
    return lastModelErrorKind_;
}

utilxx_base::Json KeyMetrics::toJson() const {
    utilxx_base::Json j = utilxx_base::Json::object();

    utilxx_base::Json turns = utilxx_base::Json::object();
    turns["total"]       = turns_.load(std::memory_order_relaxed);
    turns["completed"]   = turnsCompleted_.load(std::memory_order_relaxed);
    turns["failed"]      = turnsFailed_.load(std::memory_order_relaxed);
    turns["cancelled"]   = turnsCancelled_.load(std::memory_order_relaxed);
    turns["interrupted"] = turnsInterrupted_.load(std::memory_order_relaxed);
    turns["avg_ms"]      = avgOf(turnTotalMs_.load(std::memory_order_relaxed), turns_.load());
    turns["max_ms"]      = turnMaxMs_.load(std::memory_order_relaxed);
    j["turns"]           = std::move(turns);

    // 首 token 延迟 (TTFT): 用户感知的"开始有反应"的时间
    utilxx_base::Json ttft = utilxx_base::Json::object();
    ttft["samples"] = firstTokenSamples_.load(std::memory_order_relaxed);
    ttft["avg_ms"]  = avgOf(
        firstTokenTotalMs_.load(std::memory_order_relaxed),
        firstTokenSamples_.load(std::memory_order_relaxed)
    );
    ttft["max_ms"] = firstTokenMaxMs_.load(std::memory_order_relaxed);
    j["ttft"]      = std::move(ttft);

    utilxx_base::Json model = utilxx_base::Json::object();
    model["calls"]             = modelCalls_.load(std::memory_order_relaxed);
    model["errors"]            = modelErrors_.load(std::memory_order_relaxed);
    model["last_error_kind"]   = lastModelErrorKind();
    model["prompt_tokens"]     = promptTokens_.load(std::memory_order_relaxed);
    model["completion_tokens"] = completionTokens_.load(std::memory_order_relaxed);
    model["cached_tokens"]     = cachedTokens_.load(std::memory_order_relaxed);
    model["cache_write_tokens"] = cacheWriteTokens_.load(std::memory_order_relaxed);
    j["model"]                 = std::move(model);

    utilxx_base::Json tools = utilxx_base::Json::object();
    tools["ok"]          = toolOk_.load(std::memory_order_relaxed);
    tools["failed"]      = toolFailed_.load(std::memory_order_relaxed);
    tools["cancelled"]   = toolCancelled_.load(std::memory_order_relaxed);
    tools["interrupted"] = toolInterrupted_.load(std::memory_order_relaxed);
    tools["total"]       = toolOk_.load(std::memory_order_relaxed)
                           + toolFailed_.load(std::memory_order_relaxed)
                           + toolCancelled_.load(std::memory_order_relaxed)
                           + toolInterrupted_.load(std::memory_order_relaxed);
    j["tools"] = std::move(tools);

    utilxx_base::Json compaction = utilxx_base::Json::object();
    compaction["count"]         = compactions_.load(std::memory_order_relaxed);
    compaction["tokens_before"] = compactionTokensBefore_.load(std::memory_order_relaxed);
    compaction["tokens_after"]  = compactionTokensAfter_.load(std::memory_order_relaxed);
    j["compaction"]             = std::move(compaction);

    return j;
}

std::string KeyMetrics::summary() const {
    const auto  turns   = turns_.load(std::memory_order_relaxed);
    const auto  samples = firstTokenSamples_.load(std::memory_order_relaxed);
    const auto& toolsTotal
        = toolOk_.load(std::memory_order_relaxed) + toolFailed_.load(std::memory_order_relaxed)
          + toolCancelled_.load(std::memory_order_relaxed)
          + toolInterrupted_.load(std::memory_order_relaxed);
    return fmt::format(
        "turns={} (ok={} failed={} cancelled={} interrupted={}) ttft_avg={:.1f}ms (n={}) "
        "model_calls={} errors={} tools={} (failed={}) compactions={}",
        turns,
        turnsCompleted_.load(std::memory_order_relaxed),
        turnsFailed_.load(std::memory_order_relaxed),
        turnsCancelled_.load(std::memory_order_relaxed),
        turnsInterrupted_.load(std::memory_order_relaxed),
        avgOf(firstTokenTotalMs_.load(std::memory_order_relaxed), samples),
        samples,
        modelCalls_.load(std::memory_order_relaxed),
        modelErrors_.load(std::memory_order_relaxed),
        toolsTotal,
        toolFailed_.load(std::memory_order_relaxed),
        compactions_.load(std::memory_order_relaxed)
    );
}

void KeyMetrics::reset() {
    turns_.store(0);
    turnsCompleted_.store(0);
    turnsFailed_.store(0);
    turnsCancelled_.store(0);
    turnsInterrupted_.store(0);
    firstTokenSamples_.store(0);
    firstTokenTotalMs_.store(0);
    firstTokenMaxMs_.store(0);
    turnTotalMs_.store(0);
    turnMaxMs_.store(0);
    modelCalls_.store(0);
    modelErrors_.store(0);
    promptTokens_.store(0);
    completionTokens_.store(0);
    cachedTokens_.store(0);
    cacheWriteTokens_.store(0);
    toolOk_.store(0);
    toolFailed_.store(0);
    toolCancelled_.store(0);
    toolInterrupted_.store(0);
    compactions_.store(0);
    compactionTokensBefore_.store(0);
    compactionTokensAfter_.store(0);
    turnStartSteadyMs_.store(0);
    firstTokenSeen_.store(false);
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastModelErrorKind_.clear();
    }
}

} // namespace util
} // namespace agentxx
