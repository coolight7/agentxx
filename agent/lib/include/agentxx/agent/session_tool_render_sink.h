#pragma once

namespace agentxx {
namespace agent {

/// 工具特化渲染变更的出口 (由"当前驱动这个 agent 的端点"实现)
///
/// 背景: 渲染器登记在 agent 侧 (`agentxx.agent.tool_render`), 但显示名与"算内容"
/// 的请求在客户端。主程序需要一条通道告诉客户端"这份集合变了", 客户端据此替换
/// 本地快照并清掉失效条目的缓存。
///
/// 实现方: `SessionServerAgentIO` (服务端会话端点)。它把弱引用登记在
/// `AgentContext::toolRenderSink` 上, 挂载变化时从这里取一次。
class SessionToolRenderSink {
public:

    virtual ~SessionToolRenderSink() = default;

    /// 本 agent 的工具渲染器集合变了 (注册 / 注销 / 插件禁用或卸载)
    ///
    /// - 实现方应把最新**全量**快照推给已连接的客户端 (空集合也要推, 表示清空)
    /// - 可能在任意线程调用 (实际来自 io 线程的注册路径), 实现方需自行投递到自己的
    ///   事件循环; 不得同步阻塞
    virtual void onToolRenderersChanged() {}
};

} // namespace agent
} // namespace agentxx
