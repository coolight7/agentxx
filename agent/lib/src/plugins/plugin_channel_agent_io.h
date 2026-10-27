#pragma once

#include "agentxx/agent/io/embedded_client_agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/plugin/api/plugin_api.h"
#include "asio/any_io_executor.hpp"
#include "utilxx_base/json.h"
#include <atomic>
#include <functional>
#include <string>
#include <string_view>

namespace agentxx {
namespace plugin {

/// 插件通道的 client 端点: 把服务端产出的事件翻成通道事件回调
///
/// 与 FFI 的 `FfiClientAgentIO` 同一层 (都是 [EmbeddedClientAgentIO] 的子类):
/// 中断应答、增量合批、线程判定来自基类, 本类只做"wire 消息 → 通道事件"的映射,
/// 事件的落点由管理器给的 [EventFn] 决定 (它负责填 `AgentxxPluginChannelEvent`
/// 并调用插件的回调)。
///
/// 线程约定: 与基类一致, 全部状态只在 client io 线程读写。插件通道的 client 侧就用
/// 主程序 io 执行器 (与根的 agent 侧同一个), 因此回调也在主程序 io 线程上执行。
class PluginChannelAgentIO : public agent::EmbeddedClientAgentIO,
                             public std::enable_shared_from_this<PluginChannelAgentIO> {
public:

    /// 事件交付回调 (由管理器提供)
    /// - `text` 与 `payload` 只在调用期间有效 (`payload` 为 nullptr 表示空载荷)
    /// - `kind` 取 `AGENTXX_PLUGIN_CHANNEL_EVT_*`
    using EventFn = std::function<void(
        int32_t                  kind,
        int64_t                  interruptId,
        std::string_view         text,
        const utilxx_base::Json* payload
    )>;

    PluginChannelAgentIO(asio::any_io_executor ex, int64_t channelId);

    int64_t channelId() const noexcept {
        return channelId_;
    }

    void setEventFn(EventFn fn) {
        eventFn_ = std::move(fn);
    }

    /// 停止再回调插件 (禁用/卸载/销毁前调用; 之后的收尾事件一律丢弃)
    void clearEventFn() {
        eventFn_ = nullptr;
    }

    bool hasEventFn() const {
        return static_cast<bool>(eventFn_);
    }

    // -----------------------------------------------------------------------
    // 管理器发起的事件 (均在 client io 线程调用)
    // -----------------------------------------------------------------------

    /// 通道可用: 服务端点已就绪, 可以投递输入
    void notifyReady(std::string_view rootId, std::string_view model);

    /// 运行时错误 (init 失败 / 根不可用等)
    void notifyError(std::string code, std::string message);

    /// 通道已关闭 (本端点的最后一条事件)
    void notifyClosed(std::string_view reason);

    // -----------------------------------------------------------------------
    // 中断应答 (服务端发来的询问)
    // -----------------------------------------------------------------------

    /// 当前挂起的询问 id (0 = 无; 任意线程可读)
    int64_t pendingInterruptId() const;

    /// 提交应答 (须在 client io 线程; id 无效返回 false)
    bool submitInterrupt(int64_t interruptId, utilxx_base::Json values);

    asio::any_io_executor executor() const noexcept {
        return ex_;
    }

protected:

    // ---- 基类被动回调 (均已在 client io 线程) ----

    /// 合批后的增量文本 (本端点默认不合批, 但基类仍可能在关闭时冲刷)
    void emitDeltaText(std::string_view kind, std::string_view text) override;

    /// 逐条增量 (正文/思考/工具生命周期/生成阶段/提示)
    void emitDeltaEvent(const agent::WireDelta& delta) override;

    void onTurnResult(const agent::WireTurnResult& result) override;

    void onContextStats(const agent::WireContextStats& stats) override;

    void onServerReady() override;

    void onPeerMessage(agent::WireMessage msg) override;

private:

    /// 交付一条事件 (无回调时丢弃)
    void
        emit(int32_t kind, int64_t interruptId, std::string_view text, const utilxx_base::Json* payload);

    int64_t channelId_ = 0;
    EventFn eventFn_;

    /// 当前挂起的询问 id (0 = 无; 原子量供任意线程读取)
    std::atomic<int64_t> pendingInterruptId_{0};

    // ---- 本轮正文累积 (TURN_END 的 text 取最后一段非空正文) ----
    std::string turnText_;    ///< 本轮全部正文 (拼接后作为 payload.fullText)
    std::string segmentText_; ///< 当前连续正文段 (工具调用会打断)
    std::string lastSegment_; ///< 最后一个已结束的非空正文段
    double      lastTps_ = 0.0;
};

} // namespace plugin
} // namespace agentxx
