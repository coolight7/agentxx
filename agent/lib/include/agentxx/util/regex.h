#pragma once
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

    /// 正则匹配器 (hyperscan 后端 / std::regex 回退)
    ///
    /// **实例不可跨线程并发使用**:
    /// - hyperscan 后端在构造时分配一份 scratch (hs_alloc_scratch),
    ///   match/replace/remove 全程复用它; hyperscan 明确要求 scratch 不能
    ///   被多线程并发使用 (多线程需各自 hs_clone_scratch)
    /// - std::regex 后端同理: 同一 regex 对象被多线程并发 match 不保证安全
    /// - 因此本对象只能在单个线程内使用; 需要跨线程共享时, 每个线程各自
    ///   createRegex 一份 (或用线程局部实例), 不要放进插件上下文/静态缓存
    ///   后由多个线程调用
    XXRegex()                          = default;
    XXRegex(const XXRegex&)            = delete;
    XXRegex& operator=(const XXRegex&) = delete;

    [[nodiscard]] static std::shared_ptr<agentxx::util::XXRegex> createRegex(
        const std::string& regstr,
        unsigned int       flags           = defHSFlags_normal,
        bool               caseInsensitive = false
    );
    [[nodiscard]] static std::shared_ptr<agentxx::util::XXRegex> createRegex(
        const std::vector<std::string>& regstrs,
        unsigned int                    flags           = defHSFlags_normal,
        bool                            caseInsensitive = false
    );

    virtual ~XXRegex() {}

    // 匹配
    virtual bool match(std::string_view input, std::vector<XXRegexMatchResult>& results) const = 0;

    // 移除匹配的子串
    [[nodiscard]] virtual std::string
        remove(std::string_view input, std::vector<XXRegexMatchResult>& results) const
        = 0;

    // 替换匹配的子串
    [[nodiscard]] virtual std::string replace(
        std::string_view                 input,
        std::string_view                 target,
        std::vector<XXRegexMatchResult>& results
    ) const
        = 0;
    XXRegex& operator=(XXRegex&& other) = delete;
};

} // namespace util
} // namespace agentxx