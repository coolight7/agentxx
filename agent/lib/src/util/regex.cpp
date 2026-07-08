#include "agentxx/util/regex.h"
#include <algorithm>

#if defined(AGENTXX_ENABLE_VECTORSCAN) || defined(AGENTXX_ENABLE_HYPERSCAN)
#include <hs_compile.h>
#include <hs_runtime.h>

const unsigned int agentxx::util::XXRegex::defHSFlags_normal =
    HS_FLAG_UTF8 | HS_FLAG_UCP | HS_FLAG_SOM_LEFTMOST;
const unsigned int agentxx::util::XXRegex::defHSFlags_onlyContains =
    HS_FLAG_UTF8 | HS_FLAG_UCP;

class XXRegexHP : public agentxx::util::XXRegex {
public:
  static const unsigned int defHSFlags_normal;
  // 仅用于判断是否存在待查找值，但不能返回匹配结果和 replace、remove
  static const unsigned int defHSFlags_onlyContains;

  XXRegexHP(const XXRegexHP &) = delete;
  XXRegexHP &operator=(const XXRegexHP &) = delete;

  // 初始化Hyperscan数据库
  XXRegexHP(const std::string &regstr,
            unsigned int flags = agentxx::util::XXRegex::defHSFlags_normal)
      : hs_db(nullptr), hs_scratch(nullptr) {
    // 编译正则表达式到Hyperscan数据库
    hs_compile_error_t *compile_err = nullptr;
    hs_error_t err = hs_compile(regstr.c_str(), flags, HS_MODE_BLOCK, nullptr,
                                &hs_db, &compile_err);
    if (err != HS_SUCCESS) {
      XX_LOGE("Hyperscan编译正则失败: {} | {}", compile_err->message, regstr);
      hs_free_compile_error(compile_err);
      return;
    }

    // 创建扫描缓冲区
    err = hs_alloc_scratch(hs_db, &hs_scratch);
    if (err != HS_SUCCESS) {
      XX_LOGE("创建Hyperscan扫描缓冲区失败");
      hs_free_database(hs_db);
      hs_db = nullptr;
      return;
    }
  }

  XXRegexHP(const std::vector<std::string> &regstrs,
            unsigned int flags = agentxx::util::XXRegex::defHSFlags_normal)
      : hs_db(nullptr), hs_scratch(nullptr) {
    // 编译正则表达式到Hyperscan数据库
    hs_compile_error_t *compile_err = nullptr;
    std::vector<unsigned int> flagslist(regstrs.size(), flags);
    std::vector<const char *> reglist;
    reglist.reserve(regstrs.size());
    for (const auto &reg : regstrs) {
      reglist.push_back(reg.c_str());
    }

    hs_error_t err = hs_compile_multi(reglist.data(), flagslist.data(), nullptr,
                                      regstrs.size(), HS_MODE_BLOCK, nullptr,
                                      &hs_db, &compile_err);

    if (err != HS_SUCCESS) {
      XX_LOGE("Hyperscan编译正则失败: {} | {}", compile_err->message,
              regstrs.size());
      hs_free_compile_error(compile_err);
      return;
    }

    // 创建扫描缓冲区
    err = hs_alloc_scratch(hs_db, &hs_scratch);
    if (err != HS_SUCCESS) {
      XX_LOGE("创建Hyperscan扫描缓冲区失败");
      hs_free_database(hs_db);
      hs_db = nullptr;
      return;
    }
  }

  ~XXRegexHP() {
    if (hs_scratch != nullptr) {
      hs_free_scratch(hs_scratch);
    }
    if (hs_db != nullptr) {
      hs_free_database(hs_db);
    }
  }

  // Hyperscan匹配回调函数
  inline static int xxregexMatchCallback(unsigned int id,
                                         unsigned long long from,
                                         unsigned long long to,
                                         unsigned int flags, void *context) {
    std::vector<agentxx::util::XXRegexMatchResult> *results =
        static_cast<std::vector<agentxx::util::XXRegexMatchResult> *>(context);
    if (results == nullptr) {
      return 0;
    }

    // 提取并格式化当前匹配区间
    const size_t curr_start = from;
    const size_t curr_end = to;
    const size_t new_start = std::min(curr_start, curr_end);
    const size_t new_end = std::max(curr_start, curr_end);

    // 处理区间合并（仅合并重叠区间，不合并相邻区间）
    if (!results->empty()) {
      size_t merge_start = new_start;
      size_t merge_end = new_end;
      std::vector<
          typename std::vector<agentxx::util::XXRegexMatchResult>::iterator>
          to_erase;

      // 找出所有重叠的区间
      for (auto it = results->begin(); it != results->end(); ++it) {
        const auto &exist = *it;
        bool is_overlap =
            (exist.start < merge_end) && (merge_start < exist.end);

        if (is_overlap) {
          // 更新合并区间：取所有相关区间的最小start和最大end
          merge_start = std::min(merge_start, exist.start);
          merge_end = std::max(merge_end, exist.end);
          // 标记该旧区间为待删除
          to_erase.push_back(it);
        }
      }

      // 删除所有被合并的旧区间（逆序删除，避免迭代器失效）
      for (auto it = to_erase.rbegin(); it != to_erase.rend(); ++it) {
        results->erase(*it);
      }

      // 添加合并后的新区间
      results->push_back({merge_start, merge_end});
    } else {
      // 结果容器为空，直接添加当前匹配区间
      results->push_back({new_start, new_end});
    }

    // 返回0继续扫描，非0终止扫描
    return 0;
  }

  // 匹配
  bool match(
      std::string_view input,
      std::vector<agentxx::util::XXRegexMatchResult> &results) const override {
    results.clear();
    if (hs_db == nullptr || hs_scratch == nullptr) {
      return false;
    }

    // block模式、适合短文本
    hs_error_t err = hs_scan(hs_db, input.data(), input.length(), 0, hs_scratch,
                             xxregexMatchCallback, &results);
    if (err != HS_SUCCESS && err != HS_SCAN_TERMINATED) {
      XX_LOGE("Hyperscan扫描失败");
      return false;
    }

    return !results.empty();
  }

  // 移除匹配的子串
  std::string remove(
      std::string_view input,
      std::vector<agentxx::util::XXRegexMatchResult> &results) const override {
    std::string result;
    if (match(input, results)) {
      size_t index = 0;
      for (auto &match : results) {
        result += input.substr(index, match.start - index);
        index = match.end;
      }
      if (index < input.length()) {
        result += input.substr(index);
      }
    } else {
      result = input;
    }
    return result;
  }

  // 替换匹配的子串
  std::string replace(
      std::string_view input, std::string_view target,
      std::vector<agentxx::util::XXRegexMatchResult> &results) const override {
    std::string result;
    if (match(input, results)) {
      size_t index = 0;
      for (auto &match : results) {
        result += input.substr(index, match.start - index);
        result += target;
        index = match.end;
      }
      if (index < input.length()) {
        result += input.substr(index);
      }
    } else {
      result = input;
    }
    return result;
  }

private:
  hs_database_t *hs_db;     // Hyperscan编译后的正则数据库
  hs_scratch_t *hs_scratch; // Hyperscan扫描缓冲区
};

std::shared_ptr<agentxx::util::XXRegex>
agentxx::util::XXRegex::createRegex(const std::string &regstr,
                                    unsigned int flags) {
  return std::make_shared<XXRegexHP>(regstr, flags);
}

std::shared_ptr<agentxx::util::XXRegex>
agentxx::util::XXRegex::createRegex(const std::vector<std::string> &regstrs,
                                    unsigned int flags) {
  return std::make_shared<XXRegexHP>(regstrs, flags);
}

#else
#include <regex>

const unsigned int agentxx::util::XXRegex::defHSFlags_normal = 0;
const unsigned int agentxx::util::XXRegex::defHSFlags_onlyContains = 0;

class XXRegexStdRegex : public agentxx::util::XXRegex {
public:
  // 禁止拷贝
  XXRegexStdRegex(const XXRegexStdRegex &) = delete;
  XXRegexStdRegex &operator=(const XXRegexStdRegex &) = delete;

  // 单模式构造函数
  XXRegexStdRegex(
      const std::string &regstr,
      unsigned int flags = agentxx::util::XXRegex::defHSFlags_normal)
      : valid_(false), multi_mode_(false) {
    try {
      regex_ =
          std::regex(regstr, std::regex::ECMAScript | std::regex::optimize);
      valid_ = true;
    } catch (const std::regex_error &e) {
      XX_LOGE("Regex编译失败: {} | {}", e.what(), regstr);
    }
  }

  // 多模式构造函数
  XXRegexStdRegex(
      const std::vector<std::string> &regstrs,
      unsigned int flags = agentxx::util::XXRegex::defHSFlags_normal)
      : valid_(false), multi_mode_(true) {
    regexes_.reserve(regstrs.size());
    for (const auto &str : regstrs) {
      try {
        regexes_.emplace_back(str,
                              std::regex::ECMAScript | std::regex::optimize);
      } catch (const std::regex_error &e) {
        XX_LOGE("Regex编译失败: {} | {}", e.what(), str);
        regexes_.clear();
        return;
      }
    }
    valid_ = true;
  }

  // 移动构造
  XXRegexStdRegex(XXRegexStdRegex &&other) noexcept
      : regex_(std::move(other.regex_)), regexes_(std::move(other.regexes_)),
        valid_(other.valid_), multi_mode_(other.multi_mode_) {
    other.valid_ = false;
  }

  ~XXRegexStdRegex() = default;

  // 匹配：返回所有合并后的非重叠区间
  bool match(
      std::string_view input,
      std::vector<agentxx::util::XXRegexMatchResult> &results) const override {
    results.clear();
    if (!valid_)
      return false;

    std::vector<std::pair<size_t, size_t>> raw_matches;
    auto inputStr = std::string{input};

    // 收集所有原始匹配区间
    auto collect = [&](const std::regex &re) {
      std::sregex_iterator begin(inputStr.begin(), inputStr.end(), re);
      std::sregex_iterator end;
      for (auto it = begin; it != end; ++it) {
        const std::smatch &m = *it;
        raw_matches.emplace_back(m.position(), m.position() + m.length());
      }
    };

    if (multi_mode_) {
      for (const auto &re : regexes_) {
        collect(re);
      }
    } else {
      collect(regex_);
    }

    if (raw_matches.empty()) {
      return false;
    }

    // 按起始位置排序
    std::sort(raw_matches.begin(), raw_matches.end());

    // 合并重叠区间（不合并相邻区间）
    agentxx::util::XXRegexMatchResult current{raw_matches[0].first,
                                              raw_matches[0].second};
    results.push_back(current);
    for (size_t i = 1; i < raw_matches.size(); ++i) {
      auto &last = results.back();
      if (raw_matches[i].first < last.end) { // 仅重叠合并
        last.end = std::max(last.end, raw_matches[i].second);
      } else {
        results.push_back({raw_matches[i].first, raw_matches[i].second});
      }
    }

    return true;
  }

  // 移除匹配子串
  std::string remove(
      std::string_view input,
      std::vector<agentxx::util::XXRegexMatchResult> &results) const override {
    std::string result;
    if (match(input, results)) {
      size_t index = 0;
      for (const auto &m : results) {
        result += input.substr(index, m.start - index);
        index = m.end;
      }
      if (index < input.length())
        result += input.substr(index);
    } else {
      result = input;
    }
    return result;
  }

  // 替换匹配子串
  std::string replace(
      std::string_view input, std::string_view target,
      std::vector<agentxx::util::XXRegexMatchResult> &results) const override {
    std::string result;
    if (match(input, results)) {
      size_t index = 0;
      for (const auto &m : results) {
        result += input.substr(index, m.start - index);
        result += target;
        index = m.end;
      }
      if (index < input.length())
        result += input.substr(index);
    } else {
      result = input;
    }
    return result;
  }

private:
  std::regex regex_;                // 单模式
  std::vector<std::regex> regexes_; // 多模式
  bool valid_ = false;              // 编译是否成功
  bool multi_mode_ = false;         // 是否多模式
};

std::shared_ptr<agentxx::util::XXRegex>
agentxx::util::XXRegex::createRegex(const std::string &regstr,
                                    unsigned int flags) {
  return std::make_shared<XXRegexStdRegex>(regstr, flags);
}

std::shared_ptr<agentxx::util::XXRegex>
agentxx::util::XXRegex::createRegex(const std::vector<std::string> &regstrs,
                                    unsigned int flags) {
  return std::make_shared<XXRegexStdRegex>(regstrs, flags);
}

#endif