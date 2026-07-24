#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx/agent/model_registry.h"
#include "fmt/format.h"
#include "ftxui/screen/terminal.hpp"

using namespace ftxui;

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
        text(" model: ") | color(theme_.hintColor),
        text(modelName) | color(theme_.accentColor) | bold,
        text(" [F2] ") | color(theme_.hintColor),
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
    });
}

void AgentTUI::openModelSelector() {
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
    }
    showModelSelector_ = false;
}
