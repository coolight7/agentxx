#pragma once

#include "agentxx/tools/tool.h"
#include "utilxx_base/json.h"

namespace agentxx {
namespace tools {

/// 事件中枢工具 (`agentxx_event`): 发布值 / 结束事件流
///
/// 只做**写侧**:
/// - `opt=emit`: 建（可选）/ 发布一个值到 `agentxx.agent.event.*` 事件
/// - `opt=end`:  结束一个流式事件（可顺带移除记录）
///
/// 读侧已经拆走: 后台任务的结果由 `agentxx_async_start` 主动插回会话,
/// 查询 / 等待 / 取消分别见 `agentxx_async_query` / `agentxx_async_await` /
/// `agentxx_async_cancel`（见 [async_tools.h](/agent/lib/include/agentxx/tools/async_tools.h)）。
///
/// 独占执行 (会改 hub 状态), 不声明并行安全; 不做重复调用检查。
class EventTool : public XXToolBase {
public:

    EventTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<agentxx::util::ToolResult> execute_async(const utilxx_base::Json& arguments) override;
};

} // namespace tools
} // namespace agentxx
