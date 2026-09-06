#pragma once

#include "fmt/format.h"
#include <string>
#include <string_view>
#include <utility>

/// 界面翻译表列 (与 TUISettings 语言设置一致)
///
/// 语言取值定义在 tui_settings.h 的 TuiLanguage (Auto=0 自动 / ZhCn=1 简体中文 /
/// EnUs=2 English), 持久化键 `tui.lang` (global.db), 默认自动选择系统语言。
/// 本头文件仅提供查询接口, 翻译表与查找实现在 tui_i18n.cpp。
///
/// 设计约定:
/// - 每条目 = { key, en, zh }: en 列等于原英文界面文本, zh 列是简体中文;
///   键缺失时回退英文列, 仍缺失时返回 key 本身 (便于尽早发现漏配)
/// - 消息角色标记 ([Think]/[Tool]/[System]/[Permission]/[Interrupt] 等)、
///   协议字段标签 (args:/result:/tool_calls: 等)、插件提供文本保持原样不翻译
/// - 语言切换只切换查找表 (原子整数), 查询本身无锁, 任意线程可调用
class TuiI18n {
public:

    /// 获取全局单例
    static TuiI18n& instance();

    /// 查询当前语言下的文本 (返回静态存储的 string_view, 不拷贝)
    std::string_view t(std::string_view key) const noexcept;

    /// 带格式参数的查询 (占位符 {} 同 fmt 语义, 如 t("x", count))
    template<typename... Args>
    std::string t(std::string_view key, Args&&... args) const {
        return fmt::format(fmt::runtime(t(key)), std::forward<Args>(args)...);
    }

private:

    TuiI18n()  = default;
    ~TuiI18n() = default;
};

/// 便捷查询 (无参数, 返回 string_view)
inline std::string_view tr(std::string_view key) {
    return TuiI18n::instance().t(key);
}

/// 便捷查询 (带格式参数, 返回 std::string)
template<typename... Args>
inline std::string trf(std::string_view key, Args&&... args) {
    return TuiI18n::instance().t(key, std::forward<Args>(args)...);
}
