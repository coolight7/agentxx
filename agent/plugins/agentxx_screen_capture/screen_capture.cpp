// XX_IS_WIN_D 等平台宏由 screen_capture_plugin.h 推导, 须最先引入
// (否则下方 #if XX_IS_WIN_D 分支在 windows.h 之前即判定失效)
#include "screen_capture_plugin.h"

#if XX_IS_WIN_D
// 顺序是必要的，否则可能编译失败
// 在 windows.h 系列之前不要 include asio 相关的头文件
#include <windows.h>
// ---
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
#endif

#include "screen_capture.h"
#include <atomic>
#include <thread>

namespace agentxx {
namespace expand {

#if XX_IS_WIN_D

class ScreenCapture::Impl {
public:

    Impl() :
        running_(false),
        frame_rate_(0) {
        dxgiAvailable_ = initDxgi();
        if (dxgiAvailable_) {
            XX_LOGI("ScreenCapture: DXGI Desktop Duplication initialized");
        } else {
            XX_LOGI("ScreenCapture: DXGI unavailable, falling back to GDI");
        }
    }

    ~Impl() {
        stopStreaming();
        cleanupGdiCache();
        cleanupDxgi();
    }

    std::vector<ScreenFrame> captureAllScreens() {
        std::vector<ScreenFrame> frames;
        int                      screenCount = getScreenCount();

        for (int i = 0; i < screenCount; ++i) {
            auto frame = captureScreen(i);
            if (frame.width > 0 && frame.height > 0) {
                frames.push_back(std::move(frame));
            }
        }

        return frames;
    }

    ScreenFrame captureMouseScreen() {
        POINT cursorPos;
        if (!GetCursorPos(&cursorPos)) {
            XX_LOGE("ScreenCapture: GetCursorPos failed");
            return {};
        }

        HMONITOR hMonitor = MonitorFromPoint(cursorPos, MONITOR_DEFAULTTONULL);
        if (!hMonitor) {
            XX_LOGE("ScreenCapture: MonitorFromPoint failed");
            return {};
        }

        MONITORINFOEXW monitorInfo = {};
        monitorInfo.cbSize         = sizeof(monitorInfo);
        if (!GetMonitorInfoW(hMonitor, &monitorInfo)) {
            XX_LOGE("ScreenCapture: GetMonitorInfoW failed");
            return {};
        }

        int screenIndex = getMonitorIndex(hMonitor);
        if (screenIndex < 0) {
            return {};
        }

        return doCaptureScreen(
            monitorInfo.rcMonitor.left,
            monitorInfo.rcMonitor.top,
            monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
            monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
            screenIndex,
            hMonitor
        );
    }

    ScreenFrame captureScreen(int screenIndex) {
        int screenCount = getScreenCount();
        if (screenIndex < 0 || screenIndex >= screenCount) {
            XX_LOGE("ScreenCapture: invalid screen index {}", screenIndex);
            return {};
        }

        HMONITOR hMonitor = getMonitorByIndex(screenIndex);
        if (!hMonitor) {
            XX_LOGE("ScreenCapture: monitor not found for index {}", screenIndex);
            return {};
        }

        MONITORINFOEXW monitorInfo = {};
        monitorInfo.cbSize         = sizeof(monitorInfo);
        if (!GetMonitorInfoW(hMonitor, &monitorInfo)) {
            XX_LOGE("ScreenCapture: GetMonitorInfoW failed for index {}", screenIndex);
            return {};
        }

        return doCaptureScreen(
            monitorInfo.rcMonitor.left,
            monitorInfo.rcMonitor.top,
            monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
            monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
            screenIndex,
            hMonitor
        );
    }

    int getScreenCount() const {
        int count = 0;
        EnumDisplayMonitors(
            nullptr,
            nullptr,
            monitorCountEnumProc,
            reinterpret_cast<LPARAM>(&count)
        );
        return count;
    }

    bool startStreaming(int frameRate, ScreenFrameListener listener) {
        if (running_.load()) {
            XX_LOGW("ScreenCapture: streaming already running");
            return false;
        }

        if (frameRate <= 0) {
            XX_LOGE("ScreenCapture: invalid frame rate {}", frameRate);
            return false;
        }

        if (!listener) {
            XX_LOGE("ScreenCapture: listener is null");
            return false;
        }

        frame_rate_ = frameRate;
        listener_   = std::move(listener);
        running_.store(true);

        stream_thread_ = std::thread(&Impl::streamLoop, this);
        XX_LOGI("ScreenCapture: streaming started at {} fps (DXGI: {})", frameRate, dxgiAvailable_);
        return true;
    }

    void stopStreaming() {
        if (!running_.load()) {
            return;
        }

        running_.store(false);
        if (stream_thread_.joinable()) {
            stream_thread_.join();
        }
        listener_ = nullptr;
        XX_LOGI("ScreenCapture: streaming stopped");
    }

    void shutdown() {
        stopStreaming();
        cleanupGdiCache();
        cleanupDxgi();
    }

    bool isStreaming() const {
        return running_.load();
    }

    /// WIC 编码 BGRA 像素为 PNG 文件
    /// - WIC 在调用线程内自管理 COM 初始化 (CoInitializeEx/CoUninitialize),
    ///   线程池线程安全调用 (宿主 execute 回调即运行在线程池)
    /// - pixelData 为 B8G8R8A8 (DXGI/GDI 原生内存顺序), 与 WIC 32bppBGRA
    ///   格式完全一致, 无需像素转换
    bool saveFramePng(const ScreenFrame& frame, const std::string& path) const {
        if (frame.width <= 0 || frame.height <= 0 || frame.pixelData.empty()) {
            XX_LOGW("ScreenCapture: saveFramePng skipped (empty frame)");
            return false;
        }
        const UINT stride = static_cast<UINT>(frame.width) * 4;
        if (frame.pixelData.size() < static_cast<size_t>(stride) * frame.height) {
            XX_LOGW("ScreenCapture: saveFramePng skipped (pixel data too small)");
            return false;
        }

        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (hr == RPC_E_CHANGED_MODE) {
            // 线程已初始化 MTA: WIC 在 MTA 下同样可用
            hr = S_OK;
        }
        if (FAILED(hr)) {
            XX_LOGW("ScreenCapture: CoInitializeEx failed (0x{:08X})", static_cast<unsigned>(hr));
            return false;
        }

        bool ok = false;
        {
            ComPtr<IWICImagingFactory>    factory;
            ComPtr<IWICBitmapEncoder>     encoder;
            ComPtr<IWICBitmapFrameEncode> frameEncode;
            ComPtr<IWICStream>            stream;
            IPropertyBag2*                propBag = nullptr;

            do {
                hr = CoCreateInstance(
                    CLSID_WICImagingFactory,
                    nullptr,
                    CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(&factory)
                );
                if (FAILED(hr)) {
                    break;
                }
                hr = factory->CreateStream(&stream);
                if (FAILED(hr)) {
                    break;
                }
                // UTF-8 路径 → UTF-16 (WIC 要求宽字符路径)
                std::wstring wpath;
                {
                    int len = MultiByteToWideChar(
                        CP_UTF8,
                        0,
                        path.data(),
                        static_cast<int>(path.size()),
                        nullptr,
                        0
                    );
                    if (len > 0) {
                        wpath.resize(static_cast<size_t>(len));
                        MultiByteToWideChar(
                            CP_UTF8,
                            0,
                            path.data(),
                            static_cast<int>(path.size()),
                            wpath.data(),
                            len
                        );
                    }
                }
                hr = stream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE);
                if (FAILED(hr)) {
                    XX_LOGW(
                        "ScreenCapture: WIC stream open failed for {} (0x{:08X})",
                        path,
                        static_cast<unsigned>(hr)
                    );
                    break;
                }
                hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
                if (FAILED(hr)) {
                    break;
                }
                hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
                if (FAILED(hr)) {
                    break;
                }
                hr = encoder->CreateNewFrame(&frameEncode, &propBag);
                if (FAILED(hr)) {
                    break;
                }
                hr = frameEncode->Initialize(propBag);
                if (FAILED(hr)) {
                    break;
                }
                hr = frameEncode->SetSize(
                    static_cast<UINT>(frame.width),
                    static_cast<UINT>(frame.height)
                );
                if (FAILED(hr)) {
                    break;
                }
                WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
                hr                        = frameEncode->SetPixelFormat(&format);
                if (FAILED(hr)) {
                    break;
                }
                hr = frameEncode->WritePixels(
                    static_cast<UINT>(frame.height),
                    stride,
                    static_cast<UINT>(frame.pixelData.size()),
                    const_cast<BYTE*>(frame.pixelData.data())
                );
                if (FAILED(hr)) {
                    break;
                }
                hr = frameEncode->Commit();
                if (FAILED(hr)) {
                    break;
                }
                hr = encoder->Commit();
                ok = SUCCEEDED(hr);
            } while (false);

            if (propBag) {
                propBag->Release();
            }
        }
        CoUninitialize();

        if (!ok) {
            XX_LOGW("ScreenCapture: PNG encode failed (0x{:08X})", static_cast<unsigned>(hr));
        } else {
            XX_LOGI("ScreenCapture: saved PNG {} ({}x{})", path, frame.width, frame.height);
        }
        return ok;
    }

private:

    ScreenFrame
        doCaptureScreen(int x, int y, int width, int height, int screenIndex, HMONITOR hMonitor) {
        if (dxgiAvailable_ && screenIndex >= 0
            && screenIndex < static_cast<int>(dxgiMonitors_.size())
            && dxgiMonitors_[screenIndex].duplication) {
            auto frame = captureScreenDxgi(screenIndex, hMonitor, x, y, width, height);
            // DXGI AcquireNextFrame 超时（桌面无变化）时会返回带宽高但无像素数据的帧，
            // 此时也应回退 GDI，确保始终能拿到完整像素
            if (frame.width > 0 && frame.height > 0 && !frame.pixelData.empty()) {
                return frame;
            }
            XX_LOGW("ScreenCapture: DXGI error for screen {}, falling back to GDI", screenIndex);
        }

        return captureScreenGdi(x, y, width, height, screenIndex, hMonitor);
    }

    bool initDxgi() {
        ComPtr<ID3D11Device>        device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL           featureLevel;

        UINT createFlags = 0;
#ifdef _DEBUG
        createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            createFlags,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            device.GetAddressOf(),
            &featureLevel,
            context.GetAddressOf()
        );

        if (FAILED(hr)) {
            hr = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                createFlags,
                nullptr,
                0,
                D3D11_SDK_VERSION,
                device.GetAddressOf(),
                &featureLevel,
                context.GetAddressOf()
            );
        }

        if (FAILED(hr)) {
            return false;
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        hr = device.As(&dxgiDevice);
        if (FAILED(hr)) {
            return false;
        }

        ComPtr<IDXGIAdapter> dxgiAdapter;
        hr = dxgiDevice->GetAdapter(dxgiAdapter.GetAddressOf());
        if (FAILED(hr)) {
            return false;
        }

        int monitorCount = getScreenCount();
        dxgiMonitors_.resize(monitorCount);

        ComPtr<IDXGIOutput> dxgiOutput;
        for (UINT i = 0;; ++i) {
            hr = dxgiAdapter->EnumOutputs(i, dxgiOutput.GetAddressOf());
            if (FAILED(hr)) {
                break;
            }

            ComPtr<IDXGIOutput1> dxgiOutput1;
            hr = dxgiOutput.As(&dxgiOutput1);
            if (FAILED(hr)) {
                dxgiOutput.Reset();
                continue;
            }

            DXGI_OUTPUT_DESC outputDesc;
            hr = dxgiOutput->GetDesc(&outputDesc);
            if (FAILED(hr)) {
                dxgiOutput.Reset();
                continue;
            }

            HMONITOR outputMonitor = outputDesc.Monitor;
            int      screenIndex   = getMonitorIndex(outputMonitor);
            if (screenIndex < 0 || screenIndex >= monitorCount) {
                dxgiOutput.Reset();
                continue;
            }

            ComPtr<IDXGIOutputDuplication> duplication;
            hr = dxgiOutput1->DuplicateOutput(device.Get(), duplication.GetAddressOf());
            if (FAILED(hr)) {
                XX_LOGW(
                    "ScreenCapture: DuplicateOutput failed for screen {} (hr=0x{:08X})",
                    screenIndex,
                    static_cast<unsigned>(hr)
                );
                dxgiMonitors_[screenIndex].duplicationFailed = true;
                dxgiOutput.Reset();
                continue;
            }

            dxgiMonitors_[screenIndex].duplication       = std::move(duplication);
            dxgiMonitors_[screenIndex].duplicationFailed = false;

            dxgiOutput.Reset();
        }

        d3dDevice_  = std::move(device);
        d3dContext_ = std::move(context);

        bool anyAvailable = false;
        for (const auto& m : dxgiMonitors_) {
            if (m.duplication) {
                anyAvailable = true;
                break;
            }
        }

        if (!anyAvailable) {
            cleanupDxgi();
            return false;
        }

        return true;
    }

    void cleanupDxgi() {
        for (auto& m : dxgiMonitors_) {
            m.duplication.Reset();
            m.stagingTexture.Reset();
        }
        dxgiMonitors_.clear();
        d3dContext_.Reset();
        d3dDevice_.Reset();
    }

    ScreenFrame
        captureScreenDxgi(int screenIndex, HMONITOR hMonitor, int x, int y, int width, int height) {
        ScreenFrame frame;
        frame.width       = width;
        frame.height      = height;
        frame.offsetX     = x;
        frame.offsetY     = y;
        frame.screenIndex = screenIndex;
        frame.timestamp   = std::chrono::steady_clock::now();

        MONITORINFOEXW monitorInfo = {};
        monitorInfo.cbSize         = sizeof(monitorInfo);
        if (GetMonitorInfoW(hMonitor, &monitorInfo)) {
            frame.screenName = wcharToUtf8(monitorInfo.szDevice);
            frame.isPrimary  = (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) != 0;
        }

        auto& dxgiState = dxgiMonitors_[screenIndex];
        if (!dxgiState.duplication) {
            frame.width = 0;
            return frame;
        }

        ComPtr<IDXGIResource>   desktopResource;
        DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
        HRESULT                 hr        = dxgiState.duplication
                         ->AcquireNextFrame(100, &frameInfo, desktopResource.GetAddressOf());

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            return frame;
        }

        if (FAILED(hr)) {
            if (hr == DXGI_ERROR_ACCESS_LOST) {
                XX_LOGW(
                    "ScreenCapture: DXGI access lost for screen {}, reinitializing",
                    screenIndex
                );
                dxgiState.duplication.Reset();
                dxgiState.stagingTexture.Reset();
            }
            frame.width = 0;
            return frame;
        }

        ComPtr<ID3D11Texture2D> desktopTexture;
        hr = desktopResource.As(&desktopTexture);
        if (FAILED(hr)) {
            dxgiState.duplication->ReleaseFrame();
            frame.width = 0;
            return frame;
        }

        D3D11_TEXTURE2D_DESC texDesc;
        desktopTexture->GetDesc(&texDesc);

        if (!dxgiState.stagingTexture || dxgiState.lastWidth != static_cast<int>(texDesc.Width)
            || dxgiState.lastHeight != static_cast<int>(texDesc.Height)) {
            D3D11_TEXTURE2D_DESC stagingDesc = {};
            stagingDesc.Width                = texDesc.Width;
            stagingDesc.Height               = texDesc.Height;
            stagingDesc.MipLevels            = 1;
            stagingDesc.ArraySize            = 1;
            stagingDesc.Format               = texDesc.Format;
            stagingDesc.SampleDesc.Count     = 1;
            stagingDesc.Usage                = D3D11_USAGE_STAGING;
            stagingDesc.CPUAccessFlags       = D3D11_CPU_ACCESS_READ;

            dxgiState.stagingTexture.Reset();
            hr = d3dDevice_->CreateTexture2D(
                &stagingDesc,
                nullptr,
                dxgiState.stagingTexture.GetAddressOf()
            );
            if (FAILED(hr)) {
                dxgiState.duplication->ReleaseFrame();
                frame.width = 0;
                return frame;
            }

            dxgiState.lastWidth  = texDesc.Width;
            dxgiState.lastHeight = texDesc.Height;
        }

        d3dContext_->CopyResource(dxgiState.stagingTexture.Get(), desktopTexture.Get());

        dxgiState.duplication->ReleaseFrame();

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        hr = d3dContext_->Map(dxgiState.stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            frame.width = 0;
            return frame;
        }

        int pixelDataSize = width * height * 4;
        frame.pixelData.resize(pixelDataSize);

        const uint8_t* src          = static_cast<const uint8_t*>(mapped.pData);
        int            srcRowPitch  = static_cast<int>(mapped.RowPitch);
        int            destRowPitch = width * 4;

        for (int row = 0; row < height; ++row) {
            memcpy(
                frame.pixelData.data() + row * destRowPitch,
                src + row * srcRowPitch,
                destRowPitch
            );
        }

        d3dContext_->Unmap(dxgiState.stagingTexture.Get(), 0);

        return frame;
    }

    void cleanupGdiCache() {
        if (gdiCache_.oldObj && gdiCache_.memDC) {
            SelectObject(gdiCache_.memDC, gdiCache_.oldObj);
            gdiCache_.oldObj = nullptr;
        }
        if (gdiCache_.bitmap) {
            DeleteObject(gdiCache_.bitmap);
            gdiCache_.bitmap = nullptr;
        }
        if (gdiCache_.memDC) {
            DeleteDC(gdiCache_.memDC);
            gdiCache_.memDC = nullptr;
        }
        gdiCache_.width  = 0;
        gdiCache_.height = 0;
    }

    ScreenFrame
        captureScreenGdi(int x, int y, int width, int height, int screenIndex, HMONITOR hMonitor) {
        ScreenFrame frame;
        frame.width       = width;
        frame.height      = height;
        frame.offsetX     = x;
        frame.offsetY     = y;
        frame.screenIndex = screenIndex;
        frame.timestamp   = std::chrono::steady_clock::now();

        MONITORINFOEXW monitorInfo = {};
        monitorInfo.cbSize         = sizeof(monitorInfo);
        if (GetMonitorInfoW(hMonitor, &monitorInfo)) {
            frame.screenName = wcharToUtf8(monitorInfo.szDevice);
            frame.isPrimary  = (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) != 0;
        }

        HDC hdcScreen = GetDC(nullptr);
        if (!hdcScreen) {
            XX_LOGE("ScreenCapture: GetDC failed");
            return frame;
        }

        if (gdiCache_.width != width || gdiCache_.height != height) {
            cleanupGdiCache();

            gdiCache_.memDC = CreateCompatibleDC(hdcScreen);
            if (!gdiCache_.memDC) {
                XX_LOGE("ScreenCapture: CreateCompatibleDC failed");
                ReleaseDC(nullptr, hdcScreen);
                return frame;
            }

            gdiCache_.bitmap = CreateCompatibleBitmap(hdcScreen, width, height);
            if (!gdiCache_.bitmap) {
                XX_LOGE("ScreenCapture: CreateCompatibleBitmap failed");
                DeleteDC(gdiCache_.memDC);
                gdiCache_.memDC = nullptr;
                ReleaseDC(nullptr, hdcScreen);
                return frame;
            }

            gdiCache_.oldObj = SelectObject(gdiCache_.memDC, gdiCache_.bitmap);
            gdiCache_.width  = width;
            gdiCache_.height = height;
        }

        if (!BitBlt(gdiCache_.memDC, 0, 0, width, height, hdcScreen, x, y, SRCCOPY | CAPTUREBLT)) {
            XX_LOGE("ScreenCapture: BitBlt failed");
            ReleaseDC(nullptr, hdcScreen);
            return frame;
        }

        BITMAPINFO bmi              = {};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = width;
        bmi.bmiHeader.biHeight      = -height;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        int pixelDataSize = width * height * 4;
        frame.pixelData.resize(pixelDataSize);

        if (!GetDIBits(
                gdiCache_.memDC,
                gdiCache_.bitmap,
                0,
                height,
                frame.pixelData.data(),
                &bmi,
                DIB_RGB_COLORS
            )) {
            XX_LOGE("ScreenCapture: GetDIBits failed");
            frame.pixelData.clear();
        }

        ReleaseDC(nullptr, hdcScreen);

        return frame;
    }

    void streamLoop() {
        auto frameInterval = std::chrono::milliseconds(1000 / frame_rate_);

        while (running_.load()) {
            auto start = std::chrono::steady_clock::now();

            auto frames = captureAllScreens();
            if (listener_) {
                listener_(frames);
            }

            auto elapsed   = std::chrono::steady_clock::now() - start;
            auto sleepTime = frameInterval - elapsed;

            if (sleepTime > std::chrono::milliseconds(0)) {
                std::this_thread::sleep_for(sleepTime);
            }
        }
    }

    static BOOL CALLBACK monitorCountEnumProc(
        HMONITOR /*hMonitor*/,
        HDC /*hdcMonitor*/,
        LPRECT /*lprcMonitor*/,
        LPARAM dwData
    ) {
        int* count = reinterpret_cast<int*>(dwData);
        (*count)++;
        return TRUE;
    }

    struct MonitorEnumData {
        int      targetIndex;
        int      currentIndex;
        HMONITOR result;
    };

    static BOOL CALLBACK monitorEnumProc(
        HMONITOR hMonitor,
        HDC /*hdcMonitor*/,
        LPRECT /*lprcMonitor*/,
        LPARAM dwData
    ) {
        MonitorEnumData* data = reinterpret_cast<MonitorEnumData*>(dwData);
        if (data->currentIndex == data->targetIndex) {
            data->result = hMonitor;
            return FALSE;
        }
        data->currentIndex++;
        return TRUE;
    }

    HMONITOR getMonitorByIndex(int index) const {
        MonitorEnumData data = {index, 0, nullptr};
        EnumDisplayMonitors(nullptr, nullptr, monitorEnumProc, reinterpret_cast<LPARAM>(&data));
        return data.result;
    }

    struct MonitorIndexData {
        HMONITOR target;
        int      currentIndex;
        int      result;
    };

    static BOOL CALLBACK monitorIndexEnumProc(
        HMONITOR hMonitor,
        HDC /*hdcMonitor*/,
        LPRECT /*lprcMonitor*/,
        LPARAM dwData
    ) {
        MonitorIndexData* data = reinterpret_cast<MonitorIndexData*>(dwData);
        if (hMonitor == data->target) {
            data->result = data->currentIndex;
            return FALSE;
        }
        data->currentIndex++;
        return TRUE;
    }

    int getMonitorIndex(HMONITOR hMonitor) const {
        MonitorIndexData data = {hMonitor, 0, -1};
        EnumDisplayMonitors(
            nullptr,
            nullptr,
            monitorIndexEnumProc,
            reinterpret_cast<LPARAM>(&data)
        );
        return data.result;
    }

    static std::string wcharToUtf8(const WCHAR* wstr) {
        if (!wstr) {
            return {};
        }
        int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 0) {
            return {};
        }
        std::string result(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result.data(), len, nullptr, nullptr);
        return result;
    }

    std::atomic<bool>   running_;
    std::thread         stream_thread_;
    int                 frame_rate_;
    ScreenFrameListener listener_;

    bool                        dxgiAvailable_ = false;
    ComPtr<ID3D11Device>        d3dDevice_;
    ComPtr<ID3D11DeviceContext> d3dContext_;

    struct DxgiMonitorState {
        ComPtr<IDXGIOutputDuplication> duplication;
        ComPtr<ID3D11Texture2D>        stagingTexture;
        int                            lastWidth         = 0;
        int                            lastHeight        = 0;
        bool                           duplicationFailed = false;
    };

    std::vector<DxgiMonitorState> dxgiMonitors_;

    struct GdiCache {
        HDC     memDC  = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ oldObj = nullptr;
        int     width  = 0;
        int     height = 0;
    };

    GdiCache gdiCache_;
};

#else

class ScreenCapture::Impl {
public:

    Impl()  = default;
    ~Impl() = default;

    std::vector<ScreenFrame> captureAllScreens() {
        XX_LOGW("ScreenCapture: not supported on this platform");
        return {};
    }

    ScreenFrame captureMouseScreen() {
        XX_LOGW("ScreenCapture: not supported on this platform");
        return {};
    }

    ScreenFrame captureScreen(int /*screenIndex*/) {
        XX_LOGW("ScreenCapture: not supported on this platform");
        return {};
    }

    int getScreenCount() const {
        return 0;
    }

    bool startStreaming(int /*frameRate*/, ScreenFrameListener /*listener*/) {
        XX_LOGW("ScreenCapture: not supported on this platform");
        return false;
    }

    void stopStreaming() {}

    void shutdown() {}

    bool isStreaming() const {
        return false;
    }

    bool saveFramePng(const ScreenFrame& /*frame*/, const std::string& /*path*/) const {
        XX_LOGW("ScreenCapture: saveFramePng not supported on this platform");
        return false;
    }
};

#endif

ScreenCapture::ScreenCapture() :
    impl_(std::make_unique<Impl>()) {}

ScreenCapture::~ScreenCapture() = default;

std::vector<ScreenFrame> ScreenCapture::captureAllScreens() {
    return impl_->captureAllScreens();
}

ScreenFrame ScreenCapture::captureMouseScreen() {
    return impl_->captureMouseScreen();
}

ScreenFrame ScreenCapture::captureScreen(int screenIndex) {
    return impl_->captureScreen(screenIndex);
}

int ScreenCapture::getScreenCount() const {
    return impl_->getScreenCount();
}

bool ScreenCapture::startStreaming(int frameRate, ScreenFrameListener listener) {
    return impl_->startStreaming(frameRate, std::move(listener));
}

void ScreenCapture::stopStreaming() {
    impl_->stopStreaming();
}

void ScreenCapture::shutdown() {
    impl_->shutdown();
}

bool ScreenCapture::isStreaming() const {
    return impl_->isStreaming();
}

bool ScreenCapture::saveFramePng(const ScreenFrame& frame, const std::string& path) const {
    return impl_->saveFramePng(frame, path);
}

} // namespace expand
} // namespace agentxx