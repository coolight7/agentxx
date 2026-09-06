#pragma once

#include "agentxx/util/log.h"
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

/// 末尾思考展示模式
///
/// 指定末尾思考 (流式接收中或末尾消息) 的展示形态:
/// - AutoExpand: 自动展开 (流式阶段实时展开多行 Markdown 渲染思考过程)
/// - SingleLine: 保持单行折叠截取末尾指定长度字符显示 (终端更整洁, 不刷屏)
enum class TailThinkingMode : int {
    AutoExpand = 0, ///< 自动展开 (默认/流式展开渲染)
    SingleLine = 1, ///< 保持单行折叠截取末尾字符显示
};

/// 界面语言 (自动 / 简体中文 / English)
///
/// 语言取值供界面翻译表 [TuiI18n](/agent/client/include/agentxx-client/io/tui/framework/tui_i18n.h)
/// 查询使用: 界面代码只写翻译 key, 展示文本由实际生效语言决定。
/// - Auto: 自动检测当前系统语言环境, 在已支持语言列表中选择语言使用 (默认)
/// - ZhCn: 简体中文 (zh-cn)
/// - EnUs: English (en-us)
/// - 持久化键: tui.lang (global.db), 值为 0/1/2
enum class TuiLanguage : int {
    Auto = 0, ///< 自动 (根据系统语言环境选择, 默认)
    ZhCn = 1, ///< 简体中文 (zh-cn)
    EnUs = 2, ///< English (en-us)
};

/// 根据系统语言标签字符串在已支持的语言列表中匹配最佳语言
///
/// - 支持常见格式: BCP-47 (如 "zh-CN", "en-US", "zh-Hans-CN"),
///   POSIX locale (如 "zh_CN.UTF-8", "en_US.UTF-8"),
///   以及简写 ("zh", "en") 或语言名 ("chinese", "english")
/// - 多条目列表 (如 LANGUAGE="zh_CN:zh:en"): 依次尝试匹配首个已支持的语言
/// - 匹配规则:
///   1) 中文相关 (zh, zh-cn, zh-tw, zh-hk, chinese 等) -> TuiLanguage::ZhCn
///   2) 英文相关 (en, en-us, en-gb, english 等) -> TuiLanguage::EnUs
///   3) 其它非中文系统语言 (如 ja, ko, fr, de, es, ru 等) -> 回退到 TuiLanguage::EnUs (国际通用语言)
///   4) 空串或无法识别 (如 "C", "POSIX") -> 回退到默认中文 TuiLanguage::ZhCn
///
/// - `args`:
///     - [localeStr] 待匹配的系统语言标签或 locale 字符串
/// - `return` 匹配到的已支持语言 (ZhCn 或 EnUs, 永不返回 Auto)
TuiLanguage matchSupportedLanguage(std::string_view localeStr) noexcept;

/// 探测当前操作系统的语言环境标签 (跨平台: 环境变量 -> Win32 API -> setlocale)
///
/// - `return` 探测到的系统语言字符串 (未获取到返回空字符串)
std::string detectSystemLocale();

/// 探测当前系统语言环境并返回匹配的已支持语言 (ZhCn 或 EnUs)
TuiLanguage detectSystemLanguage();

/// 权限询问处理模式已移除: 改为由 yaml 配置文件 `permission.mode` 指定
/// (ask/all_ask/pass/deny, 见
/// [config.h](/agent/lib/include/agentxx/agent/config.h) 的
/// agentxx::agent::PermissionMode),
/// 服务端 CodeAgent 按模式注册规则, 客户端仅兜底处理到达的询问;
/// 启动时经 TUIClientAgentIO 构造参数注入, 不再作为 TUI 全局设置项。

/// TUI 全局设置存储 (进程级单例)
///
/// 集中管理 TUI 全局设置项, 供各组件跨线程读写:
/// - 主题 (ThemeKind: Dark/Light)
/// - 动画等级 (AnimationLevel)
/// - 日志等级 (LogLevel: Trace/Debug/Info/Warn/Error/Out, TUI 日志侧边栏过滤)
/// - 末尾思考展示模式 (TailThinkingMode: AutoExpand/SingleLine)
/// - 界面语言 (TuiLanguage: ZhCn/EnUs, 见 tui_i18n.h 翻译表)
///
/// 线程安全: 所有设置项均为 std::atomic, 读写无锁,
/// 可从 UI 线程 (渲染/事件) 与后台线程并发访问。
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

    /// 日志等级名称 (供设置弹窗展示; 与 agentxx::util::LogLevel 枚举值一一对应)
    inline static constexpr std::array<const char*, 6> kLogLevelNames
        = {"Trace", "Debug", "Info", "Warn", "Error", "Out"};

    /// 默认日志等级: Info (TUI 日志侧边栏仅显示 Info 及以上, Out 恒显示)
    inline static constexpr agentxx::util::LogLevel kDefaultLogLevel
        = agentxx::util::LogLevel::Info;

    /// 末尾思考模式名称 (供设置弹窗展示)
    inline static constexpr std::array<const char*, 2> kTailThinkingModeNames
        = {"Auto Expand", "Single Line"};

    /// 默认末尾思考展示模式: AutoExpand (自动展开)
    inline static constexpr TailThinkingMode kDefaultTailThinkingMode
        = TailThinkingMode::AutoExpand;

    /// 语言显示名称 (供设置弹窗展示; 与 TuiLanguage 枚举值一一对应)
    inline static constexpr std::array<const char*, 3> kLanguageNames
        = {"自动 (Auto)", "简体中文 (zh-cn)", "English (en-us)"};

    /// 默认界面语言: 自动 (根据系统语言环境在已支持语言列表中自动选择)
    inline static constexpr TuiLanguage kDefaultLanguage = TuiLanguage::Auto;

    /// 主题枚举 (与 tui.theme 库中存储的整数值对应)
    enum ThemeKind : int {
        kThemeDark  = 0, ///< Dark (默认)
        kThemeLight = 1, ///< Light
    };

    TUISettings(const TUISettings&)            = delete;
    TUISettings& operator=(const TUISettings&) = delete;

    /// 绑定全局设置数据库并加载已存设置 (启动时调用一次)
    /// - 重复调用以首次为准; db 为空时忽略
    /// - 从库中恢复: 主题 / 动画等级 / 日志等级 / 末尾思考模式 (键: tui.*)
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
        auto logLevel = db_->getInt64("tui.logLevel", -1);
        if (logLevel >= 0 && logLevel < static_cast<int64_t>(kLogLevelNames.size())) {
            logLevel_.store(static_cast<int>(logLevel), std::memory_order_release);
        }
        auto tailThink = db_->getInt64("tui.tailThinking", -1);
        if (tailThink >= 0 && tailThink < static_cast<int64_t>(kTailThinkingModeNames.size())) {
            tailThinkingMode_.store(static_cast<int>(tailThink), std::memory_order_release);
        }
        auto lang = db_->getInt64("tui.lang", -1);
        if (lang >= 0 && lang < static_cast<int64_t>(kLanguageNames.size())) {
            language_.store(static_cast<int>(lang), std::memory_order_release);
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

    /// 获取当前日志等级 (TUI 日志侧边栏过滤: 显示 >= 该等级的日志, Out 恒显示)
    agentxx::util::LogLevel logLevel() const noexcept {
        const int v = logLevel_.load(std::memory_order_acquire);
        if (v >= 0 && v < static_cast<int>(kLogLevelNames.size())) {
            return static_cast<agentxx::util::LogLevel>(v);
        }
        return kDefaultLogLevel;
    }

    /// 设置日志等级 (越界值回退默认 Info)
    /// - 变更同步持久化到全局设置数据库 (失败仅记日志, 不影响本次设置)
    inline void setLogLevel(agentxx::util::LogLevel level) noexcept {
        const int v = static_cast<int>(level);
        logLevel_.store(
            (v >= 0 && v < static_cast<int>(kLogLevelNames.size()))
                ? v
                : static_cast<int>(kDefaultLogLevel),
            std::memory_order_release
        );
        if (db_) {
            db_->setInt64("tui.logLevel", logLevel_.load(std::memory_order_relaxed));
        }
    }

    /// 日志等级名称 (当前设置值)
    std::string_view logLevelName() const noexcept {
        return logLevelName(logLevel());
    }

    /// 获取末尾思考展示模式
    TailThinkingMode tailThinkingMode() const noexcept {
        const int v = tailThinkingMode_.load(std::memory_order_acquire);
        if (v >= 0 && v < static_cast<int>(kTailThinkingModeNames.size())) {
            return static_cast<TailThinkingMode>(v);
        }
        return kDefaultTailThinkingMode;
    }

    /// 设置末尾思考展示模式
    /// - 变更同步持久化到全局设置数据库 (失败仅记日志, 不影响本次设置)
    inline void setTailThinkingMode(TailThinkingMode mode) noexcept {
        const int v = static_cast<int>(mode);
        tailThinkingMode_.store(
            (v >= 0 && v < static_cast<int>(kTailThinkingModeNames.size()))
                ? v
                : static_cast<int>(kDefaultTailThinkingMode),
            std::memory_order_release
        );
        if (db_) {
            db_->setInt64("tui.tailThinking", tailThinkingMode_.load(std::memory_order_relaxed));
        }
    }

    /// 末尾思考展示模式名称 (当前设置值)
    std::string_view tailThinkingModeName() const noexcept {
        return tailThinkingModeName(tailThinkingMode());
    }

    /// 获取当前界面语言设置项 (0=Auto, 1=ZhCn, 2=EnUs; 越界值按 Auto 处理)
    TuiLanguage language() const noexcept {
        const int v = language_.load(std::memory_order_acquire);
        if (v >= 0 && v < static_cast<int>(kLanguageNames.size())) {
            return static_cast<TuiLanguage>(v);
        }
        return kDefaultLanguage;
    }

    /// 设置界面语言 (非法值按 Auto 处理)
    /// - 变更同步持久化到全局设置数据库 (失败仅记日志, 不影响本次设置)
    inline void setLanguage(TuiLanguage lang) noexcept {
        const int v   = static_cast<int>(lang);
        const int val = (v >= 0 && v < static_cast<int>(kLanguageNames.size()))
                            ? v
                            : static_cast<int>(kDefaultLanguage);
        language_.store(val, std::memory_order_release);
        if (val == static_cast<int>(TuiLanguage::Auto)) {
            refreshAutoLanguage();
        }
        if (db_) {
            db_->setInt64("tui.lang", language_.load(std::memory_order_relaxed));
        }
    }

    /// 获取当前实际生效的界面语言 (始终返回已支持的具体语言: ZhCn 或 EnUs, 绝不返回 Auto)
    /// - 当 language() == Auto 时, 返回根据系统语言环境自动解析的语言
    /// - 当 language() != Auto 时, 直接返回用户指定的语言 (ZhCn 或 EnUs)
    TuiLanguage effectiveLanguage() const noexcept {
        const TuiLanguage cur = language();
        if (cur == TuiLanguage::Auto) {
            const int resolved = autoResolvedLanguage_.load(std::memory_order_acquire);
            return (resolved == static_cast<int>(TuiLanguage::EnUs)) ? TuiLanguage::EnUs
                                                                     : TuiLanguage::ZhCn;
        }
        return cur;
    }

    /// 获取当前真实生效使用的语言代码 ("en" 或 "zh-cn"; 绝不返回 "auto")
    inline std::string languageCode() const noexcept {
        return (effectiveLanguage() == TuiLanguage::ZhCn) ? "zh-cn" : "en";
    }

    /// 根据语言代码字符串设置语言 ("auto" -> Auto, "zh"/"zh-cn" -> ZhCn, 其余包括 "en" -> EnUs)
    inline void setLanguageByCode(std::string_view code) noexcept {
        std::string s{code};
        for (auto& c : s) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c + ('a' - 'A'));
            }
        }
        if (s == "auto") {
            setLanguage(TuiLanguage::Auto);
        } else if (s == "zh" || s == "zh-cn" || s.starts_with("zh-") || s.starts_with("zh_")
            || s.find("chinese") != std::string::npos) {
            setLanguage(TuiLanguage::ZhCn);
        } else {
            setLanguage(TuiLanguage::EnUs);
        }
    }

    /// 刷新自动模式下的系统语言探测缓存
    inline void refreshAutoLanguage() noexcept {
        autoResolvedLanguage_.store(
            static_cast<int>(detectSystemLanguage()),
            std::memory_order_release
        );
    }

    /// 语言显示名称 (当前设置值)
    std::string_view languageName() const noexcept {
        const int idx = static_cast<int>(language());
        if (idx >= 0 && idx < static_cast<int>(kLanguageNames.size())) {
            return kLanguageNames[static_cast<size_t>(idx)];
        }
        return "Unknown";
    }

private:

    TUISettings() :
        animationLevel_(static_cast<int>(kDefaultAnimationLevel)),
        autoResolvedLanguage_(static_cast<int>(detectSystemLanguage())) {}

    /// 动画等级名称 (越界返回 "Unknown")
    inline static constexpr std::string_view levelName(AnimationLevel level) noexcept {
        const int idx = static_cast<int>(level);
        if (idx >= 0 && idx < static_cast<int>(kAnimationLevelNames.size())) {
            return kAnimationLevelNames[static_cast<size_t>(idx)];
        }
        return "Unknown";
    }

    /// 日志等级名称 (越界返回 "Unknown")
    inline static constexpr std::string_view logLevelName(agentxx::util::LogLevel level) noexcept {
        const int idx = static_cast<int>(level);
        if (idx >= 0 && idx < static_cast<int>(kLogLevelNames.size())) {
            return kLogLevelNames[static_cast<size_t>(idx)];
        }
        return "Unknown";
    }

    /// 末尾思考模式名称 (越界返回 "Unknown")
    inline static constexpr std::string_view tailThinkingModeName(TailThinkingMode mode) noexcept {
        const int idx = static_cast<int>(mode);
        if (idx >= 0 && idx < static_cast<int>(kTailThinkingModeNames.size())) {
            return kTailThinkingModeNames[static_cast<size_t>(idx)];
        }
        return "Unknown";
    }

    /// 主题 (存储为 int 以便原子读写; 0=Dark, 1=Light)
    std::atomic<int> themeKind_{kThemeDark};
    /// 动画等级 (存储为 int 以便原子读写)
    std::atomic<int> animationLevel_;
    /// 日志等级 (存储为 int 以便原子读写; 与 agentxx::util::LogLevel 枚举值对应)
    std::atomic<int> logLevel_{static_cast<int>(kDefaultLogLevel)};
    /// 末尾思考展示模式 (存储为 int 以便原子读写; 0=AutoExpand, 1=SingleLine)
    std::atomic<int> tailThinkingMode_{static_cast<int>(kDefaultTailThinkingMode)};
    /// 界面语言设置 (存储为 int 以便原子读写; 0=Auto, 1=ZhCn, 2=EnUs)
    std::atomic<int> language_{static_cast<int>(kDefaultLanguage)};
    /// 自动模式下解析出的生效语言 (始终为已支持的具体语言: ZhCn 或 EnUs)
    std::atomic<int> autoResolvedLanguage_{static_cast<int>(TuiLanguage::ZhCn)};
    /// 全局设置数据库 (空 = 未持久化, 设置仅存内存)
    std::shared_ptr<agentxx::util::SettingsDb> db_;
};
