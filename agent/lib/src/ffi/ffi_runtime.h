#pragma once

#include "agentxx/ffi_api.h"
#include "ffi_call.h"
#include "ffi_client_io.h"
#include "ffi_ops.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "asio/executor_work_guard.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace asio = ::boost::asio;

namespace agentxx {
namespace agent {
class CodeAgent;
class AgentHost;
class SessionServerAgentIO;
class AgentSessionRuntime;
} // namespace agent

namespace ffi {

/// 把 C ABI 字符串视图转为 std::string_view (零拷贝; NULL/空视图返回空)
///
/// - 供 ffi_api.cpp (C 导出面) 与 ffi_runtime.cpp (运行时) 共用,
///   避免"入参读取"语义在两处各写一份
inline std::string_view toSv(const AgentxxStringView* sv) {
    if (sv == nullptr || sv->data == nullptr || sv->size == 0) {
        return {};
    }
    return std::string_view{sv->data, static_cast<size_t>(sv->size)};
}

/// FFI 运行时 (agentxx_ffi_create 返回句柄的实体)
///
/// 线程结构 (独立 Client-IO 线程 + 独立 Server-IO 线程 + 可选的收尾线程):
/// - Server-IO 线程 (serverThread_ 运行 serverIoCtx_):
///   运行 CodeAgent / AgentHost / SessionServerAgentIO, 处理 ReAct 循环、工具调用、
///   LLM 流式请求、SessionStore (单线程读写保证安全)。
/// - Client-IO 线程 (clientThread_ 运行 clientIoCtx_):
///   运行 FfiClientAgentIO (客户端端点), 处理 Wire 协议编解码、事件分发与嵌入方
///   C 回调、挂起的中断等待与合批、调用回执的组装, 以及超时检查与日志搬运。
/// - 收尾线程 (finishThread_, 只在 stop 时起): 等会话停下 → 插件 shutdown → 停两条
///   io 线程 → join → 置 Stopped → 发 `EVT_STOPPED`。它只做收尾, 不接受调用。
/// - 两端点通过进程内 ChannelAgentIOTransport::makePair(clientEx, agentEx) 直连。
///
/// 调用模型: 所有业务操作都走 [call] —— 提交后立即返回 reqId, 结果经 `EVT_RESULT`
/// 由 client io 线程发出; 调用线程上没有任何跨线程等待 (事件回调里也能调)。
class FfiAgentRuntime : public std::enable_shared_from_this<FfiAgentRuntime> {
public:

    enum class State {
        Created,  ///< 已创建未启动
        Starting, ///< start 受理, init/加载进行中
        Ready,    ///< 服务端就绪 (EVT_READY 已发), 可正常交互
        Stopping, ///< stop 进行中 (收尾线程在跑)
        Stopped,  ///< 已停止 (所有线程已退出, EVT_STOPPED 已发)
        Failed,   ///< 启动失败 (init 异常; 可 stop/destroy 清理)
    };

    /// 创建运行时 (构造 agent 对象与配置; 不启动线程)
    /// - [err] 非 NULL 时失败填入详情
    static std::shared_ptr<FfiAgentRuntime> create(
        const AgentxxStringView*   config_json,
        const AgentxxStringView*   model_json,
        const AgentxxFFICallbacks* cb,
        std::string&               err
    );

    ~FfiAgentRuntime();

    FfiAgentRuntime(const FfiAgentRuntime&)            = delete;
    FfiAgentRuntime& operator=(const FfiAgentRuntime&) = delete;

    State state() const {
        return state_.load(std::memory_order_acquire);
    }

    /// 是否已就绪可交互
    bool isReady() const {
        return state() == State::Ready;
    }

    /// 是否受理新的调用 (Starting / Ready)
    bool acceptsCalls() const {
        const State st = state();
        return st == State::Starting || st == State::Ready;
    }

    /// 是否在 agent io 线程
    bool isOnAgentThread() const;

    /// 是否在 client io 线程 (即事件回调执行线程)
    bool isOnClientThread() const;

    /// 是否在收尾线程
    bool isOnFinishThread() const;

    /// 是否在任意内部线程 (用于判断 destroy 调用合法性)
    bool isOnAnyInternalThread() const {
        return isOnAgentThread() || isOnClientThread() || isOnFinishThread();
    }

    // -------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------

    /// 异步启动; 返回 0 成功 (就绪经 EVT_READY, 失败经 EVT_ERROR)
    int start(std::string& err);

    /// 受理停止; 返回 0 = 已受理或早已停止 (收尾完成时发 EVT_STOPPED)
    int stop(std::string& err);

    /// 销毁 (只在 Created / Stopped 状态成功)
    int destroy(std::string& err);

    // -------------------------------------------------------------------
    // 调用 (提交 + 回执)
    // -------------------------------------------------------------------

    /// 提交一次调用
    /// - [outReqId] 受理成功时写入 reqId; 同步失败时不写
    /// - `return` 0 = 已受理 (结果经 EVT_RESULT); 其余为同步错误 (不会有回执)
    int call(std::string_view requestJson, uint64_t* outReqId, std::string& err);

    /// 取消一次仍在等待的调用 (只停等待, 不撤回服务端操作)
    int callCancel(uint64_t reqId, std::string& err);

    // -------------------------------------------------------------------
    // 本地同步信息
    // -------------------------------------------------------------------

    std::string getCapabilities() const;

    /// 取走积压日志 JSON 数组并清空
    std::string drainLogs();

    // -------------------------------------------------------------------
    // 供 op 处理函数使用
    // -------------------------------------------------------------------

    std::shared_ptr<FfiClientAgentIO> clientIO() const {
        return clientIO_;
    }

    asio::io_context* clientIoCtx() const {
        return clientIoCtx_.get();
    }

    std::shared_ptr<agent::CodeAgent> agent() const {
        return agent_;
    }

    /// 当前绑定的会话 id (快照)
    std::string currentSessionId() const;

    /// 切换本地会话绑定 (`session.switch` 提交时调用)
    /// - 只改本端后续调用的会话作用域, 真正切换由服务端处理
    /// - 同时把 client 端点的会话 id 改掉 (它把会话写进事件载荷)
    void setSessionBinding(std::string sessionId);

    /// 当前生效的语言 (规范化的语言代码)
    std::string language() const;

    /// 把语言应用到本句柄与当前会话 (投递到 server io; 受理即返回)
    void applyLanguage(std::string normalized);

    /// 设置增量合批窗口 (投递到 client io; <=0 关闭)
    void applyDeltaBatch(int32_t maxDelayMs);

    /// 指定 id 的中断是否仍在等待应答 (任意线程, lock-free 读取)
    bool hasPendingInterrupt(int64_t interruptId) const;

    // -------------------------------------------------------------------
    // client io 侧回调
    // -------------------------------------------------------------------

    /// 一次调用的回执到达 (client io 线程)
    void onCallReply(
        uint64_t          reqId,
        utilxx_base::Json result,
        bool              ok,
        std::string       reason,
        std::string       message
    );

    /// 端点收尾: 仍在等待的调用全部按取消收尾 (client io 线程, join 之前)
    void onRuntimeStopping();

    /// 上报与调用无关的内部错误 (任意线程; EVT_ERROR)
    void notifyError(int code, std::string name, std::string message);

private:

    /// 日志条目 (环形缓冲)
    struct LogItem {
        int         level = 0; ///< 0=trace 1=debug 2=info 3=warn 4=error
        std::string message;
    };

    /// 日志接收器
    /// - 只做入队 (生产者线程) 与搬运 (client io 线程 1s 一次 pump)
    /// - 队列满时丢新条目, 因此必须低频 pump (排队等 drain_logs 才 pump 会让
    ///   新日志全部丢掉, 保留的全是最旧的)
    class FfiLogSink final : public utilxx_base::LogSink {
    public:

        FfiLogSink(FfiAgentRuntime& owner) :
            owner_(owner) {}

    protected:

        void onLog(const utilxx_base::LogEntry& entry) override;

    private:

        FfiAgentRuntime& owner_;
    };

    // 单例构造 (经 create 工厂; 保证 enable_shared_from_this 可用)
    explicit FfiAgentRuntime();
    bool buildConfigs(
        const AgentxxStringView* config_json,
        const AgentxxStringView* model_json,
        std::string&             err
    );

    /// 收尾实装 (收尾线程执行; 幂等)
    void stopInternal();

    /// 收尾完成后: 置 Stopped 并发 EVT_STOPPED (最后一个事件)
    void finishStop();

    /// 回收收尾线程 (已在收尾线程上时改为 detach, 避免自我 join)
    void joinFinishThreadIfNeeded();

    /// 服务端点 (未启动时为空; 所有权在启动壳)
    std::shared_ptr<agent::SessionServerAgentIO> serverIO() const;

    /// 回执投递 (client io 线程同步发, 其余线程 post 过去)
    void deliverResult(uint64_t reqId, std::string_view op, const CallOutcome& outcome);

    /// 排定日志搬运 (client io; 1s 一次)
    void scheduleLogPump();

    /// 生成唯一会话 sessionId
    static std::string generateSessionId();

    /// 追加一条日志到环形缓冲 (FfiLogSink 调用; 任意线程)
    void pushLogItem(LogItem item);

    // ---- 1. io_context 执行器 (必须最先声明，以便最后析构) ----
    std::shared_ptr<asio::io_context>                                         serverIoCtx_;
    std::shared_ptr<asio::io_context>                                         clientIoCtx_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> serverWorkGuard_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> clientWorkGuard_;
    std::thread                                                               serverThread_;
    std::thread                                                               clientThread_;

    // ---- 2. 依赖 io_context 的实体对象 (后声明，先析构) ----
    std::shared_ptr<agent::CodeAgent>           agent_;
    /// 统一启动壳 (两端点装配 / init 与驱动顺序 / 关闭顺序)
    std::shared_ptr<agent::AgentSessionRuntime> runtime_;
    /// client 端点 (仅作为便捷引用; 所有权在 [runtime_])
    std::shared_ptr<FfiClientAgentIO> clientIO_;
    /// 待完成调用表 (客户端应答 / 取消 / 超时 / 收尾都经它结算)
    std::shared_ptr<CallRegistry> calls_;
    /// 日志搬运定时器 (client io)
    std::shared_ptr<asio::steady_timer> logPumpTimer_;

    // ---- 3. 状态与会话 ----
    /// 会话 id (切换会话会改写; sessionMutex_ 保护)
    std::string              sessionId_;
    mutable std::mutex       sessionMutex_;
    std::atomic<State>       state_{State::Created};
    /// stop 是否已经受理 (保证只发一次 EVT_STOPPED)
    std::atomic<bool> stopRequested_{false};
    /// 收尾线程 (只在 stop 时创建; destroy/析构时 join)
    std::thread finishThread_;

    /// 首屏同步的尾窗条数 (config_json 的 `syncTailCount`, 默认 200; 0 = 完整同步)
    size_t initialSyncTailCount_ = 200;
    /// 模型增删等 op 的默认超时 (毫秒): 要写 yaml 落盘, 给得比查询宽
    // (op 表里各自声明, 这里不重复)

    /// HIL 中断等待嵌入方程序应答超时 (SessionServerAgentIO 配置)
    std::chrono::milliseconds interruptTimeout_{0};

    // ---- 事件回调 (值拷贝) ----
    AgentxxFFICallbacks callbacks_{};

    // ---- 日志环形缓冲 (logMutex_ 保护) ----
    std::shared_ptr<FfiLogSink> logSink_;
    mutable std::mutex          logMutex_;
    std::deque<LogItem>         logRing_;
    static constexpr size_t     kLogRingCap = 512;

    // ---- 语言设置 (langMutex_ 保护) ----
    mutable std::mutex langMutex_;
    std::string        language_ = "en";
};

} // namespace ffi
} // namespace agentxx
