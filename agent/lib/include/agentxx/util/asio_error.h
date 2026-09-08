/// asio 错误类型别名 (agentxx::util::AsioErrorCode / AsioSystemError)
///
/// - 从 `neograph/define.h` 的 `neograph_asio_*` 迁移而来: util 层不再依赖
///   图引擎头, asio 类型别名由 util 自有头承载
/// - 本头仅做类型别名, 调用方须先 include asio/boost.system 相关头
///   (http_client.h/ws_client.h 等已具备), 与原 neograph/define.h 用法一致
#pragma once

// 全局别名保持与 neograph/define.h 相同的条件语义:
// NEOGRAPH_USE_BOOST_ASIO 由构建系统统一定义 (见 lib/plugins/CMakeLists)
// - asio 独立头模式 (BOOST_ASIO_STANDALONE) 下 <boost/asio.hpp> 提供 boost::asio,
//   与 neograph/define.h 的 `namespace asio = ::boost::asio` 一致
#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#ifdef NEOGRAPH_USE_BOOST_ASIO
namespace asio = ::boost::asio;
using agentxx_asio_system_error = ::boost::system::system_error;
using agentxx_asio_error_code   = ::boost::system::error_code;
#else
using agentxx_asio_system_error = ::asio::system_error;
using agentxx_asio_error_code   = ::asio::error_code;
#endif

namespace agentxx {
namespace util {

#ifdef NEOGRAPH_USE_BOOST_ASIO
using AsioSystemError = ::boost::system::system_error;
using AsioErrorCode   = ::boost::system::error_code;
#else
using AsioSystemError = ::asio::system_error;
using AsioErrorCode   = ::asio::error_code;
#endif

} // namespace util
} // namespace agentxx

// 存量代码兼容别名 (与 neograph/define.h 同名, 便于渐进迁移)
using neograph_asio_system_error = agentxx_asio_system_error;
using neograph_asio_error_code   = agentxx_asio_error_code;
