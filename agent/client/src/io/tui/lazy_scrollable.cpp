#include "agentxx-client/io/tui/lazy_scrollable.h"
#include "agentxx-client/io/tui/scroll_common.h"
#include "ftxui/dom/node.hpp"
#include "ftxui/dom/requirement.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/util/autoreset.hpp"
#include <algorithm>
#include <limits>

namespace agentxx::client {

using namespace ftxui;

namespace {

/// 无效命中盒 (未布局的子项)
constexpr Box kInvalidBox{0, -1, 0, -1};

/// 视口下方预取条数: 顺带校验这些条目的 key (只比较, 不构建),
/// 使内容变化的重建提前一帧发生, 滚动进入视口时不会先显示旧内容
constexpr size_t kPrefetchItems = 4;

} // namespace

/// 视口布局节点: SetBox/Render 委托给 LazyScrollable 组件。
///
/// 与 ftxui 内置 yframe+focusPositionRelative 方案 (需全量布局整列表) 不同:
/// - ComputeRequirement 不递归子项 (父级以 |flex 撑满, 不依赖 min_x/min_y)
/// - SetBox 阶段只构建/定位"锚点到视口底部"这一段的子项 (懒构建 + 高度缓存),
///   锚点以上子项本帧零成本; 局部超出视口的部分由 screen.stencil 裁剪
/// - Render 阶段仅绘制可见子项
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

int LazyScrollable::scrollOffset() const {
    // 锚点派生值: 只用于滚动条长度/位置与预取判定 (视口上方未实测项按粗略估算计入)
    const int offset = rowsAboveAnchor_ + anchorRow_;
    return std::clamp(offset, 0, std::max(0, totalHeight_ - viewportHeight_));
}

void LazyScrollable::clearCache() {
    lruList_.clear();
    cachedBytes_ = 0;
    // vector<bool> 的代理引用不能绑定 bool&, 用 auto&&
    for (auto&& f : hasCache_) {
        f = false;
    }
    // 缓存清空后本帧重建的 Element 是新对象 (无上帧布局状态), 必须重新布局:
    // 定位阶段的"缓存命中且 box 相同则跳过布局"优化以元素对象未变为前提,
    // 若沿用旧 lastBoxes_ 判断, 主题切换后新构建的元素会被误判为
    // "box 与上帧一致"而跳过 SetBox —— 元素从未布局 (box_ 未初始化),
    // 渲染位置错误/内容丢失, 且此后每帧都跳过 (锚点不变则 box 恒同),
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
    if (!event.is_mouse()) {
        return false;
    }
    const auto& mouse = event.mouse();
    if (!box_.Contain(mouse.x, mouse.y)) {
        return false;
    }
    // 滚轮固定每次 1 行: 只累积行数, 下一帧 prepareLayout 落实到锚点。
    // 为什么不在事件里直接移动锚点: 移动跨条目时需要实测目标子项高度, 那会
    // 构建 Element —— 而本组件每帧在 OnRender 里清空命中登记表 (decor 按钮/
    // 中断控件/附件卡片), 由"本帧布局阶段构建"的 Element 重新登记。若在
    // OnEvent 期构建, 元素进入缓存后下一帧 OnRender 清空了登记表却不会再构建,
    // 该帧的按钮/控件命中区就会丢失 (点击失效)。布局阶段构建则天然安全。
    if (mouse.button == ftxui::Mouse::WheelUp) {
        stickToBottom_ = false; // 上滚即解除吸附 (与既有语义一致)
        --pendingScrollRows_;
        return true;
    }
    if (mouse.button == ftxui::Mouse::WheelDown) {
        ++pendingScrollRows_;
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
    // 锚点以上子项的高度变化会改变派生滚动偏移 (只影响滚动条/预取判定):
    // 锚点本身与其下方子项的变化不影响视图位置 (见头文件类注释不变量 1)
    if (index < anchorIndex_) {
        rowsAboveAnchor_ += newValue - oldValue;
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

void LazyScrollable::recomputeRowsAboveAnchor() {
    // 只在锚点位置整体变化 (列表收缩裁掉锚点) 或高度整体失效 (宽度变化) 时调用
    long long sum = 0;
    const size_t n = std::min(anchorIndex_, heights_.size());
    for (size_t i = 0; i < n; ++i) {
        sum += std::max(1, std::max(0, heights_[i]));
    }
    rowsAboveAnchor_ = static_cast<int>(
        std::min<long long>(sum, std::numeric_limits<int>::max())
    );
}

void LazyScrollable::resetHeightsToQuick() {
    // 宽度变化后的整体失效: 每条 O(1) 粗略估算 (不解析文本/不渲染), 实测标记清零
    long long total = 0;
    for (size_t i = 0; i < heights_.size(); ++i) {
        const int h  = static_cast<int>(quickHeightFor(i));
        heights_[i]  = h;
        measured_[i] = false;
        total += h;
    }
    totalHeight_ = static_cast<int>(std::min<long long>(total, std::numeric_limits<int>::max()));
    recomputeRowsAboveAnchor();
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
            const int h = std::max(0, heights_[i]);
            totalHeight_ -= h;
            if (i < anchorIndex_) {
                rowsAboveAnchor_ -= h;
            }
        }
        if (anchorIndex_ >= count) {
            // 锚点被裁掉: 退到列表末尾 (O(count) 重算只在列表收缩时发生)
            anchorIndex_ = count > 0 ? count - 1 : 0;
            anchorRow_   = 0;
            recomputeRowsAboveAnchor();
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
        setItemHeight(i, static_cast<int>(quickHeightFor(i)));
        keys_[i]      = itemKey_ ? itemKey_(i) : 0;
        keyFrames_[i] = prepareSeq_;
    }

    // 头部前插区 (notifyPrepended 设置: 那时状态快照尚未刷新, 不能估算)
    // 在此按新快照口径补齐粗略高度 —— 只补前插区这一段前缀, 不做全量扫描。
    // 前插区全部位于锚点上方: 补齐只改变 totalHeight_/rowsAboveAnchor_ (滚动条),
    // 视口内容不受影响 (不变量 1)
    if (unknownPrefix_ > 0) {
        const size_t n = std::min(unknownPrefix_, count);
        for (size_t i = 0; i < n; ++i) {
            if (heights_[i] < 0) {
                setItemHeight(i, static_cast<int>(quickHeightFor(i)));
            }
        }
        unknownPrefix_ = 0;
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
    // 标记为本帧已确保: 本帧定位阶段处理过的条目禁止被 evictIfNeeded 淘汰
    // (淘汰由本条插入触发的 evictIfNeeded 即时执行, 先标记后淘汰才有效)
    markEnsured(index);
    // 新构建的元素没有上帧布局状态: 清空其 lastBoxes_, 使定位阶段的
    // "缓存命中且 box 相同则跳过布局" 判定失效, 强制重新布局 (SetBox)。
    // 否则: key 变化 (单条替换 / onSync 整体重建) 但内容与高度不变的项,
    // 重建了新元素后, 定位阶段用旧 lastBoxes_ 误判 sameBox=true 跳过
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
    // 首次布局之前 (无任何高度数据): 索引尚未建立, 只需记录条数语义
    // (首屏填充场景: 消息从空列表整体前插, 无旧视口需要锚定)
    const bool laidOutBefore = !heights_.empty();

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

    // 并行数组头部插入 count 个新条目; 既有数据整体后移 —— key 与条目同步平移,
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

    if (laidOutBefore) {
        // **锚点随索引平移**: 视口顶行仍指向同一条内容 -> 前插对视口零影响,
        // 不存在任何"偏移校正"状态 (方案 §3.4: 前插 = 锚点平移)
        anchorIndex_ += count;
    }
    // 新增区高度暂记未知 (-1): 调用方在状态前插后、本帧快照刷新前调用
    // (UI 动作队列语义), 此时经回调估算读到的是旧快照内容, 口径必然错误。
    // 下一帧 prepareLayout 的 syncItemArrays 以新快照口径补齐粗略高度 ——
    // 新增区位于锚点上方, 只影响滚动条长度, 不影响视口内容。
    // 帧间连续多次前插 (分页连发) 时未知区在前缀累加, 故用 += 而非取最大值
    unknownPrefix_ += count;
}

void LazyScrollable::resetAnchorState() {
    anchorIndex_       = 0;
    anchorRow_         = 0;
    rowsAboveAnchor_   = 0;
    pendingScrollRows_ = 0;
}

bool LazyScrollable::atContentBottom() const {
    // 上一帧定位阶段的精确结论 (内容末尾是否落在视口内, 见 prepareLayout 末尾):
    // 只由实测高度与视口高度得出, 与视口上方/下方未实测条目的估算无关 ——
    // 不能用估算总和判断, 否则低估会让下滚提前判成"到底"并跳到底部
    return contentEndsInViewport_;
}

void LazyScrollable::applyPendingScrollRows(int contentWidth, size_t count) {
    int rows = pendingScrollRows_;
    pendingScrollRows_ = 0;
    if (rows == 0 || count == 0) {
        return;
    }

    // 上滚 (负): 视口顶行上移 —— 行偏移先减, 减到 0 就跨入上一条目 (先实测取
    // 真实高度, 顶行落在它的最后一行); 已到首条且行偏移为 0 时停住
    while (rows < 0) {
        if (anchorRow_ > 0) {
            --anchorRow_;
            ++rows;
            continue;
        }
        if (anchorIndex_ == 0) {
            break; // 已在列表顶部
        }
        const size_t prev = anchorIndex_ - 1;
        const int    h    = measureItem(prev, contentWidth); // 即将进入视口 -> 实测
        // 锚点上移: 该条目不再计入"锚点以上高度和" (measureItem 已按新高度
        // 增量修正过它, 此处整体扣回)
        rowsAboveAnchor_ -= h;
        anchorIndex_ = prev;
        anchorRow_   = h - 1;
        ++rows;
    }

    // 下滚 (正): 内容末尾已落在视口内时恢复吸附底部 (与"滚到底自动跟随新增内容"
    // 语义一致); 否则行偏移加 1, 到底就进入下一条目首行
    while (rows > 0) {
        if (atContentBottom()) {
            stickToBottom_ = true;
            break;
        }
        const int h = std::max(1, std::max(0, heights_[anchorIndex_]));
        if (anchorRow_ + 1 < h) {
            ++anchorRow_;
            --rows;
            continue;
        }
        if (anchorIndex_ + 1 >= count) {
            // 一次性落实多行下滚 (事件突发) 时可能跨过"到底"的那一行:
            // 锚点已到末尾条目行尾即视为到底, 同样恢复吸附底部
            stickToBottom_ = true;
            break;
        }
        rowsAboveAnchor_ += h;
        ++anchorIndex_;
        anchorRow_ = 0;
        --rows;
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
        // 本帧已确保的子项 (定位阶段处理过, 含可见项) 禁止淘汰:
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
    // (每条 O(1): 不解析文本、不渲染、不加锁)。锚点 (条目索引 + 行偏移) 保持不变,
    // 视口仍停在同一条内容上 (行偏移在新高度下夹取), 比"按偏移从顶部重定位"稳定
    if (contentWidth != measuredWidth_) {
        clearCache();
        measuredWidth_ = contentWidth;
        resetHeightsToQuick();
    }

    const size_t count = itemCount_();
    syncItemArrays(count);
    if (count == 0) {
        totalHeight_           = 0;
        rowsAboveAnchor_       = 0;
        anchorIndex_           = 0;
        anchorRow_             = 0;
        pendingScrollRows_     = 0;
        contentEndsInViewport_ = true; // 空列表: 无可滚动内容
        return;
    }
    if (anchorIndex_ >= count) {
        // 锚点被列表收缩裁掉 (或复位后超出): 退到列表末尾
        anchorIndex_ = count - 1;
        anchorRow_   = 0;
        recomputeRowsAboveAnchor();
    }

    // === 窗口发现 (吸附底部) ===
    // 从尾部往前实测累计到够一屏 -> 锚点 = 尾部窗口起点, 行偏移 = 窗口内被视口
    // 遮住的行数。定位只依赖"尾部一屏内条目"的实测高度, 与视口上方未实测条目的
    // 估算无关 (估算再离谱也不会把最新内容推出视口)。
    if (stickToBottom_) {
        size_t    first = count;
        long long rows  = 0;
        while (first > 0 && rows < viewportHeight_) {
            --first;
            rows += measureItem(first, contentWidth);
        }
        anchorIndex_ = first;
        // 反推锚点以上高度和 (含上方估算): 派生偏移因此严格等于
        // totalHeight_ - viewportHeight_ (滚动条贴底)。注意: 上面 while 里
        // setItemHeight 对 rowsAboveAnchor_ 的增量修正会被本行整体覆盖
        rowsAboveAnchor_ = std::max(0, totalHeight_ - static_cast<int>(rows));
        anchorRow_       = rows > viewportHeight_ ? static_cast<int>(rows - viewportHeight_) : 0;
    } else {
        // 非吸附: 锚点就是视口顶行, 只把它夹进自身高度内 —— 锚点以上条目本帧
        // 完全不参与计算 (不构建/不测量/不读高度)
        const int h = measureItem(anchorIndex_, contentWidth);
        anchorRow_  = std::clamp(anchorRow_, 0, h - 1);
    }

    // 滚轮累积行数: 锚点模型下改变滚动位置的唯一入口 (视口顶行上/下移 N 行)
    applyPendingScrollRows(contentWidth, count);

    // === 定位并布局视口内条目 (从锚点向后, 锚点以上零成本) ===
    // 定位只用实测高度: 每个条目先测量 (key 变化则重建重测) 再落到屏幕坐标,
    // 不存在"先按估算定位、下一帧再修正"的抖动窗口 (方案 §3.4 不变量)
    size_t nextIndex = count;
    int    laidRows  = 0;
    {
        int cum = -anchorRow_; // 相对视口顶的行号 (锚点条目的 anchorRow_ 行落在 0)
        for (size_t i = anchorIndex_; i < count; ++i) {
            if (cum >= vh) {
                nextIndex = i;
                break; // 视口已填满 (后续更靠下, 提前结束)
            }
            const bool fresh   = !measured_[i];
            const int  itemH   = measureItem(i, contentWidth);
            const int  screenY = box.y_min + cum;
            Box        itemBox{box.x_min, contentXMax_, screenY, screenY + itemH - 1};
            if (fresh) {
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
            cum += itemH;
        }
        laidRows = cum;
    }

    // 内容末尾是否落在视口内: 布局已走到最后一个条目, 且从锚点行起的累计行数
    // 不超过视口高度 -> 已经不可能再向下滚动。这是**精确**结论 (只由实测高度与
    // 视口高度得出, 不含任何估算), 供下一帧落实滚轮下滚时判定"是否已到底"
    contentEndsInViewport_ = (nextIndex == count && laidRows <= vh);

    // === 预取带: 视口下方若干条目的 key 校验 (只比较, 不构建) ===
    // 使内容变化的重建提前一帧发生, 滚动进入视口时直接是新内容
    for (size_t i = nextIndex; i < std::min(count, nextIndex + kPrefetchItems); ++i) {
        refreshKey(i);
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
    // 半行精度的 thumb 高度与起始位置 (算法同 ftxui vscroll_indicator);
    // 偏移取锚点派生值 (吸附底部时等于 totalHeight_ - viewportHeight_)
    const int vh        = viewportHeight_;
    const int offset    = scrollOffset();
    int       thumbSize = 2 * vh * vh / totalHeight_;
    thumbSize           = std::max(thumbSize, 1);
    const int start     = 2 * offset * vh / totalHeight_;
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
