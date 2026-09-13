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

namespace detail {

struct BuildDateHolder {
    char data[11]{};
};

/// 将 ANSI C 标准宏 `__DATE__` ("Mmm dd yyyy") 解析为 ISO 8601 日期 ("YYYY-MM-DD")
/// - [date] 为 11 字符的日期字符串 (如 "Mar 29 2026" 或 "Jan  5 2026")
constexpr BuildDateHolder parseBuildDate(const char* date = __DATE__) noexcept {
    char m0 = '0';
    char m1 = '0';
    if (date[0] == 'J' && date[1] == 'a' && date[2] == 'n') {
        m0 = '0';
        m1 = '1';
    } else if (date[0] == 'F' && date[1] == 'e' && date[2] == 'b') {
        m0 = '0';
        m1 = '2';
    } else if (date[0] == 'M' && date[1] == 'a' && date[2] == 'r') {
        m0 = '0';
        m1 = '3';
    } else if (date[0] == 'A' && date[1] == 'p' && date[2] == 'r') {
        m0 = '0';
        m1 = '4';
    } else if (date[0] == 'M' && date[1] == 'a' && date[2] == 'y') {
        m0 = '0';
        m1 = '5';
    } else if (date[0] == 'J' && date[1] == 'u' && date[2] == 'n') {
        m0 = '0';
        m1 = '6';
    } else if (date[0] == 'J' && date[1] == 'u' && date[2] == 'l') {
        m0 = '0';
        m1 = '7';
    } else if (date[0] == 'A' && date[1] == 'u' && date[2] == 'g') {
        m0 = '0';
        m1 = '8';
    } else if (date[0] == 'S' && date[1] == 'e' && date[2] == 'p') {
        m0 = '0';
        m1 = '9';
    } else if (date[0] == 'O' && date[1] == 'c' && date[2] == 't') {
        m0 = '1';
        m1 = '0';
    } else if (date[0] == 'N' && date[1] == 'o' && date[2] == 'v') {
        m0 = '1';
        m1 = '1';
    } else if (date[0] == 'D' && date[1] == 'e' && date[2] == 'c') {
        m0 = '1';
        m1 = '2';
    }

    const char d0 = (date[4] == ' ') ? '0' : date[4];
    const char d1 = date[5];

    BuildDateHolder res{};
    res.data[0] = date[7];
    res.data[1] = date[8];
    res.data[2] = date[9];
    res.data[3] = date[10];
    res.data[4] = '-';
    res.data[5] = m0;
    res.data[6] = m1;
    res.data[7] = '-';
    res.data[8] = d0;
    res.data[9] = d1;
    res.data[10] = '\0';
    return res;
}

inline constexpr BuildDateHolder kBuildDateHolder = parseBuildDate();

} // namespace detail

/// 项目发布版本字符串视图 (如 "0.1.0")
inline constexpr std::string_view kVersion = AGENTXX_VERSION_STRING;

/// 项目构建日期字符串视图 (ISO 8601 格式，如 "2026-03-29")
inline constexpr std::string_view kBuildDate{detail::kBuildDateHolder.data, 10};

/// 项目构建时间字符串视图 (如 "14:30:25")
inline constexpr std::string_view kBuildTime{__TIME__};

/// 主版本号 (Major)
inline constexpr int kVersionMajor = AGENTXX_VERSION_MAJOR;

/// 次版本号 (Minor)
inline constexpr int kVersionMinor = AGENTXX_VERSION_MINOR;

/// 修订版本号 (Patch)
inline constexpr int kVersionPatch = AGENTXX_VERSION_PATCH;

} // namespace agentxx
#endif
