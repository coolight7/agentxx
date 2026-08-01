#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/util/string_util.h"
#include "fmt/format.h"
#include "ftxui/screen/terminal.hpp"

using namespace ftxui;

ftxui::Element AgentTUI::renderModelSelectorOverlay() {
    const auto& st         = *frameState_;
    const int   maxVisible = std::max(5, ftxui::Terminal::Size().dimy / 2);

    Elements items;
    for (size_t i = 0; i < st.modelNames.size(); ++i) {
        auto entry = text(" " + st.modelNames[i] + " ");
        if (static_cast<int>(i) == selectedModelIndex_) {
            entry = entry | bgcolor(theme_.buttonActiveBgColor)
                    | color(theme_.buttonActiveTextColor) | bold | focus;
        } else {
            entry = entry | bgcolor(theme_.buttonBgColor) | color(theme_.buttonTextColor);
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
           | border | size(WIDTH, LESS_THAN, 50) | color(theme_.accentColor);
}

ftxui::Element AgentTUI::renderStatusBar() {
    const auto& st = *frameState_;

    std::string modelName = st.cachedModelName;
    if (modelName.empty()) {
        modelName = "<none>";
    }

    size_t ctx    = 0;
    size_t maxCtx = 0;
    if (session_ && session_->contextStats) {
        ctx    = session_->contextStats->contextTokens.load();
        maxCtx = session_->contextStats->maxContextTokens.load();
    }
    ftxui::Element ctxText;
    if (maxCtx > 0) {
        ctxText = hbox({
            text(agentxx::util::formatSize(ctx)) | color(theme_.hintColor),
            text("/") | color(theme_.hintColor) | dim,
            text(agentxx::util::formatSize(maxCtx)) | color(theme_.hintColor),
            text("·") | color(theme_.hintColor) | dim,
            text(fmt::format(
                "{}%",
                static_cast<int>(100.0 * static_cast<double>(ctx) / static_cast<double>(maxCtx))
            )) | color(theme_.hintColor),
        });
    } else {
        ctxText = text(fmt::format("{}", agentxx::util::formatSize(ctx))) | color(theme_.hintColor);
    }

    auto modelInfo = hbox({
        text("[F2] ") | color(theme_.hintColor),
        text(modelName) | color(theme_.accentColor),
        text(" · ") | color(theme_.hintColor),
        ctxText | color(theme_.hintColor),
    });
    return hbox({
        text(" "),
        modelInfo,
        text(" "),
        filler(),
        text(" "),
        text("[F3] Settings") | color(theme_.hintColor),
        text(" "),
    });
}

void AgentTUI::openModelSelector() {
    if (transport_) {
        sendToPeer(agentxx::agent::WireGetModel{threadId_});
    }
    selectedModelIndex_ = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < state_->modelNames.size(); ++i) {
        if (state_->modelNames[i] == state_->cachedModelName) {
            selectedModelIndex_ = static_cast<int>(i);
            break;
        }
    }
    showModelSelector_ = true;
}

void AgentTUI::confirmModelSelection() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (selectedModelIndex_ >= 0
        && selectedModelIndex_ < static_cast<int>(state_->modelNames.size())) {
        auto& st          = mutableStateLocked();
        st.cachedModelName = st.modelNames[selectedModelIndex_];
        requestSelectModel(threadId_, st.cachedModelName);
    }
    showModelSelector_ = false;
}

ftxui::Element AgentTUI::renderSettingsOverlay() {
    static constexpr const char* themeNames[] = {"Dark", "Light"};
    const int                    themeCount   = 2;

    Elements items;
    items.push_back(text(" Theme ") | color(theme_.hintColor));
    for (int i = 0; i < themeCount; ++i) {
        auto entry = text(" " + std::string(themeNames[i]) + " ");
        if (i == selectedSettingIndex_) {
            entry = entry | bgcolor(theme_.buttonActiveBgColor)
                    | color(theme_.buttonActiveTextColor) | bold | focus;
        } else {
            entry = entry | bgcolor(theme_.buttonBgColor) | color(theme_.buttonTextColor);
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
           | border | size(WIDTH, LESS_THAN, 40) | color(theme_.accentColor);
}

void AgentTUI::applyThemeSelection() {
    if (selectedSettingIndex_ == 0) {
        theme_ = TUITheme::darkTheme();
    } else if (selectedSettingIndex_ == 1) {
        theme_ = TUITheme::lightTheme();
    }
    // 主题变化后, 已缓存的消息/日志元素颜色已过时, 清空缓存强制下帧重建
    messageCache_.clear();
    logLineCache_.clear();
    showSettings_ = false;
}

ftxui::Element AgentTUI::renderPendingInputsOverlay() {
    const auto& st = *frameState_;

    pendingInputBoxes_.assign(st.pendingInputs.size(), ftxui::Box{});
    pendingInputDelBoxes_.assign(st.pendingInputs.size(), ftxui::Box{});

    // 顶部: 两端对齐 "待发送消息队列" ... [清空]
    auto clearBtn = text(" 清空 ") | bgcolor(theme_.buttonBgColor) | color(theme_.buttonTextColor)
                    | bold | reflect(pendingInputClearBox_);
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
        auto delBtn    = text(" ✕ ") | bgcolor(theme_.buttonBgColor) | color(theme_.systemColor)
                      | reflect(pendingInputDelBoxes_[i]);
        Element row;
        if (pi.expanded) {
            row = hbox({
                text("- ") | color(theme_.hintColor),
                paragraph(pi.text) | flex,
                delBtn,
            });
        } else {
            row = hbox({
                text("+ ") | color(theme_.hintColor),
                text(oneLinePreview(pi.text)) | color(theme_.assistantColor) | flex,
                delBtn,
            });
        }
        items.push_back(row | reflect(pendingInputBoxes_[i]));
    }

    const int maxVisible = std::max(5, ftxui::Terminal::Size().dimy / 2);
    return vbox({
               header,
               separator(),
               vbox(std::move(items)) | yframe | vscroll_indicator
                   | size(HEIGHT, LESS_THAN, maxVisible),
               separator(),
               text(" 点击消息展开/折叠  点击 ✕ 删除  [Esc] 关闭 ") | center | dim,
           })
           | border | size(WIDTH, LESS_THAN, 70) | size(WIDTH, GREATER_THAN, 40)
           | color(theme_.accentColor);
}

bool AgentTUI::handlePendingInputsMouse(const ftxui::Mouse& mouse) {
    if (mouse.button != Mouse::Left || mouse.motion != Mouse::Released) {
        return false;
    }
    // 清空按钮: 清空队列并关闭弹窗
    if (pendingInputClearBox_.Contain(mouse.x, mouse.y)) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto&                       st = mutableStateLocked();
        st.pendingInputs.clear();
        showPendingInputs_ = false;
        return true;
    }
    // 删除按钮 (优先于行展开判定)
    std::lock_guard<std::mutex> lock(mutex_);
    auto&                       st = mutableStateLocked();
    for (size_t i = 0; i < pendingInputDelBoxes_.size() && i < st.pendingInputs.size(); ++i) {
        if (pendingInputDelBoxes_[i].Contain(mouse.x, mouse.y)) {
            st.pendingInputs.erase(st.pendingInputs.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    // 消息行: 展开/折叠
    for (size_t i = 0; i < pendingInputBoxes_.size() && i < st.pendingInputs.size(); ++i) {
        if (pendingInputBoxes_[i].Contain(mouse.x, mouse.y)) {
            st.pendingInputs[i].expanded = !st.pendingInputs[i].expanded;
            return true;
        }
    }
    return false;
}

ftxui::Element AgentTUI::renderContextOverlay() {
    const auto& st   = *frameState_;
    const auto& msgs = st.contextMessages;

    const int maxVisible = std::max(8, ftxui::Terminal::Size().dimy - 10);

    Elements items;
    if (!msgs.is_array() || msgs.empty()) {
        items.push_back(text(" (empty) ") | dim);
    } else {
        const int totalItems = static_cast<int>(msgs.size());
        // clamp scroll offset
        const int maxScroll  = std::max(0, totalItems - maxVisible);
        contextScrollOffset_ = std::clamp(contextScrollOffset_, 0, maxScroll);

        // 仅渲染可见范围内的条目 (手动滚动, 无需 focus/yframe)
        const int end = std::min(totalItems, contextScrollOffset_ + maxVisible);
        for (int i = contextScrollOffset_; i < end; ++i) {
            const auto& m       = msgs[static_cast<size_t>(i)];
            auto        role    = m.value("role", std::string{});
            auto        content = m.value("content", std::string{});

            ftxui::Color roleColor = theme_.assistantColor;
            if (role == "user") {
                roleColor = theme_.promptColor;
            } else if (role == "system") {
                roleColor = theme_.hintColor;
            } else if (role == "tool") {
                roleColor = theme_.thinkingColor;
            }

            std::string preview = oneLinePreview(content, sidebarWidth_ - 5);
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
                text(fmt::format("{:>3} ", i)) | color(theme_.hintColor),
                text(fmt::format("[{}] ", role)) | color(roleColor) | bold,
                text(preview) | color(theme_.assistantColor) | flex,
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
           | color(theme_.accentColor);
}