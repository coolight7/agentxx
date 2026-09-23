#pragma once

/// 声明式动作列表 (菜单 / 设置项 / 弹窗列表项的统一实现)
///
/// 同一个交互模式在 TUI 里出现了很多次: "若干可选项 + 上下键移动 + Enter 确认
/// + 鼠标点击命中 + 选中项高亮"。此前每个弹窗各写一份: 每个条目一个
/// `ftxui::Box` 成员、`handleMouse()` 里逐项 `Contain` 分支、`OnEvent` 里按
/// `selectedIndex_` 的魔法数字写 `if (selectedIndex_ == 2) ...`。加一个条目要
/// 改 4 处 (成员/渲染分支/键盘分支/鼠标分支), 漏改就出现"点击无反应"或
/// "点错了项"的问题。
///
/// 本类把这些收敛为一张**数据表**: 条目 = (id, 文本, 当前值, 激活动作), 交互与
/// 渲染只有一份实现:
/// - 上下/Home/End 移动选中项 (跳过 disabled 项), Enter 激活
/// - 鼠标左键释放命中条目时先置选中再激活 (键盘与鼠标路径行为一致)
/// - 命中区域经 [UiHitMap] 登记: 每帧重建, 未渲染的条目不会命中
/// - 渲染默认整行高亮 (文本 + 右对齐值), 需要多行/自定义版式时传 rowBuilder
/// - 条目可声明分组标题 ([UiActionItem::group]): 分组首项前插一行标题, 标题
///   不参与选中与点击, 只用来分组显示 (如设置弹窗的 界面/显示/更新/其他)
///
/// 用法:
/// ```c++
/// // 构造时声明一次, 或每帧按状态重建 (选中项按 id 保持)
/// list_.setItems({
///     {.id = "theme", .label = tr("settings.themeLabel"),
///      .value = currentThemeName(), .onActivate = [this] { cycleTheme(); }},
///     {.id = "about", .label = tr("settings.infoLabel"),
///      .value = std::string(tr("settings.aboutValue")), .onActivate = [this] { openAbout(); }},
/// });
///
/// ftxui::Element OnRender() override {
///     hits_.beginFrame();
///     return tuiSurfacePopup(style, title, list_.render(hits_, style_), hint);
/// }
///
/// bool OnEvent(ftxui::Event event) override {
///     if (event == ftxui::Event::Escape) { close(); return true; }
///     if (event.is_mouse() ? list_.onMouseEvent(event.mouse(), hits_) : list_.onKeyEvent(event)) {
///         postRedraw();
///         return true;
///     }
///     return true;   // 弹窗吞掉其余事件
/// }
/// ```
#include "agentxx-client/io/tui/framework/ui_hit.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace client {

/// 可点击条目 (菜单项/设置项/列表项的统一描述)
struct UiActionItem {
    /// 命中标识 (同一列表内唯一; 选中项按它跨重建保持)
    std::string id;
    /// 主文本
    std::string label;
    /// 次级说明 (弱化色, 显示在主文本后; 空 = 不显示)
    std::string hint;
    /// 所属分组标题 (空 = 不分组)
    ///
    /// 渲染时在该分组首项之前插入一行标题 (标题行不参与选中与点击); 与上一项
    /// 标题不同即视为新分组, 标题相同的连续条目属同一组。
    std::string group;
    /// 是否可激活 (false: 弱化显示, 键盘跳过, 点击无效)
    bool enabled = true;
    /// 激活动作 (Enter 或鼠标点击命中); 为空时仅把该项置为选中
    std::function<void()> onActivate;
};

/// 条目渲染样式
struct UiActionStyle {
    /// 未选中项文字色
    ftxui::Color normalFg;
    /// 未选中项背景 (默认透明)
    ftxui::Color normalBg = ftxui::Color::Default;
    /// 选中项文字色
    ftxui::Color selectedFg;
    /// 选中项背景
    ftxui::Color selectedBg;
    /// 不可用项文字色
    ftxui::Color disabledFg;
    /// 分组标题文字色 (见 [UiActionItem::group])
    ftxui::Color groupFg = ftxui::Color::Default;

    /// 取主题按钮配色 (弹窗内列表项的默认样式)
    static UiActionStyle fromTheme(const TUITheme& theme);

    /// 单行行元素: 主文本 + (可选次级说明) + 右对齐值; 整行铺满便于高亮
    ftxui::Element row(const UiActionItem& item, bool selected) const;

    /// 分组标题行 (整行; 不参与命中, 不可点击)
    ftxui::Element groupRow(std::string_view title) const;
};

/// 声明式动作列表 (状态 + 交互 + 渲染; 由持有它的组件在 OnRender/OnEvent 中驱动)
class UiActionList {
public:

    using Item = UiActionItem;
    /// 自定义行渲染 (返回的整个元素即命中区域; 需要"只有值行可点"等版式时自行拆分)
    using RowBuilder
        = std::function<ftxui::Element(const UiActionItem& item, bool selected, size_t index)>;

    /// 重建条目表 (每帧或数据变化时调用)
    /// - 选中项按 id 保持 (条目顺序变化/增删后仍指向同一项)
    /// - 原选中项消失时按原下标收敛到有效范围
    /// - 分组标题行只是版式: 不占条目下标, 键盘导航仍在条目之间移动
    void setItems(std::vector<UiActionItem> items);

    /// 条目之间的空行数 (默认 0; 与 rowBuilder 一起决定整体版式)
    void setRowGap(int lines) {
        rowGap_ = lines < 0 ? 0 : lines;
    }

    /// 键盘事件: Up/Down/Home/End 移动选中项, Enter 激活选中项
    /// - 返回 true 表示事件已消费 (调用方应直接返回)
    /// - 未消费的键 (如 Esc/PgUp/PgDn) 由调用方继续处理
    bool onKeyEvent(const ftxui::Event& event);

    /// 鼠标事件: 左键释放命中条目时置选中并激活; 返回是否消费
    /// (命中项的 id 不在本列表内时返回 false, 调用方仍可处理该命中)
    bool onMouseEvent(const ftxui::Mouse& mouse, UiHitMap& hits);

    /// 渲染条目 (逐项登记命中区域; rowBuilder 为空时用 [UiActionStyle::row])
    ftxui::Element
        render(UiHitMap& hits, const UiActionStyle& style, RowBuilder rowBuilder = {}) const;

    /// 选中项下标 (-1 = 列表为空)
    int selectedIndex() const;

    /// 设置选中项下标 (越界自动收敛)
    void setSelectedIndex(int index);

    /// 按 id 选中 (不存在则忽略)
    void selectById(std::string_view id);

    size_t size() const {
        return items_.size();
    }

    bool empty() const {
        return items_.empty();
    }

    const std::vector<UiActionItem>& items() const {
        return items_;
    }

    /// 选中项 (列表为空时返回 nullptr)
    const UiActionItem* selected() const;

    /// 激活指定下标的条目 (越界/不可用返回 false)
    bool activateIndex(int index);

    /// 选中项变化回调 (供外部做滚动跟随 / 分页预取等; 参数为新下标)
    void onSelectionChanged(std::function<void(int)> fn) {
        onSelectionChanged_ = std::move(fn);
    }

private:

    /// 按 delta 移动选中项 (跳过 disabled 项; 越界即停, 不循环)
    void moveSelection(int delta);

    /// 跳到首个/末个可用项
    void moveSelectionEdge(bool toLast);

    std::vector<UiActionItem> items_;

    /// 选中项下标 (始终落在 [0, size-1]; 列表为空时无意义)
    int selectedIndex_ = 0;

    /// 选中项 id (setItems 后据此恢复选中项)
    std::string selectedId_;

    /// 条目之间插入的空行数
    int rowGap_ = 0;

    std::function<void(int)> onSelectionChanged_;
};

} // namespace client
} // namespace agentxx
