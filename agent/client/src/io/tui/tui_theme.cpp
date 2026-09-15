#include "agentxx-client/io/tui/tui_theme.h"

TUITheme TUITheme::darkTheme() {
    return TUITheme{
        .name                  = "Dark",
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
        .blockColor            = ftxui::Color::RGB(18, 18, 18),    // #121212
        .inputBgColor          = ftxui::Color::RGB(24, 26, 30),    //
        .inputTextColor        = ftxui::Color::RGB(220, 220, 220), // #fff
        .buttonBgColor         = ftxui::Color::RGBA(102, 204, 255, 128),    // #66ccff
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
            .code_block  = ftxui::bgcolor(ftxui::Color::RGB(18, 18, 18)) // blockColor #121212
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

TUITheme TUITheme::lightTheme() {
    return TUITheme{
        .name                  = "Light",
        .userColor             = ftxui::Color::RGB(60, 80, 130),
        .assistantColor        = ftxui::Color::RGB(50, 50, 50),
        .thinkingColor         = ftxui::Color::Yellow4,
        .toolColor             = ftxui::Color::RGB(135, 136, 137), // #878889
        .systemColor           = ftxui::Color::RGB(135, 136, 137),
        .errorColor            = ftxui::Color::RGB(200, 30, 30),   // #c81e1e
        .accentColor           = ftxui::Color::RGB(102, 204, 255),
        .normalColor           = ftxui::Color::RGB(50, 50, 50),
        .hintColor             = ftxui::Color::RGB(135, 136, 137),
        .backgroundColor       = ftxui::Color::RGB(255, 255, 255),
        .blockColor            = ftxui::Color::RGB(246, 247, 252),
        .inputBgColor          = ftxui::Color::RGB(235, 238, 240),
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
            .blockquote  = ftxui::color(ftxui::Color::RGB(135, 136, 137)),  // 引用
            .table_header = ftxui::color(ftxui::Color::RGB(50, 50, 50)) | ftxui::bold, // 表格
            .table_border = ftxui::color(ftxui::Color::RGB(50, 50, 50)),
            .diagram_pending  = ftxui::Color::RGB(135, 136, 137), // hintColor
            .diagram_running  = ftxui::Color::Yellow4,            // thinkingColor
            .diagram_done     = ftxui::Color::RGB(102, 204, 255), // accentColor
            .diagram_failed   = ftxui::Color::RGB(200, 30, 30),   // errorColor
        },
    };
}
