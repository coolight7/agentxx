#pragma once

/// utilxx 版本标识
///
/// - 版本号由构建侧经宏 `XX_VERSION_STRING` 注入 (superbuild 传入项目版本号);
///   未注入时回退 "0.1.0"
/// - 用途: HTTP 服务端 `Server` 头与客户端 `User-Agent` 等对外标识中的版本部分
/// - 区分: 此处为软件发行版本, 与插件 ABI 契约版本 (AGENTXX_PLUGIN_API_VERSION)
///   无关

#ifndef XX_VERSION_STRING
#define XX_VERSION_STRING "0.1.0"
#endif

#include <string_view>

namespace utilxx_base {

/// 项目发布版本字符串视图 (如 "0.1.0")
inline constexpr std::string_view kVersion = XX_VERSION_STRING;

} // namespace utilxx_base
