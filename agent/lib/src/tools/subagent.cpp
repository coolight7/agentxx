#include "agentxx/tools/subagent.h"

#include "agentxx/tools/subagent_shared.h"
#include "fmt/format.h"
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace agentxx {
namespace tools {

events::ReqSubagentBatch parseSubagentBatchFromInterrupt(
    const middleware::InterruptHandleArg&         interruptArg,
    std::string_view                              agentName,
    std::string_view                              sessionId,
    std::shared_ptr<neograph::graph::CancelToken> cancelToken
) {
    events::ReqSubagentBatch batchReq{
        .parentAgentName = std::string{agentName},
        .parentSessionId = std::string{sessionId},
        .cancelToken     = std::move(cancelToken),
    };
    const auto& arg = interruptArg.arg;
    if (arg.contains("tasks") && arg["tasks"].is_array()) {
        // 统一批量语义: {tasks: [...]} 数组, 每项一个子代理任务 (并行运行)
        for (const auto& t : arg["tasks"]) {
            batchReq.tasks.push_back(events::SubagentBatchItem{
                .subagentName = t.value("subagent", std::string{}),
                .systemPrompt = t.value("system_prompt", std::string{}),
                .message      = t.value("message", std::string{}),
                // 结构化消息透传 (同上下文模式): 中断参数携带完整消息前缀
                .messages = (t.contains("messages") && t["messages"].is_array())
                                ? std::optional<neograph::json>{t["messages"]}
                                : std::nullopt,
                // 指定运行 session (同上下文模式): 空时保持默认独立 subagent 线程
                .sessionId = t.value("sessionId", std::string{}),
                // 工具策略 (无工具/继承父/自定义): 缺省不设置 (子代理默认全量)
                .tools = (t.contains("tools") && t["tools"].is_array())
                             ? std::optional<neograph::json>{t["tools"]}
                             : std::nullopt,
                // 压缩中间件开关: 缺省不设置 (继承 config 默认)
                .enableSummarization
                = (t.contains("enable_summarization") && t["enable_summarization"].is_boolean())
                      ? std::optional<bool>{t["enable_summarization"].get<bool>()}
                      : std::nullopt,
                // 任务结果标识: 缺省按任务序号兜底
                .resultId = t.value("result_id", std::string{}),
            });
        }
    } else {
        // 旧单发参数格式兼容: 直接含 subagent 字段时包装为 1 个 task
        batchReq.tasks.push_back(events::SubagentBatchItem{
            .subagentName = arg.value("subagent", std::string{}),
            .systemPrompt = arg.value("system_prompt", std::string{}),
            .message      = arg.value("message", std::string{}),
            .messages     = (arg.contains("messages") && arg["messages"].is_array())
                                ? std::optional<neograph::json>{arg["messages"]}
                                : std::nullopt,
            .sessionId    = arg.value("sessionId", std::string{}),
            .tools        = (arg.contains("tools") && arg["tools"].is_array())
                                ? std::optional<neograph::json>{arg["tools"]}
                                : std::nullopt,
            .enableSummarization
            = (arg.contains("enable_summarization") && arg["enable_summarization"].is_boolean())
                  ? std::optional<bool>{arg["enable_summarization"].get<bool>()}
                  : std::nullopt,
            .resultId = interruptArg.resultId,
        });
    }
    return batchReq;
}

SubAgentTaskBase::SubAgentTaskBase(
    std::string_view in_subAgentName,
    std::string_view in_subAgentDepict,
    std::string_view in_systemPrompt
) :
    name(in_subAgentName),
    depict(in_subAgentDepict),
    systemPrompt(in_systemPrompt) {}

SubAgentTaskBase::~SubAgentTaskBase() {}

SubAgentNormalTask::SubAgentNormalTask(
    std::string_view in_subAgentName,
    std::string_view in_subAgentDepict
) :
    SubAgentTaskBase(in_subAgentName, in_subAgentDepict, "") {}

SubAgentManagerTool::SubAgentManagerTool(
    std::string_view                            in_nodeName,
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase(in_nodeName, in_agentContext, true, false) {}

std::string SubAgentManagerTool::get_name() const {
    return "agentxx_subagent";
}

neograph::ChatTool SubAgentManagerTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    auto               subagentNameList = std::vector<std::string>{};
    std::ostringstream subagentNameDepict;
    for (const auto& item : subAgentList) {
        subagentNameList.push_back(item.first);
        subagentNameDepict << fmt::format("`{}`: {}\n", item.first, item.second->depict);
    }

    // 任务项结构 (tasks 数组元素, 与顶层单任务字段一致)
    const auto taskItemSchema = neograph::json{
        {"type", "object"},
        {
         "properties", {
                {
                    "subagent",
                    {
                        {"type", "string"},
                        // 注意: 必须用圆括号直接初始化 (而非 {} 列表初始化),
                        // 否则重载决议会优先选择 initializer_list 构造函数,
                        // 把 vector 包成单个元素产生 [[...]] 嵌套数组,
                        // 生成非法 enum schema 导致严格校验的上游 (如 gpt-5.6-luna) HTTP 400
                        {"enum", subagentNameList},
                        {
                            "description",
                            fmt::format(
                                "{}\n{}",
                                prompt.getArg("subagent"),
                                subagentNameDepict.str()
                            ),
                        },
                    },
                },
                {
                    "system_prompt",
                    {
                        {"type", "string"},
                        {"description", prompt.getArg("system_prompt")},
                    },
                },
                {
                    "message",
                    {
                        {"type", "string"},
                        {"description", prompt.getArg("message")},
                    },
                },
                {
                    "messages",
                    {
                        {"type", "array"},
                        // Gemini 网关要求 array 类型必须带 items 字段 (缺失报
                        // INVALID_ARGUMENT 400 "missing field"), 元素为消息对象
                        {"items", {{"type", "object"}}},
                        {"description", prompt.getArg("messages")},
                    },
                },
                {
                    "session_id",
                    {
                        {"type", "string"},
                        {"description", prompt.getArg("session_id")},
                    },
                },
                {
                    "tools",
                    {
                        {"type", "array"},
                        {"items", {{"type", "string"}}},
                        {"description", prompt.getArg("tools")},
                    },
                },
                {
                    "enable_summarization",
                    {
                        {"type", "boolean"},
                        {"description", prompt.getArg("enable_summarization")},
                    },
                },
                {
                    "result_id",
                    {
                        {"type", "string"},
                        {
                            "description",
                            "Optional task result id (used to identify this task's result "
                            "when multiple tasks are submitted in one call; empty = numbered "
                            "by task order)",
                        },
                    },
                },
            }, },
        {"required", neograph::json::array({"subagent", "message"})},
    };

    return {
        "agentxx_subagent",
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
                        "tasks",
                        {
                            {"type", "array"},
                            {
                                "description",
                                "Optional batch of subagent tasks, each an object of "
                                "{subagent, system_prompt, message, messages, thread_id, "
                                "tools, enable_summarization, result_id}. When provided "
                                "(non-empty), the top-level single-task fields are ignored "
                                "and all tasks run in parallel.",
                            },
                            {"items", taskItemSchema},
                        },
                    },
                    {
                        "subagent",
                        {
                            {"type", "string"},
                            // 注意: 必须用圆括号直接初始化 (而非 {} 列表初始化),
                            // 否则重载决议会优先选择 initializer_list 构造函数,
                            // 把 vector 包成单个元素产生 [[...]] 嵌套数组,
                            // 生成非法 enum schema 导致严格校验的上游 (如 gpt-5.6-luna) HTTP 400
                            {"enum", neograph::json(subagentNameList)},
                            {
                                "description",
                                fmt::format(
                                    "{}\n{}",
                                    prompt.getArg("subagent"),
                                    subagentNameDepict.str()
                                ),
                            },
                        },
                    },
                    {
                        "system_prompt",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("system_prompt")},
                        },
                    },
                    {
                        "message",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("message")},
                        },
                    },
                    {
                        "messages",
                        {
                            {"type", "array"},
                            // 同 taskItemSchema: Gemini 要求 array 必须带 items
                            {"items", {{"type", "object"}}},
                            {"description", prompt.getArg("messages")},
                        },
                    },
                    {
                        "session_id",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("session_id")},
                        },
                    },
                    {
                        "tools",
                        {
                            {"type", "array"},
                            {"items", {{"type", "string"}}},
                            {"description", prompt.getArg("tools")},
                        },
                    },
                    {
                        "enable_summarization",
                        {
                            {"type", "boolean"},
                            {"description", prompt.getArg("enable_summarization")},
                        },
                    },
                },
            }, {
                "required",
                neograph::json::array({"subagent", "message"}),
            }, },
    };
}

asio::awaitable<std::string> SubAgentManagerTool::execute_async(const neograph::json& arguments) {
    // 统一批量委派 (单发与批量合并):
    // - `tasks` 数组非空: 批量模式 (每项一个子代理任务, 并行运行)
    // - 无 `tasks`: 单任务模式 (顶层 subagent/message 字段; summarization
    //   等直接调用路径兼容), 包装为 1 个 task
    // 执行经 NodeInterrupt 暂停父 agent, 由 AgentHost 派生独立 agent 并发运行,
    // 结果按 (tool_call_id + "_") + (result_id | 任务序号) 注入 interruptResult,
    // 此处按相同规则提取并聚合返回

    struct TaskArg {
        std::string                   subagent;
        std::string                   systemPrompt;
        std::string                   message;
        std::optional<neograph::json> messages;
        std::string                   sessionId;
        std::optional<neograph::json> tools;
        std::optional<bool>           enableSummarization;
        std::string                   resultId;
    };

    auto parseTask = [](const neograph::json& t) -> TaskArg {
        TaskArg task;
        task.subagent     = t.value("subagent", std::string{});
        task.systemPrompt = t.value("system_prompt", std::string{});
        task.message      = t.value("message", std::string{});
        task.sessionId    = t.value("sessionId", std::string{});
        task.resultId     = t.value("result_id", std::string{});
        if (t.contains("messages") && t["messages"].is_array()) {
            task.messages = t["messages"];
        }
        if (t.contains("tools") && t["tools"].is_array()) {
            task.tools = t["tools"];
        }
        if (t.contains("enable_summarization") && t["enable_summarization"].is_boolean()) {
            task.enableSummarization = t["enable_summarization"].get<bool>();
        }
        return task;
    };

    std::vector<TaskArg> tasks;
    if (arguments.contains("tasks") && arguments["tasks"].is_array()
        && !arguments["tasks"].empty()) {
        for (const auto& t : arguments["tasks"]) {
            tasks.push_back(parseTask(t));
        }
    } else {
        tasks.push_back(parseTask(arguments));
    }

    // 校验: 每个任务须有合法 subagent 名, 且 message / messages 至少其一
    {
        std::ostringstream subagentNames;
        bool               isFirst = true;
        for (const auto& item : subAgentList) {
            if (false == isFirst) {
                subagentNames << ",";
            }
            subagentNames << item.first;
            isFirst = false;
        }
        for (const auto& task : tasks) {
            if (task.subagent.empty()) {
                co_return R"({"error":"Arg `subagent` is empty"})";
            }
            if (task.message.empty() && !task.messages.has_value()) {
                // 注意: raw string 内容不能含 `)"` 序列, 故括号提示移到引号外
                co_return R"({"error":"Arg `message` is empty"})";
            }
            auto subIt = subAgentList.find(task.subagent);
            if (subIt == subAgentList.end() || nullptr == subIt->second) {
                co_return fmt::format(
                    R"({{"error":"Arg `subagent` is not one of [{}]"}})",
                    subagentNames.str()
                );
            }
        }
    }

    auto agentCtxPtr = agentContext.lock();
    if (!agentCtxPtr || !agentCtxPtr->middlewareHandleContext) {
        co_return R"({"error":"AgentContext not available"})";
    }
    // sessionId 由 toolcall 节点在执行前注入 arguments (见 toolcall.cpp)
    auto sessionId = arguments.value("sessionId", std::string{});
    auto resultId  = arguments.value("tool_call_id", std::string{});

    // 通过 requestInterrupt 触发/恢复中断
    // - 首次: 存储中断参数 (tasks 数组) 到 graphData, 抛出 NodeInterrupt
    // - 恢复: 从 graphData 读取中断结果, 按 resultId 提取
    auto result = co_await agentCtxPtr->middlewareHandleContext->requestInterrupt(
        sessionId,
        [&]() {
            auto tasksJson = neograph::json::array();
            for (const auto& task : tasks) {
                auto t = neograph::json{
                    {"subagent",      task.subagent    },
                    {"system_prompt", task.systemPrompt},
                    {"message",       task.message     },
                };
                // 结构化消息透传 (同上下文模式): 中断参数携带完整消息前缀
                if (task.messages.has_value()) {
                    t["messages"] = *task.messages;
                }
                // 指定运行 session (同上下文模式): 空时保持默认独立 subagent 线程
                if (!task.sessionId.empty()) {
                    t["sessionId"] = task.sessionId;
                }
                // 工具策略 (无工具/继承父/自定义): 缺省不设置 (子代理默认全量)
                if (task.tools.has_value()) {
                    t["tools"] = *task.tools;
                }
                // 压缩中间件开关: 缺省不设置 (继承 config 默认)
                if (task.enableSummarization.has_value()) {
                    t["enable_summarization"] = *task.enableSummarization;
                }
                // 任务结果标识: 缺省按任务序号兜底
                if (!task.resultId.empty()) {
                    t["result_id"] = task.resultId;
                }
                tasksJson.push_back(std::move(t));
            }
            return agentxx::middleware::InterruptHandleArg{
                .name     = "subagent",
                .arg      = neograph::json{{"tasks", std::move(tasksJson)}},
                .resultId = resultId,
            };
        },
        nullptr
    );

    // 提取结果: key = (tool_call_id + "_") + (task.result_id | 任务序号)
    // (与中断处理循环 buildSubagentResumeValues 共用 makeSubagentResumeKey 规则;
    //  前缀避免同一轮多个中断的序号 key 互相覆盖)
    auto   outputs = neograph::json::array();
    size_t idx     = 0;
    for (const auto& task : tasks) {
        ++idx;
        auto key = makeSubagentResumeKey(resultId, task.resultId, idx);
        if (result.is_object() && result.contains(key)) {
            const auto& val = result[key];
            if (val.is_string()) {
                outputs.push_back(val.get<std::string>());
            } else {
                outputs.push_back(val.dump());
            }
        }
    }
    if (false == outputs.empty()) {
        // 单任务: 返回纯文本 (与旧行为一致, LLM/压缩路径均期望文本)
        if (outputs.size() == 1) {
            co_return outputs[0].get<std::string>();
        }
        // 多任务: 返回 json 数组 (按任务顺序)
        co_return outputs.dump();
    }
    // 兜底: 非 toolcall 路径直接调用 (如上下文压缩中间件): resultId 为空,
    // 单中断场景下取 map 中的第一个字符串值 (中断处理以 argIndex 兜底编号)
    if (result.is_object() && resultId.empty()) {
        for (const auto& [key, val] : result.items()) {
            if (val.is_string()) {
                co_return val.get<std::string>();
            }
        }
        co_return result.dump();
    }
    if (result.is_string()) {
        co_return result.get<std::string>();
    }
    co_return result.dump();
}

}; // namespace tools
}; // namespace agentxx
