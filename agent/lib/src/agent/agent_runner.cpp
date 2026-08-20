#include "agentxx/agent/agent_runner.h"

#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/tools/subagent_shared.h"
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
            const auto& im = ctx->middlewareHandleContext->getGraphDataItemValue<neograph::json>(
                sessionId,
                agentxx::middleware::MiddlewareContext::graphDataKey_tempMessages
            );
            if (im.is_array()) {
                session->llmMessages = im;
            }
        } else {
            session->llmMessages = result.channel_raw("messages");
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
                // 已禁用时直接返回错误, 不再派生
                if (ctx->agentConfig && !ctx->agentConfig->enableSubagent) {
                    XX_LOGW("AgentRunner `{}` subagent disabled, delegation rejected", sessionId);
                    resumeValues
                        [interruptArg.resultId.empty() ? std::to_string(argIndex)
                                                       : interruptArg.resultId]
                        = neograph::json{
                            {"error", "subagent disabled by config (subagent.enable=false)"}
                    };
                    continue;
                }
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
                // HIL: 中断头消息 (根插入会话历史 + MessageTip Delta;
                // 子代理无 Delta 输出通道, hook 为空)
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
                    if (resp.has_value() && resp->handled) {
                        auto rid = interruptArg.resultId;
                        if (rid.empty()) {
                            rid = std::to_string(argIndex);
                        }
                        resumeValues[rid] = neograph::json::parse(resp->resultJson);
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

            // 恢复执行中断点, 直接回到触发中断的 Node
            result = co_await engine->resume_async(
                std::string{sessionId},
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
