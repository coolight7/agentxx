#pragma once

#include "agentxx/agent/io/agent_io_transport.h"
#include <string_view>

namespace agentxx {
namespace agent {

/// 客户端事件接收器: client 端点 (TUI/CLI) 关键路径上的被动回调接口
///
/// 用途: 向 client 侧插件系统 (ClientPluginManager) 转发端点事件, 使插件可
/// 订阅 会话/连接/消息 等 client 生命周期事件 (见 client_plugin_api.h 的
/// AgentxxClientEvent 枚举)。
///
/// 线程约定:
/// - 所有回调在 client io 线程同步调用 (端点 onPeerMessage / sendUserInput /
///   setConnState 等路径); 实现必须快速返回, 不得阻塞
/// - 实现侧负责分发到订阅的插件回调 (同样在 io 线程)
/// - 端点销毁/停止前无需显式摘除: 端点是 sink 的持有方 (shared_ptr),
///   生命周期一致
///
/// 装配: AgentIOBase::setEventSink() 注入 (模式启动时由 mode_runners 调用)。
/// 端点未设置 sink 时所有回调为 no-op, 不影响现有功能。
class ClientEventSink {
public:

    virtual ~ClientEventSink() = default;

    /// 服务端就绪 (agent init 完成, 会话驱动循环即将开始消费输入)
    /// - payload: {"uiCaps": n, "sessionId": "..."}
    virtual void onReady() {}

    /// 连接状态变化 (state: "connecting" / "connected" / "failed";
    ///  progress: 启动步骤文本, 可为空)
    /// - payload: {"connState": "...", "startupProgress": "..."}
    virtual void onConnStateChanged(std::string_view state, std::string_view progress) {}

    /// 用户输入已发送 (text 为发送原文)
    /// - payload: {"sessionId": "...", "text": "..."}
    virtual void onUserInput(std::string_view sessionId, std::string_view text) {}

    /// 增量事件 (流式 token / tool 生命周期 / 轮次边界等)
    /// - payload: delta JSON (与 wire delta 字段一致)
    virtual void onDelta(const WireDelta& delta) {}

    /// 轮次结束
    /// - payload: turn result JSON
    virtual void onTurnResult(const WireTurnResult& result) {}

    /// 当前会话切换 (sessionId 为切换后的会话)
    /// - payload: {"sessionId": "..."}
    virtual void onSessionSwitched(std::string_view sessionId) {}

    /// 插件事件转发 (Server -> Client, WirePluginData)
    /// - payload: {"plugin": "...", "event": "...", "data": "..."}
    virtual void onPluginData(const WirePluginData& data) {}
};

} // namespace agent
} // namespace agentxx
