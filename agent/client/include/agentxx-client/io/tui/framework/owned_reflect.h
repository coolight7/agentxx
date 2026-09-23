#pragma once

#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/node.hpp"
#include "ftxui/screen/box.hpp"
#include <memory>
#include <utility>

namespace agentxx {
namespace client {

/// 绑定自有反射框的元素节点 (元素自己持有 `reflect` 用的 Box)
///
/// 背景: FTXUI 的 `reflect(Box&)` 只保存**引用**, Box 必须比元素活得久。行模型/
/// 命中登记的元素常被接入点搬进滚动容器、消息块缓存 (可能跨帧存活), 而生成它的
/// `UiRow::box` / 命中登记项往往随局部结果析构 —— 一旦只搬元素, 元素随后参与布局
/// 时就会写已释放内存 (ASan: heap-use-after-free, 栈顶为 `ftxui::Reflect::SetBox`,
/// `layoutAndMeasure` 为调用方)。
///
/// 本节点把 Box 的所有权绑在元素上: 移动/缓存元素即等于带走 Box, 接入点无需额外
/// 保存。语义与 FTXUI 的 `Reflect` 完全一致 (ComputeRequirement 透传子项需求,
/// SetBox 写回坐标并向下传递, Render 与屏幕 stencil 求交), 命中也仍按同一个 Box
/// 读取坐标 —— 唯一区别是 Box 由 shared_ptr 持有, 生命周期不短于元素。
///
/// 相关: 使用处见 [ui_components.cpp] 的行渲染 (renderItem/mergeTextButton) 与
/// [ui_hit.h] 的命中登记 (UiHitRegistry::add/addRegions)
class OwnedReflect : public ftxui::Node {
public:

    OwnedReflect(ftxui::Element child, std::shared_ptr<ftxui::Box> box) :
        ftxui::Node({std::move(child)}),
        box_(std::move(box)) {}

    void ComputeRequirement() override {
        Node::ComputeRequirement();
        requirement_ = children_[0]->requirement();
    }

    void SetBox(ftxui::Box box) override {
        *box_ = box;
        Node::SetBox(box);
        children_[0]->SetBox(box);
    }

    void Render(ftxui::Screen& screen) override {
        *box_ = ftxui::Box::Intersection(screen.stencil, *box_);
        Node::Render(screen);
    }

private:

    std::shared_ptr<ftxui::Box> box_;
};

} // namespace client
} // namespace agentxx
