#pragma once

/// 插件 UI items 共享渲染 helper (TUI 零特化的收敛点)
///
/// 背景: sidebar / panel / message decor 三处曾各写一份 button 解析与样式,
/// 导致 planning 语义 (mermaid 即弹窗) 三处漂移。本文件是唯一的 button
/// 解析/渲染实现, 三处强制复用, 禁止各自独立实现。
///
/// 内容:
/// - PluginButtonDesc: button 项解析结果 (label/prefix/action_id/args/role)
/// - parsePluginButton: 从 items 数组元素解析 button (兼容旧 action kind)
/// - renderPluginButton: 按 role 配色渲染按钮 Element
/// - renderPluginTextItem: text 项按 role 配色渲染 (title/normal/hint)
/// - hasPluginBinding: 快照中是否存在该 plugin 的绑定 (精确或 "" 兜底)
/// - estimatePluginButtonLines: 高度估算 (button 恒 1 行)
/// - renderPluginDiff: diff 内容渲染 (message decor 与 DiffOverlay 共用;
///   复用 computeLineDiff + side-by-side/统一双样式, 避免双份实现)
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx/plugin/client_plugin_manager.h"
#include "agentxx/util/diff_util.h"
#include "ftxui/dom/elements.hpp"
#include "neograph/json.h"
#include <string>

namespace agentxx {
namespace client {

/// 按钮 role 配色:
/// - normal: 默认按钮配色 (buttonBg/buttonText)
/// - accent: 强调 (buttonActiveBg/buttonActiveText, 如 Graph)
/// - danger: 错误色背景 + 按钮文字 (如删除/高危操作)
enum class PluginButtonRole : uint8_t {
    Normal = 0,
    Accent = 1,
    Danger = 2,
};

/// button 项解析结果
struct PluginButtonDesc {
    std::string      label;    ///< 按钮文字 (已 strip; 渲染时自动左右补空格)
    std::string      prefix;   ///< 同行前导 (如 "|- "; 空 = 无前缀)
    std::string      actionId; ///< 可点动作 id (空 = 纯静态)
    std::string      argsJson; ///< 点击透传参数 dump (无参 = "{}")
    PluginButtonRole role = PluginButtonRole::Normal;
    /// 是否可点: actionId 非空 && 快照有该 plugin 绑定 (精确或 "" 兜底)
    bool clickable = false;
};

/// 从 items 元素解析 button (it 须为 object; 非 button 返回 false)
/// - 兼容旧 Panel `{"kind":"action","id":...,"label":...}`: 视为 action_id=id
/// - args 缺失/非 object → "{}"; role 非法值 → Normal
/// - clickable 由 hasPluginBinding(plugin, reg) 决定 (reg 可空 → false)
bool parsePluginButton(
    const neograph::json&                    it,
    std::string_view                         plugin,
    const agentxx::plugin::ClientUiRegistry* reg,
    PluginButtonDesc&                        out
);

/// 按 role 配色渲染按钮 (label 自动左右各补一空格)
ftxui::Element renderPluginButton(const PluginButtonDesc& desc, const TUITheme& theme);

/// text 项按 role 配色渲染 (title=高亮强调/normal=普通/hint=减淡)
ftxui::Element
    renderPluginTextItem(const std::string& text, const std::string& role, const TUITheme& theme);

/// diff 内容渲染 (message decor 与 DiffOverlay 共用)
/// - path 非空时首行展示 "  file: {path}"
/// - screenW<=0 时取当前终端宽度; side-by-side 门槛 100 列 (与历史实现一致)
ftxui::Element renderPluginDiff(
    std::string_view path,
    std::string_view oldStr,
    std::string_view newStr,
    const TUITheme&  theme,
    int              screenW = 0
);

/// 快照中是否存在该 plugin 的绑定 (精确 target 或 "" 兜底任一)
bool hasPluginBinding(std::string_view plugin, const agentxx::plugin::ClientUiRegistry* reg);

/// 快照中是否存在该 plugin 在指定 owner 下的绑定 (精确 owner 优先, 否则 "" 兜底)
bool hasPluginBindingFor(
    std::string_view                         plugin,
    std::string_view                         ownerId,
    const agentxx::plugin::ClientUiRegistry* reg
);

/// role 字符串 → 枚举 (非法值 → Normal)
PluginButtonRole parseButtonRole(std::string_view role);

} // namespace client
} // namespace agentxx
