#pragma once

#include <cstddef>

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
template <typename ContainerT, typename K>
[[nodiscard]] bool eraseHeterogeneous(ContainerT& container, const K& key) {
    auto it = container.find(key);
    if (it == container.end()) {
        return false;
    }
    container.erase(it);
    return true;
}

}; // namespace util
}; // namespace agentxx
