/// Scrollable / LazyScrollable 共用的布局测量逻辑
///
/// - 两个组件都实现"仅布局/绘制与视口相交的子项"的列表容器, 差异只在子项构建方式
///   (全量构建 vs 懒构建) 与滚动状态模型 (偏移从顶部计 vs 锚点即主状态),
///   元素测量口径一致, 收敛到这里避免两份拷贝漂移
/// - 滚轮处理**不再共用**: Scrollable 用偏移 (行, 从顶部计), LazyScrollable 用
///   锚点 (条目索引 + 条目内行偏移) 并在跨条目时实测目标条目高度, 两者语义不同
/// - 相关: [Scrollable] / [LazyScrollable]
#pragma once

#include "ftxui/dom/node.hpp"
#include "ftxui/dom/requirement.hpp"
#include "ftxui/screen/box.hpp"
#include <algorithm>

namespace agentxx::client {

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

} // namespace agentxx::client
