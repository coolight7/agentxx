#pragma once

/// @file agentxx/util/env.h
/// @brief 统一的环境变量访问封装 (预设变量 -> 系统环境变量)
/// - 预设变量: 进程内全局存储 (启动时由 main 注入 AGENTXX_WORK_DIR / AGENTXX_EXEC_DIR 等),
///   优先级高于系统环境变量, 用于消除对 ::getenv 的直接调用 (MSVC C4996)
/// - 系统环境变量: Windows 使用 _dupenv_s (安全), POSIX 使用 ::getenv
/// - 读取顺序: 预设 -> 系统; 未找到返回 nullopt
/// - 写入预设变量仅影响本封装的读取, 不会修改操作系统环境 (避免跨进程污染/线程安全问题)
/// - 设计: 全局单例 class ApplicationEnv (Meyer's singleton), 替代此前的独立命名空间

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace agentxx {
namespace util {

/// 全局单例: 统一环境变量管理
/// - 预设变量存储于单例成员 (受互斥保护), 优先级高于系统环境变量
/// - 单例经 ApplicationEnv::instance() 访问, 进程生命周期内唯一
class ApplicationEnv {
public:

    /// 获取全局单例
    static ApplicationEnv& instance();

    ApplicationEnv(const ApplicationEnv&)            = delete;
    ApplicationEnv& operator=(const ApplicationEnv&) = delete;
    ApplicationEnv(ApplicationEnv&&)                 = delete;
    ApplicationEnv& operator=(ApplicationEnv&&)      = delete;

    // -----------------------------------------------------------------------
    // 预设环境变量 (进程内全局, 优先级高于系统)
    // -----------------------------------------------------------------------

    /// 设置预设环境变量 (覆盖已存在的值, 值可为空串)
    /// @param name  变量名 (不能为空)
    /// @param value 变量值
    void set(std::string_view name, std::string_view value);
    void set(std::string_view name, std::string&& value);
    void set(std::string_view name, const std::string& value);

    /// 移除单个预设变量 (不存在时无操作)
    void remove(std::string_view name);

    /// 清空全部预设变量
    void clear();

    /// 查询预设变量 (仅查预设, 不查系统)
    [[nodiscard]] std::optional<std::string> getPreset(std::string_view name);

    /// 是否存在预设变量
    [[nodiscard]] bool hasPreset(std::string_view name);

    // -----------------------------------------------------------------------
    // 系统环境变量 (安全封装)
    // -----------------------------------------------------------------------

    /// 查询系统环境变量 (不查预设)
    /// - Windows: _dupenv_s (线程安全, 已消除 C4996)
    /// - POSIX:   ::getenv
    [[nodiscard]] std::optional<std::string> getSystem(std::string_view name);

    [[nodiscard]] bool hasSystem(std::string_view name);

    // -----------------------------------------------------------------------
    // 统一查询 (预设 -> 系统)
    // -----------------------------------------------------------------------

    /// 查询环境变量 (优先级: 预设 -> 系统)
    /// @return 有值时返回 string, 无值时 nullopt
    [[nodiscard]] std::optional<std::string> get(std::string_view name);

    /// 查询环境变量, 带默认值
    /// - 仅提供 string_view 重载: 传 const char* 字面量时若再提供
    /// const std::string& 重载会构成等价用户定义转换, MSVC 报 C2668 歧义
    /// (GCC/Clang 选 string_view, 行为不一致); string_view 版本对 std::string
    /// 实参同样兼容 (隐式转换), 且无临时 string 构造开销
    [[nodiscard]] std::string getOr(std::string_view name, std::string_view defaultValue);

    /// 是否存在环境变量 (预设或系统任一存在即 true)
    [[nodiscard]] bool has(std::string_view name);

private:

    ApplicationEnv()  = default;
    ~ApplicationEnv() = default;

    std::mutex                                      m_mutex;
    std::map<std::string, std::string, std::less<>> m_preset;
};

// ---------------------------------------------------------------------------
// 便捷别名 (直接经 util 命名空间, 等价于 ApplicationEnv::instance().get)
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::optional<std::string> getEnv(std::string_view name) {
    return ApplicationEnv::instance().get(name);
}

[[nodiscard]] inline std::string getEnvOr(std::string_view name, std::string_view defaultValue) {
    return ApplicationEnv::instance().getOr(name, defaultValue);
}

[[nodiscard]] inline std::optional<std::string> getSystemEnv(std::string_view name) {
    return ApplicationEnv::instance().getSystem(name);
}

} // namespace util
} // namespace agentxx
