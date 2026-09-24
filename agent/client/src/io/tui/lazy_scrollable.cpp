#include "agentxx-client/io/tui/lazy_scrollable.h"
#include "agentxx-client/io/tui/scroll_common.h"
#include "ftxui/dom/node.hpp"
#include "ftxui/dom/requirement.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/util/autoreset.hpp"
#include <algorithm>

namespace agentxx::client {

using namespace ftxui;

namespace {

/// 无效命中盒 (未布局的子项)
constexpr Box kInvalidBox{0, -1, 0, -1};

/// 可见性判定的估算容错行数: 未实测子项按粗略估算定位, 与实测存在偏差
/// (长段落/表格/状态图等)。把"实测范围"向视口外扩这么多行, 使估算偏差
/// 范围内的子项提前实测修正; 定位/渲染仍按精确可见区间。
constexpr int kEstimateSlack = 6;

/// 视口下方预取条数: 顺带校验这些条目的 key (只比较, 不构建),
/// 使内容变化的重建提前一帧发生, 滚动进入视口时不会先显示旧内容
constexpr size_t kPrefetchItems = 4;

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
    ItemCountFunc    itemCount,
    ItemKeyFunc      itemKey,
    QuickHeightFunc  quickHeight,
    BuildFunc        buildItem,
    CacheBudget      budget,
    FillViewportFunc fillViewport
) :
    itemCount_(std::move(itemCount)),
    itemKey_(std::move(itemKey)),
    quickHeight_(std::move(quickHeight)),
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
        b = kInvalidBox;
    }
}

ftxui::Element LazyScrollable::OnRender() {
    ++frameSeq_; // 帧边界: 不可缓存项按帧重建 (见 ensureElement)
    return std::make_shared<ListViewNode>(this) | ftxui::reflect(box_);
}

bool LazyScrollable::OnEvent(ftxui::Event event) {
    return handleWheelScroll(
        event,
        box_,
        scrollOffset_,
        stickToBottom_,
        totalHeight_,
        viewportHeight_
    );
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

size_t LazyScrollable::quickHeightFor(size_t index) {
    // 粗略估算必须 O(1) (不解析文本/不渲染); measuredWidth_ 首帧布局前为 -1,
    // 由回调自行兜底默认宽度
    const size_t h = quickHeight_ ? quickHeight_(index, measuredWidth_) : 1;
    return std::max(static_cast<size_t>(1), h);
}

void LazyScrollable::setItemHeight(size_t index, int height) {
    if (index >= heights_.size()) {
        return;
    }
    const int oldValue = std::max(0, heights_[index]);
    const int newValue = std::max(1, height);
    heights_[index]    = newValue;
    totalHeight_ += newValue - oldValue;
    if (index < scanStartIndex_) {
        rowsAboveScanStart_ += newValue - oldValue;
    }
}

void LazyScrollable::refreshKey(size_t index) {
    if (index >= keys_.size() || keyFrames_[index] == prepareSeq_) {
        return; // 本帧已校验过 (或索引越界)
    }
    keyFrames_[index]  = prepareSeq_;
    const uint64_t key = itemKey_ ? itemKey_(index) : 0;
    if (key == keys_[index]) {
        return; // 内容未变: 零成本 (不读内容、不重建)
    }
    keys_[index]     = key;
    measured_[index] = false;
    removeCacheAt(index);
    // 内容变化 -> 已知高度作废, 回到粗略估算 (等实测修正)
    setItemHeight(index, static_cast<int>(quickHeightFor(index)));
}

void LazyScrollable::markEnsured(size_t index) {
    if (index < protectedIndices_.size() && !protectedIndices_[index]) {
        protectedIndices_[index] = true;
        ensuredIndices_.push_back(index);
    }
}

void LazyScrollable::moveScanStartTo(size_t index) {
    while (scanStartIndex_ < index && scanStartIndex_ < heights_.size()) {
        rowsAboveScanStart_ += std::max(1, std::max(0, heights_[scanStartIndex_]));
        ++scanStartIndex_;
    }
    while (scanStartIndex_ > index) {
        --scanStartIndex_;
        rowsAboveScanStart_ -= std::max(1, std::max(0, heights_[scanStartIndex_]));
    }
}

int LazyScrollable::measureItem(size_t index, int contentWidth) {
    refreshKey(index); // 惰性 key 校验: 变化则失效缓存并回到粗略高度
    if (index >= heights_.size()) {
        return 1;
    }
    // 先确保元素存在: 占据整个视口的特殊项 (空状态 banner) 同样要构建 ——
    // 它的高度虽直接取视口高度, 但内容 (含可点区域) 仍需渲染
    ensureElement(index);
    if (fillViewport_ && fillViewport_(index)) {
        setItemHeight(index, viewportHeight_);
        measured_[index] = true;
        return std::max(1, viewportHeight_);
    }
    if (!measured_[index]) {
        const Box measureBox{0, contentWidth - 1, 0, kTallHeight};
        setItemHeight(index, layoutAndMeasure(elementAt(index), measureBox));
        measured_[index] = true;
    }
    return std::max(1, std::max(0, heights_[index]));
}

void LazyScrollable::syncItemArrays(size_t count) {
    const size_t oldSize = heights_.size();

    // 尾部收缩 (列表变短, 如清空会话): 移除被裁掉子项的缓存并减去高度
    if (count < oldSize) {
        for (size_t i = count; i < oldSize; ++i) {
            removeCacheAt(i);
            totalHeight_ -= std::max(0, heights_[i]);
        }
        if (scanStartIndex_ >= count) {
            // 扫描起点被裁掉: 回到头部重扫一次 (列表刚变短, 成本可忽略)
            scanStartIndex_     = 0;
            rowsAboveScanStart_ = 0;
        }
    }

    heights_.resize(count, -1);
    measured_.resize(count, false);
    keys_.resize(count, 0);
    keyFrames_.resize(count, 0);
    hasCache_.resize(count, false);
    itemCache_.resize(count);
    lastBoxes_.resize(count, kInvalidBox);
    protectedIndices_.resize(count, false);
    visibleBoxes_.resize(count, kInvalidBox);

    // 新增项 (尾部追加): 按粗略高度初始化并校验一次 key
    for (size_t i = oldSize; i < count; ++i) {
        const int h  = static_cast<int>(quickHeightFor(i));
        heights_[i]  = h;
        totalHeight_ += h;
        keys_[i]      = itemKey_ ? itemKey_(i) : 0;
        keyFrames_[i] = prepareSeq_;
    }

    // 未知高度 (notifyPrepended 头插区: 那时状态快照尚未刷新, 不能估算)
    // 在此补齐 —— 只在存在未知项时全量扫一遍
    bool hasUnknown = false;
    for (int h : heights_) {
        if (h < 0) {
            hasUnknown = true;
            break;
        }
    }
    if (hasUnknown) {
        for (size_t i = 0; i < count; ++i) {
            if (heights_[i] < 0) {
                const int h = static_cast<int>(quickHeightFor(i));
                heights_[i] = h;
                totalHeight_ += h;
                if (i < scanStartIndex_) {
                    rowsAboveScanStart_ += h;
                }
            }
        }
    }
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
        markEnsured(index);
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
    markEnsured(index);
    // 新构建的元素没有上帧布局状态: 清空其 lastBoxes_, 使阶段 2 的
    // "缓存命中且 box 相同则跳过布局" 判定失效, 强制重新布局 (SetBox)。
    // 否则: key 变化 (单条替换 / onSync 整体重建) 但内容与高度不变的项,
    // 阶段 1 重建了新元素后, 阶段 2 用旧 lastBoxes_ 误判 sameBox=true 跳过
    // 布局 —— 新元素从未 SetBox (box_ = {0,0,0,0}), Text 只画首字符到 (0,0),
    // 表现为消息少开头/整行不显示 (用户报告症状)。
    // 注意: 缓存命中路径 (本函数开头 return) 不清 lastBoxes_, sameBox 判定
    // 正常生效 —— 内容未变 + box 未变时跳过布局是安全的。
    if (index < lastBoxes_.size()) {
        lastBoxes_[index] = kInvalidBox;
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

    // 上一帧的可见/已确保记录先复位 (索引即将整体平移)
    for (size_t i : visibleIndices_) {
        if (i < visibleBoxes_.size()) {
            visibleBoxes_[i] = kInvalidBox;
        }
    }
    visibleIndices_.clear();
    for (size_t i : ensuredIndices_) {
        if (i < protectedIndices_.size()) {
            protectedIndices_[i] = false;
        }
    }
    ensuredIndices_.clear();

    // 并行数组头部插入 k 个新条目; 既有数据整体后移 —— key 与条目同步平移,
    // 旧条目 key 校验依然匹配 (缓存 Element/实测高度全保留), 仅新增区为初始值
    heights_.insert(heights_.begin(), count, -1);
    measured_.insert(measured_.begin(), count, false);
    keys_.insert(keys_.begin(), count, 0);
    keyFrames_.insert(keyFrames_.begin(), count, 0);
    hasCache_.insert(hasCache_.begin(), count, false);
    visibleBoxes_.insert(visibleBoxes_.begin(), count, kInvalidBox);
    using ListIt = std::list<Entry>::iterator;
    itemCache_.insert(itemCache_.begin(), count, ListIt{});
    lastBoxes_.insert(lastBoxes_.begin(), count, kInvalidBox);
    protectedIndices_.insert(protectedIndices_.begin(), count, false);

    // LRU 缓存条目索引平移 (list 迭代器稳定, 直接改 index 字段即可;
    // itemCache_ 中迭代器的存储位置已随 vector 头插对齐到新索引)
    for (auto& entry : lruList_) {
        entry.index += count;
    }

    // 新增区高度暂为未知 (-1): 调用方在状态前插后、本帧快照刷新前调用
    // (UI 动作队列语义), 此时经回调估算读到的是旧快照内容, 口径必然错误。
    // 下一帧 prepareLayout 的 syncItemArrays 以新快照口径补齐粗略高度。
    scanStartIndex_ += count; // 既有条目的索引整体后移
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
    // 新增区当前已知总高度: 实测优先, 未测子项沿用粗略估算
    long long actualRows  = 0;
    bool      allMeasured = true;
    for (size_t i = 0; i < n; ++i) {
        if (!measured_[i]) {
            allMeasured = false;
        }
        actualRows += std::max(1, std::max(0, heights_[i]));
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
    ++prepareSeq_;

    // 上一帧的可见/已确保记录复位: 只处理上一帧记录过的条目, 不做 O(n) 全量填充
    for (size_t i : visibleIndices_) {
        if (i < visibleBoxes_.size()) {
            visibleBoxes_[i] = kInvalidBox;
        }
    }
    visibleIndices_.clear();
    for (size_t i : ensuredIndices_) {
        if (i < protectedIndices_.size()) {
            protectedIndices_[i] = false;
        }
    }
    ensuredIndices_.clear();

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

    // 内容宽度变化 -> 换行结果失效: 清空缓存, 高度全部回到粗略估算
    // (每条 O(1): 不解析文本、不渲染、不加锁), 扫描起点回到头部重扫一次
    if (contentWidth != measuredWidth_) {
        clearCache();
        measuredWidth_ = contentWidth;
        int total      = 0;
        for (size_t i = 0; i < heights_.size(); ++i) {
            const int h  = static_cast<int>(quickHeightFor(i));
            heights_[i]  = h;
            measured_[i] = false;
            total += h;
        }
        totalHeight_        = total;
        scanStartIndex_     = 0;
        rowsAboveScanStart_ = 0;
    }

    const size_t count = itemCount_();
    syncItemArrays(count);
    if (count == 0) {
        totalHeight_  = 0;
        scrollOffset_ = 0;
        return;
    }

    // === 窗口发现 ===
    // 吸附底部 (默认): 从尾部往前走, 一边走一边实测, 直到累计高度够一屏 ——
    // 视口定位只依赖"尾部一屏内条目"的实测高度 + 视口上方条目的高度和
    // (rowsAboveScanStart_), 与"视口上方未实测条目的估算"无关: 估算再离谱
    // (10 倍高估/低估) 也不会把最新内容推出视口或让视口空白。
    // 非吸附 (用户上滚后): 以 scrollOffset_ 为准, 扫描起点对齐到
    // 视口顶 - 估算容错带 (逐条回退, 正常滚动回退距离 <= 容错带行数)。
    size_t scannedEnd = scanStartIndex_;
    if (stickToBottom_) {
        size_t    first = count;
        long long rows  = 0;
        while (first > 0 && rows < viewportHeight_) {
            --first;
            rows += measureItem(first, contentWidth);
        }
        moveScanStartTo(first);
        scrollOffset_ = std::max(
            0,
            rowsAboveScanStart_ + static_cast<int>(rows) - viewportHeight_
        );
    } else {
        const int target = std::max(0, scrollOffset_ - kEstimateSlack);
        while (scanStartIndex_ > 0 && rowsAboveScanStart_ > target) {
            --scanStartIndex_;
            rowsAboveScanStart_ -= std::max(1, std::max(0, heights_[scanStartIndex_]));
        }
    }

    // === 阶段 1: 构建并测量可见子项 (从扫描起点向后, 视口局部) ===
    // 只做 ensureElement + 实测 (修正估算高度), 不在此阶段定位:
    // 子项定位必须等总高度/滚动偏移按实测修正后进行, 否则当前帧子项位置
    // 基于估算滚动偏移, 与最终偏移不一致 —— 流式输出时流式项每帧 key 变化
    // 导致估算/实测高度偏差 (通常 ±1 行), 若在测量前定位, 内容帧会把子项
    // 画在错误的偏移上 (底部多出空行), 下一帧 (如鼠标移动) 才回到正确位置,
    // 帧间交替即表现为消息列表上下抖动
    //
    // 可见性判定带估算容错 (kEstimateSlack): 估算高度与实际渲染存在偏差
    // (markdown 段落折行/mermaid 图形/表格换行等), 若按估算位置严格判定,
    // 估算偏低的子项会被误判为"完全在可见区上方"而永不实测修正 —— 其实际
    // 内容占据视口却未渲染, 表现为消息空白; 且其低估的高度使总高度偏低,
    // stickToBottom 偏移偏小, 底部内容被推出视口。
    {
        int cum = rowsAboveScanStart_;
        for (size_t i = scanStartIndex_; i < count; ++i) {
            const int h   = std::max(1, std::max(0, heights_[i]));
            const int top = cum;
            if (top >= scrollOffset_ + viewportHeight_ + kEstimateSlack) {
                break; // 完全在可见区下方 (后续更靠下, 提前结束)
            }
            cum += h;
            scannedEnd = i + 1;
            if (cum <= scrollOffset_ - kEstimateSlack) {
                // 完全在可见区上方 (含容错范围外): 前移扫描起点, 下次不再重扫
                scanStartIndex_     = i + 1;
                rowsAboveScanStart_ = cum;
                continue;
            }
            // 与 (含容错的) 可见区相交 -> 构建 (缓存命中或 buildItem) 并测量
            cum = top + measureItem(i, contentWidth);
        }
    }

    // === 预取带: 视口下方若干条目的 key 校验 (只比较, 不构建) ===
    // 使内容变化的重建提前一帧发生, 滚动进入视口时直接是新内容
    for (size_t i = scannedEnd; i < std::min(count, scannedEnd + kPrefetchItems); ++i) {
        refreshKey(i);
    }

    // === 总高度/滚动偏移 (高度和由 setItemHeight 增量维护) ===
    const int maxOffset = std::max(0, totalHeight_ - vh);
    if (stickToBottom_) {
        scrollOffset_ = maxOffset;
    }
    scrollOffset_ = std::clamp(scrollOffset_, 0, maxOffset);

    // === 头部插入锚定校正 (历史分页前插) ===
    // 新增区子项被实测后与初始估算的偏差在此增量补偿到滚动偏移 (多帧收敛),
    // 保证视口内容在分页插入后保持稳定; 补偿后重新夹取防止越界
    applyPrependAnchorCorrection();
    scrollOffset_ = std::clamp(scrollOffset_, 0, std::max(0, totalHeight_ - vh));

    // 偏移可能已变化 -> 重新对齐扫描起点 (只回退; 前进由阶段 1 的推进逻辑完成)
    {
        const int target = std::max(0, scrollOffset_ - kEstimateSlack);
        while (scanStartIndex_ > 0 && rowsAboveScanStart_ > target) {
            --scanStartIndex_;
            rowsAboveScanStart_ -= std::max(1, std::max(0, heights_[scanStartIndex_]));
        }
    }

    // === 阶段 2: 以最终滚动偏移定位并布局可见子项 ===
    // 阶段 1 未扫到的可见子项 (偏移/高度修正改变可见区间) 在此补建/补测
    {
        int cum = rowsAboveScanStart_;
        for (size_t i = scanStartIndex_; i < count; ++i) {
            const int h   = std::max(1, std::max(0, heights_[i]));
            const int top = cum;
            if (top >= scrollOffset_ + vh) {
                break; // 完全在可见区下方 (后续更靠下, 提前结束)
            }
            cum += h;
            if (cum <= scrollOffset_) {
                // 完全在可见区上方: 前移扫描起点
                scanStartIndex_     = i + 1;
                rowsAboveScanStart_ = cum;
                continue;
            }

            const bool wasMeasured = measured_[i];
            const int  itemH       = measureItem(i, contentWidth);
            if (!wasMeasured) {
                cum = top + itemH;
            }
            const int screenY = box.y_min + (top - scrollOffset_);
            Box       itemBox{box.x_min, contentXMax_, screenY, screenY + itemH - 1};
            if (!wasMeasured) {
                // 测量时同宽度布局已收敛, 仅 SetBox 重定位
                elementAt(i)->SetBox(itemBox);
            } else {
                // 跳过布局优化: 缓存命中 (key 未变 -> 内容未变) 且 box 与上帧一致时,
                // 子项内部布局状态与上帧完全相同, 无需重跑 ComputeRequirement/SetBox
                // 迭代; 其 reflect 命中框 (点击检测读取) 也保持上帧值 (box 相同)
                const bool cached  = i < hasCache_.size() && hasCache_[i];
                const bool sameBox = i < lastBoxes_.size() && lastBoxes_[i] == itemBox;
                if (!(cached && sameBox)) {
                    layoutAndMeasure(elementAt(i), itemBox);
                }
            }
            lastBoxes_[i] = itemBox;
            visibleIndices_.push_back(i);
            if (i < visibleBoxes_.size()) {
                visibleBoxes_[i] = Box::Intersection(itemBox, box);
            }
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
        const int   yUp               = 2 * (y - box_.y_min);
        const int   yDown             = yUp + 1;
        const bool  up                = (start <= yUp) && (yUp <= start + thumbSize);
        const bool  down              = (start <= yDown) && (yDown <= start + thumbSize);
        const char* c                 = up ? (down ? "┃" : "╹") : (down ? "╻" : " ");
        screen.CellAt(x, y).character = c;
    }
}

} // namespace agentxx::client
