#pragma once

#include "agentxx/agent/io/embedded_client_agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/ffi_api.h"
#include "asio/any_io_executor.hpp"
#include "utilxx_base/json.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>

namespace agentxx {
namespace ffi {

/// FFI client 端点: 自定义 AgentIOBase client 端点 (与 TUIClientAgentIO /
/// StdIOClientAgentIO 同级), 将 agent 服务端产出的事件转为 C 回调 (JSON
/// payload), 并将 FFI 侧的输入/取消/模型切换等 C API 转为 Wire 消息。
///
/// 中断应答、文本合批与线程判定来自 [EmbeddedClientAgentIO] (与插件通道端点共用);
/// 本类只负责"把事件变成 C 回调"和 FFI 专有的消息 (嵌入方程序工具/模型管理/同步应答)。
///
/// 线程约定 (方案 A: 独立 Client-IO 线程, 完全无锁设计):
/// - 本端点状态 (pending_、sessionId_、callbacks_ 等) 仅在 client io 线程读写
/// - 所有被动回调 (onDelta/onSync/onPeerMessage 等) 及对外事件回调 (on_event) 均在此线程执行
/// - 中断状态使用 lock-free std::atomic 维护, 任意线程无锁秒级校验
/// - submitInterruptResponse / onSyncReply / failAllPendingInterrupts 均在 client io 线程执行
class FfiClientAgentIO : public agent::EmbeddedClientAgentIO,
                         public std::enable_shared_from_this<FfiClientAgentIO> {
public:

    FfiClientAgentIO(asio::any_io_executor ex, AgentxxFFICallbacks callbacks);

    ~FfiClientAgentIO() override;

    /// 上报内部错误事件 (EVT_ERROR); 任意线程可调用 (自动投递到 client io 线程)
    void notifyError(int code, std::string message);

    /// 服务端就绪通知 (触发 EVT_READY; 任意线程可调用, 自动投递到 client io 线程)
    void notifyServerReady();

    /// 失败全部挂起中断 (client io 线程调用; 停止或析构时清理)
    /// - 在基类清理之上, 额外为尚未应答的嵌入方程序工具调用上报取消事件
    void failAllPendingInterrupts() override;

    /// 提交嵌入方程序工具调用应答 (client io 线程)
    bool submitHostToolResponse(int64_t callId, int32_t isError, std::string resultJson);

    // -----------------------------------------------------------------------
    // 同步应答路由 (由运行层注入; 收到对应 Wire 响应时在 client io 线程调用)
    // -----------------------------------------------------------------------

    enum class SyncKind {
        ModelInfo,
        ContextMessages,
        SessionList,
        AddModelResult,
        RemoveModelResult,
    };

    /// client io 线程: 把指定类型的服务端应答转发给运行层同步查询等待方
    std::function<void(SyncKind, utilxx_base::Json)> onSyncReply;

protected:

    // ---- AgentIOBase 被动接收回调 (均在 client io 线程执行) ----
    /// 完整/部分同步: FFI 把历史同步载荷原样转给嵌入方程序 (EVT_SYNC)
    void onSync(const agent::WireSyncPayload& payload) override;

    void onTurnResult(const agent::WireTurnResult& result) override;

    void onContextStats(const agent::WireContextStats& stats) override;

    /// 服务端就绪 (触发 EVT_READY)
    void onServerReady() override;

    /// 处理对端消息: 拦截中断/模型/组件/插件/错误/同步应答, 其余委托基类
    void onPeerMessage(agent::WireMessage msg) override;

private:

    /// 合批后的增量文本交付 (EVT_DELTA_TEXT_BATCH)
    void emitDeltaText(std::string_view kind, std::string_view text) override;

    /// 非合批的增量交付 (EVT_DELTA)
    void emitDeltaEvent(const agent::WireDelta& delta) override;

    /// 发事件到 C 回调 (异常不外泄)
    void emitEvent(AgentxxFFIEventType type, std::string json);

    static std::string dump(const utilxx_base::Json& j);

    /// 事件回调 (值拷贝, 保持有效)
    AgentxxFFICallbacks callbacks_;

    /// 已下发 EVT_HOST_TOOL_CALL 但嵌入方程序尚未应答的调用 id (用于停止时上报取消)
    std::set<int64_t> pendingHostToolCalls_;
};

} // namespace ffi
} // namespace agentxx
