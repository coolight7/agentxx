#pragma once

#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/color.hpp"
#include <memory>

namespace agentxx::client {

/// 模态层容器 (参考 Flutter 的 Navigator / Overlay 分层)
///
/// 设计:
/// - 持有 main (主界面) 和至多一个 activeModal (模态弹窗)
/// - 事件分发: 模态打开时事件只给模态, **不再下发给 main**
///   (模态是全屏遮挡层: 未处理的事件下发给 main 会让字符落到被遮挡的输入框、
///   滚轮滚动被遮挡的消息列表 —— 视觉上"看不见却在动")
/// - 渲染: 无模态时渲染 main; 模态打开时只渲染模态 (居中 + 全屏蒙版色),
///   主界面整棵树 (消息列表懒布局/绘制) 不再参与本帧渲染
///
/// 替代原 CatchEvent 中的 if (showModelSelector_) ... if (showSettings_) ... 链:
/// 各模态作为独立 Component 实现自己的 OnEvent, 由本容器统一调度。
///
/// 用法:
///   auto modal = ModalContainer::Create(mainComponent);
///   modal->setBgColor(theme.surfaceScrimColor);
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
        if (activeModal_) {
            // 模态打开: 主界面被全屏蒙版完整遮挡, 不渲染 (省掉整棵树每帧的
            // 构建与布局开销 —— 消息列表懒布局是渲染成本的大头);
            // 关闭模态后的下一帧重新渲染主界面, 立即恢复
            return activeModal_->Render() | ftxui::center | ftxui::bgcolor(bgColor_);
        }
        return main_->Render();
    }

    bool OnEvent(ftxui::Event event) override {
        if (activeModal_) {
            // 局部持有引用: 模态可能在自身 OnEvent 回调中 popModal()/pushModal() 替换自己,
            // 导致 activeModal_ 释放、正在执行的回调闭包随模态一起析构 (use-after-free)。
            // 这里保证模态对象至少存活到本次事件分发结束。
            auto keepAlive = activeModal_;
            keepAlive->OnEvent(event);
            // 模态是阻塞层: 无论模态是否消费该事件, 都不再下发给 main
            // (全局快捷键由外层 CatchEvent 在主界面之前处理, 不受影响)
            return true;
        }
        return main_->OnEvent(event);
    }

private:

    ftxui::Component main_;
    ftxui::Component activeModal_;
    ftxui::Color     bgColor_ = ftxui::Color::Default;
};

} // namespace agentxx::client
