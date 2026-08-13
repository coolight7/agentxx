#pragma once

#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/scrollable.h"
#include "ftxui/component/component_base.hpp"
#include "ftxui/dom/elements.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

/// 右侧边栏组件: tab 栏 + 可滚动内容 + 底部常驻 + 左侧拖拽手柄
///
/// 事件处理:
/// - tab 左键点击切换, 右键关闭
/// - 左侧手柄拖拽调整宽度
/// - 滚轮由内部 Scrollable 处理
class SidebarComponent : public ftxui::ComponentBase {
public:

    struct Tab {
        std::string                              id;
        std::string                              title;
        std::function<std::vector<ScrollItem>()> render;
        std::function<ftxui::Element()>          footer;
    };

    explicit SidebarComponent(TUICtx& ctx);

    void addTab(
        std::string_view                         id,
        std::string_view                         title,
        std::function<std::vector<ScrollItem>()> render,
        std::function<ftxui::Element()>          footer = nullptr
    );
    void removeTab(std::string_view id);
    bool hasTab(std::string_view id) const;

    bool empty() const {
        return tabs_.empty();
    }

    /// 当前激活 tab 的 id (无 tab 时返回空 string_view)
    std::string_view activeTabId() const {
        if (activeTab_ >= 0 && activeTab_ < static_cast<int>(tabs_.size())) {
            return tabs_[activeTab_].id;
        }
        return {};
    }

    /// 指定 tab 是否为当前激活 tab (供外部判断其渲染是否对可见 UI 有影响,
    /// 如日志窗口未激活时日志更新无需触发重绘)
    bool isTabActive(std::string_view id) const {
        return activeTabId() == id;
    }

    /// 侧边栏宽度 (供外部布局使用)
    int width() const {
        return width_;
    }

    /// 设置 footer 区域点击回调 (如 "上下文" 按钮)
    void onFooterClick(std::function<bool(const ftxui::Mouse&)> fn) {
        onFooterClick_ = std::move(fn);
    }

    /// 清除侧边栏可见项的鼠标选中高亮 (拖选松开复制完成后调用;
    /// 转发给 scrollable_ 的 resetSelectionHighlight)
    void clearSelectionHighlight() {
        scrollable_->resetSelectionHighlight();
    }

    ftxui::Element OnRender() override;
    bool           OnEvent(ftxui::Event event) override;

private:

    bool handleTabMouse(const ftxui::Mouse& mouse);
    bool handleResizeMouse(const ftxui::Mouse& mouse);

    TUICtx&                     ctx_;
    std::shared_ptr<Scrollable> scrollable_;

    std::vector<Tab> tabs_;
    int              activeTab_ = 0;

    int  width_        = kDefaultWidth;
    bool resizing_     = false;
    int  resizeStartX_ = 0;
    int  resizeStartW_ = 0;

    std::vector<ftxui::Box> tabBoxes_;
    ftxui::Box              handleBox_;
    ftxui::Box              footerBox_;

    std::function<bool(const ftxui::Mouse&)> onFooterClick_;

    static constexpr int kMinWidth     = 24;
    static constexpr int kMaxWidth     = 120;
    static constexpr int kDefaultWidth = 40;
};
