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

/// CDP (Chrome DevTools Protocol) 访问所需的两个网络动作
///
/// 网络一律经主程序 `<应用名>.net` 表 (插件不自己建连接), 由插件在挂载时注入:
/// - `httpGet(url, timeoutMs)`: 取一个 HTTP 地址的响应体, 失败返回空串;
/// - `wsRoundTrip(url, message, timeoutMs)`: 连一条 WebSocket、发一条文本、
///   等第一条返回消息、关闭, 失败返回空串。
///
/// 两个函数都在**工作线程**上调用 (插件里是文本选择的处理线程), 可以阻塞。
struct CdpNetFns {
    std::function<std::string(std::string_view url, int timeoutMs)> httpGet;
    std::function<std::string(std::string_view url, std::string_view message, int timeoutMs)>
        wsRoundTrip;

    bool ready() const {
        return static_cast<bool>(httpGet) && static_cast<bool>(wsRoundTrip);
    }
};

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

    /// CDP 网络动作 (插件挂载时注入; 未注入时浏览器 DevTools 路径直接跳过)
    void setCdpNetFns(CdpNetFns fns);

    bool start();
    void stop();

    bool isRunning() const;

private:

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace agentxx_text_selection_monitor_plugin