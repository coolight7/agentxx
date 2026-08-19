#pragma once

#include "agentxx/agent/io/agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/ffi_api.h"
#include "asio/any_io_executor.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include "neograph/json.h"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace agentxx {
namespace ffi {

/// FFI client 端点: 自定义 AgentIOBase client 端点 (与 TUIClientAgentIO /
/// StdIOClientAgentIO 同级), 将 agent 服务端产出的事件转为 C 回调 (JSON
/// payload), 并将 FFI 侧的输入/取消/模型切换等 C API 转为 Wire 消息。
///
/// 线程约定:
/// - 所有被动回调 (onDelta/onSync/onPeerMessage 等) 在 agent io 线程执行;
///   对外事件回调同步调用, 必须快速返回
/// - submitInterruptResponse / 同步应答路由 (onSyncReply) 由运行层显式投递
///   到 io 线程后调用; 端点内除 activeIds_ (互斥锁保护) 外无锁
/// - isOnAgentThread(): 判定 stop/destroy 是否在回调线程内 (拒绝执行)
class FfiClientAgentIO : public agent::AgentIOBase,
                         public std::enable_shared_from_this<FfiClientAgentIO> {
public:

    FfiClientAgentIO(asio::any_io_executor ex, AgentxxCallbacks callbacks);

    ~FfiClientAgentIO() override;

    /// 设置本端点绑定会话 thread_id (运行层在装配时设置)
    void setThreadId(std::string threadId);

    /// 设置 agent io 线程 id (运行层注入; 判 stop/destroy 调用方)
    void setAgentThreadId(std::thread::id tid);

    /// 是否当前线程为 agent io 线程
    bool isOnAgentThread() const;

    /// io 线程: 上报内部错误事件 (EVT_ERROR); 任意线程可调用 (自动 post)
    void notifyError(int code, std::string message);

    /// 服务端就绪通知 (运行层在 init 完成后调用; 触发 EVT_READY)
    void notifyServerReady();

    // -----------------------------------------------------------------------
    // AgentIOBase 纯虚实现 (client 端点)
    // -----------------------------------------------------------------------

    asio::awaitable<std::optional<std::string>> getInput() override;

    /// 本 client 端点不注册会话总线, 此纯虚实现仅满足契约 (返回值未使用);
    /// 真实 HIL 流程: onPeerMessage(WireInterruptRequest) → waitHostInterrupt()
    asio::awaitable<neograph::json> handleInterrupt(
        std::string_view threadId,
        std::string_view interruptNode,
        std::string_view interruptValue,
        std::string_view interruptArgJson
    ) override;

    // -----------------------------------------------------------------------
    // FFI 应答通道 (由运行层投递到 io 线程后调用)
    // -----------------------------------------------------------------------

    /// 是否仍有指定 id 的中断在等待应答 (任意线程; 应答前参数校验)
    bool hasPendingInterrupt(int64_t interruptId) const;

    /// 提交中断应答 (io 线程): 完成挂起的等待并触发回送 WireInterruptResponse;
    /// id 无效返回 false
    bool submitInterruptResponse(int64_t interruptId, neograph::json values);

    /// 失败全部挂起中断 (停止时调用; io 线程)
    void failAllPendingInterrupts();

    // -----------------------------------------------------------------------
    // 同步应答路由 (由运行层注入; 收到对应 Wire 响应时调用)
    // -----------------------------------------------------------------------

    enum class SyncKind {
        ModelInfo,
        ContextMessages,
        SessionList,
    };

    /// io 线程: 把指定类型的服务端应答转发给运行层同步查询等待方
    std::function<void(SyncKind, neograph::json)> onSyncReply;

protected:

    // ---- AgentIOBase 被动接收回调 ----
    void onDelta(const agent::Delta& delta) override;

    void onSync(const agent::SyncPayload& payload) override;

    void onTurnResult(const agent::WireTurnResult& result) override;

    void onContextStats(const agent::WireContextStats& stats) override;

    /// 服务端就绪 (运行层在 init 完成后调用): 发 EVT_READY
    void onServerReady() override;

    /// 处理对端消息: 拦截中断/模型/组件/插件/错误/同步应答, 其余委托基类
    void onPeerMessage(agent::WireMessage msg) override;

private:

    using ErrorCode   = neograph_asio_error_code;
    using RespChannel = asio::experimental::concurrent_channel<void(ErrorCode, neograph::json)>;

    /// io 线程: 挂起等待宿主应答 (agentxx_interrupt_respond 经 submitInterruptResponse
    /// 完成 channel); 返回 {answered=false} 表示通道被关闭 (过期/停止, 不回送响应)
    asio::awaitable<std::pair<bool, neograph::json>> waitHostInterrupt(
        int64_t id, std::shared_ptr<RespChannel> ch
    );

    /// io 线程: 发事件到 C 回调 (异常不外泄)
    void emitEvent(AgentxxEventType type, std::string json);

    static std::string dump(const neograph::json& j);

    asio::any_io_executor ex_;

    /// 事件回调 (值拷贝, 保持有效)
    AgentxxCallbacks callbacks_;
    /// 本端点绑定的会话 thread_id (EVT_READY payload 使用)
    std::string threadId_;

    /// 挂起的中断应答通道 (wire id → channel; pendingMutex_ 保护)
    std::map<int64_t, std::shared_ptr<RespChannel>> pending_;
    mutable std::mutex pendingMutex_;

    /// 挂起中断 id 快照 (任意线程只读; 仅应答前校验用)
    mutable std::mutex idsMutex_;
    std::set<int64_t>  activeIds_;

    /// agent io 线程 id (由运行层设置; 判 stop/destroy 调用方)
    std::thread::id agentThreadId_;
};

} // namespace ffi
} // namespace agentxx