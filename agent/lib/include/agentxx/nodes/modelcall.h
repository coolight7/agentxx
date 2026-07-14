#pragma once

#include "agentxx/nodes/warp_handle.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include <cstdlib>
#include <functional>
#include <iostream>
#include <neograph/llm/rate_limited_provider.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/neograph.h>
#include <neograph/types.h>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace nodes {

class NEOGRAPH_API ModelCallWrapNode
    : public WrapHandleBaseNode<neograph::graph::LLMCallNode> {
protected:
public:
  inline static constexpr auto defNodeType =
      std::string_view{"xx_ModelCallWrap"};

  ModelCallWrapNode(const std::string &name,
                    const neograph::graph::NodeContext &ctx,
                    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : WrapHandleBaseNode<neograph::graph::LLMCallNode>(name, in_agentContext,
                                                         ctx) {}

  asio::awaitable<neograph::ChatCompletion>
  onReceiveToken(neograph::CompletionParams &params,
                 neograph::graph::NodeInput input) override {
    auto ctxPtr = agentContext.lock()->middlewareHandleContext;

    auto callback = input.stream_cb;
    std::function<void(const std::string &chunk)> onToken;
    if (nullptr != callback) {
      onToken = [&input, callback, ctxPtr, this](const std::string &token) {
        // 记录 本次请求的临时LLM消息，以便触发异常时处理
        ctxPtr->modifyGraphDataItemValue<std::string>(
            input.ctx.thread_id,
            agentxx::middleware::MiddlewareContext::graphDataKey_tempLLMMessage,
            [&token](std::string &msg) { msg += token; });

        if (nullptr != callback) {
          (*callback)(neograph::graph::GraphEvent{
              neograph::graph::GraphEvent::Type::LLM_TOKEN,
              nodeName,
              neograph::json(token),
          });
        }
      };
    }
    auto completion = co_await provider_->invoke(params, onToken);

    // 记录 token使用量
    ctxPtr->setGraphDataItemValue<int>(
        input.ctx.thread_id,
        agentxx::middleware::MiddlewareContext::graphDataKey_LLMTokenUsage,
        completion.usage.total_tokens);
    co_return completion.message;
  }

  asio::awaitable<void>
  onHandleStart(agentxx::middleware::BaseMiddlewareHandleInterface &item,
                neograph::graph::NodeInput &in) override {
    co_await item.onModelcallStartFunc(in);
  }

  asio::awaitable<void>
  onHandleEnd(agentxx::middleware::BaseMiddlewareHandleInterface &item,
              const neograph::graph::NodeInput &in,
              neograph::graph::NodeOutput &result) override {
    co_await item.onModelcallEndFunc(in, result);
  }

  void
  onHandleStartError(bool errorRethrow, bool isCurrentError,
                     std::string_view exceptionStr,
                     agentxx::middleware::BaseMiddlewareHandleInterface &item,
                     neograph::graph::NodeInput &in,
                     neograph::graph::NodeOutput &result) noexcept override {
    // 插入消息，保证消息顺序正确
    // 不会记录 toolcall
    if (false == errorRethrow) {
      auto msg = neograph::ChatMessage{
          .role = "assistant",
          .content =
              fmt::format(R"({{"error": "{}/Start call `{}` exception: {}"}})",
                          nodeName, item.name, exceptionStr),
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
          .role = "assistant",
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

  void repairMessages(neograph::graph::NodeInput &in) {
    // 最后一条消息应当是 system/user/toolcall
    auto lastMsg =
        agentxx::middleware::BaseMiddlewareHandleInterface::getLastMessage(in);
    if (lastMsg.has_value()) {
      const auto &role = lastMsg.value().role;
      if ("system" == role || "user" == role || "tool" == role) {
        return;
      }
      // 插入 user msg
      auto userMsg = neograph::ChatMessage{
          .role = "user",
          .content = "[Please continue]",
          .flags = neograph::MessageFlag::AutoInserted,
      };
      auto userMsgJson = neograph::json{};
      neograph::to_json(userMsgJson, userMsg);
      in.state.write("messages", userMsgJson);
    }
  }

  asio::awaitable<void>
  baseRun(std::vector<std::shared_ptr<
              agentxx::middleware::BaseMiddlewareHandleInterface>> &handles,
          neograph::graph::NodeInput &in,
          neograph::graph::NodeOutput &result) override {
    auto agentCtxPtr = agentContext.lock();

    {
      // 添加 system Msg
      auto msglist = in.state.get("messages");
      bool haveSystemMsg = false;
      auto newSystemMsg = neograph::ChatMessage{.role = "system"};
      if (msglist.is_array() && false == msglist.empty()) {
        auto systemMsg = neograph::ChatMessage{};
        neograph::from_json(msglist.front(), systemMsg);
        if (systemMsg.role == "system") {
          haveSystemMsg = true;
          newSystemMsg = std::move(systemMsg);
        }
      }

      {
        auto &appendSystemMsgList =
            agentCtxPtr->middlewareHandleContext
                ->getGraphDataItemValue<std::vector<std::string>>(
                    in.ctx.thread_id, agentxx::middleware::MiddlewareContext::
                                          graphDataKey_systemMessage);

        // 清空原本的 content
        newSystemMsg.content = "";
        std::ostringstream oss;
        oss << agentCtxPtr->agentConfig->prompt.systemPrompt;

        if (false == appendSystemMsgList.empty()) {
          for (const auto &item : appendSystemMsgList) {
            oss << item << "\n";
          }
        }

        newSystemMsg.content = oss.str();
      }

      neograph::json sysMsgJson;
      neograph::to_json(sysMsgJson, newSystemMsg);
      if (haveSystemMsg) {
        // 替换 system msg
        msglist[0] = std::move(sysMsgJson);
      } else {
        // 缺少 system msg，在开头插入
        auto newlist = neograph::json::array();
        newlist.push_back(std::move(sysMsgJson));
        for (auto item : msglist.items()) {
          newlist.push_back(std::move(item.second));
        }
        msglist = std::move(newlist);
      }
      in.state.overwrite("messages", std::move(msglist));
    }

    auto ctxPtr = agentContext.lock()->middlewareHandleContext;
    auto timer = asio::steady_timer(co_await asio::this_coro::executor);
    size_t retry = 0;
    do {
      // 清理过时的 临时 LLM 消息
      ctxPtr->removeGraphDataItem(
          in.ctx.thread_id,
          agentxx::middleware::MiddlewareContext::graphDataKey_tempLLMMessage);
      // 修正上下文角色顺序
      repairMessages(in);

      for (auto &handle : handles) {
        co_await handle->onModelcallRunFunc(in);
      }

      if (agentCtxPtr->agentConfig->logPrintMessagesBeforeLLM) {
        agentxx::middleware::BaseMiddlewareHandleInterface::printMessages(
            in.state.get_messages(),
            agentCtxPtr->agentConfig->logPrintMessagesBeforeLLMWithSystemMsg);
      }

      bool isCancel = false;
      std::string errInfo;
      std::exception_ptr errorPtr;

      try {
        // 触发异常时，本次 LLM 消息不会添加到 result 中，因此需要额外处理
        co_await WrapHandleBaseNode<neograph::graph::LLMCallNode>::baseRun(
            agentCtxPtr->middlewareHandleContext->handles, in, result);
        co_return;
      } catch (const neograph::graph::CancelledException &e) {
        isCancel = true;
        errorPtr = std::current_exception();
        // } catch (const neograph::graph::NodeInterrupt &e) {
        // isCancel = true;
        // llm node 无 Interrupt
      } catch (const std::exception &e) {
        errorPtr = std::current_exception();
        errInfo = e.what();
      } catch (const boost::exception &e) {
        errInfo = boost::diagnostic_information(e);
        errorPtr = std::current_exception();
      } catch (...) {
        errorPtr = std::current_exception();
      }

      // 触发异常
      auto lastMsg = ctxPtr->getGraphDataItemValue<std::string>(
          in.ctx.thread_id,
          agentxx::middleware::MiddlewareContext::graphDataKey_tempLLMMessage);
      if (lastMsg.size() >= 512) {
        // - 保留已有的 llm 消息，而不是丢弃
        // - 插入 assistant 消息，此时末尾消息未 assistant, 将在下一次进入
        // baseRun 时自动修复上下文角色顺序 [repairMessages]
        // TODO: 修正消息上下文，应当与客户端同步信息
        auto msg = neograph::ChatMessage{
            .role = "assistant",
            .content = fmt::format("{}\n{}", nodeName,
                                   isCancel ? "[User cancelled]"
                                            : "[Exception aborted]"),
            .flags = neograph::MessageFlag::AutoInserted,
        };
        auto msgJson = neograph::json{};
        neograph::to_json(msgJson, msg);
        in.state.write("messages", msgJson);
      }

      if (isCancel || retry >= agentCtxPtr->agentConfig->llmMaxRetry) {
        std::rethrow_exception(errorPtr);
      }
      // 自动重试
      XX_LOGD("LLMCallNode retry: {}/{} | {}", retry,
              agentCtxPtr->agentConfig->llmMaxRetry, errInfo);
      retry++;
      // 逐渐延长延时等待
      timer.expires_after(std::chrono::milliseconds(retry * 1000));
      co_await timer.async_wait(asio::use_awaitable);
    } while (true);
  }
};
} // namespace nodes
} // namespace agentxx