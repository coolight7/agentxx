#pragma once

#include "agentxx/tools/tool.h"
#include "utilxx_base/json.h"

namespace agentxx {
namespace tools {

/// 事件发布工具 (`agentxx_async_emit`): 发布值 / 结束事件流
///
/// 只做**写侧**:
/// - 给 `value` 就发布一个值;
/// - 给 `end: true` 就结束这条流 (可以同时发布);
/// - 事件不存在且当前调用有权创建时, 顺手按流式事件建出来 (归属 `tool`)。
///
/// 能不能往某个事件发布, 由**创建它的那一方**决定: 创建时可以用 `public_publish`
/// 声明"任何人也能发", 没声明就只有创建者能发。因此模型可以给插件的事件发值
/// (插件声明了公开时), 也会被内建事件拒绝。
///
/// 读侧不在这里: 后台任务的结果由 `agentxx_async_run` 主动插回会话,
/// 查询 / 等待 / 管理分别见 `agentxx_async_query` / `agentxx_async_await` /
/// `agentxx_async_manager`（见 [async_tools.h](/agent/lib/include/agentxx/tools/async_tools.h)）。
///
/// 独占执行 (会改 hub 状态), 不声明并行安全; 不做重复调用检查。
class AsyncEmitTool : public XXToolBase {
public:

    AsyncEmitTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<agentxx::util::ToolResult> execute_async(const utilxx_base::Json& arguments) override;
};

} // namespace tools
} // namespace agentxx
