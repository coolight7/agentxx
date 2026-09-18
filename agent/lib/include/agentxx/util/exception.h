/// agentxx::util —— 宿主/图引擎耦合件 (仅保留必须与 neograph 打交道的部分)
///
/// 本头只保留图引擎相关的异常分类与统一捕获工具:
/// - 通用错误/取消处理 (utilxx_base::catchError 系列) 已下沉到 `cxx_utilxx_base`
///   (见 utilxx_base/exception.h), 本头把它与 neograph 的取消/中断语义衔接起来;
/// - 调用方无需关心分类细节: `agentxx::util::catchError<T>(...)` 保持原有签名与行为
///   (多一个可选的取消令牌参数)。
///
/// 所有权说明: 图引擎的 `CancelToken` 与 `utilxx::CancelToken` 是两套取消抽象,
/// 二者经 `agentxx/util/cancel_adapter.h` 的适配器互通。
#pragma once

#include "asio/awaitable.hpp"
#include "boost/exception/diagnostic_information.hpp"
#include "boost/exception/exception.hpp"
#include "neograph/api.h"
#include "neograph/graph/cancel.h"
#include "neograph/graph/types.h"
#include "utilxx/cancel.h"
#include "utilxx_base/exception.h"
#include "utilxx_base/string_util.h"
#include <concepts>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace agentxx {
namespace util {

/// 通用工具 (转发 utilxx_base 实现; 语义与拆分前一致)
using utilxx_base::ControlFlowKind;
using utilxx_base::ExceptionClassification;
using utilxx_base::isNullCallable;

/// neograph 取消令牌的共享所有权别名 (与图引擎一致)
using NeographCancelTokenPtr = std::shared_ptr<neograph::graph::CancelToken>;

/// 判断 system_error 是否为取消信号的作用结果
/// - 当 cancelToken 已取消时, 协程 co_await 点上出现的 operation_aborted 是
///   asio cancellation_signal 中断异步 IO 的结果, 属于取消语义而非超时/传输错误
inline bool isCancelAbort(
    const utilxx_base::AsioSystemError& e,
    const NeographCancelTokenPtr&       cancelToken
) noexcept {
    return e.code() == asio::error::operation_aborted && nullptr != cancelToken
           && cancelToken->is_cancelled();
}

/// 识别 neograph 的控制流异常 (取消/中断) 并填入分类结果
/// - 必须在 `catch` 块内调用 (内部经 `throw;` 重抛当前异常)
/// - 返回 false 表示当前异常不是 neograph 控制流异常, 交由其他分类分支处理
inline bool classifyNeographControlFlow(ExceptionClassification& out) noexcept {
    try {
        throw;
    } catch (const neograph::graph::CancelledException& e) {
        out.isControlFlow = true;
        out.controlKind   = ControlFlowKind::Cancelled;
        out.errInfo       = e.what();
        utilxx_base::autoConvertToUtf8(out.errInfo);
        out.exPtr = std::current_exception();
        return true;
    } catch (const neograph::graph::NodeInterrupt& e) {
        out.isControlFlow = true;
        out.controlKind   = ControlFlowKind::Interrupt;
        out.errInfo       = e.what();
        utilxx_base::autoConvertToUtf8(out.errInfo);
        out.exPtr = std::current_exception();
        return true;
    } catch (...) {
        return false;
    }
}

/// 把 neograph 控制流识别注册为 utilxx_base 的追加分类器
/// - 注册后, 通用库 (http/ws 等) 内部经 utilxx_base::catchError 捕获异常时,
///   图引擎的取消/中断异常同样按控制流向上传播, 而不会被当作普通错误吞掉
/// - 进程级生效, 幂等; 见 utilxx_base/exception.h 的 setExtraExceptionClassifier
inline void installExceptionClassifier() noexcept {
    utilxx_base::setExtraExceptionClassifier(&classifyNeographControlFlow);
}

/// 统一异常分类分析工具 (图引擎取消令牌版)
/// - 识别: utilxx::CancelledException / neograph 取消与中断 / 取消导致的
///   operation_aborted (按取消语义) / boost 异常 / 标准异常 / 未知异常
/// - 必须在 `catch` 块内调用
inline ExceptionClassification
    classifyCurrentException(const NeographCancelTokenPtr& cancelToken = nullptr) noexcept {
    // 首次使用即注册通用库侧的追加分类器 (见 installExceptionClassifier)
    static const bool kClassifierInstalled = [] {
        installExceptionClassifier();
        return true;
    }();
    (void)kClassifierInstalled;

    ExceptionClassification res;
    try {
        throw;
    } catch (const utilxx::CancelledException& e) {
        // 统一取消抽象 (utilxx::CancelToken) 抛出的取消异常: 与图引擎版同语义。
        // 异常指针统一归一化为图引擎取消异常, 使上层 (toolcall 节点等) 按
        // neograph::graph::CancelledException 判定取消的逻辑保持有效。
        res.isControlFlow = true;
        res.controlKind   = ControlFlowKind::Cancelled;
        res.errInfo       = e.what();
        utilxx_base::autoConvertToUtf8(res.errInfo);
        res.exPtr = std::make_exception_ptr(neograph::graph::CancelledException(res.errInfo));
    } catch (const neograph::graph::CancelledException& e) {
        res.isControlFlow = true;
        res.controlKind   = ControlFlowKind::Cancelled;
        res.errInfo       = e.what();
        utilxx_base::autoConvertToUtf8(res.errInfo);
        res.exPtr = std::current_exception();
    } catch (const neograph::graph::NodeInterrupt& e) {
        res.isControlFlow = true;
        res.controlKind   = ControlFlowKind::Interrupt;
        res.errInfo       = e.what();
        utilxx_base::autoConvertToUtf8(res.errInfo);
        res.exPtr = std::current_exception();
    } catch (const utilxx_base::AsioSystemError& e) {
        if (isCancelAbort(e, cancelToken)) {
            // 取消信号中断异步 IO 产生的 operation_aborted 按取消语义处理
            res.isControlFlow = true;
            res.controlKind   = ControlFlowKind::Cancelled;
            res.errInfo       = "operation aborted";
            res.exPtr
                = std::make_exception_ptr(neograph::graph::CancelledException("operation aborted"));
        } else {
            auto ec      = e.code();
            auto errInfo = std::string{e.what()};
            utilxx_base::autoConvertToUtf8(errInfo);
            if (ec == asio::error::operation_aborted) {
                res.errInfo = fmt::format("timeout: {}", errInfo);
            } else {
                res.errInfo = std::move(errInfo);
            }
            res.exPtr = std::current_exception();
        }
    } catch (const boost::exception& e) {
        // boost::exception 在 std::exception 之前捕获, 保留完整诊断信息
        res.errInfo = boost::diagnostic_information(e);
        utilxx_base::autoConvertToUtf8(res.errInfo);
        res.exPtr = std::current_exception();
    } catch (const std::exception& e) {
        // 部分系统上 (如 Windows) 系统函数返回的异常消息使用本地代码页, 需转为 UTF-8
        res.errInfo = e.what();
        utilxx_base::autoConvertToUtf8(res.errInfo);
        res.exPtr = std::current_exception();
    } catch (...) {
        res.errInfo = "unknown exception";
        res.exPtr   = std::current_exception();
    }
    return res;
}

template<typename T = void, typename Func, typename OnError, typename OnRethrow = std::nullptr_t>
    requires(!std::is_same_v<std::decay_t<OnRethrow>, NeographCancelTokenPtr>)
T catchError(
    Func&&                 func,
    OnError&&              onError,
    OnRethrow&&            onRethrow   = nullptr,
    NeographCancelTokenPtr cancelToken = nullptr
) {
    return utilxx_base::catchErrorImpl<T>(
        [&cancelToken]() noexcept {
            return classifyCurrentException(cancelToken);
        },
        std::forward<Func>(func),
        std::forward<OnError>(onError),
        std::forward<OnRethrow>(onRethrow)
    );
}

template<typename T = void, typename Func, typename OnError>
T catchError(Func&& func, OnError&& onError, NeographCancelTokenPtr cancelToken) {
    return catchError<T>(
        std::forward<Func>(func),
        std::forward<OnError>(onError),
        nullptr,
        std::move(cancelToken)
    );
}

template<typename T = void, typename Func, typename OnError, typename OnRethrow = std::nullptr_t>
    requires(!std::is_same_v<std::decay_t<OnRethrow>, NeographCancelTokenPtr>)
asio::awaitable<T> catchErrorAsync(
    Func&&                 func,
    OnError&&              onError,
    OnRethrow&&            onRethrow   = nullptr,
    NeographCancelTokenPtr cancelToken = nullptr
) {
    co_return co_await utilxx_base::catchErrorAsyncImpl<T>(
        // 按值捕获令牌 (shared_ptr 拷贝进协程帧), 避免协程挂起后引用失效
        [cancelToken]() noexcept {
            return classifyCurrentException(cancelToken);
        },
        std::forward<Func>(func),
        std::forward<OnError>(onError),
        std::forward<OnRethrow>(onRethrow)
    );
}

template<typename T = void, typename Func, typename OnError>
asio::awaitable<T>
    catchErrorAsync(Func&& func, OnError&& onError, NeographCancelTokenPtr cancelToken) {
    return catchErrorAsync<T>(
        std::forward<Func>(func),
        std::forward<OnError>(onError),
        nullptr,
        std::move(cancelToken)
    );
}

template<typename T, typename Func>
asio::awaitable<std::expected<T, std::string>>
    catchErrorToUnexpectedAsync(Func&& func, NeographCancelTokenPtr cancelToken = nullptr) {
    co_return co_await catchErrorAsync<std::expected<T, std::string>>(
        std::forward<Func>(func),
        [](std::string errmsg) -> asio::awaitable<std::expected<T, std::string>> {
            co_return std::unexpected<std::string>(std::move(errmsg));
        },
        nullptr,
        std::move(cancelToken)
    );
}

template<typename T, typename Func>
asio::awaitable<std::optional<T>>
    catchErrorToOptionalAsync(Func&& func, NeographCancelTokenPtr cancelToken = nullptr) {
    co_return co_await catchErrorAsync<std::optional<T>>(
        std::forward<Func>(func),
        [](std::string /*errmsg*/) -> asio::awaitable<std::optional<T>> {
            co_return std::nullopt;
        },
        nullptr,
        std::move(cancelToken)
    );
}

} // namespace util
} // namespace agentxx
