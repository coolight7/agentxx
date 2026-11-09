#pragma once

#include "agentxx/tools/tool.h"
#include "utilxx_base/json.h"

namespace agentxx {
namespace tools {

/// 后台任务工具族: 开始 / 查询 / 阻塞等待 / 取消
///
/// 统一约定:
/// - 任务标识是 `asyncId`（`async-7` 这种），提交后立刻返回，结果由任务自己产出；
/// - **默认主动回上下文**（`agentxx_async_start` 的 `notify`，默认 `true`）：受理时就给
///   创建者会话登记一条 `next-step` 定向等待 —— 轮次在跑就插到下一次模型调用前，
///   轮次已结束就入队触发下一轮；
/// - 查询范围固定为**本会话**（不再有跨 agent 的全局视图）。

/// 开始一条后台任务 (`agentxx_async_start`)
///
/// - 参数 `tool`（必填，**只有 `agentxx_async_start` 自己不允许** —— 套自己只会变成无意义
///   的递归; 其余工具全部可用: 内置、插件/动态注册、其他后台任务管理工具）
/// - `args`（原样转交）+ `notify`（默认 true）+ `label` + `delayMs`（起跑延时，
///   未到之前一直算排队中）
/// - `tool` 是 `agentxx_subagent` 时走 `kind=agent`（参数与直接调用完全一致），
///   其余走 `kind=tool`
/// - 提交前按目标工具问一次权限（与前台同一套权限中间件）。
class AsyncStartTool : public XXToolBase {
public:

    AsyncStartTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<agentxx::util::ToolResult>
        execute_async(const utilxx_base::Json& arguments) override;
};

/// 查询后台任务 (`agentxx_async_query`)
///
/// `id` 给单条（含结果正文）；否则按 `group` 列本会话的任务
/// （`all` / `queued` / `running` / `done` / `failed` / `canceled`，默认 `all`）。
class AsyncQueryTool : public XXToolBase {
public:

    AsyncQueryTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<agentxx::util::ToolResult>
        execute_async(const utilxx_base::Json& arguments) override;
};

/// 阻塞等待某条后台任务 (`agentxx_async_await`)
///
/// - `id` 必填；`timeout_ms` 默认 30 秒，硬上限 5 分钟；
/// - `start_now`（默认 false）：目标还在 `delayMs` 延时里时，抹掉剩余延时让它立刻起跑；
///   不给时如果剩余延时比 `timeout_ms` 还长，直接回一段说明（不挂到必然超时）；
/// - 已经结束的任务直接返回台账里的结果，不再等事件；
/// - 超时返回一段说明文本（不是工具失败），会话取消抛取消。
class AsyncAwaitTool : public XXToolBase {
public:

    AsyncAwaitTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<agentxx::util::ToolResult>
        execute_async(const utilxx_base::Json& arguments) override;
};

/// 取消后台任务 (`agentxx_async_cancel`)
///
/// `id` 取消单条（只限本会话启动的）；`group` 取消本会话的一批
/// （`all` / `queued` / `running`；终态分组没有可取消的东西，按参数错误拒）。
class AsyncCancelTool : public XXToolBase {
public:

    AsyncCancelTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<agentxx::util::ToolResult>
        execute_async(const utilxx_base::Json& arguments) override;
};

} // namespace tools
} // namespace agentxx
