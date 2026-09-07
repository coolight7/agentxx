#pragma once
#include <string_view>

namespace agentxx::client {

/// 跨平台将文本写入系统剪贴板 (仅写入, 不读取)
/// - Windows: 使用 Win32 API (OpenClipboard / SetClipboardData CF_UNICODETEXT)
/// - Linux/macOS/WSL: 使用 OSC 52 转义序列写入终端主剪贴板
/// - `text`: 待复制的 UTF-8 文本内容
/// - `return`: 写入成功返回 true, 失败或文本为空返回 false
bool copyTextToSystemClipboard(std::string_view text);

} // namespace agentxx::client
