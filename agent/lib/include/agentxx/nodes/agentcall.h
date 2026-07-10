#pragma once

#include "agentxx/nodes/warp_handle.h"
#include "asio/io_context.hpp"
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

class NEOGRAPH_API AgentStartCallWrapNode
    : public WrapHandleBaseNode<agentxx::nodes::WarpBaseNodeInterface> {
protected:
public:
  inline static constexpr auto defNodeType =
      std::string_view{"xx_MiddlewareWrapAgentStartCall"};

  AgentStartCallWrapNode(
      const std::string &name,
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : WrapHandleBaseNode<agentxx::nodes::WarpBaseNodeInterface>(
            name, in_agentContext) {}

  asio::awaitable<void>
  onHandleStart(agentxx::middleware::BaseMiddlewareHandleInterface &item,
                neograph::graph::NodeInput &in) override {
    {
      // 创建单次执行的临时数据
      auto ptr = agentContext.lock();
      ptr->middlewareHandleContext->graphData[in.ctx.thread_id].clear();
      // 清理一些临时参数
      in.state.remove(agentxx::middleware::BaseMiddlewareHandleInterface::
                          channelKey_interruptMessages);
      in.state.remove(agentxx::middleware::BaseMiddlewareHandleInterface::
                          channelKey_interruptToolcallCache);
      in.state.remove(agentxx::middleware::BaseMiddlewareHandleInterface::
                          channelKey_interruptArgs);
      in.state.remove(agentxx::middleware::BaseMiddlewareHandleInterface::
                          channelKey_interruptResult);
    }

    co_await item.onAgentcallStartFunc(in);
  }

  asio::awaitable<void>
  onHandleEnd(agentxx::middleware::BaseMiddlewareHandleInterface &item,
              const neograph::graph::NodeInput &in,
              neograph::graph::NodeOutput &result) override {
    co_return;
  }
};

class NEOGRAPH_API MiddlewareWrapAgentEndCallNode
    : public WrapHandleBaseNode<agentxx::nodes::WarpBaseNodeInterface> {
protected:
public:
  inline static constexpr auto defNodeType =
      std::string_view{"xx_MiddlewareWrapAgentEndCall"};

  MiddlewareWrapAgentEndCallNode(
      const std::string &name,
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : WrapHandleBaseNode<agentxx::nodes::WarpBaseNodeInterface>(
            name, in_agentContext) {}

  asio::awaitable<void>
  onHandleStart(agentxx::middleware::BaseMiddlewareHandleInterface &item,
                neograph::graph::NodeInput &in) override {
    co_return;
  }

  asio::awaitable<void>
  onHandleEnd(agentxx::middleware::BaseMiddlewareHandleInterface &item,
              const neograph::graph::NodeInput &in,
              neograph::graph::NodeOutput &result) override {
    co_await item.onAgentcallEndFunc(in, result);

    {
      // 清理单次执行的临时数据
      auto ptr = agentContext.lock();
      auto it = ptr->middlewareHandleContext->graphData.find(in.ctx.thread_id);
      if (it != ptr->middlewareHandleContext->graphData.end()) {
        ptr->middlewareHandleContext->graphData.erase(it);
      }
    }
  }

  void
  onHandleEndError(bool errorRethrow, bool isCurrentError,
                   std::string_view exceptionStr,
                   agentxx::middleware::BaseMiddlewareHandleInterface &item,
                   const neograph::graph::NodeInput &in,
                   neograph::graph::NodeOutput &result) noexcept override {
    {
      // 清理单次执行的临时数据
      auto ptr = agentContext.lock();
      auto it = ptr->middlewareHandleContext->graphData.find(in.ctx.thread_id);
      if (it != ptr->middlewareHandleContext->graphData.end()) {
        ptr->middlewareHandleContext->graphData.erase(it);
      }
    }
  }
};

} // namespace nodes
} // namespace agentxx