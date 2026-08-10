#pragma once

#include <array>
#include <atomic>
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

/// 权限询问处理模式 (PermissionMiddleware 判定 INTERRUPT 时客户端如何处理)
///
/// - Ask:  询问 — 弹出中断询问界面, 由用户决定允许/拒绝
/// - Pass: 通行 — 不询问, 直接放行 (相当于默认允许)
///
/// 注意: 该模式仅在客户端应答权限询问时生效 (询问经总线到达客户端);
/// 中间件已注册的显式规则 (ALLOW/DENY) 始终优先于本模式。
enum class PermissionMode : int {
    Ask  = 0,
    Pass = 1,
};

/// TUI 全局设置存储 (进程级单例)
///
/// 集中管理 TUI 全局设置项, 供各组件跨线程读写:
/// - 动画等级 (AnimationLevel)
/// - Info 侧边栏是否显示系统资源占用 (showSystemInfo)
/// - 权限询问处理模式 (PermissionMode): 询问/通行
///
/// 线程安全: 所有设置项均为 std::atomic, 读写无锁,
/// 可从 UI 线程 (渲染/事件) 与后台线程 (如资源监控线程) 并发访问。
///
/// 当前设置仅存在于内存 (进程生命周期内有效), 不做持久化。
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

    /// 权限询问处理模式名称 (供设置弹窗展示)
    inline static constexpr std::array<const char*, 2> kPermissionModeNames = {"Ask", "Pass"};

    /// 默认权限询问处理模式: 询问
    inline static constexpr PermissionMode kDefaultPermissionMode = PermissionMode::Ask;

    TUISettings(const TUISettings&)            = delete;
    TUISettings& operator=(const TUISettings&) = delete;

    /// 获取当前动画等级
    AnimationLevel animationLevel() const noexcept {
        return static_cast<AnimationLevel>(animationLevel_.load(std::memory_order_acquire));
    }

    /// 设置动画等级 (越界值不做 clamp, 直接存储; 调用方应使用合法枚举值)
    void setAnimationLevel(AnimationLevel level) noexcept {
        animationLevel_.store(static_cast<int>(level), std::memory_order_release);
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

    void setShowSystemInfo(bool enabled) noexcept {
        showSystemInfo_.store(enabled, std::memory_order_release);
    }

    /// 内部原子量引用 (供 TUICtx::showSystemInfo 指针指向单例存储,
    /// 保持既有组件经指针访问的方式不变)
    std::atomic<bool>& showSystemInfoRef() noexcept {
        return showSystemInfo_;
    }

    /// 当前权限询问处理模式
    PermissionMode permissionMode() const noexcept {
        return static_cast<PermissionMode>(permissionMode_.load(std::memory_order_acquire));
    }

    /// 设置权限询问处理模式 (调用方应使用合法枚举值)
    void setPermissionMode(PermissionMode mode) noexcept {
        permissionMode_.store(static_cast<int>(mode), std::memory_order_release);
    }

    /// 权限询问处理模式名称 (当前设置值; 越界返回 "Unknown")
    std::string_view permissionModeName() const noexcept {
        const int idx = static_cast<int>(permissionMode());
        if (idx >= 0 && idx < static_cast<int>(kPermissionModeNames.size())) {
            return kPermissionModeNames[static_cast<size_t>(idx)];
        }
        return "Unknown";
    }

private:

    TUISettings() :
        animationLevel_(static_cast<int>(kDefaultAnimationLevel)),
        permissionMode_(static_cast<int>(kDefaultPermissionMode)) {}

    /// 动画等级名称 (越界返回 "Unknown")
    inline static constexpr std::string_view levelName(AnimationLevel level) noexcept {
        const int idx = static_cast<int>(level);
        if (idx >= 0 && idx < static_cast<int>(kAnimationLevelNames.size())) {
            return kAnimationLevelNames[static_cast<size_t>(idx)];
        }
        return "Unknown";
    }

    /// 动画等级 (存储为 int 以便原子读写)
    std::atomic<int> animationLevel_;
    /// Info 侧边栏系统资源显示开关
    std::atomic<bool> showSystemInfo_{true};
    /// 权限询问处理模式 (存储为 int 以便原子读写)
    std::atomic<int> permissionMode_;
};
