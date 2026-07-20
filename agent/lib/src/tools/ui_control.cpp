#include "agentxx/tools/ui_control.h"
#include "agentxx/agent/prompt.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "asio/awaitable.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#if XX_IS_WIN_D
#include <windows.h>
#endif

namespace agentxx {
namespace tools {

#if XX_IS_WIN_D

static WORD uiControlCharToVk(char ch) {
  if (ch >= 'A' && ch <= 'Z')
    return static_cast<WORD>(ch);
  if (ch >= 'a' && ch <= 'z')
    return static_cast<WORD>(toupper(ch));
  if (ch >= '0' && ch <= '9')
    return static_cast<WORD>(ch);
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

static WORD uiControlKeyNameToVk(std::string_view key) {
  if (key.size() == 1) {
    return uiControlCharToVk(key[0]);
  }

  auto upper = agentxx::util::toUpper(key);
  for (auto &c : upper) {
    c = static_cast<char>(toupper(c));
  }

  if (upper == "ENTER" || upper == "RETURN")
    return VK_RETURN;
  if (upper == "TAB")
    return VK_TAB;
  if (upper == "ESCAPE" || upper == "ESC")
    return VK_ESCAPE;
  if (upper == "BACKSPACE" || upper == "BACK")
    return VK_BACK;
  if (upper == "DELETE" || upper == "DEL")
    return VK_DELETE;
  if (upper == "INSERT" || upper == "INS")
    return VK_INSERT;
  if (upper == "HOME")
    return VK_HOME;
  if (upper == "END")
    return VK_END;
  if (upper == "PAGEUP" || upper == "PAGE_UP")
    return VK_PRIOR;
  if (upper == "PAGEDOWN" || upper == "PAGE_DOWN")
    return VK_NEXT;
  if (upper == "UP" || upper == "ARROWUP" || upper == "ARROW_UP")
    return VK_UP;
  if (upper == "DOWN" || upper == "ARROWDOWN" || upper == "ARROW_DOWN")
    return VK_DOWN;
  if (upper == "LEFT" || upper == "ARROWLEFT" || upper == "ARROW_LEFT")
    return VK_LEFT;
  if (upper == "RIGHT" || upper == "ARROWRIGHT" || upper == "ARROW_RIGHT")
    return VK_RIGHT;
  if (upper == "SPACE")
    return VK_SPACE;
  if (upper == "F1")
    return VK_F1;
  if (upper == "F2")
    return VK_F2;
  if (upper == "F3")
    return VK_F3;
  if (upper == "F4")
    return VK_F4;
  if (upper == "F5")
    return VK_F5;
  if (upper == "F6")
    return VK_F6;
  if (upper == "F7")
    return VK_F7;
  if (upper == "F8")
    return VK_F8;
  if (upper == "F9")
    return VK_F9;
  if (upper == "F10")
    return VK_F10;
  if (upper == "F11")
    return VK_F11;
  if (upper == "F12")
    return VK_F12;
  if (upper == "SHIFT" || upper == "LSHIFT")
    return VK_LSHIFT;
  if (upper == "RSHIFT")
    return VK_RSHIFT;
  if (upper == "CTRL" || upper == "CONTROL" || upper == "LCTRL")
    return VK_LCONTROL;
  if (upper == "RCTRL")
    return VK_RCONTROL;
  if (upper == "ALT" || upper == "LALT")
    return VK_LMENU;
  if (upper == "RALT")
    return VK_RMENU;
  if (upper == "WIN" || upper == "LWIN" || upper == "META")
    return VK_LWIN;
  if (upper == "RWIN")
    return VK_RWIN;
  if (upper == "APPS" || upper == "MENU")
    return VK_APPS;
  if (upper == "CAPSLOCK" || upper == "CAPS")
    return VK_CAPITAL;
  if (upper == "NUMLOCK")
    return VK_NUMLOCK;
  if (upper == "SCROLLLOCK")
    return VK_SCROLL;
  if (upper == "PRINTSCREEN" || upper == "PRTSC")
    return VK_SNAPSHOT;
  if (upper == "PAUSE" || upper == "BREAK")
    return VK_PAUSE;
  if (upper == "NUMPAD0")
    return VK_NUMPAD0;
  if (upper == "NUMPAD1")
    return VK_NUMPAD1;
  if (upper == "NUMPAD2")
    return VK_NUMPAD2;
  if (upper == "NUMPAD3")
    return VK_NUMPAD3;
  if (upper == "NUMPAD4")
    return VK_NUMPAD4;
  if (upper == "NUMPAD5")
    return VK_NUMPAD5;
  if (upper == "NUMPAD6")
    return VK_NUMPAD6;
  if (upper == "NUMPAD7")
    return VK_NUMPAD7;
  if (upper == "NUMPAD8")
    return VK_NUMPAD8;
  if (upper == "NUMPAD9")
    return VK_NUMPAD9;
  if (upper == "VOLUME_UP" || upper == "VOLUMEUP")
    return VK_VOLUME_UP;
  if (upper == "VOLUME_DOWN" || upper == "VOLUMEDOWN")
    return VK_VOLUME_DOWN;
  if (upper == "VOLUME_MUTE" || upper == "VOLUMEMUTE")
    return VK_VOLUME_MUTE;

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
    if (vk >= VK_F1 && vk <= VK_F12)
      return fmt::format("F{}", vk - VK_F1 + 1);
    if (vk >= 'A' && vk <= 'Z')
      return std::string(1, static_cast<char>(vk));
    if (vk >= '0' && vk <= '9')
      return std::string(1, static_cast<char>(vk));
    return fmt::format("Vk({})", vk);
  }
}

struct UICmdResult {
  bool ok = true;
  std::string msg;
};

static asio::awaitable<void> uiControlDelay(int ms) {
  asio::steady_timer timer(co_await asio::this_coro::executor);
  timer.expires_after(std::chrono::milliseconds(ms));
  co_await timer.async_wait(asio::use_awaitable);
}

static UINT uiControlSendInput(UINT cInputs, LPINPUT pInputs, int cbSize) {
  UINT sent = SendInput(cInputs, pInputs, cbSize);
  if (sent < cInputs) {
    // TODO: 支持选择指定窗口
    HWND fgWnd = GetForegroundWindow();
    if (fgWnd) {
      DWORD fgThreadId = GetWindowThreadProcessId(fgWnd, nullptr);
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

static asio::awaitable<UINT>
uiControlSendInputAsync(UINT cInputs, LPINPUT pInputs, int cbSize) {
  // TODO: 异步
  UINT sent = uiControlSendInput(cInputs, pInputs, cbSize);
  co_return sent;
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

static void uiControlPrepareKeyInput(INPUT &input, WORD vk, DWORD flags) {
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = vk;
  input.ki.wScan = static_cast<WORD>(MapVirtualKey(vk, MAPVK_VK_TO_VSC));
  input.ki.dwFlags = flags | KEYEVENTF_SCANCODE;
  input.ki.time = 0;
  input.ki.dwExtraInfo = 0;
  if (uiControlIsExtendedKey(vk)) {
    input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
  }
}

static asio::awaitable<void> uiControlMouseMoveTo(int x, int y) {
  int screenW = GetSystemMetrics(SM_CXSCREEN);
  int screenH = GetSystemMetrics(SM_CYSCREEN);
  INPUT input = {};
  input.type = INPUT_MOUSE;
  input.mi.dx = static_cast<LONG>((static_cast<LONGLONG>(x) * 65535) / screenW);
  input.mi.dy = static_cast<LONG>((static_cast<LONGLONG>(y) * 65535) / screenH);
  input.mi.mouseData = 0;
  input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
  input.mi.time = 0;
  input.mi.dwExtraInfo = 0;
  co_await uiControlSendInputAsync(1, &input, sizeof(INPUT));
}

static std::pair<int, int> uiControlGetCursorPosPair() {
  POINT pt;
  GetCursorPos(&pt);
  return {pt.x, pt.y};
}

static asio::awaitable<UICmdResult> uiControlMouseMove(int x, int y) {
  co_await uiControlMouseMoveTo(x, y);
  co_return UICmdResult{true, fmt::format("mouse_move -> ({}, {})", x, y)};
}

static asio::awaitable<UICmdResult> uiControlMouseClick(std::string_view button,
                                                        int x, int y, bool at,
                                                        int click_count) {
  if (at) {
    co_await uiControlMouseMoveTo(x, y);
    co_await uiControlDelay(30);
  }

  DWORD downFlag = 0, upFlag = 0;
  DWORD dataVal = 0;
  std::string btnName;

  if (button == "right") {
    downFlag = MOUSEEVENTF_RIGHTDOWN;
    upFlag = MOUSEEVENTF_RIGHTUP;
    btnName = "right";
  } else if (button == "middle") {
    downFlag = MOUSEEVENTF_MIDDLEDOWN;
    upFlag = MOUSEEVENTF_MIDDLEUP;
    btnName = "middle";
  } else {
    downFlag = MOUSEEVENTF_LEFTDOWN;
    upFlag = MOUSEEVENTF_LEFTUP;
    btnName = "left";
  }

  auto [ptX, ptY] = uiControlGetCursorPosPair();

  for (int i = 0; i < click_count; i++) {
    INPUT inputDown = {};
    inputDown.type = INPUT_MOUSE;
    inputDown.mi.dx = 0;
    inputDown.mi.dy = 0;
    inputDown.mi.mouseData = dataVal;
    inputDown.mi.dwFlags = downFlag;
    inputDown.mi.time = 0;
    inputDown.mi.dwExtraInfo = 0;
    co_await uiControlSendInputAsync(1, &inputDown, sizeof(INPUT));

    co_await uiControlDelay(20);

    INPUT inputUp = {};
    inputUp.type = INPUT_MOUSE;
    inputUp.mi.dx = 0;
    inputUp.mi.dy = 0;
    inputUp.mi.mouseData = dataVal;
    inputUp.mi.dwFlags = upFlag;
    inputUp.mi.time = 0;
    inputUp.mi.dwExtraInfo = 0;
    co_await uiControlSendInputAsync(1, &inputUp, sizeof(INPUT));

    if (i < click_count - 1) {
      co_await uiControlDelay(80);
    }
  }

  std::string action = click_count > 1 ? "double_click" : "click";
  if (at) {
    co_return UICmdResult{
        true, fmt::format("mouse_{} @ ({}, {}) [{}]", action, x, y, btnName)};
  }
  co_return UICmdResult{
      true, fmt::format("mouse_{} @ ({}, {}) [{}]", action, ptX, ptY, btnName)};
}

static asio::awaitable<UICmdResult> uiControlMouseScroll(int delta, int x,
                                                         int y, bool at) {
  if (at) {
    co_await uiControlMouseMoveTo(x, y);
    co_await uiControlDelay(30);
  }

  INPUT input = {};
  input.type = INPUT_MOUSE;
  input.mi.dx = 0;
  input.mi.dy = 0;
  input.mi.mouseData = static_cast<DWORD>(delta);
  input.mi.dwFlags = MOUSEEVENTF_WHEEL;
  input.mi.time = 0;
  input.mi.dwExtraInfo = 0;

  co_await uiControlSendInputAsync(1, &input, sizeof(INPUT));

  auto [ptX, ptY] = uiControlGetCursorPosPair();
  co_return UICmdResult{
      true, fmt::format("mouse_scroll delta={} @ ({}, {})", delta, ptX, ptY)};
}

static asio::awaitable<UICmdResult> uiControlMouseDrag(int x1, int y1, int x2,
                                                       int y2,
                                                       std::string_view button,
                                                       int duration_ms) {
  co_await uiControlMouseMoveTo(x1, y1);
  co_await uiControlDelay(30);

  DWORD downFlag = 0, upFlag = 0;
  if (button == "right") {
    downFlag = MOUSEEVENTF_RIGHTDOWN;
    upFlag = MOUSEEVENTF_RIGHTUP;
  } else if (button == "middle") {
    downFlag = MOUSEEVENTF_MIDDLEDOWN;
    upFlag = MOUSEEVENTF_MIDDLEUP;
  } else {
    downFlag = MOUSEEVENTF_LEFTDOWN;
    upFlag = MOUSEEVENTF_LEFTUP;
  }

  INPUT inputDown = {};
  inputDown.type = INPUT_MOUSE;
  inputDown.mi.dx = 0;
  inputDown.mi.dy = 0;
  inputDown.mi.dwFlags = downFlag;
  inputDown.mi.time = 0;
  inputDown.mi.dwExtraInfo = 0;
  co_await uiControlSendInputAsync(1, &inputDown, sizeof(INPUT));

  co_await uiControlDelay(50);

  int steps = std::max(duration_ms / 10, 1);
  for (int i = 1; i <= steps; i++) {
    int cx = x1 + (x2 - x1) * i / steps;
    int cy = y1 + (y2 - y1) * i / steps;
    co_await uiControlMouseMoveTo(cx, cy);
    co_await uiControlDelay(duration_ms / steps);
  }

  co_await uiControlMouseMoveTo(x2, y2);
  co_await uiControlDelay(30);

  INPUT inputUp = {};
  inputUp.type = INPUT_MOUSE;
  inputUp.mi.dx = 0;
  inputUp.mi.dy = 0;
  inputUp.mi.dwFlags = upFlag;
  inputUp.mi.time = 0;
  inputUp.mi.dwExtraInfo = 0;
  co_await uiControlSendInputAsync(1, &inputUp, sizeof(INPUT));

  co_return UICmdResult{true,
                        fmt::format("mouse_drag ({}, {}) -> ({}, {}) [{}]", x1,
                                    y1, x2, y2, button)};
}

static asio::awaitable<UICmdResult> uiControlKeyDown(WORD vk) {
  INPUT input = {};
  uiControlPrepareKeyInput(input, vk, 0);
  co_await uiControlSendInputAsync(1, &input, sizeof(INPUT));
  co_return UICmdResult{true,
                        fmt::format("key_down [{}]", uiControlVkToKeyName(vk))};
}

static asio::awaitable<UICmdResult> uiControlKeyUp(WORD vk) {
  INPUT input = {};
  uiControlPrepareKeyInput(input, vk, KEYEVENTF_KEYUP);
  co_await uiControlSendInputAsync(1, &input, sizeof(INPUT));
  co_return UICmdResult{true,
                        fmt::format("key_up [{}]", uiControlVkToKeyName(vk))};
}

static asio::awaitable<UICmdResult> uiControlKeyPress(WORD vk) {
  INPUT keyDown = {};
  uiControlPrepareKeyInput(keyDown, vk, 0);
  co_await uiControlSendInputAsync(1, &keyDown, sizeof(INPUT));

  co_await uiControlDelay(20);

  INPUT keyUp = {};
  uiControlPrepareKeyInput(keyUp, vk, KEYEVENTF_KEYUP);
  co_await uiControlSendInputAsync(1, &keyUp, sizeof(INPUT));

  co_return UICmdResult{
      true, fmt::format("key_press [{}]", uiControlVkToKeyName(vk))};
}

static asio::awaitable<UICmdResult>
uiControlKeyCombo(const std::vector<WORD> &vks) {
  if (vks.empty())
    co_return UICmdResult{false, "key_combo: empty keys"};

  std::string comboStr;
  for (auto vk : vks) {
    if (!comboStr.empty())
      comboStr += "+";
    comboStr += uiControlVkToKeyName(vk);
  }

  for (auto vk : vks) {
    INPUT down = {};
    uiControlPrepareKeyInput(down, vk, 0);
    co_await uiControlSendInputAsync(1, &down, sizeof(INPUT));
    co_await uiControlDelay(10);
  }

  co_await uiControlDelay(20);

  for (auto it = vks.rbegin(); it != vks.rend(); ++it) {
    INPUT up = {};
    uiControlPrepareKeyInput(up, *it, KEYEVENTF_KEYUP);
    co_await uiControlSendInputAsync(1, &up, sizeof(INPUT));
    co_await uiControlDelay(5);
  }

  co_return UICmdResult{true, fmt::format("key_combo [{}]", comboStr)};
}

static asio::awaitable<UICmdResult> uiControlKeyType(std::string_view text) {
  if (text.empty())
    co_return UICmdResult{true, "key_type [0 chars]"};

  std::vector<INPUT> inputs;
  inputs.reserve(text.size() * 2);

  for (char ch : text) {
    if (ch == '\n' || ch == '\r') {
      if (!inputs.empty()) {
        co_await uiControlSendInputAsync(static_cast<UINT>(inputs.size()),
                                         inputs.data(), sizeof(INPUT));
        inputs.clear();
        co_await uiControlDelay(5);
      }
      INPUT down = {};
      uiControlPrepareKeyInput(down, VK_RETURN, 0);
      co_await uiControlSendInputAsync(1, &down, sizeof(INPUT));
      co_await uiControlDelay(5);
      INPUT up = {};
      uiControlPrepareKeyInput(up, VK_RETURN, KEYEVENTF_KEYUP);
      co_await uiControlSendInputAsync(1, &up, sizeof(INPUT));
      co_await uiControlDelay(5);
      continue;
    }

    if (ch == '\t') {
      if (!inputs.empty()) {
        co_await uiControlSendInputAsync(static_cast<UINT>(inputs.size()),
                                         inputs.data(), sizeof(INPUT));
        inputs.clear();
        co_await uiControlDelay(5);
      }
      INPUT down = {};
      uiControlPrepareKeyInput(down, VK_TAB, 0);
      co_await uiControlSendInputAsync(1, &down, sizeof(INPUT));
      co_await uiControlDelay(5);
      INPUT up = {};
      uiControlPrepareKeyInput(up, VK_TAB, KEYEVENTF_KEYUP);
      co_await uiControlSendInputAsync(1, &up, sizeof(INPUT));
      co_await uiControlDelay(5);
      continue;
    }

    if (ch == '\b') {
      if (!inputs.empty()) {
        co_await uiControlSendInputAsync(static_cast<UINT>(inputs.size()),
                                         inputs.data(), sizeof(INPUT));
        inputs.clear();
        co_await uiControlDelay(5);
      }
      INPUT down = {};
      uiControlPrepareKeyInput(down, VK_BACK, 0);
      co_await uiControlSendInputAsync(1, &down, sizeof(INPUT));
      co_await uiControlDelay(5);
      INPUT up = {};
      uiControlPrepareKeyInput(up, VK_BACK, KEYEVENTF_KEYUP);
      co_await uiControlSendInputAsync(1, &up, sizeof(INPUT));
      co_await uiControlDelay(5);
      continue;
    }

    if (ch == '\x1b') {
      if (!inputs.empty()) {
        co_await uiControlSendInputAsync(static_cast<UINT>(inputs.size()),
                                         inputs.data(), sizeof(INPUT));
        inputs.clear();
        co_await uiControlDelay(5);
      }
      INPUT down = {};
      uiControlPrepareKeyInput(down, VK_ESCAPE, 0);
      co_await uiControlSendInputAsync(1, &down, sizeof(INPUT));
      co_await uiControlDelay(5);
      INPUT up = {};
      uiControlPrepareKeyInput(up, VK_ESCAPE, KEYEVENTF_KEYUP);
      co_await uiControlSendInputAsync(1, &up, sizeof(INPUT));
      co_await uiControlDelay(5);
      continue;
    }

    unsigned char uch = static_cast<unsigned char>(ch);
    if (uch < 32)
      continue;

    INPUT keyDown = {};
    keyDown.type = INPUT_KEYBOARD;
    keyDown.ki.wVk = 0;
    keyDown.ki.wScan = uch;
    keyDown.ki.dwFlags = KEYEVENTF_UNICODE;
    keyDown.ki.time = 0;
    keyDown.ki.dwExtraInfo = 0;
    inputs.push_back(keyDown);

    INPUT keyUp = {};
    keyUp.type = INPUT_KEYBOARD;
    keyUp.ki.wVk = 0;
    keyUp.ki.wScan = uch;
    keyUp.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    keyUp.ki.time = 0;
    keyUp.ki.dwExtraInfo = 0;
    inputs.push_back(keyUp);
  }

  if (!inputs.empty()) {
    co_await uiControlSendInputAsync(static_cast<UINT>(inputs.size()),
                                     inputs.data(), sizeof(INPUT));
  }

  co_return UICmdResult{true, fmt::format("key_type [{} chars]", text.size())};
}

static UICmdResult uiControlGetCursorPos() {
  auto [x, y] = uiControlGetCursorPosPair();
  return {true, fmt::format("cursor_pos: ({}, {})", x, y)};
}

static UICmdResult uiControlGetScreenSize() {
  int w = GetSystemMetrics(SM_CXSCREEN);
  int h = GetSystemMetrics(SM_CYSCREEN);
  return {true, fmt::format("screen_size: {}x{}", w, h)};
}

static asio::awaitable<UICmdResult>
uiControlExecuteOne(const neograph::json &cmd) {
  if (!cmd.is_object() || !cmd.contains("action")) {
    co_return UICmdResult{false, "missing `action` field"};
  }

  auto action = cmd.value("action", std::string{});
  if (action.empty()) {
    co_return UICmdResult{false, "`action` is empty"};
  }

  if (action == "mouse_move") {
    if (!cmd.contains("x") || !cmd.contains("y")) {
      co_return UICmdResult{false, "mouse_move requires `x` and `y`"};
    }
    co_return co_await uiControlMouseMove(cmd.value<int>("x", 0),
                                          cmd.value<int>("y", 0));
  }

  if (action == "mouse_click" || action == "mouse_double_click") {
    auto button = cmd.value("button", std::string{"left"});
    int x = cmd.value<int>("x", 0);
    int y = cmd.value<int>("y", 0);
    bool at = cmd.contains("x") && cmd.contains("y");
    int count = (action == "mouse_double_click") ? 2 : 1;
    co_return co_await uiControlMouseClick(button, x, y, at, count);
  }

  if (action == "mouse_scroll") {
    if (!cmd.contains("delta")) {
      co_return UICmdResult{false, "mouse_scroll requires `delta`"};
    }
    int delta = cmd.value<int>("delta", 0);
    int x = cmd.value<int>("x", 0);
    int y = cmd.value<int>("y", 0);
    bool at = cmd.contains("x") && cmd.contains("y");
    co_return co_await uiControlMouseScroll(delta, x, y, at);
  }

  if (action == "mouse_drag") {
    if (!cmd.contains("x1") || !cmd.contains("y1") || !cmd.contains("x2") ||
        !cmd.contains("y2")) {
      co_return UICmdResult{false,
                            "mouse_drag requires `x1`, `y1`, `x2`, `y2`"};
    }
    auto button = cmd.value("button", std::string{"left"});
    int duration = cmd.value<int>("duration_ms", 200);
    co_return co_await uiControlMouseDrag(
        cmd.value<int>("x1", 0), cmd.value<int>("y1", 0),
        cmd.value<int>("x2", 0), cmd.value<int>("y2", 0), button, duration);
  }

  if (action == "key_press") {
    if (!cmd.contains("key")) {
      co_return UICmdResult{false, "key_press requires `key`"};
    }
    auto key = cmd.value("key", std::string{});
    WORD vk = uiControlKeyNameToVk(key);
    if (vk == 0) {
      co_return UICmdResult{false, fmt::format("unknown key: {}", key)};
    }
    co_return co_await uiControlKeyPress(vk);
  }

  if (action == "key_down") {
    if (!cmd.contains("key")) {
      co_return UICmdResult{false, "key_down requires `key`"};
    }
    auto key = cmd.value("key", std::string{});
    WORD vk = uiControlKeyNameToVk(key);
    if (vk == 0) {
      co_return UICmdResult{false, fmt::format("unknown key: {}", key)};
    }
    co_return co_await uiControlKeyDown(vk);
  }

  if (action == "key_up") {
    if (!cmd.contains("key")) {
      co_return UICmdResult{false, "key_up requires `key`"};
    }
    auto key = cmd.value("key", std::string{});
    WORD vk = uiControlKeyNameToVk(key);
    if (vk == 0) {
      co_return UICmdResult{false, fmt::format("unknown key: {}", key)};
    }
    co_return co_await uiControlKeyUp(vk);
  }

  if (action == "key_combo") {
    if (!cmd.contains("keys") || !cmd["keys"].is_array()) {
      co_return UICmdResult{false, "key_combo requires `keys` array"};
    }
    std::vector<WORD> vks;
    for (const auto &k : cmd["keys"]) {
      auto keyStr = k.get<std::string>();
      WORD vk = uiControlKeyNameToVk(keyStr);
      if (vk == 0) {
        co_return UICmdResult{false,
                              fmt::format("unknown key in combo: {}", keyStr)};
      }
      vks.push_back(vk);
    }
    co_return co_await uiControlKeyCombo(vks);
  }

  if (action == "key_type") {
    if (!cmd.contains("text")) {
      co_return UICmdResult{false, "key_type requires `text`"};
    }
    co_return co_await uiControlKeyType(cmd.value("text", std::string{}));
  }

  if (action == "wait") {
    int ms = cmd.value<int>("ms", 100);
    ms = std::clamp(ms, 0, 30000);
    co_await uiControlDelay(ms);
    co_return UICmdResult{true, fmt::format("wait {}ms", ms)};
  }

  if (action == "get_cursor_pos") {
    co_return uiControlGetCursorPos();
  }

  if (action == "get_screen_size") {
    co_return uiControlGetScreenSize();
  }

  co_return UICmdResult{false, fmt::format("unknown action: {}", action)};
}

#endif // XX_IS_WIN_D

neograph::ChatTool UIControlKeyboardMouseTool::get_definition() const {
  auto agentPtr = agentContext.lock();
  const auto &prompt =
      agentPtr->agentConfig->prompt.toolPrompt["ui_control_keyboard_mouse"];

  return {
      "ui_control_keyboard_mouse",
      prompt.depict,
      {
          {"type", "object"},
          {
              "properties",
              {
                  {
                      "commands",
                      {
                          {"type", "array"},
                          {"description", prompt.getArg("commands")},
                          {
                              "items",
                              {
                                  {"type", "object"},
                                  {"properties",
                                   {{
                                        "action",
                                        {{"type", "string"},
                                         {"description",
                                          "Action to perform. One of: "
                                          "mouse_move, "
                                          "mouse_click, "
                                          "mouse_double_click, "
                                          "mouse_scroll, mouse_drag, "
                                          "key_press, "
                                          "key_down, key_up, key_combo, "
                                          "key_type, "
                                          "wait, get_cursor_pos, "
                                          "get_screen_size"}},
                                    },
                                    {"x", {{"type", "number"}}},
                                    {"y", {{"type", "number"}}},
                                    {"x1", {{"type", "number"}}},
                                    {"y1", {{"type", "number"}}},
                                    {"x2", {{"type", "number"}}},
                                    {"y2", {{"type", "number"}}},
                                    {
                                        "button",
                                        {
                                            {"type", "string"},
                                            {"enum",
                                             {"left", "right", "middle"}},
                                        },
                                    },
                                    {"delta", {{"type", "number"}}},
                                    {
                                        "key",
                                        {{"type", "string"},
                                         {"description",
                                          "Key name for "
                                          "key_press/key_down/key_up"}},
                                    },
                                    {
                                        "keys",
                                        {
                                            {"type", "array"},
                                            {"items", {{"type", "string"}}},
                                            {"description",
                                             "Key names for key_combo, "
                                             "e.g. "
                                             "[\"ctrl\",\"c\"]"},
                                        },
                                    },
                                    {
                                        "text",
                                        {{"type", "string"},
                                         {"description",
                                          "Text string for key_type"}},
                                    },
                                    {
                                        "ms",
                                        {{"type", "number"},
                                         {"description", "Wait duration in "
                                                         "milliseconds"}},
                                    },
                                    {
                                        "duration_ms",
                                        {{"type", "number"},
                                         {"description", "Drag duration in "
                                                         "milliseconds"}},
                                    }}},
                                  {
                                      "required",
                                      neograph::json::array({"action"}),
                                  },
                              },
                          },
                      },
                  },
                  {
                      "interval_ms",
                      {
                          {"type", "number"},
                          {
                              "description",
                              prompt.getArg("interval_ms"),
                          },
                      },
                  },
              },
          },
          {"required", neograph::json::array({"commands"})},
      },
  };
}

asio::awaitable<std::string>
UIControlKeyboardMouseTool::execute_async(const neograph::json &arguments) {
  if (!arguments.contains("commands") || !arguments["commands"].is_array()) {
    co_return R"({"error":"Arg `commands` is required and must be an array"})";
  }

  auto cmds = arguments["commands"].get<neograph::json>();
  if (cmds.empty()) {
    co_return R"({"error":"`commands` is empty"})";
  }

#if XX_IS_WIN_D
  int interval_ms = arguments.value<int>("interval_ms", 50);
  asio::steady_timer timer{co_await asio::this_coro::executor};
  timer.expires_after(std::chrono::milliseconds(interval_ms));

  neograph::json results = neograph::json::array();
  int ok_count = 0;
  int fail_count = 0;

  for (size_t i = 0; i < cmds.size(); i++) {
    auto r = co_await uiControlExecuteOne(cmds[i]);
    if (r.ok) {
      ok_count++;
    } else {
      fail_count++;
    }
    results.push_back(neograph::json{
        {"index", i},
        {"action", cmds[i].value("action", std::string{})},
        {"ok", r.ok},
        {"msg", r.msg},
    });

    if (!r.ok) {
      break;
    }

    if (interval_ms > 0 && i < cmds.size() - 1) {
      co_await timer.async_wait(asio::use_awaitable);
    }
  }

  co_return results.dump();
#else
  co_return R"({"error":"ui_control_keyboard_mouse is not available on current system"})";
#endif
}

} // namespace tools
} // namespace agentxx