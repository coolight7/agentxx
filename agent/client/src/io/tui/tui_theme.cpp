#include "agentxx-client/io/tui/tui_theme.h"

TUITheme TUITheme::darkTheme() {
    return TUITheme{
        .userColor             = ftxui::Color::Cyan,
        .assistantColor        = ftxui::Color::RGB(255, 255, 255), // #fff
        .thinkingColor         = ftxui::Color::RGB(255, 175, 95),  // #ffaf5f
        .toolColor             = ftxui::Color::RGB(117, 125, 138), // #757d8a
        .systemColor           = ftxui::Color::RGB(117, 125, 138),
        .promptColor           = ftxui::Color::Green,
        .accentColor           = ftxui::Color::RGB(102, 204, 255), // #66ccff
        .statusColor           = ftxui::Color::GrayLight,
        .hintColor             = ftxui::Color::GrayDark,
        .backgroundColor       = ftxui::Color::RGB(0, 0, 0),       // #000
        .blockColor            = ftxui::Color::RGB(18, 18, 18),    // #121212
        .inputBgColor          = ftxui::Color::RGB(24, 26, 30),    //
        .inputTextColor        = ftxui::Color::RGB(255, 255, 255), // #fff
        .buttonBgColor         = ftxui::Color::RGB(42, 49, 56),    // #2a3138
        .buttonTextColor       = ftxui::Color::RGB(255, 255, 255), // #fff
        .buttonActiveBgColor   = ftxui::Color::RGB(102, 204, 255), // #66ccff
        .buttonActiveTextColor = ftxui::Color::RGB(0, 0, 0),       // #000
    };
}

TUITheme TUITheme::lightTheme() {
    return TUITheme{
        .userColor             = ftxui::Color::Blue3,
        .assistantColor        = ftxui::Color::Black,
        .thinkingColor         = ftxui::Color::Yellow4,
        .toolColor             = ftxui::Color::RGB(135, 136, 137), // #878889
        .systemColor           = ftxui::Color::RGB(135, 136, 137),
        .promptColor           = ftxui::Color::DarkGreen,
        .accentColor           = ftxui::Color::Blue3,
        .statusColor           = ftxui::Color::Grey37,
        .hintColor             = ftxui::Color::Grey53,
        .backgroundColor       = ftxui::Color::RGB(255, 255, 255),
        .blockColor            = ftxui::Color::RGB(246, 247, 252),
        .inputBgColor          = ftxui::Color::RGB(235, 238, 240),
        .inputTextColor        = ftxui::Color::Black,
        .buttonBgColor         = ftxui::Color::RGB(220, 224, 228),
        .buttonTextColor       = ftxui::Color::Black,
        .buttonActiveBgColor   = ftxui::Color::Blue3,
        .buttonActiveTextColor = ftxui::Color::White,
    };
}
