#include "agentxx-client/io/tui/scrollable.h"
#include "ftxui/dom/node.hpp"
#include "ftxui/dom/requirement.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/util/autoreset.hpp"
#include <algorithm>

using namespace ftxui;

namespace {

/// 布局迭代上限 (与 ftxui::Render 内部保持一致)
constexpr int kMaxLayoutIteration = 20;
/// 测量/布局时给出的"足够大"高度 (换行仅依赖宽度, 高度给足即可读取自然高度)
constexpr int kTallHeight = 1000000;

/// 对元素执行完整迭代布局 (ComputeRequirement + SetBox 多轮直至收敛),
/// 返回其在该宽度下的自然高度 (行)。
///
/// 原理: flexbox/paragraph 的换行高度在布局迭代收敛后体现为 requirement().min_y,
/// 故布局收敛后直接读取 min_y 即为该宽度下的实际高度。
int layoutAndMeasure(const Element& el, Box box) {
    if (!el) {
        return 1;
    }
    Node::Status status;
    el->Check(&status);
    int iteration = 0;
    bool laidOut = false;
    while (status.need_iteration && iteration < kMaxLayoutIteration) {
        el->ComputeRequirement();
        el->SetBox(box);
        laidOut                 = true;
        status.need_iteration = false;
        status.iteration++;
        el->Check(&status);
        ++iteration;
    }
    if (!laidOut) {
        el->ComputeRequirement();
        el->SetBox(box);
    }
    return std::max(1, el->requirement().min_y);
}

/// 传递给 ListView 布局节点的可变状态 (均指向 Scrollable 的成员, 地址稳定)
struct ListViewState {
    const std::vector<ScrollItem>* items;
    std::vector<int>*              heights;
    int*                           scrollOffset;
    bool*                          stickToBottom;
    int*                           totalHeight;
    int*                           viewportHeight;
    int*                           measuredWidth;
    std::vector<ftxui::Box>*       visibleBoxes;
};

/// 仿 Flutter ListView 的 viewport 布局节点。
///
/// 与 ftxui 内置 yframe+focusPositionRelative 方案 (需全量布局整列表) 不同,
/// 本节点:
/// - ComputeRequirement 不递归子项 (父级以 |flex 撑满, 不依赖 min_x/min_y)
/// - SetBox 阶段仅布局与可见区域相交的子项 (按缓存高度绝对定位), 其余跳过
/// - Render 阶段仅绘制可见子项, 局部超出视口的部分由 screen.stencil 裁剪
class ListView : public Node {
public:

    explicit ListView(ListViewState st) :
        st_(st) {}

    void ComputeRequirement() override {
        // 父级以 |flex 撑满视口, 不依赖内容的 min_x/min_y; 不递归子项 (惰性)
        requirement_         = Requirement{};
        requirement_.min_x = 0;
        requirement_.min_y = 0;
    }

    void SetBox(Box box) override {
        Node::SetBox(box);
        const int vw = box.x_max - box.x_min + 1;
        const int vh = box.y_max - box.y_min + 1;
        if (vw <= 0 || vh <= 0) {
            return;
        }
        *st_.viewportHeight = vh;

        // 预留 1 列滚动条 gutter (与 vscroll_indicator 行为一致), 内容宽度相应减 1
        const bool hasGutter      = vw >= 2;
        const int  contentWidth   = hasGutter ? vw - 1 : vw;
        const int  contentXMax    = hasGutter ? box.x_max - 1 : box.x_max;

        auto& items   = *st_.items;
        auto& heights = *st_.heights;

        // 内容宽度变化 -> 所有高度缓存失效 (换行结果随宽度变化)
        if (contentWidth != *st_.measuredWidth) {
            std::fill(heights.begin(), heights.end(), -1);
            *st_.measuredWidth = contentWidth;
        }
        if (heights.size() != items.size()) {
            heights.assign(items.size(), -1);
        }

        // === Pass 1: 测量所有待测高度 (惰性: 仅子项变化/宽度变化时) ===
        // 记录本帧刚完成测量的子项索引; 这些子项的布局已收敛 (同宽度),
        // Pass 2 中仅需 SetBox 重定位, 无需再次完整迭代布局
        std::vector<size_t> freshlyMeasured;
        for (size_t i = 0; i < items.size(); ++i) {
            if (heights[i] >= 0) {
                continue;
            }
            if (items[i].fillViewport) {
                heights[i] = vh;
                continue;
            }
            Box measureBox{0, contentWidth - 1, 0, kTallHeight};
            heights[i] = layoutAndMeasure(items[i].element, measureBox);
            freshlyMeasured.push_back(i);
        }
        // 用 bool 标记便于 Pass 2 O(1) 查询
        std::vector<bool> justMeasured(items.size(), false);
        for (size_t idx : freshlyMeasured) {
            justMeasured[idx] = true;
        }

        // === 总高度 + 滚动偏移 (stickToBottom / clamp) ===
        int total = 0;
        for (int h : heights) {
            total += std::max(0, h);
        }
        *st_.totalHeight = total;

        const int maxOffset = std::max(0, total - vh);
        if (*st_.stickToBottom) {
            *st_.scrollOffset = maxOffset;
        }
        *st_.scrollOffset  = std::clamp(*st_.scrollOffset, 0, maxOffset);
        const int scrollOffset = *st_.scrollOffset;

        // === Pass 2: 定位并布局可见子项 ===
        st_.visibleBoxes->assign(items.size(), Box{0, -1, 0, -1});
        visibleIndices_.clear();

        int cum = 0; // 累计高度 (当前子项的内容顶边, 行)
        for (size_t i = 0; i < items.size(); ++i) {
            const int h      = std::max(0, heights[i]);
            const int top    = cum;
            const int bottom = cum + h; // 不含
            cum              = bottom;

            if (bottom <= scrollOffset) {
                continue; // 完全在可见区上方
            }
            if (top >= scrollOffset + vh) {
                break; // 完全在可见区下方 (后续更靠下, 提前结束)
            }

            // 与可见区相交 -> 布局该子项 (绝对屏幕坐标)
            const int screenY = box.y_min + (top - scrollOffset);
            Box       itemBox;
            itemBox.x_min = box.x_min;
            itemBox.x_max = contentXMax;
            itemBox.y_min = screenY;
            itemBox.y_max = screenY + h - 1;
            // 本帧刚完成测量的子项: 布局已收敛 (同宽度), 仅 SetBox 重定位即可,
            // 避免重复完整迭代布局; 其余 (上帧已测/缓存命中) 需完整布局
            if (justMeasured[i]) {
                items[i].element->SetBox(itemBox);
            } else {
                layoutAndMeasure(items[i].element, itemBox);
            }

            (*st_.visibleBoxes)[i] = Box::Intersection(itemBox, box);
            visibleIndices_.push_back(i);
        }

        drawScrollbarInfo_ = hasGutter && (total > vh);
        scrollbarX_        = box.x_max;
        scrollbarTotal_    = total;
        scrollbarOffset_   = scrollOffset;
    }

    void Render(Screen& screen) override {
        // 裁剪到视口: 局部超出视口的子项内容经 CellAt 的 stencil 检查被丢弃
        const AutoReset<Box> stencil(&screen.stencil, Box::Intersection(box_, screen.stencil));
        const auto&          items = *st_.items;
        for (size_t i : visibleIndices_) {
            items[i].element->Render(screen);
        }
        if (drawScrollbarInfo_) {
            drawScrollbar(screen);
        }
    }

private:

    void drawScrollbar(Screen& screen) {
        const int vh = box_.y_max - box_.y_min + 1;
        if (vh <= 0 || scrollbarTotal_ <= vh) {
            return;
        }
        // 半行精度的 thumb 高度与起始位置 (算法同 ftxui vscroll_indicator)
        int thumbSize = 2 * vh * vh / scrollbarTotal_;
        thumbSize     = std::max(thumbSize, 1);
        const int start = 2 * scrollbarOffset_ * vh / scrollbarTotal_;
        const int x     = scrollbarX_;
        for (int y = box_.y_min; y <= box_.y_max; ++y) {
            const int yUp   = 2 * (y - box_.y_min) + 0;
            const int yDown = 2 * (y - box_.y_min) + 1;
            const bool up   = (start <= yUp) && (yUp <= start + thumbSize);
            const bool down = (start <= yDown) && (yDown <= start + thumbSize);
            const char* c   = up ? (down ? "┃" : "╹") : (down ? "╻" : " ");
            screen.CellAt(x, y).character = c;
        }
    }

    ListViewState         st_;
    std::vector<size_t>   visibleIndices_;
    bool                  drawScrollbarInfo_ = false;
    int                   scrollbarX_        = 0;
    int                   scrollbarTotal_    = 0;
    int                   scrollbarOffset_   = 0;
};

} // namespace

ftxui::Element Scrollable::OnRender() {
    auto items = render_();
    const size_t n = items.size();

    // 同步缓存大小: resize 保留已有条目 (仅新增项置空/待测),
    // 避免列表增长时把全部子项误判为变化而全量重测高度
    cachedElements_.resize(n);
    cachedFill_.resize(n);
    heights_.resize(n, -1);
    for (size_t i = 0; i < n; ++i) {
        if (cachedElements_[i] != items[i].element || cachedFill_[i] != items[i].fillViewport) {
            heights_[i]        = -1;
            cachedElements_[i] = items[i].element;
            cachedFill_[i]     = items[i].fillViewport;
        }
    }
    items_ = std::move(items);
    visibleBoxes_.assign(n, ftxui::Box{0, -1, 0, -1});

    ListViewState st;
    st.items          = &items_;
    st.heights        = &heights_;
    st.scrollOffset   = &scrollOffset_;
    st.stickToBottom  = &stickToBottom_;
    st.totalHeight    = &totalHeight_;
    st.viewportHeight = &viewportHeight_;
    st.measuredWidth  = &measuredWidth_;
    st.visibleBoxes   = &visibleBoxes_;

    return std::make_shared<ListView>(st) | ftxui::reflect(box_);
}

bool Scrollable::OnEvent(ftxui::Event event) {
    if (!event.is_mouse()) {
        return false;
    }
    const auto& mouse = event.mouse();
    if (!box_.Contain(mouse.x, mouse.y)) {
        return false;
    }
    // 固定每次滚动 1 行高度
    if (mouse.button == ftxui::Mouse::WheelUp) {
        stickToBottom_ = false;
        scrollOffset_  = std::max(0, scrollOffset_ - 1);
        return true;
    }
    if (mouse.button == ftxui::Mouse::WheelDown) {
        const int maxOffset = std::max(0, totalHeight_ - viewportHeight_);
        scrollOffset_       = std::min(maxOffset, scrollOffset_ + 1);
        if (scrollOffset_ >= maxOffset) {
            stickToBottom_ = true; // 滚到底部 -> 恢复吸附
        }
        return true;
    }
    return false;
}
