#pragma once

#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/events.h"
#include <map>
#include <memory>
#include <string>

namespace agentxx {
namespace middleware {

/// Subagent
/// - service.subagent: 单个 subagent 委派 (ReqSubagentStart ->
/// RespSubagentResult)
/// - service.subagent.batch: 批量 subagent 并发委派 (wait_for_all)
/// - service.crossagent: 跨 agent 查询路由 (ReqCrossAgent -> RespCrossAgent)
///   - 维护当前运行中 subagent 的注册表, 查询按 toAgent 路由
/// - 把 subagent 的 GraphEvent 转成 SubagentProgress 发布到总线
class SubagentSupervisor {
public:

    std::weak_ptr<agentxx::agent::AgentContext> agentContext;
    size_t                                      serverId           = 0;
    size_t                                      batchServerId      = 0;
    size_t                                      crossAgentServerId = 0;
    bool                                        registered         = false;

    /// 当前运行中 subagent 名单 (用于跨 agent 查询路由)
    /// - subagent 启动时插入, 完成时移除
    /// - 跨 agent 查询按 toAgent 在此查找是否运行中
    /// NOTE: 跨 agent 查询的完整实现 (向 subagent 注入消息并等待应答) 尚未完成,
    ///       当前 handleCrossAgent 返回明确的 "not implemented" 错误,
    ///       此名单仅用于校验目标 agent 是否运行中
    std::map<std::string, bool, std::less<>> runningRegistry_;

    explicit SubagentSupervisor(std::weak_ptr<agentxx::agent::AgentContext> ctx);

    asio::awaitable<void> start();

    void stop();

    ~SubagentSupervisor();

private:

    /// 运行单个 subagent (单/批量共用)
    asio::awaitable<events::RespSubagentResult> runSubagent(
        std::string_view subagentName,
        std::string_view systemPromptIn,
        std::string_view message,
        std::string_view parentThreadId = ""
    );

    /// 批量并发运行多个 subagent (真并发: co_spawn + channel wait_for_all)
    /// - 单 io_context 协作式调度: 各 subagent 协程在 co_await 挂起点交替推进
    /// - 等待全部完成才返回, 结果顺序与输入任务一致 (按 index 写入)
    asio::awaitable<events::RespSubagentBatch> runBatch(const events::ReqSubagentBatch& req);

    /// 跨 agent 查询路由
    /// - 按 toAgent 查找运行中 subagent
    /// - NOTE: 完整实现 (向 subagent 注入消息并等待应答) 尚未完成
    ///         当前返回明确的 "not implemented" 错误, 避免调用方误以为可用
    ///         runningRegistry_ 仅用于校验目标 agent 是否运行中
    asio::awaitable<events::RespCrossAgent> handleCrossAgent(const events::ReqCrossAgent& req);
};

} // namespace middleware
} // namespace agentxx
