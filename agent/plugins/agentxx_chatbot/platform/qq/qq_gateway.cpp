#include "qq_gateway.h"

#include "qq_api.h"
#include "utilxx/ws_client.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <chrono>

namespace agentxx {
namespace chatbot {

namespace {

constexpr const char* kLogTag = "chatbot";

/// 只订阅群与单聊消息 (`GROUP_AND_C2C_EVENT = 1 << 25`)
///
/// 频道相关位不订阅: 没权限时连接会被直接关闭 (设计文档 §3.3)。
constexpr int64_t kIntentsGroupAndC2c = 1LL << 25;

int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

/// 退避时长 (指数; 5 → 30 秒封顶)
int64_t backoffMs(int32_t attempt) {
    const int64_t base = 5000;
    const int64_t cap  = 30000;
    int64_t       delay = base;
    for (int32_t i = 1; i < attempt && delay < cap; ++i) {
        delay *= 2;
    }
    return std::min(delay, cap);
}

} // namespace

QqGateway::QqGateway(asio::any_io_executor ex, QqApi& api) :
    ex_(std::move(ex)),
    api_(api) {}

QqGateway::~QqGateway() {
    stop();
}

void QqGateway::setState(PlatformState state, std::string reason) {
    if (hooks_.onState) {
        hooks_.onState(state, reason);
    }
}

void QqGateway::start(const AccountConfig& cfg, QqGatewayHooks hooks) {
    config_  = cfg;
    hooks_   = std::move(hooks);
    stopped_ = false;
    reconnectCount_ = 0;
    sequence_       = 0;
    sessionId_.clear();
    setState(PlatformState::Connecting, "start");
    connect();
}

void QqGateway::stop() {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    ++generation_;
    stopHeartbeat();
    if (reconnectTimer_) {
        reconnectTimer_->cancel();
        // 定时器带着 io 的 executor: 插件 io 线程停后不能再留着它
        reconnectTimer_.reset();
    }
    closeSocket();
    online_ = false;
}

void QqGateway::closeSocket() {
    if (!ws_) {
        return;
    }
    // 先 abort 关掉底层 socket, 挂起的收发会以错误收尾; 对象本身的释放交给强引用
    // (发送/接收协程各持一份), 所以这里放开引用不会让协程读到已释放的连接
    ws_->abort();
    ws_.reset();
}

void QqGateway::sendFrame(const utilxx_base::Json& frame) {
    if (!ws_ || !ws_->isOpen()) {
        return;
    }
    // 发送期间持有连接: 重连/停止会换掉 ws_, 但这次发送仍然走完 (以错误或成功收尾)
    auto       client     = ws_;
    const auto text       = frame.dump();
    auto       self       = this;
    const auto generation = generation_;
    asio::co_spawn(
        ex_,
        [self, client, text, generation]() -> asio::awaitable<void> {
            if (self->generation_ != generation || self->ws_ != client) {
                co_return;
            }
            auto result = co_await client->sendText(text);
            if (!result.has_value()) {
                XX_LOGW("[{}] 网关发送失败: {}", kLogTag, result.error());
            }
        },
        asio::detached
    );
}

void QqGateway::connect() {
    if (stopped_) {
        return;
    }
    ++generation_;
    const auto generation = generation_;
    auto       self       = this;
    asio::co_spawn(
        ex_,
        [self, generation]() -> asio::awaitable<void> {
            if (self->stopped_ || self->generation_ != generation) {
                co_return;
            }
            // 网关地址每次连接都重新取 (平台会换节点)
            auto response = co_await self->api_.gatewayUrl();
            if (!response.ok()) {
                self->lastError_ = "取网关地址失败 (status " + std::to_string(response.status)
                                   + " " + response.error + ")";
                self->scheduleReconnect(self->lastError_, -1);
                co_return;
            }
            auto json = response.json();
            if (!json.has_value() || !json->is_object()) {
                self->lastError_ = "网关地址响应不是 JSON";
                self->scheduleReconnect(self->lastError_, -1);
                co_return;
            }
            std::string url = json->value("url", std::string{});
            if (url.empty()) {
                self->lastError_ = "网关地址为空 (账号未开通或 token 无权限)";
                self->scheduleReconnect(self->lastError_, -1);
                co_return;
            }
            XX_LOGI("[{}] 连接网关: {}", kLogTag, url);

            utilxx::WsClientConfig wsConfig;
            wsConfig.connectTimeout = std::chrono::seconds{20};
            // 心跳间隔可能到 41 秒, 接收超时要比它宽松 (心跳 ACK 之外的静默是正常的)
            wsConfig.recvTimeout    = std::chrono::seconds{90};
            wsConfig.sendTimeout    = std::chrono::seconds{30};
            wsConfig.maxMessageSize = 8 * 1024 * 1024;

            auto client = co_await utilxx::wsConnect(self->ex_, url, {}, wsConfig);
            if (!client.has_value()) {
                self->lastError_ = "连接网关失败: " + client.error();
                self->scheduleReconnect(self->lastError_, -1);
                co_return;
            }
            if (self->stopped_ || self->generation_ != generation) {
                if (client.has_value()) {
                    client->get()->abort();
                }
                co_return;
            }
            self->ws_     = std::shared_ptr<utilxx::WsClient>(std::move(*client));
            self->online_ = true;
            self->readLoop();
        },
        asio::detached
    );
}

void QqGateway::readLoop() {
    if (!ws_ || stopped_) {
        return;
    }
    const auto generation = generation_;
    // 与发送同理: 手里持一份强引用, 重连换掉 ws_ 后本循环也能安全收尾
    auto client = ws_;
    auto self   = this;
    asio::co_spawn(
        ex_,
        [self, client, generation]() -> asio::awaitable<void> {
            while (!self->stopped_ && self->generation_ == generation && self->ws_ == client) {
                auto message = co_await client->recv();
                if (self->generation_ != generation || self->stopped_) {
                    co_return;
                }
                if (!message.has_value()) {
                    const auto reason = message.error();
                    if (self->stopped_) {
                        co_return;
                    }
                    self->lastError_ = "网关接收失败: " + reason;
                    self->scheduleReconnect(self->lastError_, -1);
                    co_return;
                }
                if (message->type == utilxx::WsMessage::Type::Close) {
                    self->lastError_ = "网关对端关闭连接 (code " + std::to_string(message->closeCode)
                                       + " " + message->closeReason + ")";
                    self->scheduleReconnect(self->lastError_, -1);
                    co_return;
                }
                self->onFrame(message->payload);
            }
        },
        asio::detached
    );
}

void QqGateway::onFrame(std::string_view payload) {
    utilxx_base::Json frame;
    try {
        frame = utilxx_base::Json::parse(payload);
    } catch (const std::exception& e) {
        XX_LOGW("[{}] 网关帧不是合法 JSON, 已忽略: {}", kLogTag, e.what());
        return;
    }
    if (!frame.is_object()) {
        return;
    }
    const int op = frame.value("op", -1);

    // 序号: Resume 时用最近一次收到的 s
    if (frame.contains("s") && frame["s"].is_number_integer()) {
        const auto s = frame["s"].get<long long>();
        if (s > sequence_) {
            sequence_ = s;
        }
    }

    switch (op) {
        case 10: { // Hello
            const auto& data = frame.contains("d") ? frame["d"] : utilxx_base::Json::object();
            const auto interval = data.value("heartbeat_interval", int64_t{30000});
            heartbeatMs_ = std::max<int64_t>(1000, interval);
            XX_LOGI("[{}] 网关 Hello (心跳间隔 {} ms)", kLogTag, heartbeatMs_);
            startHeartbeat(heartbeatMs_);

            // 有 session_id 且序号有效时优先 Resume (不消耗 Identify 配额)
            if (!sessionId_.empty() && sequence_ > 0) {
                utilxx_base::Json resume = utilxx_base::Json::object();
                resume["op"] = 6;
                utilxx_base::Json d = utilxx_base::Json::object();
                d["token"]      = "QQBot " + tokenOfAccount();
                d["session_id"] = sessionId_;
                d["seq"]        = sequence_;
                resume["d"]     = std::move(d);
                sendFrame(resume);
                setState(PlatformState::Reconnecting, "resume");
                break;
            }
            // Identify
            {
                utilxx_base::Json identify = utilxx_base::Json::object();
                identify["op"] = 2;
                utilxx_base::Json d = utilxx_base::Json::object();
                d["token"]   = "QQBot " + tokenOfAccount();
                d["intents"] = kIntentsGroupAndC2c;
                utilxx_base::Json shard = utilxx_base::Json::array();
                shard.push_back(config_.shard.empty() ? 0 : config_.shard[0]);
                shard.push_back(config_.shard.size() < 2 ? 1 : config_.shard[1]);
                d["shard"] = std::move(shard);
                d["properties"] = utilxx_base::Json{
                    {"$os",      "linux"    },
                    {"$browser", "agentxx"  },
                    {"$device",  "agentxx"  },
                };
                identify["d"] = std::move(d);
                sendFrame(identify);
                identifying_ = true;
            }
            break;
        }
        case 11: // Heartbeat ACK
            missedAcks_ = 0;
            break;
        case 1: // 服务端要求心跳
            sendHeartbeat();
            break;
        case 7: // Reconnect
            lastError_ = "网关要求重连 (op 7)";
            scheduleReconnect(lastError_, 1000);
            break;
        case 9: // Invalid Session: 必须重新 Identify
            lastError_ = "网关会话失效 (op 9), 重新 Identify";
            sessionId_.clear();
            sequence_ = 0;
            scheduleReconnect(lastError_, 1000);
            break;
        case 0: { // Dispatch
            const auto eventName = frame.value("t", std::string{});
            const auto& data     = frame.contains("d") ? frame["d"] : utilxx_base::Json::object();
            if (eventName == "READY") {
                sessionId_ = data.value("session_id", std::string{});
                identifying_ = false;
                reconnectCount_ = 0;
                setState(PlatformState::Online, "ready");
                if (hooks_.onReady) {
                    hooks_.onReady(sessionId_, sequence_);
                }
                XX_LOGI("[{}] 网关已就绪 (session '{}')", kLogTag, sessionId_);
                break;
            }
            if (eventName == "RESUMED") {
                // 断线恢复成功: 会话接续, 状态要回到 online —— 否则一直停在
                // reconnecting, 上层的"在线才发"判断会把后面的出站全部挡掉
                identifying_ = false;
                reconnectCount_ = 0;
                setState(PlatformState::Online, "resumed");
                if (hooks_.onReady) {
                    hooks_.onReady(sessionId_, sequence_);
                }
                XX_LOGI("[{}] 网关会话已恢复 (session '{}', seq {})", kLogTag, sessionId_, sequence_);
                break;
            }
            if (hooks_.onDispatch) {
                hooks_.onDispatch(eventName, data);
            }
            break;
        }
        case 12: // webhook 模式专用, 忽略
        case 13:
            break;
        default:
            XX_LOGD("[{}] 未知网关 op {}", kLogTag, op);
            break;
    }
}

std::string QqGateway::tokenOfAccount() const {
    // 网关帧里的 token 就是 access_token (带 `QQBot ` 前缀); 由 QqApi 统一缓存,
    // 这里同步取一次缓存值 (不会触发网络请求)
    return api_.cachedToken();
}

void QqGateway::startHeartbeat(int64_t intervalMs) {
    stopHeartbeat();
    // 0.8 倍间隔发送 (设计文档 §12.1)
    const int64_t period = std::max<int64_t>(1000, intervalMs * 8 / 10);
    heartbeatTimer_      = std::make_shared<asio::steady_timer>(ex_);
    heartbeatTimer_->expires_after(std::chrono::milliseconds(period));
    const auto generation = generation_;
    auto       self       = this;
    heartbeatTimer_->async_wait([self, period, generation](const auto& ec) {
        if (ec || self->stopped_ || self->generation_ != generation) {
            return;
        }
        self->sendHeartbeat();
        // 连续 2 个周期没收到 ACK: 判定连接失效并重连 (不等 TCP 超时)
        if (self->missedAcks_ >= 2) {
            self->lastError_ = "心跳连续 2 个周期没有 ACK, 判定连接失效";
            self->scheduleReconnect(self->lastError_, 0);
            return;
        }
        self->startHeartbeat(period * 10 / 8);
    });
}

void QqGateway::stopHeartbeat() {
    if (heartbeatTimer_) {
        heartbeatTimer_->cancel();
        heartbeatTimer_.reset();
    }
}

void QqGateway::sendHeartbeat() {
    ++missedAcks_;
    sendFrame(heartbeatFrame());
    lastHeartbeatMs_ = steadyNowMs();
}

utilxx_base::Json QqGateway::heartbeatFrame() const {
    utilxx_base::Json frame = utilxx_base::Json::object();
    frame["op"] = 1;
    if (sequence_ > 0) {
        frame["d"] = sequence_;
    } else {
        frame["d"] = nullptr; // 首帧 null
    }
    return frame;
}

void QqGateway::scheduleReconnect(std::string reason, int64_t delayMs) {
    if (stopped_) {
        return;
    }
    online_ = false;
    ++generation_;
    stopHeartbeat();
    closeSocket();
    setState(PlatformState::Reconnecting, reason);
    // 次数统一在这里递增: 之前只有"连接失败"那条路径在调 backoffMs(++reconnectCount_),
    // op 7 / op 9 直接给常量延迟, 计数就一直是 0 (日志看着像没重连过)。
    // 调用方传 `delayMs < 0` 表示"按当前次数算指数退避"。
    ++reconnectCount_;
    if (delayMs < 0) {
        delayMs = backoffMs(reconnectCount_);
    }
    XX_LOGW(
        "[{}] 网关将重连 (第 {} 次, {} ms 后): {}",
        kLogTag,
        reconnectCount_,
        delayMs,
        reason
    );
    if (!reconnectTimer_) {
        reconnectTimer_ = std::make_shared<asio::steady_timer>(ex_);
    }
    reconnectTimer_->expires_after(std::chrono::milliseconds(std::max<int64_t>(0, delayMs)));
    auto self = this;
    reconnectTimer_->async_wait([self](const auto& ec) {
        if (ec || self->stopped_) {
            return;
        }
        self->connect();
    });
}

} // namespace chatbot
} // namespace agentxx
