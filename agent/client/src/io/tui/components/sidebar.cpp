#include "agentxx-client/io/tui/components/sidebar.h"
#include "fmt/format.h"
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
        return {
            ScrollItem{text(" "), false}
        };
    });
    // tabs 竖向列表: 内容自顶部展示 (不吸附底部), 超出显示高度时可滚动
    tabList_ = std::make_shared<Scrollable>([this]() -> std::vector<ScrollItem> {
        return pendingListItems_; // OnRender 每帧先经 buildTabList() 构建
    });
    tabList_->setStickToBottom(false);
    Add(scrollable_);
    Add(tabList_);
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

int SidebarComponent::findTabIndex(std::string_view id) const {
    for (size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool SidebarComponent::isPinned(std::string_view id) const {
    for (const auto& pin : pinned_) {
        if (pin.id == id) {
            return true;
        }
    }
    return false;
}

void SidebarComponent::buildTabList() {
    pendingListItems_.clear();
    listEntries_.clear();

    // 按钮行: 文本 + filler 填满列表宽度, 使激活项呈整行高亮
    auto pushButton = [&](bool active, std::string_view title, ListEntry entry) {
        listEntries_.push_back(entry);
        auto row = hbox({
            text(fmt::format(" {} ", title)),
            filler(),
        });
        if (active) {
            row = row | bgcolor(ctx_.theme->buttonActiveBgColor)
                  | color(ctx_.theme->buttonActiveTextColor) | bold;
        } else {
            row = row | color(ctx_.theme->hintColor);
        }
        pendingListItems_.push_back(ScrollItem{std::move(row), false});
    };

    // 条目顺序: 常驻标签 (注册序, 固定顶部) + 动态 tab (添加序);
    // 已作为 tab 存在的常驻标签只显示一次 (位于常驻区)
    for (size_t p = 0; p < pinned_.size(); ++p) {
        const auto& pin   = pinned_[p];
        const int   tabIx = findTabIndex(pin.id);
        pushButton(
            tabIx >= 0 && tabIx == activeTab_,
            pin.title,
            ListEntry{true, static_cast<int>(p)}
        );
    }
    for (size_t i = 0; i < tabs_.size(); ++i) {
        if (isPinned(tabs_[i].id)) {
            continue;
        }
        pushButton(
            static_cast<int>(i) == activeTab_,
            tabs_[i].title,
            ListEntry{false, static_cast<int>(i)}
        );
    }
}

Element SidebarComponent::OnRender() {
    const auto& theme = *ctx_.theme;

    buildTabList();

    // 列表自然宽度: 最长按钮行的显示宽 (含双宽字符, 经 ComputeRequirement 读取),
    // +1 列滚动条 gutter (内容超高出现滚动条时不挤压按钮文本)
    Elements measureEls;
    measureEls.reserve(pendingListItems_.size());
    for (auto& item : pendingListItems_) {
        measureEls.push_back(item.element);
    }
    auto measureNode = vbox(std::move(measureEls));
    measureNode->ComputeRequirement();
    const int listW = measureNode->requirement().min_x + 1;

    // tabList_ (Scrollable/ListView) 按设计 requirement min_y=0, 依赖父级以
    // flex 分配空间 (见 scrollable.h) —— 此处必须加 |flex 占据 vbox 剩余高度,
    // 否则列表被分配 0 行, 常驻标签 (Info/Logs) 与 tab 按钮全部不可见
    auto tabBar = vbox({
                       text(" "),
                       tabList_->Render() | flex,
                       text(" "),
                   })
                   | size(WIDTH, EQUAL, listW);

    auto handle
        = separatorStyled(BorderStyle::LIGHT) | color(theme.inputBgColor) | reflect(handleBox_);

    // ===== 左侧: 当前高亮 tab 的内容 (无高亮 tab 则不渲染, 仅剩 tabs 列表) =====
    const bool hasActive = activeTab_ >= 0 && activeTab_ < static_cast<int>(tabs_.size());
    if (!hasActive) {
        // 无 footer 渲染: 清空命中区, 避免残留旧区域导致误触
        footerBox_ = Box{0, -1, 0, -1};
        return hbox({
                   handle,
                   std::move(tabBar),
               })
               | bgcolor(theme.blockColor);
    }

    Elements layout;
    layout.push_back(text(" "));
    layout.push_back(hbox({text(" "), scrollable_->Render() | flex, text(" ")}) | flex);
    if (tabs_[activeTab_].footer) {
        layout.push_back(
            hbox({text(" "), tabs_[activeTab_].footer() | flex, text(" ")}) | xframe
            | reflect(footerBox_)
        );
        layout.push_back(text(" "));
    } else {
        footerBox_ = Box{0, -1, 0, -1};
    }

    auto contentSep = separatorStyled(BorderStyle::LIGHT) | color(theme.inputBgColor);

    return hbox({
               handle,
               vbox(std::move(layout)) | flex,
               contentSep,
               std::move(tabBar),
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
    if (handleListMouse(mouse)) {
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
    if (scrollable_->OnEvent(event)) {
        return true;
    }
    return tabList_->OnEvent(event);
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

bool SidebarComponent::handleListMouse(const Mouse& mouse) {
    const auto& boxes = tabList_->visibleBoxes();
    for (size_t i = 0; i < listEntries_.size() && i < boxes.size(); ++i) {
        if (!boxes[i].Contain(mouse.x, mouse.y)) {
            continue;
        }
        const bool isPin = listEntries_[i].isPin;
        const int  idx   = listEntries_[i].index;

        if (mouse.button == Mouse::Left && mouse.motion == Mouse::Released) {
            if (isPin) {
                const auto& pin   = pinned_[idx];
                const int   tabIx = findTabIndex(pin.id);
                if (tabIx < 0) {
                    // 对应 tab 未创建: 经 ensure 回调创建 (addTab 自动激活)
                    if (pin.ensure) {
                        pin.ensure();
                    }
                } else if (tabIx == activeTab_) {
                    activeTab_ = -1; // 已激活再点一次: 取消激活, 内容区隐藏
                } else {
                    activeTab_ = tabIx;
                    scrollable_->setStickToBottom(true);
                }
            } else {
                activeTab_ = idx;
                scrollable_->setStickToBottom(true);
            }
            return true;
        }
        if (mouse.button == Mouse::Right && mouse.motion == Mouse::Released) {
            if (isPin) {
                // 常驻标签不可移除: 已激活则仅取消激活
                const int tabIx = findTabIndex(pinned_[idx].id);
                if (tabIx >= 0 && tabIx == activeTab_) {
                    activeTab_ = -1;
                }
            } else {
                removeTab(tabs_[idx].id);
            }
            return true;
        }
    }
    return false;
}
