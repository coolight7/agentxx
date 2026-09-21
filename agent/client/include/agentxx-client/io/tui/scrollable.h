#pragma once

#include "agentxx-client/io/tui/framework/ui_hit.h"
#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/box.hpp"
#include <cstddef>
#include <functional>
#include <vector>

namespace agentxx::client {

/// 可滚动列表子项 (仿 Flutter ListView 的 child)
struct ScrollItem {
    ftxui::Element element;
    /// 占据整个视口高度 (空状态居中展示用); 默认 false
    bool fillViewport = false;
    /// 子项内的可命中区域 (局部坐标: 相对本子项左上角)
    ///
    /// 调用方在渲染子项时一并给出可点位置 (按钮 / 表格单元格 / 控件等);
    /// 点击定位经 [Scrollable::hitTestItem] 映射到"第几个子项 + 子项内局部坐标",
    /// 再按本字段判定命中具体位置。不要用子项元素内部的 `reflect` 做命中检测
    /// (滚动容器测量子项时会以临时大框布局, 视口外子项会残留该框 → 幽灵命中)。
    std::vector<UiHitRegion> hits;
};

/// 可复用的可滚动容器组件 (仿 Flutter ListView 的 viewport 局部绘制)
///
/// 设计目标 (参考 Flutter 的 Widget/渲染分层):
/// - [viewport 局部绘制] 仅对与可见区域相交的子项做布局(layout)与绘制(paint),
///   不可见子项完全跳过, 避免整列表的全量布局/绘制开销
/// - [高度缓存] 子项高度按 (内容, 视口宽度) 缓存; 仅在子项变化或宽度变化时惰性重测,
///   类似 Flutter SliverList 的 estimated/cached extent
/// - [按行滚动] 滚轮固定每次滚动 1 行 (而非比例浮动), 滚动位置以"行偏移"表示
/// - [stickToBottom] 靠近底部时自动吸附, 内容增长时跟随到底
///
/// 用法:
///   auto scroll = Scrollable::Create([&] { return buildItems(); });
///   scroll->onContentUpdate();          // 内容更新 (stickToBottom 时自动吸附)
///   bool atBottom = scroll->isStickToBottom();
///   const auto& boxes = scroll->visibleBoxes(); // 各子项可见屏幕区域 (鼠标命中检测)
class Scrollable : public ftxui::ComponentBase {
public:

    using RenderFunc = std::function<std::vector<ScrollItem>()>;

    static ftxui::Component Create(RenderFunc render) {
        return std::make_shared<Scrollable>(std::move(render));
    }

    explicit Scrollable(RenderFunc render) :
        render_(std::move(render)) {}

    /// stickToBottom 模式: 滚动位置自动吸附到底部
    void setStickToBottom(bool v) {
        stickToBottom_ = v;
    }

    bool isStickToBottom() const {
        return stickToBottom_;
    }

    /// 内容更新时调用: stickToBottom 模式下由布局阶段自动保持底部 (无需手动设置偏移)
    void onContentUpdate() {}

    /// 当前滚动偏移 (行, 从顶部计)
    int scrollOffset() const {
        return scrollOffset_;
    }

    /// 设置滚动偏移 (行, 从顶部计); 越界值在下次布局时被 clamp
    /// (供外部键盘滚动等场景使用; 通常应同时 setStickToBottom(false))
    void setScrollOffset(int offset) {
        scrollOffset_ = std::max(0, offset);
    }

    /// 内容总高度 (行)
    int totalHeight() const {
        return totalHeight_;
    }

    /// 视口高度 (行)
    int viewportHeight() const {
        return viewportHeight_;
    }

    /// 内容可用宽度 (终端列数; 已扣除滚动条 gutter)。
    /// 首帧布局前返回 -1 (尚未测量)。
    int contentWidth() const {
        return measuredWidth_;
    }

    /// 上一帧各子项的可见屏幕区域 (索引与 render 返回的 items 对应)。
    /// 不可见子项为空 Box (IsEmpty() 为 true)。供外部鼠标命中检测。
    ///
    /// 注意: 鼠标命中检测应使用本接口, 不要用子项元素内的 `ftxui::reflect`
    /// 命中框 —— 本组件测量子项高度时会以"测量用临时大框" (局部坐标: x = 0..
    /// 内容宽, y = 0..很大) 调用 SetBox, 视口外子项会残留该框; 它的局部坐标
    /// 与屏幕坐标部分重叠, 点击会先命中到视口外 (看不见) 的子项。
    /// 参考 [ContextOverlay] 与 [MessageListComponent] 的可见区域命中判定。
    const std::vector<ftxui::Box>& visibleBoxes() const {
        return visibleBoxes_;
    }

    /// 上一帧渲染的子项 (索引与 render 返回的 items 对应; 可取各子项的 hits)
    const std::vector<ScrollItem>& items() const {
        return items_;
    }

    /// 屏幕坐标 → 子项内位置 (仅可见子项可命中)
    ///
    /// - 命中时返回 true, 并给出子项下标与**相对该子项左上角**的局部坐标
    ///   (即使子项被视口上边缘裁剪, 局部坐标仍按子项实际顶边计算, 与子项内
    ///   登记的可命中区域 [ScrollItem::hits] 口径一致)
    /// - 未命中 (坐标在视口外或落在子项之间的空隙) 返回 false
    /// - `args`:
    ///     - [x] [y] 屏幕坐标 (与 FTXUI 鼠标事件坐标一致)
    ///     - [itemIndex] 命中子项下标 (输出)
    ///     - [localX] [localY] 子项内局部坐标 (输出)
    bool hitTestItem(int x, int y, size_t& itemIndex, int& localX, int& localY) const;

    /// 清除可见子项残留的鼠标选中高亮 (Text::has_selection_)。
    /// 背景与实现同 LazyScrollable::resetSelectionHighlight:
    /// 本组件跳过 FTXUI 每帧 ComputeRequirement, Text 节点的选择状态不会
    /// 自动复位, 需显式对可见子项执行 ComputeRequirement 归零 (幂等)。
    void resetSelectionHighlight() {
        for (size_t i = 0; i < items_.size(); ++i) {
            if (i < visibleBoxes_.size() && !visibleBoxes_[i].IsEmpty() && items_[i].element) {
                items_[i].element->ComputeRequirement();
            }
        }
    }

    // === ComponentBase 接口 ===
    ftxui::Element OnRender() override;
    bool           OnEvent(ftxui::Event event) override;

private:

    RenderFunc render_;

    // 子项缓存 (与 render_ 最近一次返回的 items 一一对应)
    std::vector<ScrollItem>     items_;
    std::vector<int>            heights_;        // 各子项缓存高度 (-1 = 待测量)
    std::vector<ftxui::Element> cachedElements_; // 上次元素指针 (检测内容变化)
    std::vector<bool>           cachedFill_;     // 上次 fillViewport 标记

    int  scrollOffset_   = 0;    // 滚动偏移 (行)
    bool stickToBottom_  = true; // 吸附底部
    int  totalHeight_    = 0;    // 内容总高度 (行)
    int  viewportHeight_ = 0;    // 视口高度 (行)
    int  measuredWidth_  = -1;   // 上次测量所用内容宽度 (变化时使高度缓存失效)

    std::vector<ftxui::Box> visibleBoxes_; // 各子项可见屏幕区域 (输出)

    /// 上一帧可见子项下标 (清理离开视口的子项时用)
    std::vector<size_t> prevVisibleIndices_;

    /// 各子项本帧的完整布局区域 (未布局项为空 Box; 命中定位时换算局部坐标用)
    std::vector<ftxui::Box> itemBoxes_;

    ftxui::Box box_; // 本组件渲染区域 (reflect 填充, 用于滚轮命中检测)
};

} // namespace agentxx::client
