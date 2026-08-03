#pragma once

#include "agentxx-client/io/tui/framework/tui_context.h"
#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/box.hpp"
#include <functional>
#include <vector>

/// 模型选择器弹窗组件 (独立处理键盘导航事件; 每帧重建以反映最新状态)
class ModelSelectorOverlay : public ftxui::ComponentBase {
public:

    explicit ModelSelectorOverlay(TUICtx& ctx) :
        ctx_(ctx) {}

    void setInitialIndex(int idx) {
        selectedIndex_ = idx;
    }

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    void onConfirm(std::function<void(std::string)> fn) {
        onConfirm_ = std::move(fn);
    }

    bool OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

private:

    void confirmSelection();

    TUICtx&                          ctx_;
    int                              selectedIndex_ = 0;
    std::function<void()>            onClose_;
    std::function<void(std::string)> onConfirm_;
};

/// 设置弹窗组件 (主题切换)
class SettingsOverlay : public ftxui::ComponentBase {
public:

    explicit SettingsOverlay(TUICtx& ctx) :
        ctx_(ctx) {}

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    bool OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

private:

    TUICtx&               ctx_;
    int                   selectedIndex_ = 0;
    std::function<void()> onClose_;
};

/// 待发送消息队列弹窗组件
class PendingInputsOverlay : public ftxui::ComponentBase {
public:

    explicit PendingInputsOverlay(TUICtx& ctx) :
        ctx_(ctx) {}

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    bool OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

private:

    bool handleMouse(const ftxui::Mouse& mouse);

    TUICtx&               ctx_;
    std::function<void()> onClose_;

    std::vector<ftxui::Box> itemBoxes_;
    std::vector<ftxui::Box> delBoxes_;
    ftxui::Box              clearBox_;
};

/// 上下文弹窗组件 (显示 llm messages)
class ContextOverlay : public ftxui::ComponentBase {
public:

    explicit ContextOverlay(TUICtx& ctx) :
        ctx_(ctx) {}

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    bool OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

private:

    TUICtx&               ctx_;
    int                   scrollOffset_ = 0;
    std::function<void()> onClose_;
};
