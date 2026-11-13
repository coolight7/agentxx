#pragma once

#include "agentxx/tools/tool.h"
#include "utilxx_base/json.h"

namespace agentxx {
namespace tools {

/// 后台任务工具族: 运行 / 查询 / 阻塞等待 / 管理
///
/// 统一约定:
/// - 任务标识是 `asyncId`（`async-7` 这种），提交后立刻返回，结果由任务自己产出；
/// - **默认主动回上下文**（`agentxx_async_run` 的 `notify`，默认 `true`）：受理时就给
///   创建者会话登记一条 `next-step` 定向等待 —— 轮次在跑就插到下一次模型调用前，
///   轮次已结束就入队触发下一轮；
/// - 查询范围固定为**本会话**（不再有跨 agent 的全局视图）。

/// 运行一条后台任务 (`agentxx_async_run`)
///
/// - 参数 `tool`（必填，**只有 `agentxx_async_run` 自己不允许** —— 套自己只会变成无意义
///   的递归; 其余工具全部可用: 内置、插件/动态注册、其他后台任务管理工具）
/// - `args`（原样转交）+ `notify`（默认 true）+ `label` + `delayMs`（起跑延时，
///   未到之前一直算排队中）
/// - `queueTimeoutMs`（排队超时，延时不计入）+ `runTimeoutMs`（运行超时，排队/延时不计入），
///   默认都是 0 = 不限制
/// - `tool` 是 `agentxx_subagent` 时走 `kind=agent`（参数与直接调用完全一致），
///   其余走 `kind=tool`
/// - 提交前按目标工具问一次权限（与前台同一套权限中间件）。
class AsyncRunTool : public XXToolBase {
public:

    AsyncRunTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<agentxx::util::ToolResult>
        execute_async(const utilxx_base::Json& arguments) override;
};

/// 查询后台任务 (`agentxx_async_query`)
///
/// `id` 给单条（含结果正文）；否则按 `group` 列本会话的任务
/// （`all` / `queued` / `running` / `done` / `failed` / `canceled`，默认 `all`）。
/// 还在起跑延时里的任务在列表项开头带 `*`。
class AsyncQueryTool : public XXToolBase {
public:

    AsyncQueryTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<agentxx::util::ToolResult>
        execute_async(const utilxx_base::Json& arguments) override;
};

/// 阻塞等待某条后台任务 (`agentxx_async_await`)
///
/// - `id` 必填；`timeoutMs` 是**等待结果的整体超时**（延时 + 排队 + 运行全算在内），
///   默认 30 秒，硬上限 5 分钟
/// - 任务还在 `delayMs` 延时里、且剩余延时比 `timeoutMs` 还长时，直接回一段说明
///   （不挂到必然超时）；要提前起跑用 `agentxx_async_manager {opt:"delay", delayMs:0}`
/// - 已经结束的任务直接返回任务表里的结果（再问一次就再给一份，工具语义不做去重）
/// - 还在跑时在任务条目上登记一次等待，等它进入终态（不订阅事件）；
///   任务收尾时看到有等待登记就把正文交给本次调用，本次自动通知不再插入会话
/// - 超时返回一段说明文本（不是工具失败），会话取消抛取消。
class AsyncAwaitTool : public XXToolBase {
public:

    AsyncAwaitTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<agentxx::util::ToolResult>
        execute_async(const utilxx_base::Json& arguments) override;
};

/// 管理后台任务 (`agentxx_async_manager`)
///
/// - `opt`（必填）：
///     - `cancel`：取消目标任务（排队中的直接出队并按取消收尾，运行中的置取消令牌）
///     - `delay`：调整目标任务的起跑延时，`delayMs` 必填，`0` = 移除延时（立即排队起跑）
/// - `id`：任务 id 数组，逐条处理（声明就是数组，模型给单个字符串时由执行层
///   按 schema 自愈成数组；`group` 批量入口已移除，要批量就传数组）
/// - 只能操作本会话启动的任务；逐条结果写进返回文本，全部失败时按工具失败返回。
class AsyncManagerTool : public XXToolBase {
public:

    AsyncManagerTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<agentxx::util::ToolResult>
        execute_async(const utilxx_base::Json& arguments) override;
};

} // namespace tools
} // namespace agentxx
