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

    bool isStreaming() const;

private:

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace expand
} // namespace agentxx