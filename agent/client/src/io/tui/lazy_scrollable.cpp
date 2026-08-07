#include "agentxx-client/io/tui/lazy_scrollable.h"
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
    int  iteration = 0;
    bool laidOut   = false;
    while (status.need_iteration && iteration < kMaxLayoutIteration) {
        el->ComputeRequirement();
        el->SetBox(box);
        laidOut               = true;
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

} // namespace

/// 视口布局节点: SetBox/Render 委托给 LazyScrollable 组件。
///
/// 与 ftxui 内置 yframe+focusPositionRelative 方案 (需全量布局整列表) 不同:
/// - ComputeRequirement 不递归子项 (父级以 |flex 撑满, 不依赖 min_x/min_y)
/// - SetBox 阶段仅构建/布局与可见区域相交的子项 (懒构建 + 高度缓存), 其余跳过
/// - Render 阶段仅绘制可见子项, 局部超出视口的部分由 screen.stencil 裁剪
class LazyScrollable::ListViewNode : public Node {
public:

    explicit ListViewNode(LazyScrollable* comp) :
        comp_(comp) {}

    void ComputeRequirement() override {
        // 父级以 |flex 撑满视口, 不依赖内容的 min_x/min_y; 不递归子项 (惰性)
        requirement_       = Requirement{};
        requirement_.min_x = 0;
        requirement_.min_y = 0;
    }

    void SetBox(Box box) override {
        Node::SetBox(box);
        comp_->prepareLayout(box);
    }

    void Render(Screen& screen) override {
        // 裁剪到视口: 局部超出视口的子项内容经 CellAt 的 stencil 检查被丢弃
        const AutoReset<Box> stencil(&screen.stencil, Box::Intersection(box_, screen.stencil));
        comp_->renderVisible(screen);
    }

private:

    LazyScrollable* comp_;
};

LazyScrollable::LazyScrollable(
    ItemCountFunc      itemCount,
    ItemKeyFunc        itemKey,
    EstimateHeightFunc estimateHeight,
    BuildFunc          buildItem,
    CacheBudget        budget,
    FillViewportFunc   fillViewport
) :
    itemCount_(std::move(itemCount)),
    itemKey_(std::move(itemKey)),
    estimateHeight_(std::move(estimateHeight)),
    buildItem_(std::move(buildItem)),
    fillViewport_(std::move(fillViewport)),
    budget_(budget) {}

void LazyScrollable::clearCache() {
    lruList_.clear();
    cachedBytes_ = 0;
    // vector<bool> 的代理引用不能绑定 bool&, 用 auto&&
    for (auto&& f : hasCache_) {
        f = false;
    }
}

ftxui::Element LazyScrollable::OnRender() {
    ++frameSeq_; // 帧边界: 不可缓存项按帧重建 (见 ensureElement)
    return std::make_shared<ListViewNode>(this) | ftxui::reflect(box_);
}

bool LazyScrollable::OnEvent(ftxui::Event event) {
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

int LazyScrollable::estimateHeightFor(size_t index) const {
    // measuredWidth_ 首帧布局前为 -1; 由 estimateHeight 回调自行兜底默认宽度
    const int h = estimateHeight_ ? estimateHeight_(index, measuredWidth_) : 1;
    return std::max(1, h);
}

ftxui::Element& LazyScrollable::elementAt(size_t index) {
    if (index < hasCache_.size() && hasCache_[index]) {
        return itemCache_[index]->element;
    }
    for (auto& entry : transientItems_) {
        if (entry.index == index) {
            return entry.item.element;
        }
    }
    // 理论上不可达 (调用前必已 ensureElement); 兜底避免空指针崩溃
    static ftxui::Element fallback = ftxui::text("");
    return fallback;
}

void LazyScrollable::ensureElement(size_t index) {
    if (index < hasCache_.size() && hasCache_[index]) {
        // 缓存命中: LRU 提前 (最近使用)
        lruList_.splice(lruList_.begin(), lruList_, itemCache_[index]);
        return;
    }
    // 不可缓存项按帧复用: 同一帧内多次布局迭代 (layoutAndMeasure 收敛循环)
    // 不重复调用 buildItem (如流式 markdown 解析开销大); 跨帧复用由
    // prepareLayout 开头的帧边界清理保证
    for (auto& entry : transientItems_) {
        if (entry.index == index) {
            return;
        }
    }

    LazyBuiltItem built = buildItem_(index);
    if (!built.element) {
        built.element = ftxui::text("");
    }
    if (!built.cacheable) {
        transientItems_.push_back(TransientEntry{index, std::move(built)});
        return;
    }
    Entry entry;
    entry.index        = index;
    entry.sourceBytes  = built.sourceBytes;
    entry.bytesCounted = (built.sourceBytes > budget_.byteExemptThreshold);
    entry.element      = std::move(built.element);
    entry.attachments  = std::move(built.attachments);
    lruList_.push_front(std::move(entry));
    itemCache_[index] = lruList_.begin();
    if (index < hasCache_.size()) {
        hasCache_[index] = true;
    }
    if (lruList_.front().bytesCounted) {
        cachedBytes_ += lruList_.front().sourceBytes;
    }
    evictIfNeeded();
}

void LazyScrollable::removeCacheAt(size_t index) {
    if (index >= hasCache_.size() || !hasCache_[index]) {
        return;
    }
    auto it = itemCache_[index];
    if (it->bytesCounted && cachedBytes_ >= it->sourceBytes) {
        cachedBytes_ -= it->sourceBytes;
    }
    lruList_.erase(it);
    hasCache_[index] = false;
}

void LazyScrollable::evictIfNeeded() {
    // 从 LRU 尾部 (最久未使用) 淘汰, 直至满足条数与字节双预算。
    // 可见子项在本帧均已被 splice/push 至头部, 不会被误淘汰。
    while (!lruList_.empty()
           && (lruList_.size() > budget_.maxItems || cachedBytes_ > budget_.maxBytes)) {
        auto& back = lruList_.back();
        if (back.index < hasCache_.size()) {
            hasCache_[back.index] = false;
        }
        if (back.bytesCounted && cachedBytes_ >= back.sourceBytes) {
            cachedBytes_ -= back.sourceBytes;
        }
        lruList_.pop_back();
    }
}

void LazyScrollable::prepareLayout(const ftxui::Box& box) {
    box_ = box;
    visibleIndices_.clear();
    visibleBoxes_.clear();

    // 帧边界: 清空上一帧的不可缓存项 (如流式增量 Element), 及时释放内存。
    // (不能等下一次 transient 构建才清理 —— 流式结束后最后一帧的大体积
    //  markdown Element 会一直驻留到下一轮流式输出)
    if (lastPreparedFrame_ != frameSeq_) {
        transientItems_.clear();
        lastPreparedFrame_ = frameSeq_;
    }

    const int vw = box.x_max - box.x_min + 1;
    const int vh = box.y_max - box.y_min + 1;
    if (vw <= 0 || vh <= 0) {
        viewportHeight_ = 0;
        return;
    }
    viewportHeight_ = vh;

    // 预留 1 列滚动条 gutter (与 vscroll_indicator 行为一致), 内容宽度相应减 1
    hasGutter_             = vw >= 2;
    const int contentWidth = hasGutter_ ? vw - 1 : vw;
    contentXMax_           = hasGutter_ ? box.x_max - 1 : box.x_max;

    const size_t count = itemCount_();
    visibleBoxes_.assign(count, ftxui::Box{0, -1, 0, -1});

    // 内容宽度变化 -> 换行结果失效: 清空缓存, 高度全部重算
    if (contentWidth != measuredWidth_) {
        clearCache();
        for (auto& h : heights_) {
            h = -1;
        }
        for (auto&& m : measured_) {
            m = false;
        }
        measuredWidth_ = contentWidth;
    }

    // 尾部收缩 (列表变短, 如清空会话): 移除被裁掉子项的缓存, 及时释放内存
    if (count < heights_.size()) {
        for (size_t i = count; i < heights_.size(); ++i) {
            removeCacheAt(i);
        }
    }
    heights_.resize(count, -1);
    measured_.resize(count, false);
    keys_.resize(count, 0);
    hasCache_.resize(count, false);
    itemCache_.resize(count);

    // key 变化 -> 内容变化: 使该子项缓存失效并重算估算高度。
    // key 未变的子项零成本 (不读内容、不重建)
    for (size_t i = 0; i < count; ++i) {
        const uint64_t k = itemKey_(i);
        if (k != keys_[i]) {
            keys_[i] = k;
            removeCacheAt(i);
            heights_[i]  = -1;
            measured_[i] = false;
        }
        if (fillViewport_ && fillViewport_(i)) {
            // 占据整个视口的特殊项 (空状态居中展示): 高度恒为视口高度
            heights_[i]  = vh;
            measured_[i] = true;
        } else if (heights_[i] < 0) {
            heights_[i] = estimateHeightFor(i);
        }
    }

    // === 总高度 + 滚动偏移 (stickToBottom / clamp) ===
    // 未测量子项使用估算高度, 可见后实测修正
    int total = 0;
    for (size_t i = 0; i < count; ++i) {
        total += std::max(1, heights_[i]);
    }
    totalHeight_ = total;

    const int maxOffset = std::max(0, total - vh);
    if (stickToBottom_) {
        scrollOffset_ = maxOffset;
    }
    scrollOffset_          = std::clamp(scrollOffset_, 0, maxOffset);
    const int scrollOffset = scrollOffset_;

    // === 构建并布局可见子项 (视口局部) ===
    int  cum       = 0;     // 累计高度 (当前子项的内容顶边, 行)
    bool corrected = false; // 是否有估算高度被实测修正
    for (size_t i = 0; i < count; ++i) {
        const int h   = std::max(1, heights_[i]);
        const int top = cum;
        if (top >= scrollOffset + vh) {
            break; // 完全在可见区下方 (后续更靠下, 提前结束)
        }
        cum += h;
        if (cum <= scrollOffset) {
            continue; // 完全在可见区上方
        }

        // 与可见区相交 -> 构建 (缓存命中或 buildItem) 并布局该子项
        ensureElement(i);
        const int screenY = box.y_min + (top - scrollOffset);
        Box       itemBox{box.x_min, contentXMax_, screenY, screenY + h - 1};
        if (!measured_[i]) {
            // 首次布局: 完整迭代布局并测量自然高度, 修正估算值
            const Box measureBox{0, contentWidth - 1, 0, kTallHeight};
            const int realH = layoutAndMeasure(elementAt(i), measureBox);
            heights_[i]     = std::max(1, realH);
            measured_[i]    = true;
            if (heights_[i] != h) {
                corrected = true;
            }
            // 以实测高度重定位 (测量时同宽度布局已收敛, 仅 SetBox 即可)
            itemBox.y_max = screenY + heights_[i] - 1;
            elementAt(i)->SetBox(itemBox);
            cum = top + heights_[i];
        } else {
            layoutAndMeasure(elementAt(i), itemBox);
        }
        visibleIndices_.push_back(i);
        visibleBoxes_[i] = Box::Intersection(itemBox, box);
    }

    // 估算被修正 -> 刷新总高度; 吸附模式下同步修正滚动偏移 (下帧定位完全一致)
    if (corrected) {
        int t = 0;
        for (size_t i = 0; i < count; ++i) {
            t += std::max(1, heights_[i]);
        }
        totalHeight_ = t;
        if (stickToBottom_) {
            scrollOffset_ = std::max(0, t - vh);
        }
    }
}

void LazyScrollable::renderVisible(ftxui::Screen& screen) {
    // 仅绘制可见子项 (不可见子项未构建/零成本)
    for (size_t i : visibleIndices_) {
        elementAt(i)->Render(screen);
    }
    if (hasGutter_ && viewportHeight_ > 0 && totalHeight_ > viewportHeight_) {
        drawScrollbar(screen);
    }
}

void LazyScrollable::drawScrollbar(ftxui::Screen& screen) {
    // 半行精度的 thumb 高度与起始位置 (算法同 ftxui vscroll_indicator)
    const int vh        = viewportHeight_;
    int       thumbSize = 2 * vh * vh / totalHeight_;
    thumbSize           = std::max(thumbSize, 1);
    const int start     = 2 * scrollOffset_ * vh / totalHeight_;
    const int x         = box_.x_max;
    for (int y = box_.y_min; y <= box_.y_max; ++y) {
        const int   yUp   = 2 * (y - box_.y_min);
        const int   yDown = yUp + 1;
        const bool  up    = (start <= yUp) && (yUp <= start + thumbSize);
        const bool  down  = (start <= yDown) && (yDown <= start + thumbSize);
        const char* c     = up ? (down ? "┃" : "╹") : (down ? "╻" : " ");
        screen.CellAt(x, y).character = c;
    }
}
