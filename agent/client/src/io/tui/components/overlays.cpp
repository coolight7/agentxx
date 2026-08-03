#include "agentxx-client/io/tui/components/overlays.h"
#include "agentxx-client/io/tui/agent_tui.h"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/terminal.hpp"
#include <algorithm>

using namespace ftxui;

// ---------------------------------------------------------------------------
// ModelSelectorOverlay
// ---------------------------------------------------------------------------

Element ModelSelectorOverlay::OnRender() {
    const auto& st    = *ctx_.frameState;
    const auto& theme = *ctx_.theme;
    const int   maxVisible = std::max(5, Terminal::Size().dimy / 2);

    Elements items;
    for (size_t i = 0; i < st.modelNames.size(); ++i) {
        auto entry = text(" " + st.modelNames[i] + " ");
        if (static_cast<int>(i) == selectedIndex_) {
            entry = entry | bgcolor(theme.buttonActiveBgColor)
                    | color(theme.buttonActiveTextColor) | bold | focus;
        } else {
            entry = entry | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
        }
        items.push_back(entry);
    }

    Element list;
    if (st.modelNames.empty()) {
        list = text(" (no models available) ") | dim;
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
        if (onClose_) onClose_();
        return true;
    }
    if (event == Event::Escape) {
        ctx_.postRedraw();
        if (onClose_) onClose_();
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
    const int                    themeCount   = 2;
    const auto&                  theme        = *ctx_.theme;

    Elements items;
    items.push_back(text(" Theme ") | color(theme.hintColor));
    for (int i = 0; i < themeCount; ++i) {
        auto entry = text(" " + std::string(themeNames[i]) + " ");
        if (i == selectedIndex_) {
            entry = entry | bgcolor(theme.buttonActiveBgColor)
                    | color(theme.buttonActiveTextColor) | bold | focus;
        } else {
            entry = entry | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
        }
        items.push_back(entry);
    }

    return vbox({
               text(" Settings ") | bold | inverted,
               separator(),
               vbox(std::move(items)),
               separator(),
               text(" [Up/Down] Move  [Enter] Apply  [Esc] Close ") | center | dim,
           })
           | border | size(WIDTH, LESS_THAN, 40) | color(theme.accentColor);
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
        if (selectedIndex_ < 1) {
            ++selectedIndex_;
        }
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::Return) {
        if (selectedIndex_ == 0) {
            *ctx_.theme = TUITheme::darkTheme();
        } else if (selectedIndex_ == 1) {
            *ctx_.theme = TUITheme::lightTheme();
        }
        ctx_.postRedraw();
        if (onClose_) onClose_();
        return true;
    }
    if (event == Event::Escape) {
        ctx_.postRedraw();
        if (onClose_) onClose_();
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
        const auto& pi = st.pendingInputs[i];
        auto delBtn    = text(" ✕ ") | bgcolor(theme.buttonBgColor) | color(theme.systemColor)
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
                text("+ ") | color(theme.hintColor),
                text(oneLinePreview(pi.text)) | color(theme.assistantColor) | flex,
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
        if (onClose_) onClose_();
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
        if (onClose_) onClose_();
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
                handled = true;
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
    const auto& msgs  = st.contextMessages;

    const int maxVisible = std::max(8, Terminal::Size().dimy - 10);

    Elements items;
    if (!msgs.is_array() || msgs.empty()) {
        items.push_back(text(" (empty) ") | dim);
    } else {
        const int totalItems = static_cast<int>(msgs.size());
        const int maxScroll  = std::max(0, totalItems - maxVisible);
        scrollOffset_        = std::clamp(scrollOffset_, 0, maxScroll);

        const int end = std::min(totalItems, scrollOffset_ + maxVisible);
        for (int i = scrollOffset_; i < end; ++i) {
            const auto& m       = msgs[static_cast<size_t>(i)];
            auto        role    = m.value("role", std::string{});
            auto        content = m.value("content", std::string{});

            Color roleColor = theme.assistantColor;
            if (role == "user") {
                roleColor = theme.promptColor;
            } else if (role == "system") {
                roleColor = theme.hintColor;
            } else if (role == "tool") {
                roleColor = theme.thinkingColor;
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
                    preview = "[tool_calls: " + names + "]";
                }
            }

            items.push_back(hbox({
                text(fmt::format("{:>3} ", i)) | color(theme.hintColor),
                text(fmt::format("[{}] ", role)) | color(roleColor) | bold,
                text(preview) | color(theme.assistantColor) | flex,
            }));
        }
    }

    auto title = fmt::format(" LLM Context ({}) ", (msgs.is_array() ? msgs.size() : 0));

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
        if (onClose_) onClose_();
        return true;
    }
    auto snap       = ctx_.state->readSnapshot();
    const int total = snap->contextMessages.is_array()
                          ? static_cast<int>(snap->contextMessages.size())
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
