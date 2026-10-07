#pragma once

#include "utilxx_base/json.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace util {

/// 关键指标 (计划 OBS-3)
///
/// 背景: 会话质量与成本问题 (首 token 变慢、工具大量失败、压缩频繁、上下文逼近上限)
/// 在运行期没有任何可读的汇总, 只能翻日志。这里把少量**低成本**计数与计时集中在
/// 一处: 每次事件只做几个原子操作, 汇总在需要时 (诊断包/装配快照/测试) 才计算。
///
/// - 只统计"发生了什么", 不记录内容 (无提示词/参数/结果正文), 因此可以安全导出;
/// - 时间量为累计值与计数, 平均值在导出时计算;
/// - 线程安全 (原子量), 因为一个 agent 的事件可能来自 IO 线程与工作线程。
class KeyMetrics {
public:

    /// 工具调用终态 (与工具结算规则一致)
    enum class ToolOutcome {
        Ok = 0,      ///< 执行成功 (含工具自身返回的错误文本)
        Failed,      ///<< 抛异常被格式化为错误结果
        Cancelled,   ///< 用户取消占位
        Interrupted, ///< 中断占位 (等待用户输入)
    };

    /// 轮次终态
    enum class TurnOutcome {
        Completed = 0, ///< 正常结束
        Failed,        ///< 出错结束
        Cancelled,     ///< 取消结束
        Interrupted,   ///< 中断 (等待用户输入后结束本轮)
    };

    // ---- 轮次与首 token 延迟 ----

    void noteTurnStart();
    /// 本轮首个 token (正文或思考) 到达; 同一轮重复调用只记第一次
    void noteFirstToken();
    void noteTurnEnd(TurnOutcome outcome, int64_t durationMs);

    // ---- 模型调用 ----

    /// 一次模型调用结算 (成功路径); usage 可为 0 (provider 未上报)
    /// - cacheWriteTokens: 写入 prompt 缓存的 token 数 (仅 Anthropic 回报, 计划 LLM-8)
    void noteModelCall(
        int64_t promptTokens,
        int64_t completionTokens,
        int64_t cachedTokens,
        int64_t cacheWriteTokens = 0
    );
    /// 一次模型调用失败 (kind = 错误分类文本, 见 nodes/llm_error.h)
    void noteModelError(std::string_view kind);

    // ---- 工具 ----

    void noteToolCall(ToolOutcome outcome);

    // ---- 压缩 ----

    /// 一次上下文压缩完成 (自动或手动)
    void noteCompaction(int64_t tokensBefore, int64_t tokensAfter);

    // ---- 导出 ----

    /// 指标 JSON (字段稳定; 时间量为毫秒累计 + 计数, 便于外部求平均)
    utilxx_base::Json toJson() const;

    /// 人类可读单行摘要 (日志/诊断包用)
    std::string summary() const;

    /// 清空全部计数 (测试/基准分段)
    void reset();

    // 读取 (供测试与诊断)
    uint64_t turns() const noexcept {
        return turns_.load(std::memory_order_relaxed);
    }
    uint64_t turnsCompleted() const noexcept {
        return turnsCompleted_.load(std::memory_order_relaxed);
    }
    uint64_t turnsFailed() const noexcept {
        return turnsFailed_.load(std::memory_order_relaxed);
    }
    uint64_t turnsCancelled() const noexcept {
        return turnsCancelled_.load(std::memory_order_relaxed);
    }
    uint64_t turnsInterrupted() const noexcept {
        return turnsInterrupted_.load(std::memory_order_relaxed);
    }
    uint64_t firstTokenSamples() const noexcept {
        return firstTokenSamples_.load(std::memory_order_relaxed);
    }
    int64_t firstTokenTotalMs() const noexcept {
        return firstTokenTotalMs_.load(std::memory_order_relaxed);
    }
    int64_t firstTokenMaxMs() const noexcept {
        return firstTokenMaxMs_.load(std::memory_order_relaxed);
    }
    int64_t turnTotalMs() const noexcept {
        return turnTotalMs_.load(std::memory_order_relaxed);
    }
    int64_t turnMaxMs() const noexcept {
        return turnMaxMs_.load(std::memory_order_relaxed);
    }
    uint64_t modelCalls() const noexcept {
        return modelCalls_.load(std::memory_order_relaxed);
    }
    uint64_t modelErrors() const noexcept {
        return modelErrors_.load(std::memory_order_relaxed);
    }
    int64_t promptTokens() const noexcept {
        return promptTokens_.load(std::memory_order_relaxed);
    }
    int64_t completionTokens() const noexcept {
        return completionTokens_.load(std::memory_order_relaxed);
    }
    int64_t cachedTokens() const noexcept {
        return cachedTokens_.load(std::memory_order_relaxed);
    }
    /// 写入 prompt 缓存的 token 累计 (仅 Anthropic 回报; 计划 LLM-8)
    int64_t cacheWriteTokens() const noexcept {
        return cacheWriteTokens_.load(std::memory_order_relaxed);
    }
    uint64_t toolOk() const noexcept {
        return toolOk_.load(std::memory_order_relaxed);
    }
    uint64_t toolFailed() const noexcept {
        return toolFailed_.load(std::memory_order_relaxed);
    }
    uint64_t toolCancelled() const noexcept {
        return toolCancelled_.load(std::memory_order_relaxed);
    }
    uint64_t toolInterrupted() const noexcept {
        return toolInterrupted_.load(std::memory_order_relaxed);
    }
    uint64_t compactions() const noexcept {
        return compactions_.load(std::memory_order_relaxed);
    }
    int64_t compactionTokensBefore() const noexcept {
        return compactionTokensBefore_.load(std::memory_order_relaxed);
    }
    int64_t compactionTokensAfter() const noexcept {
        return compactionTokensAfter_.load(std::memory_order_relaxed);
    }
    /// 最近一次模型错误分类 (空 = 无错误)
    std::string lastModelErrorKind() const;

private:

    std::atomic<uint64_t> turns_{0};
    std::atomic<uint64_t> turnsCompleted_{0};
    std::atomic<uint64_t> turnsFailed_{0};
    std::atomic<uint64_t> turnsCancelled_{0};
    std::atomic<uint64_t> turnsInterrupted_{0};

    std::atomic<uint64_t> firstTokenSamples_{0};
    std::atomic<int64_t>  firstTokenTotalMs_{0};
    std::atomic<int64_t>  firstTokenMaxMs_{0};
    std::atomic<int64_t>  turnTotalMs_{0};
    std::atomic<int64_t>  turnMaxMs_{0};

    std::atomic<uint64_t> modelCalls_{0};
    std::atomic<uint64_t> modelErrors_{0};
    std::atomic<int64_t>  promptTokens_{0};
    std::atomic<int64_t>  completionTokens_{0};
    std::atomic<int64_t>  cachedTokens_{0};
    std::atomic<int64_t>  cacheWriteTokens_{0};

    std::atomic<uint64_t> toolOk_{0};
    std::atomic<uint64_t> toolFailed_{0};
    std::atomic<uint64_t> toolCancelled_{0};
    std::atomic<uint64_t> toolInterrupted_{0};

    std::atomic<uint64_t> compactions_{0};
    std::atomic<int64_t>  compactionTokensBefore_{0};
    std::atomic<int64_t>  compactionTokensAfter_{0};

    /// 本轮的单调起点 (noteTurnStart 记录, noteFirstToken/noteTurnEnd 取值)
    std::atomic<int64_t> turnStartSteadyMs_{0};
    std::atomic<bool>    firstTokenSeen_{false};

    /// 最近一次模型错误分类 (只在 IO 线程写, 读时加锁拷贝)
    mutable std::mutex  errorMutex_;
    std::string         lastModelErrorKind_;
};

} // namespace util
} // namespace agentxx
