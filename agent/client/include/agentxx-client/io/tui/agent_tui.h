#pragma once

#include "agentxx-client/io/tui/framework/modal_container.h"
#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/scrollable.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx/agent/agent_io.h"
#include "agentxx/agent/context.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "asio/awaitable.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include "fmt/format.h"
#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "neograph/api.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// TUI 共享工具函数
// ---------------------------------------------------------------------------

inline std::string formatDurationMilliseconds(int64_t milliseconds) {
    if (milliseconds < 0) {
        return "0.0s";
    }
    const int64_t totalSec = milliseconds / 1000;
    const int64_t hours    = totalSec / 3600;
    const int64_t minutes  = (totalSec % 3600) / 60;
    const int64_t seconds  = totalSec % 60;
    if (hours > 0) {
        return fmt::format("{}h{}m{}s", hours, minutes, seconds);
    }
    if (minutes > 0) {
        if (seconds > 0) {
            return fmt::format("{}m{}s", minutes, seconds);
        }
        return fmt::format("{}m0s", minutes);
    }
    const double sec = static_cast<double>(milliseconds) / 1000.0;
    return fmt::format("{:.1f}s", sec);
}

inline std::string formatTimestampMilliseconds(int64_t timestamp_ms) {
    if (timestamp_ms <= 0) {
        return "00:00:00";
    }
    std::chrono::zoned_time time{
        std::chrono::current_zone(),
        std::chrono::sys_time{std::chrono::seconds(timestamp_ms / 1000)}
    };
    return std::format("{:%H:%M:%S}", time);
}

inline std::string oneLinePreview(std::string_view s, size_t max = 60) {
    const auto  nl = s.find('\n');
    std::string line{(nl == std::string_view::npos) ? s : s.substr(0, nl)};
    const auto  idx = agentxx::util::findIndexByUtf8Length(line, max);
    if (idx > 0 && idx < line.size()) {
        line.resize(idx);
        line += "...";
    }
    return line;
}

/// TUI 日志接收器
class TUILogSink : public agentxx::util::LogSink {
public:

    struct Line {
        agentxx::util::LogLevel level;
        std::string             text;
    };

    std::vector<Line> snapshot() const;
    void clear();
    uint64_t poppedCount() const {
        return poppedCount_;
    }

protected:

    void onLog(const agentxx::util::LogEntry& entry) override;

private:

    std::deque<Line> lines_;
    size_t           maxLines_    = 2000;
    uint64_t         poppedCount_ = 0;
};

// 前向声明组件
class MessageListComponent;
class SidebarComponent;
class StatusBarComponent;
class InputComponent;

/// TUI 主类 (重构后为轻量编排器)
///
/// 职责:
/// - 线程管理 (UI 线程 + client 线程)
/// - 协议处理 (onDelta/onSync/onPeerMessage → 更新 TUISharedState)
/// - 构建组件树 + 启动 FTXUI Loop
///
/// 渲染/事件逻辑已分解到各组件:
/// - MessageListComponent: 消息列表渲染 + 折叠交互
/// - InputComponent: 输入栏 + 发送逻辑
/// - StatusBarComponent: 状态栏
/// - SidebarComponent: 侧边栏 tab + 拖拽
/// - Overlay 组件: 模型选择/设置/待发送/上下文弹窗
///
/// 线程模型 (不变):
/// - client 线程: onDelta/onSync/onPeerMessage → sharedState_.mutate()
/// - UI 线程: FTXUI Loop 渲染 + 事件; 每帧 readSnapshot() 后无锁渲染
class AgentTUI : public agentxx::agent::AgentIOBase,
                 public std::enable_shared_from_this<AgentTUI> {
public:

    using LineChannel
        = asio::experimental::concurrent_channel<void(neograph_asio_error_code, std::string)>;

    /// 兼容旧代码的类型别名
    using Message      = TUIMessage;
    using PendingInput = TUIPendingInput;
    using RenderState  = TUIRenderState;

    explicit AgentTUI(
        asio::any_io_executor                         ex,
        std::shared_ptr<agentxx::agent::AgentContext> agentContext,
        std::string                                   threadId = "session",
        TUITheme                                      theme    = TUITheme::darkTheme()
    );
    ~AgentTUI() override;

    void start();
    void stop();

    bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    void setRemoteUrl(std::string url) {
        remoteUrl_ = std::move(url);
    }

    asio::awaitable<std::optional<std::string>> getInput() override;
    asio::awaitable<neograph::json>             handleInterrupt(
                    std::string_view threadId,
                    std::string_view interruptNode,
                    std::string_view interruptValue,
                    std::string_view interruptArgJson
                ) override;
    void requestCancel(std::string_view threadId) override;
    void requestSelectModel(std::string_view threadId, std::string_view model) override;

    /// 供组件访问共享状态 (UI 线程渲染/事件时使用)
    TUISharedState& sharedState() { return sharedState_; }

protected:

    // ---- AgentIOBase 被动接收回调 (client 端点实现; 仅由 onPeerMessage 分发) ----
    void onDelta(const agentxx::agent::Delta& delta) override;
    void onSync(const agentxx::agent::SyncPayload& payload) override;
    void onTurnResult(const agentxx::agent::WireTurnResult& result) override;
    void onContextStats(const agentxx::agent::WireContextStats& stats) override;

    void onPeerMessage(agentxx::agent::WireMessage msg) override;

private:

    // -----------------------------------------------------------------------
    // 协议处理辅助 (client 线程, 须持有 sharedState_.mutex())
    // -----------------------------------------------------------------------
    void pushCurrentTokenLocked(TUIRenderState& st);
    void cancelCurrentRunLocked(TUIRenderState& st);
    void sendUserInputLocked(TUIRenderState& st, std::string text);
    void dispatchNextPendingInput(TUIRenderState& st);

    void postRedraw();

    /// 打开模型选择器模态
    void openModelSelector();
    /// 打开设置模态
    void openSettings();
    /// F12: 切换日志窗口 tab
    void toggleLogWindow();

    /// 侧边栏渲染辅助
    std::vector<ScrollItem> renderLogWindow();
    std::vector<ScrollItem> renderInfoSidebar();
    ftxui::Element renderInfoSidebarFooter();
    ftxui::Element renderLogSidebarFooter();
    std::optional<ftxui::Element> renderPlanningInfo();

    // -----------------------------------------------------------------------
    // 状态
    // -----------------------------------------------------------------------
    TUISharedState sharedState_;

    std::shared_ptr<agentxx::agent::AgentContext> agentContext_;
    TUITheme                                      theme_;
    std::string                                   threadId_;
    asio::any_io_executor                         ex_;

    std::mutex                                screenMutex_;
    std::shared_ptr<ftxui::ScreenInteractive> screen_;
    std::thread                               uiThread_;
    std::atomic<bool>                         running_{false};
    std::atomic<bool>                         awaitingInterruptInput_{false};

    std::shared_ptr<LineChannel> inputChannel_;
    std::shared_ptr<TUILogSink>  logSink_;
    std::string                  remoteUrl_;

    std::shared_ptr<agentxx::agent::Session> session_;

    /// UI 线程组件 (start() 中创建, UI 线程独占)
    TUICtx                                     ctx_;
    std::shared_ptr<MessageListComponent>      messageList_;
    std::shared_ptr<SidebarComponent>          sidebar_;
    std::shared_ptr<StatusBarComponent>        statusBar_;
    std::shared_ptr<InputComponent>            inputBar_;
    std::shared_ptr<ModalContainer>            modal_;

    /// 日志行缓存 (UI 线程, 供 renderLogWindow 使用)
    std::vector<ftxui::Element> logLineCache_;
    uint64_t                    logCachePoppedCount_ = 0;

    /// 鼠标命中区域 (渲染时 reflect 填充, 全局事件处理时检测)
    ftxui::Box pendingCounterBox_;
    ftxui::Box contextButtonBox_;

    static constexpr const char* kLogTabId           = "xx_logs";
    static constexpr const char* kInfoTabId          = "xx_info";
    static constexpr int         kInfoSidebarMinWidth = 120;
    static constexpr const char* kAgentxxVersion     = "0.1.0";
};
