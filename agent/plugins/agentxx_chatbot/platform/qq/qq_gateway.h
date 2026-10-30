#pragma once

/// QQ 网关 (WebSocket): 连接 / Hello / Identify / 心跳 / Resume / 重连退避
///
/// 协议事实见 `resource/history/chatbot/plan.md` §3.2:
/// - 连接后第一条是 `op 10 Hello` (`{heartbeat_interval}` 毫秒);
/// - 发 `op 2 Identify` (`{token: "QQBot <access_token>", intents, shard, properties}`);
/// - 收 `op 0 Dispatch` (`t` = 事件名, `s` = 递增序号, `d` = 事件体);
/// - 心跳 `op 1` (`{d: 最近一次的 s}`) 按 0.8 × interval 发, 连续 2 个周期没
///   `op 11 ACK` 判定连接失效并重连;
/// - `op 6 Resume` 补发遗漏事件 (优先于 Identify, 不消耗 Identify 配额);
/// - `op 7 Reconnect` / `op 9 Invalid Session` 与错误码 4009/4006/4007 的处置见 §12.1。
///
/// 线程: 全部在插件 io 线程上跑; `stop()` 只能在该线程 (或该 io_context 已停) 调用。
#include "../../chatbot_config.h"
#include "../../chatbot_types.h"

#include "asio/any_io_executor.hpp"
#include "asio/steady_timer.hpp"
#include "utilxx_base/json.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace asio = ::boost::asio;

namespace utilxx {
class WsClient;
}

namespace agentxx {
namespace chatbot {

class QqApi;

/// 网关事件回调
struct QqGatewayHooks {
    /// 收到一条 Dispatch (`t` 事件名, `d` 事件体)
    std::function<void(std::string_view eventName, const utilxx_base::Json& data)> onDispatch;
    /// 连接状态变化
    std::function<void(PlatformState, std::string)> onState;
    /// 收到 Ready (`d.session_id`, 之后断线可 Resume)
    std::function<void(std::string_view sessionId, int64_t sequence)> onReady;
};

/// 网关连接
class QqGateway {
public:

    QqGateway(asio::any_io_executor ex, QqApi& api);
    ~QqGateway();

    /// 开始连接 (异步; 结果经 hooks 体现)
    void start(const AccountConfig& cfg, QqGatewayHooks hooks);

    /// 断开并释放自管资源 (幂等)
    void stop();

    /// 是否在线
    bool online() const noexcept {
        return online_;
    }

    /// 最近一次错误 (诊断)
    const std::string& lastError() const noexcept {
        return lastError_;
    }

    /// 已重连次数
    int32_t reconnectCount() const noexcept {
        return reconnectCount_;
    }

    /// 当前 session 的序号 (Resume 用; 诊断)
    int64_t sequence() const noexcept {
        return sequence_;
    }

private:

    /// 建立一次连接 (之后由 `readLoop` 推进)
    void connect();

    void readLoop();

    void onFrame(std::string_view payload);

    void sendFrame(const utilxx_base::Json& frame);

    void startHeartbeat(int64_t intervalMs);
    void stopHeartbeat();
    void sendHeartbeat();

    /// 心跳帧 (`op 1`, `d` = 最近一次收到的 s; 首帧为 null)
    utilxx_base::Json heartbeatFrame() const;

    /// 缓存里的 access token (供 Identify/Resume 组帧; 不触发网络请求)
    std::string tokenOfAccount() const;

    void scheduleReconnect(std::string reason, int64_t delayMs);

    void closeSocket();

    void setState(PlatformState state, std::string reason);

    asio::any_io_executor          ex_;
    QqApi&                         api_;
    AccountConfig                  config_;
    QqGatewayHooks                 hooks_;
    std::shared_ptr<utilxx::WsClient> ws_;
    std::shared_ptr<asio::steady_timer> heartbeatTimer_;
    std::shared_ptr<asio::steady_timer> reconnectTimer_;
    std::string                    sessionId_;
    int64_t                        sequence_      = 0;
    int64_t                        heartbeatMs_   = 30000;
    int32_t                        missedAcks_    = 0;
    int64_t                        lastHeartbeatMs_ = 0;
    int32_t                        reconnectCount_ = 0;
    bool                           online_        = false;
    bool                           stopped_       = false;
    bool                           identifying_   = false;
    std::string                    lastError_;
    uint64_t                       generation_    = 0; ///< 每次连接递增, 旧协程据此退出
};

} // namespace chatbot
} // namespace agentxx
