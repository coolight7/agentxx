#pragma once

#include "agentxx-client/config_loader.h"
#include "agentxx-client/io/tui/framework/modal_container.h"
#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/scrollable.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/agent_io.h"
#include "agentxx/plugin/client_plugin_manager.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "asio/awaitable.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "fmt/format.h"
#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "neograph/api.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// TUI 共享工具函数
// (时长/时间戳格式化已迁移到 agentxx/util/string_util.h, 供 agent 端构造
//  系统提示文本复用, 此处仅保留 UI 专用函数)
// ---------------------------------------------------------------------------

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
    void              clear();

    uint64_t poppedCount() const {
        return poppedCount_;
    }

    size_t lineCount() const {
        return lines_.size();
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
class TUIClientAgentIO : public agentxx::agent::AgentIOBase,
                         public std::enable_shared_from_this<TUIClientAgentIO> {
public:

    using LineChannel
        = asio::experimental::concurrent_channel<void(neograph_asio_error_code, std::string)>;

    /// 兼容旧代码的类型别名
    using Message      = TUIMessage;
    using PendingInput = TUIPendingInput;
    using RenderState  = TUIRenderState;

    /// 复制鼠标选中的文本到系统剪贴板 (鼠标左键拖选后松开时调用, UI 线程):
    /// - 从 FTXUI Screen 的当前 selection 提取文本 (GetSelection, 取上一绘制帧
    ///   累积的选中文本, 与屏幕显示一致, 已含本次拖动终点)
    /// - 写入系统剪贴板: Windows 用 Win32 API, 其他平台用 OSC 52 转义序列
    ///   (依赖终端模拟器支持, 如 Windows Terminal/wezterm/kitty/xterm)
    /// - 返回 true 表示已复制; 无选中文本或剪贴板不可用时返回 false
    /// - 复制成功/失败时均以 toast 提示 (无选中文本不提示)
    bool copySelectionToClipboard();

    explicit TUIClientAgentIO(
        asio::any_io_executor ex,
        std::string           threadId = "session",
        TUITheme              theme    = TUITheme::darkTheme(),
        /// 权限询问处理模式 (来自 yaml 配置 `permission.mode`, 见 config.h)
        agentxx::agent::PermissionMode permissionMode = agentxx::agent::PermissionMode::Ask
    );
    ~TUIClientAgentIO() override;

    void start();
    void stop();

    bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    void setRemoteUrl(std::string url) {
        remoteUrl_ = std::move(url);
    }

    /// 装配 client 插件管理器 (mode_runners 在 start() 后调用):
    /// - 供命令管线 (onSend 拦截 "/" 命令) 与组件渲染 (状态栏/侧边栏) 读取
    /// - 同时也是事件接收器 (ClientEventSink), 经 setEventSink 注入
    void setPluginManager(std::shared_ptr<agentxx::plugin::ClientPluginManager> mgr) {
        pluginManager_ = std::move(mgr);
        if (pluginManager_) {
            pluginManager_->setThreadId(currentThreadId());
        }
    }

    std::shared_ptr<agentxx::plugin::ClientPluginManager> pluginManager() const {
        return pluginManager_;
    }

    // -----------------------------------------------------------------------
    // 插件适配器接口 (由 TuiPluginAdapter 在 client io 线程调用; 内部自行
    // 投递到 UI 线程 / 加锁, 线程安全)
    // -----------------------------------------------------------------------

    /// 投递 UI 线程独占操作 (等价 enqueueUiAction; 任意线程可调用)
    void postToUi(std::function<void()> fn) {
        enqueueUiAction(std::move(fn));
    }

    /// 请求 UI 重绘 (任意线程可调用; 合并同帧多次请求)
    void requestRedraw() {
        postRedraw();
    }

    /// 侧边栏组件访问器 (UI 线程使用; 未启动时返回 nullptr)
    std::shared_ptr<SidebarComponent> sidebar() const {
        return sidebar_;
    }

    /// 插件面板挂载: 添加侧边栏 tab (UI 线程调用; 内容渲染经 renderPluginPanel)
    /// - 已存在同名 tab 时跳过 (幂等)
    void addPluginPanelTab(const std::string& id, const std::string& title);

    /// 插件面板摘除: 移除侧边栏 tab (UI 线程调用; 不存在时忽略)
    void removePluginPanelTab(const std::string& id);

    /// 渲染插件侧边栏面板内容 (UI 线程; 从 pluginManager UI 注册表快照读取)
    std::vector<ScrollItem> renderPluginPanel(const std::string& panelId);

    /// 显示 toast (任意线程可调用; 内部投递到 UI 线程)
    void uiToast(std::string text, int level);

    /// 代发用户消息 (client io 线程; 与用户输入同排队语义: 未连接/流式中
    /// 进 pendingInputs, 连接后按轮次分发; 发送后通知事件接收器)
    void sendPluginUserInput(std::string text);

    /// 跨端插件数据上行 (client io 线程): WirePluginDataUp → agent 侧插件
    /// 返回 true 表示已投递 (未连接等失败返回 false)
    bool sendPluginDataUp(
        const std::string& plugin,
        const std::string& event,
        const std::string& json
    );

    /// 设置连接状态 (跨线程安全: 更新 sharedState 并触发重绘)
    /// 状态枚举 ConnState 定义于 tui_state.h (TUIRenderState::connState)
    void setConnState(ConnState state);

    /// AgentIOBase::onServerReady 覆写: 置 Connected 并刷新待发送队列
    /// (连接建立后由 mode_runners 调用)
    void onServerReady() override;

    /// AgentIOBase::onServerProgress 覆写: 更新 banner 当前启动步骤
    /// (agent 线程同步调用, 经 sharedState 锁 + postRedraw 安全更新)
    void onServerProgress(std::string_view step) override;

    /// 连接建立后刷新待发送队列: 发送连接前排队输入的首条
    /// (置 isStreaming 并经 transport 发送, 后续排队输入由 TurnEnd 分发)
    void flushPendingInput();

    /// 等待用户点击"重试" (连接失败后由连接协程 await; TUI 退出时尽快返回,
    /// 避免失败后用户退出导致协程永久挂起阻塞 io_context)
    asio::awaitable<void> waitRetry();

    /// 用户点击 banner 上的"重试"按钮 (UI 线程调用): 置 Connecting 并唤醒 waitRetry
    void requestRetry();

    asio::awaitable<std::optional<std::string>> getInput() override;
    asio::awaitable<neograph::json>             handleInterrupt(
                    std::string_view threadId,
                    std::string_view interruptNode,
                    std::string_view interruptValue,
                    std::string_view interruptArgJson
                ) override;
    void requestCancel(std::string threadId) override;
    void requestSelectModel(std::string threadId, std::string model) override;

    /// 供组件访问共享状态 (UI 线程渲染/事件时使用)
    TUISharedState& sharedState() {
        return sharedState_;
    }

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

    /// 将 UI 线程独占的组件操作 (弹窗开关/消息列表状态等) 投递到 UI 线程执行。
    /// client 线程 (onDelta/onSync/onPeerMessage) 不得直接触碰组件树
    /// (modal_/messageList_ 等由 UI 线程独占), 必须经本接口排队,
    /// 由帧循环开头消费, 消除跨线程数据竞争。
    void enqueueUiAction(std::function<void()> fn);

    void postRedraw();

    /// 打开模型选择器模态
    void openModelSelector();
    /// 打开设置模态
    void openSettings();
    /// 打开会话选择模态 (F4 / 状态栏 [F4] Sessions 按钮):
    /// - 仅当前会话非运行状态时可打开 (否则提示先停止当前会话)
    /// - 请求服务端会话列表并展示; 确认后经 WireSwitchSession 切换
    void openSessionSelector();
    /// 屏幕上方提示 (toast): 设置提示文本并安排 kToastDuration 后触发重绘,
    /// 由 UI 线程渲染时检查超时并清除 (toastText_/toastShownAt_ 为 UI 线程独占,
    /// 定时器回调仅触发重绘, 不直接写状态, 无跨线程竞争)
    void showToast(std::string text);

    /// 通知事件接收器: 用户输入已发送 (sendUserInputLocked 内部调用;
    /// 任意线程, 内部按需 post 到 client io 线程)
    void notifyUserInputSent(const std::string& threadId, const std::string& text);
    /// 会话选择弹窗确认后的切换逻辑 (UI 线程):
    /// - 更新本地 threadId 绑定与重连握手 threadId (WS 模式)
    /// - 发送 WireSwitchSession, 服务端回推全量 Sync/模型/上下文统计 (WireModelInfo
    ///   / WireContextStats) 恢复界面; TUI 不持有 Session (属于 agent-io 线程)
    void switchToSession(std::string newThreadId);

    /// 当前会话 thread_id 的跨线程安全读写:
    /// UI 线程切换会话时写入, client 线程发送用户输入时读取
    std::string currentThreadId() const {
        std::lock_guard<std::mutex> lock(threadIdMutex_);
        return threadId_;
    }

    void setCurrentThreadId(std::string newThreadId) {
        std::lock_guard<std::mutex> lock(threadIdMutex_);
        threadId_ = std::move(newThreadId);
    }

    /// client 插件管理器 (装配后不可变; uiRegistrySnapshot/hasCommand 线程安全)
    std::shared_ptr<agentxx::plugin::ClientPluginManager> pluginManager_;

    /// F12: 切换日志窗口 tab
    void toggleLogWindow();
    /// 打开 Plan 状态图模态 (Info 侧边栏 [View Plan Diagram] 按钮触发)
    void openPlanDiagram();

    /// 侧边栏渲染辅助
    std::vector<ScrollItem>       renderLogWindow();
    std::vector<ScrollItem>       renderInfoSidebar();
    ftxui::Element                renderInfoSidebarFooter();
    ftxui::Element                renderLogSidebarFooter();
    std::optional<ftxui::Element> renderPlanningInfo();

    // -----------------------------------------------------------------------
    // 状态
    // -----------------------------------------------------------------------
    TUISharedState sharedState_;

    TUITheme theme_;
    /// 当前会话 thread_id (切换会话时由 UI 线程写入, client 线程发送输入时读取;
    /// 经 threadIdMutex_ 保护, 见 currentThreadId()/setCurrentThreadId())
    std::string           threadId_;
    mutable std::mutex    threadIdMutex_;
    asio::any_io_executor ex_;
    /// 权限询问处理模式 (yaml 配置 `permission.mode` 注入, 不可运行时切换)
    agentxx::agent::PermissionMode permissionMode_ = agentxx::agent::PermissionMode::Ask;

    std::mutex                                screenMutex_;
    std::shared_ptr<ftxui::ScreenInteractive> screen_;
    std::thread                               uiThread_;
    std::atomic<bool>                         running_{false};
    std::atomic<bool>                         awaitingInterruptInput_{false};

    std::shared_ptr<LineChannel> inputChannel_;
    std::shared_ptr<TUILogSink>  logSink_;
    std::string                  remoteUrl_;

    /// UI 线程组件 (start() 中创建, UI 线程独占)
    TUICtx                                ctx_;
    std::shared_ptr<MessageListComponent> messageList_;
    std::shared_ptr<SidebarComponent>     sidebar_;
    std::shared_ptr<StatusBarComponent>   statusBar_;
    std::shared_ptr<InputComponent>       inputBar_;
    std::shared_ptr<ModalContainer>       modal_;

    /// 日志行缓存 (UI 线程, 供 renderLogWindow 使用)
    std::vector<ftxui::Element> logLineCache_;
    uint64_t                    logCachePoppedCount_ = 0;
    /// 上次快照的日志行数 (用于判断日志是否新增, 避免每帧全量 snapshot 拷贝)
    size_t logCacheLineCount_ = 0;

    /// renderPlanningInfo 的解析缓存: 仅当 plan 消息变化 (指针/文本长度/toolFinished)
    /// 时重新解析 JSON, 避免 Info 侧边栏每帧重复解析 planning 参数
    const TUIMessage* planCacheMsgPtr_   = nullptr;
    size_t            planCacheTextLen_  = 0;
    bool              planCacheFinished_ = false;
    bool              planCacheValid_    = false;
    neograph::json    planCacheArgs_     = neograph::json::array();

    /// 重绘请求合并 (postRedraw 由 client/UI 线程并发调用):
    /// - redrawPosted_: 在途 Custom 标记, 仅当无在途事件时才 Post, 同帧内多次请求合并为一次
    /// - redrawSeq_:    请求计数, UI 线程在帧结束时据此判断帧期间是否有请求被合并
    ///                  (被合并进本帧渲染, 而本帧快照取的是帧开头, 可能未反映其状态变更),
    ///                  若有则补 Post 一帧, 保证以最新快照重绘, 避免请求丢失
    std::atomic<uint64_t> redrawSeq_{0};
    std::atomic<bool>     redrawPosted_{false};

    // ---- 屏幕上方提示 (toast, UI 线程独占) ----
    /// 当前 toast 文本 (空 = 无提示); 渲染时检查超时并清除
    std::string toastText_;
    /// toast 显示起始时刻 (渲染时据此判断是否超过 kToastDuration)
    std::chrono::steady_clock::time_point toastShownAt_;
    /// toast 超时定时器 (client io_context 上): 超时后仅触发重绘,
    /// 由 UI 线程渲染时清除状态; stop() 时 cancel 避免挂起等待
    std::shared_ptr<asio::steady_timer> toastTimer_;
    /// toast 显示时长
    static constexpr std::chrono::seconds kToastDuration{3};

    // ---- 鼠标拖选跟踪 (UI 线程独占, 用于"松开即复制") ----
    /// 左键是否处于按下状态 (Left Pressed 置位, Released 复位)
    bool mouseDown_ = false;
    /// 按下后是否发生过拖动 (Left Moved 置位); Released 且该标志为真时
    /// 判定为一次拖选完成 -> 自动复制选中文本并 toast 提示。
    /// 单击 (无拖动) 不复制, 保持原有点击交互 (按钮/折叠/拖拽条等)
    bool mouseDragged_ = false;

    // ---- UI 线程动作队列 ----
    /// client 线程投递、UI 线程 (帧循环开头) 消费的组件操作队列;
    /// 与 sharedState_ 无关, 独立加锁 (消费方仅短暂持有, 不嵌套 sharedState 锁)
    std::mutex                         uiActionsMutex_;
    std::vector<std::function<void()>> uiActions_;

    // ---- 中断请求 (client 线程独占) ----
    /// 当前进行中中断的 wire id (onPeerMessage 收到 WireInterruptRequest 时设置,
    /// 与 handleInterrupt 同线程顺序执行, 无需同步)
    int64_t interruptWireId_ = 0;
    /// 进行中中断的结果回传通道: wireId → 通道 (中断输入消息共享引用)。
    /// handleInterrupt 插入/移除; WireInterruptExpired / stop() 关闭通道以终止等待
    std::map<int64_t, std::shared_ptr<InterruptResultChannel>> activeInterrupts_;

    // ---- 连接失败重试 (跨线程原子标志) ----
    /// 用户点击 banner"重试"按钮的标志 (UI 线程 requestRetry 置位;
    /// 连接协程 waitRetry 轮询消费并据此返回重试连接)
    std::atomic<bool> retryRequested_{false};

    /// 鼠标命中区域 (渲染时 reflect 填充, 全局事件处理时检测)
    ftxui::Box pendingCounterBox_;
    ftxui::Box contextButtonBox_;
    ftxui::Box planDiagramButtonBox_; // Info 侧边栏 Plan 状态图按钮

    static constexpr const char* kLogTabId            = "xx_logs";
    static constexpr const char* kInfoTabId           = "xx_info";
    static constexpr int         kInfoSidebarMinWidth = 120;
    static constexpr const char* kAgentxxVersion      = "0.1.0";
};
