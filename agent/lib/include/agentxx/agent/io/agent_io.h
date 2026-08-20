#pragma once

#include "agentxx/agent/conversation_types.h"
#include "agentxx/agent/io/agent_io_transport.h"
#include "agentxx/agent/io/client_event_sink.h"
#include "agentxx/util/log.h"
#include "asio/awaitable.hpp"
#include "asio/this_coro.hpp"
#include "fmt/format.h"
#include "neograph/json.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace agentxx::middleware {
class EventBus;
} // namespace agentxx::middleware

namespace agentxx::event {
class EventBus;
} // namespace agentxx::event

namespace agentxx {
namespace agent {

/// Agent 端点基类: 定义 client/server 之间的操作契约
///
/// 拓扑 (强制 transport: 两端点之间必须经 transport 连接, 进程内用 Channel):
///
///   BaseAgent ──进程内直调──▶ server 端点 (SessionServerAgentIO)
///            sendToPeer(事件)/        │ transport (Channel | WebSocket)
///            getInput/handleInterrupt ▼
///                                client 端点 (TUIClientAgentIO / StdIOClientAgentIO)
///
/// 两端点之间为对称的消息传递模型:
/// - 发送: 本端调用 sendToPeer() 经 transport 发送 WireMessage 到对端
/// - 接收: runTransportLoop() 收对端消息 → onPeerMessage() 分发到 onXXX 被动回调
///
/// 子类:
/// - TUIClientAgentIO / StdIOClientAgentIO: 客户端渲染端点
/// - SessionServerAgentIO: 服务端会话驱动端点 (被 BaseAgent 驱动)
///
/// 下文按接口角色标注:
/// - [双向]   client/server 端点都可用/需实现
/// - [client] 仅客户端端点应当实现或使用
/// - [server] 仅服务端端点 (被 BaseAgent 驱动的一侧) 应当实现或使用
class AgentIOBase {
public:

    virtual ~AgentIOBase();

    // -----------------------------------------------------------------------
    // 主动发送 [双向] (唯一出站口)
    // -----------------------------------------------------------------------

    /// 向对端发送消息 (经 transport)
    /// - 必须先 setTransport; 未设置时记录错误日志并丢弃
    /// - virtual: 服务端点 (SessionServerAgentIO) 覆写以对 Delta 追加缓冲等本地处理
    virtual void sendToPeer(WireMessage msg);

    /// 请求取消指定会话当前轮次 [client]
    virtual void requestCancel(std::string sessionId);

    /// 请求切换指定会话的模型 [client]
    virtual void requestSelectModel(std::string sessionId, std::string model);

    /// 请求拉取会话启动信息 (MCP/Skill/Memory) [client]
    /// - 客户端启动后调用一次; 服务端以 WireAppendComponentInfo 回应, 由 onPeerMessage 处理
    virtual void requestAppendComponentInfo(std::string sessionId);

    /// 发送用户输入到对端 [client]
    /// - 是否首轮由服务端自行管理; 模型切换经 requestSelectModel
    /// - 发送后通知事件接收器 (ClientEventSink::onUserInput)
    virtual void sendUserInput(std::string sessionId, std::string text);

    /// 服务端就绪通知 [client] (默认空实现, 客户端端点按需覆写):
    /// - 本地模式: agent-io (SessionServerAgentIO) 的会话驱动循环启动前由
    ///   mode_runners 调用, 表示 init() 等启动工作完成、可以开始消费用户输入
    /// - 远程模式: 连接握手完成后由连接协程调用
    /// 客户端 (TUI) 据此解除"启动中"输入限制并刷新待发送队列
    /// - 基类默认实现通知事件接收器 (ClientEventSink::onReady); 覆写方应调用
    ///   基类实现 (或自行 emitEventSink)
    virtual void onServerReady();

    /// 服务端启动进度通知 [client] (默认空实现, 客户端端点按需覆写):
    /// - 本地模式: agent-io 的 init() 各启动阶段 (加载 MCP/Skill/Memory/
    ///   RAG/CodeGraph 等) 经 AgentContext::initNotifier → 本接口逐步上报,
    ///   客户端 (TUI) 据此在"启动中"banner 中展示当前正在执行的操作
    /// - 由 agent 线程同步调用, 实现须自行加锁同步
    virtual void onServerProgress(std::string_view /*step*/) {}

    // -----------------------------------------------------------------------
    // 拉取接口 [双向] (调用方: server 侧由 BaseAgent 驱动循环调用,
    //                   client 侧由本端输入循环/onPeerMessage 调用)
    // -----------------------------------------------------------------------

    /// 获取用户输入 (协程阻塞直到有输入; nullopt 表示输入结束)
    virtual asio::awaitable<std::optional<std::string>> getInput() = 0;

    /// 统一的 HIL 处理: 用于权限询问、中断输入收集等所有用户交互场景
    /// - server 侧: 经会话总线 (registerOnBus) 被 BaseAgent 的中断流程调用
    /// - client 侧: 收到对端 WireInterruptRequest 后由 onPeerMessage 调用
    virtual asio::awaitable<neograph::json> handleInterrupt(
        std::string_view sessionId,
        std::string_view interruptNode,
        std::string_view interruptValue,
        std::string_view interruptArgJson
    ) = 0;

    // -----------------------------------------------------------------------
    // Transport 管理 [双向]
    // -----------------------------------------------------------------------

    /// 设置传输层 (构造后注入; 端点间通信前必须设置)
    void setTransport(std::shared_ptr<AgentIOTransportBase> transport);

    /// 获取传输层 (可能为 nullptr)
    std::shared_ptr<AgentIOTransportBase> transport() const noexcept;

    /// 设置事件接收器 (client 插件系统等扩展点的被动回调入口)
    /// - 由模式启动时 (mode_runners) 注入; 端点是 sink 的持有方 (shared_ptr)
    void setEventSink(std::shared_ptr<ClientEventSink> sink);

    /// 获取事件接收器 (可能为 nullptr)
    std::shared_ptr<ClientEventSink> eventSink() const noexcept {
        return eventSink_;
    }

    /// 运行接收循环: 从 transport 读消息并 dispatch 到 onPeerMessage
    /// - 应在协程中 co_await 调用; transport 关闭时自然退出
    /// - 未设置 transport 时立即返回
    asio::awaitable<void> runTransportLoop();

    // -----------------------------------------------------------------------
    // 事件总线 [server] (仅被 BaseAgent 驱动的端点使用)
    // -----------------------------------------------------------------------

    /// 在会话总线上注册本 IO 的事件处理器 (interrupt / permission)
    /// - 由 BaseAgent::runTurnAsync 调用
    /// - 重复调用会先移除上一次注册的处理器, 避免 handler 累积、泄漏与悬空 this
    virtual void registerOnBus(std::shared_ptr<agentxx::event::EventBus> sessionBus);

protected:

    // -----------------------------------------------------------------------
    // 事件接收器辅助 [client] (子类在关键路径调用; 空 sink 时为 no-op)
    // -----------------------------------------------------------------------

    /// 转发事件到接收器 (sink 为空时跳过; 调用方线程即回调线程 —— 须为
    /// client io 线程, 符合 ClientEventSink 线程约定)
    template<typename Fn>
    void emitEventSink(Fn fn) const {
        if (eventSink_) {
            fn(*eventSink_);
        }
    }

    // -----------------------------------------------------------------------
    // 被动接收回调 [client] (仅由 onPeerMessage 分发调用, 外部不应直接调用)
    // - server 端点不会从 client 收到这些消息, 以空实现满足纯虚契约即可
    // -----------------------------------------------------------------------

    /// 增量事件推送 (流式 token、tool 生命周期、轮次边界)
    virtual void onDelta(const Delta& delta) = 0;

    /// 全量/部分同步 (从 viewMessages 校准 client 状态)
    virtual void onSync(const SyncPayload& payload) = 0;

    /// 轮次结束通知
    virtual void onTurnResult(const WireTurnResult& /*result*/) {}

    /// 上下文统计更新
    virtual void onContextStats(const WireContextStats& /*stats*/) {}

    /// 处理从 transport 收到的对端消息 [双向] (子类覆写以分发到具体处理器)
    /// - 默认实现: 按消息类型分发到 onDelta/onSync/onTurnResult/onContextStats
    /// - 客户端子类额外处理 InterruptRequest; 服务端子类额外处理 UserInput/Cancel 等
    virtual void onPeerMessage(WireMessage msg);

    /// 从已注册的总线上移除本 IO 的处理器 (若总线仍存活)
    void unregisterFromBus();

    std::shared_ptr<AgentIOTransportBase>   transport_;
    std::shared_ptr<ClientEventSink>        eventSink_;
    std::weak_ptr<agentxx::event::EventBus> registeredBus_;
    size_t                                  interruptServerId_  = 0;
    size_t                                  permissionServerId_ = 0;
};

} // namespace agent
} // namespace agentxx
