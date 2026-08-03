#pragma once

#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/color.hpp"
#include <memory>

/// 模态层容器 (参考 Flutter 的 Navigator / Overlay 分层)
///
/// 设计:
/// - 持有 main (主界面) 和至多一个 activeModal (模态弹窗)
/// - 事件分发: 模态打开时优先派发给模态; 模态消费则不传递到 main
/// - 渲染: main 始终渲染; 模态叠加在 main 之上 (居中), 背景降低亮度
///
/// 替代原 CatchEvent 中的 if (showModelSelector_) ... if (showSettings_) ... 链:
/// 各模态作为独立 Component 实现自己的 OnEvent, 由本容器统一调度。
///
/// 用法:
///   auto modal = ModalContainer::Create(mainComponent);
///   modal->setBgColor(theme.backgroundColor);
///   modal->pushModal(modelSelectorComponent);  // 打开模态
///   modal->popModal();                         // 关闭模态
class ModalContainer : public ftxui::ComponentBase {
public:

    static std::shared_ptr<ModalContainer> Create(ftxui::Component main) {
        auto self   = std::make_shared<ModalContainer>();
        self->main_ = std::move(main);
        self->Add(self->main_);
        return self;
    }

    /// 设置弹窗背景色 (确保弹窗区域不透明, 遮挡下层内容)
    void setBgColor(ftxui::Color c) {
        bgColor_ = c;
    }

    /// 打开模态 (替换当前模态; 同一时刻仅一个模态)
    void pushModal(ftxui::Component modal) {
        if (activeModal_) {
            activeModal_->Detach();
        }
        activeModal_ = std::move(modal);
        Add(activeModal_);
    }

    /// 关闭当前模态
    void popModal() {
        if (activeModal_) {
            activeModal_->Detach();
            activeModal_ = nullptr;
        }
    }

    bool hasModal() const {
        return activeModal_ != nullptr;
    }

    ftxui::Element OnRender() override {
        // 始终渲染 main 以驱动子组件状态更新 (frameState 等)
        auto base = main_->Render();
        if (activeModal_) {
            // 模态打开时: 完全替换下层, 全屏背景色 + 弹窗居中 (不叠加 base)
            return activeModal_->Render() | ftxui::center | ftxui::bgcolor(bgColor_);
        }
        return base;
    }

    bool OnEvent(ftxui::Event event) override {
        if (activeModal_ && activeModal_->OnEvent(event)) {
            return true;
        }
        return main_->OnEvent(event);
    }

private:

    ftxui::Component main_;
    ftxui::Component activeModal_;
    ftxui::Color     bgColor_ = ftxui::Color::Default;
};
