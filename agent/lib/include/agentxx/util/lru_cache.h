#pragma once

#include <cstddef>
#include <list>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace agentxx {
namespace util {

template <typename K, typename V> class LruCache {
public:
  explicit LruCache(size_t capacity)[[expects:capacity > 0]]
      : capacity_(capacity) {}

  std::optional<V> get(const K &key) {
    auto it = map_.find(key);
    if (it == map_.end()) {
      return std::nullopt;
    }
    items_.splice(items_.begin(), items_, it->second);
    return it->second->second;
  }

  void put(const K &key, V value) {
    auto it = map_.find(key);
    if (it != map_.end()) {
      it->second->second = std::move(value);
      items_.splice(items_.begin(), items_, it->second);
      return;
    }
    if (map_.size() >= capacity_) {
      evict_one();
    }
    items_.emplace_front(key, std::move(value));
    map_[key] = items_.begin();
  }

  bool exists(const K &key) const { return map_.find(key) != map_.end(); }

  bool erase(const K &key) {
    auto it = map_.find(key);
    if (it == map_.end()) {
      return false;
    }
    items_.erase(it->second);
    map_.erase(it);
    return true;
  }

  void clear() {
    items_.clear();
    map_.clear();
  }

  size_t size() const { return map_.size(); }

  size_t capacity() const { return capacity_; }

  bool empty() const { return map_.empty(); }

  bool evict() {
    if (items_.empty())
      return false;
    evict_one();
    return true;
  }

private:
  void evict_one() {
    auto last = --items_.end();
    map_.erase(last->first);
    items_.pop_back();
  }

  const size_t capacity_;
  std::list<std::pair<K, V>> items_;
  std::unordered_map<K, std::list<std::pair<K, V>>::iterator> map_;
};

} // namespace util
} // namespace agentxx
