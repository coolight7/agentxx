#include "agentxx/agent/agent_runner.h"

#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/tools/subagent.h"
#include "agentxx/util/log.h"
#include <optional>

namespace agentxx {
namespace agent {

asio::awaitable<AgentRunner::Outcome> AgentRunner::run(
    std::shared_ptr<AgentContext>                 ctx,
    neograph::graph::GraphEngine*                 engine,
    std::string_view                              sessionId,
    neograph::graph::RunConfig                    cfg,
    std::shared_ptr<neograph::graph::CancelToken> cancelToken,
    Hooks                                         hooks,
    std::optional<neograph::graph::RunResult>     initialResult
) {
    auto session = ctx->getSession(sessionId);

    // 首跑 cfg 已被 move 进 engine, 提前保存 resume 需要沿用的运行参数
    // (resume 必须与首跑同 stream_mode/max_steps, 否则事件回调/步数预算漂移)
    const auto resumeStreamMode = cfg.stream_mode;
    const auto resumeMaxSteps   = cfg.max_steps;

    auto fOnBeforeResume = [&]() -> asio::awaitable<void> {
        engine->update_state(std::string{sessionId}, [&](neograph::graph::GraphState& state) {
            state.overwrite("messages", session->llmMessages);
        });
        if (hooks.onBeforeResume) {
            co_await hooks.onBeforeResume(sessionId);
        }
        co_return;
    };

    auto fOnRunResult = [&](neograph::graph::RunResult& result) {
        if (result.interrupted) {
            // 中断时 [result] 内的 messages 是被 neograph::engine
            // 回滚的，本轮 session 的上下文已经被丢弃；应该取中断时
            // 保存的 messages
            auto imCopy = ctx->middlewareHandleContext->getGraphDataItemValue<neograph::json>(
                sessionId,
                agentxx::middleware::MiddlewareContext::graphDataKey_tempMessages
            );
            if (imCopy.is_array()) {
                session->llmMessages = std::move(imCopy);
            }
            // 注意: 此处不可清理 tempMessages —— 随后的 HIL 处理阶段
            // (handleInterrupt/权限询问) 仍会读取该快照校验中断时刻上下文;
            // 清理时机收敛到图完整结束 (下方 else 分支)
        } else {
            session->llmMessages = result.channel_raw("messages");
            // 图已完整结束: 清理中断/异常期间遗留的 tempMessages 快照。
            // - 本轮为 resume 完成时快照已被权威结果取代, 留存会误导后续
            //   错误路径的上下文回退源 (过期回卷)
            // - getGraphDataItemValue 对缺失键会自动创建空条目, 无论存在与否
            //   统一移除保持干净
            ctx->middlewareHandleContext->removeGraphDataItem(
                sessionId,
                agentxx::middleware::MiddlewareContext::graphDataKey_tempMessages
            );
        }
        if (hooks.onRunResult) {
            hooks.onRunResult(result, sessionId);
        }
    };

    Outcome outcome;

    // HIL 请求超时: 优先取 IO 端点配置 (SessionServerAgentIO::interruptTimeout),
    // 否则默认不限制 (<=0 表示不限制, 避免 HIL 弹窗被总线默认 30s 超时提前截断)
    auto interruptTimeout = std::chrono::milliseconds{0};
    if (session && session->io) {
        if (auto* serverIo
            = dynamic_cast<agentxx::agent::SessionServerAgentIO*>(session->io.get())) {
            interruptTimeout = serverIo->interruptTimeout();
        }
    }

    std::optional<neograph::graph::RunResult> result;

    if (initialResult.has_value()) {
        // 程序重启恢复中断: 跳过首跑, 直接进入中断处理循环
        result = std::move(initialResult);
    } else {
        result = co_await engine->run_stream_async(std::move(cfg), hooks.eventCallback);
    }

    fOnRunResult(*result);

    while (result.has_value() && result->interrupted) {
        // 记录中断节点信息到 graphData, 供程序重启恢复中断时复用
        ctx->middlewareHandleContext->setGraphDataItemValue<std::string>(
            sessionId,
            agentxx::middleware::MiddlewareContext::graphDataKey_interruptNode,
            result->interrupt_node
        );
        ctx->middlewareHandleContext->setGraphDataItemValue<neograph::json>(
            sessionId,
            agentxx::middleware::MiddlewareContext::graphDataKey_interruptValue,
            result->interrupt_value
        );

        // 本轮 graph 还没有执行完成, 序列化 graphData 到 state checkpoint,
        // 以防中断处理期间程序终止导致 graphData 丢失
        engine->update_state(std::string{sessionId}, [&](neograph::graph::GraphState& state) {
            auto data = ctx->middlewareHandleContext->getGraphDataToState(state, sessionId);
            state.overwrite(agentxx::middleware::MiddlewareContext::channel_savedGraphData, data);
        });

        auto crudeResult = std::move(result);
        result           = std::nullopt;

        outcome.interrupted = true;
        auto interruptNode  = crudeResult->interrupt_node;
        auto interruptValue = crudeResult->interrupt_value.dump();

        auto resumeValues = neograph::json{};

        // 从 [graphDataKey_interruptArgs] 提取中断参数
        const auto interruptArglist = agentxx::middleware::InterruptHandleArg::listFromJson(
            ctx->middlewareHandleContext->getGraphDataItemValue<neograph::json>(
                sessionId,
                agentxx::middleware::MiddlewareContext::graphDataKey_interruptArgs
            )
        );
        size_t argIndex = 0;
        for (const auto& interruptArg : interruptArglist) {
            ++argIndex;

            if (interruptArg.name == "subagent") {
                // 统一批量委派: 参数解析收敛到共享实现 (parseSubagentBatchFromInterrupt +
                // buildSubagentResumeValues, 与 SubAgentManagerTool 提取规则一致)
                // - 经 ctx->bus 请求 service.subagent: 宿主在根与每个子代理的
                //   总线上统一 registerServer, 嵌套委派与根委派完全同路径 (扁平化)
                auto batchReq = agentxx::tools::parseSubagentBatchFromInterrupt(
                    interruptArg,
                    ctx->agentConfig ? ctx->agentConfig->agentName : std::string_view{},
                    sessionId,
                    cancelToken
                );
                std::expected<events::RespSubagentBatch, std::string> batchResp;
                if (ctx->bus) {
                    // 委派请求不限制超时: 子代理可能长时间运行, 总线默认
                    // 30s 会截断长任务 (旧实现缺陷, 统一修复)
                    batchResp = co_await ctx->bus
                                    ->request<events::ReqSubagentBatch, events::RespSubagentBatch>(
                                        events::Topic::Subagent,
                                        std::move(batchReq),
                                        std::chrono::milliseconds{0}
                                    );
                }
                if (batchResp.has_value()) {
                    // 取消检查: 子代理被取消 (返回 cancelled 结构化字段置位) 时不写回
                    // resume 值, 由外层统一按取消处理, 避免把结果当正常摘要/结果 resume 后继续执行
                    bool subagentCancelled = false;
                    for (const auto& r : batchResp->results) {
                        if (r.cancelled) {
                            subagentCancelled = true;
                            XX_LOGI(
                                "AgentRunner `{}` subagent delegation cancelled: resultId={}, msg={}",
                                sessionId,
                                r.resultId,
                                r.errorMessage
                            );
                            break;
                        }
                    }
                    if (subagentCancelled || (cancelToken && cancelToken->is_cancelled())) {
                        throw neograph::graph::CancelledException("subagent delegation cancelled");
                    }
                    agentxx::tools::buildSubagentResumeValues(
                        resumeValues,
                        *batchResp,
                        interruptArg.resultId
                    );
                } else {
                    XX_LOGE(
                        "AgentRunner `{}` subagent delegation failed: {}",
                        sessionId,
                        batchResp.error()
                    );
                }
            } else {
                // HIL: 中断头消息 (根插入会话历史 + MessageTip WireDelta;
                // 子代理无 WireDelta 输出通道, hook 为空)
                if (hooks.onInterruptTip) {
                    hooks.onInterruptTip(interruptNode, interruptValue, interruptArg.name);
                }
                if (session && session->bus) {
                    // 显式传递中断等待超时: 与 IO 端点 interruptTimeout 配置一致,
                    // 避免被总线默认 30s 超时截断 (用户长时间未响应中断弹窗时丢失中断);
                    // <=0 表示不限制
                    auto resp = co_await
                        [&](
                        ) -> asio::awaitable<std::expected<events::RespInterrupt, std::string>> {
                        auto req = events::ReqInterrupt{
                            .agentName
                            = ctx->agentConfig ? ctx->agentConfig->agentName : std::string{},
                            .sessionId         = std::string{sessionId},
                            .interruptNode     = interruptNode,
                            .interruptValue    = interruptValue,
                            .handleName        = interruptArg.name,
                            .interruptArgsJson = interruptArg.toJson().dump(),
                            .resultId          = interruptArg.resultId,
                        };
                        co_return co_await session->bus
                            ->request<events::ReqInterrupt, events::RespInterrupt>(
                                events::Topic::Interrupt,
                                std::move(req),
                                interruptTimeout
                            );
                    }();
                    // HIL 未响应 (无处理者/超时/过期): 不写回 resume 值,
                    // 由外层按"中断未完成"处理; 取消标记则直接抛取消,
                    // 不再 resume (否则"打断后马上自动恢复")
                    if (resp.has_value() && resp->handled) {
                        auto rid = interruptArg.resultId;
                        if (rid.empty()) {
                            rid = std::to_string(argIndex);
                        }
                        resumeValues[rid] = neograph::json::parse(resp->resultJson);
                    } else if (resp.has_value() && !resp->handled && resp->resultJson.find("__cancelled__") != std::string::npos) {
                        throw neograph::graph::CancelledException("HIL interrupted by cancel");
                    }
                }
            }
        }

        if (false == resumeValues.empty()) {
            // 中断处理完成, 清理参数并写回结果
            ctx->middlewareHandleContext->removeGraphDataItem(
                sessionId,
                agentxx::middleware::MiddlewareContext::graphDataKey_interruptArgs
            );
            ctx->middlewareHandleContext->setGraphDataItemValue<neograph::json>(
                sessionId,
                agentxx::middleware::MiddlewareContext::graphDataKey_interruptResult,
                resumeValues
            );

            co_await fOnBeforeResume();

            // 取消检查: 中断处理完成、resume 前若令牌已取消则直接抛取消,
            // 不再恢复执行 (否则表现为"打断后马上自动恢复继续执行")
            if (cancelToken && cancelToken->is_cancelled()) {
                throw neograph::graph::CancelledException("cancelled before resume");
            }

            // 恢复执行中断点, 直接回到触发中断的 Node
            // - 必须携带 cancel_token: 否则 resume 出的新 run 无取消能力,
            //   后续 llm/toolcall 的取消埋点 (if cancel_token) 全部跳过,
            //   执行中 HTTP 也无法被打断, 表现为"压缩完成后怎么都停不下来"
            neograph::graph::RunConfig resumeCfg;
            resumeCfg.thread_id    = std::string{sessionId};
            resumeCfg.cancel_token = cancelToken;
            resumeCfg.stream_mode  = resumeStreamMode;
            resumeCfg.max_steps    = resumeMaxSteps;
            result                 = co_await engine->resume_async(
                std::move(resumeCfg),
                neograph::json{},
                hooks.eventCallback
            );
            fOnRunResult(*result);
        }
        // 无任何可注入结果: 停止循环, 按"中断未完成"处理
        // result 保持 nullopt, while 退出
    }

    // 循环退出条件:
    // - resume 正常完成 (result->interrupted == false) → 中断已全部处理
    // - resumeValues 空 (无处理者/未响应) → 中断未完成
    outcome.unresolvedInterrupt = result.has_value() && result->interrupted;
    if (outcome.unresolvedInterrupt) {
        // 未完成的中断节点: 从 graphData 读取 (循环内已记录)
        outcome.interruptNode = ctx->middlewareHandleContext->getGraphDataItemValue<std::string>(
            sessionId,
            agentxx::middleware::MiddlewareContext::graphDataKey_interruptNode
        );
    }

    co_return outcome;
}

} // namespace agent
} // namespace agentxx
