#pragma once

/// 插件 UI items 共享渲染 helper (TUI 零特化的统一入口)
///
/// 背景: sidebar / panel / message decor 三处曾各写一份 button 样式,
/// 导致一处改动别处不一致。本文件是唯一的按钮样式实现, 三处强制复用,
/// 禁止各自独立实现。
///
/// 组件**描述**的解析与渲染在 [ui_components.h] (界面描述层 `pluginxx::ui`) ——
/// 本文件只保留渲染样式与绑定判定这类共享小件:
/// - PluginButtonDesc: 按钮样式参数 (label/动作/样式/配色)
/// - renderPluginButton: 按 role 配色渲染按钮 Element
/// - hasPluginBinding: 快照中是否存在该 plugin 的绑定 (精确匹配优先, 否则用 "")
/// - renderPluginDiff: diff 内容渲染 (message decor 与 DiffOverlay 共用;
///   复用 computeLineDiff + side-by-side/统一双样式, 避免双份实现)
/// - parsePluginDiffPayload: diff overlay 的 payload 解析 (单段/多段)
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx/plugin/client_plugin_manager.h"
#include "ftxui/dom/elements.hpp"
#include "utilxx/diff_util.h"
#include "utilxx_base/json.h"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

/// 按钮渲染参数 (由组件项 `Button` 的 action/variant/tone 字段换算而来)
struct PluginButtonDesc {
    std::string      label;    ///< 按钮文字 (已 strip; 渲染时自动左右补空格)
    std::string      actionId; ///< 可点动作 id (空 = 纯静态)
    std::string      argsJson; ///< 点击原样转发的参数 dump (无参 = "{}")
    PluginButtonRole role = PluginButtonRole::Normal;
    /// 是否可点: actionId 非空 && 快照有该 plugin 绑定 (精确匹配优先, 否则用 "")
    bool clickable = false;
};

/// 按 role 配色渲染按钮 (label 自动左右各补一空格)
ftxui::Element renderPluginButton(const PluginButtonDesc& desc, const TUITheme& theme);

/// UI 项文本颜色 (tone → 主题色; 插件 items 与中断 items 共用同一映射)
/// - title/accent → accentColor; hint → hintColor; error → errorColor;
///   thinking → thinkingColor; tool → toolColor; normal/未知 → normalColor
ftxui::Color uiRoleColor(std::string_view tone, const TUITheme& theme);

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

/// diff 的一段替换 (`oldStr` → `newStr`; 一次编辑可以有多段)
struct PluginDiffSegment {
    std::string oldStr;
    std::string newStr;
};

/// diff overlay 的 payload 解析结果 (见 [parsePluginDiffPayload])
struct PluginDiffPayload {
    std::string                    path;
    std::vector<PluginDiffSegment> segments;
};

/// 解析 diff overlay 的 payload
///
/// 段落形态与 edit 工具参数同一套"少写一层"规则 —— 工具参数的类型自动修正会把
/// 单个对象与单元素数组成对互转 (见 ToolcallWrapNode::autoFixArgsType), 因此界面层
/// 两种形态都必须认:
/// - 多段: `{"path":"a.cpp","edits":[{"old_str":"...","new_str":"..."}, ...]}`
/// - `edits` 为**单个对象** (少写一层数组): `{"path":"a.cpp","edits":{...}}`
/// - 单段 (旧写法, 保留支持): `{"path":"a.cpp","old_str":"...","new_str":"..."}`
/// - 裸数组 `[{...},{...}]`: 按"无文件路径的多段"处理
///
/// - 段内缺 `old_str`/`new_str` 时按空串处理 (空串 = 删除 / 纯新增那一侧)
/// - 段内两侧都为空的对象跳过 (没有内容的段不占一行)
/// - `return` payload 不是 JSON / 顶层既不是对象也不是数组时为 `std::nullopt`
///   (调用方按"打不开弹窗"处理); 解析成功但没有任何可用段时 `segments` 为空
std::optional<PluginDiffPayload> parsePluginDiffPayload(std::string_view payloadJson);

/// 快照中是否存在该 plugin 的绑定 (精确匹配 target 或 "" 任一)
bool hasPluginBinding(std::string_view plugin, const agentxx::plugin::ClientUiRegistry* reg);

/// 快照中是否存在该 plugin 在指定 owner 下的绑定 (精确 owner 优先, 否则用 "")
bool hasPluginBindingFor(
    std::string_view                         plugin,
    std::string_view                         ownerId,
    const agentxx::plugin::ClientUiRegistry* reg
);

} // namespace client
} // namespace agentxx
