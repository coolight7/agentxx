#include "agentxx-client/io/tui/tui_theme.h"

TUITheme TUITheme::darkTheme() {
    return TUITheme{
        .userColor             = ftxui::Color::RGB(102, 204, 255),
        .assistantColor        = ftxui::Color::RGB(255, 255, 255), // #fff
        .thinkingColor         = ftxui::Color::RGB(255, 175, 95),  // #ffaf5f
        .toolColor             = ftxui::Color::RGB(117, 125, 138), // #757d8a
        .systemColor           = ftxui::Color::RGB(117, 125, 138),
        .errorColor            = ftxui::Color::RGB(255, 85, 85),   // #ff5555
        .promptColor           = ftxui::Color::Green,
        .accentColor           = ftxui::Color::RGB(102, 204, 255), // #66ccff
        .statusColor           = ftxui::Color::GrayLight,
        .hintColor             = ftxui::Color::RGB(117, 125, 138),
        .backgroundColor       = ftxui::Color::RGB(0, 0, 0),       // #000
        .blockColor            = ftxui::Color::RGB(18, 18, 18),    // #121212
        .inputBgColor          = ftxui::Color::RGB(24, 26, 30),    //
        .inputTextColor        = ftxui::Color::RGB(255, 255, 255), // #fff
        .buttonBgColor         = ftxui::Color::RGB(42, 49, 56),    // #2a3138
        .buttonTextColor       = ftxui::Color::RGB(255, 255, 255), // #fff
        .buttonActiveBgColor   = ftxui::Color::RGB(102, 204, 255), // #66ccff
        .buttonActiveTextColor = ftxui::Color::RGB(0, 0, 0),       // #000
        .markdownTheme         = markdown::Theme{
            .name        = "Dark",
            .syntax      = ftxui::color(ftxui::Color::Yellow) | ftxui::dim,
            .gutter      = ftxui::color(ftxui::Color::RGB(117, 125, 138)),
            .heading1    = ftxui::Decorator(ftxui::bold) | ftxui::underlined
                        | ftxui::color(ftxui::Color::RGB(102, 204, 255)),
            .heading2    = ftxui::color(ftxui::Color::RGB(102, 204, 255)) | ftxui::bold,
            .heading3    = ftxui::Decorator(ftxui::bold) | ftxui::dim,
            .link        = ftxui::color(ftxui::Color::Cyan),
            .code_inline = ftxui::color(ftxui::Color::RGB(255, 175, 95)),
            .code_block  = ftxui::bgcolor(ftxui::Color::RGB(18, 18, 18)) // blockColor #121212
                        | ftxui::color(ftxui::Color::RGB(180, 180, 180)),
            .blockquote  = ftxui::color(ftxui::Color::RGB(117, 125, 138)),
            .table_header = ftxui::color(ftxui::Color::RGB(255, 255, 255)) | ftxui::bold,
            .table_border = ftxui::color(ftxui::Color::RGB(255, 255, 255)),
        },
    };
}

TUITheme TUITheme::lightTheme() {
    return TUITheme{
        .userColor             = ftxui::Color::RGB(60, 80, 130),
        .assistantColor        = ftxui::Color::RGB(50, 50, 50),
        .thinkingColor         = ftxui::Color::Yellow4,
        .toolColor             = ftxui::Color::RGB(135, 136, 137), // #878889
        .systemColor           = ftxui::Color::RGB(135, 136, 137),
        .errorColor            = ftxui::Color::RGB(200, 30, 30),   // #c81e1e
        .promptColor           = ftxui::Color::DarkGreen,
        .accentColor           = ftxui::Color::RGB(102, 204, 255),
        .statusColor           = ftxui::Color::Grey37,
        .hintColor             = ftxui::Color::RGB(135, 136, 137),
        .backgroundColor       = ftxui::Color::RGB(255, 255, 255),
        .blockColor            = ftxui::Color::RGB(246, 247, 252),
        .inputBgColor          = ftxui::Color::RGB(235, 238, 240),
        .inputTextColor        = ftxui::Color::RGB(50, 50, 50),
        .buttonBgColor         = ftxui::Color::RGB(220, 224, 228),
        .buttonTextColor       = ftxui::Color::RGB(50, 50, 50),
        .buttonActiveBgColor   = ftxui::Color::RGB(60, 80, 130),
        .buttonActiveTextColor = ftxui::Color::White,
        .markdownTheme         = markdown::Theme{
            .name        = "Light",
            .syntax      = ftxui::color(ftxui::Color::DarkGreen),
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
        },
    };
}
