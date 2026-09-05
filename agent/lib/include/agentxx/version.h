#pragma once

/// agentxx/version.h —— Agentxx 项目统一软件发布版本定义
///
/// - 单一权威真实源: [agent/VERSION](agent/VERSION)
/// - CMake 在编译时注入 `AGENTXX_VERSION_STRING`、`AGENTXX_VERSION_MAJOR` 等宏
/// - 若未通过 CMake 编译 (如代码静态分析、未生成缓存的 IDE 打开)，使用下方默认 fallback 值
/// - 区分: 此处为软件发行版本 (Release Version)，不可与插件 ABI 契约版本
///   (`AGENTXX_PLUGIN_API_VERSION`) 或 FFI 协议版本混淆。

#ifndef AGENTXX_VERSION_STRING
#define AGENTXX_VERSION_STRING "0.1.0"
#endif

#ifndef AGENTXX_VERSION_MAJOR
#define AGENTXX_VERSION_MAJOR 0
#endif

#ifndef AGENTXX_VERSION_MINOR
#define AGENTXX_VERSION_MINOR 1
#endif

#ifndef AGENTXX_VERSION_PATCH
#define AGENTXX_VERSION_PATCH 0
#endif

#ifdef __cplusplus
#include <string_view>

namespace agentxx {

/// 项目发布版本字符串视图 (如 "0.1.0")
inline constexpr std::string_view kVersion = AGENTXX_VERSION_STRING;

/// 主版本号 (Major)
inline constexpr int kVersionMajor = AGENTXX_VERSION_MAJOR;

/// 次版本号 (Minor)
inline constexpr int kVersionMinor = AGENTXX_VERSION_MINOR;

/// 修订版本号 (Patch)
inline constexpr int kVersionPatch = AGENTXX_VERSION_PATCH;

} // namespace agentxx
#endif
