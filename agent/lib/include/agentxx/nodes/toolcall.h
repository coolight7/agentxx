#pragma once

#include "agentxx/middlewares/permission.h"
#include "agentxx/nodes/warp_handle.h"
#include "asio/io_context.hpp"
#include "fmt/base.h"
#include "fmt/format.h"
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
                                        neograph::json &args) const override {
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
        result =
            co_await neograph::graph::ToolDispatchNode::execTool(tool, args);
        break;
      } catch (const neograph::graph::CancelledException &e) {
        isCancel = true;
        errorPtr = std::current_exception();
      } catch (const std::exception &e) {
        errInfo = e.what();
        errorPtr = std::current_exception();
      } catch (const boost::exception &e) {
        errorPtr = std::current_exception();
        errInfo = boost::diagnostic_information(e);
      } catch (...) {
        errorPtr = std::current_exception();
      }

      // 触发异常
      XX_LOGD("ToolCallNode {} retry: {}/{} | {}", tool->get_name(), retry,
              maxRetry, errInfo);
      if (retry >= maxRetry || isCancel) {
        std::rethrow_exception(errorPtr);
      } else {
        retry++;
      }
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