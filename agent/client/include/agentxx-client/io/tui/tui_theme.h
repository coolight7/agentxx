#pragma once

#include "ftxui/screen/color.hpp"
#include <markdown/theme.hpp>
#include <string>

namespace agentxx::client {

/// TUI 主题配色
/// - 通过静态函数 darkTheme()/lightTheme() 生成内置主题
/// - 默认使用黑色主题 darkTheme()
class TUITheme {
public:

    std::string name; // 主题名 ("Dark"/"Light"), 供设置弹窗识别当前主题

    ftxui::Color userColor;      // 用户消息
    ftxui::Color assistantColor; // 助手消息 (content)
    ftxui::Color thinkingColor;  // 思考消息
    ftxui::Color toolColor;      // toolcall 消息
    ftxui::Color systemColor;    // 系统消息
    ftxui::Color errorColor;     // 错误消息 (红色)
    ftxui::Color accentColor;    // 强调 (边框/标题/高亮)
    ftxui::Color normalColor;    // 普通内容颜色 (文本)
    ftxui::Color hintColor;      // 弱化提示文字

    ftxui::Color backgroundColor;       // 整体背景
    ftxui::Color blockColor;            // 分块背景 (侧边栏等)
    ftxui::Color inputBgColor;          // 输入框背景
    ftxui::Color inputTextColor;        // 输入框文字
    ftxui::Color buttonBgColor;         // 非高亮按钮背景
    ftxui::Color buttonTextColor;       // 非高亮按钮文字
    ftxui::Color buttonActiveBgColor;   // 高亮按钮背景
    ftxui::Color buttonActiveTextColor; // 高亮按钮文字

    /// 弹窗配色: 面性风格
    /// - 浮层不使用边框和分割线, 标题栏/内容区/底部提示栏各自以背景色区分;
    ///   各区域背景行由 vbox 在交叉轴上拉伸铺满整行宽度 (见 overlays.cpp 外框辅助函数)
    /// - 命名对应 "[surface] 表面" 语义: 内容区是最外层表面, 标题栏/底栏是附着的次级表面
    ftxui::Color surfaceColor;            // 浮层内容区背景
    ftxui::Color surfaceHeaderColor;      // 浮层标题栏背景
    ftxui::Color surfaceFooterColor;      // 浮层底部提示栏背景
    ftxui::Color surfaceTitleColor;       // 浮层标题文字
    ftxui::Color surfaceErrorHeaderColor; // 错误类浮层标题栏背景 (如加载失败列表)
    ftxui::Color surfaceScrimColor; // 浮层打开时铺满屏幕的下层背景色 (衬托浮层表面)

    markdown::Theme markdownTheme; // markdown-ui 渲染主题

    /// 黑色主题 (默认)
    /// - 适用于深色终端背景
    static TUITheme darkTheme();

    /// 白色主题
    /// - 适用于浅色终端背景
    static TUITheme lightTheme();
};

} // namespace agentxx::client
