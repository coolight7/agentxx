#pragma once

#include "agentxx/nodes/agentcall.h"
#include "agentxx/nodes/modelcall.h"
#include "agentxx/nodes/toolcall.h"
#include "agentxx/nodes/warp_handle.h"
#include "agentxx/tools/tool.h"
#include "fmt/format.h"
#include "neograph/neograph.h"
#include <ctime>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace agentxx {
namespace tools {

class SubAgentTaskBase {
protected:
  std::shared_ptr<neograph::graph::GraphEngine> subgraph = nullptr;

public:
  const std::string name;
  const std::string depict;
  std::string systemPrompt;

  SubAgentTaskBase(std::string_view in_subAgentName,
                   std::string_view in_subAgentDepict,
                   std::string_view in_systemPrompt)
      : name(in_subAgentName), depict(in_subAgentDepict),
        systemPrompt(in_systemPrompt) {}

  virtual std::shared_ptr<neograph::graph::GraphEngine> getSubgraph() const {
    assert(nullptr != subgraph);
    return subgraph;
  }

  virtual asio::awaitable<void> onSubagentEnd(std::string &result) {
    co_return;
  }

  virtual ~SubAgentTaskBase() {}
};

class SubAgentNormalTask : public SubAgentTaskBase {
public:
  SubAgentNormalTask(std::string_view in_subAgentName,
                     std::string_view in_subAgentDepict,
                     const neograph::graph::NodeContext &in_context)
      : SubAgentTaskBase(in_subAgentName, in_subAgentDepict, "") {
    createSubgraph(in_context);
  }

  void createSubgraph(const neograph::graph::NodeContext &context) {
    if (nullptr == subgraph) {
      auto inner = neograph::graph::GraphEngine::compile(
          defCreateSubGraphDefine(), context);
      subgraph = std::shared_ptr<neograph::graph::GraphEngine>(inner.release());
    }
  }

  inline static neograph::json defCreateSubGraphDefine() {
    return neograph::json{
        {"name", "xx_SubAgentTask"},
        {
            "channels",
            {
                {"messages", {{"reducer", "append"}}},
            },
        },
        {
            "nodes",
            {
                {
                    "agent_start",
                    {{
                        "type",
                        agentxx::nodes::AgentStartCallWrapNode::defNodeType,
                    }},
                },
                {
                    "agent_end",
                    {{
                        "type",
                        agentxx::nodes::MiddlewareWrapAgentEndCallNode::
                            defNodeType,
                    }},
                },
                {
                    "tools",
                    {{
                        "type",
                        agentxx::nodes::ToolcallWrapNode::defNodeType,
                    }},
                },
                {
                    "llm",
                    {{
                        "type",
                        agentxx::nodes::ModelCallWrapNode::defNodeType,
                    }},
                },
            },
        },
        {
            "edges",
            neograph::json::array({
                {{"from", "__start__"}, {"to", "llm"}},
                {{"from", "agent_start"}, {"to", "llm"}},
                {
                    {"from", "llm"},
                    {"type", "conditional"},
                    {"condition", "has_tool_calls"},
                    {"routes", {{"true", "tools"}, {"false", "agent_end"}}},
                },
                {{"from", "tools"}, {"to", "llm"}},
                {{"from", "agent_end"}, {"to", "__end__"}},
            }),
        },
    };
  }
};

class SubAgentManagerTool : public XXToolBase {
public:
  std::map<std::string, std::shared_ptr<SubAgentTaskBase>> subAgentList{};

  SubAgentManagerTool(
      std::string_view in_nodeName,
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase(in_nodeName, in_agentContext, true, false) {}

  std::string get_name() const override { return "subagent_switch"; }

  neograph::ChatTool get_definition() const override {
    auto subagentNameList = std::vector<std::string>{};
    std::ostringstream subagentNameDepict;
    for (const auto &item : subAgentList) {
      subagentNameList.push_back(item.first);
      subagentNameDepict << fmt::format("`{}`: {}\n", item.first,
                                        item.second->depict);
    }

    return {
        "subagent_switch",
        "Switch a isolation messages context sub-agent to exec.",
        neograph::json{
            {"type", "object"},
            {
                "properties",
                {
                    {
                        "subagent",
                        {
                            {"type", "string"},
                            {"enum", neograph::json{subagentNameList}},
                            {
                                "description",
                                fmt::format("Target sub-agent name.\n{}",
                                            subagentNameDepict.str()),
                            },
                        },
                    },
                    {
                        "system_prompt",
                        {
                            {"type", "string"},
                            {"description",
                             "Sub-agent system prompt if subagent not set"},
                        },
                    },
                    {
                        "message",
                        {
                            {"type", "string"},
                            {"description", "Task content as a user message"},
                        },
                    },
                },
            },
            {
                "required",
                neograph::json::array({"subagent", "message"}),
            },
        },
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    // 不直接运行 subgraph, 而是通过 NodeInterrupt 暂停父 agent
    // - 首次调用: 抛出 subagent 中断, 父 graph checkpoint 暂停
    // - Session 捕获中断后经总线派发给 SubagentSupervisor 运行 subagent
    // - 结果注入 interruptResult channel, 父 graph resume 后此函数返回结果
    auto subagentName = arguments.value("subagent", std::string{});
    if (subagentName.empty()) {
      co_return R"({"error":"Arg `subagent` is empty"})";
    }
    auto message = arguments.value("message", std::string{});
    if (message.empty()) {
      co_return R"({"error":"Arg `message` is empty"})";
    }
    auto system_prompt = arguments.value("system_prompt", std::string{});

    auto subagentIt = subAgentList.find(subagentName);
    if (subagentIt == subAgentList.end() || nullptr == subagentIt->second) {
      std::ostringstream subagentNames;
      bool isFirst = true;
      for (const auto &item : subAgentList) {
        subagentNames << item.first;
        if (false == isFirst) {
          subagentNames << ",";
        }
        isFirst = false;
      }
      co_return fmt::format(
          R"({{"error":"Arg `subagent` is not one of [{}]"}})",
          subagentNames.str());
    }

    // 获取当前 GraphState (由 ToolcallWrapNode::baseRun 注入 graphData)
    auto ctxPtr = agentContext.lock();
    if (!ctxPtr || !ctxPtr->middlewareHandleContext) {
      co_return R"({"error":"AgentContext not available"})";
    }
    auto thread_id = arguments.value("thread_id", std::string{});
    neograph::graph::GraphState *statePtr = nullptr;
    {
      auto &graphData = ctxPtr->middlewareHandleContext->graphData;
      auto it = graphData.find(thread_id);
      if (it != graphData.end()) {
        auto itemIt = it->second.find(
            agentxx::middleware::MiddlewareContext::graphDataKey_currentState);
        if (itemIt != it->second.end()) {
          statePtr =
              std::any_cast<neograph::graph::GraphState *>(itemIt->second);
        }
      }
    }
    if (nullptr == statePtr) {
      co_return R"({"error":"GraphState not available for subagent interrupt"})";
    }

    // 构造中断参数并触发/恢复
    // - 首次: getInterruptResult 抛出 NodeInterrupt, 父 graph 暂停
    // - 恢复: getInterruptResult 返回 interruptResult channel 的整个 map
    //   {resultId: resultValue}, 需按自身 resultId 提取
    auto resultId = arguments.value("tool_call_id", std::string{});
    auto result = agentxx::middleware::InterruptHandleArg::getInterruptResult(
        *statePtr, [&]() {
          return agentxx::middleware::InterruptHandleArg{
              .name = "subagent",
              .arg =
                  neograph::json{
                      {"subagent", subagentName},
                      {"system_prompt", system_prompt},
                      {"message", message},
                  },
              .resultId = resultId,
          };
        });

    // interruptResult channel 存储的是 {resultId: value} map (见 Session
    // resumeValues 注入); 按自身 resultId 提取单个结果
    if (result.is_object() && !resultId.empty() && result.contains(resultId)) {
      auto val = result[resultId];
      if (val.is_string()) {
        co_return val.get<std::string>();
      }
      co_return val.dump();
    }
    // result 本身是单个值 (非 map)
    if (result.is_string()) {
      co_return result.get<std::string>();
    }
    co_return result.dump();
  }
};

}; // namespace tools
}; // namespace agentxx