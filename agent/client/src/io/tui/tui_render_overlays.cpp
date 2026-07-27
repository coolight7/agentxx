#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/util/string_util.h"
#include "fmt/format.h"
#include "ftxui/screen/terminal.hpp"

using namespace ftxui;

namespace {
/// 取首行并按 utf8 长度截断 (用于待发送消息折叠为一行)
std::string oneLine(std::string_view s, size_t max = 60) {
    const auto  nl = s.find('\n');
    std::string line{(nl == std::string::npos) ? s : s.substr(0, nl)};
    const auto  idx = agentxx::util::findIndexByUtf8Length(line, max);
    if (idx > 0 && idx < line.size()) {
        line.resize(idx);
        line += "...";
    }
    return line;
}
} // namespace

ftxui::Element AgentTUI::renderModelSelectorOverlay() {
    const int maxVisible = std::max(5, ftxui::Terminal::Size().dimy / 2);

    Elements items;
    for (size_t i = 0; i < modelNames_.size(); ++i) {
        auto entry = text(" " + modelNames_[i] + " ");
        if (static_cast<int>(i) == selectedModelIndex_) {
            entry = entry | bgcolor(theme_.buttonActiveBgColor)
                    | color(theme_.buttonActiveTextColor) | bold | focus;
        } else {
            entry = entry | bgcolor(theme_.buttonBgColor) | color(theme_.buttonTextColor);
        }
        items.push_back(entry);
    }

    Element list;
    if (modelNames_.empty()) {
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

ftxui::Element AgentTUI::renderPermissionOverlay() {
    const auto& req = pendingPermission_.value();
    return vbox({
               text(" Permission Request ") | bold | inverted,
               separator(),
               hbox({text(" Tool    : ") | bold, text(req.toolName)}),
               hbox({text(" Category: ") | bold, text(req.category)}),
               hbox({text(" Target  : ") | bold, text(req.target)}),
               separator(),
               text(" [y] Allow  [n/Esc] Deny ") | center,
           })
           | border | size(WIDTH, LESS_THAN, 60) | color(theme_.systemColor);
}

ftxui::Element AgentTUI::renderStatusBar() {
    std::string modelName = cachedModelName_;
    if (modelName.empty()) {
        modelName = "<none>";
    }
    auto modelInfo = hbox({
        text(" [F2] ") | color(theme_.hintColor),
        text(modelName) | color(theme_.accentColor) | bold,
    });

    size_t ctx    = 0;
    size_t maxCtx = 0;
    if (session_ && session_->contextStats) {
        ctx    = session_->contextStats->contextTokens.load();
        maxCtx = session_->contextStats->maxContextTokens.load();
    }
    const auto toK = [](size_t v) {
        return fmt::format("{:.1f}k", static_cast<double>(v) / 1000.0);
    };
    std::string ctxText;
    if (maxCtx > 0) {
        const double pct = 100.0 * static_cast<double>(ctx) / static_cast<double>(maxCtx);
        ctxText          = fmt::format(" {}/{} ({:.1f}%) ", toK(ctx), toK(maxCtx), pct);
    } else {
        ctxText = fmt::format(" {} ", toK(ctx));
    }
    auto ctxInfo = text(ctxText) | color(theme_.statusColor);

    return hbox({
        modelInfo,
        filler(),
        ctxInfo,
        text(" [Ctrl+I] Settings ") | color(theme_.hintColor),
    });
}

void AgentTUI::openModelSelector() {
    // 远程模式: 请求服务端刷新模型信息
    if (transport_) {
        sendToPeer(agentxx::agent::WireGetModel{threadId_});
    }
    modelNames_.clear();
    selectedModelIndex_ = 0;
    if (agentContext_ && agentContext_->modelRegistry) {
        modelNames_ = agentContext_->modelRegistry->listModelNames();
        for (size_t i = 0; i < modelNames_.size(); ++i) {
            if (modelNames_[i] == cachedModelName_) {
                selectedModelIndex_ = static_cast<int>(i);
                break;
            }
        }
    }
    showModelSelector_ = true;
}

void AgentTUI::confirmModelSelection() {
    if (selectedModelIndex_ >= 0 && selectedModelIndex_ < static_cast<int>(modelNames_.size())) {
        cachedModelName_ = modelNames_[selectedModelIndex_];
        if (session_) {
            session_->setModelName(cachedModelName_);
        }
        requestSelectModel(threadId_, cachedModelName_);
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
    showSettings_ = false;
}

ftxui::Element AgentTUI::renderPendingInputsOverlay() {
    pendingInputBoxes_.assign(pendingInputs_.size(), ftxui::Box{});
    pendingInputDelBoxes_.assign(pendingInputs_.size(), ftxui::Box{});

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
    if (pendingInputs_.empty()) {
        items.push_back(text(" (空) ") | dim);
    }
    for (size_t i = 0; i < pendingInputs_.size(); ++i) {
        const auto& pi = pendingInputs_[i];
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
                text(oneLine(pi.text)) | color(theme_.assistantColor) | flex,
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
        pendingInputs_.clear();
        showPendingInputs_ = false;
        return true;
    }
    // 删除按钮 (优先于行展开判定)
    for (size_t i = 0; i < pendingInputDelBoxes_.size() && i < pendingInputs_.size(); ++i) {
        if (pendingInputDelBoxes_[i].Contain(mouse.x, mouse.y)) {
            pendingInputs_.erase(pendingInputs_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    // 消息行: 展开/折叠
    for (size_t i = 0; i < pendingInputBoxes_.size() && i < pendingInputs_.size(); ++i) {
        if (pendingInputBoxes_[i].Contain(mouse.x, mouse.y)) {
            pendingInputs_[i].expanded = !pendingInputs_[i].expanded;
            return true;
        }
    }
    return false;
}
