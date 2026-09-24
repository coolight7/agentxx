#include "agentxx-client/io/tui/tui_theme.h"

#include "ftxui/dom/node.hpp"
#include "ftxui/screen/box.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/screen/terminal.hpp"
#include <memory>
#include <utility>

namespace agentxx::client {

namespace {

/// 浅色主题弱化文字时, 前景色向背景色混合的比例 (0 = 不变, 1 = 完全变背景色)
constexpr float kSubduedMix = 0.45F;

/// 把整块元素的前景色向背景色混合的元素 (浅色主题的"弱化文字"实现)
///
/// 与终端 `dim` 属性的区别: 终端按"前景色亮度减半"实现 `dim`, 在浅色背景上
/// 会让深色文字更黑、对比更强; 本元素改为把前景色向背景色靠拢, 对比降低后才
/// 是真正的"弱化"。
///
/// 混合目标逐个单元格取:
/// - 单元格自身背景色 (例如选中行的深色底色) 优先, 这样底色上的文字也是向自己
///   的底色靠拢, 而不是一味向主题背景色变浅
/// - 未设置背景色时取主题背景色 ([TUITheme::backgroundColor])
///
/// 未设置前景色的单元格 (跟随终端默认前景色, 无法参与混合计算) 以
/// [TUITheme::normalColor] 为基准色, 即"变淡的正文色"。
class SubduedNode : public ftxui::Node {
public:

    SubduedNode(ftxui::Element child, ftxui::Color background, ftxui::Color baseColor) :
        Node({std::move(child)}),
        background_(background),
        baseColor_(baseColor) {}

    // 盒子必须转发给子元素 (基类 Node::SetBox 只记录自身盒子)
    void SetBox(ftxui::Box box) override {
        box_ = box;
        children_[0]->SetBox(box);
    }

    void Render(ftxui::Screen& screen) override {
        Node::Render(screen);
        for (int y = box_.y_min; y <= box_.y_max; ++y) {
            for (int x = box_.x_min; x <= box_.x_max; ++x) {
                auto& cell = screen.CellAt(x, y);
                // 清掉子元素带上的 dim 标记: 浅色终端下该标记只会让文字更深
                cell.dim                      = false;
                const ftxui::Color background = (cell.background_color == ftxui::Color::Default)
                                                    ? background_
                                                    : cell.background_color;
                const ftxui::Color foreground = (cell.foreground_color == ftxui::Color::Default)
                                                    ? baseColor_
                                                    : cell.foreground_color;
                cell.foreground_color
                    = ftxui::Color::Interpolate(kSubduedMix, foreground, background);
            }
        }
    }

private:

    ftxui::Color background_;
    ftxui::Color baseColor_;
};

} // namespace

ftxui::Decorator TUITheme::dim() const {
    // 深色主题: 终端 `dim` 属性在深色背景上就是"变淡", 保持原有表现
    //
    // 浅色主题但终端不报告颜色支持 (如终端设了 NO_COLOR): 主题各色都会被降级成
    // 终端默认色, 没有可参与混合的颜色, 这种情况同样退回终端 `dim` 属性
    if (!lightBackground || ftxui::Terminal::ColorSupport() == ftxui::Terminal::Color::Palette1) {
        return ftxui::dim;
    }
    // 颜色按值捕获: 装饰器产出的元素可能被跨帧缓存 (如 LazyScrollable 的
    // 已闭合块), 不能持有主题对象的引用
    const ftxui::Color background = backgroundColor;
    const ftxui::Color baseColor  = normalColor;
    return [background, baseColor](ftxui::Element child) -> ftxui::Element {
        return std::make_shared<SubduedNode>(std::move(child), background, baseColor);
    };
}

TUITheme TUITheme::lightTheme() {
    return TUITheme{
        .name                  = "Light",
        .lightBackground       = true,
        .userColor             = ftxui::Color::RGB(60, 80, 130),
        .assistantColor        = ftxui::Color::RGB(50, 50, 50),
        .thinkingColor         = ftxui::Color::Yellow4,
        .toolColor             = ftxui::Color::RGB(120, 122, 125), // #878889
        .systemColor           = ftxui::Color::RGB(120, 122, 125),
        .errorColor            = ftxui::Color::RGB(200, 30, 30),   // #c81e1e
        .accentColor           = ftxui::Color::RGB(102, 204, 255),
        .normalColor           = ftxui::Color::RGB(50, 50, 50),
        .hintColor             = ftxui::Color::RGB(120, 122, 125),
        .backgroundColor       = ftxui::Color::RGB(255, 255, 255),
        .blockColor            = ftxui::Color::RGB(246, 247, 252),
        .inputBgColor          = ftxui::Color::RGB(246, 247, 252),
        .inputTextColor        = ftxui::Color::RGB(50, 50, 50),
        .buttonBgColor         = ftxui::Color::RGBA(102, 204, 255, 128),    // #66ccff
        .buttonTextColor       = ftxui::Color::RGB(0, 0, 0),       // #fff
        .buttonActiveBgColor   = ftxui::Color::RGB(60, 80, 130),
        .buttonActiveTextColor = ftxui::Color::White,
        // 弹窗配色: 浅色终端下以灰色蒙版衬托白色弹窗表面, 标题栏带强调色浅色调
        .surfaceColor            = ftxui::Color::RGB(255, 255, 255), // #ffffff
        .surfaceHeaderColor      = ftxui::Color::RGB(223, 233, 245), // #dfe9f5
        .surfaceFooterColor      = ftxui::Color::RGB(238, 242, 247), // #eef2f7
        .surfaceTitleColor       = ftxui::Color::RGB(60, 80, 130),   // 深蓝, 在浅色标题栏上清晰
        .surfaceErrorHeaderColor = ftxui::Color::RGB(252, 233, 233), // #fce9e9
        .surfaceScrimColor       = ftxui::Color::RGB(232, 236, 241), // #e8ecf1 (灰蒙版)
        .markdownTheme           = markdown::Theme{
            .name        = "Light",
            .syntax      = ftxui::color(ftxui::Color::Yellow4),
            .gutter      = ftxui::color(ftxui::Color::Grey53),
            .heading1    = ftxui::Decorator(ftxui::bold) | ftxui::underlined
                        | ftxui::color(ftxui::Color::Blue3),
            .heading2    = ftxui::bold | ftxui::color(ftxui::Color::Blue3),
            .heading3    = ftxui::Decorator(ftxui::bold) | ftxui::color(ftxui::Color::Grey37),
            .link        = ftxui::color(ftxui::Color::Blue3),
            .code_inline = ftxui::color(ftxui::Color::RGB(150, 80, 0)),
            .code_block  = ftxui::bgcolor(ftxui::Color::RGB(246, 247, 252)) // blockColor #f6f7fc
                        | ftxui::color(ftxui::Color::RGB(50, 50, 50)),
            .blockquote  = ftxui::color(ftxui::Color::RGB(120, 122, 125)),  // 引用
            .table_header = ftxui::color(ftxui::Color::RGB(50, 50, 50)) | ftxui::bold, // 表格
            .table_border = ftxui::color(ftxui::Color::RGB(50, 50, 50)),
            .diagram_pending  = ftxui::Color::RGB(120, 122, 125), // hintColor
            .diagram_running  = ftxui::Color::Yellow4,            // thinkingColor
            .diagram_done     = ftxui::Color::RGB(102, 204, 255), // accentColor
            .diagram_failed   = ftxui::Color::RGB(200, 30, 30),   // errorColor
        },
    };
}

TUITheme TUITheme::darkTheme() {
    return TUITheme{
        .name                  = "Dark",
        .lightBackground       = false,
        .userColor             = ftxui::Color::RGB(102, 204, 255),
        .assistantColor        = ftxui::Color::RGB(220, 220, 220), 
        .thinkingColor         = ftxui::Color::RGB(245, 245, 52),  
        .toolColor             = ftxui::Color::RGB(140, 140, 140), 
        .systemColor           = ftxui::Color::RGB(140, 140, 140),
        .errorColor            = ftxui::Color::RGB(255, 85, 85),   // #ff5555
        .accentColor           = ftxui::Color::RGB(102, 204, 255), // #66ccff
        .normalColor           = ftxui::Color::RGB(220, 220, 220),
        .hintColor             = ftxui::Color::RGB(140, 140, 140),
        .backgroundColor       = ftxui::Color::RGB(0, 0, 0),       // #000
        .blockColor            = ftxui::Color::RGB(25, 30, 35),    // #121212
        .inputBgColor          = ftxui::Color::RGB(25, 30, 35),    //
        .inputTextColor        = ftxui::Color::RGB(220, 220, 220), // #fff
        .buttonBgColor         = ftxui::Color::RGBA(102, 204, 255, 100),    // #66ccff
        .buttonTextColor       = ftxui::Color::RGB(0, 0, 0),       // #fff
        .buttonActiveBgColor   = ftxui::Color::RGB(102, 204, 255), // #66ccff
        .buttonActiveTextColor = ftxui::Color::RGB(0, 0, 0),       // #000
        // 弹窗配色: 内容区比整体背景 (#000) 稍亮, 标题栏再亮一档并带强调色色调,
        // 底栏介于两者之间, 使同一弹窗内三个区域可仅凭背景色区分 (无边框/分割线);
        // 表面与蒙版的明度差同时决定圆角的可见度
        .surfaceColor            = ftxui::Color::RGB(27, 30, 36),  // #1b1e24
        .surfaceHeaderColor      = ftxui::Color::RGB(40, 50, 66),  // #283242
        .surfaceFooterColor      = ftxui::Color::RGB(33, 37, 44),  // #21252c
        .surfaceTitleColor       = ftxui::Color::RGB(102, 204, 255), // accentColor
        .surfaceErrorHeaderColor = ftxui::Color::RGB(58, 22, 24),  // #3a1618
        .surfaceScrimColor       = ftxui::Color::RGB(0, 0, 0),     // 同整体背景
        .markdownTheme           = markdown::Theme{
            .name        = "Dark",
            .syntax      = ftxui::color(ftxui::Color::RGB(245, 245, 52)),
            .gutter      = ftxui::color(ftxui::Color::RGB(140, 140, 140)),
            .heading1    = ftxui::Decorator(ftxui::bold) | ftxui::underlined
                        | ftxui::color(ftxui::Color::RGB(102, 204, 255)),
            .heading2    = ftxui::color(ftxui::Color::RGB(102, 204, 255)) | ftxui::bold,
            .heading3    = ftxui::Decorator(ftxui::bold),
            .link        = ftxui::color(ftxui::Color::RGB(102, 204, 255)),
            .code_inline = ftxui::color(ftxui::Color::RGB(245, 245, 52)),
            .code_block  = ftxui::bgcolor(ftxui::Color::RGB(25, 30, 35)) // blockColor #121212
                        | ftxui::color(ftxui::Color::RGB(180, 180, 180)),
            .blockquote  = ftxui::color(ftxui::Color::RGB(140, 140, 140)),
            .table_header = ftxui::color(ftxui::Color::RGB(220, 220, 220)) | ftxui::bold,
            .table_border = ftxui::color(ftxui::Color::RGB(220, 220, 220)),
            .diagram_pending  = ftxui::Color::RGB(140, 140, 140), // hintColor
            .diagram_running  = ftxui::Color::RGB(245, 245, 52),  // thinkingColor
            .diagram_done     = ftxui::Color::RGB(102, 204, 255), // accentColor
            .diagram_failed   = ftxui::Color::RGB(255, 85, 85),   // errorColor
        },
    };
}

} // namespace agentxx::client
