#pragma once

#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/string_util.h"
#include "asio/io_context.hpp"
#include "fmt/format.h"
#include <cstdlib>
#include <functional>
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

template<typename T>
concept BaseGraphNodeType = std::same_as<T, neograph::graph::GraphNode>
                            || std::derived_from<T, neograph::graph::GraphNode>;

class WrapBaseNodeInterface : public neograph::graph::GraphNode {
protected:

    std::string name_;

public:

    WrapBaseNodeInterface(std::string_view name);

    asio::awaitable<neograph::graph::NodeOutput> run(neograph::graph::NodeInput in) override;

    std::string get_name() const override;
};

template<BaseGraphNodeType T>
class NEOGRAPH_API MiddlewareWrapBaseNode : public T {
protected:

    std::string                                    name;
    agentxx::middleware::onGraphNodeBeforeCallFunc onBeforeCall;
    agentxx::middleware::onGraphNodeAfterCallFunc  onAfterCall;

public:

    MiddlewareWrapBaseNode(
        std::string_view                                      in_name,
        const neograph::graph::NodeContext&                   ctx,
        const agentxx::middleware::onGraphNodeBeforeCallFunc& in_onBeforeCall = nullptr,
        const agentxx::middleware::onGraphNodeAfterCallFunc&  in_onAfterCall  = nullptr
    ) :
        name(in_name),
        onBeforeCall(in_onBeforeCall),
        onAfterCall(in_onAfterCall) {}

    virtual asio::awaitable<void> onBeforeCallFunc(neograph::graph::NodeInput& in) {
        if (nullptr != onBeforeCall) {
            co_await onBeforeCall(in);
        }
    }

    virtual asio::awaitable<void>
        onAfterCallFunc(const neograph::graph::NodeInput& in, neograph::graph::NodeOutput& result) {
        if (nullptr != onAfterCall) {
            co_await onAfterCall(in, result);
        }
    }

    virtual asio::awaitable<neograph::graph::NodeOutput> baseRun(neograph::graph::NodeInput& in) {
        std::string errInfo;
        try {
            co_return co_await T::run(in);
        } catch (const neograph::graph::CancelledException& e) {
            throw;
        } catch (const neograph::graph::NodeInterrupt& e) {
            throw;
        } catch (const boost::exception& e) {
            // boost::exception 在 std::exception 之前捕获, 保留完整诊断信息
            errInfo = agentxx::util::autoTryConvertToUtf8(boost::diagnostic_information(e));
        } catch (const std::exception& e) {
            errInfo = agentxx::util::autoTryConvertToUtf8(e.what());
        } catch (...) {
            errInfo = "Unknown error";
        }

        neograph::graph::NodeOutput out;
        // TODO: 确认是否应该写入到 messages
        out.writes.push_back(neograph::graph::ChannelWrite{
            "messages",
            neograph::json{
                           {"error", fmt::format("Middleware Wrap `{}` exception: {}", name, errInfo)},
                           }
                .dump(),
        });
        co_return out;
    }

    asio::awaitable<neograph::graph::NodeOutput> run(neograph::graph::NodeInput in) override final {
        co_await onBeforeCallFunc(in);
        auto result = co_await baseRun(in);
        co_await onAfterCallFunc(in, result);
        co_return result;
    }
};

template<BaseGraphNodeType T>
class NEOGRAPH_API WrapHandleBaseNode : public T {
protected:

    std::string                                 nodeName;
    std::weak_ptr<agentxx::agent::AgentContext> agentContext;

    /// 是否拦截普通异常 (不重抛, 视为节点正常完成)
    /// - false (默认): 普通异常统一重抛, 停止本轮执行
    ///   (如 ModelCall 重试耗尽后应结束会话, 不能静默吞掉)
    /// - true: 普通异常被拦截, 图继续调度
    ///   (如 Toolcall 由 onHandle*Error 插入错误消息后, 让 agent 基于错误继续运行)
    /// 取消/中断/取消信号 类控制流异常不受影响, 始终重抛
    bool interceptOrdinaryError_ = false;

public:

    template<typename... Args>
    WrapHandleBaseNode(
        std::string_view                            name,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext,
        Args&&... args
    ) :
        T(std::string{name}, std::forward<Args>(args)...),
        nodeName(name),
        agentContext(in_agentContext) {}

    virtual asio::awaitable<void> onNodeStart(neograph::graph::NodeInput& in) {
        co_return;
    }

    // 取消/中断/异常重抛时 (onHandle*Error 路径) 不执行;
    // 正常完成 或 普通异常被节点拦截 (视为正常完成) 时执行
    virtual asio::awaitable<void>
        onNodeEnd(const neograph::graph::NodeInput& in, neograph::graph::NodeOutput& result) {
        co_return;
    }

    virtual asio::awaitable<void> onHandleStart(
        agentxx::middleware::BaseMiddlewareHandleInterface& item,
        neograph::graph::NodeInput&                         in
    ) = 0;

    virtual asio::awaitable<void> onHandleEnd(
        agentxx::middleware::BaseMiddlewareHandleInterface& item,
        const neograph::graph::NodeInput&                   in,
        neograph::graph::NodeOutput&                        result
    ) = 0;

    // 如果是消息节点，应当添加消息，后续不执行 BaseRun
    virtual void onHandleStartError(
        bool                                                errorRethrow,
        bool                                                isCurrentError,
        std::string_view                                    exceptionStr,
        agentxx::middleware::BaseMiddlewareHandleInterface& item,
        neograph::graph::NodeInput&                         in,
        neograph::graph::NodeOutput&                        result
    ) noexcept {}

    virtual void onHandleBaseRunError(
        bool                         errorRethrow,
        bool                         isCurrentError,
        std::string_view             exceptionStr,
        neograph::graph::NodeInput&  in,
        neograph::graph::NodeOutput& result
    ) noexcept {}

    // 一般不修改 [result]，消息已经由前面的 start/baseRun 添加
    virtual void onHandleEndError(
        bool                                                errorRethrow,
        bool                                                isCurrentError,
        std::string_view                                    exceptionStr,
        agentxx::middleware::BaseMiddlewareHandleInterface& item,
        const neograph::graph::NodeInput&                   in,
        neograph::graph::NodeOutput&                        result
    ) noexcept {}

    virtual asio::awaitable<void> baseRun(
        std::vector<std::shared_ptr<agentxx::middleware::BaseMiddlewareHandleInterface>>& handles,
        neograph::graph::NodeInput&                                                       in,
        neograph::graph::NodeOutput&                                                      result
    ) {
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
    asio::awaitable<neograph::graph::NodeOutput> run(neograph::graph::NodeInput in) override final {
        if (in.ctx.cancel_token) {
            // 取消埋点
            in.ctx.cancel_token->throw_if_cancelled(
                fmt::format("before WrapNode call: {}", nodeName)
            );
        }

        co_await onNodeStart(in);

        std::string        errInfo;
        std::exception_ptr errorPtr;
        // 取消/中断标记: 仅 取消/中断/取消信号 类控制流异常为 true, 普通异常为 false
        // - 传给 onHandle*Error 回调用于区分 (如 ToolcallWrapNode 仅对普通异常
        //   插入错误消息, 取消/中断由节点内部处理)
        // - 末尾判定: errorRethrow=false (普通异常) 且节点声明拦截时, 不重抛
        bool                        errorRethrow = false;
        neograph::graph::NodeOutput out;

        auto       agentCtxPtr = agentContext.lock();
        const auto len         = agentCtxPtr->middlewareHandleContext->handles.size();
        // 已实际执行 onHandleStart 的下标记录 (M7): end 阶段按此回放, 不依赖
        // 当前 disabled 位 —— 运行中插件禁用 (置 disabled) 不会破坏 start/end
        // 配对 (start 已执行的中间件, end 必定执行; 未被 start 的绝不 end)
        std::vector<size_t> startedIdxs;
        startedIdxs.reserve(len);
        size_t i = 0;
        for (; i < len; ++i) {
            auto& item = agentCtxPtr->middlewareHandleContext->handles[i];
            // 跳过已禁用中间件 (插件热卸载/禁用; len 已缓存, 跳过不影响
            // 下标遍历安全)
            if (item->disabled) {
                continue;
            }
            XX_LOGT(R"_({}/start, {}/{})_", nodeName, item->name, i);
            try {
                co_await onHandleStart(*item, in);
                // 成功执行 start 才记录: 出错项不参与 end 回放 (与原语义一致)
                startedIdxs.push_back(i);
                continue;
            } catch (const neograph::graph::CancelledException&) {
                errorRethrow = true;
                errInfo      = "cancelled";
                onHandleStartError(errorRethrow, true, errInfo, *item, in, out);
                errorPtr = std::current_exception();
            } catch (const neograph::graph::NodeInterrupt&) {
                errorRethrow = true;
                errInfo      = "interrupt";
                onHandleStartError(errorRethrow, true, errInfo, *item, in, out);
                errorPtr = std::current_exception();
            } catch (const neograph_asio_system_error& e) {
                if (agentxx::util::isCancelAbort(e, in.ctx.cancel_token)) {
                    // asio 取消信号中断 co_await 的异常表现, 转换为取消语义重抛,
                    // 避免被当作普通错误吞掉后继续执行 graph
                    errorRethrow = true;
                    errInfo      = "operation cancelled";
                    onHandleStartError(errorRethrow, true, errInfo, *item, in, out);
                    errorPtr = std::make_exception_ptr(
                        neograph::graph::CancelledException("operation aborted")
                    );
                } else {
                    errInfo = agentxx::util::autoTryConvertToUtf8(e.what());
                    // 替代 baseRun
                    onHandleStartError(errorRethrow, true, errInfo, *item, in, out);
                    errorPtr = std::current_exception();
                }
            } catch (const boost::exception& e) {
                // boost::exception 在 std::exception 之前捕获, 保留完整诊断信息
                errInfo = agentxx::util::autoTryConvertToUtf8(boost::diagnostic_information(e));
                onHandleStartError(errorRethrow, true, errInfo, *item, in, out);
                errorPtr = std::current_exception();
            } catch (const std::exception& e) {
                errInfo = agentxx::util::autoTryConvertToUtf8(e.what());
                // 替代 baseRun
                onHandleStartError(errorRethrow, true, errInfo, *item, in, out);
                errorPtr = std::current_exception();
            } catch (...) {
                errInfo = "Unknown error";
                onHandleStartError(errorRethrow, true, errInfo, *item, in, out);
                errorPtr = std::current_exception();
            }
            XX_LOGE("{}/Start call `{}` exception: {}", nodeName, item->name, errInfo);
            // 触发异常，不再执行后面的 start / baseRun
            break;
        }

        do {
            if (i >= len) {
                try {
                    co_await baseRun(agentCtxPtr->middlewareHandleContext->handles, in, out);
                    i = len;
                    break;
                } catch (const neograph::graph::CancelledException&) {
                    errorRethrow = true;
                    errInfo      = "cancelled";
                    onHandleBaseRunError(errorRethrow, true, errInfo, in, out);
                    errorPtr = std::current_exception();
                } catch (const neograph::graph::NodeInterrupt&) {
                    errorRethrow = true;
                    errInfo      = "interrupt";
                    onHandleBaseRunError(errorRethrow, true, errInfo, in, out);
                    errorPtr = std::current_exception();
                } catch (const neograph_asio_system_error& e) {
                    if (agentxx::util::isCancelAbort(e, in.ctx.cancel_token)) {
                        // asio 取消信号中断 co_await 的异常表现, 转换为取消语义重抛,
                        // 避免被当作普通错误吞掉后继续执行 graph
                        errorRethrow = true;
                        errInfo      = "operation cancelled";
                        onHandleBaseRunError(errorRethrow, true, errInfo, in, out);
                        errorPtr = std::make_exception_ptr(
                            neograph::graph::CancelledException("operation aborted")
                        );
                    } else {
                        errInfo = agentxx::util::autoTryConvertToUtf8(e.what());
                        onHandleBaseRunError(errorRethrow, true, errInfo, in, out);
                        errorPtr = std::current_exception();
                    }
                } catch (const boost::exception& e) {
                    // boost::exception 在 std::exception 之前捕获, 保留完整诊断信息
                    errInfo = agentxx::util::autoTryConvertToUtf8(boost::diagnostic_information(e));
                    onHandleBaseRunError(errorRethrow, true, errInfo, in, out);
                    errorPtr = std::current_exception();
                } catch (const std::exception& e) {
                    errInfo = agentxx::util::autoTryConvertToUtf8(e.what());
                    onHandleBaseRunError(errorRethrow, true, errInfo, in, out);
                    errorPtr = std::current_exception();
                } catch (...) {
                    errInfo = "Unknown error";
                    onHandleBaseRunError(errorRethrow, true, errInfo, in, out);
                    errorPtr = std::current_exception();
                }
                XX_LOGE("{}/run exception: {}", nodeName, errInfo);
            } else if (nullptr != errorPtr) {
                onHandleBaseRunError(errorRethrow, false, errInfo, in, out);
            } else {
                XX_LOGE(
                    R"_({}/run, Before `baseRun` should exec all `onStart` or catch exception)_",
                    nodeName
                );
                assert(false);
            }
        } while (false);

        // end 阶段: 按 startedIdxs 逆序回放 (仅回放实际执行过 start 的中间件,
        // 不再检查 disabled —— 运行中禁用不破坏配对)
        for (auto it = startedIdxs.rbegin(); it != startedIdxs.rend(); ++it) {
            auto& item = agentCtxPtr->middlewareHandleContext->handles[*it];
            XX_LOGT(R"_({}/end, {}/{})_", nodeName, item->name, *it);
            if (nullptr != errorPtr) {
                onHandleEndError(errorRethrow, false, errInfo, *item, in, out);
            } else {
                try {
                    co_await onHandleEnd(*item, in, out);
                    continue;
                } catch (const neograph::graph::CancelledException&) {
                    errorRethrow = true;
                    errInfo      = "cancelled";
                    onHandleEndError(errorRethrow, true, errInfo, *item, in, out);
                    errorPtr = std::current_exception();
                } catch (const neograph::graph::NodeInterrupt&) {
                    errorRethrow = true;
                    errInfo      = "interrupt";
                    onHandleEndError(errorRethrow, true, errInfo, *item, in, out);
                    errorPtr = std::current_exception();
                } catch (const neograph_asio_system_error& e) {
                    if (agentxx::util::isCancelAbort(e, in.ctx.cancel_token)) {
                        // asio 取消信号中断 co_await 的异常表现, 转换为取消语义重抛,
                        // 避免被当作普通错误吞掉后继续执行 graph
                        errorRethrow = true;
                        errInfo      = "operation cancelled";
                        onHandleEndError(errorRethrow, true, errInfo, *item, in, out);
                        errorPtr = std::make_exception_ptr(
                            neograph::graph::CancelledException("operation aborted")
                        );
                    } else {
                        errInfo = agentxx::util::autoTryConvertToUtf8(e.what());
                        onHandleEndError(errorRethrow, true, errInfo, *item, in, out);
                        if (false == errorRethrow) {
                            // 避免覆盖之前的错误，导致未重新抛出异常
                            errorPtr = std::current_exception();
                        }
                    }
                } catch (const boost::exception& e) {
                    // boost::exception 在 std::exception 之前捕获, 保留完整诊断信息
                    errInfo = agentxx::util::autoTryConvertToUtf8(boost::diagnostic_information(e));
                    onHandleEndError(errorRethrow, true, errInfo, *item, in, out);
                    if (false == errorRethrow) {
                        // 避免覆盖之前的错误，导致未重新抛出异常
                        errorPtr = std::current_exception();
                    }
                } catch (const std::exception& e) {
                    errInfo = agentxx::util::autoTryConvertToUtf8(e.what());
                    onHandleEndError(errorRethrow, true, errInfo, *item, in, out);
                    if (false == errorRethrow) {
                        // 避免覆盖之前的错误，导致未重新抛出异常
                        errorPtr = std::current_exception();
                    }
                } catch (...) {
                    errInfo = "Unknown error";
                    onHandleEndError(errorRethrow, true, errInfo, *item, in, out);
                    if (false == errorRethrow) {
                        errorPtr = std::current_exception();
                    }
                }
                XX_LOGE("{}/End call `{}` exception: {}", nodeName, item->name, errInfo);
            }
        }

        if (nullptr != errorPtr) {
            if (interceptOrdinaryError_ && false == errorRethrow) {
                // 节点声明拦截普通异常 (如 Toolcall): 不重抛, 视为节点正常完成
                // - onHandle*Error 已插入错误消息到 [out], 图继续调度, 让 agent
                //   基于错误消息继续运行 (工具调用失败是常态, 不应终止会话)
                // - 仅拦截普通异常; 取消/中断/取消信号 (errorRethrow=true)
                //   仍走下方重抛, 保证控制流语义
                XX_LOGW("{}/intercept ordinary exception: {}", nodeName, errInfo);
                co_await onNodeEnd(in, out);
                co_return out;
            }
            // 保存此时的上下文，如果直接抛异常到 neograph::engine，会丢失本轮 session 增加的上下文
            auto session = agentCtxPtr->getSession(in.ctx.thread_id);
            auto data    = in.state.get("messages");
            XX_LOGD("Store(By WrapHandleBaseNode/rethrow) LLM-Messages Context: {}", data.size());
            agentCtxPtr->middlewareHandleContext->setGraphDataItemValue(
                in.ctx.thread_id,
                agentxx::middleware::MiddlewareContext::graphDataKey_tempMessages,
                std::move(data)
            );

            // 取消/中断 或 未配置拦截的节点 (如 ModelCall): 统一重抛, 不静默吞掉
            // - 静默吞掉会让节点"假装成功"返回空输出, 图继续调度, 可能造成
            //   [has_tool_calls] 等条件误路由 (如 llm 重试耗尽后末尾仍为
            //   assistant(tool_calls) 时被误路由回 tools 节点, 导致工具重复
            //   执行与消息无限堆积)
            // - 业务层错误 (如工具执行失败) 由节点内部捕获消息化, 不会抛到这里
            std::rethrow_exception(errorPtr);
        }

        co_await onNodeEnd(in, out);
        co_return out;
    }
};

} // namespace nodes
} // namespace agentxx