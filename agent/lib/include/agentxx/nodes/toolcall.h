#pragma once

#include "agentxx/middlewares/permission.h"
#include "agentxx/nodes/warp_handle.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "asio/io_context.hpp"
#include "fmt/base.h"
#include "fmt/format.h"
#include <charconv>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <neograph/llm/rate_limited_provider.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/neograph.h>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace nodes {

class NEOGRAPH_API ToolcallWrapNode
    : public WrapHandleBaseNode<neograph::graph::ToolDispatchNode> {
protected:
public:
  inline static constexpr auto defNodeType = std::string_view{"xx_Toolcall"};

  ToolcallWrapNode(const std::string &in_name,
                   const neograph::graph::NodeContext &in_ctx,
                   std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : WrapHandleBaseNode<neograph::graph::ToolDispatchNode>(
            in_name, in_agentContext, in_ctx) {}

  void
  onHandleStartError(bool errorRethrow, bool isCurrentError,
                     std::string_view exceptionStr,
                     agentxx::middleware::BaseMiddlewareHandleInterface &item,
                     neograph::graph::NodeInput &in,
                     neograph::graph::NodeOutput &result) noexcept override {
    // 插入消息，保证消息顺序正确
    if (false == errorRethrow) {
      auto msg = neograph::ChatMessage{
          .role = "tool",
          .content =
              fmt::format(R"({{"error": "{}/Start call `{}` exception: {}"}})",
                          nodeName, item.name, exceptionStr),
          .flags = neograph::MessageFlag::AutoInserted,
      };
      auto msgJson = neograph::json{};
      neograph::to_json(msgJson, msg);
      result.writes.push_back(neograph::graph::ChannelWrite{
          "messages",
          msgJson,
      });
    }
  }

  void
  onHandleBaseRunError(bool errorRethrow, bool isCurrentError,
                       std::string_view exceptionStr,
                       neograph::graph::NodeInput &in,
                       neograph::graph::NodeOutput &result) noexcept override {
    // 插入消息，保证消息顺序正确
    if (false == errorRethrow && isCurrentError) {
      auto msg = neograph::ChatMessage{
          .role = "tool",
          .content = fmt::format(R"({{"error": "{}/run exception: {}"}})",
                                 nodeName, exceptionStr),
          .flags = neograph::MessageFlag::AutoInserted,
      };
      auto msgJson = neograph::json{};
      neograph::to_json(msgJson, msg);
      result.writes.push_back(neograph::graph::ChannelWrite{
          "messages",
          msgJson,
      });
    }
  }

  asio::awaitable<void>
  onHandleStart(agentxx::middleware::BaseMiddlewareHandleInterface &item,
                neograph::graph::NodeInput &in) override {
    co_await item.onToolcallStartFunc(in);
  }

  asio::awaitable<void>
  onHandleEnd(agentxx::middleware::BaseMiddlewareHandleInterface &item,
              const neograph::graph::NodeInput &in,
              neograph::graph::NodeOutput &result) override {
    co_await item.onToolcallEndFunc(in, result);
  }

  asio::awaitable<std::string> execTool(neograph::Tool *tool,
                                        neograph::json &args) const {
    auto agentCtxPtr = agentContext.lock();
    {
      // 权限检查
      auto it =
          agentCtxPtr->permissionMiddleware->handles.find(tool->get_name());
      if (it != agentCtxPtr->permissionMiddleware->handles.end()) {
        auto allow = co_await it->second(*tool, args);
        if (false == allow) {
          co_return "[Permission denied]";
        }
      }
    }

    size_t maxRetry = 0;
    {
      auto str = tool->extra["maxRetry"];
      auto result =
          std::from_chars(str.c_str(), str.c_str() + str.size(), maxRetry);
      if (result.ec != std::errc{}) {
        maxRetry = 0;
      }
    }

    std::string result;
    size_t retry = 0;
    do {
      bool isCancel = false;
      std::string errInfo;
      std::exception_ptr errorPtr;

      try {
        result = co_await tool->real_execute_async(args);
        break;
      } catch (const neograph::graph::CancelledException &e) {
        isCancel = true;
        errorPtr = std::current_exception();
      } catch (const neograph::graph::NodeInterrupt &e) {
        isCancel = true;
        errorPtr = std::current_exception();
      } catch (const std::exception &e) {
        errInfo = e.what();
        errorPtr = std::current_exception();
      } catch (const boost::exception &e) {
        errInfo = boost::diagnostic_information(e);
        errorPtr = std::current_exception();
      } catch (...) {
        errorPtr = std::current_exception();
      }

      // 触发异常
      if (retry >= maxRetry || isCancel) {
        std::rethrow_exception(errorPtr);
      }
      XX_LOGD("ToolCallNode {} retry: {}/{} | {}", tool->get_name(), retry,
              maxRetry, errInfo);
      retry++;
    } while (true);

    const size_t limitLength =
        agentCtxPtr->agentConfig->toolcallSummaryLimitOutputLength;
    if ("true" == tool->extra["autoSummaryOutput"] &&
        result.size() >= limitLength) {
      // 字节数量超过，按 utf8 长度判断
      auto [targetIndex, lineCount, lastLineIndex] =
          agentxx::util::findIndexAndLastLineIndexByUtf8Length(result,
                                                               limitLength);
      if (targetIndex > 0) {
        const auto thread_id = args.value("thread_id", std::string{});
        assert(false == thread_id.empty());
        // 超过限制长度，截断并存储原文
        auto storeId =
            agentCtxPtr->middlewareHandleContext->addShareStoreItemValue(
                thread_id, result);
        // - 如果超过总摘要 1/3，按行摘要，留出行数以便后续用
        // `share_store` 分页按行取值 否则取总摘要
        if (lastLineIndex >= targetIndex / 3) {
          co_return fmt::format(
              R"([Content offloaded to `share_store`, id={}; Summary {} lines:] {}...)",
              storeId, lineCount,
              std::string_view{result}.substr(0, lastLineIndex));
        } else {
          co_return fmt::format(
              R"([Content offloaded to `share_store`, id={}; Summary:] {}...)",
              storeId, std::string_view{result}.substr(0, targetIndex));
        }
      }
    }
    co_return result;
  }

  asio::awaitable<void>
  baseRun(std::vector<std::shared_ptr<
              agentxx::middleware::BaseMiddlewareHandleInterface>> &handles,
          neograph::graph::NodeInput &in,
          neograph::graph::NodeOutput &out) override {
    auto agentCtxPtr = agentContext.lock();

    auto toolcallsCache = std::map<std::string, std::string>{};
    {
      auto toolcallsCacheJson =
          agentCtxPtr->middlewareHandleContext
              ->getGraphDataItemValue<neograph::json>(
                  in.ctx.thread_id, agentxx::middleware::MiddlewareContext::
                                        graphDataKey_interruptToolcallCache);
      agentCtxPtr->middlewareHandleContext->removeGraphDataItem(
          in.ctx.thread_id, agentxx::middleware::MiddlewareContext::
                                graphDataKey_interruptToolcallCache);
      if (toolcallsCacheJson.is_array()) {
        for (const auto &item : toolcallsCacheJson) {
          neograph::ChatMessage msg;
          neograph::from_json(item, msg);
          if (false == msg.tool_call_id.empty() &&
              false == neograph::hasFlag(msg.flags,
                                         neograph::MessageFlag::Interrupt)) {
            toolcallsCache[msg.tool_call_id] = std::move(msg.content);
          }
        }
      }
    }

    auto messages = in.state.get_messages();
    if (messages.empty()) {
      out = neograph::graph::NodeOutput{};
      co_return;
    }

    // Find the last assistant message with tool_calls
    const neograph::ChatMessage *assistant_msg = nullptr;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
      if (it->role == "assistant" && !it->tool_calls.empty()) {
        assistant_msg = &(*it);
        break;
      }
    }
    if (!assistant_msg) {
      out = neograph::graph::NodeOutput{};
      co_return;
    }

    bool isInterrupt = false;
    auto interruptArgs = std::map<std::string, neograph::json>{};
    auto results = neograph::json::array();

    auto onExecTool = [&](const neograph::ToolCall &tc)
        -> asio::awaitable<neograph::ChatMessage> {
      neograph::ChatMessage tool_msg;
      tool_msg.role = "tool";
      tool_msg.tool_call_id = tc.id;
      tool_msg.tool_name = tc.name;
      {
        // 尝试缓存
        auto cacheit = toolcallsCache.find(tc.id);
        if (cacheit != toolcallsCache.end()) {
          tool_msg.content = cacheit->second;
          co_return tool_msg;
        }
      }

      auto it =
          std::find_if(tools_.begin(), tools_.end(), [&](neograph::Tool *t) {
            return t->get_name() == tc.name;
          });
      if (it == tools_.end()) {
        tool_msg.content = R"({"error": "Tool not found: )" + tc.name + "\"}";
      } else {
        try {
          auto args = neograph::json::parse(tc.arguments);
          if (args.is_object()) {
            // append arg `thread_id`
            args["thread_id"] = in.ctx.thread_id;
            // - 注入 tool_call_id 供 tool 使用 (如 subagent_switch 的中断
            // resultId)
            args["tool_call_id"] = tc.id;
          }
          tool_msg.content = co_await execTool(*it, args);
        } catch (const neograph::graph::NodeInterrupt &e) {
          // tool触发中断
          // - 不应在这里提取中断参数，协程并发等 co_await
          // 执行完成时可能参数数组已经不是单一值
          isInterrupt = true;
          tool_msg.flags |= neograph::MessageFlag::Interrupt;
          tool_msg.content = "[Interrupt]";
        } catch (const std::exception &e) {
          tool_msg.content = std::string(R"({"error": ")") + e.what() + "\"}";
        }
      }
      co_return tool_msg;
    };

    /// 并发执行 toolcall
    std::vector<asio::awaitable<neograph::ChatMessage>> toolcallResults{};
    for (const auto &tc : assistant_msg->tool_calls) {
      toolcallResults.emplace_back(onExecTool(tc));
    }
    for (auto &item : toolcallResults) {
      auto msg = co_await std::move(item);
      neograph::json msg_json;
      neograph::to_json(msg_json, msg);
      results.push_back(msg_json);
    }

    if (isInterrupt) {
      // 暂存 toolcall list 结果到 graphData
      agentCtxPtr->middlewareHandleContext
          ->setGraphDataItemValue<neograph::json>(
              in.ctx.thread_id,
              agentxx::middleware::MiddlewareContext::
                  graphDataKey_interruptToolcallCache,
              results);
      // 保存当前 messages，供 handler 恢复
      auto messages = in.state.get("messages");
      // 重新抛出异常
      agentCtxPtr->middlewareHandleContext->throwNodeInterruptBase(
          in.ctx.thread_id, messages);
    }

    out.writes.push_back(neograph::graph::ChannelWrite{"messages", results});
    co_return;
  }

  inline static asio::awaitable<void>
  defStdoutLogOnToolcallStart(neograph::graph::NodeInput &in,
                              size_t limitOutput = 0) {
    auto messages = in.state.get_messages();
    auto assistant_msg = agentxx::middleware::BaseMiddlewareHandleInterface::
        getLastAssistantToolcallMessage(messages);

    std::ostringstream out{};
    if (assistant_msg) {
      size_t index = 0;
      for (auto &item : assistant_msg->tool_calls) {
        ++index;
        out << "┣━ Argument: " << index << ". " << item.name << "/" << item.id
            << std::endl;
        out << "             - "
            << ((0 == limitOutput)
                    ? item.arguments
                    : std::string_view{item.arguments}.substr(0, limitOutput))
            << std::endl;
      }
    } else {
      out << "┣━ Empty Argument List\n";
    }

    fmt::print(R"(
┏━━━━━━ Toolcall ━━━━━━┓
{}
)",
               out.str());
    co_return;
  }

  inline static asio::awaitable<void>
  defStdoutLogOnToolcallEnd(const neograph::graph::NodeInput &in,
                            neograph::graph::NodeOutput &result,
                            size_t limitOutput = 0) {
    std::ostringstream out{};
    if (false == result.writes.empty()) {
      size_t index = 0;
      for (auto &item : result.writes) {
        ++index;
        auto str = item.value.dump();
        out << "┣━ Result  : " << index << ". " << item.channel << ": "
            << ((0 == limitOutput)
                    ? str
                    : std::string_view{str}.substr(0, limitOutput))
            << std::endl;
      }
    } else {
      out << "┣━ Empty Result List\n";
    }

    fmt::print(R"(
{}
┗━━━━━━ Toolcall ━━━━━━┛

)",
               out.str());
    co_return;
  }
};
} // namespace nodes
} // namespace agentxx