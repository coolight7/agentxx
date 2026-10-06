#pragma once
/// 假 provider (计划 LLM-5 / TST-1): 不经网络直接产出固定流、错误、延迟与工具调用。
///
/// 用途: 重试、压缩、中断、取消与工具循环用例共用同一注入接缝, 不依赖真实网络、
/// 额度或本地 HTTP 模拟服务器。注入点是 `ModelProviderRegistry::setProvider`
/// (与真实 provider 完全相同的调用路径)。
///
/// 用法:
/// ```cpp
/// auto provider = std::make_shared<agentxx::test::FakeProvider>();
/// provider->pushToolCalls({agentxx::test::FakeProvider::toolCall("c1", "echo", "{}")});
/// provider->pushText("done");
/// agentxx::test::FakeProvider::inject(agent, "test-fake", provider);
/// ```
/// 注意: 必须在 `agent.init()` 之后注入 —— 注册模型会清除已注入的实例。
#include "agentxx/agent/context.h"
#include "agentxx/agent/model_registry.h"
#include "neograph/graph/cancel.h"
#include "neograph/provider.h"
#include <asio/awaitable.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <fmt/format.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agentxx {
namespace test {

/// 假 provider 的一次响应脚本 (按入队顺序依次生效)
struct FakeStep {
    enum class Kind {
        Text,      ///< 纯文本回复 (按 chunkChars 切片流式返回)
        ToolCalls, ///< 工具调用回复 (无正文)
        Error,     ///< 抛出异常 (文本带 `HTTP <状态码>` 供 LLM 错误分类识别)
    };

    Kind   kind = Kind::Text;
    /// Text: 完整回复正文
    std::string text;
    /// Text: 先于正文返回的思考片段 (TYPE_THINKING)
    std::string thinking;
    /// ToolCalls: 本次要调用的工具
    std::vector<neograph::ToolCall> toolCalls;
    /// Error: 模拟 HTTP 状态码 (进异常文本, 分类器按状态码/关键词判定)
    int errorStatus = 500;
    /// Error: 模拟响应体 (关键词分类用, 如 context_length_exceeded)
    std::string errorBody;
    /// 产出响应前的等待时长; 等待期间被取消则抛取消异常
    int delayMs = 0;
    /// >0: 正文按该字符数切片流式返回 (0 = 整段一次返回)
    size_t chunkChars = 0;
    /// 思考片段的切片长度 (0 = 整段)
    size_t thinkingChunkChars = 0;
    /// 用量统计 (记账与聚合断言用)
    int promptTokens     = 100;
    int completionTokens = 50;
    int cachedTokens     = 0;
    int reasoningTokens  = 0;
};

/// 假 provider: 按脚本产出响应, 并记录收到的请求
/// - 脚本用尽后按 `defaultText_` 返回 (默认 `"fake response"`), 因此"无限轮"用例
///   不必把脚本写满
/// - 所有方法仅由 agent io 线程调用 (与真实 provider 一致), 内部互斥量只为
///   测试线程读取记录时的安全
class FakeProvider final : public neograph::Provider,
                           public std::enable_shared_from_this<FakeProvider> {
public:

    using FormatDataStreamCallback = neograph::FormatDataStreamCallback;

    /// 拼接一个工具调用声明
    static neograph::ToolCall toolCall(std::string id, std::string name, std::string arguments) {
        return neograph::ToolCall{std::move(id), std::move(name), std::move(arguments)};
    }

    /// 把假 provider 注入 agent 上下文的模型注册表
    /// - 需在 `agent.init()` 之后调用
    template<typename AgentT>
    static void inject(
        AgentT&                              agent,
        std::string_view                     modelName,
        std::shared_ptr<FakeProvider>        provider
    ) {
        agent.agentContext->modelRegistry->setProvider(modelName, std::move(provider));
    }

    // ---- 脚本 ----

    /// 追加一次纯文本回复
    void pushText(std::string text, int delayMs = 0, size_t chunkChars = 0) {
        FakeStep step;
        step.kind       = FakeStep::Kind::Text;
        step.text       = std::move(text);
        step.delayMs    = delayMs;
        step.chunkChars = chunkChars;
        pushStep(std::move(step));
    }

    /// 追加一次"思考 + 正文"回复
    void pushThinking(std::string thinking, std::string answer, int delayMs = 0, size_t chunkChars = 0) {
        FakeStep step;
        step.kind           = FakeStep::Kind::Text;
        step.thinking       = std::move(thinking);
        step.text           = std::move(answer);
        step.delayMs        = delayMs;
        step.chunkChars     = chunkChars;
        step.thinkingChunkChars = chunkChars;
        pushStep(std::move(step));
    }

    /// 追加一次工具调用回复
    void pushToolCalls(std::vector<neograph::ToolCall> calls, int delayMs = 0) {
        FakeStep step;
        step.kind      = FakeStep::Kind::ToolCalls;
        step.toolCalls = std::move(calls);
        step.delayMs   = delayMs;
        pushStep(std::move(step));
    }

    /// 追加一次失败响应
    /// - `status`: 进异常文本的 HTTP 状态码 (401 鉴权 / 400+关键词 超限 / 429 限流 …)
    /// - `body`:   附加到异常文本的响应体 (关键词分类用)
    void pushError(int status, std::string body = {}, int delayMs = 0) {
        FakeStep step;
        step.kind        = FakeStep::Kind::Error;
        step.errorStatus = status;
        step.errorBody   = std::move(body);
        step.delayMs     = delayMs;
        pushStep(std::move(step));
    }

    /// 直接追加完整脚本条目 (需要设置用量等字段时使用)
    void pushStep(FakeStep step) {
        std::lock_guard<std::mutex> lock(mutex_);
        script_.push_back(std::move(step));
    }

    /// 脚本用尽后的回复文本
    void setDefaultText(std::string text) {
        std::lock_guard<std::mutex> lock(mutex_);
        defaultText_ = std::move(text);
    }

    /// 清空脚本、默认文本与全部记录
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        script_.clear();
        defaultText_ = "fake response";
        requests_.clear();
        cancelObserved_ = 0;
    }

    // ---- 记录 ----

    size_t requestCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_.size();
    }

    /// 第 index 次请求的副本 (越界抛 std::out_of_range)
    neograph::CompletionParams requestAt(size_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= requests_.size()) {
            throw std::out_of_range{"FakeProvider::requestAt: index out of range"};
        }
        return requests_[index];
    }

    std::vector<neograph::CompletionParams> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    /// 最后一次请求的消息角色序列 (断言请求体结构用; 无请求时为空)
    std::vector<std::string> lastRequestRoles() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string>    roles;
        if (requests_.empty()) {
            return roles;
        }
        for (const auto& m : requests_.back().messages) {
            roles.push_back(m.role);
        }
        return roles;
    }

    /// 在等待期间观察到取消的次数
    int cancelObserved() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cancelObserved_;
    }

    // ---- neograph::Provider ----

    std::string get_name() const override {
        return "fake";
    }

    asio::awaitable<neograph::ChatCompletion>
        complete_async(const neograph::CompletionParams& params) override {
        co_return co_await invoke_format_data(params, nullptr);
    }

    asio::awaitable<neograph::ChatCompletion> invoke_format_data(
        const neograph::CompletionParams& params,
        FormatDataStreamCallback          on_chunk
    ) override {
        // 协程挂起期间保活 (调用方可能已释放注册表里的引用)
        auto self = shared_from_this();

        const FakeStep step = takeStep();
        recordRequest(params);

        if (step.delayMs > 0 && !co_await waitInterruptible(step.delayMs, params)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cancelObserved_++;
            }
            // 与真实 provider 的网络中断同语义: 交给上层按取消处理
            throw neograph::graph::CancelledException{
                "fake provider: request cancelled while waiting"
            };
        }
        if (step.kind == FakeStep::Kind::Error) {
            throw std::runtime_error{errorTextOf(step)};
        }

        emitChunks(step.thinking, step.thinkingChunkChars, neograph::ChatStreamChunk::TYPE_THINKING, on_chunk);
        emitChunks(step.text, step.chunkChars, neograph::ChatStreamChunk::TYPE_CONTENT, on_chunk);

        neograph::ChatCompletion completion;
        completion.message.role       = "assistant";
        completion.message.content    = step.text;
        completion.message.tool_calls = step.toolCalls;
        completion.stop_reason        = step.toolCalls.empty() ? "end_turn" : "tool_use";
        completion.usage.prompt_tokens        = step.promptTokens;
        completion.usage.completion_tokens    = step.completionTokens;
        completion.usage.total_tokens         = step.promptTokens + step.completionTokens;
        completion.usage.cached_prompt_tokens = step.cachedTokens;
        completion.usage.reasoning_tokens     = step.reasoningTokens;
        co_return completion;
    }

private:

    FakeStep takeStep() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (script_.empty()) {
            FakeStep step;
            step.kind = FakeStep::Kind::Text;
            step.text = defaultText_;
            return step;
        }
        FakeStep step = std::move(script_.front());
        script_.pop_front();
        return step;
    }

    void recordRequest(const neograph::CompletionParams& params) {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(params);
    }

    /// 等待 `ms` 毫秒; 期间轮询取消令牌, 被取消返回 false
    /// - 轮询间隔 10 ms: 假 provider 没有真实套接字, 用它模拟"取消立即中断在途请求"
    asio::awaitable<bool>
        waitInterruptible(int ms, const neograph::CompletionParams& params) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{ms};
        asio::steady_timer timer{co_await asio::this_coro::executor};
        while (std::chrono::steady_clock::now() < deadline) {
            if (params.cancel_token && params.cancel_token->is_cancelled()) {
                co_return false;
            }
            auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()
            );
            timer.expires_after(std::min(remain, std::chrono::milliseconds{10}));
            // boost.asio 的 error_code 是 boost::system::error_code (不是 std::error_code)
            boost::system::error_code ec;
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
                // 协程被取消 (运行取消 / 消费方放弃): 与网络中断同语义
                co_return false;
            }
        }
        co_return true;
    }

    /// 按 UTF-8 边界切片发送流式片段 (不切断多字节字符)
    static void emitChunks(
        const std::string&             text,
        size_t                         chunkChars,
        int                            type,
        const FormatDataStreamCallback& on_chunk
    ) {
        if (!on_chunk || text.empty()) {
            return;
        }
        const size_t want = (chunkChars == 0) ? text.size() : chunkChars;
        size_t       pos  = 0;
        while (pos < text.size()) {
            size_t end = std::min(pos + want, text.size());
            // 切成多字节字符中间时向后让位到下一个字符起点
            while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
                ++end;
            }
            neograph::ChatStreamChunk chunk;
            chunk.type = type;
            chunk.data = text.substr(pos, end - pos);
            on_chunk(chunk);
            pos = end;
        }
    }

    /// 失败响应的异常文本: 带 HTTP 状态码与响应体, 便于错误分类识别
    static std::string errorTextOf(const FakeStep& step) {
        if (step.errorBody.empty()) {
            return fmt::format("fake provider error: HTTP {}", step.errorStatus);
        }
        return fmt::format("fake provider error: HTTP {} {}", step.errorStatus, step.errorBody);
    }

    mutable std::mutex              mutex_;
    std::deque<FakeStep>            script_;
    std::string                     defaultText_ = "fake response";
    std::vector<neograph::CompletionParams> requests_;
    int                             cancelObserved_ = 0;
};

} // namespace test
} // namespace agentxx
