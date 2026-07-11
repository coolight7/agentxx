#include "agentxx/expand/text_selection_monitor.h"
#include "agentxx/util/log.h"
#include <mutex>
#include <thread>

#if XX_IS_WIN_D
// include 顺序是必要的
#include <winsock2.h>
// ---
#include <Ws2tcpip.h>
// ---
#include <UIAutomation.h>
#include <oleacc.h>
#include <windows.h>
#include <winhttp.h>

// ---
#include "simdjson.h"
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <optional>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

namespace asio = boost::asio;

#endif

namespace agentxx {
namespace expand {

#if XX_IS_WIN_D

/// 系统级 文本选择事件流
/// - 当选择 程序窗口、浏览器 中的文本时，收到事件，分析文本内容并发起通知
/// - 启动独立线程处理接收事件
class TextSelectionMonitor::Impl : public IUIAutomationEventHandler {
public:
  static constexpr UINT WM_SELECTION_COMPLETE = WM_APP + 1;

  Impl()
      : running_(false), debounceMs_(300), lastSelectionChangeTime_({}),
        selectionPending_(false), lastSelectionHwnd_(nullptr), refCount_(1) {}

  ~Impl() { stop(); }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (riid == IID_IUnknown || riid == IID_IUIAutomationEventHandler) {
      *ppv = static_cast<IUIAutomationEventHandler *>(this);
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

  void setDebounceMs(int ms) { debounceMs_ = ms; }

  bool start() {
    if (running_.load()) {
      return true;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                             COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
      XX_LOGE("TextSelectionMonitor: CoInitializeEx failed, hr=0x{:08X}",
              static_cast<unsigned>(hr));
      return false;
    }

    com_initialized_ = (SUCCEEDED(hr));

    hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IUIAutomation,
                          reinterpret_cast<void **>(&automation_));
    if (FAILED(hr) || !automation_) {
      XX_LOGE("TextSelectionMonitor: CoCreateInstance CUIAutomation failed, "
              "hr=0x{:08X}",
              static_cast<unsigned>(hr));
      if (com_initialized_) {
        CoUninitialize();
        com_initialized_ = false;
      }
      return false;
    }

    instancePtr() = this;
    running_.store(true);

    IUIAutomationElement *desktop = nullptr;
    hr = automation_->GetRootElement(&desktop);
    if (SUCCEEDED(hr) && desktop) {
      automation_->AddAutomationEventHandler(
          UIA_Text_TextSelectionChangedEventId, desktop, TreeScope_Descendants,
          nullptr, static_cast<IUIAutomationEventHandler *>(this));
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
      PostThreadMessageW(GetThreadId(workerThread_.native_handle()), WM_QUIT, 0,
                         0);
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

  bool isRunning() const { return running_.load(); }

private:
  void workerLoop() {
    hook_ = SetWinEventHook(EVENT_OBJECT_TEXTSELECTIONCHANGED,
                            EVENT_OBJECT_TEXTSELECTIONCHANGED, nullptr,
                            WinEventProc, 0, 0,
                            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

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

      if (running_.load() &&
          MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT) ==
              WAIT_TIMEOUT) {
        continue;
      }
    }
  }

  static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
      Impl *self = instancePtr();
      if (!self)
        return CallNextHookEx(nullptr, nCode, wParam, lParam);

      MSLLHOOKSTRUCT *pMouseStruct = (MSLLHOOKSTRUCT *)lParam;

      switch (wParam) {
      case WM_LBUTTONDOWN:
        self->mouseDownPos_ = pMouseStruct->pt;
        break;

      case WM_LBUTTONUP: {
        if (self->selectionPending_.load(std::memory_order_acquire)) {
          PostThreadMessageW(GetCurrentThreadId(), WM_SELECTION_COMPLETE, 0, 0);
        } else {
          int dx = abs(pMouseStruct->pt.x - self->mouseDownPos_.x);
          int dy = abs(pMouseStruct->pt.y - self->mouseDownPos_.y);
          bool wasDrag = (dx > 3 || dy > 3);

          auto now = std::chrono::steady_clock::now();
          auto clickElapsed =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  now - self->lastClickTime_);
          int cdx = abs(pMouseStruct->pt.x - self->lastClickPos_.x);
          int cdy = abs(pMouseStruct->pt.y - self->lastClickPos_.y);
          bool wasDoubleClick = (clickElapsed.count() < GetDoubleClickTime() &&
                                 cdx < 5 && cdy < 5);

          if (wasDrag || wasDoubleClick) {
            HWND hwnd = WindowFromPoint(pMouseStruct->pt);
            if (hwnd) {
              HWND rootHwnd = GetAncestor(hwnd, GA_ROOT);
              std::lock_guard<std::mutex> lock(self->debounceMutex_);
              self->lastSelectionHwnd_ = rootHwnd;
              self->lastSelectionChangeTime_ = now;
              self->selectionPending_.store(true, std::memory_order_release);
              PostThreadMessageW(GetCurrentThreadId(), WM_SELECTION_COMPLETE, 0,
                                 0);
            }
          }
          self->lastClickTime_ = now;
          self->lastClickPos_ = pMouseStruct->pt;
        }
        break;
      }
      }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
  }

  static void CALLBACK WinEventProc(HWINEVENTHOOK /*hWinEventHook*/,
                                    DWORD event, HWND hwnd, LONG idObject,
                                    LONG /*idChild*/, DWORD /*dwEventThread*/,
                                    DWORD /*dwmsEventTime*/) {
    if (event != EVENT_OBJECT_TEXTSELECTIONCHANGED) {
      return;
    }

    if (idObject != OBJID_CLIENT && idObject != OBJID_WINDOW) {
      return;
    }

    Impl *self = instancePtr();
    if (!self || !self->running_.load()) {
      return;
    }

    auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(self->debounceMutex_);
      self->lastSelectionHwnd_ = hwnd;
      self->lastSelectionChangeTime_ = now;
    }
    self->selectionPending_.store(true, std::memory_order_release);
  }

  HRESULT STDMETHODCALLTYPE HandleAutomationEvent(IUIAutomationElement *sender,
                                                  EVENTID eventId) override {
    if (eventId != UIA_Text_TextSelectionChangedEventId) {
      return S_OK;
    }
    if (!running_.load()) {
      return S_OK;
    }

    IUIAutomationTextPattern *textPattern = nullptr;
    HRESULT hr = sender->GetCurrentPatternAs(
        UIA_TextPatternId, IID_IUIAutomationTextPattern,
        reinterpret_cast<void **>(&textPattern));

    std::string extractedText;
    if (SUCCEEDED(hr) && textPattern) {
      IUIAutomationTextRangeArray *selectionRanges = nullptr;
      hr = textPattern->GetSelection(&selectionRanges);
      if (SUCCEEDED(hr) && selectionRanges) {
        int count = 0;
        selectionRanges->get_Length(&count);
        for (int i = 0; i < count; ++i) {
          IUIAutomationTextRange *range = nullptr;
          hr = selectionRanges->GetElement(i, &range);
          if (SUCCEEDED(hr) && range) {
            BSTR text = nullptr;
            hr = range->GetText(-1, &text);
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
      pendingUiaText_ = std::move(extractedText);
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
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, bstr, len, nullptr, 0,
                                      nullptr, nullptr);
    if (utf8Len <= 0) {
      return {};
    }
    std::string result;
    result.resize(utf8Len);
    WideCharToMultiByte(CP_UTF8, 0, bstr, len, &result[0], utf8Len, nullptr,
                        nullptr);
    return result;
  }

  std::pair<std::string, TextSource> getSelectedTextByEmGetSel(HWND hwnd) {
    HWND targetHwnd = hwnd;
    wchar_t className[64] = {};
    GetClassNameW(targetHwnd, className, 64);
    bool isEdit = (wcscmp(className, L"Edit") == 0 ||
                   wcsstr(className, L"RichEdit") != nullptr);

    if (!isEdit) {
      targetHwnd = FindWindowExW(hwnd, nullptr, L"Edit", nullptr);
      if (!targetHwnd) {
        return {};
      }
    }

    DWORD_PTR msgResult = 0;
    DWORD selStart = 0, selEnd = 0;
    LRESULT lr = SendMessageTimeoutW(
        targetHwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&selStart),
        reinterpret_cast<LPARAM>(&selEnd), SMTO_ABORTIFHUNG, 200, &msgResult);
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

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, selText.data(),
                                      static_cast<int>(selText.size()), nullptr,
                                      0, nullptr, nullptr);
    if (utf8Len <= 0) {
      return {};
    }

    std::string result;
    result.resize(utf8Len);
    WideCharToMultiByte(CP_UTF8, 0, selText.data(),
                        static_cast<int>(selText.size()), &result[0], utf8Len,
                        nullptr, nullptr);

    return {result, TextSource::EmGetSel};
  }

  std::pair<std::string, TextSource> getSelectedTextByAccessible(HWND hwnd) {
    IAccessible *acc = nullptr;
    HRESULT hr = AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, IID_IAccessible,
                                            reinterpret_cast<void **>(&acc));
    if (FAILED(hr) || !acc) {
      hr = AccessibleObjectFromWindow(hwnd, OBJID_WINDOW, IID_IAccessible,
                                      reinterpret_cast<void **>(&acc));
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
      hr = acc->get_accValue(varSel, &value);
      if (SUCCEEDED(hr) && value) {
        std::string text = bstrToUtf8(value);
        if (!text.empty()) {
          ret = {text, TextSource::AccessibleObject};
        }
        SysFreeString(value);
      }
    } else if (SUCCEEDED(hr) && varSel.vt == VT_DISPATCH && varSel.pdispVal) {
      IEnumVARIANT *enumVar = nullptr;
      hr = varSel.pdispVal->QueryInterface(IID_IEnumVARIANT,
                                           reinterpret_cast<void **>(&enumVar));
      if (SUCCEEDED(hr) && enumVar) {
        VARIANT childVar;
        VariantInit(&childVar);
        ULONG fetched = 0;
        std::string combined;
        while (enumVar->Next(1, &childVar, &fetched) == S_OK && fetched == 1) {
          if (childVar.vt == VT_DISPATCH && childVar.pdispVal) {
            IAccessible *childAcc = nullptr;
            hr = childVar.pdispVal->QueryInterface(
                IID_IAccessible, reinterpret_cast<void **>(&childAcc));
            if (SUCCEEDED(hr) && childAcc) {
              VARIANT varChildId;
              varChildId.vt = VT_I4;
              varChildId.lVal = CHILDID_SELF;
              BSTR name = nullptr;
              hr = childAcc->get_accValue(varChildId, &name);
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

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                      static_cast<int>(text.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (utf8Len <= 0) {
      return {};
    }

    std::string result;
    result.resize(utf8Len);
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        &result[0], utf8Len, nullptr, nullptr);

    return {result, TextSource::WmGetText};
  }

  static bool isBrowserWindow(HWND hwnd) {
    wchar_t className[64] = {};
    GetClassNameW(hwnd, className, 64);
    if (wcscmp(className, L"Chrome_WidgetWin_1") == 0 ||
        wcscmp(className, L"Chrome_RenderWidgetHostHWND") == 0 ||
        wcscmp(className, L"MozillaWindowClass") == 0) {
      return true;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0) {
      return false;
    }

    HANDLE hProcess =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!hProcess) {
      return false;
    }

    wchar_t exePath[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    bool isBrowser = false;
    if (QueryFullProcessImageNameW(hProcess, 0, exePath, &size)) {
      wchar_t *fileName = wcsrchr(exePath, L'\\');
      if (!fileName) {
        fileName = exePath;
      } else {
        ++fileName;
      }
      _wcslwr_s(fileName, wcslen(fileName) + 1);
      static constexpr auto browserKeywords = std::array<const wchar_t *, 19>{
          L"chrome",       L"chromium",     L"firefox",  L"msedge",
          L"edge",         L"opera",        L"brave",    L"vivaldi",
          L"arc",          L"360chrome",    L"360se",    L"sogouexplorer",
          L"qqbrowser",    L"maxthon",      L"liebao",   L"browser",
          L"2345explorer", L"baidubrowser", L"ucbrowser"};
      for (const auto &kw : browserKeywords) {
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

    HANDLE hProcess =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!hProcess) {
      return false;
    }

    wchar_t exePath[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    bool isElectron = false;
    if (QueryFullProcessImageNameW(hProcess, 0, exePath, &size)) {
      wchar_t *fileName = wcsrchr(exePath, L'\\');
      if (!fileName) {
        fileName = exePath;
      } else {
        ++fileName;
      }
      _wcslwr_s(fileName, wcslen(fileName) + 1);
      isElectron = (wcscmp(fileName, L"code.exe") == 0 ||
                    wcscmp(fileName, L"code") == 0 ||
                    wcscmp(fileName, L"code-insiders.exe") == 0 ||
                    wcscmp(fileName, L"code-insiders") == 0 ||
                    wcscmp(fileName, L"codium.exe") == 0 ||
                    wcscmp(fileName, L"codium") == 0 ||
                    wcscmp(fileName, L"cursor.exe") == 0 ||
                    wcscmp(fileName, L"cursor") == 0);
    }
    CloseHandle(hProcess);
    return isElectron;
  }

  static bool needsClipboardFallback(HWND hwnd) {
    return isBrowserWindow(hwnd) || isElectronAppWindow(hwnd);
  }

  static bool ensureWSA() {
    static bool initialized = false;
    if (!initialized) {
      WSADATA wsaData;
      initialized = (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);
    }
    return initialized;
  }

  static bool tcpConnectWithTimeout(SOCKET sock, sockaddr_in &addr,
                                    int timeoutMs) {
    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);

    int result =
        connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (result == SOCKET_ERROR) {
      int err = WSAGetLastError();
      if (err != WSAEWOULDBLOCK) {
        u_long blocking = 0;
        ioctlsocket(sock, FIONBIO, &blocking);
        return false;
      }

      fd_set writeSet;
      FD_ZERO(&writeSet);
      FD_SET(sock, &writeSet);
      timeval tv = {};
      tv.tv_sec = timeoutMs / 1000;
      tv.tv_usec = (timeoutMs % 1000) * 1000;

      result = select(0, nullptr, &writeSet, nullptr, &tv);
      if (result <= 0) {
        u_long blocking = 0;
        ioctlsocket(sock, FIONBIO, &blocking);
        return false;
      }
    }

    u_long blocking = 0;
    ioctlsocket(sock, FIONBIO, &blocking);
    return true;
  }

  static int findCDPPort() {
    static int cachedPort = 0;
    static std::chrono::steady_clock::time_point cacheTime = {};
    constexpr auto kCacheTtl = std::chrono::seconds(30);

    if (cachedPort != 0) {
      auto age = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - cacheTime);
      if (age < kCacheTtl) {
        return (cachedPort > 0) ? cachedPort : 0;
      }
      cachedPort = 0;
    }

    for (int port : {9222, 9223, 9224, 9225, 9226, 9227, 9228, 9229, 9230}) {
      SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (sock == INVALID_SOCKET) {
        continue;
      }

      sockaddr_in addr = {};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(static_cast<u_short>(port));
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

      if (tcpConnectWithTimeout(sock, addr, 200)) {
        closesocket(sock);

        HINTERNET hSession =
            WinHttpOpen(L"AgentXX/CDP", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) {
          continue;
        }

        std::wstring server = L"127.0.0.1";
        HINTERNET hConnect = WinHttpConnect(
            hSession, server.c_str(), static_cast<INTERNET_PORT>(port), 0);
        if (hConnect) {
          HINTERNET hRequest = WinHttpOpenRequest(
              hConnect, L"GET", L"/json/version", nullptr, WINHTTP_NO_REFERER,
              WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
          if (hRequest) {
            if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hRequest, nullptr)) {
              cachedPort = port;
              cacheTime = std::chrono::steady_clock::now();
              WinHttpCloseHandle(hRequest);
              WinHttpCloseHandle(hConnect);
              WinHttpCloseHandle(hSession);
              return port;
            }
            WinHttpCloseHandle(hRequest);
          }
          WinHttpCloseHandle(hConnect);
        }
        WinHttpCloseHandle(hSession);
      } else {
        closesocket(sock);
      }
    }

    cachedPort = -1;
    cacheTime = std::chrono::steady_clock::now();
    return 0;
  }

  static std::string httpGet(int port, const std::wstring &path) {
    HINTERNET hSession =
        WinHttpOpen(L"AgentXX/CDP", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
      return {};
    }

    HINTERNET hConnect = WinHttpConnect(hSession, L"127.0.0.1",
                                        static_cast<INTERNET_PORT>(port), 0);
    if (!hConnect) {
      WinHttpCloseHandle(hSession);
      return {};
    }

    HINTERNET hRequest =
        WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
      WinHttpCloseHandle(hConnect);
      WinHttpCloseHandle(hSession);
      return {};
    }

    std::string result;
    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {
      DWORD bytesRead = 0;
      char buffer[4096];
      while (WinHttpReadData(hRequest, buffer, sizeof(buffer), &bytesRead) &&
             bytesRead > 0) {
        result.append(buffer, bytesRead);
      }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
  }

  static SOCKET wsConnect(int port, const std::string &wsPath) {
    if (!ensureWSA()) {
      return INVALID_SOCKET;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
      return INVALID_SOCKET;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (!tcpConnectWithTimeout(sock, addr, 3000)) {
      closesocket(sock);
      return INVALID_SOCKET;
    }

    std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    std::string upgradeReq = "GET " + wsPath +
                             " HTTP/1.1\r\n"
                             "Host: 127.0.0.1:" +
                             std::to_string(port) +
                             "\r\n"
                             "Upgrade: websocket\r\n"
                             "Connection: Upgrade\r\n"
                             "Sec-WebSocket-Key: " +
                             key +
                             "\r\n"
                             "Sec-WebSocket-Version: 13\r\n"
                             "\r\n";

    if (send(sock, upgradeReq.c_str(), static_cast<int>(upgradeReq.size()),
             0) <= 0) {
      closesocket(sock);
      return INVALID_SOCKET;
    }

    wsSetRecvTimeout(sock, 3000);
    char recvBuf[4096] = {};
    int recvLen = recv(sock, recvBuf, sizeof(recvBuf) - 1, 0);
    if (recvLen <= 0) {
      closesocket(sock);
      return INVALID_SOCKET;
    }
    recvBuf[recvLen] = '\0';

    if (!strstr(recvBuf, "101")) {
      closesocket(sock);
      return INVALID_SOCKET;
    }

    return sock;
  }

  static bool wsSetRecvTimeout(SOCKET sock, int timeoutMs) {
    DWORD tv = static_cast<DWORD>(timeoutMs);
    return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                      reinterpret_cast<const char *>(&tv), sizeof(tv)) == 0;
  }

  static bool wsSend(SOCKET sock, std::string_view message) {
    std::vector<unsigned char> frame;
    frame.push_back(0x81);
    size_t len = message.size();
    if (len <= 125) {
      frame.push_back(static_cast<unsigned char>(0x80 | len));
    } else if (len <= 65535) {
      frame.push_back(0xFE);
      frame.push_back(static_cast<unsigned char>((len >> 8) & 0xFF));
      frame.push_back(static_cast<unsigned char>(len & 0xFF));
    } else {
      frame.push_back(0xFF);
      for (int i = 7; i >= 0; --i) {
        frame.push_back(static_cast<unsigned char>((len >> (i * 8)) & 0xFF));
      }
    }

    unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
    frame.insert(frame.end(), mask, mask + 4);

    for (size_t i = 0; i < len; ++i) {
      frame.push_back(static_cast<unsigned char>(message[i]) ^ mask[i % 4]);
    }

    return send(sock, reinterpret_cast<const char *>(frame.data()),
                static_cast<int>(frame.size()), 0) > 0;
  }

  static std::string wsRecv(SOCKET sock) {
    wsSetRecvTimeout(sock, 5000);
    unsigned char header[2] = {};
    if (recv(sock, reinterpret_cast<char *>(header), 2, 0) != 2) {
      return {};
    }

    size_t payloadLen = header[1] & 0x7F;
    if (payloadLen == 126) {
      unsigned char ext[2] = {};
      if (recv(sock, reinterpret_cast<char *>(ext), 2, 0) != 2) {
        return {};
      }
      payloadLen = (static_cast<size_t>(ext[0]) << 8) | ext[1];
    } else if (payloadLen == 127) {
      unsigned char ext[8] = {};
      if (recv(sock, reinterpret_cast<char *>(ext), 8, 0) != 8) {
        return {};
      }
      payloadLen = 0;
      for (int i = 0; i < 8; ++i) {
        payloadLen = (payloadLen << 8) | ext[i];
      }
    }

    if (payloadLen == 0 || payloadLen > 16 * 1024 * 1024) {
      return {};
    }

    std::vector<char> payload(payloadLen);
    size_t totalRecv = 0;
    while (totalRecv < payloadLen) {
      int n = recv(sock, payload.data() + totalRecv,
                   static_cast<int>(payloadLen - totalRecv), 0);
      if (n <= 0) {
        return {};
      }
      totalRecv += n;
    }

    return std::string(payload.data(), payloadLen);
  }

  static void wsClose(SOCKET sock) {
    if (sock != INVALID_SOCKET) {
      closesocket(sock);
    }
  }

  static std::optional<std::string> extractCdpResult(std::string_view json) {
    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(json);
    auto doc = parser.iterate(padded);
    if (doc.error()) {
      return std::nullopt;
    }
    auto result = doc.find_field("result");
    if (result.error()) {
      return std::nullopt;
    }
    auto innerResult = result.find_field("result");
    if (innerResult.error()) {
      return std::nullopt;
    }
    auto val = innerResult.find_field("value");
    if (val.error()) {
      return std::nullopt;
    }
    std::string_view sv;
    auto err = val.get(sv);
    if (err) {
      return std::nullopt;
    }
    return std::string(sv);
  }

  std::pair<std::string, TextSource> getSelectedTextByCDP(HWND hwnd) {
    int port = findCDPPort();
    if (port <= 0) {
      return {};
    }

    wchar_t windowTitle[512] = {};
    GetWindowTextW(hwnd, windowTitle, 512);
    std::wstring wTitle(windowTitle);

    std::string targetsJson = httpGet(port, L"/json");
    if (targetsJson.empty()) {
      return {};
    }

    std::string targetId;
    std::string targetTitle;

    {
      simdjson::ondemand::parser parser;
      simdjson::padded_string padded(targetsJson);
      auto doc = parser.iterate(padded);
      if (!doc.error()) {
        for (auto elem : doc.get_array()) {
          if (elem.error()) {
            continue;
          }
          std::string_view id;
          std::string_view title;
          auto obj = elem.get_object();
          if (obj.error()) {
            continue;
          }
          auto idField = obj["id"];
          auto titleField = obj["title"];
          if (idField.error() || titleField.error()) {
            continue;
          }
          if (idField.get(id) || titleField.get(title)) {
            continue;
          }

          int titleLen =
              MultiByteToWideChar(CP_UTF8, 0, title.data(),
                                  static_cast<int>(title.size()), nullptr, 0);
          if (titleLen > 0) {
            std::vector<wchar_t> wTitleBuf(titleLen);
            MultiByteToWideChar(CP_UTF8, 0, title.data(),
                                static_cast<int>(title.size()),
                                wTitleBuf.data(), titleLen);
            if (wTitle.find(wTitleBuf.data()) != std::wstring::npos) {
              targetId = std::string(id);
              targetTitle = std::string(title);
              break;
            }
          }

          if (targetId.empty() &&
              title.find("about:blank") == std::string_view::npos) {
            targetId = std::string(id);
            targetTitle = std::string(title);
          }
        }
      }
    }

    if (targetId.empty()) {
      return {};
    }

    std::string wsPath = "/devtools/page/" + targetId;
    SOCKET sock = wsConnect(port, wsPath);
    if (sock == INVALID_SOCKET) {
      return {};
    }

    const char *cdpCmd =
        R"_({"id":1,"method":"Runtime.evaluate","params":{"expression":"window.getSelection().toString()",
       "returnByValue" : true
  }})_";
    if (!wsSend(sock, cdpCmd)) {
      wsClose(sock);
      return {};
    }

    std::string response = wsRecv(sock);
    wsClose(sock);

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

    HWND foregroundHwnd = GetForegroundWindow();
    bool needRestoreFocus =
        (foregroundHwnd != rootHwnd && foregroundHwnd != nullptr);

    DWORD targetThreadId = 0;
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
        wchar_t *pwsz = static_cast<wchar_t *>(GlobalLock(hData));
        if (pwsz) {
          int utf8Len = WideCharToMultiByte(CP_UTF8, 0, pwsz, -1, nullptr, 0,
                                            nullptr, nullptr);
          if (utf8Len > 1) {
            savedText.resize(utf8Len - 1);
            WideCharToMultiByte(CP_UTF8, 0, pwsz, -1, &savedText[0], utf8Len,
                                nullptr, nullptr);
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
        wchar_t *pwsz = static_cast<wchar_t *>(GlobalLock(hData));
        if (pwsz) {
          int utf8Len = WideCharToMultiByte(CP_UTF8, 0, pwsz, -1, nullptr, 0,
                                            nullptr, nullptr);
          if (utf8Len > 1) {
            result.resize(utf8Len - 1);
            WideCharToMultiByte(CP_UTF8, 0, pwsz, -1, &result[0], utf8Len,
                                nullptr, nullptr);
          }
          GlobalUnlock(hData);
        }
      }

      if (!savedText.empty()) {
        size_t wLen =
            MultiByteToWideChar(CP_UTF8, 0, savedText.c_str(), -1, nullptr, 0);
        if (wLen > 0) {
          HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wLen * sizeof(wchar_t));
          if (hMem) {
            wchar_t *pwszDst = static_cast<wchar_t *>(GlobalLock(hMem));
            if (pwszDst) {
              MultiByteToWideChar(CP_UTF8, 0, savedText.c_str(), -1, pwszDst,
                                  static_cast<int>(wLen));
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
      IUIAutomation *localAutomation = nullptr;
      HRESULT hr = CoCreateInstance(
          CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
          reinterpret_cast<void **>(&localAutomation));
      if (FAILED(hr) || !localAutomation) {
        return {};
      }

      IUIAutomationElement *element = nullptr;
      hr = localAutomation->ElementFromHandle(hwnd, &element);
      if (FAILED(hr) || !element) {
        localAutomation->Release();
        return {};
      }

      IUIAutomationTextPattern *textPattern = nullptr;
      TextSource source = TextSource::TextPattern;
      hr = element->GetCurrentPatternAs(
          UIA_TextPatternId, IID_IUIAutomationTextPattern,
          reinterpret_cast<void **>(&textPattern));
      if (FAILED(hr) || !textPattern) {
        IUIAutomationElement *focused = nullptr;
        hr = localAutomation->GetFocusedElement(&focused);
        if (SUCCEEDED(hr) && focused) {
          hr = focused->GetCurrentPatternAs(
              UIA_TextPatternId, IID_IUIAutomationTextPattern,
              reinterpret_cast<void **>(&textPattern));
          focused->Release();
        }
      }

      if (!textPattern) {
        IUIAutomationTextChildPattern *textChildPattern = nullptr;
        hr = element->GetCurrentPatternAs(
            UIA_TextChildPatternId, IID_IUIAutomationTextChildPattern,
            reinterpret_cast<void **>(&textChildPattern));
        if (SUCCEEDED(hr) && textChildPattern) {
          IUIAutomationElement *container = nullptr;
          hr = textChildPattern->get_TextContainer(&container);
          if (SUCCEEDED(hr) && container) {
            hr = container->GetCurrentPatternAs(
                UIA_TextPatternId, IID_IUIAutomationTextPattern,
                reinterpret_cast<void **>(&textPattern));
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

        IUIAutomationTextRangeArray *selectionRanges = nullptr;
        hr = textPattern->GetSelection(&selectionRanges);
        if (SUCCEEDED(hr) && selectionRanges) {
          int count = 0;
          selectionRanges->get_Length(&count);

          for (int i = 0; i < count; ++i) {
            IUIAutomationTextRange *range = nullptr;
            hr = selectionRanges->GetElement(i, &range);
            if (SUCCEEDED(hr) && range) {
              BSTR text = nullptr;
              hr = range->GetText(-1, &text);
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
        IUIAutomationElement *elemForValue = nullptr;
        hr = localAutomation->ElementFromHandle(hwnd, &elemForValue);
        if (SUCCEEDED(hr) && elemForValue) {
          IUIAutomationValuePattern *valuePattern = nullptr;
          hr = elemForValue->GetCurrentPatternAs(
              UIA_ValuePatternId, IID_IUIAutomationValuePattern,
              reinterpret_cast<void **>(&valuePattern));
          if (SUCCEEDED(hr) && valuePattern) {
            BSTR value = nullptr;
            hr = valuePattern->get_CurrentValue(&value);
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
    if (!selectionPending_.compare_exchange_strong(expected, false,
                                                   std::memory_order_acquire,
                                                   std::memory_order_relaxed)) {
      return;
    }

    std::string uiaText;
    HWND hwnd = nullptr;
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
        evt.text = std::move(text);
        evt.timestamp = std::chrono::steady_clock::now();
        evt.source = TextSource::FlutterAccessibility;
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
      evt.text = std::move(selectedText);
      evt.timestamp = std::chrono::steady_clock::now();
      evt.source = source;

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
        std::chrono::steady_clock::now() - lastTime);
    if (elapsed.count() >= debounceMs_) {
      processPendingSelection();
    }
  }

  void notifyListeners(const TextSelectionEvent &event) {
    std::lock_guard<std::mutex> lock(listenersMutex_);
    for (auto &listener : listeners_) {
      if (listener) {
        listener(event);
      }
    }
  }

  static Impl *&instancePtr() {
    static Impl *ptr = nullptr;
    return ptr;
  }

  void delayMs(int ms) {
    asio::steady_timer timer(ioContext_);
    timer.expires_after(std::chrono::milliseconds(ms));
    timer.wait();
  }

  std::atomic<bool> running_;
  bool com_initialized_ = false;
  std::thread workerThread_;
  std::thread processingThread_;
  HWINEVENTHOOK hook_ = nullptr;
  HHOOK mouseHook_ = nullptr;
  IUIAutomation *automation_ = nullptr;
  int debounceMs_;
  std::chrono::steady_clock::time_point lastSelectionChangeTime_;
  std::atomic<bool> selectionPending_;
  HWND lastSelectionHwnd_ = nullptr;
  POINT mouseDownPos_ = {};
  std::chrono::steady_clock::time_point lastClickTime_ = {};
  POINT lastClickPos_ = {};
  std::mutex debounceMutex_;
  std::mutex listenersMutex_;
  std::vector<TextSelectionListener> listeners_;
  volatile LONG refCount_;
  std::string pendingUiaText_;
  asio::io_context ioContext_;
};

#else

class TextSelectionMonitor::Impl {
public:
  Impl() = default;
  ~Impl() = default;

  void addListener(TextSelectionListener /*listener*/) {}
  void removeAllListeners() {}
  void setDebounceMs(int /*ms*/) {}

  bool start() {
    XX_LOGW("TextSelectionMonitor: not supported on this platform");
    return false;
  }

  void stop() {}
  bool isRunning() const { return false; }
};

#endif

TextSelectionMonitor::TextSelectionMonitor()
    : impl_(std::make_unique<Impl>()) {}

TextSelectionMonitor::~TextSelectionMonitor() = default;

void TextSelectionMonitor::addListener(TextSelectionListener listener) {
  impl_->addListener(std::move(listener));
}

void TextSelectionMonitor::removeAllListeners() { impl_->removeAllListeners(); }

void TextSelectionMonitor::setDebounceMs(int ms) { impl_->setDebounceMs(ms); }

bool TextSelectionMonitor::start() { return impl_->start(); }

void TextSelectionMonitor::stop() { impl_->stop(); }

bool TextSelectionMonitor::isRunning() const { return impl_->isRunning(); }

} // namespace expand
} // namespace agentxx