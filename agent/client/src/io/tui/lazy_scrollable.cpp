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

    // 覆写 Node::Select: 本节点的可见子项以可见区域懒构建 (存于
    // visibleIndices_/缓存), 不在 children_ 列表中 —— 默认实现只递归
    // children_, 导致消息列表/空态 banner 的文本不参与 FTXUI 的鼠标选择
    // (拖动选中无反色高亮、GetSelection 收集不到文本)。
    // 与 Render 同帧顺序: Select 于 SetBox->prepareLayout 之后执行,
    // 可见子项已构建并定位 (屏幕坐标), 此处对可见子项逐一递归即可。
    void Select(Selection& selection) override {
        if (Box::Intersection(selection.GetBox(), box_).IsEmpty()) {
            return;
        }
        for (size_t i : comp_->visibleIndices_) {
            auto& el = comp_->elementAt(i);
            if (el) {
                el->Select(selection);
            }
        }
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
    // 缓存清空后本帧重建的 Element 是新对象 (无上帧布局状态), 必须重新布局:
    // 阶段 2 的"缓存命中且 box 相同则跳过布局"优化以元素对象未变为前提,
    // 若沿用旧 lastBoxes_ 判断, 主题切换后新构建的元素会被误判为
    // "box 与上帧一致"而跳过 SetBox —— 元素从未布局 (box_ 未初始化),
    // 渲染位置错误/内容丢失, 且此后每帧都跳过 (scrollOffset 不变则 box 恒同),
    // 列表持续消失直到内容变化 (发送消息) 才恢复。
    // 清空 lastBoxes_ 使下一帧所有可见子项 sameBox=false, 强制重新布局。
    // (宽度变化路径同样调用 clearCache, 但其 heights_/measured_ 已全失效,
    //  全部走 fresh 分支布局, 不依赖 lastBoxes_, 无副作用)
    for (auto& b : lastBoxes_) {
        b = ftxui::Box{0, -1, 0, -1};
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

void LazyScrollable::resetSelectionHighlight() {
    // 对当前可见子项执行 ComputeRequirement:
    // Text::ComputeRequirement 会复位 has_selection_ (见 ftxui text.cpp),
    // 从而清除拖动选中残留的反色高亮。幂等操作, 不影响内容/布局。
    for (size_t i : visibleIndices_) {
        auto& el = elementAt(i);
        if (el) {
            el->ComputeRequirement();
        }
    }
}

size_t LazyScrollable::estimateHeightFor(size_t index) const {
    // measuredWidth_ 首帧布局前为 -1; 由 estimateHeight 回调自行兜底默认宽度
    const size_t h = estimateHeight_ ? estimateHeight_(index, measuredWidth_) : 1;
    return std::max(static_cast<size_t>(1), h);
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
        // 标记为本帧已确保: evictIfNeeded 不得淘汰本条 (本帧仍要渲染)
        if (index < protectedIndices_.size()) {
            protectedIndices_[index] = true;
        }
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
    // 标记为本帧已确保: 本帧阶段 1/2 处理过的条目禁止被 evictIfNeeded 淘汰
    // (淘汰由本条插入触发的 evictIfNeeded 即时执行, 先标记后淘汰才有效)
    if (index < protectedIndices_.size()) {
        protectedIndices_[index] = true;
    }
    // 新构建的元素没有上帧布局状态: 清空其 lastBoxes_, 使阶段 2 的
    // "缓存命中且 box 相同则跳过布局" 判定失效, 强制重新布局 (SetBox)。
    // 否则: key 变化 (单条替换 / onSync 整体重建) 但内容与高度不变的项,
    // 阶段 1 重建了新元素后, 阶段 2 用旧 lastBoxes_ 误判 sameBox=true 跳过
    // 布局 —— 新元素从未 SetBox (box_ = {0,0,0,0}), Text 只画首字符到 (0,0),
    // 表现为消息少开头/整行不显示 (用户报告症状)。
    // 注意: 缓存命中路径 (本函数开头 return) 不清 lastBoxes_, sameBox 判定
    // 正常生效 —— 内容未变 + box 未变时跳过布局是安全的。
    if (index < lastBoxes_.size()) {
        lastBoxes_[index] = ftxui::Box{0, -1, 0, -1};
    }
    if (lruList_.front().bytesCounted) {
        cachedBytes_ += lruList_.front().sourceBytes;
    }
    evictIfNeeded();
}

void LazyScrollable::notifyPrepended(size_t count) {
    if (count == 0) {
        return;
    }
    // 尚未布局过 (首屏填充场景): 无既有视口内容需要稳定, 仅记录条数
    // (prepareLayout 首次布局时新增区按估算高度参与总高, 无偏移校正必要)
    if (heights_.empty() && measuredWidth_ < 0) {
        pendingPrepend_ = PendingPrepend{true, count, 0};
        return;
    }

    const size_t oldSize = heights_.size();
    // 并行数组头部插入 k 个新条目; 既有数据整体后移 —— key 与条目同步平移,
    // 旧条目 key 校验依然匹配 (缓存 Element/实测高度全保留), 仅新增区为初始值
    heights_.insert(heights_.begin(), count, -1);
    measured_.insert(measured_.begin(), count, false);
    keys_.insert(keys_.begin(), count, 0);
    hasCache_.insert(hasCache_.begin(), count, false);
    using ListIt = std::list<Entry>::iterator;
    itemCache_.insert(itemCache_.begin(), count, ListIt{});
    lastBoxes_.insert(lastBoxes_.begin(), count, ftxui::Box{});
    protectedIndices_.insert(protectedIndices_.begin(), count, false);
    (void)oldSize;

    // LRU 缓存条目索引平移 (list 迭代器稳定, 直接改 index 字段即可;
    // itemCache_ 中迭代器的存储位置已随 vector 头插对齐到新索引)
    for (auto& entry : lruList_) {
        entry.index += count;
    }
    // transientItems_ 属于上一帧 (帧边界清理), 索引陈旧无影响

    // 注意: 此处不做估算也不调整滚动偏移 —— 调用方在状态前插后、本帧快照
    // 刷新前调用 (UI 动作队列语义), 此时经回调估算读到的是旧快照内容,
    // 口径必然错误。偏移补偿统一下放到下一帧 prepareLayout 内的
    // applyPrependAnchorCorrection: 该处以新快照口径计算新增区高度,
    // 相对 appliedRows=0 全额下移偏移, 后续实测修正继续增量收敛
    pendingPrepend_ = PendingPrepend{true, count, 0};
}

void LazyScrollable::clearPrependAnchor() {
    pendingPrepend_ = PendingPrepend{};
}

void LazyScrollable::applyPrependAnchorCorrection() {
    if (!pendingPrepend_.active || pendingPrepend_.count == 0) {
        return;
    }
    const size_t n = std::min(pendingPrepend_.count, heights_.size());
    if (n == 0) {
        pendingPrepend_ = PendingPrepend{};
        return;
    }
    // 新增区当前已知总高度: 实测优先, 未测子项沿用估算 (滚动接近时再收敛)
    long long actualRows  = 0;
    bool      allMeasured = true;
    for (size_t i = 0; i < n; ++i) {
        if (!measured_[i]) {
            allMeasured = false;
        }
        actualRows += (heights_[i] >= 0) ? static_cast<long long>(std::max(1, heights_[i]))
                                         : static_cast<long long>(estimateHeightFor(i));
    }
    // 增量补偿: 只应用与已应用值的差值, 多帧多次调用天然幂等收敛
    const long long delta = actualRows - pendingPrepend_.appliedRows;
    if (delta != 0) {
        // stickToBottom 时偏移由吸附接管, 仅同步已应用值避免后续误补偿
        if (!stickToBottom_) {
            scrollOffset_ = std::max(0, scrollOffset_ + static_cast<int>(delta));
        }
        pendingPrepend_.appliedRows = actualRows;
    }
    if (allMeasured) {
        // 全部实测完成, 校正收敛结束
        pendingPrepend_ = PendingPrepend{};
    }
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
        // 本帧已确保的子项 (阶段 1/2 处理过, 含可见项与容错区项) 禁止淘汰:
        // 其索引已进入/将进入 visibleIndices_, 渲染时经 elementAt 取缓存;
        // 若被淘汰, 索引残留在 visibleIndices_ (命中盒正常 -> 可点击), 但
        // 缓存缺失时 elementAt 回退空 text -> 连续多条消息显示为空白。
        // LRU 序 = 最近使用在前, 本帧确保过的条目全部位于前部; 从尾部
        // 淘汰先遇到未确保 (视口外旧缓存) 项, 遇到首个已确保项即停止。
        // 预算仍超限说明"可见集自身"超预算 (长内容/高终端), 此时保可见集
        // 优先于压预算 (多余内存由可见集大小界定, 移出窗口即被淘汰释放)。
        if (back.index < protectedIndices_.size() && protectedIndices_[back.index]) {
            break;
        }
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
    lastBoxes_.resize(count);
    // 本布局遍清空"已确保"保护标记 (本遍 ensureElement 重新标记):
    // prepareLayout 在 FTXUI 布局迭代中可能同帧重入多次, 每遍都从零
    // 重新标记可见集; 两遍之间无缓存淘汰发生 (淘汰仅在 ensureElement 内)
    protectedIndices_.assign(count, false);

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
            heights_[i] = static_cast<int>(estimateHeightFor(i));
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
    scrollOffset_    = std::clamp(scrollOffset_, 0, maxOffset);
    int scrollOffset = scrollOffset_;

    // === 阶段 1: 构建并测量可见子项 (视口局部, 按估算高度定位) ===
    // 只做 ensureElement + 实测 (修正估算高度), 不在此阶段定位:
    // 子项定位必须等总高度/滚动偏移按实测修正后进行, 否则当前帧子项位置
    // 基于估算滚动偏移, 与最终偏移不一致 —— 流式输出时流式项每帧 key 变化
    // 导致估算/实测高度偏差 (通常 ±1 行), 若在测量前定位, 内容帧会把子项
    // 画在错误的偏移上 (底部多出空行), 下一帧 (如鼠标移动) 才回到正确位置,
    // 帧间交替即表现为消息列表上下抖动
    //
    // 可见性判定带估算容错 (kEstimateSlack): 估算高度与实际渲染存在偏差
    // (markdown 段落折行/mermaid 图形/表格换行等), 若按估算位置严格判定,
    // 估算偏低的子项会被误判为"完全在可见区上方" (continue 跳过) 而永不
    // 实测修正 —— 其实际内容占据视口却未渲染, 表现为消息空白; 且其低估
    // 的高度使总高度偏低, stickToBottom 偏移偏小, 底部内容被推出视口。
    // 将"实测范围"向视口外扩 kEstimateSlack 行, 估算偏差在该范围内的子项
    // 提前实测修正, 后续定位即准确 (滑动到该位置时已自愈, 不再空白)。
    // 注意: 仅扩大"实测范围"; 阶段 2 的定位/渲染仍按精确可见区间。
    constexpr int kEstimateSlack = 6;
    int           cum            = 0;     // 累计高度 (当前子项的内容顶边, 行)
    bool          corrected      = false; // 是否有估算高度被实测修正
    for (size_t i = 0; i < count; ++i) {
        const int h   = std::max(1, heights_[i]);
        const int top = cum;
        if (top >= scrollOffset + vh + kEstimateSlack) {
            break; // 完全在可见区下方 (后续更靠下, 提前结束)
        }
        cum += h;
        if (cum <= scrollOffset - kEstimateSlack) {
            continue; // 完全在可见区上方 (含容错范围外)
        }

        // 与 (含容错的) 可见区相交 -> 构建 (缓存命中或 buildItem) 并测量
        ensureElement(i);
        if (!measured_[i]) {
            // 首次布局: 完整迭代布局并测量自然高度, 修正估算值
            const Box measureBox{0, contentWidth - 1, 0, kTallHeight};
            const int realH = layoutAndMeasure(elementAt(i), measureBox);
            heights_[i]     = std::max(1, realH);
            measured_[i]    = true;
            if (heights_[i] != h) {
                corrected = true;
            }
            cum = top + heights_[i];
        }
    }

    // 估算被修正 -> 以实测高度刷新总高度与滚动偏移 (供阶段 2 定位)
    if (corrected) {
        int t = 0;
        for (size_t i = 0; i < count; ++i) {
            t += std::max(1, heights_[i]);
        }
        totalHeight_           = t;
        const int newMaxOffset = std::max(0, t - vh);
        if (stickToBottom_) {
            scrollOffset_ = newMaxOffset;
        } else {
            scrollOffset_ = std::clamp(scrollOffset_, 0, newMaxOffset);
        }
    }

    // === 头部插入锚定校正 (历史分页前插) ===
    // 新增区子项被实测后与初始估算的偏差在此增量补偿到滚动偏移 (多帧收敛),
    // 保证视口内容在分页插入后保持稳定; 补偿后重新夹取防止越界
    applyPrependAnchorCorrection();
    {
        const int maxOff = std::max(0, totalHeight_ - vh);
        scrollOffset_    = std::clamp(scrollOffset_, 0, maxOff);
    }
    scrollOffset = scrollOffset_;

    // === 阶段 2: 以最终滚动偏移定位并布局可见子项 ===
    // 阶段 1 未扫到的可见子项 (估算偏差改变可见区间) 在此补建/补测
    cum = 0;
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

        ensureElement(i);
        int itemH = h;
        bool fresh = false; // 本阶段刚完成测量 (同宽度布局已收敛, 仅 SetBox 即可)
        if (!measured_[i]) {
            // 阶段 1 未覆盖 (估算偏差改变可见区间): 补测
            const Box measureBox{0, contentWidth - 1, 0, kTallHeight};
            itemH        = std::max(1, layoutAndMeasure(elementAt(i), measureBox));
            heights_[i]  = itemH;
            measured_[i] = true;
            fresh        = true;
            cum          = top + itemH;
        }
        const int screenY = box.y_min + (top - scrollOffset);
        Box       itemBox{box.x_min, contentXMax_, screenY, screenY + itemH - 1};
        if (fresh) {
            // 测量时同宽度布局已收敛, 仅 SetBox 重定位
            elementAt(i)->SetBox(itemBox);
        } else {
            // 跳过布局优化: 缓存命中 (key 未变 -> 内容未变) 且 box 与上帧一致时,
            // 子项内部布局状态与上帧完全相同, 无需重跑 ComputeRequirement/SetBox
            // 迭代; 其 reflect 命中框 (点击检测读取) 也保持上帧值 (box 相同)
            const bool cached  = i < hasCache_.size() && hasCache_[i];
            const bool sameBox = lastBoxes_[i] == itemBox;
            if (!(cached && sameBox)) {
                layoutAndMeasure(elementAt(i), itemBox);
            }
        }
        lastBoxes_[i] = itemBox;
        visibleIndices_.push_back(i);
        visibleBoxes_[i] = Box::Intersection(itemBox, box);
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
        const int   yUp               = 2 * (y - box_.y_min);
        const int   yDown             = yUp + 1;
        const bool  up                = (start <= yUp) && (yUp <= start + thumbSize);
        const bool  down              = (start <= yDown) && (yDown <= start + thumbSize);
        const char* c                 = up ? (down ? "┃" : "╹") : (down ? "╻" : " ");
        screen.CellAt(x, y).character = c;
    }
}
