#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/tools/tool.h"
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <neograph/llm/rate_limited_provider.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/neograph.h>
#include <sstream>
#include <string>
#include <vector>

namespace agentxx {
namespace tools {

/// - 寄存信息，节省模型上下文、为 llm、node、skill、tool
/// 之间方便传递数据
/// TODO: 支持重启恢复
class ThreadShareStoreTool : public XXToolBase {
public:
  ThreadShareStoreTool(
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("share_store", in_agentContext, false, false) {}

  std::optional<agentxx::middleware::SummarizationToolHandle>
  createSummarizationToolHandle() const override {
    return agentxx::middleware::SummarizationToolHandle{
        .requestHandle =
            [](size_t index, std::map<std::string, size_t> &lastWriteIndex,
               neograph::json &args, neograph::ToolCall &toolcall) {
              if (args.is_object() && args["id"].is_string()) {
                auto argId = args["id"].get<std::string>();
                const auto key = fmt::format("share_store:{}", argId);
                if (lastWriteIndex.contains(key)) {
                  // 裁剪 result
                  args["text"] = "[Outdated Message Truncated]";
                  toolcall.arguments = args.dump();
                } else {
                  lastWriteIndex[key] = index;
                }
              }
            },
        .responseHandle =
            [](size_t index, std::map<std::string, size_t> &lastWriteIndex,
               neograph::json &args, neograph::ChatMessage &msg) {
              if (args.is_object() && args["id"].is_string()) {
                auto argId = args["id"].get<std::string>();
                const auto key = fmt::format("share_store:{}", argId);
                if (lastWriteIndex.contains(key)) {
                  msg.content = "[Outdated Content truncated]";
                  msg.flags |= neograph::MessageFlag::ShareStoreTruncated |
                               neograph::MessageFlag::Outdated;
                } else {
                  lastWriteIndex[key] = index;
                }
              }
            },
    };
  }

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt =
        agentPtr->agentConfig->prompt.toolPrompt["share_store"];

    return {
        "share_store",
        prompt.depict,
        neograph::json{
            {"type", "object"},
            {
                "properties",
                {
                    {
                        "opt",
                        {
                            {"type", "string"},
                            {"enum", neograph::json::array({
                                         "get",
                                         "insert",
                                         "set",
                                         "delete",
                                     })},
                            {"description", prompt.getArg("opt")},
                        },
                    },
                    {
                        "text",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("text")},
                        },
                    },
                    {
                        "line_offset",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("line_offset")},
                        },
                    },
                    {
                        "line_limit",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("line_limit")},
                        },
                    },
                    {
                        "id",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("id")},
                        },
                    },
                },
            },
            {"required", neograph::json::array({"opt"})},
        },
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    auto thread_id = arguments.value("thread_id", std::string{});
    if (thread_id.empty()) {
      co_return R"({"error":"Toolcall inner error, need `thread_id`"})";
    }
    size_t text_id = arguments.value<size_t>("id", 0);
    auto text_line_offset = arguments.value<int64_t>("line_offset", -1);
    auto text_line_limit = arguments.value<int64_t>("line_limit", -1);
    auto text = arguments.value("text", std::string{});
    auto text_opt = arguments.value("opt", std::string{});
    if (text_opt.empty()) {
      co_return R"({"error":"Arg `opt` is empty"})";
    }

    if (text_line_offset >= 0 || text_line_limit > 0) {
      const auto offset =
          (text_line_offset >= 0) ? size_t(text_line_offset) : 0;
      const auto limit = (text_line_limit > 0)
                             ? size_t(text_line_limit)
                             : std::numeric_limits<size_t>::max();
      auto stream = std::istringstream{text};
      std::stringstream result{};
      size_t lineNum = 0;

      for (std::string buf; lineNum < offset + limit; lineNum++) {
        if (!std::getline(stream, buf)) {
          if (lineNum >= offset) {
            result << buf;
          }
          break;
        }

        if (lineNum >= offset) {
          result << buf;
        }

        buf.clear();
      }

      if (lineNum <= offset) {
        // offset 超出文件行数
        throw std::runtime_error{fmt::format(
            R"(Arg `line_offset`({} lines) is out of range of file lines({} lines).)",
            offset, lineNum)};
      }

      text = result.str();
    }

    auto agentContextPtr = agentContext.lock();
    if (text_opt == std::string_view{"insert"}) {
      auto reId =
          agentContextPtr->middlewareHandleContext->addShareStoreItemValue(
              thread_id, text);
      co_return neograph::json{
          {"id", reId},
      }
          .dump();
    } else if (text_opt == std::string_view{"get"}) {
      if (text_id <= 0) {
        co_return R"({"error":"Arg `id` is empty"})";
      }
      auto result =
          agentContextPtr->middlewareHandleContext->getShareStoreItemValue(
              thread_id, text_id);
      co_return result.value_or(R"({"error":"Not found"})");
    } else if (text_opt == std::string_view{"set"}) {
      if (text_id <= 0) {
        co_return R"({"error":"Arg `id` is empty"})";
      }
      agentContextPtr->middlewareHandleContext->setShareStoreItemValue(
          thread_id, text_id, text);
      co_return "success";
    } else if (text_opt == std::string_view{"delete"}) {
      if (text_id <= 0) {
        co_return R"({"error":"Arg `id` is empty"})";
      }
      agentContextPtr->middlewareHandleContext->removeShareStoreItemValue(
          thread_id, text_id);
      co_return "success";
    } else {
      co_return R"({"error":"Arg `opt` is invalid"})";
    }
  }
};
} // namespace tools
} // namespace agentxx