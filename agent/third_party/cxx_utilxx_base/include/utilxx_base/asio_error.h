/// asio 错误类型别名 (utilxx_base::AsioErrorCode / utilxx_base::AsioSystemError)
///
/// - 本库统一使用 **Boost.Asio**: 源码里的 `asio/xxx.hpp` 前缀由构建侧加入的
///   `${Boost_INCLUDE_DIRS}/boost/` include 根映射到 `boost/asio/xxx.hpp`
/// - `UTILXX_USE_BOOST_ASIO` 为本库统一开关 (构建侧 PUBLIC 定义), 打开后同时
///   提供全局别名 `namespace asio = ::boost::asio` (asio 前缀写法与 boost 类型统一);
///   `NEOGRAPH_USE_BOOST_ASIO` 为存量开关, 效果一致 (图引擎头使用同名宏)
/// - 本头仅做类型别名, 调用方须先 include asio/boost.system 相关头
///   (http_client.h/ws_client.h 等已具备)
#pragma once

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

#if defined(UTILXX_USE_BOOST_ASIO) || defined(NEOGRAPH_USE_BOOST_ASIO)
namespace asio = ::boost::asio;
#endif

namespace utilxx_base {

#if defined(UTILXX_USE_BOOST_ASIO) || defined(NEOGRAPH_USE_BOOST_ASIO)
using AsioSystemError = ::boost::system::system_error;
using AsioErrorCode   = ::boost::system::error_code;
#else
using AsioSystemError = ::asio::system_error;
using AsioErrorCode   = ::asio::error_code;
#endif

} // namespace utilxx_base
