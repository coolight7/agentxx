#pragma once
#include <cstdint>
#include <string_view>

namespace agentxx::util::hash {

inline constexpr uint64_t kFnv1a64OffsetBasis = 14695981039346656037ULL;
inline constexpr uint64_t kFnv1a64Prime       = 1099511628211ULL;

/// 64 位 FNV-1a 哈希算法
/// - 纯 constexpr 实现, 支持编译期求值与运行期快速计算
/// - `s`: 待哈希的输入字符串/字节切片
/// - `seed`: 初始哈希种子, 默认取标准 FNV 偏移基准
[[nodiscard]] inline constexpr uint64_t fnv1a64(
    std::string_view s,
    uint64_t seed = kFnv1a64OffsetBasis
) noexcept {
    uint64_t hash = seed;
    for (unsigned char c : s) {
        hash ^= c;
        hash *= kFnv1a64Prime;
    }
    return hash;
}

} // namespace agentxx::util::hash
