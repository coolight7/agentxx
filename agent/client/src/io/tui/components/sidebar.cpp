#include "agentxx-client/io/tui/components/sidebar.h"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/terminal.hpp"
#include <algorithm>

using namespace ftxui;

SidebarComponent::SidebarComponent(TUICtx& ctx) :
    ctx_(ctx) {
    scrollable_ = std::make_shared<Scrollable>([this]() -> std::vector<ScrollItem> {
        if (activeTab_ >= 0 && activeTab_ < static_cast<int>(tabs_.size())) {
            return tabs_[activeTab_].render();
        }
        return {ScrollItem{text(" "), false}};
    });
    Add(scrollable_);
}

void SidebarComponent::addTab(
    std::string_view                         id,
    std::string_view                         title,
    std::function<std::vector<ScrollItem>()> render,
    std::function<ftxui::Element()>          footer
) {
    for (auto& tab : tabs_) {
        if (tab.id == id) {
            tab.title  = std::string{title};
            tab.render = std::move(render);
            tab.footer = std::move(footer);
            return;
        }
    }
    tabs_.push_back(Tab{std::string{id}, std::string{title}, std::move(render), std::move(footer)});
    activeTab_ = static_cast<int>(tabs_.size()) - 1;
}

void SidebarComponent::removeTab(std::string_view id) {
    for (size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i].id == id) {
            tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(i));
            if (activeTab_ >= static_cast<int>(tabs_.size())) {
                activeTab_ = static_cast<int>(tabs_.size()) - 1;
            }
            return;
        }
    }
}

bool SidebarComponent::hasTab(std::string_view id) const {
    for (const auto& tab : tabs_) {
        if (tab.id == id) {
            return true;
        }
    }
    return false;
}

Element SidebarComponent::OnRender() {
    const auto& theme = *ctx_.theme;

    tabBoxes_.assign(tabs_.size(), Box{});
    Elements tabEls;
    for (size_t i = 0; i < tabs_.size(); ++i) {
        auto label = text(" " + tabs_[i].title + " ");
        if (static_cast<int>(i) == activeTab_) {
            label = label | bgcolor(theme.buttonActiveBgColor)
                    | color(theme.buttonActiveTextColor) | bold;
        } else {
            label = label | color(theme.hintColor);
        }
        tabEls.push_back(label | reflect(tabBoxes_[i]));
    }
    auto tabBar = hbox(std::move(tabEls)) | xframe;

    std::function<Element()> footerRender;
    if (activeTab_ >= 0 && activeTab_ < static_cast<int>(tabs_.size())) {
        footerRender = tabs_[activeTab_].footer;
    }

    Elements layout;
    layout.push_back(tabBar);
    layout.push_back(text(" "));
    layout.push_back(hbox({text(" "), scrollable_->Render() | flex, text(" ")}) | flex);
    if (footerRender) {
        layout.push_back(
            hbox({text(" "), footerRender() | flex, text(" ")}) | xframe | reflect(footerBox_)
        );
        layout.push_back(text(" "));
    }

    auto handle = separatorStyled(BorderStyle::LIGHT) | color(theme.inputBgColor)
                  | reflect(handleBox_);

    return hbox({
               handle,
               vbox(std::move(layout)) | flex,
           })
           | size(WIDTH, EQUAL, width_) | bgcolor(theme.blockColor);
}

bool SidebarComponent::OnEvent(Event event) {
    if (!event.is_mouse()) {
        return false;
    }
    const auto& mouse = event.mouse();
    if (handleResizeMouse(mouse)) {
        ctx_.postRedraw();
        return true;
    }
    if (handleTabMouse(mouse)) {
        ctx_.postRedraw();
        return true;
    }
    // footer 区域点击 (如 "上下文" 按钮)
    if (onFooterClick_ && mouse.button == Mouse::Left && mouse.motion == Mouse::Released
        && footerBox_.Contain(mouse.x, mouse.y)) {
        if (onFooterClick_(mouse)) {
            ctx_.postRedraw();
            return true;
        }
    }
    return scrollable_->OnEvent(event);
}

bool SidebarComponent::handleResizeMouse(const Mouse& mouse) {
    if (resizing_) {
        if (mouse.motion == Mouse::Released) {
            resizing_ = false;
            return true;
        }
        const int newWidth = resizeStartW_ + (resizeStartX_ - mouse.x);
        const int screenW  = Terminal::Size().dimx;
        const int maxW     = std::min(kMaxWidth, std::max(kMinWidth, screenW - 10));
        width_             = std::clamp(newWidth, kMinWidth, maxW);
        return true;
    }
    if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed
        && handleBox_.Contain(mouse.x, mouse.y)) {
        resizing_     = true;
        resizeStartX_ = mouse.x;
        resizeStartW_ = width_;
        return true;
    }
    return false;
}

bool SidebarComponent::handleTabMouse(const Mouse& mouse) {
    for (size_t i = 0; i < tabs_.size() && i < tabBoxes_.size(); ++i) {
        if (!tabBoxes_[i].Contain(mouse.x, mouse.y)) {
            continue;
        }
        if (mouse.button == Mouse::Left && mouse.motion == Mouse::Released) {
            activeTab_ = static_cast<int>(i);
            scrollable_->setStickToBottom(true);
            return true;
        }
        if (mouse.button == Mouse::Right && mouse.motion == Mouse::Released) {
            removeTab(tabs_[i].id);
            return true;
        }
    }
    return false;
}
