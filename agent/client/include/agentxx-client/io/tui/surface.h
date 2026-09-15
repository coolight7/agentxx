#pragma once

/// 弹窗面性风格外框
///
/// **风格**: 弹窗是一块纯色填充的圆角矩形 —— 只画四角圆角, 不画边框线与分割线,
/// 内部各区域以背景色区分。
///
/// **外框统一提供的留白** (调用方不需要再为标题/内容行补左右留白):
/// - 弹窗整体上下左右各 1 格内边距 (以内容区背景色 [TuiSurfaceStyle::body] 填充)
/// - 标题栏 / 内容区 / 底部提示栏之间各 1 行间距 (同样以 [TuiSurfaceStyle::body] 填充)
/// - 四角圆角: 角格绘制圆角字符, 字符前景取 [TuiSurfaceStyle::body], 背景取弹窗外部色
///   [TuiSurfaceStyle::outside] (弹窗打开时的蒙版色), 使填充块四角呈圆角
///
/// **组装顺序**:
/// - 纵向: 上内边距 (圆角行) → 标题栏 → 间距 → 内容区 → 间距 → 底部提示栏 → 下内边距 (圆角行)
/// - 横向: 1 格内边距 + 区域内容 (区域整行宽度由 vbox 交叉轴拉伸铺满)
///
/// 用法:
/// ```c++
/// const auto style = TuiSurfaceStyle::fromTheme(*ctx_.theme);
/// return tuiSurfacePopup(style, tr("model.title"), list, tr("model.hint"))
///        | size(WIDTH, LESS_THAN, 50);
/// ```
/// 错误类弹窗可在 fromTheme 结果上改写标题栏配色:
/// ```c++
/// auto style   = TuiSurfaceStyle::fromTheme(*ctx_.theme);
/// style.header = ctx_.theme->surfaceErrorHeaderColor;
/// style.title  = ctx_.theme->errorColor;
/// ```
#include "agentxx-client/io/tui/tui_theme.h"
#include "ftxui/dom/elements.hpp"
#include <string>
#include <string_view>
#include <utility>

/// 弹窗面性风格配色 (取值自当前主题, 见 TUITheme::surface*)
struct TuiSurfaceStyle {
    ftxui::Color body;    ///< 内容区背景 (同时填充内边距与区域间距)
    ftxui::Color header;  ///< 标题栏背景
    ftxui::Color footer;  ///< 底部提示栏背景
    ftxui::Color title;   ///< 标题文字
    ftxui::Color hint;    ///< 底部提示文字
    ftxui::Color outside; ///< 弹窗外部背景 (圆角外侧; 弹窗打开时为蒙版色)

    /// 取当前主题的默认面性配色
    static TuiSurfaceStyle fromTheme(const TUITheme& theme) {
        return TuiSurfaceStyle{
            .body    = theme.surfaceColor,
            .header  = theme.surfaceHeaderColor,
            .footer  = theme.surfaceFooterColor,
            .title   = theme.surfaceTitleColor,
            .hint    = theme.hintColor,
            .outside = theme.surfaceScrimColor,
        };
    }
};

/// 标题栏行 (纯文字标题; 左右留白由外框提供, 文字无需自带空格)
inline ftxui::Element
    tuiSurfaceHeaderBar(std::string_view title, const ftxui::Color& fg, const ftxui::Color& bg) {
    return ftxui::text(title) | ftxui::bold | ftxui::color(fg) | ftxui::bgcolor(bg);
}

/// 标题栏行 (自定义内容: 如标题文字 + 右侧按钮), 整行填充 [bg]
/// - 自定义行内的排版由调用方决定 (外框只负责整行拉伸与左右内边距)
inline ftxui::Element tuiSurfaceHeaderRow(ftxui::Element content, const ftxui::Color& bg) {
    return std::move(content) | ftxui::bgcolor(bg);
}

/// 底部提示行 (整行填充 [bg], 提示文字 [fg] 居中)
inline ftxui::Element
    tuiSurfaceFooterBar(std::string_view hint, const ftxui::Color& fg, const ftxui::Color& bg) {
    return ftxui::text(hint) | ftxui::center | ftxui::color(fg) | ftxui::bgcolor(bg);
}

/// 间距行 (1 行高, 整行填充 [bg]); 供外框与需要额外间距的弹窗使用
inline ftxui::Element tuiSurfaceGapRow(const ftxui::Color& bg) {
    return ftxui::text("") | ftxui::bgcolor(bg);
}

/// 圆角行 (上下内边距所在行): 左右角格绘制圆角字符, 其余整行填充 [body]
/// - 角格前景取 [body] (与填充同色), 背景取 [outside] (弹窗外部色),
///   使填充块的这一角呈圆角
inline ftxui::Element tuiSurfaceCornerRow(
    const ftxui::Color& body,
    const ftxui::Color& outside,
    std::string_view    leftCorner,
    std::string_view    rightCorner
) {
    return ftxui::hbox({
               ftxui::text(std::string(leftCorner)) | ftxui::color(body) | ftxui::bgcolor(outside),
               ftxui::filler(),
               ftxui::text(std::string(rightCorner)) | ftxui::color(body) | ftxui::bgcolor(outside),
           })
           | ftxui::bgcolor(body);
}

/// 弹窗外框 (自定义标题栏/底栏行)
/// - [headerBar]/[footerBar] 通常由 tuiSurfaceHeaderBar / tuiSurfaceHeaderRow /
///   tuiSurfaceFooterBar 构造
/// - [content] 为内容区元素 (列表/滚动区等), 无需自带左右留白;
///   弹窗高度被撑大时由内容区吸收 (内容为可变高度时应加 `| flex`)
inline ftxui::Element tuiSurfaceFrame(
    const TuiSurfaceStyle& style,
    ftxui::Element         headerBar,
    ftxui::Element         content,
    ftxui::Element         footerBar
) {
    // 区域纵向排列: 区域之间各留 1 行间距
    ftxui::Element regions = ftxui::vbox({
        std::move(headerBar),
        tuiSurfaceGapRow(style.body),
        std::move(content),
        tuiSurfaceGapRow(style.body),
        std::move(footerBar),
    });

    // 左右各 1 格内边距 (整列填充内容区背景色); yflex 使弹窗高度被撑大时
    // 由外框吸收多余高度, 保证底部圆角行始终贴着弹窗下边界 (不出现镂空)
    ftxui::Element middle = ftxui::hbox({
                                ftxui::text("  ") | ftxui::bgcolor(style.body),
                                std::move(regions) | ftxui::xflex,
                                ftxui::text("  ") | ftxui::bgcolor(style.body),
                            })
                            | ftxui::bgcolor(style.body) | ftxui::yflex;

    // 上下各 1 行内边距 (同时是四角圆角所在行)
    return ftxui::vbox({
        tuiSurfaceCornerRow(style.body, style.outside, "+ ", " +"),
        std::move(middle),
        tuiSurfaceCornerRow(style.body, style.outside, "+ ", " +"),
    });
}

/// 弹窗外框 (常用形态: 文字标题 + 内容 + 文字提示)
inline ftxui::Element tuiSurfacePopup(
    const TuiSurfaceStyle& style,
    std::string_view       title,
    ftxui::Element         content,
    std::string_view       hint
) {
    return tuiSurfaceFrame(
        style,
        tuiSurfaceHeaderBar(title, style.title, style.header),
        std::move(content),
        tuiSurfaceFooterBar(hint, style.hint, style.footer)
    );
}
