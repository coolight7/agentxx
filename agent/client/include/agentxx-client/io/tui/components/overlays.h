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

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

private:

    void confirmSelection();

    TUICtx&                          ctx_;
    int                              selectedIndex_ = 0;
    std::function<void()>            onClose_;
    std::function<void(std::string)> onConfirm_;
};

/// 设置弹窗组件
/// - 主题切换 (Dark/Light)
/// - 系统资源占用显示开关 (Info 侧边栏; 默认开启)
/// - 动画等级 (Disabled/Low/Medium/High/Ultra; 见 TUISettings)
///
/// 交互: Up/Down 选择条目, Enter 应用/切换 (动画等级循环切换); 也支持鼠标点击
class SettingsOverlay : public ftxui::ComponentBase {
public:

    explicit SettingsOverlay(TUICtx& ctx) :
        ctx_(ctx) {}

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

private:

    bool handleMouse(const ftxui::Mouse& mouse);

    /// 循环切换动画等级: Disabled -> Low -> Medium -> High -> Ultra -> Disabled
    static void cycleAnimationLevel();

    TUICtx&               ctx_;
    /// 条目索引: 0/1 = 主题 Dark/Light, 2 = 系统资源显示开关, 3 = 动画等级
    /// Enter/鼠标点击索引 3 时循环切换动画等级
    static constexpr int kItemCount = 4;
    int                  selectedIndex_ = 0;
    std::function<void()> onClose_;

    ftxui::Box themeBoxes_[2]; // Dark/Light 点击区域
    ftxui::Box sysInfoBox_;    // 系统资源开关点击区域
    ftxui::Box animLevelBox_;  // 动画等级点击区域
};

/// 待发送消息队列弹窗组件
class PendingInputsOverlay : public ftxui::ComponentBase {
public:

    explicit PendingInputsOverlay(TUICtx& ctx) :
        ctx_(ctx) {}

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    bool           OnEvent(ftxui::Event event) override;
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

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

private:

    TUICtx&               ctx_;
    int                   scrollOffset_ = 0;
    std::function<void()> onClose_;
};
