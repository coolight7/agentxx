/// agentxx 功能点值缓存
///
/// 每个功能点可以选择三种缓存策略:
/// - `None`:       不缓存 (默认; 结果要写回会话 / 结果很大 / 结果天生每次不同);
/// - `Latest`:     只留"最近一次算出来的值" (单槽, 与身份无关);
/// - `ByIdentity`: 按身份存若干条 (条数 + 字节双上限, 达到上限按最近最少使用淘汰)。
///
/// 约束 (与功能点子系统其余部分一致):
/// - 只在内存、按 agent 实例一份、不落盘, 进程重启即空;
/// - `max_bytes` 是硬上限: 单条超过上限的值直接不入缓存 (不截断、不报错);
/// - 每条记录产出方 `by` (如 `plugin:<插件名>` / `core:<域>:<实现>`),
///   实现被摘除或插件被禁用/卸载时按来源失效 (见 [invalidateBy])。
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace agentxx {
namespace feature {

/// 值缓存策略
enum class CacheMode : int32_t {
    None       = 0, ///< 不缓存
    Latest     = 1, ///< 只留最新一条
    ByIdentity = 2, ///< 按身份存若干条
};

const char* cacheModeKey(CacheMode mode) noexcept;

/// 值缓存默认上限 (点声明未给出时使用)
inline constexpr size_t kDefaultCacheMaxItems = 64;
inline constexpr size_t kDefaultCacheMaxBytes = 256u * 1024u;

/// 一份缓存条目
struct ValueCacheEntry {
    std::string valueJson; ///< 值 (JSON 文本)
    std::string by;        ///< 产出方 (见文件头说明)
    uint64_t    seq = 0;   ///< 最近使用序号 (淘汰时取最小者)
    size_t      bytes = 0; ///< valueJson 字节数
};

/// 值缓存 (按点一份; 不做跨线程同步 —— 功能点只在宿主 io 线程上访问)
class ValueCache {
public:

    /// 配置策略与上限
    /// - `mode` 为 `None` 时清空已有条目
    /// - `maxItems` / `maxBytes` 传 0 表示用内置默认值 (仅 `ByIdentity` 生效)
    void configure(CacheMode mode, size_t maxItems = 0, size_t maxBytes = 0) {
        mode_ = mode;
        if (mode_ == CacheMode::None) {
            entries_.clear();
            bytes_ = 0;
            return;
        }
        maxItems_ = (maxItems > 0) ? maxItems : kDefaultCacheMaxItems;
        maxBytes_ = (maxBytes > 0) ? maxBytes : kDefaultCacheMaxBytes;
        trim();
    }

    CacheMode mode() const noexcept {
        return mode_;
    }

    size_t maxItems() const noexcept {
        return maxItems_;
    }

    size_t maxBytes() const noexcept {
        return maxBytes_;
    }

    size_t size() const noexcept {
        return entries_.size();
    }

    size_t bytes() const noexcept {
        return bytes_;
    }

    uint64_t hits() const noexcept {
        return hits_;
    }

    uint64_t misses() const noexcept {
        return misses_;
    }

    /// 查缓存 (命中时刷新最近使用序号)
    /// - `None` 策略恒返回 nullptr
    /// - `Latest` 策略忽略身份, 只看"有没有值"
    ///
    /// `return`: 命中返回条目指针 (指向内部存储, 下一次写入即可能失效); 未命中返回 nullptr
    const ValueCacheEntry* find(std::string_view identity) {
        if (mode_ == CacheMode::None) {
            return nullptr;
        }
        auto it = entries_.find(keyOf(identity));
        if (it == entries_.end()) {
            ++misses_;
            return nullptr;
        }
        ++hits_;
        it->second.seq = ++useCounter_;
        return &it->second;
    }

    /// 写缓存
    /// - `None` 策略直接忽略
    /// - 单条超过 `maxBytes` 时不入缓存
    ///
    /// `return`: true = 已写入; false = 策略不缓存或单条超上限
    bool store(std::string_view identity, std::string valueJson, std::string by) {
        if (mode_ == CacheMode::None) {
            return false;
        }
        const size_t bytes = valueJson.size();
        if (bytes > maxBytes_) {
            return false;
        }
        auto     key = keyOf(identity);
        auto     it  = entries_.find(key);
        uint64_t seq = ++useCounter_;
        if (it != entries_.end()) {
            bytes_ -= it->second.bytes;
            bytes_ += bytes;
        } else {
            bytes_ += bytes;
        }
        entries_[std::move(key)] = ValueCacheEntry{
            std::move(valueJson),
            std::move(by),
            seq,
            bytes,
        };
        trim();
        return true;
    }

    /// 按产出方失效 (实现被摘除 / 插件禁用卸载时调用; `by` 精确匹配)
    ///
    /// `return`: 清除的条目数
    size_t invalidateBy(std::string_view owner) {
        if (entries_.empty() || owner.empty()) {
            return 0;
        }
        size_t removed = 0;
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.by == owner) {
                bytes_ -= it->second.bytes;
                it = entries_.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }

    /// 清空全部条目 (计数保留)
    void clear() {
        entries_.clear();
        bytes_ = 0;
    }

private:

    /// 缓存键: `Latest` 策略共用同一个键 (单槽语义)
    std::string keyOf(std::string_view identity) const {
        if (mode_ == CacheMode::Latest) {
            return std::string{"\x01latest"};
        }
        return std::string{identity};
    }

    /// 淘汰到上限以内 (条数 + 字节双上限)
    /// - 先按条数淘汰; 再按字节淘汰 (只剩 1 条时不再淘汰, 避免刚写入就被清掉)
    void trim() {
        while (entries_.size() > maxItems_ && entries_.size() > 1) {
            eraseLru();
        }
        while (bytes_ > maxBytes_ && entries_.size() > 1) {
            eraseLru();
        }
    }

    void eraseLru() {
        auto lru = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.seq < lru->second.seq) {
                lru = it;
            }
        }
        bytes_ -= lru->second.bytes;
        entries_.erase(lru);
    }

    CacheMode                                   mode_      = CacheMode::None;
    size_t                                      maxItems_  = kDefaultCacheMaxItems;
    size_t                                      maxBytes_  = kDefaultCacheMaxBytes;
    size_t                                      bytes_     = 0;
    uint64_t                                    useCounter_ = 0;
    uint64_t                                    hits_       = 0;
    uint64_t                                    misses_     = 0;
    std::map<std::string, ValueCacheEntry, std::less<>> entries_;
};

} // namespace feature
} // namespace agentxx
