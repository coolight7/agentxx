#pragma once

#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/exception.h"
#include "asio/io_context.hpp"
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
#include <variant>
#include <vector>

namespace asio = ::boost::asio;

namespace agentxx {
namespace nodes {

template <typename T>
concept BaseGraphNodeType = std::same_as<T, neograph::graph::GraphNode> ||
                            std::derived_from<T, neograph::graph::GraphNode>;

class WarpBaseNodeInterface : public neograph::graph::GraphNode {
protected:
  std::string name_;

public:
  WarpBaseNodeInterface(std::string_view name) : name_(name) {}

  asio::awaitable<neograph::graph::NodeOutput>
  run(neograph::graph::NodeInput in) override {
    co_return neograph::graph::NodeOutput{};
  }

  std::string get_name() const override { return name_; }
};

template <BaseGraphNodeType T>
class NEOGRAPH_API MiddlewareWrapBaseNode : public T {

protected:
  std::string name;
  agentxx::middleware::onGraphNodeBeforeCallFunc onBeforeCall;
  agentxx::middleware::onGraphNodeAfterCallFunc onAfterCall;

public:
  MiddlewareWrapBaseNode(
      std::string_view in_name, const neograph::graph::NodeContext &ctx,
      const agentxx::middleware::onGraphNodeBeforeCallFunc &in_onBeforeCall =
          nullptr,
      const agentxx::middleware::onGraphNodeAfterCallFunc &in_onAfterCall =
          nullptr)
      : name(in_name), onBeforeCall(in_onBeforeCall),
        onAfterCall(in_onAfterCall) {}

  virtual asio::awaitable<void>
  onBeforeCallFunc(neograph::graph::NodeInput &in) {
    if (nullptr != onBeforeCall) {
      co_return onBeforeCall(in);
    }
  }

  virtual asio::awaitable<void>
  onAfterCallFunc(const neograph::graph::NodeInput &in,
                  neograph::graph::NodeOutput &result) {
    if (nullptr != onAfterCall) {
      co_return onAfterCall(in, result);
    }
  }

  virtual asio::awaitable<neograph::graph::NodeOutput>
  baseRun(neograph::graph::NodeInput &in) {
    std::string errInfo;
    try {
      co_return co_await T::run(in);
    } catch (const neograph::graph::CancelledException &e) {
      throw;
    } catch (const neograph::graph::NodeInterrupt &e) {
      throw;
    } catch (const std::exception &e) {
      errInfo = e.what();
    } catch (const boost::exception &e) {
      errInfo = boost::diagnostic_information(e);
    } catch (...) {
      errInfo = "Unknown error";
    }

    neograph::graph::NodeOutput out;
    out.writes.push_back(neograph::graph::ChannelWrite{
        "messages",
        fmt::format(R"({{"error": "Middleware Wrap `{}` exception: {}"}})",
                    name, errInfo),
    });
    co_return out;
  }

  asio::awaitable<neograph::graph::NodeOutput>
  run(neograph::graph::NodeInput in) override final {
    co_await onBeforeCallFunc(in);
    auto result = co_await baseRun(in);
    co_await onAfterCallFunc(in, result);
    co_return result;
  }
};

template <BaseGraphNodeType T>
class NEOGRAPH_API WrapHandleBaseNode : public T {
protected:
  std::string nodeName;
  std::weak_ptr<agentxx::agent::AgentContext> agentContext;

public:
  template <typename... Args>
  WrapHandleBaseNode(
      const std::string &name,
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext,
      Args &&...args)
      : T(name, std::forward<Args>(args)...), nodeName(name),
        agentContext(in_agentContext) {}

  virtual asio::awaitable<void>
  onHandleStart(agentxx::middleware::BaseMiddlewareHandleInterface &item,
                neograph::graph::NodeInput &in) = 0;

  virtual asio::awaitable<void>
  onHandleEnd(agentxx::middleware::BaseMiddlewareHandleInterface &item,
              const neograph::graph::NodeInput &in,
              neograph::graph::NodeOutput &result) = 0;

  // 如果是消息节点，应当添加消息，后续不执行 BaseRun
  virtual void
  onHandleStartError(bool errorRethrow, bool isCurrentError,
                     std::string_view exceptionStr,
                     agentxx::middleware::BaseMiddlewareHandleInterface &item,
                     neograph::graph::NodeInput &in,
                     neograph::graph::NodeOutput &result) noexcept {}
  virtual void
  onHandleBaseRunError(bool errorRethrow, bool isCurrentError,
                       std::string_view exceptionStr,
                       neograph::graph::NodeInput &in,
                       neograph::graph::NodeOutput &result) noexcept {}
  // 一般不修改 [result]，消息已经由前面的 start/baseRun 添加
  virtual void
  onHandleEndError(bool errorRethrow, bool isCurrentError,
                   std::string_view exceptionStr,
                   agentxx::middleware::BaseMiddlewareHandleInterface &item,
                   const neograph::graph::NodeInput &in,
                   neograph::graph::NodeOutput &result) noexcept {}

  virtual asio::awaitable<void>
  baseRun(std::vector<std::shared_ptr<
              agentxx::middleware::BaseMiddlewareHandleInterface>> &handles,
          neograph::graph::NodeInput &in, neograph::graph::NodeOutput &result) {
    result = co_await T::run(in);
  }

  /// 栈式调用和异常处理
  /// start1 -> start2 -> start3
  ///             v         |
  ///           error     baseRun
  ///             v         |
  ///  end1  <-  end2  <-  end3
  ///
  /// - start 出现错误时，跳过 baseRun，执行对应的 end
  asio::awaitable<neograph::graph::NodeOutput>
  run(neograph::graph::NodeInput in) override final {
    std::exception_ptr errorPtr;
    bool errorRethrow = false;
    neograph::graph::NodeOutput out;

    auto agentCtxPtr = agentContext.lock();
    const auto len = agentCtxPtr->middlewareHandleContext->handles.size();
    size_t i = 0;
    for (; i < len; ++i) {
      auto &item = agentCtxPtr->middlewareHandleContext->handles[i];
      std::string errInfo;
      try {
        co_await onHandleStart(*item, in);
        continue;
      } catch (const neograph::graph::CancelledException &e) {
        errorRethrow = true;
        onHandleStartError(errorRethrow, true, "", *item, in, out);
        errorPtr = std::current_exception();
      } catch (const neograph::graph::NodeInterrupt &e) {
        errorRethrow = true;
        onHandleStartError(errorRethrow, true, "", *item, in, out);
        errorPtr = std::current_exception();
      } catch (const std::exception &e) {
        errInfo = e.what();
        // 替代 baseRun
        onHandleStartError(errorRethrow, true, errInfo, *item, in, out);
        errorPtr = std::current_exception();
      } catch (const boost::exception &e) {
        errInfo = boost::diagnostic_information(e);
        onHandleStartError(errorRethrow, true, errInfo, *item, in, out);
        errorPtr = std::current_exception();
      } catch (...) {
        errInfo = "Unknown error";
        onHandleStartError(errorRethrow, true, errInfo, *item, in, out);
        errorPtr = std::current_exception();
      }
      XX_LOGE("{}/Start call `{}` exception: {}", nodeName, item->name,
              errInfo);
      // 触发异常，不再执行后面的 start / baseRun
      break;
    }

    do {
      if (i >= len) {
        std::string errInfo;
        try {
          co_await baseRun(agentCtxPtr->middlewareHandleContext->handles, in,
                           out);
          i = len;
          break;
        } catch (const neograph::graph::CancelledException &e) {
          errorRethrow = true;
          onHandleBaseRunError(errorRethrow, true, "", in, out);
          errorPtr = std::current_exception();
        } catch (const neograph::graph::NodeInterrupt &e) {
          errorRethrow = true;
          onHandleBaseRunError(errorRethrow, true, "", in, out);
          errorPtr = std::current_exception();
        } catch (const std::exception &e) {
          errInfo = e.what();
          onHandleBaseRunError(errorRethrow, true, errInfo, in, out);
          errorPtr = std::current_exception();
        } catch (const boost::exception &e) {
          errInfo = boost::diagnostic_information(e);
          onHandleBaseRunError(errorRethrow, true, errInfo, in, out);
          errorPtr = std::current_exception();
        } catch (...) {
          errInfo = "Unknown error";
          onHandleBaseRunError(errorRethrow, true, errInfo, in, out);
          errorPtr = std::current_exception();
        }
        XX_LOGE("{}/run exception: {})", nodeName, errInfo);
      } else if (nullptr != errorPtr) {
        onHandleBaseRunError(errorRethrow, false, "", in, out);
      } else {
        XX_LOGE(
            R"_({}/run, Before `baseRun` should exec all `onStart` or catch exception)_",
            nodeName);
        assert(false);
      }
    } while (false);

    for (; i-- > 0;) {
      auto &item = agentCtxPtr->middlewareHandleContext->handles[i];
      if (nullptr != errorPtr) {
        onHandleEndError(errorRethrow, false, "", *item, in, out);
      } else {
        std::string errInfo;
        try {
          co_await onHandleEnd(*item, in, out);
          continue;
        } catch (const neograph::graph::CancelledException &e) {
          errorRethrow = true;
          onHandleEndError(errorRethrow, true, "", *item, in, out);
          errorPtr = std::current_exception();
        } catch (const neograph::graph::NodeInterrupt &e) {
          errorRethrow = true;
          onHandleEndError(errorRethrow, true, "", *item, in, out);
          errorPtr = std::current_exception();
        } catch (const std::exception &e) {
          errInfo = e.what();
          onHandleEndError(errorRethrow, true, errInfo, *item, in, out);
          if (false == errorRethrow) {
            // 避免覆盖之前的错误，导致未重新抛出异常
            errorPtr = std::current_exception();
          }
        } catch (const boost::exception &e) {
          errInfo = boost::diagnostic_information(e);
          onHandleEndError(errorRethrow, true, errInfo, *item, in, out);
          if (false == errorRethrow) {
            errorPtr = std::current_exception();
          }
        } catch (...) {
          errInfo = "Unknown error";
          onHandleEndError(errorRethrow, true, errInfo, *item, in, out);
          if (false == errorRethrow) {
            errorPtr = std::current_exception();
          }
        }
        XX_LOGE("{}/End call `{}` exception: {}", nodeName, item->name,
                errInfo);
      }
    }

    if (errorRethrow) {
      std::rethrow_exception(errorPtr);
    }

    co_return out;
  }
};

} // namespace nodes
} // namespace agentxx