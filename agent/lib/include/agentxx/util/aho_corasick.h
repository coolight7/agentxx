#pragma once

#include "agentxx/util/log.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace agentxx {
namespace util {

template <typename CharType = char> class AhoCorasick {
protected:
  using StringType = std::basic_string<CharType>;
  using StringViewType = std::basic_string_view<CharType>;

  inline static const auto caseShiftSheet =
      std::unordered_map<CharType, CharType>{
          {65, 97},  {66, 98},  {67, 99},  {68, 100}, {69, 101}, {70, 102},
          {71, 103}, {72, 104}, {73, 105}, {74, 106}, {75, 107}, {76, 108},
          {77, 109}, {78, 110}, {79, 111}, {80, 112}, {81, 113}, {82, 114},
          {83, 115}, {84, 116}, {85, 117}, {86, 118}, {87, 119}, {88, 120},
          {89, 121}, {90, 122},
      };

  struct Node {
    std::unordered_map<CharType, size_t> children{}; // ASCII字符集
    size_t fail = 0;                                 // 失败指针
    std::vector<size_t> outputs;                     // 输出模式索引

    Node() {}
  };

  bool caseInsensitive;               // 忽略大小写
  std::vector<Node> nodes{};          // Trie节点存储
  std::vector<StringType> patterns{}; // 所有模式串

public:
  struct AhoCorasickMatchInfo {
    size_t start;
    size_t end;
    size_t patternId;

    AhoCorasickMatchInfo(size_t s, size_t e, size_t p)
        : start(s), end(e), patternId(p) {}

    bool operator<(const AhoCorasickMatchInfo &other) const {
      // 按起始位置升序，起始位置相同则按结束位置降序（长优先）
      if (start != other.start) {
        return start < other.start;
      }
      return end > other.end;
    }
  };

  AhoCorasick(const std::vector<StringViewType> &patterns,
              bool in_caseInsensitive = true)
      : caseInsensitive(in_caseInsensitive) {
    nodes.emplace_back(); // 初始化根节点
    for (const auto &item : patterns) {
      addPattern(item);
    }
    build();
  }

  AhoCorasick(const std::vector<StringType> &patterns,
              bool in_caseInsensitive = true)
      : caseInsensitive(in_caseInsensitive) {
    nodes.emplace_back(); // 初始化根节点
    for (const auto &item : patterns) {
      addPattern(item);
    }
    build();
  }

  CharType onCharCode(CharType ch) const {
    if (caseInsensitive) {
      // 转换为小写字母
      if (ch >= 65 && ch <= 90) {
        return ch + 32;
      }
    }
    return ch;
  }

  // 添加模式串
  void addPattern(const StringViewType pattern) {
    patterns.emplace_back(pattern);
    size_t current = 0;
    for (CharType ch : pattern) {
      auto useChar = onCharCode(ch);
      if (false == nodes[current].children.contains(useChar)) {
        nodes[current].children[useChar] = nodes.size();
        nodes.emplace_back();
      }
      current = nodes[current].children[useChar];
    }
    nodes[current].outputs.push_back(patterns.size() - 1);
  }

  const StringType &getPattern(size_t id) const { return patterns[id]; }

  // 构建失败指针和合并输出
  void build() {
    std::queue<size_t> q;
    nodes[0].fail = 0;

    // 初始化根节点的子节点
    for (const auto [c, child] : nodes[0].children) {
      nodes[child].fail = 0;
      q.push(child);
    }

    // BFS构建失败指针
    while (!q.empty()) {
      auto curr = q.front();
      q.pop();

      for (const auto [c, child] : nodes[curr].children) {
        auto fail = nodes[curr].fail;

        // 查找失败指针
        while (fail != 0 && false == nodes[fail].children.contains(c)) {
          fail = nodes[fail].fail;
        }

        nodes[child].fail =
            nodes[fail].children.contains(c) ? nodes[fail].children[c] : 0;

        // 合并输出
        auto &outputs = nodes[child].outputs;
        auto &fail_outputs = nodes[nodes[child].fail].outputs;
        outputs.insert(outputs.end(), fail_outputs.begin(), fail_outputs.end());

        q.push(child);
      }
    }
  }

  // 搜索文本中的模式串
  std::vector<AhoCorasick::AhoCorasickMatchInfo>
  search(const StringViewType text, bool onlyContains = false) const {
    std::vector<AhoCorasick::AhoCorasickMatchInfo> matches;
    size_t current = 0;

    for (size_t i = 0; i < text.length(); ++i) {
      auto useChar = onCharCode(text[i]);

      // 跳转失败指针直到找到匹配或回到根节点
      while (current > 0 &&
             false == nodes[current].children.contains(useChar)) {
        current = nodes[current].fail;
      }

      const auto it = nodes[current].children.find(useChar);
      current = (it != nodes[current].children.end()) ? it->second : 0;

      // 收集所有匹配的模式
      for (size_t patternId : nodes[current].outputs) {
        if (i + 1 >= patterns[patternId].length()) {
          auto start = i - patterns[patternId].length() + 1;
          matches.emplace_back(start, i + 1, patternId);
          if (onlyContains) {
            return matches;
          }
        }
      }
    }
    if (matches.empty()) {
      return matches;
    }

    std::sort(matches.begin(), matches.end());
    std::vector<AhoCorasick::AhoCorasickMatchInfo> result;
    size_t lastEnd = 0;
    for (const auto &match : matches) {
      if (match.start >= lastEnd) {
        result.emplace_back(match);
        lastEnd = match.end;
      }
      // 如果match.start < lastEnd，说明这个匹配与之前的重叠
      // 由于我们已按结束位置降序排序，之前保留的是更长的匹配
      // 所以跳过这个较短的匹配
    }

    return result;
  }

  StringType
  removeAll(const StringViewType text,
            std::function<
                void(const std::vector<AhoCorasick::AhoCorasickMatchInfo> &)>
                onResults = nullptr) const {
    auto matches = search(text, false);

    if (nullptr != onResults) {
      onResults(matches);
    }

    if (matches.empty()) {
      return StringType{text};
    }

    size_t index = 0;
    auto result = StringType{};
    for (const auto &match : matches) {
      if (match.start > index) {
        result += text.substr(index, match.start - index);
      }
      size_t end = match.end;
      if (end > index) {
        index = end;
      }
    }
    if (index < text.length()) {
      result += text.substr(index);
    }

    return result;
  }
};

} // namespace util
} // namespace agentxx