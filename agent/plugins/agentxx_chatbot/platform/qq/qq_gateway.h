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
/// 连接由主程序的网络表持有 (见 `chatbot_net.h`), 本类只做协议状态机:
/// - 建连: `ws.connect` (返回时还在握手) → `ws.open` 事件到达才算连上;
/// - 收帧: `ws.message` 事件; 断开: `ws.close` 事件;
/// - 发帧: `ws.send` (握手未完成时发送会失败, 所以只看 `connected_`)。
///
/// 线程: 全部在插件 io 线程上跑 (网络事件先由主程序 io 线程投递过来);
/// `stop()` 只能在该线程 (或该 io_context 已停) 调用。
#include "../../chatbot_config.h"
#include "../../chatbot_types.h"

#include "asio/any_io_executor.hpp"
#include "asio/steady_timer.hpp"
#include "pluginxx/api/abi.h"
#include "utilxx_base/json.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace asio = ::boost::asio;

namespace agentxx {
namespace chatbot {

class QqApi;
struct ChatbotNet;

/// 网关事件回调
struct QqGatewayHooks {
    /// 收到一条 Dispatch (`t` 事件名, `d` 事件体)
    std::function<void(std::string_view eventName, const utilxx_base::Json& data)> onDispatch;
    /// 连接状态变化
    std::function<void(PlatformState, std::string)> onState;
    /// 收到 Ready (`d.session_id`, 之后断线可 Resume)
    ///
    /// `user` 是事件里的 `d.user` (机器人自己: `id` / `union_openid` / `username`),
    /// 平台层拿它判断全量消息里“@ 的是不是我”。Resume 成功时没有这个字段, 传空对象。
    std::function<void(std::string_view sessionId, int64_t sequence, const utilxx_base::Json& user)>
        onReady;
};

/// 网关连接
class QqGateway {
public:

    QqGateway(asio::any_io_executor ex, QqApi& api, const ChatbotNet* net);
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

    /// 订阅本插件的网络事件频道 (幂等)
    void subscribeNetEvents();

    /// 网络事件回调 (主程序 io 线程; 只做拷贝 + 投回插件 io 线程)
    static void onNetEvent(const PluginxxStringView* payload, void* ud);

    /// 网络事件分发 (插件 io 线程)
    void handleNetEvent(const std::string& payload);

    /// 建立一次连接 (之后靠事件推进)
    void connect();

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

    /// 关掉当前连接 (主程序按 id 找, 找不到就算了)
    void closeSocket();

    void setState(PlatformState state, std::string reason);

    asio::any_io_executor               ex_;
    QqApi&                              api_;
    const ChatbotNet*                   net_ = nullptr;
    AccountConfig                       config_;
    QqGatewayHooks                      hooks_;
    std::shared_ptr<asio::steady_timer> heartbeatTimer_;
    std::shared_ptr<asio::steady_timer> reconnectTimer_;
    /// 当前连接在 `<应用名>.net` 表里的 id (每次连接换一个; 旧事件据此丢掉)
    std::string                         connId_;
    /// 握手完成 (`ws.open` 之后才能发帧)
    bool                                connected_ = false;
    /// 网络事件订阅句柄 (`stop` 里退订)
    PluginxxSubscription*               netSub_ = nullptr;
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
    uint64_t                       generation_    = 0; ///< 每次连接递增, 旧流程据此退出
};

} // namespace chatbot
} // namespace agentxx
