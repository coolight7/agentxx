#pragma once

#include "agentxx/util/settings_db.h"
#include <array>
#include <atomic>
#include <memory>
#include <string>

/// 动画等级 (从低到高)
///
/// 组件按所需效果声明最低动画等级, 经 TUISettings::isAnimationEnabled()
/// 判断当前设置是否达到该等级 (相等或更高即启用), 以便在低端终端/远程连接
/// 等场景下降级或关闭动画渲染:
/// - Disabled: 禁用全部动画
/// - Low:      仅基础动画 (如闪烁提示)
/// - Medium:   常规动画
/// - High:     增强动画
/// - Ultra:    极致动画 (启用全部动画效果)
enum class AnimationLevel : int {
    Disabled = 0,
    Low      = 1,
    Medium   = 2,
    High     = 3,
    Ultra    = 4,
};

/// TUI 全局设置存储 (进程级单例)
///
/// 集中管理 TUI 全局设置项, 供各组件跨线程读写:
/// - 主题 (ThemeKind: Dark/Light)
/// - 动画等级 (AnimationLevel)
/// - Info 侧边栏是否显示系统资源占用 (showSystemInfo)
///
/// 线程安全: 所有设置项均为 std::atomic, 读写无锁,
/// 可从 UI 线程 (渲染/事件) 与后台线程 (如资源监控线程) 并发访问。
///
/// 持久化: 启动时经 attachDb() 绑定全局设置数据库
/// ({dataDir}/sqlite/global.db), 设置项变更时同步写入;
/// 未绑定数据库时设置仅存在于内存 (进程生命周期内有效)。
class TUISettings {
public:

    /// 获取全局单例
    inline static TUISettings& instance() {
        static TUISettings inst;
        return inst;
    }

    /// 动画等级名称 (供设置弹窗展示)
    inline static constexpr std::array<const char*, 5> kAnimationLevelNames
        = {"Disabled", "Low", "Medium", "High", "Ultra"};

    /// 默认动画等级: Ultra (启用全部动画)
    inline static constexpr AnimationLevel kDefaultAnimationLevel = AnimationLevel::High;

    /// 主题枚举 (与 tui.theme 库中存储的整数值对应)
    enum ThemeKind : int {
        kThemeDark  = 0, ///< Dark (默认)
        kThemeLight = 1, ///< Light
    };

    TUISettings(const TUISettings&)            = delete;
    TUISettings& operator=(const TUISettings&) = delete;

    /// 绑定全局设置数据库并加载已存设置 (启动时调用一次)
    /// - 重复调用以首次为准; db 为空时忽略
    /// - 从库中恢复: 主题 / 动画等级 / showSystemInfo (键: tui.*)
    inline void attachDb(std::shared_ptr<agentxx::util::SettingsDb> db) noexcept {
        if (db_ || !db) {
            return;
        }
        db_ = std::move(db);
        // 恢复已存设置 (库损坏/无记录时保留默认值)
        auto theme = db_->getInt64("tui.theme", -1);
        if (theme == kThemeDark || theme == kThemeLight) {
            themeKind_.store(static_cast<int>(theme), std::memory_order_release);
        }
        auto anim = db_->getInt64("tui.animationLevel", -1);
        if (anim >= 0 && anim < static_cast<int64_t>(kAnimationLevelNames.size())) {
            animationLevel_.store(static_cast<int>(anim), std::memory_order_release);
        }
        if (auto v = db_->get("tui.showSystemInfo"); v.has_value()) {
            showSystemInfo_.store(*v == "1", std::memory_order_release);
        }
    }

    /// 当前主题 (ThemeKind 枚举值; 越界值视为 Dark)
    ThemeKind themeKind() const noexcept {
        const int v = themeKind_.load(std::memory_order_acquire);
        return v == kThemeLight ? kThemeLight : kThemeDark;
    }

    /// 设置主题 (非法值按 Dark 处理)
    /// - 变更同步持久化到全局设置数据库 (失败仅记日志, 不影响本次设置)
    inline void setThemeKind(ThemeKind kind) noexcept {
        themeKind_.store(kind == kThemeLight ? kThemeLight : kThemeDark, std::memory_order_release);
        if (db_) {
            db_->setInt64("tui.theme", themeKind_.load(std::memory_order_relaxed));
        }
    }

    /// 获取当前动画等级
    AnimationLevel animationLevel() const noexcept {
        return static_cast<AnimationLevel>(animationLevel_.load(std::memory_order_acquire));
    }

    /// 设置动画等级 (越界值不做 clamp, 直接存储; 调用方应使用合法枚举值)
    /// - 变更同步持久化到全局设置数据库 (失败仅记日志, 不影响本次设置)
    inline void setAnimationLevel(AnimationLevel level) noexcept {
        animationLevel_.store(static_cast<int>(level), std::memory_order_release);
        if (db_) {
            db_->setInt64("tui.animationLevel", static_cast<int64_t>(level));
        }
    }

    /// 动画等级名称 (当前设置值)
    std::string_view animationLevelName() const noexcept {
        return levelName(animationLevel());
    }

    /// 判断指定等级是否启用动画:
    /// 当 设置项的动画等级 >= required (相等或更高) 时返回 true。
    /// 特殊: 设置为 Disabled 时禁用全部动画, 恒返回 false。
    /// 组件用法: if (TUISettings::instance().isAnimationEnabled(AnimationLevel::Medium)) {...}
    bool isAnimationEnabled(AnimationLevel required) const noexcept {
        const AnimationLevel cur = animationLevel();
        return cur != AnimationLevel::Disabled && cur >= required;
    }

    /// Info 侧边栏是否显示系统资源占用 (CPU/内存); 默认开启
    bool showSystemInfo() const noexcept {
        return showSystemInfo_.load(std::memory_order_acquire);
    }

    /// 设置 Info 侧边栏系统资源显示开关
    /// - 变更同步持久化到全局设置数据库 (失败仅记日志, 不影响本次设置)
    inline void setShowSystemInfo(bool enabled) noexcept {
        showSystemInfo_.store(enabled, std::memory_order_release);
        if (db_) {
            db_->setBool("tui.showSystemInfo", enabled);
        }
    }

    /// 内部原子量引用 (供 TUICtx::showSystemInfo 指针指向单例存储,
    /// 保持既有组件经指针访问的方式不变)
    std::atomic<bool>& showSystemInfoRef() noexcept {
        return showSystemInfo_;
    }

private:

    TUISettings() :
        animationLevel_(static_cast<int>(kDefaultAnimationLevel)) {}

    /// 动画等级名称 (越界返回 "Unknown")
    inline static constexpr std::string_view levelName(AnimationLevel level) noexcept {
        const int idx = static_cast<int>(level);
        if (idx >= 0 && idx < static_cast<int>(kAnimationLevelNames.size())) {
            return kAnimationLevelNames[static_cast<size_t>(idx)];
        }
        return "Unknown";
    }

    /// 主题 (存储为 int 以便原子读写; 0=Dark, 1=Light)
    std::atomic<int> themeKind_{kThemeDark};
    /// 动画等级 (存储为 int 以便原子读写)
    std::atomic<int> animationLevel_;
    /// Info 侧边栏系统资源显示开关
    std::atomic<bool> showSystemInfo_{true};
    /// 全局设置数据库 (空 = 未持久化, 设置仅存内存)
    std::shared_ptr<agentxx::util::SettingsDb> db_;
};
