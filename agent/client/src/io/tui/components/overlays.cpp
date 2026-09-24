#include "agentxx-client/io/tui/components/overlays.h"
#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/framework/tui_i18n.h"
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx-client/io/tui/plugin_ui_items.h"
#include "agentxx-client/io/tui/surface.h"
#include "agentxx-client/io/tui/text_layout.h"
#include "agentxx/agent/config_static.h"
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/ui/text_width.h"
#include "agentxx/util/exception.h"
#include "ftxui/component/component.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/terminal.hpp"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include <algorithm>
#include <filesystem>
#include <markdown/dom_builder.hpp>
#include <markdown/parser.hpp>
#include <markdown/state_diagram.hpp>
#include <markdown/text_utils.hpp>

#if XX_IS_WIN_D
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif XX_IS_MACOS_D || XX_IS_IOS_D
#include <mach-o/dyld.h> // _NSGetExecutablePath
#include <vector>
#endif

namespace agentxx::client {

using namespace ftxui;

// ---------------------------------------------------------------------------
// 弹窗  面性风格外框见 [surface.h](/agent/client/include/agentxx-client/io/tui/surface.h):
// 标题栏/内容区/底部提示栏以不同背景色区分, 不使用边框与分割线。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// ModelSelectorOverlay
// ---------------------------------------------------------------------------

ModelSelectorOverlay::ModelSelectorOverlay(TUICtx& ctx) :
    ctx_(ctx),
    style_(UiActionStyle::fromTheme(*ctx.theme)) {}

void ModelSelectorOverlay::buildItems() {
    const auto& st = *ctx_.frameState;

    std::vector<UiActionItem> items;
    items.reserve(st.modelNames.size());
    for (const auto& name : st.modelNames) {
        items.push_back(UiActionItem{
            .id    = name,
            .label = name,
            .onActivate =
                [this, model = std::string{name}] {
                    confirmItem(model);
                },
        });
    }
    list_.setItems(std::move(items));

    // 首次渲染时把选中项对齐到当前使用中的模型 (须在 setItems 之后: 选中项按
    // 条目表定位; 之后以用户的选择为准)
    if (!initialAligned_) {
        list_.selectById(st.cachedModelName);
        initialAligned_ = true;
    }
}

Element ModelSelectorOverlay::OnRender() {
    const auto& theme      = *ctx_.theme;
    const int   maxVisible = std::max(5, ctx_.terminalSize().dimy / 2);

    // 主题可能被设置弹窗切换: 每帧按当前主题刷新条目配色
    style_ = UiActionStyle::fromTheme(theme);
    hits_.beginFrame();
    buildItems();

    Element list;
    if (list_.empty()) {
        // 尚未收到服务端模型信息响应 → 加载中; 已收到但为空 → 确实无可用模型
        list = text(ctx_.frameState->modelInfoLoaded ? tr("model.empty") : tr("model.loading"))
               | theme.dim();
    } else {
        // 条目整行高亮由 UiActionStyle 提供; 选中项带 focus, 配合 yframe 自动滚入视口
        list = list_.render(hits_, style_) | bold | yframe | vscroll_indicator
               | size(HEIGHT, LESS_THAN, maxVisible);
    }

    const auto surface = TuiSurfaceStyle::fromTheme(theme);
    return tuiSurfacePopup(surface, tr("model.title"), std::move(list), tr("model.hint"))
           | size(WIDTH, LESS_THAN, 50);
}

bool ModelSelectorOverlay::OnEvent(Event event) {
    if (event == Event::Escape) {
        ctx_.postRedraw();
        close();
        return true;
    }
    // 上下键/Enter/鼠标点击命中统一由条目列表处理 (命中 = 选中 + 激活 = 切换模型)
    const bool handled
        = event.is_mouse() ? list_.onMouseEvent(event.mouse(), hits_) : list_.onKeyEvent(event);
    if (handled) {
        ctx_.postRedraw();
        flushActivation();
        return true;
    }
    return true;
}

void ModelSelectorOverlay::confirmItem(std::string_view model) {
    if (model.empty()) {
        return;
    }
    ctx_.state->mutate([&](TUIRenderState& st) {
        st.cachedModelName = std::string{model};
    });
    if (onConfirm_) {
        onConfirm_(std::string{model});
    }
    closeRequested_ = true;
}

void ModelSelectorOverlay::flushActivation() {
    if (closeRequested_) {
        close();
    }
}

void ModelSelectorOverlay::close() {
    closeRequested_ = false;
    if (onClose_) {
        onClose_();
    }
}

// ---------------------------------------------------------------------------
// SessionSelectorOverlay
// ---------------------------------------------------------------------------

SessionSelectorOverlay::SessionSelectorOverlay(TUICtx& ctx) :
    ctx_(ctx),
    style_(UiActionStyle::fromTheme(*ctx.theme)) {
    // 选择项接近已加载列表末尾时预取下一页 (提前 kSessionPrefetchAhead 项):
    // 实现方内部做执行中去重与 hasMore 边界判断, 高频调用安全
    list_.onSelectionChanged([this](int index) {
        if (index + kSessionPrefetchAhead >= static_cast<int>(list_.size())
            && ctx_.requestMoreSessions) {
            ctx_.requestMoreSessions();
        }
    });
}

void SessionSelectorOverlay::buildItems() {
    const auto& st = *ctx_.frameState;

    std::vector<UiActionItem> items;
    items.reserve(st.sessionList.size() + 1);

    // 顶部固定 "新会话" 入口 (列表加载中也常驻, 保证始终可新建)
    items.push_back(UiActionItem{
        .id    = kNewSessionId,
        .label = std::string{tr("session.new")},
        .onActivate =
            [this] {
                requestClose({});
            },
    });

    for (const auto& s : st.sessionList) {
        // 第一行: 会话名称 (title 为空时回退 sessionId); 第二行: 最近活动日期
        const bool        isCurrent = (s.sessionId == ctx_.sessionId);
        const std::string title     = s.title.empty() ? s.sessionId : s.title;
        items.push_back(UiActionItem{
            .id    = std::string{kSessionIdPrefix} + s.sessionId,
            .label = isCurrent ? trf("session.current", title) : title,
            .hint  = utilxx_base::formatDateTimeMilliseconds(s.lastActiveMs),
            .onActivate =
                [this, id = s.sessionId] {
                    requestClose(id);
                },
        });
    }
    list_.setItems(std::move(items));
}

Element SessionSelectorOverlay::OnRender() {
    const auto& theme      = *ctx_.theme;
    const int   maxVisible = std::max(5, ctx_.terminalSize().dimy / 2);

    style_ = UiActionStyle::fromTheme(theme);
    hits_.beginFrame();
    buildItems();

    // 条目版式: 两行 (名称 / 最近活动时间)
    auto rowBuilder = [&](const UiActionItem& item, bool selected, size_t) -> Element {
        Element row = vbox({
            text(item.label),
            text(item.hint) | theme.dim(),
        });
        if (selected) {
            row = row | bgcolor(theme.buttonActiveBgColor) | color(theme.buttonActiveTextColor)
                  | focus;
        } else {
            row = row | color(theme.normalColor);
        }
        return row;
    };

    // 分页状态行 (非选择项, 不登记命中): 加载中显示提示; 还有未加载会话时显示
    // 续取提示与已加载进度
    const auto& st = *ctx_.frameState;
    std::string tailHint;
    if (!st.sessionListLoaded) {
        tailHint = std::string{tr("session.loading")};
    } else if (st.sessionList.empty()) {
        tailHint = std::string{tr("session.empty")};
    } else if (st.sessionListLoadingMore) {
        tailHint = std::string{tr("session.loadingMore")};
    } else if (st.sessionListHasMore) {
        tailHint = st.sessionListTotalCount > 0
                       ? trf("session.loadedMore", st.sessionList.size(), st.sessionListTotalCount)
                       : std::string{tr("session.loadMore")};
    }

    Elements rows;
    rows.push_back(list_.render(hits_, style_, rowBuilder));
    if (!tailHint.empty()) {
        rows.push_back(text(tailHint) | theme.dim());
    }

    const auto surface = TuiSurfaceStyle::fromTheme(theme);
    return tuiSurfacePopup(
               surface,
               tr("session.title"),
               vbox(std::move(rows)) | bold | yframe | vscroll_indicator
                   | size(HEIGHT, LESS_THAN, maxVisible),
               tr("session.hint")
           )
           | size(WIDTH, LESS_THAN, 70);
}

bool SessionSelectorOverlay::OnEvent(Event event) {
    if (event == Event::Escape) {
        ctx_.postRedraw();
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    // Up/Down/Home/End 移动 (接近末尾时自动预取), Enter/鼠标点击命中 = 确认
    const bool handled
        = event.is_mouse() ? list_.onMouseEvent(event.mouse(), hits_) : list_.onKeyEvent(event);
    if (handled) {
        ctx_.postRedraw();
        flushActivation();
        return true;
    }
    return true;
}

void SessionSelectorOverlay::requestClose(std::string sessionId) {
    // 只记录目标: 真正的关闭/切换在条目列表事件处理返回后执行 —— 在条目激活动作内
    // 关闭弹窗会析构正在被遍历的条目表 (闭包正在执行中)
    closeRequested_   = true;
    pendingSessionId_ = std::move(sessionId);
}

void SessionSelectorOverlay::flushActivation() {
    if (!closeRequested_) {
        return;
    }
    closeRequested_    = false;
    std::string target = std::move(pendingSessionId_);
    pendingSessionId_.clear();
    if (onClose_) {
        onClose_();
    }
    if (target.empty()) {
        // "新会话" 入口: 新建会话 (无历史)
        if (onNewSession_) {
            onNewSession_();
        }
        return;
    }
    if (onSelect_) {
        onSelect_(std::move(target));
    }
}

// ---------------------------------------------------------------------------
// SettingsOverlay
// ---------------------------------------------------------------------------

namespace {

/// 设置条目版式常量
/// - 单个条目占 1 行; 条目之间 1 行间距, 分组标题 1 行 (标题前另有一空行)
/// - 外框行数: 上下内边距 2 + 标题栏 1 + 内容与区域之间 2 + 底部提示 1
/// - 内容超出可用高度时不再压缩间距: 内容区限高并可滚动 (选中项自动滚入视口)
constexpr int kSettingsSurfaceRows = 6;

} // namespace

SettingsOverlay::SettingsOverlay(TUICtx& ctx) :
    ctx_(ctx) {
    list_.setRowGap(1);
}

void SettingsOverlay::buildItems() {
    // 分组标题 (同一分组的条目填同一个标题; 见 [UiActionItem::group])
    const std::string groupInterface = std::string{tr("settings.groupInterface")};
    const std::string groupDisplay   = std::string{tr("settings.groupDisplay")};
    const std::string groupUpdate    = std::string{tr("settings.groupUpdate")};
    const std::string groupOther     = std::string{tr("settings.groupOther")};

    // 条目文字: 一个条目占一行, 文字取 `settings.*Value` (条目名称 + 当前值 / 动作
    // 文案已含在整句里), 不再另起一行重复显示条目名称
    list_.setItems({
  // ---- 界面: 主题 / 动画 / 语言 ----
  // 主题 (点击/Enter 循环切换 Dark <-> Light)
        {.id    = "theme",
         .label = trf("settings.themeValue", ctx_.theme->name),
         .group = groupInterface,
         .onActivate =
             [this] {
                 cycleTheme();
             }},
 // 动画等级 (点击/Enter 循环切换)
        {.id    = "animation",
         .label = trf("settings.animValue", TUISettings::instance().animationLevelName()),
         .group = groupInterface,
         .onActivate =
             [this] {
                 cycleAnimationLevel();
             }},
 // 界面语言 (点击/Enter 循环切换)
        {.id    = "language",
         .label = trf("settings.langValue", TUISettings::instance().languageName()),
         .group = groupInterface,
         .onActivate =
             [this] {
                 cycleLanguage();
             }},

 // ---- 显示: 日志 / 末尾思考 ----
  // 日志等级 (点击/Enter 循环切换; TUI 日志侧边栏按此过滤)
        {.id    = "log-level",
         .label = trf("settings.logValue", TUISettings::instance().logLevelName()),
         .group = groupDisplay,
         .onActivate =
             [this] {
                 cycleLogLevel();
             }},
 // 末尾思考展示模式 (点击/Enter 循环切换: Auto Expand <-> Single Line)
        {.id    = "tail-thinking",
         .label = trf("settings.thinkValue", TUISettings::instance().tailThinkingModeName()),
         .group = groupDisplay,
         .onActivate =
             [] {
                 cycleTailThinkingMode();
             }},
 // ---- 更新: 启动时检查开关 + 立即检查 ----
  // 启动时检查更新 (点击/Enter 切换 开/关; 仅影响下次启动)
        {.id    = "check-update",
         .label = trf(
             "settings.updateValue", std::string{
                 tr(TUISettings::instance().checkUpdateOnStartup() ? "settings.switchOn"
                                                                   : "settings.switchOff")
             }
         ), .group = groupUpdate,
         .onActivate =
             [] {
                 cycleCheckUpdateOnStartup();
             }},
 // 检查更新 (点击/Enter 立即检查一次; 有新版本时外部打开更新提示弹窗)
        {.id    = "check-update-now",
         .label = std::string{tr("settings.checkUpdateValue")},
         .group = groupUpdate,
         .onActivate =
             [this] {
                 if (onCheckUpdate_) {
                     onCheckUpdate_();
                 }
             }},
 // ---- 其他: 快捷键 / 信息 ----
  // 快捷键 (只读列表: 显示插件已注册的全局快捷键条数; 打开列表弹窗查看详情)
        {.id    = "keybinds",
         .label = trf("settings.keybindValue", keybindCount()),
         .group = groupOther,
         .onActivate =
             [this] {
                 if (onKeybindList_) {
                     onKeybindList_();
                 }
             }},
 // Info (点击/Enter 打开关于弹窗)
        {.id    = "about",
         .label = std::string{tr("settings.aboutValue")},
         .group = groupOther,
         .onActivate =
             [this] {
                 if (onAbout_) {
                     onAbout_();
                 }
             }},
    });
}

Element SettingsOverlay::OnRender() {
    const auto& theme = *ctx_.theme;

    hits_.beginFrame();
    buildItems();
    // 主题可能被本弹窗切换: 每帧按当前主题刷新配色 (分组标题色也来自这里)
    style_ = UiActionStyle::fromTheme(theme);

    // 内容区限高: 外框固定占 [kSettingsSurfaceRows] 行 (上下内边距/标题栏/间距/底部提示),
    // 内容更高时弹窗不超出终端, 由滚动区显示其余条目 (选中项自动滚入视口)
    const int termH          = std::max(1, ctx_.terminalSize().dimy);
    const int maxContentRows = std::max(1, termH - kSettingsSurfaceRows);

    // 条目版式: 一个条目一行 (整行色带, 即命中区域); 条目之间留一空行
    // - 选中态: 高亮背景覆盖整行 (与模型/会话列表弹窗的整行高亮一致)
    // - 非选中态: 浅色色带同样覆盖整行 (仅配色不同, 行宽与选中态一致)
    // - 面性风格: 不使用边框/下划线; 左右留白由外框统一提供
    auto rowBuilder = [&](const UiActionItem& item, bool selected, size_t) -> Element {
        Element row = hbox({
            text(item.label),
            filler(),
        });
        if (selected) {
            row = row | bgcolor(theme.buttonActiveBgColor) | color(theme.buttonActiveTextColor)
                  | bold;
        } else {
            row = row | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
        }
        // 选中项带 focus: 交给 yframe 把选中项滚入视口 (条目多/终端过矮时)
        return selected ? (std::move(row) | focus) : std::move(row);
    };

    // 内容区: 条目 + 分组标题 + 间距; 超出限高时右侧显示滚动条 (vscroll_indicator 要在
    // yframe 内侧: 它按"内容高度 vs 可见高度"决定是否画滚动条)
    const auto surface = TuiSurfaceStyle::fromTheme(theme);
    return tuiSurfacePopup(
               surface,
               tr("settings.title"),
               list_.render(hits_, style_, rowBuilder) | vscroll_indicator | yframe
                   | size(HEIGHT, LESS_THAN, maxContentRows),
               tr("settings.hint")
           )
           | size(WIDTH, LESS_THAN, 80);
}

bool SettingsOverlay::OnEvent(Event event) {
    if (event == Event::Escape) {
        ctx_.postRedraw();
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    if (event.is_mouse()) {
        // 滚轮 = 移动选中项 (与文件选择弹窗一致); 滚动区随选中项自动滚动
        const auto& mouse = event.mouse();
        if (mouse.button == Mouse::WheelUp || mouse.button == Mouse::WheelDown) {
            list_.onKeyEvent(mouse.button == Mouse::WheelUp ? Event::ArrowUp : Event::ArrowDown);
            ctx_.postRedraw();
            return true;
        }
    }
    // 上下键选择条目, Enter/鼠标点击命中激活 (切换设置项; 弹窗保持打开便于连续调整)
    const bool handled
        = event.is_mouse() ? list_.onMouseEvent(event.mouse(), hits_) : list_.onKeyEvent(event);
    if (handled) {
        ctx_.postRedraw();
        return true;
    }
    // 弹窗吞掉其余事件 (模态: 不得落到被遮挡的主界面)
    return true;
}

// ---------------------------------------------------------------------------
// LogMenuOverlay
// ---------------------------------------------------------------------------

LogMenuOverlay::LogMenuOverlay(TUICtx& ctx) :
    ctx_(ctx) {}

void LogMenuOverlay::buildItems() {
    list_.setItems({
        {.id    = "llm-context",
         .label = std::string{tr("menu.llmContext")},
         .onActivate =
             [this] {
                 if (onLlmContext_) {
                     onLlmContext_();
                 }
             }},
        {.id    = "summary-context",
         .label = std::string{tr("menu.summaryContext")},
         .onActivate =
             [this] {
                 if (onSummyContext_) {
                     onSummyContext_();
                 }
             }},
        {.id    = "clear-logs",
         .label = std::string{tr("menu.clearLogs")},
         .onActivate =
             [this] {
                 if (onClearLogs_) {
                     onClearLogs_();
                 }
             }},
    });
}

Element LogMenuOverlay::OnRender() {
    const auto& theme = *ctx_.theme;

    style_          = UiActionStyle::fromTheme(theme);
    style_.normalBg = theme.buttonBgColor;
    style_.normalFg = theme.buttonTextColor;
    hits_.beginFrame();
    buildItems();
    // 菜单项: 整行背景色块 (面性风格: 不用 [] 括号描边, 选中态换高亮背景)
    list_.setRowGap(1);

    const auto surface = TuiSurfaceStyle::fromTheme(theme);
    return tuiSurfacePopup(surface, tr("menu.title"), list_.render(hits_, style_), tr("menu.hint"))
           | size(WIDTH, EQUAL, 36);
}

bool LogMenuOverlay::OnEvent(Event event) {
    if (event == Event::Escape) {
        ctx_.postRedraw();
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    // 上下键选择, Enter/鼠标点击命中激活 (LLM Context / 总结上下文 / 清空日志)
    const bool handled
        = event.is_mouse() ? list_.onMouseEvent(event.mouse(), hits_) : list_.onKeyEvent(event);
    if (handled) {
        ctx_.postRedraw();
        return true;
    }
    // 弹窗吞掉其余事件
    return true;
}

void SettingsOverlay::cycleTheme() {
    auto&      settings = TUISettings::instance();
    const bool light    = settings.themeKind() == TUISettings::kThemeLight;
    settings.setThemeKind(light ? TUISettings::kThemeDark : TUISettings::kThemeLight);
    *ctx_.theme = light ? TUITheme::darkTheme() : TUITheme::lightTheme();
    if (onThemeChange_) {
        onThemeChange_();
    }
}

void SettingsOverlay::cycleAnimationLevel() {
    auto&     settings = TUISettings::instance();
    const int next     = (static_cast<int>(settings.animationLevel()) + 1)
                     % static_cast<int>(TUISettings::kAnimationLevelNames.size());
    settings.setAnimationLevel(static_cast<AnimationLevel>(next));
    // 通知外部同步动画门控 (插件定时器在 Disabled 等级下不注册)
    if (onAnimationLevelChange_) {
        onAnimationLevelChange_();
    }
}

void SettingsOverlay::cycleLogLevel() {
    auto&     settings = TUISettings::instance();
    const int next     = (static_cast<int>(settings.logLevel()) + 1)
                     % static_cast<int>(TUISettings::kLogLevelNames.size());
    settings.setLogLevel(static_cast<utilxx_base::LogLevel>(next));
    if (onLogLevelChange_) {
        onLogLevelChange_();
    }
}

void SettingsOverlay::cycleTailThinkingMode() {
    auto&     settings = TUISettings::instance();
    const int next     = (static_cast<int>(settings.tailThinkingMode()) + 1)
                     % static_cast<int>(TUISettings::kTailThinkingModeNames.size());
    settings.setTailThinkingMode(static_cast<TailThinkingMode>(next));
}

void SettingsOverlay::cycleLanguage() {
    auto&     settings = TUISettings::instance();
    const int next     = (static_cast<int>(settings.language()) + 1)
                     % static_cast<int>(TUISettings::kLanguageNames.size());
    settings.setLanguage(static_cast<TuiLanguage>(next));
    // 语言变化: 通知外部刷新静态文本 (侧边栏标签/输入框占位符/缓存等),
    // 设置弹窗保持打开, 语言立即生效 (切换后本弹窗自身经 ctx_.postRedraw 重建)
    if (onLanguageChange_) {
        onLanguageChange_();
    }
}

void SettingsOverlay::cycleCheckUpdateOnStartup() {
    auto& settings = TUISettings::instance();
    settings.setCheckUpdateOnStartup(!settings.checkUpdateOnStartup());
}

size_t SettingsOverlay::keybindCount() const {
    if (!ctx_.pluginManager) {
        return 0;
    }
    auto snapshot = ctx_.pluginManager->uiRegistrySnapshot();
    return snapshot ? snapshot->keybinds.size() : 0;
}

// ---------------------------------------------------------------------------
// UpdateNoticeOverlay
// ---------------------------------------------------------------------------

UpdateNoticeOverlay::UpdateNoticeOverlay(
    TUICtx&     ctx,
    std::string currentVersion,
    std::string latestTag,
    std::string url
) :
    ctx_(ctx),
    currentVersion_(std::move(currentVersion)),
    latestTag_(std::move(latestTag)),
    url_(std::move(url)) {}

Element UpdateNoticeOverlay::OnRender() {
    const auto& theme = *ctx_.theme;

    // 帧首清空命中表: 未渲染出来的按钮不参与命中 (见 ui_hit.h)
    hits_.beginFrame();

    const auto surface = TuiSurfaceStyle::fromTheme(theme);

    // 版本行: "新版本 {当前版本} -> {新版本}" (新版本标签用强调色突出)
    Element versionLine = text(trf("update.versionLine", currentVersion_, latestTag_)) | bold
                          | color(theme.accentColor);
    // 链接行: "· {发布页链接}"; 链接可能很长, 用 paragraph 按可用宽度换行 (不截断,
    // 用户可用鼠标拖选复制)
    Element linkLine = paragraph(fmt::format("· {}", url_)) | color(theme.normalColor);
    // 下载按钮 (面性风格色块, 与其它弹窗按钮一致): 点击/Enter 打开浏览器
    Element button = hits_.add(
        text(tr("update.download")) | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor)
            | bold,
        std::string{kDownloadHitId}
    );

    Element content = vbox({
        std::move(versionLine),
        std::move(linkLine),
        text(""),
        hbox({filler(), std::move(button), filler()}),
    });
    return tuiSurfacePopup(surface, tr("update.title"), std::move(content), tr("update.hint"))
           | size(WIDTH, LESS_THAN, 74);
}

bool UpdateNoticeOverlay::OnEvent(Event event) {
    if (event == Event::Escape) {
        ctx_.postRedraw();
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    // Enter 等价点击 [ 前往下载 ]
    if (event == Event::Return) {
        activateDownload();
        return true;
    }
    if (event.is_mouse()) {
        if (hits_.findClick(event.mouse()) != nullptr) {
            activateDownload();
            return true;
        }
        // 模态: 其余鼠标事件被吞掉 (不下发给被遮挡的主界面)
        return true;
    }
    return true;
}

void UpdateNoticeOverlay::activateDownload() {
    ctx_.postRedraw();
    if (onDownload_) {
        // 打开浏览器与"是否关闭弹窗"由外部决定: 关闭动作在外部延后到本次事件
        // 处理返回后执行 (在自身事件处理里关闭弹窗会析构正在执行的弹窗对象)
        onDownload_();
    }
}

ftxui::Box UpdateNoticeOverlay::downloadButtonBox() const {
    for (const auto& entry : hits_.entries()) {
        if (std::string_view{entry.payload.id} == kDownloadHitId) {
            return *entry.box;
        }
    }
    return agentxx::client::kNoBox;
}

// ---------------------------------------------------------------------------
// KeybindListOverlay
// ---------------------------------------------------------------------------

namespace {

/// 键位列宽度上限 (键位描述通常很短; 超长时按显示列宽截断, 避免挤掉说明)
constexpr int kKeybindKeyColumnMax = 18;

/// 键位列宽度 (取最长键位, 受上限约束)
int keybindKeyColumnWidth(const std::vector<agentxx::plugin::ClientKeybind>& binds) {
    int width = 0;
    for (const auto& b : binds) {
        width = std::max(width, agentxx::ui::displayWidth(b.keys));
    }
    return std::min(width, kKeybindKeyColumnMax);
}

} // namespace

KeybindListOverlay::KeybindListOverlay(TUICtx& ctx) :
    ctx_(ctx) {
    scrollable_ = std::make_shared<Scrollable>([this]() -> std::vector<ScrollItem> {
        return buildItems();
    });
    scrollable_->setStickToBottom(false);
    Add(scrollable_);
}

std::vector<ScrollItem> KeybindListOverlay::buildItems() {
    const auto& theme = *ctx_.theme;

    // 快照读取 (UI 线程短锁; 未装配插件管理器时按"无注册"处理)
    std::vector<agentxx::plugin::ClientKeybind>         binds;
    std::vector<agentxx::plugin::ClientKeybindConflict> conflicts;
    if (ctx_.pluginManager) {
        auto snapshot = ctx_.pluginManager->uiRegistrySnapshot();
        if (snapshot) {
            binds     = snapshot->keybinds;
            conflicts = snapshot->keybindConflicts;
        }
    }
    // 按键位排序展示 (注册顺序随插件加载顺序变化, 排序后更稳定)
    std::sort(binds.begin(), binds.end(), [](const auto& a, const auto& b) {
        return a.keys < b.keys;
    });
    std::sort(conflicts.begin(), conflicts.end(), [](const auto& a, const auto& b) {
        if (a.keys != b.keys) {
            return a.keys < b.keys;
        }
        return a.plugin < b.plugin;
    });
    keybindCount_ = binds.size();

    std::vector<ScrollItem> items;
    if (binds.empty()) {
        items.push_back(ScrollItem{text(std::string{tr("keybind.empty")}) | theme.dim(), false});
        items.push_back(ScrollItem{text(""), false});
    }

    const int keyWidth = keybindKeyColumnWidth(binds);
    for (const auto& bind : binds) {
        // 键位列: 按显示列宽截断后补齐, 让后续列对齐 (宽字符安全)
        const std::string keyText = agentxx::ui::padRightToWidth(
            agentxx::ui::truncateToWidth(bind.keys, keyWidth),
            keyWidth
        );
        const std::string desc
            = bind.description.empty() ? std::string{tr("keybind.noDesc")} : bind.description;
        items.push_back(ScrollItem{
            hbox({
                text(keyText) | bold | color(theme.accentColor),
                text("  "),
                text(desc) | color(theme.normalColor),
                filler(),
                text(bind.plugin) | color(theme.hintColor),
            }),
            false,
        });
    }

    // 冲突段: 键位被占用导致注册失败 (跨插件); 帮用户解释"快捷键没生效"
    if (!conflicts.empty()) {
        items.push_back(ScrollItem{text(""), false});
        items.push_back(ScrollItem{
            text(trf("keybind.conflictTitle", conflicts.size())) | bold | color(theme.errorColor),
            false,
        });
        for (const auto& conflict : conflicts) {
            const std::string keyText = agentxx::ui::padRightToWidth(
                agentxx::ui::truncateToWidth(conflict.keys, keyWidth),
                keyWidth
            );
            items.push_back(ScrollItem{
                hbox({
                    text(keyText) | bold | color(theme.errorColor),
                    text("  "),
                    text(trf("keybind.conflictLine", conflict.plugin, conflict.owner))
                        | color(theme.hintColor),
                    filler(),
                }),
                false,
            });
        }
    }

    return items;
}

Element KeybindListOverlay::OnRender() {
    const auto& theme    = *ctx_.theme;
    const auto  termSize = ctx_.terminalSize();
    const int   margin   = 2;
    const int   termW    = std::max(1, termSize.dimx);
    const int   termH    = std::max(1, termSize.dimy);
    const int   wantW    = std::max(44, std::min(78, termW * 4 / 5));
    const int   wantH    = std::max(10, std::min(24, termH * 4 / 5));
    const int   popupW   = std::min(wantW, std::max(1, termW - margin * 2));
    const int   popupH   = std::min(wantH, std::max(1, termH - margin * 2));
    const auto  style    = TuiSurfaceStyle::fromTheme(theme);
    return tuiSurfacePopup(
               style,
               tr("keybind.title"),
               scrollable_->Render() | flex,
               tr("keybind.hint")
           )
           | size(WIDTH, GREATER_THAN, popupW) | size(WIDTH, LESS_THAN, popupW)
           | size(HEIGHT, GREATER_THAN, popupH) | size(HEIGHT, LESS_THAN, popupH);
}

bool KeybindListOverlay::OnEvent(Event event) {
    if (event == Event::Escape || event == Event::Return) {
        ctx_.postRedraw();
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    if (event.is_mouse()) {
        if (scrollable_->OnEvent(event)) {
            ctx_.postRedraw();
            return true;
        }
        return true;
    }
    if (event == Event::ArrowUp) {
        scrollable_->setScrollOffset(scrollable_->scrollOffset() - 1);
        scrollable_->setStickToBottom(false);
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::ArrowDown) {
        scrollable_->setScrollOffset(scrollable_->scrollOffset() + 1);
        if (scrollable_->totalHeight() - scrollable_->viewportHeight()
            <= scrollable_->scrollOffset()) {
            scrollable_->setStickToBottom(true);
        }
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::PageUp) {
        scrollable_->setScrollOffset(scrollable_->scrollOffset() - scrollable_->viewportHeight());
        scrollable_->setStickToBottom(false);
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::PageDown) {
        scrollable_->setScrollOffset(scrollable_->scrollOffset() + scrollable_->viewportHeight());
        if (scrollable_->totalHeight() - scrollable_->viewportHeight()
            <= scrollable_->scrollOffset()) {
            scrollable_->setStickToBottom(true);
        }
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::Home) {
        scrollable_->setScrollOffset(0);
        scrollable_->setStickToBottom(false);
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::End) {
        // 偏移越界由滚动容器在下次布局时收敛到"内容底部"
        scrollable_->setScrollOffset(scrollable_->totalHeight());
        scrollable_->setStickToBottom(true);
        ctx_.postRedraw();
        return true;
    }
    // 只读弹窗: 其余按键吞掉 (模态, 不得落到被遮挡的主界面)
    return true;
}

// ---------------------------------------------------------------------------
// AboutOverlay
// ---------------------------------------------------------------------------

namespace {

std::string getExecutablePath() noexcept {
#if XX_IS_WIN_D
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return "( Unknown )";
        }
        if (len < buf.size()) {
            buf.resize(len);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return std::filesystem::path(buf).generic_string();
#elif XX_IS_MACOS_D || XX_IS_IOS_D
    uint32_t size = 0;
    (void)::_NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, '\0');
    if (::_NSGetExecutablePath(buf.data(), &size) != 0) {
        return "( Unknown )";
    }
    std::error_code ec;
    auto            exe = std::filesystem::weakly_canonical(std::filesystem::path(buf.data()), ec);
    if (ec) {
        return std::filesystem::path(buf.data()).generic_string();
    }
    return exe.generic_string();
#else
    std::error_code ec;
    auto            exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return "( Unknown )";
    }
    return exe.generic_string();
#endif
}

} // namespace

AboutOverlay::AboutOverlay(TUICtx& ctx) :
    ctx_(ctx) {
    scrollable_ = std::make_shared<Scrollable>([this]() -> std::vector<ScrollItem> {
        return buildItems();
    });
    scrollable_->setStickToBottom(false);
    Add(scrollable_);
}

std::vector<ScrollItem> AboutOverlay::buildItems() {
    const auto& st    = *ctx_.frameState;
    const auto& theme = *ctx_.theme;

    // 1. 内嵌编译的插件列表
    std::vector<std::string> builtinPlugins;
    // get_builtin_plugins 以 uint64_t 计数 (纯 C ABI 定长类型); 此处不可用 size_t:
    // Linux x86_64 上二者同为 unsigned long, 但 macOS/Windows 上 uint64_t 为
    // unsigned long long, 与 size_t (unsigned long) 不同, 传 &size_t 会编译失败
    uint64_t    builtinCount = 0;
    const auto* builtinList  = agentxx::plugin::get_builtin_plugins(&builtinCount);
    if (builtinList && builtinCount > 0) {
        for (size_t i = 0; i < builtinCount; ++i) {
            if (builtinList[i].name.data != nullptr && builtinList[i].name.size > 0) {
                builtinPlugins.emplace_back(builtinList[i].name.data, builtinList[i].name.size);
            }
        }
    }
    std::sort(builtinPlugins.begin(), builtinPlugins.end());
    builtinPlugins.erase(
        std::unique(builtinPlugins.begin(), builtinPlugins.end()),
        builtinPlugins.end()
    );

    // 2. 当前加载的插件列表 (Agent 侧已加载 + Client 侧已加载)
    std::vector<std::string> loadedPlugins;
    for (const auto& notif : st.appendComponents) {
        if (notif.type == agentxx::agent::AppendComponentNotification::Type::Plugin
            && notif.success) {
            loadedPlugins.push_back(notif.name);
        }
    }
    if (auto mgr = ctx_.pluginManager) {
        auto list = mgr->list();
        for (const auto& p : list) {
            if (p.enabled) {
                loadedPlugins.push_back(p.name);
            }
        }
    }
    std::sort(loadedPlugins.begin(), loadedPlugins.end());
    loadedPlugins.erase(
        std::unique(loadedPlugins.begin(), loadedPlugins.end()),
        loadedPlugins.end()
    );

    // 3. 各字段信息
    static const std::string kExePath = getExecutablePath();

    std::string serverIoStr = ctx_.remoteUrl.empty() ? std::string(tr("about.innerServer"))
                                                     : trf("about.remote", ctx_.remoteUrl);

    std::string dataDirStr = ctx_.dataDir;
    if (dataDirStr.empty()) {
        dataDirStr = ctx_.remoteUrl.empty() ? agentxx::agent::AgentConfigStatic::getDataDir("")
                                            : std::string(tr("about.remoteNoCfg"));
    }

    std::string workDirStr = ctx_.workDir;
    if (workDirStr.empty()) {
        workDirStr = agentxx::agent::AgentConfigStatic::getCurrentWorkPath();
    }
    if (workDirStr.empty()) {
        workDirStr = "( Unknown )";
    }

    auto formatList = [](const std::vector<std::string>& list) -> std::string {
        if (list.empty()) {
            return std::string(tr("about.none"));
        }
        std::string res;
        for (size_t i = 0; i < list.size(); ++i) {
            if (i > 0) {
                res += ", ";
            }
            res += list[i];
        }
        return res;
    };

    std::string builtinStr = formatList(builtinPlugins);
    std::string loadedStr  = formatList(loadedPlugins);

    std::vector<ScrollItem> items;

    auto addSection = [&](std::string_view title, const std::string& content) {
        items.push_back(
            ScrollItem{text(fmt::format("• {}", title)) | color(theme.hintColor) | bold, false}
        );
        items.push_back(
            ScrollItem{paragraph(fmt::format("  {}", content)) | color(theme.normalColor), false}
        );
        items.push_back(ScrollItem{text(""), false});
    };

    // Header: Agentxx
    items.push_back(ScrollItem{text("Agentxx") | bold | color(theme.accentColor), false});
    items.push_back(ScrollItem{text(""), false});

    addSection(
        tr("about.version"),
        fmt::format(
            "v{} · {}",
            TUIClientAgentIO::kAgentxxVersion,
            TUIClientAgentIO::kAgentxxBuildDate
        )
    );
    addSection("GitHub · MIT", "https://github.com/coolight7/agentxx");
    addSection(tr("about.develop"), "coolight · 2465045051@qq.com");
    addSection(tr("about.execPath"), kExePath);
    addSection(tr("about.serverIoType"), serverIoStr);
    addSection(tr("about.dataDir"), dataDirStr);
    addSection(tr("about.workDir"), workDirStr);
    addSection(trf("about.builtinPlugins", builtinPlugins.size()), builtinStr);
    addSection(trf("about.loadedPlugins", loadedPlugins.size()), loadedStr);

    return items;
}

Element AboutOverlay::OnRender() {
    const auto& theme    = *ctx_.theme;
    const auto  termSize = ctx_.terminalSize();
    const int   margin   = 2;
    const int   termW    = termSize.dimx;
    const int   termH    = termSize.dimy;
    const int   wantW    = std::max(50, std::min(76, termW * 4 / 5));
    const int   wantH    = std::max(12, std::min(24, termH * 4 / 5));
    const int   availW   = std::max(1, termW - margin * 2);
    const int   availH   = std::max(1, termH - margin * 2);
    const int   popupW   = std::min(wantW, availW);
    const int   popupH   = std::min(wantH, availH);
    const auto  style    = TuiSurfaceStyle::fromTheme(theme);
    return tuiSurfacePopup(style, tr("about.title"), scrollable_->Render() | flex, tr("about.hint"))
           | size(WIDTH, GREATER_THAN, popupW) | size(WIDTH, LESS_THAN, popupW)
           | size(HEIGHT, GREATER_THAN, popupH) | size(HEIGHT, LESS_THAN, popupH);
}

bool AboutOverlay::OnEvent(Event event) {
    if (event == Event::Escape || event == Event::Return) {
        ctx_.postRedraw();
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    if (event.is_mouse()) {
        if (scrollable_->OnEvent(event)) {
            ctx_.postRedraw();
            return true;
        }
        return true;
    }
    if (event == Event::ArrowUp) {
        scrollable_->setScrollOffset(scrollable_->scrollOffset() - 1);
        scrollable_->setStickToBottom(false);
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::ArrowDown) {
        scrollable_->setScrollOffset(scrollable_->scrollOffset() + 1);
        if (scrollable_->totalHeight() - scrollable_->viewportHeight()
            <= scrollable_->scrollOffset()) {
            scrollable_->setStickToBottom(true);
        }
        ctx_.postRedraw();
        return true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// PendingInputsOverlay
// ---------------------------------------------------------------------------

Element PendingInputsOverlay::OnRender() {
    const auto& st    = *ctx_.frameState;
    const auto& theme = *ctx_.theme;

    // 帧首清空命中表: 本帧未渲染出来的按钮 (空列表时没有删除按钮/条目) 不会命中
    hits_.beginFrame();

    const auto surface = TuiSurfaceStyle::fromTheme(theme);

    // 标题栏右侧 "清空" 按钮; 标题栏: 左侧标题文字 (面性风格: 标题栏整体背景色区分)
    Element clearBtn = hits_.add(
        text(fmt::format(" {} ", tr("queue.clear"))) | bgcolor(theme.buttonBgColor)
            | color(theme.buttonTextColor) | bold,
        HitInfo{HitInfo::Kind::Clear, {}}
    );
    Element header = tuiSurfaceHeaderRow(
        hbox({
            text(tr("queue.title")) | bold | color(surface.title),
            filler(),
            std::move(clearBtn),
        }),
        surface.header
    );

    Elements items;
    if (st.pendingInputs.empty()) {
        items.push_back(text(tr("queue.empty")) | theme.dim());
    }
    for (const auto& pi : st.pendingInputs) {
        // 删除按钮登记在条目之前: 命中查询按登记顺序返回首个匹配项, 因此删除优先于
        // 条目本体 (条目命中 = 展开/折叠)
        Element delBtn = hits_.add(
            text(" ✕ ") | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor),
            HitInfo{HitInfo::Kind::Delete, pi.id}
        );
        Element body = pi.expanded ? paragraph(pi.text) | flex
                                   : text(oneLinePreview(pi.text)) | color(theme.userColor) | flex;
        // 多模态排队项: 标题行后缀附件计数, 不展示 Base64 内容
        if (!pi.attachments.empty()) {
            body = hbox({
                std::move(body),
                text(trf("queue.attachCount", pi.attachments.size())) | color(theme.accentColor),
            });
        }
        Element row = pi.expanded ? hbox({
                          text("- ") | color(theme.hintColor),
                          std::move(body),
                          std::move(delBtn),
                      })
                                  : hbox({
                                      text("+ ") | color(theme.userColor),
                                      std::move(body),
                                      std::move(delBtn),
                                  });
        items.push_back(hits_.add(std::move(row), HitInfo{HitInfo::Kind::Item, pi.id}));
    }

    const int maxVisible = std::max(5, ctx_.terminalSize().dimy / 2);
    return tuiSurfaceFrame(
               surface,
               std::move(header),
               vbox(std::move(items)) | yframe | vscroll_indicator
                   | size(HEIGHT, LESS_THAN, maxVisible),
               tuiSurfaceFooterBar(tr("queue.hint"), surface.hint, surface.footer)
           )
           | size(WIDTH, LESS_THAN, 70) | size(WIDTH, GREATER_THAN, 40);
}

bool PendingInputsOverlay::OnEvent(Event event) {
    if (event == Event::Escape) {
        ctx_.postRedraw();
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    if (event.is_mouse() && handleClick(event.mouse())) {
        ctx_.postRedraw();
        return true;
    }
    return true;
}

bool PendingInputsOverlay::handleClick(const Mouse& mouse) {
    const auto* hit = hits_.findClick(mouse);
    if (hit == nullptr) {
        return false;
    }
    const auto& info = hit->payload;

    if (info.kind == HitInfo::Kind::Clear) {
        if (onClear_) {
            onClear_();
        }
        ctx_.state->mutate([](TUIRenderState& st) {
            st.pendingInputs.clear();
        });
        if (onClose_) {
            onClose_();
        }
        return true;
    }

    // 条目: 按条目 id 定位 (列表重建后行下标可能变化)
    ctx_.state->mutate([&](TUIRenderState& st) {
        for (auto it = st.pendingInputs.begin(); it != st.pendingInputs.end(); ++it) {
            if (it->id != info.itemId) {
                continue;
            }
            if (info.kind == HitInfo::Kind::Delete) {
                if (onDeleteItem_) {
                    onDeleteItem_(info.itemId);
                }
                st.pendingInputs.erase(it);
            } else {
                it->expanded = !it->expanded;
            }
            return;
        }
    });
    return true;
}

ftxui::Box PendingInputsOverlay::boxOf(HitInfo::Kind kind, std::string_view itemId) const {
    for (const auto& entry : hits_.entries()) {
        if (entry.payload.kind != kind) {
            continue;
        }
        if (!itemId.empty() && entry.payload.itemId != itemId) {
            continue;
        }
        return *entry.box;
    }
    return agentxx::client::kNoBox;
}

// ---------------------------------------------------------------------------
// ContextOverlay
// ---------------------------------------------------------------------------

namespace {

/// 从消息 JSON 提取 role 字符串 (缺失时返回空串)
std::string ctxMsgRole(const utilxx_base::Json& m) {
    return m.value("role", std::string{});
}

/// 从消息 JSON 提取 tool_calls 名称列表 (缺失/非数组返回空)
/// 用于折叠头预览与展开体摘要行
std::vector<std::string> ctxMsgToolNames(const utilxx_base::Json& m) {
    std::vector<std::string> names;
    if (!m.contains("tool_calls")) {
        return names;
    }
    const auto& tcs = m["tool_calls"];
    if (!tcs.is_array()) {
        return names;
    }
    names.reserve(tcs.size());
    for (const auto& tc : tcs) {
        if (tc.is_object() && tc.contains("name")) {
            names.push_back(tc.value("name", std::string{}));
        }
    }
    return names;
}

/// 消息角色 → 主题颜色 (与消息列表一致)
ftxui::Color ctxRoleColor(const TUITheme& theme, std::string_view role) {
    if (role == "user") {
        return theme.userColor;
    }
    if (role == "system") {
        return theme.systemColor;
    }
    if (role == "tool") {
        return theme.toolColor;
    }
    return theme.assistantColor;
}

} // namespace

std::vector<ScrollItem> ContextOverlay::buildItems() {
    const auto& theme   = *ctx_.theme;
    const auto& msgsPtr = ctx_.frameState->contextMessages;

    // 子项下标 -> 消息下标 映射随本帧子项列表一起重建 (与 Scrollable 保存的
    // visibleBoxes 一一对应, 供鼠标点击命中换算被点击的消息)
    itemMessages_.clear();

    std::vector<ScrollItem> items;
    if (!msgsPtr || !msgsPtr->is_array() || msgsPtr->empty()) {
        itemMessages_.push_back(kNoMessage);
        items.push_back(ScrollItem{text(tr("ctx.empty")) | theme.dim(), true});
        return items;
    }

    const auto& msgs = *msgsPtr;
    items.reserve(msgs.size() * 2 + 1);
    for (size_t i = 0; i < msgs.size(); ++i) {
        const auto& m         = msgs[i];
        const auto  role      = ctxMsgRole(m);
        const Color roleColor = ctxRoleColor(theme, role);
        const bool  expanded  = expandedSet_.contains(i);

        itemMessages_.push_back(i);
        items.push_back(ScrollItem{buildMessageHeader(m, expanded, roleColor), false});
        if (expanded) {
            itemMessages_.push_back(kNoMessage);
            items.push_back(ScrollItem{buildMessageBody(m), false});
        }
    }
    return items;
}

std::vector<ftxui::Box> ContextOverlay::headerBoxes() const {
    // 由 Scrollable 上一帧的可见子项区域反推各消息折叠头的屏幕区域 (载荷 =
    // 消息下标): 未被布局的子项 (视口外) 区域为空, 点击不会误命中
    const auto&             msgsPtr = ctx_.frameState->contextMessages;
    const size_t            nMsgs   = (msgsPtr && msgsPtr->is_array()) ? msgsPtr->size() : 0;
    std::vector<ftxui::Box> boxes(nMsgs, agentxx::client::kNoBox);

    const auto&  vboxes = scrollable_->visibleBoxes();
    const size_t n      = std::min(vboxes.size(), itemMessages_.size());
    for (size_t i = 0; i < n; ++i) {
        const size_t msgIndex = itemMessages_[i];
        if (msgIndex < boxes.size() && !vboxes[i].IsEmpty()) {
            boxes[msgIndex] = vboxes[i];
        }
    }
    return boxes;
}

size_t ContextOverlay::headerMessageAt(int x, int y) const {
    // 命中判定用 Scrollable 上一帧的可见子项区域 (与用户当前看到的屏幕内容
    // 一致; 本组件每帧重建子项元素, 该区域与本帧子项列表同源)。
    //
    // 不能用子项元素自带的 reflect 命中框: Scrollable 测量子项高度时以
    // "测量用临时大框" (局部坐标: x = 0..内容宽, y = 0..很大) 调用 SetBox,
    // 之后只有视口内的子项会被重新定位到真实屏幕坐标 —— 视口外 (上方/下方)
    // 的子项残留测量大框, 该框的局部坐标与弹窗屏幕坐标部分重叠, 于是点击会
    // 命中到看不见的消息 (命中的是消息列表中靠前的那条, 与长消息滚动后尤为
    // 明显)。可见子项区域仅在子项真正被定位时写入, 视口外恒为空, 无此问题。
    const auto&  boxes = scrollable_->visibleBoxes();
    const size_t n     = std::min(boxes.size(), itemMessages_.size());
    for (size_t i = 0; i < n; ++i) {
        if (itemMessages_[i] == kNoMessage || boxes[i].IsEmpty()) {
            continue;
        }
        if (boxes[i].Contain(x, y)) {
            return itemMessages_[i];
        }
    }
    return kNoMessage;
}

size_t ContextOverlay::firstVisibleHeaderMessage() const {
    // 子项按列表顺序 (自上而下) 排列, 首个非空区域即视口内最上方的折叠头
    const auto&  boxes = scrollable_->visibleBoxes();
    const size_t n     = std::min(boxes.size(), itemMessages_.size());
    for (size_t i = 0; i < n; ++i) {
        if (itemMessages_[i] != kNoMessage && !boxes[i].IsEmpty()) {
            return itemMessages_[i];
        }
    }
    return kNoMessage;
}

ftxui::Element ContextOverlay::buildMessageHeader(
    const utilxx_base::Json& m,
    bool                     expanded,
    const Color&             roleColor
) {
    const auto& theme     = *ctx_.theme;
    const auto  role      = ctxMsgRole(m);
    const int   maxW      = std::max(1, scrollable_->contentWidth());
    const auto  toolNames = ctxMsgToolNames(m);

    // 前缀列数: "+ " + "[role] " + 可选 "tool_calls: N " 标记 (窄屏时
    // 预览预算保底, 超出部分由 xflex_shrink 右缘裁剪兜底)
    int prefixCols = 3 + static_cast<int>(markdown::utf8_display_width(role));
    if (!toolNames.empty()) {
        prefixCols += 12; // "tool_calls: N " 粗估列数
    }
    const int budget = collapsedPreviewBudget(maxW, prefixCols);

    std::string preview;
    if (!toolNames.empty()) {
        // tool_calls 消息: 预览工具名列表 (折叠/展开头均显示, 便于快速定位)
        std::string names;
        for (size_t k = 0; k < toolNames.size(); ++k) {
            if (k > 0) {
                names += ", ";
            }
            names += toolNames[k];
        }
        preview = fmt::format("tool_calls: {}", names);
    } else {
        preview = oneLinePreview(m.value("content", std::string{}), static_cast<size_t>(budget));
    }

    Element head = hbox({
        text(expanded ? "- " : "+ ") | color(theme.hintColor),
        text(fmt::format("[{}] ", role)) | color(roleColor) | bold,
        text(preview) | color(theme.normalColor) | xflex_shrink,
    });
    // 折叠头整行可点 (命中区域由事件处理时按 Scrollable 可见子项区域换算:
    // 见 headerMessageAt —— 此处不附加 reflect 命中框)
    return head;
}

ftxui::Element ContextOverlay::buildMessageBody(const utilxx_base::Json& m) {
    const auto& theme = *ctx_.theme;

    // 摘要行: 完整字段清单 (role + content 长度 + tool_calls 数 + 其余字段),
    // 便于不展开也能了解该消息的结构
    std::string summary = " ";
    {
        const auto toolNames = ctxMsgToolNames(m);
        const auto content   = m.value("content", std::string{});
        summary += fmt::format("content[{}] tool_calls[{}]", content.size(), toolNames.size());
        std::vector<std::string> extra;
        for (const auto& kv : m.items()) {
            const auto& k = kv.first;
            if (k == "role" || k == "content" || k == "tool_calls") {
                continue;
            }
            extra.push_back(k);
        }
        if (!extra.empty()) {
            std::string joined;
            for (size_t k = 0; k < extra.size(); ++k) {
                if (k > 0) {
                    joined += ',';
                }
                joined += extra[k];
            }
            summary += " +" + joined;
        }
    }

    // 完整、原始的 JSON 展示 (美化 2 空格缩进; dump 失败时降级为原始文本)
    std::string jsonText;
    try {
        jsonText = m.dump(2);
    } catch (...) {
        jsonText = m.value("content", std::string{});
    }

    return vbox({
        text(summary) | color(theme.hintColor),
        paragraph(jsonText) | color(theme.normalColor),
    });
}

Element ContextOverlay::OnRender() {
    const auto& theme    = *ctx_.theme;
    const auto& msgsPtr  = ctx_.frameState->contextMessages;
    const auto  termSize = ctx_.terminalSize();

    const int margin = 2;
    const int termW  = termSize.dimx;
    const int termH  = termSize.dimy;
    const int wantW  = std::max(60, termW * 4 / 5);
    const int wantH  = std::max(14, termH * 4 / 5);
    const int availW = std::max(1, termW - margin * 2);
    const int availH = std::max(1, termH - margin * 2);
    const int popupW = std::min(wantW, availW);
    const int popupH = std::min(wantH, availH);

    auto title = trf("ctx.title", (msgsPtr && msgsPtr->is_array()) ? msgsPtr->size() : 0);

    // 先渲染滚动区, 再返回整体布局; 折叠头命中区域由事件处理时
    // 从 scrollable_->visibleBoxes() 实时反推 (见 headerBoxes/handleHeaderClick)
    Element body = scrollable_->Render() | flex;

    const auto style = TuiSurfaceStyle::fromTheme(theme);
    return tuiSurfacePopup(style, title, std::move(body), tr("ctx.hint"))
           | size(WIDTH, GREATER_THAN, popupW) | size(WIDTH, LESS_THAN, popupW)
           | size(HEIGHT, GREATER_THAN, popupH) | size(HEIGHT, LESS_THAN, popupH);
}

bool ContextOverlay::OnEvent(Event event) {
    if (event == Event::Escape) {
        ctx_.state->mutate([](TUIRenderState& st) {
            st.showContextOverlay = false;
        });
        ctx_.postRedraw();
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    if (event.is_mouse()) {
        if (handleHeaderClick(event.mouse())) {
            ctx_.postRedraw();
            return true;
        }
        // 滚轮滚动 (Scrollable 内部处理)
        if (scrollable_->OnEvent(event)) {
            ctx_.postRedraw();
            return true;
        }
        return true;
    }
    // 键盘滚动 / 折叠切换 (与 MermaidDiagramOverlay 等弹窗交互一致)
    if (event == Event::ArrowUp) {
        scrollable_->setScrollOffset(scrollable_->scrollOffset() - 1);
        scrollable_->setStickToBottom(false);
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::ArrowDown) {
        scrollable_->setScrollOffset(scrollable_->scrollOffset() + 1);
        if (scrollable_->totalHeight() - scrollable_->viewportHeight()
            <= scrollable_->scrollOffset()) {
            scrollable_->setStickToBottom(true);
        }
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::PageUp) {
        scrollable_->setScrollOffset(scrollable_->scrollOffset() - scrollable_->viewportHeight());
        scrollable_->setStickToBottom(false);
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::PageDown) {
        scrollable_->setScrollOffset(scrollable_->scrollOffset() + scrollable_->viewportHeight());
        if (scrollable_->totalHeight() - scrollable_->viewportHeight()
            <= scrollable_->scrollOffset()) {
            scrollable_->setStickToBottom(true);
        }
        ctx_.postRedraw();
        return true;
    }
    // Enter / Space: 切换视口内首个可见消息的折叠状态
    if (event == Event::Return || event == Event::Character(" ")) {
        const size_t index = firstVisibleHeaderMessage();
        if (index != kNoMessage) {
            toggleExpanded(index);
            ctx_.postRedraw();
        }
        return true;
    }
    return true;
}

bool ContextOverlay::handleHeaderClick(const Mouse& mouse) {
    // 一次点击以左键释放为准; 命中折叠头 -> 切换该消息的折叠状态
    // (视口外折叠头的可见区域为空, 不会命中)
    if (mouse.button != Mouse::Left || mouse.motion != Mouse::Released) {
        return false;
    }
    const size_t index = headerMessageAt(mouse.x, mouse.y);
    if (index == kNoMessage) {
        return false;
    }
    toggleExpanded(index);
    return true;
}

void ContextOverlay::toggleExpanded(size_t index) {
    if (expandedSet_.contains(index)) {
        expandedSet_.erase(index);
    } else {
        expandedSet_.insert(index);
    }
}

// ---------------------------------------------------------------------------
// MermaidDiagramOverlay
// ---------------------------------------------------------------------------

MermaidDiagramOverlay::MermaidDiagramOverlay(TUICtx& ctx, std::string mermaid, std::string title) :
    ctx_(ctx),
    mermaid_(std::move(mermaid)),
    title_(std::move(title)) {
    scrollable_ = std::make_shared<Scrollable>([this]() -> std::vector<ScrollItem> {
        return buildItems();
    });
    scrollable_->setStickToBottom(false);
    Add(scrollable_);
}

std::vector<ScrollItem> MermaidDiagramOverlay::buildItems() {
    const auto& theme = *ctx_.theme;
    const int   maxW  = std::max(40, ctx_.terminalSize().dimx - 10);
    if (cachedMermaid_ != mermaid_ || cachedMaxW_ != maxW || cachedThemeName_ != theme.name) {
        cachedMermaid_   = mermaid_;
        cachedMaxW_      = maxW;
        cachedThemeName_ = theme.name;
        cachedDiagram_   = markdown::parseMermaidStateDiagram(mermaid_);
        if (!cachedDiagram_.nodes.empty()) {
            cachedElement_ = markdown::renderMermaidStateDiagram(
                cachedDiagram_,
                maxW,
                theme.normalColor,
                markdown::diagramNodeColor(theme.markdownTheme)
            );
        } else {
            cachedElement_ = nullptr;
        }
    }
    if (!cachedElement_) {
        return {
            ScrollItem{ftxui::text(tr("graph.noDiagram")) | theme.dim(), false}
        };
    }
    return {
        ScrollItem{cachedElement_, false}
    };
}

ftxui::Element MermaidDiagramOverlay::OnRender() {
    const auto& theme = *ctx_.theme;
    // 标题: 插件自定义优先, 空则回退通用翻译
    const std::string titleText = title_.empty() ? std::string(tr("graph.title")) : title_;
    const int         margin    = 2;
    const auto        termSize  = ctx_.terminalSize();
    const int         termW     = termSize.dimx;
    const int         termH     = termSize.dimy;
    const int         wantW     = std::max(40, termW * 4 / 5);
    const int         wantH     = std::max(14, termH * 4 / 5);
    const int         availW    = std::max(1, termW - margin * 2);
    const int         availH    = std::max(1, termH - margin * 2);
    const int         popupW    = std::min(wantW, availW);
    const int         popupH    = std::min(wantH, availH);
    const auto        style     = TuiSurfaceStyle::fromTheme(theme);
    return tuiSurfacePopup(
               style,
               titleText,
               scrollable_->Render() | ftxui::flex,
               tr("overlay.scrollHint")
           )
           | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, popupW)
           | ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, popupW)
           | ftxui::size(ftxui::HEIGHT, ftxui::GREATER_THAN, popupH)
           | ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, popupH);
}

bool MermaidDiagramOverlay::OnEvent(ftxui::Event event) {
    if (event == ftxui::Event::Escape) {
        ctx_.postRedraw();
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    if (event.is_mouse()) {
        if (scrollable_->OnEvent(event)) {
            ctx_.postRedraw();
            return true;
        }
        return true;
    }
    if (event == ftxui::Event::ArrowUp) {
        scrollable_->setScrollOffset(scrollable_->scrollOffset() - 1);
        scrollable_->setStickToBottom(false);
        ctx_.postRedraw();
        return true;
    }
    if (event == ftxui::Event::ArrowDown) {
        scrollable_->setScrollOffset(scrollable_->scrollOffset() + 1);
        if (scrollable_->totalHeight() - scrollable_->viewportHeight()
            <= scrollable_->scrollOffset()) {
            scrollable_->setStickToBottom(true);
        }
        ctx_.postRedraw();
        return true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// FailedComponentsOverlay
// ---------------------------------------------------------------------------

namespace {

/// AppendComponentNotification 类型 → 展示标签
/// MCP/Skill/Memory/Plugin 属技术名词, 不翻译; 未知类型回退翻译文本
std::string appendTypeLabel(agentxx::agent::AppendComponentNotification::Type type) {
    using T = agentxx::agent::AppendComponentNotification;
    switch (type) {
        case T::Type::Mcp:
            return "MCP";
        case T::Type::Skill:
            return "Skill";
        case T::Type::Memory:
            return "Memory";
        case T::Type::Plugin:
            return "Plugin";
    }
    return std::string(tr("failed.unknownType"));
}

} // namespace

FailedComponentsOverlay::FailedComponentsOverlay(TUICtx& ctx) :
    ctx_(ctx) {
    scrollable_ = std::make_shared<Scrollable>([this]() -> std::vector<ScrollItem> {
        return buildItems();
    });
    // 失败列表为静态内容: 打开时从顶部开始显示, 而非吸附到底部
    scrollable_->setStickToBottom(false);
    Add(scrollable_);
}

std::vector<ScrollItem> FailedComponentsOverlay::buildItems() {
    const auto& st    = *ctx_.frameState;
    const auto& theme = *ctx_.theme;

    // 每帧从本帧状态快照过滤 success=false 项 (数量小, 无需缓存)
    std::vector<ScrollItem> items;
    bool                    first = true;
    for (const auto& notif : st.appendComponents) {
        if (notif.success) {
            continue;
        }
        if (!first) {
            items.push_back(ScrollItem{text(""), false});
        }
        first = false;
        // 标题行: "[类型] 名称" (错误色类型标签)
        items.push_back(ScrollItem{
            hbox({
                text(fmt::format("[{}] ", appendTypeLabel(notif.type))) | color(theme.errorColor),
                text(notif.name) | color(theme.normalColor) | xflex_shrink,
            }),
            false
        });
        // 错误信息行 (自动换行; 空消息跳过)
        if (!notif.errorMessage.empty()) {
            items.push_back(
                ScrollItem{paragraph(notif.errorMessage) | color(theme.hintColor), false}
            );
        }
    }
    if (items.empty()) {
        items.push_back(ScrollItem{text(tr("failed.empty")) | theme.dim(), false});
    }
    return items;
}

Element FailedComponentsOverlay::OnRender() {
    const auto& theme = *ctx_.theme;

    // 弹窗大小: 宽 3/5 屏、高 2/5 屏, 不超过窗口可用空间 (减去边距);
    // 高度同时给 GREATER_THAN 下限, 避免惰性 viewport 自然高度塌缩成单行
    // (原因详见下方弹窗 OnRender 注释)
    const auto termSize = ctx_.terminalSize();
    const int  margin   = 2;
    const int  termW    = termSize.dimx;
    const int  termH    = termSize.dimy;
    const int  wantW    = std::max(40, termW * 3 / 5);
    const int  wantH    = std::max(10, termH * 2 / 5);
    const int  availW   = std::max(1, termW - margin * 2);
    const int  availH   = std::max(1, termH - margin * 2);
    const int  popupW   = std::min(wantW, availW);
    const int  popupH   = std::min(wantH, availH);
    // 错误类弹窗: 标题栏用偏红背景 + 错误色标题文字 (替代原错误色边框)
    auto style   = TuiSurfaceStyle::fromTheme(theme);
    style.header = theme.surfaceErrorHeaderColor;
    style.title  = theme.errorColor;
    return tuiSurfacePopup(
               style,
               tr("failed.title"),
               scrollable_->Render() | flex,
               tr("overlay.scrollHint")
           )
           | size(WIDTH, GREATER_THAN, popupW) | size(WIDTH, LESS_THAN, popupW)
           | size(HEIGHT, GREATER_THAN, popupH) | size(HEIGHT, LESS_THAN, popupH);
}

bool FailedComponentsOverlay::OnEvent(Event event) {
    if (event == Event::Escape) {
        ctx_.postRedraw();
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    if (event.is_mouse()) {
        // 滚轮滚动 (Scrollable 内部处理)
        if (scrollable_->OnEvent(event)) {
            ctx_.postRedraw();
            return true;
        }
        return true;
    }
    // 键盘滚动 (与 ContextOverlay 等弹窗交互一致)
    if (event == Event::ArrowUp) {
        scrollable_->setScrollOffset(scrollable_->scrollOffset() - 1);
        scrollable_->setStickToBottom(false);
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::ArrowDown) {
        scrollable_->setScrollOffset(scrollable_->scrollOffset() + 1);
        // 滚到底部恢复吸附
        if (scrollable_->totalHeight() - scrollable_->viewportHeight()
            <= scrollable_->scrollOffset()) {
            scrollable_->setStickToBottom(true);
        }
        ctx_.postRedraw();
        return true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 通用 overlay: Text / Diff / Custom (open_overlay 驱动, 零插件特化)
// ---------------------------------------------------------------------------

namespace {

/// 通用 overlay 弹窗尺寸: 宽/高按屏占比, 不超过可用空间, 双约束防塌缩
/// (抄 Mermaid/Failed: 惰性 viewport 自然高度会塌缩成单行, 必须同时给
/// GREATER_THAN 下限)
void overlayPopupSize(
    const TUICtx& ctx,
    double        widthFrac,
    double        heightFrac,
    int&          popupW,
    int&          popupH
) {
    const auto termSize = ctx.terminalSize();
    const int  termW    = termSize.dimx;
    const int  termH    = termSize.dimy;
    const int  wantW    = std::max(40, static_cast<int>(static_cast<double>(termW) * widthFrac));
    const int  wantH    = std::max(10, static_cast<int>(static_cast<double>(termH) * heightFrac));
    const int  availW   = std::max(1, termW - 4);
    const int  availH   = std::max(1, termH - 4);
    popupW              = std::min(wantW, availW);
    popupH              = std::min(wantH, availH);
}

bool overlayScrollByKey(TUICtx& ctx, const std::shared_ptr<Scrollable>& scrollable, Event event) {
    if (event == Event::ArrowUp) {
        scrollable->setScrollOffset(scrollable->scrollOffset() - 1);
        scrollable->setStickToBottom(false);
        ctx.postRedraw();
        return true;
    }
    if (event == Event::ArrowDown) {
        scrollable->setScrollOffset(scrollable->scrollOffset() + 1);
        if (scrollable->totalHeight() - scrollable->viewportHeight()
            <= scrollable->scrollOffset()) {
            scrollable->setStickToBottom(true);
        }
        ctx.postRedraw();
        return true;
    }
    return false;
}

Element overlayFrame(
    TUICtx&               ctx,
    const TUITheme&       theme,
    const std::string&    title,
    Scrollable&           scrollable,
    const OverlayOptions& options = {}
) {
    double wFrac = 0.6, hFrac = 0.8;
    options.resolveFractions(wFrac, hFrac);
    int popupW = 0, popupH = 0;
    overlayPopupSize(ctx, wFrac, hFrac, popupW, popupH);
    const auto style = TuiSurfaceStyle::fromTheme(theme);
    // 底栏: 选项关闭时不显示提示 (面性风格下底栏是独立分区, 无提示则不渲染)
    const std::string footer
        = options.footer ? std::string{tr("overlay.scrollHint")} : std::string{};
    return tuiSurfacePopup(style, title, scrollable.Render() | flex, footer)
           | size(WIDTH, GREATER_THAN, popupW) | size(WIDTH, LESS_THAN, popupW)
           | size(HEIGHT, GREATER_THAN, popupH) | size(HEIGHT, LESS_THAN, popupH);
}

} // namespace

OverlayOptions OverlayOptions::fromJson(std::string_view extraJson) {
    OverlayOptions out;
    if (extraJson.empty() || extraJson == "{}") {
        return out;
    }
    try {
        auto j = utilxx_base::Json::parse(extraJson);
        if (!j.is_object()) {
            return out;
        }
        out.size       = j.value("size", out.size);
        out.widthFrac  = j.value("width_frac", out.widthFrac);
        out.heightFrac = j.value("height_frac", out.heightFrac);
        out.footer     = j.value("footer", out.footer);
        out.stack      = j.value("stack", out.stack);
        // scroll=false 视为"内容不需要滚动": 隐藏滚动提示 (与 footer=false 等效)
        if (j.contains("scroll") && j["scroll"].is_boolean() && !j["scroll"].get<bool>()) {
            out.footer = false;
        }
    } catch (...) {
        return OverlayOptions{};
    }
    return out;
}

void OverlayOptions::resolveFractions(double& wFrac, double& hFrac) const {
    // 预设比例: 紧凑 / 常规 / 大 / 全屏 (auto 与常规一致, 宽度由内容自适应)
    double presetW = 0.6;
    double presetH = 0.8;
    if (size == "compact") {
        presetW = 0.4;
        presetH = 0.5;
    } else if (size == "large") {
        presetW = 0.8;
        presetH = 0.8;
    } else if (size == "full") {
        presetW = 1.0;
        presetH = 1.0;
    }
    wFrac = (widthFrac > 0.0 && widthFrac <= 1.0) ? widthFrac : presetW;
    hFrac = (heightFrac > 0.0 && heightFrac <= 1.0) ? heightFrac : presetH;
}

TextOverlay::TextOverlay(TUICtx& ctx, std::string title, std::string content, bool markdown) :
    ctx_(ctx),
    title_(std::move(title)),
    content_(std::move(content)),
    markdown_(markdown) {
    scrollable_ = std::make_shared<Scrollable>([this]() -> std::vector<ScrollItem> {
        return buildItems();
    });
    scrollable_->setStickToBottom(false);
    Add(scrollable_);
}

std::vector<ScrollItem> TextOverlay::buildItems() {
    const auto& theme = *ctx_.theme;
    const int   maxW  = std::max(40, ctx_.terminalSize().dimx - 10);
    if (cachedContent_ != content_ || cachedMaxW_ != maxW || cachedThemeName_ != theme.name
        || cachedMarkdown_ != markdown_) {
        cachedContent_   = content_;
        cachedMaxW_      = maxW;
        cachedThemeName_ = theme.name;
        cachedMarkdown_  = markdown_;
        cachedAttachments_.clear();
        if (content_.empty()) {
            cachedElement_ = text(tr("info.empty")) | theme.dim();
        } else if (markdown_) {
            auto parser  = markdown::make_cmark_parser();
            auto ast     = parser->parse(content_);
            auto builder = std::make_shared<markdown::DomBuilder>();
            builder->set_max_width(maxW);
            cachedElement_
                = builder->build(ast, -1, theme.markdownTheme) | color(theme.normalColor);
            cachedAttachments_.push_back(std::move(builder));
        } else {
            cachedElement_ = paragraph(content_) | color(theme.normalColor);
        }
    }
    return {
        ScrollItem{cachedElement_, false}
    };
}

Element TextOverlay::OnRender() {
    return overlayFrame(ctx_, *ctx_.theme, title_, *scrollable_, options_);
}

bool TextOverlay::OnEvent(Event event) {
    if (event == Event::Escape) {
        ctx_.postRedraw();
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    if (event.is_mouse()) {
        if (scrollable_->OnEvent(event)) {
            ctx_.postRedraw();
            return true;
        }
        return true;
    }
    if (overlayScrollByKey(ctx_, scrollable_, event)) {
        return true;
    }
    return true;
}

std::shared_ptr<ftxui::ComponentBase> createUniversalOverlay(
    TUICtx&               ctx,
    int                   type,
    std::string_view      title,
    std::string_view      payload,
    std::string_view      extraJson,
    std::string_view      ownerPlugin,
    std::function<void()> onClose
) {
    const OverlayOptions options = OverlayOptions::fromJson(extraJson);
    switch (type) {
        case AGENTXX_OVERLAY_MERMAID: {
            if (payload.empty()) {
                return nullptr;
            }
            auto m = std::make_shared<MermaidDiagramOverlay>(
                ctx,
                std::string(payload),
                std::string(title)
            );
            m->onClose(std::move(onClose));
            return m;
        }
        case AGENTXX_OVERLAY_TEXT: {
            bool markdown = true;
            try {
                if (!extraJson.empty() && extraJson != "{}") {
                    auto extra = utilxx_base::Json::parse(extraJson);
                    if (extra.is_object() && extra.contains("markdown")
                        && extra["markdown"].is_boolean()) {
                        markdown = extra["markdown"].get<bool>();
                    }
                }
            } catch (...) {
            }
            auto m = std::make_shared<TextOverlay>(
                ctx,
                std::string(title),
                std::string(payload),
                markdown
            );
            m->setOptions(options);
            m->onClose(std::move(onClose));
            return m;
        }
        case AGENTXX_OVERLAY_DIFF: {
            std::string path, oldStr, newStr;
            try {
                auto j = utilxx_base::Json::parse(payload.empty() ? "{}" : payload);
                path   = j.value("path", std::string{});
                oldStr = j.value("old_str", std::string{});
                newStr = j.value("new_str", std::string{});
            } catch (...) {
                return nullptr;
            }
            auto m = std::make_shared<DiffOverlay>(
                ctx,
                std::string(title),
                std::move(path),
                std::move(oldStr),
                std::move(newStr)
            );
            m->setOptions(options);
            m->onClose(std::move(onClose));
            return m;
        }
        case AGENTXX_OVERLAY_CUSTOM: {
            utilxx_base::Json items = utilxx_base::Json::array();
            try {
                auto j = utilxx_base::Json::parse(payload.empty() ? "{}" : payload);
                if (j.is_object() && j.contains("items") && j["items"].is_array()) {
                    items = j["items"];
                } else if (j.is_array()) {
                    items = std::move(j);
                }
            } catch (...) {
                return nullptr;
            }
            auto m = std::make_shared<CustomOverlay>(
                ctx,
                std::string(title),
                std::move(items),
                std::string(ownerPlugin)
            );
            m->setOptions(options);
            m->onClose(std::move(onClose));
            return m;
        }
        default:
            return nullptr;
    }
}

DiffOverlay::DiffOverlay(
    TUICtx&     ctx,
    std::string title,
    std::string path,
    std::string oldStr,
    std::string newStr
) :
    ctx_(ctx),
    title_(std::move(title)),
    path_(std::move(path)),
    oldStr_(std::move(oldStr)),
    newStr_(std::move(newStr)) {
    scrollable_ = std::make_shared<Scrollable>([this]() -> std::vector<ScrollItem> {
        return buildItems();
    });
    scrollable_->setStickToBottom(false);
    Add(scrollable_);
}

std::vector<ScrollItem> DiffOverlay::buildItems() {
    const auto& theme = *ctx_.theme;
    return {
        ScrollItem{agentxx::client::renderPluginDiff(path_, oldStr_, newStr_, theme), false}
    };
}

Element DiffOverlay::OnRender() {
    return overlayFrame(ctx_, *ctx_.theme, title_, *scrollable_, options_);
}

bool DiffOverlay::OnEvent(Event event) {
    if (event == Event::Escape) {
        ctx_.postRedraw();
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    if (event.is_mouse()) {
        if (scrollable_->OnEvent(event)) {
            ctx_.postRedraw();
            return true;
        }
        return true;
    }
    if (overlayScrollByKey(ctx_, scrollable_, event)) {
        return true;
    }
    return true;
}

CustomOverlay::CustomOverlay(
    TUICtx&           ctx,
    std::string       title,
    utilxx_base::Json items,
    std::string       ownerPlugin
) :
    ctx_(ctx),
    title_(std::move(title)),
    items_(std::move(items)),
    ownerPlugin_(std::move(ownerPlugin)) {
    scrollable_ = std::make_shared<Scrollable>([this]() -> std::vector<ScrollItem> {
        // CUSTOM overlay 内容走共享组件层 (与面板/Info/装饰/中断同一实现):
        // 每个子项携带可命中区域 (局部坐标), 点击经 Scrollable::hitTestItem
        // 定位子项后按区域派发 (滚动到视口外的子项不占点击区域)
        const auto& theme = *ctx_.theme;
        auto        reg   = ctx_.pluginManager ? ctx_.pluginManager->uiRegistrySnapshot() : nullptr;
        const auto* regPtr = reg.get();
        // 点击派发时复查实例代次: overlay 打开后插件被重载, 旧按钮不得转交新实例
        if (regPtr) {
            ownerGeneration_ = regPtr->generationOf(ownerPlugin_);
        }

        agentxx::client::UiRenderCtx rc;
        rc.theme            = &theme;
        rc.width            = ctx_.terminalSize().dimx - 8;
        rc.indent           = 0;
        rc.plugin           = ownerPlugin_;
        rc.ownerId          = std::string{AGENTXX_CLIENT_OVERLAY_OWNER};
        rc.registry         = regPtr;
        rc.separatorStyle   = agentxx::client::UiSeparatorStyle::Block;
        rc.collapseExpanded = [this](const std::string& id, bool defaultValue) {
            auto it = collapseStates_.find(id);
            if (it != collapseStates_.end()) {
                return it->second;
            }
            collapseStates_.emplace(id, defaultValue);
            return defaultValue;
        };
        // 表单状态: 按最新描述初始化 (保留用户已编辑的值), 控件交互由本组件处理
        formItems_ = agentxx::ui::parseItemList(items_);
        agentxx::client::initFormState(form_, formItems_);
        rc.form = &form_;

        UiRenderResult res;
        agentxx::client::renderItems(formItems_, rc, res);
        if (!res.builders.empty()) {
            mdBuilders_ = std::move(res.builders);
        }
        std::vector<ScrollItem> out;
        out.reserve(res.rows.size() + 1);
        for (auto& row : res.rows) {
            ScrollItem item;
            item.element = std::move(row.element);
            item.hits    = std::move(row.regions);
            out.push_back(std::move(item));
        }
        if (out.empty()) {
            out.push_back(ScrollItem{text(tr("info.empty")) | color(theme.hintColor), false});
        }
        return out;
    });
    scrollable_->setStickToBottom(false);
    Add(scrollable_);
}

Element CustomOverlay::OnRender() {
    auto el = overlayFrame(ctx_, *ctx_.theme, title_, *scrollable_, options_);
    // 上报 overlay 区域尺寸 (区域 id 固定 `__overlay`): 插件按可用宽高重排内容,
    // 也作为 `pause_when_hidden` 定时器的门控依据 (可见性由打开/关闭时上报)
    if (auto mgr = ctx_.pluginManager) {
        mgr->reportRegionSize(
            std::string{AGENTXX_CLIENT_OVERLAY_OWNER},
            scrollable_->contentWidth(),
            scrollable_->totalHeight()
        );
    }
    return el;
}

bool CustomOverlay::OnEvent(Event event) {
    if (event == Event::Escape) {
        ctx_.postRedraw();
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    if (event.is_mouse()) {
        const auto& mouse = event.mouse();
        // overlay 局部命中: 先经滚动容器把坐标映射到子项与子项内局部坐标,
        // 再按子项登记的区域分类处理 (折叠标题切换展开状态; 动作区域派发)
        // owner 固定 "__overlay", 被实例级动作绑定接住
        size_t index  = 0;
        int    localX = 0;
        int    localY = 0;
        if (scrollable_->hitTestItem(mouse.x, mouse.y, index, localX, localY)) {
            const auto& rows = scrollable_->items();
            if (index < rows.size()) {
                const auto* region = matchUiHitRegion(rows[index].hits, localX, localY);
                const bool  clicked
                    = (mouse.button == Mouse::Left && mouse.motion == Mouse::Released);
                if (region != nullptr && region->kind == UiHitRegionKind::Collapse && clicked) {
                    bool current = true;
                    if (auto it = collapseStates_.find(region->id); it != collapseStates_.end()) {
                        current = it->second;
                    }
                    collapseStates_[region->id] = !current;
                    ctx_.postRedraw();
                    return true;
                }
                if (region != nullptr && region->kind == UiHitRegionKind::Form && clicked) {
                    const auto action = agentxx::client::handleFormControlHit(
                        formItems_,
                        form_,
                        region->id,
                        region->sub
                    );
                    if (action != agentxx::client::UiFormAction::None) {
                        if (action == agentxx::client::UiFormAction::Submit) {
                            submitForm();
                        }
                        ctx_.postRedraw();
                        return true;
                    }
                }
                if (region != nullptr && region->kind == UiHitRegionKind::FormSubmit && clicked) {
                    const auto action = agentxx::client::handleFormSubmitHit(region->id);
                    if (action == agentxx::client::UiFormAction::Submit) {
                        submitForm();
                    } else if (action == agentxx::client::UiFormAction::Cancel) {
                        if (auto mgr = ctx_.pluginManager) {
                            mgr->dispatchAction(
                                ownerPlugin_,
                                AGENTXX_CLIENT_OVERLAY_OWNER,
                                std::string{agentxx::client::kFormCancelActionId},
                                "{}",
                                ownerGeneration_
                            );
                        }
                    }
                    ctx_.postRedraw();
                    return true;
                }
                if (region != nullptr && region->kind == UiHitRegionKind::Action && clicked) {
                    if (auto mgr = ctx_.pluginManager) {
                        mgr->dispatchAction(
                            ownerPlugin_,
                            AGENTXX_CLIENT_OVERLAY_OWNER,
                            region->id,
                            region->arg,
                            ownerGeneration_
                        );
                    }
                    ctx_.postRedraw();
                    return true;
                }
            }
        }
        if (scrollable_->OnEvent(event)) {
            ctx_.postRedraw();
            return true;
        }
        return true;
    }
    // 表单键盘输入 (有焦点时字符键进入控件)
    if (!form_.focusedId.empty()) {
        if (event == Event::Return) {
            submitForm();
            ctx_.postRedraw();
            return true;
        }
        if (agentxx::client::handleFormKeyInput(formItems_, form_, event)) {
            ctx_.postRedraw();
            return true;
        }
    }
    if (overlayScrollByKey(ctx_, scrollable_, event)) {
        return true;
    }
    return true;
}

void CustomOverlay::submitForm() {
    if (!agentxx::client::validateForm(formItems_, form_)) {
        return;
    }
    if (auto mgr = ctx_.pluginManager) {
        const std::string values = agentxx::client::formValues(formItems_, form_).dump();
        mgr->dispatchAction(
            ownerPlugin_,
            AGENTXX_CLIENT_OVERLAY_OWNER,
            std::string{agentxx::client::kFormSubmitActionId},
            values,
            ownerGeneration_
        );
    }
}

} // namespace agentxx::client
