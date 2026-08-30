#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/event/events.h"
#include "agentxx/middlewares/middleware.h"
#include "neograph/graph/engine.h"
#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace agentxx {
namespace agent {

/// 统一的 "引擎运行 + 中断处理 + 恢复" 循环 (主 agent 与子代理共用)
/// - 语义与旧 BaseAgent::runTurnAsync / AgentHost::spawnOneTask
///   内联的中断循环完全一致, 消除两份复制与行为漂移
/// - 中断分派 (按 interrupt handle name):
///   - "subagent": 经 ctx->bus 请求 service.subagent (宿主在根与子代理总线上
///     统一 registerServer; 嵌套委派与根委派完全同路径, 不需要宿主函数直调)
///   - 其他: 经 session->bus 请求 service.interrupt (HIL, 由 IO 端点应答)
/// - 取消: cancelToken 传入, operation_aborted 按取消语义由调用方
///   (catchErrorAsync) 处理
/// - 超时策略:
///   - HIL 请求: 统一取 IO 端点 (SessionServerAgentIO::interruptTimeout) 配置,
///     <=0 不限制 (避免 HIL 弹窗被总线默认 30s 超时提前截断)
///   - subagent 委派请求: 不限制超时 (子代理可能长时间运行; 修复旧实现
///     根 agent 总线请求默认 30s 截断长任务的问题)
class AgentRunner {
public:

    /// 调用方差异 (主 agent 与子代理的行为差异点, 全部收敛为 hook)
    struct Hooks {
        /// 引擎事件回调 (run/resume 共用; 根: EventBridge,
        /// 子代理: token 收集 + hostBus 进度发布)
        neograph::graph::GraphStreamCallback eventCallback;

        /// 中断头消息回调 (根: 插入会话历史 + MessageTip WireDelta;
        /// 子代理无 WireDelta 输出通道: 为空)
        std::function<void(std::string_view node, std::string_view value, std::string_view handle)>
            onInterruptTip;

        /// 中断处理完成、resume 前的回调 (根: 将 session llmMessages 写回
        /// engine state; 子代理: 空)
        std::function<asio::awaitable<void>(std::string_view sessionId)> onBeforeResume;

        /// 每次 run/resume 返回后的回调 (根: 从中断保存的 tempMessages 恢复
        /// session llmMessages; 子代理: 空)
        std::function<void(neograph::graph::RunResult& result, std::string_view sessionId)>
            onRunResult;
    };

    struct Outcome {
        /// 本轮是否发生过中断 (循环至少执行一次)
        /// - 根 agent (runTurnAsync) 使用: 中断被处理并 resume
        ///   完成后仍为 true, 表示"本轮有中断交互" (旧语义, 测试依赖)
        bool interrupted = false;

        /// 循环退出时中断仍未完成 (无处理者/未响应/无结果可注入)
        /// - 子代理 (spawnOneTask) 使用: 仅此情况报错 "interrupt not handled",
        ///   中断成功处理并 resume 正常完成后为 false
        bool unresolvedInterrupt = false;

        /// 未完成中断发生的节点名 (供错误报告)
        std::string interruptNode;
    };

    /// - [ctx]       agent 上下文 (主 agent 或子代理)
    /// - [engine]    图引擎 (AgentContext 不持有, 由调用方传入)
    /// - [sessionId] 会话
    /// - [cfg]       首跑配置 (resume_if_exists 等由调用方构造)
    /// - [cancelToken] 取消令牌 (可空)
    /// - [hooks]     调用方差异
    /// - [initialResult] 非空时跳过首跑直接进入中断处理循环
    ///   (程序重启恢复中断路径, 由调用方从 checkpoint 重建中断信息)
    asio::awaitable<Outcome>
        run(std::shared_ptr<AgentContext>                 ctx,
            neograph::graph::GraphEngine*                 engine,
            std::string_view                              sessionId,
            neograph::graph::RunConfig                    cfg,
            std::shared_ptr<neograph::graph::CancelToken> cancelToken,
            Hooks                                         hooks,
            std::optional<neograph::graph::RunResult>     initialResult = std::nullopt);
};

} // namespace agent
} // namespace agentxx
