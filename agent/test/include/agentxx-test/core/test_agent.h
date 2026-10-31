#pragma once

#include "utilxx/http_server.h"
#include "utilxx_base/json.h"
#include <asio/awaitable.hpp>
#include <atomic>
#include <concepts>
#include <memory>
#include <mutex>
#include <neograph/api.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

// ===========================================================================
// Local LLM Simulator — an OpenAI-compatible HTTP server
// ===========================================================================

/// 模拟器共享量的锁 (递归: 包装量内部也要拿它, 所以允许在已持锁的块里继续用包装量)
std::recursive_mutex& daSimMutex();

/// 模拟器共享量的持锁包装
///
/// 这些量同时被**模拟器 HTTP 服务线程**(请求处理时读写) 与**测试线程**(设置/断言) 访问,
/// 所以一律包成本类型: 读在锁内拷一份副本返回, 写在锁内整体替换, 测试代码保持
/// `g_da_sim_x = ...` / `g_da_sim_x.dump()` 的写法即可, 不必到处手写加锁。
/// - 曾经出现过"测试线程 dump 请求记录, 服务线程同时 push_back 触发 vector 扩容"
///   导致的 heap-use-after-free —— 包装后读到的副本与全局量不再有关联
/// - 拷贝构造 = 锁内取快照 (`const auto copy = g_da_sim_last_request;` 得到独立副本)
/// - 要把若干个量当成一个整体读/写时, 用 `daSimMutex()` 自行加锁
/// - 相关: 单独取最近请求 / 请求记录快照可用 [daSimLastRequestText] / [daSimRequestsSnapshot]
template <typename T>
class DaSimValue {
public:
    using value_type = T;

    DaSimValue() = default;

    /// 初始值 (显式构造: 避免 `g_da_sim_x = 值` 与拷贝赋值产生二义)
    explicit DaSimValue(T initial) : value_(std::move(initial)) {}

    /// 拷贝 = 锁内取快照
    DaSimValue(const DaSimValue& other) : value_(other.get()) {}

    DaSimValue& operator=(const DaSimValue& other) {
        if (this != &other) {
            *this = other.get();
        }
        return *this;
    }

    /// 锁内取一份副本
    T get() const {
        std::lock_guard<std::recursive_mutex> lock(daSimMutex());
        return value_;
    }

    operator T() const {
        return get();
    }

    /// 锁内整体替换
    DaSimValue& operator=(T value) {
        std::lock_guard<std::recursive_mutex> lock(daSimMutex());
        value_ = std::move(value);
        return *this;
    }

    /// 清空 (锁内; 需 `T::clear()`)
    void clear()
        requires requires(T& v) { v.clear(); }
    {
        std::lock_guard<std::recursive_mutex> lock(daSimMutex());
        value_.clear();
    }

    /// 元素数 / 字节数 (锁内)
    size_t size() const
        requires requires(const T& v) { v.size(); }
    {
        std::lock_guard<std::recursive_mutex> lock(daSimMutex());
        return value_.size();
    }

    /// 是否为空 (锁内)
    bool empty() const
        requires requires(const T& v) { v.empty(); }
    {
        std::lock_guard<std::recursive_mutex> lock(daSimMutex());
        return value_.empty();
    }

    /// 追加元素 (锁内; 仅 `std::vector<utilxx_base::Json>` 的请求记录)
    void push_back(utilxx_base::Json item)
        requires requires(T& v, utilxx_base::Json e) { v.push_back(std::move(e)); }
    {
        std::lock_guard<std::recursive_mutex> lock(daSimMutex());
        value_.push_back(std::move(item));
    }

    /// 末尾元素副本 (锁内; 仅 `std::vector<utilxx_base::Json>` 的请求记录)
    utilxx_base::Json back() const
        requires requires(const T& v) { v.back(); }
    {
        std::lock_guard<std::recursive_mutex> lock(daSimMutex());
        if (value_.empty()) {
            throw std::out_of_range("DaSimValue::back: no element");
        }
        return value_.back();
    }

    /// 按下标读取元素副本 (锁内; 仅 `std::vector<utilxx_base::Json>` 的请求记录)
    utilxx_base::Json operator[](size_t index) const
        requires requires(const T& v, size_t i) { v[i]; }
    {
        std::lock_guard<std::recursive_mutex> lock(daSimMutex());
        if (index >= value_.size()) {
            throw std::out_of_range("DaSimValue::operator[]: index out of range");
        }
        return value_[index];
    }

    /// 按键读取副本 (锁内; 仅 `utilxx_base::Json`)
    auto operator[](std::string_view key) const
        requires requires(const T& v, std::string_view k) { v[k]; }
    {
        using Value = std::remove_cvref_t<decltype(std::declval<const T&>()[std::string_view{}])>;
        std::lock_guard<std::recursive_mutex> lock(daSimMutex());
        return Value{value_[key]};
    }

    /// 序列化为 JSON 文本 (锁内; 仅 `utilxx_base::Json`)
    std::string dump() const
        requires requires(const T& v) { v.dump(); }
    {
        std::lock_guard<std::recursive_mutex> lock(daSimMutex());
        return value_.dump();
    }

    /// 按键安全提取, 缺失/类型不符返回默认值 (锁内; 仅 `utilxx_base::Json`)
    template <typename U>
    U value(std::string_view key, const U& defaultVal) const
        requires requires(const T& v, std::string_view k, const U& d) { v.value(k, d); }
    {
        std::lock_guard<std::recursive_mutex> lock(daSimMutex());
        return value_.value(key, defaultVal);
    }

    /// 是否含某键 (锁内; 仅 `utilxx_base::Json`)
    bool contains(std::string_view key) const
        requires requires(const T& v, std::string_view k) { v.contains(k); }
    {
        std::lock_guard<std::recursive_mutex> lock(daSimMutex());
        return value_.contains(key);
    }

    /// 自增 (锁内; 仅整型量)
    DaSimValue& operator++()
        requires std::integral<T> && (!std::same_as<T, bool>)
    {
        std::lock_guard<std::recursive_mutex> lock(daSimMutex());
        ++value_;
        return *this;
    }

    /// 自减 (锁内; 仅整型量, 服务线程用它递减剩余失败次数)
    DaSimValue& operator--()
        requires std::integral<T> && (!std::same_as<T, bool>)
    {
        std::lock_guard<std::recursive_mutex> lock(daSimMutex());
        --value_;
        return *this;
    }

    T operator++(int)
        requires std::integral<T> && (!std::same_as<T, bool>)
    {
        std::lock_guard<std::recursive_mutex> lock(daSimMutex());
        return value_++;
    }

    T operator--(int)
        requires std::integral<T> && (!std::same_as<T, bool>)
    {
        std::lock_guard<std::recursive_mutex> lock(daSimMutex());
        return value_--;
    }

private:
    T value_{};
};

// 下面的量都跨线程共享, 访问一律经上面的包装 (读写都在锁内)
extern DaSimValue<std::string>       g_da_sim_response_content;
extern DaSimValue<int>               g_da_sim_prompt_tokens;
extern DaSimValue<int>               g_da_sim_completion_tokens;
extern DaSimValue<utilxx_base::Json> g_da_sim_tool_calls;
/// 最后一次收到的 /chat/completions 请求体 (供测试断言模型名/消息前缀)
extern DaSimValue<utilxx_base::Json> g_da_sim_last_request;
/// 按到达顺序记录所有 /chat/completions 请求 (供测试断言多次请求)
extern DaSimValue<std::vector<utilxx_base::Json>> g_da_sim_requests;
/// 模拟 thinking 模型的推理文本: 非空时流式响应先推送一段
/// [reasoning_content] delta (TYPE_THINKING), 并通过非流式的 message 字段返回,
/// 用于验证 reasoning_content → Think 历史消息的持久化流程
extern DaSimValue<std::string> g_da_sim_reasoning_content;
/// 响应前延迟 (毫秒), 用于模拟慢速 LLM 以测试取消; 0 表示不延迟
extern DaSimValue<int> g_da_sim_delay_ms;
/// 累计请求计数 (每次 /chat/completions 请求递增, 含失败请求), 供测试验证调用次数
/// - 由模拟器 HTTP 服务线程递增、测试线程轮询读取, 本来就是原子量, 读它不必加锁
/// - 请求记录本身要另取: 先看计数增长, 再用 [daSimRequestsSnapshot] / 包装量读取
extern std::atomic<int> g_da_sim_request_count;
/// 剩余失败次数: >0 时接下来的请求直接返回 HTTP 500 并递减, 用于模拟 LLM API 持续失败
extern DaSimValue<int> g_da_sim_fail_count;
/// 失败响应的 HTTP 状态码 (0 = 默认 500): 供测试区分"可重试"与"不可重试"错误 ——
/// 例如 401 鉴权失败应立即停止重试, 400 + 上下文超限关键词应触发压缩后重试
extern DaSimValue<int> g_da_sim_fail_status;
/// 失败响应的响应体 (空 = 默认 `{"error":{"message":"simulated failure"}}`)
extern DaSimValue<std::string> g_da_sim_fail_body;
/// 前 N 次请求返回 tool_calls (之后返回纯文本); -1 = 不限制 (旧行为)
extern DaSimValue<int> g_da_sim_tool_calls_remaining;

/// 最近一次 /chat/completions 请求体的 JSON 文本 (锁内拷贝; 没有请求时返回空串)
std::string daSimLastRequestText();

/// 按到达顺序的请求快照 (锁内拷贝)
std::vector<utilxx_base::Json> daSimRequestsSnapshot();

/// 设置下一次响应要返回的 tool_calls 与生效次数 (锁内; `remaining` < 0 = 不限制)
void daSimSetToolCalls(utilxx_base::Json calls, int remaining);

/// 设置响应正文 (锁内); 空串 = 最小回复 `done`
void daSimSetResponseContent(std::string content);

/// 设置响应前延迟毫秒数 (锁内; 0 = 不延迟)
void daSimSetDelayMs(int ms);

// ===========================================================================
// 模拟前缀缓存 (provider prompt cache)
//
// 打开后模拟器按"工具定义 + 消息前缀"与上一条成功请求逐字节比较, 把命中的
// token 数写进 `usage.prompt_tokens_details.cached_tokens`, 供压缩请求的
// 前缀对齐回归测试使用 (主程序侧已有的缓存用量解析会记入台账与关键指标)。
// 同时 prompt_tokens 改为按本次请求体大小估算 (否则比例没有意义)。
// ===========================================================================

/// 是否启用模拟前缀缓存 (默认关闭, 不影响其他用例)
extern DaSimValue<bool> g_da_sim_prefix_cache_enabled;
/// 最近一条成功请求的 estimated(cached / prompt), 未启用时为 0
extern DaSimValue<double> g_da_sim_last_cache_ratio;
/// 最近一条成功请求的命中前缀 token 数与总 prompt token 数 (估算值)
extern DaSimValue<int> g_da_sim_last_cached_tokens;
extern DaSimValue<int> g_da_sim_last_prompt_tokens;

/// 模拟器配置选项
struct DaSimConfig {
    bool                      enableRandomDelay = false; // 启用随机延迟模拟网络抖动
    std::chrono::milliseconds minDelay{5};               // 最小延迟（毫秒）
    std::chrono::milliseconds maxDelay{50};              // 最大延迟（毫秒）

    bool injectError429    = false; // 模拟限流错误 (HTTP 429)
    bool injectError503    = false; // 模拟服务不可用 (HTTP 503)
    bool injectStreamBreak = false; // 模拟流式断开

    bool   supportThinkingBlock = false; // 支持 Anthropic thinking block
    bool   streamByWords        = true;  // 按单词分片模拟真实流量
    size_t wordsPerChunk        = 5;     // 每次 chunk 的单词数
};

struct DaSimServer {
    std::unique_ptr<utilxx::HttpServer> svr;
    std::thread                         thr;
    uint16_t                            port = 0;

    DaSimServer() = default;
    DaSimServer(DaSimServer&& o) noexcept;
    DaSimServer& operator=(DaSimServer&& o) noexcept;
    DaSimServer(const DaSimServer&)            = delete;
    DaSimServer& operator=(const DaSimServer&) = delete;
    ~DaSimServer();

    void stop();
};

asio::awaitable<TestResult> run_agent_tests();

/// 启动 LLM 模拟器
DaSimServer startDaSimServer();

/// LLM API 持续失败时重试耗尽的行为测试 (重试停止 + 不重复执行 toolcall)
asio::awaitable<void> test_agent_llm_retry_exhaust();

/// Toolcall 拦截普通异常继续运行的行为测试 (错误消息化 + agent 继续)
asio::awaitable<void> test_agent_toolcall_intercept_exception();

} // namespace test
} // namespace agentxx
