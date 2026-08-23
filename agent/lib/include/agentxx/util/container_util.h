#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace agentxx {
namespace util {

/// 关联容器 (map/set) 异构查找删除, 免除 key 拷贝:
/// - C++23 异构 erase 重载 (P2077) 在 libc++ (Android NDK) 尚未实现, 以
///   string_view 调用 erase(key) 无法编译; 退路 erase(std::string(key)) 又会
///   引入一次多余的堆分配。本函数改用容器自身的透明比较器做异构 find,
///   命中后按迭代器删除, 全程零拷贝且各平台行为一致
/// - 要求 [container] 使用透明比较器 (如 std::less<>) 且支持与 K 比较
///   (如 std::map<std::string, V, std::less<>> 配 std::string_view)
/// - 返回是否删除了元素; multimap/multiset 仅删除首个匹配元素
template<typename ContainerT>
bool eraseHeterogeneous(ContainerT& container, const std::string_view key) {
    auto it = container.find(key);
    if (it == container.end()) {
        return false;
    }
    container.erase(it);
    return true;
}

/// 关联容器 (map/set) 插入实现:
/// - [key] 声明为 std::string 传值: 若传入右值 (std::string&& 将亡值) 可直接 move 复用,
/// 避免额外拷贝;
///   若传入 string_view / const char* 则在形参处构造一次 key
/// - 当 key 已存在时不会覆盖已有元素 (保持 insert/emplace 语义)
/// - 返回 std::pair<iterator, bool>
template<typename ContainerT, typename... Args>
auto insertHeterogeneous(ContainerT& container, std::string key, Args&&... args) {
    if constexpr (sizeof...(args) == 0) {
        return container.emplace(std::move(key));
    } else {
        return container.emplace(std::move(key), std::forward<Args>(args)...);
    }
}

/// 关联容器 (map) 覆盖实现 (若已存在则直接覆盖 value, 找不到值则插入):
/// - [key] 声明为通用引用模板类型 KeyT&&, 先利用透明比较器进行异构 find 查找:
///   - 若找到已有元素: 直接更新 it->second, 全程无需构造/拷贝 std::string, 零堆内存分配
///   - 若未找到: 仅在需要插入时根据 KeyT 类型构造或 move key 存入容器
/// - 返回 std::pair<iterator, bool>, bool 为 true 表示新插入, false 表示覆盖已有值
template<typename ContainerT, typename KeyT, typename ValueT>
auto insertOrAssignHeterogeneous(ContainerT& container, KeyT&& key, ValueT&& value) {
    auto it = container.find(key);
    if (it != container.end()) {
        it->second = std::forward<ValueT>(value);
        return std::make_pair(it, false);
    }
    using KeyType = typename ContainerT::key_type;
    if constexpr (std::is_same_v<std::remove_cvref_t<KeyT>, KeyType>) {
        auto [newIt, inserted]
            = container.emplace(std::forward<KeyT>(key), std::forward<ValueT>(value));
        return std::make_pair(newIt, inserted);
    } else {
        auto [newIt, inserted]
            = container.emplace(KeyType(std::forward<KeyT>(key)), std::forward<ValueT>(value));
        return std::make_pair(newIt, inserted);
    }
}

/// 覆盖操作别名 (语义同 insertOrAssignHeterogeneous)
template<typename ContainerT, typename KeyT, typename ValueT>
auto overwriteHeterogeneous(ContainerT& container, KeyT&& key, ValueT&& value) {
    return insertOrAssignHeterogeneous(
        container,
        std::forward<KeyT>(key),
        std::forward<ValueT>(value)
    );
}

/// 关联容器 (map) 下标访问/获取或默认创建实现 (类似 operator[] 的异构版本):
/// - 若 key 已存在则直接返回对应 mapped_type 引用 (零 key 拷贝)
/// - 若 key 不存在则构造默认值插入并返回引用
template<typename ContainerT, typename KeyT>
typename ContainerT::mapped_type& getOrCreateHeterogeneous(ContainerT& container, KeyT&& key) {
    auto it = container.find(key);
    if (it != container.end()) {
        return it->second;
    }
    using KeyType   = typename ContainerT::key_type;
    using ValueType = typename ContainerT::mapped_type;
    if constexpr (std::is_same_v<std::remove_cvref_t<KeyT>, KeyType>) {
        auto [newIt, _] = container.emplace(std::forward<KeyT>(key), ValueType{});
        return newIt->second;
    } else {
        auto [newIt, _] = container.emplace(KeyType(std::forward<KeyT>(key)), ValueType{});
        return newIt->second;
    }
}

}; // namespace util
}; // namespace agentxx
