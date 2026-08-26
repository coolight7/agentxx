#pragma once

#include "agentxx/util/log.h"
#include <algorithm>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <iomanip>
#include <optional>
#include <queue>
#include <ranges>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace agentxx {
namespace util {

inline static constexpr int CODE_0 = 48;
inline static constexpr int CODE_9 = 57;
inline static constexpr int CODE_A = 65;
inline static constexpr int CODE_Z = 90;
inline static constexpr int CODE_a = 97;
inline static constexpr int CODE_z = 122;

using PinyinCallback = std::function<std::string(std::string_view)>;

// constexpr 字符操作辅助函数，替代非 constexpr 的
// std::tolower/std::toupper/std::isspace
[[nodiscard]] constexpr char charToLower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

[[nodiscard]] constexpr char charToUpper(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
}

[[nodiscard]] constexpr bool charIsSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

[[nodiscard]] inline constexpr bool isCode_num(int code) {
    return (code >= CODE_0 && code <= CODE_9);
}

[[nodiscard]] inline constexpr bool isCode_AZ(int code) {
    return (code >= CODE_A && code <= CODE_Z);
}

[[nodiscard]] inline constexpr bool isCode_az(int code) {
    return (code >= CODE_a && code <= CODE_z);
}

[[nodiscard]] inline constexpr bool isCode_AZaz(int code) {
    return (isCode_AZ(code) || isCode_az(code));
}

[[nodiscard]] inline constexpr std::optional<int> toCode_tryAZ(int code) {
    if (isCode_az(code)) {
        return code - (CODE_a - CODE_A);
    } else if (isCode_AZ(code)) {
        return code;
    }
    return std::nullopt;
}

[[nodiscard]] inline constexpr std::optional<int> toCode_tryaz(int code) {
    if (isCode_az(code)) {
        return code;
    } else if (isCode_AZ(code)) {
        return code + (CODE_a - CODE_A);
    }
    return std::nullopt;
}

[[nodiscard]] inline constexpr int toCode_mayAZ(int code) {
    auto result = toCode_tryAZ(code);
    return result.has_value() ? result.value() : code;
}

[[nodiscard]] inline constexpr int toCode_mayaz(int code) {
    auto result = toCode_tryaz(code);
    return result.has_value() ? result.value() : code;
}

[[nodiscard]] inline constexpr int toCode_AZ(int code) {
    if (isCode_az(code)) {
        return code - (CODE_a - CODE_A);
    }
    if (!std::is_constant_evaluated()) {
        assert(isCode_AZ(code));
    }
    return code;
}

[[nodiscard]] inline constexpr int toCode_az(int code) {
    if (isCode_AZ(code)) {
        return code + (CODE_a - CODE_A);
    }
    if (!std::is_constant_evaluated()) {
        assert(isCode_az(code));
    }
    return code;
}

inline constexpr void toUpperSelf(std::string& str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
        return charToUpper(static_cast<char>(c));
    });
}

[[nodiscard]] inline constexpr std::string toUpper(std::string_view str) {
    auto result = std::string{str};
    toUpperSelf(result);
    return result;
}

inline constexpr void toLowerSelf(std::string& str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
        return charToLower(static_cast<char>(c));
    });
}

[[nodiscard]] inline constexpr std::string toLower(std::string_view str) {
    auto result = std::string{str};
    toLowerSelf(result);
    return result;
}

// 不区分大小写哈希
struct IgnoreCaseHash {
    using is_transparent = void;

    size_t operator()(const std::string& s) const {
        return std::hash<std::string>()(toLower(s));
    }

    size_t operator()(std::string_view s) const {
        return std::hash<std::string>()(toLower(s));
    }

    // 支持 const char* 透明查找 (字符串字面量), 否则字面量同时可转换到
    // std::string 与 std::string_view, 导致重载解析歧义无法编译
    size_t operator()(const char* s) const {
        return (*this)(std::string_view{s});
    }
};

// 不区分大小写相等判断
struct IgnoreCaseEqual {
    using is_transparent = void;

    inline static constexpr bool equal(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            if (charToLower(static_cast<char>(static_cast<unsigned char>(a[i])))
                != charToLower(static_cast<char>(static_cast<unsigned char>(b[i])))) {
                return false;
            }
        }
        return true;
    }

    template<typename T1, typename T2>
    constexpr bool operator()(const T1& a, const T2& b) const {
        return equal(a, b);
    }
};

template<typename V>
using IgnoreCaseMap = std::unordered_map<std::string, V, IgnoreCaseHash, IgnoreCaseEqual>;

using IgnoreCaseSet = std::unordered_set<std::string, IgnoreCaseHash, IgnoreCaseEqual>;

[[nodiscard]] inline constexpr size_t utf8GetLength(std::string_view in_str) {
    size_t length = 0;
    for (size_t i = 0, step = 0; i < in_str.size(); i += step) {
        unsigned char byte = in_str[i];
        // lenght 6
        if (byte >= 0xFC) {
            step = 6;
        } else if (byte >= 0xF8) {
            step = 5;
        } else if (byte >= 0xF0) {
            step = 4;
        } else if (byte >= 0xE0) {
            step = 3;
        } else if (byte >= 0xC0) {
            step = 2;
        } else {
            step = 1;
        }
        length++;
    }
    return length;
}

[[nodiscard]] inline constexpr size_t
    findIndexByUtf8Length(std::string_view in_str, size_t targetLen, size_t start = 0) {
    size_t count = 0;
    size_t i = start, step = 0;
    for (; i < in_str.size();) {
        unsigned char byte = in_str[i];
        // lenght 6
        if (byte >= 0xFC) {
            step = 6;
        } else if (byte >= 0xF8) {
            step = 5;
        } else if (byte >= 0xF0) {
            step = 4;
        } else if (byte >= 0xE0) {
            step = 3;
        } else if (byte >= 0xC0) {
            step = 2;
        } else {
            step = 1;
        }
        ++count;
        i += step;

        if (count >= targetLen) {
            return i;
        }
    }
    // length not found
    return 0;
}

/// return <index, lineCount, lastLineIndex>
[[nodiscard]] inline constexpr std::tuple<size_t, size_t, size_t>
    findIndexAndLastLineIndexByUtf8Length(std::string_view in_str, size_t targetLen) {
    if (in_str.size() >= targetLen) {
        size_t count = 0, lineCount = 0, lastLineIndex = 0;
        size_t i = 0, step = 0;
        for (; i < in_str.size();) {
            const unsigned char byte = in_str[i];
            // lenght 6
            if (byte >= 0xFC) {
                step = 6;
            } else if (byte >= 0xF8) {
                step = 5;
            } else if (byte >= 0xF0) {
                step = 4;
            } else if (byte >= 0xE0) {
                step = 3;
            } else if (byte >= 0xC0) {
                step = 2;
            } else {
                step = 1;
                if (byte == (unsigned char)'\n') {
                    ++lineCount;
                    lastLineIndex = i;
                }
            }
            ++count;
            i += step;

            if (count >= targetLen) {
                return std::tuple<size_t, size_t, size_t>{i, lineCount, lastLineIndex};
            }
        }
    }
    // length not found
    return std::tuple<size_t, size_t, size_t>{0, 0, 0};
}

/// 统计文本的行数: '\n' 数量 + 末尾不以 '\n' 结尾的最后一个不完整行
/// 与 `agentxx_share_store` 按行分页取值的行划分方式一致 (逐行读取)
[[nodiscard]] inline constexpr size_t countLines(std::string_view in_str) {
    if (in_str.empty()) {
        return 0;
    }
    size_t lineCount = 0;
    for (const char ch : in_str) {
        if ('\n' == ch) {
            ++lineCount;
        }
    }
    if ('\n' != in_str.back()) {
        // 最后一行没有换行符, 也算一行
        ++lineCount;
    }
    return lineCount;
}

[[nodiscard]] inline constexpr bool utf8IsContinuationChar(unsigned char ch) {
    return (ch & 0xC0) == 0x80; // 10xxxxxx 的二进制特征：前两位是 10
}

[[nodiscard]] inline constexpr size_t utf8GetLengthCheckAvail(std::string_view str) {
    size_t     length = 0;
    const auto strLen = str.length();
    for (size_t i = 0, step = 0; i < strLen; i += step) {
        unsigned char ch = str[i];
        if (ch == '\0') {
            break;
        } else if (ch >= 0xF8) {
            // (ch >= 0xFC || ch >= 0xF8)
            // lenght 6、5 无效
            return 0;
        } else if (ch >= 0xF0) {
            if (ch == 0xF0 && i + 1 < strLen
                && (static_cast<unsigned char>(str[i + 1]) & 0xF0) == 0x80) {
                // 检查非最短编码长度，对应 0~0xFFFF
                return 0;
            }
            step = 4;
        } else if (ch >= 0xE0) {
            if (ch == 0xE0 && i + 1 < strLen
                && (static_cast<unsigned char>(str[i + 1]) & 0xE0) == 0x80) {
                // 0xE0 0x80~0x9F 对应 0~0x7FF
                return 0;
            }
            // 部分转换需要用 � 替代非法编码，因此这里放行
            // if (ch == 0xEF && i + 2 < strLen) {
            //     unsigned char ch1 = static_cast<unsigned char>(str[i + 1]);
            //     unsigned char ch2 = static_cast<unsigned char>(str[i + 2]);
            //     if (ch1 == 0xBF && ch2 == 0xBD) {
            //         return 0; // 匹配�，判定为无效UTF-8
            //     }
            // }
            step = 3;
        } else if (ch >= 0xC0) {
            if (ch == 0xC0 || ch == 0xC1) {
                // 对应 0~0x3F
                return 0;
            }
            step = 2;
        } else {
            if (str[i] < 0) {
                return 0;
            }
            step = 1;
        }

        if (step > 1) {
            for (size_t j = 1; j < step; j++) {
                if (i + j >= strLen || str[i + j] == '\0'
                    || false == utf8IsContinuationChar((unsigned char)str[i + j])) {
                    // 不合规
                    return 0;
                }
            }
        }
        length++;
    }
    return length;
}

[[nodiscard]] inline constexpr bool utf8IsAvail(std::string_view str) {
    if (str.empty() || str[0] == '\0') {
        return true;
    }
    return utf8GetLengthCheckAvail(str) != 0;
}

/// 修复非法 UTF-8: 将每一处非法字节序列 (按 Unicode 推荐的
/// "maximal subpart" 规则划分的最大非法子部分) 替换为占位符 U+FFFD (EF BF BD)
/// - 非法序列包括: 孤立延续字节 (10xxxxxx 无前导)、5/6 字节头 (0xF8~0xFF)、
///   过短编码头 (0xC0/0xC1)、非最短编码序列 (0xE0 0x80..、0xF0 0x80..)、
///   序列中途出现非延续字节、末尾被截断的多字节序列
/// - 合法的多字节序列与 ASCII 原样保留; 非法序列只替换为一个 U+FFFD,
///   且其后紧跟的合法字符 (如 ASCII) 不会被吞掉
/// @return true 表示输入含非法序列且已被修复, false 表示输入本就合法未做修改
[[nodiscard]] inline constexpr bool utf8Repair(std::string& str) {
    constexpr char kReplacement[] = {'\xEF', '\xBF', '\xBD'}; // U+FFFD 的 UTF-8 编码
    const size_t   size           = str.size();

    // 快速路径: 输入本就合法则不做任何修改直接返回
    if (size == 0 || utf8GetLengthCheckAvail(str) != 0) {
        return false;
    }

    std::string result;
    result.reserve(size);

    for (size_t i = 0; i < size;) {
        const unsigned char byte = static_cast<unsigned char>(str[i]);

        if (byte < 0x80) {
            // ASCII
            result += str[i++];
            continue;
        }

        if (utf8IsContinuationChar(byte)) {
            // 孤立延续字节: 一个 maximal subpart 只含这一个字节,
            // 后续字节可能是新的合法序列前导, 不能一并吞掉
            result.append(kReplacement, 3);
            ++i;
            continue;
        }

        if (byte < 0xC2) {
            // 0x80~0xBF 已在上方处理, 此处即 0xC0/0xC1:
            // 过短编码 (只能表示 0~0x7F, 必须用 1 字节 ASCII 表示)
            result.append(kReplacement, 3);
            ++i;
            continue;
        }

        if (byte >= 0xF8) {
            // 0xF8~0xFF: 5/6 字节头或无效头, UTF-8 最多 4 字节, 单字节非法
            result.append(kReplacement, 3);
            ++i;
            continue;
        }

        // 多字节前导: 计算期望序列长度与第二个字节范围要求,
        // 与 utf8GetLengthCheckAvail 的校验规则保持一致
        size_t step;
        if (byte >= 0xF0) {
            step = 4;
        } else if (byte >= 0xE0) {
            step = 3;
        } else {
            step = 2; // 0xC2~0xDF
        }

        size_t j = 1;
        for (; j < step && i + j < size; ++j) {
            const unsigned char c = static_cast<unsigned char>(str[i + j]);
            if (byte == 0xE0 && j == 1 && c < 0xA0) {
                // 0xE0 0x80~0x9F: 非最短编码 (对应 U+0000~U+07FF)
                break;
            }
            if (byte == 0xF0 && j == 1 && c < 0x90) {
                // 0xF0 0x80~0x8F: 非最短编码 (对应 U+0000~U+FFFF)
                break;
            }
            if (false == utf8IsContinuationChar(c)) {
                break;
            }
        }

        if (j == step) {
            // 序列合法, 原样拷贝
            result.append(str.data() + i, step);
            i += step;
        } else {
            // 非法序列: 替换其 maximal subpart (已消费的 j 个字节)
            result.append(kReplacement, 3);
            i += j;
        }
    }

    str = std::move(result);
    return true;
}

template<typename T>
[[nodiscard]] inline std::string
    stringVectorJoin(const std::vector<T>& list, std::string_view sep = ", ") {
    std::ostringstream oss;
    auto               len = list.size();
    for (size_t i = 0; i < len; ++i) {
        oss << list[i];
        if (i < len - 1) {
            oss << sep;
        }
    }
    return oss.str();
}

[[nodiscard]] inline std::vector<std::string_view>
    strSplit(std::string_view in_str, char delim) noexcept {
    auto                          split = in_str | std::views::split(delim);
    std::vector<std::string_view> result;

    for (auto&& sub : split) {
        if (sub.empty()) {
            result.emplace_back();
        } else {
            result.emplace_back(&*sub.begin(), sub.size());
        }
    }
    return result;
}

[[nodiscard]] inline std::vector<std::string> strSplitCopid(std::string_view in_str, char delim) {
    auto                     split = in_str | std::views::split(delim);
    std::vector<std::string> result;

    for (const auto& sub : split) {
        result.emplace_back(sub.begin(), sub.end());
    }
    return result;
}

[[nodiscard]] inline constexpr std::string_view toStringNotNull(const char* str) {
    if (nullptr == str) {
        return std::string_view{""};
    }
    return std::string_view{str};
}

[[nodiscard]] std::string base64Encode(std::string_view data);

/// base64 解码
/// - 返回 nullopt 表示输入不是合法 base64; 合法但解码为空时返回 optional("")
///   (用以区分 "非法输入" 与 "空结果", 例如写入空二进制文件)
[[nodiscard]] std::optional<std::string> base64Decode(std::string_view str);

[[nodiscard]] std::tuple<bool, std::optional<std::string>> convertCharset(
    std::string_view src,
    std::string_view srcEncoding,
    std::string_view targetEncoding
);

[[nodiscard]] std::tuple<bool, std::optional<std::string>> autoConvertCharset(
    std::string_view str,
    std::string&     encoding,
    std::string_view targetEncoding
);

[[nodiscard]] std::tuple<bool, std::optional<std::string>>
    autoConvertToUtf8(std::string_view str, bool _);

bool autoConvertToUtf8(std::string& str);

[[nodiscard]] std::string autoTryConvertToUtf8(std::string_view str);

/// 自动转换为系统路径编码
/// - [windows] UTF-16LE
/// - [其他系统] UTF-8
bool autoConvertToSystemPath(std::string& str);

template<typename T>
inline constexpr std::from_chars_result parseNumberFromString(std::string_view str, T& num) {
    return std::from_chars(str.data(), str.data() + str.size(), num);
}

inline PinyinCallback s_pinyinCallback = nullptr;

[[nodiscard]] inline constexpr std::string removeAllSpace(std::string_view str) {
    std::string result;
    result.reserve(str.size());
    for (char c : str) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            result += c;
        }
    }
    return result;
}

[[nodiscard]] inline constexpr std::optional<std::string> removeAllSpaceMayNull(std::string_view str
) {
    if (str.empty()) {
        return std::nullopt;
    }
    std::string result = removeAllSpace(str);
    if (result.empty()) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] inline constexpr std::string removeBetweenSpace(
    std::string_view str,
    bool             removeLine = true,
    bool             subLeft    = true,
    bool             subRight   = true
) {
    if (!std::is_constant_evaluated()) {
        assert(subLeft || subRight);
    }

    if (str.empty()) {
        return std::string{str};
    }

    int left  = 0;
    int right = static_cast<int>(str.size()) - 1;

    if (subRight) {
        for (; right >= left; --right) {
            if (str[right] != ' ' && str[right] != '\t'
                && (!removeLine || (str[right] != '\r' && str[right] != '\n'))) {
                break;
            }
        }
    }

    if (subLeft) {
        for (; left <= right; ++left) {
            if (str[left] != ' ' && str[left] != '\t'
                && (!removeLine || (str[left] != '\r' && str[left] != '\n'))) {
                break;
            }
        }
    }

    if (left <= right) {
        return std::string{str.substr(left, right - left + 1)};
    } else {
        return "";
    }
}

[[nodiscard]] inline constexpr std::optional<std::string> removeBetweenSpaceMayNull(
    std::string_view str,
    bool             removeLine = true,
    bool             subLeft    = true,
    bool             subRight   = true
) {
    if (str.empty()) {
        return std::nullopt;
    }

    std::string result = removeBetweenSpace(str, removeLine, subLeft, subRight);
    if (result.empty()) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::string getFirstWordPinyin(std::string_view str);

[[nodiscard]] std::string getFirstCharPinyinFast(std::string_view str);

[[nodiscard]] std::optional<int> getComparableCode(std::string_view str, size_t index);

[[nodiscard]] std::optional<std::string>
    getFirstCharPinyin(std::string_view str, bool enableAZ = true, bool enableNum = true);

[[nodiscard]] std::optional<std::string> getFirstCharPinyinFirstChar(std::string_view str);

[[nodiscard]] int compareExtend(std::string_view left, std::string_view right);

[[nodiscard]] inline constexpr std::string collapseSlashes(std::string_view path) {
    std::string result;
    result.reserve(path.size());
    bool prevSlash = false;
    for (char c : path) {
        if (c == '/') {
            if (!prevSlash) {
                result += c;
            }
            prevSlash = true;
        } else {
            result    += c;
            prevSlash  = false;
        }
    }
    return result;
}

[[nodiscard]] inline constexpr std::string collapseBackslashes(std::string_view path) {
    std::string result;
    result.reserve(path.size());
    bool prevBackslash = false;
    for (char c : path) {
        if (c == '\\') {
            if (!prevBackslash) {
                result += c;
            }
            prevBackslash = true;
        } else {
            result        += c;
            prevBackslash  = false;
        }
    }
    return result;
}

[[nodiscard]] inline constexpr std::string collapseMixedSlashes(std::string_view path) {
    std::string result;
    result.reserve(path.size());
    size_t i = 0;
    while (i < path.size()) {
        if (path[i] == '/' || path[i] == '\\') {
            size_t start = i;
            while (i < path.size() && (path[i] == '/' || path[i] == '\\')) {
                i++;
            }
            size_t count = i - start;
            if (count >= 2) {
                result += '\\';
            } else {
                result += path[start];
            }
        } else {
            result += path[i];
            i++;
        }
    }
    return result;
}

[[nodiscard]] inline constexpr std::string toStandardPath(std::string_view path) {
    std::string result = collapseSlashes(path);
    result             = collapseBackslashes(result);
    result             = collapseMixedSlashes(result);
    return result;
}

[[nodiscard]] inline constexpr std::string toWindowsStandardPath(std::string_view path) {
    std::string result;
    result.reserve(path.size());
    bool prevSlash = false;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!prevSlash) {
                result += '\\';
            }
            prevSlash = true;
        } else {
            result    += c;
            prevSlash  = false;
        }
    }
    return result;
}

[[nodiscard]] inline constexpr std::string toUnixStandardPath(std::string_view path) {
    std::string result;
    result.reserve(path.size());
    bool prevSlash = false;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!prevSlash) {
                result += '/';
            }
            prevSlash = true;
        } else {
            result    += c;
            prevSlash  = false;
        }
    }
    return result;
}

[[nodiscard]] inline constexpr std::string toUnixStandardDirPath(std::string_view path) {
    if (path.empty()) {
        return std::string{path};
    }
    std::string normalized = toUnixStandardPath(path);
    if (normalized.back() == '/') {
        return normalized;
    }
    return normalized + "/";
}

[[nodiscard]] inline constexpr std::string toCurrentSystemStandardPath(std::string_view path) {
#if XX_IS_WIN_D
    return toWindowsStandardPath(path);
#else
    if (path.size() >= 2 && isCode_AZaz(path[0]) && path[1] == ':') {
        std::string result
            = fmt::format("/mnt/{}/{}", static_cast<char>(toCode_az(path[0])), path.substr(2));
        return toUnixStandardPath(result);
    }
    return toUnixStandardPath(path);
#endif
}

/// 判断路径是否为绝对路径。
/// 直接使用标准库 std::filesystem::path::is_absolute():
///  - Unix: 以 `/` 开头
///  - Windows: 盘符 + 分隔符 (如 `C:\`) 或 UNC (如 `\\server\share`);
///    `C:foo` (盘符相对) 返回 false, 与手写判断语义一致
///  - 空路径返回 false
/// 注意: Windows 上 `\foo` (根相对) 无盘符, 按标准库语义返回 false
[[nodiscard]] inline bool isAbsolutePath(std::string_view path) {
    return std::filesystem::path{path}.is_absolute();
}

/// 展开路径开头的 `~` 为用户主目录 (Unix: $HOME, Windows: %USERPROFILE%)。
/// 仅处理 `~` 或 `~/xxx` 形式; `~user` 等其它形式保持原样。
[[nodiscard]] inline std::string expandUserHomePath(std::string_view path) {
    if (path.empty() || path.front() != '~') {
        return std::string{path};
    }
    // `~` 之后必须是路径分隔符或结束, 否则视为普通名称 (如 `~user`) 不展开
    if (path.size() > 1 && path[1] != '/' && path[1] != '\\') {
        return std::string{path};
    }
#if XX_IS_WIN_D
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (!home || !*home) {
        return std::string{path};
    }
    auto homePath = agentxx::util::toCurrentSystemStandardPath(home);
    if (path.size() == 1) {
        return homePath;
    }
    // path[1] 为分隔符, 拼接剩余部分
    auto rest = path.substr(2);
    if (homePath.empty()) {
        return std::string{rest};
    }
    if (homePath.back() == '/' || homePath.back() == '\\') {
        return homePath + std::string{rest};
    }
    return homePath + "/" + std::string{rest};
}

/// 将用户提供的路径统一转换为当前系统的绝对路径 (相对路径基于指定基准目录):
/// 1. 展开开头的 `~` 为用户主目录
/// 2. 规范化分隔符/盘符 (toCurrentSystemStandardPath, 含 Windows 盘符 -> /mnt/ 转换)
/// 3. 相对路径基于 baseDir 拼接为绝对路径; baseDir 为空时基于进程当前工作目录
/// 4. 词法规范化 (去除 `./`、多余的 `../` 等, 不访问文件系统, 对不存在的路径安全)
/// - 供 filesystem 系列工具与 permission 权限判断使用, 使相对路径
///   (如 `src/main.cpp`、`./a.txt`) 也能与绝对路径规则稳定匹配
/// - baseDir 用于会话工作目录与进程 cwd 解耦 (AgentConfig::workDir,
///   嵌入场景单进程多 agent 实例各自独立项目目录); 调用方应传入绝对路径
[[nodiscard]] inline std::string
    toCurrentSystemAbsolutePath(std::string_view path, std::string_view baseDir) {
    if (path.empty()) {
        return std::string{path};
    }
    auto expanded   = expandUserHomePath(path);
    auto normalized = toCurrentSystemStandardPath(expanded);
    if (isAbsolutePath(normalized)) {
        return std::filesystem::path{normalized}.lexically_normal().generic_string();
    }
    if (!baseDir.empty()) {
        // 相对路径: 基于指定基准目录拼接 (词法规范化, 不访问文件系统)
        return (std::filesystem::path{baseDir} / std::filesystem::path{normalized})
            .lexically_normal()
            .generic_string();
    }
    // 无基准目录: 基于进程当前工作目录转换为绝对路径 (旧行为)
    std::error_code ec;
    auto            abs = std::filesystem::absolute(normalized, ec);
    if (ec) {
        // absolute 仅在获取 cwd 时可能失败, 回退为原规范化路径
        return std::filesystem::path{normalized}.lexically_normal().generic_string();
    }
    return abs.lexically_normal().generic_string();
}

/// 将用户提供的路径统一转换为当前系统的绝对路径 (相对路径基于进程当前工作目录):
/// - 兼容旧接口, 等价于 baseDir 为空的两参版本
[[nodiscard]] inline std::string toCurrentSystemAbsolutePath(std::string_view path) {
    return toCurrentSystemAbsolutePath(path, std::string_view{});
}

[[nodiscard]] inline constexpr std::string_view
    getFileName(std::string_view in_path, bool removeEXT = false, bool useRigthDot = true) {
    if (in_path.empty()) {
        return "";
    }

    int  i             = static_cast<int>(in_path.size());
    bool isContinueDot = false;

    while (i-- > 0) {
        if (in_path[i] != '/' && in_path[i] != '\\') {
            break;
        }
        removeEXT = false;
    }

    int nameEndIndex = i + 1;

    int leftDotIndex     = -1;
    int leftTempDotIndex = -1;
    int rightDotIndex    = -1;

    if (nameEndIndex <= 0) {
        return in_path;
    }

    for (; i-- > 0;) {
        if (in_path[i] == '/' || in_path[i] == '\\') {
            if (i == static_cast<int>(in_path.size())) {
                return "";
            } else {
                break;
            }
        } else if ('.' == in_path[i]) {
            if (removeEXT) {
                if (rightDotIndex == -1) {
                    rightDotIndex = i;
                }
                leftTempDotIndex = leftDotIndex;
                leftDotIndex     = i;
                isContinueDot    = (leftDotIndex != rightDotIndex);
            }
        } else {
            isContinueDot = false;
        }
    }

    if (rightDotIndex == -1) {
        rightDotIndex = nameEndIndex;
    }

    int start = i + 1;
    int end   = -1;

    if (useRigthDot) {
        end = rightDotIndex;
    } else {
        end = leftDotIndex;
    }

    if (start < 0) {
        start = 0;
    }
    if (end < 0) {
        end = 0;
    }

    if (leftDotIndex == start) {
        if (isContinueDot || false == removeEXT || leftDotIndex == rightDotIndex) {
            end = nameEndIndex;
        } else {
            if (useRigthDot) {
                end = rightDotIndex;
            } else {
                end = leftTempDotIndex;
            }
        }
    }

    return in_path.substr(start, end - start);
}

[[nodiscard]] inline constexpr std::optional<std::string_view>
    getFileNameEXT(std::string_view in_path) {
    if (in_path.empty() || in_path.back() == '.' || in_path.back() == '/'
        || in_path.back() == '\\') {
        return std::nullopt;
    }

    for (int i = static_cast<int>(in_path.size()) - 1; i-- > 1;) {
        if (in_path[i] == '.') {
            return in_path.substr(i + 1);
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::string replaceOrAppendExt(std::string_view inpath, std::string_view newExt);

[[nodiscard]] inline constexpr std::optional<std::string_view>
    getParentDirPath(std::string_view in_path) {
    if (in_path.empty()) {
        return std::nullopt;
    }

    int i = static_cast<int>(in_path.size());

    while (i-- > 0) {
        if (in_path[i] != '/' && in_path[i] != '\\') {
            break;
        }
    }

    if (i < 0 && !in_path.empty()) {
        return "/";
    }

    for (; i-- > 0;) {
        if (in_path[i] == '/' || in_path[i] == '\\') {
            if (i == static_cast<int>(in_path.size())) {
                return "";
            } else {
                return in_path.substr(0, i + 1);
            }
        }
    }

    return std::nullopt;
}

[[nodiscard]] inline constexpr std::string
    formatSize(size_t bytes, double base = 1024, bool showFloat = true) {
    if (bytes == 0) {
        return "0";
    }

    // 定义单位列表（顺序：B, K, M, G, T, P...）
    const std::vector<std::string> units_decimal = {"", "K", "M", "G", "T", "P"};

    const auto& units = units_decimal;

    // 计算应该使用的单位索引
    size_t index = 0;
    double size  = static_cast<double>(bytes);
    while (size >= base && index < units.size() - 1) {
        size /= base;
        ++index;
    }

    // 格式化输出: size 为整数时无小数 (避免 "1" 显示为 "1.0"),
    // 否则保留一位小数 (如 1.5K); 原逻辑仅按 >=100 去掉小数,
    // 导致 formatSize(1) 返回 "1.0" 这类不合理显示
    if (false == showFloat || size == std::floor(size)) {
        return fmt::format("{}{}", static_cast<int64_t>(size), units[index]);
    } else {
        return fmt::format("{:.1f}{}", size, units[index]);
    }
}

[[nodiscard]] inline constexpr bool
    isIgnoreCaseEqual(std::string_view left, std::string_view right) {
    if (left.size() == right.size()) {
        return toLower(left) == toLower(right);
    }
    return false;
}

[[nodiscard]] inline constexpr bool
    isIgnoreCaseContains(std::string_view longStr, std::string_view shortStr) {
    std::string lowerLong  = toLower(longStr);
    std::string lowerShort = toLower(shortStr);
    return lowerLong.find(lowerShort) != std::string::npos;
}

[[nodiscard]] inline constexpr bool
    isIgnoreCaseContainsAny(std::string_view str1, std::string_view str2) {
    return (str1.size() >= str2.size()) ? isIgnoreCaseContains(str1, str2)
                                        : isIgnoreCaseContains(str2, str1);
}

[[nodiscard]] inline constexpr bool
    isNotEmptyAndIgnoreCaseContains(std::string_view str1, std::string_view str2) {
    if (str1.empty() || str2.empty()) {
        return false;
    }
    return isIgnoreCaseContains(str1, str2);
}

// ---------------------------------------------------------------------------
// 时长 / 时间戳格式化 (agent 端构造系统提示消息文本时使用, 如轮次统计)
// ---------------------------------------------------------------------------

/// 格式化运行时长: 50ms / 1.2s / 3m5s / 1h2m3s
[[nodiscard]] inline std::string formatDurationMilliseconds(int64_t milliseconds) {
    if (milliseconds < 0) {
        return "0ms";
    }
    const int64_t totalSec = milliseconds / 1000;
    const int64_t hours    = totalSec / 3600;
    const int64_t minutes  = (totalSec % 3600) / 60;
    const int64_t seconds  = totalSec % 60;
    if (hours > 0) {
        return fmt::format("{}h{}m{}s", hours, minutes, seconds);
    }
    if (minutes > 0) {
        if (seconds > 0) {
            return fmt::format("{}m{}s", minutes, seconds);
        }
        return fmt::format("{}m0s", minutes);
    }
    if (milliseconds < 100) {
        return fmt::format("{}ms", milliseconds);
    }
    const double sec = static_cast<double>(milliseconds) / 1000.0;
    return fmt::format("{:.1f}s", sec);
}

#if XX_IS_ANDROID_D
// Android NDK libc++ 未实现 chrono 时区数据库 (current_zone/zoned_time 不可
// 用, 链接期缺 tzdb), 回退为 POSIX localtime_r 格式化本地时间 (bionic 支持)
#include <cstdio>
#include <ctime>

/// 格式化时间戳: HH:MM:SS (本地时区)
[[nodiscard]] inline std::string formatTimestampMilliseconds(int64_t timestamp_ms) {
    if (timestamp_ms <= 0) {
        return "00:00:00";
    }
    std::time_t sec = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm     local{};
    localtime_r(&sec, &local);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
    return buf;
}

/// 格式化时间戳: YYYY-MM-DD HH:MM (会话列表展示用)
[[nodiscard]] inline std::string formatDateTimeMilliseconds(int64_t timestamp_ms) {
    if (timestamp_ms <= 0) {
        return "-";
    }
    std::time_t sec = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm     local{};
    localtime_r(&sec, &local);
    char buf[24];
    std::snprintf(
        buf,
        sizeof(buf),
        "%04d-%02d-%02d %02d:%02d",
        local.tm_year + 1900,
        local.tm_mon + 1,
        local.tm_mday,
        local.tm_hour,
        local.tm_min
    );
    return buf;
}
#else

/// 格式化时间戳: HH:MM:SS (本地时区)
[[nodiscard]] inline std::string formatTimestampMilliseconds(int64_t timestamp_ms) {
    if (timestamp_ms <= 0) {
        return "00:00:00";
    }
    std::chrono::zoned_time time{
        std::chrono::current_zone(),
        std::chrono::sys_time{std::chrono::seconds(timestamp_ms / 1000)}
    };
    return std::format("{:%H:%M:%S}", time);
}

/// 格式化时间戳: YYYY-MM-DD HH:MM (会话列表展示用)
[[nodiscard]] inline std::string formatDateTimeMilliseconds(int64_t timestamp_ms) {
    if (timestamp_ms <= 0) {
        return "-";
    }
    std::chrono::zoned_time time{
        std::chrono::current_zone(),
        std::chrono::sys_time{std::chrono::seconds(timestamp_ms / 1000)}
    };
    return std::format("{:%Y-%m-%d %H:%M}", time);
}
#endif

[[nodiscard]] inline constexpr bool
    isNotEmptyAndIgnoreCaseContainsAny(std::string_view str1, std::string_view str2) {
    if (str1.empty() || str2.empty()) {
        return false;
    }
    return isIgnoreCaseContainsAny(str1, str2);
}

[[nodiscard]] std::string toArgument(std::string_view str, char mark = '"');

}; // namespace util
}; // namespace agentxx