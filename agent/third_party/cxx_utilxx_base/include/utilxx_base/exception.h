#pragma once

/// 异常处理工具 (错误信息统一 UTF-8 化 / 控制流异常识别 / catchError 系列)
///
/// 本头提供**与宿主无关**的默认实现:
/// - 默认分类器 [classifyCurrentException] 只把 [utilxx::CancelledException] 认作取消,
///   其余异常一律按错误处理 (asio `operation_aborted` 记为超时, 消息前缀 "timeout: ");
/// - 宿主如有自己的控制流异常 (例如图引擎的取消/中断异常), 可用
///   [setExtraExceptionClassifier] 注册追加识别回调, 使这类异常在**本库被调用的
///   路径上**(http/ws 等) 同样按控制流向上传播, 而不是被当成错误吞掉。
///
/// 使用方式与图引擎版一致: 在 catch 块内调用 `catchError<T>(func, onError)`,
/// 控制流异常默认原样重抛 (传 onRethrow 时交给它处理)。

#include "utilxx_base/asio_error.h"
#include "utilxx_base/string_util.h"
#include "utilxx/cancel.h"
#include "asio/awaitable.hpp"
#include "boost/exception/diagnostic_information.hpp"
#include "boost/exception/exception.hpp"
#include <atomic>
#include <concepts>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace utilxx_base {

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

enum class ControlFlowKind {
    None,
    Cancelled,
    Interrupt,
};

struct ExceptionClassification {
    bool            isControlFlow = false; // 取消信号或中断异常
    ControlFlowKind controlKind   = ControlFlowKind::None;
    std::string     errInfo;
    std::exception_ptr exPtr;
};

/// 宿主追加的控制流识别回调
/// - 须在 catch 块内调用 (内部用 `throw;` 重抛当前异常并按类型判断)
/// - 返回 true 表示该异常属于控制流, 且已填好 `out`; false 表示不是本回调关心的类型
using ExtraExceptionClassifier = bool (*)(ExceptionClassification& out) noexcept;

/// 追加分类回调的进程级槽位 (宿主注册一次即可)
inline std::atomic<ExtraExceptionClassifier>& extraClassifierSlot() noexcept {
    static std::atomic<ExtraExceptionClassifier> slot{nullptr};
    return slot;
}

/// 注册追加的控制流识别回调 (传 nullptr 撤销); 线程安全, 进程级生效
inline void setExtraExceptionClassifier(ExtraExceptionClassifier fn) noexcept {
    extraClassifierSlot().store(fn, std::memory_order_release);
}

/// 读取当前注册的追加回调 (未注册返回 nullptr)
inline ExtraExceptionClassifier extraExceptionClassifier() noexcept {
    return extraClassifierSlot().load(std::memory_order_acquire);
}

/// 默认异常分类器
/// - 先在 catch 块内调用追加回调 (宿主自定义控制流异常, 见 [setExtraExceptionClassifier]);
/// - 再依次识别: 取消异常 → 取消语义; asio 系统错误 (operation_aborted 记为超时);
///   boost::exception (保留完整诊断信息); std::exception; 其余按未知异常处理;
/// - 错误消息统一转为 UTF-8 (部分平台上系统函数返回本地代码页消息)
///
/// 注意: 本函数必须在 `catch` 块内调用 (内部经 `throw;` 重启当前异常)
inline ExceptionClassification classifyCurrentException() noexcept {
    ExceptionClassification res;

    // 宿主追加的识别回调优先 (其内部自行按类型重抛, 未识别时返回 false)
    if (auto extra = extraExceptionClassifier(); extra != nullptr) {
        try {
            if (extra(res)) {
                return res;
            }
        } catch (...) {
            // 防御: 回调不应抛出; 抛出时退回默认分类
            res = ExceptionClassification{};
        }
    }

    try {
        throw;
    } catch (const utilxx::CancelledException& e) {
        res.isControlFlow = true;
        res.controlKind   = ControlFlowKind::Cancelled;
        res.errInfo       = e.what();
        autoConvertToUtf8(res.errInfo);
        res.exPtr = std::current_exception();
    } catch (const boost::system::system_error& e) {
        auto ec      = e.code();
        auto errInfo = std::string{e.what()};
        autoConvertToUtf8(errInfo);
        if (ec == asio::error::operation_aborted) {
            res.errInfo = fmt::format("timeout: {}", errInfo);
        } else {
            res.errInfo = std::move(errInfo);
        }
        res.exPtr = std::current_exception();
    } catch (const boost::exception& e) {
        // boost::exception 在 std::exception 之前捕获, 保留完整诊断信息
        res.errInfo = boost::diagnostic_information(e);
        autoConvertToUtf8(res.errInfo);
        res.exPtr = std::current_exception();
    } catch (const std::exception& e) {
        // 部分系统上 (如 Windows) 系统函数返回的异常消息使用本地代码页, 需转为 UTF-8
        res.errInfo = e.what();
        autoConvertToUtf8(res.errInfo);
        res.exPtr = std::current_exception();
    } catch (...) {
        res.errInfo = "unknown exception";
        res.exPtr   = std::current_exception();
    }
    return res;
}

/// catchError 的实现骨架 (分类器由调用方注入, 见 [catchError])
template<
    typename T = void,
    typename Classifier,
    typename Func,
    typename OnError,
    typename OnRethrow = std::nullptr_t>
T catchErrorImpl(
    Classifier&& classifier,
    Func&&       func,
    OnError&&    onError,
    OnRethrow&&  onRethrow = nullptr
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
        auto info = classifier();
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

/// 同步执行 func 并捕获异常, 把错误信息交给 onError; 控制流异常原样重抛
template<typename T = void, typename Func, typename OnError, typename OnRethrow = std::nullptr_t>
T catchError(Func&& func, OnError&& onError, OnRethrow&& onRethrow = nullptr) {
    return catchErrorImpl<T>(
        []() noexcept { return classifyCurrentException(); },
        std::forward<Func>(func),
        std::forward<OnError>(onError),
        std::forward<OnRethrow>(onRethrow)
    );
}

/// 协程版 [catchError] 的实现骨架 (分类器由调用方注入)
template<
    typename T = void,
    typename Classifier,
    typename Func,
    typename OnError,
    typename OnRethrow = std::nullptr_t>
asio::awaitable<T> catchErrorAsyncImpl(
    Classifier&& classifier,
    Func&&       func,
    OnError&&    onError,
    OnRethrow&&  onRethrow = nullptr
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
        auto info = classifier();
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

/// 协程版 [catchError]
template<typename T = void, typename Func, typename OnError, typename OnRethrow = std::nullptr_t>
asio::awaitable<T>
    catchErrorAsync(Func&& func, OnError&& onError, OnRethrow&& onRethrow = nullptr) {
    co_return co_await catchErrorAsyncImpl<T>(
        []() noexcept { return classifyCurrentException(); },
        std::forward<Func>(func),
        std::forward<OnError>(onError),
        std::forward<OnRethrow>(onRethrow)
    );
}

/// 协程版: 错误转为 `std::unexpected` 返回 (控制流异常仍抛出)
template<typename T, typename Func>
asio::awaitable<std::expected<T, std::string>> catchErrorToUnexpectedAsync(Func&& func) {
    co_return co_await catchErrorAsync<std::expected<T, std::string>>(
        std::forward<Func>(func),
        [](std::string errmsg) -> asio::awaitable<std::expected<T, std::string>> {
            co_return std::unexpected<std::string>(std::move(errmsg));
        }
    );
}

/// 协程版: 错误转为 `std::nullopt` 返回 (控制流异常仍抛出)
template<typename T, typename Func>
asio::awaitable<std::optional<T>> catchErrorToOptionalAsync(Func&& func) {
    co_return co_await catchErrorAsync<std::optional<T>>(
        std::forward<Func>(func),
        [](std::string /*errmsg*/) -> asio::awaitable<std::optional<T>> { co_return std::nullopt; }
    );
}

} // namespace utilxx_base
