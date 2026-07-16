#pragma once
#include "agentxx/util/log.h"
#include <memory>
#include <string>
#include <vector>

namespace agentxx {
namespace util {

// 存储匹配结果的结构体
struct XXRegexMatchResult {
  size_t start;
  size_t end;
};

class XXRegex {
public:
  static const unsigned int defHSFlags_normal;
  // 仅用于判断是否存在待查找值，但不能返回匹配结果和 replace、remove
  static const unsigned int defHSFlags_onlyContains;

  XXRegex() = default;
  XXRegex(const XXRegex &) = delete;
  XXRegex &operator=(const XXRegex &) = delete;

  [[nodiscard]] static std::shared_ptr<agentxx::util::XXRegex>
  createRegex(const std::string &regstr,
              unsigned int flags = defHSFlags_normal);
  [[nodiscard]] static std::shared_ptr<agentxx::util::XXRegex>
  createRegex(const std::vector<std::string> &regstrs,
              unsigned int flags = defHSFlags_normal);

  virtual ~XXRegex() {}

  // 匹配
  virtual bool match(std::string_view input,
                     std::vector<XXRegexMatchResult> &results) const = 0;

  // 移除匹配的子串
  [[nodiscard]] virtual std::string
  remove(std::string_view input,
         std::vector<XXRegexMatchResult> &results) const = 0;

  // 替换匹配的子串
  [[nodiscard]] virtual std::string
  replace(std::string_view input, std::string_view target,
          std::vector<XXRegexMatchResult> &results) const = 0;
  XXRegex &operator=(XXRegex &&other) = delete;
};

} // namespace util
} // namespace agentxx