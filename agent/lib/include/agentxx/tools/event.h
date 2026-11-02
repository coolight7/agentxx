#pragma once

#include "agentxx/tools/tool.h"
#include "utilxx_base/json.h"

namespace agentxx {
namespace tools {

/// 事件中枢工具 (`agentxx_event`): 挂起等待 / 阻塞等待 / 查状态 / 取消 / 发布
///
/// 语义分两侧:
/// - **读侧** (`watch` / `await`): 登记一个等待, 值到达后作为一条以 `[Event]` 开头的
///   输入进入会话; `watch` 立即返回, 不占轮次; `await` 是有上限的阻塞
/// - **写侧** (`emit` / `end`): 与子代理/插件协作时主动发布值
///
/// 独占执行 (会改 hub 状态), 不声明并行安全; 不做重复调用检查 (`watch` 同一事件
/// 换个 mode 是合法用法)。
class EventTool : public XXToolBase {
public:

    EventTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<agentxx::util::ToolResult> execute_async(const utilxx_base::Json& arguments) override;
};

} // namespace tools
} // namespace agentxx
