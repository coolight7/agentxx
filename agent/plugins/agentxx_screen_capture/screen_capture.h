#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace agentxx {
namespace expand {

struct ScreenFrame {
    int                                   width       = 0;
    int                                   height      = 0;
    int                                   offsetX     = 0;
    int                                   offsetY     = 0;
    int                                   screenIndex = 0;
    std::string                           screenName;
    bool                                  isPrimary = false;
    std::vector<uint8_t>                  pixelData;
    std::chrono::steady_clock::time_point timestamp;
};

using ScreenFrameListener = std::function<void(const std::vector<ScreenFrame>& frames)>;

class ScreenCapture {
public:

    ScreenCapture();
    ~ScreenCapture();

    ScreenCapture(const ScreenCapture&)            = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    std::vector<ScreenFrame> captureAllScreens();

    ScreenFrame captureMouseScreen();

    ScreenFrame captureScreen(int screenIndex);

    int getScreenCount() const;

    bool startStreaming(int frameRate, ScreenFrameListener listener);

    void stopStreaming();

    /// 显式释放全部底层资源 (DXGI/D3D11 设备、GDI 缓存, 并停止流式线程)
    /// - 必须在插件 unload 回调中调用 (先于宿主 FreeLibrary): GPU 资源若留到
    ///   DLL 卸载时的静态析构中释放, 会在 Windows loader lock 下执行
    ///   D3D11 设备销毁 —— 显卡驱动 (如 AMD atidxx64) 的内部工作线程此时无法
    ///   获取 loader lock 退出, 主线程在 GetExitCodeThread 上无限自旋 (实测挂死)
    /// - 重复调用安全; 析构函数会再次兜底 (此时已无资源可释放)
    void shutdown();

    bool isStreaming() const;

    /// 把帧像素保存为 PNG 文件 (Windows: WIC 编码; 其他平台恒失败)
    /// - frame.pixelData 为 BGRA (B8G8R8A8, 32bpp, 行宽 = width*4, 无行填充)
    /// - path: 目标文件路径 (含 .png 后缀), 目录须已存在
    /// - 返回 true 成功; false 失败 (非 Windows / 像素为空 / 编码或写文件失败)
    bool saveFramePng(const ScreenFrame& frame, const std::string& path) const;

private:

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace expand
} // namespace agentxx