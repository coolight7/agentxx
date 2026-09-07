#include "agentxx-client/util/clipboard.h"
#include "agentxx/util/string_util.h"
#include <iostream>

#if XX_IS_WIN_D
#include <windows.h>

namespace agentxx::client {

bool copyTextToSystemClipboard(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    // OpenClipboard(nullptr): 不关联具体窗口, 供无 GUI 窗口句柄的线程使用
    if (!OpenClipboard(nullptr)) {
        return false;
    }
    EmptyClipboard();
    bool      ok = false;
    const int wlen
        = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (wlen > 0) {
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(wlen + 1) * sizeof(wchar_t));
        if (hMem) {
            wchar_t* dst = static_cast<wchar_t*>(GlobalLock(hMem));
            if (dst) {
                MultiByteToWideChar(
                    CP_UTF8,
                    0,
                    text.data(),
                    static_cast<int>(text.size()),
                    dst,
                    wlen
                );
                dst[wlen] = L'\0';
                GlobalUnlock(hMem);
                // 成功时剪贴板拥有 hMem 所有权; 失败则释放, 避免泄漏
                ok = SetClipboardData(CF_UNICODETEXT, hMem) != nullptr;
                if (!ok) {
                    GlobalFree(hMem);
                }
            } else {
                GlobalFree(hMem);
            }
        }
    }
    CloseClipboard();
    return ok;
}

} // namespace agentxx::client

#else

namespace agentxx::client {

bool copyTextToSystemClipboard(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    // ESC ] 52 ; c ; <base64> BEL — 无可见输出, 与 FTXUI 屏幕刷新流交错安全
    std::cout << "\x1b]52;c;" << agentxx::util::base64Encode(text) << "\x07" << std::flush;
    return true;
}

} // namespace agentxx::client

#endif
