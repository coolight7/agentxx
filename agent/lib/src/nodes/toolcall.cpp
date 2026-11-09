#include "agentxx/nodes/toolcall.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/neograph_json_bridge.h"
#include "agentxx/util/observability.h"
#include "agentxx/util/share_store_offload.h"
#include "agentxx/util/tool_args_fix.h"
#include "agentxx/util/tool_result.h"

#include "agentxx/event/event_stream.h"
#include "agentxx/event/events.h"
#include "agentxx/event/hub.h"
#include "agentxx/middlewares/interrupt_presets.h"
#include "agentxx/nodes/session_context.h"
#include "agentxx/plugin/tool_registry.h"
#include "agentxx/tools/tool.h"
#include "asio/as_tuple.hpp"
#include "asio/bind_cancellation_slot.hpp"
#include "asio/cancellation_signal.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/experimental/channel.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>

namespace agentxx {
namespace nodes {
namespace {

/// 工具执行事件 (总线 typed 事件 + 事件中枢的 `agentxx.agent.event.tool.end.<工具名>` 值)
///
/// - 两条路都是"先问有没有人关心再发": 没有订阅者/没人等时零开销 —— 工具调用是
///   热路径 (一轮可能几十次), 不能每次都构造事件
/// - 只带元信息 (工具名 / 调用 id / 终态 / 结果字节数), 不带结果正文: 一次工具
///   输出可能很大, 需要正文的工具自己 `emit` 到指定事件
///
/// - `args`:
///     - [ctx]          agent 上下文 (取总线与中枢)
///     - [sessionId]    会话 id (空 = 未知, 只发不带会话信息的版本)
///     - [isStart]      true = 开始, false = 结束
///     - [status]       结束时的终态 (开始无意义, 传 `ToolStatus::Ok`)
///     - [resultBytes]  结果文本字节数
asio::awaitable<void> publishToolEvent(
    const std::shared_ptr<agentxx::agent::AgentContext>& ctx,
    std::string_view                                     sessionId,
    std::string_view                                     toolName,
    std::string_view                                     toolCallId,
    bool                                                 isStart,
    agentxx::util::ToolStatus                            status,
    size_t                                               resultBytes
) {
    if (!ctx) {
        co_return;
    }
    // 事件发布只是元信息: 订阅方抛错、或发布过程正好撞上取消, 都不能打断工具
    // 执行与结果写入 (取消有各自的检查点), 因此这里统一兜住, 只记一条调试日志
    co_await agentxx::util::catchErrorAsync<bool>(
        [&]() -> asio::awaitable<bool> {
            const auto agentName
                = ctx->agentConfig ? ctx->agentConfig->agentName : std::string{"Agentxx"};
            if (isStart) {
                if (ctx->bus
                    && ctx->bus->hasListeners<agentxx::events::EventToolCallStart>(
                        agentxx::events::Topic::ToolCallStart
                    )) {
                    co_await ctx->bus->publish(
                        agentxx::events::Topic::ToolCallStart,
                        agentxx::events::EventToolCallStart{
                            .agentName  = agentName,
                            .sessionId  = std::string{sessionId},
                            .toolName   = std::string{toolName},
                            .toolCallId = std::string{toolCallId},
                        }
                    );
                }
                co_return true;
            }
            if (ctx->bus
                && ctx->bus->hasListeners<agentxx::events::EventToolCallEnd>(
                    agentxx::events::Topic::ToolCallEnd
                )) {
                co_await ctx->bus->publish(
                    agentxx::events::Topic::ToolCallEnd,
                    agentxx::events::EventToolCallEnd{
                        .agentName  = agentName,
                        .sessionId  = std::string{sessionId},
                        .toolName   = std::string{toolName},
                        .toolCallId = std::string{toolCallId},
                        .status     = status,
                    }
                );
            }
            if (ctx->eventHub) {
                const auto hubEvent = fmt::format("agentxx.agent.event.tool.end.{}", toolName);
                if (ctx->eventHub->wants(hubEvent)) {
                    std::string err;
                    if (!ctx->eventHub->publishValue(
                            hubEvent,
                            utilxx_base::Json{
                                {"toolCallId",  std::string{toolCallId}},
                                {"sessionId",   std::string{sessionId}},
                                {"status",      std::string{agentxx::util::toolStatusName(status)}},
                                {"resultBytes", resultBytes      },
                            },
                            "tool",
                            &err
                        )) {
                        XX_LOGW("[event_hub] publish `{}` failed: {}", hubEvent, err);
                    }
                }
            }
            co_return true;
        },
        [&](std::string errinfo) -> asio::awaitable<bool> {
            XX_LOGD("[toolcall] publish tool event failed (ignored): {}", errinfo);
            co_return true;
        },
        [&](std::string errinfo) -> std::optional<bool> {
            XX_LOGD("[toolcall] publish tool event canceled (ignored): {}", errinfo);
            return std::optional<bool>{true};
        }
    );
    co_return;
}

/// 把工具终态翻成给模型读的文本 (实现见 `agentxx/util/tool_result.h` 的
/// [toolResultTextForModel]; 全项目就一份, 测试也用它断言文本)
///
/// - `args`:
///     - [result] 执行层拿到的终态与正文
/// - `return` 写进会话上下文的工具消息正文
std::string formatToolResultText(const agentxx::util::ToolResult& result) {
    return agentxx::util::toolResultTextForModel(result);
}

/// 短路径定案 (未执行就出结果) 的正文排版
/// - 短路径包括: 中断缓存命中 / 工具不存在 / 权限拒绝 / 重复调用被拒 / 参数 JSON
///   解析失败 / 准备阶段异常 / 中断占位
/// - `PreparedToolCall::shortCircuit` 的正文约定是"已排版文本" (缓存命中项存的就是
///   上一轮的结果正文), 因此排版必须在**产生**短路径结果时完成: 漏掉这一步, 模型
///   会读到不带 `[Error: ...]` 的裸错误内容, 与提示词的说明对不上
///
/// - `args`:
///     - [result] 未排版的结果 (正文是错误内容或占位文本)
/// - `return` 正文已按状态排版的结果
agentxx::util::ToolResult formattedShortCircuit(agentxx::util::ToolResult result) {
    result.text = formatToolResultText(result);
    return result;
}

/// 消息级终态标记写入口: 只有非 `Ok` 才写 `extra["status"]`
/// - 界面 / CLI / 插件只看该字段, 不看文本; 缺字段 = `Ok`
/// - `Ok` 不写是为了不给每条成功的工具消息多塞一份状态 (会话里每条消息都会进上下文)
void markToolStatus(neograph::ChatMessage& msg, agentxx::util::ToolStatus status) {
    if (status == agentxx::util::ToolStatus::Ok) {
        return;
    }
    msg.extra["status"] = std::string{agentxx::util::toolStatusName(status)};
}

/// 工具终态 -> 关键指标的工具终态 (与结果判定用同一套规则)
agentxx::util::KeyMetrics::ToolOutcome outcomeOf(agentxx::util::ToolStatus status) {
    switch (status) {
        case agentxx::util::ToolStatus::Ok:
            return agentxx::util::KeyMetrics::ToolOutcome::Ok;
        case agentxx::util::ToolStatus::Failed:
            return agentxx::util::KeyMetrics::ToolOutcome::Failed;
        case agentxx::util::ToolStatus::Interrupted:
            return agentxx::util::KeyMetrics::ToolOutcome::Interrupted;
        case agentxx::util::ToolStatus::Canceled:
            return agentxx::util::KeyMetrics::ToolOutcome::Canceled;
    }
    return agentxx::util::KeyMetrics::ToolOutcome::Ok;
}

/// 从中断缓存里的结果消息 (Json 形态) 还原终态与正文
/// - 缓存保存的是上一轮中断前已完成调用的结果消息, 正文已经格式化过, 因此直接用作正文
/// - 终态取 `extra.status` (缺失 = `Ok`)
agentxx::util::ToolResult toolResultFromCachedMessage(const std::string& cachedMessageJson) {
    agentxx::util::ToolResult result;
    try {
        auto j = utilxx_base::Json::parse(cachedMessageJson);
        if (j.is_object()) {
            result.text = j.value("content", std::string{});
            if (j.contains("extra") && j["extra"].is_object()) {
                const auto& extra = j["extra"];
                if (extra.contains("status") && extra["status"].is_string()) {
                    result.status
                        = agentxx::util::toolStatusFromName(extra["status"].get<std::string>());
                }
            }
        } else {
            result.text = cachedMessageJson;
        }
    } catch (const std::exception& e) {
        // 缓存内容异常时按成功正文透传, 不影响后续调用
        XX_LOGW("Toolcall interrupt cache is not valid JSON: {}", e.what());
        result.text = cachedMessageJson;
    }
    return result;
}

} // namespace

std::string
    ToolcallWrapNode::makeRepeatCallKey(std::string_view toolName, std::string_view arguments) {
    // key = {toolName}_{arg字符串长度}_{arg哈希值}
    // - 哈希使用进程内稳定的 std::hash; 长度参与拼接降低哈希碰撞导致
    //   不同参数被误判为相同调用的概率
    return fmt::format(
        "{}_{}_{}",
        toolName,
        arguments.size(),
        static_cast<size_t>(std::hash<std::string_view>{}(arguments))
    );
}

std::set<std::string> ToolcallWrapNode::findConsecutiveRepeatCallKeys(
    const std::vector<neograph::ChatMessage>& messages,
    size_t                                    assistantMsgIndex,
    size_t                                    threshold
) {
    auto triggered = std::set<std::string>{};
    if (messages.empty() || assistantMsgIndex >= messages.size() || 0 == threshold) {
        return triggered;
    }
    auto counts = std::map<std::string, size_t>{};
    // 从当前 assistant (带 tool_calls) 起向前回溯, 提前终止条件:
    // - user/system 等其他角色消息、不带 tool_calls 的最终回复: 打断交替链
    // - 最多回溯 threshold 条 assistant: 某 key 若真的连续出现达 threshold 次,
    //   最近 threshold 条 assistant 每条必含该 key, 更早的不可能补足缺口
    // - 某条 (非当前轮) assistant 的所有 tool_call key 均为全新 (不在已统计
    //   集合中) 时, 说明该条开启了全新调用, 重复计数至多延续到这里, 终止
    // - 任一 key 计数达到 threshold: 已确定存在循环调用, 立即返回
    size_t assistantCount = 0;
    for (int64_t i = static_cast<int64_t>(assistantMsgIndex); i >= 0; --i) {
        const auto& msg = messages[i];
        if (msg.role == "assistant") {
            if (msg.tool_calls.empty()) {
                // 不带 tool_calls 的 assistant 是最终文本回复, 打断 llm <-> tool 交替链
                break;
            }
            if (assistantCount >= threshold) {
                // 已检查满 threshold 条仍未确定循环, 更早的消息不可能再补足
                break;
            }
            ++assistantCount;
            bool allNewKeys = true;
            for (const auto& tc : msg.tool_calls) {
                auto [it, inserted]
                    = counts.emplace(makeRepeatCallKey(tc.name, tc.arguments), size_t{1});
                if (!inserted) {
                    allNewKeys = false;
                    ++(it->second);
                    if (it->second >= threshold) {
                        triggered.insert(it->first);
                    }
                } else if (threshold <= 1) {
                    triggered.insert(it->first);
                }
            }
            if (!triggered.empty()) {
                // 本轮已发现达到阈值的循环调用 (支持同轮多 key 并行触发), 立即返回
                return triggered;
            }
            if (assistantCount > 1 && allNewKeys) {
                // 非当前轮且与已统计内容零重叠: 该条开启了全新调用,
                // 重复计数至多延续到这里, 终止回溯
                break;
            }
        } else if (msg.role == "tool") {
            // tool 结果消息: 属于交替链, 继续向前回溯上一条 assistant
        } else {
            // user/system 等其他消息打断连续性: 中间有用户消息则不认为是连续
            break;
        }
    }
    return triggered;
}

/// 根据 tool 的参数 JSON Schema 自动修正参数类型兼容性 (实现见
/// [agentxx/util/tool_args_fix.h] 的 autoFixToolArgsTypes):
/// - 执行前 (工具真正拿到参数) 与展示侧 (工具行的参数文本) 共用同一份实现,
///   特化渲染器看到的参数形态与工具实际收到的完全一致
/// - 支持的转换清单见共享实现的文档注释
/// `return` 是否发生了参数转换
bool ToolcallWrapNode::autoFixArgsType(const neograph::ChatTool& def, utilxx_base::Json& args) {
    // def.parameters 为图边界类型 (neograph::json): 快照为业务 Json 后交给共享实现
    // (工具 schema 为小对象, 拷贝开销可忽略)
    return agentxx::util::autoFixToolArgsTypes(
        agentxx::util::fromNeographJson(def.parameters),
        args,
        def.name
    );
}

ToolcallWrapNode::ToolcallWrapNode(
    std::string_view                            in_name,
    const neograph::graph::NodeContext&         in_ctx,
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    WrapHandleBaseNode<neograph::graph::ToolDispatchNode>(in_name, in_agentContext, in_ctx) {
    // 拦截所有普通异常: 工具调用/中间件回调异常由 onHandle*Error 插入错误消息,
    // 图继续调度, 让 agent 基于错误消息继续运行 (工具调用失败是常态, 不应终止会话);
    // 取消/中断/取消信号 仍重抛, 由 base_agent 按控制流处理
    interceptOrdinaryError_ = true;
}

namespace {

void insertAbortedToolResults(
    std::string_view                                     phasePrefix,
    std::string_view                                     exceptionStr,
    neograph::graph::NodeInput&                          in,
    const std::shared_ptr<agentxx::agent::AgentContext>& agentCtxPtr,
    neograph::graph::NodeOutput&                         result
) noexcept {
    // 追加到会话上下文 (图状态不再持有 messages 通道): 通过节点事件通道
    // 通知 UI 与节流持久化, 与旧实现向 result.writes 挂 messages 通道写等价
    try {
        auto session = (agentCtxPtr && agentCtxPtr->sessions)
                           ? agentCtxPtr->sessions->get(in.ctx.thread_id)
                           : nullptr;
        if (!session) {
            return;
        }
        const auto&                  messages     = session->messages();
        const neograph::ChatMessage* assistantMsg = nullptr;
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (it->role == "assistant" && !it->tool_calls.empty()) {
                assistantMsg = &(*it);
                break;
            }
        }
        if (!assistantMsg || assistantMsg->tool_calls.empty()) {
            return;
        }
        // 声明内容先拷出: 追加会修改会话上下文 (引用失效)
        const auto toolCalls = assistantMsg->tool_calls;
        auto       aborted   = std::vector<neograph::ChatMessage>{};
        aborted.reserve(toolCalls.size());
        for (const auto& tool : toolCalls) {
            // 正文与终态走同一套排版 (与执行路径同一实现), 不手写 `[Error: ...]`
            auto msg = neograph::ChatMessage{
                .role = "tool",
                .content = formatToolResultText(agentxx::util::ToolResult::fail(fmt::format(
                    "{} (tool call aborted at {} phase)",
                    exceptionStr,
                    phasePrefix
                ))),
                .tool_call_id = tool.id,
                .tool_name    = tool.name,
                .flags        = neograph::MessageFlag::AutoInserted,
            };
            // 终态: 因中间件异常而中止的调用属于失败 (界面/CLI 只看该字段)
            markToolStatus(msg, agentxx::util::ToolStatus::Failed);
            aborted.push_back(std::move(msg));
        }
        agentxx::nodes::appendSessionMessages(agentCtxPtr, in, std::move(aborted));
    } catch (const std::exception& e) {
        // 异常路径插入失败不应掩盖原始异常
        XX_LOGE("Toolcall insertAbortedToolResults failed: {}", e.what());
    }
}

} // namespace

void ToolcallWrapNode::onHandleStartError(
    bool                                                errorRethrow,
    bool                                                isCurrentError,
    std::string_view                                    exceptionStr,
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    neograph::graph::NodeInput&                         in,
    neograph::graph::NodeOutput&                        result
) noexcept {
    // START 出错，不运行 execTool，直接替换插入消息，保证消息顺序正确
    if (false == errorRethrow && isCurrentError) {
        insertAbortedToolResults("Start", exceptionStr, in, agentContext.lock(), result);
    }
}

void ToolcallWrapNode::onHandleBaseRunError(
    bool                         errorRethrow,
    bool                         isCurrentError,
    std::string_view             exceptionStr,
    neograph::graph::NodeInput&  in,
    neograph::graph::NodeOutput& result
) noexcept {
    // 插入消息，保证消息顺序正确
    if (false == errorRethrow && isCurrentError) {
        insertAbortedToolResults("BaseRun", exceptionStr, in, agentContext.lock(), result);
    }
}

asio::awaitable<void> ToolcallWrapNode::onHandleStart(
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    neograph::graph::NodeInput&                         in
) {
    co_await item.onToolcallStartFunc(in);
}

asio::awaitable<void> ToolcallWrapNode::onHandleEnd(
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    const neograph::graph::NodeInput&                   in,
    neograph::graph::NodeOutput&                        result
) {
    co_await item.onToolcallEndFunc(in, result);
}

bool ToolcallWrapNode::isParallelSafeTool(const neograph::Tool& tool) {
    // 执行体显式声明并行安全才参与并发 (未声明 = 独占, 与后续调用形成顺序屏障):
    // 声明来自工具自身 —— 内置工具走 XXToolBase 构造参数, 插件工具走注册 flags,
    // 两者最终都落在 tool->extra 表上
    const auto it = tool.extra.find("supportsParallel");
    return it != tool.extra.end() && it->second == "true";
}

size_t ToolcallWrapNode::toolParallelLimit() const {
    size_t limit = 4;
    if (auto ctx = agentContext.lock(); ctx && ctx->agentConfig) {
        limit = ctx->agentConfig->toolParallelMaxConcurrency;
    }
    // 上限夹到 [1, 32]: 0/异常配置退化为串行, 避免误配出无界并发
    if (limit == 0) {
        return 1;
    }
    return std::min<size_t>(limit, 32);
}

namespace {

/// prepare 阶段的执行体无关部分: 参数类型修正 + 权限检查 + 连续重复调用确认
/// - 全程串行执行 (权限询问与 HIL 询问按模型声明顺序发生), 不写会话、不执行工具
/// - `return` 无需执行执行体时的结果 (权限拒绝 / 重复调用被用户拒绝), 状态为 `Failed`
/// - 取消 (CanceledException) 与中断 (NodeInterrupt) 照常抛出, 由调用方按控制流处理
asio::awaitable<std::optional<agentxx::util::ToolResult>> prepareToolInvocation(
    const std::shared_ptr<agentxx::agent::AgentContext>& agentCtxPtr,
    neograph::Tool*                                      tool,
    utilxx_base::Json&                                   args,
    const std::shared_ptr<neograph::graph::CancelToken>& cancelToken,
    bool                                                 repeatCallTriggered,
    std::string_view                                     repeatCallKey
) {
    (void)cancelToken;
    {
        // 参数类型自动修正: 根据 tool 参数 JSON Schema 尽量让 arg 类型匹配参数需求
        // (string<->number/bool, string->字符串数组, [单字符串数组]->string 等)
        try {
            ToolcallWrapNode::autoFixArgsType(tool->get_definition(), args);
        } catch (const std::exception& e) {
            XX_LOGW("Toolcall auto-fix arg type failed: {}", e.what());
        }
    }
    {
        // 权限检查 (经 EventBus 请求 service.permission.check, 未配置权限服务时默认放行)
        // - 拒绝理由随响应返回 (decision/reason/rule/target), 一并回给模型,
        //   便于模型与用户理解"为什么被拒绝"而不是只看到一句 permission denied
        if (agentCtxPtr && agentCtxPtr->bus) {
            auto resp
                = co_await agentCtxPtr->bus
                      ->request<events::ReqToolPermissionCheck, events::RespToolPermissionCheck>(
                          events::Topic::ToolPermissionCheck,
                          events::ReqToolPermissionCheck{
                              .agentName = agentCtxPtr->agentConfig
                                               ? agentCtxPtr->agentConfig->agentName
                                               : std::string{},
                              .sessionId = args.value("sessionId", std::string{}),
                              .toolName  = tool->get_name(),
                              .arguments = args,
                          },
                          std::chrono::milliseconds{0}
                      );
            if (resp.has_value() && !resp->allow) {
                // 拒绝原因只写内容, 不自带方括号标记: 执行层统一套 `[Error: ...]`
                if (resp->reason.empty()) {
                    co_return agentxx::util::ToolResult::fail("permission denied");
                }
                co_return agentxx::util::ToolResult::fail(
                    fmt::format("permission denied: {}", resp->reason)
                );
            }
        }
    }
    {
        // 连续重复调用检查 (repeatCallCheck 由 XXToolBase 按 tool 启用, 默认关闭):
        // - baseRun 已按 messages 的 llm <-> tool 交替链检测出触发阈值的调用
        //   (见 findConsecutiveRepeatCallKeys, 阈值
        //   [AgentConfig::toolcallRepeatCheckThreshold] 默认 5), 当前调用命中且
        //   该 tool 启用检查时发起 HIL 中断询问用户:
        //   - 首次: requestInterrupt 存储询问参数并抛出 NodeInterrupt, 由上层
        //     (agent_runner) 经总线询问外部授权者 (CLI/TUI/ACP 各自注册 interrupt 处理器)
        //   - 恢复: 图 resume 后本函数重新执行, requestInterrupt 直接返回用户响应,
        //     确认后继续执行, 拒绝则中止本次调用并插入提示消息让 LLM 改换方式;
        //     此后的重复调用会再次发起询问, 持续交由用户监督
        // - 用于拦截 LLM 陷入死循环反复以相同参数调用同一工具的情况
        // - 无 middlewareHandleContext 时无询问通道, 直接放行 (与 permission 行为一致)
        if (repeatCallTriggered && agentCtxPtr && agentCtxPtr->middlewareHandleContext) {
            const auto extraIt = tool->extra.find("repeatCallCheck");
            const bool enabled = (extraIt != tool->extra.end() && extraIt->second == "true");
            if (enabled) {
                using InterruptHandleArg = agentxx::middleware::InterruptHandleArg;
                // sessionId/tool_call_id 由 baseRun 在执行前注入 arguments
                auto sessionId = args.value("sessionId", std::string{});
                auto result    = co_await agentCtxPtr->middlewareHandleContext->requestInterrupt(
                    sessionId,
                    [&]() -> InterruptHandleArg {
                        auto arg     = InterruptHandleArg{};
                        arg.name     = "repeat_toolcall";
                        arg.resultId = args.value("tool_call_id", std::string{});
                        arg.arg      = utilxx_base::Json{
                                    {"tool_name", tool->get_name()},
                                    {"key",       repeatCallKey   },
                        };
                        // 确认卡片 (预设模板生成): 标题 + 说明 + 是/否一键按钮
                        // (结果控件 id = "allow", 取值 "true"/"false")
                        agentxx::middleware::preset::ConfirmCardOptions card;
                        card.title = fmt::format("[{}] Repeated identical call", tool->get_name());
                        card.text  = fmt::format(
                            "This tool has been called repeatedly with identical arguments "
                                "({}). Allow it to run again?",
                            repeatCallKey
                        );
                        arg.ui = agentxx::middleware::preset::confirmCard(card);
                        return arg;
                    }
                );
                // 解析用户响应: agent_runner 把结果 values 对象 (控件 id → 值)
                // 按 resultId (工具调用 id) 写回 interruptResult, 形如
                // {"<tool_call_id>": {"allow": "true"}}; 未应答/取消 = 空对象 → 拒绝
                // - 必须先按自身 resultId 逐层展开取值 (与 subagent 工具一致):
                //   直接对顶层对象取 "allow" 恒取不到, 会使用户点"允许"也被拒绝
                const auto  toolCallId = args.value("tool_call_id", std::string{});
                const auto* valuesPtr  = &result;
                if (result.is_object() && false == toolCallId.empty()) {
                    if (auto it = result.find(toolCallId); it != result.end()) {
                        valuesPtr = &(*it);
                    }
                }
                bool allow = agentxx::middleware::interruptValueBool(*valuesPtr, "allow", false);
                if (false == allow) {
                    XX_LOGD(
                        "Toolcall repeat check: deny '{}' ({})",
                        tool->get_name(),
                        repeatCallKey
                    );
                    co_return agentxx::util::ToolResult::fail(fmt::format(
                        "repeated identical call denied by the user: the tool has been executed "
                        "repeatedly with identical arguments. Change your approach or the arguments."
                    ));
                }
            }
        }
    }
    co_return std::nullopt;
}

} // namespace

asio::awaitable<bool> ToolcallWrapNode::prepareToolCall(
    const neograph::ToolCall&                            tc,
    std::string_view                                     sessionId,
    const std::shared_ptr<neograph::graph::CancelToken>& cancelToken,
    const std::set<std::string>&                         repeatTriggeredKeys,
    const std::map<std::string, std::string>&            toolcallsCache,
    PreparedToolCall&                                    out
) const {
    auto agentCtxPtr = agentContext.lock();

    out             = PreparedToolCall{};
    out.id          = tc.id;
    out.name        = tc.name;
    out.cancelToken = cancelToken;
    out.startMs     = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch()
    )
                                           .count());
    out.repeatKey   = makeRepeatCallKey(tc.name, tc.arguments);
    out.repeatHit   = repeatTriggeredKeys.count(out.repeatKey) > 0;

    // 中断缓存: 上次中断前已完成的结果直接复用 (不再执行, 也不再询问)
    // - 缓存里是 Json 形态的消息 (含 extra.status): 还原出终态与正文
    if (auto cacheIt = toolcallsCache.find(tc.id); cacheIt != toolcallsCache.end()) {
        out.shortCircuit = toolResultFromCachedMessage(cacheIt->second);
        co_return false;
    }

    // 执行体查找: 静态工具列表优先, 未命中再查动态插件工具注册表
    // - 插件工具以 shared_ptr 持有 (与插件 inflight 计数配合: 卸载等待归零后
    //   才 dlclose, 执行期间代码段保持有效)
    auto it = std::find_if(tools_.begin(), tools_.end(), [&](neograph::Tool* t) {
        return t->get_name() == tc.name;
    });
    if (it != tools_.end()) {
        out.tool = *it;
    } else {
        auto pluginTool = (agentCtxPtr && agentCtxPtr->toolRegistry)
                              ? agentCtxPtr->toolRegistry->find(tc.name)
                              : nullptr;
        if (!pluginTool) {
            out.shortCircuit = formattedShortCircuit(
                agentxx::util::ToolResult::fail(fmt::format("tool not found: {}", tc.name))
            );
            co_return false;
        }
        out.tool      = pluginTool.get();
        out.keepAlive = std::move(pluginTool);
    }

    // 参数: 解析模型给出的 JSON 后注入会话标识 (与旧实现一致)
    // - 解析失败按"本次调用失败"处理 (结果消息回给模型), 不影响同批其他调用
    utilxx_base::Json args;
    try {
        args = utilxx_base::Json::parse(tc.arguments);
    } catch (const std::exception& e) {
        out.shortCircuit = formattedShortCircuit(agentxx::util::ToolResult::fail(e.what()));
        co_return false;
    }
    if (args.is_object()) {
        args["sessionId"]    = std::string{sessionId};
        args["tool_call_id"] = tc.id;
    }
    out.args         = std::move(args);
    out.parallelSafe = isParallelSafeTool(*out.tool);

    // 参数修正 + 权限检查 + 重复调用确认 (可能挂起等待用户, 全程串行)
    auto shortCircuit = co_await prepareToolInvocation(
        agentCtxPtr,
        out.tool,
        out.args,
        cancelToken,
        out.repeatHit,
        out.repeatKey
    );
    if (shortCircuit.has_value()) {
        out.shortCircuit = formattedShortCircuit(std::move(*shortCircuit));
        co_return false;
    }

    // 执行前目标复验: 判定阶段记录过"已批准目标", 这里用最终参数
    // 按同一套规则再解析一次并比对 —— 判定与执行之间的任何改写都会在这里被拦下
    // (纯校验, 不重复询问用户; 服务未注册/无调用 id 时视为通过)
    if (agentCtxPtr && agentCtxPtr->bus && out.tool && !tc.id.empty()) {
        auto resp = co_await agentCtxPtr->bus
                        ->request<events::ReqPermissionReverify, events::RespPermissionReverify>(
                            events::Topic::PermissionReverify,
                            events::ReqPermissionReverify{
                                .sessionId  = std::string{sessionId},
                                .toolName   = out.tool->get_name(),
                                .toolCallId = tc.id,
                                .arguments  = out.args,
                            },
                            std::chrono::milliseconds{0}
                        );
        if (resp.has_value() && !resp->ok) {
            XX_LOGW(
                "Toolcall permission reverify failed: tool='{}' id='{}' reason={}",
                out.tool->get_name(),
                tc.id,
                resp->reason
            );
            out.shortCircuit = formattedShortCircuit(agentxx::util::ToolResult::fail(
                fmt::format("permission denied: {}", resp->reason)
            ));
            co_return false;
        }
    }
    co_return true;
}

asio::awaitable<agentxx::util::ToolResult>
    ToolcallWrapNode::runToolCallBody(const PreparedToolCall& prepared) const {
    auto*       tool = prepared.tool;
    const auto& args = prepared.args;

    // 权限检查/重复确认可能 co_await 挂起过, 执行 tool 前检查取消检查点
    if (prepared.cancelToken) {
        prepared.cancelToken->throw_if_cancelled("before tool execution");
    }

    size_t maxRetry = 0;
    {
        // 用 find 读取 (operator[] 会在共享的 tool 定义 map 上插入缺失键,
        // 查询不该改状态)
        auto it     = tool->extra.find("maxRetry");
        auto str    = (it == tool->extra.end()) ? std::string{} : it->second;
        auto result = utilxx_base::parseNumberFromString(str, maxRetry);
        if (result.ec != std::errc{}) {
            maxRetry = 0;
        }
    }

    agentxx::util::ToolResult result;
    size_t                     retry = 0;
    do {
        bool               isCancel = false;
        std::string        errInfo;
        std::exception_ptr errorPtr;

        try {
            if (auto* xxTool = dynamic_cast<agentxx::tools::XXToolBase*>(tool)) {
                result = co_await xxTool->execute_async(args);
            } else {
                // 原生 neograph::Tool (如未包装的 MCP 原样转发工具): 经桥接调用
                // 原生工具没有状态通道, 文本一律按成功
                auto neoArgs = agentxx::util::toNeographJson(args);
                result = agentxx::util::ToolResult::ok(co_await tool->execute_async(neoArgs));
            }
            break;
        } catch (const neograph::graph::CancelledException&) {
            isCancel = true;
            errorPtr = std::current_exception();
        } catch (const neograph::graph::NodeInterrupt&) {
            isCancel = true;
            errorPtr = std::current_exception();
        } catch (const boost::exception& e) {
            // boost::exception 在 std::exception 之前捕获, 保留完整诊断信息
            errInfo  = utilxx_base::autoTryConvertToUtf8(boost::diagnostic_information(e));
            errorPtr = std::current_exception();
        } catch (const std::exception& e) {
            errInfo  = utilxx_base::autoTryConvertToUtf8(e.what());
            errorPtr = std::current_exception();
        } catch (...) {
            errorPtr = std::current_exception();
        }

        // 触发异常
        if (retry >= maxRetry || isCancel) {
            std::rethrow_exception(errorPtr);
        }
        XX_LOGD("ToolCallNode {} retry: {}/{} | {}", tool->get_name(), retry, maxRetry, errInfo);
        retry++;
    } while (true);

    co_return result;
}

asio::awaitable<agentxx::util::ToolResult> ToolcallWrapNode::finalizeToolCall(
    const PreparedToolCall&          prepared,
    agentxx::util::ToolResult        rawResult
) const {
    auto agentCtxPtr = agentContext.lock();
    if (!agentCtxPtr || !agentCtxPtr->agentConfig) {
        co_return std::move(rawResult);
    }
    auto*       tool   = prepared.tool;
    const auto& args   = prepared.args;
    std::string result = std::move(rawResult.text);

    const size_t limitLength = agentCtxPtr->agentConfig->toolcallSummaryLimitOutputLength;
    // 用 find 读取 (operator[] 会插入缺失键); 缺失 = 未启用压缩
    const auto autoSummaryIt = tool->extra.find("autoSummaryOutput");
    const bool autoSummary
        = (autoSummaryIt != tool->extra.end() && autoSummaryIt->second == "true");
    if (autoSummary && result.size() >= limitLength) {
        const auto session_id = args.value("sessionId", std::string{});
        assert(false == session_id.empty());
        // 超过限制长度: 原文存入 share store, 结果只留开头片段与取回提示
        // (判定/提示格式与 agent 队列输入压缩共用, 见 offloadShareStoreIfOversized)
        auto offload = agentxx::util::offloadShareStoreIfOversized(
            agentCtxPtr,
            session_id,
            result,
            limitLength
        );
        if (offload.offloaded) {
            rawResult.text = std::move(offload.text);
            co_return std::move(rawResult);
        }
    }
    rawResult.text = std::move(result);
    co_return std::move(rawResult);
}

asio::awaitable<std::optional<neograph::ChatMessage>> ToolcallWrapNode::runPreparedToolCall(
    const PreparedToolCall& prepared,
    std::exception_ptr*     outError,
    bool*                   outInterrupted
) const {
    // 节点/agent 已释放 (协程被并发调度后上下文消失): 不产出结果
    if (agentContext.expired()) {
        co_return std::nullopt;
    }

    neograph::ChatMessage tool_msg;
    tool_msg.role                 = "tool";
    tool_msg.tool_call_id         = prepared.id;
    tool_msg.tool_name            = prepared.name;
    tool_msg.extra["startTimeMs"] = prepared.startMs;

    if (prepared.shortCircuit.has_value()) {
        // 无需执行: 中断缓存命中 / 工具不存在 / 权限拒绝 / 重复调用被拒 / 中断占位
        // - `shortCircuit.text` 在产生处已按状态排版 (见 formattedShortCircuit),
        //   缓存命中项存的本来就是上一轮的结果正文, 这里原样写入, 不再二次包裹
        const auto& cached = *prepared.shortCircuit;
        tool_msg.content   = cached.text;
        if (prepared.interrupted) {
            tool_msg.flags |= neograph::MessageFlag::Interrupt;
        }
        markToolStatus(tool_msg, cached.status);
        tool_msg.extra["durationMs"] = 0;
        // 关键指标: 未执行即定案 (中断占位记 Interrupted, 其余按状态计)
        if (auto ctx = agentContext.lock(); ctx && ctx->metrics) {
            ctx->metrics->noteToolCall(
                prepared.interrupted ? agentxx::util::KeyMetrics::ToolOutcome::Interrupted
                                     : outcomeOf(cached.status)
            );
        }
        co_return tool_msg;
    }

    bool                      ok     = false;
    agentxx::util::ToolResult result;
    co_await agentxx::util::catchErrorAsync<bool>(
        [&]() -> asio::awaitable<bool> {
            try {
                result = co_await runToolCallBody(prepared);
                result = co_await finalizeToolCall(prepared, std::move(result));
                // 取消检查点: tool 执行完成后检查, 避免取消后继续收集/执行后续 tool
                if (prepared.cancelToken) {
                    prepared.cancelToken->throw_if_cancelled("after tool execution");
                }
                ok = true;
            } catch (const neograph::graph::CancelledException&) {
                // 取消属于控制流: 记录后由调用方按取消路径收尾
                if (outError) {
                    *outError = std::current_exception();
                }
                if (auto ctx = agentContext.lock(); ctx && ctx->metrics) {
                    ctx->metrics->noteToolCall(agentxx::util::KeyMetrics::ToolOutcome::Canceled);
                }
            } catch (const neograph::graph::NodeInterrupt&) {
                // tool 触发中断: 只标记本调用, 中断参数由上层在恢复时按 resultId 取
                if (outInterrupted) {
                    *outInterrupted = true;
                }
                tool_msg.flags   |= neograph::MessageFlag::Interrupt;
                result            = agentxx::util::ToolResult::interrupted();
                if (auto ctx = agentContext.lock(); ctx && ctx->metrics) {
                    ctx->metrics->noteToolCall(agentxx::util::KeyMetrics::ToolOutcome::Interrupted);
                }
            }
            co_return true;
        },
        [&](std::string errinfo) -> asio::awaitable<bool> {
            // 执行期异常 = 本次调用失败: 文本只存错误内容, 前缀由下方统一加
            // (插件工具以 FAILED 终态报错已在插件边界翻成 ToolResult, 走不到这里)
            result = agentxx::util::ToolResult::fail(std::move(errinfo));
            ok     = true;
            co_return true;
        },
        // 控制流异常保护 (取消/中断): 不由本函数向外抛 —— 派生协程里逃逸的异常
        // 会走到 detached 处理器并终止进程; 顺序路径也会跳过调用方的取消收尾。
        // 这里统一记为"本次调用被取消", 由调用方补 [Canceled] 占位
        [&](std::string& errinfo) -> std::optional<bool> {
            if (outError) {
                *outError = std::make_exception_ptr(neograph::graph::CancelledException(errinfo));
            }
            return std::optional<bool>{true};
        },
        // 传入取消令牌: tool 被取消信号中断产生的 operation_aborted 转换为
        // CanceledException, 避免取消被当作普通 tool 错误吞掉
        prepared.cancelToken
    );

    if (outError && *outError) {
        // 取消: 不产出结果 (由调用方补 [Canceled] 占位并保留已完成结果)
        co_return std::nullopt;
    }
    // 结果定稿: 正文格式化 (只有 Failed 加 `[Error...]`) + 状态位写进消息
    // - 中断/取消占位在 `text` 为空时由 [formatToolResultText] 补 `[Interrupt]` / `[Canceled]`
    tool_msg.content = formatToolResultText(result);
    markToolStatus(tool_msg, result.status);
    if (ok) {
        // 关键指标: 按终态统计 (失败/取消/中断不再算成功)
        if (auto ctx = agentContext.lock(); ctx && ctx->metrics) {
            ctx->metrics->noteToolCall(outcomeOf(result.status));
        }
    }
    if (ok) {
        const auto endMs
            = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch()
            )
                                       .count());
        tool_msg.extra["durationMs"] = std::max<int64_t>(0, endMs - prepared.startMs);
    } else {
        tool_msg.extra["durationMs"] = 0;
    }
    co_return tool_msg;
}

asio::awaitable<void> ToolcallWrapNode::runPreparedToolCallGuarded(
    const PreparedToolCall&               prepared,
    std::optional<neograph::ChatMessage>& outMsg,
    std::exception_ptr&                   outError,
    bool&                                 outInterrupted
) const {
    // 工具执行事件源: 每次调用开始/结束各发一次 (没人订阅/没人等时零开销,
    // 见 publishToolEvent)。放在本函数而不是批处理循环里, 顺序与并发两条路径都能覆盖
    const auto toolSessionId = prepared.args.is_object()
                                   ? prepared.args.value("sessionId", std::string{})
                                   : std::string{};
    if (auto ctx = agentContext.lock()) {
        co_await publishToolEvent(
            ctx,
            toolSessionId,
            prepared.name,
            prepared.id,
            /*isStart=*/true,
            agentxx::util::ToolStatus::Ok,
            /*resultBytes=*/0
        );
    }
    try {
        auto msg = co_await runPreparedToolCall(prepared, &outError, &outInterrupted);
        if (msg.has_value()) {
            outMsg = std::move(*msg);
        }
    } catch (const neograph::graph::NodeInterrupt&) {
        // 中断逃逸到此 (内层已按中断语义处理大部分情况): 保持中断语义
        outInterrupted = true;
        neograph::ChatMessage msg;
        msg.role                 = "tool";
        msg.tool_call_id         = prepared.id;
        msg.tool_name            = prepared.name;
        msg.content              = "[Interrupt]";
        msg.flags               |= neograph::MessageFlag::Interrupt;
        msg.extra["startTimeMs"] = prepared.startMs;
        msg.extra["durationMs"]  = 0;
        markToolStatus(msg, agentxx::util::ToolStatus::Interrupted);
        outMsg = std::move(msg);
    } catch (...) {
        // 取消 (含 asio 取消产生的 operation_aborted) 与其它逃逸异常: 一律记为取消,
        // 由调用方补 [Canceled] 占位并保留已完成结果
        if (outError == nullptr) {
            outError
                = std::make_exception_ptr(neograph::graph::CancelledException("tool call aborted"));
        }
    }

    // 工具执行结束: 发布 `tool.end` / `tool.end.<工具名>`
    // - 只带元信息 (终态 + 结果字节数), 不带结果正文: 一个工具的完整输出可能
    //   很大, 需要正文就让它自己 emit 到指定事件
    if (auto ctx = agentContext.lock()) {
        auto                                status = agentxx::util::ToolStatus::Ok;
        size_t                              bytes  = 0;
        if (outMsg.has_value()) {
            bytes = outMsg->content.size();
            if (outMsg->extra.is_object() && outMsg->extra.contains("status")) {
                status = agentxx::util::toolStatusFromName(
                    outMsg->extra["status"].get<std::string>()
                );
            }
        } else {
            // 没有结果 = 取消/中断路径
            status = outInterrupted ? agentxx::util::ToolStatus::Interrupted
                                    : agentxx::util::ToolStatus::Canceled;
        }
        co_await publishToolEvent(
            ctx,
            toolSessionId,
            prepared.name,
            prepared.id,
            /*isStart=*/false,
            status,
            bytes
        );
    }
    co_return;
}

asio::awaitable<void> ToolcallWrapNode::runToolBatch(
    const std::vector<PreparedToolCall>&               batch,
    std::vector<std::optional<neograph::ChatMessage>>& outMsgs,
    std::exception_ptr&                                outError,
    bool&                                              outInterrupted
) const {
    outMsgs.assign(batch.size(), std::nullopt);
    outError       = nullptr;
    outInterrupted = false;
    if (batch.empty()) {
        co_return;
    }

    // 单条调用: 直接顺序执行 (不额外派生协程, 省一次调度)
    if (batch.size() == 1) {
        co_await runPreparedToolCallGuarded(batch[0], outMsgs[0], outError, outInterrupted);
        co_return;
    }

    // 多条: 派生为独立协程并发推进 (单 io 线程上交错执行, 不引入线程锁)
    // - 结果按 batch 内槽位写回: 完成顺序与提交顺序无关
    // - batch 复制进共享状态: 派生的协程不引用本协程栈上的数据
    auto ex = co_await asio::this_coro::executor;

    struct BatchState {
        std::vector<PreparedToolCall>                     batch;
        std::vector<std::optional<neograph::ChatMessage>> msgs;
        std::exception_ptr                                error;
        bool                                              interrupted = false;
    };

    auto state   = std::make_shared<BatchState>();
    state->batch = batch;
    state->msgs.assign(batch.size(), std::nullopt);
    auto doneChannel
        = std::make_shared<asio::experimental::channel<void(utilxx_base::AsioErrorCode, size_t)>>(
            ex,
            static_cast<unsigned>(batch.size())
        );
    // 本批次的取消信号: 会话取消时先向子协程发信号 (让它们的挂起等待尽快结束),
    // 再按取消路径返回, 不留下后台仍在推进的工具执行
    //
    // 每个子协程**各自一个** signal: asio 的 cancellation_signal 同一时刻只容纳
    // 一个已安装的取消处理器, 多个并发协程共用一个 signal 会互相覆盖对方的处理器,
    // 协程收尾 (clear_cancellation_slot) 时会按残留的处理器指针去释放内存 ——
    // 实测为野指针释放 (Release 下 malloc abort / ASan 报 bad-free)
    std::vector<std::shared_ptr<asio::cancellation_signal>> batchSignals;
    batchSignals.reserve(state->batch.size());
    for (size_t i = 0; i < state->batch.size(); ++i) {
        batchSignals.push_back(std::make_shared<asio::cancellation_signal>());
    }

    for (size_t i = 0; i < state->batch.size(); ++i) {
        auto signal = batchSignals[i];
        asio::co_spawn(
            ex,
            [this, state, doneChannel, signal, i]() -> asio::awaitable<void> {
                // RAII 守卫: 无论单条调用如何退出都保证发送完成信号 (父协程不悬挂)
                struct DoneGuard {
                    std::shared_ptr<
                        asio::experimental::channel<void(utilxx_base::AsioErrorCode, size_t)>>
                           ch;
                    size_t idx;

                    ~DoneGuard() {
                        if (ch) {
                            ch->async_send(
                                utilxx_base::AsioErrorCode{},
                                idx,
                                [](utilxx_base::AsioErrorCode) {}
                            );
                        }
                    }
                } guard{doneChannel, i};

                // 派生协程内绝不允许异常逃逸 (detached 处理器的未捕获异常会终止进程):
                // 取消/中断等控制流异常记入共享状态, 其余记为该调用的错误结果
                co_await runPreparedToolCallGuarded(
                    state->batch[i],
                    state->msgs[i],
                    state->error,
                    state->interrupted
                );
            },
            asio::bind_cancellation_slot(signal->slot(), asio::detached)
        );
    }
    // 等待全部完成 (接收 n 次)
    // - 会话取消会让节点协程上挂起的等待以 operation_aborted 结束 (as_tuple 把
    //   错误作为结果返回, 不抛异常): 此时停止等待, 保留已完成结果, 让调用方按
    //   取消路径补齐 [Canceled] 占位 (未完成的槽位保持空)
    bool waitCanceled = false;
    for (size_t i = 0; i < state->batch.size(); ++i) {
        auto [ec, idx] = co_await doneChannel->async_receive(asio::as_tuple(asio::use_awaitable));
        (void)idx;
        if (ec) {
            waitCanceled = true;
            break;
        }
    }
    if (waitCanceled) {
        // 让子协程的挂起等待尽快结束 (避免取消后仍在后台推进工具执行)
        for (const auto& signal : batchSignals) {
            signal->emit(asio::cancellation_type::all);
        }
        outMsgs = state->msgs;
        if (outError == nullptr) {
            outError = std::make_exception_ptr(
                neograph::graph::CancelledException("Canceled while waiting tool batch")
            );
        }
        XX_LOGD(
            "Toolcall batch wait Canceled: {} of {} call(s) already finished",
            [&] {
                size_t done = 0;
                for (const auto& m : outMsgs) {
                    if (m.has_value()) {
                        ++done;
                    }
                }
                return done;
            }(),
            outMsgs.size()
        );
        co_return;
    }

    outMsgs        = std::move(state->msgs);
    outError       = state->error;
    outInterrupted = state->interrupted;
    co_return;
}

asio::awaitable<agentxx::util::ToolResult> ToolcallWrapNode::execTool(
    neograph::Tool*                                      tool,
    utilxx_base::Json&                                   args,
    const std::shared_ptr<neograph::graph::CancelToken>& cancelToken,
    bool                                                 repeatCallTriggered,
    std::string_view                                     repeatCallKey
) const {
    // 单次调用的完整路径 (prepare → run → finalize), 顺序执行时使用
    auto agentCtxPtr = agentContext.lock();

    PreparedToolCall prepared;
    prepared.tool        = tool;
    prepared.args        = args;
    prepared.cancelToken = cancelToken;
    prepared.repeatKey   = std::string{repeatCallKey};
    prepared.repeatHit   = repeatCallTriggered;

    auto shortCircuit = co_await prepareToolInvocation(
        agentCtxPtr,
        tool,
        prepared.args,
        cancelToken,
        repeatCallTriggered,
        repeatCallKey
    );
    if (shortCircuit.has_value()) {
        // 参数被修正过也要写回 (调用方可能继续使用 args)
        args = prepared.args;
        co_return formattedShortCircuit(std::move(*shortCircuit));
    }
    auto raw = co_await runToolCallBody(prepared);
    args     = prepared.args;
    co_return co_await finalizeToolCall(prepared, std::move(raw));
}

asio::awaitable<void> ToolcallWrapNode::baseRun(
    std::vector<std::shared_ptr<agentxx::middleware::BaseMiddlewareHandleInterface>>& handles,
    neograph::graph::NodeInput&                                                       in,
    neograph::graph::NodeOutput&                                                      out
) {
    auto agentCtxPtr = agentContext.lock();
    if (!agentCtxPtr || !agentCtxPtr->middlewareHandleContext) {
        // agent 上下文已释放 (主程序销毁/会话卸载过程中): 没有可用的中间件上下文,
        // 直接结束本节点, 与同文件其它位置的防御保持一致
        out = {};
        co_return;
    }

    auto toolcallsCache = std::map<std::string, std::string>{};
    {
        auto toolcallsCacheJson
            = agentCtxPtr->middlewareHandleContext->getGraphDataItemValue<utilxx_base::Json>(
                in.ctx.thread_id,
                agentxx::middleware::MiddlewareContext::graphDataKey_interruptToolcallCache
            );
        agentCtxPtr->middlewareHandleContext->removeGraphDataItem(
            in.ctx.thread_id,
            agentxx::middleware::MiddlewareContext::graphDataKey_interruptToolcallCache
        );
        if (toolcallsCacheJson.is_array()) {
            for (const auto& item : toolcallsCacheJson) {
                neograph::ChatMessage msg;
                auto                  neoItem = agentxx::util::toNeographJson(item);
                neograph::from_json(neoItem, msg);
                if (false == msg.tool_call_id.empty()
                    && false == neograph::hasFlag(msg.flags, neograph::MessageFlag::Interrupt)) {
                    toolcallsCache[msg.tool_call_id] = std::move(msg.content);
                }
            }
        }
    }

    auto session = (agentCtxPtr && agentCtxPtr->sessions)
                       ? agentCtxPtr->sessions->getOrCreate(in.ctx.thread_id)
                       : nullptr;
    if (!session || session->messages().empty()) {
        out = neograph::graph::NodeOutput{};
        co_return;
    }
    // 上下文以会话为唯一权威; 本节点只读 (工具结果在成功路径统一追加)
    const auto& messages = session->messages();

    // Find the last assistant message with tool_calls
    const neograph::ChatMessage* assistantMsg      = nullptr;
    size_t                       assistantMsgIndex = 0;
    for (size_t i = messages.size(); i > 0; --i) {
        const auto& msg = messages[i - 1];
        if (msg.role == "assistant" && !msg.tool_calls.empty()) {
            assistantMsg      = &msg;
            assistantMsgIndex = i - 1;
            break;
        }
    }
    if (!assistantMsg) {
        out = neograph::graph::NodeOutput{};
        co_return;
    }
    // 声明内容先拷出: 后备插入 (取消/异常) 会追加会话上下文, 使原引用失效
    const std::vector<neograph::ToolCall> declaredToolCalls = assistantMsg->tool_calls;

// debug 检查警告: 若该 assistant 声明的 tool_calls 在其之后已全部有 tool 结果消息,
// 说明这些工具已执行过, 本节点被重复调度
// - 正常流程不会发生 (工具执行后 llm 重新调用, 末尾变为新的 assistant 消息)
// - 出现时说明存在异常路径 (如异常被吞掉后 [xx_has_tool_calls] 误路由), 记录
//   警告日志供排查; 不跳过执行, 避免掩盖真正的问题
#if XX_IS_DEBUG_D
    {
        const int64_t         assistantIndex = static_cast<int64_t>(assistantMsgIndex);
        std::set<std::string> replied;
        for (size_t i = static_cast<size_t>(assistantIndex) + 1; i < messages.size(); ++i) {
            if (messages[i].role == "tool" && false == messages[i].tool_call_id.empty()) {
                replied.insert(messages[i].tool_call_id);
            }
        }
        const bool allReplied = std::all_of(
            assistantMsg->tool_calls.begin(),
            assistantMsg->tool_calls.end(),
            [&](const neograph::ToolCall& tc) {
                return false == tc.id.empty() && replied.count(tc.id) > 0;
            }
        );
        if (allReplied) {
            XX_LOGW(
                "Toolcall re-execute check: last assistant tool_calls already fully replied ({} tools); abnormal scheduling suspected",
                assistantMsg->tool_calls.size()
            );
        }
    }
#endif

    bool isInterrupt   = false;
    bool isCancel      = false;
    auto interruptArgs = std::map<std::string, utilxx_base::Json>{};
    auto results       = utilxx_base::Json::array();
    // 本轮工具结果的 typed 形态 (追加进会话上下文用); results 为 Json 形态,
    // 供中断缓存 (graphDataKey_interruptToolcallCache) 与日志使用
    std::vector<neograph::ChatMessage> typedResults{};
    // 已执行完成的 tool_call_id (取消时用于区分已完成/未完成, 未完成的补 [Canceled])
    std::set<std::string> completedToolcallIds{};
    std::exception_ptr    cancelErrorPtr;

    std::set<std::string> repeatTriggeredKeys;
    if (nullptr != assistantMsg) {
        // 连续重复调用检测: 以当前 assistant 结尾的 llm <-> tool 交替链内,
        // 达到阈值 [AgentConfig::toolcallRepeatCheckThreshold] 的重复调用 key 集合
        // (key = {toolName}_{arg长度}_{arg哈希}); 供 execTool 对启用
        // [repeatCallCheck] 且命中的 tool 发起用户确认; 阈值 <= 0 时禁用检查
        const size_t repeatThreshold = (agentCtxPtr && agentCtxPtr->agentConfig)
                                           ? agentCtxPtr->agentConfig->toolcallRepeatCheckThreshold
                                           : 0;
        repeatTriggeredKeys
            = findConsecutiveRepeatCallKeys(messages, assistantMsgIndex, repeatThreshold);
    }

    // ---- 工具调用执行: prepare (串行) → run (受限并发) → finalize (串行) ----
    //
    // - prepare/finalize 串行: 权限询问与重复调用确认按模型声明顺序发生, 询问与
    //   HIL 语义与顺序执行时完全一致
    // - run 只对**声明并行安全**的执行体并发 (见 XXToolBase::supportsParallel):
    //   连续的可并行调用合成一批 (上限 [AgentConfig::toolParallelMaxConcurrency]),
    //   独占调用单独成批, 与前后调用形成顺序屏障 —— 写文件、命令执行、交互询问
    //   仍保持模型声明的先后关系
    // - 结果始终按 [declaredToolCalls] 顺序写回: 并发完成顺序不影响提交顺序;
    //   取消时已完成结果保留, 未完成补 [Canceled] 占位 (见下方取消分支)
    auto preparedCalls = std::vector<PreparedToolCall>{};
    preparedCalls.reserve(declaredToolCalls.size());
    for (const auto& tc : declaredToolCalls) {
        PreparedToolCall   prepared;
        std::exception_ptr prepError;
        bool               prepInterrupted = false;
        co_await agentxx::util::catchErrorAsync<bool>(
            [&]() -> asio::awaitable<bool> {
                try {
                    co_return co_await prepareToolCall(
                        tc,
                        in.ctx.thread_id,
                        in.ctx.cancel_token,
                        repeatTriggeredKeys,
                        toolcallsCache,
                        prepared
                    );
                } catch (const neograph::graph::CancelledException&) {
                    prepError = std::current_exception();
                } catch (const neograph::graph::NodeInterrupt&) {
                    // 重复调用确认触发中断: 本调用留 [Interrupt] 占位且不执行,
                    // 其余调用照常执行 (与旧实现一致: 结果与占位一起进中断缓存,
                    // 恢复时已完成的结果复用, 中断的那条重新执行)
                    prepInterrupted = true;
                } catch (const std::exception& e) {
                    // 其他异常: 会话已取消 (如 asio 取消导致的 operation_aborted)
                    // 按取消处理, 否则作为本次调用的错误结果回给模型
                    if (in.ctx.cancel_token && in.ctx.cancel_token->is_cancelled()) {
                        prepError = std::current_exception();
                    } else {
                        prepared.shortCircuit
                            = formattedShortCircuit(agentxx::util::ToolResult::fail(e.what()));
                    }
                }
                co_return false;
            },
            [&](std::string errinfo) -> asio::awaitable<bool> {
                // 参数解析/权限服务等普通异常: 作为本次调用的结果, 不影响同批其他调用
                prepared.shortCircuit
                    = formattedShortCircuit(agentxx::util::ToolResult::fail(errinfo));
                co_return true;
            },
            // 控制流异常保护 (取消/中断): 记为取消, 由调用方按取消路径收尾
            [&](std::string& errinfo) -> std::optional<bool> {
                prepError = std::make_exception_ptr(neograph::graph::CancelledException(errinfo));
                return std::optional<bool>{true};
            },
            in.ctx.cancel_token
        );
        if (prepError) {
            // 取消: 停止准备, 未执行的调用由下方统一补占位
            isCancel       = true;
            cancelErrorPtr = prepError;
            break;
        }
        if (prepInterrupted) {
            prepared.interrupted  = true;
            // 中断占位也要排版: 空文本会被补成 `[Interrupt]`, 让中断缓存里的这条
            // 消息结构完整 (内容非空) 且一眼能看出是哪一条
            prepared.shortCircuit = formattedShortCircuit(
                agentxx::util::ToolResult::interrupted()
            );
            isInterrupt           = true;
        }
        preparedCalls.push_back(std::move(prepared));
    }

    if (false == isCancel) {
        // ---- 分批执行: 批内并发 (仅并行安全调用), 批间按声明顺序串行 ----
        //
        // 整个执行段包在 [catchErrorAsync] 里: 取消可能从任一 await 点逃出来 (工具
        // 自身 / 事件发布 / 结果卸载), 而"未完成的调用补占位"必须结构上一定执行 ——
        // 这条路径一旦被跳过, 上下文就留下没有结果的 tool_calls (下一次请求会被
        // LLM API 拒绕)。已完成的结果保留, 逃逸的取消在这里转成取消收尾
        co_await agentxx::util::catchErrorAsync<bool>(
            [&]() -> asio::awaitable<bool> {
                const size_t parallelLimit = toolParallelLimit();
                size_t       index         = 0;
                while (index < preparedCalls.size()) {
                    std::vector<PreparedToolCall> batch;
                    batch.reserve(parallelLimit);
                    batch.push_back(preparedCalls[index]);
                    const bool collectMore = preparedCalls[index].parallelSafe;
                    ++index;
                    // 只把**连续**的并行安全调用合批: 中间出现独占工具即断批, 保证写/交互
                    // 类工具与其前后调用的先后顺序
                    if (collectMore) {
                        while (index < preparedCalls.size() && preparedCalls[index].parallelSafe
                               && batch.size() < parallelLimit) {
                            batch.push_back(preparedCalls[index]);
                            ++index;
                        }
                    }

                    std::vector<std::optional<neograph::ChatMessage>> batchMsgs;
                    std::exception_ptr                                batchError;
                    bool                                              batchInterrupted = false;
                    co_await runToolBatch(batch, batchMsgs, batchError, batchInterrupted);
                    if (batchInterrupted) {
                        isInterrupt = true;
                    }
                    for (auto& msg : batchMsgs) {
                        if (false == msg.has_value()) {
                            continue;
                        }
                        neograph::json neoJson;
                        neograph::to_json(neoJson, *msg);
                        results.push_back(agentxx::util::fromNeographJson(neoJson));
                        completedToolcallIds.insert(msg->tool_call_id);
                        typedResults.push_back(std::move(*msg));
                    }
                    if (batchError) {
                        // - 取消: 停止执行后续批次, 由下方补齐未完成 tool 的取消提示消息
                        // - 已完成批次的工具结果保留 (会话是唯一权威, 不受图状态回滚影响)
                        isCancel       = true;
                        cancelErrorPtr = batchError;
                        break;
                    }
                }
                co_return true;
            },
            [&](std::string errinfo) -> asio::awaitable<bool> {
                // 普通异常同样按取消收尾: 上下文完整性优先 (已抛出的错误在下面重新抛出)
                isCancel       = true;
                cancelErrorPtr = std::make_exception_ptr(
                    neograph::graph::CancelledException(std::move(errinfo))
                );
                co_return true;
            },
            [&](std::string errinfo) -> std::optional<bool> {
                if (isInterrupt) {
                    // 中断: 保持中断语义, 原异常原样重抛 (结果已进中断缓存, 恢复时复用)
                    return std::optional<bool>{};
                }
                isCancel       = true;
                cancelErrorPtr = std::make_exception_ptr(
                    neograph::graph::CancelledException(std::move(errinfo))
                );
                return std::optional<bool>{true};
            },
            in.ctx.cancel_token
        );
    }

    if (isCancel) {
        // - 取消后重新从图开始节点执行 (与中断不同, 中断会 resume 到本节点恢复,
        //   取消不会), 因此本轮的 toolcall 结果直接追加到会话上下文 —— 会话是
        //   唯一权威, 不受图状态回滚影响, 已完成的 tool 结果不会丢失
        // - 未完成的 tool 插入 [Canceled] 提示, 保证每条 assistant tool_call
        //   都有对应的 tool 结果消息, 上下文角色顺序和内容完整
        XX_LOGD(
            "Toolcall cancel path: declared={} completed={} results={}",
            declaredToolCalls.size(),
            completedToolcallIds.size(),
            typedResults.size()
        );
        for (const auto& tc : declaredToolCalls) {
            if (completedToolcallIds.count(tc.id)) {
                continue;
            }
            neograph::ChatMessage tool_msg;
            tool_msg.role         = "tool";
            tool_msg.tool_call_id = tc.id;
            tool_msg.tool_name    = tc.name;
            tool_msg.content      = "[Canceled]";
            tool_msg.flags        = neograph::MessageFlag::AutoInserted;
            markToolStatus(tool_msg, agentxx::util::ToolStatus::Canceled);
            typedResults.push_back(std::move(tool_msg));
        }
        agentxx::nodes::appendSessionMessages(agentCtxPtr, in, std::move(typedResults), nodeName);
        // 往外抛 cancel 异常, 由 WrapNode 统一按控制流处理
        std::rethrow_exception(cancelErrorPtr);
    }

    if (isInterrupt) {
        // 暂存 toolcall list 结果到 graphData
        agentCtxPtr->middlewareHandleContext->setGraphDataItemValue<utilxx_base::Json>(
            in.ctx.thread_id,
            agentxx::middleware::MiddlewareContext::graphDataKey_interruptToolcallCache,
            results
        );
        // 重新抛出异常 (中断恢复不靠上下文快照: 会话是唯一权威, 节点已写入
        // 的结果与消息都留在会话里)
        agentCtxPtr->middlewareHandleContext->throwNodeInterruptBase(in.ctx.thread_id);
    }

    // 工具结果定稿: 追加到会话上下文并发出与旧 messages 通道写入同形的事件
    agentxx::nodes::appendSessionMessages(agentCtxPtr, in, std::move(typedResults), nodeName);
    co_return;
}

void ToolcallWrapNode::defStdoutLogOnToolcallStart(
    neograph::graph::NodeInput&                          in,
    size_t                                               limitOutput,
    const std::shared_ptr<agentxx::agent::AgentContext>& ctx
) {
    // 上下文以会话为唯一权威; 本方法是静态方法 (无节点成员), agent 上下文由调用方传入
    auto session = (ctx && ctx->sessions) ? ctx->sessions->getOrCreate(in.ctx.thread_id) : nullptr;
    if (!session) {
        return;
    }
    const neograph::ChatMessage* assistantMsg = nullptr;
    {
        const auto& messages = session->messages();
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (it->role == "assistant" && !it->tool_calls.empty()) {
                assistantMsg = &(*it);
                break;
            }
        }
    }

    std::ostringstream out{};
    if (assistantMsg) {
        size_t index = 0;
        for (auto& item : assistantMsg->tool_calls) {
            ++index;
            out << "┣━ Argument: " << index << ". " << item.name << "/" << item.id << std::endl;
            out << "             - "
                << ((0 == limitOutput) ? item.arguments
                                       : std::string_view{item.arguments}.substr(0, limitOutput))
                << std::endl;
        }
    } else {
        out << "┣━ Empty Argument List\n";
    }
    XX_LOGD(
        R"_(
┏━━━━━━ Toolcall ━━━━━━┓
{}
)_",
        out.str()
    );
}

void ToolcallWrapNode::defStdoutLogOnToolcallEnd(
    const neograph::graph::NodeInput& in,
    neograph::graph::NodeOutput&      result,
    size_t                            limitOutput
) {
    std::ostringstream out{};
    if (false == result.writes.empty()) {
        size_t index = 0;
        for (auto& item : result.writes) {
            ++index;
            auto str = item.value.dump();
            out << "┣━ Result  : " << index << ". " << item.channel << ": "
                << ((0 == limitOutput) ? str : std::string_view{str}.substr(0, limitOutput))
                << std::endl;
        }
    } else {
        out << "┣━ Empty Result List\n";
    }

    XX_LOGD(
        R"_(
{}
┗━━━━━━ Toolcall ━━━━━━┛
)_",
        out.str()
    );
}

} // namespace nodes
} // namespace agentxx
