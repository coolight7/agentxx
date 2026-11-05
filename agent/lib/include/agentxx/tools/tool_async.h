#pragma once

#include "agentxx/tools/tool.h"
#include "utilxx_base/json.h"

namespace agentxx {
namespace tools {

/// 后台工具调用 (`agentxx_tool_async`)
///
/// 把**一次工具调用**放到后台跑 (提交 `kind=tool` 的后台任务):
/// - 参数 `tool` 是工具名 (必须在工具注册表里: 插件工具或动态注册的工具),
///   `args` 是它的参数原样转交;
/// - 提交前按目标工具问一次权限 (与前台同一套权限中间件), 后台执行期间不再问人;
/// - 受理后立即返回 `[Async tool started] id=...`, 本轮可以继续做别的事;
/// - 跑完结果作为 `agentxx.agent.async.done` 的值回到会话 (`agentxx_event` 查/等),
///   也可以由插件用 `notify_delivery` / 自己投递;
/// - 取消走 `agentxx_event {opt:"cancel", id:"async-N"}` (尽力而为)。
///
/// 只在开启 `async_work.enable` 时注册 (关掉就没有后台执行能力, 应该是同步调用原工具)。
class ToolAsyncTool : public XXToolBase {
public:

    ToolAsyncTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<agentxx::util::ToolResult>
        execute_async(const utilxx_base::Json& arguments) override;
};

} // namespace tools
} // namespace agentxx
