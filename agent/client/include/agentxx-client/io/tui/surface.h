#pragma once

/// 弹窗面性风格外框
///
/// 设计: 浮层不使用边框与分割线, 改为把浮层分成若干横向区域, 各区域以主题背景色
/// 区分 —— 标题栏 ([TUITheme::surfaceHeaderColor]) / 内容区
/// ([TUITheme::surfaceColor]) / 底部提示栏 ([TUITheme::surfaceFooterColor]);
/// 错误类浮层标题栏改用 [TUITheme::surfaceErrorHeaderColor] + 错误色标题文字。
///
/// 整行背景铺满由 vbox 的交叉轴拉伸实现 (FTXUI 的 VBox 把父级 x 范围传给每个
/// 子节点), 故区域行只需叠加 bgcolor, 无需再画横线; 弹窗之间的层次感由弹窗打开时
/// 铺满屏幕的 [TUITheme::surfaceScrimColor] 衬托 (见 ModalContainer::setBgColor)。
///
/// 颜色由调用方从当前主题取值传入, 本模块不绑定 TUITheme, 便于错误类浮层等变体复用。
#include "ftxui/dom/elements.hpp"
#include <string>
#include <string_view>

/// 浮层标题栏 (纯文本标题)
/// - 整行填充 [bg], 标题文字 [fg] 加粗, 左右自动各留 1 列内边距
///
/// - `args`:
///     - [title] 标题文本 (不含内边距)
///     - [fg] 常规浮层用 TUITheme::surfaceTitleColor, 错误类浮层用 errorColor
///     - [bg] 常规浮层用 TUITheme::surfaceHeaderColor, 错误类浮层用 surfaceErrorHeaderColor
inline ftxui::Element
    tuiSurfaceTitleBar(std::string_view title, const ftxui::Color& fg, const ftxui::Color& bg) {
    return ftxui::text(" " + std::string(title) + " ") | ftxui::bold | ftxui::color(fg)
           | ftxui::bgcolor(bg);
}

/// 浮层标题栏 (自定义内容: 如标题文字 + 右侧按钮), 整行填充 [bg]
inline ftxui::Element tuiSurfaceTitleBarContent(ftxui::Element content, const ftxui::Color& bg) {
    return ftxui::hbox({std::move(content), ftxui::filler()}) | ftxui::bgcolor(bg);
}

/// 浮层区域留白行 (填充背景 [bg] 的整行空行)
/// - 用于标题栏/底栏与内容区之间的间距: 面性风格下间距由内容区背景色填充,
///   而非空行留白 (空行会露出下层背景, 使浮层表面出现断口)
inline ftxui::Element tuiSurfacePadRow(const ftxui::Color& bg) {
    return ftxui::text("") | ftxui::bgcolor(bg);
}

/// 浮层底部提示栏 (整行填充 [bg], 提示文字 [fg] 居中)
inline ftxui::Element
    tuiSurfaceFooterBar(std::string_view hint, const ftxui::Color& fg, const ftxui::Color& bg) {
    return ftxui::text(hint) | ftxui::center | ftxui::color(fg) | ftxui::bgcolor(bg);
}
