#if XX_IS_WIN_D
// include 顺序是必要的
#include <winsock2.h>
// ---
#include <ws2tcpip.h>
// ---
#include <UIAutomation.h>
#include <ole2.h>
#include <oleacc.h>
#include <windows.h>

#undef max
#undef min

#endif
#include "text_selection_monitor_plugin.h"

#include "asio/awaitable.hpp"
#include "asio/use_awaitable.hpp"
#include "text_selection_monitor.h"
#include "utilxx_base/json_view.h"
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>

#include "utilxx_base/asio_error.h"
namespace asio = ::boost::asio;

namespace agentxx_text_selection_monitor_plugin {

#if XX_IS_WIN_D

/// 系统级 文本选择事件流
/// - 当选择 程序窗口、浏览器 中的文本时，收到事件，分析文本内容并发起通知
/// - 启动独立线程处理接收事件
class TextSelectionMonitor::Impl : public IUIAutomationEventHandler {
public:

    static constexpr UINT WM_SELECTION_COMPLETE = WM_APP + 1;

    Impl() :
        running_(false),
        debounceMs_(300),
        lastSelectionChangeTime_({}),
        selectionPending_(false),
        lastSelectionHwnd_(nullptr),
        refCount_(1) {}

    ~Impl() {
        stop();
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IUIAutomationEventHandler) {
            *ppv = static_cast<IUIAutomationEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        return static_cast<ULONG>(InterlockedDecrement(&refCount_));
    }

    void addListener(TextSelectionListener listener) {
        std::lock_guard<std::mutex> lock(listenersMutex_);
        listeners_.push_back(std::move(listener));
    }

    void removeAllListeners() {
        std::lock_guard<std::mutex> lock(listenersMutex_);
        listeners_.clear();
    }

    void setDebounceMs(int ms) {
        debounceMs_ = ms;
    }

    bool start() {
        if (running_.load()) {
            return true;
        }

        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            XX_LOGE(
                "TextSelectionMonitor: CoInitializeEx failed, hr=0x{:08X}",
                static_cast<unsigned>(hr)
            );
            return false;
        }

        com_initialized_ = (SUCCEEDED(hr));

        hr = CoCreateInstance(
            CLSID_CUIAutomation,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_IUIAutomation,
            reinterpret_cast<void**>(&automation_)
        );
        if (FAILED(hr) || !automation_) {
            XX_LOGE(
                "TextSelectionMonitor: CoCreateInstance CUIAutomation failed, "
                "hr=0x{:08X}",
                static_cast<unsigned>(hr)
            );
            if (com_initialized_) {
                CoUninitialize();
                com_initialized_ = false;
            }
            return false;
        }

        instancePtr() = this;
        running_.store(true);

        IUIAutomationElement* desktop = nullptr;
        hr                            = automation_->GetRootElement(&desktop);
        if (SUCCEEDED(hr) && desktop) {
            automation_->AddAutomationEventHandler(
                UIA_Text_TextSelectionChangedEventId,
                desktop,
                TreeScope_Descendants,
                nullptr,
                static_cast<IUIAutomationEventHandler*>(this)
            );
            desktop->Release();
        }

        workerThread_ = std::thread(&Impl::workerLoop, this);
        XX_LOGI("TextSelectionMonitor: started");
        return true;
    }

    void stop() {
        if (!running_.load()) {
            return;
        }

        running_.store(false);

        if (hook_) {
            UnhookWinEvent(hook_);
            hook_ = nullptr;
        }

        if (mouseHook_) {
            UnhookWindowsHookEx(mouseHook_);
            mouseHook_ = nullptr;
        }

        if (workerThread_.joinable()) {
            PostThreadMessageW(GetThreadId(workerThread_.native_handle()), WM_QUIT, 0, 0);
            workerThread_.join();
        }

        if (processingThread_.joinable()) {
            processingThread_.join();
        }

        if (automation_) {
            automation_->RemoveAllEventHandlers();
            automation_->Release();
            automation_ = nullptr;
        }

        instancePtr() = nullptr;
        if (com_initialized_) {
            CoUninitialize();
            com_initialized_ = false;
        }
        XX_LOGI("TextSelectionMonitor: stopped");
    }

    bool isRunning() const {
        return running_.load();
    }

    /// CDP 网络动作 (插件注入; 未注入时不走 DevTools 路径)
    void setCdpNetFns(CdpNetFns fns) {
        std::lock_guard<std::mutex> lk(cdpFnsMutex_);
        cdpFns_ = std::move(fns);
    }

private:

    /// 取一份 CDP 动作副本 (持有锁; 回调里不要持锁调网络)
    CdpNetFns cdpFnsCopy() {
        std::lock_guard<std::mutex> lk(cdpFnsMutex_);
        return cdpFns_;
    }

    void workerLoop() {
        hook_ = SetWinEventHook(
            EVENT_OBJECT_TEXTSELECTIONCHANGED,
            EVENT_OBJECT_TEXTSELECTIONCHANGED,
            nullptr,
            WinEventProc,
            0,
            0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
        );

        if (!hook_) {
            XX_LOGE("TextSelectionMonitor: SetWinEventHook failed");
            running_.store(false);
            return;
        }

        mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, nullptr, 0);
        if (!mouseHook_) {
            XX_LOGE("TextSelectionMonitor: SetWindowsHookEx(WH_MOUSE_LL) failed");
            UnhookWinEvent(hook_);
            hook_ = nullptr;
            running_.store(false);
            return;
        }

        MSG msg;
        while (running_.load()) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    return;
                }
                if (msg.message == WM_SELECTION_COMPLETE) {
                    processPendingSelection();
                    continue;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            checkDebounce();

            if (running_.load()
                && MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT) == WAIT_TIMEOUT) {
                continue;
            }
        }
    }

    static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode >= 0) {
            Impl* self = instancePtr();
            if (!self) {
                return CallNextHookEx(nullptr, nCode, wParam, lParam);
            }

            MSLLHOOKSTRUCT* pMouseStruct = (MSLLHOOKSTRUCT*)lParam;

            switch (wParam) {
                case WM_LBUTTONDOWN:
                    self->mouseDownPos_ = pMouseStruct->pt;
                    break;

                case WM_LBUTTONUP: {
                    if (self->selectionPending_.load(std::memory_order_acquire)) {
                        PostThreadMessageW(GetCurrentThreadId(), WM_SELECTION_COMPLETE, 0, 0);
                    } else {
                        int  dx      = abs(pMouseStruct->pt.x - self->mouseDownPos_.x);
                        int  dy      = abs(pMouseStruct->pt.y - self->mouseDownPos_.y);
                        bool wasDrag = (dx > 3 || dy > 3);

                        auto now          = std::chrono::steady_clock::now();
                        auto clickElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - self->lastClickTime_
                        );
                        int  cdx = abs(pMouseStruct->pt.x - self->lastClickPos_.x);
                        int  cdy = abs(pMouseStruct->pt.y - self->lastClickPos_.y);
                        bool wasDoubleClick
                            = (clickElapsed.count() < GetDoubleClickTime() && cdx < 5 && cdy < 5);

                        if (wasDrag || wasDoubleClick) {
                            HWND hwnd = WindowFromPoint(pMouseStruct->pt);
                            if (hwnd) {
                                HWND                        rootHwnd = GetAncestor(hwnd, GA_ROOT);
                                std::lock_guard<std::mutex> lock(self->debounceMutex_);
                                self->lastSelectionHwnd_       = rootHwnd;
                                self->lastSelectionChangeTime_ = now;
                                self->selectionPending_.store(true, std::memory_order_release);
                                PostThreadMessageW(
                                    GetCurrentThreadId(),
                                    WM_SELECTION_COMPLETE,
                                    0,
                                    0
                                );
                            }
                        }
                        self->lastClickTime_ = now;
                        self->lastClickPos_  = pMouseStruct->pt;
                    }
                    break;
                }
            }
        }
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    static void CALLBACK WinEventProc(
        HWINEVENTHOOK /*hWinEventHook*/,
        DWORD event,
        HWND  hwnd,
        LONG  idObject,
        LONG /*idChild*/,
        DWORD /*dwEventThread*/,
        DWORD /*dwmsEventTime*/
    ) {
        if (event != EVENT_OBJECT_TEXTSELECTIONCHANGED) {
            return;
        }

        if (idObject != OBJID_CLIENT && idObject != OBJID_WINDOW) {
            return;
        }

        Impl* self = instancePtr();
        if (!self || !self->running_.load()) {
            return;
        }

        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(self->debounceMutex_);
            self->lastSelectionHwnd_       = hwnd;
            self->lastSelectionChangeTime_ = now;
        }
        self->selectionPending_.store(true, std::memory_order_release);
    }

    HRESULT STDMETHODCALLTYPE
        HandleAutomationEvent(IUIAutomationElement* sender, EVENTID eventId) override {
        if (eventId != UIA_Text_TextSelectionChangedEventId) {
            return S_OK;
        }
        if (!running_.load()) {
            return S_OK;
        }

        IUIAutomationTextPattern* textPattern = nullptr;
        HRESULT                   hr          = sender->GetCurrentPatternAs(
            UIA_TextPatternId,
            IID_IUIAutomationTextPattern,
            reinterpret_cast<void**>(&textPattern)
        );

        std::string extractedText;
        if (SUCCEEDED(hr) && textPattern) {
            IUIAutomationTextRangeArray* selectionRanges = nullptr;
            hr = textPattern->GetSelection(&selectionRanges);
            if (SUCCEEDED(hr) && selectionRanges) {
                int count = 0;
                selectionRanges->get_Length(&count);
                for (int i = 0; i < count; ++i) {
                    IUIAutomationTextRange* range = nullptr;
                    hr                            = selectionRanges->GetElement(i, &range);
                    if (SUCCEEDED(hr) && range) {
                        BSTR text = nullptr;
                        hr        = range->GetText(-1, &text);
                        if (SUCCEEDED(hr) && text) {
                            extractedText += bstrToUtf8(text);
                            SysFreeString(text);
                        }
                        range->Release();
                    }
                }
                selectionRanges->Release();
            }
            textPattern->Release();
        }

        if (extractedText.empty()) {
            return S_OK;
        }

        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(debounceMutex_);
            lastSelectionChangeTime_ = now;
            pendingUiaText_          = std::move(extractedText);
        }
        selectionPending_.store(true, std::memory_order_release);

        return S_OK;
    }

    static std::string bstrToUtf8(BSTR bstr) {
        if (!bstr) {
            return {};
        }
        int len = SysStringLen(bstr);
        if (len <= 0) {
            return {};
        }
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, bstr, len, nullptr, 0, nullptr, nullptr);
        if (utf8Len <= 0) {
            return {};
        }
        std::string result;
        result.resize(utf8Len);
        WideCharToMultiByte(CP_UTF8, 0, bstr, len, &result[0], utf8Len, nullptr, nullptr);
        return result;
    }

    std::pair<std::string, TextSource> getSelectedTextByEmGetSel(HWND hwnd) {
        HWND    targetHwnd    = hwnd;
        wchar_t className[64] = {};
        GetClassNameW(targetHwnd, className, 64);
        bool isEdit
            = (wcscmp(className, L"Edit") == 0 || wcsstr(className, L"RichEdit") != nullptr);

        if (!isEdit) {
            targetHwnd = FindWindowExW(hwnd, nullptr, L"Edit", nullptr);
            if (!targetHwnd) {
                return {};
            }
        }

        DWORD_PTR msgResult = 0;
        DWORD     selStart = 0, selEnd = 0;
        LRESULT   lr = SendMessageTimeoutW(
            targetHwnd,
            EM_GETSEL,
            reinterpret_cast<WPARAM>(&selStart),
            reinterpret_cast<LPARAM>(&selEnd),
            SMTO_ABORTIFHUNG,
            200,
            &msgResult
        );
        if (!lr || selStart >= selEnd) {
            return {};
        }

        int textLen = GetWindowTextLengthW(targetHwnd);
        if (textLen <= 0) {
            return {};
        }

        std::vector<wchar_t> buf(static_cast<size_t>(textLen) + 1);
        GetWindowTextW(targetHwnd, buf.data(), textLen + 1);
        std::wstring fullText(buf.data());

        if (selStart >= fullText.size() || selEnd > fullText.size()) {
            return {};
        }

        std::wstring selText = fullText.substr(selStart, selEnd - selStart);

        int utf8Len = WideCharToMultiByte(
            CP_UTF8,
            0,
            selText.data(),
            static_cast<int>(selText.size()),
            nullptr,
            0,
            nullptr,
            nullptr
        );
        if (utf8Len <= 0) {
            return {};
        }

        std::string result;
        result.resize(utf8Len);
        WideCharToMultiByte(
            CP_UTF8,
            0,
            selText.data(),
            static_cast<int>(selText.size()),
            &result[0],
            utf8Len,
            nullptr,
            nullptr
        );

        return {result, TextSource::EmGetSel};
    }

    std::pair<std::string, TextSource> getSelectedTextByAccessible(HWND hwnd) {
        IAccessible* acc = nullptr;
        HRESULT      hr  = AccessibleObjectFromWindow(
            hwnd,
            OBJID_CLIENT,
            IID_IAccessible,
            reinterpret_cast<void**>(&acc)
        );
        if (FAILED(hr) || !acc) {
            hr = AccessibleObjectFromWindow(
                hwnd,
                OBJID_WINDOW,
                IID_IAccessible,
                reinterpret_cast<void**>(&acc)
            );
            if (FAILED(hr) || !acc) {
                return {};
            }
        }

        std::pair<std::string, TextSource> ret;

        VARIANT varSel;
        VariantInit(&varSel);
        hr = acc->get_accSelection(&varSel);

        if (SUCCEEDED(hr) && varSel.vt == VT_I4) {
            BSTR value = nullptr;
            hr         = acc->get_accValue(varSel, &value);
            if (SUCCEEDED(hr) && value) {
                std::string text = bstrToUtf8(value);
                if (!text.empty()) {
                    ret = {text, TextSource::AccessibleObject};
                }
                SysFreeString(value);
            }
        } else if (SUCCEEDED(hr) && varSel.vt == VT_DISPATCH && varSel.pdispVal) {
            IEnumVARIANT* enumVar = nullptr;
            hr                    = varSel.pdispVal->QueryInterface(
                IID_IEnumVARIANT,
                reinterpret_cast<void**>(&enumVar)
            );
            if (SUCCEEDED(hr) && enumVar) {
                VARIANT childVar;
                VariantInit(&childVar);
                ULONG       fetched = 0;
                std::string combined;
                while (enumVar->Next(1, &childVar, &fetched) == S_OK && fetched == 1) {
                    if (childVar.vt == VT_DISPATCH && childVar.pdispVal) {
                        IAccessible* childAcc = nullptr;
                        hr                    = childVar.pdispVal->QueryInterface(
                            IID_IAccessible,
                            reinterpret_cast<void**>(&childAcc)
                        );
                        if (SUCCEEDED(hr) && childAcc) {
                            VARIANT varChildId;
                            varChildId.vt   = VT_I4;
                            varChildId.lVal = CHILDID_SELF;
                            BSTR name       = nullptr;
                            hr              = childAcc->get_accValue(varChildId, &name);
                            if (SUCCEEDED(hr) && name) {
                                std::string part = bstrToUtf8(name);
                                if (!part.empty()) {
                                    combined += part;
                                }
                                SysFreeString(name);
                            }
                            childAcc->Release();
                        }
                    }
                    VariantClear(&childVar);
                }
                if (!combined.empty()) {
                    ret = {combined, TextSource::AccessibleObject};
                }
                enumVar->Release();
            }
        }
        VariantClear(&varSel);
        acc->Release();
        return ret;
    }

    std::pair<std::string, TextSource> getSelectedTextByWmGetText(HWND hwnd) {
        int textLen = GetWindowTextLengthW(hwnd);
        if (textLen <= 0) {
            return {};
        }

        std::vector<wchar_t> buf(static_cast<size_t>(textLen) + 1);
        GetWindowTextW(hwnd, buf.data(), textLen + 1);
        std::wstring text(buf.data());

        if (text.empty()) {
            return {};
        }

        int utf8Len = WideCharToMultiByte(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            nullptr,
            0,
            nullptr,
            nullptr
        );
        if (utf8Len <= 0) {
            return {};
        }

        std::string result;
        result.resize(utf8Len);
        WideCharToMultiByte(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            &result[0],
            utf8Len,
            nullptr,
            nullptr
        );

        return {result, TextSource::WmGetText};
    }

    static bool isBrowserWindow(HWND hwnd) {
        wchar_t className[64] = {};
        GetClassNameW(hwnd, className, 64);
        if (wcscmp(className, L"Chrome_WidgetWin_1") == 0
            || wcscmp(className, L"Chrome_RenderWidgetHostHWND") == 0
            || wcscmp(className, L"MozillaWindowClass") == 0) {
            return true;
        }

        DWORD processId = 0;
        GetWindowThreadProcessId(hwnd, &processId);
        if (processId == 0) {
            return false;
        }

        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (!hProcess) {
            return false;
        }

        wchar_t exePath[MAX_PATH] = {};
        DWORD   size              = MAX_PATH;
        bool    isBrowser         = false;
        if (QueryFullProcessImageNameW(hProcess, 0, exePath, &size)) {
            wchar_t* fileName = wcsrchr(exePath, L'\\');
            if (!fileName) {
                fileName = exePath;
            } else {
                ++fileName;
            }
            _wcslwr_s(fileName, wcslen(fileName) + 1);
            static constexpr auto browserKeywords = std::array<const wchar_t*, 19>{
                L"chrome",
                L"chromium",
                L"firefox",
                L"msedge",
                L"edge",
                L"opera",
                L"brave",
                L"vivaldi",
                L"arc",
                L"360chrome",
                L"360se",
                L"sogouexplorer",
                L"qqbrowser",
                L"maxthon",
                L"liebao",
                L"browser",
                L"2345explorer",
                L"baidubrowser",
                L"ucbrowser"
            };
            for (const auto& kw : browserKeywords) {
                if (wcsstr(fileName, kw)) {
                    isBrowser = true;
                    break;
                }
            }
        }
        CloseHandle(hProcess);
        return isBrowser;
    }

    static bool isElectronAppWindow(HWND hwnd) {
        DWORD processId = 0;
        GetWindowThreadProcessId(hwnd, &processId);
        if (processId == 0) {
            return false;
        }

        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (!hProcess) {
            return false;
        }

        wchar_t exePath[MAX_PATH] = {};
        DWORD   size              = MAX_PATH;
        bool    isElectron        = false;
        if (QueryFullProcessImageNameW(hProcess, 0, exePath, &size)) {
            wchar_t* fileName = wcsrchr(exePath, L'\\');
            if (!fileName) {
                fileName = exePath;
            } else {
                ++fileName;
            }
            _wcslwr_s(fileName, wcslen(fileName) + 1);
            isElectron
                = (wcscmp(fileName, L"code.exe") == 0 || wcscmp(fileName, L"code") == 0
                   || wcscmp(fileName, L"code-insiders.exe") == 0
                   || wcscmp(fileName, L"code-insiders") == 0
                   || wcscmp(fileName, L"codium.exe") == 0 || wcscmp(fileName, L"codium") == 0
                   || wcscmp(fileName, L"cursor.exe") == 0 || wcscmp(fileName, L"cursor") == 0);
        }
        CloseHandle(hProcess);
        return isElectron;
    }

    static bool needsClipboardFallback(HWND hwnd) {
        return isBrowserWindow(hwnd) || isElectronAppWindow(hwnd);
    }

    std::mutex                            cdpCacheMutex_;
    int                                   cdpCachedPort_ = 0;
    std::chrono::steady_clock::time_point cdpCacheTime_{};
    std::mutex                            cdpFnsMutex_;
    CdpNetFns                             cdpFns_;

    /// 找一个在跑的 CDP 端口 (`/json/version` 有响应就算命中; 结果缓存 30 秒)
    int findCDPPort() {
        constexpr auto kCacheTtl = std::chrono::seconds(30);

        {
            std::lock_guard<std::mutex> lk(cdpCacheMutex_);
            if (cdpCachedPort_ != 0) {
                auto age = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - cdpCacheTime_
                );
                if (age < kCacheTtl) {
                    return (cdpCachedPort_ > 0) ? cdpCachedPort_ : 0;
                }
                cdpCachedPort_ = 0;
            }
        }

        const auto fns = cdpFnsCopy();
        if (!fns.ready()) {
            return 0;
        }

        for (int port : {9222, 9223, 9224, 9225, 9226, 9227, 9228, 9229, 9230}) {
            const std::string url
                = fmt::format("http://127.0.0.1:{}/json/version", port);
            if (fns.httpGet(url, 1500).empty()) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(cdpCacheMutex_);
                cdpCachedPort_ = port;
                cdpCacheTime_  = std::chrono::steady_clock::now();
            }
            return port;
        }

        {
            std::lock_guard<std::mutex> lk(cdpCacheMutex_);
            cdpCachedPort_ = -1;
            cdpCacheTime_  = std::chrono::steady_clock::now();
        }
        return 0;
    }

    static std::optional<std::string> extractCdpResult(std::string_view json) {
        try {
            auto view = utilxx_base::JsonView::parse(json);
            auto val  = view["result"]["result"]["value"];
            if (!val.is_string()) {
                return std::nullopt;
            }
            return std::string(val.get_string_view());
        } catch (...) {
            return std::nullopt;
        }
    }

    std::pair<std::string, TextSource> getSelectedTextByCDP(HWND hwnd) {
        const auto fns = cdpFnsCopy();
        if (!fns.ready()) {
            return {};
        }
        int port = findCDPPort();
        if (port <= 0) {
            return {};
        }

        wchar_t windowTitle[512] = {};
        GetWindowTextW(hwnd, windowTitle, 512);
        std::wstring wTitle(windowTitle);

        std::string targetsJson
            = fns.httpGet(fmt::format("http://127.0.0.1:{}/json", port), 3000);
        if (targetsJson.empty()) {
            return {};
        }

        std::string targetId;
        std::string targetTitle;

        {
            // CDP /json 目标列表: 经 JsonView 零拷贝提取 id/title
            // (历史 simdjson::ondemand 局部解析已统一为新体系)
            try {
                auto targets = utilxx_base::JsonView::parse(targetsJson);
                if (!targets.is_array()) {
                    return {};
                }
                for (size_t i = 0, n = targets.size(); i < n; ++i) {
                    auto elem  = targets[i];
                    auto id    = elem.value<std::string_view>("id", std::string_view{});
                    auto title = elem.value<std::string_view>("title", std::string_view{});
                    if (id.empty() || title.empty()) {
                        continue;
                    }

                    int titleLen = MultiByteToWideChar(
                        CP_UTF8,
                        0,
                        title.data(),
                        static_cast<int>(title.size()),
                        nullptr,
                        0
                    );
                    if (titleLen > 0) {
                        std::vector<wchar_t> wTitleBuf(titleLen);
                        MultiByteToWideChar(
                            CP_UTF8,
                            0,
                            title.data(),
                            static_cast<int>(title.size()),
                            wTitleBuf.data(),
                            titleLen
                        );
                        if (wTitle.find(wTitleBuf.data()) != std::wstring::npos) {
                            targetId    = std::string(id);
                            targetTitle = std::string(title);
                            break;
                        }
                    }

                    if (targetId.empty() && title.find("about:blank") == std::string_view::npos) {
                        targetId    = std::string(id);
                        targetTitle = std::string(title);
                    }
                }
            } catch (...) {
                return {};
            }
        }

        if (targetId.empty()) {
            return {};
        }

        // 一次 CDP 往返: 连到该页面的 DevTools WebSocket, 发一条 Runtime.evaluate,
        // 等回包。连接、帧、重连都归主程序的网络服务 (插件不自己建连接)。
        const std::string wsUrl
            = fmt::format("ws://127.0.0.1:{}/devtools/page/{}", port, targetId);
        const char* cdpCmd =
            R"_({"id":1,"method":"Runtime.evaluate","params":{"expression":"window.getSelection().toString()",
       "returnByValue" : true
  }})_";
        std::string response = fns.wsRoundTrip(wsUrl, cdpCmd, 5000);
        if (response.empty()) {
            return {};
        }

        auto value = extractCdpResult(response);
        if (!value || value->empty()) {
            return {};
        }

        return std::pair<std::string, TextSource>{*value, TextSource::DevTools};
    }

    std::pair<std::string, TextSource> getSelectedTextByClipboard(HWND hwnd) {
        HWND rootHwnd = GetAncestor(hwnd, GA_ROOT);
        if (!rootHwnd) {
            rootHwnd = hwnd;
        }

        HWND foregroundHwnd   = GetForegroundWindow();
        bool needRestoreFocus = (foregroundHwnd != rootHwnd && foregroundHwnd != nullptr);

        DWORD targetThreadId  = 0;
        DWORD currentThreadId = GetCurrentThreadId();
        if (needRestoreFocus) {
            targetThreadId = GetWindowThreadProcessId(rootHwnd, nullptr);
            AttachThreadInput(currentThreadId, targetThreadId, TRUE);
            SetForegroundWindow(rootHwnd);
            delayMs(50);
        }

        if (!OpenClipboard(nullptr)) {
            if (needRestoreFocus) {
                AttachThreadInput(currentThreadId, targetThreadId, FALSE);
                SetForegroundWindow(foregroundHwnd);
            }
            return {};
        }

        std::string savedText;
        {
            HANDLE hData = GetClipboardData(CF_UNICODETEXT);
            if (hData) {
                wchar_t* pwsz = static_cast<wchar_t*>(GlobalLock(hData));
                if (pwsz) {
                    int utf8Len
                        = WideCharToMultiByte(CP_UTF8, 0, pwsz, -1, nullptr, 0, nullptr, nullptr);
                    if (utf8Len > 1) {
                        savedText.resize(utf8Len - 1);
                        WideCharToMultiByte(
                            CP_UTF8,
                            0,
                            pwsz,
                            -1,
                            &savedText[0],
                            utf8Len,
                            nullptr,
                            nullptr
                        );
                    }
                    GlobalUnlock(hData);
                }
            }
        }
        EmptyClipboard();
        CloseClipboard();

        keybd_event(VK_CONTROL, 0, 0, 0);
        keybd_event('C', 0, 0, 0);
        keybd_event('C', 0, KEYEVENTF_KEYUP, 0);
        keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
        delayMs(30);

        std::string result;
        if (OpenClipboard(nullptr)) {
            HANDLE hData = GetClipboardData(CF_UNICODETEXT);
            if (hData) {
                wchar_t* pwsz = static_cast<wchar_t*>(GlobalLock(hData));
                if (pwsz) {
                    int utf8Len
                        = WideCharToMultiByte(CP_UTF8, 0, pwsz, -1, nullptr, 0, nullptr, nullptr);
                    if (utf8Len > 1) {
                        result.resize(utf8Len - 1);
                        WideCharToMultiByte(
                            CP_UTF8,
                            0,
                            pwsz,
                            -1,
                            &result[0],
                            utf8Len,
                            nullptr,
                            nullptr
                        );
                    }
                    GlobalUnlock(hData);
                }
            }

            if (!savedText.empty()) {
                size_t wLen = MultiByteToWideChar(CP_UTF8, 0, savedText.c_str(), -1, nullptr, 0);
                if (wLen > 0) {
                    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wLen * sizeof(wchar_t));
                    if (hMem) {
                        wchar_t* pwszDst = static_cast<wchar_t*>(GlobalLock(hMem));
                        if (pwszDst) {
                            MultiByteToWideChar(
                                CP_UTF8,
                                0,
                                savedText.c_str(),
                                -1,
                                pwszDst,
                                static_cast<int>(wLen)
                            );
                            GlobalUnlock(hMem);
                        }
                        SetClipboardData(CF_UNICODETEXT, hMem);
                    }
                }
            }
            CloseClipboard();
        }

        if (needRestoreFocus) {
            AttachThreadInput(currentThreadId, targetThreadId, FALSE);
            SetForegroundWindow(foregroundHwnd);
        }

        if (result.empty()) {
            return {};
        }

        return {result, TextSource::Clipboard};
    }

    std::pair<std::string, TextSource> getSelectedText(HWND hwnd) {
        auto legacyResult = getSelectedTextByEmGetSel(hwnd);
        if (!legacyResult.first.empty()) {
            return legacyResult;
        }

        if (needsClipboardFallback(hwnd)) {
            auto cdpResult = getSelectedTextByCDP(hwnd);
            if (!cdpResult.first.empty()) {
                return cdpResult;
            }
        }

        {
            IUIAutomation* localAutomation = nullptr;
            HRESULT        hr              = CoCreateInstance(
                CLSID_CUIAutomation,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_IUIAutomation,
                reinterpret_cast<void**>(&localAutomation)
            );
            if (FAILED(hr) || !localAutomation) {
                return {};
            }

            IUIAutomationElement* element = nullptr;
            hr                            = localAutomation->ElementFromHandle(hwnd, &element);
            if (FAILED(hr) || !element) {
                localAutomation->Release();
                return {};
            }

            IUIAutomationTextPattern* textPattern = nullptr;
            TextSource                source      = TextSource::TextPattern;
            hr                                    = element->GetCurrentPatternAs(
                UIA_TextPatternId,
                IID_IUIAutomationTextPattern,
                reinterpret_cast<void**>(&textPattern)
            );
            if (FAILED(hr) || !textPattern) {
                IUIAutomationElement* focused = nullptr;
                hr                            = localAutomation->GetFocusedElement(&focused);
                if (SUCCEEDED(hr) && focused) {
                    hr = focused->GetCurrentPatternAs(
                        UIA_TextPatternId,
                        IID_IUIAutomationTextPattern,
                        reinterpret_cast<void**>(&textPattern)
                    );
                    focused->Release();
                }
            }

            if (!textPattern) {
                IUIAutomationTextChildPattern* textChildPattern = nullptr;
                hr                                              = element->GetCurrentPatternAs(
                    UIA_TextChildPatternId,
                    IID_IUIAutomationTextChildPattern,
                    reinterpret_cast<void**>(&textChildPattern)
                );
                if (SUCCEEDED(hr) && textChildPattern) {
                    IUIAutomationElement* container = nullptr;
                    hr = textChildPattern->get_TextContainer(&container);
                    if (SUCCEEDED(hr) && container) {
                        hr = container->GetCurrentPatternAs(
                            UIA_TextPatternId,
                            IID_IUIAutomationTextPattern,
                            reinterpret_cast<void**>(&textPattern)
                        );
                        container->Release();
                    }
                    textChildPattern->Release();
                    if (textPattern) {
                        source = TextSource::TextChildPattern;
                    }
                }
            }

            element->Release();

            if (textPattern) {
                std::string result;

                IUIAutomationTextRangeArray* selectionRanges = nullptr;
                hr = textPattern->GetSelection(&selectionRanges);
                if (SUCCEEDED(hr) && selectionRanges) {
                    int count = 0;
                    selectionRanges->get_Length(&count);

                    for (int i = 0; i < count; ++i) {
                        IUIAutomationTextRange* range = nullptr;
                        hr                            = selectionRanges->GetElement(i, &range);
                        if (SUCCEEDED(hr) && range) {
                            BSTR text = nullptr;
                            hr        = range->GetText(-1, &text);
                            if (SUCCEEDED(hr) && text) {
                                result = bstrToUtf8(text);
                                SysFreeString(text);
                            }
                            range->Release();
                        }
                    }
                    selectionRanges->Release();
                    if (false == result.empty()) {
                        textPattern->Release();
                        localAutomation->Release();
                        return {result, source};
                    }
                }

                textPattern->Release();
            }

            {
                IUIAutomationElement* elemForValue = nullptr;
                hr = localAutomation->ElementFromHandle(hwnd, &elemForValue);
                if (SUCCEEDED(hr) && elemForValue) {
                    IUIAutomationValuePattern* valuePattern = nullptr;
                    hr                                      = elemForValue->GetCurrentPatternAs(
                        UIA_ValuePatternId,
                        IID_IUIAutomationValuePattern,
                        reinterpret_cast<void**>(&valuePattern)
                    );
                    if (SUCCEEDED(hr) && valuePattern) {
                        BSTR value = nullptr;
                        hr         = valuePattern->get_CurrentValue(&value);
                        if (SUCCEEDED(hr) && value) {
                            std::string result = bstrToUtf8(value);
                            SysFreeString(value);
                            valuePattern->Release();
                            elemForValue->Release();
                            localAutomation->Release();
                            return {result, TextSource::ValuePattern};
                        }
                        if (value) {
                            SysFreeString(value);
                        }
                        valuePattern->Release();
                    }
                    elemForValue->Release();
                }
                localAutomation->Release();
            }
        }

        legacyResult = getSelectedTextByAccessible(hwnd);
        if (!legacyResult.first.empty()) {
            return legacyResult;
        }

        {
            auto clipboardResult = getSelectedTextByClipboard(hwnd);
            if (!clipboardResult.first.empty()) {
                return clipboardResult;
            }
        }

        return getSelectedTextByWmGetText(hwnd);
    }

    void processPendingSelection() {
        bool expected = true;
        if (!selectionPending_.compare_exchange_strong(
                expected,
                false,
                std::memory_order_acquire,
                std::memory_order_relaxed
            )) {
            return;
        }

        std::string uiaText;
        HWND        hwnd = nullptr;
        {
            std::lock_guard<std::mutex> lock(debounceMutex_);
            hwnd = lastSelectionHwnd_;
            if (!pendingUiaText_.empty()) {
                uiaText = std::move(pendingUiaText_);
                pendingUiaText_.clear();
            }
        }

        if (!uiaText.empty()) {
            if (processingThread_.joinable()) {
                processingThread_.join();
            }
            processingThread_ = std::thread([this, text = std::move(uiaText)]() {
                TextSelectionEvent evt;
                evt.text      = std::move(text);
                evt.timestamp = std::chrono::steady_clock::now();
                evt.source    = TextSource::FlutterAccessibility;
                notifyListeners(evt);
            });
            return;
        }

        if (!hwnd) {
            return;
        }

        if (processingThread_.joinable()) {
            processingThread_.join();
        }
        processingThread_ = std::thread([this, hwnd]() {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);

            auto [selectedText, source] = getSelectedText(hwnd);
            if (selectedText.empty()) {
                CoUninitialize();
                return;
            }

            TextSelectionEvent evt;
            evt.text      = std::move(selectedText);
            evt.timestamp = std::chrono::steady_clock::now();
            evt.source    = source;

            notifyListeners(evt);
            CoUninitialize();
        });
    }

    void checkDebounce() {
        if (!selectionPending_.load(std::memory_order_acquire)) {
            return;
        }

        std::chrono::steady_clock::time_point lastTime;
        {
            std::lock_guard<std::mutex> lock(debounceMutex_);
            lastTime = lastSelectionChangeTime_;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - lastTime
        );
        if (elapsed.count() >= debounceMs_) {
            processPendingSelection();
        }
    }

    void notifyListeners(const TextSelectionEvent& event) {
        std::lock_guard<std::mutex> lock(listenersMutex_);
        for (auto& listener : listeners_) {
            if (listener) {
                listener(event);
            }
        }
    }

    static Impl*& instancePtr() {
        static Impl* ptr = nullptr;
        return ptr;
    }

    void delayMs(int ms) {
        // 同步睡眠 (自有 worker 线程内调用, 阻塞自身线程无碍);
        // 原实现借 asio steady_timer::wait() 当 sleep 用 —— 语义等价但误导
        // (ioContext_ 从未 run(), timer.wait() 本就是同步阻塞), 改为直白实现
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    std::atomic<bool>                     running_;
    bool                                  com_initialized_ = false;
    std::thread                           workerThread_;
    std::thread                           processingThread_;
    HWINEVENTHOOK                         hook_       = nullptr;
    HHOOK                                 mouseHook_  = nullptr;
    IUIAutomation*                        automation_ = nullptr;
    int                                   debounceMs_;
    std::chrono::steady_clock::time_point lastSelectionChangeTime_;
    std::atomic<bool>                     selectionPending_;
    HWND                                  lastSelectionHwnd_ = nullptr;
    POINT                                 mouseDownPos_      = {};
    std::chrono::steady_clock::time_point lastClickTime_     = {};
    POINT                                 lastClickPos_      = {};
    std::mutex                            debounceMutex_;
    std::mutex                            listenersMutex_;
    std::vector<TextSelectionListener>    listeners_;
    volatile LONG                         refCount_;
    std::string                           pendingUiaText_;
};

#else

class TextSelectionMonitor::Impl {
public:

    Impl()  = default;
    ~Impl() = default;

    void addListener(TextSelectionListener /*listener*/) {}

    void removeAllListeners() {}

    void setDebounceMs(int /*ms*/) {}

    void setCdpNetFns(CdpNetFns /*fns*/) {}

    bool start() {
        XX_LOGW("TextSelectionMonitor: not supported on this platform");
        return false;
    }

    void stop() {}

    bool isRunning() const {
        return false;
    }
};

#endif

TextSelectionMonitor::TextSelectionMonitor() :
    impl_(std::make_unique<Impl>()) {}

TextSelectionMonitor::~TextSelectionMonitor() = default;

void TextSelectionMonitor::addListener(TextSelectionListener listener) {
    impl_->addListener(std::move(listener));
}

void TextSelectionMonitor::removeAllListeners() {
    impl_->removeAllListeners();
}

void TextSelectionMonitor::setDebounceMs(int ms) {
    impl_->setDebounceMs(ms);
}

void TextSelectionMonitor::setCdpNetFns(CdpNetFns fns) {
    impl_->setCdpNetFns(std::move(fns));
}

bool TextSelectionMonitor::start() {
    return impl_->start();
}

void TextSelectionMonitor::stop() {
    impl_->stop();
}

bool TextSelectionMonitor::isRunning() const {
    return impl_->isRunning();
}

} // namespace agentxx_text_selection_monitor_plugin