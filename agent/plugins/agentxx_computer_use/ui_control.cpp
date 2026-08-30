#include "computer_use_plugin.h"
#include "fmt/format.h"
#include <neograph/json.h>
#include <cctype>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

#if XX_IS_WIN_D
#include <windows.h>
#undef max
#undef min
#endif

// computer_use_plugin.h 的 JSON 辅助 (jsonGetString/jsonGetInt 等) 定义于
// agentxx_computer_use_plugin 命名空间, 此处直接使用
using namespace agentxx_computer_use_plugin;

namespace agentxx_computer_use_plugin {

#if XX_IS_WIN_D

static WORD uiControlCharToVk(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<WORD>(ch);
    }
    if (ch >= 'a' && ch <= 'z') {
        return static_cast<WORD>(toupper(ch));
    }
    if (ch >= '0' && ch <= '9') {
        return static_cast<WORD>(ch);
    }
    switch (ch) {
        case ' ':
            return VK_SPACE;
        case '\n':
            return VK_RETURN;
        case '\t':
            return VK_TAB;
        case '\b':
            return VK_BACK;
        case '\x1b':
            return VK_ESCAPE;
        case '!':
            return '1';
        case '@':
            return '2';
        case '#':
            return '3';
        case '$':
            return '4';
        case '%':
            return '5';
        case '^':
            return '6';
        case '&':
            return '7';
        case '*':
            return '8';
        case '(':
            return '9';
        case ')':
            return '0';
        case '-':
            return VK_OEM_MINUS;
        case '=':
            return VK_OEM_PLUS;
        case '[':
            return VK_OEM_4;
        case ']':
            return VK_OEM_6;
        case '\\':
            return VK_OEM_5;
        case ';':
            return VK_OEM_1;
        case '\'':
            return VK_OEM_7;
        case ',':
            return VK_OEM_COMMA;
        case '.':
            return VK_OEM_PERIOD;
        case '/':
            return VK_OEM_2;
        case '`':
            return VK_OEM_3;
        case '_':
            return VK_OEM_MINUS;
        case '+':
            return VK_OEM_PLUS;
        case '{':
            return VK_OEM_4;
        case '}':
            return VK_OEM_6;
        case '|':
            return VK_OEM_5;
        case ':':
            return VK_OEM_1;
        case '"':
            return VK_OEM_7;
        case '<':
            return VK_OEM_COMMA;
        case '>':
            return VK_OEM_PERIOD;
        case '?':
            return VK_OEM_2;
        case '~':
            return VK_OEM_3;
        default:
            return VkKeyScanA(ch) & 0xFF;
    }
}

static bool uiControlNeedsShift(char ch) {
    switch (ch) {
        case '!':
        case '@':
        case '#':
        case '$':
        case '%':
        case '^':
        case '&':
        case '*':
        case '(':
        case ')':
        case '_':
        case '+':
        case '{':
        case '}':
        case '|':
        case ':':
        case '"':
        case '<':
        case '>':
        case '?':
        case '~':
            return true;
        default:
            return false;
    }
}

/// 大写转换 (替代 libagentxx::util::toUpper; 插件不链接 libagentxx)
static std::string uiControlToUpper(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

static WORD uiControlKeyNameToVk(std::string_view key) {
    if (key.size() == 1) {
        return uiControlCharToVk(key[0]);
    }

    auto upper = uiControlToUpper(key);
    for (auto& c : upper) {
        c = static_cast<char>(toupper(c));
    }

    if (upper == "ENTER" || upper == "RETURN") {
        return VK_RETURN;
    }
    if (upper == "TAB") {
        return VK_TAB;
    }
    if (upper == "ESCAPE" || upper == "ESC") {
        return VK_ESCAPE;
    }
    if (upper == "BACKSPACE" || upper == "BACK") {
        return VK_BACK;
    }
    if (upper == "DELETE" || upper == "DEL") {
        return VK_DELETE;
    }
    if (upper == "INSERT" || upper == "INS") {
        return VK_INSERT;
    }
    if (upper == "HOME") {
        return VK_HOME;
    }
    if (upper == "END") {
        return VK_END;
    }
    if (upper == "PAGEUP" || upper == "PAGE_UP") {
        return VK_PRIOR;
    }
    if (upper == "PAGEDOWN" || upper == "PAGE_DOWN") {
        return VK_NEXT;
    }
    if (upper == "UP" || upper == "ARROWUP" || upper == "ARROW_UP") {
        return VK_UP;
    }
    if (upper == "DOWN" || upper == "ARROWDOWN" || upper == "ARROW_DOWN") {
        return VK_DOWN;
    }
    if (upper == "LEFT" || upper == "ARROWLEFT" || upper == "ARROW_LEFT") {
        return VK_LEFT;
    }
    if (upper == "RIGHT" || upper == "ARROWRIGHT" || upper == "ARROW_RIGHT") {
        return VK_RIGHT;
    }
    if (upper == "SPACE") {
        return VK_SPACE;
    }
    if (upper == "F1") {
        return VK_F1;
    }
    if (upper == "F2") {
        return VK_F2;
    }
    if (upper == "F3") {
        return VK_F3;
    }
    if (upper == "F4") {
        return VK_F4;
    }
    if (upper == "F5") {
        return VK_F5;
    }
    if (upper == "F6") {
        return VK_F6;
    }
    if (upper == "F7") {
        return VK_F7;
    }
    if (upper == "F8") {
        return VK_F8;
    }
    if (upper == "F9") {
        return VK_F9;
    }
    if (upper == "F10") {
        return VK_F10;
    }
    if (upper == "F11") {
        return VK_F11;
    }
    if (upper == "F12") {
        return VK_F12;
    }
    if (upper == "SHIFT" || upper == "LSHIFT") {
        return VK_LSHIFT;
    }
    if (upper == "RSHIFT") {
        return VK_RSHIFT;
    }
    if (upper == "CTRL" || upper == "CONTROL" || upper == "LCTRL") {
        return VK_LCONTROL;
    }
    if (upper == "RCTRL") {
        return VK_RCONTROL;
    }
    if (upper == "ALT" || upper == "LALT") {
        return VK_LMENU;
    }
    if (upper == "RALT") {
        return VK_RMENU;
    }
    if (upper == "WIN" || upper == "LWIN" || upper == "META") {
        return VK_LWIN;
    }
    if (upper == "RWIN") {
        return VK_RWIN;
    }
    if (upper == "APPS" || upper == "MENU") {
        return VK_APPS;
    }
    if (upper == "CAPSLOCK" || upper == "CAPS") {
        return VK_CAPITAL;
    }
    if (upper == "NUMLOCK") {
        return VK_NUMLOCK;
    }
    if (upper == "SCROLLLOCK") {
        return VK_SCROLL;
    }
    if (upper == "PRINTSCREEN" || upper == "PRTSC") {
        return VK_SNAPSHOT;
    }
    if (upper == "PAUSE" || upper == "BREAK") {
        return VK_PAUSE;
    }
    if (upper == "NUMPAD0") {
        return VK_NUMPAD0;
    }
    if (upper == "NUMPAD1") {
        return VK_NUMPAD1;
    }
    if (upper == "NUMPAD2") {
        return VK_NUMPAD2;
    }
    if (upper == "NUMPAD3") {
        return VK_NUMPAD3;
    }
    if (upper == "NUMPAD4") {
        return VK_NUMPAD4;
    }
    if (upper == "NUMPAD5") {
        return VK_NUMPAD5;
    }
    if (upper == "NUMPAD6") {
        return VK_NUMPAD6;
    }
    if (upper == "NUMPAD7") {
        return VK_NUMPAD7;
    }
    if (upper == "NUMPAD8") {
        return VK_NUMPAD8;
    }
    if (upper == "NUMPAD9") {
        return VK_NUMPAD9;
    }
    if (upper == "VOLUME_UP" || upper == "VOLUMEUP") {
        return VK_VOLUME_UP;
    }
    if (upper == "VOLUME_DOWN" || upper == "VOLUMEDOWN") {
        return VK_VOLUME_DOWN;
    }
    if (upper == "VOLUME_MUTE" || upper == "VOLUMEMUTE") {
        return VK_VOLUME_MUTE;
    }

    return 0;
}

static std::string uiControlVkToKeyName(WORD vk) {
    switch (vk) {
        case VK_RETURN:
            return "Enter";
        case VK_TAB:
            return "Tab";
        case VK_ESCAPE:
            return "Escape";
        case VK_BACK:
            return "Backspace";
        case VK_DELETE:
            return "Delete";
        case VK_INSERT:
            return "Insert";
        case VK_HOME:
            return "Home";
        case VK_END:
            return "End";
        case VK_PRIOR:
            return "PageUp";
        case VK_NEXT:
            return "PageDown";
        case VK_UP:
            return "Up";
        case VK_DOWN:
            return "Down";
        case VK_LEFT:
            return "Left";
        case VK_RIGHT:
            return "Right";
        case VK_SPACE:
            return "Space";
        case VK_LSHIFT:
        case VK_RSHIFT:
            return "Shift";
        case VK_LCONTROL:
        case VK_RCONTROL:
            return "Ctrl";
        case VK_LMENU:
        case VK_RMENU:
            return "Alt";
        case VK_LWIN:
        case VK_RWIN:
            return "Win";
        case VK_CAPITAL:
            return "CapsLock";
        default:
            if (vk >= VK_F1 && vk <= VK_F12) {
                return fmt::format("F{}", vk - VK_F1 + 1);
            }
            if (vk >= 'A' && vk <= 'Z') {
                return std::string(1, static_cast<char>(vk));
            }
            if (vk >= '0' && vk <= '9') {
                return std::string(1, static_cast<char>(vk));
            }
            return fmt::format("Vk({})", vk);
    }
}

struct UICmdResult {
    bool        ok = true;
    std::string msg;
};

static void uiControlDelay(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static UINT uiControlSendInput(UINT cInputs, LPINPUT pInputs, int cbSize) {
    UINT sent = SendInput(cInputs, pInputs, cbSize);
    if (sent < cInputs) {
        // TODO: 支持选择指定窗口
        HWND fgWnd = GetForegroundWindow();
        if (fgWnd) {
            DWORD fgThreadId  = GetWindowThreadProcessId(fgWnd, nullptr);
            DWORD curThreadId = GetCurrentThreadId();
            AttachThreadInput(curThreadId, fgThreadId, TRUE);
            sent = SendInput(cInputs - sent, pInputs + sent, cbSize);
            AttachThreadInput(curThreadId, fgThreadId, FALSE);
        }
        if (sent < cInputs) {
            sent += SendInput(cInputs - sent, pInputs + sent, cbSize);
        }
    }
    return sent;
}

static UINT uiControlSendInputAsync(UINT cInputs, LPINPUT pInputs, int cbSize) {
    UINT sent = uiControlSendInput(cInputs, pInputs, cbSize);
    return sent;
}

static bool uiControlIsExtendedKey(WORD vk) {
    switch (vk) {
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_LWIN:
        case VK_RWIN:
        case VK_APPS:
        case VK_RCONTROL:
        case VK_RMENU:
        case VK_DIVIDE:
        case VK_NUMLOCK:
        case VK_SNAPSHOT:
            return true;
        default:
            return false;
    }
}

static void uiControlPrepareKeyInput(INPUT& input, WORD vk, DWORD flags) {
    input.type           = INPUT_KEYBOARD;
    input.ki.wVk         = vk;
    input.ki.wScan       = static_cast<WORD>(MapVirtualKey(vk, MAPVK_VK_TO_VSC));
    input.ki.dwFlags     = flags | KEYEVENTF_SCANCODE;
    input.ki.time        = 0;
    input.ki.dwExtraInfo = 0;
    if (uiControlIsExtendedKey(vk)) {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
}

static void uiControlMouseMoveTo(int x, int y) {
    // 多显示器: 使用虚拟屏幕坐标系 (覆盖所有显示器), 支持副屏的负坐标。
    // 绝对坐标归一化按虚拟屏尺寸计算, 越界坐标 clamp 到虚拟屏范围避免溢出
    const int vx         = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy         = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw         = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh         = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    x                    = std::clamp(x, vx, vx + vw - 1);
    y                    = std::clamp(y, vy, vy + vh - 1);
    INPUT input          = {};
    input.type           = INPUT_MOUSE;
    input.mi.dx          = static_cast<LONG>((static_cast<LONGLONG>(x - vx) * 65535) / (vw - 1));
    input.mi.dy          = static_cast<LONG>((static_cast<LONGLONG>(y - vy) * 65535) / (vh - 1));
    input.mi.mouseData   = 0;
    input.mi.dwFlags     = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    input.mi.time        = 0;
    input.mi.dwExtraInfo = 0;
    uiControlSendInputAsync(1, &input, sizeof(INPUT));
}

static std::pair<int, int> uiControlGetCursorPosPair() {
    POINT pt;
    GetCursorPos(&pt);
    return {pt.x, pt.y};
}

static UICmdResult uiControlMouseMove(int x, int y) {
    uiControlMouseMoveTo(x, y);
    return UICmdResult{true, fmt::format("mouse_move -> ({}, {})", x, y)};
}

static UICmdResult
    uiControlMouseClick(std::string_view button, int x, int y, bool at, int click_count) {
    if (at) {
        uiControlMouseMoveTo(x, y);
        uiControlDelay(30);
    }

    DWORD       downFlag = 0, upFlag = 0;
    DWORD       dataVal = 0;
    std::string btnName;

    if (button == "right") {
        downFlag = MOUSEEVENTF_RIGHTDOWN;
        upFlag   = MOUSEEVENTF_RIGHTUP;
        btnName  = "right";
    } else if (button == "middle") {
        downFlag = MOUSEEVENTF_MIDDLEDOWN;
        upFlag   = MOUSEEVENTF_MIDDLEUP;
        btnName  = "middle";
    } else {
        downFlag = MOUSEEVENTF_LEFTDOWN;
        upFlag   = MOUSEEVENTF_LEFTUP;
        btnName  = "left";
    }

    auto [ptX, ptY] = uiControlGetCursorPosPair();

    for (int i = 0; i < click_count; i++) {
        INPUT inputDown          = {};
        inputDown.type           = INPUT_MOUSE;
        inputDown.mi.dx          = 0;
        inputDown.mi.dy          = 0;
        inputDown.mi.mouseData   = dataVal;
        inputDown.mi.dwFlags     = downFlag;
        inputDown.mi.time        = 0;
        inputDown.mi.dwExtraInfo = 0;
        uiControlSendInputAsync(1, &inputDown, sizeof(INPUT));

        uiControlDelay(20);

        INPUT inputUp          = {};
        inputUp.type           = INPUT_MOUSE;
        inputUp.mi.dx          = 0;
        inputUp.mi.dy          = 0;
        inputUp.mi.mouseData   = dataVal;
        inputUp.mi.dwFlags     = upFlag;
        inputUp.mi.time        = 0;
        inputUp.mi.dwExtraInfo = 0;
        uiControlSendInputAsync(1, &inputUp, sizeof(INPUT));

        if (i < click_count - 1) {
            uiControlDelay(80);
        }
    }

    std::string action = click_count > 1 ? "double_click" : "click";
    if (at) {
        return UICmdResult{true, fmt::format("mouse_{} @ ({}, {}) [{}]", action, x, y, btnName)};
    }
    return UICmdResult{true, fmt::format("mouse_{} @ ({}, {}) [{}]", action, ptX, ptY, btnName)};
}

static UICmdResult uiControlMouseScroll(int delta, int x, int y, bool at) {
    if (at) {
        uiControlMouseMoveTo(x, y);
        uiControlDelay(30);
    }

    INPUT input          = {};
    input.type           = INPUT_MOUSE;
    input.mi.dx          = 0;
    input.mi.dy          = 0;
    input.mi.mouseData   = static_cast<DWORD>(delta);
    input.mi.dwFlags     = MOUSEEVENTF_WHEEL;
    input.mi.time        = 0;
    input.mi.dwExtraInfo = 0;

    uiControlSendInputAsync(1, &input, sizeof(INPUT));

    auto [ptX, ptY] = uiControlGetCursorPosPair();
    return UICmdResult{true, fmt::format("mouse_scroll delta={} @ ({}, {})", delta, ptX, ptY)};
}

static UICmdResult
    uiControlMouseDrag(int x1, int y1, int x2, int y2, std::string_view button, int duration_ms) {
    uiControlMouseMoveTo(x1, y1);
    uiControlDelay(30);

    DWORD downFlag = 0, upFlag = 0;
    if (button == "right") {
        downFlag = MOUSEEVENTF_RIGHTDOWN;
        upFlag   = MOUSEEVENTF_RIGHTUP;
    } else if (button == "middle") {
        downFlag = MOUSEEVENTF_MIDDLEDOWN;
        upFlag   = MOUSEEVENTF_MIDDLEUP;
    } else {
        downFlag = MOUSEEVENTF_LEFTDOWN;
        upFlag   = MOUSEEVENTF_LEFTUP;
    }

    INPUT inputDown          = {};
    inputDown.type           = INPUT_MOUSE;
    inputDown.mi.dx          = 0;
    inputDown.mi.dy          = 0;
    inputDown.mi.dwFlags     = downFlag;
    inputDown.mi.time        = 0;
    inputDown.mi.dwExtraInfo = 0;
    uiControlSendInputAsync(1, &inputDown, sizeof(INPUT));

    uiControlDelay(50);

    int steps = std::max(duration_ms / 10, 1);
    for (int i = 1; i <= steps; i++) {
        int cx = x1 + (x2 - x1) * i / steps;
        int cy = y1 + (y2 - y1) * i / steps;
        uiControlMouseMoveTo(cx, cy);
        uiControlDelay(duration_ms / steps);
    }

    uiControlMouseMoveTo(x2, y2);
    uiControlDelay(30);

    INPUT inputUp          = {};
    inputUp.type           = INPUT_MOUSE;
    inputUp.mi.dx          = 0;
    inputUp.mi.dy          = 0;
    inputUp.mi.dwFlags     = upFlag;
    inputUp.mi.time        = 0;
    inputUp.mi.dwExtraInfo = 0;
    uiControlSendInputAsync(1, &inputUp, sizeof(INPUT));

    return UICmdResult{
        true,
        fmt::format("mouse_drag ({}, {}) -> ({}, {}) [{}]", x1, y1, x2, y2, button)
    };
}

static UICmdResult uiControlKeyDown(WORD vk, bool withShift = false) {
    if (withShift) {
        INPUT shift = {};
        uiControlPrepareKeyInput(shift, VK_LSHIFT, 0);
        uiControlSendInputAsync(1, &shift, sizeof(INPUT));
    }
    INPUT input = {};
    uiControlPrepareKeyInput(input, vk, 0);
    uiControlSendInputAsync(1, &input, sizeof(INPUT));
    return UICmdResult{true, fmt::format("key_down [{}]", uiControlVkToKeyName(vk))};
}

static UICmdResult uiControlKeyUp(WORD vk, bool withShift = false) {
    INPUT input = {};
    uiControlPrepareKeyInput(input, vk, KEYEVENTF_KEYUP);
    uiControlSendInputAsync(1, &input, sizeof(INPUT));
    if (withShift) {
        INPUT shift = {};
        uiControlPrepareKeyInput(shift, VK_LSHIFT, KEYEVENTF_KEYUP);
        uiControlSendInputAsync(1, &shift, sizeof(INPUT));
    }
    return UICmdResult{true, fmt::format("key_up [{}]", uiControlVkToKeyName(vk))};
}

static UICmdResult uiControlKeyPress(WORD vk, bool withShift = false) {
    if (withShift) {
        INPUT shift = {};
        uiControlPrepareKeyInput(shift, VK_LSHIFT, 0);
        uiControlSendInputAsync(1, &shift, sizeof(INPUT));
    }

    INPUT keyDown = {};
    uiControlPrepareKeyInput(keyDown, vk, 0);
    uiControlSendInputAsync(1, &keyDown, sizeof(INPUT));

    uiControlDelay(20);

    INPUT keyUp = {};
    uiControlPrepareKeyInput(keyUp, vk, KEYEVENTF_KEYUP);
    uiControlSendInputAsync(1, &keyUp, sizeof(INPUT));

    if (withShift) {
        INPUT shift = {};
        uiControlPrepareKeyInput(shift, VK_LSHIFT, KEYEVENTF_KEYUP);
        uiControlSendInputAsync(1, &shift, sizeof(INPUT));
    }

    return UICmdResult{true, fmt::format("key_press [{}]", uiControlVkToKeyName(vk))};
}

static UICmdResult uiControlKeyCombo(const std::vector<WORD>& vks) {
    if (vks.size() == 0) {
        return UICmdResult{false, "key_combo: empty keys"};
    }

    std::string comboStr;
    for (auto vk : vks) {
        if (!comboStr.size() == 0) {
            comboStr += "+";
        }
        comboStr += uiControlVkToKeyName(vk);
    }

    for (auto vk : vks) {
        INPUT down = {};
        uiControlPrepareKeyInput(down, vk, 0);
        uiControlSendInputAsync(1, &down, sizeof(INPUT));
        uiControlDelay(10);
    }

    uiControlDelay(20);

    for (auto it = vks.rbegin(); it != vks.rend(); ++it) {
        INPUT up = {};
        uiControlPrepareKeyInput(up, *it, KEYEVENTF_KEYUP);
        uiControlSendInputAsync(1, &up, sizeof(INPUT));
        uiControlDelay(5);
    }

    return UICmdResult{true, fmt::format("key_combo [{}]", comboStr)};
}

static UICmdResult uiControlKeyType(std::string_view text) {
    if (text.size() == 0) {
        return UICmdResult{true, "key_type [0 chars]"};
    }

    // UTF-8 -> UTF-16 (Windows 宽字符编码): 逐字节发送 UTF-8 会让中文等多字节
    // 字符变成多个乱码字符, 必须先解码为 UTF-16 码元再经 KEYEVENTF_UNICODE 发送
    std::wstring wtext;
    const int    wlen
        = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (wlen > 0) {
        wtext.resize(static_cast<size_t>(wlen));
        MultiByteToWideChar(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            wtext.data(),
            wlen
        );
    }
    // 转换失败 (非 UTF-8 输入) 时降级为逐字节原样发送, 保证不丢内容
    if (wtext.size() == 0) {
        wtext.assign(text.begin(), text.end());
    }

    std::vector<INPUT> inputs;
    inputs.reserve(wtext.size() * 2);

    for (wchar_t wch : wtext) {
        if (wch == L'\n' || wch == L'\r') {
            if (!inputs.size() == 0) {
                uiControlSendInputAsync(
                    static_cast<UINT>(inputs.size()),
                    inputs.data(),
                    sizeof(INPUT)
                );
                inputs.clear();
                uiControlDelay(5);
            }
            INPUT down = {};
            uiControlPrepareKeyInput(down, VK_RETURN, 0);
            uiControlSendInputAsync(1, &down, sizeof(INPUT));
            uiControlDelay(5);
            INPUT up = {};
            uiControlPrepareKeyInput(up, VK_RETURN, KEYEVENTF_KEYUP);
            uiControlSendInputAsync(1, &up, sizeof(INPUT));
            uiControlDelay(5);
            continue;
        }

        if (wch == L'\t') {
            if (!inputs.size() == 0) {
                uiControlSendInputAsync(
                    static_cast<UINT>(inputs.size()),
                    inputs.data(),
                    sizeof(INPUT)
                );
                inputs.clear();
                uiControlDelay(5);
            }
            INPUT down = {};
            uiControlPrepareKeyInput(down, VK_TAB, 0);
            uiControlSendInputAsync(1, &down, sizeof(INPUT));
            uiControlDelay(5);
            INPUT up = {};
            uiControlPrepareKeyInput(up, VK_TAB, KEYEVENTF_KEYUP);
            uiControlSendInputAsync(1, &up, sizeof(INPUT));
            uiControlDelay(5);
            continue;
        }

        if (wch == L'\b') {
            if (!inputs.size() == 0) {
                uiControlSendInputAsync(
                    static_cast<UINT>(inputs.size()),
                    inputs.data(),
                    sizeof(INPUT)
                );
                inputs.clear();
                uiControlDelay(5);
            }
            INPUT down = {};
            uiControlPrepareKeyInput(down, VK_BACK, 0);
            uiControlSendInputAsync(1, &down, sizeof(INPUT));
            uiControlDelay(5);
            INPUT up = {};
            uiControlPrepareKeyInput(up, VK_BACK, KEYEVENTF_KEYUP);
            uiControlSendInputAsync(1, &up, sizeof(INPUT));
            uiControlDelay(5);
            continue;
        }

        if (wch == L'\x1b') {
            if (!inputs.size() == 0) {
                uiControlSendInputAsync(
                    static_cast<UINT>(inputs.size()),
                    inputs.data(),
                    sizeof(INPUT)
                );
                inputs.clear();
                uiControlDelay(5);
            }
            INPUT down = {};
            uiControlPrepareKeyInput(down, VK_ESCAPE, 0);
            uiControlSendInputAsync(1, &down, sizeof(INPUT));
            uiControlDelay(5);
            INPUT up = {};
            uiControlPrepareKeyInput(up, VK_ESCAPE, KEYEVENTF_KEYUP);
            uiControlSendInputAsync(1, &up, sizeof(INPUT));
            uiControlDelay(5);
            continue;
        }

        if (wch < 32) {
            continue;
        }

        // UTF-16 码元 (代理对如 emoji 会分两个码元发送, KEYEVENTF_UNICODE 支持)
        const unsigned short codeUnit = static_cast<unsigned short>(wch);

        INPUT keyDown          = {};
        keyDown.type           = INPUT_KEYBOARD;
        keyDown.ki.wVk         = 0;
        keyDown.ki.wScan       = codeUnit;
        keyDown.ki.dwFlags     = KEYEVENTF_UNICODE;
        keyDown.ki.time        = 0;
        keyDown.ki.dwExtraInfo = 0;
        inputs.push_back(keyDown);

        INPUT keyUp          = {};
        keyUp.type           = INPUT_KEYBOARD;
        keyUp.ki.wVk         = 0;
        keyUp.ki.wScan       = codeUnit;
        keyUp.ki.dwFlags     = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        keyUp.ki.time        = 0;
        keyUp.ki.dwExtraInfo = 0;
        inputs.push_back(keyUp);
    }

    if (!inputs.size() == 0) {
        uiControlSendInputAsync(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    }

    return UICmdResult{true, fmt::format("key_type [{} chars]", text.size())};
}

static UICmdResult uiControlGetCursorPos() {
    auto [x, y] = uiControlGetCursorPosPair();
    return {true, fmt::format("cursor_pos: ({}, {})", x, y)};
}

static UICmdResult uiControlGetScreenSize() {
    // 返回虚拟屏幕尺寸 (覆盖所有显示器), 与 mouse_move/get_cursor_pos 的
    // 虚拟屏坐标系一致; 多显示器时主屏仅是虚拟屏的一部分
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return {true, fmt::format("screen_size: {}x{} (virtual screen, covers all monitors)", vw, vh)};
}

/// 命令对象字段 (顺序无关提取; ondemand 惰性迭代要求按序访问, 这里遍历全部字段匹配)
struct UiCmdFields {
    bool        hasAction = false;
    bool        hasX = false, hasY = false, hasX1 = false, hasY1 = false;
    bool        hasX2 = false, hasY2 = false, hasDelta = false, hasKey = false;
    bool        hasKeys = false, hasText = false, hasButton = false, hasDuration = false;
    bool        hasMs = false;
    std::string action, button, key, text;
    std::vector<std::string> keys;
    int64_t                  x = 0, y = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    int64_t                  delta = 0, duration = 200, ms = 100;
};

static bool uiControlParseCmd(simdjson::ondemand::value& v, UiCmdFields& f) {
    if (v.type().error() || v.type().value() != simdjson::ondemand::json_type::object) {
        return false;
    }
    simdjson::ondemand::object obj;
    if (v.get_object().get(obj)) {
        return false;
    }
    for (auto field : obj) {
        std::string_view key;
        // key() 返回 raw_json_string (get 只接受 raw_json_string&), 用
        // unescaped_key 直接取转义后的 string_view (simdjson API 兼容)
        if (field.unescaped_key().get(key)) {
            continue;
        }
        auto val = field.value();
        if (key == "action") {
            f.hasAction = jsonGetString(val, f.action);
        } else if (key == "x") {
            f.hasX = jsonGetInt(val, f.x);
        } else if (key == "y") {
            f.hasY = jsonGetInt(val, f.y);
        } else if (key == "x1") {
            f.hasX1 = jsonGetInt(val, f.x1);
        } else if (key == "y1") {
            f.hasY1 = jsonGetInt(val, f.y1);
        } else if (key == "x2") {
            f.hasX2 = jsonGetInt(val, f.x2);
        } else if (key == "y2") {
            f.hasY2 = jsonGetInt(val, f.y2);
        } else if (key == "delta") {
            f.hasDelta = jsonGetInt(val, f.delta);
        } else if (key == "key") {
            f.hasKey = jsonGetString(val, f.key);
        } else if (key == "text") {
            f.hasText = jsonGetString(val, f.text);
        } else if (key == "button") {
            f.hasButton = jsonGetString(val, f.button);
        } else if (key == "ms") {
            f.hasMs = jsonGetInt(val, f.ms);
        } else if (key == "durationMs") {
            f.hasDuration = jsonGetInt(val, f.duration);
        } else if (key == "keys") {
            simdjson::ondemand::array arr;
            if (!val.value().get_array().get(arr)) {
                f.hasKeys = true;
                for (auto e : arr) {
                    std::string_view sv;
                    if (e.get_string().get(sv)) {
                        break;
                    }
                    f.keys.emplace_back(sv);
                }
            }
        }
    }
    return true;
}

static UICmdResult uiControlExecuteOne(const UiCmdFields& f) {
    const auto& action = f.action;

    if (action == "mouse_move") {
        if (!f.hasX || !f.hasY) {
            return UICmdResult{false, "mouse_move requires `x` and `y`"};
        }
        return uiControlMouseMove(static_cast<int>(f.x), static_cast<int>(f.y));
    }

    if (action == "mouse_click" || action == "mouse_double_click") {
        auto button = f.hasButton ? f.button : std::string{"left"};
        bool at     = f.hasX && f.hasY;
        int  count  = (action == "mouse_double_click") ? 2 : 1;
        return uiControlMouseClick(button, static_cast<int>(f.x), static_cast<int>(f.y), at, count);
    }

    if (action == "mouse_scroll") {
        if (!f.hasDelta) {
            return UICmdResult{false, "mouse_scroll requires `delta`"};
        }
        bool at = f.hasX && f.hasY;
        return uiControlMouseScroll(
            static_cast<int>(f.delta),
            static_cast<int>(f.x),
            static_cast<int>(f.y),
            at
        );
    }

    if (action == "mouse_drag") {
        if (!f.hasX1 || !f.hasY1 || !f.hasX2 || !f.hasY2) {
            return UICmdResult{false, "mouse_drag requires `x1`, `y1`, `x2`, `y2`"};
        }
        auto button   = f.hasButton ? f.button : std::string{"left"};
        int  duration = static_cast<int>(f.duration);
        return uiControlMouseDrag(
            static_cast<int>(f.x1),
            static_cast<int>(f.y1),
            static_cast<int>(f.x2),
            static_cast<int>(f.y2),
            button,
            duration
        );
    }

    if (action == "key_press") {
        if (!f.hasKey) {
            return UICmdResult{false, "key_press requires `key`"};
        }
        WORD vk = uiControlKeyNameToVk(f.key);
        if (vk == 0) {
            return UICmdResult{false, fmt::format("unknown key: {}", f.key)};
        }
        // 符号字符 (如 `!` `@`) 需要按住 Shift 才能输入
        bool withShift = (f.key.size() == 1 && uiControlNeedsShift(f.key[0]));
        return uiControlKeyPress(vk, withShift);
    }

    if (action == "key_down") {
        if (!f.hasKey) {
            return UICmdResult{false, "key_down requires `key`"};
        }
        WORD vk = uiControlKeyNameToVk(f.key);
        if (vk == 0) {
            return UICmdResult{false, fmt::format("unknown key: {}", f.key)};
        }
        bool withShift = (f.key.size() == 1 && uiControlNeedsShift(f.key[0]));
        return uiControlKeyDown(vk, withShift);
    }

    if (action == "key_up") {
        if (!f.hasKey) {
            return UICmdResult{false, "key_up requires `key`"};
        }
        WORD vk = uiControlKeyNameToVk(f.key);
        if (vk == 0) {
            return UICmdResult{false, fmt::format("unknown key: {}", f.key)};
        }
        bool withShift = (f.key.size() == 1 && uiControlNeedsShift(f.key[0]));
        return uiControlKeyUp(vk, withShift);
    }

    if (action == "key_combo") {
        if (!f.hasKeys || f.keys.empty()) {
            return UICmdResult{false, "key_combo requires `keys` array"};
        }
        std::vector<WORD> vks;
        bool              needShift = false;
        for (const auto& keyStr : f.keys) {
            WORD vk = uiControlKeyNameToVk(keyStr);
            if (vk == 0) {
                return UICmdResult{false, fmt::format("unknown key in combo: {}", keyStr)};
            }
            // 组合中含符号字符 (如 `!` `?`) 时自动补 Shift
            if (keyStr.size() == 1 && uiControlNeedsShift(keyStr[0])) {
                needShift = true;
            }
            vks.push_back(vk);
        }
        if (needShift) {
            // Shift 在组合最前按下、最后松开 (key_combo 的松开顺序为逆序)
            vks.insert(vks.begin(), VK_LSHIFT);
        }
        return uiControlKeyCombo(vks);
    }

    if (action == "key_type") {
        if (!f.hasText) {
            return UICmdResult{false, "key_type requires `text`"};
        }
        return uiControlKeyType(f.text);
    }

    if (action == "wait") {
        int ms = static_cast<int>(f.ms);
        ms     = std::clamp(ms, 0, 30000);
        uiControlDelay(ms);
        return UICmdResult{true, fmt::format("wait {}ms", ms)};
    }

    if (action == "get_cursor_pos") {
        return uiControlGetCursorPos();
    }

    if (action == "get_screen_size") {
        return uiControlGetScreenSize();
    }

    return UICmdResult{false, fmt::format("unknown action: {}", action)};
}

std::string uiControlExecute(agentxx_computer_use_plugin::SimpleJson& arguments) {
    auto commands = arguments.doc().at_pointer("/commands");
    if (commands.error()) {
        return R"({"error":"Arg `commands` is required and must be an array"})";
    }
    simdjson::ondemand::array arr;
    if (commands.value().get_array().get(arr)) {
        return R"({"error":"Arg `commands` is required and must be an array"})";
    }
    int64_t interval_ms = 50;
    jsonGetInt(arguments.doc().at_pointer("/interval_ms"), interval_ms);

    neograph::json results    = neograph::json::array();
    int             ok_count   = 0;
    int             fail_count = 0;
    size_t          i          = 0;
    bool            first      = true;
    for (auto elem : arr) {
        if (!first && interval_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }
        first = false;

        UiCmdFields f;
        if (elem.error() || !uiControlParseCmd(elem.value(), f) || !f.hasAction
            || f.action.empty()) {
            results.push_back(neograph::json{
                {"index",  i                       },
                {"action", ""                      },
                {"ok",     false                   },
                {"msg",    "missing `action` field"},
            });
            fail_count++;
            break;
        }
        auto r = uiControlExecuteOne(f);
        if (r.ok) {
            ok_count++;
        } else {
            fail_count++;
        }
        results.push_back(neograph::json{
            {"index",  i       },
            {"action", f.action},
            {"ok",     r.ok    },
            {"msg",    r.msg   },
        });
        if (!r.ok) {
            break;
        }
        ++i;
    }

    return results.dump();
}
#else
std::string uiControlExecute(agentxx_computer_use_plugin::SimpleJson&) {
    return R"({"error":"agentxx_ui_control_keyboard_mouse is not available on current system"})";
}
#endif

} // namespace agentxx_computer_use_plugin
