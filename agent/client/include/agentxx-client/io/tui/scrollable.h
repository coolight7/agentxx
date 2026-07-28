#pragma once

#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/box.hpp"

/// 可复用的可滚动容器组件
///
/// 职责:
/// - 通过 Render() 提供 yframe + vscroll_indicator 包裹的内容
/// - 内部处理 Mouse::WheelUp/WheelDown, 自动调整滚动位置
/// - 提供 stickToBottom 模式: 内容更新时自动吸附底部
/// - 通过 focusPositionRelative 定位, 渲染回调无需关心 focus
///
/// 用法:
///   auto scroll = Scrollable::Create([&] { return renderMyContent(); });
///   // 内容更新时:
///   scroll->onContentUpdate();
///   // 查询状态:
///   bool atBottom = scroll->isStickToBottom();
class Scrollable : public ftxui::ComponentBase {
public:

    using RenderFunc = std::function<ftxui::Element()>;

    static ftxui::Component Create(RenderFunc render) {
        return std::make_shared<Scrollable>(std::move(render));
    }

    /// stickToBottom 模式: 滚动位置自动吸附到底部
    void setStickToBottom(bool v) {
        stickToBottom_ = v;
        if (v) {
            scroll_ = 1.0f;
        }
    }

    bool isStickToBottom() const {
        return stickToBottom_;
    }

    /// 内容更新时调用: stickToBottom 模式下自动保持底部
    void onContentUpdate() {
        if (stickToBottom_) {
            scroll_ = 1.0f;
        }
    }

    float scroll() const {
        return scroll_;
    }

    void setScroll(float s) {
        scroll_        = std::max(0.0f, std::min(1.0f, s));
        stickToBottom_ = false;
    }

    explicit Scrollable(RenderFunc render) :
        render_(std::move(render)) {}

    // ComponentBase 接口
    ftxui::Element OnRender() override {
        return render_() | ftxui::focusPositionRelative(0.0f, scroll_) | ftxui::vscroll_indicator
               | ftxui::yframe | ftxui::reflect(box_);
    }

    bool OnEvent(ftxui::Event event) override {
        if (!event.is_mouse()) {
            return false;
        }
        const auto& mouse = event.mouse();
        if (!box_.Contain(mouse.x, mouse.y)) {
            return false;
        }

        constexpr float kStep = 0.05f;
        if (mouse.button == ftxui::Mouse::WheelUp) {
            stickToBottom_ = false;
            scroll_        = std::max(0.0f, scroll_ - kStep);
            return true;
        }
        if (mouse.button == ftxui::Mouse::WheelDown) {
            if (scroll_ >= 1.0f) {
                stickToBottom_ = true;
            } else {
                scroll_ = std::min(1.0f, scroll_ + kStep);
            }
            return true;
        }
        return false;
    }

private:

    RenderFunc render_;
    float      scroll_        = 1.0f;
    bool       stickToBottom_ = true;
    ftxui::Box box_;
};
