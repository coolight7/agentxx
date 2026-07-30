#pragma once

#include "ftxui/screen/color.hpp"
#include <markdown/theme.hpp>

/// TUI 主题配色
/// - 通过静态函数 darkTheme()/lightTheme() 生成内置主题
/// - 默认使用黑色主题 darkTheme()
class TUITheme {
public:

    ftxui::Color userColor;      // 用户消息
    ftxui::Color assistantColor; // 助手消息 (content)
    ftxui::Color thinkingColor;  // 思考消息
    ftxui::Color toolColor;      // toolcall 消息
    ftxui::Color systemColor;    // 系统消息
    ftxui::Color errorColor;     // 错误消息 (红色)
    ftxui::Color promptColor;    // 输入提示符 ">>>"
    ftxui::Color accentColor;    // 强调 (边框/标题/高亮)
    ftxui::Color statusColor;    // 状态栏文字
    ftxui::Color hintColor;      // 弱化提示文字

    ftxui::Color backgroundColor;       // 整体背景
    ftxui::Color blockColor;            // 分块背景 (侧边栏等)
    ftxui::Color inputBgColor;          // 输入框背景
    ftxui::Color inputTextColor;        // 输入框文字
    ftxui::Color buttonBgColor;         // 非高亮按钮背景
    ftxui::Color buttonTextColor;       // 非高亮按钮文字
    ftxui::Color buttonActiveBgColor;   // 高亮按钮背景
    ftxui::Color buttonActiveTextColor; // 高亮按钮文字

    markdown::Theme markdownTheme; // markdown-ui 渲染主题

    /// 黑色主题 (默认)
    /// - 适用于深色终端背景
    static TUITheme darkTheme();

    /// 白色主题
    /// - 适用于浅色终端背景
    static TUITheme lightTheme();
};
