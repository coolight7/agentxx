#include "agentxx-client/io/tui/components/overlays.h"
#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx/agent/config_static.h"
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/util/exception.h"
#include "ftxui/component/component.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/terminal.hpp"
#include <algorithm>
#include <filesystem>
#include <markdown/state_diagram.hpp>
#include <markdown/text_utils.hpp>

#if XX_IS_WIN_D
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace ftxui;

// ---------------------------------------------------------------------------
// 工具函数
// ---------------------------------------------------------------------------

/// 折叠消息头部单行预览的可用列数预算 (与 message_list.cpp 同款自适应宽度):
/// 内容区总列数 - 头部前缀显示列数 - 安全余量。
/// maxWidth 为 scrollable_->contentWidth() (已扣除滚动条 gutter);
/// 余量 1 列防边界取整溢出 (超宽仍由 xflex_shrink 在右缘兜底裁剪)。
/// 极窄终端下保底 8 列, 避免预览被完全挤没。
inline int collapsedPreviewBudget(int maxWidth, int prefixCols) {
    constexpr int kSlack     = 1;
    constexpr int kMinBudget = 8;
    const int     avail      = maxWidth - prefixCols - kSlack;
    return (avail >= kMinBudget) ? avail : kMinBudget;
}

// ---------------------------------------------------------------------------
// ModelSelectorOverlay
// ---------------------------------------------------------------------------

Element ModelSelectorOverlay::OnRender() {
    const auto& st         = *ctx_.frameState;
    const auto& theme      = *ctx_.theme;
    const int   maxVisible = std::max(5, Terminal::Size().dimy / 2);

    Elements items;
    for (size_t i = 0; i < st.modelNames.size(); ++i) {
        auto entry = text(st.modelNames[i]);
        if (static_cast<int>(i) == selectedIndex_) {
            entry = entry | bgcolor(theme.buttonActiveBgColor) | color(theme.buttonActiveTextColor)
                    | focus;
        } else {
            entry = entry | color(theme.normalColor);
        }
        items.push_back(entry);
    }

    Element list;
    if (st.modelNames.empty()) {
        // 尚未收到服务端模型信息响应 → 加载中; 已收到但为空 → 确实无可用模型
        if (!st.modelInfoLoaded) {
            list = text("Loading models...") | dim;
        } else {
            list = text("(no models available)") | dim;
        }
    } else {
        list = hbox({
                   text(" "),
                   vbox(std::move(items)) | bold | yframe | vscroll_indicator,
                   text(" "),
               })
               | size(HEIGHT, LESS_THAN, maxVisible);
    }

    return vbox({
               text(" Select Model ") | bold | inverted,
               separator(),
               list,
               separator(),
               text(" [Up/Down] Move  [Enter] Select  [Esc] Cancel ") | center | dim,
           })
           | border | size(WIDTH, LESS_THAN, 50) | color(theme.accentColor);
}

bool ModelSelectorOverlay::OnEvent(Event event) {
    if (event == Event::ArrowUp) {
        if (selectedIndex_ > 0) {
            --selectedIndex_;
        }
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::ArrowDown) {
        auto snap = ctx_.state->readSnapshot();
        if (selectedIndex_ + 1 < static_cast<int>(snap->modelNames.size())) {
            ++selectedIndex_;
        }
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::Return) {
        confirmSelection();
        ctx_.postRedraw();
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    if (event == Event::Escape) {
        ctx_.postRedraw();
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    return true;
}

void ModelSelectorOverlay::confirmSelection() {
    std::string selected;
    ctx_.state->mutate([&](TUIRenderState& st) {
        if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(st.modelNames.size())) {
            st.cachedModelName = st.modelNames[selectedIndex_];
            selected           = st.cachedModelName;
        }
    });
    if (onConfirm_ && !selected.empty()) {
        onConfirm_(selected);
    }
}

// ---------------------------------------------------------------------------
// SessionSelectorOverlay
// ---------------------------------------------------------------------------

Element SessionSelectorOverlay::OnRender() {
    const auto& st         = *ctx_.frameState;
    const auto& theme      = *ctx_.theme;
    const int   maxVisible = std::max(5, Terminal::Size().dimy / 2);

    // 列表项布局: 索引 0 = 固定 "新会话" 入口, 其后为持久化会话 (sessionList[0..])
    itemBoxes_.assign(st.sessionList.size() + 1, Box{});

    Elements items;

    // 顶部固定 "新会话" 项 (列表加载中也常驻, 保证始终可新建)
    {
        auto newEntry = text("+ 新会话");
        if (selectedIndex_ == 0) {
            newEntry = newEntry | bgcolor(theme.buttonActiveBgColor)
                       | color(theme.buttonActiveTextColor) | focus;
        } else {
            newEntry = newEntry | color(theme.normalColor);
        }
        items.push_back(newEntry | reflect(itemBoxes_[0]));
    }

    if (!st.sessionListLoaded) {
        // 列表请求已发出, 响应尚未到达
        items.push_back(text("Loading sessions...") | dim);
    } else if (st.sessionList.empty()) {
        items.push_back(text("(no persisted sessions)") | dim);
    } else {
        for (size_t i = 0; i < st.sessionList.size(); ++i) {
            const auto& s = st.sessionList[i];
            // 第一行: 会话名称 (title 为空时回退 sessionId)
            const std::string title     = s.title.empty() ? s.sessionId : s.title;
            const bool        isCurrent = (s.sessionId == ctx_.sessionId);
            // 第二行: 最近活动日期
            auto dateLine = text(agentxx::util::formatDateTimeMilliseconds(s.lastActiveMs)) | dim;

            // 当前会话条目: 名称后附加 "(current)" 标记
            Element row = vbox({
                isCurrent ? text(fmt::format("{} (current)", title)) : text(title),
                dateLine,
            });

            // +1: 会话条目从索引 1 开始 (0 为 "新会话" 入口)
            if (static_cast<int>(i) + 1 == selectedIndex_) {
                row = row | bgcolor(theme.buttonActiveBgColor) | color(theme.buttonActiveTextColor)
                      | focus;
            } else {
                row = row | color(theme.normalColor);
            }
            items.push_back(row | reflect(itemBoxes_[i + 1]));
        }

        // 尾部分页状态行 (非选择项, 无命中区域): 分页加载中显示加载提示;
        // 还有未加载会话时显示续取提示与已加载进度 (选择下移接近末尾时自动预取)
        if (st.sessionListLoadingMore) {
            items.push_back(text("↓ 加载中...") | dim);
        } else if (st.sessionListHasMore) {
            const std::string hint = st.sessionListTotalCount > 0
                                         ? fmt::format(
                                               "已加载 {}/{}  ↓ 下移加载更多",
                                               st.sessionList.size(),
                                               st.sessionListTotalCount
                                           )
                                         : std::string("↓ 下移加载更多");
            items.push_back(text(hint) | dim);
        }
    }

    return vbox({
               text(" Select Session ") | bold | inverted,
               separator(),
               hbox({
                   text(" "),
                   vbox(std::move(items)) | bold | yframe | vscroll_indicator,
                   text(" "),
               }) | size(HEIGHT, LESS_THAN, maxVisible),
               separator(),
               text(" [Up/Down] Move  [Enter] Switch  [Esc] Cancel ") | center | dim,
           })
           | border | size(WIDTH, LESS_THAN, 70) | color(theme.accentColor);
}

bool SessionSelectorOverlay::OnEvent(Event event) {
    auto snap = ctx_.state->readSnapshot();
    // 可选项总数: "新会话" 入口 (0) + 持久化会话
    const int count = 1 + static_cast<int>(snap->sessionList.size());

    if (event == Event::ArrowUp) {
        if (selectedIndex_ > 0) {
            --selectedIndex_;
        }
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::ArrowDown) {
        if (selectedIndex_ + 1 < count) {
            ++selectedIndex_;
        }
        // 选择项接近已加载列表末尾时预取下一页 (提前 kSessionPrefetchAhead 项):
        // 实现方内部做在途去重与 hasMore 边界判断, 高频调用安全
        constexpr int kSessionPrefetchAhead = 3;
        if (ctx_.requestMoreSessions && selectedIndex_ + kSessionPrefetchAhead >= count) {
            ctx_.requestMoreSessions();
        }
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::Return) {
        confirmSelection();
        return true;
    }
    if (event == Event::Escape) {
        ctx_.postRedraw();
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    if (event.is_mouse()) {
        const auto& mouse = event.mouse();
        if (mouse.button == Mouse::Left && mouse.motion == Mouse::Released) {
            for (size_t i = 0; i < itemBoxes_.size(); ++i) {
                if (itemBoxes_[i].Contain(mouse.x, mouse.y)) {
                    selectedIndex_ = static_cast<int>(i);
                    confirmSelection();
                    return true;
                }
            }
        }
    }
    return true;
}

void SessionSelectorOverlay::confirmSelection() {
    if (selectedIndex_ == 0) {
        // 顶部 "新会话" 入口: 新建会话 (无历史)
        if (onClose_) {
            onClose_();
        }
        if (onNewSession_) {
            onNewSession_();
        }
        return;
    }
    std::string selected;
    {
        auto snap = ctx_.state->readSnapshot();
        // -1: 会话条目从索引 1 开始 (0 为 "新会话" 入口)
        const int sessionIdx = selectedIndex_ - 1;
        if (sessionIdx >= 0 && sessionIdx < static_cast<int>(snap->sessionList.size())) {
            selected = snap->sessionList[sessionIdx].sessionId;
        }
    }
    if (onClose_) {
        onClose_();
    }
    if (onSelect_ && !selected.empty()) {
        onSelect_(std::move(selected));
    }
}

// ---------------------------------------------------------------------------
// SettingsOverlay
// ---------------------------------------------------------------------------

Element SettingsOverlay::OnRender() {
    const auto& theme = *ctx_.theme;

    // 当前主题名 (Dark/Light)
    const char* curThemeName = (theme.name == "Light") ? "Light" : "Dark";

    Elements items;

    // 主题 (单行显示当前值, 点击/Enter 循环切换 Dark <-> Light)
    items.push_back(text(" Theme ") | color(theme.hintColor));
    auto themeEntry = text(fmt::format(" Theme: {} ", curThemeName));
    if (selectedIndex_ == 0) {
        themeEntry = themeEntry | bgcolor(theme.buttonActiveBgColor)
                     | color(theme.buttonActiveTextColor) | bold | focus;
    } else {
        themeEntry = themeEntry | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
    }
    items.push_back(themeEntry | reflect(themeBox_));

    // 动画等级 (点击/Enter 循环切换; 组件经 TUISettings::isAnimationEnabled() 判断启用)
    items.push_back(text(" "));
    items.push_back(text(" Animation ") | color(theme.hintColor));
    auto animEntry
        = text(fmt::format(" Animation Level: {} ", TUISettings::instance().animationLevelName()));
    if (selectedIndex_ == 1) {
        animEntry = animEntry | bgcolor(theme.buttonActiveBgColor)
                    | color(theme.buttonActiveTextColor) | bold | focus;
    } else {
        animEntry = animEntry | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
    }
    items.push_back(animEntry | reflect(animLevelBox_));

    // 日志等级 (点击/Enter 循环切换; TUI 日志侧边栏按此过滤)
    items.push_back(text(" "));
    items.push_back(text(" Log ") | color(theme.hintColor));
    auto logEntry = text(fmt::format(" Log Level: {} ", TUISettings::instance().logLevelName()));
    if (selectedIndex_ == 2) {
        logEntry = logEntry | bgcolor(theme.buttonActiveBgColor)
                   | color(theme.buttonActiveTextColor) | bold | focus;
    } else {
        logEntry = logEntry | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
    }
    items.push_back(logEntry | reflect(logLevelBox_));

    // 末尾思考展示模式 (点击/Enter 循环切换: Auto Expand <-> Single Line)
    items.push_back(text(" "));
    items.push_back(text(" Thinking ") | color(theme.hintColor));
    auto thinkEntry
        = text(fmt::format(" Tail Thinking: {} ", TUISettings::instance().tailThinkingModeName()));
    if (selectedIndex_ == 3) {
        thinkEntry = thinkEntry | bgcolor(theme.buttonActiveBgColor)
                     | color(theme.buttonActiveTextColor) | bold | focus;
    } else {
        thinkEntry = thinkEntry | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
    }
    items.push_back(thinkEntry | reflect(tailThinkingBox_));

    // About (点击/Enter 打开关于弹窗)
    items.push_back(text(" "));
    items.push_back(text(" Info ") | color(theme.hintColor));
    auto aboutEntry = text(" About ");
    if (selectedIndex_ == 4) {
        aboutEntry = aboutEntry | bgcolor(theme.buttonActiveBgColor)
                     | color(theme.buttonActiveTextColor) | bold | focus;
    } else {
        aboutEntry = aboutEntry | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
    }
    items.push_back(aboutEntry | reflect(aboutBox_));

    return vbox({
               text(" Settings ") | bold | inverted,
               separator(),
               vbox(std::move(items)),
               separator(),
               text(" [Up/Down] Move  [Enter] Toggle  [Esc] Close ") | center | dim,
           })
           | border | size(WIDTH, LESS_THAN, 80) | color(theme.accentColor);
}

bool SettingsOverlay::OnEvent(Event event) {
    if (event == Event::ArrowUp) {
        if (selectedIndex_ > 0) {
            --selectedIndex_;
        }
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::ArrowDown) {
        if (selectedIndex_ + 1 < kItemCount) {
            ++selectedIndex_;
        }
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::Return) {
        if (selectedIndex_ == 0) {
            // 主题循环切换; 切换后保持弹窗打开, 便于继续调整
            // (主题变化经 onThemeChange_ 通知外部清理渲染缓存)
            cycleTheme();
            ctx_.postRedraw();
        } else if (selectedIndex_ == 1) {
            // 动画等级循环切换; 切换后保持弹窗打开, 便于继续调整
            cycleAnimationLevel();
            ctx_.postRedraw();
        } else if (selectedIndex_ == 2) {
            // 日志等级循环切换; 切换后保持弹窗打开, 便于继续调整
            cycleLogLevel();
            ctx_.postRedraw();
        } else if (selectedIndex_ == 3) {
            // 末尾思考模式循环切换; 切换后保持弹窗打开, 便于继续调整
            cycleTailThinkingMode();
            ctx_.postRedraw();
        } else if (selectedIndex_ == 4) {
            // About: 打开关于弹窗 (经回调通知外部)
            if (onAbout_) {
                onAbout_();
            }
        }
        return true;
    }
    if (event == Event::Escape) {
        ctx_.postRedraw();
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    if (event.is_mouse() && handleMouse(event.mouse())) {
        ctx_.postRedraw();
        return true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// LogMenuOverlay
// ---------------------------------------------------------------------------

Element LogMenuOverlay::OnRender() {
    const auto& theme = *ctx_.theme;

    auto renderBtn = [&](int idx, std::string_view label, Box& box) {
        const bool selected = (selectedIndex_ == idx);
        auto       el       = text(fmt::format(" {} ", label));
        if (selected) {
            el = el | bgcolor(theme.buttonActiveBgColor) | color(theme.buttonActiveTextColor)
                 | bold;
        } else {
            el = el | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
        }
        return hbox({
                   text("  "),
                   std::move(el) | reflect(box) | xflex,
                   text("  "),
               })
               | xflex;
    };

    auto btn1 = renderBtn(0, "LLM Context", llmContextBox_);
    auto btn2 = renderBtn(1, "Summy Context", summyContextBox_);
    auto btn3 = renderBtn(2, "Clear Logs", clearLogsBox_);

    auto header = hbox({
        text(" Menu ") | bold | inverted,
        filler(),
    });

    return vbox({
               header,
               separator(),
               text(" "),
               std::move(btn1),
               text(" "),
               std::move(btn2),
               text(" "),
               std::move(btn3),
               text(" "),
               separator(),
               text(" [Up/Down] Select  [Enter] Confirm  [Esc] Close ") | center | dim,
           })
           | border | size(WIDTH, EQUAL, 32) | color(theme.accentColor);
}

bool LogMenuOverlay::OnEvent(Event event) {
    if (event == Event::Escape) {
        ctx_.postRedraw();
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    if (event == Event::ArrowUp) {
        selectedIndex_ = (selectedIndex_ - 1 + kItemCount) % kItemCount;
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::ArrowDown) {
        selectedIndex_ = (selectedIndex_ + 1) % kItemCount;
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::Return) {
        confirmSelection();
        return true;
    }
    if (event.is_mouse() && handleMouse(event.mouse())) {
        ctx_.postRedraw();
        return true;
    }
    return true;
}

void LogMenuOverlay::confirmSelection() {
    ctx_.postRedraw();
    if (selectedIndex_ == 0) {
        if (onLlmContext_) {
            onLlmContext_();
        }
    } else if (selectedIndex_ == 1) {
        if (onSummyContext_) {
            onSummyContext_();
        }
    } else if (selectedIndex_ == 2) {
        if (onClearLogs_) {
            onClearLogs_();
        }
    }
}

bool LogMenuOverlay::handleMouse(const Mouse& mouse) {
    if (mouse.button != Mouse::Left || mouse.motion != Mouse::Released) {
        return false;
    }
    if (llmContextBox_.Contain(mouse.x, mouse.y)) {
        selectedIndex_ = 0;
        if (onLlmContext_) {
            onLlmContext_();
        }
        return true;
    }
    if (summyContextBox_.Contain(mouse.x, mouse.y)) {
        selectedIndex_ = 1;
        if (onSummyContext_) {
            onSummyContext_();
        }
        return true;
    }
    if (clearLogsBox_.Contain(mouse.x, mouse.y)) {
        selectedIndex_ = 2;
        if (onClearLogs_) {
            onClearLogs_();
        }
        return true;
    }
    return false;
}

bool SettingsOverlay::handleMouse(const Mouse& mouse) {
    if (mouse.button != Mouse::Left || mouse.motion != Mouse::Released) {
        return false;
    }
    if (themeBox_.Contain(mouse.x, mouse.y)) {
        // 主题循环切换 (与键盘 Enter 路径一致, 经 onThemeChange_ 通知外部清理缓存)
        cycleTheme();
        return true;
    }
    if (animLevelBox_.Contain(mouse.x, mouse.y)) {
        cycleAnimationLevel();
        return true;
    }
    if (logLevelBox_.Contain(mouse.x, mouse.y)) {
        cycleLogLevel();
        return true;
    }
    if (tailThinkingBox_.Contain(mouse.x, mouse.y)) {
        cycleTailThinkingMode();
        return true;
    }
    if (aboutBox_.Contain(mouse.x, mouse.y)) {
        if (onAbout_) {
            onAbout_();
        }
        return true;
    }
    return false;
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
}

void SettingsOverlay::cycleLogLevel() {
    auto&     settings = TUISettings::instance();
    const int next     = (static_cast<int>(settings.logLevel()) + 1)
                     % static_cast<int>(TUISettings::kLogLevelNames.size());
    settings.setLogLevel(static_cast<agentxx::util::LogLevel>(next));
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
            return "[Unknown]";
        }
        if (len < buf.size()) {
            buf.resize(len);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return std::filesystem::path(buf).generic_string();
#else
    std::error_code ec;
    auto            exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return "[Unknown]";
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
    size_t                   builtinCount = 0;
    const auto*              builtinList  = agentxx_plugin_get_builtin_plugins(&builtinCount);
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

    std::string serverIoStr = ctx_.remoteUrl.empty() ? "Inner Server (In-process)"
                                                     : fmt::format("Remote ({})", ctx_.remoteUrl);

    std::string dataDirStr = ctx_.dataDir;
    if (dataDirStr.empty()) {
        dataDirStr = ctx_.remoteUrl.empty() ? agentxx::agent::AgentConfigStatic::getDataDir("")
                                            : "(remote / not configured)";
    }

    std::string workDirStr = ctx_.workDir;
    if (workDirStr.empty()) {
        workDirStr = agentxx::agent::AgentConfigStatic::getCurrentWorkPath();
    }
    if (workDirStr.empty()) {
        workDirStr = "[Unknown]";
    }

    auto formatList = [](const std::vector<std::string>& list) -> std::string {
        if (list.empty()) {
            return "(none)";
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

    // Header: Agentxx & Version
    items.push_back(ScrollItem{
        hbox({
            text("Agentxx ") | bold | color(theme.accentColor),
            text(fmt::format("v{}", TUIClientAgentIO::kAgentxxVersion)) | bold
                | color(theme.normalColor),
        }),
        false
    });
    items.push_back(ScrollItem{text(""), false});

    addSection("GitHub · MIT", "https://github.com/coolight7/agentxx");
    addSection("Develop", "coolight · 郑泳坤 · 2465045051@qq.com");
    addSection("Executable Path", kExePath);
    addSection("Server-IO Type", serverIoStr);
    addSection("Data Directory (data_dir)", dataDirStr);
    addSection("Session Working Directory", workDirStr);
    addSection(fmt::format("Builtin Plugins ({})", builtinPlugins.size()), builtinStr);
    addSection(fmt::format("Loaded Plugins ({})", loadedPlugins.size()), loadedStr);

    return items;
}

Element AboutOverlay::OnRender() {
    const auto& theme  = *ctx_.theme;
    auto        header = hbox({
        text(" About ") | bold | inverted,
        filler(),
        text(" "),
    });

    const int margin = 2;
    const int termW  = Terminal::Size().dimx;
    const int termH  = Terminal::Size().dimy;
    const int wantW  = std::max(50, std::min(76, termW * 4 / 5));
    const int wantH  = std::max(12, std::min(24, termH * 4 / 5));
    const int availW = std::max(1, termW - margin * 2);
    const int availH = std::max(1, termH - margin * 2);
    const int popupW = std::min(wantW, availW);
    const int popupH = std::min(wantH, availH);
    return vbox({
               header,
               separator(),
               hbox({text(" "), scrollable_->Render() | flex, text(" ")}) | flex,
               separator(),
               text(" [Wheel/Up/Down] Scroll  [Esc/Enter] Close ") | center | dim,
           })
           | border | size(WIDTH, GREATER_THAN, popupW) | size(WIDTH, LESS_THAN, popupW)
           | size(HEIGHT, GREATER_THAN, popupH) | size(HEIGHT, LESS_THAN, popupH)
           | color(theme.accentColor);
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

    itemBoxes_.assign(st.pendingInputs.size(), Box{});
    delBoxes_.assign(st.pendingInputs.size(), Box{});

    auto clearBtn = text(" 清空 ") | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor)
                    | bold | reflect(clearBox_);
    auto header = hbox({
        text(" 待发送消息队列 ") | bold,
        filler(),
        clearBtn,
        text(" "),
    });

    Elements items;
    if (st.pendingInputs.empty()) {
        items.push_back(text(" (空) ") | dim);
    }
    for (size_t i = 0; i < st.pendingInputs.size(); ++i) {
        const auto& pi     = st.pendingInputs[i];
        auto        delBtn = text(" ✕ ") | bgcolor(theme.buttonBgColor) | color(theme.systemColor)
                      | reflect(delBoxes_[i]);
        Element row;
        if (pi.expanded) {
            row = hbox({
                text("- ") | color(theme.hintColor),
                paragraph(pi.text) | flex,
                delBtn,
            });
        } else {
            row = hbox({
                text("+ ") | color(theme.userColor),
                text(oneLinePreview(pi.text)) | color(theme.userColor) | flex,
                delBtn,
            });
        }
        items.push_back(row | reflect(itemBoxes_[i]));
    }

    const int maxVisible = std::max(5, Terminal::Size().dimy / 2);
    return vbox({
               header,
               separator(),
               vbox(std::move(items)) | yframe | vscroll_indicator
                   | size(HEIGHT, LESS_THAN, maxVisible),
               separator(),
               text(" 点击消息展开/折叠  点击 ✕ 删除  [Esc] 关闭 ") | center | dim,
           })
           | border | size(WIDTH, LESS_THAN, 70) | size(WIDTH, GREATER_THAN, 40)
           | color(theme.accentColor);
}

bool PendingInputsOverlay::OnEvent(Event event) {
    if (event == Event::Escape) {
        ctx_.postRedraw();
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    if (event.is_mouse() && handleMouse(event.mouse())) {
        ctx_.postRedraw();
        return true;
    }
    return true;
}

bool PendingInputsOverlay::handleMouse(const Mouse& mouse) {
    if (mouse.button != Mouse::Left || mouse.motion != Mouse::Released) {
        return false;
    }
    if (clearBox_.Contain(mouse.x, mouse.y)) {
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
    bool handled = false;
    ctx_.state->mutate([&](TUIRenderState& st) {
        for (size_t i = 0; i < delBoxes_.size() && i < st.pendingInputs.size(); ++i) {
            if (delBoxes_[i].Contain(mouse.x, mouse.y)) {
                auto itemId = st.pendingInputs[i].id;
                if (onDeleteItem_) {
                    onDeleteItem_(std::move(itemId));
                }
                st.pendingInputs.erase(st.pendingInputs.begin() + static_cast<std::ptrdiff_t>(i));
                handled = true;
                return;
            }
        }
        for (size_t i = 0; i < itemBoxes_.size() && i < st.pendingInputs.size(); ++i) {
            if (itemBoxes_[i].Contain(mouse.x, mouse.y)) {
                st.pendingInputs[i].expanded = !st.pendingInputs[i].expanded;
                handled                      = true;
                return;
            }
        }
    });
    return handled;
}

// ---------------------------------------------------------------------------
// ContextOverlay
// ---------------------------------------------------------------------------

namespace {

/// 从消息 JSON 提取 role 字符串 (缺失时返回空串)
std::string ctxMsgRole(const neograph::json& m) {
    return m.value("role", std::string{});
}

/// 从消息 JSON 提取 tool_calls 名称列表 (缺失/非数组返回空)
/// 用于折叠头预览与展开体摘要行
std::vector<std::string> ctxMsgToolNames(const neograph::json& m) {
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

    std::vector<ScrollItem> items;
    if (!msgsPtr || !msgsPtr->is_array() || msgsPtr->empty()) {
        items.push_back(ScrollItem{text(" (empty) ") | dim, true});
        return items;
    }

    const auto& msgs = *msgsPtr;
    items.reserve(msgs.size() * 2 + 1);
    headerItemIndex_.assign(msgs.size(), 0);
    for (size_t i = 0; i < msgs.size(); ++i) {
        const auto& m         = msgs[i];
        const auto  role      = ctxMsgRole(m);
        const Color roleColor = ctxRoleColor(theme, role);
        const bool  expanded  = expandedSet_.contains(i);

        // 记录折叠头在 items 中的子项索引 (消息可能展开插入 body, 头索引不固定)
        headerItemIndex_[i] = items.size();
        items.push_back(ScrollItem{buildMessageHeader(m, expanded, roleColor), false});

        if (expanded) {
            items.push_back(ScrollItem{buildMessageBody(m), false});
        }
    }
    return items;
}

std::vector<ftxui::Box> ContextOverlay::headerBoxes() const {
    // 从 Scrollable 最近一次渲染的可见区域反推各消息折叠头命中区域:
    // visibleBoxes 与 buildItems 返回的 items 一一对应, 消息 i 的折叠头
    // 子项索引由 headerItemIndex_ 记录 (展开体插入会使索引不固定)。
    // 已按视口裁剪 —— 视口外子项为空 Box, 不含测量盒, 点击不会误命中。
    const auto&             msgsPtr = ctx_.frameState->contextMessages;
    const size_t            nMsgs   = (msgsPtr && msgsPtr->is_array()) ? msgsPtr->size() : 0;
    std::vector<ftxui::Box> boxes(nMsgs, ftxui::Box{0, -1, 0, -1});
    const auto&             vis = scrollable_->visibleBoxes();
    for (size_t i = 0; i < nMsgs && i < headerItemIndex_.size(); ++i) {
        const size_t itemIdx = headerItemIndex_[i];
        if (itemIdx < vis.size()) {
            boxes[i] = vis[itemIdx];
        }
    }
    return boxes;
}

ftxui::Element ContextOverlay::buildMessageHeader(
    const neograph::json& m,
    bool                  expanded,
    const ftxui::Color&   roleColor
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
    return head;
}

ftxui::Element ContextOverlay::buildMessageBody(const neograph::json& m) {
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
    const auto& theme   = *ctx_.theme;
    const auto& msgsPtr = ctx_.frameState->contextMessages;

    const int margin = 2;
    const int termW  = Terminal::Size().dimx;
    const int termH  = Terminal::Size().dimy;
    const int wantW  = std::max(60, termW * 4 / 5);
    const int wantH  = std::max(14, termH * 4 / 5);
    const int availW = std::max(1, termW - margin * 2);
    const int availH = std::max(1, termH - margin * 2);
    const int popupW = std::min(wantW, availW);
    const int popupH = std::min(wantH, availH);

    auto title
        = fmt::format(" LLM Context · {}", (msgsPtr && msgsPtr->is_array()) ? msgsPtr->size() : 0);

    // 先渲染滚动区, 再返回整体布局; 折叠头命中区域由事件处理时
    // 从 scrollable_->visibleBoxes() 实时反推 (见 headerBoxes/handleHeaderClick)
    Element body = scrollable_->Render() | flex;

    return vbox({
               text(title) | bold | inverted,
               separator(),
               hbox({text(" "), body, text(" ")}) | flex,
               separator(),
               text(
                   " [Click/Enter/Space] Toggle  [Wheel/Up/Down] Scroll  [PgUp/PgDn] Page  [Esc] Close "
               ) | center
                   | dim,
           })
           | border | size(WIDTH, GREATER_THAN, popupW) | size(WIDTH, LESS_THAN, popupW)
           | size(HEIGHT, GREATER_THAN, popupH) | size(HEIGHT, LESS_THAN, popupH)
           | color(theme.accentColor);
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
    // Enter / Space: 切换最近可见 (首个可见) 消息的折叠状态
    if (event == Event::Return || event == Event::Character(" ")) {
        const auto&  msgsPtr = ctx_.frameState->contextMessages;
        const size_t nMsgs   = (msgsPtr && msgsPtr->is_array()) ? msgsPtr->size() : 0;
        const auto&  vis     = scrollable_->visibleBoxes();
        for (size_t i = 0; i < nMsgs && i < headerItemIndex_.size(); ++i) {
            const size_t itemIdx = headerItemIndex_[i];
            if (itemIdx >= vis.size() || vis[itemIdx].IsEmpty()) {
                continue;
            }
            toggleExpanded(i);
            ctx_.postRedraw();
            return true;
        }
    }
    return true;
}

bool ContextOverlay::handleHeaderClick(const Mouse& mouse) {
    if (mouse.button != Mouse::Left || mouse.motion != Mouse::Released) {
        return false;
    }
    // 命中区域基于最近一次渲染的可见子项 (与 msgs 索引对应: 消息 i 的
    // 折叠头子项索引由 headerItemIndex_ 记录); 视口外子项为空 Box, 自然跳过
    const auto&  msgsPtr = ctx_.frameState->contextMessages;
    const size_t nMsgs   = (msgsPtr && msgsPtr->is_array()) ? msgsPtr->size() : 0;
    const auto&  vis     = scrollable_->visibleBoxes();
    for (size_t i = 0; i < nMsgs && i < headerItemIndex_.size(); ++i) {
        const size_t itemIdx = headerItemIndex_[i];
        if (itemIdx >= vis.size()) {
            continue;
        }
        const auto& box = vis[itemIdx];
        if (box.IsEmpty()) {
            continue;
        }
        if (mouse.y < box.y_min || mouse.y > box.y_max) {
            continue;
        }
        if (mouse.x < box.x_min || mouse.x > box.x_max) {
            continue;
        }
        toggleExpanded(i);
        return true;
    }
    return false;
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

MermaidDiagramOverlay::MermaidDiagramOverlay(TUICtx& ctx, std::string mermaid) :
    ctx_(ctx),
    mermaid_(std::move(mermaid)) {
    scrollable_ = std::make_shared<Scrollable>([this]() -> std::vector<ScrollItem> {
        return buildItems();
    });
    scrollable_->setStickToBottom(false);
    Add(scrollable_);
}

std::vector<ScrollItem> MermaidDiagramOverlay::buildItems() {
    const auto& theme = *ctx_.theme;
    const int   maxW  = std::max(40, ftxui::Terminal::Size().dimx - 10);
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
            ScrollItem{ftxui::text(" (no diagram) ") | ftxui::dim, false}
        };
    }
    return {
        ScrollItem{cachedElement_, false}
    };
}

ftxui::Element MermaidDiagramOverlay::OnRender() {
    const auto& theme  = *ctx_.theme;
    auto        header = ftxui::hbox({
        ftxui::text(" Graph ") | ftxui::bold,
        ftxui::filler(),
        ftxui::text(" "),
    });
    const int   margin = 2;
    const int   termW  = ftxui::Terminal::Size().dimx;
    const int   termH  = ftxui::Terminal::Size().dimy;
    const int   wantW  = std::max(40, termW * 4 / 5);
    const int   wantH  = std::max(14, termH * 4 / 5);
    const int   availW = std::max(1, termW - margin * 2);
    const int   availH = std::max(1, termH - margin * 2);
    const int   popupW = std::min(wantW, availW);
    const int   popupH = std::min(wantH, availH);
    return ftxui::vbox({
               header,
               ftxui::separator(),
               ftxui::hbox({ftxui::text(" "), scrollable_->Render() | ftxui::flex, ftxui::text(" ")}
               ) | ftxui::flex,
               ftxui::separator(),
               ftxui::text(" [Wheel/Up/Down] Scroll  [Esc] Close ") | ftxui::center | ftxui::dim,
           })
           | ftxui::border | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, popupW)
           | ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, popupW)
           | ftxui::size(ftxui::HEIGHT, ftxui::GREATER_THAN, popupH)
           | ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, popupH) | ftxui::color(theme.accentColor);
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
const char* appendTypeLabel(agentxx::agent::AppendComponentNotification::Type type) {
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
    return "Unknown";
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
        items.push_back(ScrollItem{text(" (no failed components) ") | dim, false});
    }
    return items;
}

Element FailedComponentsOverlay::OnRender() {
    const auto& theme  = *ctx_.theme;
    auto        header = hbox({
        text(" Failed Components ") | bold,
        filler(),
        text(" "),
    });

    // 弹窗大小: 宽 3/5 屏、高 2/5 屏, 不超过窗口可用空间 (减去边距);
    // 高度同时给 GREATER_THAN 下限, 避免惰性 viewport 自然高度塌缩成单行
    // (原因详见下方弹窗 OnRender 注释)
    const int margin = 2;
    const int termW  = Terminal::Size().dimx;
    const int termH  = Terminal::Size().dimy;
    const int wantW  = std::max(40, termW * 3 / 5);
    const int wantH  = std::max(10, termH * 2 / 5);
    const int availW = std::max(1, termW - margin * 2);
    const int availH = std::max(1, termH - margin * 2);
    const int popupW = std::min(wantW, availW);
    const int popupH = std::min(wantH, availH);
    return vbox({
               header,
               separator(),
               hbox({text(" "), scrollable_->Render() | flex, text(" ")}) | flex,
               separator(),
               text(" [Wheel/Up/Down] Scroll  [Esc] Close ") | center | dim,
           })
           | border | size(WIDTH, GREATER_THAN, popupW) | size(WIDTH, LESS_THAN, popupW)
           | size(HEIGHT, GREATER_THAN, popupH) | size(HEIGHT, LESS_THAN, popupH)
           | color(theme.errorColor);
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
