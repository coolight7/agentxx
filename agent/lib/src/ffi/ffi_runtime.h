#pragma once

#include "agentxx/ffi_api.h"
#include "agentxx/util/log.h"
#include "ffi_client_io.h"
#include "neograph/json.h"
#include <asio/awaitable.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace agentxx {
namespace agent {
class CodeAgent;
class AgentHost;
class SessionServerAgentIO;
} // namespace agent

namespace ffi {

/// FFI 运行时 (agentxx_create 返回句柄的实体)
///
/// 持有: 独立 io_context + 专用线程 (agent io 线程)、CodeAgent、
///       AgentHost (子代理委派)、SessionServerAgentIO (服务端点)、
///       FfiClientAgentIO (FFI client 端点, 见 ffi_client_io.h)。
/// 装配复刻 agent/client/src/mode_runners.cpp setupLocalUnifiedDirect:
///   ChannelAgentIOTransport::makePair → 双端点 setTransport →
///   runTransportLoop(server) 先启动 → init → AgentHost attachRoot →
///   requestAppendComponentInfo → onServerReady → serverIO->run()。
///
/// 线程模型:
/// - 所有 agent 状态 (BaseAgent/AgentHost/SessionServerAgentIO/FfiClientAgentIO)
///   仅在 agent io 线程 (ioThread_ 运行 ioCtx_) 访问; C 事件回调同步于该线程
/// - 对外 C API (ffi_api.cpp) 在宿主任意线程调用; 会话交互类经 asio::post
///   投递 io 线程串行执行; 同步查询类经 promise/future 等待 io 线程应答
/// - stop/destroy 不得在 agent io 线程 (即回调内) 调用 (返回 AGENTXX_ERR_STATE)
class FfiAgentRuntime : public std::enable_shared_from_this<FfiAgentRuntime> {
public:

    enum class State {
        Created, ///< 已创建未启动
        Starting, ///< start 受理, init/装配进行中
        Ready,    ///< 服务端就绪 (EVT_READY 已发), 可正常交互
        Stopping, ///< stop 进行中 (等待 io 线程退出)
        Stopped,  ///< 已停止 (io 线程已退出)
        Failed,   ///< 启动失败 (init 异常; 可 stop/destroy 清理)
    };

    /// 创建运行时 (构造 agent 对象与配置; 不启动线程)
    /// @param err 非 NULL 时失败填入详情
    static std::shared_ptr<FfiAgentRuntime>
        create(const char* config_json, const char* model_json, const AgentxxCallbacks* cb, std::string& err);

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

    bool isOnAgentThread() const;

    // -------------------------------------------------------------------
    // 生命周期 (对外 C API 直接转发)
    // -------------------------------------------------------------------

    /// 异步启动; 返回 0 成功 (就绪经 EVT_READY, 失败经 EVT_ERROR)
    int start(std::string& err);

    /// 同步停止并等待 io 线程退出 (幂等; 非 io 线程)
    int stop(std::string& err);

    /// 销毁 (未 stop 时自动 stop; 非 io 线程)
    int destroy(std::string& err);

    // -------------------------------------------------------------------
    // 会话交互 (异步, 投递 io 线程)
    // -------------------------------------------------------------------

    int sendInput(std::string_view text, std::string& err);
    int cancel(std::string& err);
    int selectModel(std::string_view modelName, std::string& err);
    int setPermission(std::string_view path, int allow, int op, std::string& err);
    int switchSession(std::string_view threadId, std::string& err);

    // -------------------------------------------------------------------
    // 同步查询 (阻塞等待服务端应答, 最长 10s; 返回 JSON 字符串或空)
    // -------------------------------------------------------------------

    std::string getModelInfo(std::string& err);
    std::string getContextMessages(std::string& err);
    std::string listSessions(std::string& err);

    // -------------------------------------------------------------------
    // HIL 中断
    // -------------------------------------------------------------------

    /// 指定 id 的中断是否仍在等待应答 (任意线程)
    bool hasPendingInterrupt(int64_t interruptId) const;

    /// 应答中断 (任意线程; 内部投递 io 线程)
    int interruptRespond(int64_t interruptId, const char* valuesJson, std::string& err);

    // -------------------------------------------------------------------
    // 日志
    // -------------------------------------------------------------------

    /// 取走积压日志 JSON 数组并清空
    std::string drainLogs();

    /// 当前绑定会话 thread_id
    std::string_view threadId() const {
        return threadId_;
    }

private:

    /// 日志条目 (环形缓冲)
    struct LogItem {
        int         level = 0; ///< 0=trace 1=debug 2=info 3=warn 4=error
        std::string message;
    };

    /// 日志接收器 (自带后台线程; 入 ring)
    class FfiLogSink final : public util::ThreadedLogSink {
    public:

        FfiLogSink(FfiAgentRuntime& owner) :
            owner_(owner) {}
        ~FfiLogSink() override {
            shutdownThread();
        }

    protected:

        void onLog(const util::LogEntry& entry) override;

    private:

        FfiAgentRuntime& owner_;
    };

    /// 同步查询等待器 (promise; 调用方线程与 io 线程共享, syncMutex_ 保护)
    struct SyncWait {
        std::promise<std::string> promise;
    };

    // 单例构造 (经 create 工厂; 保证 enable_shared_from_this 可用)
    explicit FfiAgentRuntime();
    bool buildConfigs(const char* config_json, const char* model_json, std::string& err);

    /// io 线程: 装配端点/传输并启动各协程 (复刻 setupLocalUnifiedDirect)
    void startOnIoThread();

    /// io 线程: 主协程 (init → host → ready → serverIO->run())
    asio::awaitable<void> runAgentMain();

    /// 停止实装 (调用方线程执行; 幂等)
    void stopInternal();

    /// io 线程: 处理同步应答 (完成对应 SyncWait)
    void onSyncReplyOnIoThread(FfiClientAgentIO::SyncKind kind, neograph::json j);

    /// 同步查询通用实现; send 为 io 线程执行的请求发送动作
    std::string syncQuery(FfiClientAgentIO::SyncKind kind, std::function<void()> send, std::string& err);

    /// 生成唯一会话 thread_id
    static std::string generateThreadId();

    /// 追加一条日志到环形缓冲 (FfiLogSink 调用; 任意线程)
    void pushLogItem(LogItem item);

    // ---- 运行时拓扑 ----
    std::shared_ptr<asio::io_context>                 ioCtx_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> workGuard_;
    std::thread                                       ioThread_;
    std::shared_ptr<agent::CodeAgent>                 agent_;
    std::shared_ptr<agent::AgentHost>                 host_;
    std::shared_ptr<agent::SessionServerAgentIO>      serverIO_;
    std::shared_ptr<FfiClientAgentIO>                 clientIO_;
    std::string                                       threadId_;
    std::atomic<State>                                state_{State::Created};

    /// HIL 中断等待宿主应答超时 (SessionServerAgentIO 配置)
    std::chrono::milliseconds                        interruptTimeout_{0};

    // ---- 事件回调 (值拷贝) ----
    AgentxxCallbacks callbacks_{};

    // ---- 同步查询等待 (syncMutex_ 保护) ----
    mutable std::mutex syncMutex_;
    std::map<FfiClientAgentIO::SyncKind, std::deque<std::shared_ptr<SyncWait>>> syncWaits_;

    // ---- 日志环形缓冲 (logMutex_ 保护) ----
    std::shared_ptr<FfiLogSink> logSink_;
    mutable std::mutex          logMutex_;
    std::deque<LogItem>         logRing_;
    static constexpr size_t     kLogRingCap = 512;
};

} // namespace ffi
} // namespace agentxx