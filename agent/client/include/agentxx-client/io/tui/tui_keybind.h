#pragma once

/// 终端按键事件 → 快捷键描述 (与宿主的 `normalizeKeybindSpec` 同一口径)
///
/// 背景: 插件经 `agentxx.client.keybind` 表注册全局快捷键, 注册时宿主把键位描述
/// 规范化 (小写 + 修饰键固定顺序, 如 `"ctrl+alt+k"`); 界面侧收到按键事件后必须
/// 转成同一格式的字符串才能做匹配 —— 本文件就是那次转换的**唯一实现**。
///
/// 可表达的键位受终端与 FTXUI 事件模型限制:
/// - `ctrl` / `alt` / `ctrl+alt` + 字母 (`ftxui::Event::CtrlA` 等);
///   终端不区分 `ctrl+shift+x` 与 `ctrl+x`, 因此 `shift` 不参与字母组合
/// - 独立功能键: `f1`~`f12` / 方向键 (含 ctrl 变体) / home / end / pageup /
///   pagedown / insert / delete / backspace / tab / enter / esc
/// - **无修饰键的可打印字符不作快捷键** (返回空串): 否则会与输入框抢字符
///   (宿主侧注册同样的键位也会失败, 两侧规则一致)
///
/// 相关: 能力声明见 [tui_plugin_adapter.h]; 键位格式见
/// [client_plugin_api.h](/agent/lib/include/agentxx/plugin/api/client_plugin_api.h)
/// 的 `AgentxxKeybindSpec`
#include "ftxui/component/event.hpp"
#include <string>
#include <string_view>

namespace agentxx {
namespace client {

/// 按键事件 → 快捷键描述 (`""` = 不作为全局快捷键匹配)
inline std::string keybindOfEvent(const ftxui::Event& event) {
    using ftxui::Event;
    if (event.is_mouse()) {
        return {};
    }

    // ---- 独立功能键 (自身即完整键位, 允许无修饰键) ----
    struct KeyName {
        const Event* event;
        const char*  name;
    };
    static const KeyName kKeys[] = {
        {&Event::ArrowUp, "up"},
        {&Event::ArrowDown, "down"},
        {&Event::ArrowLeft, "left"},
        {&Event::ArrowRight, "right"},
        {&Event::ArrowUpCtrl, "ctrl+up"},
        {&Event::ArrowDownCtrl, "ctrl+down"},
        {&Event::ArrowLeftCtrl, "ctrl+left"},
        {&Event::ArrowRightCtrl, "ctrl+right"},
        {&Event::Home, "home"},
        {&Event::End, "end"},
        {&Event::PageUp, "pageup"},
        {&Event::PageDown, "pagedown"},
        {&Event::Insert, "insert"},
        {&Event::Delete, "delete"},
        {&Event::Backspace, "backspace"},
        {&Event::Tab, "tab"},
        {&Event::Return, "enter"},
        {&Event::Escape, "esc"},
        {&Event::F1, "f1"},
        {&Event::F2, "f2"},
        {&Event::F3, "f3"},
        {&Event::F4, "f4"},
        {&Event::F5, "f5"},
        {&Event::F6, "f6"},
        {&Event::F7, "f7"},
        {&Event::F8, "f8"},
        {&Event::F9, "f9"},
        {&Event::F10, "f10"},
        {&Event::F11, "f11"},
        {&Event::F12, "f12"},
    };
    for (const auto& key : kKeys) {
        if (event == *key.event) {
            return std::string{key.name};
        }
    }

    // ---- ctrl / alt / ctrl+alt + 字母 ----
    // FTXUI 把三类组合分别编码为 "\x01".."\x1a" (ctrl+字母)、"\x1b{字母}"、
    // "\x1b\x01".."\x1b\x1a" (ctrl+alt+字母); 事件比较只看原始输入串, 因此
    // 逐个比对即可 (表驱动, 26 个字母)
    for (int i = 0; i < 26; ++i) {
        const char letter = static_cast<char>('a' + i);
        const char ctrl   = static_cast<char>(1 + i);
        std::string combo;
        combo += '\x1b';
        combo += ctrl;
        if (event == Event::Character(std::string(1, ctrl))) {
            return std::string{"ctrl+"} + letter;
        }
        if (event == Event::Character(combo)) {
            return std::string{"ctrl+alt+"} + letter;
        }
        std::string alt;
        alt += '\x1b';
        alt += letter;
        if (event == Event::Character(alt)) {
            return std::string{"alt+"} + letter;
        }
    }
    return {};
}

} // namespace client
} // namespace agentxx
