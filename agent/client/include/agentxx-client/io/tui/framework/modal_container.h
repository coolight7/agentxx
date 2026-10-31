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
/// - 鼠标拖选: 弹窗内容同样支持拖选复制 (弹窗文本多是 text/paragraph/markdown 节点,
///   本身可被 FTXUI 选中)。FTXUI 只在"组件树未处理该事件"时记录选择
///   (见 App::HandleSelection: 已处理会立即清空选择), 而弹窗组件对鼠标事件多数
///   一律返回 true, 因此左键按下/移动固定对外报"未处理" —— 这仅影响 FTXUI 的
///   选择跟踪, 弹窗自身已收到同一事件, 交互不变
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
            const bool handled = keepAlive->OnEvent(event);
            // 模态是阻塞层: 无论模态是否处理该事件, 都不再下发给 main
            // (全局快捷键由外层 CatchEvent 在主界面之前处理, 不受影响)
            if (!event.is_mouse()) {
                return true;
            }
            // 左键按下/移动对外报"未处理", 使 FTXUI 能为弹窗内的文本记录拖选
            // (返回值只影响 FTXUI 的选择跟踪, 事件已经交给弹窗处理过一遍)。
            // 松开仍回报弹窗的处理结果: 单击未拖动时选择状态被清掉, 不留残影;
            // 拖选松开时外层 CatchEvent 已先行完成复制并吃掉事件。
            const auto& mouse = event.mouse();
            if (mouse.button == ftxui::Mouse::Left && mouse.motion != ftxui::Mouse::Released) {
                return false;
            }
            return handled;
        }
        return main_->OnEvent(event);
    }

private:

    ftxui::Component main_;
    ftxui::Component activeModal_;
    ftxui::Color     bgColor_ = ftxui::Color::Default;
};

} // namespace agentxx::client
