/// Scrollable / LazyScrollable 共用的滚动容器逻辑
///
/// - 两个组件都实现"仅布局/绘制与视口相交的子项"的列表容器, 差异只在子项构建方式
///   (全量构建 vs 懒构建); 元素测量与滚轮处理完全一致, 收敛到这里避免两份拷贝漂移
/// - 相关: [Scrollable] / [LazyScrollable]
#pragma once

#include "ftxui/component/event.hpp"
#include "ftxui/dom/node.hpp"
#include "ftxui/dom/requirement.hpp"
#include "ftxui/screen/box.hpp"
#include <algorithm>

/// 布局迭代上限 (与 ftxui::Render 内部保持一致)
inline constexpr int kMaxLayoutIteration = 20;

/// 测量/布局时给出的"足够大"高度 (换行仅依赖宽度, 高度给足即可读取自然高度)
inline constexpr int kTallHeight = 1000000;

/// 对元素执行完整迭代布局 (ComputeRequirement + SetBox 多轮直至收敛),
/// 返回其在该宽度下的自然高度 (行)。
///
/// 原理: flexbox/paragraph 的换行高度在布局迭代收敛后体现为 requirement().min_y,
/// 故布局收敛后直接读取 min_y 即为该宽度下的实际高度。
inline int layoutAndMeasure(const ftxui::Element& el, ftxui::Box box) {
    if (!el) {
        return 1;
    }
    ftxui::Node::Status status;
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

/// 处理鼠标滚轮事件 (固定每次滚动 1 行高度)
///
/// - 命中判定: 事件为鼠标事件且坐标落在 [box] 内, 否则不消费
/// - 向上滚动: 取消"吸附底部"并上移 1 行 (顶部处保持不变)
/// - 向下滚动: 下移 1 行 (不越过最大偏移); 滚到底部后恢复"吸附底部"
///
/// - `args`:
///     - [event] 待处理的 FTXUI 事件 (须为左值: FTXUI 只提供非 const 的 `mouse()` 访问)
///     - [box] 组件本帧渲染区域 (命中检测)
///     - [scrollOffset] 当前滚动偏移 (行, 从顶部计), 就地更新
///     - [stickToBottom] 是否吸附底部, 就地更新
///     - [totalHeight] 内容总高度 (行)
///     - [viewportHeight] 视口高度 (行)
///
/// - `return` true 表示事件已被消费 (调用方应返回 true)
inline bool handleWheelScroll(
    ftxui::Event&     event,
    const ftxui::Box& box,
    int&              scrollOffset,
    bool&             stickToBottom,
    int               totalHeight,
    int               viewportHeight
) {
    if (!event.is_mouse()) {
        return false;
    }
    const auto& mouse = event.mouse();
    if (!box.Contain(mouse.x, mouse.y)) {
        return false;
    }
    // 固定每次滚动 1 行高度
    if (mouse.button == ftxui::Mouse::WheelUp) {
        stickToBottom = false;
        scrollOffset  = std::max(0, scrollOffset - 1);
        return true;
    }
    if (mouse.button == ftxui::Mouse::WheelDown) {
        const int maxOffset = std::max(0, totalHeight - viewportHeight);
        scrollOffset        = std::min(maxOffset, scrollOffset + 1);
        if (scrollOffset >= maxOffset) {
            stickToBottom = true; // 滚到底部 -> 恢复吸附
        }
        return true;
    }
    return false;
}
