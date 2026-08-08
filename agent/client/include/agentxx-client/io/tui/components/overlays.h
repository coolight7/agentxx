#pragma once

#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/scrollable.h"
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

/// Plan 状态图弹窗组件
///
/// 显示 agentxx_planning_write 工具 roadmap (Mermaid stateDiagram-v2) 的 ASCII 状态图:
/// - 全宽渲染, 内部 Scrollable 滚动 (滚轮 / Up/Down)
/// - 节点按 id 状态后缀着色 (_in_progress/_completed/_failed/_pending)
/// - 弹窗打开期间 plan 消息更新时 (指针/文本长度变化) 重新解析, 其余帧走缓存
///
/// 交互: 滚轮 / Up/Down 滚动, 右上 ✕ 或 Esc 关闭
class PlanDiagramOverlay : public ftxui::ComponentBase {
public:

    explicit PlanDiagramOverlay(TUICtx& ctx);

    void onClose(std::function<void()> fn) {
        onClose_ = std::move(fn);
    }

    bool           OnEvent(ftxui::Event event) override;
    ftxui::Element OnRender() override;

private:

    std::vector<ScrollItem> buildItems();

    TUICtx&               ctx_;
    std::shared_ptr<Scrollable> scrollable_;
    std::function<void()> onClose_;
    ftxui::Box              closeBox_;

    /// plan 消息 JSON 解析缓存: 仅当消息指针/文本长度变化时重新解析
    /// (弹窗打开期间 roadmap 随 agent 执行更新, 指针/长度必然变化)
    const TUIMessage* cachedMsgPtr_ = nullptr;
    size_t            cachedTextLen_ = 0;
    bool              cachedValid_    = false;
    neograph::json    cachedArgs_     = neograph::json::array();
};
