#include "agentxx-client/io/tui/framework/ui_action_list.h"
#include "ftxui/component/mouse.hpp"
#include <algorithm>
#include <cstddef>
#include <utility>

using namespace ftxui;

namespace agentxx {
namespace client {

UiActionStyle UiActionStyle::fromTheme(const TUITheme& theme) {
    return UiActionStyle{
        .normalFg   = theme.normalColor,
        .normalBg   = Color::Default,
        .selectedFg = theme.buttonActiveTextColor,
        .selectedBg = theme.buttonActiveBgColor,
        .valueFg    = theme.accentColor,
        .disabledFg = theme.hintColor,
    };
}

Element UiActionStyle::row(const UiActionItem& item, bool selected) const {
    Elements children;
    children.push_back(text(item.label));
    if (!item.hint.empty()) {
        children.push_back(text("  "));
        children.push_back(text(item.hint));
    }
    // filler 撑满整行: 选中项背景色条覆盖整行 (左右留白由外框统一提供)
    children.push_back(filler());
    if (!item.value.empty()) {
        children.push_back(text(item.value) | color(selected ? selectedFg : valueFg));
    }

    Element row = hbox(std::move(children));
    if (!item.enabled) {
        return row | color(disabledFg);
    }
    if (selected) {
        // focus: 交给父级 yframe/vscroll_indicator 自动把选中项滚入视口
        return row | bgcolor(selectedBg) | color(selectedFg) | focus;
    }
    row = row | color(normalFg);
    // 背景仅在显式指定时着色: `bgcolor(Color::Default)` 会把单元格背景重置为
    // 终端默认色, 抹掉父级 (弹窗内容区) 的背景, 形成镂空
    if (normalBg != Color::Default) {
        row = row | bgcolor(normalBg);
    }
    return row;
}

// ---------------------------------------------------------------------------
// UiActionList
// ---------------------------------------------------------------------------

void UiActionList::setItems(std::vector<UiActionItem> items) {
    items_ = std::move(items);

    // 选中项按 id 保持: 列表重建 (每帧/数据刷新) 后仍指向同一个条目
    if (!selectedId_.empty()) {
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].id == selectedId_) {
                selectedIndex_ = static_cast<int>(i);
                return;
            }
        }
    }
    if (items_.empty()) {
        selectedIndex_ = 0;
        selectedId_.clear();
        return;
    }
    // 原选中项已消失: 原下标收敛到有效范围
    selectedIndex_ = std::clamp(selectedIndex_, 0, static_cast<int>(items_.size()) - 1);
    selectedId_    = items_[static_cast<size_t>(selectedIndex_)].id;
}

int UiActionList::selectedIndex() const {
    return items_.empty() ? -1 : selectedIndex_;
}

void UiActionList::setSelectedIndex(int index) {
    if (items_.empty()) {
        return;
    }
    const int next = std::clamp(index, 0, static_cast<int>(items_.size()) - 1);
    if (next == selectedIndex_ && !selectedId_.empty()) {
        return;
    }
    selectedIndex_ = next;
    selectedId_    = items_[static_cast<size_t>(next)].id;
    if (onSelectionChanged_) {
        onSelectionChanged_(next);
    }
}

void UiActionList::selectById(std::string_view id) {
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].id == id) {
            setSelectedIndex(static_cast<int>(i));
            return;
        }
    }
}

const UiActionItem* UiActionList::selected() const {
    if (items_.empty()) {
        return nullptr;
    }
    return &items_[static_cast<size_t>(selectedIndex_)];
}

bool UiActionList::activateIndex(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return false;
    }
    const auto& item = items_[static_cast<size_t>(index)];
    if (!item.enabled) {
        return false;
    }
    if (item.onActivate) {
        item.onActivate();
    }
    return true;
}

void UiActionList::moveSelection(int delta) {
    if (items_.empty() || delta == 0) {
        return;
    }
    // 只在可激活项之间移动 (disabled 项跳过); 越界停在边界, 不循环
    for (int i  = selectedIndex_ + delta; i >= 0 && i < static_cast<int>(items_.size());
         i     += delta) {
        if (items_[static_cast<size_t>(i)].enabled) {
            setSelectedIndex(i);
            return;
        }
    }
}

void UiActionList::moveSelectionEdge(bool toLast) {
    if (items_.empty()) {
        return;
    }
    const int last = static_cast<int>(items_.size()) - 1;
    for (int i = toLast ? last : 0; i >= 0 && i <= last; i += (toLast ? -1 : 1)) {
        if (items_[static_cast<size_t>(i)].enabled) {
            setSelectedIndex(i);
            return;
        }
    }
}

bool UiActionList::onKeyEvent(const Event& event) {
    if (event == Event::ArrowUp) {
        moveSelection(-1);
        return true;
    }
    if (event == Event::ArrowDown) {
        moveSelection(+1);
        return true;
    }
    if (event == Event::Home) {
        moveSelectionEdge(false);
        return true;
    }
    if (event == Event::End) {
        moveSelectionEdge(true);
        return true;
    }
    if (event == Event::Return) {
        activateIndex(selectedIndex());
        return true;
    }
    return false;
}

bool UiActionList::onMouseEvent(const Mouse& mouse, UiHitMap& hits) {
    const UiHitMap::Entry* hit = hits.findClick(mouse);
    if (hit == nullptr) {
        return false;
    }
    // 命中项的 id 即条目 id: 按 id 定位 (行下标在条目表重建后可能变化)
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].id != hit->payload.id) {
            continue;
        }
        if (!items_[i].enabled) {
            return false;
        }
        setSelectedIndex(static_cast<int>(i));
        if (items_[i].onActivate) {
            items_[i].onActivate();
        }
        return true;
    }
    return false;
}

Element
    UiActionList::render(UiHitMap& hits, const UiActionStyle& style, RowBuilder rowBuilder) const {
    Elements rows;
    rows.reserve(items_.size());
    for (size_t i = 0; i < items_.size(); ++i) {
        if (i > 0 && rowGap_ > 0) {
            for (int g = 0; g < rowGap_; ++g) {
                rows.push_back(text(""));
            }
        }
        const auto& item     = items_[i];
        const bool  selected = (static_cast<int>(i) == selectedIndex_);
        Element     row = rowBuilder ? rowBuilder(item, selected, i) : style.row(item, selected);
        // 命中登记: 只有渲染出来的条目才会命中 (列表为空/条目被裁剪即无命中)
        rows.push_back(hits.add(std::move(row), item.id, std::string{}));
    }
    return vbox(std::move(rows));
}

} // namespace client
} // namespace agentxx
