#include "agentxx-client/io/tui/components/overlays.h"
#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/terminal.hpp"
#include <algorithm>

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
        auto entry = text(fmt::format(" {} ", st.modelNames[i]));
        if (static_cast<int>(i) == selectedIndex_) {
            entry = entry | bgcolor(theme.buttonActiveBgColor) | color(theme.buttonActiveTextColor)
                    | bold | focus;
        } else {
            entry = entry | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
        }
        items.push_back(entry);
    }

    Element list;
    if (st.modelNames.empty()) {
        // 尚未收到服务端模型信息响应 → 加载中; 已收到但为空 → 确实无可用模型
        if (!st.modelInfoLoaded) {
            list = text(" Loading models... ") | dim;
        } else {
            list = text(" (no models available) ") | dim;
        }
    } else {
        list = vbox(std::move(items)) | yframe | vscroll_indicator
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
// SettingsOverlay
// ---------------------------------------------------------------------------

Element SettingsOverlay::OnRender() {
    static constexpr const char* themeNames[] = {"Dark", "Light"};
    const auto&                  theme        = *ctx_.theme;

    // 当前主题对应的条目索引 (供初始高亮; 键盘导航时由 selectedIndex_ 接管)
    const int curThemeIdx = (theme.name == "Light") ? 1 : 0;

    Elements items;
    items.push_back(text(" Theme ") | color(theme.hintColor));
    for (int i = 0; i < 2; ++i) {
        auto entry = text(fmt::format(" {} ", themeNames[i]));
        if (i == selectedIndex_) {
            entry = entry | bgcolor(theme.buttonActiveBgColor) | color(theme.buttonActiveTextColor)
                    | bold | focus;
        } else {
            entry = entry | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
        }
        if (i == curThemeIdx && i != selectedIndex_) {
            entry = entry | bold;
        }
        items.push_back(entry | reflect(themeBoxes_[i]));
    }

    // 系统资源占用显示开关 (Info 侧边栏)
    const bool showSys
        = ctx_.showSystemInfo && ctx_.showSystemInfo->load(std::memory_order_relaxed);
    items.push_back(text(" "));
    items.push_back(text(" Info Sidebar ") | color(theme.hintColor));
    auto sysEntry = text(fmt::format(" Show System Info: {} ", showSys ? "ON" : "OFF"));
    if (selectedIndex_ == 2) {
        sysEntry = sysEntry | bgcolor(theme.buttonActiveBgColor)
                   | color(theme.buttonActiveTextColor) | bold | focus;
    } else {
        sysEntry = sysEntry | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
    }
    items.push_back(sysEntry | reflect(sysInfoBox_));

    // 动画等级 (Enter/点击循环切换; 组件经 TUISettings::isAnimationEnabled() 判断启用)
    items.push_back(text(" "));
    items.push_back(text(" Animation ") | color(theme.hintColor));
    auto animEntry = text(fmt::format(
        " Animation Level: {} ",
        TUISettings::instance().animationLevelName()
    ));
    if (selectedIndex_ == 3) {
        animEntry = animEntry | bgcolor(theme.buttonActiveBgColor)
                    | color(theme.buttonActiveTextColor) | bold | focus;
    } else {
        animEntry = animEntry | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
    }
    items.push_back(animEntry | reflect(animLevelBox_));

    return vbox({
               text(" Settings ") | bold | inverted,
               separator(),
               vbox(std::move(items)),
               separator(),
               text(" [Up/Down] Move  [Enter] Apply/Toggle  [Esc] Close ") | center | dim,
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
            *ctx_.theme = TUITheme::darkTheme();
            ctx_.postRedraw();
            if (onClose_) {
                onClose_();
            }
        } else if (selectedIndex_ == 1) {
            *ctx_.theme = TUITheme::lightTheme();
            ctx_.postRedraw();
            if (onClose_) {
                onClose_();
            }
        } else if (selectedIndex_ == 2 && ctx_.showSystemInfo) {
            // 切换后保持弹窗打开, 便于继续调整; 由 [Esc] 关闭
            ctx_.showSystemInfo->store(
                !ctx_.showSystemInfo->load(std::memory_order_relaxed),
                std::memory_order_relaxed
            );
            ctx_.postRedraw();
        } else if (selectedIndex_ == 3) {
            // 动画等级循环切换; 切换后保持弹窗打开, 便于继续调整
            cycleAnimationLevel();
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
    if (themeBoxes_[0].Contain(mouse.x, mouse.y)) {
        *ctx_.theme = TUITheme::darkTheme();
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    if (themeBoxes_[1].Contain(mouse.x, mouse.y)) {
        *ctx_.theme = TUITheme::lightTheme();
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    if (ctx_.showSystemInfo && sysInfoBox_.Contain(mouse.x, mouse.y)) {
        ctx_.showSystemInfo->store(
            !ctx_.showSystemInfo->load(std::memory_order_relaxed),
            std::memory_order_relaxed
        );
        return true;
    }
    if (animLevelBox_.Contain(mouse.x, mouse.y)) {
        cycleAnimationLevel();
        return true;
    }
    return false;
}

void SettingsOverlay::cycleAnimationLevel() {
    auto& settings = TUISettings::instance();
    const int next = (static_cast<int>(settings.animationLevel()) + 1)
                     % static_cast<int>(TUISettings::kAnimationLevelNames.size());
    settings.setAnimationLevel(static_cast<AnimationLevel>(next));
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
    const auto& st    = *ctx_.frameState;
    const auto& theme = *ctx_.theme;
    const auto& msgsPtr = st.contextMessages;

    const int maxVisible = std::max(8, Terminal::Size().dimy - 10);

    Elements items;
    if (!msgsPtr || !msgsPtr->is_array() || msgsPtr->empty()) {
        items.push_back(text(" (empty) ") | dim);
    } else {
        const auto& msgs      = *msgsPtr;
        const int   totalItems = static_cast<int>(msgs.size());
        const int maxScroll  = std::max(0, totalItems - maxVisible);
        scrollOffset_        = std::clamp(scrollOffset_, 0, maxScroll);

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

    auto title = fmt::format(
        " LLM Context ({}) ",
        (msgsPtr && msgsPtr->is_array()) ? msgsPtr->size() : 0
    );

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
    auto      snap = ctx_.state->readSnapshot();
    const int total = (snap->contextMessages && snap->contextMessages->is_array())
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
