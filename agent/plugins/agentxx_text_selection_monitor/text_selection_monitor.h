#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace agentxx_text_selection_monitor_plugin {

enum class TextSource {
    Unknown,
    TextPattern,
    TextChildPattern,
    ValuePattern,
    EmGetSel,
    AccessibleObject,
    WmGetText,
    DevTools,
    FlutterAccessibility,
    Clipboard,
};

struct TextSelectionEvent {
    std::string                           text;
    std::chrono::steady_clock::time_point timestamp;
    TextSource                            source = TextSource::Unknown;
};

using TextSelectionListener = std::function<void(const TextSelectionEvent& event)>;

/// 系统级 文本选择事件流
class TextSelectionMonitor {
public:

    TextSelectionMonitor();
    ~TextSelectionMonitor();

    TextSelectionMonitor(const TextSelectionMonitor&)            = delete;
    TextSelectionMonitor& operator=(const TextSelectionMonitor&) = delete;

    void addListener(TextSelectionListener listener);
    void removeAllListeners();

    void setDebounceMs(int ms);

    bool start();
    void stop();

    bool isRunning() const;

private:

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace agentxx_text_selection_monitor_plugin