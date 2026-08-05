#pragma once
#include "agentxx/util/string_util.h"
#include "asio/awaitable.hpp"
#include "boost/exception/diagnostic_information.hpp"
#include "boost/exception/exception.hpp"
#include "neograph/api.h"
#include "neograph/graph/cancel.h"
#include "neograph/graph/types.h"
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace agentxx {
namespace util {

/// 判断 system_error 是否为取消信号的作用结果
/// - 当 cancelToken 已取消时, 协程 co_await 点上出现的 operation_aborted 是
///   asio cancellation_signal 中断异步 IO 的结果, 属于取消语义而非超时/传输错误
inline bool isCancelAbort(
    const boost::system::system_error&                   e,
    const std::shared_ptr<neograph::graph::CancelToken>& cancelToken
) noexcept {
    return e.code() == asio::error::operation_aborted && nullptr != cancelToken
        && cancelToken->is_cancelled();
}

template<typename T>
T catchError(
    std::function<T()>                            func,
    std::function<T(std::string)>                 onError,
    std::function<std::optional<T>(std::string&)> onRethrow   = nullptr,
    std::shared_ptr<neograph::graph::CancelToken> cancelToken = nullptr
) {
    std::string errmsg;
    try {
        return func();
    } catch (const neograph::graph::CancelledException& e) {
        // 保留抛出，避免影响 BaseAgent 的取消、中断处理
        if (nullptr == onRethrow) {
            throw;
        }
        auto errInfo = std::string{e.what()};
        agentxx::util::autoConvertToUtf8(errInfo);
        auto result = onRethrow(errInfo);
        if (result.has_value()) {
            // std::move: 兼容 move-only 返回类型 (如 expected<unique_ptr>)
            return std::move(result.value());
        }
        errmsg = fmt::format("Cancelled: {}", errInfo);
    } catch (const neograph::graph::NodeInterrupt& e) {
        if (nullptr == onRethrow) {
            throw;
        }
        auto errInfo = std::string{e.what()};
        agentxx::util::autoConvertToUtf8(errInfo);
        auto result = onRethrow(errInfo);
        if (result.has_value()) {
            return std::move(result.value());
        }
        errmsg = fmt::format("NodeInterrupt: {}", errInfo);
    } catch (const boost::system::system_error& e) {
        if (isCancelAbort(e, cancelToken)) {
            // 取消信号中断异步 IO 产生的 operation_aborted 按取消语义处理 (同
            // CancelledException 分支), 避免取消被当作普通错误吞掉
            if (nullptr == onRethrow) {
                throw neograph::graph::CancelledException("operation aborted");
            }
            auto errInfo = std::string{"operation aborted"};
            auto result  = onRethrow(errInfo);
            if (result.has_value()) {
                return std::move(result.value());
            }
            errmsg = fmt::format("Cancelled: {}", errInfo);
        } else {
            auto ec      = e.code();
            auto errInfo = std::string{e.what()};
            agentxx::util::autoConvertToUtf8(errInfo);
            if (ec == asio::error::operation_aborted) {
                errmsg = fmt::format("timeout: {}", errInfo);
            } else {
                errmsg = errInfo;
            }
        }
    } catch (const std::exception& e) {
        // - 部分系统上，系统函数返回的
        // 异常消息字符编码是系统环境的字符编码 (windows)，而非总是utf8，因此这里需要转换
        errmsg = e.what();
        agentxx::util::autoConvertToUtf8(errmsg);
    } catch (const boost::exception& e) {
        errmsg = boost::diagnostic_information(e);
        agentxx::util::autoConvertToUtf8(errmsg);
    } catch (...) {
        errmsg = "unknown exception";
    }
    return onError(std::move(errmsg));
}

template<typename T>
asio::awaitable<T> catchErrorAsync(
    std::function<asio::awaitable<T>()>            func,
    std::function<asio::awaitable<T>(std::string)> onError,
    std::function<std::optional<T>(std::string&)>  onRethrow   = nullptr,
    std::shared_ptr<neograph::graph::CancelToken>  cancelToken = nullptr
) {
    std::string errmsg;
    try {
        co_return co_await func();
    } catch (const neograph::graph::CancelledException& e) {
        if (nullptr == onRethrow) {
            throw;
        }
        auto errInfo = std::string{e.what()};
        agentxx::util::autoConvertToUtf8(errInfo);
        auto result = onRethrow(errInfo);
        if (result.has_value()) {
            // std::move: 兼容 move-only 返回类型 (如 expected<unique_ptr>)
            co_return std::move(result.value());
        }
        errmsg = fmt::format("Cancelled: {}", errInfo);
    } catch (const neograph::graph::NodeInterrupt& e) {
        if (nullptr == onRethrow) {
            throw;
        }
        auto errInfo = std::string{e.what()};
        agentxx::util::autoConvertToUtf8(errInfo);
        auto result = onRethrow(errInfo);
        if (result.has_value()) {
            co_return std::move(result.value());
        }
        errmsg = fmt::format("NodeInterrupt: {}", errInfo);
    } catch (const boost::system::system_error& e) {
        if (isCancelAbort(e, cancelToken)) {
            // 取消信号中断异步 IO 产生的 operation_aborted 按取消语义处理 (同
            // CancelledException 分支), 避免取消被当作普通错误吞掉
            if (nullptr == onRethrow) {
                throw neograph::graph::CancelledException("operation aborted");
            }
            auto errInfo = std::string{"operation aborted"};
            auto result  = onRethrow(errInfo);
            if (result.has_value()) {
                co_return std::move(result.value());
            }
            errmsg = fmt::format("Cancelled: {}", errInfo);
        } else {
            auto ec      = e.code();
            auto errInfo = std::string{e.what()};
            agentxx::util::autoConvertToUtf8(errInfo);
            if (ec == asio::error::operation_aborted) {
                errmsg = fmt::format("timeout: {}", errInfo);
            } else {
                errmsg = errInfo;
            }
        }
    } catch (const std::exception& e) {
        errmsg = e.what();
        agentxx::util::autoConvertToUtf8(errmsg);
    } catch (const boost::exception& e) {
        errmsg = boost::diagnostic_information(e);
        agentxx::util::autoConvertToUtf8(errmsg);
    } catch (...) {
        errmsg = "unknown exception";
    }
    co_return co_await onError(std::move(errmsg));
}

template<typename T>
asio::awaitable<std::expected<T, std::string>>
    catchErrorToUnexpectedAsync(std::function<asio::awaitable<std::expected<T, std::string>>()> func
    ) {
    co_return co_await catchErrorAsync<std::expected<T, std::string>>(
        std::move(func),
        [](std::string errmsg) -> asio::awaitable<std::expected<T, std::string>> {
            co_return std::unexpected<std::string>(std::move(errmsg));
        }
    );
}

} // namespace util
} // namespace agentxx
