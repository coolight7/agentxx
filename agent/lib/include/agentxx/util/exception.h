#pragma once
#include "agentxx/util/string_util.h"
#include "asio/awaitable.hpp"
#include "boost/exception/diagnostic_information.hpp"
#include "boost/exception/exception.hpp"
#include "neograph/api.h"
#include "neograph/graph/cancel.h"
#include "neograph/graph/types.h"
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

/// 判断可调用对象是否为空 (兼容 nullptr_t, std::function, 指针及普通 lambda)
template<typename F>
constexpr bool isNullCallable(const F& f) noexcept {
    if constexpr (std::is_same_v<std::decay_t<F>, std::nullptr_t>) {
        return true;
    } else if constexpr (requires { bool(f); }) {
        return !f;
    } else {
        return false;
    }
}

/// 判断 system_error 是否为取消信号的作用结果
/// - 当 cancelToken 已取消时, 协程 co_await 点上出现的 operation_aborted 是
///   asio cancellation_signal 中断异步 IO 的结果, 属于取消语义而非超时/传输错误
inline bool isCancelAbort(
    const neograph_asio_system_error&                    e,
    const std::shared_ptr<neograph::graph::CancelToken>& cancelToken
) noexcept {
    return e.code() == asio::error::operation_aborted && nullptr != cancelToken
           && cancelToken->is_cancelled();
}

enum class ControlFlowKind {
    None,
    Cancelled,
    Interrupt,
};

struct ExceptionClassification {
    bool               isControlFlow = false; // 取消信号或中断异常
    ControlFlowKind    controlKind   = ControlFlowKind::None;
    std::string        errInfo;
    std::exception_ptr exPtr;
};

/// 统一异常分类分析工具
inline ExceptionClassification classifyCurrentException(
    const std::shared_ptr<neograph::graph::CancelToken>& cancelToken = nullptr
) noexcept {
    ExceptionClassification res;
    try {
        throw;
    } catch (const neograph::graph::CancelledException& e) {
        res.isControlFlow = true;
        res.controlKind   = ControlFlowKind::Cancelled;
        res.errInfo       = e.what();
        agentxx::util::autoConvertToUtf8(res.errInfo);
        res.exPtr = std::current_exception();
    } catch (const neograph::graph::NodeInterrupt& e) {
        res.isControlFlow = true;
        res.controlKind   = ControlFlowKind::Interrupt;
        res.errInfo       = e.what();
        agentxx::util::autoConvertToUtf8(res.errInfo);
        res.exPtr = std::current_exception();
    } catch (const neograph_asio_system_error& e) {
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
            agentxx::util::autoConvertToUtf8(errInfo);
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
        agentxx::util::autoConvertToUtf8(res.errInfo);
        res.exPtr = std::current_exception();
    } catch (const std::exception& e) {
        // 部分系统上 (如 Windows) 系统函数返回的异常消息使用本地代码页, 需转为 UTF-8
        res.errInfo = e.what();
        agentxx::util::autoConvertToUtf8(res.errInfo);
        res.exPtr = std::current_exception();
    } catch (...) {
        res.errInfo = "unknown exception";
        res.exPtr   = std::current_exception();
    }
    return res;
}

template<typename T = void, typename Func, typename OnError, typename OnRethrow = std::nullptr_t>
    requires(!std::
                 is_same_v<std::decay_t<OnRethrow>, std::shared_ptr<neograph::graph::CancelToken>>)
T catchError(
    Func&&                                        func,
    OnError&&                                     onError,
    OnRethrow&&                                   onRethrow   = nullptr,
    std::shared_ptr<neograph::graph::CancelToken> cancelToken = nullptr
) {
    std::string errmsg;
    try {
        if constexpr (std::is_void_v<T>) {
            func();
            return;
        } else {
            return func();
        }
    } catch (...) {
        auto info = classifyCurrentException(cancelToken);
        if (info.isControlFlow) {
            if constexpr (!std::is_same_v<std::decay_t<OnRethrow>, std::nullptr_t>) {
                if (!isNullCallable(onRethrow)) {
                    if constexpr (std::is_void_v<T>) {
                        onRethrow(info.errInfo);
                        return;
                    } else {
                        auto result = onRethrow(info.errInfo);
                        if (result.has_value()) {
                            return std::move(result.value());
                        }
                    }
                } else {
                    std::rethrow_exception(info.exPtr);
                }
            } else {
                std::rethrow_exception(info.exPtr);
            }
            std::string prefix
                = (info.controlKind == ControlFlowKind::Interrupt) ? "NodeInterrupt" : "Cancelled";
            errmsg = fmt::format("{}: {}", prefix, info.errInfo);
        } else {
            errmsg = std::move(info.errInfo);
        }
    }
    if constexpr (std::is_void_v<T>) {
        onError(std::move(errmsg));
        return;
    } else {
        return onError(std::move(errmsg));
    }
}

template<typename T = void, typename Func, typename OnError>
T catchError(
    Func&&                                        func,
    OnError&&                                     onError,
    std::shared_ptr<neograph::graph::CancelToken> cancelToken
) {
    return catchError<T>(
        std::forward<Func>(func),
        std::forward<OnError>(onError),
        nullptr,
        std::move(cancelToken)
    );
}

template<typename T = void, typename Func, typename OnError, typename OnRethrow = std::nullptr_t>
    requires(!std::
                 is_same_v<std::decay_t<OnRethrow>, std::shared_ptr<neograph::graph::CancelToken>>)
asio::awaitable<T> catchErrorAsync(
    Func&&                                        func,
    OnError&&                                     onError,
    OnRethrow&&                                   onRethrow   = nullptr,
    std::shared_ptr<neograph::graph::CancelToken> cancelToken = nullptr
) {
    std::string errmsg;
    try {
        if constexpr (std::is_void_v<T>) {
            co_await func();
            co_return;
        } else {
            co_return co_await func();
        }
    } catch (...) {
        auto info = classifyCurrentException(cancelToken);
        if (info.isControlFlow) {
            if constexpr (!std::is_same_v<std::decay_t<OnRethrow>, std::nullptr_t>) {
                if (!isNullCallable(onRethrow)) {
                    if constexpr (std::is_void_v<T>) {
                        onRethrow(info.errInfo);
                        co_return;
                    } else {
                        auto result = onRethrow(info.errInfo);
                        if (result.has_value()) {
                            co_return std::move(result.value());
                        }
                    }
                } else {
                    std::rethrow_exception(info.exPtr);
                }
            } else {
                std::rethrow_exception(info.exPtr);
            }
            std::string prefix
                = (info.controlKind == ControlFlowKind::Interrupt) ? "NodeInterrupt" : "Cancelled";
            errmsg = fmt::format("{}: {}", prefix, info.errInfo);
        } else {
            errmsg = std::move(info.errInfo);
        }
    }
    if constexpr (std::is_void_v<T>) {
        co_await onError(std::move(errmsg));
        co_return;
    } else {
        co_return co_await onError(std::move(errmsg));
    }
}

template<typename T = void, typename Func, typename OnError>
asio::awaitable<T> catchErrorAsync(
    Func&&                                        func,
    OnError&&                                     onError,
    std::shared_ptr<neograph::graph::CancelToken> cancelToken
) {
    return catchErrorAsync<T>(
        std::forward<Func>(func),
        std::forward<OnError>(onError),
        nullptr,
        std::move(cancelToken)
    );
}

template<typename T, typename Func>
asio::awaitable<std::expected<T, std::string>> catchErrorToUnexpectedAsync(
    Func&&                                        func,
    std::shared_ptr<neograph::graph::CancelToken> cancelToken = nullptr
) {
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
asio::awaitable<std::optional<T>> catchErrorToOptionalAsync(
    Func&&                                        func,
    std::shared_ptr<neograph::graph::CancelToken> cancelToken = nullptr
) {
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
