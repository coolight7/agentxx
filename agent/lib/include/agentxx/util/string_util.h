#pragma once

#include "agentxx/util/log.h"
#include "boost/beast/core/detail/base64.hpp"
#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
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
constexpr char charToLower(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

constexpr char charToUpper(char c) noexcept {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
}

constexpr bool charIsSpace(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
         c == '\f';
}

inline constexpr bool isCode_num(int code) {
  return (code >= CODE_0 && code <= CODE_9);
}

inline constexpr bool isCode_AZ(int code) {
  return (code >= CODE_A && code <= CODE_Z);
}

inline constexpr bool isCode_az(int code) {
  return (code >= CODE_a && code <= CODE_z);
}

inline constexpr bool isCode_AZaz(int code) {
  return (isCode_AZ(code) || isCode_az(code));
}

inline constexpr std::optional<int> toCode_tryAZ(int code) {
  if (isCode_az(code)) {
    return code - (CODE_a - CODE_A);
  } else if (isCode_AZ(code)) {
    return code;
  }
  return std::nullopt;
}

inline constexpr std::optional<int> toCode_tryaz(int code) {
  if (isCode_az(code)) {
    return code;
  } else if (isCode_AZ(code)) {
    return code + (CODE_a - CODE_A);
  }
  return std::nullopt;
}

inline constexpr int toCode_mayAZ(int code) {
  auto result = toCode_tryAZ(code);
  return result.has_value() ? result.value() : code;
}

inline constexpr int toCode_mayaz(int code) {
  auto result = toCode_tryaz(code);
  return result.has_value() ? result.value() : code;
}

inline constexpr int toCode_AZ(int code) {
  if (isCode_az(code)) {
    return code - (CODE_a - CODE_A);
  }
  if (!std::is_constant_evaluated()) {
    assert(isCode_AZ(code));
  }
  return code;
}

inline constexpr int toCode_az(int code) {
  if (isCode_AZ(code)) {
    return code + (CODE_a - CODE_A);
  }
  if (!std::is_constant_evaluated()) {
    assert(isCode_az(code));
  }
  return code;
}

inline constexpr void toUpperSelf(std::string &str) {
  std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
    return charToUpper(static_cast<char>(c));
  });
}

inline constexpr std::string toUpper(std::string_view str) {
  auto result = std::string{str};
  toUpperSelf(result);
  return result;
}

inline constexpr void toLowerSelf(std::string &str) {
  std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
    return charToLower(static_cast<char>(c));
  });
}

inline constexpr std::string toLower(std::string_view str) {
  auto result = std::string{str};
  toLowerSelf(result);
  return result;
}

// 不区分大小写哈希
struct IgnoreCaseHash {
  size_t operator()(const std::string &s) const {
    return std::hash<std::string>()(toLower(s));
  }

  size_t operator()(std::string_view s) const {
    return std::hash<std::string>()(toLower(s));
  }
};

// 不区分大小写相等判断（用于无序容器）
struct IgnoreCaseEqual {
  constexpr bool operator()(const std::string &a, const std::string &b) const {
    if (a.size() != b.size()) {
      return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
      if (charToLower(static_cast<char>(static_cast<unsigned char>(a[i]))) !=
          charToLower(static_cast<char>(static_cast<unsigned char>(b[i])))) {
        return false;
      }
    }
    return true;
  }

  constexpr bool operator()(std::string_view a, std::string_view b) const {
    if (a.size() != b.size()) {
      return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
      if (charToLower(static_cast<char>(static_cast<unsigned char>(a[i]))) !=
          charToLower(static_cast<char>(static_cast<unsigned char>(b[i])))) {
        return false;
      }
    }
    return true;
  }
};

template <typename V>
using IgnoreCaseMap =
    std::unordered_map<std::string, V, IgnoreCaseHash, IgnoreCaseEqual>;

using IgnoreCaseSet =
    std::unordered_set<std::string, IgnoreCaseHash, IgnoreCaseEqual>;

inline constexpr size_t utf8GetLength(std::string_view in_str) {
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

inline constexpr size_t findIndexByUtf8Length(std::string_view in_str,
                                              size_t targetLen,
                                              size_t start = 0) {
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
inline constexpr std::tuple<size_t, size_t, size_t>
findIndexAndLastLineIndexByUtf8Length(std::string_view in_str,
                                      size_t targetLen) {
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

inline constexpr bool utf8IsContinuationChar(unsigned char ch) {
  return (ch & 0xC0) == 0x80; // 10xxxxxx 的二进制特征：前两位是 10
}

inline constexpr size_t utf8GetLengthCheckAvail(std::string_view str) {
  size_t length = 0;
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
      if (ch == 0xF0 &&
          (static_cast<unsigned char>(str[i + 1]) & 0xF0) == 0x80) {
        // 检查非最短编码长度，对应 0~0xFFFF
        return 0;
      }
      step = 4;
    } else if (ch >= 0xE0) {
      if (ch == 0xE0 &&
          (static_cast<unsigned char>(str[i + 1]) & 0xE0) == 0x80) {
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
        if (i + j >= strLen || str[i + j] == '\0' ||
            false == utf8IsContinuationChar((unsigned char)str[i + j])) {
          // 不合规
          return 0;
        }
      }
    }
    length++;
  }
  return length;
}

inline constexpr bool utf8IsAvail(std::string_view str) {
  if (str.empty() || str[0] == '\0' || (utf8GetLengthCheckAvail(str) == 0)) {
    return false;
  }
  return true;
}

template <typename T>
inline std::string stringVectorJoin(const std::vector<T> &list,
                                    std::string_view sep = ", ") {
  std::ostringstream oss;
  auto len = list.size();
  for (size_t i = 0; i < len; ++i) {
    oss << list[i];
    if (i < len - 1) {
      oss << sep;
    }
  }
  return oss.str();
}

inline constexpr std::vector<std::string_view> strSplit(std::string_view in_str,
                                                        char delim) {
  auto split = in_str | std::views::split(delim);
  std::vector<std::string_view> result;
  result.reserve(std::ranges::distance(split));
  for (auto &&sub : split) {
    result.emplace_back(&*sub.begin(), std::ranges::distance(sub));
  }
  return result;
}

inline constexpr std::vector<std::string> strSplitCopid(std::string_view in_str,
                                                        char delim) {
  auto split_view = in_str | std::views::split(delim);
  std::vector<std::string> result;
  result.reserve(std::ranges::distance(split_view));
  for (auto sub : split_view) {
    result.emplace_back(sub.begin(), sub.end());
  }
  return result;
}

inline constexpr std::string_view toStringNotNull(const char *str) {
  if (nullptr == str) {
    return std::string_view{""};
  }
  return std::string_view{str};
}

inline std::string base64Encode(const void *data, size_t len) {
  // 预分配
  std::string result;
  result.resize(boost::beast::detail::base64::encoded_size(len));

  size_t bytes_written =
      boost::beast::detail::base64::encode(result.data(), data, len);

  // 调整大小为实际写入的字节数
  result.resize(bytes_written);
  return result;
}

inline std::string base64Decode(std::string_view str) {
  std::string result;
  result.resize(boost::beast::detail::base64::decoded_size(str.size()));

  auto [bytes_written, _] = boost::beast::detail::base64::decode(
      result.data(), str.data(), str.size());

  result.resize(bytes_written);
  return result;
}

std::tuple<bool, std::optional<std::string>>
convertToUtf8(std::string_view src, std::string_view srcEncoding);

std::tuple<bool, std::optional<std::string>>
autoConvertToUtf8(std::string_view str, std::string &encoding);

std::tuple<bool, std::optional<std::string>>
autoConvertToUtf8(std::string_view str, bool _);

bool autoConvertToUtf8(std::string &str);

inline PinyinCallback s_pinyinCallback = nullptr;

inline constexpr std::string removeAllSpace(std::string_view str) {
  std::string result;
  result.reserve(str.size());
  for (char c : str) {
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
      result += c;
    }
  }
  return result;
}

inline constexpr std::optional<std::string>
removeAllSpaceMayNull(std::string_view str) {
  if (str.empty()) {
    return std::nullopt;
  }
  std::string result = removeAllSpace(str);
  if (result.empty()) {
    return std::nullopt;
  }
  return result;
}

inline constexpr std::string removeBetweenSpace(std::string_view str,
                                                bool removeLine = true,
                                                bool subLeft = true,
                                                bool subRight = true) {

  if (!std::is_constant_evaluated()) {
    assert(subLeft || subRight);
  }

  if (str.empty()) {
    return std::string{str};
  }

  int left = 0;
  int right = static_cast<int>(str.size()) - 1;

  if (subRight) {
    for (; right >= left; --right) {
      if (str[right] != ' ' && str[right] != '\t' &&
          (!removeLine || (str[right] != '\r' && str[right] != '\n'))) {
        break;
      }
    }
  }

  if (subLeft) {
    for (; left <= right; ++left) {
      if (str[left] != ' ' && str[left] != '\t' &&
          (!removeLine || (str[left] != '\r' && str[left] != '\n'))) {
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

inline constexpr std::optional<std::string>
removeBetweenSpaceMayNull(std::string_view str, bool removeLine = true,
                          bool subLeft = true, bool subRight = true) {

  if (str.empty()) {
    return std::nullopt;
  }

  std::string result = removeBetweenSpace(str, removeLine, subLeft, subRight);
  if (result.empty()) {
    return std::nullopt;
  }
  return result;
}

inline std::string getFirstWordPinyin(std::string_view str) {
  if (s_pinyinCallback) {
    return s_pinyinCallback(str);
  }
  return "";
}

inline std::string getFirstCharPinyinFast(std::string_view str) {
  if (str.empty())
    return "";

  int code = static_cast<unsigned char>(str[0]);
  if (isCode_num(code))
    return std::string(1, str[0]);
  if (isCode_az(code))
    return std::string(1, str[0]);
  if (isCode_AZ(code))
    return std::string(1, static_cast<char>(code + (CODE_a - CODE_A)));

  std::string result = getFirstWordPinyin(str);
  if (!result.empty()) {
    int firstCode = static_cast<unsigned char>(result[0]);
    if (isCode_AZaz(firstCode)) {
      toLowerSelf(result);
      return result;
    }
  }
  return "";
}

inline std::optional<int> getComparableCode(std::string_view str,
                                            size_t index) {
  int code = static_cast<unsigned char>(str[index]);

  if (isCode_az(code))
    return code;
  if (isCode_AZ(code))
    return code + (CODE_a - CODE_A);

  if (code < 128)
    return std::nullopt;

  auto target = (index == 0) ? str : str.substr(index);
  std::string pinyin = getFirstCharPinyinFast(target);
  if (!pinyin.empty()) {
    int pinyinCode = static_cast<unsigned char>(pinyin[0]);
    if (isCode_AZ(pinyinCode))
      return pinyinCode + (CODE_a - CODE_A);
    if (isCode_az(pinyinCode))
      return pinyinCode;
  }
  return std::nullopt;
}

inline std::optional<std::string> getFirstCharPinyin(std::string_view str,
                                                     bool enableAZ = true,
                                                     bool enableNum = true) {
  if (str.empty()) {
    return std::nullopt;
  }

  std::string trimmed = removeBetweenSpace(str, true, true, false);
  if (trimmed.empty()) {
    return std::nullopt;
  }

  int code = static_cast<unsigned char>(trimmed[0]);

  if (isCode_num(code)) {
    if (enableNum) {
      return std::string(1, trimmed[0]);
    } else {
      return std::nullopt;
    }
  } else if (isCode_az(code)) {
    if (enableAZ) {
      return std::string(1, trimmed[0]);
    } else {
      return std::nullopt;
    }
  } else if (isCode_AZ(code)) {
    if (enableAZ) {
      return std::string(1, static_cast<char>(code + (CODE_a - CODE_A)));
    } else {
      return std::nullopt;
    }
  } else {
    std::string result = getFirstWordPinyin(trimmed);
    if (!result.empty()) {
      int firstCode = static_cast<unsigned char>(result[0]);
      if (isCode_AZaz(firstCode)) {
        toLowerSelf(result);
        return result;
      }
    }
    return std::nullopt;
  }
}

inline std::optional<std::string>
getFirstCharPinyinFirstChar(std::string_view str) {
  auto restr = getFirstCharPinyin(str);
  if (restr.has_value() && !restr->empty()) {
    return std::string(1, (*restr)[0]);
  }
  return std::nullopt;
}

inline int compareExtend(std::string_view left, std::string_view right) {
  if (left.empty()) {
    if (right.empty()) {
      return 0;
    }
    return -1;
  }
  if (right.empty()) {
    return 1;
  }

  size_t i = 0, j = 0;

  while (i < left.size() && j < right.size()) {
    int leftCode = static_cast<unsigned char>(left[i]);
    int rightCode = static_cast<unsigned char>(right[j]);
    bool leftIsNum = isCode_num(leftCode);
    bool rightIsNum = isCode_num(rightCode);

    if (leftIsNum != rightIsNum) {
      return leftIsNum ? -1 : 1;
    }

    if (leftIsNum) {
      int leftSum = 0;
      while (i < left.size() &&
             isCode_num(static_cast<unsigned char>(left[i]))) {
        leftSum = leftSum * 10 + (static_cast<unsigned char>(left[i]) - CODE_0);
        i++;
      }
      int rightSum = 0;
      while (j < right.size() &&
             isCode_num(static_cast<unsigned char>(right[j]))) {
        rightSum =
            rightSum * 10 + (static_cast<unsigned char>(right[j]) - CODE_0);
        j++;
      }
      if (leftSum != rightSum) {
        return leftSum - rightSum;
      }
      continue;
    }

    auto leftComp = getComparableCode(left, i);
    auto rightComp = getComparableCode(right, j);

    if (leftComp.has_value() && rightComp.has_value()) {
      int result = leftComp.value() - rightComp.value();
      if (result != 0)
        return result;
    } else if (leftComp.has_value()) {
      return 1;
    } else if (rightComp.has_value()) {
      return -1;
    } else {
      int leftLower = toCode_tryaz(leftCode).value_or(leftCode);
      int rightLower = toCode_tryaz(rightCode).value_or(rightCode);
      int result = leftLower - rightLower;
      if (result != 0)
        return result;
    }

    i++;
    j++;
  }

  if (i < left.size() || j < right.size()) {
    return static_cast<int>(left.size() - right.size());
  }
  return 0;
}

inline constexpr std::string collapseSlashes(std::string_view path) {
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
      result += c;
      prevSlash = false;
    }
  }
  return result;
}

inline constexpr std::string collapseBackslashes(std::string_view path) {
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
      result += c;
      prevBackslash = false;
    }
  }
  return result;
}

inline constexpr std::string collapseMixedSlashes(std::string_view path) {
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

inline constexpr std::string toStandardPath(std::string_view path) {
  std::string result = collapseSlashes(path);
  result = collapseBackslashes(result);
  result = collapseMixedSlashes(result);
  return result;
}

inline constexpr std::string toWindowsStandardPath(std::string_view path) {
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
      result += c;
      prevSlash = false;
    }
  }
  return result;
}

inline constexpr std::string toUnixStandardPath(std::string_view path) {
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
      result += c;
      prevSlash = false;
    }
  }
  return result;
}

inline constexpr std::string toUnixStandardDirPath(std::string_view path) {
  if (path.empty()) {
    return std::string{path};
  }
  std::string normalized = toUnixStandardPath(path);
  if (normalized.back() == '/') {
    return normalized;
  }
  return normalized + "/";
}

inline constexpr std::string_view getFileName(std::string_view in_path,
                                              bool removeEXT = false,
                                              bool useRigthDot = true) {

  if (in_path.empty()) {
    return "";
  }

  int i = static_cast<int>(in_path.size());
  bool isContinueDot = false;

  while (i-- > 0) {
    if (in_path[i] != '/' && in_path[i] != '\\') {
      break;
    }
    removeEXT = false;
  }

  int nameEndIndex = i + 1;

  int leftDotIndex = -1;
  int leftTempDotIndex = -1;
  int rightDotIndex = -1;

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
        leftDotIndex = i;
        isContinueDot = (leftDotIndex != rightDotIndex);
      }
    } else {
      isContinueDot = false;
    }
  }

  if (rightDotIndex == -1) {
    rightDotIndex = nameEndIndex;
  }

  int start = i + 1;
  int end = -1;

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

inline constexpr std::optional<std::string_view>
getFileNameEXT(std::string_view in_path) {
  if (in_path.empty() || in_path.back() == '.' || in_path.back() == '/' ||
      in_path.back() == '\\') {
    return std::nullopt;
  }

  for (int i = static_cast<int>(in_path.size()) - 1; i-- > 1;) {
    if (in_path[i] == '.') {
      return in_path.substr(i + 1);
    }
  }
  return std::nullopt;
}

inline std::string replaceOrAppendExt(std::string_view inpath,
                                      std::string_view newExt) {
  auto ext = getFileNameEXT(inpath);
  if (ext.has_value()) {
    auto result = std::string{inpath};
    result.replace(inpath.size() - ext->size(), ext->size(), newExt);
    return result;
  }
  if (!inpath.empty() && inpath.back() == '.') {
    return fmt::format("{}{}", inpath, newExt);
  }
  return fmt::format("{}.{}", inpath, newExt);
}

inline constexpr std::optional<std::string_view>
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

inline constexpr bool isIgnoreCaseEqual(std::string_view left,
                                        std::string_view right) {
  if (left.size() == right.size()) {
    return toLower(left) == toLower(right);
  }
  return false;
}

inline constexpr bool isIgnoreCaseContains(std::string_view longStr,
                                           std::string_view shortStr) {
  std::string lowerLong = toLower(longStr);
  std::string lowerShort = toLower(shortStr);
  return lowerLong.find(lowerShort) != std::string::npos;
}

inline constexpr bool isIgnoreCaseContainsAny(std::string_view str1,
                                              std::string_view str2) {
  return (str1.size() >= str2.size()) ? isIgnoreCaseContains(str1, str2)
                                      : isIgnoreCaseContains(str2, str1);
}

inline constexpr bool isNotEmptyAndIgnoreCaseContains(std::string_view str1,
                                                      std::string_view str2) {
  if (str1.empty() || str2.empty()) {
    return false;
  }
  return isIgnoreCaseContains(str1, str2);
}

inline constexpr bool
isNotEmptyAndIgnoreCaseContainsAny(std::string_view str1,
                                   std::string_view str2) {
  if (str1.empty() || str2.empty()) {
    return false;
  }
  return isIgnoreCaseContainsAny(str1, str2);
}

inline std::string toArgument(std::string_view str, char mark = '"') {
  std::string result;
  result.reserve(str.size() * 2);
  size_t len = str.size();

  for (size_t i = 0; i < len; ++i) {
    if (str[i] == mark) {
      bool shouldEscape = false;

      if (i == 0) {
        shouldEscape = true;
      } else {
        char prev = str[i - 1];
        if (prev != '\\') {
          shouldEscape = true;
        }
      }

      if (shouldEscape) {
        result += '\\';
      }
      result += mark;
    } else {
      result += str[i];
    }
  }

  return fmt::format("{}{}{}", mark, result, mark);
}

}; // namespace util
}; // namespace agentxx