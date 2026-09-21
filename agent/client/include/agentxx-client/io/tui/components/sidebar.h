#pragma once

#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/ui_hit.h"
#include "agentxx-client/io/tui/scrollable.h"
#include "ftxui/component/component_base.hpp"
#include "ftxui/dom/elements.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace agentxx::client {

/// 右侧边栏组件: 横向布局 = [当前高亮 tab 内容] | [tabs 竖向可滚动列表] + 左侧拖拽手柄
///
/// - 左侧内容区: 当前激活 tab 的可滚动内容 + 底部 footer; 无激活 tab 时不渲染
///   (此时侧边栏收窄为仅剩 tabs 列表)
/// - 右侧 tabs 竖向列表: 常驻显示, 超出显示高度时可滚动 (复用 Scrollable);
///   常驻标签 (如 Info/Logs) 固定显示于列表顶部, 对应 tab 未创建时也可点击创建
///
/// 事件处理 (命中区域经 [agentxx::client::UiHitMap] 登记, 每帧渲染时重建):
/// - tab 左键点击切换/取消激活 (常驻标签已激活时再点一次取消激活, 内容区隐藏),
///   右键关闭动态 tab (常驻标签右键仅取消激活, 按钮不可移除)
/// - 左侧手柄拖拽调整宽度 (作用于有内容时的整体宽度)
/// - 内容区/列表滚轮分别由各自内部 Scrollable 处理
/// - footer 区域的按钮属于 shell 级动作 (见 TUIClientAgentIO::shellHits_),
///   本组件不再自行维护 footer 命中区域
class SidebarComponent : public ftxui::ComponentBase {
public:

    struct Tab {
        std::string                              id;
        std::string                              title;
        std::function<std::vector<ScrollItem>()> render;
        std::function<ftxui::Element()>          footer;
    };

    /// 常驻标签: 始终显示于 tabs 竖向列表顶部的固定按钮
    struct PinnedTab {
        std::string id;
        std::string title;
        /// 左键点击且对应 tab 不存在时调用 (内部应 addTab 创建; addTab 自动激活新 tab)
        std::function<void()> ensure;
    };

    explicit SidebarComponent(TUICtx& ctx);

    /// 注册常驻标签 (如 Info/Logs); 列表中固定显示于动态 tab 之前。
    /// 界面语言切换时外部会以新语言标题重新调用 (标题随语言刷新)
    void setPinnedTabs(std::vector<PinnedTab> pins) {
        pinned_ = std::move(pins);
    }

    void addTab(
        std::string_view                         id,
        std::string_view                         title,
        std::function<std::vector<ScrollItem>()> render,
        std::function<ftxui::Element()>          footer = nullptr
    );
    void removeTab(std::string_view id);
    bool hasTab(std::string_view id) const;

    /// 更新已存在 tab 的标题 (界面语言切换后由外部调用刷新标签按钮文本;
    /// 不存在时忽略)
    void setTabTitle(std::string_view id, std::string_view title) {
        for (auto& tab : tabs_) {
            if (tab.id == id) {
                tab.title = std::string{title};
                return;
            }
        }
    }

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

    /// 侧边栏宽度 (供外部布局使用; 仅在有激活 tab 内容时生效)
    int width() const {
        return width_;
    }

    /// 清除侧边栏可见项的鼠标选中高亮 (拖选松开复制完成后调用;
    /// 转发给 scrollable_ 的 resetSelectionHighlight)
    void clearSelectionHighlight() {
        scrollable_->resetSelectionHighlight();
    }

    /// 内容区滚动容器 (当前激活 tab 的子项与命中区域由它持有)
    ///
    /// 用途: 面板/Info 段落内的可点位置 (按钮、表格单元格、折叠标题、控件) 的命中
    /// 判定 —— 经 [Scrollable::hitTestItem] 把屏幕坐标映射到"第几个子项 + 子项内
    /// 局部坐标", 再与子项登记的可命中区域比对。不要用子项元素内的 `reflect`
    /// 判定 (滚动容器测量子项时会以临时大框布局, 视口外子项的反射框会残留)。
    Scrollable* contentScrollable() const {
        return scrollable_.get();
    }

    ftxui::Element OnRender() override;
    bool           OnEvent(ftxui::Event event) override;

private:

    /// tab 列表按钮命中载荷: 指向被点的条目 (常驻标签或动态 tab)
    struct TabHit {
        bool isPin = false;
        int  index = -1; ///< isPin ? pinned_ 下标 : tabs_ 下标
    };

    /// 命中项分类 (侧边栏内两类命中: tab 列表按钮 / 左侧拖拽手柄)
    struct HitInfo {
        enum class Kind : uint8_t {
            TabButton,   ///< tabs 竖向列表按钮 (载荷看 [TabHit])
            ResizeHandle ///< 左侧宽度拖拽手柄
        };

        Kind   kind = Kind::TabButton;
        TabHit tab;
    };

    /// 构建本帧 tabs 竖向列表按钮 (命中登记进 hits_)
    void buildTabList();
    bool handleListMouse(const ftxui::Mouse& mouse);
    bool handleResizeMouse(const ftxui::Mouse& mouse);

    /// 返回指定 id 的 tab 下标; 不存在返回 -1
    int  findTabIndex(std::string_view id) const;
    bool isPinned(std::string_view id) const;

    TUICtx&                     ctx_;
    std::shared_ptr<Scrollable> scrollable_; // 当前激活 tab 的内容区
    std::shared_ptr<Scrollable> tabList_;    // tabs 竖向常驻列表

    std::vector<Tab>       tabs_;
    std::vector<PinnedTab> pinned_;
    int                    activeTab_ = -1; // -1 = 无激活 tab (内容区不渲染)

    int  width_        = kDefaultWidth;
    bool resizing_     = false;
    int  resizeStartX_ = 0;
    int  resizeStartW_ = 0;

    std::vector<ScrollItem> pendingListItems_; // 本帧待渲染的列表按钮 (tabList_ 的 render 源)

    /// 命中登记表 (每帧 OnRender 帧首清空; tab 按钮与拖拽手柄共用):
    /// 载荷直接带条目归属, 无需再维护"可见子项下标 → tabs_/pinned_ 下标"的映射表,
    /// 未渲染的按钮 (列表滚动到视口外) 也不会命中
    agentxx::client::UiHitRegistry<HitInfo> hits_;

    // 横向布局下内容区与竖向列表共享宽度: 相比原垂直布局适度加宽,
    // 保证左侧内容区在列表占据约 10 列后仍有可用宽度
    static constexpr int kMinWidth     = 28;
    static constexpr int kMaxWidth     = 120;
    static constexpr int kDefaultWidth = 46;
};

} // namespace agentxx::client
