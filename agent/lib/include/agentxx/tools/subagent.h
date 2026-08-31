#pragma once

#include "agentxx/event/events.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/tools/tool.h"
#include "neograph/graph/cancel.h"
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace agentxx {
namespace tools {

/// subagent 注册项基类: 仅承载名称/描述/系统提示等静态元数据
/// - 实际执行由 AgentHost 派生独立 agent 完成 (中断委派, 不再使用图内
///   嵌套 subgraph; 旧的 getSubgraph/onSubagentEnd 已移除)
class SubAgentTaskBase {
public:

    const std::string name;
    const std::string depict;
    std::string       systemPrompt;

    SubAgentTaskBase(
        std::string_view in_subAgentName,
        std::string_view in_subAgentDepict,
        std::string_view in_systemPrompt
    );

    virtual ~SubAgentTaskBase();
};

/// 默认 subagent 任务 (普通委派: 隔离上下文独立运行)
class SubAgentNormalTask : public SubAgentTaskBase {
public:

    SubAgentNormalTask(std::string_view in_subAgentName, std::string_view in_subAgentDepict);
};

class SubAgentManagerTool : public XXToolBase {
public:

    std::map<std::string, std::shared_ptr<SubAgentTaskBase>, std::less<>> subAgentList{};

    SubAgentManagerTool(
        std::string_view                            in_nodeName,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
    );

    std::string get_name() const override;

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;

    ~SubAgentManagerTool() override;

    /// 在 EventBus 上注册 subagent 执行服务 (service.subagent.execute)
    void registerOnBus(const std::shared_ptr<agentxx::event::EventBus>& bus);

    /// 从 EventBus 注销
    void unregisterFromBus();

private:

    std::weak_ptr<agentxx::event::EventBus> registeredBus_;
    size_t                                  executeServerId_ = 0;
};

/// 子代理委派共享逻辑 (中断处理循环写入侧 与 SubAgentManagerTool 读取侧共用)
/// - 消除 BaseAgent / AgentHost / SubAgentManagerTool 三处重复的 key 规则
///   与参数组装, 防止规则漂移
/// - 中断参数格式: {tasks: [...]} 数组 (SubAgentManagerTool 构造);
///   兼容旧单发格式 (直接含 subagent 字段时包装为 1 个 task)

/// 从中断参数构建批量委派请求 (统一批量语义)
/// - agentName: 发起方 agent 名 (ReqSubagentBatch.parentAgentName)
/// - sessionId:  发起方会话 thread (ReqSubagentBatch.parentSessionId,
///   宿主据此查 sessionDepth_ 嵌套深度预算)
/// - cancelToken: 级联取消令牌 (父取消 → 中止全部在跑子代理; 可空)
events::ReqSubagentBatch parseSubagentBatchFromInterrupt(
    const middleware::InterruptHandleArg&         interruptArg,
    std::string_view                              agentName,
    std::string_view                              sessionId,
    std::shared_ptr<neograph::graph::CancelToken> cancelToken
);

/// 子代理结果 key 规则: (tool_call_id + "_") + (result_id | 任务序号)
/// - 前缀避免同一轮多个中断的序号 key 互相覆盖
/// - 写入侧 (中断处理循环 buildSubagentResumeValues) 与读取侧
///   (SubAgentManagerTool::execute_async) 必须使用同一函数
inline std::string
    makeSubagentResumeKey(std::string_view toolCallId, std::string_view resultId, size_t idx) {
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
        // 注意: 正常结果必须用圆括号直接初始化为标量字符串 (而非 {} 列表初始化,
        // 否则会命中 initializer_list 构造产生 ["content"] 数组包裹,
        // 破坏读取端 "单任务返回纯文本" 语义); 错误任务保持 {"error": ...} 对象
        resumeValues[key] = r.hasError
                                ? neograph::json{{"error", std::string{r.errorMessage}}}
                                : neograph::json(r.content);
    }
}

}; // namespace tools
}; // namespace agentxx
