#include "agentxx-client/io/tui/components/overlays.h"
#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx/util/exception.h"
#include "ftxui/component/component.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/terminal.hpp"
#include <algorithm>
#include <markdown/state_diagram.hpp>

using namespace ftxui;

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
            const std::string hint =
                st.sessionListTotalCount > 0
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

Element ContextOverlay::OnRender() {
    const auto& st      = *ctx_.frameState;
    const auto& theme   = *ctx_.theme;
    const auto& msgsPtr = st.contextMessages;

    const int maxVisible = std::max(8, Terminal::Size().dimy - 10);

    Elements items;
    if (!msgsPtr || !msgsPtr->is_array() || msgsPtr->empty()) {
        items.push_back(text(" (empty) ") | dim);
    } else {
        const auto& msgs       = *msgsPtr;
        const int   totalItems = static_cast<int>(msgs.size());
        const int   maxScroll  = std::max(0, totalItems - maxVisible);
        scrollOffset_          = std::clamp(scrollOffset_, 0, maxScroll);

        const int end = std::min(totalItems, scrollOffset_ + maxVisible);
        for (int i = scrollOffset_; i < end; ++i) {
            const auto& m       = msgs[static_cast<size_t>(i)];
            auto        role    = m.value("role", std::string{});
            auto        content = m.value("content", std::string{});

            Color roleColor = theme.assistantColor;
            if (role == "user") {
                roleColor = theme.userColor;
            } else if (role == "system") {
                roleColor = theme.systemColor;
            } else if (role == "tool") {
                roleColor = theme.toolColor;
            }

            std::string preview = oneLinePreview(content, 60);
            if (m.contains("tool_calls")) {
                auto toolCalls = m["tool_calls"];
                if (toolCalls.is_array() && !toolCalls.empty()) {
                    std::string names;
                    for (const auto& tc : toolCalls) {
                        if (!names.empty()) {
                            names += ", ";
                        }
                        names += tc.value("name", std::string{});
                    }
                    preview = fmt::format("[tool_calls: {}]", names);
                }
            }

            items.push_back(hbox({
                text(fmt::format("{:>3} ", i)) | color(theme.hintColor),
                text(fmt::format("[{}] ", role)) | color(roleColor) | bold,
                text(preview) | color(theme.normalColor) | flex,
            }));
        }
    }

    auto title
        = fmt::format(" LLM Context ({}) ", (msgsPtr && msgsPtr->is_array()) ? msgsPtr->size() : 0);

    return vbox({
               text(title) | bold | inverted,
               separator(),
               vbox(std::move(items)) | size(HEIGHT, LESS_THAN, maxVisible),
               separator(),
               text(" [Up/Down] Scroll  [PgUp/PgDn] Page  [Esc] Close ") | center | dim,
           })
           | border | size(WIDTH, LESS_THAN, 100) | size(WIDTH, GREATER_THAN, 50)
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
    auto      snap       = ctx_.state->readSnapshot();
    const int total      = (snap->contextMessages && snap->contextMessages->is_array())
                               ? static_cast<int>(snap->contextMessages->size())
                               : 0;
    const int maxVisible = std::max(8, Terminal::Size().dimy - 10);
    const int maxScroll  = std::max(0, total - maxVisible);

    if (event == Event::ArrowUp) {
        scrollOffset_ = std::max(0, scrollOffset_ - 1);
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::ArrowDown) {
        scrollOffset_ = std::min(maxScroll, scrollOffset_ + 1);
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::PageUp) {
        scrollOffset_ = std::max(0, scrollOffset_ - maxVisible);
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::PageDown) {
        scrollOffset_ = std::min(maxScroll, scrollOffset_ + maxVisible);
        ctx_.postRedraw();
        return true;
    }
    if (event.is_mouse()) {
        const auto& mouse = event.mouse();
        if (mouse.button == Mouse::WheelUp) {
            scrollOffset_ = std::max(0, scrollOffset_ - 3);
            ctx_.postRedraw();
            return true;
        }
        if (mouse.button == Mouse::WheelDown) {
            scrollOffset_ = std::min(maxScroll, scrollOffset_ + 3);
            ctx_.postRedraw();
            return true;
        }
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
        case T::Type::Mcp: return "MCP";
        case T::Type::Skill: return "Skill";
        case T::Type::Memory: return "Memory";
        case T::Type::Plugin: return "Plugin";
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
