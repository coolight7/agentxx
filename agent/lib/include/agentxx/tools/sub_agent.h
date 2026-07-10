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
    // 这里无法修改跳转到指定节点，因此只接收 llm 节点决定启用哪个 subagent
    // 的参数，转由 [SubAgentMiddlewareHandle::onToolcallEndFunc] 处理
    auto subagentName = arguments.value("subagent", std::string{});
    if (subagentName.empty()) {
      co_return R"({"error":"Arg `subagent` is empty"})";
    }
    auto message = arguments.value("message", std::string{});
    if (message.empty()) {
      co_return R"({"error":"Arg `message` is empty"})";
    }

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

    auto subagent = subagentIt->second;
    assert(nullptr != subagent->getSubgraph());

    std::string system_prompt = subagent->systemPrompt;
    if (system_prompt.empty()) {
      system_prompt = arguments.value(
          "system_prompt", std::string{"你是一个专门处理用户请求的辅助助手."});
    }

    try {
      neograph::graph::RunConfig cfg{
          .thread_id = fmt::format("session_subagent_{}", subagentName),
          .input = {{
              "messages",
              neograph::json::array({
                  {
                      {"role", "system"},
                      {"content", system_prompt},
                  },
                  {
                      {"role", "user"},
                      {"content", message},
                  },
              }),
          }},
          .resume_if_exists = false,
      };

      std::ostringstream oss;
      XX_LOGD("    ## Subagent - {}", subagent->name);
      co_await subagent->getSubgraph()->run_stream_async(
          cfg, [&oss](const neograph::graph::GraphEvent &event) {
            switch (event.type) {
            case neograph::graph::GraphEvent::Type::NODE_START:
            case neograph::graph::GraphEvent::Type::NODE_END:
              break;
            case neograph::graph::GraphEvent::Type::LLM_TOKEN: {
              const auto token = event.data.get<std::string>();
              oss << token;
#if XX_IS_DEBUG_D
              std::cout << token << std::flush;
#endif
            } break;
            case neograph::graph::GraphEvent::Type::CHANNEL_WRITE:
            case neograph::graph::GraphEvent::Type::INTERRUPT:
            case neograph::graph::GraphEvent::Type::ERROR:
              break;
            }
          });
      auto result = oss.str();
      co_await subagent->onSubagentEnd(result);
      co_return result;
    } catch (const std::exception &e) {
      co_return fmt::format(R"({{"error": "Sub-agent Response failed: {}"}})",
                            e.what());
    }
  }
};

}; // namespace tools
}; // namespace agentxx