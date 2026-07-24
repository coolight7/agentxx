#pragma once
#include "agentxx/util/string_util.h"
#include "asio/awaitable.hpp"
#include "boost/exception/diagnostic_information.hpp"
#include "boost/exception/exception.hpp"
#include "neograph/api.h"
#include <exception>
#include <functional>
#include <string>

#define AGENTXX_CATCH_EXCEPTION_D(errInfo, code)           \
    catch (const std::exception& e) {                      \
        {code} errInfo = e.what();                         \
    }                                                      \
    catch (const boost::exception& e) {                    \
        {code} errInfo = boost::diagnostic_information(e); \
    }                                                      \
    catch (...) {                                          \
        code                                               \
    }

namespace agentxx {
namespace util {

template<typename T>
asio::awaitable<T> catchErrorAsync(
    std::function<asio::awaitable<T>()>            func,
    std::function<asio::awaitable<T>(std::string)> onError
) {
    std::string errmsg;
    try {
        co_return co_await func();
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
