#pragma once

#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/box.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <vector>

/// buildItem 回调的返回值
struct LazyBuiltItem {
    ftxui::Element element;
    /// 本条目在内存中的实际占用估算字节数 (参与字节预算统计; 估算值即可)。
    ///
    /// 语义注意: 应为"渲染结果的内存开销"而非"源数据字节" —— 若按源文本
    /// 字节上报, 字节预算与真实内存脱节 (如 FTXUI 渲染树为源文本 30~70 倍,
    /// 按源字节预算会放行远超预期的驻留内存)。调用方需按实际放大折算
    /// (见 MessageListComponent::buildMessageItem 按 ×64 系数上报)。
    size_t sourceBytes = 0;
    /// 是否可缓存 (如流式增量项每帧都变, 缓存无意义, 置 false)
    bool cacheable = true;
    /// 生命周期需与 element 绑定的附属对象
    /// (如 markdown DomBuilder: Element 内 reflect 的 Box 指向其内部容器)
    std::vector<std::shared_ptr<void>> attachments;
};

/// 懒构建可滚动容器 (对标 Flutter ListView.builder)
///
/// 与全量构建的 Scrollable 不同, 本组件采用惰性构建 + 有界缓存:
///
/// - [懒构建] 通过 itemCount()/itemKey()/estimateHeight()/buildItem() 回调描述列表,
///   仅按需构建子项 Element; 每帧只对与视口相交的可见子项调用 buildItem
/// - [视口局部布局/绘制] 布局阶段仅对可见子项执行测量与布局, 渲染阶段仅绘制
///   可见子项 (超出部分经 screen stencil 裁剪); 不可见子项零成本
/// - [有界缓存] 已构建的子项 Element 按 LRU 缓存 (条数 + 累计源字节双预算),
///   窗口外的旧子项被淘汰释放 —— 内存占用与列表长度解耦 (消息列表不再随
///   对话持续无限增长)
/// - [高度缓存与估算] 子项高度按 (itemKey, 视口宽度) 缓存; 未测量过的子项使用
///   estimateHeight 提供的估算值, 首次进入视口时测量修正
/// - [滚动] 滚轮固定每次 1 行; 吸附底部模式 (stickToBottom) 下内容增长自动跟随到底
///
/// 线程模型: 本组件仅供 UI 线程使用 (FTXUI Loop 内)
class LazyScrollable : public ftxui::ComponentBase {
public:

    using ItemCountFunc      = std::function<size_t()>;
    using ItemKeyFunc        = std::function<uint64_t(size_t index)>;
    using EstimateHeightFunc = std::function<size_t(size_t index, int width)>;
    using BuildFunc          = std::function<LazyBuiltItem(size_t index)>;
    /// 判断子项是否占据整个视口高度 (空状态居中展示用); 返回 false 则正常布局
    using FillViewportFunc = std::function<bool(size_t index)>;

    /// 缓存预算配置
    struct CacheBudget {
        /// 缓存子项条数上限
        size_t maxItems = 256;
        /// 缓存子项内存估算字节上限 (以 sourceBytes 累计; sourceBytes 应为
        /// 渲染结果的内存估算, 而非源数据字节 —— 见 LazyBuiltItem::sourceBytes)
        size_t maxBytes = 16 * 1024 * 1024;
        /// 字节预算豁免: sourceBytes 不超过该值的子项不计入字节预算
        /// (避免大量短条目 (如状态行) 过早触发字节淘汰; 仍受 maxItems 条数约束)
        size_t byteExemptThreshold = 1024;
    };

    explicit LazyScrollable(
        ItemCountFunc      itemCount,
        ItemKeyFunc        itemKey,
        EstimateHeightFunc estimateHeight,
        BuildFunc          buildItem,
        CacheBudget        budget,
        FillViewportFunc   fillViewport = nullptr
    );

    // === 状态访问 ===

    bool isStickToBottom() const {
        return stickToBottom_;
    }

    void setStickToBottom(bool v) {
        stickToBottom_ = v;
    }

    /// 当前滚动偏移 (行, 从顶部计)
    int scrollOffset() const {
        return scrollOffset_;
    }

    /// 内容总高度 (行; 未测量子项使用估算高度)
    int totalHeight() const {
        return totalHeight_;
    }

    /// 视口高度 (行)
    int viewportHeight() const {
        return viewportHeight_;
    }

    /// 内容可用宽度 (终端列数, 已扣除滚动条 gutter); 首帧布局前返回 -1
    int contentWidth() const {
        return measuredWidth_;
    }

    /// 上一帧各子项的可见屏幕区域 (索引对应 itemCount() 的项; 不可见为空 Box)
    /// 供外部鼠标命中检测使用
    const std::vector<ftxui::Box>& visibleBoxes() const {
        return visibleBoxes_;
    }

    /// 清空缓存 (如主题切换后旧 Element 的颜色已过时)
    void clearCache();

    /// 清除可见子项残留的鼠标选中高亮 (Text::has_selection_)。
    ///
    /// 背景: 本组件为懒构建/局部布局, 跳过 FTXUI 每帧的 ComputeRequirement
    /// (Text 节点只在 ComputeRequirement 里复位 has_selection_), 因此拖动选中
    /// 的文本在选择被清空后高亮不消失。调用本方法对当前可见子项执行一次
    /// ComputeRequirement, 使其选择状态随内容重算归零 (幂等, 不影响布局).
    /// 用于"拖选松开自动复制"完成后清除高亮 (见 agent_tui.cpp 拖选跟踪)
    void resetSelectionHighlight();

    // === ComponentBase 接口 ===
    ftxui::Element OnRender() override;
    bool           OnEvent(ftxui::Event event) override;

private:

    class ListViewNode; // 视口布局节点 (嵌套类可访问私有成员; 定义见 .cpp)

    /// LRU 缓存条目
    struct Entry {
        size_t         index = 0; // 对应子项索引 (淘汰时回写失效标记)
        ftxui::Element element;
        std::vector<std::shared_ptr<void>> attachments;
        size_t                             sourceBytes  = 0;
        bool                               bytesCounted = false; // 是否已计入字节预算
    };

    /// 布局阶段 (由布局节点在 SetBox 时调用):
    /// 同步条数/key, 计算可见区间, 构建/复用并布局可见子项, 执行 LRU 淘汰
    void prepareLayout(const ftxui::Box& box);
    /// 渲染阶段: 仅绘制可见子项 + 滚动条
    void renderVisible(ftxui::Screen& screen);
    /// 绘制滚动条
    void drawScrollbar(ftxui::Screen& screen);

    /// 获取子项 Element (缓存命中或本帧刚构建); 仅在 prepareLayout 后对可见项调用
    ftxui::Element& elementAt(size_t index);
    /// 确保可见子项的 Element 存在 (缓存命中则 LRU 提前; 否则 buildItem)
    void ensureElement(size_t index);
    /// 移除指定索引的缓存条目
    void removeCacheAt(size_t index);
    /// 按条数/字节预算从 LRU 尾部淘汰
    void evictIfNeeded();
    /// 估算未测量子项的高度 (行), 兜底 >= 1
    size_t estimateHeightFor(size_t index) const;

    // ---- 回调 ----
    ItemCountFunc      itemCount_;
    ItemKeyFunc        itemKey_;
    EstimateHeightFunc estimateHeight_;
    BuildFunc          buildItem_;
    FillViewportFunc   fillViewport_;
    CacheBudget        budget_;

    // ---- 逐帧布局状态 ----
    std::vector<int>      heights_;        // 各子项高度 (-1 = 未测量)
    std::vector<bool>     measured_;       // 高度是否已实测
    std::vector<uint64_t> keys_;           // 各子项上次布局时的 key
    std::vector<bool>     hasCache_;       // 各子项是否有缓存 Element
    std::vector<size_t>   visibleIndices_; // 本帧可见子项索引

    /// 不可缓存项 (cacheable=false) 的 Element (每帧重建一次, 跨布局迭代复用)
    struct TransientEntry {
        size_t        index;
        LazyBuiltItem item;
    };

    std::vector<TransientEntry> transientItems_;
    uint64_t                    frameSeq_          = 0;     // OnRender 递增 (帧边界)
    uint64_t                    lastPreparedFrame_ = ~0ULL; // transientItems_ 所属帧

    int  scrollOffset_   = 0;
    bool stickToBottom_  = true;
    int  totalHeight_    = 0;
    int  viewportHeight_ = 0;
    int  measuredWidth_  = -1;    // 上次布局所用内容宽度 (变化时缓存整体失效)
    bool hasGutter_      = false; // 是否预留滚动条列 (影响滚动条绘制判断)
    int  contentXMax_    = 0;     // 内容区右边界 (已扣除 gutter)

    // ---- LRU 缓存 (头部为最近使用) ----
    std::list<Entry>                                 lruList_;
    std::vector<typename std::list<Entry>::iterator> itemCache_;
    size_t                                           cachedBytes_ = 0;

    // ---- 命中检测输出 ----
    std::vector<ftxui::Box> visibleBoxes_;
    /// 各子项上一帧的布局 Box (与 items 按 index 对应; 未布局过的项为无效 Box)。
    /// 缓存命中且 box 与上帧一致时, 子项内部布局状态与上帧完全相同,
    /// 可跳过整棵子树的 ComputeRequirement/SetBox 迭代 (见 prepareLayout 阶段 2)
    std::vector<ftxui::Box> lastBoxes_;
    ftxui::Box              box_;
};
