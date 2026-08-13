#pragma once

#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/box.hpp"
#include <functional>
#include <vector>

/// 可滚动列表子项 (仿 Flutter ListView 的 child)
struct ScrollItem {
    ftxui::Element element;
    /// 占据整个视口高度 (空状态居中展示用); 默认 false
    bool fillViewport = false;
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
    const std::vector<ftxui::Box>& visibleBoxes() const {
        return visibleBoxes_;
    }

    /// 清除可见子项残留的鼠标选中高亮 (Text::has_selection_)。
    /// 背景与实现同 LazyScrollable::resetSelectionHighlight:
    /// 本组件跳过 FTXUI 每帧 ComputeRequirement, Text 节点的选择状态不会
    /// 自动复位, 需显式对可见子项执行 ComputeRequirement 归零 (幂等)。
    void resetSelectionHighlight() {
        for (size_t i = 0; i < items_.size(); ++i) {
            if (i < visibleBoxes_.size() && !visibleBoxes_[i].IsEmpty()
                && items_[i].element) {
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

    ftxui::Box box_; // 本组件渲染区域 (reflect 填充, 用于滚轮命中检测)
};
