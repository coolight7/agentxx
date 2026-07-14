#pragma once

#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"

namespace agentxx {
namespace util {

class HeaderMap {
public:
  using _BaseMap = agentxx::util::IgnoreCaseMap<std::vector<std::string>>;
  _BaseMap data;

  HeaderMap() = default;
  HeaderMap(_BaseMap in_data) : data(std::move(in_data)) {}

  bool empty() const { return data.empty(); }

  bool contains(std::string_view name) const noexcept {
    return data.find(name) != data.end();
  }

  _BaseMap::Iterator get(std::string_view name) {
    auto it = data.find(name);
    if (it == data.end()) {
      // 插入新条目，key 由 string_view 构造为 string
      auto [new_it, inserted] =
          data.emplace(std::string(name), std::vector<std::string>{});
      return new_it;
    }
    return it;
  }

  std::string_view getSingle(std::string_view name) const noexcept {
    auto it = data.find(name);
    if (it == data.end() || it->second.empty()) {
      return "";
    }
    return it->second.front();
  }

  void set(std::string_view name, const std::vector<std::string> &value) {
    auto it = data.find(name);
    if (it == data.end()) {
      data.emplace(std::string(name), value);
    } else {
      it->second = value;
    }
  }

  void set(std::string_view name, const std::string &value) {
    set(name, std::vector<std::string>{value});
  }
};

} // namespace util
} // namespace agentxx