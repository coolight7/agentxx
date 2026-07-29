#pragma once

#include "agentxx/agent/agent_io_transport.h"
#include "agentxx/agent/conversation_types.h"
#include "agentxx/util/log.h"
#include "asio/awaitable.hpp"
#include "asio/this_coro.hpp"
#include "fmt/format.h"
#include "neograph/json.h"
#include <memory>
#include <optional>
#include <string>

namespace agentxx::middleware {
class EventBus;
} // namespace agentxx::middleware

namespace agentxx {
namespace agent {

/// Agent 端点基类: 定义 client/server 之间的操作契约
///
/// 数据流: Client UI → AgentIOBase → AgentIOTransportBase → AgentIOBase → BaseAgent
///
/// 每个端点持有一个 transport (组合关系), 通过 transport 与对端通信:
/// - 本端调用 sendToPeer() 发送命令/输入到对端
/// - runTransportLoop() 接收对端消息并 dispatch 到 onPeerMessage()
///
/// 子类:
/// - AgentTUI / AgentStdIO: 客户端渲染端点
/// - SessionController: 服务端会话驱动端点
class AgentIOBase {
public:

    virtual ~AgentIOBase();

    // -----------------------------------------------------------------------
    // 对端推给我的事件 (我被动接收, 由 onPeerMessage 分发)
    // -----------------------------------------------------------------------

    /// 增量事件推送 (流式 token、tool 生命周期、轮次边界)
    virtual void onDelta(const Delta& delta) = 0;

    /// 全量/部分同步 (从 fullHistory 校准 client 状态)
    virtual void onSync(const SyncPayload& payload) = 0;

    /// 轮次结束通知
    virtual void onTurnResult(const WireTurnResult& /*result*/) {}

    /// 上下文统计更新
    virtual void onContextStats(const WireContextStats& /*stats*/) {}

    // -----------------------------------------------------------------------
    // 对端从我这拉取的 (我主动提供)
    // -----------------------------------------------------------------------

    virtual asio::awaitable<std::optional<std::string>> getInput() = 0;

    /// 统一的 HIL 处理: 用于权限询问、中断输入收集等所有用户交互场景
    virtual asio::awaitable<neograph::json> handleInterrupt(
        std::string_view threadId,
        std::string_view interruptNode,
        std::string_view interruptValue,
        std::string_view interruptArgJson
    ) = 0;

    // -----------------------------------------------------------------------
    // 我主动发给对端的命令 (经 transport 发送)
    // -----------------------------------------------------------------------

    /// 请求取消指定会话当前轮次
    virtual void requestCancel(std::string_view threadId);

    /// 请求切换指定会话的模型
    virtual void requestSelectModel(std::string_view threadId, std::string_view model);

    /// 请求拉取会话启动信息 (MCP/Skill/Memory)
    /// - 客户端启动后调用一次; 服务端以 WireAppendComponentInfo 回应, 由 onPeerMessage 处理
    virtual void requestAppendComponentInfo(std::string_view threadId);

    /// 发送用户输入到对端
    /// - 是否首轮由服务端自行管理; 模型切换经 requestSelectModel
    virtual void sendUserInput(std::string_view threadId, std::string_view text);

    // -----------------------------------------------------------------------
    // Transport 管理
    // -----------------------------------------------------------------------

    /// 设置传输层 (构造后注入; 未设置时为本地直连模式)
    void setTransport(std::shared_ptr<AgentIOTransportBase> transport);

    /// 获取传输层 (可能为 nullptr)
    std::shared_ptr<AgentIOTransportBase> transport() const noexcept;

    /// 运行接收循环: 从 transport 读消息并 dispatch 到 onPeerMessage
    /// - 应在协程中 co_await 调用; transport 关闭时自然退出
    /// - 未设置 transport 时立即返回 (本地直连模式不需要)
    asio::awaitable<void> runTransportLoop();

    // -----------------------------------------------------------------------
    // 事件总线
    // -----------------------------------------------------------------------

    /// 在会话总线上注册本 IO 的事件处理器 (interrupt / permission)
    /// - 重复调用会先移除上一次注册的处理器, 避免 handler 累积、泄漏与悬空 this
    virtual void registerOnBus(std::shared_ptr<agentxx::middleware::EventBus> sessionBus);

    /// 向对端发送消息 (经 transport; transport 为空时静默丢弃)
    void sendToPeer(WireMessage msg);

protected:

    /// 处理从 transport 收到的对端消息 (子类覆写以分发到具体处理器)
    /// - 默认实现: 按消息类型分发到 onDelta/onSync/onTurnResult/onContextStats
    /// - 客户端子类额外处理 InterruptRequest; 服务端子类额外处理 UserInput/Cancel 等
    virtual void onPeerMessage(WireMessage msg);

    /// 从已注册的总线上移除本 IO 的处理器 (若总线仍存活)
    void unregisterFromBus();

    std::shared_ptr<AgentIOTransportBase>        transport_;
    std::weak_ptr<agentxx::middleware::EventBus> registeredBus_;
    size_t                                       interruptServerId_  = 0;
    size_t                                       permissionServerId_ = 0;
};

} // namespace agent
} // namespace agentxx
