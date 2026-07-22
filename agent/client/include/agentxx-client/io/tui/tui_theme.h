#pragma once

#include "ftxui/screen/color.hpp"

/// TUI 主题配色
/// - 通过静态函数 darkTheme()/lightTheme() 生成内置主题
/// - 默认使用黑色主题 darkTheme()
class TUITheme {
public:
  ftxui::Color userColor;      // 用户消息
  ftxui::Color assistantColor; // 助手消息 (content)
  ftxui::Color thinkingColor;  // 思考消息
  ftxui::Color systemColor;    // 系统/错误消息
  ftxui::Color promptColor;    // 输入提示符 ">>>"
  ftxui::Color accentColor;    // 强调 (边框/标题/高亮)
  ftxui::Color statusColor;    // 状态栏文字
  ftxui::Color hintColor;      // 弱化提示文字

  ftxui::Color backgroundColor;       // 整体背景
  ftxui::Color inputBgColor;          // 输入框背景
  ftxui::Color inputTextColor;        // 输入框文字
  ftxui::Color buttonBgColor;         // 非高亮按钮背景
  ftxui::Color buttonTextColor;       // 非高亮按钮文字
  ftxui::Color buttonActiveBgColor;   // 高亮按钮背景
  ftxui::Color buttonActiveTextColor; // 高亮按钮文字

  /// 黑色主题 (默认)
  /// - 适用于深色终端背景
  static TUITheme darkTheme() {
    return TUITheme{
        .userColor = ftxui::Color::Cyan,
        .assistantColor = ftxui::Color::RGB(255, 255, 255), // #fff
        .thinkingColor = ftxui::Color::RGB(255, 175, 95),   // #ffaf5f
        .systemColor = ftxui::Color::RedLight,
        .promptColor = ftxui::Color::Green,
        .accentColor = ftxui::Color::RGB(102, 204, 255), // #66ccff
        .statusColor = ftxui::Color::GrayLight,
        .hintColor = ftxui::Color::GrayDark,
        .backgroundColor = ftxui::Color::RGB(0, 0, 0),       // #000
        .inputBgColor = ftxui::Color::RGB(35, 40, 45),       // #23282d
        .inputTextColor = ftxui::Color::RGB(255, 255, 255),  // #fff
        .buttonBgColor = ftxui::Color::RGB(42, 49, 56),      // #2a3138
        .buttonTextColor = ftxui::Color::RGB(255, 255, 255), // #fff
        .buttonActiveBgColor = ftxui::Color::RGB(102, 204, 255), // #66ccff
        .buttonActiveTextColor = ftxui::Color::RGB(255, 255, 255), // #fff
    };
  }

  /// 白色主题
  /// - 适用于浅色终端背景
  static TUITheme lightTheme() {
    return TUITheme{
        .userColor = ftxui::Color::Blue3,
        .assistantColor = ftxui::Color::Black,
        .thinkingColor = ftxui::Color::Yellow4,
        .systemColor = ftxui::Color::DarkRed,
        .promptColor = ftxui::Color::DarkGreen,
        .accentColor = ftxui::Color::Blue3,
        .statusColor = ftxui::Color::Grey37,
        .hintColor = ftxui::Color::Grey53,
        .backgroundColor = ftxui::Color::RGB(255, 255, 255),
        .inputBgColor = ftxui::Color::RGB(235, 238, 240),
        .inputTextColor = ftxui::Color::Black,
        .buttonBgColor = ftxui::Color::RGB(220, 224, 228),
        .buttonTextColor = ftxui::Color::Black,
        .buttonActiveBgColor = ftxui::Color::Blue3,
        .buttonActiveTextColor = ftxui::Color::White,
    };
  }
};
