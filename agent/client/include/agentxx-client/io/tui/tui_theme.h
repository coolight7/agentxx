#pragma once

#include "ftxui/screen/color.hpp"

/// TUI 主题配色
/// - 通过静态函数 darkTheme()/lightTheme() 生成内置主题
/// - 默认使用黑色主题 darkTheme()
class TUITheme {
public:
  ftxui::Color userColor;      // 用户消息
  ftxui::Color assistantColor; // 助手消息
  ftxui::Color thinkingColor;  // 思考消息
  ftxui::Color systemColor;    // 系统/错误消息
  ftxui::Color promptColor;    // 输入提示符 ">>>"
  ftxui::Color accentColor;    // 强调 (边框/标题/高亮)
  ftxui::Color statusColor;    // 状态栏文字
  ftxui::Color hintColor;      // 弱化提示文字

  /// 黑色主题 (默认)
  /// - 适用于深色终端背景
  static TUITheme darkTheme() {
    return TUITheme{
        .userColor = ftxui::Color::Cyan,
        .assistantColor = ftxui::Color::GrayLight,
        .thinkingColor = ftxui::Color::Yellow,
        .systemColor = ftxui::Color::RedLight,
        .promptColor = ftxui::Color::Green,
        .accentColor = ftxui::Color::CyanLight,
        .statusColor = ftxui::Color::GrayLight,
        .hintColor = ftxui::Color::GrayDark,
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
    };
  }
};
