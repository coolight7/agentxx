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

namespace agentxx::client {

/// buildItem 回调的返回值
struct LazyBuiltItem {
    ftxui::Element element;
    /// 本条目在内存中的实际占用估算字节数 (参与字节预算统计; 估算值即可)。
    ///
    /// 注意: 应为"渲染结果的内存开销"而非"源数据字节" —— 若按源文本
    /// 字节上报, 字节预算与真实内存占用不一致 (如 FTXUI 渲染树为源文本
    /// 30~70 倍, 按源字节预算会放行远超预期的驻留内存)。调用方需按实际
    /// 放大折算 (见 MessageListComponent::buildMessageItem 的实测标定)。
    size_t sourceBytes = 0;
    /// 是否可缓存 (如流式增量项每帧都变, 缓存无意义, 置 false)
    bool cacheable = true;
    /// 生命周期需与 element 绑定的附属对象
    /// (如 markdown DomBuilder: Element 内 reflect 的 Box 指向其内部容器)
    std::vector<std::shared_ptr<void>> attachments;
};

/// 懒构建可滚动容器 (仿照 Flutter ListView.builder)
///
/// 与全量构建的 Scrollable 不同, 本组件采用惰性构建 + 局部缓存:
///
/// - [懒构建] 通过 itemCount()/itemKey()/quickHeight()/buildItem() 回调描述列表,
///   仅按需构建子项 Element; 每帧只对与视口相交的子项调用 buildItem
/// - [视口局部布局/绘制] 布局阶段仅对可见子项执行测量与布局, 渲染阶段仅绘制
///   可见子项 (超出部分经 screen stencil 裁剪); 不可见子项零成本
/// - [局部缓存] 已构建的子项 Element 按 LRU 缓存 (条数 + 累计源字节双预算),
///   窗口外的旧子项被淘汰释放 —— 内存占用与列表长度解耦 (消息列表不再随
///   对话持续无限增长)
///
/// ## 滚动模型: 锚点即主状态
///
/// 唯一滚动状态是**锚点** `(anchorIndex_, anchorRow_)`: 视口顶行落在
/// `anchorIndex_` 号子项的 `anchorRow_` 行 (0 <= anchorRow_ < 高度(anchor))。
/// 三个不变量由这个模型直接给出:
///
/// 1. **定位只用实测高度**: 每帧从锚点向后逐条实测并顺次定位; 锚点上方子项
///    完全不参与本帧计算 (不构建/不测量/不读高度)。视口上方的高度和
///    (rowsAboveAnchor_) 只用于派生 scrollOffset() (滚动条长度/位置、历史分页
///    预取判定), 未实测子项在其中按 quickHeight() 粗略估算 —— 估算再离谱
///    (高估/低估 10 倍) 也不会把内容推出视口或让视口空白。
/// 2. **测量即渲染**: 子项高度来自它自己的布局结果 (自绘折行节点一次布局即
///    得准确高度), 不存在"第二套行数判定"; 高度按 (itemKey, 宽度) 缓存。
/// 3. **每帧成本与列表长度无关**: 每帧只处理"锚点到视口底部"这一段的子项;
///    子项 key 按帧标记惰性比较 (视口外子项在重新进入窗口时才校验),
///    上一帧的可见/已确保记录按列表复位。宽度变化时全部高度回到粗略估算
///    (每条 O(1), 不解析文本/不渲染), 是一次 O(n) 例外。
///
/// ### 吸附底部 (stickToBottom_, 默认开启)
///
/// 每帧从尾部向前实测累计, 直到够一屏 —— 锚点即这个"尾部窗口"的起点,
/// 行偏移 = 窗口内被视口遮住的行数。因此吸附态下最新内容始终贴底,
/// 与视口上方未实测子项的估算无关。
///
/// ### 滚动 (滚轮, 每次 1 行)
///
/// 上滚 = 视口顶行上移 1 行: 行偏移先减, 减到 0 就跨入上一条目 (先实测取真实
/// 高度, 顶行落在它的最后一行); 已到首条且行偏移为 0 时停住。上滚即解除吸附。
/// 下滚 = 视口顶行下移 1 行; "是否已到底"由上一帧定位阶段的**精确**结论判定
/// (布局是否走到末尾条目且行数未满视口, 不含估算), 确认到底即恢复吸附底部
/// (与"滚到底自动跟随新增内容"的语义一致)。
/// 滚轮事件只累积行数 (pendingScrollRows_), 在下一帧布局时落实 —— 布局前
/// 不构建子项 Element, 避免 OnEvent 期构建的元素其命中登记被下一帧
/// OnRender 的清理动作丢掉 (详见 prepareLayout 注释)。
///
/// ### 头部插入 (历史分页前插)
///
/// `notifyPrepended()` 后并行数组整体平移、锚点随索引平移 —— 视口内容零跳变,
/// 不需要任何"偏移校正"状态 (新增区的粗略高度与实测偏差只影响滚动条长度)。
///
/// ### 线程模型
///
/// 本组件仅供 UI 线程使用 (FTXUI Loop 内)
class LazyScrollable : public ftxui::ComponentBase {
public:

    using ItemCountFunc      = std::function<size_t()>;
    using ItemKeyFunc        = std::function<uint64_t(size_t index)>;
    /// 粗略高度回调 (**必须 O(1)**: 不解析文本/不渲染/不加锁)。
    /// 只用于"未实测子项的总高度估计"(滚动条长度) 与首帧占位; 返回值会被缓存,
    /// 同一宽度下每条仅调用一次 (宽度变化或子项 key 变化时重新调用)
    using QuickHeightFunc    = std::function<size_t(size_t index, int width)>;
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
        ItemCountFunc    itemCount,
        ItemKeyFunc      itemKey,
        QuickHeightFunc  quickHeight,
        BuildFunc        buildItem,
        CacheBudget      budget,
        FillViewportFunc fillViewport = nullptr
    );

    // === 状态访问 ===

    bool isStickToBottom() const {
        return stickToBottom_;
    }

    void setStickToBottom(bool v) {
        stickToBottom_ = v;
    }

    /// 当前滚动偏移 (行, 从顶部计) —— 由锚点派生: rowsAboveAnchor_ + anchorRow_,
    /// 并夹取到 [0, totalHeight_ - viewportHeight_]。
    ///
    /// 注意: 视口上方未实测子项按粗略估算计入, 因此该值只保证"滚动条/预取判定"
    /// 够用 (滚动条长度与位置), **不代表**视口内容对应的高度 —— 视口内的位置只由
    /// 锚点与实测高度决定 (见类注释不变量 1)。吸附底部时它与
    /// `totalHeight() - viewportHeight()` 严格相等。
    int scrollOffset() const;

    /// 内容总高度 (行; 未测量子项使用粗略估算高度)
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
    ///
    /// 注意: 鼠标命中检测应使用本接口, 不要用子项元素内的 `ftxui::reflect`
    /// 命中框 —— 本组件测量子项高度时会以"测量用临时大框" (局部坐标: x = 0..
    /// 内容宽, y = 0..很大) 调用 SetBox, 视口外子项会残留该框; 它的局部坐标
    /// 与屏幕坐标部分重叠, 点击会先命中到视口外 (看不见) 的子项。
    /// 参考 [MessageListComponent] 的可见区域命中判定。
    const std::vector<ftxui::Box>& visibleBoxes() const {
        return visibleBoxes_;
    }

    /// 锚点子项索引 (视口顶行所在子项; 测试/诊断用)
    size_t anchorIndex() const {
        return anchorIndex_;
    }

    /// 锚点行偏移 (视口顶行在该子项内的行号, 0 <= 值 < 高度(anchor); 测试/诊断用)
    int anchorRow() const {
        return anchorRow_;
    }

    /// 清空缓存 (如主题切换后旧 Element 的颜色已过时)
    void clearCache();

    /// 头部插入子项后的滚动锚定 (历史分页前插场景; UI 线程, 帧间调用)
    ///
    /// 在并行数组头部插入 count 个新条目 —— 既有条目的缓存 Element 与实测
    /// 高度随索引整体平移而保留 (key 对齐校验通过, 不失效重建), 锚点索引同步
    /// 平移: 视口顶行仍指向同一条内容, **零校正**。
    /// 新增区高度暂记未知 (-1), 由下一次 prepareLayout 以新快照口径补齐粗略
    /// 高度 (调用时刻状态快照尚未刷新, 此刻经 quickHeight 读到的是旧快照内容,
    /// 口径必然错误) —— 新增区位于锚点上方, 只影响滚动条长度。
    /// - 应在状态前插完成后、下一帧渲染前调用 (UI 动作队列语义)
    /// - 尚未布局过 (无任何缓存/高度数据) 时仅记录条数 (首屏填充场景无需锚定)
    void notifyPrepended(size_t count);

    /// 复位滚动状态 (消息列表整体替换/会话切换时调用: 内容已换, 旧锚点不再
    /// 有意义): 锚点回到 (0, 0), 并丢弃未落实的滚轮行数。
    /// 吸附底部状态下调用后, 下一帧仍会按尾部窗口重新确定锚点。
    void resetAnchorState();

    /// 清除可见子项残留的鼠标选中高亮 (Text::has_selection_)。
    ///
    /// 背景: 本组件为懒构建/局部布局, 跳过 FTXUI 每帧的 ComputeRequirement
    /// (Text 节点只在 ComputeRequirement 里复位 has_selection_), 因此拖动选中
    /// 的文本在选择被清空后高亮不消失。调用本方法对当前可见子项执行一次
    /// ComputeRequirement, 使其选择状态随内容重算归零 (幂等, 不影响布局).
    /// 用于"拖选松开自动复制"完成后清除高亮 (见
    /// [agent_tui.cpp](/agent/client/src/io/tui/agent_tui.cpp) 拖选跟踪)
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
    /// 同步条数/key, 落实滚轮行数, 发现窗口 (吸附底部时尾部向前, 否则锚点向后),
    /// 构建/复用并定位窗口内子项, 执行 LRU 淘汰
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
    /// 未实测子项的粗略高度 (行; 调 quickHeight 回调并缓存, 兜底 >= 1)
    size_t quickHeightFor(size_t index);
    /// 设置子项高度 (行), 增量维护 totalHeight_ 与 rowsAboveAnchor_
    void setItemHeight(size_t index, int height);
    /// 同步逐条目数组长度; 新增尾部项按粗略高度初始化并校验一次 key,
    /// 移除项减去高度 (锚点被裁掉时回到末尾), 头部前插区的未知高度在此补齐
    void syncItemArrays(size_t count);
    /// 校验子项 key (本帧未校验过时比较; 变化则失效缓存并回到粗略高度)
    void refreshKey(size_t index);
    /// 标记子项为本帧已确保 (evictIfNeeded 不得淘汰)
    void markEnsured(size_t index);
    /// 重算 rowsAboveAnchor_ (锚点以上高度和; 仅锚点位置/高度整体失效时调用, O(锚点索引))
    void recomputeRowsAboveAnchor();
    /// 全部子项高度回到粗略估算并重算总高度/锚点以上高度和 (宽度变化路径, O(n))
    void resetHeightsToQuick();
    /// 确保子项高度已实测 (未实测则构建并测量), 返回其高度 (行)
    int measureItem(size_t index, int contentWidth);

    /// 落实滚轮累积的行数 (负=上滚, 正=下滚): 逐行移动视口顶行, 跨条目时先实测
    /// 目标子项高度 (它即将进入视口), 内容末尾已在视口内时恢复吸附底部
    void applyPendingScrollRows(int contentWidth, size_t count);
    /// 内容末尾是否已在视口内 (取上一帧定位阶段的结论; 已到底时下滚转为吸附底部)
    bool atContentBottom() const;

    // ---- 回调 ----
    ItemCountFunc    itemCount_;
    ItemKeyFunc      itemKey_;
    QuickHeightFunc  quickHeight_;
    BuildFunc        buildItem_;
    FillViewportFunc fillViewport_;
    CacheBudget      budget_;

    // ---- 逐条目状态 ----
    std::vector<int>      heights_;        // 各子项有效高度 (行; -1 = 未填)
    std::vector<bool>     measured_;       // 高度是否已实测
    std::vector<uint64_t> keys_;           // 各子项上次校验时的 key
    std::vector<bool>     hasCache_;       // 各子项是否有缓存 Element
    std::vector<size_t>   visibleIndices_; // 本帧可见子项索引
    /// 本帧已确保 (构建/测量过, 待渲染) 的子项索引 (用于复位保护标记)
    std::vector<size_t>   ensuredIndices_;
    /// 各子项 key 的校验帧号 (惰性比较: 只在与视口相关的条目上校验, 见 prepareLayout)
    std::vector<uint64_t> keyFrames_;
    /// 头部前插区待补高度的前缀长度 (notifyPrepended 设置: 该区间高度为 -1,
    /// 下一次 syncItemArrays 以新快照口径补齐后归零)
    size_t                unknownPrefix_ = 0;

    /// 不可缓存项 (cacheable=false) 的 Element (每帧重建一次, 跨布局迭代复用)
    struct TransientEntry {
        size_t        index;
        LazyBuiltItem item;
    };

    std::vector<TransientEntry> transientItems_;
    uint64_t                    frameSeq_          = 0;     // OnRender 递增 (帧边界)
    uint64_t                    lastPreparedFrame_ = ~0ULL; // transientItems_ 所属帧

    // ---- 滚动状态 (锚点即主状态) ----
    /// 视口顶行所在子项索引
    size_t anchorIndex_ = 0;
    /// 视口顶行在该子项内的行号 (0 <= 值 < 高度(anchor))
    int    anchorRow_   = 0;
    /// 锚点以上子项的高度和 (未实测项按粗略估算; 派生出 scrollOffset)
    int    rowsAboveAnchor_ = 0;
    /// 待落实的滚轮行数 (负=上滚; 由 OnEvent 累积, prepareLayout 开头落实)
    int    pendingScrollRows_ = 0;
    bool   stickToBottom_     = true;
    /// 上一帧定位阶段的精确结论: 内容末尾是否落在视口内 (= 已到底, 无法再下滚)。
    /// 只由实测高度与视口高度得出, 不含任何估算 —— 下滚时据此恢复吸附底部
    bool   contentEndsInViewport_ = false;

    int  totalHeight_   = 0; // 全部子项有效高度和 (增量维护, 不再每帧全量求和)
    int  viewportHeight_ = 0;
    int  measuredWidth_  = -1;    // 上次布局所用内容宽度 (变化时缓存整体失效)
    bool hasGutter_      = false; // 是否预留滚动条列 (影响滚动条绘制判断)
    int  contentXMax_    = 0;     // 内容区右边界 (已扣除 gutter)
    uint64_t prepareSeq_ = 0;     // prepareLayout 次数 (key 校验帧标记)

    // ---- LRU 缓存 (头部为最近使用) ----
    std::list<Entry>                                 lruList_;
    std::vector<typename std::list<Entry>::iterator> itemCache_;
    size_t                                           cachedBytes_ = 0;

    // ---- 命中检测输出 ----
    std::vector<ftxui::Box> visibleBoxes_;
    /// 各子项上一帧的布局 Box (与 items 按 index 对应; 未布局过的项为无效 Box)。
    /// 缓存命中且 box 与上帧一致时, 子项内部布局状态与上帧完全相同,
    /// 可跳过整棵子树的 ComputeRequirement/SetBox 迭代 (见 prepareLayout 定位阶段)
    std::vector<ftxui::Box> lastBoxes_;
    ftxui::Box              box_;

    /// 本帧已确保 (prepareLayout 定位阶段处理过, 待渲染) 的子项索引标记:
    /// evictIfNeeded 禁止淘汰这些条目。
    ///
    /// 背景: 预算淘汰在 ensureElement 插入时从 LRU 尾部执行, 尾部先消耗视口外
    /// 旧缓存, 但当"可见集自身"超过预算 (长消息折算超 maxBytes / 高终端
    /// 可见条数超 maxItems) 时, 淘汰会一直延续到本帧已处理、仍待渲染的可见
    /// 子项 —— 它们仍在 visibleIndices_ (命中盒有效, 可点击折叠/展开), 但
    /// 缓存被删 (hasCache_=false), 渲染时 elementAt 回退空 text, 连续多条消息
    /// 显示为空白 (用户报告 "滚动到一定位置时连续几条消息不显示, 再滚动恢复")。
    /// 标记后淘汰在遇到首个帧内已确保条目时停止 (LRU 序 = 最近使用在前,
    /// 本帧条目全部位于前部, 尾部未确保条目先被淘汰完), 保证渲染优先于压预算。
    std::vector<bool> protectedIndices_;
};

} // namespace agentxx::client
