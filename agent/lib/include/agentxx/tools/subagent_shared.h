#pragma once

#include "agentxx/middlewares/events.h"
#include "agentxx/middlewares/middleware.h"
#include "neograph/graph/cancel.h"
#include <memory>
#include <string>
#include <string_view>

namespace agentxx {
namespace tools {

/// 子代理委派共享逻辑 (中断处理循环写入侧 与 SubAgentManagerTool 读取侧共用)
/// - 消除 BaseAgent / AgentHost / SubAgentManagerTool 三处重复的 key 规则
///   与参数组装, 防止规则漂移
/// - 中断参数格式: {tasks: [...]} 数组 (SubAgentManagerTool 构造);
///   兼容旧单发格式 (直接含 subagent 字段时包装为 1 个 task)

/// 从中断参数构建批量委派请求 (统一批量语义)
/// - agentName: 发起方 agent 名 (ReqSubagentBatch.parentAgentName)
/// - threadId:  发起方会话 thread (ReqSubagentBatch.parentThreadId,
///   宿主据此查 threadDepth_ 嵌套深度预算)
/// - cancelToken: 级联取消令牌 (父取消 → 中止全部在跑子代理; 可空)
events::ReqSubagentBatch parseSubagentBatchFromInterrupt(
    const middleware::InterruptHandleArg&              interruptArg,
    std::string_view                                   agentName,
    std::string_view                                   threadId,
    std::shared_ptr<neograph::graph::CancelToken>      cancelToken
);

/// 子代理结果 key 规则: (tool_call_id + "_") + (result_id | 任务序号)
/// - 前缀避免同一轮多个中断的序号 key 互相覆盖
/// - 写入侧 (中断处理循环 buildSubagentResumeValues) 与读取侧
///   (SubAgentManagerTool::execute_async) 必须使用同一函数
inline std::string makeSubagentResumeKey(
    std::string_view toolCallId,
    std::string_view resultId,
    size_t           idx
) {
    auto key = resultId.empty() ? std::to_string(idx) : std::string{resultId};
    if (!toolCallId.empty()) {
        key = std::string{toolCallId} + "_" + key;
    }
    return key;
}

/// 将批量委派响应写入中断结果 map (key 规则见 makeSubagentResumeKey)
/// - 单任务返回纯文本, 多任务按任务顺序编号; 错误任务写入 {"error": ...}
inline void buildSubagentResumeValues(
    neograph::json&                  resumeValues,
    const events::RespSubagentBatch& batchResp,
    std::string_view                 toolCallId
) {
    size_t idx = 0;
    for (const auto& r : batchResp.results) {
        ++idx;
        auto key = makeSubagentResumeKey(toolCallId, r.resultId, idx);
        resumeValues[key] = r.hasError
                                ? neograph::json{{"error", std::string{r.errorMessage}}}
                                : neograph::json{std::string{r.content}};
    }
}

} // namespace tools
} // namespace agentxx
