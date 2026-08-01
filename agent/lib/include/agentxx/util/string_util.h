#pragma once

#include "agentxx/util/log.h"
#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstring>
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
            if (ch == 0xEF && i + 2 < strLen) {
                unsigned char ch1 = static_cast<unsigned char>(str[i + 1]);
                unsigned char ch2 = static_cast<unsigned char>(str[i + 2]);
                if (ch1 == 0xBF && ch2 == 0xBD) {
                    return 0; // 匹配�，判定为无效UTF-8
                }
            }
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

[[nodiscard]] inline constexpr std::vector<std::string_view>
    strSplit(std::string_view in_str, char delim) {
    auto                          split = in_str | std::views::split(delim);
    std::vector<std::string_view> result;
    result.reserve(std::ranges::distance(split));
    for (auto&& sub : split) {
        result.emplace_back(&*sub.begin(), std::ranges::distance(sub));
    }
    return result;
}

[[nodiscard]] inline constexpr std::vector<std::string>
    strSplitCopid(std::string_view in_str, char delim) {
    auto                     split_view = in_str | std::views::split(delim);
    std::vector<std::string> result;
    result.reserve(std::ranges::distance(split_view));
    for (auto sub : split_view) {
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

std::string autoTryConvertToUtf8(std::string_view str);

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

[[nodiscard]] inline constexpr std::string formatSize(size_t bytes, double base = 1024) {
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
    if (size == std::floor(size)) {
        return fmt::format("{}{}", int64_t(size), units[index]);
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